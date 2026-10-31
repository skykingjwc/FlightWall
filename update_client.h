#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*flightwall_update_download_callback_t)(const char *version);

void flightwall_update_init(void);
void flightwall_update_set_download_callback(flightwall_update_download_callback_t callback);

// Checks the signed GitHub update feed immediately. Returns true only when a
// newer image was downloaded, authenticated, and staged in the inactive slot.
bool flightwall_update_check_now(void);

// Performs the same check when the six-hour interval (or one-hour retry
// interval after a failure) has elapsed.
bool flightwall_update_check_due(void);

// Web requests only set flags; the display/main loop performs network work.
void flightwall_update_request_check(void);
void flightwall_update_request_install(void);
bool flightwall_update_process_requests(void);
bool flightwall_update_installing(void);

const char *flightwall_update_status(void);

#ifdef __cplusplus
}
#endif
