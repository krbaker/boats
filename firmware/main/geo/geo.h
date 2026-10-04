// Geodesy helpers: distance/bearing and chart projection.
//
// Pure C (no ESP-IDF dependencies) so it can be unit-tested on the host.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bounds of a chart image in Web Mercator (EPSG:3857) metres. The map
// builder renders charts in EPSG:3857, so pixels are linear in these.
typedef struct {
    double x_min, y_min;
    double x_max, y_max;
} geo_bbox_t;

// Great-circle distance in nautical miles.
double geo_distance_nm(double lat1, double lon1, double lat2, double lon2);

// Initial true bearing from point 1 to point 2, 0..360 degrees.
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

// Lat/lon to Web Mercator (EPSG:3857) metres.
void geo_mercator(double lat, double lon, double *x, double *y);

// Project lat/lon onto a w x h image covering bbox. (0,0) is top-left.
void geo_project(const geo_bbox_t *bbox, int w, int h, double lat, double lon, float *x, float *y);

#ifdef __cplusplus
}
#endif
