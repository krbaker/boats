// LVGL configuration for the host preview: the firmware's own Kconfig values
// (fonts, widgets) minus the bits that only make sense on the device.
#pragma once
#include "sdkconfig.h"
#undef CONFIG_LV_USE_LOG
#undef CONFIG_LV_USE_PERF_MONITOR
#undef CONFIG_LV_USE_SYSMON
#undef CONFIG_LV_USE_FS_STDIO
#undef CONFIG_LV_USE_FS_POSIX
#undef CONFIG_LV_USE_FREETYPE
#undef CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM
