/* spi_cfg.c — SPI bus pin config helpers */
#include "spi_cfg.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static const uint64_t size_table[SD_SIZE_COUNT] = {
    128ULL  * 1024,                  /* 128KB */
    512ULL  * 1024,                  /* 512KB */
    2ULL    * 1024 * 1024,           /* 2MB   */
    32ULL   * 1024 * 1024,           /* 32MB  */
    128ULL  * 1024 * 1024,           /* 128MB */
    512ULL  * 1024 * 1024,           /* 512MB */
    1024ULL * 1024 * 1024,           /* 1GB   */
    2048ULL * 1024 * 1024,           /* 2GB   */
    4096ULL * 1024 * 1024,           /* 4GB   */
    8192ULL * 1024 * 1024,           /* 8GB   */
    16384ULL* 1024 * 1024,           /* 16GB  */
    32768ULL* 1024 * 1024,           /* 32GB  */
};

static const char *size_labels[SD_SIZE_COUNT] = {
    "128KB","512KB","2MB","32MB","128MB","512MB",
    "1GB","2GB","4GB","8GB","16GB","32GB"
};

uint64_t sd_size_bytes(sd_size_id_t id) {
    if (id < 0 || id >= SD_SIZE_COUNT) return size_table[SD_SIZE_2GB];
    return size_table[id];
}

const char *sd_size_label(sd_size_id_t id) {
    if (id < 0 || id >= SD_SIZE_COUNT) return "?";
    return size_labels[id];
}

void spi_cfg_defaults(spi_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled      = false;
    cfg->strict       = false;
    cfg->card_mode    = SD_CARD_AUTO;
    cfg->timing = (sd_timing_t)SD_TIMING_DEFAULTS;
    /* Default: VIA1 port B — SCK=B0, MOSI=B1, MISO=B2, CS=B3 */
    cfg->sck.via  = 1; cfg->sck.port  = 1; cfg->sck.bit  = 0;
    cfg->mosi.via = 1; cfg->mosi.port = 1; cfg->mosi.bit = 1;
    cfg->miso.via = 1; cfg->miso.port = 1; cfg->miso.bit = 2;
    cfg->cs.via   = 1; cfg->cs.port   = 1; cfg->cs.bit   = 3;
    cfg->image_path[0] = '\0';
}

bool spi_pin_parse(const char *s, spi_pin_t *pin) {
    if (!s || !pin) return false;
    /* Format: "1B3" — digit, A/B, digit */
    int via  = s[0] - '0';
    int port = (toupper((unsigned char)s[1]) == 'B') ? 1 : 0;
    int bit  = s[2] - '0';
    if (via < 1 || via > 2) return false;
    if (bit < 0 || bit > 7) return false;
    pin->via  = via;
    pin->port = port;
    pin->bit  = bit;
    return true;
}

void spi_pin_format(const spi_pin_t *pin, char *buf, int bufsz) {
    snprintf(buf, bufsz, "%d%c%d",
             pin->via,
             pin->port ? 'B' : 'A',
             pin->bit);
}
