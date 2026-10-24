#include "runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include "source.h"
#include "preproc.h"
#include "asm.h"
#include "diag.h"

/* Derive the output .bin path by replacing the extension of asm_path
 * (everything after the last dot beyond the last slash) with ".bin".
 * If there is no extension, ".bin" is simply appended.
 * in:  asm_path — source file path
 * out: out[0..out_cap-1] — NUL-terminated output path */
static void path_make_outbin(const char* asm_path, char* out, size_t out_cap) {
  if (!out || out_cap == 0) return;
  out[0] = 0;
  if (!asm_path || !*asm_path) return;

  snprintf(out, out_cap, "%s", asm_path);

  /* Strip existing extension (only after the last slash). */
  char* last_slash = strrchr(out, '/');
  char* last_dot = strrchr(out, '.');
  if (last_dot && (!last_slash || last_dot > last_slash)) {
    *last_dot = 0;
  }
  /* Append .bin, respecting the buffer capacity. */
  size_t n = strlen(out);
  if (n + 4 < out_cap) {
    strcat(out, ".bin");
  } else {
    if (out_cap >= 5) {
      out[out_cap - 5] = 0;
      strcat(out, ".bin");
    }
  }
}

/* Free all diagnostic message strings and the items array. */
void runner_result_free(runner_result_t* r) {
  if (!r) return;
  if (r->items) {
    for (size_t i = 0; i < r->count; i++) {
      free(r->items[i].message);
      free(r->items[i].file);
    }
    free(r->items);
  }
  r->items = NULL;
  r->count = 0;
}

/* Populate out->items from the core diags_t produced by the assembler.
 * Converts diag_severity_t to runner_diag_kind_t to keep the runner
 * header independent of core/diag.h.
 * in:  diags — core diagnostic list from asm_assemble/preproc_run */
static void runner_result_from_diags(runner_result_t* out, const diags_t* diags) {
  if (!out) return;
  runner_result_free(out);
  if (!diags || diags->count == 0) return;

  out->items = (runner_diag_item_t*)calloc(diags->count, sizeof(runner_diag_item_t));
  if (!out->items) return;
  out->count = diags->count;

  for (size_t i = 0; i < diags->count; i++) {
    const diag_t* d = &diags->items[i];
    out->items[i].line1   = (size_t)d->line;
    out->items[i].col1    = (size_t)d->col;
    out->items[i].kind    = (d->sev == DIAG_ERROR) ? RUN_DIAG_ERROR : RUN_DIAG_WARNING;
    out->items[i].message = strdup(d->msg ? d->msg : "");
    out->items[i].file    = d->file ? strdup(d->file) : NULL;
    if (!out->items[i].message) out->items[i].message = strdup("out of memory");
  }
}

/* Full assembly pipeline: load source, preprocess, assemble, write binary.
 *
 * Runs the entire toolchain in-process (no subprocess), so the editor
 * gets live diagnostics with correct line/column numbers.
 *
 * in:  asm_path   — path to the .asm source file
 *      out_path   — buffer to receive the .bin output path (may be NULL)
 *      out_cap    — size of out_path buffer
 *      out_result — receives diagnostics and assembly stats (may be NULL)
 *      opts       — assembler options (NULL = defaults)
 * ret: true if the file was assembled and the binary written successfully */
/* Run the full assembly pipeline in-process.
 * Stages: load source -> preprocess -> assemble -> write binary.
 * in:  asm_path   — source file path
 *      out_path   — receives the .bin output path (may be NULL)
 *      out_cap    — size of out_path buffer
 *      out_result — receives diagnostics and stats (may be NULL)
 *      opts       — assembler options (NULL = defaults)
 * ret: true if assembly succeeded and the binary was written */
