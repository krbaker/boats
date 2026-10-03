#include "geo.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define EARTH_RADIUS_NM 3440.065
#define DEG2RAD(d) ((d) * M_PI / 180.0)
#define RAD2DEG(r) ((r) * 180.0 / M_PI)

double geo_distance_nm(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = DEG2RAD(lat2 - lat1);
    double dlon = DEG2RAD(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(DEG2RAD(lat1)) * cos(DEG2RAD(lat2)) * sin(dlon / 2) * sin(dlon / 2);
    return 2 * EARTH_RADIUS_NM * atan2(sqrt(a), sqrt(1 - a));
}

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = DEG2RAD(lat1), p2 = DEG2RAD(lat2);
    double dlon = DEG2RAD(lon2 - lon1);
    double y = sin(dlon) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dlon);
    double b = RAD2DEG(atan2(y, x));
    return fmod(b + 360.0, 360.0);
}

void geo_project(const geo_bbox_t *bbox, int w, int h, double lat, double lon, float *x, float *y)
{
    *x = (float)((lon - bbox->lon_min) / (bbox->lon_max - bbox->lon_min) * w);
    *y = (float)((bbox->lat_max - lat) / (bbox->lat_max - bbox->lat_min) * h);
}
