/*
  6502caEd cli (terminal cli)
  Part of 6502caEd cross compiler suite
  MJD 2026
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "license_text.h"

#include "asm.h"
#include "diag.h"
#include "preproc.h"
#include "source.h"

/* Print usage information to stderr. */
static void usage(void) {
  fprintf(
      stderr,
      "6502ca - 6502/65C02 assembler (WIP)\n"
      "Usage: 6502ca <input.asm> [-o out.bin|--out out.bin] [--cpu "
      "6502|6502u|65c02] [--undoc]\n"
      "              [--start <expr>] [--end <expr>]\n"
      "              [-I <dir>]...\n"
      "\n"
      "Listing (default: print to stdout):\n"
      "  --nolist                  Disable listing to stdout\n"
      "  --listfile <file.lst>     Write listing to file (also prints unless "
      "--nolist)\n"
      "  --listbytes N             Bytes per line in listing (default 3)\n"
      "  --list-includes           Show include file markers in listing\n"
      "  --noaddr                  Omit address column\n"
      "  --nobytes                 Omit byte column\n"
      "  --list-preserve-case      Do not uppercase/space-normalise listing "
      "source\n"
      "  --hexprefix none|$|&      Prefix for listing hex values (default "
      "none)\n"
      "  --binprefix none|%%       Prefix for listing bin values / #assert bin "
      "(default %%)\n"
      "\n"
      "Licence:\n"
      "  --license                 Print the MIT licence and exit\n\n"
      "Symbols:\n"
      "  --sym                     Print symbol table to stdout\n"
      "  --symfile <file.sym>      Write symbol table to file\n"
      "  --symsort name|addr       Sort symbols by name (default) or address\n"
      "  --symhexprefix none|$|&   Prefix for symbol hex values (default: "
      "none; BBC-style)\n"

      "Safety (default: warn on overwrites, max 10):\n"
      "  --no-overwrite-warn      Disable overwrite warnings\n"
      "  --overwrite-warn-limit N Set max overwrite warnings (0 = "
      "unlimited)\n");
}

/* Append dir to the include-path array, growing it if needed.
 * in/out: *paths, *count, *cap — the dynamic array and its metadata
 *         dir                  — path string to append (not copied)
 * ret: false on OOM */
static bool push_include_path(const char ***paths, size_t *count, size_t *cap,
                              const char *dir) {
  if (*count == *cap) {
    size_t ncap = *cap ? *cap * 2 : 8;
    const char **n = (const char **)realloc((void *)*paths,
                                            ncap * sizeof(**paths));
    if (!n) {
      fprintf(stderr, "out of memory\n");
      return false;
    }
    *paths = n;
    *cap = ncap;
  }
  (*paths)[(*count)++] = dir;
  return true;
}

/* CLI entry point for the 6502ca command-line assembler.
 * Parses arguments, runs the preprocessor and assembler, writes the
 * binary output, and prints a summary listing to stderr.
 * Exit codes: 0 = success, 1 = assembly error, 2 = usage/argument error. */
