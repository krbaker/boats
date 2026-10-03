#include <math.h>
#include <stdio.h>

#include "geo.h"

static int failures;

#define CHECK_NEAR(a, b, eps)                                                               \
    do {                                                                                    \
        if (fabs((double)(a) - (double)(b)) >= (eps)) {                                     \
            printf("FAIL %s:%d: %s = %f, expected %f\n", __FILE__, __LINE__, #a, (double)(a), \
                   (double)(b));                                                            \
            failures++;                                                                     \
        }                                                                                   \
    } while (0)

int main(void)
{
    // One minute of latitude is one nautical mile.
    CHECK_NEAR(geo_distance_nm(47.0, -122.0, 47.0 + 1.0 / 60, -122.0), 1.0, 0.01);
    // Seattle to Victoria BC, ~63 nm.
    CHECK_NEAR(geo_distance_nm(47.6062, -122.3321, 48.4284, -123.3656), 63.0, 1.5);

    CHECK_NEAR(geo_bearing_deg(47.0, -122.0, 48.0, -122.0), 0.0, 0.01);
    CHECK_NEAR(geo_bearing_deg(47.0, -122.0, 47.0, -121.0), 89.6, 0.5);
    CHECK_NEAR(geo_bearing_deg(47.0, -122.0, 46.0, -122.0), 180.0, 0.01);
    CHECK_NEAR(geo_bearing_deg(47.0, -122.0, 47.0, -123.0), 270.4, 0.5);

    geo_bbox_t bb = {.lat_min = 47.0, .lat_max = 48.0, .lon_min = -123.0, .lon_max = -122.0};
    float x, y;
    geo_project(&bb, 800, 800, 47.5, -122.5, &x, &y);
    CHECK_NEAR(x, 400, 0.01);
    CHECK_NEAR(y, 400, 0.01);
    geo_project(&bb, 800, 800, 48.0, -123.0, &x, &y);
    CHECK_NEAR(x, 0, 0.01);
    CHECK_NEAR(y, 0, 0.01);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all geo tests passed\n");
    return 0;
}
