#include "update_client.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/http_client.h"
#include "lwip/dns.h"
#include "lwip/pbuf.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "ota_server.h"
#include "flightwall_settings.h"
#include "update_public_key.h"

#define UPDATE_API_HOST "api.github.com"
#define UPDATE_PORT 443
#define FEED_API_PATH "/repos/skykingjwc/FlightWall/contents/update/feed.json?ref=main"
#define BLOB_API_PREFIX "/repos/skykingjwc/FlightWall/git/blobs/"
#define SMALL_RESPONSE_CAPACITY 4096u
#define FEED_HTTP_CAPACITY 8192u
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
    size_t decoded_received;
    bool blob_content_found;
    bool blob_content_done;
    bool blob_escape;
    uint8_t blob_scan_state;
    uint8_t blob_key_match;
    char b64_quartet[4];
    uint8_t b64_count;
} update_request_t;

typedef struct {
    char version[16];
    char blob_sha[41];
    uint32_t build;
    uint32_t size;
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
static struct altcp_pcb *feed_pcb;
static uint8_t feed_http_response[FEED_HTTP_CAPACITY + 1];
static size_t feed_http_received;
static uint8_t small_response[SMALL_RESPONSE_CAPACITY + 1];
static uint8_t feed_bytes[SMALL_RESPONSE_CAPACITY + 1];
static uint8_t manifest_bytes[2048];
static uint8_t signature[SIGNATURE_CAPACITY];
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
    UPDATE_STATE_READY_TO_REBOOT
} update_state_t;

static update_state_t update_state = UPDATE_STATE_IDLE;
static bool pending_force_install;
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

static bool firmware_write_decoded(const uint8_t *data, size_t length) {
    if (request.decoded_received + length > request.expected ||
        !flightwall_ota_http_write(data, length) ||
        mbedtls_sha256_update(&request.sha, data, length) != 0) {
        return false;
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
                !firmware_write_decoded(decoded, decoded_length)) {
                return false;
            }
            request.b64_count = 0;
        }
    }
    return true;
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
                if (!firmware_blob_feed((const uint8_t *)part->payload, part->len)) {
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
        // GitHub's blob API returns JSON/base64, so HTTP Content-Length is the
        // encoded envelope size rather than the decoded UF2 size.
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


static void feed_finish(httpc_result_t result) {
    request.result = result;
    request.done = true;
    if (feed_pcb) {
        altcp_arg(feed_pcb, NULL);
        altcp_recv(feed_pcb, NULL);
        altcp_err(feed_pcb, NULL);
        altcp_poll(feed_pcb, NULL, 0);
        err_t rc = altcp_close(feed_pcb);
        if (rc != ERR_OK) altcp_abort(feed_pcb);
        feed_pcb = NULL;
    }
}

static err_t feed_recv(void *arg, struct altcp_pcb *pcb, struct pbuf *p, err_t err) {
    (void)arg;
    if (!p) {
        if (!request.done) feed_finish(request.failed ? HTTPC_RESULT_ERR_MEM : HTTPC_RESULT_OK);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        pbuf_free(p);
        request.failed = true;
        feed_finish(HTTPC_RESULT_ERR_UNKNOWN);
        return err;
    }

    for (struct pbuf *part = p; part; part = part->next) {
        if (feed_http_received + part->len > FEED_HTTP_CAPACITY) {
            request.failed = true;
            break;
        }
        memcpy(feed_http_response + feed_http_received, part->payload, part->len);
        feed_http_received += part->len;
    }

    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);

    if (request.failed) {
        feed_finish(HTTPC_RESULT_ERR_MEM);
        return ERR_BUF;
    }
    return ERR_OK;
}

static void feed_error(void *arg, err_t err) {
    (void)arg;
    feed_pcb = NULL;
    if (!request.done) {
        request.failed = true;
        request.result = err == ERR_TIMEOUT ? HTTPC_RESULT_ERR_TIMEOUT : HTTPC_RESULT_ERR_CONNECT;
        request.done = true;
    }
}

static err_t feed_poll(void *arg, struct altcp_pcb *pcb) {
    (void)arg;
    (void)pcb;
    return ERR_OK;
}

