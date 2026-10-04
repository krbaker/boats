#pragma once
#include <stdint.h>
typedef uint32_t EventBits_t;
typedef void *EventGroupHandle_t;
static inline EventBits_t xEventGroupClearBits(EventGroupHandle_t g, EventBits_t b) { return 0; }
