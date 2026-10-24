#include "sym.h"

#include <stdlib.h>
#include <string.h>

/* Convert a single ASCII character to uppercase (ASCII only). */
static char up_ascii(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

/* FNV-1a hash of the uppercased bytes of p[0..n).
 * Applying the case fold inside the hash ensures identical results
 * regardless of whether the input is already uppercase. */
static uint32_t fnv1a32_upper(const char* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= (uint8_t)up_ascii(p[i]);
        h *= 16777619u;
    }
    return h;
}

/* Allocate a heap copy of the string view s, converted to uppercase. */
static char* dup_sv_upper(sv_t s) {
    char* out = (char*)malloc(s.len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < s.len; ++i) out[i] = up_ascii(s.p[i]);
    out[s.len] = 0;
    return out;
}

/* Resize the hash table to new_cap slots (must be a power of two) and
 * rehash all existing entries.  Called when the load factor reaches ~0.7.
 * Uses stored key_len to avoid calling strlen during rehash.
 * ret: false on OOM (table is left in a valid but smaller state) */
static bool symtab_grow(symtab_t* st, size_t new_cap) {
    sym_entry_t* old = st->entries;
    size_t old_cap = st->cap;

    sym_entry_t* ne = (sym_entry_t*)calloc(new_cap, sizeof(sym_entry_t));
    if (!ne) return false;

    st->entries = ne;
    st->cap = new_cap;
    st->count = 0;

    if (old) {
        for (size_t i = 0; i < old_cap; ++i) {
            if (!old[i].used) continue;
            // Re-insert using stored key_len — avoids sv_from_cstr/strlen.
            sv_t k = { old[i].key, old[i].key_len };
            (void)symtab_put(st, k, old[i].value);
            free(old[i].key);
        }
        free(old);
    }
    return true;
}

/* Initialise an empty symbol table, pre-allocating 256 slots. */
void symtab_init(symtab_t* st) {
    memset(st, 0, sizeof(*st));
    (void)symtab_grow(st, 256);
}

/* Free all allocated memory and zero the struct. */
void symtab_free(symtab_t* st) {
    if (!st) return;
    for (size_t i = 0; i < st->cap; ++i) {
        if (st->entries[i].used) free(st->entries[i].key);
    }
    free(st->entries);
    memset(st, 0, sizeof(*st));
}

/* Insert or update a symbol.  Uses linear probing on hash collision.
 * Grows the table (doubling cap) when the load factor reaches ~0.7. */
bool symtab_put(symtab_t* st, sv_t key, int32_t value) {
    if (!st || key.len == 0) return false;
    if (!st->entries) {
        if (!symtab_grow(st, 256)) return false;
    }
    // Load factor ~0.7
    if ((st->count * 10) >= (st->cap * 7)) {
        if (!symtab_grow(st, st->cap ? st->cap * 2 : 256)) return false;
    }

    uint32_t h = fnv1a32_upper(key.p, key.len);
    size_t mask = st->cap - 1;
    size_t idx = (size_t)h & mask;
    for (;;) {
        sym_entry_t* e = &st->entries[idx];
        if (!e->used) {
            /* Empty slot — insert new entry. */
            e->key = dup_sv_upper(key);
            if (!e->key) return false;
            e->key_len = key.len;
            e->value = value;
            e->used = true;
            st->count++;
            return true;
        }
        if (e->key_len == key.len) {
            /* Candidate slot — compare case-insensitively.
             * Keys are stored uppercase so only the incoming key needs folding. */
            bool same = true;
            for (size_t j = 0; j < key.len; ++j) {
                if ((char)e->key[j] != up_ascii(key.p[j])) { same = false; break; }
            }
            if (same) {
                e->value = value;  /* update existing */
                return true;
            }
        }
        idx = (idx + 1) & mask;  /* linear probe */
    }
}

/* Look up a symbol.  Returns true and writes *out_value on success. */
bool symtab_get(const symtab_t* st, sv_t key, int32_t* out_value) {
    if (!st || !st->entries || key.len == 0) return false;
    uint32_t h = fnv1a32_upper(key.p, key.len);
    size_t mask = st->cap - 1;
    size_t idx = (size_t)h & mask;
    for (;;) {
        const sym_entry_t* e = &st->entries[idx];
        if (!e->used) return false;  /* empty slot terminates the probe chain */
        if (e->key_len == key.len) {
            bool same = true;
            for (size_t j = 0; j < key.len; ++j) {
                if ((char)e->key[j] != up_ascii(key.p[j])) { same = false; break; }
            }
            if (same) {
                if (out_value) *out_value = e->value;
                return true;
            }
        }
        idx = (idx + 1) & mask;
    }
}
