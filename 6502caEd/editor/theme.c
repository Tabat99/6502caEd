#include "theme.h"
#include "curses_compat.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* ── Platform: exe dir and home dir ─────────────────────────────────── */
#ifdef _WIN32
#  include <windows.h>
#  include <direct.h>
static void theme_exe_dir(char *out, size_t cap) {
  DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
  if (n == 0) { out[0] = 0; return; }
  char *sl = strrchr(out, '\\'); char *sf = strrchr(out, '/');
  char *sep = sl > sf ? sl : sf;
  if (sep) *sep = 0; else out[0] = 0;
}
static void theme_config_path(char *out, size_t cap) {
  const char *a = getenv("APPDATA");
  if (a && a[0]) snprintf(out, cap, "%s/6502caEd/theme", a);
  else { const char *h = getenv("USERPROFILE");
         snprintf(out, cap, "%s/AppData/Roaming/6502caEd/theme", h ? h : "."); }
}
#else
#  include <unistd.h>
static void theme_exe_dir(char *out, size_t cap) {
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  if (n <= 0) { out[0] = 0; return; }
  out[n] = 0;
  char *slash = strrchr(out, '/');
  if (slash) *slash = 0; else out[0] = 0;
}
static void theme_config_path(char *out, size_t cap) {
  const char *home = getenv("HOME");
  snprintf(out, cap, "%s/.config/6502caEd/theme", home ? home : ".");
}
#endif
/* ───────────────────────────────────────────────────────────────────── */

/* Clamp a colour index to the range ncurses accepts (-1..7). */
static int clamp_col(int v) {
  if (v < -1) return -1;
  if (v > 7) return 7;
  return v;
}

/* Apply built-in defaults for every token kind.
 * Called first; values may be overridden by theme files loaded afterwards. */
static void theme_set_defaults(theme_t* t) {
  for (int i = 0; i < (int)TK__COUNT; i++) {
    t->fg[i] = -1;   /* terminal default foreground */
    t->bg[i] = -1;   /* terminal default background */
    t->pair[i] = 0;
    t->bold[i] = 0;
  }
  /* Reasonable defaults for common terminals. */
  t->fg[TK_DEFAULT] = -1;
  t->fg[TK_MNEMONIC] = COLOR_CYAN;
  t->fg[TK_REGISTER] = COLOR_GREEN;
  t->bold[TK_REGISTER] = 1;
  t->fg[TK_DIRECTIVE] = COLOR_MAGENTA;
  t->fg[TK_DATA] = COLOR_CYAN;
  t->bold[TK_DATA] = 1; /* bright_cyan */
  t->fg[TK_NUMBER] = COLOR_YELLOW;
  /* Numeric sub-kinds (addr, imm, data-directive values) default to TK_NUMBER's colour. */
  t->fg[TK_ADDR_HEX]  = COLOR_YELLOW;
  t->fg[TK_ADDR_DEC]  = COLOR_YELLOW;
  t->fg[TK_ADDR_BIN]  = COLOR_YELLOW;
  t->fg[TK_IMM_HEX]   = COLOR_YELLOW;
  t->fg[TK_IMM_DEC]   = COLOR_YELLOW;
  t->fg[TK_IMM_BIN]   = COLOR_YELLOW;
  t->fg[TK_IMM_CHAR]  = COLOR_YELLOW;
  t->fg[TK_DV_HEX]    = COLOR_YELLOW;
  t->fg[TK_DV_DEC]    = COLOR_YELLOW;
  t->fg[TK_DV_BIN]    = COLOR_YELLOW;
  t->fg[TK_DV_STR]    = COLOR_GREEN;
  t->fg[TK_STRING] = COLOR_GREEN;
  t->fg[TK_COMMENT] = COLOR_BLUE;
  t->fg[TK_LABEL] = COLOR_CYAN;
  t->fg[TK_OPERATOR] = COLOR_WHITE;
  /* Errors: bold red.  Warnings: yellow. */
  t->fg[TK_DIAG_ERROR] = COLOR_RED;
  t->bold[TK_DIAG_ERROR] = 1;
  t->fg[TK_DIAG_WARNING] = COLOR_YELLOW;

  /* File selector token kinds. */
  t->fg[TK_FS_TITLE]      = COLOR_MAGENTA; t->bold[TK_FS_TITLE]      = 1;
  t->fg[TK_FS_PATH]       = COLOR_CYAN;    t->bold[TK_FS_PATH]       = 1;
  t->fg[TK_FS_TAB_ACTIVE] = COLOR_MAGENTA; t->bold[TK_FS_TAB_ACTIVE] = 1;
  t->fg[TK_FS_TAB]        = COLOR_BLUE;
  t->fg[TK_FS_DIR]        = COLOR_CYAN;    t->bold[TK_FS_DIR]        = 1;
  t->fg[TK_FS_FILE]       = -1;
  t->fg[TK_FS_SELECTED]   = COLOR_YELLOW;  t->bold[TK_FS_SELECTED]   = 1;
  t->fg[TK_FS_INPUT]      = COLOR_WHITE;   t->bold[TK_FS_INPUT]      = 0;
  t->fg[TK_FS_HELP]       = COLOR_BLUE;
}

