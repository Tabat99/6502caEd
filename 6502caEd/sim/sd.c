/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 MJD
 */
#ifndef _WIN32
#  ifndef _FILE_OFFSET_BITS
#    define _FILE_OFFSET_BITS 64
#  endif
#endif
/* sd.c — SD card SPI mode 0 emulation
 *
 * Implements the SD SPI command set sufficient for FAT filesystem access:
 *   CMD0, CMD8, CMD9, CMD10, CMD12, CMD16, CMD17, CMD18,
 *   CMD24, CMD25, CMD32/33/38, CMD55, CMD58, CMD59,
 *   ACMD13, ACMD22, ACMD23, ACMD41, ACMD51
 *
 * Timing (strict mode):
 *   NCR  = 1-8 bytes (we use 1 byte = 8 cycles) after command before response
 *   NBR  = 1 byte before data token
 *   NWR  = 1 byte minimum write busy after block accepted
 *   Busy = DO held low during write processing (~250ms real, we use 10ms)
 */
#include "sd.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Use 64-bit offsets on Windows as well as POSIX. */
static bool image_seek(FILE *fp, uint64_t offset) {
    if (!fp || offset > INT64_MAX) return false;
#ifdef _WIN32
    return _fseeki64(fp, (int64_t)offset, SEEK_SET) == 0;
#else
    return fseeko(fp, (off_t)offset, SEEK_SET) == 0;
#endif
}

static uint32_t transfer_len(const sd_t *sd) {
    return sd->is_sdhc ? SD_BLOCK_SIZE : sd->block_len;
}

static bool transfer_fits(const sd_t *sd, uint64_t addr) {
    uint32_t len = transfer_len(sd);
    return sd->fp && len > 0 && len <= SD_BLOCK_SIZE &&
           addr <= sd->capacity && len <= sd->capacity - addr;
}

/* -------------------------------------------------------------------------
 * CRC helpers
 * ---------------------------------------------------------------------- */

/* CRC7 for SD command/response (polynomial x^7 + x^3 + 1).
 * Returns the transmitted CRC byte, including the required end bit.
 * Known checks:
 *   CMD0 40 00 00 00 00 -> 95
 *   CMD8 48 00 00 01 AA -> 87
 */
static uint8_t crc7(const uint8_t *data, int len) {
    uint8_t crc = 0;

    for (int i = 0; i < len; i++) {
        uint8_t d = data[i];

        for (int b = 0; b < 8; b++) {
            crc <<= 1;

            if ((d ^ crc) & 0x80)
                crc ^= 0x09;

            d <<= 1;
            crc &= 0x7F;
        }
    }

    return (uint8_t)((crc << 1) | 1);
}

/* CRC16-CCITT for SD data blocks (polynomial x^16 + x^12 + x^5 + 1) */
static uint16_t crc16(const uint8_t *data, int len) {
    uint16_t crc = 0;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else              crc <<= 1;
        }
    }
    return crc;
}

/* -------------------------------------------------------------------------
 * CSD / CID generation
 * ---------------------------------------------------------------------- */

