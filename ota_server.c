/**
 * Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "pico/sha256.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "hardware/watchdog.h"
#include "hardware/structs/watchdog.h"
#include "boot/picobin.h"
#include "boot/picoboot.h"
#include "boot/uf2.h"

#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "ota_server.h"

#define TCP_PORT 4242
// #define DEBUG_printf(...) printf(__VA_ARGS__)
#define DEBUG_printf(...)
#define BUF_SIZE 2048
#define POLL_TIME_S 5

#define FLASH_SECTOR_ERASE_SIZE 4096u

typedef struct TCP_UPDATE_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    struct tcp_pcb *client_pcb;
    bool complete;
    __attribute__((aligned(4))) uint8_t buffer_sent[SHA256_RESULT_BYTES];
    __attribute__((aligned(4))) uint8_t buffer_recv[BUF_SIZE];
    int sent_len;
    int recv_len;
    int num_blocks;
    int blocks_done;
    uint32_t family_id;
    uint32_t flash_update;
    int32_t write_offset;
    uint32_t write_size;
    uint32_t highest_erased_sector;
    bool http_active;
    size_t http_expected;
    size_t http_received;
    uint64_t ready_after_us;
} TCP_UPDATE_SERVER_T;

typedef struct uf2_block uf2_block_t;
static TCP_UPDATE_SERVER_T *ota_state;
static char ota_last_error[160] = "none";
static char ota_boot_diag[96] = "none";
#define OTA_REBOOT_MAGIC 0x46574f54u
#define OTA_FAILURE_MAGIC 0x46574641u
#define OTA_STAGE_MAGIC 0x46575354u
#define OTA_STAGE_WATCHDOG_MS 30000u

enum {
    OTA_STAGE_NONE = 0,
    OTA_STAGE_DOWNLOAD = 1,
    OTA_STAGE_PARTITION = 2,
    OTA_STAGE_ERASE = 3,
    OTA_STAGE_PROGRAM = 4
};

enum {
    OTA_FAILURE_NONE = 0,
    OTA_FAILURE_UF2_INVALID = 1,
    OTA_FAILURE_UF2_FIRST_BLOCK = 2,
    OTA_FAILURE_PARTITION_LOOKUP = 3,
    OTA_FAILURE_PARTITION_RANGE = 4,
    OTA_FAILURE_UF2_SEQUENCE = 5,
    OTA_FAILURE_DEST_RANGE = 6,
    OTA_FAILURE_FLASH_ERASE = 7,
    OTA_FAILURE_FLASH_PROGRAM = 8,
    OTA_FAILURE_OTHER = 9
};

static void ota_stage_set(uint32_t stage, uint32_t block_no, uintptr_t address) {
    watchdog_hw->scratch[4] = OTA_STAGE_MAGIC;
    watchdog_hw->scratch[5] = stage;
    watchdog_hw->scratch[6] = block_no;
    watchdog_hw->scratch[7] = (uint32_t)address;
}

static void ota_stage_clear(void) {
    watchdog_hw->scratch[4] = 0;
    watchdog_hw->scratch[5] = 0;
    watchdog_hw->scratch[6] = 0;
    watchdog_hw->scratch[7] = 0;
}

static const char *ota_stage_text(uint32_t stage) {
    switch (stage) {
        case OTA_STAGE_DOWNLOAD: return "DOWNLOAD";
        case OTA_STAGE_PARTITION: return "PARTITION";
        case OTA_STAGE_ERASE: return "ERASE";
        case OTA_STAGE_PROGRAM: return "PROGRAM";
        default: return "UNKNOWN";
    }
}

static uint32_t ota_failure_code(void) {
    if (strncmp(ota_last_error, "UF2 INVALID", 11) == 0) return OTA_FAILURE_UF2_INVALID;
    if (strncmp(ota_last_error, "UF2 FIRST BLOCK", 15) == 0) return OTA_FAILURE_UF2_FIRST_BLOCK;
    if (strncmp(ota_last_error, "PARTITION LOOKUP", 16) == 0) return OTA_FAILURE_PARTITION_LOOKUP;
    if (strncmp(ota_last_error, "PARTITION RANGE", 15) == 0) return OTA_FAILURE_PARTITION_RANGE;
    if (strncmp(ota_last_error, "UF2 SEQUENCE", 12) == 0) return OTA_FAILURE_UF2_SEQUENCE;
    if (strncmp(ota_last_error, "DEST RANGE", 10) == 0) return OTA_FAILURE_DEST_RANGE;
    if (strncmp(ota_last_error, "FLASH ERASE", 11) == 0) return OTA_FAILURE_FLASH_ERASE;
    if (strncmp(ota_last_error, "FLASH PROGRAM", 13) == 0) return OTA_FAILURE_FLASH_PROGRAM;
    return OTA_FAILURE_OTHER;
}

static const char *ota_failure_text(uint32_t code) {
    switch (code) {
        case OTA_FAILURE_UF2_INVALID: return "LAST OTA FAIL: UF2 INVALID";
        case OTA_FAILURE_UF2_FIRST_BLOCK: return "LAST OTA FAIL: UF2 FIRST BLOCK";
        case OTA_FAILURE_PARTITION_LOOKUP: return "LAST OTA FAIL: PARTITION LOOKUP";
        case OTA_FAILURE_PARTITION_RANGE: return "LAST OTA FAIL: PARTITION RANGE";
        case OTA_FAILURE_UF2_SEQUENCE: return "LAST OTA FAIL: UF2 SEQUENCE";
        case OTA_FAILURE_DEST_RANGE: return "LAST OTA FAIL: DEST RANGE";
        case OTA_FAILURE_FLASH_ERASE: return "LAST OTA FAIL: FLASH ERASE";
        case OTA_FAILURE_FLASH_PROGRAM: return "LAST OTA FAIL: FLASH PROGRAM";
        default: return "LAST OTA FAIL: OTHER";
    }
}

static void ota_record_error(const char *stage, int rc, uint32_t block_no, uintptr_t destination) {
    snprintf(ota_last_error, sizeof(ota_last_error),
             "%s rc=%d block=%lu addr=0x%08lx",
             stage ? stage : "unknown", rc,
             (unsigned long)block_no, (unsigned long)destination);
    printf("OTA staging failure: %s\n", ota_last_error);
}

static TCP_UPDATE_SERVER_T* tcp_update_server_init(void) {
    TCP_UPDATE_SERVER_T *state = calloc(1, sizeof(TCP_UPDATE_SERVER_T));
    if (!state) {
        DEBUG_printf("failed to allocate state\n");
        return NULL;
    }
    return state;
}


static __attribute__((aligned(4))) uint8_t workarea[4 * 1024];

static void tcp_update_server_reset_transfer(TCP_UPDATE_SERVER_T *state) {
    state->complete = false;
    state->recv_len = 0;
    state->sent_len = 0;
    state->num_blocks = 0;
    state->blocks_done = 0;
    state->family_id = 0;
    state->flash_update = 0;
    state->write_offset = 0;
    state->write_size = 0;
    state->highest_erased_sector = UINT32_MAX;
    state->http_active = false;
    state->http_expected = 0;
    state->http_received = 0;
    state->ready_after_us = 0;
}

static err_t tcp_update_client_close(TCP_UPDATE_SERVER_T *state) {
    err_t err = ERR_OK;
    if (state->client_pcb != NULL) {
        tcp_arg(state->client_pcb, NULL);
        tcp_poll(state->client_pcb, NULL, 0);
        tcp_sent(state->client_pcb, NULL);
        tcp_recv(state->client_pcb, NULL);
        tcp_err(state->client_pcb, NULL);
        err = tcp_close(state->client_pcb);
        if (err != ERR_OK) {
            tcp_abort(state->client_pcb);
            err = ERR_ABRT;
        }
        state->client_pcb = NULL;
    }
    return err;
}

static err_t tcp_update_server_close(void *arg) {
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;
    err_t err = tcp_update_client_close(state);
    if (state->server_pcb) {
        tcp_arg(state->server_pcb, NULL);
        tcp_close(state->server_pcb);
        state->server_pcb = NULL;
    }
    return err;
}

static err_t tcp_update_server_result(void *arg, int status) {
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;
    if (status == 0) {
        DEBUG_printf("test success\n");
        state->complete = true;
        state->ready_after_us = time_us_64() + 500000u;
        return tcp_update_server_close(arg);
    } else {
        DEBUG_printf("test failed %d\n", status);
        // A port probe, abandoned connection, or malformed upload is not an
        // update. Close only that client, discard its transfer state, and keep
        // the listener available for the next genuine uploader connection.
        err_t close_err = tcp_update_client_close(state);
        tcp_update_server_reset_transfer(state);
        return close_err;
    }
}

static err_t tcp_update_server_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) {
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;
    DEBUG_printf("tcp_update_server_sent %u\n", len);
    state->sent_len += len;

    if (state->sent_len >= SHA256_RESULT_BYTES) {

        // We should get the data back from the client
        state->recv_len = 0;
        DEBUG_printf("Waiting for buffer from client\n");
    }

    return ERR_OK;
}

err_t tcp_update_server_send_data(void *arg, struct tcp_pcb *tpcb)
{
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;

    state->sent_len = 0;
    DEBUG_printf("Writing %ld bytes to client\n", SHA256_RESULT_BYTES);
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    cyw43_arch_lwip_check();
    err_t err = tcp_write(tpcb, state->buffer_sent, SHA256_RESULT_BYTES, TCP_WRITE_FLAG_COPY);
    if (err != ERR_OK) {
        DEBUG_printf("Failed to write data %d\n", err);
        return tcp_update_server_result(arg, -1);
    }
    return ERR_OK;
}

// Validate and write one complete 2 KiB group of four UF2 blocks. Returns 1
// when the declared image is complete, 0 when more blocks are required, and
// -1 for malformed/out-of-range data or a flash failure.
static int process_uf2_buffer(TCP_UPDATE_SERVER_T *state) {
    for (int i = 0; i < BUF_SIZE / (int)sizeof(uf2_block_t); i++) {
        uf2_block_t *block = (uf2_block_t *)(state->buffer_recv + i * sizeof(uf2_block_t));

        if (block->magic_start0 != UF2_MAGIC_START0 ||
            block->magic_start1 != UF2_MAGIC_START1 ||
            block->magic_end != UF2_MAGIC_END ||
            block->payload_size != 256 ||
            block->num_blocks == 0 ||
            block->block_no >= block->num_blocks) {
            ota_record_error("UF2 INVALID", -1, block->block_no, block->target_addr);
            return -1;
        }

        if (state->num_blocks == 0) {
            if (block->block_no != 0) {
                ota_record_error("UF2 FIRST BLOCK", -1, block->block_no, block->target_addr);
                return -1;
            }
            state->num_blocks = block->num_blocks;
            state->family_id = block->file_size;

            resident_partition_t target_partition = {0};
            printf("OTA USB: BLOCK %lu FIRST target=0x%08lx family=0x%08lx total=%lu\n",
                   (unsigned long)block->block_no,
                   (unsigned long)block->target_addr,
                   (unsigned long)state->family_id,
                   (unsigned long)block->num_blocks);
            stdio_flush();
            ota_stage_set(OTA_STAGE_PARTITION, block->block_no, block->target_addr);
            watchdog_update();
            printf("OTA USB: PARTITION LOOKUP START\n");
            stdio_flush();
            rom_flash_flush_cache();
            int partition_rc = rom_get_uf2_target_partition(
                workarea, sizeof(workarea), state->family_id, &target_partition);
            printf("OTA USB: PARTITION LOOKUP DONE rc=%d\n", partition_rc);
            stdio_flush();
            // This ROM call returns a non-negative target partition index on
            // success, not necessarily BOOTROM_OK (zero). Boot ROM failures
            // use negative BOOTROM_ERROR_* values.
            if (partition_rc < 0) {
                ota_record_error("PARTITION LOOKUP", partition_rc, block->block_no, block->target_addr);
                return -1;
            }

            uint16_t first_sector =
                (target_partition.permissions_and_location & PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_BITS) >>
                PICOBIN_PARTITION_LOCATION_FIRST_SECTOR_LSB;
            uint16_t last_sector =
                (target_partition.permissions_and_location & PICOBIN_PARTITION_LOCATION_LAST_SECTOR_BITS) >>
                PICOBIN_PARTITION_LOCATION_LAST_SECTOR_LSB;
            if (last_sector < first_sector) {
                ota_record_error("PARTITION RANGE", -1, block->block_no, block->target_addr);
                return -1;
            }

            uint32_t code_start = first_sector * FLASH_SECTOR_ERASE_SIZE;
            uint32_t code_end = (last_sector + 1u) * FLASH_SECTOR_ERASE_SIZE;
            state->flash_update = code_start + XIP_BASE;
            state->write_offset = (int32_t)(code_start + XIP_BASE - block->target_addr);
            state->write_size = code_end - code_start;
            printf("OTA USB: TARGET PARTITION start=0x%08lx size=%lu offset=%ld\n",
                   (unsigned long)state->flash_update,
                   (unsigned long)state->write_size,
                   (long)state->write_offset);
            stdio_flush();
        }

        if (state->blocks_done != (int)block->block_no ||
            state->family_id != block->file_size ||
            state->num_blocks != (int)block->num_blocks) {
            ota_record_error("UF2 SEQUENCE", -1, block->block_no, block->target_addr);
            return -1;
        }

        uintptr_t destination = block->target_addr + state->write_offset;
        uintptr_t partition_start = state->flash_update;
        uintptr_t partition_end = partition_start + state->write_size;
        if (destination < partition_start ||
            destination + block->payload_size > partition_end) {
            ota_record_error("DEST RANGE", -1, block->block_no, destination);
            return -1;
        }

        cflash_flags_t flags;
        int ret;
        uint32_t destination_sector = (uint32_t)((destination - XIP_BASE) / FLASH_SECTOR_ERASE_SIZE);
        if (destination_sector != state->highest_erased_sector) {
            flags.flags =
                (CFLASH_OP_VALUE_ERASE << CFLASH_OP_LSB) |
                (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
                (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB);
            // Pico SDK 2.3.0's rom_flash_op() already uses flash_safe_execute()
            // internally. Do not wrap it in a second multicore lockout.
            ota_stage_set(OTA_STAGE_ERASE, block->block_no,
                          destination & ~(FLASH_SECTOR_ERASE_SIZE - 1u));
            watchdog_update();
            printf("OTA USB: ERASE START block=%lu addr=0x%08lx\n",
                   (unsigned long)block->block_no,
                   (unsigned long)(destination & ~(FLASH_SECTOR_ERASE_SIZE - 1u)));
            stdio_flush();
            ret = rom_flash_op(flags, destination & ~(FLASH_SECTOR_ERASE_SIZE - 1u),
                               FLASH_SECTOR_ERASE_SIZE, NULL);
            printf("OTA USB: ERASE DONE block=%lu rc=%d\n",
                   (unsigned long)block->block_no, ret);
            stdio_flush();
            if (ret != BOOTROM_OK) {
                ota_record_error("FLASH ERASE", ret, block->block_no,
                                 destination & ~(FLASH_SECTOR_ERASE_SIZE - 1u));
                return -1;
            }
            state->highest_erased_sector = destination_sector;
        }

        flags.flags =
            (CFLASH_OP_VALUE_PROGRAM << CFLASH_OP_LSB) |
            (CFLASH_SECLEVEL_VALUE_SECURE << CFLASH_SECLEVEL_LSB) |
            (CFLASH_ASPACE_VALUE_STORAGE << CFLASH_ASPACE_LSB);
        // rom_flash_op() performs its own flash-safe IRQ/core coordination.
        ota_stage_set(OTA_STAGE_PROGRAM, block->block_no, destination);
        watchdog_update();
        ret = rom_flash_op(flags, destination, block->payload_size, block->data);
        if (ret != BOOTROM_OK) {
            ota_record_error("FLASH PROGRAM", ret, block->block_no, destination);
            return -1;
        }

        state->blocks_done++;
        if (state->blocks_done == state->num_blocks) return 1;
    }
    return 0;
}

err_t tcp_update_server_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;
    if (!p) {
        return tcp_update_server_result(arg, -1);
    }
    // this method is callback from lwIP, so cyw43_arch_lwip_begin is not required, however you
    // can use this method to cause an assertion in debug mode, if this method is called when
    // cyw43_arch_lwip_begin IS needed
    cyw43_arch_lwip_check();
    if (p->tot_len > 0) {
        DEBUG_printf("tcp_update_server_recv %d/%d err %d\n", p->tot_len, state->recv_len, err);

        // Receive the buffer
        const uint16_t buffer_left = BUF_SIZE - state->recv_len;
        state->recv_len += pbuf_copy_partial(p, state->buffer_recv + state->recv_len,
                                             p->tot_len > buffer_left ? buffer_left : p->tot_len, 0);
        tcp_recved(tpcb, p->tot_len);
    }
    pbuf_free(p);

    // Have we have received the whole buffer
    if (state->recv_len == BUF_SIZE) {
        int process_result = process_uf2_buffer(state);
        if (process_result < 0) return tcp_update_server_result(arg, -1);
        if (process_result > 0) {
            tcp_update_server_result(arg, 0);
            return ERR_OK;
        }

        // Hash the received data
        pico_sha256_state_t sha_state;
        int rc = pico_sha256_start_blocking(&sha_state, SHA256_BIG_ENDIAN, true); // using some DMA system resources
        hard_assert(rc == PICO_OK);
        pico_sha256_update_blocking(&sha_state, (const uint8_t*)state->buffer_recv, sizeof(state->buffer_recv));

        // Get the result of the sha256 calculation
        sha256_result_t* result;
        result = (sha256_result_t*)state->buffer_sent;
        pico_sha256_finish(&sha_state, result);

        // Send another buffer
        return tcp_update_server_send_data(arg, state->client_pcb);
    }
    return ERR_OK;
}

static err_t tcp_update_server_poll(void *arg, struct tcp_pcb *tpcb) {
    DEBUG_printf("tcp_update_server_poll_fn\n");
    return tcp_update_server_result(arg, -1); // no response is an error?
}

static void tcp_update_server_err(void *arg, err_t err) {
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;
    // lwIP has already freed the PCB before invoking this callback.
    state->client_pcb = NULL;
    if (err != ERR_ABRT) {
        DEBUG_printf("tcp_client_err_fn %d\n", err);
        tcp_update_server_result(arg, err);
    }
}

static err_t tcp_update_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;
    if (err != ERR_OK || client_pcb == NULL) {
        DEBUG_printf("Failure in accept\n");
        tcp_update_server_result(arg, err);
        return ERR_VAL;
    }
    DEBUG_printf("Client connected\n");

    state->client_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_sent(client_pcb, tcp_update_server_sent);
    tcp_recv(client_pcb, tcp_update_server_recv);
    tcp_poll(client_pcb, tcp_update_server_poll, POLL_TIME_S * 2);
    tcp_err(client_pcb, tcp_update_server_err);

    return ERR_OK;
}

static bool tcp_update_server_open(void *arg) {
    TCP_UPDATE_SERVER_T *state = (TCP_UPDATE_SERVER_T*)arg;
    printf("Starting server at %s on port %u\n", ip4addr_ntoa(netif_ip4_addr(netif_list)), TCP_PORT);

    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) {
        DEBUG_printf("failed to create pcb\n");
        return false;
    }

    err_t err = tcp_bind(pcb, NULL, TCP_PORT);
    if (err) {
        DEBUG_printf("failed to bind to port %u\n", TCP_PORT);
        return false;
    }

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    if (!state->server_pcb) {
        DEBUG_printf("failed to listen\n");
        if (pcb) {
            tcp_close(pcb);
        }
        return false;
    }

    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_update_server_accept);

    return true;
}

void flightwall_ota_confirm_running_image(void) {
    uint32_t marker = watchdog_hw->scratch[0];
    uint32_t target = watchdog_hw->scratch[1];
    uint32_t failure_marker = watchdog_hw->scratch[2];
    uint32_t failure_code = watchdog_hw->scratch[3];
    uint32_t stage_marker = watchdog_hw->scratch[4];
    uint32_t stage = watchdog_hw->scratch[5];
    uint32_t stage_block = watchdog_hw->scratch[6];
    uint32_t stage_address = watchdog_hw->scratch[7];
#ifdef __riscv
    // Increased bootrom stack is required for some of the functions in this example
    bootrom_stack_t stack = {
        .base = malloc(0x400),
        .size = 0x400
    };
    rom_set_bootrom_stack(&stack);
#endif
    boot_info_t boot_info = {};
    int ret = rom_get_boot_info(&boot_info);
    printf("Boot partition was %d\n", boot_info.partition);

    if (rom_get_last_boot_type() == BOOT_TYPE_FLASH_UPDATE) {
        snprintf(ota_boot_diag, sizeof(ota_boot_diag), "UPDATE BOOT TARGET 0x%08lx", (unsigned long)target);
        printf("Someone updated into me\n");
        if (boot_info.reboot_params[0]) printf("Flash update base was %x\n", boot_info.reboot_params[0]);
        if (boot_info.tbyb_and_update_info) printf("Update info %x\n", boot_info.tbyb_and_update_info);
        ret = rom_explicit_buy(workarea, sizeof(workarea));
        if (ret) {
            printf("Buy returned %d\n", ret);
            snprintf(ota_boot_diag, sizeof(ota_boot_diag), "BUY FAILED rc=%d", ret);
        } else {
            snprintf(ota_boot_diag, sizeof(ota_boot_diag), "UPDATE BOOT COMMITTED");
            watchdog_hw->scratch[0] = 0;
            watchdog_hw->scratch[1] = 0;
            ota_stage_clear();
        }
        ret = rom_get_boot_info(&boot_info);
        if (boot_info.tbyb_and_update_info) printf("Update info now %x\n", boot_info.tbyb_and_update_info);
    } else if (failure_marker == OTA_FAILURE_MAGIC) {
        snprintf(ota_boot_diag, sizeof(ota_boot_diag), "%s", ota_failure_text(failure_code));
        watchdog_hw->scratch[2] = 0;
        watchdog_hw->scratch[3] = 0;
        ota_stage_clear();
    } else if (marker == OTA_REBOOT_MAGIC) {
        snprintf(ota_boot_diag, sizeof(ota_boot_diag), "ROLLBACK TARGET 0x%08lx", (unsigned long)target);
        watchdog_hw->scratch[0] = 0;
        watchdog_hw->scratch[1] = 0;
        ota_stage_clear();
    } else if (stage_marker == OTA_STAGE_MAGIC) {
        snprintf(ota_boot_diag, sizeof(ota_boot_diag),
                 "OTA WATCHDOG: %s B%lu 0x%08lx",
                 ota_stage_text(stage),
                 (unsigned long)stage_block,
                 (unsigned long)stage_address);
        ota_stage_clear();
    }
}

bool flightwall_ota_start(void) {
    if (ota_state) return true;
    ota_state = tcp_update_server_init();
    if (!ota_state) {
        return false;
    }
    if (!tcp_update_server_open(ota_state)) {
        tcp_update_server_result(ota_state, -1);
        free(ota_state);
        ota_state = NULL;
        return false;
    }
    return true;
}

bool flightwall_ota_http_begin(size_t content_length) {
    if (!ota_state || ota_state->complete || ota_state->client_pcb ||
        content_length < BUF_SIZE || content_length > (4u * 1024u * 1024u) ||
        (content_length % BUF_SIZE) != 0) {
        return false;
    }
    tcp_update_server_reset_transfer(ota_state);
    ota_state->http_active = true;
    ota_state->http_expected = content_length;
    printf("OTA USB: HTTP STAGING BEGIN bytes=%lu\n", (unsigned long)content_length);
    stdio_flush();

    // GitHub OTA is the only path that uses this HTTP staging entry point.
    // Arm a watchdog before the first ROM partition/flash call so a deadlock
    // automatically returns to the known-good image. Scratch registers retain
    // the last staging checkpoint across the watchdog reset for diagnosis.
    ota_stage_set(OTA_STAGE_DOWNLOAD, 0, 0);
    watchdog_enable(OTA_STAGE_WATCHDOG_MS, true);
    watchdog_update();
    return true;
}

bool flightwall_ota_http_write(const uint8_t *data, size_t length) {
    watchdog_update();
    if (!ota_state || !ota_state->http_active || !data ||
        ota_state->http_received + length > ota_state->http_expected) {
        return false;
    }

    while (length > 0) {
        size_t room = BUF_SIZE - (size_t)ota_state->recv_len;
        size_t take = length < room ? length : room;
        memcpy(ota_state->buffer_recv + ota_state->recv_len, data, take);
        ota_state->recv_len += (int)take;
        ota_state->http_received += take;
        data += take;
        length -= take;

        if (ota_state->recv_len == BUF_SIZE) {
            int result = process_uf2_buffer(ota_state);
            ota_state->recv_len = 0;
            if (result < 0) {
                tcp_update_server_reset_transfer(ota_state);
                return false;
            }
            if (result > 0) ota_state->complete = true;
        }
    }
    return true;
}

bool flightwall_ota_http_finish(void) {
    if (!ota_state || !ota_state->http_active) return false;
    bool valid = ota_state->complete &&
                 ota_state->http_received == ota_state->http_expected &&
                 ota_state->recv_len == 0 &&
                 ota_state->num_blocks > 0 &&
                 ota_state->blocks_done == ota_state->num_blocks &&
                 ota_state->flash_update != 0;
    ota_state->http_active = false;
    if (!valid) {
        tcp_update_server_reset_transfer(ota_state);
        return false;
    }
    // Stop accepting a simultaneous legacy TCP upload while the main loop
    // displays the restart message and switches to the new A/B image.
    tcp_update_server_close(ota_state);
    // Give lwIP time to deliver the HTTP success page before rebooting.
    ota_state->ready_after_us = time_us_64() + 2000000u;
    return true;
}

void flightwall_ota_http_abort(void) {
    if (!ota_state) return;
    tcp_update_server_reset_transfer(ota_state);
}

const char *flightwall_ota_last_error(void) {
    return ota_last_error;
}

const char *flightwall_ota_boot_diagnostic(void) {
    return ota_boot_diag;
}

void flightwall_ota_reboot_current(void) {
    printf("Rebooting current known-good image after OTA staging failure: %s\n", ota_last_error);
    watchdog_hw->scratch[2] = OTA_FAILURE_MAGIC;
    watchdog_hw->scratch[3] = ota_failure_code();
    cyw43_arch_disable_sta_mode();
    cyw43_arch_deinit();
    watchdog_reboot(0, 0, 100);
    while (true) tight_loop_contents();
}

bool flightwall_ota_update_ready(void) {
    return ota_state &&
           ota_state->complete &&
           ota_state->num_blocks > 0 &&
           ota_state->blocks_done == ota_state->num_blocks &&
           ota_state->flash_update != 0 &&
           time_us_64() >= ota_state->ready_after_us;
}

bool flightwall_ota_transfer_active(void) {
    return ota_state &&
           (ota_state->client_pcb != NULL || ota_state->http_active);
}

void flightwall_ota_reboot_to_update(void) {
    if (!flightwall_ota_update_ready()) return;
    watchdog_hw->scratch[0] = OTA_REBOOT_MAGIC;
    watchdog_hw->scratch[1] = ota_state->flash_update;
    cyw43_arch_disable_sta_mode();
    cyw43_arch_deinit();
    int ret = rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_FLASH_UPDATE, 1000, ota_state->flash_update, 0);
    printf("Done - rebooting for a flash update boot %d\n", ret);
    free(ota_state);
    ota_state = NULL;
    sleep_ms(2000);
}
