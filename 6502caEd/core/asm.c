#include "asm.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "expr.h"
#include "sym.h"
#include "util.h"

/* Parse a CPU name string and return the corresponding enum value.
 * Accepts: 6502, 6502u, 65c02, 65C02, 65816, 65c816, w65c816, 65816s.
 * Defaults to ASM_CPU_6502 for unrecognised names.
 * in:  s — CPU name string (may be NULL)
 * ret: asm_cpu_t enum value */
asm_cpu_t asm_cpu_from_name(const char *s) {
  if (!s)
    return ASM_CPU_6502;
  sv_t v = sv_from_cstr(s);
  if (sv_ieq(v, "65c02") || sv_ieq(v, "65C02"))
    return ASM_CPU_65C02;
  if (sv_ieq(v, "65816") || sv_ieq(v, "65c816") || sv_ieq(v, "65816s") ||
      sv_ieq(v, "w65c816"))
    return ASM_CPU_65816;
  return ASM_CPU_6502;
}

/* Table-driven keyword matching — avoids long sv_ieq chains. */
static bool sv_in_table(sv_t tok, const char *const *tbl, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (sv_ieq(tok, tbl[i]))
      return true;
  return false;
}
#define SV_IN(tok, ...)                                                        \
  sv_in_table(tok, (const char *const[]){__VA_ARGS__},                         \
              sizeof((const char *const[]){__VA_ARGS__}) /                     \
                  sizeof(const char *))

// Dot tokens that are directives (not dot-labels)
/* Return true if tok is a recognised assembler dot-directive keyword.
 * Used to distinguish dot-labels from directives during parsing. */
static bool is_dot_directive(sv_t tok) {
  static const char *const kw[] = {
      ".org",    ".pc",        ".pc=",      ".byte",  ".word",
      ".dword",  ".macro",     ".endmacro", ".align", "align",
      ".res",    "res",        ".include",  ".error", ".warning",
      ".assert", ".proc",      ".endproc",  ".scope", ".endscope",
      ".repeat", ".endrepeat", ".incbin",   ".longa", ".longi", /* 65816 */
      ".halt",   ".unhalt",   ".alert",    ".unalert",          /* guards */
      ".pragma",                                                   /* pragma once */
  };
  return sv_in_table(tok, kw, sizeof(kw) / sizeof(kw[0]));
}

/* Return true if tok is an include directive (.include or include). */
static bool is_include_tok(sv_t tok) {
  return SV_IN(tok, "include", ".include");
}

/* Return true if tok is a data-emission directive
 * (equb, .byte, equw, .word, equd, .dword, equs, db, dq, dd, etc.). */
static bool is_data_directive_tok(sv_t tok) {
  static const char *const kw[] = {
      "equb", "eqb", "db", "equw",   "eqw",   "dq",   ".word",
      "equd", "eqd", "dd", ".dword", ".byte", "equs",
  };
  return sv_in_table(tok, kw, sizeof(kw) / sizeof(kw[0]));
}

/* Return true if tok sets the program counter
 * (*=, .org, org, pc=, .pc=). */
static bool is_pc_set_tok(sv_t tok) {
  return SV_IN(tok, "*=", ".org", "org", "pc=", ".pc=");
}

/* Forward declaration — implemented after the opcode tables. */
static bool is_known_instruction_tok(sv_t tok, bool allow_undoc);

typedef struct {
  const char *p;        // cursor
  const char *line_end; // one past last char (before '\n')
  unsigned line_no;     // 1-based
} line_cursor_t;

/* Advance the cursor past whitespace (space, tab, CR) and update *col_io. */
static void skip_ws(line_cursor_t *c, unsigned *col_io) {
  while (c->p < c->line_end && is_ws(*c->p)) {
    c->p++;
    (*col_io)++;
  }
}

/* Consume an identifier token from the cursor, filling *out and advancing
 * the cursor.  Returns false if the cursor does not point at an identifier
 * start. out: *out    — string view of the consumed identifier *len_out —
 * length of the identifier (may be NULL) */
static bool take_ident(line_cursor_t *c, sv_t *out, unsigned *col_io,
                       unsigned *len_out) {
  const char *s = c->p;
  unsigned col = *col_io;
  if (s >= c->line_end)
    return false;
  if (!is_ident_start(*s))
    return false;
  s++;
  while (s < c->line_end && is_ident_char(*s))
    s++;
  out->p = c->p;
  out->len = (size_t)(s - c->p);
  if (len_out)
    *len_out = (unsigned)out->len;
  c->p = s;
  *col_io = col + (unsigned)out->len;
  return true;
}

/* Return true if tok is an equality directive (equ or =). */
static bool is_equ_tok(sv_t tok) {
  return sv_ieq(tok, "equ") || sv_ieq(tok, "=");
}

/* Peek ahead from cur to decide whether the preceding token is a colonless
 * label definition (as opposed to a mnemonic or directive at the start of
 * line). Returns true if the next non-whitespace token looks like it follows a
 * label: an instruction, directive, data keyword, PC-set, or known punctuation.
 * in:  allow_undoc — whether undocumented opcodes are enabled */
static bool is_probable_colonless_label_follow(const line_cursor_t *cur,
                                               bool allow_undoc) {
  line_cursor_t look = *cur;
  unsigned dummy_col = 1;
  skip_ws(&look, &dummy_col);
  if (look.p >= look.line_end)
    return true;

  if (*look.p == ';')
    return true;

  if (*look.p == '.' || *look.p == '*' || *look.p == '=')
    return true;

  sv_t next = {0};
  unsigned next_len = 0;
  line_cursor_t after = look;
  if (!take_ident(&after, &next, &dummy_col, &next_len))
    return false;

  return is_known_instruction_tok(next, allow_undoc) ||
         is_data_directive_tok(next) || is_include_tok(next) ||
         is_pc_set_tok(next) || is_equ_tok(next) ||
         SV_IN(next, "cpu", ".cpu", "macro", ".macro", "endmacro", ".endmacro",
               "if", ".if", "ifdef", ".ifdef", "ifndef", ".ifndef", "else",
               ".else", "endif", ".endif", "warning", ".warning", "error",
               ".error", "assert", ".assert", "proc", ".proc", "endproc",
               ".endproc", "align", ".align", "res", ".res", ".scope",
               ".endscope", ".repeat", ".endrepeat",
               "halt", ".halt", "unhalt", ".unhalt",
               "alert", ".alert", "unalert", ".unalert",
               "pragma", ".pragma");
}

/* Return true if the next character in the cursor is ch (without consuming it).
 */
static bool peek_char(line_cursor_t *c, char ch) {
  return (c->p < c->line_end) && (*c->p == ch);
}

/* Stage-1 parse-only pass over the preprocessed source.
 * Processes labels, directives, and operands to build the symbol table and
 * track the program counter, without emitting any code.
 * Used by the editor for live error checking without full assembly.
 * in:  file              — source file path (for diagnostics)
 *      preprocessed_text — text after preprocessing
 *      settings          — assembler settings (CPU, etc.)
 * out: *diags            — errors and warnings
 * ret: false if any errors were emitted */
bool asm_parse_only(const char *file, const char *text,
                    const asm_settings_t *settings, diags_t *diags) {
  (void)settings;
  if (!file)
    file = "<input>";
  if (!text)
    return true;

  symtab_t syms;
  symtab_init(&syms);
  uint16_t pc = 0;

  // Anonymous label support:
  //   ':'  defines an anonymous label at current PC
  //   :+ / :- refer to next/previous anonymous label
  //   ++ / -- are ca65-style aliases for :+ / :-
  //   '@:' is accepted as an alias for ':' (ACME-style local label marker)
  uint16_t *anon_addrs = NULL;
  size_t anon_count = 0;
  size_t anon_cap = 0;
  size_t anon_here = 0;

  const char *p = text;
  unsigned line_no = 1;

  while (*p) {
    const char *line_start = p;
    /* Line-number reset marker from preprocessor macro expansion */
    if ((unsigned char)*line_start == 0x1f) {
      const char *line_end = line_start + 1;
      while (*line_end && *line_end != '\n')
        line_end++;
      {
        char *_ae;
        unsigned long _al = strtoul(line_start + 1, &_ae, 10);
        line_no = (_ae != line_start + 1) ? (unsigned)_al : line_no;
      }
      p = (*line_end == '\n') ? line_end + 1 : line_end;
      continue;
    }
    if (*line_start == '\x1e')
      line_start++;
    const char *line_end = p;
    while (*line_end && *line_end != '\n')
      line_end++;

    // Setup cursor
    line_cursor_t cur = {line_start, line_end, line_no};
    unsigned col = 1;

    skip_ws(&cur, &col);

    // Anonymous label definition at start of line: ':' or '@:' or '.' (dot)
    // Note: '.' only counts as an anonymous label if it stands alone (followed
    // by whitespace/end-of-line/comment), so it won't conflict with directives
    // like '.cpu' or dot-labels like '.loop'.
    if (cur.p < cur.line_end) {
      bool anon_is_colon = (*cur.p == ':');
      bool anon_is_at_colon =
          (*cur.p == '@' && (cur.p + 1) < cur.line_end && cur.p[1] == ':');
      bool anon_is_dot = false;
      if (*cur.p == '.') {
        const char *n = cur.p + 1;
        if (n >= cur.line_end) {
          anon_is_dot = true;
        } else {
          char nc = *n;
          anon_is_dot = is_ws(nc) || nc == '\r' || nc == ';' || nc == '/';
        }
      }
      if (anon_is_colon || anon_is_at_colon || anon_is_dot) {
        unsigned take = anon_is_at_colon ? 2u : 1u;
        unsigned anon_col = col;
        cur.p += take;
        col += take;

        // record anonymous label address
        if (anon_count == anon_cap) {
          size_t nc = anon_cap ? (anon_cap * 2) : 32;
          uint16_t *na = (uint16_t *)realloc(anon_addrs, nc * sizeof(uint16_t));
          if (!na) {
            diag_addf(diags, DIAG_ERROR, file, line_no, anon_col, take,
                      "out of memory");
            free(anon_addrs);
            symtab_free(&syms);
            return false;
          }
          anon_addrs = na;
          anon_cap = nc;
        }
        anon_addrs[anon_count++] = pc;
        anon_here = anon_count;

        // allow label-only line
        skip_ws(&cur, &col);
        if (cur.p >= cur.line_end) {
          p = (*line_end == '\n') ? line_end + 1 : line_end;
          line_no++;
          continue;
        }
      }
    }

    const char *tok0 = cur.p;
    unsigned tok0_col = col;
    if (cur.p >= cur.line_end) {
      // empty line
      p = (*line_end == '\n') ? line_end + 1 : line_end;
      line_no++;
      continue;
    }

    sv_t label = {0};
    unsigned label_col = col;
    unsigned label_len = 0;

    // Label detection: "label:" OR ".label" (no colon), but NOT ".label:"
    // Also: "@name" or "@name:" -- local label scoped to nearest global label.
    sv_t first = {0};
    unsigned first_len = 0;
    const char *save_p = cur.p;
    unsigned save_col = col;

    // @ local label: @ident or @ident:
    if (cur.p < cur.line_end && *cur.p == '@') {
      const char *at_p = cur.p;
      unsigned at_col = col;
      const char *n = cur.p + 1;
      if (n < cur.line_end && is_ident_start(*n)) {
        cur.p++;
        col++;
        if (take_ident(&cur, &first, &col, &first_len)) {
          first.p = at_p;
          first.len++;
          first_len++;
          if (peek_char(&cur, ':')) {
            label = first;
            label_col = at_col;
            label_len = (unsigned)first.len + 1;
            cur.p++;
            col++;
          } else {
            label = first;
            label_col = at_col;
            label_len = (unsigned)first.len;
          }
        } else {
          cur.p = save_p;
          col = save_col;
        }
      }
    }

    if (!label.len && take_ident(&cur, &first, &col, &first_len)) {
      if (first.len && first.p[0] == '.') {
        if (peek_char(&cur, ':')) {
          // explicitly disallow .label:
          diag_addf(
              diags, DIAG_ERROR, file, line_no, save_col,
              (unsigned)first.len + 1,
              "dot-labels must not end with ':' (use '.label' or 'label:')");
          // consume ':' to avoid cascades
          cur.p++;
          col++;
        } else if (!is_dot_directive(first)) {
          label = first;
          label_col = save_col;
          label_len = (unsigned)label.len;
        } else {
          // It's a directive; rewind so it becomes mnemonic.
          cur.p = save_p;
          col = save_col;
        }
      } else {
        // check for immediate ':' after token
        if (peek_char(&cur, ':')) {
          label = first;
          label_col = save_col;
          label_len = (unsigned)label.len + 1;
          cur.p++;
          col++; // consume ':'
        } else if (!is_known_instruction_tok(first, settings->allow_undoc) &&
                   is_probable_colonless_label_follow(&cur,
                                                      settings->allow_undoc)) {
          label = first;
          label_col = save_col;
          label_len = (unsigned)label.len;
        } else {
          // Not a label; rewind so it becomes mnemonic.
          cur.p = save_p;
          col = save_col;
        }
      }
    }

    // If the line looks like an attempted "label:" but the label token isn't a
    // valid identifier, report a clear error (e.g. "^Rt$:" should not become
    // "unknown instruction").
    if (!label.len && tok0 < cur.line_end && tok0[0] != '.' && tok0[0] != '@') {
      const char *q = tok0;
      // scan first token (up to whitespace or ';')
      while (q < cur.line_end && *q != ';' && !is_ws(*q))
        q++;
      if (q > tok0 && q[-1] == ':') {
        const char *name_end = q - 1;
        unsigned i = 0;
        for (const char *r = tok0; r < name_end; r++, i++) {
          char ch = *r;
          bool ok = (i == 0) ? is_ident_start(ch) : is_ident_char(ch);
          if (!ok) {
            unsigned err_col = tok0_col + i;
            diag_addf(diags, DIAG_ERROR, file, line_no, err_col, 1,
                      "illegal character(s) in label name");
            // skip rest of line
            cur.p = cur.line_end;
            break;
          }
        }
      }
    }

    if (label.len) {
      // define label -> current pc (normalize dot-labels)
      sv_t k = label;
      if (k.len && k.p[0] == '.') {
        k.p++;
        k.len--;
      } // <--- ADD THIS

      // @ local label: mangle as "parent::@name" using last seen global label.
      char at_scoped[256];
      if (k.len && k.p[0] == '@') {
        // In parse_only we don't track parent_label, so just store bare @name.
        // Resolution in expr.c handles the scoping at eval time.
        // (parse_only doesn't evaluate operand expressions for sizing.)
      }

      // 'cpu' is a predefined symbol reflecting the active .cpu; don't allow it
      // to be redefined as a label.
      if (sv_ieq(k, "cpu")) {
        diag_addf(diags, DIAG_ERROR, file, line_no, label_col,
                  label_len         ? label_len
                  : (unsigned)k.len ? (unsigned)k.len
                                    : 1,
                  "label '%.*s' is reserved (predefined CPU symbol)",
                  (int)k.len, k.p);
      } else {
        int32_t prev = 0;
        if (symtab_get(&syms, k, &prev)) {
          diag_addf(diags, DIAG_ERROR, file, line_no, label_col,
                    label_len         ? label_len
                    : (unsigned)k.len ? (unsigned)k.len
                                      : 1,
                    "duplicate label '%.*s'", (int)k.len, k.p);
        } else if (!symtab_put(&syms, k, pc)) { // label -> k
          diag_addf(diags, DIAG_ERROR, file, line_no, label_col,
                    label_len ? label_len : 1,
                    "out of memory while storing label");
          symtab_free(&syms);
          return false;
        }
      }
      skip_ws(&cur, &col);
      if (cur.p >= cur.line_end) {
        // label-only line
        p = (*line_end == '\n') ? line_end + 1 : line_end;
        line_no++;
        continue;
      }
    }

    // Parse mnemonic/directive token. Also handle PC-setter forms specially.
    sv_t mnem = {0};
    unsigned mnem_len = 0;

    // Accept both "*=" and "* =" (optional whitespace around '=')
    if (peek_char(&cur, '*')) {
      const char *save_p = cur.p;
      unsigned save_col = col;
      cur.p++;
      col++;
      skip_ws(&cur, &col);
      if (peek_char(&cur, '=')) {
        static const char k_star_eq[] = "*=";
        cur.p++;
        col++;
        mnem.p = k_star_eq;
        mnem.len = 2;
        mnem_len = 2;
      } else {
        // Not a PC-setter; rewind and treat '*' as start of mnemonic token.
        cur.p = save_p;
        col = save_col;
      }
    }

    if (mnem.len == 0) {
      if (!take_ident(&cur, &mnem, &col, &mnem_len)) {
        diag_addf(diags, DIAG_ERROR, file, line_no, col, 1,
                  "expected instruction or directive");
        p = (*line_end == '\n') ? line_end + 1 : line_end;
        line_no++;
        continue;
      }

      // Allow "pc=" and ".pc=" with optional whitespace: "pc =" / ".pc ="
      if (sv_ieq(mnem, "pc") || sv_ieq(mnem, ".pc")) {
        const char *save_p = cur.p;
        unsigned save_col = col;
        skip_ws(&cur, &col);
        if (peek_char(&cur, '=')) {
          static const char k_pc_eq[] = "pc=";
          static const char k_dotpc_eq[] = ".pc=";
          cur.p++;
          col++;
          if (sv_ieq(mnem, "pc")) {
            mnem.p = k_pc_eq;
            mnem.len = 3;
            mnem_len = 3;
          } else {
            mnem.p = k_dotpc_eq;
            mnem.len = 4;
            mnem_len = 4;
          }
        } else {
          // No '=', restore position.
          cur.p = save_p;
          col = save_col;
        }
      }

      // Allow "org" and ".org" with optional whitespace around '=': "org ="
      // This is syntactic sugar; internally it is treated the same as "org".
      if (sv_ieq(mnem, "org") || sv_ieq(mnem, ".org")) {
        const char *save_p = cur.p;
        unsigned save_col = col;
        skip_ws(&cur, &col);
        if (peek_char(&cur, '=')) {
          cur.p++;
          col++;
        } else {
          cur.p = save_p;
          col = save_col;
        }
      }
    }

    skip_ws(&cur, &col);
    sv_t operand = {0};
    unsigned op_col = col;
    if (cur.p < cur.line_end) {
      operand.p = cur.p;
      operand.len = (size_t)(cur.line_end - cur.p);
      operand = sv_trim(operand);
    }

    // Directive handling: PC setters + data emit sizing (pass-1 PC tracking)
    // PC setters: '*=' '.org' 'org' 'pc=' '.pc='
    bool is_pc_set = false;
    if (sv_ieq(mnem, "*="))
      is_pc_set = true;
    if (sv_ieq(mnem, ".org") || sv_ieq(mnem, "org"))
      is_pc_set = true;
    if (sv_ieq(mnem, "pc=") || sv_ieq(mnem, ".pc="))
      is_pc_set = true;

    if (is_pc_set) {
      if (operand.len == 0) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col, 1,
                  "missing address expression");
      } else {
        expr_val_t v = {0};
        bool unr = false;
        // Allow forward references during layout.
        if (expr_eval(file, line_no, op_col, operand, &syms, anon_addrs,
                      anon_count, anon_here, true, &unr, &v, diags, NULL,
                      NULL)) {
          if (v.kind != EXPR_VAL_NUM) {
            diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                      (unsigned)operand.len,
                      "address expression must be numeric");
          } else if (!in_u16(v.num)) {
            diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                      (unsigned)operand.len, "address out of range (0..65535)");
          } else {
            pc = (uint16_t)v.num;
          }
        }
        expr_val_free(&v);
      }
    } else {
      // Data directives (aliases). Case-insensitive.
      // byte:  equb, eqb, db
      // word:  equw, eqw, dq, .word
      // dword: equd, eqd, dd, .dword
      // special: .byte (mixed list, numbers auto-size to 1/2/4; strings emit
      // bytes)
      bool is_byte_dir =
          sv_ieq(mnem, "equb") || sv_ieq(mnem, "eqb") || sv_ieq(mnem, "db");
      bool is_word_dir = sv_ieq(mnem, "equw") || sv_ieq(mnem, "eqw") ||
                         sv_ieq(mnem, "dq") || sv_ieq(mnem, ".word");
      bool is_dword_dir = sv_ieq(mnem, "equd") || sv_ieq(mnem, "eqd") ||
                          sv_ieq(mnem, "dd") || sv_ieq(mnem, ".dword");
      bool is_special_byte = sv_ieq(mnem, ".byte");

      if (is_byte_dir || is_word_dir || is_dword_dir || is_special_byte) {
        if (operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, file, line_no, op_col, 1,
                    "missing data operand");
        } else {
          const char *s = operand.p;
          const char *e = operand.p + operand.len;
          const char *item_start = s;

          while (item_start < e) {
            // find end of this item (comma at depth0, not inside quotes)
            const char *q = item_start;
            int paren = 0;
            bool in_str = false;
            while (q < e) {
              char cch = *q;
              if (cch == '"') {
                in_str = !in_str;
              } else if (!in_str) {
                if (cch == '(')
                  paren++;
                else if (cch == ')' && paren > 0)
                  paren--;
                else if (cch == ',' && paren == 0)
                  break;
              }
              q++;
            }

            const char *item_end = q;

            // trim whitespace
            while (item_start < item_end &&
                   (*item_start == ' ' || *item_start == '\t' ||
                    *item_start == '\r'))
              item_start++;
            while (item_end > item_start &&
                   (item_end[-1] == ' ' || item_end[-1] == '\t' ||
                    item_end[-1] == '\r'))
              item_end--;

            if (item_end == item_start) {
              unsigned ccol = op_col + (unsigned)(item_start - s);
              diag_addf(diags, DIAG_ERROR, file, line_no, ccol, 1,
                        "empty item in data list");
            } else {
              sv_t item = {item_start, (size_t)(item_end - item_start)};
              unsigned item_col = op_col + (unsigned)(item_start - s);
              expr_val_t v = {0};
              bool unr = false;
              if (expr_eval(file, line_no, item_col, item, &syms, anon_addrs,
                            anon_count, anon_here, true, &unr, &v, diags, NULL,
                            NULL)) {
                if (v.kind == EXPR_VAL_STR) {
                  if (!is_special_byte) {
                    diag_addf(diags, DIAG_ERROR, file, line_no, item_col,
                              (unsigned)item.len,
                              "string not allowed in this directive (use .byte "
                              "for mixed data)");
                  } else {
                    // .byte: strings emit bytes verbatim
                    pc = (uint16_t)(pc + (uint16_t)v.str_len);
                  }
                } else {
                  if (!unr && v.num < 0) {
                    diag_addf(diags, DIAG_ERROR, file, line_no, item_col,
                              (unsigned)item.len, "numeric value must be >= 0");
                  } else {
                    unsigned w = 0;
                    if (unr) {
                      // Forward reference — assume minimum fixed width for
                      // this directive; pass 2 will validate the resolved
                      // value.
                      if (is_byte_dir)
                        w = 1u;
                      else if (is_word_dir)
                        w = 2u;
                      else if (is_dword_dir)
                        w = 4u;
                      else
                        w = 4u; // .byte worst-case
                    } else if (is_byte_dir)
                      w = (v.num <= 0xFF) ? 1u : 0u;
                    else if (is_word_dir)
                      w = (v.num <= 0xFFFF) ? 2u : 0u;
                    else if (is_dword_dir)
                      w = (v.num <= 0xFFFFFFFFLL) ? 4u : 0u;
                    else /* .byte */ {
                      w = smallest_width_u32(v.num);
                    }

                    if (w == 0) {
                      diag_addf(diags, DIAG_ERROR, file, line_no, item_col,
                                (unsigned)item.len,
                                "numeric value out of range for directive");
                    } else {
                      pc = (uint16_t)(pc + (uint16_t)w);
                    }
                  }
                }
              }
              expr_val_free(&v);
            }

            if (q >= e)
              break; // no comma
            q++;     // consume comma
            item_start = q;
          }
        }
      }
    }

    // .align N — advance PC to next multiple of N
    if (sv_ieq(mnem, ".align") || sv_ieq(mnem, "align")) {
      if (operand.len == 0) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col, 1,
                  ".align requires a power-of-2 operand");
      } else {
        expr_val_t v = {0};
        bool unr = false;
        if (expr_eval(file, line_no, op_col, operand, &syms, anon_addrs,
                      anon_count, anon_here, true, &unr, &v, diags, NULL,
                      NULL)) {
          if (!unr) {
            uint16_t n = (uint16_t)v.num;
            if (n < 2 || (n & (n - 1)) != 0) {
              diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                        (unsigned)operand.len,
                        ".align value must be a power of 2 (2, 4, 8, ...)");
            } else {
              uint16_t pad = (uint16_t)((n - (pc & (n - 1))) & (n - 1));
              pc = (uint16_t)(pc + pad);
            }
          }
        }
        expr_val_free(&v);
      }
    }

    // .res N [, fill] — reserve N bytes
    if (sv_ieq(mnem, ".res") || sv_ieq(mnem, "res")) {
      if (operand.len == 0) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col, 1,
                  ".res requires a byte count");
      } else {
        // Split operand on first comma to get count (fill is ignored for
        // sizing)
        sv_t count_sv = operand;
        const char *comma = operand.p;
        while (comma < operand.p + operand.len && *comma != ',')
          comma++;
        count_sv.len = (size_t)(comma - operand.p);
        // trim right
        while (count_sv.len > 0 && (count_sv.p[count_sv.len - 1] == ' ' ||
                                    count_sv.p[count_sv.len - 1] == '\t'))
          count_sv.len--;
        expr_val_t v = {0};
        bool unr = false;
        if (expr_eval(file, line_no, op_col, count_sv, &syms, anon_addrs,
                      anon_count, anon_here, true, &unr, &v, diags, NULL,
                      NULL)) {
          if (!unr)
            pc = (uint16_t)(pc + (uint16_t)v.num);
        }
        expr_val_free(&v);
      }
    }

    // TODO: instruction sizing + IR for pass2 encode
    // TODO: data directives sizing + instruction sizing (pass1)
    // TODO: macro expansion stage will run before this parse stage

    p = (*line_end == '\n') ? line_end + 1 : line_end;
    line_no++;
  }

  free(anon_addrs);
  symtab_free(&syms);
  return !diags_has_errors(diags);
}

// ----------------------------
// Stage-2: full assembly
// ----------------------------

typedef enum {
  LK_EMPTY = 0,
  LK_PC_SET,
  LK_EQU,
  LK_OPT,
  LK_DIAG,
  LK_ASSERT, // .assert <expr>, <error|warning|ldwarning>, "message"
  LK_DATA,
  LK_INSTR,
  LK_PROC,     // .proc Name
  LK_ENDPROC,  // .endproc
  LK_SCOPE,    // .scope [Name]
  LK_ENDSCOPE, // .endscope
  LK_ALIGN,    // .align N
  LK_RES,      // .res N [, fill]
  LK_INCBIN,   // .incbin "file" [, offset [, length]]
  LK_LONGA,    // .longa on|off  (65816: 16-bit accumulator)
  LK_LONGI,    // .longi on|off  (65816: 16-bit index registers)
  LK_GUARD,    // .halt/.alert addr[-addr] "msg" / .unhalt/.unalert addr
} line_kind_t;

