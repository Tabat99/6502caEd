#pragma once
/* lcd.h — HD44780-compatible LCD controller emulation
 *
 * Supports:
 *   - 4-bit and 8-bit interface modes
 *   - 1-line (16×1, 20×1) and 2-line (16×2, 20×4 logical) configurations
 *   - Full DDRAM (80 bytes) and CGRAM (64 bytes, 8 custom chars)
 *   - Busy flag with accurate cycle-based timing
 *   - All HD44780 commands
 *   - Permissive mode (default) and strict timing mode
 *   - E-strobe edge detection (data latched on falling edge)
 *
 * Pin mapping is external — the caller reads VIA output pins and calls
 * lcd_strobe() when it detects a falling edge on E.
 */
#include <stdint.h>
#include <stdbool.h>

/* -------------------------------------------------------------------------
 * Display geometry
 * The HD44780 has 80 bytes of DDRAM. For a 20×4 display the mapping is:
 *   Row 0: $00-$13   Row 1: $40-$53
 *   Row 2: $14-$27   Row 3: $54-$67
 * For 16×2:
 *   Row 0: $00-$0F   Row 1: $40-$4F
 * ------------------------------------------------------------------------- */
#define LCD_DDRAM_SIZE  104  /* covers $00-$67: rows 0-3 of 20x4 display */
#define LCD_CGRAM_SIZE  64   /* 8 chars × 8 bytes */
#define LCD_COLS_MAX    20
#define LCD_ROWS_MAX    4

/* HD44780 A00 CGROM — 5×8 pixel character bitmaps, indexed by char code.
 * Provided as extern array defined in lcd.c */
extern const uint8_t lcd_cgrom[256][8];

/* -------------------------------------------------------------------------
 * Timing constants (in nanoseconds, from HD44780 datasheet)
 * ------------------------------------------------------------------------- */
#define LCD_TIME_CLEAR_NS   15200000UL  /* Clear display / Return home: 15.2ms @ 250kHz */
#define LCD_TIME_CMD_NS        37000UL  /* Most commands */
#define LCD_TIME_WRITE_NS      37000UL  /* Data write */
#define LCD_TIME_READ_NS       37000UL  /* Data read */
#define LCD_TIME_E_CYCLE_NS    1000UL   /* Minimum E cycle time (tcycle ≥ 1000ns) */
#define LCD_TIME_E_PW_NS        450UL   /* Minimum E pulse width high (PWEH ≥ 450ns) */
#define LCD_TIME_SETUP_NS       140UL   /* RS,RW setup before E↑ (tAS ≥ 140ns) */
#define LCD_TIME_HOLD_NS         10UL   /* RS,RW hold after E↓ (tAH ≥ 10ns) */

/* -------------------------------------------------------------------------
 * LCD state machine
 * ------------------------------------------------------------------------- */
typedef struct {
    /* DDRAM and CGRAM */
    uint8_t ddram[LCD_DDRAM_SIZE];
    uint8_t cgram[LCD_CGRAM_SIZE];

    /* Address counter */
    uint8_t  ac;           /* current address counter (DDRAM or CGRAM) */
    bool     cgram_mode;   /* true = AC points into CGRAM */

    /* Display control */
    bool     display_on;
    bool     cursor_on;
    bool     blink_on;

    /* Entry mode */
    bool     increment;    /* true = increment AC after write, false = decrement */
    bool     shift_on;     /* true = display shifts on write */

    /* Function set */
    bool     bus_8bit;     /* true = 8-bit interface, false = 4-bit */
    bool     two_line;     /* true = 2-line mode */
    bool     font_5x10;    /* true = 5×10, false = 5×8 */

    /* Display shift offset (applied to DDRAM read for display) */
    int8_t   shift_offset;

    /* 4-bit mode nibble tracking */
    bool     nibble_pending;  /* waiting for second nibble */
    uint8_t  nibble_high;     /* first (high) nibble received */
    bool     nibble_rs;       /* RS state when first nibble arrived */

    /* Busy flag timing */
    uint64_t busy_until_cycles; /* CPU cycle count when busy flag clears */
    uint64_t cpu_cycles;        /* current CPU cycle count (updated each tick) */
    uint64_t cpu_hz;            /* CPU clock frequency for timing calculation */
    bool     strict_timing;     /* if false, busy flag always reads 0 */

    /* Initialisation state — HD44780 requires specific init sequence */
    int      init_step;        /* 0=uninitialised, tracks function-set calls */
    bool     initialised;

    /* Blink state for cursor */
    uint64_t blink_cycle;
    bool     blink_state;

    /* Set true when used with a 4-bit interface — affects init nibble handling */
    bool     is_4bit_iface;

    /* Cols/rows for display (set by user, affects wrap behaviour) */
    int      cols;   /* 16 or 20 */
    int      rows;   /* 1, 2, or 4 */

} lcd_t;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Initialise LCD state. cols/rows = display geometry (16×2, 20×4 etc.)
 * cpu_hz = CPU clock frequency for busy flag timing calculation. */
void lcd_init(lcd_t *lcd, int cols, int rows, uint64_t cpu_hz);

/* Reset LCD to power-on state (same as lcd_init but keeps cpu_hz/cols/rows) */
void lcd_reset(lcd_t *lcd);

/* Called every CPU cycle to advance internal timing */
void lcd_tick(lcd_t *lcd);

/* Process a falling edge on E (data strobe).
 * rs=RS pin, rw=RW pin, data=D0-D7 (or D4-D7 in lower nibble for 4-bit mode)
 * Returns the data byte the LCD would drive onto the bus (for reads),
 * or 0xFF if this is a write. In 4-bit mode caller must assemble full byte
 * from two calls.
 * busy_out: set to true if BF is set (LCD still processing previous command) */
uint8_t lcd_strobe(lcd_t *lcd, bool rs, bool rw, uint8_t data, bool *busy_out);

/* Read busy flag and address counter (RW=1, RS=0) without an E strobe.
 * Used for the VIA input pin simulation. */
uint8_t lcd_read_bf_ac(const lcd_t *lcd);

/* Read data register (RW=1, RS=1) without an E strobe. */
uint8_t lcd_read_data(const lcd_t *lcd);

/* Get the character code at display position col,row.
 * Returns 0x20 (space) if display is off or position out of range. */
uint8_t lcd_get_char(const lcd_t *lcd, int col, int row);

/* Get the pixel bitmap for a character code (from CGROM or CGRAM).
 * out_pixels: 8 bytes, each byte is a row of 5 pixels in bits 4..0 */
void lcd_get_bitmap(const lcd_t *lcd, uint8_t char_code, uint8_t *out_pixels);

/* True if cursor should be shown at this position right now */
bool lcd_cursor_at(const lcd_t *lcd, int col, int row);

/* True if cursor blink is active and currently in the "on" phase */
bool lcd_blink_on(const lcd_t *lcd);

/* Update CPU Hz (called when simulator speed changes) */
void lcd_set_cpu_hz(lcd_t *lcd, uint64_t cpu_hz);

/* Set strict/permissive timing mode */
void lcd_set_strict(lcd_t *lcd, bool strict);
