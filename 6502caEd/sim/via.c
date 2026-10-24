#include "via.h"
#include <string.h>

static void update_irq(via_t *v) {
    bool asserted = (v->IFR & v->IER & 0x7F) != 0;
    if (asserted) v->IFR |= VIA_IRQ_ANY;
    else          v->IFR &= ~VIA_IRQ_ANY;
    if (asserted != v->_irq_state) {
        v->_irq_state = asserted;
        if (v->irq_fn) v->irq_fn(asserted, v->irq_ud);
    }
}

static void set_ifr(via_t *v, uint8_t bits) {
    v->IFR |= bits;
    update_irq(v);
}

void via_init(via_t *v, via_irq_fn irq_fn, void *ud) {
    memset(v, 0, sizeof(*v));
    v->irq_fn = irq_fn;
    v->irq_ud = ud;
}

void via_reset(via_t *v) {
    via_irq_fn fn = v->irq_fn;
    void *ud = v->irq_ud;
    memset(v, 0, sizeof(*v));
    v->irq_fn = fn;
    v->irq_ud = ud;
}

void via_tick(via_t *v) {
    /* --- Timer 1 --- */
    if (v->T1_running) {
        v->T1C--;
        if (v->T1C == 0xFFFF) {  /* underflow */
            set_ifr(v, VIA_IRQ_T1);
            if (v->ACR & 0x40) {
                /* Free-run: reload from latch */
                v->T1C = v->T1L;
                /* Toggle PB7 if ACR bit 7 set */
                if (v->ACR & 0x80) v->ORB ^= 0x80;
            } else {
                v->T1_running = false;
            }
        }
    }

    /* --- Timer 2 --- */
    if (v->T2_running) {
        if (v->ACR & 0x20) {
            /* Pulse counting mode — count falling edges on PB6 (external)
             * For now: count every tick (software can simulate PB6 manually) */
        } else {
            v->T2C--;
            if (v->T2C == 0xFFFF) {
                set_ifr(v, VIA_IRQ_T2);
                v->T2_running = false;
            }
        }
    }

    /* --- Shift register --- */
    if (v->SR_bits_left > 0) {
        v->SR_clk_count++;
        if (v->SR_clk_count >= 2) {  /* simplified: shift every 2 cycles */
            v->SR_clk_count = 0;
            v->SR = (v->SR << 1) | (v->SR >> 7); /* rotate for now */
            v->SR_bits_left--;
            if (v->SR_bits_left == 0) set_ifr(v, VIA_IRQ_SR);
        }
    }

    /* --- CA1/CB1 edge detect --- */
    bool ca1_edge = (v->PCR & 0x01) ? (v->CA1 && !v->_CA1_prev)   /* rising */
                                      : (!v->CA1 && v->_CA1_prev); /* falling */
    if (ca1_edge) set_ifr(v, VIA_IRQ_CA1);
    v->_CA1_prev = v->CA1;

    bool cb1_edge = (v->PCR & 0x10) ? (v->CB1 && !v->_CB1_prev)
                                       : (!v->CB1 && v->_CB1_prev);
    if (cb1_edge) set_ifr(v, VIA_IRQ_CB1);
    v->_CB1_prev = v->CB1;
}

