#pragma once
#include <stdint.h>
/* Representative simulation presets, independent of SDSC/SDHC capacity. */
typedef struct {
    uint32_t read_us, write_us, erase_us, max_spi_hz;
} sd_timing_t;
#define SD_TIMING_DEFAULTS {100, 2000, 10000, 25000000}

/* sd.h — SD card SPI mode emulation
 *
 * Emulates an SD/SDHC card in SPI mode 0 (CPOL=0, CPHA=0).
 * The host bit-bangs SCK/MOSI/CS; this module tracks the SPI
 * state machine and responds on MISO.
 *
 * Timing model:
 *   cpu_hz     — host CPU frequency (for strict timing)
 *   cpu_cycles — incremented by sd_tick() each CPU cycle
 *   strict     — when true, power-up clock and write-busy timing is enforced
 *   CRC checks — mandatory for CMD0/CMD8; CMD59 controls other command/data CRCs
 *
 * Card image:
 *   Backed by a file on disk. SDSC transfers are 1..512 bytes;
 *   SDHC memory transfers are always 512 bytes.
 *   Supports standard SD (≤2GB) and SDHC (>2GB, block-addressed).
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* SD command frame is 6 bytes: start(1) cmd(1) arg(4) crc(1) */
#define SD_CMD_LEN      6
#define SD_BLOCK_SIZE   512

/* R1 response bits */
#define SD_R1_IDLE          0x01
#define SD_R1_ERASE_RESET   0x02
#define SD_R1_ILLEGAL_CMD   0x04
#define SD_R1_CRC_ERR       0x08
#define SD_R1_ERASE_SEQ     0x10
#define SD_R1_ADDR_ERR      0x20
#define SD_R1_PARAM_ERR     0x40
#define SD_R1_READY         0x00

/* Data token for single/multiple block read */
#define SD_TOKEN_START      0xFE
#define SD_TOKEN_START_MBW  0xFC  /* multiple block write */
#define SD_TOKEN_STOP_TRAN  0xFD  /* stop multiple write */

/* Data response token bits */
#define SD_DATA_ACCEPTED    0x05
#define SD_DATA_CRC_ERR     0x0B
#define SD_DATA_WRITE_ERR   0x0D

/* Internal state machine states */
typedef enum {
    SD_STATE_IDLE,              /* waiting for CS low */
    SD_STATE_CMD,               /* receiving command bytes */
    SD_STATE_NCR,               /* command processing delay (1-8 bytes) */
    SD_STATE_RESPONSE,          /* sending response bytes */
    SD_STATE_DATA_TOKEN,        /* sending data token before block */
    SD_STATE_DATA_READ,         /* sending block data + CRC */
    SD_STATE_DATA_WRITE_WAIT,   /* waiting for host write token */
    SD_STATE_DATA_WRITE,        /* receiving block data from host */
    SD_STATE_BUSY,              /* busy after write (DO low) */
    SD_STATE_ACMD,              /* next command is ACMD (after CMD55) */
    SD_STATE_MULTI_READ,        /* CMD18 multiple block read in progress */
    SD_STATE_MULTI_WRITE,       /* CMD25 multiple block write in progress */
} sd_state_t;

