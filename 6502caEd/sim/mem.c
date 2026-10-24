#include "mem.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

bool mem_init(mem_t *m, size_t ram_size) {
    memset(m, 0, sizeof(*m));
    if (ram_size == 0 || ram_size > MEM_MAX_SIZE) return false;
    m->ram = (uint8_t *)calloc(1, ram_size);
    if (!m->ram) return false;
    m->ram_size = ram_size;
    return true;
}

void mem_free(mem_t *m) {
    free(m->ram);
    memset(m, 0, sizeof(*m));
}

static mem_region_t *add_region(mem_t *m) {
    if (m->n_regions >= MEM_MAX_REGIONS) return NULL;
    return &m->regions[m->n_regions++];
}

bool mem_add_ram(mem_t *m, uint32_t base, uint32_t size,
                 uint32_t data_offset, const char *label) {
    if (data_offset + size > m->ram_size) return false;
    mem_region_t *r = add_region(m);
    if (!r) return false;
    r->base = base; r->size = size; r->type = MEM_RAM;
    r->data = m->ram + data_offset;
    snprintf(r->label, sizeof(r->label), "%s", label ? label : "RAM");
    return true;
}

bool mem_add_rom(mem_t *m, uint32_t base, uint32_t size,
                 uint32_t data_offset, const char *label) {
    if (data_offset + size > m->ram_size) return false;
    mem_region_t *r = add_region(m);
    if (!r) return false;
    r->base = base; r->size = size; r->type = MEM_ROM;
    r->data = m->ram + data_offset;
    snprintf(r->label, sizeof(r->label), "%s", label ? label : "ROM");
    return true;
}

void mem_clear_io(mem_t *m) {
    /* Remove all IO regions, keeping RAM/ROM regions in place */
    int j = 0;
    for (int i = 0; i < m->n_regions; i++) {
        if (m->regions[i].type != MEM_IO)
            m->regions[j++] = m->regions[i];
    }
    m->n_regions = j;
}

bool mem_add_io(mem_t *m, uint32_t base, uint32_t size,
                io_read_fn rd, io_write_fn wr, void *ud, const char *label) {
    mem_region_t *r = add_region(m);
    if (!r) return false;
    r->base = base; r->size = size; r->type = MEM_IO;
    r->io_rd = rd; r->io_wr = wr; r->io_ud = ud;
    snprintf(r->label, sizeof(r->label), "%s", label ? label : "IO");
    return true;
}

int mem_load_bin(mem_t *m, uint32_t data_offset, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0 || (uint32_t)sz > m->ram_size - data_offset) {
        fclose(f); return -1;
    }
    size_t n = fread(m->ram + data_offset, 1, (size_t)sz, f);
    fclose(f);
    return (int)n;
}

static mem_region_t *find_region(mem_t *m, uint32_t addr) {
    /* Search in reverse so later-added regions take priority over earlier ones.
     * This allows ROM regions to overlay a general RAM region added first. */
    for (int i = m->n_regions - 1; i >= 0; i--) {
        mem_region_t *r = &m->regions[i];
        if (addr >= r->base && addr < r->base + r->size)
            return r;
    }
    return NULL;
}

uint8_t mem_read(uint32_t addr, void *ud) {
    mem_t *m = (mem_t *)ud;
    mem_region_t *r = find_region(m, addr);
    if (!r) return 0xFF;
    switch (r->type) {
        case MEM_RAM:
        case MEM_ROM:  return r->data[addr - r->base];
        case MEM_IO:   return r->io_rd ? r->io_rd(addr, r->io_ud) : 0xFF;
        default:       return 0xFF;
    }
}

void mem_write(uint32_t addr, uint8_t val, void *ud) {
    mem_t *m = (mem_t *)ud;
    mem_region_t *r = find_region(m, addr);
    if (!r) return;
    switch (r->type) {
        case MEM_RAM:  r->data[addr - r->base] = val; break;
        case MEM_ROM:  break; /* ignore */
        case MEM_IO:   if (r->io_wr) r->io_wr(addr, val, r->io_ud); break;
        default:       break;
    }
}

uint8_t mem_peek(mem_t *m, uint32_t addr) {
    return mem_read(addr, m);
}

void mem_poke(mem_t *m, uint32_t addr, uint8_t val) {
    mem_region_t *r = find_region(m, addr);
    if (r && (r->type == MEM_RAM)) r->data[addr - r->base] = val;
}
