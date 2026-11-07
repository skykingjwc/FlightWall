#include "airline_assets.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/apps/http_client.h"
#include "lwip/pbuf.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "pico/bootrom.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#define GITHUB_HOST "api.github.com"
#define GITHUB_PORT 443
#define AIRLINE_CHECK_PATH "/repos/skykingjwc/FlightWall/contents/airlines/generated/check.json?ref=main"
#define GITHUB_BLOB_PREFIX "/repos/skykingjwc/FlightWall/git/blobs/"

#define AIRLINE_META_MAGIC 0x4657414du
#define AIRLINE_PACK_MAGIC 0x46574150u
#define AIRLINE_SCHEMA 1u
#define AIRLINE_MAX_ICONS 47u

#define AIRLINE_META_FLASH_OFFSET (PICO_FLASH_BANK_STORAGE_OFFSET + (2u * FLASH_SECTOR_SIZE))
#define AIRLINE_PAYLOAD_FLASH_OFFSET (PICO_FLASH_BANK_STORAGE_OFFSET + (3u * FLASH_SECTOR_SIZE))
#define AIRLINE_PAYLOAD_CAPACITY (52u * 1024u)

#define SMALL_CAPACITY 4096u
#define REQUEST_TIMEOUT_MS 20000u
#define FIRST_CHECK_DELAY_MS (90u * 1000u)
#define CHECK_INTERVAL_MS (24u * 60u * 60u * 1000u)
#define RETRY_INTERVAL_MS (60u * 60u * 1000u)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t schema;
    uint16_t icon_count;
    uint8_t width;
    uint8_t height;
    uint16_t reserved;
    uint32_t data_offset;
} airline_pack_header_t;

typedef struct __attribute__((packed)) {
    char prefix[4];
    uint32_t offset;
} airline_pack_entry_t;

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t reserved;
    uint32_t payload_size;
    uint8_t sha256[32];
} airline_meta_t;

typedef struct {
    uint32_t schema;
    uint32_t size;
    char blob_sha[41];
    uint8_t sha256[32];
} airline_manifest_t;

typedef enum {
    ASSET_IDLE,
    ASSET_MANIFEST_PENDING,
    ASSET_PACK_PENDING
} asset_state_t;

typedef enum {
    REQUEST_NONE,
    REQUEST_SMALL,
    REQUEST_BLOB
} request_kind_t;

typedef struct {
    request_kind_t kind;
    volatile bool done;
    volatile bool failed;
    volatile httpc_result_t result;
    volatile uint32_t http_status;
    volatile bool content_length_known;
    size_t received;
    size_t capacity;
    uint8_t *buffer;

    bool blob_content_found;
    bool blob_content_done;
    bool blob_escape;
    uint8_t blob_scan_state;
    uint8_t blob_key_match;
    char b64_quartet[4];
    uint8_t b64_count;

    bool http_chunked;
    uint32_t chunk_remaining;
    uint32_t chunk_size;
    uint8_t chunk_state;
    bool chunk_extension;
} asset_request_t;

static struct altcp_tls_config *tls_config;
static altcp_allocator_t tls_allocator;
static httpc_connection_t http_connection;
static struct altcp_pcb *active_pcb;
static asset_request_t request;
static asset_state_t state;
static uint8_t small_response[SMALL_CAPACITY + 1];
static uint8_t decoded_manifest[SMALL_CAPACITY + 1];
static uint8_t pack_buffer[AIRLINE_PAYLOAD_CAPACITY];
static airline_manifest_t pending_manifest;
static airline_meta_t current_meta;
static bool current_valid;
static uint32_t next_check_ms;
static uint32_t request_started_ms;
static char status_text[48] = "BUILT-IN";

enum {
    HTTP_CHUNK_SIZE = 0,
    HTTP_CHUNK_SIZE_LF,
    HTTP_CHUNK_DATA,
    HTTP_CHUNK_DATA_CR,
    HTTP_CHUNK_DATA_LF,
    HTTP_CHUNK_TRAILER
};

static uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
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

static bool decode_base64_quartet(const char input[4], uint8_t output[3], size_t *length) {
    int v0 = base64_value((unsigned char)input[0]);
    int v1 = base64_value((unsigned char)input[1]);
    int v2 = base64_value((unsigned char)input[2]);
    int v3 = base64_value((unsigned char)input[3]);
    if (v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1) return false;
    output[0] = (uint8_t)((v0 << 2) | (v1 >> 4));
    if (v2 == -2) {
        if (v3 != -2) return false;
        *length = 1;
        return true;
    }
    output[1] = (uint8_t)(((v1 & 0x0f) << 4) | (v2 >> 2));
    if (v3 == -2) {
        *length = 2;
        return true;
    }
    output[2] = (uint8_t)(((v2 & 0x03) << 6) | v3);
    *length = 3;
    return true;
}

