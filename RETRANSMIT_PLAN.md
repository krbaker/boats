# Plan: retransmit what the dongle receives

Send the AIS sentences the RTL-SDR dongle decodes on to **up to five destinations**, for example online sharing services or chart software on the boat. For each destination you pick **UDP or TCP**, enter an **IP address (or host name) and port**, and turn it on.

Only the dongle's sentences are sent. The network (TCP) feed is never retransmitted: it's another station's data, and sending it on would duplicate it on the sharing services, or loop it back to its source.

This is step 6 of `RTL_SDR_PLAN.md`.

## The services

| Service | How it takes a feed | Fits a destination? |
|---|---|---|
| **MarineTraffic** | Register the station. They give you an IP address and a UDP port that's unique to it, and you push plain `!AIVDM` sentences there. | Yes: UDP, their IP and port. |
| **AIS Friends** | Register the station. The registration gives you an upload IP address and port, plus a station page. | Yes: UDP, their IP and port. |
| **aiscatcher.org** | Takes plain NMEA on an IP address and port it gives the station. (AIS-catcher's own `-X` sharing uses a different format, over TCP to port 4242; we don't need it.) | Yes: their IP and port. |

None of the three needs a station ID in the data: the address and port they assign identify the station.

## What it does

- **Three destination slots**, one per service you feed, each with its own on/off, UDP or TCP, host and port. They all run at once, each with its own socket and counters, so one being down doesn't affect the others.
- **UDP:** each sentence goes as its own datagram. This is what MarineTraffic and AIS Friends take, and OpenCPN can listen for it too.
- **TCP:** the display connects out to the IP and port as a client, and streams the sentences one per line. If the connection drops or can't be made, it retries every 5 s, backing off to once a minute.
- **What's sent:** the dongle's `!AIVDM` sentences with their channel letter (A/B), each ending in CR LF, exactly as the decoder produces them and as AIS-catcher would send them.
- **A multi-part message goes as a unit.** A type 5 message is two sentences. Both are queued together or not at all, so a full queue can't send half a message.
- **Nothing is held back:** a sentence that can't be sent right away (TCP not connected, the queue full) is dropped and counted. AIS data that arrives late is useless, and the services discard it anyway.

## How it fits the current firmware

The SDR decoder's `on_sentence()` (`sdr_probe.c`) passes each sentence to the AIS parser. It gets one more call: `fwd_sentence(line)`.

```
SDR decoder ─▶ fwd_sentence() ─▶ queue (8 KB) ─▶ forwarder task ─┬─▶ destination 1 (UDP socket / TCP connection)
                    │                                            ├─▶ destination 2
                    │                                            └─▶ destination 3
                    └─ drops and counts when the queue is full: never blocks the decoder
```

- **Never blocking:** `fwd_sentence()` only copies the sentence into a FreeRTOS ring buffer and returns, so a stalled TCP destination can't hold up the decoder.
- **The forwarder task** (low priority) owns the sockets. It resolves host names (again on each TCP reconnect, and every 10 minutes for UDP), sends, reconnects, and keeps the counters.
- **Settings apply live.** Saving on the web page raises a new `SETTINGS_EV_FWD`. The task closes the destination's socket and starts again with the new settings, like the network client does now.
- **Without a dongle,** nothing is sent; the destinations just wait.

## Settings and web page

Three sets of these, saved in NVS like the rest (`fwd1_*` to `fwd3_*`):

| Setting | Values | Default |
|---|---|---|
| `enabled` | on / off | off |
| `proto` | UDP / TCP | UDP |
| `host` | IP address or host name, up to 63 characters | empty |
| `port` | 1–65535 | empty |

The web page gets a "Retransmit" section, after the RTL-SDR one, with five rows: on/off, UDP/TCP, address, port. Under each row is its status line. A row can't be turned on without a host and port.

## Status

- **Data:** in `/api/status`, a `retransmit` array of five entries, each with `state`, `sent`, `sent_per_min`, `dropped`, the last error and, for TCP, how long it's been connected.
- **States:** `off`, `not configured`, `waiting for network`, `connecting` (TCP), `sending`, `error`.
- **Where it shows:** on the web page, and on the display's Network screen as one line, "sending to N of M", plus the first problem if a destination isn't sending. The screen is round and already full, so one line per destination was too much.

