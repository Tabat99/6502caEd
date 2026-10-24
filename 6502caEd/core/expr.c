#include "expr.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  TK_EOF = 0,
  TK_NUM,
  TK_IDENT,
  TK_STR,
  TK_BANG,      // !
  TK_ANDAND,    // &&
  TK_OROR,      // ||
  TK_EQEQ,      // ==
  TK_NEQ,       // !=
  TK_LTE,       // <=
  TK_GTE,       // >=
  TK_PLUS,
  TK_MINUS,
  TK_STAR,
  TK_SLASH,
  TK_LPAREN,
  TK_RPAREN,
  TK_LT,
  TK_GT,
  TK_AMP,    // &  bitwise AND
  TK_PIPE,   // |  bitwise OR
  TK_CARET,  // ^  bitwise XOR
  TK_TILDE,  // ~  bitwise NOT (unary)
  TK_SHL,    // << shift left
  TK_SHR,    // >> shift right
  TK_PERCENT,// %  modulo
} tok_kind_t;

typedef struct {
  tok_kind_t k;
  sv_t lex;       // source slice (for span)
  unsigned col;   // absolute 1-based column in line
  int64_t num;    // for TK_NUM; for TK_STR holds (intptr_t)heap_buf
  size_t aux_len; // for TK_STR: string length in heap_buf
} tok_t;

typedef struct {
  const char *file;
  unsigned line_no;
  unsigned base_col;
  sv_t src;
  const symtab_t *syms;
  const uint16_t *anon_addrs;
  size_t anon_count;
  size_t anon_here;
  bool allow_undef;
  bool unresolved;
  const char *proc_name;    // current .proc scope, or NULL
  const char *parent_label; // most recent global label (for @-local resolution), or NULL
  const char *p;
  const char *end;
  unsigned rel_col; // 0-based within src
  tok_t cur;
  diags_t *diags;
} lex_t;

/* Free any heap-allocated data inside an expr_val_t and reset it to a
 * zero numeric value.  Safe to call on an uninitialised struct if zeroed. */
void expr_val_free(expr_val_t *v) {
  if (!v)
    return;
  if (v->kind == EXPR_VAL_STR)
    free(v->str);
  v->kind = EXPR_VAL_NUM;
  v->num = 0;
  v->str = NULL;
  v->str_len = 0;
}

/* Emit an error diagnostic at the given column within the current expression.
 * in:  col — 1-based absolute column in the source line
 *      len — highlight length
 *      msg — error message string */
static void diag_at(lex_t *L, unsigned col, unsigned len, const char *msg) {
  diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, col, len ? len : 1, "%s",
            msg);
}

/* Consume a double-quoted string literal starting at *L->p.
 * Handles \n \r \t \\ \" escape sequences.
 * The decoded string is stored in a heap buffer referenced by out->num.
 * ret: false on unterminated string or OOM */
static bool parse_string_tok(lex_t *L, tok_t *out) {
  // assumes *p == '"'
  const char *start = L->p;
  unsigned start_col = L->base_col + L->rel_col;
  L->p++;
  L->rel_col++;

  size_t cap = 32, len = 0;
  char *buf = (char *)malloc(cap);
  if (!buf) {
    diag_at(L, start_col, 1, "out of memory");
    return false;
  }

  while (L->p < L->end) {
    char c = *L->p;
    if (c == '"') {
      L->p++;
      L->rel_col++;
      break;
    }
    if (c == '\n' || c == 0) {
      free(buf);
      diag_at(L, start_col, 1, "unterminated string literal");
      return false;
    }
    if (c == '\\' && (L->p + 1 < L->end)) {
      char n = L->p[1];
      char outc = 0;
      switch (n) {
      case 'n':
        outc = '\n';
        break;
      case 'r':
        outc = '\r';
        break;
      case 't':
        outc = '\t';
        break;
      case '\\':
        outc = '\\';
        break;
      case '"':
        outc = '"';
        break;
      default:
        outc = n;
        break;
      }
      if (len + 1 > cap) {
        cap *= 2;
        char *nb = (char *)realloc(buf, cap);
        if (!nb) {
          free(buf);
          diag_at(L, start_col, 1, "out of memory");
          return false;
        }
        buf = nb;
      }
      buf[len++] = outc;
      L->p += 2;
      L->rel_col += 2;
      continue;
    }
    if (len + 1 > cap) {
      cap *= 2;
      char *nb = (char *)realloc(buf, cap);
      if (!nb) {
        free(buf);
        diag_at(L, start_col, 1, "out of memory");
        return false;
      }
      buf = nb;
    }
    buf[len++] = c;
    L->p++;
    L->rel_col++;
  }

  out->k = TK_STR;
  out->lex.p = start;
  out->lex.len = (size_t)(L->p - start);
  out->col = start_col;
  out->num = (int64_t)(intptr_t)buf;
  out->aux_len = len;
  return true;
}