static err_t feed_connected(void *arg, struct altcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK) {
        request.failed = true;
        feed_finish(HTTPC_RESULT_ERR_CONNECT);
        return err;
    }

    static const char feed_request[] =
        "GET " FEED_API_PATH " HTTP/1.1\r\n"
        "Host: " UPDATE_API_HOST "\r\n"
        "User-Agent: FlightWall/" FLIGHTWALL_VERSION "\r\n"
        "Accept: application/vnd.github.raw+json\r\n"
        "X-GitHub-Api-Version: 2022-11-28\r\n"
        "Connection: close\r\n"
        "\r\n";

    err_t rc = altcp_write(pcb, feed_request, (u16_t)(sizeof(feed_request) - 1), TCP_WRITE_FLAG_COPY);
    if (rc == ERR_OK) rc = altcp_output(pcb);
    if (rc != ERR_OK) {
        request.failed = true;
        feed_finish(HTTPC_RESULT_ERR_CONNECT);
    }
    return rc;
}

static void feed_dns_found(const char *name, const ip_addr_t *ipaddr, void *arg) {
    (void)name;
    (void)arg;

    // This callback is either invoked by lwIP itself or called synchronously
    // while cyw43_arch_lwip_begin() is held for a cached DNS result.
    cyw43_arch_lwip_check();

    if (!ipaddr) {
        request.failed = true;
        request.result = HTTPC_RESULT_ERR_HOSTNAME;
        request.done = true;
        return;
    }

    tls_sni_host = UPDATE_API_HOST;
    feed_pcb = altcp_new_ip_type(&tls_allocator, IP_GET_TYPE(ipaddr));
    if (!feed_pcb) {
        request.failed = true;
        request.result = HTTPC_RESULT_ERR_MEM;
        request.done = true;
        return;
    }

    altcp_arg(feed_pcb, NULL);
    altcp_recv(feed_pcb, feed_recv);
    altcp_err(feed_pcb, feed_error);
    altcp_poll(feed_pcb, feed_poll, 4);

    err_t rc = altcp_connect(feed_pcb, ipaddr, UPDATE_PORT, feed_connected);
    if (rc != ERR_OK) {
        request.failed = true;
        feed_finish(HTTPC_RESULT_ERR_CONNECT);
    }
}

static bool start_feed_get(void) {
    feed_http_received = 0;
    feed_http_response[0] = '\0';

    ip_addr_t address;
    cyw43_arch_lwip_begin();
    err_t rc = dns_gethostbyname(UPDATE_API_HOST, &address, feed_dns_found, NULL);

    // dns_gethostbyname() returns ERR_OK immediately when the name is already
    // cached. In that case there is no asynchronous callback, so create and
    // connect the altcp/TLS PCB *before* releasing the lwIP lock.
    if (rc == ERR_OK) {
        feed_dns_found(UPDATE_API_HOST, &address, NULL);
    }
    cyw43_arch_lwip_end();

    if (rc == ERR_OK) return !request.done;
    if (rc == ERR_INPROGRESS) return true;

    request.failed = true;
    request.result = HTTPC_RESULT_ERR_HOSTNAME;
    request.done = true;
    return false;
}

static bool parse_feed_http_response(size_t *body_offset, size_t *body_length) {
    if (!body_offset || !body_length || feed_http_received == 0 ||
        feed_http_received > FEED_HTTP_CAPACITY) return false;

    feed_http_response[feed_http_received] = '\0';

    char *status_end = strstr((char *)feed_http_response, "\r\n");
    char *headers_end = strstr((char *)feed_http_response, "\r\n\r\n");
    if (!status_end || !headers_end) return false;

    unsigned status = 0;
    if (sscanf((char *)feed_http_response, "HTTP/%*u.%*u %u", &status) != 1)
        return false;
    request.http_status = status;
    if (status != 200) return false;

    size_t offset = (size_t)((headers_end + 4) - (char *)feed_http_response);
    if (offset > feed_http_received) return false;

    *body_offset = offset;
    *body_length = feed_http_received - offset;
    return *body_length > 0;
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
    uint32_t schema = 0;
    memset(manifest, 0, sizeof(*manifest));
    const char *json = (const char *)data;
    if (!json_u32(json, length, "schema", &schema) || schema != 2 ||
        !json_u32(json, length, "build", &manifest->build) ||
        !json_u32(json, length, "size", &manifest->size) ||
        !json_string(json, length, "version", manifest->version, sizeof(manifest->version)) ||
        !json_string(json, length, "blob_sha", manifest->blob_sha, sizeof(manifest->blob_sha)) ||
        !json_string(json, length, "sha256", sha_text, sizeof(sha_text)) ||
        !hex_digest(sha_text, manifest->sha256)) {
        return false;
    }
    if (strlen(manifest->blob_sha) != 40) return false;
    for (size_t i = 0; i < 40; ++i)
        if (!isxdigit((unsigned char)manifest->blob_sha[i])) return false;

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
    if (!start_feed_get()) {
        snprintf(update_status, sizeof(update_status), "UPDATE FEED START FAILED");
        update_tls_end();
        return false;
    }

    update_state = UPDATE_STATE_FEED_PENDING;
    return true;
}