typedef struct {
    /* Configuration */
    bool        enabled;
    sd_timing_t timing;
    uint64_t read_ready, last_sck_cycle;
    bool read_waiting, have_sck, clock_fault;
    uint64_t clock_violations;
    bool        strict;         /* enforce timing; CRC checking is controlled by CMD59 */
    bool        is_sdhc;        /* true if card is block addressed */
    int         card_mode;      /* 0=auto, 1=force SDSC, 2=force SDHC */
    uint64_t    capacity;       /* card size in bytes */
    uint64_t    cpu_hz;         /* host CPU frequency */
    uint64_t    cpu_cycles;     /* cycles elapsed (from sd_tick) */

    /* Card image file */
    bool        read_only;      /* Preserved by reset and enforced by SPI writes. */
    FILE       *fp;             /* open file handle, NULL if no card */
    char        image_path[512];

    /* SPI bit-bang state */
    bool        cs_last;        /* previous CS state */
    bool        sck_last;       /* previous SCK state */
    uint8_t     shift_in;       /* byte being assembled from MOSI */
    int         bit_count;      /* bits received in current byte (0-7) */
    uint8_t     miso_byte;      /* current byte being sent on MISO */
    int         miso_bit;       /* which bit of miso_byte is next (7 downto 0) */

    /* Command reception */
    uint8_t     cmd_buf[SD_CMD_LEN];
    int         cmd_pos;        /* bytes received into cmd_buf */

    /* Response queue — up to 20 bytes */
    uint8_t     resp[96];   /* R1 + token + up to 64 bytes + CRC16 */
    int         resp_len;
    int         resp_pos;

    /* State machine */
    sd_state_t  state;
    bool        app_cmd;        /* next cmd is ACMD (after CMD55) */
    bool        initialised;    /* card has completed init sequence */
    bool        crc_enabled;    /* CMD59: command and write-data CRC checking */
    uint8_t     status;         /* sticky second byte of CMD13 R2 */
    uint64_t    erase_start, erase_end;
    bool        erase_start_valid, erase_end_valid;
    uint32_t    written_blocks; /* successful writes since CMD25/CMD24 */

    /* Block I/O */
    uint64_t    byte_addr;      /* exact current byte offset in the image */
    uint32_t    block_len;      /* logical block length from CMD16; defaults to 512 */
    uint8_t     block_buf[SD_BLOCK_SIZE + 2]; /* data + 2 CRC bytes */
    int         block_pos;      /* position within block_buf */
    bool        multi_read;
    bool        multi_write;
    sd_state_t  busy_next_state; /* resume multi-write after the busy period */

    /* Timing */
    uint64_t    busy_until;     /* cpu_cycles when busy ends */

    /* Pending state after response queue is drained (e.g. DATA_READ after CMD17) */
    sd_state_t  pending_data_state;

    int         powerup_clocks;  /* clocks seen with CS=high before first CMD0 */

    /* NCR counter (response delay) */
    int         ncr_count;      /* bytes of 0xFF to send before response */

    /* CSD/CID */
    uint8_t     csd[16];
    uint8_t     cid[16];

} sd_t;

/* --------------------------------------------------------------------- */

/* Initialise SD emulation (does not open file) */
void sd_init(sd_t *sd, uint64_t cpu_hz, bool strict);

/* Reset SD state machine (card stays inserted) */
void sd_reset(sd_t *sd);

/* Open a card image file — returns true on success */
bool sd_open(sd_t *sd, const char *path);

/* Close card image file (eject) */
void sd_close(sd_t *sd);

/* Create a new card image file of given size — returns true on success */
bool sd_create(const char *path, uint64_t size_bytes);

/* Advance internal timers by one CPU cycle */
void sd_tick(sd_t *sd);
void sd_set_timing(sd_t *sd, const sd_timing_t *timing);

/* Update cpu_hz (e.g. when simulator speed changes) */
void sd_set_cpu_hz(sd_t *sd, uint64_t cpu_hz);

/* Called when SCK, MOSI or CS changes.
 * cs   — current CS pin state (active low, so false = selected)
 * sck  — current SCK pin state
 * mosi — current MOSI pin state
 * Returns current MISO pin state (0 or 1) */
int sd_update(sd_t *sd, bool cs, bool sck, bool mosi);

/* Read current MISO state without clocking */
int sd_miso(const sd_t *sd);

/* New blank filesystems use a superfloppy layout (no partition table).
 * Creation is exclusive and refuses existing files. */
typedef enum {
    SD_IMAGE_RAW, SD_IMAGE_FAT12, SD_IMAGE_FAT16, SD_IMAGE_FAT32
} sd_image_format_t;
bool sd_image_format_supported(uint64_t bytes, sd_image_format_t format);
bool sd_create_formatted(const char *path, uint64_t bytes,
                         sd_image_format_t format, const char *label);
