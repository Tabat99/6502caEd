/* sim_gtk.c — GTK4 simulator window
 *
 * Layout:
 *   ┌─ toolbar: Run / Stop / Step / Reset ──────────────────────────┐
 *   ├─ left pane (terminal) ──┬─ right pane (registers/debug) ──────┤
 *   │  scrolled text view     │  register grid                      │
 *   │                         │  flags row                          │
 *   │                         │  cycle counter                      │
 *   │                         │  breakpoints                        │
 *   │                         │  disassembly list                   │
 *   ├─────────────────────────┴─────────────────────────────────────┤
 *   │  VIA1 panel │ VIA2 panel │ ACIA panel                         │
 *   └────────────────────────────────────────────────────────────────┘
 */
#include "sim_gtk.h"
#include "cpu.h"
#include "simprofile.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
#  define SIM_MKDIR(p) _mkdir(p)
#else
#  define SIM_MKDIR(p) mkdir(p, 0755)
#endif

/* ---------------------------------------------------------------------------
 * Colours for VIA port bit indicators
 * ------------------------------------------------------------------------- */
#define CSS_SIM                                                                \
  "label.bit-high  { background: #2d7a2d; color: #e8ffe8; "                    \
  "                  border-radius: 4px; padding: 2px 5px; "                   \
  "                  font-family: monospace; font-size: 11px; }\n"             \
  "label.bit-low   { background: #7a2d2d; color: #ffe8e8; "                    \
  "                  border-radius: 4px; padding: 2px 5px; "                   \
  "                  font-family: monospace; font-size: 11px; }\n"             \
  "label.bit-input { background: #444; color: #aaa; "                          \
  "                  border-radius: 4px; padding: 2px 5px; "                   \
  "                  font-family: monospace; font-size: 11px; }\n"             \
  "label.reg-val   { font-family: monospace; font-size: 13px; "                \
  "                  font-weight: bold; }\n"                                   \
  "label.reg-name  { font-family: monospace; font-size: 11px; "                \
  "                  color: #888; }\n"                                         \
  "label.flag-set  { font-family: monospace; font-weight: bold; "              \
  "                  color: #6af; }\n"                                         \
  "label.flag-clr  { font-family: monospace; color: #555; }\n"                 \
  "label.pc-label  { font-family: monospace; font-size: 14px; "                \
  "                  font-weight: bold; color: #fa0; }\n"                      \
  "textview.terminal { font-family: monospace; font-size: 13px; "              \
  "                    background: #0a0a0a; color: #d0d0d0; }\n"               \
  ".sim-section    { border: 1px solid #333; border-radius: 4px; "             \
  "                  padding: 4px; margin: 2px; }\n"                           \
  "label.section-title { font-size: 10px; color: #666; "                       \
  "                      font-weight: bold; }\n"                               \
  "label.acia-conn  { color: #2d7; font-weight: bold; }\n"                     \
  "label.acia-disc  { color: #744; }\n"\
  "label.speed-warn { font-family: monospace; font-size: 11px; font-weight: bold; color: #f80; background: rgba(255,136,0,0.15); border-radius: 4px; padding: 2px 6px; min-width: 220px; }\n"

#define UPDATE_INTERVAL_MS 100 /* UI refresh rate */
/* Types and sim_win_t struct defined in sim_gtk.h */

/* ---------------------------------------------------------------------------
 * LCD GTK rendering — inlined from lcd_gtk.c
 * ------------------------------------------------------------------------- */
/* lcd_gtk.c — GTK4 rendering for HD44780 LCD emulation */

/* -------------------------------------------------------------------------
 * Drawing constants
 * ------------------------------------------------------------------------- */
#define LCD_PIX_W     3    /* pixels per LCD dot (embedded) */
#define LCD_PIX_H     4    /* pixels per LCD dot (embedded) */
#define LCD_PIX_W_POP 5    /* pixels per LCD dot (pop-out) */
#define LCD_PIX_H_POP 7    /* pixels per LCD dot (pop-out) */
#define LCD_GAP_X     1    /* gap between pixels horizontally */
#define LCD_GAP_Y     1    /* gap between pixels vertically */
#define LCD_CELL_PAD_X 2   /* padding between characters horizontally */
#define LCD_CELL_PAD_Y 4   /* padding between characters vertically */
#define LCD_MARGIN    6    /* outer margin */

/* LCD green background and pixel colours */
static const double LCD_BG_R = 0.13, LCD_BG_G = 0.37, LCD_BG_B = 0.13;
static const double LCD_ON_R = 0.07, LCD_ON_G = 0.15, LCD_ON_B = 0.07;
static const double LCD_OFF_R= 0.17, LCD_OFF_G= 0.47, LCD_OFF_B= 0.17;

/* -------------------------------------------------------------------------
 * Read a single pin from the VIA output state
 * Returns 0 or 1
 * ------------------------------------------------------------------------- */
static int read_pin(struct sim_win_t *w, const lcd_pin_t *pin) {
    via_t *v = &w->sim.via[pin->via - 1];
    uint8_t out = (pin->port == 0) ? via_get_pa_out(v) : via_get_pb_out(v);
    return (out >> pin->bit) & 1;
}

/* Read 8-bit data bus (or 4-bit nibble for 4-bit mode) from VIA pins */
static uint8_t read_data_pins(struct sim_win_t *w) {
    lcd_cfg_t *cfg = &w->lcd_ui.cfg;
    int nbits = (cfg->bus_width == 4) ? 4 : 8;
    uint8_t val = 0;
    for (int i = 0; i < nbits; i++) {
        if (read_pin(w, &cfg->data[i]))
            val |= (1 << i);
    }
    return val;
}

/* Write a pin back to the VIA input (for read operations from LCD) */
static void write_pin(struct sim_win_t *w, const lcd_pin_t *pin, int value) {
    via_t *v = &w->sim.via[pin->via - 1];
    uint8_t mask = (uint8_t)(1 << pin->bit);
    uint8_t cur  = (pin->port == 0) ? v->PA_in : v->PB_in;
    if (value)
        cur |= mask;
    else
        cur &= ~mask;
    if (pin->port == 0)
        via_set_pa(v, cur);
    else
        via_set_pb(v, cur);
}

/* Write the data bus back to VIA input pins (for LCD→CPU reads) */
static void write_data_pins(struct sim_win_t *w, uint8_t val) {
    lcd_cfg_t *cfg = &w->lcd_ui.cfg;
    int nbits = (cfg->bus_width == 4) ? 4 : 8;
    for (int i = 0; i < nbits; i++)
        write_pin(w, &cfg->data[i], (val >> i) & 1);
}

/* -------------------------------------------------------------------------
 * E-strobe detection and LCD command dispatch
 * ------------------------------------------------------------------------- */
void lcd_ui_via_changed(struct sim_win_t *w, int via_idx) {
    lcd_ui_t *ui = &w->lcd_ui;
    if (!ui->cfg.enabled) return;

    /* Check if the E pin belongs to this VIA */
    if (ui->cfg.e.via - 1 != via_idx) return;

    bool e_now = read_pin(w, &ui->cfg.e) != 0;

    bool rs = read_pin(w, &ui->cfg.rs) != 0;
    bool rw = read_pin(w, &ui->cfg.rw) != 0;

    /* Real HD44780 read timing:
     * When RW=1, the LCD drives DB0-DB7 while E is high.  The CPU/VIA
     * must therefore be able to read the data bus before E falls.
     *
     * Minimal simulator fix: on E rising edge, pre-drive the VIA input
     * pins with the LCD read value.  The existing falling-edge strobe is
     * kept below so read-side effects still occur at the end of the E
     * pulse, while writes continue to latch on E falling edge as before.
     */
    if (!ui->e_last && e_now && rw) {
        uint8_t result = rs ? lcd_read_data(&ui->lcd)
                            : lcd_read_bf_ac(&ui->lcd);
        write_data_pins(w, result);
    }

    /* Detect falling edge of E — write data is latched on E↓ */
    if (ui->e_last && !e_now) {
        uint8_t data = rw ? 0 : read_data_pins(w);

        bool busy = false;
        uint8_t result = lcd_strobe(&ui->lcd, rs, rw, data, &busy);

        /* For read operations, leave the final value on the VIA input pins. */
        if (rw) {
            write_data_pins(w, result);
        }
    }
    ui->e_last = e_now;
}

/* -------------------------------------------------------------------------
 * Drawing
 * ------------------------------------------------------------------------- */
static void draw_lcd(GtkDrawingArea *area, cairo_t *cr,
                     int width, int height, gpointer ud) {
    struct sim_win_t *w = (struct sim_win_t *)ud;
    lcd_ui_t *ui = &w->lcd_ui;
    lcd_t    *lcd = &ui->lcd;
    int cols = lcd->cols;
    int rows = lcd->rows;
    bool popout = (GTK_WIDGET(area) == ui->popout_drawing);

    int pw = popout ? LCD_PIX_W_POP : LCD_PIX_W;
    int ph = popout ? LCD_PIX_H_POP : LCD_PIX_H;

    /* Character cell size: 5 pixels wide + gaps, 8 rows + gaps */
    int cell_w = 5 * (pw + LCD_GAP_X) - LCD_GAP_X + LCD_CELL_PAD_X;
    int cell_h = 8 * (ph + LCD_GAP_Y) - LCD_GAP_Y + LCD_CELL_PAD_Y;

    int total_w = LCD_MARGIN * 2 + cols * cell_w;
    int total_h = LCD_MARGIN * 2 + rows * cell_h;

    /* Background — fill full allocated widget area to avoid border artifacts */
    cairo_set_source_rgb(cr, LCD_BG_R, LCD_BG_G, LCD_BG_B);
    cairo_rectangle(cr, 0, 0, width, height);
    cairo_fill(cr);

    if (!ui->cfg.enabled) {
        /* Show "LCD disabled" message */
        cairo_set_source_rgb(cr, LCD_OFF_R, LCD_OFF_G, LCD_OFF_B);
        cairo_select_font_face(cr, "monospace", CAIRO_FONT_SLANT_NORMAL,
                               CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, 11);
        cairo_move_to(cr, LCD_MARGIN, total_h / 2);
        cairo_show_text(cr, "LCD disabled");
        return;
    }

    /* Draw each character */
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            uint8_t ch = lcd_get_char(lcd, col, row);
            uint8_t pixels[8];
            lcd_get_bitmap(lcd, ch, pixels);

            bool cursor = lcd_cursor_at(lcd, col, row);
            bool blink  = cursor && lcd_blink_on(lcd);

            int cx = LCD_MARGIN + col * cell_w;
            int cy = LCD_MARGIN + row * cell_h;

            for (int py = 0; py < 8; py++) {
                for (int px = 0; px < 5; px++) {
                    bool on;
                    if (blink)
                        on = true; /* blink: all pixels on */
                    else
                        on = (pixels[py] >> (4 - px)) & 1;

                    /* Cursor underline on row 7 */
                    if (cursor && !blink && py == 7)
                        on = true;

                    double r = on ? LCD_ON_R : LCD_OFF_R;
                    double g = on ? LCD_ON_G : LCD_OFF_G;
                    double b = on ? LCD_ON_B : LCD_OFF_B;

                    cairo_set_source_rgb(cr, r, g, b);
                    cairo_rectangle(cr,
                        cx + px * (pw + LCD_GAP_X),
                        cy + py * (ph + LCD_GAP_Y),
                        pw, ph);
                    cairo_fill(cr);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * Pop-out window
 * ------------------------------------------------------------------------- */
static void on_popout_close(GtkWindow *win, gpointer ud) {
    (void)win;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    w->lcd_ui.popout_win     = NULL;
    w->lcd_ui.popout_drawing = NULL;
}

static void on_popout_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    lcd_ui_t *ui = &w->lcd_ui;

    if (ui->popout_win) {
        gtk_window_present(GTK_WINDOW(ui->popout_win));
        return;
    }

    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "LCD Display");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(w->window));
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    /* Match dark/light */
    if (gtk_widget_has_css_class(w->window, "dark"))
        gtk_widget_add_css_class(win, "dark");

    int cols = ui->lcd.cols;
    int rows = ui->lcd.rows;
    int cell_w = 5 * (LCD_PIX_W_POP + LCD_GAP_X) - LCD_GAP_X + LCD_CELL_PAD_X;
    int cell_h = 8 * (LCD_PIX_H_POP + LCD_GAP_Y) - LCD_GAP_Y + LCD_CELL_PAD_Y;
    int tw = LCD_MARGIN * 2 + cols * cell_w;
    int th = LCD_MARGIN * 2 + rows * cell_h;

    GtkWidget *da = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), tw);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), th);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_lcd, w, NULL);
    gtk_window_set_child(GTK_WINDOW(win), da);

    ui->popout_win     = win;
    ui->popout_drawing = da;

    g_signal_connect(win, "destroy", G_CALLBACK(on_popout_close), w);
    gtk_window_present(GTK_WINDOW(win));
}

/* -------------------------------------------------------------------------
 * Strict mode toggle
 * ------------------------------------------------------------------------- */
static void on_strict_toggled(GtkCheckButton *btn, gpointer ud) {
    struct sim_win_t *w = (struct sim_win_t *)ud;
    bool strict = gtk_check_button_get_active(btn);
    w->lcd_ui.cfg.strict = strict;
    lcd_set_strict(&w->lcd_ui.lcd, strict);
}

/* -------------------------------------------------------------------------
 * Panel creation
 * ------------------------------------------------------------------------- */
/* Resize the embedded LCD drawing area to match current cols/rows */
static void lcd_ui_resize_drawing(struct sim_win_t *w) {
    lcd_ui_t *ui = &w->lcd_ui;
    if (!ui->drawing) return;
    int cell_w = 5 * (LCD_PIX_W + LCD_GAP_X) - LCD_GAP_X + LCD_CELL_PAD_X;
    int cell_h = 8 * (LCD_PIX_H + LCD_GAP_Y) - LCD_GAP_Y + LCD_CELL_PAD_Y;
    int tw = LCD_MARGIN * 2 + ui->lcd.cols * cell_w;
    int th = LCD_MARGIN * 2 + ui->lcd.rows * cell_h;
    gtk_widget_set_size_request(ui->drawing, tw, th);
    gtk_widget_queue_draw(ui->drawing);
}

static void on_lcd_size_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud) {
    (void)ps;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    lcd_ui_t *ui = &w->lcd_ui;
    guint sel = gtk_drop_down_get_selected(dd);
    /* 0 = 20x4, 1 = 16x2 */
    int new_cols = (sel == 1) ? 16 : 20;
    int new_rows = (sel == 1) ? 2  : 4;
    if (ui->lcd.cols == new_cols && ui->lcd.rows == new_rows) return;
    /* Update config and reinit LCD core */
    ui->cfg.cols = new_cols;
    ui->cfg.rows = new_rows;
    lcd_init(&ui->lcd, new_cols, new_rows, ui->lcd.cpu_hz);
    lcd_set_strict(&ui->lcd, ui->cfg.strict);
    ui->lcd.is_4bit_iface = (ui->cfg.bus_width == 4);
    ui->e_last = false;
    /* Update mode label */
    if (ui->lbl_mode) {
        char mode_str[64];
        snprintf(mode_str, sizeof(mode_str), "%dÃ%d  %d-bit  %s",
                 new_cols, new_rows, ui->cfg.bus_width,
                 ui->cfg.strict ? "Strict" : "Permissive");
        gtk_label_set_text(GTK_LABEL(ui->lbl_mode), mode_str);
    }
    /* Resize the drawing area */
    lcd_ui_resize_drawing(w);
    /* Resize pop-out if open */
    if (ui->popout_drawing) {
        int cell_w = 5 * (LCD_PIX_W_POP + LCD_GAP_X) - LCD_GAP_X + LCD_CELL_PAD_X;
        int cell_h = 8 * (LCD_PIX_H_POP + LCD_GAP_Y) - LCD_GAP_Y + LCD_CELL_PAD_Y;
        int tw = LCD_MARGIN * 2 + new_cols * cell_w;
        int th = LCD_MARGIN * 2 + new_rows * cell_h;
        gtk_widget_set_size_request(ui->popout_drawing, tw, th);
        if (ui->popout_win)
            gtk_window_set_default_size(GTK_WINDOW(ui->popout_win), tw, th);
        gtk_widget_queue_draw(ui->popout_drawing);
    }
}

GtkWidget *lcd_ui_create(struct sim_win_t *w) {
    lcd_ui_t *ui = &w->lcd_ui;
    lcd_cfg_t *cfg = &ui->cfg;

    /* Initialise LCD core */
    lcd_init(&ui->lcd, cfg->cols, cfg->rows, 1000000);
    lcd_set_strict(&ui->lcd, cfg->strict);
    ui->lcd.is_4bit_iface = (cfg->bus_width == 4);
    ui->e_last = false;

    /* Outer panel */
    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(panel, "sim-section");
    ui->panel = panel;

    /* Header row */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *title = gtk_label_new("LCD");
    gtk_widget_add_css_class(title, "section-title");
    gtk_widget_set_hexpand(title, TRUE);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(hdr), title);

    /* Mode label */
    char mode_str[64];
    snprintf(mode_str, sizeof(mode_str), "%d×%d  %d-bit  %s",
             cfg->cols, cfg->rows, cfg->bus_width,
             cfg->strict ? "Strict" : "Permissive");
    GtkWidget *lbl = gtk_label_new(mode_str);
    gtk_widget_add_css_class(lbl, "reg-name");
    gtk_box_append(GTK_BOX(hdr), lbl);
    ui->lbl_mode = lbl;

    /* Strict toggle */
    GtkWidget *chk = gtk_check_button_new_with_label("Strict timing");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), cfg->strict);
    gtk_widget_add_css_class(chk, "reg-name");
    g_signal_connect(chk, "toggled", G_CALLBACK(on_strict_toggled), w);
    gtk_box_append(GTK_BOX(hdr), chk);
    ui->btn_strict = chk;

    /* Size toggle dropdown */
    GtkWidget *dd_size = gtk_drop_down_new_from_strings(
        (const char *[]){"20×4", "16×2", NULL});
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd_size),
        (cfg->cols == 16 && cfg->rows == 2) ? 1 : 0);
    gtk_widget_set_tooltip_text(dd_size, "Switch display size (resets LCD)");
    g_signal_connect(dd_size, "notify::selected",
        G_CALLBACK(on_lcd_size_changed), w);
    gtk_box_append(GTK_BOX(hdr), dd_size);

    /* Pop-out button */
    GtkWidget *btn = gtk_button_new_with_label("⤢");
    gtk_widget_set_tooltip_text(btn, "Pop out LCD display");
    g_signal_connect(btn, "clicked", G_CALLBACK(on_popout_clicked), w);
    gtk_box_append(GTK_BOX(hdr), btn);
    ui->btn_popout = btn;

    gtk_box_append(GTK_BOX(panel), hdr);

    /* Drawing area */
    int cell_w = 5 * (LCD_PIX_W + LCD_GAP_X) - LCD_GAP_X + LCD_CELL_PAD_X;
    int cell_h = 8 * (LCD_PIX_H + LCD_GAP_Y) - LCD_GAP_Y + LCD_CELL_PAD_Y;
    int tw = LCD_MARGIN * 2 + cfg->cols * cell_w;
    int th = LCD_MARGIN * 2 + cfg->rows * cell_h;

    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, tw, th);
    gtk_widget_set_hexpand(da, FALSE);
    gtk_widget_set_halign(da, GTK_ALIGN_START);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(da), draw_lcd, w, NULL);
    gtk_box_append(GTK_BOX(panel), da);
    ui->drawing = da;

    return panel;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void lcd_ui_update(struct sim_win_t *w) {
    lcd_ui_t *ui = &w->lcd_ui;
    if (!ui->cfg.enabled) return;

    /* Redraw embedded panel */
    if (ui->drawing)
        gtk_widget_queue_draw(ui->drawing);

    /* Redraw pop-out if open */
    if (ui->popout_drawing)
        gtk_widget_queue_draw(ui->popout_drawing);
}

void lcd_ui_reset(struct sim_win_t *w) {
    lcd_reset(&w->lcd_ui.lcd);
    w->lcd_ui.e_last = false;
}

void lcd_ui_apply_cfg(struct sim_win_t *w, const lcd_cfg_t *cfg) {
    lcd_ui_t *ui = &w->lcd_ui;
    ui->cfg = *cfg;
    lcd_init(&ui->lcd, cfg->cols, cfg->rows, ui->lcd.cpu_hz);
    lcd_set_strict(&ui->lcd, cfg->strict);
    ui->lcd.is_4bit_iface = (cfg->bus_width == 4);
    ui->e_last = false;
    /* Show or hide the LCD panel widget */
    if (w->lcd_panel_widget)
        gtk_widget_set_visible(w->lcd_panel_widget, cfg->enabled);
    /* Update middle column visibility */
    if (w->bottom_col_mid) {
        bool mid = cfg->enabled || w->sd_ui.cfg.enabled;
        gtk_widget_set_visible(w->bottom_col_mid, mid);
        if (w->bottom_col_sep) gtk_widget_set_visible(w->bottom_col_sep, mid);
        if (w->bottom_mid_sep)
            gtk_widget_set_visible(w->bottom_mid_sep,
                                   cfg->enabled && w->sd_ui.cfg.enabled);
    }

    /* Update mode label */
    if (ui->lbl_mode) {
        char mode_str[64];
        snprintf(mode_str, sizeof(mode_str), "%d×%d  %d-bit  %s",
                 cfg->cols, cfg->rows, cfg->bus_width,
                 cfg->strict ? "Strict" : "Permissive");
        gtk_label_set_text(GTK_LABEL(ui->lbl_mode), mode_str);
    }
}

void lcd_ui_set_cpu_hz(struct sim_win_t *w, uint64_t cpu_hz) {
    lcd_set_cpu_hz(&w->lcd_ui.lcd, cpu_hz);
}

