#include "update_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/http_client.h"
#include "lwip/pbuf.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "ota_server.h"
#include "flightwall_settings.h"

#define UPDATE_API_HOST "api.github.com"
#define UPDATE_PORT 443
#define CHECK_API_PATH "/repos/skykingjwc/FlightWall/contents/update/check.json?ref=main"
#define BLOB_API_PREFIX "/repos/skykingjwc/FlightWall/git/blobs/"
#define SMALL_RESPONSE_CAPACITY 4096u
#define UPDATE_TIMEOUT_MS 60000u
#define UPDATE_INTERVAL_MS (6u * 60u * 60u * 1000u)
#define UPDATE_RETRY_MS (60u * 60u * 1000u)
#define MAX_UPDATE_SIZE (4u * 1024u * 1024u)
#define FIRMWARE_QUEUE_CAPACITY (64u * 1024u)
#define MAX_FIRMWARE_CHUNKS 48u

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
    size_t decoded_received;
    size_t flashed_received;
    volatile uint32_t queue_head;
    volatile uint32_t queue_tail;
    volatile uint32_t tcp_credit_pending;
    struct altcp_pcb *firmware_pcb;
    bool blob_content_found;
    bool blob_content_done;
    bool blob_escape;
    uint8_t blob_scan_state;
    uint8_t blob_key_match;
    char b64_quartet[4];
    uint8_t b64_count;
    bool firmware_chunked;
    uint32_t chunk_remaining;
    uint32_t chunk_size;
    uint8_t chunk_state;
    bool chunk_extension;
    char failure_reason[32];
} update_request_t;

typedef struct {
    char version[16];
    uint32_t schema;
    uint32_t build;
    uint32_t size;
    uint32_t chunk_size;
    uint32_t chunk_count;
    char chunk_shas[MAX_FIRMWARE_CHUNKS][41];
    uint8_t sha256[32];
} update_manifest_t;

static struct altcp_tls_config *tls_config;
static altcp_allocator_t tls_allocator;
static const char *tls_sni_host = UPDATE_API_HOST;
// lwIP retains a pointer to these settings for the lifetime of each request,
// so this must not be a stack-local object in start_get(). Requests are
// serialized by flightwall_update_check_now().
static httpc_connection_t http_connection;
static update_request_t request;
static uint8_t small_response[SMALL_RESPONSE_CAPACITY + 1];
static uint8_t feed_bytes[SMALL_RESPONSE_CAPACITY + 1];
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
static update_manifest_t verified_manifest;
static bool verified_manifest_available;
static uint32_t current_chunk_index;

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


static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    if (c == '=') return -2;
    return -1;
}

static bool decode_base64_quartet(const char input[4], uint8_t output[3], size_t *output_length) {
    int v0 = base64_value((unsigned char)input[0]);
    int v1 = base64_value((unsigned char)input[1]);
    int v2 = base64_value((unsigned char)input[2]);
    int v3 = base64_value((unsigned char)input[3]);
    if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1) return false;
    output[0] = (uint8_t)((v0 << 2) | (v1 >> 4));
    if (v2 == -2) {
        if (v3 != -2) return false;
        *output_length = 1;
        return true;
    }
    output[1] = (uint8_t)(((v1 & 0x0f) << 4) | (v2 >> 2));
    if (v3 == -2) {
        *output_length = 2;
        return true;
    }
    output[2] = (uint8_t)(((v2 & 0x03) << 6) | v3);
    *output_length = 3;
    return true;
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

