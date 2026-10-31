#include "update_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/http_client.h"
#include "lwip/pbuf.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "ota_server.h"
#include "flightwall_settings.h"
#include "update_public_key.h"

#define UPDATE_HOST "raw.githubusercontent.com"
#define UPDATE_PORT 443
#define MANIFEST_PATH "/skykingjwc/FlightWall/main/update/manifest.json"
#define SIGNATURE_PATH "/skykingjwc/FlightWall/main/update/manifest.sig"
#define REPOSITORY_PATH_PREFIX "/skykingjwc/FlightWall/"
#define SMALL_RESPONSE_CAPACITY 2048u
#define SIGNATURE_CAPACITY 96u
#define UPDATE_TIMEOUT_MS 60000u
#define UPDATE_INTERVAL_MS (6u * 60u * 60u * 1000u)
#define UPDATE_RETRY_MS (60u * 60u * 1000u)
#define MAX_UPDATE_SIZE (4u * 1024u * 1024u)

typedef enum {
    REQUEST_NONE,
    REQUEST_SMALL,
    REQUEST_FIRMWARE
} request_kind_t;

typedef struct {
    request_kind_t kind;
    uint8_t *buffer;
    size_t capacity;
    volatile size_t received;
    size_t expected;
    volatile bool done;
    volatile bool failed;
    volatile bool content_length_known;
    volatile httpc_result_t result;
    volatile uint32_t http_status;
    mbedtls_sha256_context sha;
    bool sha_active;
} update_request_t;

typedef struct {
    char version[16];
    char path[192];
    uint32_t build;
    uint32_t size;
    uint8_t sha256[32];
} update_manifest_t;

static struct altcp_tls_config *tls_config;
static altcp_allocator_t tls_allocator;
// lwIP retains a pointer to these settings for the lifetime of each request,
// so this must not be a stack-local object in start_get(). Requests are
// serialized by flightwall_update_check_now().
static httpc_connection_t http_connection;
static update_request_t request;
static uint8_t small_response[SMALL_RESPONSE_CAPACITY + 1];
static uint8_t signature[SIGNATURE_CAPACITY];
static uint32_t last_check_ms;
static bool last_check_succeeded;
static char update_status[48] = "NOT CHECKED";
static volatile bool check_requested;
static volatile bool install_requested;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static struct altcp_pcb *tls_alloc_with_sni(void *arg, u8_t ip_type) {
    struct altcp_pcb *pcb = altcp_tls_alloc(arg, ip_type);
    if (!pcb) return NULL;
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
    if (!ssl || mbedtls_ssl_set_hostname(ssl, UPDATE_HOST) != 0) {
        altcp_abort(pcb);
        return NULL;
    }
    return pcb;
}

static err_t receive_body(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) return ERR_OK;
    if (err == ERR_OK && !request.failed) {
        for (struct pbuf *part = p; part; part = part->next) {
            if (request.kind == REQUEST_SMALL) {
                if (request.received + part->len > request.capacity) {
                    request.failed = true;
                    break;
                }
                memcpy(request.buffer + request.received, part->payload, part->len);
            } else if (request.kind == REQUEST_FIRMWARE) {
                if (request.received + part->len > request.expected ||
                    !flightwall_ota_http_write((const uint8_t *)part->payload, part->len) ||
                    mbedtls_sha256_update(&request.sha,
                                          (const unsigned char *)part->payload,
                                          part->len) != 0) {
                    request.failed = true;
                    break;
                }
            }
            request.received += part->len;
        }
    }
    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return request.failed ? ERR_BUF : ERR_OK;
}

static void request_finished(void *arg, httpc_result_t result, u32_t rx_content_len,
                             u32_t srv_res, err_t err) {
    (void)arg;
    (void)rx_content_len;
    (void)err;
    request.result = result;
    request.http_status = srv_res;
    request.done = true;
}