/* Consume a single-quoted character literal ('A') and store its ASCII value
 * as a numeric token.  Only single characters are supported; no escape sequences.
 * ret: false on unterminated or empty literal */
static bool parse_char_tok(lex_t *L, tok_t *out) {
  // assumes *p == '\''
  const char *start = L->p;
  unsigned start_col = L->base_col + L->rel_col;
  L->p++;
  L->rel_col++;

  if (L->p >= L->end || *L->p == '\n' || *L->p == 0) {
    diag_at(L, start_col, 1, "unterminated character literal");
    return false;
  }

  unsigned char value = 0;
  if (*L->p == '\\') {
    if ((L->p + 1) >= L->end) {
      diag_at(L, start_col, 1, "unterminated character literal");
      return false;
    }
    char n = L->p[1];
    switch (n) {
    case 'n':
      value = '\n';
      break;
    case 'r':
      value = '\r';
      break;
    case 't':
      value = '\t';
      break;
    case '\\':
      value = '\\';
      break;
    case '\'':
      value = '\'';
      break;
    case '0':
      value = '\0';
      break;
    default:
      value = (unsigned char)n;
      break;
    }
    L->p += 2;
    L->rel_col += 2;
  } else {
    value = (unsigned char)*L->p;
    L->p++;
    L->rel_col++;
  }

  if (L->p >= L->end || *L->p != '\'') {
    diag_at(L, start_col, (unsigned)(L->p - start),
            "character literal must contain exactly one character");
    return false;
  }

  L->p++;
  L->rel_col++;

  out->k = TK_NUM;
  out->lex.p = start;
  out->lex.len = (size_t)(L->p - start);
  out->col = start_col;
  out->num = (int64_t)value;
  out->aux_len = 0;
  return true;
}

/* Advance the lexer to the next token, storing it in L->cur.
 * Skips whitespace.  Handles all operator pairs (==, !=, <=, >=, &&, ||,
 * <<, >>), numeric literals ($hex, &hex, %bin, decimal), string/char
 * literals, and identifiers including anonymous label references (:+/:-). */
