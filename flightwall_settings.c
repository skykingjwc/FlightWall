#include "flightwall_settings.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/bootrom.h"
#include "pico/flash.h"

#define SETTINGS_MAGIC 0x46575331u  // "FWS1"
#define SETTINGS_VERSION 5u
#define SETTINGS_FLASH_OFFSET (PICO_FLASH_BANK_STORAGE_OFFSET + FLASH_SECTOR_SIZE)

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    float latitude;
    float longitude;
    uint16_t radius_nm;
    uint8_t brightness_percent;
    uint8_t reserved0;
    uint16_t refresh_seconds;
    uint16_t reserved1;
    uint32_t checksum;
} stored_settings_v1_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    float latitude;
    float longitude;
    uint16_t radius_nm;
    uint8_t brightness_percent;
    uint8_t watched_count;
    uint16_t refresh_seconds;
    uint16_t reserved1;
    char watched[FLIGHTWALL_MAX_WATCHED][FLIGHTWALL_WATCH_ID_SIZE];
    uint32_t checksum;
} stored_settings_v2_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    float latitude;
    float longitude;
    uint16_t radius_nm;
    uint8_t brightness_percent;
    uint8_t panel_output_basis;
    uint16_t refresh_seconds;
    uint8_t watched_count;
    uint8_t reserved0;
    char watched[FLIGHTWALL_MAX_WATCHED][FLIGHTWALL_WATCH_ID_SIZE];
    uint32_t checksum;
} stored_settings_v3_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    float latitude;
    float longitude;
    uint16_t radius_nm;
    uint8_t brightness_percent;
    uint8_t panel_output_basis;
    uint16_t refresh_seconds;
    uint8_t display_seconds;
    uint8_t watched_count;
    char watched[FLIGHTWALL_MAX_WATCHED][FLIGHTWALL_WATCH_ID_SIZE];
    uint32_t checksum;
} stored_settings_v4_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    float latitude;
    float longitude;
    uint16_t radius_nm;
    uint8_t brightness_percent;
    uint8_t panel_output_basis;
    uint16_t refresh_seconds;
    uint8_t display_seconds;
    uint8_t auto_install_updates;
    uint8_t watched_count;
    uint8_t reserved0;
    char watched[FLIGHTWALL_MAX_WATCHED][FLIGHTWALL_WATCH_ID_SIZE];
    uint32_t checksum;
} stored_settings_v5_t;

static flightwall_settings_t current_settings;
static volatile uint32_t settings_generation;

