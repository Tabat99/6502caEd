#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "util.h"

/* ── Symbol table ────────────────────────────────────────────────────────
 * Open-addressing hash table mapping label/constant names to their
 * integer values.  Keys are stored in uppercase so all lookups are
 * case-insensitive without repeated conversion.  The hash function is
 * FNV-1a applied to the uppercased key.
 *
 * Used by the assembler for label resolution across two passes.
 */

/* Single slot in the hash table. */
typedef struct {
    char* key;      /* owned, stored uppercase                             */
    size_t key_len; /* cached strlen(key) — avoids strlen in every probe   */
    int32_t value;  /* address or constant (may be 24-bit for 65816 banks) */
    bool used;      /* false = empty slot                                   */
} sym_entry_t;

/* The hash table itself.  cap is always a power of two. */
typedef struct {
    sym_entry_t* entries;
    size_t cap;    /* allocated slot count (power of two)  */
    size_t count;  /* number of occupied slots             */
} symtab_t;

/* Initialise an empty symbol table (pre-allocates 256 slots).
 * Must be paired with a symtab_free call. */
void symtab_init(symtab_t* st);

/* Free all memory owned by the symbol table and zero the struct. */
void symtab_free(symtab_t* st);

/* Insert or update a symbol.
 * Keys are case-insensitive and stored in uppercase.
 * in:  st    — symbol table
 *      key   — symbol name (string view, need not be NUL-terminated)
 *      value — address or constant value
 * ret: false on OOM */
bool symtab_put(symtab_t* st, sv_t key, int32_t value);

/* Look up a symbol by name (case-insensitive).
 * in:  st        — symbol table
 *      key       — name to look up
 * out: *out_value — value of the symbol (only written on success)
 * ret: true if found */
bool symtab_get(const symtab_t* st, sv_t key, int32_t* out_value);