typedef struct {
  const char *file;
  unsigned line_no;
  unsigned label_col;
  unsigned mnem_col;
  unsigned op_col;
  sv_t label; // as written (\".label\" or \"label\")
  bool label_has_colon;
  bool anon_label_def; // ':' or '@:' at start of line
  unsigned anon_label_col;
  unsigned anon_label_len;
  sv_t mnem;
  sv_t operand;
  sv_t src; // trimmed original source text for this line (after preproc/comment
            // stripping)
  uint16_t pc;       // address at start of line in pass2
  uint16_t emit_len; // number of bytes emitted by this line in pass2
  bool from_macro;   // line was generated by macro expansion (preproc sentinel)
  line_kind_t kind;
  uint8_t opt_level; // BBC OPT level in effect for this line (0..2). Affects
                     // listing only.
} line_rec_t;

typedef enum {
  AM_NONE = 0,
  AM_IMP,
  AM_ACC,
  AM_IMM,
  AM_ZP,
  AM_ZPX,
  AM_ZPY,
  AM_ABS,
  AM_ABSX,
  AM_ABSY,
  AM_IND,  // (abs) or (zp) 65c02
  AM_INDX, // (zp,X)
  AM_INDY, // (zp),Y
  AM_REL,
  AM_ZPIND,   // (zp) 65c02, distinguished from AM_IND for JMP (abs)
  AM_ABSINDX, // (abs,X) 65c02 JMP
  AM_ZPREL,   // zp,rel (BBR/BBS)
  AM_LONG,    // $AABBCC   24-bit absolute
  AM_LONGX,   // $AABBCC,X 24-bit absolute indexed X
  AM_SR,      // nn,S      stack-relative
  AM_SRINY,   // (nn,S),Y  stack-relative indirect indexed Y
  AM_INDL,    // [dp]      direct-page indirect long
  AM_INDLY,   // [dp],Y    direct-page indirect long indexed Y
  AM_ABSL,    // [abs]     absolute indirect long (JML)
  AM_RELL,    // long relative (BRL)
  AM_BLK,     // srcbank,dstbank  block move (MVN/MVP)
} addrmode_t;

typedef struct {
  const char *mn;
  addrmode_t am;
  uint8_t cpu_mask; // bit0=6502, bit1=65c02
  uint8_t opcode;
  uint8_t bytes;
} op_t;

#define CPU_6502 (1u << 0)
#define CPU_65C02 (1u << 1)
#define CPU_65816 (1u << 2)

// Note: This table includes official 6502 instructions and the standard
// WDC/Rockwell 65C02 additions. It does NOT include undocumented/illegal
// opcodes.
static const op_t g_ops[] = {
    // ADC
    {"ADC", AM_IMM, CPU_6502 | CPU_65C02, 0x69, 2},
    {"ADC", AM_ZP, CPU_6502 | CPU_65C02, 0x65, 2},
    {"ADC", AM_ZPX, CPU_6502 | CPU_65C02, 0x75, 2},
    {"ADC", AM_ABS, CPU_6502 | CPU_65C02, 0x6D, 3},
    {"ADC", AM_ABSX, CPU_6502 | CPU_65C02, 0x7D, 3},
    {"ADC", AM_ABSY, CPU_6502 | CPU_65C02, 0x79, 3},
    {"ADC", AM_INDX, CPU_6502 | CPU_65C02, 0x61, 2},
    {"ADC", AM_INDY, CPU_6502 | CPU_65C02, 0x71, 2},
    {"ADC", AM_ZPIND, CPU_65C02, 0x72, 2},

    // AND
    {"AND", AM_IMM, CPU_6502 | CPU_65C02, 0x29, 2},
    {"AND", AM_ZP, CPU_6502 | CPU_65C02, 0x25, 2},
    {"AND", AM_ZPX, CPU_6502 | CPU_65C02, 0x35, 2},
    {"AND", AM_ABS, CPU_6502 | CPU_65C02, 0x2D, 3},
    {"AND", AM_ABSX, CPU_6502 | CPU_65C02, 0x3D, 3},
    {"AND", AM_ABSY, CPU_6502 | CPU_65C02, 0x39, 3},
    {"AND", AM_INDX, CPU_6502 | CPU_65C02, 0x21, 2},
    {"AND", AM_INDY, CPU_6502 | CPU_65C02, 0x31, 2},
    {"AND", AM_ZPIND, CPU_65C02, 0x32, 2},

    // ASL
    {"ASL", AM_ACC, CPU_6502 | CPU_65C02, 0x0A, 1},
    {"ASL", AM_ZP, CPU_6502 | CPU_65C02, 0x06, 2},
    {"ASL", AM_ZPX, CPU_6502 | CPU_65C02, 0x16, 2},
    {"ASL", AM_ABS, CPU_6502 | CPU_65C02, 0x0E, 3},
    {"ASL", AM_ABSX, CPU_6502 | CPU_65C02, 0x1E, 3},

    // Branches
    {"BCC", AM_REL, CPU_6502 | CPU_65C02, 0x90, 2},
    {"BCS", AM_REL, CPU_6502 | CPU_65C02, 0xB0, 2},
    {"BEQ", AM_REL, CPU_6502 | CPU_65C02, 0xF0, 2},
    {"BMI", AM_REL, CPU_6502 | CPU_65C02, 0x30, 2},
    {"BNE", AM_REL, CPU_6502 | CPU_65C02, 0xD0, 2},
    {"BPL", AM_REL, CPU_6502 | CPU_65C02, 0x10, 2},
    {"BVC", AM_REL, CPU_6502 | CPU_65C02, 0x50, 2},
    {"BVS", AM_REL, CPU_6502 | CPU_65C02, 0x70, 2},
    {"BRA", AM_REL, CPU_65C02, 0x80, 2},

    // BIT
    {"BIT", AM_ZP, CPU_6502 | CPU_65C02, 0x24, 2},
    {"BIT", AM_ABS, CPU_6502 | CPU_65C02, 0x2C, 3},
    {"BIT", AM_IMM, CPU_65C02, 0x89, 2},
    {"BIT", AM_ZPX, CPU_65C02, 0x34, 2},
    {"BIT", AM_ABSX, CPU_65C02, 0x3C, 3},

    // BRK/RTI/RTS
    {"BRK", AM_IMP, CPU_6502 | CPU_65C02, 0x00, 1},
    {"RTI", AM_IMP, CPU_6502 | CPU_65C02, 0x40, 1},
    {"RTS", AM_IMP, CPU_6502 | CPU_65C02, 0x60, 1},

    // CLC/CLD/CLI/CLV
    {"CLC", AM_IMP, CPU_6502 | CPU_65C02, 0x18, 1},
    {"CLD", AM_IMP, CPU_6502 | CPU_65C02, 0xD8, 1},
    {"CLI", AM_IMP, CPU_6502 | CPU_65C02, 0x58, 1},
    {"CLV", AM_IMP, CPU_6502 | CPU_65C02, 0xB8, 1},

    // CMP
    {"CMP", AM_IMM, CPU_6502 | CPU_65C02, 0xC9, 2},
    {"CMP", AM_ZP, CPU_6502 | CPU_65C02, 0xC5, 2},
    {"CMP", AM_ZPX, CPU_6502 | CPU_65C02, 0xD5, 2},
    {"CMP", AM_ABS, CPU_6502 | CPU_65C02, 0xCD, 3},
    {"CMP", AM_ABSX, CPU_6502 | CPU_65C02, 0xDD, 3},
    {"CMP", AM_ABSY, CPU_6502 | CPU_65C02, 0xD9, 3},
    {"CMP", AM_INDX, CPU_6502 | CPU_65C02, 0xC1, 2},
    {"CMP", AM_INDY, CPU_6502 | CPU_65C02, 0xD1, 2},
    {"CMP", AM_ZPIND, CPU_65C02, 0xD2, 2},

    // CPX/CPY
    {"CPX", AM_IMM, CPU_6502 | CPU_65C02, 0xE0, 2},
    {"CPX", AM_ZP, CPU_6502 | CPU_65C02, 0xE4, 2},
    {"CPX", AM_ABS, CPU_6502 | CPU_65C02, 0xEC, 3},
    {"CPY", AM_IMM, CPU_6502 | CPU_65C02, 0xC0, 2},
    {"CPY", AM_ZP, CPU_6502 | CPU_65C02, 0xC4, 2},
    {"CPY", AM_ABS, CPU_6502 | CPU_65C02, 0xCC, 3},

    // DEC/INC (incl 65c02 acc)
    {"DEC", AM_ZP, CPU_6502 | CPU_65C02, 0xC6, 2},
    {"DEC", AM_ZPX, CPU_6502 | CPU_65C02, 0xD6, 2},
    {"DEC", AM_ABS, CPU_6502 | CPU_65C02, 0xCE, 3},
    {"DEC", AM_ABSX, CPU_6502 | CPU_65C02, 0xDE, 3},
    {"DEC", AM_ACC, CPU_65C02, 0x3A, 1},
    {"INC", AM_ZP, CPU_6502 | CPU_65C02, 0xE6, 2},
    {"INC", AM_ZPX, CPU_6502 | CPU_65C02, 0xF6, 2},
    {"INC", AM_ABS, CPU_6502 | CPU_65C02, 0xEE, 3},
    {"INC", AM_ABSX, CPU_6502 | CPU_65C02, 0xFE, 3},
    {"INC", AM_ACC, CPU_65C02, 0x1A, 1},

    // DEX/DEY/INX/INY
    {"DEX", AM_IMP, CPU_6502 | CPU_65C02, 0xCA, 1},
    {"DEY", AM_IMP, CPU_6502 | CPU_65C02, 0x88, 1},
    {"INX", AM_IMP, CPU_6502 | CPU_65C02, 0xE8, 1},
    {"INY", AM_IMP, CPU_6502 | CPU_65C02, 0xC8, 1},

    // EOR
    {"EOR", AM_IMM, CPU_6502 | CPU_65C02, 0x49, 2},
    {"EOR", AM_ZP, CPU_6502 | CPU_65C02, 0x45, 2},
    {"EOR", AM_ZPX, CPU_6502 | CPU_65C02, 0x55, 2},
    {"EOR", AM_ABS, CPU_6502 | CPU_65C02, 0x4D, 3},
    {"EOR", AM_ABSX, CPU_6502 | CPU_65C02, 0x5D, 3},
    {"EOR", AM_ABSY, CPU_6502 | CPU_65C02, 0x59, 3},
    {"EOR", AM_INDX, CPU_6502 | CPU_65C02, 0x41, 2},
    {"EOR", AM_INDY, CPU_6502 | CPU_65C02, 0x51, 2},
    {"EOR", AM_ZPIND, CPU_65C02, 0x52, 2},

    // JMP/JSR
    {"JMP", AM_ABS, CPU_6502 | CPU_65C02, 0x4C, 3},
    {"JMP", AM_IND, CPU_6502 | CPU_65C02, 0x6C, 3},
    {"JMP", AM_ABSINDX, CPU_65C02, 0x7C, 3},
    {"JSR", AM_ABS, CPU_6502 | CPU_65C02, 0x20, 3},

    // LDA
    {"LDA", AM_IMM, CPU_6502 | CPU_65C02, 0xA9, 2},
    {"LDA", AM_ZP, CPU_6502 | CPU_65C02, 0xA5, 2},
    {"LDA", AM_ZPX, CPU_6502 | CPU_65C02, 0xB5, 2},
    {"LDA", AM_ABS, CPU_6502 | CPU_65C02, 0xAD, 3},
    {"LDA", AM_ABSX, CPU_6502 | CPU_65C02, 0xBD, 3},
    {"LDA", AM_ABSY, CPU_6502 | CPU_65C02, 0xB9, 3},
    {"LDA", AM_INDX, CPU_6502 | CPU_65C02, 0xA1, 2},
    {"LDA", AM_INDY, CPU_6502 | CPU_65C02, 0xB1, 2},
    {"LDA", AM_ZPIND, CPU_65C02, 0xB2, 2},

    // LDX
    {"LDX", AM_IMM, CPU_6502 | CPU_65C02, 0xA2, 2},
    {"LDX", AM_ZP, CPU_6502 | CPU_65C02, 0xA6, 2},
    {"LDX", AM_ZPY, CPU_6502 | CPU_65C02, 0xB6, 2},
    {"LDX", AM_ABS, CPU_6502 | CPU_65C02, 0xAE, 3},
    {"LDX", AM_ABSY, CPU_6502 | CPU_65C02, 0xBE, 3},

    // LDY
    {"LDY", AM_IMM, CPU_6502 | CPU_65C02, 0xA0, 2},
    {"LDY", AM_ZP, CPU_6502 | CPU_65C02, 0xA4, 2},
    {"LDY", AM_ZPX, CPU_6502 | CPU_65C02, 0xB4, 2},
    {"LDY", AM_ABS, CPU_6502 | CPU_65C02, 0xAC, 3},
    {"LDY", AM_ABSX, CPU_6502 | CPU_65C02, 0xBC, 3},

    // LSR
    {"LSR", AM_ACC, CPU_6502 | CPU_65C02, 0x4A, 1},
    {"LSR", AM_ZP, CPU_6502 | CPU_65C02, 0x46, 2},
    {"LSR", AM_ZPX, CPU_6502 | CPU_65C02, 0x56, 2},
    {"LSR", AM_ABS, CPU_6502 | CPU_65C02, 0x4E, 3},
    {"LSR", AM_ABSX, CPU_6502 | CPU_65C02, 0x5E, 3},

    // NOP
    {"NOP", AM_IMP, CPU_6502 | CPU_65C02, 0xEA, 1},

    // ORA
    {"ORA", AM_IMM, CPU_6502 | CPU_65C02, 0x09, 2},
    {"ORA", AM_ZP, CPU_6502 | CPU_65C02, 0x05, 2},
    {"ORA", AM_ZPX, CPU_6502 | CPU_65C02, 0x15, 2},
    {"ORA", AM_ABS, CPU_6502 | CPU_65C02, 0x0D, 3},
    {"ORA", AM_ABSX, CPU_6502 | CPU_65C02, 0x1D, 3},
    {"ORA", AM_ABSY, CPU_6502 | CPU_65C02, 0x19, 3},
    {"ORA", AM_INDX, CPU_6502 | CPU_65C02, 0x01, 2},
    {"ORA", AM_INDY, CPU_6502 | CPU_65C02, 0x11, 2},
    {"ORA", AM_ZPIND, CPU_65C02, 0x12, 2},

    // PHA/PHP/PLA/PLP
    {"PHA", AM_IMP, CPU_6502 | CPU_65C02, 0x48, 1},
    {"PHP", AM_IMP, CPU_6502 | CPU_65C02, 0x08, 1},
    {"PLA", AM_IMP, CPU_6502 | CPU_65C02, 0x68, 1},
    {"PLP", AM_IMP, CPU_6502 | CPU_65C02, 0x28, 1},
    {"PHX", AM_IMP, CPU_65C02, 0xDA, 1},
    {"PLX", AM_IMP, CPU_65C02, 0xFA, 1},
    {"PHY", AM_IMP, CPU_65C02, 0x5A, 1},
    {"PLY", AM_IMP, CPU_65C02, 0x7A, 1},

    // ROL
    {"ROL", AM_ACC, CPU_6502 | CPU_65C02, 0x2A, 1},
    {"ROL", AM_ZP, CPU_6502 | CPU_65C02, 0x26, 2},
    {"ROL", AM_ZPX, CPU_6502 | CPU_65C02, 0x36, 2},
    {"ROL", AM_ABS, CPU_6502 | CPU_65C02, 0x2E, 3},
    {"ROL", AM_ABSX, CPU_6502 | CPU_65C02, 0x3E, 3},

    // ROR
    {"ROR", AM_ACC, CPU_6502 | CPU_65C02, 0x6A, 1},
    {"ROR", AM_ZP, CPU_6502 | CPU_65C02, 0x66, 2},
    {"ROR", AM_ZPX, CPU_6502 | CPU_65C02, 0x76, 2},
    {"ROR", AM_ABS, CPU_6502 | CPU_65C02, 0x6E, 3},
    {"ROR", AM_ABSX, CPU_6502 | CPU_65C02, 0x7E, 3},

    // SBC
    {"SBC", AM_IMM, CPU_6502 | CPU_65C02, 0xE9, 2},
    {"SBC", AM_ZP, CPU_6502 | CPU_65C02, 0xE5, 2},
    {"SBC", AM_ZPX, CPU_6502 | CPU_65C02, 0xF5, 2},
    {"SBC", AM_ABS, CPU_6502 | CPU_65C02, 0xED, 3},
    {"SBC", AM_ABSX, CPU_6502 | CPU_65C02, 0xFD, 3},
    {"SBC", AM_ABSY, CPU_6502 | CPU_65C02, 0xF9, 3},
    {"SBC", AM_INDX, CPU_6502 | CPU_65C02, 0xE1, 2},
    {"SBC", AM_INDY, CPU_6502 | CPU_65C02, 0xF1, 2},
    {"SBC", AM_ZPIND, CPU_65C02, 0xF2, 2},

    // SEC/SED/SEI
    {"SEC", AM_IMP, CPU_6502 | CPU_65C02, 0x38, 1},
    {"SED", AM_IMP, CPU_6502 | CPU_65C02, 0xF8, 1},
    {"SEI", AM_IMP, CPU_6502 | CPU_65C02, 0x78, 1},

    // STA
    {"STA", AM_ZP, CPU_6502 | CPU_65C02, 0x85, 2},
    {"STA", AM_ZPX, CPU_6502 | CPU_65C02, 0x95, 2},
    {"STA", AM_ABS, CPU_6502 | CPU_65C02, 0x8D, 3},
    {"STA", AM_ABSX, CPU_6502 | CPU_65C02, 0x9D, 3},
    {"STA", AM_ABSY, CPU_6502 | CPU_65C02, 0x99, 3},
    {"STA", AM_INDX, CPU_6502 | CPU_65C02, 0x81, 2},
    {"STA", AM_INDY, CPU_6502 | CPU_65C02, 0x91, 2},
    {"STA", AM_ZPIND, CPU_65C02, 0x92, 2},

    // STX
    {"STX", AM_ZP, CPU_6502 | CPU_65C02, 0x86, 2},
    {"STX", AM_ZPY, CPU_6502 | CPU_65C02, 0x96, 2},
    {"STX", AM_ABS, CPU_6502 | CPU_65C02, 0x8E, 3},

    // STY
    {"STY", AM_ZP, CPU_6502 | CPU_65C02, 0x84, 2},
    {"STY", AM_ZPX, CPU_6502 | CPU_65C02, 0x94, 2},
    {"STY", AM_ABS, CPU_6502 | CPU_65C02, 0x8C, 3},

    // Transfers
    {"TAX", AM_IMP, CPU_6502 | CPU_65C02, 0xAA, 1},
    {"TAY", AM_IMP, CPU_6502 | CPU_65C02, 0xA8, 1},
    {"TSX", AM_IMP, CPU_6502 | CPU_65C02, 0xBA, 1},
    {"TXA", AM_IMP, CPU_6502 | CPU_65C02, 0x8A, 1},
    {"TXS", AM_IMP, CPU_6502 | CPU_65C02, 0x9A, 1},
    {"TYA", AM_IMP, CPU_6502 | CPU_65C02, 0x98, 1},

    // STZ
    {"STZ", AM_ZP, CPU_65C02, 0x64, 2},
    {"STZ", AM_ZPX, CPU_65C02, 0x74, 2},
    {"STZ", AM_ABS, CPU_65C02, 0x9C, 3},
    {"STZ", AM_ABSX, CPU_65C02, 0x9E, 3},

    // TRB/TSB
    {"TSB", AM_ZP, CPU_65C02, 0x04, 2},
    {"TSB", AM_ABS, CPU_65C02, 0x0C, 3},
    {"TRB", AM_ZP, CPU_65C02, 0x14, 2},
    {"TRB", AM_ABS, CPU_65C02, 0x1C, 3},

    // WAI/STP
    {"WAI", AM_IMP, CPU_65C02, 0xCB, 1},
    {"STP", AM_IMP, CPU_65C02, 0xDB, 1},

    /* ── 65816 opcodes ───────────────────────────────────────────────────── */
    {"ADC", AM_LONG, CPU_65816, 0x6F, 4},
    {"ADC", AM_LONGX, CPU_65816, 0x7F, 4},
    {"ADC", AM_SR, CPU_65816, 0x63, 2},
    {"ADC", AM_SRINY, CPU_65816, 0x73, 2},
    {"ADC", AM_INDL, CPU_65816, 0x67, 2},
    {"ADC", AM_INDLY, CPU_65816, 0x77, 2},
    {"AND", AM_LONG, CPU_65816, 0x2F, 4},
    {"AND", AM_LONGX, CPU_65816, 0x3F, 4},
    {"AND", AM_SR, CPU_65816, 0x23, 2},
    {"AND", AM_SRINY, CPU_65816, 0x33, 2},
    {"AND", AM_INDL, CPU_65816, 0x27, 2},
    {"AND", AM_INDLY, CPU_65816, 0x37, 2},
    {"CMP", AM_LONG, CPU_65816, 0xCF, 4},
    {"CMP", AM_LONGX, CPU_65816, 0xDF, 4},
    {"CMP", AM_SR, CPU_65816, 0xC3, 2},
    {"CMP", AM_SRINY, CPU_65816, 0xD3, 2},
    {"CMP", AM_INDL, CPU_65816, 0xC7, 2},
    {"CMP", AM_INDLY, CPU_65816, 0xD7, 2},
    {"EOR", AM_LONG, CPU_65816, 0x4F, 4},
    {"EOR", AM_LONGX, CPU_65816, 0x5F, 4},
    {"EOR", AM_SR, CPU_65816, 0x43, 2},
    {"EOR", AM_SRINY, CPU_65816, 0x53, 2},
    {"EOR", AM_INDL, CPU_65816, 0x47, 2},
    {"EOR", AM_INDLY, CPU_65816, 0x57, 2},
    {"LDA", AM_LONG, CPU_65816, 0xAF, 4},
    {"LDA", AM_LONGX, CPU_65816, 0xBF, 4},
    {"LDA", AM_SR, CPU_65816, 0xA3, 2},
    {"LDA", AM_SRINY, CPU_65816, 0xB3, 2},
    {"LDA", AM_INDL, CPU_65816, 0xA7, 2},
    {"LDA", AM_INDLY, CPU_65816, 0xB7, 2},
    {"ORA", AM_LONG, CPU_65816, 0x0F, 4},
    {"ORA", AM_LONGX, CPU_65816, 0x1F, 4},
    {"ORA", AM_SR, CPU_65816, 0x03, 2},
    {"ORA", AM_SRINY, CPU_65816, 0x13, 2},
    {"ORA", AM_INDL, CPU_65816, 0x07, 2},
    {"ORA", AM_INDLY, CPU_65816, 0x17, 2},
    {"SBC", AM_LONG, CPU_65816, 0xEF, 4},
    {"SBC", AM_LONGX, CPU_65816, 0xFF, 4},
    {"SBC", AM_SR, CPU_65816, 0xE3, 2},
    {"SBC", AM_SRINY, CPU_65816, 0xF3, 2},
    {"SBC", AM_INDL, CPU_65816, 0xE7, 2},
    {"SBC", AM_INDLY, CPU_65816, 0xF7, 2},
    {"STA", AM_LONG, CPU_65816, 0x8F, 4},
    {"STA", AM_LONGX, CPU_65816, 0x9F, 4},
    {"STA", AM_SR, CPU_65816, 0x83, 2},
    {"STA", AM_SRINY, CPU_65816, 0x93, 2},
    {"STA", AM_INDL, CPU_65816, 0x87, 2},
    {"STA", AM_INDLY, CPU_65816, 0x97, 2},
    {"JML", AM_LONG, CPU_65816, 0x5C, 4},
    {"JML", AM_ABSL, CPU_65816, 0xDC, 3},
    {"JSL", AM_LONG, CPU_65816, 0x22, 4},
    {"RTL", AM_IMP, CPU_65816, 0x6B, 1},
    {"BRL", AM_RELL, CPU_65816, 0x82, 3},
    {"MVN", AM_BLK, CPU_65816, 0x54, 3},
    {"MVP", AM_BLK, CPU_65816, 0x44, 3},
    {"PEA", AM_ABS, CPU_65816, 0xF4, 3},
    {"PEI", AM_ZPIND, CPU_65816, 0xD4, 2},
    {"PER", AM_RELL, CPU_65816, 0x62, 3},
    {"PHB", AM_IMP, CPU_65816, 0x8B, 1},
    {"PLB", AM_IMP, CPU_65816, 0xAB, 1},
    {"PHD", AM_IMP, CPU_65816, 0x0B, 1},
    {"PLD", AM_IMP, CPU_65816, 0x2B, 1},
    {"PHK", AM_IMP, CPU_65816, 0x4B, 1},
    {"REP", AM_IMM, CPU_65816, 0xC2, 2},
    {"SEP", AM_IMM, CPU_65816, 0xE2, 2},
    {"TCD", AM_IMP, CPU_65816, 0x5B, 1},
    {"TDC", AM_IMP, CPU_65816, 0x7B, 1},
    {"TCS", AM_IMP, CPU_65816, 0x1B, 1},
    {"TSC", AM_IMP, CPU_65816, 0x3B, 1},
    {"TXY", AM_IMP, CPU_65816, 0x9B, 1},
    {"TYX", AM_IMP, CPU_65816, 0xBB, 1},
    {"XBA", AM_IMP, CPU_65816, 0xEB, 1},
    {"XCE", AM_IMP, CPU_65816, 0xFB, 1},
    {"WDM", AM_IMM, CPU_65816, 0x42, 2},
};

/* ------------------------- Disassembly helper (GUI) -------------------------
 */

static const op_t *g_op_by_opcode_6502[256];
static bool g_op_by_opcode_6502_inited = false;

static const op_t *g_op_by_opcode_65c02[256];
static bool g_op_by_opcode_65c02_inited = false;

/* Populate the 6502 disassembly lookup table (opcode -> mnemonic + mode).
 * Called once on first use, protected by a static initialised flag. */
static void disasm_init_6502(void) {
  if (g_op_by_opcode_6502_inited)
    return;
  g_op_by_opcode_6502_inited = true;
  for (int i = 0; i < 256; i++)
    g_op_by_opcode_6502[i] = NULL;
  for (const op_t *op = g_ops; op && op->mn; op++) {
    /* Only include instructions available on a plain 6502. */
    if ((op->cpu_mask & CPU_6502) == 0)
      continue;
    g_op_by_opcode_6502[op->opcode] = op;
  }
}

/* Populate the 65C02 disassembly lookup table, extending the 6502 table
 * with additional opcodes and addressing modes. */
static void disasm_init_65c02(void) {
  if (g_op_by_opcode_65c02_inited)
    return;
  g_op_by_opcode_65c02_inited = true;
  for (int i = 0; i < 256; i++)
    g_op_by_opcode_65c02[i] = NULL;
  for (int i = 0; i < (int)(sizeof(g_ops) / sizeof(g_ops[0])); i++) {
    const op_t *op = &g_ops[i];
    if (!(op->cpu_mask & CPU_65C02))
      continue;
    g_op_by_opcode_65c02[op->opcode] = op;
  }
}

/* Format the operand of a disassembled instruction into out.
 * in:  mem — 64KB memory image, pc — address of the instruction,
 *      op  — opcode descriptor (mnemonic, addressing mode, size)
 * ret: instruction length in bytes */
