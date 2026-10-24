#pragma once
/* curses_compat.h — selects the right curses header and smooths over
 * API differences between ncurses (Linux/macOS) and PDCurses (Windows).
 */

#ifdef _WIN32
/* PDCurses in MSYS2 UCRT64 installs its header at <pdcurses/curses.h>.
 * The top-level <pdcurses.h> is just a forwarding header. */
#  include <pdcurses/curses.h>

/* PDCurses uses resize_term(); ncurses uses resizeterm(). */
#  ifndef resizeterm
#    define resizeterm(r,c)  resize_term((r),(c))
#  endif

/* PDCurses does not define is_term_resized(). */
#  ifndef is_term_resized
#    define is_term_resized(r,c)  ((r) != LINES || (c) != COLS)
#  endif

/* PDCurses WinCon returns numpad/virtual keypad codes for cursor keys.
 * Map them to the standard KEY_* names our code expects.
 * KEY_A2=up, KEY_C2=down, KEY_B1=left, KEY_B3=right on the virtual keypad.
 * KEY_A1=home, KEY_C3=end, KEY_A3=pgup, KEY_C1=pgdn */
#  ifdef KEY_A2
#    undef KEY_UP
#    undef KEY_DOWN
#    undef KEY_LEFT
#    undef KEY_RIGHT
#    define KEY_UP     KEY_A2   /* 0xec00 + 0xc2 */
#    define KEY_DOWN   KEY_C2   /* 0xec00 + 0xc8 */
#    define KEY_LEFT   KEY_B1   /* 0xec00 + 0xc4 */
#    define KEY_RIGHT  KEY_B3   /* 0xec00 + 0xc6 */
#  endif
#  ifdef KEY_A1
#    undef KEY_HOME
#    undef KEY_END
#    undef KEY_PPAGE
#    undef KEY_NPAGE
#    define KEY_HOME   KEY_A1   /* 0xec00 + 0xc1 */
#    define KEY_END    KEY_C3   /* 0xec00 + 0xc9 */
#    define KEY_PPAGE  KEY_A3   /* 0xec00 + 0xc3 */
#    define KEY_NPAGE  KEY_C1   /* 0xec00 + 0xc7 */
#  endif

/* Also map Ctrl+arrow keys used for word movement */
#  ifdef CTL_LEFT
#    define KEY_CTRL_LEFT_WIN   CTL_LEFT
#    define KEY_CTRL_RIGHT_WIN  CTL_RIGHT
#    define KEY_CTRL_UP_WIN     CTL_UP
#    define KEY_CTRL_DOWN_WIN   CTL_DN
#  endif

/* PDCurses 4.x provides nc_getmouse(MEVENT*) as a ncurses-compatible
 * wrapper. Map the ncurses getmouse() name to it. */
#  define getmouse(ev)  nc_getmouse(ev)

/* PDCurses does not support -1 as "terminal default" colour.
 * Helper macro to clamp colour values for PDCurses. */
#  define PDC_COL_FG(c)  ((short)(c) < 0 ? COLOR_WHITE : (short)(c))
#  define PDC_COL_BG(c)  ((short)(c) < 0 ? COLOR_BLACK : (short)(c))
#  define pdc_init_pair(p,f,b)  init_pair((p), PDC_COL_FG(f), PDC_COL_BG(b))

#else
/* Standard ncurses on Linux/macOS */
#  include <ncurses.h>
#endif
