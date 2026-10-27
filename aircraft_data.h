#ifndef FLIGHTWALL_AIRCRAFT_DATA_H
#define FLIGHTWALL_AIRCRAFT_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLIGHTWALL_MAX_AIRCRAFT 64

typedef struct {
    char hex[7];
    char id[10];
    char registration[9];
    char type[6];
    char squawk[5];
    int32_t altitude_ft;       // INT32_MIN means unavailable; -1 means ground
    uint16_t speed_kt;
    uint16_t track_deg;
    uint16_t bearing_deg;
    int16_t vertical_rate_fpm;
    float distance_nm;
    bool emergency;
    bool watched;
} flightwall_aircraft_t;

void flightwall_aircraft_init(void);
bool flightwall_aircraft_refresh(void);
bool flightwall_aircraft_refresh_due(uint32_t refresh_seconds);
const flightwall_aircraft_t *flightwall_aircraft_list(size_t *count);
const flightwall_aircraft_t *flightwall_aircraft_nearest(void);
const flightwall_aircraft_t *flightwall_aircraft_emergency(void);
bool flightwall_aircraft_has_success(void);
bool flightwall_aircraft_is_stale(uint32_t refresh_seconds);
const char *flightwall_aircraft_status(void);
uint32_t flightwall_aircraft_unique_hour(void);
uint32_t flightwall_aircraft_unique_day(void);

#ifdef __cplusplus
}
#endif

#endif