bool run_assembler_ex(const char* asm_path, char* out_path, size_t out_cap,
                      runner_result_t* out_result, const runner_opts_t* opts) {
  if (out_result) runner_result_free(out_result);
  if (out_path && out_cap) out_path[0] = 0;

  if (!asm_path || !*asm_path) return false;

  /* Stage 1: load source. */
  source_t src;
  if (!source_load(asm_path, &src)) {
    if (out_result) {
      out_result->items = (runner_diag_item_t*)calloc(1, sizeof(runner_diag_item_t));
      if (out_result->items) {
        out_result->count = 1;
        out_result->items[0].kind = RUN_DIAG_ERROR;
        out_result->items[0].line1 = 1;
        out_result->items[0].col1 = 1;
        out_result->items[0].message = strdup("cannot read input file");
      }
    }
    return false;
  }

  source_strip_comments_preserve_layout(&src);

  /* Stage 2: preprocess ($define substitution, .include expansion, macros). */
  diags_t diags;
  diags_init(&diags);
  defs_t defs;
  defs_init(&defs);

  char list_hex_prefix = 0;
  bool list_hex_prefix_cli = false;
  char list_bin_prefix = '%';

  /* Derive the directory of the source file for relative .include resolution. */
  char inc_dir_buf_ex[PATH_MAX];
  const char *inc_paths_ex[1];
  size_t inc_count_ex = 0;
  {
    strncpy(inc_dir_buf_ex, asm_path, sizeof(inc_dir_buf_ex) - 1);
    inc_dir_buf_ex[sizeof(inc_dir_buf_ex) - 1] = '\0';
    char *slash = strrchr(inc_dir_buf_ex, '/');
    if (slash) {
      *slash = '\0';
      inc_paths_ex[0] = inc_dir_buf_ex;
      inc_count_ex = 1;
    }
  }

  char* pp = preproc_run(asm_path, src.text, &diags, &defs,
                         &list_hex_prefix, list_hex_prefix_cli, list_bin_prefix,
                         inc_paths_ex, inc_count_ex);

  if (diags_has_errors(&diags) || !pp) {
    if (out_result) runner_result_from_diags(out_result, &diags);
    free(pp);
    defs_free(&defs);
    diags_free(&diags);
    source_free(&src);
    return false;
  }

  /* Determine output path (default: replace source extension with .bin). */
  char local_out[PATH_MAX];
  path_make_outbin(asm_path, local_out, sizeof(local_out));
  if (out_path && out_cap) {
    snprintf(out_path, out_cap, "%s", local_out);
  }

  /* Build assembler settings.  CPU is 6502 by default, but the source
   * may override via .cpu (the preprocessor writes defs["cpu"]). */
  asm_settings_t as;
  memset(&as, 0, sizeof(as));
  asm_cpu_t cpu = asm_cpu_from_name("6502");
  const char* cpuv = defs_get(&defs, sv_from_cstr("cpu"));
  if (cpuv && (strcmp(cpuv, "6503") == 0 || strcmp(cpuv, "65c02") == 0 || strcmp(cpuv, "65C02") == 0)) {
    cpu = asm_cpu_from_name("65c02");
  } else if (cpuv && (strcmp(cpuv, "65816") == 0 || strcmp(cpuv, "65c816") == 0 ||
                      strcmp(cpuv, "65816s") == 0 || strcmp(cpuv, "w65c816") == 0)) {
    cpu = asm_cpu_from_name("65816");
  }
  /* .cpu 6502u enables undocumented opcodes on the base 6502. */
  bool undoc_from_cpu = (cpuv && strcmp(cpuv, "6502u") == 0);
  as.cpu = cpu;
  as.allow_undoc = undoc_from_cpu || (opts && opts->allow_undoc);
  as.list_stdout = false;
  as.list_path = NULL;
  as.list_show_addr = true;
  as.list_show_bytes = true;
  as.list_bytes_per_line = 3;
  as.list_hex_prefix = list_hex_prefix;
  as.list_preserve_case = false;
  as.list_includes = false;
  as.sym_stdout = false;
  as.sym_path = NULL;
  as.sym_sort_addr = false;
  as.sym_hex_prefix = 0;
  as.overwrite_warn = true;
  as.overwrite_warn_limit = 10;
  as.guard_warn = true;
  as.out_path = local_out;

  /* Stage 3: assemble into a 64KB memory image.
   * Static so 128 KB lives in BSS rather than on the stack. */
  static uint8_t mem[65536];
  static uint8_t written[65536];

  memset(mem, 0, sizeof(mem));
  memset(written, 0, sizeof(written));
  uint16_t minw = 0xFFFF, maxw = 0x0000;
  uint16_t out_start = 0x0000, out_end = 0xFFFF;

  bool ok = asm_assemble(asm_path, pp, &as, NULL, NULL, mem, written, NULL, &minw, &maxw,
                         &out_start, &out_end, &diags);

  /* Convert warnings/errors to runner_result regardless of success. */
  if (out_result) runner_result_from_diags(out_result, &diags);

  /* Compute byte count and contiguous address ranges for the caller's UI. */
  if (out_result && ok && !diags_has_errors(&diags) && minw <= maxw) {
    out_result->asm_start = minw;
    out_result->asm_end   = maxw;
    uint32_t byte_count = 0;
    int rc = 0;
    int in_range = 0;
    for (int _i = 0; _i <= 65535; _i++) {
      if (written[_i]) {
        byte_count++;
        if (!in_range) {
          if (rc < ASM_MAX_RANGES) {
            out_result->ranges[rc].start = (uint16_t)_i;
            out_result->ranges[rc].count = 0;
          }
          in_range = 1;
        }
        if (rc < ASM_MAX_RANGES) out_result->ranges[rc].count++;
      } else if (in_range) {
        if (rc < ASM_MAX_RANGES) {
          out_result->ranges[rc].end = (uint16_t)(_i - 1);
          rc++;
        }
        in_range = 0;
      }
    }
    if (in_range && rc < ASM_MAX_RANGES) {
      out_result->ranges[rc].end = 0xFFFF;
      rc++;
    }
    out_result->asm_bytes   = byte_count;
    out_result->range_count = rc;
    out_result->asm_valid   = true;
  }

  if (!ok || diags_has_errors(&diags)) {
    free(pp);
    defs_free(&defs);
    diags_free(&diags);
    source_free(&src);
    return false;
  }

  /* Stage 4: write the binary output file. */
  FILE* f = fopen(local_out, "wb");
  if (!f) {
    if (out_result) {
      size_t n = out_result->count;
      runner_diag_item_t* ni = (runner_diag_item_t*)realloc(out_result->items, (n + 1) * sizeof(*ni));
      if (ni) {
        out_result->items = ni;
        out_result->count = n + 1;
        out_result->items[n].kind = RUN_DIAG_ERROR;
        out_result->items[n].line1 = 1;
        out_result->items[n].col1 = 1;
        out_result->items[n].message = strdup("cannot open output file for writing");
      }
    }
    free(pp);
    defs_free(&defs);
    diags_free(&diags);
    source_free(&src);
    return false;
  }

  uint16_t start = out_start;
  uint16_t end   = out_end;
  size_t want = (size_t)((uint32_t)end - (uint32_t)start + 1u);

  bool write_ok = (fwrite(mem + start, 1, want, f) == want);
  fclose(f);

  if (!write_ok) {
    if (out_result) {
      size_t n = out_result->count;
      runner_diag_item_t* ni = (runner_diag_item_t*)realloc(out_result->items, (n + 1) * sizeof(*ni));
      if (ni) {
        out_result->items = ni;
        out_result->count = n + 1;
        out_result->items[n].kind = RUN_DIAG_ERROR;
        out_result->items[n].line1 = 1;
        out_result->items[n].col1 = 1;
        out_result->items[n].message = strdup("write failed");
      }
    }
    free(pp);
    defs_free(&defs);
    diags_free(&diags);
    source_free(&src);
    return false;
  }

  free(pp);
  defs_free(&defs);
  diags_free(&diags);
  source_free(&src);
  return true;
}

