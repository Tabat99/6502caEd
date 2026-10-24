#include "lexer.h"
#include "curses_compat.h"
#include <ctype.h>
#include <string.h>

/* Return true if ch may begin an identifier token. */
static int is_ident0(int ch) { return isalpha((unsigned char)ch) || ch == '_'; }
/* Return true if ch may continue an identifier token. */
static int is_ident(int ch) { return isalnum((unsigned char)ch) || ch == '_'; }

/* Classify the first significant word on a line to determine its colour.
 * The word is compared case-insensitively against known directive and
 * mnemonic lists.
 * in:  w — pointer to word start, n — byte length
 * ret: TK_DATA for data directives, TK_DIRECTIVE for structural directives,
 *      TK_MNEMONIC for 3-letter instruction mnemonics, TK_LABEL for .labels,
 *      TK_DEFAULT if not recognised */
/* Classify the first word on a line for syntax highlighting.
 * in:  w — word start, n — byte length
 * ret: TK_DATA, TK_DIRECTIVE, TK_MNEMONIC, TK_LABEL, or TK_DEFAULT */
static token_kind_t classify_first_word(const char* w, size_t n) {
  if (!w || n == 0) return TK_DEFAULT;
  // '.labels' (and 'labels') is a directive, but we intentionally color it
  // using the label style so it always matches label color/theme.
  // NOTE: we keep other directives ('.if', '.else', '.endif', '.ifdef', etc.) as directives.
  {
    char up2[16];
    size_t nn = n;
    if (nn >= sizeof(up2)) nn = sizeof(up2) - 1;
    for (size_t i = 0; i < nn; i++) up2[i] = (char)toupper((unsigned char)w[i]);
    up2[nn] = 0;
    if (!strcmp(up2, ".LABELS") || !strcmp(up2, "LABELS")) return TK_LABEL;
  }
  // Data-emission directives — coloured as TK_DATA.
  static const char* data_dirs[] = {
      "EQUB", "EQUW", "EQUD", "EQUS", "EQB", "EQW", "EQD",
      "DB", "DQ", "DD",
      ".BYTE", ".WORD", ".DWORD",
      ".ALIGN", "ALIGN", ".RES", "RES",
  };
  // Structural / control directives — coloured as TK_DIRECTIVE.
  static const char* dirs[] = {
      "ORG", "EQU", "OPT", "INCLUDE", "INCLUDEROOT",
      ".ASSERT",
      ".ORG", ".PC", ".CPU",
      ".IF", ".IFDEF", ".IFNDEF", ".ELSE",
      ".ELIF", ".ELSEIF", ".ENDIF", ".PROC", ".ENDPROC",
      ".SCOPE", ".ENDSCOPE", ".REPEAT", ".ENDREPEAT",
      ".INCBIN", "INCBIN",
      ".INCLUDE", ".INCLUDEROOT",
      ".DEF", ".DEFINE", "#DEFINE", "#DEF",
      ".OPT", ".WARNING", ".ERROR",
      ".MACRO", ".ENDMACRO", ".ENDM",
      ".LONGA", ".LONGI",   /* 65816 register-width directives */
      ".HALT", ".UNHALT", ".ALERT", ".UNALERT", /* guard directives */
      ".PRAGMA",                                  /* pragma once */
      /* Boolean operators used in .if expressions */
      ".OR", ".AND", ".NOT", "OR", "AND", "NOT",
  };
  char up[16];
  if (n >= sizeof(up)) n = sizeof(up) - 1;
  for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)w[i]);
  up[n] = 0;
  for (size_t i = 0; i < sizeof(data_dirs) / sizeof(data_dirs[0]); i++) {
    if (!strcmp(up, data_dirs[i])) return TK_DATA;
  }
  for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
    if (!strcmp(up, dirs[i])) return TK_DIRECTIVE;
  }
  // Bare macro keywords
  if (!strcmp(up, "MACRO") || !strcmp(up, "ENDMACRO") || !strcmp(up, "ENDM"))
    return TK_DIRECTIVE;
  // Heuristic: 6502 mnemonics are typically 3 letters.
  if (n == 3 && isalpha((unsigned char)w[0]) && isalpha((unsigned char)w[1]) && isalpha((unsigned char)w[2]))
    return TK_MNEMONIC;
  return TK_DEFAULT;
}

/* Return true if ch is an operator or punctuation character. */
static int is_op_char(int ch) {
  return ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '<' || ch == '>' || ch == '&' || ch == '|' ||
         ch == '^' || ch == '~' || ch == '!' || ch == '=' || ch == ',' || ch == '#';
}

