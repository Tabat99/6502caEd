/*
  6502caEd TUI (Terminal editor)
  Part of 6502caEd cross compiler suite
  MJD 2026
*/

#include "tui.h"
#include "diagdb.h"
#include "fold.h"

#include "version.h"

#include "lexer.h"
#include "runner.h"
#include "symdb.h"
#include "theme.h"
#include <ctype.h>
#include <stdint.h>

/* Named range struct for tui_memmap_dialog */
typedef struct {
  uint16_t start;
  uint16_t end;
  uint32_t count;
} tui_range_t;
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Platform abstractions ───────────────────────────────────────────── */
#ifdef _WIN32
#  include <windows.h>    /* ShellExecuteA, GetModuleFileNameA, clipboard */
#  include <io.h>         /* _isatty */
#  include <direct.h>     /* _mkdir */
#  include <pdcurses/curses.h>   /* PDCurses on MSYS2 UCRT64 */

#  define TUI_PATH_SEP      ';'
#  define TUI_SLASH         '\\'

/* PDCurses on Windows handles resize differently — it fires KEY_RESIZE
   which wgetch() returns directly; no SIGWINCH needed. */
#  define SIGWINCH          0   /* dummy — never registered on Windows */

/* usleep equivalent */
static inline void tui_usleep(int us) { Sleep((DWORD)(us / 1000 + 1)); }

/* isatty */
#  define isatty(fd)        _isatty(fd)

/* mkdir (one-arg on Windows) */
#  define tui_mkdir(p)      _mkdir(p)

/* Home directory */
static const char *tui_home(void) {
  const char *h = getenv("USERPROFILE");
  if (!h) h = getenv("HOME");
  return h;
}

/* Config dir: %APPDATA%\6502caEd  (falls back to USERPROFILE) */
static void tui_config_dir(char *out, size_t cap) {
  const char *a = getenv("APPDATA");
  if (a && a[0])
    snprintf(out, cap, "%s/6502caEd", a);
  else {
    const char *h = tui_home();
    snprintf(out, cap, "%s/AppData/Roaming/6502caEd", h ? h : ".");
  }
}

/* Exe directory */
static void tui_exe_dir(char *out, size_t cap) {
  DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
  if (n == 0) { out[0] = 0; return; }
  char *sl = strrchr(out, '\\');
  char *sf = strrchr(out, '/');
  char *sep = sl > sf ? sl : sf;
  if (sep) *sep = 0; else out[0] = 0;
}

/* Win32 clipboard */
static int clipboard_win32_set(const char *text) {
  if (!text) return 0;
  size_t len = strlen(text) + 1;
  HGLOBAL hm = GlobalAlloc(GMEM_MOVEABLE, len);
  if (!hm) return 0;
  memcpy(GlobalLock(hm), text, len);
  GlobalUnlock(hm);
  if (!OpenClipboard(NULL)) { GlobalFree(hm); return 0; }
  EmptyClipboard();
  SetClipboardData(CF_TEXT, hm);
  CloseClipboard();
  return 1;
}
static char *clipboard_win32_get(void) {
  if (!OpenClipboard(NULL)) return NULL;
  HGLOBAL hm = GetClipboardData(CF_TEXT);
  if (!hm) { CloseClipboard(); return NULL; }
  const char *src = (const char *)GlobalLock(hm);
  char *out = src ? strdup(src) : NULL;
  GlobalUnlock(hm);
  CloseClipboard();
  return out;
}

/* Open directory in Explorer */
static void open_dir_safe(const char *dir) {
  if (!dir || !dir[0]) return;
  ShellExecuteA(NULL, "explore", dir, NULL, NULL, SW_SHOWNORMAL);
}

#else
/* ── Linux / macOS ─────────────────────────────────────────── */
#  include <unistd.h>
#  include <sys/ioctl.h>
#  include <termios.h>

#  define TUI_PATH_SEP      ':'
#  define TUI_SLASH         '/'

static inline void tui_usleep(int us) { usleep((useconds_t)us); }
#  define tui_mkdir(p)      mkdir((p), 0755)

static const char *tui_home(void) { return getenv("HOME"); }
static void tui_config_dir(char *out, size_t cap) {
  const char *h = tui_home();
  snprintf(out, cap, "%s/.config/6502caEd", h ? h : ".");
}
static void tui_exe_dir(char *out, size_t cap) {
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  if (n <= 0) { out[0] = 0; return; }
  out[n] = 0;
  char *slash = strrchr(out, '/');
  if (slash) *slash = 0; else out[0] = 0;
}

/* Open directory in system file manager */
static void open_dir_safe(const char *dir) {
  if (!dir || !dir[0]) return;
  pid_t pid = fork();
  if (pid == 0) {
    int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      dup2(devnull, STDOUT_FILENO);
      dup2(devnull, STDERR_FILENO);
      close(devnull);
    }
    setsid();
    execlp("xdg-open", "xdg-open", dir, (char *)NULL);
    _exit(127);
  }
  (void)pid;
}
#endif
/* ───────────────────────────────────────────────────────────────────── */

#include "curses_compat.h"

static volatile sig_atomic_t g_need_resize = 0;
static volatile sig_atomic_t g_quit_request = 0;
static volatile sig_atomic_t g_quit_signal = 0;

static const int g_input_timeout_ms = 50;

/* dialog_getch(win) — replacement for wgetch(win) in all dialog loops.
 *
 * On Linux/macOS, wgetch on a subwindow delivers KEY_MOUSE correctly.
 * On PDCurses WinCon, mouse events only come through stdscr/getch(), not
 * through subwindow wgetch(). This helper polls with a short timeout then
 * falls back to getch() so mouse events are never missed.
 *
 * All dialog while(1) loops should use dialog_getch(win) instead of
 * wgetch(win). Keyboard input is unaffected.
 */
static int dialog_getch(WINDOW *win) {
#ifdef _WIN32
  /* PDCurses WinCon: mouse events only come through stdscr getch(),
   * not wgetch() on sub-windows. Poll with timeout then check stdscr. */
  wtimeout(win, 50);
  int ch = wgetch(win);
  if (ch == ERR) ch = getch();
  if (ch == KEY_RESIZE) g_need_resize = 1;
  return ch;
#else
  return wgetch(win);
#endif
}

// ---------------------------------------------------------------------------
// Prefs
// ---------------------------------------------------------------------------
typedef struct {
  int tab_width;            // spaces per Tab key (1..16)
  int comment_col;          // column for auto-aligned comments (0 = off)
  int show_problems;        // 0=hide bottom error panel, 1=show
  int dark_mode;            // 1=dark palette (default), 0=light palette
  int auto_indent;          // 1=auto-indent on Enter (default), 0=off
  int clear_mem_on_asm;     // 1=clear memory before each assemble (default 1)
  int error_underline;      // 1=underline semantic errors in editor (default 1)
  int keep_comment_aligned; // 1=keep comment column aligned on edit (default 1)
  int allow_undoc;   // 1=allow undocumented opcodes when assembling (default 0)
  int fold_comments; // 1=fold comment blocks (default 0)
  int fold_macros;   // 1=fold macro bodies (default 0)
  int autocomplete;  // 1=show completion popup (default 1)
} prefs_t;

static prefs_t g_prefs = {8, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1};

static void prefs_path(char *out, size_t cap) {
  char dir[1024];
  tui_config_dir(dir, sizeof(dir));
  snprintf(out, cap, "%s/prefs.ini", dir);
}

static void prefs_load(void) {
  char p[1024];
  prefs_path(p, sizeof(p));
  FILE *f = fopen(p, "r");
  if (!f)
    return;
  char line[256];
  while (fgets(line, sizeof(line), f)) {
    char *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq++ = 0;
    // trim key
    char *k = line;
    while (*k == ' ')
      k++;
    char *ke = k + strlen(k);
    while (ke > k && (ke[-1] == '\n' || ke[-1] == '\r' || ke[-1] == ' '))
      *--ke = 0;
    // trim val
    while (*eq == ' ')
      eq++;
    char *ve = eq + strlen(eq);
    while (ve > eq && (ve[-1] == '\n' || ve[-1] == '\r' || ve[-1] == ' '))
      *--ve = 0;
    char *_end;
    long _lv = strtol(eq, &_end, 10);
    int v = (_end != eq && _lv >= INT_MIN && _lv <= INT_MAX) ? (int)_lv : 0;
    if (!strcmp(k, "tab_width"))
      g_prefs.tab_width = (v >= 1 && v <= 16) ? v : 8;
    if (!strcmp(k, "comment_col"))
      g_prefs.comment_col = (v >= 0 && v <= 200) ? v : 0;
    if (!strcmp(k, "show_problems"))
      g_prefs.show_problems = v ? 1 : 0;
    // Key is "light_mode" (not "dark_mode") so old files with dark_mode=0 are
    // ignored. light_mode=1 → user chose light; absent or 0 → dark (the safe
    // default).
    if (!strcmp(k, "light_mode"))
      g_prefs.dark_mode = v ? 0 : 1;
    if (!strcmp(k, "auto_indent"))
      g_prefs.auto_indent = v ? 1 : 0;
    if (!strcmp(k, "clear_mem_on_asm"))
      g_prefs.clear_mem_on_asm = v ? 1 : 0;
    if (!strcmp(k, "error_underline"))
      g_prefs.error_underline = v ? 1 : 0;
    if (!strcmp(k, "keep_comment_aligned"))
      g_prefs.keep_comment_aligned = v ? 1 : 0;
    if (!strcmp(k, "allow_undoc"))
      g_prefs.allow_undoc = v ? 1 : 0;
    if (!strcmp(k, "fold_comments"))
      g_prefs.fold_comments = v ? 1 : 0;
    if (!strcmp(k, "fold_macros"))
      g_prefs.fold_macros = v ? 1 : 0;
    if (!strcmp(k, "autocomplete"))
      g_prefs.autocomplete = v ? 1 : 0;
  }
  fclose(f);
}

/* Forward declaration — defined later; creates ~/.config/6502caEd/ if absent.
 */
static void ensure_config_dir(void); // forward

static void prefs_save(void) {
  ensure_config_dir();
  char p[1024];
  prefs_path(p, sizeof(p));
  FILE *f = fopen(p, "w");
  if (!f)
    return;
  fprintf(f, "tab_width=%d\n", g_prefs.tab_width);
  fprintf(f, "comment_col=%d\n", g_prefs.comment_col);
  fprintf(f, "show_problems=%d\n", g_prefs.show_problems);
  // Store as light_mode so absent key (new installs / old files) → dark
  // default.
  fprintf(f, "light_mode=%d\n", g_prefs.dark_mode ? 0 : 1);
  fprintf(f, "auto_indent=%d\n", g_prefs.auto_indent);
  fprintf(f, "clear_mem_on_asm=%d\n", g_prefs.clear_mem_on_asm);
  fprintf(f, "error_underline=%d\n", g_prefs.error_underline);
  fprintf(f, "keep_comment_aligned=%d\n", g_prefs.keep_comment_aligned);
  fprintf(f, "allow_undoc=%d\n", g_prefs.allow_undoc);
  fprintf(f, "autocomplete=%d\n", g_prefs.autocomplete);
  fprintf(f, "fold_comments=%d\n", g_prefs.fold_comments);
  fprintf(f, "fold_macros=%d\n", g_prefs.fold_macros);
  fclose(f);
}

/* Bookmark persistence: ~/.config/6502caEd/bookmarks.ini
   Format: path=<absolute path>\nline=<1-based>\n... repeated */

// ---------------------------------------------------------------------------
// Selection / clipboard / undo (unchanged from original)
// ---------------------------------------------------------------------------

typedef struct {
  bool active;
  size_t sl, sc; // anchor
  size_t el, ec; // end
} sel_t;

static void sel_begin_at_cursor(sel_t *s, const view_t *v) {
  if (!s || !v)
    return;
  if (s->active)
    return;
  s->active = true;
  s->sl = v->top + (size_t)v->row;
  s->sc = (v->col < 0) ? 0 : (size_t)v->col;
  s->el = s->sl;
  s->ec = s->sc;
}

static void sel_update_end(sel_t *s, const view_t *v) {
  if (!s || !v || !s->active)
    return;
  s->el = v->top + (size_t)v->row;
  s->ec = (v->col < 0) ? 0 : (size_t)v->col;
}

typedef struct {
  size_t sl, sc;
  size_t el, ec;
  char *old_text;
  char *new_text;
} edit_t;

/* Snapshot-based undo/redo using two separate stacks.
 * Each entry is a full copy of all buffer lines.
 * Supports 64 undo levels and 64 redo levels. */
#define UNDO_MAX 64

typedef struct {
  char **lines;
  size_t count;
  view_t cursor;
} snap_t;

typedef struct {
  snap_t undo_stack[UNDO_MAX];
  int undo_top; /* number of valid undo snapshots */
  snap_t redo_stack[UNDO_MAX];
  int redo_top; /* number of valid redo snapshots */
} undo_t;

static void snap_free(snap_t *s) {
  if (!s || !s->lines)
    return;
  for (size_t i = 0; i < s->count; i++)
    free(s->lines[i]);
  free(s->lines);
  s->lines = NULL;
  s->count = 0;
}

static bool snap_copy(snap_t *dst, const buffer_t *buf, const view_t *v) {
  dst->lines = (char **)malloc(buf->count * sizeof(char *));
  if (!dst->lines)
    return false;
  for (size_t i = 0; i < buf->count; i++) {
    dst->lines[i] = strdup(buf->lines[i]);
    if (!dst->lines[i]) {
      for (size_t j = 0; j < i; j++)
        free(dst->lines[j]);
      free(dst->lines);
      dst->lines = NULL;
      return false;
    }
  }
  dst->count = buf->count;
  dst->cursor = *v;
  return true;
}

/* Restore a buffer and view to the state captured in snap src.
 * Frees the current buffer contents before overwriting. */
static void snap_restore(const snap_t *src, buffer_t *buf, view_t *v) {
  for (size_t i = 0; i < buf->count; i++)
    free(buf->lines[i]);
  free(buf->lines);
  buf->lines = (char **)malloc(src->count * sizeof(char *));
  buf->count = src->count;
  for (size_t i = 0; i < src->count; i++)
    buf->lines[i] = strdup(src->lines[i]);
  buf->modified = true;
  *v = src->cursor;
}

/* Initialise an empty undo/redo stack. */
static void undo_init(undo_t *u) { memset(u, 0, sizeof(*u)); }

/* Free all snapshots in the undo and redo stacks. */
static void undo_free(undo_t *u) {
  for (int i = 0; i < u->undo_top; i++)
    snap_free(&u->undo_stack[i]);
  for (int i = 0; i < u->redo_top; i++)
    snap_free(&u->redo_stack[i]);
}

/* Push snapshot before an edit. Clears redo history. */
static void undo_push(undo_t *u, const buffer_t *buf, const view_t *v) {
  /* Clear redo stack */
  for (int i = 0; i < u->redo_top; i++)
    snap_free(&u->redo_stack[i]);
  u->redo_top = 0;
  /* If undo stack is full, drop the oldest entry */
  if (u->undo_top == UNDO_MAX) {
    snap_free(&u->undo_stack[0]);
    memmove(&u->undo_stack[0], &u->undo_stack[1],
            (UNDO_MAX - 1) * sizeof(snap_t));
    u->undo_top--;
  }
  snap_copy(&u->undo_stack[u->undo_top], buf, v);
  u->undo_top++;
}

/* Undo: restore previous state, save current state to redo stack. */
static bool undo_pop(undo_t *u, buffer_t *buf, view_t *v) {
  if (u->undo_top == 0)
    return false;
  /* Save current state to redo stack */
  if (u->redo_top < UNDO_MAX) {
    snap_copy(&u->redo_stack[u->redo_top], buf, v);
    u->redo_top++;
  }
  /* Restore top of undo stack */
  u->undo_top--;
  snap_restore(&u->undo_stack[u->undo_top], buf, v);
  snap_free(&u->undo_stack[u->undo_top]);
  return true;
}

/* Redo: re-apply undone edit, save current state back to undo stack. */
static bool redo_pop(undo_t *u, buffer_t *buf, view_t *v) {
  if (u->redo_top == 0)
    return false;
  /* Save current state to undo stack */
  if (u->undo_top < UNDO_MAX) {
    snap_copy(&u->undo_stack[u->undo_top], buf, v);
    u->undo_top++;
  }
  /* Restore top of redo stack */
  u->redo_top--;
  snap_restore(&u->redo_stack[u->redo_top], buf, v);
  snap_free(&u->redo_stack[u->redo_top]);
  return true;
}

static char *g_clipboard = NULL;

// ---------------------------------------------------------------------------
// System clipboard — platform-aware
// ---------------------------------------------------------------------------

#ifndef _WIN32
static int path_has_exec(const char *exe) {
  if (!exe || !*exe) return 0;
  const char *path = getenv("PATH");
  if (!path) return 0;
  const char *p = path;
  while (*p) {
    const char *sep = strchr(p, TUI_PATH_SEP);
    size_t len = sep ? (size_t)(sep - p) : strlen(p);
    if (len > 0) {
      char buf[512];
      if (len + 1 + strlen(exe) + 1 < sizeof(buf)) {
        memcpy(buf, p, len); buf[len] = 0;
        strcat(buf, "/"); strcat(buf, exe);
        if (access(buf, X_OK) == 0) return 1;
      }
    }
    if (!sep) break;
    p = sep + 1;
  }
  return 0;
}
static int is_wayland_session(void) {
  const char *s = getenv("XDG_SESSION_TYPE");
  if (s && strcmp(s, "wayland") == 0) return 1;
  if (getenv("WAYLAND_DISPLAY")) return 1;
  return 0;
}
static int pipe_write_all(const char *cmd, const char *text) {
  if (!cmd) return 0;
  FILE *f = popen(cmd, "w");
  if (!f) return 0;
  if (text && *text) {
    size_t n = strlen(text);
    if (fwrite(text, 1, n, f) != n) { pclose(f); return 0; }
  }
  return (pclose(f) == 0);
}
static char *pipe_read_all(const char *cmd, size_t max_bytes) {
  if (!cmd) return NULL;
  FILE *f = popen(cmd, "r");
  if (!f) return NULL;
  if (max_bytes < 1024) max_bytes = 1024;
  char *out = (char *)malloc(max_bytes + 1);
  if (!out) { pclose(f); return NULL; }
  size_t n = 0;
  while (!feof(f)) {
    if (n >= max_bytes) break;
    size_t r = fread(out + n, 1, max_bytes - n, f);
    if (r == 0) break;
    n += r;
  }
  out[n] = 0;
  pclose(f);
  return out;
}
#endif /* !_WIN32 */

static int clipboard_set_system(const char *text) {
  free(g_clipboard);
  g_clipboard = text ? strdup(text) : strdup("");
  if (!text) text = "";
#ifdef _WIN32
  return clipboard_win32_set(text);
#else
  int ok = 0;
  if (is_wayland_session() && path_has_exec("wl-copy"))
    ok = pipe_write_all("wl-copy", text);
  else if (path_has_exec("xclip"))
    ok = pipe_write_all("xclip -selection clipboard -in", text);
  else if (path_has_exec("xsel"))
    ok = pipe_write_all("xsel -ib", text);
  return ok;
#endif
}

static char *clipboard_get_system(void) {
#ifdef _WIN32
  char *t = clipboard_win32_get();
  if (!t && g_clipboard) t = strdup(g_clipboard);
  return t;
#else
  char *t = NULL;
  if (is_wayland_session() && path_has_exec("wl-paste"))
    t = pipe_read_all("wl-paste -n", 1024 * 1024);
  else if (path_has_exec("xclip"))
    t = pipe_read_all("xclip -selection clipboard -o", 1024 * 1024);
  else if (path_has_exec("xsel"))
    t = pipe_read_all("xsel -ob", 1024 * 1024);
  if (!t && g_clipboard) t = strdup(g_clipboard);
  return t;
#endif
}

// Forward declaration
static void delete_forward(buffer_t *buf, view_t *v);

static int pos_cmp(size_t al, size_t ac, size_t bl, size_t bc) {
  if (al < bl)
    return -1;
  if (al > bl)
    return 1;
  if (ac < bc)
    return -1;
  if (ac > bc)
    return 1;
  return 0;
}

static void sel_norm(const sel_t *s, size_t *out_sl, size_t *out_sc,
                     size_t *out_el, size_t *out_ec) {
  size_t sl = s->sl, sc = s->sc, el = s->el, ec = s->ec;
  if (pos_cmp(sl, sc, el, ec) > 0) {
    size_t tl = sl, tc = sc;
    sl = el;
    sc = ec;
    el = tl;
    ec = tc;
  }
  *out_sl = sl;
  *out_sc = sc;
  *out_el = el;
  *out_ec = ec;
}

static char *buffer_extract_range(const buffer_t *buf, size_t sl, size_t sc,
                                  size_t el, size_t ec) {
  if (!buf || sl >= buf->count)
    return NULL;
  if (el >= buf->count) {
    el = buf->count - 1;
    ec = strlen(buf->lines[el]);
  }
  size_t cap = 1;
  for (size_t li = sl; li <= el; li++) {
    const char *line = buf->lines[li];
    size_t len = strlen(line);
    size_t a = (li == sl) ? sc : 0;
    size_t b = (li == el) ? ec : len;
    if (a > len)
      a = len;
    if (b > len)
      b = len;
    if (b > a)
      cap += (b - a);
    if (li != el)
      cap += 1;
  }
  char *out = (char *)malloc(cap);
  if (!out)
    return NULL;
  size_t n = 0;
  for (size_t li = sl; li <= el; li++) {
    const char *line = buf->lines[li];
    size_t len = strlen(line);
    size_t a = (li == sl) ? sc : 0;
    size_t b = (li == el) ? ec : len;
    if (a > len)
      a = len;
    if (b > len)
      b = len;
    for (size_t i = a; i < b; i++)
      out[n++] = line[i];
    if (li != el)
      out[n++] = '\n';
  }
  out[n] = 0;
  return out;
}

/* Delete the text from (sl,sc) to (el,ec) in buf (all 0-based).
 * Joins lines if the range spans more than one line. */
static void buffer_delete_range(buffer_t *buf, size_t sl, size_t sc, size_t el,
                                size_t ec) {
  if (!buf || sl >= buf->count)
    return;
  if (pos_cmp(sl, sc, el, ec) >= 0)
    return;
  char *text = buffer_extract_range(buf, sl, sc, el, ec);
  if (!text)
    return;
  view_t tv = {0};
  tv.top = sl;
  tv.row = 0;
  tv.col = (int)sc;
  for (size_t i = 0; text[i]; i++)
    delete_forward(buf, &tv);
  free(text);
}

// ---------------------------------------------------------------------------
// Signal handlers + terminal resize (unchanged)
// ---------------------------------------------------------------------------

static void on_sigwinch(int sig) {
  (void)sig;
  g_need_resize = 1;
}
static void on_sigquit(int sig) {
  g_quit_signal = sig;
  g_quit_request = 1;
}

/* Ctrl+Z is handled as undo in the key dispatch (ch==26).
 * SIGTSTP is ignored so the OS does not intercept Ctrl+Z before
 * wgetch() can return it. */

static void request_starting_term_size(void) {
#ifndef _WIN32
  /* Resize the terminal window to a requested size via escape sequence.
   * Only meaningful on Linux/macOS where TIOCGWINSZ is available. */
  const char *sc = getenv("ED_START_COLS");
  const char *sr = getenv("ED_START_ROWS");
  if (!sc && !sr) return;
  if (!isatty(STDOUT_FILENO)) return;
  int cols = sc ? (int)strtol(sc, NULL, 10) : 0;
  int rows = sr ? (int)strtol(sr, NULL, 10) : 0;
  if (cols <= 0 && rows <= 0) return;
  struct winsize ws;
  int cur_rows = 0, cur_cols = 0;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    cur_rows = ws.ws_row; cur_cols = ws.ws_col;
  }
  if (rows <= 0) rows = cur_rows;
  if (cols <= 0) cols = cur_cols;
  if (rows <= 0 || cols <= 0) return;
  fprintf(stdout, "\033[8;%d;%dt", rows, cols);
  fflush(stdout);
  const int max_wait_ms = 200, step_ms = 10;
  for (int waited = 0; waited < max_wait_ms; waited += step_ms) {
    struct winsize ws2;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws2) == 0) {
      if ((ws2.ws_row > 0 && ws2.ws_col > 0) &&
          (ws2.ws_row != cur_rows || ws2.ws_col != cur_cols))
        break;
    }
    tui_usleep(step_ms * 1000);
  }
#endif /* !_WIN32 */
}

#ifndef REPORT_MOUSE_POSITION
#define REPORT_MOUSE_POSITION 0x08000000UL
#endif
#define ED_MOUSE_MASK                                                          \
  (BUTTON1_CLICKED | BUTTON1_PRESSED | BUTTON1_RELEASED | BUTTON3_PRESSED |    \
   BUTTON3_RELEASED | BUTTON3_CLICKED | REPORT_MOUSE_POSITION)

// ---------------------------------------------------------------------------
// Menu layout — 4 menus with separators
// ---------------------------------------------------------------------------

// Separator sentinel: must not match any real key code
#define MENU_SEP "---"

// File menu items (indices 0..9)
static const char *g_items_file[] = {
    "New\tCtrl+N", "Open\tCtrl+O", "Save\tCtrl+S", "Save As\tCtrl+Shift+S",
    MENU_SEP,      "Check\tF5",    MENU_SEP,       "Recent Files",
    MENU_SEP,      "Quit\tCtrl+Q",  MENU_SEP,      "Assemble\tF9"};
#define N_FILE 12

// Edit menu items (indices 0..11)
static const char *g_items_edit[] = {"Undo\tCtrl+Z",
                                     "Redo\tCtrl+Y",
                                     MENU_SEP,
                                     "Find\tCtrl+F",
                                     "Replace\tCtrl+R",
                                     "Goto Line\tCtrl+G",
                                     MENU_SEP,
                                     "Duplicate Line\tCtrl+D",
                                     "Remove Duplicate Lines",
                                     MENU_SEP,
                                     "Format Layout",
                                     "Uppercase (skip comments)",
                                     "Lowercase (skip comments)",
                                     "Uppercase (incl. comments)",
                                     "Lowercase (incl. comments)",
                                     "Uppercase instructions",
                                     "Lowercase instructions",
                                     "Unicode Replace",
                                     "Comment/Uncomment\tCtrl+K",
                                     "Insert Character...\tF2",
                                     MENU_SEP,
                                     "Find References\tCtrl+F11",
                                     MENU_SEP,
                                     "Toggle Bookmark\tCtrl+B",
                                     MENU_SEP,
                                     "Autocomplete"};
#define N_EDIT 26

// View menu items
static const char *g_items_view[] = {"Toggle Problems Panel\tF8",
                                     "Memory Viewer",
                                     "Includes",
                                     MENU_SEP,
                                     "Next Bookmark\tF4",
                                     "Prev Bookmark\tShift+F4",
                                     "Clear All Bookmarks",
                                     MENU_SEP,
                                     "Tab Width...",
                                     "Comment Column...",
                                     "Toggle Dark/Light",
                                     MENU_SEP,
                                     "Preferences...",
                                     MENU_SEP,
                                     "Config Files..."};
#define N_VIEW 15

// Help menu
static const char *g_items_help[] = {"Contents\tF1",
                                     MENU_SEP,
                                     "Instruction Reference\tF6",
                                     "Directive Reference\tF7",
                                     MENU_SEP,
                                     "About"};
#define N_HELP 6

static const char **menu_items(int idx, int *out_n) {
  if (idx == 0) {
    *out_n = N_FILE;
    return g_items_file;
  }
  if (idx == 1) {
    *out_n = N_EDIT;
    return g_items_edit;
  }
  if (idx == 2) {
    *out_n = N_VIEW;
    return g_items_view;
  }
  *out_n = N_HELP;
  return g_items_help;
}
#define N_MENUS 4
static const char *g_menu_names[N_MENUS] = {"File", "Edit", "View", "Help"};

// Skip separators when navigating Up/Down
static int menu_next_item(int idx, int cur, int dir, int n) {
  const char **items;
  int dummy;
  items = menu_items(idx, &dummy);
  int i = cur;
  for (int tries = 0; tries < n; tries++) {
    i += dir;
    if (i < 0)
      i = n - 1;
    if (i >= n)
      i = 0;
    if (strcmp(items[i], MENU_SEP) != 0)
      return i;
  }
  return cur; // all seps (shouldn't happen)
}

// First non-separator item
static int menu_first_item(int idx) {
  int n;
  const char **items = menu_items(idx, &n);
  for (int i = 0; i < n; i++)
    if (strcmp(items[i], MENU_SEP) != 0)
      return i;
  return 0;
}

// ---------------------------------------------------------------------------
// Menu geometry helpers
// ---------------------------------------------------------------------------
static int menubar_hit_test_x(int x) {
  int curx = 1;
  for (int i = 0; i < N_MENUS; i++) {
    int w = (int)strlen(g_menu_names[i]) + 2;
    if (x >= curx && x < curx + w)
      return i;
    curx += (int)strlen(g_menu_names[i]) + 3;
  }
  return -1;
}

static void menu_window_geom(int menu_idx, int *out_x, int *out_w,
                             int *out_items) {
  int n;
  const char **items = menu_items(menu_idx, &n);
  int wmax = 0;
  for (int i = 0; i < n; i++) {
    int lw = (int)strlen(items[i]);
    if (lw > wmax)
      wmax = lw;
  }
  int win_w = wmax + 4;
  // compute startx matching draw()'s topbar layout
  int startx = 1;
  for (int i = 0; i < menu_idx; i++)
    startx += (int)strlen(g_menu_names[i]) + 3;
  int scr_h, scr_w;
  getmaxyx(stdscr, scr_h, scr_w);
  (void)scr_h;
  if (startx + win_w > scr_w)
    startx = scr_w - win_w;
  if (startx < 0)
    startx = 0;
  if (out_x)
    *out_x = startx;
  if (out_w)
    *out_w = win_w;
  if (out_items)
    *out_items = n;
}

// ---------------------------------------------------------------------------
// tui_handle_resize (unchanged)
// ---------------------------------------------------------------------------
#ifndef ED_BUILD_ID
#define ED_BUILD_ID __DATE__ " " __TIME__
#endif

// Gutter: col 0 = diag mark, cols 1..N = right-aligned line number, col N+1 =
// space Width grows automatically: 1 (mark) + digits(total_lines) + 1 (space)
static int gutter_w(size_t total_lines) {
  int d = 1;
  size_t n = total_lines;
  while (n >= 10) {
    d++;
    n /= 10;
  }
  int base = 1 + d + 1; /* mark + digits + space */
  /* extra column for fold [-]/[+] icon when folding enabled */
  if (g_prefs.fold_comments || g_prefs.fold_macros)
    base += 2;
  return base;
}
#define TOPBAR_H 1
#ifdef _WIN32
/* PDCurses WinCon provides CTL_* directly for Ctrl+arrow keys */
#  define KEY_CTRL_HOME  CTL_HOME
#  define KEY_CTRL_END   CTL_END
#  define KEY_CTRL_LEFT  CTL_LEFT
#  define KEY_CTRL_RIGHT CTL_RIGHT
#  define KEY_CTRL_UP    CTL_UP
#  define KEY_CTRL_DOWN  CTL_DN
#else
#  define KEY_CTRL_HOME  KEY_F(25)
#  define KEY_CTRL_END   KEY_F(26)
#  define KEY_CTRL_LEFT  KEY_F(28)
#  define KEY_CTRL_RIGHT KEY_F(29)
#  define KEY_CTRL_UP    KEY_F(30)
#  define KEY_CTRL_DOWN  KEY_F(31)
#endif
#define KEY_CTRL_F12 KEY_F(32)
#define KEY_SHIFT_F12 KEY_F(33)
#define KEY_SHIFT_F4 KEY_F(16)

typedef struct {
  diagdb_t diags;
  size_t cur_diag_idx;
  bool last_build_ok;
  char last_status[256];
  uint16_t asm_start; // lowest address written by last successful build
  uint16_t asm_end;   // highest address written
  uint32_t asm_bytes; // total bytes emitted
  bool asm_valid;     // true once a successful build has been done
  /* Copy of ranges from last build */
  struct {
    uint16_t start;
    uint16_t end;
    uint32_t count;
  } asm_ranges[ASM_MAX_RANGES];
  int asm_range_count;
  fold_map_t fold; /* per-line fold state */
  /* Bookmarks */
#define MAX_BOOKMARKS 64
  size_t bookmarks[MAX_BOOKMARKS]; /* 0-based line numbers */
  int bookmark_count;
} diag_state_t;

static void bookmarks_path(char *out, size_t cap) {
  char dir[1024];
  tui_config_dir(dir, sizeof(dir));
  snprintf(out, cap, "%s/bookmarks.ini", dir);
}

/* ---------------------------------------------------------------------------
 * Bookmark helpers
 * ------------------------------------------------------------------------- */
static int bookmark_find(const diag_state_t *ds, size_t line) {
  for (int i = 0; i < ds->bookmark_count; i++)
    if (ds->bookmarks[i] == line)
      return i;
  return -1;
}

static void bookmark_toggle(diag_state_t *ds, size_t line) {
  int idx = bookmark_find(ds, line);
  if (idx >= 0) {
    /* remove */
    for (int i = idx; i < ds->bookmark_count - 1; i++)
      ds->bookmarks[i] = ds->bookmarks[i + 1];
    ds->bookmark_count--;
  } else if (ds->bookmark_count < MAX_BOOKMARKS) {
    /* insert keeping sorted order */
    int ins = ds->bookmark_count;
    for (int i = 0; i < ds->bookmark_count; i++) {
      if (ds->bookmarks[i] > line) {
        ins = i;
        break;
      }
    }
    for (int i = ds->bookmark_count; i > ins; i--)
      ds->bookmarks[i] = ds->bookmarks[i - 1];
    ds->bookmarks[ins] = line;
    ds->bookmark_count++;
  }
}

/* Returns next bookmark line after cur_line (wraps), or -1 if none. */
static int bookmark_next(const diag_state_t *ds, size_t cur_line) {
  if (ds->bookmark_count == 0)
    return -1;
  for (int i = 0; i < ds->bookmark_count; i++)
    if (ds->bookmarks[i] > cur_line)
      return (int)ds->bookmarks[i];
  return (int)ds->bookmarks[0]; /* wrap */
}

/* Returns prev bookmark line before cur_line (wraps), or -1 if none. */
static int bookmark_prev(const diag_state_t *ds, size_t cur_line) {
  if (ds->bookmark_count == 0)
    return -1;
  for (int i = ds->bookmark_count - 1; i >= 0; i--)
    if (ds->bookmarks[i] < cur_line)
      return (int)ds->bookmarks[i];
  return (int)ds->bookmarks[ds->bookmark_count - 1]; /* wrap */
}

static void bookmarks_save(const diag_state_t *ds, const char *file_path) {
  if (!file_path || !file_path[0] || ds->bookmark_count == 0) {
    /* Remove stale entry for this file by rewriting without it */
  }
  char bp[1024];
  bookmarks_path(bp, sizeof(bp));

  /* Read existing entries for other files */
  char lines[256][1280];
  int nlines = 0;
  FILE *rf = fopen(bp, "r");
  if (rf) {
    char buf[1280];
    int skip = 0;
    while (fgets(buf, sizeof(buf), rf) && nlines < 254) {
      /* strip newline */
      int bl = (int)strlen(buf);
      while (bl > 0 && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r'))
        buf[--bl] = 0;
      if (strncmp(buf, "path=", 5) == 0) {
        skip = (file_path && strcmp(buf + 5, file_path) == 0) ? 1 : 0;
        if (!skip) {
          snprintf(lines[nlines++], 1280, "%s", buf);
        }
      } else if (strncmp(buf, "line=", 5) == 0) {
        if (!skip)
          snprintf(lines[nlines++], 1280, "%s", buf);
      }
    }
    fclose(rf);
  }

  /* Write back + new entries */
  FILE *wf = fopen(bp, "w");
  if (!wf)
    return;
  for (int i = 0; i < nlines; i++)
    fprintf(wf, "%s\n", lines[i]);
  if (file_path && file_path[0] && ds->bookmark_count > 0) {
    fprintf(wf, "path=%s\n", file_path);
    for (int i = 0; i < ds->bookmark_count; i++)
      fprintf(wf, "line=%zu\n", ds->bookmarks[i] + 1); /* 1-based */
  }
  fclose(wf);
}

static void bookmarks_load(diag_state_t *ds, const char *file_path) {
  if (!file_path || !file_path[0])
    return;
  ds->bookmark_count = 0;
  char bp[1024];
  bookmarks_path(bp, sizeof(bp));
  FILE *f = fopen(bp, "r");
  if (!f)
    return;
  char buf[1280];
  int in_file = 0;
  while (fgets(buf, sizeof(buf), f)) {
    int bl = (int)strlen(buf);
    while (bl > 0 && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r'))
      buf[--bl] = 0;
    if (strncmp(buf, "path=", 5) == 0) {
      in_file = (strcmp(buf + 5, file_path) == 0) ? 1 : 0;
    } else if (in_file && strncmp(buf, "line=", 5) == 0) {
      char *_bend;
      long _lln = strtol(buf + 5, &_bend, 10);
      size_t ln = (_bend != buf + 5 && _lln > 0) ? (size_t)_lln : 0;
      if (ln > 0 && ds->bookmark_count < MAX_BOOKMARKS)
        ds->bookmarks[ds->bookmark_count++] = ln - 1; /* 0-based */
    }
  }
  fclose(f);
}

typedef struct {
  bool active;
  int menu_idx;
  int item_idx;
  bool opened_by_press; // true = opened by BUTTON1_PRESSED; swallow the
                        // matching RELEASED on the title
} menu_state_t;

/* Right-click context menu state — driven inline in the main loop */
typedef struct {
  bool active;
  int sel;      /* currently highlighted item */
  int origin_x; /* screen col where menu is drawn */
  int origin_y; /* screen row where menu is drawn */
  int width;
  int nitems;
} ctx_state_t;

static const char *g_ctx_items[] = {
    "Uppercase (skip comments)",
    "Lowercase (skip comments)",
    "Uppercase (incl. comments)",
    "Lowercase (incl. comments)",
    "Uppercase instructions only",
    "Lowercase instructions only",
    "Toggle Bookmark",
};
#define N_CTX_ITEMS 7

static void tui_handle_resize(void) {
  int new_h = 0, new_w = 0;
#ifdef _WIN32
  /* PDCurses updates LINES/COLS automatically on KEY_RESIZE. */
  new_h = LINES; new_w = COLS;
#else
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
    new_h = ws.ws_row; new_w = ws.ws_col;
  }
#endif
  if (new_h <= 0 || new_w <= 0)
    resizeterm(0, 0);
  else if (is_term_resized(new_h, new_w))
    resizeterm(new_h, new_w);
  clearok(stdscr, true);
  endwin();
  refresh();
  keypad(stdscr, true);
  mousemask(ED_MOUSE_MASK, NULL);
  mouseinterval(0);
#ifndef _WIN32
  printf("\033[?1003h");  /* xterm any-event mouse tracking — Linux/macOS only */
  fflush(stdout);
#endif
  timeout(g_input_timeout_ms);
  erase();
  refresh();
}

/* Clamp the cursor so it stays within the buffer and the visible text area.
 * in:  text_h — visible height in rows
 *      w_text — visible width in columns */
static void clamp_cursor(const buffer_t *buf, view_t *v, int text_h,
                         int w_text) {
  size_t line = v->top + (size_t)v->row;
  if (buf->count == 0) {
    v->row = 0;
    v->col = 0;
    v->left = 0;
    return;
  }
  if (line >= buf->count) {
    v->top = buf->count - 1;
    v->row = 0;
    line = v->top;
  }
  if (v->row < 0)
    v->row = 0;
  if (v->row >= text_h)
    v->row = text_h - 1;
  if (v->row < 0)
    v->row = 0;
  const char *s = buffer_get_line(buf, line);
  int len = (int)strlen(s);
  if (v->col < 0)
    v->col = 0;
  if (v->col > len)
    v->col = len;
  if ((size_t)v->col < v->left)
    v->left = (size_t)v->col;
  if (v->col - (int)v->left >= w_text)
    v->left = (size_t)(v->col - (w_text - 1));
}

/* Scroll the viewport so the cursor row is visible.
 * Keeps the cursor within a few lines of the top/bottom for context. */
static void ensure_cursor_visible_vertical(const buffer_t *buf, view_t *v,
                                           int text_h) {
  if (buf->count == 0) {
    v->top = 0;
    v->row = 0;
    return;
  }
  if (v->row < 0) {
    size_t up = (size_t)(-v->row);
    v->top = (v->top >= up) ? v->top - up : 0;
    v->row = 0;
  } else if (v->row >= text_h) {
    size_t down = (size_t)(v->row - (text_h - 1));
    v->top += down;
    v->row = text_h - 1;
  }
  if (buf->count > 0) {
    size_t max_top =
        (buf->count > (size_t)text_h) ? (buf->count - (size_t)text_h) : 0;
    if (v->top > max_top)
      v->top = max_top;
  }
}

/* Set the status bar message using a printf-style format string. */
static void status_set(diag_state_t *ds, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(ds->last_status, sizeof(ds->last_status), fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------------------
// draw() — updated for 4-menu bar + problems panel toggle
// ---------------------------------------------------------------------------
static void draw(const char *path, const buffer_t *buf, view_t *v,
                 diag_state_t *ds, const theme_t *theme, const symdb_t *symdb,
                 const menu_state_t *ms, const ctx_state_t *ctx,
                 size_t last_edit_line) {
  int h, w;
  getmaxyx(stdscr, h, w);
  // Reserve bottom row for status; optionally a problems row above it
  int prob_h = (g_prefs.show_problems && ds->diags.count > 0) ? 1 : 0;
  int status_rows = 1 + prob_h;
  int text_h = h - status_rows - TOPBAR_H;
  if (text_h < 1)
    text_h = 1;
  int gw = gutter_w(buf->count ? buf->count : 1);
  int w_text = (w > gw) ? (w - gw) : 0;

  clamp_cursor(buf, v, text_h, w_text);
  bkgd((unsigned)theme_attr(theme, TK_DEFAULT, 0));
  erase();

  if ((ms && ms->active) || (ctx && ctx->active))
    curs_set(0);
  else
    curs_set(1);

  // ---- top bar ----
  attrset(theme_attr(theme, TK_DEFAULT, 0));
  attron(A_REVERSE);
  mvhline(0, 0, ' ', w);

  int x = 1;
  for (int i = 0; i < N_MENUS; i++) {
    if (ms && ms->active && ms->menu_idx == i)
      attron(A_BOLD);
    mvprintw(0, x, " %s ", g_menu_names[i]);
    if (ms && ms->active && ms->menu_idx == i)
      attroff(A_BOLD);
    x += (int)strlen(g_menu_names[i]) + 3;
  }

  attroff(A_REVERSE);

  // ---- text area ----
  {
    bool folding = (g_prefs.fold_comments || g_prefs.fold_macros);
    int fold_col = folding ? (gw - 3) : -1; /* column of fold icon */
    bool num_w_adj = folding; /* line-number field is 2 chars narrower */
    (void)num_w_adj;
    /* visual row counter — maps display rows to buffer lines via fold map */
    int r = 0;
    size_t buf_row = v->top; /* buffer line for first visible row */
    /* skip hidden lines at top */
    while (buf_row < buf->count && ds->fold.lines && buf_row < ds->fold.count) {
      fold_line_t *fl = &ds->fold.lines[buf_row];
      bool hidden = false;
      if (fl->kind == FL_FOLD_BODY || fl->kind == FL_FOLD_END) {
        size_t s = buf_row;
        while (s > 0 && ds->fold.lines[s].kind != FL_FOLD_START)
          s--;
        if (ds->fold.lines[s].collapsed)
          hidden = true;
      }
      if (!hidden)
        break;
      buf_row++;
    }
    while (r < text_h && buf_row < buf->count) {
      size_t li = buf_row;
      /* skip hidden (folded body) lines */
      if (ds->fold.lines && li < ds->fold.count) {
        fold_line_t *fl = &ds->fold.lines[li];
        bool hidden = false;
        if (fl->kind == FL_FOLD_BODY || fl->kind == FL_FOLD_END) {
          size_t s = li;
          while (s > 0 && ds->fold.lines[s].kind != FL_FOLD_START)
            s--;
          if (ds->fold.lines[s].collapsed)
            hidden = true;
        }
        if (hidden) {
          buf_row++;
          continue;
        }
      }
      const char *line = buffer_get_line(buf, li);
      int is_cur_line = ((int)(li - v->top) == v->row);
      int suppress_errs = (li == last_edit_line);
      const diag_item_t *di = suppress_errs
          ? NULL : diagdb_first_on_line(&ds->diags, li + 1);
      char dmark = ' ';
      if (di)
        dmark = (di->kind == DIAG_ERROR) ? '!' : '~';
      if (di) {
        token_kind_t mk =
            (di->kind == DIAG_ERROR) ? TK_DIAG_ERROR : TK_DIAG_WARNING;
        attrset(theme_attr(theme, mk, 0));
        mvaddch(r + TOPBAR_H, 0, dmark);
      } else if (bookmark_find(ds, li) >= 0) {
        attrset(theme_attr(theme, TK_MNEMONIC, 0));
        mvaddch(r + TOPBAR_H, 0, 'B');
      } else {
        attrset(theme_attr(theme, TK_DEFAULT, 0));
        mvaddch(r + TOPBAR_H, 0, ' ');
      }
      /* Line number */
      {
        int num_w = folding ? (gw - 4) : (gw - 2);
        if (num_w < 1)
          num_w = 1;
        size_t lnum = li + 1;
        if (is_cur_line)
          attrset(theme_attr(theme, TK_LABEL, 0) | A_BOLD);
        else
          attrset(theme_attr(theme, TK_COMMENT, 0));
        mvprintw(r + TOPBAR_H, 1, "%*zu", num_w, lnum);
        attrset(theme_attr(theme, TK_DEFAULT, 0));
        /* fold icon column */
        if (folding && fold_col >= 0) {
          fold_line_t *fl = (ds->fold.lines && li < ds->fold.count)
                                ? &ds->fold.lines[li]
                                : NULL;
          char icon = ' ';
          if (fl && fl->kind == FL_FOLD_START)
            icon = fl->collapsed ? '+' : '-';
          attrset(theme_attr(theme, TK_OPERATOR, 0));
          mvaddch(r + TOPBAR_H, fold_col, '[');
          mvaddch(r + TOPBAR_H, fold_col + 1, icon);
          mvaddch(r + TOPBAR_H, fold_col + 2, ']');
          attrset(theme_attr(theme, TK_DEFAULT, 0));
        } else {
          mvaddch(r + TOPBAR_H, gw - 1, ' ');
        }
      }
      /* Content: if collapsed fold start, show summary instead */
      if (ds->fold.lines && li < ds->fold.count &&
          ds->fold.lines[li].kind == FL_FOLD_START &&
          ds->fold.lines[li].collapsed) {
        char summary[128];
        int hidden_n = ds->fold.lines[li].group_size - 1;
        const char *type = ds->fold.lines[li].is_macro ? "macro" : "...";
        snprintf(summary, sizeof(summary), "%s  [%d lines]", type, hidden_n);
        attrset(theme_attr(theme, TK_COMMENT, 0));
        mvaddnstr(r + TOPBAR_H, gw, summary, w_text);
        attrset(theme_attr(theme, TK_DEFAULT, 0));
      } else {
        size_t len = strlen(line);
        if (v->left < len && w_text > 0)
          lexer_render_line(r + TOPBAR_H, gw, line, v->left, w_text, theme, 0,
                            symdb, li, suppress_errs);
      }
      r++;
      buf_row++;
    }
  }

  // ---- problems panel (if enabled and there are diags) ----
  if (prob_h > 0) {
    attrset(theme_attr(theme, TK_DIAG_ERROR, 0));
    attron(A_REVERSE);
    int row = h - 1 - prob_h;
    char pbuf[512];
    size_t idx = ds->cur_diag_idx;
    if (idx == (size_t)-1)
      idx = 0;
    if (idx < ds->diags.count) {
      const diag_item_t *di = &ds->diags.items[idx];
      snprintf(pbuf, sizeof(pbuf), " [%zu/%zu] %s %zu:%zu  %s ", idx + 1,
               ds->diags.count, (di->kind == DIAG_ERROR) ? "ERR" : "WRN",
               di->line1, di->col1, di->message ? di->message : "");
    } else {
      snprintf(pbuf, sizeof(pbuf), " No diagnostics ");
    }
    mvhline(row, 0, ' ', w);
    mvaddnstr(row, 0, pbuf, w);
    attroff(A_REVERSE);
  }

  // ---- status bar ----
  attrset(theme_attr(theme, TK_DEFAULT, 0));
  attron(A_REVERSE);
  size_t cur_line1 = v->top + (size_t)v->row + 1;
  /* Don't show the error for the line currently being edited — it may be
     mid-keystroke and not yet valid. It will reappear once the cursor moves. */
  const diag_item_t *cur_di = diagdb_first_on_line(&ds->diags, cur_line1);
  /* Suppress error in status bar while the cursor is on the edited line. */
  if (cur_di && last_edit_line == cur_line1 - 1) cur_di = NULL;
  char status[1024];
  const char *shown_path = (path && path[0]) ? path : "(no file)";
  bool has_path = (path && path[0]);
  const char *base = shown_path;
  for (const char *t = shown_path; *t; t++)
    if (*t == '/')
      base = t + 1;

  if (ds->last_status[0]) {
    if (cur_di && cur_di->message)
      snprintf(status, sizeof(status), " %s | %s%s | %zu:%d | %s ",
               ds->last_status, base, (has_path && buf->modified) ? " [+]" : "",
               cur_line1, v->col + 1, cur_di->message);
    else
      snprintf(status, sizeof(status), " %s | %s%s | %zu:%d ", ds->last_status,
               base, (has_path && buf->modified) ? " [+]" : "", cur_line1,
               v->col + 1);
  } else {
    if (cur_di && cur_di->message)
      snprintf(status, sizeof(status), " %s%s | " ED_BUILD_ID " | %zu:%d | %s ",
               base, (has_path && buf->modified) ? " [+]" : "", cur_line1,
               v->col + 1, cur_di->message);
    else
      snprintf(status, sizeof(status), " %s%s | " ED_BUILD_ID " | %zu:%d ",
               base, (has_path && buf->modified) ? " [+]" : "", cur_line1,
               v->col + 1);
  }
  mvhline(h - 1, 0, ' ', w);
  mvaddnstr(h - 1, 0, status, w);
  attroff(A_REVERSE);
  attrset(A_NORMAL);

  // cursor
  int cur_y = v->row + TOPBAR_H;
  int cur_x = gw + (v->col - (int)v->left);
  if (cur_x < gw)
    cur_x = gw;
  if (cur_x >= w)
    cur_x = w - 1;
  if (cur_y < 0)
    cur_y = 0;
  if (cur_y >= text_h + TOPBAR_H)
    cur_y = text_h + TOPBAR_H - 1;
  move(cur_y, cur_x);

  wnoutrefresh(stdscr);

  // ---- dropdown menu overlay ----
  if (ms && ms->active) {
    int n;
    const char **items = menu_items(ms->menu_idx, &n);
    int wmax = 0;
    for (int i = 0; i < n; i++) {
      if (strcmp(items[i], MENU_SEP) == 0)
        continue;
      const char *tab = strchr(items[i], '\t');
      int lw;
      if (tab) {
        /* label + 2 gap + shortcut */
        lw = (int)(tab - items[i]) + 2 + (int)strlen(tab + 1);
      } else {
        lw = (int)strlen(items[i]);
      }
      if (lw > wmax)
        wmax = lw;
    }
    int win_w = wmax + 4;
    int win_h = n + 2;
    int startx = 1;
    for (int i = 0; i < ms->menu_idx; i++)
      startx += (int)strlen(g_menu_names[i]) + 3;
    if (startx + win_w > w)
      startx = w - win_w;
    if (startx < 0)
      startx = 0;
    WINDOW *mw = newwin(win_h, win_w, 1, startx);
    wbkgd(mw, theme_attr(theme, TK_DEFAULT, 0));
    box(mw, 0, 0);
    for (int i = 0; i < n; i++) {
      if (strcmp(items[i], MENU_SEP) == 0) {
        mvwhline(mw, 1 + i, 1, ACS_HLINE, win_w - 2);
      } else {
        if (i == ms->item_idx)
          wattron(mw, A_REVERSE);
        /* Split "Label\tShortcut" and right-align the shortcut */
        const char *tab = strchr(items[i], '\t');
        if (tab) {
          int llen = (int)(tab - items[i]);
          int slen = (int)strlen(tab + 1);
          int gap = (win_w - 2) - llen - slen;
          if (gap < 1)
            gap = 1;
          mvwprintw(mw, 1 + i, 1, "%.*s%*s%s", llen, items[i], gap, "",
                    tab + 1);
        } else {
          mvwprintw(mw, 1 + i, 1, "%-*s", win_w - 2, items[i]);
        }
        if (i == ms->item_idx)
          wattroff(mw, A_REVERSE);
      }
    }
    wnoutrefresh(mw);
    delwin(mw);
  }

  /* Draw right-click context menu overlay */
  if (ctx && ctx->active) {
    int ox = ctx->origin_x, oy = ctx->origin_y;
    int cw = ctx->width, cn = ctx->nitems;
    int h2, w2;
    getmaxyx(stdscr, h2, w2);
    /* Clamp to screen */
    if (ox + cw >= w2)
      ox = w2 - cw - 1;
    if (oy + cn + 2 >= h2)
      oy = h2 - cn - 2;
    if (ox < 0)
      ox = 0;
    if (oy < 1)
      oy = 1;
    WINDOW *cw2 = newwin(cn + 2, cw, oy, ox);
    if (cw2) {
      wbkgd(cw2, theme_attr(theme, TK_DEFAULT, 0));
      box(cw2, 0, 0);
      for (int i = 0; i < cn; i++) {
        /* Dim "Toggle Bookmark" when at max and line not already bookmarked */
        int bm_idx = N_CTX_ITEMS - 1; /* last item */
        bool bm_disabled = (i == bm_idx) &&
                           (ds->bookmark_count >= MAX_BOOKMARKS) &&
                           (bookmark_find(ds, v->top + (size_t)v->row) < 0);
        if (bm_disabled) {
          wattron(cw2, theme_attr(theme, TK_COMMENT, 0));
          mvwprintw(cw2, 1 + i, 1, "%-*s", cw - 2, g_ctx_items[i]);
          wattroff(cw2, theme_attr(theme, TK_COMMENT, 0));
        } else {
          if (i == ctx->sel)
            wattron(cw2, A_REVERSE);
          mvwprintw(cw2, 1 + i, 1, "%-*s", cw - 2, g_ctx_items[i]);
          if (i == ctx->sel)
            wattroff(cw2, A_REVERSE);
        }
      }
      wnoutrefresh(cw2);
      delwin(cw2);
    }
  }

  doupdate();
}

// ---------------------------------------------------------------------------
// Low-level edit primitives (unchanged from original)
// ---------------------------------------------------------------------------

static void insert_char(buffer_t *buf, size_t line_idx, int col, int ch) {
  if (line_idx >= buf->count)
    return;
  char *s = buf->lines[line_idx];
  size_t len = strlen(s);
  if (col < 0)
    col = 0;
  if ((size_t)col > len)
    col = (int)len;
  char *ns = (char *)realloc(s, len + 2);
  if (!ns)
    return;
  s = ns;
  memmove(s + col + 1, s + col, len - (size_t)col + 1);
  s[col] = (char)ch;
  buf->lines[line_idx] = s;
  buf->modified = true;
}

/* Delete the character before the cursor, or join with the previous line
 * if the cursor is at column 0.  Updates the view cursor position. */
static void backspace(buffer_t *buf, view_t *v) {
  size_t line_idx = v->top + (size_t)v->row;
  if (line_idx >= buf->count)
    return;
  if (v->col > 0) {
    char *s = buf->lines[line_idx];
    size_t len = strlen(s);
    size_t pos = (size_t)(v->col - 1);
    if (pos < len) {
      memmove(s + pos, s + pos + 1, len - pos);
      buf->modified = true;
      v->col--;
    }
    return;
  }
  if (line_idx == 0)
    return;
  char *cur = buf->lines[line_idx];
  char *prev = buf->lines[line_idx - 1];
  size_t prev_len = strlen(prev), cur_len = strlen(cur);
  if (prev_len > SIZE_MAX - cur_len - 1)
    return; /* overflow guard */
  char *nprev = (char *)realloc(prev, prev_len + cur_len + 1);
  if (!nprev)
    return;
  prev = nprev;
  memcpy(prev + prev_len, cur, cur_len + 1);
  free(cur);
  buf->lines[line_idx - 1] = prev;
  memmove(&buf->lines[line_idx], &buf->lines[line_idx + 1],
          (buf->count - line_idx - 1) * sizeof(char *));
  buf->count--;
  buf->modified = true;
  if (v->row > 0)
    v->row--;
  else if (v->top > 0)
    v->top--;
  v->col = (int)prev_len;
}

/* Delete the character under the cursor, or join with the next line
 * if the cursor is at the end of the line. */
static void delete_forward(buffer_t *buf, view_t *v) {
  size_t line_idx = v->top + (size_t)v->row;
  if (line_idx >= buf->count)
    return;
  size_t len = strlen(buf->lines[line_idx]);
  if ((size_t)v->col < len) {
    view_t tv = *v;
    tv.col = v->col + 1;
    backspace(buf, &tv);
    return;
  }
  if (line_idx + 1 < buf->count) {
    view_t tv = *v;
    if (tv.row + 1 < (int)buf->count)
      tv.row += 1;
    else
      tv.top += 1;
    tv.col = 0;
    backspace(buf, &tv);
  }
}

/* Split the current line at the cursor, inserting a new line below
 * and moving the cursor to the start of it. */
static void insert_newline(buffer_t *buf, view_t *v) {
  size_t line_idx = v->top + (size_t)v->row;
  if (line_idx >= buf->count)
    return;
  char *s = buf->lines[line_idx];
  size_t len = strlen(s);
  size_t pos = (size_t)v->col;
  if (pos > len)
    pos = len;
  char *left = (char *)malloc(pos + 1);
  char *right = (char *)malloc(len - pos + 1);
  if (!left || !right) {
    free(left);
    free(right);
    return;
  }
  memcpy(left, s, pos);
  left[pos] = 0;
  memcpy(right, s + pos, len - pos + 1);

  // Auto-indent (only if pref enabled)
  char indent[64];
  indent[0] = 0;
  if (g_prefs.auto_indent) {
    size_t lead = 0;
    while (lead < pos && (left[lead] == ' ' || left[lead] == '\t'))
      lead++;
    if (pos <= lead) {
      size_t n = pos < sizeof(indent) - 1 ? pos : sizeof(indent) - 1;
      memcpy(indent, left, n);
      indent[n] = 0;
    } else {
      size_t n = lead < sizeof(indent) - 1 ? lead : sizeof(indent) - 1;
      memcpy(indent, left, n);
      indent[n] = 0;
    }
    {
      const char *s0 = buf->lines[line_idx];
      size_t i = 0;
      while (s0[i] == ' ' || s0[i] == '\t')
        i++;
      if (i == 0) {
        if (s0[0] == '.' && ((s0[1] >= 'A' && s0[1] <= 'Z') ||
                             (s0[1] >= 'a' && s0[1] <= 'z') || s0[1] == '_')) {
          char w[16];
          size_t j = 0;
          while (j < sizeof(w) - 1 && s0[j] && s0[j] != ' ' && s0[j] != '\t') {
            w[j] = (char)toupper((unsigned char)s0[j]);
            j++;
          }
          w[j] = 0;
          if (strcmp(w, ".DEF") && strcmp(w, ".DEFINE") && strcmp(w, ".IF") &&
              strcmp(w, ".IFDEF") && strcmp(w, ".IFNDEF") &&
              strcmp(w, ".ELSE") && strcmp(w, ".ELIF") &&
              strcmp(w, ".ELSEIF") && strcmp(w, ".ENDIF") &&
              strcmp(w, ".ASSERT") && strcmp(w, ".WARNING") &&
              strcmp(w, ".ERROR") && strcmp(w, ".ORG") && strcmp(w, ".BYTE") &&
              strcmp(w, ".WORD") && strcmp(w, ".DWORD") &&
              strcmp(w, ".INCLUDE") && strcmp(w, ".OPT"))
            memcpy(indent, "        ", 9);  /* 8 spaces + NUL */
        } else {
          size_t j = 0;
          while (s0[j] && (isalnum((unsigned char)s0[j]) || s0[j] == '_'))
            j++;
          if (j > 0 && s0[j] == ':')
            memcpy(indent, "        ", 9);  /* 8 spaces + NUL */
        }
      }
    }
    if (indent[0]) {
      size_t il = strlen(indent), rl = strlen(right);
      char *nr = (char *)malloc(il + rl + 1);
      if (nr) {
        memcpy(nr, indent, il);
        memcpy(nr + il, right, rl + 1);
        free(right);
        right = nr;
      }
    }
  } // end auto_indent
  free(s);
  buf->lines[line_idx] = left;
  char **newp = (char **)realloc(buf->lines, (buf->count + 1) * sizeof(char *));
  if (!newp) {
    free(right);
    return;
  }
  buf->lines = newp;
  memmove(&buf->lines[line_idx + 2], &buf->lines[line_idx + 1],
          (buf->count - line_idx - 1) * sizeof(char *));
  buf->lines[line_idx + 1] = right;
  buf->count++;
  buf->modified = true;
  int hh, ww;
  getmaxyx(stdscr, hh, ww);
  (void)ww;
  int text_h = hh - 1 - TOPBAR_H;
  if (v->row < text_h - 1)
    v->row++;
  else
    v->top++;
  v->col = (int)strlen(indent);
}

// ---------------------------------------------------------------------------
// Diagnostics jump + duplicate-def helpers (unchanged)
// ---------------------------------------------------------------------------

static void jump_to_diag(buffer_t *buf, view_t *v, const diag_item_t *it) {
  if (!it)
    return;
  size_t target_line = (it->line1 > 0) ? (it->line1 - 1) : 0;
  int h, w;
  getmaxyx(stdscr, h, w);
  (void)w;
  int text_h = h - 1 - TOPBAR_H;
  if (buf->count == 0)
    return;
  if (target_line >= buf->count)
    target_line = buf->count - 1;
  size_t center = (size_t)(text_h / 2);
  v->top = (target_line > center) ? (target_line - center) : 0;
  size_t max_top =
      (buf->count > (size_t)text_h) ? (buf->count - (size_t)text_h) : 0;
  if (v->top > max_top)
    v->top = max_top;
  v->row = (int)(target_line - v->top);
  if (v->row < 0)
    v->row = 0;
  if (v->row >= text_h)
    v->row = text_h - 1;
  v->col = (int)((it->col1 > 0) ? (it->col1 - 1) : 0);
  v->left = (size_t)((v->col > 10) ? (v->col - 10) : 0);
  clamp_cursor(buf, v, text_h,
               (w > gutter_w(buf->count ? buf->count : 1))
                   ? (w - gutter_w(buf->count ? buf->count : 1))
                   : 0);
}

/* Scan symdb for duplicate symbol definitions and add a DIAG_ERROR
 * entry to db for each one, so they appear in the problems panel. */
static void add_duplicate_def_diags(const symdb_t *symdb, diagdb_t *db) {
  if (!symdb || !db)
    return;
  for (size_t li = 0; li < symdb->line_cap; li++) {
    size_t s = 0, e = 0;
    bool dup = false;
    if (symdb_line_def(symdb, li, &s, &e, &dup) && dup)
      diagdb_add(db, li + 1, (int)s + 1, DIAG_ERROR, "duplicate definition");
  }
}

// ---------------------------------------------------------------------------
// Utility
// ---------------------------------------------------------------------------

static void set_term_title(const char *app, const char *file) {
  if (!app)
    app = "6502caEd";
  if (!file || !file[0])
    file = "(untitled)";
  printf("\033]0;%s - %s\007", app, file);
  fflush(stdout);
}

static int file_exists(const char *path) {
  struct stat st;
  return path && path[0] && stat(path, &st) == 0;
}

static int has_suffix(const char *path) {
  if (!path)
    return 0;
  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base, '.');
  return (dot && dot[1] != 0);
}

/* Append ".asm" to path if it has no file extension already. */
static void ensure_asm_suffix(char *path, size_t cap) {
  if (!path || !path[0])
    return;
  if (has_suffix(path))
    return;
  size_t n = strlen(path);
  if (n + 4 < cap)
    strcat(path, ".asm");
}

/* Create the config directory if it does not already exist. */
static void ensure_config_dir(void) {
#ifdef _WIN32
  const char *appdata = getenv("APPDATA");
  if (!appdata) return;
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s/6502caEd", appdata);
  tui_mkdir(dir);
#else
  const char *home = tui_home();
  if (!home) return;
  char dir[1024];
  snprintf(dir, sizeof(dir), "%s/.config", home);       tui_mkdir(dir);
  snprintf(dir, sizeof(dir), "%s/.config/6502caEd", home); tui_mkdir(dir);
#endif
}

// ---------------------------------------------------------------------------
// Jump history buffer
// ---------------------------------------------------------------------------
#define JUMP_BUF_SIZE 10
typedef struct {
  size_t line;
  int col;
} jump_pos_t;
typedef struct {
  jump_pos_t slots[JUMP_BUF_SIZE];
  int count;
  int cur; // index of current position; -1 = empty
} jump_buf_t;

static void jump_buf_push(jump_buf_t *jb, size_t line, int col) {
  // Discard forward history
  if (jb->cur >= 0 && jb->cur < jb->count - 1)
    jb->count = jb->cur + 1;
  if (jb->count == JUMP_BUF_SIZE) {
    memmove(&jb->slots[0], &jb->slots[1],
            (JUMP_BUF_SIZE - 1) * sizeof(jump_pos_t));
    jb->count--;
    if (jb->cur > 0)
      jb->cur--;
  }
  jb->slots[jb->count++] = (jump_pos_t){line, col};
  jb->cur = jb->count - 1;
}

static int jump_buf_back(jump_buf_t *jb, size_t *ol, int *oc) {
  if (jb->cur <= 0 || jb->count == 0)
    return 0;
  jb->cur--;
  *ol = jb->slots[jb->cur].line;
  *oc = jb->slots[jb->cur].col;
  return 1;
}

/* Move forward in the jump history and write the destination to *ol/*oc.
 * ret: 1 if a forward entry exists, 0 if already at the tip */
static int jump_buf_fwd(jump_buf_t *jb, size_t *ol, int *oc) {
  if (jb->cur < 0 || jb->cur >= jb->count - 1)
    return 0;
  jb->cur++;
  *ol = jb->slots[jb->cur].line;
  *oc = jb->slots[jb->cur].col;
  return 1;
}

/* Adjust stored jump positions when lines are inserted (delta>0) or
 * deleted (delta<0) at at_line, so history stays aligned with the buffer. */
static void jump_buf_adjust(jump_buf_t *jb, size_t at_line, int delta) {
  for (int i = 0; i < jb->count; i++) {
    if (delta < 0) {
      size_t del_end = at_line + (size_t)(-delta);
      if (jb->slots[i].line >= at_line && jb->slots[i].line < del_end) {
        memmove(&jb->slots[i], &jb->slots[i + 1],
                (jb->count - i - 1) * sizeof(jump_pos_t));
        jb->count--;
        if (jb->cur >= i && jb->cur > 0)
          jb->cur--;
        i--;
      } else if (jb->slots[i].line >= del_end) {
        jb->slots[i].line = (size_t)((int)jb->slots[i].line + delta);
      }
    } else {
      if (jb->slots[i].line >= at_line)
        jb->slots[i].line += (size_t)delta;
    }
  }
}

// ---------------------------------------------------------------------------
// UI widgets: prompt_string, dialog_yesno, dialog_ok, dialog_save_changes
// (unchanged from original, consolidated here)
// ---------------------------------------------------------------------------

static void status_prompt_bar(const theme_t *theme, const char *prompt) {
  int h, w;
  getmaxyx(stdscr, h, w);
  attrset(theme_attr(theme, TK_DEFAULT, 0));
  attron(A_REVERSE);
  mvhline(h - 1, 0, ' ', w);
  mvaddnstr(h - 1, 0, prompt, w);
  attroff(A_REVERSE);
  refresh();
}

static int prompt_string(const theme_t *theme, const char *prompt, char *out,
                         size_t out_cap, const char *defval) {
  if (!out || out_cap == 0)
    return 0;
  out[0] = 0;
  char line[1024];
  if (defval && defval[0])
    snprintf(line, sizeof(line), "%s [%s]: ", prompt, defval);
  else
    snprintf(line, sizeof(line), "%s: ", prompt);
  size_t len = 0;
  curs_set(1);
  noecho();
  keypad(stdscr, true);
  while (1) {
    char bar[1200];
    snprintf(bar, sizeof(bar), "%s%s", line, out);
    status_prompt_bar(theme, bar);
    int ch = getch();
    if (ch == 27 || ch == KEY_EXIT) {
      flushinp();
      out[0] = 0;
      return 0;
    }
    if (ch == '\n' || ch == '\r')
      break;
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (len) {
        len--;
        out[len] = 0;
      }
      continue;
    }
    if (ch == KEY_DC)
      continue;
    if (ch >= 32 && ch <= 126) {
      if (len + 1 < out_cap) {
        out[len++] = (char)ch;
        out[len] = 0;
      }
      continue;
    }
  }
  if (out[0] == 0 && defval && defval[0]) {
    strncpy(out, defval, out_cap - 1);
    out[out_cap - 1] = 0;
  }
  return out[0] != 0;
}

/* Display a centred Yes/No dialog and wait for a keypress.
 * ret: 1 for Yes, 0 for No */
static int dialog_yesno(const theme_t *theme, const char *title,
                        const char *msg) {
  curs_set(0);
  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  if (!title)
    title = "";
  if (!msg)
    msg = "";
  int msg_w = (int)strlen(msg), title_w = (int)strlen(title);
  int win_w = msg_w;
  if (title_w > win_w)
    win_w = title_w;
  if (win_w < 24)
    win_w = 24;
  win_w += 6;
  int win_h = 7;
  if (win_w > sw - 2)
    win_w = sw - 2;
  int y = (sh - win_h) / 2, x = (sw - win_w) / 2;
  if (y < 1)
    y = 1;
  if (x < 0)
    x = 0;
  WINDOW *win = newwin(win_h, win_w, y, x);
  if (!win)
    return -1;
  keypad(win, true);
  int sel = 0;
  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);
    if (title[0]) {
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, 2, " %s ", title);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }
    mvwprintw(win, 2, 2, "%s", msg);
    const char *by = " Yes ";
    const char *bn = " No ";
    int by_w = (int)strlen(by), bn_w = (int)strlen(bn);
    int btn_y = win_h - 3;
    int bx = (win_w - by_w - 2 - bn_w) / 2;
    if (bx < 2)
      bx = 2;
    chtype btn_attr = theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD;
    wattron(win, btn_attr | (sel == 0 ? 0 : A_DIM));
    mvwprintw(win, btn_y, bx, "%s", by);
    wattroff(win, btn_attr | A_DIM);
    wattron(win, btn_attr | (sel == 1 ? 0 : A_DIM));
    mvwprintw(win, btn_y, bx + by_w + 2, "%s", bn);
    wattroff(win, btn_attr | A_DIM);
    wrefresh(win);
    int ch = dialog_getch(win);
    if (ch == 27 || ch == KEY_EXIT) {
      flushinp();
      delwin(win);
      return -1;
    }
    if (ch == 'y' || ch == 'Y') {
      delwin(win);
      return 1;
    }
    if (ch == 'n' || ch == 'N') {
      delwin(win);
      return 0;
    }
    if (ch == '\n' || ch == '\r') {
      delwin(win);
      return sel == 0 ? 1 : 0;
    }
    if (ch == KEY_LEFT || ch == KEY_RIGHT || ch == '\t') {
      sel ^= 1;
      continue;
    }
    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        if (ev.bstate &
            (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_RELEASED)) {
          int my = ev.y - y, mx = ev.x - x;
          if (my == btn_y) {
            if (mx >= bx && mx < bx + by_w) {
              delwin(win);
              return 1;
            }
            if (mx >= bx + by_w + 2 && mx < bx + by_w + 2 + bn_w) {
              delwin(win);
              return 0;
            }
          }
        }
      }
      continue;
    }
  }
}

/* ── TUI Memory Map dialog ───────────────────────────────────────────────────
 * Shows a scrollable ASCII heatmap of the 64KB address space plus a range
 * table.  Each row = one page (&xx00-&xxFF).  Written pages shown as '█',
 * unwritten as '·'.  16 pages per display row to fit in a terminal.
 * Arrow keys / PgUp/PgDn scroll; ESC or Enter closes.
 */
static void tui_memmap_dialog(const theme_t *theme, const uint8_t *written_snap,
                              int range_count, const tui_range_t *ranges,
                              uint32_t total_bytes, int first_page) {
  int sh, sw;
  getmaxyx(stdscr, sh, sw);

  /* Build display lines: header + 16 rows of 16 pages each + separator +
   * range table.  Each heatmap row covers 16 pages = 4096 bytes. */
  /* 256 pages / 16 per row = 16 display rows for heatmap */
#define TMMAP_HMAP_ROWS 16
#define TMMAP_PAGES_PER_ROW 16
#define TMMAP_MAX_LINES (4 + TMMAP_HMAP_ROWS + 2 + 64 + 4)
  static char lines[TMMAP_MAX_LINES][128];
  int nlines = 0;

  /* Header */
  snprintf(lines[nlines++], 128,
           "  64KB address space — %u byte%s in %d range%s", total_bytes,
           total_bytes == 1 ? "" : "s", range_count,
           range_count == 1 ? "" : "s");
  snprintf(lines[nlines++], 128,
           "  Each block = 16 pages (&1000).  # = written,  . = empty.");
  snprintf(lines[nlines++], 128, "  %s", "");

  /* Heatmap: 16 rows, each row = 16 pages */
  for (int row = 0; row < TMMAP_HMAP_ROWS; row++) {
    char buf[128];
    int pos = 0;
    int base_page = row * TMMAP_PAGES_PER_ROW;
    pos += snprintf(buf + pos, sizeof(buf) - pos, "  &%02X00  ", base_page);
    for (int pg_off = 0; pg_off < TMMAP_PAGES_PER_ROW; pg_off++) {
      int page = base_page + pg_off;
      /* Check if any byte in this page was written */
      int any = 0;
      for (int b = 0; b < 256 && !any; b++)
        if (written_snap[page * 256 + b])
          any = 1;
      /* UTF-8 full block or middle dot */
      if (any)
        pos += snprintf(buf + pos, sizeof(buf) - pos, "##");
      else
        pos += snprintf(buf + pos, sizeof(buf) - pos, ". ");
    }
    snprintf(lines[nlines++], 128, "%s", buf);
  }

  /* Separator + range table */
  snprintf(lines[nlines++], 128, "  %s", "");
  snprintf(lines[nlines++], 128, "  %-10s  %-10s  %s", "Start", "End", "Bytes");
  snprintf(lines[nlines++], 128, "  %s",
           "------------------------------------");
  for (int i = 0; i < range_count && nlines < TMMAP_MAX_LINES - 2; i++) {
    snprintf(lines[nlines++], 128, "  &%04X      &%04X      %u byte%s",
             ranges[i].start, ranges[i].end, ranges[i].count,
             ranges[i].count == 1 ? "" : "s");
  }
  snprintf(lines[nlines++], 128, "  %s",
           "------------------------------------");
  snprintf(lines[nlines++], 128,
           "  Total               %u byte%s in %d range%s", total_bytes,
           total_bytes == 1 ? "" : "s", range_count,
           range_count == 1 ? "" : "s");

  /* Window sizing */
  int win_w = sw < 70 ? sw : 70;
  if (win_w < 40)
    win_w = 40;
  int list_h = sh - 6;
  if (list_h < 5)
    list_h = 5;
  int win_h = list_h + 4;
  if (win_h > sh - 2)
    win_h = sh - 2;
  int wy = (sh - win_h) / 2, wx = (sw - win_w) / 2;
  if (wy < 1)
    wy = 1;
  if (wx < 0)
    wx = 0;

  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return;
  keypad(win, true);
  curs_set(0);

  /* Start scroll position: show first written page's row */
  int scroll = (first_page / TMMAP_PAGES_PER_ROW);
  /* Offset into lines array for the heatmap rows */
  int hmap_start_line = 3; /* lines 0,1,2 = header */
  scroll = hmap_start_line + scroll;
  if (scroll > nlines - list_h)
    scroll = nlines - list_h;
  if (scroll < 0)
    scroll = 0;

  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);
    wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwprintw(win, 0, 2, " Memory Map ");
    wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));

    for (int i = 0; i < list_h; i++) {
      int li = scroll + i;
      if (li < 0 || li >= nlines)
        continue;
      /* Highlight written heatmap rows */
      int in_hmap =
          (li >= hmap_start_line && li < hmap_start_line + TMMAP_HMAP_ROWS);
      int row_page =
          in_hmap ? (li - hmap_start_line) * TMMAP_PAGES_PER_ROW : -1;
      /* Check if this heatmap row has any written bytes */
      int row_has_code = 0;
      if (row_page >= 0) {
        for (int pg = row_page;
             pg < row_page + TMMAP_PAGES_PER_ROW && !row_has_code; pg++)
          for (int b = 0; b < 256 && !row_has_code; b++)
            if (written_snap[pg * 256 + b])
              row_has_code = 1;
      }
      chtype attr = row_has_code ? (theme_attr(theme, TK_NUMBER, 0) | A_BOLD)
                              : theme_attr(theme, TK_DEFAULT, 0);
      wattron(win, attr);
      mvwaddnstr(win, 1 + i, 1, lines[li], win_w - 2);
      wattroff(win, attr);
    }

    /* Scrollbar */
    if (nlines > list_h) {
      int bar_h = list_h * list_h / nlines;
      if (bar_h < 1)
        bar_h = 1;
      int bar_y = scroll * list_h / nlines;
      wattron(win, theme_attr(theme, TK_OPERATOR, 0));
      for (int i = 0; i < list_h; i++)
        mvwaddch(win, 1 + i, win_w - 2,
                 (i >= bar_y && i < bar_y + bar_h) ? ACS_BLOCK : ACS_VLINE);
      wattroff(win, theme_attr(theme, TK_OPERATOR, 0));
    }

    /* Footer */
    wattron(win, theme_attr(theme, TK_COMMENT, 0));
    mvwaddstr(win, win_h - 2, 2,
              " Up/Down/PgUp/PgDn scroll   ESC/Enter close ");
    wattroff(win, theme_attr(theme, TK_COMMENT, 0));
    wrefresh(win);

    int ch = dialog_getch(win);
    if (ch == 27 || ch == KEY_EXIT || ch == '\n' || ch == '\r' || ch == ' ')
      break;
    if (ch == KEY_UP) {
      if (scroll > 0)
        scroll--;
    } else if (ch == KEY_DOWN) {
      if (scroll + list_h < nlines)
        scroll++;
    } else if (ch == KEY_PPAGE) {
      scroll -= list_h;
      if (scroll < 0)
        scroll = 0;
    } else if (ch == KEY_NPAGE) {
      scroll += list_h;
      if (scroll + list_h > nlines)
        scroll = nlines - list_h;
      if (scroll < 0)
        scroll = 0;
    }
    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        if (ev.bstate & BUTTON4_PRESSED) {
          if (scroll > 0)
            scroll--;
        } else if (ev.bstate & BUTTON5_PRESSED) {
          if (scroll + list_h < nlines)
            scroll++;
        }
      }
    }
  }
  curs_set(1);
  delwin(win);
}

/* Display a centred informational dialog and wait for any keypress. */
static void dialog_ok(const theme_t *theme, const char *title,
                      const char *msg) {
  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  if (!title)
    title = "";
  if (!msg)
    msg = "";
  const char *lines[4] = {0};
  int nlines = 0;
  char tmp[512];
  snprintf(tmp, sizeof(tmp), "%s", msg);
  char *p = tmp;
  while (nlines < 4) {
    lines[nlines++] = p;
    char *nl = strchr(p, '\n');
    if (!nl)
      break;
    *nl = 0;
    p = nl + 1;
    if (!*p)
      break;
  }
  int win_w = 24;
  int title_w = (int)strlen(title);
  if (title_w + 6 > win_w)
    win_w = title_w + 6;
  for (int i = 0; i < nlines; i++) {
    int lw = (int)strlen(lines[i]);
    if (lw + 6 > win_w)
      win_w = lw + 6;
  }
  if (win_w > sw - 2)
    win_w = sw - 2;
  int win_h = 5 + nlines;
  if (win_h < 7)
    win_h = 7;
  if (win_h > sh - 2)
    win_h = sh - 2;
  int y = (sh - win_h) / 2, x = (sw - win_w) / 2;
  if (y < 1)
    y = 1;
  if (x < 0)
    x = 0;
  WINDOW *win = newwin(win_h, win_w, y, x);
  if (!win)
    return;
  keypad(win, true);
  const char *bok = " OK ";
  int bok_w = (int)strlen(bok);
  curs_set(0);
  int btn_y = win_h - 3;
  int bx = (win_w - bok_w) / 2;
  if (bx < 2)
    bx = 2;
  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);
    if (title[0]) {
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, 2, " %s ", title);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }
    int row = 2;
    for (int i = 0; i < nlines && row < win_h - 4; i++, row++)
      mvwprintw(win, row, 2, "%s", lines[i]);
    chtype btn_attr = theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD;
    wattron(win, btn_attr);
    mvwprintw(win, btn_y, bx, "%s", bok);
    wattroff(win, btn_attr);
    wrefresh(win);
    int ch = dialog_getch(win);
    if (ch == 27 || ch == KEY_EXIT) {
      flushinp();
      break;
    }
    if (ch == '\n' || ch == '\r' || ch == ' ' || ch == 'o' || ch == 'O')
      break;
    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        if (ev.bstate &
            (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_RELEASED)) {
          int my = ev.y - y, mx = ev.x - x;
          if (my == btn_y && mx >= bx && mx < bx + bok_w)
            break;
        }
      }
      continue;
    }
  }
  curs_set(1);
  delwin(win);
}

/* Prompt the user to save unsaved changes before closing or opening a file.
 * ret: 1 to save, 0 to discard, -1 to cancel */
static int dialog_save_changes(const theme_t *theme) {
  curs_set(0);
  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int win_w = 40, win_h = 7;
  if (win_w > sw - 2)
    win_w = sw - 2;
  int y = (sh - win_h) / 2, x = (sw - win_w) / 2;
  if (y < 1)
    y = 1;
  if (x < 0)
    x = 0;
  WINDOW *win = newwin(win_h, win_w, y, x);
  if (!win)
    return 0;
  keypad(win, true);
  int sel = 0;
  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);
    wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwprintw(win, 0, 2, " Unsaved changes ");
    wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwprintw(win, 2, 2, "Save changes before closing?");
    const char *b0 = " Yes ";
    const char *b1 = " No ";
    const char *b2 = " Cancel ";
    int w0 = (int)strlen(b0), w1 = (int)strlen(b1), w2 = (int)strlen(b2);
    int btn_y = win_h - 3;
    int bx0 = (win_w - w0 - 2 - w1 - 2 - w2) / 2;
    if (bx0 < 2)
      bx0 = 2;
    int bx1 = bx0 + w0 + 2, bx2 = bx1 + w1 + 2;
    chtype btn_attr = theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD;
    wattron(win, btn_attr | (sel == 0 ? 0 : A_DIM));
    mvwprintw(win, btn_y, bx0, "%s", b0);
    wattroff(win, btn_attr | A_DIM);
    wattron(win, btn_attr | (sel == 1 ? 0 : A_DIM));
    mvwprintw(win, btn_y, bx1, "%s", b1);
    wattroff(win, btn_attr | A_DIM);
    wattron(win, btn_attr | (sel == 2 ? 0 : A_DIM));
    mvwprintw(win, btn_y, bx2, "%s", b2);
    wattroff(win, btn_attr | A_DIM);
    wrefresh(win);
    int ch = dialog_getch(win);
    if (ch == 27 || ch == KEY_EXIT) {
      flushinp();
      delwin(win);
      return 0;
    }
    if (ch == 'y' || ch == 'Y') {
      delwin(win);
      return 2;
    }
    if (ch == 'n' || ch == 'N') {
      delwin(win);
      return 1;
    }
    if (ch == '\n' || ch == '\r') {
      delwin(win);
      return sel == 0 ? 2 : sel == 1 ? 1 : 0;
    }
    if (ch == KEY_LEFT || ch == '\t')
      sel = (sel + 2) % 3;
    else if (ch == KEY_RIGHT)
      sel = (sel + 1) % 3;
    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        /* Any button event: check position relative to window */
        if (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_RELEASED |
                         BUTTON2_PRESSED | BUTTON3_PRESSED)) {
          int my = ev.y - y, mx = ev.x - x;
          bool in_win = (my >= 0 && my < win_h && mx >= 0 && mx < win_w);
          if (in_win) {
            /* Try exact button hit first */
            if (my >= btn_y - 1 && my <= btn_y + 1 && mx >= bx0 &&
                mx < bx0 + w0) {
              delwin(win);
              return 2;
            }
            if (my >= btn_y - 1 && my <= btn_y + 1 && mx >= bx1 &&
                mx < bx1 + w1) {
              delwin(win);
              return 1;
            }
            if (my >= btn_y - 1 && my <= btn_y + 1 && mx >= bx2 &&
                mx < bx2 + w2) {
              delwin(win);
              return 0;
            }
            /* Click elsewhere in window: update sel based on rough position */
            if (mx < win_w / 3)
              sel = 0;
            else if (mx < 2 * win_w / 3)
              sel = 1;
            else
              sel = 2;
          }
          /* Click outside window = cancel */
          else {
            delwin(win);
            return 0;
          }
        }
      }
      continue;
    }
  }
  return 0;
}

static int prompt_yesno(const theme_t *theme, const char *prompt) {
  return dialog_yesno(theme, "Confirm", prompt) == 1;
}

// ---------------------------------------------------------------------------
// Help viewer — displays help/help.md as scrollable plain text
// ---------------------------------------------------------------------------
static void do_help(const theme_t *theme) {
  /* Find help.md */
  const char *candidates[] = {"/usr/share/6502caEd/help/help.md",
                              "./help/help.md", "../help/help.md", NULL};

  FILE *f = NULL;
  for (int i = 0; candidates[i]; i++) {
    f = fopen(candidates[i], "r");
    if (f)
      break;
  }

/* ── Line kind tags (set during load, used during render) ── */
#define HLP_NORMAL 0
#define HLP_H1 1
#define HLP_H2 2
#define HLP_H3 3
#define HLP_CODE 4
#define HLP_TABLE 5
#define HLP_RULE 6
#define HLP_TBLSEP 7 /* |---|---| separator — skip drawing */
#define HLP_BULLET 8

  static char line_buf[8192][256];
  static int line_kind[8192];
  static int n_lines = 0;
  n_lines = 0;

/* Strip inline Markdown from a string in-place:
   removes **bold**, *italic*, `code`, [text](url) -> text */
#define STRIP_INLINE(s)                                                        \
  do {                                                                         \
    char _tmp[256];                                                            \
    int _wi = 0, _len = (int)strlen(s);                                        \
    for (int _i = 0; _i < _len && _wi < 254;) {                                \
      /* [text](url) -> text */                                                \
      if (s[_i] == '[') {                                                      \
        int _j = _i + 1;                                                       \
        while (_j < _len && s[_j] != ']')                                      \
          _j++;                                                                \
        if (s[_j] == ']' && s[_j + 1] == '(') {                                \
          int _k = _j + 2;                                                     \
          while (_k < _len && s[_k] != ')')                                    \
            _k++;                                                              \
          for (int _m = _i + 1; _m < _j && _wi < 254; _m++)                    \
            _tmp[_wi++] = s[_m];                                               \
          _i = _k + 1;                                                         \
          continue;                                                            \
        }                                                                      \
      }                                                                        \
      /* **bold** or *italic* or `code` */                                     \
      if ((_i + 1 < _len && s[_i] == '*' && s[_i + 1] == '*') ||               \
          s[_i] == '*' || s[_i] == '`') {                                      \
        char _c = s[_i];                                                       \
        int _skip = (_i + 1 < _len && s[_i + 1] == '*') ? 2 : 1;               \
        int _j = _i + _skip;                                                   \
        while (_j < _len) {                                                    \
          if (s[_j] == _c &&                                                   \
              (_skip == 1 || (_j + 1 < _len && s[_j + 1] == _c))) {            \
            _j += _skip;                                                       \
            break;                                                             \
          }                                                                    \
          if (_wi < 254)                                                       \
            _tmp[_wi++] = s[_j];                                               \
          _j++;                                                                \
        }                                                                      \
        _i = _j;                                                               \
        continue;                                                              \
      }                                                                        \
      _tmp[_wi++] = s[_i++];                                                   \
    }                                                                          \
    _tmp[_wi] = '\0';                                                          \
    strncpy(s, _tmp, 255);                                                     \
  } while (0)

  if (!f) {
    strncpy(line_buf[0], "Help file not found.", 255);
    strncpy(line_buf[1], "", 255);
    strncpy(line_buf[2], "Expected: /usr/share/6502caEd/help/help.md", 255);
    strncpy(line_buf[3], "      or: ./help/help.md", 255);
    strncpy(line_buf[4], "", 255);
    strncpy(line_buf[5], "Run ./build_help.sh to generate the help file.", 255);
    for (int _i = 0; _i < 6; _i++)
      line_kind[_i] = HLP_NORMAL;
    n_lines = 6;
  } else {
    char raw[512];
    while (n_lines < 8192 && fgets(raw, sizeof(raw), f)) {
      /* Strip trailing newline */
      int rlen = (int)strlen(raw);
      while (rlen > 0 && (raw[rlen - 1] == '\n' || raw[rlen - 1] == '\r'))
        raw[--rlen] = '\0';

      char *p = raw;
      int kind = HLP_NORMAL;

      /* Classify and strip block-level markers */
      if (p[0] == '#' && p[1] == '#' && p[2] == '#') {
        kind = HLP_H3;
        p += 3;
        while (*p == ' ')
          p++;
      } else if (p[0] == '#' && p[1] == '#') {
        kind = HLP_H2;
        p += 2;
        while (*p == ' ')
          p++;
      } else if (p[0] == '#') {
        kind = HLP_H1;
        p += 1;
        while (*p == ' ')
          p++;
      } else if (p[0] == ' ' && p[1] == ' ' && p[2] == ' ' && p[3] == ' ') {
        kind = HLP_CODE; /* keep indentation, just shift 4 */
        p += 4;
      } else if (p[0] == '-' && p[1] == '-' && p[2] == '-') {
        kind = HLP_RULE;
      } else if (p[0] == '|') {
        /* Detect |---|---| separator rows */
        int all_sep = 1;
        for (char *c = p + 1; *c; c++) {
          if (*c != '|' && *c != '-' && *c != ':' && *c != ' ') {
            all_sep = 0;
            break;
          }
        }
        kind = all_sep ? HLP_TBLSEP : HLP_TABLE;
      } else if ((p[0] == '-' || p[0] == '*') && p[1] == ' ') {
        kind = HLP_BULLET;
        p += 2;
      } else if (p[0] == '>' && p[1] == ' ') {
        kind = HLP_NORMAL;
        p += 2; /* blockquote — treat as normal */
      }

      strncpy(line_buf[n_lines], p, 255);
      line_buf[n_lines][255] = '\0';

      /* Strip non-ASCII bytes (multi-byte UTF-8 sequences show as garbage
         in basic ncurses terminals) — replace with '?' */
      for (unsigned char *_b = (unsigned char *)line_buf[n_lines]; *_b; _b++)
        if (*_b > 127)
          *_b = '?';

      /* Strip inline markup from non-code lines */
      if (kind != HLP_CODE && kind != HLP_RULE && kind != HLP_TBLSEP)
        STRIP_INLINE(line_buf[n_lines]);

      /* For table rows: strip leading/trailing pipes, replace | with spaces */
      if (kind == HLP_TABLE) {
        char *s = line_buf[n_lines];
        int slen = (int)strlen(s);
        /* strip trailing pipe+space */
        while (slen > 0 && (s[slen - 1] == ' ' || s[slen - 1] == '|'))
          s[--slen] = '\0';
        /* strip leading pipe */
        if (s[0] == '|')
          memmove(s, s + 1, slen);
        /* replace remaining pipes with 2 spaces */
        for (char *c = s; *c; c++)
          if (*c == '|') {
            *c = ' ';
          }
      }

      line_kind[n_lines] = kind;

      /* Skip separator rows entirely */
      if (kind != HLP_TBLSEP)
        n_lines++;
    }
    fclose(f);
  }

  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int win_h = sh - 2;
  int win_w = sw - 4;
  int wy = 1, wx = 2;

  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return;
  keypad(win, true);

  int top = 0;
  int vis = win_h - 2;
  const char *status = "Arrows/PgUp/PgDn: scroll    F1/Q/Esc: close";

  flushinp();
  curs_set(0);
  while (1) {
    if (top > n_lines - vis)
      top = n_lines - vis;
    if (top < 0)
      top = 0;

    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    wattron(win, theme_attr(theme, TK_DEFAULT, 0));
    box(win, 0, 0);

    /* Title bar */
    const char *title = " 6502caEd Help ";
    wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwprintw(win, 0, (win_w - (int)strlen(title)) / 2, "%s", title);
    wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));

    /* Status / hint bar */
    wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwhline(win, win_h - 1, 1, ' ', win_w - 2);
    mvwprintw(win, win_h - 1, 2, "%s", status);
    if (n_lines > vis) {
      int pct = (top * 100) / (n_lines - vis);
      mvwprintw(win, win_h - 1, win_w - 7, " %3d%% ", pct);
    }
    wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));

    /* Content */
    for (int i = 0; i < vis && (top + i) < n_lines; i++) {
      const char *ln = line_buf[top + i];
      int kind = line_kind[top + i];
      int row = i + 1;

      switch (kind) {
      case HLP_H1:
        wattron(win, theme_attr(theme, TK_DIRECTIVE, 0) | A_BOLD | A_UNDERLINE);
        mvwprintw(win, row, 2, "%-*.*s", win_w - 5, win_w - 5, ln);
        wattroff(win,
                 theme_attr(theme, TK_DIRECTIVE, 0) | A_BOLD | A_UNDERLINE);
        break;
      case HLP_H2:
        wattron(win, theme_attr(theme, TK_LABEL, 0) | A_BOLD);
        mvwprintw(win, row, 2, "%-*.*s", win_w - 5, win_w - 5, ln);
        wattroff(win, theme_attr(theme, TK_LABEL, 0) | A_BOLD);
        break;
      case HLP_H3:
        wattron(win, theme_attr(theme, TK_LABEL, 0));
        mvwprintw(win, row, 2, "  %-*.*s", win_w - 6, win_w - 6, ln);
        wattroff(win, theme_attr(theme, TK_LABEL, 0));
        break;
      case HLP_CODE:
        wattron(win, theme_attr(theme, TK_NUMBER, 0));
        mvwprintw(win, row, 4, "%-*.*s", win_w - 7, win_w - 7, ln);
        wattroff(win, theme_attr(theme, TK_NUMBER, 0));
        break;
      case HLP_TABLE:
        wattron(win, theme_attr(theme, TK_DATA, 0));
        mvwprintw(win, row, 4, "%-*.*s", win_w - 7, win_w - 7, ln);
        wattroff(win, theme_attr(theme, TK_DATA, 0));
        break;
      case HLP_RULE:
        wattron(win, theme_attr(theme, TK_COMMENT, 0));
        mvwhline(win, row, 2, ACS_HLINE, win_w - 5);
        wattroff(win, theme_attr(theme, TK_COMMENT, 0));
        break;
      case HLP_BULLET:
        mvwprintw(win, row, 2, " * %-*.*s", win_w - 7, win_w - 7, ln);
        break;
      default:
        mvwprintw(win, row, 2, "%-*.*s", win_w - 5, win_w - 5, ln);
        break;
      }
    }

    wrefresh(win);

    int ch = dialog_getch(win);
    if (ch == 'q' || ch == 'Q' || ch == 27 || ch == KEY_F(1))
      break;
    if (ch == KEY_UP || ch == 'k')
      top--;
    if (ch == KEY_DOWN || ch == 'j')
      top++;
    if (ch == KEY_PPAGE || ch == 'b')
      top -= vis;
    if (ch == KEY_NPAGE || ch == ' ')
      top += vis;
    if (ch == KEY_HOME || ch == 'g')
      top = 0;
    if (ch == KEY_END || ch == 'G')
      top = n_lines - vis;
  }

  delwin(win);
  touchwin(stdscr);
  refresh();
}

// Instruction Reference viewer
// ---------------------------------------------------------------------------
// Split layout: left=mnemonic list, right=scrollable detail
// Navigation: arrows move list; Enter selects; /  searches; Q/Esc closes

/* -- Flat data tables (subset of gui/main.c g_iref, text-only) -- */
typedef struct {
  const char *mnem;
  const char *full_name;
  const char *operation; /* may be NULL */
  const char *cpu;       /* "6502" / "6502/65C02" / "65C02/65816" / "All" etc */
  const char *flags;     /* 7-char "CZIDBNV" — y=mod n=no 0/1=forced */
  const char *flags_note; /* may be NULL */
  const char *desc;
  /* addressing modes as flat string lines, NULL-terminated */
  const char *const *modes; /* each: "Mode           Bytes  Cyc  Opcode" */
} tui_iref_t;

/* Addressing mode lines for each mnemonic */
static const char *tm_adc[] = {"Immediate          2     2    $69   All",
                               "Zero page          2     3    $65   All",
                               "Zero page,X        2     4    $75   All",
                               "Zero page indirect 2     5    $72   65C02+",
                               "Absolute           3     4    $6D   All",
                               "Absolute,X         3     4+   $7D   All",
                               "Absolute,Y         3     4+   $79   All",
                               "(Indirect,X)       2     6    $61   All",
                               "(Indirect),Y       2     5+   $71   All",
                               "[dp] indirect long 2     6    $67   65816",
                               "Absolute long      4     5    $6F   65816",
                               "Stack relative     2     4    $63   65816",
                               NULL};
static const char *tm_and[] = {"Immediate          2     2    $29   All",
                               "Zero page          2     3    $25   All",
                               "Zero page,X        2     4    $35   All",
                               "Zero page indirect 2     5    $32   65C02+",
                               "Absolute           3     4    $2D   All",
                               "Absolute,X         3     4+   $3D   All",
                               "Absolute,Y         3     4+   $39   All",
                               "(Indirect,X)       2     6    $21   All",
                               "(Indirect),Y       2     5+   $31   All",
                               "[dp] indirect long 2     6    $27   65816",
                               "Absolute long      4     5    $2F   65816",
                               "Stack relative     2     4    $23   65816",
                               NULL};
static const char *tm_asl[] = {"Accumulator        1     2    $0A   All",
                               "Zero page          2     5    $06   All",
                               "Zero page,X        2     6    $16   All",
                               "Absolute           3     6    $0E   All",
                               "Absolute,X         3     7    $1E   All",
                               NULL};
static const char *tm_bcc[] = {"Relative           2     2++  $90   All", NULL};
static const char *tm_bcs[] = {"Relative           2     2++  $B0   All", NULL};
static const char *tm_beq[] = {"Relative           2     2++  $F0   All", NULL};
static const char *tm_bit[] = {"Zero page          2     3    $24   All",
                               "Absolute           3     4    $2C   All",
                               "Immediate          2     2    $89   65C02+",
                               "Zero page,X        2     4    $34   65C02+",
                               "Absolute,X         3     4+   $3C   65C02+",
                               NULL};
static const char *tm_bmi[] = {"Relative           2     2++  $30   All", NULL};
static const char *tm_bne[] = {"Relative           2     2++  $D0   All", NULL};
static const char *tm_bpl[] = {"Relative           2     2++  $10   All", NULL};
static const char *tm_bra[] = {"Relative           2     2++  $80   65C02+",
                               NULL};
static const char *tm_brl[] = {"Long relative      3     4    $82   65816",
                               NULL};
static const char *tm_brk[] = {"Implied            1     7    $00   All", NULL};
static const char *tm_bvc[] = {"Relative           2     2++  $50   All", NULL};
static const char *tm_bvs[] = {"Relative           2     2++  $70   All", NULL};
static const char *tm_clc[] = {"Implied            1     2    $18   All", NULL};
static const char *tm_cld[] = {"Implied            1     2    $D8   All", NULL};
static const char *tm_cli[] = {"Implied            1     2    $58   All", NULL};
static const char *tm_clv[] = {"Implied            1     2    $B8   All", NULL};
static const char *tm_cmp[] = {"Immediate          2     2    $C9   All",
                               "Zero page          2     3    $C5   All",
                               "Zero page,X        2     4    $D5   All",
                               "Zero page indirect 2     5    $D2   65C02+",
                               "Absolute           3     4    $CD   All",
                               "Absolute,X         3     4+   $DD   All",
                               "Absolute,Y         3     4+   $D9   All",
                               "(Indirect,X)       2     6    $C1   All",
                               "(Indirect),Y       2     5+   $D1   All",
                               "[dp] indirect long 2     6    $C7   65816",
                               "Absolute long      4     5    $CF   65816",
                               "Stack relative     2     4    $C3   65816",
                               NULL};
static const char *tm_cpx[] = {"Immediate          2     2    $E0   All",
                               "Zero page          2     3    $E4   All",
                               "Absolute           3     4    $EC   All", NULL};
static const char *tm_cpy[] = {"Immediate          2     2    $C0   All",
                               "Zero page          2     3    $C4   All",
                               "Absolute           3     4    $CC   All", NULL};
static const char *tm_dec[] = {"Accumulator        1     2    $3A   65C02+",
                               "Zero page          2     5    $C6   All",
                               "Zero page,X        2     6    $D6   All",
                               "Absolute           3     6    $CE   All",
                               "Absolute,X         3     7    $DE   All",
                               NULL};
static const char *tm_dex[] = {"Implied            1     2    $CA   All", NULL};
static const char *tm_dey[] = {"Implied            1     2    $88   All", NULL};
static const char *tm_eor[] = {"Immediate          2     2    $49   All",
                               "Zero page          2     3    $45   All",
                               "Zero page,X        2     4    $55   All",
                               "Zero page indirect 2     5    $52   65C02+",
                               "Absolute           3     4    $4D   All",
                               "Absolute,X         3     4+   $5D   All",
                               "Absolute,Y         3     4+   $59   All",
                               "(Indirect,X)       2     6    $41   All",
                               "(Indirect),Y       2     5+   $51   All",
                               "[dp] indirect long 2     6    $47   65816",
                               "Absolute long      4     5    $4F   65816",
                               "Stack relative     2     4    $43   65816",
                               NULL};
static const char *tm_inc[] = {"Accumulator        1     2    $1A   65C02+",
                               "Zero page          2     5    $E6   All",
                               "Zero page,X        2     6    $F6   All",
                               "Absolute           3     6    $EE   All",
                               "Absolute,X         3     7    $FE   All",
                               NULL};
static const char *tm_inx[] = {"Implied            1     2    $E8   All", NULL};
static const char *tm_iny[] = {"Implied            1     2    $C8   All", NULL};
static const char *tm_jml[] = {"Absolute long      4     4    $5C   65816",
                               "[Absolute] indir.  3     5    $DC   65816",
                               NULL};
static const char *tm_jmp[] = {"Absolute           3     3    $4C   All",
                               "Indirect           3     5    $6C   All",
                               "(Abs,X) indirect   3     6    $7C   65C02+",
                               NULL};
static const char *tm_jsl[] = {"Absolute long      4     8    $22   65816",
                               NULL};
static const char *tm_jsr[] = {"Absolute           3     6    $20   All", NULL};
static const char *tm_lda[] = {"Immediate          2     2    $A9   All",
                               "Zero page          2     3    $A5   All",
                               "Zero page,X        2     4    $B5   All",
                               "Zero page indirect 2     5    $B2   65C02+",
                               "Absolute           3     4    $AD   All",
                               "Absolute,X         3     4+   $BD   All",
                               "Absolute,Y         3     4+   $B9   All",
                               "(Indirect,X)       2     6    $A1   All",
                               "(Indirect),Y       2     5+   $B1   All",
                               "[dp] indirect long 2     6    $A7   65816",
                               "Absolute long      4     5    $AF   65816",
                               "Stack relative     2     4    $A3   65816",
                               NULL};
static const char *tm_ldx[] = {"Immediate          2     2    $A2   All",
                               "Zero page          2     3    $A6   All",
                               "Zero page,Y        2     4    $B6   All",
                               "Absolute           3     4    $AE   All",
                               "Absolute,Y         3     4+   $BE   All",
                               NULL};
static const char *tm_ldy[] = {"Immediate          2     2    $A0   All",
                               "Zero page          2     3    $A4   All",
                               "Zero page,X        2     4    $B4   All",
                               "Absolute           3     4    $AC   All",
                               "Absolute,X         3     4+   $BC   All",
                               NULL};
static const char *tm_lsr[] = {"Accumulator        1     2    $4A   All",
                               "Zero page          2     5    $46   All",
                               "Zero page,X        2     6    $56   All",
                               "Absolute           3     6    $4E   All",
                               "Absolute,X         3     7    $5E   All",
                               NULL};
static const char *tm_mvn[] = {"Block src,dst banks 3     ~    $54   65816",
                               NULL};
static const char *tm_mvp[] = {"Block src,dst banks 3     ~    $44   65816",
                               NULL};
static const char *tm_nop[] = {"Implied            1     2    $EA   All", NULL};
static const char *tm_ora[] = {"Immediate          2     2    $09   All",
                               "Zero page          2     3    $05   All",
                               "Zero page,X        2     4    $15   All",
                               "Zero page indirect 2     5    $12   65C02+",
                               "Absolute           3     4    $0D   All",
                               "Absolute,X         3     4+   $1D   All",
                               "Absolute,Y         3     4+   $19   All",
                               "(Indirect,X)       2     6    $01   All",
                               "(Indirect),Y       2     5+   $11   All",
                               "[dp] indirect long 2     6    $07   65816",
                               "Absolute long      4     5    $0F   65816",
                               "Stack relative     2     4    $03   65816",
                               NULL};
static const char *tm_pea[] = {"Absolute           3     5    $F4   65816",
                               NULL};
static const char *tm_pei[] = {"(dp) indirect      2     6    $D4   65816",
                               NULL};
static const char *tm_per[] = {"Long relative      3     6    $62   65816",
                               NULL};
static const char *tm_pha[] = {"Implied            1     3    $48   All", NULL};
static const char *tm_phb[] = {"Implied            1     3    $8B   65816",
                               NULL};
static const char *tm_phd[] = {"Implied            1     4    $0B   65816",
                               NULL};
static const char *tm_phk[] = {"Implied            1     3    $4B   65816",
                               NULL};
static const char *tm_php[] = {"Implied            1     3    $08   All", NULL};
static const char *tm_phx[] = {"Implied            1     3    $DA   65C02+",
                               NULL};
static const char *tm_phy[] = {"Implied            1     3    $5A   65C02+",
                               NULL};
static const char *tm_pla[] = {"Implied            1     4    $68   All", NULL};
static const char *tm_plb[] = {"Implied            1     4    $AB   65816",
                               NULL};
static const char *tm_pld[] = {"Implied            1     5    $2B   65816",
                               NULL};
static const char *tm_plp[] = {"Implied            1     4    $28   All", NULL};
static const char *tm_plx[] = {"Implied            1     4    $FA   65C02+",
                               NULL};
static const char *tm_ply[] = {"Implied            1     4    $7A   65C02+",
                               NULL};
static const char *tm_rep[] = {"Immediate          2     3    $C2   65816",
                               NULL};
static const char *tm_rol[] = {"Accumulator        1     2    $2A   All",
                               "Zero page          2     5    $26   All",
                               "Zero page,X        2     6    $36   All",
                               "Absolute           3     6    $2E   All",
                               "Absolute,X         3     7    $3E   All",
                               NULL};
static const char *tm_ror[] = {"Accumulator        1     2    $6A   All",
                               "Zero page          2     5    $66   All",
                               "Zero page,X        2     6    $76   All",
                               "Absolute           3     6    $6E   All",
                               "Absolute,X         3     7    $7E   All",
                               NULL};
static const char *tm_rti[] = {"Implied            1     6    $40   All", NULL};
static const char *tm_rtl[] = {"Implied            1     6    $6B   65816",
                               NULL};
static const char *tm_rts[] = {"Implied            1     6    $60   All", NULL};
static const char *tm_sbc[] = {"Immediate          2     2    $E9   All",
                               "Zero page          2     3    $E5   All",
                               "Zero page,X        2     4    $F5   All",
                               "Zero page indirect 2     5    $F2   65C02+",
                               "Absolute           3     4    $ED   All",
                               "Absolute,X         3     4+   $FD   All",
                               "Absolute,Y         3     4+   $F9   All",
                               "(Indirect,X)       2     6    $E1   All",
                               "(Indirect),Y       2     5+   $F1   All",
                               "[dp] indirect long 2     6    $E7   65816",
                               "Absolute long      4     5    $EF   65816",
                               "Stack relative     2     4    $E3   65816",
                               NULL};
static const char *tm_sec[] = {"Implied            1     2    $38   All", NULL};
static const char *tm_sed[] = {"Implied            1     2    $F8   All", NULL};
static const char *tm_sei[] = {"Implied            1     2    $78   All", NULL};
static const char *tm_sep[] = {"Immediate          2     3    $E2   65816",
                               NULL};
static const char *tm_sta[] = {"Zero page          2     3    $85   All",
                               "Zero page,X        2     4    $95   All",
                               "Zero page indirect 2     6    $92   65C02+",
                               "Absolute           3     4    $8D   All",
                               "Absolute,X         3     5    $9D   All",
                               "Absolute,Y         3     5    $99   All",
                               "(Indirect,X)       2     6    $81   All",
                               "(Indirect),Y       2     6    $91   All",
                               "[dp] indirect long 2     6    $87   65816",
                               "Absolute long      4     5    $8F   65816",
                               "Stack relative     2     4    $83   65816",
                               NULL};
static const char *tm_stp[] = {"Implied            1     3    $DB   65C02+",
                               NULL};
static const char *tm_stx[] = {"Zero page          2     3    $86   All",
                               "Zero page,Y        2     4    $96   All",
                               "Absolute           3     4    $8E   All", NULL};
static const char *tm_sty[] = {"Zero page          2     3    $84   All",
                               "Zero page,X        2     4    $94   All",
                               "Absolute           3     4    $8C   All", NULL};
static const char *tm_stz[] = {"Zero page          2     3    $64   65C02+",
                               "Zero page,X        2     4    $74   65C02+",
                               "Absolute           3     4    $9C   65C02+",
                               "Absolute,X         3     5    $9E   65C02+",
                               NULL};
static const char *tm_tax[] = {"Implied            1     2    $AA   All", NULL};
static const char *tm_tay[] = {"Implied            1     2    $A8   All", NULL};
static const char *tm_tcd[] = {"Implied            1     2    $5B   65816",
                               NULL};
static const char *tm_tcs[] = {"Implied            1     2    $1B   65816",
                               NULL};
static const char *tm_tdc[] = {"Implied            1     2    $7B   65816",
                               NULL};
static const char *tm_trb[] = {"Zero page          2     5    $14   65C02+",
                               "Absolute           3     6    $1C   65C02+",
                               NULL};
static const char *tm_tsb[] = {"Zero page          2     5    $04   65C02+",
                               "Absolute           3     6    $0C   65C02+",
                               NULL};
static const char *tm_tsc[] = {"Implied            1     2    $3B   65816",
                               NULL};
static const char *tm_tsx[] = {"Implied            1     2    $BA   All", NULL};
static const char *tm_txa[] = {"Implied            1     2    $8A   All", NULL};
static const char *tm_txs[] = {"Implied            1     2    $9A   All", NULL};
static const char *tm_txy[] = {"Implied            1     2    $9B   65816",
                               NULL};
static const char *tm_tya[] = {"Implied            1     2    $98   All", NULL};
static const char *tm_tyx[] = {"Implied            1     2    $BB   65816",
                               NULL};
static const char *tm_wai[] = {"Implied            1     3    $CB   65C02+",
                               NULL};
static const char *tm_xba[] = {"Implied            1     3    $EB   65816",
                               NULL};
static const char *tm_xce[] = {"Implied            1     2    $FB   65816",
                               NULL};
/* undocumented */
static const char *tm_lax[] = {"Immediate          2     2    $AB   6502u",
                               "Zero page          2     3    $A7   6502u",
                               "Zero page,Y        2     4    $B7   6502u",
                               "Absolute           3     4    $AF   6502u",
                               "Absolute,Y         3     4+   $BF   6502u",
                               "(Indirect,X)       2     6    $A3   6502u",
                               "(Indirect),Y       2     5+   $B3   6502u",
                               NULL};
static const char *tm_sax[] = {"Zero page          2     3    $87   6502u",
                               "Zero page,Y        2     4    $97   6502u",
                               "Absolute           3     4    $8F   6502u",
                               "(Indirect,X)       2     6    $83   6502u",
                               NULL};
static const char *tm_slo[] = {"Zero page          2     5    $07   6502u",
                               "Zero page,X        2     6    $17   6502u",
                               "Absolute           3     6    $0F   6502u",
                               "Absolute,X         3     7    $1F   6502u",
                               "Absolute,Y         3     7    $1B   6502u",
                               "(Indirect,X)       2     8    $03   6502u",
                               "(Indirect),Y       2     8    $13   6502u",
                               NULL};
static const char *tm_rla[] = {"Zero page          2     5    $27   6502u",
                               "Zero page,X        2     6    $37   6502u",
                               "Absolute           3     6    $2F   6502u",
                               "Absolute,X         3     7    $3F   6502u",
                               "Absolute,Y         3     7    $3B   6502u",
                               "(Indirect,X)       2     8    $23   6502u",
                               "(Indirect),Y       2     8    $33   6502u",
                               NULL};
static const char *tm_sre[] = {"Zero page          2     5    $47   6502u",
                               "Zero page,X        2     6    $57   6502u",
                               "Absolute           3     6    $4F   6502u",
                               "Absolute,X         3     7    $5F   6502u",
                               "Absolute,Y         3     7    $5B   6502u",
                               "(Indirect,X)       2     8    $43   6502u",
                               "(Indirect),Y       2     8    $53   6502u",
                               NULL};
static const char *tm_rra[] = {"Zero page          2     5    $67   6502u",
                               "Zero page,X        2     6    $77   6502u",
                               "Absolute           3     6    $6F   6502u",
                               "Absolute,X         3     7    $7F   6502u",
                               "Absolute,Y         3     7    $7B   6502u",
                               "(Indirect,X)       2     8    $63   6502u",
                               "(Indirect),Y       2     8    $73   6502u",
                               NULL};
static const char *tm_dcp[] = {"Zero page          2     5    $C7   6502u",
                               "Zero page,X        2     6    $D7   6502u",
                               "Absolute           3     6    $CF   6502u",
                               "Absolute,X         3     7    $DF   6502u",
                               "Absolute,Y         3     7    $DB   6502u",
                               "(Indirect,X)       2     8    $C3   6502u",
                               "(Indirect),Y       2     8    $D3   6502u",
                               NULL};
static const char *tm_isc[] = {"Zero page          2     5    $E7   6502u",
                               "Zero page,X        2     6    $F7   6502u",
                               "Absolute           3     6    $EF   6502u",
                               "Absolute,X         3     7    $FF   6502u",
                               "Absolute,Y         3     7    $FB   6502u",
                               "(Indirect,X)       2     8    $E3   6502u",
                               "(Indirect),Y       2     8    $F3   6502u",
                               NULL};

static const tui_iref_t g_tui_iref[] = {
    {"ADC", "Add with Carry", "A,C=A+M+C", "All", "ynnnnyy",
     "(+1 if decimal mode)",
     "Adds memory to accumulator with carry. If overflow occurs carry is "
     "set,\nenabling multi-byte addition.",
     tm_adc},
    {"AND", "Logical AND", "A=A&M", "All", "nynnnn y", NULL,
     "Bitwise AND of accumulator and memory, result in A.", tm_and},
    {"ASL", "Shift Left", "A,C=M*2", "All", "ynnnnn y", NULL,
     "Shifts all bits left. Bit 0 cleared, bit 7 goes to carry.", tm_asl},
    {"BCC", "Branch if Carry Clear", NULL, "All", "nnnnnnn", NULL,
     "Branch if carry flag is clear.", tm_bcc},
    {"BCS", "Branch if Carry Set", NULL, "All", "nnnnnnn", NULL,
     "Branch if carry flag is set.", tm_bcs},
    {"BEQ", "Branch if Equal", NULL, "All", "nnnnnnn", NULL,
     "Branch if zero flag is set.", tm_beq},
    {"BIT", "Bit Test", "A&M", "All", "nynnn yy", "N<-M7, V<-M6",
     "Tests memory bits against A. Result not stored. N<-bit7, V<-bit6.",
     tm_bit},
    {"BMI", "Branch if Minus", NULL, "All", "nnnnnnn", NULL,
     "Branch if negative flag is set.", tm_bmi},
    {"BNE", "Branch if Not Equal", NULL, "All", "nnnnnnn", NULL,
     "Branch if zero flag is clear.", tm_bne},
    {"BPL", "Branch if Plus", NULL, "All", "nnnnnnn", NULL,
     "Branch if negative flag is clear.", tm_bpl},
    {"BRA", "Branch Always", NULL, "65C02+", "nnnnnnn", NULL,
     "Unconditional 8-bit relative branch. Range: -128 to +127 bytes.", tm_bra},
    {"BRK", "Force Interrupt", NULL, "All", "nn1n1nn", NULL,
     "Forces software interrupt. Pushes PC and status, loads IRQ vector.",
     tm_brk},
    {"BRL", "Branch Long", NULL, "65816", "nnnnnnn", NULL,
     "16-bit relative branch. Range: -32768 to +32767 bytes.", tm_brl},
    {"BVC", "Branch if Overflow Clear", NULL, "All", "nnnnnnn", NULL,
     "Branch if overflow flag is clear.", tm_bvc},
    {"BVS", "Branch if Overflow Set", NULL, "All", "nnnnnnn", NULL,
     "Branch if overflow flag is set.", tm_bvs},
    {"CLC", "Clear Carry", "C=0", "All", "0nnnnnn", NULL,
     "Clears the carry flag.", tm_clc},
    {"CLD", "Clear Decimal", "D=0", "All", "nnn0nnn", NULL,
     "Clears the decimal mode flag, switching to binary arithmetic.", tm_cld},
    {"CLI", "Clear Interrupt Disable", "I=0", "All", "nn0nnnn", NULL,
     "Clears the interrupt disable flag, enabling IRQ interrupts.", tm_cli},
    {"CLV", "Clear Overflow", "V=0", "All", "nnnnn0n", NULL,
     "Clears the overflow flag.", tm_clv},
    {"CMP", "Compare Accumulator", "A-M", "All", "yynnnny", NULL,
     "Compares A with memory. Sets flags as if subtracted; result not stored.",
     tm_cmp},
    {"CPX", "Compare X", "X-M", "All", "yynnnny", NULL,
     "Compares X register with memory.", tm_cpx},
    {"CPY", "Compare Y", "Y-M", "All", "yynnnny", NULL,
     "Compares Y register with memory.", tm_cpy},
    {"DCP", "DEC+CMP (undoc)", "M=M-1;A-M", "6502u", "yynnnny", NULL,
     "Decrements memory then compares result with A. Undocumented opcode.",
     tm_dcp},
    {"DEC", "Decrement", "M=M-1", "All", "nynnnny", NULL,
     "Decrements memory or accumulator (65C02+) by one.", tm_dec},
    {"DEX", "Decrement X", "X=X-1", "All", "nynnnny", NULL,
     "Decrements X register by one.", tm_dex},
    {"DEY", "Decrement Y", "Y=Y-1", "All", "nynnnny", NULL,
     "Decrements Y register by one.", tm_dey},
    {"EOR", "Exclusive OR", "A=A^M", "All", "nynnnn y", NULL,
     "Bitwise exclusive OR of accumulator and memory.", tm_eor},
    {"INC", "Increment", "M=M+1", "All", "nynnnny", NULL,
     "Increments memory or accumulator (65C02+) by one.", tm_inc},
    {"INX", "Increment X", "X=X+1", "All", "nynnnny", NULL,
     "Increments X register by one.", tm_inx},
    {"INY", "Increment Y", "Y=Y+1", "All", "nynnnny", NULL,
     "Increments Y register by one.", tm_iny},
    {"ISC", "INC+SBC (undoc)", "M=M+1;A=A-M", "6502u", "yynnn yy", NULL,
     "Increments memory then subtracts from A. Undocumented opcode.", tm_isc},
    {"JML", "Jump Long", "PC=24-bit addr", "65816", "nnnnnnn", NULL,
     "Jumps to a 24-bit absolute address, changing the Program Bank Register.",
     tm_jml},
    {"JMP", "Jump", "PC=addr", "All", "nnnnnnn", NULL,
     "Sets the program counter to the specified address.", tm_jmp},
    {"JSL", "Jump Subroutine Long", "PC=24-bit", "65816", "nnnnnnn", NULL,
     "Calls a subroutine at a 24-bit address. Pushes 3-byte return address.",
     tm_jsl},
    {"JSR", "Jump to Subroutine", "PC=addr", "All", "nnnnnnn", NULL,
     "Pushes return address-1 then jumps to target.", tm_jsr},
    {"LAX", "Load A and X (undoc)", "A=X=M", "6502u", "nynnnny", NULL,
     "Simultaneously loads A and X from memory. Undocumented opcode.", tm_lax},
    {"LDA", "Load Accumulator", "A=M", "All", "nynnnny", NULL,
     "Loads a value from memory into the accumulator.", tm_lda},
    {"LDX", "Load X", "X=M", "All", "nynnnny", NULL,
     "Loads a value from memory into the X register.", tm_ldx},
    {"LDY", "Load Y", "Y=M", "All", "nynnnny", NULL,
     "Loads a value from memory into the Y register.", tm_ldy},
    {"LSR", "Shift Right", "A,C=A/2", "All", "yynn n0y", NULL,
     "Shifts all bits right. Bit 7 cleared, bit 0 goes to carry.", tm_lsr},
    {"MVN", "Block Move Negative", "[dst++]=[src++]", "65816", "nnnnnnn",
     "A=0 after",
     "Block copy incrementing. Set A=count-1, X=src, Y=dst. MVN "
     "srcbank,dstbank.",
     tm_mvn},
    {"MVP", "Block Move Positive", "[dst--]=[src--]", "65816", "nnnnnnn",
     "A=0 after",
     "Block copy decrementing. Use when dst > src and ranges overlap.", tm_mvp},
    {"NOP", "No Operation", NULL, "All", "nnnnnnn", NULL,
     "No operation. Increments PC only.", tm_nop},
    {"ORA", "Logical OR", "A=A|M", "All", "nynnnn y", NULL,
     "Bitwise OR of accumulator and memory.", tm_ora},
    {"PEA", "Push Effective Absolute", NULL, "65816", "nnnnnnn", NULL,
     "Pushes a 16-bit absolute value onto the stack (not the contents).",
     tm_pea},
    {"PEI", "Push Effective Indirect", NULL, "65816", "nnnnnnn", NULL,
     "Pushes the 16-bit value pointed to by a direct page address.", tm_pei},
    {"PER", "Push PC-Relative", NULL, "65816", "nnnnnnn", NULL,
     "Pushes a PC-relative effective address. Used for position-independent "
     "code.",
     tm_per},
    {"PHA", "Push A", "S:A", "All", "nnnnnnn", NULL,
     "Pushes accumulator onto the stack.", tm_pha},
    {"PHB", "Push Data Bank", "S:DBR", "65816", "nnnnnnn", NULL,
     "Pushes the Data Bank Register onto the stack.", tm_phb},
    {"PHD", "Push Direct Page", "S:D", "65816", "nnnnnnn", NULL,
     "Pushes the 16-bit Direct Page register onto the stack.", tm_phd},
    {"PHK", "Push Program Bank", "S:PBR", "65816", "nnnnnnn", NULL,
     "Pushes the Program Bank Register onto the stack.", tm_phk},
    {"PHP", "Push Status", "S:P", "All", "nnnnnnn", NULL,
     "Pushes the processor status register onto the stack.", tm_php},
    {"PHX", "Push X", "S:X", "65C02+", "nnnnnnn", NULL,
     "Pushes X register onto the stack.", tm_phx},
    {"PHY", "Push Y", "S:Y", "65C02+", "nnnnnnn", NULL,
     "Pushes Y register onto the stack.", tm_phy},
    {"PLA", "Pull A", "A=S", "All", "nynnnny", NULL,
     "Pulls top byte from stack into accumulator.", tm_pla},
    {"PLB", "Pull Data Bank", "DBR=S", "65816", "nynnnny", NULL,
     "Pulls top byte from stack into the Data Bank Register.", tm_plb},
    {"PLD", "Pull Direct Page", "D=S", "65816", "nynnnny", NULL,
     "Pulls top 16-bit word from stack into Direct Page register.", tm_pld},
    {"PLP", "Pull Status", "P=S", "All", "yyyyyyy", NULL,
     "Pulls processor status flags from the stack.", tm_plp},
    {"PLX", "Pull X", "X=S", "65C02+", "nynnnny", NULL,
     "Pulls top byte from stack into X register.", tm_plx},
    {"PLY", "Pull Y", "Y=S", "65C02+", "nynnnny", NULL,
     "Pulls top byte from stack into Y register.", tm_ply},
    {"RLA", "ROL+AND (undoc)", "A=A&ROL(M)", "6502u", "yynnnny", NULL,
     "Rotates memory left then ANDs result into A. Undocumented opcode.",
     tm_rla},
    {"REP", "Reset Processor Bits", "P=P&~M", "65816", "yyyyyyy",
     "bits specified by imm",
     "Clears bits in processor status. REP #$20=16-bit A, REP #$10=16-bit "
     "index.",
     tm_rep},
    {"ROL", "Rotate Left", "A,C=M*2+C", "All", "yynnnny", NULL,
     "Shifts all bits left. Carry fills bit 0, bit 7 goes to carry.", tm_rol},
    {"ROR", "Rotate Right", "A,C=M/2+C*127", "All", "yynnnny", NULL,
     "Shifts all bits right. Carry fills bit 7, bit 0 goes to carry.", tm_ror},
    {"RRA", "ROR+ADC (undoc)", "A=A+ROR(M)+C", "6502u", "yynnn yy", NULL,
     "Rotates memory right then adds to A. Undocumented opcode.", tm_rra},
    {"RTI", "Return from Interrupt", NULL, "All", "yyyyyyy", NULL,
     "Pulls processor status then program counter from the stack.", tm_rti},
    {"RTL", "Return Long", NULL, "65816", "nnnnnnn", NULL,
     "Pulls 24-bit return address from stack, adds one. Counterpart to JSL.",
     tm_rtl},
    {"RTS", "Return from Subroutine", NULL, "All", "nnnnnnn", NULL,
     "Pulls PC-1 from stack. Counterpart to JSR.", tm_rts},
    {"SAX", "Store A&X (undoc)", "M=A&X", "6502u", "nnnnnnn", NULL,
     "Stores result of A AND X to memory. Undocumented opcode.", tm_sax},
    {"SBC", "Subtract with Carry", "A,C=A-M-(1-C)", "All", "yynnn yy",
     "(+1 if decimal)", "Subtracts memory and borrow from accumulator.",
     tm_sbc},
    {"SEC", "Set Carry", "C=1", "All", "1nnnnnn", NULL, "Sets the carry flag.",
     tm_sec},
    {"SED", "Set Decimal", "D=1", "All", "nnn1nnn", NULL,
     "Sets decimal mode flag, enabling BCD arithmetic.", tm_sed},
    {"SEI", "Set Interrupt Disable", "I=1", "All", "nn1nnnn", NULL,
     "Sets interrupt disable flag, preventing IRQ interrupts.", tm_sei},
    {"SEP", "Set Processor Bits", "P=P|M", "65816", "yyyyyyy",
     "bits specified by imm",
     "Sets bits in processor status. SEP #$20=8-bit A, SEP #$10=8-bit index.",
     tm_sep},
    {"SLO", "ASL+ORA (undoc)", "A=A|(M<<1)", "6502u", "yynnnny", NULL,
     "Shifts memory left then ORs into A. Undocumented opcode.", tm_slo},
    {"SRE", "LSR+EOR (undoc)", "A=A^(M>>1)", "6502u", "yynnnny", NULL,
     "Shifts memory right then EORs into A. Undocumented opcode.", tm_sre},
    {"STA", "Store Accumulator", "M=A", "All", "nnnnnnn", NULL,
     "Stores accumulator to memory.", tm_sta},
    {"STP", "Stop the Clock", NULL, "65C02+", "nnnnnnn", NULL,
     "Halts CPU until hardware reset. Low-power state.", tm_stp},
    {"STX", "Store X", "M=X", "All", "nnnnnnn", NULL,
     "Stores X register to memory.", tm_stx},
    {"STY", "Store Y", "M=Y", "All", "nnnnnnn", NULL,
     "Stores Y register to memory.", tm_sty},
    {"STZ", "Store Zero", "M=0", "65C02+", "nnnnnnn", NULL,
     "Stores zero to memory.", tm_stz},
    {"TAX", "Transfer A to X", "X=A", "All", "nynnnny", NULL,
     "Copies accumulator to X register.", tm_tax},
    {"TAY", "Transfer A to Y", "Y=A", "All", "nynnnny", NULL,
     "Copies accumulator to Y register.", tm_tay},
    {"TCD", "Transfer C to DP", "D=C", "65816", "nynnnny", NULL,
     "Transfers 16-bit accumulator to Direct Page register.", tm_tcd},
    {"TCS", "Transfer C to SP", "S=C", "65816", "nnnnnnn", NULL,
     "Transfers 16-bit accumulator to Stack Pointer.", tm_tcs},
    {"TDC", "Transfer DP to C", "C=D", "65816", "nynnnny", NULL,
     "Transfers Direct Page register to 16-bit accumulator.", tm_tdc},
    {"TRB", "Test and Reset Bits", "M=M&~A", "65C02+", "nynnnn n", "Z=(M&A)==0",
     "Tests bits then clears those set in A.", tm_trb},
    {"TSB", "Test and Set Bits", "M=M|A", "65C02+", "nynnnn n", "Z=(M&A)==0",
     "Tests bits then sets those set in A.", tm_tsb},
    {"TSC", "Transfer SP to C", "C=S", "65816", "nynnnny", NULL,
     "Transfers Stack Pointer to 16-bit accumulator.", tm_tsc},
    {"TSX", "Transfer S to X", "X=S", "All", "nynnnny", NULL,
     "Copies Stack Pointer to X register.", tm_tsx},
    {"TXA", "Transfer X to A", "A=X", "All", "nynnnny", NULL,
     "Copies X register to accumulator.", tm_txa},
    {"TXS", "Transfer X to S", "S=X", "All", "nnnnnnn", NULL,
     "Copies X register to Stack Pointer.", tm_txs},
    {"TXY", "Transfer X to Y", "Y=X", "65816", "nynnnny", NULL,
     "Copies X register to Y register.", tm_txy},
    {"TYA", "Transfer Y to A", "A=Y", "All", "nynnnny", NULL,
     "Copies Y register to accumulator.", tm_tya},
    {"TYX", "Transfer Y to X", "X=Y", "65816", "nynnnny", NULL,
     "Copies Y register to X register.", tm_tyx},
    {"WAI", "Wait for Interrupt", NULL, "65C02+", "nnnnnnn", NULL,
     "Halts CPU until an interrupt occurs. Reduces power consumption.", tm_wai},
    {"XBA", "Exchange B and A", "A<->B", "65816", "nynnnny", NULL,
     "Swaps the two 8-bit halves of the 16-bit C register.", tm_xba},
    {"XCE", "Exchange Carry/Emulation", "C<->E", "65816", "ynnnnn n",
     "E flag swapped with C",
     "Exchanges carry with emulation flag. CLC+XCE=native mode; "
     "SEC+XCE=emulation.",
     tm_xce},
    {NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL}};
#define TUI_IREF_COUNT ((int)(sizeof(g_tui_iref) / sizeof(g_tui_iref[0]) - 1))

/* -------------------------------------------------------------------------
 * Autocomplete
 * ------------------------------------------------------------------------- */

/* Mnemonic list for completion (uppercase) */
static const char *g_ac_mnemonics[] = {
    "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL", "BRA",
    "BRK", "BRL", "BVC", "BVS", "CLC", "CLD", "CLI", "CLV", "CMP", "CPX", "CPY",
    "DEC", "DEX", "DEY", "EOR", "INC", "INX", "INY", "JML", "JMP", "JSL", "JSR",
    "LDA", "LDX", "LDY", "LSR", "MVN", "MVP", "NOP", "ORA", "PEA", "PEI", "PER",
    "PHA", "PHB", "PHD", "PHK", "PHP", "PHX", "PHY", "PLA", "PLB", "PLD", "PLP",
    "PLX", "PLY", "REP", "ROL", "ROR", "RTI", "RTL", "RTS", "SBC", "SEC", "SED",
    "SEI", "SEP", "STA", "STP", "STX", "STY", "STZ", "TAX", "TAY", "TCD", "TCS",
    "TDC", "TRB", "TSB", "TSC", "TSX", "TXA", "TXS", "TXY", "TYA", "TYX", "WAI",
    "WDM", "XBA", "XCE", NULL};

/* Directive list for completion (lowercase) */
static const char *g_ac_directives[] = {
    ".align",   ".assert",    ".byte",        ".cpu",    ".def",      ".define",
    ".dword",   ".elif",      ".else",        ".endif",  ".endmacro", ".endm",
    ".endproc", ".endrepeat", ".endscope",    ".error",  ".if",       ".ifdef",
    ".ifndef",  ".include",   ".includeroot", ".labels", ".longa",    ".longi",
    ".macro",   ".opt",       ".org",         ".pc",     ".proc",     ".repeat",
    ".res",     ".scope",     ".warning",     ".word",   NULL};

#define AC_MAX_RESULTS 64
#define AC_MAX_WORD 64

/* Extract the token being typed at (line, col). Returns length (0 if none). */
static int ac_word_at(const char *line, int col, char *out, int cap) {
  if (!line || col <= 0)
    return 0;
  int len = (int)strlen(line);
  if (col > len)
    col = len;
  /* scan back to start of token: ident chars or leading dot */
  int start = col;
  while (start > 0) {
    char c = line[start - 1];
    if (isalnum((unsigned char)c) || c == '_' || c == '.')
      start--;
    else
      break;
  }
  int wlen = col - start;
  if (wlen < 2 || wlen >= cap)
    return 0;
  memcpy(out, line + start, (size_t)wlen);
  out[wlen] = 0;
  return wlen;
}

/* Check if col is inside a comment or string on line */
static int ac_in_comment_or_string(const char *line, int col) {
  if (!line)
    return 0;
  int in_str = 0;
  for (int i = 0; i < col && line[i]; i++) {
    if (!in_str && line[i] == ';')
      return 1;
    if (line[i] == '"')
      in_str = !in_str;
  }
  return 0;
}

/* Build completion list for prefix. Returns count. results[] must hold
 * AC_MAX_RESULTS. */
static int ac_build_list(const char *prefix, const symdb_t *symdb,
                         char results[][AC_MAX_WORD], int max) {
  int n = 0;
  int dot = (prefix[0] == '.');
  char up[AC_MAX_WORD];
  int plen = (int)strlen(prefix);
  /* uppercase prefix for case-insensitive match */
  for (int i = 0; i < plen && i < AC_MAX_WORD - 1; i++)
    up[i] = (char)toupper((unsigned char)prefix[i]);
  up[plen] = 0;

  /* Mnemonics (no dot prefix) */
  if (!dot) {
    for (int i = 0; g_ac_mnemonics[i] && n < max; i++) {
      if (strncasecmp(g_ac_mnemonics[i], up, (size_t)plen) == 0) {
        strncpy(results[n++], g_ac_mnemonics[i], AC_MAX_WORD - 1);
        results[n - 1][AC_MAX_WORD - 1] = 0;
      }
    }
  }

  /* Directives */
  for (int i = 0; g_ac_directives[i] && n < max; i++) {
    if (strncasecmp(g_ac_directives[i], prefix, (size_t)plen) == 0) {
      strncpy(results[n++], g_ac_directives[i], AC_MAX_WORD - 1);
      results[n - 1][AC_MAX_WORD - 1] = 0;
    }
  }

  /* Labels / macros from symdb (no dot prefix) */
  if (!dot && symdb) {
    for (size_t i = 0; i < symdb->def_count && n < max; i++) {
      const char *name = symdb->defs[i].name; /* uppercase in symdb */
      if (!name)
        continue;
      if (strncasecmp(name, up, (size_t)plen) == 0) {
        /* avoid duplicating mnemonics */
        int dup = 0;
        for (int j = 0; j < n && !dup; j++)
          if (strcasecmp(results[j], name) == 0)
            dup = 1;
        if (!dup) {
          strncpy(results[n++], name, AC_MAX_WORD - 1);
          results[n - 1][AC_MAX_WORD - 1] = 0;
        }
      }
    }
  }
  return n;
}

/*
 * Show the completion popup. Returns the accepted string (into `accepted`,
 * max `acc_cap`) or returns 0 if dismissed.
 * `prefix_len` is the number of chars already typed (to be replaced).
 */
static int do_autocomplete_popup(const theme_t *theme,
                                 char results[][AC_MAX_WORD], int nresults,
                                 int cur_screen_row, int cur_screen_col,
                                 char *accepted, int acc_cap) {
  if (nresults <= 0)
    return 0;

  int sh, sw;
  getmaxyx(stdscr, sh, sw);

  int win_w = 24;
  /* make wide enough for longest result */
  for (int i = 0; i < nresults; i++) {
    int l = (int)strlen(results[i]) + 2;
    if (l > win_w)
      win_w = l;
  }
  if (win_w > sw - 4)
    win_w = sw - 4;

  int max_h = 8;                                         /* max visible items */
  int win_h = (nresults < max_h ? nresults : max_h) + 2; /* +2 for border */

  /* Position below cursor, flip up if not enough room */
  int wy = cur_screen_row + 1;
  if (wy + win_h > sh - 2)
    wy = cur_screen_row - win_h;
  if (wy < 1)
    wy = 1;
  int wx = cur_screen_col;
  if (wx + win_w > sw)
    wx = sw - win_w;
  if (wx < 0)
    wx = 0;

  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return 0;
  keypad(win, TRUE);
  curs_set(0);

  int sel = 0, top = 0;
  int list_h = win_h - 2;
  int result = 0;

  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);

    if (sel < top)
      top = sel;
    if (sel >= top + list_h)
      top = sel - list_h + 1;

    for (int i = 0; i < list_h && (top + i) < nresults; i++) {
      int is_sel = (top + i == sel);
      if (is_sel)
        wattron(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
      else
        wattron(win, theme_attr(theme, TK_DEFAULT, 0));
      mvwprintw(win, 1 + i, 1, "%-*.*s", win_w - 2, win_w - 2,
                results[top + i]);
      if (is_sel)
        wattroff(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
      else
        wattroff(win, theme_attr(theme, TK_DEFAULT, 0));
    }
    wrefresh(win);

    int ch = dialog_getch(win);

    if (ch == KEY_UP || ch == 'k') {
      if (sel > 0)
        sel--;
    } else if (ch == KEY_DOWN || ch == 'j') {
      if (sel + 1 < nresults)
        sel++;
    } else if (ch == KEY_PPAGE) {
      sel -= list_h;
      if (sel < 0)
        sel = 0;
    } else if (ch == KEY_NPAGE) {
      sel += list_h;
      if (sel >= nresults)
        sel = nresults - 1;
    } else if (ch == '\t' || ch == '\n' || ch == '\r') {
      /* Accept */
      strncpy(accepted, results[sel], (size_t)(acc_cap - 1));
      accepted[acc_cap - 1] = 0;
      result = 1;
      break;
    } else if (ch == 27 || ch == KEY_EXIT) {
      /* Dismiss */
      result = 0;
      break;
    } else {
      /* Any other key — dismiss and pass key back via ungetch */
      ungetch(ch);
      result = 0;
      break;
    }
  }

  curs_set(1);
  delwin(win);
  touchwin(stdscr);
  refresh();
  return result;
}

static void do_iref(const theme_t *theme) {
  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int win_h = sh - 2, win_w = sw - 2;
  int wy = 1, wx = 1;
  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return;
  keypad(win, true);

  int list_w = 14;
  int detail_x = list_w + 1;
  int detail_w = win_w - detail_x - 1;

  int sel = 0, list_top = 0, detail_top = 0;
  int focus = 0;
  char search[16] = "";
  int searching = 0;

  /* CPU filter: 0=All 1=6502legal 2=6502undoc 3=6502all 4=65C02 5=65816 */
  static const char *cpu_filter_names[] = {
      "All CPUs", "6502 legal", "6502 undoc", "6502 all", "65C02", "65816"};
  static const char *cpu_filter_match[] = {NULL, "All\0",   "6502u\0",
                                           NULL, "65C02\0", "65816\0"};
  /* For 6502 legal: cpu contains "All" or "6502" but NOT "6502u"
     For 6502 all: cpu contains "All" or "6502" (including "6502u")
     For 65C02: cpu contains "65C02" or "All"
     For 65816: cpu contains "65816" or "All" */
  int cpu_filt = 0; /* index into cpu_filter_names */
#define N_CPU_FILT 6

  /* Address mode filter cycles through common modes */
  static const char *am_filter_names[] = {
      "All Modes",          "Implied",       "Immediate",     "Zero page",
      "Absolute",           "Relative",      "(Indirect,X)",  "(Indirect),Y",
      "Zero page indirect", "Absolute long", "Stack relative"};
#define N_AM_FILT 11
  int am_filt = 0;

  flushinp();
  curs_set(0);

  while (1) {
    int list_h = win_h - 4; /* one extra row for filter bar */

    if (sel < list_top)
      list_top = sel;
    if (sel >= list_top + list_h)
      list_top = sel - list_h + 1;
    if (list_top < 0)
      list_top = 0;

    /* Build filtered index */
    int filtered[TUI_IREF_COUNT];
    int nf = 0;
    char sup[16] = "";
    for (int i = 0; search[i] && i < 15; i++)
      sup[i] = (char)toupper((unsigned char)search[i]);

    for (int i = 0; i < TUI_IREF_COUNT; i++) {
      const tui_iref_t *ir2 = &g_tui_iref[i];

      /* Text filter */
      if (sup[0]) {
        char mu[8] = "";
        for (int j = 0; ir2->mnem[j] && j < 7; j++)
          mu[j] = (char)toupper((unsigned char)ir2->mnem[j]);
        if (!strstr(mu, sup))
          continue;
      }

      /* CPU filter */
      if (cpu_filt > 0) {
        const char *cpu = ir2->cpu ? ir2->cpu : "";
        int ok = 0;
        switch (cpu_filt) {
        case 1: /* 6502 legal: All or 6502 but not 6502u */
          ok = (strcmp(cpu, "All") == 0 ||
                (strncmp(cpu, "6502", 4) == 0 && strcmp(cpu, "6502u") != 0));
          break;
        case 2: /* 6502 undoc */
          ok = (strcmp(cpu, "6502u") == 0);
          break;
        case 3: /* 6502 all */
          ok = (strcmp(cpu, "All") == 0 || strncmp(cpu, "6502", 4) == 0);
          break;
        case 4: /* 65C02 */
          ok = (strcmp(cpu, "All") == 0 || strstr(cpu, "65C02") != NULL);
          break;
        case 5: /* 65816 */
          ok = (strcmp(cpu, "All") == 0 || strstr(cpu, "65816") != NULL);
          break;
        }
        if (!ok)
          continue;
        (void)cpu_filter_match;
      }

      /* Address mode filter */
      if (am_filt > 0) {
        const char *want = am_filter_names[am_filt];
        int found = 0;
        if (ir2->modes) {
          for (int mi = 0; ir2->modes[mi] && !found; mi++) {
            /* mode line starts with the mode name */
            if (strncmp(ir2->modes[mi], want, strlen(want)) == 0)
              found = 1;
          }
        }
        if (!found)
          continue;
      }

      filtered[nf++] = i;
    }

    if (sel >= nf)
      sel = nf > 0 ? nf - 1 : 0;
    const tui_iref_t *ir = nf > 0 ? &g_tui_iref[filtered[sel]] : NULL;

    /* ---- Draw ---- */
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);

    /* Title */
    {
      const char *t = " Instruction Reference ";
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, (win_w - (int)strlen(t)) / 2, "%s", t);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }
    mvwvline(win, 1, list_w, ACS_VLINE, win_h - 2);
    mvwaddch(win, 0, list_w, ACS_TTEE);
    mvwaddch(win, win_h - 1, list_w, ACS_BTEE);

    /* Filter bar (row win_h-2) */
    {
      wattron(win, theme_attr(theme, TK_DEFAULT, 0));
      mvwhline(win, win_h - 2, 1, ACS_HLINE, win_w - 2);
      mvwaddch(win, win_h - 2, 0, ACS_LTEE);
      mvwaddch(win, win_h - 2, win_w - 1, ACS_RTEE);
      mvwaddch(win, win_h - 2, list_w, ACS_PLUS);
      wattroff(win, theme_attr(theme, TK_DEFAULT, 0));

      /* CPU filter indicator */
      wattron(win, (cpu_filt > 0) ? (theme_attr(theme, TK_MNEMONIC, 0) | A_BOLD)
                                  : theme_attr(theme, TK_COMMENT, 0));
      mvwprintw(win, win_h - 2, 2, "C:%-10s", cpu_filter_names[cpu_filt]);
      wattroff(win, (cpu_filt > 0)
                        ? (theme_attr(theme, TK_MNEMONIC, 0) | A_BOLD)
                        : theme_attr(theme, TK_COMMENT, 0));

      /* AM filter indicator */
      wattron(win, (am_filt > 0) ? (theme_attr(theme, TK_DIRECTIVE, 0) | A_BOLD)
                                 : theme_attr(theme, TK_COMMENT, 0));
      mvwprintw(win, win_h - 2, detail_x + 2, "A:%-18s",
                am_filter_names[am_filt]);
      wattroff(win, (am_filt > 0)
                        ? (theme_attr(theme, TK_DIRECTIVE, 0) | A_BOLD)
                        : theme_attr(theme, TK_COMMENT, 0));
    }

    /* Status bar */
    {
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwhline(win, win_h - 1, 1, ' ', win_w - 2);
      if (searching) {
        mvwprintw(win, win_h - 1, 2, "Search: %s_", search);
      } else {
        mvwprintw(win, win_h - 1, 2,
                  "/ Search  C CPU  A Mode  Tab Detail  Q Quit");
        if (nf > 0)
          mvwprintw(win, win_h - 1, win_w - 8, "%3d/%3d", sel + 1, nf);
      }
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }

    /* List */
    for (int i = 0; i < list_h && (list_top + i) < nf; i++) {
      int idx = filtered[list_top + i];
      int row = i + 1;
      int is_sel = (list_top + i) == sel;
      if (is_sel && focus == 0)
        wattron(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
      else
        wattron(win, theme_attr(theme, TK_MNEMONIC, 0));
      mvwprintw(win, row, 1, " %-*.*s ", list_w - 3, list_w - 3,
                g_tui_iref[idx].mnem);
      if (is_sel && focus == 0)
        wattroff(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
      else
        wattroff(win, theme_attr(theme, TK_MNEMONIC, 0));
    }

    /* Detail panel */
    if (ir) {
      static char dlines[128][80];
      static int dkinds[128];
      int nd = 0;
#define DADD(k, fmt, ...)                                                      \
  do {                                                                         \
    if (nd < 127) {                                                            \
      snprintf(dlines[nd], 79, fmt, ##__VA_ARGS__);                            \
      dkinds[nd] = k;                                                          \
      nd++;                                                                    \
    }                                                                          \
  } while (0)
      DADD(1, "%s  --  %s", ir->mnem, ir->full_name);
      if (ir->operation)
        DADD(3, "  %s", ir->operation);
      DADD(0, "");
      DADD(0, "Supported on: %s", ir->cpu);
      DADD(0, "");
      DADD(1, "Flags (C Z I D B V N):");
      {
        const char *fn[] = {"C=carry", "Z=zero", "I=irqdis", "D=dec",
                            "B=brk",   "V=ovf",  "N=neg"};
        for (int fi = 0; fi < 7; fi++) {
          char fc = (fi < (int)strlen(ir->flags)) ? ir->flags[fi] : 'n';
          const char *eff = (fc == 'y')   ? "modified"
                            : (fc == '0') ? "cleared"
                            : (fc == '1') ? "set"
                                          : "not affected";
          DADD(4, "  %-10s : %s", fn[fi], eff);
        }
      }
      if (ir->flags_note)
        DADD(3, "  Note: %s", ir->flags_note);
      DADD(0, "");
      {
        const char *dp = ir->desc;
        while (*dp) {
          /* handle \n in desc: process one newline-delimited segment at a time
           */
          const char *nl = strchr(dp, '\n');
          int seg = nl ? (int)(nl - dp) : (int)strlen(dp);
          int avail = detail_w - 3;
          if (avail < 10)
            avail = 10;
          while (seg > 0) {
            int take = seg < avail ? seg : avail;
            /* back up to a word boundary */
            while (take < seg && dp[take] != ' ')
              take--;
            if (take == 0)
              take = seg < avail ? seg : avail;
            char tmp[80] = "";
            int cp = take < 79 ? take : 79;
            memcpy(tmp, dp, (size_t)cp);
            tmp[cp] = 0;
            DADD(0, "%s", tmp);
            dp += take;
            seg -= take;
            while (*dp == ' ') {
              dp++;
              seg--;
            }
          }
          if (nl)
            dp = nl + 1;
          else
            break;
        }
      }
      if (ir->modes && ir->modes[0]) {
        DADD(0, "");
        DADD(1, "Addressing Modes:");
        DADD(3, "  %-20s Bytes Cyc  Op    CPU", "Mode");
        DADD(3, "  %s", "--------------------+-----+----+------+-------");
        for (int mi = 0; ir->modes[mi]; mi++)
          DADD(2, "  %s", ir->modes[mi]);
        DADD(3, "  + =page crossed; ++ =branch+page; ~ =variable");
      }
#undef DADD

      if (detail_top > nd - (win_h - 4))
        detail_top = nd - (win_h - 4);
      if (detail_top < 0)
        detail_top = 0;

      for (int di = 0; di < win_h - 4 && (detail_top + di) < nd; di++) {
        int dk = dkinds[detail_top + di];
        int row = di + 1, col = detail_x + 1, avail = detail_w - 2;
        switch (dk) {
        case 1:
          wattron(win, theme_attr(theme, TK_LABEL, 0) | A_BOLD);
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          wattroff(win, theme_attr(theme, TK_LABEL, 0) | A_BOLD);
          break;
        case 2:
          wattron(win, theme_attr(theme, TK_NUMBER, 0));
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          wattroff(win, theme_attr(theme, TK_NUMBER, 0));
          break;
        case 3:
          wattron(win, theme_attr(theme, TK_COMMENT, 0));
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          wattroff(win, theme_attr(theme, TK_COMMENT, 0));
          break;
        case 4:
          wattron(win, theme_attr(theme, TK_DATA, 0));
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          wattroff(win, theme_attr(theme, TK_DATA, 0));
          break;
        default:
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          break;
        }
      }
    }

    wrefresh(win);

    /* ---- Input ---- */
    int ch = dialog_getch(win);

    if (searching) {
      if (ch == 27 || ch == '\n' || ch == '\r') {
        searching = 0;
        sel = 0;
        detail_top = 0;
        curs_set(0);
      } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        int sl = (int)strlen(search);
        if (sl > 0)
          search[sl - 1] = 0;
        sel = 0;
        detail_top = 0;
      } else if (ch >= 32 && ch < 127 && (int)strlen(search) < 14) {
        int sl = (int)strlen(search);
        search[sl] = (char)ch;
        search[sl + 1] = 0;
        sel = 0;
        detail_top = 0;
      }
      continue;
    }

    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        int ry = ev.y - wy, rx = ev.x - wx;
        bool clicked = (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED |
                                     BUTTON1_RELEASED)) != 0;
        if (clicked && ry >= 1 && ry < win_h - 2) {
          if (rx < list_w) {
            focus = 0;
            int ni = list_top + (ry - 1);
            if (ni >= 0 && ni < nf) {
              sel = ni;
              detail_top = 0;
            }
          } else {
            focus = 1;
          }
        }
        /* Click on filter bar cycles filters */
        if (clicked && ry == win_h - 2) {
          if (rx < list_w) {
            cpu_filt = (cpu_filt + 1) % N_CPU_FILT;
            sel = 0;
            detail_top = 0;
          } else {
            am_filt = (am_filt + 1) % N_AM_FILT;
            sel = 0;
            detail_top = 0;
          }
        }
        if (rx >= list_w) {
          if (ev.bstate & BUTTON4_PRESSED)
            detail_top -= 3;
          if (ev.bstate & BUTTON5_PRESSED)
            detail_top += 3;
        }
      }
      continue;
    }

    if (ch == 'q' || ch == 'Q' || ch == 27)
      break;
    if (ch == '\t') {
      focus = 1 - focus;
      continue;
    }
    if (ch == '/') {
      searching = 1;
      search[0] = 0;
      sel = 0;
      detail_top = 0;
      curs_set(1);
      continue;
    }
    /* Cycle CPU filter */
    if (ch == 'c' || ch == 'C') {
      cpu_filt = (cpu_filt + 1) % N_CPU_FILT;
      sel = 0;
      detail_top = 0;
      continue;
    }
    /* Cycle address mode filter */
    if (ch == 'a' || ch == 'A') {
      am_filt = (am_filt + 1) % N_AM_FILT;
      sel = 0;
      detail_top = 0;
      continue;
    }

    if (focus == 0) {
      if (ch == KEY_UP || ch == 'k') {
        sel--;
        detail_top = 0;
      }
      if (ch == KEY_DOWN || ch == 'j') {
        sel++;
        detail_top = 0;
      }
      if (ch == KEY_PPAGE) {
        sel -= win_h / 2;
        detail_top = 0;
      }
      if (ch == KEY_NPAGE) {
        sel += win_h / 2;
        detail_top = 0;
      }
      if (ch == KEY_HOME) {
        sel = 0;
        detail_top = 0;
      }
      if (ch == KEY_END) {
        sel = nf - 1;
        detail_top = 0;
      }
      if (ch == '\n' || ch == '\r') {
        focus = 1;
      }
    } else {
      if (ch == KEY_UP || ch == 'k')
        detail_top--;
      if (ch == KEY_DOWN || ch == 'j')
        detail_top++;
      if (ch == KEY_PPAGE || ch == 'b')
        detail_top -= win_h - 4;
      if (ch == KEY_NPAGE || ch == ' ')
        detail_top += win_h - 4;
      if (ch == KEY_HOME || ch == 'g')
        detail_top = 0;
      if (ch == KEY_LEFT) {
        focus = 0;
      }
    }
    if (sel < 0)
      sel = 0;
    if (sel >= nf && nf > 0)
      sel = nf - 1;
  }

  delwin(win);
  touchwin(stdscr);
  refresh();
}

// Directive Reference viewer
// ---------------------------------------------------------------------------
typedef struct {
  const char *name;
  const char *aliases; /* may be NULL */
  const char *group;
  const char *syntax;
  const char *desc;
  const char *example; /* may be NULL */
} tui_dref_t;

static const tui_dref_t g_tui_dref[] = {
    {".org", "*= / .pc=", "Origin/CPU", ".org $addr",
     "Sets the program counter. Also accepted as *= or .pc=.", ".org $8000"},
    {".cpu", NULL, "Origin/CPU", ".cpu type",
     "Selects target CPU: 6502, 6502u, 65c02, 65816. Must appear before first "
     "instruction.\nSets built-in 'cpu' symbol for conditional assembly.",
     ".cpu 65816"},
    {".byte", "equb / eqb / db", "Data", ".byte expr [, ...]",
     "Defines one or more bytes. Accepts numbers, 'char', and \"strings\".",
     ".byte $01, 'A', \"hi\""},
    {".word", "equw / eqw / dq", "Data", ".word expr [, ...]",
     "Defines one or more 16-bit little-endian words.", ".word $1234, label"},
    {".dword", "equd / eqd / dd", "Data", ".dword expr [, ...]",
     "Defines one or more 32-bit little-endian doublewords.",
     ".dword $12345678"},
    {".res", "res", "Data", ".res count [, fill]",
     "Reserves count bytes, filled with fill byte (default 0).",
     ".res 256, $FF"},
    {".align", "align", "Data", ".align boundary",
     "Advances PC to next multiple of boundary. Fills skipped bytes with 0.",
     ".align 256"},
    {".incbin", "incbin", "Data", ".incbin \"file\" [, off [, len]]",
     "Includes raw binary data from a file. Optional offset and length.",
     ".incbin \"sprite.bin\", 0, 64"},
    {"NAME =", "NAME EQU", "Symbols", "NAME = expr",
     "Defines a constant symbol. Visible throughout the file.",
     "SCREEN = $0400\nFONT EQU $C000"},
    {".def", "#def / #define", "Symbols", ".def NAME [value]",
     "Preprocessor constant. Text-substitution before assembly. Defaults to "
     "1.\nDuplicate .def of same name is an error.",
     ".def NTSC 1\n.def COLS 40"},
    {".if", NULL, "Conditionals", ".if expr / .else / .endif",
     "Assembles block only if expression is non-zero. Supports .else and .elif "
     "chains.",
     ".if cpu == 65816\n  XCE\n.endif"},
    {".ifdef", NULL, "Conditionals", ".ifdef NAME / .endif",
     "Assembles block only if NAME has been defined via .def or NAME=.",
     ".ifdef DEBUG\n  NOP\n.endif"},
    {".ifndef", NULL, "Conditionals", ".ifndef NAME / .endif",
     "Assembles block only if NAME has NOT been defined.",
     ".ifndef RELEASE\n  .warning \"debug\"\n.endif"},
    {".else", ".elif / .elseif", "Conditionals", ".else",
     "Introduces alternative branch of a .if/.ifdef/.ifndef block.", NULL},
    {".endif", NULL, "Conditionals", ".endif",
     "Closes a .if / .ifdef / .ifndef block.", NULL},
    {".proc", "proc", "Procedures", ".proc name / .endproc",
     "Named procedure block. Labels inside are local. Procedure name is also a "
     "label.",
     ".proc init\n  LDA #0\n  STA $00\n.endproc"},
    {".endproc", "endproc", "Procedures", ".endproc", "Closes a .proc block.",
     NULL},
    {".scope", NULL, "Procedures", ".scope [name] / .endscope",
     "Like .proc but no implied entry point. Groups labels without a callable "
     "routine.",
     ".scope vars\n  count: .byte 0\n.endscope"},
    {".endscope", NULL, "Procedures", ".endscope", "Closes a .scope block.",
     NULL},
    {"@label", NULL, "Local Labels",
     "@labelname:", "Local label scoped to nearest enclosing .proc or .scope.",
     ".proc loop\n@again:\n  DEX\n  BNE @again\n.endproc"},
    {": (anon)", "@ / .", "Local Labels", ":",
     "Anonymous label. :+ / .+ = next; :- / .- = prev. Multiple +/- look "
     "further.",
     ":\n  LDA #0\n  BEQ :+\n  INX\n:"},
    {".macro", "macro", "Macros", ".macro NAME [args] / .endmacro",
     "Defines a macro expanded inline at invocation. Args referenced by name "
     "or '\\' then name.",
     ".macro STORE16 val, addr\n  LDA #<val\n  STA addr\n.endmacro"},
    {".endmacro", "endmacro / endm", "Macros", ".endmacro",
     "Closes a .macro definition.", NULL},
    {".repeat", NULL, "Repeat", ".repeat count / .endrepeat",
     "Repeats enclosed block count times. count must be a constant (0-65536).",
     ".repeat 8\n  NOP\n.endrepeat"},
    {".endrepeat", NULL, "Repeat", ".endrepeat", "Closes a .repeat block.",
     NULL},
    {".include", "include", "Include", ".include \"filename\"",
     "Includes another source file, resolved relative to the current file's "
     "directory.",
     ".include \"macros.asm\""},
    {".includeroot", "#includeroot", "Include", ".includeroot \"directory/\"",
     "Sets the base search directory for subsequent .include directives.\nOnly "
     "permitted in the top-level (root) file.",
     ".includeroot \"src/\"\n.include \"macros.asm\"  ; -> src/macros.asm"},
    {".assert", "assert", "Diagnostics", ".assert cond, severity, \"msg\"",
     "If condition is false, emits a diagnostic. Severity: error, warning, "
     "ldwarning.",
     ".assert * < $C000, error, \"code overflow\""},
    {".error", "error", "Diagnostics", ".error \"message\"",
     "Always emits an assembler error, halting assembly.",
     ".error \"Unsupported CPU\""},
    {".warning", "warning", "Diagnostics", ".warning \"message\"",
     "Always emits an assembler warning.", ".warning \"deprecated feature\""},
    {"opt", ".opt", "Listing", "opt [no]list / [no]h / [no]p",
     "Controls assembler listing. 'list'/'nolist' enables/disables. BBC BASIC "
     "compatible.",
     "opt list\nopt noh"},
    {".longa", "longa", "65816", ".longa on|off",
     "Tells assembler if accumulator is 16-bit (on) or 8-bit (off).\nUse when "
     "REP/SEP tracking is insufficient. 65816 only.",
     "REP #$20\n.longa on\nLDA #$1234"},
    {".longi", "longi", "65816", ".longi on|off",
     "Tells assembler if index registers are 16-bit (on) or 8-bit (off). 65816 "
     "only.",
     "REP #$10\n.longi on\nLDX #$0100"},
    {NULL, NULL, NULL, NULL, NULL, NULL}};
#define TUI_DREF_COUNT ((int)(sizeof(g_tui_dref) / sizeof(g_tui_dref[0]) - 1))

/* Show the definition-reference browser dialog (where is this symbol defined
 * and where is it used), allowing the user to jump to any occurrence. */
static void do_dref(const theme_t *theme) {
  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int win_h = sh - 2, win_w = sw - 2;
  int wy = 1, wx = 1;
  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return;
  keypad(win, true);

  int list_w = 16;
  int detail_x = list_w + 1;
  int detail_w = win_w - detail_x - 1;

  int sel = 0, list_top = 0, detail_top = 0;
  int focus = 0;
  char search[16] = "";
  int searching = 0;

  flushinp();
  curs_set(0);

  while (1) {
    int list_h = win_h - 3;

    /* Build filtered index */
    int filtered[TUI_DREF_COUNT];
    int nf = 0;
    char sup[16] = "";
    for (int i = 0; search[i] && i < 15; i++)
      sup[i] = (char)toupper((unsigned char)search[i]);
    for (int i = 0; i < TUI_DREF_COUNT; i++) {
      if (!sup[0]) {
        filtered[nf++] = i;
        continue;
      }
      char nu[32] = "";
      for (int j = 0; g_tui_dref[i].name[j] && j < 31; j++)
        nu[j] = (char)toupper((unsigned char)g_tui_dref[i].name[j]);
      if (strstr(nu, sup))
        filtered[nf++] = i;
    }
    if (sel >= nf)
      sel = nf > 0 ? nf - 1 : 0;

    if (sel < list_top)
      list_top = sel;
    if (sel >= list_top + list_h)
      list_top = sel - list_h + 1;
    if (list_top < 0)
      list_top = 0;

    const tui_dref_t *dr = nf > 0 ? &g_tui_dref[filtered[sel]] : NULL;

    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);

    /* Title */
    {
      const char *t = " Directive Reference ";
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, (win_w - (int)strlen(t)) / 2, "%s", t);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }
    mvwvline(win, 1, list_w, ACS_VLINE, win_h - 2);
    mvwaddch(win, 0, list_w, ACS_TTEE);
    mvwaddch(win, win_h - 1, list_w, ACS_BTEE);

    /* Status bar */
    wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwhline(win, win_h - 1, 1, ' ', win_w - 2);
    if (searching) {
      mvwprintw(win, win_h - 1, 2, "Search: %s_", search);
    } else {
      mvwprintw(win, win_h - 1, 2,
                "/ Search  Up/Dn Select  Tab Detail  Q Quit");
      if (nf > 0)
        mvwprintw(win, win_h - 1, win_w - 8, "%3d/%3d", sel + 1, nf);
    }
    wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));

    /* List */
    for (int i = 0; i < list_h && (list_top + i) < nf; i++) {
      int idx = filtered[list_top + i];
      int row = i + 1;
      int is_sel = (list_top + i) == sel;
      if (is_sel && focus == 0)
        wattron(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
      else
        wattron(win, theme_attr(theme, TK_DIRECTIVE, 0));
      mvwprintw(win, row, 1, " %-*.*s ", list_w - 3, list_w - 3,
                g_tui_dref[idx].name);
      if (is_sel && focus == 0)
        wattroff(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
      else
        wattroff(win, theme_attr(theme, TK_DIRECTIVE, 0));
    }

    /* Detail panel */
    if (dr) {
      static char dlines[64][80];
      static int dkinds[64];
      int nd = 0;
#define DADD2(k, fmt, ...)                                                     \
  do {                                                                         \
    if (nd < 63) {                                                             \
      snprintf(dlines[nd], 79, fmt, ##__VA_ARGS__);                            \
      dkinds[nd] = k;                                                          \
      nd++;                                                                    \
    }                                                                          \
  } while (0)
      DADD2(1, "%s", dr->name);
      if (dr->aliases)
        DADD2(3, "Also: %s", dr->aliases);
      if (dr->group)
        DADD2(3, "[%s]", dr->group);
      DADD2(0, "");
      DADD2(1, "Syntax:");
      /* split syntax on \n */
      {
        char tmp[80];
        strncpy(tmp, dr->syntax, 79);
        tmp[79] = 0;
        char *line = tmp, *nl;
        while ((nl = strchr(line, '\n')) || *line) {
          if (nl)
            *nl = 0;
          DADD2(2, "  %s", line);
          if (!nl)
            break;
          line = nl + 1;
        }
      }
      DADD2(0, "");
      /* desc, wrapped */
      {
        const char *dp = dr->desc;
        while (*dp) {
          /* handle \n in desc */
          const char *nl = strchr(dp, '\n');
          int seg = nl ? (int)(nl - dp) : (int)strlen(dp);
          int avail = detail_w - 3;
          if (avail < 10)
            avail = 10;
          while (seg > 0) {
            int take = seg < avail ? seg : avail;
            while (take < seg && dp[take] != ' ')
              take--;
            if (take == 0)
              take = seg < avail ? seg : avail;
            char tmp2[80] = "";
            int cp = take < 79 ? take : 79;
            memcpy(tmp2, dp, (size_t)cp);
            tmp2[cp] = 0;
            DADD2(0, "%s", tmp2);
            dp += take;
            seg -= take;
            while (*dp == ' ') {
              dp++;
              seg--;
            }
          }
          if (nl)
            dp = nl + 1;
          else
            break;
        }
      }
      if (dr->example) {
        DADD2(0, "");
        DADD2(1, "Example:");
        char tmp[80];
        strncpy(tmp, dr->example, 79);
        tmp[79] = 0;
        char *line = tmp, *nl;
        while ((nl = strchr(line, '\n')) || *line) {
          if (nl)
            *nl = 0;
          DADD2(2, "  %s", line);
          if (!nl)
            break;
          line = nl + 1;
        }
      }
#undef DADD2

      if (detail_top > nd - (win_h - 3))
        detail_top = nd - (win_h - 3);
      if (detail_top < 0)
        detail_top = 0;

      for (int di = 0; di < win_h - 3 && (detail_top + di) < nd; di++) {
        int dk = dkinds[detail_top + di];
        int row = di + 1, col = detail_x + 1, avail = detail_w - 2;
        switch (dk) {
        case 1:
          wattron(win, theme_attr(theme, TK_LABEL, 0) | A_BOLD);
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          wattroff(win, theme_attr(theme, TK_LABEL, 0) | A_BOLD);
          break;
        case 2:
          wattron(win, theme_attr(theme, TK_NUMBER, 0));
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          wattroff(win, theme_attr(theme, TK_NUMBER, 0));
          break;
        case 3:
          wattron(win, theme_attr(theme, TK_COMMENT, 0));
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          wattroff(win, theme_attr(theme, TK_COMMENT, 0));
          break;
        default:
          mvwprintw(win, row, col, "%-*.*s", avail, avail,
                    dlines[detail_top + di]);
          break;
        }
      }
    }

    wrefresh(win);

    int ch = dialog_getch(win);

    if (searching) {
      if (ch == 27 || ch == '\n' || ch == '\r') {
        searching = 0;
        sel = 0;
        detail_top = 0;
        curs_set(0);
      } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
        int sl = (int)strlen(search);
        if (sl > 0)
          search[sl - 1] = 0;
        sel = 0;
        detail_top = 0;
      } else if (ch >= 32 && ch < 127 && (int)strlen(search) < 14) {
        int sl = (int)strlen(search);
        search[sl] = (char)ch;
        search[sl + 1] = 0;
        sel = 0;
        detail_top = 0;
      }
      continue;
    }

    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        int ry = ev.y - wy, rx = ev.x - wx;
        bool clicked = (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED |
                                     BUTTON1_RELEASED)) != 0;
        if (clicked && ry >= 1 && ry < win_h - 1) {
          if (rx < list_w) {
            focus = 0;
            int ni = list_top + (ry - 1);
            if (ni >= 0 && ni < nf) {
              sel = ni;
              detail_top = 0;
            }
          } else {
            focus = 1;
          }
        }
        if (rx >= list_w) {
          if (ev.bstate & BUTTON4_PRESSED)
            detail_top -= 3;
          if (ev.bstate & BUTTON5_PRESSED)
            detail_top += 3;
        }
      }
      continue;
    }

    if (ch == 'q' || ch == 'Q' || ch == 27)
      break;
    if (ch == '\t') {
      focus = 1 - focus;
      continue;
    }
    if (ch == '/') {
      searching = 1;
      search[0] = 0;
      sel = 0;
      detail_top = 0;
      curs_set(1);
      continue;
    }

    if (focus == 0) {
      if (ch == KEY_UP || ch == 'k') {
        sel--;
        detail_top = 0;
      }
      if (ch == KEY_DOWN || ch == 'j') {
        sel++;
        detail_top = 0;
      }
      if (ch == KEY_PPAGE) {
        sel -= win_h / 2;
        detail_top = 0;
      }
      if (ch == KEY_NPAGE) {
        sel += win_h / 2;
        detail_top = 0;
      }
      if (ch == KEY_HOME) {
        sel = 0;
        detail_top = 0;
      }
      if (ch == KEY_END) {
        sel = nf - 1;
        detail_top = 0;
      }
      if (ch == '\n' || ch == '\r') {
        focus = 1;
      }
    } else {
      if (ch == KEY_UP || ch == 'k')
        detail_top--;
      if (ch == KEY_DOWN || ch == 'j')
        detail_top++;
      if (ch == KEY_PPAGE || ch == 'b')
        detail_top -= win_h - 3;
      if (ch == KEY_NPAGE || ch == ' ')
        detail_top += win_h - 3;
      if (ch == KEY_HOME || ch == 'g')
        detail_top = 0;
      if (ch == KEY_LEFT) {
        focus = 0;
      }
    }
    if (sel < 0)
      sel = 0;
    if (sel >= nf && nf > 0)
      sel = nf - 1;
  }

  delwin(win);
  touchwin(stdscr);
  refresh();
}

// About dialog
// ---------------------------------------------------------------------------
static void do_about(const theme_t *theme) {
  // Content lines (mirroring the GTK about dialog)
  static const char *const lines[] = {
      APP_NAME, "", APP_AUTHOR, APP_DESC, APP_VERSION,
  };
  static const int n_lines = 5;
  static const char *const btn_label = "[ OK ]";

  // Measure width: widest content line or button, plus 4 padding
  int content_w = (int)strlen(btn_label);
  for (int i = 0; i < n_lines; i++) {
    int l = (int)strlen(lines[i]);
    if (l > content_w)
      content_w = l;
  }
  int win_w = content_w + 8; // 4 padding each side
  int win_h = n_lines + 5;   // lines + blank + button row + 2 border + 1 gap
  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int wy = (sh - win_h) / 2;
  if (wy < 1)
    wy = 1;
  int wx = (sw - win_w) / 2;
  if (wx < 0)
    wx = 0;

  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return;
  keypad(win, true);

  // Button row (window-relative): last row before bottom border
  int btn_row = win_h - 2;
  int btn_col = (win_w - (int)strlen(btn_label)) / 2;

  flushinp();
  curs_set(0);
  while (1) {
    // ---- Draw ----
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    wattron(win, theme_attr(theme, TK_DEFAULT, 0));
    box(win, 0, 0);

    // Title in border
    {
      const char *t = " About ";
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, (win_w - (int)strlen(t)) / 2, "%s", t);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }

    // Content lines, centred
    for (int i = 0; i < n_lines; i++) {
      int llen = (int)strlen(lines[i]);
      int lx = (win_w - llen) / 2;
      if (lx < 1)
        lx = 1;
      // First line (app name) gets directive/title colour + bold
      if (i == 0) {
        wattron(win, theme_attr(theme, TK_DIRECTIVE, 0) | A_BOLD);
        mvwprintw(win, 1 + i, lx, "%s", lines[i]);
        wattroff(win, theme_attr(theme, TK_DIRECTIVE, 0) | A_BOLD);
      } else {
        wattron(win, theme_attr(theme, TK_DEFAULT, 0));
        mvwprintw(win, 1 + i, lx, "%s", lines[i]);
        wattroff(win, theme_attr(theme, TK_DEFAULT, 0));
      }
    }

    // OK button
    wattron(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD);
    mvwprintw(win, btn_row, btn_col, "%s", btn_label);
    wattroff(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD);

    wrefresh(win);

    // ---- Input ----
    int ch = dialog_getch(win);

    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        // Check click on button (absolute coords → window-relative)
        int ry = ev.y - wy, rx = ev.x - wx;
        bool clicked = (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED |
                                     BUTTON1_RELEASED)) != 0;
        if (clicked && ry == btn_row && rx >= btn_col &&
            rx < btn_col + (int)strlen(btn_label))
          break;
      }
      continue;
    }

    if (ch == 27 || ch == KEY_EXIT || ch == '\n' || ch == '\r' || ch == ' ')
      break;
  }

  flushinp();
  curs_set(1);
  delwin(win);
}

// ---------------------------------------------------------------------------
// Recent files (MRU) — unchanged
// ---------------------------------------------------------------------------
#define MRU_MAX 10
static void recent_path(char *out, size_t cap) {
  char dir[1024];
  tui_config_dir(dir, sizeof(dir));
  snprintf(out, cap, "%s/recent.txt", dir);
}

static int recent_load(char items[MRU_MAX][1024]) {
  for (int i = 0; i < MRU_MAX; i++)
    items[i][0] = 0;
  char p[1024];
  recent_path(p, sizeof(p));
  FILE *f = fopen(p, "rb");
  if (!f)
    return 0;
  int n = 0;
  while (n < MRU_MAX && fgets(items[n], 1024, f)) {
    size_t len = strlen(items[n]);
    while (len && (items[n][len - 1] == '\n' || items[n][len - 1] == '\r'))
      items[n][--len] = 0;
    if (items[n][0])
      n++;
  }
  fclose(f);
  return n;
}

/* Persist the most-recently-used file list to ~/.config/6502caEd/recent. */
static void recent_save(char items[MRU_MAX][1024], int n) {
  ensure_config_dir();
  char p[1024];
  recent_path(p, sizeof(p));
  FILE *f = fopen(p, "wb");
  if (!f)
    return;
  for (int i = 0; i < n && i < MRU_MAX; i++)
    if (items[i][0]) {
      fputs(items[i], f);
      fputc('\n', f);
    }
  fclose(f);
}

/* Prepend path to the MRU list, removing any previous occurrence,
 * then save the updated list to disk. */
static void recent_add(const char *path) {
  if (!path || !path[0])
    return;
  char items[MRU_MAX][1024], tmp[MRU_MAX][1024];
  int n = recent_load(items), out_n = 0;
  strncpy(tmp[out_n], path, 1023);
  tmp[out_n][1023] = 0;
  out_n++;
  for (int i = 0; i < n && out_n < MRU_MAX; i++) {
    if (!strcmp(items[i], path))
      continue;
    strncpy(tmp[out_n], items[i], 1023);
    tmp[out_n][1023] = 0;
    out_n++;
  }
  for (int i = 0; i < MRU_MAX; i++)
    strncpy(items[i], tmp[i], 1024);
  recent_save(items, out_n);
}

// ---------------------------------------------------------------------------
// File operations (unchanged)
// ---------------------------------------------------------------------------

static int confirm_discard_or_save(const theme_t *theme, buffer_t *buf,
                                   char *io_path, size_t cap,
                                   const char *suggested) {
  (void)suggested; (void)io_path; (void)cap;
  if (!buf->modified)
    return 1;
  int r = dialog_save_changes(theme);
  if (r == 2) return 2;
  if (r == 1) return 1;
  return 0;
}

// ---------------------------------------------------------------------------
// File selector
// ---------------------------------------------------------------------------
#include <dirent.h>
#include <errno.h>

typedef enum { FS_OPEN, FS_SAVE } fs_mode_t;

// Extension filter table
static const char *const FS_EXTS[] = {"*.asm", "*.s", "*.inc", "*.65s", "*.*"};
#define FS_N_EXTS 5

// One directory entry
typedef struct {
  char name[256];
  int is_dir;
} fs_entry_t;

static int fs_entry_cmp(const void *a, const void *b) {
  const fs_entry_t *ea = (const fs_entry_t *)a;
  const fs_entry_t *eb = (const fs_entry_t *)b;
  // ".." always sorts first, regardless of other names
  int a_dotdot = (strcmp(ea->name, "..") == 0);
  int b_dotdot = (strcmp(eb->name, "..") == 0);
  if (a_dotdot)
    return -1;
  if (b_dotdot)
    return 1;
  if (ea->is_dir != eb->is_dir)
    return eb->is_dir - ea->is_dir; // dirs first
  return strcasecmp(ea->name, eb->name);
}

static int fs_ext_match(const char *name, const char *pat) {
  // pat is like "*.asm" or "*.*"
  if (strcmp(pat, "*.*") == 0)
    return 1;
  const char *dot = strrchr(pat, '.');
  if (!dot)
    return 1;
  const char *ndot = strrchr(name, '.');
  if (!ndot)
    return 0;
  return strcasecmp(ndot, dot) == 0;
}

// Load directory entries into a malloc'd array; returns count, *out must be
// freed.
static int fs_load_dir(const char *path, const char *ext_pat,
                       fs_entry_t **out) {
  DIR *d = opendir(path);
  if (!d)
    return 0;
  int cap = 64, n = 0;
  fs_entry_t *arr = (fs_entry_t *)malloc((size_t)cap * sizeof(fs_entry_t));
  if (!arr) {
    closedir(d);
    return 0;
  }

  // Always add ".." unless we're at root
  if (strcmp(path, "/") != 0) {
    strncpy(arr[n].name, "..", sizeof(arr[n].name) - 1);
    arr[n].name[sizeof(arr[n].name) - 1] = 0;
    arr[n].is_dir = 1;
    n++;
  }

  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.')
      continue; // skip hidden + . + ..
    // Stat to determine type
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", path, de->d_name);
    struct stat st;
    if (stat(full, &st) != 0)
      continue;
    int is_dir = S_ISDIR(st.st_mode);
    if (!is_dir && !fs_ext_match(de->d_name, ext_pat))
      continue;
    if (n >= cap) {
      cap *= 2;
      fs_entry_t *tmp =
          (fs_entry_t *)realloc(arr, (size_t)cap * sizeof(fs_entry_t));
      if (!tmp)
        break;
      arr = tmp;
    }
    strncpy(arr[n].name, de->d_name, sizeof(arr[n].name) - 1);
    arr[n].name[sizeof(arr[n].name) - 1] = 0;
    arr[n].is_dir = is_dir;
    n++;
  }
  closedir(d);
  qsort(arr, (size_t)n, sizeof(fs_entry_t), fs_entry_cmp);
  *out = arr;
  return n;
}

// Normalise a path: resolve ".." segments, ensure no trailing slash (except
// root)
static void fs_normalise(char *path) {
  // Simple in-place ".." resolution
  char tmp[1024];
  strncpy(tmp, path, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = 0;
  char *parts[128];
  int np = 0;
  char *tok = strtok(tmp, "/");
  while (tok && np < 127) {
    if (strcmp(tok, "..") == 0) {
      if (np > 0)
        np--;
    } else if (strcmp(tok, ".") != 0) {
      parts[np++] = tok;
    }
    tok = strtok(NULL, "/");
  }
  if (np == 0) {
    strcpy(path, "/");
    return;
  }
  /* Bounds-checked rebuild — callers always pass a 1024-byte buffer. */
  size_t pos = 0;
  const size_t cap = 1024;
  path[0] = 0;
  for (int i = 0; i < np; i++) {
    size_t seg = strlen(parts[i]);
    if (pos + 1 + seg + 1 > cap)
      break; /* truncate on overflow */
    path[pos++] = '/';
    memcpy(path + pos, parts[i], seg);
    pos += seg;
    path[pos] = 0;
  }
}

// Returns 1 if selection confirmed (path written to out_path), 0 if cancelled.
// out_path must be at least 1024 bytes.
static int file_selector(const theme_t *theme, fs_mode_t mode,
                         const char *start_path, char *out_path) {
  // Determine starting directory and filename
  char cwd[1024];
  char fname[256];
  fname[0] = 0;

  if (start_path && start_path[0]) {
    // Check if it's a directory or a file path
    struct stat st;
    if (stat(start_path, &st) == 0 && S_ISDIR(st.st_mode)) {
      strncpy(cwd, start_path, sizeof(cwd) - 1);
      cwd[sizeof(cwd) - 1] = 0;
    } else {
      // Split directory / filename
      strncpy(cwd, start_path, sizeof(cwd) - 1);
      cwd[sizeof(cwd) - 1] = 0;
      char *sl = strrchr(cwd, '/');
      if (sl) {
        strncpy(fname, sl + 1, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = 0;
        *sl = 0;
        if (!cwd[0])
          strcpy(cwd, "/");
      } else {
        // relative — prepend cwd
        if (!getcwd(cwd, sizeof(cwd)))
          strcpy(cwd, ".");
        strncpy(fname, start_path, sizeof(fname) - 1);
      }
    }
  } else {
    if (!getcwd(cwd, sizeof(cwd)))
      strcpy(cwd, ".");
    if (mode == FS_SAVE)
      strncpy(fname, "newfile.asm", sizeof(fname) - 1);
  }
  fs_normalise(cwd);

  int ext_idx = 0; // current extension filter

  // Load initial directory
  fs_entry_t *entries = NULL;
  int n_entries = 0;
  int list_sel = 0; // selected item index
  int list_top = 0; // scroll offset

  // Input field state (filename box)
  int inp_cur = (int)strlen(fname);
  int inp_scroll = 0;

  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  // Window: full screen minus small margin
  int wh = sh - 2;
  if (wh < 10)
    wh = 10;
  int ww = sw - 4;
  if (ww < 40)
    ww = 40;
  int wy = (sh - wh) / 2;
  if (wy < 0)
    wy = 0;
  int wx = (sw - ww) / 2;
  if (wx < 0)
    wx = 0;

  WINDOW *win = newwin(wh, ww, wy, wx);
  if (!win)
    return 0;
  keypad(win, true);
  curs_set(1);

// Layout inside window (all coords relative to win):
//  row 0       : box top border + title
//  row 1       : path bar
//  row 2       : ext filter tabs
//  row 3       : divider
//  row 4..wh-5 : file list
//  row wh-4    : divider
//  row wh-3    : filename input label + field
//  row wh-2    : help line
//  row wh-1    : box bottom border
#define FS_LIST_TOP 4
#define FS_LIST_BOT (wh - 5) // inclusive last list row
#define FS_INPUT_ROW (wh - 3)
#define FS_HELP_ROW (wh - 2)
#define FS_INNER_W (ww - 2) // usable width inside border

  int list_h = FS_LIST_BOT - FS_LIST_TOP + 1;
  if (list_h < 1)
    list_h = 1;

// Inp field: after "Filename: " label (10 chars)
#define FS_INP_LABEL "Filename: "
#define FS_INP_LABEL_W 10
  int inp_w = FS_INNER_W - FS_INP_LABEL_W - 1;
  if (inp_w < 10)
    inp_w = 10;

  int need_reload = 1;
  // Track last-click for double-click detection
  int last_click_row = -1;
  // POSIX clock for double-click timing not available easily; use a counter
  // instead
  int last_click_count = 0;

  while (1) {
    if (need_reload) {
      free(entries);
      entries = NULL;
      n_entries = fs_load_dir(cwd, FS_EXTS[ext_idx], &entries);
      // Try to re-select fname in list
      list_sel = 0;
      for (int i = 0; i < n_entries; i++) {
        if (!entries[i].is_dir && strcmp(entries[i].name, fname) == 0) {
          list_sel = i;
          break;
        }
      }
      if (list_sel >= n_entries)
        list_sel = n_entries > 0 ? 0 : 0;
      list_top = 0;
      if (list_sel >= list_h)
        list_top = list_sel - list_h / 2;
      need_reload = 0;
    }

    // Clamp scroll
    if (list_top > n_entries - list_h)
      list_top = n_entries - list_h;
    if (list_top < 0)
      list_top = 0;
    if (list_sel >= n_entries)
      list_sel = n_entries > 0 ? n_entries - 1 : 0;

    // Clamp input scroll
    if (inp_cur > (int)strlen(fname))
      inp_cur = (int)strlen(fname);
    if (inp_cur < inp_scroll)
      inp_scroll = inp_cur;
    if (inp_cur - inp_scroll >= inp_w)
      inp_scroll = inp_cur - inp_w + 1;

    // ---- Draw ----
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    wattron(win, theme_attr(theme, TK_DEFAULT, 0));
    box(win, 0, 0);

    // Title — fs_title
    {
      const char *title = (mode == FS_OPEN) ? " Open File " : " Save As ";
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, (ww - (int)strlen(title)) / 2, "%s", title);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }

    // Path bar (row 1) — fs_path
    {
      char pbar[1100];
      snprintf(pbar, sizeof(pbar), " %s", cwd);
      wattron(win, theme_attr(theme, TK_FS_PATH, 0));
      for (int i = 1; i <= FS_INNER_W; i++)
        mvwaddch(win, 1, i, ' ');
      mvwaddnstr(win, 1, 1, pbar, FS_INNER_W);
      wattroff(win, theme_attr(theme, TK_FS_PATH, 0));
    }

    // Extension filter tabs (row 2) — fs_tab / fs_tab_active
    {
      wattron(win, theme_attr(theme, TK_DEFAULT, 0));
      for (int i = 1; i <= FS_INNER_W; i++)
        mvwaddch(win, 2, i, ' ');
      int tx = 1;
      for (int i = 0; i < FS_N_EXTS && tx < FS_INNER_W - 2; i++) {
        int ew = (int)strlen(FS_EXTS[i]) + 2;
        if (i == ext_idx) {
          wattron(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
          mvwprintw(win, 2, tx, " %s ", FS_EXTS[i]);
          wattroff(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE);
        } else {
          wattron(win, theme_attr(theme, TK_FS_TAB, 0));
          mvwprintw(win, 2, tx, " %s ", FS_EXTS[i]);
          wattroff(win, theme_attr(theme, TK_FS_TAB, 0));
        }
        tx += ew + 1;
      }
    }

    // Dividers — operator colour
    wattron(win, theme_attr(theme, TK_OPERATOR, 0));
    mvwhline(win, 3, 1, ACS_HLINE, FS_INNER_W);
    mvwhline(win, wh - 4, 1, ACS_HLINE, FS_INNER_W);
    wattroff(win, theme_attr(theme, TK_OPERATOR, 0));

    // File list
    if (n_entries == 0) {
      wattron(win, theme_attr(theme, TK_FS_HELP, 0));
      mvwprintw(win, FS_LIST_TOP, 2, "(empty)");
      wattroff(win, theme_attr(theme, TK_FS_HELP, 0));
    } else {
      for (int i = 0; i < list_h; i++) {
        int idx = list_top + i;
        if (idx >= n_entries)
          break;
        int row = FS_LIST_TOP + i;
        bool sel = (idx == list_sel);

        if (entries[idx].is_dir) {
          chtype attr = theme_attr(theme, sel ? TK_FS_SELECTED : TK_FS_DIR, 0);
          if (sel)
            attr |= A_REVERSE;
          wattron(win, attr);
          char line[280];
          snprintf(line, sizeof(line), " +  %-*s", FS_INNER_W - 4,
                   entries[idx].name);
          mvwaddnstr(win, row, 1, line, FS_INNER_W);
          wattroff(win, attr);
        } else {
          chtype attr = sel ? (theme_attr(theme, TK_FS_SELECTED, 0) | A_REVERSE)
                         : theme_attr(theme, TK_FS_FILE, 0);
          wattron(win, attr);
          char line[280];
          snprintf(line, sizeof(line), "    %-*s", FS_INNER_W - 4,
                   entries[idx].name);
          mvwaddnstr(win, row, 1, line, FS_INNER_W);
          wattroff(win, attr);
        }
      }
      // Scrollbar
      if (n_entries > list_h) {
        int bar_h = list_h * list_h / n_entries;
        if (bar_h < 1)
          bar_h = 1;
        int bar_y = list_top * list_h / n_entries;
        wattron(win, theme_attr(theme, TK_OPERATOR, 0));
        for (int i = 0; i < list_h; i++)
          mvwaddch(win, FS_LIST_TOP + i, ww - 2,
                   (i >= bar_y && i < bar_y + bar_h) ? ACS_BLOCK : ACS_VLINE);
        wattroff(win, theme_attr(theme, TK_OPERATOR, 0));
      }
    }

    // Filename label — fs_help colour, bold
    wattron(win, theme_attr(theme, TK_FS_HELP, 0) | A_BOLD);
    mvwprintw(win, FS_INPUT_ROW, 1, FS_INP_LABEL);
    wattroff(win, theme_attr(theme, TK_FS_HELP, 0) | A_BOLD);

    // Filename input field — fs_input reversed
    {
      chtype attr = theme_attr(theme, TK_FS_INPUT, 0) | A_REVERSE;
      wattron(win, attr);
      int fx = 1 + FS_INP_LABEL_W;
      for (int i = 0; i < inp_w; i++)
        mvwaddch(win, FS_INPUT_ROW, fx + i, ' ');
      int len = (int)strlen(fname);
      for (int i = 0; i < inp_w && (inp_scroll + i) < len; i++)
        mvwaddch(win, FS_INPUT_ROW, fx + i,
                 (unsigned char)fname[inp_scroll + i]);
      wattroff(win, attr);
      int cur_x = fx + (inp_cur - inp_scroll);
      if (cur_x >= fx && cur_x < fx + inp_w)
        wmove(win, FS_INPUT_ROW, cur_x);
    }

    // Help line — fs_help
    {
      const char *help =
          mode == FS_OPEN
              ? "Up/Dn=navigate  Enter=open/cd  Tab=filter  Bksp=up  ESC=cancel"
              : "Up/Dn=navigate  Enter=save/cd  Tab=filter  Bksp=up  "
                "ESC=cancel";
      wattron(win, theme_attr(theme, TK_FS_HELP, 0));
      for (int i = 1; i <= FS_INNER_W; i++)
        mvwaddch(win, FS_HELP_ROW, i, ' ');
      mvwaddnstr(win, FS_HELP_ROW, 1, help, FS_INNER_W);
      wattroff(win, theme_attr(theme, TK_FS_HELP, 0));
    }

    wrefresh(win);

    // ---- Input ----
    int ch = dialog_getch(win);

    // Mouse
    if (ch == KEY_MOUSE) {
      MEVENT me;
      if (getmouse(&me) != OK)
        continue;
      // Convert to window-relative coords
      int ry = me.y - wy, rx = me.x - wx;
      bool clicked = (me.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED |
                                   BUTTON1_RELEASED)) != 0;
      if (!clicked)
        continue;

      // Click on ext filter tabs (row 2)
      if (ry == 2) {
        int tx = 1;
        for (int i = 0; i < FS_N_EXTS; i++) {
          int ew = (int)strlen(FS_EXTS[i]) + 2;
          if (rx >= tx && rx < tx + ew) {
            ext_idx = i;
            need_reload = 1;
            break;
          }
          tx += ew + 1;
        }
        continue;
      }
      // Click on file list
      if (ry >= FS_LIST_TOP && ry <= FS_LIST_BOT) {
        int idx = list_top + (ry - FS_LIST_TOP);
        if (idx >= 0 && idx < n_entries) {
          if (idx == last_click_row && last_click_count > 0) {
            // Double-click: treat as Enter
            last_click_count = 0;
            last_click_row = -1;
            ch = '\n';
            goto handle_enter;
          } else {
            list_sel = idx;
            last_click_row = idx;
            last_click_count = 1;
            // Fill filename field from selection
            if (!entries[idx].is_dir) {
              strncpy(fname, entries[idx].name, sizeof(fname) - 1);
              inp_cur = (int)strlen(fname);
              inp_scroll = 0;
            }
          }
        }
        continue;
      }
      continue;
    }

    if (ch == 27 || ch == KEY_EXIT) {
      free(entries);
      delwin(win);
      curs_set(1);
      return 0;
    }

    // Tab: cycle extension filter
    if (ch == '\t') {
      ext_idx = (ext_idx + 1) % FS_N_EXTS;
      need_reload = 1;
      continue;
    }

    // Backspace in list context → go up a directory
    // In filename field it deletes; we route based on whether fname is empty
    if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (inp_cur > 0) {
        // Delete char before cursor in filename
        memmove(fname + inp_cur - 1, fname + inp_cur,
                strlen(fname) - (size_t)inp_cur + 1);
        inp_cur--;
      } else {
        // Go up directory
        char *sl = strrchr(cwd, '/');
        if (sl && sl != cwd) {
          *sl = 0;
        } else if (sl == cwd && cwd[1]) {
          cwd[1] = 0;
        }
        // else already at root
        need_reload = 1;
      }
      continue;
    }

    // Arrow keys: navigate list
    if (ch == KEY_UP) {
      if (list_sel > 0)
        list_sel--;
      if (list_sel < list_top)
        list_top = list_sel;
      if (!entries[list_sel].is_dir) {
        strncpy(fname, entries[list_sel].name, sizeof(fname) - 1);
        inp_cur = (int)strlen(fname);
        inp_scroll = 0;
      }
      last_click_row = -1;
      continue;
    }
    if (ch == KEY_DOWN) {
      if (list_sel + 1 < n_entries)
        list_sel++;
      if (list_sel >= list_top + list_h)
        list_top = list_sel - list_h + 1;
      if (!entries[list_sel].is_dir) {
        strncpy(fname, entries[list_sel].name, sizeof(fname) - 1);
        inp_cur = (int)strlen(fname);
        inp_scroll = 0;
      }
      last_click_row = -1;
      continue;
    }
    if (ch == KEY_PPAGE) {
      list_sel -= list_h;
      if (list_sel < 0)
        list_sel = 0;
      list_top = list_sel;
      last_click_row = -1;
      continue;
    }
    if (ch == KEY_NPAGE) {
      list_sel += list_h;
      if (list_sel >= n_entries)
        list_sel = n_entries > 0 ? n_entries - 1 : 0;
      if (list_sel >= list_top + list_h)
        list_top = list_sel - list_h + 1;
      last_click_row = -1;
      continue;
    }
    if (ch == KEY_HOME) {
      list_sel = 0;
      list_top = 0;
      last_click_row = -1;
      continue;
    }
    if (ch == KEY_END) {
      list_sel = n_entries > 0 ? n_entries - 1 : 0;
      if (list_sel >= list_top + list_h)
        list_top = list_sel - list_h + 1;
      last_click_row = -1;
      continue;
    }

    // Left arrow in filename → move cursor left
    if (ch == KEY_LEFT) {
      if (inp_cur > 0)
        inp_cur--;
      continue;
    }
    if (ch == KEY_RIGHT) {
      if (inp_cur < (int)strlen(fname))
        inp_cur++;
      continue;
    }

    // Enter
    if (ch == '\n' || ch == '\r') {
    handle_enter:;
      // If filename field has a path with '/' treat it as an absolute/relative
      // jump
      if (fname[0] == '/') {
        // absolute path typed in filename box
        struct stat st;
        if (stat(fname, &st) == 0 && S_ISDIR(st.st_mode)) {
          strncpy(cwd, fname, sizeof(cwd) - 1);
          fname[0] = 0;
          inp_cur = 0;
          need_reload = 1;
          continue;
        }
        // It's a file path — use as-is
        strncpy(out_path, fname, 1023);
        out_path[1023] = 0;
        free(entries);
        delwin(win);
        curs_set(1);
        return 1;
      }
      // Use selected list item if filename matches or is empty
      if (n_entries > 0 && list_sel < n_entries) {
        fs_entry_t *e = &entries[list_sel];
        // Check if the typed fname matches the selected entry or is empty
        bool fname_matches = (fname[0] == 0 || strcmp(fname, e->name) == 0);
        if (e->is_dir && fname_matches) {
          // Navigate into directory
          if (strcmp(e->name, "..") == 0) {
            char *sl = strrchr(cwd, '/');
            if (sl && sl != cwd)
              *sl = 0;
            else if (sl == cwd && cwd[1])
              cwd[1] = 0;
          } else {
            size_t cl = strlen(cwd);
            if (cwd[cl - 1] != '/')
              strncat(cwd, "/", sizeof(cwd) - cl - 1);
            strncat(cwd, e->name, sizeof(cwd) - strlen(cwd) - 1);
          }
          fs_normalise(cwd);
          fname[0] = 0;
          inp_cur = 0;
          need_reload = 1;
          continue;
        }
      }
      // Confirm selection: build full path from cwd + fname
      if (fname[0] == 0)
        continue; // nothing typed
      {
        char full[1024];
        size_t cl = strlen(cwd);
        if (cwd[cl - 1] == '/')
          snprintf(full, sizeof(full), "%s%s", cwd, fname);
        else
          snprintf(full, sizeof(full), "%s/%s", cwd, fname);
        strncpy(out_path, full, 1023);
        out_path[1023] = 0;
      }
      free(entries);
      delwin(win);
      curs_set(1);
      return 1;
    }

    // Printable: type into filename field
    if (ch >= 32 && ch <= 126) {
      size_t fl = strlen(fname);
      if (fl + 1 < sizeof(fname)) {
        memmove(fname + inp_cur + 1, fname + inp_cur, fl - (size_t)inp_cur + 1);
        fname[inp_cur] = (char)ch;
        inp_cur++;
        // If user starts typing, deselect list to avoid confusion
        last_click_row = -1;
      }
      continue;
    }
  }

#undef FS_LIST_TOP
#undef FS_LIST_BOT
#undef FS_INPUT_ROW
#undef FS_HELP_ROW
#undef FS_INNER_W
#undef FS_INP_LABEL
#undef FS_INP_LABEL_W
}

static int do_save_as(const theme_t *theme, buffer_t *buf, char *io_path,
                      size_t cap, const char *suggested) {
  char name[1024];
  const char *start = (suggested && suggested[0]) ? suggested
                      : (io_path && io_path[0])   ? io_path
                                                  : NULL;
  if (!file_selector(theme, FS_SAVE, start, name))
    return 0;
  ensure_asm_suffix(name, sizeof(name));
  if (file_exists(name) &&
      !prompt_yesno(theme, "File exists. Overwrite? (y/n)"))
    return 0;
  if (!buffer_save(buf, name))
    return 0;
  buf->modified = false;
  if (io_path && cap) {
    strncpy(io_path, name, cap - 1);
    io_path[cap - 1] = 0;
  }
  set_term_title("6502caEd", io_path);
  recent_add(io_path);
  return 1;
}

/* Save the buffer.  If io_path is empty, prompts with a file-selector dialog.
 * ret: 1 on success, 0 on cancel or error */
static int do_save(const theme_t *theme, buffer_t *buf, char *io_path,
                   size_t cap, const char *suggested) {
  if (!io_path || !cap)
    return 0;
  if (!io_path[0])
    return do_save_as(theme, buf, io_path, cap, suggested);
  if (!buffer_save(buf, io_path))
    return 0;
  buf->modified = false;
  set_term_title("6502caEd", io_path);
  recent_add(io_path);
  return 1;
}

/* Open a file chosen via the file-selector dialog into buf.
 * Prompts to save unsaved changes first if buf is modified.
 * ret: 1 on success, 0 on cancel */
static int do_open(const theme_t *theme, buffer_t *buf, char *io_path,
                   size_t cap) {
  curs_set(0);
  flushinp();
  char name[1024];
  if (!file_selector(theme, FS_OPEN, io_path && io_path[0] ? io_path : NULL,
                     name))
    return 0;
  ensure_asm_suffix(name, sizeof(name));
  if (!file_exists(name)) {
    if (prompt_yesno(theme, "File does not exist. Create it? (y/n)")) {
      FILE *nf = fopen(name, "wb");
      if (!nf)
        return 0;
      fclose(nf);
    } else
      return 0;
  }
  buffer_t nb;
  if (!buffer_load(&nb, name))
    return 0;
  buffer_free(buf);
  *buf = nb;
  if (io_path && cap) {
    strncpy(io_path, name, cap - 1);
    io_path[cap - 1] = 0;
  }
  set_term_title("6502caEd", io_path);
  recent_add(io_path);
  return 1;
}

/* Show a pop-up menu at (y,x) with n items and return the index selected.
 * sel0 is the initially highlighted item.  Returns -1 on Escape. */
static int menu_select(const theme_t *theme, int y, int x, const char **items,
                       int n, int sel0) {
  (void)theme;
  int w = 0;
  for (int i = 0; i < n; i++) {
    int l = (int)strlen(items[i]);
    if (l > w)
      w = l;
  }
  w += 4;
  int h = n + 2;
  WINDOW *win = newwin(h, w, y, x);
  if (!win)
    return -1;
  keypad(win, true);
  box(win, 0, 0);
  int sel = (sel0 >= 0 && sel0 < n) ? sel0 : 0;
  bool mouse_pressed_inside =
      false; /* track whether a press landed inside us */
  while (1) {
    for (int i = 0; i < n; i++) {
      if (i == sel)
        wattron(win, A_REVERSE);
      mvwprintw(win, 1 + i, 1, "%-*s", w - 2, items[i]);
      if (i == sel)
        wattroff(win, A_REVERSE);
    }
    wrefresh(win);
    int ch = dialog_getch(win);
    if (ch == KEY_UP) {
      if (sel > 0)
        sel--;
      else
        sel = n - 1;
    } else if (ch == KEY_DOWN) {
      if (sel + 1 < n)
        sel++;
      else
        sel = 0;
    } else if (ch == 27) {
      delwin(win);
      return -1;
    } else if (ch == '\n' || ch == '\r') {
      delwin(win);
      return sel;
    } else if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        int ry = ev.y - y, rx = ev.x - x;
        bool inside = (ry >= 1 && ry <= n && rx >= 1 && rx < w - 1);
        bool in_win = (ry >= 0 && ry < h && rx >= 0 && rx < w);
        if (inside)
          sel = ry - 1; /* hover highlight */
        if (ev.bstate & BUTTON1_PRESSED) {
          if (inside) {
            mouse_pressed_inside = true;
          } else if (!in_win) {
            delwin(win);
            return -1;
          } /* press outside = cancel */
        }
        if (ev.bstate & (BUTTON1_CLICKED | BUTTON1_RELEASED)) {
          if (inside && mouse_pressed_inside) {
            sel = ry - 1;
            delwin(win);
            return sel;
          } /* release inside after press inside = select */
          else if (!in_win && mouse_pressed_inside) {
            delwin(win);
            return -1;
          } /* release outside after press inside = cancel */
          /* release from a stale press (mouse_pressed_inside==false) — ignore
           */
        }
      }
    }
  }
}

static int do_open_recent(const theme_t *theme, buffer_t *buf, char *io_path,
                          size_t cap) {
  curs_set(0);
  flushinp(); /* discard stale mouse events from preceding dialog */
  char items[MRU_MAX][1024];
  int n = recent_load(items);
  if (n <= 0)
    return 0;
  const char *view[MRU_MAX];
  for (int i = 0; i < n; i++)
    view[i] = items[i];
  int sel = menu_select(theme, 1, 0, view, n, 0);
  if (sel < 0)
    return 0;
  buffer_t nb;
  if (!buffer_load(&nb, items[sel]))
    return 0;
  buffer_free(buf);
  *buf = nb;
  strncpy(io_path, items[sel], cap - 1);
  io_path[cap - 1] = 0;
  set_term_title("6502caEd", io_path);
  recent_add(io_path);
  return 1;
}

/* Show a scrollable list dialog with title and n items.
 * ret: index of the selected item, or -1 on Escape/cancel */
static int menu_select_list(const theme_t *theme, const char *title,
                            char items[][1024], int n) {
  if (n <= 0)
    return -1;
  int h, w;
  getmaxyx(stdscr, h, w);
  int win_w = w < 60 ? w : 60;
  int win_h = (n + 4 < h - 2) ? (n + 4) : (h - 2);
  WINDOW *win = newwin(win_h, win_w, 1, 0);
  if (!win)
    return -1;
  keypad(win, true);
  curs_set(0);
  int sel = 0;
  int list_h = win_h - 2;
  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);
    if (title) {
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwaddnstr(win, 0, 2, title, win_w - 4);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }
    int top = 0;
    if (sel >= list_h)
      top = sel - (list_h - 1);
    for (int i = 0; i < list_h && (top + i) < n; i++) {
      bool s = (top + i == sel);
      chtype attr = s ? (theme_attr(theme, TK_FS_SELECTED, 0) | A_REVERSE)
                   : theme_attr(theme, TK_DEFAULT, 0);
      wattron(win, attr);
      mvwaddnstr(win, 1 + i, 1, items[top + i], win_w - 2);
      wattroff(win, attr);
    }
    // scrollbar
    if (n > list_h) {
      int bar_h = list_h * list_h / n;
      if (bar_h < 1)
        bar_h = 1;
      int bar_y = top * list_h / n;
      wattron(win, theme_attr(theme, TK_OPERATOR, 0));
      for (int i = 0; i < list_h; i++)
        mvwaddch(win, 1 + i, win_w - 2,
                 (i >= bar_y && i < bar_y + bar_h) ? ACS_BLOCK : ACS_VLINE);
      wattroff(win, theme_attr(theme, TK_OPERATOR, 0));
    }
    wrefresh(win);
    int ch = dialog_getch(win);
    if (ch == 27 || ch == KEY_EXIT) {
      curs_set(1);
      delwin(win);
      return -1;
    }
    if (ch == KEY_UP) {
      if (sel > 0)
        sel--;
    } else if (ch == KEY_DOWN) {
      if (sel + 1 < n)
        sel++;
    } else if (ch == KEY_PPAGE) {
      sel -= list_h;
      if (sel < 0)
        sel = 0;
    } else if (ch == KEY_NPAGE) {
      sel += list_h;
      if (sel >= n)
        sel = n - 1;
    } else if (ch == '\n' || ch == '\r') {
      curs_set(1);
      delwin(win);
      return sel;
    } else if (ch == KEY_MOUSE) {
      MEVENT me;
      if (getmouse(&me) != OK)
        continue;
      bool clicked = (me.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED |
                                   BUTTON1_RELEASED)) != 0;
      if (clicked && me.y >= 1 && me.y < 1 + list_h) {
        int idx = top + (me.y - 1);
        if (idx >= 0 && idx < n) {
          if (idx == sel) {
            curs_set(1);
            delwin(win);
            return sel;
          }
          sel = idx;
        }
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Find / replace (unchanged)
// ---------------------------------------------------------------------------

static int find_next(const buffer_t *buf, view_t *v, const char *needle,
                     int forward) {
  if (!buf || !needle || !needle[0])
    return 0;
  size_t nlen = strlen(needle);
  size_t start_line = v->top + (size_t)v->row;
  size_t start_col = (size_t)v->col;
  if (forward) {
    for (size_t li = start_line; li < buf->count; li++) {
      const char *s = buffer_get_line(buf, li);
      size_t len = strlen(s);
      size_t c0 = (li == start_line) ? (start_col + 1) : 0;
      for (size_t c = c0; c + nlen <= len; c++) {
        if (!strncmp(s + c, needle, nlen)) {
          v->row = (int)(li - v->top);
          int h, w;
          getmaxyx(stdscr, h, w);
          (void)w;
          int text_h = h - 1 - TOPBAR_H;
          ensure_cursor_visible_vertical(buf, v, text_h);
          v->col = (int)c;
          return 1;
        }
      }
    }
  } else {
    for (size_t li = start_line + 1; li-- > 0;) {
      const char *s = buffer_get_line(buf, li);
      size_t len = strlen(s);
      size_t c_end = (li == start_line) ? (start_col ? start_col - 1 : 0)
                                        : (len ? len - 1 : 0);
      for (size_t c = c_end + 1; c-- > 0;) {
        if (c + nlen <= len && !strncmp(s + c, needle, nlen)) {
          v->row = (int)(li - v->top);
          int h, w;
          getmaxyx(stdscr, h, w);
          (void)w;
          int text_h = h - 1 - TOPBAR_H;
          ensure_cursor_visible_vertical(buf, v, text_h);
          v->col = (int)c;
          return 1;
        }
        if (c == 0)
          break;
      }
      if (li == 0)
        break;
    }
  }
  return 0;
}

/* Prompt for a line number and scroll the view to it.
 * ret: 1 if the cursor was moved, 0 on cancel */
static int do_goto(const theme_t *theme, const buffer_t *buf, view_t *v) {
  curs_set(0);
  char tmp[64];
  if (!prompt_string(theme, "Goto line", tmp, sizeof(tmp), NULL))
    return 0;
  char *_gend;
  long n = strtol(tmp, &_gend, 10);
  if (_gend == tmp || n <= 0)
    return 0;
  size_t line0 = (size_t)(n - 1);
  if (line0 >= buf->count)
    line0 = buf->count ? (buf->count - 1) : 0;
  int h, w;
  getmaxyx(stdscr, h, w);
  (void)w;
  int text_h = h - 1 - TOPBAR_H;
  size_t max_top =
      (buf->count > (size_t)text_h) ? (buf->count - (size_t)text_h) : 0;
  v->top = (line0 > (size_t)(text_h / 2)) ? (line0 - (size_t)(text_h / 2)) : 0;
  if (v->top > max_top)
    v->top = max_top;
  v->row = (int)(line0 - v->top);
  v->col = 0;
  return 1;
}

/* Prompt for a search string and a replacement, then perform a
 * find-and-replace across the entire buffer.
 * ret: 1 if any replacements were made, 0 otherwise */
static int do_replace(const theme_t *theme, buffer_t *buf, view_t *v,
                      char *last_search, size_t last_search_cap) {
  char q[128], rpl[128];
  static char last_replace[128];
  if (!prompt_string(theme, "Replace: find", q, sizeof(q),
                     (last_search && last_search[0]) ? last_search : NULL))
    return 0;
  if (!prompt_string(theme, "Replace: with", rpl, sizeof(rpl), last_replace))
    return 0;
  if (last_search && last_search_cap) {
    strncpy(last_search, q, last_search_cap - 1);
    last_search[last_search_cap - 1] = 0;
  }
  strncpy(last_replace, rpl, sizeof(last_replace) - 1);
  last_replace[sizeof(last_replace) - 1] = 0;
  size_t count = 0;
  if (!buffer_replace_all(buf, q, rpl, &count))
    return 0;
  clamp_cursor(buf, v, 1, 1);
  return (int)count;
}

// ---------------------------------------------------------------------------
// NEW: Text transformation helpers
// ---------------------------------------------------------------------------

// Grow a heap buffer. Returns new pointer (old pointer freed on failure).
static char *buf_append(char *out, size_t *len, size_t *cap, const char *src,
                        size_t n) {
  if (*len + n + 1 > *cap) {
    size_t nc = *cap ? *cap * 2 : 256;
    while (nc < *len + n + 1)
      nc *= 2;
    char *np = (char *)realloc(out, nc);
    if (!np) {
      free(out);
      return NULL;
    }
    out = np;
    *cap = nc;
  }
  memcpy(out + *len, src, n);
  *len += n;
  out[*len] = 0;
  return out;
}
static char *buf_appendc(char *out, size_t *len, size_t *cap, char c) {
  return buf_append(out, len, cap, &c, 1);
}

// --- Format layout (port of GTK format_source_text_layout) ---
// Canonicalises blank lines between code blocks. Returns malloc'd string,
// caller frees.
static char *format_layout_text(const char *in) {
  if (!in)
    return strdup("");
  size_t len = 0, cap = 0;
  char *out = NULL;
  int prev_kind = 0; // 0=other/blank, 1=instruction, 2=label/directive
  bool in_block = false;

  const char *p = in;
  while (*p) {
    const char *ls = p;
    const char *le = strchr(p, '\n');
    size_t line_len = le ? (size_t)(le - p) : strlen(p);

    // copy to mutable buf for analysis
    char *line = (char *)malloc(line_len + 1);
    if (!line)
      break;
    memcpy(line, ls, line_len);
    line[line_len] = 0;

    // strip trailing whitespace for analysis only
    char *trimmed = (char *)malloc(line_len + 1);
    if (!trimmed) {
      free(line);
      break;
    }
    memcpy(trimmed, line, line_len + 1);
    size_t tl = strlen(trimmed);
    while (tl && (trimmed[tl - 1] == ' ' || trimmed[tl - 1] == '\t'))
      trimmed[--tl] = 0;

    int kind = 0;
    if (tl == 0) {
      kind = 0; // blank
    } else if (in_block) {
      kind = prev_kind;
      // detect end of block comment
      if (strstr(trimmed, "*/"))
        in_block = false;
    } else if (trimmed[0] == ';' || (trimmed[0] == '/' && trimmed[1] == '/')) {
      kind = prev_kind; // comment: inherit
    } else if (trimmed[0] == '/' && trimmed[1] == '*') {
      in_block = true;
      kind = 2;
    } else {
      // leading whitespace = instruction (kind 1)
      if (trimmed[0] == ' ' || trimmed[0] == '\t') {
        kind = 1;
      } else {
        // label or directive at col 0
        kind = 2;
      }
    }

    // Insert blank line between blocks of different non-blank kinds
    if (kind != 0 && prev_kind != 0 && kind != prev_kind) {
      out = buf_appendc(out, &len, &cap, '\n');
      if (!out) {
        free(line);
        free(trimmed);
        break;
      }
    }

    out = buf_append(out, &len, &cap, line, line_len);
    if (!out) {
      free(line);
      free(trimmed);
      break;
    }
    if (le)
      out = buf_appendc(out, &len, &cap, '\n');
    if (!out) {
      free(line);
      free(trimmed);
      break;
    }

    if (kind != 0)
      prev_kind = kind;
    free(line);
    free(trimmed);
    p = le ? le + 1 : p + line_len;
    if (!le)
      break;
  }
  if (!out)
    out = strdup("");
  return out;
}

// --- Upper/lower case (port of GTK format_source_text, case-only variant) ---
// Only transforms non-comment, non-string tokens that are NOT the first operand
// token. Simpler approach for assembler: uppercase/lowercase everything except
// string literals and comment text. This matches the GTK behaviour closely
// enough.
static char *format_case_text(const char *in, int upper) {
  if (!in)
    return strdup("");
  size_t len = 0, cap = 0;
  char *out = NULL;
  bool in_block = false, in_string = false;
  const char *p = in;
  while (*p) {
    if (*p == '\n') {
      out = buf_appendc(out, &len, &cap, '\n');
      if (!out)
        break;
      in_string = false;
      p++;
      continue;
    }
    if (in_block) {
      if (p[0] == '*' && p[1] == '/') {
        out = buf_append(out, &len, &cap, p, 2);
        if (!out)
          break;
        p += 2;
        in_block = false;
      } else {
        out = buf_appendc(out, &len, &cap, *p++);
        if (!out)
          break;
      }
      continue;
    }
    if (in_string) {
      out = buf_appendc(out, &len, &cap, *p);
      if (*p == '"')
        in_string = false;
      if (!out)
        break;
      p++;
      continue;
    }
    // line comment: copy rest of line verbatim
    if (*p == ';' || (p[0] == '/' && p[1] == '/')) {
      while (*p && *p != '\n') {
        out = buf_appendc(out, &len, &cap, *p++);
        if (!out)
          break;
      }
      continue;
    }
    // block comment
    if (p[0] == '/' && p[1] == '*') {
      out = buf_append(out, &len, &cap, p, 2);
      if (!out)
        break;
      p += 2;
      in_block = true;
      continue;
    }
    // string
    if (*p == '"') {
      out = buf_appendc(out, &len, &cap, *p++);
      if (!out)
        break;
      in_string = true;
      continue;
    }
    // transform
    char c =
        (char)(upper ? toupper((unsigned char)*p) : tolower((unsigned char)*p));
    out = buf_appendc(out, &len, &cap, c);
    if (!out)
      break;
    p++;
  }
  if (!out)
    out = strdup("");
  return out;
}

// --- Instructions-only case converter ---
static const char *g_instrs_6502[] = {
    "adc", "and", "asl", "bcc", "bcs", "beq", "bit", "bmi", "bne", "bpl", "brk",
    "bvc", "bvs", "clc", "cld", "cli", "clv", "cmp", "cpx", "cpy", "dec", "dex",
    "dey", "eor", "inc", "inx", "iny", "jmp", "jsr", "lda", "ldx", "ldy", "lsr",
    "nop", "ora", "pha", "php", "pla", "plp", "rol", "ror", "rti", "rts", "sbc",
    "sec", "sed", "sei", "sta", "stx", "sty", "tax", "tay", "tsx", "txa", "txs",
    "tya", "bra", "phx", "phy", "plx", "ply", "stz", "trb", "tsb", "ahx", "alr",
    "anc", "arr", "axs", "dcp", "isc", "las", "lax", "rla", "rra", "sax", "shx",
    "shy", "slo", "sre", "tas", "xaa", NULL};
static int is_instr_tok(const char *tok) {
  for (int i = 0; g_instrs_6502[i]; i++)
    if (strcasecmp(tok, g_instrs_6502[i]) == 0)
      return 1;
  return 0;
}
static int fmt_is_delim_tui(char c) {
  return c == ' ' || c == '\t' || c == ',' || c == '\r' || c == '\n';
}
static char *format_instrs_only_text(const char *in, int upper) {
  if (!in)
    return strdup("");
  size_t len = 0, cap = 0;
  char *out = NULL;
  bool in_block = false, in_string = false;
  const char *p = in;
  while (*p) {
    if (in_block) {
      if (p[0] == '*' && p[1] == '/') {
        out = buf_append(out, &len, &cap, p, 2);
        p += 2;
        in_block = false;
      } else
        out = buf_appendc(out, &len, &cap, *p++);
      continue;
    }
    if (in_string) {
      out = buf_appendc(out, &len, &cap, *p);
      if (*p == '"')
        in_string = false;
      p++;
      continue;
    }
    if (*p == ';' || (p[0] == '/' && p[1] == '/')) {
      while (*p && *p != '\n')
        out = buf_appendc(out, &len, &cap, *p++);
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      out = buf_append(out, &len, &cap, p, 2);
      p += 2;
      in_block = true;
      continue;
    }
    if (*p == '"') {
      out = buf_appendc(out, &len, &cap, *p++);
      in_string = true;
      continue;
    }
    if (fmt_is_delim_tui(*p) || *p == '\n') {
      out = buf_appendc(out, &len, &cap, *p++);
      continue;
    }
    const char *t0 = p;
    while (*p && !fmt_is_delim_tui(*p) && *p != '\n') {
      if (*p == ';' || (*p == '/' && (p[1] == '/' || p[1] == '*')))
        break;
      if (*p == '"')
        break;
      p++;
    }
    size_t tl = (size_t)(p - t0);
    char tmp[64];
    int heap = 0;
    char *tok;
    if (tl < sizeof(tmp)) {
      memcpy(tmp, t0, tl);
      tmp[tl] = 0;
      tok = tmp;
    } else {
      tok = (char *)malloc(tl + 1);
      if (!tok) {
        out = buf_append(out, &len, &cap, t0, tl);
        continue;
      }
      memcpy(tok, t0, tl);
      tok[tl] = 0;
      heap = 1;
    }
    if (is_instr_tok(tok))
      for (size_t k = 0; k < tl; k++)
        out = buf_appendc(out, &len, &cap,
                          (char)(upper ? toupper((unsigned char)tok[k])
                                       : tolower((unsigned char)tok[k])));
    else
      out = buf_append(out, &len, &cap, t0, tl);
    if (heap)
      free(tok);
  }
  if (!out)
    out = strdup("");
  return out;
}

// --- Unicode replace (port of GTK unicode_common_replace_all_in_text) ---
// Replaces common "smart" unicode punctuation with ASCII equivalents.
// Operates on UTF-8 input.
typedef struct {
  unsigned long cp;
  const char *repl;
} ucmap_t;
static const ucmap_t g_ucmap[] = {
    {0x2018, "'"},   {0x2019, "'"},  {0x201A, "'"},  {0x201B, "'"},
    {0x2032, "'"},   {0x201C, "\""}, {0x201D, "\""}, {0x201E, "\""},
    {0x201F, "\""},  {0x00AB, "\""}, {0x00BB, "\""}, {0x2033, "\""},
    {0x2010, "-"},   {0x2011, "-"},  {0x2012, "-"},  {0x2013, "-"},
    {0x2014, "-"},   {0x2015, "-"},  {0x2212, "-"},  {0x2500, "-"},
    {0x2501, "-"},   {0x2502, "|"},  {0x2503, "|"},  {0x250C, "+"},
    {0x2510, "+"},   {0x2514, "+"},  {0x2518, "+"},  {0x251C, "+"},
    {0x2524, "+"},   {0x252C, "+"},  {0x2534, "+"},  {0x253C, "+"},
    {0x254B, "+"},   {0x2550, "-"},  {0x2551, "|"},  {0x2554, "+"},
    {0x2557, "+"},   {0x255A, "+"},  {0x255D, "+"},  {0x2560, "+"},
    {0x2563, "+"},   {0x2566, "+"},  {0x2569, "+"},  {0x256C, "+"},
    {0x007C, "|"},   {0x00A6, "|"},  {0x2758, "|"},  {0x2192, "->"},
    {0x21D2, "->"},  {0x2794, "->"}, {0x2190, "<-"}, {0x21D0, "<-"},
    {0x2191, "^"},   {0x21D1, "^"},  {0x2193, "v"},  {0x21D3, "v"},
    {0x2026, "..."}, {0x2022, "*"},  {0x00B7, "."},  {0x00D7, "x"},
    {0x00F7, "/"},   {0x00A0, " "},  {0x202F, " "},  {0x205F, " "},
    {0x3000, " "},   {0, NULL}};

// Decode one UTF-8 codepoint; advance *pp. Returns 0xFFFD on invalid.
static unsigned long utf8_decode(const unsigned char **pp) {
  const unsigned char *p = *pp;
  unsigned long cp;
  if (!(*p & 0x80)) {
    cp = *p++;
  } else if ((*p & 0xE0) == 0xC0) {
    cp = (*p++ & 0x1F) << 6;
    if ((*p & 0xC0) == 0x80)
      cp |= (*p++ & 0x3F);
    else
      cp = 0xFFFD;
  } else if ((*p & 0xF0) == 0xE0) {
    cp = (*p++ & 0x0F) << 12;
    if ((*p & 0xC0) == 0x80) {
      cp |= (*p++ & 0x3F) << 6;
      if ((*p & 0xC0) == 0x80)
        cp |= (*p++ & 0x3F);
      else
        cp = 0xFFFD;
    } else
      cp = 0xFFFD;
  } else if ((*p & 0xF8) == 0xF0) {
    cp = (*p++ & 0x07) << 18;
    if ((*p & 0xC0) == 0x80) {
      cp |= (*p++ & 0x3F) << 12;
      if ((*p & 0xC0) == 0x80) {
        cp |= (*p++ & 0x3F) << 6;
        if ((*p & 0xC0) == 0x80)
          cp |= (*p++ & 0x3F);
        else
          cp = 0xFFFD;
      } else
        cp = 0xFFFD;
    } else
      cp = 0xFFFD;
  } else {
    cp = 0xFFFD;
    p++;
  }
  *pp = p;
  return cp;
}

// Encode codepoint to UTF-8 in buf (must be >= 5 bytes). Returns bytes written.
static int utf8_encode(unsigned long cp, char *buf) {
  if (cp < 0x80) {
    buf[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    buf[0] = (char)(0xC0 | (cp >> 6));
    buf[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    buf[0] = (char)(0xE0 | (cp >> 12));
    buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    buf[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  buf[0] = (char)(0xF0 | (cp >> 18));
  buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  buf[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

static char *unicode_replace_text(const char *in) {
  if (!in)
    return strdup("");
  size_t len = 0, cap = 0;
  char *out = NULL;
  const unsigned char *p = (const unsigned char *)in;
  while (*p) {
    const unsigned char *before = p;
    unsigned long cp = utf8_decode(&p);
    // look up in map
    const char *repl = NULL;
    for (int i = 0; g_ucmap[i].cp; i++) {
      if (g_ucmap[i].cp == cp) {
        repl = g_ucmap[i].repl;
        break;
      }
    }
    if (repl) {
      out = buf_append(out, &len, &cap, repl, strlen(repl));
    } else {
      // Re-encode original bytes
      char enc[5];
      int nb = utf8_encode(cp, enc);
      (void)before;
      out = buf_append(out, &len, &cap, enc, (size_t)nb);
    }
    if (!out)
      break;
  }
  if (!out)
    out = strdup("");
  return out;
}

typedef enum {
  XFORM_LAYOUT,
  XFORM_UPPER,
  XFORM_LOWER,
  XFORM_UPPER_ALL,
  XFORM_LOWER_ALL,
  XFORM_UPPER_INSTRS,
  XFORM_LOWER_INSTRS,
  XFORM_UNICODE
} xform_t;

// --- Apply a whole-buffer text transform ---
static void apply_text_transform(buffer_t *buf, view_t *v, xform_t kind) {
  // Collect entire buffer text
  size_t total = 0;
  for (size_t i = 0; i < buf->count; i++)
    total += strlen(buf->lines[i]) + 1;
  char *full = (char *)malloc(total + 1);
  if (!full)
    return;
  size_t pos = 0;
  for (size_t i = 0; i < buf->count; i++) {
    size_t l = strlen(buf->lines[i]);
    memcpy(full + pos, buf->lines[i], l);
    pos += l;
    if (i + 1 < buf->count)
      full[pos++] = '\n';
  }
  full[pos] = 0;

  /* Layout and Unicode always operate on the whole buffer.
     Case transforms operate on the current line only. */
  char *result = NULL;
  bool whole_buf = (kind == XFORM_LAYOUT || kind == XFORM_UNICODE);
  if (!whole_buf) {
    /* Replace just the current line */
    size_t cur_line = v->top + (size_t)v->row;
    if (cur_line < buf->count) {
      const char *in = buf->lines[cur_line];
      char *ln_result = NULL;
      switch (kind) {
      case XFORM_UPPER:
        ln_result = format_case_text(in, 1);
        break;
      case XFORM_LOWER:
        ln_result = format_case_text(in, 0);
        break;
      case XFORM_UPPER_ALL: {
        ln_result = strdup(in);
        if (ln_result)
          for (char *q = ln_result; *q; q++)
            *q = (char)toupper((unsigned char)*q);
        break;
      }
      case XFORM_LOWER_ALL: {
        ln_result = strdup(in);
        if (ln_result)
          for (char *q = ln_result; *q; q++)
            *q = (char)tolower((unsigned char)*q);
        break;
      }
      case XFORM_UPPER_INSTRS:
        ln_result = format_instrs_only_text(in, 1);
        break;
      case XFORM_LOWER_INSTRS:
        ln_result = format_instrs_only_text(in, 0);
        break;
      default:
        break;
      }
      if (ln_result) {
        free(buf->lines[cur_line]);
        buf->lines[cur_line] = ln_result;
        buf->modified = true;
        size_t new_len = strlen(ln_result);
        if ((size_t)v->col > new_len)
          v->col = (int)new_len;
      }
    }
    free(full);
    int h2, w2;
    getmaxyx(stdscr, h2, w2);
    int text_h2 = h2 - 1 - TOPBAR_H;
    int w_text2 = (w2 > gutter_w(buf->count ? buf->count : 1))
                      ? (w2 - gutter_w(buf->count ? buf->count : 1))
                      : 0;
    clamp_cursor(buf, v, text_h2, w_text2);
    return;
  }
  switch (kind) {
  case XFORM_LAYOUT:
    result = format_layout_text(full);
    break;
  case XFORM_UNICODE:
    result = unicode_replace_text(full);
    break;
  default:
    free(full);
    return;
  }
  free(full);
  if (!result)
    return;

  // Rebuild buffer from result
  // Free old lines
  for (size_t i = 0; i < buf->count; i++)
    free(buf->lines[i]);
  free(buf->lines);
  buf->lines = NULL;
  buf->count = 0;

  // Split on '\n'
  const char *p = result;
  while (1) {
    const char *nl = strchr(p, '\n');
    size_t ll = nl ? (size_t)(nl - p) : strlen(p);
    char **newp =
        (char **)realloc(buf->lines, (buf->count + 1) * sizeof(char *));
    if (!newp)
      break;
    buf->lines = newp;
    buf->lines[buf->count] = (char *)malloc(ll + 1);
    if (!buf->lines[buf->count])
      break;
    memcpy(buf->lines[buf->count], p, ll);
    buf->lines[buf->count][ll] = 0;
    buf->count++;
    if (!nl)
      break;
    p = nl + 1;
  }
  if (buf->count == 0) {
    buffer_init_empty(buf);
  }
  buf->modified = true;
  free(result);

  // Clamp cursor
  int h, w;
  getmaxyx(stdscr, h, w);
  int text_h = h - 1 - TOPBAR_H;
  int w_text = (w > gutter_w(buf->count ? buf->count : 1))
                   ? (w - gutter_w(buf->count ? buf->count : 1))
                   : 0;
  clamp_cursor(buf, v, text_h, w_text);
}

// --- Remove duplicate lines ---
static void do_remove_dup_lines(buffer_t *buf, view_t *v) {
  if (!buf || buf->count == 0)
    return;
  // Build a seen set using simple O(n^2) comparison (fine for asm files)
  char **kept = (char **)malloc(buf->count * sizeof(char *));
  if (!kept)
    return;
  size_t kn = 0;
  for (size_t i = 0; i < buf->count; i++) {
    const char *s = buf->lines[i];
    bool dup = false;
    for (size_t j = 0; j < kn; j++) {
      if (!strcmp(kept[j], s)) {
        dup = true;
        break;
      }
    }
    if (!dup)
      kept[kn++] = buf->lines[i];
    else
      free(buf->lines[i]);
  }
  free(buf->lines);
  buf->lines = kept;
  buf->count = kn;
  buf->modified = true;
  int h, w;
  getmaxyx(stdscr, h, w);
  int text_h = h - 1 - TOPBAR_H;
  int w_text = (w > gutter_w(buf->count ? buf->count : 1))
                   ? (w - gutter_w(buf->count ? buf->count : 1))
                   : 0;
  clamp_cursor(buf, v, text_h, w_text);
}

// --- Duplicate current line ---
static void do_dup_line(buffer_t *buf, view_t *v) {
  size_t li = v->top + (size_t)v->row;
  if (li >= buf->count)
    return;
  const char *src = buf->lines[li];
  char *copy = strdup(src);
  if (!copy)
    return;
  char **np = (char **)realloc(buf->lines, (buf->count + 1) * sizeof(char *));
  if (!np) {
    free(copy);
    return;
  }
  buf->lines = np;
  memmove(&buf->lines[li + 2], &buf->lines[li + 1],
          (buf->count - li - 1) * sizeof(char *));
  buf->lines[li + 1] = copy;
  buf->count++;
  buf->modified = true;
  // move cursor to duplicated line
  int h, w;
  getmaxyx(stdscr, h, w);
  (void)w;
  int text_h = h - 1 - TOPBAR_H;
  if (v->row < text_h - 1)
    v->row++;
  else
    v->top++;
}

// --- Comment / uncomment current line or selection ---
static void do_comment_toggle(buffer_t *buf, view_t *v, sel_t *sel) {
  size_t sl = v->top + (size_t)v->row, el = sl;
  if (sel && sel->active) {
    size_t nsl, nsc, nel, nec;
    sel_norm(sel, &nsl, &nsc, &nel, &nec);
    sl = nsl;
    el = nel;
    if (nel < buf->count && nec == 0 && nel > nsl)
      el = nel - 1; // don't include empty last line
  }
  if (el >= buf->count)
    el = buf->count - 1;

  // Determine action: if ALL lines start with ';', uncomment; else comment
  bool all_commented = true;
  for (size_t li = sl; li <= el; li++) {
    const char *s = buf->lines[li];
    size_t i = 0;
    while (s[i] == ' ' || s[i] == '\t')
      i++;
    if (s[i] != ';') {
      all_commented = false;
      break;
    }
  }

  for (size_t li = sl; li <= el; li++) {
    char *s = buf->lines[li];
    size_t len = strlen(s);
    if (all_commented) {
      // Remove first ';'
      size_t i = 0;
      while (s[i] == ' ' || s[i] == '\t')
        i++;
      if (s[i] == ';') {
        memmove(s + i, s + i + 1, len - i);
        buf->modified = true;
      }
    } else {
      // Prepend ';'
      char *ns = (char *)malloc(len + 2);
      if (!ns)
        continue;
      ns[0] = ';';
      memcpy(ns + 1, s, len + 1);
      free(buf->lines[li]);
      buf->lines[li] = ns;
      buf->modified = true;
    }
  }
  if (sel)
    sel->active = false;
}

// ---------------------------------------------------------------------------
// Memory viewer  (hex editor style: cursor, arrow keys, hex/ASCII entry)
// ---------------------------------------------------------------------------
static void do_mem_viewer(const theme_t *theme, const buffer_t *buf,
                          const diag_state_t *ds) {
  (void)buf;

  int sh, sw;
  getmaxyx(stdscr, sh, sw);

  static unsigned char mem[65536];
  static bool mem_init = false;
  static uint16_t mem_last_addr = 0;
  static bool mem_last_valid = false;
  if (!mem_init) {
    memset(mem, 0, sizeof(mem));
    mem_init = true;
  }

  // Window layout:
  //  row 0          : box top border  (title embedded here)
  //  row 1          : column header "Addr  00 01 ..."
  //  row 2 .. r+1   : data rows
  //  row win_h-2    : status / help line
  //  row win_h-1    : box bottom border
  // So data rows = win_h - 4   (top_border + header + help + bottom_border)
  int win_h = sh - 2;
  if (win_h < 8)
    win_h = 8;
  // Width needed: 2 (margin) + 4(addr) + 2(gap) + 8*3 + 1(mid) + 8*3 + 2(gap) +
  // 16(ascii) + 2(margin) = 72
  int win_w = 76;
  if (win_w > sw)
    win_w = sw;
  int wy = (sh - win_h) / 2;
  if (wy < 0)
    wy = 0;
  int wx = (sw - win_w) / 2;
  if (wx < 0)
    wx = 0;

  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return;
  keypad(win, true);
  curs_set(1);

  const int COLS16 = 16;
  int data_rows = win_h - 4; // rows available for hex data
  if (data_rows < 1)
    data_rows = 1;

  // Initial cursor: last used address > asm start > 0
  uint32_t cursor = mem_last_valid
                        ? (uint32_t)mem_last_addr
                        : (ds->asm_valid ? (uint32_t)ds->asm_start : 0u);
  uint32_t view_top = (cursor / 16u) * 16u; // align to row
  int nibble = 0;          // 0 = high nibble, 1 = low nibble (hex entry)
  bool ascii_mode = false; // Tab toggles hex/ascii entry panel
  bool modified = false;

// Column offsets within the content area (col 2 = left margin inside border)
// "XXXX  " = 6, then pairs "XX " for each byte, with extra " " before byte 8
// hex_col(c)  = 6 + c*3 + (c >= 8 ? 1 : 0)
// ascii_col(c)= 6 + 8*3 + 1 + 8*3 + 2 + c  = 6+24+1+24+2+c = 57+c
#define HEX_COL(c) (6 + (c) * 3 + ((c) >= 8 ? 1 : 0))
#define ASCII_COL(c) (57 + (c))

// Ensure cursor is visible
#define ENSURE_VISIBLE()                                                       \
  do {                                                                         \
    uint32_t cur_row = cursor / (uint32_t)COLS16;                              \
    uint32_t top_row = view_top / (uint32_t)COLS16;                            \
    if (cur_row < top_row)                                                     \
      view_top = cur_row * (uint32_t)COLS16;                                   \
    else if ((int)(cur_row - top_row) >= data_rows)                            \
      view_top = (cur_row - (uint32_t)(data_rows - 1)) * (uint32_t)COLS16;     \
  } while (0)

  while (1) {
    ENSURE_VISIBLE();

    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);
    mvwprintw(win, 0, 2, " Memory Viewer%s ", modified ? " [modified]" : "");

    // Build header row to match exact data column positions.
    // Data layout (0-based within the line string, excluding the 2-col window
    // margin):
    //   [0..5]   "XXXX  "  (addr, 6 chars)
    //   HEX_COL(c) = 6 + c*3 + (c>=8?1:0)  — "XX " per byte, extra space before
    //   byte 8 ASCII_COL(c) = 6 + 8*3 + 1 + 8*3 + 2 + c = 57 + c
    // Total line width for 16 bytes: 6 + 24 + 1 + 24 + 2 + 16 = 73 chars
    {
      char hdr[96];
      int hw = 0;
      // "Addr  " label
      hw += snprintf(hdr + hw, sizeof(hdr) - (size_t)hw, "Addr  ");
      // Data section header: fill from HEX_COL(0) to just before ASCII_COL(0)
      // HEX_COL(0)=6 (already at hw=6), ASCII_COL(0)=57, so data header = 51
      // chars Format: "--- Data ---" centred in 51 chars with dashes padding
      {
        const char *lbl = " Data ";
        int total = 51; // 57 - 6
        int lblw = (int)strlen(lbl);
        int ldash = (total - lblw) / 2;
        int rdash = total - lblw - ldash;
        for (int d = 0; d < ldash; d++)
          hdr[hw++] = '-';
        memcpy(hdr + hw, lbl, (size_t)lblw);
        hw += lblw;
        for (int d = 0; d < rdash; d++)
          hdr[hw++] = '-';
      }
      // ASCII section header: 16 chars wide, "--- ASCII ---" centred
      {
        const char *lbl = " ASCII ";
        int total = 16;
        int lblw = (int)strlen(lbl);
        int ldash = (total - lblw) / 2;
        int rdash = total - lblw - ldash;
        for (int d = 0; d < ldash; d++)
          hdr[hw++] = '-';
        memcpy(hdr + hw, lbl, (size_t)lblw);
        hw += lblw;
        for (int d = 0; d < rdash; d++)
          hdr[hw++] = '-';
      }
      hdr[hw] = 0;
      mvwaddnstr(win, 1, 2, hdr, win_w - 4);
    }

    for (int r = 0; r < data_rows; r++) {
      uint32_t row_addr = view_top + (uint32_t)(r * COLS16);
      if (row_addr > 0xFFFF)
        break;
      int screen_row = 2 + r;

      // Address
      mvwprintw(win, screen_row, 2, "%04X  ", (unsigned)row_addr);

      // Hex bytes
      for (int c = 0; c < COLS16; c++) {
        uint32_t ba = row_addr + (uint32_t)c;
        if (ba > 0xFFFF)
          break;
        int hx = 2 + HEX_COL(c);
        bool is_cur = (ba == cursor);

        if (is_cur && !ascii_mode) {
          wattron(win, A_REVERSE);
          mvwprintw(win, screen_row, hx, "%02X", mem[ba]);
          wattroff(win, A_REVERSE);
          mvwprintw(win, screen_row, hx + 2, " ");
        } else {
          mvwprintw(win, screen_row, hx, "%02X ", mem[ba]);
        }
      }

      // ASCII panel — gap is already included in ASCII_COL(0) = 57, which
      // starts 2 cols after the last hex byte's space (last byte ends at 55).
      for (int c = 0; c < COLS16; c++) {
        uint32_t ba = row_addr + (uint32_t)c;
        if (ba > 0xFFFF)
          break;
        int ax = 2 + ASCII_COL(c);
        unsigned char b = mem[ba];
        char ch2 = (b >= 32 && b < 127) ? (char)b : '.';
        bool is_cur = (ba == cursor);

        if (is_cur && ascii_mode) {
          wattron(win, A_REVERSE);
          mvwaddch(win, screen_row, ax, (unsigned)ch2);
          wattroff(win, A_REVERSE);
        } else {
          mvwaddch(win, screen_row, ax, (unsigned)ch2);
        }
      }
    }

    // Status / help line
    char mode_str[128];
    snprintf(mode_str, sizeof(mode_str),
             "Addr:%04X  %s | Arrows=move  Tab=hex/ASCII  F10=goto  ESC=close",
             (unsigned)cursor, ascii_mode ? "ASCII" : "HEX  ");
    mvwaddnstr(win, win_h - 2, 2, mode_str, win_w - 4);

    // Position the real terminal cursor on the active cell
    {
      uint32_t cur_row = cursor / (uint32_t)COLS16;
      uint32_t top_row = view_top / (uint32_t)COLS16;
      int cur_col = (int)(cursor % (uint32_t)COLS16);
      int scr_row = 2 + (int)(cur_row - top_row);
      int scr_col;
      if (ascii_mode) {
        scr_col = 2 + ASCII_COL(cur_col);
      } else {
        scr_col = 2 + HEX_COL(cur_col) + nibble; // nibble 0=high, 1=low digit
      }
      wmove(win, scr_row, scr_col);
    }

    wrefresh(win);

    int ch = dialog_getch(win);

    if (ch == 27 || ch == KEY_EXIT)
      break;

    // Navigation
    if (ch == KEY_LEFT) {
      if (cursor > 0) {
        cursor--;
        nibble = 0;
      }
      continue;
    }
    if (ch == KEY_RIGHT) {
      if (cursor < 0xFFFF) {
        cursor++;
        nibble = 0;
      }
      continue;
    }
    if (ch == KEY_UP) {
      if (cursor >= (uint32_t)COLS16) {
        cursor -= (uint32_t)COLS16;
        nibble = 0;
      }
      continue;
    }
    if (ch == KEY_DOWN) {
      if (cursor + (uint32_t)COLS16 <= 0xFFFF) {
        cursor += (uint32_t)COLS16;
        nibble = 0;
      }
      continue;
    }
    if (ch == KEY_PPAGE) {
      uint32_t step = (uint32_t)(data_rows * COLS16);
      cursor = (cursor >= step) ? cursor - step : cursor % (uint32_t)COLS16;
      nibble = 0;
      continue;
    }
    if (ch == KEY_NPAGE) {
      uint32_t step = (uint32_t)(data_rows * COLS16);
      cursor =
          (cursor + step <= 0xFFFF)
              ? cursor + step
              : (0xFFFF - (uint32_t)(COLS16 - 1) + cursor % (uint32_t)COLS16);
      if (cursor > 0xFFFF)
        cursor = 0xFFFF;
      nibble = 0;
      continue;
    }
    if (ch == KEY_HOME) {
      cursor = cursor - cursor % (uint32_t)COLS16;
      nibble = 0;
      continue;
    }
    if (ch == KEY_END) {
      cursor = cursor - cursor % (uint32_t)COLS16 + (uint32_t)(COLS16 - 1);
      nibble = 0;
      continue;
    }

    // Tab: toggle hex/ASCII entry mode
    if (ch == '\t') {
      ascii_mode = !ascii_mode;
      nibble = 0;
      continue;
    }

    // Goto address
    if (ch == KEY_F(10)) {
      char tmp[8];
      tmp[0] = 0;
      // Use a mini prompt drawn in the status row
      wmove(win, win_h - 2, 2);
      wclrtoeol(win);
      mvwprintw(win, win_h - 2, 2, "Goto (hex): ");
      wrefresh(win);
      curs_set(1);
      // Read up to 4 hex digits
      int tlen = 0;
      while (tlen < 4) {
        int tc = dialog_getch(win);
        if (tc == 27 || tc == KEY_EXIT) {
          tlen = -1;
          break;
        }
        if (tc == '\n' || tc == '\r')
          break;
        if (tc == KEY_BACKSPACE || tc == 127 || tc == 8) {
          if (tlen > 0) {
            tlen--;
            tmp[tlen] = 0;
            mvwprintw(win, win_h - 2, 14 + tlen, " ");
            wmove(win, win_h - 2, 14 + tlen);
            wrefresh(win);
          }
          continue;
        }
        if ((tc >= '0' && tc <= '9') || (tc >= 'a' && tc <= 'f') ||
            (tc >= 'A' && tc <= 'F')) {
          tmp[tlen++] = (char)tc;
          tmp[tlen] = 0;
          mvwaddch(win, win_h - 2, 14 + tlen - 1, (unsigned)tc);
          wrefresh(win);
        }
      }
      if (tlen > 0) {
        unsigned long a = strtoul(tmp, NULL, 16);
        if (a <= 0xFFFF) {
          cursor = (uint32_t)(a & 0xFFF0);
          nibble = 0;
        }
      }
      continue;
    }

    // Editing: hex mode — accept 0-9, a-f, A-F
    if (!ascii_mode) {
      int nib = -1;
      if (ch >= '0' && ch <= '9')
        nib = ch - '0';
      else if (ch >= 'a' && ch <= 'f')
        nib = ch - 'a' + 10;
      else if (ch >= 'A' && ch <= 'F')
        nib = ch - 'A' + 10;
      if (nib >= 0) {
        if (nibble == 0) {
          mem[cursor] = (unsigned char)((mem[cursor] & 0x0F) | (nib << 4));
          nibble = 1;
        } else {
          mem[cursor] = (unsigned char)((mem[cursor] & 0xF0) | nib);
          nibble = 0;
          if (cursor < 0xFFFF)
            cursor++;
        }
        modified = true;
      }
      continue;
    }

    // Editing: ASCII mode — accept printable chars
    if (ascii_mode) {
      if (ch >= 32 && ch <= 126) {
        mem[cursor] = (unsigned char)ch;
        if (cursor < 0xFFFF)
          cursor++;
        modified = true;
      }
      continue;
    }
  }

  mem_last_addr = (uint16_t)(cursor & 0xFFFF);
  mem_last_valid = true;

  curs_set(1); // restore
  if (win)
    delwin(win);

#undef HEX_COL
#undef ASCII_COL
#undef ENSURE_VISIBLE
}

// ---------------------------------------------------------------------------
// Includes list
// ---------------------------------------------------------------------------

/* Forward declarations for functions used before their definition */
static int tui_word_at_cursor(const char *line, size_t col, char *out,
                              size_t cap);
static int tui_find_definition(const buffer_t *buf, const char *sym,
                               size_t *out_line0);

// ── Find all references to a symbol in the buffer ────────────────────────────
// Returns count; fills refs_lines[]/refs_cols[] up to max_refs entries.
static int tui_collect_references(const buffer_t *buf, const char *sym,
                                  size_t *refs_lines, int *refs_cols,
                                  int max_refs) {
  if (!buf || !sym || !*sym || !refs_lines || !refs_cols || max_refs <= 0)
    return 0;
  size_t slen = strlen(sym);
  if (slen == 0 || slen > 64)
    return 0;

  char usym[128];
  for (size_t i = 0; i < slen; i++)
    usym[i] = (char)toupper((unsigned char)sym[i]);
  usym[slen] = 0;
  /* strip leading dot and trailing colon */
  const char *us = usym;
  size_t un = slen;
  if (un && us[0] == '.') {
    us++;
    un--;
  }
  if (un && us[un - 1] == ':')
    un--;
  if (!un)
    return 0;

  int count = 0;
  for (size_t li = 0; li < buf->count && count < max_refs; li++) {
    const char *line = buffer_get_line(buf, li);
    size_t len = strlen(line);
    /* Scan every position in the line for the symbol as a whole token */
    for (size_t ci = 0; ci + un <= len && count < max_refs; ci++) {
      /* Check char before is not ident */
      if (ci > 0 && (isalnum((unsigned char)line[ci - 1]) ||
                     line[ci - 1] == '_' || line[ci - 1] == '.'))
        continue;
      /* Check candidate matches (case-insensitive) */
      bool match = true;
      for (size_t k = 0; k < un && match; k++)
        match = (toupper((unsigned char)line[ci + k]) == (unsigned char)us[k]);
      if (!match)
        continue;
      /* Check char after is not ident */
      size_t after = ci + un;
      if (after < len &&
          (isalnum((unsigned char)line[after]) || line[after] == '_'))
        continue;
      /* Skip if inside a comment */
      bool in_comment = false;
      bool in_str = false;
      for (size_t k = 0; k < ci && !in_comment; k++) {
        if (!in_str && line[k] == '"') {
          in_str = true;
          continue;
        }
        if (in_str && line[k] == '"') {
          in_str = false;
          continue;
        }
        if (!in_str && line[k] == ';') {
          in_comment = true;
        }
      }
      if (in_comment || in_str)
        continue;
      refs_lines[count] = li;
      refs_cols[count] = (int)ci;
      count++;
    }
  }
  return count;
}

#define MAX_REFS 256

/* Show all references to the symbol under the cursor in a list dialog.
 * Allows jumping to any reference line. */
static void do_find_refs(const theme_t *theme, buffer_t *buf, view_t *v,
                         diag_state_t *ds) {
  size_t cur_line = v->top + (size_t)v->row;
  const char *cur_text =
      (cur_line < buf->count) ? buffer_get_line(buf, cur_line) : "";
  char sym[128] = {0};
  int col_pos = (int)v->left + v->col;
  if (col_pos < 0)
    col_pos = 0;
  if (tui_word_at_cursor(cur_text, (size_t)col_pos, sym, sizeof(sym)) == 0) {
    status_set(ds, "no symbol under cursor");
    return;
  }

  static size_t refs_lines[MAX_REFS];
  static int refs_cols[MAX_REFS];
  int n = tui_collect_references(buf, sym, refs_lines, refs_cols, MAX_REFS);
  if (n == 0) {
    status_set(ds, "no references found: %s", sym);
    return;
  }

  /* Build display strings: "line:col  <snippet>" */
  char items[MAX_REFS][128];
  for (int i = 0; i < n; i++) {
    const char *line = buffer_get_line(buf, refs_lines[i]);
    /* trim leading whitespace for snippet */
    while (*line == ' ' || *line == '\t')
      line++;
    snprintf(items[i], sizeof(items[i]), "%4zu:%-3d  %.*s", refs_lines[i] + 1,
             refs_cols[i] + 1, 60, line);
  }
  /* Build pointer array for menu_select_list */
  char title[128];
  snprintf(title, sizeof(title), " References: %s (%d) ", sym, n);

  /* Use menu_select_list to show results */
  char list_items[MAX_REFS][1024];
  for (int i = 0; i < n; i++)
    snprintf(list_items[i], 1024, "%s", items[i]);

  int sel = menu_select_list(theme, title, list_items, n);
  if (sel >= 0 && sel < n) {
    /* Jump to selected reference */
    int h, w;
    getmaxyx(stdscr, h, w);
    (void)w;
    int th = h - 1 - TOPBAR_H;
    if (th < 1)
      th = 1;
    int half = th / 2;
    size_t tl = refs_lines[sel];
    if (tl < (size_t)half) {
      v->top = 0;
      v->row = (int)tl;
    } else {
      v->top = tl - (size_t)half;
      v->row = half;
    }
    v->col = refs_cols[sel];
    v->left = 0;
    status_set(ds, "jumped to reference %d/%d", sel + 1, n);
  }
}

/* Scan the buffer for .include directives, show them in a list dialog,
 * and jump to the selected include line on Enter. */
static void do_includes_list(const theme_t *theme, buffer_t *buf, view_t *v,
                             const char *path) {
// Scan buffer for .include directives, show a list, jump to line on Enter
#define MAX_INCLUDES 64
  char inc_paths[MAX_INCLUDES][256];
  size_t inc_lines[MAX_INCLUDES];
  int n = 0;

  for (size_t li = 0; li < buf->count && n < MAX_INCLUDES; li++) {
    const char *s = buf->lines[li];
    // skip whitespace
    while (*s == ' ' || *s == '\t')
      s++;
    // match .include (case-insensitive)
    if (strncasecmp(s, ".include", 8) == 0) {
      const char *rest = s + 8;
      while (*rest == ' ' || *rest == '\t')
        rest++;
      // extract quoted filename
      char fname[256];
      fname[0] = 0;
      if (*rest == '"') {
        rest++;
        size_t fi = 0;
        while (*rest && *rest != '"' && fi < sizeof(fname) - 1)
          fname[fi++] = *rest++;
        fname[fi] = 0;
      } else {
        size_t fi = 0;
        while (*rest && *rest != ' ' && *rest != '\t' && *rest != ';' &&
               fi < sizeof(fname) - 1)
          fname[fi++] = *rest++;
        fname[fi] = 0;
      }
      if (fname[0]) {
        snprintf(inc_paths[n], sizeof(inc_paths[n]), "%4zu: %s", li + 1, fname);
        inc_lines[n] = li;
        n++;
      }
    }
  }

  if (n == 0) {
    dialog_ok(theme, "Includes", "No .include directives found.");
    return;
  }

  char items[MAX_INCLUDES][1024];
  for (int i = 0; i < n; i++)
    strncpy(items[i], inc_paths[i], 1023);
  int sel =
      menu_select_list(theme, " Includes (Enter=jump, ESC=close) ", items, n);
  if (sel >= 0) {
    // jump to the include line
    size_t target = inc_lines[sel];
    int h, w;
    getmaxyx(stdscr, h, w);
    (void)w;
    int text_h = h - 1 - TOPBAR_H;
    size_t center = (size_t)(text_h / 2);
    v->top = (target > center) ? (target - center) : 0;
    size_t max_top =
        (buf->count > (size_t)text_h) ? (buf->count - (size_t)text_h) : 0;
    if (v->top > max_top)
      v->top = max_top;
    v->row = (int)(target - v->top);
    v->col = 0;
  }
  (void)path;
}

// ---------------------------------------------------------------------------
// Prefs dialogs
// ---------------------------------------------------------------------------
static void do_tab_width_dialog(const theme_t *theme) {
  char tmp[8];
  tmp[0] = 0;
  char def[8];
  snprintf(def, sizeof(def), "%d", g_prefs.tab_width);
  if (prompt_string(theme, "Tab width (1-16)", tmp, sizeof(tmp), def)) {
    char *_e;
    long _l = strtol(tmp, &_e, 10);
    int v = (_e != tmp) ? (int)_l : -1;
    if (v >= 1 && v <= 16) {
      g_prefs.tab_width = v;
      prefs_save();
    }
  }
}

static void do_comment_col_dialog(const theme_t *theme) {
  char tmp[8];
  tmp[0] = 0;
  char def[8];
  snprintf(def, sizeof(def), "%d", g_prefs.comment_col);
  if (prompt_string(theme, "Comment column (0=off, 1-200)", tmp, sizeof(tmp),
                    def)) {
    char *_e;
    long _l = strtol(tmp, &_e, 10);
    int v = (_e != tmp) ? (int)_l : -1;
    if (v >= 0 && v <= 200) {
      g_prefs.comment_col = v;
      prefs_save();
    }
  }
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Jump to definition — find where a symbol is defined in the buffer
// Returns 1 and sets *out_line0 (0-based) on success, 0 if not found.
// Recognises: label: / .label / NAME EQU / NAME = / .def NAME /
//             .define NAME / .proc NAME / .macro NAME / macro NAME
// ---------------------------------------------------------------------------
static int tui_find_definition(const buffer_t *buf, const char *sym,
                               size_t *out_line0) {
  if (!buf || !sym || !*sym)
    return 0;
  size_t slen = strlen(sym);

  // Build uppercase version of sym for comparison
  char usym[128];
  if (slen >= sizeof(usym))
    return 0;
  for (size_t i = 0; i < slen; i++)
    usym[i] = (char)toupper((unsigned char)sym[i]);
  usym[slen] = 0;
  // Strip leading '.' and trailing ':'
  const char *us = usym;
  size_t un = slen;
  if (un && us[0] == '.') {
    us++;
    un--;
  }
  if (un && us[un - 1] == ':')
    un--;
  if (!un)
    return 0;

  for (size_t li = 0; li < buf->count; li++) {
    const char *line = buffer_get_line(buf, li);
    size_t len = strlen(line);
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t'))
      i++;
    if (i >= len || line[i] == ';')
      continue;

    // Extract first token
    size_t t0 = i, t1 = i;
    if (line[i] == '.') {
      t1 = i + 1;
      while (t1 < len && (isalnum((unsigned char)line[t1]) || line[t1] == '_'))
        t1++;
    } else if (isalpha((unsigned char)line[i]) || line[i] == '_') {
      t1 = i + 1;
      while (t1 < len && (isalnum((unsigned char)line[t1]) || line[t1] == '_'))
        t1++;
    } else {
      continue;
    }
    size_t toklen = t1 - t0;
    // Uppercase the token
    char utok[64];
    if (toklen >= sizeof(utok))
      continue;
    for (size_t k = 0; k < toklen; k++)
      utok[k] = (char)toupper((unsigned char)line[t0 + k]);
    utok[toklen] = 0;
    // Strip leading dot for comparison
    const char *utoks = utok;
    size_t utokn = toklen;
    if (utokn && utoks[0] == '.') {
      utoks++;
      utokn--;
    }

    // Skip after first token to get rest of line
    size_t k = t1;
    while (k < len && (line[k] == ' ' || line[k] == '\t'))
      k++;

    // --- label: ---
    if (k < len && line[k] == ':') {
      // token is the label name (strip trailing : for match)
      char uname[64];
      size_t nn = toklen;
      if (nn >= sizeof(uname))
        continue;
      for (size_t x = 0; x < nn; x++)
        uname[x] = (char)toupper((unsigned char)line[t0 + x]);
      uname[nn] = 0;
      if (nn == un && !strncmp(uname, us, un)) {
        *out_line0 = li;
        return 1;
      }
      continue;
    }

    // --- dot-label (.name without colon) ---
    if (line[t0] == '.') {
      if (utokn == un && !strncmp(utoks, us, un)) {
        // But only if it's not a known directive keyword
        int is_dir = (!strcmp(utok, ".ORG") || !strcmp(utok, ".BYTE") ||
                      !strcmp(utok, ".WORD") || !strcmp(utok, ".DEF") ||
                      !strcmp(utok, ".DEFINE") || !strcmp(utok, ".IF") ||
                      !strcmp(utok, ".IFDEF") || !strcmp(utok, ".IFNDEF") ||
                      !strcmp(utok, ".ELSE") || !strcmp(utok, ".ENDIF") ||
                      !strcmp(utok, ".INCLUDE") || !strcmp(utok, ".PROC") ||
                      !strcmp(utok, ".ENDPROC") || !strcmp(utok, ".MACRO") ||
                      !strcmp(utok, ".ENDMACRO") || !strcmp(utok, ".ENDM") ||
                      !strcmp(utok, ".ASSERT") || !strcmp(utok, ".WARNING") ||
                      !strcmp(utok, ".ERROR") || !strcmp(utok, ".DWORD"));
        if (!is_dir) {
          *out_line0 = li;
          return 1;
        }
      }
    }

    // --- NAME EQU / NAME = ---
    if (utokn == un && !strncmp(utoks, us, un)) {
      if (k < len && line[k] == '=') {
        *out_line0 = li;
        return 1;
      }
      // check EQU
      size_t t2 = k, t3 = k;
      while (t3 < len && (isalnum((unsigned char)line[t3]) || line[t3] == '_'))
        t3++;
      if (t3 > t2) {
        char uequ[8];
        size_t en = t3 - t2;
        if (en == 3) {
          uequ[0] = (char)toupper((unsigned char)line[t2]);
          uequ[1] = (char)toupper((unsigned char)line[t2 + 1]);
          uequ[2] = (char)toupper((unsigned char)line[t2 + 2]);
          uequ[3] = 0;
          if (!strcmp(uequ, "EQU")) {
            *out_line0 = li;
            return 1;
          }
        }
      }
    }

    // --- .def NAME / .define NAME / .proc NAME / .macro NAME ---
    if (!strcmp(utok, ".DEF") || !strcmp(utok, ".DEFINE") ||
        !strcmp(utok, ".PROC") || !strcmp(utok, ".MACRO")) {
      // next token is the name
      size_t n0 = k, n1 = k;
      while (n1 < len && (isalnum((unsigned char)line[n1]) || line[n1] == '_'))
        n1++;
      if (n1 > n0) {
        char uname[64];
        size_t nn = n1 - n0;
        if (nn >= sizeof(uname))
          continue;
        for (size_t x = 0; x < nn; x++)
          uname[x] = (char)toupper((unsigned char)line[n0 + x]);
        uname[nn] = 0;
        if (nn == un && !strncmp(uname, us, un)) {
          *out_line0 = li;
          return 1;
        }
      }
      continue;
    }

    // --- bare macro NAME / MACRO NAME ---
    if (!strcmp(utok, "MACRO")) {
      size_t n0 = k, n1 = k;
      while (n1 < len && (isalnum((unsigned char)line[n1]) || line[n1] == '_'))
        n1++;
      if (n1 > n0) {
        char uname[64];
        size_t nn = n1 - n0;
        if (nn >= sizeof(uname))
          continue;
        for (size_t x = 0; x < nn; x++)
          uname[x] = (char)toupper((unsigned char)line[n0 + x]);
        uname[nn] = 0;
        if (nn == un && !strncmp(uname, us, un)) {
          *out_line0 = li;
          return 1;
        }
      }
    }
  }
  return 0;
}

// Get the identifier token under the cursor (col-based, 0-based col)
static int tui_word_at_cursor(const char *line, size_t col, char *out,
                              size_t cap) {
  if (!line || !out || cap == 0)
    return 0;
  size_t len = strlen(line);
  if (col >= len)
    return 0;
  // Walk back to start of token
  size_t s = col;
  while (s > 0 && (isalnum((unsigned char)line[s - 1]) || line[s - 1] == '_' ||
                   line[s - 1] == '.'))
    s--;
  // Walk forward to end
  size_t e = col;
  while (e < len && (isalnum((unsigned char)line[e]) || line[e] == '_'))
    e++;
  if (e <= s)
    return 0;
  size_t n = e - s;
  if (n >= cap)
    n = cap - 1;
  memcpy(out, line + s, n);
  out[n] = 0;
  return (int)n;
}

// ---------------------------------------------------------------------------
// Config Files dialog
// Two outer tabs: Prefs | Colours
// Three inner tabs each: Project | Executable | User
// Each panel: description, filename, full path, Found/Not found status,
//             [ Open Dir ]  [ Create ] buttons
// ---------------------------------------------------------------------------

static void cfg_resolve_paths(const char *cur_file, const char *exe_path,
                              char paths[6][PATH_MAX]) {
  // Directories for the three locations
  char proj_dir[PATH_MAX] = {0};
  char exe_dir[PATH_MAX] = {0};
  char user_dir[PATH_MAX] = {0};

  if (cur_file && cur_file[0]) {
    // dirname without modifying original: copy then find last slash
    snprintf(proj_dir, sizeof(proj_dir), "%s", cur_file);
    char *sl = strrchr(proj_dir, '/');
    if (sl)
      *sl = 0;
    else
      snprintf(proj_dir, sizeof(proj_dir), ".");
  }

  if (exe_path && exe_path[0]) {
    snprintf(exe_dir, sizeof(exe_dir), "%s", exe_path);
    char *sl = strrchr(exe_dir, '/');
    if (sl)
      *sl = 0;
    else
      snprintf(exe_dir, sizeof(exe_dir), ".");
  } else {
    // fallback: find exe dir via platform helper
    tui_exe_dir(exe_dir, sizeof(exe_dir));
  }

  tui_config_dir(user_dir, sizeof(user_dir));

  const char *dirs[3] = {proj_dir, exe_dir, user_dir};

  for (int i = 0; i < 3; i++) {
    if (dirs[i][0])
      snprintf(paths[i], PATH_MAX, "%s/prefs.ini", dirs[i]);
    else
      paths[i][0] = 0;
    if (dirs[i][0])
      snprintf(paths[3 + i], PATH_MAX, "%s/6502caEd.colours", dirs[i]);
    else
      paths[3 + i][0] = 0;
  }
}

/* Create the config file at paths[idx] if it does not already exist.
 * Ensures the parent directory exists first. */
static void cfg_create_file(int idx, char paths[6][PATH_MAX]) {
  if (!paths[idx][0])
    return;
  // Ensure parent dir exists
  char dir[PATH_MAX];
  snprintf(dir, sizeof(dir), "%s", paths[idx]);
  char *sl = strrchr(dir, '/');
  if (sl) {
    *sl = 0;
    // mkdir -p the two levels (.config and .config/6502caEd)
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", dir);
    for (char *p = tmp + 1; *p; p++) {
      if (*p == '/' || *p == '\\') {
        char saved = *p; *p = 0;
        tui_mkdir(tmp);
        *p = saved;
      }
    }
    tui_mkdir(tmp);
  }
  FILE *f = fopen(paths[idx], "w");
  if (!f)
    return;
  if (idx < 3) {
    fprintf(f, "; 6502caEd preferences\n");
    fprintf(f, "; Uncomment and edit values as needed.\n;\n");
    fprintf(f, "; tab_width=4\n");
    fprintf(f, "; allow_undoc=0\n");
    fprintf(f, "; auto_indent=1\n");
    fprintf(f, "; error_underline=1\n");
    fprintf(f, "; keep_comment_aligned=0\n");
    fprintf(f, "; comment_col=0\n");
    fprintf(f, "; clear_mem_on_asm=1\n");
    fprintf(f, "; show_problems=1\n");
  } else {
    fprintf(f, "; 6502caEd colour configuration\n");
    fprintf(f, "; Format: key = #RRGGBB or colour name\n;\n");
    fprintf(f, "; instr   = bright_cyan\n");
    fprintf(f, "; dir     = bright_magenta\n");
    fprintf(f, "; data    = bright_cyan\n");
    fprintf(f, "; num     = bright_yellow\n");
    fprintf(f, "; label   = yellow\n");
    fprintf(f, "; comment = blue\n");
    fprintf(f, "; string  = green\n");
    fprintf(f, "; reg     = green\n");
    fprintf(f, "; error   = red\n");
  }
  fclose(f);
}

// Draw one panel's content into the window.
// Returns button focus: 0 = Open Dir, 1 = Create
static void cfg_draw_panel(WINDOW *win, const theme_t *theme, int idx,
                           char paths[6][PATH_MAX], int win_h, int win_w,
                           int btn_focus) {
  static const char *descriptions[6] = {
      "Project-level preferences. Overrides exe/user defaults\n"
      "for this project only.",
      "Installation-wide preference defaults.\n"
      "Applies to all projects unless overridden.",
      "Your personal preference defaults.\n"
      "Used when no project or exe-level file is found.\n"
      "This is where preferences are saved.",
      "Project-specific colour overrides.\n"
      "Place here to customise colours for this project only.",
      "Installation-wide colour defaults.\n"
      "Applies to all projects unless overridden.",
      "Your personal colour defaults.\n"
      "Used when no project or exe-level colour file is found.",
  };
  static const char *filenames[6] = {
      "prefs.ini",
      "prefs.ini",
      "prefs.ini",
      "6502caEd.colours (.colors)",
      "6502caEd.colours (.colors)",
      "6502caEd.colours (.colors)",
  };

  int row = 2; // start below border + outer tab line

  // Description
  wattron(win, theme_attr(theme, TK_DEFAULT, 0));
  // Split description at \n and print each line
  char desc[256];
  snprintf(desc, sizeof(desc), "%s", descriptions[idx]);
  char *line = desc;
  while (line && *line && row < win_h - 6) {
    char *nl = strchr(line, '\n');
    if (nl)
      *nl = 0;
    mvwprintw(win, row++, 2, "%.*s", win_w - 4, line);
    line = nl ? nl + 1 : NULL;
  }
  row++;

  // Filename
  wattron(win, theme_attr(theme, TK_DEFAULT, 0) | A_BOLD);
  mvwprintw(win, row, 2, "File: ");
  wattroff(win, A_BOLD);
  wattron(win, theme_attr(theme, TK_DEFAULT, 0));
  mvwprintw(win, row++, 8, "%.*s", win_w - 10, filenames[idx]);

  // Path
  wattron(win, theme_attr(theme, TK_DEFAULT, 0) | A_BOLD);
  mvwprintw(win, row, 2, "Path: ");
  wattroff(win, A_BOLD);
  if (paths[idx][0]) {
    wattron(win, theme_attr(theme, TK_DEFAULT, 0));
    // Truncate path from the left if too long
    const char *p = paths[idx];
    int avail = win_w - 10;
    int plen = (int)strlen(p);
    if (plen > avail && avail > 3) {
      mvwprintw(win, row++, 8, "...%s", p + plen - (avail - 3));
    } else {
      mvwprintw(win, row++, 8, "%.*s", avail, p);
    }
  } else {
    wattron(win, theme_attr(theme, TK_COMMENT, 0));
    mvwprintw(win, row++, 8, "(no file open - save first)");
    wattroff(win, theme_attr(theme, TK_COMMENT, 0));
  }
  row++;

  // Status
  if (paths[idx][0]) {
    struct stat st;
    bool exists = (stat(paths[idx], &st) == 0);
    if (exists) {
      wattron(win, theme_attr(theme, TK_STRING, 0) | A_BOLD);
      mvwprintw(win, row++, 2, "* File found");
      wattroff(win, theme_attr(theme, TK_STRING, 0) | A_BOLD);
    } else {
      wattron(win, theme_attr(theme, TK_DEFAULT, 0));
      mvwprintw(win, row++, 2, "x File not found");
      wattroff(win, theme_attr(theme, TK_DEFAULT, 0));
    }
  }

  // Buttons
  int btn_row = win_h - 3;
  const char *lbl_open = "[ Open Dir ]";
  const char *lbl_create = "[ Create ]";
  int col_open = 2;
  int col_create = col_open + (int)strlen(lbl_open) + 2;

  bool can_create = paths[idx][0] != 0;
  struct stat st2;
  if (can_create)
    can_create = (stat(paths[idx], &st2) != 0); // only if not exists

  // Open Dir button
  attr_t open_attr =
      (btn_focus == 0)
          ? (theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD)
          : theme_attr(theme, TK_DEFAULT, 0);
  wattron(win, open_attr);
  mvwprintw(win, btn_row, col_open, "%s", lbl_open);
  wattroff(win, open_attr);

  // Create button
  attr_t create_attr;
  if (!can_create) {
    create_attr = theme_attr(theme, TK_COMMENT, 0); // dimmed
  } else if (btn_focus == 1) {
    create_attr = theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD;
  } else {
    create_attr = theme_attr(theme, TK_DEFAULT, 0);
  }
  wattron(win, create_attr);
  mvwprintw(win, btn_row, col_create, "%s", lbl_create);
  wattroff(win, create_attr);
}

static void do_config_files(const theme_t *theme, const char *cur_file,
                            const char *exe_path) {
  char paths[6][PATH_MAX];
  cfg_resolve_paths(cur_file, exe_path, paths);

  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int win_h = (sh > 24) ? 22 : sh - 2;
  if (win_h < 12)
    win_h = 12;
  int win_w = (sw > 90) ? 80 : sw - 4;
  if (win_w < 40)
    win_w = 40;
  int wy = (sh - win_h) / 2;
  if (wy < 1)
    wy = 1;
  int wx = (sw - win_w) / 2;
  if (wx < 0)
    wx = 0;

  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return;
  keypad(win, TRUE);
  flushinp();
  curs_set(0);

  // State: outer_tab (0=Prefs, 1=Colours), inner_tab (0=Project,1=Exe,2=User)
  // btn_focus: 0=Open Dir, 1=Create
  int outer_tab = 0, inner_tab = 0, btn_focus = 0;

  static const char *outer_labels[2] = {"Preferences", "Colours"};
  static const char *inner_labels[3] = {"Project", "Executable", "User"};

  while (1) {
    // ---- Draw ----
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    wattron(win, theme_attr(theme, TK_DEFAULT, 0));
    box(win, 0, 0);

    // Title in top border
    {
      const char *t = " Config Files ";
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, (win_w - (int)strlen(t)) / 2, "%s", t);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }

    // Outer tabs (row 1)
    {
      int col = 2;
      for (int i = 0; i < 2; i++) {
        bool active = (i == outer_tab);
        attr_t at = active ? (theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_BOLD)
                           : theme_attr(theme, TK_FS_TAB, 0);
        wattron(win, at);
        mvwprintw(win, 1, col, " %s ", outer_labels[i]);
        wattroff(win, at);
        col += (int)strlen(outer_labels[i]) + 3;
      }
    }

    // Inner tabs (row 2, indented)
    {
      int col = 4;
      for (int i = 0; i < 3; i++) {
        bool active = (i == inner_tab);
        attr_t at = active
                        ? (theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE)
                        : theme_attr(theme, TK_COMMENT, 0);
        wattron(win, at);
        mvwprintw(win, 2, col, " %s ", inner_labels[i]);
        wattroff(win, at);
        col += (int)strlen(inner_labels[i]) + 3;
      }
    }

    // Horizontal separator below inner tabs
    wattron(win, theme_attr(theme, TK_DEFAULT, 0));
    mvwhline(win, 3, 1, ACS_HLINE, win_w - 2);

    // Panel content
    int panel_idx = outer_tab * 3 + inner_tab;
    cfg_draw_panel(win, theme, panel_idx, paths, win_h, win_w, btn_focus);

    // Close button bottom-right
    {
      const char *lbl = "[ Close ]";
      int br = win_h - 2;
      int bc = win_w - (int)strlen(lbl) - 2;
      wattron(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD);
      mvwprintw(win, br, bc, "%s", lbl);
      wattroff(win,
               theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD);
    }

    wrefresh(win);

    // ---- Input ----
    int ch = dialog_getch(win);

    if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        int ry = ev.y - wy, rx = ev.x - wx;
        bool clicked = (ev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED |
                                     BUTTON1_RELEASED)) != 0;
        if (!clicked)
          continue;

        // Close button
        {
          const char *lbl = "[ Close ]";
          int br = win_h - 2, bc = win_w - (int)strlen(lbl) - 2;
          if (ry == br && rx >= bc && rx < bc + (int)strlen(lbl))
            break;
        }
        // Outer tabs (row 1)
        if (ry == 1) {
          int col = 2;
          for (int i = 0; i < 2; i++) {
            int tw = (int)strlen(outer_labels[i]) + 2;
            if (rx >= col && rx < col + tw) {
              outer_tab = i;
              btn_focus = 0;
            }
            col += tw + 1;
          }
        }
        // Inner tabs (row 2)
        if (ry == 2) {
          int col = 4;
          for (int i = 0; i < 3; i++) {
            int tw = (int)strlen(inner_labels[i]) + 2;
            if (rx >= col && rx < col + tw) {
              inner_tab = i;
              btn_focus = 0;
            }
            col += tw + 1;
          }
        }
        // Buttons (row win_h-3)
        if (ry == win_h - 3) {
          const char *lbl_open = "[ Open Dir ]";
          const char *lbl_create = "[ Create ]";
          int col_open = 2, col_create = col_open + (int)strlen(lbl_open) + 2;
          int pidx = outer_tab * 3 + inner_tab;
          if (rx >= col_open && rx < col_open + (int)strlen(lbl_open)) {
            // Open Dir
            if (paths[pidx][0]) {
              char dir[PATH_MAX];
              snprintf(dir, sizeof(dir), "%s", paths[pidx]);
              char *sl = strrchr(dir, '/');
              if (sl)
                *sl = 0;
              open_dir_safe(dir);
            }
          }
          if (rx >= col_create && rx < col_create + (int)strlen(lbl_create)) {
            struct stat st;
            if (paths[pidx][0] && stat(paths[pidx], &st) != 0) {
              cfg_create_file(pidx, paths);
              cfg_resolve_paths(cur_file, exe_path, paths); // refresh
            }
          }
        }
      }
      continue;
    }

    // Keyboard
    int panel_idx2 = outer_tab * 3 + inner_tab;

    if (ch == 27 || ch == KEY_EXIT)
      break;

    // Left/Right: cycle outer tabs
    if (ch == KEY_LEFT) {
      outer_tab = (outer_tab + 1) % 2;
      btn_focus = 0;
      continue;
    }
    if (ch == KEY_RIGHT) {
      outer_tab = (outer_tab + 1) % 2;
      btn_focus = 0;
      continue;
    }

    // Up/Down: cycle inner tabs
    if (ch == KEY_UP) {
      inner_tab = (inner_tab + 2) % 3;
      btn_focus = 0;
      continue;
    }
    if (ch == KEY_DOWN) {
      inner_tab = (inner_tab + 1) % 3;
      btn_focus = 0;
      continue;
    }

    // Tab: cycle button focus (Open Dir → Create → back)
    if (ch == '\t') {
      btn_focus = (btn_focus + 1) % 2;
      continue;
    }

    // Enter/Space: activate focused button
    if (ch == '\n' || ch == '\r' || ch == ' ') {
      struct stat st;
      bool can_create =
          paths[panel_idx2][0] != 0 && stat(paths[panel_idx2], &st) != 0;
      if (btn_focus == 0) {
        // Open Dir
        if (paths[panel_idx2][0]) {
          char dir[PATH_MAX];
          snprintf(dir, sizeof(dir), "%s", paths[panel_idx2]);
          char *sl = strrchr(dir, '/');
          if (sl)
            *sl = 0;
          open_dir_safe(dir);
        }
      } else if (btn_focus == 1 && can_create) {
        cfg_create_file(panel_idx2, paths);
        cfg_resolve_paths(cur_file, exe_path, paths);
      }
      continue;
    }

    // 1/2 shortcut for outer tabs
    if (ch == '1') {
      outer_tab = 0;
      btn_focus = 0;
      continue;
    }
    if (ch == '2') {
      outer_tab = 1;
      btn_focus = 0;
      continue;
    }
    // p/e/u shortcut for inner tabs
    if (ch == 'p' || ch == 'P') {
      inner_tab = 0;
      btn_focus = 0;
      continue;
    }
    if (ch == 'e' || ch == 'E') {
      inner_tab = 1;
      btn_focus = 0;
      continue;
    }
    if (ch == 'u' || ch == 'U') {
      inner_tab = 2;
      btn_focus = 0;
      continue;
    }
  }

  delwin(win);
  touchwin(stdscr);
  refresh();
}

// Preferences dialog — toggle boolean prefs + numeric inputs in one place
// ---------------------------------------------------------------------------
static int do_preferences_dialog(const theme_t *theme) {
  int prefs_changed = 0;
  typedef struct {
    const char *label;
    int *val;
    int is_numeric;
  } pref_row_t;
  pref_row_t rows[] = {
      {"Auto-indent on Enter", &g_prefs.auto_indent, 0},
      {"Clear memory on assemble", &g_prefs.clear_mem_on_asm, 0},
      {"Underline semantic errors", &g_prefs.error_underline, 0},
      {"Keep comment column aligned", &g_prefs.keep_comment_aligned, 0},
      {"Show problems panel", &g_prefs.show_problems, 0},
      {"Allow undocumented opcodes", &g_prefs.allow_undoc, 0},
      {"Tab width...", &g_prefs.tab_width, 1},
      {"Comment column...", &g_prefs.comment_col, 2},
  };
  int n_rows = (int)(sizeof(rows) / sizeof(rows[0]));
  static const char *const btn_label = "[ Close ]";

  int max_label = 0;
  for (int i = 0; i < n_rows; i++) {
    int l = (int)strlen(rows[i].label);
    if (l > max_label)
      max_label = l;
  }
  // Ensure window is wide enough for the button too
  int min_w = (int)strlen(btn_label) + 4;
  int win_w = max_label + 16;
  if (win_w < min_w)
    win_w = min_w;

  // rows + title border + blank gap + button row + bottom border
  int win_h = n_rows + 4;
  int btn_row = win_h - 2;
  int btn_col = (win_w - (int)strlen(btn_label)) / 2;

  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int wy = (sh - win_h) / 2;
  if (wy < 1)
    wy = 1;
  int wx = (sw - win_w) / 2;
  if (wx < 0)
    wx = 0;

  WINDOW *win = newwin(win_h, win_w, wy, wx);
  if (!win)
    return 0;
  keypad(win, true);
  curs_set(0);

  int sel = 0;

  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    wattron(win, theme_attr(theme, TK_DEFAULT, 0));
    box(win, 0, 0);

    // Title
    {
      const char *t = " Preferences ";
      wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
      mvwprintw(win, 0, (win_w - (int)strlen(t)) / 2, "%s", t);
      wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    }

    // Pref rows
    for (int i = 0; i < n_rows; i++) {
      bool s = (i == sel);
      chtype row_attr = s ? (theme_attr(theme, TK_FS_SELECTED, 0) | A_REVERSE)
                       : theme_attr(theme, TK_DEFAULT, 0);

      wattron(win, row_attr);
      for (int x = 1; x < win_w - 1; x++)
        mvwaddch(win, 1 + i, x, ' ');
      mvwprintw(win, 1 + i, 2, "%-*s", max_label, rows[i].label);

      if (rows[i].is_numeric == 1) {
        chtype vattr = s ? row_attr : theme_attr(theme, TK_NUMBER, 0);
        wattroff(win, row_attr);
        wattron(win, vattr);
        mvwprintw(win, 1 + i, 2 + max_label + 1, " %2d ", *rows[i].val);
        wattroff(win, vattr);
        if (s)
          wattron(win, row_attr);
      } else if (rows[i].is_numeric == 2) {
        chtype vattr = s ? row_attr : theme_attr(theme, TK_NUMBER, 0);
        wattroff(win, row_attr);
        wattron(win, vattr);
        if (*rows[i].val == 0)
          mvwprintw(win, 1 + i, 2 + max_label + 1, " off");
        else
          mvwprintw(win, 1 + i, 2 + max_label + 1, " %3d", *rows[i].val);
        wattroff(win, vattr);
        if (s)
          wattron(win, row_attr);
      } else {
        chtype vattr = s ? row_attr
                      : (*rows[i].val ? theme_attr(theme, TK_REGISTER, 0)
                                      : theme_attr(theme, TK_COMMENT, 0));
        wattroff(win, row_attr);
        wattron(win, vattr);
        mvwprintw(win, 1 + i, 2 + max_label + 1, " [%s]",
                  *rows[i].val ? "ON " : "OFF");
        wattroff(win, vattr);
        if (s)
          wattron(win, row_attr);
      }
      wattroff(win, row_attr);
    }

    // Close button — same style as About's OK button
    wattron(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD);
    mvwprintw(win, btn_row, btn_col, "%s", btn_label);
    wattroff(win, theme_attr(theme, TK_FS_TAB_ACTIVE, 0) | A_REVERSE | A_BOLD);

    wrefresh(win);

    int ch = dialog_getch(win);

    if (ch == KEY_MOUSE) {
      MEVENT me;
      if (getmouse(&me) != OK)
        continue;
      bool clicked = (me.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED |
                                   BUTTON1_RELEASED)) != 0;
      if (!clicked)
        continue;
      int ry = me.y - wy, rx = me.x - wx;
      // Click on Close button
      if (ry == btn_row && rx >= btn_col &&
          rx < btn_col + (int)strlen(btn_label))
        break;
      // Click on a pref row
      if (ry >= 1 && ry <= n_rows) {
        int idx = ry - 1;
        if (idx == sel)
          goto do_activate;
        sel = idx;
      }
      continue;
    }

    if (ch == 27 || ch == KEY_EXIT)
      break;
    if (ch == KEY_UP) {
      if (sel > 0)
        sel--;
    } else if (ch == KEY_DOWN) {
      if (sel < n_rows - 1)
        sel++;
    } else if (ch == '\n' || ch == '\r' || ch == ' ') {
    do_activate:;
      pref_row_t *r = &rows[sel];
      if (r->is_numeric) {
        curs_set(1);
        char tmp[8], def[8];
        snprintf(def, sizeof(def), "%d", *r->val);
        const char *prompt = (r->is_numeric == 1)
                                 ? "Tab width (1-16)"
                                 : "Comment column (0=off, 1-200)";
        int lo = (r->is_numeric == 1) ? 1 : 0;
        int hi = (r->is_numeric == 1) ? 16 : 200;
        if (prompt_string(theme, prompt, tmp, sizeof(tmp), def)) {
          char *_e;
          long _l = strtol(tmp, &_e, 10);
          int v = (_e != tmp) ? (int)_l : lo - 1;
          if (v >= lo && v <= hi)
            *r->val = v;
        }
        curs_set(0);
      } else {
        *r->val ^= 1;
      }
      prefs_save();
      prefs_changed = 1;
    }
  }

  curs_set(1);
  delwin(win);
  return prefs_changed;
}

// Apply a complete colour palette to the theme struct and reinit ncurses pairs.
// In dark mode we only reinit the colour pairs (preserving any file-loaded
// theme). In light mode we apply a full light palette overriding all colours.
static void apply_palette(theme_t *theme, int dark) {
  if (!dark) {
    // ---- Light palette — dark text on white/light background ----
    for (int i = 0; i < (int)TK__COUNT; i++) {
      theme->fg[i] = COLOR_BLACK;
      theme->bg[i] = COLOR_WHITE;
      theme->bold[i] = 0;
    }
    theme->fg[TK_MNEMONIC] = COLOR_BLUE;
    theme->bold[TK_MNEMONIC] = 1;
    theme->fg[TK_REGISTER] = COLOR_GREEN;
    theme->bold[TK_REGISTER] = 1;
    theme->fg[TK_DIRECTIVE] = COLOR_MAGENTA;
    theme->fg[TK_DATA] = COLOR_MAGENTA;
    theme->bold[TK_DATA] = 1;
    theme->fg[TK_NUMBER] = COLOR_RED;
    theme->fg[TK_ADDR_HEX] = COLOR_RED;
    theme->fg[TK_ADDR_DEC] = COLOR_RED;
    theme->fg[TK_ADDR_BIN] = COLOR_RED;
    theme->fg[TK_IMM_HEX] = COLOR_RED;
    theme->fg[TK_IMM_DEC] = COLOR_RED;
    theme->fg[TK_IMM_BIN] = COLOR_RED;
    theme->fg[TK_IMM_CHAR] = COLOR_RED;
    theme->fg[TK_DV_HEX] = COLOR_RED;
    theme->fg[TK_DV_DEC] = COLOR_RED;
    theme->fg[TK_DV_BIN] = COLOR_RED;
    theme->fg[TK_DV_STR] = COLOR_GREEN;
    theme->bold[TK_DV_STR] = 1;
    theme->fg[TK_STRING] = COLOR_GREEN;
    theme->bold[TK_STRING] = 1;
    theme->fg[TK_COMMENT] = COLOR_CYAN;
    theme->bold[TK_COMMENT] = 1;
    theme->fg[TK_LABEL] = COLOR_BLUE;
    theme->fg[TK_OPERATOR] = COLOR_BLACK;
    theme->fg[TK_DIAG_ERROR] = COLOR_RED;
    theme->bold[TK_DIAG_ERROR] = 1;
    theme->fg[TK_DIAG_WARNING] = COLOR_MAGENTA;
  }
  // Dark mode: leave fg/bg as-is (set by theme_load_and_init / theme file).
  // Either way, reinitialise ncurses colour pairs from current fg/bg arrays.
  start_color();
#ifdef NCURSES_VERSION
  use_default_colors();
#endif
  short next_pair = 1;
  for (int i = 0; i < (int)TK__COUNT; i++) {
    short fg = (short)theme->fg[i];
    short bg = (short)theme->bg[i];
#ifdef _WIN32
    pdc_init_pair(next_pair, fg, bg);
#else
    init_pair(next_pair, fg, bg);
#endif
    theme->pair[i] = next_pair;
    theme->color_attr[i] = (uint64_t)COLOR_PAIR(next_pair);
    next_pair++;
  }
  clearok(stdscr, true);
}

// Toggle between dark (default) and light palette.
static void toggle_theme(theme_t *theme) {
  g_prefs.dark_mode ^= 1;
  prefs_save();
  if (g_prefs.dark_mode) {
    // Restore dark: reload defaults + file then reinit pairs
    theme_load_and_init(theme);
  } else {
    apply_palette(theme, 0);
  }
}

// ---------------------------------------------------------------------------
// Virtual action codes (synthetic ch values from menu)
// ---------------------------------------------------------------------------
#define ACT_NEW_FILE 0x1001
#define ACT_SAVE_AS 0x1002
#define ACT_RECENT 0x1003
#define ACT_REPLACE 0x1004
#define ACT_ABOUT 0x1005
#define ACT_HELP 0x1015
#define ACT_IREF 0x1016
#define ACT_DREF 0x1017
#define ACT_DUP_LINE 0x1006
#define ACT_REM_DUP_LINES 0x1007
#define ACT_FMT_LAYOUT 0x1008
#define ACT_UPPER_CASE 0x1009
#define ACT_LOWER_CASE 0x100A
#define ACT_UNI_REPLACE 0x100B
#define ACT_COMMENT_TOG 0x100C
#define ACT_TOGGLE_PROBS 0x100D
#define ACT_MEM_VIEWER 0x100E
#define ACT_INCLUDES 0x100F
#define ACT_TAB_WIDTH 0x1010
#define ACT_COMMENT_COL 0x1011
#define ACT_TOGGLE_THEME 0x1012
#define ACT_PREFS 0x1013
#define ACT_CONFIG_FILES 0x1014
#define ACT_UPPER_SKIP 0x1015
#define ACT_LOWER_SKIP 0x1016
#define ACT_UPPER_ALL 0x1017
#define ACT_LOWER_ALL 0x1018
#define ACT_UPPER_INSTRS 0x1019
#define ACT_LOWER_INSTRS 0x101A
#define ACT_FIND_REFS 0x101D
#define ACT_INSERT_CHAR 0x101C
#define ACT_BOOKMARK_TOGGLE 0x101E
#define ACT_BOOKMARK_NEXT 0x101F
#define ACT_BOOKMARK_PREV 0x1020
#define ACT_BOOKMARK_CLEAR 0x1021
#define ACT_AUTOCOMPLETE 0x1022

/* Context menu action table — must be after ACT_ defines */
static const int g_ctx_acts[] = {
    ACT_UPPER_SKIP,   ACT_LOWER_SKIP,   ACT_UPPER_ALL,       ACT_LOWER_ALL,
    ACT_UPPER_INSTRS, ACT_LOWER_INSTRS, ACT_BOOKMARK_TOGGLE,
};

// Map menu_idx + item_idx → action code
static int menu_item_action(int menu_idx, int item_idx) {
  if (menu_idx == 0) { // File
    // New Open Save SaveAs --- Build(F5) --- Recent --- Quit
    if (item_idx == 0)
      return ACT_NEW_FILE;
    if (item_idx == 1)
      return 15; // Ctrl+O
    if (item_idx == 2)
      return 19; // Ctrl+S
    if (item_idx == 3)
      return ACT_SAVE_AS;
    if (item_idx == 5)
      return KEY_F(5); // Check errors
    if (item_idx == 7)
      return ACT_RECENT;
    if (item_idx == 9)
      return 17; // Ctrl+Q
    if (item_idx == 11)
      return KEY_F(9); // Assemble
  }
  if (menu_idx == 1) { // Edit
    // Find Replace Goto --- DupLine RemDup --- FmtLayout UpperSkip LowerSkip
    // UpperAll LowerAll UpperInstrs LowerInstrs Unicode Comment
    if (item_idx == 0)
      return 26; /* Ctrl+Z undo */
    if (item_idx == 1)
      return 25; /* Ctrl+Y redo */
    if (item_idx == 2)
      return 0; /* sep */
    if (item_idx == 3)
      return 6; /* Ctrl+F find */
    if (item_idx == 4)
      return ACT_REPLACE;
    if (item_idx == 5)
      return 7; /* Ctrl+G goto */
    if (item_idx == 7)
      return ACT_DUP_LINE;
    if (item_idx == 8)
      return ACT_REM_DUP_LINES;
    if (item_idx == 10)
      return ACT_FMT_LAYOUT;
    if (item_idx == 11)
      return ACT_UPPER_SKIP;
    if (item_idx == 12)
      return ACT_LOWER_SKIP;
    if (item_idx == 13)
      return ACT_UPPER_ALL;
    if (item_idx == 14)
      return ACT_LOWER_ALL;
    if (item_idx == 15)
      return ACT_UPPER_INSTRS;
    if (item_idx == 16)
      return ACT_LOWER_INSTRS;
    if (item_idx == 17)
      return ACT_UNI_REPLACE;
    if (item_idx == 18)
      return ACT_COMMENT_TOG;
    if (item_idx == 19)
      return ACT_INSERT_CHAR;
    if (item_idx == 20)
      return 0; /* sep */
    if (item_idx == 21)
      return ACT_FIND_REFS;
    if (item_idx == 22)
      return 0; /* sep */
    if (item_idx == 23)
      return ACT_BOOKMARK_TOGGLE;
    if (item_idx == 24)
      return 0; /* sep */
    if (item_idx == 25)
      return ACT_AUTOCOMPLETE;
  }
  if (menu_idx == 2) { // View
    // ToggleProbs MemViewer Includes --- NextBkm PrevBkm ClearBkm ---
    // TabWidth CommentCol ToggleTheme --- Prefs --- ConfigFiles
    if (item_idx == 0)
      return ACT_TOGGLE_PROBS;
    if (item_idx == 1)
      return ACT_MEM_VIEWER;
    if (item_idx == 2)
      return ACT_INCLUDES;
    if (item_idx == 4)
      return ACT_BOOKMARK_NEXT;
    if (item_idx == 5)
      return ACT_BOOKMARK_PREV;
    if (item_idx == 6)
      return ACT_BOOKMARK_CLEAR;
    if (item_idx == 8)
      return ACT_TAB_WIDTH;
    if (item_idx == 9)
      return ACT_COMMENT_COL;
    if (item_idx == 10)
      return ACT_TOGGLE_THEME;
    if (item_idx == 12)
      return ACT_PREFS;
    if (item_idx == 14)
      return ACT_CONFIG_FILES;
  }
  if (menu_idx == 3) { // Help
    if (item_idx == 0)
      return ACT_HELP;
    if (item_idx == 2)
      return ACT_IREF;
    if (item_idx == 3)
      return ACT_DREF;
    if (item_idx == 5)
      return ACT_ABOUT;
  }
  return 0;
}

/* ── Insert character by ASCII/Unicode code ─────────────────────────────────
   Triggered by Ctrl+\  (ch==28).  Shows a small dialog, user types decimal
   code 0-255 (or hex with 0x prefix), Enter inserts, ESC cancels. */
static int dialog_insert_char(const theme_t *theme) {
  int sh, sw;
  getmaxyx(stdscr, sh, sw);
  int win_w = 44, win_h = 7;
  if (win_w > sw - 2)
    win_w = sw - 2;
  int y = (sh - win_h) / 2, x = (sw - win_w) / 2;
  if (y < 1)
    y = 1;
  if (x < 0)
    x = 0;
  WINDOW *win = newwin(win_h, win_w, y, x);
  if (!win)
    return -1;
  keypad(win, true);
  curs_set(1);

  char buf[16] = {0};
  int blen = 0;
  int result = -1;

  while (1) {
    wbkgd(win, theme_attr(theme, TK_DEFAULT, 0));
    werase(win);
    box(win, 0, 0);
    wattron(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwprintw(win, 0, 2, " Insert Character ");
    wattroff(win, theme_attr(theme, TK_FS_TITLE, 0));
    mvwprintw(win, 2, 2, "Decimal (e.g. 65) or hex (e.g. 0x41):");
    /* input field */
    wattron(win, A_REVERSE);
    mvwprintw(win, 3, 2, "%-*s", win_w - 4, buf);
    wattroff(win, A_REVERSE);
    mvwprintw(win, 5, 2, "Enter=insert  ESC=cancel");
    /* preview */
    if (blen > 0) {
      {
        char *_pe = NULL;
        long code = -1;
        if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
          code = strtol(buf + 2, &_pe, 16);
        else
          code = strtol(buf, &_pe, 10);
        if (_pe && _pe != buf && code > 0 && code <= 255)
          mvwprintw(win, 3, win_w - 6, "[%c]", (int)code);
      }
    }
    wmove(win, 3, 2 + blen);
    wrefresh(win);

    int ch = dialog_getch(win);
    if (ch == 27 || ch == KEY_EXIT) {
      result = -1;
      break;
    }
    if (ch == '\n' || ch == '\r') {
      if (blen == 0) {
        result = -1;
        break;
      }
      {
        char *_ue = NULL;
        long code = -1;
        if (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'))
          code = strtol(buf + 2, &_ue, 16);
        else
          code = strtol(buf, &_ue, 10);
        if (_ue && _ue != buf && _ue != buf + 2 && code > 0 && code <= 65535)
          result = (int)code;
      }
      break;
    }
    if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && blen > 0) {
      buf[--blen] = 0;
      continue;
    }
    if (blen < (int)sizeof(buf) - 1) {
      bool hex_mode =
          (blen >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X'));
      bool accept =
          (hex_mode && ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
                        (ch >= 'A' && ch <= 'F'))) ||
          (!hex_mode && (ch >= '0' && ch <= '9')) ||
          (blen == 1 && buf[0] == '0' && (ch == 'x' || ch == 'X'));
      if (accept) {
        buf[blen++] = (char)ch;
        buf[blen] = 0;
      }
    }
  }
  curs_set(0);
  delwin(win);
  return result;
}

// ---------------------------------------------------------------------------
// tui_run — main editor loop
// ---------------------------------------------------------------------------
void tui_run(const char *editor_exe_path, char *io_path, size_t io_path_cap,
             const char *suggested_path, buffer_t *buf) {
  request_starting_term_size();
  prefs_load();

  initscr();
  start_color(); /* must be called before any colour operations */
#if defined(NCURSES_VERSION_MAJOR)
  set_escdelay(25);
#endif

#ifndef _WIN32
  signal(SIGWINCH, on_sigwinch);
  signal(SIGHUP,   on_sigquit);
  signal(SIGTSTP, SIG_IGN); /* ignore so Ctrl+Z reaches wgetch as ch=26 */
#endif
  signal(SIGTERM, on_sigquit);
  signal(SIGINT, SIG_IGN);

  raw();
  noecho();
#ifndef _WIN32
  /* Disable XON/XOFF flow control so Ctrl+Q (XON) and Ctrl+S (XOFF)
   * are delivered to the application rather than consumed by the terminal.
   * Must be done after raw() since ncurses may reset terminal settings. */
  {
    struct termios t;
    int fds[] = { STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO };
    for (int i = 0; i < 3; i++) {
      if (tcgetattr(fds[i], &t) == 0) {
        t.c_iflag &= ~(IXON | IXOFF | IXANY);
        tcsetattr(fds[i], TCSANOW, &t);
      }
    }
  }
#endif
  keypad(stdscr, true);
#ifndef _WIN32
  // Map Ctrl+Home / Ctrl+End escape sequences to unique key codes.
  // xterm-compatible terminals send these sequences.
  define_key("\033[1;5H", KEY_F(25));  // Ctrl+Home
  define_key("\033[1;5F", KEY_F(26));  // Ctrl+End
  define_key("\033[1;5D", KEY_F(28));  // Ctrl+Left
  define_key("\033[1;5C", KEY_F(29));  // Ctrl+Right
  define_key("\033[1;5A", KEY_F(30));  // Ctrl+Up
  define_key("\033[1;5B", KEY_F(31));  // Ctrl+Down
  define_key("\033[24;5~", KEY_F(32)); // Ctrl+F12
  define_key("\033[24;2~", KEY_F(33)); // Shift+F12
  define_key("\033[1;2S", KEY_F(16));  // Shift+F4 (xterm/VTE)
  /* Explicitly bind F10 escape sequences so the terminal doesn't intercept
     them before ncurses sees them. Different terminals send different
     sequences. */
  define_key("\033[21~", KEY_F(10));   // F10 in xterm/VTE (most common)
  define_key("\033[21;2~", KEY_F(10)); // F10 alternate
#endif
  timeout(g_input_timeout_ms);
  mousemask(ED_MOUSE_MASK, NULL);
  mouseinterval(0);
#ifndef _WIN32
  // Enable xterm any-event mouse tracking (Linux/macOS only).
  printf("\033[?1003h");
  fflush(stdout);
#endif

  theme_t theme;
  /* editor_exe_path used by Config Files dialog */
  theme_load_and_init(&theme);

  // Apply saved dark/light preference (always call so pairs are consistent)
  apply_palette(&theme, g_prefs.dark_mode);

  view_t v = {0};
  diag_state_t ds;
  diagdb_init(&ds.diags);
  ds.cur_diag_idx = (size_t)-1;
  ds.last_build_ok = true;
  ds.last_status[0] = 0;
  ds.bookmark_count = 0;
  bookmarks_load(&ds, io_path);

  menu_state_t ms;
  ms.active = false;
  ms.menu_idx = 0;
  ms.item_idx = 0;
  ms.opened_by_press = false;
  ctx_state_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.width = 32;
  ctx.nitems = N_CTX_ITEMS;

  symdb_t symdb;
  symdb_init(&symdb);
  jump_buf_t jbuf;
  memset(&jbuf, 0, sizeof(jbuf));
  jbuf.cur = -1;
  bool sem_dirty = true;
  /* 0-based line last modified by the user. Errors on this line are suppressed
     while the cursor stays on it; they appear as soon as the cursor moves off. */
  size_t last_edit_line = (size_t)-1;

  sel_t sel = {0};
  undo_t undo;
  undo_init(&undo);
  bool undo_typing_run = false; /* true while consecutive chars being typed */
  static char last_search[128];

  // Ctrl+K chord state
  bool ctrl_k_pending = false;

  int ch;
  int injected_ch = -1;

  while (1) {
    if (g_quit_request) {
      g_quit_request = 0;
      injected_ch = 17;
    }

    if (g_need_resize) {
      g_need_resize = 0;
      tui_handle_resize();
      ms.active = false;
      ctrl_k_pending = false;
      {
        int h, w;
        getmaxyx(stdscr, h, w);
        int text_h = h - 1 - TOPBAR_H;
        int w_text = (w > gutter_w(buf->count ? buf->count : 1))
                         ? (w - gutter_w(buf->count ? buf->count : 1))
                         : 0;
        clamp_cursor(buf, &v, text_h, w_text);
      }
      sem_dirty = true;
      continue;
    }

    if (sem_dirty) {
      symdb_rebuild(&symdb, buf);
      diagdb_clear(&ds.diags);
      add_duplicate_def_diags(&symdb, &ds.diags);

      /* Run a background assembler check so undefined symbols and other
         assembler errors appear live, not just after F5.
         Write the buffer to a temp file so we never touch the user's file. */
      if (io_path && io_path[0]) {
        char tmp_path[PATH_MAX];
        snprintf(tmp_path, sizeof(tmp_path), "%s.~sem~", io_path);
        if (buffer_save(buf, tmp_path)) {
          runner_result_t sem_rr = {0};
          runner_opts_t sem_opts = {.allow_undoc = (bool)g_prefs.allow_undoc};
          run_check_ex(tmp_path, &sem_rr, &sem_opts);
          size_t cur_line0 = v.top + (size_t)v.row;
          for (size_t _si = 0; _si < sem_rr.count; _si++) {
            const runner_diag_item_t *_sd = &sem_rr.items[_si];
            /* Only show errors attributed to the root (temp) file —
               errors in included files have wrong line numbers relative
               to the current buffer and would show on incorrect lines. */
            if (_sd->file && strcmp(_sd->file, tmp_path) != 0)
              continue;
            /* Suppress errors on the line currently being edited */
            if (_sd->line1 > 0 &&
                _sd->line1 - 1 == last_edit_line)
              continue;
            (void)cur_line0;
            diag_kind_t _sk = (_sd->kind == RUN_DIAG_ERROR)
                                  ? DIAG_ERROR : DIAG_WARNING;
            diagdb_add(&ds.diags,
                       _sd->line1 ? _sd->line1 : 1,
                       _sd->col1  ? _sd->col1  : 1,
                       _sk,
                       _sd->message ? _sd->message : "");
          }
          runner_result_free(&sem_rr);
          unlink(tmp_path);
        }
      }

      ds.cur_diag_idx = (size_t)-1;
      fold_map_rebuild(&ds.fold, (const char **)buf->lines, buf->count,
                       g_prefs.fold_comments != 0, g_prefs.fold_macros != 0);
      sem_dirty = false;
    }

    /* If the cursor has moved off the last edited line, clear the suppression
       so errors reappear on that line immediately. */
    {
      size_t cur_line0 = v.top + (size_t)v.row;
      if (last_edit_line != (size_t)-1 && cur_line0 != last_edit_line)
        last_edit_line = (size_t)-1;
    }

    draw(io_path, buf, &v, &ds, &theme, &symdb, &ms, &ctx, last_edit_line);

    if (injected_ch != -1) {
      ch = injected_ch;
      injected_ch = -1;
    } else {
      ch = getch();
      if (ch == ERR)
        continue;
#ifdef _WIN32
      /* PDCurses delivers terminal resize as KEY_RESIZE rather than SIGWINCH */
      if (ch == KEY_RESIZE) {
        g_need_resize = 1;
        continue;
      }
#endif
    }

    // Ctrl+R → Replace
    if (ch == 18 || (ch == 8 && erasechar() != 8))
      ch = ACT_REPLACE;

    // ---- Mouse ----
    if (ch == KEY_MOUSE) {
      MEVENT me;
      if (getmouse(&me) == OK) {
        bool is_press = (me.bstate & BUTTON1_PRESSED) != 0;
        bool is_release =
            (me.bstate & (BUTTON1_CLICKED | BUTTON1_RELEASED)) != 0;

        /* ---- Context menu mouse interaction ---- */
        if (ctx.active) {
          int ox = ctx.origin_x, oy = ctx.origin_y;
          int cw = ctx.width, cn = ctx.nitems;
          int h2, w2;
          getmaxyx(stdscr, h2, w2);
          if (ox + cw >= w2)
            ox = w2 - cw - 1;
          if (oy + cn + 2 >= h2)
            oy = h2 - cn - 2;
          if (ox < 0)
            ox = 0;
          if (oy < 1)
            oy = 1;
          bool inside = (me.x >= ox && me.x < ox + cw && me.y >= oy &&
                         me.y < oy + cn + 2);
          if (inside) {
            int item = me.y - (oy + 1);
            if (item >= 0 && item < cn)
              ctx.sel = item;
            /* Left-click selects item */
            if (is_release && item >= 0 && item < cn) {
              ctx.active = false;
              bool bm_disabled =
                  (item == N_CTX_ITEMS - 1) &&
                  (ds.bookmark_count >= MAX_BOOKMARKS) &&
                  (bookmark_find(&ds, v.top + (size_t)v.row) < 0);
              if (!bm_disabled)
                injected_ch = g_ctx_acts[item];
            }
          } else if (is_press || is_release) {
            /* Click outside → dismiss */
            ctx.active = false;
          }
          continue;
        }

        // ---- Hover: runs on every KEY_MOUSE event (move, press, or release).
        // When ms.active, always update the highlighted item from cursor
        // position. A PRESSED event while dragging is treated as hover, not a
        // close trigger.
        if (ms.active) {
          if (me.y == 0) {
            int hit = menubar_hit_test_x(me.x);
            if (hit >= 0 && hit != ms.menu_idx) {
              ms.menu_idx = hit;
              ms.item_idx = menu_first_item(hit);
            }
          } else {
            int mx = 0, mw = 0, nitems = 0;
            menu_window_geom(ms.menu_idx, &mx, &mw, &nitems);
            if (me.y >= 2 && me.y < 2 + nitems && me.x >= mx &&
                me.x < mx + mw) {
              int item = me.y - 2;
              int n;
              const char **items = menu_items(ms.menu_idx, &n);
              if (item >= 0 && item < n && strcmp(items[item], MENU_SEP) != 0)
                ms.item_idx = item;
            }
          }
          // Only act on release/click — not on press (which also fires during
          // drag). Exception: a pure move (no button) off the dropdown+menubar
          // closes it.
          if (!is_release) {
            if (!is_press && me.y > 0) {
              // Check if cursor is outside the dropdown
              int mx = 0, mw = 0, nitems = 0;
              menu_window_geom(ms.menu_idx, &mx, &mw, &nitems);
              int win_y = 1, win_h = nitems + 2;
              bool inside = (me.y >= win_y && me.y < win_y + win_h &&
                             me.x >= mx && me.x < mx + mw);
              if (!inside) {
                ms.active = false;
                continue;
              }
            }
            continue;
          }

          // Release inside dropdown → activate item
          {
            int mx = 0, mw = 0, nitems = 0;
            menu_window_geom(ms.menu_idx, &mx, &mw, &nitems);
            int win_y = 1, win_h = nitems + 2;
            if (me.y >= win_y && me.y < win_y + win_h && me.x >= mx &&
                me.x < mx + mw) {
              int item = me.y - (win_y + 1);
              int n;
              const char **items = menu_items(ms.menu_idx, &n);
              if (item >= 0 && item < n && strcmp(items[item], MENU_SEP) != 0) {
                ms.item_idx = item;
                int act = menu_item_action(ms.menu_idx, ms.item_idx);
                ms.active = false;
                // Use injected_ch so draw() runs first (clears the overlay)
                // before the action handler fires.
                if (act) {
                  injected_ch = act;
                }
                continue;
              } else
                continue;
            } else if (me.y == 0) {
              // Released on menu bar — toggle or switch menu
              int hit = menubar_hit_test_x(me.x);
              if (hit >= 0) {
                if (hit == ms.menu_idx && ms.opened_by_press) {
                  // This is the release of the press that opened us — swallow
                  // it. The menu stays open; a second click will close it.
                  ms.opened_by_press = false;
                } else if (hit == ms.menu_idx) {
                  ms.active = false; // second click on same title → close
                } else {
                  ms.menu_idx = hit;
                  ms.item_idx = menu_first_item(hit);
                  ms.opened_by_press = false;
                }
              } else {
                ms.active = false;
              }
              continue;
            } else {
              // Released outside dropdown and menu bar → close
              ms.active = false;
              continue;
            }
          }
        }

        // ---- Menu not active: open on press or click of menu bar ----
        if (is_press || is_release) {
          if (me.y == 0) {
            int hit = menubar_hit_test_x(me.x);
            if (hit >= 0) {
              ms.active = true;
              ms.menu_idx = hit;
              ms.item_idx = menu_first_item(hit);
              ms.opened_by_press = is_press;
              continue;
            }
          }
        }

        /* Fold icon gutter click: [+]/[-] in fold column */
        if ((g_prefs.fold_comments || g_prefs.fold_macros) &&
            (me.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED)) &&
            me.y >= TOPBAR_H && ds.fold.lines) {
          int gw2 = gutter_w(buf->count ? buf->count : 1);
          int fold_col2 = gw2 - 3;
          if ((int)me.x >= fold_col2 && (int)me.x <= fold_col2 + 2) {
            int vis_row = (int)me.y - TOPBAR_H;
            if (vis_row >= 0) {
              size_t buf_line =
                  fold_visual_to_buf(&ds.fold, (size_t)(v.top + vis_row));
              fold_toggle(&ds.fold, buf_line);
            }
            continue;
          }
        }

        /* Right-click: open context menu at click position */
        bool is_right = (me.bstate & (BUTTON3_PRESSED | BUTTON3_CLICKED |
                                      BUTTON3_RELEASED)) != 0;
        if (is_right && me.y >= TOPBAR_H) {
          ctx.active = true;
          ctx.sel = 0;
          ctx.origin_x = me.x;
          ctx.origin_y = me.y;
          /* Also move text cursor to clicked position */
          int gw2 = gutter_w(buf->count ? buf->count : 1);
          int text_col = me.x - gw2 + v.left;
          size_t text_row = (size_t)(me.y - TOPBAR_H) + v.top;
          if (text_row < buf->count) {
            v.row = (int)(me.y - TOPBAR_H);
            size_t llen = strlen(buf->lines[text_row]);
            v.col = (text_col >= 0 && (size_t)text_col <= llen) ? text_col
                                                                : (int)llen;
          }
          ms.active = false; /* close any open menubar dropdown */
          continue;
        }
      }
    }

    if (ch == KEY_RESIZE) {
      tui_handle_resize();
      ms.active = false;
      ctrl_k_pending = false;
      {
        int h, w;
        getmaxyx(stdscr, h, w);
        int text_h = h - 1 - TOPBAR_H;
        int w_text = (w > gutter_w(buf->count ? buf->count : 1))
                         ? (w - gutter_w(buf->count ? buf->count : 1))
                         : 0;
        clamp_cursor(buf, &v, text_h, w_text);
      }
      sem_dirty = true;
      continue;
    }

    // ---- Quit ----
    // if (ch == KEY_F(9)) ch = 17;
    if (ch == 17) {
      int a = confirm_discard_or_save(&theme, buf, io_path, io_path_cap,
                                      suggested_path);
      if (a == 0)
        continue;
      if (a == 2 &&
          !do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
        status_set(&ds, "save failed");
        continue;
      }
      bookmarks_save(&ds, io_path);
      break;
    }

    int h, w;
    getmaxyx(stdscr, h, w);
    int text_h = h - 1 - TOPBAR_H;
    int w_text = (w > gutter_w(buf->count ? buf->count : 1))
                     ? (w - gutter_w(buf->count ? buf->count : 1))
                     : 0;

    // ---- Insert character by code: F2 or Edit menu ----
    if (ch == KEY_F(2) || ch == ACT_INSERT_CHAR) {
      int code = dialog_insert_char(&theme);
      if (code > 0) {
        static const unsigned long w1252[32] = {
            0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
            0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178};
        unsigned long uc = (code >= 0x80 && code <= 0x9F) ? w1252[code - 0x80]
                                                          : (unsigned long)code;
        if (uc > 0) {
          char utf8[8];
          int nb = utf8_encode(uc, utf8);
          utf8[nb] = 0;
          size_t li = v.top + (size_t)v.row;
          for (int _i = 0; _i < nb; _i++) {
            insert_char(buf, li, v.col, (unsigned char)utf8[_i]);
            v.col++;
          }
          buf->modified = true;
          sem_dirty = true;
          status_set(&ds, "inserted char U+%04lX", uc);
        }
      } else {
        status_set(&ds, "insert char: cancelled");
      }
      continue;
    }

    if (!ms.active && ch == 27) {
      timeout(0);
      int n = getch();
      timeout(g_input_timeout_ms);
      if (n != ERR) {
        if (n == 'f' || n == 'F') {
          ms.active = true;
          ms.menu_idx = 0;
          ms.item_idx = menu_first_item(0);
          continue;
        }
        if (n == 'e' || n == 'E') {
          ms.active = true;
          ms.menu_idx = 1;
          ms.item_idx = menu_first_item(1);
          continue;
        }
        if (n == 'v' || n == 'V') {
          ms.active = true;
          ms.menu_idx = 2;
          ms.item_idx = menu_first_item(2);
          continue;
        }
        if (n == 'h' || n == 'H') {
          ms.active = true;
          ms.menu_idx = 3;
          ms.item_idx = menu_first_item(3);
          continue;
        }
        if (n == 'q' || n == 'Q') {
          ch = 17; /* fall through */
        } else
          continue; /* unknown Alt+key */
      } else {
        /* Pure ESC with no following char — nothing to do, fall through */
        continue;
      }
    }

    /* Keyboard navigation of context menu when active */
    if (ctx.active && ch != KEY_MOUSE) {
      if (ch == KEY_UP) {
        ctx.sel = (ctx.sel + ctx.nitems - 1) % ctx.nitems;
        continue;
      }
      if (ch == KEY_DOWN) {
        ctx.sel = (ctx.sel + 1) % ctx.nitems;
        continue;
      }
      if (ch == '\n' || ch == '\r' || ch == ' ') {
        ctx.active = false;
        /* Don't fire disabled bookmark item */
        bool bm_disabled = (ctx.sel == N_CTX_ITEMS - 1) &&
                           (ds.bookmark_count >= MAX_BOOKMARKS) &&
                           (bookmark_find(&ds, v.top + (size_t)v.row) < 0);
        if (!bm_disabled)
          injected_ch = g_ctx_acts[ctx.sel];
        continue;
      }
      /* Any other key: dismiss without action */
      ctx.active = false;
      /* Don't consume the key — let it fall through */
    }

    // ---- F10: open menu bar (F1=Help) ----
    if (!ms.active && ch == KEY_F(10)) {
      ms.active = true;
      ms.menu_idx = 0;
      ms.item_idx = menu_first_item(0);
      continue;
    }

    // ---- Menu navigation ----
    if (ms.active) {
      if (ch == 27) {
        ms.active = false;
        ctrl_k_pending = false;
        continue;
      }
      if (ch == KEY_LEFT) {
        ms.menu_idx = (ms.menu_idx + N_MENUS - 1) % N_MENUS;
        ms.item_idx = menu_first_item(ms.menu_idx);
        continue;
      }
      if (ch == KEY_RIGHT) {
        ms.menu_idx = (ms.menu_idx + 1) % N_MENUS;
        ms.item_idx = menu_first_item(ms.menu_idx);
        continue;
      }
      int n;
      menu_items(ms.menu_idx, &n);
      if (ch == KEY_UP) {
        ms.item_idx = menu_next_item(ms.menu_idx, ms.item_idx, -1, n);
        continue;
      }
      if (ch == KEY_DOWN) {
        ms.item_idx = menu_next_item(ms.menu_idx, ms.item_idx, +1, n);
        continue;
      }
      if (ch == '\n' || ch == '\r') {
        int act = menu_item_action(ms.menu_idx, ms.item_idx);
        ms.active = false;
        if (act) {
          injected_ch = act;
        }
        continue;
      } else {
        continue;
      }
    }

    // Second quit path (from menu dispatch)
    if (ch == 17) {
      int a = confirm_discard_or_save(&theme, buf, io_path, io_path_cap,
                                      suggested_path);
      if (a == 0)
        continue;
      if (a == 2 &&
          !do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
        status_set(&ds, "save failed");
        continue;
      }
      bookmarks_save(&ds, io_path);
      break;
    }

    // ---- Ctrl+K chord ----
    if (ch == 11) { // Ctrl+K
      ctrl_k_pending = true;
      status_set(&ds, "Ctrl+K: C=comment  U=uncomment");
      continue;
    }
    if (ctrl_k_pending) {
      ctrl_k_pending = false;
      ds.last_status[0] = 0;
      if (ch == 'c' || ch == 'C' || ch == 'u' || ch == 'U') {
        ch = ACT_COMMENT_TOG; // fall through to handler
      } else {
        continue;
      }
    }

    // ---- Check errors (F5) ----
    if (ch == KEY_F(5)) {
      if (!io_path || !io_path[0]) {
        if (dialog_yesno(&theme, "Check",
                         "File must be saved first. Save now?") != 1) {
          status_set(&ds, "check cancelled");
          continue;
        }
        if (!do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
          status_set(&ds, "save failed");
          continue;
        }
      } else if (buf->modified) {
        if (!do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
          status_set(&ds, "save failed");
          continue;
        }
      }
      runner_result_t rr = {0};
      runner_opts_t ropts = {.allow_undoc = (bool)g_prefs.allow_undoc};
      bool ok = run_check_ex(io_path, &rr, &ropts);
      diagdb_clear(&ds.diags);
      last_edit_line = (size_t)-1;  /* show all errors after explicit check */
      for (size_t i = 0; i < rr.count; i++) {
        const runner_diag_item_t *d = &rr.items[i];
        diag_kind_t k = (d->kind == RUN_DIAG_ERROR) ? DIAG_ERROR : DIAG_WARNING;
        diagdb_add(&ds.diags, d->line1 ? d->line1 : 1, d->col1 ? d->col1 : 1,
                   k, d->message ? d->message : "");
      }
      runner_result_free(&rr);
      symdb_rebuild(&symdb, buf);
      add_duplicate_def_diags(&symdb, &ds.diags);
      ds.cur_diag_idx = (size_t)-1;
      if (ok && !ds.diags.num_errors && !ds.diags.num_warnings)
        status_set(&ds, "Check OK — no errors");
      else if (ok && !ds.diags.num_errors)
        status_set(&ds, "Check OK — %zu warning%s", ds.diags.num_warnings,
                   ds.diags.num_warnings == 1 ? "" : "s");
      else
        status_set(&ds, "Check: %zu error%s, %zu warning%s",
                   ds.diags.num_errors,  ds.diags.num_errors  == 1 ? "" : "s",
                   ds.diags.num_warnings, ds.diags.num_warnings == 1 ? "" : "s");
      if (ds.diags.count) {
        ds.cur_diag_idx = 0;
        jump_to_diag(buf, &v, &ds.diags.items[0]);
      }
      continue;
    }

    // ---- Diag navigation: F8=next, F12/Shift+F8=prev ----
    if (ch == KEY_F(8)) {
      if (ds.diags.count) {
        ds.cur_diag_idx = diagdb_next_index(&ds.diags, ds.cur_diag_idx);
        if (ds.cur_diag_idx != (size_t)-1)
          jump_to_diag(buf, &v, &ds.diags.items[ds.cur_diag_idx]);
      }
      continue;
    }
    // ---- Assemble (F9) ----
    if (ch == KEY_F(9)) {
      if (!io_path || !io_path[0]) {
        if (dialog_yesno(&theme, "Assemble",
                         "File must be saved first. Save now?") != 1) {
          status_set(&ds, "assemble cancelled");
          continue;
        }
        if (!do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
          status_set(&ds, "save failed");
          continue;
        }
      } else if (buf->modified) {
        if (dialog_yesno(&theme, "Assemble",
                         "File has unsaved changes. Save before assembling?") != 1) {
          status_set(&ds, "assemble cancelled");
          continue;
        }
        if (!do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
          status_set(&ds, "save failed");
          continue;
        }
      }
      char outbin[1024];
      outbin[0] = 0;
      runner_result_t rr = {0};
      runner_opts_t ropts = {.allow_undoc = (bool)g_prefs.allow_undoc};
      bool ok = run_assembler_ex(io_path, outbin, sizeof(outbin), &rr, &ropts);
      diagdb_clear(&ds.diags);
      last_edit_line = (size_t)-1;  /* show all errors after explicit assemble */
      for (size_t i = 0; i < rr.count; i++) {
        const runner_diag_item_t *d = &rr.items[i];
        diag_kind_t k = (d->kind == RUN_DIAG_ERROR) ? DIAG_ERROR : DIAG_WARNING;
        diagdb_add(&ds.diags, d->line1 ? d->line1 : 1, d->col1 ? d->col1 : 1,
                   k, d->message ? d->message : "");
      }
      runner_result_free(&rr);
      symdb_rebuild(&symdb, buf);
      add_duplicate_def_diags(&symdb, &ds.diags);
      ds.last_build_ok = ok;
      ds.cur_diag_idx = (size_t)-1;
      if (ok && rr.asm_valid) {
        ds.asm_start = rr.asm_start;
        ds.asm_end   = rr.asm_end;
        ds.asm_bytes = rr.asm_bytes;
        ds.asm_valid = true;
        ds.asm_range_count = rr.range_count;
        if (rr.range_count > 0)
          memcpy(ds.asm_ranges, rr.ranges,
                 (size_t)rr.range_count * sizeof(ds.asm_ranges[0]));
      }
      if (ok) {
        if (!ds.diags.num_errors && !ds.diags.num_warnings)
          status_set(&ds, "Build OK — %u bytes in %d range%s -> %s",
                     ds.asm_bytes, ds.asm_range_count,
                     ds.asm_range_count == 1 ? "" : "s",
                     outbin[0] ? outbin : "(no output)");
        else if (!ds.diags.num_errors)
          status_set(&ds, "Build OK — %u bytes in %d range%s (%zu warning%s) -> %s",
                     ds.asm_bytes, ds.asm_range_count,
                     ds.asm_range_count == 1 ? "" : "s", ds.diags.num_warnings,
                     ds.diags.num_warnings == 1 ? "" : "s",
                     outbin[0] ? outbin : "(no output)");
        else
          status_set(&ds, "Build with %zu error%s, %zu warning%s",
                     ds.diags.num_errors, ds.diags.num_errors == 1 ? "" : "s",
                     ds.diags.num_warnings, ds.diags.num_warnings == 1 ? "" : "s");
      } else {
        if (ds.diags.num_errors || ds.diags.num_warnings)
          status_set(&ds, "Build FAILED (%zu error%s, %zu warning%s)",
                     ds.diags.num_errors, ds.diags.num_errors == 1 ? "" : "s",
                     ds.diags.num_warnings, ds.diags.num_warnings == 1 ? "" : "s");
        else
          status_set(&ds, "Build FAILED");
      }
      {
        char msg[512];
        if (ok && !ds.diags.num_errors && !ds.diags.num_warnings) {
          snprintf(msg, sizeof(msg), "Assembled successfully.\n%u bytes in %d range%s\n%s",
                   ds.asm_bytes, ds.asm_range_count,
                   ds.asm_range_count == 1 ? "" : "s",
                   outbin[0] ? outbin : "(no output)");
          dialog_ok(&theme, "Build OK", msg);
        } else if (ok && !ds.diags.num_errors) {
          snprintf(msg, sizeof(msg), "Assembled with %zu warning%s.\n%u bytes in %d range%s\n%s",
                   ds.diags.num_warnings, ds.diags.num_warnings == 1 ? "" : "s",
                   ds.asm_bytes, ds.asm_range_count,
                   ds.asm_range_count == 1 ? "" : "s",
                   outbin[0] ? outbin : "(no output)");
          dialog_ok(&theme, "Build OK", msg);
        } else {
          snprintf(msg, sizeof(msg), "%s: %zu error%s, %zu warning%s.\nSee diagnostics (F8/F11)",
                   ok ? "Completed" : "Build failed",
                   ds.diags.num_errors, ds.diags.num_errors == 1 ? "" : "s",
                   ds.diags.num_warnings, ds.diags.num_warnings == 1 ? "" : "s");
          dialog_ok(&theme, ok ? "Build" : "Build FAILED", msg);
        }
      }
      if (ok && !ds.diags.num_errors && ds.asm_valid && ds.asm_range_count > 0) {
        int _first_page = (ds.asm_range_count > 0) ? (ds.asm_ranges[0].start >> 8) : 0;
        static uint8_t _tui_written[65536];
        memset(_tui_written, 0, sizeof(_tui_written));
        for (int _ri = 0; _ri < ds.asm_range_count; _ri++)
          for (int _a = ds.asm_ranges[_ri].start; _a <= ds.asm_ranges[_ri].end; _a++)
            _tui_written[_a] = 1;
        tui_memmap_dialog(&theme, _tui_written, ds.asm_range_count,
                          (const tui_range_t *)ds.asm_ranges, ds.asm_bytes, _first_page);
      }
      if (ds.diags.count) {
        ds.cur_diag_idx = 0;
        jump_to_diag(buf, &v, &ds.diags.items[0]);
      }
      continue;
    }
    if (ch == KEY_F(11) || ch == KEY_F(20)) {
      if (ds.diags.count) {
        ds.cur_diag_idx = diagdb_prev_index(&ds.diags, ds.cur_diag_idx);
        if (ds.cur_diag_idx != (size_t)-1)
          jump_to_diag(buf, &v, &ds.diags.items[ds.cur_diag_idx]);
      }
      continue;
    }

    // ---- Jump history: Shift+F12=back, F12=forward ----
    if (ch == KEY_SHIFT_F12) {
      size_t jl;
      int jc;
      if (jump_buf_back(&jbuf, &jl, &jc)) {
        int h2, w2;
        getmaxyx(stdscr, h2, w2);
        (void)w2;
        int th = h2 - 1 - TOPBAR_H;
        if (th < 1)
          th = 1;
        int half = th / 2;
        if (jl < (size_t)half) {
          v.top = 0;
          v.row = (int)jl;
        } else {
          v.top = jl - (size_t)half;
          v.row = half;
        }
        v.col = jc;
        v.left = 0;
        status_set(&ds, "jump back");
      } else {
        status_set(&ds, "jump history: at start");
      }
      continue;
    }
    if (ch == KEY_F(12)) {
      size_t jl;
      int jc;
      if (jump_buf_fwd(&jbuf, &jl, &jc)) {
        int h2, w2;
        getmaxyx(stdscr, h2, w2);
        (void)w2;
        int th = h2 - 1 - TOPBAR_H;
        if (th < 1)
          th = 1;
        int half = th / 2;
        if (jl < (size_t)half) {
          v.top = 0;
          v.row = (int)jl;
        } else {
          v.top = jl - (size_t)half;
          v.row = half;
        }
        v.col = jc;
        v.left = 0;
        status_set(&ds, "jump forward");
      } else {
        status_set(&ds, "jump history: at end");
      }
      continue;
    }

    // ---- File operations ----
    if (ch == ACT_NEW_FILE) {
      int a = confirm_discard_or_save(&theme, buf, io_path, io_path_cap,
                                      suggested_path);
      if (a == 0)
        continue;
      if (a == 2 &&
          !do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
        status_set(&ds, "save failed");
        continue;
      }
      buffer_free(buf);
      buffer_init_empty(buf);
      fold_map_rebuild(&ds.fold, (const char **)buf->lines, buf->count,
                       g_prefs.fold_comments != 0, g_prefs.fold_macros != 0);
      if (io_path && io_path_cap)
        io_path[0] = 0;
      v.top = 0;
      v.row = 0;
      v.col = 0;
      v.left = 0;
      undo_init(&undo);
      set_term_title("6502caEd", "(untitled)");
      sem_dirty = true;
      status_set(&ds, "new file");
      continue;
    }
    if (ch == 15) { // Ctrl+O
      int a = confirm_discard_or_save(&theme, buf, io_path, io_path_cap,
                                      suggested_path);
      if (a == 0)
        continue;
      if (a == 2 &&
          !do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
        status_set(&ds, "save failed");
        continue;
      }
      if (do_open(&theme, buf, io_path, io_path_cap)) {
        v.top = 0;
        v.row = 0;
        v.col = 0;
        v.left = 0;
        undo_init(&undo);
        sem_dirty = true;
        status_set(&ds, "opened");
        fold_map_rebuild(&ds.fold, (const char **)buf->lines, buf->count,
                         g_prefs.fold_comments != 0, g_prefs.fold_macros != 0);
      } else
        status_set(&ds, "open cancelled");
      continue;
    }
    if (ch == 19) { // Ctrl+S
      if (do_save(&theme, buf, io_path, io_path_cap, suggested_path))
        status_set(&ds, "saved");
      else
        status_set(&ds, "save failed");
      continue;
    }
    if (ch == ACT_SAVE_AS) {
      if (do_save_as(&theme, buf, io_path, io_path_cap, suggested_path))
        status_set(&ds, "saved");
      else
        status_set(&ds, "save cancelled");
      continue;
    }
    if (ch == ACT_RECENT) {
      int a = confirm_discard_or_save(&theme, buf, io_path, io_path_cap,
                                      suggested_path);
      if (a == 0)
        continue;
      if (a == 2 &&
          !do_save(&theme, buf, io_path, io_path_cap, suggested_path)) {
        status_set(&ds, "save failed");
        continue;
      }
      if (do_open_recent(&theme, buf, io_path, io_path_cap)) {
        v.top = 0;
        v.row = 0;
        v.col = 0;
        v.left = 0;
        undo_init(&undo);
        sem_dirty = true;
        status_set(&ds, "opened");
      }
      continue;
    }

    // ---- Edit operations ----
    if (ch == ACT_REPLACE) {
      int nrep = do_replace(&theme, buf, &v, last_search, sizeof(last_search));
      if (nrep < 0)
        status_set(&ds, "replace failed");
      else
        status_set(&ds, "replaced %d", nrep);
      sem_dirty = true;
      continue;
    }
    if (ch == ACT_HELP || (!ms.active && ch == KEY_F(1))) {
      do_help(&theme);
      continue;
    }
    if (ch == ACT_ABOUT) {
      do_about(&theme);
      continue;
    }
    if (ch == ACT_IREF || ch == KEY_F(6)) {
      do_iref(&theme);
      continue;
    }
    if (ch == ACT_DREF || ch == KEY_F(7)) {
      do_dref(&theme);
      continue;
    }

    // ---- Autocomplete toggle (menu) ----
    if (ch == ACT_AUTOCOMPLETE) {
      g_prefs.autocomplete ^= 1;
      prefs_save();
      status_set(&ds, "Autocomplete: %s", g_prefs.autocomplete ? "on" : "off");
      continue;
    }

    // ---- Bookmarks ----
    if (ch == ACT_BOOKMARK_TOGGLE || ch == 2) { // Ctrl+B = 2
      size_t cur_line = v.top + (size_t)v.row;
      bookmark_toggle(&ds, cur_line);
      status_set(&ds, bookmark_find(&ds, cur_line) >= 0 ? "Bookmark set"
                                                        : "Bookmark cleared");
      continue;
    }
    if (ch == ACT_BOOKMARK_NEXT || ch == KEY_F(4)) {
      int nl = bookmark_next(&ds, v.top + (size_t)v.row);
      if (nl < 0) {
        status_set(&ds, "No bookmarks");
      } else {
        int sh2, sw2;
        getmaxyx(stdscr, sh2, sw2);
        (void)sw2;
        int th2 = sh2 - 1 - TOPBAR_H -
                  ((g_prefs.show_problems && ds.diags.count > 0) ? 1 : 0);
        if (th2 < 1)
          th2 = 1;
        int half = th2 / 2;
        v.top = (size_t)nl >= (size_t)half ? (size_t)nl - (size_t)half : 0;
        v.row = (int)((size_t)nl - v.top);
        v.col = 0;
        status_set(&ds, "Bookmark %d/%d", bookmark_find(&ds, (size_t)nl) + 1,
                   ds.bookmark_count);
      }
      continue;
    }
    if (ch == ACT_BOOKMARK_PREV || ch == KEY_SHIFT_F4) {
      int nl = bookmark_prev(&ds, v.top + (size_t)v.row);
      if (nl < 0) {
        status_set(&ds, "No bookmarks");
      } else {
        int sh2, sw2;
        getmaxyx(stdscr, sh2, sw2);
        (void)sw2;
        int th2 = sh2 - 1 - TOPBAR_H -
                  ((g_prefs.show_problems && ds.diags.count > 0) ? 1 : 0);
        if (th2 < 1)
          th2 = 1;
        int half = th2 / 2;
        v.top = (size_t)nl >= (size_t)half ? (size_t)nl - (size_t)half : 0;
        v.row = (int)((size_t)nl - v.top);
        v.col = 0;
        status_set(&ds, "Bookmark %d/%d", bookmark_find(&ds, (size_t)nl) + 1,
                   ds.bookmark_count);
      }
      continue;
    }
    if (ch == ACT_BOOKMARK_CLEAR) {
      ds.bookmark_count = 0;
      status_set(&ds, "All bookmarks cleared");
      continue;
    }

    if (ch == 6) { // Ctrl+F Find
      char q[128];
      if (prompt_string(&theme, "Find", q, sizeof(q), last_search)) {
        strncpy(last_search, q, sizeof(last_search) - 1);
        last_search[sizeof(last_search) - 1] = 0;
        if (!find_next(buf, &v, last_search, 1))
          status_set(&ds, "not found");
        else
          status_set(&ds, "found");
      }
      continue;
    }
    if (ch == KEY_F(3)) {
      if (last_search[0] && !find_next(buf, &v, last_search, 1))
        status_set(&ds, "not found");
      continue;
    }
    if (ch == KEY_F(15) || ch == KEY_F(27)) {
      if (last_search[0] && !find_next(buf, &v, last_search, 0))
        status_set(&ds, "not found");
      continue;
    }

    if (ch == 7) {
      do_goto(&theme, buf, &v);
      continue;
    } // Ctrl+G

    if (ch == 4) { // Ctrl+D duplicate line
      do_dup_line(buf, &v);
      sem_dirty = true;
      status_set(&ds, "duplicated");
      continue;
    }
    if (ch == ACT_DUP_LINE) {
      undo_push(&undo, buf, &v);
      do_dup_line(buf, &v);
      sem_dirty = true;
      status_set(&ds, "duplicated");
      continue;
    }
    if (ch == ACT_REM_DUP_LINES) {
      undo_push(&undo, buf, &v);
      do_remove_dup_lines(buf, &v);
      sem_dirty = true;
      status_set(&ds, "removed duplicates");
      continue;
    }

    if (ch == ACT_FMT_LAYOUT) {
      apply_text_transform(buf, &v, XFORM_LAYOUT);
      sem_dirty = true;
      status_set(&ds, "formatted layout");
      continue;
    }
    if (ch == ACT_UPPER_CASE) {
      apply_text_transform(buf, &v, XFORM_UPPER);
      sem_dirty = true;
      status_set(&ds, "uppercased");
      continue;
    }
    if (ch == ACT_LOWER_CASE) {
      apply_text_transform(buf, &v, XFORM_LOWER);
      sem_dirty = true;
      status_set(&ds, "lowercased");
      continue;
    }
    if (ch == ACT_UPPER_SKIP) {
      apply_text_transform(buf, &v, XFORM_UPPER);
      sem_dirty = true;
      status_set(&ds, "uppercase (skip comments)");
      continue;
    }
    if (ch == ACT_LOWER_SKIP) {
      apply_text_transform(buf, &v, XFORM_LOWER);
      sem_dirty = true;
      status_set(&ds, "lowercase (skip comments)");
      continue;
    }
    if (ch == ACT_UPPER_ALL) {
      apply_text_transform(buf, &v, XFORM_UPPER_ALL);
      sem_dirty = true;
      status_set(&ds, "uppercase (incl. comments)");
      continue;
    }
    if (ch == ACT_LOWER_ALL) {
      apply_text_transform(buf, &v, XFORM_LOWER_ALL);
      sem_dirty = true;
      status_set(&ds, "lowercase (incl. comments)");
      continue;
    }
    if (ch == ACT_UPPER_INSTRS) {
      apply_text_transform(buf, &v, XFORM_UPPER_INSTRS);
      sem_dirty = true;
      status_set(&ds, "uppercase instructions");
      continue;
    }
    if (ch == ACT_LOWER_INSTRS) {
      apply_text_transform(buf, &v, XFORM_LOWER_INSTRS);
      sem_dirty = true;
      status_set(&ds, "lowercase instructions");
      continue;
    }
    if (ch == ACT_UNI_REPLACE) {
      apply_text_transform(buf, &v, XFORM_UNICODE);
      sem_dirty = true;
      status_set(&ds, "unicode replaced");
      continue;
    }
    if (ch == ACT_COMMENT_TOG) {
      undo_push(&undo, buf, &v);
      do_comment_toggle(buf, &v, &sel);
      sem_dirty = true;
      status_set(&ds, "toggled comments");
      continue;
    }

    // ---- View operations ----
    if (ch == ACT_TOGGLE_PROBS) {
      g_prefs.show_problems ^= 1;
      prefs_save();
      status_set(&ds, g_prefs.show_problems ? "problems panel: on"
                                            : "problems panel: off");
      continue;
    }
    if (ch == ACT_MEM_VIEWER) {
      do_mem_viewer(&theme, buf, &ds);
      continue;
    }
    if (ch == ACT_INCLUDES) {
      do_includes_list(&theme, buf, &v, io_path);
      continue;
    }
    if (ch == ACT_TAB_WIDTH) {
      do_tab_width_dialog(&theme);
      continue;
    }
    if (ch == ACT_COMMENT_COL) {
      do_comment_col_dialog(&theme);
      continue;
    }
    if (ch == ACT_TOGGLE_THEME) {
      toggle_theme(&theme);
      status_set(&ds, g_prefs.dark_mode ? "dark theme" : "light theme");
      continue;
    }
    if (ch == ACT_PREFS) {
      if (do_preferences_dialog(&theme))
        sem_dirty = true;
      continue;
    }
    if (ch == ACT_CONFIG_FILES) {
      do_config_files(&theme, io_path, editor_exe_path);
      continue;
    }
    /* Ctrl+F11 / menu = find references */
    if (ch == ACT_FIND_REFS) {
      do_find_refs(&theme, buf, &v, &ds);
      sem_dirty = true;
      continue;
    }

    /* Ctrl+Z = undo, Ctrl+Y = redo */
    if (ch == 26) { /* Ctrl+Z */
      undo_typing_run = false;
      if (undo_pop(&undo, buf, &v)) {
        int _h, _w;
        getmaxyx(stdscr, _h, _w);
        int _th = _h - 1 - TOPBAR_H;
        if (_th < 1)
          _th = 1;
        int _wt = (_w > gutter_w(buf->count ? buf->count : 1))
                      ? (_w - gutter_w(buf->count ? buf->count : 1))
                      : 1;
        clamp_cursor(buf, &v, _th, _wt);
        sem_dirty = true;
        status_set(&ds, "undo");
      } else {
        status_set(&ds, "nothing to undo");
      }
      continue;
    }
    if (ch == 25) { /* Ctrl+Y */
      undo_typing_run = false;
      if (redo_pop(&undo, buf, &v)) {
        int _h, _w;
        getmaxyx(stdscr, _h, _w);
        int _th = _h - 1 - TOPBAR_H;
        if (_th < 1)
          _th = 1;
        int _wt = (_w > gutter_w(buf->count ? buf->count : 1))
                      ? (_w - gutter_w(buf->count ? buf->count : 1))
                      : 1;
        clamp_cursor(buf, &v, _th, _wt);
        sem_dirty = true;
        status_set(&ds, "redo");
      } else {
        status_set(&ds, "nothing to redo");
      }
      continue;
    }

    // ---- Ctrl+F12: jump to definition of symbol under cursor ----
    if (ch == KEY_CTRL_F12) {
      // Push current position before jumping
      jump_buf_push(&jbuf, v.top + (size_t)v.row, v.col);
      size_t cur_line = v.top + (size_t)v.row;
      const char *cur_text =
          (cur_line < buf->count) ? buffer_get_line(buf, cur_line) : "";
      char sym[128] = {0};
      int col_pos = (int)v.left + v.col;
      if (col_pos < 0)
        col_pos = 0;
      if (tui_word_at_cursor(cur_text, (size_t)col_pos, sym, sizeof(sym)) > 0) {
        size_t def_line0 = 0;
        if (tui_find_definition(buf, sym, &def_line0)) {
          int h2, w2;
          getmaxyx(stdscr, h2, w2);
          (void)w2;
          int th = h2 - 1 - TOPBAR_H;
          if (th < 1)
            th = 1;
          // Centre the target line on screen
          int half = th / 2;
          if (def_line0 < (size_t)half) {
            v.top = 0;
            v.row = (int)def_line0;
          } else {
            v.top = def_line0 - (size_t)half;
            v.row = half;
          }
          v.col = 0;
          v.left = 0;
          status_set(&ds, "jumped to definition");
        } else {
          status_set(&ds, "no definition found for: %s", sym);
        }
      } else {
        status_set(&ds, "no symbol under cursor");
      }
      continue;
    }

    // ---- Fold toggle: Ctrl+] (ch == 29) ----
    if (ch == 29 && (g_prefs.fold_comments || g_prefs.fold_macros)) {
      size_t cur_buf = v.top + (size_t)v.row;
      /* Find the fold start at or before the cursor */
      if (ds.fold.lines && cur_buf < ds.fold.count) {
        size_t s = cur_buf;
        while (s > 0 && ds.fold.lines[s].kind != FL_FOLD_START)
          s--;
        if (ds.fold.lines[s].kind == FL_FOLD_START)
          fold_toggle(&ds.fold, s);
      }
      continue;
    }

    // ---- Selection toggle ----
    if (ch == 0 || ch == 20 || ch == KEY_F(11)) {
      if (!sel.active) {
        sel.active = true;
        sel.sl = v.top + (size_t)v.row;
        sel.sc = (v.col < 0) ? 0 : (size_t)v.col;
        sel.el = sel.sl;
        sel.ec = sel.sc;
        status_set(&ds, "select: on");
      } else {
        sel.active = false;
        status_set(&ds, "select: off");
      }
      continue;
    }
    if (sel.active && ch == 27) {
      sel.active = false;
      status_set(&ds, "select: off");
      continue;
    }

    // ---- Copy / Cut / Paste ----
    if (ch == 3 || ch == KEY_F(6)) { // Ctrl+C copy
      if (!sel.active) {
        status_set(&ds, "copy: no selection");
        continue;
      }
      size_t sl, sc, el, ec;
      sel_norm(&sel, &sl, &sc, &el, &ec);
      if (pos_cmp(sl, sc, el, ec) == 0) {
        sel.active = false;
        status_set(&ds, "copy: empty");
        continue;
      }
      char *txt = buffer_extract_range(buf, sl, sc, el, ec);
      if (!txt) {
        sel.active = false;
        status_set(&ds, "copy failed");
        continue;
      }
      int ok = clipboard_set_system(txt);
      free(txt);
      sel.active = false;
      status_set(&ds, ok ? "copied" : "copied (internal)");
      continue;
    }
    if (ch == 24 || ch == KEY_F(7)) { // Ctrl+X cut
      if (!sel.active) {
        status_set(&ds, "cut: no selection");
        continue;
      }
      size_t sl, sc, el, ec;
      sel_norm(&sel, &sl, &sc, &el, &ec);
      if (pos_cmp(sl, sc, el, ec) == 0) {
        sel.active = false;
        status_set(&ds, "cut: empty");
        continue;
      }
      char *txt = buffer_extract_range(buf, sl, sc, el, ec);
      if (!txt) {
        sel.active = false;
        status_set(&ds, "cut failed");
        continue;
      }
      int ok = clipboard_set_system(txt);
      free(txt);
      buffer_delete_range(buf, sl, sc, el, ec);
      v.top = sl;
      v.row = 0;
      v.col = (int)sc;
      sem_dirty = true;
      sel.active = false;
      status_set(&ds, ok ? "cut" : "cut (internal)");
      continue;
    }
    if (ch == 22 || ch == KEY_F(8)) { // Ctrl+V paste (F8 no longer used for
                                      // diag; diag moved to F8 above)
      char *clip = clipboard_get_system();
      if (clip && clip[0]) {
        if (sel.active) {
          size_t sl, sc, el, ec;
          sel_norm(&sel, &sl, &sc, &el, &ec);
          if (pos_cmp(sl, sc, el, ec) != 0) {
            buffer_delete_range(buf, sl, sc, el, ec);
            v.top = sl;
            v.row = 0;
            v.col = (int)sc;
          }
          sel.active = false;
        }
        undo_typing_run = false;
        undo_push(&undo, buf, &v);
        for (size_t i = 0; clip[i]; i++) {
          if (clip[i] == '\n')
            insert_newline(buf, &v);
          else {
            size_t li = v.top + (size_t)v.row;
            insert_char(buf, li, v.col, (unsigned char)clip[i]);
            v.col++;
          }
        }
        sem_dirty = true;
        status_set(&ds, "pasted");
      } else
        status_set(&ds, "paste: clipboard empty");
      free(clip);
      continue;
    }

    // ---- Cursor movement / text editing ----
    switch (ch) {
    case KEY_SLEFT:
      sel_begin_at_cursor(&sel, &v);
      if (v.col > 0)
        v.col--;
      else {
        size_t al = v.top + (size_t)v.row;
        if (buf->count && al > 0) {
          if (v.row > 0)
            v.row--;
          else if (v.top > 0)
            v.top--;
          size_t nl = v.top + (size_t)v.row;
          v.col = (int)strlen(buffer_get_line(buf, nl));
        }
      }
      sel_update_end(&sel, &v);
      break;
    case KEY_SRIGHT:
      sel_begin_at_cursor(&sel, &v);
      v.col++;
      sel_update_end(&sel, &v);
      break;
    case KEY_SR:
      sel_begin_at_cursor(&sel, &v);
      v.row--;
      ensure_cursor_visible_vertical(buf, &v, text_h);
      sel_update_end(&sel, &v);
      break;
#if !defined(KEY_SF) || (KEY_SF + 0) != (KEY_SR + 0)
    case KEY_SF:
      sel_begin_at_cursor(&sel, &v);
      {
        size_t al = v.top + (size_t)v.row;
        if (buf->count && al + 1 < buf->count) {
          v.row++;
          ensure_cursor_visible_vertical(buf, &v, text_h);
        }
      }
      sel_update_end(&sel, &v);
      break;
#endif
    case KEY_UP:
      undo_typing_run = false;
      v.row--;
      ensure_cursor_visible_vertical(buf, &v, text_h);
      break;
    case KEY_DOWN:
      undo_typing_run = false;
      {
        size_t al = v.top + (size_t)v.row;
        if (buf->count && al + 1 < buf->count) {
          v.row++;
          ensure_cursor_visible_vertical(buf, &v, text_h);
        }
      }
      break;
    case KEY_LEFT:
      undo_typing_run = false;
      if (v.col > 0)
        v.col--;
      else {
        size_t al = v.top + (size_t)v.row;
        if (buf->count && al > 0) {
          if (v.row > 0)
            v.row--;
          else if (v.top > 0)
            v.top--;
          v.col = (int)strlen(buffer_get_line(buf, v.top + (size_t)v.row));
        }
      }
      break;
    case KEY_RIGHT:
      undo_typing_run = false;
      v.col++;
      break;
    // Ctrl+Left: jump back one word; if at col 0, stay at beginning of line
    case KEY_CTRL_LEFT:
      undo_typing_run = false;
      {
        size_t li = v.top + (size_t)v.row;
        const char *ln = (li < buf->count) ? buffer_get_line(buf, li) : "";
        int c = v.col;
        if (c == 0)
          break; // already at start of line
        c--;     // step back onto previous char
        // skip trailing non-word chars
        while (c > 0 && !isalnum((unsigned char)ln[c]) && ln[c] != '_')
          c--;
        // skip word chars
        while (c > 0 && (isalnum((unsigned char)ln[c - 1]) || ln[c - 1] == '_'))
          c--;
        v.col = c;
      }
      break;
    // Ctrl+Right: jump forward one word; if no next word, go to end of line
    case KEY_CTRL_RIGHT:
      undo_typing_run = false;
      {
        size_t li = v.top + (size_t)v.row;
        const char *ln = (li < buf->count) ? buffer_get_line(buf, li) : "";
        int llen = (int)strlen(ln);
        int c = v.col;
        if (c >= llen)
          break; // already at end of line
        // skip current word chars
        while (c < llen && (isalnum((unsigned char)ln[c]) || ln[c] == '_'))
          c++;
        // skip non-word chars to start of next word
        while (c < llen && !isalnum((unsigned char)ln[c]) && ln[c] != '_')
          c++;
        v.col = c;
      }
      break;
    case KEY_PPAGE:
      undo_typing_run = false;
      v.top = (v.top > (size_t)text_h) ? v.top - (size_t)text_h : 0;
      v.row = 0;
      break;
    case KEY_NPAGE:
      undo_typing_run = false;
      {
        size_t mt =
            (buf->count > (size_t)text_h) ? (buf->count - (size_t)text_h) : 0;
        size_t step = (size_t)text_h;
        v.top = (v.top + step <= mt) ? v.top + step : mt;
        v.row = 0;
      }
      break;
    // Ctrl+Up: same as HOME (top of screen, no scroll)
    case KEY_CTRL_UP:
    // HOME: move to top visible line (no scroll); col unchanged
    case KEY_HOME:
      undo_typing_run = false;
      {
        v.row = 0;
        v.col = 0;
      }
      break;
    // Ctrl+Down: same as END (bottom of screen, no scroll)
    case KEY_CTRL_DOWN:
    // END: move to last visible line (or last file line if fewer); col = 0
    case KEY_END:
      undo_typing_run = false;
      {
        size_t last_visible = v.top + (size_t)(text_h - 1);
        if (buf->count == 0) {
          v.row = 0;
        } else if (last_visible >= buf->count) {
          v.row = (int)(buf->count - 1 - v.top);
        } else {
          v.row = text_h - 1;
        }
        v.col = 0;
      }
      break;
    // Ctrl+Home: jump to very first line, col 0 (scrolls to top)
    case KEY_CTRL_HOME: {
      v.top = 0;
      v.row = 0;
      v.col = 0;
    } break;
    // Ctrl+End: jump to very last line, col 0 (scrolls to show it)
    case KEY_CTRL_END: {
      if (buf->count == 0) {
        v.top = 0;
        v.row = 0;
      } else {
        size_t last = buf->count - 1;
        if (last < (size_t)text_h) {
          v.top = 0;
          v.row = (int)last;
        } else {
          v.top = last - (size_t)(text_h - 1);
          v.row = text_h - 1;
        }
      }
      v.col = 0;
    } break;
    case KEY_BACKSPACE:
    case 127:
    case 8: {
      undo_typing_run = false;
      undo_push(&undo, buf, &v);
      if (sel.active) {
        size_t sl, sc, el, ec;
        sel_norm(&sel, &sl, &sc, &el, &ec);
        if (pos_cmp(sl, sc, el, ec) != 0) {
          buffer_delete_range(buf, sl, sc, el, ec);
          v.top = sl; v.row = 0; v.col = (int)sc;
          jump_buf_adjust(&jbuf, sl, -(int)(el - sl));
        }
        sel.active = false;
        last_edit_line = v.top + (size_t)v.row;
        sem_dirty = true;
      } else {
        size_t _lc = buf->count;
        size_t _at = v.top + (size_t)v.row;
        backspace(buf, &v);
        last_edit_line = v.top + (size_t)v.row;
        sem_dirty = true;
        if (buf->count < _lc)
          jump_buf_adjust(&jbuf, _at > 0 ? _at - 1 : 0, -1);
      }
    } break;
    case KEY_DC: {
      undo_typing_run = false;
      undo_push(&undo, buf, &v);
      if (sel.active) {
        size_t sl, sc, el, ec;
        sel_norm(&sel, &sl, &sc, &el, &ec);
        if (pos_cmp(sl, sc, el, ec) != 0) {
          buffer_delete_range(buf, sl, sc, el, ec);
          v.top = sl; v.row = 0; v.col = (int)sc;
          jump_buf_adjust(&jbuf, sl, -(int)(el - sl));
        }
        sel.active = false;
        last_edit_line = v.top + (size_t)v.row;
        sem_dirty = true;
      } else {
        size_t _lc = buf->count;
        size_t _at = v.top + (size_t)v.row;
        delete_forward(buf, &v);
        last_edit_line = _at;
        sem_dirty = true;
        if (buf->count < _lc)
          jump_buf_adjust(&jbuf, _at, -1);
      }
    } break;
    case '\t': {
      // Insert tab_width spaces (one undo step per tab)
      undo_typing_run = false;
      undo_push(&undo, buf, &v);
      size_t li = v.top + (size_t)v.row;
      for (int ti = 0; ti < g_prefs.tab_width; ti++) {
        insert_char(buf, li, v.col, ' ');
        v.col++;
      }
      undo_typing_run = false;
      last_edit_line = li;
      sem_dirty = true;
    } break;
    case '\n':
    case '\r': {
      size_t _at = v.top + (size_t)v.row;
      undo_typing_run = false;
      undo_push(&undo, buf, &v);
      insert_newline(buf, &v);
      last_edit_line = v.top + (size_t)v.row;  /* new line is now current */
      sem_dirty = true;
      jump_buf_adjust(&jbuf, _at + 1, 1);
    } break;
    default:
      if (isprint(ch)) {
        /* If a selection is active, delete it first then insert the character. */
        if (sel.active) {
          size_t sl, sc, el, ec;
          sel_norm(&sel, &sl, &sc, &el, &ec);
          if (pos_cmp(sl, sc, el, ec) != 0) {
            undo_typing_run = false;
            undo_push(&undo, buf, &v);
            buffer_delete_range(buf, sl, sc, el, ec);
            v.top = sl; v.row = 0; v.col = (int)sc;
            jump_buf_adjust(&jbuf, sl, -(int)(el - sl));
          }
          sel.active = false;
        }
        size_t li = v.top + (size_t)v.row;
        if (!undo_typing_run) {
          undo_push(&undo, buf, &v);
        }
        undo_typing_run = true;
        insert_char(buf, li, v.col, ch);
        v.col++;
        last_edit_line = li;
        sem_dirty = true;

        /* Autocomplete: check prefix after inserting char */
        if (g_prefs.autocomplete) {
          const char *cur_line_text = buffer_get_line(buf, li);
          char prefix[AC_MAX_WORD];
          int plen = ac_word_at(cur_line_text, v.col, prefix, sizeof(prefix));
          if (plen >= 2 && !ac_in_comment_or_string(cur_line_text, v.col)) {
            char results[AC_MAX_RESULTS][AC_MAX_WORD];
            int nr = ac_build_list(prefix, &symdb, results, AC_MAX_RESULTS);
            /* Don't show if exact match (already typed the full word) */
            if (nr == 1 && strcasecmp(results[0], prefix) == 0)
              nr = 0;
            if (nr > 0) {
              /* Draw current frame first so editor is visible behind popup */
              draw(io_path, buf, &v, &ds, &theme, &symdb, &ms, &ctx, last_edit_line);
              /* Compute screen position of cursor */
              int gw = gutter_w(buf->count ? buf->count : 1);
              int scr_row = v.row + TOPBAR_H;
              int scr_col = gw + (v.col - (int)v.left);
              char accepted[AC_MAX_WORD] = {0};
              if (do_autocomplete_popup(&theme, results, nr, scr_row, scr_col,
                                        accepted, sizeof(accepted))) {
                /* Replace the typed prefix with the accepted word */
                int acc_len = (int)strlen(accepted);
                /* Delete the prefix chars already typed (inline) */
                for (int _d = 0; _d < plen; _d++) {
                  v.col--;
                  char *_s = buf->lines[li];
                  size_t _len = strlen(_s);
                  if (v.col >= 0 && (size_t)v.col < _len) {
                    memmove(_s + v.col, _s + v.col + 1, _len - (size_t)v.col);
                    buf->modified = true;
                  }
                }
                /* Insert the accepted word */
                for (int _i = 0; _i < acc_len; _i++) {
                  insert_char(buf, li, v.col, (unsigned char)accepted[_i]);
                  v.col++;
                }
                sem_dirty = true;
              }
            }
          }
        }
      }
      break;
    }

    if (sel.active) {
      sel.el = v.top + (size_t)v.row;
      sel.ec = (v.col < 0) ? 0 : (size_t)v.col;
    }
    clamp_cursor(buf, &v, text_h, w_text);
  }

  fold_map_free(&ds.fold);
  symdb_free(&symdb);
  diagdb_free(&ds.diags);
  undo_free(&undo);
  free(g_clipboard);
  g_clipboard = NULL;
  // Disable any-event mouse tracking before leaving ncurses
  printf("\033[?1003l");
  fflush(stdout);
  endwin();
}
