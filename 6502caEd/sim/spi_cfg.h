#pragma once
/* spi_cfg.h — SPI bus and SD card pin mapping configuration
 *
 * Describes how MOSI, MISO, SCK and CS connect to VIA1/VIA2 port pins.
 * Saved in the .simprofile [sd] section.
 */
#include <stdint.h>
#include "sd.h"
#include <stdbool.h>

/* Reuse the lcd_pin_t concept — one pin = VIA + port + bit */
typedef struct {
    int via;    /* 1 = VIA1, 2 = VIA2 */
    int port;   /* 0 = port A, 1 = port B */
    int bit;    /* 0-7 */
} spi_pin_t;

/* SD card image sizes (bytes) */
typedef enum {
    SD_SIZE_128KB  = 0,
    SD_SIZE_512KB  = 1,
    SD_SIZE_2MB    = 2,
    SD_SIZE_32MB   = 3,
    SD_SIZE_128MB  = 4,
    SD_SIZE_512MB  = 5,
    SD_SIZE_1GB    = 6,
    SD_SIZE_2GB    = 7,
    SD_SIZE_4GB    = 8,
    SD_SIZE_8GB    = 9,
    SD_SIZE_16GB   = 10,
    SD_SIZE_32GB   = 11,
    SD_SIZE_COUNT  = 12
} sd_size_id_t;

/* Returns the byte count for a given size ID */
uint64_t sd_size_bytes(sd_size_id_t id);
/* Returns a human-readable label e.g. "32MB" */
const char *sd_size_label(sd_size_id_t id);

/* SD card address mode override.
 * AUTO uses image size: >2GB => SDHC/block addressed, otherwise SDSC/byte addressed.
 * SDSC and SDHC are useful for small-image hardware-compatibility tests. */
typedef enum {
    SD_CARD_AUTO = 0,
    SD_CARD_SDSC = 1,
    SD_CARD_SDHC = 2
} sd_card_mode_t;

/* Full SPI/SD configuration */
typedef struct {
    bool        enabled;        /* SD emulation active */

    /* SPI bus pins */
    spi_pin_t   mosi;           /* Master Out Slave In  (output from 6502) */
    spi_pin_t   miso;           /* Master In  Slave Out (input  to  6502) */
    spi_pin_t   sck;            /* Serial Clock (output from 6502) */
    spi_pin_t   cs;             /* Chip Select, active low (output from 6502) */

    /* Card image file — absolute path, empty = no card inserted */
    char        image_path[512];

    /* Strict timing enforcement */
    bool        read_only;      /* Open rb and reject guest writes/erase. */
    bool        image_specified; /* Explicit empty image means eject. */
    bool        strict;         /* true = enforce SPI/SD timing */

    sd_timing_t timing;
    sd_card_mode_t card_mode;    /* auto/sdsc/sdhc address mode override */

} spi_cfg_t;

/* Fill with sensible defaults (VIA1 port B: SCK=B0 MOSI=B1 MISO=B2 CS=B3) */
void spi_cfg_defaults(spi_cfg_t *cfg);

/* Parse a pin string like "1B3" → VIA1, port B, bit 3 */
bool spi_pin_parse(const char *s, spi_pin_t *pin);

/* Format a pin to string like "1B3" */
void spi_pin_format(const spi_pin_t *pin, char *buf, int bufsz);