/* ---------------------------------------------------------------------------
 * I/O dispatch callbacks
 * ------------------------------------------------------------------------- */
static uint8_t via1_rd(uint32_t addr, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  return via_read(&w->sim.via[0], (uint8_t)(addr - w->cfg.via1_base));
}
static void via1_wr(uint32_t addr, uint8_t val, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  via_write(&w->sim.via[0], (uint8_t)(addr - w->cfg.via1_base), val);
  lcd_ui_via_changed(w, 0); /* notify LCD of VIA1 output change */
  sd_ui_via_changed(w, 0);  /* notify SD  of VIA1 output change */
}
static uint8_t via2_rd(uint32_t addr, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  return via_read(&w->sim.via[1], (uint8_t)(addr - w->cfg.via2_base));
}
static void via2_wr(uint32_t addr, uint8_t val, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  via_write(&w->sim.via[1], (uint8_t)(addr - w->cfg.via2_base), val);
  lcd_ui_via_changed(w, 1); /* notify LCD of VIA2 output change */
  sd_ui_via_changed(w, 1);  /* notify SD  of VIA2 output change */
}
static uint8_t acia_rd(uint32_t addr, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  return acia_read(&w->sim.acia, (uint8_t)(addr - w->cfg.acia_base));
}
static void acia_wr(uint32_t addr, uint8_t val, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  acia_write(&w->sim.acia, (uint8_t)(addr - w->cfg.acia_base), val);
}
static uint8_t simio_rd(uint32_t addr, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  if ((w->profile.simio_rx.enabled || w->cfg.simio_enable) &&
      addr == (uint32_t)(w->profile.simio_rx.enabled ? w->profile.simio_rx.addr
                                                     : w->cfg.simio_rx_addr)) {
    /* Block until a keypress arrives in term_in.
     * IMPORTANT: the sim thread holds state_lock while executing.
     * We must RELEASE it while waiting so the UI thread can still
     * update the display and push keypresses. Reacquire after. */
    char c = 0;
    while (w->sim.running && !w->sim.paused) {
      /* Release lock so UI thread can run */
      pthread_mutex_unlock(&w->sim.state_lock);

      /* Short sleep — 1ms for responsiveness */
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);

      /* Check running/paused before reacquiring */
      if (!w->sim.running || w->sim.paused)
        return 0xFF;

      /* Reacquire state_lock and check for input */
      pthread_mutex_lock(&w->sim.state_lock);
      int got = sim_term_in_pop(&w->sim, &c);
      if (got) {
        return (uint8_t)c;
      }
    }
    return 0xFF;
  }
  return 0xFF;
}
/* LCD per-cycle tick callback — called from sim thread */
static void lcd_tick_cb(void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  if (w->lcd_ui.cfg.enabled)
    lcd_tick(&w->lcd_ui.lcd);
  if (w->sd_ui.cfg.enabled)
    sd_tick(&w->sd_ui.sd);
}

/* Forward declarations for callbacks used in sim_apply_profile */
static uint8_t via1_rd(uint32_t addr, void *ud);
static void via1_wr(uint32_t addr, uint8_t val, void *ud);
static uint8_t via2_rd(uint32_t addr, void *ud);
static void via2_wr(uint32_t addr, uint8_t val, void *ud);
static uint8_t acia_rd(uint32_t addr, void *ud);
static void acia_wr(uint32_t addr, uint8_t val, void *ud);
static uint8_t simio_rd(uint32_t addr, void *ud);
static void simio_wr(uint32_t addr, uint8_t val, void *ud);
static void bank_wr(uint32_t addr, uint8_t val, void *ud);
static void acia_irq_cb(bool asserted, void *ud);
static bool profile_ed_save_and_apply(GtkWidget *dlg, void *ctx);
static void on_profile_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud);

/* -----------------------------------------------------------------------
 * ROM control register IO handler
 */
static uint8_t rom_ctrl_rd(uint32_t addr, void *ud) {
  (void)addr;
  sim_win_t *w = (sim_win_t *)ud;
  return simprofile_ctrl_read(&w->profile);
}
static void rom_ctrl_wr(uint32_t addr, uint8_t val, void *ud) {
  (void)addr;
  sim_win_t *w = (sim_win_t *)ud;
  simprofile_ctrl_write(&w->profile, val);
}

/* -----------------------------------------------------------------------
 * Apply a profile to the simulator — rebuilds IO map from profile settings.
 * Call after sim_init() and after loading ROM data.
 * profile is copied into w->profile.
 */
/* ---------------------------------------------------------------------------
 * ROM range IO callbacks
 *
 * Each enabled ROM range is registered as a MEM_IO region so that reads
 * and writes can be dispatched based on the range's current rw_state:
 *
 *   rw_state=0 (default): ROM mode
 *     reads  → return the ROM image byte from backing RAM
 *     writes → go to backing RAM (shadow write-through)
 *
 *   rw_state=1: RAM overlay mode
 *     reads  → return the backing RAM byte (overlay)
 *     writes → silently discarded
 *
 * This is called from mem_read/mem_write with the per-range rom_ud as ud.
 * The backing data is always w->sim.mem.ram[addr], which holds the
 * assembled binary (ROM image).  In rw_state=0 shadow mode the CPU can
 * write to RAM beneath the ROM (e.g. self-modifying code workarounds),
 * but reads still come from the ROM image — this matches the hardware
 * behaviour described in the profile spec.
 * ------------------------------------------------------------------------- */
/* Take an immutable snapshot of mem.ram for ROM-mode reads.
 * Called after the assembled binary has been loaded into mem.ram,
 * and again whenever Reload ROM refreshes the binary. */
static void sim_take_rom_snapshot(sim_win_t *w) {
  uint32_t sz = (uint32_t)w->sim.mem.ram_size;
  if (w->sim.rom_image_size != sz) {
    free(w->sim.rom_image);
    w->sim.rom_image = malloc(sz);
    w->sim.rom_image_size = w->sim.rom_image ? sz : 0;
  }
  if (w->sim.rom_image)
    memcpy(w->sim.rom_image, w->sim.mem.ram, sz);
}

static uint8_t rom_range_rd(uint32_t addr, void *ud) {
  typeof(((sim_win_t *)NULL)->rom_ud[0]) *ru = ud;
  sim_win_t *w   = ru->w;
  int        idx = ru->idx;
  const profile_rom_t *r = &w->profile.rom[idx];
  uint16_t a = addr & 0xFFFF;

  if (!r->rw_state) {
    /* rw_state=0: ROM mode — read from the immutable ROM image snapshot.
     * Shadow writes go to mem.ram but reads must still return ROM content. */
    if (w->sim.rom_image && a < w->sim.rom_image_size)
      return w->sim.rom_image[a];
    return 0xFF;
  } else {
    /* rw_state=1: RAM overlay mode — read from live mem.ram */
    return w->sim.mem.ram[a];
  }
}

static void rom_range_wr(uint32_t addr, uint8_t val, void *ud) {
  typeof(((sim_win_t *)NULL)->rom_ud[0]) *ru = ud;
  sim_win_t *w  = ru->w;
  int        idx = ru->idx;
  const profile_rom_t *r = &w->profile.rom[idx];

  if (!r->rw_state) {
    /* rw_state=0: shadow write — goes to backing RAM beneath the ROM */
    w->sim.mem.ram[addr & 0xFFFF] = val;
  }
  /* rw_state=1: RAM overlay — writes are lost (hardware behaviour) */
}

static void sim_apply_profile(sim_win_t *w, const simprofile_t *prof,
                              const simcfg_t *cfg) {
  /* Copy profile into window */
  w->profile = *prof;
  simprofile_reset_locks(&w->profile);

  /* Apply LCD config from profile */
  lcd_ui_apply_cfg(w, &prof->lcd);

  /* Apply SD card config from profile */
  sd_ui_apply_cfg(w, &prof->sd);

  const simprofile_t *p = &w->profile;

  /* ROM ranges — registered as IO regions so reads/writes can be
   * dispatched dynamically based on rw_state (set via ctrl register).
   * They are added after the flat RAM region so find_region() (which
   * searches in reverse) finds them first. */
  for (int i = 0; i < PROFILE_ROM_MAX; i++) {
    const profile_rom_t *r = &p->rom[i];
    if (!r->enabled) continue;
    w->rom_ud[i].w   = w;
    w->rom_ud[i].idx = i;
    uint32_t sz = (uint32_t)(r->to - r->from + 1);
    mem_add_io(&w->sim.mem, r->from, sz,
               rom_range_rd, rom_range_wr, &w->rom_ud[i], "ROM");
  }

  /* VIA1 */
  if (p->via1.enabled)
    mem_add_io(&w->sim.mem, p->via1.addr, 16, via1_rd, via1_wr, w, "VIA1");

  /* VIA2 */
  if (p->via2.enabled)
    mem_add_io(&w->sim.mem, p->via2.addr, 16, via2_rd, via2_wr, w, "VIA2");

  /* ACIA */
  int cpu_hz = 1000000;
  if (cfg) {
    const long hz_table[] = {2000000, 1000000, 512000, 128000, 32000, 8000, 2000,
                             512,     128,     32,     8,      2,    1,    0};
    int spd = (int)cfg->speed;
    if (spd >= 0 && spd <= 13)
      cpu_hz = (int)hz_table[spd];
  }
  acia_init(&w->sim.acia, cfg ? cfg->acia_tcp_port : 6551, cpu_hz, acia_irq_cb,
            w);
  w->sim.acia_enable = p->acia.enabled;
  if (p->acia.enabled) {
    mem_add_io(&w->sim.mem, p->acia.addr, 4, acia_rd, acia_wr, w, "ACIA");
    acia_tcp_open(&w->sim.acia);
  }

  /* Simple IO (TX/RX share a 2-byte region) */
  if (p->simio_tx.enabled || p->simio_rx.enabled) {
    uint32_t base = p->simio_tx.enabled ? p->simio_tx.addr : p->simio_rx.addr;
    mem_add_io(&w->sim.mem, base, 2, simio_rd, simio_wr, w, "SIMIO");
  }

  /* ROM control register */
  if (p->rom_ctrl.enabled)
    mem_add_io(&w->sim.mem, p->rom_ctrl.addr, 1, rom_ctrl_rd, rom_ctrl_wr, w,
               "ROMCTRL");

  /* Banking (6502/65C02 only) */
  w->sim.bank_enable = p->bank_reg.enabled && p->cpu_type != PROFILE_CPU_65816;
  w->sim.bank_addr = p->bank_reg.addr;
  w->sim.bank_reg = 0;
  w->sim.bank_window = p->bank_window;
  w->sim.bank_window_size = p->bank_window_sz;
  if (w->sim.bank_enable)
    mem_add_io(&w->sim.mem, p->bank_reg.addr, 1, NULL, bank_wr, w, "BANK");

  /* Update VIA base address fields used by rd/wr callbacks */
  /* (callbacks use cfg fields — patch them from profile) */
  if (cfg) {
    /* We can't easily modify cfg (const), so we patch sim-level
     * via the cfg copy stored in w->cfg */
    w->cfg.via1_base = p->via1.addr;
    w->cfg.via1_enable = p->via1.enabled;
    w->cfg.via2_base = p->via2.addr;
    w->cfg.via2_enable = p->via2.enabled;
    w->cfg.acia_base = p->acia.addr;
    w->cfg.acia_enable = p->acia.enabled;
    w->cfg.simio_enable = p->simio_tx.enabled || p->simio_rx.enabled;
    w->cfg.simio_tx_addr = p->simio_tx.addr;
    w->cfg.simio_rx_addr = p->simio_rx.addr;
  }
}

static void bank_wr(uint32_t addr, uint8_t val, void *ud) {
  (void)addr;
  sim_win_t *w = (sim_win_t *)ud;
  sim_t *s = &w->sim;
  if (!s->bank_enable || !s->bank_rom)
    return;
  s->bank_reg = val;
  /* Map selected bank into the bank window */
  uint32_t bank_size = s->bank_window_size;
  uint32_t offset = (uint32_t)val * bank_size;
  if (offset + bank_size <= s->bank_rom_size) {
    /* Copy selected bank into RAM at bank window */
    memcpy(s->mem.ram + s->bank_window, s->bank_rom + offset, bank_size);
  }
}

static void simio_wr(uint32_t addr, uint8_t val, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  if ((w->profile.simio_tx.enabled || w->cfg.simio_enable) &&
      addr == (uint32_t)(w->profile.simio_tx.enabled ? w->profile.simio_tx.addr
                                                     : w->cfg.simio_tx_addr)) {
    /* TX: CPU writes char → push to term_out so UI displays it */
    sim_term_out_push(&w->sim, (char)val);
  }
}

/* VIA IRQ callbacks */
static void via_irq_cb(bool asserted, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  w->sim.cpu.irq = asserted;
}
static void acia_irq_cb(bool asserted, void *ud) {
  sim_win_t *w = (sim_win_t *)ud;
  if (asserted)
    w->sim.cpu.irq = true;
}

/* ---------------------------------------------------------------------------
 * Terminal append (called from update timer — main thread only)
 * ------------------------------------------------------------------------- */
#define TERM_COLS 80       /* wrap column */
#define TERM_MAX_LINES 500 /* clear oldest lines when exceeded */

static void terminal_append(sim_win_t *w, const char *text, int len) {
  GtkTextIter end;
  /* Process character by character to handle wrapping */
  for (int i = 0; i < len; i++) {
    char ch = text[i];

    /* Printable or control chars we handle */
    if (ch == '\r')
      continue; /* ignore CR */
    if (ch == '\n' || ch == '\0') {
      gtk_text_buffer_get_end_iter(w->tbuf, &end);
      gtk_text_buffer_insert(w->tbuf, &end, "\n", 1);
      w->term_col = 0;
      continue;
    }
    if (ch == '\b') {
      /* Backspace — delete last char if any on this line */
      if (w->term_col > 0) {
        GtkTextIter bs_end, bs_start;
        gtk_text_buffer_get_end_iter(w->tbuf, &bs_end);
        bs_start = bs_end;
        gtk_text_iter_backward_char(&bs_start);
        gtk_text_buffer_delete(w->tbuf, &bs_start, &bs_end);
        w->term_col--;
      }
      continue;
    }

    /* Auto-wrap at TERM_COLS */
    if (w->term_col >= TERM_COLS) {
      gtk_text_buffer_get_end_iter(w->tbuf, &end);
      gtk_text_buffer_insert(w->tbuf, &end, "\n", 1);
      w->term_col = 0;
    }

    gtk_text_buffer_get_end_iter(w->tbuf, &end);
    gtk_text_buffer_insert(w->tbuf, &end, &ch, 1);
    w->term_col++;
  }

  /* Trim old lines if buffer is too large */
  int lines = gtk_text_buffer_get_line_count(w->tbuf);
  if (lines > TERM_MAX_LINES) {
    GtkTextIter from, to;
    gtk_text_buffer_get_start_iter(w->tbuf, &from);
    gtk_text_buffer_get_iter_at_line(w->tbuf, &to, lines - TERM_MAX_LINES);
    gtk_text_buffer_delete(w->tbuf, &from, &to);
  }

  /* Scroll to end */
  gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(w->tv), w->tend);
}

/* ---------------------------------------------------------------------------
 * Key press on terminal view → send to simulator
 * ------------------------------------------------------------------------- */
/* Forward declarations for toolbar callbacks used in key handler */
static void on_run(GtkButton *b, gpointer ud);
static void on_stop(GtkButton *b, gpointer ud);
static void on_step(GtkButton *b, gpointer ud);
static void on_reset(GtkButton *b, gpointer ud);
static void on_cpu_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud);

static gboolean on_key_press(GtkEventControllerKey *ctrl, guint keyval,
                             guint keycode, GdkModifierType mods, gpointer ud) {
  sim_win_t *w = (sim_win_t *)ud;
  (void)ctrl;
  (void)keycode;

  bool ctrl_held = (mods & GDK_CONTROL_MASK) != 0;

  /* Simulator control keys — handled before terminal input */
  if (keyval == GDK_KEY_F5) {
    on_run(NULL, w);
    return TRUE;
  }
  if (keyval == GDK_KEY_F6) {
    on_stop(NULL, w);
    return TRUE;
  }
  if (keyval == GDK_KEY_F7) {
    on_step(NULL, w);
    return TRUE;
  }
  if (ctrl_held && (keyval == GDK_KEY_r || keyval == GDK_KEY_R)) {
    on_reset(NULL, w);
    return TRUE;
  }

  /* Terminal input — only forward printable/control chars */
  if (keyval < 128 && !ctrl_held) {
    sim_term_write(&w->sim, (char)keyval);
    return TRUE;
  }
  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)
    sim_term_write(&w->sim, '\r');
  else if (keyval == GDK_KEY_BackSpace)
    sim_term_write(&w->sim, '\b');
  else if (keyval == GDK_KEY_Escape)
    sim_term_write(&w->sim, 27);
  else if (ctrl_held && keyval >= GDK_KEY_a && keyval <= GDK_KEY_z)
    sim_term_write(&w->sim, (char)(keyval - GDK_KEY_a + 1)); /* Ctrl+A=1 etc */
  else
    return FALSE;
  return TRUE;
}

/* forward decl */
static void sim_push_sentinel(sim_win_t *w);

/* ---------------------------------------------------------------------------
 * Behaviour dropdown callbacks
 * ------------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Profile helpers
 */

/* Rebuild the profile dropdown from available profiles */
static void profile_dd_rebuild(sim_win_t *w) {
  if (!w->dd_profile)
    return;

  simprofile_list_t *list =
      simprofile_list_load(w->project_dir[0] ? w->project_dir : NULL);

  /* Build string array for dropdown */
  int n = list ? list->count : 0;
  const char **names = g_new0(const char *, n + 2);
  names[0] = "—"; /* placeholder, no action on select */
  for (int i = 0; i < n; i++)
    names[i + 1] = list->items[i]->name;
  names[n + 1] = NULL;

  /* Block signal while rebuilding to prevent spurious profile_apply calls */
  gulong hid =
      g_signal_handler_find(w->dd_profile, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                            G_CALLBACK(on_profile_changed), NULL);
  if (hid)
    g_signal_handler_block(w->dd_profile, hid);

  GtkStringList *sl = gtk_string_list_new(names);
  gtk_drop_down_set_model(GTK_DROP_DOWN(w->dd_profile), G_LIST_MODEL(sl));

  /* Select current profile by name */
  guint sel = 0;
  for (int i = 0; i < n; i++) {
    if (strcmp(list->items[i]->name, w->profile.name) == 0) {
      sel = (guint)(i + 1);
      break;
    }
  }
  gtk_drop_down_set_selected(GTK_DROP_DOWN(w->dd_profile), sel);

  if (hid)
    g_signal_handler_unblock(w->dd_profile, hid);

  g_free(names);
  simprofile_list_free(list);
}

/* Apply a new profile — warns if running */
static void profile_apply(sim_win_t *w, simprofile_t *prof) {
  /* Warn if running — pause immediately, user can reset if they want */
  if (!w->sim.paused) {
    sim_pause(&w->sim);
    gtk_widget_set_sensitive(w->btn_run, TRUE);
    gtk_widget_set_sensitive(w->btn_stop, FALSE);
    gtk_widget_set_sensitive(w->btn_step, TRUE);
  }

  /* Stop ACIA before reinit */
  acia_tcp_close(&w->sim.acia);

  /* Rebuild memory map */
  mem_clear_io(&w->sim.mem);
  sim_apply_profile(w, prof, &w->cfg);

  /* Reset CPU */
  pthread_mutex_lock(&w->sim.state_lock);
  cpu_reset(&w->sim.cpu, mem_read, &w->sim.mem);
  w->sim.paused = true;
  w->sim.do_step = false;
  pthread_cond_signal(&w->sim.step_cond);
  pthread_mutex_unlock(&w->sim.state_lock);
  sim_push_sentinel(w);

  /* Clear terminal */
  gtk_text_buffer_set_text(w->tbuf, "", 0);
  w->term_col = 0;

  /* Update CPU dropdown to match profile (block signal to avoid recursion) */
  if (w->dd_cpu) {
    guint cpu_sel = 0;
    if (prof->cpu_type == PROFILE_CPU_65816)
      cpu_sel = 2;
    else if (prof->cpu_type == PROFILE_CPU_65C02)
      cpu_sel = 1;
    gulong hid = g_signal_handler_find(w->dd_cpu, G_SIGNAL_MATCH_FUNC, 0, 0,
                                       NULL, G_CALLBACK(on_cpu_changed), NULL);
    if (hid)
      g_signal_handler_block(w->dd_cpu, hid);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(w->dd_cpu), cpu_sel);
    if (hid)
      g_signal_handler_unblock(w->dd_cpu, hid);
  }

  /* Reset button states */
  gtk_widget_set_sensitive(w->btn_run, TRUE);
  gtk_widget_set_sensitive(w->btn_stop, FALSE);
  gtk_widget_set_sensitive(w->btn_step, TRUE);
  sim_win_update(w);

  /* Persist the last-used profile filepath to sim.cfg
   * but NOT if it's a temporary embedded profile (those are deleted after load) */
  if (prof->filepath[0] &&
      !strstr(prof->filepath, "_embedded.simprofile")) {
    snprintf(w->cfg.last_profile, sizeof(w->cfg.last_profile),
             "%s", prof->filepath);
    char cfgpath[1024];
    snprintf(cfgpath, sizeof(cfgpath), "%s/sim.cfg", g_get_home_dir());
    simcfg_save(&w->cfg, cfgpath);
  }
}

static void on_profile_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud) {
  (void)ps;
  sim_win_t *w = (sim_win_t *)ud;
  guint sel = gtk_drop_down_get_selected(dd);
  if (sel == 0)
    return; /* placeholder — no action */

  simprofile_list_t *list =
      simprofile_list_load(w->project_dir[0] ? w->project_dir : NULL);
  if (!list)
    return;

  int idx = (int)sel - 1;
  if (idx >= 0 && idx < list->count) {
    profile_apply(w, list->items[idx]);
  }
  simprofile_list_free(list);
}