static bool firmware_queue_decoded(const uint8_t *data, size_t length) {
    if (request.decoded_received + length > request.expected) {
        request_fail("DECODE SIZE");
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
    request.decoded_received += length;
    return true;
}

static bool firmware_blob_feed(const uint8_t *data, size_t length) {
    static const char content_key[] = "\"content\"";
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = data[i];

        if (request.blob_content_done) continue;

        if (!request.blob_content_found) {
            if (request.blob_scan_state == 0) {
                if (ch == (unsigned char)content_key[request.blob_key_match]) {
                    request.blob_key_match++;
                    if (request.blob_key_match == sizeof(content_key) - 1) {
                        request.blob_scan_state = 1;
                        request.blob_key_match = 0;
                    }
                } else {
                    request.blob_key_match = (ch == (unsigned char)content_key[0]) ? 1 : 0;
                }
            } else if (request.blob_scan_state == 1) {
                if (isspace(ch)) continue;
                if (ch != ':') return false;
                request.blob_scan_state = 2;
            } else {
                if (isspace(ch)) continue;
                if (ch != '"') return false;
                request.blob_content_found = true;
                request.blob_scan_state = 0;
            }
            continue;
        }

        if (request.blob_escape) {
            request.blob_escape = false;
            if (ch == 'n' || ch == 'r' || ch == 't') continue;
            return false;
        }
        if (ch == '\\') {
            request.blob_escape = true;
            continue;
        }
        if (ch == '"') {
            if (request.b64_count != 0) return false;
            request.blob_content_done = true;
            continue;
        }
        if (isspace(ch)) continue;

        if (base64_value(ch) == -1) return false;
        request.b64_quartet[request.b64_count++] = (char)ch;
        if (request.b64_count == 4) {
            uint8_t decoded[3];
            size_t decoded_length = 0;
            if (!decode_base64_quartet(request.b64_quartet, decoded, &decoded_length) ||
                !firmware_queue_decoded(decoded, decoded_length)) {
                return false;
            }
            request.b64_count = 0;
        }
    }
    return true;
}


enum {
    FW_CHUNK_SIZE = 0,
    FW_CHUNK_SIZE_LF,
    FW_CHUNK_DATA,
    FW_CHUNK_DATA_CR,
    FW_CHUNK_DATA_LF,
    FW_CHUNK_TRAILER
};

