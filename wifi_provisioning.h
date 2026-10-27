#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*flightwall_wifi_status_cb)(const char *line1, const char *line2, const char *line3);

// Connect using saved credentials, or run the FlightWall-Setup captive portal
// until working credentials are supplied. Returns 0 when STA is online.
int flightwall_wifi_connect_or_provision(flightwall_wifi_status_cb status_cb);

// IPv4 address assigned to the station interface. Valid after a successful
// call to flightwall_wifi_connect_or_provision().
const char *flightwall_wifi_ip_address(void);

// Controlled by the local web interface. The display loop reads this flag so
// the test exercises the same emergency renderer used for live aircraft.
bool flightwall_emergency_test_active(void);
bool flightwall_load_test_active(void);
void flightwall_load_test_complete(void);

#ifdef __cplusplus
}
#endif
