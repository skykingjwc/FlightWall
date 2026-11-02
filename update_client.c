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
#define CHECK_PATH "/skykingjwc/FlightWall/main/update/check.json"
#define FIRMWARE_PATH "/skykingjwc/FlightWall/main/update/flightwall.uf2"
#define SMALL_RESPONSE_CAPACITY 4096u
#define SIGNATURE_CAPACITY 96u
#define UPDATE_TIMEOUT_MS 60000u
#define UPDATE_INTERVAL_MS (6u * 60u * 60u * 1000u)
#define UPDATE_RETRY_MS (60u * 60u * 1000u)
#define MAX_UPDATE_SIZE (4u * 1024u * 1024u)
#define FIRMWARE_QUEUE_CAPACITY (64u * 1024u)

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
    size_t firmware_received;
    size_t flashed_received;
    volatile uint32_t queue_head;
    volatile uint32_t queue_tail;
    volatile uint32_t tcp_credit_pending;
    struct altcp_pcb *firmware_pcb;
    char failure_reason[32];
} update_request_t;

typedef struct {
    char version[16];
    char blob_sha[41];
    uint32_t schema;
    uint32_t build;
    uint32_t size;
    uint8_t sha256[32];
} update_manifest_t;

static struct altcp_tls_config *tls_config;
static altcp_allocator_t tls_allocator;
static const char *tls_sni_host = UPDATE_HOST;
// lwIP retains a pointer to these settings for the lifetime of each request,
// so this must not be a stack-local object in start_get(). Requests are
// serialized by flightwall_update_check_now().
static httpc_connection_t http_connection;
static update_request_t request;
static uint8_t small_response[SMALL_RESPONSE_CAPACITY + 1];
static uint8_t feed_bytes[SMALL_RESPONSE_CAPACITY + 1];
static uint8_t manifest_bytes[2048];
static uint8_t signature[SIGNATURE_CAPACITY];
static uint8_t firmware_queue[FIRMWARE_QUEUE_CAPACITY];
static uint32_t last_check_ms;
static bool last_check_succeeded;
static char update_status[48] = "NOT CHECKED";
static volatile bool check_requested;
static volatile bool install_requested;
static flightwall_update_download_callback_t download_callback;

typedef enum {
    UPDATE_STATE_IDLE,
    UPDATE_STATE_FEED_PENDING,
    UPDATE_STATE_FIRMWARE_PENDING,
    UPDATE_STATE_READY_TO_REBOOT,
    UPDATE_STATE_RECOVERY_REBOOT
} update_state_t;

static update_state_t update_state = UPDATE_STATE_IDLE;
static bool pending_force_install;
static uint64_t recovery_reboot_at_us;
static update_manifest_t pending_manifest;

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static struct altcp_pcb *tls_alloc_with_sni(void *arg, u8_t ip_type) {
    struct altcp_pcb *pcb = altcp_tls_alloc(arg, ip_type);
    if (!pcb) return NULL;
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
    if (!ssl || !tls_sni_host || mbedtls_ssl_set_hostname(ssl, tls_sni_host) != 0) {
        altcp_abort(pcb);
        return NULL;
    }
    return pcb;
}


static void request_fail(const char *reason) {
    request.failed = true;
    if (reason && reason[0] && request.failure_reason[0] == '\0')
        snprintf(request.failure_reason, sizeof(request.failure_reason), "%s", reason);
}

static uint32_t firmware_queue_used(void) {
    return request.queue_head - request.queue_tail;
}

static uint32_t firmware_queue_free(void) {
    return FIRMWARE_QUEUE_CAPACITY - firmware_queue_used();
}

static bool firmware_queue_bytes(const uint8_t *data, size_t length) {
    if (request.firmware_received + length > request.expected) {
        request_fail("FIRMWARE SIZE");
        return false;
    }
    if (length > firmware_queue_free()) {
        request_fail("RX QUEUE FULL");
        return false;
    }

    for (size_t i = 0; i < length; ++i) {
        firmware_queue[request.queue_head % FIRMWARE_QUEUE_CAPACITY] = data[i];
        request.queue_head++;
    }
    request.firmware_received += length;
    return true;
}

