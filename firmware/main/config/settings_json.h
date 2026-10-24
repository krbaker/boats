// settings_t as JSON. The web page's settings (GET/POST /api/config) and the
// microSD card's config.json share these keys and checks.

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "settings.h"

// Every setting in settings_t (not the Wi-Fi network or any secret).
cJSON *settings_to_json(const settings_t *s);

// Apply the keys o has to s; missing ones are left as they are. False on a bad
// value, with msg saying which (s may then be partly changed).
bool settings_from_json(const cJSON *o, settings_t *s, char *msg, size_t cap);

// A string field, if o has it: false (and *bad = key) if it isn't a string or
// doesn't fit in cap.
bool settings_json_str(const cJSON *o, const char *key, char *dst, size_t cap, const char **bad);

// Letters and digits only, and shorter than cap.
bool settings_alnum(const char *s, size_t cap);