static void build_csd(sd_t *sd) {
    uint8_t *c = sd->csd;
    memset(c, 0, 16);

    if (sd->is_sdhc) {
        /* CSD version 2.0 (SDHC/SDXC) */
        uint32_t c_size = (uint32_t)((sd->capacity / (512ULL * 1024)) - 1);
        c[0]  = 0x40;           /* CSD_STRUCTURE=1 */
        c[1]  = 0x0E;           /* TAAC */
        c[2]  = 0x00;           /* NSAC */
        c[3]  = 0x32;           /* TRAN_SPEED 25MHz */
        c[4]  = 0x13;           /* CCC high */
        c[5]  = 0x59;           /* CCC low, READ_BL_LEN=9 (512) */
        c[6]  = 0x00;
        c[7]  = (uint8_t)((c_size >> 16) & 0x3F);
        c[8]  = (uint8_t)((c_size >>  8) & 0xFF);
        c[9]  = (uint8_t)( c_size        & 0xFF);
        c[10] = 0x7F;
        c[11] = 0x80;
        c[12] = 0x0A;
        c[13] = 0x40;
        c[14] = 0x00;
        c[15] = crc7(c, 15);
    } else {
        /* CSD version 1.0 (standard SD ≤2GB) */
        /* Calculate C_SIZE, C_SIZE_MULT, READ_BL_LEN for capacity */
        uint8_t read_bl_len = sd->capacity > (1ULL << 30) ? 10 : 9;
        uint8_t c_size_mult = 0;
        uint64_t unit = 1ULL << (read_bl_len + 2);
        while (sd->capacity / unit > 4096 && c_size_mult < 7) {
            ++c_size_mult;
            unit <<= 1;
        }
        uint16_t c_size = (uint16_t)(sd->capacity / unit - 1);

        c[0]  = 0x00;           /* CSD_STRUCTURE=0 */
        c[1]  = 0x26;           /* TAAC */
        c[2]  = 0x00;           /* NSAC */
        c[3]  = 0x32;           /* TRAN_SPEED */
        c[4]  = 0x13;
        c[5]  = 0x50 | read_bl_len;
        c[6]  = 0xE0 | (uint8_t)((c_size >> 10) & 0x03);
        c[7]  = (uint8_t)((c_size >> 2) & 0xFF);
        c[8]  = (uint8_t)((c_size & 0x03) << 6) | 0x3F;
        c[9]  = (uint8_t)((c_size_mult >> 1) & 0x03);
        c[10] = 0x40 | (uint8_t)((c_size_mult & 1) << 7);
        c[11] = 0x00;
        c[12] = 0x02;          /* WRITE_BL_LEN=9 */
        c[13] = 0x60;          /* WRITE_BL_LEN low bits + WRITE_BL_PARTIAL */
        c[14] = 0x00;
        c[15] = crc7(c, 15);
    }
}

static void build_cid(sd_t *sd) {
    uint8_t *c = sd->cid;
    memset(c, 0, 16);
    c[0] = 0xAA; /* synthetic simulator manufacturer */
    c[1] = '6'; c[2] = '5';
    memcpy(c + 3, "65SIM", 5);
    c[8] = 0x10; /* product revision 1.0 */
    c[9] = 0; c[10] = 0; c[11] = 0; c[12] = 1; /* serial */
    c[13] = 0x01; c[14] = 0xAA; /* date: October 2026 */
    c[15] = crc7(c, 15);
    (void)sd;
}

/* -------------------------------------------------------------------------
 * Response helpers
 * ---------------------------------------------------------------------- */

static void resp_clear(sd_t *sd) {
    sd->resp_len = 0;
    sd->resp_pos = 0;
}

static void resp_push(sd_t *sd, uint8_t b) {
    if (sd->resp_len < (int)sizeof(sd->resp))
        sd->resp[sd->resp_len++] = b;
}

static void resp_r1(sd_t *sd, uint8_t r1) {
    resp_clear(sd);
    /* NCR: 1 byte of 0xFF before response */
    resp_push(sd, 0xFF);
    resp_push(sd, r1);
}

static void resp_r3(sd_t *sd, uint8_t r1, uint32_t ocr) {
    resp_clear(sd);
    resp_push(sd, 0xFF);
    resp_push(sd, r1);
    resp_push(sd, (ocr >> 24) & 0xFF);
    resp_push(sd, (ocr >> 16) & 0xFF);
    resp_push(sd, (ocr >>  8) & 0xFF);
    resp_push(sd,  ocr        & 0xFF);
}

static void resp_r7(sd_t *sd, uint8_t r1, uint32_t val) {
    resp_r3(sd, r1, val);   /* same structure as R3 */
}

/* Queue a register/data response as R1 + token + data + CRC16. */
static void resp_data(sd_t *sd, uint8_t r1, const uint8_t *data, int len) {
    resp_clear(sd);
    resp_push(sd, 0xFF);            /* NCR */
    resp_push(sd, r1);              /* R1 */
    resp_push(sd, SD_TOKEN_START);  /* data token */
    for (int i = 0; i < len; i++)
        resp_push(sd, data[i]);
    /* CRC16 over exactly the returned data bytes. */
    uint16_t crc = crc16(data, len);
    resp_push(sd, (crc >> 8) & 0xFF);
    resp_push(sd,  crc       & 0xFF);
}

static bool read_block(sd_t *sd) {
    uint32_t len = transfer_len(sd);
    if (!transfer_fits(sd, sd->byte_addr) || !image_seek(sd->fp, sd->byte_addr))
        return false;
    if (fread(sd->block_buf, 1, len, sd->fp) != len) return false;
    uint16_t crc = crc16(sd->block_buf, (int)len);
    sd->block_buf[len] = (uint8_t)(crc >> 8);
    sd->block_buf[len + 1] = (uint8_t)crc;
    sd->block_pos = 0;
    return true;
}

