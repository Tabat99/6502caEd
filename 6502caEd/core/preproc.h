#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "util.h"
#include "diag.h"

typedef struct {
    char* key;
    size_t key_len; // cached strlen(key) — avoids strlen on every probe
    char* val; // raw string
    bool used;
} def_entry_t;

typedef struct {
    def_entry_t* tab;
    size_t cap;
    size_t count;
} defs_t;

void defs_init(defs_t* d);
void defs_free(defs_t* d);
bool defs_set(defs_t* d, const char* key, const char* val);
const char* defs_get(const defs_t* d, sv_t key);
// Remove a definition (used by #undef / .undef). Returns true if the key existed.
bool defs_del(defs_t* d, sv_t key);

// Runs the preprocessor:
// - processes #def lines (stores raw value strings)
// - replaces $name occurrences in other lines (skipping string literals)
// - processes .include/include "path" by recursively inlining included files
// Returns newly allocated NUL-terminated text. Line count is preserved for #def and macro definition blocks; macro expansions may increase line count.
// hex_prefix/bin_prefix control output formatting of listings (not input parsing).
// Pass '\0' to mean "none".
// include_paths: optional list of search directories for INCLUDE when the file is not found
// relative to the including file.

// Extended entrypoint with control over include expansion.
// When expand_includes is false, .include/.includeroot are treated as no-ops
// (but line numbers are preserved via blank lines), which is useful for
// editor-time semantic analysis.
char* preproc_run_ex(const char* file, const char* in_text, diags_t* diags, defs_t* out_defs,
                     char* io_hex_prefix, bool hex_prefix_cli, char bin_prefix,
                     bool expand_includes,
                     const char** include_paths, size_t include_path_count);

char* preproc_run(const char* file, const char* in_text, diags_t* diags, defs_t* out_defs,
                  char* io_hex_prefix, bool hex_prefix_cli, char bin_prefix,
                  const char** include_paths, size_t include_path_count);