static bool next_tok(lex_t *L) {
  while (L->p < L->end && is_ws(*L->p)) {
    L->p++;
    L->rel_col++;
  }
  tok_t t;
  memset(&t, 0, sizeof(t));
  t.col = L->base_col + L->rel_col;
  t.lex.p = L->p;
  if (L->p >= L->end || *L->p == 0) {
    t.k = TK_EOF;
    t.lex.len = 0;
    L->cur = t;
    return true;
  }
  char c = *L->p;

  // multi-char operators (check before single-char cases)
  if ((L->end - L->p) >= 2) {
    char c2 = L->p[1];

    // Anonymous label references (6502-style): :+ / :- (and .+ / .- alias)
    // Extended to :++, :---, etc. Meaning: jump to the Nth anonymous label
    // in that direction, where N is the number of + or - characters.
    // ca65-style aliases: ++ / -- (and longer runs; one fewer sign).
    if ((c == ':' || c == '.') && (c2 == '+' || c2 == '-')) {
      char sign = c2;
      size_t run = 1;
      while ((L->p + 1 + (ptrdiff_t)run) < L->end && L->p[1 + run] == sign) run++;

      t.k = TK_IDENT;
      t.lex.p = L->p;
      t.lex.len = 1 + run;
      L->p += 1 + run;
      L->rel_col += (int)(1 + run);
      L->cur = t;
      return true;
    }
    if ((c == '+' && c2 == '+') || (c == '-' && c2 == '-')) {
      char sign = c;
      size_t run = 2;
      while ((L->p + (ptrdiff_t)run) < L->end && L->p[run] == sign) run++;

      t.k = TK_IDENT;
      t.lex.p = L->p;
      t.lex.len = run;
      L->p += run;
      L->rel_col += (int)run;
      L->cur = t;
      return true;
    }
    if (c == '&' && c2 == '&') {
      t.k = TK_ANDAND;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
    if (c == '|' && c2 == '|') {
      t.k = TK_OROR;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
    if (c == '=' && c2 == '=') {
      t.k = TK_EQEQ;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
    if (c == '!' && c2 == '=') {
      t.k = TK_NEQ;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
    if (c == '<' && c2 == '=') {
      t.k = TK_LTE;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
    if (c == '>' && c2 == '=') {
      t.k = TK_GTE;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
    if (c == '<' && c2 == '<') {
      t.k = TK_SHL;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
    if (c == '>' && c2 == '>') {
      t.k = TK_SHR;
      t.lex.len = 2;
      L->p += 2;
      L->rel_col += 2;
      L->cur = t;
      return true;
    }
  }

  // one char tokens
  switch (c) {
  case '=': {
    // Accept single '=' as equality (== alias), common in cc65 and many assemblers.
    // Double '==' is already handled above in the multi-char section.
    t.k = TK_EQEQ;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  }
  case '!':
    t.k = TK_BANG;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '+':
    t.k = TK_PLUS;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '-':
    t.k = TK_MINUS;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '*':
    t.k = TK_STAR;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '/':
    t.k = TK_SLASH;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '(':
    t.k = TK_LPAREN;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case ')':
    t.k = TK_RPAREN;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '&': {
    /* '&' is a hex prefix when followed by a hex digit (e.g. &900 = 0x900).
       Otherwise it is bitwise AND.  This mirrors the '%' / binary-literal logic. */
    char nx = (L->p + 1 < L->end) ? L->p[1] : 0;
    bool is_hex_digit = (nx >= '0' && nx <= '9') || (nx >= 'a' && nx <= 'f') || (nx >= 'A' && nx <= 'F');
    if (!is_hex_digit) {
      t.k = TK_AMP;
      t.lex.len = 1;
      L->p++;
      L->rel_col++;
      L->cur = t;
      return true;
    }
    break; /* fall through to numeric literal parsing */
  }
  case '|':
    t.k = TK_PIPE;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '^':
    t.k = TK_CARET;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '~':
    t.k = TK_TILDE;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '%':
    // '%' starts a binary literal only when followed by 0 or 1.
    // Otherwise treat as modulo operator.
    if (L->p + 1 < L->end && (L->p[1] == '0' || L->p[1] == '1')) {
      // fall through to number parsing below (handled by parse_num_tok)
      break;
    }
    t.k = TK_PERCENT;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '<':
    t.k = TK_LT;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '>':
    t.k = TK_GT;
    t.lex.len = 1;
    L->p++;
    L->rel_col++;
    L->cur = t;
    return true;
  case '"':
    if (!parse_string_tok(L, &t))
      return false;
    L->cur = t;
    return true;
  case '\'':
    if (!parse_char_tok(L, &t))
      return false;
    L->cur = t;
    return true;
  default:
    break;
  }

  // numeric literal token
  if ((c >= '0' && c <= '9') || c == '$' || c == '&' || c == '%') {
    const char *s = L->p;
    const char *q = s + 1;
    while (q < L->end) {
      char d = *q;
      if (is_ws(d) || d == '+' || d == '-' || d == '*' || d == '/' ||
          d == '(' || d == ')' || d == '<' || d == '>' || d == ',')
        break;
      q++;
    }
    sv_t tok = {s, (size_t)(q - s)};
    int64_t v = 0;
    if (!parse_int_literal(tok, &v)) {
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                (unsigned)tok.len, "invalid number literal");
      return false;
    }
    t.k = TK_NUM;
    t.lex = tok;
    t.num = v;
    L->p = q;
    L->rel_col += (unsigned)tok.len;
    L->cur = t;
    return true;
  }

  // identifier token (includes @name local label references)
  if (is_ident_start(c) || c == '@') {
    const char *s = L->p;
    const char *q = s + 1;
    // If @ prefix, consume the following ident chars
    if (c == '@') {
      while (q < L->end && is_ident_char(*q))
        q++;
    } else {
      while (q < L->end && is_ident_char(*q))
        q++;
    }
    t.k = TK_IDENT;
    t.lex.p = s;
    t.lex.len = (size_t)(q - s);
    /* Map boolean keyword operators to their token kinds so that
       .if expressions can use natural-language operators:
         .or  / or  -> ||  (TK_OROR)
         .and / and -> &&  (TK_ANDAND)
         .not / not -> !   (TK_BANG)
       Dot-prefix optional; case-insensitive. */
    {
      const char *kw = t.lex.p;
      size_t      kl = t.lex.len;
      const char *w  = (kl > 0 && kw[0] == '.') ? kw + 1 : kw;
      size_t      wl = (kl > 0 && kw[0] == '.') ? kl - 1 : kl;
      /* case-fold comparison helper: tolower each char */
#define KW_EQ2(a,b)   ((w[0]|32)==(a) && (w[1]|32)==(b) && wl==2)
#define KW_EQ3(a,b,d) ((w[0]|32)==(a) && (w[1]|32)==(b) && (w[2]|32)==(d) && wl==3)
      if      (KW_EQ2('o','r'))           t.k = TK_OROR;
      else if (KW_EQ3('a','n','d'))       t.k = TK_ANDAND;
      else if (KW_EQ3('n','o','t'))       t.k = TK_BANG;
#undef KW_EQ2
#undef KW_EQ3
    }
    L->p = q;
    L->rel_col += (unsigned)t.lex.len;
    L->cur = t;
    return true;
  }

  diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col, 1,
            "unexpected character '%c' in expression", c);
  return false;
}

/* Return the left-binding power (precedence) for a binary operator token.
 * Higher value = tighter binding.  Used by the Pratt parser. */
static int lbp(tok_kind_t k) {
  switch (k) {
  case TK_OROR:
    return 2;
  case TK_ANDAND:
    return 3;
  case TK_EQEQ:
  case TK_NEQ:
  case TK_LTE:
  case TK_GTE:
  case TK_LT:
  case TK_GT:
    return 5;
  case TK_PIPE:
    return 6;
  case TK_CARET:
    return 7;
  case TK_AMP:
    return 8;
  case TK_SHL:
  case TK_SHR:
    return 9;
  case TK_PLUS:
  case TK_MINUS:
    return 10;
  case TK_STAR:
  case TK_SLASH:
  case TK_PERCENT:
    return 20;
  default:
    return 0;
  }
}

/* Set v to a numeric value, freeing any previous string. */
static bool val_set_num(expr_val_t *v, int64_t n) {
  expr_val_free(v);
  v->kind = EXPR_VAL_NUM;
  v->num = n;
  return true;
}

/* Set v to a string value (takes ownership of s/n), freeing any previous value. */
static bool val_set_str_owned(expr_val_t *v, char *s, size_t n) {
  expr_val_free(v);
  v->kind = EXPR_VAL_STR;
  v->str = s;
  v->str_len = n;
  v->num = 0;
  return true;
}

/* Pratt (top-down operator precedence) parser entry point.
 * Parses and evaluates a sub-expression with operators whose left-binding
 * power exceeds rbp.
 * in:  rbp — right-binding-power threshold (0 for full expression)
 * out: *out — result value
 * ret: false on parse error (diagnostic already emitted) */
static bool parse_expr_bp(lex_t *L, int rbp, expr_val_t *out);

/* Null-denotation handler: parse a prefix position token (literal, unary op,
 * or parenthesised sub-expression).
 * Handles: numbers, strings, chars, identifiers, unary +/-/</>/ !/~,
 *          current-PC '*', and opening parenthesis.
 * ret: false on error */
static bool nud(lex_t *L, tok_t t, expr_val_t *out) {
  if (t.k == TK_NUM) {
    return val_set_num(out, t.num);
  }
  if (t.k == TK_STR) {
    char *b = (char *)(intptr_t)t.num;
    return val_set_str_owned(out, b, t.aux_len);
  }
  if (t.k == TK_IDENT) {
    int32_t v = 0;
    sv_t key = t.lex;

    // cc65-style byte-extraction functions: .lobyte(expr) and .hibyte(expr)
    // These are equivalent to the unary prefix operators < and > respectively.
    // Also accept lobyte/hibyte without the leading dot.
    {
      sv_t k = key;
      if (k.len && k.p[0] == '.') { k.p++; k.len--; } // strip optional dot
      bool is_lobyte   = (k.len == 6 && sv_ieq(k, "lobyte"));
      bool is_hibyte   = (k.len == 6 && sv_ieq(k, "hibyte"));
      bool is_bankbyte = (k.len == 8 && sv_ieq(k, "bankbyte"));
      if (is_lobyte || is_hibyte || is_bankbyte) {
        // When nud() is called, L->cur already holds the lookahead (the token
        // immediately after the identifier), so '(' should already be there.
        if (L->cur.k != TK_LPAREN) {
          diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                    (unsigned)t.lex.len,
                    "%.*s requires parentheses: %.*s(expr)",
                    (int)t.lex.len, t.lex.p, (int)t.lex.len, t.lex.p);
          return false;
        }
        // Advance past '(' so parse_expr_bp starts at the inner expression
        next_tok(L);
        expr_val_t inner = {0};
        if (!parse_expr_bp(L, 0, &inner)) {
          expr_val_free(&inner);
          return false;
        }
        // parse_expr_bp leaves L->cur as the lookahead — should be ')'
        if (L->cur.k != TK_RPAREN) {
          diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                    (unsigned)t.lex.len, "expected ')' after %.*s(...)",
                    (int)t.lex.len, t.lex.p);
          expr_val_free(&inner);
          return false;
        }
        // Advance past ')' so the caller's lookahead is correct
        next_tok(L);
        if (inner.kind != EXPR_VAL_NUM) {
          diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                    (unsigned)t.lex.len,
                    "%.*s requires a numeric expression",
                    (int)t.lex.len, t.lex.p);
          expr_val_free(&inner);
          return false;
        }
        int64_t result = is_lobyte   ? (inner.num & 0xFF)
                       : is_hibyte   ? ((inner.num >> 8) & 0xFF)
                       : /* bankbyte */ ((inner.num >> 16) & 0xFF);
        expr_val_free(&inner);
        return val_set_num(out, result);
      }
    }

    // Anonymous label references.
    //  - :+   next anonymous label forward ('.+' alias)
    //  - :++  2nd next anonymous label forward
    //  - :-   previous anonymous label backward ('.-' alias)
    //  - :--  2nd previous anonymous label backward
    //  - ++ / -- are ca65-style aliases (and longer runs; one fewer sign)
    if (t.lex.len >= 2 && L->anon_addrs) {
      const char a0 = t.lex.p[0];
      const char a1 = t.lex.p[1];

      // :+ / :- / :++ / :--- (and .+ / .- variants)
      if ((a0 == ':' || a0 == '.') && (a1 == '+' || a1 == '-')) {
        const char sign = a1;
        size_t n = 1;
        while ((1 + n) < (size_t)t.lex.len && t.lex.p[1 + n] == sign) n++;
        // Mixed signs => not an anonymous label ref; fall through.
        if (1 + n == (size_t)t.lex.len) {
          const bool fwd = (sign == '+');
          // L->anon_here is the count of anonymous labels already encountered.
          // Next label is index anon_here (0-based); previous is anon_here-1.
          const int64_t idx = fwd ? (int64_t)L->anon_here + (int64_t)(n - 1)
                                  : (int64_t)L->anon_here - (int64_t)n;
          if (idx < 0 || (size_t)idx >= L->anon_count) {
            if (L->allow_undef) {
              L->unresolved = true;
              v = 0;
              return val_set_num(out, v);
            }
            diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                      (unsigned)t.lex.len,
                      "undefined anonymous label '%.*s'", (int)t.lex.len, t.lex.p);
            return false;
          }
          v = L->anon_addrs[(size_t)idx];
          return val_set_num(out, v);
        }
      }

      // ca65 aliases: ++ / -- / +++ / --- ... => :+ / :- / :++ / :-- ...
      if ((a0 == '+' || a0 == '-') && t.lex.len >= 2) {
        const char sign = a0;
        size_t run = 1;
        while (run < (size_t)t.lex.len && t.lex.p[run] == sign) run++;
        if (run == (size_t)t.lex.len) {
          const size_t n = run - 1; // '++' -> :+ ; '---' -> :--
          const bool fwd = (sign == '+');
          const int64_t idx = fwd ? (int64_t)L->anon_here + (int64_t)(n - 1)
                                  : (int64_t)L->anon_here - (int64_t)n;
          if (idx < 0 || (size_t)idx >= L->anon_count) {
            if (L->allow_undef) {
              L->unresolved = true;
              v = 0;
              return val_set_num(out, v);
            }
            diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                      (unsigned)t.lex.len,
                      "undefined anonymous label '%.*s'", (int)t.lex.len, t.lex.p);
            return false;
          }
          v = L->anon_addrs[(size_t)idx];
          return val_set_num(out, v);
        }
      }
    }

    if (key.len && key.p[0] == '.') {
      key.p++;
      key.len--;
    } // allow optional dot on use
    bool have_sym = false;
    // If key starts with '@', resolve as parent_label::@name (local label).
    // The parent is either the current proc name (if inside a .proc) or the
    // most recently seen global label (parent_label).
    if (key.len && key.p[0] == '@') {
      const char *parent = (L->proc_name && L->proc_name[0]) ? L->proc_name : L->parent_label;
      if (parent && L->syms) {
        size_t plen = strlen(parent);
        char scoped[256];
        if (plen + 2 + key.len < sizeof(scoped)) {
          memcpy(scoped, parent, plen);
          scoped[plen]     = ':';
          scoped[plen + 1] = ':';
          memcpy(scoped + plen + 2, key.p, key.len);
          sv_t scoped_key = { scoped, plen + 2 + key.len };
          if (symtab_get(L->syms, scoped_key, &v))
            have_sym = true;
        }
      }
      if (!have_sym) {
        if (L->allow_undef) {
          L->unresolved = true;
          v = 0;
          return val_set_num(out, v);
        }
        diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                  (unsigned)t.lex.len, "undefined local label '%.*s'",
                  (int)t.lex.len, t.lex.p);
        return false;
      }
      return val_set_num(out, (int64_t)v);
    }
    // If inside a .proc, try ProcName::Label first
    if (L->proc_name && L->syms && !have_sym) {
      size_t plen = strlen(L->proc_name);
      // Build "ProcName::Label" into a stack buffer (proc names are short)
      char scoped[256];
      if (plen + 2 + key.len < sizeof(scoped)) {
        memcpy(scoped, L->proc_name, plen);
        scoped[plen]     = ':';
        scoped[plen + 1] = ':';
        memcpy(scoped + plen + 2, key.p, key.len);
        sv_t scoped_key = { scoped, plen + 2 + key.len };
        if (symtab_get(L->syms, scoped_key, &v))
          have_sym = true;
      }
    }
    if (L->syms && !have_sym && symtab_get(L->syms, key, &v)) {
      have_sym = true;
    } else if (!have_sym && L->syms && sv_ieq(key, "cpu")) {
      /* Special case: allow .if CPU == 65c02 etc.
         The assembler defines/updates this symbol as "cpu".
         Keep general symbols case-sensitive, but treat this one as
         case-agnostic for convenience. */
      sv_t k2 = {"cpu", 3};
      if (symtab_get(L->syms, k2, &v))
        have_sym = true;
    }

    if (!have_sym) {
      if (L->allow_undef) {
        L->unresolved = true;
        v = 0;
      } else {
        diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
                  (unsigned)t.lex.len, "undefined symbol '%.*s'",
                  (int)t.lex.len, t.lex.p);
        return false;
      }
    }
    return val_set_num(out, (int64_t)v);
  }
  if (t.k == TK_LPAREN) {
    expr_val_t inner = {0};
    if (!parse_expr_bp(L, 0, &inner)) {
      expr_val_free(&inner);
      return false;
    }
    if (L->cur.k != TK_RPAREN) {
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, L->cur.col, 1,
                "expected ')'");
      expr_val_free(&inner);
      return false;
    }
    // consume ')'
    tok_t rp = L->cur;
    (void)rp;
    if (!next_tok(L)) {
      expr_val_free(&inner);
      return false;
    }
    *out = inner;
    return true;
  }

  if (t.k == TK_BANG) {
    expr_val_t right = {0};
    if (!parse_expr_bp(L, 30, &right)) {
      expr_val_free(&right);
      return false;
    }
    if (right.kind != EXPR_VAL_NUM) {
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col, 1,
                "numeric operator on string");
      expr_val_free(&right);
      return false;
    }
    int64_t n = (right.num == 0) ? 1 : 0;
    expr_val_free(&right);
    return val_set_num(out, n);
  }
  if (t.k == TK_TILDE) {
    expr_val_t right = {0};
    if (!parse_expr_bp(L, 30, &right)) {
      expr_val_free(&right);
      return false;
    }
    if (right.kind != EXPR_VAL_NUM) {
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col, 1,
                "unary ~ requires numeric operand");
      expr_val_free(&right);
      return false;
    }
    int64_t n = ~right.num;
    expr_val_free(&right);
    return val_set_num(out, n);
  }
  if (t.k == TK_PLUS || t.k == TK_MINUS || t.k == TK_LT || t.k == TK_GT) {
    // unary
    /* Check for missing operand before recursing — without this, the lexer
       advances to the next line and the error gets reported there instead. */
    if (L->cur.k == TK_EOF) {
      const char *op_name = (t.k == TK_LT) ? "<" :
                            (t.k == TK_GT) ? ">" :
                            (t.k == TK_PLUS) ? "+" : "-";
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col, 1,
                "'%s' operator requires an operand", op_name);
      return false;
    }
    expr_val_t right = {0};
    if (!parse_expr_bp(L, 30, &right)) {
      expr_val_free(&right);
      return false;
    }
    if (right.kind != EXPR_VAL_NUM) {
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col, 1,
                "unary operator requires numeric operand");
      expr_val_free(&right);
      return false;
    }
    int64_t n = right.num;
    if (t.k == TK_MINUS)
      n = -n;
    else if (t.k == TK_LT)
      n = (n & 0xFF);
    else if (t.k == TK_GT)
      n = ((n >> 8) & 0xFF);
    expr_val_free(&right);
    return val_set_num(out, n);
  }

  // '*' as current PC — resolved via a symbol named "*" injected by the assembler.
  if (t.k == TK_STAR && L->syms) {
    int32_t pc_val = 0;
    sv_t star_key = { "*", 1 };
    if (symtab_get(L->syms, star_key, &pc_val)) {
      return val_set_num(out, (int64_t)pc_val);
    }
  }

  diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, t.col,
            (unsigned)(t.lex.len ? t.lex.len : 1),
            "unexpected token in expression");
  return false;
}