/* Write a single character to the ncurses screen at (y, x) using the
 * colour/attribute for token kind k, optionally reversed for the cursor line.
 * in:  y, x          — screen coordinates
 *      ch            — character to draw
 *      k             — token kind (determines colour)
 *      hl_line       — non-zero if this is the highlighted cursor line */
/* Write a character to the ncurses screen at (y,x) with the colour for kind k. */
static void putch_styled(int y, int x, int ch, const theme_t* theme, token_kind_t k, int hl_line) {
  chtype a = theme_attr(theme, k, hl_line);
  attrset(a);
  mvaddch(y, x, ch);
}

/* Return the numeric value of a hex digit (0..15), or -1 if ch is not hex. */
static int hex_val(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

// Parse a numeric literal at line[p]. Supports optional leading '-' and prefixes '&'/'$' (hex) and '%' (bin).
// Returns 1 on success and fills end/value. Returns 0 if not a number.
/* Try to parse a numeric literal starting at line[p].
 * Supports optional leading '-', and prefixes '&'/'$' (hex) and '%' (binary).
 * in:  line, len — the source line and its length
 *      p         — start position within line
 * out: *end      — one past the last digit on success
 *      *value    — parsed value on success
 * ret: 1 on success, 0 if no valid number starts at p */
/* Try to parse a numeric literal at line[p] ($hex, &hex, %bin, or decimal).
 * out: *end — one past the last digit, *value — parsed integer
 * ret: 1 on success, 0 if no valid number starts at p */
static int parse_number_at(const char* line, size_t len, size_t p, size_t* end, long long* value) {
  if (!line || p >= len) return 0;
  size_t i = p;
  int neg = 0;
  if (line[i] == '-') { neg = 1; i++; if (i >= len) return 0; }

  int base = 10;
  if (line[i] == '&' || line[i] == '$') { base = 16; i++; }
  else if (line[i] == '%') { base = 2; i++; }

  size_t d0 = i;
  long long v = 0;
  if (base == 16) {
    int hv;
    while (i < len && (hv = hex_val((unsigned char)line[i])) >= 0) {
      v = (v * 16) + hv;
      i++;
    }
  } else if (base == 2) {
    while (i < len && (line[i] == '0' || line[i] == '1')) {
      v = (v * 2) + (line[i] - '0');
      i++;
    }
  } else {
    while (i < len && isdigit((unsigned char)line[i])) {
      v = (v * 10) + (line[i] - '0');
      i++;
    }
  }

  if (i == d0) return 0; // no digits
  if (neg) v = -v;
  if (end) *end = i;
  if (value) *value = v;
  return 1;
}

/* Render a single source line to the ncurses screen with syntax highlighting.
 * Uses a lightweight lexical state machine — no full parse, no regex.
 *
 * Highlighting logic (in priority order):
 *   1. Duplicate-definition span  -> TK_DIAG_ERROR (red)
 *   2. Label at start of line     -> TK_LABEL
 *   3. First keyword after label  -> TK_MNEMONIC / TK_DIRECTIVE / TK_DATA
 *   4. Strings                    -> TK_STRING
 *   5. Comments                   -> TK_COMMENT
 *   6. Index registers after ','  -> TK_REGISTER
 *   7. Numeric literals           -> TK_NUMBER (or TK_DIAG_ERROR if out of range)
 *   8. Identifiers                -> TK_DIRECTIVE (macro call), TK_DIAG_ERROR
 *                                    (undefined), or TK_DEFAULT
 *
 * in:  y, x         — top-left screen cell for this line
 *      line         — NUL-terminated source line text
 *      left         — horizontal scroll offset (byte index into line)
 *      width        — number of screen columns to fill
 *      theme        — colour theme
 *      highlight_line — non-zero to apply reverse video (cursor line)
 *      symdb        — symbol database for undefined-identifier highlighting
 *      line_idx0    — 0-based line index (for per-line def-span lookup) */
/* Render a source line with syntax highlighting to the ncurses screen.
 * in:  y, x         — top-left screen cell
 *      line         — NUL-terminated source text
 *      left         — horizontal scroll offset (byte index into line)
 *      width        — screen columns to fill
 *      theme        — colour theme
 *      highlight_line — non-zero for cursor line (reverse video)
 *      symdb        — for undefined-identifier and macro highlighting
 *      line_idx0    — 0-based line index for per-line def-span lookup */
void lexer_render_line(int y, int x, const char* line, size_t left, int width,
                       const theme_t* theme, int highlight_line,
                       const symdb_t* symdb, size_t line_idx0, int suppress_diag) {
  if (!line || width <= 0) return;
  size_t len = strlen(line);
  if (left >= len) return;

  // Determine label/def span at start of line (optional).
  size_t i = 0;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  size_t label_end = 0;
  size_t def_s = 0, def_e = 0;
  bool def_dup = false;
  (void)symdb_line_def(symdb, line_idx0, &def_s, &def_e, &def_dup);
  if (def_dup && suppress_diag) def_dup = false;
  if (i < len && line[i] == '.') {
    // While typing a dot-label, treat a lone '.' as label prefix, not directive.
    if ((i + 1) >= len || !is_ident0((unsigned char)line[i + 1])) {
      label_end = i + 1;
    }
    size_t j = i + 1;
      if (j < len && is_ident0((unsigned char)line[j])) {
        j++;
        while (j < len && is_ident((unsigned char)line[j])) j++;
        // Only treat as a dot-label if classify_first_word doesn't
        // recognise it as a directive (e.g. .proc, .if, .org etc.)
        if (classify_first_word(line + i, j - i) != TK_DIRECTIVE)
          label_end = j;
      }
  } else if (i < len && line[i] == '@' && (i + 1) < len && is_ident0((unsigned char)line[i + 1])) {
    // @ local label definition: @name or @name:
    size_t j = i + 1;
    while (j < len && is_ident((unsigned char)line[j])) j++;
    if (j < len && line[j] == ':') {
      label_end = j + 1;
    } else {
      label_end = j;
    }
  } else if (i < len && is_ident0((unsigned char)line[i])) {
    size_t j = i + 1;
    while (j < len && is_ident((unsigned char)line[j])) j++;
    if (j < len && line[j] == ':') {
      label_end = j + 1;
    } else {
      // Colonless label definition at start of line:
      //   Label
      //   Label  lda #0
      //   Label  .byte 0
      // Keep this conservative so mnemonics/directives are not mis-coloured.
      token_kind_t this_word_kind = classify_first_word(line + i, j - i);
      if (this_word_kind == TK_DEFAULT) {
        size_t k2 = j;
        while (k2 < len && (line[k2] == ' ' || line[k2] == '\t')) k2++;
        if (k2 >= len || line[k2] == ';') {
          label_end = j;
        } else if (line[k2] == '=') {
          /* Constant definition: LABEL=value or LABEL = value */
          label_end = j;
        } else if (is_ident0((unsigned char)line[k2]) || line[k2] == '.') {
          /* Extract only the leading mnemonic/directive word after a
             potential colonless label. This keeps colouring working for:
               Label  lda #6
               Label  .byte 0
             and also while typing compact/incomplete forms like lda#6. */
          size_t w2 = k2 + 1;
          while (w2 < len && is_ident((unsigned char)line[w2]))
            w2++;
          token_kind_t next_kind = classify_first_word(line + k2, w2 - k2);
          if (next_kind == TK_MNEMONIC || next_kind == TK_DIRECTIVE || next_kind == TK_DATA)
            label_end = j;
        }
      }
    }
  }

  // Determine first word after label for mnemonic/directive styling.
  size_t w0 = 0, wlen = 0;
  size_t k = (label_end ? label_end : i);
  while (k < len && (line[k] == ' ' || line[k] == '\t')) k++;
  if (k < len && (is_ident0((unsigned char)line[k]) || line[k] == '.')) {
    w0 = k;
    size_t j = k + 1;
    while (j < len && (is_ident((unsigned char)line[j]) || (line[k] == '.' && is_ident((unsigned char)line[j])))) j++;
    wlen = j - k;
  }
  token_kind_t first_word_kind = classify_first_word(line + w0, wlen);
  // If first word is unrecognised but is a macro call, use directive colour.
  if (first_word_kind == TK_DEFAULT && symdb && wlen > 0) {
    if (symdb_is_macro(symdb, line + w0, wlen))
      first_word_kind = TK_DIRECTIVE;
  }

  // Render visible region char-by-char with a small lexical state machine.
  int in_string = 0;
  int in_comment = 0;
  int in_number = 0;
  int num_base = 10;
  int in_immediate = 0; // set when we see '#', cleared after we parse next number/identifier
  size_t num_err_end = 0;
  token_kind_t num_tk = TK_NUMBER;
  int want_reg = 0; // set after ',' and cleared after next non-space char
  size_t ident_end = 0;
  token_kind_t ident_tk = TK_DEFAULT;
  for (int col = 0; col < width; col++) {
    size_t p = left + (size_t)col;
    if (p >= len) break;
    unsigned char ch = (unsigned char)line[p];

    token_kind_t tk = TK_DEFAULT;

    // Continue an identifier token we started earlier.
    if (ident_end) {
      if (p < ident_end) {
        putch_styled(y, x + col, ch, theme, ident_tk, highlight_line);
        continue;
      }
      ident_end = 0;
    }

    // Continue a number token we started earlier.
    if (num_err_end) {
      if (p < num_err_end) {
        putch_styled(y, x + col, ch, theme, num_tk, highlight_line);
        continue;
      }
      num_err_end = 0;
      num_tk = TK_NUMBER;
    }

    if (in_comment) {
      tk = TK_COMMENT;
    } else if (in_string) {
      tk = TK_STRING;
      if (ch == '"') in_string = 0;
    } else {
      if (ch == ';') {
        in_comment = 1;
        tk = TK_COMMENT;
      } else if (ch == '"') {
        in_string = 1;
        tk = TK_STRING;
      } else if (def_dup && def_e > def_s && p >= def_s && p < def_e) {
        // Duplicate definition span (covers label defs and defining directives like .def/.define).
        tk = TK_DIAG_ERROR;
      } else if (p < label_end) {
        tk = TK_LABEL;
      } else if (wlen && p >= w0 && p < (w0 + wlen)) {
        tk = first_word_kind;
      } else {

        // Special-case index register after comma: ,X or ,Y (with optional whitespace).
        if (want_reg) {
          if (ch == ' ' || ch == '\t') {
            tk = TK_DEFAULT;
          } else {
            if (ch == 'X' || ch == 'x' || ch == 'Y' || ch == 'y') tk = TK_REGISTER;
            want_reg = 0;
          }
        }

        // Numbers: highlight the whole literal, not just the prefix.
        if (tk == TK_DEFAULT && in_number) {
          int ok = 0;
          if (num_base == 16) ok = isxdigit(ch);
          else if (num_base == 10) ok = isdigit(ch);
          else if (num_base == 2) ok = (ch == '0' || ch == '1');
          if (ok) {
            tk = TK_NUMBER;
          } else {
            in_number = 0;
          }
        }
        if (tk == TK_DEFAULT && !in_number) {
          // Numbers: parse full literal and apply out-of-bounds checks.
          if (ch == '-' || ch == '&' || ch == '$' || ch == '%' || isdigit(ch)) {
            size_t nend = 0;
            long long v = 0;
            if (parse_number_at(line, len, p, &nend, &v)) {
              // If the number is immediately followed by a letter/underscore
              // it's an alphanumeric token (e.g. "65c02") — consume whole token
              // and colour as TK_NUMBER to match GTK behaviour.
              if (nend < len && (isalpha((unsigned char)line[nend]) || line[nend] == '_')) {
                size_t j = p + 1;
                while (j < len && (isalnum((unsigned char)line[j]) || line[j] == '_')) j++;
                num_err_end = j;
                num_tk = TK_NUMBER;
                in_immediate = 0;
                putch_styled(y, x + col, ch, theme, TK_NUMBER, highlight_line);
                continue;
              } else {
                // Immediate byte range: -128..255 covers signed/unsigned byte
                // (e.g. ~VALUE yields a negative int64 that maps to a valid byte).
                if (in_immediate) {
                  num_tk = (!suppress_diag && (v < -128 || v > 255)) ? TK_DIAG_ERROR : TK_NUMBER;
                  in_immediate = 0;
                } else {
                  // Generic address/value range: -32768..65535.
                  num_tk = (!suppress_diag && (v < -32768 || v > 65535)) ? TK_DIAG_ERROR : TK_NUMBER;
                }
                num_err_end = nend;
                tk = num_tk;
                putch_styled(y, x + col, ch, theme, tk, highlight_line);
                continue;
              }
            }
          }

          /* Local branch labels used by some assemblers: :+ and :- */
          if (ch == ':' && p + 1 < len && (line[p + 1] == '+' || line[p + 1] == '-')) {
            ident_end = p + 2;
            ident_tk = TK_LABEL;
            tk = TK_LABEL;
          } else if (is_op_char(ch)) {
            tk = TK_OPERATOR;
            if (ch == ',') want_reg = 1;
            if (ch == '#') in_immediate = 1;
          } else if (is_ident0(ch)) {
            // Identifier token: treat as symbol reference and colour red if undefined.
            // Macro invocations get directive colour.
            size_t j = p + 1;
            while (j < len && is_ident((unsigned char)line[j])) j++;
            size_t tn = j - p;
            bool undef = false;
            bool is_macro_call = false;
            if (symdb && tn > 0) {
              if (symdb_is_macro(symdb, (const char*)line + p, tn))
                is_macro_call = true;
              else if (!symdb_is_defined(symdb, (const char*)line + p, tn))
                undef = true;
            }
            ident_end = j;
            ident_tk = is_macro_call ? TK_DIRECTIVE
                     : (undef && !suppress_diag) ? TK_DIAG_ERROR
                                     : TK_LABEL;
            tk = ident_tk;
            if (in_immediate) in_immediate = 0;
          } else {
            tk = TK_DEFAULT;
          }
        }
      }
    }

    putch_styled(y, x + col, ch, theme, tk, highlight_line);
  }
}
