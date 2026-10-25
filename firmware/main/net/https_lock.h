// One HTTPS request at a time, across every task that fetches (weather, tide,
// WeatherLink). Each TLS connection holds a lot of memory while it's open;
// several at once, as happened when the network came up, ran internal RAM out
// and starved the SDIO link to the Wi-Fi co-processor.

#pragma once

void https_lock_init(void);     // first, before any task fetches
void https_lock(void);
void https_unlock(void);