/* Show the profile editor dialog */
static void show_profile_editor(sim_win_t *w, simprofile_t *prof, bool is_new);

static void on_profile_edit(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  show_profile_editor(w, &w->profile, false);
}

static void on_profile_new(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  simprofile_t *prof = calloc(1, sizeof(*prof));
  simprofile_defaults(prof);
  show_profile_editor(w, prof, true);
  free(prof);
}

static void on_profile_delete_confirmed(GtkButton *b, gpointer ud) {
  (void)b;
  GtkWidget *dlg = GTK_WIDGET(ud);
  sim_win_t *w = (sim_win_t *)g_object_get_data(G_OBJECT(dlg), "del_win");
  const char *path = (const char *)g_object_get_data(G_OBJECT(dlg), "del_path");
  if (path && w) {
    remove(path);
    /* Clear current profile and refresh dropdown */
    simprofile_defaults(&w->profile);
    w->profile.filepath[0] = '\0';
    profile_apply(w, &w->profile);
    profile_dd_rebuild(w);
  }
  gtk_window_destroy(GTK_WINDOW(dlg));
}

static void on_profile_delete(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;

  /* Don't delete if it's the only profile or has no filepath */
  if (!w->profile.filepath[0]) {
    return;
  }

  /* Build confirmation dialog */
  GtkWidget *dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Delete Profile");
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w->window));
  gtk_window_set_default_size(GTK_WINDOW(dlg), 360, -1);
  if (gtk_widget_has_css_class(w->window, "dark"))
    gtk_widget_add_css_class(dlg, "dark");

  GtkWidget *hdr = gtk_header_bar_new();
  gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hdr), FALSE);
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hdr),
      gtk_label_new("Delete Profile"));
  gtk_window_set_titlebar(GTK_WINDOW(dlg), hdr);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(vbox, 16);
  gtk_widget_set_margin_end(vbox, 16);
  gtk_widget_set_margin_top(vbox, 16);
  gtk_widget_set_margin_bottom(vbox, 16);

  char msg[512];
  snprintf(msg, sizeof(msg),
           "Delete profile \"%s\"?\n\nThis will permanently remove the file:\n%s",
           w->profile.name[0] ? w->profile.name : "(unnamed)",
           w->profile.filepath);
  GtkWidget *lbl = gtk_label_new(msg);
  gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
  gtk_box_append(GTK_BOX(vbox), lbl);

  GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_row, GTK_ALIGN_END);
  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *btn_del    = gtk_button_new_with_label("Delete");
  gtk_widget_add_css_class(btn_del, "destructive-action");
  gtk_box_append(GTK_BOX(btn_row), btn_cancel);
  gtk_box_append(GTK_BOX(btn_row), btn_del);
  gtk_box_append(GTK_BOX(vbox), btn_row);
  gtk_window_set_child(GTK_WINDOW(dlg), vbox);

  /* Store filepath in dialog for use by delete callback */
  char *fpath = strdup(w->profile.filepath);
  g_object_set_data_full(G_OBJECT(dlg), "del_path", fpath, g_free);
  g_object_set_data(G_OBJECT(dlg), "del_win", w);

  g_signal_connect_swapped(btn_cancel, "clicked",
      G_CALLBACK(gtk_window_destroy), dlg);

  /* Delete callback */
  g_signal_connect(btn_del, "clicked",
      G_CALLBACK(on_profile_delete_confirmed), dlg);

  gtk_window_present(GTK_WINDOW(dlg));
}

static void on_stp_db_toggled(GtkCheckButton *btn, gpointer ud) {
  sim_win_t *w = (sim_win_t *)ud;
  bool on = gtk_check_button_get_active(btn);
  w->cfg.stp_on_db = on;
  w->sim.cpu.stp_on_db = on;
  /* Persist to sim.cfg */
  char cfgpath[1024];
  snprintf(cfgpath, sizeof(cfgpath), "%s/sim.cfg", g_get_home_dir());
  simcfg_save(&w->cfg, cfgpath);
}

static void on_speed_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud) {
  (void)ps;
  sim_win_t *w = (sim_win_t *)ud;
  guint sel = gtk_drop_down_get_selected(dd);
  if (sel > 12)
    return;
  pthread_mutex_lock(&w->sim.state_lock);
  w->sim.speed = (sim_speed_t)sel;
  pthread_mutex_unlock(&w->sim.state_lock);
  /* Persist speed to sim.cfg */
  w->cfg.speed = (sim_speed_t)sel;
  {
    char cfgpath[1024];
    snprintf(cfgpath, sizeof(cfgpath), "%s/sim.cfg", g_get_home_dir());
    simcfg_save(&w->cfg, cfgpath);
  }
  /* Update ACIA cpu_hz for baud timing */
  long hz_table[] = {1000000, 512000, 128000, 32000, 8000, 2000,   512,
                     128,     32,     8,      2,     1,    2000000};
  acia_set_cpu_hz(&w->sim.acia, hz_table[sel]);
  /* Update speed info label — warn when disasm/panel updates are reduced */
  /* Update LCD timing when speed changes */
  {
    long hz_table2[] = {1000000, 512000, 128000, 32000, 8000, 2000, 512,
                        128, 32, 8, 2, 1, 2000000};
    lcd_ui_set_cpu_hz(w, (uint64_t)hz_table2[sel]);
    sd_ui_set_cpu_hz(w,  (uint64_t)hz_table2[sel]);
  }
  if (w->lbl_speed_info) {
    const char *spd_names[] = {"2MHz","1MHz","512KHz","128KHz","32KHz","8KHz",
                                "2KHz","512Hz","128Hz","32Hz","8Hz",
                                "2Hz","1Hz","MAX"};
    char info[64];
    bool is_fast = (sel < (guint)SIM_SPEED_2KHZ || sel == (guint)SIM_SPEED_MAX);
    if (is_fast)
      snprintf(info, sizeof(info), "%s  \u26a0 display reduced", spd_names[sel]);
    else
      snprintf(info, sizeof(info), "%s", spd_names[sel]);
    gtk_label_set_text(GTK_LABEL(w->lbl_speed_info), info);
    /* Swap CSS class between normal and warning */
    if (is_fast) {
      gtk_widget_remove_css_class(w->lbl_speed_info, "reg-name");
      gtk_widget_add_css_class(w->lbl_speed_info, "speed-warn");
    } else {
      gtk_widget_remove_css_class(w->lbl_speed_info, "speed-warn");
      gtk_widget_add_css_class(w->lbl_speed_info, "reg-name");
    }
  }
}

static void on_cpu_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud) {
  (void)ps;
  sim_win_t *w = (sim_win_t *)ud;
  guint sel = gtk_drop_down_get_selected(dd);
  cpu_type_t types[] = {CPU_6502, CPU_65C02, CPU_65816};
  if (sel >= 3)
    return;
  /* Pause, change CPU type, full re-init then reset — stay paused */
  bool was_paused = w->sim.paused;
  sim_pause(&w->sim);
  pthread_mutex_lock(&w->sim.state_lock);
  cpu_init(&w->sim.cpu, types[sel]);
  pthread_mutex_unlock(&w->sim.state_lock);
  cpu_reset(&w->sim.cpu, mem_read, &w->sim.mem);
  sim_push_sentinel(w);
  /* Only resume if it was already running before the change */
  if (!was_paused)
    sim_resume(&w->sim);
}

static void on_rts_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud) {
  (void)ps;
  sim_win_t *w = (sim_win_t *)ud;
  w->sim.on_rts = (int)gtk_drop_down_get_selected(dd);
  /* If switching to Restart mode, push sentinel immediately */
  if (w->sim.on_rts == 0)
    sim_push_sentinel(w);
}
static void on_brk_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud) {
  (void)ps;
  sim_win_t *w = (sim_win_t *)ud;
  w->sim.on_brk = (int)gtk_drop_down_get_selected(dd);
}

/* ---------------------------------------------------------------------------
 * Toolbar callbacks
 * ------------------------------------------------------------------------- */
static void on_run(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  sim_resume(&w->sim);
  gtk_widget_set_sensitive(w->btn_run, FALSE);
  gtk_widget_set_sensitive(w->btn_stop, TRUE);
  gtk_widget_set_sensitive(w->btn_step, FALSE);
  /* Ensure the UI polling timer is alive — it can be missing if it was
   * removed while sim.running was false (e.g. before first start). */
  if (!w->update_timer)
    w->update_timer = g_timeout_add(UPDATE_INTERVAL_MS, sim_win_update, w);
}
static void on_stop(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  sim_pause(&w->sim);
  gtk_widget_set_sensitive(w->btn_run, TRUE);
  gtk_widget_set_sensitive(w->btn_stop, FALSE);
  gtk_widget_set_sensitive(w->btn_step, TRUE);
}
static void on_step(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  sim_step(&w->sim);
}
static void sim_push_sentinel(sim_win_t *w) {
  /* Push reset_vector-1 so top-level RTS returns to reset vector.
   * For 65816, skip this — the 65816 uses a proper 16-bit stack and
   * the test code manages its own JSR/RTS correctly. */
  if (w->sim.on_rts != 0)
    return;
  if (w->sim.cpu.type == CPU_65816)
    return;

  uint16_t rv = (uint16_t)(mem_read(0xFFFC, &w->sim.mem) |
                           (mem_read(0xFFFD, &w->sim.mem) << 8));
  uint16_t sentinel = rv - 1;
  /* 6502/65C02: stack is always page 1, SP is 8-bit */
  mem_write(0x0100 | (w->sim.cpu.SP & 0xFF), (sentinel >> 8) & 0xFF,
            &w->sim.mem);
  w->sim.cpu.SP = (w->sim.cpu.SP - 1) & 0xFF;
  mem_write(0x0100 | (w->sim.cpu.SP & 0xFF), sentinel & 0xFF, &w->sim.mem);
  w->sim.cpu.SP = (w->sim.cpu.SP - 1) & 0xFF;
}

static void on_reload(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  if (!w->app_state)
    return;

    /* Re-read mem_image from AppState and reload into simulator RAM */
#ifdef ED_BUILD_SIM_RELOAD
  /* Implemented in main.c via reload callback */
  sim_reload_fn fn =
      (sim_reload_fn)g_object_get_data(G_OBJECT(w->window), "reload_fn");
  if (fn)
    fn(w);
#endif
  /* Direct approach: write new temp file and reload memory */
  /* We store a GBytes* snapshot updated by main.c on each assemble */
  GBytes *snap =
      (GBytes *)g_object_get_data(G_OBJECT(w->window), "mem_snapshot");
  if (snap) {
    gsize sz;
    const uint8_t *data = g_bytes_get_data(snap, &sz);
    if (data && sz == 65536) {
      pthread_mutex_lock(&w->sim.state_lock);
      memcpy(w->sim.mem.ram, data, 65536);
      pthread_mutex_unlock(&w->sim.state_lock);
      /* Refresh ROM image snapshot before reset so rom_range_rd
       * returns the new binary's ROM content, not the old one. */
      sim_take_rom_snapshot(w);
      pthread_mutex_lock(&w->sim.state_lock);
      cpu_reset(&w->sim.cpu, mem_read, &w->sim.mem);
      w->sim.paused = true;
      w->sim.do_step = false;
      pthread_cond_signal(&w->sim.step_cond);
      pthread_mutex_unlock(&w->sim.state_lock);
      sim_push_sentinel(w);
      /* Clear terminal */
      gtk_text_buffer_set_text(w->tbuf, "", 0);
      w->term_col = 0;
      /* Reset buttons */
      gtk_widget_set_sensitive(w->btn_run, TRUE);
      gtk_widget_set_sensitive(w->btn_stop, FALSE);
      gtk_widget_set_sensitive(w->btn_step, TRUE);
      sim_win_update(w);
    }
  }
}

static void on_reset(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;

  /* First ensure simio_rd exits if blocking on input */
  w->sim.paused = true;
  pthread_cond_broadcast(&w->sim.step_cond);

  /* Small yield so simio_rd can exit its loop */
  struct timespec ts = {0, 2000000}; /* 2ms */
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&w->sim.state_lock);
  cpu_reset(&w->sim.cpu, mem_read, &w->sim.mem);
  w->sim.paused = true;
  w->sim.do_step = false;
  w->sim.do_reset = false;
  simprofile_reset_locks(&w->profile);   /* clear R/W and lock bits */
  lcd_ui_reset(w);                       /* reset LCD to power-on state */
  sd_ui_reset(w);                        /* reset SD card state machine */
  pthread_cond_broadcast(&w->sim.step_cond);
  pthread_mutex_unlock(&w->sim.state_lock);

  /* Push sentinel outside lock (only writes to memory/SP) */
  sim_push_sentinel(w);

  /* Drain and discard any pending terminal output */
  char _drain[256];
  while (sim_term_read(&w->sim, _drain, sizeof(_drain)) > 0) {
  }

  /* Clear terminal display */
  gtk_text_buffer_set_text(w->tbuf, "", 0);
  w->term_col = 0;

  /* Reset button states */
  gtk_widget_set_sensitive(w->btn_run, TRUE);
  gtk_widget_set_sensitive(w->btn_stop, FALSE);
  gtk_widget_set_sensitive(w->btn_step, TRUE);

  /* Update register display */
  sim_win_update(w);
}

/* ---------------------------------------------------------------------------
 * Breakpoint UI
 * ------------------------------------------------------------------------- */
static void on_bp_add(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  const char *txt = gtk_editable_get_text(GTK_EDITABLE(w->bp_entry));
  if (!txt || !*txt)
    return;
  uint32_t addr = (uint32_t)strtoul(txt, NULL, 16);
  sim_add_breakpoint(&w->sim, addr);

  /* Add to listbox */
  char label[16];
  snprintf(label, sizeof(label), "$%04X", addr);
  GtkWidget *row = gtk_label_new(label);
  gtk_widget_add_css_class(row, "reg-val");
  GtkWidget *lb_row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(lb_row), row);
  gtk_list_box_append(GTK_LIST_BOX(w->bp_list), lb_row);
  gtk_editable_set_text(GTK_EDITABLE(w->bp_entry), "");
}
static void on_bp_clear(GtkButton *b, gpointer ud) {
  (void)b;
  sim_win_t *w = (sim_win_t *)ud;
  sim_clear_breakpoints(&w->sim);
  /* Clear listbox */
  GtkWidget *child = gtk_widget_get_first_child(w->bp_list);
  while (child) {
    GtkWidget *next = gtk_widget_get_next_sibling(child);
    gtk_list_box_remove(GTK_LIST_BOX(w->bp_list), child);
    child = next;
  }
}

/* ---------------------------------------------------------------------------
 * CA1/CB1 handshake line toggle callbacks
 * ------------------------------------------------------------------------- */
typedef struct {
  sim_win_t *w;
  int via_idx;
  int line;
} ca_cb_t;

static void on_ca1_toggle(GtkButton *btn, gpointer ud) {
  (void)btn;
  ca_cb_t *cc = (ca_cb_t *)ud;
  sim_win_t *w = cc->w;
  int vi = cc->via_idx;
  via_ui_t *vu = &w->via_ui[vi];
  vu->ca1_state = !vu->ca1_state;
  pthread_mutex_lock(&w->sim.state_lock);
  via_set_ca1(&w->sim.via[vi], vu->ca1_state);
  pthread_mutex_unlock(&w->sim.state_lock);
  gtk_button_set_label(GTK_BUTTON(vu->ca1_btn),
                       vu->ca1_state ? "CA1: 1" : "CA1: 0");
}
static void on_cb1_toggle(GtkButton *btn, gpointer ud) {
  (void)btn;
  ca_cb_t *cc = (ca_cb_t *)ud;
  sim_win_t *w = cc->w;
  int vi = cc->via_idx;
  via_ui_t *vu = &w->via_ui[vi];
  vu->cb1_state = !vu->cb1_state;
  pthread_mutex_lock(&w->sim.state_lock);
  via_set_cb1(&w->sim.via[vi], vu->cb1_state);
  pthread_mutex_unlock(&w->sim.state_lock);
  gtk_button_set_label(GTK_BUTTON(vu->cb1_btn),
                       vu->cb1_state ? "CB1: 1" : "CB1: 0");
}

/* ---------------------------------------------------------------------------
 * VIA bit toggle button callback
 * ------------------------------------------------------------------------- */
typedef struct {
  sim_win_t *w;
  int via_idx;
  int port;
  int bit;
} bit_cb_t;

static void on_bit_toggle(GtkButton *btn, gpointer ud) {
  (void)btn;
  bit_cb_t *bc = (bit_cb_t *)ud;
  sim_win_t *w = bc->w;
  int vi = bc->via_idx, po = bc->port, bi = bc->bit;
  via_ui_t *vu = &w->via_ui[vi];
  vu->input[po] ^= (1 << bi);
  /* Lock while updating input pins so sim thread sees consistent state */
  pthread_mutex_lock(&w->sim.state_lock);
  if (po == 0)
    via_set_pa(&w->sim.via[vi], vu->input[0]);
  else
    via_set_pb(&w->sim.via[vi], vu->input[1]);
  pthread_mutex_unlock(&w->sim.state_lock);
}

/* ---------------------------------------------------------------------------
 * Build a VIA panel
 * ------------------------------------------------------------------------- */
static GtkWidget *build_via_panel(sim_win_t *w, int idx, const char *title) {
  via_ui_t *vu = &w->via_ui[idx];
  vu->via = &w->sim.via[idx];

  GtkWidget *frame = gtk_frame_new(title);
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(vbox, 4);
  gtk_widget_set_margin_end(vbox, 4);
  gtk_widget_set_margin_top(vbox, 4);
  gtk_widget_set_margin_bottom(vbox, 4);
  gtk_frame_set_child(GTK_FRAME(frame), vbox);

  /* Use a grid so header numbers and buttons share the same column widths */
  {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), FALSE);

    /* Column 0: port label (PA/PB) — row 0 is invisible spacer for header */
    GtkWidget *spacer = gtk_label_new("PA");
    gtk_widget_add_css_class(spacer, "reg-name");
    gtk_widget_set_opacity(spacer, 0.0);
    gtk_grid_attach(GTK_GRID(grid), spacer, 0, 0, 1, 1);

    /* Header row: bit numbers 7..0 in columns 1..8 */
    for (int b = 7; b >= 0; b--) {
      char txt[4];
      snprintf(txt, sizeof(txt), "%d", b);
      GtkWidget *lbl = gtk_label_new(txt);
      gtk_widget_add_css_class(lbl, "reg-name");
      gtk_widget_set_halign(lbl, GTK_ALIGN_CENTER);
      gtk_widget_set_hexpand(lbl, TRUE);
      gtk_grid_attach(GTK_GRID(grid), lbl, 8 - b, 0, 1, 1);
    }

    /* Port rows */
    const char *port_names[] = {"PA", "PB"};
    for (int p = 0; p < 2; p++) {
      GtkWidget *port_lbl = gtk_label_new(port_names[p]);
      gtk_widget_add_css_class(port_lbl, "reg-name");
      gtk_grid_attach(GTK_GRID(grid), port_lbl, 0, p + 1, 1, 1);

      for (int b = 7; b >= 0; b--) {
        char txt[4];
        snprintf(txt, sizeof(txt), "%d", b);
        GtkWidget *bit_lbl = gtk_label_new(txt);
        gtk_widget_add_css_class(bit_lbl, "bit-input");
        gtk_widget_set_size_request(bit_lbl, 22, 18);
        vu->port_lbl[p][b] = bit_lbl;

        GtkWidget *btn = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(btn), bit_lbl);
        gtk_widget_set_tooltip_text(btn, "Click to toggle input bit");
        gtk_widget_set_hexpand(btn, TRUE);
        vu->btn[p][b] = btn;

        bit_cb_t *bc = g_new0(bit_cb_t, 1);
        bc->w = w;
        bc->via_idx = idx;
        bc->port = p;
        bc->bit = b;
        g_signal_connect_data(btn, "clicked", G_CALLBACK(on_bit_toggle), bc,
                              (GClosureNotify)g_free, 0);

        gtk_grid_attach(GTK_GRID(grid), btn, 8 - b, p + 1, 1, 1);
      }
    }
    gtk_box_append(GTK_BOX(vbox), grid);
  }

  /* Timer / IFR status row */
  GtkWidget *status_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  vu->t1_lbl = gtk_label_new("T1:0000");
  vu->t2_lbl = gtk_label_new("T2:0000");
  vu->ifr_lbl = gtk_label_new("IFR:00");
  gtk_widget_add_css_class(vu->t1_lbl, "reg-val");
  gtk_widget_add_css_class(vu->t2_lbl, "reg-val");
  gtk_widget_add_css_class(vu->ifr_lbl, "reg-val");
  gtk_box_append(GTK_BOX(status_row), vu->t1_lbl);
  gtk_box_append(GTK_BOX(status_row), vu->t2_lbl);
  gtk_box_append(GTK_BOX(status_row), vu->ifr_lbl);
  gtk_box_append(GTK_BOX(vbox), status_row);

  /* CA1 / CB1 handshake buttons */
  GtkWidget *ca_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  vu->ca1_state = false;
  vu->ca1_btn = gtk_button_new_with_label("CA1: 0");
  gtk_widget_set_tooltip_text(
      vu->ca1_btn,
      "Toggle CA1 handshake input line\n"
      "Generates IRQ on active edge (PCR bit 0: 0=falling, 1=rising)");
  ca_cb_t *ca1_cb = g_new0(ca_cb_t, 1);
  ca1_cb->w = w;
  ca1_cb->via_idx = idx;
  ca1_cb->line = 0;
  g_signal_connect_data(vu->ca1_btn, "clicked", G_CALLBACK(on_ca1_toggle),
                        ca1_cb, (GClosureNotify)g_free, 0);

  vu->cb1_state = false;
  vu->cb1_btn = gtk_button_new_with_label("CB1: 0");
  gtk_widget_set_tooltip_text(
      vu->cb1_btn,
      "Toggle CB1 handshake input line\n"
      "Generates IRQ on active edge (PCR bit 4: 0=falling, 1=rising)");
  ca_cb_t *cb1_cb = g_new0(ca_cb_t, 1);
  cb1_cb->w = w;
  cb1_cb->via_idx = idx;
  cb1_cb->line = 1;
  g_signal_connect_data(vu->cb1_btn, "clicked", G_CALLBACK(on_cb1_toggle),
                        cb1_cb, (GClosureNotify)g_free, 0);

  gtk_box_append(GTK_BOX(ca_row), vu->ca1_btn);
  gtk_box_append(GTK_BOX(ca_row), vu->cb1_btn);
  gtk_box_append(GTK_BOX(vbox), ca_row);

  return frame;
}