static uint32_t fnv1a(const void *data, size_t size) {
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool values_valid(float latitude, float longitude, unsigned radius_nm,
                         unsigned brightness, unsigned refresh_seconds) {
    return latitude >= -90.0f && latitude <= 90.0f &&
           longitude >= -180.0f && longitude <= 180.0f &&
           radius_nm >= 1 && radius_nm <= 250 &&
           brightness >= 1 && brightness <= 100 &&
           refresh_seconds >= 10 && refresh_seconds <= 300;
}

static bool panel_output_valid(unsigned basis) {
    return basis == 8 || basis == 10 || basis == 12 || basis == 16 ||
           basis == 24 || basis == 32 || basis == 48 || basis == 64;
}

static bool display_seconds_valid(unsigned seconds) {
    return seconds >= 3 && seconds <= 30;
}

static void load_defaults(void) {
    memset(&current_settings, 0, sizeof(current_settings));
    // Public, non-residential default: Capital Region International Airport
    // (KLAN). The user can replace this location on the setup page.
    current_settings.latitude = 42.7787f;
    current_settings.longitude = -84.5874f;
    current_settings.radius_nm = 50;
    current_settings.brightness_percent = 75;
    current_settings.panel_output_basis = 8;
    current_settings.refresh_seconds = 30;
    current_settings.display_seconds = 6;
    current_settings.auto_install_updates = true;
}

static bool parse_watchlist(const char *text, flightwall_settings_t *settings) {
    settings->watched_count = 0;
    memset(settings->watched, 0, sizeof(settings->watched));
    if (!text) return true;

    char token[FLIGHTWALL_WATCH_ID_SIZE] = {0};
    size_t length = 0;
    for (;;) {
        unsigned char c = (unsigned char)*text++;
        bool separator = c == '\0' || c == ',' || c == ';' || isspace(c);
        if (!separator) {
            if (!(isalnum(c) || c == '-') || length + 1 >= sizeof(token)) return false;
            token[length++] = (char)toupper(c);
            continue;
        }

        if (length) {
            token[length] = '\0';
            bool duplicate = false;
            for (uint8_t i = 0; i < settings->watched_count; ++i) {
                if (strcmp(settings->watched[i], token) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                if (settings->watched_count >= FLIGHTWALL_MAX_WATCHED) return false;
                snprintf(settings->watched[settings->watched_count], FLIGHTWALL_WATCH_ID_SIZE,
                         "%s", token);
                settings->watched_count++;
            }
            length = 0;
            token[0] = '\0';
        }
        if (c == '\0') break;
    }
    return true;
}

static bool persist_settings(const flightwall_settings_t *settings) {
    uint8_t sector[FLASH_SECTOR_SIZE] = {0};
    stored_settings_v5_t *stored = (stored_settings_v5_t *)sector;
    stored->magic = SETTINGS_MAGIC;
    stored->version = SETTINGS_VERSION;
    stored->size = sizeof(*stored);
    stored->latitude = settings->latitude;
    stored->longitude = settings->longitude;
    stored->radius_nm = settings->radius_nm;
    stored->brightness_percent = settings->brightness_percent;
    stored->panel_output_basis = settings->panel_output_basis;
    stored->refresh_seconds = settings->refresh_seconds;
    stored->display_seconds = settings->display_seconds;
    stored->auto_install_updates = settings->auto_install_updates ? 1u : 0u;
    stored->watched_count = settings->watched_count;
    memcpy(stored->watched, settings->watched, sizeof(stored->watched));
    stored->checksum = fnv1a(stored, offsetof(stored_settings_v5_t, checksum));

    cflash_flags_t erase_flags = {
        .flags =
            (CFLASH_OP_VALUE_ERASE << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    int rc = rom_flash_op(erase_flags, XIP_BASE + SETTINGS_FLASH_OFFSET,
                          FLASH_SECTOR_SIZE, NULL);
    if (rc != BOOTROM_OK) return false;

    cflash_flags_t program_flags = {
        .flags =
            (CFLASH_OP_VALUE_PROGRAM << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    rc = rom_flash_op(program_flags, XIP_BASE + SETTINGS_FLASH_OFFSET,
                      sizeof(sector), sector);
    return rc == BOOTROM_OK;
}

void flightwall_settings_load(void) {
    uint8_t sector[FLASH_SECTOR_SIZE];
    cflash_flags_t flags = {
        .flags =
            (CFLASH_OP_VALUE_READ << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    int rc = rom_flash_op(flags, XIP_BASE + SETTINGS_FLASH_OFFSET,
                          sizeof(sector), sector);
    load_defaults();
    bool valid = false;

    if (rc == BOOTROM_OK) {
        const stored_settings_v5_t *v5 = (const stored_settings_v5_t *)sector;
        if (v5->magic == SETTINGS_MAGIC && v5->version == SETTINGS_VERSION &&
            v5->size == sizeof(*v5) && v5->watched_count <= FLIGHTWALL_MAX_WATCHED &&
            v5->auto_install_updates <= 1 &&
            v5->checksum == fnv1a(v5, offsetof(stored_settings_v5_t, checksum)) &&
            values_valid(v5->latitude, v5->longitude, v5->radius_nm,
                         v5->brightness_percent, v5->refresh_seconds) &&
            panel_output_valid(v5->panel_output_basis) &&
            display_seconds_valid(v5->display_seconds)) {
            current_settings.latitude = v5->latitude;
            current_settings.longitude = v5->longitude;
            current_settings.radius_nm = v5->radius_nm;
            current_settings.brightness_percent = v5->brightness_percent;
            current_settings.panel_output_basis = v5->panel_output_basis;
            current_settings.refresh_seconds = v5->refresh_seconds;
            current_settings.display_seconds = v5->display_seconds;
            current_settings.auto_install_updates = v5->auto_install_updates != 0;
            current_settings.watched_count = v5->watched_count;
            memcpy(current_settings.watched, v5->watched, sizeof(v5->watched));
            for (uint8_t i = 0; i < current_settings.watched_count; ++i)
                current_settings.watched[i][FLIGHTWALL_WATCH_ID_SIZE - 1] = 0;
            valid = true;
            printf("loaded FlightWall settings v5\n");
        }

        const stored_settings_v1_t *v1 = (const stored_settings_v1_t *)sector;
        if (v1->magic == SETTINGS_MAGIC && v1->version == 1 &&
            v1->size == sizeof(*v1) &&
            v1->checksum == fnv1a(v1, offsetof(stored_settings_v1_t, checksum)) &&
            values_valid(v1->latitude, v1->longitude, v1->radius_nm,
                         v1->brightness_percent, v1->refresh_seconds)) {
            current_settings.latitude = v1->latitude;
            current_settings.longitude = v1->longitude;
            current_settings.radius_nm = v1->radius_nm;
            current_settings.brightness_percent = v1->brightness_percent;
            current_settings.refresh_seconds = v1->refresh_seconds;
            valid = true;
            printf("loaded and migrated FlightWall v1 settings\n");
        } else {
            const stored_settings_v2_t *v2 = (const stored_settings_v2_t *)sector;
            if (v2->magic == SETTINGS_MAGIC && v2->version == 2 &&
                v2->size == sizeof(*v2) && v2->watched_count <= FLIGHTWALL_MAX_WATCHED &&
                v2->checksum == fnv1a(v2, offsetof(stored_settings_v2_t, checksum)) &&
                values_valid(v2->latitude, v2->longitude, v2->radius_nm,
                             v2->brightness_percent, v2->refresh_seconds)) {
                current_settings.latitude = v2->latitude;
                current_settings.longitude = v2->longitude;
                current_settings.radius_nm = v2->radius_nm;
                current_settings.brightness_percent = v2->brightness_percent;
                current_settings.refresh_seconds = v2->refresh_seconds;
                current_settings.watched_count = v2->watched_count;
                memcpy(current_settings.watched, v2->watched, sizeof(v2->watched));
                for (uint8_t i = 0; i < current_settings.watched_count; ++i)
                    current_settings.watched[i][FLIGHTWALL_WATCH_ID_SIZE - 1] = '\0';
                valid = true;
                printf("loaded and migrated FlightWall v2 settings\n");
            } else {
                const stored_settings_v3_t *v3 = (const stored_settings_v3_t *)sector;
                if (v3->magic == SETTINGS_MAGIC && v3->version == 3 &&
                    v3->size == sizeof(*v3) && v3->watched_count <= FLIGHTWALL_MAX_WATCHED &&
                    v3->checksum == fnv1a(v3, offsetof(stored_settings_v3_t, checksum)) &&
                    values_valid(v3->latitude, v3->longitude, v3->radius_nm,
                                 v3->brightness_percent, v3->refresh_seconds) &&
                    panel_output_valid(v3->panel_output_basis)) {
                    current_settings.latitude = v3->latitude;
                    current_settings.longitude = v3->longitude;
                    current_settings.radius_nm = v3->radius_nm;
                    current_settings.brightness_percent = v3->brightness_percent;
                    current_settings.panel_output_basis = v3->panel_output_basis;
                    current_settings.refresh_seconds = v3->refresh_seconds;
                    current_settings.watched_count = v3->watched_count;
                    memcpy(current_settings.watched, v3->watched, sizeof(v3->watched));
                    for (uint8_t i = 0; i < current_settings.watched_count; ++i)
                        current_settings.watched[i][FLIGHTWALL_WATCH_ID_SIZE - 1] = '\0';
                    valid = true;
                    printf("loaded and migrated FlightWall v3 settings\n");
                } else {
                    const stored_settings_v4_t *v4 = (const stored_settings_v4_t *)sector;
                    if (v4->magic == SETTINGS_MAGIC && v4->version == 4 &&
                        v4->size == sizeof(*v4) && v4->watched_count <= FLIGHTWALL_MAX_WATCHED &&
                        v4->checksum == fnv1a(v4, offsetof(stored_settings_v4_t, checksum)) &&
                        values_valid(v4->latitude, v4->longitude, v4->radius_nm,
                                     v4->brightness_percent, v4->refresh_seconds) &&
                        panel_output_valid(v4->panel_output_basis) &&
                        display_seconds_valid(v4->display_seconds)) {
                        current_settings.latitude = v4->latitude;
                        current_settings.longitude = v4->longitude;
                        current_settings.radius_nm = v4->radius_nm;
                        current_settings.brightness_percent = v4->brightness_percent;
                        current_settings.panel_output_basis = v4->panel_output_basis;
                        current_settings.refresh_seconds = v4->refresh_seconds;
                        current_settings.display_seconds = v4->display_seconds;
                        current_settings.auto_install_updates = true;
                        current_settings.watched_count = v4->watched_count;
                        memcpy(current_settings.watched, v4->watched, sizeof(v4->watched));
                        for (uint8_t i = 0; i < current_settings.watched_count; ++i)
                            current_settings.watched[i][FLIGHTWALL_WATCH_ID_SIZE - 1] = '\0';
                        valid = true;
                        printf("loaded and migrated FlightWall v4 settings\n");
                    }
                }
            }
        }
    }
    if (!valid) printf("using default FlightWall settings\n");
    settings_generation++;
}

const flightwall_settings_t *flightwall_settings_get(void) {
    return &current_settings;
}

uint32_t flightwall_settings_generation(void) {
    return settings_generation;
}

bool flightwall_settings_update(float latitude, float longitude,
                                unsigned radius_nm, unsigned brightness,
                                unsigned panel_output_basis,
                                unsigned refresh_seconds, unsigned display_seconds,
                                const char *watchlist) {
    if (!values_valid(latitude, longitude, radius_nm, brightness, refresh_seconds) ||
        !panel_output_valid(panel_output_basis) ||
        !display_seconds_valid(display_seconds)) {
        return false;
    }

    flightwall_settings_t updated = current_settings;
    updated.latitude = latitude;
    updated.longitude = longitude;
    updated.radius_nm = (uint16_t)radius_nm;
    updated.brightness_percent = (uint8_t)brightness;
    updated.panel_output_basis = (uint8_t)panel_output_basis;
    updated.refresh_seconds = (uint16_t)refresh_seconds;
    updated.display_seconds = (uint8_t)display_seconds;
    if (!parse_watchlist(watchlist, &updated) || !persist_settings(&updated)) return false;

    current_settings = updated;
    settings_generation++;
    return true;
}

bool flightwall_settings_set_auto_install_updates(bool enabled) {
    if (current_settings.auto_install_updates == enabled) return true;
    flightwall_settings_t updated = current_settings;
    updated.auto_install_updates = enabled;
    if (!persist_settings(&updated)) return false;
    current_settings = updated;
    settings_generation++;
    return true;
}

static bool identifier_equal(const char *left, const char *right) {
    if (!left || !right || !left[0] || !right[0]) return false;
    while (*left && *right) {
        if (toupper((unsigned char)*left++) != toupper((unsigned char)*right++)) return false;
    }
    return *left == '\0' && *right == '\0';
}

bool flightwall_settings_is_watched(const char *id, const char *registration,
                                    const char *hex) {
    for (uint8_t i = 0; i < current_settings.watched_count; ++i) {
        const char *watch = current_settings.watched[i];
        if (identifier_equal(watch, id) || identifier_equal(watch, registration) ||
            identifier_equal(watch, hex)) return true;
    }
    return false;
}

size_t flightwall_watchlist_format(char *buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return 0;
    size_t used = 0;
    buffer[0] = '\0';
    for (uint8_t i = 0; i < current_settings.watched_count; ++i) {
        int written = snprintf(buffer + used, buffer_size - used, "%s%s",
                               i ? ", " : "", current_settings.watched[i]);
        if (written < 0 || (size_t)written >= buffer_size - used) {
            buffer[buffer_size - 1] = '\0';
            return buffer_size - 1;
        }
        used += (size_t)written;
    }
    return used;
}
