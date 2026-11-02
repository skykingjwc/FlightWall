#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Confirm a Try-Before-You-Buy image early in boot. This keeps a known-good
// OTA image and allows the RP2350 ROM to roll back a bad one automatically.
void flightwall_ota_confirm_running_image(void);

// Starts the verified UF2 receiver on TCP port 4242.
bool flightwall_ota_start(void);

// Streaming HTTP upload interface. The web server passes raw UF2 bytes here
// after the browser removes the partition-table block at the start of the
// file. No whole-image allocation is required.
bool flightwall_ota_http_begin(size_t content_length);
bool flightwall_ota_http_write(const uint8_t *data, size_t length);
bool flightwall_ota_http_finish(void);
void flightwall_ota_http_abort(void);

// Last detailed OTA staging error. This survives transfer reset long enough for
// the update client to report the failing operation, ROM return code, address,
// and UF2 block number.
const char *flightwall_ota_last_error(void);

// Boot/update diagnostic retained across the reboot attempt. A rollback-capable
// image can expose this after the ROM falls back to the previous partition.
const char *flightwall_ota_boot_diagnostic(void);

// Reboot the currently running known-good image after a staging failure. This
// does not request a FLASH_UPDATE boot into the partially written partition.
void flightwall_ota_reboot_current(void);

// Returns true after a complete image has arrived. The caller should blank or
// annotate the display and then call flightwall_ota_reboot_to_update().
bool flightwall_ota_update_ready(void);

// True while a manual TCP upload or HTTP-backed OTA write is actively using
// the firmware staging path. GitHub background checks must stay out of the way.
bool flightwall_ota_transfer_active(void);

void flightwall_ota_reboot_to_update(void);

#ifdef __cplusplus
}
#endif