/* Left-denotation handler: apply a binary operator to a left-hand value.
 * Handles: arithmetic (+,-,*,/,%), shifts (<<,>>), bitwise (&,|,^),
 *          comparisons (==,!=,<,>,<=,>=), logical (&&,||).
 * in:  op   — the infix operator token
 *      left — already-evaluated left operand (consumed/freed on success)
 * out: *out — result
 * ret: false on type error or division by zero */
/* Left-denotation: apply a binary operator to a left-hand value.
 * Handles arithmetic, bitwise, shift, comparison, and logical operators.
 * in:  op   — the infix operator token
 *      left — already-evaluated left operand (consumed on success)
 * out: *out — result
 * ret: false on type mismatch or divide-by-zero */
static bool led(lex_t *L, tok_t op, expr_val_t *left, expr_val_t *out) {
  int prec = lbp(op.k);
  expr_val_t right = {0};
  if (!parse_expr_bp(L, prec, &right)) {
    expr_val_free(&right);
    return false;
  }

  if (op.k == TK_PLUS) {
    if (left->kind == EXPR_VAL_STR || right.kind == EXPR_VAL_STR) {
      if (left->kind != EXPR_VAL_STR || right.kind != EXPR_VAL_STR) {
        diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, op.col, 1,
                  "cannot concatenate string with number");
        expr_val_free(&right);
        return false;
      }
      size_t n = left->str_len + right.str_len;
      char *b = (char *)malloc(n ? n : 1);
      if (!b) {
        diag_at(L, op.col, 1, "out of memory");
        expr_val_free(&right);
        return false;
      }
      if (left->str_len)
        memcpy(b, left->str, left->str_len);
      if (right.str_len)
        memcpy(b + left->str_len, right.str, right.str_len);
      expr_val_free(left);
      expr_val_free(&right);
      return val_set_str_owned(out, b, n);
    }
    int64_t n = left->num + right.num;
    expr_val_free(left);
    expr_val_free(&right);
    return val_set_num(out, n);
  }

  if (left->kind != EXPR_VAL_NUM || right.kind != EXPR_VAL_NUM) {
    diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, op.col, 1,
              "numeric operator on string");
    expr_val_free(&right);
    expr_val_free(left);
    return false;
  }

  int64_t n = left->num;
  if (op.k == TK_ANDAND)
    n = ((left->num != 0) && (right.num != 0)) ? 1 : 0;
  else if (op.k == TK_OROR)
    n = ((left->num != 0) || (right.num != 0)) ? 1 : 0;
  else if (op.k == TK_EQEQ)
    n = (left->num == right.num) ? 1 : 0;
  else if (op.k == TK_NEQ)
    n = (left->num != right.num) ? 1 : 0;
  else if (op.k == TK_LTE)
    n = (left->num <= right.num) ? 1 : 0;
  else if (op.k == TK_GTE)
    n = (left->num >= right.num) ? 1 : 0;
  else if (op.k == TK_LT)
    n = (left->num < right.num) ? 1 : 0;
  else if (op.k == TK_GT)
    n = (left->num > right.num) ? 1 : 0;
  else if (op.k == TK_AMP)
    n = left->num & right.num;
  else if (op.k == TK_PIPE)
    n = left->num | right.num;
  else if (op.k == TK_CARET)
    n = left->num ^ right.num;
  else if (op.k == TK_SHL)
    n = (right.num >= 0 && right.num < 64) ? (left->num << (int)right.num) : 0;
  else if (op.k == TK_SHR)
    n = (right.num >= 0 && right.num < 64) ? (left->num >> (int)right.num) : 0;
  if (op.k == TK_MINUS)
    n = left->num - right.num;
  else if (op.k == TK_STAR)
    n = left->num * right.num;
  else if (op.k == TK_SLASH) {
    if (right.num == 0) {
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, op.col,
                (unsigned)op.lex.len, "division by zero");
      expr_val_free(&right);
      expr_val_free(left);
      return false;
    }
    n = left->num / right.num;
  } else if (op.k == TK_PERCENT) {
    if (right.num == 0) {
      diag_addf(L->diags, DIAG_ERROR, L->file, L->line_no, op.col,
                (unsigned)op.lex.len, "modulo by zero");
      expr_val_free(&right);
      expr_val_free(left);
      return false;
    }
    n = left->num % right.num;
  }
  expr_val_free(left);
  expr_val_free(&right);
  return val_set_num(out, n);
}

