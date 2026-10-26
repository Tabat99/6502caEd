/* sd_gtk.c — GTK4 SD card emulation panel */
#include "sd_gtk.h"
#include "sim_gtk.h"
#include "via.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* -------------------------------------------------------------------------
 * Pin reading helpers (same pattern as lcd_gtk.c)
 * ---------------------------------------------------------------------- */

static int read_spi_pin(struct sim_win_t *w, const spi_pin_t *pin) {
    via_t *v = &w->sim.via[pin->via - 1];
    uint8_t out = (pin->port == 0) ? via_get_pa_out(v) : via_get_pb_out(v);
    return (out >> pin->bit) & 1;
}

static void write_spi_pin(struct sim_win_t *w, const spi_pin_t *pin, int val) {
    via_t *v = &w->sim.via[pin->via - 1];
    uint8_t mask = (uint8_t)(1 << pin->bit);
    uint8_t cur  = (pin->port == 0) ? v->PA_in : v->PB_in;
    if (val) cur |= mask; else cur &= ~mask;
    if (pin->port == 0) via_set_pa(v, cur);
    else                via_set_pb(v, cur);
}

/* -------------------------------------------------------------------------
 * Diagnostics — print SPI bytes to terminal when enabled
 * ---------------------------------------------------------------------- */

static bool sd_diag_enabled = false;  /* toggle with SD diag checkbox */

static FILE *sd_diag_fp = NULL;  /* log file, NULL = stderr */

static void sd_diag_push(struct sim_win_t *w, const char *s) {
    (void)w;
    FILE *f = sd_diag_fp ? sd_diag_fp : stderr;
    fputs(s, f);
    fflush(f);
}

static void sd_diag_byte(struct sim_win_t *w, const char *prefix,
                         uint8_t mosi_byte, uint8_t miso_byte) {
    if (!sd_diag_enabled) return;
    char buf[32];
    snprintf(buf, sizeof(buf), "%s MOSI:%02X MISO:%02X\n",
             prefix, mosi_byte, miso_byte);
    sd_diag_push(w, buf);
}

/* -------------------------------------------------------------------------
 * VIA change handler — called every VIA write
 * ---------------------------------------------------------------------- */

void sd_ui_via_changed(struct sim_win_t *w, int via_idx) {
    sd_ui_t *ui = &w->sd_ui;

    if (!ui->cfg.enabled) return;

    /* If no card and diagnostics on, still show pin state */
    spi_cfg_t *c = &ui->cfg;
    bool relevant = (c->sck.via  - 1 == via_idx) ||
                    (c->mosi.via - 1 == via_idx) ||
                    (c->cs.via   - 1 == via_idx);
    if (!relevant) return;

    bool cs   = read_spi_pin(w, &c->cs)   != 0;
    bool sck  = read_spi_pin(w, &c->sck)  != 0;
    bool mosi = read_spi_pin(w, &c->mosi) != 0;

    if (!ui->sd.fp) {
        /* No card — drive MISO high, log state if diag on */
        if (sd_diag_enabled) {
            static bool last_cs2 = true;
            static int  no_card_sck_count = 0;
            if (cs != last_cs2) {
                char buf[64];
                snprintf(buf, sizeof(buf),
                         "[SD] NO CARD  CS=%d->%d\n", last_cs2, cs);
                sd_diag_push(w, buf);
                last_cs2 = cs;
                no_card_sck_count = 0;
            } else if (!cs) {
                /* CS selected but no card — count SCK pulses */
                bool sck_last2 = ui->sd.sck_last;
                if (!sck_last2 && sck) {
                    no_card_sck_count++;
                    if (no_card_sck_count <= 16 || (no_card_sck_count % 8)==0) {
                        char buf[48];
                        snprintf(buf, sizeof(buf),
                                 "[SD] NO CARD  SCK pulse #%d\n",
                                 no_card_sck_count);
                        sd_diag_push(w, buf);
                    }
                }
            }
        }
        /* Still need to track SCK for diagnostics */
        ui->sd.sck_last = sck;
        ui->sd.cs_last  = cs;
        write_spi_pin(w, &c->miso, 1);
        return;
    }

    /* Track bit count for diagnostics */
    static uint8_t diag_mosi_byte = 0;
    static uint8_t diag_miso_byte = 0;
    static int     diag_bits = 0;
    static bool    diag_cs_last = true;

    bool sck_last = ui->sd.sck_last;
    bool sck_rise = !sck_last && sck;
    bool cs_fall  = diag_cs_last && !cs;

    if (cs_fall) {
        diag_bits = 0;
        diag_mosi_byte = 0;
        diag_miso_byte = 0;
        if (sd_diag_enabled) sd_diag_push(w, "[SD] CS low\n");
    }
    if (!cs && diag_cs_last != cs)
        if (cs && sd_diag_enabled) sd_diag_push(w, "[SD] CS high\n");
    diag_cs_last = cs;

    int miso = sd_update(&ui->sd, cs, sck, mosi);
    write_spi_pin(w, &c->miso, miso);
    ui->miso_last = miso;

    /* Accumulate bits for diagnostics — limit to first 64 bytes */
    if (sd_diag_enabled && !cs && sck_rise) {
        static int diag_total_bytes = 0;
        if (cs_fall) diag_total_bytes = 0;
        diag_mosi_byte = (diag_mosi_byte << 1) | (mosi ? 1 : 0);
        diag_miso_byte = (diag_miso_byte << 1) | (miso ? 1 : 0);
        diag_bits++;
        if (diag_bits == 8) {
            if (diag_total_bytes < 64) {
                sd_diag_byte(w, "[SD]", diag_mosi_byte, diag_miso_byte);
                diag_total_bytes++;
                if (diag_total_bytes == 64) {
                    sd_diag_push(w, "[SD] ... (truncated at 64 bytes)\n");
                }
            }
            diag_bits = 0;
            diag_mosi_byte = 0;
            diag_miso_byte = 0;
        }
    }
}

