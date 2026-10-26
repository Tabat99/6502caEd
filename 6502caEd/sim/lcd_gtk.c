/* lcd_gtk.c — GTK4 rendering for HD44780 LCD emulation */
#include "lcd_gtk.h"
#include "sim_gtk.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Drawing constants
 * ------------------------------------------------------------------------- */
#define LCD_PIX_W     2    /* pixels per LCD dot (embedded) */
#define LCD_PIX_H     3    /* pixels per LCD dot (embedded) */
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
        /* An eight-wire connection can run the controller in 4-bit mode.
         * lcd_strobe expects a nibble in bits 0..3 in that mode. Select
         * physical D4..D7, rather than mistaking D0..D3 for the nibble.
         * Decide before the strobe: this transfer may change bus_8bit. */
        if (!rw && ui->cfg.bus_width == 8 && !ui->lcd.bus_8bit)
            data >>= 4;

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
    (void)width; (void)height;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    lcd_ui_t *ui = &w->lcd_ui;
    /* Render a coherent snapshot while the CPU updates the live LCD. */
    pthread_mutex_lock(&w->sim.state_lock);
    lcd_t snapshot = ui->lcd;
    bool enabled = ui->cfg.enabled;
    pthread_mutex_unlock(&w->sim.state_lock);
    lcd_t *lcd = &snapshot;
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

    /* Background */
    /* Visual status cue, not a model of a physical backlight circuit. */
    bool lit = enabled && lcd->initialised;
    double light = lit ? 1.0 : 0.28;
    cairo_set_source_rgb(cr, LCD_BG_R * light, LCD_BG_G * light,
                         LCD_BG_B * light);
    cairo_rectangle(cr, 0, 0, total_w, total_h);
    cairo_fill(cr);

    if (!enabled) {
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

                    double r = (on ? LCD_ON_R : LCD_OFF_R) * light;
                    double g = (on ? LCD_ON_G : LCD_OFF_G) * light;
                    double b = (on ? LCD_ON_B : LCD_OFF_B) * light;

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
GtkWidget *lcd_ui_create(struct sim_win_t *w) {
    lcd_ui_t *ui = &w->lcd_ui;
    lcd_cfg_t *cfg = &ui->cfg;

    /* Initialise LCD core */
    lcd_init(&ui->lcd, cfg->cols, cfg->rows, 1000000);
    lcd_set_strict(&ui->lcd, cfg->strict);
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
    snprintf(mode_str, sizeof(mode_str), "%dx%d  %d-bit  %s",
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
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(da), tw);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(da), th);
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
    ui->e_last = false;

    /* Update mode label */
    if (ui->lbl_mode) {
        char mode_str[64];
        snprintf(mode_str, sizeof(mode_str), "%dx%d  %d-bit  %s",
                 cfg->cols, cfg->rows, cfg->bus_width,
                 cfg->strict ? "Strict" : "Permissive");
        gtk_label_set_text(GTK_LABEL(ui->lbl_mode), mode_str);
    }
}

void lcd_ui_set_cpu_hz(struct sim_win_t *w, uint64_t cpu_hz) {
    lcd_set_cpu_hz(&w->lcd_ui.lcd, cpu_hz);
}