/* Parse a colour name or digit (0..7) from a theme file value string.
 * "bright_X" colours are encoded as 100 + X so the caller can detect
 * them and set the bold flag.
 * ret: colour index, or -1 for "default", or -2 for unrecognised */
static int parse_color_name(const char* s) {
  if (!s || !*s) return -2;
  /* Single digit 0..7 */
  if (isdigit((unsigned char)s[0]) && !s[1]) {
    int v = s[0] - '0';
    return (v >= 0 && v <= 7) ? v : -2;
  }
  char buf[32];
  size_t n = strlen(s);
  if (n >= sizeof(buf)) n = sizeof(buf) - 1;
  for (size_t i = 0; i < n; i++) buf[i] = (char)tolower((unsigned char)s[i]);
  buf[n] = 0;

  if (!strcmp(buf, "default")) return -1;
  /* Bright colours are represented as bold + base colour. */
  if (!strcmp(buf, "bright_red"))     return 100 + COLOR_RED;
  if (!strcmp(buf, "bright_green"))   return 100 + COLOR_GREEN;
  if (!strcmp(buf, "bright_yellow"))  return 100 + COLOR_YELLOW;
  if (!strcmp(buf, "bright_blue"))    return 100 + COLOR_BLUE;
  if (!strcmp(buf, "bright_magenta")) return 100 + COLOR_MAGENTA;
  if (!strcmp(buf, "bright_cyan"))    return 100 + COLOR_CYAN;
  if (!strcmp(buf, "bright_white"))   return 100 + COLOR_WHITE;
  if (!strcmp(buf, "black"))    return COLOR_BLACK;
  if (!strcmp(buf, "red"))      return COLOR_RED;
  if (!strcmp(buf, "green"))    return COLOR_GREEN;
  if (!strcmp(buf, "yellow"))   return COLOR_YELLOW;
  if (!strcmp(buf, "blue"))     return COLOR_BLUE;
  if (!strcmp(buf, "magenta"))  return COLOR_MAGENTA;
  if (!strcmp(buf, "cyan"))     return COLOR_CYAN;
  if (!strcmp(buf, "white"))    return COLOR_WHITE;
  return -2;
}

/* Map a lowercase theme-file key string to a token_kind_t enum value.
 * Returns TK__COUNT for unrecognised keys. */