/* Convenience wrapper: assembles with default options (allow_undoc=false). */
bool run_assembler(const char* asm_path, char* out_path, size_t out_cap, runner_result_t* out_result) {
  return run_assembler_ex(asm_path, out_path, out_cap, out_result, NULL);
}

/* Check-only pipeline: preprocess + assemble for diagnostics, no binary written.
 * The source file must exist on disk (include resolution requires a real path),
 * but the file does not need to be saved — any content already on disk is used.
 * Used by the editor's F5 "refresh errors" action. */
bool run_check_ex(const char* asm_path, runner_result_t* out_result, const runner_opts_t* opts) {
  if (out_result) runner_result_free(out_result);
  if (!asm_path || !*asm_path) return false;

  source_t src;
  if (!source_load(asm_path, &src)) {
    if (out_result) {
      out_result->items = (runner_diag_item_t*)calloc(1, sizeof(runner_diag_item_t));
      if (out_result->items) {
        out_result->count = 1;
        out_result->items[0].kind = RUN_DIAG_ERROR;
        out_result->items[0].line1 = 1;
        out_result->items[0].col1 = 1;
        out_result->items[0].message = strdup("cannot read input file");
      }
    }
    return false;
  }

  source_strip_comments_preserve_layout(&src);

  diags_t diags;
  diags_init(&diags);
  defs_t defs;
  defs_init(&defs);

  char list_hex_prefix = 0;
  bool list_hex_prefix_cli = false;
  char list_bin_prefix = '%';

  /* Derive the directory of the source file so that relative .include
     directives (e.g. .include "definitions.asm") resolve correctly. */
  char inc_dir_buf[PATH_MAX];
  const char *inc_paths[1];
  size_t inc_count = 0;
  {
    strncpy(inc_dir_buf, asm_path, sizeof(inc_dir_buf) - 1);
    inc_dir_buf[sizeof(inc_dir_buf) - 1] = '\0';
    char *slash = strrchr(inc_dir_buf, '/');
    if (slash) {
      *slash = '\0';
      inc_paths[0] = inc_dir_buf;
      inc_count = 1;
    }
  }

  char* pp = preproc_run(asm_path, src.text, &diags, &defs,
                         &list_hex_prefix, list_hex_prefix_cli, list_bin_prefix,
                         inc_paths, inc_count);

  if (diags_has_errors(&diags) || !pp) {
    if (out_result) runner_result_from_diags(out_result, &diags);
    free(pp);
    defs_free(&defs);
    diags_free(&diags);
    source_free(&src);
    return false;
  }

  asm_settings_t as;
  memset(&as, 0, sizeof(as));
  asm_cpu_t cpu = asm_cpu_from_name("6502");
  const char* cpuv = defs_get(&defs, sv_from_cstr("cpu"));
  if (cpuv && (strcmp(cpuv, "6503") == 0 || strcmp(cpuv, "65c02") == 0 || strcmp(cpuv, "65C02") == 0))
    cpu = asm_cpu_from_name("65c02");
  else if (cpuv && (strcmp(cpuv, "65816") == 0 || strcmp(cpuv, "65c816") == 0 ||
                    strcmp(cpuv, "65816s") == 0 || strcmp(cpuv, "w65c816") == 0))
    cpu = asm_cpu_from_name("65816");
  bool undoc_from_cpu = (cpuv && strcmp(cpuv, "6502u") == 0);
  as.cpu = cpu;
  as.allow_undoc = undoc_from_cpu || (opts && opts->allow_undoc);
  as.list_stdout = false;
  as.out_path = NULL;  /* no output file */
  as.overwrite_warn = true;
  as.overwrite_warn_limit = 10;
  as.guard_warn = true;

  static uint8_t mem[65536];
  static uint8_t written[65536];
  memset(mem, 0, sizeof(mem));
  memset(written, 0, sizeof(written));
  uint16_t minw = 0xFFFF, maxw = 0x0000;
  uint16_t out_start = 0, out_end = 0xFFFF;

  bool ok = asm_assemble(asm_path, pp, &as, NULL, NULL, mem, written, NULL,
                         &minw, &maxw, &out_start, &out_end, &diags);

  if (out_result) runner_result_from_diags(out_result, &diags);

  free(pp);
  defs_free(&defs);
  diags_free(&diags);
  source_free(&src);
  return ok && !diags_has_errors(&diags);
}