static bool decode_manifest_b64(const char *manifest_b64, size_t *manifest_length) {
    *manifest_length = 0;
    char quartet[4];
    uint8_t quartet_count = 0;

    for (size_t i = 0; manifest_b64[i]; ++i) {
        unsigned char ch = (unsigned char)manifest_b64[i];
        if (isspace(ch)) continue;
        if (base64_value(ch) == -1) return false;

        quartet[quartet_count++] = (char)ch;
        if (quartet_count == 4) {
            uint8_t decoded[3];
            size_t decoded_length = 0;
            if (!decode_base64_quartet(quartet, decoded, &decoded_length) ||
                *manifest_length + decoded_length > sizeof(manifest_bytes)) {
                return false;
            }
            memcpy(manifest_bytes + *manifest_length, decoded, decoded_length);
            *manifest_length += decoded_length;
            quartet_count = 0;
        }
    }

    return quartet_count == 0 && *manifest_length > 0;
}

static bool start_firmware_download(const update_manifest_t *manifest) {
    char blob_path[160];
    if (snprintf(blob_path, sizeof(blob_path), "%s%s", BLOB_API_PREFIX,
                 manifest->blob_sha) <= 0) {
        return false;
    }

    if (!flightwall_ota_http_begin(manifest->size)) return false;

    pending_manifest = *manifest;
    memset(&request, 0, sizeof(request));
    request.kind = REQUEST_FIRMWARE;
    request.expected = manifest->size;
    request.result = HTTPC_RESULT_ERR_UNKNOWN;
    mbedtls_sha256_init(&request.sha);
    request.sha_active = mbedtls_sha256_starts(&request.sha, 0) == 0;

    if (!request.sha_active || !start_get(UPDATE_API_HOST, blob_path)) {
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

    if (request.failed || request.result != HTTPC_RESULT_OK) {
        snprintf(update_status, sizeof(update_status), "FEED HTTP %lu ERR %d",
                 (unsigned long)request.http_status, (int)request.result);
        finish_update_session();
        return false;
    }

    size_t body_offset = 0;
    size_t feed_length = 0;
    if (!parse_feed_http_response(&body_offset, &feed_length)) {
        snprintf(update_status, sizeof(update_status), "FEED HTTP %lu ERR %d",
                 (unsigned long)request.http_status, (int)request.result);
        finish_update_session();
        return false;
    }

    const char *feed_json = (const char *)feed_http_response + body_offset;

    char manifest_b64[3072];
    char signature_hex[2 * SIGNATURE_CAPACITY + 1];
    if (!json_string(feed_json, feed_length, "manifest_b64",
                     manifest_b64, sizeof(manifest_b64)) ||
        !json_string(feed_json, feed_length, "signature_hex",
                     signature_hex, sizeof(signature_hex))) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE FEED");
        finish_update_session();
        return false;
    }

    size_t manifest_length = 0;
    size_t signature_length = 0;
    if (!decode_manifest_b64(manifest_b64, &manifest_length)) {
        snprintf(update_status, sizeof(update_status), "BAD MANIFEST ENCODING");
        finish_update_session();
        return false;
    }
    if (!hex_bytes(signature_hex, strlen(signature_hex),
                   signature, SIGNATURE_CAPACITY, &signature_length) ||
        signature_length == 0) {
        snprintf(update_status, sizeof(update_status), "BAD SIGNATURE FORMAT");
        finish_update_session();
        return false;
    }
    if (!verify_manifest_signature(manifest_bytes, manifest_length,
                                   signature, signature_length)) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE SIGNATURE");
        finish_update_session();
        return false;
    }

    update_manifest_t manifest;
    if (!parse_manifest(manifest_bytes, manifest_length, &manifest)) {
        snprintf(update_status, sizeof(update_status), "BAD UPDATE MANIFEST");
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

static bool process_firmware_result(void) {
    if (!request.done) return false;

    bool transfer_ok = request_ok() &&
                       request.blob_content_found &&
                       request.blob_content_done &&
                       request.decoded_received == request.expected;

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
           update_state == UPDATE_STATE_READY_TO_REBOOT;
}

bool flightwall_update_network_busy(void) {
    return update_state == UPDATE_STATE_FEED_PENDING ||
           update_state == UPDATE_STATE_FIRMWARE_PENDING;
}

const char *flightwall_update_status(void) {
    return update_status;
}

