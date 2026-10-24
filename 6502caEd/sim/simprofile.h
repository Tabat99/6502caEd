#pragma once
/* simprofile.h — Simulator memory map profiles
 *
 * A profile describes the complete memory map for a target system:
 * ROM ranges, IO device addresses, banking, and ROM protection.
 * Profiles are stored as INI-style .simprofile files, either globally
 * in ~/.6502caed/profiles/ or per-project in the project folder.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "lcd_cfg.h"
#include "spi_cfg.h"

/* CPU types that support banking (not 65816) */
#define PROFILE_CPU_6502    0
#define PROFILE_CPU_6502U   1   /* 6502 + undocumented opcodes */
#define PROFILE_CPU_65C02   2
#define PROFILE_CPU_65816   3

#define PROFILE_BANK_ALL   -1   /* device present in all banks */

#define PROFILE_ROM_MAX     4   /* max ROM ranges per profile */

/* -----------------------------------------------------------------------
 * ROM range
 * -----------------------------------------------------------------------
 * R/W semantics:
 *   rw=0 : reads come from ROM image, writes go to underlying RAM (shadow)
 *   rw=1 : reads come from RAM (overlay), writes are lost
 *
 * Lock semantics:
 *   lock=0 : rw bit can be changed via the control register
 *   lock=1 : rw bit AND lock bit are frozen until CPU reset
 *            (lock is automatically cleared on reset)
 *
 * rw_bit / lock_bit : which bit in the ROM control register byte
 *                     controls this range (-1 = not connected)
 */
typedef struct {
    bool        enabled;
    int         bank;           /* PROFILE_BANK_ALL or 0-255          */
    uint16_t    from;           /* start address (inclusive)           */
    uint16_t    to;             /* end address   (inclusive)           */
    int         rw_bit;         /* control reg bit for R/W  (-1=none) */
    int         lock_bit;       /* control reg bit for lock (-1=none) */

    /* Runtime state (not saved) */
    _Atomic bool rw_state;        /* current R/W bit value               */
    _Atomic bool lock_state;      /* current lock bit value              */
} profile_rom_t;

/* -----------------------------------------------------------------------
 * ROM protection control register
 *
 * A single IO byte whose bits map to the rw_bit / lock_bit fields of
 * each ROM range.  Writing a bit sets the corresponding R/W or lock
 * state.  Reading returns the current state.
 */
typedef struct {
    bool        enabled;
    int         bank;
    uint16_t    addr;
} profile_ctrl_t;

/* -----------------------------------------------------------------------
 * Generic IO device (VIA, ACIA, console TX/RX, bank register)
 */
typedef struct {
    bool        enabled;
    int         bank;
    uint16_t    addr;           /* base address (or single-byte addr)  */
} profile_io_t;

/* -----------------------------------------------------------------------
 * Full profile
 */
typedef struct {
    /* Identity */
    char        name[64];
    char        description[256];
    char        filepath[512];  /* full path, set when loaded          */
    bool        is_global;      /* true = ~/.6502caed/profiles/        */

    /* Target CPU */
    int         cpu_type;       /* PROFILE_CPU_* */

    /* ROM ranges (up to PROFILE_ROM_MAX) */
    profile_rom_t   rom[PROFILE_ROM_MAX];

    /* ROM protection control register */
    profile_ctrl_t  rom_ctrl;

    /* IO devices */
    profile_io_t    via1;
    profile_io_t    via2;
    profile_io_t    acia;
    profile_io_t    simio_tx;
    profile_io_t    simio_rx;

    /* Banking (6502/65C02 only; ignored for 65816) */
    profile_io_t    bank_reg;       /* the bank-select register        */
    uint16_t        bank_window;    /* start of banked window          */
    uint16_t        bank_window_sz; /* size of banked window           */
    uint8_t         bank_num;       /* number of banks                 */

    /* LCD configuration */
    lcd_cfg_t lcd;

    /* SD card / SPI configuration */
    spi_cfg_t sd;
} simprofile_t;

