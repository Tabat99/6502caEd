#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * Memory region types
 * ------------------------------------------------------------------------- */
typedef enum {
    MEM_RAM  = 0,   /* Read/write RAM */
    MEM_ROM  = 1,   /* Read-only; writes ignored */
    MEM_IO   = 2,   /* I/O — dispatched to callback */
    MEM_NONE = 3,   /* Unmapped — reads return 0xFF, writes ignored */
} mem_region_type_t;

#define MEM_MAX_REGIONS 32
#define MEM_MAX_SIZE    (16 * 1024 * 1024)  /* 16 MB for 65816 */

/* ---------------------------------------------------------------------------
 * I/O callback — called for every read/write in an IO region
 * ------------------------------------------------------------------------- */
typedef uint8_t (*io_read_fn) (uint32_t addr, void *ud);
typedef void    (*io_write_fn)(uint32_t addr, uint8_t val, void *ud);

/* ---------------------------------------------------------------------------
 * A single memory region descriptor
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t          base;    /* Start address */
    uint32_t          size;    /* Size in bytes */
    mem_region_type_t type;
    uint8_t          *data;    /* For RAM/ROM: pointer into mem_t.ram */
    io_read_fn        io_rd;   /* For IO regions */
    io_write_fn       io_wr;
    void             *io_ud;
    char              label[24];
} mem_region_t;

/* ---------------------------------------------------------------------------
 * Memory system state
 * ------------------------------------------------------------------------- */
typedef struct {
    uint8_t       *ram;           /* Backing store */
    size_t         ram_size;
    mem_region_t   regions[MEM_MAX_REGIONS];
    int            n_regions;
} mem_t;

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Allocate backing RAM, set up empty region table */
bool mem_init(mem_t *m, size_t ram_size);
void mem_free(mem_t *m);

/* Add a region. For RAM/ROM, data_offset is offset into m->ram.
 * Returns false if too many regions or out of range. */
bool mem_add_ram(mem_t *m, uint32_t base, uint32_t size,
                 uint32_t data_offset, const char *label);
bool mem_add_rom(mem_t *m, uint32_t base, uint32_t size,
                 uint32_t data_offset, const char *label);
void mem_clear_io(mem_t *m);   /* remove all IO regions, keep RAM */
bool mem_add_io (mem_t *m, uint32_t base, uint32_t size,
                 io_read_fn rd, io_write_fn wr, void *ud,
                 const char *label);

/* Load a binary file into RAM at given offset, returns bytes loaded or -1 */
int  mem_load_bin(mem_t *m, uint32_t data_offset, const char *path);

/* Read/write — these are the cpu_read_fn / cpu_write_fn implementations */
uint8_t mem_read (uint32_t addr, void *ud);   /* ud = mem_t* */
void    mem_write(uint32_t addr, uint8_t val, void *ud);

/* Direct RAM access (bypasses region dispatch, for debugger) */
uint8_t mem_peek(mem_t *m, uint32_t addr);
void    mem_poke(mem_t *m, uint32_t addr, uint8_t val);