static bool hex_digest(const char *text, uint8_t output[32]) {
    if (!text || strlen(text) != 64u) return false;
    for (size_t i = 0; i < 32; ++i) {
        int hi, lo;
        unsigned char a = (unsigned char)text[i * 2u];
        unsigned char b = (unsigned char)text[i * 2u + 1u];
        if (a >= '0' && a <= '9') hi = a - '0';
        else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10;
        else if (a >= 'A' && a <= 'F') hi = a - 'A' + 10;
        else return false;
        if (b >= '0' && b <= '9') lo = b - '0';
        else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10;
        else if (b >= 'A' && b <= 'F') lo = b - 'A' + 10;
        else return false;
        output[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool digest_equal(const uint8_t a[32], const uint8_t b[32]) {
    uint8_t diff = 0;
    for (size_t i = 0; i < 32; ++i) diff |= a[i] ^ b[i];
    return diff == 0;
}

static const char *json_value(const char *json, const char *key) {
    static char pattern[48];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return NULL;
    p = strchr(p + strlen(pattern), ':');
    if (!p) return NULL;
    do { ++p; } while (*p && isspace((unsigned char)*p));
    return p;
}

static bool json_u32(const char *json, const char *key, uint32_t *value) {
    const char *p = json_value(json, key);
    if (!p || !isdigit((unsigned char)*p)) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(p, &end, 10);
    if (!end || end == p || parsed > UINT32_MAX) return false;
    *value = (uint32_t)parsed;
    return true;
}

static bool json_string(const char *json, const char *key, char *out, size_t out_size) {
    const char *p = json_value(json, key);
    if (!p || *p != '\"') return false;
    ++p;
    size_t n = 0;
    while (*p && *p != '\"') {
        if (*p == '\\' || (unsigned char)*p < 32 || n + 1 >= out_size) return false;
        out[n++] = *p++;
    }
    if (*p != '\"') return false;
    out[n] = '\0';
    return true;
}

static bool parse_manifest(const uint8_t *data, size_t length, airline_manifest_t *manifest) {
    if (!data || !manifest || length == 0 || length > SMALL_CAPACITY) return false;
    char json[SMALL_CAPACITY + 1];
    memcpy(json, data, length);
    json[length] = '\0';

    char sha_text[65];
    memset(manifest, 0, sizeof(*manifest));
    if (!json_u32(json, "schema", &manifest->schema) ||
        !json_u32(json, "size", &manifest->size) ||
        !json_string(json, "blob_sha", manifest->blob_sha, sizeof(manifest->blob_sha)) ||
        !json_string(json, "sha256", sha_text, sizeof(sha_text)) ||
        manifest->schema != AIRLINE_SCHEMA ||
        manifest->size == 0 ||
        manifest->size > AIRLINE_PAYLOAD_CAPACITY ||
        strlen(manifest->blob_sha) != 40u ||
        !hex_digest(sha_text, manifest->sha256)) {
        return false;
    }
    return true;
}

static bool validate_pack(const uint8_t *data, size_t length) {
    if (!data || length < sizeof(airline_pack_header_t)) return false;
    const airline_pack_header_t *header = (const airline_pack_header_t *)data;
    if (header->magic != AIRLINE_PACK_MAGIC ||
        header->schema != AIRLINE_SCHEMA ||
        header->icon_count > AIRLINE_MAX_ICONS ||
        header->width != FLIGHTWALL_AIRLINE_ICON_WIDTH ||
        header->height != FLIGHTWALL_AIRLINE_ICON_HEIGHT) {
        return false;
    }

    size_t index_end = sizeof(*header) +
                       (size_t)header->icon_count * sizeof(airline_pack_entry_t);
    if (header->data_offset < index_end || header->data_offset > length) return false;

    const airline_pack_entry_t *entries =
        (const airline_pack_entry_t *)(data + sizeof(*header));
    const size_t icon_bytes = (size_t)header->width * (size_t)header->height;
    for (uint16_t i = 0; i < header->icon_count; ++i) {
        if (entries[i].prefix[3] != '\0') return false;
        for (int j = 0; j < 3; ++j)
            if (!isalnum((unsigned char)entries[i].prefix[j])) return false;
        if ((size_t)entries[i].offset < header->data_offset ||
            (size_t)entries[i].offset + icon_bytes > length) {
            return false;
        }
    }
    return true;
}

static bool read_flash(uint32_t offset, void *data, size_t length) {
    cflash_flags_t flags = {
        .flags =
            (CFLASH_OP_VALUE_READ << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    return rom_flash_op(flags, XIP_BASE + offset, length, data) == BOOTROM_OK;
}

static bool load_existing_pack(void) {
    airline_meta_t meta;
    if (!read_flash(AIRLINE_META_FLASH_OFFSET, &meta, sizeof(meta)) ||
        meta.magic != AIRLINE_META_MAGIC ||
        meta.schema != AIRLINE_SCHEMA ||
        meta.payload_size == 0 ||
        meta.payload_size > AIRLINE_PAYLOAD_CAPACITY) {
        return false;
    }

    if (!read_flash(AIRLINE_PAYLOAD_FLASH_OFFSET, pack_buffer, meta.payload_size) ||
        !validate_pack(pack_buffer, meta.payload_size)) {
        return false;
    }

    uint8_t digest[32];
    mbedtls_sha256(pack_buffer, meta.payload_size, digest, 0);
    if (!digest_equal(digest, meta.sha256)) return false;

    current_meta = meta;
    current_valid = true;
    const airline_pack_header_t *header = (const airline_pack_header_t *)pack_buffer;
    snprintf(status_text, sizeof(status_text), "CURRENT %u ICONS", (unsigned)header->icon_count);
    return true;
}

static bool store_pack(const airline_manifest_t *manifest) {
    if (!manifest || !validate_pack(pack_buffer, manifest->size)) return false;

    size_t program_size = (manifest->size + 255u) & ~(size_t)255u;
    if (program_size > AIRLINE_PAYLOAD_CAPACITY) return false;

    cflash_flags_t erase_flags = {
        .flags =
            (CFLASH_OP_VALUE_ERASE << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    cflash_flags_t program_flags = {
        .flags =
            (CFLASH_OP_VALUE_PROGRAM << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };

    watchdog_update();
    if (rom_flash_op(erase_flags, XIP_BASE + AIRLINE_META_FLASH_OFFSET,
                     FLASH_SECTOR_SIZE, NULL) != BOOTROM_OK) {
        return false;
    }
    current_valid = false;

    size_t erase_size =
        (program_size + FLASH_SECTOR_SIZE - 1u) &
        ~(size_t)(FLASH_SECTOR_SIZE - 1u);
    watchdog_update();
    if (rom_flash_op(erase_flags, XIP_BASE + AIRLINE_PAYLOAD_FLASH_OFFSET,
                     erase_size, NULL) != BOOTROM_OK) {
        return false;
    }

    memset(pack_buffer + manifest->size, 0, program_size - manifest->size);
    for (size_t offset = 0; offset < program_size; offset += 256u) {
        watchdog_update();
        if (rom_flash_op(program_flags,
                         XIP_BASE + AIRLINE_PAYLOAD_FLASH_OFFSET + offset,
                         256u, pack_buffer + offset) != BOOTROM_OK) {
            return false;
        }
    }

    uint8_t sector[FLASH_SECTOR_SIZE];
    memset(sector, 0xff, sizeof(sector));
    airline_meta_t *meta = (airline_meta_t *)sector;
    meta->magic = AIRLINE_META_MAGIC;
    meta->schema = AIRLINE_SCHEMA;
    meta->payload_size = manifest->size;
    memcpy(meta->sha256, manifest->sha256, sizeof(meta->sha256));

    watchdog_update();
    if (rom_flash_op(program_flags, XIP_BASE + AIRLINE_META_FLASH_OFFSET,
                     sizeof(sector), sector) != BOOTROM_OK) {
        return false;
    }

    current_meta = *meta;
    current_valid = true;
    const airline_pack_header_t *header = (const airline_pack_header_t *)pack_buffer;
    snprintf(status_text, sizeof(status_text), "CURRENT %u ICONS", (unsigned)header->icon_count);
    printf("Airline icon pack installed: %u icons, %lu bytes\n",
           (unsigned)header->icon_count, (unsigned long)manifest->size);
    return true;
}

static struct altcp_pcb *tls_alloc(void *arg, u8_t ip_type) {
    struct altcp_pcb *pcb = altcp_tls_alloc(arg, ip_type);
    if (!pcb) return NULL;
    mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)altcp_tls_context(pcb);
    if (!ssl || mbedtls_ssl_set_hostname(ssl, GITHUB_HOST) != 0) {
        altcp_abort(pcb);
        return NULL;
    }
    active_pcb = pcb;
    return pcb;
}

static bool tls_begin(void) {
    if (tls_config) return true;
    tls_config = altcp_tls_create_config_client(NULL, 0);
    if (!tls_config) return false;
    tls_allocator.alloc = tls_alloc;
    tls_allocator.arg = tls_config;
    return true;
}

static void tls_end(void) {
    if (!tls_config) return;
    altcp_tls_free_config(tls_config);
    tls_config = NULL;
    tls_allocator.arg = NULL;
    active_pcb = NULL;
}

static void request_finished(void *arg, httpc_result_t result, u32_t rx_content_len,
                             u32_t srv_res, err_t err) {
    (void)arg;
    (void)rx_content_len;
    (void)err;
    request.result = result;
    request.http_status = srv_res;
    active_pcb = NULL;
    request.done = true;
}

static int chunk_hex(unsigned char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool blob_feed(const uint8_t *data, size_t length) {
    static const char key[] = "\"content\"";
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = data[i];
        if (request.blob_content_done) continue;

        if (!request.blob_content_found) {
            if (request.blob_scan_state == 0) {
                if (ch == (unsigned char)key[request.blob_key_match]) {
                    request.blob_key_match++;
                    if (request.blob_key_match == sizeof(key) - 1u) {
                        request.blob_scan_state = 1;
                        request.blob_key_match = 0;
                    }
                } else {
                    request.blob_key_match = (ch == (unsigned char)key[0]) ? 1u : 0u;
                }
            } else if (request.blob_scan_state == 1) {
                if (isspace(ch)) continue;
                if (ch != ':') return false;
                request.blob_scan_state = 2;
            } else {
                if (isspace(ch)) continue;
                if (ch != '\"') return false;
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
        if (ch == '\"') {
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
                request.received + decoded_length > request.capacity) {
                return false;
            }
            memcpy(request.buffer + request.received, decoded, decoded_length);
            request.received += decoded_length;
            request.b64_count = 0;
        }
    }
    return true;
}

static bool blob_http_feed(const uint8_t *data, size_t length) {
    if (!request.http_chunked) return blob_feed(data, length);

    size_t i = 0;
    while (i < length) {
        unsigned char ch = data[i];
        switch (request.chunk_state) {
            case HTTP_CHUNK_SIZE: {
                if (ch == '\r') {
                    request.chunk_state = HTTP_CHUNK_SIZE_LF;
                    ++i;
                    break;
                }
                if (request.chunk_extension) {
                    ++i;
                    break;
                }
                if (ch == ';') {
                    request.chunk_extension = true;
                    ++i;
                    break;
                }
                int value = chunk_hex(ch);
                if (value < 0 || request.chunk_size > 0x0fffffffu) return false;
                request.chunk_size = (request.chunk_size << 4) | (uint32_t)value;
                ++i;
                break;
            }
            case HTTP_CHUNK_SIZE_LF:
                if (ch != '\n') return false;
                ++i;
                if (request.chunk_size == 0) request.chunk_state = HTTP_CHUNK_TRAILER;
                else {
                    request.chunk_remaining = request.chunk_size;
                    request.chunk_state = HTTP_CHUNK_DATA;
                }
                break;
            case HTTP_CHUNK_DATA: {
                size_t available = length - i;
                size_t take = request.chunk_remaining < available
                    ? request.chunk_remaining : available;
                if (take && !blob_feed(data + i, take)) return false;
                i += take;
                request.chunk_remaining -= (uint32_t)take;
                if (request.chunk_remaining == 0) request.chunk_state = HTTP_CHUNK_DATA_CR;
                break;
            }
            case HTTP_CHUNK_DATA_CR:
                if (ch != '\r') return false;
                request.chunk_state = HTTP_CHUNK_DATA_LF;
                ++i;
                break;
            case HTTP_CHUNK_DATA_LF:
                if (ch != '\n') return false;
                request.chunk_state = HTTP_CHUNK_SIZE;
                request.chunk_size = 0;
                request.chunk_extension = false;
                ++i;
                break;
            case HTTP_CHUNK_TRAILER:
                i = length;
                break;
            default:
                return false;
        }
    }
    return true;
}

static bool decode_http_chunks(uint8_t *data, size_t *length, size_t capacity) {
    size_t input = 0, output = 0;
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
                request.received += part->len;
            } else if (request.kind == REQUEST_BLOB) {
                if (!blob_http_feed((const uint8_t *)part->payload, part->len)) {
                    request.failed = true;
                    break;
                }
            }
        }
    }

    altcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
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
    } else if (request.kind == REQUEST_BLOB) {
        request.http_chunked = !request.content_length_known;
        request.chunk_state = HTTP_CHUNK_SIZE;
    }
    return ERR_OK;
}

static void reset_request(request_kind_t kind, uint8_t *buffer, size_t capacity) {
    memset(&request, 0, sizeof(request));
    request.kind = kind;
    request.buffer = buffer;
    request.capacity = capacity;
    request.result = HTTPC_RESULT_ERR_UNKNOWN;
}

static bool start_get(const char *path) {
    memset(&http_connection, 0, sizeof(http_connection));
    http_connection.altcp_allocator = &tls_allocator;
    http_connection.result_fn = request_finished;
    http_connection.headers_done_fn = headers_received;
    active_pcb = NULL;

    cyw43_arch_lwip_begin();
    err_t rc = httpc_get_file_dns(GITHUB_HOST, GITHUB_PORT, path,
                                  &http_connection, receive_body, NULL, NULL);
    cyw43_arch_lwip_end();
    if (rc == ERR_OK) request_started_ms = now_ms();
    return rc == ERR_OK;
}

static bool decode_contents_response(size_t *decoded_length) {
    size_t envelope_length = request.received;
    if (!request.content_length_known &&
        !decode_http_chunks(small_response, &envelope_length, SMALL_CAPACITY)) {
        return false;
    }

    asset_request_t saved = request;
    reset_request(REQUEST_BLOB, decoded_manifest, SMALL_CAPACITY);
    request.http_chunked = false;
    bool ok = blob_feed(small_response, envelope_length) &&
              request.blob_content_found &&
              request.blob_content_done;
    if (ok) *decoded_length = request.received;
    request = saved;
    return ok;
}

static bool start_manifest_request(void) {
    if (!tls_begin()) return false;
    reset_request(REQUEST_SMALL, small_response, SMALL_CAPACITY);
    if (!start_get(AIRLINE_CHECK_PATH)) {
        tls_end();
        return false;
    }
    state = ASSET_MANIFEST_PENDING;
    snprintf(status_text, sizeof(status_text), "CHECKING");
    printf("Airline icon pack check started\n");
    return true;
}

static bool start_pack_request(const airline_manifest_t *manifest) {
    char path[160];
    if (snprintf(path, sizeof(path), "%s%s", GITHUB_BLOB_PREFIX,
                 manifest->blob_sha) <= 0) {
        return false;
    }
    pending_manifest = *manifest;
    reset_request(REQUEST_BLOB, pack_buffer, AIRLINE_PAYLOAD_CAPACITY);
    if (!start_get(path)) return false;
    state = ASSET_PACK_PENDING;
    snprintf(status_text, sizeof(status_text), "DOWNLOADING");
    printf("Airline icon pack download started: %lu bytes\n",
           (unsigned long)manifest->size);
    return true;
}

static void finish_check(bool success) {
    tls_end();
    state = ASSET_IDLE;
    next_check_ms = now_ms() + (success ? CHECK_INTERVAL_MS : RETRY_INTERVAL_MS);
}

void flightwall_airline_assets_init(void) {
    memset(&current_meta, 0, sizeof(current_meta));
    current_valid = false;
    state = ASSET_IDLE;
    if (!load_existing_pack()) {
        snprintf(status_text, sizeof(status_text), "BUILT-IN");
        printf("Airline icon pack: using built-in logos\n");
    } else {
        printf("Airline icon pack: %s\n", status_text);
    }
    next_check_ms = now_ms() + FIRST_CHECK_DELAY_MS;
}

void flightwall_airline_assets_process(void) {
    // Temporary 1.6.96 stabilization: leave persistent asset loading/rendering
    // enabled, but do not start a second GitHub TLS client automatically.
    // 1.6.95 showed runtime lockups correlated with the first asset check.
    // Existing packs remain readable and built-in logos remain the fallback.
    if (state == ASSET_IDLE) return;

    if (!request.done && now_ms() - request_started_ms >= REQUEST_TIMEOUT_MS) {
        printf("Airline icon request timed out\n");
        if (active_pcb) {
            struct altcp_pcb *stalled = active_pcb;
            active_pcb = NULL;
            cyw43_arch_lwip_begin();
            altcp_abort(stalled);
            cyw43_arch_lwip_end();
        }
        request.failed = true;
        request.done = true;
        request.result = HTTPC_RESULT_ERR_CLOSED;
        request.http_status = 0;
    }

    if (!request.done) return;

    if (request.failed || request.result != HTTPC_RESULT_OK || request.http_status != 200) {
        printf("Airline icon request failed: http=%lu result=%d\n",
               (unsigned long)request.http_status, (int)request.result);
        snprintf(status_text, sizeof(status_text), "CHECK FAILED");
        finish_check(false);
        return;
    }

    if (state == ASSET_MANIFEST_PENDING) {
        size_t decoded_length = 0;
        if (!decode_contents_response(&decoded_length)) {
            snprintf(status_text, sizeof(status_text), "BAD MANIFEST ENVELOPE");
            finish_check(false);
            return;
        }

        airline_manifest_t manifest;
        if (!parse_manifest(decoded_manifest, decoded_length, &manifest)) {
            snprintf(status_text, sizeof(status_text), "BAD MANIFEST");
            finish_check(false);
            return;
        }

        if (current_valid &&
            current_meta.payload_size == manifest.size &&
            digest_equal(current_meta.sha256, manifest.sha256)) {
            const airline_pack_header_t *header = (const airline_pack_header_t *)pack_buffer;
            snprintf(status_text, sizeof(status_text), "CURRENT %u ICONS",
                     (unsigned)header->icon_count);
            finish_check(true);
            return;
        }

        if (!start_pack_request(&manifest)) {
            snprintf(status_text, sizeof(status_text), "DOWNLOAD START FAILED");
            finish_check(false);
        }
        return;
    }

    if (state == ASSET_PACK_PENDING) {
        if (!request.blob_content_found ||
            !request.blob_content_done ||
            request.received != pending_manifest.size ||
            !validate_pack(pack_buffer, request.received)) {
            snprintf(status_text, sizeof(status_text), "BAD ICON PACK");
            finish_check(false);
            return;
        }

        uint8_t digest[32];
        mbedtls_sha256(pack_buffer, request.received, digest, 0);
        if (!digest_equal(digest, pending_manifest.sha256)) {
            snprintf(status_text, sizeof(status_text), "ICON SHA MISMATCH");
            finish_check(false);
            return;
        }

        tls_end();
        state = ASSET_IDLE;
        if (!store_pack(&pending_manifest)) {
            snprintf(status_text, sizeof(status_text), "ICON FLASH FAILED");
            next_check_ms = now_ms() + RETRY_INTERVAL_MS;
            return;
        }
        next_check_ms = now_ms() + CHECK_INTERVAL_MS;
    }
}

bool flightwall_airline_assets_network_busy(void) {
    return state != ASSET_IDLE;
}

bool flightwall_airline_assets_valid(void) {
    return current_valid;
}

const char *flightwall_airline_assets_status(void) {
    return status_text;
}

bool flightwall_airline_assets_get(const char *callsign,
                                   const uint8_t **pixels,
                                   uint8_t *width,
                                   uint8_t *height) {
    if (!current_valid || !callsign || !pixels || !width || !height ||
        strlen(callsign) < 3u) {
        return false;
    }

    // The pack was already read from storage, validated, and SHA-256 checked at boot.\n    // Render from that RAM copy instead of directly dereferencing storage through XIP;\n    // the latter is unsafe with the RP2350 TBYB/storage address-space mapping.\n    const uint8_t *payload = pack_buffer;
    const airline_pack_header_t *header = (const airline_pack_header_t *)payload;
    if (header->magic != AIRLINE_PACK_MAGIC ||
        header->schema != AIRLINE_SCHEMA ||
        header->icon_count > AIRLINE_MAX_ICONS) {
        return false;
    }

    char prefix[4] = {
        (char)toupper((unsigned char)callsign[0]),
        (char)toupper((unsigned char)callsign[1]),
        (char)toupper((unsigned char)callsign[2]),
        '\0'
    };
    const airline_pack_entry_t *entries =
        (const airline_pack_entry_t *)(payload + sizeof(*header));
    for (uint16_t i = 0; i < header->icon_count; ++i) {
        if (memcmp(entries[i].prefix, prefix, 4) == 0) {
            *pixels = payload + entries[i].offset;
            *width = header->width;
            *height = header->height;
            return true;
        }
    }
    return false;
}
