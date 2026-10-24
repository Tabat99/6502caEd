#pragma once
/* sd_gtk.h — GTK4 panel for SD card emulation
 *
 * Provides:
 *  - Pin mapper for MOSI/MISO/SCK/CS
 *  - Card image file management (attach, create, eject, duplicate)
 *  - Status display (inserted/ejected, size, path)
 *  - Strict timing toggle
 */
#include <gtk/gtk.h>
#include "sd.h"
#include "spi_cfg.h"

/* Forward declaration */
struct sim_win_t;

/* SD UI state — one per simulator window */
typedef struct {
    sd_t        sd;             /* core SD emulation */
    spi_cfg_t   cfg;            /* pin mapping + config */

    /* GTK widgets */
    GtkWidget  *panel;          /* outer panel box */
    GtkWidget  *lbl_status;     /* "Inserted: foo.img (32MB)" */
    GtkWidget  *btn_attach;     /* Attach file */
    GtkWidget  *btn_eject;      /* Eject */
    GtkWidget  *btn_create;     /* Create new image */
    GtkWidget  *btn_duplicate;  /* Duplicate current image */
    GtkWidget  *btn_strict;     /* Strict timing checkbox */
    GtkWidget  *lbl_miso;       /* MISO indicator */

    /* Track last MISO state for display */
    int         miso_last;

} sd_ui_t;

/* Create and return the SD card panel widget */
GtkWidget *sd_ui_create(struct sim_win_t *w);

/* Called when any VIA pin changes — check SCK/MOSI/CS */
void sd_ui_via_changed(struct sim_win_t *w, int via_idx);

/* Update display (called from UI refresh timer) */
void sd_ui_update(struct sim_win_t *w);

/* Apply a new config (called when profile changes) */
void sd_ui_apply_cfg(struct sim_win_t *w, const spi_cfg_t *cfg);

/* Update CPU Hz for timing (called when speed changes) */
void sd_ui_set_cpu_hz(struct sim_win_t *w, uint64_t cpu_hz);

/* Reset SD state (called when simulator resets) */
void sd_ui_reset(struct sim_win_t *w);