static int chunk_hex_value(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

// lwIP's httpc body callback includes HTTP chunk framing when Content-Length
// is unknown. Strip that framing before feeding GitHub's JSON/base64 body to
// the firmware decoder. Without this, hexadecimal chunk-size text is valid
// base64 and silently corrupts the UF2 stream.
static bool firmware_http_feed(const uint8_t *data, size_t length) {
    if (!request.firmware_chunked) return firmware_blob_feed(data, length);

    size_t i = 0;
    while (i < length) {
        unsigned char ch = data[i];

        switch (request.chunk_state) {
            case FW_CHUNK_SIZE: {
                if (ch == '\r') {
                    request.chunk_state = FW_CHUNK_SIZE_LF;
                    i++;
                    break;
                }
                if (request.chunk_extension) {
                    i++;
                    break;
                }
                if (ch == ';') {
                    request.chunk_extension = true;
                    i++;
                    break;
                }
                int value = chunk_hex_value(ch);
                if (value < 0 || request.chunk_size > 0x0fffffffu) return false;
                request.chunk_size = (request.chunk_size << 4) | (uint32_t)value;
                i++;
                break;
            }

            case FW_CHUNK_SIZE_LF:
                if (ch != '\n') return false;
                i++;
                if (request.chunk_size == 0) {
                    request.chunk_state = FW_CHUNK_TRAILER;
                } else {
                    request.chunk_remaining = request.chunk_size;
                    request.chunk_state = FW_CHUNK_DATA;
                }
                break;

            case FW_CHUNK_DATA: {
                size_t available = length - i;
                size_t take = request.chunk_remaining < available
                    ? request.chunk_remaining : available;
                if (take && !firmware_blob_feed(data + i, take)) return false;
                i += take;
                request.chunk_remaining -= (uint32_t)take;
                if (request.chunk_remaining == 0)
                    request.chunk_state = FW_CHUNK_DATA_CR;
                break;
            }

            case FW_CHUNK_DATA_CR:
                if (ch != '\r') return false;
                request.chunk_state = FW_CHUNK_DATA_LF;
                i++;
                break;

            case FW_CHUNK_DATA_LF:
                if (ch != '\n') return false;
                request.chunk_state = FW_CHUNK_SIZE;
                request.chunk_size = 0;
                request.chunk_extension = false;
                i++;
                break;

            case FW_CHUNK_TRAILER:
                // The zero-sized chunk comes after the JSON body. Nothing in
                // the HTTP trailers belongs to the firmware payload.
                i = length;
                break;

            default:
                return false;
        }
    }
    return true;
}

static err_t receive_body(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) return ERR_OK;

    const uint16_t received_now = p->tot_len;

    if (err == ERR_OK && !request.failed) {
        // Firmware data is decoded into a RAM queue only. Flash erase/program
        // operations must never run from this lwIP callback context.
        //
        // Important: do not compare the encoded HTTP/JSON/base64 byte count
        // against decoded queue capacity. Only firmware_queue_decoded() knows
        // how many actual firmware bytes a packet produces.
        for (struct pbuf *part = p; part; part = part->next) {
            if (request.kind == REQUEST_SMALL) {
                if (request.received + part->len > request.capacity) {
                    request.failed = true;
                    break;
                }
                memcpy(request.buffer + request.received, part->payload, part->len);
            } else if (request.kind == REQUEST_FIRMWARE) {
                if (!firmware_http_feed((const uint8_t *)part->payload, part->len)) {
                    request_fail("HTTP/BLOB DECODE");
                    break;
                }
            }
            request.received += part->len;
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
    return ERR_OK;
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
        // GitHub's blob API returns JSON/base64, so HTTP Content-Length is the
        // encoded envelope size rather than the decoded UF2 size. When there
        // is no Content-Length, lwIP supplies the HTTP chunk framing to the
        // body callback, so strip it incrementally before JSON/base64 decode.
        request.firmware_chunked = !request.content_length_known;
        request.chunk_state = FW_CHUNK_SIZE;
        request.chunk_remaining = 0;
        request.chunk_size = 0;
        request.chunk_extension = false;
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
    if (!start_get(UPDATE_API_HOST, path) || !wait_for_request(UPDATE_TIMEOUT_MS)) return false;
    size_t length = request.received;
    if (!request.content_length_known && !decode_chunked(output, &length, capacity)) return false;
    *output_length = length;
    return true;
}

static bool decode_base64_json_field(const uint8_t *json, size_t length, const char *key,
                                     uint8_t *output, size_t capacity, size_t *output_length) {
    const char *p = find_json_value((const char *)json, length, key);
    const char *limit = (const char *)json + length;
    if (!p || p >= limit || *p++ != '"') return false;

    char quartet[4];
    uint8_t quartet_count = 0;
    size_t used = 0;
    bool escape = false;

    while (p < limit) {
        unsigned char ch = (unsigned char)*p++;
        if (escape) {
            escape = false;
            if (ch == 'n' || ch == 'r' || ch == 't') continue;
            return false;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') break;
        if (isspace(ch)) continue;
        if (base64_value(ch) == -1) return false;
        quartet[quartet_count++] = (char)ch;
        if (quartet_count == 4) {
            uint8_t decoded[3];
            size_t decoded_length = 0;
            if (!decode_base64_quartet(quartet, decoded, &decoded_length) ||
                used + decoded_length > capacity) {
                return false;
            }
            memcpy(output + used, decoded, decoded_length);
            used += decoded_length;
            quartet_count = 0;
        }
    }

    if (quartet_count != 0) return false;
    *output_length = used;
    return true;
}

static bool fetch_api_file(const char *path, uint8_t *output, size_t capacity,
                           size_t *output_length) {
    size_t response_length = 0;
    if (!fetch_small(path, small_response, SMALL_RESPONSE_CAPACITY, &response_length))
        return false;
    return decode_base64_json_field(small_response, response_length, "content",
                                    output, capacity, output_length);
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

static bool parse_chunk_shas(const char *json, size_t length,
                             update_manifest_t *manifest) {
    char chunks[MAX_FIRMWARE_CHUNKS * 41u];
    if (!json_string(json, length, "chunks", chunks, sizeof(chunks))) return false;

    uint32_t count = 0;
    char *cursor = chunks;
    while (*cursor) {
        if (count >= MAX_FIRMWARE_CHUNKS) return false;
        char *comma = strchr(cursor, ',');
        size_t item_length = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (item_length != 40) return false;
        for (size_t i = 0; i < 40; ++i)
            if (!isxdigit((unsigned char)cursor[i])) return false;
        memcpy(manifest->chunk_shas[count], cursor, 40);
        manifest->chunk_shas[count][40] = '\0';
        count++;
        if (!comma) break;
        cursor = comma + 1;
    }

    manifest->chunk_count = count;
    return count > 0;
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
        manifest->schema != 4 ||
        !json_u32(json, length, "build", &manifest->build) ||
        !json_u32(json, length, "size", &manifest->size) ||
        !json_u32(json, length, "chunk_size", &manifest->chunk_size) ||
        !json_string(json, length, "version", manifest->version, sizeof(manifest->version)) ||
        !json_string(json, length, "sha256", sha_text, sizeof(sha_text)) ||
        !hex_digest(sha_text, manifest->sha256) ||
        !parse_chunk_shas(json, length, manifest)) {
        return false;
    }

    uint32_t expected_chunks =
        (manifest->size + manifest->chunk_size - 1u) / manifest->chunk_size;

    return manifest->build > 0 &&
           manifest->size >= 2048u &&
           manifest->size <= MAX_UPDATE_SIZE &&
           (manifest->size % 2048u) == 0 &&
           manifest->chunk_size > 0 &&
           manifest->chunk_size <= FIRMWARE_QUEUE_CAPACITY &&
           manifest->chunk_count == expected_chunks &&
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
    verified_manifest_available = false;
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
    if (!start_get(UPDATE_API_HOST, CHECK_API_PATH)) {
        snprintf(update_status, sizeof(update_status), "UPDATE FEED START FAILED");
        update_tls_end();
        return false;
    }

    update_state = UPDATE_STATE_FEED_PENDING;
    return true;
}

static void reset_chunk_request_state(void) {
    request.received = 0;
    request.done = false;
    request.failed = false;
    request.content_length_known = false;
    request.result = HTTPC_RESULT_ERR_UNKNOWN;
    request.http_status = 0;
    request.tcp_credit_pending = 0;
    request.firmware_pcb = NULL;
    request.blob_content_found = false;
    request.blob_content_done = false;
    request.blob_escape = false;
    request.blob_scan_state = 0;
    request.blob_key_match = 0;
    request.b64_count = 0;
    request.firmware_chunked = false;
    request.chunk_remaining = 0;
    request.chunk_size = 0;
    request.chunk_state = FW_CHUNK_SIZE;
    request.chunk_extension = false;
    request.failure_reason[0] = '\0';
}

static bool start_chunk_download(void) {
    if (current_chunk_index >= pending_manifest.chunk_count) return false;

    char blob_path[160];
    if (snprintf(blob_path, sizeof(blob_path), "%s%s", BLOB_API_PREFIX,
                 pending_manifest.chunk_shas[current_chunk_index]) <= 0) {
        return false;
    }

    reset_chunk_request_state();
    printf("OTA chunk %lu/%lu starting\n",
           (unsigned long)(current_chunk_index + 1u),
           (unsigned long)pending_manifest.chunk_count);
    return start_get(UPDATE_API_HOST, blob_path);
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
    current_chunk_index = 0;

    if (!request.sha_active || !start_chunk_download()) {
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

    size_t envelope_length = 0;
    if (!decode_base64_json_field(small_response, response_length, "content",
                                  feed_bytes, SMALL_RESPONSE_CAPACITY, &envelope_length) ||
        envelope_length == 0) {
        snprintf(update_status, sizeof(update_status), "UPDATE FEED DECODE FAILED");
        finish_update_session();
        return false;
    }

    update_manifest_t manifest;
    if (!parse_manifest(feed_bytes, envelope_length, &manifest)) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE FEED");
        finish_update_session();
        return false;
    }

    last_check_succeeded = true;
    if (manifest.build <= FLIGHTWALL_BUILD_NUMBER) {
        verified_manifest_available = false;
        snprintf(update_status, sizeof(update_status), "CURRENT %s", FLIGHTWALL_VERSION);
        finish_update_session();
        return false;
    }

    printf("FlightWall update %s (build %lu, %lu bytes) available\n",
           manifest.version, (unsigned long)manifest.build, (unsigned long)manifest.size);

    // Cache the already-parsed update metadata so a manual Install click can
    // go straight to the firmware blob without repeating the feed request.
    verified_manifest = manifest;
    verified_manifest_available = true;

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
    if (firmware_queue_used() <= (FIRMWARE_QUEUE_CAPACITY * 3u) / 4u &&
        request.tcp_credit_pending != 0) {
        cyw43_arch_lwip_begin();
        if (request.firmware_pcb && !request.done) {
            // altcp_recved()/tcp_recved() accept a 16-bit length. Keep the
            // accounting in 32 bits, but return it to lwIP in bounded chunks
            // so credit can never truncate at 64 KiB and stall the sender.
            while (request.tcp_credit_pending != 0) {
                uint16_t credit = request.tcp_credit_pending > 0xffffu
                    ? 0xffffu
                    : (uint16_t)request.tcp_credit_pending;
                altcp_recved(request.firmware_pcb, credit);
                request.tcp_credit_pending -= credit;
            }
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
        snprintf(update_status, sizeof(update_status), "%s - REBOOTING", reason);
        recovery_reboot_at_us = time_us_64() + 2000000u;
        update_state = UPDATE_STATE_RECOVERY_REBOOT;
        return false;
    }

    if (!request.done || request.queue_tail != request.queue_head) return false;

    request.firmware_pcb = NULL;
    request.tcp_credit_pending = 0;

    bool chunk_ok = request.http_status == 200 &&
                    request.blob_content_found &&
                    request.blob_content_done;

    if (!chunk_ok) {
        printf("OTA chunk %lu/%lu incomplete: http=%lu result=%d content=%d/%d total=%lu/%lu\n",
               (unsigned long)(current_chunk_index + 1u),
               (unsigned long)pending_manifest.chunk_count,
               (unsigned long)request.http_status, (int)request.result,
               request.blob_content_found ? 1 : 0,
               request.blob_content_done ? 1 : 0,
               (unsigned long)request.decoded_received,
               (unsigned long)request.expected);
        flightwall_ota_http_abort();
        last_check_succeeded = false;
        snprintf(update_status, sizeof(update_status), "CHUNK %lu FAILED",
                 (unsigned long)(current_chunk_index + 1u));
        finish_update_session();
        return false;
    }

    printf("OTA chunk %lu/%lu complete; total=%lu/%lu\n",
           (unsigned long)(current_chunk_index + 1u),
           (unsigned long)pending_manifest.chunk_count,
           (unsigned long)request.decoded_received,
           (unsigned long)request.expected);

    if (current_chunk_index + 1u < pending_manifest.chunk_count) {
        current_chunk_index++;
        if (!start_chunk_download()) {
            flightwall_ota_http_abort();
            last_check_succeeded = false;
            snprintf(update_status, sizeof(update_status), "NEXT CHUNK FAILED");
            finish_update_session();
        }
        return false;
    }

    bool transfer_ok = request.decoded_received == request.expected &&
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

    if (!transfer_ok) {
        printf("OTA transfer incomplete: decoded=%lu/%lu flashed=%lu/%lu\n",
               (unsigned long)request.decoded_received,
               (unsigned long)request.expected,
               (unsigned long)request.flashed_received,
               (unsigned long)request.expected);
        flightwall_ota_http_abort();
        last_check_succeeded = false;
        snprintf(update_status, sizeof(update_status), "TRANSFER INCOMPLETE");
        finish_update_session();
        return false;
    }

    if (!hash_ok) {
        printf("OTA SHA-256 mismatch after %lu bytes\n",
               (unsigned long)request.flashed_received);
        flightwall_ota_http_abort();
        last_check_succeeded = false;
        snprintf(update_status, sizeof(update_status), "SHA256 MISMATCH");
        finish_update_session();
        return false;
    }

    if (!flightwall_ota_http_finish()) {
        printf("OTA finalization failed: %s\n", flightwall_ota_last_error());
        flightwall_ota_http_abort();
        last_check_succeeded = false;
        snprintf(update_status, sizeof(update_status), "OTA FINALIZE FAILED");
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

        if (verified_manifest_available &&
            verified_manifest.build > FLIGHTWALL_BUILD_NUMBER) {
            if (!update_tls_begin()) {
                snprintf(update_status, sizeof(update_status), "TLS INIT FAILED");
                return false;
            }

            update_manifest_t manifest = verified_manifest;
            verified_manifest_available = false;
            if (!start_firmware_download(&manifest)) {
                last_check_succeeded = false;
                snprintf(update_status, sizeof(update_status), "UPDATE DOWNLOAD START FAILED");
                update_tls_end();
            }
            return false;
        }

        // Direct install without a prior successful check still performs a
        // fresh update feed lookup.
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

