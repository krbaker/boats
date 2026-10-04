#include <math.h>
#include <stdio.h>

#include "geo.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

    // Web Mercator reference values (EPSG:3857).
    double mx, my;
    geo_mercator(0.0, 0.0, &mx, &my);
    CHECK_NEAR(mx, 0, 1e-6);
    CHECK_NEAR(my, 0, 1e-6);
    geo_mercator(47.6005, -122.3550, &mx, &my);
    CHECK_NEAR(mx, -13620496.29, 0.1);
    CHECK_NEAR(my, 6040647.75, 0.1);

    // A chart 2 nm either side of the centre: Mercator metres are 1/cos(lat) longer than ground metres.
    double half = 2 * 1852.0 / cos(47.6005 * M_PI / 180);
    geo_bbox_t bb = {.x_min = mx - half, .y_min = my - half, .x_max = mx + half, .y_max = my + half};
    float x, y;
    geo_project(&bb, 800, 800, 47.6005, -122.3550, &x, &y);
    CHECK_NEAR(x, 400, 0.01);
    CHECK_NEAR(y, 400, 0.01);
    // One nautical mile north is one minute of latitude, a quarter of the way to the top edge. (A minute
    // of arc is 1852 m on a slightly smaller sphere than Mercator's, hence the 1 px tolerances.)
    geo_project(&bb, 800, 800, 47.6005 + 1.0 / 60, -122.3550, &x, &y);
    CHECK_NEAR(x, 400, 0.01);
    CHECK_NEAR(y, 200, 1.0);
    // One nautical mile east.
    geo_project(&bb, 800, 800, 47.6005, -122.3550 + 1.0 / 60 / cos(47.6005 * M_PI / 180), &x, &y);
    CHECK_NEAR(x, 600, 1.0);
    CHECK_NEAR(y, 400, 0.01);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all geo tests passed\n");
    return 0;
}