static err_t headers_received(httpc_state_t *connection, void *arg, struct pbuf *hdr,
                              u16_t hdr_len, u32_t content_len) {
    (void)connection;
    (void)arg;
    (void)hdr;
    (void)hdr_len;
    request.content_length_known = content_len != 0xffffffffu;
    if (request.kind == REQUEST_SMALL) {
        if (request.content_length_known && content_len > request.capacity) {
            request.failed = true;
            return ERR_BUF;
        }
    } else if (request.kind == REQUEST_FIRMWARE) {
        if (!request.content_length_known || content_len != request.expected) {
            request.failed = true;
            return ERR_BUF;
        }
    }
    return ERR_OK;
}

static bool wait_for_request(uint32_t timeout_ms) {
    uint32_t started = now_ms();
    while (!request.done && now_ms() - started < timeout_ms) sleep_ms(20);
    return request.done && !request.failed &&
           request.result == HTTPC_RESULT_OK && request.http_status == 200;
}

static bool decode_chunked(uint8_t *data, size_t *length, size_t capacity) {
    size_t input = 0;
    size_t output = 0;
    while (input < *length) {
        size_t line_end = input;
        while (line_end + 1 < *length &&
               !(data[line_end] == '\r' && data[line_end + 1] == '\n')) {
            line_end++;
        }
        if (line_end + 1 >= *length) return false;
        char number[17];
        size_t number_length = line_end - input;
        if (number_length == 0 || number_length >= sizeof(number)) return false;
        memcpy(number, data + input, number_length);
        number[number_length] = '\0';
        char *extension = strchr(number, ';');
        if (extension) *extension = '\0';
        char *end = NULL;
        unsigned long chunk = strtoul(number, &end, 16);
        if (!end || end == number || *end != '\0') return false;
        input = line_end + 2;
        if (chunk == 0) break;
        if (chunk > *length - input || output + chunk > capacity) return false;
        memmove(data + output, data + input, chunk);
        output += chunk;
        input += chunk;
        if (input + 1 >= *length || data[input] != '\r' || data[input + 1] != '\n')
            return false;
        input += 2;
    }
    *length = output;
    return true;
}

static bool start_get(const char *path) {
    memset(&http_connection, 0, sizeof(http_connection));
    http_connection.altcp_allocator = &tls_allocator;
    http_connection.result_fn = request_finished;
    http_connection.headers_done_fn = headers_received;
    cyw43_arch_lwip_begin();
    err_t rc = httpc_get_file_dns(UPDATE_HOST, UPDATE_PORT, path, &http_connection,
                                  receive_body, NULL, NULL);
    cyw43_arch_lwip_end();
    return rc == ERR_OK;
}

static bool fetch_small(const char *path, uint8_t *output, size_t capacity,
                        size_t *output_length) {
    memset(&request, 0, sizeof(request));
    request.kind = REQUEST_SMALL;
    request.buffer = output;
    request.capacity = capacity;
    request.result = HTTPC_RESULT_ERR_UNKNOWN;
    if (!start_get(path) || !wait_for_request(UPDATE_TIMEOUT_MS)) return false;
    size_t length = request.received;
    if (!request.content_length_known && !decode_chunked(output, &length, capacity)) return false;
    *output_length = length;
    return true;
}

static bool verify_manifest_signature(const uint8_t *manifest, size_t manifest_length,
                                      const uint8_t *sig, size_t signature_length) {
    uint8_t digest[32];
    if (mbedtls_sha256(manifest, manifest_length, digest, 0) != 0) return false;
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    int rc = mbedtls_pk_parse_public_key(&key, flightwall_update_public_key_der,
                                         flightwall_update_public_key_der_size);
    if (rc == 0) {
        rc = mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                               sig, signature_length);
    }
    mbedtls_pk_free(&key);
    return rc == 0;
}

static const char *find_json_value(const char *json, size_t length, const char *key) {
    char needle[32];
    int needle_length = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (needle_length <= 0 || (size_t)needle_length >= sizeof(needle)) return NULL;
    for (size_t i = 0; i + (size_t)needle_length < length; ++i) {
        if (memcmp(json + i, needle, (size_t)needle_length) != 0) continue;
        size_t p = i + (size_t)needle_length;
        while (p < length && isspace((unsigned char)json[p])) p++;
        if (p >= length || json[p++] != ':') continue;
        while (p < length && isspace((unsigned char)json[p])) p++;
        return p < length ? json + p : NULL;
    }
    return NULL;
}

