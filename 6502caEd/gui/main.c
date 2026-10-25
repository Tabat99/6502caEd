/*
  6502caEd GTK (gui)
  Part of 6502caEd cross compiler suite
  MJD 2026
*/

#include <ctype.h>
#include <errno.h>
#include <gtk/gtk.h>
#include <gtksourceview/gtksource.h>

/*
 * Lock-key status (Caps/Num):
 * Keep this GTK build dependency-light. We avoid linking against X11/XKB (or
 * loading it via gmodule) just to read LED indicators. Instead we use the
 * GDK modifier state.
 */
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Platform abstractions ───────────────────────────────────────────── */
#ifdef _WIN32
#  include <windows.h>   /* GetModuleFileNameA */
#  include <io.h>        /* _pipe, _dup, _dup2, _close, _read */
#  include <fcntl.h>     /* _O_BINARY */
#  ifndef PATH_MAX
#    define PATH_MAX 4096
#  endif
/* ssize_t is already defined by MinGW's corecrt.h — do not redefine */
/* realpath(path, NULL) allocates on POSIX; _fullpath needs a fixed buffer.
   Wrap it so callers get the same contract (heap string or NULL). */
static inline char *_gui_realpath(const char *path) {
  char buf[PATH_MAX];
  char *r = _fullpath(buf, path, PATH_MAX);
  return r ? strdup(r) : NULL;
}
#  define realpath(p,ignored)  _gui_realpath(p)
/* Redirect pipe/dup/dup2/read/close to their MinGW equivalents */
#  define pipe(fds)           _pipe((fds), 65536, _O_BINARY)
#  define dup(fd)             _dup(fd)
#  define dup2(a,b)           _dup2((a),(b))
#  define read(fd,buf,n)      _read((fd),(buf),(unsigned)(n))
#  define close(fd)           _close(fd)
/* exe dir helper */
static void _gui_exe_dir(char *out, size_t cap) {
  DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
  if (n == 0) { out[0] = 0; return; }
  char *sl = strrchr(out, '\\');
  char *sf = strrchr(out, '/');
  char *sep = sl > sf ? sl : sf;
  if (sep) *sep = 0; else out[0] = 0;
}
#else
#  include <unistd.h>    /* readlink, ssize_t, pipe, dup, dup2, read, close */
static void _gui_exe_dir(char *out, size_t cap) {
  ssize_t n = readlink("/proc/self/exe", out, cap - 1);
  if (n <= 0) { out[0] = 0; return; }
  out[n] = 0;
  char *slash = strrchr(out, '/');
  if (slash) *slash = 0; else out[0] = 0;
}
#endif
/* ───────────────────────────────────────────────────────────────────── */

// Use the existing assembler's diagnostics for semantic error highlighting.
#include "../license_text.h"
#include "../core/asm.h"
#include "../core/diag.h"
#include "../core/preproc.h"
#include "../core/source.h"
#include "../editor/runner.h"

#ifndef ED_SOURCE_DIR
#define ED_SOURCE_DIR "."
#endif

// GTK4 doesn't always expose MOD2 in headers depending on platform/version.
// MOD2 is commonly used for NumLock on X11.
#ifndef GDK_MOD2_MASK
#define GDK_MOD2_MASK ((GdkModifierType)(1u << 4))
#endif

#include "../editor/fold.h"
#include "../editor/version.h"

#ifdef ED_BUILD_SIM
#include "../sim/sim_gtk.h"
#include "../sim/simcfg.h"
#include "../sim/simprofile.h"
#endif
#define AppVerString APP_VERSION

typedef struct EditorDoc EditorDoc;

// NOTE: EditorDoc is defined later in this file.  When helper code that runs
// earlier needs to read the active document's path, it must do so via an
// accessor declared here and defined after the EditorDoc struct definition.
static const char *editor_doc_get_path(const EditorDoc *d);

// ---------------------------------------------------------------------------
// Jump history buffer (browser back/forward style, 10 slots)
// ---------------------------------------------------------------------------
#define JUMP_BUF_SIZE 10

/* Horizontal inset (px) for menu section separator lines.
   Increase to make separators shorter at each end. */
#define MENU_SEPARATOR_MARGIN 40
typedef struct {
  int line; // 0-based
  int col;  // 0-based
} jump_pos_t;

typedef struct {
  jump_pos_t slots[JUMP_BUF_SIZE];
  int count; // number of valid entries
  int cur;   // current position in history (-1 = at tip)
} jump_buf_t;

typedef struct AppState {
  GtkApplication *app;
  GtkWindow *win;
  GtkHeaderBar *hdr;
  GtkLabel *title;

  GtkNotebook *notebook;
  GPtrArray *docs; /* EditorDoc* */
  EditorDoc *active_doc;

  GtkSourceBuffer *buf; /* alias: active doc */
  GtkSourceView *view;  /* alias: active doc */
  GtkWidget *status;
  char *status_msg; // transient status text (e.g. "Ready", errors)
  guint status_poll_id;
  char *status_last;

  char *path;
  GFile *last_source_folder; /* session-only; shared by Open and Save As */
  gboolean dirty;

  // Highlight tags
  GtkTextTag *tag_instr;
  GtkTextTag *tag_dir;
  GtkTextTag *tag_data;
  GtkTextTag *tag_num;
  GtkTextTag *tag_label;
  GtkTextTag *tag_addr_hex;
  GtkTextTag *tag_addr_dec;
  GtkTextTag *tag_addr_bin;
  GtkTextTag *tag_imm_hex;
  GtkTextTag *tag_imm_dec;
  GtkTextTag *tag_imm_bin;
  GtkTextTag *tag_imm_char;
  GtkTextTag *tag_dv_hex;
  GtkTextTag *tag_dv_dec;
  GtkTextTag *tag_dv_bin;
  GtkTextTag *tag_dv_str;
  GtkTextTag *tag_hover_def;
  GtkTextTag *tag_reg;
  GtkTextTag *tag_comment;
  GtkTextTag *tag_string;

  guint hl_idle_id;

  GtkTextTag *tag_error;       // semantic errors (dup labels etc)
  GtkTextTag *tag_inactive;    // dim inactive preprocessor branches
  GtkTextTag *tag_fold_hidden; /* invisible lines when folded */
  GtkTextTag *tag_fold_marker; /* summary line style */

  /* Preference: semantic error rendering.
     FALSE => red/bold text (default)
     TRUE  => red underline (keep normal text color) */
  gboolean error_underline;

  // Theme
  gboolean dark_mode; // TRUE=dark, FALSE=light
  GtkCssProvider *css;
  GtkCssProvider *css_font; /* separate provider for font overrides */

  GtkTextTag *tag_curline;
  int curline_applied; // -1 initially

  // Find/Replace bar (in-window)
  GtkSourceSearchSettings *search_settings;
  GtkSourceSearchContext *search_ctx;
  GtkWidget *findbar;
  GtkWidget *find_entry;
  GtkWidget *find_case_btn;
  gboolean find_match_case; /* FALSE => case-insensitive */
  GtkWidget *replace_entry;
  GtkWidget *find_prev_btn;
  GtkWidget *find_next_btn;
  GtkWidget *replace_btn;
  GtkWidget *replace_all_btn;
  GtkWidget *find_close_btn;
  GtkWidget *replace_label;
  gboolean findbar_replace_mode;
  GtkWidget *find_scope_drop; /* scope selector: 0=This Tab, 1=All Tabs, 2=All Linked Files */
  int find_scope;             /* 0/1/2 */

  /* Problems panel (bottom diagnostics list) */
  gboolean problems_enabled; /* Preferences toggle */
  gboolean problems_visible; /* Current UI state (shown vs hidden) */
  GtkWidget
      *problems_paned; /* vertical splitter containing editor + problems */
  GtkWidget *problems_revealer;
  GtkWidget *problems_view;
  GListStore *problems_store;
  gboolean problems_user_hidden;
  guint problems_refresh_idle_id;
  guint problems_init_idle_id;
  gboolean problems_refresh_force_show_pending;
  int problems_filter; /* 0=All, 1=Assemble, 2=Editor */
  GtkWidget *problems_filter_dd;
  GtkSelectionModel
      *problems_sel; /* GtkSingleSelection for the problems list */
  gboolean
      problems_autojump_pending; /* auto-jump to first issue after assemble */
  gboolean problems_clamping;
  int problems_min_end_px;
  int problems_default_end_px;
  int problems_last_end_px;    /* last non-collapsed end height (px), 0=unset */
  int problems_want_end_px;    /* desired end height while clamping/resizing */
  int problems_collapsed_pos;  /* cached paned position when collapsed */
  gboolean problems_collapsed; /* true if end child effectively hidden */
  /* problems_visible already declared above */

  /* Small transient confirmation toast (e.g. Problems copied). */
  GtkWidget *copy_toast;
  GtkWidget *copy_toast_label;
  guint copy_toast_hide_id;

  /* Assemble options */
  gboolean asm_clear_mem; /* TRUE: clear 64KB image before assembling */
  gboolean allow_undoc;   /* TRUE: allow undocumented/illegal opcodes */
  gboolean fold_comments; /* TRUE: fold comment blocks (2+ lines) */
  gboolean fold_macros;   /* TRUE: fold macro bodies */
  fold_map_t fold_map;    /* per-document fold state */
  char src_ext[16];       /* default source file extension, e.g. ".asm" */
  char bin_ext[16];       /* default binary file extension, e.g. ".bin" */
  char font_family[128];  /* editor font family, e.g. "Monospace" */
  int font_size;          /* editor font size in points, e.g. 12 */
  int default_font_size;  /* font size to restore on Ctrl+0 */
  gboolean default_font_size_set; /* true once set explicitly */
  /* Recent files — most-recently-used first */
#define RECENT_FILES_MAX 10
  char *recent_files[RECENT_FILES_MAX];
  GMenu *recent_menu;        /* dynamic submenu rebuilt on open */
  int win_width, win_height; /* saved window geometry */

  /* Jump history buffer for back/forward navigation */
  jump_buf_t jump_buf;

  /* Editor preferences */
  gboolean auto_indent;      /* auto-indent enabled */
  gboolean word_wrap;        /* word wrap enabled */
  gboolean reopen_last_file; /* reopen files from last session on startup */
  gboolean _prefs_loaded;    /* internal: TRUE after first gtk_prefs_load */
  int tab_width;             /* spaces per tab */

  /* Commenting preferences */
  gboolean keep_comment_aligned;   /* align comment marker column on comment */
  int comment_align_col;           /* target column (0=disabled) */
  gboolean comment_use_slashslash; /* FALSE=';' (default), TRUE='//' */

  /* Ctrl+K chord tracking */
  gboolean chord_ctrlk_pending;
  guint chord_ctrlk_timeout_id;

  /* Diagnostics sources feeding Problems panel */
  diags_t sem_diags; /* background/inline (buffer) diagnostics */
  diags_t asm_diags; /* last explicit Assemble diagnostics */

  /* Memory image (64KB) for viewer/editor */
  guint8 *mem_image;            /* active memory image (malloc 64KB) */
  gboolean mem_valid;           /* has meaningful contents */
  gboolean mem_dirty;           /* edited via memory viewer */
  asm_cpu_t mem_cpu;            /* CPU of last assembly, for disassembly */
  gpointer mem_view;            /* (MemViewState*) active window, if any */
  guint16 mem_last_addr;        /* address when viewer was last closed */
  gboolean mem_last_addr_valid; /* TRUE once viewer has been used */
  guint16 mem_asm_start;        /* min_written from last successful assemble */
  gboolean mem_asm_valid;       /* TRUE once assembly has been done */
  /* Set when a tab-close is pending after a Save As dialog completes */
  GtkWidget *pending_close_page; /* page widget to close after save-as */
  EditorDoc *pending_close_doc;  /* doc to destroy after save-as */

  /* Editor colour configuration (overridable via config file next to the
   * executable) */
  struct {
    char instr[16];
    char dir[16];
    char data[16];
    char num[16];
    char label[16];
    char hover_bg[16];
    char reg[16];
    char comment[16];
    char str[16];
    char inactive[16];
    // Underline colour used for diagnostics (separate from text colour)
    char err_underline[16];
    /* Address operand sub-colours (default to num) */
    char addr_hex[16];
    char addr_dec[16];
    char addr_bin[16];
    /* Immediate operand sub-colours (default to num) */
    char imm_hex[16];
    char imm_dec[16];
    char imm_bin[16];
    char imm_char[16];
    /* Data-directive value sub-colours (default to num / str) */
    char dv_hex[16];
    char dv_dec[16];
    char dv_bin[16];
    char dv_str[16];
  } colors;

  /* Alt+numpad character entry (Windows-style) */
  gboolean alt_numpad_active; /* TRUE while Alt is held with numpad digits */
  int alt_numpad_accum;       /* accumulated decimal value */

  /* Bookmarks */
#define GTK_MAX_BOOKMARKS 64
  int bookmarks[GTK_MAX_BOOKMARKS]; /* 0-based line numbers */
  int bookmark_count;
  GtkSourceGutterRenderer *gutter_renderer; /* our FoldLineNums renderer */
  GtkWidget *context_menu; /* right-click nested popover menu */
  gboolean autocomplete;   /* TRUE: show completion popup (default) */

} AppState;

/* Push a new position onto the jump history, discarding any forward entries.
 * If the buffer is full the oldest entry is evicted. */
static void jump_buf_push(jump_buf_t *jb, int line, int col) {
  // Discard any forward history beyond cur
  if (jb->cur >= 0 && jb->cur < jb->count - 1) {
    jb->count = jb->cur + 1;
  }
  if (jb->count == JUMP_BUF_SIZE) {
    // Shift oldest entry out
    memmove(&jb->slots[0], &jb->slots[1],
            (JUMP_BUF_SIZE - 1) * sizeof(jump_pos_t));
    jb->count--;
    if (jb->cur > 0)
      jb->cur--;
  }
  jb->slots[jb->count++] = (jump_pos_t){line, col};
  jb->cur = jb->count - 1; // tip
}

/* Move back in the jump history and write the destination to
 * *out_line/*out_col. ret: 1 if a previous entry exists, 0 if already at the
 * start */
static int jump_buf_back(jump_buf_t *jb, int *out_line, int *out_col) {
  if (jb->cur <= 0 || jb->count == 0)
    return 0;
  jb->cur--;
  *out_line = jb->slots[jb->cur].line;
  *out_col = jb->slots[jb->cur].col;
  return 1;
}

/* Move forward in the jump history and write the destination to
 * *out_line/*out_col. ret: 1 if a forward entry exists, 0 if already at the tip
 */
static int jump_buf_fwd(jump_buf_t *jb, int *out_line, int *out_col) {
  if (jb->cur < 0 || jb->cur >= jb->count - 1)
    return 0;
  jb->cur++;
  *out_line = jb->slots[jb->cur].line;
  *out_col = jb->slots[jb->cur].col;
  return 1;
}

/* Adjust stored jump positions when lines are inserted (delta>0) or
 * deleted (delta<0) at at_line, keeping history aligned with the buffer. */
static void jump_buf_adjust(jump_buf_t *jb, int at_line, int delta) {
  // Called when lines are inserted (+delta) or deleted (-delta) at at_line.
  // Best-effort: shift stored line numbers.
  for (int i = 0; i < jb->count; i++) {
    if (delta < 0) {
      // Lines deleted: remove entries on deleted lines, shift those below
      int del_start = at_line;
      int del_end = at_line + (-delta);
      if (jb->slots[i].line >= del_start && jb->slots[i].line < del_end) {
        // Entry is on a deleted line — remove it
        memmove(&jb->slots[i], &jb->slots[i + 1],
                (jb->count - i - 1) * sizeof(jump_pos_t));
        jb->count--;
        if (jb->cur >= i && jb->cur > 0)
          jb->cur--;
        i--;
      } else if (jb->slots[i].line >= del_end) {
        jb->slots[i].line += delta;
      }
    } else {
      if (jb->slots[i].line >= at_line)
        jb->slots[i].line += delta;
    }
  }
}

// Forward declaration needed because include-tree callbacks can request a
// highlight update before the helper is defined later in this file.
static void schedule_highlight(AppState *s);
// Forward declaration needed because gtk_prefs_apply_kv calls it before
// definition.
static void apply_editor_prefs(AppState *s);
static void apply_folds(AppState *s);                              /* fwd */
static void apply_fold_tags(AppState *s);                          /* fwd */
static void gtk_fold_toggle_line(AppState *s, int line_num);       /* fwd */
static void apply_window_dark_hint(GtkWidget *win, gboolean dark); /* fwd */

// ------------------------------------------------------------
// Colour configuration
// ------------------------------------------------------------

static void colors_set_defaults(AppState *s) {
  /* Built-in defaults vary by dark/light mode so the editor looks correct
   * even before any .colours file is loaded. */
  if (s->dark_mode) {
    g_strlcpy(s->colors.instr, "#2aa1d3", sizeof(s->colors.instr));
    g_strlcpy(s->colors.dir, "#b48ead", sizeof(s->colors.dir));
    g_strlcpy(s->colors.data, "#7ecfc0", sizeof(s->colors.data));
    g_strlcpy(s->colors.num, "#d19a66", sizeof(s->colors.num));
    g_strlcpy(s->colors.label, "#00ff55", sizeof(s->colors.label));
    g_strlcpy(s->colors.hover_bg, "#204020", sizeof(s->colors.hover_bg));
    g_strlcpy(s->colors.reg, "#98c379", sizeof(s->colors.reg));
    g_strlcpy(s->colors.comment, "#70b070", sizeof(s->colors.comment));
    g_strlcpy(s->colors.str, "#d09070", sizeof(s->colors.str));
    g_strlcpy(s->colors.inactive, "#888888", sizeof(s->colors.inactive));
    g_strlcpy(s->colors.err_underline, "#ff5555",
              sizeof(s->colors.err_underline));
  } else {
    g_strlcpy(s->colors.instr, "#0055aa", sizeof(s->colors.instr));
    g_strlcpy(s->colors.dir, "#7700aa", sizeof(s->colors.dir));
    g_strlcpy(s->colors.data, "#006688", sizeof(s->colors.data));
    g_strlcpy(s->colors.num, "#885500", sizeof(s->colors.num));
    g_strlcpy(s->colors.label, "#007700", sizeof(s->colors.label));
    g_strlcpy(s->colors.hover_bg, "#c8f0c8", sizeof(s->colors.hover_bg));
    g_strlcpy(s->colors.reg, "#336600", sizeof(s->colors.reg));
    g_strlcpy(s->colors.comment, "#448844", sizeof(s->colors.comment));
    g_strlcpy(s->colors.str, "#884400", sizeof(s->colors.str));
    g_strlcpy(s->colors.inactive, "#aaaaaa", sizeof(s->colors.inactive));
    g_strlcpy(s->colors.err_underline, "#cc0000",
              sizeof(s->colors.err_underline));
  }
  /* Sub-colours default to their group colour */
  g_strlcpy(s->colors.addr_hex, s->colors.num, sizeof(s->colors.addr_hex));
  g_strlcpy(s->colors.addr_dec, s->colors.num, sizeof(s->colors.addr_dec));
  g_strlcpy(s->colors.addr_bin, s->colors.num, sizeof(s->colors.addr_bin));
  g_strlcpy(s->colors.imm_hex, s->colors.num, sizeof(s->colors.imm_hex));
  g_strlcpy(s->colors.imm_dec, s->colors.num, sizeof(s->colors.imm_dec));
  g_strlcpy(s->colors.imm_bin, s->colors.num, sizeof(s->colors.imm_bin));
  g_strlcpy(s->colors.imm_char, s->colors.num, sizeof(s->colors.imm_char));
  g_strlcpy(s->colors.dv_hex, s->colors.num, sizeof(s->colors.dv_hex));
  g_strlcpy(s->colors.dv_dec, s->colors.num, sizeof(s->colors.dv_dec));
  g_strlcpy(s->colors.dv_bin, s->colors.num, sizeof(s->colors.dv_bin));
  g_strlcpy(s->colors.dv_str, s->colors.str, sizeof(s->colors.dv_str));
}

static gboolean colors_is_hex6(const char *v) {
  if (!v)
    return FALSE;
  if (v[0] != '#')
    return FALSE;
  if (strlen(v) != 7)
    return FALSE;
  for (int i = 1; i < 7; i++) {
    char c = v[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return FALSE;
  }
  return TRUE;
}

/* Apply one key=value pair from a colour config file to AppState.
 * k is the colour role name, v is a #RRGGBB hex colour string. */
static void colors_apply_kv(AppState *s, const char *k, const char *v) {
  if (!k || !v)
    return;
  if (!colors_is_hex6(v))
    return;

  // keys are case-insensitive
  if (g_ascii_strcasecmp(k, "instr") == 0 ||
      g_ascii_strcasecmp(k, "instruction") == 0)
    g_strlcpy(s->colors.instr, v, sizeof(s->colors.instr));
  else if (g_ascii_strcasecmp(k, "dir") == 0 ||
           g_ascii_strcasecmp(k, "directive") == 0)
    g_strlcpy(s->colors.dir, v, sizeof(s->colors.dir));
  else if (g_ascii_strcasecmp(k, "data") == 0)
    g_strlcpy(s->colors.data, v, sizeof(s->colors.data));
  else if (g_ascii_strcasecmp(k, "num") == 0 ||
           g_ascii_strcasecmp(k, "number") == 0)
    g_strlcpy(s->colors.num, v, sizeof(s->colors.num));
  else if (g_ascii_strcasecmp(k, "label") == 0)
    g_strlcpy(s->colors.label, v, sizeof(s->colors.label));
  else if (g_ascii_strcasecmp(k, "hover_bg") == 0 ||
           g_ascii_strcasecmp(k, "hover") == 0 ||
           g_ascii_strcasecmp(k, "label_hover_bg") == 0)
    g_strlcpy(s->colors.hover_bg, v, sizeof(s->colors.hover_bg));
  else if (g_ascii_strcasecmp(k, "reg") == 0 ||
           g_ascii_strcasecmp(k, "register") == 0)
    g_strlcpy(s->colors.reg, v, sizeof(s->colors.reg));
  else if (g_ascii_strcasecmp(k, "comment") == 0)
    g_strlcpy(s->colors.comment, v, sizeof(s->colors.comment));
  else if (g_ascii_strcasecmp(k, "string") == 0 ||
           g_ascii_strcasecmp(k, "str") == 0)
    g_strlcpy(s->colors.str, v, sizeof(s->colors.str));
  else if (g_ascii_strcasecmp(k, "inactive") == 0)
    g_strlcpy(s->colors.inactive, v, sizeof(s->colors.inactive));
  else if (g_ascii_strcasecmp(k, "err_underline") == 0 ||
           g_ascii_strcasecmp(k, "error_underline") == 0 ||
           g_ascii_strcasecmp(k, "diag_underline") == 0)
    g_strlcpy(s->colors.err_underline, v, sizeof(s->colors.err_underline));
  /* Address sub-colours */
  else if (g_ascii_strcasecmp(k, "addr_hex") == 0 ||
           g_ascii_strcasecmp(k, "address_hex") == 0)
    g_strlcpy(s->colors.addr_hex, v, sizeof(s->colors.addr_hex));
  else if (g_ascii_strcasecmp(k, "addr_dec") == 0 ||
           g_ascii_strcasecmp(k, "address_dec") == 0 ||
           g_ascii_strcasecmp(k, "addr_decimal") == 0)
    g_strlcpy(s->colors.addr_dec, v, sizeof(s->colors.addr_dec));
  else if (g_ascii_strcasecmp(k, "addr_bin") == 0 ||
           g_ascii_strcasecmp(k, "address_bin") == 0 ||
           g_ascii_strcasecmp(k, "addr_binary") == 0)
    g_strlcpy(s->colors.addr_bin, v, sizeof(s->colors.addr_bin));
  /* addr group shorthand — sets all three */
  else if (g_ascii_strcasecmp(k, "addr") == 0 ||
           g_ascii_strcasecmp(k, "address") == 0) {
    g_strlcpy(s->colors.addr_hex, v, sizeof(s->colors.addr_hex));
    g_strlcpy(s->colors.addr_dec, v, sizeof(s->colors.addr_dec));
    g_strlcpy(s->colors.addr_bin, v, sizeof(s->colors.addr_bin));
  }
  /* Immediate sub-colours */
  else if (g_ascii_strcasecmp(k, "imm_hex") == 0 ||
           g_ascii_strcasecmp(k, "immediate_hex") == 0)
    g_strlcpy(s->colors.imm_hex, v, sizeof(s->colors.imm_hex));
  else if (g_ascii_strcasecmp(k, "imm_dec") == 0 ||
           g_ascii_strcasecmp(k, "immediate_dec") == 0 ||
           g_ascii_strcasecmp(k, "imm_decimal") == 0)
    g_strlcpy(s->colors.imm_dec, v, sizeof(s->colors.imm_dec));
  else if (g_ascii_strcasecmp(k, "imm_bin") == 0 ||
           g_ascii_strcasecmp(k, "immediate_bin") == 0 ||
           g_ascii_strcasecmp(k, "imm_binary") == 0)
    g_strlcpy(s->colors.imm_bin, v, sizeof(s->colors.imm_bin));
  else if (g_ascii_strcasecmp(k, "imm_char") == 0 ||
           g_ascii_strcasecmp(k, "immediate_char") == 0 ||
           g_ascii_strcasecmp(k, "imm_character") == 0)
    g_strlcpy(s->colors.imm_char, v, sizeof(s->colors.imm_char));
  /* imm group shorthand — sets all four */
  else if (g_ascii_strcasecmp(k, "imm") == 0 ||
           g_ascii_strcasecmp(k, "immediate") == 0) {
    g_strlcpy(s->colors.imm_hex, v, sizeof(s->colors.imm_hex));
    g_strlcpy(s->colors.imm_dec, v, sizeof(s->colors.imm_dec));
    g_strlcpy(s->colors.imm_bin, v, sizeof(s->colors.imm_bin));
    g_strlcpy(s->colors.imm_char, v, sizeof(s->colors.imm_char));
  }
  /* Data value sub-colours */
  else if (g_ascii_strcasecmp(k, "dv_hex") == 0 ||
           g_ascii_strcasecmp(k, "data_hex") == 0)
    g_strlcpy(s->colors.dv_hex, v, sizeof(s->colors.dv_hex));
  else if (g_ascii_strcasecmp(k, "dv_dec") == 0 ||
           g_ascii_strcasecmp(k, "data_dec") == 0 ||
           g_ascii_strcasecmp(k, "data_decimal") == 0)
    g_strlcpy(s->colors.dv_dec, v, sizeof(s->colors.dv_dec));
  else if (g_ascii_strcasecmp(k, "dv_bin") == 0 ||
           g_ascii_strcasecmp(k, "data_bin") == 0 ||
           g_ascii_strcasecmp(k, "data_binary") == 0)
    g_strlcpy(s->colors.dv_bin, v, sizeof(s->colors.dv_bin));
  else if (g_ascii_strcasecmp(k, "dv_str") == 0 ||
           g_ascii_strcasecmp(k, "data_str") == 0 ||
           g_ascii_strcasecmp(k, "data_string") == 0)
    g_strlcpy(s->colors.dv_str, v, sizeof(s->colors.dv_str));
  /* dv group shorthand — sets all four */
  else if (g_ascii_strcasecmp(k, "dv") == 0 ||
           g_ascii_strcasecmp(k, "data_value") == 0) {
    g_strlcpy(s->colors.dv_hex, v, sizeof(s->colors.dv_hex));
    g_strlcpy(s->colors.dv_dec, v, sizeof(s->colors.dv_dec));
    g_strlcpy(s->colors.dv_bin, v, sizeof(s->colors.dv_bin));
    g_strlcpy(s->colors.dv_str, v, sizeof(s->colors.dv_str));
  }
}

/* Try to load a colour config from a specific directory.
 * Tries .colours first, then .colors.  Returns TRUE on success.
 *
 * Section support: lines inside a [dark] block are only applied when
 * dark_mode is TRUE; lines inside a [light] block only when FALSE.
 * Lines before any section header (or inside a [common] block) apply
 * regardless of the current mode.  Unknown section names are skipped. */
static gboolean colors_load_from_dir(AppState *s, const gchar *dir) {
  if (!dir || !dir[0])
    return FALSE;
  gchar *path = g_build_filename(dir, "6502caEd.colours", NULL);
  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &contents, &len, NULL)) {
    g_free(path);
    path = g_build_filename(dir, "6502caEd.colors", NULL);
    if (!g_file_get_contents(path, &contents, &len, NULL)) {
      g_free(path);
      return FALSE;
    }
  }
  g_free(path);

  /* section_active: TRUE = apply current line, FALSE = skip it.
   * Starts TRUE so that lines before any section header always apply. */
  gboolean section_active = TRUE;

  gchar **lines = g_strsplit(contents, "\n", -1);
  for (gchar **pp = lines; pp && *pp; pp++) {
    gchar *line = g_strstrip(*pp);
    if (!line[0] || line[0] == ';' || line[0] == '#')
      continue;

    /* Section header: [dark], [light], or [common] */
    if (line[0] == '[') {
      gchar *close = strchr(line, ']');
      if (close) {
        *close = 0;
        gchar *name = g_strstrip(line + 1);
        if (g_ascii_strcasecmp(name, "dark") == 0)
          section_active = s->dark_mode;
        else if (g_ascii_strcasecmp(name, "light") == 0)
          section_active = !s->dark_mode;
        else if (g_ascii_strcasecmp(name, "common") == 0)
          section_active = TRUE;
        else
          section_active = FALSE; /* unknown section name — skip */
      }
      continue;
    }

    if (!section_active)
      continue;

    gchar *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = 0;
    colors_apply_kv(s, g_strstrip(line), g_strstrip(eq + 1));
  }
  g_strfreev(lines);
  g_free(contents);
  return TRUE;
}

/* Load colour config.  Search order (first match wins):
     1. directory of the currently open source file (project-specific)
     2. directory of the executable (installation-wide)
     3. ~/.config/6502caEd/  (user config)                              */
/* Load colours in cascade: user first (lowest priority), then exe dir,
   then project dir (highest priority). Missing keys fall back gracefully. */
static void colors_load_from_exe_dir(AppState *s) {
  // 3. User config dir (lowest priority — load first)
  {
    const char *home = g_get_home_dir();
    gchar *dir =
        g_build_filename(home ? home : ".", ".config", "6502caEd", NULL);
    colors_load_from_dir(s, dir);
    g_free(dir);
  }

  // 2. Executable dir (overrides user)
  {
    char exe[PATH_MAX];
    _gui_exe_dir(exe, sizeof(exe));
    if (exe[0]) {
      colors_load_from_dir(s, exe);
    }
  }

  // 1. Project dir (highest priority — load last so it wins)
  if (s->path && s->path[0]) {
    gchar *dir = g_path_get_dirname(s->path);
    colors_load_from_dir(s, dir);
    g_free(dir);
  }
}

// ------------------------------------------------------------
// Include tree viewer (disk-based, does not require open tabs)
// ------------------------------------------------------------
// ------------------------------------------------------------
// Preferences persistence  (~/.config/6502caEd/prefs.ini)
// Also searched next to executable and next to open source file.
// First file found wins for load; save always goes to user config dir.
// ------------------------------------------------------------

/* Build a config file path into out[cap].
   dir:  directory containing the file (NUL-terminated, or NULL → out[0]=0).
   file: filename to append (e.g. "prefs.ini"). */
static void config_path_join(char *out, size_t cap, const char *dir,
                             const char *file) {
  if (dir && dir[0])
    g_snprintf(out, cap, "%s/%s", dir, file);
  else
    out[0] = 0;
}

static void gtk_prefs_path_user(char *out, size_t cap) {
  const char *home = g_get_home_dir();
  gchar *dir = g_build_filename(home ? home : ".", ".config", "6502caEd", NULL);
  config_path_join(out, cap, dir, "prefs.ini");
  g_free(dir);
}

static void gtk_session_path(char *out, size_t cap) {
  const char *home = g_get_home_dir();
  gchar *dir = g_build_filename(home ? home : ".", ".config", "6502caEd", NULL);
  config_path_join(out, cap, dir, "session.ini");
  g_free(dir);
}

/* Build the prefs.ini path next to the running executable into out. */
static void gtk_prefs_path_exe(char *out, size_t cap) {
  char dir[PATH_MAX];
  _gui_exe_dir(dir, sizeof(dir));
  if (dir[0])
    config_path_join(out, cap, dir, "prefs.ini");
  else
    out[0] = 0;
}

/* Build the prefs.ini path next to the currently open file into out.
 * Set to empty string if no file is open. */
static void gtk_prefs_path_project(AppState *s, char *out, size_t cap) {
  if (s->path && s->path[0]) {
    gchar *dir = g_path_get_dirname(s->path);
    config_path_join(out, cap, dir, "prefs.ini");
    g_free(dir);
  } else {
    out[0] = 0;
  }
}

/* Apply a single key=value pair from prefs.ini to AppState.
   Also updates the corresponding GAction state so menu checkmarks stay in sync.
 */
static void gtk_prefs_apply_kv(AppState *s, const char *k, int v) {
  GAction *a;
  if (!strcmp(k, "dark_mode")) {
    s->dark_mode = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app), "dark");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->dark_mode));
  } else if (!strcmp(k, "reopen_last_file")) {
    s->reopen_last_file = v ? TRUE : FALSE;
  } else if (!strcmp(k, "word_wrap")) {
    s->word_wrap = v ? TRUE : FALSE;
  } else if (!strcmp(k, "auto_indent")) {
    s->auto_indent = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app), "auto_indent");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->auto_indent));
    apply_editor_prefs(s);
  } else if (!strcmp(k, "tab_width")) {
    if (v >= 1 && v <= 16)
      s->tab_width = v;
    apply_editor_prefs(s);
  } else if (!strcmp(k, "problems_enabled")) {
    s->problems_enabled = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app), "problems");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->problems_enabled));
  } else if (!strcmp(k, "error_underline")) {
    s->error_underline = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app), "error_underline");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->error_underline));
  } else if (!strcmp(k, "asm_clear_mem")) {
    s->asm_clear_mem = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app), "asm_clear_mem");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->asm_clear_mem));
  } else if (!strcmp(k, "fold_comments")) {
    s->fold_comments = v ? TRUE : FALSE;
  } else if (!strcmp(k, "fold_macros")) {
    s->fold_macros = v ? TRUE : FALSE;
  } else if (!strcmp(k, "allow_undoc")) {
    s->allow_undoc = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app), "allow_undoc");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->allow_undoc));
  } else if (!strcmp(k, "autocomplete")) {
    s->autocomplete = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app), "autocomplete");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->autocomplete));
  } else if (!strcmp(k, "keep_comment_aligned")) {
    s->keep_comment_aligned = v ? TRUE : FALSE;
    a = g_action_map_lookup_action(G_ACTION_MAP(s->app),
                                   "keep_comment_aligned");
    if (a)
      g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                g_variant_new_boolean(s->keep_comment_aligned));
  } else if (!strcmp(k, "comment_align_col")) {
    if (v >= 0 && v <= 200)
      s->comment_align_col = v;
  } else if (!strcmp(k, "comment_use_slashslash")) {
    s->comment_use_slashslash = v ? TRUE : FALSE;
  } else if (!strcmp(k, "win_width")) {
    if (v > 0)
      s->win_width = v;
  } else if (!strcmp(k, "win_height")) {
    if (v > 0)
      s->win_height = v;
  }
}

/* Parse a prefs.ini file and apply all key=value settings to AppState. */
static void gtk_prefs_load_from_file(AppState *s, const char *path) {
  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &contents, &len, NULL))
    return;
  gchar **lines = g_strsplit(contents, "\n", -1);
  for (gchar **pp = lines; pp && *pp; pp++) {
    gchar *line = g_strstrip(*pp);
    if (!line[0] || line[0] == ';' || line[0] == '#')
      continue;
    gchar *eq = strchr(line, '=');
    if (!eq)
      continue;
    *eq = 0;
    gchar *k = g_strstrip(line);
    gchar *vs = g_strstrip(eq + 1);
    /* String keys: handled before integer fallback */
    if (!strcmp(k, "src_ext")) {
      if (vs[0]) {
        /* Ensure leading dot */
        if (vs[0] != '.')
          g_snprintf(s->src_ext, sizeof(s->src_ext), ".%s", vs);
        else
          g_strlcpy(s->src_ext, vs, sizeof(s->src_ext));
      }
      continue;
    }
    if (!strcmp(k, "bin_ext")) {
      if (vs[0]) {
        if (vs[0] != '.')
          g_snprintf(s->bin_ext, sizeof(s->bin_ext), ".%s", vs);
        else
          g_strlcpy(s->bin_ext, vs, sizeof(s->bin_ext));
      }
      continue;
    }
    if (!strcmp(k, "font_family")) {
      if (vs[0])
        g_strlcpy(s->font_family, vs, sizeof(s->font_family));
      continue;
    }
    if (!strcmp(k, "default_font_size")) {
      int vd = (int)g_ascii_strtoll(vs, NULL, 10);
      if (vd >= 8 && vd <= 72) {
        s->default_font_size = vd;
        s->default_font_size_set = TRUE;
      }
      continue;
    } else if (!strcmp(k, "font_size")) {
      int v2 = (int)g_ascii_strtoll(vs, NULL, 10);
      if (v2 >= 6 && v2 <= 72)
        s->font_size = v2;
      continue;
    }
    int v = (int)g_ascii_strtoll(vs, NULL, 10);
    gtk_prefs_apply_kv(s, k, v);
  }
  g_strfreev(lines);
  g_free(contents);
}

/* Load prefs: project dir first, then exe dir, then user config. First wins. */
static void gtk_prefs_defaults(AppState *s) {
  /* Set default values before loading prefs, so that if the
     prefs file is absent or the keys are missing, sensible defaults apply. */
  if (!s->src_ext[0])
    g_strlcpy(s->src_ext, ".asm", sizeof(s->src_ext));
  if (!s->bin_ext[0])
    g_strlcpy(s->bin_ext, ".bin", sizeof(s->bin_ext));
  if (!s->font_family[0])
    g_strlcpy(s->font_family, "Monospace", sizeof(s->font_family));
  if (s->font_size <= 0)
    s->font_size = 12;
  /* Default: reopen files from last session.
   * Only set before prefs are loaded so a saved value of 0 is respected. */
  if (!s->_prefs_loaded)
    s->reopen_last_file = TRUE;
}

/* Load prefs in cascade: user first (lowest priority), then exe, then project.
   Each level overrides keys set by the previous, so project-level settings
   win but missing keys fall back gracefully to lower-level files. */
static void gtk_prefs_load(AppState *s) {
  char proj[PATH_MAX] = {0}, exe[PATH_MAX] = {0}, user[PATH_MAX] = {0};
  gtk_prefs_path_project(s, proj, sizeof(proj));
  gtk_prefs_path_exe(exe, sizeof(exe));
  gtk_prefs_path_user(user, sizeof(user));

  /* Load from lowest to highest priority so higher-priority files win */
  if (user[0] && g_file_test(user, G_FILE_TEST_EXISTS))
    gtk_prefs_load_from_file(s, user);
  if (exe[0] && g_file_test(exe, G_FILE_TEST_EXISTS))
    gtk_prefs_load_from_file(s, exe);
  if (proj[0] && g_file_test(proj, G_FILE_TEST_EXISTS))
    gtk_prefs_load_from_file(s, proj);
  s->_prefs_loaded = TRUE;
}

/* Save prefs to user config dir (always). */
static void gtk_prefs_save(AppState *s) {
  char path[PATH_MAX];
  gtk_prefs_path_user(path, sizeof(path));

  /* Ensure directory exists */
  gchar *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);

  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "; 6502caEd preferences (GTK)\n");
  fprintf(f, "dark_mode=%d\n", s->dark_mode ? 1 : 0);
  fprintf(f, "auto_indent=%d\n", s->auto_indent ? 1 : 0);
  fprintf(f, "word_wrap=%d\n", s->word_wrap ? 1 : 0);
  fprintf(f, "reopen_last_file=%d\n", s->reopen_last_file ? 1 : 0);
  fprintf(f, "tab_width=%d\n", s->tab_width);
  fprintf(f, "problems_enabled=%d\n", s->problems_enabled ? 1 : 0);
  fprintf(f, "error_underline=%d\n", s->error_underline ? 1 : 0);
  fprintf(f, "asm_clear_mem=%d\n", s->asm_clear_mem ? 1 : 0);
  fprintf(f, "allow_undoc=%d\n", s->allow_undoc ? 1 : 0);
  fprintf(f, "autocomplete=%d\n", s->autocomplete ? 1 : 0);
  fprintf(f, "fold_comments=%d\n", s->fold_comments ? 1 : 0);
  fprintf(f, "fold_macros=%d\n", s->fold_macros ? 1 : 0);
  fprintf(f, "keep_comment_aligned=%d\n", s->keep_comment_aligned ? 1 : 0);
  fprintf(f, "comment_align_col=%d\n", s->comment_align_col);
  fprintf(f, "comment_use_slashslash=%d\n", s->comment_use_slashslash ? 1 : 0);
  fprintf(f, "src_ext=%s\n", s->src_ext[0] ? s->src_ext : ".asm");
  fprintf(f, "bin_ext=%s\n", s->bin_ext[0] ? s->bin_ext : ".bin");
  fprintf(f, "font_family=%s\n",
          s->font_family[0] ? s->font_family : "Monospace");
  fprintf(f, "font_size=%d\n", s->font_size > 0 ? s->font_size : 12);
  /* Only persist default_font_size if explicitly set via Font dialog */
  if (s->default_font_size_set && s->default_font_size > 0)
    fprintf(f, "default_font_size=%d\n", s->default_font_size);
  if (s->win) {
    int w = gtk_widget_get_width(GTK_WIDGET(s->win));
    int h = gtk_widget_get_height(GTK_WIDGET(s->win));
    if (w > 0 && h > 0) {
      fprintf(f, "win_width=%d\n", w);
      fprintf(f, "win_height=%d\n", h);
    }
  }
  fclose(f);
}

// Include manager colors.
// Requested: normal files cyan, opened files green (not neon), missing red.
static const char *it_color_file = "#00c8ff";    // cyan
static const char *it_color_open = "#00c000";    // green (not neon)
static const char *it_color_missing = "#ff4040"; // soft-ish red

/* Legacy GtkTreeStore columns (kept while migrating; no longer used by the
   Includes Manager UI after the GtkListView migration). */
enum { IT_COL_TEXT = 0, IT_COL_PATH = 1, IT_COL_COLOR = 2, IT_COL_N };

// ---- GTK4-native Includes Manager model (GtkListView + GtkTreeListModel) ----

typedef struct _IncludeNode IncludeNode;
typedef struct _IncludeNodeClass IncludeNodeClass;

struct _IncludeNode {
  GObject parent_instance;
  char *text;
  char *path; // canonical full path when possible
  gboolean missing;
  gboolean open;
  GListStore *children; // IncludeNode
};

struct _IncludeNodeClass {
  GObjectClass parent_class;
};

G_DEFINE_TYPE(IncludeNode, include_node, G_TYPE_OBJECT)

enum { PROP_0, PROP_TEXT, PROP_PATH, PROP_MISSING, PROP_OPEN, N_PROPS };

static GParamSpec *include_node_props[N_PROPS];

static void include_node_finalize(GObject *o) {
  IncludeNode *n = (IncludeNode *)o;
  g_clear_pointer(&n->text, g_free);
  g_clear_pointer(&n->path, g_free);
  g_clear_object(&n->children);
  G_OBJECT_CLASS(include_node_parent_class)->finalize(o);
}

/* GObject property getter for IncludeNode (name and path properties). */
static void include_node_get_property(GObject *o, guint prop_id, GValue *v,
                                      GParamSpec *pspec) {
  (void)pspec;
  IncludeNode *n = (IncludeNode *)o;
  switch (prop_id) {
  case PROP_TEXT:
    g_value_set_string(v, n->text);
    break;
  case PROP_PATH:
    g_value_set_string(v, n->path);
    break;
  case PROP_MISSING:
    g_value_set_boolean(v, n->missing);
    break;
  case PROP_OPEN:
    g_value_set_boolean(v, n->open);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(o, prop_id, pspec);
    break;
  }
}

/* GObject property setter for IncludeNode. */
static void include_node_set_property(GObject *o, guint prop_id,
                                      const GValue *v, GParamSpec *pspec) {
  (void)pspec;
  IncludeNode *n = (IncludeNode *)o;
  switch (prop_id) {
  case PROP_TEXT:
    g_free(n->text);
    n->text = g_value_dup_string(v);
    break;
  case PROP_PATH:
    g_free(n->path);
    n->path = g_value_dup_string(v);
    break;
  case PROP_MISSING:
    n->missing = g_value_get_boolean(v);
    break;
  case PROP_OPEN:
    n->open = g_value_get_boolean(v);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(o, prop_id, pspec);
    break;
  }
}

/* GObject class initialiser for IncludeNode: registers finalize and properties.
 */
static void include_node_class_init(IncludeNodeClass *c) {
  GObjectClass *oc = G_OBJECT_CLASS(c);
  oc->finalize = include_node_finalize;
  oc->get_property = include_node_get_property;
  oc->set_property = include_node_set_property;

  include_node_props[PROP_TEXT] = g_param_spec_string(
      "text", "text", "Display text", NULL, G_PARAM_READWRITE);
  include_node_props[PROP_PATH] =
      g_param_spec_string("path", "path", "Full path", NULL, G_PARAM_READWRITE);
  include_node_props[PROP_MISSING] = g_param_spec_boolean(
      "missing", "missing", "Missing file", FALSE, G_PARAM_READWRITE);
  include_node_props[PROP_OPEN] = g_param_spec_boolean(
      "open", "open", "Open in editor", FALSE, G_PARAM_READWRITE);

  g_object_class_install_properties(oc, N_PROPS, include_node_props);
}

/* Instance initialiser for IncludeNode: creates the children list store. */
static void include_node_init(IncludeNode *n) {
  n->children = g_list_store_new(include_node_get_type());
}

static IncludeNode *include_node_new(const char *text, const char *path,
                                     gboolean missing, gboolean open) {
  IncludeNode *n = g_object_new(include_node_get_type(), NULL);
  n->text = g_strdup(text ? text : "(unknown)");
  n->path = g_strdup(path ? path : "");
  n->missing = missing;
  n->open = open;
  return n;
}

/* Append child to parent's children list and set child's parent pointer. */
static void include_node_add_child(IncludeNode *parent, IncludeNode *child) {
  if (!parent || !child)
    return;
  g_list_store_append(parent->children, child);
}

static GListModel *include_node_children_model(gpointer item,
                                               gpointer user_data) {
  (void)user_data;

  /* GtkTreeListModel may call this func with either:
     - the underlying item (IncludeNode), or
     - a GtkTreeListRow (notably when passthrough == FALSE).
     Be defensive to avoid crashes across GTK4 versions/builds.
  */

  if (!item)
    return NULL;

  IncludeNode *n = NULL;
  if (GTK_IS_TREE_LIST_ROW(item)) {
    gpointer inner = gtk_tree_list_row_get_item(GTK_TREE_LIST_ROW(item));
    if (inner && G_IS_OBJECT(inner) &&
        G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)inner,
                                   include_node_get_type())) {
      n = (IncludeNode *)inner;
    }
  } else if (G_IS_OBJECT(item) &&
             G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)item,
                                        include_node_get_type())) {
    n = (IncludeNode *)item;
  }

  if (!n || !n->children)
    return NULL;
  // Return a ref; TreeListModel will own it.
  return G_LIST_MODEL(g_object_ref(n->children));
}

static int app_notebook_page_for_path(AppState *s, const char *path);
static bool app_doc_is_open(AppState *s, const char *path);
static EditorDoc *app_doc_new(AppState *s, const char *path);
static void load_file_into_doc(AppState *s, EditorDoc *d, const char *path);
static void app_add_doc_tab(AppState *s, EditorDoc *d, const char *label);
static void show_error(AppState *s, const char *msg);

// Needed by include-tree double-click open (defined later in this file)
static void app_add_doc_tab(AppState *s, EditorDoc *d, const char *label);

// Free IncludeTreeCtx after GTK has finished tearing down the dialog/treeview.
// (Avoids a use-after-free during widget destruction and signal emission.)
static void include_tree_ctx_free(gpointer p);

static gboolean include_tree_free_ctx_idle(gpointer data) {
  g_free(data);
  return G_SOURCE_REMOVE;
}

static gboolean include_tree_ctx_free_idle(gpointer data) {
  include_tree_ctx_free(data);
  return G_SOURCE_REMOVE;
}

/* Whitespace including newlines, for include-tree path scanning. */
static gboolean it_is_ws(char c) {
  return is_ws(c) || c == '\n' || c == '\f' || c == '\v';
}

static char *it_trim_and_dup(const char *s) {
  if (!s)
    return NULL;
  while (*s && it_is_ws(*s))
    s++;
  size_t n = strlen(s);
  while (n > 0 && it_is_ws(s[n - 1]))
    n--;
  char *out = (char *)g_malloc(n + 1);
  memcpy(out, s, n);
  out[n] = 0;
  return out;
}

static gboolean it_path_is_abs(const char *p) {
  if (!p || !*p)
    return FALSE;
  if (p[0] == '/')
    return TRUE;
  // Windows-ish absolute paths are possible in source even on Linux.
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':' && (p[2] == '/' || p[2] == '\\'))
    return TRUE;
  return FALSE;
}

static char *it_dirname_dup(const char *path) {
  if (!path)
    return NULL;
  char *d = g_path_get_dirname(path);
  return d;
}

// Parse a single "path" argument from a directive line. Returns newly allocated
// string or NULL.
static char *it_parse_path_arg_relaxed(const char *line) {
  if (!line)
    return NULL;
  // find first whitespace after directive token
  const char *p = line;
  while (*p && !it_is_ws(*p))
    p++;
  while (*p && it_is_ws(*p))
    p++;
  if (!*p)
    return NULL;

  // strip any trailing comment starting with ';'
  const char *semi = strchr(p, ';');
  size_t len = semi ? (size_t)(semi - p) : strlen(p);
  while (len > 0 && it_is_ws(p[len - 1]))
    len--;
  if (len == 0)
    return NULL;

  // quoted "..." or <...>
  if (p[0] == '"') {
    size_t i = 1;
    while (i < len && p[i] != '"')
      i++;
    if (i <= 1)
      return NULL;
    char *out = (char *)g_malloc(i);
    memcpy(out, p + 1, i - 1);
    out[i - 1] = 0;
    return out;
  }
  if (p[0] == '<') {
    size_t i = 1;
    while (i < len && p[i] != '>')
      i++;
    if (i <= 1)
      return NULL;
    char *out = (char *)g_malloc(i);
    memcpy(out, p + 1, i - 1);
    out[i - 1] = 0;
    return out;
  }

  // bare token until whitespace
  size_t i = 0;
  while (i < len && !it_is_ws(p[i]))
    i++;
  if (i == 0)
    return NULL;
  char *out = (char *)g_malloc(i + 1);
  memcpy(out, p, i);
  out[i] = 0;
  return out;
}

static gboolean it_is_bare_filename(const char *p) {
  if (!p || !*p)
    return FALSE;
  if (strchr(p, '/') || strchr(p, '\\'))
    return FALSE;
  if (it_path_is_abs(p))
    return FALSE;
  return TRUE;
}

static char *it_resolve_include(const char *including_file, const char *raw,
                                const char *includeroot) {
  if (!raw || !*raw)
    return NULL;
  if (it_path_is_abs(raw))
    return g_strdup(raw);

  char *inc_dir = it_dirname_dup(including_file);
  char *try_path = NULL;

  if (it_is_bare_filename(raw) && includeroot && *includeroot) {
    try_path = g_build_filename(includeroot, raw, NULL);
    if (g_file_test(try_path, G_FILE_TEST_EXISTS)) {
      g_free(inc_dir);
      return try_path;
    }
    g_free(try_path);
    try_path = NULL;
  }

  // relative to including file
  if (inc_dir) {
    try_path = g_build_filename(inc_dir, raw, NULL);
    g_free(inc_dir);
    return try_path;
  }
  return g_strdup(raw);
}

static char *it_realpath_dup(const char *path) {
  if (!path)
    return NULL;
  char *rp = realpath(path, NULL);
  if (!rp)
    return g_strdup(path);
  char *out = g_strdup(rp);
  free(rp);
  return out;
}

/* Compare two paths by realpath() when possible, else fallback to strcmp. */
static bool it_path_eq(const char *a, const char *b) {
  if (!a || !b)
    return false;
  char *ra = it_realpath_dup(a);
  char *rb = it_realpath_dup(b);
  bool eq = false;
  if (ra && rb) {
    eq = (strcmp(ra, rb) == 0);
  } else {
    eq = (strcmp(a, b) == 0);
  }
  g_free(ra);
  g_free(rb);
  return eq;
}

static void it_tree_add_file(AppState *s, GtkTreeStore *store,
                             GtkTreeIter *parent, const char *label,
                             const char *fullpath, gboolean missing) {
  GtkTreeIter it;
  gtk_tree_store_append(store, &it, parent);

  const char *path_in = fullpath ? fullpath : "";
  // Canonicalise so open-tab detection is stable (symlinks/relative paths).
  char *canon = (!missing && path_in[0]) ? it_realpath_dup(path_in) : NULL;
  if (!canon)
    canon = g_strdup(path_in);

  bool is_open = false;
  if (!missing && canon[0]) {
    is_open = app_doc_is_open(s, canon);
    // The include tree is invoked from the currently-open root file; ensure the
    // root item (top-level) shows as open when it exists on disk.
    if (!is_open && parent == NULL)
      is_open = true;
  }
  // Colors requested: missing=red-ish, open=green, closed=cyan.
  // (Cycle nodes are handled separately.)
  const char *color =
      missing ? "#ff4040" : (is_open ? it_color_open : it_color_file);

  if (missing) {
    char *txt = g_strdup_printf("%s (missing)", label ? label : "(unknown)");
    gtk_tree_store_set(store, &it, IT_COL_TEXT, txt, IT_COL_PATH, canon,
                       IT_COL_COLOR, color, -1);
    g_free(txt);
  } else {
    gtk_tree_store_set(store, &it, IT_COL_TEXT, label ? label : "(unknown)",
                       IT_COL_PATH, canon, IT_COL_COLOR, color, -1);
  }

  g_free(canon);
}

/* A variant used for circular-reference markers (bright red). */
static void it_tree_add_cycle(AppState *s, GtkTreeStore *store,
                              GtkTreeIter *parent, const char *label,
                              const char *fullpath) {
  GtkTreeIter it;
  gtk_tree_store_append(store, &it, parent);

  const char *path_in = fullpath ? fullpath : "";
  char *canon = (path_in[0]) ? it_realpath_dup(path_in) : NULL;
  if (!canon)
    canon = g_strdup(path_in);

  gtk_tree_store_set(store, &it, IT_COL_TEXT, label ? label : "(cycle)",
                     IT_COL_PATH, canon, IT_COL_COLOR,
                     "#ff0000", /* bright red */
                     -1);
  g_free(canon);
}

static void it_build_tree_rec(AppState *s, GtkTreeStore *store,
                              GtkTreeIter *parent, const char *filepath,
                              const char *rootfile, char **io_includeroot,
                              GHashTable *seen, GHashTable *onstack,
                              int depth) {
  if (!filepath || depth > 64)
    return;
  char *canon = it_realpath_dup(filepath);
  if (g_hash_table_contains(onstack, canon)) {
    // circular reference
    char *base = g_path_get_basename(filepath);
    char *txt = g_strdup_printf("%s (cycle)", base);
    it_tree_add_cycle(s, store, parent, txt, filepath);
    g_free(txt);
    g_free(base);
    g_free(canon);
    return;
  }

  // If we've already expanded this file elsewhere, still show it, but don't
  // recurse again (keeps trees readable and avoids exponential growth).
  const gboolean already_seen = g_hash_table_contains(seen, canon);
  if (!already_seen) {
    g_hash_table_add(seen, canon); // takes ownership
  }

  // Add to recursion stack (use a separate duplicated key if canon ownership
  // went to 'seen').
  char *stack_key = already_seen ? canon : NULL;
  if (already_seen) {
    // canon not owned by seen in this branch; onstack will own it.
    g_hash_table_add(onstack, canon);
  } else {
    // seen owns canon; add a duplicate to onstack.
    stack_key = g_strdup(canon);
    g_hash_table_add(onstack, stack_key);
  }

  FILE *fp = fopen(filepath, "rb");
  if (!fp) {
    char *base = g_path_get_basename(filepath);
    it_tree_add_file(s, store, parent, base, filepath, TRUE);
    g_free(base);
    g_hash_table_remove(onstack, stack_key ? stack_key : canon);
    return;
  }

  // Add this node
  char *base = g_path_get_basename(filepath);
  GtkTreeIter me;
  gtk_tree_store_append(store, &me, parent);
  // Prefer canonical path for matching open tabs.
  const char *store_path = (canon && *canon) ? canon : filepath;
  bool is_open =
      (store_path && *store_path) ? app_doc_is_open(s, store_path) : false;
  // Root item (top-level) is the file we're currently viewing when the dialog
  // is launched, so ensure it shows as open.
  if (parent == NULL)
    is_open = true;
  // Open tabs: use configured green; closed (but found): cyan.
  const char *color = is_open ? it_color_open : it_color_file;
  gtk_tree_store_set(store, &me, IT_COL_TEXT, base, IT_COL_PATH, store_path,
                     IT_COL_COLOR, color, -1);
  g_free(base);

  char line[4096];
  while (!already_seen && fgets(line, sizeof(line), fp)) {
    const char *p = line;
    while (*p && it_is_ws(*p))
      p++;
    if (!*p)
      continue;
    if (*p == ';')
      continue;
    /* Accept both '.' and '#' directive prefixes (.include / #include) */
    if (*p != '.' && *p != '#')
      continue;
    p++;
    // directive name
    const char *name = p;
    while (*p && !it_is_ws(*p))
      p++;
    size_t nlen = (size_t)(p - name);
    if (nlen == 0)
      continue;

    // .includeroot only honoured from the root file (matches assembler
    // behaviour)
    if (nlen == 10 && g_ascii_strncasecmp(name, "includeroot", 10) == 0) {
      if (rootfile && strcmp(filepath, rootfile) == 0) {
        char *arg = it_parse_path_arg_relaxed(line);
        if (arg) {
          char *trim = it_trim_and_dup(arg);
          g_free(arg);
          if (trim && *trim) {
            g_free(*io_includeroot);
            *io_includeroot = trim;
          } else {
            g_free(trim);
          }
        }
      }
      continue;
    }

    if (nlen == 7 && g_ascii_strncasecmp(name, "include", 7) == 0) {
      char *raw = it_parse_path_arg_relaxed(line);
      if (!raw) {
        continue;
      }
      char *resolved = it_resolve_include(filepath, raw, *io_includeroot);
      g_free(raw);
      if (!resolved)
        continue;
      gboolean exists = g_file_test(resolved, G_FILE_TEST_EXISTS);
      if (!exists) {
        char *b = g_path_get_basename(resolved);
        it_tree_add_file(s, store, &me, b, resolved, TRUE);
        g_free(b);
        g_free(resolved);
        continue;
      }
      it_build_tree_rec(s, store, &me, resolved, rootfile, io_includeroot, seen,
                        onstack, depth + 1);
      g_free(resolved);
    }
  }
  fclose(fp);

  g_hash_table_remove(onstack, stack_key ? stack_key : canon);
}

static GtkTreeStore *it_build_tree_store(AppState *s, const char *root_path) {
  GtkTreeStore *store =
      gtk_tree_store_new(IT_COL_N, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  if (!root_path || !*root_path)
    return store;

  char *includeroot = NULL;
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GHashTable *onstack =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  // Canonicalise root so (a) the root gets the correct "open" colour when the
  // same file is open via an equivalent path spelling, and (b) .includeroot
  // matching is stable.
  char *rootcanon = it_realpath_dup(root_path);
  const char *root_use = rootcanon ? rootcanon : root_path;
  it_build_tree_rec(s, store, NULL, root_use, root_use, &includeroot, seen,
                    onstack, 0);
  g_free(rootcanon);

  g_hash_table_destroy(onstack);
  g_hash_table_destroy(seen);
  g_free(includeroot);
  return store;
}

/* -------- Root file detection (heuristic) --------

   The Includes Manager should show the include-tree for the *root* source file
   that leads (directly or indirectly) to the currently-focused document.

   We don't have a full project database here, so we use a practical heuristic:
   - Consider currently-open tabs as the working set.
   - If any open file contains an include that resolves to the current file,
     that open file is treated as a parent.
   - Repeat until no parent is found.

   This matches typical workflows: the root is open (the "main" assembly file),
   and included files are opened via navigation.
*/

static gboolean it_file_includes_target_rec(const char *including_file,
                                            const char *target_canon,
                                            GHashTable *visited, int depth) {
  if (!including_file || !including_file[0] || !target_canon ||
      !target_canon[0])
    return FALSE;
  if (depth > 64)
    return FALSE;

  char *inc_canon = it_realpath_dup(including_file);
  if (!inc_canon)
    inc_canon = g_strdup(including_file);

  // avoid cycles (keyed by canonical path where possible)
  if (g_hash_table_contains(visited, inc_canon)) {
    g_free(inc_canon);
    return FALSE;
  }
  g_hash_table_add(visited, inc_canon); // takes ownership

  FILE *f = fopen(inc_canon, "rb");
  if (!f)
    return FALSE;

  // includeroot defaults to directory of including file; can be overridden
  // within this file via .includeroot (like the assembler).
  char *includeroot = g_path_get_dirname(inc_canon);

  char line[4096];
  gboolean found = FALSE;

  while (fgets(line, (int)sizeof(line), f)) {
    const char *p = line;
    while (*p && it_is_ws(*p))
      p++;
    if (!*p)
      continue;
    if (*p == ';')
      continue;
    /* Accept both '.' and '#' directive prefixes (.include / #include) */
    if (*p != '.' && *p != '#')
      continue;
    p++;

    const char *name = p;
    while (*p && !it_is_ws(*p))
      p++;
    size_t nlen = (size_t)(p - name);
    if (nlen == 0)
      continue;

    if (nlen == 10 && g_ascii_strncasecmp(name, "includeroot", 10) == 0) {
      char *arg = it_parse_path_arg_relaxed(line);
      if (arg) {
        char *trim = it_trim_and_dup(arg);
        g_free(arg);
        if (trim && *trim) {
          g_free(includeroot);
          includeroot = trim;
        } else {
          g_free(trim);
        }
      }
      continue;
    }

    if (nlen == 7 && g_ascii_strncasecmp(name, "include", 7) == 0) {
      char *raw = it_parse_path_arg_relaxed(line);
      if (!raw)
        continue;

      char *resolved = it_resolve_include(inc_canon, raw, includeroot);
      g_free(raw);
      if (!resolved)
        continue;

      // Canonicalise and compare to target
      char *rcanon = it_realpath_dup(resolved);
      if (!rcanon)
        rcanon = g_strdup(resolved);

      if (g_strcmp0(rcanon, target_canon) == 0) {
        found = TRUE;
      } else if (!g_hash_table_contains(visited, rcanon)) {
        found = it_file_includes_target_rec(rcanon, target_canon, visited,
                                            depth + 1);
      }

      g_free(resolved);
      g_free(rcanon);
      if (found)
        break;
    }
  }

  fclose(f);
  g_free(includeroot);
  return found;
}

/* Return TRUE if including_file directly or indirectly includes target_canon.
 * Used to detect which file is the root for the active tab. */
static gboolean it_file_includes_target(const char *including_file,
                                        const char *target_canon) {
  if (!including_file || !including_file[0] || !target_canon ||
      !target_canon[0])
    return FALSE;
  GHashTable *visited =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean ok =
      it_file_includes_target_rec(including_file, target_canon, visited, 0);
  g_hash_table_destroy(visited);
  return ok;
}

static char *it_find_root_for_active(AppState *s, const char *active_path) {
  if (!s || !active_path || !*active_path)
    return NULL;

  char *cur = it_realpath_dup(active_path);
  if (!cur)
    cur = g_strdup(active_path);

  /* Walk upward: first check open tabs, then sibling .asm files on disk.
   * Repeat until no further parent can be found (stable). */
  for (int guard = 0; guard < 64; guard++) {
    char *parent = NULL;

    /* 1. Check open tabs first — exact match preferred. */
    const int n = s->notebook
                      ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(s->notebook))
                      : 0;
    for (int i = 0; i < n && !parent; i++) {
      GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(s->notebook), i);
      if (!page) continue;
      EditorDoc *d = (EditorDoc *)g_object_get_data(G_OBJECT(page), "app-doc");
      const char *dpath = editor_doc_get_path(d);
      if (!dpath || !dpath[0]) continue;
      char *pcanon = it_realpath_dup(dpath);
      if (!pcanon) pcanon = g_strdup(dpath);
      if (!it_path_eq(pcanon, cur) && it_file_includes_target(pcanon, cur))
        parent = pcanon;
      else
        g_free(pcanon);
    }

    /* 2. If no open tab is a parent, scan sibling .asm files on disk. */
    if (!parent) {
      gchar *dir = g_path_get_dirname(cur);
      if (dir && *dir) {
        GDir *gd = g_dir_open(dir, 0, NULL);
        if (gd) {
          const gchar *fname;
          while ((fname = g_dir_read_name(gd)) != NULL && !parent) {
            /* Only consider assembly source files */
            if (!g_str_has_suffix(fname, ".asm") &&
                !g_str_has_suffix(fname, ".ASM") &&
                !g_str_has_suffix(fname, ".s")   &&
                !g_str_has_suffix(fname, ".S"))
              continue;
            gchar *candidate = g_build_filename(dir, fname, NULL);
            char *ccanon = it_realpath_dup(candidate);
            if (!ccanon) ccanon = g_strdup(candidate);
            g_free(candidate);
            if (!it_path_eq(ccanon, cur) && it_file_includes_target(ccanon, cur))
              parent = ccanon;
            else
              g_free(ccanon);
          }
          g_dir_close(gd);
        }
      }
      g_free(dir);
    }

    if (!parent)
      break;

    g_free(cur);
    cur = parent;
  }

  return cur;
}

// Build a GTK4-native tree of IncludeNode objects.
static IncludeNode *it_build_node_rec(AppState *s, const char *filepath,
                                      const char *rootfile,
                                      char **io_includeroot,
                                      GHashTable *visited, int depth) {
  if (!filepath || depth > 64)
    return NULL;

  char *canon = it_realpath_dup(filepath);
  const char *use_path = (canon && *canon) ? canon : filepath;

  // Cycle detection (canonical paths where possible).
  if (canon && g_hash_table_contains(visited, canon)) {
    char *base = g_path_get_basename(use_path);
    char *txt = g_strdup_printf("%s (cycle)", base);
    IncludeNode *n = include_node_new(txt, use_path, FALSE, FALSE);
    g_free(txt);
    g_free(base);
    g_free(canon);
    return n;
  }
  if (canon)
    g_hash_table_add(visited, canon); // takes ownership

  gboolean exists = g_file_test(use_path, G_FILE_TEST_EXISTS);
  if (!exists) {
    char *base = g_path_get_basename(use_path);
    char *txt = g_strdup_printf("%s (missing)", base);
    IncludeNode *n = include_node_new(txt, use_path, TRUE, FALSE);
    g_free(txt);
    g_free(base);
    // If canon was NULL, nothing added to visited.
    if (!canon)
      g_free(canon);
    return n;
  }

  FILE *fp = fopen(use_path, "rb");
  if (!fp) {
    char *base = g_path_get_basename(use_path);
    char *txt = g_strdup_printf("%s (unreadable)", base);
    IncludeNode *n = include_node_new(txt, use_path, FALSE, FALSE);
    g_free(txt);
    g_free(base);
    return n;
  }

  char *base = g_path_get_basename(use_path);
  gboolean is_open = app_doc_is_open(s, use_path);
  if (rootfile && strcmp(use_path, rootfile) == 0)
    is_open = TRUE; // root shows open
  IncludeNode *me = include_node_new(base, use_path, FALSE, is_open);
  g_free(base);

  char line[4096];
  while (fgets(line, sizeof(line), fp)) {
    const char *p = line;
    while (*p && it_is_ws(*p))
      p++;
    if (!*p)
      continue;
    if (*p == ';')
      continue;
    /* Accept both '.' and '#' directive prefixes (.include / #include) */
    if (*p != '.' && *p != '#')
      continue;
    p++;
    const char *name = p;
    while (*p && !it_is_ws(*p))
      p++;
    size_t nlen = (size_t)(p - name);
    if (nlen == 0)
      continue;

    // .includeroot only honoured from the root file
    if (nlen == 10 && g_ascii_strncasecmp(name, "includeroot", 10) == 0) {
      if (rootfile && strcmp(use_path, rootfile) == 0) {
        char *arg = it_parse_path_arg_relaxed(line);
        if (arg) {
          char *trim = it_trim_and_dup(arg);
          g_free(arg);
          if (trim && *trim) {
            g_free(*io_includeroot);
            *io_includeroot = trim;
          } else {
            g_free(trim);
          }
        }
      }
      continue;
    }

    if (nlen == 7 && g_ascii_strncasecmp(name, "include", 7) == 0) {
      char *raw = it_parse_path_arg_relaxed(line);
      if (!raw)
        continue;
      char *resolved = it_resolve_include(use_path, raw, *io_includeroot);
      g_free(raw);
      if (!resolved)
        continue;

      gboolean ex = g_file_test(resolved, G_FILE_TEST_EXISTS);
      if (!ex) {
        char *b = g_path_get_basename(resolved);
        char *txt = g_strdup_printf("%s (missing)", b);
        IncludeNode *kid = include_node_new(txt, resolved, TRUE, FALSE);
        include_node_add_child(me, kid);
        g_object_unref(kid);
        g_free(txt);
        g_free(b);
        g_free(resolved);
        continue;
      }

      IncludeNode *kid = it_build_node_rec(s, resolved, rootfile,
                                           io_includeroot, visited, depth + 1);
      if (kid) {
        include_node_add_child(me, kid);
        g_object_unref(kid);
      }
      g_free(resolved);
    }
  }

  fclose(fp);
  return me;
}

static GListStore *it_build_node_root_store(AppState *s,
                                            const char *root_path) {
  GListStore *root = g_list_store_new(include_node_get_type());
  if (!root_path || !*root_path)
    return root;

  char *includeroot = NULL;
  GHashTable *visited =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char *rootcanon = it_realpath_dup(root_path);
  const char *use_root = rootcanon ? rootcanon : root_path;

  IncludeNode *n =
      it_build_node_rec(s, use_root, use_root, &includeroot, visited, 0);
  if (n) {
    g_list_store_append(root, n);
    g_object_unref(n);
  }

  g_free(rootcanon);
  g_hash_table_destroy(visited);
  g_free(includeroot);
  return root;
}

typedef struct {
  AppState *s;
  GtkWidget *dlg;
  GtkWidget *tv;
  GtkTreeModel *model;
  GtkTreeStore *store;
  char *root_dir;
  guint row_activated_handler_id;
} IncludeTreeCtx;

static const char *it_color_include = "#c0c0c0"; // grey

static void it_mark_open_rec(IncludeTreeCtx *c, GtkTreeIter *parent);

static void it_mark_open_rec(IncludeTreeCtx *c, GtkTreeIter *parent) {
  if (!c || !c->model || !c->store)
    return;

  GtkTreeIter it;
  gboolean ok = gtk_tree_model_iter_children(c->model, &it, parent);
  while (ok) {
    char *fullpath = NULL;
    gtk_tree_model_get(c->model, &it, IT_COL_PATH, &fullpath, -1);
    if (fullpath && *fullpath) {
      if (app_doc_is_open(c->s, fullpath)) {
        gtk_tree_store_set(c->store, &it, IT_COL_COLOR, it_color_open, -1);
        GtkTreePath *p = gtk_tree_model_get_path(c->model, &it);
        if (p) {
          gtk_tree_model_row_changed(GTK_TREE_MODEL(c->store), p, &it);
          gtk_tree_path_free(p);
        }
      }
    }
    g_free(fullpath);

    // Recurse into children.
    it_mark_open_rec(c, &it);
    ok = gtk_tree_model_iter_next(c->model, &it);
  }
}

static void include_tree_ctx_free(gpointer p) {
  IncludeTreeCtx *c = (IncludeTreeCtx *)p;
  if (!c)
    return;

  if (c->store) {
    g_object_unref(c->store);
    c->store = NULL;
  } else if (c->model) {
    g_object_unref(c->model);
  }
  c->model = NULL;
  g_free(c->root_dir);
  g_free(c);
}

/* GtkWidget::destroy uses signature (GtkWidget*, gpointer). We keep a wrapper
   so we don't accidentally treat the GtkWidget* as our IncludeTreeCtx (crash).
 */
static void include_tree_on_destroy(GtkWidget *w, gpointer user_data) {
  (void)w;
  if (!user_data)
    return;
  /* The treeview/dialog are being destroyed; clear our pointers now so the
     later deferred free doesn't accidentally touch a stale instance pointer. */
  IncludeTreeCtx *c = (IncludeTreeCtx *)user_data;
  c->tv = NULL;
  c->dlg = NULL;
  /* Defer freeing until after GTK has finished tearing down the dialog.
     This avoids use-after-free when GTK emits final signals during destroy. */
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, include_tree_ctx_free_idle,
                  user_data, NULL);
}

static void it_row_activated(GtkTreeView *tv, GtkTreePath *path,
                             GtkTreeViewColumn *col, gpointer user_data) {
  (void)tv;
  (void)col;
  IncludeTreeCtx *c = (IncludeTreeCtx *)user_data;
  if (!c || !c->s || !c->model || !path)
    return;

  GtkTreeIter it;
  if (!gtk_tree_model_get_iter(c->model, &it, path))
    return;

  char *fullpath = NULL;
  gtk_tree_model_get(c->model, &it, IT_COL_PATH, &fullpath, -1);
  if (!fullpath || !*fullpath) {
    g_free(fullpath);
    return;
  }

  if (!g_file_test(fullpath, G_FILE_TEST_EXISTS)) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "File not found:\n%s", fullpath);
    show_error(c->s, msg);
    g_free(fullpath);
    return;
  }

  int page = app_notebook_page_for_path(c->s, fullpath);
  if (page >= 0) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(c->s->notebook), page);
    /* Switching tabs via the include manager should trigger checks immediately.
     */
    schedule_highlight(c->s);
  } else {
    EditorDoc *d = app_doc_new(c->s, NULL);
    if (d) {
      load_file_into_doc(c->s, d, fullpath);
      char *base = g_path_get_basename(fullpath);
      app_add_doc_tab(c->s, d, base ? base : fullpath);
      if (base)
        g_free(base);
      int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(c->s->notebook)) - 1;
      if (last >= 0)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(c->s->notebook), last);
      /* Newly opened includes should be highlighted/checked right away. */
      schedule_highlight(c->s);
    }
  }

  if (c->store)
    gtk_tree_store_set(c->store, &it, IT_COL_COLOR, it_color_open, -1);
  /* In GTK4's deprecated TreeView stack, some backends/themes don't always
     repaint immediately on store updates. Force a row-changed notify and a
     redraw so the colour update is visible right away. */
  if (c->store) {
    gtk_tree_model_row_changed(GTK_TREE_MODEL(c->store), path, &it);
    gtk_widget_queue_draw(c->tv);
  }
  g_free(fullpath);
}

/* GTK4 + deprecated GtkTreeView:
   We update the cursor/selection on click so that activation (double-click,
   Enter key, etc.) uses the expected row. We **do not** open files from this
   handler, because GtkTreeView will emit "row-activated" on double-click.
   Calling it manually here would cause a double-open (two tabs). */
static void it_tree_gesture_pressed(GtkGestureClick *gesture, int n_press,
                                    double x, double y, gpointer user_data) {
  IncludeTreeCtx *c = (IncludeTreeCtx *)user_data;
  if (!c || !c->tv)
    return;

  const int btn =
      gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  /* Treat btn==0 ("any") as acceptable too. */
  if (btn != 0 && btn != GDK_BUTTON_PRIMARY)
    return;

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *col = NULL;
  if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(c->tv), (gint)x, (gint)y,
                                     &path, &col, NULL, NULL)) {
    return;
  }

  /* Ensure selection/cursor follows the click. */
  gtk_tree_view_set_cursor(GTK_TREE_VIEW(c->tv), path, NULL, FALSE);
  (void)n_press; /* handled by GtkTreeView's "row-activated" */

  gtk_tree_path_free(path);
}

// ---- GTK4-native Includes Manager view helpers ----

typedef struct {
  AppState *s;
  GtkSingleSelection *sel; // owns ref
} IncludeMgrCtx;

static void include_mgr_ctx_free(gpointer p) {
  IncludeMgrCtx *c = (IncludeMgrCtx *)p;
  if (!c)
    return;
  g_clear_object(&c->sel);
  g_free(c);
}

static void it_css_ensure_installed(void) {
  static gboolean installed = FALSE;
  if (installed)
    return;
  installed = TRUE;

  GtkCssProvider *prov = gtk_css_provider_new();
  const char *css =
      "label.it-file { color: "
      "#00c8ff" // cyan
      "; }"
      "label.it-open { color: "
      "#00c000" // green
      "; }"
      "label.it-missing { color: "
      "#ff4040" // red
      "; }"
      /* Keep our colours even when the row is selected. */
      "listview row:selected label.it-file { color: #00c8ff; }"
      "listview row:selected label.it-open { color: #00c000; }"
      "listview row:selected label.it-missing { color: #ff4040; }";
  gtk_css_provider_load_from_data(prov, css, -1);

  GdkDisplay *disp = gdk_display_get_default();
  if (disp) {
    gtk_style_context_add_provider_for_display(
        disp, GTK_STYLE_PROVIDER(prov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  g_object_unref(prov);
}

static void it_label_apply_classes(GtkWidget *lbl, IncludeNode *node) {
  if (!lbl || !node)
    return;
  gtk_widget_remove_css_class(lbl, "it-file");
  gtk_widget_remove_css_class(lbl, "it-open");
  gtk_widget_remove_css_class(lbl, "it-missing");
  if (node->missing) {
    gtk_widget_add_css_class(lbl, "it-missing");
  } else if (node->open) {
    gtk_widget_add_css_class(lbl, "it-open");
  } else {
    gtk_widget_add_css_class(lbl, "it-file");
  }
}

/* Signal handler: update list-item label style when a node's state changes. */
static void it_node_notify_style(GObject *obj, GParamSpec *pspec,
                                 gpointer user_data) {
  (void)pspec;
  GtkWidget *lbl = (GtkWidget *)user_data;
  if (!lbl)
    return;
  if (!obj || !G_IS_OBJECT(obj))
    return;
  if (!G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)obj,
                                  include_node_get_type()))
    return;
  it_label_apply_classes(lbl, (IncludeNode *)obj);
}

/* GtkListItemFactory setup: create the label widget for each include-tree row.
 */
static void it_factory_setup(GtkSignalListItemFactory *factory,
                             GtkListItem *item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  GtkWidget *expander = gtk_tree_expander_new();
  gtk_widget_set_hexpand(expander, TRUE);
  gtk_widget_set_margin_start(expander, 6);
  gtk_widget_set_margin_end(expander, 6);
  gtk_widget_set_margin_top(expander, 2);
  gtk_widget_set_margin_bottom(expander, 2);

  GtkWidget *lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(lbl, TRUE);
  gtk_tree_expander_set_child(GTK_TREE_EXPANDER(expander), lbl);

  // Remember label for bind.
  g_object_set_data(G_OBJECT(item), "it-label", lbl);
  g_object_set_data(G_OBJECT(item), "it-expander", expander);

  gtk_list_item_set_child(item, expander);
}

static void it_factory_bind(GtkSignalListItemFactory *factory,
                            GtkListItem *item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  gpointer it_item = gtk_list_item_get_item(item);
  if (!it_item)
    return;

  GtkTreeListRow *row = NULL;
  IncludeNode *node = NULL;

  if (GTK_IS_TREE_LIST_ROW(it_item)) {
    row = GTK_TREE_LIST_ROW(it_item);
    gpointer inner = gtk_tree_list_row_get_item(row);
    if (inner && G_IS_OBJECT(inner) &&
        G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)inner,
                                   include_node_get_type())) {
      node = (IncludeNode *)inner;
    } else {
      node = NULL;
    }
  } else if (it_item && G_IS_OBJECT(it_item) &&
             G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)it_item,
                                        include_node_get_type())) {
    /* Some GTK builds may hand us the underlying item (passthrough behavior).
     */
    node = (IncludeNode *)it_item;
  } else {
    return;
  }

  if (!node)
    return;

  GtkWidget *lbl = (GtkWidget *)g_object_get_data(G_OBJECT(item), "it-label");
  GtkWidget *expander =
      (GtkWidget *)g_object_get_data(G_OBJECT(item), "it-expander");
  if (!lbl || !expander)
    return;

  if (row)
    gtk_tree_expander_set_list_row(GTK_TREE_EXPANDER(expander), row);
  gtk_label_set_text(GTK_LABEL(lbl), node->text ? node->text : "(unknown)");

  /* Apply colour classes now... */
  it_label_apply_classes(lbl, node);

  /* ...and keep them updated when node->open changes (opening from the include
     manager). GtkListItemFactory "bind" is not re-run on property notification,
     so we must listen. */
  g_object_set_data_full(G_OBJECT(item), "it-node", g_object_ref(node),
                         g_object_unref);
  gulong hid_open = g_signal_connect(node, "notify::open",
                                     G_CALLBACK(it_node_notify_style), lbl);
  gulong hid_miss = g_signal_connect(node, "notify::missing",
                                     G_CALLBACK(it_node_notify_style), lbl);
  g_object_set_data(G_OBJECT(item), "it-hid-open",
                    GUINT_TO_POINTER((guint)hid_open));
  g_object_set_data(G_OBJECT(item), "it-hid-miss",
                    GUINT_TO_POINTER((guint)hid_miss));
}

static void it_factory_unbind(GtkSignalListItemFactory *factory,
                              GtkListItem *item, gpointer user_data) {
  (void)factory;
  (void)user_data;

  IncludeNode *node =
      (IncludeNode *)g_object_get_data(G_OBJECT(item), "it-node");
  if (!node)
    return;

  guint hid_open =
      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "it-hid-open"));
  guint hid_miss =
      GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item), "it-hid-miss"));
  if (hid_open)
    g_signal_handler_disconnect(node, hid_open);
  if (hid_miss)
    g_signal_handler_disconnect(node, hid_miss);

  g_object_set_data(G_OBJECT(item), "it-hid-open", NULL);
  g_object_set_data(G_OBJECT(item), "it-hid-miss", NULL);
  g_object_set_data(G_OBJECT(item), "it-node", NULL);
}

/* Callback: user double-clicked an include-tree row; open that file. */
static void it_list_activate(GtkListView *lv, guint position,
                             gpointer user_data) {
  (void)lv;
  IncludeMgrCtx *c = (IncludeMgrCtx *)user_data;
  if (!c || !c->s || !c->sel)
    return;

  // GtkSelectionModel implements GListModel. Use g_list_model_get_item() for
  // compatibility with GTK4 builds that may not export
  // gtk_selection_model_get_item().
  GObject *obj = g_list_model_get_item(G_LIST_MODEL(c->sel), position);
  if (!obj)
    return;

  GtkTreeListRow *row = NULL;
  IncludeNode *node = NULL;

  if (GTK_IS_TREE_LIST_ROW(obj)) {
    row = GTK_TREE_LIST_ROW(obj);
    gpointer inner = gtk_tree_list_row_get_item(row);
    if (inner && G_IS_OBJECT(inner) &&
        G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)inner,
                                   include_node_get_type())) {
      node = (IncludeNode *)inner;
    } else {
      node = NULL;
    }
  } else if (obj && G_IS_OBJECT(obj) &&
             G_TYPE_CHECK_INSTANCE_TYPE((GTypeInstance *)obj,
                                        include_node_get_type())) {
    node = (IncludeNode *)obj;
  } else {
    g_object_unref(obj);
    return;
  }
  if (!node || !node->path || !node->path[0]) {
    g_object_unref(obj);
    return;
  }

  if (node->missing || !g_file_test(node->path, G_FILE_TEST_EXISTS)) {
    char msg[1024];
    snprintf(msg, sizeof(msg), "File not found:\n%s", node->path);
    show_error(c->s, msg);
    g_object_unref(obj);
    return;
  }

  int page = app_notebook_page_for_path(c->s, node->path);
  if (page >= 0) {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(c->s->notebook), page);
    schedule_highlight(c->s);
  } else {
    EditorDoc *d = app_doc_new(c->s, NULL);
    if (d) {
      load_file_into_doc(c->s, d, node->path);
      char *base = g_path_get_basename(node->path);
      app_add_doc_tab(c->s, d, base ? base : node->path);
      if (base)
        g_free(base);
      int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(c->s->notebook)) - 1;
      if (last >= 0)
        gtk_notebook_set_current_page(GTK_NOTEBOOK(c->s->notebook), last);
      schedule_highlight(c->s);
    }
  }

  // Mark node as open and notify so the row recolours.
  if (!node->open) {
    node->open = TRUE;
    g_object_notify_by_pspec(G_OBJECT(node), include_node_props[PROP_OPEN]);
  }

  g_object_unref(obj);
}

static void action_include_tree(GSimpleAction *a, GVariant *p,
                                gpointer user_data);

/* ---------------- Tabbed documents ---------------- */
typedef struct EditorDoc {
  GtkSourceBuffer *buf;
  GtkSourceView *view;
  GtkWidget *scroller; /* widget packed into notebook page */

  char *path;
  gboolean dirty;

  /* Per-doc highlight tags (belong to doc->buf's tag table) */
  GtkTextTag *tag_instr;
  GtkTextTag *tag_dir;
  GtkTextTag *tag_data;
  GtkTextTag *tag_num;
  GtkTextTag *tag_label;
  GtkTextTag *tag_addr_hex;
  GtkTextTag *tag_addr_dec;
  GtkTextTag *tag_addr_bin;
  GtkTextTag *tag_imm_hex;
  GtkTextTag *tag_imm_dec;
  GtkTextTag *tag_imm_bin;
  GtkTextTag *tag_imm_char;
  GtkTextTag *tag_dv_hex;
  GtkTextTag *tag_dv_dec;
  GtkTextTag *tag_dv_bin;
  GtkTextTag *tag_dv_str;
  GtkTextTag *tag_hover_def;
  GtkTextTag *tag_reg;
  GtkTextTag *tag_comment;
  GtkTextTag *tag_string;
  GtkTextTag *tag_error;
  GtkTextTag *tag_inactive;
  GtkTextTag *tag_fold_hidden;
  GtkTextTag *tag_fold_marker;
  GtkTextTag *tag_curline;
  int curline_applied;
  guint hl_idle_id;

  /* Macro definition set — names defined via .macro / macro keyword.
     Key: lowercase macro name, Value: ignored (g_strdup("")) */
  GHashTable *macro_defs;

  /* Label definition index for hover-to-definition highlighting.
     Key: label name (with/without leading '.')
     Value: malloc'd int[2] => {start_offset, end_offset} */
  GHashTable *label_defs;
  /* Case-insensitive helper index for tooltip hints.
     Key: lowercase label name
     Value: canonical label spelling as it appears in the file */
  GHashTable *label_defs_ci;
  int hover_start;
  int hover_end;

  /* Diagnostics + memory image */
  diags_t sem_diags;
  diags_t asm_diags;

  guint8 *mem_image; /* malloc 64KB */
  gboolean mem_valid;
  gboolean mem_dirty;
  asm_cpu_t mem_cpu;
  gpointer mem_view; /* (MemViewState*) window for this doc, if any */
  guint16 mem_asm_start;
  gboolean mem_asm_valid;
  guint16 mem_last_addr;
  gboolean mem_last_addr_valid;
} EditorDoc;

// Buffer/document change notifications (per-tab)
static void on_buffer_changed(GtkTextBuffer *tb, gpointer user);
static void on_modified_changed(GObject *obj, GParamSpec *pspec, gpointer user);

static const char *editor_doc_get_path(const EditorDoc *d) {
  return d ? d->path : NULL;
}

/* Helpers that need EditorDoc fully defined */
static int app_notebook_page_for_path(AppState *s, const char *path) {
  if (!s || !path || !*path)
    return -1;

  // Canonicalise for stable matching (symlinks, relative paths, etc.)
  char *canon_in = it_realpath_dup(path);
  if (!canon_in)
    canon_in = g_strdup(path);

  const int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s->notebook));
  for (int i = 0; i < n; i++) {
    GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(s->notebook), i);
    if (!page)
      continue;
    EditorDoc *d = (EditorDoc *)g_object_get_data(G_OBJECT(page), "app-doc");
    if (!d || !d->path || !*d->path)
      continue;

    char *canon_d = it_realpath_dup(d->path);
    if (!canon_d)
      canon_d = g_strdup(d->path);

    const bool match = (strcmp(canon_d, canon_in) == 0);
    g_free(canon_d);
    if (match) {
      g_free(canon_in);
      return i;
    }
  }

  g_free(canon_in);
  return -1;
}

/* Return true if a tab for path is already open in the notebook. */
static bool app_doc_is_open(AppState *s, const char *path) {
  return app_notebook_page_for_path(s, path) >= 0;
}

static void app_doc_destroy(EditorDoc *d);
static EditorDoc *app_doc_new(AppState *s, const char *path);
static void app_activate_doc(AppState *s, EditorDoc *d);
static void search_context_init(AppState *s); /* forward decl */
static EditorDoc *app_get_active_doc(AppState *s);
static void app_add_doc_tab(AppState *s, EditorDoc *d, const char *label);
static void on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page,
                                    guint page_num, gpointer user_data);
static void tab_plus_clicked(GtkButton *btn, gpointer user_data);
static void tab_close_clicked(GtkButton *btn, gpointer user_data);

/* Forward declarations for editor helpers used by doc creation */
static void create_tags(AppState *s);
static void setup_source_styling(AppState *s);
static void editor_install_ctrl_click(AppState *s);
static void editor_install_nav_keys(AppState *s);
static void editor_install_context_menu(AppState *s);
static void editor_install_hover_tooltips(AppState *s);
// Hover-highlight label definitions when hovering label references (incl. :+ /
// :- branch labels)
static void editor_install_hover_label_defs(EditorDoc *d);
static void doc_rebuild_label_defs(AppState *s, EditorDoc *d);
static int is_instruction_token(const char *t);
static int is_directive_token(const char *t);
static int is_register_token(const char *t);
static int is_number_token(const char *t);
static void editor_install_ctrlk_chords(AppState *s);
static void update_title(AppState *s);

static void install_actions_and_shortcuts(AppState *s);
static void problems_refresh_now(AppState *s);
static void app_state_set_path(AppState *s, const char *path);
static void show_info(AppState *s, const char *msg);
static void show_error(AppState *s, const char *msg);
static const char *path_basename(const char *path);

static const char *path_basename(const char *path) {
  if (!path || !*path)
    return "";
  const char *s1 = strrchr(path, '/');
  const char *s2 = strrchr(path, '\\');
  const char *s = s1;
  if (s2 && (!s || s2 > s))
    s = s2;
  return s ? (s + 1) : path;
}
/* Bookmark forward declarations */
static void gtk_bookmark_redraw(AppState *s);
static int gtk_bookmark_find(AppState *s, int line);
static void gtk_bookmark_toggle(AppState *s, int line);
static int gtk_bookmark_next(AppState *s, int cur_line);
static int gtk_bookmark_prev(AppState *s, int cur_line);
static void gtk_bookmark_jump(AppState *s, int line);
static void gtk_bookmarks_save(AppState *s, const char *file_path);
static void gtk_bookmarks_load(AppState *s, const char *file_path);
static int gtk_get_cursor_line(AppState *s);
static void apply_theme(AppState *s);
static void apply_style_scheme(AppState *s);

static void apply_editor_prefs(AppState *s) {
  if (!s || !s->view)
    return;

  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(s->view), s->word_wrap
                                                          ? GTK_WRAP_WORD_CHAR
                                                          : GTK_WRAP_NONE);

  gtk_source_view_set_auto_indent(s->view, s->auto_indent ? TRUE : FALSE);
  /* Indent when Tab is pressed (common expectation for assembler sources). */
  gtk_source_view_set_indent_on_tab(s->view, s->auto_indent ? TRUE : FALSE);

  if (s->tab_width <= 0)
    s->tab_width = 4;
  gtk_source_view_set_tab_width(s->view, (guint)s->tab_width);

  /* ???Spaces per tab???: make Tab insert spaces rather than '\t'. */
  gtk_source_view_set_insert_spaces_instead_of_tabs(s->view, TRUE);
}

/* Free a document */
static void app_doc_destroy(EditorDoc *d) {
  if (!d)
    return;

  if (d->label_defs) {
    g_hash_table_destroy(d->label_defs);
    d->label_defs = NULL;
  }
  if (d->label_defs_ci) {
    g_hash_table_destroy(d->label_defs_ci);
    d->label_defs_ci = NULL;
  }
  if (d->path)
    g_free(d->path);
  diags_free(&d->sem_diags);
  diags_free(&d->asm_diags);
  if (d->mem_image)
    g_free(d->mem_image);
  /*
   * IMPORTANT (GTK ownership):
   * Widgets are owned by the notebook/container. Unref'ing them here can
   * double-free when GTK later unparents/destroys the page.
   * We only free our non-widget allocations above.
   */
  g_free(d);
}

static void apply_font_css(AppState *s); /* forward decl for scroll cb */

/* Ctrl+scroll wheel adjusts font size */
static gboolean editor_scroll_cb(GtkEventControllerScroll *ctl, double dx,
                                 double dy, gpointer user_data) {
  (void)dx;
  AppState *s = (AppState *)user_data;
  if (!s)
    return FALSE;

  /* GTK4: get modifier state from the underlying GDK event, which is
     more reliable than get_current_event_state for scroll events. */
  GdkEvent *ev =
      gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(ctl));
  GdkModifierType state = ev ? gdk_event_get_modifier_state(ev) : 0;
  if (!(state & GDK_CONTROL_MASK))
    return FALSE; /* let normal scrolling through */

  if (s->font_size <= 0)
    s->font_size = 12;
  if (dy < 0) {
    /* scroll up = zoom in */
    if (s->font_size < 72)
      s->font_size++;
  } else if (dy > 0) {
    /* scroll down = zoom out */
    if (s->font_size > 6)
      s->font_size--;
  }
  apply_font_css(s);
  return TRUE; /* consumed */
}

/* Create a new document (buffer/view/memory/diags) */

/* ── FoldLineNums diagnostic test ─────────────────────────────────────── */
typedef struct {
  GtkSourceGutterRenderer parent;
  AppState *app;
  GtkSourceView *view;
} FoldLineNums;
typedef struct {
  GtkSourceGutterRendererClass parent_class;
} FoldLineNumsClass;
G_DEFINE_TYPE(FoldLineNums, fold_line_nums, GTK_SOURCE_TYPE_GUTTER_RENDERER)

static void fln_snapshot_line(GtkSourceGutterRenderer *renderer,
                              GtkSnapshot *snapshot,
                              GtkSourceGutterLines *lines, guint line) {
  FoldLineNums *self = (FoldLineNums *)renderer;
  AppState *s = self->app;

  /* Skip folded body lines — check fold_map directly */
  if (s && s->fold_map.lines && (size_t)line < s->fold_map.count) {
    fold_line_t *fl = &s->fold_map.lines[line];
    // g_print("FLN line %u: kind=%d collapsed=%d\n", line, (int)fl->kind,
    //         (int)fl->collapsed);
    if (fl->kind == FL_FOLD_BODY || fl->kind == FL_FOLD_END) {
      size_t gi = line;
      while (gi > 0 && s->fold_map.lines[gi].kind != FL_FOLD_START)
        gi--;
      // g_print("FLN  -> group start %zu collapsed=%d\n", gi,
      //         (int)s->fold_map.lines[gi].collapsed);
      if (s->fold_map.lines[gi].collapsed)
        return;
    }
  } else {
    // g_print("FLN line %u: NO MAP (lines=%p count=%zu)\n", line,
    //         (void *)(s ? s->fold_map.lines : NULL), s ? s->fold_map.count :
    //         0);
  }

  /* Get absolute y position within the gutter widget using
  GTK_TEXT_WINDOW_LEFT */
  FoldLineNums *fln = (FoldLineNums *)renderer;
  GtkTextView *tv = GTK_TEXT_VIEW(fln->view);
  GtkTextBuffer *tb = gtk_text_view_get_buffer(tv);
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_line(tb, &it, (gint)line);
  GdkRectangle rect;
  gtk_text_view_get_iter_location(tv, &it, &rect);
  int wy = 0;
  gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_LEFT, 0, rect.y,
                                        NULL, &wy);
  float y = (float)wy;
  float h = rect.height > 0 ? (float)rect.height : 16.0f;
  int width = gtk_widget_get_width(GTK_WIDGET(renderer));

  char num[16];
  snprintf(num, sizeof(num), "%u", line + 1);
  PangoLayout *lo = gtk_widget_create_pango_layout(GTK_WIDGET(renderer), num);
  int tw, th;
  pango_layout_get_pixel_size(lo, &tw, &th);

  GdkRGBA col = {0.5f, 0.56f, 0.63f, 1.0f};

  /* Draw bookmark marker (filled rectangle in accent colour) left of number */
  if (s && gtk_bookmark_find(s, (int)line) >= 0) {
    GdkRGBA bm_col = {0.4f, 0.7f, 1.0f, 1.0f}; /* light blue */
    float bm_w = 6.0f, bm_h = (float)th * 0.65f;
    float bm_x = 2.0f;
    float bm_y = y + (h - bm_h) / 2.0f;
    gtk_snapshot_append_color(snapshot, &bm_col,
                              &GRAPHENE_RECT_INIT(bm_x, bm_y, bm_w, bm_h));
  }

  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot,
                         &GRAPHENE_POINT_INIT((float)(width - tw) - 4.0f,
                                              y + (h - (float)th) / 2.0f));
  gtk_snapshot_append_layout(snapshot, lo, &col);
  gtk_snapshot_restore(snapshot);
  g_object_unref(lo);
}

static void fold_line_nums_init(FoldLineNums *self) { (void)self; }
static void fold_line_nums_class_init(FoldLineNumsClass *klass) {
  GTK_SOURCE_GUTTER_RENDERER_CLASS(klass)->snapshot_line = fln_snapshot_line;
}
static GtkSourceGutterRenderer *fold_line_nums_new(AppState *s,
                                                   GtkSourceView *v) {
  FoldLineNums *r = g_object_new(fold_line_nums_get_type(), NULL);
  r->app = s;
  r->view = v;
  return GTK_SOURCE_GUTTER_RENDERER(r);
}

/* Click callback on the fold gutter: toggle the fold at the clicked line. */
static void fold_gutter_click_cb(GtkGestureClick *gesture, int n_press,
                                 double x, double y, gpointer user_data) {
  (void)n_press;
  AppState *s = (AppState *)user_data;
  if (!s || !s->view || !s->fold_map.lines)
    return;
  gint bx, by;
  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(s->view),
                                        GTK_TEXT_WINDOW_WIDGET, (gint)x,
                                        (gint)y, &bx, &by);
  if (bx >= 0)
    return; /* not in gutter */
  GtkTextIter it;
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &it, 0, by);
  int line = gtk_text_iter_get_line(&it);
  if (line < 0 || (size_t)line >= s->fold_map.count)
    return;
  if (s->fold_map.lines[line].kind != FL_FOLD_START)
    return;
  gtk_fold_toggle_line(s, line);
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ============================================================
 * 6502 Assembly Completion Provider
 * ============================================================ */

/* Fixed completion word lists */
static const char *g_ac_mnemonics[] = {
    "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL", "BRA",
    "BRK", "BRL", "BVC", "BVS", "CLC", "CLD", "CLI", "CLV", "CMP", "CPX", "CPY",
    "DEC", "DEX", "DEY", "EOR", "INC", "INX", "INY", "JML", "JMP", "JSL", "JSR",
    "LDA", "LDX", "LDY", "LSR", "MVN", "MVP", "NOP", "ORA", "PEA", "PEI", "PER",
    "PHA", "PHB", "PHD", "PHK", "PHP", "PHX", "PHY", "PLA", "PLB", "PLD", "PLP",
    "PLX", "PLY", "REP", "ROL", "ROR", "RTI", "RTL", "RTS", "SBC", "SEC", "SED",
    "SEI", "SEP", "STA", "STP", "STX", "STY", "STZ", "TAX", "TAY", "TCD", "TCS",
    "TDC", "TRB", "TSB", "TSC", "TSX", "TXA", "TXS", "TXY", "TYA", "TYX", "WAI",
    "WDM", "XBA", "XCE",
    /* undocumented */
    "AHX", "ALR", "ANC", "ARR", "AXS", "DCP", "ISC", "KIL", "LAS", "LAX", "RLA",
    "RRA", "SAX", "SHX", "SHY", "SLO", "SRE", "TAS", "XAA", NULL};

static const char *g_ac_directives[] = {
    ".align",   ".assert",    ".byte",        ".cpu",    ".def",      ".define",
    ".dword",   ".elif",      ".else",        ".endif",  ".endmacro", ".endm",
    ".endproc", ".endrepeat", ".endscope",    ".error",  ".if",       ".ifdef",
    ".ifndef",  ".include",   ".includeroot", ".labels", ".longa",    ".longi",
    ".halt",    ".unhalt",    ".alert",       ".unalert",
    ".pragma",
    ".macro",   ".opt",       ".org",         ".pc",     ".proc",     ".repeat",
    ".res",     ".scope",     ".warning",     ".word",   "equb",      "equd",
    "equs",     "equw",       "incbin",       "org",     NULL};

/* ---- AsmProposal: implements GtkSourceCompletionProposal ---- */
typedef struct {
  GObject parent;
  char *word; /* heap-owned completion string */
} AsmProposal;
typedef struct {
  GObjectClass parent_class;
} AsmProposalClass;

static GType asm_proposal_get_type(void);
static void
asm_proposal_iface_init(GtkSourceCompletionProposalInterface *iface);

G_DEFINE_TYPE_WITH_CODE(
    AsmProposal, asm_proposal, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GTK_SOURCE_TYPE_COMPLETION_PROPOSAL,
                          asm_proposal_iface_init))

static void asm_proposal_finalize(GObject *obj) {
  AsmProposal *self = (AsmProposal *)obj;
  g_free(self->word);
  G_OBJECT_CLASS(asm_proposal_parent_class)->finalize(obj);
}
static void asm_proposal_init(AsmProposal *self) { (void)self; }
/* GObject class init for AsmProposal (autocomplete suggestion item). */
static void asm_proposal_class_init(AsmProposalClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = asm_proposal_finalize;
}
static char *asm_proposal_get_typed_text(GtkSourceCompletionProposal *prop) {
  return g_strdup(((AsmProposal *)prop)->word);
}
static void
asm_proposal_iface_init(GtkSourceCompletionProposalInterface *iface) {
  iface->get_typed_text = asm_proposal_get_typed_text;
}
static AsmProposal *asm_proposal_new(const char *word) {
  AsmProposal *p = g_object_new(asm_proposal_get_type(), NULL);
  p->word = g_strdup(word);
  return p;
}

typedef struct {
  GObject parent;
  AppState *app;
} AsmCompletionProvider;

typedef struct {
  GObjectClass parent_class;
} AsmCompletionProviderClass;

static GType asm_completion_provider_get_type(void);

/* Forward: GtkSourceCompletionProvider interface implementation */
static void
asm_provider_iface_init(GtkSourceCompletionProviderInterface *iface);

G_DEFINE_TYPE_WITH_CODE(
    AsmCompletionProvider, asm_completion_provider, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GTK_SOURCE_TYPE_COMPLETION_PROVIDER,
                          asm_provider_iface_init))

static void asm_completion_provider_init(AsmCompletionProvider *self) {
  (void)self;
}
static void
asm_completion_provider_class_init(AsmCompletionProviderClass *klass) {
  (void)klass;
}

static char *asm_provider_get_title(GtkSourceCompletionProvider *self) {
  (void)self;
  return g_strdup("6502 Assembly");
}

/* Return the priority of the assembler completion provider. */
static int asm_provider_get_priority(GtkSourceCompletionProvider *self,
                                     GtkSourceCompletionContext *ctx) {
  (void)self;
  (void)ctx;
  return 100;
}

static gboolean asm_provider_is_trigger(GtkSourceCompletionProvider *self,
                                        const GtkTextIter *iter, gunichar ch) {
  (void)self;
  (void)iter;
  gboolean result = g_unichar_isalnum(ch) || ch == '_' || ch == '.';
  return result;
}

/* Return TRUE if pressing this key should activate the selected proposal. */
static gboolean asm_provider_key_activates(GtkSourceCompletionProvider *self,
                                           GtkSourceCompletionContext *ctx,
                                           GtkSourceCompletionProposal *prop,
                                           guint keyval,
                                           GdkModifierType state) {
  (void)self;
  (void)ctx;
  (void)prop;
  (void)state;
  /* Tab accepts; Enter does NOT (falls through to insert newline) */
  return keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab;
}

/* Check if cursor is inside a comment or string — don't complete there */
static gboolean iter_in_comment_or_string(GtkSourceBuffer *buf,
                                          const GtkTextIter *iter) {
  /* Use GtkSourceBuffer context classes */
  return gtk_source_buffer_iter_has_context_class(buf, iter, "comment") ||
         gtk_source_buffer_iter_has_context_class(buf, iter, "string");
}

/* Build a GListStore of AsmProposals matching word */
/* Return TRUE if the cursor is at the first token on the line
   (i.e. nothing but optional whitespace precedes the word being typed).
   If so, mnemonics and directives are appropriate completions.
   If FALSE, only labels/symbols should be offered. */
static gboolean iter_is_first_token(GtkSourceCompletionContext *ctx) {
  GtkTextIter begin, end;
  if (!gtk_source_completion_context_get_bounds(ctx, &begin, &end))
    return TRUE;
  /* Walk backwards from begin to the start of the line */
  GtkTextIter sol = begin;
  gtk_text_iter_set_line_offset(&sol, 0);
  /* Check if everything between sol and begin is whitespace */
  while (!gtk_text_iter_equal(&sol, &begin)) {
    gunichar ch = gtk_text_iter_get_char(&sol);
    if (ch != ' ' && ch != '\t')
      return FALSE; /* something non-whitespace before our word */
    gtk_text_iter_forward_char(&sol);
  }
  return TRUE;
}

static GListStore *asm_build_proposals(AppState *s,
                                       GtkSourceCompletionContext *ctx,
                                       const char *word) {
  GListStore *store = g_list_store_new(asm_proposal_get_type());
  if (!word || strlen(word) < 1)
    return store;

  /* Don't complete in comments or strings */
  if (ctx && s && s->buf) {
    GtkTextIter begin, end;
    gtk_source_completion_context_get_bounds(ctx, &begin, &end);
    if (iter_in_comment_or_string(GTK_SOURCE_BUFFER(s->buf), &end))
      return store;
  }

  char *word_up = g_ascii_strup(word, -1);
  gboolean dot_prefix = (word[0] == '.');

  /* Detect case style of what was typed:
     all-uppercase → offer uppercase; otherwise → offer lowercase */
  gboolean typed_upper = g_ascii_isupper(word[0]);

  /* Only offer mnemonics/directives at the first token on the line */
  gboolean first_token = ctx ? iter_is_first_token(ctx) : TRUE;

  if (first_token && !dot_prefix) {
    for (int i = 0; g_ac_mnemonics[i]; i++) {
      if (g_str_has_prefix(g_ac_mnemonics[i], word_up)) {
        /* Offer in the same case as typed */
        char *offer = typed_upper ? g_ascii_strup(g_ac_mnemonics[i], -1)
                                  : g_ascii_strdown(g_ac_mnemonics[i], -1);
        AsmProposal *obj = asm_proposal_new(offer);
        g_free(offer);
        g_list_store_append(store, obj);
        g_object_unref(obj);
      }
    }
  }

  if (first_token) {
    char *word_lo = g_ascii_strdown(word, -1);
    for (int i = 0; g_ac_directives[i]; i++) {
      if (g_str_has_prefix(g_ac_directives[i], word_lo)) {
        AsmProposal *obj = asm_proposal_new(g_ac_directives[i]);
        g_list_store_append(store, obj);
        g_object_unref(obj);
      }
    }
    g_free(word_lo);
  }

  /* Labels / macros — drawn from label_defs (maintained by the highlight pass)
   * so we only offer actual symbol definitions, not every word in the document.
   * We also scan all open documents so labels from other tabs are offered. */
  if (s && !dot_prefix) {
    /* Build a dedup set from proposals already in the store */
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    guint already_n = g_list_model_get_n_items(G_LIST_MODEL(store));
    for (guint j = 0; j < already_n; j++) {
      AsmProposal *ex =
          (AsmProposal *)g_list_model_get_item(G_LIST_MODEL(store), j);
      g_hash_table_add(seen, (gpointer)ex->word);
      g_object_unref(ex);
    }

    /* Iterate all open documents */
    if (s->docs) {
      for (guint di = 0; di < s->docs->len; di++) {
        EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, di);
        if (!d || !d->label_defs)
          continue;
        GHashTableIter hit;
        gpointer key;
        g_hash_table_iter_init(&hit, d->label_defs);
        while (g_hash_table_iter_next(&hit, &key, NULL)) {
          const char *label = (const char *)key;
          if (!label || strlen(label) < (size_t)strlen(word))
            continue;
          if (g_ascii_strcasecmp(label, word) == 0)
            continue;
          char *label_up = g_ascii_strup(label, -1);
          if (g_str_has_prefix(label_up, word_up) &&
              !g_hash_table_contains(seen, label)) {
            AsmProposal *obj = asm_proposal_new(label);
            g_list_store_append(store, obj);
            g_hash_table_add(seen, (gpointer)label);
            g_object_unref(obj);
          }
          g_free(label_up);
        }
      }
    }
    g_hash_table_destroy(seen);
  }

  g_free(word_up);
  return store;
}

/* Asynchronously populate the completion list for the current context. */
static void asm_provider_populate_async(GtkSourceCompletionProvider *self,
                                        GtkSourceCompletionContext *ctx,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  (void)cancellable;
  AsmCompletionProvider *provider = (AsmCompletionProvider *)self;
  GTask *task = g_task_new(self, cancellable, callback, user_data);

  char *word = gtk_source_completion_context_get_word(ctx);
  if (!word || strlen(word) == 0) {
    g_free(word);
    GtkTextIter begin, end;
    if (gtk_source_completion_context_get_bounds(ctx, &begin, &end))
      word = gtk_text_iter_get_text(&begin, &end);
    else
      word = NULL;
  }

  /* Empty word — return error so the framework creates NO popup.
     Returning an empty list creates a zero-height popup that gets
     reused with stale geometry on the next real trigger. */
  if (!word || strlen(word) == 0) {
    g_free(word);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "no word");
    g_object_unref(task);
    return;
  }

  GListStore *store = asm_build_proposals(provider->app, ctx, word);
  g_free(word);

  /* Force GtkSourceView to re-measure the popup by cycling page_size.
     Without this, the popup reuses stale geometry from the previous session. */
  if (provider->app && provider->app->view) {
    GtkSourceCompletion *compl =
        gtk_source_view_get_completion(provider->app->view);
    gtk_source_completion_set_page_size(compl, 1);
    gtk_source_completion_set_page_size(compl, 5);
  }

  g_task_return_pointer(task, store, g_object_unref);
  g_object_unref(task);
}

static GListModel *
asm_provider_populate_finish(GtkSourceCompletionProvider *self,
                             GAsyncResult *result, GError **error) {
  (void)self;
  GListModel *model =
      G_LIST_MODEL(g_task_propagate_pointer(G_TASK(result), error));
  /* Clear error from cancelled/empty-word tasks so framework
     doesn't log it as an unexpected failure */
  if (!model && error && *error &&
      g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_clear_error(error);
  }
  return model;
}

static void asm_provider_refilter(GtkSourceCompletionProvider *self,
                                  GtkSourceCompletionContext *ctx,
                                  GListModel *model) {
  (void)model;
  AsmCompletionProvider *provider = (AsmCompletionProvider *)self;
  char *word = gtk_source_completion_context_get_word(ctx);
  if (!word || strlen(word) == 0) {
    g_free(word);
    GtkTextIter begin, end;
    if (gtk_source_completion_context_get_bounds(ctx, &begin, &end))
      word = gtk_text_iter_get_text(&begin, &end);
    else
      word = NULL;
  }
  if (!word || strlen(word) == 0) {
    g_free(word);
    return;
  }
  AppState *s = provider->app;

  GListStore *store = asm_build_proposals(s, ctx, word);
  g_free(word);
  gtk_source_completion_context_set_proposals_for_provider(ctx, self,
                                                           G_LIST_MODEL(store));
  g_object_unref(store);
}

/* Set the display text for a completion proposal cell. */
static void asm_provider_display(GtkSourceCompletionProvider *self,
                                 GtkSourceCompletionContext *ctx,
                                 GtkSourceCompletionProposal *proposal,
                                 GtkSourceCompletionCell *cell) {
  (void)self;
  (void)ctx;
  GtkSourceCompletionColumn col = gtk_source_completion_cell_get_column(cell);
  if (col == GTK_SOURCE_COMPLETION_COLUMN_TYPED_TEXT) {
    const char *text = ((AsmProposal *)proposal)->word;
    gtk_source_completion_cell_set_text(cell, text);
  }
}

/* Insert the selected completion proposal into the document. */
static void asm_provider_activate(GtkSourceCompletionProvider *self,
                                  GtkSourceCompletionContext *ctx,
                                  GtkSourceCompletionProposal *proposal) {
  (void)self;
  const char *text = ((AsmProposal *)proposal)->word;

  GtkTextIter begin, end;
  if (!gtk_source_completion_context_get_bounds(ctx, &begin, &end))
    return;

  GtkSourceCompletion *completion =
      gtk_source_completion_context_get_completion(ctx);
  GtkSourceView *view = gtk_source_completion_get_view(completion);
  GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));

  /* Left-gravity mark stays at insert point after the delete */
  GtkTextMark *mark = gtk_text_buffer_create_mark(tb, NULL, &begin, TRUE);
  gtk_text_buffer_begin_user_action(tb);
  gtk_text_buffer_delete(tb, &begin, &end);
  GtkTextIter ins;
  gtk_text_buffer_get_iter_at_mark(tb, &ins, mark);
  gtk_text_buffer_insert(tb, &ins, text, -1);
  gtk_text_buffer_end_user_action(tb);
  gtk_text_buffer_delete_mark(tb, mark);
}

static void
asm_provider_iface_init(GtkSourceCompletionProviderInterface *iface) {
  iface->get_title = asm_provider_get_title;
  iface->get_priority = asm_provider_get_priority;
  iface->is_trigger = asm_provider_is_trigger;
  iface->key_activates = asm_provider_key_activates;
  iface->populate_async = asm_provider_populate_async;
  iface->populate_finish = asm_provider_populate_finish;
  iface->refilter = asm_provider_refilter;
  iface->display = asm_provider_display;
  iface->activate = asm_provider_activate;
}

static GtkSourceCompletionProvider *asm_completion_provider_new(AppState *s) {
  AsmCompletionProvider *p =
      g_object_new(asm_completion_provider_get_type(), NULL);
  p->app = s;
  return GTK_SOURCE_COMPLETION_PROVIDER(p);
}

/* Install / remove provider based on autocomplete pref */
static void editor_completion_apply(AppState *s) {
  if (!s || !s->view)
    return;
  GtkSourceCompletion *c =
      gtk_source_view_get_completion(GTK_SOURCE_VIEW(s->view));
  if (s->autocomplete)
    gtk_source_completion_unblock_interactive(c);
  else
    gtk_source_completion_block_interactive(c);
}

static void fold_gutter_install(AppState *s) {
  if (!s || !s->view)
    return;
  /* Install diagnostic FoldLineNums renderer */
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(GTK_SOURCE_VIEW(s->view),
                                                       GTK_TEXT_WINDOW_LEFT);
  if (gutter) {
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(s->view), FALSE);
    GtkSourceGutterRenderer *ln =
        fold_line_nums_new(s, GTK_SOURCE_VIEW(s->view));
    gtk_widget_set_size_request(GTK_WIDGET(ln), 44, -1);
    gtk_source_gutter_insert(gutter, ln, 1);
    /* Keep a ref so we can queue_draw on it for bookmark repaints */
    s->gutter_renderer = g_object_ref(ln);
    g_object_unref(ln);
  }
  GtkGesture *gc = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), 1);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gc),
                                             GTK_PHASE_CAPTURE);
  g_signal_connect(gc, "pressed", G_CALLBACK(fold_gutter_click_cb), s);
  gtk_widget_add_controller(GTK_WIDGET(s->view), GTK_EVENT_CONTROLLER(gc));
}

static EditorDoc *app_doc_new(AppState *s, const char *path) {
  EditorDoc *d = g_malloc0(sizeof(EditorDoc));
  d->buf = GTK_SOURCE_BUFFER(gtk_source_buffer_new(NULL));
  d->view = GTK_SOURCE_VIEW(gtk_source_view_new_with_buffer(d->buf));
  /* Match baseline editor view properties (was previously done only once for
   * the initial view). */
  /* FoldLineNums renderer handles line numbers - keep built-in OFF */
  gtk_source_view_set_show_line_numbers(d->view, FALSE);
  gtk_source_view_set_highlight_current_line(d->view, FALSE);
  // Show diagnostics in the left gutter (error/warning markers)
  gtk_source_view_set_show_line_marks(d->view, TRUE);
  {
    GtkSourceMarkAttributes *ma_err = gtk_source_mark_attributes_new();
    gtk_source_mark_attributes_set_icon_name(ma_err, "dialog-error-symbolic");
    gtk_source_view_set_mark_attributes(d->view, "diag-error", ma_err, 0);
    g_object_unref(ma_err);
    GtkSourceMarkAttributes *ma_warn = gtk_source_mark_attributes_new();
    gtk_source_mark_attributes_set_icon_name(ma_warn,
                                             "dialog-warning-symbolic");
    gtk_source_view_set_mark_attributes(d->view, "diag-warning", ma_warn, 0);
    g_object_unref(ma_warn);
  }
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(d->view), TRUE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(d->view), TRUE);

  d->curline_applied = -1;

  diags_init(&d->sem_diags);
  diags_init(&d->asm_diags);
  d->mem_valid = FALSE;
  d->mem_dirty = FALSE;
  d->mem_view = NULL;

  d->mem_image = g_malloc0(65536);

  if (path)
    d->path = g_strdup(path);

  /* scroller wrapper */
  GtkWidget *sw = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), GTK_WIDGET(d->view));
  d->scroller = sw;

  /* Apply current editor prefs to this view */
  GtkSourceView *old_view = s->view;
  GtkSourceBuffer *old_buf = s->buf;

  /* Temporarily alias so existing setup helpers operate on the doc */
  s->view = d->view;
  s->buf = d->buf;

  create_tags(s);
  /* copy tags into doc */
  d->tag_instr = s->tag_instr;
  d->tag_dir = s->tag_dir;
  d->tag_data = s->tag_data;
  d->tag_num = s->tag_num;
  d->tag_label = s->tag_label;
  d->tag_addr_hex = s->tag_addr_hex;
  d->tag_addr_dec = s->tag_addr_dec;
  d->tag_addr_bin = s->tag_addr_bin;
  d->tag_imm_hex = s->tag_imm_hex;
  d->tag_imm_dec = s->tag_imm_dec;
  d->tag_imm_bin = s->tag_imm_bin;
  d->tag_imm_char = s->tag_imm_char;
  d->tag_dv_hex = s->tag_dv_hex;
  d->tag_dv_dec = s->tag_dv_dec;
  d->tag_dv_bin = s->tag_dv_bin;
  d->tag_dv_str = s->tag_dv_str;
  d->tag_hover_def = s->tag_hover_def;
  d->tag_reg = s->tag_reg;
  d->tag_comment = s->tag_comment;
  d->tag_string = s->tag_string;
  d->tag_error = s->tag_error;
  d->tag_inactive = s->tag_inactive;
  d->tag_fold_hidden = s->tag_fold_hidden;
  d->tag_fold_marker = s->tag_fold_marker;
  d->tag_curline = s->tag_curline;
  d->curline_applied = s->curline_applied;
  d->hl_idle_id = s->hl_idle_id;

  d->label_defs =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  d->hover_start = -1;
  d->hover_end = -1;

  setup_source_styling(s);
  editor_install_ctrl_click(s);
  editor_install_nav_keys(s);
  editor_install_context_menu(s);
  fold_gutter_install(s);
  editor_install_hover_tooltips(s);
  editor_install_hover_label_defs(d);

  /* Register assembly completion provider */
  {
    GtkSourceCompletion *c =
        gtk_source_view_get_completion(GTK_SOURCE_VIEW(s->view));
    gtk_source_completion_set_page_size(c, 5);
    GtkSourceCompletionProvider *p = asm_completion_provider_new(s);
    gtk_source_completion_add_provider(c, p);
    g_object_unref(p);
    if (!s->autocomplete)
      gtk_source_completion_block_interactive(c);
  }

  editor_install_ctrlk_chords(s);

  apply_editor_prefs(s);

  // Link buffer -> document, and install per-buffer change notifications.
  // Without this, only the initial buffer is monitored, so highlighting and
  // diagnostics stop updating correctly once multiple tabs are open.
  g_object_set_data(G_OBJECT(d->buf), "editor-doc", d);
  g_signal_connect(GTK_TEXT_BUFFER(d->buf), "changed",
                   G_CALLBACK(on_buffer_changed), s);
  g_signal_connect(G_OBJECT(d->buf), "notify::modified",
                   G_CALLBACK(on_modified_changed), s);

  /* Ctrl+scroll wheel zooms font size */
  GtkEventController *scroll_ctl =
      gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
  g_signal_connect(scroll_ctl, "scroll", G_CALLBACK(editor_scroll_cb), s);
  gtk_widget_add_controller(GTK_WIDGET(d->view), scroll_ctl);

  /* Restore alias */
  s->view = old_view;
  s->buf = old_buf;

  if (path)
    d->path = g_strdup(path);

  return d;
}

/* Activate a doc: swap alias fields + diags to make existing code operate on
 * active tab */
static void app_activate_doc(AppState *s, EditorDoc *d) {
  if (!s || !d)
    return;

  /* Save current doc state back (diags) */
  if (s->active_doc) {
    EditorDoc *cur = s->active_doc;
    /* swap diagnostics structs */
    diags_t tmp = cur->sem_diags;
    cur->sem_diags = s->sem_diags;
    s->sem_diags = tmp;

    tmp = cur->asm_diags;
    cur->asm_diags = s->asm_diags;
    s->asm_diags = tmp;

    cur->mem_valid = s->mem_valid;
    cur->mem_dirty = s->mem_dirty;
    cur->mem_cpu = s->mem_cpu;
    if (s->mem_image && cur->mem_image)
      memcpy(cur->mem_image, s->mem_image, 65536);
    cur->mem_view = s->mem_view;
    cur->mem_asm_start = s->mem_asm_start;
    cur->mem_asm_valid = s->mem_asm_valid;
    cur->mem_last_addr = s->mem_last_addr;
    cur->mem_last_addr_valid = s->mem_last_addr_valid;
    cur->dirty = s->dirty;
    /* s->path is an alias to cur->path while active; do not free/dup here. */
  }

  s->active_doc = d;

  /* Alias pointers/state for existing code */
  s->buf = d->buf;
  s->view = d->view;
  search_context_init(s);
  app_state_set_path(s, d->path);
  s->dirty = d->dirty;

  s->tag_instr = d->tag_instr;
  s->tag_dir = d->tag_dir;
  s->tag_data = d->tag_data;
  s->tag_num = d->tag_num;
  s->tag_label = d->tag_label;
  s->tag_addr_hex = d->tag_addr_hex;
  s->tag_addr_dec = d->tag_addr_dec;
  s->tag_addr_bin = d->tag_addr_bin;
  s->tag_imm_hex = d->tag_imm_hex;
  s->tag_imm_dec = d->tag_imm_dec;
  s->tag_imm_bin = d->tag_imm_bin;
  s->tag_imm_char = d->tag_imm_char;
  s->tag_dv_hex = d->tag_dv_hex;
  s->tag_dv_dec = d->tag_dv_dec;
  s->tag_dv_bin = d->tag_dv_bin;
  s->tag_dv_str = d->tag_dv_str;
  s->tag_hover_def = d->tag_hover_def;
  s->tag_reg = d->tag_reg;
  s->tag_comment = d->tag_comment;
  s->tag_string = d->tag_string;
  s->tag_error = d->tag_error;
  s->tag_inactive = d->tag_inactive;
  s->tag_fold_hidden = d->tag_fold_hidden;
  s->tag_fold_marker = d->tag_fold_marker;
  s->tag_curline = d->tag_curline;
  s->curline_applied = d->curline_applied;
  s->hl_idle_id = d->hl_idle_id;

  /* Re-apply theme styling to the newly-active buffer/view. The CSS provider is
     global, but the GtkSourceBuffer style scheme is per-buffer and needs to be
     refreshed when switching documents. */
  apply_style_scheme(s);
  if (s->tag_curline) {
    g_object_set(s->tag_curline, "background",
                 s->dark_mode ? "#2a2a2a" : "#e9e9e9", NULL);
  }

  /* Restore per-doc diagnostics by swapping */
  diags_t tmp = d->sem_diags;
  d->sem_diags = s->sem_diags;
  s->sem_diags = tmp;

  tmp = d->asm_diags;
  d->asm_diags = s->asm_diags;
  s->asm_diags = tmp;

  /* Memory image alias via copy into AppState storage */
  s->mem_valid = d->mem_valid;
  s->mem_dirty = d->mem_dirty;
  s->mem_cpu = d->mem_cpu;
  if (s->mem_image && d->mem_image)
    memcpy(s->mem_image, d->mem_image, 65536);
  s->mem_view = d->mem_view;
  s->mem_asm_start = d->mem_asm_start;
  s->mem_asm_valid = d->mem_asm_valid;
  s->mem_last_addr = d->mem_last_addr;
  s->mem_last_addr_valid = d->mem_last_addr_valid;

  /* Update UI title */
  update_title(s);

  /* Ensure prefs applied to active view */
  apply_editor_prefs(s);

  /* Refresh problems list if visible */
  problems_refresh_now(s);
}

static EditorDoc *app_get_active_doc(AppState *s) {
  if (!s)
    return NULL;
  if (s->active_doc)
    return s->active_doc;
  if (s->notebook && s->docs) {
    int page = gtk_notebook_get_current_page(s->notebook);
    if (page >= 0 && (guint)page < s->docs->len)
      return (EditorDoc *)g_ptr_array_index(s->docs, (guint)page);
  }
  return NULL;
}

static void tab_close_clicked(GtkButton *btn, gpointer user_data);

static void app_add_doc_tab(AppState *s, EditorDoc *d, const char *label) {
  if (!s || !s->notebook || !d)
    return;

  /* Custom tab label: text + a small close button. The global '+' button is an
     action widget at the end of the notebook (not per-tab). */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *lbl = gtk_label_new(label ? label : "(untitled)");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  /* Prevent tabs from expanding to fill the full width. */
  gtk_widget_set_hexpand(lbl, FALSE);
  gtk_box_append(GTK_BOX(box), lbl);

  GtkWidget *close = gtk_button_new_with_label("x");
  gtk_widget_set_focusable(close, FALSE);
  gtk_widget_add_css_class(close, "flat");
  gtk_box_append(GTK_BOX(box), close);
  /* Pass the page widget to the handler (it can recover the doc + index). */
  g_signal_connect(close, "clicked", G_CALLBACK(tab_close_clicked),
                   d->scroller);

  /* Stash pointers on the page widget so we can update the label later. */
  g_object_set_data(G_OBJECT(d->scroller), "tab-label", lbl);

  int page = gtk_notebook_append_page(s->notebook, d->scroller, box);
  /* Bind the notebook page widget to its document so tab switching stays
     correct even if the user reorders tabs. */
  g_object_set_data(G_OBJECT(d->scroller), "app-doc", d);
  gtk_notebook_set_tab_reorderable(s->notebook, d->scroller, TRUE);
  /* Keep tabs compact instead of stretching across the full bar. */
  {
    GtkNotebookPage *page =
        gtk_notebook_get_page(GTK_NOTEBOOK(s->notebook), d->scroller);
    if (page) {
      // These are GObject properties on GtkNotebookPage in GTK4.
      g_object_set(G_OBJECT(page), "tab-expand", FALSE, "tab-fill", FALSE,
                   NULL);
    }
  }
  gtk_notebook_set_current_page(s->notebook, page);
}

static void on_notebook_switch_page(GtkNotebook *nb, GtkWidget *page,
                                    guint page_num, gpointer user_data) {
  (void)nb;
  (void)page;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;

  /* Prefer the document pointer bound to the page widget. This stays correct
     when tabs are reordered. */
  EditorDoc *d = NULL;
  if (page)
    d = (EditorDoc *)g_object_get_data(G_OBJECT(page), "app-doc");

  /* Fallback to index-based lookup (older state). */
  if (!d) {
    if (!s->docs)
      return;
    if (s->docs->len == 0)
      return;
    if (page_num >= s->docs->len)
      return;
    d = g_ptr_array_index(s->docs, page_num);
  }
  app_activate_doc(s, d);

  /* Re-run semantic highlighting/diagnostics when switching documents.
     Otherwise files opened via the include manager can appear "unchecked"
     until edited. */
  schedule_highlight(s);
}

/* Close-button on the last remaining tab acts as a request to exit the app. */
/* Forward declarations needed by tab-close callbacks */
static gboolean save_buffer_to_file(AppState *s, const char *path,
                                    GError **err);
static void action_save_as(GSimpleAction *a, GVariant *p, gpointer user_data);

/* Context for the "save before closing last tab?" dialog */
typedef struct {
  AppState *s;
  GtkWidget *page; /* the tab page widget to remove after decision */
  EditorDoc *d;    /* the doc being closed */
} LastTabCloseCtx;

static void last_tab_do_replace(AppState *s, GtkWidget *page, EditorDoc *d) {
  /* Open a fresh untitled tab first so there is always one visible */
  EditorDoc *nd = app_doc_new(s, NULL);
  if (nd) {
    g_ptr_array_add(s->docs, nd);
    app_add_doc_tab(s, nd, "(untitled)");
    int idx = gtk_notebook_get_n_pages(s->notebook) - 1;
    if (idx >= 0)
      gtk_notebook_set_current_page(s->notebook, idx);
    app_activate_doc(s, nd);
  }
  /* Now remove the old tab */
  GtkNotebook *nb = s->notebook;
  int page_idx = gtk_notebook_page_num(nb, page);
  if (page_idx >= 0)
    gtk_notebook_remove_page(nb, page_idx);
  if (d && s->docs) {
    for (guint i = 0; i < s->docs->len; i++) {
      if (g_ptr_array_index(s->docs, i) == d) {
#if GLIB_CHECK_VERSION(2, 58, 0)
        EditorDoc *stolen = (EditorDoc *)g_ptr_array_steal_index(s->docs, i);
        if (stolen)
          app_doc_destroy(stolen);
#else
        g_ptr_array_remove_index(s->docs, i);
#endif
        break;
      }
    }
  }
}

/* Response callback for the save-before-close dialog on last tab close. */
static void last_tab_save_response(GtkDialog *dlg, int response_id,
                                   gpointer user_data) {
  LastTabCloseCtx *ctx = (LastTabCloseCtx *)user_data;
  AppState *s = ctx->s;
  GtkWidget *page = ctx->page;
  EditorDoc *d = ctx->d;
  g_free(ctx);
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (!s)
    return;

  if (response_id == GTK_RESPONSE_CANCEL)
    return;

  if (response_id == GTK_RESPONSE_YES) {
    if (s->path) {
      /* Has a path: save synchronously then replace */
      GError *err = NULL;
      save_buffer_to_file(s, s->path, &err);
      if (err)
        g_clear_error(&err);
      last_tab_do_replace(s, page, d);
    } else {
      /* No path: open Save As dialog; complete close in save_finish_cb */
      s->pending_close_page = page;
      s->pending_close_doc = d;
      action_save_as(NULL, NULL, s);
    }
    return;
  }
  /* GTK_RESPONSE_NO: discard */
  last_tab_do_replace(s, page, d);
}

static void last_tab_exit_response(GtkDialog *dlg, int response_id,
                                   gpointer user_data) {
  AppState *s = (AppState *)user_data;
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (!s || !s->win)
    return;
  if (response_id == GTK_RESPONSE_ACCEPT)
    gtk_window_close(GTK_WINDOW(s->win));
}

static void on_notebook_pages_changed(GtkNotebook *nb, GtkWidget *page,
                                      guint page_num, gpointer user_data) {
  (void)nb;
  (void)page;
  (void)page_num;
  /* No-op: the '+' button is a notebook action widget, not part of tab labels.
   */
  (void)user_data;
}

/* Context for the "save before closing a tab?" dialog (multi-tab case) */
typedef struct {
  AppState *s;
  GtkWidget *page;
  EditorDoc *d;
} TabCloseCtx;

static void tab_do_close(AppState *s, GtkWidget *page, EditorDoc *d) {
  GtkNotebook *nb = s->notebook;
  int page_idx = gtk_notebook_page_num(nb, page);
  if (page_idx >= 0)
    gtk_notebook_remove_page(nb, page_idx);
  if (d && s->docs) {
    for (guint i = 0; i < s->docs->len; i++) {
      if (g_ptr_array_index(s->docs, i) == d) {
#if GLIB_CHECK_VERSION(2, 58, 0)
        EditorDoc *stolen = (EditorDoc *)g_ptr_array_steal_index(s->docs, i);
        if (stolen)
          app_doc_destroy(stolen);
#else
        g_ptr_array_remove_index(s->docs, i);
#endif
        break;
      }
    }
  }
  /* Safety net: should never be zero here in the multi-tab path,
     but guard just in case. */
  if (gtk_notebook_get_n_pages(nb) == 0) {
    EditorDoc *nd = app_doc_new(s, NULL);
    if (nd) {
      g_ptr_array_add(s->docs, nd);
      app_add_doc_tab(s, nd, "(untitled)");
      gtk_notebook_set_current_page(nb, 0);
    }
  }
}

static void tab_close_save_response(GtkDialog *dlg, int response_id,
                                    gpointer user_data) {
  TabCloseCtx *ctx = (TabCloseCtx *)user_data;
  AppState *s = ctx->s;
  GtkWidget *page = ctx->page;
  EditorDoc *d = ctx->d;
  g_free(ctx);
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (!s)
    return;
  if (response_id == GTK_RESPONSE_CANCEL)
    return;
  if (response_id == GTK_RESPONSE_YES) {
    if (s->path) {
      /* Has a path: save synchronously then close */
      GError *err = NULL;
      save_buffer_to_file(s, s->path, &err);
      if (err)
        g_clear_error(&err);
      tab_do_close(s, page, d);
    } else {
      /* No path: open Save As; complete close in save_finish_cb */
      s->pending_close_page = page;
      s->pending_close_doc = d;
      action_save_as(NULL, NULL, s);
    }
    return;
  }
  /* GTK_RESPONSE_NO: discard */
  tab_do_close(s, page, d);
}

static void tab_close_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  GtkWidget *page = GTK_WIDGET(user_data);
  if (!page)
    return;

  /* Find the notebook. In GTK4 the page is usually packed into an internal
     stack, so use ancestor walking. */
  GtkWidget *nbw = gtk_widget_get_ancestor(page, GTK_TYPE_NOTEBOOK);
  if (!nbw)
    return;
  GtkNotebook *nb = GTK_NOTEBOOK(nbw);

  AppState *s = (AppState *)g_object_get_data(G_OBJECT(nb), "app-state");
  if (!s)
    return;

  EditorDoc *d = (EditorDoc *)g_object_get_data(G_OBJECT(page), "app-doc");

  /* If this is the last remaining tab, open a new untitled tab instead
     of quitting. If the current tab has unsaved changes, prompt first. */
  if (gtk_notebook_get_n_pages(nb) == 1) {
    gboolean is_dirty =
        (d && d->buf && gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(d->buf)));
    if (is_dirty) {
      const char *label = (d->path && d->path[0]) ? d->path : "(untitled)";
      char msg[512];
      snprintf(msg, sizeof(msg), "Save changes to %s?", label);
      GtkWidget *dlg = gtk_message_dialog_new(
          GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
          GTK_BUTTONS_NONE, "%s", msg);
      gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
      gtk_dialog_add_button(GTK_DIALOG(dlg), "Discard", GTK_RESPONSE_NO);
      gtk_dialog_add_button(GTK_DIALOG(dlg), "Save", GTK_RESPONSE_YES);
      gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);
      LastTabCloseCtx *ctx = g_new0(LastTabCloseCtx, 1);
      ctx->s = s;
      ctx->page = page;
      ctx->d = d;
      g_signal_connect(dlg, "response", G_CALLBACK(last_tab_save_response),
                       ctx);
      gtk_window_present(GTK_WINDOW(dlg));
    } else {
      last_tab_do_replace(s, page, d);
    }
    return;
  }

  /* Prompt if unsaved changes */
  gboolean is_dirty =
      (d && d->buf && gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(d->buf)));
  if (is_dirty) {
    const char *label = (d->path && d->path[0]) ? d->path : "(untitled)";
    char msg[512];
    snprintf(msg, sizeof(msg), "Save changes to %s?", label);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "%s", msg);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Discard", GTK_RESPONSE_NO);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Save", GTK_RESPONSE_YES);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);
    TabCloseCtx *ctx = g_new0(TabCloseCtx, 1);
    ctx->s = s;
    ctx->page = page;
    ctx->d = d;
    g_signal_connect(dlg, "response", G_CALLBACK(tab_close_save_response), ctx);
    gtk_window_present(GTK_WINDOW(dlg));
    return;
  }

  tab_do_close(s, page, d);
}

/* Callback for the '+' tab button: open a new empty document. */
static void tab_plus_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;
  // Mirror action_new() behaviour (append a new untitled doc and activate it).
  EditorDoc *d = app_doc_new(s, NULL);
  if (!d)
    return;
  g_ptr_array_add(s->docs, d);
  app_add_doc_tab(s, d, "(untitled)");
  int idx = gtk_notebook_get_n_pages(s->notebook) - 1;
  if (idx >= 0)
    gtk_notebook_set_current_page(s->notebook, idx);
  app_activate_doc(s, d);
}

/* NOTE: notebook_refresh_plus() removed (the '+' is now an action widget). */

static char *path_with_default_ext(const char *path, const char *def_ext) {
  if (!path || !*path)
    return NULL;
  if (!def_ext || !*def_ext)
    return g_strdup(path);

  char *base = g_path_get_basename(path);
  if (!base)
    return g_strdup(path);

  const char *dot = strrchr(base, '.');
  gboolean has_ext = FALSE;
  if (dot && dot != base) {
    // Treat "." as no extension. Anything like "foo." counts as no extension
    // too.
    has_ext = (dot[1] != '\0');
  }

  g_free(base);
  if (has_ext)
    return g_strdup(path);
  return g_strconcat(path, def_ext, NULL);
}

static asm_cpu_t cpu_from_preproc_defs(const defs_t *defs) {
  if (!defs)
    return ASM_CPU_6502;
  const char *cpuv = defs_get((defs_t *)defs, sv_from_cstr("cpu"));
  if (!cpuv)
    return ASM_CPU_6502;
  if (strcmp(cpuv, "6503") == 0 || strcmp(cpuv, "65c02") == 0 ||
      strcmp(cpuv, "65C02") == 0)
    return ASM_CPU_65C02;
  if (strcmp(cpuv, "65816") == 0 || strcmp(cpuv, "65c816") == 0 ||
      strcmp(cpuv, "65816s") == 0 || strcmp(cpuv, "w65c816") == 0)
    return ASM_CPU_65816;
  return ASM_CPU_6502;
}

/* ---------------- Problems panel (bottom diagnostics list) --------------- */

typedef struct _ProblemItem {
  GObject parent_instance;
  char *severity;
  char *file;
  guint line;
  guint col;
  char *message;
} ProblemItem;

typedef struct _ProblemItemClass {
  GObjectClass parent_class;
} ProblemItemClass;

G_DEFINE_TYPE(ProblemItem, problem_item, G_TYPE_OBJECT)

static void problem_item_finalize(GObject *obj) {
  ProblemItem *p = (ProblemItem *)obj;
  g_clear_pointer(&p->severity, g_free);
  g_clear_pointer(&p->file, g_free);
  g_clear_pointer(&p->message, g_free);
  G_OBJECT_CLASS(problem_item_parent_class)->finalize(obj);
}

/* GObject class init for ProblemItem (problems-panel list row). */
static void problem_item_class_init(ProblemItemClass *klass) {
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->finalize = problem_item_finalize;
}

static void problem_item_init(ProblemItem *p) { (void)p; }

static ProblemItem *problem_item_new(const char *sev, const char *file,
                                     guint line, guint col, const char *msg) {
  ProblemItem *p = (ProblemItem *)g_object_new(problem_item_get_type(), NULL);
  p->severity = g_strdup(sev ? sev : "");
  p->file = g_strdup(file ? file : "");
  p->line = line;
  p->col = col;
  p->message = g_strdup(msg ? msg : "");
  return p;
}

/* Idle callback: hide the 'copied' toast notification after its timeout. */
static gboolean copy_toast_hide_cb(gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s)
    return G_SOURCE_REMOVE;
  s->copy_toast_hide_id = 0;
  if (s->copy_toast)
    gtk_revealer_set_reveal_child(GTK_REVEALER(s->copy_toast), FALSE);
  return G_SOURCE_REMOVE;
}

/* Show a brief toast notification (e.g. 'Copied to clipboard'). */
static void copy_toast_show(AppState *s, const char *msg) {
  if (!s || !s->copy_toast || !s->copy_toast_label)
    return;

  gtk_label_set_text(GTK_LABEL(s->copy_toast_label),
                     (msg && *msg) ? msg : "Copied to clipboard");

  if (s->copy_toast_hide_id) {
    g_source_remove(s->copy_toast_hide_id);
    s->copy_toast_hide_id = 0;
  }

  gtk_revealer_set_reveal_child(GTK_REVEALER(s->copy_toast), TRUE);
  s->copy_toast_hide_id = g_timeout_add(2000, copy_toast_hide_cb, s);
}

static ProblemItem *problems_get_selected_item(AppState *s) {
  if (!s || !s->problems_sel || !s->problems_store)
    return NULL;

  guint pos =
      gtk_single_selection_get_selected(GTK_SINGLE_SELECTION(s->problems_sel));
  if (pos == GTK_INVALID_LIST_POSITION)
    return NULL;

  return (ProblemItem *)g_list_model_get_item(G_LIST_MODEL(s->problems_store),
                                              pos);
}

/* Copy the message of the selected problems-panel row to the clipboard. */
static void problems_copy_selected(AppState *s) {
  if (!s || !s->problems_view)
    return;

  ProblemItem *p = problems_get_selected_item(s);
  if (!p)
    return;

  char *txt = g_strdup_printf("%u: %s", p->line, p->message ? p->message : "");
  GdkClipboard *cb = gtk_widget_get_clipboard(s->problems_view);
  if (cb)
    gdk_clipboard_set_text(cb, txt);

  copy_toast_show(s, "Copied to clipboard");

  g_free(txt);
  g_object_unref(p);
}

/* Key handler for the problems panel: Ctrl+C copies the selected item. */
static gboolean problems_key_pressed_cb(GtkEventControllerKey *controller,
                                        guint keyval, guint keycode,
                                        GdkModifierType state,
                                        gpointer user_data) {
  (void)controller;
  (void)keycode;
  AppState *s = (AppState *)user_data;
  if (!s)
    return FALSE;

  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask();
  if ((mods & GDK_CONTROL_MASK) &&
      (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
    problems_copy_selected(s);
    return TRUE;
  }

  return FALSE;
}

/* Click handler for the problems panel: navigate to the clicked error. */
static void problems_click_pressed_cb(GtkGestureClick *gesture, int n_press,
                                      double x, double y, gpointer user_data) {
  (void)x;
  (void)y;
  AppState *s = (AppState *)user_data;
  if (!s || n_press != 1)
    return;

  guint button =
      gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(gesture));
  if (button == GDK_BUTTON_SECONDARY) {
    problems_copy_selected(s);
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  }
}

/* Hide the problems panel revealer. */
static void problems_hide(AppState *s) {
  if (!s || !s->problems_revealer)
    return;
  s->problems_user_hidden = TRUE;

  /* GtkRevealer itself can still consume layout space if left vexpand=TRUE.
     When hiding, disable vexpand and hint a 0 height so the editor pane really
     reclaims the area instead of leaving a blank strip. */
  gtk_widget_set_vexpand(s->problems_revealer, FALSE);
  gtk_widget_set_size_request(s->problems_revealer, -1, 0);

  gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), FALSE);

  /* Collapse the end child so the editor gets the space back and the divider
     disappears (as much as possible) when hidden. */
  if (s->problems_paned && GTK_IS_PANED(s->problems_paned)) {
    int h = gtk_widget_get_height(s->problems_paned);
    if (h > 0) {
      int pos = gtk_paned_get_position(GTK_PANED(s->problems_paned));
      int end_h = h - pos;
      if (!s->problems_collapsed && end_h > s->problems_min_end_px)
        s->problems_last_end_px = end_h;

      s->problems_collapsed_pos = pos;
      gtk_paned_set_position(GTK_PANED(s->problems_paned), h);
      s->problems_collapsed = TRUE;
      gtk_widget_add_css_class(s->problems_paned, "problems-hidden");
    }
  }

  s->problems_visible = FALSE;

  /* Keep View->Problems panel toggle in sync. */
  if (s && s->app) {
    GAction *ga =
        g_action_map_lookup_action(G_ACTION_MAP(s->app), "problems_visible");
    if (ga)
      g_simple_action_set_state(G_SIMPLE_ACTION(ga),
                                g_variant_new_boolean(FALSE));
  }
}

/* Auto-hide: used when the problems list becomes empty. This should collapse
   the panel and return space to the editor, but MUST NOT mark the panel as
   user-hidden (otherwise future errors won't auto-show). */
static void problems_autohide(AppState *s) {
  if (!s || !s->problems_revealer)
    return;

  /* Same layout fix as problems_hide(), but without changing the
     user-hidden state. */
  gtk_widget_set_vexpand(s->problems_revealer, FALSE);
  gtk_widget_set_size_request(s->problems_revealer, -1, 0);

  gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), FALSE);

  /* Collapse the end child so the editor gets the space back and the divider
     disappears (as much as possible) when hidden. */
  if (s->problems_paned && GTK_IS_PANED(s->problems_paned)) {
    int h = gtk_widget_get_height(s->problems_paned);
    if (h > 0) {
      int pos = gtk_paned_get_position(GTK_PANED(s->problems_paned));
      int end_h = h - pos;
      if (!s->problems_collapsed && end_h > s->problems_min_end_px)
        s->problems_last_end_px = end_h;

      s->problems_collapsed_pos = pos;
      gtk_paned_set_position(GTK_PANED(s->problems_paned), h);
      s->problems_collapsed = TRUE;
      gtk_widget_add_css_class(s->problems_paned, "problems-hidden");
    }
  }

  s->problems_visible = FALSE;

  /* Keep View->Problems panel toggle in sync. */
  if (s && s->app) {
    GAction *ga =
        g_action_map_lookup_action(G_ACTION_MAP(s->app), "problems_visible");
    if (ga)
      g_simple_action_set_state(G_SIMPLE_ACTION(ga),
                                g_variant_new_boolean(FALSE));
  }
}

static void problems_show(AppState *s) {
  if (!s || !s->problems_revealer)
    return;
  s->problems_user_hidden = FALSE;

  /* Re-enable expansion now that the panel is visible. */
  gtk_widget_set_vexpand(s->problems_revealer, TRUE);
  gtk_widget_set_size_request(s->problems_revealer, -1, -1);

  gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), TRUE);

  /* Restore a reasonable size (prefer the last user size). */
  if (s->problems_paned && GTK_IS_PANED(s->problems_paned)) {
    int h = gtk_widget_get_height(s->problems_paned);
    if (h > 0) {
      int want_end = (s->problems_last_end_px > 0) ? s->problems_last_end_px
                                                   : s->problems_default_end_px;
      if (want_end < s->problems_min_end_px)
        want_end = s->problems_min_end_px;
      if (want_end > h - s->problems_min_end_px)
        want_end = h - s->problems_min_end_px;
      gtk_paned_set_position(GTK_PANED(s->problems_paned), h - want_end);
      s->problems_collapsed = FALSE;
      gtk_widget_remove_css_class(s->problems_paned, "problems-hidden");
    }
  }

  s->problems_visible = TRUE;

  /* Keep View->Problems panel toggle in sync. */
  if (s && s->app) {
    GAction *ga =
        g_action_map_lookup_action(G_ACTION_MAP(s->app), "problems_visible");
    if (ga)
      g_simple_action_set_state(G_SIMPLE_ACTION(ga),
                                g_variant_new_boolean(TRUE));
  }
}
/* ---------------- Memory viewer/editor ---------------- */

/* Forward decl: used by mem-view dialogs before its definition later in the
 * file. */
static void dialog_add_default_keys(GtkWidget *dlg, int ok_resp,
                                    int cancel_resp);

typedef struct MemViewState {
  AppState *app;
  GtkWidget *win;
  GtkWidget *hdr; /* ADT-style status line (monospace) */
  GtkWidget *tv;
  GtkTextBuffer *tb;
  GtkTextTag *tag_sel_hex;
  GtkTextTag *tag_sel_ascii;

  guint8 *orig; /* snapshot of app->mem_image when opened */
  guint8 *work; /* editable copy */
  guint16 addr; /* selected byte address */

  gboolean hex_mode; /* FALSE=ASCII entry, TRUE=hex byte entry */
  int hex_nibble;    /* 0 or 1 while entering hex */

  /* TAB toggles between data-entry mode and an address-entry (goto) mode. */
  gboolean addr_mode; /* TRUE when typing edits the address instead of memory */
  guint16 addr_entry; /* working address during addr_mode */
  int addr_digits;    /* 0..4 hex digits entered */

  /* View mode (ADT-style cycling). */
  int view_mode; /* 0=HEX+ASCII, 1=DISASM, 2=ASCII, 3=BYTES */

  /* Disasm mode: maps each address to a text-buffer offset for the
   * corresponding byte */
  uint32_t *disasm_offs;  /* [65536], or NULL when not in DISASM view */
  uint16_t *disasm_start; /* [65536], start PC for containing instruction,
                             0xFFFF=unknown */
  uint16_t *disasm_prev;  /* [65536], previous instruction start for instruction
                             PCs, 0xFFFF=none */

  /* Incremental selection highlight (avoid clearing tags over whole buffer each
   * keypress) */
  gboolean sel_valid_hex;
  int sel_hex_line;
  int sel_hex_col;
  int sel_hex_len;

  gboolean sel_valid_ascii;
  int sel_ascii_line;
  int sel_ascii_col;
  int sel_ascii_len;

  gboolean dirty;
  asm_cpu_t cpu; /* CPU type for disassembly (from last assembly) */

  /* When TRUE, selection updates must not auto-scroll the text view.
     Used to keep the view stable while typing hex bytes in DISASM mode. */
  gboolean suppress_scroll;

  // Deferred scrolling target (GtkTextBuffer offset). This avoids GTK warnings
  // about snapshotting without allocation on some platforms when we scroll
  // while layout is still settling.
  gboolean pending_scroll;
  int pending_scroll_offset;

  /* Key diagnostics (helps debug keys that never seem to arrive, e.g. End). */
  char key_diag[128];
  FILE *key_diag_fp;
} MemViewState;

// Some operations (notably scrolling to an iter) can trigger GTK snapshot
// warnings if performed before the TextView has a real allocation. Guard these
// calls.
static inline gboolean mem_view_tv_ready(MemViewState *mv) {
  if (!mv || !mv->tv)
    return FALSE;
  GtkWidget *w = GTK_WIDGET(mv->tv);
  if (!gtk_widget_get_mapped(w))
    return FALSE;
  return gtk_widget_get_allocated_width(w) > 0 &&
         gtk_widget_get_allocated_height(w) > 0;
}

static gboolean mem_view_scroll_idle_cb(gpointer data) {
  MemViewState *mv = (MemViewState *)data;
  if (!mv || !mv->tv)
    return G_SOURCE_REMOVE;
  if (!mv->pending_scroll)
    return G_SOURCE_REMOVE;
  if (!mem_view_tv_ready(mv))
    return G_SOURCE_CONTINUE;

  GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(mv->tv));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_offset(buf, &it, mv->pending_scroll_offset);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(mv->tv), &it, 0.1, FALSE, 0.0,
                               0.0);
  mv->pending_scroll = FALSE;
  return G_SOURCE_REMOVE;
}

/* Scroll the memory-viewer text view so that *it is visible. */
static void mem_view_request_scroll_to_iter(MemViewState *mv,
                                            const GtkTextIter *it) {
  if (!mv || !mv->tv || !it)
    return;
  if (mv->suppress_scroll)
    return;

  // Always defer the scroll to idle. Even if the widget is currently mapped
  // and has an allocation, doing scroll-to-iter during certain input paths
  // can trigger GTK warnings on some systems.
  mv->pending_scroll_offset = gtk_text_iter_get_offset((GtkTextIter *)it);
  mv->pending_scroll = TRUE;
  g_idle_add(mem_view_scroll_idle_cb, mv);
}

// Mouse hit-testing helpers
static gboolean mem_view_click_to_addr(MemViewState *mv, GtkTextIter *it,
                                       gboolean prefer_ascii);
static void mem_view_click_pressed_cb(GtkGestureClick *gesture, int n_press,
                                      double x, double y, gpointer user_data);

// Forward decls used by click hit-testing
static int disasm_inst_len(MemViewState *mv, uint16_t pc);

static void mem_view_refresh(MemViewState *mv);
static void mem_view_update_hdr(MemViewState *mv);
static void mem_view_destroy_cb(GtkWidget *w, gpointer user_data);

static void mem_view_update_title(MemViewState *mv) {
  if (!mv || !mv->win)
    return;
  char t[256];
  const char *mode = "HEX";
  if (mv->view_mode == 1)
    mode = "DISASM";
  else if (mv->view_mode == 2)
    mode = "ASCII";
  else if (mv->view_mode == 3)
    mode = "BYTES";
  const char *diag = (mv->key_diag[0] ? mv->key_diag : NULL);
  if (mv->addr_mode) {
    if (diag)
      snprintf(t, sizeof(t),
               "Memory Viewer [%s] - Goto %04X (ESC to exit)  |  %s", mode,
               (unsigned)mv->addr_entry, diag);
    else
      snprintf(t, sizeof(t), "Memory Viewer [%s] - Goto %04X (ESC to exit)",
               mode, (unsigned)mv->addr_entry);
  } else {
    const char *m =
        mv->hex_mode ? "Hex entry (END toggles)" : "ASCII entry (END toggles)";
    if (diag)
      snprintf(t, sizeof(t), "Memory Viewer [%s] - %s  |  %s", mode, m, diag);
    else
      snprintf(t, sizeof(t), "Memory Viewer [%s] - %s", mode, m);
  }
  // gtk_window_set_title(GTK_WINDOW(mv->win), t);
  mem_view_update_hdr(mv);
}

static void mem_view_update_hdr(MemViewState *mv) {
  char buf[256];
  const guint8 v = mv->work[mv->addr];
  const char ch = (v >= 32 && v < 127) ? (char)v : '.';

  char bin[9];
  for (int i = 0; i < 8; i++) {
    bin[i] = (v & (1u << (7 - i))) ? '1' : '0';
  }
  bin[8] = 0;

  if (mv->addr_mode) {
    /* Build a 4-hex-digit field showing how many digits have been entered. */
    char a[5] = {'_', '_', '_', '_', 0};
    guint16 tmp = mv->addr_entry;
    char full[5];
    snprintf(full, sizeof(full), "%04X", (unsigned)tmp);
    for (int i = 0; i < 4; i++) {
      if (i < mv->addr_digits)
        a[i] = full[i];
    }
    snprintf(buf, sizeof(buf), "G %s   (Enter=go  Esc=cancel)", a);
  } else {
    snprintf(buf, sizeof(buf),
             "%04X (%c) %02X %s   Entry type:%s  |  TAB:mode  END:entry type  "
             "F1:goto",
             (unsigned)mv->addr, ch, (unsigned)v, bin,
             mv->hex_mode ? "Hex" : "Ascii");
  }

  gtk_label_set_text(GTK_LABEL(mv->hdr), buf);
}

static inline int mem_line_for_addr(guint16 a) { return (int)(a >> 4); }
static inline int mem_col_for_addr(guint16 a) { return (int)(a & 0x0F); }

/* line layout:
   "0000: " (6 chars) +
   16 * "AA " (48 chars) +
   " |" (2 chars) +
   16 * "." (16 chars) +
   "|\n" (2 chars)
*/
enum { MEM_LINE_HEX0 = 6, MEM_LINE_ASCII0 = 6 + (16 * 3) + 2 };

static void mem_view_clear_selection(MemViewState *mv) {
  if (!mv || !mv->tb)
    return;

  if (mv->sel_valid_hex) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &a, mv->sel_hex_line,
                                            mv->sel_hex_col);
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &b, mv->sel_hex_line,
                                            mv->sel_hex_col + mv->sel_hex_len);
    gtk_text_buffer_remove_tag(mv->tb, mv->tag_sel_hex, &a, &b);
    mv->sel_valid_hex = FALSE;
  }

  if (mv->sel_valid_ascii) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &a, mv->sel_ascii_line,
                                            mv->sel_ascii_col);
    gtk_text_buffer_get_iter_at_line_offset(
        mv->tb, &b, mv->sel_ascii_line, mv->sel_ascii_col + mv->sel_ascii_len);
    gtk_text_buffer_remove_tag(mv->tb, mv->tag_sel_ascii, &a, &b);
    mv->sel_valid_ascii = FALSE;
  }
}

/* Refresh the memory-viewer highlight and address display after selection
 * changes. */
static void mem_view_update_selection(MemViewState *mv) {
  if (!mv || !mv->tb)
    return;

  mem_view_clear_selection(mv);

  /* Compute selection position based on view mode */
  if (mv->view_mode == 1) {
    /* DISASM: offset table maps each address to the 2 hex digits of that byte
     */
    if (mv->disasm_offs) {
      const uint32_t off = mv->disasm_offs[mv->addr];
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_offset(mv->tb, &it, (int)off);
      /* Convert to line/col for incremental removal next time */
      mv->sel_hex_line = gtk_text_iter_get_line(&it);
      mv->sel_hex_col = gtk_text_iter_get_line_offset(&it);
      mv->sel_hex_len = 2;
      mv->sel_valid_hex = TRUE;

      GtkTextIter a, b;
      a = it;
      b = it;
      gtk_text_iter_forward_chars(&b, 2);
      gtk_text_buffer_apply_tag(mv->tb, mv->tag_sel_hex, &a, &b);

      gtk_text_buffer_place_cursor(mv->tb, &a);
      if (!mv->suppress_scroll && mem_view_tv_ready(mv))
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(mv->tv), &a, 0.1, FALSE, 0.0,
                                     0.0);
    }
    mem_view_update_hdr(mv);
    return;
  }

  if (mv->view_mode == 2) {
    /* ASCII grid (32 bytes per row): "AAAA  ................................"
     */
    const int line = mv->addr >> 5;
    const int col_ascii0 = 6;
    const int col = col_ascii0 + (mv->addr & 31);

    mv->sel_ascii_line = line;
    mv->sel_ascii_col = col;
    mv->sel_ascii_len = 1;
    mv->sel_valid_ascii = TRUE;

    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &a, line, col);
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &b, line, col + 1);
    gtk_text_buffer_apply_tag(mv->tb, mv->tag_sel_ascii, &a, &b);

    gtk_text_buffer_place_cursor(mv->tb, &a);
    if (!mv->suppress_scroll && mem_view_tv_ready(mv))
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(mv->tv), &a, 0.1, FALSE, 0.0,
                                   0.0);
    mem_view_update_hdr(mv);
    return;
  }

  /* Mode 0 = HEX+ASCII (16/row); Mode 3 = BYTES hex-only (16/row) */
  const int line = mv->addr >> 4;
  const int idx = mv->addr & 15;

  int col_hex0 = MEM_LINE_HEX0;
  int col_ascii0 = MEM_LINE_ASCII0;

  if (mv->view_mode == 3) {
    /* "AAAA: xx xx ..." -> first byte starts at column 6 */
    col_hex0 = 6;
  }

  /* HEX selection (always present in mode 0 and 3) */
  {
    const int col = col_hex0 + idx * 3;
    mv->sel_hex_line = line;
    mv->sel_hex_col = col;
    mv->sel_hex_len = 2;
    mv->sel_valid_hex = TRUE;

    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &a, line, col);
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &b, line, col + 2);
    gtk_text_buffer_apply_tag(mv->tb, mv->tag_sel_hex, &a, &b);

    gtk_text_buffer_place_cursor(mv->tb, &a);
    if (!mv->suppress_scroll && mem_view_tv_ready(mv))
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(mv->tv), &a, 0.1, FALSE, 0.0,
                                   0.0);
  }

  /* ASCII selection only in mode 0 */
  if (mv->view_mode == 0) {
    const int col = col_ascii0 + idx;
    mv->sel_ascii_line = line;
    mv->sel_ascii_col = col;
    mv->sel_ascii_len = 1;
    mv->sel_valid_ascii = TRUE;

    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &a, line, col);
    gtk_text_buffer_get_iter_at_line_offset(mv->tb, &b, line, col + 1);
    gtk_text_buffer_apply_tag(mv->tb, mv->tag_sel_ascii, &a, &b);
  }

  /* Always sync the header label to the current address */
  mem_view_update_hdr(mv);
}

static gboolean mem_view_click_to_addr(MemViewState *mv, GtkTextIter *it,
                                       gboolean prefer_ascii) {
  if (!mv || !it)
    return FALSE;

  const int line = gtk_text_iter_get_line(it);
  const int col = gtk_text_iter_get_line_offset(it);

  // Most modes are fixed-width and can be mapped purely by (line,col)
  if (mv->view_mode == 0) {
    // Hex + ASCII view: "AAAA: <hex...> |<ascii>|"
    const int base = (line < 0) ? 0 : (line * 16);
    if (col >= MEM_LINE_HEX0 && col < MEM_LINE_HEX0 + 16 * 3) {
      const int idx = (col - MEM_LINE_HEX0) / 3;
      if (idx >= 0 && idx < 16) {
        mv->addr = (uint16_t)(base + idx);
        mv->hex_mode = TRUE;
        return TRUE;
      }
    }
    if (col >= MEM_LINE_ASCII0 && col < MEM_LINE_ASCII0 + 16) {
      const int idx = col - MEM_LINE_ASCII0;
      if (idx >= 0 && idx < 16) {
        mv->addr = (uint16_t)(base + idx);
        // Only switch to ASCII entry if this build supports it; otherwise keep
        // current.
        if (prefer_ascii)
          mv->hex_mode = FALSE;
        return TRUE;
      }
    }
    return FALSE;
  }

  if (mv->view_mode == 3) {
    // Hex-only view: "AAAA: <hex...>"
    const int base = (line < 0) ? 0 : (line * 16);
    if (col >= MEM_LINE_HEX0 && col < MEM_LINE_HEX0 + 16 * 3) {
      const int idx = (col - MEM_LINE_HEX0) / 3;
      if (idx >= 0 && idx < 16) {
        mv->addr = (uint16_t)(base + idx);
        mv->hex_mode = TRUE;
        return TRUE;
      }
    }
    return FALSE;
  }

  if (mv->view_mode == 2) {
    // ASCII-only view: "AAAA  <ascii...>"
    const int base = (line < 0) ? 0 : (line * 16);
    if (col >= MEM_LINE_HEX0 && col < MEM_LINE_HEX0 + 16) {
      const int idx = col - MEM_LINE_HEX0;
      if (idx >= 0 && idx < 16) {
        mv->addr = (uint16_t)(base + idx);
        mv->hex_mode = FALSE;
        return TRUE;
      }
    }
    return FALSE;
  }

  if (mv->view_mode == 1) {
    // Disassembly view: parse the address at the start of the line ("%04X  ")
    GtkTextIter ls = *it;
    gtk_text_iter_set_line_offset(&ls, 0);
    GtkTextIter le = ls;
    gtk_text_iter_forward_chars(&le, 4);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(mv->tv));
    gchar *addr_txt = gtk_text_buffer_get_text(buf, &ls, &le, FALSE);
    if (!addr_txt)
      return FALSE;
    char *endp = NULL;
    long inst = strtol(addr_txt, &endp, 16);
    g_free(addr_txt);
    if (!endp || endp == addr_txt || inst < 0 || inst > 0xFFFF)
      return FALSE;

    int len = disasm_inst_len(mv, (uint16_t)inst);
    if (len < 1)
      len = 1;
    if (len > 3)
      len = 3;

    int idx = 0;
    if (col >= MEM_LINE_HEX0 && col < MEM_LINE_HEX0 + (3 * 3)) {
      idx = (col - MEM_LINE_HEX0) / 3;
    }
    if (idx < 0)
      idx = 0;
    if (idx >= len)
      idx = len - 1;
    mv->addr = (uint16_t)((uint16_t)inst + idx);
    mv->hex_mode = TRUE; // disasm editing is hex-only
    return TRUE;
  }

  return FALSE;
}

static void mem_view_click_pressed_cb(GtkGestureClick *gesture, int n_press,
                                      double x, double y, gpointer user_data) {
  (void)n_press;
  MemViewState *mv = (MemViewState *)user_data;
  if (!mv || !mv->tv)
    return;

  // Any click cancels an in-progress G:goto entry.
  if (mv->addr_mode) {
    mv->addr_mode = FALSE;
    mv->addr_digits = 0;
    mv->hex_nibble = 0;
    mem_view_update_title(mv);
  }

  // Translate click position into a buffer iterator
  int bx = 0, by = 0;
  gtk_text_view_window_to_buffer_coords(
      GTK_TEXT_VIEW(mv->tv), GTK_TEXT_WINDOW_WIDGET, (int)x, (int)y, &bx, &by);
  GtkTextIter it;
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(mv->tv), &it, bx, by);

  // In the combined view, clicking in the ASCII column should select ASCII
  // entry.
  const gboolean prefer_ascii = (mv->view_mode == 0 || mv->view_mode == 2);
  if (!mem_view_click_to_addr(mv, &it, prefer_ascii))
    return;

  // Update selection + keep focus in the mem viewer.
  mem_view_update_selection(mv);
  gtk_widget_grab_focus(mv->tv);
}

static void mem_view_set_byte(MemViewState *mv, guint16 addr, guint8 v) {
  if (!mv || !mv->work)
    return;
  if (mv->work[addr] == v)
    return;
  mv->work[addr] = v;
  mv->dirty = TRUE;
  mv->app->mem_dirty = TRUE;
}

/* Update just one line worth of text in the buffer for the row containing addr.
 */
static void mem_view_update_line(MemViewState *mv, guint16 addr) {
  if (!mv || !mv->tb)
    return;
  if (mv->view_mode != 0 && mv->view_mode != 3) {
    /* Non-HEX views don't have fixed row geometry; just repaint. */
    mem_view_refresh(mv);
    mem_view_update_selection(mv);
    return;
  }
  int line = mem_line_for_addr(addr);
  int base = line * 16;

  char ascii[17];
  for (int i = 0; i < 16; i++) {
    guint8 b = mv->work[base + i];
    ascii[i] = (b >= 32 && b <= 126) ? (char)b : '.';
  }
  ascii[16] = 0;

  char linebuf[96];
  int n = 0;
  n += snprintf(linebuf + n, sizeof(linebuf) - n, "%04X: ", base);
  for (int i = 0; i < 16; i++)
    n +=
        snprintf(linebuf + n, sizeof(linebuf) - n, "%02X ", mv->work[base + i]);
  n += snprintf(linebuf + n, sizeof(linebuf) - n, " |%s|\n", ascii);

  GtkTextIter a, b;
  gtk_text_buffer_get_iter_at_line(mv->tb, &a, line);
  b = a;
  gtk_text_iter_forward_to_line_end(&b);
  gtk_text_iter_forward_char(&b); /* include '\n' */

  gtk_text_buffer_begin_user_action(mv->tb);
  gtk_text_buffer_delete(mv->tb, &a, &b);
  gtk_text_buffer_insert(mv->tb, &a, linebuf, -1);
  gtk_text_buffer_end_user_action(mv->tb);
}

static void mem_view_free(MemViewState *mv) {
  if (!mv)
    return;
  /* Save last cursor address so next open resumes here */
  if (mv->app) {
    mv->app->mem_last_addr = mv->addr;
    mv->app->mem_last_addr_valid = TRUE;
  }
  if (mv->app && mv->app->mem_view == mv)
    mv->app->mem_view = NULL;
  mv->win = NULL;
  mv->tb = NULL;
  g_free(mv->orig);
  g_free(mv->work);
  if (mv->key_diag_fp) {
    fclose(mv->key_diag_fp);
    mv->key_diag_fp = NULL;
  }
  if (mv->disasm_offs)
    g_free(mv->disasm_offs);
  if (mv->disasm_start)
    g_free(mv->disasm_start);
  if (mv->disasm_prev)
    g_free(mv->disasm_prev);
  g_free(mv);
}

/* Cleanup callback when the memory-viewer window is destroyed. */
static void mem_view_destroy_cb(GtkWidget *w, gpointer user_data) {
  (void)w;
  MemViewState *mv = (MemViewState *)user_data;
  /* The window is gone; release state + clear the app pointer so Ctrl+M can
   * reopen. */
  mem_view_free(mv);
}

static void mem_view_finish_close(MemViewState *mv, gboolean keep_changes) {
  if (!mv)
    return;

  if (keep_changes) {
    memcpy(mv->app->mem_image, mv->work, 65536);
    mv->app->mem_valid = TRUE;
    mv->app->mem_dirty = FALSE;
  } else {
    /* discard => leave app->mem_image as-is */
  }

  mv->dirty = FALSE; /* so close-request won't prompt again */
  /* Destroy triggers mem_view_destroy_cb, which frees the state. */
  gtk_window_destroy(GTK_WINDOW(mv->win));
}

static void mem_view_discard_confirm_response(GtkDialog *dlg, int resp,
                                              gpointer ud) {
  MemViewState *mv = (MemViewState *)ud;
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (!mv)
    return;

  if (resp == GTK_RESPONSE_REJECT) {
    mem_view_finish_close(mv, FALSE);
  }
  /* else cancelled => keep window open */
}

static void mem_view_keep_discard_response(GtkDialog *dlg, int resp,
                                           gpointer ud) {
  MemViewState *mv = (MemViewState *)ud;
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (!mv)
    return;

  if (resp == GTK_RESPONSE_ACCEPT) {
    mem_view_finish_close(mv, TRUE);
    return;
  }

  if (resp == GTK_RESPONSE_REJECT) {
    GtkWidget *q2 = gtk_message_dialog_new(
        GTK_WINDOW(mv->win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE, "Discard changes?\nAll changes will be lost.");
    gtk_dialog_add_button(GTK_DIALOG(q2), "No", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(q2), "Yes, discard", GTK_RESPONSE_REJECT);
    dialog_add_default_keys(q2, GTK_RESPONSE_CANCEL, GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(q2), GTK_RESPONSE_CANCEL);
    g_signal_connect(q2, "response",
                     G_CALLBACK(mem_view_discard_confirm_response), mv);
    gtk_window_present(GTK_WINDOW(q2));
    return;
  }

  /* cancel/delete => keep open */
}

static gboolean mem_view_close_request(GtkWindow *w, gpointer user_data) {
  (void)w;
  MemViewState *mv = (MemViewState *)user_data;
  if (!mv)
    return FALSE;

  if (mv->dirty) {
    GtkWidget *q = gtk_message_dialog_new(
        GTK_WINDOW(mv->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "Keep changes to memory?");
    gtk_dialog_add_button(GTK_DIALOG(q), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(q), "Discard", GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(q), "Keep", GTK_RESPONSE_ACCEPT);
    dialog_add_default_keys(q, GTK_RESPONSE_ACCEPT, GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(q), GTK_RESPONSE_ACCEPT);
    g_signal_connect(q, "response", G_CALLBACK(mem_view_keep_discard_response),
                     mv);
    gtk_window_present(GTK_WINDOW(q));
    return TRUE; /* stop close for now */
  }

  return FALSE; /* allow close */
}

static int disasm_inst_len(MemViewState *mv, uint16_t pc) {
  char tmp[64];
  int len = (mv->cpu == ASM_CPU_65816)
                ? asm_disasm_65816(mv->work, pc, tmp, sizeof(tmp))
            : (mv->cpu == ASM_CPU_65C02)
                ? asm_disasm_65c02(mv->work, pc, tmp, sizeof(tmp))
                : asm_disasm_6502(mv->work, pc, tmp, sizeof(tmp));
  if (len < 1)
    len = 1;
  return len;
}

static uint16_t disasm_inst_start(MemViewState *mv, uint16_t addr) {
  /* Prefer the map built during rendering (reliable), else fall back to a small
   * heuristic. */
  if (mv->disasm_start) {
    uint16_t s = mv->disasm_start[addr];
    if (s != 0xFFFF)
      return s;
  }

  uint16_t best = addr;
  for (int back = 0; back <= 2; back++) {
    uint16_t cand = (uint16_t)(addr - (uint16_t)back);
    int len = disasm_inst_len(mv, cand);
    if ((uint16_t)(cand + (uint16_t)len) > addr) {
      best = cand;
      break;
    }
  }
  return best;
}

static void mem_view_keydiag(MemViewState *mv, const char *where, guint keyval,
                             guint keycode, GdkModifierType state,
                             gboolean handled) {
  if (!mv)
    return;

  const char *name = gdk_keyval_name(keyval);
  if (!name)
    name = "(null)";

  /* Update title so we can see what key GTK is delivering, even if stdout isn't
   * visible. */
  snprintf(mv->key_diag, sizeof(mv->key_diag),
           "%s: %s (0x%X) code=%u state=0x%X %s", where ? where : "key", name,
           (unsigned)keyval, (unsigned)keycode, (unsigned)state,
           handled ? "[handled]" : "[pass]");
  mem_view_update_title(mv);

  /* Also log to a temp file (useful when app isn't launched from a terminal).
   */
  if (!mv->key_diag_fp) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", g_get_tmp_dir(),
             "6502caEd_keydiag.log");
    mv->key_diag_fp = fopen(path, "a");
  }
  if (mv->key_diag_fp) {
    fprintf(mv->key_diag_fp, "%s\n", mv->key_diag);
    fflush(mv->key_diag_fp);
  }
}

static gboolean mem_view_key(GtkEventControllerKey *c, guint keyval,
                             guint keycode, GdkModifierType state,
                             gpointer user_data) {
  (void)c;
  MemViewState *mv = (MemViewState *)user_data;
  if (!mv)
    return GDK_EVENT_PROPAGATE;

  /* Always record what key we got (even if we don't end up handling it). */
  mem_view_keydiag(mv, "memview", keyval, keycode, state, FALSE);

  gboolean shift = (state & GDK_SHIFT_MASK) != 0;

  if (keyval == GDK_KEY_Escape) {
    if (mv->addr_mode) {
      mv->addr_mode = FALSE;
      mv->addr_digits = 0;
      mv->hex_nibble = 0;
      mem_view_update_title(mv);
      mem_view_update_selection(mv);
      mem_view_keydiag(mv, "memview", keyval, keycode, state, TRUE);
      return GDK_EVENT_STOP;
    }
    gtk_window_close(GTK_WINDOW(mv->win));
    mem_view_keydiag(mv, "memview", keyval, keycode, state, TRUE);
    return GDK_EVENT_STOP;
  }

  /* ADT-style: TAB cycles view modes (HEX+ASCII, DISASM, ASCII, BYTES). */
  if (keyval == GDK_KEY_Tab) {
    mv->addr_mode = FALSE;
    mv->addr_digits = 0;
    mv->hex_nibble = 0;
    mv->view_mode = (mv->view_mode + 1) % 4;
    /* In DISASM view we keep editing as hex-only (ADT-like). */
    if (mv->view_mode == 1)
      mv->hex_mode = TRUE;
    mem_view_refresh(mv);
    mem_view_update_title(mv);
    mem_view_update_selection(mv);
    mem_view_keydiag(mv, "memview", keyval, keycode, state, TRUE);
    return GDK_EVENT_STOP;
  }

  /* F1 enters "goto address" entry mode (plain 'g' is a valid ASCII byte,
   * and Ctrl+G conflicts with other uses). */
  if (keyval == GDK_KEY_F1) {
    mv->addr_mode = TRUE;
    mv->addr_entry = mv->addr;
    mv->addr_digits = 0;
    mv->hex_nibble = 0;
    mem_view_update_title(mv);
    mem_view_update_selection(mv);
    return GDK_EVENT_STOP;
  }

  /* Address entry mode */
  if (mv->addr_mode) {
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
      mv->addr = mv->addr_entry;
      mv->addr_mode = FALSE;
      mv->hex_nibble = 0;
      mv->addr_digits = 0;
      /* Leaving goto mode should immediately clear the header/title prompt. */
      mem_view_update_title(mv);
      mem_view_update_selection(mv);
      mem_view_keydiag(mv, "memview", keyval, keycode, state, TRUE);
      return GDK_EVENT_STOP;
    }
    if (keyval == GDK_KEY_BackSpace) {
      if (mv->addr_digits > 0)
        mv->addr_digits--;
      /* simple backspace: shift right by one nibble */
      mv->addr_entry = (guint16)(mv->addr_entry >> 4);
      mem_view_update_title(mv);
      mem_view_keydiag(mv, "memview", keyval, keycode, state, TRUE);
      return GDK_EVENT_STOP;
    }

    int v = -1;
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
      v = (int)(keyval - GDK_KEY_0);
    else if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_f)
      v = 10 + (int)(keyval - GDK_KEY_a);
    else if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_F)
      v = 10 + (int)(keyval - GDK_KEY_A);

    if (v >= 0) {
      mv->addr_entry =
          (guint16)(((mv->addr_entry << 4) | (guint16)(v & 0xF)) & 0xFFFF);
      if (mv->addr_digits < 4)
        mv->addr_digits++;
      mem_view_update_title(mv);
      return GDK_EVENT_STOP;
    }

    /* Any other key cancels address entry and is processed normally. */
    mv->addr_mode = FALSE;
    mv->addr_digits = 0;
    mv->hex_nibble = 0;
    mem_view_update_title(mv);
    /* fallthrough */
  }

  /*
    END key can arrive as:
      - GDK_KEY_End (main End)
      - GDK_KEY_KP_End (keypad End when NumLock is off)
      - GDK_KEY_KP_1  (keypad 1 when NumLock is on; physically shares the End
    key)
  */
  if (keyval == GDK_KEY_End || keyval == GDK_KEY_KP_End ||
      keyval == GDK_KEY_KP_1) {
    /* In DISASM view, keep editing as hex-only (ADT-like). */
    if (mv->view_mode != 1) {
      mv->hex_mode = !mv->hex_mode;
      mv->hex_nibble = 0;
      mem_view_update_selection(mv);
    }
    mem_view_keydiag(mv, "memview", keyval, keycode, state, TRUE);
    return GDK_EVENT_STOP;
  }

  /* navigation */
  guint16 a = mv->addr;
  if (keyval == GDK_KEY_Left)
    a = (guint16)(a - 1);
  if (keyval == GDK_KEY_Right)
    a = (guint16)(a + 1);
  if (keyval == GDK_KEY_Up) {
    if (shift) {
      a = (guint16)(a - 0x100);
    } else if (mv->view_mode == 1) {
      /* DISASM view: move by instruction, preserving byte index where possible
       * (ADT-like). */
      const guint16 cur = mv->addr;
      const guint16 cur_start = disasm_inst_start(mv, cur);
      int idx = (int)((guint16)(cur - cur_start));

      guint16 prev_start = cur_start;
      if (mv->disasm_prev && cur_start != 0xFFFF) {
        const guint16 p = mv->disasm_prev[cur_start];
        if (p != 0xFFFF)
          prev_start = p;
      } else if (cur_start > 0) {
        prev_start = disasm_inst_start(mv, (guint16)(cur_start - 1));
      }

      const int prev_len = disasm_inst_len(mv, prev_start);
      if (idx >= prev_len)
        idx = prev_len - 1;
      if (idx < 0)
        idx = 0;
      a = (guint16)(prev_start + (guint16)idx);
    } else {
      a = (guint16)(a - 16);
    }
  }
  if (keyval == GDK_KEY_Down) {
    if (shift) {
      a = (guint16)(a + 0x100);
    } else if (mv->view_mode == 1) {
      /* DISASM view: move by instruction, preserving byte index where possible
       * (ADT-like). */
      const guint16 cur = mv->addr;
      const guint16 cur_start = disasm_inst_start(mv, cur);
      int idx = (int)((guint16)(cur - cur_start));

      const int cur_len = disasm_inst_len(mv, cur_start);
      guint16 next_start = (guint16)(cur_start + (guint16)cur_len);
      const int next_len = disasm_inst_len(mv, next_start);
      if (idx >= next_len)
        idx = next_len - 1;
      if (idx < 0)
        idx = 0;
      a = (guint16)(next_start + (guint16)idx);
    } else {
      a = (guint16)(a + 16);
    }
  }
  if (keyval == GDK_KEY_Page_Up)
    a = (guint16)(a - 0x100);
  if (keyval == GDK_KEY_Page_Down)
    a = (guint16)(a + 0x100);
  if (a != mv->addr) {
    mv->addr = a;
    mv->hex_nibble = 0;
    mem_view_update_selection(mv);
    return GDK_EVENT_STOP;
  }

  /* input */
  if (mv->hex_mode) {
    int v = -1;
    if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9)
      v = (int)(keyval - GDK_KEY_0);
    else if (keyval >= GDK_KEY_a && keyval <= GDK_KEY_f)
      v = 10 + (int)(keyval - GDK_KEY_a);
    else if (keyval >= GDK_KEY_A && keyval <= GDK_KEY_F)
      v = 10 + (int)(keyval - GDK_KEY_A);

    if (v >= 0) {
      guint16 line_addr = mv->addr;
      guint8 cur = mv->work[mv->addr];
      if (mv->hex_nibble == 0) {
        cur = (guint8)((cur & 0x0F) | (v << 4));
        mem_view_set_byte(mv, mv->addr, cur);
        mv->hex_nibble = 1;
      } else {
        cur = (guint8)((cur & 0xF0) | (v & 0x0F));
        mem_view_set_byte(mv, mv->addr, cur);
        mv->hex_nibble = 0;
        mv->addr = (guint16)(mv->addr + 1);
      }

      /* In DISASM mode we rebuild the whole view on each edit. That can cause
         visible jumpiness (buffer reset + auto-scroll). Preserve the current
         scroll position while typing bytes. */
      if (mv->view_mode == 1) {
        GtkAdjustment *vadj =
            gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(mv->tv));
        const double vpos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

        mv->suppress_scroll = TRUE;
        mem_view_refresh(mv);
        mem_view_update_selection(mv);
        mv->suppress_scroll = FALSE;

        if (vadj) {
          const double upper = gtk_adjustment_get_upper(vadj);
          const double page = gtk_adjustment_get_page_size(vadj);
          double maxv = upper - page;
          if (maxv < 0.0)
            maxv = 0.0;
          double nv = vpos;
          if (nv < 0.0)
            nv = 0.0;
          if (nv > maxv)
            nv = maxv;
          gtk_adjustment_set_value(vadj, nv);
        }
      } else {
        mem_view_update_line(mv, line_addr);
        mem_view_update_selection(mv);
      }
      return GDK_EVENT_STOP;
    }
  } else {
    /* ASCII entry: use Unicode -> byte for basic ASCII */
    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (uc >= 32 && uc <= 126) {
      guint16 line_addr = mv->addr;
      mem_view_set_byte(mv, mv->addr, (guint8)uc);
      mem_view_update_line(mv, line_addr);
      mv->addr = (guint16)(mv->addr + 1);
      mem_view_update_selection(mv);
      return GDK_EVENT_STOP;
    }
  }

  return GDK_EVENT_PROPAGATE;
}

static void mem_view_refresh(MemViewState *mv) {
  if (!mv || !mv->tb || !mv->work)
    return;

  /* If leaving DISASM view, drop the address->offset map. */
  if (mv->view_mode != 1) {
    if (mv->disasm_offs) {
      g_free(mv->disasm_offs);
      mv->disasm_offs = NULL;
    }
    if (mv->disasm_start) {
      g_free(mv->disasm_start);
      mv->disasm_start = NULL;
    }
    if (mv->disasm_prev) {
      g_free(mv->disasm_prev);
      mv->disasm_prev = NULL;
    }
  }

  if (mv->view_mode == 1) {
    /* DISASM view: one instruction per line, ADT-style.
       We also build a 64K address->text-offset map so the cursor highlight can
       follow byte-wise navigation. */
    if (mv->disasm_offs) {
      g_free(mv->disasm_offs);
      mv->disasm_offs = NULL;
    }
    if (mv->disasm_start) {
      g_free(mv->disasm_start);
      mv->disasm_start = NULL;
    }
    if (mv->disasm_prev) {
      g_free(mv->disasm_prev);
      mv->disasm_prev = NULL;
    }
    mv->disasm_offs = (uint32_t *)g_malloc0_n(65536, sizeof(uint32_t));
    mv->disasm_start = (uint16_t *)g_malloc_n(65536, sizeof(uint16_t));
    mv->disasm_prev = (uint16_t *)g_malloc_n(65536, sizeof(uint16_t));
    memset(mv->disasm_start, 0xFF, 65536 * sizeof(uint16_t));
    memset(mv->disasm_prev, 0xFF, 65536 * sizeof(uint16_t));

    GString *gs = g_string_sized_new(80 * 16384);
    uint16_t prev_pc = 0xFFFF;
    for (guint32 pc = 0; pc < 65536;) {
      const uint32_t line_off = (uint32_t)gs->len;

      char opbuf[64];
      int len =
          (mv->cpu == ASM_CPU_65816)
              ? asm_disasm_65816(mv->work, (guint16)pc, opbuf, sizeof(opbuf))
          : (mv->cpu == ASM_CPU_65C02)
              ? asm_disasm_65c02(mv->work, (guint16)pc, opbuf, sizeof(opbuf))
              : asm_disasm_6502(mv->work, (guint16)pc, opbuf, sizeof(opbuf));
      if (len < 1)
        len = 1;

      char bytes[16] = "";
      {
        /* Up to 3 bytes shown like the ADT screenshots. */
        int n = 0;
        for (int i = 0; i < 3; i++) {
          if (i < len && (pc + (guint32)i) < 65536)
            n += snprintf(bytes + n, sizeof(bytes) - n, "%02X ",
                          mv->work[pc + i]);
          else
            n += snprintf(bytes + n, sizeof(bytes) - n, "   ");
        }
      }

      /* Map each byte of the instruction to the 2 hex digits in the bytes
         column. Line format is: "AAAA??????" then bytes, so byte0 starts at
         column 6, each byte is 3 columns ("HH???"). */
      for (int i = 0; i < len && (pc + (guint32)i) < 65536; i++) {
        mv->disasm_offs[pc + (guint32)i] = line_off + 6u + (uint32_t)i * 3u;
        mv->disasm_start[pc + (guint32)i] = (uint16_t)pc;
      }
      mv->disasm_prev[pc] = prev_pc;
      prev_pc = (uint16_t)pc;

      char linebuf[128];
      int n = snprintf(linebuf, sizeof(linebuf), "%04X  %s %s\n", (unsigned)pc,
                       bytes, opbuf);
      g_string_append_len(gs, linebuf, n);
      pc += (guint32)len;
    }

    gtk_text_buffer_set_text(mv->tb, gs->str, (int)gs->len);
    g_string_free(gs, TRUE);
    mem_view_update_selection(mv);
    return;
  }

  if (mv->view_mode == 2) {
    /* ASCII view: wider rows (32 bytes). */
    const int bpl = 32;
    GString *gs = g_string_sized_new(96 * (65536 / bpl));
    for (int base = 0; base < 65536; base += bpl) {
      char ascii[bpl + 1];
      for (int i = 0; i < bpl; i++) {
        guint8 b = mv->work[base + i];
        ascii[i] = (b >= 32 && b <= 126) ? (char)b : '.';
      }
      ascii[bpl] = 0;
      char linebuf[160];
      int n = snprintf(linebuf, sizeof(linebuf), "%04X  %s\n", base, ascii);
      g_string_append_len(gs, linebuf, n);
    }
    gtk_text_buffer_set_text(mv->tb, gs->str, (int)gs->len);
    g_string_free(gs, TRUE);
    mem_view_update_selection(mv);
    return;
  }

  /* BYTES (hex only, ADT-like) */
  if (mv->view_mode == 3) {
    GString *gs = g_string_sized_new(80 * 4096);
    for (int base = 0; base < 65536; base += 16) {
      char linebuf[96];
      int n = 0;
      n += snprintf(linebuf + n, sizeof(linebuf) - n, "%04X: ", base);
      for (int i = 0; i < 16; i++)
        n += snprintf(linebuf + n, sizeof(linebuf) - n, "%02X ",
                      mv->work[base + i]);
      n += snprintf(linebuf + n, sizeof(linebuf) - n, "\n");
      g_string_append_len(gs, linebuf, n);
    }
    gtk_text_buffer_set_text(mv->tb, gs->str, (int)gs->len);
    g_string_free(gs, TRUE);
    mem_view_update_selection(mv);
    return;
  }

  /* HEX+ASCII (default) */
  GString *gs = g_string_sized_new(80 * 4096);
  for (int base = 0; base < 65536; base += 16) {
    char ascii[17];
    for (int i = 0; i < 16; i++) {
      guint8 b = mv->work[base + i];
      ascii[i] = (b >= 32 && b <= 126) ? (char)b : '.';
    }
    ascii[16] = 0;
    char linebuf[96];
    int n = 0;
    n += snprintf(linebuf + n, sizeof(linebuf) - n, "%04X: ", base);
    for (int i = 0; i < 16; i++)
      n += snprintf(linebuf + n, sizeof(linebuf) - n, "%02X ",
                    mv->work[base + i]);
    n += snprintf(linebuf + n, sizeof(linebuf) - n, " |%s|\n", ascii);
    g_string_append_len(gs, linebuf, n);
  }
  gtk_text_buffer_set_text(mv->tb, gs->str, (int)gs->len);
  g_string_free(gs, TRUE);
  mem_view_update_selection(mv);
}

/* Action handler: open or bring to front the memory viewer window. */
static void action_memory_viewer(GSimpleAction *a, GVariant *p,
                                 gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;

  /* If already open, present it. */
  if (s->mem_view) {
    MemViewState *mv = (MemViewState *)s->mem_view;
    gtk_window_present(GTK_WINDOW(mv->win));
    return;
  }

  MemViewState *mv = g_new0(MemViewState, 1);
  mv->app = s;
  mv->cpu = s->mem_cpu;
  mv->orig = (guint8 *)g_malloc(65536);
  mv->work = (guint8 *)g_malloc(65536);

  if (!s->mem_valid)
    memset(s->mem_image, 0, 65536);
  memcpy(mv->orig, s->mem_image, 65536);
  memcpy(mv->work, s->mem_image, 65536);

  /* Initial address priority:
     1. Last address used in a previous viewer session this run
     2. Start address of last successful assembly (min_written)
     3. 0x0000 */
  if (s->mem_last_addr_valid)
    mv->addr = s->mem_last_addr;
  else if (s->mem_asm_valid)
    mv->addr = s->mem_asm_start;
  else
    mv->addr = 0;
  mv->hex_mode = FALSE;
  mv->hex_nibble = 0;
  mv->addr_mode = FALSE;
  mv->addr_entry = 0;
  mv->addr_digits = 0;
  mv->dirty = FALSE;
  mv->view_mode = 0;

  mv->win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(mv->win), "Memory Viewer");
  gtk_window_set_default_size(GTK_WINDOW(mv->win), 820, 640);
  gtk_window_set_transient_for(GTK_WINDOW(mv->win), s->win);
  apply_window_dark_hint(mv->win, s->dark_mode);
  if (s->app)
    gtk_application_add_window(s->app, GTK_WINDOW(mv->win));

  mv->key_diag[0] = 0;
  mv->key_diag_fp = NULL;
  {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", g_get_tmp_dir(),
             "6502caEd_keydiag.log");
    mv->key_diag_fp = fopen(path, "a");
    if (mv->key_diag_fp) {
      setvbuf(mv->key_diag_fp, NULL, _IOLBF, 0);
      fprintf(mv->key_diag_fp, "\n--- keydiag start ---\n");
      fflush(mv->key_diag_fp);
    }
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(mv->win), vbox);

  mv->hdr = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(mv->hdr), 0.0f);
  gtk_widget_add_css_class(mv->hdr, "monospace");
  gtk_widget_set_margin_start(mv->hdr, 8);
  gtk_widget_set_margin_end(mv->hdr, 8);
  gtk_widget_set_margin_top(mv->hdr, 6);
  gtk_widget_set_margin_bottom(mv->hdr, 6);
  gtk_box_append(GTK_BOX(vbox), mv->hdr);

  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_widget_set_hexpand(sc, TRUE);
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_box_append(GTK_BOX(vbox), sc);

  mv->tv = gtk_text_view_new();
  gtk_widget_set_hexpand(mv->tv, TRUE);
  gtk_widget_set_vexpand(mv->tv, TRUE);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(mv->tv), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(mv->tv), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(mv->tv), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(mv->tv), GTK_WRAP_NONE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), mv->tv);

  mv->tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(mv->tv));
  mv->tag_sel_hex = gtk_text_buffer_create_tag(
      mv->tb, "sel_hex", "background", "#3a6ea5", "foreground", "white", NULL);
  mv->tag_sel_ascii =
      gtk_text_buffer_create_tag(mv->tb, "sel_ascii", "background", "#3a6ea5",
                                 "foreground", "white", NULL);

  GtkEventController *kc = gtk_event_controller_key_new();
  /*
   * End/Home/etc can be consumed by GtkTextView's built-in key bindings
   * (even when non-editable). If we attach our controller in the default
   * bubble phase, GtkTextView may handle the key before we see it.
   *
   * Use CAPTURE phase so we always receive navigation keys like End.
   */
  gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
  g_signal_connect(kc, "key-pressed", G_CALLBACK(mem_view_key), mv);
  gtk_widget_add_controller(mv->tv, kc);

  /* Also attach a controller to the window: some environments (or IM/key
     bindings) can swallow keys before they reach our view. This lets us confirm
     whether the End key is arriving at all. */
  GtkEventController *kcw = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(kcw, GTK_PHASE_CAPTURE);
  g_signal_connect(kcw, "key-pressed", G_CALLBACK(mem_view_key), mv);
  gtk_widget_add_controller(GTK_WIDGET(mv->win), kcw);

  // Mouse: click to move the byte cursor (and optionally pick ascii/hex side).
  // This makes navigation feel much more like ADT's MEX viewer.
  GtkGesture *gc = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), 0); /* any button */
  g_signal_connect(gc, "pressed", G_CALLBACK(mem_view_click_pressed_cb), mv);
  gtk_widget_add_controller(mv->tv, GTK_EVENT_CONTROLLER(gc));

  g_signal_connect(mv->win, "close-request", G_CALLBACK(mem_view_close_request),
                   mv);
  g_signal_connect(mv->win, "destroy", G_CALLBACK(mem_view_destroy_cb), mv);

  s->mem_view = mv;

  mem_view_update_title(mv);
  mem_view_refresh(mv);
  mem_view_update_selection(mv);
  gtk_window_present(GTK_WINDOW(mv->win));
  /* Ensure our TextView has focus so key events (like End) reach the
   * controller. */
  gtk_widget_grab_focus(mv->tv);
  /* Defer scroll-to-addr until after the window is mapped and allocated.
   * mem_view_update_selection above is a no-op for scrolling when the
   * widget is not yet realised. */
  if (mv->addr != 0) {
    int line = (int)(mv->addr >> 4);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_line(mv->tb, &it, line);
    mv->pending_scroll_offset = gtk_text_iter_get_offset(&it);
    mv->pending_scroll = TRUE;
    g_idle_add(mem_view_scroll_idle_cb, mv);
  }
}

/* ---------------- End memory viewer/editor ---------------- */

static void action_include_tree(GSimpleAction *a, GVariant *p, gpointer u) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)u;
  if (!s || !s->win) {
    return;
  }

  EditorDoc *d = app_get_active_doc(s);
  if (!d || !d->path || !d->path[0]) {
    show_info(s, "Includes Manager: this document has no file path yet.\n\n"
                 "Save the file first, then try again.");
    return;
  }

  it_css_ensure_installed();

  /* NOTE:
     We attempted a GTK4-native GtkListView + GtkTreeListModel migration, but
     it caused runtime crashes on some GTK4 builds (ABI / behaviour differences
     around TreeListModel callbacks and item types).

     For stability, keep the Includes Manager on the deprecated GtkTreeView/
     GtkTreeStore stack for now (it works reliably across GTK4 builds).

     We still get the features you want (double-click open, no duplicate tabs,
     persistent green for open files) and we fix the selection-colour override
     by explicitly enabling "foreground-set" on the renderer.
  */

  GtkWidget *dlg = gtk_dialog_new_with_buttons(
      "Includes Manager", GTK_WINDOW(s->win), GTK_DIALOG_MODAL, "Close",
      GTK_RESPONSE_CLOSE, NULL);

  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(sc, 700, 450);
  gtk_box_append(GTK_BOX(area), sc);

  /* Build from the *root* file, not necessarily the currently-selected tab. */
  char *root_path = it_find_root_for_active(s, d->path);
  GtkTreeStore *store = it_build_tree_store(s, root_path ? root_path : d->path);
  GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);

  GtkCellRenderer *r = gtk_cell_renderer_text_new();
  /* Critical: keep our foreground colour even when the row is selected. */
  g_object_set(r, "foreground-set", TRUE, NULL);

  GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
      "", r, "text", IT_COL_TEXT, "foreground", IT_COL_COLOR, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);

  IncludeTreeCtx *ctx = g_malloc0(sizeof(*ctx));
  ctx->s = s;
  ctx->dlg = dlg;
  ctx->tv = tv;
  ctx->store = store;
  ctx->model = GTK_TREE_MODEL(store);
  g_object_ref(ctx->model);

  /* Mark currently-open tabs green so reopening the dialog keeps state. */
  it_mark_open_rec(ctx, NULL);

  /* Start expanded so the tree is immediately usable. */
  gtk_tree_view_expand_all(GTK_TREE_VIEW(tv));

  /* Open on row activation (double click / enter). */
  ctx->row_activated_handler_id =
      g_signal_connect(tv, "row-activated", G_CALLBACK(it_row_activated), ctx);

  /* Also attach a gesture controller to make double-click reliable in GTK4. */
  GtkGesture *gc = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), 0); /* any button */
  g_signal_connect(gc, "pressed", G_CALLBACK(it_tree_gesture_pressed), ctx);
  gtk_widget_add_controller(tv, GTK_EVENT_CONTROLLER(gc));

  g_signal_connect(dlg, "destroy", G_CALLBACK(include_tree_on_destroy), ctx);
  g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_window_destroy),
                           dlg);
  gtk_window_present(GTK_WINDOW(dlg));

  g_free(root_path);
}

static void problems_clear(AppState *s) {
  if (!s || !s->problems_store)
    return;
  g_list_store_remove_all(s->problems_store);
}

static gboolean problems_focus_editor_idle(gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (s && s->view && s->buf) {
    /* Ensure the insertion mark is on screen before we focus the editor.
       This makes the jump location obvious even if the problems list keeps
       selection focus. */
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(
        GTK_TEXT_BUFFER(s->buf), &it,
        gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(s->buf)));
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &it, 0.15, TRUE, 0.0,
                                 0.1);
    gtk_widget_grab_focus(GTK_WIDGET(s->view));
  }
  return G_SOURCE_REMOVE;
}

static void problems_jump_to(AppState *s, const ProblemItem *p) {
  if (!s || !p)
    return;

  if (p->line == 0)
    return;

  /* If the diagnostic has a file attribution, ensure that file is the
     active tab before jumping to the line number. */
  if (p->file && p->file[0] && p->file[0] != '(') {
    const char *cur_path = s->path ? s->path : "";
    char *diag_canon = g_canonicalize_filename(p->file, NULL);
    char *cur_canon  = cur_path[0]
                           ? g_canonicalize_filename(cur_path, NULL)
                           : NULL;
    bool already_active = (diag_canon && cur_canon &&
                           strcmp(diag_canon, cur_canon) == 0);
    g_free(cur_canon);

    if (!already_active) {
      /* Look for an already-open tab. */
      int page = app_notebook_page_for_path(s, p->file);
      if (page >= 0) {
        gtk_notebook_set_current_page(GTK_NOTEBOOK(s->notebook), page);
        schedule_highlight(s);
      } else if (g_file_test(p->file, G_FILE_TEST_EXISTS)) {
        /* File not open yet — open it in a new tab. */
        EditorDoc *d = app_doc_new(s, NULL);
        if (d) {
          g_ptr_array_add(s->docs, d);
          load_file_into_doc(s, d, p->file);
          char *base = g_path_get_basename(p->file);
          app_add_doc_tab(s, d, base ? base : p->file);
          g_free(base);
          int last = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s->notebook)) - 1;
          if (last >= 0)
            gtk_notebook_set_current_page(GTK_NOTEBOOK(s->notebook), last);
          schedule_highlight(s);
        }
      }
      /* After switching/opening, s->buf now points to the new tab's buffer. */
    }
    g_free(diag_canon);
  }

  if (!s->buf || !s->view)
    return;

  GtkTextIter it;
  int line0 = (int)p->line - 1;
  if (line0 < 0)
    line0 = 0;
  gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(s->buf), &it, line0);
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(s->buf), &it);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &it, 0.2, FALSE, 0.0,
                               0.0);
  /* GtkColumnView often re-grabs focus after activation; defer editor focus
     to the next main-loop tick so the caret stays visible in the editor. */
  g_idle_add(problems_focus_editor_idle, s);
}

static void problems_activate_cb(GtkColumnView *cv, guint position,
                                 gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s || !s->problems_store)
    return;
  GObject *obj =
      g_list_model_get_item(G_LIST_MODEL(s->problems_store), position);
  if (!obj)
    return;
  problems_jump_to(s, (ProblemItem *)obj);
  g_object_unref(obj);
}

typedef struct {
  int col; /* 0=sev,1=file,2=line,3=msg */
} ProblemsColCtx;

static void problems_setup_cell(GtkListItemFactory *factory, GtkListItem *item,
                                gpointer user_data) {
  (void)factory;
  (void)user_data;
  GtkWidget *lbl = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
  gtk_list_item_set_child(item, lbl);
}

static void problems_bind_cell(GtkListItemFactory *factory, GtkListItem *item,
                               gpointer user_data) {
  (void)factory;
  ProblemsColCtx *ctx = (ProblemsColCtx *)user_data;
  GtkWidget *w = gtk_list_item_get_child(item);
  ProblemItem *p = (ProblemItem *)gtk_list_item_get_item(item);
  if (!GTK_IS_LABEL(w) || !p || !ctx)
    return;

  const char *text = "";
  char buf[64];
  buf[0] = '\0';

  switch (ctx->col) {
  case 0:
    text = p->severity ? p->severity : "";
    break;
  case 1:
    text = p->file ? p->file : "";
    break;
  case 2:
    if (p->line > 0) {
      g_snprintf(buf, sizeof(buf), "%u", p->line);
      text = buf;
    } else {
      text = "";
    }
    break;
  case 3:
  default:
    text = p->message ? p->message : "";
    break;
  }

  gtk_label_set_text(GTK_LABEL(w), text);
}

static GtkColumnViewColumn *problems_make_column(const char *title,
                                                 int which_col) {
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  ProblemsColCtx *ctx = (ProblemsColCtx *)g_malloc0(sizeof(ProblemsColCtx));
  ctx->col = which_col;

  g_signal_connect_data(factory, "setup", G_CALLBACK(problems_setup_cell), ctx,
                        NULL, 0);
  g_signal_connect_data(factory, "bind", G_CALLBACK(problems_bind_cell), ctx,
                        (GClosureNotify)g_free, 0);

  GtkColumnViewColumn *col = gtk_column_view_column_new(title, factory);
  gtk_column_view_column_set_resizable(col, TRUE);
  return col;
}

/* Forward declarations for problems panel helpers. */
static void problems_filter_changed(GObject *obj, GParamSpec *pspec,
                                    gpointer user_data);
static gboolean problems_paned_init_position_cb(gpointer user_data);

static GtkWidget *create_problems_panel(AppState *s) {
  /* Store of ProblemItem */
  s->problems_store = g_list_store_new(problem_item_get_type());
  GtkSelectionModel *sel = GTK_SELECTION_MODEL(
      gtk_single_selection_new(G_LIST_MODEL(s->problems_store)));

  GtkWidget *cv = gtk_column_view_new(sel);
  s->problems_view = cv;
  s->problems_sel = sel;

  gtk_column_view_append_column(GTK_COLUMN_VIEW(cv),
                                problems_make_column("Severity", 0));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(cv),
                                problems_make_column("File", 1));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(cv),
                                problems_make_column("Line", 2));
  gtk_column_view_append_column(GTK_COLUMN_VIEW(cv),
                                problems_make_column("Message", 3));

  g_signal_connect(cv, "activate", G_CALLBACK(problems_activate_cb), s);

  GtkEventController *problems_keys = gtk_event_controller_key_new();
  g_signal_connect(problems_keys, "key-pressed",
                   G_CALLBACK(problems_key_pressed_cb), s);
  gtk_widget_add_controller(cv, problems_keys);

  GtkGesture *problems_click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(problems_click), 0);
  g_signal_connect(problems_click, "pressed",
                   G_CALLBACK(problems_click_pressed_cb), s);
  gtk_widget_add_controller(cv, GTK_EVENT_CONTROLLER(problems_click));

  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), cv);
  /* Must expand so the list grows/shrinks when the splitter is dragged. */
  gtk_widget_set_vexpand(sc, TRUE);
  /* Default compact height (~3 rows). User can expand via splitter. */
  gtk_widget_set_size_request(sc, -1, 90);

  /* Header row */
  GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(hdr, 6);
  gtk_widget_set_margin_bottom(hdr, 6);
  gtk_widget_set_margin_start(hdr, 8);
  gtk_widget_set_margin_end(hdr, 8);

  GtkWidget *lbl = gtk_label_new("Problems");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_widget_set_hexpand(lbl, TRUE);

  /* Filter: All / Assemble / Editor */
  GtkWidget *dd = gtk_drop_down_new_from_strings(
      (const char *[]){"All", "Assemble", "Editor", NULL});
  s->problems_filter_dd = dd;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), (guint)s->problems_filter);
  g_signal_connect(dd, "notify::selected", G_CALLBACK(problems_filter_changed),
                   s);

  GtkWidget *btn_close = gtk_button_new_with_label("Hide");
  g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(problems_hide), s);

  gtk_box_append(GTK_BOX(hdr), lbl);
  gtk_box_append(GTK_BOX(hdr), dd);
  gtk_box_append(GTK_BOX(hdr), btn_close);

  GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(panel), hdr);
  gtk_box_append(GTK_BOX(panel), sc);

  /* Let the panel grow/shrink with the paned; minimum size is enforced
   * elsewhere. */
  gtk_widget_set_vexpand(panel, TRUE);

  GtkWidget *rev = gtk_revealer_new();
  s->problems_revealer = rev;
  gtk_revealer_set_transition_type(GTK_REVEALER(rev),
                                   GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
  gtk_revealer_set_reveal_child(GTK_REVEALER(rev), FALSE);
  gtk_revealer_set_child(GTK_REVEALER(rev), panel);
  gtk_widget_set_vexpand(rev, TRUE);
  return rev;
}

static void diags_clone_into(diags_t *dst, const diags_t *src) {
  if (!dst)
    return;
  diags_free(dst);
  diags_init(dst);
  if (!src || src->count == 0)
    return;
  dst->items = (diag_t *)calloc(src->count, sizeof(diag_t));
  if (!dst->items)
    return;
  dst->count = src->count;
  dst->cap = src->count;

  // Deep copy: diag_t owns its file string.
  for (size_t i = 0; i < src->count; ++i) {
    dst->items[i] = src->items[i];
    if (src->items[i].file) {
      dst->items[i].file = strdup(src->items[i].file);
    } else {
      dst->items[i].file = strdup("<input>");
    }
    if (!dst->items[i].file)
      dst->items[i].file = NULL;
  }
}

static void problems_refresh(AppState *s, gboolean force_show);
static gboolean problems_paned_init_position_cb(gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s)
    return G_SOURCE_REMOVE;
  if (!s->problems_paned) {
    s->problems_init_idle_id = 0;
    return G_SOURCE_REMOVE;
  }

  int h = gtk_widget_get_height(s->problems_paned);
  if (h <= 0)
    return G_SOURCE_CONTINUE;

  int pos = h - s->problems_default_end_px;
  if (pos < 0)
    pos = 0;

  gtk_paned_set_position(GTK_PANED(s->problems_paned), pos);
  s->problems_init_idle_id = 0;
  return G_SOURCE_REMOVE;
}

/* Forward declarations for paned-position and filter callbacks. */
static void problems_paned_position_notify(GObject *obj, GParamSpec *pspec,
                                           gpointer user_data);
static void problems_filter_changed(GObject *obj, GParamSpec *pspec,
                                    gpointer user_data);
/* Idle callback: run a full assembly pass and refresh the problems panel. */
static gboolean problems_refresh_idle_cb(gpointer ud) {
  AppState *s = (AppState *)ud;
  if (!s)
    return G_SOURCE_REMOVE;
  s->problems_refresh_idle_id = 0;
  gboolean force = s->problems_refresh_force_show_pending;
  s->problems_refresh_force_show_pending = FALSE;
  problems_refresh(s, force);
  return G_SOURCE_REMOVE;
}

/* Schedule a background problems refresh, debounced via idle callback. */
static void schedule_problems_refresh(AppState *s, gboolean force_show) {
  if (!s)
    return;
  if (force_show)
    s->problems_refresh_force_show_pending = TRUE;
  if (s->problems_refresh_idle_id != 0)
    return;
  s->problems_refresh_idle_id = g_idle_add(problems_refresh_idle_cb, s);
}
static void problems_filter_changed(GObject *obj, GParamSpec *pspec,
                                    gpointer user_data) {
  (void)obj;
  (void)pspec;
  AppState *s = (AppState *)user_data;
  if (!s || !s->problems_filter_dd)
    return;
  s->problems_filter =
      (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(s->problems_filter_dd));
  /* Do not force-open; just refresh list contents. */
  schedule_problems_refresh(s, FALSE);
}

static void problems_refresh_now(AppState *s) {
  if (!s)
    return;
  if (s->problems_refresh_idle_id) {
    g_source_remove(s->problems_refresh_idle_id);
    s->problems_refresh_idle_id = 0;
  }
  problems_refresh(s, FALSE);
}

static void problems_refresh(AppState *s, gboolean force_show) {
  if (!s)
    return;

  if (!s->problems_enabled) {
    if (s->problems_revealer)
      gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), FALSE);
    problems_clear(s);
    return;
  }

  problems_clear(s);

  /* Merge semantic (inline) + last assemble diags, de-duplicated. */
  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  const diags_t *sources[2] = {NULL, NULL};
  int scount = 0;
  if (s->problems_filter == 1) { /* Assemble */
    sources[scount++] = &s->asm_diags;
  } else if (s->problems_filter == 2) { /* Editor */
    sources[scount++] = &s->sem_diags;
  } else { /* All */
    sources[scount++] = &s->sem_diags;
    sources[scount++] = &s->asm_diags;
  }

  for (int si = 0; si < scount; si++) {
    const diags_t *d = sources[si];
    if (!d || d->count == 0)
      continue;
    for (size_t i = 0; i < d->count; i++) {
      const diag_t *x = &d->items[i];
      const char *sev = (x->sev == DIAG_ERROR) ? "Error" : "Warning";
      const char *file = x->file ? x->file : "(buffer)";
      const char *msg = x->msg ? x->msg : "";
      char *key =
          g_strdup_printf("%s|%s|%u|%u|%s", sev, file, x->line, x->col, msg);
      if (g_hash_table_contains(seen, key)) {
        g_free(key);
        continue;
      }
      g_hash_table_add(seen, key);

      ProblemItem *pi =
          problem_item_new(sev, file, (guint)x->line, (guint)x->col, msg);
      g_list_store_append(s->problems_store, pi);
      g_object_unref(pi);
    }
  }

  g_hash_table_destroy(seen);

  guint n = s->problems_store
                ? g_list_model_get_n_items(G_LIST_MODEL(s->problems_store))
                : 0;
  if (n == 0) {
    /* No problems left: collapse the panel and give space back to the editor.
       This is an auto-hide, so we must NOT mark the panel as user-hidden. */
    problems_autohide(s);
    return;
  }

  gboolean show = (force_show || !s->problems_user_hidden);

  /* If the panel was auto-collapsed (no problems) we need to *restore* the
     paned position when problems reappear. Merely revealing the child isn't
     enough because the paned will still be stuck at the collapsed position. */
  if (show) {
    if (s->problems_collapsed || !s->problems_visible) {
      problems_show(s);
    } else {
      gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), TRUE);
      s->problems_visible = TRUE;
    }
  } else {
    gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), FALSE);
    s->problems_visible = FALSE;
  }

  /* After Assemble, it is useful to jump to the first issue automatically.
     Keep focus in the editor so the cursor stays visible. */
  if (show && s->problems_autojump_pending) {
    s->problems_autojump_pending = FALSE;
    if (s->problems_sel)
      gtk_single_selection_set_selected(GTK_SINGLE_SELECTION(s->problems_sel),
                                        0);
    /* Reuse the same activation path as clicking a row. */
    if (s->problems_view)
      problems_activate_cb(GTK_COLUMN_VIEW(s->problems_view), 0, s);
  }
}

/* Keep Problems pane size sensible using the GtkPaned "position" property.
   We avoid the removed/invalid "size-allocate" signal on GTK4 widgets.

   Rules:
   - When Problems are hidden: keep the end pane tiny.
   - When shown: enforce a small minimum end height and a max of ~1/2 window
   height.
*/
static void problems_paned_position_notify(GObject *obj, GParamSpec *pspec,
                                           gpointer user_data) {
  (void)pspec;
  AppState *s = (AppState *)user_data;
  if (!s || !GTK_IS_PANED(obj))
    return;

  GtkPaned *paned = GTK_PANED(obj);
  GtkWidget *w = GTK_WIDGET(paned);
  int height = gtk_widget_get_height(w);
  if (height <= 0)
    return;

  if (!s->problems_enabled) {
    /* Feature disabled: keep collapsed. */
    if (!s->problems_clamping) {
      s->problems_clamping = TRUE;
      gtk_paned_set_position(paned, height);
      s->problems_clamping = FALSE;
    }
    return;
  }

  gboolean visible =
      s->problems_revealer
          ? gtk_revealer_get_reveal_child(GTK_REVEALER(s->problems_revealer))
          : FALSE;

  int pos = gtk_paned_get_position(paned);

  if (!visible) {
    /* Hidden: collapse end pane. */
    int want = height;
    if (pos != want && !s->problems_clamping) {
      s->problems_clamping = TRUE;
      gtk_paned_set_position(paned, want);
      s->problems_clamping = FALSE;
    }
    return;
  }

  int min_end = (s->problems_min_end_px > 0) ? s->problems_min_end_px : 80;
  /* Clamp against current size to avoid negative/overflow positions. */
  if (min_end > height)
    min_end = height;
  int max_end = height / 2;
  if (max_end < min_end)
    max_end = min_end;
  if (max_end > height)
    max_end = height;

  /* end_h = height - pos */
  int end_h = height - pos;
  if (end_h < min_end)
    pos = height - min_end;
  if (end_h > max_end)
    pos = height - max_end;

  if (pos < 0)
    pos = 0;
  if (pos > height)
    pos = height;

  if (!s->problems_clamping) {
    s->problems_clamping = TRUE;
    gtk_paned_set_position(paned, pos);
    s->problems_clamping = FALSE;
  }
}

static void problems_set_from_diags(AppState *s, const diags_t *d) {
  if (!s)
    return;
  /* Treat explicit assemble results as authoritative for the Assemble channel.
   */
  diags_clone_into(&s->asm_diags, d);
  /* Assemble should reopen the panel even if the user previously hid it. */
  schedule_problems_refresh(s, TRUE);
}

// Forward decls (keep file self-contained; avoid implicit-decl build failures)
static void load_file_into_doc(AppState *s, EditorDoc *d, const char *path);
static gboolean save_buffer_to_file(AppState *s, const char *path,
                                    GError **err);
static void update_title(AppState *s);

static void session_save(AppState *s) {
  if (!s || !s->reopen_last_file) {
    char path[PATH_MAX];
    gtk_session_path(path, sizeof(path));
    remove(path);
    return;
  }
  char path[PATH_MAX];
  gtk_session_path(path, sizeof(path));
  gchar *dir = g_path_get_dirname(path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fprintf(f, "; 6502caEd session\n");
  if (s->docs) {
    for (guint i = 0; i < s->docs->len; i++) {
      EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, i);
      if (d && d->path && d->path[0])
        fprintf(f, "file=%s\n", d->path);
    }
  }
  fclose(f);
}

static void doc_update_tab_label(AppState *s, EditorDoc *d); /* fwd */

/* ---- Recent files ---- */

static void set_status(AppState *s, const char *msg); /* fwd */

static void recent_files_add(AppState *s, const char *path) {
  if (!s || !path || !path[0])
    return;
  /* Remove existing entry for this path */
  for (int i = 0; i < RECENT_FILES_MAX; i++) {
    if (s->recent_files[i] && strcmp(s->recent_files[i], path) == 0) {
      g_free(s->recent_files[i]);
      /* Shift down */
      for (int j = i; j < RECENT_FILES_MAX - 1; j++)
        s->recent_files[j] = s->recent_files[j + 1];
      s->recent_files[RECENT_FILES_MAX - 1] = NULL;
      break;
    }
  }
  /* Free last slot and shift up to make room at front */
  g_free(s->recent_files[RECENT_FILES_MAX - 1]);
  for (int i = RECENT_FILES_MAX - 1; i > 0; i--)
    s->recent_files[i] = s->recent_files[i - 1];
  s->recent_files[0] = g_strdup(path);
}

static void recent_files_save(AppState *s) {
  char path[PATH_MAX];
  gtk_session_path(path, sizeof(path));
  /* Append recent= lines to session file (session_save writes file= lines
     first; we re-open and append) */
  FILE *f = fopen(path, "a");
  if (!f)
    return;
  for (int i = 0; i < RECENT_FILES_MAX; i++)
    if (s->recent_files[i] && s->recent_files[i][0])
      fprintf(f, "recent=%s\n", s->recent_files[i]);
  fclose(f);
}

static void recent_files_load(AppState *s) {
  char spath[PATH_MAX];
  gtk_session_path(spath, sizeof(spath));
  gchar *contents = NULL;
  if (!g_file_get_contents(spath, &contents, NULL, NULL))
    return;
  gchar **lines = g_strsplit(contents, "\n", -1);
  int n = 0;
  for (gchar **pp = lines; pp && *pp && n < RECENT_FILES_MAX; pp++) {
    gchar *line = g_strstrip(*pp);
    if (!g_str_has_prefix(line, "recent="))
      continue;
    const char *fpath = line + 7;
    if (!fpath[0])
      continue;
    s->recent_files[n++] = g_strdup(fpath);
  }
  g_strfreev(lines);
  g_free(contents);
}

/* Rebuild the File > Open Recent sub-menu from the MRU list. */
static void recent_menu_rebuild(AppState *s) {
  if (!s || !s->recent_menu)
    return;
  /* Clear existing items */
  while (g_menu_model_get_n_items(G_MENU_MODEL(s->recent_menu)) > 0)
    g_menu_remove(s->recent_menu, 0);
  gboolean any = FALSE;
  for (int i = 0; i < RECENT_FILES_MAX; i++) {
    if (!s->recent_files[i] || !s->recent_files[i][0])
      continue;
    gchar *base = g_path_get_basename(s->recent_files[i]);
    /* Label: "basename  (dir)" */
    gchar *dir = g_path_get_dirname(s->recent_files[i]);
    char lbl[256];
    g_snprintf(lbl, sizeof(lbl), "%s  (%s)", base, dir);
    g_free(base);
    g_free(dir);
    GMenuItem *mi = g_menu_item_new(lbl, NULL);
    g_menu_item_set_action_and_target_value(
        mi, "app.open_recent", g_variant_new_string(s->recent_files[i]));
    g_menu_append_item(s->recent_menu, mi);
    g_object_unref(mi);
    any = TRUE;
  }
  if (!any)
    g_menu_append(s->recent_menu, "(no recent files)", NULL);
}

static void action_open_recent(GSimpleAction *a, GVariant *p,

                               gpointer user_data) {
  (void)a;
  AppState *s = (AppState *)user_data;
  if (!s || !p)
    return;
  const char *path = g_variant_get_string(p, NULL);
  if (!path || !path[0])
    return;
  if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
    set_status(s, "Recent file no longer exists.");
    return;
  }
  /* Reuse blank tab or open new */
  EditorDoc *d = NULL;
  if (s->active_doc && (!s->active_doc->path || !s->active_doc->path[0])) {
    GtkTextBuffer *tb =
        s->active_doc->buf ? GTK_TEXT_BUFFER(s->active_doc->buf) : NULL;
    if (tb && !gtk_text_buffer_get_modified(tb))
      d = s->active_doc;
  }
  if (!d) {
    d = app_doc_new(s, path);
    g_ptr_array_add(s->docs, d);
    app_activate_doc(s, d);
    gchar *base = g_path_get_basename(path);
    app_add_doc_tab(s, d, base);
    g_free(base);
  }
  load_file_into_doc(s, d, path);
  recent_files_add(s, path);
  recent_menu_rebuild(s);
  gtk_bookmarks_load(s, path);
}

/* Restore the previous session: reopen the last file if configured to do so. */
static void session_load(AppState *s) {
  if (!s || !s->reopen_last_file)
    return;
  char spath[PATH_MAX];
  gtk_session_path(spath, sizeof(spath));
  gchar *contents = NULL;
  if (!g_file_get_contents(spath, &contents, NULL, NULL))
    return;
  gchar **lines = g_strsplit(contents, "\n", -1);
  gboolean first = TRUE;
  for (gchar **pp = lines; pp && *pp; pp++) {
    gchar *line = g_strstrip(*pp);
    if (!g_str_has_prefix(line, "file="))
      continue;
    const char *fpath = line + 5;
    if (!fpath[0])
      continue;
    if (!g_file_test(fpath, G_FILE_TEST_EXISTS))
      continue;

    gchar *base = g_path_get_basename(fpath);
    EditorDoc *d = NULL;

    if (first && s->active_doc &&
        (!s->active_doc->path || !s->active_doc->path[0])) {
      /* Reuse the initial blank tab for the first file */
      GtkTextBuffer *tb =
          s->active_doc->buf ? GTK_TEXT_BUFFER(s->active_doc->buf) : NULL;
      if (tb && !gtk_text_buffer_get_modified(tb)) {
        d = s->active_doc;
        /* Update tab label from "(untitled)" to the filename */
        doc_update_tab_label(s, d);
      }
    }

    if (!d) {
      d = app_doc_new(s, fpath);
      g_ptr_array_add(s->docs, d);
      app_activate_doc(s, d);
      app_add_doc_tab(s, d, base);
    }

    load_file_into_doc(s, d, fpath);
    first = FALSE;
    g_free(base);
  }
  g_strfreev(lines);
  g_free(contents);
}

static void open_finish_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data);
/* Async completion callback for GFile save operations. */
static void save_finish_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data);

static void tab_plus_clicked(GtkButton *btn, gpointer user_data);
static void tab_close_clicked(GtkButton *btn, gpointer user_data);
static char *path_with_default_ext(const char *path, const char *ext);

static void setup_source_styling(AppState *s);
static void schedule_highlight(AppState *s);
static void on_buffer_changed(GtkTextBuffer *tb, gpointer user_data);
static void apply_current_line_tag(AppState *s);
static void apply_semantic_errors(AppState *s);

// Only apply the red error tag to diagnostics that belong to the file
// currently shown in an editor buffer.
//
// With include expansion enabled for semantic analysis, diagnostics may
// originate from included files. Those should appear in the Problems panel
// but must not be painted onto the wrong buffer.
static bool diag_file_matches_current(const char *diag_file,
                                      const char *cur_file) {
  if (!diag_file || !cur_file)
    return false;
  // Special pseudo files like "(buffer)" should match exactly.
  if (cur_file[0] == '(' || diag_file[0] == '(')
    return strcmp(diag_file, cur_file) == 0;
  char *a = g_canonicalize_filename(diag_file, NULL);
  char *b = g_canonicalize_filename(cur_file, NULL);
  bool eq = (a && b && strcmp(a, b) == 0);
  g_free(a);
  g_free(b);
  return eq;
}
static void apply_comment_tags(AppState *s);
static void highlight_line(AppState *s, int line, const char *text);

// Find/Replace bar
static void show_findbar(AppState *s, gboolean with_replace);
static void hide_findbar(AppState *s);
static void find_next(AppState *s, gboolean forward);
static void do_replace_current(AppState *s);
static void do_replace_all(AppState *s);
static void action_find(GSimpleAction *a, GVariant *p, gpointer ud);
static void action_replace(GSimpleAction *a, GVariant *p, gpointer ud);
static void action_goto(GSimpleAction *a, GVariant *p, gpointer ud);
static GPtrArray *collect_linked_files(AppState *s);
static int replace_all_in_file(AppState *s, const char *path,
                                const char *q, const char *rep,
                                gboolean case_sensitive);
static void action_dup_line(GSimpleAction *a, GVariant *p, gpointer ud);
static void action_dedupe_lines(GSimpleAction *a, GVariant *p, gpointer ud);
static void action_format_layout(GSimpleAction *a, GVariant *p, gpointer ud);

static void update_status_bar(AppState *s);

static void set_status(AppState *s, const char *msg) {
  if (!s)
    return;
  g_free(s->status_msg);
  s->status_msg = g_strdup(msg ? msg : "");
  update_status_bar(s);
}

/* Query the current Caps Lock and Num Lock state from the GDK keymap. */
static void get_lock_state(AppState *s, gboolean *caps, gboolean *num) {
  if (caps)
    *caps = FALSE;
  if (num)
    *num = FALSE;
  if (!s || !s->win)
    return;
  GdkDisplay *disp = gtk_widget_get_display(GTK_WIDGET(s->win));
  if (!disp)
    return;
  GdkSeat *seat = gdk_display_get_default_seat(disp);
  if (!seat)
    return;
  GdkDevice *kbd = gdk_seat_get_keyboard(seat);
  if (!kbd)
    return;
  GdkModifierType mods = gdk_device_get_modifier_state(kbd);
  if (caps)
    *caps = (mods & GDK_LOCK_MASK) != 0;

    // NumLock mask is not consistently exposed in GTK4 across all builds.
    // If GDK_MOD2_MASK exists, use it; otherwise report FALSE.
#ifdef GDK_MOD2_MASK
  if (num)
    *num = (mods & GDK_MOD2_MASK) != 0;
#else
  if (num)
    *num = FALSE;
#endif

  /* No extra platform-specific probing. */
}

static gboolean status_poll_cb(gpointer ud) {
  AppState *s = (AppState *)ud;
  update_status_bar(s);
  return G_SOURCE_CONTINUE;
}

static const char *basename_c(const char *path) {
  if (!path || !*path)
    return NULL;
  const char *a = strrchr(path, '/');
  const char *b = strrchr(path, '\\');
  const char *p = a > b ? a : b;
  return p ? (p + 1) : path;
}

/* ---- Dialog keyboard defaults ----
 * If a dialog has an OK-style response, Enter should activate it.
 * If a dialog has a Cancel-style response, Escape should activate it.
 */

typedef struct {
  int ok_resp;
  int cancel_resp;
} DialogKeyMap;

static gboolean dialog_key_controller_cb(GtkEventControllerKey *ctl,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer user_data) {
  (void)ctl;
  (void)keycode;
  (void)state;
  DialogKeyMap *m = (DialogKeyMap *)user_data;
  GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctl));
  if (!w || !GTK_IS_DIALOG(w) || !m)
    return FALSE;

  if ((keyval == GDK_KEY_Return) || (keyval == GDK_KEY_KP_Enter)) {
    if (m->ok_resp != -1) {
      gtk_dialog_response(GTK_DIALOG(w), m->ok_resp);
      return TRUE;
    }
  } else if (keyval == GDK_KEY_Escape) {
    if (m->cancel_resp != -1) {
      gtk_dialog_response(GTK_DIALOG(w), m->cancel_resp);
      return TRUE;
    }
  }
  return FALSE;
}

/* Add keyboard accelerators to a dialog: Enter -> ok_resp, Escape ->
 * cancel_resp. */
static void dialog_add_default_keys(GtkWidget *dlg, int ok_resp,
                                    int cancel_resp) {
  if (!dlg || !GTK_IS_DIALOG(dlg))
    return;

  /* Set GTK's default response where possible (still useful for accessibility).
   */
  if (ok_resp != -1) {
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), ok_resp);
  }

  GtkEventController *ctl = gtk_event_controller_key_new();
  DialogKeyMap *m = g_new0(DialogKeyMap, 1);
  m->ok_resp = ok_resp;
  m->cancel_resp = cancel_resp;
  g_signal_connect(ctl, "key-pressed", G_CALLBACK(dialog_key_controller_cb), m);
  g_object_set_data_full(G_OBJECT(dlg), "dialog-keymap", m, g_free);
  gtk_widget_add_controller(dlg, ctl);
}

static void update_status_bar(AppState *s) {
  if (!s || !s->status)
    return;

  // Cursor
  int line = 1, col = 1;
  if (s->buf) {
    GtkTextIter it;
    GtkTextMark *mk = gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(s->buf));
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(s->buf), &it, mk);
    line = gtk_text_iter_get_line(&it) + 1;
    col = gtk_text_iter_get_line_offset(&it) + 1;
  }

  // Dirty / filename
  gboolean dirty = FALSE;
  if (s->buf)
    dirty = gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(s->buf));
  const char *bn = basename_c(s->path);
  char file_part[256];
  if (bn && *bn) {
    g_snprintf(file_part, sizeof(file_part), "%s%s", bn, dirty ? "*" : "");
  } else {
    g_snprintf(file_part, sizeof(file_part), "%s",
               dirty ? "*unsaved*" : "(no file)");
  }

  // Insert/overwrite
  gboolean ovr = FALSE;
  if (s->view && GTK_IS_TEXT_VIEW(s->view))
    ovr = gtk_text_view_get_overwrite(GTK_TEXT_VIEW(s->view));

  // Locks
  gboolean caps = FALSE, num = FALSE;
  get_lock_state(s, &caps, &num);

  const char *theme = s->dark_mode ? "Dark" : "Light";
  const char *mode = ovr ? "OVR" : "INS";

  // Only show lock indicators when enabled.
  char locks[64];
  locks[0] = 0;
  if (caps)
    g_strlcat(locks, " | Caps Lock", sizeof(locks));
  if (num)
    g_strlcat(locks, " | Num Lock", sizeof(locks));

  char out[1024];
  int fsize = (s->font_size > 0) ? s->font_size : 12;
  g_snprintf(out, sizeof(out), "Ln %d, Col %d | %s | %s | %s | %dpt%s", line,
             col, file_part, mode, theme, fsize, locks);

  if (!s->status_last || g_strcmp0(s->status_last, out) != 0) {
    g_free(s->status_last);
    s->status_last = g_strdup(out);
    gtk_label_set_text(GTK_LABEL(s->status), s->status_last);
  }
}

// Ensure periodic sources don't fire while the main window is being destroyed.
static gboolean main_close_request_cb(GtkWindow *w, gpointer user_data) {
  (void)w;
  AppState *s = (AppState *)user_data;
  if (!s)
    return FALSE;

  if (s->status_poll_id) {
    g_source_remove(s->status_poll_id);
    s->status_poll_id = 0;
  }

  if (s->problems_refresh_idle_id) {
    g_source_remove(s->problems_refresh_idle_id);
    s->problems_refresh_idle_id = 0;
  }

  if (s->problems_init_idle_id) {
    g_source_remove(s->problems_init_idle_id);
    s->problems_init_idle_id = 0;
  }
  if (s->copy_toast_hide_id) {
    g_source_remove(s->copy_toast_hide_id);
    s->copy_toast_hide_id = 0;
  }

  // Save preferences before window closes.
  gtk_prefs_save(s);
  session_save(s);
  recent_files_save(s);
  /* Save bookmarks for active document */
  if (s->notebook && s->docs) {
    int pg = gtk_notebook_get_current_page(s->notebook);
    if (pg >= 0 && (guint)pg < s->docs->len) {
      EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, (guint)pg);
      if (d && d->path)
        gtk_bookmarks_save(s, d->path);
    }
  }
  return FALSE; // allow normal close
}

static void on_cursor_moved(GObject *obj, GParamSpec *pspec,
                            gpointer user_data) {
  (void)obj;
  (void)pspec;
  AppState *s = (AppState *)user_data;
  apply_current_line_tag(s);
  update_status_bar(s);
}

static void app_state_set_path(AppState *s, const char *path) {
  /* s->path is owned by AppState and must never alias an EditorDoc path. */
  g_free(s->path);
  s->path = path ? g_strdup(path) : NULL;
}

static void doc_update_tab_label(AppState *s, EditorDoc *d) {
  if (!s || !s->notebook || !d || !d->scroller)
    return;

  /* Tab labels are wrapped in a header box (label + close button). We stash the
     GtkLabel on the scroller so we can update it reliably. */
  GtkWidget *tab_lbl =
      (GtkWidget *)g_object_get_data(G_OBJECT(d->scroller), "tab-label");
  if (!tab_lbl) {
    /* Fallback for older tabs: try the notebook tab label directly. */
    tab_lbl = gtk_notebook_get_tab_label(s->notebook, d->scroller);
  }
  if (!tab_lbl || !GTK_IS_LABEL(tab_lbl))
    return;

  const char *base = d->path ? path_basename(d->path) : NULL;
  char *base_utf8 =
      base ? g_filename_to_utf8(base, -1, NULL, NULL, NULL) : NULL;

  char title[1024];
  if (base_utf8 && base_utf8[0]) {
    snprintf(title, sizeof(title), "%s%s", base_utf8, d->dirty ? "*" : "");
  } else {
    snprintf(title, sizeof(title), "%s%s", "(untitled)", d->dirty ? "*" : "");
  }

  gtk_label_set_text(GTK_LABEL(tab_lbl), title);
  g_free(base_utf8);
}
static void set_title(AppState *s) {
  /* Paths are not guaranteed to be UTF-8. Use display-name conversion for UI.
   */
  char *base = NULL;
  if (s->path)
    base = g_filename_display_basename(s->path);
  const char *name = s->path ? base : "(untitled)";

  char tmp[512];
  g_snprintf(tmp, sizeof(tmp), "6502caEd - %s%s", name, s->dirty ? " *" : "");
  gtk_window_set_title(s->win, tmp);
  gtk_label_set_text(s->title, tmp);

  /* Keep the active tab label in sync with the active document, not "current
     page" (switch-page can be re-entrant while we are creating/selecting
     pages). */

  g_free(base);
}

static void mark_dirty(AppState *s, gboolean dirty) {
  s->dirty = dirty;
  if (s->active_doc) {
    s->active_doc->dirty = dirty;
    if (s->active_doc->scroller)
      doc_update_tab_label(s, s->active_doc);
  }
  set_title(s);
}

static void on_modified_changed(GObject *obj, GParamSpec *pspec,
                                gpointer user_data) {
  (void)obj;
  (void)pspec;
  AppState *s = (AppState *)user_data;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(obj);
  EditorDoc *d = (EditorDoc *)g_object_get_data(G_OBJECT(tb), "editor-doc");
  gboolean m = gtk_text_buffer_get_modified(tb);

  // Track dirty state per-document. Only update title/status for the active
  // tab.
  if (d)
    d->dirty = m;
  if (d && d->scroller)
    doc_update_tab_label(s, d);

  if (!d || d == app_get_active_doc(s)) {
    s->dirty = m;
    set_title(s);
    update_status_bar(s);
  }
}

/* ---------------- Theme handling ---------------- */

static void ensure_css_provider(AppState *s) {
  if (s->css)
    return;
  s->css = gtk_css_provider_new();

  GdkDisplay *disp = gdk_display_get_default();
  if (!disp)
    return;

  gtk_style_context_add_provider_for_display(
      disp, GTK_STYLE_PROVIDER(s->css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static void apply_theme_css(AppState *s) {
  ensure_css_provider(s);
  if (!s->css)
    return;

  // Force editor background + text colours regardless of system theme.
  // This targets GtkSourceView + its internal text widget.
  const char *css_dark =
      /* window + menubar + popovers */
      "window { background-color: #1e1e1e; color: #d4d4d4; }"
      "popovermenubar { background-color: #2a2a2a; color: #d4d4d4; }"
      "popovermenubar button { color: #d4d4d4; }"
      "popovermenubar button:hover { background-color: #3a3a3a; }"
      "popover.menu { background-color: #2a2a2a; color: #d4d4d4; }"
      "popover.menu modelbutton { color: #d4d4d4; }"
      "popover.menu modelbutton:hover { background-color: #3a3a3a; }"
      "popover.menu modelbutton:selected { background-color: #3a3a3a; }"
      /* Menu section separators: visible line, inset from edges */
      /* separator margin set by MENU_SEPARATOR_MARGIN */
      "popover.menu separator {"
      "  background-color: #4a4a4a;"
      "  min-height: 1px;"
      "  margin-left: %dpx;"
      "  margin-right: %dpx;"
      "  margin-top: 3px;"
      "  margin-bottom: 3px;"
      "}"
      /* main text area */
      "scrolledwindow, viewport {"
      "  background-color: #1e1e1e;"
      "}"
      /* GTK4 sometimes paints the left "border" node (used behind the gutter)
         with the default theme background unless we override it. */
      "textview > border, textview > border.left, textview > border.right, "
      "textview > border.top, textview > border.bottom {"
      "  background-color: #1e1e1e;"
      "}"
      "GtkSourceView border, GtkTextView border {"
      "  background-color: #1e1e1e;"
      "}"
      "GtkSourceView, GtkTextView, textview {"
      "  background-color: #1e1e1e;"
      "  color: #d4d4d4;"
      "}"
      "GtkSourceView text, GtkTextView text, textview text {"
      "  background-color: #1e1e1e;"
      "  color: #d4d4d4;"
      "}"
      /* gutter (line numbers column) - GtkSourceView's node names vary across
         versions; cover both the generic "gutter" node and GTK's child selector
         for the internal textview. */
      "GtkSourceView gutter, textview > gutter {"
      "  background-color: #1e1e1e;"
      "}"
      "GtkSourceView gutter line-number, textview > gutter line-number {"
      "  color: #5a5a5a;"
      "}"
      "textview > gutter separator {"
      "  background-color: #1e1e1e;"
      "}"
      /* current line highlight (text + gutter) */
      "GtkSourceView .current-line {"
      "  background-color: #2a2a2a;"
      "}"
      "GtkSourceView gutter .current-line line-number {"
      "  color: #d4d4d4;"
      "}"
      /* selection + caret */
      "GtkSourceView selection, GtkTextView selection, textview selection {"
      "  background-color: #264f78;"
      "}"
      "GtkSourceView, GtkTextView, textview {"
      "  caret-color: #d4d4d4;"
      "}"
      "GtkSourceView text, GtkTextView text, textview text {"
      "  caret-color: #d4d4d4;"
      "}"
      /* hide the paned handle when the problems panel is collapsed */
      "paned.problems-hidden > separator { opacity: 0; min-height: 0px; "
      "min-width: 0px; }"
      /* completion popup — override the hardcoded min-content-height:1
         in GtkSourceCompletionList's UI template so multiple rows show */
      "gtk-source-completion scrolledwindow { min-height: 200px; }";

  const char *css_light =
      /* window + menubar + popovers */
      "window { background-color: #f4f4f4; color: #222222; }"
      "popovermenubar { background-color: #f4f4f4; color: #222222; }"
      "popovermenubar button { color: #222222; }"
      "popovermenubar button:hover { background-color: #e6e6e6; }"
      "popover.menu { background-color: #f4f4f4; color: #222222; }"
      "popover.menu modelbutton { color: #222222; }"
      "popover.menu modelbutton:hover { background-color: #e6e6e6; }"
      "popover.menu modelbutton:selected { background-color: #e6e6e6; }"
      /* Menu section separators */
      /* separator margin set by MENU_SEPARATOR_MARGIN */
      "popover.menu separator {"
      "  background-color: #cccccc;"
      "  min-height: 1px;"
      "  margin-left: %dpx;"
      "  margin-right: %dpx;"
      "  margin-top: 3px;"
      "  margin-bottom: 3px;"
      "}"
      /* main text area */
      "scrolledwindow, viewport {"
      "  background-color: #ffffff;"
      "}"
      "textview > border, textview > border.left, textview > border.right, "
      "textview > border.top, textview > border.bottom {"
      "  background-color: #ffffff;"
      "}"
      "GtkSourceView border, GtkTextView border {"
      "  background-color: #ffffff;"
      "}"
      "GtkSourceView, GtkTextView, textview {"
      "  background-color: #ffffff;"
      "  color: #000000;"
      "}"
      "GtkSourceView text, GtkTextView text, textview text {"
      "  background-color: #f2f2f2;"
      "  color: #000000;"
      "}"
      /* gutter (line numbers column) */
      "GtkSourceView gutter, textview > gutter {"
      "  background-color: #f2f2f2;"
      "}"
      "GtkSourceView gutter line-number, textview > gutter line-number {"
      "  color: #7a7a7a;"
      "}"
      "textview > gutter separator {"
      "  background-color: #f2f2f2;"
      "}"
      /* current line highlight (text + gutter) */
      "GtkSourceView .current-line {"
      "  background-color: #e6e6e6;"
      "}"
      "GtkSourceView gutter .current-line line-number {"
      "  color: #000000;"
      "}"
      /* selection + caret */
      "GtkSourceView selection, GtkTextView selection, textview selection {"
      "  background-color: #b5d5ff;"
      "}"
      "GtkSourceView, GtkTextView, textview {"
      "  caret-color: #000000;"
      "}"
      "GtkSourceView text, GtkTextView text, textview text {"
      "  caret-color: #000000;"
      "}"
      /* hide the paned handle when the problems panel is collapsed */
      "paned.problems-hidden > separator { opacity: 0; min-height: 0px; "
      "min-width: 0px; }"
      /* completion popup — override the hardcoded min-content-height:1
         in GtkSourceCompletionList's UI template so multiple rows show */
      "gtk-source-completion scrolledwindow { min-height: 200px; }";

  /* Format separator margins using the compile-time constant */
  gchar *css_final = g_strdup_printf(
      s->dark_mode ? css_dark : css_light, MENU_SEPARATOR_MARGIN,
      MENU_SEPARATOR_MARGIN, MENU_SEPARATOR_MARGIN, MENU_SEPARATOR_MARGIN);
  gtk_css_provider_load_from_string(s->css, css_final);
  g_free(css_final);
}

static void apply_font_css(AppState *s) {
  ensure_css_provider(s);
  if (!s->css)
    return;
  const char *family = s->font_family[0] ? s->font_family : "Monospace";
  int size = (s->font_size > 0) ? s->font_size : 12;
  /* Build CSS that overrides the font on all editor text views */
  char css[512];
  g_snprintf(css, sizeof(css),
             "GtkSourceView, GtkTextView, textview {"
             "  font-family: \"%s\";"
             "  font-size: %dpt;"
             "}"
             "GtkSourceView text, GtkTextView text, textview text {"
             "  font-family: \"%s\";"
             "  font-size: %dpt;"
             "}",
             family, size, family, size);

  /* Use a separate per-font CSS provider so we do not clobber
     the theme colours set by apply_theme_css. */
  if (!s->css_font) {
    s->css_font = gtk_css_provider_new();
    GdkDisplay *disp = gdk_display_get_default();
    if (disp)
      gtk_style_context_add_provider_for_display(
          disp, GTK_STYLE_PROVIDER(s->css_font),
          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  }
  gtk_css_provider_load_from_string(s->css_font, css);
}

static void apply_style_scheme(AppState *s) {
  GtkSourceStyleSchemeManager *mgr =
      gtk_source_style_scheme_manager_get_default();
  if (!mgr)
    return;

  // s->buf is stored as GtkTextBuffer* (or similar). GtkSourceBuffer is a
  // subclass of GtkTextBuffer.
  if (!s->buf || !GTK_SOURCE_IS_BUFFER(s->buf))
    return;

  GtkSourceBuffer *sbuf = GTK_SOURCE_BUFFER(s->buf);

  const char *scheme_id = s->dark_mode ? "Adwaita-dark" : "Adwaita";
  GtkSourceStyleScheme *scheme =
      gtk_source_style_scheme_manager_get_scheme(mgr, scheme_id);
  if (scheme) {
    gtk_source_buffer_set_style_scheme(sbuf, scheme);
  }
}

/* Apply the dark/light window hint to a child window.
   Registering with the GtkApplication causes GTK to set the
   _GTK_THEME_VARIANT hint so the compositor draws the title bar
   chrome in the correct dark/light style. */
static void apply_window_dark_hint(GtkWidget *win, gboolean dark) {
  if (dark) {
    gtk_widget_add_css_class(win, "dark");
    gtk_widget_remove_css_class(win, "light");
  } else {
    gtk_widget_add_css_class(win, "light");
    gtk_widget_remove_css_class(win, "dark");
  }
  /* Force CSD by setting a GtkHeaderBar — this makes the title bar
     drawn by GTK (using our CSS) rather than by the compositor. */
  if (GTK_IS_WINDOW(win) && gtk_window_get_titlebar(GTK_WINDOW(win)) == NULL) {
    const char *title = gtk_window_get_title(GTK_WINDOW(win));
    GtkWidget *hdr = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hdr), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(win), hdr);
  }
}

static void apply_theme(AppState *s) {
  // GTK theme preference (chrome). Some systems only apply this to window
  // chrome.
  GtkSettings *settings = gtk_settings_get_default();
  if (settings) {
    g_object_set(settings, "gtk-application-prefer-dark-theme",
                 s->dark_mode ? TRUE : FALSE, NULL);
  }
  /* Update chrome hint on all open application windows */
  if (s->app) {
    GList *wins = gtk_application_get_windows(s->app);
    for (GList *l = wins; l; l = l->next)
      apply_window_dark_hint(GTK_WIDGET(l->data), s->dark_mode);
  }

  // Force the editor area colours.
  apply_theme_css(s);
  apply_font_css(s);
  apply_style_scheme(s);

  /* Update all syntax-highlight tag colours to match the newly loaded palette.
   * create_tags() only runs once at startup; after a dark/light switch we must
   * update the existing tag objects in-place so the change takes effect. */
  if (s->tag_instr)
    g_object_set(s->tag_instr, "foreground", s->colors.instr, NULL);
  if (s->tag_dir)
    g_object_set(s->tag_dir, "foreground", s->colors.dir, NULL);
  if (s->tag_data)
    g_object_set(s->tag_data, "foreground", s->colors.data, NULL);
  if (s->tag_num)
    g_object_set(s->tag_num, "foreground", s->colors.num, NULL);
  if (s->tag_label)
    g_object_set(s->tag_label, "foreground", s->colors.label, NULL);
  if (s->tag_reg)
    g_object_set(s->tag_reg, "foreground", s->colors.reg, NULL);
  if (s->tag_comment)
    g_object_set(s->tag_comment, "foreground", s->colors.comment, NULL);
  if (s->tag_string)
    g_object_set(s->tag_string, "foreground", s->colors.str, NULL);
  if (s->tag_inactive)
    g_object_set(s->tag_inactive, "foreground", s->colors.inactive, NULL);
  if (s->tag_hover_def)
    g_object_set(s->tag_hover_def, "background", s->colors.hover_bg, NULL);
  if (s->tag_addr_hex)
    g_object_set(s->tag_addr_hex, "foreground", s->colors.addr_hex, NULL);
  if (s->tag_addr_dec)
    g_object_set(s->tag_addr_dec, "foreground", s->colors.addr_dec, NULL);
  if (s->tag_addr_bin)
    g_object_set(s->tag_addr_bin, "foreground", s->colors.addr_bin, NULL);
  if (s->tag_imm_hex)
    g_object_set(s->tag_imm_hex, "foreground", s->colors.imm_hex, NULL);
  if (s->tag_imm_dec)
    g_object_set(s->tag_imm_dec, "foreground", s->colors.imm_dec, NULL);
  if (s->tag_imm_bin)
    g_object_set(s->tag_imm_bin, "foreground", s->colors.imm_bin, NULL);
  if (s->tag_imm_char)
    g_object_set(s->tag_imm_char, "foreground", s->colors.imm_char, NULL);
  if (s->tag_dv_hex)
    g_object_set(s->tag_dv_hex, "foreground", s->colors.dv_hex, NULL);
  if (s->tag_dv_dec)
    g_object_set(s->tag_dv_dec, "foreground", s->colors.dv_dec, NULL);
  if (s->tag_dv_bin)
    g_object_set(s->tag_dv_bin, "foreground", s->colors.dv_bin, NULL);
  if (s->tag_dv_str)
    g_object_set(s->tag_dv_str, "foreground", s->colors.dv_str, NULL);
  if (s->tag_error) {
    GdkRGBA u;
    gdk_rgba_parse(&u, s->colors.err_underline);
    if (s->error_underline)
      g_object_set(s->tag_error, "underline-rgba", &u, NULL);
    else
      g_object_set(s->tag_error, "foreground-rgba", &u, NULL);
  }

  g_object_set(s->tag_curline, "background",
               s->dark_mode ? "#2a2a2a" : "#e9e9e9", NULL);
  apply_current_line_tag(s);

  /* Reschedule syntax highlighting so all open lines pick up the new colours */
  schedule_highlight(s);

  set_status(s, s->dark_mode ? "Theme: Dark" : "Theme: Light");
}

/* ---------------- file I/O ---------------- */

/* Remember only completed source-file operations; cancelling a chooser
 * leaves this session location untouched. GFile preserves drive/volume paths. */
static void remember_source_folder(AppState *s, const char *path) {
  if (!s || !path || !*path) return;
  GFile *file = g_file_new_for_path(path);
  GFile *parent = g_file_get_parent(file);
  g_object_unref(file);
  if (parent) {
    g_clear_object(&s->last_source_folder);
    s->last_source_folder = parent;
  }
}

static void source_dialog_initial_folder(AppState *s, GtkFileDialog *dlg) {
  GFile *folder = s->last_source_folder;
  if (folder && g_file_query_file_type(folder, G_FILE_QUERY_INFO_NONE, NULL)
                    == G_FILE_TYPE_DIRECTORY) {
    gtk_file_dialog_set_initial_folder(dlg, folder);
    return;
  }
  /* First use, or a removed/unmounted remembered directory: use the active
   * document's directory if available, otherwise retain the chooser default. */
  if (s->path && *s->path) {
    GFile *file = g_file_new_for_path(s->path);
    GFile *parent = g_file_get_parent(file);
    if (parent && g_file_query_file_type(parent, G_FILE_QUERY_INFO_NONE, NULL)
                      == G_FILE_TYPE_DIRECTORY)
      gtk_file_dialog_set_initial_folder(dlg, parent);
    g_clear_object(&parent);
    g_object_unref(file);
  }
}



static void update_title(AppState *s) { set_title(s); }

static gboolean file_looks_binary(const guint8 *p, gsize n) {
  if (!p || n == 0)
    return FALSE;
  gsize bad = 0;
  for (gsize i = 0; i < n; ++i) {
    guint8 c = p[i];
    if (c == 0)
      return TRUE; /* NUL is a very strong signal */
    if (c < 0x09)
      bad++; /* 0x00..0x08 */
    else if (c > 0x0D && c < 0x20)
      bad++; /* control chars excluding \t\n\r */
  }
  /* Allow some control characters (e.g. ANSI), but not many. */
  return (bad * 100 / n) > 5;
}

typedef struct {
  int resp;
  GMainLoop *loop;
} ConfirmCtx;

static void confirm_open_resp_cb(GtkDialog *d, int resp, gpointer user_data) {
  ConfirmCtx *c = (ConfirmCtx *)user_data;
  if (c)
    c->resp = resp;
  gtk_window_destroy(GTK_WINDOW(d));
  if (c && c->loop)
    g_main_loop_quit(c->loop);
}

static gboolean confirm_open_nontext(AppState *s, const char *path) {
  if (!s || !s->win)
    return FALSE;
  const char *name = path;
  const char *slash = strrchr(path, '/');
  if (slash && slash[1])
    name = slash + 1;

  gchar *msg = g_strdup_printf(
      "The selected file looks like a non-text file (%s).\n\nOpen anyway?",
      name);

  GtkWidget *dlg =
      gtk_message_dialog_new(GTK_WINDOW(s->win), GTK_DIALOG_MODAL,
                             GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", msg);
  g_free(msg);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "Open", GTK_RESPONSE_ACCEPT);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_CANCEL);

  /* Small blocking modal confirm (used only on explicit open). */
  ConfirmCtx ctx = {GTK_RESPONSE_CANCEL, g_main_loop_new(NULL, FALSE)};
  g_signal_connect(dlg, "response", G_CALLBACK(confirm_open_resp_cb), &ctx);

  gtk_window_present(GTK_WINDOW(dlg));
  g_main_loop_run(ctx.loop);
  g_main_loop_unref(ctx.loop);
  return ctx.resp == GTK_RESPONSE_ACCEPT;
}

static void load_file_into_doc(AppState *s, EditorDoc *d, const char *path) {
  if (!s || !d || !path)
    return;

  GtkSourceBuffer *buf = d->buf;
  char *contents = NULL;
  gsize len = 0;
  GError *err = NULL;

  if (!g_file_get_contents(path, &contents, &len, &err)) {
    show_error(s, err ? err->message : "Failed to open file");
    if (err)
      g_error_free(err);
    return;
  }

  remember_source_folder(s, path);

  /* Update doc path (keep per-doc; AppState mirrors active doc). */
  g_free(d->path);
  d->path = g_strdup(path);
  d->dirty = false;

  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buf), contents, (gint)len);
  g_free(contents);

  /* Clear modified state and any stale diagnostics/highlights. */
  gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(buf), FALSE);

  if (s->active_doc == d) {
    s->buf = d->buf;
    app_state_set_path(s, d->path);
    s->dirty = d->dirty;
    doc_update_tab_label(s, d);
    set_title(s);
    schedule_highlight(s);
    problems_refresh_now(s);
  } else {
    doc_update_tab_label(s, d);
  }
}

static gboolean save_buffer_to_file(AppState *s, const char *path,
                                    GError **err) {
  if (!s || !s->buf || !path)
    return FALSE;

  /* If any folds are active, temporarily unfold all so the saved file
   * contains the complete unfolded text, then restore fold state after. */
  gboolean had_folds = FALSE;
  gboolean *collapsed_state = NULL;
  size_t fold_count = 0;

  if (s->fold_map.lines && s->fold_map.count > 0) {
    fold_count = s->fold_map.count;
    collapsed_state = g_malloc0(fold_count * sizeof(gboolean));
    for (size_t i = 0; i < fold_count; i++) {
      if (s->fold_map.lines[i].kind == FL_FOLD_START &&
          s->fold_map.lines[i].collapsed) {
        collapsed_state[i] = TRUE;
        s->fold_map.lines[i].collapsed = FALSE;
        had_folds = TRUE;
      }
    }
    if (had_folds)
      apply_fold_tags(s);
  }

  GtkTextIter a, b;
  gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(s->buf), &a, &b);
  char *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &a, &b, FALSE);
  if (!text)
    text = g_strdup("");

  gboolean ok = g_file_set_contents(path, text, -1, err);
  g_free(text);
  if (ok) {
    gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(s->buf), FALSE);
    set_status(s, "Saved.");
  }

  /* Restore fold state */
  if (had_folds && collapsed_state && s->fold_map.lines) {
    for (size_t i = 0; i < fold_count && i < s->fold_map.count; i++) {
      if (collapsed_state[i])
        s->fold_map.lines[i].collapsed = TRUE;
    }
    apply_fold_tags(s);
  }
  g_free(collapsed_state);

  return ok;
}

static void open_finish_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data) {
  (void)source;
  AppState *s = (AppState *)user_data;
  GError *err = NULL;
  GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, &err);
  if (!file) {
    if (err) {
      g_warning("open_finish: %s", err->message);
      g_clear_error(&err);
    }
    return;
  }

  /* Use UTF-8-safe display basename for UI */
  char *base_utf8 = g_file_get_basename(file);
  char *path = g_file_get_path(file);
  g_object_unref(file);

  if (!path) {
    g_free(base_utf8);
    return;
  }

  /* If the current tab is the initial empty/untitled document and it hasn't
     been modified, reuse it instead of always creating a second tab.
     This matches typical editor behaviour (start with one blank tab; opening
     the first file replaces it). */
  EditorDoc *d = NULL;
  if (s->active_doc && (!s->active_doc->path || !s->active_doc->path[0])) {
    GtkTextBuffer *tb =
        s->active_doc->buf ? GTK_TEXT_BUFFER(s->active_doc->buf) : NULL;
    const gboolean modified =
        tb ? gtk_text_buffer_get_modified(tb) : s->active_doc->dirty;
    if (!modified && !s->active_doc->dirty) {
      d = s->active_doc;
    }
  }

  if (!d) {
    /* Open into a new tab/document.
       IMPORTANT: give the doc its path up-front so that early tab-switch
       signals (triggered by gtk_notebook_set_current_page) don't momentarily
       label the new tab as "(untitled)" and/or desync tab titles from
       documents. */
    d = app_doc_new(s, path);
    g_ptr_array_add(s->docs, d);

    /* Make it the active document BEFORE we add/select the notebook page.
       This avoids switch-page re-entrancy clobbering the wrong tab title. */
    app_activate_doc(s, d);
    app_add_doc_tab(s, d, base_utf8 ? base_utf8 : "(untitled)");
  }

  /* Load file text into the chosen document (new or reused). */
  load_file_into_doc(s, d, path);

  /* load_file_into_buffer() already updates d->path and titles */
  recent_files_add(s, path);
  recent_menu_rebuild(s);
  gtk_bookmarks_load(s, path);

  g_free(base_utf8);
  g_free(path);
}

static void save_finish_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data) {
  (void)source;
  AppState *s = (AppState *)user_data;
  GError *err = NULL;
  GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, &err);
  if (!file) {
    if (err) {
      g_warning("save_finish: %s", err->message);
      g_clear_error(&err);
    }
    return;
  }
  char *path0 = g_file_get_path(file);
  g_object_unref(file);
  if (path0) {
    char *path =
        path_with_default_ext(path0, (s->src_ext[0] ? s->src_ext : ".asm"));
    g_free(path0);

    if (save_buffer_to_file(s, path, &err)) {
      remember_source_folder(s, path);
      if (s->active_doc) {
        if (s->active_doc->path)
          g_free(s->active_doc->path);
        s->active_doc->path = g_strdup(path);
        app_state_set_path(s, s->active_doc->path);
      } else {
        app_state_set_path(s, path);
      }
      update_title(s);
      /* Update the tab label to show the new filename */
      if (s->active_doc)
        doc_update_tab_label(s, s->active_doc);
      /* Complete a pending tab-close that was waiting for this save */
      if (s->pending_close_page) {
        GtkWidget *pg = s->pending_close_page;
        EditorDoc *doc = s->pending_close_doc;
        s->pending_close_page = NULL;
        s->pending_close_doc = NULL;
        if (gtk_notebook_get_n_pages(s->notebook) == 1)
          last_tab_do_replace(s, pg, doc);
        else
          tab_do_close(s, pg, doc);
      }
    } else if (err) {
      g_warning("save failed: %s", err->message);
      g_clear_error(&err);
      set_status(s, "Save failed.");
      /* Clear pending close — save failed, leave the tab open */
      s->pending_close_page = NULL;
      s->pending_close_doc = NULL;
    }
    g_free(path);
  }
}

/* ---------------- highlighting scheduling ---------------- */

static void setup_source_styling(AppState *s) {
  if (!s || !s->buf)
    return;

  // We do our own lightweight token colouring via tags.
  gtk_source_buffer_set_highlight_syntax(GTK_SOURCE_BUFFER(s->buf), FALSE);
  gtk_source_buffer_set_highlight_matching_brackets(GTK_SOURCE_BUFFER(s->buf),
                                                    TRUE);
}

static gboolean highlight_idle_cb(gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s || !s->buf)
    return G_SOURCE_REMOVE;

  s->hl_idle_id = 0;
  {
    EditorDoc *ad = app_get_active_doc(s);
    if (ad)
      ad->hl_idle_id = 0;
  }

  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  int lines = gtk_text_buffer_get_line_count(tb);
  // Rebuild label/macro definition index FIRST so highlight_line can
  // query macro_defs while colouring tokens.
  EditorDoc *d = app_get_active_doc(s);
  if (d)
    doc_rebuild_label_defs(s, d);

  for (int line = 0; line < lines; line++) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line(tb, &a, line);
    b = a;
    gtk_text_iter_forward_to_line_end(&b);
    char *lt = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
    if (lt) {
      highlight_line(s, line, lt);
      g_free(lt);
    }
  }

  // Comments override other tags and support /* ... */ across lines.
  apply_comment_tags(s);
  apply_folds(s);

  apply_semantic_errors(s);
  apply_current_line_tag(s);
  return G_SOURCE_REMOVE;
}

static void schedule_highlight(AppState *s) {
  if (!s)
    return;
  if (s->hl_idle_id) {
    GSource *src = g_main_context_find_source_by_id(NULL, s->hl_idle_id);
    if (src)
      g_source_remove(s->hl_idle_id);
    s->hl_idle_id = 0;
  }
  // Debounce: wait briefly so fast typing doesn't re-highlight constantly.
  s->hl_idle_id = g_timeout_add(50, highlight_idle_cb, s);
  {
    EditorDoc *ad = app_get_active_doc(s);
    if (ad)
      ad->hl_idle_id = s->hl_idle_id;
  }
}

/* Track line insertions/deletions to adjust jump history positions */
static void on_buf_insert_text(GtkTextBuffer *tb, GtkTextIter *pos,
                               const gchar *text, gint len, gpointer ud) {
  (void)tb;
  AppState *s = (AppState *)ud;
  /* Count newlines in inserted text */
  int newlines = 0;
  for (int i = 0; i < len; i++)
    if (text[i] == '\n')
      newlines++;
  if (newlines > 0) {
    int at_line = gtk_text_iter_get_line(pos);
    jump_buf_adjust(&s->jump_buf, at_line + 1, newlines);
  }
}

static void on_buf_delete_range(GtkTextBuffer *tb, GtkTextIter *start,
                                GtkTextIter *end, gpointer ud) {
  (void)tb;
  AppState *s = (AppState *)ud;
  int sl = gtk_text_iter_get_line(start);
  int el = gtk_text_iter_get_line(end);
  int delta = el - sl;
  if (delta > 0)
    jump_buf_adjust(&s->jump_buf, sl + 1, -delta);
}

/* Signal handler: buffer changed — mark document modified, schedule refresh. */
static void on_buffer_changed(GtkTextBuffer *tb, gpointer user_data) {
  AppState *s = (AppState *)user_data;

  // Multiple tabs each have their own buffer. Only schedule highlighting for
  // the buffer that actually changed (normally the active tab).
  EditorDoc *d = (EditorDoc *)g_object_get_data(G_OBJECT(tb), "editor-doc");
  if (d && d != app_get_active_doc(s))
    return;

  /* Invalidate the last explicit check/assemble result — the buffer has
     changed so those diagnostics are now stale. The background semantic
     pass (sem_diags) will provide fresh inline results immediately. */
  diags_free(&s->asm_diags);
  diags_init(&s->asm_diags);

  schedule_highlight(s);
}

/* ---------------- token helpers ---------------- */

static int streqi(const char *a, const char *b) {
  while (*a && *b) {
    int ca = toupper((unsigned char)*a++);
    int cb = toupper((unsigned char)*b++);
    if (ca != cb)
      return 0;
  }
  return *a == 0 && *b == 0;
}

static int gui_is_ident_start(int c) {
  return isalpha(c) || c == '_' || c == '.';
}
static int gui_is_ident_char(int c) {
  return isalnum(c) || c == '_' || c == '.';
}

static int gui_is_label_char(int c) {
  // Label refs/defs: allow '.' for local labels, '_' and alnum.
  // Do NOT include ':' here (handled separately for :+ / :- branch labels).
  return isalnum(c) || c == '_' || c == '.';
}

// Labels may begin with '.' or ':' but the name portion must be a simple
// identifier: [A-Za-z_][A-Za-z0-9_]*
// (No special characters other than underscore.)
static int gui_is_simple_label_name(const char *s) {
  if (!s || !*s)
    return 0;
  unsigned char c0 = (unsigned char)*s;
  if (!(isalpha(c0) || c0 == '_'))
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (!(isalnum(*p) || *p == '_'))
      return 0;
  }
  return 1;
}

static void doc_clear_hover(EditorDoc *d) {
  if (!d || !d->buf || !d->tag_hover_def)
    return;
  if (d->hover_start >= 0 && d->hover_end > d->hover_start) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(d->buf), &a,
                                       d->hover_start);
    gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(d->buf), &b,
                                       d->hover_end);
    gtk_text_buffer_remove_tag(GTK_TEXT_BUFFER(d->buf), d->tag_hover_def, &a,
                               &b);
  }
  d->hover_start = -1;
  d->hover_end = -1;
}

/* Apply the hover-definition underline tag to byte range [start,end). */
static void doc_apply_hover(EditorDoc *d, int start, int end) {
  if (!d || !d->buf || !d->tag_hover_def)
    return;
  doc_clear_hover(d);
  if (start < 0 || end <= start)
    return;
  GtkTextIter a, b;
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(d->buf), &a, start);
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(d->buf), &b, end);
  gtk_text_buffer_apply_tag(GTK_TEXT_BUFFER(d->buf), d->tag_hover_def, &a, &b);
  d->hover_start = start;
  d->hover_end = end;
}

/* Clear the per-document label/definition index. */
static void doc_label_index_clear(EditorDoc *d) {
  if (!d)
    return;
  if (!d->label_defs)
    d->label_defs =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  else
    g_hash_table_remove_all(d->label_defs);

  if (!d->label_defs_ci)
    d->label_defs_ci =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  else
    g_hash_table_remove_all(d->label_defs_ci);

  if (!d->macro_defs)
    d->macro_defs =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  else
    g_hash_table_remove_all(d->macro_defs);
}

/* Add a label definition entry to the per-document index.
 * in: name — symbol name, start/end — character offset range. */
static void doc_label_index_put(EditorDoc *d, const char *name, int start,
                                int end) {
  if (!d || !d->label_defs || !name || !*name)
    return;
  // Value is int[2]
  int *span = (int *)g_malloc(sizeof(int) * 2);
  span[0] = start;
  span[1] = end;
  g_hash_table_replace(d->label_defs, g_strdup(name), span);

  /* Case-insensitive helper map for tooltip hints.
     Keep a canonical spelling for each lowercase name. */
  if (d->label_defs_ci) {
    char *low = g_ascii_strdown(name, -1);
    if (low) {
      g_hash_table_replace(d->label_defs_ci, low, g_strdup(name));
    }
  }
}

static const char *doc_label_index_find_ci(EditorDoc *d, const char *name) {
  if (!d || !d->label_defs_ci || !name || !*name)
    return NULL;
  char *low = g_ascii_strdown(name, -1);
  if (!low)
    return NULL;
  const char *canon = (const char *)g_hash_table_lookup(d->label_defs_ci, low);
  g_free(low);
  return canon;
}

/* Scan the document and rebuild the label definition index. */
static void doc_rebuild_label_defs(AppState *s, EditorDoc *d) {
  (void)s;
  if (!d || !d->buf)
    return;
  doc_label_index_clear(d);

  GtkTextIter it;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(d->buf), &it);
  while (!gtk_text_iter_is_end(&it)) {
    GtkTextIter line_start = it;
    GtkTextIter line_end = it;
    gtk_text_iter_set_line_offset(&line_start, 0);
    gtk_text_iter_forward_to_line_end(&line_end);

    char *line = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(d->buf), &line_start,
                                          &line_end, FALSE);
    if (!line) {
      gtk_text_iter_forward_line(&it);
      continue;
    }

    // Skip leading whitespace.
    int i = 0;
    while (line[i] == ' ' || line[i] == '\t')
      i++;

    // Ignore empty or comment-only lines.
    if (line[i] == 0 || line[i] == ';') {
      g_free(line);
      gtk_text_iter_forward_line(&it);
      continue;
    }

    // Branch labels ":+" / ":-" as standalone label definitions at line start.
    if (line[i] == ':' && (line[i + 1] == '+' || line[i + 1] == '-') &&
        (line[i + 2] == 0 || line[i + 2] == ' ' || line[i + 2] == '\t' ||
         line[i + 2] == ';')) {
      int base_off = gtk_text_iter_get_offset(&line_start);
      doc_label_index_put(d, (line[i + 1] == '+') ? ":+" : ":-", base_off + i,
                          base_off + i + 2);
      g_free(line);
      gtk_text_iter_forward_line(&it);
      continue;
    }

    // Parse a potential label at start of line.
    int j = i;
    if (!gui_is_ident_start((unsigned char)line[j])) {
      g_free(line);
      gtk_text_iter_forward_line(&it);
      continue;
    }
    while (gui_is_label_char((unsigned char)line[j]))
      j++;
    if (j == i) {
      g_free(line);
      gtk_text_iter_forward_line(&it);
      continue;
    }

    char tok[128];
    int tlen = j - i;
    if (tlen >= (int)sizeof(tok))
      tlen = (int)sizeof(tok) - 1;
    memcpy(tok, line + i, (size_t)tlen);
    tok[tlen] = 0;

    // If the first token is an instruction/directive/register/number, it's not
    // a label definition — but check for macro keyword first.
    if (is_directive_token(tok) &&
        (streqi(tok, "macro") || streqi(tok, ".macro"))) {
      // This is a macro definition line: .macro NAME or macro NAME
      // Index the NAME token into macro_defs.
      int mk = j;
      while (line[mk] == ' ' || line[mk] == '\t')
        mk++;
      if (line[mk] && gui_is_ident_start((unsigned char)line[mk])) {
        int me = mk;
        while (gui_is_label_char((unsigned char)line[me]))
          me++;
        if (me > mk && d->macro_defs) {
          char mname[128];
          int mlen = me - mk;
          if (mlen >= (int)sizeof(mname))
            mlen = (int)sizeof(mname) - 1;
          memcpy(mname, line + mk, (size_t)mlen);
          mname[mlen] = 0;
          char *mlow = g_ascii_strdown(mname, -1);
          if (mlow)
            g_hash_table_replace(d->macro_defs, mlow, g_strdup(""));
        }
      }
      g_free(line);
      gtk_text_iter_forward_line(&it);
      continue;
    }
    if (is_instruction_token(tok) || is_directive_token(tok) ||
        is_register_token(tok) || is_number_token(tok)) {
      g_free(line);
      gtk_text_iter_forward_line(&it);
      continue;
    }

    int is_def = 0;
    int k = j;
    if (line[k] == ':') {
      is_def = 1;
    } else if (line[k] == '=') {
      /* Equate: ZP_STR = &70  — treat as a label definition */
      is_def = 1;
    } else {
      // Allow label definitions of the form "label <opcode>", "label = val", or
      // "label".
      while (line[k] == ' ' || line[k] == '\t')
        k++;
      if (line[k] == 0 || line[k] == ';') {
        is_def = 1;
      } else if (line[k] == '=') {
        /* Equate with spaces: ZP_STR = &70 */
        is_def = 1;
      } else {
        // Next token should be an opcode or directive to consider this a label.
        int m = k;
        char next[128];
        int nlen = 0;
        // Accept optional '.' or '#'
        while (line[m] && !isspace((unsigned char)line[m]) && line[m] != ';' &&
               nlen < (int)sizeof(next) - 1) {
          next[nlen++] = line[m++];
        }
        next[nlen] = 0;
        if (is_instruction_token(next) || is_directive_token(next) ||
            (next[0] == '#' && is_directive_token(next + 1)))
          is_def = 1;
      }
    }

    if (is_def) {
      int base_off = gtk_text_iter_get_offset(&line_start);
      doc_label_index_put(d, tok, base_off + i, base_off + i + (j - i));
      // If the label begins with '.', also index without the dot (so "test"
      // finds ".test").
      if (tok[0] == '.' && tok[1] &&
          !g_hash_table_contains(d->label_defs, tok + 1)) {
        doc_label_index_put(d, tok + 1, base_off + i, base_off + i + (j - i));
      }
    }

    g_free(line);
    gtk_text_iter_forward_line(&it);
  }
}

static int is_register_token(const char *t) {
  return streqi(t, "A") || streqi(t, "X") || streqi(t, "Y") || streqi(t, "S") ||
         streqi(t, "SP") || streqi(t, "P");
}

/* Number-token classifier — returns a sub-kind:
 *   0  = not a number
 *   1  = hex  ($xx / &xx / #$xx / #&xx)
 *   2  = dec  (decimal digits / #decimal)
 *   3  = bin  (%xx / #%xx)
 *   4  = char (#'x')
 * The old boolean callers still work: any non-zero means "is a number". */
static int is_number_token(const char *t) {
  if (!t || !*t)
    return 0;
  /* bare address forms */
  if (t[0] == '$' || t[0] == '&')
    return (t[1] != 0) ? 1 : 0;
  if (t[0] == '%')
    return (t[1] != 0) ? 3 : 0;
  if (isdigit((unsigned char)t[0]))
    return 2;
  /* immediate prefix # */
  if (t[0] == '#') {
    if (t[1] == '$' || t[1] == '&')
      return 1;
    if (t[1] == '%')
      return 3;
    if (isdigit((unsigned char)t[1]))
      return 2;
    if (t[1] == '\'')
      return 4;
  }
  return 0;
}

/* Given a token already known to be numeric (is_number_token != 0),
 * return the sub-kind accounting for a possible leading '#'. */
static int num_subkind(const char *t) {
  if (!t || !*t)
    return 0;
  const char *p = (t[0] == '#') ? t + 1 : t;
  if (p[0] == '$' || p[0] == '&')
    return 1;
  if (p[0] == '%')
    return 3;
  if (isdigit((unsigned char)p[0]))
    return 2;
  if (p[0] == '\'')
    return 4;
  return 2;
}

static int is_data_directive_token(const char *t) {
  if (!t || !*t)
    return 0;
  // Data-emission directives — coloured differently from structural directives.
  const char *data_dirs[] = {"BYTE",   ".BYTE", "WORD",  ".WORD",  "DWORD",
                             ".DWORD", "DB",    "DQ",    "DD",     "EQUB",
                             ".EQUB",  "EQUW",  ".EQUW", "EQUD",   ".EQUD",
                             "EQUS",   ".EQUS", "EQB",   "EQW",    "EQD",
                             "RES",    ".RES",  "ALIGN", ".ALIGN", NULL};
  for (int i = 0; data_dirs[i]; i++) {
    if (streqi(t, data_dirs[i]))
      return 1;
  }
  return 0;
}

static int is_directive_token(const char *t) {
  if (!t || !*t)
    return 0;
  // IMPORTANT: do NOT treat arbitrary dot-prefixed symbols as directives.
  // Many users use dot-prefixed *labels* like `.loop` or `.local`. Those must
  // NOT be auto-formatted. Only known assembler directives should match.

  // Accept both dotted and non-dotted variants.
  const char *dirs[] = {
      "ORG", ".ORG", "EQUB", ".EQUB", "EQUW", ".EQUW", "EQUS", ".EQUS", "EQU",
      ".EQU", "DEF", ".DEF", "CPU", ".CPU", "INCLUDE", ".INCLUDE",
      "INCLUDEROOT", ".INCLUDEROOT", "INCBIN", ".INCBIN", "MACRO", ".MACRO",
      "ENDMACRO", ".ENDMACRO", "LABELS", ".LABELS", "IF", ".IF", "IFDEF",
      ".IFDEF", "IFNDEF", ".IFNDEF", "ELSE", ".ELSE", "ELSEIF", ".ELSEIF",
      "ENDIF", ".ENDIF", "#INCLUDE", "#INCLUDEROOT", "#DEFINE", "#UNDEF", "#IF",
      "#IFDEF", "#IFNDEF", "#ELSE", "#ELSEIF", "#ENDIF", ".IF", ".IFDEF",
      ".IFNDEF", ".IFNDEF", ".ELSE", ".ELSEIF", ".ENDIF", "ASSERT", ".ASSERT",
      "PROC", ".PROC", "ENDPROC", ".ENDPROC", "SCOPE", ".SCOPE", "ENDSCOPE",
      ".ENDSCOPE", "REPEAT", ".REPEAT", "ENDREPEAT", ".ENDREPEAT", "LONGA",
      ".LONGA", "LONGI", ".LONGI", /* 65816 */
      "HALT", ".HALT", "UNHALT", ".UNHALT",
      "ALERT", ".ALERT", "UNALERT", ".UNALERT", /* guard directives */
      "PRAGMA", ".PRAGMA", /* pragma once */
      "OPT", ".OPT", "RES", ".RES", "ALIGN", ".ALIGN", "WARNING", ".WARNING",
      "ERROR", ".ERROR",
      /* Boolean operators for .if expressions */
      "OR", ".OR", "AND", ".AND", "NOT", ".NOT", NULL};

  for (int i = 0; dirs[i]; i++) {
    if (streqi(t, dirs[i]))
      return 1;
  }
  return 0;
}

static int is_instruction_token(const char *t) {
  if (!t || !*t)
    return 0;
  const char *ops[] = {
      "ADC", "AND", "ASL", "BCC", "BCS", "BEQ", "BIT", "BMI", "BNE", "BPL",
      "BRK", "BVC", "BVS", "CLC", "CLD", "CLI", "CLV", "CMP", "CPX", "CPY",
      "DEC", "DEX", "DEY", "EOR", "INC", "INX", "INY", "JMP", "JSR", "LDA",
      "LDX", "LDY", "LSR", "NOP", "ORA", "PHA", "PHP", "PLA", "PLP", "ROL",
      "ROR", "RTI", "RTS", "SBC", "SEC", "SED", "SEI", "STA", "STX", "STY",
      "TAX", "TAY", "TSX", "TXA", "TXS", "TYA",

      // WDC 65C02 additions
      "BRA", "PHX", "PHY", "PLX", "PLY", "STZ", "TRB", "TSB", "WAI", "STP",
      // WDC 65816 additions
      "XCE", "REP", "SEP", "JML", "JSL", "RTL", "BRL", "MVN", "MVP", "PEA",
      "PEI", "PER", "PHB", "PHD", "PHK", "PLB", "PLD", "TCD", "TCS", "TDC",
      "TSC", "TXY", "TYX", "XBA", "WDM", NULL};
  for (int i = 0; ops[i]; i++)
    if (streqi(t, ops[i]))
      return 1;
  return 0;
}

// ---------------- auto formatter ----------------

static char fmt_case_alpha(char c, bool upper) {
  if (c >= 'A' && c <= 'Z')
    return upper ? c : (char)(c - 'A' + 'a');
  if (c >= 'a' && c <= 'z')
    return upper ? (char)(c - 'a' + 'A') : c;
  return c;
}

static char fmt_case_hex(char c, bool upper) {
  if (c >= 'A' && c <= 'F')
    return upper ? c : (char)(c - 'A' + 'a');
  if (c >= 'a' && c <= 'f')
    return upper ? (char)(c - 'a' + 'A') : c;
  return c;
}

// Find a token boundary for simple token scanning (spaces, tabs, commas).
static bool fmt_is_delim(char c) {
  return (c == ' ' || c == '\t' || c == ',' || c == '\r' || c == '\n');
}

static void fmt_append_token(GString *out, const char *tok, size_t len,
                             bool upper, bool allow_mnemonic, bool allow_hex) {
  if (!tok || len == 0)
    return;

  char tmp[128];
  if (len >= sizeof(tmp)) {
    // Too big for stack temp; just append raw.
    g_string_append_len(out, tok, (gssize)len);
    return;
  }
  memcpy(tmp, tok, len);
  tmp[len] = 0;

  if (allow_mnemonic && is_instruction_token(tmp)) {
    for (size_t i = 0; i < len; i++)
      tmp[i] = fmt_case_alpha(tmp[i], upper);
    g_string_append(out, tmp);
    return;
  }

  if (allow_hex) {
    // Numbers can be prefixed (e.g. #&ffe3). Keep the prefix as-is.
    const char *num = tmp;
    if (num[0] == '#')
      num++;
    if (is_number_token(num)) {
      for (size_t i = 0; i < len; i++)
        tmp[i] = fmt_case_hex(tmp[i], upper);
      g_string_append(out, tmp);
      return;
    }
  }

  g_string_append_len(out, tok, (gssize)len);
}

static char *format_source_text(const char *in, bool upper) {
  if (!in)
    return g_strdup("");

  GString *out = g_string_sized_new(strlen(in) + 32);
  bool in_block = false;
  bool in_string = false;
  bool bol = true;
  bool first_token = false;
  bool skip_mnemonic_this_line = false;

  const char *p = in;
  while (*p) {
    // Newline resets per-line state (but not block comment).
    if (*p == '\n') {
      g_string_append_c(out, *p++);
      bol = true;
      first_token = false;
      skip_mnemonic_this_line = false;
      in_string = false; // strings are not allowed to span lines in our asm
      continue;
    }

    if (bol) {
      // True start of a new line (after emitting \n)
      bol = false;
      first_token = true;
      skip_mnemonic_this_line = false;
    }

    // If we're inside a block comment, copy until we see */.
    if (in_block) {
      if (p[0] == '*' && p[1] == '/') {
        g_string_append_len(out, p, 2);
        p += 2;
        in_block = false;
      } else {
        g_string_append_c(out, *p++);
      }
      continue;
    }

    // If we're inside a string, copy until closing quote (no formatting).
    if (in_string) {
      g_string_append_c(out, *p);
      if (*p == '"')
        in_string = false;
      p++;
      continue;
    }

    // Comment starts (line comments): ';' or '//' (outside strings).
    if (*p == ';' || (p[0] == '/' && p[1] == '/')) {
      // Copy rest of line verbatim (until \n or NUL).
      while (*p && *p != '\n')
        g_string_append_c(out, *p++);
      continue;
    }

    // Block comment start.
    if (p[0] == '/' && p[1] == '*') {
      g_string_append_len(out, p, 2);
      p += 2;
      in_block = true;
      continue;
    }

    // String start.
    if (*p == '"') {
      g_string_append_c(out, *p++);
      in_string = true;
      continue;
    }

    // Delimiters pass-through.
    if (fmt_is_delim(*p)) {
      g_string_append_c(out, *p++);
      continue;
    }

    // Token.
    const char *t0 = p;
    while (*p && !fmt_is_delim(*p)) {
      // Stop token before comment start markers so they are handled above.
      if (p[0] == ';' || (p[0] == '/' && (p[1] == '/' || p[1] == '*')))
        break;
      if (*p == '"')
        break;
      p++;
    }
    size_t tlen = (size_t)(p - t0);
    if (tlen == 0) {
      // Shouldn't happen, but avoid stalling.
      g_string_append_c(out, *p++);
      continue;
    }

    // Copy token to a NUL-terminated buffer for classification.
    char stack_tok[128];
    char *tok = NULL;
    if (tlen < sizeof(stack_tok)) {
      memcpy(stack_tok, t0, tlen);
      stack_tok[tlen] = 0;
      tok = stack_tok;
    } else {
      tok = g_strndup(t0, tlen);
    }

    // Decide what we are allowed to modify for this token.
    bool allow_mnemonic = true;

    if (first_token) {
      // Never touch a leading label token.
      // - Labels like "foo:".
      // - Local labels like ".loop" (but not directives like ".org").
      if (tok[tlen - 1] == ':') {
        allow_mnemonic = false;
      } else if (tok[0] == '.' && !is_directive_token(tok)) {
        allow_mnemonic = false;
      }
    }

    // Directives: upper/lower case the directive keyword itself.
    // Do NOT touch .labels (treated like a marker rather than a directive
    // keyword).
    if (is_directive_token(tok) && g_ascii_strcasecmp(tok, ".labels") != 0 &&
        g_ascii_strcasecmp(tok, "labels") != 0) {
      for (size_t k = 0; k < tlen; k++)
        g_string_append_c(out, fmt_case_alpha(tok[k], upper));

      // Mnemonics: upper/lower case the whole token.
    } else if (allow_mnemonic && is_instruction_token(tok)) {
      for (size_t k = 0; k < tlen; k++)
        g_string_append_c(out, fmt_case_alpha(tok[k], upper));
    } else {
      // Numbers: only case-adjust hex digits a-f/A-F.
      const char *numtok = tok;
      if (numtok[0] == '#')
        numtok++;
      if (is_number_token(numtok)) {
        for (size_t k = 0; k < tlen; k++) {
          char c = tok[k];
          if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
            c = upper ? (char)(c & ~0x20) : (char)(c | 0x20);
          g_string_append_c(out, c);
        }
      } else {
        g_string_append_len(out, tok, (gssize)tlen);
      }
    }

    if (tok != stack_tok)
      g_free(tok);
    first_token = false;
  }

  // Finish case-format pass.
  char *res = g_string_free(out, FALSE);
  return res;
}

static gboolean is_space_char(char c) {
  return c == ' ' || c == '\t' || c == '\r';
}

static char *format_source_text_layout(const char *in) {
  if (!in)
    return g_strdup("");

  GString *out = g_string_new(NULL);
  gboolean in_block = FALSE;
  int prev_kind = 0; // 0 other/blank, 1 instruction, 2 label/directive

  const char *p = in;
  while (*p) {
    // Grab one line (without '\n')
    const char *line_start = p;
    const char *line_end = strchr(p, '\n');
    size_t len = line_end ? (size_t)(line_end - p) : strlen(p);

    // Copy to mutable buffer
    char *line = g_strndup(line_start, len);

    if (in_block) {
      g_string_append(out, line);
      if (line_end)
        g_string_append_c(out, '\n');
      if (strstr(line, "*/"))
        in_block = FALSE;
      g_free(line);
      p = line_end ? (line_end + 1) : (line_start + len);
      prev_kind = 0;
      continue;
    }

    // Trim left to detect comment-only / label/directive
    char *t = line;
    while (*t && is_space_char(*t))
      t++;

    // comment-only lines: preserve as-is
    if (*t == ';' || (t[0] == '/' && t[1] == '/')) {
      g_string_append(out, line);
      if (line_end)
        g_string_append_c(out, '\n');
      g_free(line);
      p = line_end ? (line_end + 1) : (line_start + len);
      prev_kind = 0;
      continue;
    }

    // Find inline comment start, respecting strings
    gboolean in_sq = FALSE, in_dq = FALSE;
    size_t cpos = (size_t)-1;
    for (size_t i = 0; i < len; i++) {
      char c = line[i];
      char n = (i + 1 < len) ? line[i + 1] : '\0';
      if (!in_sq && c == '"') {
        in_dq = !in_dq;
        continue;
      }
      if (!in_dq && c == '\'') {
        in_sq = !in_sq;
        continue;
      }

      if (!in_sq && !in_dq) {
        if (c == ';') {
          cpos = i;
          break;
        }
        if (c == '/' && n == '/') {
          cpos = i;
          break;
        }
        if (c == '/' && n == '*') {
          cpos = i;
          in_block = TRUE;
          break;
        }
      }
    }

    char *code = NULL;
    char *comm = NULL;
    if (cpos != (size_t)-1) {
      code = g_strndup(line, cpos);
      comm = g_strdup(line + cpos);
    } else {
      code = g_strdup(line);
      comm = NULL;
    }

    // Trim code
    char *cs = code;
    while (*cs && is_space_char(*cs))
      cs++;
    char *ce = cs + strlen(cs);
    while (ce > cs && is_space_char(ce[-1]))
      ce--;
    *ce = '\0';

    int kind = 0;
    char *newcode = NULL;

    if (*cs == '\0') {
      // blank or only comment
      newcode = g_strdup("");
      kind = 0;
    } else {
      gboolean is_dir = (*cs == '.' && g_ascii_isalpha((guchar)cs[1]));
      // label if first token has ':' before whitespace
      gboolean is_lbl = FALSE;
      size_t tok_len = 0;
      while (cs[tok_len] && !is_space_char(cs[tok_len]))
        tok_len++;
      for (size_t j = 0; j < tok_len; j++) {
        if (cs[j] == ':') {
          is_lbl = TRUE;
          break;
        }
      }

      if (is_dir || is_lbl) {
        kind = 2;
        if (is_lbl) {
          char *colon = strchr(cs, ':');
          if (colon) {
            size_t lhs_len = (size_t)(colon - cs) + 1;
            char *lhs = g_strndup(cs, lhs_len);
            char *rhs = g_strdup(colon + 1);
            g_strstrip(rhs);
            if (rhs && *rhs) {
              newcode = g_strdup_printf("%s\t%s", lhs, rhs);
            } else {
              newcode = g_strdup(lhs);
            }
            g_free(lhs);
            g_free(rhs);
          } else {
            newcode = g_strdup(cs);
          }
        } else {
          newcode = g_strdup(cs);
        }
      } else {
        kind = 1;
        newcode = g_strdup_printf("\t%s", cs);
      }
    }

    // Prepare comment part (trim left)
    char *newcomm = NULL;
    if (comm) {
      char *x = comm;
      while (*x && is_space_char(*x))
        x++;
      newcomm = g_strdup(x);
    }

    // Insert blank line before label/directive if previous line was instruction
    // and we don't already have a blank line at the end.
    if (kind == 2 && prev_kind == 1) {
      // Check if out already ends with "\n\n"
      size_t outlen = out->len;
      if (outlen >= 1 && out->str[outlen - 1] == '\n') {
        if (outlen >= 2 && out->str[outlen - 2] != '\n') {
          g_string_append_c(out, '\n');
        }
      } else if (outlen > 0) {
        g_string_append_c(out, '\n');
      }
    }

    // Emit line
    if (newcode && *newcode) {
      g_string_append(out, newcode);
      if (newcomm && *newcomm) {
        g_string_append_c(out, ' ');
        g_string_append(out, newcomm);
      }
    } else {
      // Preserve pure comment spacing if no code
      if (newcomm && *newcomm) {
        g_string_append(out, newcomm);
      }
    }

    if (line_end)
      g_string_append_c(out, '\n');

    prev_kind = kind;

    g_free(line);
    g_free(code);
    g_free(comm);
    g_free(newcode);
    g_free(newcomm);

    p = line_end ? (line_end + 1) : (line_start + len);
  }

  return g_string_free(out, FALSE);
}

/* Convert the entire source buffer to uppercase or lowercase. */
static void format_buffer_case(AppState *s, bool upper) {
  if (!s || !s->buf)
    return;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  GtkTextIter a, b;
  gtk_text_buffer_get_start_iter(tb, &a);
  gtk_text_buffer_get_end_iter(tb, &b);

  GtkTextMark *ins = gtk_text_buffer_get_insert(tb);
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark(tb, &cur, ins);
  int cur_off = gtk_text_iter_get_offset(&cur);

  char *text = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
  if (!text)
    return;

  char *out = format_source_text(text, upper);
  if (!out) {
    g_free(text);
    return;
  }

  gtk_text_buffer_begin_user_action(tb);
  gtk_text_buffer_set_text(tb, out, -1);

  // Restore cursor as best we can.
  gtk_text_buffer_get_end_iter(tb, &b);
  int max_off = gtk_text_iter_get_offset(&b);
  if (cur_off > max_off)
    cur_off = max_off;
  gtk_text_buffer_get_iter_at_offset(tb, &cur, cur_off);
  gtk_text_buffer_place_cursor(tb, &cur);
  gtk_text_buffer_end_user_action(tb);

  g_free(out);
  g_free(text);
}

/* ---------------- Find/Replace bar (Option B) ---------------- */

static void findbar_set_replace_mode(AppState *s, gboolean replace_mode) {
  s->findbar_replace_mode = replace_mode ? TRUE : FALSE;
  if (!s->replace_label || !s->replace_entry || !s->replace_btn ||
      !s->replace_all_btn)
    return;
  gtk_widget_set_visible(s->replace_label, s->findbar_replace_mode);
  gtk_widget_set_visible(s->replace_entry, s->findbar_replace_mode);
  gtk_widget_set_visible(s->replace_btn, s->findbar_replace_mode);
  gtk_widget_set_visible(s->replace_all_btn, s->findbar_replace_mode);
}

static void hide_findbar(AppState *s) {
  if (!s || !s->findbar)
    return;
  gtk_widget_set_visible(s->findbar, FALSE);
  /* Clear search highlights when findbar closes */
  if (s->search_settings)
    gtk_source_search_settings_set_search_text(s->search_settings, NULL);
  /* Return focus to editor */
  if (s->view)
    gtk_widget_grab_focus(GTK_WIDGET(s->view));
}

static gboolean do_find(AppState *s, gboolean forward) {
  if (!s || !s->buf || !s->view || !s->find_entry)
    return FALSE;
  const char *q = gtk_editable_get_text(GTK_EDITABLE(s->find_entry));
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  if (!s->search_ctx)
    search_context_init(s);
  if (s->search_settings) {
    gtk_source_search_settings_set_search_text(s->search_settings,
                                               (q && *q) ? q : NULL);
    gtk_source_search_settings_set_case_sensitive(s->search_settings,
                                                  s->find_match_case);
  }
  if (!q || !*q)
    return FALSE;
  if (!s->search_ctx)
    return FALSE;

  GtkTextIter start, match_start, match_end;
  GtkTextIter sel_start, sel_end;
  gboolean has_sel =
      gtk_text_buffer_get_selection_bounds(tb, &sel_start, &sel_end);
  start = forward ? (has_sel ? sel_end : sel_start)
                  : (has_sel ? sel_start : sel_end);

  gboolean found = FALSE, wrapped = FALSE;
  if (forward)
    found = gtk_source_search_context_forward(
        s->search_ctx, &start, &match_start, &match_end, &wrapped);
  else
    found = gtk_source_search_context_backward(
        s->search_ctx, &start, &match_start, &match_end, &wrapped);

  if (found) {
    gtk_text_buffer_select_range(tb, &match_start, &match_end);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &match_start, 0.2,
                                 FALSE, 0.0, 0.0);
    return TRUE;
  }

  /* Not found in current tab — if scope > 0, try other tabs in order. */
  if (s->find_scope > 0 && s->notebook) {
    GPtrArray *files = (s->find_scope == 2)
                           ? collect_linked_files(s)
                           : NULL; /* for scope 1 we use notebook pages */
    const int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s->notebook));
    int cur_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook));
    /* Try pages after current first, then wrap from beginning */
    for (int delta = 1; delta < n; delta++) {
      int try_page = (cur_page + (forward ? delta : n - delta)) % n;
      GtkWidget *pg = gtk_notebook_get_nth_page(GTK_NOTEBOOK(s->notebook), try_page);
      if (!pg) continue;
      EditorDoc *d = (EditorDoc *)g_object_get_data(G_OBJECT(pg), "app-doc");
      if (!d) continue;
      /* Scope 2: only search files in the include tree */
      if (s->find_scope == 2 && files && d->path) {
        gboolean in_tree = FALSE;
        for (guint fi = 0; fi < files->len; fi++) {
          char *fc = g_canonicalize_filename((char *)files->pdata[fi], NULL);
          char *dc = d->path ? g_canonicalize_filename(d->path, NULL) : NULL;
          if (fc && dc && strcmp(fc, dc) == 0) in_tree = TRUE;
          g_free(fc); g_free(dc);
          if (in_tree) break;
        }
        if (!in_tree) continue;
      }
      gtk_notebook_set_current_page(GTK_NOTEBOOK(s->notebook), try_page);
      /* s->buf now points to the new tab */
      GtkTextBuffer *tb2 = GTK_TEXT_BUFFER(s->buf);
      GtkTextIter s2, me2, ms2;
      if (forward)
        gtk_text_buffer_get_start_iter(tb2, &s2);
      else
        gtk_text_buffer_get_end_iter(tb2, &s2);
      gboolean f2 = FALSE, w2 = FALSE;
      if (s->search_ctx) {
        /* Reinit search context for new buffer */
        search_context_init(s);
        if (s->search_settings)
          gtk_source_search_settings_set_search_text(s->search_settings, q);
        f2 = forward
            ? gtk_source_search_context_forward(s->search_ctx, &s2, &ms2, &me2, &w2)
            : gtk_source_search_context_backward(s->search_ctx, &s2, &ms2, &me2, &w2);
      }
      if (f2) {
        gtk_text_buffer_select_range(tb2, &ms2, &me2);
        gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &ms2, 0.2, FALSE, 0.0, 0.0);
        if (files) g_ptr_array_free(files, TRUE);
        return TRUE;
      }
    }
    if (files) g_ptr_array_free(files, TRUE);
  }
  return FALSE;
}

/* Perform a single find-and-replace using the current find/replace bar text. */
static void do_replace_once(AppState *s) {
  if (!s || !s->buf || !s->find_entry || !s->replace_entry)
    return;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  const char *q = gtk_editable_get_text(GTK_EDITABLE(s->find_entry));
  if (!q || !*q)
    return;

  GtkTextIter a, b;
  if (!gtk_text_buffer_get_selection_bounds(tb, &a, &b)) {
    // nothing selected -> find next
    do_find(s, TRUE);
    return;
  }

  char *sel = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
  gboolean matches = sel && strcmp(sel, q) == 0;
  g_free(sel);

  if (!matches) {
    // selection doesn't match, find next
    do_find(s, TRUE);
    return;
  }

  const char *rep = gtk_editable_get_text(GTK_EDITABLE(s->replace_entry));
  if (!rep)
    rep = "";

  gtk_text_buffer_begin_user_action(tb);
  gtk_text_buffer_delete(tb, &a, &b);
  gtk_text_buffer_insert(tb, &a, rep, -1);
  gtk_text_buffer_end_user_action(tb);

  // select the replaced text then find next
  GtkTextIter after;
  gtk_text_buffer_get_iter_at_mark(tb, &after, gtk_text_buffer_get_insert(tb));
  gtk_text_buffer_select_range(tb, &a, &after);
  do_find(s, TRUE);
}

static void do_replace_all(AppState *s) {
  if (!s || !s->find_entry || !s->replace_entry)
    return;

  const char *q = gtk_editable_get_text(GTK_EDITABLE(s->find_entry));
  if (!q || !*q) return;
  const char *rep = gtk_editable_get_text(GTK_EDITABLE(s->replace_entry));
  if (!rep) rep = "";

  int scope = s->find_scope; /* 0=This Tab, 1=All Tabs, 2=All Linked Files */

  if (scope == 0) {
    /* Original single-buffer replace */
    if (!s->buf) return;
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
    int count = 0;
    gtk_text_buffer_begin_user_action(tb);
    if (s->search_ctx) {
      GError *err = NULL;
      count = gtk_source_search_context_replace_all(s->search_ctx, rep, -1, &err);
      if (err) { g_error_free(err); count = 0; }
    } else {
      GtkTextIter iter;
      gtk_text_buffer_get_start_iter(tb, &iter);
      GtkTextSearchFlags flags = GTK_TEXT_SEARCH_TEXT_ONLY;
      if (!s->find_match_case)
        flags = (GtkTextSearchFlags)(flags | GTK_TEXT_SEARCH_CASE_INSENSITIVE);
      GtkTextIter m1, m2;
      while (gtk_text_iter_forward_search(&iter, q, flags, &m1, &m2, NULL)) {
        gtk_text_buffer_delete(tb, &m1, &m2);
        gtk_text_buffer_insert(tb, &m1, rep, -1);
        count++;
        iter = m1;
      }
    }
    gtk_text_buffer_end_user_action(tb);
    char msg[128];
    snprintf(msg, sizeof(msg), count > 0 ? "Replaced %d occurrence(s)." : "No matches.", count);
    set_status(s, msg);
    return;
  }

  /* Scope 1: All open tabs */
  if (scope == 1) {
    int total = 0;
    const int n = gtk_notebook_get_n_pages(GTK_NOTEBOOK(s->notebook));
    for (int i = 0; i < n; i++) {
      GtkWidget *page = gtk_notebook_get_nth_page(GTK_NOTEBOOK(s->notebook), i);
      if (!page) continue;
      EditorDoc *d = (EditorDoc *)g_object_get_data(G_OBJECT(page), "app-doc");
      if (!d || !d->path) continue;
      total += replace_all_in_file(s, d->path, q, rep, s->find_match_case);
    }
    char msg[128];
    snprintf(msg, sizeof(msg), total > 0 ? "Replaced %d occurrence(s) across all tabs." : "No matches in any tab.", total);
    set_status(s, msg);
    return;
  }

  /* Scope 2: All linked files (root + all includes) */
  GPtrArray *files = collect_linked_files(s);
  int total = 0;
  for (guint i = 0; i < files->len; i++)
    total += replace_all_in_file(s, (const char *)files->pdata[i], q, rep, s->find_match_case);
  g_ptr_array_free(files, TRUE);
  char msg[160];
  snprintf(msg, sizeof(msg), total > 0 ? "Replaced %d occurrence(s) across linked files." : "No matches in linked files.", total);
  set_status(s, msg);
}

/* Key handler for the find/replace bar: Enter finds next, Escape closes. */
static gboolean findbar_key_cb(GtkEventControllerKey *controller, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data) {
  (void)controller;
  (void)keycode;
  AppState *s = (AppState *)user_data;
  if (!s)
    return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Escape) {
    hide_findbar(s);
    return GDK_EVENT_STOP;
  }

  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    gboolean backward = (state & GDK_SHIFT_MASK) ? TRUE : FALSE;
    do_find(s, backward ? FALSE : TRUE);
    return GDK_EVENT_STOP;
  }

  return GDK_EVENT_PROPAGATE;
}

// In GTK4, GtkDialog is just a GtkWindow; it does not auto-destroy itself on
// button presses. We centralize the common ???close on response??? behaviour.
static void dialog_destroy_on_response(GtkDialog *dlg, int response,
                                       gpointer user_data) {
  (void)response;
  (void)user_data;
  gtk_window_destroy(GTK_WINDOW(dlg));
}

static void find_prev_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  do_find((AppState *)user_data, FALSE);
}
static void find_next_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  do_find((AppState *)user_data, TRUE);
}
static void replace_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  do_replace_once((AppState *)user_data);
}
static void replace_all_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  do_replace_all((AppState *)user_data);
}
/* Callback: close the find/replace bar. */
static void find_close_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  hide_findbar((AppState *)user_data);
}



static void on_find_scope_changed(GtkDropDown *dd, GParamSpec *ps,
                                   gpointer user_data) {
  (void)ps;
  AppState *s = (AppState *)user_data;
  if (!s) return;
  s->find_scope = (int)gtk_drop_down_get_selected(dd);
}

/* Collect all files reachable from root_path via .include/.#include into
 * `out` (GPtrArray of heap-owned gchar*).  Visited set prevents duplicates. */
static void collect_linked_files_rec(const char *filepath, GHashTable *visited,
                                     GPtrArray *out) {
  if (!filepath || !*filepath) return;
  char *canon = g_canonicalize_filename(filepath, NULL);
  if (!canon) canon = g_strdup(filepath);
  if (g_hash_table_contains(visited, canon)) { g_free(canon); return; }
  g_hash_table_add(visited, canon); /* visited takes ownership */
  g_ptr_array_add(out, g_strdup(filepath));

  FILE *fp = fopen(filepath, "rb");
  if (!fp) return;
  char line[4096];
  while (fgets(line, sizeof(line), fp)) {
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == ';') continue;
    if (*p != '.' && *p != '#') continue;
    p++;
    const char *name = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    size_t nlen = (size_t)(p - name);
    if (nlen == 7 && g_ascii_strncasecmp(name, "include", 7) == 0) {
      char *raw = it_parse_path_arg_relaxed(line);
      if (!raw) continue;
      char *resolved = it_resolve_include(filepath, raw, NULL);
      g_free(raw);
      if (!resolved) continue;
      if (g_file_test(resolved, G_FILE_TEST_EXISTS))
        collect_linked_files_rec(resolved, visited, out);
      g_free(resolved);
    }
  }
  fclose(fp);
}

static GPtrArray *collect_linked_files(AppState *s) {
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  if (!s) return out;
  /* Find the root file */
  const char *active = s->path && s->path[0] ? s->path : NULL;
  if (!active) return out;
  char *root = it_find_root_for_active(s, active);
  GHashTable *visited = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  collect_linked_files_rec(root ? root : active, visited, out);
  g_hash_table_destroy(visited);
  g_free(root);
  return out;
}

/* Replace all occurrences in a single file path (load, replace, save). */
static int replace_all_in_file(AppState *s, const char *path,
                                const char *q, const char *rep,
                                gboolean case_sensitive) {
  if (!path || !q || !*q) return 0;
  /* Check if this file is an open tab — operate on live buffer if so. */
  int page = app_notebook_page_for_path(s, path);
  if (page >= 0) {
    /* Switch to that tab, run replace-all on its buffer. */
    int prev_page = gtk_notebook_get_current_page(GTK_NOTEBOOK(s->notebook));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(s->notebook), page);
    /* s->buf now points to this tab's buffer */
    GtkTextIter iter;
    gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &iter);
    GtkTextSearchFlags flags = GTK_TEXT_SEARCH_TEXT_ONLY;
    if (!case_sensitive)
      flags = (GtkTextSearchFlags)(flags | GTK_TEXT_SEARCH_CASE_INSENSITIVE);
    GtkTextIter m1, m2;
    int count = 0;
    gtk_text_buffer_begin_user_action(GTK_TEXT_BUFFER(s->buf));
    while (gtk_text_iter_forward_search(&iter, q, flags, &m1, &m2, NULL)) {
      gtk_text_buffer_delete(GTK_TEXT_BUFFER(s->buf), &m1, &m2);
      gtk_text_buffer_insert(GTK_TEXT_BUFFER(s->buf), &m1, rep, -1);
      count++;
      iter = m1;
    }
    gtk_text_buffer_end_user_action(GTK_TEXT_BUFFER(s->buf));
    (void)prev_page;
    return count;
  }
  /* File not open — load, replace in memory, save back. */
  gchar *contents = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &contents, &len, NULL)) return 0;
  GString *result = g_string_new(NULL);
  const char *src = contents;
  size_t qlen = strlen(q);
  int count = 0;
  while (*src) {
    gboolean match = case_sensitive
        ? (strncmp(src, q, qlen) == 0)
        : (g_ascii_strncasecmp(src, q, qlen) == 0);
    if (match) {
      g_string_append(result, rep);
      src += qlen;
      count++;
    } else {
      g_string_append_c(result, *src++);
    }
  }
  g_free(contents);
  if (count > 0)
    g_file_set_contents(path, result->str, (gssize)result->len, NULL);
  g_string_free(result, TRUE);
  return count;
}

static void on_find_case_toggled(GtkCheckButton *btn, gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s)
    return;
  s->find_match_case = gtk_check_button_get_active(btn) ? TRUE : FALSE;
  if (s->search_settings)
    gtk_source_search_settings_set_case_sensitive(s->search_settings,
                                                  s->find_match_case);
}

/* Callback: find-bar text changed; update the live search highlight. */
static void on_find_text_changed(GtkEditable *editable, gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s)
    return;
  if (!s->search_ctx)
    search_context_init(s);
  const char *q = gtk_editable_get_text(editable);
  if (s->search_settings)
    gtk_source_search_settings_set_search_text(s->search_settings,
                                               (q && *q) ? q : NULL);
}

static GtkWidget *create_findbar_widget(AppState *s) {
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(bar, 8);
  gtk_widget_set_margin_end(bar, 8);
  gtk_widget_set_margin_top(bar, 6);
  gtk_widget_set_margin_bottom(bar, 6);

  GtkWidget *find_label = gtk_label_new("Find:");
  gtk_box_append(GTK_BOX(bar), find_label);

  s->find_entry = gtk_entry_new();
  gtk_widget_set_hexpand(s->find_entry, FALSE);
  gtk_widget_set_size_request(s->find_entry, 180, -1);
  gtk_box_append(GTK_BOX(bar), s->find_entry);

  /* Case sensitivity: default off => case-insensitive searches */
  s->find_case_btn = gtk_check_button_new_with_label("Match case");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(s->find_case_btn), FALSE);
  g_signal_connect(s->find_case_btn, "toggled",
                   G_CALLBACK(on_find_case_toggled), s);
  gtk_box_append(GTK_BOX(bar), s->find_case_btn);

  g_signal_connect(s->find_entry, "changed", G_CALLBACK(on_find_text_changed),
                   s);

  s->find_prev_btn = gtk_button_new_with_label("Prev");
  gtk_box_append(GTK_BOX(bar), s->find_prev_btn);
  g_signal_connect(s->find_prev_btn, "clicked", G_CALLBACK(find_prev_clicked),
                   s);

  s->find_next_btn = gtk_button_new_with_label("Next");
  gtk_box_append(GTK_BOX(bar), s->find_next_btn);
  g_signal_connect(s->find_next_btn, "clicked", G_CALLBACK(find_next_clicked),
                   s);

  s->replace_label = gtk_label_new("Replace:");
  gtk_box_append(GTK_BOX(bar), s->replace_label);

  s->replace_entry = gtk_entry_new();
  gtk_widget_set_hexpand(s->replace_entry, FALSE);
  gtk_widget_set_size_request(s->replace_entry, 180, -1);
  gtk_box_append(GTK_BOX(bar), s->replace_entry);

  s->replace_btn = gtk_button_new_with_label("Replace");
  gtk_box_append(GTK_BOX(bar), s->replace_btn);
  g_signal_connect(s->replace_btn, "clicked", G_CALLBACK(replace_clicked), s);

  s->replace_all_btn = gtk_button_new_with_label("Replace All");
  gtk_box_append(GTK_BOX(bar), s->replace_all_btn);
  g_signal_connect(s->replace_all_btn, "clicked",
                   G_CALLBACK(replace_all_clicked), s);

  /* Scope selector */
  const char *scope_items[] = {"This Tab", "All Tabs", "All Linked Files", NULL};
  GtkStringList *scope_list = gtk_string_list_new(scope_items);
  s->find_scope_drop = gtk_drop_down_new(G_LIST_MODEL(scope_list), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(s->find_scope_drop), 0);
  g_signal_connect(s->find_scope_drop, "notify::selected",
                   G_CALLBACK(on_find_scope_changed), s);
  gtk_box_append(GTK_BOX(bar), s->find_scope_drop);


  /* Spacer to push Close to the far right */
  GtkWidget *spacer = gtk_label_new(NULL);
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(bar), spacer);

  s->find_close_btn = gtk_button_new_with_label("Close");
  gtk_box_append(GTK_BOX(bar), s->find_close_btn);
  g_signal_connect(s->find_close_btn, "clicked", G_CALLBACK(find_close_clicked),
                   s);

  // Key handling (Enter = Next, Shift+Enter = Prev, Esc = Close)
  GtkEventController *k1 = gtk_event_controller_key_new();
  g_signal_connect(k1, "key-pressed", G_CALLBACK(findbar_key_cb), s);
  gtk_widget_add_controller(s->find_entry, k1);

  GtkEventController *k2 = gtk_event_controller_key_new();
  g_signal_connect(k2, "key-pressed", G_CALLBACK(findbar_key_cb), s);
  gtk_widget_add_controller(s->replace_entry, k2);

  // Default to Find-only mode
  findbar_set_replace_mode(s, FALSE);

  return bar;
}

static void search_context_init(AppState *s) {
  if (!s || !s->buf)
    return;
  if (s->search_ctx) {
    g_object_unref(s->search_ctx);
    s->search_ctx = NULL;
  }
  if (s->search_settings) {
    g_object_unref(s->search_settings);
    s->search_settings = NULL;
  }
  s->search_settings = gtk_source_search_settings_new();
  gtk_source_search_settings_set_case_sensitive(s->search_settings,
                                                s->find_match_case);
  gtk_source_search_settings_set_wrap_around(s->search_settings, TRUE);
  s->search_ctx = gtk_source_search_context_new(s->buf, s->search_settings);
}

/* Show the find bar (or find+replace bar) and focus the search entry. */
static void show_findbar(AppState *s, gboolean replace_mode) {
  if (!s)
    return;
  if (!s->findbar)
    return;
  if (!s->search_ctx)
    search_context_init(s);
  gtk_widget_set_visible(s->findbar, TRUE);
  findbar_set_replace_mode(s, replace_mode);

  if (s->find_entry) {
    gtk_widget_grab_focus(s->find_entry);
    gtk_editable_select_region(GTK_EDITABLE(s->find_entry), 0, -1);
  }
}

/* Action handler: show the find bar. */
static void action_find(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  show_findbar((AppState *)user_data, FALSE);
}
static void action_replace(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  show_findbar((AppState *)user_data, TRUE);
}

/* Response callback for the Go To Line dialog. */
static void goto_dialog_response(GtkDialog *d, int resp, gpointer user_data) {
  AppState *s = (AppState *)user_data;
  GtkWidget *e = g_object_get_data(G_OBJECT(d), "goto_entry");
  if (resp == GTK_RESPONSE_OK && s && e && GTK_IS_ENTRY(e) && s->buf) {
    const char *t = gtk_editable_get_text(GTK_EDITABLE(e));
    if (t && *t) {
      char *_ge;
      long _gl = strtol(t, &_ge, 10);
      int line = (_ge != t && _gl >= 1) ? (int)_gl : 1;
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(s->buf), &it, line - 1);
      gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(s->buf), &it);
      gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &it, 0.2, FALSE, 0,
                                   0);
    }
  }
  gtk_window_destroy(GTK_WINDOW(d));
}

/* Action handler: show the Go To Line dialog. */
static void action_goto(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;

  GtkWidget *dlg = gtk_dialog_new_with_buttons(
      "Goto line", s->win, GTK_DIALOG_MODAL, "Cancel", GTK_RESPONSE_CANCEL,
      "OK", GTK_RESPONSE_OK, NULL);
  dialog_add_default_keys(dlg, GTK_RESPONSE_OK, GTK_RESPONSE_CANCEL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);

  GtkWidget *lbl = gtk_label_new("Line:");
  GtkWidget *e = gtk_entry_new();
  gtk_widget_set_hexpand(e, TRUE);

  gtk_box_append(GTK_BOX(box), lbl);
  gtk_box_append(GTK_BOX(box), e);
  gtk_box_append(GTK_BOX(area), box);

  g_object_set_data(G_OBJECT(dlg), "goto_entry", e);
  g_signal_connect(dlg, "response", G_CALLBACK(goto_dialog_response), s);

  gtk_window_present(GTK_WINDOW(dlg));
  gtk_widget_grab_focus(e);
}

/* Action handler: duplicate the current line below itself. */
static void action_dup_line(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->buf)
    return;

  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(tb, &it, gtk_text_buffer_get_insert(tb));

  GtkTextIter line_start = it;
  gtk_text_iter_set_line_offset(&line_start, 0);

  GtkTextIter line_end = line_start;
  gtk_text_iter_forward_to_line_end(&line_end);

  /* line_end is at the newline char (if present), or at end-of-buffer.
     Grab the line text *without* its newline, then insert a copy below. */
  gchar *line_text =
      gtk_text_buffer_get_text(tb, &line_start, &line_end, FALSE);
  if (!line_text)
    return;

  gtk_text_buffer_begin_user_action(tb);

  GtkTextIter insert_pos = line_end;
  if (gtk_text_iter_is_end(&insert_pos)) {
    /* Ensure we duplicate onto a new line if the last line has no trailing \n.
     */
    gtk_text_buffer_insert(tb, &insert_pos, "\n", 1);
  } else {
    /* Move past the existing newline to the start of the next line. */
    if (gtk_text_iter_get_char(&insert_pos) == '\n')
      gtk_text_iter_forward_char(&insert_pos);
  }

  gchar *dup = g_strconcat(line_text, "\n", NULL);
  gtk_text_buffer_insert(tb, &insert_pos, dup, -1);
  g_free(dup);

  gtk_text_buffer_end_user_action(tb);
  g_free(line_text);
}

/* ---------------- Comment / Uncomment (Ctrl+K, then C/U) ---------------- */

static gboolean editor_copy_selection_to_clipboard(AppState *s) {
  if (!s || !s->buf || !s->view)
    return FALSE;

  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  GtkTextIter a, b;
  if (!gtk_text_buffer_get_selection_bounds(tb, &a, &b))
    return FALSE;

  char *sel = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
  if (!sel)
    return FALSE;

  GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(s->view));
  if (cb)
    gdk_clipboard_set_text(cb, sel);

  g_free(sel);
  return TRUE;
}

/* Copy the current selection to clipboard with all ; comments stripped.
 * Each line is trimmed of its trailing comment (and any trailing whitespace).
 * If nothing is selected, copies the current line. */
static gboolean editor_copy_without_comments(AppState *s) {
  if (!s || !s->buf || !s->view)
    return FALSE;

  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  GtkTextIter a, b;
  gboolean had_sel = gtk_text_buffer_get_selection_bounds(tb, &a, &b);
  if (!had_sel) {
    /* No selection — use the current line */
    gtk_text_buffer_get_iter_at_mark(tb, &a, gtk_text_buffer_get_insert(tb));
    b = a;
    gtk_text_iter_set_line_offset(&a, 0);
    if (!gtk_text_iter_ends_line(&b))
      gtk_text_iter_forward_to_line_end(&b);
  }

  char *sel = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
  if (!sel)
    return FALSE;

  /* Process line by line: strip ; comment and trailing whitespace */
  GString *out = g_string_new(NULL);
  gchar **lines = g_strsplit(sel, "\n", -1);
  g_free(sel);
  for (gchar **lp = lines; *lp; lp++) {
    gchar *line = *lp;
    gboolean in_str = FALSE;
    char str_ch = 0;
    gchar *comment = NULL;
    for (gchar *p = line; *p; p++) {
      if (in_str) {
        if (*p == str_ch)
          in_str = FALSE;
      } else if (*p == '"' || *p == '\'') {
        in_str = TRUE;
        str_ch = *p;
      } else if (*p == ';') {
        comment = p;
        break;
      }
    }
    if (comment)
      *comment = '\0';
    /* Strip trailing whitespace */
    gchar *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t'))
      *--end = '\0';
    if (lp != lines)
      g_string_append_c(out, '\n');
    g_string_append(out, line);
  }
  g_strfreev(lines);

  GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(s->view));
  if (cb)
    gdk_clipboard_set_text(cb, out->str);
  g_string_free(out, TRUE);
  return TRUE;
}

/* Action handler: copy selection without comments */
static void action_copy(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a; (void)p;
  AppState *s = (AppState *)user_data;
  editor_copy_selection_to_clipboard(s);
}

static void action_cut(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a; (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->buf || !s->view) return;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  GtkTextIter a2, b;
  if (!gtk_text_buffer_get_selection_bounds(tb, &a2, &b)) return;
  char *sel = gtk_text_buffer_get_text(tb, &a2, &b, FALSE);
  if (!sel) return;
  GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(s->view));
  if (cb) gdk_clipboard_set_text(cb, sel);
  g_free(sel);
  gtk_text_buffer_delete_selection(tb, TRUE, TRUE);
}

static void action_paste(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a; (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->view) return;
  GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(s->view));
  if (!cb) return;
  /* gtk_text_view_emit_paste_clipboard triggers the built-in paste action */
  g_signal_emit_by_name(s->view, "paste-clipboard");
}

static void action_copy_without_comments(GSimpleAction *a, GVariant *p,
                                         gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (editor_copy_without_comments(s))
    copy_toast_show(s, "Copied (no comments)");
}

/* Toggle comments on the selected lines (or current line if no selection). */
static void comment_or_uncomment_selection(AppState *s, gboolean uncomment) {
  if (!s || !s->buf)
    return;

  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  GtkTextIter sel_a, sel_b;
  gboolean has_sel = gtk_text_buffer_get_selection_bounds(tb, &sel_a, &sel_b);
  if (!has_sel) {
    gtk_text_buffer_get_iter_at_mark(tb, &sel_a,
                                     gtk_text_buffer_get_insert(tb));
    sel_b = sel_a;
  }

  /* Normalize selection order */
  if (gtk_text_iter_compare(&sel_a, &sel_b) > 0) {
    GtkTextIter tmp = sel_a;
    sel_a = sel_b;
    sel_b = tmp;
  }

  int start_line = gtk_text_iter_get_line(&sel_a);
  int end_line = gtk_text_iter_get_line(&sel_b);

  /* If selection ends at column 0 on a later line, don't include that last line
   */
  if (has_sel && gtk_text_iter_get_line_offset(&sel_b) == 0 &&
      end_line > start_line)
    end_line--;

  gtk_text_buffer_begin_user_action(tb);

  for (int line = start_line; line <= end_line; line++) {
    GtkTextIter line_it;
    gtk_text_buffer_get_iter_at_line(tb, &line_it, line);

    /* Insert/remove after leading whitespace */
    GtkTextIter ws = line_it;
    while (!gtk_text_iter_ends_line(&ws)) {
      gunichar ch = gtk_text_iter_get_char(&ws);
      if (ch != ' ' && ch != '\t')
        break;
      gtk_text_iter_forward_char(&ws);
    }

    if (!uncomment) {
      /* Comment: insert the chosen line-comment marker at first non-ws.
       * If ???keep comment column aligned??? is enabled, pad with spaces so the
       * marker begins at the chosen column. If the code indentation is already
       * beyond the target column, leave it unchanged. */
      const char *marker = (s && s->comment_use_slashslash) ? "//" : ";";
      int marker_len = (s && s->comment_use_slashslash) ? 2 : 1;

      if (s && s->keep_comment_aligned && s->comment_align_col > 0) {
        int col = gtk_text_iter_get_line_offset(&ws);
        if (col < s->comment_align_col) {
          int pad = s->comment_align_col - col;
          if (pad > 0) {
            gchar *spaces = g_malloc((gsize)pad + 1);
            memset(spaces, ' ', (gsize)pad);
            spaces[pad] = 0;
            gtk_text_buffer_insert(tb, &ws, spaces, pad);
            g_free(spaces);
          }
        }
      }

      gtk_text_buffer_insert(tb, &ws, marker, marker_len);

      /* Optional single space after marker */
      gtk_text_buffer_insert(tb, &ws, " ", 1);
      continue;
    }

    /* Uncomment: remove ';' or '//' if present at the first non-ws position.
     * If ???keep comment column aligned??? is enabled, also accept optional
     * spaces between the indentation and the comment marker. */
    GtkTextIter a = ws;
    GtkTextIter b = ws;
    if (gtk_text_iter_ends_line(&a))
      continue;

    gboolean found = FALSE;

    /* Try at first non-ws, then (if aligned) after padding spaces. */
    for (int attempt = 0; attempt < 2 && !found; attempt++) {
      gunichar c1 = gtk_text_iter_get_char(&a);

      if (c1 == ';') {
        b = a;
        gtk_text_iter_forward_char(&b);
        found = TRUE;
        break;
      }

      if (c1 == '/') {
        GtkTextIter t = a;
        gtk_text_iter_forward_char(&t);
        if (!gtk_text_iter_ends_line(&t) && gtk_text_iter_get_char(&t) == '/') {
          b = t;
          gtk_text_iter_forward_char(&b);
          found = TRUE;
          break;
        }
      }

      if (attempt == 0 && (s && s->keep_comment_aligned)) {
        GtkTextIter t = ws;
        while (!gtk_text_iter_ends_line(&t)) {
          gunichar ch = gtk_text_iter_get_char(&t);
          if (ch != ' ')
            break;
          gtk_text_iter_forward_char(&t);
        }
        a = t;
        b = t;
        if (gtk_text_iter_ends_line(&a))
          break;
        continue;
      }
      break;
    }

    if (!found)
      continue;

    gtk_text_buffer_delete(tb, &a, &b);

    /* Optional single following space */
    GtkTextIter sp_a = a;
    GtkTextIter sp_b = a;
    if (!gtk_text_iter_ends_line(&sp_b)) {
      gunichar c = gtk_text_iter_get_char(&sp_b);
      if (c == ' ') {
        gtk_text_iter_forward_char(&sp_b);
        gtk_text_buffer_delete(tb, &sp_a, &sp_b);
      }
    }
  }

  gtk_text_buffer_end_user_action(tb);
}

static gboolean ctrlk_timeout_cb(gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s)
    return G_SOURCE_REMOVE;
  s->chord_ctrlk_pending = FALSE;
  s->chord_ctrlk_timeout_id = 0;
  return G_SOURCE_REMOVE;
}

/* Key handler for Ctrl+K chord sequences (e.g. Ctrl+K,C to comment). */
static gboolean editor_ctrlk_key_pressed(GtkEventControllerKey *controller,
                                         guint keyval, guint keycode,
                                         GdkModifierType state,
                                         gpointer user_data) {
  (void)controller;
  (void)keycode;

  AppState *s = (AppState *)user_data;
  if (!s)
    return FALSE;

  /* Normal copy: Ctrl+C copies only the current selection. */
  if ((state & GDK_CONTROL_MASK) &&
      !(state & (GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
      (keyval == GDK_KEY_c || keyval == GDK_KEY_C)) {
    if (editor_copy_selection_to_clipboard(s))
      return TRUE;
  }

  /* Ctrl+B: toggle bookmark on current line (captured before GtkTextView
     consumes it as Emacs-style backward-char). */
  if ((state & GDK_CONTROL_MASK) &&
      !(state & (GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) &&
      (keyval == GDK_KEY_b || keyval == GDK_KEY_B)) {
    int line = gtk_get_cursor_line(s);
    gtk_bookmark_toggle(s, line);
    return TRUE;
  }

  /* Start chord: Ctrl+K */
  if ((state & GDK_CONTROL_MASK) &&
      (keyval == GDK_KEY_k || keyval == GDK_KEY_K)) {
    s->chord_ctrlk_pending = TRUE;
    if (s->chord_ctrlk_timeout_id)
      g_source_remove(s->chord_ctrlk_timeout_id);
    s->chord_ctrlk_timeout_id = g_timeout_add(1200, ctrlk_timeout_cb, s);
    return TRUE;
  }

  if (!s->chord_ctrlk_pending)
    return FALSE;

  /* Second key of chord */
  if (s->chord_ctrlk_timeout_id) {
    g_source_remove(s->chord_ctrlk_timeout_id);
    s->chord_ctrlk_timeout_id = 0;
  }
  s->chord_ctrlk_pending = FALSE;

  if (keyval == GDK_KEY_c || keyval == GDK_KEY_C) {
    comment_or_uncomment_selection(s, FALSE);
    return TRUE;
  }
  if (keyval == GDK_KEY_u || keyval == GDK_KEY_U) {
    comment_or_uncomment_selection(s, TRUE);
    return TRUE;
  }

  /* Not our chord: let the key go through (don't consume). */
  return FALSE;
}

static void editor_install_ctrlk_chords(AppState *s) {
  if (!s || !s->view)
    return;

  GtkEventController *kc = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
  g_signal_connect(kc, "key-pressed", G_CALLBACK(editor_ctrlk_key_pressed), s);
  gtk_widget_add_controller(GTK_WIDGET(s->view), kc);

  s->chord_ctrlk_pending = FALSE;
  s->chord_ctrlk_timeout_id = 0;
}

static void action_dedupe_lines(GSimpleAction *a, GVariant *p,
                                gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->buf)
    return;

  GtkTextIter a_it, b_it;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &a_it);
  gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(s->buf), &b_it);
  char *text =
      gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &a_it, &b_it, FALSE);
  if (!text)
    return;

  GHashTable *seen =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  GString *out = g_string_new(NULL);

  char *pcur = text;
  while (*pcur) {
    char *line_start = pcur;
    char *nl = strchr(pcur, '\n');
    size_t len = nl ? (size_t)(nl - line_start) : strlen(line_start);

    char *line = g_strndup(line_start, len);
    if (!g_hash_table_contains(seen, line)) {
      g_hash_table_add(seen, g_strdup(line));
      g_string_append_len(out, line_start, len);
      if (nl)
        g_string_append_c(out, '\n');
    }
    g_free(line);

    if (!nl)
      break;
    pcur = nl + 1;
  }

  // Replacing the whole buffer is an irreversible change; GtkTextBuffer will
  // internally start an irreversible action for set_text(). Do not wrap this
  // in begin/end_user_action, otherwise GTK warns about nesting.
  gtk_text_buffer_begin_irreversible_action(GTK_TEXT_BUFFER(s->buf));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(s->buf), out->str, (int)out->len);
  gtk_text_buffer_end_irreversible_action(GTK_TEXT_BUFFER(s->buf));

  g_hash_table_destroy(seen);
  g_string_free(out, TRUE);
  g_free(text);

  set_status(s, "Removed duplicate lines.");
}

static void action_format_layout(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  if (!s || !s->buf)
    return;

  GtkTextIter a_it, b_it;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &a_it);
  gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(s->buf), &b_it);
  char *text =
      gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &a_it, &b_it, FALSE);

  char *out = format_source_text_layout(text);

  gtk_text_buffer_begin_user_action(GTK_TEXT_BUFFER(s->buf));
  gtk_text_buffer_set_text(GTK_TEXT_BUFFER(s->buf), out, -1);
  gtk_text_buffer_end_user_action(GTK_TEXT_BUFFER(s->buf));

  g_free(text);
  g_free(out);

  set_status(s, "Formatted layout.");
}

/* ---------------- custom highlighting using tags ---------------- */

static void remove_all_tags(AppState *s, GtkTextIter *a, GtkTextIter *b) {
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  gtk_text_buffer_remove_tag(tb, s->tag_instr, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_dir, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_data, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_num, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_label, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_reg, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_comment, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_string, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_addr_hex, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_addr_dec, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_addr_bin, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_imm_hex, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_imm_dec, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_imm_bin, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_imm_char, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_dv_hex, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_dv_dec, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_dv_bin, a, b);
  gtk_text_buffer_remove_tag(tb, s->tag_dv_str, a, b);
}

/* Apply tag to character range [col0,col1) on line in the text buffer. */
static void add_tag_range(AppState *s, GtkTextTag *tag, int line, int col0,
                          int col1) {
  if (col1 <= col0)
    return;
  GtkTextIter a, b;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  gtk_text_buffer_get_iter_at_line_offset(tb, &a, line, col0);
  gtk_text_buffer_get_iter_at_line_offset(tb, &b, line, col1);
  gtk_text_buffer_apply_tag(tb, tag, &a, &b);
}

/* Apply syntax-highlight tags to one source line in the GTK text buffer. */
static void highlight_line(AppState *s, int line, const char *text) {
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  GtkTextIter la, lb;
  gtk_text_buffer_get_iter_at_line(tb, &la, line);
  lb = la;
  gtk_text_iter_forward_to_line_end(&lb);
  remove_all_tags(s, &la, &lb);

  int n = (int)strlen(text);

  // strings
  for (int i = 0; i < n; i++) {
    if (text[i] == '"') {
      int j = i + 1;
      while (j < n) {
        if (text[j] == '\\' && j + 1 < n) {
          j += 2;
          continue;
        }
        if (text[j] == '"') {
          j++;
          break;
        }
        j++;
      }
      add_tag_range(s, s->tag_string, line, i, j);
      i = j - 1;
    }
  }

  // comments: ';' to end (ignore if inside a string)
  for (int i = 0; i < n; i++) {
    if (text[i] == ';') {
      int q = 0;
      for (int k = 0; k < i; k++)
        if (text[k] == '"' && (k == 0 || text[k - 1] != '\\'))
          q++;
      if ((q % 2) == 0) {
        add_tag_range(s, s->tag_comment, line, i, n);
        n = i; // stop further token colouring after comment
      }
      break;
    }
  }

  // leading whitespace
  int i = 0;
  while (i < n && (text[i] == ' ' || text[i] == '\t'))
    i++;
  int bol = i;

  // label / equate at BOL
  // Only treat the first token as a label if it is explicitly a label (ends
  // with ':') or an equate (followed by '='):
  //   FOO:   ...
  //   FOO =  ...
  // This avoids mis-highlighting opcodes (e.g. "pha") and directives
  // (".include") as labels.
  if (i < n) {
    int tok_start = i;

    // If the first non-space char is '.' or '#', it is a directive (or a
    // preprocessor directive). Do not treat it as a label unless it explicitly
    // ends with ':' (local labels like ".loop:").
    int allow_local_label = 0;
    if (text[tok_start] == '.' || text[tok_start] == '#')
      allow_local_label = 1;

    int tok_end = tok_start;
    while (tok_end < n && !g_ascii_isspace((unsigned char)text[tok_end]) &&
           text[tok_end] != '=')
      tok_end++;

    int has_colon = 0;
    int colon_pos = -1;
    for (int k = tok_start; k < tok_end; k++) {
      if (text[k] == ':') {
        has_colon = 1;
        colon_pos = k;
        break;
      }
    }

    // Look for '=' after the token (allow whitespace between)
    int p = tok_end;
    while (p < n && g_ascii_isspace((unsigned char)text[p]))
      p++;
    /* Single '=' only — not '==' (which is comparison, not assignment) */
    int is_equate =
        (p < n && text[p] == '=' && (p + 1 >= n || text[p + 1] != '='));

    // Copy first token for directive/label checks.
    char ftok[128];
    int ftlen = tok_end - tok_start;
    if (ftlen > (int)sizeof(ftok) - 1)
      ftlen = (int)sizeof(ftok) - 1;
    memcpy(ftok, text + tok_start, (size_t)ftlen);
    ftok[ftlen] = 0;

    if (has_colon) {
      // include the ':' itself
      add_tag_range(s, s->tag_label, line, tok_start, colon_pos + 1);
    } else if (is_equate && (!allow_local_label) &&
               gui_is_ident_start((unsigned char)text[tok_start])) {
      // "FOO = ..." equate label
      add_tag_range(s, s->tag_label, line, tok_start, tok_end);
    } else if (!has_colon && !is_equate) {
      // Dot/colon-prefixed labels without a trailing ':' are allowed, but the
      // name portion must be a simple identifier and must NOT be a directive.
      //   .localLabel
      //   :localLabel
      if (ftok[0] == '.' && !is_directive_token(ftok) &&
          !is_data_directive_token(ftok) &&
          gui_is_simple_label_name(ftok + 1)) {
        add_tag_range(s, s->tag_label, line, tok_start, tok_end);
      } else if (ftok[0] == ':' && gui_is_simple_label_name(ftok + 1)) {
        add_tag_range(s, s->tag_label, line, tok_start, tok_end);
      } else if (gui_is_simple_label_name(ftok) &&
                 !is_instruction_token(ftok) && !is_directive_token(ftok) &&
                 !is_data_directive_token(ftok) && !is_register_token(ftok) &&
                 !is_number_token(ftok)) {
        // Before treating as a colonless label, check if it is a macro call.
        gboolean _ftok_is_macro = FALSE;
        if (s->active_doc && s->active_doc->macro_defs) {
          gchar *_flow = g_ascii_strdown(ftok, -1);
          _ftok_is_macro =
              g_hash_table_contains(s->active_doc->macro_defs, _flow);
          g_free(_flow);
        }
        if (_ftok_is_macro) {
          // Macro call at start of line — colour as directive, not label.
          add_tag_range(s, s->tag_dir, line, tok_start, tok_end);
        } else {
          // Conservative colonless label highlighting at start of line:
          //   Label
          //   Label  lda #0
          //   Label  .byte 0
          // Only colour as a label when the remainder of the line is
          // empty/comment or clearly starts with an opcode/directive.
          if (p >= n || text[p] == ';') {
            add_tag_range(s, s->tag_label, line, tok_start, tok_end);
          } else {
            char next[128];
            int q = p;
            int nlen = 0;

            /* Extract just the leading mnemonic/directive word after a
               potential colonless label. This handles both:
                 Label    lda #6
                 Label    .byte 0
               and also incomplete/compact typing such as:
                 Label    lda#6
               by recognising the leading instruction/directive token only. */
            if (q < n && (text[q] == '.' ||
                          gui_is_ident_start((unsigned char)text[q]))) {
              next[nlen++] = text[q++];
              while (q < n && gui_is_label_char((unsigned char)text[q]) &&
                     nlen < (int)sizeof(next) - 1) {
                next[nlen++] = text[q++];
              }
            }
            next[nlen] = 0;

            if (nlen > 0 &&
                (is_instruction_token(next) || is_directive_token(next) ||
                 is_data_directive_token(next))) {
              add_tag_range(s, s->tag_label, line, tok_start, tok_end);
            }
          }
        } // end !_ftok_is_macro
      }
    } else if (allow_local_label && has_colon) {
      // local label, already handled above
    }
  }

  // tokens — track context for sub-colour selection
  // ctx: 0=unknown, 1=after-instruction (addr/imm), 2=after-data-directive (dv)
  int ctx = 0;
  int line_has_data_dir = 0;

  for (i = bol; i < n;) {
    while (i < n && isspace((unsigned char)text[i]))
      i++;
    if (i >= n)
      break;

    int start = i;
    /* Tokens can include '(' ')'  '[' ']' as part of addressing mode syntax,
       e.g. ($90,x) or ($90),y — consume them as part of the token so that
       "($90)" is a single token we can classify. */
    while (i < n && !isspace((unsigned char)text[i]) && text[i] != ',' &&
           text[i] != '=')
      i++;
    /* skip over '=' or '==' separator without making it a token */
    int end = i;
    if (i < n && text[i] == '=') {
      /* skip one or two '=' and any following whitespace */
      i++;
      if (i < n && text[i] == '=')
        i++; /* skip second = of == */
      while (i < n && isspace((unsigned char)text[i]))
        i++;
      if (end == start)
        continue;
    }

    char tok[128];
    int tlen = end - start;
    if (tlen > (int)sizeof(tok) - 1)
      tlen = (int)sizeof(tok) - 1;
    memcpy(tok, text + start, (size_t)tlen);
    tok[tlen] = 0;

    /* Strip leading '(' '[' and trailing ')' ']' 'y' 'x' punctuation so that
       ($90,x) and ($90),y expose the inner $90 for classification. */
    const char *num_tok = tok;
    char stripped[128];
    {
      int s2 = 0, e2 = (int)strlen(tok);
      while (s2 < e2 && (tok[s2] == '(' || tok[s2] == '['))
        s2++;
      /* Strip trailing ) ] ,  unconditionally */
      while (e2 > s2 &&
             (tok[e2 - 1] == ')' || tok[e2 - 1] == ']' || tok[e2 - 1] == ','))
        e2--;
      /* Strip trailing register letters only if result stays non-empty,
         e.g. "addr,x" stripped -> "addr"; but bare "x" stays "x" */
      while (e2 > s2 + 1 &&
             (tok[e2 - 1] == 'x' || tok[e2 - 1] == 'X' || tok[e2 - 1] == 'y' ||
              tok[e2 - 1] == 'Y' || tok[e2 - 1] == 's' || tok[e2 - 1] == 'S'))
        e2--;
      int sl = e2 - s2;
      if (sl > 0 && sl < (int)sizeof(stripped)) {
        memcpy(stripped, tok + s2, (size_t)sl);
        stripped[sl] = 0;
        num_tok = stripped;
      }
    }

    const char *class_tok = num_tok;
    if (class_tok[0] == '#')
      class_tok++;

    // Anonymous/local branch labels: :+ / :- / :++ etc.
    if (tok[0] == ':' && (tok[1] == '+' || tok[1] == '-')) {
      char c = tok[1];
      int ok = 1;
      for (int k = 2; tok[k]; k++) {
        if (tok[k] != c) {
          ok = 0;
          break;
        }
      }
      if (ok) {
        add_tag_range(s, s->tag_label, line, start, end);
        if (i < n && text[i] == ',')
          i++;
        continue;
      }
    }

    /* Check macro call before the token classification chain */
    gboolean _tok_is_macro = FALSE;
    if (s->active_doc && s->active_doc->macro_defs) {
      gchar *_mlow = g_ascii_strdown(tok, -1);
      _tok_is_macro = g_hash_table_contains(s->active_doc->macro_defs, _mlow);
      g_free(_mlow);
    }
    if (is_instruction_token(tok)) {
      add_tag_range(s, s->tag_instr, line, start, end);
      ctx = 1;
    } else if (is_data_directive_token(tok)) {
      add_tag_range(s, s->tag_data, line, start, end);
      ctx = 2;
      line_has_data_dir = 1;
    } else if (is_directive_token(tok)) {
      add_tag_range(s, s->tag_dir, line, start, end);
      /* ctx=3: directive operand — colour keyword args as directive colour */
      ctx = 3;
    } else if (_tok_is_macro) {
      /* Macro call site — colour same as directive */
      add_tag_range(s, s->tag_dir, line, start, end);
      ctx = 0;
    } else if (is_register_token(tok) || is_register_token(num_tok)) {
      /* Colour only the register character(s), not any surrounding punctuation.
         num_tok points into stripped[] which begins at the first non-( char;
         calculate its offset from tok so we know where it starts in the line.
       */
      int reg_start = start;
      int reg_end = end;
      if (!is_register_token(tok) && is_register_token(num_tok)) {
        /* Find where num_tok starts within tok: skip leading ( [ */
        int lead = 0;
        while (tok[lead] == '(' || tok[lead] == '[')
          lead++;
        reg_start = start + lead;
        reg_end = reg_start + (int)strlen(num_tok);
        /* Colour any trailing punctuation ) ] with the address/number colour
           so it matches the opening ( that was swept into the preceding token
         */
        if (reg_end < end)
          add_tag_range(s, s->tag_num, line, reg_end, end);
      }
      add_tag_range(s, s->tag_reg, line, reg_start, reg_end);
    } else {
      /* ctx=1: if token is a label reference (branch/jump target), colour it.
         Guard: skip if the raw token OR the stripped token is a register name,
         to avoid colouring "x)" as a label when it appears as "($10,x)".
         We check num_tok (the stripped form) for the leading character so that
         indirect addressing modes like (ZP_STR),Y are also caught — the raw
         tok starts with '(' but the identifier inside is still a label ref. */
      if (ctx == 1 && !is_number_token(num_tok) &&
          !is_number_token(class_tok) && !is_register_token(tok) &&
          !is_register_token(num_tok) && !is_register_token(stripped)) {
        /* Check raw tok first (direct addressing: label, .label, @label) */
        char fc = tok[0];
        char fc2 = num_tok[0]; /* stripped: identifier inside ( ) */
        if (fc == '.' || fc == ':' || fc == '@' || (fc >= 'A' && fc <= 'Z') ||
            (fc >= 'a' && fc <= 'z') || fc == '_') {
          add_tag_range(s, s->tag_label, line, start, end);
          if (i < n && text[i] == ',')
            i++;
          continue;
        }
        /* Indirect addressing: tok starts with '(' or '[' but the stripped
           identifier (num_tok) is a label.  Colour only the identifier span
           so that the surrounding punctuation stays uncoloured. */
        if ((fc == '(' || fc == '[') &&
            (fc2 == '.' || fc2 == ':' || fc2 == '@' ||
             (fc2 >= 'A' && fc2 <= 'Z') || (fc2 >= 'a' && fc2 <= 'z') ||
             fc2 == '_')) {
          /* Find where num_tok starts within the raw text */
          int lead = start;
          while (lead < end && (text[lead] == '(' || text[lead] == '['))
            lead++;
          int ident_end_pos = lead + (int)strlen(num_tok);
          if (ident_end_pos <= end)
            add_tag_range(s, s->tag_label, line, lead, ident_end_pos);
          if (i < n && text[i] == ',')
            i++;
          continue;
        }
      }

      /* ctx=3: directive keyword operand (on/off/cpu names) */
      if (ctx == 3) {
        /* 'on' and 'off' get directive colour; other args get number colour */
        if (streqi(tok, "on") || streqi(tok, "off")) {
          add_tag_range(s, s->tag_dir, line, start, end);
          if (i < n && text[i] == ',')
            i++;
          continue;
        }
        /* cpu type names */
        if (streqi(tok, "6502") || streqi(tok, "65c02") ||
            streqi(tok, "65816") || streqi(tok, "6502u") ||
            streqi(tok, "65c816")) {
          add_tag_range(s, s->tag_num, line, start, end);
          if (i < n && text[i] == ',')
            i++;
          continue;
        }
        /* In .if expressions, also colour any plain numeric token */
        if (is_number_token(tok) || is_number_token(num_tok)) {
          add_tag_range(s, s->tag_num, line, start, end);
          if (i < n && text[i] == ',')
            i++;
          continue;
        }
        /* Colour .or/.and/.not as directive colour */
        if (streqi(tok, ".or") || streqi(tok, "or") || streqi(tok, ".and") ||
            streqi(tok, "and") || streqi(tok, ".not") || streqi(tok, "not")) {
          add_tag_range(s, s->tag_dir, line, start, end);
          if (i < n && text[i] == ',')
            i++;
          continue;
        }
        /* Skip comparison operators == != <= >= and single = in .if context */
        if (tok[0] == '=' || tok[0] == '!' || tok[0] == '<' || tok[0] == '>') {
          if (i < n && text[i] == ',')
            i++;
          continue;
        }
      }
      int nk = is_number_token(num_tok);
      if (!nk)
        nk = is_number_token(class_tok);
      if (nk) {
        int sk = num_subkind(num_tok);
        /* pick the right sub-tag based on context and sub-kind */
        GtkTextTag *ntag = s->tag_num; /* fallback */
        if (ctx == 1) {
          /* instruction operand — addr or imm */
          int is_imm = (num_tok[0] == '#');
          if (is_imm) {
            switch (sk) {
            case 1:
              ntag = s->tag_imm_hex;
              break;
            case 2:
              ntag = s->tag_imm_dec;
              break;
            case 3:
              ntag = s->tag_imm_bin;
              break;
            case 4:
              ntag = s->tag_imm_char;
              break;
            default:
              ntag = s->tag_imm_hex;
              break;
            }
          } else {
            switch (sk) {
            case 1:
              ntag = s->tag_addr_hex;
              break;
            case 2:
              ntag = s->tag_addr_dec;
              break;
            case 3:
              ntag = s->tag_addr_bin;
              break;
            default:
              ntag = s->tag_addr_hex;
              break;
            }
          }
        } else if (ctx == 2) {
          /* data directive — dv */
          switch (sk) {
          case 1:
            ntag = s->tag_dv_hex;
            break;
          case 2:
            ntag = s->tag_dv_dec;
            break;
          case 3:
            ntag = s->tag_dv_bin;
            break;
          default:
            ntag = s->tag_dv_hex;
            break;
          }
        }
        add_tag_range(s, ntag, line, start, end);
      }
    }

    if (i < n && text[i] == ',')
      i++;
  }

  /* Re-tag quoted strings on data-directive lines with tag_dv_str.
     The initial string scan already applied tag_string; we overlay dv_str
     on top by removing tag_string first in the data-value region.
     We only do this when the line contained a data directive. */
  if (line_has_data_dir) {
    /* Find the position of the data directive to only re-tag values after it */
    int dd_end = bol;
    for (int si = bol; si < (int)strlen(text); si++) {
      if (!isspace((unsigned char)text[si])) {
        /* advance past first (possible label) token and then the directive */
        int te = si;
        while (te < (int)strlen(text) && !isspace((unsigned char)text[te]))
          te++;
        /* check if this is the data directive */
        char chk[64];
        int cl = te - si;
        if (cl < (int)sizeof(chk)) {
          memcpy(chk, text + si, (size_t)cl);
          chk[cl] = 0;
          if (is_data_directive_token(chk)) {
            dd_end = te;
            break;
          }
        }
        /* else might be label — skip and look for directive next */
        si = te - 1;
      }
    }
    int fulllen = (int)strlen(text);
    for (int si = dd_end; si < fulllen;) {
      if (text[si] == '"') {
        int sj = si + 1;
        while (sj < fulllen) {
          if (text[sj] == '\\' && sj + 1 < fulllen) {
            sj += 2;
            continue;
          }
          if (text[sj] == '"') {
            sj++;
            break;
          }
          sj++;
        }
        /* Remove tag_string, apply tag_dv_str */
        GtkTextIter ta, tb2;
        GtkTextBuffer *tb3 = GTK_TEXT_BUFFER(s->buf);
        gtk_text_buffer_get_iter_at_line_offset(tb3, &ta, line, si);
        gtk_text_buffer_get_iter_at_line_offset(tb3, &tb2, line, sj);
        gtk_text_buffer_remove_tag(tb3, s->tag_string, &ta, &tb2);
        gtk_text_buffer_apply_tag(tb3, s->tag_dv_str, &ta, &tb2);
        si = sj;
      } else {
        si++;
      }
    }
  }
}

// Apply comment tagging (highest priority) across the whole buffer.
// Supports ';' line comments, '//' line comments, and '/* ... */' block
// comments. Comment markers inside strings are ignored.
/* ── GTK fold application ──────────────────────────────────────────────────
 * Rebuilds the fold map from the current buffer text, then applies:
 *   - tag_fold_hidden (invisible) to body lines of collapsed groups
 *   - tag_fold_marker (italic/grey) to the start line, appending summary text
 * Expanding a fold removes those tags. Called after every buffer change.
 */
/* Rebuild the fold detection map from current buffer text.
 * Called when buffer CONTENT changes. Preserves collapsed state. */
static void fold_map_update(AppState *s) {
  if (!s || !s->buf)
    return;
  if (!s->fold_comments && !s->fold_macros) {
    fold_map_free(&s->fold_map);
    return;
  }
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  int line_count = gtk_text_buffer_get_line_count(tb);
  if (line_count <= 0)
    return;
  const char **line_texts = g_new0(const char *, (gsize)line_count);
  char **line_owned = g_new0(char *, (gsize)line_count);
  for (int i = 0; i < line_count; i++) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line(tb, &a, i);
    b = a;
    gtk_text_iter_forward_to_line_end(&b);
    line_owned[i] = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
    line_texts[i] = line_owned[i] ? line_owned[i] : "";
  }
  fold_map_rebuild(&s->fold_map, line_texts, (size_t)line_count,
                   s->fold_comments != FALSE, s->fold_macros != FALSE);
  for (int i = 0; i < line_count; i++)
    g_free(line_owned[i]);
  g_free(line_texts);
  g_free(line_owned);
}

/* Apply fold tags from the existing map WITHOUT rebuilding it.
 * Called after fold_toggle to update the display. */
static void apply_fold_tags(AppState *s) {
  if (!s || !s->buf)
    return;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  /* Clear all fold tags first, re-assert priorities */
  {
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(tb, &start, &end);
    if (s->tag_fold_hidden)
      gtk_text_buffer_remove_tag(tb, s->tag_fold_hidden, &start, &end);
    if (s->tag_fold_marker)
      gtk_text_buffer_remove_tag(tb, s->tag_fold_marker, &start, &end);
    GtkTextTagTable *tt2 = gtk_text_buffer_get_tag_table(tb);
    int n2 = gtk_text_tag_table_get_size(tt2);
    if (n2 >= 1)
      gtk_text_tag_set_priority(s->tag_fold_hidden, n2 - 1);
    if (n2 >= 2)
      gtk_text_tag_set_priority(s->tag_fold_marker, n2 - 2);
  }
  if (!s->fold_map.lines)
    return;
  /* Apply tags based on current collapsed state */
  for (size_t i = 0; i < s->fold_map.count; i++) {
    fold_line_t *fl = &s->fold_map.lines[i];
    if (fl->kind != FL_FOLD_START)
      continue;
    if (!fl->collapsed)
      continue;
    /* Style the start line */
    {
      GtkTextIter a, b;
      gtk_text_buffer_get_iter_at_line(tb, &a, (int)i);
      b = a;
      gtk_text_iter_forward_to_line_end(&b);
      gtk_text_buffer_apply_tag(tb, s->tag_fold_marker, &a, &b);
    }
    /* Hide body lines */
    size_t body_start = i + 1;
    size_t body_end = fl->group_end;
    if (body_start <= body_end && body_end < s->fold_map.count) {
      GtkTextIter a, b;
      gtk_text_buffer_get_iter_at_line(tb, &a, (int)body_start);
      gtk_text_buffer_get_iter_at_line(tb, &b, (int)body_end);
      gtk_text_iter_forward_to_line_end(&b);
      gtk_text_iter_forward_char(&b);
      gtk_text_buffer_apply_tag(tb, s->tag_fold_hidden, &a, &b);
    }
  }
}

/* apply_folds: rebuild map then apply tags.
 * Called from highlight_idle_cb after buffer content changes. */
static void apply_folds(AppState *s) {
  if (!s || !s->buf)
    return;
  if (!s->fold_comments && !s->fold_macros) {
    GtkTextBuffer *tb0 = GTK_TEXT_BUFFER(s->buf);
    GtkTextIter s0, e0;
    gtk_text_buffer_get_bounds(tb0, &s0, &e0);
    if (s->tag_fold_hidden)
      gtk_text_buffer_remove_tag(tb0, s->tag_fold_hidden, &s0, &e0);
    if (s->tag_fold_marker)
      gtk_text_buffer_remove_tag(tb0, s->tag_fold_marker, &s0, &e0);
    fold_map_free(&s->fold_map);
    return;
  }
  fold_map_update(s);
  apply_fold_tags(s);
}

/* Toggle fold at a given buffer line number. Called from gutter click. */
static void gtk_fold_toggle_line(AppState *s, int line_num) {
  if (!s || !s->buf)
    return;
  if (line_num < 0)
    return;
  /* Build map on demand if not yet done */
  if (!s->fold_map.lines)
    fold_map_update(s);
  if (!s->fold_map.lines)
    return;
  if ((size_t)line_num >= s->fold_map.count)
    return;
  /* Find FL_FOLD_START at or containing line_num */
  size_t idx = (size_t)line_num;
  while (idx > 0 && s->fold_map.lines[idx].kind == FL_FOLD_BODY)
    idx--;
  if (s->fold_map.lines[idx].kind == FL_FOLD_END)
    while (idx > 0 && s->fold_map.lines[idx].kind != FL_FOLD_START)
      idx--;
  if (s->fold_map.lines[idx].kind == FL_FOLD_START &&
      s->fold_map.lines[idx].group_end >= (size_t)line_num)
    fold_toggle(&s->fold_map, idx);
  /* Apply tags only — do NOT rebuild the map (would reset collapsed state) */
  apply_fold_tags(s);
}

static void apply_comment_tags(AppState *s) {
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  GtkTextIter a, b;
  gtk_text_buffer_get_bounds(tb, &a, &b);
  gtk_text_buffer_remove_tag(tb, s->tag_comment, &a, &b);

  char *all = gtk_text_buffer_get_text(tb, &a, &b, FALSE);
  if (!all)
    return;

  /* Use g_utf8_pointer_to_offset to convert byte positions to character
     (Unicode codepoint) offsets, which is what GTK text buffer iters use.
     This fixes tag misalignment when non-ASCII UTF-8 characters are present. */
  glong total_chars = (glong)g_utf8_strlen(all, -1);
  const char *buf_start = all;

  int in_str = 0;
  int in_block = 0;
  const char *block_start_ptr = NULL; /* byte pointer into all[] */

  const char *p = all;
  while (*p) {
    char c = *p;
    char n1 = *(p + 1);

    if (!in_block) {
      if (c == '"' && (p == all || *(p - 1) != '\\'))
        in_str = !in_str;
    }

    if (in_str) {
      p = g_utf8_next_char(p);
      continue;
    }

    if (in_block) {
      if (c == '*' && n1 == '/') {
        /* end of block comment */
        const char *end_ptr = p + 2;
        GtkTextIter ia, ib;
        glong off_a = g_utf8_pointer_to_offset(buf_start, block_start_ptr);
        glong off_b = g_utf8_pointer_to_offset(buf_start, end_ptr);
        gtk_text_buffer_get_iter_at_offset(tb, &ia, (gint)off_a);
        gtk_text_buffer_get_iter_at_offset(tb, &ib, (gint)off_b);
        gtk_text_buffer_apply_tag(tb, s->tag_comment, &ia, &ib);
        in_block = 0;
        block_start_ptr = NULL;
        p += 2;
      } else {
        p = g_utf8_next_char(p);
      }
      continue;
    }

    /* line comment: ; or // */
    if (c == ';' || (c == '/' && n1 == '/')) {
      const char *start_ptr = p;
      /* advance to end of line */
      while (*p && *p != '\n')
        p = g_utf8_next_char(p);
      GtkTextIter ia, ib;
      glong off_a = g_utf8_pointer_to_offset(buf_start, start_ptr);
      glong off_b = g_utf8_pointer_to_offset(buf_start, p);
      gtk_text_buffer_get_iter_at_offset(tb, &ia, (gint)off_a);
      gtk_text_buffer_get_iter_at_offset(tb, &ib, (gint)off_b);
      gtk_text_buffer_apply_tag(tb, s->tag_comment, &ia, &ib);
      continue;
    }

    /* block comment start */
    if (c == '/' && n1 == '*') {
      in_block = 1;
      block_start_ptr = p;
      p += 2;
      continue;
    }

    p = g_utf8_next_char(p);
  }

  /* unterminated block comment: tag to end */
  if (in_block && block_start_ptr) {
    GtkTextIter ia, ib;
    glong off_a = g_utf8_pointer_to_offset(buf_start, block_start_ptr);
    gtk_text_buffer_get_iter_at_offset(tb, &ia, (gint)off_a);
    gtk_text_buffer_get_end_iter(tb, &ib);
    gtk_text_buffer_apply_tag(tb, s->tag_comment, &ia, &ib);
  }

  g_free(all);
}

static void clear_error_tags(AppState *s) {
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  GtkTextIter a, b;
  gtk_text_buffer_get_start_iter(tb, &a);
  gtk_text_buffer_get_end_iter(tb, &b);
  gtk_text_buffer_remove_tag(tb, s->tag_error, &a, &b);
}

// ------------------------ Inactive-branch dimming ------------------------

static inline sv_t sv_make_local(const char *p, size_t len) {
  sv_t v;
  v.p = p;
  v.len = len;
  return v;
}

static inline sv_t sv_null_local(void) { return (sv_t){0}; }

static inline bool sv_is_empty_local(sv_t v) {
  return v.p == NULL || v.len == 0;
}

static bool dim_sv_eq_ci_local(sv_t a, const char *lit) {
  // util.h provides sv_ieq(sv_t, const char*) (case-insensitive)
  return sv_ieq(a, lit);
}

static const char *dim_skip_ws(const char *p, const char *end) {
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
    p++;
  return p;
}

static bool dim_parse_token(const char **io_p, const char *end, sv_t *out) {
  const char *p = dim_skip_ws(*io_p, end);
  const char *s = p;
  while (p < end) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '$') {
      p++;
      continue;
    }
    break;
  }
  if (p == s)
    return false;
  *out = sv_make_local(s, (size_t)(p - s));
  *io_p = p;
  return true;
}

/* Parse one operator token from a conditional-assembly expression. */
static bool dim_parse_op(const char **io_p, const char *end, sv_t *out_op) {
  const char *p = dim_skip_ws(*io_p, end);
  const char *s = p;
  if (p + 1 < end && p[0] == '=' && p[1] == '=') {
    p += 2;
  } else if (p + 1 < end && p[0] == '!' && p[1] == '=') {
    p += 2;
  } else if (p < end && p[0] == '=') {
    /* Treat single '=' as equality for dimming only (avoid
       propagating earlier parser quirks into display logic). */
    p += 1;
  } else {
    return false;
  }
  *out_op = sv_make_local(s, (size_t)(p - s));
  *io_p = p;
  return true;
}

/* Resolve a token against defs, with CPU alias normalisation.
   Returns a NUL-terminated string in a static buffer — use immediately. */
static const char *dim_resolve_token(const defs_t *defs, sv_t tok) {
  static char buf[64];
  const char *v = defs_get(defs, tok);
  if (!v) {
    size_t n = tok.len < sizeof(buf) - 1 ? tok.len : sizeof(buf) - 1;
    memcpy(buf, tok.p, n);
    buf[n] = 0;
    v = buf;
  }
  return v;
}

/* Normalise CPU value strings so both sides of a comparison use the same
   representation regardless of whether .cpu stored "6503" or "65c02" etc. */
static const char *dim_norm_cpu(const char *v) {
  if (strcasecmp(v, "6503") == 0)
    return "65c02";
  if (strcasecmp(v, "6501") == 0)
    return "6502u";
  return v;
}

/* Evaluate a single comparison: lhs op rhs.
   Returns -1 if the expression couldn't be parsed (skip it). */
static int dim_eval_one_cmp(const defs_t *defs, const char **pp,
                            const char *end) {
  const char *p = *pp;
  sv_t lhs = sv_null_local();
  sv_t op = sv_null_local();
  sv_t rhs = sv_null_local();
  if (!dim_parse_token(&p, end, &lhs))
    return -1;
  if (!dim_parse_op(&p, end, &op))
    return -1;
  if (!dim_parse_token(&p, end, &rhs))
    return -1;
  *pp = p;

  const char *lv = dim_resolve_token(defs, lhs);
  const char *rv = dim_resolve_token(defs, rhs);

  /* CPU alias normalisation */
  bool lhs_is_cpu = dim_sv_eq_ci_local(lhs, "cpu");
  bool rhs_is_cpu = dim_sv_eq_ci_local(rhs, "cpu");
  if (lhs_is_cpu || rhs_is_cpu) {
    if (lhs_is_cpu)
      lv = dim_norm_cpu(lv);
    if (rhs_is_cpu)
      rv = dim_norm_cpu(rv);
    lv = dim_norm_cpu(lv);
    rv = dim_norm_cpu(rv);
  }

  bool eq = (strcasecmp(lv, rv) == 0);
  if (op.len == 2 && op.p[0] == '=' && op.p[1] == '=')
    return eq ? 1 : 0;
  if (op.len == 2 && op.p[0] == '!' && op.p[1] == '=')
    return eq ? 0 : 1;
  return -1;
}

/* Parse a keyword token that may start with '.', e.g. ".or", ".and".
   dim_parse_token rejects '.' so we need our own variant for junctions. */
static bool dim_parse_kw_token(const char **io_p, const char *end, sv_t *out) {
  const char *p = dim_skip_ws(*io_p, end);
  if (p >= end)
    return false;
  const char *s = p;
  if (*p == '.')
    p++; /* accept leading dot */
  while (p < end) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
      p++;
      continue;
    }
    break;
  }
  if (p == s)
    return false;
  *out = sv_make_local(s, (size_t)(p - s));
  *io_p = p;
  return true;
}

static bool dim_kw_is_or(sv_t t) {
  const char *w = (t.len > 0 && t.p[0] == '.') ? t.p + 1 : t.p;
  size_t wl = (t.len > 0 && t.p[0] == '.') ? t.len - 1 : t.len;
  return wl == 2 && (w[0] | 32) == 'o' && (w[1] | 32) == 'r';
}
static bool dim_kw_is_and(sv_t t) {
  const char *w = (t.len > 0 && t.p[0] == '.') ? t.p + 1 : t.p;
  size_t wl = (t.len > 0 && t.p[0] == '.') ? t.len - 1 : t.len;
  return wl == 3 && (w[0] | 32) == 'a' && (w[1] | 32) == 'n' &&
         (w[2] | 32) == 'd';
}

/* Check whether the next non-ws content is a boolean junction (|| && .or .and)
   and if so return which kind. Does NOT advance the pointer. */
static bool dim_peek_junction(const char *p, const char *end, bool *out_is_or) {
  p = dim_skip_ws(p, end);
  if (p >= end)
    return false;
  /* symbolic: || or && */
  if (p + 1 < end && p[0] == '|' && p[1] == '|') {
    *out_is_or = true;
    return true;
  }
  if (p + 1 < end && p[0] == '&' && p[1] == '&') {
    *out_is_or = false;
    return true;
  }
  /* keyword: .or / or / .and / and */
  sv_t kw = sv_null_local();
  const char *tmp = p;
  if (dim_parse_kw_token(&tmp, end, &kw)) {
    if (dim_kw_is_or(kw)) {
      *out_is_or = true;
      return true;
    }
    if (dim_kw_is_and(kw)) {
      *out_is_or = false;
      return true;
    }
  }
  return false;
}

/* Consume the junction token, advancing *pp past it. */
static void dim_consume_junction(const char **pp, const char *end) {
  const char *p = dim_skip_ws(*pp, end);
  if (p + 1 < end &&
      ((p[0] == '|' && p[1] == '|') || (p[0] == '&' && p[1] == '&'))) {
    *pp = p + 2;
    return;
  }
  sv_t kw = sv_null_local();
  dim_parse_kw_token(&p, end, &kw); /* consume .or/.and/or/and */
  *pp = p;
}

static bool dim_eval_simple_if(const defs_t *defs, const char *expr,
                               size_t expr_len) {
  const char *p = expr;
  const char *end = expr + expr_len;

  /* Evaluate a chain of comparisons joined by .or/.and/||/&&.
     Left-to-right evaluation; up to 32 terms. */
  int result = dim_eval_one_cmp(defs, &p, end);
  if (result < 0)
    return false;

  for (int guard = 0; guard < 32; guard++) {
    bool is_or = false;
    if (!dim_peek_junction(p, end, &is_or))
      break;
    dim_consume_junction(&p, end);
    int next = dim_eval_one_cmp(defs, &p, end);
    if (next < 0)
      break;
    result = is_or ? (result || next) : (result && next);
  }
  return result != 0;
}

static bool dim_is_directive_line(const char *line, size_t len, sv_t *out_kw,
                                  const char **out_rest) {
  const char *p = line;
  const char *end = line + len;
  p = dim_skip_ws(p, end);
  if (p >= end)
    return false;
  if (*p != '.' && *p != '#')
    return false;
  p++;
  const char *kw_s = p;
  while (p < end) {
    unsigned char c = (unsigned char)*p;
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      p++;
      continue;
    }
    break;
  }
  if (p == kw_s)
    return false;
  *out_kw = sv_make_local(kw_s, (size_t)(p - kw_s));
  *out_rest = p;
  return true;
}

typedef struct {
  bool parent_active;
  bool this_active;
  bool branch_taken;
  bool saw_else;
} CondFrame;

/* Apply the 'inactive' tag to source regions disabled by .if/.else/.endif. */
static void apply_inactive_dimming(AppState *s, GtkTextBuffer *tbuf,
                                   const char *src_text, const defs_t *defs) {
  if (!s || !s->tag_inactive || !tbuf || !src_text || !defs)
    return;

  GtkTextIter a, b;
  gtk_text_buffer_get_start_iter(tbuf, &a);
  gtk_text_buffer_get_end_iter(tbuf, &b);
  gtk_text_buffer_remove_tag(tbuf, s->tag_inactive, &a, &b);

  // `defs` from the semantic/preproc pass is the *final* define-state after the
  // whole file is processed. For dimming we must evaluate conditional blocks
  // using the define-state as we scan line-by-line (so #define/#undef affect
  // subsequent #if/#ifdef/#ifndef correctly).
  //
  // Start from a copy of the final defs (to keep builtins/externals), then
  // apply #define/#undef as we encounter them.
  defs_t cur_defs;
  defs_init(&cur_defs);
  if (defs->tab && defs->cap) {
    for (size_t i = 0; i < defs->cap; ++i) {
      const def_entry_t *e = &defs->tab[i];
      if (e->used && e->key && e->val)
        defs_set(&cur_defs, e->key, e->val);
    }
  }

  /* Determine any CPU-related behaviour from defs directly.
     We avoid depending on core cpu_kind_t here so the GUI stays
     buildable even when core internals change. */

  CondFrame stack[128];
  int sp = 0;
  bool cur_active = true;

  int line_no = 0;
  const char *p = src_text;
  while (*p) {
    const char *ls = p;
    const char *le = ls;
    while (*le && *le != '\n')
      le++;
    size_t len = (size_t)(le - ls);

    sv_t kw = sv_null_local();
    const char *rest = NULL;
    bool is_dir = dim_is_directive_line(ls, len, &kw, &rest);
    if (is_dir) {
      rest = dim_skip_ws(rest, ls + len);

      // Keep a running define-state so #define/#undef (and the assembler-style
      // .def/.undef forms) affect subsequent conditionals.
      if (dim_sv_eq_ci_local(kw, "define") || dim_sv_eq_ci_local(kw, "def")) {
        // #define/.define/.def NAME [VALUE...]
        sv_t name = sv_null_local();
        const char *rr = rest;
        bool have = dim_parse_token(&rr, ls + len, &name);
        if (have) {
          rr = dim_skip_ws(rr, ls + len);
          sv_t val = sv_trim(sv_make_local(rr, (size_t)((ls + len) - rr)));

          // defs_set() duplicates key/value internally, so we can pass
          // temporary NUL-terminated buffers here.
          char *name_c = g_strndup(name.p, name.len);
          if (name_c) {
            if (val.len == 0) {
              defs_set(&cur_defs, name_c, "1");
            } else {
              char *val_c = g_strndup(val.p, val.len);
              if (val_c) {
                defs_set(&cur_defs, name_c, val_c);
                g_free(val_c);
              }
            }
            g_free(name_c);
          }
        }
      } else if (dim_sv_eq_ci_local(kw, "undef")) {
        // #undef/.undef NAME
        sv_t name = sv_null_local();
        const char *rr = rest;
        bool have = dim_parse_token(&rr, ls + len, &name);
        if (have)
          defs_del(&cur_defs, name);
      }
      if (dim_sv_eq_ci_local(kw, "if")) {
        bool cond =
            dim_eval_simple_if(&cur_defs, rest, (size_t)((ls + len) - rest));
        if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
          stack[sp++] = (CondFrame){.parent_active = cur_active,
                                    .this_active = (cur_active && cond),
                                    .branch_taken = (cur_active && cond),
                                    .saw_else = false};
          cur_active = stack[sp - 1].this_active;
        }
      } else if (dim_sv_eq_ci_local(kw, "ifdef") ||
                 dim_sv_eq_ci_local(kw, "ifndef")) {
        sv_t name = sv_null_local();
        const char *rr = rest;
        bool have = dim_parse_token(&rr, ls + len, &name);
        bool defd = have ? (defs_get(&cur_defs, name) != NULL) : false;
        bool cond = dim_sv_eq_ci_local(kw, "ifdef") ? defd : !defd;
        if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
          stack[sp++] = (CondFrame){.parent_active = cur_active,
                                    .this_active = (cur_active && cond),
                                    .branch_taken = (cur_active && cond),
                                    .saw_else = false};
          cur_active = stack[sp - 1].this_active;
        }
      } else if (dim_sv_eq_ci_local(kw, "elseif") ||
                 dim_sv_eq_ci_local(kw, "elif")) {
        if (sp > 0) {
          CondFrame *f = &stack[sp - 1];
          if (!f->saw_else) {
            bool cond = dim_eval_simple_if(&cur_defs, rest,
                                           (size_t)((ls + len) - rest));
            bool take = (f->parent_active && !f->branch_taken && cond);
            f->this_active = take;
            f->branch_taken = f->branch_taken || take;
            cur_active = f->this_active;
          }
        }
      } else if (dim_sv_eq_ci_local(kw, "else")) {
        if (sp > 0) {
          CondFrame *f = &stack[sp - 1];
          bool take = (f->parent_active && !f->branch_taken);
          f->this_active = take;
          f->branch_taken = true;
          f->saw_else = true;
          cur_active = f->this_active;
        }
      } else if (dim_sv_eq_ci_local(kw, "endif")) {
        if (sp > 0) {
          CondFrame f = stack[--sp];
          cur_active = f.parent_active;
        }
      }
    } else {
      if (!cur_active) {
        GtkTextIter ia, ib;
        gtk_text_buffer_get_iter_at_line(tbuf, &ia, line_no);
        gtk_text_buffer_get_iter_at_line(tbuf, &ib, line_no + 1);
        gtk_text_buffer_apply_tag(tbuf, s->tag_inactive, &ia, &ib);
      }
    }

    if (*le == '\n')
      le++;
    p = le;
    line_no++;
  }

  defs_free(&cur_defs);
}

typedef struct {
  int line;
  int col_a;
  int col_b;
} Occ;

/* Apply error underline tags to all lines that have diagnostics. */
static void apply_semantic_errors(AppState *s) {
  if (!s || !s->buf || !s->tag_error)
    return;

  GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(s->buf);

  // Clear any previous error marks.
  GtkTextIter a, b;
  gtk_text_buffer_get_bounds(tbuf, &a, &b);
  gtk_text_buffer_remove_tag(tbuf, s->tag_error, &a, &b);
  gtk_source_buffer_remove_source_marks(GTK_SOURCE_BUFFER(tbuf), &a, &b,
                                        "diag-error");
  gtk_source_buffer_remove_source_marks(GTK_SOURCE_BUFFER(tbuf), &a, &b,
                                        "diag-warning");

  // Get all text from the editor.
  char *text = gtk_text_buffer_get_text(tbuf, &a, &b, FALSE);
  if (!text)
    return;

  // Match CLI/TUI pipeline:
  // 1) strip comments (preserve layout)
  // 2) run preprocessor (.def/#def, $var substitution, includes)
  // 3) run assembler on preprocessed text
  source_t src;
  src.text = text;
  src.len = strlen(text);
  source_strip_comments_preserve_layout(&src);

  diags_t diags;
  diags_init(&diags);

  defs_t defs;
  defs_init(&defs);

  char hex_prefix = '\0';
  char bin_prefix = '\0';

  const char *file = (s->path && s->path[0]) ? s->path : "(buffer)";

  // Derive the directory of the current file to use as the include search
  // path, so that .include "definitions.asm" resolves relative to the file.
  gchar *file_dir = (s->path && s->path[0])
      ? g_path_get_dirname(s->path) : NULL;
  const char *inc_paths[1];
  size_t      inc_count = 0;
  if (file_dir) { inc_paths[0] = file_dir; inc_count = 1; }

  // Expand includes so symbols from included files are visible to the
  // assembler pass. The assembler tracks original file/line via markers
  // emitted by the preprocessor, so diagnostics reference correct lines.
  char *pp = preproc_run_ex(file, src.text, &diags, &defs, &hex_prefix, true,
                            bin_prefix, true, inc_paths, inc_count);

  // Even if preprocessing failed, we can still show preproc diagnostics.
  // Only run the assembler if we got a non-NULL preprocessed buffer.
  if (pp) {
    asm_settings_t st;
    memset(&st, 0, sizeof(st));
    st.cpu = cpu_from_preproc_defs(&defs);
    s->mem_cpu = st.cpu;
    {
      const char *_cv = defs_get((defs_t *)&defs, sv_from_cstr("cpu"));
      st.allow_undoc =
          (_cv && strcmp(_cv, "6502u") == 0) || (s->allow_undoc ? true : false);
    }

    // Disable optional outputs for this background pass.
    st.out_path = NULL;
    st.list_stdout = false;
    st.list_path = NULL;
    st.sym_stdout = false;
    st.sym_path = NULL;
    st.list_hex_prefix = 0;
    st.sym_hex_prefix = 0;
    st.overwrite_warn = false;
    st.overwrite_warn_limit = 0;

    // Scratch output (65536 bytes) so we don't touch the on-disk outputs.
    uint8_t *mem = (uint8_t *)g_malloc0(65536);
    uint8_t *written = (uint8_t *)g_malloc0(65536);

    uint16_t min_written = 0, max_written = 0;
    uint16_t out_start = 0, out_end = 0;

    (void)asm_assemble(file, pp, &st, NULL, NULL,  // start/end range (optional)
                       mem, written,               // scratch output
                       NULL,                       // written_explicit (not needed here)
                       &min_written, &max_written, // memory span written
                       &out_start, &out_end,       // file output span (unused)
                       &diags);

    g_free(mem);
    g_free(written);
  }

  // Map diagnostics back to the editor buffer.
  const int line_count = gtk_text_buffer_get_line_count(tbuf);
  const int cursor_line = gtk_get_cursor_line(s);
  for (size_t i = 0; i < diags.count; i++) {
    const diag_t *d = &diags.items[i];

    // Show both errors and warnings inline.
    if (d->sev != DIAG_ERROR && d->sev != DIAG_WARNING)
      continue;

    const bool is_warn = (d->sev == DIAG_WARNING);
    if (!diag_file_matches_current(d->file, file))
      continue;
    if (d->line == 0)
      continue;

    int line0 = (int)d->line - 1;
    if (line0 < 0 || line0 >= line_count)
      continue;

    /* Don't highlight errors on the line currently being edited —
       wait until the cursor moves off it. */
    if (line0 == cursor_line)
      continue;

    int col0 = (d->col > 0) ? ((int)d->col - 1) : 0;
    int len = (int)d->len;

    GtkTextIter ia, ib;
    gtk_text_buffer_get_iter_at_line_offset(tbuf, &ia, line0, col0);

    ib = ia;
    if (len <= 0) {
      /*
        Some diagnostics don't provide a token length (len==0). Highlighting
        to EOL can look like a big "bar" (especially when the region includes
        lots of trailing space). Instead, highlight a best-effort token span:
        advance until whitespace or a likely delimiter.
      */
      GtkTextIter t = ia;
      while (!gtk_text_iter_ends_line(&t)) {
        gunichar ch = gtk_text_iter_get_char(&t);

        if (g_unichar_isspace(ch))
          break;

        /* common token delimiters in asm / expressions */
        if (ch == ';' || ch == ',' || ch == '(' || ch == ')' || ch == '[' ||
            ch == ']' || ch == '{' || ch == '}' || ch == ':' || ch == '"' ||
            ch == '\'' || ch == '+' || ch == '-' || ch == '*' || ch == '/' ||
            ch == '<' || ch == '>' || ch == '=' || ch == '!' || ch == '&' ||
            ch == '|' || ch == '^')
          break;

        gtk_text_iter_forward_char(&t);
      }

      /* Ensure we highlight at least one character if we're sitting on a
         non-whitespace glyph. */
      if (gtk_text_iter_equal(&t, &ia) && !gtk_text_iter_ends_line(&t))
        gtk_text_iter_forward_char(&t);

      ib = t;
    } else {
      gtk_text_iter_forward_chars(&ib, len);
    }

    // Reuse the existing error tag for underline/marking.
    // (If you later introduce a dedicated warning tag, swap it here.)
    gtk_text_buffer_apply_tag(tbuf, s->tag_error, &ia, &ib);
    // Add a marker in the left gutter for this line
    {
      GtkTextIter il;
      if (d->line > 0)
        gtk_text_buffer_get_iter_at_line(tbuf, &il, d->line - 1);
      else
        gtk_text_buffer_get_start_iter(tbuf, &il);
      gtk_source_buffer_create_source_mark(
          GTK_SOURCE_BUFFER(tbuf), NULL,
          is_warn ? "diag-warning" : "diag-error", &il);
    }
  }

  /* Feed Problems panel — but omit diagnostics on the cursor line so that
     incomplete lines don't appear as errors while the user is still typing.
     They will reappear on the next highlight pass once the cursor moves. */
  diags_free(&s->sem_diags);
  diags_init(&s->sem_diags);
  for (size_t _i = 0; _i < diags.count; _i++) {
    const diag_t *_d = &diags.items[_i];
    if (_d->line > 0 && (int)_d->line - 1 == cursor_line)
      continue;
    diag_addf(&s->sem_diags, _d->sev, _d->file ? _d->file : "(buffer)",
              _d->line, _d->col, _d->len, "%s", _d->msg ? _d->msg : "");
  }
  schedule_problems_refresh(s, FALSE);

  // Dim inactive preprocessor branches (based on
  // .if/.else/.endif/.ifdef/.ifndef)
  apply_inactive_dimming(s, tbuf, src.text, &defs);

  if (pp)
    free(pp);
  defs_free(&defs);
  diags_free(&diags);
  g_free(file_dir);
  g_free(text);
}

/* ---------------- Actions + menus ---------------- */

static void action_new(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->notebook || !s->docs)
    return;

  EditorDoc *d = app_doc_new(s, NULL);
  g_ptr_array_add(s->docs, d);
  app_add_doc_tab(s, d, "(untitled)");
  app_activate_doc(s, d);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Open Assembly File");
  source_dialog_initial_folder(s, dlg);
  /* Filters: default to *.asm, but allow *.* */
  {
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *f_asm = gtk_file_filter_new();
    gtk_file_filter_set_name(f_asm, "Assembly (*.asm)");
    gtk_file_filter_add_pattern(f_asm, "*.asm");
    g_list_store_append(filters, f_asm);
    GtkFileFilter *f_all = gtk_file_filter_new();
    gtk_file_filter_set_name(f_all, "All files (*.*)");
    gtk_file_filter_add_pattern(f_all, "*");
    g_list_store_append(filters, f_all);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dlg, f_asm);
    g_object_unref(filters);
    g_object_unref(f_asm);
    g_object_unref(f_all);
  }
  gtk_file_dialog_open(dlg, s->win, NULL, open_finish_cb, s);
}

static void action_save_as(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Save As");
  source_dialog_initial_folder(s, dlg);
  if (s->path && *s->path) {
    char *name = g_path_get_basename(s->path);
    gtk_file_dialog_set_initial_name(dlg, name);
    g_free(name);
  }
  /* Filters: default to *.asm, but allow *.* */
  {
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *f_asm = gtk_file_filter_new();
    gtk_file_filter_set_name(f_asm, "Assembly (*.asm)");
    gtk_file_filter_add_pattern(f_asm, "*.asm");
    g_list_store_append(filters, f_asm);
    GtkFileFilter *f_all = gtk_file_filter_new();
    gtk_file_filter_set_name(f_all, "All files (*.*)");
    gtk_file_filter_add_pattern(f_all, "*");
    g_list_store_append(filters, f_all);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_set_default_filter(dlg, f_asm);
    g_object_unref(filters);
    g_object_unref(f_asm);
    g_object_unref(f_all);
  }
  gtk_file_dialog_save(dlg, s->win, NULL, save_finish_cb, s);
}

static void action_save(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s->path) {
    action_save_as(NULL, NULL, s);
    return;
  }

  GError *err = NULL;
  if (!save_buffer_to_file(s, s->path, &err)) {
    if (err) {
      g_warning("save failed: %s", err->message);
      g_clear_error(&err);
    }
    set_status(s, "Save failed.");
  }
}

/* Action handler: check for unsaved changes then quit the application. */
/* Context for the quit-with-unsaved-changes dialog. */
typedef struct {
  AppState *s;
  guint     doc_idx;   /* which doc triggered the prompt */
} QuitCheckCtx;

static void quit_check_next(AppState *s, guint start_idx); /* forward decl */

static void quit_discard_response(GtkDialog *dlg, int resp, gpointer ud) {
  QuitCheckCtx *ctx = (QuitCheckCtx *)ud;
  AppState *s = ctx->s;
  guint idx  = ctx->doc_idx;
  g_free(ctx);
  gtk_window_destroy(GTK_WINDOW(dlg));

  if (resp == GTK_RESPONSE_CANCEL) return; /* abort quit entirely */

  if (resp == GTK_RESPONSE_YES && s->docs && idx < s->docs->len) {
    EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, idx);
    if (d && d->buf && d->path && d->path[0]) {
      GtkTextIter st, en;
      gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(d->buf), &st, &en);
      char *text = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(d->buf),
                                             &st, &en, FALSE);
      GError *err = NULL;
      g_file_set_contents(d->path, text, -1, &err);
      g_free(text);
      if (err) { g_error_free(err); return; } /* save failed — abort */
      gtk_text_buffer_set_modified(GTK_TEXT_BUFFER(d->buf), FALSE);
    }
  }
  /* Discard or saved — continue checking from next tab */
  quit_check_next(s, idx + 1);
}

static void quit_do_quit(AppState *s) {
  gtk_prefs_save(s);
  session_save(s);
  recent_files_save(s);
  if (s->notebook && s->docs) {
    int pg = gtk_notebook_get_current_page(s->notebook);
    if (pg >= 0 && (guint)pg < s->docs->len) {
      EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, (guint)pg);
      if (d && d->path)
        gtk_bookmarks_save(s, d->path);
    }
  }
  g_application_quit(G_APPLICATION(s->app));
}

static void quit_check_next(AppState *s, guint start_idx) {
  if (!s->docs) { quit_do_quit(s); return; }
  /* Find next dirty tab from start_idx onwards */
  for (guint i = start_idx; i < s->docs->len; i++) {
    EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, i);
    if (!d || !d->buf) continue;
    if (!gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(d->buf))) continue;
    /* Prompt for this dirty tab */
    const char *label = (d->path && d->path[0]) ? d->path : "(untitled)";
    char msg[512];
    snprintf(msg, sizeof(msg), "Save changes to %s before quitting?", label);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(s->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "%s", msg);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel",  GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Discard", GTK_RESPONSE_NO);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Save",    GTK_RESPONSE_YES);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_YES);
    QuitCheckCtx *ctx = g_new0(QuitCheckCtx, 1);
    ctx->s = s;
    ctx->doc_idx = i;
    g_signal_connect(dlg, "response",
                     G_CALLBACK(quit_discard_response), ctx);
    gtk_window_present(GTK_WINDOW(dlg));
    return; /* wait for dialog response */
  }
  /* All tabs clean — proceed with quit */
  quit_do_quit(s);
}

static void action_quit(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a; (void)p;
  AppState *s = (AppState *)user_data;
  quit_check_next(s, 0);
}

/* Key handler for the About dialog: close on Escape. */
static gboolean about_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                  guint keycode, GdkModifierType state,
                                  gpointer user_data) {
  (void)ctl;
  (void)keycode;
  (void)state;
  GtkWindow *w = GTK_WINDOW(user_data);
  if (keyval == GDK_KEY_Escape) {
    gtk_window_destroy(w);
    return TRUE;
  }
  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    gtk_window_destroy(w);
    return TRUE;
  }
  return FALSE;
}

// ------------------------------------------------------------
// "Config Files..." dialog
// Two outer tabs: Prefs / Colours
// Three inner tabs each: Project / Executable / User
// Each panel: description, resolved path, exists indicator,
//             [Open Folder]  [Create] buttons.
// ------------------------------------------------------------

typedef struct {
  AppState *s;
  GtkWidget *dlg;
  // Per-panel widgets (6 panels: pref×3, col×3)
  // We refresh them all when the dialog opens.
  GtkWidget *path_labels[6];   // full path text
  GtkWidget *status_labels[6]; // "✓ Found" / "✗ Not found"
  GtkWidget *create_btns[6];   // Create button (enabled only when missing)
  GtkWidget *edit_btns[6];     // Edit button (enabled only when file exists)
  GtkWidget *delete_btns[6];   // Delete button (enabled only when file exists)
  GtkWidget *apply_btns[6];    // Apply button (enabled only when file exists)
  GtkWidget *copy_btns[6];     // Copy button (enabled only when file exists)
  char paths[6][PATH_MAX];
  const char *filenames[6]; // basename searched for
} CfgDlgState;

static const char *cfg_descriptions[6] = {
    /* prefs - project */
    "Project-level preferences.\n"
    "Settings here override the executable and user defaults\n"
    "for this project only.",
    /* prefs - exe */
    "Installation-wide preference defaults.\n"
    "Applies to all projects unless overridden by a project-level file.",
    /* prefs - user */
    "Your personal preference defaults.\n"
    "Used when no project-level or executable-level file is found.\n"
    "This is where 6502caEd saves your preferences.",
    /* colours - project */
    "Project-specific colour overrides.\n"
    "Place a colour config here to customise syntax colours\n"
    "for this project only.",
    /* colours - exe */
    "Installation-wide colour defaults.\n"
    "Applies to all projects unless a project-level file is present.",
    /* colours - user */
    "Your personal colour defaults.\n"
    "Used when no project-level or executable-level colour file is found.",
};

static void cfg_dlg_resolve_paths(CfgDlgState *ds) {
  AppState *s = ds->s;

  // Project dir
  char proj_dir[PATH_MAX] = {0};
  if (s->path && s->path[0]) {
    gchar *d = g_path_get_dirname(s->path);
    g_strlcpy(proj_dir, d, sizeof(proj_dir));
    g_free(d);
  }

  // Exe dir
  char exe_dir[PATH_MAX] = {0};
  _gui_exe_dir(exe_dir, sizeof(exe_dir));

  // User config dir
  char user_dir[PATH_MAX] = {0};
  {
    const char *home = g_get_home_dir();
    g_snprintf(user_dir, sizeof(user_dir), "%s/.config/6502caEd",
               home ? home : ".");
  }

  const char *dirs[3] = {proj_dir, exe_dir, user_dir};

  // Panels 0-2: prefs.ini
  for (int i = 0; i < 3; i++) {
    ds->filenames[i] = "prefs.ini";
    if (dirs[i][0])
      g_snprintf(ds->paths[i], PATH_MAX, "%s/prefs.ini", dirs[i]);
    else
      ds->paths[i][0] = 0;
  }

  // Panels 3-5: colour config (.colours / .colors — show .colours as canonical)
  for (int i = 0; i < 3; i++) {
    ds->filenames[3 + i] = "6502caEd.colours";
    if (dirs[i][0])
      g_snprintf(ds->paths[3 + i], PATH_MAX, "%s/6502caEd.colours", dirs[i]);
    else
      ds->paths[3 + i][0] = 0;
  }
}

static void cfg_dlg_refresh(CfgDlgState *ds) {
  cfg_dlg_resolve_paths(ds);
  for (int i = 0; i < 6; i++) {
    if (!ds->path_labels[i])
      continue;
    if (ds->paths[i][0]) {
      gtk_label_set_text(GTK_LABEL(ds->path_labels[i]), ds->paths[i]);
      gboolean exists = g_file_test(ds->paths[i], G_FILE_TEST_EXISTS);
      if (exists)
        gtk_label_set_markup(
            GTK_LABEL(ds->status_labels[i]),
            "<b><span foreground=\"#2ea043\">✓  File found</span></b>");
      else
        gtk_label_set_text(GTK_LABEL(ds->status_labels[i]),
                           "✗  File not found");
      gtk_widget_set_sensitive(ds->create_btns[i], !exists);
      if (ds->edit_btns[i])
        gtk_widget_set_sensitive(ds->edit_btns[i], exists);
      if (ds->delete_btns[i])
        gtk_widget_set_sensitive(ds->delete_btns[i], exists);
      if (ds->apply_btns[i])
        gtk_widget_set_sensitive(ds->apply_btns[i], exists);
      if (ds->copy_btns[i])
        gtk_widget_set_sensitive(ds->copy_btns[i], exists);
    } else {
      gtk_label_set_text(GTK_LABEL(ds->path_labels[i]),
                         "(no file open — save your file first)");
      gtk_label_set_text(GTK_LABEL(ds->status_labels[i]), "");
      gtk_widget_set_sensitive(ds->create_btns[i], FALSE);
      if (ds->edit_btns[i])
        gtk_widget_set_sensitive(ds->edit_btns[i], FALSE);
      if (ds->delete_btns[i])
        gtk_widget_set_sensitive(ds->delete_btns[i], FALSE);
      if (ds->apply_btns[i])
        gtk_widget_set_sensitive(ds->apply_btns[i], FALSE);
      if (ds->copy_btns[i])
        gtk_widget_set_sensitive(ds->copy_btns[i], FALSE);
    }
  }
}

/* Callback: open the folder containing a config file in the file manager. */
static void cfg_open_folder_cb(GtkButton *btn, gpointer user_data) {
  (void)btn;
  int idx = GPOINTER_TO_INT(user_data) & 0xFF;
  CfgDlgState *ds = (CfgDlgState *)(gpointer)((guintptr)user_data >> 8);
  if (!ds->paths[idx][0])
    return;
  gchar *dir = g_path_get_dirname(ds->paths[idx]);
  gchar *uri = g_filename_to_uri(dir, NULL, NULL);
  if (uri) {
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
      // fallback: xdg-open
      gchar *cmd = g_strdup_printf("xdg-open \"%s\"", dir);
      g_spawn_command_line_async(cmd, NULL);
      g_free(cmd);
      if (err)
        g_error_free(err);
    }
    g_free(uri);
  }
  g_free(dir);
}

static void cfg_create_cb(GtkButton *btn, gpointer user_data) {
  (void)btn;
  int idx = GPOINTER_TO_INT(user_data) & 0xFF;
  CfgDlgState *ds = (CfgDlgState *)(gpointer)((guintptr)user_data >> 8);
  if (!ds->paths[idx][0])
    return;

  // Ensure parent dir exists
  gchar *dir = g_path_get_dirname(ds->paths[idx]);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);

  /* Write current settings as active (uncommented) values so the file
     works immediately as a useful override rather than a blank template. */
  FILE *f = fopen(ds->paths[idx], "w");
  if (f) {
    AppState *s = ds->s;
    if (idx < 3) {
      /* prefs.ini — write current live values */
      fprintf(f, "; 6502caEd preferences\n");
      fprintf(f, "; Created from current settings. Edit as needed.\n");
      fprintf(f, "dark_mode=%d\n", s->dark_mode ? 1 : 0);
      fprintf(f, "auto_indent=%d\n", s->auto_indent ? 1 : 0);
      fprintf(f, "word_wrap=%d\n", s->word_wrap ? 1 : 0);
      fprintf(f, "reopen_last_file=%d\n", s->reopen_last_file ? 1 : 0);
      fprintf(f, "tab_width=%d\n", s->tab_width);
      fprintf(f, "problems_enabled=%d\n", s->problems_enabled ? 1 : 0);
      fprintf(f, "error_underline=%d\n", s->error_underline ? 1 : 0);
      fprintf(f, "asm_clear_mem=%d\n", s->asm_clear_mem ? 1 : 0);
      fprintf(f, "allow_undoc=%d\n", s->allow_undoc ? 1 : 0);
      fprintf(f, "autocomplete=%d\n", s->autocomplete ? 1 : 0);
      fprintf(f, "keep_comment_aligned=%d\n", s->keep_comment_aligned ? 1 : 0);
      fprintf(f, "comment_align_col=%d\n", s->comment_align_col);
      fprintf(f, "comment_use_slashslash=%d\n",
              s->comment_use_slashslash ? 1 : 0);
      fprintf(f, "src_ext=%s\n", s->src_ext[0] ? s->src_ext : ".asm");
      fprintf(f, "bin_ext=%s\n", s->bin_ext[0] ? s->bin_ext : ".bin");
      fprintf(f, "font_family=%s\n",
              s->font_family[0] ? s->font_family : "Monospace");
      fprintf(f, "font_size=%d\n", s->font_size > 0 ? s->font_size : 12);
    } else {
      /* colour config — write current values under the active mode section.
       * If the file already exists, preserve any section for the other mode
       * so that editing colours in dark mode does not lose light mode values
       * and vice versa. */

      /* Read the other-mode section from the existing file (if any). */
      const char *this_section = s->dark_mode ? "[dark]" : "[light]";
      const char *other_section = s->dark_mode ? "[light]" : "[dark]";
      GString *other_block = g_string_new(NULL);
      {
        gchar *old_contents = NULL;
        gsize old_len = 0;
        if (g_file_get_contents(ds->paths[idx], &old_contents, &old_len,
                                NULL)) {
          gchar **flines = g_strsplit(old_contents, "\n", -1);
          gboolean in_other = FALSE;
          for (gchar **lp = flines; lp && *lp; lp++) {
            gchar *fl = g_strstrip(*lp);
            if (fl[0] == '[') {
              gchar *cb = strchr(fl, ']');
              if (cb) {
                gchar saved = *(cb + 1);
                *(cb + 1) = 0;
                in_other = (g_ascii_strcasecmp(fl, other_section) == 0);
                *(cb + 1) = saved;
              }
            }
            if (in_other)
              g_string_append_printf(other_block, "%s\n", *lp);
          }
          g_strfreev(flines);
          g_free(old_contents);
        }
      }

      fprintf(f, "; 6502caEd colour configuration\n");
      fprintf(f, "; Edit as needed. Use [dark] and [light] sections for\n");
      fprintf(f, "; per-mode colours, or put values before any section\n");
      fprintf(f, "; header to apply them in both modes.\n");
      fprintf(f, "%s\n", this_section);
      fprintf(f, "instr         = %s\n", s->colors.instr);
      fprintf(f, "dir           = %s\n", s->colors.dir);
      fprintf(f, "data          = %s\n", s->colors.data);
      fprintf(f, "num           = %s\n", s->colors.num);
      fprintf(f, "label         = %s\n", s->colors.label);
      fprintf(f, "comment       = %s\n", s->colors.comment);
      fprintf(f, "str           = %s\n", s->colors.str);
      fprintf(f, "reg           = %s\n", s->colors.reg);
      fprintf(f, "hover_bg      = %s\n", s->colors.hover_bg);
      fprintf(f, "inactive      = %s\n", s->colors.inactive);
      fprintf(f, "err_underline = %s\n", s->colors.err_underline);
      fprintf(f, "addr_hex      = %s\n", s->colors.addr_hex);
      fprintf(f, "addr_dec      = %s\n", s->colors.addr_dec);
      fprintf(f, "addr_bin      = %s\n", s->colors.addr_bin);
      fprintf(f, "imm_hex       = %s\n", s->colors.imm_hex);
      fprintf(f, "imm_dec       = %s\n", s->colors.imm_dec);
      fprintf(f, "imm_bin       = %s\n", s->colors.imm_bin);
      fprintf(f, "imm_char      = %s\n", s->colors.imm_char);
      fprintf(f, "dv_hex        = %s\n", s->colors.dv_hex);
      fprintf(f, "dv_dec        = %s\n", s->colors.dv_dec);
      fprintf(f, "dv_bin        = %s\n", s->colors.dv_bin);
      fprintf(f, "dv_str        = %s\n", s->colors.dv_str);

      /* Append the preserved other-mode section (may be empty). */
      if (other_block->len > 0) {
        fprintf(f, "\n");
        fputs(other_block->str, f);
      }
      g_string_free(other_block, TRUE);
    }
    fclose(f);
  }
  cfg_dlg_refresh(ds);
}

/* Forward declarations — editors for prefs and colour config files. */
static void open_prefs_editor(AppState *s, const char *path,
                              GtkWindow *parent); /* fwd */
static void open_colours_editor(AppState *s, const char *path,
                                GtkWindow *parent); /* fwd */

static void cfg_apply_btn_cb(GtkButton *btn, gpointer user_data) {
  (void)btn;
  CfgDlgState *ds = (CfgDlgState *)(gpointer)((guintptr)user_data >> 8);
  int idx = (int)((guintptr)user_data & 0xFF);
  if (!ds || !ds->paths[idx][0])
    return;
  const char *path = ds->paths[idx];
  if (!g_file_test(path, G_FILE_TEST_EXISTS))
    return;
  if (idx < 3) {
    /* Prefs: reset to defaults first so removed keys revert cleanly,
       then apply just this file on top */
    gtk_prefs_defaults(ds->s);
    gtk_prefs_load_from_file(ds->s, path);
    gtk_prefs_defaults(ds->s);
    /* Sync dark mode action state so menu checkmark updates */
    {
      GAction *ad =
          g_action_map_lookup_action(G_ACTION_MAP(ds->s->app), "dark");
      if (ad)
        g_simple_action_set_state(G_SIMPLE_ACTION(ad),
                                  g_variant_new_boolean(ds->s->dark_mode));
    }
    apply_theme(ds->s);
    apply_editor_prefs(ds->s);
    apply_font_css(ds->s);
    schedule_highlight(ds->s);
    set_status(ds->s, "Preferences applied.");
  } else {
    /* Colours: reset to defaults then load just this file */
    colors_set_defaults(ds->s);
    gchar *dir = g_path_get_dirname(path);
    colors_load_from_dir(ds->s, dir);
    g_free(dir);
    schedule_highlight(ds->s);
    set_status(ds->s, "Colours applied.");
  }
}

static void cfg_delete_confirm_cb(GtkDialog *dlg, int resp,
                                  gpointer user_data) {
  gpointer *pack = (gpointer *)user_data;
  CfgDlgState *ds = (CfgDlgState *)pack[0];
  int idx = (int)(intptr_t)pack[1];
  g_free(pack);
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (resp != GTK_RESPONSE_YES)
    return;
  if (remove(ds->paths[idx]) != 0) {
    set_status(ds->s, "Delete failed.");
    return;
  }
  /* Reload so live settings revert to lower-priority file/defaults */
  colors_set_defaults(ds->s);
  colors_load_from_exe_dir(ds->s);
  gtk_prefs_defaults(ds->s);
  gtk_prefs_load(ds->s);
  gtk_prefs_defaults(ds->s);
  apply_editor_prefs(ds->s);
  apply_font_css(ds->s);
  schedule_highlight(ds->s);
  cfg_dlg_refresh(ds);
  set_status(ds->s, "File deleted.");
}

static void cfg_delete_btn_cb(GtkButton *btn, gpointer user_data) {
  (void)btn;
  CfgDlgState *ds = (CfgDlgState *)(gpointer)((guintptr)user_data >> 8);
  int idx = (int)((guintptr)user_data & 0xFF);
  if (!ds || !ds->paths[idx][0])
    return;
  /* Confirmation dialog */
  GtkWidget *q = gtk_message_dialog_new(GTK_WINDOW(ds->dlg), GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                        "Delete this config file?");
  gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(q), "%s",
                                           ds->paths[idx]);
  gtk_dialog_add_button(GTK_DIALOG(q), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(q), "Delete", GTK_RESPONSE_YES);
  gtk_dialog_set_default_response(GTK_DIALOG(q), GTK_RESPONSE_CANCEL);
  gpointer *pack = g_new(gpointer, 2);
  pack[0] = ds;
  pack[1] = (gpointer)(intptr_t)idx;
  g_signal_connect(q, "response", G_CALLBACK(cfg_delete_confirm_cb), pack);
  gtk_window_present(GTK_WINDOW(q));
}

/* ── Copy infrastructure ────────────────────────────────────────────────────
 */

static const char *cfg_profile_names[6] = {
    "Project prefs",   "Executable prefs",   "User prefs",
    "Project colours", "Executable colours", "User colours"};

typedef struct {
  CfgDlgState *ds;
  char src[PATH_MAX];
  char dst[PATH_MAX];
} CfgCopyCtx;

/* Step 3: actually perform the file copy */
static void cfg_copy_do(CfgDlgState *ds, const char *src, const char *dst) {
  gchar *contents = NULL;
  gsize len = 0;
  GError *err = NULL;
  if (!g_file_get_contents(src, &contents, &len, &err)) {
    g_warning("Copy read failed %s: %s", src, err ? err->message : "?");
    if (err)
      g_error_free(err);
    return;
  }
  gchar *dir = g_path_get_dirname(dst);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  if (!g_file_set_contents(dst, contents, (gssize)len, &err)) {
    g_warning("Copy write failed %s: %s", dst, err ? err->message : "?");
    if (err)
      g_error_free(err);
  }
  g_free(contents);
  cfg_dlg_refresh(ds);
}

/* Step 2: replace-confirmation response */
static void cfg_copy_replace_cb(GtkDialog *dlg, int resp, gpointer ud) {
  CfgCopyCtx *ctx = (CfgCopyCtx *)ud;
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (resp == GTK_RESPONSE_YES)
    cfg_copy_do(ctx->ds, ctx->src, ctx->dst);
  g_free(ctx);
}

/* Step 2b: destination chosen — check for clash then copy */
static void cfg_copy_to(CfgDlgState *ds, int src_idx, int dst_idx) {
  const char *src = ds->paths[src_idx];
  const char *dst = ds->paths[dst_idx];
  if (!src[0] || !dst[0])
    return;
  if (g_file_test(dst, G_FILE_TEST_EXISTS)) {
    CfgCopyCtx *ctx = g_new0(CfgCopyCtx, 1);
    ctx->ds = ds;
    g_strlcpy(ctx->src, src, PATH_MAX);
    g_strlcpy(ctx->dst, dst, PATH_MAX);
    gchar *msg =
        g_strdup_printf("The %s file already exists:\n\n%s\n\nReplace it?",
                        cfg_profile_names[dst_idx], dst);
    GtkWidget *q = gtk_message_dialog_new(GTK_WINDOW(ds->dlg), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION,
                                          GTK_BUTTONS_NONE, "%s", msg);
    g_free(msg);
    gtk_dialog_add_button(GTK_DIALOG(q), "Cancel", GTK_RESPONSE_CANCEL);
    GtkWidget *rep =
        gtk_dialog_add_button(GTK_DIALOG(q), "Replace", GTK_RESPONSE_YES);
    gtk_widget_add_css_class(rep, "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(q), GTK_RESPONSE_CANCEL);
    gtk_window_set_title(GTK_WINDOW(q), "Confirm Replace");
    g_signal_connect(q, "response", G_CALLBACK(cfg_copy_replace_cb), ctx);
    gtk_window_present(GTK_WINDOW(q));
  } else {
    cfg_copy_do(ds, src, dst);
  }
}

/* Step 1: destination-picker response */
typedef struct {
  CfgDlgState *ds;
  int src_idx;
} CfgCopyPickCtx;

static void cfg_copy_pick_cb(GtkDialog *dlg, int resp, gpointer ud) {
  CfgCopyPickCtx *ctx = (CfgCopyPickCtx *)ud;
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (resp >= 0 && resp < 6 && resp != ctx->src_idx)
    cfg_copy_to(ctx->ds, ctx->src_idx, resp);
  g_free(ctx);
}

/* Step 1: show destination picker */
static void cfg_copy_dest_clicked(GtkButton *btn, gpointer ud) {
  (void)ud;
  GtkWidget *dlg = GTK_WIDGET(g_object_get_data(G_OBJECT(btn), "copy-dlg"));
  CfgCopyPickCtx *ctx =
      (CfgCopyPickCtx *)g_object_get_data(G_OBJECT(btn), "copy-ctx");
  int dst_idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "copy-dst"));
  gtk_window_destroy(GTK_WINDOW(dlg));
  if (ctx) {
    cfg_copy_to(ctx->ds, ctx->src_idx, dst_idx);
    g_free(ctx);
  }
}

static void cfg_copy_btn_cb(GtkButton *btn, gpointer user_data) {
  (void)btn;
  CfgDlgState *ds = (CfgDlgState *)(gpointer)((guintptr)user_data >> 8);
  int src_idx = (int)((guintptr)user_data & 0xFF);
  int base = (src_idx < 3) ? 0 : 3;

  /* Build a plain GtkWindow (not GtkMessageDialog) so apply_window_dark_hint
     can install a GtkHeaderBar and get proper dark-mode title bar chrome. */
  GtkWidget *dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Copy to...");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(ds->dlg));
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(dlg), 360, -1);
  apply_window_dark_hint(dlg, ds->s->dark_mode);
  if (ds->s->app)
    gtk_application_add_window(ds->s->app, GTK_WINDOW(dlg));

  /* Content */
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(vbox, 20);
  gtk_widget_set_margin_bottom(vbox, 20);
  gtk_widget_set_margin_start(vbox, 20);
  gtk_widget_set_margin_end(vbox, 20);
  gtk_window_set_child(GTK_WINDOW(dlg), vbox);

  gchar *msg = g_strdup_printf("Copy \"%s\" to which profile?",
                               cfg_profile_names[src_idx]);
  GtkWidget *lbl = gtk_label_new(msg);
  g_free(msg);
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_box_append(GTK_BOX(vbox), lbl);

  /* One button per destination profile */
  GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(vbox), btn_row);

  CfgCopyPickCtx *ctx = g_new0(CfgCopyPickCtx, 1);
  ctx->ds = ds;
  ctx->src_idx = src_idx;

  /* Cancel button */
  GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
  g_object_set_data(G_OBJECT(cancel_btn), "dlg", dlg);
  g_signal_connect_swapped(cancel_btn, "clicked",
                           G_CALLBACK(gtk_window_destroy), dlg);
  gtk_box_append(GTK_BOX(btn_row), cancel_btn);

  for (int i = base; i < base + 3; i++) {
    if (i == src_idx)
      continue;
    GtkWidget *dest_btn = gtk_button_new_with_label(cfg_profile_names[i]);
    /* Pack idx into button data */
    g_object_set_data(G_OBJECT(dest_btn), "copy-dlg", dlg);
    g_object_set_data(G_OBJECT(dest_btn), "copy-ctx", ctx);
    g_object_set_data(G_OBJECT(dest_btn), "copy-dst", GINT_TO_POINTER(i));
    g_signal_connect(dest_btn, "clicked", G_CALLBACK(cfg_copy_dest_clicked),
                     NULL);
    gtk_box_append(GTK_BOX(btn_row), dest_btn);
  }

  gtk_window_present(GTK_WINDOW(dlg));
}

static void cfg_edit_btn_cb(GtkButton *btn, gpointer user_data) {
  (void)btn;
  CfgDlgState *ds = (CfgDlgState *)(gpointer)((guintptr)user_data >> 8);
  int idx = (int)((guintptr)user_data & 0xFF);
  if (!ds || !ds->s)
    return;
  const char *path = ds->paths[idx];
  if (!path || !path[0]) {
    set_status(ds->s, "Cannot determine file path.");
    return;
  }
  GtkWindow *parent = ds->dlg ? GTK_WINDOW(ds->dlg) : ds->s->win;
  if (idx < 3)
    open_prefs_editor(ds->s, path, parent);
  else
    open_colours_editor(ds->s, path, parent);
}

static GtkWidget *cfg_build_panel(CfgDlgState *ds, int idx) {
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(vbox, 16);
  gtk_widget_set_margin_bottom(vbox, 16);
  gtk_widget_set_margin_start(vbox, 16);
  gtk_widget_set_margin_end(vbox, 16);

  // Description
  GtkWidget *desc = gtk_label_new(cfg_descriptions[idx]);
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0.0f);
  gtk_widget_set_hexpand(desc, TRUE);
  gtk_box_append(GTK_BOX(vbox), desc);

  // Separator
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  // Filename label
  GtkWidget *fn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *fn_hdr = gtk_label_new("Filename:");
  gtk_label_set_xalign(GTK_LABEL(fn_hdr), 0.0f);
  gtk_widget_set_size_request(fn_hdr, 90, -1);
  GtkWidget *fn_val =
      gtk_label_new(idx < 3 ? "prefs.ini" : "6502caEd.colours  (or .colors)");
  gtk_label_set_xalign(GTK_LABEL(fn_val), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(fn_val), TRUE);
  gtk_widget_set_hexpand(fn_val, TRUE);
  gtk_box_append(GTK_BOX(fn_row), fn_hdr);
  gtk_box_append(GTK_BOX(fn_row), fn_val);
  gtk_box_append(GTK_BOX(vbox), fn_row);

  // Path label
  GtkWidget *path_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *path_hdr = gtk_label_new("Full path:");
  gtk_label_set_xalign(GTK_LABEL(path_hdr), 0.0f);
  gtk_widget_set_size_request(path_hdr, 90, -1);
  GtkWidget *path_val = gtk_label_new("...");
  gtk_label_set_xalign(GTK_LABEL(path_val), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(path_val), TRUE);
  gtk_label_set_wrap(GTK_LABEL(path_val), TRUE);
  gtk_widget_set_hexpand(path_val, TRUE);
  gtk_box_append(GTK_BOX(path_row), path_hdr);
  gtk_box_append(GTK_BOX(path_row), path_val);
  gtk_box_append(GTK_BOX(vbox), path_row);
  ds->path_labels[idx] = path_val;

  // Status label
  GtkWidget *status = gtk_label_new("...");
  gtk_label_set_xalign(GTK_LABEL(status), 0.0f);
  gtk_widget_set_hexpand(status, TRUE);
  gtk_box_append(GTK_BOX(vbox), status);
  ds->status_labels[idx] = status;

  // Spacer
  GtkWidget *spacer = gtk_label_new("");
  gtk_widget_set_vexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(vbox), spacer);

  // Button row
  GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_row, GTK_ALIGN_START);

  GtkWidget *open_btn = gtk_button_new_with_label("Open Folder");
  GtkWidget *create_btn = gtk_button_new_with_label("Create");
  ds->create_btns[idx] = create_btn;

  // Pack idx and ds pointer into a single gpointer via bit-packing
  // idx fits in low byte; ds pointer in upper bits (safe on 64-bit with small
  // idx)
  gpointer cb_data = (gpointer)((guintptr)ds << 8 | (guintptr)(idx & 0xFF));
  g_signal_connect(open_btn, "clicked", G_CALLBACK(cfg_open_folder_cb),
                   cb_data);
  g_signal_connect(create_btn, "clicked", G_CALLBACK(cfg_create_cb), cb_data);

  gtk_box_append(GTK_BOX(btn_row), open_btn);
  gtk_box_append(GTK_BOX(btn_row), create_btn);

  /* "Edit..." button — opens the appropriate GUI editor */
  GtkWidget *edit_btn = gtk_button_new_with_label("Edit...");
  gtk_widget_set_tooltip_text(
      edit_btn, idx < 3 ? "Open the preferences editor for this file"
                        : "Open the colours editor for this file");
  g_signal_connect(edit_btn, "clicked", G_CALLBACK(cfg_edit_btn_cb), cb_data);
  gtk_box_append(GTK_BOX(btn_row), edit_btn);
  ds->edit_btns[idx] = edit_btn;

  /* "Apply" button */
  GtkWidget *apply_btn = gtk_button_new_with_label("Apply");
  gtk_widget_set_tooltip_text(
      apply_btn, idx < 3 ? "Apply this preferences file to the current session"
                         : "Apply this colours file to the current session");
  g_signal_connect(apply_btn, "clicked", G_CALLBACK(cfg_apply_btn_cb), cb_data);
  gtk_box_append(GTK_BOX(btn_row), apply_btn);
  ds->apply_btns[idx] = apply_btn;

  /* "Copy to others" button */
  GtkWidget *copy_btn = gtk_button_new_with_label("Copy to others");
  gtk_widget_set_tooltip_text(
      copy_btn,
      idx < 3 ? "Copy this prefs file to the other two profile locations"
              : "Copy this colours file to the other two profile locations");
  g_signal_connect(copy_btn, "clicked", G_CALLBACK(cfg_copy_btn_cb), cb_data);
  gtk_box_append(GTK_BOX(btn_row), copy_btn);
  ds->copy_btns[idx] = copy_btn;

  /* "Delete" button */
  GtkWidget *delete_btn = gtk_button_new_with_label("Delete");
  gtk_widget_set_tooltip_text(delete_btn, "Delete this config file");
  gtk_widget_add_css_class(delete_btn, "destructive-action");
  g_signal_connect(delete_btn, "clicked", G_CALLBACK(cfg_delete_btn_cb),
                   cb_data);
  gtk_box_append(GTK_BOX(btn_row), delete_btn);
  ds->delete_btns[idx] = delete_btn;
  gtk_box_append(GTK_BOX(vbox), btn_row);

  return vbox;
}

static void cfg_dlg_free(gpointer data) { g_free(data); }

static void action_show_in_files(GSimpleAction *a, GVariant *p,
                                 gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->active_doc || !s->active_doc->path || !s->active_doc->path[0]) {
    set_status(s, "No file open in current tab.");
    return;
  }
  /* Open the parent directory in the system file manager via xdg-open.
     Most desktop file managers (Nautilus, Thunar, Dolphin) will select
     the file when given its full path directly. */
  GFile *f = g_file_new_for_path(s->active_doc->path);
  GFile *parent = g_file_get_parent(f);
  g_object_unref(f);
  if (!parent) {
    set_status(s, "Could not determine file location.");
    return;
  }
  char *dir_uri = g_file_get_uri(parent);
  g_object_unref(parent);
  if (dir_uri) {
    GError *err = NULL;
    /* gtk_show_uri opens the URI with the default handler (file manager) */
    gtk_show_uri(s->win, dir_uri, GDK_CURRENT_TIME);
    g_free(dir_uri);
    (void)err;
  }
}

/* ============================================================
   Prefs file editor — launched from Config Files > Prefs tab
   ============================================================ */

typedef struct {
  AppState *s;
  char path[PATH_MAX];
  GtkWindow *win;
  /* Per-row widgets indexed by row number */
  GtkWidget *rows[32]; /* container rows for showing/hiding */
  int n_rows;
  /* We store all widgets needed to reconstruct the file on save */
} PrefsEditorState;

/* Known prefs keys with types and metadata */
typedef enum { PREF_BOOL, PREF_INT, PREF_STRING } PrefType;
typedef struct {
  const char *key;
  PrefType type;
  int imin, imax; /* for PREF_INT */
  const char *desc;
} PrefMeta;

static const PrefMeta PREF_META[] = {
    {"dark_mode", PREF_BOOL, 0, 0, "Dark colour theme"},
    {"auto_indent", PREF_BOOL, 0, 0, "Auto-indent on Enter"},
    {"word_wrap", PREF_BOOL, 0, 0, "Wrap long lines"},
    {"reopen_last_file", PREF_BOOL, 0, 0, "Reopen files on startup"},
    {"problems_enabled", PREF_BOOL, 0, 0, "Show problems panel"},
    {"error_underline", PREF_BOOL, 0, 0, "Underline semantic errors"},
    {"asm_clear_mem", PREF_BOOL, 0, 0, "Clear memory on assemble"},
    {"allow_undoc", PREF_BOOL, 0, 0, "Allow undocumented opcodes"},
    {"autocomplete", PREF_BOOL, 0, 0,
     "Autocomplete (labels, mnemonics, directives)"},
    {"keep_comment_aligned", PREF_BOOL, 0, 0, "Keep comments aligned"},
    {"fold_comments", PREF_BOOL, 0, 0, "Fold > Comments (2+ line blocks)"},
    {"fold_macros", PREF_BOOL, 0, 0, "Fold > Macros (.macro bodies)"},
    {"comment_use_slashslash", PREF_BOOL, 0, 0, "Use // for new comments"},
    {"tab_width", PREF_INT, 1, 16, "Tab width (1-16)"},
    {"comment_align_col", PREF_INT, 0, 200, "Comment alignment column"},
    {"font_family", PREF_STRING, 0, 0, "Editor font family"},
    {"font_size", PREF_INT, 6, 72, "Editor font size (pt)"},
    {"default_font_size", PREF_INT, 6, 72,
     "Default font size for Ctrl+0 reset"},
    {"src_ext", PREF_STRING, 0, 0, "Source file extension"},
    {"bin_ext", PREF_STRING, 0, 0, "Binary file extension"},
};
static const int PREF_META_N = (int)(sizeof(PREF_META) / sizeof(PREF_META[0]));

/* Widgets stored per row (key widget + value widget + status label) */
typedef struct {
  const PrefMeta *meta;
  GtkWidget *val_widget; /* GtkSwitch / GtkSpinButton / GtkEntry */
  GtkWidget *status;     /* label: OK / warning / unknown */
} PrefsRow;

typedef struct {
  AppState *s;
  char path[PATH_MAX];
  PrefsRow rows[16];
  int n;
  /* Unknown keys found in file — shown in a text view */
  GtkWidget *unknown_tv;
  GString *unknown_lines;
} PrefsEdCtx;

static void prefs_editor_ctx_free(gpointer data) {
  PrefsEdCtx *ctx = (PrefsEdCtx *)data;
  if (ctx->unknown_lines)
    g_string_free(ctx->unknown_lines, TRUE);
  g_free(ctx);
}

static void prefs_editor_reset_cb(GtkButton *btn, gpointer ud) {
  (void)btn;
  PrefsEdCtx *ctx = (PrefsEdCtx *)ud;
  /* Reset each widget to the compiled-in default */
  /* Defaults: bools default to these specific values */
  static const struct {
    const char *key;
    int def_i;
    const char *def_s;
  } DEFS[] = {
      {"dark_mode", 1, NULL},
      {"auto_indent", 1, NULL},
      {"word_wrap", 0, NULL},
      {"reopen_last_file", 1, NULL},
      {"problems_enabled", 1, NULL},
      {"error_underline", 1, NULL},
      {"asm_clear_mem", 1, NULL},
      {"allow_undoc", 0, NULL},
      {"keep_comment_aligned", 0, NULL},
      {"comment_use_slashslash", 0, NULL},
      {"tab_width", 4, NULL},
      {"comment_align_col", 32, NULL},
      {"font_family", 0, "Monospace"},
      {"font_size", 12, NULL},
      {"default_font_size", 12, NULL},
      {"src_ext", 0, ".asm"},
      {"bin_ext", 0, ".bin"},
  };
  int ndefs = (int)(sizeof(DEFS) / sizeof(DEFS[0]));
  for (int i = 0; i < ctx->n; i++) {
    const char *key = ctx->rows[i].meta->key;
    GtkWidget *w = ctx->rows[i].val_widget;
    for (int j = 0; j < ndefs; j++) {
      if (strcmp(key, DEFS[j].key))
        continue;
      if (ctx->rows[i].meta->type == PREF_BOOL)
        gtk_switch_set_active(GTK_SWITCH(w), DEFS[j].def_i ? TRUE : FALSE);
      else if (ctx->rows[i].meta->type == PREF_INT)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), DEFS[j].def_i);
      else
        gtk_editable_set_text(GTK_EDITABLE(w),
                              DEFS[j].def_s ? DEFS[j].def_s : "");
      break;
    }
  }
}

/* Callback: save button pressed in prefs text editor; write file to disk. */
static void prefs_editor_save_cb(GtkButton *btn, gpointer ud) {
  (void)btn;
  PrefsEdCtx *ctx = (PrefsEdCtx *)ud;
  /* Ensure directory exists */
  gchar *dir = g_path_get_dirname(ctx->path);
  g_mkdir_with_parents(dir, 0755);
  g_free(dir);
  FILE *f = fopen(ctx->path, "w");
  if (!f) {
    char errmsg[256];
    g_snprintf(errmsg, sizeof(errmsg), "Save failed: %s", g_strerror(errno));
    set_status(ctx->s, errmsg);
    copy_toast_show(ctx->s, errmsg);
    return;
  }
  fprintf(f, "; 6502caEd preferences\n");
  for (int i = 0; i < ctx->n; i++) {
    const PrefMeta *m = ctx->rows[i].meta;
    GtkWidget *w = ctx->rows[i].val_widget;
    if (m->type == PREF_BOOL) {
      gboolean v = gtk_switch_get_active(GTK_SWITCH(w));
      fprintf(f, "%s=%d\n", m->key, v ? 1 : 0);
    } else if (m->type == PREF_INT) {
      int v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w));
      fprintf(f, "%s=%d\n", m->key, v);
    } else {
      const char *v = gtk_editable_get_text(GTK_EDITABLE(w));
      if (v && v[0])
        fprintf(f, "%s=%s\n", m->key, v);
    }
  }
  /* Append any unknown lines preserved from original */
  if (ctx->unknown_lines && ctx->unknown_lines->len > 0)
    fprintf(f, "\n; (unrecognised keys preserved below)\n%s",
            ctx->unknown_lines->str);
  fclose(f);
  /* Reload prefs live and apply fully (including dark mode etc.) */
  AppState *s = ctx->s;
  gtk_prefs_load(s);
  gtk_prefs_defaults(s);
  /* Sync all stateful action states that may have changed */
  {
    struct {
      const char *name;
      gboolean val;
    } actions[] = {
        {"dark", s->dark_mode},
        {"reopen_last_file", s->reopen_last_file},
        {"word_wrap", s->word_wrap},
        {"auto_indent", s->auto_indent},
        {"problems", s->problems_enabled},
        {"error_underline", s->error_underline},
        {"asm_clear_mem", s->asm_clear_mem},
        {"allow_undoc", s->allow_undoc},
    };
    int na = (int)(sizeof(actions) / sizeof(actions[0]));
    for (int i = 0; i < na; i++) {
      GAction *a =
          g_action_map_lookup_action(G_ACTION_MAP(s->app), actions[i].name);
      if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
                                  g_variant_new_boolean(actions[i].val));
    }
  }
  apply_theme(s);
  apply_editor_prefs(s);
  apply_font_css(s);
  schedule_highlight(s);
  copy_toast_show(s, "Preferences saved.");
  set_status(s, "Preferences saved.");
  /* Close the editor window after a successful save */
  GtkWidget *win = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_WINDOW);
  if (win)
    gtk_window_destroy(GTK_WINDOW(win));
}

static void open_prefs_editor(AppState *s, const char *path,
                              GtkWindow *parent) {
  /* Parse the file first */
  gchar *contents = NULL;
  gboolean file_exists = g_file_get_contents(path, &contents, NULL, NULL);

  PrefsEdCtx *ctx = g_new0(PrefsEdCtx, 1);
  ctx->s = s;
  g_strlcpy(ctx->path, path, sizeof(ctx->path));
  ctx->unknown_lines = g_string_new(NULL);

  /* Build a lookup from key -> current AppState value */
  /* We'll pre-fill from AppState so even a new/empty file shows current vals */
  struct {
    const char *key;
    int ival;
    const char *sval;
  } cur[] = {
      {"dark_mode", s->dark_mode, NULL},
      {"auto_indent", s->auto_indent, NULL},
      {"word_wrap", s->word_wrap, NULL},
      {"reopen_last_file", s->reopen_last_file, NULL},
      {"problems_enabled", s->problems_enabled, NULL},
      {"error_underline", s->error_underline, NULL},
      {"asm_clear_mem", s->asm_clear_mem, NULL},
      {"allow_undoc", s->allow_undoc, NULL},
      {"keep_comment_aligned", s->keep_comment_aligned, NULL},
      {"fold_comments", s->fold_comments, NULL},
      {"fold_macros", s->fold_macros, NULL},
      {"comment_use_slashslash", s->comment_use_slashslash, NULL},
      {"tab_width", s->tab_width, NULL},
      {"comment_align_col", s->comment_align_col, NULL},
      {"font_family", 0, s->font_family},
      {"font_size", s->font_size, NULL},
      {"default_font_size",
       s->default_font_size > 0 ? s->default_font_size : s->font_size, NULL},
      {"src_ext", 0, s->src_ext},
      {"bin_ext", 0, s->bin_ext},
  };
  int cur_n = (int)(sizeof(cur) / sizeof(cur[0]));

  /* Parse file to find which keys are explicitly set and catch unknowns */
  gboolean key_in_file[16] = {0};
  if (file_exists && contents) {
    gchar **lines = g_strsplit(contents, "\n", -1);
    for (gchar **pp = lines; pp && *pp; pp++) {
      gchar *line = g_strstrip(*pp);
      if (!line[0] || line[0] == ';')
        continue;
      gchar *eq = strchr(line, '=');
      if (!eq)
        continue;
      *eq = 0;
      gchar *k = g_strstrip(line);
      gchar *v = g_strstrip(eq + 1);
      gboolean known = FALSE;
      for (int i = 0; i < PREF_META_N; i++) {
        if (!strcmp(k, PREF_META[i].key)) {
          known = TRUE;
          key_in_file[i] = TRUE;
          /* Override cur[] with file value */
          for (int j = 0; j < cur_n; j++) {
            if (!strcmp(cur[j].key, k)) {
              if (PREF_META[i].type == PREF_STRING)
                cur[j].sval = g_strdup(v);
              else
                cur[j].ival = (int)g_ascii_strtoll(v, NULL, 10);
              break;
            }
          }
          break;
        }
      }
      if (!known)
        g_string_append_printf(ctx->unknown_lines, "%s=%s\n", k, v);
    }
    g_strfreev(lines);
    g_free(contents);
  }

  /* Build window */
  GtkWidget *win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(win), "Edit Preferences");
  gtk_window_set_transient_for(GTK_WINDOW(win), parent ? parent : s->win);
  gtk_window_set_modal(GTK_WINDOW(win), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(win), 520, 600);
  apply_window_dark_hint(win, s->dark_mode);
  if (s->app)
    gtk_application_add_window(s->app, GTK_WINDOW(win));
  if (s->app)
    gtk_application_add_window(s->app, GTK_WINDOW(win));
  if (s->app)
    gtk_application_add_window(s->app, GTK_WINDOW(win));

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), outer);

  /* Scrollable grid */
  GtkWidget *sw = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(sw, TRUE);
  gtk_box_append(GTK_BOX(outer), sw);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_widget_set_margin_start(grid, 16);
  gtk_widget_set_margin_end(grid, 16);
  gtk_widget_set_margin_top(grid, 12);
  gtk_widget_set_margin_bottom(grid, 12);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw),
                                                   TRUE);
  gtk_widget_set_focusable(grid, FALSE);

  /* Column headers */
  GtkWidget *hdr_key = gtk_label_new("Key");
  gtk_widget_add_css_class(hdr_key, "heading");
  gtk_widget_set_halign(hdr_key, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), hdr_key, 0, 0, 1, 1);
  GtkWidget *hdr_val = gtk_label_new("Value");
  gtk_widget_add_css_class(hdr_val, "heading");
  gtk_widget_set_halign(hdr_val, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), hdr_val, 1, 0, 1, 1);
  GtkWidget *hdr_desc = gtk_label_new("Description");
  gtk_widget_add_css_class(hdr_desc, "heading");
  gtk_widget_set_halign(hdr_desc, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), hdr_desc, 2, 0, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                  0, 1, 3, 1);

  int row = 2;
  for (int i = 0; i < PREF_META_N && ctx->n < 16; i++) {
    const PrefMeta *m = &PREF_META[i];

    /* Key label */
    GtkWidget *lbl = gtk_label_new(m->key);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

    /* Value widget */
    GtkWidget *val_w = NULL;
    /* find current value */
    int ival = 0;
    const char *sval = "";
    for (int j = 0; j < cur_n; j++) {
      if (!strcmp(cur[j].key, m->key)) {
        ival = cur[j].ival;
        sval = cur[j].sval ? cur[j].sval : "";
        break;
      }
    }
    if (m->type == PREF_BOOL) {
      val_w = gtk_switch_new();
      gtk_switch_set_active(GTK_SWITCH(val_w), ival ? TRUE : FALSE);
      gtk_widget_set_halign(val_w, GTK_ALIGN_START);
    } else if (m->type == PREF_INT) {
      GtkAdjustment *adj = gtk_adjustment_new(ival, m->imin, m->imax, 1, 5, 0);
      val_w = gtk_spin_button_new(adj, 1, 0);
      gtk_widget_set_halign(val_w, GTK_ALIGN_START);
    } else {
      val_w = gtk_entry_new();
      gtk_editable_set_text(GTK_EDITABLE(val_w), sval);
      gtk_widget_set_hexpand(val_w, TRUE);
      gtk_entry_set_max_length(GTK_ENTRY(val_w), 127);
    }
    gtk_grid_attach(GTK_GRID(grid), val_w, 1, row, 1, 1);

    /* Status / description */
    GtkWidget *desc_lbl = gtk_label_new(m->desc);
    gtk_widget_set_halign(desc_lbl, GTK_ALIGN_START);
    if (!file_exists)
      gtk_widget_set_opacity(desc_lbl, 0.6);
    gtk_grid_attach(GTK_GRID(grid), desc_lbl, 2, row, 1, 1);

    ctx->rows[ctx->n].meta = m;
    ctx->rows[ctx->n].val_widget = val_w;
    ctx->rows[ctx->n].status = desc_lbl;
    ctx->n++;
    row++;
  }

  /* Unknown keys section */
  if (ctx->unknown_lines->len > 0) {
    gtk_grid_attach(GTK_GRID(grid),
                    gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, row, 3,
                    1);
    row++;
    GtkWidget *unk_lbl =
        gtk_label_new("Unrecognised keys (preserved on save):");
    gtk_widget_set_halign(unk_lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class(unk_lbl, "warning");
    gtk_grid_attach(GTK_GRID(grid), unk_lbl, 0, row, 3, 1);
    row++;
    GtkWidget *unk_tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(unk_tv), FALSE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(unk_tv)),
                             ctx->unknown_lines->str, -1);
    gtk_widget_set_hexpand(unk_tv, TRUE);
    gtk_grid_attach(GTK_GRID(grid), unk_tv, 0, row, 3, 1);
  }

  /* Button row */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(btn_box, 12);
  gtk_widget_set_margin_end(btn_box, 12);
  gtk_widget_set_margin_top(btn_box, 8);
  gtk_widget_set_margin_bottom(btn_box, 12);

  if (!file_exists) {
    GtkWidget *warn =
        gtk_label_new("⚠ File does not exist — saving will create it.");
    gtk_widget_set_halign(warn, GTK_ALIGN_START);
    gtk_widget_add_css_class(warn, "warning");
    gtk_widget_set_hexpand(warn, TRUE);
    gtk_box_append(GTK_BOX(btn_box), warn);
  } else {
    GtkWidget *sp = gtk_label_new("");
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(btn_box), sp);
  }

  GtkWidget *reset_btn = gtk_button_new_with_label("Reset to defaults");
  gtk_widget_set_tooltip_text(
      reset_btn, "Reset all values to built-in defaults (does not save)");
  g_signal_connect(reset_btn, "clicked", G_CALLBACK(prefs_editor_reset_cb),
                   ctx);
  gtk_box_append(GTK_BOX(btn_box), reset_btn);

  GtkWidget *save_btn = gtk_button_new_with_label("Save to file");
  gtk_widget_add_css_class(save_btn, "suggested-action");
  g_signal_connect(save_btn, "clicked", G_CALLBACK(prefs_editor_save_cb), ctx);
  gtk_box_append(GTK_BOX(btn_box), save_btn);

  GtkWidget *close_btn = gtk_button_new_with_label("Cancel");
  g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy),
                           win);
  gtk_box_append(GTK_BOX(btn_box), close_btn);
  gtk_box_append(GTK_BOX(outer), btn_box);

  /* Free ctx when window is destroyed */
  g_object_set_data_full(G_OBJECT(win), "prefs-ed-ctx", ctx,
                         (GDestroyNotify)prefs_editor_ctx_free);

  gtk_window_present(GTK_WINDOW(win));
}

/* ============================================================
   Colours file editor — launched from Config Files > Colours tab
   ============================================================ */

typedef struct {
  const char *key;
  const char *label;
  char *field_ptr; /* points into AppState.colors */
  size_t field_sz;
} ColourMeta;

/* Forward — we need AppState* so we pass it via ColEdCtx */
typedef struct ColEdCtx ColEdCtx;

typedef struct {
  ColEdCtx *ctx;
  int idx;
  int page;             /* 0=dark, 1=light */
  GtkWidget *swatch;    /* colour preview box */
  GtkWidget *hex_entry; /* 6-char hex digits only (no #) */
  GtkWidget *r_scale;
  GtkWidget *g_scale;
  GtkWidget *b_scale;
  gboolean updating;          /* prevent feedback loops */
  GtkCssProvider *swatch_css; /* reused provider for swatch colour */
} ColourRow;

struct ColEdCtx {
  AppState *s;
  char path[PATH_MAX];
  /* rows[0..n-1] = dark page, rows[n..2n-1] = light page */
  ColourRow rows[64];
  int n;
  /* per-page entries (dark then light) */
  struct {
    const char *key;
    const char *label;
    char val[16];
  } entries[64];
};

static void colour_row_update_swatch(ColourRow *cr) {
  /* Read 6 hex digits from entry (no # prefix) */
  const char *digits = gtk_editable_get_text(GTK_EDITABLE(cr->hex_entry));
  gboolean ok = (digits && strlen(digits) == 6);
  if (ok)
    for (int i = 0; i < 6; i++)
      if (!g_ascii_isxdigit(digits[i])) {
        ok = FALSE;
        break;
      }
  if (!ok) {
    gtk_widget_add_css_class(cr->hex_entry, "error");
    return;
  }
  gtk_widget_remove_css_class(cr->hex_entry, "error");
  char css[256];
  /* Target the frame widget directly via its unique name */
  g_snprintf(css, sizeof(css),
             "#col-swatch-%d-%d { background-color: #%s;"
             " min-width:32px; min-height:24px;"
             " border-radius:4px; border:1px solid #555; }",
             cr->page, cr->idx, digits);
  if (!cr->swatch_css) {
    cr->swatch_css = gtk_css_provider_new();
    GdkDisplay *disp = gdk_display_get_default();
    if (disp)
      gtk_style_context_add_provider_for_display(
          disp, GTK_STYLE_PROVIDER(cr->swatch_css),
          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
  }
  gtk_css_provider_load_from_string(cr->swatch_css, css);
}

/* Re-read the hex entry for a given row and update its AppState colour slot
 * so that a live-preview reload picks it up. */
static void colour_row_apply_to_state(ColourRow *cr) {
  ColEdCtx *ctx = cr->ctx;
  AppState *s = ctx->s;
  const char *digits = gtk_editable_get_text(GTK_EDITABLE(cr->hex_entry));
  if (!digits || strlen(digits) != 6)
    return;
  gboolean ok = TRUE;
  for (int i = 0; i < 6; i++)
    if (!g_ascii_isxdigit(digits[i])) {
      ok = FALSE;
      break;
    }
  if (!ok)
    return;

  char hex7[8];
  g_snprintf(hex7, sizeof(hex7), "#%s", digits);

  /* Only apply if this row's page matches current dark_mode */
  if ((cr->page == 0) != (s->dark_mode ? 1 : 0))
    return;

  colors_apply_kv(s, ctx->entries[cr->page * ctx->n + cr->idx].key, hex7);
}

static void colour_hex_changed(GtkEditable *e, gpointer ud) {
  ColourRow *cr = (ColourRow *)ud;
  if (cr->updating)
    return;
  cr->updating = TRUE;
  const char *digits = gtk_editable_get_text(e);
  if (digits && strlen(digits) == 6) {
    unsigned int r = 0, g = 0, b = 0;
    sscanf(digits, "%02x%02x%02x", &r, &g, &b);
    gtk_range_set_value(GTK_RANGE(cr->r_scale), (double)r);
    gtk_range_set_value(GTK_RANGE(cr->g_scale), (double)g);
    gtk_range_set_value(GTK_RANGE(cr->b_scale), (double)b);
    colour_row_update_swatch(cr);
  }
  cr->updating = FALSE;
}

/* Callback: an RGB slider moved; update the hex colour field. */
static void colour_scale_changed(GtkRange *range, gpointer ud) {
  (void)range;
  ColourRow *cr = (ColourRow *)ud;
  if (cr->updating)
    return;
  cr->updating = TRUE;
  int r = (int)gtk_range_get_value(GTK_RANGE(cr->r_scale));
  int g = (int)gtk_range_get_value(GTK_RANGE(cr->g_scale));
  int b = (int)gtk_range_get_value(GTK_RANGE(cr->b_scale));
  char hex[8];
  g_snprintf(hex, sizeof(hex), "%02x%02x%02x", r, g, b);
  gtk_editable_set_text(GTK_EDITABLE(cr->hex_entry), hex);
  colour_row_update_swatch(cr);
  cr->updating = FALSE;
}

/* Input filter for the hex colour entry: allow only 0-9 and a-f/A-F. */
static void colour_hex_insert_filter(GtkEditable *e, const char *text,
                                     int length, int *pos, gpointer ud) {
  (void)ud;
  (void)pos;
  (void)e;
  /* Only allow hex digits — no # prefix in the entry */
  for (int i = 0; i < length; i++) {
    if (!g_ascii_isxdigit((unsigned char)text[i])) {
      g_signal_stop_emission_by_name(e, "insert-text");
      return;
    }
  }
}

static void colours_editor_reset_cb(GtkButton *btn, gpointer ud) {
  (void)btn;
  ColEdCtx *ctx = (ColEdCtx *)ud;
  /* Built-in default colours for dark and light modes */
  static const struct {
    const char *key;
    const char *dark;
    const char *light;
  } CDEFS[] = {
      {"instr", "#2aa1d3", "#0055aa"},
      {"dir", "#b48ead", "#7700aa"},
      {"data", "#7ecfc0", "#006688"},
      {"num", "#d19a66", "#885500"},
      {"label", "#00ff55", "#007700"},
      {"hover_bg", "#204020", "#c8f0c8"},
      {"reg", "#98c379", "#336600"},
      {"comment", "#70b070", "#448844"},
      {"str", "#d09070", "#884400"},
      {"inactive", "#888888", "#aaaaaa"},
      {"err_underline", "#ff5555", "#cc0000"},
      {"addr_hex", "#d19a66", "#885500"},
      {"addr_dec", "#d19a66", "#885500"},
      {"addr_bin", "#d19a66", "#885500"},
      {"imm_hex", "#d19a66", "#885500"},
      {"imm_dec", "#d19a66", "#885500"},
      {"imm_bin", "#d19a66", "#885500"},
      {"imm_char", "#d19a66", "#885500"},
      {"dv_hex", "#d19a66", "#885500"},
      {"dv_dec", "#d19a66", "#885500"},
      {"dv_bin", "#d19a66", "#885500"},
      {"dv_str", "#d09070", "#884400"},
  };
  int ncdefs = (int)(sizeof(CDEFS) / sizeof(CDEFS[0]));
  /* Reset both pages */
  for (int page = 0; page < 2; page++) {
    for (int i = 0; i < ctx->n; i++) {
      for (int j = 0; j < ncdefs; j++) {
        if (strcmp(ctx->entries[page * ctx->n + i].key, CDEFS[j].key))
          continue;
        const char *dh = (page == 0) ? CDEFS[j].dark : CDEFS[j].light;
        gtk_editable_set_text(
            GTK_EDITABLE(ctx->rows[page * ctx->n + i].hex_entry),
            (dh && dh[0] == '#') ? dh + 1 : dh);
        break;
      }
    }
  }
}

static void colours_editor_save_cb(GtkButton *btn, gpointer ud) {
  (void)btn;
  ColEdCtx *ctx = (ColEdCtx *)ud;
  /* Ensure directory exists */
  gchar *cdir = g_path_get_dirname(ctx->path);
  g_mkdir_with_parents(cdir, 0755);
  g_free(cdir);
  FILE *f = fopen(ctx->path, "w");
  if (!f) {
    char errmsg[256];
    g_snprintf(errmsg, sizeof(errmsg), "Save failed: %s", g_strerror(errno));
    set_status(ctx->s, errmsg);
    copy_toast_show(ctx->s, errmsg);
    return;
  }
  /* Write dark section then light section */
  int skipped = 0;
  fprintf(f, "; 6502caEd colour configuration\n");
  fprintf(f, "; Edit as needed. [dark] and [light] sections apply per mode.\n");
  for (int page = 0; page < 2; page++) {
    fprintf(f, "\n%s\n", page == 0 ? "[dark]" : "[light]");
    for (int i = 0; i < ctx->n; i++) {
      int ri = page * ctx->n + i;
      const char *digits =
          gtk_editable_get_text(GTK_EDITABLE(ctx->rows[ri].hex_entry));
      gboolean ok = (digits && strlen(digits) == 6);
      for (int j = 0; ok && j < 6; j++)
        if (!g_ascii_isxdigit(digits[j]))
          ok = FALSE;
      if (ok)
        fprintf(f, "%s=#%s\n", ctx->entries[ri].key, digits);
      else
        skipped++;
    }
  }
  fclose(f);
  /* Reload colours for the active mode and redraw */
  colors_set_defaults(ctx->s);
  colors_load_from_exe_dir(ctx->s);
  schedule_highlight(ctx->s);
  if (skipped > 0) {
    char wmsg[128];
    g_snprintf(wmsg, sizeof(wmsg),
               "Colours saved (%d entr%s skipped — invalid hex).", skipped,
               skipped == 1 ? "y" : "ies");
    copy_toast_show(ctx->s, wmsg);
    set_status(ctx->s, wmsg);
  } else {
    copy_toast_show(ctx->s, "Colours saved.");
    set_status(ctx->s, "Colours saved.");
  }
}

static void open_colours_editor(AppState *s, const char *path,
                                GtkWindow *parent) {
  /* Colour roles shown in the editor — same for both dark and light pages */
  static const struct {
    const char *key;
    const char *label;
  } CMETA[] = {
      {"instr", "Instructions / opcodes"},
      {"dir", "Directives"},
      {"data", "Data directives"},
      {"num", "Numbers"},
      {"label", "Labels"},
      {"hover_bg", "Label hover background"},
      {"reg", "Registers"},
      {"comment", "Comments"},
      {"str", "Strings"},
      {"inactive", "Inactive branches"},
      {"err_underline", "Error underline"},
      {"addr_hex", "Address (hex)"},
      {"addr_dec", "Address (decimal)"},
      {"addr_bin", "Address (binary)"},
      {"imm_hex", "Immediate (hex)"},
      {"imm_dec", "Immediate (decimal)"},
      {"imm_bin", "Immediate (binary)"},
      {"imm_char", "Immediate (char)"},
      {"dv_hex", "Data value (hex)"},
      {"dv_dec", "Data value (decimal)"},
      {"dv_bin", "Data value (binary)"},
      {"dv_str", "Data value (string)"},
  };
  static const int CMETA_N = (int)(sizeof(CMETA) / sizeof(CMETA[0]));

  /* Current live colours (for whichever mode is active) */
  const char *cur_vals[22] = {
      s->colors.instr,    s->colors.dir,           s->colors.data,
      s->colors.num,      s->colors.label,         s->colors.hover_bg,
      s->colors.reg,      s->colors.comment,       s->colors.str,
      s->colors.inactive, s->colors.err_underline, s->colors.addr_hex,
      s->colors.addr_dec, s->colors.addr_bin,      s->colors.imm_hex,
      s->colors.imm_dec,  s->colors.imm_bin,       s->colors.imm_char,
      s->colors.dv_hex,   s->colors.dv_dec,        s->colors.dv_bin,
      s->colors.dv_str,
  };

  /* Default colours for the OTHER mode (used to pre-populate its page) */
  static const struct {
    const char *key;
    const char *dark;
    const char *light;
  } CDEFS[] = {
      {"instr", "#2aa1d3", "#0055aa"},
      {"dir", "#b48ead", "#7700aa"},
      {"data", "#7ecfc0", "#006688"},
      {"num", "#d19a66", "#885500"},
      {"label", "#00ff55", "#007700"},
      {"hover_bg", "#204020", "#c8f0c8"},
      {"reg", "#98c379", "#336600"},
      {"comment", "#70b070", "#448844"},
      {"str", "#d09070", "#884400"},
      {"inactive", "#888888", "#aaaaaa"},
      {"err_underline", "#ff5555", "#cc0000"},
      {"addr_hex", "#d19a66", "#885500"},
      {"addr_dec", "#d19a66", "#885500"},
      {"addr_bin", "#d19a66", "#885500"},
      {"imm_hex", "#d19a66", "#885500"},
      {"imm_dec", "#d19a66", "#885500"},
      {"imm_bin", "#d19a66", "#885500"},
      {"imm_char", "#d19a66", "#885500"},
      {"dv_hex", "#d19a66", "#885500"},
      {"dv_dec", "#d19a66", "#885500"},
      {"dv_bin", "#d19a66", "#885500"},
      {"dv_str", "#d09070", "#884400"},
  };

  ColEdCtx *ctx = g_new0(ColEdCtx, 1);
  ctx->s = s;
  g_strlcpy(ctx->path, path, sizeof(ctx->path));
  ctx->n = CMETA_N;

  /* Populate entries for both pages (page 0=dark, page 1=light).
   * The active mode page gets live values; the other page gets values
   * read from the file's other-mode section (falling back to built-in
   * defaults). */
  int active_page = s->dark_mode ? 0 : 1;
  int other_page = 1 - active_page;

  /* Read the other-mode section from the file if it exists */
  char other_vals[22][16];
  for (int i = 0; i < CMETA_N; i++) {
    /* Start with built-in defaults for the other mode */
    const char *def = (other_page == 0) ? CDEFS[i].dark : CDEFS[i].light;
    g_strlcpy(other_vals[i], def ? def : "#808080", sizeof(other_vals[i]));
  }
  if (g_file_test(path, G_FILE_TEST_EXISTS)) {
    gchar *fc = NULL;
    gsize fl = 0;
    if (g_file_get_contents(path, &fc, &fl, NULL)) {
      const char *want_sec = (other_page == 0) ? "[dark]" : "[light]";
      gboolean in_sec = FALSE;
      gchar **fls = g_strsplit(fc, "\n", -1);
      for (gchar **lp = fls; lp && *lp; lp++) {
        gchar *fl2 = g_strstrip(*lp);
        if (fl2[0] == '[') {
          gchar *cb = strchr(fl2, ']');
          if (cb) {
            gchar sv = *(cb + 1);
            *(cb + 1) = 0;
            in_sec = (g_ascii_strcasecmp(fl2, want_sec) == 0);
            *(cb + 1) = sv;
          }
          continue;
        }
        if (!in_sec)
          continue;
        gchar *eq = strchr(fl2, '=');
        if (!eq)
          continue;
        *eq = 0;
        gchar *k2 = g_strstrip(fl2);
        gchar *v2 = g_strstrip(eq + 1);
        if (!colors_is_hex6(v2))
          continue;
        for (int i = 0; i < CMETA_N; i++) {
          if (g_ascii_strcasecmp(k2, CMETA[i].key) == 0) {
            g_strlcpy(other_vals[i], v2, sizeof(other_vals[i]));
            break;
          }
        }
      }
      g_strfreev(fls);
      g_free(fc);
    }
  }

  /* Fill ctx->entries for both pages */
  for (int page = 0; page < 2; page++) {
    for (int i = 0; i < CMETA_N; i++) {
      int ei = page * CMETA_N + i;
      ctx->entries[ei].key = CMETA[i].key;
      ctx->entries[ei].label = CMETA[i].label;
      if (page == active_page)
        g_strlcpy(ctx->entries[ei].val, cur_vals[i],
                  sizeof(ctx->entries[ei].val));
      else
        g_strlcpy(ctx->entries[ei].val, other_vals[i],
                  sizeof(ctx->entries[ei].val));
    }
  }

  gboolean file_exists = g_file_test(path, G_FILE_TEST_EXISTS);

  /* Build window */
  GtkWidget *win = gtk_window_new();
  char title[256];
  g_snprintf(title, sizeof(title), "Edit Colours — %s",
             file_exists ? path : "(new file)");
  gtk_window_set_title(GTK_WINDOW(win), title);
  gtk_window_set_transient_for(GTK_WINDOW(win), parent ? parent : s->win);
  gtk_window_set_modal(GTK_WINDOW(win), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(win), 720, 700);
  apply_window_dark_hint(win, s->dark_mode);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(win), outer);

  /* Dark / Light tab switcher using GtkStackSwitcher + GtkStack */
  GtkWidget *stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(stack),
                                GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

  GtkWidget *switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(stack));
  gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(switcher, 8);
  gtk_widget_set_margin_bottom(switcher, 4);
  gtk_box_append(GTK_BOX(outer), switcher);
  gtk_widget_set_vexpand(stack, TRUE);
  gtk_box_append(GTK_BOX(outer), stack);

  /* Build one grid per page (dark=0, light=1) */
  for (int page = 0; page < 2; page++) {
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_widget_set_margin_start(grid, 16);
    gtk_widget_set_margin_end(grid, 16);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), grid);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw),
                                                     TRUE);
    gtk_widget_set_focusable(grid, FALSE);

    /* Column headers */
    const char *hdrs[] = {"Colour", "#", "Hex", "R", "G", "B", "Description"};
    for (int c = 0; c < 7; c++) {
      GtkWidget *h = gtk_label_new(hdrs[c]);
      gtk_widget_add_css_class(h, "heading");
      gtk_widget_set_halign(h, GTK_ALIGN_START);
      gtk_grid_attach(GTK_GRID(grid), h, c, 0, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(grid),
                    gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, 1, 7, 1);

    for (int i = 0; i < CMETA_N; i++) {
      int ri = page * CMETA_N + i;
      ColourRow *cr = &ctx->rows[ri];
      cr->ctx = ctx;
      cr->idx = i;
      cr->page = page;
      cr->updating = FALSE;

      const char *hex = ctx->entries[ri].val;
      unsigned int r = 0x80, g2 = 0x80, b = 0x80;
      const char *digits6 =
          (hex && hex[0] == '#' && strlen(hex) == 7) ? hex + 1 : "808080";
      if (hex && hex[0] == '#' && strlen(hex) == 7)
        sscanf(hex + 1, "%02x%02x%02x", &r, &g2, &b);

      int row = i + 2;

      /* Swatch */
      cr->swatch = gtk_frame_new(NULL);
      cr->swatch_css = NULL;
      char wname[32];
      g_snprintf(wname, sizeof(wname), "col-swatch-%d-%d", page, i);
      gtk_widget_set_name(cr->swatch, wname);
      gtk_widget_set_size_request(cr->swatch, 32, 24);
      gtk_widget_set_valign(cr->swatch, GTK_ALIGN_CENTER);
      gtk_grid_attach(GTK_GRID(grid), cr->swatch, 0, row, 1, 1);

      GtkWidget *hash_lbl = gtk_label_new("#");
      gtk_widget_set_valign(hash_lbl, GTK_ALIGN_CENTER);
      gtk_grid_attach(GTK_GRID(grid), hash_lbl, 1, row, 1, 1);

      cr->hex_entry = gtk_entry_new();
      gtk_entry_set_max_length(GTK_ENTRY(cr->hex_entry), 6);
      gtk_editable_set_text(GTK_EDITABLE(cr->hex_entry), digits6);
      gtk_widget_set_size_request(cr->hex_entry, 72, -1);
      g_signal_connect(cr->hex_entry, "insert-text",
                       G_CALLBACK(colour_hex_insert_filter), NULL);
      g_signal_connect(cr->hex_entry, "changed", G_CALLBACK(colour_hex_changed),
                       cr);
      gtk_grid_attach(GTK_GRID(grid), cr->hex_entry, 2, row, 1, 1);

      cr->r_scale =
          gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
      gtk_scale_set_draw_value(GTK_SCALE(cr->r_scale), FALSE);
      gtk_range_set_value(GTK_RANGE(cr->r_scale), (double)r);
      gtk_widget_set_size_request(cr->r_scale, 80, -1);
      g_signal_connect(cr->r_scale, "value-changed",
                       G_CALLBACK(colour_scale_changed), cr);
      gtk_grid_attach(GTK_GRID(grid), cr->r_scale, 3, row, 1, 1);

      cr->g_scale =
          gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
      gtk_scale_set_draw_value(GTK_SCALE(cr->g_scale), FALSE);
      gtk_range_set_value(GTK_RANGE(cr->g_scale), (double)g2);
      gtk_widget_set_size_request(cr->g_scale, 80, -1);
      g_signal_connect(cr->g_scale, "value-changed",
                       G_CALLBACK(colour_scale_changed), cr);
      gtk_grid_attach(GTK_GRID(grid), cr->g_scale, 4, row, 1, 1);

      cr->b_scale =
          gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 255, 1);
      gtk_scale_set_draw_value(GTK_SCALE(cr->b_scale), FALSE);
      gtk_range_set_value(GTK_RANGE(cr->b_scale), (double)b);
      gtk_widget_set_size_request(cr->b_scale, 80, -1);
      g_signal_connect(cr->b_scale, "value-changed",
                       G_CALLBACK(colour_scale_changed), cr);
      gtk_grid_attach(GTK_GRID(grid), cr->b_scale, 5, row, 1, 1);

      GtkWidget *lbl = gtk_label_new(CMETA[i].label);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      gtk_widget_set_hexpand(lbl, TRUE);
      gtk_grid_attach(GTK_GRID(grid), lbl, 6, row, 1, 1);

      colour_row_update_swatch(cr);
    }

    gtk_stack_add_titled(GTK_STACK(stack), sw, page == 0 ? "dark" : "light",
                         page == 0 ? "🌙 Dark" : "☀ Light");
  }

  /* Start on the tab matching the current mode */
  gtk_stack_set_visible_child_name(GTK_STACK(stack),
                                   s->dark_mode ? "dark" : "light");

  /* Button row */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(btn_box, 12);
  gtk_widget_set_margin_end(btn_box, 12);
  gtk_widget_set_margin_top(btn_box, 8);
  gtk_widget_set_margin_bottom(btn_box, 12);

  if (!file_exists) {
    GtkWidget *warn =
        gtk_label_new("⚠ File does not exist — saving will create it.");
    gtk_widget_add_css_class(warn, "warning");
    gtk_widget_set_hexpand(warn, TRUE);
    gtk_box_append(GTK_BOX(btn_box), warn);
  } else {
    GtkWidget *sp = gtk_label_new("");
    gtk_widget_set_hexpand(sp, TRUE);
    gtk_box_append(GTK_BOX(btn_box), sp);
  }

  GtkWidget *creset_btn = gtk_button_new_with_label("Reset to defaults");
  gtk_widget_set_tooltip_text(
      creset_btn,
      "Reset all colours (both dark and light) to built-in defaults");
  g_signal_connect(creset_btn, "clicked", G_CALLBACK(colours_editor_reset_cb),
                   ctx);
  gtk_box_append(GTK_BOX(btn_box), creset_btn);

  GtkWidget *save_btn = gtk_button_new_with_label("Save to file");
  gtk_widget_add_css_class(save_btn, "suggested-action");
  g_signal_connect(save_btn, "clicked", G_CALLBACK(colours_editor_save_cb),
                   ctx);
  gtk_box_append(GTK_BOX(btn_box), save_btn);

  GtkWidget *close_btn = gtk_button_new_with_label("Close");
  g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy),
                           win);
  gtk_box_append(GTK_BOX(btn_box), close_btn);
  gtk_box_append(GTK_BOX(outer), btn_box);

  g_object_set_data_full(G_OBJECT(win), "col-ed-ctx", ctx,
                         (GDestroyNotify)g_free);

  gtk_window_present(GTK_WINDOW(win));
}

/* Action handler: undo the last edit in the active document. */
static void action_undo(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  if (tb && gtk_text_buffer_get_can_undo(tb))
    gtk_text_buffer_undo(tb);
}
/* Action handler: redo the last undone edit in the active document. */
static void action_redo(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  GtkTextBuffer *tb2 = GTK_TEXT_BUFFER(s->buf);
  if (tb2 && gtk_text_buffer_get_can_redo(tb2))
    gtk_text_buffer_redo(tb2);
}
static void action_save_prefs(GSimpleAction *a, GVariant *p,
                              gpointer user_data);
/* Action handler: open the Config Files dialog. */
static void action_config_files(GSimpleAction *a, GVariant *p,
                                gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;

  CfgDlgState *ds = g_new0(CfgDlgState, 1);
  ds->s = s;

  GtkWidget *dlg = gtk_window_new();
  ds->dlg = dlg;
  gtk_window_set_title(GTK_WINDOW(dlg), "Config Files");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), s->win);
  gtk_window_set_modal(GTK_WINDOW(dlg), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(dlg), 620, 420);
  apply_window_dark_hint(dlg, s->dark_mode);
  if (s->app)
    gtk_application_add_window(s->app, GTK_WINDOW(dlg));

  // Free ds when dialog is destroyed
  g_object_set_data_full(G_OBJECT(dlg), "cfg-state", ds, cfg_dlg_free);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(dlg), outer);

  // Outer notebook: Preferences | Colours
  GtkWidget *outer_nb = gtk_notebook_new();
  gtk_widget_set_hexpand(outer_nb, TRUE);
  gtk_widget_set_vexpand(outer_nb, TRUE);
  gtk_box_append(GTK_BOX(outer), outer_nb);

  const char *outer_labels[2] = {"Preferences (prefs.ini)",
                                 "Colours (6502caEd.colours)"};
  const char *inner_labels[3] = {"Project", "Executable", "User"};

  for (int grp = 0; grp < 2; grp++) {
    GtkWidget *inner_nb = gtk_notebook_new();
    for (int loc = 0; loc < 3; loc++) {
      int idx = grp * 3 + loc;
      GtkWidget *panel = cfg_build_panel(ds, idx);
      GtkWidget *tab_lbl = gtk_label_new(inner_labels[loc]);
      gtk_notebook_append_page(GTK_NOTEBOOK(inner_nb), panel, tab_lbl);
    }
    GtkWidget *outer_lbl = gtk_label_new(outer_labels[grp]);
    gtk_notebook_append_page(GTK_NOTEBOOK(outer_nb), inner_nb, outer_lbl);
  }

  // Button row: Close (right)
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_start(btn_box, 12);
  gtk_widget_set_margin_end(btn_box, 12);
  gtk_widget_set_margin_top(btn_box, 8);
  gtk_widget_set_margin_bottom(btn_box, 12);
  /* Spacer pushes Close to the right */
  GtkWidget *spacer = gtk_label_new("");
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(btn_box), spacer);
  GtkWidget *close_btn = gtk_button_new_with_label("Close");
  gtk_box_append(GTK_BOX(btn_box), close_btn);
  gtk_box_append(GTK_BOX(outer), btn_box);
  g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(gtk_window_destroy),
                           dlg);
  gtk_window_set_default_widget(GTK_WINDOW(dlg), close_btn);

  // Escape closes
  GtkEventController *kc = gtk_event_controller_key_new();
  g_signal_connect(kc, "key-pressed", G_CALLBACK(about_key_pressed), dlg);
  gtk_widget_add_controller(dlg, kc);

  // Populate path/status labels
  cfg_dlg_refresh(ds);

  gtk_window_present(GTK_WINDOW(dlg));
}

/* =========================================================================
 * Instruction Reference Browser
 * ========================================================================= */

/* CPU availability flags */
#define IREF_6502 (1u << 0)
#define IREF_65C02 (1u << 1)
#define IREF_65816 (1u << 2)
#define IREF_ALL (IREF_6502 | IREF_65C02 | IREF_65816)
#define IREF_UNDOC (1u << 3) /* illegal/undocumented NMOS 6502 */

typedef struct {
  const char *mode;
  uint8_t bytes;
  const char *cycles;
  const char *opcode;
  uint8_t cpu; /* IREF_6502 | IREF_65C02 | IREF_65816 */
} iref_mode_t;

typedef struct {
  const char *mnem;      /* e.g. "ADC" */
  const char *full_name; /* e.g. "Add with Carry" */
  const char *operation; /* e.g. "A,C = A+M+C" */
  const char *desc;      /* paragraph description */
  /* flags affected: C Z I D B V N  — 'y' set/cleared, 'n' not affected,
     '0' always cleared, '1' always set, '-' not applicable */
  const char flags[8];      /* C Z I D B V N  (index 0..6, [7]=NUL) */
  const char *flags_note;   /* extra flag notes */
  uint8_t cpu_mask;         /* which CPUs support this mnemonic at all */
  const char *group;        /* "Arithmetic","Load/Store","Branch","Jump",
                               "Stack","Transfer","Logical","Shift","Bit",
                               "Compare","Control","65816" */
  const iref_mode_t *modes; /* addressing mode table, NULL-terminated */
} iref_t;

/* ---- Mode tables (per mnemonic) ---- */
static const iref_mode_t iref_modes_adc[] = {
    {"Immediate", 2, "2", "$69", IREF_ALL},
    {"Zero page", 2, "3", "$65", IREF_ALL},
    {"Zero page, X", 2, "4", "$75", IREF_ALL},
    {"Zero page indirect", 2, "5", "$72", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$6D", IREF_ALL},
    {"Absolute, X", 3, "4+", "$7D", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$79", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$61", IREF_ALL},
    {"(Indirect), Y", 2, "5+", "$71", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$67", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$77", IREF_65816},
    {"Absolute long", 4, "5", "$6F", IREF_65816},
    {"Absolute long, X", 4, "5", "$7F", IREF_65816},
    {"Stack relative", 2, "4", "$63", IREF_65816},
    {"(SR, S), Y", 2, "7", "$73", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_and[] = {
    {"Immediate", 2, "2", "$29", IREF_ALL},
    {"Zero page", 2, "3", "$25", IREF_ALL},
    {"Zero page, X", 2, "4", "$35", IREF_ALL},
    {"Zero page indirect", 2, "5", "$32", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$2D", IREF_ALL},
    {"Absolute, X", 3, "4+", "$3D", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$39", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$21", IREF_ALL},
    {"(Indirect), Y", 2, "5+", "$31", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$27", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$37", IREF_65816},
    {"Absolute long", 4, "5", "$2F", IREF_65816},
    {"Absolute long, X", 4, "5", "$3F", IREF_65816},
    {"Stack relative", 2, "4", "$23", IREF_65816},
    {"(SR, S), Y", 2, "7", "$33", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_asl[] = {
    {"Accumulator", 1, "2", "$0A", IREF_ALL},
    {"Zero page", 2, "5", "$06", IREF_ALL},
    {"Zero page, X", 2, "6", "$16", IREF_ALL},
    {"Absolute", 3, "6", "$0E", IREF_ALL},
    {"Absolute, X", 3, "7", "$1E", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bcc[] = {
    {"Relative", 2, "2++", "$90", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bcs[] = {
    {"Relative", 2, "2++", "$B0", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_beq[] = {
    {"Relative", 2, "2++", "$F0", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bit[] = {
    {"Zero page", 2, "3", "$24", IREF_ALL},
    {"Absolute", 3, "4", "$2C", IREF_ALL},
    {"Immediate", 2, "2", "$89", IREF_65C02 | IREF_65816},
    {"Zero page, X", 2, "4", "$34", IREF_65C02 | IREF_65816},
    {"Absolute, X", 3, "4+", "$3C", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bmi[] = {
    {"Relative", 2, "2++", "$30", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bne[] = {
    {"Relative", 2, "2++", "$D0", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bpl[] = {
    {"Relative", 2, "2++", "$10", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bra[] = {
    {"Relative", 2, "2++", "$80", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_brl[] = {
    {"Long relative", 3, "4", "$82", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_brk[] = {
    {"Implied", 1, "7", "$00", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bvc[] = {
    {"Relative", 2, "2++", "$50", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_bvs[] = {
    {"Relative", 2, "2++", "$70", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_clc[] = {
    {"Implied", 1, "2", "$18", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_cld[] = {
    {"Implied", 1, "2", "$D8", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_cli[] = {
    {"Implied", 1, "2", "$58", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_clv[] = {
    {"Implied", 1, "2", "$B8", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_cmp[] = {
    {"Immediate", 2, "2", "$C9", IREF_ALL},
    {"Zero page", 2, "3", "$C5", IREF_ALL},
    {"Zero page, X", 2, "4", "$D5", IREF_ALL},
    {"Zero page indirect", 2, "5", "$D2", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$CD", IREF_ALL},
    {"Absolute, X", 3, "4+", "$DD", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$D9", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$C1", IREF_ALL},
    {"(Indirect), Y", 2, "5+", "$D1", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$C7", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$D7", IREF_65816},
    {"Absolute long", 4, "5", "$CF", IREF_65816},
    {"Absolute long, X", 4, "5", "$DF", IREF_65816},
    {"Stack relative", 2, "4", "$C3", IREF_65816},
    {"(SR, S), Y", 2, "7", "$D3", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_cpx[] = {
    {"Immediate", 2, "2", "$E0", IREF_ALL},
    {"Zero page", 2, "3", "$E4", IREF_ALL},
    {"Absolute", 3, "4", "$EC", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_cpy[] = {
    {"Immediate", 2, "2", "$C0", IREF_ALL},
    {"Zero page", 2, "3", "$C4", IREF_ALL},
    {"Absolute", 3, "4", "$CC", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_dec[] = {
    {"Accumulator", 1, "2", "$3A", IREF_65C02 | IREF_65816},
    {"Zero page", 2, "5", "$C6", IREF_ALL},
    {"Zero page, X", 2, "6", "$D6", IREF_ALL},
    {"Absolute", 3, "6", "$CE", IREF_ALL},
    {"Absolute, X", 3, "7", "$DE", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_dex[] = {
    {"Implied", 1, "2", "$CA", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_dey[] = {
    {"Implied", 1, "2", "$88", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_eor[] = {
    {"Immediate", 2, "2", "$49", IREF_ALL},
    {"Zero page", 2, "3", "$45", IREF_ALL},
    {"Zero page, X", 2, "4", "$55", IREF_ALL},
    {"Zero page indirect", 2, "5", "$52", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$4D", IREF_ALL},
    {"Absolute, X", 3, "4+", "$5D", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$59", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$41", IREF_ALL},
    {"(Indirect), Y", 2, "5+", "$51", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$47", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$57", IREF_65816},
    {"Absolute long", 4, "5", "$4F", IREF_65816},
    {"Absolute long, X", 4, "5", "$5F", IREF_65816},
    {"Stack relative", 2, "4", "$43", IREF_65816},
    {"(SR, S), Y", 2, "7", "$53", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_inc[] = {
    {"Accumulator", 1, "2", "$1A", IREF_65C02 | IREF_65816},
    {"Zero page", 2, "5", "$E6", IREF_ALL},
    {"Zero page, X", 2, "6", "$F6", IREF_ALL},
    {"Absolute", 3, "6", "$EE", IREF_ALL},
    {"Absolute, X", 3, "7", "$FE", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_inx[] = {
    {"Implied", 1, "2", "$E8", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_iny[] = {
    {"Implied", 1, "2", "$C8", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_jml[] = {
    {"Absolute long", 4, "4", "$5C", IREF_65816},
    {"[Absolute] indirect long", 3, "5", "$DC", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_jmp[] = {
    {"Absolute", 3, "3", "$4C", IREF_ALL},
    {"Indirect", 3, "5", "$6C", IREF_ALL},
    {"(Absolute, X) indirect", 3, "6", "$7C", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_jsl[] = {
    {"Absolute long", 4, "8", "$22", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_jsr[] = {
    {"Absolute", 3, "6", "$20", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_lda[] = {
    {"Immediate", 2, "2", "$A9", IREF_ALL},
    {"Zero page", 2, "3", "$A5", IREF_ALL},
    {"Zero page, X", 2, "4", "$B5", IREF_ALL},
    {"Zero page indirect", 2, "5", "$B2", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$AD", IREF_ALL},
    {"Absolute, X", 3, "4+", "$BD", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$B9", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$A1", IREF_ALL},
    {"(Indirect), Y", 2, "5+", "$B1", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$A7", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$B7", IREF_65816},
    {"Absolute long", 4, "5", "$AF", IREF_65816},
    {"Absolute long, X", 4, "5", "$BF", IREF_65816},
    {"Stack relative", 2, "4", "$A3", IREF_65816},
    {"(SR, S), Y", 2, "7", "$B3", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_ldx[] = {
    {"Immediate", 2, "2", "$A2", IREF_ALL},
    {"Zero page", 2, "3", "$A6", IREF_ALL},
    {"Zero page, Y", 2, "4", "$B6", IREF_ALL},
    {"Absolute", 3, "4", "$AE", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$BE", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_ldy[] = {
    {"Immediate", 2, "2", "$A0", IREF_ALL},
    {"Zero page", 2, "3", "$A4", IREF_ALL},
    {"Zero page, X", 2, "4", "$B4", IREF_ALL},
    {"Absolute", 3, "4", "$AC", IREF_ALL},
    {"Absolute, X", 3, "4+", "$BC", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_lsr[] = {
    {"Accumulator", 1, "2", "$4A", IREF_ALL},
    {"Zero page", 2, "5", "$46", IREF_ALL},
    {"Zero page, X", 2, "6", "$56", IREF_ALL},
    {"Absolute", 3, "6", "$4E", IREF_ALL},
    {"Absolute, X", 3, "7", "$5E", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_mvn[] = {
    {"Block (src,dst banks)", 3, "~", "$54", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_mvp[] = {
    {"Block (src,dst banks)", 3, "~", "$44", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_nop[] = {
    {"Implied", 1, "2", "$EA", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_ora[] = {
    {"Immediate", 2, "2", "$09", IREF_ALL},
    {"Zero page", 2, "3", "$05", IREF_ALL},
    {"Zero page, X", 2, "4", "$15", IREF_ALL},
    {"Zero page indirect", 2, "5", "$12", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$0D", IREF_ALL},
    {"Absolute, X", 3, "4+", "$1D", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$19", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$01", IREF_ALL},
    {"(Indirect), Y", 2, "5+", "$11", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$07", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$17", IREF_65816},
    {"Absolute long", 4, "5", "$0F", IREF_65816},
    {"Absolute long, X", 4, "5", "$1F", IREF_65816},
    {"Stack relative", 2, "4", "$03", IREF_65816},
    {"(SR, S), Y", 2, "7", "$13", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_pea[] = {
    {"Absolute", 3, "5", "$F4", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_pei[] = {
    {"(dp) indirect", 2, "6", "$D4", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_per[] = {
    {"Long relative", 3, "6", "$62", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_pha[] = {
    {"Implied", 1, "3", "$48", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_phb[] = {
    {"Implied", 1, "3", "$8B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_phd[] = {
    {"Implied", 1, "4", "$0B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_phk[] = {
    {"Implied", 1, "3", "$4B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_php[] = {
    {"Implied", 1, "3", "$08", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_phx[] = {
    {"Implied", 1, "3", "$DA", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_phy[] = {
    {"Implied", 1, "3", "$5A", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_pla[] = {
    {"Implied", 1, "4", "$68", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_plb[] = {
    {"Implied", 1, "4", "$AB", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_pld[] = {
    {"Implied", 1, "5", "$2B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_plp[] = {
    {"Implied", 1, "4", "$28", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_plx[] = {
    {"Implied", 1, "4", "$FA", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_ply[] = {
    {"Implied", 1, "4", "$7A", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_rep[] = {
    {"Immediate", 2, "3", "$C2", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_rol[] = {
    {"Accumulator", 1, "2", "$2A", IREF_ALL},
    {"Zero page", 2, "5", "$26", IREF_ALL},
    {"Zero page, X", 2, "6", "$36", IREF_ALL},
    {"Absolute", 3, "6", "$2E", IREF_ALL},
    {"Absolute, X", 3, "7", "$3E", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_ror[] = {
    {"Accumulator", 1, "2", "$6A", IREF_ALL},
    {"Zero page", 2, "5", "$66", IREF_ALL},
    {"Zero page, X", 2, "6", "$76", IREF_ALL},
    {"Absolute", 3, "6", "$6E", IREF_ALL},
    {"Absolute, X", 3, "7", "$7E", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_rti[] = {
    {"Implied", 1, "6", "$40", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_rtl[] = {
    {"Implied", 1, "6", "$6B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_rts[] = {
    {"Implied", 1, "6", "$60", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sbc[] = {
    {"Immediate", 2, "2", "$E9", IREF_ALL},
    {"Zero page", 2, "3", "$E5", IREF_ALL},
    {"Zero page, X", 2, "4", "$F5", IREF_ALL},
    {"Zero page indirect", 2, "5", "$F2", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$ED", IREF_ALL},
    {"Absolute, X", 3, "4+", "$FD", IREF_ALL},
    {"Absolute, Y", 3, "4+", "$F9", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$E1", IREF_ALL},
    {"(Indirect), Y", 2, "5+", "$F1", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$E7", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$F7", IREF_65816},
    {"Absolute long", 4, "5", "$EF", IREF_65816},
    {"Absolute long, X", 4, "5", "$FF", IREF_65816},
    {"Stack relative", 2, "4", "$E3", IREF_65816},
    {"(SR, S), Y", 2, "7", "$F3", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sec[] = {
    {"Implied", 1, "2", "$38", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sed[] = {
    {"Implied", 1, "2", "$F8", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sei[] = {
    {"Implied", 1, "2", "$78", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sep[] = {
    {"Immediate", 2, "3", "$E2", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sta[] = {
    {"Zero page", 2, "3", "$85", IREF_ALL},
    {"Zero page, X", 2, "4", "$95", IREF_ALL},
    {"Zero page indirect", 2, "6", "$92", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$8D", IREF_ALL},
    {"Absolute, X", 3, "5", "$9D", IREF_ALL},
    {"Absolute, Y", 3, "5", "$99", IREF_ALL},
    {"(Indirect, X)", 2, "6", "$81", IREF_ALL},
    {"(Indirect), Y", 2, "6", "$91", IREF_ALL},
    {"[dp] indirect long", 2, "6", "$87", IREF_65816},
    {"[dp], Y indirect long", 2, "6", "$97", IREF_65816},
    {"Absolute long", 4, "5", "$8F", IREF_65816},
    {"Absolute long, X", 4, "5", "$9F", IREF_65816},
    {"Stack relative", 2, "4", "$83", IREF_65816},
    {"(SR, S), Y", 2, "7", "$93", IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_stp[] = {
    {"Implied", 1, "3", "$DB", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_stx[] = {
    {"Zero page", 2, "3", "$86", IREF_ALL},
    {"Zero page, Y", 2, "4", "$96", IREF_ALL},
    {"Absolute", 3, "4", "$8E", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sty[] = {
    {"Zero page", 2, "3", "$84", IREF_ALL},
    {"Zero page, X", 2, "4", "$94", IREF_ALL},
    {"Absolute", 3, "4", "$8C", IREF_ALL},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_stz[] = {
    {"Zero page", 2, "3", "$64", IREF_65C02 | IREF_65816},
    {"Zero page, X", 2, "4", "$74", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "4", "$9C", IREF_65C02 | IREF_65816},
    {"Absolute, X", 3, "5", "$9E", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tax[] = {
    {"Implied", 1, "2", "$AA", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tay[] = {
    {"Implied", 1, "2", "$A8", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tcd[] = {
    {"Implied", 1, "2", "$5B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tcs[] = {
    {"Implied", 1, "2", "$1B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tdc[] = {
    {"Implied", 1, "2", "$7B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_trb[] = {
    {"Zero page", 2, "5", "$14", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "6", "$1C", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tsb[] = {
    {"Zero page", 2, "5", "$04", IREF_65C02 | IREF_65816},
    {"Absolute", 3, "6", "$0C", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tsc[] = {
    {"Implied", 1, "2", "$3B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tsx[] = {
    {"Implied", 1, "2", "$BA", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_txa[] = {
    {"Implied", 1, "2", "$8A", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_txs[] = {
    {"Implied", 1, "2", "$9A", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_txy[] = {
    {"Implied", 1, "2", "$9B", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tya[] = {
    {"Implied", 1, "2", "$98", IREF_ALL}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_tyx[] = {
    {"Implied", 1, "2", "$BB", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_wai[] = {
    {"Implied", 1, "3", "$CB", IREF_65C02 | IREF_65816},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_xba[] = {
    {"Implied", 1, "3", "$EB", IREF_65816}, {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_xce[] = {
    {"Implied", 1, "2", "$FB", IREF_65816}, {NULL, 0, NULL, NULL, 0}};

/* ---- Undocumented/illegal 6502 mode tables ---- */
static const iref_mode_t iref_modes_lax[] = {
    {"Immediate", 2, "2", "$AB", IREF_6502},
    {"Zero page", 2, "3", "$A7", IREF_6502},
    {"Zero page, Y", 2, "4", "$B7", IREF_6502},
    {"Absolute", 3, "4", "$AF", IREF_6502},
    {"Absolute, Y", 3, "4+", "$BF", IREF_6502},
    {"(Indirect, X)", 2, "6", "$A3", IREF_6502},
    {"(Indirect), Y", 2, "5+", "$B3", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sax[] = {
    {"Zero page", 2, "3", "$87", IREF_6502},
    {"Zero page, Y", 2, "4", "$97", IREF_6502},
    {"Absolute", 3, "4", "$8F", IREF_6502},
    {"(Indirect, X)", 2, "6", "$83", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_slo[] = {
    {"Zero page", 2, "5", "$07", IREF_6502},
    {"Zero page, X", 2, "6", "$17", IREF_6502},
    {"Absolute", 3, "6", "$0F", IREF_6502},
    {"Absolute, X", 3, "7", "$1F", IREF_6502},
    {"Absolute, Y", 3, "7", "$1B", IREF_6502},
    {"(Indirect, X)", 2, "8", "$03", IREF_6502},
    {"(Indirect), Y", 2, "8", "$13", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_rla[] = {
    {"Zero page", 2, "5", "$27", IREF_6502},
    {"Zero page, X", 2, "6", "$37", IREF_6502},
    {"Absolute", 3, "6", "$2F", IREF_6502},
    {"Absolute, X", 3, "7", "$3F", IREF_6502},
    {"Absolute, Y", 3, "7", "$3B", IREF_6502},
    {"(Indirect, X)", 2, "8", "$23", IREF_6502},
    {"(Indirect), Y", 2, "8", "$33", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_sre[] = {
    {"Zero page", 2, "5", "$47", IREF_6502},
    {"Zero page, X", 2, "6", "$57", IREF_6502},
    {"Absolute", 3, "6", "$4F", IREF_6502},
    {"Absolute, X", 3, "7", "$5F", IREF_6502},
    {"Absolute, Y", 3, "7", "$5B", IREF_6502},
    {"(Indirect, X)", 2, "8", "$43", IREF_6502},
    {"(Indirect), Y", 2, "8", "$53", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_rra[] = {
    {"Zero page", 2, "5", "$67", IREF_6502},
    {"Zero page, X", 2, "6", "$77", IREF_6502},
    {"Absolute", 3, "6", "$6F", IREF_6502},
    {"Absolute, X", 3, "7", "$7F", IREF_6502},
    {"Absolute, Y", 3, "7", "$7B", IREF_6502},
    {"(Indirect, X)", 2, "8", "$63", IREF_6502},
    {"(Indirect), Y", 2, "8", "$73", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_dcp[] = {
    {"Zero page", 2, "5", "$C7", IREF_6502},
    {"Zero page, X", 2, "6", "$D7", IREF_6502},
    {"Absolute", 3, "6", "$CF", IREF_6502},
    {"Absolute, X", 3, "7", "$DF", IREF_6502},
    {"Absolute, Y", 3, "7", "$DB", IREF_6502},
    {"(Indirect, X)", 2, "8", "$C3", IREF_6502},
    {"(Indirect), Y", 2, "8", "$D3", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};
static const iref_mode_t iref_modes_isc[] = {
    {"Zero page", 2, "5", "$E7", IREF_6502},
    {"Zero page, X", 2, "6", "$F7", IREF_6502},
    {"Absolute", 3, "6", "$EF", IREF_6502},
    {"Absolute, X", 3, "7", "$FF", IREF_6502},
    {"Absolute, Y", 3, "7", "$FB", IREF_6502},
    {"(Indirect, X)", 2, "8", "$E3", IREF_6502},
    {"(Indirect), Y", 2, "8", "$F3", IREF_6502},
    {NULL, 0, NULL, NULL, 0}};

/* ---- Master instruction table ---- */
/* flags: index 0=C 1=Z 2=I 3=D 4=B 5=V 6=N */
#define FL(c, z, i, d, b, v, n)                                                \
  { c, z, i, d, b, v, n, 0 }
static const iref_t g_iref[] = {
    {"ADC", "Add with Carry", "A,C = A+M+C",
     "Adds the contents of a memory location to the accumulator together with "
     "the carry bit. "
     "If overflow occurs the carry bit is set, enabling multi-byte addition.",
     FL('y', 'y', 'n', 'n', 'n', 'y', 'y'), "(+1 cycle if decimal mode set)",
     IREF_ALL, "Arithmetic", iref_modes_adc},
    {"AND", "Logical AND", "A = A & M",
     "Performs a bitwise AND between the accumulator and a memory value, "
     "storing the result in the accumulator.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Logical",
     iref_modes_and},
    {"ASL", "Arithmetic Shift Left", "A,C = M*2",
     "Shifts all bits of the accumulator or memory location one bit to the "
     "left. "
     "Bit 0 is set to 0 and bit 7 (or 15 in 16-bit mode) is placed in the "
     "carry flag.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Shift",
     iref_modes_asl},
    {"BCC", "Branch if Carry Clear", NULL,
     "Branches to a new location if the carry flag is clear.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_bcc},
    {"BCS", "Branch if Carry Set", NULL,
     "Branches to a new location if the carry flag is set.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_bcs},
    {"BEQ", "Branch if Equal (Zero Set)", NULL,
     "Branches to a new location if the zero flag is set.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_beq},
    {"BIT", "Bit Test", "A & M",
     "Tests bits in memory by ANDing with the accumulator. The result is not "
     "stored. "
     "Bit 7 of the memory value is transferred to the N flag, bit 6 to the V "
     "flag.",
     FL('n', 'y', 'n', 'n', 'n', 'y', 'y'),
     "N <- M7, V <- M6 (not affected for Immediate mode)", IREF_ALL, "Bit",
     iref_modes_bit},
    {"BMI", "Branch if Minus (Negative Set)", NULL,
     "Branches to a new location if the negative flag is set.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_bmi},
    {"BNE", "Branch if Not Equal (Zero Clear)", NULL,
     "Branches to a new location if the zero flag is clear.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_bne},
    {"BPL", "Branch if Plus (Negative Clear)", NULL,
     "Branches to a new location if the negative flag is clear.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_bpl},
    {"BRA", "Branch Always", NULL,
     "Unconditional 8-bit relative branch. Range is -128 to +127 bytes from "
     "the next instruction. 65C02/65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65C02 | IREF_65816,
     "Branch", iref_modes_bra},
    {"BRK", "Force Interrupt", NULL,
     "Forces a software interrupt. The program counter and processor status "
     "are pushed to the stack, "
     "and the IRQ interrupt vector at $FFFE/$FFFF is loaded into the PC.",
     FL('n', 'n', '1', 'n', '1', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_brk},
    {"BRL", "Branch Long", NULL,
     "16-bit relative branch with a range of -32768 to +32767 bytes. 65816 "
     "only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Branch",
     iref_modes_brl},
    {"BVC", "Branch if Overflow Clear", NULL,
     "Branches to a new location if the overflow flag is clear.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_bvc},
    {"BVS", "Branch if Overflow Set", NULL,
     "Branches to a new location if the overflow flag is set.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Branch",
     iref_modes_bvs},
    {"CLC", "Clear Carry Flag", "C = 0", "Clears the carry flag.",
     FL('0', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_clc},
    {"CLD", "Clear Decimal Mode", "D = 0",
     "Clears the decimal mode flag, switching back to binary arithmetic.",
     FL('n', 'n', 'n', '0', 'n', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_cld},
    {"CLI", "Clear Interrupt Disable", "I = 0",
     "Clears the interrupt disable flag, allowing IRQ interrupts.",
     FL('n', 'n', '0', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_cli},
    {"CLV", "Clear Overflow Flag", "V = 0", "Clears the overflow flag.",
     FL('n', 'n', 'n', 'n', 'n', '0', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_clv},
    {"CMP", "Compare Accumulator", "A - M",
     "Compares the accumulator with a memory value. Sets flags as if "
     "subtraction were performed, "
     "but does not store the result.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Compare",
     iref_modes_cmp},
    {"CPX", "Compare X Register", "X - M",
     "Compares the X register with a memory value.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Compare",
     iref_modes_cpx},
    {"CPY", "Compare Y Register", "Y - M",
     "Compares the Y register with a memory value.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Compare",
     iref_modes_cpy},
    {"DEC", "Decrement Memory", "M = M - 1",
     "Decrements a memory location or the accumulator (65C02/65816) by one.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Arithmetic",
     iref_modes_dec},
    {"DEX", "Decrement X Register", "X = X - 1",
     "Decrements the X register by one.", FL('n', 'y', 'n', 'n', 'n', 'n', 'y'),
     NULL, IREF_ALL, "Arithmetic", iref_modes_dex},
    {"DEY", "Decrement Y Register", "Y = Y - 1",
     "Decrements the Y register by one.", FL('n', 'y', 'n', 'n', 'n', 'n', 'y'),
     NULL, IREF_ALL, "Arithmetic", iref_modes_dey},
    {"EOR", "Exclusive OR", "A = A ^ M",
     "Performs a bitwise exclusive OR between the accumulator and a memory "
     "value.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Logical",
     iref_modes_eor},
    {"INC", "Increment Memory", "M = M + 1",
     "Increments a memory location or the accumulator (65C02/65816) by one.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Arithmetic",
     iref_modes_inc},
    {"INX", "Increment X Register", "X = X + 1",
     "Increments the X register by one.", FL('n', 'y', 'n', 'n', 'n', 'n', 'y'),
     NULL, IREF_ALL, "Arithmetic", iref_modes_inx},
    {"INY", "Increment Y Register", "Y = Y + 1",
     "Increments the Y register by one.", FL('n', 'y', 'n', 'n', 'n', 'n', 'y'),
     NULL, IREF_ALL, "Arithmetic", iref_modes_iny},
    {"JML", "Jump Long", "PC = address (24-bit)",
     "Jumps to a 24-bit absolute address, changing the Program Bank Register. "
     "65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Jump",
     iref_modes_jml},
    {"JMP", "Jump", "PC = address",
     "Sets the program counter to the specified address.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Jump",
     iref_modes_jmp},
    {"JSL", "Jump Subroutine Long", "PC = address (24-bit)",
     "Calls a subroutine at a 24-bit absolute address. Pushes a 3-byte return "
     "address. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Jump",
     iref_modes_jsl},
    {"JSR", "Jump to Subroutine", "PC = address",
     "Pushes the return address minus one onto the stack then jumps to the "
     "target address.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Jump",
     iref_modes_jsr},
    {"LDA", "Load Accumulator", "A = M",
     "Loads a value from memory into the accumulator. In 65816 16-bit mode, "
     "loads a 16-bit value.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Load/Store",
     iref_modes_lda},
    {"LDX", "Load X Register", "X = M",
     "Loads a value from memory into the X register.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Load/Store",
     iref_modes_ldx},
    {"LDY", "Load Y Register", "Y = M",
     "Loads a value from memory into the Y register.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Load/Store",
     iref_modes_ldy},
    {"LSR", "Logical Shift Right", "A,C = A/2",
     "Shifts all bits of the accumulator or memory one bit to the right. Bit 7 "
     "is set to 0, bit 0 goes to carry.",
     FL('y', 'y', 'n', 'n', 'n', '0', 'y'), NULL, IREF_ALL, "Shift",
     iref_modes_lsr},
    {"MVN", "Block Move Negative", "[dest++] = [src++]",
     "Copies a block of memory from one bank to another, incrementing "
     "addresses. "
     "Set A=count-1, X=source address, Y=destination address. MVN "
     "srcbank,dstbank.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), "A=0, X and Y advanced after move",
     IREF_65816, "65816", iref_modes_mvn},
    {"MVP", "Block Move Positive", "[dest--] = [src--]",
     "Copies a block of memory from one bank to another, decrementing "
     "addresses. "
     "Use when source and destination overlap and destination is above source.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), "A=0, X and Y advanced after move",
     IREF_65816, "65816", iref_modes_mvp},
    {"NOP", "No Operation", NULL,
     "Causes no change to the processor other than the normal incrementing of "
     "the program counter.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_nop},
    {"ORA", "Logical OR", "A = A | M",
     "Performs a bitwise OR between the accumulator and a memory value.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Logical",
     iref_modes_ora},
    {"PEA", "Push Effective Absolute Address", NULL,
     "Pushes a 16-bit absolute address value onto the stack. Unlike PHA, this "
     "pushes the address itself, not the contents. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Stack",
     iref_modes_pea},
    {"PEI", "Push Effective Indirect Address", NULL,
     "Pushes the 16-bit value pointed to by a direct page address onto the "
     "stack. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Stack",
     iref_modes_pei},
    {"PER", "Push Effective PC-Relative Address", NULL,
     "Pushes a PC-relative effective address onto the stack. Used for "
     "position-independent code. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Stack",
     iref_modes_per},
    {"PHA", "Push Accumulator", "S: A",
     "Pushes the accumulator onto the stack.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Stack",
     iref_modes_pha},
    {"PHB", "Push Data Bank Register", "S: DBR",
     "Pushes the Data Bank Register onto the stack. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Stack",
     iref_modes_phb},
    {"PHD", "Push Direct Page Register", "S: D",
     "Pushes the 16-bit Direct Page register onto the stack. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Stack",
     iref_modes_phd},
    {"PHK", "Push Program Bank Register", "S: PBR",
     "Pushes the Program Bank Register onto the stack. Read-only (no PLK). "
     "65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Stack",
     iref_modes_phk},
    {"PHP", "Push Processor Status", "S: P",
     "Pushes the processor status flags onto the stack.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Stack",
     iref_modes_php},
    {"PHX", "Push X Register", "S: X",
     "Pushes the X register onto the stack. 65C02/65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65C02 | IREF_65816,
     "Stack", iref_modes_phx},
    {"PHY", "Push Y Register", "S: Y",
     "Pushes the Y register onto the stack. 65C02/65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65C02 | IREF_65816,
     "Stack", iref_modes_phy},
    {"PLA", "Pull Accumulator", "A = S",
     "Pulls the top byte from the stack into the accumulator.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Stack",
     iref_modes_pla},
    {"PLB", "Pull Data Bank Register", "DBR = S",
     "Pulls the top byte from the stack into the Data Bank Register. 65816 "
     "only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "Stack",
     iref_modes_plb},
    {"PLD", "Pull Direct Page Register", "D = S",
     "Pulls the top 16-bit word from the stack into the Direct Page register. "
     "65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "Stack",
     iref_modes_pld},
    {"PLP", "Pull Processor Status", "P = S",
     "Pulls the processor status flags from the stack.",
     FL('y', 'y', 'y', 'y', 'y', 'y', 'y'), NULL, IREF_ALL, "Stack",
     iref_modes_plp},
    {"PLX", "Pull X Register", "X = S",
     "Pulls the top byte from the stack into the X register. 65C02/65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65C02 | IREF_65816,
     "Stack", iref_modes_plx},
    {"PLY", "Pull Y Register", "Y = S",
     "Pulls the top byte from the stack into the Y register. 65C02/65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65C02 | IREF_65816,
     "Stack", iref_modes_ply},
    {"REP", "Reset Processor Status Bits", "P = P & ~M",
     "Clears bits in the processor status register. In native mode: REP #$20 "
     "clears M (16-bit A), REP #$10 clears X (16-bit index). 65816 only.",
     FL('y', 'y', 'y', 'y', 'y', 'y', 'y'),
     "Bits specified by immediate byte are cleared", IREF_65816, "65816",
     iref_modes_rep},
    {"ROL", "Rotate Left", "A,C = M*2+C",
     "Shifts all bits left by one. The carry flag fills bit 0, and bit 7 moves "
     "into the carry flag.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Shift",
     iref_modes_rol},
    {"ROR", "Rotate Right", "A,C = M/2+C*127",
     "Shifts all bits right by one. The carry flag fills bit 7, and bit 0 "
     "moves into the carry flag.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Shift",
     iref_modes_ror},
    {"RTI", "Return from Interrupt", NULL,
     "Pulls the processor status and then the program counter from the stack.",
     FL('y', 'y', 'y', 'y', 'y', 'y', 'y'), NULL, IREF_ALL, "Control",
     iref_modes_rti},
    {"RTL", "Return from Subroutine Long", NULL,
     "Pulls the 24-bit return address from the stack and adds one. Counterpart "
     "to JSL. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Jump",
     iref_modes_rtl},
    {"RTS", "Return from Subroutine", NULL,
     "Pulls the program counter (minus one) from the stack. Counterpart to "
     "JSR.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Jump",
     iref_modes_rts},
    {"SBC", "Subtract with Carry", "A,C = A-M-(1-C)",
     "Subtracts a memory value from the accumulator with borrow (inverse of "
     "carry). "
     "If carry is clear, an extra 1 is subtracted.",
     FL('y', 'y', 'n', 'n', 'n', 'y', 'y'), "(+1 cycle if decimal mode set)",
     IREF_ALL, "Arithmetic", iref_modes_sbc},
    {"SEC", "Set Carry Flag", "C = 1", "Sets the carry flag.",
     FL('1', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_sec},
    {"SED", "Set Decimal Flag", "D = 1",
     "Sets the decimal flag, enabling BCD arithmetic.",
     FL('n', 'n', 'n', '1', 'n', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_sed},
    {"SEI", "Set Interrupt Disable", "I = 1",
     "Sets the interrupt disable flag, preventing IRQ interrupts.",
     FL('n', 'n', '1', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Control",
     iref_modes_sei},
    {"SEP", "Set Processor Status Bits", "P = P | M",
     "Sets bits in the processor status register. SEP #$20 sets M (8-bit A), "
     "SEP #$10 sets X (8-bit index). 65816 only.",
     FL('y', 'y', 'y', 'y', 'y', 'y', 'y'),
     "Bits specified by immediate byte are set", IREF_65816, "65816",
     iref_modes_sep},
    {"STA", "Store Accumulator", "M = A",
     "Stores the accumulator value into memory.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Load/Store",
     iref_modes_sta},
    {"STP", "Stop the Clock", NULL,
     "Stops the processor clock until a hardware reset. 65C02/65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65C02 | IREF_65816,
     "Control", iref_modes_stp},
    {"STX", "Store X Register", "M = X", "Stores the X register into memory.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Load/Store",
     iref_modes_stx},
    {"STY", "Store Y Register", "M = Y", "Stores the Y register into memory.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Load/Store",
     iref_modes_sty},
    {"STZ", "Store Zero", "M = 0", "Stores zero into memory. 65C02/65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65C02 | IREF_65816,
     "Load/Store", iref_modes_stz},
    {"TAX", "Transfer A to X", "X = A",
     "Copies the accumulator into the X register.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Transfer",
     iref_modes_tax},
    {"TAY", "Transfer A to Y", "Y = A",
     "Copies the accumulator into the Y register.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Transfer",
     iref_modes_tay},
    {"TCD", "Transfer C to Direct Page", "D = C",
     "Transfers the 16-bit accumulator (C) to the Direct Page register. 65816 "
     "only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "Transfer",
     iref_modes_tcd},
    {"TCS", "Transfer C to Stack Pointer", "S = C",
     "Transfers the 16-bit accumulator (C) to the Stack Pointer. 65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65816, "Transfer",
     iref_modes_tcs},
    {"TDC", "Transfer Direct Page to C", "C = D",
     "Transfers the Direct Page register to the 16-bit accumulator (C). 65816 "
     "only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "Transfer",
     iref_modes_tdc},
    {"TRB", "Test and Reset Bits", "M = M & ~A",
     "Tests bits in memory against the accumulator, then resets (clears) those "
     "bits. 65C02/65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'n'), "Z set if (M & A) == 0",
     IREF_65C02 | IREF_65816, "Bit", iref_modes_trb},
    {"TSB", "Test and Set Bits", "M = M | A",
     "Tests bits in memory against the accumulator, then sets those bits. "
     "65C02/65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'n'), "Z set if (M & A) == 0",
     IREF_65C02 | IREF_65816, "Bit", iref_modes_tsb},
    {"TSC", "Transfer Stack Pointer to C", "C = S",
     "Transfers the Stack Pointer to the 16-bit accumulator (C). 65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "Transfer",
     iref_modes_tsc},
    {"TSX", "Transfer S to X", "X = S",
     "Copies the stack pointer into the X register.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Transfer",
     iref_modes_tsx},
    {"TXA", "Transfer X to A", "A = X",
     "Copies the X register into the accumulator.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Transfer",
     iref_modes_txa},
    {"TXS", "Transfer X to S", "S = X",
     "Copies the X register into the stack pointer.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_ALL, "Transfer",
     iref_modes_txs},
    {"TXY", "Transfer X to Y", "Y = X",
     "Copies the X register into the Y register. 65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "Transfer",
     iref_modes_txy},
    {"TYA", "Transfer Y to A", "A = Y",
     "Copies the Y register into the accumulator.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_ALL, "Transfer",
     iref_modes_tya},
    {"TYX", "Transfer Y to X", "X = Y",
     "Copies the Y register into the X register. 65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "Transfer",
     iref_modes_tyx},
    {"WAI", "Wait for Interrupt", NULL,
     "Halts the CPU until an interrupt occurs, reducing power consumption. "
     "65C02/65816 only.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_65C02 | IREF_65816,
     "Control", iref_modes_wai},
    {"XBA", "Exchange B and A", "A<->B",
     "Swaps the high and low bytes of the 16-bit C register (i.e. swaps the "
     "two 8-bit halves). 65816 only.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_65816, "65816",
     iref_modes_xba},
    {"XCE", "Exchange Carry and Emulation", "C<->E",
     "Exchanges the carry flag with the emulation flag. CLC then XCE switches "
     "to native 16-bit mode. "
     "SEC then XCE returns to emulation (6502-compatible) mode. 65816 only.",
     FL('y', 'n', 'n', 'n', 'n', 'n', 'n'), "E (emulation) flag swapped with C",
     IREF_65816, "65816", iref_modes_xce},
    /* ---- Undocumented/illegal NMOS 6502 opcodes ---- */
    {"LAX", "Load A and X (undocumented)", "A=X=M",
     "Simultaneously loads a value into both A and X. Behaviour on immediate "
     "mode is unstable on some chips. Enable with --undoc.",
     FL('n', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_lax},
    {"SAX", "Store A AND X (undocumented)", "M = A & X",
     "Stores the result of A AND X to memory without affecting flags. Enable "
     "with --undoc.",
     FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_sax},
    {"SLO", "ASL then ORA (undocumented)", "A = A | (M<<1)",
     "Arithmetic shifts memory left then ORs the result into A. Also known as "
     "ASO. Enable with --undoc.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_slo},
    {"RLA", "ROL then AND (undocumented)", "A = A & ROL(M)",
     "Rotates memory left through carry then ANDs the result into A. Enable "
     "with --undoc.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_rla},
    {"SRE", "LSR then EOR (undocumented)", "A = A ^ (M>>1)",
     "Logical shifts memory right then EORs the result into A. Also known as "
     "LSE. Enable with --undoc.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_sre},
    {"RRA", "ROR then ADC (undocumented)", "A = A + ROR(M) + C",
     "Rotates memory right through carry then adds the result to A. Enable "
     "with --undoc.",
     FL('y', 'y', 'n', 'n', 'n', 'y', 'y'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_rra},
    {"DCP", "DEC then CMP (undocumented)", "M=M-1; A-M",
     "Decrements memory then compares the result with A. Also known as DCM. "
     "Enable with --undoc.",
     FL('y', 'y', 'n', 'n', 'n', 'n', 'y'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_dcp},
    {"ISC", "INC then SBC (undocumented)", "M=M+1; A=A-M-!C",
     "Increments memory then subtracts the result from A with borrow. Also "
     "known as INS. Enable with --undoc.",
     FL('y', 'y', 'n', 'n', 'n', 'y', 'y'), NULL, IREF_6502 | IREF_UNDOC,
     "Undocumented", iref_modes_isc},
    {NULL, NULL, NULL, NULL, FL('n', 'n', 'n', 'n', 'n', 'n', 'n'), NULL, 0,
     NULL, NULL}};
#undef FL
#define G_IREF_COUNT ((int)(sizeof(g_iref) / sizeof(g_iref[0]) - 1))

/* ---- Instruction Reference Dialog state ---- */
/* CPU filter indices */
#define IREF_CPU_ALL 0
#define IREF_CPU_6502_LEGAL 1
#define IREF_CPU_6502_UNDOC 2
#define IREF_CPU_6502_ALL 3
#define IREF_CPU_65C02 4
#define IREF_CPU_65816 5

typedef struct {
  AppState *app;
  GtkWidget *dlg;
  GtkWidget *search;
  GtkWidget *cpu_drop;      /* GtkDropDown for CPU filter */
  GtkWidget *am_drop;       /* GtkDropDown for addressing mode filter */
  GtkWidget *list;          /* GtkListBox of mnemonics */
  GtkWidget *detail_box;    /* vertical box holding detail widgets */
  GtkWidget *detail_scroll; /* scrolled window around detail_box */
  int cur_index;            /* index into g_iref of selected instruction */
  int cpu_filter;           /* IREF_CPU_* */
  int am_filter;            /* 0=all, else index into am_filter_names */
} IRefDlg;

static void iref_build_detail(IRefDlg *d, int idx);

/* Filter the list based on search text */
/* Addressing mode filter names — index 0 = "All Modes" */
static const char *iref_am_names[] = {"All Modes",
                                      "Implied",
                                      "Accumulator",
                                      "Immediate",
                                      "Zero page",
                                      "Zero page, X",
                                      "Zero page, Y",
                                      "Zero page indirect",
                                      "(Indirect, X)",
                                      "(Indirect), Y",
                                      "Absolute",
                                      "Absolute, X",
                                      "Absolute, Y",
                                      "Indirect",
                                      "(Absolute, X) indirect",
                                      "Relative",
                                      "Long relative",
                                      "Absolute long",
                                      "Absolute long, X",
                                      "[dp] indirect long",
                                      "[dp], Y indirect long",
                                      "Stack relative",
                                      "(SR, S), Y",
                                      "Block (src,dst banks)",
                                      "[Absolute] indirect long",
                                      NULL};
#define IREF_AM_COUNT 24 /* entries excluding NULL sentinel */

static void iref_invalidate_filter(IRefDlg *d);

static gboolean iref_filter_row(GtkListBoxRow *row, gpointer user_data) {
  IRefDlg *d = (IRefDlg *)user_data;
  if (!d)
    return TRUE;

  int iref_idx =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "iref-index"));
  const iref_t *ir = &g_iref[iref_idx];

  /* --- Text filter --- */
  const char *query = gtk_editable_get_text(GTK_EDITABLE(d->search));
  if (query && query[0] != '\0') {
    char q[16] = {0};
    int qi = 0;
    for (const char *p = query; *p && qi < 15; p++, qi++)
      q[qi] = (char)g_ascii_toupper(*p);
    char m[16] = {0};
    int mi = 0;
    for (const char *p = ir->mnem; *p && mi < 15; p++, mi++)
      m[mi] = (char)g_ascii_toupper(*p);
    if (!g_strstr_len(m, -1, q))
      return FALSE;
  }

  /* --- CPU filter --- */
  switch (d->cpu_filter) {
  case IREF_CPU_6502_LEGAL:
    if (!(ir->cpu_mask & IREF_6502))
      return FALSE;
    if (ir->cpu_mask & IREF_UNDOC)
      return FALSE; /* exclude undoc */
    break;
  case IREF_CPU_6502_UNDOC:
    if (!(ir->cpu_mask & IREF_UNDOC))
      return FALSE;
    break;
  case IREF_CPU_6502_ALL:
    if (!(ir->cpu_mask & IREF_6502))
      return FALSE;
    break;
  case IREF_CPU_65C02:
    if (!(ir->cpu_mask & IREF_65C02))
      return FALSE;
    break;
  case IREF_CPU_65816:
    if (!(ir->cpu_mask & IREF_65816))
      return FALSE;
    break;
  case IREF_CPU_ALL:
  default:
    break;
  }

  /* --- Address mode filter --- */
  if (d->am_filter > 0 && d->am_filter < IREF_AM_COUNT + 1) {
    const char *want_am = iref_am_names[d->am_filter]; /* offset by 1 (0=All) */
    gboolean found = FALSE;
    if (ir->modes) {
      for (const iref_mode_t *m = ir->modes; m->mode; m++) {
        if (strcmp(m->mode, want_am) == 0) {
          found = TRUE;
          break;
        }
      }
    }
    if (!found)
      return FALSE;
  }

  return TRUE;
}

static void iref_invalidate_filter(IRefDlg *d) {
  gtk_list_box_set_filter_func(GTK_LIST_BOX(d->list), iref_filter_row, d, NULL);
  gtk_list_box_invalidate_filter(GTK_LIST_BOX(d->list));
  /* After filter changes, select first visible row */
  for (int i = 0; i < G_IREF_COUNT; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(d->list), i);
    if (r && gtk_widget_get_visible(GTK_WIDGET(r))) {
      gtk_list_box_select_row(GTK_LIST_BOX(d->list), r);
      int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "iref-index"));
      iref_build_detail(d, idx);
      break;
    }
  }
}

static void iref_search_changed(GtkSearchEntry *se, gpointer user_data) {
  (void)se;
  iref_invalidate_filter((IRefDlg *)user_data);
}

static void iref_cpu_changed(GtkDropDown *dd, GParamSpec *ps,
                             gpointer user_data) {
  (void)ps;
  IRefDlg *d = (IRefDlg *)user_data;
  d->cpu_filter = (int)gtk_drop_down_get_selected(dd);
  iref_invalidate_filter(d);
}

/* Callback: addressing-mode filter changed in the instruction reference dialog.
 */
static void iref_am_changed(GtkDropDown *dd, GParamSpec *ps,
                            gpointer user_data) {
  (void)ps;
  IRefDlg *d = (IRefDlg *)user_data;
  d->am_filter = (int)gtk_drop_down_get_selected(dd);
  iref_invalidate_filter(d);
}

/* Callback: a row selected in the instruction reference list; show details. */
static void iref_row_selected(GtkListBox *lb, GtkListBoxRow *row,
                              gpointer user_data) {
  (void)lb;
  if (!row)
    return;
  IRefDlg *d = (IRefDlg *)user_data;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "iref-index"));
  iref_build_detail(d, idx);
}

/* Add a bold label */
static GtkWidget *iref_bold_label(const char *text) {
  GtkWidget *l = gtk_label_new(NULL);
  char *mu = g_markup_printf_escaped("<b>%s</b>", text);
  gtk_label_set_markup(GTK_LABEL(l), mu);
  g_free(mu);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(l), TRUE);
  return l;
}

/* Add a monospace label */
static GtkWidget *iref_mono_label(const char *text) {
  GtkWidget *l = gtk_label_new(NULL);
  char *mu = g_markup_printf_escaped("<tt>%s</tt>", text);
  gtk_label_set_markup(GTK_LABEL(l), mu);
  g_free(mu);
  gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
  return l;
}

static void iref_add_separator(GtkWidget *box) {
  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_top(sep, 6);
  gtk_widget_set_margin_bottom(sep, 6);
  gtk_box_append(GTK_BOX(box), sep);
}

static void iref_build_detail(IRefDlg *d, int idx) {
  d->cur_index = idx;
  const iref_t *ir = &g_iref[idx];
  /* Remove all existing children */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(d->detail_box)) != NULL)
    gtk_box_remove(GTK_BOX(d->detail_box), child);

  /* ---- Header: mnemonic + full name ---- */
  {
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "%s — %s", ir->mnem, ir->full_name);
    GtkWidget *l = gtk_label_new(NULL);
    char *mu =
        g_markup_printf_escaped("<span size='large'><b>%s</b></span>", hdr);
    gtk_label_set_markup(GTK_LABEL(l), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(d->detail_box), l);
  }

  /* Operation string */
  if (ir->operation) {
    GtkWidget *l = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped("<i>%s</i>", ir->operation);
    gtk_label_set_markup(GTK_LABEL(l), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_box_append(GTK_BOX(d->detail_box), l);
  }

  iref_add_separator(d->detail_box);

  /* Description */
  {
    GtkWidget *l = gtk_label_new(ir->desc);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(d->detail_box), l);
  }

  /* Supported CPUs */
  {
    iref_add_separator(d->detail_box);
    gtk_box_append(GTK_BOX(d->detail_box), iref_bold_label("Supported on:"));
    char cpubuf[64] = "";
    if (ir->cpu_mask & IREF_6502) {
      if (cpubuf[0])
        strcat(cpubuf, ",  ");
      strcat(cpubuf, "6502");
    }
    if (ir->cpu_mask & IREF_65C02) {
      if (cpubuf[0])
        strcat(cpubuf, ",  ");
      strcat(cpubuf, "65C02");
    }
    if (ir->cpu_mask & IREF_65816) {
      if (cpubuf[0])
        strcat(cpubuf, ",  ");
      strcat(cpubuf, "65816");
    }
    gtk_box_append(GTK_BOX(d->detail_box), gtk_label_new(cpubuf));
    gtk_label_set_xalign(GTK_LABEL(gtk_widget_get_last_child(d->detail_box)),
                         0.0f);
  }

  /* Processor status flags */
  {
    iref_add_separator(d->detail_box);
    gtk_box_append(GTK_BOX(d->detail_box),
                   iref_bold_label("Processor status after use:"));
    static const char *flag_names[] = {
        "C (carry)", "Z (zero)",     "I (interrupt disable)", "D (decimal)",
        "B (break)", "V (overflow)", "N (negative)"};
    for (int fi = 0; fi < 7; fi++) {
      char ch = ir->flags[fi];
      const char *effect = (ch == 'y')   ? "modified"
                           : (ch == '0') ? "cleared"
                           : (ch == '1') ? "set"
                                         : "not affected";
      char fbuf[128];
      snprintf(fbuf, sizeof(fbuf), "  %-28s : %s", flag_names[fi], effect);
      gtk_box_append(GTK_BOX(d->detail_box), iref_mono_label(fbuf));
    }
    if (ir->flags_note) {
      GtkWidget *l = gtk_label_new(ir->flags_note);
      gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
      gtk_widget_set_margin_top(l, 4);
      gtk_box_append(GTK_BOX(d->detail_box), l);
    }
  }

  /* Addressing modes table */
  if (ir->modes && ir->modes[0].mode) {
    iref_add_separator(d->detail_box);
    gtk_box_append(GTK_BOX(d->detail_box),
                   iref_bold_label("Addressing modes:"));

    /* Grid: Mode | Bytes | Cycles | Opcode | CPU */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_box_append(GTK_BOX(d->detail_box), grid);

    /* Header row */
    const char *hdrs[] = {"Mode", "Bytes", "Cycles", "Opcode", "CPU"};
    for (int c = 0; c < 5; c++) {
      GtkWidget *lh = gtk_label_new(NULL);
      char *mu = g_markup_printf_escaped("<b><u>%s</u></b>", hdrs[c]);
      gtk_label_set_markup(GTK_LABEL(lh), mu);
      g_free(mu);
      gtk_label_set_xalign(GTK_LABEL(lh), 0.0f);
      gtk_grid_attach(GTK_GRID(grid), lh, c, 0, 1, 1);
    }

    int row = 1;
    for (const iref_mode_t *m = ir->modes; m->mode; m++, row++) {
      /* Mode name */
      GtkWidget *lm = gtk_label_new(m->mode);
      gtk_label_set_xalign(GTK_LABEL(lm), 0.0f);
      gtk_grid_attach(GTK_GRID(grid), lm, 0, row, 1, 1);
      /* Bytes */
      char bb[8];
      snprintf(bb, sizeof(bb), "%d", m->bytes);
      GtkWidget *lb = gtk_label_new(bb);
      gtk_label_set_xalign(GTK_LABEL(lb), 0.0f);
      gtk_grid_attach(GTK_GRID(grid), lb, 1, row, 1, 1);
      /* Cycles */
      GtkWidget *lc = gtk_label_new(m->cycles);
      gtk_label_set_xalign(GTK_LABEL(lc), 0.0f);
      gtk_grid_attach(GTK_GRID(grid), lc, 2, row, 1, 1);
      /* Opcode */
      GtkWidget *lo = iref_mono_label(m->opcode);
      gtk_label_set_xalign(GTK_LABEL(lo), 0.0f);
      gtk_grid_attach(GTK_GRID(grid), lo, 3, row, 1, 1);
      /* CPU badges */
      char cpub[32] = "";
      if (m->cpu & IREF_6502)
        strcat(cpub, "6502 ");
      if (m->cpu & IREF_65C02)
        strcat(cpub, "65C02 ");
      if (m->cpu & IREF_65816)
        strcat(cpub, "65816");
      GtkWidget *lp = gtk_label_new(cpub);
      gtk_label_set_xalign(GTK_LABEL(lp), 0.0f);
      gtk_grid_attach(GTK_GRID(grid), lp, 4, row, 1, 1);
    }

    /* Cycles footnote */
    GtkWidget *fn =
        gtk_label_new("+ = add 1 cycle if page boundary crossed;  ++ = add 1 "
                      "if branch taken, 2 if page crossed;  ~ = variable");
    gtk_label_set_xalign(GTK_LABEL(fn), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(fn), TRUE);
    gtk_widget_set_margin_top(fn, 4);
    GtkWidget *fnl = gtk_label_new(NULL);
    gtk_label_set_markup(
        GTK_LABEL(fnl),
        "<small><i>+ = add 1 if page crossed;  ++ = +1 if branch taken, +2 if "
        "page crossed;  ~ = variable</i></small>");
    gtk_label_set_xalign(GTK_LABEL(fnl), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(fnl), TRUE);
    gtk_box_append(GTK_BOX(d->detail_box), fnl);
    (void)fn;
  }
}

static void iref_dialog_free(gpointer data) { g_free(data); }

/* =========================================================================
 * Assembler Directive Reference
 * ========================================================================= */

typedef struct {
  const char *name;    /* primary name e.g. ".org" */
  const char *aliases; /* comma-separated aliases, or NULL */
  const char *group;   /* category */
  const char *syntax;  /* usage syntax */
  const char *desc;    /* description */
  const char *example; /* short example, or NULL */
} dref_t;

static const dref_t g_dref[] = {

    /* ---- Origin / CPU ---- */
    {".org", "*= / .pc=", "Origin & CPU", ".org $addr",
     "Sets the program counter to the specified address. Also accepted as *= "
     "or .pc=.",
     ".org $8000"},
    {".cpu", NULL, "Origin & CPU", ".cpu type",
     "Selects the target CPU. Must appear before the first instruction.\n"
     "Accepted values: 6502, 6502u (with undocumented opcodes), 65c02, 65816 "
     "(also 65c816).\n"
     "Sets the built-in symbol 'cpu' for conditional assembly (6502=6502, "
     "65c02=6503, 65816=65816).",
     ".cpu 65816"},

    /* ---- Data ---- */
    {".byte", "equb / eqb / db", "Data", ".byte expr [, expr ...]",
     "Defines one or more bytes. Accepts numeric expressions, character "
     "literals ('A'), "
     "and string literals (\"hello\" — each character becomes one byte).",
     ".byte $01, $02, \"hi\", 'A'"},
    {".word", "equw / eqw / dq", "Data", ".word expr [, expr ...]",
     "Defines one or more 16-bit little-endian words.", ".word $1234, label"},
    {".dword", "equd / eqd / dd", "Data", ".dword expr [, expr ...]",
     "Defines one or more 32-bit little-endian doublewords.",
     ".dword $12345678"},
    {".res", "res", "Data", ".res count [, fill]",
     "Reserves count bytes, filled with fill (default 0). count must be a "
     "constant.",
     ".res 256, $FF"},
    {".align", "align", "Data", ".align boundary",
     "Advances the program counter to the next multiple of boundary. Fills "
     "skipped bytes with zero.",
     ".align 256"},
    {".incbin", "incbin", "Data", ".incbin \"file\" [, offset [, length]]",
     "Includes raw binary data from a file. Optional offset (default 0) and "
     "length (default: rest of file).",
     ".incbin \"sprite.bin\", 0, 64"},

    /* ---- Symbols & Constants ---- */
    {"NAME = expr", "NAME EQU expr", "Symbols", "NAME = expr",
     "Defines a constant symbol. The symbol is visible throughout the file. "
     "EQU is accepted as an alternative. The = form requires a label in the "
     "first column.\n\n"
     "The current program counter can be captured as a symbol using * or .:\n"
     "  block_start = *   ; save current PC\n"
     "  block_size  = * - block_start",
     "SCREEN = $0400\nFONT EQU $C000\nblock_start = *"},
    {".def", "#def / #define", "Symbols", ".def NAME [value]",
     "Preprocessor constant definition. Unlike NAME=expr, .def is a "
     "text-substitution "
     "macro expanded before assembly. Value defaults to 1 if omitted. "
     "Duplicate .def of the same name is an error.",
     ".def NTSC 1\n.def COLS 40"},

    /* ---- Conditionals ---- */
    {".if", NULL, "Conditionals",
     ".if expression\n  ...\n[.else]\n  ...\n.endif",
     "Assembles the block only if expression evaluates to non-zero. "
     "Supports .else and .elif/.elseif chains. Can be nested.",
     ".if cpu == 65816\n  XCE\n.endif"},
    {".ifdef", NULL, "Conditionals", ".ifdef NAME\n  ...\n.endif",
     "Assembles the block only if NAME has been defined (via .def or NAME=).",
     ".ifdef DEBUG\n  .byte $EA\n.endif"},
    {".ifndef", NULL, "Conditionals", ".ifndef NAME\n  ...\n.endif",
     "Assembles the block only if NAME has NOT been defined.",
     ".ifndef RELEASE\n  .warning \"debug build\"\n.endif"},
    {".else", NULL, "Conditionals", ".else",
     "Introduces the alternative branch of a .if/.ifdef/.ifndef block.", NULL},
    {".elif", ".elseif", "Conditionals", ".elif expression",
     "Introduces a chained conditional branch. Equivalent to .else followed by "
     ".if.",
     ".if cpu == 6502\n  ...\n.elif cpu == 6503\n  ...\n.endif"},
    {".endif", NULL, "Conditionals", ".endif",
     "Closes a .if / .ifdef / .ifndef block.", NULL},

    /* ---- Procedures & Scopes ---- */
    {".proc", "proc", "Procedures & Scopes", ".proc name\n  ...\n.endproc",
     "Begins a named procedure block. Labels defined inside are local to the "
     "procedure "
     "and accessed from outside as name::label. The procedure name itself is "
     "also a label "
     "pointing to the first instruction.",
     ".proc init\n  LDA #0\n  STA $00\n.endproc"},
    {".endproc", "endproc", "Procedures & Scopes", ".endproc",
     "Closes a .proc block.", NULL},
    {".scope", NULL, "Procedures & Scopes", ".scope [name]\n  ...\n.endscope",
     "Like .proc but does not imply an entry point. Used to group labels "
     "without "
     "creating a callable routine. Name is optional.",
     ".scope data_tables\n  table: .byte 1,2,3\n.endscope"},
    {".endscope", NULL, "Procedures & Scopes", ".endscope",
     "Closes a .scope block.", NULL},

    /* ---- Local Labels ---- */
    {"@label", NULL, "Local Labels", "@labelname:",
     "Defines a local label scoped to the nearest enclosing .proc or .scope. "
     "Can be referenced by name within the same scope without qualification.",
     ".proc loop\n@again:\n  DEX\n  BNE @again\n.endproc"},
    {".label (anonymous)", ": / @: / .", "Local Labels", ":  or  @:  or  .",
     "Anonymous label. Forward references use :+ or .+ (next anonymous label), "
     "back-references use :- or .- (previous anonymous label). "
     "Multiple + or - signs look further ahead/back.",
     ":\n  LDA #0\n  BEQ :+\n  INX\n:"},

    /* ---- Macros ---- */
    {".macro", "macro", "Macros",
     ".macro NAME [arg1, arg2, ...]\n  ...\n.endmacro",
     "Defines a macro. Arguments are referenced by name or '\\' followed by "
     "name in the "
     "body. "
     "Macros are expanded inline at the point of invocation. "
     "Recursive macros are not supported.",
     ".macro STORE16 val, addr\n  LDA #<val\n  STA addr\n  LDA #>val\n  "
     "STA "
     "addr+1\n.endmacro"},
    {".endmacro", "endmacro / endm", "Macros", ".endmacro",
     "Closes a .macro definition.", NULL},

    /* ---- Repeat ---- */
    {".repeat", NULL, "Repeat", ".repeat count\n  ...\n.endrepeat",
     "Repeats the enclosed block count times. count must be a constant "
     "(0..65536). "
     "Useful for emitting repeated data patterns or NOP sleds.",
     ".repeat 8\n  NOP\n.endrepeat"},
    {".endrepeat", NULL, "Repeat", ".endrepeat", "Closes a .repeat block.",
     NULL},

    /* ---- Include ---- */
    {".include", "include", "Include", ".include \"filename\"",
     "Includes another source file, relative to the directory of the current "
     "file. "
     "Included files are processed as if their contents were inserted at the "
     "point of the directive.",
     ".include \"macros.asm\""},
    {".includeroot", "#includeroot", "Include", ".includeroot \"directory/\"",
     "Sets a base directory that subsequent .include directives search when "
     "the include "
     "path contains no directory separator. Only permitted in the top-level "
     "(root) file. "
     "Unlike .include, this sets a search path — it does not include a file "
     "itself.",
     "; In main.asm (root file only):\n"
     ".includeroot \"src/\"\n"
     ".include \"macros.asm\"  ; resolves to src/macros.asm"},
    {".pragma once", "#pragma once", "Include",
     ".pragma once",
     "Include guard. Place at the top of any file that may be included from "
     "multiple places. The preprocessor ensures the file's content is "
     "assembled only once per build, regardless of how many .include "
     "directives reference it.\n"
     "This is preferred over .ifndef/.def/.endif guards because it has no "
     "visual side-effects (the editor does not grey out the file contents).",
     "; definitions.asm\n.pragma once\n\nMY_CONST = $1234"},

    /* ---- Guard Directives ---- */
    {".halt", "halt", "Guards",
     ".halt start[-end] [\"message\"]",
     "Defines an address range that triggers an assembler ERROR if any byte "
     "is written there. Assembly halts at the first hit.\n"
     "Use to protect ROM regions, reserved areas, or hardware registers from "
     "accidental writes.\n\n"
     "Message format specifiers:\n"
     "  %s  4-digit hex of the actual write address\n"
     "  %r  range as XXXX-YYYY\n\n"
     "Guards only fire during an explicit check (F5) or assemble (F9), not "
     "during live background checking. Each range emits at most 5 diagnostics "
     "before suppressing further hits with '...'.",
     ".halt $C000-$CFFF \"write to OS ROM at $%s\""},
    {".alert", "alert", "Guards",
     ".alert start[-end] [\"message\"]",
     "Like .halt but emits a WARNING instead of an error — assembly continues.\n"
     "Useful for flagging writes to areas that are valid but suspicious.\n\n"
     "Message format specifiers:\n"
     "  %s  4-digit hex of the actual write address\n"
     "  %r  range as XXXX-YYYY",
     ".alert $0200-$02FF \"caution: page 2 write at $%s\""},
    {".unhalt", "unhalt", "Guards",
     ".unhalt start",
     "Removes a .halt or .alert guard whose start address matches start. "
     "Either keyword removes either type of guard.",
     ".unhalt $C000"},
    {".unalert", "unalert", "Guards",
     ".unalert start",
     "Removes a .halt or .alert guard whose start address matches start. "
     "Either keyword removes either type of guard.",
     ".unalert $0200"},

    /* ---- Assertions & Diagnostics ---- */
    {".assert", "assert", "Diagnostics",
     ".assert condition, severity, \"message\"",
     "Evaluates condition. If false (zero), emits a diagnostic. "
     "Severity is one of: error, warning, ldwarning.",
     ".assert * < $C000, error, \"code overflowed into ROM area\""},
    {".error", "error", "Diagnostics", ".error \"message\"",
     "Always emits an assembler error with the given message, halting "
     "assembly.",
     ".error \"This CPU is not supported\""},
    {".warning", "warning", "Diagnostics", ".warning \"message\"",
     "Always emits an assembler warning with the given message.",
     ".warning \"deprecated feature used\""},

    /* ---- Listing ---- */
    {"opt", ".opt", "Listing", "opt [no]list  /  opt [no]h  /  opt [no]p",
     "Controls assembler listing output. 'list'/'nolist' enables/disables the "
     "listing. "
     "'h' toggles byte-level output in hex. 'p' toggles page breaks. "
     "BBC BASIC compatible.",
     "opt list\nopt noh"},

    /* ---- 65816 Width ---- */
    {".longa", "longa", "65816", ".longa on|off",
     "Tells the assembler whether the accumulator is currently 16-bit (on) or "
     "8-bit (off). "
     "Used when REP/SEP tracking is insufficient, e.g. when jumping into the "
     "middle of code. "
     "65816 only.",
     "REP #$20\n.longa on\nLDA #$1234"},
    {".longi", "longi", "65816", ".longi on|off",
     "Tells the assembler whether the index registers are currently 16-bit "
     "(on) or 8-bit (off). "
     "65816 only.",
     "REP #$10\n.longi on\nLDX #$0100"},

    {NULL, NULL, NULL, NULL, NULL, NULL}};
#define G_DREF_COUNT ((int)(sizeof(g_dref) / sizeof(g_dref[0]) - 1))

/* ---- Directive Reference Dialog ---- */
typedef struct {
  AppState *app;
  GtkWidget *dlg;
  GtkWidget *search;
  GtkWidget *group_drop;
  GtkWidget *list;
  GtkWidget *detail_box;
  GtkWidget *detail_scroll;
  int group_filter; /* 0=all */
} DRefDlg;

static const char *dref_groups[] = {
    "All Groups",          "Origin & CPU", "Data",    "Symbols", "Conditionals",
    "Procedures & Scopes", "Local Labels", "Macros",  "Repeat",  "Include",
    "Guards",              "Diagnostics",  "Listing", "65816",   NULL};

static gboolean dref_filter_row(GtkListBoxRow *row, gpointer user_data) {
  DRefDlg *d = (DRefDlg *)user_data;
  if (!d)
    return TRUE;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "dref-index"));
  const dref_t *dr = &g_dref[idx];

  /* text filter */
  const char *query = gtk_editable_get_text(GTK_EDITABLE(d->search));
  if (query && query[0]) {
    char q[32] = {0};
    int qi = 0;
    for (const char *p = query; *p && qi < 31; p++, qi++)
      q[qi] = (char)g_ascii_toupper(*p);
    char n[32] = {0};
    int ni = 0;
    for (const char *p = dr->name; *p && ni < 31; p++, ni++)
      n[ni] = (char)g_ascii_toupper(*p);
    if (!g_strstr_len(n, -1, q))
      return FALSE;
  }
  /* group filter */
  if (d->group_filter > 0) {
    const char *want = dref_groups[d->group_filter];
    if (!dr->group || strcmp(dr->group, want) != 0)
      return FALSE;
  }
  return TRUE;
}

static void dref_build_detail(DRefDlg *d, int idx) {
  const dref_t *dr = &g_dref[idx];
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(d->detail_box)) != NULL)
    gtk_box_remove(GTK_BOX(d->detail_box), child);

  /* Header */
  {
    GtkWidget *l = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped(
        "<span size='large'><b><tt>%s</tt></b></span>", dr->name);
    gtk_label_set_markup(GTK_LABEL(l), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_box_append(GTK_BOX(d->detail_box), l);
  }
  /* Aliases */
  if (dr->aliases) {
    GtkWidget *l = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped("<i>Also: %s</i>", dr->aliases);
    gtk_label_set_markup(GTK_LABEL(l), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_box_append(GTK_BOX(d->detail_box), l);
  }
  /* Group badge */
  if (dr->group) {
    GtkWidget *l = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped("<small><b>%s</b></small>", dr->group);
    gtk_label_set_markup(GTK_LABEL(l), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_widget_set_margin_top(l, 4);
    gtk_box_append(GTK_BOX(d->detail_box), l);
  }
  /* Separator */
  {
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 6);
    gtk_widget_set_margin_bottom(sep, 6);
    gtk_box_append(GTK_BOX(d->detail_box), sep);
  }
  /* Syntax */
  {
    GtkWidget *lh = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lh), "<b>Syntax:</b>");
    gtk_label_set_xalign(GTK_LABEL(lh), 0.0f);
    gtk_box_append(GTK_BOX(d->detail_box), lh);
    GtkWidget *ls = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped("<tt>%s</tt>", dr->syntax);
    gtk_label_set_markup(GTK_LABEL(ls), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(ls), 0.0f);
    gtk_widget_set_margin_start(ls, 12);
    gtk_widget_set_margin_bottom(ls, 6);
    gtk_box_append(GTK_BOX(d->detail_box), ls);
  }
  /* Description */
  {
    GtkWidget *l = gtk_label_new(dr->desc);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(d->detail_box), l);
  }
  /* Example */
  if (dr->example) {
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_top(sep, 6);
    gtk_widget_set_margin_bottom(sep, 6);
    gtk_box_append(GTK_BOX(d->detail_box), sep);
    GtkWidget *lh = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lh), "<b>Example:</b>");
    gtk_label_set_xalign(GTK_LABEL(lh), 0.0f);
    gtk_box_append(GTK_BOX(d->detail_box), lh);
    GtkWidget *le = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped("<tt>%s</tt>", dr->example);
    gtk_label_set_markup(GTK_LABEL(le), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(le), 0.0f);
    gtk_widget_set_margin_start(le, 12);
    gtk_box_append(GTK_BOX(d->detail_box), le);
  }
}

static void dref_row_selected(GtkListBox *lb, GtkListBoxRow *row,
                              gpointer user_data) {
  (void)lb;
  if (!row)
    return;
  DRefDlg *d = (DRefDlg *)user_data;
  int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "dref-index"));
  dref_build_detail(d, idx);
}

/* Invalidate (refilter) the definition-reference list. */
static void dref_invalidate(DRefDlg *d) {
  gtk_list_box_set_filter_func(GTK_LIST_BOX(d->list), dref_filter_row, d, NULL);
  gtk_list_box_invalidate_filter(GTK_LIST_BOX(d->list));
  for (int i = 0; i < G_DREF_COUNT; i++) {
    GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(d->list), i);
    if (r && gtk_widget_get_visible(GTK_WIDGET(r))) {
      gtk_list_box_select_row(GTK_LIST_BOX(d->list), r);
      dref_build_detail(
          d, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(r), "dref-index")));
      break;
    }
  }
}

/* Callback: search text changed in definition-reference dialog. */
static void dref_search_changed(GtkSearchEntry *se, gpointer user_data) {
  (void)se;
  dref_invalidate((DRefDlg *)user_data);
}

static void dref_group_changed(GtkDropDown *dd, GParamSpec *ps,
                               gpointer user_data) {
  (void)ps;
  DRefDlg *d = (DRefDlg *)user_data;
  d->group_filter = (int)gtk_drop_down_get_selected(dd);
  dref_invalidate(d);
}

/* GDestroyNotify: free the DRefDlg state when the dialog is destroyed. */
static void dref_dialog_free(gpointer data) { g_free(data); }

/* ---- Copy-to-clipboard helper for reference dialogs ---- */
static void iref_copy_detail_to_clipboard(GtkWidget *detail_box,
                                          GtkWidget *dlg) {
  GString *out = g_string_new(NULL);
  GtkWidget *child = gtk_widget_get_first_child(detail_box);
  while (child) {
    if (GTK_IS_LABEL(child)) {
      const char *text = gtk_label_get_text(GTK_LABEL(child));
      if (text && *text) {
        g_string_append(out, text);
        g_string_append_c(out, '\n');
      }
    } else if (GTK_IS_SEPARATOR(child)) {
      g_string_append(out, "---\n");
    } else if (GTK_IS_GRID(child)) {
      GtkWidget *gc = gtk_widget_get_first_child(child);
      int col = 0, prev_row = -1;
      while (gc) {
        if (GTK_IS_LABEL(gc)) {
          int row = 0;
          gtk_grid_query_child(GTK_GRID(child), gc, NULL, &row, NULL, NULL);
          if (row != prev_row && prev_row >= 0) {
            g_string_append_c(out, '\n');
            col = 0;
          }
          if (col > 0)
            g_string_append(out, "  ");
          const char *t = gtk_label_get_text(GTK_LABEL(gc));
          if (t && *t)
            g_string_append(out, t);
          prev_row = row;
          col++;
        }
        gc = gtk_widget_get_next_sibling(gc);
      }
      g_string_append_c(out, '\n');
    }
    child = gtk_widget_get_next_sibling(child);
  }
  GdkClipboard *cb = gtk_widget_get_clipboard(dlg);
  gdk_clipboard_set_text(cb, out->str);
  g_string_free(out, TRUE);
}

/* Callback: copy selected opcode row to clipboard in instruction reference. */
static void iref_copy_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  IRefDlg *d = (IRefDlg *)user_data;
  iref_copy_detail_to_clipboard(d->detail_box, d->dlg);
}

static void dref_copy_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  DRefDlg *d = (DRefDlg *)user_data;
  iref_copy_detail_to_clipboard(d->detail_box, d->dlg);
}

/* ---------------------------------------------------------------------------
 * Bookmark helpers (GTK)
 * ------------------------------------------------------------------------- */

static void gtk_bookmark_redraw(AppState *s) {
  if (!s || !s->view)
    return;
  /* The gutter renderer is itself a GtkWidget. Calling queue_draw on it
     directly is the reliable way to repaint just the gutter without
     disturbing the text buffer. */
  if (s->gutter_renderer)
    gtk_widget_queue_draw(GTK_WIDGET(s->gutter_renderer));
  /* Also notify the gutter container */
  GtkSourceGutter *gutter = gtk_source_view_get_gutter(GTK_SOURCE_VIEW(s->view),
                                                       GTK_TEXT_WINDOW_LEFT);
  if (gutter)
    gtk_widget_queue_draw(GTK_WIDGET(gutter));
}

static int gtk_bookmark_find(AppState *s, int line) {
  for (int i = 0; i < s->bookmark_count; i++)
    if (s->bookmarks[i] == line)
      return i;
  return -1;
}

static void gtk_bookmark_toggle(AppState *s, int line) {
  int idx = gtk_bookmark_find(s, line);
  if (idx >= 0) {
    for (int i = idx; i < s->bookmark_count - 1; i++)
      s->bookmarks[i] = s->bookmarks[i + 1];
    s->bookmark_count--;
  } else if (s->bookmark_count < GTK_MAX_BOOKMARKS) {
    int ins = s->bookmark_count;
    for (int i = 0; i < s->bookmark_count; i++) {
      if (s->bookmarks[i] > line) {
        ins = i;
        break;
      }
    }
    for (int i = s->bookmark_count; i > ins; i--)
      s->bookmarks[i] = s->bookmarks[i - 1];
    s->bookmarks[ins] = line;
    s->bookmark_count++;
  }
  gtk_bookmark_redraw(s);
}

/* Return the line number of the next bookmark after cur_line (wrapping),
 * or -1 if there are no bookmarks. */
static int gtk_bookmark_next(AppState *s, int cur_line) {
  if (s->bookmark_count == 0)
    return -1;
  for (int i = 0; i < s->bookmark_count; i++)
    if (s->bookmarks[i] > cur_line)
      return s->bookmarks[i];
  return s->bookmarks[0]; /* wrap */
}

static int gtk_bookmark_prev(AppState *s, int cur_line) {
  if (s->bookmark_count == 0)
    return -1;
  for (int i = s->bookmark_count - 1; i >= 0; i--)
    if (s->bookmarks[i] < cur_line)
      return s->bookmarks[i];
  return s->bookmarks[s->bookmark_count - 1]; /* wrap */
}

static void gtk_bookmark_jump(AppState *s, int line) {
  if (!s || !s->view || line < 0)
    return;
  GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->view));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_line(tb, &it, line);
  gtk_text_buffer_place_cursor(tb, &it);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &it, 0.1, TRUE, 0.0,
                               0.5);
  gtk_widget_grab_focus(GTK_WIDGET(s->view));
}

/* Persist bookmarks to ~/.config/6502caEd/bookmarks.ini */
static void gtk_bookmarks_path(char *out, size_t cap) {
  const char *home = g_get_home_dir();
  if (!home)
    home = ".";
  snprintf(out, cap, "%s/.config/6502caEd/bookmarks.ini", home);
}

static void gtk_bookmarks_save(AppState *s, const char *file_path) {
  if (!file_path || !file_path[0])
    return;
  char bp[PATH_MAX];
  gtk_bookmarks_path(bp, sizeof(bp));

  /* Read existing entries for other files */
  GPtrArray *kept = g_ptr_array_new_with_free_func(g_free);
  FILE *rf = fopen(bp, "r");
  if (rf) {
    char buf[1280];
    int skip = 0;
    while (fgets(buf, sizeof(buf), rf)) {
      int bl = (int)strlen(buf);
      while (bl > 0 && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r'))
        buf[--bl] = 0;
      if (strncmp(buf, "path=", 5) == 0) {
        skip = (strcmp(buf + 5, file_path) == 0) ? 1 : 0;
        if (!skip)
          g_ptr_array_add(kept, g_strdup(buf));
      } else if (strncmp(buf, "line=", 5) == 0) {
        if (!skip)
          g_ptr_array_add(kept, g_strdup(buf));
      }
    }
    fclose(rf);
  }

  FILE *wf = fopen(bp, "w");
  if (!wf) {
    g_ptr_array_free(kept, TRUE);
    return;
  }
  for (guint i = 0; i < kept->len; i++)
    fprintf(wf, "%s\n", (char *)kept->pdata[i]);
  if (s->bookmark_count > 0) {
    fprintf(wf, "path=%s\n", file_path);
    for (int i = 0; i < s->bookmark_count; i++)
      fprintf(wf, "line=%d\n", s->bookmarks[i] + 1); /* 1-based */
  }
  fclose(wf);
  g_ptr_array_free(kept, TRUE);
}

static void gtk_bookmarks_load(AppState *s, const char *file_path) {
  if (!file_path || !file_path[0])
    return;
  s->bookmark_count = 0;
  char bp[PATH_MAX];
  gtk_bookmarks_path(bp, sizeof(bp));
  FILE *f = fopen(bp, "r");
  if (!f)
    return;
  char buf[1280];
  int in_file = 0;
  while (fgets(buf, sizeof(buf), f)) {
    int bl = (int)strlen(buf);
    while (bl > 0 && (buf[bl - 1] == '\n' || buf[bl - 1] == '\r'))
      buf[--bl] = 0;
    if (strncmp(buf, "path=", 5) == 0)
      in_file = (strcmp(buf + 5, file_path) == 0) ? 1 : 0;
    else if (in_file && strncmp(buf, "line=", 5) == 0) {
      char *_be;
      long _bl = strtol(buf + 5, &_be, 10);
      int ln = (_be != buf + 5 && _bl > 0 && _bl <= INT_MAX) ? (int)_bl : 0;
      if (ln > 0 && s->bookmark_count < GTK_MAX_BOOKMARKS)
        s->bookmarks[s->bookmark_count++] = ln - 1;
    }
  }
  fclose(f);
}

/* Shared key handler for Instruction/Directive Reference windows: Escape closes
 */
static gboolean refwnd_key_pressed_cb(GtkEventControllerKey *ctl, guint keyval,
                                      guint keycode, GdkModifierType state,
                                      gpointer user_data) {
  (void)ctl;
  (void)keycode;
  (void)state;
  (void)user_data;
  if (keyval == GDK_KEY_Escape) {
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(ctl));
    if (w)
      gtk_window_close(GTK_WINDOW(w));
    return TRUE;
  }
  return FALSE;
}

static void action_directive_ref(GSimpleAction *a, GVariant *p,
                                 gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;

  DRefDlg *d = g_new0(DRefDlg, 1);
  d->app = s;

  d->dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(d->dlg), "Directive Reference");
  gtk_window_set_transient_for(GTK_WINDOW(d->dlg), GTK_WINDOW(s->win));
  gtk_window_set_default_size(GTK_WINDOW(d->dlg), 860, 580);
  g_object_set_data_full(G_OBJECT(d->dlg), "dref-dlg", d, dref_dialog_free);
  apply_window_dark_hint(d->dlg, s->dark_mode);

  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(paned), 200);
  gtk_window_set_child(GTK_WINDOW(d->dlg), paned);

  /* Left panel */
  GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(lbox, 8);
  gtk_widget_set_margin_bottom(lbox, 8);
  gtk_widget_set_margin_start(lbox, 8);
  gtk_widget_set_margin_end(lbox, 4);
  gtk_paned_set_start_child(GTK_PANED(paned), lbox);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

  d->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(d->search),
                                        "Search...");
  gtk_box_append(GTK_BOX(lbox), d->search);

  {
    GtkStringList *sl = gtk_string_list_new(dref_groups);
    d->group_drop = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->group_drop), 0);
    gtk_box_append(GTK_BOX(lbox), d->group_drop);
  }

  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc, TRUE);
  gtk_box_append(GTK_BOX(lbox), sc);

  d->list = gtk_list_box_new();
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(d->list), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), d->list);

  for (int i = 0; i < G_DREF_COUNT; i++) {
    GtkWidget *lbl = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped("<tt><b>%s</b></tt>", g_dref[i].name);
    gtk_label_set_markup(GTK_LABEL(lbl), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_margin_top(row, 2);
    gtk_widget_set_margin_bottom(row, 2);
    gtk_widget_set_margin_start(row, 4);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
    g_object_set_data(G_OBJECT(row), "dref-index", GINT_TO_POINTER(i));
    gtk_list_box_append(GTK_LIST_BOX(d->list), row);
  }

  /* Right panel */
  d->detail_scroll = gtk_scrolled_window_new();
  d->detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(d->detail_box, 12);
  gtk_widget_set_margin_bottom(d->detail_box, 12);
  gtk_widget_set_margin_start(d->detail_box, 12);
  gtk_widget_set_margin_end(d->detail_box, 12);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(d->detail_scroll),
                                d->detail_box);
  {
    GtkWidget *copy_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(copy_bar, 6);
    gtk_widget_set_margin_end(copy_bar, 8);
    gtk_widget_set_halign(copy_bar, GTK_ALIGN_END);
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    gtk_widget_set_tooltip_text(copy_btn, "Copy this entry to clipboard");
    gtk_box_append(GTK_BOX(copy_bar), copy_btn);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(dref_copy_clicked), d);
    GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_set_end_child(GTK_PANED(paned), rbox);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_box_append(GTK_BOX(rbox), copy_bar);
    gtk_box_append(GTK_BOX(rbox), d->detail_scroll);
    gtk_widget_set_vexpand(d->detail_scroll, TRUE);
  }

  g_signal_connect(d->search, "search-changed", G_CALLBACK(dref_search_changed),
                   d);
  g_signal_connect(d->group_drop, "notify::selected",
                   G_CALLBACK(dref_group_changed), d);
  g_signal_connect(d->list, "row-selected", G_CALLBACK(dref_row_selected), d);

  gtk_list_box_set_filter_func(GTK_LIST_BOX(d->list), dref_filter_row, d, NULL);
  GtkListBoxRow *first =
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(d->list), 0);
  if (first) {
    gtk_list_box_select_row(GTK_LIST_BOX(d->list), first);
    dref_build_detail(d, 0);
  }

  GtkEventController *dref_keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(dref_keys, GTK_PHASE_CAPTURE);
  g_signal_connect(dref_keys, "key-pressed", G_CALLBACK(refwnd_key_pressed_cb),
                   NULL);
  gtk_widget_add_controller(GTK_WIDGET(d->dlg), dref_keys);

  gtk_window_present(GTK_WINDOW(d->dlg));
}

/* Action handler: open or bring to front the instruction reference browser. */
static void action_instruction_ref(GSimpleAction *a, GVariant *p,
                                   gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;

  IRefDlg *d = g_new0(IRefDlg, 1);
  d->app = s;
  d->cur_index = 0;

  /* Top-level dialog */
  d->dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(d->dlg), "Instruction Reference");
  gtk_window_set_transient_for(GTK_WINDOW(d->dlg), GTK_WINDOW(s->win));
  gtk_window_set_default_size(GTK_WINDOW(d->dlg), 900, 640);
  g_object_set_data_full(G_OBJECT(d->dlg), "iref-dlg", d, iref_dialog_free);
  apply_window_dark_hint(d->dlg, s->dark_mode);

  /* Outer horizontal pane */
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_position(GTK_PANED(paned), 180);
  gtk_window_set_child(GTK_WINDOW(d->dlg), paned);

  /* ---- Left panel: search + list ---- */
  GtkWidget *lbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(lbox, 8);
  gtk_widget_set_margin_bottom(lbox, 8);
  gtk_widget_set_margin_start(lbox, 8);
  gtk_widget_set_margin_end(lbox, 4);
  gtk_paned_set_start_child(GTK_PANED(paned), lbox);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

  d->search = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(d->search),
                                        "Search...");
  gtk_box_append(GTK_BOX(lbox), d->search);

  /* CPU filter dropdown */
  {
    const char *cpu_opts[] = {"All CPUs",
                              "6502 (legal only)",
                              "6502 (illegal/undoc)",
                              "6502 (all)",
                              "65C02",
                              "65816",
                              NULL};
    GtkStringList *sl = gtk_string_list_new(cpu_opts);
    d->cpu_drop = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->cpu_drop), 0);
    gtk_box_append(GTK_BOX(lbox), d->cpu_drop);
  }

  /* Address mode filter dropdown */
  {
    GtkStringList *sl = gtk_string_list_new(iref_am_names);
    d->am_drop = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(d->am_drop), 0);
    gtk_box_append(GTK_BOX(lbox), d->am_drop);
  }

  GtkWidget *sc_list = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(sc_list, TRUE);
  gtk_box_append(GTK_BOX(lbox), sc_list);

  d->list = gtk_list_box_new();
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(d->list), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc_list), d->list);

  /* Populate list — sorted alphabetically (already in order in g_iref) */
  for (int i = 0; i < G_IREF_COUNT; i++) {
    GtkWidget *lbl = gtk_label_new(NULL);
    char *mu = g_markup_printf_escaped("<tt><b>%s</b></tt>  <small>%s</small>",
                                       g_iref[i].mnem, g_iref[i].full_name);
    gtk_label_set_markup(GTK_LABEL(lbl), mu);
    g_free(mu);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_widget_set_margin_top(row, 2);
    gtk_widget_set_margin_bottom(row, 2);
    gtk_widget_set_margin_start(row, 4);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
    g_object_set_data(G_OBJECT(row), "iref-index", GINT_TO_POINTER(i));
    g_object_set_data(G_OBJECT(row), "mnem", (gpointer)g_iref[i].mnem);
    gtk_list_box_append(GTK_LIST_BOX(d->list), row);
  }

  /* ---- Right panel: scrollable detail view ---- */
  d->detail_scroll = gtk_scrolled_window_new();
  d->detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_top(d->detail_box, 12);
  gtk_widget_set_margin_bottom(d->detail_box, 12);
  gtk_widget_set_margin_start(d->detail_box, 12);
  gtk_widget_set_margin_end(d->detail_box, 12);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(d->detail_scroll),
                                d->detail_box);
  {
    GtkWidget *copy_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(copy_bar, 6);
    gtk_widget_set_margin_end(copy_bar, 8);
    gtk_widget_set_halign(copy_bar, GTK_ALIGN_END);
    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    gtk_widget_set_tooltip_text(copy_btn, "Copy this entry to clipboard");
    gtk_box_append(GTK_BOX(copy_bar), copy_btn);
    g_signal_connect(copy_btn, "clicked", G_CALLBACK(iref_copy_clicked), d);
    GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_paned_set_end_child(GTK_PANED(paned), rbox);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_box_append(GTK_BOX(rbox), copy_bar);
    gtk_box_append(GTK_BOX(rbox), d->detail_scroll);
    gtk_widget_set_vexpand(d->detail_scroll, TRUE);
  }

  /* Connect signals */
  g_signal_connect(d->search, "search-changed", G_CALLBACK(iref_search_changed),
                   d);
  g_signal_connect(d->cpu_drop, "notify::selected",
                   G_CALLBACK(iref_cpu_changed), d);
  g_signal_connect(d->am_drop, "notify::selected", G_CALLBACK(iref_am_changed),
                   d);
  g_signal_connect(d->list, "row-selected", G_CALLBACK(iref_row_selected), d);

  /* Install initial filter and select first entry */
  gtk_list_box_set_filter_func(GTK_LIST_BOX(d->list), iref_filter_row, d, NULL);
  GtkListBoxRow *first =
      gtk_list_box_get_row_at_index(GTK_LIST_BOX(d->list), 0);
  if (first) {
    gtk_list_box_select_row(GTK_LIST_BOX(d->list), first);
    iref_build_detail(d, 0);
  }

  GtkEventController *iref_keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(iref_keys, GTK_PHASE_CAPTURE);
  g_signal_connect(iref_keys, "key-pressed", G_CALLBACK(refwnd_key_pressed_cb),
                   NULL);
  gtk_widget_add_controller(GTK_WIDGET(d->dlg), iref_keys);

  gtk_window_present(GTK_WINDOW(d->dlg));
}

static void action_help(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;

  /* Find help.html — check AppImage, installed, then source dir */
  gchar *path = NULL;
  const char *appdir = g_getenv("APPDIR");
  if (appdir) {
    path = g_build_filename(appdir, "usr", "share", "6502caEd", "help",
                            "help.html", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_free(path);
      path = NULL;
    }
  }
  if (!path) {
    path = g_strdup("/usr/share/6502caEd/help/help.html");
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_free(path);
      path = NULL;
    }
  }
  if (!path) {
    path = g_build_filename(ED_SOURCE_DIR, "help", "help.html", NULL);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
      g_free(path);
      path = NULL;
    }
  }

  if (!path) {
    show_error(s, "Help file not found.\nExpected help/help.html in the "
                  "installation directory.");
    return;
  }

  gchar *uri = g_strdup_printf("file://%s", path);
  gtk_show_uri(s->win, uri, GDK_CURRENT_TIME);
  g_free(uri);
  g_free(path);
}

/* Licence and dependency notices are informational, with no acceptance gate. */
static void show_legal_text(GtkWindow *parent, const char *title, const char *text) {
  GtkWidget *win = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(win), title);
  gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(win), TRUE);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(win), 720, 540);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_window_set_child(GTK_WINDOW(win), box);
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkWidget *view = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 10);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 10);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text, -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
  gtk_box_append(GTK_BOX(box), scroll);
  GtkWidget *ok = gtk_button_new_with_label("OK");
  gtk_widget_set_halign(ok, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(box), ok);
  g_signal_connect_swapped(ok, "clicked", G_CALLBACK(gtk_window_destroy), win);
  gtk_window_set_default_widget(GTK_WINDOW(win), ok);
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(about_key_pressed), win);
  gtk_widget_add_controller(win, keys);
  gtk_widget_grab_focus(ok);
  gtk_window_present(GTK_WINDOW(win));
}

static void about_show_licence(GtkButton *button, gpointer parent) {
  (void)button;
  show_legal_text(GTK_WINDOW(parent), "6502caEd — MIT Licence", ED_MIT_LICENSE_TEXT);
}

/* Read the first matching distribution file; never use an old user GPL notice. */
static gchar *load_distribution_text(const char *name) {
  char exe[4096];
  _gui_exe_dir(exe, sizeof(exe));
  const char *dirs[] = {exe, "/usr/share/6502caEd", "/usr/local/share/6502caEd",
#ifdef ED_SOURCE_DIR
                       ED_SOURCE_DIR,
#endif
                       NULL};
  for (int i = 0; dirs[i]; ++i) {
    if (!dirs[i][0]) continue;
    gchar *path = g_build_filename(dirs[i], name, NULL);
    gchar *text = NULL;
    gboolean found = g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (found) {
      gchar *valid = g_utf8_make_valid(text, -1);
      g_free(text);
      return valid;
    }
  }
  return NULL;
}

static gint legal_name_compare(gconstpointer a, gconstpointer b) {
  return g_strcmp0(*(const gchar *const *)a, *(const gchar *const *)b);
}

static void append_packaged_notices(GString *text, const gchar *dir, int depth) {
  if (depth > 8) return;
  GDir *d = g_dir_open(dir, 0, NULL);
  if (!d) return;
  GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
  const gchar *name;
  while ((name = g_dir_read_name(d))) g_ptr_array_add(names, g_strdup(name));
  g_dir_close(d);
  g_ptr_array_sort(names, legal_name_compare);
  for (guint i = 0; i < names->len; ++i) {
    gchar *path = g_build_filename(dir, (gchar *)names->pdata[i], NULL);
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK)) { g_free(path); continue; }
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
      append_packaged_notices(text, path, depth + 1);
    } else {
      gchar *contents = NULL;
      if (g_file_get_contents(path, &contents, NULL, NULL)) {
        gchar *valid = g_utf8_make_valid(contents, -1);
        g_string_append_printf(text, "\n\n--- %s ---\n\n%s", path, valid);
        g_free(valid);
        g_free(contents);
      }
    }
    g_free(path);
  }
  g_ptr_array_free(names, TRUE);
}

static void about_show_notices(GtkButton *button, gpointer parent) {
  (void)button;
  gchar *summary = load_distribution_text("THIRD_PARTY_NOTICES.txt");
  GString *text = g_string_new(summary ? summary :
      "6502caEd uses GTK, GtkSourceView and their dependencies.\n"
      "These components retain their own copyrights and licences.\n"
      "See the distribution's third-party notices and package documentation.\n");
  g_free(summary);
  char exe[4096];
  _gui_exe_dir(exe, sizeof(exe));
  if (exe[0]) {
    gchar *dir = g_build_filename(exe, "third-party-licenses", NULL);
    append_packaged_notices(text, dir, 0);
    g_free(dir);
  }
  show_legal_text(GTK_WINDOW(parent), "6502caEd — Third-party notices", text->str);
  g_string_free(text, TRUE);
}

#include "about_tigger.h"

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;

  GtkWidget *w = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(w), "About");
  gtk_window_set_transient_for(GTK_WINDOW(w), s->win);
  gtk_window_set_modal(GTK_WINDOW(w), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(w), FALSE);
  apply_window_dark_hint(w, s->dark_mode);
  if (s->app)
    gtk_application_add_window(s->app, GTK_WINDOW(w));
  //  gtk_window_set_default_size(GTK_WINDOW(w), 520, 360);
  gtk_window_set_default_size(GTK_WINDOW(w), 480, 300);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top(outer, 24);
  gtk_widget_set_margin_bottom(outer, 24);
  gtk_widget_set_margin_start(outer, 24);
  gtk_widget_set_margin_end(outer, 24);
  gtk_window_set_child(GTK_WINDOW(w), outer);

  /* Title spans the whole header; decorative artwork does not affect its
   * horizontal allocation or centre point. */
  GtkWidget *header = gtk_overlay_new();
  gtk_widget_set_size_request(header, -1, 44);
  gtk_widget_set_halign(header, GTK_ALIGN_FILL);
  gtk_box_append(GTK_BOX(outer), header);

  GtkWidget *center = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_hexpand(center, TRUE);
  gtk_widget_set_vexpand(center, TRUE);
  gtk_widget_set_halign(center, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(center, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(outer), center);

  GtkWidget *title = gtk_label_new("6502caEd");
  gtk_label_set_selectable(GTK_LABEL(title), FALSE);
  gtk_widget_set_can_focus(title, FALSE);
  gtk_widget_add_css_class(title, "title-1");
  gtk_widget_set_hexpand(title, TRUE);
  gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
  gtk_overlay_set_child(GTK_OVERLAY(header), title);

  GdkTexture *cat_texture = about_tigger_texture();
  GtkWidget *cat = gtk_picture_new_for_paintable(GDK_PAINTABLE(cat_texture));
  g_object_unref(cat_texture);
  gtk_picture_set_can_shrink(GTK_PICTURE(cat), FALSE);
  gtk_widget_set_size_request(cat, 72, 37);
  gtk_widget_set_halign(cat, GTK_ALIGN_END);
  gtk_widget_set_valign(cat, GTK_ALIGN_START);
  gtk_widget_set_tooltip_text(cat, "Tigger");
  gtk_accessible_update_property(GTK_ACCESSIBLE(cat),
      GTK_ACCESSIBLE_PROPERTY_LABEL, "Tigger, a sleeping tabby cat", -1);
  gtk_overlay_add_overlay(GTK_OVERLAY(header), cat);
  gtk_overlay_set_measure_overlay(GTK_OVERLAY(header), cat, FALSE);

  GtkWidget *desc = gtk_label_new("6502 assembler editor");
  gtk_box_append(GTK_BOX(center), desc);

  GtkWidget *ver = gtk_label_new(AppVerString);
  gtk_box_append(GTK_BOX(center), ver);

  GtkWidget *byyear = gtk_label_new("© 2026 MJD");
  gtk_box_append(GTK_BOX(center), byyear);

  GtkWidget *legal = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(legal, GTK_ALIGN_CENTER);
  GtkWidget *licence = gtk_button_new_with_label("Licence");
  GtkWidget *notices = gtk_button_new_with_label("Third-party notices");
  g_signal_connect(licence, "clicked", G_CALLBACK(about_show_licence), w);
  g_signal_connect(notices, "clicked", G_CALLBACK(about_show_notices), w);
  gtk_box_append(GTK_BOX(legal), licence);
  gtk_box_append(GTK_BOX(legal), notices);
  gtk_box_append(GTK_BOX(center), legal);

  GtkWidget *ok = gtk_button_new_with_label("OK");
  gtk_widget_set_halign(ok, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(ok, 18);
  gtk_box_append(GTK_BOX(outer), ok);
  g_signal_connect_swapped(ok, "clicked", G_CALLBACK(gtk_window_destroy), w);

  /* Enter activates OK, Escape closes. */
  gtk_window_set_default_widget(GTK_WINDOW(w), ok);
  gtk_widget_grab_focus(ok);
  GtkEventController *kc = gtk_event_controller_key_new();
  g_signal_connect(kc, "key-pressed", G_CALLBACK(about_key_pressed), w);
  gtk_widget_add_controller(w, kc);

  gtk_window_present(GTK_WINDOW(w));
}

// Toggle action: app.dark (stateful boolean)
static void action_toggle_dark(GSimpleAction *a, GVariant *p,
                               gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;

  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : FALSE;
  gboolean next = !cur;

  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->dark_mode = next;
  colors_set_defaults(s);
  colors_load_from_exe_dir(s);
  apply_theme(s);
  gtk_prefs_save(s);
}

// Toggle action: app.problems (stateful boolean)
static void action_toggle_problems(GSimpleAction *a, GVariant *p,
                                   gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;

  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : TRUE;
  gboolean next = !cur;

  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->problems_enabled = next;

  if (!s->problems_enabled) {
    /* Feature disabled: hide and clear. */
    if (s->problems_revealer)
      gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), FALSE);
    problems_clear(s);
  } else {
    /* Recompute from current sources; do not force-open if user hid it. */
    schedule_problems_refresh(s, FALSE);
  }

  if (s->problems_paned)
    gtk_widget_queue_allocate(s->problems_paned);
}

// Toggle action: app.error_underline (stateful boolean)
static void action_toggle_error_underline(GSimpleAction *a, GVariant *p,
                                          gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;

  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : FALSE;
  gboolean next = !cur;

  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->error_underline = next;

  /* Changing tag properties updates existing highlights immediately. */
  GdkRGBA u;
  gdk_rgba_parse(&u, s->colors.err_underline);
  for (guint i = 0; s->docs && i < s->docs->len; i++) {
    EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, i);
    if (!d || !d->tag_error)
      continue;
    if (s->error_underline) {
      g_object_set(d->tag_error, "foreground-set", FALSE, "weight-set", FALSE,
                   "underline", PANGO_UNDERLINE_SINGLE, "underline-set", TRUE,
                   "underline-rgba", &u, "underline-rgba-set", TRUE, NULL);
    } else {
      GdkRGBA fg;
      /* This build only exposes one configured error colour (err_underline).
         When underline mode is OFF we also use it as the foreground colour to
         get the classic "red + bold" style. */
      gdk_rgba_parse(&fg, s->colors.err_underline);
      g_object_set(d->tag_error, "underline-set", FALSE, "underline-rgba-set",
                   FALSE, "foreground-rgba", &fg, "foreground-set", TRUE,
                   "weight", PANGO_WEIGHT_BOLD, "weight-set", TRUE, NULL);
    }
  }
}

// Toggle action: app.asm_clear_mem (stateful boolean)
static void action_toggle_asm_clear_mem(GSimpleAction *a, GVariant *p,
                                        gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;

  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : TRUE;
  gboolean next = !cur;

  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->asm_clear_mem = next;
}

// Toggle action: app.allow_undoc (stateful boolean)
static void action_toggle_allow_undoc(GSimpleAction *a, GVariant *p,
                                      gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;
  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : FALSE;
  gboolean next = !cur;
  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->allow_undoc = next;
}

// Toggle action: app.autocomplete
static void action_toggle_autocomplete(GSimpleAction *a, GVariant *p,
                                       gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;
  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : TRUE;
  gboolean next = !cur;
  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->autocomplete = next;
  /* Apply to all open docs */
  if (s->docs) {
    GtkSourceView *saved_view = s->view;
    for (guint i = 0; i < s->docs->len; i++) {
      EditorDoc *d = (EditorDoc *)g_ptr_array_index(s->docs, i);
      if (d && d->view) {
        s->view = d->view;
        editor_completion_apply(s);
      }
    }
    s->view = saved_view;
  }
}

// Toggle action: app.fold_comments
/* Helper: collapse or expand all groups of a given type */
static void fold_set_all(AppState *s, gboolean is_macro, gboolean collapsed) {
  if (!s)
    return;
  /* Ensure fold_comments/fold_macros is set so map is built */
  if (!is_macro)
    s->fold_comments = TRUE;
  else
    s->fold_macros = TRUE;
  fold_map_update(s);
  if (!s->fold_map.lines)
    return;
  for (size_t i = 0; i < s->fold_map.count; i++) {
    if (s->fold_map.lines[i].kind == FL_FOLD_START &&
        (gboolean)s->fold_map.lines[i].is_macro == is_macro)
      s->fold_map.lines[i].collapsed = collapsed;
  }
  apply_fold_tags(s);
  gtk_prefs_save(s);
}

static void action_fold_comments(GSimpleAction *a, GVariant *par, gpointer ud) {
  (void)a;
  (void)par;
  fold_set_all((AppState *)ud, FALSE, TRUE);
}
/* Action handler: expand all collapsed comment folds. */
static void action_unfold_comments(GSimpleAction *a, GVariant *par,
                                   gpointer ud) {
  (void)a;
  (void)par;
  fold_set_all((AppState *)ud, FALSE, FALSE);
}
static void action_fold_macros(GSimpleAction *a, GVariant *par, gpointer ud) {
  (void)a;
  (void)par;
  fold_set_all((AppState *)ud, TRUE, TRUE);
}
/* Action handler: expand all collapsed macro folds. */
static void action_unfold_macros(GSimpleAction *a, GVariant *par, gpointer ud) {
  (void)a;
  (void)par;
  fold_set_all((AppState *)ud, TRUE, FALSE);
}

// Toggle action: app.auto_indent (stateful boolean)
static void action_toggle_auto_indent(GSimpleAction *a, GVariant *p,
                                      gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;

  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : TRUE;
  gboolean next = !cur;

  g_simple_action_set_state(a, g_variant_new_boolean(next));
  if (s) {
    s->auto_indent = next;
    apply_editor_prefs(s);
  }
}

// Toggle action: app.keep_comment_aligned (stateful boolean)
static void action_toggle_keep_comment_aligned(GSimpleAction *a, GVariant *p,
                                               gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;

  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : FALSE;
  gboolean next = !cur;

  g_simple_action_set_state(a, g_variant_new_boolean(next));
  if (s) {
    s->keep_comment_aligned = next;
    if (s->keep_comment_aligned && s->comment_align_col <= 0)
      s->comment_align_col = 32; /* sensible default */
  }
}

typedef struct {
  GMainLoop *loop;
  int response;
} DialogRunState;

static void dialog_response_quit(GtkDialog *d, int response_id,
                                 gpointer user_data) {
  (void)d;
  DialogRunState *r = (DialogRunState *)user_data;
  r->response = response_id;
  if (r->loop)
    g_main_loop_quit(r->loop);
}

static void action_save_prefs(GSimpleAction *a, GVariant *p,
                              gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;
  gtk_prefs_save(s);
  set_status(s, "Settings saved.");
}

/* Action handler: toggle the 'reopen last file on startup' preference. */
static void action_toggle_reopen_last_file(GSimpleAction *a, GVariant *p,
                                           gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;
  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : FALSE;
  gboolean next = !cur;
  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->reopen_last_file = next;
  /* Save immediately so the setting persists even if the app crashes */
  gtk_prefs_save(s);
  if (!next) {
    /* User disabled it — remove stale session file immediately */
    char path[PATH_MAX];
    gtk_session_path(path, sizeof(path));
    remove(path);
  }
}

static void action_toggle_word_wrap(GSimpleAction *a, GVariant *p,
                                    gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;
  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : FALSE;
  gboolean next = !cur;
  g_simple_action_set_state(a, g_variant_new_boolean(next));
  s->word_wrap = next;
  apply_editor_prefs(s);
}

/* Action handler: increase the editor font size by one step. */
static void action_zoom_in(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  if (!s)
    return;
  if (s->font_size <= 0)
    s->font_size = 12;
  if (s->font_size < 72)
    s->font_size++;
  apply_font_css(s);
  gtk_prefs_save(s);
}

/* Action handler: decrease the editor font size by one step. */
static void action_zoom_out(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  if (!s)
    return;
  if (s->font_size <= 0)
    s->font_size = 12;
  if (s->font_size > 6)
    s->font_size--;
  apply_font_css(s);
  gtk_prefs_save(s);
}

/* Action handler: reset the editor font size to the default. */
static void action_zoom_reset(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  if (!s)
    return;
  s->font_size = (s->default_font_size_set && s->default_font_size > 0)
                     ? s->default_font_size
                     : 12;
  apply_font_css(s);
}

/* Action handler: open the font chooser dialog. */
static void action_font(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  if (!s || !s->win)
    return;

  GtkWidget *dlg = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Editor Font");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), s->win);
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
  gtk_widget_set_margin_start(grid, 16);
  gtk_widget_set_margin_end(grid, 16);
  gtk_widget_set_margin_top(grid, 12);
  gtk_widget_set_margin_bottom(grid, 12);
  gtk_box_append(GTK_BOX(content), grid);

  /* Font family row */
  GtkWidget *lbl_fam = gtk_label_new("Font family:");
  gtk_widget_set_halign(lbl_fam, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), lbl_fam, 0, 0, 1, 1);
  GtkWidget *entry_fam = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry_fam),
                        s->font_family[0] ? s->font_family : "Monospace");
  gtk_widget_set_size_request(entry_fam, 220, -1);
  gtk_entry_set_activates_default(GTK_ENTRY(entry_fam), TRUE);
  gtk_grid_attach(GTK_GRID(grid), entry_fam, 1, 0, 1, 1);

  /* Font size row */
  GtkWidget *lbl_sz = gtk_label_new("Size (pt):");
  gtk_widget_set_halign(lbl_sz, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), lbl_sz, 0, 1, 1, 1);
  if (s->font_size <= 0)
    s->font_size = 12;
  GtkAdjustment *adj = gtk_adjustment_new(s->font_size, 6, 72, 1, 2, 0);
  GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
  gtk_widget_set_halign(spin, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), spin, 1, 1, 1, 1);

  gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

  DialogRunState dr = {0};
  dr.loop = g_main_loop_new(NULL, FALSE);
  dr.response = GTK_RESPONSE_CANCEL;
  g_signal_connect(dlg, "response", G_CALLBACK(dialog_response_quit), &dr);
  gtk_widget_show(dlg);
  g_main_loop_run(dr.loop);
  int resp = dr.response;
  g_main_loop_unref(dr.loop);

  if (resp == GTK_RESPONSE_OK) {
    const char *fam = gtk_editable_get_text(GTK_EDITABLE(entry_fam));
    if (fam && fam[0])
      g_strlcpy(s->font_family, fam, sizeof(s->font_family));
    s->font_size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    s->default_font_size = s->font_size;
    s->default_font_size_set = TRUE;
    apply_font_css(s);
    gtk_prefs_save(s);
  }
  gtk_window_destroy(GTK_WINDOW(dlg));
}

/* Action handler: change the default source file extension. */
static void action_set_src_ext(GSimpleAction *a, GVariant *p,
                               gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->win)
    return;

  GtkWidget *dlg = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Default source extension");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), s->win);
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_box_append(GTK_BOX(content), box);
  GtkWidget *lbl =
      gtk_label_new("Source file extension (e.g. .asm, .s, .65s):");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), lbl);
  GtkWidget *entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry),
                        s->src_ext[0] ? s->src_ext : ".asm");
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_widget_set_halign(entry, GTK_ALIGN_START);
  gtk_widget_set_size_request(entry, 120, -1);
  gtk_box_append(GTK_BOX(box), entry);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
  DialogRunState dr = {0};
  dr.loop = g_main_loop_new(NULL, FALSE);
  dr.response = GTK_RESPONSE_CANCEL;
  g_signal_connect(dlg, "response", G_CALLBACK(dialog_response_quit), &dr);
  gtk_widget_show(dlg);
  g_main_loop_run(dr.loop);
  int resp = dr.response;
  g_main_loop_unref(dr.loop);
  if (resp == GTK_RESPONSE_OK) {
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (txt && txt[0]) {
      if (txt[0] != '.')
        g_snprintf(s->src_ext, sizeof(s->src_ext), ".%s", txt);
      else
        g_strlcpy(s->src_ext, txt, sizeof(s->src_ext));
    }
  }
  gtk_window_destroy(GTK_WINDOW(dlg));
}

/* Action handler: change the default binary output extension. */
static void action_set_bin_ext(GSimpleAction *a, GVariant *p,
                               gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->win)
    return;

  GtkWidget *dlg = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Default binary extension");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), s->win);
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_box_append(GTK_BOX(content), box);
  GtkWidget *lbl =
      gtk_label_new("Binary output extension (e.g. .bin, .rom, .prg):");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), lbl);
  GtkWidget *entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(entry),
                        s->bin_ext[0] ? s->bin_ext : ".bin");
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_widget_set_halign(entry, GTK_ALIGN_START);
  gtk_widget_set_size_request(entry, 120, -1);
  gtk_box_append(GTK_BOX(box), entry);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
  DialogRunState dr = {0};
  dr.loop = g_main_loop_new(NULL, FALSE);
  dr.response = GTK_RESPONSE_CANCEL;
  g_signal_connect(dlg, "response", G_CALLBACK(dialog_response_quit), &dr);
  gtk_widget_show(dlg);
  g_main_loop_run(dr.loop);
  int resp = dr.response;
  g_main_loop_unref(dr.loop);
  if (resp == GTK_RESPONSE_OK) {
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (txt && txt[0]) {
      if (txt[0] != '.')
        g_snprintf(s->bin_ext, sizeof(s->bin_ext), ".%s", txt);
      else
        g_strlcpy(s->bin_ext, txt, sizeof(s->bin_ext));
    }
  }
  gtk_window_destroy(GTK_WINDOW(dlg));
}

/* Action handler: set the editor tab stop width. */
static void action_tab_width(GSimpleAction *a, GVariant *p,
                             gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->win)
    return;

  GtkWidget *dlg = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Tab width");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), s->win);
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_box_append(GTK_BOX(content), box);

  GtkWidget *lbl = gtk_label_new("Spaces per tab (Tab inserts spaces):");
  gtk_widget_set_halign(lbl, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), lbl);

  if (s->tab_width <= 0)
    s->tab_width = 4;
  GtkAdjustment *adj = gtk_adjustment_new(s->tab_width, 1, 16, 1, 2, 0);
  GtkWidget *spin = gtk_spin_button_new(adj, 1, 0);
  gtk_widget_set_halign(spin, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), spin);

  gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dlg), "OK", GTK_RESPONSE_OK);
  gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

  /* GTK4 removed gtk_dialog_run(). Implement a tiny blocking wait using a
     nested main loop so this stays dependency-free and minimal. */
  DialogRunState dr = {0};
  dr.loop = g_main_loop_new(NULL, FALSE);
  dr.response = GTK_RESPONSE_CANCEL;
  g_signal_connect(dlg, "response", G_CALLBACK(dialog_response_quit), &dr);

  gtk_widget_show(dlg);
  g_main_loop_run(dr.loop);
  int resp = dr.response;
  g_main_loop_unref(dr.loop);
  if (resp == GTK_RESPONSE_OK) {
    int v = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin));
    if (v < 1)
      v = 1;
    if (v > 16)
      v = 16;
    s->tab_width = v;
    apply_editor_prefs(s);
  }

  gtk_window_destroy(GTK_WINDOW(dlg));
}

// Stateful action: app.comment_marker (string: "semicolon" | "slashslash")
static void action_comment_marker(GSimpleAction *a, GVariant *p,
                                  gpointer user_data) {
  AppState *s = (AppState *)user_data;
  if (!s)
    return;

  const char *v = g_variant_get_string(p, NULL);
  gboolean use_slash = (g_strcmp0(v, "slashslash") == 0);

  s->comment_use_slashslash = use_slash;
  g_simple_action_set_state(
      a, g_variant_new_string(use_slash ? "slashslash" : "semicolon"));
}

// Convenience toggle: Ctrl+Alt+M
static void action_toggle_comment_marker(GSimpleAction *a, GVariant *p,
                                         gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s)
    return;

  s->comment_use_slashslash = !s->comment_use_slashslash;

  GAction *act =
      g_action_map_lookup_action(G_ACTION_MAP(s->app), "comment_marker");
  if (act) {
    g_action_change_state(act, g_variant_new_string(s->comment_use_slashslash
                                                        ? "slashslash"
                                                        : "semicolon"));
  }
}

static void action_comment_align_col(GSimpleAction *a, GVariant *p,
                                     gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  if (!s || !s->win)
    return;

  GtkDialog *d = GTK_DIALOG(gtk_dialog_new());
  gtk_window_set_transient_for(GTK_WINDOW(d), s->win);
  gtk_window_set_modal(GTK_WINDOW(d), TRUE);
  gtk_window_set_title(GTK_WINDOW(d), "Comment align column");
  gtk_dialog_add_button(d, "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(d, "OK", GTK_RESPONSE_OK);

  GtkWidget *content = gtk_dialog_get_content_area(d);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 12);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_box_append(GTK_BOX(content), box);

  GtkWidget *lbl = gtk_label_new(
      "Set the target column for the comment marker when commenting.\n"
      "Lines already past this column are left unchanged.");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_box_append(GTK_BOX(box), lbl);

  GtkAdjustment *adj = gtk_adjustment_new(
      (double)(s->comment_align_col > 0 ? s->comment_align_col : 32), 1.0,
      240.0, 1.0, 8.0, 0.0);
  GtkWidget *spin = gtk_spin_button_new(adj, 1.0, 0);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE);
  gtk_box_append(GTK_BOX(box), spin);

  gtk_widget_show(GTK_WIDGET(d));

  DialogRunState r = {0};
  r.loop = g_main_loop_new(NULL, FALSE);
  r.response = GTK_RESPONSE_CANCEL;
  g_signal_connect(d, "response", G_CALLBACK(dialog_response_quit), &r);
  g_main_loop_run(r.loop);
  g_main_loop_unref(r.loop);

  if (r.response == GTK_RESPONSE_OK) {
    int v = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin));
    if (v < 1)
      v = 1;
    if (v > 240)
      v = 240;
    s->comment_align_col = v;
    /* If user sets a column, enabling alignment is usually intended. */
    s->keep_comment_aligned = TRUE;
    /* Keep action state in sync */
    GAction *ga = g_action_map_lookup_action(G_ACTION_MAP(s->app),
                                             "keep_comment_aligned");
    if (ga && G_IS_SIMPLE_ACTION(ga))
      g_simple_action_set_state(G_SIMPLE_ACTION(ga),
                                g_variant_new_boolean(TRUE));
  }

  gtk_window_destroy(GTK_WINDOW(d));
}

// Toggle action: app.problems_visible (stateful boolean) - show/hide the panel
static void action_toggle_problems_visible(GSimpleAction *a, GVariant *p,
                                           gpointer user_data) {
  (void)p;
  AppState *s = (AppState *)user_data;

  GVariant *state = g_action_get_state(G_ACTION(a));
  gboolean cur = state ? g_variant_get_boolean(state) : TRUE;
  gboolean next = !cur;

  g_simple_action_set_state(a, g_variant_new_boolean(next));

  if (!s)
    return;

  if (!s->problems_enabled) {
    /* If the feature is disabled, keep hidden regardless of visibility toggle.
     */
    if (s->problems_revealer)
      gtk_revealer_set_reveal_child(GTK_REVEALER(s->problems_revealer), FALSE);
    return;
  }

  if (next)
    problems_show(s);
  else
    problems_hide(s);

  if (s->problems_paned)
    gtk_widget_queue_allocate(s->problems_paned);
}

static void action_format_upper(GSimpleAction *a, GVariant *p,
                                gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  format_buffer_case(s, TRUE);
}

/* Action handler: convert all source text to lowercase. */
static void action_format_lower(GSimpleAction *a, GVariant *p,
                                gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  format_buffer_case(s, FALSE);
}

typedef struct UnicodeReplaceMap {
  gunichar ch;
  const char *repl;
  const char *desc;
} UnicodeReplaceMap;

typedef struct {
  GMainLoop *loop;
  int response;
} UnicodeDialogRunState;

/* Response callback for the Unicode replacement review dialog. */
static void unicode_dialog_response_quit(GtkDialog *d, int response_id,
                                         gpointer user_data) {
  (void)d;
  UnicodeDialogRunState *r = (UnicodeDialogRunState *)user_data;
  r->response = response_id;
  if (r->loop)
    g_main_loop_quit(r->loop);
}

static const UnicodeReplaceMap *unicode_common_replacement(gunichar ch) {
  static const UnicodeReplaceMap map[] = {
      {0x2018, "'", "left single quote"},
      {0x2019, "'", "right single quote"},
      {0x201A, "'", "single low-9 quote"},
      {0x201B, "'", "single high-reversed-9 quote"},
      {0x2032, "'", "prime"},
      {0x201C, "\"", "left double quote"},
      {0x201D, "\"", "right double quote"},
      {0x201E, "\"", "double low-9 quote"},
      {0x201F, "\"", "double high-reversed-9 quote"},
      {0x00AB, "\"", "left guillemet"},
      {0x00BB, "\"", "right guillemet"},
      {0x2033, "\"", "double prime"},
      {0x2010, "-", "hyphen"},
      {0x2011, "-", "non-breaking hyphen"},
      {0x2012, "-", "figure dash"},
      {0x2013, "-", "en dash"},
      {0x2014, "-", "em dash"},
      {0x2015, "-", "horizontal bar"},
      {0x2212, "-", "minus sign"},
      {0x2500, "-", "box horizontal"},
      {0x2501, "-", "box heavy horizontal"},
      {0x2502, "|", "box vertical"},
      {0x2503, "|", "box heavy vertical"},
      {0x250C, "+", "box corner"},
      {0x2510, "+", "box corner"},
      {0x2514, "+", "box corner"},
      {0x2518, "+", "box corner"},
      {0x251C, "+", "box tee"},
      {0x2524, "+", "box tee"},
      {0x252C, "+", "box tee"},
      {0x2534, "+", "box tee"},
      {0x253C, "+", "box cross"},
      {0x254B, "+", "box heavy cross"},
      {0x2550, "-", "double horizontal"},
      {0x2551, "|", "double vertical"},
      {0x2554, "+", "double corner"},
      {0x2557, "+", "double corner"},
      {0x255A, "+", "double corner"},
      {0x255D, "+", "double corner"},
      {0x2560, "+", "double tee"},
      {0x2563, "+", "double tee"},
      {0x2566, "+", "double tee"},
      {0x2569, "+", "double tee"},
      {0x256C, "+", "double cross"},
      {0x007C, "|", "vertical bar"},
      {0x00A6, "|", "broken bar"},
      {0x2758, "|", "light vertical bar"},
      {0x2192, "->", "right arrow"},
      {0x21D2, "->", "right double arrow"},
      {0x2794, "->", "wide headed right arrow"},
      {0x2190, "<-", "left arrow"},
      {0x21D0, "<-", "left double arrow"},
      {0x2191, "^", "up arrow"},
      {0x21D1, "^", "up double arrow"},
      {0x2193, "v", "down arrow"},
      {0x21D3, "v", "down double arrow"},
      {0x2026, "...", "ellipsis"},
      {0x2022, "*", "bullet"},
      {0x00B7, ".", "middle dot"},
      {0x00D7, "x", "multiplication sign"},
      {0x00F7, "/", "division sign"},
      {0x00A0, " ", "non-breaking space"},
      {0x2000, " ", "en quad"},
      {0x2001, " ", "em quad"},
      {0x2002, " ", "en space"},
      {0x2003, " ", "em space"},
      {0x2004, " ", "three-per-em space"},
      {0x2005, " ", "four-per-em space"},
      {0x2006, " ", "six-per-em space"},
      {0x2007, " ", "figure space"},
      {0x2008, " ", "punctuation space"},
      {0x2009, " ", "thin space"},
      {0x200A, " ", "hair space"},
      {0x202F, " ", "narrow no-break space"},
      {0x205F, " ", "medium mathematical space"},
      {0x3000, " ", "ideographic space"},
      {0, NULL, NULL}};
  for (int i = 0; map[i].ch != 0; i++) {
    if (map[i].ch == ch)
      return &map[i];
  }
  return NULL;
}

/* Replace all typographic Unicode characters in src with ASCII equivalents.
 * ret: number of replacements made */
static int unicode_common_replace_all_in_text(const char *src, GString *out) {
  int count = 0;
  const char *p = src;
  while (*p) {
    gunichar ch = g_utf8_get_char(p);
    const UnicodeReplaceMap *m = unicode_common_replacement(ch);
    if (m) {
      g_string_append(out, m->repl);
      count++;
    } else {
      g_string_append_unichar(out, ch);
    }
    p = g_utf8_next_char(p);
  }
  return count;
}

/* Find the next Unicode character that has an ASCII replacement.
 * ret: 1 if found, 0 at end of string */
static int unicode_common_find_next(const char *src, gsize *byte_off,
                                    gunichar *out_ch,
                                    const UnicodeReplaceMap **out_map) {
  const char *p = src + *byte_off;
  while (*p) {
    gunichar ch = g_utf8_get_char(p);
    const UnicodeReplaceMap *m = unicode_common_replacement(ch);
    if (m) {
      *byte_off = (gsize)(p - src);
      if (out_ch)
        *out_ch = ch;
      if (out_map)
        *out_map = m;
      return 1;
    }
    p = g_utf8_next_char(p);
  }
  return 0;
}

/* Replace typographic Unicode in the active document, optionally with review.
 */
static void unicode_common_replace_buffer(AppState *s, gboolean review_mode) {
  if (!s || !s->buf)
    return;

  GtkTextIter start, end;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &start);
  gtk_text_buffer_get_end_iter(GTK_TEXT_BUFFER(s->buf), &end);
  char *text =
      gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &start, &end, FALSE);
  if (!text)
    return;

  if (!review_mode) {
    GString *out = g_string_sized_new(strlen(text) + 32);
    int count = unicode_common_replace_all_in_text(text, out);
    if (count > 0) {
      gtk_text_buffer_begin_user_action(GTK_TEXT_BUFFER(s->buf));
      gtk_text_buffer_set_text(GTK_TEXT_BUFFER(s->buf), out->str, -1);
      gtk_text_buffer_end_user_action(GTK_TEXT_BUFFER(s->buf));
      char msg[128];
      snprintf(msg, sizeof(msg), "Replaced %d Unicode character(s).", count);
      show_info(s, msg);
    } else {
      show_info(s, "No common Unicode characters found.");
    }
    g_string_free(out, TRUE);
    g_free(text);
    return;
  }

  GString *out = g_string_sized_new(strlen(text) + 32);
  gsize pos = 0;
  int replaced = 0;
  gboolean cancelled = FALSE;

  while (1) {
    gunichar ch = 0;
    const UnicodeReplaceMap *m = NULL;
    gsize hit = pos;
    if (!unicode_common_find_next(text, &hit, &ch, &m)) {
      g_string_append(out, text + pos);
      break;
    }

    g_string_append_len(out, text + pos, (gssize)(hit - pos));

    char ubuf[16];
    int ulen = g_unichar_to_utf8(ch, ubuf);
    ubuf[ulen] = '\0';

    char msg[512];
    snprintf(msg, sizeof(msg),
             "Replace U+%04X (%s) '%s' with '%s'?\n\n"
             "Yes = Replace\nNo = Skip\nCancel = Stop",
             (unsigned)ch, m->desc ? m->desc : "Unicode character", ubuf,
             m->repl);

    GtkWidget *dlg =
        gtk_message_dialog_new(s->win, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                               GTK_BUTTONS_NONE, "%s", msg);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Replace", GTK_RESPONSE_ACCEPT);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Skip", GTK_RESPONSE_REJECT);
    gtk_dialog_add_button(GTK_DIALOG(dlg), "Cancel", GTK_RESPONSE_CANCEL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    UnicodeDialogRunState dr = {0};
    dr.loop = g_main_loop_new(NULL, FALSE);
    dr.response = GTK_RESPONSE_CANCEL;
    g_signal_connect(dlg, "response", G_CALLBACK(unicode_dialog_response_quit),
                     &dr);
    gtk_widget_show(dlg);
    g_main_loop_run(dr.loop);
    int resp = dr.response;
    g_main_loop_unref(dr.loop);
    gtk_window_destroy(GTK_WINDOW(dlg));

    const char *next = g_utf8_next_char(text + hit);
    if (resp == GTK_RESPONSE_ACCEPT) {
      g_string_append(out, m->repl);
      replaced++;
    } else if (resp == GTK_RESPONSE_REJECT) {
      g_string_append_len(out, text + hit, (gssize)(next - (text + hit)));
    } else {
      g_string_append(out, text + hit);
      cancelled = TRUE;
      break;
    }
    pos = (gsize)(next - text);
  }

  if (replaced > 0) {
    gtk_text_buffer_begin_user_action(GTK_TEXT_BUFFER(s->buf));
    gtk_text_buffer_set_text(GTK_TEXT_BUFFER(s->buf), out->str, -1);
    gtk_text_buffer_end_user_action(GTK_TEXT_BUFFER(s->buf));
  }

  if (replaced > 0) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             cancelled ? "Stopped. Replaced %d Unicode character(s)."
                       : "Replaced %d Unicode character(s).",
             replaced);
    show_info(s, msg);
  } else if (cancelled) {
    show_info(s, "Unicode replacement cancelled.");
  } else {
    show_info(s, "No common Unicode characters found.");
  }

  g_string_free(out, TRUE);
  g_free(text);
}

/* Action handler: replace all typographic Unicode without confirmation. */
static void action_unicode_replace_all(GSimpleAction *a, GVariant *p,
                                       gpointer user_data) {
  (void)a;
  (void)p;
  unicode_common_replace_buffer((AppState *)user_data, FALSE);
}

static void action_unicode_replace_review(GSimpleAction *a, GVariant *p,
                                          gpointer user_data) {
  (void)a;
  (void)p;
  unicode_common_replace_buffer((AppState *)user_data, TRUE);
}

static GMenuModel *build_menubar_model(AppState *s) {
  GMenu *menubar = g_menu_new();

  /* ── File ─────────────────────────────────────────────── */
  {
    GMenu *file = g_menu_new();
    /* Document operations */
    GMenu *doc = g_menu_new();
    g_menu_append(doc, "New", "app.new");
    g_menu_append(doc, "Open...", "app.open");
    g_menu_append(doc, "Save", "app.save");
    g_menu_append(doc, "Save As...", "app.save_as");
    g_menu_append_section(file, NULL, G_MENU_MODEL(doc));
    g_object_unref(doc);
    /* Recent files section — content built at runtime */
    s->recent_menu = g_menu_new();
    g_menu_append(s->recent_menu, "(no recent files)", NULL);
    g_menu_append_submenu(file, "Open Recent", G_MENU_MODEL(s->recent_menu));
    /* File system */
    GMenu *fs = g_menu_new();
    g_menu_append(fs, "Show in File Manager", "app.show_in_files");
    g_menu_append_section(file, NULL, G_MENU_MODEL(fs));
    g_object_unref(fs);
    /* Quit */
    GMenu *quit_sec = g_menu_new();
    g_menu_append(quit_sec, "Quit", "app.quit");
    g_menu_append_section(file, NULL, G_MENU_MODEL(quit_sec));
    g_object_unref(quit_sec);
    g_menu_append_submenu(menubar, "File", G_MENU_MODEL(file));
    g_object_unref(file);
  }

  /* ── Edit ─────────────────────────────────────────────── */
  {
    GMenu *edit = g_menu_new();
    {
      GMenu *sec0 = g_menu_new();
      g_menu_append(sec0, "Undo", "app.undo");
      g_menu_append(sec0, "Redo", "app.redo");
      g_menu_append_section(edit, NULL, G_MENU_MODEL(sec0));
      g_object_unref(sec0);
      GMenu *sec1 = g_menu_new();
      g_menu_append(sec1, "Find...", "app.find");
      g_menu_append(sec1, "Replace...", "app.replace");
      g_menu_append(sec1, "Goto line...", "app.goto");
      g_menu_append_section(edit, NULL, G_MENU_MODEL(sec1));
      g_object_unref(sec1);
    }
    {
      GMenu *sec2 = g_menu_new();
      g_menu_append(sec2, "Remove duplicate lines", "app.dedupe_lines");
      g_menu_append(sec2, "Format layout", "app.format_layout");
      g_menu_append(sec2, "Replace common Unicode (all)",
                    "app.unicode_replace_all");
      g_menu_append(sec2, "Replace common Unicode (review...)",
                    "app.unicode_replace_review");
      {
        GMenu *fmt = g_menu_new();
        g_menu_append(fmt, "Upper case", "app.format_upper");
        g_menu_append(fmt, "Lower case", "app.format_lower");
        g_menu_append_submenu(sec2, "Mnemonic case formatter",
                              G_MENU_MODEL(fmt));
        g_object_unref(fmt);
      }
      g_menu_append_section(edit, NULL, G_MENU_MODEL(sec2));
      g_object_unref(sec2);
    }
    {
      GMenu *sec3 = g_menu_new();
      g_menu_append(sec3, "Toggle Bookmark", "app.bookmark_toggle");
      g_menu_append(sec3, "Next Bookmark", "app.bookmark_next");
      g_menu_append(sec3, "Prev Bookmark", "app.bookmark_prev");
      g_menu_append(sec3, "Clear All Bookmarks", "app.bookmark_clear");
      g_menu_append_section(edit, NULL, G_MENU_MODEL(sec3));
      g_object_unref(sec3);
    }
    {
      GMenu *sec4 = g_menu_new();
      g_menu_append(sec4, "Autocomplete", "app.autocomplete");
      g_menu_append_section(edit, NULL, G_MENU_MODEL(sec4));
      g_object_unref(sec4);
    }
    g_menu_append_submenu(menubar, "Edit", G_MENU_MODEL(edit));
    g_object_unref(edit);
  }

  /* ── View ─────────────────────────────────────────────── */
  {
    GMenu *view = g_menu_new();
    /* Display toggles */
    GMenu *view_tog = g_menu_new();
    g_menu_append(view_tog, "Word wrap", "app.word_wrap");
    g_menu_append(view_tog, "Problems panel", "app.problems_visible");
    g_menu_append_section(view, NULL, G_MENU_MODEL(view_tog));
    g_object_unref(view_tog);
    g_menu_append_submenu(menubar, "View", G_MENU_MODEL(view));
    g_object_unref(view);
  }

  /* ── Assembly ─────────────────────────────────────────── */
  {
    GMenu *asm_menu = g_menu_new();
    /* Assemble action */
    GMenu *asm_run = g_menu_new();
    g_menu_append(asm_run, "Check\tF5",    "app.check");
    g_menu_append(asm_run, "Assemble\tF9", "app.assemble");
#ifdef ED_BUILD_SIM
    {
      GMenuItem *sim_item = g_menu_item_new("Assemble & Emulate", "app.simulator");
      g_menu_item_set_attribute(sim_item, "accel", "s", "<Primary>e");
      g_menu_append_item(asm_run, sim_item);
      g_object_unref(sim_item);
    }
#endif
    g_menu_append_section(asm_menu, NULL, G_MENU_MODEL(asm_run));
    g_object_unref(asm_run);
    /* Assembler options */
    GMenu *asm_opts = g_menu_new();
    g_menu_append(asm_opts, "Allow undocumented opcodes", "app.allow_undoc");
    g_menu_append(asm_opts, "Clear memory on assemble", "app.asm_clear_mem");
    g_menu_append_section(asm_menu, NULL, G_MENU_MODEL(asm_opts));
    g_object_unref(asm_opts);
    /* Output file options */
    GMenu *asm_out = g_menu_new();
    {
      GMenu *exts = g_menu_new();
      g_menu_append(exts, "Source extension...", "app.set_src_ext");
      g_menu_append(exts, "Binary extension...", "app.set_bin_ext");
      g_menu_append_submenu(asm_out, "Default extensions", G_MENU_MODEL(exts));
      g_object_unref(exts);
    }
    g_menu_append_section(asm_menu, NULL, G_MENU_MODEL(asm_out));
    g_object_unref(asm_out);
    /* Assembly tools */
    GMenu *asm_tools = g_menu_new();
    g_menu_append(asm_tools, "Memory viewer...", "app.memory_viewer");
    g_menu_append(asm_tools, "Includes Manager...", "app.include_tree");
    g_menu_append_section(asm_menu, NULL, G_MENU_MODEL(asm_tools));
    g_object_unref(asm_tools);
    g_menu_append_submenu(menubar, "Assembly", G_MENU_MODEL(asm_menu));
    g_object_unref(asm_menu);
  }

  /* ── Preferences ──────────────────────────────────────── */
  {
    GMenu *prefs = g_menu_new();

    /* Session & appearance */
    GMenu *p_session = g_menu_new();
    g_menu_append(p_session, "Dark mode", "app.dark");
    g_menu_append(p_session, "Reopen files on startup", "app.reopen_last_file");
    g_menu_append_section(prefs, NULL, G_MENU_MODEL(p_session));
    g_object_unref(p_session);

    /* Editor */
    GMenu *p_editor = g_menu_new();
    g_menu_append(p_editor, "Auto indent", "app.auto_indent");
    g_menu_append(p_editor, "Tab width...", "app.tab_width");
    g_menu_append(p_editor, "Font...", "app.font");
    g_menu_append(p_editor, "Underline semantic errors", "app.error_underline");
    g_menu_append_section(prefs, NULL, G_MENU_MODEL(p_editor));
    g_object_unref(p_editor);

    /* Comments */
    GMenu *p_comments = g_menu_new();
    {
      GMenu *cm = g_menu_new();
      GMenuItem *mi;
      mi = g_menu_item_new("Semicolon (;)", "app.comment_marker");
      g_menu_item_set_attribute_value(mi, "target",
                                      g_variant_new_string("semicolon"));
      g_menu_append_item(cm, mi);
      g_object_unref(mi);
      mi = g_menu_item_new("Double-slash (//)", "app.comment_marker");
      g_menu_item_set_attribute_value(mi, "target",
                                      g_variant_new_string("slashslash"));
      g_menu_append_item(cm, mi);
      g_object_unref(mi);
      g_menu_append_submenu(p_comments, "Comment marker for CTRL+K+C/U",
                            G_MENU_MODEL(cm));
      g_object_unref(cm);
    }
    g_menu_append(p_comments, "Keep comment column aligned",
                  "app.keep_comment_aligned");
    g_menu_append(p_comments, "Comment align column...",
                  "app.comment_align_col");
    g_menu_append_section(prefs, NULL, G_MENU_MODEL(p_comments));
    g_object_unref(p_comments);

    /* Problems */
    GMenu *p_problems = g_menu_new();
    g_menu_append(p_problems, "Problems panel", "app.problems");
    g_menu_append_section(prefs, NULL, G_MENU_MODEL(p_problems));
    g_object_unref(p_problems);

    /* Folding */
    /* Fold submenu: Preferences > Fold > Comments / Macros */
    {
      GMenu *fold_sub = g_menu_new();
      g_menu_append(fold_sub, "Fold Comments", "app.fold_comments");
      g_menu_append(fold_sub, "Unfold Comments", "app.unfold_comments");
      g_menu_append(fold_sub, "Fold Macros", "app.fold_macros");
      g_menu_append(fold_sub, "Unfold Macros", "app.unfold_macros");
      g_menu_append_submenu(prefs, "Fold", G_MENU_MODEL(fold_sub));
      g_object_unref(fold_sub);
    }

    /* Config */
    GMenu *p_config = g_menu_new();
    g_menu_append(p_config, "Config Files...", "app.config_files");
    g_menu_append_section(prefs, NULL, G_MENU_MODEL(p_config));
    g_object_unref(p_config);

    g_menu_append_submenu(menubar, "Preferences", G_MENU_MODEL(prefs));
    g_object_unref(prefs);
  }

  /* ── Help ─────────────────────────────────────────────── */
  {
    GMenu *help = g_menu_new();
    g_menu_append(help, "Contents", "app.help");
    g_menu_append(help, "Instruction Reference", "app.instref");
    g_menu_append(help, "Directive Reference", "app.dref");
    g_menu_append(help, "About", "app.about");
    g_menu_append_submenu(menubar, "Help", G_MENU_MODEL(help));
    g_object_unref(help);
  }

  return G_MENU_MODEL(menubar);
}

static void add_action(GtkApplication *app, const char *name, GCallback cb,
                       AppState *s) {
  GSimpleAction *act = g_simple_action_new(name, NULL);
  g_signal_connect(act, "activate", cb, s);
  g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(act));
}

/* ---------------- Assemble (listing + optional .bin write) ---------------- */

typedef struct AssembleFlow {
  AppState *app;
  char *listing_text;

  // Assembler scratch output
  uint8_t *mem;
  uint8_t *written;
  uint16_t min_written, max_written;

  // Diagnostics from this assemble
  diags_t diags;

  // Filename context
  char *file;          // for diagnostics / asm api
  char *suggested_bin; // "<source>.bin" or NULL if unknown

  // chosen output path (for Save As...)
  char *out_bin_path;
  /* Memory-map popup data (populated on successful assemble) */
  uint8_t *written_snap;          /* 65536-byte snapshot of written[] */
  uint8_t *written_explicit_snap; /* written[] minus .res fill bytes   */
  uint32_t asm_bytes;    /* total bytes emitted */
  int range_count;
  struct {
    uint16_t start;
    uint16_t end;
    uint32_t count;
  } ranges[64];
} AssembleFlow;

static void assemble_flow_free(AssembleFlow *f) {
  if (!f)
    return;
  if (f->listing_text)
    g_free(f->listing_text);
  if (f->mem)
    g_free(f->mem);
  if (f->written)
    g_free(f->written);
  if (f->file)
    g_free(f->file);
  if (f->suggested_bin)
    g_free(f->suggested_bin);
  if (f->out_bin_path)
    g_free(f->out_bin_path);
  if (f->written_snap)
    g_free(f->written_snap);
  if (f->written_explicit_snap)
    g_free(f->written_explicit_snap);
  diags_free(&f->diags);
  g_free(f);
}

/* Parse a 16-bit unsigned integer from s, accepting decimal or hex with $ or &
 * prefix. ret: true on success */
bool parse_u16_hexish(const char *s, uint16_t *out) {
  if (!s || !*s)
    return false;

  uint32_t value = 0;
  int digits = 0;

  while (*s) {
    unsigned char c = (unsigned char)*s;
    int v = -1;

    if (c >= '0' && c <= '9')
      v = c - '0';
    else if (c >= 'A' && c <= 'F')
      v = c - 'A' + 10;
    else if (c >= 'a' && c <= 'f')
      v = c - 'a' + 10;
    else
      return false;

    value = (value << 4) | (uint32_t)v;
    digits++;
    if (digits > 4 || value > 0xFFFF)
      return false;

    s++;
  }

  if (digits < 1 || digits > 4)
    return false;

  *out = (uint16_t)value;
  return true;
}

// bool parse_u16_hexish(const char *s, uint16_t *out) {
//   if (!s || !*s)
//     return false;

//   /* Skip leading whitespace */
//   while (isspace((unsigned char)*s))
//     s++;

//   /* Optional prefixes */
//   if (s[0] == '$') {
//     s++;
//   } else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
//     s += 2;
//   }

//   uint32_t value = 0;
//   int digits = 0;

//   while (*s) {
//     char c = *s;
//     int v;

//     if (c >= '0' && c <= '9')
//       v = c - '0';
//     else if (c >= 'A' && c <= 'F')
//       v = c - 'A' + 10;
//     else if (c >= 'a' && c <= 'f')
//       v = c - 'a' + 10;
//     else
//       break;

//     value = (value << 4) | v;
//     if (value > 0xFFFF)
//       return false;

//     digits++;
//     s++;
//   }

/* Must have at least one digit and no trailing junk */
// if (digits == 0 || *s != '\0')
//   return false;

// *out = (uint16_t)value;
// return true;
// }

/* -------------------------------------------------------------------------- */
/* Hex-only filter for GtkEditable                                            */
/* -------------------------------------------------------------------------- */

// static void on_hex_entry_insert_text(GtkEditable *editable, const char *text,
//                                      int length, int *position,
//                                      gpointer user_data) {
//   char filtered[8];
//   int out = 0;

//   const char *old = gtk_editable_get_text(editable);
//   int old_len = old ? (int)strlen(old) : 0;

//   /* Current total must never exceed 4 chars */
//   int remaining = 4 - old_len;
//   if (remaining <= 0) {
//     g_signal_stop_emission_by_name(editable, "insert-text");
//     return;
//   }

//   for (int i = 0; i < length && out < remaining; i++) {
//     unsigned char c = (unsigned char)text[i];

//     if (c >= '0' && c <= '9') {
//       filtered[out++] = (char)c;
//     } else if (c >= 'a' && c <= 'f') {
//       filtered[out++] = (char)toupper(c);
//     } else if (c >= 'A' && c <= 'F') {
//       filtered[out++] = (char)c;
//     }
//   }

//   /* Block original insert */
//   g_signal_stop_emission_by_name(editable, "insert-text");

//   /* Reinsert filtered text, if any */
//   if (out > 0) {
//     filtered[out] = '\0';
//     gtk_editable_insert_text(editable, filtered, out, position);
//   }
// }

static void sanitize_hex_entry(GtkEditable *editable, gpointer user_data) {
  (void)user_data;

  const char *text = gtk_editable_get_text(editable);
  char filtered[5];
  int out = 0;

  for (int i = 0; text[i] && out < 4; i++) {
    unsigned char c = (unsigned char)text[i];

    if (c >= '0' && c <= '9')
      filtered[out++] = (char)c;
    else if (c >= 'a' && c <= 'f')
      filtered[out++] = (char)toupper(c);
    else if (c >= 'A' && c <= 'F')
      filtered[out++] = (char)c;
  }

  filtered[out] = '\0';

  if (strcmp(text, filtered) != 0) {
    int pos = gtk_editable_get_position(editable);
    if (pos > out)
      pos = out;

    g_signal_handlers_block_by_func(editable, G_CALLBACK(sanitize_hex_entry),
                                    user_data);

    gtk_editable_set_text(editable, filtered);
    gtk_editable_set_position(editable, pos);

    g_signal_handlers_unblock_by_func(editable, G_CALLBACK(sanitize_hex_entry),
                                      user_data);
  }
}

/* Input filter for hex address entry fields: allow only hex digits, cap at 4
 * chars. */
static void on_hex_entry_insert_text(GtkEditable *editable, const char *text,
                                     int length, int *position,
                                     gpointer user_data) {
  char filtered[8];
  int out = 0;

  int sel_start = 0, sel_end = 0;
  gboolean has_sel =
      gtk_editable_get_selection_bounds(editable, &sel_start, &sel_end);

  const char *old = gtk_editable_get_text(editable);
  int old_len = old ? (int)strlen(old) : 0;
  int sel_len = has_sel ? (sel_end - sel_start) : 0;

  int remaining = 4 - (old_len - sel_len);
  if (remaining <= 0) {
    g_signal_stop_emission_by_name(editable, "insert-text");
    return;
  }

  for (int i = 0; i < length && out < remaining; i++) {
    unsigned char c = (unsigned char)text[i];

    if (c >= '0' && c <= '9')
      filtered[out++] = (char)c;
    else if (c >= 'a' && c <= 'f')
      filtered[out++] = (char)toupper(c);
    else if (c >= 'A' && c <= 'F')
      filtered[out++] = (char)c;
  }

  g_signal_stop_emission_by_name(editable, "insert-text");

  if (out > 0) {
    filtered[out] = '\0';

    g_signal_handlers_block_by_func(
        editable, G_CALLBACK(on_hex_entry_insert_text), user_data);

    gtk_editable_insert_text(editable, filtered, out, position);

    g_signal_handlers_unblock_by_func(
        editable, G_CALLBACK(on_hex_entry_insert_text), user_data);
  }
}

/* Key handler for the assembly listing view. */
static gboolean listing_key_pressed(GtkEventControllerKey *ctl, guint keyval,
                                    guint keycode, GdkModifierType state,
                                    gpointer user_data) {
  (void)ctl;
  (void)keycode;
  (void)state;
  // Allow keyboard-only dismissal: Esc or Enter (same as clicking OK).
  if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_Return ||
      keyval == GDK_KEY_KP_Enter) {
    gtk_window_destroy(GTK_WINDOW(user_data));
    return TRUE;
  }
  return FALSE;
}

static void listing_ok_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  gtk_window_destroy(GTK_WINDOW(user_data));
}

/* Get the project directory from the active file path.
 * Returns a heap-allocated string — caller must g_free(). */
static char *get_project_dir(AppState *s) {
    if (s->path && s->path[0]) {
        return g_path_get_dirname(s->path);
    }
    return NULL;
}

static void apply_embedded_profile_override(AppState *s, simcfg_t *cfg) {
  const char *src_text = NULL;
  size_t src_len = 0;
  if (s->buf) {
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(tb, &start, &end);
    src_text = gtk_text_buffer_get_text(tb, &start, &end, FALSE);
    src_len = src_text ? strlen(src_text) : 0;
  }
  if ((!src_text || src_len == 0) && s->path && s->path[0]) {
    FILE *sf = fopen(s->path, "r");
    if (sf) {
      fseek(sf, 0, SEEK_END); long fsz = ftell(sf); rewind(sf);
      if (fsz > 0) {
        char *fbuf = g_malloc(fsz + 1);
        if (fbuf) {
          size_t rd = fread(fbuf, 1, fsz, sf);
          fbuf[rd] = '\0';
          if (src_text) g_free((gpointer)src_text);
          src_text = fbuf; src_len = rd;
        }
      }
      fclose(sf);
    }
  }
  if (!src_text || src_len == 0) return;
  const char *tag_open  = ";[profile]",  *tag_open2  = "; [profile]";
  const char *tag_close = ";[/profile]", *tag_close2 = "; [/profile]";
  const char *open_pos = strstr(src_text, tag_open);
  if (!open_pos) open_pos = strstr(src_text, tag_open2);
  const char *close_pos = NULL;
  if (open_pos) {
    close_pos = strstr(open_pos, tag_close);
    if (!close_pos) close_pos = strstr(open_pos, tag_close2);
  }
  if (open_pos && close_pos && close_pos > open_pos) {
    int tlen = (strncmp(open_pos, tag_open2, strlen(tag_open2)) == 0)
               ? (int)strlen(tag_open2) : (int)strlen(tag_open);
    const char *block = open_pos + tlen;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s/.6502caed_embedded.simprofile", g_get_home_dir());
    simprofile_t base; simprofile_defaults(&base);
    if (cfg->last_profile[0]) {
      simprofile_load(&base, cfg->last_profile);
    } else {
      simprofile_t *found = simprofile_find_default(
          cfg->project_dir[0] ? cfg->project_dir : NULL);
      if (found) { base = *found; free(found); }
    }
    snprintf(base.filepath, sizeof(base.filepath), "%s", tmp);
    simprofile_save_as(&base, tmp);
    FILE *f = fopen(tmp, "a");
    if (f) {
      fprintf(f, "\n# embedded overrides\n");
      const char *p = block;
      while (p < close_pos) {
        const char *lend = strchr(p, '\n');
        if (!lend || lend > close_pos) lend = close_pos;
        const char *lp = p;
        while (*lp == ' ' || *lp == '\t') lp++;
        if (*lp == ';') {
          lp++;
          if (*lp == ' ') lp++;
          int n = (int)(lend - lp); n = n < 511 ? n : 511;
          if (n > 0) {
            char line[512]; memcpy(line, lp, n); line[n] = '\0';
            char *lt = line; while (*lt == ' ') lt++;
            if (*lt && *lt != '#' && *lt != '\r') fprintf(f, "%s\n", line);
          }
        }
        p = lend + 1;
      }
      fclose(f);
      snprintf(cfg->override_profile, sizeof(cfg->override_profile), "%s", tmp);
    }
  }
  g_free((gpointer)src_text);
}

#ifdef ED_BUILD_SIM
static void action_simulator(GSimpleAction *a, GVariant *p, gpointer user_data) {
  (void)a; (void)p;
  AppState *s = (AppState *)user_data;
  simcfg_t cfg;
  simcfg_defaults(&cfg);
  char cfgpath[1024];
  snprintf(cfgpath, sizeof(cfgpath), "%s/sim.cfg", g_get_home_dir());
  simcfg_load(&cfg, cfgpath);

  /* Set CPU type from last assembly */
  if (s->mem_cpu == ASM_CPU_65816)      cfg.cpu_type = CPU_65816;
  else if (s->mem_cpu == ASM_CPU_65C02) cfg.cpu_type = CPU_65C02;
  else                                   cfg.cpu_type = CPU_6502;

  /* Write mem_image to temp file */
  if (s->mem_valid && s->mem_image && s->mem_asm_valid) {
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.6502caed_sim_tmp.bin",
             g_get_home_dir());
    FILE *tf = fopen(tmp_path, "wb");
    if (tf) {
      fwrite(s->mem_image, 1, 65536, tf);
      fclose(tf);
      snprintf(cfg.roms[0].path, 512, "%s", tmp_path);
      cfg.roms[0].load_addr = 0x0000;
      cfg.roms[0].enabled   = TRUE;
    }
  }
  /* Set project_dir in cfg so sim_win_create finds the right profile */
  {
    char *pdir = get_project_dir(s);
    if (pdir) {
      snprintf(cfg.project_dir, sizeof(cfg.project_dir), "%s", pdir);
      g_free(pdir);
    }
  }
  apply_embedded_profile_override(s, &cfg);

  sim_win_t *simwin = sim_win_create(s->win, &cfg, s->dark_mode);
  if (simwin && s->mem_valid && s->mem_image) {
    GBytes *snap = g_bytes_new(s->mem_image, 65536);
    g_object_set_data_full(G_OBJECT(sim_win_get_window(simwin)),
                           "mem_snapshot", snap,
                           (GDestroyNotify)g_bytes_unref);
    sim_win_set_app_state(simwin, s);
    /* Track active sim window on app for snapshot updates */
    g_object_set_data(G_OBJECT(s->app), "active_sim_window",
                      sim_win_get_window(simwin));
    /* Pass project directory for profile discovery */
    char *pdir = get_project_dir(s);
    if (pdir) {
      sim_win_set_project_dir(simwin, pdir);
      g_free(pdir);
    }
  }
}
static void listing_emulate_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  GtkWidget *win = GTK_WIDGET(user_data);
  AppState *s = (AppState *)g_object_get_data(G_OBJECT(win), "app_state");
  gtk_window_destroy(GTK_WINDOW(win));
  if (!s) return;

  /* Write mem_image to temp file and open simulator */
  simcfg_t cfg;
  simcfg_defaults(&cfg);
  char cfgpath[1024];
  snprintf(cfgpath, sizeof(cfgpath), "%s/sim.cfg", g_get_home_dir());
  simcfg_load(&cfg, cfgpath);

  /* Set CPU type from last assembly .cpu directive */
  if (s->mem_cpu == ASM_CPU_65816)
    cfg.cpu_type = CPU_65816;
  else if (s->mem_cpu == ASM_CPU_65C02)
    cfg.cpu_type = CPU_65C02;
  else
    cfg.cpu_type = CPU_6502;

  if (s->mem_valid && s->mem_image && s->mem_asm_valid) {
    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.6502caed_sim_tmp.bin",
             g_get_home_dir());
    FILE *tf = fopen(tmp_path, "wb");
    if (tf) {
      fwrite(s->mem_image, 1, 65536, tf);
      fclose(tf);
      snprintf(cfg.roms[0].path, 512, "%s", tmp_path);
      cfg.roms[0].load_addr = 0x0000;
      cfg.roms[0].enabled   = TRUE;
    }
  }
  /* Set project_dir in cfg so sim_win_create finds the right profile */
  {
    char *pdir = get_project_dir(s);
    if (pdir) {
      snprintf(cfg.project_dir, sizeof(cfg.project_dir), "%s", pdir);
      g_free(pdir);
    }
  }
  apply_embedded_profile_override(s, &cfg);
  sim_win_t *simwin = sim_win_create(s->win, &cfg, s->dark_mode);
  /* Store memory snapshot on simulator window for Reload ROM button */
  if (simwin && s->mem_valid && s->mem_image) {
    GBytes *snap = g_bytes_new(s->mem_image, 65536);
    g_object_set_data_full(G_OBJECT(sim_win_get_window(simwin)),
                           "mem_snapshot", snap,
                           (GDestroyNotify)g_bytes_unref);
    sim_win_set_app_state(simwin, s);
    g_object_set_data(G_OBJECT(s->app), "active_sim_window",
                      sim_win_get_window(simwin));
    /* Pass project directory for profile discovery */
    char *pdir = get_project_dir(s);
    if (pdir) {
      sim_win_set_project_dir(simwin, pdir);
      g_free(pdir);
    }
  }
}
#endif

// Forward-declared because the listing window can trigger it.
static void assemble_offer_save_binary(AssembleFlow *f);

static void listing_save_binary_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  GtkWidget *win = GTK_WIDGET(user_data);
  AssembleFlow *f =
      (AssembleFlow *)g_object_get_data(G_OBJECT(win), "assemble_flow");
  if (!f)
    return;

  // The save flow will free 'f' when it completes/cancels. Make sure
  // the window doesn't also free it on close.
  g_object_set_data(G_OBJECT(win), "assemble_flow", NULL);
  assemble_offer_save_binary(f);
}

static gboolean listing_close_request_cb(GtkWindow *win, gpointer user_data) {
  (void)user_data;
  AssembleFlow *f =
      (AssembleFlow *)g_object_get_data(G_OBJECT(win), "assemble_flow");
  if (f) {
    g_object_set_data(G_OBJECT(win), "assemble_flow", NULL);
    assemble_flow_free(f);
  }
  return FALSE; // allow the window to close
}

static void listing_copy_clicked(GtkButton *b, gpointer user_data) {
  (void)b;
  GtkWidget *w = GTK_WIDGET(user_data);
  const char *t =
      (const char *)g_object_get_data(G_OBJECT(w), "asm_output_text");
  if (!t)
    t = "";
  GdkClipboard *cb = gtk_widget_get_clipboard(w);
  if (cb)
    gdk_clipboard_set_text(cb, t);
}

/* ─── Memory-map forward declaration ─────────────────────────────────────── */
static void show_memmap_popup(GtkWindow *parent, AssembleFlow *flow,
                              gboolean dark_mode); /* fwd */

/* ─── listing_memmap_clicked callback ───────────────────────────────────── */
static void listing_memmap_clicked(GtkButton *b, gpointer user_data) {
  (void)user_data;
  GtkWidget *btn = GTK_WIDGET(b);
  AssembleFlow *flow =
      (AssembleFlow *)g_object_get_data(G_OBJECT(btn), "assemble_flow");
  AppState *s = (AppState *)g_object_get_data(G_OBJECT(btn), "app_state");
  if (!flow || !s)
    return;
  show_memmap_popup(GTK_WINDOW(s->win), flow, s->dark_mode);
}

/* ─── Heatmap drawing context ────────────────────────────────────────────── */
typedef struct {
  uint8_t *written_snap; /* borrowed from AssembleFlow — not owned */
  int range_count;
  struct {
    uint16_t start;
    uint16_t end;
    uint32_t count;
  } ranges[64];
  int scroll_to_page; /* high byte of first written address, -1 = none */
} HeatmapCtx;

/* ─── Cairo draw callback: 256x256 grid, one pixel per byte ─────────────── */
/* Y-axis label strip: 40px wide, 1024px tall, draws &0000/&1000/.../&F000 */
static void heatmap_yaxis_draw_cb(GtkDrawingArea *da, cairo_t *cr, int width,
                                  int height, gpointer user_data) {
  (void)da;
  (void)user_data;
  double ch = (double)height / 256.0; /* row height — matches heatmap */

  /* Background matching heatmap */
  cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
  cairo_paint(cr);

  cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                         CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 9.0);

  /* Label every 16 rows = &1000 boundary */
  for (int pg = 0; pg < 256; pg += 16) {
    char buf[8];
    g_snprintf(buf, sizeof(buf), "&%02X00", pg);
    double y = pg * ch;
    /* Tick mark */
    cairo_set_source_rgba(cr, 0.45, 0.55, 0.65, 1.0);
    cairo_set_line_width(cr, 0.5);
    cairo_move_to(cr, width - 4, y);
    cairo_line_to(cr, width, y);
    cairo_stroke(cr);
    /* Label — place just below the tick, but clamp so &0000 stays visible */
    cairo_set_source_rgba(cr, 0.65, 0.75, 0.85, 1.0);
    double label_y = (pg == 0) ? y + 9.0 : y + 3.0;
    cairo_move_to(cr, 1.0, label_y);
    cairo_show_text(cr, buf);
  }
}

static void heatmap_draw_cb(GtkDrawingArea *da, cairo_t *cr, int width,
                            int height, gpointer user_data) {
  (void)da;
  HeatmapCtx *ctx = (HeatmapCtx *)user_data;
  if (!ctx || !ctx->written_snap)
    return;

  /* 2px per byte cell: 256 cols x 256 rows = 512x512 natural size */
  double cw = (double)width / 256.0;
  double ch = (double)height / 256.0;

  /* Dark background */
  cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
  cairo_paint(cr);

  /* Subtle grid lines every 16 rows (&1000 boundaries) */
  cairo_set_source_rgba(cr, 0.22, 0.22, 0.26, 1.0);
  cairo_set_line_width(cr, 0.5);
  for (int pg = 16; pg < 256; pg += 16) {
    cairo_move_to(cr, 0, pg * ch);
    cairo_line_to(cr, 256.0 * cw, pg * ch);
  }
  cairo_stroke(cr);

  /* Blue cell for every written byte */
  for (int row = 0; row < 256; row++) {
    for (int col = 0; col < 256; col++) {
      if (!ctx->written_snap[row * 256 + col])
        continue;
      cairo_set_source_rgb(cr, 0.10, 0.45, 0.95);
      cairo_rectangle(cr, col * cw, row * ch, cw, ch);
      cairo_fill(cr);
    }
  }

  /* Range outlines */
  cairo_set_source_rgba(cr, 0.75, 0.90, 1.0, 0.8);
  cairo_set_line_width(cr, 1.5);
  for (int i = 0; i < ctx->range_count; i++) {
    int rs = ctx->ranges[i].start, re = ctx->ranges[i].end;
    double x1 = (rs % 256) * cw, y1 = (rs / 256) * ch;
    double x2 = (re % 256) * cw + cw, y2 = (re / 256) * ch + ch;
    if (rs / 256 == re / 256) {
      cairo_rectangle(cr, x1, y1, x2 - x1, ch);
      cairo_stroke(cr);
    } else {
      cairo_move_to(cr, x1, y1);
      cairo_line_to(cr, 256.0 * cw, y1);
      cairo_move_to(cr, x1, y1 + ch);
      cairo_line_to(cr, 256.0 * cw, y1 + ch);
      cairo_move_to(cr, 0, y2 - ch);
      cairo_line_to(cr, x2, y2 - ch);
      cairo_move_to(cr, 0, y2);
      cairo_line_to(cr, x2, y2);
      cairo_stroke(cr);
    }
  }
}

/* ─── Memory-map popup window ────────────────────────────────────────────── */
/* Idle callback: runs after the first frame, sets scroll position.
 * user_data is the GtkScrolledWindow for the heatmap. */
static gboolean heatmap_scroll_idle(gpointer user_data) {
  GtkWidget *da_sc = GTK_WIDGET(user_data);
  gint target_page =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(da_sc), "scroll_to_page"));
  if (target_page < 0)
    return G_SOURCE_REMOVE;
  GtkAdjustment *vadj =
      gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(da_sc));
  if (!vadj)
    return G_SOURCE_REMOVE;
  double upper = gtk_adjustment_get_upper(vadj);
  if (upper <= 1.0)
    return G_SOURCE_CONTINUE; /* not ready yet, retry */
  /* 1024px / 256 pages = 4px per page; show target with 8-page top margin */
  double page_h = upper / 256.0;
  double target = (target_page - 8) * page_h;
  if (target < 0.0)
    target = 0.0;
  if (target > upper - gtk_adjustment_get_page_size(vadj))
    target = upper - gtk_adjustment_get_page_size(vadj);
  gtk_adjustment_set_value(vadj, target);
  return G_SOURCE_REMOVE; /* run once */
}

static void show_memmap_popup(GtkWindow *parent, AssembleFlow *flow,
                              gboolean dark_mode) {
  if (!flow || !flow->written_snap)
    return;

  GtkWidget *win = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(win), parent);
  gtk_window_set_modal(GTK_WINDOW(win), FALSE);
  gtk_window_set_title(GTK_WINDOW(win), "Memory Map");
  gtk_window_set_default_size(GTK_WINDOW(win), 560, 480);
  apply_window_dark_hint(win, dark_mode);

  /* Borrow written_explicit_snap (instructions + data, no .res fills) for the
     heatmap — falls back to written_snap if explicit tracking wasn't available */
  HeatmapCtx *ctx = g_new0(HeatmapCtx, 1);
  ctx->written_snap = flow->written_explicit_snap
                          ? flow->written_explicit_snap
                          : flow->written_snap;
  ctx->range_count = flow->range_count < 64 ? flow->range_count : 64;
  /* First written page for auto-scroll */
  ctx->scroll_to_page =
      (flow->range_count > 0) ? (flow->ranges[0].start >> 8) : -1;
  for (int i = 0; i < ctx->range_count; i++) {
    ctx->ranges[i].start = flow->ranges[i].start;
    ctx->ranges[i].end = flow->ranges[i].end;
    ctx->ranges[i].count = flow->ranges[i].count;
  }
  g_object_set_data_full(G_OBJECT(win), "heatmap_ctx", ctx, g_free);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_top(vbox, 8);
  gtk_widget_set_margin_bottom(vbox, 8);
  gtk_widget_set_margin_start(vbox, 8);
  gtk_widget_set_margin_end(vbox, 8);

  /* Legend label */
  GtkWidget *lbl =
      gtk_label_new("64KB address space.  Each row = one page (256 bytes)."
                    "  Blue = bytes written by assembler.");
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);

  /* Y-axis label strip (40px wide x 1024px tall) */
  GtkWidget *yaxis = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(yaxis), 40);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(yaxis), 1024);
  gtk_widget_set_hexpand(yaxis, FALSE);
  gtk_widget_set_vexpand(yaxis, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(yaxis), heatmap_yaxis_draw_cb,
                                 NULL, NULL);

  /* Heatmap drawing area: 4px per byte = 1024x1024 */
  GtkWidget *da = gtk_drawing_area_new();
  gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), 1024);
  gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), 1024);
  gtk_widget_set_hexpand(da, TRUE);
  gtk_widget_set_vexpand(da, FALSE);
  gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), heatmap_draw_cb, ctx,
                                 NULL);

  /* Side-by-side: Y-axis strip + heatmap, both scroll together */
  GtkWidget *hmap_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_append(GTK_BOX(hmap_row), yaxis);
  gtk_box_append(GTK_BOX(hmap_row), da);

  /* Wrap in a scrolled window with fixed height */
  GtkWidget *da_sc = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(da_sc), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(da_sc, -1, 220);
  gtk_widget_set_hexpand(da_sc, TRUE);
  gtk_widget_set_vexpand(da_sc, FALSE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(da_sc), hmap_row);
  /* Store scroll target on the widget and schedule an idle-time scroll */
  g_object_set_data(G_OBJECT(da_sc), "scroll_to_page",
                    GINT_TO_POINTER(ctx->scroll_to_page));
  g_idle_add(heatmap_scroll_idle, da_sc);

  /* Scrollable range table */
  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(sc, -1, 140);
  gtk_widget_set_vexpand(sc, TRUE);

  GtkWidget *rtv = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(rtv), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(rtv), TRUE);
  gtk_widget_set_hexpand(rtv, TRUE);

  GtkTextBuffer *rtb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(rtv));
  GString *rs = g_string_new(NULL);
  g_string_append_printf(rs, "  %-16s  %-16s  %s\n", "Start", "End", "Bytes");
  g_string_append(rs, "  ----------------------------------------\n");
  uint32_t total = 0;
  for (int i = 0; i < flow->range_count; i++) {
    g_string_append_printf(
        rs, "  &%04X            &%04X            %u byte%s\n",
        flow->ranges[i].start, flow->ranges[i].end, flow->ranges[i].count,
        flow->ranges[i].count == 1 ? "" : "s");
    total += flow->ranges[i].count;
  }
  g_string_append(rs, "  ----------------------------------------\n");
  g_string_append_printf(
      rs, "  Total                             %u byte%s in %d range%s\n",
      total, total == 1 ? "" : "s", flow->range_count,
      flow->range_count == 1 ? "" : "s");
  gtk_text_buffer_set_text(rtb, rs->str, -1);
  g_string_free(rs, TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), rtv);

  /* Close button */
  GtkWidget *btn_close = gtk_button_new_with_label("Close");
  g_signal_connect_swapped(btn_close, "clicked", G_CALLBACK(gtk_window_destroy),
                           win);
  GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(btn_row), btn_close);

  gtk_box_append(GTK_BOX(vbox), lbl);
  gtk_box_append(GTK_BOX(vbox), da_sc);
  gtk_box_append(GTK_BOX(vbox), sc);
  gtk_box_append(GTK_BOX(vbox), btn_row);
  gtk_window_set_child(GTK_WINDOW(win), vbox);
  gtk_window_present(GTK_WINDOW(win));
}

static void show_listing_window(AppState *s, const char *title,
                                const char *text, AssembleFlow *flow) {
  GtkWidget *win = gtk_window_new();
  gtk_window_set_transient_for(GTK_WINDOW(win), s->win);
  gtk_window_set_default_size(GTK_WINDOW(win), 900, 650);
  gtk_window_set_title(GTK_WINDOW(win), title);
  apply_window_dark_hint(win, s->dark_mode);

  // Stash the output text on the window so callbacks can access it.
  g_object_set_data_full(G_OBJECT(win), "asm_output_text",
                         g_strdup(text ? text : ""), g_free);

  // Optionally stash the assembled memory context so the listing window can
  // launch the Save Binary flow on demand.
  g_object_set_data(G_OBJECT(win), "assemble_flow", flow);
  g_object_set_data(G_OBJECT(win), "app_state", s);
  g_signal_connect(win, "close-request", G_CALLBACK(listing_close_request_cb),
                   NULL);

  // Close on Escape.
  GtkEventController *keys = gtk_event_controller_key_new();
  g_signal_connect(keys, "key-pressed", G_CALLBACK(listing_key_pressed), win);
  gtk_widget_add_controller(win, keys);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_AUTOMATIC,
                                 GTK_POLICY_AUTOMATIC);
  // Expand to fill the dialog so the listing isn't clipped to a few lines.
  gtk_widget_set_hexpand(sc, TRUE);
  gtk_widget_set_vexpand(sc, TRUE);

  GtkWidget *tv = gtk_text_view_new();
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_NONE);
  gtk_widget_set_hexpand(tv, TRUE);
  gtk_widget_set_vexpand(tv, TRUE);

  GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
  gtk_text_buffer_set_text(tb, text ? text : "", -1);

  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);

  // Buttons row
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(buttons, 8);
  gtk_widget_set_margin_bottom(buttons, 8);
  gtk_widget_set_margin_start(buttons, 8);
  gtk_widget_set_margin_end(buttons, 8);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);

  GtkWidget *btn_save = NULL;
  GtkWidget *btn_memmap = NULL;
  if (flow) {
    btn_save = gtk_button_new_with_label("Save Binary");
    g_signal_connect(btn_save, "clicked",
                     G_CALLBACK(listing_save_binary_clicked), win);
    if (flow->written_snap && flow->range_count > 0) {
      btn_memmap = gtk_button_new_with_label("Memory Map");
      g_object_set_data(G_OBJECT(btn_memmap), "assemble_flow", flow);
      g_object_set_data(G_OBJECT(btn_memmap), "app_state", s);
      g_signal_connect(btn_memmap, "clicked",
                       G_CALLBACK(listing_memmap_clicked), win);
    }
  }

  GtkWidget *btn_copy = gtk_button_new_with_label("Copy to Clipboard");
  GtkWidget *btn_ok = gtk_button_new_with_label("OK");

  g_signal_connect(btn_ok, "clicked", G_CALLBACK(listing_ok_clicked), win);
  g_signal_connect(btn_copy, "clicked", G_CALLBACK(listing_copy_clicked), win);

  if (btn_save)
    gtk_box_append(GTK_BOX(buttons), btn_save);
  if (btn_memmap)
    gtk_box_append(GTK_BOX(buttons), btn_memmap);
#ifdef ED_BUILD_SIM
  if (flow) {
    /* Emulate button — only shown after a successful assembly */
    GtkWidget *btn_emu = gtk_button_new_with_label("▶  Emulate");
    g_signal_connect(btn_emu, "clicked",
                     G_CALLBACK(listing_emulate_clicked), win);
    gtk_box_append(GTK_BOX(buttons), btn_emu);
  }
#endif
  gtk_box_append(GTK_BOX(buttons), btn_copy);
  gtk_box_append(GTK_BOX(buttons), btn_ok);

  gtk_box_append(GTK_BOX(root), sc);
  gtk_box_append(GTK_BOX(root), buttons);
  gtk_window_set_child(GTK_WINDOW(win), root);
  gtk_window_present(GTK_WINDOW(win));
}

static char *read_entire_file(const char *path) {
  if (!path)
    return NULL;
  GError *err = NULL;
  char *content = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &content, &len, &err)) {
    if (err)
      g_error_free(err);
    return NULL;
  }
  return content; // g_free by caller
}

// Capture everything written to stdout while running fn(ctx).
// Returns newly allocated string (g_malloc) containing captured bytes.
typedef void (*StdoutCaptureFn)(void *ctx);

static char *capture_stdout_run(StdoutCaptureFn fn, void *ctx) {
  if (!fn)
    return NULL;

#ifdef _WIN32
  /* On Windows GUI processes, stdout FILE* is not connected to any console.
   * Redirect the C runtime stdout FILE* to a temp file to capture output. */
  char tmp_path[MAX_PATH];
  const char *tmp_dir = getenv("TEMP");
  if (!tmp_dir) tmp_dir = getenv("TMP");
  if (!tmp_dir) tmp_dir = "C:\\Temp";
  if (GetTempFileNameA(tmp_dir, "asm", 0, tmp_path) == 0)
    return NULL;

  /* Redirect C runtime stdout to temp file */
  FILE *old_stdout = stdout;
  FILE *tmp_f = freopen(tmp_path, "w+b", stdout);
  if (!tmp_f) { DeleteFileA(tmp_path); return NULL; }

  fn(ctx);
  fflush(stdout);

  /* Restore stdout — reopen NUL device */
  freopen("NUL", "w", stdout);

  /* Read captured output from temp file */
  tmp_f = fopen(tmp_path, "rb");
  GString *out = g_string_new(NULL);
  if (tmp_f) {
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), tmp_f)) > 0)
      g_string_append_len(out, buf, (gsize)n);
    fclose(tmp_f);
  }
  DeleteFileA(tmp_path);
  return g_string_free(out, FALSE);

#else
  /* A pipe cannot be drained after fn(): a large listing fills it and
   * blocks the writer on this same thread. Capture to a seekable file. */
  FILE *capture = tmpfile();
  if (!capture)
    return NULL;

  fflush(stdout);
  int saved = dup(STDOUT_FILENO);
  if (saved < 0) {
    fclose(capture);
    return NULL;
  }
  if (dup2(fileno(capture), STDOUT_FILENO) < 0) {
    close(saved);
    fclose(capture);
    return NULL;
  }

  fn(ctx);
  fflush(stdout);

  /* Restore stdout before reading or closing the temporary file. */
  int restored = dup2(saved, STDOUT_FILENO);
  close(saved);
  if (restored < 0 || fseek(capture, 0, SEEK_SET) != 0) {
    fclose(capture);
    return NULL;
  }

  GString *out = g_string_new(NULL);
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), capture)) > 0)
    g_string_append_len(out, buf, (gsize)n);
  if (ferror(capture)) {
    fclose(capture);
    g_string_free(out, TRUE);
    return NULL;
  }
  fclose(capture);
  return g_string_free(out, FALSE);
#endif
}

typedef struct {
  const char *file;
  const char *text;
  asm_settings_t *st;
  uint8_t *mem;
  uint8_t *written;
  uint8_t *written_explicit; /* only set for non-.res emissions (memory map) */
  gboolean clear_mem;
  uint16_t *min_written;
  uint16_t *max_written;
  uint16_t *out_start;
  uint16_t *out_end;
  diags_t *diags;
} AssembleCaptureCtx;

/* Assembly thread function: runs the assembler pipeline and stores the result.
 */
static void assemble_capture_fn(void *ud) {
  AssembleCaptureCtx *c = (AssembleCaptureCtx *)ud;
  if (!c)
    return;
  if (c->clear_mem && c->mem && c->written) {
    memset(c->mem, 0, 65536);
    memset(c->written, 0, 65536);
  }
  (void)asm_assemble(c->file, c->text, c->st, NULL, NULL, c->mem, c->written,
                     c->written_explicit,
                     c->min_written, c->max_written, c->out_start, c->out_end,
                     c->diags);
}

static char *make_suggested_bin_path(const char *src_path,
                                     const char *bin_ext) {
  if (!src_path || !*src_path)
    return NULL;
  if (!bin_ext || !bin_ext[0])
    bin_ext = ".bin";
  char *dir = g_path_get_dirname(src_path);
  char *base = g_path_get_basename(src_path);
  char *dot = strrchr(base, '.');
  if (dot)
    *dot = 0;
  char *binbase = g_strconcat(base, bin_ext, NULL);
  char *out = g_build_filename(dir, binbase, NULL);
  g_free(dir);
  g_free(base);
  g_free(binbase);
  return out;
}

/* Write mem[start..end_inclusive] to path.
 * ret: TRUE on success, FALSE with *err set on failure */
static gboolean write_bin_range(const char *path, const uint8_t *mem,
                                uint16_t start, uint16_t end_inclusive,
                                GError **err) {
  if (!path || !mem)
    return FALSE;
  FILE *f = fopen(path, "wb");
  if (!f) {
    if (err)
      *err = g_error_new(g_quark_from_static_string("6502caed"), 1,
                         "Failed to open output file");
    return FALSE;
  }
  size_t count = (size_t)((uint32_t)end_inclusive - (uint32_t)start + 1u);
  if (fwrite(mem + start, 1, count, f) != count) {
    fclose(f);
    if (err)
      *err = g_error_new(g_quark_from_static_string("6502caed"), 2,
                         "Failed to write output file");
    return FALSE;
  }
  fclose(f);
  return TRUE;
}

/* Show a modal error dialog with msg. */
static void show_error(AppState *s, const char *msg) {
  GtkWidget *d =
      gtk_message_dialog_new(s->win, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR,
                             GTK_BUTTONS_OK, "%s", msg ? msg : "Error");
  g_signal_connect(d, "response", G_CALLBACK(gtk_window_destroy), NULL);
  gtk_window_present(GTK_WINDOW(d));
}

/* Show a modal informational dialog with msg. */
static void show_info(AppState *s, const char *msg) {
  GtkWidget *d =
      gtk_message_dialog_new(s->win, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                             GTK_BUTTONS_OK, "%s", msg ? msg : "OK");
  g_signal_connect(d, "response", G_CALLBACK(gtk_window_destroy), NULL);
  gtk_window_present(GTK_WINDOW(d));
}

/* ---- Stage 2: range prompt ---- */

typedef enum {
  BIN_RANGE_ASSEMBLED = 1001,
  BIN_RANGE_SPECIFIED = 1002,
  BIN_RANGE_FULL = 1003,
  BIN_RANGE_16K = 1004
} BinRangeChoice;

static void specified_range_response(GtkDialog *dlg, int resp, gpointer ud);

static void range_choice_response(GtkDialog *dlg, int resp, gpointer ud) {
  AssembleFlow *f = (AssembleFlow *)ud;
  gtk_window_destroy(GTK_WINDOW(dlg));

  if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
    assemble_flow_free(f);
    return;
  }

  if (!f->out_bin_path) {
    show_error(f->app, "No output path selected.");
    assemble_flow_free(f);
    return;
  }

  uint16_t start = 0, end = 0xFFFF;

  if (resp == BIN_RANGE_FULL) {
    start = 0;
    end = 0xFFFF;
  } else if (resp == BIN_RANGE_16K) {
    /* 16KB from assembly start address */
    start = f->min_written;
    end = (uint16_t)(start + 0x3FFF);
    /* clamp to address space */
    if (end < start)
      end = 0xFFFF;
  } else if (resp == BIN_RANGE_ASSEMBLED) {
    start = f->min_written;
    end = f->max_written;
    if (end < start) {
      start = 0;
      end = 0;
    }
  } else if (resp == BIN_RANGE_SPECIFIED) {
    start = f->min_written;
    end = f->max_written;
    if (end < start) {
      start = 0;
      end = 0;
    }
    // open specify dialog
    GtkWidget *sd = gtk_dialog_new_with_buttons(
        "Specify range", f->app->win, GTK_DIALOG_MODAL, "Cancel",
        GTK_RESPONSE_CANCEL, "OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(sd));
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);
    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);

    GtkWidget *l1 = gtk_label_new("Start (hex):");
    GtkWidget *l2 = gtk_label_new("End (hex):");
    GtkWidget *e1 = gtk_entry_new();
    GtkWidget *e2 = gtk_entry_new();

    gtk_entry_set_max_length(GTK_ENTRY(e1), 4);
    gtk_entry_set_max_length(GTK_ENTRY(e2), 4);

    g_signal_connect(e1, "changed", G_CALLBACK(sanitize_hex_entry), NULL);
    g_signal_connect(e2, "changed", G_CALLBACK(sanitize_hex_entry), NULL);

    char buf1[8], buf2[8];
    g_snprintf(buf1, sizeof(buf1), "%04X", start);
    g_snprintf(buf2, sizeof(buf2), "%04X", end);

    gtk_editable_set_text(GTK_EDITABLE(e1), buf1);
    gtk_editable_set_text(GTK_EDITABLE(e2), buf2);

    gtk_grid_attach(GTK_GRID(grid), l1, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), e1, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), l2, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), e2, 1, 1, 1, 1);
    gtk_box_append(GTK_BOX(area), grid);

    // stash pointers on dialog
    g_object_set_data(G_OBJECT(sd), "e1", e1);
    g_object_set_data(G_OBJECT(sd), "e2", e2);

    g_signal_connect(sd, "response", G_CALLBACK(specified_range_response), f);
    gtk_window_present(GTK_WINDOW(sd));
    return;
  } else {
    assemble_flow_free(f);
    return;
  }

  GError *err = NULL;
  if (!write_bin_range(f->out_bin_path, f->mem, start, end, &err)) {
    const char *em =
        (err && err->message) ? err->message : "Failed to write binary";
    show_error(f->app, em);
    if (err)
      g_error_free(err);
    assemble_flow_free(f);
    return;
  }

  char msg[256];
  g_snprintf(msg, sizeof(msg), "Wrote %s (%04X..%04X)", f->out_bin_path, start,
             end);
  show_info(f->app, msg);
  assemble_flow_free(f);
}

// static void specified_range_response(GtkDialog *dlg, int resp, gpointer ud) {
//   AssembleFlow *f = (AssembleFlow *)ud;
//   GtkWidget *e1 = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "e1");
//   GtkWidget *e2 = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "e2");

//   if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
//     gtk_window_destroy(GTK_WINDOW(dlg));
//     assemble_flow_free(f);
//     return;
//   }

//   const char *t1 = gtk_editable_get_text(GTK_EDITABLE(e1));
//   const char *t2 = gtk_editable_get_text(GTK_EDITABLE(e2));

//   uint16_t start = 0, end = 0;

//   if (!parse_u16_hexish(t1, &start)) {
//     show_error(f->app,
//                "Invalid START address. Must be hex and between 0000 and
//                FFFF");
//     gtk_window_destroy(GTK_WINDOW(dlg));
//     assemble_flow_free(f);
//     return;
//   }
//   if (!parse_u16_hexish(t2, &end)) {
//     show_error(f->app,
//                "Invalid END address. Must be hex and between 0000 and FFFF");
//     gtk_window_destroy(GTK_WINDOW(dlg));
//     assemble_flow_free(f);
//     return;
//   }
//   if (end < start) {
//     show_error(f->app, "Invalid range. END  address is before START");
//     gtk_window_destroy(GTK_WINDOW(dlg));
//     assemble_flow_free(f);
//     return;
//   }

//   gtk_window_destroy(GTK_WINDOW(dlg));

//   GError *err = NULL;
//   if (!write_bin_range(f->out_bin_path, f->mem, start, end, &err)) {
//     const char *em =
//         (err && err->message) ? err->message : "Failed to write binary";
//     show_error(f->app, em);
//     if (err)
//       g_error_free(err);
//     assemble_flow_free(f);
//     return;
//   }

//   char msg[256];
//   g_snprintf(msg, sizeof(msg), "Wrote %s (%04X..%04X)", f->out_bin_path,
//   start,
//              end);
//   show_info(f->app, msg);
//   assemble_flow_free(f);
// }

static void specified_range_response(GtkDialog *dlg, int resp, gpointer ud) {
  AssembleFlow *f = (AssembleFlow *)ud;
  GtkWidget *e1 = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "e1");
  GtkWidget *e2 = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "e2");

  if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
    gtk_window_destroy(GTK_WINDOW(dlg));
    assemble_flow_free(f);
    return;
  }

  const char *t1 = gtk_editable_get_text(GTK_EDITABLE(e1));
  const char *t2 = gtk_editable_get_text(GTK_EDITABLE(e2));

  uint16_t start = 0, end = 0;

  /* START parse */
  if (!parse_u16_hexish(t1, &start)) {
    show_error(f->app,
               "Invalid START address.\nUse 1–4 hex digits (0000–FFFF).");
    gtk_window_destroy(GTK_WINDOW(dlg));
    assemble_flow_free(f);
    return;
  }

  /* END parse */
  if (!parse_u16_hexish(t2, &end)) {
    show_error(f->app, "Invalid END address.\nUse 1–4 hex digits (0000–FFFF).");
    gtk_window_destroy(GTK_WINDOW(dlg));
    assemble_flow_free(f);
    return;
  }

  /* Range validation */
  if (end < start) {
    show_error(f->app, "Invalid range:\nEND address must be >= START address.");
    gtk_window_destroy(GTK_WINDOW(dlg));
    assemble_flow_free(f);
    return;
  }

  gtk_window_destroy(GTK_WINDOW(dlg));

  GError *err = NULL;
  if (!write_bin_range(f->out_bin_path, f->mem, start, end, &err)) {
    const char *em =
        (err && err->message) ? err->message : "Failed to write binary";
    show_error(f->app, em);
    if (err)
      g_error_free(err);
    assemble_flow_free(f);
    return;
  }

  char msg[256];
  g_snprintf(msg, sizeof(msg), "Wrote %s (%04X..%04X)", f->out_bin_path, start,
             end);
  show_info(f->app, msg);

  assemble_flow_free(f);
}

/* After assembly, prompt the user to choose output byte range then save. */
static void range_prompt_and_save(AssembleFlow *f) {
  GtkWidget *q = gtk_message_dialog_new(f->app->win, GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                        "Choose range to write:");
  gtk_window_set_transient_for(GTK_WINDOW(q), f->app->win);
  gtk_window_set_modal(GTK_WINDOW(q), TRUE);
  gtk_dialog_add_button(GTK_DIALOG(q), "Cancel", GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(q), "Assembled range", BIN_RANGE_ASSEMBLED);
  gtk_dialog_add_button(GTK_DIALOG(q), "Specify range...", BIN_RANGE_SPECIFIED);
  gtk_dialog_add_button(GTK_DIALOG(q), "Entire 64KB", BIN_RANGE_FULL);
  /* 16KB from assembly start — only shown when range is known */
  if (f->max_written >= f->min_written) {
    char lbl16[32];
    g_snprintf(lbl16, sizeof(lbl16), "16KB from &%04X", f->min_written);
    gtk_dialog_add_button(GTK_DIALOG(q), lbl16, BIN_RANGE_16K);
  }
  dialog_add_default_keys(q, BIN_RANGE_ASSEMBLED, GTK_RESPONSE_CANCEL);
  gtk_dialog_set_default_response(GTK_DIALOG(q), BIN_RANGE_ASSEMBLED);
  g_signal_connect(q, "response", G_CALLBACK(range_choice_response), f);
  gtk_window_present(GTK_WINDOW(q));
}

/* ---- Stage 1: choose output path ---- */

static void save_as_finish_cb(GObject *source, GAsyncResult *res,
                              gpointer user_data) {
  AssembleFlow *f = (AssembleFlow *)user_data;
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  GError *err = NULL;
  GFile *file = gtk_file_dialog_save_finish(dlg, res, &err);
  if (!file) {
    if (err)
      g_error_free(err);
    assemble_flow_free(f);
    return;
  }
  char *path0 = g_file_get_path(file);
  g_object_unref(file);

  if (!path0) {
    assemble_flow_free(f);
    return;
  }
  f->out_bin_path = path_with_default_ext(
      path0, (f->app && f->app->bin_ext[0] ? f->app->bin_ext : ".bin"));
  g_free(path0);
  range_prompt_and_save(f);
}

/* Response callback for binary-save range dialog: write chosen range to disk.
 */
static void bin_choice_response(GtkDialog *dlg, int resp, gpointer ud) {
  AssembleFlow *f = (AssembleFlow *)ud;
  gtk_window_destroy(GTK_WINDOW(dlg));

  if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
    assemble_flow_free(f);
    return;
  }

  if (resp == 1) { // write suggested
    if (!f->suggested_bin) {
      show_error(f->app, "No source file path; use Save As...");
      assemble_flow_free(f);
      return;
    }
    f->out_bin_path = g_strdup(f->suggested_bin);
    range_prompt_and_save(f);
    return;
  }

  if (resp == 2) { // save as
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Save binary as...");
    gtk_file_dialog_save(fd, f->app->win, NULL, save_as_finish_cb, f);
    return;
  }

  assemble_flow_free(f);
}

static void assemble_offer_save_binary(AssembleFlow *f) {
  GtkWidget *q = gtk_message_dialog_new(f->app->win, GTK_DIALOG_MODAL,
                                        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                        "Write binary output?");
  gtk_window_set_transient_for(GTK_WINDOW(q), f->app->win);
  gtk_window_set_modal(GTK_WINDOW(q), TRUE);
  gtk_dialog_add_button(GTK_DIALOG(q), "Cancel", GTK_RESPONSE_CANCEL);
  if (f->suggested_bin) {
    /* Use the configured binary extension in the button label */
    const char *ext = (f->app->bin_ext[0]) ? f->app->bin_ext : ".bin";
    char lbl[64];
    g_snprintf(lbl, sizeof(lbl), "Write <source>%s", ext);
    gtk_dialog_add_button(GTK_DIALOG(q), lbl, 1);
  }
  gtk_dialog_add_button(GTK_DIALOG(q), "Save As...", 2);
  {
    int ok_resp = f->suggested_bin ? 1 : 2;
    dialog_add_default_keys(q, ok_resp, GTK_RESPONSE_CANCEL);
  }
  g_signal_connect(q, "response", G_CALLBACK(bin_choice_response), f);
  gtk_window_present(GTK_WINDOW(q));
}

static gboolean diags_has_errors_local(const diags_t *d) {
  if (!d)
    return FALSE;
  for (size_t i = 0; i < d->count; i++)
    if (d->items[i].sev == DIAG_ERROR)
      return TRUE;
  return FALSE;
}

static char *format_diags(const diags_t *d) {
  if (!d || d->count == 0)
    return g_strdup("");
  GString *s = g_string_new("\n\nDiagnostics:\n");
  for (size_t i = 0; i < d->count; i++) {
    const diag_t *x = &d->items[i];
    const char *sev = (x->sev == DIAG_ERROR)     ? "error"
                      : (x->sev == DIAG_WARNING) ? "warn"
                                                 : "info";
    g_string_append_printf(s, "%s:%u:%u: %s: %s\n",
                           x->file ? x->file : "(buffer)", x->line, x->col, sev,
                           x->msg);
  }
  return g_string_free(s, FALSE);
}

/* Action handler: save the file then run the assembler.
 * Updates the problems panel and offers to save binary on success. */
static void action_check(GSimpleAction *action, GVariant *param,
                         gpointer user_data) {
  (void)action;
  (void)param;
  AppState *s = (AppState *)user_data;
  if (!s || !s->buf)
    return;

  /* Save first if modified so run_check_ex reads current content. */
  if (s->path && s->path[0] && gtk_text_buffer_get_modified(GTK_TEXT_BUFFER(s->buf)))
    g_action_group_activate_action(G_ACTION_GROUP(s->app), "save", NULL);

  const char *file = (s->path && s->path[0]) ? s->path : NULL;
  if (!file) {
    g_action_group_activate_action(G_ACTION_GROUP(s->app), "save_as", NULL);
    file = (s->path && s->path[0]) ? s->path : NULL;
    if (!file)
      return;
  }

  runner_result_t rr = {0};
  runner_opts_t ropts = {.allow_undoc = s->allow_undoc ? true : false};
  run_check_ex(file, &rr, &ropts);

  /* Convert runner results into core diags_t for the Problems panel.
     F5 is an explicit user action — show ALL errors including on the cursor line. */
  diags_t diags;
  diags_init(&diags);
  for (size_t i = 0; i < rr.count; i++) {
    const runner_diag_item_t *rd = &rr.items[i];
    diag_severity_t sev = (rd->kind == RUN_DIAG_ERROR) ? DIAG_ERROR : DIAG_WARNING;
    diag_addf(&diags, sev, file, (unsigned)rd->line1, (unsigned)rd->col1, 1,
              "%s", rd->message ? rd->message : "");
  }
  runner_result_free(&rr);

  problems_set_from_diags(s, &diags);
  if (diags_has_errors_local(&diags))
    s->problems_autojump_pending = TRUE;

  diags_free(&diags);
  schedule_highlight(s);
}

static void action_do_assemble(GSimpleAction *action, GVariant *param,
                            gpointer user_data) {
  (void)action;
  (void)param;
  AppState *s = (AppState *)user_data;
  if (!s || !s->buf)
    return;

  /* ---- Multi-tab: offer "Assemble Project" vs "Assemble This Tab" ---- */
  const char *asm_file = (s->path && s->path[0]) ? s->path : NULL;
  char *project_text = NULL;   /* non-NULL if assembling root file from disk */
  char *project_file = NULL;   /* root file path when assembling project     */

  const int n_pages = s->notebook
      ? gtk_notebook_get_n_pages(GTK_NOTEBOOK(s->notebook)) : 1;

  if (n_pages > 1 && asm_file) {
    /* Find the root file (may not be an open tab). */
    char *root = it_find_root_for_active(s, asm_file);
    const char *root_path = (root && root[0]) ? root : asm_file;

    /* Build dialog */
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Assemble", GTK_WINDOW(s->win),
        (GtkDialogFlags)(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "Assemble Project",  1,
        "Assemble This Tab", 2,
        "Cancel",            GTK_RESPONSE_CANCEL,
        NULL);

    /* Show root file name as info label */
    char *root_base = g_path_get_basename(root_path);
    char *info_txt  = g_strdup_printf(
        "Project root: <b>%s</b>", root_base ? root_base : root_path);
    GtkWidget *info = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(info), info_txt);
    gtk_widget_set_margin_start(info, 16);
    gtk_widget_set_margin_end(info, 16);
    gtk_widget_set_margin_top(info, 12);
    gtk_widget_set_margin_bottom(info, 8);
    gtk_label_set_xalign(GTK_LABEL(info), 0.0f);
    g_free(info_txt);
    g_free(root_base);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_append(GTK_BOX(area), info);
    gtk_widget_set_visible(info, TRUE);

    DialogRunState dr = {0};
    dr.loop = g_main_loop_new(NULL, FALSE);
    dr.response = GTK_RESPONSE_CANCEL;
    g_signal_connect(dlg, "response", G_CALLBACK(dialog_response_quit), &dr);
    gtk_widget_show(dlg);
    g_main_loop_run(dr.loop);
    int resp = dr.response;
    g_main_loop_unref(dr.loop);
    gtk_window_destroy(GTK_WINDOW(dlg));

    if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
      g_free(root);
      return;
    }

    if (resp == 1) {
      /* Assemble Project: load root file text from disk (or open tab). */
      int root_page = app_notebook_page_for_path(s, root_path);
      if (root_page >= 0) {
        /* Root is open — use its live buffer text. */
        GtkWidget *pg = gtk_notebook_get_nth_page(
            GTK_NOTEBOOK(s->notebook), root_page);
        EditorDoc *rd = pg ? (EditorDoc *)g_object_get_data(
            G_OBJECT(pg), "app-doc") : NULL;
        if (rd && rd->buf) {
          GtkTextIter ra, rb;
          gtk_text_buffer_get_bounds(GTK_TEXT_BUFFER(rd->buf), &ra, &rb);
          project_text = gtk_text_buffer_get_text(
              GTK_TEXT_BUFFER(rd->buf), &ra, &rb, FALSE);
        }
      } else {
        /* Root not open — read from disk. */
        gsize rlen = 0;
        g_file_get_contents(root_path, &project_text, &rlen, NULL);
      }
      project_file = g_strdup(root_path);
      /* If we couldn't get the root file's text, bail out with an error. */
      if (!project_text) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Could not read root file:\n%s", root_path);
        show_error(s, msg);
        g_free(project_file);
        g_free(root);
        return;
      }
    }
    /* resp == 2: Assemble This Tab — fall through to existing behaviour. */
    g_free(root);
  }

  /* -------------------------------------------------------------------- */

  /* Determine what file and text to assemble. */
  const char *file = project_file ? project_file
                   : (asm_file    ? asm_file : "(buffer)");

  char *text;
  if (project_text) {
    text = project_text;  /* already heap-allocated */
  } else {
    GtkTextBuffer *tbuf = GTK_TEXT_BUFFER(s->buf);
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(tbuf, &a, &b);
    text = gtk_text_buffer_get_text(tbuf, &a, &b, FALSE);
  }
  if (!text) {
    g_free(project_file);
    return;
  }

  /* ---- original assembly pipeline (file / text now set above) ---- */

  // Build source object so we can strip comments preserving layout
  source_t src;
  src.text = text;
  src.len = strlen(text);
  source_strip_comments_preserve_layout(&src);

  AssembleFlow *f = (AssembleFlow *)g_malloc0(sizeof(AssembleFlow));
  f->app = s;
  diags_init(&f->diags);

  defs_t defs;
  defs_init(&defs);

  char hex_prefix = '\0';
  char bin_prefix = '\0';

  f->file = g_strdup(file);
  /* For bin path suggestion: use project root path when assembling project,
     otherwise use the current tab's path. */
  const char *bin_src_path = project_file ? project_file : s->path;
  f->suggested_bin = make_suggested_bin_path(bin_src_path, s->bin_ext);

  /* Include search path: directory of the file being assembled. */
  gchar *inc_dir = (file && file[0] && file[0] != '(')
                       ? g_path_get_dirname(file) : NULL;
  const char *inc_paths[1];
  size_t      inc_count = 0;
  if (inc_dir) { inc_paths[0] = inc_dir; inc_count = 1; }

  char *pp = preproc_run_ex(file, src.text, &f->diags, &defs, &hex_prefix,
                             false, bin_prefix, true,
                             inc_paths, inc_count);

  asm_settings_t st;
  memset(&st, 0, sizeof(st));
  st.cpu = cpu_from_preproc_defs(&defs);
  {
    const char *_cv = defs_get((defs_t *)&defs, sv_from_cstr("cpu"));
    st.allow_undoc =
        (_cv && strcmp(_cv, "6502u") == 0) || (s->allow_undoc ? true : false);
  }

  // Match CLI defaults for the BBC-style stdout trace.
  // If left zeroed, several fields default to "off" and the output becomes
  // confusing (e.g. missing addr/bytes columns).
  st.list_show_addr = true;
  st.list_show_bytes = true;
  st.list_bytes_per_line = 3;
  st.list_hex_prefix = 0;
  st.list_preserve_case = false;
  st.list_includes = false;

  st.overwrite_warn = true;
  st.overwrite_warn_limit = 10;
  st.guard_warn = true;

  // We want the BBC-style assembly trace (same as CLI stdout), not a .lst file.
  st.list_path = NULL;
  st.list_stdout = true;

  // output settings off
  st.out_path = NULL;
  st.sym_stdout = false;
  st.sym_path = NULL;

  f->mem = (uint8_t *)g_malloc0(65536);
  f->written = (uint8_t *)g_malloc0(65536);
  uint8_t *written_explicit_buf = (uint8_t *)g_malloc0(65536);
  f->min_written = 0xFFFF; /* sentinel: emit_u8 only updates when addr < minw */
  f->max_written = 0;
  uint16_t out_start = 0, out_end = 0;

  // Capture stdout produced by asm_assemble (the BBC-style trace).
  char *lst = NULL;
  if (pp) {
    AssembleCaptureCtx c;
    memset(&c, 0, sizeof(c));
    c.file = file;
    c.text = pp;
    c.st = &st;
    c.mem = f->mem;
    c.written = f->written;
    c.written_explicit = written_explicit_buf;
    c.clear_mem = f->app ? f->app->asm_clear_mem : TRUE;
    c.min_written = &f->min_written;
    c.max_written = &f->max_written;
    c.out_start = &out_start;
    c.out_end = &out_end;
    c.diags = &f->diags;
    lst = capture_stdout_run(assemble_capture_fn, &c);
  }

  char *dmsg = format_diags(&f->diags);

  if (!lst)
    lst = g_strdup("(no output)\n");
  f->listing_text = g_strconcat(lst, dmsg, NULL);

  g_free(lst);
  g_free(dmsg);

  if (pp)
    free(pp);
  defs_free(&defs);
  g_free(text);
  g_free(project_file);
  g_free(inc_dir);

  gboolean has_err = diags_has_errors_local(&f->diags);
  if (has_err) {
    /* Make the first error immediately visible after assembling. */
    s->problems_autojump_pending = TRUE;
  }

  /* Update the bottom Problems panel from this assemble's diagnostics. */
  problems_set_from_diags(s, &f->diags);

  if (!has_err && f->mem) {
    memcpy(s->mem_image, f->mem, 65536);
    s->mem_valid = TRUE;
    s->mem_dirty = FALSE;
    if (f->min_written <= f->max_written) {
      s->mem_asm_start = f->min_written;
      s->mem_asm_valid = TRUE;
    }
#ifdef ED_BUILD_SIM
    /* Update snapshot on any open simulator window so Reload ROM
     * picks up the freshly assembled binary */
    {
      GtkWindow *simwin_gtk = (GtkWindow *)g_object_get_data(
          G_OBJECT(s->app), "active_sim_window");
      if (simwin_gtk && GTK_IS_WINDOW(simwin_gtk)) {
        GBytes *snap = g_bytes_new(s->mem_image, 65536);
        g_object_set_data_full(G_OBJECT(simwin_gtk), "mem_snapshot",
                               snap, (GDestroyNotify)g_bytes_unref);
      }
    }
#endif
  }

  /* Count bytes emitted and compute contiguous ranges for memory-map popup */
  uint32_t _nbytes = 0;
  f->range_count = 0;
  if (!has_err && f->written) {
    int _in = 0;
    for (int _i = 0; _i <= 65535; _i++) {
      if (f->written[_i]) {
        _nbytes++;
        if (!_in && f->range_count < 64) {
          f->ranges[f->range_count].start = (uint16_t)_i;
          f->ranges[f->range_count].count = 0;
          _in = 1;
        }
        if (f->range_count < 64)
          f->ranges[f->range_count].count++;
      } else if (_in) {
        f->ranges[f->range_count].end = (uint16_t)(_i - 1);
        f->range_count++;
        _in = 0;
      }
    }
    if (_in && f->range_count < 64) {
      f->ranges[f->range_count].end = 0xFFFF;
      f->range_count++;
    }
    f->asm_bytes = _nbytes;
    f->written_snap = (uint8_t *)g_malloc(65536);
    if (f->written_snap)
      memcpy(f->written_snap, f->written, 65536);
    f->written_explicit_snap = (uint8_t *)g_malloc(65536);
    if (f->written_explicit_snap)
      memcpy(f->written_explicit_snap, written_explicit_buf, 65536);
  }
  g_free(written_explicit_buf);
  char _wtbuf[80];
  if (has_err) {
    g_snprintf(_wtbuf, sizeof(_wtbuf), "Assemble (with errors)");
  } else {
    g_snprintf(_wtbuf, sizeof(_wtbuf),
               "Assemble OK \xe2\x80\x94 %u bytes in %d range%s", _nbytes,
               f->range_count, f->range_count == 1 ? "" : "s");
  }
  const char *wtitle = _wtbuf;
  if (has_err) {
    show_listing_window(s, wtitle, f->listing_text, NULL);
    assemble_flow_free(f);
    return;
  }

  // On success, keep the assembled memory context alive until the user either
  // clicks "Save Binary" (which launches the save flow) or closes the window.
  show_listing_window(s, wtitle, f->listing_text, f);
}

/* ---------------- Jump-to-definition / Find references ----------------
 *
 * Conservative implementation:
 * - Same-file only (current buffer)
 * - Skips anything inside comment / block comment / string by using the
 *   existing highlight tags (tag_comment, tag_string).
 * - No regex. Token scans are simple ASCII checks.
 */

static gboolean editor_iter_in_comment_or_string(AppState *s,
                                                 const GtkTextIter *it) {
  /* gtk_text_iter_get_tags returns GSList* in GTK4 */
  GSList *tags = gtk_text_iter_get_tags((GtkTextIter *)it);
  for (GSList *p = tags; p; p = p->next) {
    GtkTextTag *t = GTK_TEXT_TAG(p->data);
    if (t == s->tag_comment || t == s->tag_string || t == s->tag_dv_str) {
      g_slist_free(tags);
      return TRUE;
    }
  }
  g_slist_free(tags);
  return FALSE;
}

static gboolean editor_is_ident_char(gunichar c) {
  if (c == '_' || c == '.')
    return TRUE;
  if (c >= '0' && c <= '9')
    return TRUE;
  if (c >= 'A' && c <= 'Z')
    return TRUE;
  if (c >= 'a' && c <= 'z')
    return TRUE;
  return FALSE;
}

static char *editor_symbol_at_iter(AppState *s, GtkTextIter *it_in) {
  GtkTextIter it = *it_in;

  if (editor_iter_in_comment_or_string(s, &it))
    return NULL;

  /* Special-case anonymous local labels :+ and :- (jump targets).
   * We treat these as symbols even though ':' isn't an identifier char. */
  {
    GtkTextIter t = it;
    gunichar c = gtk_text_iter_get_char(&t);

    /* If caret is on ':' */
    if (c == ':') {
      GtkTextIter t2 = t;
      if (!gtk_text_iter_ends_line(&t2) && gtk_text_iter_forward_char(&t2)) {
        gunichar c2 = gtk_text_iter_get_char(&t2);
        if (c2 == '+' || c2 == '-') {
          char tmp[3] = {':', (char)c2, 0};
          return g_strdup(tmp);
        }
      }
    }

    /* If caret is on '+' or '-' and previous char is ':' */
    if (c == '+' || c == '-') {
      GtkTextIter t2 = t;
      if (gtk_text_iter_backward_char(&t2)) {
        gunichar c2 = gtk_text_iter_get_char(&t2);
        if (c2 == ':') {
          char tmp[3] = {':', (char)c, 0};
          return g_strdup(tmp);
        }
      }
    }
  }

  /* If we're on whitespace, move left one char (common when clicking after). */
  if (gtk_text_iter_ends_line(&it) == FALSE) {
    gunichar c = gtk_text_iter_get_char(&it);
    if (!editor_is_ident_char(c)) {
      GtkTextIter left = it;
      if (gtk_text_iter_backward_char(&left)) {
        c = gtk_text_iter_get_char(&left);
        if (editor_is_ident_char(c))
          it = left;
      }
    }
  }

  gunichar c0 = gtk_text_iter_get_char(&it);
  if (!editor_is_ident_char(c0))
    return NULL;

  GtkTextIter a = it, b = it;
  while (gtk_text_iter_backward_char(&a)) {
    gunichar c = gtk_text_iter_get_char(&a);
    if (!editor_is_ident_char(c)) {
      gtk_text_iter_forward_char(&a);
      break;
    }
  }
  while (!gtk_text_iter_ends_line(&b) && gtk_text_iter_forward_char(&b)) {
    gunichar c = gtk_text_iter_get_char(&b);
    if (!editor_is_ident_char(c))
      break;
  }

  if (gtk_text_iter_equal(&a, &b))
    return NULL;

  char *sym = gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &a, &b, FALSE);
  if (!sym || !sym[0]) {
    g_free(sym);
    return NULL;
  }
  return sym;
}

/* Return TRUE if tokens a and b are equal case-insensitively. */
static gboolean editor_token_equals_ci(const char *a, const char *b) {
  if (!a || !b)
    return FALSE;
  while (*a && *b) {
    char ca = *a++, cb = *b++;
    if (tolower((unsigned char)ca) != tolower((unsigned char)cb))
      return FALSE;
  }
  return *a == 0 && *b == 0;
}

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;
  return p;
}

/* Consume the next identifier token from *pp into out.
 * ret: TRUE if a token was found */
static gboolean scan_ident_token(const char **pp, char *out, int out_sz) {
  const char *p = *pp;
  p = skip_ws(p);
  int n = 0;
  if (!editor_is_ident_char((unsigned char)*p))
    return FALSE;
  while (*p && editor_is_ident_char((unsigned char)*p)) {
    if (n + 1 < out_sz)
      out[n++] = *p;
    p++;
  }
  out[n] = 0;
  *pp = p;
  return n > 0;
}

/* Return TRUE if mn is a branch/jump mnemonic. */
static gboolean tt_is_branch_mnemonic(const char *mn) {
  static const char *branches[] = {
      "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ",
      "BRA" // 65C02
  };
  for (size_t i = 0; i < G_N_ELEMENTS(branches); i++) {
    if (g_ascii_strcasecmp(mn, branches[i]) == 0)
      return TRUE;
  }
  return FALSE;
}

/* Find definition of symbol; returns TRUE and writes out_iter when found. */
static gboolean editor_find_definition(AppState *s, const char *sym,
                                       GtkTextIter *out_iter) {
  GtkTextIter it;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &it);

  while (TRUE) {
    GtkTextIter ls = it;
    GtkTextIter le = it;
    gtk_text_iter_forward_to_line_end(&le);

    char *line =
        gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &ls, &le, FALSE);
    const char *p = line ? line : "";
    p = skip_ws(p);

    /* empty / comment-only */
    if (*p && *p != ';') {
      char tok1[128] = {0};
      const char *p2 = p;

      if (scan_ident_token(&p2, tok1, (int)sizeof(tok1))) {
        /* Ensure token itself isn't inside comment/string by checking its iter
         */
        GtkTextIter tok_it = ls;
        gtk_text_iter_set_line_offset(&tok_it, 0);
        int ws = 0;
        while (p[ws] == ' ' || p[ws] == '\t')
          ws++;
        gtk_text_iter_set_line_offset(&tok_it, ws);

        if (!editor_iter_in_comment_or_string(s, &tok_it)) {
          const char *after1 = skip_ws(p2);

          /* LABEL: */
          if (*after1 == ':' && editor_token_equals_ci(tok1, sym)) {
            *out_iter = tok_it;
            g_free(line);
            return TRUE;
          }

          /* .dotlabel (no colon) — tok1 starts with '.' and is not a
             known directive. Match sym with or without leading dot. */
          if (tok1[0] == '.' && tok1[1] && !is_directive_token(tok1) &&
              !is_data_directive_token(tok1)) {
            /* sym may be ".loop" or "loop" — try both */
            gboolean match = editor_token_equals_ci(tok1, sym);
            if (!match && sym[0] != '.') {
              /* sym has no dot: compare without the leading dot in tok1 */
              match = editor_token_equals_ci(tok1 + 1, sym);
            }
            if (!match && sym[0] == '.') {
              /* sym has dot: compare tok1+1 with sym+1 already covered above */
              match = editor_token_equals_ci(tok1, sym);
            }
            if (match) {
              *out_iter = tok_it;
              g_free(line);
              return TRUE;
            }
          }

          /* .def NAME ... / .define NAME ... */
          if (editor_token_equals_ci(tok1, ".def") ||
              editor_token_equals_ci(tok1, ".define")) {
            char tok2[128] = {0};
            const char *p3 = after1;
            if (scan_ident_token(&p3, tok2, (int)sizeof(tok2)) &&
                editor_token_equals_ci(tok2, sym)) {
              /* iter at NAME token */
              GtkTextIter name_it = tok_it;
              int off = ws;
              /* advance to after tok1 */
              off += (int)strlen(tok1);
              while (p[off] == ' ' || p[off] == '\t')
                off++;
              gtk_text_iter_set_line_offset(&name_it, off);
              *out_iter = name_it;
              g_free(line);
              return TRUE;
            }
          }

          /* .macro NAME / macro NAME */
          if (editor_token_equals_ci(tok1, ".macro") ||
              editor_token_equals_ci(tok1, "macro")) {
            char tok2[128] = {0};
            const char *p3 = after1;
            if (scan_ident_token(&p3, tok2, (int)sizeof(tok2)) &&
                editor_token_equals_ci(tok2, sym)) {
              GtkTextIter name_it = tok_it;
              int off = ws + (int)strlen(tok1);
              while (line[off] == ' ' || line[off] == '\t')
                off++;
              gtk_text_iter_set_line_offset(&name_it, off);
              *out_iter = name_it;
              g_free(line);
              return TRUE;
            }
          }

          /* NAME = ... */
          if (*after1 == '=' && editor_token_equals_ci(tok1, sym)) {
            *out_iter = tok_it;
            g_free(line);
            return TRUE;
          }

          /* NAME EQU ... */
          if (editor_token_equals_ci(tok1, sym)) {
            char tok2[128] = {0};
            const char *p3 = after1;
            if (scan_ident_token(&p3, tok2, (int)sizeof(tok2)) &&
                editor_token_equals_ci(tok2, "EQU")) {
              *out_iter = tok_it;
              g_free(line);
              return TRUE;
            }
          }
        }
      }
    }

    g_free(line);
    if (!gtk_text_iter_forward_line(&it))
      break;
  }

  return FALSE;
}

/* ----- Special navigation targets ----- */

static gboolean editor_find_cpu_directive(AppState *s, GtkTextIter *out_iter) {
  GtkTextIter it;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &it);

  while (TRUE) {
    GtkTextIter ls = it;
    GtkTextIter le = it;
    gtk_text_iter_forward_to_line_end(&le);

    char *line =
        gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &ls, &le, FALSE);
    const char *p = line ? line : "";
    int ws = 0;
    while (p[ws] == ' ' || p[ws] == '\t')
      ws++;
    p += ws;

    if (*p && *p != ';') {
      /* token at first non-ws */
      char tok1[128] = {0};
      const char *p2 = p;
      if (scan_ident_token(&p2, tok1, (int)sizeof(tok1))) {
        /* Check tag at token start */
        GtkTextIter tok_it = ls;
        gtk_text_iter_set_line_offset(&tok_it, ws);
        if (!editor_iter_in_comment_or_string(s, &tok_it)) {
          if (editor_token_equals_ci(tok1, ".cpu")) {
            *out_iter = tok_it;
            g_free(line);
            return TRUE;
          }
        }
      }
    }

    g_free(line);
    if (!gtk_text_iter_forward_line(&it))
      break;
  }

  return FALSE;
}

/* Anonymous label lines: first non-whitespace char is ':'
 * (colon-start-of-line). */
static gboolean editor_find_anon_label_from_line(AppState *s, int start_line,
                                                 int dir,
                                                 GtkTextIter *out_iter) {
  GtkTextIter it;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &it);
  gtk_text_iter_set_line(&it, start_line);

  while (TRUE) {
    GtkTextIter ls = it;
    GtkTextIter le = it;
    gtk_text_iter_forward_to_line_end(&le);

    char *line =
        gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &ls, &le, FALSE);
    const char *p = line ? line : "";
    int ws = 0;
    while (p[ws] == ' ' || p[ws] == '\t')
      ws++;

    if (p[ws] == ':') {
      GtkTextIter colon_it = ls;
      gtk_text_iter_set_line_offset(&colon_it, ws);
      if (!editor_iter_in_comment_or_string(s, &colon_it)) {
        *out_iter = colon_it;
        g_free(line);
        return TRUE;
      }
    }

    g_free(line);

    if (dir > 0) {
      if (!gtk_text_iter_forward_line(&it))
        break;
    } else {
      if (!gtk_text_iter_backward_line(&it))
        break;
    }
  }
  return FALSE;
}

/* Push current position to jump history then move cursor to *it. */
static void editor_jump_to_iter(AppState *s, const GtkTextIter *it) {
  /* Push current position to jump history before moving */
  GtkTextMark *ins = gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(s->buf));
  GtkTextIter cur;
  gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(s->buf), &cur, ins);
  jump_buf_push(&s->jump_buf, gtk_text_iter_get_line(&cur),
                gtk_text_iter_get_line_offset(&cur));
  GtkTextIter i = *it;
  gtk_text_buffer_place_cursor(GTK_TEXT_BUFFER(s->buf), &i);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &i, 0.0, TRUE, 0.5, 0.1);
}

static void editor_jump_to_definition(AppState *s, const char *sym) {
  GtkTextIter def_it;

  if (!sym || !sym[0])
    return;

  /* Special-case .cpu (and cpu) to jump to the CPU directive. */
  if (editor_token_equals_ci(sym, "cpu") ||
      editor_token_equals_ci(sym, ".cpu")) {
    if (editor_find_cpu_directive(s, &def_it)) {
      editor_jump_to_iter(s, &def_it);
      return;
    }
    show_info(s, "No .cpu directive found in this file.");
    return;
  }

  /* Special-case anonymous local labels :+ and :- (jump only). */
  if (strcmp(sym, ":+") == 0 || strcmp(sym, ":-") == 0) {
    GtkTextIter cur;
    GtkTextMark *ins = gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(s->buf));
    gtk_text_buffer_get_iter_at_mark(GTK_TEXT_BUFFER(s->buf), &cur, ins);
    int line = gtk_text_iter_get_line(&cur);

    if (strcmp(sym, ":+") == 0) {
      /* Search forward starting after current line. */
      if (editor_find_anon_label_from_line(s, line + 1, +1, &def_it)) {
        editor_jump_to_iter(s, &def_it);
        return;
      }
      show_info(s, "No next anonymous label (:) found.");
      return;
    } else {
      /* Search backward starting before current line. */
      if (editor_find_anon_label_from_line(s, line - 1, -1, &def_it)) {
        editor_jump_to_iter(s, &def_it);
        return;
      }
      show_info(s, "No previous anonymous label (:) found.");
      return;
    }
  }

  if (editor_find_definition(s, sym, &def_it)) {
    editor_jump_to_iter(s, &def_it);
    return;
  }
  show_info(s, "No definition found for that symbol.");
}

/* ----- References ----- */

typedef struct RefHit {
  int line;   /* 1-based */
  int col;    /* 1-based */
  int offset; /* buffer char offset */
  char *text; /* snippet */
} RefHit;

static void ref_hit_free(gpointer p) {
  RefHit *h = (RefHit *)p;
  if (!h)
    return;
  g_free(h->text);
  g_free(h);
}

static gboolean is_token_boundary(char c) {
  return !(isalnum((unsigned char)c) || c == '_' || c == '.');
}

static GPtrArray *editor_collect_references(AppState *s, const char *sym) {
  GPtrArray *hits = g_ptr_array_new_with_free_func(ref_hit_free);

  GtkTextIter it;
  gtk_text_buffer_get_start_iter(GTK_TEXT_BUFFER(s->buf), &it);

  while (TRUE) {
    int line_no = gtk_text_iter_get_line(&it); /* 0-based */
    GtkTextIter ls = it;
    GtkTextIter le = it;
    gtk_text_iter_forward_to_line_end(&le);

    char *line =
        gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &ls, &le, FALSE);
    if (line && *line) {
      int n = (int)strlen(line);
      for (int i = 0; i < n;) {
        /* quick skip non-ident */
        if (!editor_is_ident_char((unsigned char)line[i])) {
          i++;
          continue;
        }

        int j = i;
        while (j < n && editor_is_ident_char((unsigned char)line[j]))
          j++;

        /* token boundary check */
        char prev = (i == 0) ? ' ' : line[i - 1];
        char next = (j >= n) ? ' ' : line[j];
        if (is_token_boundary(prev) && is_token_boundary(next)) {
          char tok[128] = {0};
          int tlen = j - i;
          if (tlen > 0 && tlen < (int)sizeof(tok)) {
            memcpy(tok, &line[i], tlen);
            tok[tlen] = 0;

            if (editor_token_equals_ci(tok, sym)) {
              GtkTextIter hit_it = ls;
              gtk_text_iter_set_line_offset(&hit_it, i);

              if (!editor_iter_in_comment_or_string(s, &hit_it)) {
                RefHit *h = g_new0(RefHit, 1);
                h->line = line_no + 1;
                h->col = i + 1;
                h->offset = gtk_text_iter_get_offset(&hit_it);
                /* snippet */
                h->text = g_strdup(line);
                g_ptr_array_add(hits, h);
              }
            }
          }
        }

        i = j;
      }
    }

    g_free(line);
    if (!gtk_text_iter_forward_line(&it))
      break;
  }

  return hits;
}

static void refs_row_activated(GtkListBox *box, GtkListBoxRow *row,
                               gpointer ud) {
  AppState *s = (AppState *)ud;
  int offset = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "ref-offset"));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(s->buf), &it, offset);
  editor_jump_to_iter(s, &it);

  GtkWidget *dlg = gtk_widget_get_ancestor(GTK_WIDGET(box), GTK_TYPE_WINDOW);
  if (dlg)
    gtk_window_destroy(GTK_WINDOW(dlg));
}

/* ---------------- Hover tooltips ---------------- */

// Forward decls for tooltip-only helpers (avoid implicit-decl warnings with
// -std=gnu11).
static const char *skip_ws(const char *p);
static gboolean tt_is_ident_char(char c);
static gboolean scan_ident_token(const char **pp, char *out, int out_sz);
static gboolean tt_parse_int_literal(const char *s, int *out_value,
                                     int *out_base);
static gboolean tt_is_branch_mnemonic(const char *mn);

// NOTE: core/util.h already declares parse_int_literal(sv_t,int64_t*).
// This GUI-only helper intentionally uses a different name to avoid collisions.
static gboolean tt_parse_int_literal(const char *s, int *out_value,
                                     int *out_base) {
  if (!s || !*s)
    return FALSE;
  // Hex: $12AB or 0x12AB
  if (s[0] == '$') {
    char *end = NULL;
    long v = strtol(s + 1, &end, 16);
    if (end && end != s + 1 && *end == '\0') {
      *out_value = (int)v;
      if (out_base)
        *out_base = 16;
      return TRUE;
    }
    return FALSE;
  }
  if ((s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) {
    char *end = NULL;
    long v = strtol(s + 2, &end, 16);
    if (end && end != s + 2 && *end == '\0') {
      *out_value = (int)v;
      if (out_base)
        *out_base = 16;
      return TRUE;
    }
    return FALSE;
  }
  // Binary: %1010
  if (s[0] == '%') {
    int v = 0;
    const char *p = s + 1;
    if (!*p)
      return FALSE;
    for (; *p; p++) {
      if (*p != '0' && *p != '1')
        return FALSE;
      v = (v << 1) | (*p - '0');
    }
    *out_value = v;
    if (out_base)
      *out_base = 2;
    return TRUE;
  }
  // Decimal (allow leading -)
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (end && end != s && *end == '\0') {
    *out_value = (int)v;
    if (out_base)
      *out_base = 10;
    return TRUE;
  }
  return FALSE;
}

// Like tt_parse_int_literal(), but also returns the end pointer (first char not
// part of the literal). This is handy when parsing in-line, e.g. ".def NAME
// $09".
static gboolean tt_parse_int_literal_end(const char *s, int *out_value,
                                         int *out_base, const char **out_end) {
  if (!s)
    return FALSE;
  const char *p = s;
  // stop at whitespace or ';' comment start
  while (*p && *p != ';' && !g_ascii_isspace((guchar)*p))
    p++;
  if (p == s)
    return FALSE;
  char tmp[64];
  int n = (int)(p - s);
  if (n >= (int)sizeof(tmp))
    n = (int)sizeof(tmp) - 1;
  memcpy(tmp, s, (size_t)n);
  tmp[n] = '\0';
  if (!tt_parse_int_literal(tmp, out_value, out_base))
    return FALSE;
  if (out_end)
    *out_end = p;
  return TRUE;
}

static gboolean tt_is_ident_start(unsigned char c) {
  return (c == '_' || c == '.' || (c >= 'A' && c <= 'Z') ||
          (c >= 'a' && c <= 'z'));
}

static const char *tt_skip_ws(const char *p) {
  while (p && *p && g_ascii_isspace((unsigned char)*p))
    p++;
  return p;
}

// Forward for recursion.
static gboolean def_value_for_symbol_rec(AppState *s, const char *sym,
                                         int depth, int *out_value);

// Evaluate a very small expression grammar used in .def lines.
// Grammar:
//   expr := term (('+'|'-') term)*
//   term := int_lit | ident
// Stops at end-of-string or ';' comment.

// Parse one term and advance *pp.
static gboolean tt_parse_term_simple(AppState *s, const char **pp, int depth,
                                     int *out_term) {
  if (!pp || !*pp || !out_term)
    return FALSE;
  const char *p = tt_skip_ws(*pp);
  if (!p || !*p || *p == ';')
    return FALSE;

  int v = 0, base = 10;
  const char *end = NULL;
  if (tt_parse_int_literal_end(p, &v, &base, &end)) {
    *pp = end;
    *out_term = v;
    return TRUE;
  }

  if (!tt_is_ident_start((unsigned char)*p))
    return FALSE;
  const char *id0 = p;
  p++;
  while (*p && tt_is_ident_char((unsigned char)*p))
    p++;

  char name[128];
  int n = (int)(p - id0);
  if (n <= 0 || n >= (int)sizeof(name))
    return FALSE;
  memcpy(name, id0, (size_t)n);
  name[n] = 0;

  int idv = 0;
  if (!def_value_for_symbol_rec(s, name, depth + 1, &idv))
    return FALSE;
  *pp = p;
  *out_term = idv;
  return TRUE;
}

/* Evaluate a simple assembler expression for tooltip display.
 * ret: TRUE if evaluation succeeded */
static gboolean tt_eval_expr_simple(AppState *s, const char *expr, int depth,
                                    int *out_value) {
  if (!expr || !out_value)
    return FALSE;
  if (depth > 16)
    return FALSE;

  const char *p = tt_skip_ws(expr);
  if (!p || !*p)
    return FALSE;

  int acc = 0;
  if (!tt_parse_term_simple(s, &p, depth, &acc))
    return FALSE;

  for (;;) {
    p = tt_skip_ws(p);
    if (!p || !*p || *p == ';')
      break;
    char op = *p;
    if (op != '+' && op != '-')
      break;
    p++;
    int rhs = 0;
    if (!tt_parse_term_simple(s, &p, depth, &rhs))
      return FALSE;
    if (op == '+')
      acc += rhs;
    else
      acc -= rhs;
  }

  *out_value = acc;
  return TRUE;
}

/* Recursively resolve sym to a numeric value by scanning the document.
 * ret: TRUE if sym resolved to a value */
static gboolean def_value_for_symbol_rec(AppState *s, const char *sym,
                                         int depth, int *out_value) {
  if (!s || !sym || !*sym || !out_value)
    return FALSE;
  if (depth > 32)
    return FALSE;

  GtkTextIter it;
  if (!editor_find_definition(s, sym, &it))
    return FALSE;

  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  GtkTextIter ls = it, le = it;
  gtk_text_iter_set_line_offset(&ls, 0);
  gtk_text_iter_forward_to_line_end(&le);

  char *line = gtk_text_buffer_get_text(tb, &ls, &le, FALSE);
  if (!line)
    return FALSE;

  const char *p = skip_ws(line);
  if (*p == '.')
    p++;
  /* 'def' must be followed by whitespace (or end) to avoid matching symbols
   * like 'define' */
  if (g_ascii_strncasecmp(p, "def", 3) != 0 ||
      (p[3] && !g_ascii_isspace((guchar)p[3]))) {
    g_free(line);
    return FALSE;
  }
  p += 3;
  p = skip_ws(p);

  char name[128];
  if (!scan_ident_token(&p, name, (int)sizeof(name))) {
    g_free(line);
    return FALSE;
  }
  if (g_ascii_strcasecmp(name, sym) != 0) {
    // editor_find_definition may have matched a different thing
    // (case/substring) Only accept an exact symbol match.
    g_free(line);
    return FALSE;
  }

  p = skip_ws(p);
  if (!*p) {
    g_free(line);
    return FALSE;
  }

  int v = 0;
  gboolean ok = tt_eval_expr_simple(s, p, depth + 1, &v);
  g_free(line);
  if (!ok)
    return FALSE;

  *out_value = v;
  return TRUE;
}

// NOTE: core/util.h already declares is_ident_char(char).
// This GUI-only helper intentionally uses a different name to avoid collisions.
static gboolean tt_is_ident_char(char c) {
  return (c == '_' || c == '.' || (c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static gboolean token_at_line_offset(const char *line, int off, char *out,
                                     int out_sz, int *out_start, int *out_end) {
  if (!line || out_sz <= 0)
    return FALSE;
  int len = (int)strlen(line);
  if (off < 0)
    off = 0;
  if (off > len)
    off = len;

  // Special case for :+ / :-
  for (int d = -1; d <= 0; d++) {
    int i = off + d;
    if (i >= 0 && i + 1 < len && line[i] == ':' &&
        (line[i + 1] == '+' || line[i + 1] == '-')) {
      out[0] = ':';
      out[1] = line[i + 1];
      out[2] = 0;
      if (out_start)
        *out_start = i;
      if (out_end)
        *out_end = i + 2;
      return TRUE;
    }
  }

  // If this is a colon-start-of-line label (':' after optional whitespace),
  // treat ':name' as a token.
  {
    const char *p = line;
    int base = 0;
    while (*p && (*p == ' ' || *p == '\t')) {
      p++;
      base++;
    }
    if (*p == ':') {
      int colon = base;
      int n0 = colon + 1;
      int n1 = n0;
      while (n1 < len && tt_is_ident_char(line[n1]))
        n1++;
      if (n1 > n0) {
        // Cursor must be on ':' or within name.
        if (off >= colon && off <= n1) {
          int n = n1 - colon; // include ':'
          if (n >= out_sz)
            n = out_sz - 1;
          memcpy(out, line + colon, (size_t)n);
          out[n] = 0;
          if (out_start)
            *out_start = colon;
          if (out_end)
            *out_end = n1;
          return TRUE;
        }
      }
    }
  }

  // Normal identifier token
  int s = off;
  if (s > 0 && s == len)
    s = len - 1;
  if (s < len && !tt_is_ident_char(line[s]) && s > 0 &&
      tt_is_ident_char(line[s - 1]))
    s--;

  if (s < 0 || s >= len || !tt_is_ident_char(line[s]))
    return FALSE;

  int a = s;
  while (a > 0 && tt_is_ident_char(line[a - 1]))
    a--;
  int b = s;
  while (b < len && tt_is_ident_char(line[b]))
    b++;

  int n = b - a;
  if (n <= 0)
    return FALSE;
  if (n >= out_sz)
    n = out_sz - 1;
  memcpy(out, line + a, (size_t)n);
  out[n] = 0;

  if (out_start)
    *out_start = a;
  if (out_end)
    *out_end = b;
  return TRUE;
}

// If this is a ".def NAME VALUE" line and hover token is NAME or VALUE, extract
// VALUE. This gives useful tooltip info even without a full evaluated symbol
// table.
static gboolean tip_def_value_for_line(AppState *s, const char *line,
                                       int hover_off, char *out_name,
                                       int out_name_sz, int *out_value,
                                       int *out_base) {
  if (!line || !out_value)
    return FALSE;

  const char *p = skip_ws(line);
  if (*p != '.')
    return FALSE;
  p++; // skip '.'

  char dir[32];
  if (!scan_ident_token(&p, dir, (int)sizeof(dir)))
    return FALSE;
  if (!editor_token_equals_ci(dir, "def"))
    return FALSE;

  p = skip_ws(p);

  // NAME
  int name_off = (int)(p - line);
  char name[128];
  if (!scan_ident_token(&p, name, (int)sizeof(name)))
    return FALSE;
  int name_end = (int)(p - line);

  p = skip_ws(p);
  int val_off = (int)(p - line);

  // VALUE expression: take until ';' or end-of-line.
  const char *expr0 = p;
  const char *q = p;
  while (*q && *q != ';' && *q != '\n' && *q != '\r')
    q++;
  int val_end = (int)(q - line);

  // Only trigger for hover on NAME or VALUE.
  if (!((hover_off >= name_off && hover_off < name_end) ||
        (hover_off >= val_off && hover_off < val_end)))
    return FALSE;

  // If the cursor is over another identifier inside the value expression
  // (e.g. ".def A B+2" while hovering over B), we want the tooltip for
  // that referenced symbol, not the define being declared.
  if (hover_off >= val_off && hover_off < val_end) {
    char ident[128];
    int ts = 0, te = 0;
    if (token_at_line_offset(line, hover_off, ident, (int)sizeof(ident), &ts,
                             &te)) {
      // If we're hovering a different symbol *inside* the define's value
      // expression, fall back to the normal symbol tooltip logic (so ACIA_BASE
      // shows its own tooltip).
      if (ts >= val_off && te <= val_end) {
        if (g_ascii_strcasecmp(ident, name) != 0)
          return FALSE;
      }
    }
  }

  // Evaluate expression.
  // First try literal-only parse to preserve base formatting (bin/dec/hex).
  int v = 0, base = 10;
  char exprbuf[192];
  int ei = 0;
  const char *e = expr0;
  while (e < q && ei + 1 < (int)sizeof(exprbuf)) {
    exprbuf[ei++] = *e++;
  }
  exprbuf[ei] = 0;

  // Trim leading/trailing whitespace.
  char *es = exprbuf;
  while (*es && g_ascii_isspace((guchar)*es))
    es++;
  char *et = es + strlen(es);
  while (et > es && g_ascii_isspace((guchar)et[-1]))
    et--;
  *et = 0;
  if (!*es)
    return FALSE;

  if (tt_parse_int_literal(es, &v, &base)) {
    // ok
  } else {
    if (!s)
      return FALSE;
    if (!tt_eval_expr_simple(s, es, 0, &v))
      return FALSE;
    base = 16; // for expressions, display as hex by default.
  }

  if (out_name && out_name_sz > 0) {
    g_strlcpy(out_name, name, (gsize)out_name_sz);
  }
  if (out_base)
    *out_base = base;
  *out_value = v;
  return TRUE;
}

static void tt_format_hex(char *out, size_t out_sz, unsigned v) {
  if (v <= 0xFFu)
    snprintf(out, out_sz, "$%02X", v);
  else if (v <= 0xFFFFu)
    snprintf(out, out_sz, "$%04X", v);
  else if (v <= 0xFFFFFFu)
    snprintf(out, out_sz, "$%06X", v);
  else
    snprintf(out, out_sz, "$%08X", v);
}

/* Format v as a binary literal string with bits digits into out. */
static void tt_format_bin(char *out, size_t out_sz, unsigned v, int bits) {
  // bits: 8/16/24/32
  int n = 0;
  if (out_sz == 0)
    return;
  out[n++] = '%';
  for (int i = bits - 1; i >= 0 && (size_t)n + 1 < out_sz; --i)
    out[n++] = (v & (1u << i)) ? '1' : '0';
  out[n] = 0;
}

static char *tt_format_value_full(unsigned v, int literal_base) {
  char hexbuf[32], binbuf[64], buf[192];
  tt_format_hex(hexbuf, sizeof(hexbuf), v);
  binbuf[0] = 0;

  if (literal_base == 2) {
    int bits = (v <= 0xFFu)       ? 8
               : (v <= 0xFFFFu)   ? 16
               : (v <= 0xFFFFFFu) ? 24
                                  : 32;
    tt_format_bin(binbuf, sizeof(binbuf), v, bits);
    snprintf(buf, sizeof(buf), "Value: %s (%u)\nHex: %s", binbuf, v, hexbuf);
  } else if (literal_base == 10) {
    snprintf(buf, sizeof(buf), "Value: %u\nHex: %s", v, hexbuf);
  } else { // default hex
    snprintf(buf, sizeof(buf), "Value: %s (%u)", hexbuf, v);
  }
  return g_strdup(buf);
}

static char *format_number_hint(int v) {
  unsigned uv = (unsigned)(v & 0xFFFFFFFFu);
  // no base info here; default to hex+dec
  char *s = tt_format_value_full(uv, 16);
  return s;
}

static char *addressing_mode_hint_for_line(const char *line) {
  // Very lightweight heuristic: parse mnemonic + operand text and classify.
  if (!line)
    return NULL;
  const char *p = line;
  p = skip_ws(p);

  // Directives (e.g. .def/.byte/.org) are not instructions.
  // Don't show addressing-mode hints on directive lines.
  if (*p == '.')
    return NULL;

  // Optional label definition (name: or :name)
  {
    const char *save = p;
    char t[64];
    const char *q = p;
    if (scan_ident_token(&q, t, (int)sizeof(t))) {
      q = skip_ws(q);
      if (*q == ':') {
        p = q + 1;
        p = skip_ws(p);
      } else {
        p = save;
      }
    } else if (*p == ':') {
      // :local label
      p++;
      while (*p && tt_is_ident_char(*p))
        p++;
      p = skip_ws(p);
    }
  }

  char mn[16] = {0};
  if (!scan_ident_token(&p, mn, (int)sizeof(mn)))
    return NULL;

  const char *op = skip_ws(p);
  if (!*op)
    return g_strdup("Instruction: (implied)");

  if (mn[0] && tt_is_branch_mnemonic(mn))
    return g_strdup("Addressing: relative branch");

  // Immediate
  if (*op == '#')
    return g_strdup("Addressing: immediate");

  // Indirect forms
  if (*op == '(') {
    if (strstr(op, ",X)") || strstr(op, ",x)"))
      return g_strdup("Addressing: (indirect,X)");
    if (strstr(op, "),Y") || strstr(op, "),y"))
      return g_strdup("Addressing: (indirect),Y");
    return g_strdup("Addressing: indirect");
  }

  // Indexed
  if (strstr(op, ",X") || strstr(op, ",x"))
    return g_strdup("Addressing: indexed,X");
  if (strstr(op, ",Y") || strstr(op, ",y"))
    return g_strdup("Addressing: indexed,Y");

  // Try to parse a literal to decide zp/abs.
  {
    char lit[64] = {0};
    const char *r = op;
    int i = 0;
    while (*r && !isspace((unsigned char)*r) && *r != ',' && *r != ';' &&
           *r != ')' && i < (int)sizeof(lit) - 1) {
      lit[i++] = *r++;
    }
    lit[i] = 0;
    int v = 0;
    if (tt_parse_int_literal(lit, &v, NULL)) {
      if ((v & ~0xFF) == 0)
        return g_strdup("Addressing: zero page (literal)");
      return g_strdup("Addressing: absolute (literal)");
    }
  }

  return g_strdup("Addressing: absolute/zero page (symbolic)");
}

static char *format_where_defined(AppState *s, const char *sym) {
  if (!s || !sym || !*sym)
    return NULL;
  GtkTextIter it;
  if (!editor_find_definition(s, sym, &it))
    return NULL;

  int line = gtk_text_iter_get_line(&it) + 1;

  // Grab a trimmed copy of the definition line for a bit more context.
  GtkTextIter ls = it, le = it;
  gtk_text_iter_set_line_offset(&ls, 0);
  gtk_text_iter_forward_to_line_end(&le);
  char *raw =
      gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &ls, &le, FALSE);

  char snippet[140];
  snippet[0] = 0;
  if (raw) {
    const char *p = raw;
    while (*p == ' ' || *p == '\t')
      p++;
    int n = 0;
    while (*p && *p != '\r' && *p != '\n' && *p != ';' &&
           n < (int)sizeof(snippet) - 1) {
      snippet[n++] = *p++;
    }
    snippet[n] = 0;
  }
  g_free(raw);

  // Scan backwards from the definition line, collecting consecutive comment
  // lines (starting with optional whitespace then ';').
  // Stop at a blank line or a non-comment line.
  GString *comments = g_string_new(NULL);
  {
    int scan_line = line - 2; // 0-based, one above def line
    // Collect lines into an array so we can reverse them
    GPtrArray *comment_lines = g_ptr_array_new_with_free_func(g_free);
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
    while (scan_line >= 0) {
      GtkTextIter cla, clb;
      gtk_text_buffer_get_iter_at_line(tb, &cla, scan_line);
      clb = cla;
      gtk_text_iter_forward_to_line_end(&clb);
      char *cl = gtk_text_buffer_get_text(tb, &cla, &clb, FALSE);
      if (!cl)
        break;
      const char *cp = cl;
      while (*cp == ' ' || *cp == '\t')
        cp++;
      if (*cp == 0) {
        // blank line — stop
        g_free(cl);
        break;
      }
      if (*cp != ';') {
        // non-comment line — stop
        g_free(cl);
        break;
      }
      // strip leading ';' and optional space
      cp++;
      if (*cp == ' ')
        cp++;
      g_ptr_array_add(comment_lines, g_strdup(cp));
      g_free(cl);
      scan_line--;
    }
    // Reverse (we collected bottom-up)
    for (int ci = (int)comment_lines->len - 1; ci >= 0; ci--) {
      const char *ctext = (const char *)comment_lines->pdata[ci];
      // trim trailing whitespace
      char ctrim[256];
      g_strlcpy(ctrim, ctext, sizeof(ctrim));
      int clen = (int)strlen(ctrim);
      while (clen > 0 && (ctrim[clen - 1] == '\r' || ctrim[clen - 1] == '\n' ||
                          ctrim[clen - 1] == ' ' || ctrim[clen - 1] == '\t'))
        ctrim[--clen] = 0;
      if (comments->len)
        g_string_append_c(comments, '\n');
      g_string_append(comments, ctrim);
    }
    g_ptr_array_free(comment_lines, TRUE);
  }

  GString *result = g_string_new(NULL);
  if (comments->len) {
    g_string_append(result, comments->str);
    g_string_append_c(result, '\n');
  }
  if (snippet[0])
    g_string_append_printf(result, "Defined at line %d: %s", line, snippet);
  else
    g_string_append_printf(result, "Defined at line %d", line);

  g_string_free(comments, TRUE);
  char *out = g_strdup(result->str);
  g_string_free(result, TRUE);
  return out;
}

/* Parse a definition line and extract the numeric value for sym if it defines
 * it. ret: TRUE if the line defines sym */
static gboolean parse_def_line_for_symbol(const char *line, const char *sym,
                                          int *out_value, int *out_base) {
  if (!line || !sym || !*sym)
    return FALSE;
  const char *p = line;

  // skip leading ws
  while (*p == ' ' || *p == '\t')
    p++;

  // must start with ".def" (case-insensitive)
  if (*p != '.')
    return FALSE;
  p++;
  if (g_ascii_strncasecmp(p, "def", 3) != 0)
    return FALSE;
  p += 3;

  // ws then symbol name
  while (*p == ' ' || *p == '\t')
    p++;

  char name[96];
  int ni = 0;
  while (*p && tt_is_ident_char(*p) && ni < (int)sizeof(name) - 1) {
    name[ni++] = *p++;
  }
  name[ni] = 0;
  if (!name[0])
    return FALSE;
  if (g_ascii_strcasecmp(name, sym) != 0)
    return FALSE;

  // ws then literal
  while (*p == ' ' || *p == '\t')
    p++;

  int v = 0, base = 16;
  const char *q = p;
  if (!tt_parse_int_literal_end(q, &v, &base, &q))
    return FALSE;

  if (out_value)
    *out_value = v;
  if (out_base)
    *out_base = base;
  return TRUE;
}

static gboolean def_value_for_symbol(AppState *s, const char *sym,
                                     int *out_value, int *out_base,
                                     int *out_def_line) {
  if (!s || !sym || !*sym)
    return FALSE;
  GtkTextIter it;
  if (!editor_find_definition(s, sym, &it))
    return FALSE;

  GtkTextIter ls = it, le = it;
  gtk_text_iter_set_line_offset(&ls, 0);
  gtk_text_iter_forward_to_line_end(&le);
  char *line =
      gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &ls, &le, FALSE);
  if (!line)
    return FALSE;

  int v = 0, base = 16;
  gboolean ok = parse_def_line_for_symbol(line, sym, &v, &base);
  g_free(line);

  if (!ok)
    return FALSE;
  if (out_value)
    *out_value = v;
  if (out_base)
    *out_base = base;
  if (out_def_line)
    *out_def_line = gtk_text_iter_get_line(&it) + 1;
  return TRUE;
}

/* GtkWidget::query-tooltip callback: build and display a hover tooltip. */
static gboolean editor_tooltip_query_cb(GtkWidget *w, int x, int y,
                                        gboolean keyboard_mode,
                                        GtkTooltip *tooltip,
                                        gpointer user_data) {
  (void)w;
  (void)keyboard_mode;
  AppState *s = (AppState *)user_data;
  if (!s || !s->view)
    return FALSE;

  GtkTextIter it;
  int hover_line0 = 0;
  int bx = x, by = y;
  gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(s->view),
                                        GTK_TEXT_WINDOW_WIDGET, x, y, &bx, &by);
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &it, bx, by);
  hover_line0 = gtk_text_iter_get_line(&it);

  // Grab the full line text.
  GtkTextIter ls = it, le = it;
  gtk_text_iter_set_line_offset(&ls, 0);
  gtk_text_iter_forward_to_line_end(&le);
  gchar *line = gtk_text_iter_get_text(&ls, &le);
  if (!line)
    return FALSE;

  int off = gtk_text_iter_get_line_offset(&it);
  char tok[128] = {0};
  int ts = 0, te = 0;
  gboolean ok =
      token_at_line_offset(line, off, tok, (int)sizeof(tok), &ts, &te);

  if (!ok) {
    g_free(line);
    return FALSE;
  }

  GString *msg = g_string_new(NULL);

  // Numeric literal hover
  int num = 0;
  if (tt_parse_int_literal(tok, &num, NULL)) {
    char *h = format_number_hint(num);
    g_string_append(msg, h);
    g_free(h);
    g_free(line);
    gtk_tooltip_set_text(tooltip, msg->str);
    g_string_free(msg, TRUE);
    return TRUE;
  }

  // ".def NAME VALUE" lines: show VALUE even when hovering NAME.
  // NOTE: on ".def" lines, we should not show addressing-mode hints.
  gboolean is_def_line = FALSE;
  {
    const char *p = skip_ws(line);
    if (p[0] == '.' && (p[1] == 'd' || p[1] == 'D') &&
        (p[2] == 'e' || p[2] == 'E') && (p[3] == 'f' || p[3] == 'F') &&
        !tt_is_ident_char(p[4]))
      is_def_line = TRUE;
  }

  int defv = 0;
  char name[96];
  int def_base = 16;
  if (tip_def_value_for_line(s, line, off, name, (int)sizeof(name), &defv,
                             &def_base)) {
    // Hovering a .def line (either over the name or the literal): show the
    // define value + location, and don't fall through into addressing-mode
    // heuristics.
    unsigned uv = (unsigned)(defv & 0xFFFFFFFFu);
    char *h = tt_format_value_full(uv, def_base);
    g_string_printf(msg,
                    "Define: %s\n%s\nDefined at line %d\n\nCtrl+Click to jump "
                    "to definition",
                    name, h ? h : "", hover_line0 + 1);
    g_free(h);
    gtk_tooltip_set_text(tooltip, msg->str);
    g_string_free(msg, TRUE);
    g_free(line);
    return TRUE;
  }

  // Anonymous labels
  if (strcmp(tok, ":+") == 0 || strcmp(tok, ":-") == 0) {
    GtkTextIter def_it;
    int line0 = gtk_text_iter_get_line(&it);
    int dir = (tok[1] == '+') ? +1 : -1;
    if (editor_find_anon_label_from_line(s, line0 + dir, dir, &def_it)) {
      int dl = gtk_text_iter_get_line(&def_it) + 1;
      g_string_append_printf(msg, "%s anonymous label at line %d",
                             (dir > 0) ? "Next" : "Previous", dl);
    } else {
      g_string_append_printf(msg, "No %s anonymous label found",
                             (dir > 0) ? "next" : "previous");
    }
    g_free(line);
    gtk_tooltip_set_text(tooltip, msg->str);
    g_string_free(msg, TRUE);
    return TRUE;
  }

  // cpu directive: jump target is first cpu directive in file.
  if (editor_token_equals_ci(tok, "cpu") ||
      editor_token_equals_ci(tok, ".cpu")) {
    GtkTextIter cpu_it;
    if (editor_find_cpu_directive(s, &cpu_it)) {
      int dl = gtk_text_iter_get_line(&cpu_it) + 1;
      // Parse the directive line to extract the selected CPU value.
      GtkTextIter ls2 = cpu_it, le2 = cpu_it;
      gtk_text_iter_set_line_offset(&ls2, 0);
      gtk_text_iter_forward_to_line_end(&le2);
      char *cpu_line =
          gtk_text_buffer_get_text(GTK_TEXT_BUFFER(s->buf), &ls2, &le2, FALSE);

      char cpu_val[32] = "";
      if (cpu_line) {
        const char *pp = cpu_line;
        pp = skip_ws(pp);
        // token ".cpu"
        char t1[32] = {0};
        if (scan_ident_token(&pp, t1, (int)sizeof(t1))) {
          pp = skip_ws(pp);
          // argument token (stop at ws or ';')
          const char *a0 = pp;
          while (*pp && *pp != ';' && *pp != '\n' && *pp != '\r' &&
                 *pp != ' ' && *pp != '\t')
            pp++;
          int n = (int)(pp - a0);
          if (n > 0) {
            if (n >= (int)sizeof(cpu_val))
              n = (int)sizeof(cpu_val) - 1;
            memcpy(cpu_val, a0, (size_t)n);
            cpu_val[n] = 0;
          }
        }
      }

      // Canonicalize display: default to 65c02 if missing (matches preproc
      // default).
      const char *disp = (cpu_val[0] ? cpu_val : "65c02");
      if (g_ascii_strcasecmp(disp, "65c02") == 0)
        disp = "65c02";
      else if (g_ascii_strcasecmp(disp, "6502u") == 0)
        disp = "6502u (undocumented opcodes enabled)";

      g_string_append_printf(msg,
                             "CPU symbol\nCurrent value: %s\nSet by .cpu at "
                             "line %d\n\nCtrl+Click to jump to definition",
                             disp, dl);
      g_free(cpu_line);
    } else {
      g_string_append(msg, "CPU symbol\nCurrent value: 65c02\nSet by: "
                           "(default)\n\nCtrl+Click to jump to definition");
    }
    g_free(line);
    gtk_tooltip_set_text(tooltip, msg->str);
    g_string_free(msg, TRUE);
    return TRUE;
  }

  // Operand / addressing mode hint (any line with an instruction)
  // Don't show this on definition lines (e.g. ".def").
  if (!is_def_line) {
    // Check if the hovered token is a macro call — suppress instruction hint.
    gboolean is_macro_tok = FALSE;
    if (tok[0] && s->active_doc) {
      GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
      GtkTextIter si, ei;
      gtk_text_buffer_get_start_iter(tb, &si);
      gtk_text_buffer_get_end_iter(tb, &ei);
      /* Scan for .macro NAME or macro NAME matching tok */
      GtkTextIter scan = si;
      while (!gtk_text_iter_equal(&scan, &ei)) {
        GtkTextIter lend = scan;
        gtk_text_iter_forward_to_line_end(&lend);
        gchar *sl = gtk_text_buffer_get_text(tb, &scan, &lend, FALSE);
        if (sl) {
          const char *pp = skip_ws(sl);
          char kw[16] = {0};
          const char *pp2 = pp;
          if (scan_ident_token(&pp2, kw, (int)sizeof(kw)) &&
              (editor_token_equals_ci(kw, ".macro") ||
               editor_token_equals_ci(kw, "macro"))) {
            char nm[128] = {0};
            const char *pp3 = skip_ws(pp2);
            if (scan_ident_token(&pp3, nm, (int)sizeof(nm)) &&
                editor_token_equals_ci(nm, tok))
              is_macro_tok = TRUE;
          }
          g_free(sl);
        }
        if (is_macro_tok)
          break;
        if (!gtk_text_iter_forward_line(&scan))
          break;
      }
    }
    if (is_macro_tok) {
      if (msg->len)
        g_string_append(msg, "\n");
      g_string_append(msg, "Macro call");
      /* Show preceding comments + definition location */
      char *wd = format_where_defined(s, tok);
      if (wd) {
        g_string_append(msg, "\n");
        g_string_append(msg, wd);
        g_free(wd);
      }
      g_string_append(msg, "\n\nCtrl+Click to jump to definition");
      g_free(line);
      gtk_tooltip_set_text(tooltip, msg->str);
      g_string_free(msg, TRUE);
      return TRUE;
    } else {
      char *mode = addressing_mode_hint_for_line(line);
      if (mode) {
        g_string_append(msg, mode);
        g_free(mode);
      }
    }
  }

  // If this token is a known .def symbol, show its resolved value.
  int sym_v = 0, sym_base = 16, sym_def_line = 0;
  if (def_value_for_symbol(s, tok, &sym_v, &sym_base, &sym_def_line)) {
    char *vh = tt_format_value_full((unsigned)(sym_v & 0xFFFFFFFFu), sym_base);
    if (vh) {
      g_string_append_printf(msg, "Symbol: %s\n%s", tok, vh);
      g_free(vh);
      g_string_append_c(msg, '\n');
    }
  }

  // Where defined (for symbols) - show only if definition exists.
  {
    char *wd = format_where_defined(s, tok);
    if (wd) {
      if (msg->len)
        g_string_append(msg, "\n");
      g_string_append(msg, wd);
      g_string_append(msg, "\n\nCtrl+Click to jump to definition");
      g_free(wd);
    }
  }

  g_free(line);

  /* If we couldn't resolve the token, check if it's just a case-mismatch
   * against known labels. */
  if (msg->len == 0 && tok[0] &&
      (g_ascii_isalpha(tok[0]) || tok[0] == '_' || tok[0] == '.')) {
    /* AppState tracks the currently active document in s->active_doc. */
    const char *canon = doc_label_index_find_ci(s->active_doc, tok);
    if (canon && strcmp(canon, tok) != 0) {
      g_string_append_printf(msg, "Not defined. Possible case mismatch: '%s'",
                             canon);
    }
  }

  if (msg->len == 0) {
    g_string_free(msg, TRUE);
    return FALSE;
  }

  gtk_tooltip_set_text(tooltip, msg->str);
  g_string_free(msg, TRUE);
  return TRUE;
}

static void editor_install_hover_tooltips(AppState *s) {
  if (!s || !s->view)
    return;
  GtkWidget *w = GTK_WIDGET(s->view);
  gtk_widget_set_has_tooltip(w, TRUE);
  g_signal_connect(w, "query-tooltip", G_CALLBACK(editor_tooltip_query_cb), s);
}

/* Search for the nth anonymous label in direction dir from cur_line.
 * ret: 1-based line number, or 0 if not found */
static int doc_find_branch_def(EditorDoc *d, int cur_line, int dir, int nth,
                               int *start_out, int *end_out) {
  // Anonymous labels:
  //   Definition: a line starting with optional whitespace then a single ':'
  //   (not ':+', ':-') Reference:  :+, :-, :++, :--, etc. (nth = number of +/-
  //   chars)
  if (!d || !start_out || !end_out)
    return 0;
  if (dir != 1 && dir != -1)
    return 0;
  if (nth <= 0)
    return 0;

  int n_lines = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(d->buf));
  if (n_lines <= 0)
    return 0;

  int found = 0;
  for (int line = cur_line + dir; line >= 0 && line < n_lines; line += dir) {
    GtkTextIter ls, le;
    gtk_text_buffer_get_iter_at_line(GTK_TEXT_BUFFER(d->buf), &ls, line);
    le = ls;
    gtk_text_iter_forward_to_line_end(&le);

    char *txt =
        gtk_text_buffer_get_text(GTK_TEXT_BUFFER(d->buf), &ls, &le, FALSE);
    if (!txt)
      continue;

    int i = 0;
    while (txt[i] == ' ' || txt[i] == '	')
      i++;

    int ok = 0;
    if (txt[i] == ':') {
      char c = txt[i + 1];
      // Definition is a single ':' followed by whitespace, comment, or EOL.
      if (c == '\0' || c == '\r' || c == '\n' || c == ' ' || c == '\t' ||
          c == ';')
        ok = 1;
    }

    if (ok) {
      found++;
      if (found == nth) {
        int base = gtk_text_iter_get_offset(&ls);
        *start_out = base + i;
        *end_out = base + i + 1;
        g_free(txt);
        return 1;
      }
    }

    g_free(txt);
  }

  return 0;
}

/* Update the hover-definition underline when the mouse is at *at. */
static void editor_hover_update(EditorDoc *d, GtkTextIter *at) {
  if (!d || !d->buf || !at)
    return;

  // Ignore comments.
  if (d->tag_comment && gtk_text_iter_has_tag(at, d->tag_comment)) {
    doc_clear_hover(d);
    return;
  }

  // Find token bounds under cursor.
  GtkTextIter a = *at;
  GtkTextIter b = *at;
  int oa = gtk_text_iter_get_offset(&a);
  int ob = oa;

  // Expand left.
  GtkTextIter t = a;
  while (gtk_text_iter_backward_char(&t)) {
    gunichar ch = gtk_text_iter_get_char(&t);
    if (ch == ':' || ch == '+' || ch == '-') {
      oa = gtk_text_iter_get_offset(&t);
      continue;
    }
    if (gui_is_label_char((int)ch)) {
      oa = gtk_text_iter_get_offset(&t);
      continue;
    }
    // stepped one too far
    gtk_text_iter_forward_char(&t);
    oa = gtk_text_iter_get_offset(&t);
    break;
  }

  // Expand right.
  t = b;
  while (!gtk_text_iter_is_end(&t)) {
    gunichar ch = gtk_text_iter_get_char(&t);
    if (ch == ':' || ch == '+' || ch == '-') {
      ob = gtk_text_iter_get_offset(&t) + 1;
      if (!gtk_text_iter_forward_char(&t))
        break;
      continue;
    }
    if (gui_is_label_char((int)ch)) {
      ob = gtk_text_iter_get_offset(&t) + 1;
      if (!gtk_text_iter_forward_char(&t))
        break;
      continue;
    }
    break;
  }

  if (ob <= oa) {
    doc_clear_hover(d);
    return;
  }

  GtkTextIter ia, ib;
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(d->buf), &ia, oa);
  gtk_text_buffer_get_iter_at_offset(GTK_TEXT_BUFFER(d->buf), &ib, ob);
  char *tok =
      gtk_text_buffer_get_text(GTK_TEXT_BUFFER(d->buf), &ia, &ib, FALSE);
  if (!tok || !*tok) {
    if (tok)
      g_free(tok);
    doc_clear_hover(d);
    return;
  }

  // Ensure label index is available (rebuilt during highlighting, but keep
  // safe).
  if (!d->label_defs)
    d->label_defs =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

  // Resolve :+ / :- to the corresponding definition.
  if (tok[0] == ':' && (tok[1] == '+' || tok[1] == '-')) {
    int ok = 1;
    int nth = 0;
    for (int i = 1; tok[i]; i++) {
      if (tok[i] != tok[1]) {
        ok = 0;
        break;
      }
      nth++;
    }
    if (ok && nth > 0) {
      int hs = -1, he = -1;
      int dir = (tok[1] == '+') ? 1 : -1;
      if (doc_find_branch_def(d, gtk_text_iter_get_line(at), dir, nth, &hs,
                              &he))
        doc_apply_hover(d, hs, he);
      else
        doc_clear_hover(d);
      g_free(tok);
      return;
    }
  }

  // Normal label references: only act if we have a label definition for this
  // name.
  int *span = (int *)g_hash_table_lookup(d->label_defs, tok);
  if (!span && tok[0] == '.' && tok[1])
    span = (int *)g_hash_table_lookup(d->label_defs, tok + 1);
  if (span) {
    doc_apply_hover(d, span[0], span[1]);
  } else {
    doc_clear_hover(d);
  }

  g_free(tok);
}

static void hover_motion_cb(GtkEventControllerMotion *c, double x, double y,
                            gpointer user_data) {
  (void)c;
  EditorDoc *d = (EditorDoc *)user_data;
  if (!d || !d->view)
    return;
  GtkTextView *tv = GTK_TEXT_VIEW(d->view);
  int bx = (int)x, by = (int)y;
  gtk_text_view_window_to_buffer_coords(tv, GTK_TEXT_WINDOW_WIDGET, (int)x,
                                        (int)y, &bx, &by);
  GtkTextIter it;
  gtk_text_view_get_iter_at_location(tv, &it, bx, by);
  editor_hover_update(d, &it);
}

/* Motion-leave callback: clear hover underline when mouse leaves editor. */
static void hover_leave_cb(GtkEventControllerMotion *c, gpointer user_data) {
  (void)c;
  EditorDoc *d = (EditorDoc *)user_data;
  if (d)
    doc_clear_hover(d);
}

static void editor_install_hover_label_defs(EditorDoc *d) {
  if (!d || !d->view)
    return;
  GtkEventController *m = gtk_event_controller_motion_new();
  g_signal_connect(m, "motion", G_CALLBACK(hover_motion_cb), d);
  g_signal_connect(m, "leave", G_CALLBACK(hover_leave_cb), d);
  gtk_widget_add_controller(GTK_WIDGET(d->view), m);
}

/* Show all references to sym in a list dialog, allowing jump to any occurrence.
 */
static void editor_show_references(AppState *s, const char *sym) {
  GPtrArray *hits = editor_collect_references(s, sym);

  if (!hits || hits->len == 0) {
    if (hits)
      g_ptr_array_unref(hits);
    show_info(s, "No references found for that symbol.");
    return;
  }

  GtkWidget *dlg = gtk_dialog_new_with_buttons("References", GTK_WINDOW(s->win),
                                               GTK_DIALOG_MODAL, "Close",
                                               GTK_RESPONSE_CLOSE, NULL);

  // Make the Close button (and ESC) actually close the dialog.
  g_signal_connect(dlg, "response", G_CALLBACK(dialog_destroy_on_response),
                   NULL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  gtk_widget_set_margin_start(box, 8);
  gtk_widget_set_margin_end(box, 8);
  gtk_box_append(GTK_BOX(area), box);

  char title[256];
  snprintf(title, sizeof(title), "References to '%s' (%u):", sym,
           (unsigned)hits->len);
  GtkWidget *lbl = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_box_append(GTK_BOX(box), lbl);

  GtkWidget *sc = gtk_scrolled_window_new();
  gtk_widget_set_size_request(sc, 640, 360);
  gtk_box_append(GTK_BOX(box), sc);

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), list);

  for (guint i = 0; i < hits->len; i++) {
    RefHit *h = (RefHit *)g_ptr_array_index(hits, i);
    char linebuf[1024];
    /* show line/col and snippet */
    snprintf(linebuf, sizeof(linebuf), "%d:%d  %s", h->line, h->col,
             h->text ? h->text : "");
    GtkWidget *r = gtk_label_new(linebuf);
    gtk_label_set_xalign(GTK_LABEL(r), 0.0f);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), r);
    g_object_set_data(G_OBJECT(row), "ref-offset", GINT_TO_POINTER(h->offset));
    gtk_list_box_append(GTK_LIST_BOX(list), row);
  }

  g_signal_connect(list, "row-activated", G_CALLBACK(refs_row_activated), s);

  gtk_window_present(GTK_WINDOW(dlg));
  g_ptr_array_unref(hits);
}

/* ----- Actions ----- */

static void editor_jump_to_pos(AppState *s, int line, int col) {
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
  int nlines = gtk_text_buffer_get_line_count(tb);
  if (line < 0)
    line = 0;
  if (line >= nlines)
    line = nlines - 1;
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_line(tb, &it, line);
  int llen = gtk_text_iter_get_chars_in_line(&it);
  if (col < 0)
    col = 0;
  if (col >= llen)
    col = MAX(0, llen - 1);
  gtk_text_iter_set_line_offset(&it, col);
  gtk_text_buffer_place_cursor(tb, &it);
  gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &it, 0.0, TRUE, 0.5,
                               0.1);
}

/* Action handler: navigate back in the jump history. */
static void action_jump_back(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  int line, col;
  if (jump_buf_back(&s->jump_buf, &line, &col))
    editor_jump_to_pos(s, line, col);
  else
    set_status(s, "Jump history: at start");
}

/* Action handler: navigate forward in the jump history. */
static void action_jump_fwd(GSimpleAction *a, GVariant *p, gpointer ud) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)ud;
  int line, col;
  if (jump_buf_fwd(&s->jump_buf, &line, &col))
    editor_jump_to_pos(s, line, col);
  else
    set_status(s, "Jump history: at end");
}

/* Action handler: jump to the definition of the symbol under the cursor. */
static void action_jump_def(GSimpleAction *a, GVariant *p, gpointer ud) {
  AppState *s = (AppState *)ud;
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(
      GTK_TEXT_BUFFER(s->buf), &it,
      gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(s->buf)));
  char *sym = editor_symbol_at_iter(s, &it);
  if (!sym) {
    show_info(s, "No symbol under cursor.");
    return;
  }
  editor_jump_to_definition(s, sym);
  g_free(sym);
}

/* Action handler: show all references to the symbol under the cursor. */
static void action_find_refs(GSimpleAction *a, GVariant *p, gpointer ud) {
  AppState *s = (AppState *)ud;
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(
      GTK_TEXT_BUFFER(s->buf), &it,
      gtk_text_buffer_get_insert(GTK_TEXT_BUFFER(s->buf)));
  char *sym = editor_symbol_at_iter(s, &it);
  if (!sym) {
    show_info(s, "No symbol under cursor.");
    return;
  }
  editor_show_references(s, sym);
  g_free(sym);
}

/* ----- Ctrl+Click (GTK4-safe) ----- */

static void editor_ctrl_click_at_xy(AppState *s, double x, double y) {
  GtkTextIter it;
  /* Coordinates are widget-relative in GTK4 for text view hit-testing */
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &it, (int)x,
                                     (int)y);
  char *sym = editor_symbol_at_iter(s, &it);
  if (!sym)
    return;
  editor_jump_to_definition(s, sym);
  g_free(sym);
}

static gboolean editor_legacy_event_cb(GtkEventControllerLegacy *c,
                                       GdkEvent *event, gpointer ud) {
  AppState *s = (AppState *)ud;

  /* Use BUTTON_RELEASE so GtkTextView has already moved the caret to the click.
   * This avoids any coordinate-space ambiguity across GTK/GDK versions.
   */
  if (gdk_event_get_event_type(event) == GDK_BUTTON_PRESS) {
    guint button_press = gdk_button_event_get_button(event);
    /* Fold toggle via GtkSourceGutter click.
     * The GtkSourceView gutter (line numbers) is a child widget to the
     * left of the text window. Events on it arrive here with x < 0 in
     * buffer coords, or with widget-x within the gutter width (~40px). */
    if (button_press == 1 && (s->fold_comments || s->fold_macros) &&
        s->fold_map.lines) {
      double ex, ey;
      gdk_event_get_position(event, &ex, &ey);
      gint bx, by;
      gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(s->view),
                                            GTK_TEXT_WINDOW_WIDGET, (gint)ex,
                                            (gint)ey, &bx, &by);
      /* bx < 0 means click is in the gutter (left of text area) */
      if (bx < 0) {
        GtkTextIter it;
        gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &it, 0, by);
        gtk_fold_toggle_line(s, gtk_text_iter_get_line(&it));
        return GDK_EVENT_STOP;
      }
    }
  }

  if (gdk_event_get_event_type(event) == GDK_BUTTON_RELEASE) {
    guint button = gdk_button_event_get_button(event);
    GdkModifierType state = gdk_event_get_modifier_state(event);

    if (button == 1 && (state & GDK_CONTROL_MASK)) {
      GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->view));
      GtkTextIter it;
      gtk_text_buffer_get_iter_at_mark(b, &it, gtk_text_buffer_get_insert(b));

      if (!editor_iter_in_comment_or_string(s, &it)) {
        char *sym = editor_symbol_at_iter(s, &it);
        if (sym) {
          editor_jump_to_definition(s, sym);
          g_free(sym);
          return GDK_EVENT_STOP;
        }
      }
    }
  }
  return GDK_EVENT_PROPAGATE;
}

/* ---------------------------------------------------------------------------
 * Editor navigation key overrides:
 *   Home        → top visible line, col 0  (no scroll)
 *   End         → bottom visible line, col 0  (no scroll)
 *   Ctrl+Up     → same as Home
 *   Ctrl+Down   → same as End
 *   Ctrl+Home   → first line of file, col 0
 *   Ctrl+End    → last line of file, col 0
 * Ctrl+Left / Ctrl+Right are already handled natively by GtkTextView.
 * ---------------------------------------------------------------------------
 */
static gboolean editor_nav_key_pressed(GtkEventControllerKey *controller,
                                       guint keyval, guint keycode,
                                       GdkModifierType state,
                                       gpointer user_data) {
  (void)controller;
  (void)keycode;
  AppState *s = (AppState *)user_data;
  if (!s || !s->view || !s->buf)
    return GDK_EVENT_PROPAGATE;

  gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
  gboolean plain = !(state & (GDK_SHIFT_MASK | GDK_ALT_MASK | GDK_SUPER_MASK));

  /* Ctrl+] — toggle fold at cursor line */
  if (ctrl && plain && keyval == GDK_KEY_bracketright &&
      (s->fold_comments || s->fold_macros)) {
    GtkTextBuffer *tb2 = GTK_TEXT_BUFFER(s->buf);
    GtkTextIter it2;
    gtk_text_buffer_get_iter_at_mark(tb2, &it2,
                                     gtk_text_buffer_get_insert(tb2));
    gtk_fold_toggle_line(s, gtk_text_iter_get_line(&it2));
    return GDK_EVENT_STOP;
  }

  /* Helper: place cursor at (line, col 0) without scrolling */
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  gboolean is_home = (keyval == GDK_KEY_Home || keyval == GDK_KEY_KP_Home);
  gboolean is_end = (keyval == GDK_KEY_End || keyval == GDK_KEY_KP_End);
  gboolean is_ctrl_up =
      ctrl && plain && (keyval == GDK_KEY_Up || keyval == GDK_KEY_KP_Up);
  gboolean is_ctrl_down =
      ctrl && plain && (keyval == GDK_KEY_Down || keyval == GDK_KEY_KP_Down);
  gboolean is_ctrl_home = ctrl && plain && is_home;
  gboolean is_ctrl_end = ctrl && plain && is_end;

  /* Ctrl+Home: jump to very first line */
  if (is_ctrl_home) {
    GtkTextIter it;
    gtk_text_buffer_get_start_iter(tb, &it);
    gtk_text_buffer_place_cursor(tb, &it);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &it, 0.0, TRUE, 0.0,
                                 0.0);
    return GDK_EVENT_STOP;
  }

  /* Ctrl+End: jump to very last line */
  if (is_ctrl_end) {
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(tb, &it);
    gtk_text_iter_set_line_offset(&it, 0);
    gtk_text_buffer_place_cursor(tb, &it);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(s->view), &it, 0.0, TRUE, 0.0,
                                 1.0);
    return GDK_EVENT_STOP;
  }

  /* Home / Ctrl+Up: move to top visible line, col 0 — no scroll */
  if ((is_home && !ctrl) || is_ctrl_up) {
    GdkRectangle vr;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(s->view), &vr);
    GtkTextIter it;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &it, vr.x, vr.y);
    gtk_text_iter_set_line_offset(&it, 0);
    gtk_text_buffer_place_cursor(tb, &it);
    return GDK_EVENT_STOP;
  }

  /* End / Ctrl+Down: move to bottom visible line, col 0 — no scroll */
  if ((is_end && !ctrl) || is_ctrl_down) {
    GdkRectangle vr;
    gtk_text_view_get_visible_rect(GTK_TEXT_VIEW(s->view), &vr);
    GtkTextIter it;
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &it, vr.x,
                                       vr.y + vr.height - 1);
    gtk_text_iter_set_line_offset(&it, 0);
    gtk_text_buffer_place_cursor(tb, &it);
    return GDK_EVENT_STOP;
  }

  return GDK_EVENT_PROPAGATE;
}

/* ── Alt+numpad character entry (Windows-style) ───────────────────────────
   Hold Alt, type decimal digits on the numpad, release Alt → insert char.
   Supports 0–255 (Windows-1252 mapping for 128–159, ISO-8859-1 otherwise).
   ─────────────────────────────────────────────────────────────────────── */

/* Windows-1252 code page mapping for bytes 0x80–0x9F */
static const gunichar win1252_map[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

static gboolean editor_alt_numpad_key_pressed(GtkEventControllerKey *ctl,
                                              guint keyval, guint keycode,
                                              GdkModifierType state,
                                              gpointer user_data) {
  (void)ctl;
  (void)keycode;
  AppState *s = (AppState *)user_data;
  if (!s)
    return FALSE;

  /* Only fire when Alt is held and no other modifiers (Ctrl, Shift, Super) */
  if (!(state & GDK_ALT_MASK))
    return FALSE;
  if (state & (GDK_CONTROL_MASK | GDK_SUPER_MASK))
    return FALSE;

  /* Map numpad keys to digit values — handle both NumLock on and off */
  int digit = -1;
  switch (keyval) {
  case GDK_KEY_KP_0:
  case GDK_KEY_KP_Insert:
    digit = 0;
    break;
  case GDK_KEY_KP_1:
  case GDK_KEY_KP_End:
    digit = 1;
    break;
  case GDK_KEY_KP_2:
  case GDK_KEY_KP_Down:
    digit = 2;
    break;
  case GDK_KEY_KP_3:
  case GDK_KEY_KP_Page_Down:
    digit = 3;
    break;
  case GDK_KEY_KP_4:
  case GDK_KEY_KP_Left:
    digit = 4;
    break;
  case GDK_KEY_KP_5:
  case GDK_KEY_KP_Begin:
    digit = 5;
    break;
  case GDK_KEY_KP_6:
  case GDK_KEY_KP_Right:
    digit = 6;
    break;
  case GDK_KEY_KP_7:
  case GDK_KEY_KP_Home:
    digit = 7;
    break;
  case GDK_KEY_KP_8:
  case GDK_KEY_KP_Up:
    digit = 8;
    break;
  case GDK_KEY_KP_9:
  case GDK_KEY_KP_Page_Up:
    digit = 9;
    break;
  default:
    break;
  }
  if (digit < 0)
    return FALSE;

  /* Accumulate — cap at 255 to match Windows behaviour */
  s->alt_numpad_active = TRUE;
  s->alt_numpad_accum = s->alt_numpad_accum * 10 + digit;
  if (s->alt_numpad_accum > 255)
    s->alt_numpad_accum = 255;

  return TRUE; /* consume — prevent default Alt+numpad actions */
}

static gboolean editor_alt_numpad_key_released(GtkEventControllerKey *ctl,
                                               guint keyval, guint keycode,
                                               GdkModifierType state,
                                               gpointer user_data) {
  (void)ctl;
  (void)keycode;
  (void)state;
  AppState *s = (AppState *)user_data;
  if (!s)
    return FALSE;

  /* Only act on Alt release */
  if (keyval != GDK_KEY_Alt_L && keyval != GDK_KEY_Alt_R)
    return FALSE;
  if (!s->alt_numpad_active)
    return FALSE;

  int code = s->alt_numpad_accum;
  s->alt_numpad_active = FALSE;
  s->alt_numpad_accum = 0;

  if (code <= 0)
    return FALSE;

  /* Map code → Unicode codepoint (Windows-1252 for 128–159, else identity) */
  gunichar uc;
  if (code >= 0x80 && code <= 0x9F) {
    uc = win1252_map[code - 0x80];
    if (uc == 0)
      return FALSE; /* undefined in Windows-1252 */
  } else {
    uc = (gunichar)code;
  }
  if (uc == 0)
    return FALSE;

  /* Convert to UTF-8 and insert at cursor */
  char utf8[8];
  int len = (int)g_unichar_to_utf8(uc, utf8);
  if (len <= 0)
    return FALSE;
  utf8[len] = '\0';

  if (s->buf) {
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
    gtk_text_buffer_begin_user_action(tb);
    /* Delete selection if any, then insert */
    gtk_text_buffer_delete_selection(tb, TRUE, TRUE);
    GtkTextMark *ins = gtk_text_buffer_get_insert(tb);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(tb, &it, ins);
    gtk_text_buffer_insert(tb, &it, utf8, len);
    gtk_text_buffer_end_user_action(tb);
  }
  return TRUE;
}

static void editor_install_nav_keys(AppState *s) {
  if (!s || !s->view)
    return;
  GtkEventController *kc = gtk_event_controller_key_new();
  /* CAPTURE phase: intercept before GtkTextView's built-in bindings fire */
  gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
  g_signal_connect(kc, "key-pressed", G_CALLBACK(editor_nav_key_pressed), s);
  gtk_widget_add_controller(GTK_WIDGET(s->view), kc);

  /* Alt+numpad character entry (Windows-style) */
  GtkEventController *alt_kc = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(alt_kc, GTK_PHASE_CAPTURE);
  g_signal_connect(alt_kc, "key-pressed",
                   G_CALLBACK(editor_alt_numpad_key_pressed), s);
  g_signal_connect(alt_kc, "key-released",
                   G_CALLBACK(editor_alt_numpad_key_released), s);
  gtk_widget_add_controller(GTK_WIDGET(s->view), alt_kc);
}

static void editor_context_menu_pressed(GtkGestureClick *gesture, int n_press,
                                        double x, double y,
                                        gpointer user_data) {
  (void)n_press;
  AppState *s = (AppState *)user_data;
  if (!s || !s->context_menu)
    return;

  /* Move the cursor to the clicked position so bookmark_toggle operates
     on the right-clicked line, not wherever the cursor previously was. */
  if (s->view && s->buf) {
    GtkTextIter iter;
    int bx, by;
    gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(s->view),
                                          GTK_TEXT_WINDOW_WIDGET, (int)x,
                                          (int)y, &bx, &by);
    gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &iter, bx, by);
    /* Only move cursor if nothing is selected — preserve selection for
       clipboard operations */
    GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);
    if (!gtk_text_buffer_get_has_selection(tb))
      gtk_text_buffer_place_cursor(tb, &iter);
  }

  /* Update bookmark sensitivity based on the now-current cursor line */
  int cur_line = gtk_get_cursor_line(s);
  bool at_max = (s->bookmark_count >= GTK_MAX_BOOKMARKS);
  bool already = (gtk_bookmark_find(s, cur_line) >= 0);
  GAction *bm_act =
      g_action_map_lookup_action(G_ACTION_MAP(s->app), "bookmark_toggle");
  if (bm_act)
    g_simple_action_set_enabled(G_SIMPLE_ACTION(bm_act), !at_max || already);

  /* Position and show our popover */
  GdkRectangle rect = {(int)x, (int)y, 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(s->context_menu), &rect);
  gtk_popover_popup(GTK_POPOVER(s->context_menu));

  /* Claim the event so GtkTextView's built-in menu doesn't also appear */
  gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void editor_install_context_menu(AppState *s) {
  if (!s || !s->view)
    return;

  /* Make app actions reachable from the popover */
  gtk_widget_insert_action_group(GTK_WIDGET(s->view), "app",
                                 G_ACTION_GROUP(s->app));

  /* Build a single unified menu using GTK_POPOVER_MENU_NESTED so that
     submenus open on hover rather than requiring a click. */
  GMenu *menu = g_menu_new();

  /* Clipboard section */
  GMenu *clip = g_menu_new();
  g_menu_append(clip, "Cut",                  "app.cut");
  g_menu_append(clip, "Copy",                 "app.copy");
  g_menu_append(clip, "Copy without Comments","app.copy_without_comments");
  g_menu_append(clip, "Paste",                "app.paste");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(clip));
  g_object_unref(clip);

  /* Undo/Redo section */
  GMenu *undoredo = g_menu_new();
  g_menu_append(undoredo, "Undo", "text.undo");
  g_menu_append(undoredo, "Redo", "text.redo");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(undoredo));
  g_object_unref(undoredo);

  /* Change case submenu */
  GMenu *case_sub = g_menu_new();
  g_menu_append(case_sub, "Upper case", "source.change-case::upper");
  g_menu_append(case_sub, "Lower case", "source.change-case::lower");
  g_menu_append(case_sub, "Toggle case", "source.change-case::toggle");
  g_menu_append(case_sub, "Title case", "source.change-case::title");
  GMenu *case_sec = g_menu_new();
  g_menu_append_submenu(case_sec, "Change Case", G_MENU_MODEL(case_sub));
  g_object_unref(case_sub);
  g_menu_append_section(menu, NULL, G_MENU_MODEL(case_sec));
  g_object_unref(case_sec);

  /* Bookmark section */
  GMenu *bm = g_menu_new();
  g_menu_append(bm, "Toggle Bookmark", "app.bookmark_toggle");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(bm));
  g_object_unref(bm);

  /* Create nested popover (hover-open submenus) */
  GtkWidget *popover = gtk_popover_menu_new_from_model_full(
      G_MENU_MODEL(menu), GTK_POPOVER_MENU_NESTED);
  g_object_unref(menu);
  gtk_popover_set_has_arrow(GTK_POPOVER(popover), FALSE);
  gtk_widget_set_parent(popover, GTK_WIDGET(s->view));
  s->context_menu = popover;

  /* Capture-phase right-click: we handle it, GtkTextView won't see it */
  GtkGesture *gc = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gc), 3);
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(gc),
                                             GTK_PHASE_CAPTURE);
  g_signal_connect(gc, "pressed", G_CALLBACK(editor_context_menu_pressed), s);
  gtk_widget_add_controller(GTK_WIDGET(s->view), GTK_EVENT_CONTROLLER(gc));
}

static void editor_install_ctrl_click(AppState *s) {
  GtkEventController *lc = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(lc, GTK_PHASE_BUBBLE);
  g_signal_connect(lc, "event", G_CALLBACK(editor_legacy_event_cb), s);
  gtk_widget_add_controller(GTK_WIDGET(s->view), lc);
}

/*
 * Ctrl+Hover cursor hint:
 * When Ctrl is held and the pointer is over something that has a navigable
 * definition (label/symbol, anon label, .cpu/.CPU), show a hand cursor.
 *
 * This is deliberately lightweight: it reuses the existing symbol extraction
 * and definition lookup code, and only changes the cursor.
 */

typedef struct CtrlHoverState {
  AppState *app;
  GtkEventControllerMotion *motion;
  GtkEventControllerKey *key;
  GdkCursor *cursor_hand;
  gboolean ctrl_down;
  gboolean hand_active;
  double last_x, last_y;
} CtrlHoverState;

static void ctrl_hover_set_cursor(AppState *s, CtrlHoverState *st,
                                  gboolean hand) {
  if (!s || !st || !GTK_IS_WIDGET(s->view))
    return;
  if (hand == st->hand_active)
    return;
  st->hand_active = hand;

  /* Prefer setting the cursor on the toplevel surface; some text widgets
     don't honor gtk_widget_set_cursor() reliably across GTK4 versions. */
  GtkNative *native = gtk_widget_get_native(GTK_WIDGET(s->view));
  if (native) {
    GdkSurface *surface = gtk_native_get_surface(native);
    if (surface) {
      if (hand && !st->cursor_hand)
        st->cursor_hand = gdk_cursor_new_from_name("pointer", NULL);
      gdk_surface_set_cursor(surface, hand ? st->cursor_hand : NULL);
      return;
    }
  }

  /* Fallback: per-widget cursor. */
  if (hand && !st->cursor_hand)
    st->cursor_hand = gdk_cursor_new_from_name("pointer", NULL);
  gtk_widget_set_cursor(GTK_WIDGET(s->view), hand ? st->cursor_hand : NULL);
}

static void editor_ctrl_hover_update(AppState *s, CtrlHoverState *st) {
  if (!st->ctrl_down) {
    ctrl_hover_set_cursor(s, st, FALSE);
    return;
  }

  /* If we haven't seen a motion event yet, don't attempt a lookup at (0,0). */
  if (st->last_x < 0 || st->last_y < 0) {
    ctrl_hover_set_cursor(s, st, FALSE);
    return;
  }

  GtkTextIter it;
  gtk_text_view_get_iter_at_location(GTK_TEXT_VIEW(s->view), &it,
                                     (int)st->last_x, (int)st->last_y);
  if (editor_iter_in_comment_or_string(s, &it)) {
    ctrl_hover_set_cursor(s, st, FALSE);
    return;
  }

  char *sym = editor_symbol_at_iter(s, &it);
  if (!sym) {
    ctrl_hover_set_cursor(s, st, FALSE);
    return;
  }

  GtkTextIter def_it;
  gboolean has_def = editor_find_definition(s, sym, &def_it);
  g_free(sym);

  ctrl_hover_set_cursor(s, st, has_def);
}

/* Return TRUE if keyval is a Ctrl modifier key (left or right). */
static gboolean ctrl_key_is_ctrl(guint keyval) {
  return keyval == GDK_KEY_Control_L || keyval == GDK_KEY_Control_R;
}

static gboolean ctrl_hover_key_pressed(GtkEventControllerKey *k, guint keyval,
                                       guint keycode, GdkModifierType state,
                                       gpointer user_data) {
  (void)k;
  (void)keycode;
  CtrlHoverState *st = (CtrlHoverState *)user_data;
  if (ctrl_key_is_ctrl(keyval)) {
    st->ctrl_down = TRUE;
  } else {
    st->ctrl_down = (state & GDK_CONTROL_MASK) != 0;
  }
  if (st->app)
    editor_ctrl_hover_update(st->app, st);
  return FALSE;
}

/* Key-release handler: restore normal cursor when Ctrl is released. */
static void ctrl_hover_key_released(GtkEventControllerKey *k, guint keyval,
                                    guint keycode, GdkModifierType state,
                                    gpointer user_data) {
  (void)k;
  (void)keycode;
  CtrlHoverState *st = (CtrlHoverState *)user_data;
  if (ctrl_key_is_ctrl(keyval)) {
    st->ctrl_down = FALSE;
  } else {
    st->ctrl_down = (state & GDK_CONTROL_MASK) != 0;
  }
  if (st->app)
    editor_ctrl_hover_update(st->app, st);
}

/* Mouse-motion handler: update cursor and hover highlight while Ctrl is held.
 */
static void ctrl_hover_motion(GtkEventControllerMotion *m, double x, double y,
                              gpointer user_data) {
  (void)m;
  struct {
    AppState *s;
    CtrlHoverState *st;
  } *ctx = user_data;
  ctx->st->last_x = x;
  ctx->st->last_y = y;
  editor_ctrl_hover_update(ctx->s, ctx->st);
}

/* Mouse-leave handler: restore normal cursor when mouse leaves editor. */
static void ctrl_hover_leave(GtkEventControllerMotion *m, gpointer user_data) {
  (void)m;
  struct {
    AppState *s;
    CtrlHoverState *st;
  } *ctx = user_data;
  ctrl_hover_set_cursor(ctx->s, ctx->st, FALSE);
}

static void editor_install_ctrl_hover_cursor(AppState *s) {
  CtrlHoverState *st = g_new0(CtrlHoverState, 1);
  st->app = s;
  st->cursor_hand = gdk_cursor_new_from_name("pointer", NULL);
  st->ctrl_down = FALSE;
  st->hand_active = FALSE;
  st->last_x = st->last_y = -1.0;

  /* Motion tracking */
  st->motion = GTK_EVENT_CONTROLLER_MOTION(gtk_event_controller_motion_new());
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(st->motion),
                                             GTK_PHASE_BUBBLE);
  struct {
    AppState *s;
    CtrlHoverState *st;
  } *ctx = g_new0(typeof(*ctx), 1);
  ctx->s = s;
  ctx->st = st;
  g_signal_connect(st->motion, "motion", G_CALLBACK(ctrl_hover_motion), ctx);
  g_signal_connect(st->motion, "leave", G_CALLBACK(ctrl_hover_leave), ctx);
  gtk_widget_add_controller(GTK_WIDGET(s->view),
                            GTK_EVENT_CONTROLLER(st->motion));

  /* Key tracking (Ctrl down) */
  st->key = GTK_EVENT_CONTROLLER_KEY(gtk_event_controller_key_new());
  gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(st->key),
                                             GTK_PHASE_BUBBLE);
  g_signal_connect(st->key, "key-pressed", G_CALLBACK(ctrl_hover_key_pressed),
                   st);
  g_signal_connect(st->key, "key-released", G_CALLBACK(ctrl_hover_key_released),
                   st);
  gtk_widget_add_controller(GTK_WIDGET(s->view), GTK_EVENT_CONTROLLER(st->key));

  /* Ownership: tie everything to the view so it gets freed automatically. */
  g_object_set_data_full(G_OBJECT(s->view), "ctrl-hover-state", st,
                         (GDestroyNotify)g_free);
  g_object_set_data_full(G_OBJECT(s->view), "ctrl-hover-ctx", ctx,
                         (GDestroyNotify)g_free);
}

/* ---- Bookmark actions ---- */
static int gtk_get_cursor_line(AppState *s) {
  if (!s || !s->view)
    return 0;
  GtkTextBuffer *tb = gtk_text_view_get_buffer(GTK_TEXT_VIEW(s->view));
  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(tb, &it, gtk_text_buffer_get_insert(tb));
  return gtk_text_iter_get_line(&it);
}

static void action_bookmark_toggle(GSimpleAction *a, GVariant *p,
                                   gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  int line = gtk_get_cursor_line(s);
  gtk_bookmark_toggle(s, line);
  /* save immediately so it survives crashes */
  EditorDoc *d = s->docs ? (EditorDoc *)g_ptr_array_index(s->docs, 0) : NULL;
  /* find active doc */
  if (s->notebook) {
    int pg = gtk_notebook_get_current_page(s->notebook);
    if (pg >= 0 && s->docs && (guint)pg < s->docs->len)
      d = (EditorDoc *)g_ptr_array_index(s->docs, (guint)pg);
  }
  gtk_bookmarks_save(s, d && d->path ? d->path : "");
}

static void action_bookmark_next(GSimpleAction *a, GVariant *p,
                                 gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  int nl = gtk_bookmark_next(s, gtk_get_cursor_line(s));
  if (nl >= 0)
    gtk_bookmark_jump(s, nl);
}

static void action_bookmark_prev(GSimpleAction *a, GVariant *p,
                                 gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  int nl = gtk_bookmark_prev(s, gtk_get_cursor_line(s));
  if (nl >= 0)
    gtk_bookmark_jump(s, nl);
}

/* Action handler: clear all bookmarks in the active document. */
static void action_bookmark_clear(GSimpleAction *a, GVariant *p,
                                  gpointer user_data) {
  (void)a;
  (void)p;
  AppState *s = (AppState *)user_data;
  s->bookmark_count = 0;
  gtk_bookmark_redraw(s);
}

/* Register all GAction handlers and keyboard shortcuts for the application. */
static void install_actions_and_shortcuts(AppState *s) {
  add_action(s->app, "open", G_CALLBACK(action_open), s);
  add_action(s->app, "undo", G_CALLBACK(action_undo), s);
  add_action(s->app, "redo", G_CALLBACK(action_redo), s);
  /* Recent files action — takes a string path as parameter */
  {
    GSimpleAction *ra =
        g_simple_action_new("open_recent", G_VARIANT_TYPE_STRING);
    g_signal_connect(ra, "activate", G_CALLBACK(action_open_recent), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(ra));
    g_object_unref(ra);
  }
  add_action(s->app, "new", G_CALLBACK(action_new), s);
  add_action(s->app, "save", G_CALLBACK(action_save), s);
  add_action(s->app, "save_as", G_CALLBACK(action_save_as), s);
  add_action(s->app, "help", G_CALLBACK(action_help), s);
  add_action(s->app, "instref", G_CALLBACK(action_instruction_ref), s);
  add_action(s->app, "dref", G_CALLBACK(action_directive_ref), s);
  add_action(s->app, "about", G_CALLBACK(action_about), s);
  add_action(s->app, "config_files", G_CALLBACK(action_config_files), s);
  add_action(s->app, "check",    G_CALLBACK(action_check),       s);
  add_action(s->app, "assemble", G_CALLBACK(action_do_assemble), s);
  add_action(s->app, "format_upper", G_CALLBACK(action_format_upper), s);
  add_action(s->app, "format_lower", G_CALLBACK(action_format_lower), s);
  add_action(s->app, "find", G_CALLBACK(action_find), s);
  add_action(s->app, "replace", G_CALLBACK(action_replace), s);
  add_action(s->app, "jump_def", G_CALLBACK(action_jump_def), s);
  add_action(s->app, "jump_back", G_CALLBACK(action_jump_back), s);
  add_action(s->app, "jump_fwd", G_CALLBACK(action_jump_fwd), s);
  add_action(s->app, "find_refs", G_CALLBACK(action_find_refs), s);
  add_action(s->app, "bookmark_toggle", G_CALLBACK(action_bookmark_toggle), s);
  add_action(s->app, "bookmark_next", G_CALLBACK(action_bookmark_next), s);
  add_action(s->app, "bookmark_prev", G_CALLBACK(action_bookmark_prev), s);
  add_action(s->app, "bookmark_clear", G_CALLBACK(action_bookmark_clear), s);
  add_action(s->app, "goto", G_CALLBACK(action_goto), s);
  add_action(s->app, "dedupe_lines", G_CALLBACK(action_dedupe_lines), s);
  add_action(s->app, "format_layout", G_CALLBACK(action_format_layout), s);
  add_action(s->app, "unicode_replace_all",
             G_CALLBACK(action_unicode_replace_all), s);
  add_action(s->app, "unicode_replace_review",
             G_CALLBACK(action_unicode_replace_review), s);
  add_action(s->app, "dup_line", G_CALLBACK(action_dup_line), s);
  add_action(s->app, "save_prefs", G_CALLBACK(action_save_prefs), s);
  add_action(s->app, "show_in_files", G_CALLBACK(action_show_in_files), s);
  add_action(s->app, "font", G_CALLBACK(action_font), s);
  {
    GSimpleAction *rl = g_simple_action_new_stateful(
        "reopen_last_file", NULL, g_variant_new_boolean(s->reopen_last_file));
    g_signal_connect(rl, "activate", G_CALLBACK(action_toggle_reopen_last_file),
                     s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(rl));
    g_object_unref(rl);
  }
  add_action(s->app, "copy",   G_CALLBACK(action_copy),   s);
  add_action(s->app, "cut",    G_CALLBACK(action_cut),    s);
  add_action(s->app, "paste",  G_CALLBACK(action_paste),  s);
  add_action(s->app, "copy_without_comments",
             G_CALLBACK(action_copy_without_comments), s);
  {
    GSimpleAction *ww = g_simple_action_new_stateful(
        "word_wrap", NULL, g_variant_new_boolean(s->word_wrap));
    g_signal_connect(ww, "activate", G_CALLBACK(action_toggle_word_wrap), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(ww));
    g_object_unref(ww);
  }
  add_action(s->app, "zoom_in", G_CALLBACK(action_zoom_in), s);
  add_action(s->app, "zoom_out", G_CALLBACK(action_zoom_out), s);
  add_action(s->app, "zoom_reset", G_CALLBACK(action_zoom_reset), s);
  add_action(s->app, "set_src_ext", G_CALLBACK(action_set_src_ext), s);
  add_action(s->app, "set_bin_ext", G_CALLBACK(action_set_bin_ext), s);
  add_action(s->app, "tab_width", G_CALLBACK(action_tab_width), s);
  add_action(s->app, "include_tree", G_CALLBACK(action_include_tree), s);
  add_action(s->app, "comment_align_col", G_CALLBACK(action_comment_align_col),
             s);
  add_action(s->app, "quit", G_CALLBACK(action_quit), s);
#ifdef ED_BUILD_SIM
  add_action(s->app, "simulator", G_CALLBACK(action_simulator), s);
#endif

  // Stateful dark-mode toggle — initial state matches loaded dark_mode pref
  GSimpleAction *dark = g_simple_action_new_stateful(
      "dark", NULL, g_variant_new_boolean(s->dark_mode));
  g_signal_connect(dark, "activate", G_CALLBACK(action_toggle_dark), s);
  g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(dark));
  g_object_unref(dark);

  // Stateful Problems panel toggle (default TRUE = enabled)
  GSimpleAction *problems = g_simple_action_new_stateful(
      "problems", NULL, g_variant_new_boolean(TRUE));
  g_signal_connect(problems, "activate", G_CALLBACK(action_toggle_problems), s);
  g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(problems));
  g_object_unref(problems);

  // Stateful semantic-error rendering toggle
  GSimpleAction *err_ul = g_simple_action_new_stateful(
      "error_underline", NULL, g_variant_new_boolean(s->error_underline));
  g_signal_connect(err_ul, "activate",
                   G_CALLBACK(action_toggle_error_underline), s);
  g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(err_ul));
  g_object_unref(err_ul);

  // Stateful Problems panel visibility (default TRUE = visible when enabled)
  GSimpleAction *problems_vis = g_simple_action_new_stateful(
      "problems_visible", NULL, g_variant_new_boolean(TRUE));
  g_signal_connect(problems_vis, "activate",
                   G_CALLBACK(action_toggle_problems_visible), s);
  g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(problems_vis));
  g_object_unref(problems_vis);

  /* Memory viewer/editor */
  {
    GSimpleAction *a = g_simple_action_new("memory_viewer", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(action_memory_viewer), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
  }

  // Assemble option: clear memory before assembling (default TRUE)
  s->asm_clear_mem = TRUE;
  {
    GSimpleAction *a = g_simple_action_new_stateful(
        "asm_clear_mem", NULL, g_variant_new_boolean(s->asm_clear_mem));
    g_signal_connect(a, "activate", G_CALLBACK(action_toggle_asm_clear_mem), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
  }

  /* Assemble option: allow undocumented opcodes (default FALSE) */
  s->allow_undoc = FALSE;
  {
    GSimpleAction *a = g_simple_action_new_stateful(
        "allow_undoc", NULL, g_variant_new_boolean(FALSE));
    g_signal_connect(a, "activate", G_CALLBACK(action_toggle_allow_undoc), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
  }
  /* Autocomplete (default TRUE) */
  s->autocomplete = TRUE;
  {
    GSimpleAction *a = g_simple_action_new_stateful(
        "autocomplete", NULL, g_variant_new_boolean(TRUE));
    g_signal_connect(a, "activate", G_CALLBACK(action_toggle_autocomplete), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
  }
  /* Folding options — 4 non-stateful actions, no checkboxes */
  {
    GSimpleAction *a;
    a = g_simple_action_new("fold_comments", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(action_fold_comments), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
    a = g_simple_action_new("unfold_comments", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(action_unfold_comments), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
    a = g_simple_action_new("fold_macros", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(action_fold_macros), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
    a = g_simple_action_new("unfold_macros", NULL);
    g_signal_connect(a, "activate", G_CALLBACK(action_unfold_macros), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
  }

  /* Editor preference: auto-indent (default TRUE) */
  if (s)
    s->auto_indent = TRUE;
  {
    GSimpleAction *a = g_simple_action_new_stateful(
        "auto_indent", NULL, g_variant_new_boolean(s ? s->auto_indent : TRUE));
    g_signal_connect(a, "activate", G_CALLBACK(action_toggle_auto_indent), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
    g_object_unref(a);
  }

  /* Comment preference: keep comment column aligned (default FALSE) */
  if (s) {
    s->keep_comment_aligned = FALSE;
    if (s->comment_align_col <= 0)
      s->comment_align_col = 32; /* default column */
  }
  {
    GSimpleAction *a = g_simple_action_new_stateful(
        "keep_comment_aligned", NULL,
        g_variant_new_boolean(s ? s->keep_comment_aligned : FALSE));
    g_signal_connect(a, "activate",
                     G_CALLBACK(action_toggle_keep_comment_aligned), s);
    g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));

    /* Comment preference: comment marker (default ';') */
    if (s)
      s->comment_use_slashslash = FALSE;
    {
      GSimpleAction *a = g_simple_action_new_stateful(
          "comment_marker", G_VARIANT_TYPE_STRING,
          g_variant_new_string(s && s->comment_use_slashslash ? "slashslash"
                                                              : "semicolon"));
      g_signal_connect(a, "activate", G_CALLBACK(action_comment_marker), s);
      g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
      g_object_unref(a);
    }
    {
      GSimpleAction *a = g_simple_action_new("comment_marker_toggle", NULL);
      g_signal_connect(a, "activate", G_CALLBACK(action_toggle_comment_marker),
                       s);
      g_action_map_add_action(G_ACTION_MAP(s->app), G_ACTION(a));
      g_object_unref(a);
    }

    g_object_unref(a);
  }

  GtkShortcutController *sc =
      GTK_SHORTCUT_CONTROLLER(gtk_shortcut_controller_new());
  gtk_shortcut_controller_set_scope(sc, GTK_SHORTCUT_SCOPE_GLOBAL);

  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_o, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.open")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_n, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.new")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_s, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.save")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(
                               GDK_KEY_s, GDK_CONTROL_MASK | GDK_SHIFT_MASK),
                           gtk_named_action_new("app.save_as")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_q, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.quit")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_f, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.find")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_h, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.replace")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_d, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.dup_line")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(
                               GDK_KEY_i, GDK_CONTROL_MASK | GDK_SHIFT_MASK),
                           gtk_named_action_new("app.auto_indent")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_l, GDK_CONTROL_MASK |
                                                                 GDK_ALT_MASK),
                           gtk_named_action_new("app.keep_comment_aligned")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_m, GDK_CONTROL_MASK |
                                                                 GDK_ALT_MASK),
                           gtk_named_action_new("app.comment_marker_toggle")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_g, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.goto")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(
                               GDK_KEY_l, GDK_CONTROL_MASK | GDK_SHIFT_MASK),
                           gtk_named_action_new("app.dark")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F8, 0),
                           gtk_named_action_new("app.problems_visible")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F6, 0),
                           gtk_named_action_new("app.instref")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F7, 0),
                           gtk_named_action_new("app.dref")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F4, 0),
                           gtk_named_action_new("app.bookmark_next")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F4, GDK_SHIFT_MASK),
                           gtk_named_action_new("app.bookmark_prev")));
  gtk_shortcut_controller_add_shortcut(
      sc, gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_b, GDK_CONTROL_MASK),
                           gtk_named_action_new("app.bookmark_toggle")));

  gtk_widget_add_controller(GTK_WIDGET(s->win), GTK_EVENT_CONTROLLER(sc));

  /* Application accelerators (global). */
  /* File */
  gtk_application_set_accels_for_action(s->app, "app.new",
                                        (const char *[]){"<Primary>n", NULL});
  gtk_application_set_accels_for_action(s->app, "app.open",
                                        (const char *[]){"<Primary>o", NULL});
  gtk_application_set_accels_for_action(s->app, "app.save",
                                        (const char *[]){"<Primary>s", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.save_as", (const char *[]){"<Primary><Shift>s", NULL});
  gtk_application_set_accels_for_action(s->app, "app.undo",
                                        (const char *[]){"<Primary>z", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.redo",
      (const char *[]){"<Primary><Shift>z", "<Primary>y", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.zoom_in",
      (const char *[]){"<Primary>equal", "<Primary>plus", "<Primary>KP_Add",
                       NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.zoom_out",
      (const char *[]){"<Primary>minus", "<Primary>KP_Subtract", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.zoom_reset",
      (const char *[]){"<Primary>0", "<Primary>KP_0", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.save_as", (const char *[]){"<Primary><Shift>s", NULL});
  gtk_application_set_accels_for_action(s->app, "app.check",
                                        (const char *[]){"F5", NULL});
  gtk_application_set_accels_for_action(s->app, "app.assemble",
                                        (const char *[]){"F9", NULL});
  gtk_application_set_accels_for_action(s->app, "app.quit",
                                        (const char *[]){"<Primary>q", NULL});
#ifdef ED_BUILD_SIM
  gtk_application_set_accels_for_action(s->app, "app.simulator",
                                        (const char *[]){"<Primary>e", NULL});
#endif

  /* Edit */
  gtk_application_set_accels_for_action(s->app, "app.help",
                                        (const char *[]){"F1", NULL});
  gtk_application_set_accels_for_action(s->app, "app.find",
                                        (const char *[]){"<Primary>f", NULL});
  gtk_application_set_accels_for_action(s->app, "app.replace",
                                        (const char *[]){"<Primary>h", NULL});
  gtk_application_set_accels_for_action(s->app, "app.goto",
                                        (const char *[]){"<Primary>g", NULL});

  /* View */
  gtk_application_set_accels_for_action(s->app, "app.problems_visible",
                                        (const char *[]){"F8", NULL});
  gtk_application_set_accels_for_action(s->app, "app.instref",
                                        (const char *[]){"F6", NULL});
  gtk_application_set_accels_for_action(s->app, "app.dref",
                                        (const char *[]){"F7", NULL});
  gtk_application_set_accels_for_action(s->app, "app.bookmark_toggle",
                                        (const char *[]){"<Primary>b", NULL});
  gtk_application_set_accels_for_action(s->app, "app.bookmark_next",
                                        (const char *[]){"F4", NULL});
  gtk_application_set_accels_for_action(s->app, "app.bookmark_prev",
                                        (const char *[]){"<Shift>F4", NULL});
  gtk_application_set_accels_for_action(s->app, "app.include_tree",
                                        (const char *[]){"<Primary>i", NULL});

  /* Preferences */
  gtk_application_set_accels_for_action(
      s->app, "app.dark", (const char *[]){"<Primary><Shift>l", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.error_underline",
      (const char *[]){"<Primary><Shift>u", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.auto_indent", (const char *[]){"<Primary><Shift>i", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.keep_comment_aligned",
      (const char *[]){"<Primary><Shift>k", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.comment_marker_toggle",
      (const char *[]){"<Primary>slash", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.tab_width", (const char *[]){"<Primary><Alt>t", NULL});
  gtk_application_set_accels_for_action(
      s->app, "app.comment_align_col",
      (const char *[]){"<Primary><Alt>c", NULL});

  /* Tools */
  gtk_application_set_accels_for_action(s->app, "app.memory_viewer",
                                        (const char *[]){"<Ctrl>M", NULL});
  gtk_application_set_accels_for_action(s->app, "app.jump_back",
                                        (const char *[]){"<Shift>F12", NULL});
  gtk_application_set_accels_for_action(s->app, "app.jump_fwd",
                                        (const char *[]){"F12", NULL});
  gtk_application_set_accels_for_action(s->app, "app.find_refs",
                                        (const char *[]){"<Shift>F11", NULL});
  gtk_application_set_accels_for_action(s->app, "app.jump_def",
                                        (const char *[]){"<Ctrl>F12", NULL});
}

/* ---------------- Tags ---------------- */

static void apply_current_line_tag(AppState *s) {
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  GtkTextIter it;
  gtk_text_buffer_get_iter_at_mark(tb, &it, gtk_text_buffer_get_insert(tb));
  int line = gtk_text_iter_get_line(&it);

  if (line == s->curline_applied)
    return;

  // remove from old line
  if (s->curline_applied >= 0) {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line(tb, &a, s->curline_applied);
    b = a;
    gtk_text_iter_forward_to_line_end(&b);
    gtk_text_buffer_remove_tag(tb, s->tag_curline, &a, &b);
  }

  // apply to new line
  {
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line(tb, &a, line);
    b = a;
    gtk_text_iter_forward_to_line_end(&b);
    gtk_text_buffer_apply_tag(tb, s->tag_curline, &a, &b);
  }

  s->curline_applied = line;
}

static void create_tags(AppState *s) {
  GtkTextBuffer *tb = GTK_TEXT_BUFFER(s->buf);

  s->tag_instr = gtk_text_buffer_create_tag(tb, "instr", "foreground",
                                            s->colors.instr, NULL);
  s->tag_dir =
      gtk_text_buffer_create_tag(tb, "dir", "foreground", s->colors.dir, NULL);
  s->tag_data = gtk_text_buffer_create_tag(tb, "data", "foreground",
                                           s->colors.data, NULL);
  s->tag_num =
      gtk_text_buffer_create_tag(tb, "num", "foreground", s->colors.num, NULL);
  s->tag_label =
      gtk_text_buffer_create_tag(tb, "label", "foreground", s->colors.label,
                                 "weight", PANGO_WEIGHT_BOLD, NULL);
  /* Address operand sub-colour tags */
  s->tag_addr_hex = gtk_text_buffer_create_tag(tb, "addr_hex", "foreground",
                                               s->colors.addr_hex, NULL);
  s->tag_addr_dec = gtk_text_buffer_create_tag(tb, "addr_dec", "foreground",
                                               s->colors.addr_dec, NULL);
  s->tag_addr_bin = gtk_text_buffer_create_tag(tb, "addr_bin", "foreground",
                                               s->colors.addr_bin, NULL);
  /* Immediate operand sub-colour tags */
  s->tag_imm_hex = gtk_text_buffer_create_tag(tb, "imm_hex", "foreground",
                                              s->colors.imm_hex, NULL);
  s->tag_imm_dec = gtk_text_buffer_create_tag(tb, "imm_dec", "foreground",
                                              s->colors.imm_dec, NULL);
  s->tag_imm_bin = gtk_text_buffer_create_tag(tb, "imm_bin", "foreground",
                                              s->colors.imm_bin, NULL);
  s->tag_imm_char = gtk_text_buffer_create_tag(tb, "imm_char", "foreground",
                                               s->colors.imm_char, NULL);
  /* Data value sub-colour tags */
  s->tag_dv_hex = gtk_text_buffer_create_tag(tb, "dv_hex", "foreground",
                                             s->colors.dv_hex, NULL);
  s->tag_dv_dec = gtk_text_buffer_create_tag(tb, "dv_dec", "foreground",
                                             s->colors.dv_dec, NULL);
  s->tag_dv_bin = gtk_text_buffer_create_tag(tb, "dv_bin", "foreground",
                                             s->colors.dv_bin, NULL);
  s->tag_dv_str = gtk_text_buffer_create_tag(tb, "dv_str", "foreground",
                                             s->colors.dv_str, NULL);

  // Hover highlight for label definitions (background)
  s->tag_hover_def = gtk_text_buffer_create_tag(tb, "hover_def", "background",
                                                s->colors.hover_bg, NULL);
  s->tag_reg =
      gtk_text_buffer_create_tag(tb, "reg", "foreground", s->colors.reg, NULL);
  s->tag_comment = gtk_text_buffer_create_tag(tb, "comment", "foreground",
                                              s->colors.comment, NULL);
  s->tag_string = gtk_text_buffer_create_tag(tb, "string", "foreground",
                                             s->colors.str, NULL);
  GdkRGBA u;
  gdk_rgba_parse(&u, s->colors.err_underline);
  s->tag_error = gtk_text_buffer_create_tag(tb, "err", NULL);

  /* Configure semantic error tag style based on preference */
  if (s->error_underline) {
    g_object_set(s->tag_error, "foreground-set", FALSE, "weight-set", FALSE,
                 "underline", PANGO_UNDERLINE_SINGLE, "underline-set", TRUE,
                 "underline-rgba", &u, "underline-rgba-set", TRUE, NULL);
  } else {
    g_object_set(s->tag_error, "underline-set", FALSE, "underline-rgba-set",
                 FALSE, "foreground-rgba", &u, "foreground-set", TRUE, "weight",
                 PANGO_WEIGHT_BOLD, "weight-set", TRUE, NULL);
  }
  s->tag_inactive = gtk_text_buffer_create_tag(tb, "inactive", "foreground",
                                               s->colors.inactive, NULL);
  s->tag_curline = gtk_text_buffer_create_tag(tb, "curline", NULL);
  /* Fold tags */
  s->tag_fold_hidden = gtk_text_buffer_create_tag(
      tb, "fold_hidden", "invisible", TRUE, "size",
      1, /* collapse line height so gutter hides too */
      "size-set", TRUE, NULL);
  /* fold_marker: used on the start line when collapsed (greyed italic) */
  s->tag_fold_marker = gtk_text_buffer_create_tag(
      tb, "fold_marker", "foreground", "#6699cc", "paragraph-background",
      "#1e2a3a", "style", PANGO_STYLE_ITALIC, NULL);

  /* Ensure tag priorities are within the tag-table range.
     Higher priority wins when tags overlap. */
  GtkTextTagTable *tt = gtk_text_buffer_get_tag_table(tb);
  int n = gtk_text_tag_table_get_size(tt);
  int pr_error = (n > 0) ? (n - 1) : 0;
  int pr_inactive = (n > 1) ? (n - 2) : 0;
  int pr_comment = (n > 2) ? (n - 3) : 0;
  int pr_curline = (n > 3) ? (n - 4) : 0;

  gtk_text_tag_set_priority(s->tag_error, pr_error);
  gtk_text_tag_set_priority(s->tag_inactive, pr_inactive);
  gtk_text_tag_set_priority(s->tag_comment, pr_comment);
  gtk_text_tag_set_priority(s->tag_curline, pr_curline);
  /* fold_hidden MUST be highest priority of all tags so invisible
   * overrides every syntax/comment/error tag on folded lines. */
  n = gtk_text_tag_table_get_size(tt);
  gtk_text_tag_set_priority(s->tag_fold_hidden, n - 1);
  gtk_text_tag_set_priority(s->tag_fold_marker, n - 2);
  s->curline_applied = -1;
}

/* ---------------- App init ---------------- */

static void on_activate(GtkApplication *app, gpointer user_data) {
  AppState *s = (AppState *)user_data;
  s->app = app;

  gtk_source_init();

  // Load colour overrides (if present) before any buffers/tags are created.
  colors_set_defaults(s);
  colors_load_from_exe_dir(s);

  // Load persisted preferences (project / exe / user config dir — first wins).
  gtk_prefs_defaults(s);
  gtk_prefs_load(s);
  gtk_prefs_defaults(s); /* fill any still-missing after load */
  apply_font_css(s);     /* apply loaded/default font */

  /* Reload colours now that dark_mode is known from prefs, so the correct
   * [dark] or [light] section is applied. */
  colors_set_defaults(s);
  colors_load_from_exe_dir(s);

  s->win = GTK_WINDOW(gtk_application_window_new(app));
  gtk_window_set_default_size(s->win, s->win_width > 0 ? s->win_width : 1100,
                              s->win_height > 0 ? s->win_height : 750);

  // Stop periodic callbacks before widgets start being destroyed.
  g_signal_connect(s->win, "close-request", G_CALLBACK(main_close_request_cb),
                   s);

  s->hdr = GTK_HEADER_BAR(gtk_header_bar_new());
  s->title = GTK_LABEL(gtk_label_new("6502caEd - (untitled)"));
  gtk_header_bar_set_title_widget(s->hdr, GTK_WIDGET(s->title));
  gtk_window_set_titlebar(s->win, GTK_WIDGET(s->hdr));

  /* Tabbed editor */
  s->notebook = GTK_NOTEBOOK(gtk_notebook_new());
  gtk_notebook_set_scrollable(s->notebook, TRUE);

  /* Stash AppState on the notebook so tab callbacks (close/+) can find it. */
  g_object_set_data(G_OBJECT(s->notebook), "app-state", s);

  /* Far-right '+' action button (stays at the end even if tabs are reordered).
   */
  GtkWidget *plus = gtk_button_new_with_label("+");
  gtk_widget_add_css_class(plus, "flat");
  gtk_widget_set_tooltip_text(plus, "New Tab");
  g_signal_connect(plus, "clicked", G_CALLBACK(tab_plus_clicked), s);
  gtk_notebook_set_action_widget(s->notebook, plus, GTK_PACK_END);

  g_signal_connect(s->notebook, "page-added",
                   G_CALLBACK(on_notebook_pages_changed), s);
  g_signal_connect(s->notebook, "page-removed",
                   G_CALLBACK(on_notebook_pages_changed), s);
  g_signal_connect(s->notebook, "page-reordered",
                   G_CALLBACK(on_notebook_pages_changed), s);

  s->docs = g_ptr_array_new_with_free_func((GDestroyNotify)app_doc_destroy);
  /* First doc */
  EditorDoc *d0 = app_doc_new(s, NULL);
  g_ptr_array_add(s->docs, d0);
  app_add_doc_tab(s, d0, "(untitled)");

  /* Activate first doc so existing code paths use s->buf/s->view etc */
  s->active_doc = NULL;
  app_activate_doc(s, d0);

  g_signal_connect(s->notebook, "switch-page",
                   G_CALLBACK(on_notebook_switch_page), s);
  g_signal_connect(s->notebook, "page-added",
                   G_CALLBACK(on_notebook_pages_changed), s);
  g_signal_connect(s->notebook, "page-removed",
                   G_CALLBACK(on_notebook_pages_changed), s);
  g_signal_connect(s->notebook, "page-reordered",
                   G_CALLBACK(on_notebook_pages_changed), s);

  gtk_source_view_set_show_line_numbers(s->view,
                                        FALSE); /* FoldLineNums handles this */
  gtk_source_view_set_highlight_current_line(s->view, FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(s->view), TRUE);
  // Ensure an insertion cursor is visible (some CSS/themes can make the
  // default caret effectively invisible).
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(s->view), TRUE);

  /* Apply editor prefs loaded from file. */
  apply_editor_prefs(s);

  GtkWidget *scroller = GTK_WIDGET(s->notebook);
  gtk_widget_set_vexpand(scroller, TRUE);
  gtk_widget_set_hexpand(scroller, TRUE);

  s->status = gtk_label_new("Ready.");
  s->status_msg = g_strdup("Ready.");
  s->status_last = NULL;
  gtk_widget_set_halign(s->status, GTK_ALIGN_START);
  // Poll occasionally so lock keys / overwrite mode stay up-to-date.
  s->status_poll_id = g_timeout_add(200, status_poll_cb, s);
  update_status_bar(s);
  gtk_widget_set_margin_start(s->status, 8);
  gtk_widget_set_margin_end(s->status, 8);
  gtk_widget_set_margin_top(s->status, 6);
  gtk_widget_set_margin_bottom(s->status, 6);

  GMenuModel *model = build_menubar_model(s);
  GtkWidget *menubar = gtk_popover_menu_bar_new_from_model(model);
  g_object_unref(model);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(vbox), menubar);

  // In-window Find/Replace bar (hidden until invoked)
  s->find_match_case = FALSE;
  s->findbar = create_findbar_widget(s);
  /* start hidden; shown via Edit->Find/Replace */
  gtk_widget_set_visible(s->findbar, FALSE);
  gtk_box_append(GTK_BOX(vbox), s->findbar);

  /* Editor + Problems pane (splitter) */
  GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  s->problems_paned = paned;
  /* Make the splitter handle more obvious and easier to grab. */
  gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
  gtk_widget_set_vexpand(paned, TRUE);
  gtk_widget_set_hexpand(paned, TRUE);
  gtk_paned_set_start_child(GTK_PANED(paned), scroller);
  gtk_paned_set_resize_start_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);

  /* Problems panel defaults */
  s->problems_filter = 0; /* All */

  GtkWidget *problems = create_problems_panel(s);
  gtk_paned_set_end_child(GTK_PANED(paned), problems);
  /* End child must be resizable so the Problems list grows/shrinks with the
   * splitter. */
  gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
  gtk_paned_set_shrink_end_child(GTK_PANED(paned), TRUE);

  /* Keep a compact default height (~3 rows), but allow user resizing.
     Use notify::position because "size-allocate" signal isn't valid here in
     GTK4. */
  g_signal_connect(paned, "notify::position",
                   G_CALLBACK(problems_paned_position_notify), s);

  /* Default end-pane size in pixels (roughly 3 rows). */
  s->problems_min_end_px =
      120; // minimum height that you can reduce the problems box in pixels
  s->problems_default_end_px =
      180; // starting height of the problems box in pixels
  s->problems_last_end_px = 0;
  s->problems_want_end_px = s->problems_default_end_px;
  s->problems_collapsed_pos = -1;
  s->problems_collapsed = FALSE;

  gtk_box_append(GTK_BOX(vbox), paned);
  // Set initial position after first layout; keep id so we can cancel on close.
  s->problems_init_idle_id = g_idle_add(problems_paned_init_position_cb, s);
  /* Set initial splitter position (approx based on default window height). */
  /* Initial splitter position is set after first layout pass. */
  gtk_box_append(GTK_BOX(vbox), s->status);

  GtkWidget *overlay = gtk_overlay_new();
  gtk_overlay_set_child(GTK_OVERLAY(overlay), vbox);

  GtkWidget *toast_rev = gtk_revealer_new();
  s->copy_toast = toast_rev;
  gtk_revealer_set_transition_type(GTK_REVEALER(toast_rev),
                                   GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
  gtk_revealer_set_transition_duration(GTK_REVEALER(toast_rev), 500);
  gtk_revealer_set_reveal_child(GTK_REVEALER(toast_rev), FALSE);
  gtk_widget_set_halign(toast_rev, GTK_ALIGN_END);
  gtk_widget_set_valign(toast_rev, GTK_ALIGN_END);
  gtk_widget_set_margin_end(toast_rev, 20);
  gtk_widget_set_margin_bottom(toast_rev, 20);

  GtkWidget *toast_frame = gtk_frame_new(NULL);
  gtk_widget_add_css_class(toast_frame, "card");
  gtk_widget_set_margin_start(toast_frame, 8);
  gtk_widget_set_margin_end(toast_frame, 8);
  gtk_widget_set_margin_top(toast_frame, 4);
  gtk_widget_set_margin_bottom(toast_frame, 4);

  GtkWidget *toast_lbl = gtk_label_new("Copied to clipboard");
  s->copy_toast_label = toast_lbl;
  gtk_widget_set_margin_start(toast_lbl, 12);
  gtk_widget_set_margin_end(toast_lbl, 12);
  gtk_widget_set_margin_top(toast_lbl, 8);
  gtk_widget_set_margin_bottom(toast_lbl, 8);
  gtk_frame_set_child(GTK_FRAME(toast_frame), toast_lbl);
  gtk_revealer_set_child(GTK_REVEALER(toast_rev), toast_frame);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), toast_rev);

  gtk_window_set_child(s->win, overlay);

  // Focus the editor by default so you can type immediately and the caret is
  // shown.
  gtk_widget_grab_focus(GTK_WIDGET(s->view));

  g_signal_connect(s->buf, "notify::modified", G_CALLBACK(on_modified_changed),
                   s);
  g_signal_connect(s->buf, "changed", G_CALLBACK(on_buffer_changed), s);
  g_signal_connect(s->buf, "insert-text", G_CALLBACK(on_buf_insert_text), s);
  g_signal_connect(s->buf, "delete-range", G_CALLBACK(on_buf_delete_range), s);

  g_signal_connect(s->buf, "notify::cursor-position",
                   G_CALLBACK(on_cursor_moved), s);

  /* dark_mode was loaded from prefs above (defaults to TRUE if key absent). */
  s->css = NULL;

  install_actions_and_shortcuts(s);
  apply_theme(s); // apply CSS + prefer-dark-theme
  set_title(s);
  schedule_highlight(s);

  gtk_window_present(s->win);
  gtk_widget_grab_focus(GTK_WIDGET(s->view));

  /* Reopen files from last session — after window is fully presented
     so all signals and widgets are initialised. */
  session_load(s);
  recent_files_load(s);
  recent_menu_rebuild(s);
}

int main(int argc, char **argv) {
  AppState st = (AppState){0};

  st.problems_enabled = TRUE;
  st.problems_user_hidden = FALSE;

  diags_init(&st.sem_diags);
  diags_init(&st.asm_diags);
  st.mem_image = g_malloc0(65536);

  GtkApplication *app = gtk_application_new(NULL, G_APPLICATION_DEFAULT_FLAGS);
  if (!app) {
    g_printerr("Failed to create GtkApplication.\n");
    return 1;
  }

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), &st);
  int rc = g_application_run(G_APPLICATION(app), argc, argv);

  g_clear_object(&st.last_source_folder);
  if (st.path)
    g_free(st.path);
  if (st.hl_idle_id)
    g_source_remove(st.hl_idle_id);
  if (st.problems_refresh_idle_id)
    g_source_remove(st.problems_refresh_idle_id);
  if (st.css)
    g_object_unref(st.css);
  fold_map_free(&st.fold_map);
  diags_free(&st.sem_diags);
  diags_free(&st.asm_diags);
  if (st.mem_image)
    g_free(st.mem_image);
  g_object_unref(app);

  gtk_source_finalize();
  return rc;
}
