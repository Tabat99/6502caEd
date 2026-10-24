#pragma once
/* lcd_gtk.h — GTK4 rendering for HD44780 LCD emulation
 *
 * Creates an embedded panel (GtkDrawingArea) showing the LCD character grid.
 * A pop-out button opens a separate larger window.
 * Hooks into VIA write callbacks to detect E-strobe falling edges.
 */
#include <gtk/gtk.h>
#include "lcd.h"
#include "lcd_cfg.h"
#include "via.h"

/* Forward declaration */
struct sim_win_t;

/* -------------------------------------------------------------------------
 * LCD UI state — embedded in sim_win_t
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Core LCD state */
    lcd_t        lcd;
    lcd_cfg_t    cfg;

    /* Previous E pin state for edge detection */
    bool         e_last;

    /* Embedded panel widgets */
    GtkWidget   *panel;          /* outer box containing header + drawing area */
    GtkWidget   *drawing;        /* GtkDrawingArea for character grid */
    GtkWidget   *lbl_mode;       /* mode label: "20×4  8-bit  Permissive" */
    GtkWidget   *btn_popout;     /* pop-out button */
    GtkWidget   *btn_strict;     /* strict/permissive toggle */

    /* Pop-out window (NULL if not open) */
    GtkWidget   *popout_win;
    GtkWidget   *popout_drawing;

    /* Pixel dimensions for drawing */
    int          char_w;         /* width of one character cell in pixels */
    int          char_h;         /* height of one character cell in pixels */
    int          pixel_w;        /* width of one LCD pixel in pixels */
    int          pixel_h;        /* height of one LCD pixel */

} lcd_ui_t;

/* -------------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------------- */

/* Create the embedded LCD panel. Returns the panel GtkWidget.
 * w = parent sim_win_t (for VIA access and state_lock). */
GtkWidget *lcd_ui_create(struct sim_win_t *w);

/* Called from VIA write hooks whenever VIA1 or VIA2 output changes.
 * Reads the configured pins and detects E falling edge. */
void lcd_ui_via_changed(struct sim_win_t *w, int via_idx);

/* Called from sim_win_update to redraw the LCD panel */
void lcd_ui_update(struct sim_win_t *w);

/* Called on CPU reset */
void lcd_ui_reset(struct sim_win_t *w);

/* Apply a new LCD config (e.g. from profile editor) */
void lcd_ui_apply_cfg(struct sim_win_t *w, const lcd_cfg_t *cfg);

/* Called when simulator speed changes, so timing stays accurate */
void lcd_ui_set_cpu_hz(struct sim_win_t *w, uint64_t cpu_hz);
