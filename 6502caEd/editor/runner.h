#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Keep runner independent of both core/diag.h and editor/diagdb.h
// to avoid enum name collisions (DIAG_ERROR/DIAG_WARNING).

typedef enum {
  RUN_DIAG_WARNING = 0,
  RUN_DIAG_ERROR   = 1
} runner_diag_kind_t;

typedef struct {
  size_t line1; // 1-based
  size_t col1;  // 1-based
  runner_diag_kind_t kind;
  char* message; // heap owned
  char* file;    // heap owned; source file the error belongs to (may be NULL)
} runner_diag_item_t;

typedef struct {
  runner_diag_item_t* items;
  size_t count;
  uint16_t asm_start;  // min_written: lowest address written
  uint16_t asm_end;    // max_written: highest address written
  uint32_t asm_bytes;  // total bytes actually emitted
  bool     asm_valid;  // true if assembly succeeded

  /* Contiguous written ranges (computed from written[] array) */
#define ASM_MAX_RANGES 64
  struct { uint16_t start; uint16_t end; uint32_t count; }
      ranges[ASM_MAX_RANGES];
  int range_count;
} runner_result_t;

// Options for run_assembler_ex — zero-initialise for safe defaults.
typedef struct {
  bool allow_undoc;  // allow undocumented/illegal opcodes
} runner_opts_t;

// Extended assembler call with explicit options.
bool run_assembler_ex(const char* asm_path, char* out_path, size_t out_cap,
                      runner_result_t* out_result, const runner_opts_t* opts);

// Convenience wrapper: assembles with default options (allow_undoc=false).
bool run_assembler(const char* asm_path, char* out_path, size_t out_cap, runner_result_t* out_result);

// Check-only: runs the full assemble pipeline for diagnostics but does NOT
// write a binary.  Faster than a full build and safe to call on unsaved files.
bool run_check_ex(const char* asm_path, runner_result_t* out_result, const runner_opts_t* opts);

// Free heap memory inside runner_result_t.
void runner_result_free(runner_result_t* r);
