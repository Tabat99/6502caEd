#pragma once
#include <stdint.h>
#include "curses_compat.h"

// Simple, configurable colour theme for ncurses rendering.
// This is intentionally small and deterministic.

typedef enum {
  TK_DEFAULT = 0,
  TK_MNEMONIC,
  TK_REGISTER,
  TK_DIRECTIVE,
  TK_DATA,
  TK_NUMBER,
  TK_STRING,
  TK_COMMENT,
  TK_LABEL,
  TK_OPERATOR,
  TK_DIAG_ERROR,
  TK_DIAG_WARNING,
  /* Address operand sub-kinds */
  TK_ADDR_HEX,
  TK_ADDR_DEC,
  TK_ADDR_BIN,
  /* Immediate operand sub-kinds */
  TK_IMM_HEX,
  TK_IMM_DEC,
  TK_IMM_BIN,
  TK_IMM_CHAR,
  /* Data-directive value sub-kinds */
  TK_DV_HEX,
  TK_DV_DEC,
  TK_DV_BIN,
  TK_DV_STR,
  /* File selector */
  TK_FS_TITLE,       /* dialog title                  */
  TK_FS_PATH,        /* current directory path bar    */
  TK_FS_TAB_ACTIVE,  /* active extension filter tab   */
  TK_FS_TAB,         /* inactive extension filter tab */
  TK_FS_DIR,         /* directory entry               */
  TK_FS_FILE,        /* file entry (unselected)       */
  TK_FS_SELECTED,    /* selected list item            */
  TK_FS_INPUT,       /* filename input field          */
  TK_FS_HELP,        /* help/hint line                */
  TK__COUNT
} token_kind_t;

typedef struct {
  // Foreground colours for each token kind.
  // Values are ncurses colour indexes (0..7), or -1 for terminal default.
  int fg[TK__COUNT];
  // Background colours for each token kind (0..7), or -1 for default.
  int bg[TK__COUNT];
  // ncurses colour pair id for each token kind (computed at init).
  short pair[TK__COUNT];
  // Precomputed COLOR_PAIR(pair[k]) stored as uint64_t to handle PDCurses
  // 64-bit chtype (PDC_FORCE_UTF8 shifts colour bits by 44, overflowing int).
  uint64_t color_attr[TK__COUNT];

  // Extra attributes.
  unsigned char bold[TK__COUNT];
} theme_t;

// Load theme from a simple key=value file. If no file is found, defaults apply.
// Search order:
//   1) ./6502caEd.theme
//   2) ~/.config/6502caEd/theme
// Returns 1 on success (or defaults), 0 on fatal error.
int theme_load_and_init(theme_t* t);

// Apply the theme for token kind; returns the attribute to use in addch.
// Returns chtype to handle 64-bit colour attributes on PDCurses.
chtype theme_attr(const theme_t* t, token_kind_t k, int highlight_line);