/* ---------------------------------------------------------------------------
 * Build ACIA panel
 * ------------------------------------------------------------------------- */
static GtkWidget *build_acia_panel(sim_win_t *w) {
  GtkWidget *frame = gtk_frame_new("ACIA");
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(vbox, 4);
  gtk_widget_set_margin_end(vbox, 4);
  gtk_widget_set_margin_top(vbox, 4);
  gtk_widget_set_margin_bottom(vbox, 4);
  gtk_frame_set_child(GTK_FRAME(frame), vbox);

  w->lbl_acia_settings = gtk_label_new("9600 8N1");
  gtk_widget_add_css_class(w->lbl_acia_settings, "reg-val");

  w->lbl_acia_cpb = gtk_label_new("cpb: ---");
  gtk_widget_add_css_class(w->lbl_acia_cpb, "reg-name");
  gtk_widget_set_tooltip_text(
      w->lbl_acia_cpb, "Cycles per bit at current baud rate and CPU clock");

  char port_str[32];
  snprintf(port_str, sizeof(port_str), "TCP :%d", w->cfg.acia_tcp_port);
  GtkWidget *lbl_port = gtk_label_new(port_str);
  gtk_widget_add_css_class(lbl_port, "reg-name");

  w->lbl_acia_conn = gtk_label_new("Disconnected");
  gtk_widget_add_css_class(w->lbl_acia_conn, "acia-disc");

  gtk_box_append(GTK_BOX(vbox), w->lbl_acia_settings);
  gtk_box_append(GTK_BOX(vbox), w->lbl_acia_cpb);
  gtk_box_append(GTK_BOX(vbox), lbl_port);
  gtk_box_append(GTK_BOX(vbox), w->lbl_acia_conn);

  return frame;
}

/* ---------------------------------------------------------------------------
 * Build register panel (right side)
 * ------------------------------------------------------------------------- */
static GtkWidget *build_reg_panel(sim_win_t *w) {
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(vbox, 6);
  gtk_widget_set_margin_end(vbox, 6);
  gtk_widget_set_margin_top(vbox, 6);
  gtk_widget_set_size_request(vbox, 200, -1);

  /* PC — large */
  w->lbl_pc = gtk_label_new("PC: $FFFC");
  gtk_widget_add_css_class(w->lbl_pc, "pc-label");
  gtk_widget_set_halign(w->lbl_pc, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(vbox), w->lbl_pc);

  /* A / X / Y / SP in a grid */
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);

  struct {
    const char *name;
    GtkWidget **lbl;
  } regs[] = {
      {"A", &w->lbl_a},
      {"X", &w->lbl_x},
      {"Y", &w->lbl_y},
      {"SP", &w->lbl_sp},
  };
  for (int i = 0; i < 4; i++) {
    GtkWidget *nm = gtk_label_new(regs[i].name);
    gtk_widget_add_css_class(nm, "reg-name");
    gtk_widget_set_halign(nm, GTK_ALIGN_END);
    *regs[i].lbl = gtk_label_new("$00");
    gtk_widget_add_css_class(*regs[i].lbl, "reg-val");
    gtk_widget_set_halign(*regs[i].lbl, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), nm, 0, i, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), *regs[i].lbl, 1, i, 1, 1);
  }
  gtk_box_append(GTK_BOX(vbox), grid);

  /* Flags row */
  GtkWidget *flags_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 1);
  const char *flag_names = "NV-BDIZC";
  for (int i = 0; i < 8; i++) {
    char fn[3] = {flag_names[i], 0};
    w->lbl_flags[i] = gtk_label_new(fn);
    gtk_widget_add_css_class(w->lbl_flags[i], "flag-clr");
    gtk_box_append(GTK_BOX(flags_box), w->lbl_flags[i]);
  }
  gtk_box_append(GTK_BOX(vbox), flags_box);

  /* P register hex */
  w->lbl_p = gtk_label_new("P: $00");
  gtk_widget_add_css_class(w->lbl_p, "reg-name");
  gtk_widget_set_halign(w->lbl_p, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(vbox), w->lbl_p);

  /* Cycle counter */
  w->lbl_cycles = gtk_label_new("Cycles: 0");
  gtk_widget_add_css_class(w->lbl_cycles, "reg-name");
  gtk_widget_set_halign(w->lbl_cycles, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(vbox), w->lbl_cycles);

  /* Bank register label — hidden unless banking enabled */
  w->lbl_bank = gtk_label_new("Bank: $00");
  gtk_widget_add_css_class(w->lbl_bank, "reg-name");
  gtk_widget_set_halign(w->lbl_bank, GTK_ALIGN_START);
  gtk_widget_set_visible(w->lbl_bank, FALSE);
  gtk_box_append(GTK_BOX(vbox), w->lbl_bank);

  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* Breakpoints */
  GtkWidget *bp_title = gtk_label_new("BREAKPOINTS");
  gtk_widget_add_css_class(bp_title, "section-title");
  gtk_widget_set_halign(bp_title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(vbox), bp_title);

  GtkWidget *bp_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  w->bp_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(w->bp_entry), "FFFC");
  gtk_entry_set_max_length(GTK_ENTRY(w->bp_entry), 6);
  gtk_widget_set_hexpand(w->bp_entry, TRUE);
  GtkWidget *bp_add = gtk_button_new_with_label("Add");
  GtkWidget *bp_clr = gtk_button_new_with_label("Clear");
  g_signal_connect(bp_add, "clicked", G_CALLBACK(on_bp_add), w);
  g_signal_connect(bp_clr, "clicked", G_CALLBACK(on_bp_clear), w);
  gtk_box_append(GTK_BOX(bp_row), w->bp_entry);
  gtk_box_append(GTK_BOX(bp_row), bp_add);
  gtk_box_append(GTK_BOX(bp_row), bp_clr);
  gtk_box_append(GTK_BOX(vbox), bp_row);

  w->bp_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(w->bp_list), GTK_SELECTION_NONE);
  gtk_box_append(GTK_BOX(vbox), w->bp_list);

  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* Disassembly */
  GtkWidget *dis_title = gtk_label_new("DISASSEMBLY");
  gtk_widget_add_css_class(dis_title, "section-title");
  gtk_widget_set_halign(dis_title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(vbox), dis_title);

  GtkWidget *dis_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  for (int i = 0; i < DISASM_LINES; i++) {
    w->disasm_labels[i] = gtk_label_new("");
    gtk_widget_add_css_class(w->disasm_labels[i], "reg-val");
    gtk_widget_set_halign(w->disasm_labels[i], GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(dis_box), w->disasm_labels[i]);
  }
  GtkWidget *dis_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dis_scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dis_scroll), dis_box);
  gtk_widget_set_vexpand(dis_scroll, TRUE);
  gtk_box_append(GTK_BOX(vbox), dis_scroll);

  return vbox;
}

/* ---------------------------------------------------------------------------
 * CSS installation
 * ------------------------------------------------------------------------- */
static void install_css(void) {
  static bool done = false;
  if (done)
    return;
  done = true;
  GtkCssProvider *p = gtk_css_provider_new();
  gtk_css_provider_load_from_string(p, CSS_SIM);
  gtk_style_context_add_provider_for_display(
      gdk_display_get_default(), GTK_STYLE_PROVIDER(p),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(p);
}

/* ---------------------------------------------------------------------------
 * UI update (called from GLib timer, main thread)
 * ------------------------------------------------------------------------- */
gboolean sim_win_update(gpointer ud) {
  sim_win_t *w = (sim_win_t *)ud;

  /* --- Drain terminal output (always, regardless of run state) --- */
  char tbuf[256];
  int n;
  while ((n = sim_term_read(&w->sim, tbuf, sizeof(tbuf) - 1)) > 0) {
    tbuf[n] = 0;
    terminal_append(w, tbuf, n);
  }

  /* When running at high speed, skip the expensive disassembler and VIA
   * panel updates — they cause unnecessary lock contention and GTK work
   * that can noticeably reduce throughput at MAX speed.
   * We still update registers so the display isn't completely frozen.
   * At slow speeds (< 10KHz) or when paused, always do the full update. */
  /* full_update: do expensive disasm+VIA work.
   * When running at >= 8KHz (speeds 0-4), only update fully when paused
   * or every 5th timer tick at MAX speed — reduces GTK/lock overhead. */
  bool full_update;
  if (w->sim.paused) {
    full_update = true;
  } else if (w->sim.speed >= SIM_SPEED_2KHZ) {
    /* Slow speeds (2KHz and below) — always full update */
    full_update = true;
  } else if (w->sim.speed == SIM_SPEED_MAX) {
    /* MAX speed — full update only every 5th call (~2/sec) */
    static int max_skip = 0;
    full_update = (++max_skip >= 5);
    if (full_update) max_skip = 0;
  } else {
    /* 1MHz-8KHz running — skip disasm+VIA normally, but force full update
     * if any VIA output has changed so the display stays current */
    full_update = false;
    for (int vi = 0; vi < 2 && !full_update; vi++) {
      if (!w->sim.via_enable[vi]) continue;
      via_t *v = &w->sim.via[vi];
      uint8_t pa = via_get_pa_out(v);
      uint8_t pb = via_get_pb_out(v);
      if (pa != w->via_snap[vi].pa_out || pb != w->via_snap[vi].pb_out ||
          v->DDRA != w->via_snap[vi].ddra || v->DDRB != w->via_snap[vi].ddrb ||
          v->T1C  != w->via_snap[vi].t1c  || v->T2C  != w->via_snap[vi].t2c  ||
          v->IFR  != w->via_snap[vi].ifr)
        full_update = true;
    }
  }

  /* VIAs and ACIA are ticked per CPU cycle in the sim thread.
   * Here we just read their state for display. */

  /* --- Registers --- */
  cpu_state_t cpu;
  sim_get_cpu(&w->sim, &cpu);

  char buf[64];

  /* PC — show bank for 65816 */
  if (cpu.type == CPU_65816)
    snprintf(buf, sizeof(buf), "PC: $%02X:%04X", cpu.PBR,
             (unsigned)(cpu.PC & 0xFFFF));
  else
    snprintf(buf, sizeof(buf), "PC: $%04X", (unsigned)(cpu.PC & 0xFFFF));
  gtk_label_set_text(GTK_LABEL(w->lbl_pc), buf);

  /* A/X/Y — 16-bit in native 65816 mode */
  bool wide_m = (cpu.type == CPU_65816 && !cpu.emulation && !(cpu.P & 0x20));
  bool wide_x = (cpu.type == CPU_65816 && !cpu.emulation && !(cpu.P & 0x10));
  snprintf(buf, sizeof(buf), wide_m ? "$%04X" : "$%02X",
           (unsigned)(cpu.A & (wide_m ? 0xFFFF : 0xFF)));
  gtk_label_set_text(GTK_LABEL(w->lbl_a), buf);
  snprintf(buf, sizeof(buf), wide_x ? "$%04X" : "$%02X",
           (unsigned)(cpu.X & (wide_x ? 0xFFFF : 0xFF)));
  gtk_label_set_text(GTK_LABEL(w->lbl_x), buf);
  snprintf(buf, sizeof(buf), wide_x ? "$%04X" : "$%02X",
           (unsigned)(cpu.Y & (wide_x ? 0xFFFF : 0xFF)));
  gtk_label_set_text(GTK_LABEL(w->lbl_y), buf);

  /* SP — 16-bit in native 65816 */
  if (cpu.type == CPU_65816 && !cpu.emulation)
    snprintf(buf, sizeof(buf), "$%04X", (unsigned)(cpu.SP & 0xFFFF));
  else
    snprintf(buf, sizeof(buf), "$%02X", (unsigned)(cpu.SP & 0xFF));
  gtk_label_set_text(GTK_LABEL(w->lbl_sp), buf);

  snprintf(buf, sizeof(buf), "P: $%02X", cpu.P);
  gtk_label_set_text(GTK_LABEL(w->lbl_p), buf);
  snprintf(buf, sizeof(buf), "Cycles: %llu", (unsigned long long)cpu.cycles);
  gtk_label_set_text(GTK_LABEL(w->lbl_cycles), buf);

  /* Bank register — only shown when banking enabled */
  if (w->sim.bank_enable && w->lbl_bank) {
    snprintf(buf, sizeof(buf), "Bank: $%02X", w->sim.bank_reg);
    gtk_label_set_text(GTK_LABEL(w->lbl_bank), buf);
    gtk_widget_set_visible(w->lbl_bank, TRUE);
  } else if (w->lbl_bank) {
    gtk_widget_set_visible(w->lbl_bank, FALSE);
  }

  /* Flags — 65816 native uses M/X instead of B/D */
  const char *flag_names =
      (cpu.type == CPU_65816 && !cpu.emulation) ? "NVeMXDIZC" : "NV-BDIZC";
  (void)flag_names; /* used below */
  const char *fn8 =
      (cpu.type == CPU_65816 && !cpu.emulation) ? "NV-MXDIZC" : "NV-BDIZC";
  for (int i = 0; i < 8; i++) {
    char fn[3] = {fn8[i], 0};
    gtk_label_set_text(GTK_LABEL(w->lbl_flags[i]), fn);
    bool set = (cpu.P & (0x80 >> i)) != 0;
    if (set) {
      gtk_widget_remove_css_class(w->lbl_flags[i], "flag-clr");
      gtk_widget_add_css_class(w->lbl_flags[i], "flag-set");
    } else {
      gtk_widget_remove_css_class(w->lbl_flags[i], "flag-set");
      gtk_widget_add_css_class(w->lbl_flags[i], "flag-clr");
    }
  }

  /* Disassembly — walk forward from PC (skip when running at speed) */
  if (full_update) {
  uint32_t pc = cpu.PC & 0xFFFF;
  /* Track M/X width state as we walk through instructions */
  bool dis_m8 = (cpu.type != CPU_65816) || cpu.emulation || (cpu.P & 0x20);
  bool dis_x8 = (cpu.type != CPU_65816) || cpu.emulation || (cpu.P & 0x10);
  for (int i = 0; i < DISASM_LINES; i++) {
    char dis[48];
    int bytes = cpu_disasm(cpu.type, pc, cpu.PBR, mem_read, &w->sim.mem, dis,
                           sizeof(dis), dis_m8, dis_x8);
    char line[64];
    snprintf(line, sizeof(line), "%c %04X  %s", i == 0 ? '>' : ' ', pc, dis);
    gtk_label_set_text(GTK_LABEL(w->disasm_labels[i]), line);

    /* Update m8/x8 if this instruction is REP or SEP (65816 only) */
    if (cpu.type == CPU_65816 && !cpu.emulation) {
      uint8_t op = mem_read(pc & 0xFFFF, &w->sim.mem);
      uint8_t arg = mem_read((pc + 1) & 0xFFFF, &w->sim.mem);
      if (op == 0xC2) { /* REP */
        if (arg & 0x20)
          dis_m8 = false;
        if (arg & 0x10)
          dis_x8 = false;
      } else if (op == 0xE2) { /* SEP */
        if (arg & 0x20)
          dis_m8 = true;
        if (arg & 0x10)
          dis_x8 = true;
      } else if (op == 0xFB) { /* XCE — entering emulation resets widths */
        if (cpu.P & 0x01) {    /* carry set = switching to emulation */
          dis_m8 = true;
          dis_x8 = true;
        }
      }
    }
    pc = (pc + bytes) & 0xFFFF;
  }

  } /* end full_update disassembly */

  /* --- VIA + ACIA panels (skip when running at speed) --- */
  if (full_update) {
  pthread_mutex_lock(&w->sim.state_lock);
  for (int vi = 0; vi < 2; vi++) {
    bool via_en = (vi == 0) ? (w->profile.via1.enabled || w->cfg.via1_enable)
                            : (w->profile.via2.enabled || w->cfg.via2_enable);
    if (!via_en)
      continue;
    via_ui_t *vu = &w->via_ui[vi];
    via_t *v = &w->sim.via[vi];

    for (int p = 0; p < 2; p++) {
      uint8_t ddr = (p == 0) ? v->DDRA : v->DDRB;
      uint8_t out = (p == 0) ? via_get_pa_out(v) : via_get_pb_out(v);
      uint8_t inp = (p == 0) ? v->PA_in : v->PB_in;

      for (int b = 0; b < 8; b++) {
        GtkWidget *lbl = vu->port_lbl[p][b];
        GtkWidget *btn = vu->btn[p][b];
        bool is_output = (ddr >> b) & 1;
        bool val = is_output ? ((out >> b) & 1) : ((inp >> b) & 1);
        char txt[3];
        snprintf(txt, sizeof(txt), "%d", val ? 1 : 0);
        gtk_label_set_text(GTK_LABEL(lbl), txt);

        gtk_widget_remove_css_class(lbl, "bit-high");
        gtk_widget_remove_css_class(lbl, "bit-low");
        gtk_widget_remove_css_class(lbl, "bit-input");

        if (is_output) {
          gtk_widget_add_css_class(lbl, val ? "bit-high" : "bit-low");
          gtk_widget_set_sensitive(btn, FALSE);
        } else {
          gtk_widget_add_css_class(lbl, "bit-input");
          gtk_widget_set_sensitive(btn, TRUE);
        }
      }
    }

    snprintf(buf, sizeof(buf), "T1:%04X", v->T1C);
    gtk_label_set_text(GTK_LABEL(vu->t1_lbl), buf);
    snprintf(buf, sizeof(buf), "T2:%04X", v->T2C);
    gtk_label_set_text(GTK_LABEL(vu->t2_lbl), buf);
    /* IFR: show set flags by name */
    char ifr_str[32] = "IFR:";
    if (v->IFR & 0x40)
      strcat(ifr_str, "T1 ");
    if (v->IFR & 0x20)
      strcat(ifr_str, "T2 ");
    if (v->IFR & 0x02)
      strcat(ifr_str, "CA1 ");
    if (v->IFR & 0x10)
      strcat(ifr_str, "CB1 ");
    if (v->IFR & 0x04)
      strcat(ifr_str, "SR ");
    if (!(v->IFR & 0x7F))
      strcat(ifr_str, "---");
    gtk_label_set_text(GTK_LABEL(vu->ifr_lbl), ifr_str);
  }

  /* --- ACIA panel --- */
  char acia_cfg[32];
  acia_format_settings(&w->sim.acia, acia_cfg, sizeof(acia_cfg));
  gtk_label_set_text(GTK_LABEL(w->lbl_acia_settings), acia_cfg);

  /* Cycles per bit + bytes/sec display */
  char cpb_str[32];
  acia_format_timing(&w->sim.acia, cpb_str, sizeof(cpb_str));
  gtk_label_set_text(GTK_LABEL(w->lbl_acia_cpb), cpb_str);
  pthread_mutex_unlock(&w->sim.state_lock);
  if (acia_connected(&w->sim.acia)) {
    gtk_label_set_text(GTK_LABEL(w->lbl_acia_conn), "Connected");
    gtk_widget_remove_css_class(w->lbl_acia_conn, "acia-disc");
    gtk_widget_add_css_class(w->lbl_acia_conn, "acia-conn");
  } else {
    gtk_label_set_text(GTK_LABEL(w->lbl_acia_conn), "Disconnected");
    gtk_widget_remove_css_class(w->lbl_acia_conn, "acia-conn");
    gtk_widget_add_css_class(w->lbl_acia_conn, "acia-disc");
  }
  /* Update VIA snapshots after drawing so next tick detects new changes */
  for (int vi = 0; vi < 2; vi++) {
    if (!w->sim.via_enable[vi]) continue;
    via_t *v = &w->sim.via[vi];
    w->via_snap[vi].pa_out = via_get_pa_out(v);
    w->via_snap[vi].pb_out = via_get_pb_out(v);
    w->via_snap[vi].ddra   = v->DDRA;
    w->via_snap[vi].ddrb   = v->DDRB;
    w->via_snap[vi].t1c    = v->T1C;
    w->via_snap[vi].t2c    = v->T2C;
    w->via_snap[vi].ifr    = v->IFR;
  }
  } /* end full_update VIA+ACIA */

  /* Update LCD panel */
  lcd_ui_update(w);
  sd_ui_update(w);


  /* Keep the timer alive as long as the window is open.
   * We only remove it in on_close() / sim_win_destroy().
   * Previously this returned G_SOURCE_REMOVE when !running, which
   * permanently killed the polling timer and left the UI frozen. */
  return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * Window close
 * ------------------------------------------------------------------------- */
static gboolean on_close(GtkWindow *win, gpointer ud) {
  sim_win_t *w = (sim_win_t *)ud;
  if (w->update_timer) {
    g_source_remove(w->update_timer);
    w->update_timer = 0;
  }
  /* Signal running=false so simio_rd blocking loop exits immediately */
  w->sim.running = false;
  pthread_cond_broadcast(&w->sim.step_cond);
  sim_stop(&w->sim);
  acia_tcp_close(&w->sim.acia);
  /* Clear active sim window reference on GtkApplication.
   * app_state is AppState* but we only need GApplication* here */
  if (w->app_state) {
    GApplication *app = g_application_get_default();
    if (app) {
      GtkWidget *cur =
          (GtkWidget *)g_object_get_data(G_OBJECT(app), "active_sim_window");
      if (cur == GTK_WIDGET(win))
        g_object_set_data(G_OBJECT(app), "active_sim_window", NULL);
    }
  }
  return FALSE; /* allow close */
}

/* -----------------------------------------------------------------------
 * Profile editor context — shared between editor dialog and its callbacks
 */
typedef struct {
  sim_win_t *win;
  simprofile_t *prof;
  bool is_new;
  /* identity */
  GtkWidget *e_name, *e_desc, *chk_global, *dd_cpu;
  /* ROM */
  GtkWidget *rom_enable[PROFILE_ROM_MAX];
  GtkWidget *rom_from[PROFILE_ROM_MAX];
  GtkWidget *rom_to[PROFILE_ROM_MAX];
  GtkWidget *rom_bank[PROFILE_ROM_MAX];
  GtkWidget *rom_rw[PROFILE_ROM_MAX];
  GtkWidget *rom_lock[PROFILE_ROM_MAX];
  /* rom ctrl */
  GtkWidget *chk_rctrl, *e_rctrl;
  /* IO */
  GtkWidget *chk_via1, *e_via1;
  GtkWidget *chk_via2, *e_via2;
  GtkWidget *chk_acia, *e_acia;
  GtkWidget *chk_tx, *e_tx;
  GtkWidget *chk_rx, *e_rx;
  GtkWidget *chk_bank, *e_bank;
  GtkWidget *e_bwin, *e_bwsz, *spn_banks;
  /* LCD */
  GtkWidget *chk_lcd;
  GtkWidget *dd_lcd_bus;    /* 4 or 8 bit */
  GtkWidget *spn_lcd_cols;
  GtkWidget *spn_lcd_rows;
  GtkWidget *chk_lcd_strict;
  GtkWidget *e_lcd_rs, *e_lcd_rw, *e_lcd_e;
  GtkWidget *e_lcd_d[8];
  GtkWidget *lcd_d03_row;   /* D0-D3 row — hidden in 4-bit mode */
  /* SD card */
  GtkWidget *chk_sd;
  GtkWidget *chk_sd_strict;
  GtkWidget *e_sd_sck, *e_sd_mosi, *e_sd_miso, *e_sd_cs;
  GtkWidget *e_sd_image;
  /* CPU options */
  GtkWidget *chk_stp_db;
} EdCtx;

static void on_lcd_bus_changed(GtkDropDown *dd, GParamSpec *ps, gpointer ud) {
  (void)ps;
  EdCtx *ctx = (EdCtx *)ud;
  if (!ctx || !ctx->lcd_d03_row) return;
  bool is4bit = gtk_drop_down_get_selected(dd) == 1;
  gtk_widget_set_visible(ctx->lcd_d03_row, !is4bit);
}

/* Read all editor fields into ctx->prof */
static void ed_read_fields(EdCtx *ctx) {
  simprofile_t *p = ctx->prof;
  const char *nm =
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_name)));
  snprintf(p->name, sizeof(p->name), "%s", nm);
  const char *ds =
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_desc)));
  snprintf(p->description, sizeof(p->description), "%s", ds);
  p->is_global = gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->chk_global));
  p->cpu_type = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dd_cpu));
  for (int i = 0; i < PROFILE_ROM_MAX; i++) {
    p->rom[i].enabled =
        gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->rom_enable[i]));
    p->rom[i].from =
        (uint16_t)strtoul(gtk_entry_buffer_get_text(gtk_entry_get_buffer(
                              GTK_ENTRY(ctx->rom_from[i]))),
                          NULL, 16);
    p->rom[i].to =
        (uint16_t)strtoul(gtk_entry_buffer_get_text(
                              gtk_entry_get_buffer(GTK_ENTRY(ctx->rom_to[i]))),
                          NULL, 16);
    {
      guint bsel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->rom_bank[i]));
      p->rom[i].bank = (bsel == 0 || bsel == GTK_INVALID_LIST_POSITION)
                       ? -1 : (int)(bsel - 1);
    }
    /* rw_bit and lock_bit: plain text entries, parse as int */
    {
      const char *rv = gtk_entry_buffer_get_text(
          gtk_entry_get_buffer(GTK_ENTRY(ctx->rom_rw[i])));
      p->rom[i].rw_bit = rv ? atoi(rv) : 0;
      if (p->rom[i].rw_bit < -1 || p->rom[i].rw_bit > 7)
        p->rom[i].rw_bit = 0;
    }
    {
      const char *lv = gtk_entry_buffer_get_text(
          gtk_entry_get_buffer(GTK_ENTRY(ctx->rom_lock[i])));
      p->rom[i].lock_bit = lv ? atoi(lv) : 0;
      if (p->rom[i].lock_bit < -1 || p->rom[i].lock_bit > 7)
        p->rom[i].lock_bit = 0;
    }
  }
  p->rom_ctrl.enabled =
      gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->chk_rctrl));
  p->rom_ctrl.addr = (uint16_t)strtoul(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_rctrl))),
      NULL, 16);