/* -------------------------------------------------------------------------
 * UI callbacks
 * ---------------------------------------------------------------------- */

static void update_status_label(struct sim_win_t *w) {
    sd_ui_t *ui = &w->sd_ui;
    char buf[600];
    if (!ui->sd.fp || !ui->sd.image_path[0]) {
        snprintf(buf, sizeof(buf), "No card inserted");
    } else {
        /* Format size nicely */
        uint64_t sz = ui->sd.capacity;
        char szbuf[32];
        if      (sz >= 1024ULL*1024*1024) snprintf(szbuf,sizeof(szbuf),"%lluGB",(unsigned long long)(sz>>30));
        else if (sz >= 1024ULL*1024)      snprintf(szbuf,sizeof(szbuf),"%lluMB",(unsigned long long)(sz>>20));
        else                              snprintf(szbuf,sizeof(szbuf),"%lluKB",(unsigned long long)(sz>>10));
        /* Filename only */
        const char *slash = strrchr(ui->sd.image_path, '/');
#ifdef _WIN32
        const char *bslash = strrchr(ui->sd.image_path, '\\');
        if (bslash > slash) slash = bslash;
#endif
        const char *fname = slash ? slash + 1 : ui->sd.image_path;
        snprintf(buf, sizeof(buf), "Inserted: %s  (%s%s%s)",
                 fname, szbuf, ui->sd.is_sdhc ? " SDHC" : " SDSC",
                 ui->sd.read_only ? ", read-only" : "");
    }
    if (ui->lbl_status)
        gtk_label_set_text(GTK_LABEL(ui->lbl_status), buf);
    if (ui->btn_eject)
        gtk_widget_set_sensitive(ui->btn_eject,     ui->sd.fp != NULL);
    if (ui->btn_duplicate)
        gtk_widget_set_sensitive(ui->btn_duplicate, ui->sd.fp != NULL);
}

/* File dialog response handlers as static functions */
/* Keep the editable profile consistent with explicit Insert/Eject actions.
 * Saving the profile remains an explicit user action. */
static void remember_sd_image(struct sim_win_t *w) {
    snprintf(w->sd_ui.cfg.image_path,sizeof(w->sd_ui.cfg.image_path),"%s",w->sd_ui.sd.image_path);
    w->sd_ui.cfg.image_specified = true;
    w->profile.sd = w->sd_ui.cfg;
}

static void attach_response_cb(GObject *src, GAsyncResult *res, gpointer ud) {
    struct sim_win_t *w = (struct sim_win_t *)ud;
    GtkFileDialog *dlg = GTK_FILE_DIALOG(src);
    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish(dlg, res, &err);
    if (!file) { if (err) g_error_free(err); return; }
    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (path) {
        if (sd_open(&w->sd_ui.sd, path)) {
            remember_sd_image(w);
            update_status_label(w);
        } else {
            GtkAlertDialog *ad = gtk_alert_dialog_new(
                "Failed to open image file:\n%s", path);
            gtk_alert_dialog_show(ad, GTK_WINDOW(w->window));
            g_object_unref(ad);
        }
        g_free(path);
    }
}

