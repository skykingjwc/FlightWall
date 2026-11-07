#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FLIGHTWALL_AIRLINE_ICON_WIDTH 50u
#define FLIGHTWALL_AIRLINE_ICON_HEIGHT 22u

void flightwall_airline_assets_init(void);
void flightwall_airline_assets_process(void);
bool flightwall_airline_assets_network_busy(void);
bool flightwall_airline_assets_valid(void);
const char *flightwall_airline_assets_status(void);

bool flightwall_airline_assets_get(const char *callsign,
                                   const uint8_t **pixels,
                                   uint8_t *width,
                                   uint8_t *height);

#ifdef __cplusplus
}
#endif
