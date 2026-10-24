#pragma once
#include "cpu.h"
#include "mem.h"
#include "via.h"
#include "acia.h"
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

/* ---------------------------------------------------------------------------
 * Simulator speed modes
 * ------------------------------------------------------------------------- */
typedef enum {
    SIM_SPEED_2MHZ   = 0,   /* genuine 2MHz throttled */
    SIM_SPEED_1MHZ   = 1,
    SIM_SPEED_512KHZ = 2,
    SIM_SPEED_128KHZ = 3,
    SIM_SPEED_32KHZ  = 4,
    SIM_SPEED_8KHZ   = 5,
    SIM_SPEED_2KHZ   = 6,
    SIM_SPEED_512HZ  = 7,
    SIM_SPEED_128HZ  = 8,
    SIM_SPEED_32HZ   = 9,
    SIM_SPEED_8HZ    = 10,
    SIM_SPEED_2HZ    = 11,
    SIM_SPEED_1HZ    = 12,
    SIM_SPEED_MAX    = 13,  /* uncapped — run as fast as host allows */
} sim_speed_t;

/* ---------------------------------------------------------------------------
 * Simple I/O ports (single-address terminal in/out)
 * ------------------------------------------------------------------------- */
typedef struct {
    bool     tx_enable;
    uint32_t tx_addr;    /* write A → terminal */
    bool     rx_enable;
    uint32_t rx_addr;    /* read → wait for keypress, return in A */
} simio_cfg_t;

/* ---------------------------------------------------------------------------
 * Terminal I/O ring buffers (thread-safe)
 * ------------------------------------------------------------------------- */
#define SIM_TERM_BUF 4096
typedef struct {
    char     buf[SIM_TERM_BUF];
    int      head, tail;
    pthread_mutex_t lock;
} sim_ringbuf_t;

/* ---------------------------------------------------------------------------
 * Simulator state
 * ------------------------------------------------------------------------- */
typedef struct {
    cpu_state_t  cpu;
    mem_t        mem;
    sim_speed_t  speed;
    simio_cfg_t  simio;

    /* Run state */
    volatile bool running;
    volatile bool paused;
    volatile bool do_step;   /* single-step request */
    volatile bool do_reset;

    /* Terminal I/O */
    sim_ringbuf_t term_out;  /* CPU → terminal display */
    sim_ringbuf_t term_in;   /* keypress → CPU */

    /* Breakpoints */
#define SIM_MAX_BREAKPOINTS 32
    uint32_t breakpoints[SIM_MAX_BREAKPOINTS];
    int      n_breakpoints;
    volatile bool hit_breakpoint;

    /* Peripherals — ticked per CPU cycle in sim thread */
    via_t        via[2];
    bool         via_enable[2];
    acia_t       acia;
    bool         acia_enable;

    /* Banking (6502/65C02) */
    bool         bank_enable;
    uint32_t     bank_addr;     /* IO address that selects bank */
    uint8_t      bank_reg;      /* current bank register value */
    uint8_t     *bank_rom;      /* full banked ROM (n * 65536 bytes) */
    uint32_t     bank_rom_size; /* total size of bank_rom */
    uint32_t     bank_window;   /* start of banked window in CPU space */
    uint32_t     bank_window_size; /* size of banked window */

    /* ROM image snapshot — immutable copy of mem.ram taken after the binary
     * is loaded.  Used by ROM range reads when rw_state=0 so that shadow
     * writes to mem.ram don't corrupt what the CPU reads back as ROM. */
    uint8_t     *rom_image;     /* heap copy of mem.ram at load time */
    uint32_t     rom_image_size;

    /* RTS/BRK behaviour (copied from simcfg) */
    int on_rts;   /* 0=push-reset-vector, 1=halt */
    int on_brk;   /* 0=normal, 1=halt */

    /* Optional per-cycle tick callback — used for LCD timing */
    void (*tick_cb)(void *ud);
    void  *tick_ud;

    /* Thread */
    pthread_t       thread;
    pthread_mutex_t state_lock;
    pthread_cond_t  step_cond;
} sim_t;

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
bool sim_init(sim_t *s, cpu_type_t cpu_type, size_t mem_size);
void sim_free(sim_t *s);

/* Start the emulator thread */
bool sim_start(sim_t *s);

/* Stop and join the thread */
void sim_stop(sim_t *s);

/* Pause / resume */
void sim_pause(sim_t *s);
void sim_resume(sim_t *s);

/* Single step (must be paused first) */
void sim_step(sim_t *s);

/* Reset */
void sim_reset(sim_t *s);

/* Breakpoints */
void sim_add_breakpoint(sim_t *s, uint32_t addr);
void sim_clear_breakpoints(sim_t *s);

/* Terminal I/O — safe to call from any thread */
int  sim_term_read (sim_t *s, char *buf, int max);   /* read output from CPU */
void sim_term_write(sim_t *s, char ch);              /* send key to CPU */

/* Pop a byte from terminal input (keypress→CPU), returns 1 if got one */
int sim_term_in_pop(sim_t *s, char *ch);

/* Push a byte directly to terminal output (CPU→display) */
void sim_term_out_push(sim_t *s, char ch);

/* Get a snapshot of CPU state (thread-safe copy) */
void sim_get_cpu(sim_t *s, cpu_state_t *out);