#define RD_IO(field, chk, eaddr)                                               \
  p->field.enabled = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk));       \
  p->field.addr = (uint16_t)strtoul(                                           \
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(eaddr))), NULL, \
      16);
  RD_IO(via1, ctx->chk_via1, ctx->e_via1)
  RD_IO(via2, ctx->chk_via2, ctx->e_via2)
  RD_IO(acia, ctx->chk_acia, ctx->e_acia)
  RD_IO(simio_tx, ctx->chk_tx, ctx->e_tx)
  RD_IO(simio_rx, ctx->chk_rx, ctx->e_rx)
  RD_IO(bank_reg, ctx->chk_bank, ctx->e_bank)
#undef RD_IO
  p->bank_window = (uint16_t)strtoul(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_bwin))),
      NULL, 16);
  p->bank_window_sz = (uint16_t)strtoul(
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_bwsz))),
      NULL, 16);
  p->bank_num =
      (uint8_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->spn_banks));

  /* LCD */
  p->lcd.enabled   = gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->chk_lcd));
  p->lcd.bus_width = (gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dd_lcd_bus)) == 1) ? 4 : 8;
  p->lcd.cols      = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->spn_lcd_cols));
  p->lcd.rows      = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ctx->spn_lcd_rows));
  p->lcd.strict    = gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->chk_lcd_strict));
  {
    const char *rs = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_lcd_rs)));
    const char *rw = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_lcd_rw)));
    const char *e  = gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->e_lcd_e)));
    lcd_pin_parse(rs, &p->lcd.rs);
    lcd_pin_parse(rw, &p->lcd.rw);
    lcd_pin_parse(e,  &p->lcd.e);
    if (p->lcd.bus_width == 4) {
      /* 4-bit mode: e_lcd_d[4-7] are the visible D4-D7 entries -> data[0-3] */
      for (int _i = 4; _i < 8; _i++)  {
        const char *d = gtk_entry_buffer_get_text(
            gtk_entry_get_buffer(GTK_ENTRY(ctx->e_lcd_d[_i])));
        lcd_pin_parse(d, &p->lcd.data[_i - 4]);
      }
    } else {
      /* 8-bit mode: e_lcd_d[0-7] = D0-D7 -> data[0-7] */
      for (int _i = 0; _i < 8; _i++) {
        const char *d = gtk_entry_buffer_get_text(
            gtk_entry_get_buffer(GTK_ENTRY(ctx->e_lcd_d[_i])));
        lcd_pin_parse(d, &p->lcd.data[_i]);
      }
    }
  }
  /* SD card */
  if (ctx->chk_sd)
    p->sd.enabled = gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->chk_sd));
  if (ctx->chk_sd_strict)
    p->sd.strict = gtk_check_button_get_active(GTK_CHECK_BUTTON(ctx->chk_sd_strict));
  if (ctx->e_sd_sck)
    spi_pin_parse(gtk_entry_buffer_get_text(
        gtk_entry_get_buffer(GTK_ENTRY(ctx->e_sd_sck))), &p->sd.sck);
  if (ctx->e_sd_mosi)
    spi_pin_parse(gtk_entry_buffer_get_text(
        gtk_entry_get_buffer(GTK_ENTRY(ctx->e_sd_mosi))), &p->sd.mosi);
  if (ctx->e_sd_miso)
    spi_pin_parse(gtk_entry_buffer_get_text(
        gtk_entry_get_buffer(GTK_ENTRY(ctx->e_sd_miso))), &p->sd.miso);
  if (ctx->e_sd_cs)
    spi_pin_parse(gtk_entry_buffer_get_text(
        gtk_entry_get_buffer(GTK_ENTRY(ctx->e_sd_cs))), &p->sd.cs);
  if (ctx->e_sd_image) {
    const char *img = gtk_entry_buffer_get_text(
        gtk_entry_get_buffer(GTK_ENTRY(ctx->e_sd_image)));
    if (img && img[0] && strcmp(img, "(none)") != 0)
      snprintf(p->sd.image_path, sizeof(p->sd.image_path), "%s", img);
    else
      p->sd.image_path[0] = '\0';
  }
}

static void on_profile_ed_save(GtkButton *b, gpointer ud) {
  (void)b;
  GtkWidget *dlg = GTK_WIDGET(ud);
  EdCtx *ctx = (EdCtx *)g_object_get_data(G_OBJECT(dlg), "edctx");
  ed_read_fields(ctx);

  /* Block save if ROM ranges overlap */
  const char *overlap = simprofile_check_overlaps(ctx->prof);
  if (overlap) {
    char msg[320];
    snprintf(msg, sizeof(msg),
             "ROM ranges overlap: %s\n\nOverlapping ranges may cause "
             "unpredictable reads. Please fix before saving.",
             overlap);
    GtkAlertDialog *warn = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_show(warn, GTK_WINDOW(dlg));
    g_object_unref(warn);
    return;
  }

  /* Save + apply, then close */
  if (profile_ed_save_and_apply(dlg, ctx))
    gtk_window_destroy(GTK_WINDOW(dlg));
}

/* Shared helper: save profile to disk and apply to simulator.
 * Returns true on success.  On failure shows an error dialog and returns false.
 * Does NOT close the editor window. */
static bool profile_ed_save_and_apply(GtkWidget *dlg, void *ud) {
  EdCtx        *ctx = (EdCtx *)ud;
  simprofile_t *p = ctx->prof;
  sim_win_t    *w = ctx->win;

  /* Determine save directory */
  const char *dir;
  if (p->is_global)
    dir = simprofile_global_dir();
  else if (w->project_dir[0])
    dir = w->project_dir;
  else
    dir = simprofile_global_dir();

  /* Ensure directories exist */
  {
    const char *home = getenv("HOME");
    if (home) {
      char caed[512];
      snprintf(caed, sizeof(caed), "%s/.6502caed", home);
      struct stat _st;
      if (stat(caed, &_st) != 0) SIM_MKDIR(caed);
    }
    struct stat _st2;
    if (stat(dir, &_st2) != 0) SIM_MKDIR(dir);
  }

  /* Determine save path:
   * - Existing profile (filepath set): save back to the same file so
   *   edits persist across sessions and don't create duplicates.
   * - New profile (no filepath): derive filename from profile name. */
  char path[768];
  if (p->filepath[0]) {
    snprintf(path, sizeof(path), "%s", p->filepath);
  } else {
    char fname[128];
    int fi = 0;
    for (const char *s = p->name; *s && fi < 120; s++) {
      char ch = *s;
      if (ch == ' ') ch = '_';
      if (isalnum((unsigned char)ch) || ch == '_' || ch == '-')
        fname[fi++] = ch;
    }
    fname[fi] = 0;
    if (!fi) snprintf(fname, sizeof(fname), "profile");
    snprintf(path, sizeof(path), "%s/%s.simprofile", dir, fname);
  }

  if (!simprofile_save_as(p, path)) {
    GtkAlertDialog *err =
        gtk_alert_dialog_new("Failed to save profile to:\n%s", path);
    gtk_alert_dialog_show(err, GTK_WINDOW(dlg));
    g_object_unref(err);
    return false;
  }

  profile_dd_rebuild(w);
  profile_apply(w, p);
  return true;
}

static void on_profile_ed_apply(GtkButton *b, gpointer ud) {
  (void)b;
  GtkWidget *dlg = GTK_WIDGET(ud);
  EdCtx *ctx = (EdCtx *)g_object_get_data(G_OBJECT(dlg), "edctx");
  ed_read_fields(ctx);

  /* Warn about overlaps but allow apply */
  const char *overlap = simprofile_check_overlaps(ctx->prof);
  if (overlap) {
    char msg[320];
    snprintf(msg, sizeof(msg),
             "Warning: ROM ranges overlap: %s\n\n"
             "Applying anyway — reads from overlapping areas may be unpredictable.",
             overlap);
    GtkAlertDialog *warn = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_show(warn, GTK_WINDOW(dlg));
    g_object_unref(warn);
  }

  /* Save + apply, keep window open */
  profile_ed_save_and_apply(dlg, ctx);
}

static void on_profile_ed_default(GtkButton *b, gpointer ud) {
  (void)b;
  GtkWidget *dlg = GTK_WIDGET(ud);
  EdCtx *ctx = (EdCtx *)g_object_get_data(G_OBJECT(dlg), "edctx");
  ed_read_fields(ctx);
  simprofile_set_default(
      ctx->prof, ctx->win->project_dir[0] ? ctx->win->project_dir : NULL);
  GtkAlertDialog *inf = gtk_alert_dialog_new("Profile set as default.");
  gtk_alert_dialog_show(inf, GTK_WINDOW(dlg));
  g_object_unref(inf);
}

/* -----------------------------------------------------------------------
 * Profile editor dialog
 * Shows all profile settings in a scrollable form.
 */
/* -----------------------------------------------------------------------
 * LCD Pin Picker
 */
typedef struct {
  GtkWidget *entry;
  GtkWidget *win;
  GtkWidget *btn_via[2];
  GtkWidget *btn_port[2];
  GtkWidget *btn_bit[8];
  int via, port, bit;
} PinPickerCtx;

static void pin_picker_write(PinPickerCtx *ctx) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%c%d",
           ctx->via, ctx->port == 0 ? 'A' : 'B', ctx->bit);
  gtk_entry_buffer_set_text(
      gtk_entry_get_buffer(GTK_ENTRY(ctx->entry)), buf, -1);
}

static void pin_picker_via(GtkButton *btn, gpointer ud) {
  PinPickerCtx *ctx = (PinPickerCtx *)ud;
  for (int i = 0; i < 2; i++) {
    bool active = (GTK_WIDGET(btn) == ctx->btn_via[i]);
    if (active) ctx->via = i + 1;
    if (active) gtk_widget_add_css_class(ctx->btn_via[i], "suggested-action");
    else        gtk_widget_remove_css_class(ctx->btn_via[i], "suggested-action");
  }
  pin_picker_write(ctx);
}

static void pin_picker_port(GtkButton *btn, gpointer ud) {
  PinPickerCtx *ctx = (PinPickerCtx *)ud;
  for (int i = 0; i < 2; i++) {
    bool active = (GTK_WIDGET(btn) == ctx->btn_port[i]);
    if (active) ctx->port = i;
    if (active) gtk_widget_add_css_class(ctx->btn_port[i], "suggested-action");
    else        gtk_widget_remove_css_class(ctx->btn_port[i], "suggested-action");
  }
  pin_picker_write(ctx);
}

static void pin_picker_bit(GtkButton *btn, gpointer ud) {
  PinPickerCtx *ctx = (PinPickerCtx *)ud;
  for (int i = 0; i < 8; i++) {
    bool active = (GTK_WIDGET(btn) == ctx->btn_bit[i]);
    if (active) ctx->bit = i;
    if (active) gtk_widget_add_css_class(ctx->btn_bit[i], "suggested-action");
    else        gtk_widget_remove_css_class(ctx->btn_bit[i], "suggested-action");
  }
  pin_picker_write(ctx);
}

static void show_pin_picker(GtkWidget *entry, GtkWidget *parent_win,
                            const char *pin_name) {
  const char *cur = gtk_entry_buffer_get_text(
      gtk_entry_get_buffer(GTK_ENTRY(entry)));
  lcd_pin_t pin = {1, 0, 0};
  lcd_pin_parse(cur, &pin);

  PinPickerCtx *ctx = g_new0(PinPickerCtx, 1);
  ctx->via   = (pin.via >= 1 && pin.via <= 2) ? pin.via : 1;
  ctx->port  = (pin.port == 0 || pin.port == 1) ? pin.port : 0;
  ctx->bit   = (pin.bit >= 0 && pin.bit <= 7) ? pin.bit : 0;
  ctx->entry = entry;

  char title[64];
  snprintf(title, sizeof(title), "Pin Mapping: %s", pin_name);
  GtkWidget *win = gtk_window_new();
  ctx->win = win;
  gtk_window_set_title(GTK_WINDOW(win), title);
  gtk_window_set_modal(GTK_WINDOW(win), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(parent_win));
  gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
  if (gtk_widget_has_css_class(parent_win, "dark"))
    gtk_widget_add_css_class(win, "dark");

  GtkWidget *hdr = gtk_header_bar_new();
  gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hdr), TRUE);
  gtk_window_set_titlebar(GTK_WINDOW(win), hdr);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(vbox, 16);
  gtk_widget_set_margin_end(vbox, 16);
  gtk_widget_set_margin_top(vbox, 12);
  gtk_widget_set_margin_bottom(vbox, 16);

  /* VIA row */
  GtkWidget *via_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *via_lbl = gtk_label_new("VIA:");
  gtk_widget_set_size_request(via_lbl, 52, -1);
  gtk_label_set_xalign(GTK_LABEL(via_lbl), 1.0f);
  gtk_box_append(GTK_BOX(via_box), via_lbl);
  const char *via_labels[2] = {"VIA 1", "VIA 2"};
  for (int i = 0; i < 2; i++) {
    ctx->btn_via[i] = gtk_button_new_with_label(via_labels[i]);
    gtk_widget_set_size_request(ctx->btn_via[i], 72, -1);
    if (ctx->via == i + 1)
      gtk_widget_add_css_class(ctx->btn_via[i], "suggested-action");
    g_signal_connect(ctx->btn_via[i], "clicked",
                     G_CALLBACK(pin_picker_via), ctx);
    gtk_box_append(GTK_BOX(via_box), ctx->btn_via[i]);
  }
  gtk_box_append(GTK_BOX(vbox), via_box);

  /* Port row */
  GtkWidget *port_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  GtkWidget *port_lbl = gtk_label_new("Port:");
  gtk_widget_set_size_request(port_lbl, 52, -1);
  gtk_label_set_xalign(GTK_LABEL(port_lbl), 1.0f);
  gtk_box_append(GTK_BOX(port_box), port_lbl);
  const char *port_labels[2] = {"Port A", "Port B"};
  for (int i = 0; i < 2; i++) {
    ctx->btn_port[i] = gtk_button_new_with_label(port_labels[i]);
    gtk_widget_set_size_request(ctx->btn_port[i], 72, -1);
    if (ctx->port == i)
      gtk_widget_add_css_class(ctx->btn_port[i], "suggested-action");
    g_signal_connect(ctx->btn_port[i], "clicked",
                     G_CALLBACK(pin_picker_port), ctx);
    gtk_box_append(GTK_BOX(port_box), ctx->btn_port[i]);
  }
  gtk_box_append(GTK_BOX(vbox), port_box);

  /* Bit row */
  GtkWidget *bit_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *bit_lbl = gtk_label_new("Bit:");
  gtk_widget_set_size_request(bit_lbl, 52, -1);
  gtk_label_set_xalign(GTK_LABEL(bit_lbl), 1.0f);
  gtk_box_append(GTK_BOX(bit_box), bit_lbl);
  for (int i = 0; i < 8; i++) {
    char bl[4];
    snprintf(bl, sizeof(bl), "%d", i);
    ctx->btn_bit[i] = gtk_button_new_with_label(bl);
    gtk_widget_set_size_request(ctx->btn_bit[i], 36, -1);
    if (ctx->bit == i)
      gtk_widget_add_css_class(ctx->btn_bit[i], "suggested-action");
    g_signal_connect(ctx->btn_bit[i], "clicked",
                     G_CALLBACK(pin_picker_bit), ctx);
    gtk_box_append(GTK_BOX(bit_box), ctx->btn_bit[i]);
  }
  gtk_box_append(GTK_BOX(vbox), bit_box);

  /* Separator */
  gtk_box_append(GTK_BOX(vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* Done button */
  GtkWidget *done_btn = gtk_button_new_with_label("Done");
  gtk_widget_add_css_class(done_btn, "suggested-action");
  gtk_widget_set_halign(done_btn, GTK_ALIGN_END);
  g_signal_connect_swapped(done_btn, "clicked",
                           G_CALLBACK(gtk_window_destroy), win);
  gtk_box_append(GTK_BOX(vbox), done_btn);

  g_object_set_data_full(G_OBJECT(win), "ppctx", ctx, g_free);
  gtk_window_set_child(GTK_WINDOW(win), vbox);
  gtk_window_present(GTK_WINDOW(win));
}

typedef struct { GtkWidget *entry; GtkWidget *parent_win; char name[16]; } PinBtnData;

static void on_pin_pick_clicked(GtkButton *btn, gpointer ud) {
  (void)btn;
  PinBtnData *d = (PinBtnData *)ud;
  show_pin_picker(d->entry, d->parent_win, d->name);
}

/* Make a box with a pin entry + "..." pick button */
static GtkWidget *make_pin_row(GtkWidget *entry, GtkWidget *parent_win,
                               const char *name) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_box_append(GTK_BOX(box), entry);
  GtkWidget *btn = gtk_button_new_with_label("…");
  gtk_widget_set_tooltip_text(btn, "Pick pin mapping");
  PinBtnData *d = g_new0(PinBtnData, 1);
  d->entry      = entry;
  d->parent_win = parent_win;
  snprintf(d->name, sizeof(d->name), "%s", name);
  g_object_set_data_full(G_OBJECT(btn), "ppd", d, g_free);
  g_signal_connect(btn, "clicked", G_CALLBACK(on_pin_pick_clicked), d);
  gtk_box_append(GTK_BOX(box), btn);
  return box;
}