/* Pratt parser core: parse a sub-expression where all operators have
 * left-binding-power > rbp.  Calls nud() for prefix tokens then led()
 * for each subsequent infix operator.
 * ret: false on parse error */
static bool parse_expr_bp(lex_t *L, int rbp, expr_val_t *out) {
  tok_t t = L->cur;
  if (!next_tok(L))
    return false;
  expr_val_t left = {0};
  if (!nud(L, t, &left)) {
    expr_val_free(&left);
    return false;
  }

  while (rbp < lbp(L->cur.k)) {
    tok_t op = L->cur;
    if (!next_tok(L)) {
      expr_val_free(&left);
      return false;
    }
    expr_val_t combined = {0};
    if (!led(L, op, &left, &combined)) {
      expr_val_free(&left);
      return false;
    }
    left = combined;
  }
  *out = left;
  return true;
}

/* Evaluate a numeric or string expression in a slice of a source line.
 *
 * Supports the full expression language: numeric literals ($hex, &hex, %bin,
 * decimal), identifiers (labels, .proc-scoped locals, @-locals, anonymous
 * labels :+/:-), strings, all arithmetic and bitwise operators, comparisons,
 * and logical operators.
 *
 * in:  file, line_no, base_col — source location for diagnostics
 *      src        — expression text (slice, not NUL-terminated)
 *      syms       — label symbol table
 *      anon_addrs — array of anonymous label addresses
 *      anon_count — length of anon_addrs
 *      anon_here  — index of the 'current' anonymous label for :+/:-
 *      allow_undef — if true, undefined identifiers substitute 0 and set
 *                    *out_unresolved instead of raising an error
 *      proc_name  — current .proc scope name, or NULL
 *      parent_label — most recent global label for @-local resolution, or NULL
 * out: *out_unresolved — set if allow_undef and an identifier was missing
 *      *out            — result value
 * ret: false on parse/evaluation error (diagnostic emitted) */