static err_t receive_body(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) return ERR_OK;

    const uint16_t received_now = p->tot_len;

    if (err == ERR_OK && !request.failed) {
        // Firmware data is copied into a RAM queue only. Flash erase/program
        // operations must never run from this lwIP callback context.
        if (request.kind == REQUEST_FIRMWARE &&
            firmware_queue_free() < received_now) {
            request_fail("RX QUEUE FULL");
        } else {
            for (struct pbuf *part = p; part; part = part->next) {
                if (request.kind == REQUEST_SMALL) {
                    if (request.received + part->len > request.capacity) {
                        request.failed = true;
                        break;
                    }
                    memcpy(request.buffer + request.received, part->payload, part->len);
                } else if (request.kind == REQUEST_FIRMWARE) {
                    if (!firmware_queue_bytes((const uint8_t *)part->payload, part->len)) {
                        break;
                    }
                }
                request.received += part->len;
            }
        }
    }

    if (request.kind == REQUEST_FIRMWARE && !request.failed) {
        // Withhold TCP receive-window credit until the main loop has drained
        // queued firmware bytes to flash. This applies natural backpressure to
        // GitHub instead of allowing the callback to outrun available RAM.
        request.firmware_pcb = pcb;
        request.tcp_credit_pending += received_now;
    } else {
        altcp_recved(pcb, received_now);
    }

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
    request.firmware_pcb = NULL;
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
        if (request.content_length_known && content_len != request.expected) {
            request_fail("CONTENT LENGTH");
            return ERR_BUF;
        }
    }
    return ERR_OK;
}

static const char *find_json_value(const char *json, size_t length, const char *key);

static bool wait_for_request(uint32_t timeout_ms) {
    uint32_t started = now_ms();
    while (!request.done && now_ms() - started < timeout_ms) sleep_ms(20);
    return request.done && !request.failed &&
           request.result == HTTPC_RESULT_OK && request.http_status == 200;
}