/* -------------------------------------------------------------------------
 * SD card file management callbacks for profile editor
 * ---------------------------------------------------------------------- */

static void sd_img_update_warn(GtkWidget *entry, GtkWidget *warn) {
  const char *path = gtk_entry_buffer_get_text(
      gtk_entry_get_buffer(GTK_ENTRY(entry)));
  if (path && path[0]) {
    struct stat _st;
    gtk_label_set_text(GTK_LABEL(warn),
        stat(path, &_st) == 0 ? "" : "⚠ File not found");
  } else {
    gtk_label_set_text(GTK_LABEL(warn), "");
  }
}

static void sd_browse_response_cb(GObject *src, GAsyncResult *res, gpointer ud) {
  GtkWidget *btn = GTK_WIDGET(ud);
  GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "entry");
  GtkWidget *warn  = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "warn");
  GError *err = NULL;
  GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, &err);
  if (!file) { if (err) g_error_free(err); return; }
  char *path = g_file_get_path(file);
  g_object_unref(file);
  if (path) {
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(entry)), path, -1);
    sd_img_update_warn(entry, warn);
    g_free(path);
  }
}

static void on_sd_image_browse(GtkButton *btn, gpointer ud) {
  (void)ud;
  GtkWidget *win = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "win");
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Select SD Card Image");
  GtkFileFilter *ff = gtk_file_filter_new();
  gtk_file_filter_set_name(ff, "Disk images (*.img *.bin *.sd)");
  gtk_file_filter_add_pattern(ff, "*.img");
  gtk_file_filter_add_pattern(ff, "*.bin");
  gtk_file_filter_add_pattern(ff, "*.sd");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, ff);
  gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
  gtk_file_dialog_open(dlg, GTK_WINDOW(win), NULL,
                       sd_browse_response_cb, btn);
  g_object_unref(ff); g_object_unref(filters); g_object_unref(dlg);
}

/* Create new image dialog context */
typedef struct {
  GtkWidget *entry;   /* path entry in profile editor */
  GtkWidget *warn;
  GtkWidget *dialog;
  GtkWidget *path_entry;
  GtkWidget *dd_size;
} SdCreateCtx;

static void sd_create_ok_cb(GtkButton *b, gpointer ud) {
  (void)b;
  SdCreateCtx *ctx = (SdCreateCtx *)ud;
  const char *path = gtk_entry_buffer_get_text(
      gtk_entry_get_buffer(GTK_ENTRY(ctx->path_entry)));
  guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dd_size));
  if (sel >= SD_SIZE_COUNT) sel = SD_SIZE_32MB;
  if (!path || !path[0]) return;
  if (!sd_create(path, sd_size_bytes((sd_size_id_t)sel))) {
    GtkAlertDialog *ad = gtk_alert_dialog_new(
        "Failed to create image file:\n%s\n\nCheck path and disk space.", path);
    gtk_alert_dialog_show(ad, GTK_WINDOW(ctx->dialog));
    g_object_unref(ad);
    return;
  }
  /* Update the profile editor path entry */
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entry)),
                            path, -1);
  sd_img_update_warn(ctx->entry, ctx->warn);
  gtk_window_close(GTK_WINDOW(ctx->dialog));
}

static void on_sd_create_clicked(GtkButton *btn, gpointer ud) {
  (void)ud;
  GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "entry");
  GtkWidget *warn  = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "warn");
  GtkWidget *win   = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "win");

  GtkWidget *dlg = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(dlg), "Create SD Card Image");
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(win));
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_default_size(GTK_WINDOW(dlg), 440, -1);

  SdCreateCtx *ctx = g_new0(SdCreateCtx, 1);
  ctx->entry  = entry;
  ctx->warn   = warn;
  ctx->dialog = dlg;
  g_object_set_data_full(G_OBJECT(dlg), "ctx", ctx, g_free);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(vbox, 16); gtk_widget_set_margin_end(vbox, 16);
  gtk_widget_set_margin_top(vbox, 16);   gtk_widget_set_margin_bottom(vbox, 16);
  gtk_window_set_child(GTK_WINDOW(dlg), vbox);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Image file path:"));
  GtkWidget *path_e = gtk_entry_new();
  char def[512];
  snprintf(def, sizeof(def), "%s/sdcard.img", g_get_home_dir());
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(path_e)), def, -1);
  ctx->path_entry = path_e;
  gtk_box_append(GTK_BOX(vbox), path_e);

  gtk_box_append(GTK_BOX(vbox), gtk_label_new("Card size:"));
  const char *sznames[SD_SIZE_COUNT + 1];
  for (int i = 0; i < SD_SIZE_COUNT; i++) sznames[i] = sd_size_label(i);
  sznames[SD_SIZE_COUNT] = NULL;
  GtkWidget *dd = gtk_drop_down_new_from_strings(sznames);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), SD_SIZE_32MB);
  ctx->dd_size = dd;
  gtk_box_append(GTK_BOX(vbox), dd);

  GtkWidget *warn_lbl = gtk_label_new("⚠ Large images (≥1GB) may take time to create.");
  gtk_widget_add_css_class(warn_lbl, "dim-label");
  gtk_box_append(GTK_BOX(vbox), warn_lbl);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(hbox, GTK_ALIGN_END);
  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  GtkWidget *btn_ok     = gtk_button_new_with_label("Create");
  gtk_widget_add_css_class(btn_ok, "suggested-action");
  g_signal_connect_swapped(btn_cancel, "clicked",
                           G_CALLBACK(gtk_window_close), dlg);
  g_signal_connect(btn_ok, "clicked", G_CALLBACK(sd_create_ok_cb), ctx);
  gtk_box_append(GTK_BOX(hbox), btn_cancel);
  gtk_box_append(GTK_BOX(hbox), btn_ok);
  gtk_box_append(GTK_BOX(vbox), hbox);
  gtk_window_present(GTK_WINDOW(dlg));
}

static void sd_dup_save_cb(GObject *src, GAsyncResult *res, gpointer ud) {
  GtkWidget *btn = GTK_WIDGET(ud);
  GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "entry");
  GtkWidget *warn  = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "warn");
  GError *err = NULL;
  GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(src), res, &err);
  if (!file) { if (err) g_error_free(err); return; }
  char *dest = g_file_get_path(file);
  g_object_unref(file);
  if (!dest) return;
  const char *src_path = gtk_entry_buffer_get_text(
      gtk_entry_get_buffer(GTK_ENTRY(entry)));
  if (src_path && src_path[0]) {
    FILE *sf = fopen(src_path, "rb");
    FILE *df = fopen(dest, "wb");
    if (sf && df) {
      uint8_t buf[65536]; size_t n;
      while ((n = fread(buf, 1, sizeof(buf), sf)) > 0)
        fwrite(buf, 1, n, df);
    }
    if (sf) fclose(sf);
    if (df) fclose(df);
    /* Update entry to point to duplicate */
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(entry)), dest, -1);
    sd_img_update_warn(entry, warn);
  }
  g_free(dest);
}

static void on_sd_dup_clicked(GtkButton *btn, gpointer ud) {
  (void)ud;
  GtkWidget *win = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "win");
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Save Duplicate As");
  gtk_file_dialog_set_initial_name(dlg, "sdcard_copy.img");
  gtk_file_dialog_save(dlg, GTK_WINDOW(win), NULL, sd_dup_save_cb, btn);
  g_object_unref(dlg);
}

static void sd_delete_confirmed_cb(GtkAlertDialog *ad, GAsyncResult *res,
                                    gpointer ud) {
  GtkWidget *btn = GTK_WIDGET(ud);
  GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "entry");
  GtkWidget *warn  = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "warn");
  int idx = gtk_alert_dialog_choose_finish(ad, res, NULL);
  if (idx != 0) return; /* 0 = Delete, 1 = Cancel */
  const char *path = gtk_entry_buffer_get_text(
      gtk_entry_get_buffer(GTK_ENTRY(entry)));
  if (path && path[0]) {
    remove(path);
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(entry)), "", -1);
    gtk_label_set_text(GTK_LABEL(warn), "");
  }
}

static void on_sd_delete_clicked(GtkButton *btn, gpointer ud) {
  (void)ud;
  GtkWidget *win   = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "win");
  GtkWidget *entry = (GtkWidget *)g_object_get_data(G_OBJECT(btn), "entry");
  const char *path = gtk_entry_buffer_get_text(
      gtk_entry_get_buffer(GTK_ENTRY(entry)));
  if (!path || !path[0]) return;
  char msg[600];
  snprintf(msg, sizeof(msg),
           "Permanently delete this image file?\n\n%s\n\nThis cannot be undone.", path);
  const char *btns[] = {"Delete", "Cancel", NULL};
  GtkAlertDialog *ad = gtk_alert_dialog_new("%s", msg);
  gtk_alert_dialog_set_buttons(ad, btns);
  gtk_alert_dialog_set_cancel_button(ad, 1);
  gtk_alert_dialog_set_default_button(ad, 1);
  gtk_alert_dialog_choose(ad, GTK_WINDOW(win), NULL,
                          (GAsyncReadyCallback)sd_delete_confirmed_cb, btn);
  g_object_unref(ad);
}

static void show_profile_editor(sim_win_t *w, simprofile_t *prof, bool is_new) {
  GtkWidget *dlg = gtk_window_new();
  gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w->window));
  gtk_window_set_default_size(GTK_WINDOW(dlg), 560, 640);

  /* Match dark/light theme of the parent simulator window.
   * Must also set a GtkHeaderBar — without it the system titlebar is used
   * and CSS colour classes have no effect on it. */
  if (gtk_widget_has_css_class(w->window, "dark")) {
    gtk_widget_add_css_class(dlg, "dark");
    gtk_widget_remove_css_class(dlg, "light");
  } else {
    gtk_widget_add_css_class(dlg, "light");
    gtk_widget_remove_css_class(dlg, "dark");
  }
  GtkWidget *prof_hdr = gtk_header_bar_new();
  gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(prof_hdr), TRUE);
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(prof_hdr),
      gtk_label_new(is_new ? "New Profile" : "Edit Profile"));
  gtk_window_set_titlebar(GTK_WINDOW(dlg), prof_hdr);

  /* Working copy */
  simprofile_t *p = calloc(1, sizeof(*p));
  *p = *prof;

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(dlg), root);

  /* Scrolled content */
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_box_append(GTK_BOX(root), scroll);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
  gtk_widget_set_margin_start(grid, 12);
  gtk_widget_set_margin_end(grid, 12);
  gtk_widget_set_margin_top(grid, 12);
  gtk_widget_set_margin_bottom(grid, 12);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), grid);

  int row = 0;
#define LABEL(txt) gtk_label_new(txt)
#define HEAD(txt)                                                              \
  {                                                                            \
    GtkWidget *h = gtk_label_new(txt);                                         \
    gtk_widget_add_css_class(h, "heading");                                    \
    gtk_widget_set_halign(h, GTK_ALIGN_START);                                 \
    gtk_widget_set_margin_top(h, 8);                                           \
    gtk_grid_attach(GTK_GRID(grid), h, 0, row++, 4, 1);                        \
  }
#define ROW2(lbl, widget)                                                      \
  {                                                                            \
    GtkWidget *l = LABEL(lbl);                                                 \
    gtk_widget_set_halign(l, GTK_ALIGN_END);                                   \
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);                          \
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 3, 1);                     \
    row++;                                                                     \
  }

  /* Identity */
  HEAD("Profile");
  GtkWidget *e_name = gtk_entry_new();
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e_name)), p->name,
                            -1);
  ROW2("Name:", e_name);

  GtkWidget *e_desc = gtk_entry_new();
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e_desc)),
                            p->description, -1);
  ROW2("Description:", e_desc);

  GtkWidget *chk_global = gtk_check_button_new_with_label("Global profile");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_global), p->is_global);
  gtk_grid_attach(GTK_GRID(grid), chk_global, 1, row++, 3, 1);

  /* CPU */
  HEAD("CPU");
  const char *cpu_opts[] = {"6502", "6502 (undoc)", "65C02", "65816", NULL};
  GtkWidget *dd_cpu = gtk_drop_down_new_from_strings(cpu_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dd_cpu), (guint)(p->cpu_type & 3));
  ROW2("CPU Type:", dd_cpu);

  /* $DB=STP option — only for 6502/6502U */
  GtkWidget *chk_stp_db = gtk_check_button_new_with_label(
      "$DB = STP  (treat opcode $DB as halt on 6502)");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_stp_db), false); /* runtime-only — see toolbar $DB=STP */
  gtk_widget_set_visible(chk_stp_db,
      p->cpu_type == PROFILE_CPU_6502 || p->cpu_type == PROFILE_CPU_6502U);
  gtk_grid_attach(GTK_GRID(grid), chk_stp_db, 1, row++, 3, 1);

  /* ROM ranges */
  HEAD("ROM Ranges");
  GtkWidget *rom_enable[PROFILE_ROM_MAX];
  GtkWidget *rom_from[PROFILE_ROM_MAX];
  GtkWidget *rom_to[PROFILE_ROM_MAX];
  GtkWidget *rom_bank[PROFILE_ROM_MAX];
  GtkWidget *rom_rw[PROFILE_ROM_MAX];
  GtkWidget *rom_lock[PROFILE_ROM_MAX];

  for (int i = 0; i < PROFILE_ROM_MAX; i++) {
    char lbl[32];
    snprintf(lbl, sizeof(lbl), "ROM%d:", i);
    rom_enable[i] = gtk_check_button_new_with_label("Enable");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(rom_enable[i]),
                                p->rom[i].enabled);
    gtk_grid_attach(GTK_GRID(grid), gtk_label_new(lbl), 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), rom_enable[i], 1, row, 1, 1);
    row++;

    rom_from[i] = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(rom_from[i]), 4);
    gtk_widget_set_size_request(rom_from[i], 70, -1);
    char hx[8];
    snprintf(hx, sizeof(hx), "%04X", p->rom[i].from);
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(rom_from[i])), hx,
                              -1);
    rom_to[i] = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(rom_to[i]), 4);
    gtk_widget_set_size_request(rom_to[i], 70, -1);
    snprintf(hx, sizeof(hx), "%04X", p->rom[i].to);
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(rom_to[i])), hx,
                              -1);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(hbox), gtk_label_new("From $"));
    gtk_box_append(GTK_BOX(hbox), rom_from[i]);
    gtk_box_append(GTK_BOX(hbox), gtk_label_new("To $"));
    gtk_box_append(GTK_BOX(hbox), rom_to[i]);
    gtk_grid_attach(GTK_GRID(grid), hbox, 1, row++, 3, 1);

    /* Bank: "All" or 0-255 */
    const char *bank_opts[] = {"All", "0",  "1",  "2",  "3",  "4",
                               "5",   "6",  "7",  "8",  "9",  "10",
                               "11",  "12", "13", "14", "15", NULL};
    rom_bank[i] = gtk_drop_down_new_from_strings(bank_opts);
    {
      guint bsel = (p->rom[i].bank < 0) ? 0 : (guint)(p->rom[i].bank + 1);
      if (bsel > 16)
        bsel = 0;
      gtk_drop_down_set_selected(GTK_DROP_DOWN(rom_bank[i]), bsel);
    }
    gtk_widget_set_tooltip_text(rom_bank[i],
                                "Bank: All = active in every bank");

    /* R/W bit — plain entry: -1=none, 0-7=bit number */
    {
      char _rwtxt[8];
      snprintf(_rwtxt, sizeof(_rwtxt), "%d", p->rom[i].rw_bit);
      rom_rw[i] = gtk_entry_new();
      gtk_entry_set_max_length(GTK_ENTRY(rom_rw[i]), 2);
      gtk_widget_set_size_request(rom_rw[i], 50, -1);
      gtk_widget_set_hexpand(rom_rw[i], FALSE);
      gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(rom_rw[i])),
                                _rwtxt, -1);
      gtk_widget_set_tooltip_text(rom_rw[i],
          "Which bit in the ROM control register controls R/W for this range (0-7)");
    }

    /* Lock bit — plain entry: -1=none, 0-7=bit number */
    {
      char _lktxt[8];
      snprintf(_lktxt, sizeof(_lktxt), "%d", p->rom[i].lock_bit);
      rom_lock[i] = gtk_entry_new();
      gtk_entry_set_max_length(GTK_ENTRY(rom_lock[i]), 2);
      gtk_widget_set_size_request(rom_lock[i], 50, -1);
      gtk_widget_set_hexpand(rom_lock[i], FALSE);
      gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(rom_lock[i])),
                                _lktxt, -1);
      gtk_widget_set_tooltip_text(rom_lock[i],
          "Which bit in the ROM control register locks this range until reset (0-7)");
    }

    GtkWidget *hbox2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(hbox2), gtk_label_new("Bank:"));
    gtk_box_append(GTK_BOX(hbox2), rom_bank[i]);
    gtk_box_append(GTK_BOX(hbox2), gtk_label_new("R/W bit:"));
    gtk_box_append(GTK_BOX(hbox2), rom_rw[i]);
    gtk_box_append(GTK_BOX(hbox2), gtk_label_new("Lock bit:"));
    gtk_box_append(GTK_BOX(hbox2), rom_lock[i]);
    gtk_grid_attach(GTK_GRID(grid), hbox2, 1, row++, 3, 1);
  }

  /* ROM control register */
  HEAD("ROM Control Register");
  GtkWidget *chk_rctrl = gtk_check_button_new_with_label("Enable");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_rctrl), p->rom_ctrl.enabled);
  gtk_grid_attach(GTK_GRID(grid), chk_rctrl, 1, row++, 3, 1);
  GtkWidget *e_rctrl = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(e_rctrl), 4);
  gtk_widget_set_size_request(e_rctrl, 70, -1);
  char hx[8];
  snprintf(hx, sizeof(hx), "%04X", p->rom_ctrl.addr);
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e_rctrl)), hx, -1);
  ROW2("Address $:", e_rctrl);

  /* IO devices */
  HEAD("IO Devices");