uint8_t via_read(via_t *v, uint8_t reg) {
    reg &= 0xF;
    switch (reg) {
    case VIA_ORB: {
        /* Read: output bits come from ORB, input bits from PB_in */
        uint8_t r = (v->ORB & v->DDRB) | (v->PB_in & ~v->DDRB);
        /* Clear CB1/CB2 IFR on ORB read if PCR configured for it */
        v->IFR &= ~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
        update_irq(v);
        return r;
    }
    case VIA_ORA:
    case VIA_ORANHA: {
        uint8_t r = (v->ORA & v->DDRA) | (v->PA_in & ~v->DDRA);
        if (reg == VIA_ORA) {
            v->IFR &= ~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
            update_irq(v);
        }
        return r;
    }
    case VIA_DDRB:  return v->DDRB;
    case VIA_DDRA:  return v->DDRA;
    case VIA_T1CL:
        v->IFR &= ~VIA_IRQ_T1;
        update_irq(v);
        return v->T1C & 0xFF;
    case VIA_T1CH:  return (v->T1C >> 8) & 0xFF;
    case VIA_T1LL:  return v->T1L & 0xFF;
    case VIA_T1LH:  return (v->T1L >> 8) & 0xFF;
    case VIA_T2CL:
        v->IFR &= ~VIA_IRQ_T2;
        update_irq(v);
        return v->T2C & 0xFF;
    case VIA_T2CH:  return (v->T2C >> 8) & 0xFF;
    case VIA_SR:
        v->IFR &= ~VIA_IRQ_SR;
        update_irq(v);
        return v->SR;
    case VIA_ACR:   return v->ACR;
    case VIA_PCR:   return v->PCR;
    case VIA_IFR:   return v->IFR;
    case VIA_IER:   return v->IER | 0x80; /* bit 7 always reads 1 */
    }
    return 0xFF;
}

void via_write(via_t *v, uint8_t reg, uint8_t val) {
    reg &= 0xF;
    switch (reg) {
    case VIA_ORB:
        v->ORB = val;
        v->IFR &= ~(VIA_IRQ_CB1 | VIA_IRQ_CB2);
        update_irq(v);
        break;
    case VIA_ORA:
        v->ORA = val;
        v->IFR &= ~(VIA_IRQ_CA1 | VIA_IRQ_CA2);
        update_irq(v);
        break;
    case VIA_ORANHA:
        v->ORA = val;
        break;
    case VIA_DDRB: v->DDRB = val; break;
    case VIA_DDRA: v->DDRA = val; break;
    case VIA_T1CL: v->T1L = (v->T1L & 0xFF00) | val; break;
    case VIA_T1CH:
        v->T1L = (v->T1L & 0x00FF) | ((uint16_t)val << 8);
        v->T1C = v->T1L;
        v->IFR &= ~VIA_IRQ_T1;
        update_irq(v);
        v->T1_running = true;
        break;
    case VIA_T1LL: v->T1L = (v->T1L & 0xFF00) | val; break;
    case VIA_T1LH:
        v->T1L = (v->T1L & 0x00FF) | ((uint16_t)val << 8);
        v->IFR &= ~VIA_IRQ_T1;
        update_irq(v);
        break;
    case VIA_T2CL: v->T2L_lo = val; break;
    case VIA_T2CH:
        v->T2C = ((uint16_t)val << 8) | v->T2L_lo;
        v->IFR &= ~VIA_IRQ_T2;
        update_irq(v);
        v->T2_running = true;
        break;
    case VIA_SR:
        v->SR = val;
        v->SR_bits_left = 8;
        v->SR_clk_count = 0;
        v->IFR &= ~VIA_IRQ_SR;
        update_irq(v);
        break;
    case VIA_ACR: v->ACR = val; break;
    case VIA_PCR: v->PCR = val; break;
    case VIA_IFR:
        /* Writing clears flags */
        v->IFR &= ~(val & 0x7F);
        update_irq(v);
        break;
    case VIA_IER:
        if (val & 0x80) v->IER |=  (val & 0x7F); /* set bits */
        else            v->IER &= ~(val & 0x7F); /* clear bits */
        update_irq(v);
        break;
    }
}

void via_set_pa(via_t *v, uint8_t pins) { v->PA_in = pins; }
void via_set_pb(via_t *v, uint8_t pins) { v->PB_in = pins; }

void via_set_ca1(via_t *v, bool state) { v->CA1 = state; }
void via_set_cb1(via_t *v, bool state) { v->CB1 = state; }

uint8_t via_get_pa_out(via_t *v) { return v->ORA & v->DDRA; }
uint8_t via_get_pb_out(via_t *v) { return v->ORB & v->DDRB; }
