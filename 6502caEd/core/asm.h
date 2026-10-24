#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/*
 * Lightweight disassembly helper used by the GUI memory viewer.
 *
 * Formats a single 6502 instruction starting at pc into out (mnemonic + operand,
 * no leading address/bytes). Returns the instruction length in bytes (>=1).
 *
 * If the opcode is unknown, returns 1 and formats as ".byte &XX".
 */
int asm_disasm_6502(const uint8_t *mem, uint16_t pc,
                    char *out, size_t out_sz);

// 65C02 disassembly helper (same format/contract as asm_disasm_6502).
int asm_disasm_65c02(const uint8_t *mem, uint16_t pc,
                     char *out, size_t out_sz);

// 65816 disassembly helper.
int asm_disasm_65816(const uint8_t *mem, uint16_t pc,
                     char *out, size_t out_sz);

#include "diag.h"

typedef enum { ASM_CPU_6502 = 0, ASM_CPU_65C02 = 1, ASM_CPU_65816 = 2 } asm_cpu_t;

typedef struct {
  asm_cpu_t cpu;
  bool allow_undoc; // allow undocumented/illegal opcodes (NMOS 6502 table)
  // Listing output (BBC BASIC style)
  bool list_stdout;             // default true unless --nolist
  const char *list_path;        // optional; NULL if no file
  bool list_show_addr;          // default true
  bool list_show_bytes;         // default true
  unsigned list_bytes_per_line; // default 3
  char list_hex_prefix; // 0, '$', or '&' (applies to listing address/bytes)
  const char *out_path; // optional; may be NULL
  bool list_preserve_case;

  // Listing: include handling
  // If false (default), suppress INCLUDE/.INCLUDE directive lines and do not
  // emit file-boundary markers. If true, listing will show file markers.
  bool list_includes;

  // Symbol dump
  bool sym_stdout;      // print symbols to stdout
  const char *sym_path; // optional; NULL if no file
  bool sym_sort_addr;   // sort by address/value instead of name
  char sym_hex_prefix;  // 0, '$', or '&' (defaults to list_hex_prefix if 0)
  // Memory overwrite warnings (byte-level)
  bool overwrite_warn;           // default true; warn on second write to same address
  unsigned overwrite_warn_limit; // default 10; 0 = unlimited
  bool guard_warn;               // default true; enable .halt/.alert guard checks
} asm_settings_t;

// Parse CPU name (case-insensitive). Defaults to 6502 on unknown.
asm_cpu_t asm_cpu_from_name(const char *s);

// Stage-1: parse-only front end (labels/directives/operands) + basic PC
// tracking. Emits diagnostics with correct line/column offsets.
bool asm_parse_only(const char *file, const char *preprocessed_text,
                    const asm_settings_t *settings, diags_t *diags);

// Assemble into a 64KB memory image.
// - mem_out must point to 65536 bytes.
// - written_out may be NULL; if non-NULL, must point to 65536 bytes and will be
// set to 1 for each emitted byte.
// - min_written/max_written may be NULL; if provided they receive the min/max
// address written.
// - start_expr/end_expr are optional expressions (may reference labels) used by
// the caller for output range.
bool asm_assemble(const char *file, const char *preprocessed_text,
                  const asm_settings_t *settings, const char *start_expr,
                  const char *end_expr, uint8_t *mem_out, uint8_t *written_out,
                  uint8_t *written_explicit,
                  uint16_t *min_written, uint16_t *max_written,
                  uint16_t *out_start, uint16_t *out_end, diags_t *diags);