static int disasm_fmt_operand(const uint8_t *mem, uint16_t pc, const op_t *op,
                              char *out, size_t out_sz) {
  if (!out || out_sz == 0)
    return 0;
  out[0] = 0;
  const uint8_t b1 = mem[(uint16_t)(pc + 1)];
  const uint8_t b2 = mem[(uint16_t)(pc + 2)];
  const uint16_t w = (uint16_t)(b1 | ((uint16_t)b2 << 8));

  switch (op->am) {
  case AM_IMP:
    return 0;
  case AM_ACC:
    return snprintf(out, out_sz, "A");
  case AM_IMM:
    return snprintf(out, out_sz, "#&%02X", b1);
  case AM_ZP:
    return snprintf(out, out_sz, "&%02X", b1);
  case AM_ZPX:
    return snprintf(out, out_sz, "&%02X,X", b1);
  case AM_ZPY:
    return snprintf(out, out_sz, "&%02X,Y", b1);
  case AM_ABS:
    return snprintf(out, out_sz, "&%04X", w);
  case AM_ABSX:
    return snprintf(out, out_sz, "&%04X,X", w);
  case AM_ABSY:
    return snprintf(out, out_sz, "&%04X,Y", w);
  case AM_IND:
    return snprintf(out, out_sz, "(&%04X)", w);
  case AM_INDX:
    return snprintf(out, out_sz, "(&%02X,X)", b1);
  case AM_INDY:
    return snprintf(out, out_sz, "(&%02X),Y", b1);
  case AM_ZPIND:
    return snprintf(out, out_sz, "(&%02X)", b1);
  case AM_ABSINDX:
    return snprintf(out, out_sz, "(&%04X,X)", w);
  case AM_REL: {
    const int8_t off = (int8_t)b1;
    const uint16_t tgt = (uint16_t)(pc + 2 + (int)off);
    return snprintf(out, out_sz, "&%04X", tgt);
  }
  case AM_ZPREL: {
    const int8_t off = (int8_t)b2;
    const uint16_t tgt = (uint16_t)(pc + 3 + (int)off);
    return snprintf(out, out_sz, "&%02X,&%04X", b1, tgt);
  }
  }

  return 0;
}

/* Disassemble a single 6502 instruction at mem[pc].
 * Formats the result as "MNEMONIC operand" into out[0..out_sz).
 * Unknown opcodes are formatted as ".byte &XX".
 * in:  mem — 64KB memory image
 *      pc  — program counter (address of the instruction)
 * out: out[0..out_sz) — formatted string
 * ret: instruction length in bytes (1..3) */
int asm_disasm_6502(const uint8_t *mem, uint16_t pc, char *out, size_t out_sz) {
  if (!mem || !out || out_sz == 0)
    return 1;
  disasm_init_6502();
  const uint8_t opcode = mem[pc];
  const op_t *op = g_op_by_opcode_6502[opcode];
  if (!op || !op->mn) {
    snprintf(out, out_sz, ".byte &%02X", opcode);
    return 1;
  }

  char oper[64];
  oper[0] = 0;
  disasm_fmt_operand(mem, pc, op, oper, sizeof(oper));
  if (oper[0]) {
    snprintf(out, out_sz, "%s %s", op->mn, oper);
  } else {
    snprintf(out, out_sz, "%s", op->mn);
  }

  int len = (int)op->bytes;
  if (len < 1)
    len = 1;
  return len;
}

/* Disassemble a single 65C02 instruction.  Same contract as asm_disasm_6502.
 * Includes all 65C02 additions (STZ, TRB, TSB, BRA, etc.). */
int asm_disasm_65c02(const uint8_t *mem, uint16_t pc, char *out,
                     size_t out_sz) {
  if (!mem || !out || out_sz == 0)
    return 1;
  disasm_init_65c02();
  const uint8_t opcode = mem[pc];
  const op_t *op = g_op_by_opcode_65c02[opcode];
  if (!op || !op->mn) {
    snprintf(out, out_sz, ".byte &%02X", opcode);
    return 1;
  }

  char oper[64];
  oper[0] = 0;
  disasm_fmt_operand(mem, pc, op, oper, sizeof(oper));
  if (oper[0]) {
    snprintf(out, out_sz, "%s %s", op->mn, oper);
  } else {
    snprintf(out, out_sz, "%s", op->mn);
  }

  int len = (int)op->bytes;
  if (len < 1)
    len = 1;
  return len;
}

/* ── 65816 disassembler ─────────────────────────────────────────────────────
 */

static const op_t *g_op_by_opcode_65816[256];
static bool g_op_by_opcode_65816_inited = false;

static void disasm_init_65816(void) {
  if (g_op_by_opcode_65816_inited)
    return;
  g_op_by_opcode_65816_inited = true;
  disasm_init_65c02();
  for (int i = 0; i < 256; i++)
    g_op_by_opcode_65816[i] = g_op_by_opcode_65c02[i];
  for (size_t i = 0; i < sizeof(g_ops) / sizeof(g_ops[0]); i++) {
    const op_t *op = &g_ops[i];
    if (op->cpu_mask & CPU_65816)
      g_op_by_opcode_65816[op->opcode] = op;
  }
}

/* Disassemble a single 65816 instruction.  Same contract as asm_disasm_6502.
 * Includes 24-bit addressing, stack-relative modes, block-move, etc.
 * Note: register width (M/X flags) is not tracked; immediates may be
 * shown at 8-bit width even in 16-bit mode. */
int asm_disasm_65816(const uint8_t *mem, uint16_t pc, char *out,
                     size_t out_sz) {
  if (!mem || !out || out_sz == 0)
    return 1;
  disasm_init_65816();
  const uint8_t opcode = mem[pc];
  const op_t *op = g_op_by_opcode_65816[opcode];
  if (!op || !op->mn) {
    snprintf(out, out_sz, ".byte &%02X", opcode);
    return 1;
  }
  char oper[80];
  oper[0] = 0;
  int len = (int)op->bytes;
  switch (op->am) {
  case AM_LONG:
    if (len >= 4)
      snprintf(
          oper, sizeof(oper), "$%02X%04X", mem[(pc + 3) & 0xFFFF],
          (unsigned)(mem[(pc + 1) & 0xFFFF] | (mem[(pc + 2) & 0xFFFF] << 8)));
    break;
  case AM_LONGX:
    if (len >= 4)
      snprintf(
          oper, sizeof(oper), "$%02X%04X,X", mem[(pc + 3) & 0xFFFF],
          (unsigned)(mem[(pc + 1) & 0xFFFF] | (mem[(pc + 2) & 0xFFFF] << 8)));
    break;
  case AM_SR:
    if (len >= 2)
      snprintf(oper, sizeof(oper), "$%02X,S", mem[(pc + 1) & 0xFFFF]);
    break;
  case AM_SRINY:
    if (len >= 2)
      snprintf(oper, sizeof(oper), "($%02X,S),Y", mem[(pc + 1) & 0xFFFF]);
    break;
  case AM_INDL:
    if (len >= 2)
      snprintf(oper, sizeof(oper), "[$%02X]", mem[(pc + 1) & 0xFFFF]);
    break;
  case AM_INDLY:
    if (len >= 2)
      snprintf(oper, sizeof(oper), "[$%02X],Y", mem[(pc + 1) & 0xFFFF]);
    break;
  case AM_ABSL:
    if (len >= 3)
      snprintf(
          oper, sizeof(oper), "[$%04X]",
          (unsigned)(mem[(pc + 1) & 0xFFFF] | (mem[(pc + 2) & 0xFFFF] << 8)));
    break;
  case AM_RELL:
    if (len >= 3) {
      int16_t rel =
          (int16_t)(mem[(pc + 1) & 0xFFFF] | (mem[(pc + 2) & 0xFFFF] << 8));
      uint16_t target = (uint16_t)((int)pc + 3 + rel);
      snprintf(oper, sizeof(oper), "$%04X", (unsigned)target);
    }
    break;
  case AM_BLK:
    if (len >= 3)
      snprintf(oper, sizeof(oper), "$%02X,$%02X", mem[(pc + 2) & 0xFFFF],
               mem[(pc + 1) & 0xFFFF]);
    break;
  default:
    disasm_fmt_operand(mem, pc, op, oper, sizeof(oper));
    break;
  }
  if (oper[0])
    snprintf(out, out_sz, "%s %s", op->mn, oper);
  else
    snprintf(out, out_sz, "%s", op->mn);
  if (len < 1)
    len = 1;
  return len;
}

// Undocumented/illegal NMOS 6502 opcodes (common set).
// Enabled only when settings->allow_undoc is true and CPU is 6502.
static const op_t g_ops_undoc[] = {
    // LAX (load A and X)
    {"LAX", AM_IMM, CPU_6502, 0xAB, 2},
    {"LAX", AM_ZP, CPU_6502, 0xA7, 2},
    {"LAX", AM_ZPY, CPU_6502, 0xB7, 2},
    {"LAX", AM_ABS, CPU_6502, 0xAF, 3},
    {"LAX", AM_ABSY, CPU_6502, 0xBF, 3},
    {"LAX", AM_INDX, CPU_6502, 0xA3, 2},
    {"LAX", AM_INDY, CPU_6502, 0xB3, 2},

    // SAX (store A & X)
    {"SAX", AM_ZP, CPU_6502, 0x87, 2},
    {"SAX", AM_ZPY, CPU_6502, 0x97, 2},
    {"SAX", AM_ABS, CPU_6502, 0x8F, 3},
    {"SAX", AM_INDX, CPU_6502, 0x83, 2},

    // SLO (ASL then ORA)
    {"SLO", AM_ZP, CPU_6502, 0x07, 2},
    {"SLO", AM_ZPX, CPU_6502, 0x17, 2},
    {"SLO", AM_ABS, CPU_6502, 0x0F, 3},
    {"SLO", AM_ABSX, CPU_6502, 0x1F, 3},
    {"SLO", AM_ABSY, CPU_6502, 0x1B, 3},
    {"SLO", AM_INDX, CPU_6502, 0x03, 2},
    {"SLO", AM_INDY, CPU_6502, 0x13, 2},

    // RLA (ROL then AND)
    {"RLA", AM_ZP, CPU_6502, 0x27, 2},
    {"RLA", AM_ZPX, CPU_6502, 0x37, 2},
    {"RLA", AM_ABS, CPU_6502, 0x2F, 3},
    {"RLA", AM_ABSX, CPU_6502, 0x3F, 3},
    {"RLA", AM_ABSY, CPU_6502, 0x3B, 3},
    {"RLA", AM_INDX, CPU_6502, 0x23, 2},
    {"RLA", AM_INDY, CPU_6502, 0x33, 2},

    // SRE (LSR then EOR)
    {"SRE", AM_ZP, CPU_6502, 0x47, 2},
    {"SRE", AM_ZPX, CPU_6502, 0x57, 2},
    {"SRE", AM_ABS, CPU_6502, 0x4F, 3},
    {"SRE", AM_ABSX, CPU_6502, 0x5F, 3},
    {"SRE", AM_ABSY, CPU_6502, 0x5B, 3},
    {"SRE", AM_INDX, CPU_6502, 0x43, 2},
    {"SRE", AM_INDY, CPU_6502, 0x53, 2},

    // RRA (ROR then ADC)
    {"RRA", AM_ZP, CPU_6502, 0x67, 2},
    {"RRA", AM_ZPX, CPU_6502, 0x77, 2},
    {"RRA", AM_ABS, CPU_6502, 0x6F, 3},
    {"RRA", AM_ABSX, CPU_6502, 0x7F, 3},
    {"RRA", AM_ABSY, CPU_6502, 0x7B, 3},
    {"RRA", AM_INDX, CPU_6502, 0x63, 2},
    {"RRA", AM_INDY, CPU_6502, 0x73, 2},

    // DCP (DEC then CMP)
    {"DCP", AM_ZP, CPU_6502, 0xC7, 2},
    {"DCP", AM_ZPX, CPU_6502, 0xD7, 2},
    {"DCP", AM_ABS, CPU_6502, 0xCF, 3},
    {"DCP", AM_ABSX, CPU_6502, 0xDF, 3},
    {"DCP", AM_ABSY, CPU_6502, 0xDB, 3},
    {"DCP", AM_INDX, CPU_6502, 0xC3, 2},
    {"DCP", AM_INDY, CPU_6502, 0xD3, 2},

    // ISC/ISB (INC then SBC) - we use ISC mnemonic internally
    {"ISC", AM_ZP, CPU_6502, 0xE7, 2},
    {"ISC", AM_ZPX, CPU_6502, 0xF7, 2},
    {"ISC", AM_ABS, CPU_6502, 0xEF, 3},
    {"ISC", AM_ABSX, CPU_6502, 0xFF, 3},
    {"ISC", AM_ABSY, CPU_6502, 0xFB, 3},
    {"ISC", AM_INDX, CPU_6502, 0xE3, 2},
    {"ISC", AM_INDY, CPU_6502, 0xF3, 2},
};

/* Return true if tok matches a known mnemonic in the opcode table.
 * Performs a linear scan over g_ops (and g_ops_undoc if allow_undoc).
 * in:  tok        — token to test (case-insensitive)
 *      allow_undoc — include undocumented/illegal opcodes in search */
static bool is_known_instruction_tok(sv_t tok, bool allow_undoc) {
  // tok is case-insensitive mnemonic
  // Cheap linear scan; table is static and modest.
  for (size_t i = 0; i < (sizeof(g_ops) / sizeof(g_ops[0])); i++) {
    if (sv_ieq(tok, g_ops[i].mn))
      return true;
  }
  if (allow_undoc) {
    for (size_t i = 0; i < (sizeof(g_ops_undoc) / sizeof(g_ops_undoc[0]));
         i++) {
      if (sv_ieq(tok, g_ops_undoc[i].mn))
        return true;
    }
  }
  return false;
}

/* Copy up to 7 bytes of v into out[], converting to uppercase and
 * NUL-terminating. Used to normalise mnemonic tokens for opcode table lookup.
 */
static void sv_to_upper3(sv_t v, char out[8]) {
  size_t n = v.len;
  if (n > 7)
    n = 7;
  for (size_t i = 0; i < n; i++) {
    char c = v.p[i];
    if (c >= 'a' && c <= 'z')
      c = (char)(c - 'a' + 'A');
    out[i] = c;
  }
  out[n] = 0;
}

static const op_t *op_find(sv_t mnem, addrmode_t am, asm_cpu_t cpu,
                           bool allow_undoc) {
  char up[8];
  sv_to_upper3(mnem, up);
  uint8_t mask = (cpu == ASM_CPU_65816)   ? (CPU_65816 | CPU_65C02 | CPU_6502)
                 : (cpu == ASM_CPU_65C02) ? (CPU_65C02 | CPU_6502)
                                          : CPU_6502;
  for (size_t i = 0; i < sizeof(g_ops) / sizeof(g_ops[0]); i++) {
    const op_t *op = &g_ops[i];
    if (op->am != am)
      continue;
    if (!(op->cpu_mask & mask))
      continue;
    if (strcmp(op->mn, up) == 0)
      return op;
  }
  if (allow_undoc && cpu == ASM_CPU_6502) {
    for (size_t i = 0; i < (sizeof(g_ops_undoc) / sizeof(g_ops_undoc[0]));
         i++) {
      const op_t *op = &g_ops_undoc[i];
      if (op->am != am)
        continue;
      if (!(op->cpu_mask & mask))
        continue;
      if (strcmp(op->mn, up) == 0)
        return op;
    }
  }
  return NULL;
}

// Return which CPUs support this mnemonic in the documented table, and whether
// it exists in the undocumented table (6502-only) regardless of --undoc.
static void mnem_collect_presence(sv_t mnem, uint8_t *out_doc_cpu_mask,
                                  bool *out_has_undoc) {
  if (out_doc_cpu_mask)
    *out_doc_cpu_mask = 0;
  if (out_has_undoc)
    *out_has_undoc = false;

  char up[8];
  sv_to_upper3(mnem, up);

  // documented ops
  for (size_t i = 0; i < (sizeof(g_ops) / sizeof(g_ops[0])); i++) {
    if (strcmp(g_ops[i].mn, up) == 0) {
      if (out_doc_cpu_mask)
        *out_doc_cpu_mask |= g_ops[i].cpu_mask;
    }
  }

  // undocumented ops (6502 only)
  for (size_t i = 0; i < (sizeof(g_ops_undoc) / sizeof(g_ops_undoc[0])); i++) {
    if (strcmp(g_ops_undoc[i].mn, up) == 0) {
      if (out_has_undoc)
        *out_has_undoc = true;
    }
  }
}

/* Return true if mnem is a standard relative-branch instruction
 * (BCC, BCS, BEQ, BMI, BNE, BPL, BVC, BVS, BRA). */
static bool is_branch_mnemonic(sv_t mnem) {
  return sv_ieq(mnem, "BCC") || sv_ieq(mnem, "BCS") || sv_ieq(mnem, "BEQ") ||
         sv_ieq(mnem, "BMI") || sv_ieq(mnem, "BNE") || sv_ieq(mnem, "BPL") ||
         sv_ieq(mnem, "BVC") || sv_ieq(mnem, "BVS") || sv_ieq(mnem, "BRA");
}

/* Return true if mnem is a 65816 long-relative-branch instruction (BRL, PER).
 */
static bool is_long_branch_mnemonic(sv_t mnem) {
  return sv_ieq(mnem, "BRL") || sv_ieq(mnem, "PER");
}

/* Return true if v is exactly the single letter 'A' or 'a'.
 * Used to detect the accumulator addressing mode (e.g. LSR A). */
static bool sv_is_single_A(sv_t v) {
  v = sv_trim(v);
  return (v.len == 1) && (v.p[0] == 'A' || v.p[0] == 'a');
}

// Parse operand pattern into a base addressing mode. Some modes require value
// to decide zp vs abs.
typedef struct {
  addrmode_t am;
  sv_t expr; // expression part (without #, parens, index)
  bool has_index;
  char index_reg; // 'X' or 'Y'
  bool is_indirect;
  bool is_ind_x;
  bool is_ind_y;
  bool is_abs_ind_x; // (abs,X)
  bool is_zp_rel;    // zp,rel for BBR/BBS (not implemented in op table yet)
  sv_t expr2;        // second expression for zp,rel
  bool is_long_addr; // $AABBCC 24-bit address
  bool is_ind_long;  // [expr] indirect long
  bool is_sr;        // expr,S stack-relative
  bool is_sr_ind_y;  // (expr,S),Y
  sv_t expr_blk;     // second operand for MVN/MVP
} operand_pat_t;

static bool parse_operand_pattern(const char *file, unsigned line_no,
                                  unsigned op_col, sv_t operand, sv_t mnem,
                                  operand_pat_t *out, diags_t *diags) {
  memset(out, 0, sizeof(*out));
  operand = sv_trim(operand);
  if (operand.len == 0) {
    // ASL, LSR, ROL, ROR (and 65C02 DEC/INC) accept bare form as shorthand for
    // accumulator mode. Check mnemonic name so e.g. bare "LSR" is treated as
    // "LSR A".
    char mn_up[8] = {0};
    sv_to_upper3(mnem, mn_up);
    if (strcmp(mn_up, "ASL") == 0 || strcmp(mn_up, "LSR") == 0 ||
        strcmp(mn_up, "ROL") == 0 || strcmp(mn_up, "ROR") == 0 ||
        strcmp(mn_up, "DEC") == 0 || strcmp(mn_up, "INC") == 0) {
      out->am = AM_ACC;
    } else {
      out->am = AM_IMP;
    }
    return true;
  }
  if (sv_is_single_A(operand)) {
    out->am = AM_ACC;
    return true;
  }

  const char *p = operand.p;
  const char *e = operand.p + operand.len;
  while (p < e && is_ws(*p)) {
    p++;
    op_col++;
  }

  // 65816: [expr] indirect long, or [expr],Y indirect long indexed
  if (p < e && *p == '[') {
    const char *scan = p + 1;
    int depth = 1;
    while (scan < e) {
      if (*scan == '[')
        depth++;
      else if (*scan == ']') {
        depth--;
        if (depth == 0)
          break;
      }
      scan++;
    }
    if (scan >= e || *scan != ']') {
      diag_addf(diags, DIAG_ERROR, file, line_no, op_col, (unsigned)operand.len,
                "missing ']' in indirect long operand");
      return false;
    }
    sv_t inside = {p + 1, (size_t)(scan - (p + 1))};
    inside = sv_trim(inside);
    const char *after = scan + 1;
    while (after < e && is_ws(*after))
      after++;
    if (after < e && *after == ',') {
      after++;
      while (after < e && is_ws(*after))
        after++;
      if (after >= e) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "missing index register after ',' in indirect long operand");
        return false;
      }
      char reg = *after;
      after++;
      while (after < e && is_ws(*after))
        after++;
      if (after != e) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "unexpected text after indirect long operand");
        return false;
      }
      if (reg == 'y' || reg == 'Y') {
        out->is_ind_long = true;
        out->expr = inside;
        out->index_reg = 'Y';
        out->am = AM_INDLY;
        return true;
      }
      diag_addf(diags, DIAG_ERROR, file, line_no, op_col, (unsigned)operand.len,
                "only [expr],Y is valid for indirect long indexed");
      return false;
    }
    if (after == e) {
      out->is_ind_long = true;
      out->expr = inside;
      out->am = AM_INDL;
      return true;
    }
    diag_addf(diags, DIAG_ERROR, file, line_no, op_col, (unsigned)operand.len,
              "unexpected text after indirect long operand");
    return false;
  }

  // Immediate: starts with '#'
  if (p < e && *p == '#') {
    p++;
    out->am = AM_IMM;
    out->expr.p = p;
    out->expr.len = (size_t)(e - p);
    out->expr = sv_trim(out->expr);
    return true;
  }

  // Long branch mnemonics (65816): BRL/PER use AM_RELL
  if (is_long_branch_mnemonic(mnem)) {
    out->am = AM_RELL;
    out->expr = operand;
    return true;
  }
  // 65816: JSL always uses AM_LONG (24-bit absolute)
  if (sv_ieq(mnem, "JSL")) {
    out->am = AM_LONG;
    out->expr = operand;
    return true;
  }
  // Branch mnemonics: treat as REL
  if (is_branch_mnemonic(mnem)) {
    out->am = AM_REL;
    out->expr = operand;
    return true;
  }

  // Indirect patterns
  if (p < e && *p == '(') {
    const char *scan = p + 1;
    bool in_str2 = false;
    int depth = 1;
    while (scan < e) {
      char c = *scan;
      if (c == '"') {
        in_str2 = !in_str2;
      } else if (!in_str2) {
        if (c == '(') {
          depth++;
        } else if (c == ')') {
          depth--;
          if (depth == 0)
            break;
        }
      }
      scan++;
    }

    if (scan >= e || *scan != ')') {
      diag_addf(diags, DIAG_ERROR, file, line_no, op_col, (unsigned)operand.len,
                "missing ')' in indirect operand");
      return false;
    }

    sv_t inside = {p + 1, (size_t)(scan - (p + 1))};
    inside = sv_trim(inside);

    // Look for comma inside the outermost parentheses: (expr,X)
    const char *inside_p = inside.p;
    const char *inside_e = inside.p + inside.len;
    const char *inside_comma = NULL;
    bool inside_in_str = false;
    int inside_paren = 0;
    for (const char *t = inside_p; t < inside_e; ++t) {
      char c = *t;
      if (c == '"') {
        inside_in_str = !inside_in_str;
      } else if (!inside_in_str) {
        if (c == '(') {
          inside_paren++;
        } else if (c == ')' && inside_paren > 0) {
          inside_paren--;
        } else if (c == ',' && inside_paren == 0) {
          inside_comma = t;
          break;
        }
      }
    }

    if (inside_comma) {
      sv_t left = {inside_p, (size_t)(inside_comma - inside_p)};
      left = sv_trim(left);
      const char *idx = inside_comma + 1;
      while (idx < inside_e && is_ws(*idx))
        idx++;
      if (idx >= inside_e) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "missing index register after ',' in indirect operand");
        return false;
      }
      char reg = *idx;
      idx++;
      while (idx < inside_e && is_ws(*idx))
        idx++;
      if (idx != inside_e) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "unexpected trailing text inside indirect operand");
        return false;
      }
      if (reg == 'x' || reg == 'X') {
        out->is_ind_x = true;
        out->expr = left;
        out->index_reg = 'X';
        out->am = AM_INDX;
        const char *after = scan + 1;
        while (after < e && is_ws(*after))
          after++;
        if (after != e) {
          diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                    (unsigned)operand.len,
                    "unexpected trailing text after indirect operand");
          return false;
        }
        return true;
      } else if (reg == 's' || reg == 'S') {
        // 65816: (expr,S),Y
        const char *after = scan + 1;
        while (after < e && is_ws(*after))
          after++;
        if (after < e && *after == ',') {
          after++;
          while (after < e && is_ws(*after))
            after++;
          if (after < e && (*after == 'y' || *after == 'Y')) {
            after++;
            while (after < e && is_ws(*after))
              after++;
            if (after == e) {
              out->is_sr_ind_y = true;
              out->expr = left;
              out->am = AM_SRINY;
              return true;
            }
          }
        }
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "65816 stack-relative indirect must be '(expr,S),Y'");
        return false;
      } else if (reg == 'y' || reg == 'Y') {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "'(expr,Y)' is not a valid addressing mode");
        return false;
      } else {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "unknown index register '%c' (expected X or S)", reg);
        return false;
      }
    }

    // Look for suffix after ')': (expr),Y
    const char *after = scan + 1;
    while (after < e && is_ws(*after))
      after++;
    if (after < e) {
      if (*after != ',') {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "unexpected trailing text after indirect operand");
        return false;
      }
      after++;
      while (after < e && is_ws(*after))
        after++;
      if (after >= e) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len, "missing index register after ','");
        return false;
      }
      char reg = *after;
      after++;
      while (after < e && is_ws(*after))
        after++;
      if (after != e) {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "unexpected trailing text after indirect operand");
        return false;
      }
      if (reg == 'y' || reg == 'Y') {
        out->is_ind_y = true;
        out->expr = inside;
        out->index_reg = 'Y';
        out->am = AM_INDY;
        return true;
      }
      if (reg == 'x' || reg == 'X') {
        diag_addf(diags, DIAG_ERROR, file, line_no, op_col,
                  (unsigned)operand.len,
                  "'(expr),X' is not a valid 6502 addressing mode");
        return false;
      }
      diag_addf(diags, DIAG_ERROR, file, line_no, op_col, (unsigned)operand.len,
                "unknown index register '%c' (expected Y)", reg);
      return false;
    }

    // Plain (expr)
    out->is_indirect = true;
    out->expr = inside;
    out->am = AM_IND;
    return true;
  }

  // Non-indirect: could be expr[,X|,Y]
  const char *scan2 = p;
  bool in_str3 = false;
  int paren = 0;
  const char *comma = NULL;
  while (scan2 < e) {
    char c = *scan2;
    if (c == '"') {
      in_str3 = !in_str3;
    } else if (!in_str3) {
      if (c == '(') {
        paren++;
      } else if (c == ')' && paren > 0) {
        paren--;
      } else if (c == ',' && paren == 0) {
        comma = scan2;
        break;
      }
    }
    scan2++;
  }

  if (!comma) {
    // 65816: detect 24-bit literal address ($XXYYYY with > 4 hex digits)
    out->am = AM_ABS;
    {
      const char *tp = p;
      if (tp < e && (*tp == '$' || *tp == '&')) {
        tp++;
        size_t hlen = 0;
        while (tp + hlen < e && ((tp[hlen] >= '0' && tp[hlen] <= '9') ||
                                 (tp[hlen] >= 'a' && tp[hlen] <= 'f') ||
                                 (tp[hlen] >= 'A' && tp[hlen] <= 'F')))
          hlen++;
        if (hlen > 4)
          out->am = AM_LONG;
      }
    }
    out->expr = operand;
    return true;
  }

  sv_t left = {p, (size_t)(comma - p)};
  left = sv_trim(left);

  // 65816: MVN/MVP srcbank,dstbank
  {
    char mn_blk[8] = {0};
    sv_to_upper3(mnem, mn_blk);
    if (strcmp(mn_blk, "MVN") == 0 || strcmp(mn_blk, "MVP") == 0) {
      sv_t right = {comma + 1, (size_t)(e - (comma + 1))};
      right = sv_trim(right);
      out->am = AM_BLK;
      out->expr = left;
      out->expr_blk = right;
      return true;
    }
  }

  const char *idx = comma + 1;
  while (idx < e && is_ws(*idx))
    idx++;
  if (idx >= e) {
    diag_addf(diags, DIAG_ERROR, file, line_no, op_col + (unsigned)(comma - p),
              1, "missing index register after ','");
    return false;
  }
  char reg = *idx;
  idx++;
  while (idx < e && is_ws(*idx))
    idx++;
  if (idx != e) {
    diag_addf(diags, DIAG_ERROR, file, line_no, op_col + (unsigned)(idx - p),
              (unsigned)(e - idx),
              "unexpected trailing text after index register");
    return false;
  }
  if (reg == 'x' || reg == 'X') {
    // 65816: detect 24-bit indexed ($XXYYYY,X)
    out->am = AM_ABSX;
    {
      const char *tp = left.p;
      const char *te = left.p + left.len;
      if (tp < te && (*tp == '$' || *tp == '&')) {
        tp++;
        size_t hlen = 0;
        while (tp + hlen < te && ((tp[hlen] >= '0' && tp[hlen] <= '9') ||
                                  (tp[hlen] >= 'a' && tp[hlen] <= 'f') ||
                                  (tp[hlen] >= 'A' && tp[hlen] <= 'F')))
          hlen++;
        if (hlen > 4)
          out->am = AM_LONGX;
      }
    }
    out->expr = left;
    out->index_reg = 'X';
    return true;
  }
  if (reg == 'y' || reg == 'Y') {
    out->am = AM_ABSY;
    out->expr = left;
    out->index_reg = 'Y';
    return true;
  }
  // 65816: expr,S stack-relative
  if (reg == 's' || reg == 'S') {
    out->is_sr = true;
    out->expr = left;
    out->am = AM_SR;
    return true;
  }
  diag_addf(diags, DIAG_ERROR, file, line_no,
            op_col + (unsigned)((idx - 1) - p), 1,
            "unknown index register '%c' (expected X, Y or S)", reg);
  return false;
}

