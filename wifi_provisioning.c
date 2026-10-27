/**
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "wifi_provisioning.h"
#include "ota_server.h"
#include "flightwall_settings.h"

#include "lwip/ip4_addr.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/apps/httpd.h"
#include "lwip/pbuf.h"

#include "dhcpserver.h"
#include "dnsserver.h"

#include "pico/flash.h"
#include "pico/bootrom.h"
#include "hardware/flash.h" // for saving succesful credentials

static absolute_time_t wifi_connected_time;
static flightwall_wifi_status_cb display_status;
static bool http_started;
static volatile bool runtime_online = false;
static const char flightwall_hostname[] = CYW43_HOST_NAME;

// Set by the CGI handlers (running in lwIP context) to ask the main loop to
// attempt a STA connection using the current ssid/password globals. The main
// loop owns all radio mode transitions; the handlers only signal intent.
static volatile bool connect_requested = false;
static volatile bool emergency_test_active = false;
static volatile bool load_test_active = false;

// Maximum credential content lengths (excluding the null terminator).
// 32 is the 802.11 SSID limit; 63 is the WPA/WPA2 passphrase limit.
#define MAX_SSID_LEN 32
#define MAX_PASSWORD_LEN 63

// buffers are content length + 1 for the null terminator
static char ssid[MAX_SSID_LEN + 1];
static char password[MAX_PASSWORD_LEN + 1];
static int num_credentials;

// Credentials are stored in a single 4096-byte flash sector as a stream of
// null-separated strings: count byte, then ssid0\0password0\0ssid1\0...
// MAX_CREDENTIALS must be small enough that that many (ssid + password)
// pairs fit in FLASH_SECTOR_SIZE (4096) bytes. Worst case per pair is
// MAX_SSID_LEN + 1 + MAX_PASSWORD_LEN + 1 = 97 bytes, so 4094/97 ~= 42;
// 20 leaves comfortable margin.
#define MAX_CREDENTIALS 20
static char ssid_list[MAX_CREDENTIALS][MAX_SSID_LEN + 1];
static char password_list[MAX_CREDENTIALS][MAX_PASSWORD_LEN + 1];

// Define flash offset towards end of flash
#ifndef PICO_FLASH_BANK_TOTAL_SIZE
#define PICO_FLASH_BANK_TOTAL_SIZE (FLASH_SECTOR_SIZE * 2u)
#endif

#ifndef PICO_FLASH_BANK_STORAGE_OFFSET
#if PICO_RP2350 && PICO_RP2350_A2_SUPPORTED 
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE - PICO_FLASH_BANK_TOTAL_SIZE - FLASH_SECTOR_SIZE)
#else
#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - PICO_FLASH_BANK_TOTAL_SIZE - FLASH_SECTOR_SIZE)
#endif
#else
#define FLASH_TARGET_OFFSET PICO_FLASH_BANK_STORAGE_OFFSET
#endif

// RP2350 A/B images may be address-translated at runtime. Reading a separate
// data partition through a raw XIP pointer can fault when that physical flash
// range is not present in the running image's translated XIP window. Use the
// boot ROM's checked storage-address flash API and parse this RAM copy instead.
static uint8_t credential_store[FLASH_SECTOR_SIZE];

// Function prototypes
static bool save_credentials(const char *new_ssid, const char *new_password);
static bool write_credentials_store(void);
static void read_credentials(void);

static void attempt_wifi_connection(void);

static const char *connect_from_saved_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);
static const char *remove_saved_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);
static const char *clear_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);
static const char *test_emergency_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);
static const char *test_load_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);
static const char *cancel_test_cgi_handler(int iIndex, int iNumParams, char *pcParam[], char *pcValue[]);

static u16_t ssi_handler(int iIndex, char *pcInsert, int iInsertLen
#if LWIP_HTTPD_SSI_MULTIPART
    , uint16_t current_tag_part, uint16_t *next_tag_part
#endif
);

static tCGI cgi_handlers[] = {
    { "/connect_from_saved.cgi", connect_from_saved_cgi_handler},
    { "/remove_saved.cgi", remove_saved_cgi_handler},
    { "/clear.cgi", clear_cgi_handler},
    { "/test_emergency.cgi", test_emergency_cgi_handler},
    { "/test_load.cgi", test_load_cgi_handler},
    { "/cancel_test.cgi", cancel_test_cgi_handler}
};

// Be aware of LWIP_HTTPD_MAX_TAG_NAME_LEN
static const char *ssi_tags[] = {
    "wifilist",
    "ssid",
    "password",
    "version",
    "ipaddr",
    "hostname",
    "lat",
    "lon",
    "radius",
    "bright",
    "basis",
    "refresh",
    "timing",
    "watch"
};

static void start_http_server(void) {
    if (http_started) return;
    cyw43_arch_lwip_begin();
    httpd_init();
    http_set_cgi_handlers(cgi_handlers, LWIP_ARRAYSIZE(cgi_handlers));
    http_set_ssi_handler(ssi_handler, ssi_tags, LWIP_ARRAYSIZE(ssi_tags));
    cyw43_arch_lwip_end();
    http_started = true;
}

#define CYW43_STA_IS_ACTIVE(self) (((self)->itf_state >> CYW43_ITF_STA) & 1)
#define CYW43_AP_IS_ACTIVE(self) (((self)->itf_state >> CYW43_ITF_AP) & 1)

#define WIFI_CONNECT_TIME_S 20
#define WIFI_BOOT_CONNECT_ATTEMPTS 3
#define WIFI_RETRY_DELAY_MS 1500

#define AP_SSID "FlightWall-Setup"
#define AP_PASSWORD "flightwall"

// Note: This is called from an interrupt handler
void key_pressed_func(void *param) {
    int key = getchar_timeout_us(0); // get any pending key press but don't wait
    if (key == 'd' || key == 'D') {
        bool *exit = (bool*)param;
        *exit = true;
    }
}

int flightwall_wifi_connect_or_provision(flightwall_wifi_status_cb status_cb) {
    display_status = status_cb;
    // cyw43_arch_init() is intentionally performed before the HUB75 driver is
    // created so the radio reserves its PIO/DMA resources first.
    printf("Wi-Fi hardware initialised\n");

    // Never wait for USB keyboard input in the standalone appliance. Missing
    // or unusable saved credentials already lead to setup mode automatically.
    if (display_status) display_status("WIFI STARTING", "LOADING SETTINGS", "PLEASE WAIT");
    bool try_connect = true;

    // Loop completion is driven by the return code of the blocking connect
    // calls, not by polling link status: cyw43_arch_wifi_connect_timeout_ms
    // only returns success once an IP address has been obtained (CYW43_LINK_UP),
    // so its rc is an authoritative "connected" signal.
    bool connected = false;

    read_credentials();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], flightwall_hostname);
    if (try_connect) {
        // First, try to connect to network using one of the saved credentials
        // the idea is that once you have saved credentials wifi should "just work"
        for (int i = 0; i < num_credentials && !connected; i++) {
            for (int attempt = 1; attempt <= WIFI_BOOT_CONNECT_ATTEMPTS; attempt++) {
                char attempt_line[24];
                snprintf(attempt_line, sizeof(attempt_line), "ATTEMPT %d OF %d",
                         attempt, WIFI_BOOT_CONNECT_ATTEMPTS);

                // Re-enable STA for every attempt. A failed connection tears
                // the interface down so the CYW43 starts the next association
                // from a clean state (and so subsequent saved SSIDs work too).
                cyw43_arch_enable_sta_mode();
                netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
                printf("Trying STA \"%s\" at index %d, attempt %d/%d\n",
                       ssid_list[i], i, attempt, WIFI_BOOT_CONNECT_ATTEMPTS);
                if (display_status) display_status("CONNECTING WIFI", ssid_list[i], attempt_line);

                int rc = cyw43_arch_wifi_connect_timeout_ms(
                    ssid_list[i], password_list[i], CYW43_AUTH_WPA2_AES_PSK,
                    WIFI_CONNECT_TIME_S * 1000);
                if (rc == 0) {
                    printf("Connected.\n");
                    connected = true;
                    break;
                }

                printf("saved Wi-Fi attempt %d/%d failed rc=%d\n",
                       attempt, WIFI_BOOT_CONNECT_ATTEMPTS, rc);
                cyw43_arch_disable_sta_mode();
                if (attempt < WIFI_BOOT_CONNECT_ATTEMPTS) {
                    if (display_status) display_status("WIFI RETRY", ssid_list[i], "PLEASE WAIT");
                    sleep_ms(WIFI_RETRY_DELAY_MS);
                }
            }
        }
    }

    // Enter a loop until wifi is connected.
    // The DHCP server and DNS server structs are declared here so they outlive
    // every loop iteration - lwIP callbacks hold pointers to them. They (and
    // httpd) are initialised once, guarded by services_up.
    dhcp_server_t dhcp_server;
    dns_server_t dns_server;
    bool services_up = false;

    // The AP (and its DHCP/DNS/http services) is brought up once and kept up
    // for the whole provisioning session, so the user's browser stays connected
    // to the captive portal across failed connection attempts. The CYW43439 is
    // a single-radio part, so AP and STA share a channel: when STA associates,
    // the AP is dragged to the router's channel and notifies its client via a
    // CSA so the phone can follow. On a *successful* connect we tear the AP down
    // so the device settles cleanly onto the STA channel.
    while(!connected) {
        // Bring up the AP + services once, at the start (neither itf active yet).
        if (!CYW43_STA_IS_ACTIVE(&cyw43_state) && !CYW43_AP_IS_ACTIVE(&cyw43_state) && !services_up) {
            if (display_status) display_status("WIFI SETUP REQUIRED", "STARTING ACCESS PT", "PLEASE WAIT");
            cyw43_arch_enable_ap_mode(AP_SSID, AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
            printf("\nReady, running web server at %s\n", ip4addr_ntoa(netif_ip4_addr(netif_list)));
            printf("Connect to ssid \"%s\" with password \"%s\"\n", AP_SSID, AP_PASSWORD);
            if (display_status) display_status("JOIN FLIGHTWALL-SETUP", "PASS: FLIGHTWALL", "OPEN 192.168.4.1");

            #if LWIP_IPV6
            #define IP(x) ((x).u_addr.ip4)
            #else
            #define IP(x) (x)
            #endif

            ip4_addr_t mask;
            ip4_addr_t gw;
            IP(gw).addr = PP_HTONL(CYW43_DEFAULT_IP_AP_ADDRESS);
            IP(mask).addr = PP_HTONL(CYW43_DEFAULT_IP_MASK);

            #undef IP

            dhcp_server_init(&dhcp_server, &cyw43_state.netif[CYW43_ITF_AP], &gw, &mask);
            dns_server_init(&dns_server, &cyw43_state.netif[CYW43_ITF_AP], &gw);

            // start http server
            wifi_connected_time = get_absolute_time();
            start_http_server();

            services_up = true;
        }

        if (connect_requested) {
            // The user submitted credentials (via a CGI handler). Attempt the
            // connect here on the main loop, leaving the AP up so the portal
            // stays reachable. STA is enabled alongside the running AP.
            connect_requested = false;
            cyw43_arch_enable_sta_mode();
            // With both interfaces up, lwIP needs a default route for traffic
            // that isn't on the AP subnet (i.e. the join to the router). Make
            // the STA netif the default so the connect can route out.
            netif_set_default(&cyw43_state.netif[CYW43_ITF_STA]);
            printf("Trying to connect with STA \"%s\"\n", ssid);
            if (display_status) display_status("TRYING NETWORK", ssid, "PLEASE WAIT");
            int rc = cyw43_arch_wifi_connect_timeout_ms(ssid, password, CYW43_AUTH_WPA2_AES_PSK, WIFI_CONNECT_TIME_S * 1000);
            if (rc) {
                printf("failed to connect with credentials rc=%d\n", rc);
                if (display_status) display_status("WIFI FAILED", "REOPEN SETUP PAGE", "CHECK PASSWORD");
                // Leave the AP up; just drop the failed STA attempt and restore
                // the AP as the default netif so the portal keeps working.
                cyw43_arch_disable_sta_mode();
                netif_set_default(&cyw43_state.netif[CYW43_ITF_AP]);
            } else {
                printf("Connected.\n");
                // Save only after the router accepted the submitted password.
                if (display_status) display_status("SAVING NETWORK", ssid, "PLEASE WAIT");
                if (!save_credentials(ssid, password)) {
                    if (display_status) display_status("SAVE FAILED", "WIFI WORKS FOR NOW", "SETUP AFTER REBOOT");
                    sleep_ms(2500);
                }
                // Success: shut down the provisioning services. The DHCP/DNS
                // server UDP PCBs are bound to ports 67/53 independent of the AP
                // netif, so just disabling AP mode leaves them listening and
                // still answering requests from any lingering AP-side clients.
                // Deinit them explicitly (frees the PCBs / unbinds the ports),
                // then drop the AP netif. httpd has no deinit in the lwIP API,
                // but with the AP down and STA-only routing it is no longer
                // reachable on the AP subnet.
                cyw43_arch_lwip_begin();
                dns_server_deinit(&dns_server);
                dhcp_server_deinit(&dhcp_server);
                cyw43_arch_lwip_end();
                services_up = false;

                cyw43_arch_disable_ap_mode();
                connected = true;
            }
        }

        if (!connected) {
            cyw43_arch_poll();
            cyw43_arch_wait_for_work_until(make_timeout_time_ms(1000));
        }
    }
    printf("Finished provisioning credentials. status=%d\n", cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA));
    // On a normal boot with saved credentials we never entered setup mode, so
    // start the same status/configuration web server now on the LAN interface.
    start_http_server();
    runtime_online = true;
    // Do not submit another display frame here. main() immediately starts the
    // OTA service and presents the final ONLINE/IP status screen. Back-to-back
    // update_bgr() calls can overlap the HUB75 driver's asynchronous DMA
    // conversion and visually mix the two frames.
    return 0;
}

const char *flightwall_wifi_ip_address(void) {
    return ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]));
}

// Functions for saving and reading credentials from flash
// Saves (new_ssid, new_password). If an entry with the same ssid already exists,
// its password is updated in place (preserving list order); otherwise the entry
// is appended. Passing "" for new_ssid is treated as the "initialise empty
// store" case. Serializes from the in-memory ssid_list/password_list, so
// read_credentials must have populated them (it runs at boot and after each save).
static bool save_credentials(const char *new_ssid, const char *new_password) {
    // Special case: clear the in-memory store before persisting it.
    if (new_ssid[0] == '\0') {
        num_credentials = 0;
        memset(ssid_list, 0, sizeof(ssid_list));
        memset(password_list, 0, sizeof(password_list));
    } else {
        // Find an existing entry with the same ssid.
        int match = -1;
        for (int i = 0; i < num_credentials; i++) {
            if (strcmp(ssid_list[i], new_ssid) == 0) {
                match = i;
                break;
            }
        }

        // Update in place, or append, in the in-memory lists.
        if (match >= 0) {
            // overwrite password only; ssid and position unchanged
            strncpy(password_list[match], new_password, sizeof(password_list[0]) - 1);
            password_list[match][sizeof(password_list[0]) - 1] = '\0';
        } else if (num_credentials < MAX_CREDENTIALS) {
            int i = num_credentials;
            strncpy(ssid_list[i], new_ssid, sizeof(ssid_list[0]) - 1);
            ssid_list[i][sizeof(ssid_list[0]) - 1] = '\0';
            strncpy(password_list[i], new_password, sizeof(password_list[0]) - 1);
            password_list[i][sizeof(password_list[0]) - 1] = '\0';
            num_credentials++;
        } else {
            return false;
        }
    }

    return write_credentials_store();
}

static bool write_credentials_store(void) {
    uint8_t flash_data[FLASH_SECTOR_SIZE] = {0};
    uint count = 0;
    uint pos = 2;  // byte 0 reserved, byte 1 holds the count
    for (int i = 0; i < num_credentials; i++) {
        uint sl = strlen(ssid_list[i]);
        uint pl = strlen(password_list[i]);
        if (pos + sl + 1 + pl + 1 > sizeof(flash_data)) return false;
        memcpy(&flash_data[pos], ssid_list[i], sl);     pos += sl;
        flash_data[pos++] = 0;
        memcpy(&flash_data[pos], password_list[i], pl); pos += pl;
        flash_data[pos++] = 0;
        count++;
    }
    flash_data[1] = (uint8_t)count;

    cflash_flags_t erase_flags = {
        .flags =
            (CFLASH_OP_VALUE_ERASE << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    int rc = rom_flash_op(erase_flags,
                          XIP_BASE + FLASH_TARGET_OFFSET,
                          FLASH_SECTOR_SIZE,
                          NULL);
    if (rc != BOOTROM_OK) {
        printf("credential store erase failed rc=%d\n", rc);
        return false;
    }

    cflash_flags_t program_flags = {
        .flags =
            (CFLASH_OP_VALUE_PROGRAM << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    rc = rom_flash_op(program_flags,
                      XIP_BASE + FLASH_TARGET_OFFSET,
                      sizeof(flash_data),
                      flash_data);
    if (rc != BOOTROM_OK) {
        printf("credential store program failed rc=%d\n", rc);
        return false;
    }
    printf("saved %u Wi-Fi credential(s)\n", flash_data[1]);
    return true;
}

static void read_credentials(void) {
    uint credential_count;

    cflash_flags_t read_flags = {
        .flags =
            (CFLASH_OP_VALUE_READ << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB)
    };
    const int read_rc = rom_flash_op(read_flags,
                                     XIP_BASE + FLASH_TARGET_OFFSET,
                                     sizeof(credential_store),
                                     credential_store);
    if (read_rc != BOOTROM_OK) {
        // A missing/unreadable store must never prevent setup mode. Treat it
        // as empty so the access point can still start and repair the device.
        printf("credential store read failed rc=%d; using empty store\n", read_rc);
        memset(credential_store, 0xff, sizeof(credential_store));
    }

    // An erased settings sector is already a valid empty credential store.
    // Do not perform a flash write during first boot: the HUB75 refresh engine
    // is active on core 1, and there is nothing useful to persist yet.
    if (credential_store[0] == 255) {
        num_credentials = 0;
        memset(ssid_list, 0, sizeof(ssid_list));
        memset(password_list, 0, sizeof(password_list));
        printf("credential store is empty\n");
        return;
    }

    // second byte saves credential count
    credential_count = credential_store[1];
    // clamp to capacity: a corrupt/unexpected count byte must not overflow the
    // parse arrays below (we index t_*_list[space_count / 2])
    if (credential_count > MAX_CREDENTIALS) {
        credential_count = MAX_CREDENTIALS;
    }
    num_credentials = credential_count;
    printf("read_credentials count=%d\n", num_credentials);

    // initialise temporary ssid and password lists (zeroed to ensure null termination)
    char t_ssid_list[MAX_CREDENTIALS][MAX_SSID_LEN + 1] = {0};
    char t_password_list[MAX_CREDENTIALS][MAX_PASSWORD_LEN + 1] = {0};

    uint space_count = 0;
    uint start_index = 1;

    for (uint i = 2; i < FLASH_SECTOR_SIZE; i++) {
        if (space_count >= 2*credential_count) {
            break;
        } else if (credential_store[i] == 0) {
            space_count++;
            start_index = i;
        } else if (credential_store[i] != 0 && space_count % 2 == 0) {
            // there is a char, and even space count. So we are reading a ssid
            t_ssid_list[(int) space_count / 2][i - start_index - 1] = credential_store[i];
        } else if (credential_store[i] != 0 && space_count % 2 == 1) {
            // there is a char and odd space count, so reading password
            t_password_list[(int) space_count / 2][i - start_index - 1] = credential_store[i];
        } 
    }
    
    // update global ssid and password lists
    memset(ssid_list, 0, sizeof(ssid_list));
    memcpy(ssid_list, t_ssid_list, sizeof(t_ssid_list));

    memset(password_list, 0, sizeof(password_list));
    memcpy(password_list, t_password_list, sizeof(t_password_list));
}

static void attempt_wifi_connection(void) {
    // Signal the main loop to do the actual connect. We deliberately do not
    // change radio mode here: this runs in lwIP/network context, and all
    // cyw43 mode transitions are owned by the main loop where polling happens.
    connect_requested = true;
}

// Decodes application/x-www-form-urlencoded text in place.
// '+' -> ' ', "%XX" -> byte. dst and src may be the same buffer.
// Trailing spaces are stripped from the result.
static void url_decode(char *dst, const char *src, size_t dst_size) {
    size_t di = 0;
    while (*src && di + 1 < dst_size) {
        char c = *src;
        if (c == '+') {
            dst[di++] = ' ';
            src++;
        } else if (c == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            int hi = src[1] <= '9' ? src[1] - '0' : (src[1] | 0x20) - 'a' + 10;
            int lo = src[2] <= '9' ? src[2] - '0' : (src[2] | 0x20) - 'a' + 10;
            dst[di++] = (char)((hi << 4) | lo);
            src += 3;
        } else {
            dst[di++] = c;
            src++;
        }
    }
    // strip trailing spaces
    while (di > 0 && dst[di - 1] == ' ') {
        di--;
    }
    dst[di] = '\0';
}

// Returns the value of the named CGI parameter, or NULL if not present.
static const char *cgi_param(const char *name, int iNumParams, char *pcParam[], char *pcValue[]) {
    for (int i = 0; i < iNumParams; i++) {
        if (pcParam[i] != NULL && strcmp(pcParam[i], name) == 0) {
            return pcValue[i];
        }
    }
    return NULL;
}

// Extracts the raw (still url-encoded) value of a form parameter from the POST
// body pbuf. Returns a pointer into value_buf (null-terminated) or NULL if the
// parameter is not found / does not fit. Follows the pico-examples httpd pattern.
static char *post_param_value(struct pbuf *p, const char *param_name, char *value_buf, size_t value_buf_len) {
    size_t param_len = strlen(param_name);
    u16_t param_pos = pbuf_memfind(p, param_name, param_len, 0);
    if (param_pos == 0xFFFF) {
        return NULL;
    }
    u16_t value_pos = param_pos + param_len;
    // value runs until the next '&' or the end of the body
    u16_t amp_pos = pbuf_memfind(p, "&", 1, value_pos);
    u16_t value_len = (amp_pos != 0xFFFF) ? (amp_pos - value_pos)
                                          : (p->tot_len - value_pos);
    if (value_len >= value_buf_len) {
        return NULL;
    }
    if (value_len == 0) {
        value_buf[0] = '\0';
        return value_buf;
    }
    char *result = (char *)pbuf_get_contiguous(p, value_buf, value_buf_len, value_len, value_pos);
    if (result) {
        result[value_len] = '\0';
    }
    return result;
}

// Only one POST is handled at a time; track which connection owns it.
static void *current_post_connection;
typedef enum {
    POST_NONE,
    POST_CREDENTIALS,
    POST_SETTINGS,
    POST_FIRMWARE
} post_kind_t;
static post_kind_t current_post_kind;
// Set true once a POST body has been successfully decoded into ssid/password,
// so httpd_post_finished knows to kick off a connection attempt.
static bool post_credentials_ok;
static bool post_settings_ok;

err_t httpd_post_begin(void *connection, const char *uri, __unused const char *http_request,
                       __unused u16_t http_request_len, __unused int content_len, char *response_uri,
                       u16_t response_uri_len, u8_t *post_auto_wnd) {
    if (strcmp(uri, "/credentials.cgi") == 0) {
        current_post_connection = connection;
        current_post_kind = POST_CREDENTIALS;
        post_credentials_ok = false;
        // default redirect; overwritten on success in httpd_post_finished
        snprintf(response_uri, response_uri_len, "/index.shtml");
        *post_auto_wnd = 1;
        return ERR_OK;
    }
    if (strcmp(uri, "/firmware.cgi") == 0) {
        current_post_connection = connection;
        current_post_kind = POST_FIRMWARE;
        post_credentials_ok = flightwall_ota_http_begin((size_t)content_len);
        snprintf(response_uri, response_uri_len, "/update-failed.html");
        *post_auto_wnd = 1;
        return ERR_OK;
    }
    if (strcmp(uri, "/settings.cgi") == 0) {
        current_post_connection = connection;
        current_post_kind = POST_SETTINGS;
        post_settings_ok = false;
        snprintf(response_uri, response_uri_len, "/settings-failed.html");
        *post_auto_wnd = 1;
        return ERR_OK;
    }
    return ERR_VAL;
}

err_t httpd_post_receive_data(void *connection, struct pbuf *p) {
    err_t ret = ERR_VAL;
    if (connection == current_post_connection && current_post_kind == POST_FIRMWARE) {
        ret = ERR_OK;
        if (post_credentials_ok) {
            for (struct pbuf *q = p; q != NULL; q = q->next) {
                if (!flightwall_ota_http_write((const uint8_t *)q->payload, q->len)) {
                    post_credentials_ok = false;
                    break;
                }
            }
        }
    } else if (connection == current_post_connection && current_post_kind == POST_SETTINGS) {
        char lat_buf[24], lon_buf[24], radius_buf[12], brightness_buf[12], basis_buf[12], refresh_buf[12], display_buf[12];
        char watch_buf[384];
        char *lat = post_param_value(p, "latitude=", lat_buf, sizeof(lat_buf));
        char *lon = post_param_value(p, "longitude=", lon_buf, sizeof(lon_buf));
        char *radius = post_param_value(p, "radius=", radius_buf, sizeof(radius_buf));
        char *brightness = post_param_value(p, "brightness=", brightness_buf, sizeof(brightness_buf));
        char *basis = post_param_value(p, "paneloutput=", basis_buf, sizeof(basis_buf));
        char *refresh = post_param_value(p, "refresh=", refresh_buf, sizeof(refresh_buf));
        char *display = post_param_value(p, "displaytime=", display_buf, sizeof(display_buf));
        char *watch = post_param_value(p, "watchlist=", watch_buf, sizeof(watch_buf));
        if (lat && lon && radius && brightness && basis && refresh && display && watch) {
            url_decode(lat_buf, lat, sizeof(lat_buf));
            url_decode(lon_buf, lon, sizeof(lon_buf));
            url_decode(radius_buf, radius, sizeof(radius_buf));
            url_decode(brightness_buf, brightness, sizeof(brightness_buf));
            url_decode(basis_buf, basis, sizeof(basis_buf));
            url_decode(refresh_buf, refresh, sizeof(refresh_buf));
            url_decode(display_buf, display, sizeof(display_buf));
            url_decode(watch_buf, watch, sizeof(watch_buf));
            char *lat_end, *lon_end, *radius_end, *brightness_end, *basis_end, *refresh_end, *display_end;
            float latitude = strtof(lat_buf, &lat_end);
            float longitude = strtof(lon_buf, &lon_end);
            unsigned long radius_value = strtoul(radius_buf, &radius_end, 10);
            unsigned long brightness_value = strtoul(brightness_buf, &brightness_end, 10);
            unsigned long basis_value = strtoul(basis_buf, &basis_end, 10);
            unsigned long refresh_value = strtoul(refresh_buf, &refresh_end, 10);
            unsigned long display_value = strtoul(display_buf, &display_end, 10);
            if (*lat_end == '\0' && *lon_end == '\0' && *radius_end == '\0' &&
                *brightness_end == '\0' && *basis_end == '\0' && *refresh_end == '\0' &&
                *display_end == '\0') {
                post_settings_ok = flightwall_settings_update(
                    latitude, longitude, (unsigned)radius_value,
                    (unsigned)brightness_value, (unsigned)basis_value,
                    (unsigned)refresh_value, (unsigned)display_value, watch_buf);
            }
        }
        ret = ERR_OK;
    } else if (connection == current_post_connection && current_post_kind == POST_CREDENTIALS) {
        // For a small two-field form the body arrives in a single pbuf, so we
        // parse it directly here. post_param_value searches this pbuf only.
        char ssid_enc[sizeof(ssid)];
        char password_enc[sizeof(password)];

        char *s = post_param_value(p, "ssid=", ssid_enc, sizeof(ssid_enc));
        char *pw = post_param_value(p, "password=", password_enc, sizeof(password_enc));

        if (s != NULL && pw != NULL) {
            url_decode(ssid, s, sizeof(ssid));
            url_decode(password, pw, sizeof(password));
            printf("Received credentials for SSID: %s\n", ssid);
            post_credentials_ok = true;
            ret = ERR_OK;
        } else {
            printf("missing ssid/password in POST body\n");
        }
    }
    pbuf_free(p);
    return ret;
}

void httpd_post_finished(void *connection, char *response_uri, u16_t response_uri_len) {
    if (connection == current_post_connection && current_post_kind == POST_FIRMWARE) {
        bool update_ok = post_credentials_ok && flightwall_ota_http_finish();
        snprintf(response_uri, response_uri_len,
                 update_ok ? "/update-ok.html" : "/update-failed.html");
    } else if (connection == current_post_connection && current_post_kind == POST_SETTINGS) {
        snprintf(response_uri, response_uri_len,
                 post_settings_ok ? "/settings-saved.html" : "/settings-failed.html");
    } else if (connection == current_post_connection && current_post_kind == POST_CREDENTIALS) {
        snprintf(response_uri, response_uri_len, "/index.shtml");
        if (post_credentials_ok) {
            if (runtime_online) {
                // On the normal LAN page, preserve the current connection and
                // add/update this network for subsequent boots.
                (void)save_credentials(ssid, password);
            } else {
                // In captive-portal setup mode, validate by connecting first;
                // the provisioning loop saves only after association succeeds.
                attempt_wifi_connection();
            }
        }
    }
    current_post_connection = NULL;
    current_post_kind = POST_NONE;
}

static const char *connect_from_saved_cgi_handler(__unused int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    printf("connect_from_saved_cgi_handler called\n");

    const char *index_str = cgi_param("index", iNumParams, pcParam, pcValue);
    if (num_credentials == 0 || index_str == NULL) {
        printf("no saved network selected\n");
        return "/index.shtml";
    }

    int idx = atoi(index_str);
    if (idx < 0 || idx >= num_credentials) {
        printf("index %d out of range\n", idx);
        return "/index.shtml";
    }

    strncpy(ssid, ssid_list[idx], sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    strncpy(password, password_list[idx], sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';

    attempt_wifi_connection();
    return "/index.shtml";
}

static const char *clear_cgi_handler(__unused int iIndex, __unused int iNumParams, __unused char *pcParam[], __unused char *pcValue[]) {
    printf("clear_cgi_handler called\n");
    (void)save_credentials("", "");
    read_credentials();
    return "/index.shtml";
}

static const char *remove_saved_cgi_handler(__unused int iIndex, int iNumParams, char *pcParam[], char *pcValue[]) {
    const char *index_str = cgi_param("remove", iNumParams, pcParam, pcValue);
    if (!index_str) return "/index.shtml";

    int idx = atoi(index_str);
    if (idx < 0 || idx >= num_credentials) return "/index.shtml";

    printf("removing saved network at index %d: %s\n", idx, ssid_list[idx]);
    for (int i = idx; i + 1 < num_credentials; ++i) {
        memcpy(ssid_list[i], ssid_list[i + 1], sizeof(ssid_list[i]));
        memcpy(password_list[i], password_list[i + 1], sizeof(password_list[i]));
    }
    num_credentials--;
    memset(ssid_list[num_credentials], 0, sizeof(ssid_list[0]));
    memset(password_list[num_credentials], 0, sizeof(password_list[0]));
    (void)write_credentials_store();
    return "/index.shtml";
}

static const char *test_emergency_cgi_handler(__unused int iIndex, __unused int iNumParams, __unused char *pcParam[], __unused char *pcValue[]) {
    load_test_active = false;
    emergency_test_active = true;
    printf("emergency display test enabled\n");
    return "/index.shtml";
}

static const char *test_load_cgi_handler(__unused int iIndex, __unused int iNumParams, __unused char *pcParam[], __unused char *pcValue[]) {
    emergency_test_active = false;
    load_test_active = true;
    printf("full-panel load test enabled\n");
    return "/index.shtml";
}

static const char *cancel_test_cgi_handler(__unused int iIndex, __unused int iNumParams, __unused char *pcParam[], __unused char *pcValue[]) {
    emergency_test_active = false;
    load_test_active = false;
    printf("display test cancelled\n");
    return "/index.shtml";
}

bool flightwall_emergency_test_active(void) {
    return emergency_test_active;
}

bool flightwall_load_test_active(void) {
    return load_test_active;
}

void flightwall_load_test_complete(void) {
    load_test_active = false;
}

// Appends s to dst with HTML special chars escaped. Writes at most dst_size-1
// chars plus a null terminator. Returns number of chars written (excl. null).
static int html_escape(char *dst, size_t dst_size, const char *s) {
    size_t di = 0;
    for (; *s && di + 1 < dst_size; s++) {
        const char *rep;
        switch (*s) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default:
                dst[di++] = *s;
                continue;
        }
        size_t rlen = strlen(rep);
        if (di + rlen >= dst_size) break;   // not enough room for the whole entity
        memcpy(dst + di, rep, rlen);
        di += rlen;
    }
    dst[di] = '\0';
    return (int)di;
}

// Note that the buffer size is limited by LWIP_HTTPD_MAX_TAG_INSERT_LEN, so use LWIP_HTTPD_SSI_MULTIPART to return larger amounts of data
static u16_t ssi_handler(int iIndex, char *pcInsert, int iInsertLen
#if LWIP_HTTPD_SSI_MULTIPART
    , uint16_t current_tag_part, uint16_t *next_tag_part
#endif
) {
    int printed = 0;
    switch (iIndex) {
        case 0: { // wifilist
            if (num_credentials == 0) {
                printed = snprintf(pcInsert, iInsertLen, "<p>No saved networks</p>");
                break;
            }
            int i = current_tag_part;
            char esc[sizeof(ssid_list[0]) * 6];   // worst case: every char -> &quot; (6 chars)
            html_escape(esc, sizeof(esc), ssid_list[i]);
            printed = snprintf(pcInsert, iInsertLen,
                "<div class=n><label><input type=radio name=index value=%i%s> %s</label>"
                "<a class=delete-link href=/remove_saved.cgi?remove=%i>Delete</a></div>",
                i, (i == 0 ? " checked" : ""), esc, i);
            if (i + 1 < num_credentials) {
                *next_tag_part = i + 1;
            }
            break;
        }
        case 1: { // ssid
            char esc[sizeof(ssid) * 6];
            html_escape(esc, sizeof(esc), ssid);
            printed = snprintf(pcInsert, iInsertLen, "%s", esc);
            break;
        }
        case 2: { // password
            char esc[sizeof(password) * 6];
            html_escape(esc, sizeof(esc), password);
            printed = snprintf(pcInsert, iInsertLen, "%s", esc);
            break;
        }
        case 3: { // version
            printed = snprintf(pcInsert, iInsertLen, "%s", FLIGHTWALL_VERSION);
            break;
        }
        case 4: { // ipaddr
            printed = snprintf(pcInsert, iInsertLen, "%s", flightwall_wifi_ip_address());
            break;
        }
        case 5: { // hostname
            printed = snprintf(pcInsert, iInsertLen, "%s", flightwall_hostname);
            break;
        }
        case 6: { // latitude
            printed = snprintf(pcInsert, iInsertLen, "%.4f", flightwall_settings_get()->latitude);
            break;
        }
        case 7: { // longitude
            printed = snprintf(pcInsert, iInsertLen, "%.4f", flightwall_settings_get()->longitude);
            break;
        }
        case 8: { // radius
            printed = snprintf(pcInsert, iInsertLen, "%u", flightwall_settings_get()->radius_nm);
            break;
        }
        case 9: { // brightness
            printed = snprintf(pcInsert, iInsertLen, "%u", flightwall_settings_get()->brightness_percent);
            break;
        }
        case 10: { // panel output basis options
            static const unsigned values[] = {8, 10, 12, 16, 24, 32, 48, 64};
            static const char *labels[] = {
                "Standard", "Brighter", "Bright", "Extra bright", "High", "Very high",
                "Experimental", "Maximum"
            };
            const unsigned selected = flightwall_settings_get()->panel_output_basis;
            int i = current_tag_part;
            printed = snprintf(pcInsert, iInsertLen, "<option value=%u%s>%u - %s</option>",
                               values[i], selected == values[i] ? " selected" : "",
                               values[i], labels[i]);
            if (i + 1 < (int)LWIP_ARRAYSIZE(values)) *next_tag_part = i + 1;
            break;
        }
        case 11: { // refresh
            printed = snprintf(pcInsert, iInsertLen, "%u", flightwall_settings_get()->refresh_seconds);
            break;
        }
        case 12: { // display duration
            printed = snprintf(pcInsert, iInsertLen, "%u", flightwall_settings_get()->display_seconds);
            break;
        }
        case 13: { // watched aircraft
            printed = (int)flightwall_watchlist_format(pcInsert, (size_t)iInsertLen);
            break;
        }
        default: { // unknown tag
            printed = 0;
            break;
        }
    }
    return printed;
}
