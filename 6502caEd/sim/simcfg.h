#pragma once
/* simcfg.h — simulator configuration file load/save
 * Same key=value format as versions.txt — blank lines and # comments ignored,
 * all whitespace stripped before parsing.
 */
#include "cpu.h"
#include "sim.h"
#include <stdint.h>
#include <stdbool.h>

#define SIMCFG_MAX_ROMS 4

typedef struct {
    char     path[512];
    uint32_t load_addr;   /* address to place ROM in memory map */
    uint32_t bank;        /* 65816 bank (0 for 6502/65C02) */
    bool     enabled;
} simcfg_rom_t;

typedef struct {
    /* CPU */
    cpu_type_t  cpu_type;   /* CPU_6502 / CPU_65C02 / CPU_65816 */
    sim_speed_t speed;      /* SIM_SPEED_1MHZ / 2MHZ / MAX */

    /* Memory */
    uint32_t    mem_size;   /* total backing RAM in bytes */

    /* ROM slots */
    simcfg_rom_t roms[SIMCFG_MAX_ROMS];

    /* Banking (6502/65C02 only) */
    bool        bank_enable;
    uint32_t    bank_addr;        /* IO address that selects bank */
    uint32_t    bank_window;      /* start of banked window (default $8000) */
    uint32_t    bank_window_size; /* size of banked window  (default $8000) */
    uint8_t     bank_num_banks;   /* number of banks available */

    /* VIA 1 */
    bool        via1_enable;
    uint32_t    via1_base;

    /* VIA 2 */
    bool        via2_enable;
    uint32_t    via2_base;

    /* ACIA */
    bool        acia_enable;
    uint32_t    acia_base;
    int         acia_tcp_port;
    bool acia_w65c51n_tx_fault; /* Independent transmit-fault preference. */
    acia_model_t acia_model; /* Generic 6551, W65C51S or W65C51N. */
    int         acia_transport;  /* 0 TCP, 1 native serial */
    char        acia_serial_device[512];

    /* Simple I/O */
    bool        simio_enable;
    uint32_t    simio_tx_addr;
    uint32_t    simio_rx_addr;

    /* Debug window visible on start */
    bool        debug_window;
    /* RTS/BRK behaviour */
    /* on_rts: what happens when RTS is executed at top level (empty stack)
     *   0 = push FFFC return address (CPU jumps to reset vector on RTS)
     *   1 = halt (pause the simulator)
     * on_brk: what happens when BRK is executed
     *   0 = normal (vector through $FFFE)
     *   1 = halt (pause the simulator)
     */
    int on_rts;   /* 0=reset-vector, 1=halt */
    int on_brk;   /* 0=normal,       1=halt */

    /* Project directory — used for profile discovery.
     * Set to the directory containing the open .asm file, or "" for global. */
    char project_dir[512];

    /* Last used profile filepath — restored on next launch if still exists */
    char last_profile[768];

    /* Optional override profile path — if set, loaded instead of discovered
     * profile. Used for embedded ; [profile] blocks in .asm source files.
     * The temp file is deleted by sim_win_create after loading. */
    char override_profile[768];

    /* Map $DB to STP (halt) even on 6502 */
    bool stp_on_db;
} simcfg_t;

/* Fill cfg with sensible defaults */
void simcfg_defaults(simcfg_t *cfg);

/* Load from file — missing keys keep their default values.
 * Returns true on success (file opened), false if file not found. */
bool simcfg_load(simcfg_t *cfg, const char *path);

/* Save to file. Returns true on success. */
bool simcfg_save(const simcfg_t *cfg, const char *path);
