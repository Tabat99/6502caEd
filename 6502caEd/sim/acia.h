#pragma once
/* acia.h — 65C51 Asynchronous Communications Interface Adapter
 * TCP socket backend: acts as a server on a configurable port.
 * Connect any terminal (PuTTY, minicom, netcat) to use the serial port.
 */
#include <stdint.h>
#include <stdbool.h>

/* Register offsets from base address */
#define ACIA_DATA    0x0   /* Transmit (W) / Receive (R) data register */
#define ACIA_STATUS  0x1   /* Status register (R) / Reset (W) */
#define ACIA_CMD     0x2   /* Command register */
#define ACIA_CTRL    0x3   /* Control register */

/* Status register bits */
#define ACIA_ST_RDRF  0x08  /* Receive Data Register Full */
#define ACIA_ST_TDRE  0x10  /* Transmit Data Register Empty */
#define ACIA_ST_DCD   0x20  /* Data Carrier Detect (0=connected) */
#define ACIA_ST_DSR   0x40  /* Data Set Ready */
#define ACIA_ST_IRQ   0x80  /* Interrupt Request */

/* Control register — stop bits / word length / baud rate */
/* Bits 0-3: baud rate select (see baud table in .c)
 * Bit  4:   receiver clock (0=external, 1=baud rate gen)
 * Bits 5-6: word length (00=8, 01=7, 10=6, 11=5)
 * Bit  7:   stop bits (0=1 stop, 1=2 stop; 0 if parity+8 bits) */

/* Command register */
/* Bit 0:   DTR (1=enable receiver, 0=reset/disable)
 * Bits 1-2: transmitter IRQ control
 * Bit 3:   receive IRQ enable
 * Bit 4:   normal/loopback */

typedef void (*acia_irq_fn)(bool asserted, void *ud);

typedef struct {
    /* Registers */
    uint8_t  STATUS;
    uint8_t  CMD;
    uint8_t  CTRL;
    uint8_t  RDR;    /* Receive Data Register */
    uint8_t  TDR;    /* Transmit Data Register */
    bool     TDR_full;

    /* Baud rate info (derived from CTRL, for display) */
    int      baud;
    int      data_bits;
    int      stop_bits;
    char     parity;   /* 'N', 'O', 'E' */

    /* Cycle counter for baud rate timing */
    uint64_t cycle;          /* incremented each via_tick call */
    uint64_t tx_ready_cycle; /* cycle when TDR becomes empty */
    uint64_t rx_ready_cycle; /* cycle when RDR becomes full */
    int      cycles_per_bit; /* at current baud, given CPU clock */
    int      cpu_hz;         /* CPU clock in Hz (for baud timing) */

    /* TCP server */
    int      server_fd;  /* listening socket, -1 if not open */
    int      client_fd;  /* connected client, -1 if none */
    int      tcp_port;

    /* IRQ */
    acia_irq_fn irq_fn;
    void       *irq_ud;
    bool        _irq_state;
} acia_t;

/* Initialise. tcp_port=0 disables TCP. cpu_hz is the emulated CPU frequency. */
void acia_init(acia_t *a, int tcp_port, int cpu_hz,
               acia_irq_fn irq_fn, void *ud);

/* Open the TCP server socket. Call after acia_init. */
bool acia_tcp_open(acia_t *a);

/* Close TCP server and any connected client */
void acia_tcp_close(acia_t *a);

void acia_reset(acia_t *a);

/* Clock: call once per CPU cycle. Handles baud timing, TCP rx polling. */
void acia_tick(acia_t *a);

/* Bus access */
uint8_t acia_read (acia_t *a, uint8_t reg);
void    acia_write(acia_t *a, uint8_t reg, uint8_t val);

/* True if a client is currently connected */
bool acia_connected(acia_t *a);

/* Current settings as strings (for UI display) */
void acia_format_settings(acia_t *a, char *buf, int buf_len);
void acia_set_cpu_hz(acia_t *a, long cpu_hz); /* update baud timing */
void acia_format_timing(acia_t *a, char *buf, int buf_len);