/* Evaluate a numeric or string expression from a source line slice.
 * in:  file, line_no, base_col — source location for diagnostics
 *      src        — expression text (not NUL-terminated)
 *      syms       — assembled symbol table for label lookup
 *      anon_addrs/anon_count/anon_here — anonymous label table
 *      allow_undef — if true, missing identifiers substitute 0
 *      proc_name  — current .proc scope name or NULL
 *      parent_label — enclosing global label for @-local resolution or NULL
 * out: *out_unresolved — set when allow_undef and a symbol was missing
 *      *out            — evaluated result
 * ret: false on error (diagnostic emitted) */
bool expr_eval(const char *file, unsigned line_no, unsigned base_col, sv_t src,
               const symtab_t *syms,
               const uint16_t* anon_addrs, size_t anon_count, size_t anon_here,
               bool allow_undef, bool *out_unresolved,
               expr_val_t *out, diags_t *diags,
               const char *proc_name, const char *parent_label) {
  if (!out)
    return false;
  expr_val_free(out);

  if (out_unresolved)
    *out_unresolved = false;

  lex_t L;
  memset(&L, 0, sizeof(L));
  L.file = file ? file : "<input>";
  L.line_no = line_no;
  L.base_col = base_col;
  L.src = src;
  L.syms = syms;
  L.anon_addrs = anon_addrs;
  L.anon_count = anon_count;
  L.anon_here = anon_here;
  L.allow_undef = allow_undef;
  L.unresolved = false;
  L.proc_name = proc_name;
  L.parent_label = parent_label;
  L.p = src.p;
  L.end = src.p + src.len;
  L.rel_col = 0;
  L.diags = diags;

  if (!next_tok(&L))
    return false;
  if (L.cur.k == TK_EOF) {
    diag_addf(diags, DIAG_ERROR, L.file, line_no, base_col, 1,
              "empty expression");
    return false;
  }
  expr_val_t v = {0};
  if (!parse_expr_bp(&L, 0, &v)) {
    expr_val_free(&v);
    return false;
  }
  if (L.cur.k != TK_EOF) {
    diag_addf(diags, DIAG_ERROR, L.file, line_no, L.cur.col,
              (unsigned)(L.cur.lex.len ? L.cur.lex.len : 1),
              "unexpected trailing tokens in expression");
    expr_val_free(&v);
    return false;
  }
  if (out_unresolved)
    *out_unresolved = L.unresolved;
  *out = v;
  return true;
}