/* Print a 4-digit hex value to f with an optional prefix ('$', '&', or none).
 */
static void list_fprint_hex_u16(FILE *f, char prefix, uint16_t v) {
  if (prefix)
    fputc(prefix, f);
  fprintf(f, "%04X", (unsigned)v);
}

/* Print a 2-digit hex value to f with an optional prefix. */
static void list_fprint_hex_u8(FILE *f, char prefix, uint8_t v) {
  if (prefix)
    fputc(prefix, f);
  fprintf(f, "%02X", (unsigned)v);
}

/* Print a token to f, uppercasing each character. */
static void list_fprint_upper_token(FILE *f, sv_t s) {
  if (!f || !s.p || s.len == 0)
    return;

  // Find first token (after leading whitespace)
  size_t i = 0;
  while (i < s.len && (s.p[i] == ' ' || s.p[i] == '\t')) {
    fputc(s.p[i], f);
    i++;
  }
  const size_t tok_start = i;

  // Token chars: '.' and alnum and '_' (match take_ident-ish)
  while (i < s.len) {
    char c = s.p[i];
    if (c == '.' || c == '_' || (c >= '0' && c <= '9') ||
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      i++;
      continue;
    }
    break;
  }
  const size_t tok_end = i;

  // Uppercase token
  for (size_t k = tok_start; k < tok_end; k++) {
    unsigned char c = (unsigned char)s.p[k];
    if (c >= 'a' && c <= 'z')
      c = (unsigned char)(c - 'a' + 'A');
    fputc((char)c, f);
  }

  // Ensure one space between token and rest if there is more text and next char
  // isn't whitespace
  if (tok_end < s.len) {
    char next = s.p[tok_end];
    if (next != ' ' && next != '\t') {
      fputc(' ', f);
    }
    fwrite(s.p + tok_end, 1, s.len - tok_end, f);
  }
}

/* Print the source column of a listing line.
 * Normalises spacing between mnemonic and operand; optionally uppercases. */
static void list_fprint_pretty_source(FILE *f, sv_t s, bool preserve_case) {
  if (!f || !s.p || s.len == 0)
    return;

  if (preserve_case) {
    fwrite(s.p, 1, s.len, f);
    return;
  }

  // Find first token (after leading whitespace) and print leading whitespace
  // as-is
  size_t i = 0;
  while (i < s.len && (s.p[i] == ' ' || s.p[i] == '\t')) {
    fputc(s.p[i], f);
    i++;
  }
  const size_t tok_start = i;

  // Token chars: '.' and alnum and '_' (match take_ident-ish)
  while (i < s.len) {
    char c = s.p[i];
    if (c == '.' || c == '_' || (c >= '0' && c <= '9') ||
        (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
      i++;
      continue;
    }
    break;
  }
  const size_t tok_end = i;

  // Uppercase token (mnemonic/directive only)
  for (size_t k = tok_start; k < tok_end; k++) {
    unsigned char c = (unsigned char)s.p[k];
    if (c >= 'a' && c <= 'z')
      c = (unsigned char)(c - 'a' + 'A');
    fputc((char)c, f);
  }

  size_t after = tok_end;

  // Ensure one space between token and rest if there is more text and next char
  // isn't whitespace
  if (after < s.len) {
    char next = s.p[after];
    if (next != ' ' && next != '\t') {
      fputc(' ', f);
    }
  }

  // Pretty-print remainder: canonical spacing around = and ,
  // uppercase hex digits after $/& + uppercase standalone registers a/x/y
  bool in_hex = false;
  for (size_t k = after; k < s.len; k++) {
    unsigned char c = (unsigned char)s.p[k];

    // Equals sign: normalise to exactly one space either side
    if (c == '=') {
      if (k > after && s.p[k - 1] != ' ' && s.p[k - 1] != '\t')
        fputc(' ', f);
      fputc('=', f);
      while (k + 1 < s.len && (s.p[k + 1] == ' ' || s.p[k + 1] == '\t'))
        k++;
      fputc(' ', f);
      continue;
    }

    // Start of hex literal
    if (c == '$' || c == '&') {
      in_hex = true;
      fputc((char)c, f);
      continue;
    }

    // End hex mode when leaving hex digits
    if (in_hex && !isxdigit(c)) {
      in_hex = false;
    }

    if (c == ',') {
      fputc(',', f);
      // ensure exactly one space after comma if next char exists and isn't
      // whitespace
      if (k + 1 < s.len) {
        unsigned char n = (unsigned char)s.p[k + 1];
        if (n != ' ' && n != '	') {
          fputc(' ', f);
        }
      }
      continue;
    }

    // Uppercase hex digits inside $xxxx or &xxxx
    if (in_hex && isxdigit(c)) {
      fputc((char)toupper(c), f);
      continue;
    }

    if (c == 'a' || c == 'x' || c == 'y') {
      bool before_ok = (k == after) || !isalnum((unsigned char)s.p[k - 1]);
      bool after_ok = (k + 1 >= s.len) || !isalnum((unsigned char)s.p[k + 1]);
      if (before_ok && after_ok) {
        fputc((char)toupper(c), f);
        continue;
      }
    }

    fputc((char)c, f);
  }
}

/* Emit one assembled line to the listing output.
 * Format:  [addr]  [bytes...]  source_text
 * Controlled by settings->list_show_addr, list_show_bytes, etc. */
static void listing_write(FILE *f, const asm_settings_t *settings,
                          const char *file, const char *text,
                          const line_rec_t *lines, size_t line_count,
                          const uint8_t *mem) {
  (void)file;
  (void)text;
  if (!f || !settings || !lines || !mem)
    return;

  const bool show_addr = settings->list_show_addr;
  const bool show_bytes = settings->list_show_bytes;
  unsigned bpl =
      settings->list_bytes_per_line ? settings->list_bytes_per_line : 3;
  if (bpl > 32)
    bpl = 32; // sanity cap

  // BBC-style listings never show a hex prefix in the address/byte columns.
  // Keep columns stable regardless of the user's numeric input style.
  const char hp = 0;

  const unsigned addr_chars = (show_addr ? 4u : 0u);
  const unsigned bytes_chars = (show_bytes ? (unsigned)(bpl * 3 - 1) : 0u);

  // Helpers to keep BBC-style columns stable
  const unsigned addr_field =
      show_addr ? (addr_chars + 1) : 0; // +1 space after addr
  const unsigned bytes_field =
      show_bytes ? (bytes_chars + 1) : 0; // +1 space after bytes

  const char *last_file = NULL;
  const char *first_file = (line_count && lines[0].file) ? lines[0].file : NULL;

  // Column: addr (4) + space + bytes (3*3-1=8 for default 3) + two spaces
  // We keep formatting simple and stable.
  for (size_t i = 0; i < line_count; i++) {
    const line_rec_t *r = &lines[i];

    // OPT 0 suppresses listing output entirely.
    if (r->opt_level == 0)
      continue;

    // OPT 2 enables include visibility even if --list-includes was not set.
    const bool eff_list_includes =
        settings->list_includes || (r->opt_level >= 2);

    // Suppress INCLUDE/.INCLUDE directive lines unless includes are enabled.
    if (!eff_list_includes && r->kind == LK_EMPTY && is_include_tok(r->mnem))
      continue;

    // Optional file boundary marker when include expansion switches files.
    if (eff_list_includes && r->file && r->file != last_file) {
      if (last_file) {
        // Blank addr/bytes fields to keep columns aligned.
        for (unsigned s = 0; s < addr_field; s++)
          fputc(' ', f);
        for (unsigned s = 0; s < bytes_field; s++)
          fputc(' ', f);
        fputc(' ', f);
        fputc(' ', f);
        fprintf(f, "; ---- %s ----\n", r->file);
      }
      last_file = r->file;
    }

    // Emit label on its own line (BBC style)
    // For constant-definition lines (EQU / '=') this would duplicate the line,
    // so suppress the standalone label emission for LK_EQU.
    if (r->label.len && r->kind != LK_EQU) {
      if (show_addr) {
        list_fprint_hex_u16(f, hp, r->pc);
        fputc(' ', f);
      }
      if (show_bytes) {
        // blank bytes field (default width for bpl bytes)
        for (unsigned k = 0; k < bytes_chars; k++)
          fputc(' ', f);
        fputc(' ', f);
      }
      // Macro marker column (blank unless from macro expansion)
      fputc(r->from_macro ? '+' : ' ', f);
      fputc(' ', f);
      fwrite(r->label.p, 1, r->label.len, f);

      // Optional: show which include file a label came from (only when
      // --list-includes). This is useful when listing without include boundary
      // markers.
      if (eff_list_includes && first_file && r->file && r->file != first_file) {
        fputs("  ; ", f);
        fputs(r->file, f);
      }
      fputc('\n', f);

      // BBC style: if this line is label-only, don't also print the source
      // again.
      if (r->kind == LK_EMPTY)
        continue;
    }

    // Skip truly empty lines (no label and no text)
    if (r->src.len == 0)
      continue;

    // Now emit the source line with bytes (possibly wrapped)
    uint16_t pc = r->pc;
    uint16_t n = r->emit_len;

    if (!show_bytes || n == 0) {
      if (show_addr) {
        list_fprint_hex_u16(f, hp, pc);
        fputc(' ', f);
      }
      if (show_bytes) {
        for (unsigned k = 0; k < bytes_chars; k++)
          fputc(' ', f);
        fputc(' ', f);
      }
      fputc(r->from_macro ? '+' : ' ', f);
      fputc(' ', f);
      list_fprint_pretty_source(f, r->src, settings->list_preserve_case);
      fputc('\n', f);
      continue;
    }

    // Wrapped bytes (BBC-style):
    // - address shown only on the first chunk
    // - source text shown only on the last chunk
    uint16_t off = 0;
    while (off < n) {
      unsigned chunk = n - off;
      if (chunk > bpl)
        chunk = bpl;

      const bool first_chunk = (off == 0);
      const bool last_chunk = (off + chunk >= n);

      if (show_addr) {
        if (first_chunk) {
          list_fprint_hex_u16(f, hp, pc);
          fputc(' ', f);
        } else {
          // keep columns aligned when suppressing address on continuation lines
          for (unsigned s = 0; s < addr_field; s++)
            fputc(' ', f);
        }
      }
      if (show_bytes) {
        for (unsigned k = 0; k < chunk; k++) {
          if (k)
            fputc(' ', f);
          list_fprint_hex_u8(f, hp, mem[(uint16_t)(pc + off + k)]);
        }
        // pad remaining byte slots to fixed width
        unsigned printed_chars = chunk ? (chunk * 3 - 1) : 0;
        unsigned total_chars = bytes_chars;
        for (unsigned s = printed_chars; s < total_chars; s++)
          fputc(' ', f);
        fputc(' ', f);
      }

      fputc(r->from_macro ? '+' : ' ', f);
      fputc(' ', f);

      if (last_chunk) {
        // BBC-style: print source on the final chunk
        list_fprint_pretty_source(f, r->src, settings->list_preserve_case);
      }
      fputc('\n', f);

      off = (uint16_t)(off + chunk);
    }
  }
}

// ----------------------------
// Symbol dump
// ----------------------------

typedef struct {
  const char *name; // points into symtab storage (uppercase, NUL-terminated)
  uint16_t value;
} sym_out_t;

static int sym_cmp_name(const void *a, const void *b) {
  const sym_out_t *x = (const sym_out_t *)a;
  const sym_out_t *y = (const sym_out_t *)b;
  return strcmp(x->name, y->name);
}

/* qsort comparator: sort symbol entries by value ascending, then name
 * ascending. */
static int sym_cmp_addr_then_name(const void *a, const void *b) {
  const sym_out_t *x = (const sym_out_t *)a;
  const sym_out_t *y = (const sym_out_t *)b;
  if (x->value < y->value)
    return -1;
  if (x->value > y->value)
    return 1;
  return strcmp(x->name, y->name);
}

/* Write the symbol table to f.
 * Sorted by name (default) or address (settings->sym_sort_addr).
 * Hex values are formatted with settings->sym_hex_prefix. */
static void symbols_write(FILE *f, const asm_settings_t *settings,
                          const symtab_t *st) {
  if (!f || !settings || !st)
    return;

  // Collect
  sym_out_t *arr = NULL;
  size_t n = 0, cap = 0;
  for (size_t i = 0; i < st->cap; i++) {
    const sym_entry_t *e = &st->entries[i];
    if (!e->used)
      continue;
    if (n == cap) {
      size_t ncap = cap ? cap * 2 : 64;
      sym_out_t *na = (sym_out_t *)realloc(arr, ncap * sizeof(sym_out_t));
      if (!na) {
        free(arr);
        return;
      }
      arr = na;
      cap = ncap;
    }
    arr[n].name = e->key;
    arr[n].value = e->value;
    n++;
  }

  if (settings->sym_sort_addr)
    qsort(arr, n, sizeof(arr[0]), sym_cmp_addr_then_name);
  else
    qsort(arr, n, sizeof(arr[0]), sym_cmp_name);

  // BBC-style symbol dump: address then name, no hex prefix by default.
  // If the user explicitly sets --symhexprefix, we honour it.
  const char hp = settings->sym_hex_prefix; // 0 => none

  for (size_t i = 0; i < n; i++) {
    list_fprint_hex_u16(f, hp, arr[i].value);
    fputs("  ", f);
    fputs(arr[i].name, f);
    fputc('\n', f);
  }

  free(arr);
}

// ----------------------------
// Output overwrite warnings
// ----------------------------
static const asm_settings_t *g_ow_settings = NULL;
static const char *g_ow_file = NULL;
static diags_t *g_ow_diags = NULL;
static line_rec_t *g_ow_lines = NULL;
static size_t g_ow_line_count = 0;
static size_t g_ow_cur_index = 0;
static uint16_t *g_ow_owner =
    NULL; // 0xFFFF = never written, else index into g_ow_lines
static uint8_t *g_ow_firstval = NULL; // first value written at address
static unsigned g_ow_warn_count = 0;
static bool g_ow_warn_suppressed = false;
static bool g_ow_blame_active =
    false; // set when PC moved into already-written area
static size_t g_ow_blame_index = 0; // line index of the PC/ORG/*= directive

// Coalesced overwrite run for the current emitting line.
// We coalesce only when contiguous and written first by the same line.
static bool g_ow_run_active = false;
static uint16_t g_ow_run_start = 0;
static uint16_t g_ow_run_end = 0;
static uint16_t g_ow_run_first_idx = 0xFFFF;
static uint8_t g_ow_run_old[16];
static uint8_t g_ow_run_new[16];
static unsigned g_ow_run_len = 0;
static bool g_ow_run_trunc = false;

/* written_explicit: set for all emissions EXCEPT .res default-fill bytes.
 * Used by the memory map to show only "intentional" data, not variable reservations. */
static uint8_t *g_written_explicit = NULL;
static bool     g_emit_is_res_fill = false; /* set true only during .res emission */
static bool     g_pc_was_set       = false; /* true once .org/.pc= has been seen */

// ---------------------------------------------------------------------------
// Guard ranges: .halt/.alert / .unhalt/.unalert
// ---------------------------------------------------------------------------
#define GUARD_MAX 64
#define GUARD_MSG_MAX 256
#define GUARD_HIT_LIMIT 5  /* max diagnostics per range before "..." suppression */

typedef struct {
  uint16_t start;
  uint16_t end;
  bool     is_halt;   // true = error (halt assembly), false = warning (alert)
  char     msg[GUARD_MSG_MAX];
  unsigned hit_count; // number of diagnostics emitted for this range so far
} guard_range_t;

static guard_range_t g_guards[GUARD_MAX];
static int           g_guard_count = 0;
static bool          g_guard_enabled = false; // only true during explicit assemble
static diags_t      *g_guard_diags = NULL;
static const char   *g_guard_file  = NULL;
static size_t        g_guard_cur_index = 0;
static line_rec_t   *g_guard_lines = NULL;
static size_t        g_guard_line_count = 0;

static void guards_reset(void) {
  g_guard_count   = 0;
  g_guard_enabled = false;
}

/* Add or update a guard range.  If an existing range with the same start
 * address exists it is replaced (allows re-defining with a different message
 * or severity without needing an explicit .unhalt first). */
static void guard_add(uint16_t start, uint16_t end, bool is_halt,
                      const char *msg) {
  for (int i = 0; i < g_guard_count; i++) {
    if (g_guards[i].start == start && g_guards[i].end == end) {
      g_guards[i].is_halt   = is_halt;
      g_guards[i].hit_count = 0;
      strncpy(g_guards[i].msg, msg ? msg : "", GUARD_MSG_MAX - 1);
      g_guards[i].msg[GUARD_MSG_MAX - 1] = '\0';
      return;
    }
  }
  if (g_guard_count >= GUARD_MAX)
    return; /* silently cap — add a diagnostic here if desired */
  g_guards[g_guard_count].start   = start;
  g_guards[g_guard_count].end     = end;
  g_guards[g_guard_count].is_halt = is_halt;
  strncpy(g_guards[g_guard_count].msg, msg ? msg : "", GUARD_MSG_MAX - 1);
  g_guards[g_guard_count].msg[GUARD_MSG_MAX - 1] = '\0';
  g_guard_count++;
}

/* Remove all guard ranges whose start address equals `start`. */
static void guard_remove(uint16_t start) {
  int w = 0;
  for (int i = 0; i < g_guard_count; i++) {
    if (g_guards[i].start != start)
      g_guards[w++] = g_guards[i];
  }
  g_guard_count = w;
}

/* Format the user's message, substituting:
 *   %s  ->  4-digit uppercase hex of the write address
 *   %r  ->  "XXXX-YYYY" range string
 * Any other % sequence is passed through unchanged. */
static void guard_format_msg(char *out, size_t cap, const char *fmt,
                             uint16_t addr, uint16_t rstart, uint16_t rend) {
  size_t wi = 0;
  for (const char *p = fmt; *p && wi + 1 < cap; p++) {
    if (*p == '%' && *(p + 1)) {
      p++;
      if (*p == 's') {
        /* actual write address */
        int n = snprintf(out + wi, cap - wi, "%04X", (unsigned)addr);
        if (n > 0) wi += (size_t)n;
      } else if (*p == 'r') {
        /* range start-end */
        int n = snprintf(out + wi, cap - wi, "%04X-%04X",
                         (unsigned)rstart, (unsigned)rend);
        if (n > 0) wi += (size_t)n;
      } else {
        /* pass through */
        if (wi + 1 < cap) out[wi++] = '%';
        if (wi + 1 < cap) out[wi++] = *p;
      }
    } else {
      out[wi++] = *p;
    }
  }
  out[wi] = '\0';
}

/* Called from emit_u8 to check a write address against all guard ranges.
 * Uses g_ow_lines[g_ow_cur_index] for file/line attribution, matching the
 * overwrite-warning infrastructure. */
static void guard_check(uint16_t addr) {
  if (!g_guard_enabled || !g_guard_diags || g_guard_count == 0)
    return;
  for (int i = 0; i < g_guard_count; i++) {
    guard_range_t *g = &g_guards[i];
    if (addr < g->start || addr > g->end)
      continue;
    /* Locate the emitting line for the diagnostic. */
    const char *ef = g_guard_file ? g_guard_file : "<input>";
    unsigned    eln = 1;
    unsigned    ecol = 1;
    if (g_guard_lines && g_guard_cur_index < g_guard_line_count) {
      const line_rec_t *r = &g_guard_lines[g_guard_cur_index];
      ef   = r->file ? r->file : ef;
      eln  = r->line_no;
      ecol = r->op_col ? r->op_col : 1;
    }
    g->hit_count++;
    if (g->hit_count == GUARD_HIT_LIMIT + 1) {
      /* Emit the "..." suppression notice on the line that pushed over the limit. */
      diag_addf(g_guard_diags,
                g->is_halt ? DIAG_ERROR : DIAG_WARNING,
                ef, eln, ecol, 1,
                "... (further guard hits for $%04X-$%04X suppressed)",
                (unsigned)g->start, (unsigned)g->end);
    }
    if (g->hit_count > GUARD_HIT_LIMIT)
      return; /* suppressed */
    char buf[GUARD_MSG_MAX + 32];
    guard_format_msg(buf, sizeof(buf), g->msg, addr, g->start, g->end);
    diag_addf(g_guard_diags,
              g->is_halt ? DIAG_ERROR : DIAG_WARNING,
              ef, eln, ecol, 1,
              "%s", buf[0] ? buf : (g->is_halt ? "write to guarded address"
                                                : "write to alerted address"));
    return; /* one diagnostic per emit_u8 call is enough */
  }
}

/* Copy up to dstcap-1 bytes from s into dst, NUL-terminating.
 * Used to embed token text in overwrite-warning messages. */
static void ow_sv_snip(char *dst, size_t dstcap, sv_t s) {
  if (!dstcap)
    return;
  size_t n = 0;
  // trim leading ws
  while (n < s.len && (s.p[n] == ' ' || s.p[n] == '\t'))
    n++;
  const char *p = s.p + n;
  size_t len = s.len - n;
  if (len > dstcap - 1)
    len = dstcap - 1;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)p[i];
    if (c == '\r' || c == '\n') {
      dst[i] = ' ';
    } else
      dst[i] = (char)c;
  }
  dst[len] = 0;
}

/* Return the current number of overwrite warnings emitted this assembly run.
 * (State is held in a static variable reset at the start of each pass.) */
static size_t ow_diag_index(void) {
  if (g_ow_blame_active && g_ow_blame_index < g_ow_line_count)
    return g_ow_blame_index;
  return g_ow_cur_index;
}

/* Emit the "further overwrite warnings suppressed" message when the limit is
 * hit. */
static void ow_emit_limit_warning(void) {
  if (g_ow_warn_suppressed)
    return;
  g_ow_warn_suppressed = true;
  size_t li = ow_diag_index();
  const line_rec_t *r = &g_ow_lines[li];
  unsigned lim = g_ow_settings->overwrite_warn_limit;
  diag_addf(
      g_ow_diags, DIAG_WARNING, g_ow_file ? g_ow_file : "<input>", r->line_no,
      r->mnem_col ? r->mnem_col : 1, 1,
      "overwrite warning limit reached (%u); further overwrites suppressed",
      lim);
}

/* Emit a single overwrite warning for the current run of consecutive
 * overwritten bytes, then reset the run state. */
static void ow_emit_run_warning(void) {
  if (!g_ow_settings || !g_ow_settings->overwrite_warn)
    return;
  if (!g_ow_diags || !g_ow_lines)
    return;
  if (!g_ow_run_active)
    return;

  unsigned lim = g_ow_settings->overwrite_warn_limit;
  if (lim != 0 && g_ow_warn_count >= lim) {
    ow_emit_limit_warning();
    g_ow_run_active = false;
    return;
  }

  const line_rec_t *r1 = (g_ow_run_first_idx != 0xFFFF &&
                          (size_t)g_ow_run_first_idx < g_ow_line_count)
                             ? &g_ow_lines[g_ow_run_first_idx]
                             : NULL;
  const line_rec_t *r2 = &g_ow_lines[g_ow_cur_index];
  size_t li = ow_diag_index();
  const line_rec_t *rloc = &g_ow_lines[li];

  char s1[80] = {0}, s2[80] = {0};
  if (r1)
    ow_sv_snip(s1, sizeof(s1), r1->src);
  ow_sv_snip(s2, sizeof(s2), r2->src);

  // Address formatting: match listing preference. If none selected, default to
  // BBC-style '&'.
  char hp = g_ow_settings->list_hex_prefix;
  if (!hp)
    hp = '&';
  char addrbuf[32];
  snprintf(addrbuf, sizeof(addrbuf), "%c%04X", hp, (unsigned)g_ow_run_start);
  char endbuf[16];
  if (g_ow_run_end != g_ow_run_start) {
    snprintf(endbuf, sizeof(endbuf), "%c%04X", hp, (unsigned)g_ow_run_end);
  } else {
    endbuf[0] = 0;
  }

  // Build old/new byte previews
  char oldb[128] = {0};
  char newb[128] = {0};
  size_t op = 0, np = 0;
  for (unsigned i = 0; i < g_ow_run_len && i < 16; i++) {
    if (i) {
      if (op + 1 < sizeof(oldb))
        oldb[op++] = ' ';
      if (np + 1 < sizeof(newb))
        newb[np++] = ' ';
    }
    op += (size_t)snprintf(oldb + op, sizeof(oldb) - op, "%02X",
                           (unsigned)g_ow_run_old[i]);
    np += (size_t)snprintf(newb + np, sizeof(newb) - np, "%02X",
                           (unsigned)g_ow_run_new[i]);
    if (op >= sizeof(oldb) || np >= sizeof(newb))
      break;
  }
  if (g_ow_run_trunc) {
    strncat(oldb, " ...", sizeof(oldb) - strlen(oldb) - 1);
    strncat(newb, " ...", sizeof(newb) - strlen(newb) - 1);
  }

  unsigned nbytes = (unsigned)(g_ow_run_end - g_ow_run_start + 1);

  if (r1) {
    if (g_ow_run_end == g_ow_run_start) {
      diag_addf(g_ow_diags, DIAG_WARNING, g_ow_file ? g_ow_file : "<input>",
                rloc->line_no, rloc->mnem_col ? rloc->mnem_col : 1, 1,
                "memory overwrite at %s (old=%s new=%s). 1st write line %u: "
                "%s. 2nd write line %u: %s",
                addrbuf, oldb, newb, r1->line_no, s1, r2->line_no, s2);
    } else {
      diag_addf(g_ow_diags, DIAG_WARNING, g_ow_file ? g_ow_file : "<input>",
                rloc->line_no, rloc->mnem_col ? rloc->mnem_col : 1, 1,
                "memory overwrite at %s..%s (%u bytes). old=[%s] new=[%s]. 1st "
                "write line %u: %s. 2nd write line %u: %s",
                addrbuf, endbuf, nbytes, oldb, newb, r1->line_no, s1,
                r2->line_no, s2);
    }
  } else {
    diag_addf(g_ow_diags, DIAG_WARNING, g_ow_file ? g_ow_file : "<input>",
              rloc->line_no, rloc->mnem_col ? rloc->mnem_col : 1, 1,
              "memory overwrite at %s (old=%s new=%s)", addrbuf, oldb, newb);
  }

  g_ow_warn_count++;
  g_ow_run_active = false;
}

