#include "aircraft_data.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "flightwall_settings.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/http_client.h"
#include "lwip/pbuf.h"
#include "mbedtls/ssl.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define API_HOST "opendata.adsb.fi"
#define API_PORT 443
#define RESPONSE_CAPACITY (64u * 1024u)
#define FETCH_TIMEOUT_MS 35000u
#define SEEN_CAPACITY 256

typedef struct {
    char hex[7];
    uint32_t last_seen_s;
} seen_aircraft_t;

static flightwall_aircraft_t aircraft[FLIGHTWALL_MAX_AIRCRAFT];
static size_t aircraft_count;
static seen_aircraft_t seen_aircraft[SEEN_CAPACITY];
static size_t seen_count;
static char response[RESPONSE_CAPACITY + 1];
static volatile size_t response_length;
static volatile bool request_done;
static volatile bool request_overflow;
static volatile httpc_result_t request_result;
static volatile uint32_t request_http_status;
static uint32_t last_attempt_ms;
static uint32_t last_success_ms;
static bool has_success;
static char status_text[32] = "WAITING FOR DATA";
static struct altcp_tls_config *tls_config;
static altcp_allocator_t tls_allocator;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static struct altcp_pcb *tls_alloc_with_sni(void *arg, u8_t ip_type) {
    struct altcp_pcb *pcb = altcp_tls_alloc(arg, ip_type);
    if (!pcb) return NULL;
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
    if (!ssl || mbedtls_ssl_set_hostname(ssl, API_HOST) != 0) {
        altcp_abort(pcb);
        return NULL;
    }
    return pcb;
}

static err_t receive_body(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) return ERR_OK;
    if (err == ERR_OK && !request_overflow) {
        for (struct pbuf *part = p; part; part = part->next) {
            if (response_length + part->len > RESPONSE_CAPACITY) {
                request_overflow = true;
                break;
            }
            memcpy(response + response_length, part->payload, part->len);
            response_length += part->len;
        }
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return request_overflow ? ERR_BUF : ERR_OK;
}

static void request_finished(void *arg, httpc_result_t result, u32_t rx_content_len,
                             u32_t srv_res, err_t err) {
    (void)arg;
    (void)rx_content_len;
    (void)err;
    request_result = result;
    request_http_status = srv_res;
    request_done = true;
}

static err_t headers_received(httpc_state_t *connection, void *arg, struct pbuf *hdr,
                              u16_t hdr_len, u32_t content_len) {
    (void)connection;
    (void)arg;
    (void)hdr;
    (void)hdr_len;
    if (content_len != 0xffffffffu && content_len > RESPONSE_CAPACITY) {
        request_overflow = true;
        return ERR_BUF;
    }
    return ERR_OK;
}

static bool decode_chunked_response(void) {
    if (response_length == 0) return false;
    response[response_length] = '\0';
    if (response[0] == '{' || response[0] == '[') return true;

    size_t in = 0, out = 0;
    while (in < response_length) {
        char *line_end = NULL;
        for (size_t i = in; i + 1 < response_length; ++i) {
            if (response[i] == '\r' && response[i + 1] == '\n') {
                line_end = &response[i];
                break;
            }
        }
        if (!line_end) return false;
        char saved = *line_end;
        *line_end = '\0';
        char *endptr = NULL;
        unsigned long chunk = strtoul(&response[in], &endptr, 16);
        *line_end = saved;
        if (endptr == &response[in]) return false;
        in = (size_t)(line_end - response) + 2;
        if (chunk == 0) break;
        if (chunk > response_length - in || out + chunk > RESPONSE_CAPACITY) return false;
        memmove(response + out, response + in, chunk);
        out += chunk;
        in += chunk;
        if (in + 1 >= response_length || response[in] != '\r' || response[in + 1] != '\n') return false;
        in += 2;
    }
    response_length = out;
    response[out] = '\0';
    return out > 0 && response[0] == '{';
}