static bool wait_for_http_response(uint32_t timeout_ms) {
    uint32_t started = now_ms();
    while (!request.done && now_ms() - started < timeout_ms) sleep_ms(20);
    return request.done && !request.failed &&
           request.result == HTTPC_RESULT_OK && request.http_status != 0;
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

static bool start_get(const char *host, const char *path) {
    tls_sni_host = host;
    memset(&http_connection, 0, sizeof(http_connection));
    http_connection.altcp_allocator = &tls_allocator;
    http_connection.result_fn = request_finished;
    http_connection.headers_done_fn = headers_received;
    cyw43_arch_lwip_begin();
    err_t rc = httpc_get_file_dns(host, UPDATE_PORT, path, &http_connection,
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
    if (!start_get(UPDATE_HOST, path) || !wait_for_request(UPDATE_TIMEOUT_MS)) return false;
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

static bool hex_bytes(const char *hex, size_t hex_length,
                      uint8_t *output, size_t output_capacity, size_t *output_length) {
    if (!hex || (hex_length % 2) != 0 || hex_length / 2 > output_capacity) return false;
    size_t used = 0;
    for (size_t i = 0; i < hex_length; i += 2) {
        char pair[3] = {hex[i], hex[i + 1], '\0'};
        if (!isxdigit((unsigned char)pair[0]) || !isxdigit((unsigned char)pair[1])) return false;
        output[used++] = (uint8_t)strtoul(pair, NULL, 16);
    }
    *output_length = used;
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
    memset(manifest, 0, sizeof(*manifest));
    const char *json = (const char *)data;

    if (!json_u32(json, length, "schema", &manifest->schema) ||
        (manifest->schema != 2 && manifest->schema != 3) ||
        !json_u32(json, length, "build", &manifest->build) ||
        !json_u32(json, length, "size", &manifest->size) ||
        !json_string(json, length, "version", manifest->version, sizeof(manifest->version)) ||
        !json_string(json, length, "sha256", sha_text, sizeof(sha_text)) ||
        !hex_digest(sha_text, manifest->sha256)) {
        return false;
    }

    if (manifest->schema == 2) {
        if (!json_string(json, length, "blob_sha",
                         manifest->blob_sha, sizeof(manifest->blob_sha)) ||
            strlen(manifest->blob_sha) != 40) {
            return false;
        }
        for (size_t i = 0; i < 40; ++i)
            if (!isxdigit((unsigned char)manifest->blob_sha[i])) return false;
    }

    return manifest->build > 0 && manifest->size >= 2048u &&
           manifest->size <= MAX_UPDATE_SIZE && (manifest->size % 2048u) == 0 &&
           manifest->version[0];
}

static bool digest_equal(const uint8_t left[32], const uint8_t right[32]) {
    uint8_t difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}

static bool update_tls_begin(void);
static void update_tls_end(void);

static bool request_ok(void) {
    return request.done && !request.failed &&
           request.result == HTTPC_RESULT_OK && request.http_status == 200;
}

static void finish_update_session(void) {
    update_tls_end();
    update_state = UPDATE_STATE_IDLE;
}

static bool start_feed_check(bool force_install) {
    if (update_state != UPDATE_STATE_IDLE || flightwall_ota_transfer_active()) return false;

    last_check_ms = now_ms();
    last_check_succeeded = false;
    pending_force_install = force_install;
    flightwall_update_init();

    if (!update_tls_begin()) {
        snprintf(update_status, sizeof(update_status), "TLS INIT FAILED");
        return false;
    }

    memset(&request, 0, sizeof(request));
    request.kind = REQUEST_SMALL;
    request.buffer = small_response;
    request.capacity = SMALL_RESPONSE_CAPACITY;
    request.result = HTTPC_RESULT_ERR_UNKNOWN;

    snprintf(update_status, sizeof(update_status), "CHECKING GITHUB API");
    if (!start_get(UPDATE_HOST, CHECK_PATH)) {
        snprintf(update_status, sizeof(update_status), "UPDATE FEED START FAILED");
        update_tls_end();
        return false;
    }

    update_state = UPDATE_STATE_FEED_PENDING;
    return true;
}

static bool start_firmware_download(const update_manifest_t *manifest) {
    if (!flightwall_ota_http_begin(manifest->size)) return false;

    pending_manifest = *manifest;
    memset(&request, 0, sizeof(request));
    request.kind = REQUEST_FIRMWARE;
    request.expected = manifest->size;
    request.result = HTTPC_RESULT_ERR_UNKNOWN;
    mbedtls_sha256_init(&request.sha);
    request.sha_active = mbedtls_sha256_starts(&request.sha, 0) == 0;

    if (!request.sha_active || !start_get(UPDATE_HOST, FIRMWARE_PATH)) {
        if (request.sha_active) {
            mbedtls_sha256_free(&request.sha);
            request.sha_active = false;
        }
        flightwall_ota_http_abort();
        return false;
    }

    snprintf(update_status, sizeof(update_status), "DOWNLOADING %s", manifest->version);
    if (download_callback) download_callback(manifest->version);
    update_state = UPDATE_STATE_FIRMWARE_PENDING;
    return true;
}

static bool process_feed_result(void) {
    if (!request.done) return false;

    if (!request_ok()) {
        snprintf(update_status, sizeof(update_status), "FEED HTTP %lu ERR %d",
                 (unsigned long)request.http_status, (int)request.result);
        finish_update_session();
        return false;
    }

    size_t response_length = request.received;
    if (!request.content_length_known &&
        !decode_chunked(small_response, &response_length, SMALL_RESPONSE_CAPACITY)) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE RESPONSE");
        finish_update_session();
        return false;
    }

    if (response_length == 0 || response_length > SMALL_RESPONSE_CAPACITY) {
        snprintf(update_status, sizeof(update_status), "EMPTY UPDATE FEED");
        finish_update_session();
        return false;
    }
    memcpy(feed_bytes, small_response, response_length);
    const size_t envelope_length = response_length;
    const char *envelope = (const char *)feed_bytes;
    char signature_hex[2 * SIGNATURE_CAPACITY + 1];
    char sha_text[65];

    update_manifest_t manifest;
    if (!parse_manifest(feed_bytes, envelope_length, &manifest) ||
        !json_string(envelope, envelope_length, "sha256", sha_text, sizeof(sha_text)) ||
        !json_string(envelope, envelope_length, "signature_hex",
                     signature_hex, sizeof(signature_hex))) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE FEED");
        finish_update_session();
        return false;
    }

    int manifest_length;
    if (manifest.schema == 2) {
        manifest_length = snprintf((char *)manifest_bytes, sizeof(manifest_bytes),
            "{\"blob_sha\":\"%s\",\"build\":%lu,\"schema\":2,"
            "\"sha256\":\"%s\",\"size\":%lu,\"version\":\"%s\"}\n",
            manifest.blob_sha,
            (unsigned long)manifest.build,
            sha_text,
            (unsigned long)manifest.size,
            manifest.version);
    } else {
        manifest_length = snprintf((char *)manifest_bytes, sizeof(manifest_bytes),
            "{\"build\":%lu,\"schema\":3,"
            "\"sha256\":\"%s\",\"size\":%lu,\"version\":\"%s\"}\n",
            (unsigned long)manifest.build,
            sha_text,
            (unsigned long)manifest.size,
            manifest.version);
    }
    if (manifest_length <= 0 || (size_t)manifest_length >= sizeof(manifest_bytes)) {
        snprintf(update_status, sizeof(update_status), "BAD MANIFEST REBUILD");
        finish_update_session();
        return false;
    }

    size_t signature_length = 0;
    if (!hex_bytes(signature_hex, strlen(signature_hex),
                   signature, SIGNATURE_CAPACITY, &signature_length) ||
        signature_length == 0) {
        snprintf(update_status, sizeof(update_status), "BAD SIGNATURE FORMAT");
        finish_update_session();
        return false;
    }

    if (!verify_manifest_signature(manifest_bytes, (size_t)manifest_length,
                                   signature, signature_length)) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE SIGNATURE");
        finish_update_session();
        return false;
    }

    last_check_succeeded = true;
    if (manifest.build <= FLIGHTWALL_BUILD_NUMBER) {
        snprintf(update_status, sizeof(update_status), "CURRENT %s", FLIGHTWALL_VERSION);
        finish_update_session();
        return false;
    }

    printf("FlightWall update %s (build %lu, %lu bytes) available\n",
           manifest.version, (unsigned long)manifest.build, (unsigned long)manifest.size);

    if (!pending_force_install && !flightwall_settings_get()->auto_install_updates) {
        snprintf(update_status, sizeof(update_status), "AVAILABLE %s", manifest.version);
        finish_update_session();
        return false;
    }

    if (!start_firmware_download(&manifest)) {
        last_check_succeeded = false;
        snprintf(update_status, sizeof(update_status), "UPDATE DOWNLOAD START FAILED");
        finish_update_session();
        return false;
    }

    return false;
}