/* -----------------------------------------------------------------------
 * Profile list (for the manager UI)
 */
typedef struct {
    simprofile_t  **items;
    int             count;
    int             capacity;
} simprofile_list_t;

/* -----------------------------------------------------------------------
 * API
 */

/* Initialise a profile with sensible defaults:
 *   ROM0: enabled, bank=-1, C000-FFFF, no ctrl bits
 *   VIA1: enabled, $9000
 *   VIA2: enabled, $9010
 *   ACIA: enabled, $9020
 *   SIMIO TX: enabled, $9030
 *   SIMIO RX: enabled, $9031
 *   Everything else disabled */
void simprofile_defaults(simprofile_t *p);

/* Load a profile from a .simprofile file.
 * Returns true on success, fills *p and sets p->filepath. */
bool simprofile_load(simprofile_t *p, const char *path);

/* Save a profile to its filepath (must be set).
 * Returns true on success. */
bool simprofile_save(const simprofile_t *p);

/* Save a profile to an explicit path.
 * Updates p->filepath on success. */
bool simprofile_save_as(simprofile_t *p, const char *path);

/* Delete the file backing a profile.
 * Returns true on success. */
bool simprofile_delete(const simprofile_t *p);

/* Deep-copy src into dst.  dst->filepath is cleared (unsaved copy). */
void simprofile_copy(simprofile_t *dst, const simprofile_t *src);

/* -----------------------------------------------------------------------
 * Profile discovery
 */

/* Allocate and return a list of all profiles visible from project_dir.
 * Global profiles come from ~/.6502caed/profiles/.
 * Project profiles come from project_dir (may be NULL for global-only).
 * Caller must free with simprofile_list_free(). */
simprofile_list_t *simprofile_list_load(const char *project_dir);

/* Free a profile list and all its members. */
void simprofile_list_free(simprofile_list_t *list);

/* Find the default profile for a project:
 *   1. project_dir/default.simprofile
 *   2. ~/.6502caed/profiles/default.simprofile
 *   3. First profile in project_dir
 *   4. First global profile
 *   5. NULL (no profiles found — caller should use simprofile_defaults)
 * Returns a heap-allocated copy; caller must free(). */
simprofile_t *simprofile_find_default(const char *project_dir);

/* Mark a profile as the default for its scope (global or project).
 * Copies the file to default.simprofile in the appropriate directory.
 * Returns true on success. */
bool simprofile_set_default(simprofile_t *p, const char *project_dir);

/* -----------------------------------------------------------------------
 * Runtime helpers
 */

/* Reset all lock bits and rw bits to their power-on state (all zero).
 * Call on CPU reset. */
/* Check for overlapping ROM ranges (same bank, overlapping addresses).
 * Returns a description string of the first overlap found, or NULL if clean.
 * The returned string is a static buffer — copy if needed. */
const char *simprofile_check_overlaps(const simprofile_t *p);

void simprofile_reset_locks(simprofile_t *p);

/* Read the ROM control register byte from current rw/lock state. */
uint8_t simprofile_ctrl_read(const simprofile_t *p);

/* Write the ROM control register byte — updates rw/lock states,
 * respecting locked ranges. */
void simprofile_ctrl_write(simprofile_t *p, uint8_t val);

/* Return true if addr (in given bank) falls within a ROM range that is
 * currently in ROM-read mode (rw_state==0). */
bool simprofile_is_rom(const simprofile_t *p, uint16_t addr, int bank);

/* Return true if addr (in given bank) falls within a ROM range that is
 * currently in RAM-overlay mode (rw_state==1). */
bool simprofile_is_ram_overlay(const simprofile_t *p,
                               uint16_t addr, int bank);

/* Global and project profile directory paths.
 * Returned strings are static buffers — copy if you need to keep them. */
const char *simprofile_global_dir(void);
const char *simprofile_project_dir(const char *project_dir);