static token_kind_t parse_kind(const char* k) {
  if (!k) return TK__COUNT;
  if (!strcmp(k, "default"))    return TK_DEFAULT;
  if (!strcmp(k, "mnemonic"))   return TK_MNEMONIC;
  if (!strcmp(k, "register"))   return TK_REGISTER;
  if (!strcmp(k, "directive"))  return TK_DIRECTIVE;
  if (!strcmp(k, "data"))       return TK_DATA;
  if (!strcmp(k, "number"))     return TK_NUMBER;
  if (!strcmp(k, "addr_hex") || !strcmp(k, "address_hex")) return TK_ADDR_HEX;
  if (!strcmp(k, "addr_dec") || !strcmp(k, "address_dec") || !strcmp(k, "addr_decimal")) return TK_ADDR_DEC;
  if (!strcmp(k, "addr_bin") || !strcmp(k, "address_bin") || !strcmp(k, "addr_binary")) return TK_ADDR_BIN;
  if (!strcmp(k, "imm_hex") || !strcmp(k, "immediate_hex")) return TK_IMM_HEX;
  if (!strcmp(k, "imm_dec") || !strcmp(k, "immediate_dec") || !strcmp(k, "imm_decimal")) return TK_IMM_DEC;
  if (!strcmp(k, "imm_bin") || !strcmp(k, "immediate_bin") || !strcmp(k, "imm_binary")) return TK_IMM_BIN;
  if (!strcmp(k, "imm_char") || !strcmp(k, "immediate_char") || !strcmp(k, "imm_character")) return TK_IMM_CHAR;
  if (!strcmp(k, "dv_hex") || !strcmp(k, "data_hex")) return TK_DV_HEX;
  if (!strcmp(k, "dv_dec") || !strcmp(k, "data_dec") || !strcmp(k, "data_decimal")) return TK_DV_DEC;
  if (!strcmp(k, "dv_bin") || !strcmp(k, "data_bin") || !strcmp(k, "data_binary")) return TK_DV_BIN;
  if (!strcmp(k, "dv_str") || !strcmp(k, "data_str") || !strcmp(k, "data_string")) return TK_DV_STR;
  if (!strcmp(k, "string"))     return TK_STRING;
  if (!strcmp(k, "comment"))    return TK_COMMENT;
  if (!strcmp(k, "label"))      return TK_LABEL;
  if (!strcmp(k, "operator"))   return TK_OPERATOR;
  if (!strcmp(k, "error"))      return TK_DIAG_ERROR;
  if (!strcmp(k, "warning"))    return TK_DIAG_WARNING;
  if (!strcmp(k, "fs_title"))      return TK_FS_TITLE;
  if (!strcmp(k, "fs_path"))       return TK_FS_PATH;
  if (!strcmp(k, "fs_tab_active")) return TK_FS_TAB_ACTIVE;
  if (!strcmp(k, "fs_tab"))        return TK_FS_TAB;
  if (!strcmp(k, "fs_dir"))        return TK_FS_DIR;
  if (!strcmp(k, "fs_file"))       return TK_FS_FILE;
  if (!strcmp(k, "fs_selected"))   return TK_FS_SELECTED;
  if (!strcmp(k, "fs_input"))      return TK_FS_INPUT;
  if (!strcmp(k, "fs_help"))       return TK_FS_HELP;
  return TK__COUNT;
}

/* Trim leading and trailing whitespace from s in-place. */
static void trim(char* s) {
  if (!s) return;
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || isspace((unsigned char)s[n - 1]))) s[--n] = 0;
  size_t i = 0;
  while (s[i] && isspace((unsigned char)s[i])) i++;
  if (i) memmove(s, s + i, strlen(s + i) + 1);
}

/* Load a theme file and apply its key=value entries to t.
 * Line format:  kind = colour   or   kind.bg = colour
 * Comments start with # or ;.  Unrecognised keys are silently ignored.
 * ret: 1 if the file was opened and read, 0 if it could not be opened */
static int load_file(theme_t* t, const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    trim(line);
    if (!line[0] || line[0] == '#' || line[0] == ';') continue;

    char* eq = strchr(line, '=');
    if (!eq) continue;
    *eq++ = 0;
    trim(line);
    trim(eq);

    /* Key forms: <kind>=<colour>  or  <kind>.bg=<colour>  or  <kind>.fg=<colour> */
    char* dot = strchr(line, '.');
    int is_bg = 0;
    if (dot) {
      *dot++ = 0;
      trim(dot);
      if (!strcmp(dot, "bg")) is_bg = 1;
      else if (!strcmp(dot, "fg")) is_bg = 0;
      else continue;
    }

    /* Fold key to lowercase before lookup. */
    for (char* p = line; *p; p++) *p = (char)tolower((unsigned char)*p);
    token_kind_t kind = parse_kind(line);
    if (kind == TK__COUNT) continue;

    int c = parse_color_name(eq);
    if (c == -2) continue;
    int bold = 0;
    if (c >= 100) { bold = 1; c -= 100; }  /* "bright_X" encoded as 100+X */
    if (is_bg) t->bg[kind] = clamp_col(c);
    else { t->fg[kind] = clamp_col(c); t->bold[kind] = (unsigned char)bold; }
  }
  fclose(f);
  return 1;
}