static void on_sd_diag_toggled(GtkCheckButton *chk, gpointer ud) {
    struct sim_win_t *w = (struct sim_win_t *)ud;
    sd_diag_enabled = gtk_check_button_get_active(chk);
    if (sd_diag_enabled) {
        /* Open log file in home dir */
        char path[512];
        snprintf(path, sizeof(path), "%s/sd_diag.log", g_get_home_dir());
        sd_diag_fp = fopen(path, "w");
        if (!sd_diag_fp) sd_diag_fp = NULL; /* fall back to stderr */
        FILE *f = sd_diag_fp ? sd_diag_fp : stderr;
        if (w) {
            sd_ui_t *ui = &w->sd_ui;
            fprintf(f, "[SD] Diag ON  enabled=%d fp=%s cfg.enabled=%d\n",
                    ui->sd.enabled,
                    ui->sd.fp ? ui->sd.image_path : "NULL",
                    ui->cfg.enabled);
            fflush(f);
        }
        /* Print log path to stderr so user knows where to look */
        fprintf(stderr, "[SD] Logging to: %s\n",
                sd_diag_fp ? path : "stderr");
        fflush(stderr);
    } else {
        if (sd_diag_fp) { fclose(sd_diag_fp); sd_diag_fp = NULL; }
        fprintf(stderr, "[SD] Diag OFF\n");
        fflush(stderr);
    }
}

static void on_attach_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Attach SD Card Image");
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "Disk images");
    gtk_file_filter_add_pattern(ff, "*.img");
    gtk_file_filter_add_pattern(ff, "*.bin");
    gtk_file_filter_add_pattern(ff, "*.sd");
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, ff);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(w->window), NULL,
                         attach_response_cb, w);
    g_object_unref(ff);
    g_object_unref(filters);
    g_object_unref(dlg);
}

static void on_eject_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    sd_close(&w->sd_ui.sd);
    remember_sd_image(w);
    update_status_label(w);
}

/* Create dialog state */
typedef struct {
    struct sim_win_t *w;
    GtkWidget *entry_path;
    GtkWidget *dd_size;
    GtkWidget *dd_format;
    GtkWidget *entry_label;
    GtkWidget *lbl_format;
    GtkWidget *btn_ok;
    GtkWidget *dialog;
} CreateCtx;

static void create_selection_changed(GObject *object, GParamSpec *pspec, gpointer ud) {
    (void)object; (void)pspec;
    CreateCtx *ctx = ud;
    guint size = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dd_size));
    guint fmt = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dd_format));
    bool valid = size < SD_SIZE_COUNT && fmt <= SD_IMAGE_FAT32 &&
        sd_image_format_supported(sd_size_bytes((sd_size_id_t)size), (sd_image_format_t)fmt);
    uint64_t bytes = size < SD_SIZE_COUNT ? sd_size_bytes((sd_size_id_t)size) : 0;
    bool card_ok = !(ctx->w->sd_ui.cfg.card_mode == SD_CARD_SDSC && bytes > 2ULL*1024*1024*1024) &&
                   !(ctx->w->sd_ui.cfg.card_mode == SD_CARD_SDHC && bytes % (512*1024));
    const char *message = !card_ok ? "Size is incompatible with the configured card type." :
        !valid ? "Choose another size for this filesystem." :
        fmt == SD_IMAGE_RAW ? "Blank raw sectors; no filesystem." :
        "Empty formatted volume; no partition table. Existing files are never overwritten.";
    gtk_label_set_text(GTK_LABEL(ctx->lbl_format), message);
    gtk_widget_set_sensitive(ctx->entry_label, fmt != SD_IMAGE_RAW);
    gtk_widget_set_sensitive(ctx->btn_ok, valid && card_ok);
}