static const char *find_json_value(const char *object, size_t length, const char *key) {
    char needle[32];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return NULL;
    for (size_t i = 0; i + (size_t)n < length; ++i) {
        if (memcmp(object + i, needle, (size_t)n) != 0) continue;
        size_t p = i + (size_t)n;
        while (p < length && isspace((unsigned char)object[p])) p++;
        if (p >= length || object[p++] != ':') continue;
        while (p < length && isspace((unsigned char)object[p])) p++;
        return p < length ? object + p : NULL;
    }
    return NULL;
}

static bool json_string(const char *object, size_t length, const char *key,
                        char *output, size_t output_size) {
    const char *p = find_json_value(object, length, key);
    if (!p || *p != '\"' || output_size == 0) return false;
    p++;
    const char *limit = object + length;
    size_t out = 0;
    while (p < limit && *p != '\"') {
        char c = *p++;
        if (c == '\\' && p < limit) c = *p++;
        if (out + 1 < output_size) output[out++] = c;
    }
    output[out] = '\0';
    while (out > 0 && isspace((unsigned char)output[out - 1])) output[--out] = '\0';
    return p < limit;
}

static bool json_number(const char *object, size_t length, const char *key, float *value) {
    const char *p = find_json_value(object, length, key);
    if (!p || p >= object + length || *p == '\"' || !value) return false;
    char number[32];
    size_t n = 0;
    while (p + n < object + length && n + 1 < sizeof(number)) {
        char c = p[n];
        if (!(isdigit((unsigned char)c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')) break;
        number[n++] = c;
    }
    if (n == 0) return false;
    number[n] = '\0';
    char *end = NULL;
    *value = strtof(number, &end);
    return end && end != number;
}

static void uppercase(char *text) {
    for (; *text; ++text) *text = (char)toupper((unsigned char)*text);
}

static float haversine_nm(float lat1, float lon1, float lat2, float lon2) {
    const float radians = 0.01745329251994329577f;
    float dlat = (lat2 - lat1) * radians;
    float dlon = (lon2 - lon1) * radians;
    float a = sinf(dlat / 2) * sinf(dlat / 2) +
              cosf(lat1 * radians) * cosf(lat2 * radians) *
              sinf(dlon / 2) * sinf(dlon / 2);
    return 3440.065f * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

static bool parse_aircraft_object(const char *object, size_t length,
                                  flightwall_aircraft_t *plane) {
    memset(plane, 0, sizeof(*plane));
    plane->altitude_ft = INT32_MIN;
    json_string(object, length, "hex", plane->hex, sizeof(plane->hex));
    json_string(object, length, "r", plane->registration, sizeof(plane->registration));
    json_string(object, length, "t", plane->type, sizeof(plane->type));
    json_string(object, length, "squawk", plane->squawk, sizeof(plane->squawk));
    char flight[10] = {0};
    char emergency[16] = {0};
    json_string(object, length, "flight", flight, sizeof(flight));
    json_string(object, length, "emergency", emergency, sizeof(emergency));
    uppercase(plane->hex);
    uppercase(plane->registration);
    uppercase(plane->type);
    uppercase(flight);

    if (flight[0] && strcmp(flight, plane->registration) != 0)
        snprintf(plane->id, sizeof(plane->id), "%s", flight);
    else if (plane->registration[0])
        snprintf(plane->id, sizeof(plane->id), "%s", plane->registration);
    else if (plane->hex[0])
        snprintf(plane->id, sizeof(plane->id), "%s", plane->hex);
    else
        return false;

    const char *alt_value = find_json_value(object, length, "alt_baro");
    float number;
    if (alt_value && *alt_value == '\"') {
        char altitude[12] = {0};
        if (json_string(object, length, "alt_baro", altitude, sizeof(altitude)) &&
            strcmp(altitude, "ground") == 0) plane->altitude_ft = -1;
    } else if (json_number(object, length, "alt_baro", &number)) {
        plane->altitude_ft = (int32_t)lroundf(number);
    }
    if (json_number(object, length, "gs", &number) && number >= 0)
        plane->speed_kt = (uint16_t)fminf(999.0f, roundf(number));
    if (json_number(object, length, "track", &number))
        plane->track_deg = (uint16_t)(((int)lroundf(number) % 360 + 360) % 360);
    if (json_number(object, length, "dir", &number))
        plane->bearing_deg = (uint16_t)(((int)lroundf(number) % 360 + 360) % 360);
    if (json_number(object, length, "baro_rate", &number))
        plane->vertical_rate_fpm = (int16_t)fmaxf(-32768.0f, fminf(32767.0f, number));
    if (json_number(object, length, "dst", &number)) {
        plane->distance_nm = number;
    } else {
        float lat, lon;
        const flightwall_settings_t *settings = flightwall_settings_get();
        if (!json_number(object, length, "lat", &lat) || !json_number(object, length, "lon", &lon)) return false;
        plane->distance_nm = haversine_nm(settings->latitude, settings->longitude, lat, lon);
    }
    plane->emergency = strcmp(plane->squawk, "7500") == 0 ||
                       strcmp(plane->squawk, "7600") == 0 ||
                       strcmp(plane->squawk, "7700") == 0 ||
                       (emergency[0] && strcmp(emergency, "none") != 0);
    plane->watched = flightwall_settings_is_watched(plane->id, plane->registration, plane->hex);
    return true;
}

static int aircraft_compare(const void *a, const void *b) {
    const flightwall_aircraft_t *left = (const flightwall_aircraft_t *)a;
    const flightwall_aircraft_t *right = (const flightwall_aircraft_t *)b;
    if (left->emergency != right->emergency) return left->emergency ? -1 : 1;
    if (left->watched != right->watched) return left->watched ? -1 : 1;
    return (left->distance_nm > right->distance_nm) - (left->distance_nm < right->distance_nm);
}

static void record_seen(const char *hex) {
    if (!hex || !hex[0]) return;
    uint32_t seconds = now_ms() / 1000u;
    for (size_t i = 0; i < seen_count; ++i) {
        if (strcmp(seen_aircraft[i].hex, hex) == 0) {
            seen_aircraft[i].last_seen_s = seconds;
            return;
        }
    }
    if (seen_count < SEEN_CAPACITY) {
        snprintf(seen_aircraft[seen_count].hex, sizeof(seen_aircraft[seen_count].hex), "%s", hex);
        seen_aircraft[seen_count++].last_seen_s = seconds;
        return;
    }
    size_t oldest = 0;
    for (size_t i = 1; i < seen_count; ++i)
        if (seen_aircraft[i].last_seen_s < seen_aircraft[oldest].last_seen_s) oldest = i;
    snprintf(seen_aircraft[oldest].hex, sizeof(seen_aircraft[oldest].hex), "%s", hex);
    seen_aircraft[oldest].last_seen_s = seconds;
}

static bool parse_response(void) {
    const char *ac = strstr(response, "\"ac\"");
    if (!ac) return false;
    const char *array = strchr(ac, '[');
    if (!array) return false;
    aircraft_count = 0;
    bool in_string = false, escaped = false;
    int depth = 0;
    const char *start = NULL;
    for (const char *p = array + 1; p < response + response_length; ++p) {
        char c = *p;
        if (in_string) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '\"') in_string = false;
            continue;
        }
        if (c == '\"') { in_string = true; continue; }
        if (c == '{') {
            if (depth++ == 0) start = p;
        } else if (c == '}' && depth > 0) {
            if (--depth == 0 && start && aircraft_count < FLIGHTWALL_MAX_AIRCRAFT) {
                flightwall_aircraft_t candidate;
                if (parse_aircraft_object(start, (size_t)(p - start + 1), &candidate)) {
                    aircraft[aircraft_count++] = candidate;
                    record_seen(candidate.hex);
                }
                start = NULL;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    qsort(aircraft, aircraft_count, sizeof(aircraft[0]), aircraft_compare);
    return true;
}

void flightwall_aircraft_init(void) {
    if (tls_config) return;
    // This is a read-only public feed. SNI is enabled above; certificate
    // authentication remains optional because the Pico has no trusted clock
    // at boot with which to validate certificate dates.
    tls_config = altcp_tls_create_config_client(NULL, 0);
    tls_allocator.alloc = tls_alloc_with_sni;
    tls_allocator.arg = tls_config;
}

bool flightwall_aircraft_refresh(void) {
    flightwall_aircraft_init();
    if (!tls_config) {
        snprintf(status_text, sizeof(status_text), "TLS INIT FAILED");
        return false;
    }
    const flightwall_settings_t *settings = flightwall_settings_get();
    char uri[128];
    snprintf(uri, sizeof(uri), "/api/v3/lat/%.4f/lon/%.4f/dist/%u",
             settings->latitude, settings->longitude, settings->radius_nm);

    response_length = 0;
    request_done = false;
    request_overflow = false;
    request_result = HTTPC_RESULT_ERR_UNKNOWN;
    request_http_status = 0;
    last_attempt_ms = now_ms();
    snprintf(status_text, sizeof(status_text), "FETCHING");

    httpc_connection_t connection = {0};
    connection.altcp_allocator = &tls_allocator;
    connection.result_fn = request_finished;
    connection.headers_done_fn = headers_received;

    cyw43_arch_lwip_begin();
    err_t start_result = httpc_get_file_dns(API_HOST, API_PORT, uri, &connection,
                                            receive_body, NULL, NULL);
    cyw43_arch_lwip_end();
    if (start_result != ERR_OK) {
        snprintf(status_text, sizeof(status_text), "START ERROR %d", start_result);
        return false;
    }

    uint32_t started = now_ms();
    while (!request_done && now_ms() - started < FETCH_TIMEOUT_MS) sleep_ms(20);
    if (!request_done) {
        snprintf(status_text, sizeof(status_text), "API TIMEOUT");
        return false;
    }
    if (request_overflow) {
        snprintf(status_text, sizeof(status_text), "RESPONSE TOO LARGE");
        return false;
    }
    if (request_result != HTTPC_RESULT_OK || request_http_status != 200) {
        snprintf(status_text, sizeof(status_text), "HTTP %lu ERR %d",
                 (unsigned long)request_http_status, (int)request_result);
        return false;
    }
    if (!decode_chunked_response() || !parse_response()) {
        snprintf(status_text, sizeof(status_text), "JSON ERROR");
        return false;
    }
    has_success = true;
    last_success_ms = now_ms();
    snprintf(status_text, sizeof(status_text), "LIVE %u AIRCRAFT", (unsigned)aircraft_count);
    printf("adsb.fi refresh: %u aircraft, %u response bytes\n",
           (unsigned)aircraft_count, (unsigned)response_length);
    return true;
}

bool flightwall_aircraft_refresh_due(uint32_t refresh_seconds) {
    return now_ms() - last_attempt_ms >= refresh_seconds * 1000u;
}

const flightwall_aircraft_t *flightwall_aircraft_list(size_t *count) {
    if (count) *count = aircraft_count;
    return aircraft;
}

const flightwall_aircraft_t *flightwall_aircraft_nearest(void) {
    if (!aircraft_count) return NULL;
    const flightwall_aircraft_t *nearest = &aircraft[0];
    for (size_t i = 1; i < aircraft_count; ++i)
        if (aircraft[i].distance_nm < nearest->distance_nm) nearest = &aircraft[i];
    return nearest;
}

const flightwall_aircraft_t *flightwall_aircraft_emergency(void) {
    for (size_t i = 0; i < aircraft_count; ++i)
        if (aircraft[i].emergency) return &aircraft[i];
    return NULL;
}

bool flightwall_aircraft_has_success(void) { return has_success; }

bool flightwall_aircraft_is_stale(uint32_t refresh_seconds) {
    return has_success && now_ms() - last_success_ms > refresh_seconds * 2500u;
}

const char *flightwall_aircraft_status(void) { return status_text; }

static uint32_t unique_since(uint32_t age_seconds) {
    uint32_t now = now_ms() / 1000u, count = 0;
    for (size_t i = 0; i < seen_count; ++i)
        if (now - seen_aircraft[i].last_seen_s <= age_seconds) count++;
    return count;
}

uint32_t flightwall_aircraft_unique_hour(void) { return unique_since(3600); }
uint32_t flightwall_aircraft_unique_day(void) { return unique_since(86400); }
