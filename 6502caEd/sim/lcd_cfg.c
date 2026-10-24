/* lcd_cfg.c — HD44780 pin mapping configuration */
#include "lcd_cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void lcd_cfg_defaults(lcd_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->enabled   = false;   /* off by default until user configures */
    cfg->bus_width = 8;
    cfg->cols      = 20;
    cfg->rows      = 4;
    cfg->strict    = false;

    /* Default: RS=VIA1-B0, RW=VIA1-B1, E=VIA1-B2, data=VIA1-A0..A7 */
    cfg->rs = (lcd_pin_t){1, 1, 0};  /* VIA1, port B, bit 0 */
    cfg->rw = (lcd_pin_t){1, 1, 1};  /* VIA1, port B, bit 1 */
    cfg->e  = (lcd_pin_t){1, 1, 2};  /* VIA1, port B, bit 2 */
    for (int i = 0; i < 8; i++)
        cfg->data[i] = (lcd_pin_t){1, 0, i}; /* VIA1, port A, bits 0-7 */
}

bool lcd_pin_parse(const char *s, lcd_pin_t *pin) {
    /* Format: "1A3" = VIA1, port A, bit 3
     *         "2B7" = VIA2, port B, bit 7 */
    if (!s || !s[0] || !s[1] || !s[2]) return false;
    int via = s[0] - '0';
    if (via < 1 || via > 2) return false;
    char port_ch = toupper((unsigned char)s[1]);
    if (port_ch != 'A' && port_ch != 'B') return false;
    int bit = atoi(s + 2);
    if (bit < 0 || bit > 7) return false;
    pin->via  = via;
    pin->port = (port_ch == 'A') ? 0 : 1;
    pin->bit  = bit;
    return true;
}

void lcd_pin_format(const lcd_pin_t *pin, char *buf, int bufsz) {
    snprintf(buf, bufsz, "%d%c%d",
             pin->via,
             pin->port == 0 ? 'A' : 'B',
             pin->bit);
}