/* -------------------------------------------------------------------------
 * Command execution
 * ---------------------------------------------------------------------- */

static uint32_t cmd_arg(const uint8_t *buf) {
    return ((uint32_t)buf[1] << 24) | ((uint32_t)buf[2] << 16) |
           ((uint32_t)buf[3] <<  8) |  (uint32_t)buf[4];
}

static void execute_cmd(sd_t *sd) {
    uint8_t cmd  = sd->cmd_buf[0] & 0x3F;

    uint32_t arg = cmd_arg(sd->cmd_buf);
    bool is_acmd = sd->app_cmd;
    sd->app_cmd = false;

    if ((sd->crc_enabled || cmd == 0 || cmd == 8) &&
        sd->cmd_buf[5] != crc7(sd->cmd_buf, 5)) {
        resp_r1(sd, (sd->initialised ? SD_R1_READY : SD_R1_IDLE) | SD_R1_CRC_ERR);
        sd->state = SD_STATE_RESPONSE;
        return;
    }

    /* ACMD handling */
    if (is_acmd) {
        if (cmd == 41) {
            /* High-capacity cards require the host's HCS capability bit. */
            sd->initialised = !sd->is_sdhc || (arg & 0x40000000UL) != 0;
            resp_r1(sd, sd->initialised ? SD_R1_READY : SD_R1_IDLE);
            sd->state = SD_STATE_RESPONSE;
            return;
        }
        if (!sd->initialised) {
            resp_r1(sd, SD_R1_IDLE);
            sd->state = SD_STATE_RESPONSE;
            return;
        }
        if (cmd == 23) { /* SET_WR_BLK_ERASE_COUNT: pre-erase hint */
            resp_r1(sd, SD_R1_READY);
        } else if (cmd == 22) { /* SEND_NUM_WR_BLOCKS */
            uint32_t count = sd->written_blocks;
            uint8_t data[4] = {(uint8_t)(count >> 24), (uint8_t)(count >> 16),
                               (uint8_t)(count >> 8), (uint8_t)count};
            resp_data(sd, SD_R1_READY, data, 4);
        } else if (cmd == 51) { /* SEND_SCR: SD 2.00, one-bit SPI interface */
            uint8_t scr[8] = {0x02, 0x81, 0, 0, 0, 0, 0, 0};
            resp_data(sd, SD_R1_READY, scr, 8);
        } else if (cmd == 13) { /* SD_STATUS: R2 then 64-byte status data */
            uint8_t data[64] = {0};
            resp_clear(sd);resp_push(sd, 0xFF);resp_push(sd, 0);resp_push(sd, 0);
            resp_push(sd, SD_TOKEN_START);
            for (int i = 0; i < 64; ++i) resp_push(sd, data[i]);
            uint16_t crc = crc16(data, 64);
            resp_push(sd, (uint8_t)(crc >> 8));resp_push(sd, (uint8_t)crc);
        } else {
            resp_r1(sd, SD_R1_ILLEGAL_CMD);
        }
        sd->state = SD_STATE_RESPONSE;
        return;
    }

    if (!sd->initialised && cmd != 0 && cmd != 1 && cmd != 8 && cmd != 55 && cmd != 58 && cmd != 59) {
        resp_r1(sd, SD_R1_IDLE);
        sd->state = SD_STATE_RESPONSE;
        return;
    }

    switch (cmd) {
    case 0:  /* CMD0 GO_IDLE_STATE */
        sd->initialised = false;
        sd->block_len = SD_BLOCK_SIZE;
        sd->crc_enabled = false;
        sd->status = 0;
        sd->written_blocks = 0;
        sd->erase_start_valid = sd->erase_end_valid = false;
        sd->multi_read = sd->multi_write = false;
        sd->pending_data_state = SD_STATE_IDLE;
        /* Strict: require 74+ power-up clocks before CMD0 */
        if (sd->strict && sd->powerup_clocks < 74) {
            resp_r1(sd, SD_R1_IDLE | SD_R1_PARAM_ERR);
        } else {
            resp_r1(sd, SD_R1_IDLE);
        }
        sd->state = SD_STATE_RESPONSE;
        break;

    case 1: /* Legacy SEND_OP_COND: supported for the SDSC model only. */
        sd->initialised = !sd->is_sdhc;
        resp_r1(sd, sd->is_sdhc ? (SD_R1_IDLE | SD_R1_ILLEGAL_CMD) : SD_R1_READY);
        sd->state = SD_STATE_RESPONSE;
        break;

    case 13: /* SEND_STATUS (R2), read and clear sticky error flags. */
        resp_r1(sd, SD_R1_READY);
        resp_push(sd, sd->status);
        sd->status = 0;
        sd->state = SD_STATE_RESPONSE;
        break;

    case 59: /* CRC_ON_OFF, as defined by the SPI protocol. */
        if (arg > 1) resp_r1(sd, (sd->initialised ? 0 : SD_R1_IDLE) | SD_R1_PARAM_ERR);
        else {
            sd->crc_enabled = arg != 0;
            resp_r1(sd, sd->initialised ? 0 : SD_R1_IDLE);
        }
        sd->state = SD_STATE_RESPONSE;
        break;

    case 8:  /* CMD8 SEND_IF_COND */
        /* Check voltage range: arg should be 0x000001AA */
        if ((arg & 0xFFF) == 0x1AA) {
            resp_r7(sd, sd->initialised ? SD_R1_READY : SD_R1_IDLE, arg & 0xFFF);
        } else {
            resp_r1(sd, SD_R1_ILLEGAL_CMD);
        }
        sd->state = SD_STATE_RESPONSE;
        break;

    case 9:  /* CMD9 SEND_CSD */
        resp_data(sd, SD_R1_READY, sd->csd, 16);
        sd->state = SD_STATE_RESPONSE;
        break;

    case 10: /* CMD10 SEND_CID */
        resp_data(sd, SD_R1_READY, sd->cid, 16);
        sd->state = SD_STATE_RESPONSE;
        break;

    case 12: /* CMD12 STOP_TRANSMISSION */
        sd->multi_read = sd->multi_write = false;
        sd->block_pos = 0;
        sd->pending_data_state = SD_STATE_IDLE;
        resp_r1(sd, sd->initialised ? SD_R1_READY : SD_R1_IDLE);
        sd->state = SD_STATE_RESPONSE;
        break;

    case 16: /* CMD16 SET_BLOCKLEN */
        if (arg >= 1 && arg <= SD_BLOCK_SIZE) {
            /* On SDHC this setting does not change memory transfer size. */
            sd->block_len = arg;
            resp_r1(sd, SD_R1_READY);
        } else {
            resp_r1(sd, SD_R1_PARAM_ERR);
        }
        sd->state = SD_STATE_RESPONSE;
        break;

    case 17: /* CMD17 READ_SINGLE_BLOCK */
    case 18: /* CMD18 READ_MULTIPLE_BLOCK */ {
        uint64_t byte_addr = sd->is_sdhc ? (uint64_t)arg * SD_BLOCK_SIZE
                                          : (uint64_t)arg;
        if (!transfer_fits(sd, byte_addr)) {
            resp_r1(sd, SD_R1_ADDR_ERR);
            sd->state = SD_STATE_RESPONSE;
            break;
        }
        sd->byte_addr = byte_addr;
        sd->multi_read = cmd == 18;
        sd->multi_write = false;
        if (!read_block(sd)) {
            resp_r1(sd, SD_R1_ADDR_ERR);
            sd->state = SD_STATE_RESPONSE;
            sd->multi_read = false;
            break;
        }
        resp_clear(sd);
        resp_push(sd, 0xFF);
        resp_push(sd, SD_R1_READY);
        resp_push(sd, SD_TOKEN_START);
        /* Use RESPONSE state to send NCR+R1+token first,
         * then transition to DATA_READ when resp[] is exhausted */
        sd->state = SD_STATE_RESPONSE;
        sd->pending_data_state = SD_STATE_DATA_READ;
        break;
    }

    case 24: /* CMD24 WRITE_BLOCK */
    case 25: /* CMD25 WRITE_MULTIPLE_BLOCK */ {
        uint64_t byte_addr = sd->is_sdhc ? (uint64_t)arg * SD_BLOCK_SIZE
                                          : (uint64_t)arg;
        if (!transfer_fits(sd, byte_addr)) {
            resp_r1(sd, SD_R1_ADDR_ERR);
            sd->state = SD_STATE_RESPONSE;
            break;
        }
        sd->byte_addr = byte_addr;
        sd->multi_write = cmd == 25;
        sd->written_blocks = 0;
        sd->multi_read = false;
        resp_r1(sd, SD_R1_READY);
        sd->block_pos = 0;
        /* Send R1 via RESPONSE state, then move to write-wait */
        sd->pending_data_state = (cmd == 25) ? SD_STATE_MULTI_WRITE
                                             : SD_STATE_DATA_WRITE_WAIT;
        sd->state = SD_STATE_RESPONSE;
        break;
    }

    case 32: /* ERASE_WR_BLK_START */
    case 33: { /* ERASE_WR_BLK_END, inclusive 512-byte sector address */
        uint64_t addr = sd->is_sdhc ? (uint64_t)arg * SD_BLOCK_SIZE : arg;
        if (addr % SD_BLOCK_SIZE || addr >= sd->capacity) {
            resp_r1(sd, SD_R1_ADDR_ERR);
            sd->erase_start_valid = sd->erase_end_valid = false;
            sd->status |= 0x80;
        } else {
            if (cmd == 32) {
                sd->erase_start = addr;sd->erase_start_valid = true;
                sd->erase_end_valid = false;
            } else { sd->erase_end = addr;sd->erase_end_valid = true; }
            resp_r1(sd, SD_R1_READY);
        }
        sd->state = SD_STATE_RESPONSE;
        break;
    }

    case 38: { /* ERASE */
        if (!sd->erase_start_valid || !sd->erase_end_valid || sd->erase_end < sd->erase_start) {
            resp_r1(sd, SD_R1_ERASE_SEQ);
        } else {
            uint8_t erased[4096];memset(erased, 0xFF, sizeof erased);
            uint64_t left = sd->erase_end - sd->erase_start + SD_BLOCK_SIZE;
            bool ok = image_seek(sd->fp, sd->erase_start);
            while (ok && left) {
                size_t n = left > sizeof erased ? sizeof erased : (size_t)left;
                ok = fwrite(erased, 1, n, sd->fp) == n;
                left -= n;
            }
            if (ok) ok = fflush(sd->fp) == 0;
            resp_r1(sd, ok ? SD_R1_READY : SD_R1_PARAM_ERR);
            if (!ok) sd->status |= 0x04;
            if (ok && sd->strict) {
                sd->pending_data_state = SD_STATE_BUSY;
                sd->busy_next_state = SD_STATE_IDLE;
                sd->busy_until = sd->cpu_cycles + sd->cpu_hz / 100;
            }
        }
        sd->erase_start_valid = sd->erase_end_valid = false;
        sd->state = SD_STATE_RESPONSE;
        break;
    }

    case 55: /* CMD55 APP_CMD */
        sd->app_cmd = true;
        resp_r1(sd, sd->initialised ? SD_R1_READY : SD_R1_IDLE);
        sd->state = SD_STATE_RESPONSE;
        break;

    case 58: /* CMD58 READ_OCR */
        {
            /* Bit 31 = power up complete (only after ACMD41 succeeds)
             * Bit 30 = CCS (card capacity status — SDHC/SDXC)
             * Bits 20-23 = supported voltage range 2.7-3.6V */
            uint32_t ocr = (sd->initialised ? 0x80000000UL : 0) |
                           ((sd->initialised && sd->is_sdhc) ? 0x40000000UL : 0) |
                           0x00FF8000UL;
            uint8_t r1 = sd->initialised ? SD_R1_READY : SD_R1_IDLE;
            resp_r3(sd, r1, ocr);
        }
        sd->state = SD_STATE_RESPONSE;
        break;

    default:
        resp_r1(sd, SD_R1_ILLEGAL_CMD);
        sd->state = SD_STATE_RESPONSE;
        break;
    }
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void sd_init(sd_t *sd, uint64_t cpu_hz, bool strict) {
    memset(sd, 0, sizeof(*sd));
    sd->cpu_hz = cpu_hz > 0 ? cpu_hz : 1000000;
    sd->strict = strict;
    sd->miso_byte = 0xFF;
    sd->miso_bit  = 7;
    sd->state = SD_STATE_IDLE;
    sd->block_len = SD_BLOCK_SIZE;
    sd->cs_last  = true;    /* CS starts high (deselected) */
    sd->sck_last = false;
    sd->resp_pos = sd->resp_len = 0;
}

void sd_reset(sd_t *sd) {
    bool enabled = sd->enabled;
    bool strict = sd->strict;
    uint64_t hz = sd->cpu_hz;
    FILE *fp = sd->fp;
    uint64_t cap = sd->capacity;
    bool sdhc = sd->is_sdhc;
    int card_mode = sd->card_mode;
    uint32_t block_len = SD_BLOCK_SIZE;
    char path[512];
    memcpy(path, sd->image_path, 512);
    uint8_t csd[16], cid[16];
    memcpy(csd, sd->csd, 16);
    memcpy(cid, sd->cid, 16);

    memset(sd, 0, sizeof(*sd));
    sd->enabled = enabled;
    sd->strict = strict;
    sd->cpu_hz = hz;
    sd->fp = fp;
    sd->capacity = cap;
    sd->is_sdhc = sdhc;
    sd->card_mode = card_mode;
    sd->block_len = block_len;
    memcpy(sd->image_path, path, 512);
    memcpy(sd->csd, csd, 16);
    memcpy(sd->cid, cid, 16);
    sd->miso_byte     = 0xFF;
    sd->miso_bit      = 7;
    sd->state         = SD_STATE_IDLE;
    sd->block_len     = block_len;
    sd->cs_last       = true;
    sd->powerup_clocks = 0;   /* must see 74+ clocks before CMD0 */
}

bool sd_open(sd_t *sd, const char *path) {
    sd_close(sd);
    FILE *f = fopen(path, "r+b");
    if (!f) return false;
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    int64_t sz = _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) { fclose(f); return false; }
    int64_t sz = (int64_t)ftello(f);
#endif
    if (sz < 2048 || sz % SD_BLOCK_SIZE != 0) { fclose(f); return false; }
    bool high_capacity = sd->card_mode == 2 ||
        (sd->card_mode == 0 && (uint64_t)sz > (2ULL << 30));
    if (high_capacity) {
        if ((uint64_t)sz % (512ULL * 1024) || (uint64_t)sz > (2ULL << 40)) {
            fclose(f); return false;
        }
    } else {
        if ((uint64_t)sz > (2ULL << 30)) { fclose(f); return false; }
        unsigned read_bits = (uint64_t)sz > (1ULL << 30) ? 10 : 9;
        uint64_t unit = 1ULL << (read_bits + 2);
        unsigned mult = 0;
        while ((uint64_t)sz / unit > 4096 && mult < 7) { ++mult; unit <<= 1; }
        if ((uint64_t)sz % unit || (uint64_t)sz / unit > 4096) {
            fclose(f); return false;
        }
    }
    rewind(f);
    sd->fp = f;
    sd->capacity = (uint64_t)sz;
    if (sd->card_mode == 1)
        sd->is_sdhc = false;
    else if (sd->card_mode == 2)
        sd->is_sdhc = true;
    else
        sd->is_sdhc = (sd->capacity > 2ULL * 1024 * 1024 * 1024);
    strncpy(sd->image_path, path, sizeof(sd->image_path) - 1);

    sd->image_path[sizeof(sd->image_path) - 1] = 0;
    build_csd(sd);
    build_cid(sd);
    sd_reset(sd);
    return true;
}