#define IO_ROW(label, chk, e_addr, io_field)                                   \
  {                                                                            \
    chk = gtk_check_button_new_with_label(label);                              \
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk), p->io_field.enabled);   \
    e_addr = gtk_entry_new();                                                  \
    gtk_entry_set_max_length(GTK_ENTRY(e_addr), 4);                            \
    gtk_widget_set_size_request(e_addr, 70, -1);                               \
    snprintf(hx, sizeof(hx), "%04X", p->io_field.addr);                        \
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e_addr)), hx,     \
                              -1);                                             \
    GtkWidget *_hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);               \
    gtk_box_append(GTK_BOX(_hb), chk);                                         \
    gtk_box_append(GTK_BOX(_hb), gtk_label_new("Addr $"));                     \
    gtk_box_append(GTK_BOX(_hb), e_addr);                                      \
    gtk_grid_attach(GTK_GRID(grid), _hb, 1, row++, 3, 1);                      \
  }

  GtkWidget *chk_via1, *e_via1;
  GtkWidget *chk_via2, *e_via2;
  GtkWidget *chk_acia, *e_acia;
  GtkWidget *chk_tx, *e_tx;
  GtkWidget *chk_rx, *e_rx;
  IO_ROW("VIA1", chk_via1, e_via1, via1);
  IO_ROW("VIA2", chk_via2, e_via2, via2);
  IO_ROW("ACIA", chk_acia, e_acia, acia);
  IO_ROW("Console TX", chk_tx, e_tx, simio_tx);
  IO_ROW("Console RX", chk_rx, e_rx, simio_rx);

  /* Banking */
  HEAD("Banking (6502/65C02 only)");
  GtkWidget *chk_bank, *e_bank;
  IO_ROW("Bank Register", chk_bank, e_bank, bank_reg);
  GtkWidget *e_bwin = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(e_bwin), 4);
  gtk_widget_set_size_request(e_bwin, 70, -1);
  snprintf(hx, sizeof(hx), "%04X", p->bank_window);
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e_bwin)), hx, -1);
  ROW2("Window $:", e_bwin);
  GtkWidget *e_bwsz = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(e_bwsz), 4);
  gtk_widget_set_size_request(e_bwsz, 70, -1);
  snprintf(hx, sizeof(hx), "%04X", p->bank_window_sz);
  gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e_bwsz)), hx, -1);
  ROW2("Window Size $:", e_bwsz);
  GtkWidget *spn_banks = gtk_spin_button_new_with_range(0, 255, 1);
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(spn_banks), p->bank_num);
  ROW2("Num Banks:", spn_banks);

  /* LCD Display — widget variables at function scope so ctx can see them */
  GtkWidget *chk_lcd = NULL, *dd_lcd_bus = NULL;
  GtkWidget *spn_lcd_cols = NULL, *spn_lcd_rows = NULL, *chk_lcd_strict = NULL;
  GtkWidget *e_lcd_rs = NULL, *e_lcd_rw = NULL, *e_lcd_e = NULL;
  GtkWidget *e_lcd_d[8] = {NULL};
  GtkWidget *lcd_d03 = NULL;
  HEAD("LCD Display (HD44780)");
  {
    /* Helper macro for compact pin entry: format "1A0"=VIA1 port A bit 0 */
#define PIN_ENTRY(field)                                                            {                                                                                 char _pb[8];                                                                    lcd_pin_format(&p->lcd.field, _pb, sizeof(_pb));                               e_lcd_##field = gtk_entry_new();                                                gtk_entry_set_max_length(GTK_ENTRY(e_lcd_##field), 3);                         gtk_widget_set_size_request(e_lcd_##field, 50, -1);                            gtk_widget_set_hexpand(e_lcd_##field, FALSE);                                  gtk_entry_buffer_set_text(                                                          gtk_entry_get_buffer(GTK_ENTRY(e_lcd_##field)), _pb, -1);                  gtk_widget_set_tooltip_text(e_lcd_##field,                                         "Pin: VIA(1/2) Port(A/B) Bit(0-7)  e.g. 1A0 = VIA1 port A bit 0");     }

    /* Enable + bus width row */
    chk_lcd = gtk_check_button_new_with_label("Enable LCD");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_lcd), p->lcd.enabled);
    dd_lcd_bus = gtk_drop_down_new_from_strings(
        (const char *[]){"8-bit", "4-bit", NULL});
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd_lcd_bus),
        p->lcd.bus_width == 4 ? 1 : 0);
    gtk_widget_set_tooltip_text(dd_lcd_bus, "Interface bus width");

    GtkWidget *lcd_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(lcd_top), chk_lcd);
    gtk_box_append(GTK_BOX(lcd_top), gtk_label_new("Bus:"));
    gtk_box_append(GTK_BOX(lcd_top), dd_lcd_bus);
    gtk_grid_attach(GTK_GRID(grid), lcd_top, 0, row++, 4, 1);

    /* Geometry row */
    spn_lcd_cols = gtk_spin_button_new_with_range(8, 40, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spn_lcd_cols), p->lcd.cols);
    spn_lcd_rows = gtk_spin_button_new_with_range(1, 4, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spn_lcd_rows), p->lcd.rows);
    chk_lcd_strict = gtk_check_button_new_with_label("Strict timing");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_lcd_strict), p->lcd.strict);
    GtkWidget *lcd_geom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(lcd_geom), gtk_label_new("Cols:"));
    gtk_box_append(GTK_BOX(lcd_geom), spn_lcd_cols);
    gtk_box_append(GTK_BOX(lcd_geom), gtk_label_new("Rows:"));
    gtk_box_append(GTK_BOX(lcd_geom), spn_lcd_rows);
    gtk_box_append(GTK_BOX(lcd_geom), chk_lcd_strict);
    gtk_grid_attach(GTK_GRID(grid), lcd_geom, 0, row++, 4, 1);

    /* Control pins row: RS, RW, E — each with a pick button */
    PIN_ENTRY(rs); PIN_ENTRY(rw); PIN_ENTRY(e);
    GtkWidget *lcd_ctrl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(lcd_ctrl), gtk_label_new("RS:"));
    gtk_box_append(GTK_BOX(lcd_ctrl), make_pin_row(e_lcd_rs, dlg, "RS"));
    gtk_box_append(GTK_BOX(lcd_ctrl), gtk_label_new("RW:"));
    gtk_box_append(GTK_BOX(lcd_ctrl), make_pin_row(e_lcd_rw, dlg, "RW"));
    gtk_box_append(GTK_BOX(lcd_ctrl), gtk_label_new("E:"));
    gtk_box_append(GTK_BOX(lcd_ctrl), make_pin_row(e_lcd_e, dlg, "E"));
    gtk_grid_attach(GTK_GRID(grid), lcd_ctrl, 0, row++, 4, 1);

    /* Data pins D0-D3 */
    {
      char _pb[8];
      for (int _i = 0; _i < 8; _i++) {
        /* In 4-bit mode: D4-D7 are in data[0-3], D0-D3 unused.
         * We show D4-D7 in e_lcd_d[0-3] (d03 row hidden, d47 row shows).
         * e_lcd_d[4-7] hold D4-D7 for 8-bit mode.
         * For 4-bit: e_lcd_d[0-3] = data[0-3], e_lcd_d[4-7] = same.
         * For 8-bit: e_lcd_d[0-7] = data[0-7]. */
        /* In 4-bit mode: D4-D7 stored in data[0-3]
         * so e_lcd_d[4-7] (the visible d47 row) reads from data[0-3] */
        int data_idx = (p->lcd.bus_width == 4 && _i >= 4) ? _i - 4 : _i;
        lcd_pin_format(&p->lcd.data[data_idx], _pb, sizeof(_pb));
        e_lcd_d[_i] = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(e_lcd_d[_i]), 3);
        gtk_widget_set_size_request(e_lcd_d[_i], 50, -1);
        gtk_widget_set_hexpand(e_lcd_d[_i], FALSE);
        gtk_entry_buffer_set_text(
            gtk_entry_get_buffer(GTK_ENTRY(e_lcd_d[_i])), _pb, -1);
        gtk_widget_set_tooltip_text(e_lcd_d[_i],
            "Pin: VIA(1/2) Port(A/B) Bit(0-7)  e.g. 1A0");
      }
    }
    lcd_d03 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(lcd_d03), gtk_label_new("D0:"));
    gtk_box_append(GTK_BOX(lcd_d03), make_pin_row(e_lcd_d[0], dlg, "D0"));
    gtk_box_append(GTK_BOX(lcd_d03), gtk_label_new("D1:"));
    gtk_box_append(GTK_BOX(lcd_d03), make_pin_row(e_lcd_d[1], dlg, "D1"));
    gtk_box_append(GTK_BOX(lcd_d03), gtk_label_new("D2:"));
    gtk_box_append(GTK_BOX(lcd_d03), make_pin_row(e_lcd_d[2], dlg, "D2"));
    gtk_box_append(GTK_BOX(lcd_d03), gtk_label_new("D3:"));
    gtk_box_append(GTK_BOX(lcd_d03), make_pin_row(e_lcd_d[3], dlg, "D3"));
    gtk_grid_attach(GTK_GRID(grid), lcd_d03, 0, row++, 4, 1);
    /* Hide D0-D3 row in 4-bit mode */
    if (p->lcd.bus_width == 4)
      gtk_widget_set_visible(lcd_d03, FALSE);

    /* Data pins D4-D7 */
    GtkWidget *lcd_d47 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(lcd_d47), gtk_label_new("D4:"));
    gtk_box_append(GTK_BOX(lcd_d47), make_pin_row(e_lcd_d[4], dlg, "D4"));
    gtk_box_append(GTK_BOX(lcd_d47), gtk_label_new("D5:"));
    gtk_box_append(GTK_BOX(lcd_d47), make_pin_row(e_lcd_d[5], dlg, "D5"));
    gtk_box_append(GTK_BOX(lcd_d47), gtk_label_new("D6:"));
    gtk_box_append(GTK_BOX(lcd_d47), make_pin_row(e_lcd_d[6], dlg, "D6"));
    gtk_box_append(GTK_BOX(lcd_d47), gtk_label_new("D7:"));
    gtk_box_append(GTK_BOX(lcd_d47), make_pin_row(e_lcd_d[7], dlg, "D7"));
    gtk_grid_attach(GTK_GRID(grid), lcd_d47, 0, row++, 4, 1);

#undef PIN_ENTRY
  }

  /* ---- SD Card section ---- */
  {
    HEAD("SD Card (SPI)");

    /* Enable + strict on same row */
    GtkWidget *chk_sd = gtk_check_button_new_with_label("Enable");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_sd), p->sd.enabled);
    GtkWidget *chk_sd_strict = gtk_check_button_new_with_label("Strict timing");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_sd_strict), p->sd.strict);
    GtkWidget *sd_en_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(sd_en_row), chk_sd);
    gtk_box_append(GTK_BOX(sd_en_row), chk_sd_strict);
    gtk_grid_attach(GTK_GRID(grid), sd_en_row, 0, row++, 4, 1);

    /* Pin rows with picker buttons — reuse make_pin_row like LCD */
    const char *sd_pnames[] = {"SCK:", "MOSI:", "MISO:", "CS:"};
    spi_pin_t  *sd_pins[]   = {&p->sd.sck, &p->sd.mosi, &p->sd.miso, &p->sd.cs};
    GtkWidget  *sd_pentries[4];
    for (int _i = 0; _i < 4; _i++) {
      GtkWidget *lbl = gtk_label_new(sd_pnames[_i]);
      gtk_widget_set_halign(lbl, GTK_ALIGN_END);
      gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);
      char pb[8]; spi_pin_format(sd_pins[_i], pb, sizeof(pb));
      GtkWidget *e = gtk_entry_new();
      gtk_entry_set_max_length(GTK_ENTRY(e), 3);
      gtk_widget_set_size_request(e, 56, -1);
      gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e)), pb, -1);
      /* Wrap in make_pin_row for the ... picker button */
      GtkWidget *pr = make_pin_row(e, dlg, sd_pnames[_i]);
      gtk_grid_attach(GTK_GRID(grid), pr, 1, row++, 3, 1);
      sd_pentries[_i] = e;
    }

    /* Card image path + status */
    GtkWidget *img_lbl = gtk_label_new("Image:");
    gtk_widget_set_halign(img_lbl, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), img_lbl, 0, row, 1, 1);

    GtkWidget *e_img = gtk_entry_new();
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(e_img)),
                              p->sd.image_path[0] ? p->sd.image_path : "", -1);
    gtk_widget_set_hexpand(e_img, TRUE);

    /* Warn if path set but file doesn't exist */
    GtkWidget *img_warn = gtk_label_new("");
    gtk_widget_add_css_class(img_warn, "error");
    gtk_widget_set_halign(img_warn, GTK_ALIGN_START);
    if (p->sd.image_path[0]) {
      struct stat _st;
      if (stat(p->sd.image_path, &_st) != 0)
        gtk_label_set_text(GTK_LABEL(img_warn), "⚠ File not found");
    }

    /* Image row: entry + Browse button */
    GtkWidget *img_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(img_box), e_img);

    /* Browse button opens file chooser */
    GtkWidget *btn_browse = gtk_button_new_with_label("Browse…");
    g_object_set_data(G_OBJECT(btn_browse), "entry", e_img);
    g_object_set_data(G_OBJECT(btn_browse), "warn",  img_warn);
    g_object_set_data(G_OBJECT(btn_browse), "win",   dlg);
    g_signal_connect(btn_browse, "clicked",
        G_CALLBACK(on_sd_image_browse), NULL);
    gtk_box_append(GTK_BOX(img_box), btn_browse);
    gtk_grid_attach(GTK_GRID(grid), img_box, 1, row++, 3, 1);
    gtk_grid_attach(GTK_GRID(grid), img_warn, 1, row++, 3, 1);

    /* File management buttons: Create, Duplicate, Delete */
    GtkWidget *sd_file_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *btn_create = gtk_button_new_with_label("Create new…");
    GtkWidget *btn_dup    = gtk_button_new_with_label("Duplicate…");
    GtkWidget *btn_del    = gtk_button_new_with_label("Delete…");
    gtk_widget_add_css_class(btn_del, "destructive-action");
    g_object_set_data(G_OBJECT(btn_create), "entry", e_img);
    g_object_set_data(G_OBJECT(btn_create), "warn",  img_warn);
    g_object_set_data(G_OBJECT(btn_create), "win",   dlg);
    g_object_set_data(G_OBJECT(btn_dup),    "entry", e_img);
    g_object_set_data(G_OBJECT(btn_dup),    "warn",  img_warn);
    g_object_set_data(G_OBJECT(btn_dup),    "win",   dlg);
    g_object_set_data(G_OBJECT(btn_del),    "entry", e_img);
    g_object_set_data(G_OBJECT(btn_del),    "warn",  img_warn);
    g_object_set_data(G_OBJECT(btn_del),    "win",   dlg);
    g_signal_connect(btn_create, "clicked", G_CALLBACK(on_sd_create_clicked),  NULL);
    g_signal_connect(btn_dup,    "clicked", G_CALLBACK(on_sd_dup_clicked),     NULL);
    g_signal_connect(btn_del,    "clicked", G_CALLBACK(on_sd_delete_clicked),  NULL);
    gtk_box_append(GTK_BOX(sd_file_box), btn_create);
    gtk_box_append(GTK_BOX(sd_file_box), btn_dup);
    gtk_box_append(GTK_BOX(sd_file_box), btn_del);
    gtk_grid_attach(GTK_GRID(grid), sd_file_box, 1, row++, 3, 1);

    /* Store in ctx after ctx is allocated (done below) */
    g_object_set_data(G_OBJECT(dlg), "sd_chk",    chk_sd);
    g_object_set_data(G_OBJECT(dlg), "sd_strict", chk_sd_strict);
    g_object_set_data(G_OBJECT(dlg), "sd_sck",    sd_pentries[0]);
    g_object_set_data(G_OBJECT(dlg), "sd_mosi",   sd_pentries[1]);
    g_object_set_data(G_OBJECT(dlg), "sd_miso",   sd_pentries[2]);
    g_object_set_data(G_OBJECT(dlg), "sd_cs",     sd_pentries[3]);
    g_object_set_data(G_OBJECT(dlg), "sd_img",    e_img);
  }


#undef HEAD
#undef ROW2
#undef LABEL
#undef IO_ROW


  /* Button row */
  GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(btn_row, 12);
  gtk_widget_set_margin_end(btn_row, 12);
  gtk_widget_set_margin_top(btn_row, 8);
  gtk_widget_set_margin_bottom(btn_row, 8);
  gtk_box_append(GTK_BOX(root), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(root), btn_row);

  /* Make default button */
  GtkWidget *btn_default = gtk_button_new_with_label("Make Default");
  GtkWidget *btn_new_ed = gtk_button_new_with_label("New Profile");
  GtkWidget *btn_save = gtk_button_new_with_label("Save");
  GtkWidget *btn_apply = gtk_button_new_with_label("Apply");
  GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
  gtk_widget_set_tooltip_text(btn_new_ed, "Create a new profile from defaults");
  gtk_box_append(GTK_BOX(btn_row), btn_default);
  gtk_box_append(GTK_BOX(btn_row), btn_new_ed);
  GtkWidget *spacer = gtk_label_new("");
  gtk_widget_set_hexpand(spacer, TRUE);
  gtk_box_append(GTK_BOX(btn_row), spacer);
  gtk_box_append(GTK_BOX(btn_row), btn_save);
  gtk_box_append(GTK_BOX(btn_row), btn_apply);
  gtk_box_append(GTK_BOX(btn_row), btn_cancel);

  /* Collect widgets into context stored on the dialog window */
  EdCtx *ctx = calloc(1, sizeof(EdCtx));
  ctx->win = w;
  ctx->prof = p;
  ctx->is_new = is_new;
  ctx->e_name = e_name;
  ctx->e_desc = e_desc;
  ctx->chk_global = chk_global;
  ctx->dd_cpu = dd_cpu;
  for (int i = 0; i < PROFILE_ROM_MAX; i++) {
    ctx->rom_enable[i] = rom_enable[i];
    ctx->rom_from[i] = rom_from[i];
    ctx->rom_to[i] = rom_to[i];
    ctx->rom_bank[i] = rom_bank[i];
    ctx->rom_rw[i] = rom_rw[i];
    ctx->rom_lock[i] = rom_lock[i];
  }
  ctx->chk_rctrl = chk_rctrl;
  ctx->e_rctrl = e_rctrl;
  ctx->chk_via1 = chk_via1;
  ctx->e_via1 = e_via1;
  ctx->chk_via2 = chk_via2;
  ctx->e_via2 = e_via2;
  ctx->chk_acia = chk_acia;
  ctx->e_acia = e_acia;
  ctx->chk_tx = chk_tx;
  ctx->e_tx = e_tx;
  ctx->chk_rx = chk_rx;
  ctx->e_rx = e_rx;
  ctx->chk_bank = chk_bank;
  ctx->e_bank = e_bank;
  ctx->e_bwin = e_bwin;
  ctx->e_bwsz = e_bwsz;
  ctx->spn_banks      = spn_banks;
  ctx->chk_lcd        = chk_lcd;
  ctx->dd_lcd_bus     = dd_lcd_bus;
  g_signal_connect(dd_lcd_bus, "notify::selected",
      G_CALLBACK(on_lcd_bus_changed), ctx);
  ctx->spn_lcd_cols   = spn_lcd_cols;
  ctx->spn_lcd_rows   = spn_lcd_rows;
  ctx->chk_lcd_strict = chk_lcd_strict;
  ctx->e_lcd_rs       = e_lcd_rs;
  ctx->e_lcd_rw       = e_lcd_rw;
  ctx->e_lcd_e        = e_lcd_e;
  for (int _i = 0; _i < 8; _i++)
    ctx->e_lcd_d[_i] = e_lcd_d[_i];
  ctx->lcd_d03_row    = lcd_d03;
  /* SD card */
  ctx->chk_sd         = NULL;  /* set below in SD section */
  ctx->chk_sd_strict  = NULL;
  ctx->e_sd_sck       = NULL;
  ctx->e_sd_mosi      = NULL;
  ctx->e_sd_miso      = NULL;
  ctx->e_sd_cs        = NULL;
  ctx->e_sd_image     = NULL;
  ctx->chk_stp_db     = chk_stp_db;
  /* Pull SD widgets from dialog data store */
  ctx->chk_sd        = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "sd_chk");
  ctx->chk_sd_strict = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "sd_strict");
  ctx->e_sd_sck      = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "sd_sck");
  ctx->e_sd_mosi     = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "sd_mosi");
  ctx->e_sd_miso     = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "sd_miso");
  ctx->e_sd_cs       = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "sd_cs");
  ctx->e_sd_image    = (GtkWidget *)g_object_get_data(G_OBJECT(dlg), "sd_img");
  g_object_set_data_full(G_OBJECT(dlg), "edctx", ctx, g_free);

  /* Helper to read all fields into profile */
  void (*read_fields)(EdCtx *ctx) = NULL; /* defined via lambda below */

#define READ_ENTRY_HEX(fld, widget)                                            \
  fld = (uint16_t)strtoul(                                                     \
      gtk_entry_buffer_get_text(gtk_entry_get_buffer(GTK_ENTRY(widget))),      \
      NULL, 16)
#define READ_CHECK(fld, widget)                                                \
  fld = gtk_check_button_get_active(GTK_CHECK_BUTTON(widget))

  /* Cancel */
  g_signal_connect_swapped(btn_cancel, "clicked",
                           G_CALLBACK(gtk_window_destroy), dlg);
  g_signal_connect(btn_new_ed, "clicked", G_CALLBACK(on_profile_new), w);

  g_signal_connect(btn_save, "clicked", G_CALLBACK(on_profile_ed_save), dlg);
  g_signal_connect(btn_apply, "clicked", G_CALLBACK(on_profile_ed_apply), dlg);
  g_signal_connect(btn_default, "clicked", G_CALLBACK(on_profile_ed_default),
                   dlg);

  gtk_window_present(GTK_WINDOW(dlg));
}

/* ---------------------------------------------------------------------------
 * sim_win_create
 * ------------------------------------------------------------------------- */
