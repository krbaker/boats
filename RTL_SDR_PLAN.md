# Plan: receive AIS directly with an RTL-SDR

Plug an RTL-SDR dongle and a VHF antenna into the display's USB-A port, and the display decodes AIS itself, with no network AIS server needed. The TCP feed keeps running alongside it: both sources feed the map at the same time. The display can also send what its own dongle receives on to other places: chart software on the boat, and online AIS sharing services.

## Decisions so far

- **Sources:** the dongle and the Wi-Fi (TCP) feed run together.
- **Dongle:** an RTL-SDR Blog **V3** from rtl-sdr.com (RTL2832U + R820T2 tuner, 1 ppm TCXO). The V3 is supported first; V4 (R828D tuner) support comes later.
- **Re-send:** yes. There's a TCP server for local chart software, and UDP forwarding to online services (step 6).
- **Licensing:** GPL is fine. librtlsdr's device code gets ported, and the firmware becomes GPL-2.0-or-later. A LICENSE file and source headers are added with the port (step 2).

## How it fits the current firmware

Today: `nmea_client` (TCP) → `ais_feed_line()` / `ais_decode_payload()` → `vessel_db` → UI.

The SDR path produces the same thing a network feed does: `!AIVDM` sentences. It feeds them into the existing parser, so the vessel table, UI, filters and web API don't change.

```
RTL-SDR ─USB bulk─▶ rtlsdr driver ─IQ ring buffer─▶ ais_demod (2 channels) ─▶ !AIVDM ─┐
                                                                                      ├─▶ existing AIS parser ─▶ vessel_db
TCP feed (nmea_client) ───────────────────────────────────────────────────────────────┘
```

## Hardware

- **Port:** the board's USB-A "USB-OTG" connector is the ESP32-P4's USB host port. The BSP already has `bsp_usb_host_start(mode, limit_500mA)`. The port is powered from the USB-C input, so the supply has to cover the display plus the dongle's ~250–300 mA. Step 1 checks the port is High-Speed (480 Mbps); Full-Speed would also do, see the bandwidth figures in step 2.
- **Dongle:** your RTL-SDR Blog dongle has a 1 ppm TCXO, so it lands within about 160 Hz of the right frequency and needs no calibration. The frequency correction stays in, for other dongles (cheap crystals are 30–100 ppm off, 5–16 kHz at 162 MHz).
  - The V3 uses an R820T2 tuner.
  - The V4 uses an R828D tuner with a built-in upconverter switch: a different init sequence, and a different way of setting the frequency.
  - Both support the bias-tee for powering an active antenna or LNA.
- **Antenna:** a marine VHF antenna (AIS is 161.975 and 162.025 MHz). A splitter is needed to share one with a VHF radio. Never connect the dongle directly to an antenna a radio transmits on.
- **Interference:** the display, Wi-Fi and the P4 itself are noisy. Step 1 measures the noise floor with the dongle on a short USB extension against one plugged straight in.

## Radio and signal processing

- **Tuning:** centre 162.000 MHz, which puts AIS channel A at −25 kHz and B at +25 kHz. Sample at 288 kS/s complex, 8-bit I/Q: 576 kB/s over USB.
- **Per channel:** mix to baseband with an NCO → low-pass FIR and decimate to 48 kS/s (5 samples per bit at 9600 bit/s) → FM discriminator (phase difference of consecutive samples) → matched filter for GMSK (BT 0.4/0.5) → symbol timing recovery (early-late or Gardner PLL).
- **Decoding the bits:** NRZI decode → HDLC: find 0x7E flags, remove the stuffed bits, check the CRC-16 (CCITT), reverse the bit order per byte → AIS payload.
- **Output:** six-bit armour into `!AIVDM` sentences, split into fragments if needed, with the channel letter A/B → the existing parser.
- **Frequency correction:** a manual ppm setting, plus automatic tracking. The demodulator measures each good packet's DC offset after the discriminator and slowly adjusts the NCOs, so a cheap dongle locks on by itself. The current estimate is shown as "ppm".
- **CPU estimate:** about 30–60 MOPS for both channels. That should fit in one P4 core alongside the UI; step 5 confirms it. Floating point on the P4's FPU is fine; Q15 fixed point is the fallback if needed.

