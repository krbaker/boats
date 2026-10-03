// Geodesy helpers: distance/bearing and chart projection.
//
// Pure C (no ESP-IDF dependencies) so it can be unit-tested on the host.

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Lat/lon bounds of a chart image. The map builder renders charts in
// EPSG:4326, so pixels are linear in latitude and longitude.
typedef struct {
    double lat_min, lat_max;
    double lon_min, lon_max;
} geo_bbox_t;

// Great-circle distance in nautical miles.
double geo_distance_nm(double lat1, double lon1, double lat2, double lon2);

// Initial true bearing from point 1 to point 2, 0..360 degrees.
double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2);

// Project lat/lon onto a w x h image covering bbox. (0,0) is top-left.
void geo_project(const geo_bbox_t *bbox, int w, int h, double lat, double lon, float *x, float *y);

#ifdef __cplusplus
}
#endif