static bool drain_firmware_queue(void) {
    if (request.kind != REQUEST_FIRMWARE || request.failed) return false;

    // Keep each main-loop flash operation bounded. The OTA writer internally
    // processes complete 2 KiB UF2 groups and may briefly lock out core 1.
    size_t budget = 2048u;
    while (budget > 0 && request.queue_tail != request.queue_head) {
        uint32_t used = firmware_queue_used();
        size_t contiguous = FIRMWARE_QUEUE_CAPACITY -
                            (request.queue_tail % FIRMWARE_QUEUE_CAPACITY);
        size_t take = used < contiguous ? used : contiguous;
        if (take > budget) take = budget;

        const uint8_t *data =
            &firmware_queue[request.queue_tail % FIRMWARE_QUEUE_CAPACITY];
        if (!flightwall_ota_http_write(data, take)) {
            request_fail("OTA WRITE");
            return false;
        }
        if (mbedtls_sha256_update(&request.sha, data, take) != 0) {
            request_fail("SHA UPDATE");
            return false;
        }

        request.queue_tail += (uint32_t)take;
        request.flashed_received += take;
        budget -= take;

        if (request.expected > 0) {
            unsigned long percent =
                (unsigned long)((request.flashed_received * 100u) / request.expected);
            if (percent > 100u) percent = 100u;
            snprintf(update_status, sizeof(update_status), "DOWNLOADING %s %lu%%",
                     pending_manifest.version, percent);
        }
    }

    // Return receive-window credit only after queued data has been consumed.
    // Take the CYW43/lwIP lock because this runs from the display/main loop.
    if (firmware_queue_used() <= FIRMWARE_QUEUE_CAPACITY / 2u &&
        request.tcp_credit_pending != 0) {
        cyw43_arch_lwip_begin();
        if (request.firmware_pcb && !request.done) {
            uint32_t credit = request.tcp_credit_pending;
            request.tcp_credit_pending = 0;
            altcp_recved(request.firmware_pcb, credit);
        }
        cyw43_arch_lwip_end();
    }

    return true;
}