/* Initialise ncurses colour pairs — one pair per token kind.
 * Must be called after start_color() and use_default_colors(). */
static void init_pairs(theme_t* t) {
  start_color();
  if (!has_colors()) return;
#ifdef NCURSES_VERSION
  use_default_colors();
#endif
  short next_pair = 1;
  for (int i = 0; i < (int)TK__COUNT; i++) {
    short fg = (short)t->fg[i];
    short bg = (short)t->bg[i];
    /* ncurses uses -1 to mean "terminal default" when use_default_colors() is set. */
    if (fg == 0 && t->fg[i] == -1) fg = -1;
    if (bg == 0 && t->bg[i] == -1) bg = -1;
#ifdef _WIN32
    pdc_init_pair(next_pair, fg, bg);
#else
    init_pair(next_pair, fg, bg);
#endif
    t->pair[i] = next_pair;
    t->color_attr[i] = (uint64_t)COLOR_PAIR(next_pair);
    next_pair++;
  }
}

/* Load the theme and initialise ncurses colour pairs.
 * Theme files are searched in order:
 *   1. ./6502caEd.theme          (next to the working directory)
 *   2. <exe_dir>/6502caEd.theme  (next to the executable)
 *   3. ~/.config/6502caEd/theme  (user override — applied last, wins on conflict)
 * ret: 1 on success (or if only defaults are used), 0 on fatal error */
/* Load the theme from disk and initialise ncurses colour pairs.
 * Search order: ./6502caEd.theme, <exe_dir>/6502caEd.theme,
 *               ~/.config/6502caEd/theme (last file wins on conflict).
 * ret: 1 on success (or if only defaults are used), 0 on fatal error */
int theme_load_and_init(theme_t* t) {
  if (!t) return 0;
  theme_set_defaults(t);

  /* Try local theme first (current directory). */
  if (!load_file(t, "6502caEd.theme")) {
    /* Also try next to the executable. */
    char dir[512];
    theme_exe_dir(dir, sizeof(dir));
    if (dir[0]) {
      char tpath[560];
      snprintf(tpath, sizeof(tpath), "%s/6502caEd.theme", dir);
      (void)load_file(t, tpath);
    }
  }

  /* User override: config dir */
  {
    char path[512];
    theme_config_path(path, sizeof(path));
    (void)load_file(t, path);
  }

  init_pairs(t);
  return 1;
}

/* Return the ncurses attribute set for token kind k.
 * If highlight_line is non-zero, A_REVERSE is OR'd in (current-line highlight).
 * in:  t              — theme
 *      k              — token kind
 *      highlight_line — non-zero if this is the cursor line
 * ret: ncurses attr value suitable for passing to attrset/addch */
/* Return the ncurses attribute (colour pair + bold) for token kind k.
 * If highlight_line is non-zero, A_REVERSE is OR'd in for the cursor line.
 * in:  t              — theme
 *      k              — token kind
 *      highlight_line — non-zero for current cursor line
 * ret: ncurses attr value for attrset/addch */
chtype theme_attr(const theme_t* t, token_kind_t k, int highlight_line) {
  chtype a = A_NORMAL;
  if (t && k >= 0 && k < TK__COUNT && t->pair[k] > 0) {
    a |= (chtype)t->color_attr[k];
    if (t->bold[k]) a |= A_BOLD;
  }
  if (highlight_line) a |= A_REVERSE;
  return a;
}