/* Flush any pending overwrite-warning run before moving to a non-consecutive
 * address. */
static void ow_flush_run(void) {
  if (g_ow_run_active)
    ow_emit_run_warning();
  g_ow_run_active = false;
  g_ow_run_trunc = false;
  g_ow_run_len = 0;
}

/* Record a single byte-overwrite event.
 * Consecutive overwrites at adjacent addresses are grouped into one warning. */
static void ow_note(uint16_t addr, uint8_t oldv, uint8_t newv,
                    uint16_t first_idx) {
  if (!g_ow_settings || !g_ow_settings->overwrite_warn)
    return;
  if (!g_ow_diags || !g_ow_lines)
    return;

  if (!g_ow_run_active) {
    g_ow_run_active = true;
    g_ow_run_start = addr;
    g_ow_run_end = addr;
    g_ow_run_first_idx = first_idx;
    g_ow_run_len = 0;
    g_ow_run_trunc = false;
  } else {
    // coalesce only if contiguous and same first-writer line
    if ((uint16_t)(g_ow_run_end + 1) != addr ||
        g_ow_run_first_idx != first_idx) {
      ow_emit_run_warning();
      g_ow_run_active = true;
      g_ow_run_start = addr;
      g_ow_run_end = addr;
      g_ow_run_first_idx = first_idx;
      g_ow_run_len = 0;
      g_ow_run_trunc = false;
    } else {
      g_ow_run_end = addr;
    }
  }

  if (g_ow_run_len < 16) {
    g_ow_run_old[g_ow_run_len] = oldv;
    g_ow_run_new[g_ow_run_len] = newv;
    g_ow_run_len++;
  } else {
    g_ow_run_trunc = true;
  }
}

/* Emit one byte to the memory image at the current PC, advancing PC.
 * Records the write in written[] and tracks min/max written addresses.
 * Emits an overwrite warning if the address was already written. */
static void emit_u8(uint8_t *mem, uint8_t *written, uint16_t *minw,
                    uint16_t *maxw, uint16_t addr, uint8_t v) {
  if (g_ow_owner && g_ow_firstval) {
    uint16_t oi = g_ow_owner[addr];
    if (oi != 0xFFFF) {
      ow_note(addr, g_ow_firstval[addr], v, oi);
    } else {
      // first writer
      if (g_ow_cur_index <= 0xFFFE) {
        g_ow_owner[addr] = (uint16_t)g_ow_cur_index;
        g_ow_firstval[addr] = v;
      }
    }
  }

  mem[addr] = v;
  if (written)
    written[addr] = 1;
  /* Mark as explicit (non-fill) only when not emitting .res placeholder bytes
   * AND the PC has been explicitly set with .org/.pc= at least once */
  if (g_written_explicit && !g_emit_is_res_fill && g_pc_was_set)
    g_written_explicit[addr] = 1;
  /* Check address against .halt/.alert guard ranges. */
  guard_check(addr);
  if (minw && maxw) {
    if (addr < *minw)
      *minw = addr;
    if (addr > *maxw)
      *maxw = addr;
  }
}

/* Emit a 16-bit little-endian word to the memory image, advancing PC by 2. */
static void emit_le16(uint8_t *mem, uint8_t *written, uint16_t *minw,
                      uint16_t *maxw, uint16_t addr, uint16_t v) {
  emit_u8(mem, written, minw, maxw, addr, (uint8_t)(v & 0xFF));
  emit_u8(mem, written, minw, maxw, (uint16_t)(addr + 1),
          (uint8_t)((v >> 8) & 0xFF));
}

/* Emit a 32-bit little-endian doubleword to the memory image, advancing PC
 * by 4. */
static void emit_le32(uint8_t *mem, uint8_t *written, uint16_t *minw,
                      uint16_t *maxw, uint16_t addr, uint32_t v) {
  emit_u8(mem, written, minw, maxw, addr, (uint8_t)(v & 0xFF));
  emit_u8(mem, written, minw, maxw, (uint16_t)(addr + 1),
          (uint8_t)((v >> 8) & 0xFF));
  emit_u8(mem, written, minw, maxw, (uint16_t)(addr + 2),
          (uint8_t)((v >> 16) & 0xFF));
  emit_u8(mem, written, minw, maxw, (uint16_t)(addr + 3),
          (uint8_t)((v >> 24) & 0xFF));
}

// --- tiny string pool (for include file names) ---
typedef struct {
  char **v;
  size_t n;
  size_t cap;
} strpool_t;

static const char *strpool_intern(strpool_t *sp, const char *s, size_t n) {
  if (!sp || !s)
    return "<input>";
  // linear search is fine (usually small)
  for (size_t i = 0; i < sp->n; i++) {
    const char *e = sp->v[i];
    if (strlen(e) == n && memcmp(e, s, n) == 0)
      return e;
  }
  if (sp->n == sp->cap) {
    size_t ncap = sp->cap ? sp->cap * 2 : 8;
    char **nv = (char **)realloc(sp->v, ncap * sizeof(char *));
    if (!nv)
      return "<input>";
    sp->v = nv;
    sp->cap = ncap;
  }
  char *cp = (char *)malloc(n + 1);
  if (!cp)
    return "<input>";
  memcpy(cp, s, n);
  cp[n] = 0;
  sp->v[sp->n++] = cp;
  return cp;
}

/* Free all strings owned by the string pool and reset it. */
static void strpool_free(strpool_t *sp) {
  if (!sp)
    return;
  for (size_t i = 0; i < sp->n; i++)
    free(sp->v[i]);
  free(sp->v);
  sp->v = NULL;
  sp->n = sp->cap = 0;
}

/* Core assembler line parser — shared by both the parse-only and full-assembly
 * passes. Processes each source line: resolves labels, interprets directives
 * (org, byte, word, dword, res, align, incbin, macro calls, proc/scope), and
 * emits opcodes. in:  pass       — 1 (label collection) or 2 (code emission)
 *      emit       — if false, opcodes are sized but not written to mem
 *      mem/written — 64KB image and write-tracking array (pass 2 only)
 *      syms       — symbol table, populated in pass 1, read in pass 2 */

/* ---------------------------------------------------------------------------
 * colon_find_separator: find a ':' statement separator in a source line.
 * Not a separator if inside string, after comment, or is a label colon.
 * --------------------------------------------------------------------------- */