static void create_ok_cb(GtkButton *btn, gpointer ud) {
    (void)btn;
    CreateCtx *ctx = (CreateCtx *)ud;
    struct sim_win_t *w = ctx->w;
    const char *path = gtk_entry_buffer_get_text(
        gtk_entry_get_buffer(GTK_ENTRY(ctx->entry_path)));
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dd_size));
    if (sel >= SD_SIZE_COUNT) sel = SD_SIZE_2GB;

    uint64_t sz = sd_size_bytes((sd_size_id_t)sel);
    if (!path || !path[0]) {
        GtkAlertDialog *ad = gtk_alert_dialog_new("Please enter a file path.");
        gtk_alert_dialog_show(ad, GTK_WINDOW(w->window));
        g_object_unref(ad);
        return;
    }
    guint fmt = gtk_drop_down_get_selected(GTK_DROP_DOWN(ctx->dd_format));
    const char *label = gtk_entry_buffer_get_text(
        gtk_entry_get_buffer(GTK_ENTRY(ctx->entry_label)));
    if (!sd_create_formatted(path, sz, (sd_image_format_t)fmt,
                              fmt == SD_IMAGE_RAW ? "NO NAME" : label)) {
        GtkAlertDialog *ad = gtk_alert_dialog_new(
            "Failed to create image:\n%s\n\nUse a new filename, a compatible size, and a volume label of at most 11 ASCII characters without punctuation such as / : * ?. Check disk space and folder permissions.", path);
        gtk_alert_dialog_show(ad, GTK_WINDOW(w->window));
        g_object_unref(ad);
        return;
    }
    if (sd_open(&w->sd_ui.sd, path)) {
        remember_sd_image(w);
        update_status_label(w);
    } else {
        GtkAlertDialog *ad = gtk_alert_dialog_new(
            "Image was created, but could not be attached:\n%s\n\nCheck the selected card type and image permissions.", path);
        gtk_alert_dialog_show(ad, GTK_WINDOW(w->window));
        g_object_unref(ad);
    }
    gtk_window_close(GTK_WINDOW(ctx->dialog));
}

static void on_create_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    CreateCtx *ctx = g_new0(CreateCtx, 1);
    ctx->w = w;

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Create SD Card Image");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(w->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 420, -1);
    ctx->dialog = dlg;

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    /* Path entry */
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Image file path:"));
    GtkWidget *entry = gtk_entry_new();
    /* Default path in home dir */
    char def_path[512];
    snprintf(def_path, sizeof(def_path), "%s/sdcard.img", g_get_home_dir());
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(entry)),
                              def_path, -1);
    ctx->entry_path = entry;
    gtk_box_append(GTK_BOX(vbox), entry);

    /* Size dropdown */
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Card size:"));
    const char *size_names[SD_SIZE_COUNT + 1];
    for (int i = 0; i < SD_SIZE_COUNT; i++) size_names[i] = sd_size_label(i);
    size_names[SD_SIZE_COUNT] = NULL;
    GtkWidget *dd = gtk_drop_down_new_from_strings(size_names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), SD_SIZE_32MB); /* default 32MB */
    ctx->dd_size = dd;
    gtk_box_append(GTK_BOX(vbox), dd);

    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Format:"));
    const char *format_names[] = {"Raw image", "FAT12", "FAT16", "FAT32", NULL};
    ctx->dd_format = gtk_drop_down_new_from_strings(format_names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(ctx->dd_format), SD_IMAGE_FAT16);
    gtk_box_append(GTK_BOX(vbox), ctx->dd_format);
    gtk_box_append(GTK_BOX(vbox), gtk_label_new("Volume label (up to 11 characters):"));
    ctx->entry_label = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(ctx->entry_label), 11);
    gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(ctx->entry_label)), "6502CAED", -1);
    gtk_box_append(GTK_BOX(vbox), ctx->entry_label);
    ctx->lbl_format = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(ctx->lbl_format), TRUE);
    gtk_box_append(GTK_BOX(vbox), ctx->lbl_format);

    /* Warning */
    GtkWidget *warn = gtk_label_new(
        "⚠ Large images (≥1GB) may take a while to create.");
    gtk_widget_add_css_class(warn, "dim-label");
    gtk_box_append(GTK_BOX(vbox), warn);

    /* Buttons */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(hbox, GTK_ALIGN_END);
    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *btn_ok     = gtk_button_new_with_label("Create & Attach");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    ctx->btn_ok = btn_ok;
    g_signal_connect(ctx->dd_size, "notify::selected", G_CALLBACK(create_selection_changed), ctx);
    g_signal_connect(ctx->dd_format, "notify::selected", G_CALLBACK(create_selection_changed), ctx);
    create_selection_changed(NULL, NULL, ctx);
    g_signal_connect_swapped(btn_cancel, "clicked",
                             G_CALLBACK(gtk_window_close), dlg);
    g_signal_connect(btn_ok, "clicked", G_CALLBACK(create_ok_cb), ctx);
    gtk_box_append(GTK_BOX(hbox), btn_cancel);
    gtk_box_append(GTK_BOX(hbox), btn_ok);
    gtk_box_append(GTK_BOX(vbox), hbox);

    g_object_set_data_full(G_OBJECT(dlg), "ctx", ctx, g_free);
    gtk_window_present(GTK_WINDOW(dlg));
}