## Software licensing (decided: GPL)

**librtlsdr** (the standard driver, including the tuner register setup sequences) and the well-known AIS decoders (rtl-ais, AIS-catcher) are **GPL**. The options are:

1. Port librtlsdr's device code to the ESP-IDF USB host API, and license the firmware GPL-2.0-or-later. This is the fastest and lowest-risk route.
2. Write our own RTL2832U/R820T driver from datasheets and register dumps. This avoids the GPL, but takes much longer, and in practice the init sequences end up near-identical.

**Option 1 chosen.** The demodulator itself (step 3) is still written from scratch; it's standard DSP.

## Steps

| # | Step | Done when |
|---|---|---|
| 1 | **USB host spike.** Start the BSP USB host, enumerate the dongle, read its descriptors, run a bulk-read throughput test. Measure supply current and the RF noise floor. | The dongle enumerates and sustains ≥ 2× the needed 576 kB/s, and the display stays stable on the combined power draw. |
| 2 | **RTL-SDR driver** (`components/rtlsdr`): port the RTL2832U demod init, I2C repeater, R820T/R828D/R860 tuner init, set frequency / sample rate / gain (manual and AGC) / ppm, and the bias-tee toggle (Blog V3/V4). Stream with several queued 16 KiB bulk transfers into a PSRAM ring buffer. | IQ samples stream continuously for an hour with no dropped transfers. A capture dumped over the web matches `rtl_sdr` on a PC. |
| 3 | **Demodulator** (`components/ais_demod`): portable C, no ESP-IDF, built and unit-tested on the host like the AIS parser. Tests use synthetic IQ (extend `tools/ais_sim.py` to write GMSK baseband for both channels with noise and frequency offset), plus a real capture recorded on a PC: `rtl_sdr -f 162000000 -s 288000 -g 40 ais.cu8`. | Every synthetic packet decodes at ≥ 10 dB SNR and with ±5 kHz offset. The real capture decodes at least as many packets as AIS-catcher or rtl-ais does on the same file. |
| 4 | **Integration:** an SDR task on core 1 (driver → demod → parser), running alongside the TCP feed. Both sources update the same vessel table, so a vessel heard by both just gets updated twice. Each source can be turned on or off on the web page, alongside the dongle's gain, ppm and bias-tee settings. Show the status on the Network screen and `/api/status`: dongle found and its model, signal level, packets per minute per channel, CRC failures, ppm estimate. A message counter shows which source each vessel was heard on. | Live vessels from the dongle and from the TCP feed appear on the map together. Unplugging and replugging the dongle recovers by itself. |
| 5 | **Performance and soak:** profile CPU per stage, and check map panning stays smooth while decoding. Then a 24 h run on a busy channel. | Under 50% of one core for the demodulator, no UI stutter, and no leaks or resets over 24 h. |
| 6 | **Re-send what we receive.** First, a TCP NMEA server (port 10110, several clients at once) so OpenCPN or a chartplotter can use the display as its AIS receiver. Second, UDP forwarding of the raw `!AIVDM` sentences to a list of host:port destinations, which is how MarineTraffic, AISHub, VesselFinder and aiscatcher.org take station feeds. By default only what the dongle receives is forwarded, not the TCP feed: sending another station's data on would duplicate it, or loop it straight back to its source. Each output's on/off setting, destinations and sent count are on the web page. | OpenCPN shows the same targets as the display, and an online service lists the station as receiving. |

## Risks

- **USB host on the P4:** compatibility with the RTL2832U's bulk endpoint, and the High-Speed vs Full-Speed question. Step 1 settles this before anything else is built.
- **Power:** the dongle on top of the display may exceed what the USB-C supply or the board's port switch delivers. The fallback is a powered USB hub, or a dongle with lower draw.
- **RF self-interference** from the display and Wi-Fi reduces sensitivity. The fallbacks are a short USB extension, a ferrite, and keeping the antenna away from the unit.
- **CPU:** if both channels plus the UI are too much, use the P4's PIE SIMD instructions for the FIRs, decimate harder before the matched filter, or move to fixed point.
- **Licensing:** see the decision above.

## Still to decide

1. Which online services do you plan to feed? Some, for example MarineTraffic, give each station its own UDP host and port; others want a station ID in each message.