static const char *asm_colon_find_sep(const char *p, const char *end) {
    bool in_dq   = false;
    bool in_sq   = false;
    bool after_id = false;
    bool at_start = true;

    for (; p < end; p++) {
        char ch = *p;
        if (!in_dq && !in_sq && ch == ';') break;
        if (!in_sq && ch == '"')  { in_dq = !in_dq; after_id = false; at_start = false; continue; }
        if (!in_dq && ch == '\'') { in_sq = !in_sq; after_id = false; at_start = false; continue; }
        if (in_dq || in_sq) continue;
        if (ch == ':') {
            if (after_id && at_start) { after_id = false; at_start = false; continue; } /* label colon */
            return p;
        }
        bool is_id = (ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||
                     ch=='_'||ch=='.'||ch=='$'||ch=='@';
        if (ch==' '||ch=='\t') { if (after_id) { after_id=false; at_start=false; } }
        else if (is_id) { if (at_start) after_id=true; }
        else { after_id=false; at_start=false; }
    }
    return NULL;
}

static bool parse_lines(const char *file, const char *text,
                        line_rec_t **out_lines, size_t *out_count,
                        diags_t *diags, bool allow_undoc,
                        strpool_t *file_pool) {
  *out_lines = NULL;
  *out_count = 0;
  size_t cap = 0;

  // Track current file/line for include-expanded streams.
  const char *p = text;
  const char *cur_file = file;
  unsigned line_no = 1;

  // Intern file names found in file-mark lines so pointers remain valid.
  while (*p) {
    const char *line_start = p;
    /* Line-number reset marker from preprocessor macro expansion */
    if ((unsigned char)*line_start == 0x1f) {
      const char *line_end = line_start + 1;
      while (*line_end && *line_end != '\n')
        line_end++;
      {
        char *_ae;
        unsigned long _al = strtoul(line_start + 1, &_ae, 10);
        line_no = (_ae != line_start + 1) ? (unsigned)_al : line_no;
      }
      p = (*line_end == '\n') ? line_end + 1 : line_end;
      continue;
    }
    bool from_macro = false;
    if (*line_start == '\x1e') {
      from_macro = true;
      line_start++;
    }
    const char *line_end = p;
    while (*line_end && *line_end != '\n')
      line_end++;

    // File mark line from the preprocessor (include expansion)
    if (!from_macro && *line_start == '\x1d') {
      const char *fn = line_start + 1;
      size_t fn_len = (size_t)(line_end - fn);
      cur_file = strpool_intern(file_pool, fn, fn_len);
      // Next line in the new file is line 1
      line_no = 1;
      p = (*line_end == '\n') ? line_end + 1 : line_end;
      continue;
    }

    /* Colon statement separator: split "LDA #1 : STA $10" into two records
     * on the same line number. We loop over colon-separated segments. */
    const char *seg_start = line_start;
    const char *seg_end   = line_end;
    const char *colon_sep = asm_colon_find_sep(line_start, line_end);
    if (colon_sep) {
        seg_end = colon_sep;
        /* Trim trailing whitespace from first segment */
        while (seg_end > seg_start && (seg_end[-1]==' '||seg_end[-1]=='	')) seg_end--;
    }
    do {
    /* Use seg_start/seg_end as the effective line for this sub-statement.
     * col starts at the segment's column offset in the original line so
     * all error column numbers point to the right place in the editor. */
    line_cursor_t cur = {seg_start, seg_end, line_no};
    unsigned col = (unsigned)(seg_start - line_start) + 1;
    skip_ws(&cur, &col);
    const char *tok0 = cur.p;
    unsigned tok0_col = col;

    // If the first token looks like an attempted "label:" but contains illegal
    // characters, emit a clear label diagnostic instead of "unknown
    // instruction".
    if (tok0 < cur.line_end && tok0[0] != '.' && tok0[0] != '@') {
      const char *q = tok0;
      while (q < cur.line_end && *q != ';' && !is_ws(*q))
        q++;
      if (q > tok0 && q[-1] == ':') {
        const char *name_end = q - 1;
        unsigned i = 0;
        for (const char *r = tok0; r < name_end; r++, i++) {
          char ch = *r;
          bool ok = (i == 0) ? is_ident_start(ch) : is_ident_char(ch);
          if (!ok) {
            unsigned err_col = tok0_col + i;
            diag_addf(diags, DIAG_ERROR, cur_file, line_no, err_col, 1,
                      "illegal character(s) in label name");
            // Skip rest of line to avoid cascades
            cur.p = cur.line_end;
            col = (unsigned)(cur.line_end - line_start) + 1;
            break;
          }
        }
      }
    }

    line_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.file = cur_file;
    rec.line_no = line_no;
    rec.kind = LK_EMPTY;
    rec.from_macro = from_macro;

    // Capture trimmed source text for listing
    // Use seg_start/seg_end so each colon-split sub-statement shows correctly
    const char *ts = seg_start;
    const char *te = seg_end;
    // trim leading whitespace
    while (ts < te && (*ts == ' ' || *ts == '\t' || *ts == '\r'))
      ts++;
    // trim trailing whitespace
    while (te > ts && (te[-1] == ' ' || te[-1] == '\t' || te[-1] == '\r'))
      te--;
    rec.src.p = ts;
    rec.src.len = (size_t)(te - ts);
    rec.pc = 0;
    rec.emit_len = 0;

    // Anonymous label definition at start of line: ':' or '@:' or '.' (dot)
    // Note: '.' only counts as an anonymous label if it stands alone (followed
    // by whitespace/end-of-line/comment), so it won't conflict with directives
    // like '.cpu' or dot-labels like '.loop'.
    if (cur.p < cur.line_end) {
      bool anon_is_colon = (*cur.p == ':');
      bool anon_is_at_colon =
          (*cur.p == '@' && (cur.p + 1) < cur.line_end && cur.p[1] == ':');
      bool anon_is_dot = false;
      if (*cur.p == '.') {
        const char *n = cur.p + 1;
        if (n >= cur.line_end) {
          anon_is_dot = true;
        } else {
          char nc = *n;
          anon_is_dot = is_ws(nc) || nc == '\r' || nc == ';' || nc == '/';
        }
      }
      if (anon_is_colon || anon_is_at_colon || anon_is_dot) {
        rec.anon_label_def = true;
        rec.anon_label_col = col;
        rec.anon_label_len = (*cur.p == ':') ? 1u : 2u;
        cur.p += rec.anon_label_len;
        col += rec.anon_label_len;
        skip_ws(&cur, &col);
      }
    }

    if (cur.p < cur.line_end) {
      sv_t first = {0};
      const char *save_p = cur.p;
      unsigned save_col = col;
      unsigned first_len = 0;

      // @ local label: @ident or @ident:
      if (cur.p < cur.line_end && *cur.p == '@') {
        const char *at_p = cur.p;
        unsigned at_col = col;
        const char *n = cur.p + 1;
        if (n < cur.line_end && is_ident_start(*n)) {
          cur.p++;
          col++;
          if (take_ident(&cur, &first, &col, &first_len)) {
            first.p = at_p;
            first.len++;
            first_len++;
            if (peek_char(&cur, ':')) {
              rec.label = first;
              rec.label_col = at_col;
              rec.label_has_colon = true;
              cur.p++;
              col++;
            } else {
              rec.label = first;
              rec.label_col = at_col;
              rec.label_has_colon = false;
            }
            skip_ws(&cur, &col);
          } else {
            cur.p = save_p;
            col = save_col;
          }
        }
      }

      if (!rec.label.len && take_ident(&cur, &first, &col, &first_len)) {
        if (first.len && first.p[0] == '.') {
          if (peek_char(&cur, ':')) {
            // disallow .label:
            diag_addf(
                diags, DIAG_ERROR, cur_file, line_no, save_col,
                (unsigned)first.len + 1,
                "dot-labels must not end with ':' (use '.label' or 'label:')");
            cur.p++;
            col++;
          } else if (!is_dot_directive(first)) {
            // Dot-label must have a name after '.'
            if (first.len == 1) {
              // If there are non-space characters immediately after '.', then
              // the user attempted a dot-label but used an illegal character.
              unsigned err_col = save_col;
              const char *q = cur.p; // positioned just after '.'
              while (q < cur.line_end && (*q == ' ' || *q == '\t'))
                q++;
              if (q < cur.line_end) {
                // Point at the first offending character (e.g. ".^Rt$")
                err_col = (unsigned)(q - line_start) + 1;
                diag_addf(diags, DIAG_ERROR, cur_file, line_no, err_col, 1,
                          "illegal character(s) in label name");
              } else {
                diag_addf(
                    diags, DIAG_ERROR, cur_file, line_no, err_col, 1,
                    "expected label name after '.' (use '.label' or 'label:')");
              }
              // Skip rest of line to avoid cascaded errors.
              cur.p = cur.line_end;
              col = (unsigned)(cur.line_end - line_start) + 1;
            } else {
              rec.label = first;
              rec.label_col = save_col;
              rec.label_has_colon = false;
              skip_ws(&cur, &col);
            }
          } else {
            cur.p = save_p;
            col = save_col;
          }
        } else {
          if (peek_char(&cur, ':')) {
            rec.label = first;
            rec.label_col = save_col;
            rec.label_has_colon = true;
            cur.p++;
            col++;
            skip_ws(&cur, &col);
          } else {

            // No colon: allow classic assembler forms like
            //   Label
            //   Label  jmp Target
            //   Label  .word Target
            // while still preferring mnemonics/directives when the first token
            // is clearly an opcode or directive-like token.
            line_cursor_t look = cur;
            unsigned lcol = col;
            skip_ws(&look, &lcol);
            if (peek_char(&look, '=')) {
              rec.label = first;
              rec.label_col = save_col;
              rec.label_has_colon = false;
              // advance real cursor to after whitespace so '=' is read as the
              // mnemonic
              cur = look;
              col = lcol;
            } else {
              sv_t peek = {0};
              unsigned plen = 0;
              line_cursor_t look2 = look;
              unsigned lcol2 = lcol;
              if (take_ident(&look2, &peek, &lcol2, &plen) &&
                  sv_ieq(peek, "equ")) {
                rec.label = first;
                rec.label_col = save_col;
                rec.label_has_colon = false;
                skip_ws(&cur, &col);
              } else if (!is_known_instruction_tok(first, allow_undoc) &&
                         is_probable_colonless_label_follow(&cur,
                                                            allow_undoc)) {
                rec.label = first;
                rec.label_col = save_col;
                rec.label_has_colon = false;
                skip_ws(&cur, &col);
              } else {
                // Not a label definition; rewind so it becomes mnemonic.
                cur.p = save_p;
                col = save_col;
              }
            }
          }
        }
      } // end if (!rec.label.len && take_ident)

      if (cur.p < cur.line_end) {
        // mnemonic or PC-setter
        rec.mnem_col = col;

        // Accept both "*=" and "* =" (optional whitespace around '=')
        if (peek_char(&cur, '*')) {
          const char *save_p = cur.p;
          unsigned save_col = col;
          cur.p++;
          col++;
          skip_ws(&cur, &col);
          if (peek_char(&cur, '=')) {
            static const char k_star_eq[] = "*=";
            cur.p++;
            col++;
            rec.mnem.p = k_star_eq;
            rec.mnem.len = 2;
          } else {
            cur.p = save_p;
            col = save_col;
          }
        }

        if (rec.mnem.len == 0) {
          // Allow "NAME = expr" constant definitions (only when a label was
          // parsed).
          if (rec.label.len && peek_char(&cur, '=')) {
            static const char k_eq[] = "=";
            rec.mnem.p = k_eq;
            rec.mnem.len = 1;
            cur.p++;
            col++;
          } else {
            unsigned mlen = 0;
            if (take_ident(&cur, &rec.mnem, &col, &mlen)) {
              // Allow "pc=" and ".pc=" (and spaced "pc =" / ".pc =")
              if (sv_ieq(rec.mnem, "pc") || sv_ieq(rec.mnem, ".pc")) {
                const char *save_p = cur.p;
                unsigned save_col = col;
                skip_ws(&cur, &col);
                if (peek_char(&cur, '=')) {
                  static const char k_pc_eq[] = "pc=";
                  static const char k_dotpc_eq[] = ".pc=";
                  cur.p++;
                  col++;
                  if (sv_ieq(rec.mnem, "pc")) {
                    rec.mnem.p = k_pc_eq;
                    rec.mnem.len = 3;
                  } else {
                    rec.mnem.p = k_dotpc_eq;
                    rec.mnem.len = 4;
                  }
                } else {
                  cur.p = save_p;
                  col = save_col;
                }
              }

              // Allow "org" and ".org" with optional whitespace around '=':
              // "org =" (treated identically to plain "org" / ".org").
              if (sv_ieq(rec.mnem, "org") || sv_ieq(rec.mnem, ".org")) {
                const char *save_p = cur.p;
                unsigned save_col = col;
                skip_ws(&cur, &col);
                if (peek_char(&cur, '=')) {
                  cur.p++;
                  col++;
                } else {
                  cur.p = save_p;
                  col = save_col;
                }
              }
            }
          }
        }
        skip_ws(&cur, &col);
        rec.op_col = col;
        if (cur.p < cur.line_end) {
          rec.operand.p = cur.p;
          rec.operand.len = (size_t)(cur.line_end - cur.p);
          rec.operand = sv_trim(rec.operand);
        }
        if (rec.mnem.len) {
          if (is_include_tok(rec.mnem)) {
            rec.kind = LK_EMPTY; // include was already expanded by preproc
          } else {
            // Constant definition: NAME EQU expr  OR  NAME = expr
            if (sv_ieq(rec.mnem, "equ") || sv_ieq(rec.mnem, "=")) {
              if (!rec.label.len) {
                diag_addf(diags, DIAG_ERROR, cur_file, line_no, rec.mnem_col,
                          (unsigned)rec.mnem.len,
                          "EQU requires a symbol name (use NAME EQU expr or "
                          "NAME = expr)");
                rec.kind = LK_EMPTY;
              } else if (rec.label_has_colon) {
                diag_addf(diags, DIAG_ERROR, cur_file, line_no, rec.label_col,
                          (unsigned)rec.label.len,
                          "EQU cannot be used with 'label:' (omit ':')");
                rec.kind = LK_EMPTY;
              } else {
                rec.kind = LK_EQU;
              }
            } else {
              bool is_pc_set =
                  sv_ieq(rec.mnem, "*=") || sv_ieq(rec.mnem, ".org") ||
                  sv_ieq(rec.mnem, "org") || sv_ieq(rec.mnem, "pc=") ||
                  sv_ieq(rec.mnem, ".pc=");
              if (is_pc_set)
                rec.kind = LK_PC_SET;
              else {
                bool is_opt =
                    sv_ieq(rec.mnem, "opt") || sv_ieq(rec.mnem, ".opt");
                if (is_opt) {
                  rec.kind = LK_OPT;
                } else {
                  bool is_diag = sv_ieq(rec.mnem, ".error") ||
                                 sv_ieq(rec.mnem, "error") ||
                                 sv_ieq(rec.mnem, ".warning") ||
                                 sv_ieq(rec.mnem, "warning");
                  bool is_assert =
                      sv_ieq(rec.mnem, ".assert") || sv_ieq(rec.mnem, "assert");
                  bool is_guard =
                      sv_ieq(rec.mnem, ".halt")    || sv_ieq(rec.mnem, "halt") ||
                      sv_ieq(rec.mnem, ".alert")   || sv_ieq(rec.mnem, "alert") ||
                      sv_ieq(rec.mnem, ".unhalt")  || sv_ieq(rec.mnem, "unhalt") ||
                      sv_ieq(rec.mnem, ".unalert") || sv_ieq(rec.mnem, "unalert");
                  if (is_diag)
                    rec.kind = LK_DIAG;
                  else if (is_assert)
                    rec.kind = LK_ASSERT;
                  else if (is_guard)
                    rec.kind = LK_GUARD;
                  else {
                    bool is_align =
                        sv_ieq(rec.mnem, ".align") || sv_ieq(rec.mnem, "align");
                    bool is_res =
                        sv_ieq(rec.mnem, ".res") || sv_ieq(rec.mnem, "res");
                    bool is_incbin = sv_ieq(rec.mnem, ".incbin") ||
                                     sv_ieq(rec.mnem, "incbin");
                    bool is_longa =
                        sv_ieq(rec.mnem, ".longa") || sv_ieq(rec.mnem, "longa");
                    bool is_longi =
                        sv_ieq(rec.mnem, ".longi") || sv_ieq(rec.mnem, "longi");
                    bool is_data =
                        sv_ieq(rec.mnem, "equb") || sv_ieq(rec.mnem, "eqb") ||
                        sv_ieq(rec.mnem, "db") || sv_ieq(rec.mnem, "equw") ||
                        sv_ieq(rec.mnem, "eqw") || sv_ieq(rec.mnem, "dq") ||
                        sv_ieq(rec.mnem, ".word") || sv_ieq(rec.mnem, "equd") ||
                        sv_ieq(rec.mnem, "eqd") || sv_ieq(rec.mnem, "dd") ||
                        sv_ieq(rec.mnem, ".dword") ||
                        sv_ieq(rec.mnem, ".byte") || sv_ieq(rec.mnem, "equs");
                    bool is_proc =
                        sv_ieq(rec.mnem, ".proc") || sv_ieq(rec.mnem, "proc");
                    bool is_endproc = sv_ieq(rec.mnem, ".endproc") ||
                                      sv_ieq(rec.mnem, "endproc");
                    bool is_scope = sv_ieq(rec.mnem, ".scope");
                    bool is_endscope = sv_ieq(rec.mnem, ".endscope");
                    rec.kind = is_proc       ? LK_PROC
                               : is_endproc  ? LK_ENDPROC
                               : is_scope    ? LK_SCOPE
                               : is_endscope ? LK_ENDSCOPE
                               : is_align    ? LK_ALIGN
                               : is_res      ? LK_RES
                               : is_incbin   ? LK_INCBIN
                               : is_longa    ? LK_LONGA
                               : is_longi    ? LK_LONGI
                               : is_data     ? LK_DATA
                                             : LK_INSTR;
                  }
                }
              }
            }
          }
        }
      }
    }

    if (cap == *out_count) {
      size_t ncap = cap ? cap * 2 : 256;
      line_rec_t *n =
          (line_rec_t *)realloc(*out_lines, ncap * sizeof(line_rec_t));
      if (!n) {
        diag_addf(diags, DIAG_ERROR, cur_file, line_no, 1, 1,
                  "out of memory while parsing");
        return false;
      }
      *out_lines = n;
      cap = ncap;
    }
    (*out_lines)[(*out_count)++] = rec;

    /* Colon split loop: advance to next segment if any */
    if (colon_sep) {
        const char *next = colon_sep + 1;
        while (next < line_end && (*next==' '||*next=='	')) next++;
        seg_start = next;
        seg_end   = line_end;
        colon_sep = asm_colon_find_sep(seg_start, seg_end);
        if (colon_sep) {
            seg_end = colon_sep;
            while (seg_end > seg_start && (seg_end[-1]==' '||seg_end[-1]=='	')) seg_end--;
        }
        if (seg_start < line_end) goto next_colon_seg;
    }
    goto end_colon_loop;
    next_colon_seg: ;
    } while (true); /* end colon-split loop */
    end_colon_loop: ;

    p = (*line_end == '\n') ? line_end + 1 : line_end;
    line_no++;
  }
  bool ok = !diags_has_errors(diags);
  return ok;
}

// returns 0 if not a directive/unknown
static unsigned data_item_width_for_dir(sv_t mnem, long long num, bool *ok) {
  *ok = true;
  bool is_byte_dir =
      sv_ieq(mnem, "equb") || sv_ieq(mnem, "eqb") || sv_ieq(mnem, "db");
  bool is_word_dir = sv_ieq(mnem, "equw") || sv_ieq(mnem, "eqw") ||
                     sv_ieq(mnem, "dq") || sv_ieq(mnem, ".word");
  bool is_dword_dir = sv_ieq(mnem, "equd") || sv_ieq(mnem, "eqd") ||
                      sv_ieq(mnem, "dd") || sv_ieq(mnem, ".dword");
  bool is_special_byte = sv_ieq(mnem, ".byte");
  if (!is_byte_dir && !is_word_dir && !is_dword_dir && !is_special_byte)
    return 0;
  if (num < 0) {
    *ok = false;
    return 0;
  }
  if (is_byte_dir)
    return (num <= 0xFF) ? 1u : (*ok = false, 0u);
  if (is_word_dir)
    return (num <= 0xFFFF) ? 2u : (*ok = false, 0u);
  if (is_dword_dir)
    return (num <= 0xFFFFFFFFLL) ? 4u : (*ok = false, 0u);
  // .byte autosize
  return smallest_width_u32(num);
}

static bool dir_split_items(sv_t operand, sv_t *items, unsigned *item_cols,
                            unsigned *count, unsigned max_items,
                            unsigned base_col) {
  *count = 0;
  const char *s = operand.p;
  const char *e = operand.p + operand.len;
  const char *item_start = s;
  while (item_start < e) {
    const char *q = item_start;
    int paren = 0;
    bool in_str = false;
    while (q < e) {
      char cch = *q;
      if (cch == '"')
        in_str = !in_str;
      else if (!in_str) {
        if (cch == '(')
          paren++;
        else if (cch == ')' && paren > 0)
          paren--;
        else if (cch == ',' && paren == 0)
          break;
      }
      q++;
    }
    const char *item_end = q;
    // trim
    while (item_start < item_end &&
           (*item_start == ' ' || *item_start == '\t' || *item_start == '\r'))
      item_start++;
    while (
        item_end > item_start &&
        (item_end[-1] == ' ' || item_end[-1] == '\t' || item_end[-1] == '\r'))
      item_end--;

    if (*count < max_items) {
      items[*count] = (sv_t){item_start, (size_t)(item_end - item_start)};
      item_cols[*count] = base_col + (unsigned)(item_start - s);
    }
    (*count)++;
    if (q >= e)
      break;
    q++;
    item_start = q;
  }
  return true;
}

/* Full two-pass assembler: resolves all labels and emits machine code.
 *
 * Pass 1: scan for labels and directives, build the symbol table and
 *         compute PC for every instruction.
 * Pass 2: emit opcodes and operands into mem_out using the resolved table.
 *
 * in:  file              — source file path (for diagnostics and listings)
 *      preprocessed_text — output of preproc_run
 *      settings          — listing options, output path, CPU selection, etc.
 *      start_expr/end_expr — optional expressions for the output byte range
 *      mem_out[65536]    — 64KB output image (must be pre-zeroed by caller)
 *      written_out[65536] — set to 1 for each emitted byte (may be NULL)
 * out: *min_written, *max_written — address range actually written
 *      *out_start, *out_end       — final output range after start/end exprs
 *      *diags                     — errors and warnings
 * ret: false if any errors were emitted */
bool asm_assemble(const char *file, const char *text,
                  const asm_settings_t *settings, const char *start_expr,
                  const char *end_expr, uint8_t *mem_out, uint8_t *written_out,
                  uint8_t *written_explicit,
                  uint16_t *min_written, uint16_t *max_written,
                  uint16_t *out_start, uint16_t *out_end, diags_t *diags) {
  if (!file)
    file = "<input>";
  if (!text)
    return true;
  if (!mem_out)
    return false;
  memset(mem_out, 0, 65536);
  if (written_out)
    memset(written_out, 0, 65536);
  if (written_explicit)
    memset(written_explicit, 0, 65536);
  if (min_written && max_written) {
    *min_written = 0xFFFF;
    *max_written = 0;
  }
  if (out_start)
    *out_start = 0x0000;
  if (out_end)
    *out_end = 0xFFFF;

  line_rec_t *lines = NULL;
  size_t line_count = 0;
  strpool_t file_pool = {0};
  if (!parse_lines(file, text, &lines, &line_count, diags,
                   settings->allow_undoc, &file_pool)) {
    free(lines);
    strpool_free(&file_pool);
    return false;
  }

  // Iterative relaxation to allow ZP vs ABS selection.
  // We keep a small per-line cached addressing mode for ambiguous non-indexed
  // and indexed forms.
  addrmode_t *cached_am = (addrmode_t *)calloc(line_count, sizeof(addrmode_t));
  if (!cached_am) {
    diag_addf(diags, DIAG_ERROR, file, 1, 1, 1, "out of memory");
    free(lines);
    return false;
  }

  symtab_t syms;

  // Current .proc scope name. Empty string means not inside a proc.
  char proc_name[128] = {0};
  // .scope stack (up to 8 deep). Each entry is the scope name (may be empty for
  // anonymous scopes).
  char scope_stack[8][128];
  int scope_depth = 0;
  memset(scope_stack, 0, sizeof(scope_stack));
  // Most recently seen global (non-@) label, used as parent for @ local labels.
  char parent_label[128] = {0};

  uint16_t *anon_addrs = NULL;
  size_t anon_count = 0;
  size_t anon_cap = 0;

  bool changed = false;
  for (int iter = 0; iter < 5; iter++) {
    changed = false;
    symtab_init(&syms);
    proc_name[0] = 0; // reset proc scope at start of each pass
    scope_depth = 0;  // reset .scope stack
    memset(scope_stack, 0, sizeof(scope_stack));
    parent_label[0] = 0; // reset parent label tracking
    bool longa = true;   /* 65816: accumulator 8-bit by default */
    bool longi = true;   /* 65816: index regs 8-bit by default */

    // Built-in/reserved symbol so source can query the current CPU:
    //   .if cpu == 6502
    // Convention: 6502 => 6502, 65C02 => 6503.
    // (Using a numeric value keeps it usable in expressions.)
    const uint32_t cpu_sym =
        (settings && settings->cpu == ASM_CPU_65816)   ? 65816
        : (settings && settings->cpu == ASM_CPU_65C02) ? 6503
                                                       : 6502;
    (void)symtab_put(&syms, sv_from_cstr("cpu"), cpu_sym);
    uint16_t pc = 0;

    // rebuild anonymous label table for this iteration
    anon_count = 0;
    size_t anon_here = 0;

    // Pass1: labels and PC layout based on cached_am where relevant
    for (size_t i = 0; i < line_count; i++) {
      line_rec_t *r = &lines[i];
      const char *curf = r->file ? r->file : file;
      g_ow_cur_index = i;

      if (r->anon_label_def) {
        // record anonymous label address at current PC
        if (anon_count == anon_cap) {
          size_t nc = anon_cap ? (anon_cap * 2) : 32;
          uint16_t *na = (uint16_t *)realloc(anon_addrs, nc * sizeof(uint16_t));
          if (!na) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no,
                      r->anon_label_col ? r->anon_label_col : 1,
                      r->anon_label_len ? r->anon_label_len : 1,
                      "out of memory");
            break;
          }
          anon_addrs = na;
          anon_cap = nc;
        }
        anon_addrs[anon_count++] = pc;
        anon_here = anon_count;
      }

      if (r->label.len && r->kind != LK_EQU) {
        sv_t k = r->label;
        if (k.len && k.p[0] == '.') {
          k.p++;
          k.len--;
        } // <--- ADD THIS

        // Determine the parent for scoping: proc_name takes priority, else
        // parent_label.
        const char *scope =
            proc_name[0] ? proc_name
                         : (scope_depth > 0 ? scope_stack[scope_depth - 1]
                                            : parent_label);

        // @ labels are local to the nearest enclosing global label or proc.
        // Store as "ParentName::@LabelName".
        // Non-@ labels that are global update parent_label.
        char scoped_label[256];
        if (k.len && k.p[0] == '@') {
          // @ local label
          if (!scope[0]) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no,
                      r->label_col ? r->label_col : 1,
                      (unsigned)r->label.len ? (unsigned)r->label.len : 1,
                      "'@' label '%.*s' has no enclosing global label",
                      (int)k.len, k.p);
          } else {
            size_t plen = strlen(scope);
            if (plen + 2 + k.len < sizeof(scoped_label)) {
              memcpy(scoped_label, scope, plen);
              scoped_label[plen] = ':';
              scoped_label[plen + 1] = ':';
              memcpy(scoped_label + plen + 2, k.p, k.len);
              scoped_label[plen + 2 + k.len] = 0;
              k.p = scoped_label;
              k.len = plen + 2 + k.len;
            }
          }
        } else {
          // Global label: update parent_label (unless it's a .proc-scoped
          // label)
          if (!proc_name[0] && k.len && k.len < sizeof(parent_label)) {
            memcpy(parent_label, k.p, k.len);
            parent_label[k.len] = 0;
          }
          // If inside a .proc, store label as "ProcName::LabelName"
          if (proc_name[0] && k.len) {
            size_t plen = strlen(proc_name);
            if (plen + 2 + k.len < sizeof(scoped_label)) {
              memcpy(scoped_label, proc_name, plen);
              scoped_label[plen] = ':';
              scoped_label[plen + 1] = ':';
              memcpy(scoped_label + plen + 2, k.p, k.len);
              scoped_label[plen + 2 + k.len] = 0;
              k.p = scoped_label;
              k.len = plen + 2 + k.len;
            }
          }
        }

        int32_t prev = 0;
        if (symtab_get(&syms, k, &prev)) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no,
                    r->label_col ? r->label_col : 1,
                    (unsigned)r->label.len ? (unsigned)r->label.len : 1,
                    "duplicate label '%.*s'", (int)k.len, k.p);
        } else if (!symtab_put(&syms, k, pc)) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no,
                    r->label_col ? r->label_col : 1, (unsigned)r->label.len,
                    "out of memory storing label");
          symtab_free(&syms);
          free(cached_am);
          free(lines);
          return false;
        }
      }

      // .proc / .endproc handling (pass1)
      if (r->kind == LK_PROC) {
        if (proc_name[0]) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                    (unsigned)r->mnem.len,
                    ".proc cannot be nested (already inside '.proc %s')",
                    proc_name);
        } else if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    ".proc requires a name");
        } else if (r->operand.len >= sizeof(proc_name)) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                    (unsigned)r->operand.len, ".proc name too long");
        } else {
          memcpy(proc_name, r->operand.p, r->operand.len);
          proc_name[r->operand.len] = 0;
          // Also register the proc name itself as a label at the current PC
          // so that  jsr FooProc  works.
          sv_t pname = {proc_name, r->operand.len};
          int32_t prev = 0;
          if (symtab_get(&syms, pname, &prev)) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                      (unsigned)r->operand.len, "duplicate label '%.*s'",
                      (int)r->operand.len, r->operand.p);
          } else {
            symtab_put(&syms, pname, pc);
          }
        }
        continue;
      }

      if (r->kind == LK_ENDPROC) {
        if (!proc_name[0]) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                    (unsigned)r->mnem.len, ".endproc without matching .proc");
        } else {
          proc_name[0] = 0;
        }
        continue;
      }

      // .longa/.longi pass1
      if (r->kind == LK_LONGA || r->kind == LK_LONGI) {
        sv_t arg = sv_trim(r->operand);
        const char *dname = (r->kind == LK_LONGA) ? ".longa" : ".longi";
        bool is_on = sv_ieq(arg, "on") || (arg.len == 1 && arg.p[0] == '1');
        bool is_off = sv_ieq(arg, "off") || (arg.len == 1 && arg.p[0] == '0');
        if (!is_on && !is_off) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                    arg.len ? (unsigned)arg.len : 1,
                    "%s requires 'on' or 'off'", dname);
        } else {
          if (r->kind == LK_LONGA)
            longa = is_on ? false : true;
          else
            longi = is_on ? false : true;
        }
        continue;
      }

      // .incbin pass1: determine emit size by reading file
      if (r->kind == LK_INCBIN) {
        // operand: "filename" [, offset [, length]]
        sv_t items[3];
        unsigned icols[3];
        unsigned icnt = 0;
        dir_split_items(r->operand, items, icols, &icnt, 3, r->op_col);
        if (icnt < 1 || items[0].len < 2) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    ".incbin requires a quoted filename");
        } else {
          // strip quotes
          sv_t fn = items[0];
          if (fn.p[0] == '"') {
            fn.p++;
            fn.len -= 2;
          }
          char fnbuf[512];
          size_t fnlen =
              fn.len < sizeof(fnbuf) - 1 ? fn.len : sizeof(fnbuf) - 1;
          memcpy(fnbuf, fn.p, fnlen);
          fnbuf[fnlen] = 0;
          FILE *ibf = fopen(fnbuf, "rb");
          if (!ibf) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, icols[0],
                      (unsigned)items[0].len, "cannot open incbin file '%s'",
                      fnbuf);
          } else {
            fseek(ibf, 0, SEEK_END);
            long fsz = ftell(ibf);
            fclose(ibf);
            if (fsz >= 0) {
              long off = 0, len = fsz;
              if (icnt >= 2 && items[1].len > 0) {
                expr_val_t vo = {0};
                bool unro = false;
                if (expr_eval(curf, r->line_no, icols[1], items[1], &syms,
                              anon_addrs, anon_count, anon_here, false, &unro,
                              &vo, diags, proc_name, parent_label))
                  off = (long)vo.num;
                expr_val_free(&vo);
              }
              if (icnt >= 3 && items[2].len > 0) {
                expr_val_t vl = {0};
                bool unrl = false;
                if (expr_eval(curf, r->line_no, icols[2], items[2], &syms,
                              anon_addrs, anon_count, anon_here, false, &unrl,
                              &vl, diags, proc_name, parent_label))
                  len = (long)vl.num;
                expr_val_free(&vl);
              } else {
                len = fsz - off;
              }
              if (off < 0 || off > fsz)
                off = 0;
              if (len < 0 || off + len > fsz)
                len = fsz - off;
              pc += (uint16_t)len;
            }
          }
        }
        continue;
      }

      // .scope / .endscope handling (pass1)
      if (r->kind == LK_SCOPE) {
        if (scope_depth >= 8) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                    (unsigned)r->mnem.len, ".scope nested too deep (max 8)");
        } else {
          // Optional name; anonymous scopes get an empty string.
          if (r->operand.len > 0 && r->operand.len < sizeof(scope_stack[0])) {
            memcpy(scope_stack[scope_depth], r->operand.p, r->operand.len);
            scope_stack[scope_depth][r->operand.len] = 0;
          } else {
            scope_stack[scope_depth][0] = 0;
          }
          scope_depth++;
        }
        continue;
      }
      if (r->kind == LK_ENDSCOPE) {
        if (scope_depth == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                    (unsigned)r->mnem.len, ".endscope without matching .scope");
        } else {
          scope_depth--;
          scope_stack[scope_depth][0] = 0;
        }
        continue;
      }

      if (r->kind == LK_PC_SET) {
        if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    "missing address expression");
          continue;
        }
        expr_val_t v = {0};
        bool unr = false;
        if (expr_eval(curf, r->line_no, r->op_col, r->operand, &syms,
                      anon_addrs, anon_count, anon_here, true, &unr, &v, diags,
                      proc_name, parent_label)) {
          if (v.kind != EXPR_VAL_NUM || !in_u16(v.num)) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                      (unsigned)r->operand.len,
                      "address expression must be 0..65535");
          } else {
            pc = (uint16_t)v.num;
          }
        }
        expr_val_free(&v);
        continue;
      }

      if (r->kind == LK_EQU) {
        if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    "missing EQU expression");
          continue;
        }
        // Define constant symbol. Dot is allowed in definition; strip it for
        // storage.
        sv_t k = r->label;
        if (k.len && k.p[0] == '.') {
          k.p++;
          k.len--;
        }
        // Evaluate with allow_undef during pass1 for layout sizing.
        expr_val_t v = {0};
        bool unr = false;
        if (expr_eval(curf, r->line_no, r->op_col, r->operand, &syms,
                      anon_addrs, anon_count, anon_here, true, &unr, &v, diags,
                      proc_name, parent_label)) {
          if (v.kind != EXPR_VAL_NUM || v.num < 0 || v.num > 0xFFFFFF) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                      (unsigned)r->operand.len,
                      "EQU expression must be 0..$FFFFFF");
          } else {
            // Reject collisions with labels already defined in this pass.
            int32_t prev = 0;
            if (symtab_get(&syms, k, &prev) && prev != (int32_t)v.num) {
              // Note: this also fires if redefining an EQU with a different
              // value.
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->label_col,
                        (unsigned)r->label.len, "symbol '%.*s' redefined",
                        (int)r->label.len, r->label.p);
            } else {
              symtab_put(&syms, k, (int32_t)v.num);
            }
          }
        }
        expr_val_free(&v);
        continue;
      }

      if (r->kind == LK_OPT) {
        // OPT affects listing only; it does not affect layout.
        continue;
      }

      if (r->kind == LK_DIAG) {
        continue;
      }
      if (r->kind == LK_DIAG) {
        r->emit_len = 0;
        continue;
      }

      if (r->kind == LK_DATA) {
        if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    "missing data operand");
          continue;
        }
        bool is_equ_s = sv_ieq(r->mnem, "equs");
        sv_t items[128];
        unsigned cols[128];
        unsigned cnt = 0;
        dir_split_items(r->operand, items, cols, &cnt, 128, r->op_col);
        for (unsigned k = 0; k < cnt && k < 128; k++) {
          if (items[k].len == 0) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[k], 1,
                      "empty item in data list");
            continue;
          }
          expr_val_t v = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, cols[k], items[k], &syms, anon_addrs,
                        anon_count, anon_here, true, &unr, &v, diags, proc_name,
                        parent_label)) {
            if (v.kind == EXPR_VAL_STR) {
              if (!sv_ieq(r->mnem, ".byte") && !is_equ_s) {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[k],
                          (unsigned)items[k].len,
                          "string not allowed in this directive (use .byte for "
                          "mixed data)");
              } else {
                pc = (uint16_t)(pc + (uint16_t)v.str_len);
              }
            } else {
              if (is_equ_s) {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[k],
                          (unsigned)items[k].len,
                          "equs requires a string expression");
                expr_val_free(&v);
                continue;
              }
              bool ok;
              unsigned w = 0;
              if (unr) {
                // Forward reference in pass2 layout — assume minimum fixed
                // width. The encode pass (pass2 emit) resolves all labels so
                // unr should not occur there; if it does the value will be 0
                // which is valid.
                ok = true;
                if (sv_ieq(r->mnem, "equb") || sv_ieq(r->mnem, "eqb") ||
                    sv_ieq(r->mnem, "db"))
                  w = 1u;
                else if (sv_ieq(r->mnem, "equw") || sv_ieq(r->mnem, "eqw") ||
                         sv_ieq(r->mnem, "dq") || sv_ieq(r->mnem, ".word"))
                  w = 2u;
                else if (sv_ieq(r->mnem, "equd") || sv_ieq(r->mnem, "eqd") ||
                         sv_ieq(r->mnem, "dd") || sv_ieq(r->mnem, ".dword"))
                  w = 4u;
                else
                  w = 4u; // .byte worst-case
              } else {
                w = data_item_width_for_dir(r->mnem, v.num, &ok);
              }
              if (!ok || w == 0) {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[k],
                          (unsigned)items[k].len,
                          "numeric value out of range for directive");
              } else {
                pc = (uint16_t)(pc + (uint16_t)w);
              }
            }
          }
          expr_val_free(&v);
        }
        continue;
      }

      if (r->kind == LK_ALIGN) {
        // .align N  — advance PC to next multiple of N
        if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    ".align requires a power-of-2 operand");
        } else {
          expr_val_t v = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, r->op_col, r->operand, &syms,
                        anon_addrs, anon_count, anon_here, true, &unr, &v,
                        diags, proc_name, parent_label)) {
            if (!unr) {
              uint16_t n = (uint16_t)v.num;
              if (n < 2 || (n & (n - 1)) != 0) {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                          (unsigned)r->operand.len,
                          ".align value must be a power of 2 (2, 4, 8, ...)");
              } else {
                uint16_t pad = (uint16_t)((n - (pc & (n - 1))) & (n - 1));
                pc = (uint16_t)(pc + pad);
              }
            } else {
              // unresolved in pass1 — worst-case pad (leave room)
              // We'll recompute properly in pass2 via relaxation
            }
          }
          expr_val_free(&v);
        }
        continue;
      }

      if (r->kind == LK_RES) {
        // .res N [, fill]  — reserve N bytes
        if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    ".res requires a byte count");
        } else {
          // Split on comma to get count and optional fill value
          sv_t items[2];
          unsigned cols2[2];
          unsigned cnt2 = 0;
          dir_split_items(r->operand, items, cols2, &cnt2, 2, r->op_col);
          expr_val_t v = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, cols2[0], items[0], &syms, anon_addrs,
                        anon_count, anon_here, true, &unr, &v, diags, proc_name,
                        parent_label)) {
            if (!unr) {
              uint16_t count = (uint16_t)v.num;
              pc = (uint16_t)(pc + count);
            }
          }
          expr_val_free(&v);
        }
        continue;
      }

      if (r->kind == LK_INSTR) {
        // Determine addressing mode and size. Use cached_am for ambiguous
        // ABS/ZP forms.
        operand_pat_t pat;
        if (!parse_operand_pattern(file, r->line_no, r->op_col, r->operand,
                                   r->mnem, &pat, diags)) {
          continue;
        }

        addrmode_t am = pat.am;

        // Handle indirect X as JMP (abs,X) on 65c02
        if (pat.is_ind_x && sv_ieq(r->mnem, "JMP")) {
          am = AM_ABSINDX;
        }
        // (expr) : JMP uses AM_IND (abs indirect), all others use AM_ZPIND
        if (pat.is_indirect && am == AM_IND && !sv_ieq(r->mnem, "JMP")) {
          am = AM_ZPIND;
        }
        // 65816: JML [abs] uses AM_ABSL; JML $AABBCC uses AM_LONG
        if (sv_ieq(r->mnem, "JML")) {
          if (pat.is_ind_long)
            am = AM_ABSL;
          else if (am == AM_ABS)
            am = AM_LONG;
        }
        // 65816: JSL always AM_LONG (already set in parser, but ensure here)
        if (sv_ieq(r->mnem, "JSL") && am == AM_ABS)
          am = AM_LONG;

        // For non-indirect ABS/ABSX/ABSY, allow ZP relax based on cached
        if (am == AM_ABS || am == AM_ABSX || am == AM_ABSY) {
          if (cached_am[i] == AM_ZP)
            am = AM_ZP;
          if (cached_am[i] == AM_ZPX)
            am = AM_ZPX;
          if (cached_am[i] == AM_ZPY)
            am = AM_ZPY;
        }

        // Lookup opcode/size
        const op_t *op =
            op_find(r->mnem, am, settings->cpu, settings->allow_undoc);
        if (!op) {
          // If requested ZP but no ZP opcode, fall back to ABS for sizing in
          // pass1.
          if (am == AM_ZP)
            op = op_find(r->mnem, AM_ABS, settings->cpu, settings->allow_undoc);
          if (am == AM_ZPX)
            op =
                op_find(r->mnem, AM_ABSX, settings->cpu, settings->allow_undoc);
          if (am == AM_ZPY)
            op =
                op_find(r->mnem, AM_ABSY, settings->cpu, settings->allow_undoc);
        }
        if (!op) {
          {
            uint8_t docmask = 0;
            bool has_undoc = false;
            mnem_collect_presence(r->mnem, &docmask, &has_undoc);
            uint8_t sel = (settings->cpu == ASM_CPU_65816)   ? CPU_65816
                          : (settings->cpu == ASM_CPU_65C02) ? CPU_65C02
                                                             : CPU_6502;

            if (docmask & sel) {
              // Mnemonic exists on current CPU but addressing mode unsupported.
              // Special case: 65816 in 8-bit accumulator/index mode with a
              // wide immediate — suggest REP to widen the register.
              bool rep_hint = false;
              if (settings && settings->cpu == ASM_CPU_65816 && am == AM_IMM) {
                char mn_rep[8] = {0};
                sv_to_upper3(r->mnem, mn_rep);
                bool is_idx =
                    (strcmp(mn_rep, "CPX") == 0 || strcmp(mn_rep, "CPY") == 0 ||
                     strcmp(mn_rep, "LDX") == 0 || strcmp(mn_rep, "LDY") == 0);
                /* If the operand text contains a value that would need 16-bit,
                   we can infer the user may have forgotten REP. */
                bool cur8 = is_idx ? longi : longa;
                if (cur8)
                  rep_hint = true;
              }
              if (rep_hint) {
                char mn_rep[8] = {0};
                sv_to_upper3(r->mnem, mn_rep);
                bool is_idx =
                    (strcmp(mn_rep, "CPX") == 0 || strcmp(mn_rep, "CPY") == 0 ||
                     strcmp(mn_rep, "LDX") == 0 || strcmp(mn_rep, "LDY") == 0);
                const char *repbits = is_idx ? "#$10" : "#$20";
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "operand out of range for 8-bit mode — "
                          "did you forget REP %s to enable 16-bit %s?",
                          repbits, is_idx ? "index registers" : "accumulator");
              } else {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "unsupported addressing mode for instruction");
              }
            } else if (docmask) {
              // Mnemonic exists but not on this CPU — give a targeted hint.
              if (settings->cpu != ASM_CPU_65816 && (docmask & CPU_65816) &&
                  !(docmask & (CPU_6502 | CPU_65C02))) {
                // Pure 65816-only instruction used on 6502/65C02
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "instruction not available for selected CPU "
                          "(try --cpu 65816 or add .cpu 65816)");
              } else if (settings->cpu == ASM_CPU_6502 &&
                         (docmask & CPU_65C02)) {
                // 65C02 instruction used on 6502
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "instruction not available for selected CPU "
                          "(try --cpu 65c02 or add .cpu 65c02)");
              } else {
                // Generic fallback
                const char *hint = (settings->cpu == ASM_CPU_65C02)
                                       ? "--cpu 6502"
                                       : "--cpu 65c02";
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "instruction not available for selected CPU (try %s)",
                          hint);
              }
            } else if (has_undoc && !settings->allow_undoc) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                        (unsigned)r->mnem.len,
                        "undocumented opcode (enable --undoc)");
            } else {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                        (unsigned)r->mnem.len, "unknown instruction");
            }
          }
          continue;
        }
        // 65816 variable-width immediate
        // REP/SEP operand is ALWAYS 8-bit (processor status register byte)
        uint8_t extra65 = 0;
        if (settings && settings->cpu == ASM_CPU_65816 && am == AM_IMM) {
          char mn_p1[8] = {0};
          sv_to_upper3(r->mnem, mn_p1);
          bool always8 =
              (strcmp(mn_p1, "REP") == 0 || strcmp(mn_p1, "SEP") == 0 ||
               strcmp(mn_p1, "WDM") == 0);
          bool idx_m =
              (strcmp(mn_p1, "CPX") == 0 || strcmp(mn_p1, "CPY") == 0 ||
               strcmp(mn_p1, "LDX") == 0 || strcmp(mn_p1, "LDY") == 0);
          if (!always8) {
            if (idx_m) {
              if (!longi)
                extra65 = 1;
            } else {
              if (!longa)
                extra65 = 1;
            }
          }
        }
        pc = (uint16_t)(pc + op->bytes + extra65);

        // Track REP/SEP flag changes in pass1
        if (settings && settings->cpu == ASM_CPU_65816) {
          char mn_p1[8] = {0};
          sv_to_upper3(r->mnem, mn_p1);
          if ((strcmp(mn_p1, "REP") == 0 || strcmp(mn_p1, "SEP") == 0) &&
              r->operand.len > 0) {
            sv_t oper = sv_trim(r->operand);
            if (oper.len > 0 && oper.p[0] == '#') {
              oper.p++;
              oper.len--;
              char tmp[32];
              size_t tl = oper.len < 31 ? oper.len : 31;
              memcpy(tmp, oper.p, tl);
              tmp[tl] = 0;
              /* Parse $ or & prefix hex, or plain 0x/decimal */
              const char *sp = tmp;
              int base = 0;
              if (*sp == '$' || *sp == '&') {
                sp++;
                base = 16;
              }
              char *ep = NULL;
              long fv = strtol(sp, &ep, base);
              if (ep && ep != sp) {
                if (strcmp(mn_p1, "REP") == 0) {
                  if (fv & 0x20)
                    longa = false;
                  if (fv & 0x10)
                    longi = false;
                } else {
                  if (fv & 0x20)
                    longa = true;
                  if (fv & 0x10)
                    longi = true;
                }
              }
            }
          }
        }
      }
    }

    if (diags_has_errors(diags)) {
      symtab_free(&syms);
      free(cached_am);
      free(anon_addrs);
      free(lines);
      return false;
    }

    // Pass2: emit

    // Setup overwrite detection (byte-level) for pass2
    uint16_t *ow_owner = NULL;
    uint8_t *ow_firstval = NULL;
    if (settings && settings->overwrite_warn) {
      ow_owner = (uint16_t *)malloc(65536 * sizeof(uint16_t));
      ow_firstval = (uint8_t *)malloc(65536);
      if (!ow_owner || !ow_firstval) {
        free(ow_owner);
        free(ow_firstval);
        diag_addf(diags, DIAG_ERROR, file, 1, 1, 1, "out of memory");
        symtab_free(&syms);
        free(cached_am);
        free(anon_addrs);
        free(lines);
        return false;
      }
      // 0xFFFF = never written
      for (size_t k = 0; k < 65536; k++)
        ow_owner[k] = 0xFFFF;
      memset(ow_firstval, 0, 65536);

      g_ow_settings = settings;
      g_ow_file = file;
      g_ow_diags = diags;
      g_ow_lines = lines;
      g_ow_line_count = line_count;
      g_ow_owner = ow_owner;
      g_ow_firstval = ow_firstval;
      g_ow_warn_count = 0;
      g_ow_warn_suppressed = false;
    } else {
      g_ow_owner = NULL;
      g_ow_firstval = NULL;
    }
    /* Initialise guard range state for this assembly run. */
    guards_reset();
    g_guard_enabled    = settings && settings->guard_warn;
    g_guard_diags      = diags;
    g_guard_file       = file;
    g_guard_cur_index  = 0;
    g_guard_lines      = lines;
    g_guard_line_count = line_count;
    g_written_explicit = written_explicit;
    g_emit_is_res_fill = false;
    g_pc_was_set       = false;
    pc = 0;
    anon_here = 0;
    proc_name[0] = 0;    // reset proc scope for pass2
    parent_label[0] = 0; // reset parent label tracking for pass2
    bool longa2 = true;  /* 65816 M flag for pass2 */
    bool longi2 = true;  /* 65816 X flag for pass2 */
    g_ow_blame_active = false;
    g_ow_blame_index = 0;
    g_ow_run_active = false;
    uint8_t cur_opt = 1; // BBC default
    for (size_t i = 0; i < line_count; i++) {
      line_rec_t *r = &lines[i];
      const char *curf = r->file ? r->file : file;
      if (g_ow_owner)
        g_ow_file = curf;
      // Flush any pending coalesced overwrite run from the previous line.
      if (g_ow_owner && i > 0) {
        ow_flush_run();
      }
      g_ow_cur_index = i;
      g_guard_cur_index = i;  /* always track current line for guard diagnostics */

      if (r->anon_label_def) {
        // Anonymous label definition does not emit bytes; it marks this PC.
        anon_here++;
      }

      uint16_t pc_before = pc;
      r->pc = pc;
      r->emit_len = 0;
      r->opt_level = cur_opt;

      // Track parent_label for @ local label resolution in pass2.
      // A global (non-@) label seen here updates parent_label.
      if (r->label.len && r->kind != LK_EQU && !proc_name[0]) {
        sv_t k = r->label;
        if (k.len && k.p[0] == '.') {
          k.p++;
          k.len--;
        }
        if (k.len && k.p[0] != '@' && k.len < sizeof(parent_label)) {
          memcpy(parent_label, k.p, k.len);
          parent_label[k.len] = 0;
        }
      }

      if (r->kind == LK_PROC) {
        if (r->operand.len > 0 && r->operand.len < sizeof(proc_name)) {
          memcpy(proc_name, r->operand.p, r->operand.len);
          proc_name[r->operand.len] = 0;
          // proc name also acts as parent for @ labels inside it
          memcpy(parent_label, proc_name, r->operand.len);
          parent_label[r->operand.len] = 0;
          // Re-register proc name label at this PC (pass2 is authoritative)
          sv_t pname = {proc_name, r->operand.len};
          symtab_put(&syms, pname, pc);
        }
        r->emit_len = 0;
        continue;
      }

      if (r->kind == LK_ENDPROC) {
        proc_name[0] = 0;
        // parent_label persists after endproc so @ labels after it still work
        r->emit_len = 0;
        continue;
      }

      // .scope / .endscope (pass2)
      if (r->kind == LK_SCOPE) {
        if (scope_depth < 8) {
          if (r->operand.len > 0 && r->operand.len < sizeof(scope_stack[0])) {
            memcpy(scope_stack[scope_depth], r->operand.p, r->operand.len);
            scope_stack[scope_depth][r->operand.len] = 0;
            // Named scope: also update parent_label so @ locals resolve
            // correctly
            memcpy(parent_label, scope_stack[scope_depth], r->operand.len + 1);
          } else {
            scope_stack[scope_depth][0] = 0;
          }
          scope_depth++;
        }
        r->emit_len = 0;
        continue;
      }
      if (r->kind == LK_ENDSCOPE) {
        if (scope_depth > 0) {
          scope_depth--;
          scope_stack[scope_depth][0] = 0;
        }
        r->emit_len = 0;
        continue;
      }

      if (r->kind == LK_PC_SET) {
        // If we were in the middle of an overwrite run, close it before
        // changing PC.
        if (g_ow_owner)
          ow_flush_run();
        expr_val_t v = {0};
        bool unr = false;
        if (expr_eval(curf, r->line_no, r->op_col, r->operand, &syms,
                      anon_addrs, anon_count, anon_here, false, &unr, &v, diags,
                      proc_name, parent_label)) {
          if (v.kind == EXPR_VAL_NUM && in_u16(v.num)) {
            pc = (uint16_t)v.num;
            g_pc_was_set = true; /* explicit .org seen — subsequent bytes are intentional */
          }
        }
        expr_val_free(&v);

        // For BBC-style listings, show the *new* PC on ORG/PC-set directive
        // lines. These directives don't emit bytes, so this does not affect
        // codegen.
        r->pc = pc;

        // If PC has been set into an already-written area, blame this directive
        // for subsequent overwrites.
        if (g_ow_owner && g_ow_owner[pc] != 0xFFFF) {
          g_ow_blame_active = true;
          g_ow_blame_index = i;
        } else {
          g_ow_blame_active = false;
        }
        continue;
      }

      if (r->kind == LK_OPT) {
        if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    "missing OPT value (use OPT 0/1/2)");
          continue;
        }
        expr_val_t v = {0};
        bool unr = false;
        if (expr_eval(curf, r->line_no, r->op_col, r->operand, &syms,
                      anon_addrs, anon_count, anon_here, false, &unr, &v, diags,
                      proc_name, parent_label)) {
          if (v.kind != EXPR_VAL_NUM || v.num < 0 || v.num > 2) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                      (unsigned)r->operand.len, "OPT value must be 0, 1, or 2");
          } else {
            cur_opt = (uint8_t)v.num;
          }
        }
        expr_val_free(&v);
        r->emit_len = 0;
        continue;
      }
      if (r->kind == LK_EQU) {
        // Re-evaluate with allow_undef=false so undefined symbols in EQU are
        // errors.
        if (r->operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                    "missing EQU expression");
          continue;
        }
        sv_t k = r->label;
        if (k.len && k.p[0] == '.') {
          k.p++;
          k.len--;
        }
        expr_val_t v = {0};
        bool unr = false;
        if (expr_eval(curf, r->line_no, r->op_col, r->operand, &syms,
                      anon_addrs, anon_count, anon_here, false, &unr, &v, diags,
                      proc_name, parent_label)) {
          if (v.kind != EXPR_VAL_NUM || v.num < 0 || v.num > 0xFFFFFF) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                      (unsigned)r->operand.len,
                      "EQU expression must be 0..$FFFFFF");
          } else {
            symtab_put(&syms, k, (int32_t)v.num);
          }
        }
        expr_val_free(&v);
        r->emit_len = 0;
        continue;
      }
      if (r->kind == LK_DATA) {
        bool is_equ_s = sv_ieq(r->mnem, "equs");
        sv_t items[256];
        unsigned cols[256];
        unsigned cnt = 0;
        dir_split_items(r->operand, items, cols, &cnt, 256, r->op_col);
        for (unsigned k = 0; k < cnt && k < 256; k++) {
          sv_t item = items[k];
          if (item.len == 0)
            continue;
          expr_val_t v = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, cols[k], item, &syms, anon_addrs,
                        anon_count, anon_here, false, &unr, &v, diags,
                        proc_name, parent_label)) {
            if (v.kind == EXPR_VAL_STR) {
              if (!(sv_ieq(r->mnem, ".byte") || is_equ_s)) {
                // already diagnosed in pass1
              } else {
                for (size_t si = 0; si < v.str_len; si++) {
                  emit_u8(mem_out, written_out, min_written, max_written, pc,
                          (uint8_t)v.str[si]);
                  pc++;
                }
              }
            } else {
              if (is_equ_s) {
                // already diagnosed in pass1
                expr_val_free(&v);
                continue;
              }
              bool ok;
              unsigned w = data_item_width_for_dir(r->mnem, v.num, &ok);
              if (ok && w) {
                if (w == 1) {
                  emit_u8(mem_out, written_out, min_written, max_written, pc,
                          (uint8_t)(v.num & 0xFF));
                  pc++;
                } else if (w == 2) {
                  emit_le16(mem_out, written_out, min_written, max_written, pc,
                            (uint16_t)(v.num & 0xFFFF));
                  pc += 2;
                } else if (w == 4) {
                  emit_le32(mem_out, written_out, min_written, max_written, pc,
                            (uint32_t)(v.num & 0xFFFFFFFFu));
                  pc += 4;
                }
              }
            }
          }
          expr_val_free(&v);
        }
        r->emit_len = (uint16_t)(pc - pc_before);
        continue;
      }
      if (r->kind == LK_ALIGN) {
        if (r->operand.len > 0) {
          expr_val_t v = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, r->op_col, r->operand, &syms,
                        anon_addrs, anon_count, anon_here, false, &unr, &v,
                        diags, proc_name, parent_label)) {
            uint16_t n = (uint16_t)v.num;
            if (n >= 2 && (n & (n - 1)) == 0) {
              uint16_t pad = (uint16_t)((n - (pc & (n - 1))) & (n - 1));
              for (uint16_t i = 0; i < pad; i++) {
                emit_u8(mem_out, written_out, min_written, max_written, pc,
                        0x00);
                pc++;
              }
            }
          }
          expr_val_free(&v);
        }
        r->emit_len = (uint16_t)(pc - pc_before);
        continue;
      }

      if (r->kind == LK_RES) {
        if (r->operand.len > 0) {
          sv_t items[2];
          unsigned cols2[2];
          unsigned cnt2 = 0;
          dir_split_items(r->operand, items, cols2, &cnt2, 2, r->op_col);
          expr_val_t vc = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, cols2[0], items[0], &syms, anon_addrs,
                        anon_count, anon_here, false, &unr, &vc, diags,
                        proc_name, parent_label)) {
            uint16_t count = (uint16_t)vc.num;
            uint8_t fill = 0x00;
            if (cnt2 >= 2 && items[1].len > 0) {
              expr_val_t vf = {0};
              bool unr2 = false;
              if (expr_eval(curf, r->line_no, cols2[1], items[1], &syms,
                            anon_addrs, anon_count, anon_here, false, &unr2,
                            &vf, diags, proc_name, parent_label)) {
                fill = (uint8_t)(vf.num & 0xFF);
              }
              expr_val_free(&vf);
            }
            for (uint16_t i = 0; i < count; i++) {
              g_emit_is_res_fill = true;
              emit_u8(mem_out, written_out, min_written, max_written, pc, fill);
              g_emit_is_res_fill = false;
              pc++;
            }
          }
          expr_val_free(&vc);
        }
        r->emit_len = (uint16_t)(pc - pc_before);
        continue;
      }

      if (r->kind == LK_INCBIN) {
        sv_t items[3];
        unsigned icols[3];
        unsigned icnt = 0;
        dir_split_items(r->operand, items, icols, &icnt, 3, r->op_col);
        if (icnt >= 1 && items[0].len >= 2) {
          sv_t fn = items[0];
          if (fn.p[0] == '"') {
            fn.p++;
            fn.len -= 2;
          }
          char fnbuf[512];
          size_t fnlen =
              fn.len < sizeof(fnbuf) - 1 ? fn.len : sizeof(fnbuf) - 1;
          memcpy(fnbuf, fn.p, fnlen);
          fnbuf[fnlen] = 0;
          FILE *ibf = fopen(fnbuf, "rb");
          if (ibf) {
            fseek(ibf, 0, SEEK_END);
            long fsz = ftell(ibf);
            long off = 0, len = fsz;
            if (icnt >= 2 && items[1].len > 0) {
              expr_val_t vo = {0};
              bool unro = false;
              if (expr_eval(curf, r->line_no, icols[1], items[1], &syms,
                            anon_addrs, anon_count, anon_here, false, &unro,
                            &vo, diags, proc_name, parent_label))
                off = (long)vo.num;
              expr_val_free(&vo);
            }
            if (icnt >= 3 && items[2].len > 0) {
              expr_val_t vl = {0};
              bool unrl = false;
              if (expr_eval(curf, r->line_no, icols[2], items[2], &syms,
                            anon_addrs, anon_count, anon_here, false, &unrl,
                            &vl, diags, proc_name, parent_label))
                len = (long)vl.num;
              expr_val_free(&vl);
            } else {
              len = fsz - off;
            }
            if (off < 0 || off > fsz)
              off = 0;
            if (len < 0 || off + len > fsz)
              len = fsz - off;
            if (off > 0)
              fseek(ibf, off, SEEK_SET);
            else
              fseek(ibf, 0, SEEK_SET);
            for (long bi = 0; bi < len; bi++) {
              int ch = fgetc(ibf);
              if (ch == EOF)
                break;
              emit_u8(mem_out, written_out, min_written, max_written, pc,
                      (uint8_t)ch);
              pc++;
            }
            fclose(ibf);
          }
        }
        r->emit_len = (uint16_t)(pc - pc_before);
        continue;
      }

      if (r->kind == LK_INSTR) {
        operand_pat_t pat;
        if (!parse_operand_pattern(file, r->line_no, r->op_col, r->operand,
                                   r->mnem, &pat, diags)) {
          r->emit_len = (uint16_t)(pc - pc_before);
          continue;
        }
        addrmode_t am = pat.am;

        // 65816: JML [abs]=AM_ABSL; JML $AABBCC=AM_LONG; JSL=AM_LONG
        if (sv_ieq(r->mnem, "JML")) {
          if (pat.is_ind_long)
            am = AM_ABSL;
          else if (am == AM_ABS)
            am = AM_LONG;
        }
        if (sv_ieq(r->mnem, "JSL") && am == AM_ABS)
          am = AM_LONG;

        // For memory modes, evaluate expr to decide zp/abs
        long long val = 0;
        bool have_val = false;
        if (am == AM_ABS || am == AM_ABSX || am == AM_ABSY || am == AM_IND ||
            am == AM_INDX || am == AM_INDY) {
          if (pat.expr.len) {
            expr_val_t ev = {0};
            bool unr = false;
            if (expr_eval(curf, r->line_no, r->op_col, pat.expr, &syms,
                          anon_addrs, anon_count, anon_here, false, &unr, &ev,
                          diags, proc_name, parent_label)) {
              if (ev.kind == EXPR_VAL_NUM) {
                val = ev.num;
                have_val = true;
              } else {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                          (unsigned)r->operand.len,
                          "operand must be numeric here");
              }
            }
            expr_val_free(&ev);
          }
        }

        // Determine relax / special modes
        if (pat.is_ind_x && sv_ieq(r->mnem, "JMP")) {
          am = AM_ABSINDX;
        } else if (pat.is_indirect) {
          // (expr) : for JMP it's AM_IND (abs), otherwise 65c02 uses (zp) mode.
          if (!sv_ieq(r->mnem, "JMP")) {
            am = AM_ZPIND;
          }
        }

        if ((am == AM_ABS || am == AM_ABSX || am == AM_ABSY) && have_val) {
          if (val < 0 || val > 0xFFFF) {
            diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                      (unsigned)r->operand.len,
                      "address out of range (0..65535)");
            continue;
          }
          if (val <= 0xFF) {
            // Try ZP variant if opcode exists
            addrmode_t try_am = (am == AM_ABS)    ? AM_ZP
                                : (am == AM_ABSX) ? AM_ZPX
                                                  : AM_ZPY;
            if (op_find(r->mnem, try_am, settings->cpu,
                        settings->allow_undoc)) {
              if (cached_am[i] != try_am) {
                cached_am[i] = try_am;
                changed = true;
              }
              am = try_am;
            }
          }
        }

        if (am == AM_REL) {
          // compute offset from PC after opcode
          expr_val_t ev = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, r->op_col, pat.expr, &syms,
                        anon_addrs, anon_count, anon_here, false, &unr, &ev,
                        diags, proc_name, parent_label)) {
            if (ev.kind != EXPR_VAL_NUM || !in_u16(ev.num)) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                        (unsigned)r->operand.len,
                        "branch target must be 0..65535");
              expr_val_free(&ev);
              continue;
            }
            uint16_t target = (uint16_t)ev.num;
            int rel = (int)target - (int)(pc + 2);
            if (rel < -128 || rel > 127) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                        (unsigned)r->operand.len,
                        "branch out of range: from $%04X to $%04X (rel=%d). "
                        "Use JMP/BRA trampoline.",
                        (unsigned)pc, (unsigned)target, rel);
              expr_val_free(&ev);
              continue;
            }

            const op_t *op =
                op_find(r->mnem, AM_REL, settings->cpu, settings->allow_undoc);
            if (!op) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                        (unsigned)r->mnem.len,
                        "instruction not supported on this CPU");
              expr_val_free(&ev);
              continue;
            }
            emit_u8(mem_out, written_out, min_written, max_written, pc,
                    op->opcode);
            pc++;
            emit_u8(mem_out, written_out, min_written, max_written, pc,
                    (uint8_t)(rel & 0xFF));
            pc++;
          }
          expr_val_free(&ev);
          r->emit_len = (uint16_t)(pc - pc_before);
          continue;
        }

        const op_t *op =
            op_find(r->mnem, am, settings->cpu, settings->allow_undoc);
        if (!op) {
          // fallback if relax cached too aggressively
          if (am == AM_ZP)
            op = op_find(r->mnem, AM_ABS, settings->cpu, settings->allow_undoc);
          if (am == AM_ZPX)
            op =
                op_find(r->mnem, AM_ABSX, settings->cpu, settings->allow_undoc);
          if (am == AM_ZPY)
            op =
                op_find(r->mnem, AM_ABSY, settings->cpu, settings->allow_undoc);
        }
        if (!op) {
          {
            uint8_t docmask = 0;
            bool has_undoc = false;
            mnem_collect_presence(r->mnem, &docmask, &has_undoc);
            uint8_t sel = (settings->cpu == ASM_CPU_65816)   ? CPU_65816
                          : (settings->cpu == ASM_CPU_65C02) ? CPU_65C02
                                                             : CPU_6502;

            if (docmask & sel) {
              // Mnemonic exists on current CPU but addressing mode unsupported.
              // Special case: 65816 in 8-bit accumulator/index mode with a
              // wide immediate — suggest REP to widen the register.
              bool rep_hint = false;
              if (settings && settings->cpu == ASM_CPU_65816 && am == AM_IMM) {
                char mn_rep[8] = {0};
                sv_to_upper3(r->mnem, mn_rep);
                bool is_idx =
                    (strcmp(mn_rep, "CPX") == 0 || strcmp(mn_rep, "CPY") == 0 ||
                     strcmp(mn_rep, "LDX") == 0 || strcmp(mn_rep, "LDY") == 0);
                /* If the operand text contains a value that would need 16-bit,
                   we can infer the user may have forgotten REP. */
                bool cur8 = is_idx ? longi2 : longa2;
                if (cur8)
                  rep_hint = true;
              }
              if (rep_hint) {
                char mn_rep[8] = {0};
                sv_to_upper3(r->mnem, mn_rep);
                bool is_idx =
                    (strcmp(mn_rep, "CPX") == 0 || strcmp(mn_rep, "CPY") == 0 ||
                     strcmp(mn_rep, "LDX") == 0 || strcmp(mn_rep, "LDY") == 0);
                const char *repbits = is_idx ? "#$10" : "#$20";
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "operand out of range for 8-bit mode — "
                          "did you forget REP %s to enable 16-bit %s?",
                          repbits, is_idx ? "index registers" : "accumulator");
              } else {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "unsupported addressing mode for instruction");
              }
            } else if (docmask) {
              // Mnemonic exists but not on this CPU — give a targeted hint.
              if (settings->cpu != ASM_CPU_65816 && (docmask & CPU_65816) &&
                  !(docmask & (CPU_6502 | CPU_65C02))) {
                // Pure 65816-only instruction used on 6502/65C02
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "instruction not available for selected CPU "
                          "(try --cpu 65816 or add .cpu 65816)");
              } else if (settings->cpu == ASM_CPU_6502 &&
                         (docmask & CPU_65C02)) {
                // 65C02 instruction used on 6502
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "instruction not available for selected CPU "
                          "(try --cpu 65c02 or add .cpu 65c02)");
              } else {
                // Generic fallback
                const char *hint = (settings->cpu == ASM_CPU_65C02)
                                       ? "--cpu 6502"
                                       : "--cpu 65c02";
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                          (unsigned)r->mnem.len,
                          "instruction not available for selected CPU (try %s)",
                          hint);
              }
            } else if (has_undoc && !settings->allow_undoc) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                        (unsigned)r->mnem.len,
                        "undocumented opcode (enable --undoc)");
            } else {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->mnem_col,
                        (unsigned)r->mnem.len, "unknown instruction");
            }
          }
          r->emit_len = (uint16_t)(pc - pc_before);
          continue;
        }

        // 65816 variable-width immediate
        // REP/SEP operand is ALWAYS 8-bit (processor status register byte)
        uint8_t imm_extra = 0;
        if (settings && settings->cpu == ASM_CPU_65816 && am == AM_IMM) {
          char mn_p2[8] = {0};
          sv_to_upper3(r->mnem, mn_p2);
          bool always8b =
              (strcmp(mn_p2, "REP") == 0 || strcmp(mn_p2, "SEP") == 0 ||
               strcmp(mn_p2, "WDM") == 0);
          bool idx_m2 =
              (strcmp(mn_p2, "CPX") == 0 || strcmp(mn_p2, "CPY") == 0 ||
               strcmp(mn_p2, "LDX") == 0 || strcmp(mn_p2, "LDY") == 0);
          if (!always8b) {
            if (idx_m2) {
              if (!longi2)
                imm_extra = 1;
            } else {
              if (!longa2)
                imm_extra = 1;
            }
          }
        }

        emit_u8(mem_out, written_out, min_written, max_written, pc, op->opcode);
        if (op->bytes == 1) {
          pc += 1;
        } else if (am == AM_BLK) {
          // MVN/MVP: opcode, dstbank, srcbank
          expr_val_t ev1 = {0}, ev2 = {0};
          bool unr = false;
          long long v1 = 0, v2 = 0;
          if (expr_eval(curf, r->line_no, r->op_col, pat.expr, &syms,
                        anon_addrs, anon_count, anon_here, false, &unr, &ev1,
                        diags, proc_name, parent_label) &&
              ev1.kind == EXPR_VAL_NUM)
            v1 = ev1.num;
          expr_val_free(&ev1);
          unr = false;
          if (expr_eval(curf, r->line_no, r->op_col, pat.expr_blk, &syms,
                        anon_addrs, anon_count, anon_here, false, &unr, &ev2,
                        diags, proc_name, parent_label) &&
              ev2.kind == EXPR_VAL_NUM)
            v2 = ev2.num;
          expr_val_free(&ev2);
          emit_u8(mem_out, written_out, min_written, max_written,
                  (uint16_t)(pc + 1), (uint8_t)(v2 & 0xFF));
          emit_u8(mem_out, written_out, min_written, max_written,
                  (uint16_t)(pc + 2), (uint8_t)(v1 & 0xFF));
          pc += 3;
        } else if (op->bytes == 2) {
          // immediate or zp or (zp,x) etc
          long long opv = 0;
          sv_t ex = pat.expr;
          if (am == AM_IMM || am == AM_INDX || am == AM_INDY || am == AM_ZP ||
              am == AM_ZPX || am == AM_ZPY || am == AM_ZPIND || am == AM_SR ||
              am == AM_SRINY || am == AM_INDL || am == AM_INDLY) {
            expr_val_t ev = {0};
            bool unr = false;
            if (expr_eval(curf, r->line_no, r->op_col, ex, &syms, anon_addrs,
                          anon_count, anon_here, false, &unr, &ev, diags,
                          proc_name, parent_label)) {
              if (ev.kind != EXPR_VAL_NUM) {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                          (unsigned)r->operand.len, "operand must be numeric");
              } else {
                opv = ev.num;
                if (am == AM_IMM && imm_extra) {
                  if (opv < -32768 || opv > 0xFFFF) {
                    diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                              (unsigned)r->operand.len,
                              "word out of range (0..65535)");
                  } else {
                    emit_u8(mem_out, written_out, min_written, max_written,
                            (uint16_t)(pc + 1), (uint8_t)(opv & 0xFF));
                    emit_u8(mem_out, written_out, min_written, max_written,
                            (uint16_t)(pc + 2), (uint8_t)((opv >> 8) & 0xFF));
                  }
                } else {
                  if (opv < -128 || opv > 0xFF) {
                    // Check if this is a 65816 IMM that could be 16-bit with
                    // REP
                    bool gave_rep_hint = false;
                    if (settings && settings->cpu == ASM_CPU_65816 &&
                        am == AM_IMM && opv >= -32768 && opv <= 0xFFFF) {
                      char mn_rh[8] = {0};
                      sv_to_upper3(r->mnem, mn_rh);
                      bool always8h = (strcmp(mn_rh, "REP") == 0 ||
                                       strcmp(mn_rh, "SEP") == 0 ||
                                       strcmp(mn_rh, "WDM") == 0);
                      if (!always8h) {
                        bool is_idx_h = (strcmp(mn_rh, "CPX") == 0 ||
                                         strcmp(mn_rh, "CPY") == 0 ||
                                         strcmp(mn_rh, "LDX") == 0 ||
                                         strcmp(mn_rh, "LDY") == 0);
                        const char *repbits = is_idx_h ? "#$10" : "#$20";
                        const char *regname =
                            is_idx_h ? "index registers" : "accumulator";
                        diag_addf(diags, DIAG_ERROR, curf, r->line_no,
                                  r->op_col, (unsigned)r->operand.len,
                                  "operand out of range for 8-bit mode — "
                                  "did you forget REP %s to enable 16-bit %s?",
                                  repbits, regname);
                        gave_rep_hint = true;
                      }
                    }
                    if (!gave_rep_hint) {
                      diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                                (unsigned)r->operand.len,
                                "byte out of range (0..255)");
                    }
                  } else {
                    emit_u8(mem_out, written_out, min_written, max_written,
                            (uint16_t)(pc + 1), (uint8_t)(opv & 0xFF));
                  }
                }
              }
            }
            expr_val_free(&ev);
          }
          pc += (uint16_t)(2 + imm_extra);
        } else if (op->bytes == 3 && am == AM_RELL) {
          // BRL/PER: 16-bit relative
          expr_val_t ev = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, r->op_col, pat.expr, &syms,
                        anon_addrs, anon_count, anon_here, false, &unr, &ev,
                        diags, proc_name, parent_label)) {
            if (ev.kind != EXPR_VAL_NUM || !in_u16(ev.num)) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                        (unsigned)r->operand.len,
                        "branch target must be 0..65535");
            } else {
              uint16_t target = (uint16_t)ev.num;
              int rel = (int)target - (int)(pc + 3);
              if (rel < -32768 || rel > 32767) {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                          (unsigned)r->operand.len,
                          "BRL branch out of range (+-32767 bytes)");
              } else {
                emit_u8(mem_out, written_out, min_written, max_written,
                        (uint16_t)(pc + 1), (uint8_t)(rel & 0xFF));
                emit_u8(mem_out, written_out, min_written, max_written,
                        (uint16_t)(pc + 2), (uint8_t)((rel >> 8) & 0xFF));
              }
            }
          }
          expr_val_free(&ev);
          pc += 3;
        } else if (op->bytes == 3) {
          // abs or indexed abs or JMP indirect etc
          if (am == AM_IND || am == AM_ABSINDX) {
            // operand expr is inside parentheses
          }
          sv_t ex = pat.expr;
          if (am == AM_IMM)
            ex = pat.expr;
          expr_val_t ev = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, r->op_col, ex, &syms, anon_addrs,
                        anon_count, anon_here, false, &unr, &ev, diags,
                        proc_name, parent_label)) {
            if (ev.kind != EXPR_VAL_NUM || !in_u16(ev.num)) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                        (unsigned)r->operand.len,
                        "word out of range (0..65535)");
            } else {
              emit_le16(mem_out, written_out, min_written, max_written,
                        (uint16_t)(pc + 1), (uint16_t)ev.num);
            }
          }
          expr_val_free(&ev);
          pc += 3;
        } else if (op->bytes == 4) {
          // AM_LONG: 24-bit absolute
          expr_val_t ev = {0};
          bool unr = false;
          if (expr_eval(curf, r->line_no, r->op_col, pat.expr, &syms,
                        anon_addrs, anon_count, anon_here, false, &unr, &ev,
                        diags, proc_name, parent_label)) {
            if (ev.kind != EXPR_VAL_NUM) {
              diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                        (unsigned)r->operand.len, "operand must be numeric");
            } else {
              long long lv = ev.num;
              if (lv < 0 || lv > 0xFFFFFF) {
                diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                          (unsigned)r->operand.len,
                          "24-bit address out of range (0..$FFFFFF)");
              } else {
                emit_u8(mem_out, written_out, min_written, max_written,
                        (uint16_t)(pc + 1), (uint8_t)(lv & 0xFF));
                emit_u8(mem_out, written_out, min_written, max_written,
                        (uint16_t)(pc + 2), (uint8_t)((lv >> 8) & 0xFF));
                emit_u8(mem_out, written_out, min_written, max_written,
                        (uint16_t)(pc + 3), (uint8_t)((lv >> 16) & 0xFF));
              }
            }
          }
          expr_val_free(&ev);
          pc += 4;
        }
      }
      r->emit_len = (uint16_t)(pc - pc_before);

      // Process .halt/.alert/.unhalt/.unalert guard directives.
      if (r->kind == LK_GUARD) {
        const char *curf2 = r->file ? r->file : file;
        bool is_remove = sv_ieq(r->mnem, ".unhalt")  || sv_ieq(r->mnem, "unhalt") ||
                         sv_ieq(r->mnem, ".unalert") || sv_ieq(r->mnem, "unalert");
        bool is_halt   = sv_ieq(r->mnem, ".halt")    || sv_ieq(r->mnem, "halt");
        sv_t operand = sv_trim(r->operand);
        if (operand.len == 0) {
          diag_addf(diags, DIAG_ERROR, curf2, r->line_no, r->op_col, 1,
                    "%.*s requires an address or range",
                    (int)r->mnem.len, r->mnem.p);
        } else {
          /* Parse: addr[-addr2] ["message"]
           * addr may be a hex literal ($XXXX), decimal, or expression.
           * We use expr_eval for the start address, then look for '-'. */
          sv_t addr_part = operand;
          sv_t msg_part  = {0};

          /* Split off optional quoted message at the end. */
          const char *qp = operand.p + operand.len - 1;
          while (qp > operand.p && (*qp == ' ' || *qp == '\t')) qp--;
          if (qp >= operand.p && *qp == '"') {
            /* find matching opening quote */
            const char *qstart = qp - 1;
            while (qstart > operand.p && *qstart != '"') qstart--;
            if (*qstart == '"') {
              msg_part.p   = qstart + 1;
              msg_part.len = (size_t)(qp - qstart - 1);
              addr_part.len = (size_t)(qstart - operand.p);
              /* trim trailing whitespace from addr_part */
              while (addr_part.len > 0 &&
                     (addr_part.p[addr_part.len-1] == ' ' ||
                      addr_part.p[addr_part.len-1] == '\t'))
                addr_part.len--;
            }
          }

          /* Split addr_part on '-' to get optional end address.
           * Skip a leading '$' or '&' so we don't mistake the hex prefix. */
          sv_t start_sv = addr_part;
          sv_t end_sv   = {0};
          {
            const char *dash = NULL;
            for (size_t k = 1; k < addr_part.len; k++) {
              if (addr_part.p[k] == '-') { dash = addr_part.p + k; break; }
            }
            if (dash) {
              start_sv.len = (size_t)(dash - addr_part.p);
              /* trim */
              while (start_sv.len > 0 &&
                     (start_sv.p[start_sv.len-1] == ' ' ||
                      start_sv.p[start_sv.len-1] == '\t'))
                start_sv.len--;
              end_sv.p   = dash + 1;
              end_sv.len = (size_t)(addr_part.p + addr_part.len - end_sv.p);
              while (end_sv.len > 0 &&
                     (*end_sv.p == ' ' || *end_sv.p == '\t')) {
                end_sv.p++; end_sv.len--;
              }
            }
          }

          /* Evaluate start address. */
          expr_val_t ev_start = {0};
          bool unr_s = false;
          bool ok_s = expr_eval(curf2, r->line_no, r->op_col, start_sv,
                                &syms, anon_addrs, anon_count, anon_here,
                                false, &unr_s, &ev_start, diags,
                                proc_name, parent_label);
          uint16_t rstart = 0, rend = 0;
          bool addr_ok = false;
          if (ok_s && ev_start.kind == EXPR_VAL_NUM &&
              ev_start.num >= 0 && ev_start.num <= 0xFFFF) {
            rstart = (uint16_t)ev_start.num;
            rend   = rstart; /* default: single byte */
            addr_ok = true;
            /* Evaluate end address if present. */
            if (end_sv.len > 0) {
              expr_val_t ev_end = {0};
              bool unr_e = false;
              bool ok_e = expr_eval(curf2, r->line_no, r->op_col, end_sv,
                                    &syms, anon_addrs, anon_count, anon_here,
                                    false, &unr_e, &ev_end, diags,
                                    proc_name, parent_label);
              if (ok_e && ev_end.kind == EXPR_VAL_NUM &&
                  ev_end.num >= 0 && ev_end.num <= 0xFFFF) {
                rend = (uint16_t)ev_end.num;
                if (rend < rstart) {
                  diag_addf(diags, DIAG_ERROR, curf2, r->line_no, r->op_col,
                            (unsigned)r->operand.len,
                            "guard range end ($%04X) is less than start ($%04X)",
                            (unsigned)rend, (unsigned)rstart);
                  addr_ok = false;
                }
              } else if (ok_e) {
                diag_addf(diags, DIAG_ERROR, curf2, r->line_no, r->op_col, 1,
                          "guard range end address must be 0..65535");
                addr_ok = false;
              }
              expr_val_free(&ev_end);
            }
          } else if (ok_s) {
            diag_addf(diags, DIAG_ERROR, curf2, r->line_no, r->op_col, 1,
                      "guard address must be 0..65535");
          }
          expr_val_free(&ev_start);

          if (addr_ok) {
            if (is_remove) {
              guard_remove(rstart);
            } else {
              /* Copy message into a NUL-terminated buffer. */
              char msgbuf[GUARD_MSG_MAX];
              size_t mlen = msg_part.len < GUARD_MSG_MAX - 1
                                ? msg_part.len : GUARD_MSG_MAX - 1;
              if (mlen) memcpy(msgbuf, msg_part.p, mlen);
              msgbuf[mlen] = '\0';
              guard_add(rstart, rend, is_halt, msgbuf);
            }
          }
        }
      }

      // Track REP/SEP + .longa/.longi in pass2
      if (settings && settings->cpu == ASM_CPU_65816) {
        if (r->kind == LK_LONGA || r->kind == LK_LONGI) {
          sv_t arg = sv_trim(r->operand);
          bool is_on = sv_ieq(arg, "on") || (arg.len == 1 && arg.p[0] == '1');
          bool is_off = sv_ieq(arg, "off") || (arg.len == 1 && arg.p[0] == '0');
          if (is_on || is_off) {
            if (r->kind == LK_LONGA)
              longa2 = is_on ? false : true;
            else
              longi2 = is_on ? false : true;
          } /* else: already flagged in pass1 */
        } else if (r->kind == LK_INSTR) {
          char mn_p2[8] = {0};
          sv_to_upper3(r->mnem, mn_p2);
          if ((strcmp(mn_p2, "REP") == 0 || strcmp(mn_p2, "SEP") == 0) &&
              r->operand.len > 0) {
            sv_t oper = sv_trim(r->operand);
            if (oper.len > 0 && oper.p[0] == '#') {
              oper.p++;
              oper.len--;
              char tmp[32];
              size_t tl = oper.len < 31 ? oper.len : 31;
              memcpy(tmp, oper.p, tl);
              tmp[tl] = 0;
              const char *sp2 = tmp;
              int base2 = 0;
              if (*sp2 == '$' || *sp2 == '&') {
                sp2++;
                base2 = 16;
              }
              char *ep = NULL;
              long fv = strtol(sp2, &ep, base2);
              if (ep && ep != sp2) {
                if (strcmp(mn_p2, "REP") == 0) {
                  if (fv & 0x20)
                    longa2 = false;
                  if (fv & 0x10)
                    longi2 = false;
                } else {
                  if (fv & 0x20)
                    longa2 = true;
                  if (fv & 0x10)
                    longi2 = true;
                }
              }
            }
          }
        }
      }
    }

    // Flush any pending overwrite run from the final line.
    if (g_ow_owner) {
      ow_flush_run();
    }

    // teardown overwrite detection for this pass2 run
    if (ow_owner)
      free(ow_owner);
    if (ow_firstval)
      free(ow_firstval);
    ow_owner = NULL;
    ow_firstval = NULL;
    g_ow_settings = NULL;
    g_ow_file = NULL;
    g_ow_diags = NULL;
    g_ow_lines = NULL;
    g_ow_line_count = 0;
    g_ow_cur_index = 0;
    g_ow_owner = NULL;
    g_ow_firstval = NULL;
    /* Tear down guard state. */
    guards_reset();
    g_guard_diags      = NULL;
    g_guard_file       = NULL;
    g_guard_cur_index  = 0;
    g_guard_lines      = NULL;
    g_guard_line_count = 0;
    g_written_explicit = NULL;
    g_emit_is_res_fill = false;
    g_pc_was_set       = false;

    symtab_free(&syms);

    if (diags_has_errors(diags)) {
      free(cached_am);
      free(anon_addrs);
      free(lines);
      return false;
    }

    if (!changed)
      break; // stable
             // loop again with new cached_am affecting pass1
  }

  // Build the final symbol table from the assembled line records.
  // This is the source of truth for symbol dumps and --start/--end evaluation.
  symtab_t final_syms;
  symtab_init(&final_syms);

  // Pass A: insert all address labels using the final per-line PC.
  // We replay proc scope tracking to correctly prefix scoped labels.
  char final_proc_name[128] = {0};
  char final_parent_label[128] = {0};
  // Reuse scope_depth/scope_stack from earlier (already declared); reset for
  // final pass.
  scope_depth = 0;
  memset(scope_stack, 0, sizeof(scope_stack));
  for (size_t i = 0; i < line_count; i++) {
    const line_rec_t *r = &lines[i];
    // Track proc scope
    if (r->kind == LK_PROC) {
      if (r->operand.len > 0 && r->operand.len < sizeof(final_proc_name)) {
        memcpy(final_proc_name, r->operand.p, r->operand.len);
        final_proc_name[r->operand.len] = 0;
        // proc name also acts as parent for @ labels
        memcpy(final_parent_label, final_proc_name, r->operand.len);
        final_parent_label[r->operand.len] = 0;
        sv_t pname = {final_proc_name, r->operand.len};
        (void)symtab_put(&final_syms, pname, r->pc);
      }
      continue;
    }
    if (r->kind == LK_ENDPROC) {
      final_proc_name[0] = 0;
      continue;
    }
    // .scope / .endscope in final pass (for symbol export tracking)
    if (r->kind == LK_SCOPE) {
      if (scope_depth < 8) {
        if (r->operand.len > 0 && r->operand.len < sizeof(scope_stack[0])) {
          memcpy(scope_stack[scope_depth], r->operand.p, r->operand.len);
          scope_stack[scope_depth][r->operand.len] = 0;
        } else {
          scope_stack[scope_depth][0] = 0;
        }
        scope_depth++;
      }
      continue;
    }
    if (r->kind == LK_ENDSCOPE) {
      if (scope_depth > 0) {
        scope_depth--;
        scope_stack[scope_depth][0] = 0;
      }
      continue;
    }
    if (!r->label.len)
      continue;
    if (r->kind == LK_EQU)
      continue; // constants handled in pass B
    sv_t k = r->label;
    if (k.len && k.p[0] == '.') {
      k.p++;
      k.len--;
    }
    // Apply proc or parent prefix for @ labels and proc-scoped labels
    const char *scope = final_proc_name[0]
                            ? final_proc_name
                            : (scope_depth > 0 ? scope_stack[scope_depth - 1]
                                               : final_parent_label);
    char scoped_label[256];
    if (k.len && k.p[0] == '@') {
      // @ local label: store as "Parent::@name"
      if (scope[0]) {
        size_t plen = strlen(scope);
        if (plen + 2 + k.len < sizeof(scoped_label)) {
          memcpy(scoped_label, scope, plen);
          scoped_label[plen] = ':';
          scoped_label[plen + 1] = ':';
          memcpy(scoped_label + plen + 2, k.p, k.len);
          k.p = scoped_label;
          k.len = plen + 2 + k.len;
        }
      }
    } else {
      // Global label: update final_parent_label (outside proc)
      if (!final_proc_name[0] && k.len < sizeof(final_parent_label)) {
        memcpy(final_parent_label, k.p, k.len);
        final_parent_label[k.len] = 0;
      }
      // Apply proc prefix if inside a .proc
      if (final_proc_name[0] && k.len) {
        size_t plen = strlen(final_proc_name);
        if (plen + 2 + k.len < sizeof(scoped_label)) {
          memcpy(scoped_label, final_proc_name, plen);
          scoped_label[plen] = ':';
          scoped_label[plen + 1] = ':';
          memcpy(scoped_label + plen + 2, k.p, k.len);
          k.p = scoped_label;
          k.len = plen + 2 + k.len;
        }
      }
    }
    (void)symtab_put(&final_syms, k, r->pc);
  }

  // Pass B: evaluate and insert EQU/"=" constants.
  // (We do this after labels so --start/--end can reference any label.)
  for (size_t i = 0; i < line_count; i++) {
    const line_rec_t *r = &lines[i];
    if (r->kind != LK_EQU)
      continue;
    const char *curf = r->file ? r->file : file;
    if (!r->label.len)
      continue;
    if (r->operand.len == 0) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                "missing EQU expression");
      continue;
    }
    sv_t k = r->label;
    if (k.len && k.p[0] == '.') {
      k.p++;
      k.len--;
    }
    expr_val_t v = {0};
    bool unr = false;
    if (expr_eval(curf, r->line_no, r->op_col, r->operand, &final_syms,
                  anon_addrs, anon_count, anon_count, false, &unr, &v, diags,
                  proc_name, parent_label)) {
      if (v.kind != EXPR_VAL_NUM || v.num < 0 || v.num > 0xFFFFFF) {
        diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                  (unsigned)r->operand.len,
                  "EQU expression must be 0..$FFFFFF");
      } else {
        int32_t prev = 0;
        if (symtab_get(&final_syms, k, &prev) && prev != (int32_t)v.num) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->label_col,
                    (unsigned)r->label.len, "symbol '%.*s' redefined",
                    (int)r->label.len, r->label.p);
        } else {
          (void)symtab_put(&final_syms, k, (int32_t)v.num);
        }
      }
    }
    expr_val_free(&v);
  }

  // Process .error/.warning directives once, using the final symbol table.
  for (size_t i = 0; i < line_count; i++) {
    const line_rec_t *r = &lines[i];
    if (r->kind != LK_DIAG)
      continue;
    const char *curf = r->file ? r->file : file;
    bool is_warn = sv_ieq(r->mnem, ".warning") || sv_ieq(r->mnem, "warning");
    bool is_err = sv_ieq(r->mnem, ".error") || sv_ieq(r->mnem, "error");
    if (!is_warn && !is_err)
      continue;
    if (r->operand.len == 0) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                "%s requires a message", is_warn ? ".warning" : ".error");
      continue;
    }

    sv_t items[4];
    unsigned cols[4];
    unsigned cnt = 0;
    dir_split_items(r->operand, items, cols, &cnt, 4, r->op_col);
    if (cnt == 0) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                "%s requires a message", is_warn ? ".warning" : ".error");
      continue;
    }
    if (cnt > 2) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[2], 1,
                "too many arguments (use %s \"msg\" or %s expr, \"msg\")",
                is_warn ? ".warning" : ".error",
                is_warn ? ".warning" : ".error");
      continue;
    }

    bool trigger = true;
    if (cnt == 2) {
      expr_val_t cv = {0};
      bool unr = false;
      if (expr_eval(curf, r->line_no, cols[0], items[0], &final_syms,
                    anon_addrs, anon_count, anon_count, false, &unr, &cv, diags,
                    proc_name, parent_label)) {
        if (cv.kind != EXPR_VAL_NUM) {
          diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[0],
                    (unsigned)items[0].len,
                    "condition expression must be numeric");
          expr_val_free(&cv);
          continue;
        }
        trigger = (cv.num != 0);
      } else {
        expr_val_free(&cv);
        continue;
      }
      expr_val_free(&cv);
    }

    if (!trigger)
      continue;

    sv_t msg_sv = items[cnt - 1];
    unsigned msg_col = cols[cnt - 1];
    expr_val_t mv = {0};
    bool unr = false;
    if (expr_eval(curf, r->line_no, msg_col, msg_sv, &final_syms, anon_addrs,
                  anon_count, anon_count, false, &unr, &mv, diags, proc_name,
                  parent_label)) {
      if (mv.kind != EXPR_VAL_STR) {
        diag_addf(diags, DIAG_ERROR, curf, r->line_no, msg_col,
                  (unsigned)msg_sv.len, "message must be a string");
        expr_val_free(&mv);
        continue;
      }
      diag_addf(diags, is_warn ? DIAG_WARNING : DIAG_ERROR, curf, r->line_no,
                r->mnem_col, (unsigned)r->mnem.len, "%.*s", (int)mv.str_len,
                mv.str);
    }
    expr_val_free(&mv);
  }

  // Process .assert directives (cc65-style: .assert <expr>,
  // <error|warning|ldwarning>, "msg") Syntax: .assert <condition_expr>,
  // <errtype>, "message"
  //   errtype: error     -> assembler error if condition is FALSE (0)
  //            warning   -> assembler warning if condition is FALSE
  //            ldwarning -> treated as warning (linker warning not applicable
  //            here)
  // The condition is TRUE (non-zero) means OK — assert passes.
  for (size_t i = 0; i < line_count; i++) {
    const line_rec_t *r = &lines[i];
    if (r->kind != LK_ASSERT)
      continue;
    const char *curf = r->file ? r->file : file;

    if (r->operand.len == 0) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col, 1,
                ".assert requires: .assert <expr>, <error|warning|ldwarning>, "
                "\"message\"");
      continue;
    }

    // Split into up to 3 comma-separated items: <expr>, <errtype>, "message"
    sv_t items[4];
    unsigned cols[4];
    unsigned cnt = 0;
    dir_split_items(r->operand, items, cols, &cnt, 4, r->op_col);

    if (cnt < 3) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, r->op_col,
                (unsigned)r->operand.len,
                ".assert requires 3 arguments: .assert <expr>, "
                "<error|warning|ldwarning>, \"message\"");
      continue;
    }
    if (cnt > 3) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[3], 1,
                ".assert: too many arguments");
      continue;
    }

    // Inject "*" = current PC so .assert * < $9000 etc. works
    sv_t star_key = {"*", 1};
    symtab_put(&final_syms, star_key, r->pc);

    // Evaluate condition expression (item 0)
    expr_val_t cv = {0};
    bool unr = false;
    if (!expr_eval(curf, r->line_no, cols[0], items[0], &final_syms, anon_addrs,
                   anon_count, anon_count, false, &unr, &cv, diags, proc_name,
                   parent_label)) {
      expr_val_free(&cv);
      continue;
    }
    if (cv.kind != EXPR_VAL_NUM) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[0],
                (unsigned)items[0].len,
                ".assert condition must be a numeric expression");
      expr_val_free(&cv);
      continue;
    }
    bool condition_ok = (cv.num != 0);
    expr_val_free(&cv);

    if (condition_ok)
      continue; // assert passed — nothing to do

    // Determine error type (item 1)
    sv_t errtype = items[1];
    // trim whitespace
    while (errtype.len && (*errtype.p == ' ' || *errtype.p == '\t')) {
      errtype.p++;
      errtype.len--;
    }
    while (errtype.len && (errtype.p[errtype.len - 1] == ' ' ||
                           errtype.p[errtype.len - 1] == '\t'))
      errtype.len--;

    diag_severity_t dk;
    if (sv_ieq(errtype, "error")) {
      dk = DIAG_ERROR;
    } else if (sv_ieq(errtype, "warning") || sv_ieq(errtype, "ldwarning")) {
      dk = DIAG_WARNING;
    } else {
      diag_addf(
          diags, DIAG_ERROR, curf, r->line_no, cols[1], (unsigned)items[1].len,
          ".assert error type must be error, warning, or ldwarning (got: %.*s)",
          (int)errtype.len, errtype.p);
      continue;
    }

    // Evaluate message string (item 2)
    expr_val_t mv = {0};
    unr = false;
    if (!expr_eval(curf, r->line_no, cols[2], items[2], &final_syms, anon_addrs,
                   anon_count, anon_count, false, &unr, &mv, diags, proc_name,
                   parent_label)) {
      expr_val_free(&mv);
      continue;
    }
    if (mv.kind != EXPR_VAL_STR) {
      diag_addf(diags, DIAG_ERROR, curf, r->line_no, cols[2],
                (unsigned)items[2].len,
                ".assert message must be a string literal");
      expr_val_free(&mv);
      continue;
    }
    diag_addf(diags, dk, curf, r->line_no, r->mnem_col, (unsigned)r->mnem.len,
              ".assert failed: %.*s", (int)mv.str_len, mv.str);
    expr_val_free(&mv);
  }

  uint16_t sel_start = 0x0000;
  uint16_t sel_end = 0xFFFF;

  if (start_expr && *start_expr) {
    sv_t sv = sv_from_cstr(start_expr);
    expr_val_t v = {0};
    bool unr = false;
    if (expr_eval(file, 1, 1, sv, &final_syms, anon_addrs, anon_count,
                  anon_count, false, &unr, &v, diags, proc_name,
                  parent_label)) {
      if (v.kind != EXPR_VAL_NUM || !in_u16(v.num)) {
        diag_addf(diags, DIAG_ERROR, file, 1, 1, (unsigned)sv.len,
                  "--start out of range");
      } else {
        sel_start = (uint16_t)v.num;
      }
    }
    expr_val_free(&v);
  }
  if (end_expr && *end_expr) {
    sv_t sv = sv_from_cstr(end_expr);
    expr_val_t v = {0};
    bool unr = false;
    if (expr_eval(file, 1, 1, sv, &final_syms, anon_addrs, anon_count,
                  anon_count, false, &unr, &v, diags, proc_name,
                  parent_label)) {
      if (v.kind != EXPR_VAL_NUM || !in_u16(v.num)) {
        diag_addf(diags, DIAG_ERROR, file, 1, 1, (unsigned)sv.len,
                  "--end out of range");
      } else {
        sel_end = (uint16_t)v.num;
      }
    }
    expr_val_free(&v);
  }
  if (sel_end < sel_start) {
    diag_addf(diags, DIAG_ERROR, file, 1, 1, 1, "--end must be >= --start");
  }

  if (out_start)
    *out_start = sel_start;
  if (out_end)
    *out_end = sel_end;

  free(cached_am);

  // Listing output (BBC BASIC style)
  if (settings) {
    FILE *lf = NULL;
    if (settings->list_path && settings->list_path[0]) {
      if (strcmp(settings->list_path, "-") != 0) {
        lf = fopen(settings->list_path, "w");
        if (!lf) {
          diag_addf(diags, DIAG_ERROR, file, 1, 1, 1,
                    "cannot open listing file '%s'", settings->list_path);
        }
      } else {
        // "-" means stdout
      }
    }
    if (!diags_has_errors(diags)) {
      if (settings->list_stdout) {
        listing_write(stdout, settings, file, text, lines, line_count, mem_out);
      }
      if (lf) {
        listing_write(lf, settings, file, text, lines, line_count, mem_out);
      }
    }
    if (lf)
      fclose(lf);
  }

  // Symbol dump
  if (settings &&
      (settings->sym_stdout || (settings->sym_path && settings->sym_path[0]))) {
    FILE *sf = NULL;
    if (settings->sym_path && settings->sym_path[0] &&
        strcmp(settings->sym_path, "-") != 0) {
      sf = fopen(settings->sym_path, "w");
      if (!sf) {
        diag_addf(diags, DIAG_ERROR, file, 1, 1, 1,
                  "cannot open symbol file '%s'", settings->sym_path);
      }
    }
    if (!diags_has_errors(diags)) {
      if (settings->sym_stdout) {
        symbols_write(stdout, settings, &final_syms);
      }
      if (sf) {
        symbols_write(sf, settings, &final_syms);
      } else if (settings->sym_path && settings->sym_path[0] &&
                 strcmp(settings->sym_path, "-") == 0) {
        symbols_write(stdout, settings, &final_syms);
      }
    }
    if (sf)
      fclose(sf);
  }

  symtab_free(&final_syms);

  strpool_free(&file_pool);

  free(anon_addrs);

  free(lines);
  return !diags_has_errors(diags);
}