static bool json_string(const char *json, size_t length, const char *key,
                        char *output, size_t output_size) {
    const char *p = find_json_value(json, length, key);
    const char *limit = json + length;
    if (!p || p >= limit || *p++ != '"' || output_size == 0) return false;
    size_t used = 0;
    while (p < limit && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' || c < 0x20 || used + 1 >= output_size) return false;
        output[used++] = (char)c;
    }
    if (p >= limit || *p != '"') return false;
    output[used] = '\0';
    return true;
}

static bool json_u32(const char *json, size_t length, const char *key, uint32_t *value) {
    const char *p = find_json_value(json, length, key);
    if (!p || p >= json + length || !isdigit((unsigned char)*p)) return false;
    char number[16];
    size_t used = 0;
    while (p < json + length && isdigit((unsigned char)*p) && used + 1 < sizeof(number))
        number[used++] = *p++;
    number[used] = '\0';
    if (used == 0) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(number, &end, 10);
    if (!end || *end != '\0' || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool hex_digest(const char *hex, uint8_t output[32]) {
    if (!hex || strlen(hex) != 64) return false;
    for (size_t i = 0; i < 32; ++i) {
        char pair[3] = {hex[i * 2], hex[i * 2 + 1], '\0'};
        if (!isxdigit((unsigned char)pair[0]) || !isxdigit((unsigned char)pair[1])) return false;
        output[i] = (uint8_t)strtoul(pair, NULL, 16);
    }
    return true;
}

static bool parse_manifest(const uint8_t *data, size_t length, update_manifest_t *manifest) {
    char sha_text[65];
    uint32_t schema = 0;
    memset(manifest, 0, sizeof(*manifest));
    const char *json = (const char *)data;
    if (!json_u32(json, length, "schema", &schema) || schema != 1 ||
        !json_u32(json, length, "build", &manifest->build) ||
        !json_u32(json, length, "size", &manifest->size) ||
        !json_string(json, length, "version", manifest->version, sizeof(manifest->version)) ||
        !json_string(json, length, "path", manifest->path, sizeof(manifest->path)) ||
        !json_string(json, length, "sha256", sha_text, sizeof(sha_text)) ||
        !hex_digest(sha_text, manifest->sha256)) {
        return false;
    }
    return manifest->build > 0 && manifest->size >= 2048u &&
           manifest->size <= MAX_UPDATE_SIZE && (manifest->size % 2048u) == 0 &&
           manifest->version[0] &&
           strncmp(manifest->path, REPOSITORY_PATH_PREFIX,
                   strlen(REPOSITORY_PATH_PREFIX)) == 0 &&
           strstr(manifest->path, "..") == NULL;
}

static bool digest_equal(const uint8_t left[32], const uint8_t right[32]) {
    uint8_t difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

static bool download_update(const update_manifest_t *manifest) {
    if (!flightwall_ota_http_begin(manifest->size)) return false;
    memset(&request, 0, sizeof(request));
    request.kind = REQUEST_FIRMWARE;
    request.expected = manifest->size;
    request.result = HTTPC_RESULT_ERR_UNKNOWN;
    mbedtls_sha256_init(&request.sha);
    request.sha_active = mbedtls_sha256_starts(&request.sha, 0) == 0;
    if (!request.sha_active || !start_get(manifest->path) ||
        !wait_for_request(UPDATE_TIMEOUT_MS) || request.received != request.expected) {
        if (request.sha_active) mbedtls_sha256_free(&request.sha);
        flightwall_ota_http_abort();
        return false;
    }
    uint8_t actual_digest[32];
    bool hash_ok = mbedtls_sha256_finish(&request.sha, actual_digest) == 0 &&
                   digest_equal(actual_digest, manifest->sha256);
    mbedtls_sha256_free(&request.sha);
    request.sha_active = false;
    if (!hash_ok || !flightwall_ota_http_finish()) {
        flightwall_ota_http_abort();
        return false;
    }
    return true;
}

void flightwall_update_init(void) {
    // Do not allocate the GitHub TLS context here. Keeping a second TLS config
    // alive beside the ADS-B client was enough to destabilize the aircraft
    // connection when RSA support was compiled in. The updater creates its
    // context only for an active check/download and frees it immediately after.
    tls_allocator.alloc = tls_alloc_with_sni;
    tls_allocator.arg = NULL;
}

static bool update_tls_begin(void) {
    if (tls_config) return true;
    tls_config = altcp_tls_create_config_client(NULL, 0);
    if (!tls_config) return false;
    tls_allocator.arg = tls_config;
    return true;
}

static void update_tls_end(void) {
    if (!tls_config) return;
    altcp_tls_free_config(tls_config);
    tls_config = NULL;
    tls_allocator.arg = NULL;
}

static bool check_now_inner(bool force_install) {
    last_check_ms = now_ms();
    last_check_succeeded = false;
    flightwall_update_init();
    if (!update_tls_begin()) {
        snprintf(update_status, sizeof(update_status), "TLS INIT FAILED");
        return false;
    }

    size_t manifest_length = 0;
    size_t signature_length = 0;
    snprintf(update_status, sizeof(update_status), "CHECKING GITHUB");
    if (!fetch_small(MANIFEST_PATH, small_response, SMALL_RESPONSE_CAPACITY,
                     &manifest_length) || manifest_length == 0) {
        snprintf(update_status, sizeof(update_status), "MANIFEST FETCH FAILED");
        return false;
    }
    if (!fetch_small(SIGNATURE_PATH, signature, SIGNATURE_CAPACITY,
                     &signature_length) || signature_length == 0) {
        snprintf(update_status, sizeof(update_status), "SIGNATURE FETCH FAILED");
        return false;
    }
    if (!verify_manifest_signature(small_response, manifest_length,
                                   signature, signature_length)) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE SIGNATURE");
        return false;
    }

    update_manifest_t manifest;
    if (!parse_manifest(small_response, manifest_length, &manifest)) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE MANIFEST");
        return false;
    }
    last_check_succeeded = true;
    if (manifest.build <= FLIGHTWALL_BUILD_NUMBER) {
        snprintf(update_status, sizeof(update_status), "CURRENT %s", FLIGHTWALL_VERSION);
        return false;
    }

    printf("FlightWall update %s (build %lu, %lu bytes) available\n",
           manifest.version, (unsigned long)manifest.build, (unsigned long)manifest.size);
    if (!force_install && !flightwall_settings_get()->auto_install_updates) {
        snprintf(update_status, sizeof(update_status), "AVAILABLE %s", manifest.version);
        return false;
    }
    snprintf(update_status, sizeof(update_status), "DOWNLOADING %s", manifest.version);
    if (!download_update(&manifest)) {
        last_check_succeeded = false;
        snprintf(update_status, sizeof(update_status), "UPDATE DOWNLOAD FAILED");
        return false;
    }
    snprintf(update_status, sizeof(update_status), "VERIFIED %s", manifest.version);
    return true;
}

static bool check_now(bool force_install) {
    bool result = check_now_inner(force_install);
    update_tls_end();
    return result;
}

bool flightwall_update_check_now(void) {
    return check_now(false);
}

bool flightwall_update_check_due(void) {
    uint32_t interval = last_check_succeeded ? UPDATE_INTERVAL_MS : UPDATE_RETRY_MS;
    return now_ms() - last_check_ms >= interval && flightwall_update_check_now();
}

void flightwall_update_request_check(void) {
    check_requested = true;
    snprintf(update_status, sizeof(update_status), "CHECK REQUESTED");
}

void flightwall_update_request_install(void) {
    install_requested = true;
    snprintf(update_status, sizeof(update_status), "INSTALL REQUESTED");
}

bool flightwall_update_process_requests(void) {
    if (install_requested) {
        install_requested = false;
        check_requested = false;
        return check_now(true);
    }
    if (check_requested) {
        check_requested = false;
        return check_now(false);
    }
    return false;
}

const char *flightwall_update_status(void) {
    return update_status;
}