static void duplicate_save_cb(GObject *src, GAsyncResult *res, gpointer ud) {
    struct sim_win_t *w = (struct sim_win_t *)ud;
    GtkFileDialog *dlg = GTK_FILE_DIALOG(src);
    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish(dlg, res, &err);
    if (!file) { if (err) g_error_free(err); return; }
    char *dest = g_file_get_path(file);
    g_object_unref(file);
    if (!dest) return;

    /* Copy the image file */
    FILE *src_f = fopen(w->sd_ui.sd.image_path, "rb");
    FILE *dst_f = fopen(dest, "wb");
    if (src_f && dst_f) {
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src_f)) > 0)
            fwrite(buf, 1, n, dst_f);
    }
    if (src_f) fclose(src_f);
    if (dst_f) fclose(dst_f);
    g_free(dest);
}

static void on_duplicate_clicked(GtkButton *btn, gpointer ud) {
    (void)btn;
    struct sim_win_t *w = (struct sim_win_t *)ud;
    if (!w->sd_ui.sd.fp) return;

    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "Save Duplicate Image As");
    gtk_file_dialog_set_initial_name(dlg, "sdcard_copy.img");
    gtk_file_dialog_save(dlg, GTK_WINDOW(w->window), NULL,
                         duplicate_save_cb, w);
    g_object_unref(dlg);
}

static void timing_sync_widgets(struct sim_win_t *w) {
    sd_ui_t *ui = &w->sd_ui;
    const sd_timing_t *t = &ui->cfg.timing;
    double values[] = {t->read_us, t->write_us, t->erase_us, t->max_spi_hz};
    ui->timing_sync = true;
    for (int i = 0; i < 4; ++i)
        if (ui->timing_spin[i])
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->timing_spin[i]), values[i]);
    ui->timing_sync = false;
}

static void timing_changed(GtkSpinButton *spin, gpointer ud) {
    (void)spin;
    struct sim_win_t *w = ud;
    sd_ui_t *ui = &w->sd_ui;
    if (ui->timing_sync) return;
    sd_timing_t t = {
        (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->timing_spin[0])),
        (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->timing_spin[1])),
        (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->timing_spin[2])),
        (uint32_t)gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->timing_spin[3]))
    };
    sd_set_timing(&ui->sd, &t);
    ui->cfg.timing = ui->sd.timing;
    w->profile.sd.timing = ui->cfg.timing;
}

static void timing_defaults(GtkButton *button, gpointer ud) {
    (void)button;
    struct sim_win_t *w = ud;
    w->sd_ui.cfg.timing = (sd_timing_t)SD_TIMING_DEFAULTS;
    sd_set_timing(&w->sd_ui.sd, &w->sd_ui.cfg.timing);
    w->profile.sd.timing = w->sd_ui.cfg.timing;
    timing_sync_widgets(w);
}

static void on_strict_toggled(GtkCheckButton *chk, gpointer ud) {
    struct sim_win_t *w = (struct sim_win_t *)ud;
    bool strict = gtk_check_button_get_active(chk);
    w->sd_ui.cfg.strict = strict;
    w->profile.sd.strict = strict;
    w->sd_ui.sd.strict  = strict;
}

/* -------------------------------------------------------------------------
 * Panel creation
 * ---------------------------------------------------------------------- */