## Steps

| # | Step | Done when |
|---|---|---|
| 1 | **Settings, API and web page:** the three destinations' settings, `config_get`/`config_post`, the Retransmit rows. | Settings save, survive a restart, and bad values are refused with a message. |
| 2 | **Forwarder** (`main/net/ais_fwd.c`): the queue, the task, UDP sending, the TCP client with reconnect and back-off, multi-part messages queued whole. Called from the SDR decoder. | `nc -lu 10110` (UDP) and `nc -lk 10111` (TCP) on a PC, as two of the destinations, both show the sentences. Stopping and restarting the TCP `nc` reconnects by itself, and UDP keeps going meanwhile. |
| 3 | **Status:** `/api/status`, the web page and the Network screen. | The counters match what `nc` received. |
| 4 | **Real use:** MarineTraffic, AIS Friends and aiscatcher.org, one in each slot. Then a day running, including a Wi-Fi drop. | All three services list the station as receiving, and nothing stalls or leaks. |

Testing needs the dongle on an antenna and decoding, since the network feed isn't forwarded.

## Progress (2026-10-06)

- **Step 1 built, not yet tested on the display.**
  - **Settings:** `settings_t.fwd[3]` (`fwd_dest_t`), saved in NVS as `fwd1_on`, `fwd1_proto`, `fwd1_port`, `fwd1_host`, up to `fwd3_*`. A change raises `SETTINGS_EV_FWD`.
  - **API:** `/api/config` has a `retransmit` array of `{enabled, proto: "udp"|"tcp", host, port}`. A POST may send all three or fewer.
  - **Checks:** the host must be letters, digits, dots and hyphens. The port must be 1–65535. A destination can't be turned on without both, and the page and the server each say which destination is wrong.
  - **Web page:** the Retransmit section with the three rows. The status lines come with step 3.
- **Step 2 built and tested on the PC, not yet on the display.**
  - **Where:** `main/net/ais_fwd.c`, started from `app_main`, and fed by the SDR decoder's `on_sentence()`.
  - **Queue:** an 8 KB ring buffer. A multi-part message goes in as one item. A part arriving without the part before it is dropped.
  - **Task:** one low-priority task owns all the sockets, polling every 200 ms. TCP sockets are non-blocking. If TCP can only take part of a message, the rest goes first on the next pass. A connection that takes nothing for 30 s is reconnected. The far end closing is noticed even with nothing to send.
  - **Status:** state, sent, dropped, sent in the last minute, TCP connected time, and the last error, ready for step 3 (`ais_fwd_get_status()`).
  - **Host test** (`test/host/test_ais_fwd.c`, in `run_tests.sh`): runs the real `ais_fwd.c` on small FreeRTOS stand-ins (`test/host/fwd_stubs/`) against local UDP and TCP listeners. It covers one datagram per sentence over UDP, ordered lines over TCP, a two-part message, a lone second part dropped, the TCP receiver going away and the reconnect after the 5 s back-off, a Wi-Fi drop and its return, a live settings change, a destination turned on without an address, connection refused without holding up the others, and turning a destination off.
- **Step 3 built, not yet tested on the display.**
  - **`/api/status`:** a `retransmit` array, each entry with `state`, `sent`, `sent_per_min` (the last full minute), `dropped`, `connected_s` (TCP; -1 when not connected) and `error`.
  - **Web page:** a status line under each destination row: what it's doing, how much it has sent, how long it's been connected, and how many sentences weren't sent. Green when sending, red with the error and "retrying" when failing, amber otherwise.
  - **Network screen:** "Retransmit: sending to N of M", plus the first problem.
- **Step 4:** not started.

## Decisions

- **TCP connects out** to the destination's IP and port. There's no TCP server on the display.
- **Three destinations,** one per service.
- **The dongle only.** The network feed is never retransmitted.
- **Services:** MarineTraffic, AIS Friends and aiscatcher.org. All three take plain NMEA on an IP address and port they assign.
