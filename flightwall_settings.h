#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Callsigns, registrations, and ICAO hex identifiers are all accepted.
#define FLIGHTWALL_MAX_WATCHED 12
#define FLIGHTWALL_WATCH_ID_SIZE 10

typedef struct {
    float latitude;
    float longitude;
    uint16_t radius_nm;
    uint8_t brightness_percent;
    uint8_t panel_output_basis;
    uint16_t refresh_seconds;
    uint8_t display_seconds;
    uint8_t watched_count;
    char watched[FLIGHTWALL_MAX_WATCHED][FLIGHTWALL_WATCH_ID_SIZE];
} flightwall_settings_t;

// Load persistent settings, falling back to safe DeWitt defaults if the
// settings sector is erased or invalid.
void flightwall_settings_load(void);
const flightwall_settings_t *flightwall_settings_get(void);
uint32_t flightwall_settings_generation(void);

// Validate, persist, and publish a complete settings update.
bool flightwall_settings_update(float latitude, float longitude,
                                unsigned radius_nm, unsigned brightness_percent,
                                unsigned panel_output_basis,
                                unsigned refresh_seconds, unsigned display_seconds,
                                const char *watchlist);

bool flightwall_settings_is_watched(const char *id, const char *registration,
                                    const char *hex);
size_t flightwall_watchlist_format(char *buffer, size_t buffer_size);

#ifdef __cplusplus
}
#endif
