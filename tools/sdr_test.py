#!/usr/bin/env python3
"""SDR receive test against a running AIS display.

Reads the display's settings and status, records an I/Q capture on it, watches
the receiver while it records, downloads the recording and analyses it:

    python3 sdr_test.py 192.168.1.50                    # 60 s capture
    python3 sdr_test.py 192.168.1.50 --seconds 300
    python3 sdr_test.py --analyze captures/20261026-141500   # redo the analysis

The password is the access point's (on the display's Network screen); give it
with --password, $AIS_DISPLAY_PASSWORD, or at the prompt.

Everything goes in captures/<date-time>/ at the top of the repo:

    config.json, status_before.json, status_after.json, memory_*.json, log_*.txt
    status_timeline.jsonl   /api/status about once a second while recording
    capture.cu8             raw I/Q (288 kS/s, 162.000 MHz centre)
    capture.nmea            "<seconds> <N|S> <sentence>" heard while recording
    iq_stats.json           signal levels, spectrum, bursts per channel
    offline_decode.txt      the I/Q decoded again here (ais_demod_file)
    score.txt               SDR vs network feed (ais_capture_score -v)
    summary.json            every number below, for digging
    report.md               the readable version

The analysis tools (firmware/test/host) are built with `cc`. Only the Python
standard library is needed.
"""

import argparse
import base64
import datetime
import getpass
import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST_TESTS = os.path.join(REPO, "firmware", "test", "host")
RATE = 288000
MAX_SECONDS = 600

# ---------------------------------------------------------------------------
# The display's web API
# ---------------------------------------------------------------------------


class Display:
    def __init__(self, host, password):
        self.base = host if host.startswith("http") else "http://" + host
        self.auth = "Basic " + base64.b64encode(b"admin:" + password.encode()).decode()

    def request(self, path, body=None, timeout=15):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method="POST" if data else "GET")
        req.add_header("Authorization", self.auth)
        if data:
            req.add_header("Content-Type", "application/json")
        try:
            return urllib.request.urlopen(req, timeout=timeout)
        except urllib.error.HTTPError as e:
            msg = e.read().decode(errors="replace").strip()
            try:
                msg = json.loads(msg).get("error", msg)
            except ValueError:
                pass
            if e.code == 401:
                msg = "wrong password (use the access point's, from the display's Network screen)"
            raise SystemExit(f"{path}: HTTP {e.code}: {msg}")
        except (urllib.error.URLError, OSError) as e:
            raise SystemExit(f"{path}: can't reach the display at {self.base}: {e}")

    def json(self, path, body=None):
        with self.request(path, body) as r:
            return json.load(r)

    def text(self, path):
        with self.request(path) as r:
            return r.read().decode(errors="replace")

    def download(self, path, dest, expect=0):
        with self.request(path, timeout=60) as r, open(dest, "wb") as f:
            got, t0, shown = 0, time.time(), 0
            while True:
                chunk = r.read(256 * 1024)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if expect and (time.time() - shown > 0.5 or got >= expect):
                    shown = time.time()
                    rate = got / max(time.time() - t0, 0.01) / 1e6
                    print(f"\r  {os.path.basename(dest)}: {got / 1e6:.1f}/{expect / 1e6:.1f} MB ({rate:.2f} MB/s)",
                          end="", flush=True)
            if expect:
                print()
        return got


def save_json(run, name, obj):
    with open(os.path.join(run, name), "w") as f:
        json.dump(obj, f, indent=2)


def snapshot(disp, run, when):
    save_json(run, f"status_{when}.json", disp.json("/api/status"))
    save_json(run, f"memory_{when}.json", disp.json("/api/memory"))
    with open(os.path.join(run, f"log_{when}.txt"), "w") as f:
        f.write(disp.text("/log"))