void sd_close(sd_t *sd) {
    if (sd->fp) { fclose(sd->fp); sd->fp = NULL; }
    sd->image_path[0] = '\0';
    sd->capacity = 0;
    sd_reset(sd);
}

bool sd_create(const char *path, uint64_t size_bytes) {
    if (size_bytes < SD_BLOCK_SIZE) return false;
    /* Round to block boundary */
    size_bytes = (size_bytes / SD_BLOCK_SIZE) * SD_BLOCK_SIZE;

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    /* Write in chunks to avoid huge malloc */
    uint8_t chunk[4096];
    memset(chunk, 0xFF, sizeof(chunk));  /* SD unwritten blocks = 0xFF */
    uint64_t remaining = size_bytes;
    while (remaining > 0) {
        size_t n = (remaining > sizeof(chunk)) ? sizeof(chunk) : (size_t)remaining;
        if (fwrite(chunk, 1, n, f) != n) { fclose(f); return false; }
        remaining -= n;
    }
    fclose(f);
    return true;
}

void sd_tick(sd_t *sd) {
    if (sd->enabled) sd->cpu_cycles++;
}

void sd_set_cpu_hz(sd_t *sd, uint64_t cpu_hz) {
    sd->cpu_hz = cpu_hz > 0 ? cpu_hz : 1000000;
}