static bool process_firmware_result(void) {
    if (!drain_firmware_queue()) {
        const char *flash_detail = flightwall_ota_last_error();
        const char *reason = request.failure_reason[0] ? request.failure_reason : "UNKNOWN";
        printf("FlightWall update staging failure: %s; flash=%s\n",
               reason, flash_detail ? flash_detail : "none");
        flightwall_ota_http_abort();
        last_check_succeeded = false;
        update_tls_end();

        if (strcmp(reason, "OTA WRITE") == 0 &&
            flash_detail && strcmp(flash_detail, "none") != 0) {
            snprintf(update_status, sizeof(update_status), "FLASH FAILED - REBOOTING");
        } else {
            snprintf(update_status, sizeof(update_status), "%s - REBOOTING", reason);
        }

        recovery_reboot_at_us = time_us_64() + 2000000u;
        update_state = UPDATE_STATE_RECOVERY_REBOOT;
        return false;
    }

    if (!request.done || request.queue_tail != request.queue_head) return false;

    request.firmware_pcb = NULL;
    request.tcp_credit_pending = 0;

    bool transfer_ok = request_ok() &&
                       request.firmware_received == request.expected &&
                       request.flashed_received == request.expected;

    uint8_t actual_digest[32];
    bool hash_ok = false;
    if (request.sha_active) {
        hash_ok = transfer_ok &&
                  mbedtls_sha256_finish(&request.sha, actual_digest) == 0 &&
                  digest_equal(actual_digest, pending_manifest.sha256);
        mbedtls_sha256_free(&request.sha);
        request.sha_active = false;
    }

    if (!transfer_ok || !hash_ok || !flightwall_ota_http_finish()) {
        flightwall_ota_http_abort();
        last_check_succeeded = false;
        snprintf(update_status, sizeof(update_status), "UPDATE DOWNLOAD FAILED");
        finish_update_session();
        return false;
    }

    snprintf(update_status, sizeof(update_status), "VERIFIED %s", pending_manifest.version);
    update_tls_end();
    update_state = UPDATE_STATE_READY_TO_REBOOT;
    return true;
}

void flightwall_update_set_download_callback(flightwall_update_download_callback_t callback) {
    download_callback = callback;
}

void flightwall_update_init(void) {
    tls_allocator.alloc = tls_alloc_with_sni;
    tls_allocator.arg = NULL;
    const char *boot_diag = flightwall_ota_boot_diagnostic();
    if (boot_diag && strcmp(boot_diag, "none") != 0)
        snprintf(update_status, sizeof(update_status), "%s", boot_diag);
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

bool flightwall_update_check_now(void) {
    return start_feed_check(false);
}

bool flightwall_update_check_due(void) {
    if (update_state != UPDATE_STATE_IDLE) return false;
    uint32_t interval = last_check_succeeded ? UPDATE_INTERVAL_MS : UPDATE_RETRY_MS;
    if (now_ms() - last_check_ms < interval) return false;
    (void)start_feed_check(false);
    return false;
}

void flightwall_update_request_check(void) {
    if (update_state == UPDATE_STATE_IDLE) {
        check_requested = true;
        snprintf(update_status, sizeof(update_status), "CHECK REQUESTED");
    }
}

void flightwall_update_request_install(void) {
    if (update_state == UPDATE_STATE_IDLE) {
        install_requested = true;
        snprintf(update_status, sizeof(update_status), "INSTALL REQUESTED");
    }
}

bool flightwall_update_process_requests(void) {
    if (update_state == UPDATE_STATE_FEED_PENDING)
        return process_feed_result();

    if (update_state == UPDATE_STATE_FIRMWARE_PENDING)
        return process_firmware_result();

    if (update_state == UPDATE_STATE_READY_TO_REBOOT)
        return false;

    if (update_state == UPDATE_STATE_RECOVERY_REBOOT) {
        if (time_us_64() >= recovery_reboot_at_us)
            flightwall_ota_reboot_current();
        return false;
    }

    if (install_requested) {
        install_requested = false;
        check_requested = false;
        (void)start_feed_check(true);
        return false;
    }

    if (check_requested) {
        check_requested = false;
        (void)start_feed_check(false);
        return false;
    }

    return false;
}

bool flightwall_update_installing(void) {
    return update_state == UPDATE_STATE_FIRMWARE_PENDING ||
           update_state == UPDATE_STATE_READY_TO_REBOOT ||
           update_state == UPDATE_STATE_RECOVERY_REBOOT;
}

bool flightwall_update_network_busy(void) {
    return update_state == UPDATE_STATE_FEED_PENDING ||
           update_state == UPDATE_STATE_FIRMWARE_PENDING;
}

const char *flightwall_update_status(void) {
    return update_status;
}