def new_run_dir(out):
    base = os.path.join(out, datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    run, n = base, 1
    while os.path.exists(run):
        n += 1
        run = f"{base}-{n}"
    os.makedirs(run)
    return run


def record(args):
    """Returns the run directory."""
    password = args.password or os.environ.get("AIS_DISPLAY_PASSWORD") or getpass.getpass("Display password: ")
    disp = Display(args.host, password)

    print(f"reading settings from {disp.base}")
    config = disp.json("/api/config")
    run = new_run_dir(args.out)
    save_json(run, "config.json", config)
    snapshot(disp, run, "before")
    status = json.load(open(os.path.join(run, "status_before.json")))
    sdr = status["sdr"]
    print(f"  SDR {sdr['state']}: {sdr['tuner'] or '?'}, {sdr['center_hz'] / 1e6:.3f} MHz, {sdr['rate']} S/s, "
          f"gain {gain_str(sdr['gain']['now'])}, ppm setting {config.get('sdr_ppm', 0):+.1f}")
    print(f"  AIS network feed {status['ais']['state']}; card free {status['map']['storage_free'] / 1e6:.0f} MB")
    if sdr["state"] != "receiving":
        raise SystemExit(f"the SDR is '{sdr['state']}', not receiving: can't record")
    if status["ais"]["state"] != "connected":
        print("  note: no network feed, so there's no reference to score the SDR against")

    meta = {
        "host": disp.base,
        "seconds": args.seconds,
        "started": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "center_hz": sdr["center_hz"],
        "rate": sdr["rate"],
        "tuner": sdr["tuner"],
        "ppm_setting": config.get("sdr_ppm", 0),
        "gain_setting": config.get("sdr_gain"),
        "gain_at_start": sdr["gain"]["now"],
        "expected_bytes": args.seconds * RATE * 2,
    }
    save_json(run, "meta.json", meta)

    print(f"recording {args.seconds} s")
    disp.json("/api/sdr/capture", {"seconds": args.seconds})
    t0 = time.time()
    seen = False
    with open(os.path.join(run, "status_timeline.jsonl"), "w") as tl:
        while True:
            time.sleep(0.5 if not seen else 1)
            st = disp.json("/api/status")
            st["_t"] = round(time.time() - t0, 2)
            tl.write(json.dumps(st) + "\n")
            cap = st["sdr"]["capture"]
            if st["sdr"]["state"] == "recording":
                seen = True
                pct = 100 * cap["bytes"] / cap["total"] if cap["total"] else 0
                print(f"\r  {pct:5.1f}%  dropped {cap['dropped'] / 1e6:.2f} MB  sentences {cap['log_lines']}  "
                      f"cpu {st['sdr']['cpu_pct']:.0f}%  noise {st['sdr']['noise_dbfs']:.1f} dBFS", end="", flush=True)
                continue
            if cap["error"]:
                print()
                raise SystemExit(f"the display says: {cap['error']}")
            # A very short recording may finish between polls.
            if seen or (cap["seconds"] == args.seconds and cap["file_bytes"] == meta["expected_bytes"]):
                break
            if time.time() - t0 > 15:
                raise SystemExit("the recording never started")
    print()
    meta["finished"] = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    snapshot(disp, run, "after")

    after = json.load(open(os.path.join(run, "status_after.json")))["sdr"]["capture"]
    print("downloading")
    meta["cu8_bytes"] = disp.download("/api/sdr/capture", os.path.join(run, "capture.cu8"), after["file_bytes"])
    meta["nmea_bytes"] = disp.download("/api/sdr/capture.nmea", os.path.join(run, "capture.nmea"))
    meta["capture_status"] = after
    save_json(run, "meta.json", meta)
    return run


# ---------------------------------------------------------------------------
# Analysis
# ---------------------------------------------------------------------------


def gain_str(g):
    return {-1: "tuner AGC", -2: "best fixed"}.get(g, f"{g / 10:.1f} dB")


def build_tools():
    inc = ["-I../../components/ais/include", "-I../../components/ais_demod/include"]
    demod = ["../../components/ais_demod/ais_demod.c", "../../components/ais/ais.c"]
    tools = {
        "iq_stats": ["iq_stats.c"],
        "ais_demod_file": ["ais_demod_file.c"] + demod,
        "ais_capture_score": ["ais_capture_score.c"] + demod,
    }
    os.makedirs(os.path.join(HOST_TESTS, "build"), exist_ok=True)
    for name, srcs in tools.items():
        cmd = [os.environ.get("CC", "cc"), "-std=c11", "-O2", "-g", *inc, "-o", "build/" + name, *srcs, "-lm"]
        r = subprocess.run(cmd, cwd=HOST_TESTS, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(f"building {name} failed:\n{r.stderr}")
    return {name: os.path.join(HOST_TESTS, "build", name) for name in tools}


def run_tool(cmd, out_path):
    r = subprocess.run(cmd, capture_output=True, text=True)
    with open(out_path, "w") as f:
        f.write(r.stdout)
        if r.stderr:
            f.write("\n--- stderr ---\n" + r.stderr)
    return r.stdout


def payload_type(payload):
    v = ord(payload[0]) - 48
    return v - 8 if v > 40 else v


def payload_mmsi(payload):
    bits = ""
    for ch in payload[:7]:
        v = ord(ch) - 48
        bits += format(v - 8 if v > 40 else v, "06b")
    return int(bits[8:38], 2) if len(bits) >= 38 else 0


def parse_sentence(s):
    """(channel, payload, fragment count, fragment number) of an !AIVDM, or None."""
    i = s.find("!AIVD")
    if i < 0:
        return None
    f = s[i:].split(",")
    if len(f) < 7 or not f[5]:
        return None
    try:
        return f[4] or "?", f[5], int(f[1]), int(f[2])
    except ValueError:
        return None


def parse_nmea_log(path):
    """Packets (first fragment's time) per source: N network, S the display's SDR."""
    out = {"N": [], "S": []}
    if not os.path.exists(path):
        return out
    for line in open(path, errors="replace"):
        m = re.match(r"\s*([\d.]+)\s+([NS])\s+(.*)", line)
        if not m:
            continue
        p = parse_sentence(m.group(3))
        if p and p[3] == 1:
            out[m.group(2)].append({"t": float(m.group(1)), "ch": p[0], "type": payload_type(p[1]),
                                    "mmsi": payload_mmsi(p[1])})
    return out


def parse_offline(text):
    pkts = []
    for line in text.splitlines():
        m = re.match(r"\s*([\d.]+) s\s+(\S+)", line)
        if m:
            p = parse_sentence(m.group(2))
            if p and p[3] == 1:
                pkts.append({"t": float(m.group(1)), "ch": p[0], "type": payload_type(p[1]),
                             "mmsi": payload_mmsi(p[1])})
    m = re.search(r"correction ([+-][\d.]+) Hz \(([+-][\d.]+) ppm\)", text)
    return pkts, (float(m.group(1)), float(m.group(2))) if m else (None, None)


def source_summary(pkts):
    by_type, by_ch = {}, {}
    for p in pkts:
        by_type[p["type"]] = by_type.get(p["type"], 0) + 1
        by_ch[p["ch"]] = by_ch.get(p["ch"], 0) + 1
    return {"packets": len(pkts), "vessels": len({p["mmsi"] for p in pkts}),
            "by_channel": dict(sorted(by_ch.items())), "by_type": dict(sorted(by_type.items()))}


def match_bursts(iq, offline, live):
    """Each burst with whether a packet on its channel ended during it (offline)
    or was logged soon after (the display's live decoder)."""
    out = []
    for ch, info in iq["channels"].items():
        for b in info["bursts"]:
            start, end = b["t"] - 0.02, b["t"] + b["dur_ms"] / 1000 + 0.06
            row = dict(b, ch=ch)
            row["offline"] = any(p["ch"] == ch and start <= p["t"] <= end for p in offline)
            row["live"] = any(p["ch"] == ch and start <= p["t"] <= end + 1.0 for p in live)
            out.append(row)
    return sorted(out, key=lambda r: r["t"])


def timeline_summary(run):
    path = os.path.join(run, "status_timeline.jsonl")
    if not os.path.exists(path):
        return {}
    rows = [json.loads(l) for l in open(path) if l.strip()]
    rec = [r for r in rows if r["sdr"]["state"] == "recording"] or rows
    if not rec:
        return {}
    s = [r["sdr"] for r in rec]
    return {
        "polls": len(rec),
        "cpu_pct_max": max(x["cpu_pct"] for x in s),
        "cpu_pct_mean": round(sum(x["cpu_pct"] for x in s) / len(s), 1),
        "decoder_dropped_bytes": s[-1]["dropped_bytes"] - s[0]["dropped_bytes"],
        "capture_dropped_bytes": max(x["capture"]["dropped"] for x in s),
        "bad_crc_delta": s[-1]["bad_crc"] - s[0]["bad_crc"],
        "noise_dbfs_range": [min(x["noise_dbfs"] for x in s), max(x["noise_dbfs"] for x in s)],
        "gains_seen": sorted({x["gain"]["now"] for x in s}),
        "ppm_live_range": [min(x["ppm"] for x in s), max(x["ppm"] for x in s)],
        "sdr_rx_a_b": [rec[-1]["receivers"]["sdr"]["a"] - rec[0]["receivers"]["sdr"]["a"],
                       rec[-1]["receivers"]["sdr"]["b"] - rec[0]["receivers"]["sdr"]["b"]],
        "net_rx_a_b": [rec[-1]["receivers"]["network"]["a"] - rec[0]["receivers"]["network"]["a"],
                       rec[-1]["receivers"]["network"]["b"] - rec[0]["receivers"]["network"]["b"]],
    }


def memory_summary(run):
    out = {}
    for when in ("before", "after"):
        path = os.path.join(run, f"memory_{when}.json")
        if os.path.exists(path):
            m = json.load(open(path))
            out[when] = {h["name"]: {k: h[k] for k in ("free", "largest", "min_free")} for h in m.get("heaps", [])}
    return out


def score_numbers(text):
    out = {}
    for key, pat in {
        "reference_packets": r"reference \(network\): (\d+) packets",
        "live_packets": r"SDR live: (\d+)",
        "network_delay_s": r"network feed delay ([\d.+-]+) s",
    }.items():
        m = re.search(pat, text)
        if m:
            out[key] = float(m.group(1))
    m = re.search(r"live (\d+)/(\d+) \((\d+)%\),\s+offline (\d+)/(\d+) \((\d+)%\)", text)
    if m:
        out["live_found"], out["reference_in_capture"], out["live_pct"] = (int(m.group(i)) for i in (1, 2, 3))
        out["offline_found"], out["offline_pct"] = int(m.group(4)), int(m.group(6))
    return out


def findings(s):
    """Things worth looking at, worst first."""
    out = []
    iq, meta, tl = s["iq"], s.get("meta", {}), s.get("timeline", {})
    exp = meta.get("expected_bytes")
    if exp and s["cu8_bytes"] != exp:
        out.append(f"file is {s['cu8_bytes']} bytes, expected {exp}: the recording is short")
    if tl.get("capture_dropped_bytes"):
        out.append(f"{tl['capture_dropped_bytes'] / 1e6:.2f} MB of I/Q dropped while writing to the card "
                   "(the file has gaps; bursts may be cut)")
    if tl.get("decoder_dropped_bytes"):
        out.append(f"the live decoder fell behind and skipped {tl['decoder_dropped_bytes'] / 1e6:.2f} MB of samples")
    if iq["dead_frames"]:
        out.append(f"{iq['dead_frames']} frames of constant samples (stalled stream?)")
    if iq["clip_pct"] > 0.01:
        out.append(f"{iq['clip_pct']:.3f}% of samples clipped: gain too high")
    hot = [b for b in s["bursts"] if b["peak_db"] > -3]
    if hot:
        out.append(f"{len(hot)} bursts peak within 3 dB of full scale (strongest {max(b['peak_db'] for b in hot):.1f} dB):"
                   " little headroom, strong nearby transmitters may clip")
    if max(iq["bits_used_i"], iq["bits_used_q"]) < 5:
        out.append(f"only ~{iq['bits_used_i']:.1f} bits of the ADC used: gain may be too low")
    if abs(iq["dc_i"]) > 0.02 or abs(iq["dc_q"]) > 0.02:
        out.append(f"DC offset I {iq['dc_i']:+.3f} Q {iq['dc_q']:+.3f}")
    for sp in iq["spurs"]:
        near = [c for c, info in iq["channels"].items() if abs(sp["hz"] - info["offset_hz"]) < 8000]
        if near:
            out.append(f"spur at {sp['hz']:+.0f} Hz ({sp['db_above_floor']:.0f} dB) inside channel {near[0]}")
    missed = [b for b in s["bursts"] if not b["offline"]]
    if s["bursts"]:
        strong = [b for b in missed if b["snr_db"] >= 20]
        if strong:
            out.append(f"{len(strong)} strong bursts (SNR >= 20 dB) didn't decode offline: decoder or interference")
    else:
        out.append("no bursts above the noise on either channel: antenna, bias-tee, or no traffic")
    live_miss = [b for b in s["bursts"] if b["offline"] and not b["live"]]
    if live_miss:
        out.append(f"{len(live_miss)} packets decoded offline but not by the display live (live decoder dropped them?)")
    if tl.get("gains_seen") and len(tl["gains_seen"]) > 1:
        out.append(f"gain changed while recording: {[gain_str(g) for g in tl['gains_seen']]}")
    sc = s.get("score", {})
    if sc.get("reference_in_capture"):
        out.append(f"SDR heard {sc.get('live_pct', 0)}% live / {sc.get('offline_pct', 0)}% offline of the "
                   f"{sc['reference_in_capture']} packets the network feed did")
    return out


def analyze(run, ppm=None):
    cu8, nmea = os.path.join(run, "capture.cu8"), os.path.join(run, "capture.nmea")
    if not os.path.exists(cu8):
        raise SystemExit(f"no capture.cu8 in {run}")
    meta = json.load(open(os.path.join(run, "meta.json"))) if os.path.exists(os.path.join(run, "meta.json")) else {}
    if ppm is None:
        ppm = meta.get("ppm_setting", 0)
    print(f"analysing {run}")
    tools = build_tools()

    iq_text = run_tool([tools["iq_stats"], cu8], os.path.join(run, "iq_stats.json"))
    iq = json.loads(iq_text)
    offline_text = run_tool([tools["ais_demod_file"], cu8, str(ppm)], os.path.join(run, "offline_decode.txt"))
    offline, (corr_hz, corr_ppm) = parse_offline(offline_text)
    score_text = ""
    if os.path.exists(nmea):
        score_text = run_tool([tools["ais_capture_score"], cu8, nmea, "-v", str(ppm)], os.path.join(run, "score.txt"))
    log = parse_nmea_log(nmea)

    s = {
        "run": os.path.basename(os.path.abspath(run)),
        "meta": meta,
        "ppm_used": ppm,
        "cu8_bytes": os.path.getsize(cu8),
        "offline_correction": {"hz": corr_hz, "ppm": corr_ppm},
        "network": source_summary(log["N"]),
        "live": source_summary(log["S"]),
        "offline": source_summary(offline),
        "timeline": timeline_summary(run),
        "memory": memory_summary(run),
        "score": score_numbers(score_text),
        "iq": {k: v for k, v in iq.items() if k not in ("timeline", "hist_i", "hist_q", "spectrum_db")},
    }
    s["iq"]["channels"] = {c: {k: v for k, v in info.items() if k != "bursts"} for c, info in iq["channels"].items()}
    s["bursts"] = match_bursts(iq, offline, log["S"])
    s["findings"] = findings(s)
    save_json(run, "summary.json", s)
    with open(os.path.join(run, "report.md"), "w") as f:
        f.write(report(s))
    print(report(s))
    print(f"saved in {run}")


def report(s):
    iq, meta, tl = s["iq"], s["meta"], s["timeline"]
    L = [f"# SDR test {s['run']}", ""]
    if meta:
        L += [f"- display {meta.get('host')}, {meta.get('tuner')}",
              f"- {meta.get('seconds')} s at {meta.get('center_hz', 0) / 1e6:.3f} MHz, {meta.get('rate')} S/s",
              f"- gain setting {gain_str(meta.get('gain_setting', 0))}, at start {gain_str(meta.get('gain_at_start', 0))}; "
              f"ppm setting {meta.get('ppm_setting', 0):+.1f}", ""]
    L += ["## Findings", ""] + [f"- {x}" for x in s["findings"] or ["nothing stands out"]] + [""]
    L += ["## Signal", "",
          f"- {iq['seconds']:.1f} s, mean power {iq['power_db']:.1f} dBFS, clipped {iq['clip_pct']:.4f}%",
          f"- I range {iq['range_i']}, Q range {iq['range_q']} (~{iq['bits_used_i']:.1f} bits used)",
          f"- DC I {iq['dc_i']:+.4f} Q {iq['dc_q']:+.4f}; DC bin {iq['dc_bin_db_above_floor']:+.1f} dB over the floor",
          f"- spectrum floor {iq['spectrum_floor_db']:.1f} dB/bin; spurs: "
          + (", ".join(f"{x['hz']:+.0f} Hz ({x['db_above_floor']:.0f} dB)" for x in iq["spurs"]) or "none")]
    for c, info in iq["channels"].items():
        n = [b for b in s["bursts"] if b["ch"] == c]
        L.append(f"- channel {c} ({info['offset_hz']:+.0f} Hz): noise {info['noise_db']:.1f} dB, {len(n)} bursts, "
                 f"{sum(b['offline'] for b in n)} decoded offline, {sum(b['live'] for b in n)} live")
    oc = s["offline_correction"]
    if oc["hz"] is not None:
        L.append(f"- offline decoder frequency correction {oc['hz']:+.0f} Hz ({oc['ppm']:+.2f} ppm total)")
    L += ["", "## Packets", "", "| source | packets | vessels | A | B | types |", "|---|---|---|---|---|---|"]
    for name in ("network", "live", "offline"):
        x = s[name]
        types = " ".join(f"{t}:{n}" for t, n in x["by_type"].items())
        L.append(f"| {name} | {x['packets']} | {x['vessels']} | {x['by_channel'].get('A', 0)} | "
                 f"{x['by_channel'].get('B', 0)} | {types} |")
    if tl:
        L += ["", "## Display while recording", "",
              f"- decoder CPU {tl['cpu_pct_mean']}% mean, {tl['cpu_pct_max']}% max",
              f"- dropped: capture {tl['capture_dropped_bytes']} bytes, live decoder {tl['decoder_dropped_bytes']} bytes",
              f"- bad CRC +{tl['bad_crc_delta']}; noise {tl['noise_dbfs_range'][0]:.1f}..{tl['noise_dbfs_range'][1]:.1f} dBFS; "
              f"live ppm {tl['ppm_live_range'][0]:+.2f}..{tl['ppm_live_range'][1]:+.2f}",
              f"- gain {', '.join(gain_str(g) for g in tl['gains_seen'])}"]
    mem = s["memory"]
    if mem.get("after"):
        L.append("- heaps after (free / largest / min free): " + "; ".join(
            f"{k} {v['free'] // 1024}K/{v['largest'] // 1024}K/{v['min_free'] // 1024}K" for k, v in mem["after"].items()))
    missed = [b for b in s["bursts"] if not b["offline"]]
    if missed:
        L += ["", "## Bursts that didn't decode", "", "| t (s) | ch | ms | peak dB | SNR dB |", "|---|---|---|---|---|"]
        L += [f"| {b['t']:.3f} | {b['ch']} | {b['dur_ms']:.0f} | {b['peak_db']:.1f} | {b['snr_db']:.1f} |"
              for b in missed[:40]]
        if len(missed) > 40:
            L.append(f"\n({len(missed) - 40} more in summary.json)")
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", default=os.environ.get("AIS_DISPLAY_HOST"),
                    help="display IP or name (or $AIS_DISPLAY_HOST)")
    ap.add_argument("--password", help="access point password (or $AIS_DISPLAY_PASSWORD)")
    ap.add_argument("--seconds", type=int, default=60, help=f"capture length, 1-{MAX_SECONDS} (default 60)")
    ap.add_argument("--out", default=os.path.join(REPO, "captures"), help="where run directories go")
    ap.add_argument("--analyze", metavar="DIR", help="only analyse an existing run directory")
    ap.add_argument("--ppm", type=float, help="tuning correction for the offline decode (default: the display's)")
    args = ap.parse_args()

    if args.analyze:
        analyze(args.analyze, args.ppm)
        return
    if not args.host:
        ap.error("give the display's address")
    if not 1 <= args.seconds <= MAX_SECONDS:
        ap.error(f"--seconds must be 1-{MAX_SECONDS}")
    try:
        run = record(args)
    except KeyboardInterrupt:
        sys.exit("\nstopped (a recording already started carries on on the display)")
    analyze(run, args.ppm)


if __name__ == "__main__":
    main()