int sd_miso(const sd_t *sd) {
    if (!sd->enabled || !sd->fp)
        return 1;  /* MISO high = no card / idle */

    /* Do not globally force MISO low from busy_until here.
     *
     * SD cards return queued response bytes first, including the
     * CMD24 data-response token, and only then enter busy-low.
     * Busy-low is handled by SD_STATE_BUSY in load_next_miso().
     */
    int bit = (sd->miso_bit < 8) ? sd->miso_bit : 7;
    return (sd->miso_byte >> bit) & 1;
}

/* Load next byte to send on MISO */
static void load_next_miso(sd_t *sd) {
    switch (sd->state) {
    case SD_STATE_RESPONSE:
        if (sd->resp_pos < sd->resp_len) {
            sd->miso_byte = sd->resp[sd->resp_pos++];
        } else if (sd->pending_data_state != SD_STATE_IDLE) {
            /* Transition to next phase */
            sd->state = sd->pending_data_state;
            sd->pending_data_state = SD_STATE_IDLE;
            if (sd->state == SD_STATE_DATA_READ ||
                sd->state == SD_STATE_MULTI_READ) {
                /* Load first byte of block data */
                sd->miso_byte = sd->block_buf[sd->block_pos++];
            } else if (sd->state == SD_STATE_BUSY) {
                /* Write busy — MISO low until done */
                sd->miso_byte = 0x00;
            } else {
                /* Write wait — MISO high, host sends data token */
                sd->miso_byte = 0xFF;
            }
        } else {
            sd->miso_byte = 0xFF;
            sd->state = SD_STATE_IDLE;
        }
        break;

    case SD_STATE_DATA_READ:
    case SD_STATE_MULTI_READ:
        if (sd->block_pos < (int)transfer_len(sd) + 2) {
            sd->miso_byte = sd->block_buf[sd->block_pos++];
        } else if (sd->multi_read) {
            sd->byte_addr += transfer_len(sd);
            if (read_block(sd)) {
                sd->miso_byte = SD_TOKEN_START;
            } else {
                sd->miso_byte = 0x08; /* data error: out of range/read failure */
                sd->multi_read = false;
                sd->state = SD_STATE_IDLE;
            }
        } else {
            sd->miso_byte = 0xFF;
            sd->state = SD_STATE_IDLE;
        }
        break;

    case SD_STATE_BUSY:
        if (!sd->strict || sd->cpu_cycles >= sd->busy_until) {
            sd->miso_byte = 0xFF;
            sd->state = sd->busy_next_state;
            sd->busy_next_state = SD_STATE_IDLE;
        } else {
            sd->miso_byte = 0x00;  /* busy = MISO low */
        }
        break;

    default:
        sd->miso_byte = 0xFF;
        break;
    }
    sd->miso_bit = 7;
}