int main(int argc, char **argv) {
  const char *in_path = NULL;
  const char *out_path = NULL;
  const char *cpu_name = "6502";
  const char *start_expr = NULL;
  const char *end_expr = NULL;
  bool allow_undoc = false;
  // Listing options (BBC BASIC style)
  bool list_stdout = true;
  const char *list_path = NULL;
  bool list_show_addr = true;
  bool list_show_bytes = true;
  unsigned list_bytes_per_line = 3;
  char list_hex_prefix = 0;
  bool list_hex_prefix_cli = false;
  char list_bin_prefix = '%';

  bool list_preserve_case = false;
  bool list_includes = false;

  // Symbol dump options
  bool sym_stdout = false;
  const char *sym_path = NULL;
  bool sym_sort_addr = false;
  char sym_hex_prefix = 0;

  // Overwrite warning options
  bool overwrite_warn = true;
  unsigned overwrite_warn_limit = 10;

  const char **include_paths = NULL;
  size_t include_path_count = 0;
  size_t include_path_cap = 0;

  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!strcmp(a, "--license") || !strcmp(a, "--licence")) {
      fputs(ED_MIT_LICENSE_TEXT, stdout);
      return 0;
    }
    if (!strcmp(a, "--version") || !strcmp(a, "-V")) {
      printf("6502ca build %s %s\n", __DATE__, __TIME__);
      printf("features: forward_refs=1 label_no_colon=1 dot_label=1 equs=1 "
             "macros=1\n");
      return 0;
    }
    if (!strcmp(a, "-o") || !strcmp(a, "--out") || !strcmp(a, "--o")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing filename after %s\n", a);
        usage();
        return 2;
      }
      out_path = argv[++i];
      continue;
    }
    if (!strcmp(a, "--cpu")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing cpu name after --cpu\n");
        usage();
        return 2;
      }
      cpu_name = argv[++i];
      continue;
    }

    // Include search paths
    if (!strcmp(a, "-I") || !strcmp(a, "--include") || !strcmp(a, "--inc")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing directory after %s\n", a);
        usage();
        return 2;
      }
      const char *dir = argv[++i];
      if (!push_include_path(&include_paths, &include_path_count,
                             &include_path_cap, dir)) {
        free((void *)include_paths);
        return 2;
      }
      continue;
    }
    if (a[0] == '-' && a[1] == 'I' && a[2] != '\0') {
      const char *dir = a + 2;
      if (!push_include_path(&include_paths, &include_path_count,
                             &include_path_cap, dir)) {
        free((void *)include_paths);
        return 2;
      }
      continue;
    }
    if (!strcmp(a, "--start")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing expression after --start\n");
        usage();
        return 2;
      }
      start_expr = argv[++i];
      continue;
    }
    if (!strcmp(a, "--end")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing expression after --end\n");
        usage();
        return 2;
      }
      end_expr = argv[++i];
      continue;
    }
    if (!strcmp(a, "--undoc") || !strcmp(a, "--undocumented") ||
        !strcmp(a, "--illegal-opcodes")) {
      allow_undoc = true;
      continue;
    }

    if (!strcmp(a, "--nolist")) {
      list_stdout = false;
      continue;
    }
    if (!strcmp(a, "--listfile") || !strcmp(a, "--list")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing filename after %s\n", a);
        usage();
        return 2;
      }
      list_path = argv[++i];
      continue;
    }
    if (!strcmp(a, "--liststdout") || !strcmp(a, "--list-stdout")) {
      list_stdout = true;
      continue;
    }
    if (!strcmp(a, "--listbytes") || !strcmp(a, "--list-bytes")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing number after %s\n", a);
        usage();
        return 2;
      }
      list_bytes_per_line = (unsigned)strtoul(argv[++i], NULL, 10);
      if (list_bytes_per_line == 0)
        list_bytes_per_line = 3;
      continue;
    }
    if (!strcmp(a, "--list-includes")) {
      list_includes = true;
      continue;
    }
    if (!strcmp(a, "--noaddr") || !strcmp(a, "--list-no-addr")) {
      list_show_addr = false;
      continue;
    }
    if (!strcmp(a, "--nobytes") || !strcmp(a, "--list-no-bytes")) {
      list_show_bytes = false;
      continue;
    }
    if (!strcmp(a, "--hexprefix") || !strcmp(a, "--list-hex-prefix")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", a);
        usage();
        return 2;
      }
      const char *v = argv[++i];
      if (!strcmp(v, "none"))
        list_hex_prefix = 0;
      else if (!strcmp(v, "$"))
        list_hex_prefix = '$';
      else if (!strcmp(v, "&"))
        list_hex_prefix = '&';
      else {
        fprintf(stderr, "invalid %s: %s (use none, $, or &)\n", a, v);
        return 2;
      }
      list_hex_prefix_cli = true;
      continue;
    }

    if (!strcmp(a, "--binprefix") || !strcmp(a, "--list-bin-prefix")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", a);
        usage();
        return 2;
      }
      const char *v = argv[++i];
      if (!strcmp(v, "none"))
        list_bin_prefix = 0;
      else if (!strcmp(v, "%"))
        list_bin_prefix = '%';
      else {
        fprintf(stderr, "invalid %s: %s (use none or %%)\n", a, v);
        return 2;
      }
      continue;
    }

    if (!strcmp(a, "--list-preserve-case")) {
      list_preserve_case = true;
      continue;
    }

    // Symbols
    if (!strcmp(a, "--sym") || !strcmp(a, "--symbols")) {
      sym_stdout = true;
      continue;
    }
    if (!strcmp(a, "--symfile") || !strcmp(a, "--symbols-file")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing filename after %s\n", a);
        usage();
        return 2;
      }
      sym_path = argv[++i];
      continue;
    }
    if (!strcmp(a, "--symsort")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", a);
        usage();
        return 2;
      }
      const char *v = argv[++i];
      if (!strcmp(v, "name"))
        sym_sort_addr = false;
      else if (!strcmp(v, "addr") || !strcmp(v, "address") ||
               !strcmp(v, "value"))
        sym_sort_addr = true;
      else {
        fprintf(stderr, "invalid %s: %s (use name or addr)\n", a, v);
        return 2;
      }
      continue;
    }
    if (!strcmp(a, "--symhexprefix")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing value after %s\n", a);
        usage();
        return 2;
      }

      const char *v = argv[++i];
      if (!strcmp(v, "none"))
        sym_hex_prefix = 0;
      else if (!strcmp(v, "$"))
        sym_hex_prefix = '$';
      else if (!strcmp(v, "&"))
        sym_hex_prefix = '&';
      else {
        fprintf(stderr, "invalid %s: %s (use none, $, or &)\n", a, v);
        return 2;
      }
      continue;
    }

    // Overwrite warnings
    if (!strcmp(a, "--no-overwrite-warn") || !strcmp(a, "--no-overwrite") ||
        !strcmp(a, "--no-overwrite-warnings")) {
      overwrite_warn = false;
      continue;
    }
    if (!strcmp(a, "--overwrite-warn-limit") ||
        !strcmp(a, "--overwrite-limit")) {
      if (i + 1 >= argc) {
        fprintf(stderr, "missing number after %s\n", a);
        usage();
        return 2;
      }
      overwrite_warn_limit = (unsigned)strtoul(argv[++i], NULL, 10);
      continue;
    }
    if (a[0] == '-') {
      fprintf(stderr, "unknown option: %s\n", a);
      usage();
      return 2;
    }
    if (!in_path)
      in_path = a;
    else {
      fprintf(stderr, "multiple input files specified\n");
      usage();
      return 2;
    }
  }

  if (!in_path) {
    usage();
    return 2;
  }

  source_t src;
  if (!source_load(in_path, &src)) {
    fprintf(stderr, "cannot read %s\n", in_path);
    free((void *)include_paths);
    return 1;
  }

  // 1) comment stripping (preserve line/col layout)
  source_strip_comments_preserve_layout(&src);

  // 2) preprocessor (#def and $var substitution)
  diags_t diags;
  diags_init(&diags);
  defs_t defs;
  defs_init(&defs);

  char *pp = preproc_run(in_path, src.text, &diags, &defs, &list_hex_prefix,
                         list_hex_prefix_cli, list_bin_prefix, include_paths,
                         include_path_count);

  int exit_code = 0;

  if (diags_has_errors(&diags)) {
    diags_print(&diags, pp);
    exit_code = 1;
    goto cleanup;
  }

  // Allow the source to override the active CPU via preprocessor-level ".cpu".
  // The preprocessor sets defs["cpu"] to "6502" or "6503" (65C02).
  const char *cpuv = defs_get(&defs, sv_from_cstr("cpu"));
  if (cpuv && (strcmp(cpuv, "6503") == 0 || strcmp(cpuv, "65c02") == 0 ||
               strcmp(cpuv, "65C02") == 0)) {
    cpu_name = "65c02";
  } else if (cpuv && strcmp(cpuv, "6502") == 0) {
    cpu_name = "6502";
  } else if (cpuv && strcmp(cpuv, "6502u") == 0) {
    // 6502u: base 6502 with undocumented opcodes
    cpu_name = "6502";
    allow_undoc = true;
  }

  asm_settings_t as = {0};
  as.cpu = asm_cpu_from_name(cpu_name);
  as.allow_undoc = allow_undoc;
  as.list_stdout = list_stdout;
  as.list_path = list_path;
  as.list_show_addr = list_show_addr;
  as.list_show_bytes = list_show_bytes;
  as.list_bytes_per_line = list_bytes_per_line;
  as.list_hex_prefix = list_hex_prefix;
  as.list_preserve_case = list_preserve_case;
  as.list_includes = list_includes;

  as.sym_stdout = sym_stdout;
  as.sym_path = sym_path;
  as.sym_sort_addr = sym_sort_addr;
  as.sym_hex_prefix = sym_hex_prefix;
  as.overwrite_warn = overwrite_warn;
  as.overwrite_warn_limit = overwrite_warn_limit;
  as.out_path = out_path;

  if (!out_path) {
    fprintf(stderr, "missing output file: use -o <file> or --out <file>\n");
    usage();
    exit_code = 2;
    goto cleanup;
  }

  {
    // Static so 128 KB of zero-init data lives in BSS, not on the stack.
    static uint8_t mem[65536];
    static uint8_t written[65536];

    // Deterministic output: clear the full 64KB image before assembling.
    // (The assembler only writes bytes it touches.)
    memset(mem, 0, sizeof(mem));
    memset(written, 0, sizeof(written));
    uint16_t minw = 0xFFFF, maxw = 0x0000;
    uint16_t out_start = 0x0000, out_end = 0xFFFF;

    if (!asm_assemble(in_path, pp, &as, start_expr, end_expr, mem, written, NULL, &minw,
                      &maxw, &out_start, &out_end, &diags)) {
      diags_print(&diags, pp);
      exit_code = 1;
      goto cleanup;
    }

    // Print warnings (e.g. overwrite warnings) even if assembly succeeds.
    if (diags_has_warnings(&diags)) {
      diags_print(&diags, pp);
    }

    uint16_t start = out_start;
    uint16_t end   = out_end;

    FILE *f = fopen(out_path, "wb");
    if (!f) {
      fprintf(stderr, "cannot open %s for writing\n", out_path);
      exit_code = 1;
      goto cleanup;
    }

    size_t want = (size_t)((uint32_t)end - (uint32_t)start + 1u);
    if (fwrite(mem + start, 1, want, f) != want) {
      fprintf(stderr, "write failed\n");
      fclose(f);
      exit_code = 1;
      goto cleanup;
    }
    fclose(f);

    /* Count distinct emitted bytes and print range list */
    uint32_t emitted = 0;
    for (int _i = 0; _i < 65536; _i++)
      if (written[_i])
        emitted++;
    /* Compute contiguous ranges */
    struct {
      uint16_t s, e;
      uint32_t n;
    } rngs[64];
    int nrngs = 0, in_r = 0;
    for (int _i = 0; _i <= 65535; _i++) {
      if (written[_i]) {
        if (!in_r && nrngs < 64) {
          rngs[nrngs].s = (uint16_t)_i;
          rngs[nrngs].n = 0;
          in_r = 1;
        }
        if (nrngs < 64)
          rngs[nrngs].n++;
      } else if (in_r) {
        rngs[nrngs].e = (uint16_t)(_i - 1);
        nrngs++;
        in_r = 0;
      }
    }
    if (in_r) {
      rngs[nrngs].e = 0xFFFF;
      nrngs++;
    }
    fprintf(stderr,
            "Assembled ok: %u bytes in %d range%s, wrote %zu bytes to %s\n",
            emitted, nrngs, nrngs == 1 ? "" : "s", want, out_path);
    for (int _ri = 0; _ri < nrngs; _ri++)
      fprintf(stderr, "  &%04X - &%04X   (%u byte%s)\n", rngs[_ri].s, rngs[_ri].e,
              rngs[_ri].n, rngs[_ri].n == 1 ? "" : "s");
  }

cleanup:
  free(pp);
  defs_free(&defs);
  diags_free(&diags);
  source_free(&src);
  free((void *)include_paths);
  return exit_code;
}