GtkWidget *sd_ui_create(struct sim_win_t *w) {
    sd_ui_t *ui = &w->sd_ui;

    GtkWidget *panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_add_css_class(panel, "sim-section");
    ui->panel = panel;

    /* Header row */
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    GtkWidget *title = gtk_label_new("SD Card (SPI)");
    gtk_widget_add_css_class(title, "sim-section-title");
    gtk_box_append(GTK_BOX(hdr), title);

    GtkWidget *chk_strict = gtk_check_button_new_with_label("Strict timing");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_strict), ui->cfg.strict);
    g_signal_connect(chk_strict, "toggled", G_CALLBACK(on_strict_toggled), w);
    gtk_box_append(GTK_BOX(hdr), chk_strict);
    ui->btn_strict = chk_strict;

    gtk_box_append(GTK_BOX(panel), hdr);

    GtkWidget *expander = gtk_expander_new("Card timing");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    const char *labels[] = {"Read latency (µs)", "Write busy (µs)",
                            "Erase busy (µs)", "Maximum SPI (Hz; 0 disables limit)"};
    double limits[] = {1000000, 1000000, 10000000, 25000000};
    for (int i = 0; i < 4; ++i) {
        gtk_grid_attach(GTK_GRID(grid), gtk_label_new(labels[i]), 0, i, 1, 1);
        ui->timing_spin[i] = gtk_spin_button_new_with_range(0, limits[i], 1);
        gtk_grid_attach(GTK_GRID(grid), ui->timing_spin[i], 1, i, 1, 1);
    }
    timing_sync_widgets(w);
    for (int i = 0; i < 4; ++i)
        g_signal_connect(ui->timing_spin[i], "value-changed", G_CALLBACK(timing_changed), w);
    GtkWidget *defaults = gtk_button_new_with_label("Defaults");
    g_signal_connect(defaults, "clicked", G_CALLBACK(timing_defaults), w);
    gtk_grid_attach(GTK_GRID(grid), defaults, 1, 4, 1, 1);
    ui->lbl_clock = gtk_label_new("SPI clock violations: 0");
    gtk_grid_attach(GTK_GRID(grid), ui->lbl_clock, 0, 5, 2, 1);
    gtk_widget_set_tooltip_text(expander,
        "Representative presets, not measured card averages. Enforced with Strict timing. "
        "Initialisation is limited to 400 kHz. Save through profile settings; "
        "for embedded profiles also update the source. Change timing while stopped.");
    gtk_expander_set_child(GTK_EXPANDER(expander), grid);
    gtk_box_append(GTK_BOX(panel), expander);

    /* Status label */
    ui->lbl_status = gtk_label_new("No card inserted");
    gtk_widget_set_halign(ui->lbl_status, GTK_ALIGN_START);
    gtk_widget_add_css_class(ui->lbl_status, "sim-reg");
    gtk_box_append(GTK_BOX(panel), ui->lbl_status);

    /* Pin mapping row */
    GtkWidget *pins_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    const char *pin_labels[] = {"SCK:", "MOSI:", "MISO:", "CS:"};
    spi_pin_t  *pins[] = {
        &ui->cfg.sck, &ui->cfg.mosi, &ui->cfg.miso, &ui->cfg.cs
    };
    for (int i = 0; i < 4; i++) {
        gtk_box_append(GTK_BOX(pins_box), gtk_label_new(pin_labels[i]));
        char pinbuf[8];
        spi_pin_format(pins[i], pinbuf, sizeof(pinbuf));
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(entry), 3);
        gtk_widget_set_size_request(entry, 48, -1);
        gtk_entry_buffer_set_text(gtk_entry_get_buffer(GTK_ENTRY(entry)),
                                  pinbuf, -1);
        gtk_box_append(GTK_BOX(pins_box), entry);
    }
    gtk_box_append(GTK_BOX(panel), pins_box);

    /* Action buttons row */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    ui->btn_attach    = gtk_button_new_with_label("Attach…");
    ui->btn_eject     = gtk_button_new_with_label("Eject");
    ui->btn_create    = gtk_button_new_with_label("Create…");
    ui->btn_duplicate = gtk_button_new_with_label("Duplicate…");

    gtk_widget_set_sensitive(ui->btn_eject,     FALSE);
    gtk_widget_set_sensitive(ui->btn_duplicate, FALSE);

    g_signal_connect(ui->btn_attach,    "clicked", G_CALLBACK(on_attach_clicked),  w);
    g_signal_connect(ui->btn_eject,     "clicked", G_CALLBACK(on_eject_clicked),    w);
    g_signal_connect(ui->btn_create,    "clicked", G_CALLBACK(on_create_clicked),   w);
    g_signal_connect(ui->btn_duplicate, "clicked", G_CALLBACK(on_duplicate_clicked),w);

    gtk_box_append(GTK_BOX(btn_box), ui->btn_attach);
    gtk_box_append(GTK_BOX(btn_box), ui->btn_eject);
    gtk_box_append(GTK_BOX(btn_box), ui->btn_create);
    gtk_box_append(GTK_BOX(btn_box), ui->btn_duplicate);
    gtk_box_append(GTK_BOX(panel), btn_box);

    /* MISO indicator + SPI diagnostics toggle */
    GtkWidget *miso_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui->lbl_miso = gtk_label_new("MISO: 1");
    gtk_widget_set_halign(ui->lbl_miso, GTK_ALIGN_START);
    gtk_widget_add_css_class(ui->lbl_miso, "sim-reg");
    gtk_box_append(GTK_BOX(miso_row), ui->lbl_miso);

    GtkWidget *chk_diag = gtk_check_button_new_with_label("SPI diag");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_diag), FALSE);
    g_signal_connect(chk_diag, "toggled", G_CALLBACK(on_sd_diag_toggled), w);
    gtk_box_append(GTK_BOX(miso_row), chk_diag);
    gtk_box_append(GTK_BOX(panel), miso_row);

    update_status_label(w);
    return panel;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void sd_ui_update(struct sim_win_t *w) {
    sd_ui_t *ui = &w->sd_ui;
    if (!ui->panel) return;
    if (ui->lbl_clock) {
        char text[96];
        snprintf(text, sizeof(text), "SPI clock violations: %llu%s",
            (unsigned long long)ui->sd.clock_violations,
            ui->sd.clock_fault ? " (release CS or reset)" : "");
        gtk_label_set_text(GTK_LABEL(ui->lbl_clock), text);
    }
    int miso = sd_miso(&ui->sd);
    if (miso != ui->miso_last) {
        char buf[16];
        snprintf(buf, sizeof(buf), "MISO: %d", miso);
        gtk_label_set_text(GTK_LABEL(ui->lbl_miso), buf);
        ui->miso_last = miso;
    }
}