sim_win_t *sim_win_create(GtkWindow *parent, const simcfg_t *cfg,
                          gboolean dark_mode) {
  install_css();

  sim_win_t *w = g_new0(sim_win_t, 1);
  w->cfg = *cfg;

  /* Set project_dir early so simprofile_find_default finds project profiles */
  if (cfg->project_dir[0])
    snprintf(w->project_dir, sizeof(w->project_dir), "%s", cfg->project_dir);

  /* --- Initialise simulator --- */
  sim_init(&w->sim, cfg->cpu_type, cfg->mem_size);
  /* Wire LCD tick callback */
  w->sim.tick_cb = lcd_tick_cb;
  w->sim.tick_ud = w;
  w->sim.speed = cfg->speed;
  w->sim.cpu.stp_on_db = cfg->stp_on_db;
  w->sim.cpu.stp_on_db = cfg->stp_on_db;
  w->sim.on_rts = cfg->on_rts;
  w->sim.on_brk = cfg->on_brk;

  /* Memory map: RAM fills everything initially */
  mem_add_ram(&w->sim.mem, 0x0000, (uint32_t)cfg->mem_size, 0, "RAM");

  /* VIAs — init only; IO registration done by sim_apply_profile */
  via_init(&w->sim.via[0], via_irq_cb, w);
  via_init(&w->sim.via[1], via_irq_cb, w);

  /* Load default profile and apply IO map */
  {
    simprofile_t prof;
    simprofile_t *found =
        simprofile_find_default(w->project_dir[0] ? w->project_dir : NULL);

    /* Override with embedded profile from source file if provided */
    if (cfg->override_profile[0]) {
      simprofile_t *ovr = calloc(1, sizeof(*ovr));
      if (simprofile_load(ovr, cfg->override_profile)) {
        free(found);
        found = ovr;
        /* Mark as embedded so UI can indicate it */
        if (found->name[0] == '\0')
          snprintf(found->name, sizeof(found->name), "[src]");
        else {
          char tmp[sizeof(found->name)];
          snprintf(tmp, sizeof(tmp), "[src] %s", found->name);
          snprintf(found->name, sizeof(found->name), "%s", tmp);
        }
        /* Delete the temp file now that it's loaded */
        remove(cfg->override_profile);
      } else {
        free(ovr);
      }
    }
    /* Override with last-used profile if it still exists on disk */
    else if (cfg->last_profile[0]) {
      simprofile_t *last = calloc(1, sizeof(*last));
      if (simprofile_load(last, cfg->last_profile)) {
        free(found);
        found = last;
      } else {
        free(last); /* file gone — stick with default */
      }
    }
    if (found) {
      prof = *found;
      free(found);
    } else
      simprofile_defaults(&prof);
    /* Override CPU type from cfg */
    if (cfg->cpu_type == CPU_65816)
      prof.cpu_type = PROFILE_CPU_65816;
    else if (cfg->cpu_type == CPU_65C02)
      prof.cpu_type = PROFILE_CPU_65C02;
    else
      prof.cpu_type = PROFILE_CPU_6502;
    /* Initialise SD core before apply so timing is set correctly */
    spi_cfg_defaults(&w->sd_ui.cfg);
    sd_init(&w->sd_ui.sd,
            (uint64_t)(cfg->speed < 14 ? (long[]){2000000,1000000,512000,
                128000,32000,8000,2000,512,128,32,8,2,1,0}[cfg->speed] : 1000000),
            false);
    sim_apply_profile(w, &prof, cfg);
  }

  /* ROM files — load binary directly into RAM backing store.
   * We deliberately leave it as RAM (writable) so the stack at $0100-$01FF
   * remains writable. The content is initialised from the binary file. */
  for (int i = 0; i < SIMCFG_MAX_ROMS; i++) {
    if (!cfg->roms[i].enabled || !cfg->roms[i].path[0])
      continue;
    uint32_t load_addr = cfg->roms[i].load_addr;
    if (load_addr >= (uint32_t)cfg->mem_size)
      continue;
    int loaded = mem_load_bin(&w->sim.mem, load_addr, cfg->roms[i].path);
    if (loaded > 0) {
      g_print("Simulator: loaded %d bytes at $%04X from %s\n", loaded,
              load_addr, cfg->roms[i].path);
    } else {
      g_warning("Simulator: failed to load ROM %s", cfg->roms[i].path);
    }
  }

  /* Banking/IO setup moved to sim_apply_profile() above */

  /* Snapshot mem.ram now that the assembled binary is fully loaded.
   * rom_range_rd uses this as the immutable ROM image for rw_state=0 reads
   * so that shadow writes to mem.ram don't corrupt ROM readback. */
  sim_take_rom_snapshot(w);

  /* Reset CPU to fetch from RESET vector */
  cpu_reset(&w->sim.cpu, mem_read, &w->sim.mem);

  /* on_rts=0: push sentinel (reset_vector-1) onto stack so that a top-level
   * RTS returns cleanly to the reset vector instead of jumping to garbage */
  if (w->sim.on_rts == 0) {
    uint16_t rv = (uint16_t)(mem_read(0xFFFC, &w->sim.mem) |
                             (mem_read(0xFFFD, &w->sim.mem) << 8));
    uint16_t sentinel = rv - 1;
    /* Push hi then lo (stack grows down) */
    mem_write(0x0100 | w->sim.cpu.SP, (sentinel >> 8) & 0xFF, &w->sim.mem);
    w->sim.cpu.SP = (w->sim.cpu.SP - 1) & 0xFF;
    mem_write(0x0100 | w->sim.cpu.SP, sentinel & 0xFF, &w->sim.mem);
    w->sim.cpu.SP = (w->sim.cpu.SP - 1) & 0xFF;
  }

  /* --- GTK window --- */
  w->window = gtk_window_new();
  gtk_window_set_title(GTK_WINDOW(w->window), "6502caEd Simulator");
  /* Apply dark/light mode to title bar */
  if (dark_mode) {
    gtk_widget_add_css_class(w->window, "dark");
    gtk_widget_remove_css_class(w->window, "light");
  } else {
    gtk_widget_add_css_class(w->window, "light");
    gtk_widget_remove_css_class(w->window, "dark");
  }
  /* Force GTK CSD title bar so our CSS colours it */
  GtkWidget *hdr = gtk_header_bar_new();
  gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hdr), TRUE);
  gtk_window_set_titlebar(GTK_WINDOW(w->window), hdr);
  /* Set title on header bar */
  gtk_header_bar_set_title_widget(GTK_HEADER_BAR(hdr),
                                  gtk_label_new("6502caEd Simulator"));
  // gtk_window_set_default_size(GTK_WINDOW(w->window), 1100, 660);
  gtk_window_set_default_size(GTK_WINDOW(w->window), 1100, 660);
  if (parent)
    gtk_window_set_transient_for(GTK_WINDOW(w->window), parent);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(w->window), TRUE);
  /* Window-level key handler for F5/F6/F7/Ctrl+R */
  GtkEventController *win_keys = gtk_event_controller_key_new();
  g_signal_connect(win_keys, "key-pressed", G_CALLBACK(on_key_press), w);
  gtk_widget_add_controller(w->window, win_keys);

  g_signal_connect(w->window, "close-request", G_CALLBACK(on_close), w);

  /* --- Main vertical box --- */
  GtkWidget *main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_window_set_child(GTK_WINDOW(w->window), main_vbox);

  /* --- Toolbar --- */
  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(toolbar, 6);
  gtk_widget_set_margin_end(toolbar, 6);
  gtk_widget_set_margin_top(toolbar, 4);
  gtk_widget_set_margin_bottom(toolbar, 4);

  w->btn_run = gtk_button_new_with_label("▶  Run (F5)");
  w->btn_stop = gtk_button_new_with_label("⏹  Stop (F6)");
  w->btn_step = gtk_button_new_with_label("⏭  Step (F7)");
  w->btn_reset = gtk_button_new_with_label("↺  Reset (Ctrl+R)");

  g_signal_connect(w->btn_run, "clicked", G_CALLBACK(on_run), w);
  g_signal_connect(w->btn_stop, "clicked", G_CALLBACK(on_stop), w);
  g_signal_connect(w->btn_step, "clicked", G_CALLBACK(on_step), w);
  g_signal_connect(w->btn_reset, "clicked", G_CALLBACK(on_reset), w);

  w->btn_reload = gtk_button_new_with_label("⟳  Reload ROM");
  gtk_widget_set_tooltip_text(
      w->btn_reload, "Reload assembled binary into simulator and reset");
  g_signal_connect(w->btn_reload, "clicked", G_CALLBACK(on_reload), w);

  /* Start paused — Stop is insensitive until running */
  gtk_widget_set_sensitive(w->btn_stop, FALSE);

  /* Speed dropdown */
  const char *spd_names[] = {"2MHz", "1MHz",   "512KHz", "128KHz", "32KHz", "8KHz",
                             "2KHz", "512Hz",  "128Hz",  "32Hz",  "8Hz",
                             "2Hz",  "1Hz",    "MAX",    NULL};
  GtkWidget *dd_speed = gtk_drop_down_new_from_strings(spd_names);
  guint spd_sel = (cfg->speed < 14) ? (guint)cfg->speed : 0;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dd_speed), spd_sel);
  g_signal_connect(dd_speed, "notify::selected", G_CALLBACK(on_speed_changed),
                   w);
  char info[64];
  /* Build initial speed label text — warn immediately if starting at fast speed */
  {
    bool fast_init = (spd_sel < (guint)SIM_SPEED_2KHZ);
    bool max_init  = (spd_sel == (guint)SIM_SPEED_MAX);
    if (fast_init || max_init)
      snprintf(info, sizeof(info), "%s  \u26a0 display reduced", spd_names[spd_sel]);
    else
      snprintf(info, sizeof(info), "%s", spd_names[spd_sel]);
  }
  GtkWidget *lbl_info = gtk_label_new(info);
  gtk_widget_set_hexpand(lbl_info, TRUE);
  gtk_widget_set_halign(lbl_info, GTK_ALIGN_END);
  w->lbl_speed_info = lbl_info;
  /* Apply correct CSS class based on initial speed */
  {
    bool fast_init = (spd_sel < (guint)SIM_SPEED_2KHZ || spd_sel == (guint)SIM_SPEED_MAX);
    if (fast_init)
      gtk_widget_add_css_class(lbl_info, "speed-warn");
    else
      gtk_widget_add_css_class(lbl_info, "reg-name");
  }

  gtk_box_append(GTK_BOX(toolbar), w->btn_run);
  gtk_box_append(GTK_BOX(toolbar), w->btn_stop);
  gtk_box_append(GTK_BOX(toolbar), w->btn_step);
  gtk_box_append(GTK_BOX(toolbar), w->btn_reset);
  gtk_box_append(GTK_BOX(toolbar), w->btn_reload);

  gtk_box_append(GTK_BOX(toolbar), gtk_separator_new(GTK_ORIENTATION_VERTICAL));

  /* Speed + info on row 1 */
  gtk_box_append(GTK_BOX(toolbar), dd_speed);
  /* Wrap speed label in a fixed-width box to prevent layout shifts
   * when warning text appears/disappears */
  /* Fixed-width container for speed label — prevents window resize when
   * warning text appears, which would shift the LCD panel horizontally */
  gtk_widget_set_hexpand(lbl_info, FALSE);
  gtk_widget_set_halign(lbl_info, GTK_ALIGN_END);
  /* Fixed character width so label never changes its natural size
   * regardless of warning text — prevents window resize */
  gtk_label_set_width_chars(GTK_LABEL(lbl_info), 28);
  gtk_label_set_max_width_chars(GTK_LABEL(lbl_info), 28);
  gtk_label_set_ellipsize(GTK_LABEL(lbl_info), PANGO_ELLIPSIZE_NONE);
  /* Spacer pushes speed label to the right without growing the window */
  GtkWidget *toolbar_spacer = gtk_label_new("");
  gtk_widget_set_hexpand(toolbar_spacer, TRUE);
  gtk_box_append(GTK_BOX(toolbar), toolbar_spacer);
  gtk_box_append(GTK_BOX(toolbar), lbl_info);

  /* Append row 1 toolbar */
  gtk_box_append(GTK_BOX(main_vbox), toolbar);
  gtk_box_append(GTK_BOX(main_vbox),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* --- Row 2 toolbar: Profile, CPU, RTS, BRK --- */
  GtkWidget *toolbar2 = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_start(toolbar2, 6);
  gtk_widget_set_margin_end(toolbar2, 6);
  gtk_widget_set_margin_top(toolbar2, 2);
  gtk_widget_set_margin_bottom(toolbar2, 2);

  /* Profile selector — ⚙ opens editor for current profile */
  {
    GtkWidget *lbl_prof = gtk_label_new("Profile:");
    gtk_widget_add_css_class(lbl_prof, "reg-name");
    w->dd_profile =
        gtk_drop_down_new_from_strings((const char *[]){"Default", NULL});
    gtk_widget_set_tooltip_text(
        w->dd_profile,
        "Memory map profile — controls ROM, IO and banking addresses");
    g_signal_connect(w->dd_profile, "notify::selected",
                     G_CALLBACK(on_profile_changed), w);
    GtkWidget *btn_edit_prof = gtk_button_new_with_label("⚙");
    gtk_widget_set_tooltip_text(btn_edit_prof, "Edit current profile");
    g_signal_connect(btn_edit_prof, "clicked", G_CALLBACK(on_profile_edit), w);
    GtkWidget *btn_new_prof = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(btn_new_prof, "New profile");
    g_signal_connect(btn_new_prof, "clicked", G_CALLBACK(on_profile_new), w);
    GtkWidget *btn_del_prof = gtk_button_new_with_label("🗑");
    gtk_widget_set_tooltip_text(btn_del_prof, "Delete current profile");
    g_signal_connect(btn_del_prof, "clicked", G_CALLBACK(on_profile_delete), w);
    gtk_box_append(GTK_BOX(toolbar2), lbl_prof);
    gtk_box_append(GTK_BOX(toolbar2), w->dd_profile);
    gtk_box_append(GTK_BOX(toolbar2), btn_edit_prof);
    gtk_box_append(GTK_BOX(toolbar2), btn_new_prof);
    gtk_box_append(GTK_BOX(toolbar2), btn_del_prof);
    /* STP on $DB checkbox */
    GtkWidget *chk_stp = gtk_check_button_new_with_label("$DB=STP");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_stp), w->cfg.stp_on_db);
    gtk_widget_set_tooltip_text(chk_stp,
        "Treat opcode $DB as STP (halt) on 6502\n"
        "Allows use of STP as halt on all CPU types");
    g_signal_connect(chk_stp, "toggled", G_CALLBACK(on_stp_db_toggled), w);
    gtk_box_append(GTK_BOX(toolbar2), chk_stp);
    gtk_box_append(GTK_BOX(toolbar2),
                   gtk_separator_new(GTK_ORIENTATION_VERTICAL));
  }

  /* CPU type */
  GtkWidget *lbl_cpu = gtk_label_new("CPU:");
  gtk_widget_add_css_class(lbl_cpu, "reg-name");
  const char *cpu_opts[] = {"6502", "65C02", "65816", NULL};
  w->dd_cpu = gtk_drop_down_new_from_strings(cpu_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(w->dd_cpu), (guint)cfg->cpu_type);
  gtk_widget_set_tooltip_text(w->dd_cpu,
                              "CPU type - changing resets the simulator");
  g_signal_connect(w->dd_cpu, "notify::selected", G_CALLBACK(on_cpu_changed),
                   w);
  gtk_box_append(GTK_BOX(toolbar2), lbl_cpu);
  gtk_box_append(GTK_BOX(toolbar2), w->dd_cpu);
  gtk_box_append(GTK_BOX(toolbar2),
                 gtk_separator_new(GTK_ORIENTATION_VERTICAL));

  /* RTS behaviour */
  GtkWidget *lbl_rts = gtk_label_new("RTS:");
  gtk_widget_add_css_class(lbl_rts, "reg-name");
  const char *rts_opts[] = {"Restart", "Halt", NULL};
  w->dd_on_rts = gtk_drop_down_new_from_strings(rts_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(w->dd_on_rts), (guint)cfg->on_rts);
  gtk_widget_set_tooltip_text(
      w->dd_on_rts, "RTS: Restart=jump to reset vector, Halt=pause simulator");
  g_signal_connect(w->dd_on_rts, "notify::selected", G_CALLBACK(on_rts_changed),
                   w);

  /* BRK behaviour */
  GtkWidget *lbl_brk = gtk_label_new("BRK:");
  gtk_widget_add_css_class(lbl_brk, "reg-name");
  const char *brk_opts[] = {"Normal", "Halt", NULL};
  w->dd_on_brk = gtk_drop_down_new_from_strings(brk_opts);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(w->dd_on_brk), (guint)cfg->on_brk);
  gtk_widget_set_tooltip_text(
      w->dd_on_brk,
      "BRK: Normal=vector $FFFE, Halt=pause simulator (software breakpoint)");
  g_signal_connect(w->dd_on_brk, "notify::selected", G_CALLBACK(on_brk_changed),
                   w);

  gtk_box_append(GTK_BOX(toolbar2), lbl_rts);
  gtk_box_append(GTK_BOX(toolbar2), w->dd_on_rts);
  gtk_box_append(GTK_BOX(toolbar2), lbl_brk);
  gtk_box_append(GTK_BOX(toolbar2), w->dd_on_brk);

  gtk_box_append(GTK_BOX(main_vbox), toolbar2);
  gtk_box_append(GTK_BOX(main_vbox),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* -----------------------------------------------------------------------
   * Main content area — 3 columns in top row, VIAs+ACIA below
   *
   *  +------------+----------+-----------+
   *  |            |  LCD     |           |
   *  |  Console   +----------+   Diag    |
   *  |            |  SDCard  |           |
   *  +------------+----------+-----------+
   *  |  VIA1      |  VIA2    |  ACIA     |
   *  +---------------------------------------+
   * ----------------------------------------------------------------------- */

  /* -----------------------------------------------------------------------
   * Full window content layout:
   *
   *  hpane_main: [left_vbox                    ] | [reg_panel (full height)]
   *               top_pane: [console] | [col_mid]
   *               separator
   *               bottom: VIA1 | VIA2 | ACIA
   *
   * reg_panel spans the full height of left_vbox (top area + VIA row).
   * ----------------------------------------------------------------------- */

  /* Build reg panel — full height right column */
  GtkWidget *reg_panel = build_reg_panel(w);
  gtk_widget_set_vexpand(reg_panel, TRUE);
  gtk_widget_set_hexpand(reg_panel, FALSE);

  /* ---- Build terminal ---- */
  w->tv = gtk_text_view_new();
  w->tbuf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(w->tv));
  gtk_text_view_set_editable(GTK_TEXT_VIEW(w->tv), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(w->tv), FALSE);
  gtk_text_view_set_monospace(GTK_TEXT_VIEW(w->tv), TRUE);
  gtk_widget_add_css_class(w->tv, "terminal");
  gtk_widget_set_focusable(GTK_WIDGET(w->tv), TRUE);

  /* Click on terminal grabs focus back to window so keys still work */
  GtkGesture *click = gtk_gesture_click_new();
  g_signal_connect_swapped(click, "pressed", G_CALLBACK(gtk_widget_grab_focus),
                           w->window);
  gtk_widget_add_controller(GTK_WIDGET(w->tv), GTK_EVENT_CONTROLLER(click));

  GtkTextIter end_iter;
  gtk_text_buffer_get_end_iter(w->tbuf, &end_iter);
  w->tend = gtk_text_buffer_create_mark(w->tbuf, "end", &end_iter, FALSE);

  GtkWidget *term_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(term_scroll),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(term_scroll), w->tv);
  gtk_widget_set_vexpand(term_scroll, TRUE);
  gtk_widget_set_hexpand(term_scroll, TRUE);

  /* ---- Build middle column: LCD + SD stacked ---- */
  GtkWidget *col_mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(col_mid, GTK_ALIGN_START);
  gtk_widget_set_hexpand(col_mid, FALSE);
  gtk_widget_set_vexpand(col_mid, FALSE);

  /* LCD panel */
  {
    GtkWidget *lcd_panel = lcd_ui_create(w);
    gtk_widget_set_hexpand(lcd_panel, FALSE);
    gtk_widget_set_visible(lcd_panel, w->lcd_ui.cfg.enabled);
    gtk_box_append(GTK_BOX(col_mid), lcd_panel);
    w->lcd_panel_widget = lcd_panel;
  }
  GtkWidget *mid_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_visible(mid_sep,
      w->lcd_ui.cfg.enabled && w->sd_ui.cfg.enabled);
  gtk_box_append(GTK_BOX(col_mid), mid_sep);
  /* SD card panel */
  {
    GtkWidget *sd_panel = sd_ui_create(w);
    gtk_widget_set_hexpand(sd_panel, FALSE);
    gtk_widget_set_visible(sd_panel, w->sd_ui.cfg.enabled);
    gtk_box_append(GTK_BOX(col_mid), sd_panel);
    w->sd_panel_widget = sd_panel;
  }
  bool mid_visible = w->lcd_ui.cfg.enabled || w->sd_ui.cfg.enabled;
  gtk_widget_set_visible(col_mid, mid_visible);
  w->bottom_col_mid = col_mid;
  w->bottom_col_sep = NULL;
  w->bottom_mid_sep = mid_sep;

  /* ---- Top pane: console (left, expands) | col_mid (right, natural) ---- */
  GtkWidget *top_pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_vexpand(top_pane, TRUE);
  gtk_widget_set_hexpand(top_pane, TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(top_pane), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(top_pane), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(top_pane), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(top_pane), FALSE);
  gtk_paned_set_start_child(GTK_PANED(top_pane), term_scroll);
  gtk_paned_set_end_child(GTK_PANED(top_pane), col_mid);

  /* ---- VIA + ACIA bottom row ---- */
  GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_widget_set_margin_start(bottom, 4);
  gtk_widget_set_margin_end(bottom, 4);
  gtk_widget_set_margin_top(bottom, 4);
  gtk_widget_set_margin_bottom(bottom, 4);
  gtk_widget_set_hexpand(bottom, TRUE);
  gtk_widget_set_vexpand(bottom, FALSE);

  if (cfg->via1_enable) {
    char vtitle[32];
    snprintf(vtitle, sizeof(vtitle), "VIA 1  ($%04X)", cfg->via1_base);
    gtk_box_append(GTK_BOX(bottom), build_via_panel(w, 0, vtitle));
  }
  if (cfg->via2_enable) {
    char vtitle[32];
    snprintf(vtitle, sizeof(vtitle), "VIA 2  ($%04X)", cfg->via2_base);
    gtk_box_append(GTK_BOX(bottom), build_via_panel(w, 1, vtitle));
  }
  if (cfg->acia_enable)
    gtk_box_append(GTK_BOX(bottom), build_acia_panel(w));

  /* ---- Left vbox: top_pane + separator + bottom row ---- */
  GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(left_vbox, TRUE);
  gtk_widget_set_vexpand(left_vbox, TRUE);
  gtk_box_append(GTK_BOX(left_vbox), top_pane);
  gtk_box_append(GTK_BOX(left_vbox),
                 gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(left_vbox), bottom);

  /* ---- Main horizontal pane: left_vbox | reg_panel ---- */
  GtkWidget *hpane_main = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_vexpand(hpane_main, TRUE);
  gtk_widget_set_hexpand(hpane_main, TRUE);
  gtk_paned_set_resize_start_child(GTK_PANED(hpane_main), TRUE);
  gtk_paned_set_resize_end_child(GTK_PANED(hpane_main), FALSE);
  gtk_paned_set_shrink_start_child(GTK_PANED(hpane_main), FALSE);
  gtk_paned_set_shrink_end_child(GTK_PANED(hpane_main), FALSE);
  gtk_paned_set_start_child(GTK_PANED(hpane_main), left_vbox);
  gtk_paned_set_end_child(GTK_PANED(hpane_main), reg_panel);

  gtk_box_append(GTK_BOX(main_vbox), hpane_main);

  /* --- Start emulator thread (paused) --- */
  sim_start(&w->sim);

  /* --- UI refresh timer --- */
  w->update_timer = g_timeout_add(UPDATE_INTERVAL_MS, sim_win_update, w);

  gtk_window_present(GTK_WINDOW(w->window));

  /* Populate profile dropdown now window exists */
  profile_dd_rebuild(w);

  return w;
}

GtkWidget *sim_win_get_window(sim_win_t *w) { return w->window; }
void sim_win_set_app_state(sim_win_t *w, void *app_state) {
  w->app_state = app_state;
}
void sim_win_set_project_dir(sim_win_t *w, const char *dir) {
  if (dir)
    snprintf(w->project_dir, sizeof(w->project_dir), "%s", dir);
  else
    w->project_dir[0] = 0;
  /* Rebuild profile dropdown now we know the project */
  profile_dd_rebuild(w);
}

void sim_win_destroy(sim_win_t *w) {
  if (!w)
    return;
  if (w->update_timer)
    g_source_remove(w->update_timer);
  sim_stop(&w->sim);
  sim_free(&w->sim);
  acia_tcp_close(&w->sim.acia);
  gtk_window_destroy(GTK_WINDOW(w->window));
  g_free(w);
}

void sim_win_send_key(sim_win_t *w, char ch) { sim_term_write(&w->sim, ch); }
