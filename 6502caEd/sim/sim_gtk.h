#pragma once
/* sim_gtk.h — GTK4 simulator window
 * Opens as a child window of the main editor.
 * Integrates: terminal display, register panel, VIA panels, ACIA status.
 */
#include <gtk/gtk.h>
#include "sim.h"
#include "via.h"
#include "acia.h"
#include "simcfg.h"

#include "simprofile.h"
#include "lcd_gtk.h"
#include "sd_gtk.h"

typedef struct sim_win_t sim_win_t;  /* forward declaration */

#define DISASM_LINES 16        /* lines shown in disassembly view */

/* ---------------------------------------------------------------------------
 * Per-VIA UI state
 * ------------------------------------------------------------------------- */
typedef struct {
  via_t *via;
  GtkWidget *port_lbl[2][8]; /* [port 0=A,1=B][bit 0..7] */
  GtkWidget *t1_lbl, *t2_lbl, *ifr_lbl;
  GtkWidget *btn[2][8]; /* toggle buttons for input bits */
  /* port A=0, B=1; tracked input state */
  uint8_t input[2];
  /* CA1/CB1 handshake line buttons */
  GtkWidget *ca1_btn, *cb1_btn;
  bool ca1_state, cb1_state;
} via_ui_t;

/* ---------------------------------------------------------------------------
 * Full simulator window state
 * ------------------------------------------------------------------------- */
struct sim_win_t {
  GtkWidget *window;
  GtkWidget *tv; /* terminal GtkTextView */
  GtkTextBuffer *tbuf;
  GtkTextMark *tend; /* mark at end of terminal buffer */
  int term_col;      /* current column for word wrap */

  /* Toolbar buttons */
  GtkWidget *btn_run, *btn_stop, *btn_step, *btn_reset, *btn_reload;

  /* Active memory map profile */
  simprofile_t profile;

  /* Project directory for profile discovery */
  char project_dir[512];

  /* Back-pointer to app state for ROM reload */
  void *app_state; /* AppState* — opaque to avoid circular include */

  /* Behaviour dropdowns */
  GtkWidget *dd_cpu;
  GtkWidget *dd_profile; /* profile selector dropdown */
  GtkWidget *dd_on_rts;
  GtkWidget *dd_on_brk;

  /* Register labels */
  GtkWidget *lbl_pc, *lbl_a, *lbl_x, *lbl_y, *lbl_sp, *lbl_p;
  GtkWidget *lbl_bank; /* bank register (6502/65C02 banking) */
  GtkWidget *lbl_flags[8];
  GtkWidget *lbl_cycles;

  /* Breakpoint entry + list */
  GtkWidget *bp_entry;
  GtkWidget *bp_list; /* GtkListBox */

  /* Disassembly labels */
  GtkWidget *disasm_labels[DISASM_LINES];

  /* VIA panels */
  via_ui_t via_ui[2];

  /* ACIA panel */
  GtkWidget *lbl_acia_settings;
  GtkWidget *lbl_acia_cpb; /* cycles per bit */
  GtkWidget *lbl_acia_conn;

  /* Speed info label — shows warning when visual updates are skipped */
  GtkWidget *lbl_speed_info;

  /* VIA output snapshot — used to detect changes at high speed */
  struct {
    uint8_t pa_out, pb_out, ddra, ddrb;
    uint16_t t1c, t2c;
    uint8_t ifr;
  } via_snap[2];

  /* Simulator state */
  sim_t sim;
  simcfg_t cfg;
  guint update_timer;

  /* Per-ROM-range userdata for IO dispatch callbacks */
  struct {
    sim_win_t *w;
    int        idx; /* index into w->profile.rom[] */
  } rom_ud[PROFILE_ROM_MAX];

  /* LCD emulation */
  lcd_ui_t lcd_ui;
  GtkWidget *lcd_panel_widget;  /* outer panel box — show/hide on enable/disable */
  GtkWidget *sd_panel_widget;   /* SD card panel — show/hide on enable/disable */
  GtkWidget *bottom_col_mid;    /* middle column (LCD+SD) */
  GtkWidget *bottom_col_sep;    /* separator before middle column */
  GtkWidget *bottom_mid_sep;    /* separator between LCD and SD */

  /* SD card emulation */
  sd_ui_t    sd_ui;
  bool       embedded_profile_active; /* true when [src] embedded profile is loaded */
  GtkWidget *chk_stp_db;             /* $DB=STP checkbox — hidden for 65C02/65816 */

  /* IO memory regions route back here */
  /* (via/acia callbacks need sim_win_t*) */
};


/* Create the simulator window. parent is the main app window.
 * cfg is used to configure the simulator on first open.
 * Returns the window widget (already shown). */
sim_win_t *sim_win_create(GtkWindow *parent, const simcfg_t *cfg, gboolean dark_mode);

/* Destroy simulator window and stop emulation */
void sim_win_destroy(sim_win_t *w);

/* Called from main GTK idle/timer to refresh UI */
gboolean sim_win_update(gpointer ud);  /* ud = sim_win_t* */

/* Send a keypress to the emulator terminal */
void sim_win_send_key(sim_win_t *w, char ch);

/* Accessors for main.c to attach data to simulator window */
GtkWidget *sim_win_get_window(sim_win_t *w);
void       sim_win_set_app_state(sim_win_t *w, void *app_state);
void       sim_win_set_project_dir(sim_win_t *w, const char *dir);