int sd_update(sd_t *sd, bool cs, bool sck, bool mosi) {
    if (!sd->enabled || !sd->fp) return 1;

    bool cs_fall  = sd->cs_last  && !cs;   /* CS falling = card selected */
    bool cs_rise  = !sd->cs_last && cs;    /* CS rising  = deselected */
    bool sck_rise = !sd->sck_last && sck;  /* SCK rising = sample MOSI */
    bool sck_fall = sd->sck_last  && !sck; /* SCK falling = shift MISO */

    sd->cs_last  = cs;
    sd->sck_last = sck;

    /* CS rising — deselect, reset bit counter */
    if (cs_rise) {

        sd->bit_count = 0;
        sd->shift_in  = 0;
        bool programming = sd->state == SD_STATE_BUSY ||
                           sd->pending_data_state == SD_STATE_BUSY;
        sd->multi_read = sd->multi_write = false;
        sd->state = programming ? SD_STATE_BUSY : SD_STATE_IDLE;
        sd->busy_next_state = SD_STATE_IDLE;
        sd->pending_data_state = SD_STATE_IDLE;
        sd->cmd_pos = 0;
        resp_clear(sd);
        sd->miso_byte = 0xFF;
        sd->miso_bit  = 7;
        return 1;
    }

    /* Count power-up clocks while the card is deselected.
       SD SPI startup requires at least 74 clock pulses with CS high
       before CMD0 is accepted in strict mode.  This must be done
       before the early CS-high return below. */
    if (sck_rise && cs && sd->powerup_clocks < 74)
        sd->powerup_clocks++;

    /* Card deselected — MISO high */
    if (cs) return 1;

    /* CS just selected — reset for new transaction */
    if (cs_fall) {

        sd->bit_count = 0;
        sd->shift_in  = 0;
        /* Load first MISO byte for this transaction */
        if (sd->state == SD_STATE_RESPONSE ||
            sd->state == SD_STATE_DATA_READ ||
            sd->state == SD_STATE_MULTI_READ) {
            /* already have data queued */
        } else {
            sd->miso_byte = 0xFF;
            sd->miso_bit  = 7;
        }
    }

    /* SPI Mode 0: sample MOSI on rising SCK, shift MISO on falling SCK */
    if (sck_rise) {
        /* Sample MOSI into shift register */
        sd->shift_in = (sd->shift_in << 1) | (mosi ? 1 : 0);
        sd->bit_count++;

        if (sd->bit_count == 8) {
            uint8_t byte = sd->shift_in;
            sd->shift_in = 0;
            sd->bit_count = 0;

            if (sd->multi_read && sd->state == SD_STATE_DATA_READ && byte == 0x4C) {
                sd->cmd_buf[0] = byte;
                sd->cmd_pos = 1;
                sd->state = SD_STATE_CMD;
            } else {
            /* Process received byte based on state */
            switch (sd->state) {
            case SD_STATE_IDLE:
            case SD_STATE_ACMD:
                /* Waiting for command start bit (0x40-0x7F) */
                if ((byte & 0xC0) == 0x40) {
                    sd->cmd_buf[0] = byte;
                    sd->cmd_pos = 1;
                    sd->state = SD_STATE_CMD;
                }
                /* else: discard (could be 0xFF clock-in bytes) */
                break;

            case SD_STATE_CMD:
                sd->cmd_buf[sd->cmd_pos++] = byte;
                if (sd->cmd_pos == SD_CMD_LEN) {
                    execute_cmd(sd);
                    /* Load first response byte.
                     * Set miso_bit=8 so first SCK fall decrements to 7,
                     * giving the host bit7 on the following rise. */
                    if (sd->state == SD_STATE_RESPONSE) {
                        load_next_miso(sd);
                        sd->miso_bit = 8;
                    }
                }
                break;

            case SD_STATE_DATA_WRITE_WAIT:
            case SD_STATE_MULTI_WRITE:
                if ((!sd->multi_write && byte == SD_TOKEN_START) ||
                    (sd->multi_write && byte == SD_TOKEN_START_MBW)) {
                    sd->block_pos = 0;
                    sd->state = SD_STATE_DATA_WRITE;
                } else if (byte == SD_TOKEN_STOP_TRAN && sd->multi_write) {
                    sd->multi_write = false;
                    sd->state = SD_STATE_IDLE;
                    sd->miso_byte = 0xFF;
                } else if (byte == 0x4C && sd->multi_write) {
                    sd->cmd_buf[0] = byte;
                    sd->cmd_pos = 1;
                    sd->state = SD_STATE_CMD;
                }
                break;

            case SD_STATE_DATA_WRITE: {
                uint32_t len = transfer_len(sd);
                if (sd->block_pos < (int)len + 2)
                    sd->block_buf[sd->block_pos++] = byte;
                if (sd->block_pos == (int)len + 2) {
                    uint16_t recv_crc = ((uint16_t)sd->block_buf[len] << 8) |
                                         sd->block_buf[len + 1];
                    bool crc_ok = !sd->crc_enabled || recv_crc == crc16(sd->block_buf, (int)len);
                    bool written = false;
                    resp_clear(sd);
                    if (!crc_ok) {
                        resp_push(sd, SD_DATA_CRC_ERR);
                    } else if (!transfer_fits(sd, sd->byte_addr) ||
                               !image_seek(sd->fp, sd->byte_addr) ||
                               fwrite(sd->block_buf, 1, len, sd->fp) != len ||
                               fflush(sd->fp) != 0) {
                        sd->status |= 0x04;
                        resp_push(sd, SD_DATA_WRITE_ERR);
                    } else {
                        resp_push(sd, SD_DATA_ACCEPTED);
                        sd->byte_addr += len;
                        ++sd->written_blocks;
                        written = true;
                    }
                    sd_state_t next = sd->multi_write ? SD_STATE_MULTI_WRITE : SD_STATE_IDLE;
                    sd->pending_data_state = next;
                    if (written && sd->strict) {
                        sd->pending_data_state = SD_STATE_BUSY;
                        sd->busy_next_state = next;
                        sd->busy_until = sd->cpu_cycles + sd->cpu_hz / 100;
                    }
                    sd->state = SD_STATE_RESPONSE;
                    load_next_miso(sd);
                    sd->miso_bit = 8;
                }
                break;
            }

            default:
                break;
            }
            }
        }
    }

    if (sck_fall) {
        /* Advance MISO to next bit position */
        if (sd->miso_bit > 0) {
            sd->miso_bit--;
        } else {
            load_next_miso(sd);
        }
    }

    return sd_miso(sd);
}
