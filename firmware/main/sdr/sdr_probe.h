// RTL-SDR bring-up test: see sdr_probe.c and RTL_SDR_PLAN.md (step 1).

#pragma once

// Starts USB host on the USB-A port and tests any RTL2832U dongle plugged in,
// logging the results. Only built with CONFIG_AIS_SDR_PROBE.
void sdr_probe_start(void);
