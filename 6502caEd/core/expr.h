#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "util.h"
#include "diag.h"
#include "sym.h"

typedef enum {
    EXPR_VAL_NUM = 0,
    EXPR_VAL_STR = 1
} expr_val_kind_t;

typedef struct {
    expr_val_kind_t kind;
    int64_t num;     // valid if kind==NUM
    char* str;       // owned; valid if kind==STR
    size_t str_len;
} expr_val_t;

void expr_val_free(expr_val_t* v);

// Evaluate expression in 'src' (a slice of the operand). Supports:
// - numeric literals: $/& hex, % bin, decimal default
// - identifiers: label symbols from symtab
// - strings: "..." with basic escapes
// - operators: + - * /
// - unary: + - < > (low/high byte)
// - parentheses
// - string concatenation using '+'
//
// On error, emits diagnostic and returns false.
// Evaluate an expression in 'src' (a slice of a source line).
//
// If allow_undef is true, undefined identifiers do not raise an error.
// Instead, the evaluation succeeds, substitutes 0 for the missing symbol,
// and sets *out_unresolved=true.
//
// If allow_undef is false, undefined identifiers are reported as errors.
// proc_name: current .proc scope name, or NULL if not inside a proc.
// When non-NULL, bare label references are first tried as proc_name::label
// before falling back to the global name.
//
// parent_label: the most recently seen global (non-@) label, or NULL.
// When non-NULL, @foo references are resolved as parent_label::@foo.
bool expr_eval(const char* file, unsigned line_no, unsigned base_col,
               sv_t src, const symtab_t* syms,
               const uint16_t* anon_addrs, size_t anon_count, size_t anon_here,
               bool allow_undef, bool* out_unresolved,
               expr_val_t* out, diags_t* diags,
               const char* proc_name, const char* parent_label);
