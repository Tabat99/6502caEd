#pragma once
/* via.h — 65C22 Versatile Interface Adapter
 * Two timers, two 8-bit ports (PA/PB), shift register, interrupt logic.
 * Instantiate one via_t per VIA chip.
 */
#include <stdint.h>
#include <stdbool.h>

/* Register offsets from base address */
#define VIA_ORB   0x0   /* Output Register B */
#define VIA_ORA   0x1   /* Output Register A */
#define VIA_DDRB  0x2   /* Data Direction Register B */
#define VIA_DDRA  0x3   /* Data Direction Register A */
#define VIA_T1CL  0x4   /* Timer 1 Counter Low */
#define VIA_T1CH  0x5   /* Timer 1 Counter High */
#define VIA_T1LL  0x6   /* Timer 1 Latch Low */
#define VIA_T1LH  0x7   /* Timer 1 Latch High */
#define VIA_T2CL  0x8   /* Timer 2 Counter Low */
#define VIA_T2CH  0x9   /* Timer 2 Counter High */
#define VIA_SR    0xA   /* Shift Register */
#define VIA_ACR   0xB   /* Auxiliary Control Register */
#define VIA_PCR   0xC   /* Peripheral Control Register */
#define VIA_IFR   0xD   /* Interrupt Flag Register */
#define VIA_IER   0xE   /* Interrupt Enable Register */
#define VIA_ORANHA 0xF  /* ORA no handshake */

/* IFR/IER bit positions */
#define VIA_IRQ_CA2  0x01
#define VIA_IRQ_CA1  0x02
#define VIA_IRQ_SR   0x04
#define VIA_IRQ_CB2  0x08
#define VIA_IRQ_CB1  0x10
#define VIA_IRQ_T2   0x20
#define VIA_IRQ_T1   0x40
#define VIA_IRQ_ANY  0x80

/* Callback: called when IRQ line changes state */
typedef void (*via_irq_fn)(bool asserted, void *ud);

typedef struct {
    /* Registers */
    uint8_t ORA, ORB;       /* Output registers */
    uint8_t DDRA, DDRB;     /* Direction registers (1=output) */
    uint8_t SR;             /* Shift register */
    uint8_t ACR, PCR;       /* Control registers */
    uint8_t IFR, IER;       /* Interrupt flag/enable */

    /* Timer 1 */
    uint16_t T1C;           /* Counter (decrements each cycle) */
    uint16_t T1L;           /* Latch */
    bool     T1_running;

    /* Timer 2 */
    uint16_t T2C;
    uint8_t  T2L_lo;        /* T2 only has an 8-bit latch (lo byte) */
    bool     T2_running;

    /* Port input pins (set externally to simulate input) */
    uint8_t  PA_in;         /* Input pin state for port A */
    uint8_t  PB_in;         /* Input pin state for port B */

    /* CA1/CA2/CB1/CB2 pin state */
    bool CA1, CA2, CB1, CB2;
    bool _CA1_prev, _CB1_prev;

    /* Shift register state */
    int  SR_bits_left;
    int  SR_clk_count;

    /* IRQ callback */
    via_irq_fn irq_fn;
    void      *irq_ud;
    bool       _irq_state;
} via_t;

/* Initialise to power-on defaults */
void via_init(via_t *v, via_irq_fn irq_fn, void *ud);
void via_reset(via_t *v);

/* Clock: call once per CPU cycle. Updates timers, triggers interrupts. */
void via_tick(via_t *v);

/* Bus access from CPU */
uint8_t via_read (via_t *v, uint8_t reg);
void    via_write(via_t *v, uint8_t reg, uint8_t val);

/* Set port input pin states (call before via_tick for accurate timing) */
void via_set_pa(via_t *v, uint8_t pins);  /* full port A input */
void via_set_pb(via_t *v, uint8_t pins);  /* full port B input */
void via_set_ca1(via_t *v, bool state);
void via_set_cb1(via_t *v, bool state);

/* Get port output state (bits where DDR=1 are outputs) */
uint8_t via_get_pa_out(via_t *v);
uint8_t via_get_pb_out(via_t *v);
