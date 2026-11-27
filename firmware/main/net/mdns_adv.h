// mDNS: the display answers to <mdns_name>.local and advertises its config
// page (_http._tcp) and, when it's on, the TCP NMEA server (_nmea-0183._tcp).

#pragma once

#include <stddef.h>

void mdns_adv_start(void);

// The name actually in use, without ".local": the setting, unless another
// device already had it and mDNS picked another ("ais-2"). "" before start.
void mdns_adv_hostname(char *out, size_t cap);
