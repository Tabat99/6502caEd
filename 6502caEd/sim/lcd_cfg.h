#pragma once
/* lcd_cfg.h — HD44780 LCD pin mapping configuration
 *
 * Describes how the LCD's RS, RW, E and data bus pins
 * connect to VIA1/VIA2 port A/B bits.
 * Saved in the .simprofile [lcd] section.
 */
#include <stdint.h>
#include <stdbool.h>

/* Which VIA and port a single LCD pin is connected to */
typedef struct {
    int     via;    /* 1 = VIA1, 2 = VIA2 */
    int     port;   /* 0 = port A, 1 = port B */
    int     bit;    /* 0-7 */
} lcd_pin_t;

/* Full LCD pin configuration */
typedef struct {
    bool        enabled;      /* LCD emulation active */

    /* Control pins */
    lcd_pin_t   rs;           /* Register Select */
    lcd_pin_t   rw;           /* Read/Write */
    lcd_pin_t   e;            /* Enable (strobe) */

    /* Data bus pins D4-D7 (4-bit) or D0-D7 (8-bit) */
    lcd_pin_t   data[8];      /* data[0]=D0/D4 ... data[7]=D7 */

    /* Interface mode */
    int         bus_width;    /* 4 or 8 */

    /* Display geometry */
    int         cols;         /* 16 or 20 */
    int         rows;         /* 1, 2, or 4 */

    /* Timing mode */
    bool        strict;       /* true = enforce HD44780 timing */

} lcd_cfg_t;

/* Fill with sensible defaults (20×4, 8-bit, VIA1 port B,
 * RS=B0, RW=B1, E=B2, D0-D7=A0-A7) */
void lcd_cfg_defaults(lcd_cfg_t *cfg);

/* Parse a pin string like "1B3" (VIA1, port B, bit 3) */
bool lcd_pin_parse(const char *s, lcd_pin_t *pin);

/* Format a pin to string like "1B3" */
void lcd_pin_format(const lcd_pin_t *pin, char *buf, int bufsz);
