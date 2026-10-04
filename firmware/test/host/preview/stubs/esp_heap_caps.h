#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define heap_caps_malloc(n, caps) malloc(n)