void sd_ui_apply_cfg(struct sim_win_t *w, const spi_cfg_t *cfg) {
    sd_ui_t *ui = &w->sd_ui;
    char mounted_path[sizeof(ui->sd.image_path)];
    g_strlcpy(mounted_path, ui->sd.image_path, sizeof mounted_path);
    int old_mode = ui->sd.card_mode;
    bool old_read_only = ui->sd.read_only;
    ui->cfg = *cfg;
    sd_set_timing(&ui->sd, &cfg->timing);
    timing_sync_widgets(w);
    ui->sd.strict = cfg->strict;
    ui->sd.enabled = cfg->enabled;
    ui->sd.card_mode = (int)cfg->card_mode;
    ui->sd.read_only = cfg->read_only;
    const char *path = cfg->image_specified || cfg->image_path[0] ? cfg->image_path : mounted_path;
    if (cfg->image_specified && !path[0]) sd_close(&ui->sd);
    if (path[0] && (!ui->sd.fp || old_mode != ui->sd.card_mode ||
                    old_read_only != cfg->read_only ||
                    strcmp(path, mounted_path) != 0)) {
        if (!sd_open(&ui->sd, path))
            g_warning("SD profile: unable to open image: %s", path);
    }
    if (ui->btn_strict)
        gtk_check_button_set_active(GTK_CHECK_BUTTON(ui->btn_strict),
                                    cfg->strict);
    if (ui->panel)
        gtk_widget_set_visible(ui->panel, cfg->enabled);
    /* Update middle column visibility */
    if (w->bottom_col_mid) {
        bool mid = cfg->enabled || w->lcd_ui.cfg.enabled;
        gtk_widget_set_visible(w->bottom_col_mid, mid);
        if (w->bottom_col_sep) gtk_widget_set_visible(w->bottom_col_sep, mid);
        if (w->bottom_mid_sep)
            gtk_widget_set_visible(w->bottom_mid_sep,
                                   cfg->enabled && w->lcd_ui.cfg.enabled);
    }
    update_status_label(w);
}

void sd_ui_set_cpu_hz(struct sim_win_t *w, uint64_t cpu_hz) {
    sd_set_cpu_hz(&w->sd_ui.sd, cpu_hz);
}

void sd_ui_reset(struct sim_win_t *w) {
    sd_reset(&w->sd_ui.sd);
}
