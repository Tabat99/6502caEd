#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---------------------------------------------------------------------------
 * CPU type selection
 * ------------------------------------------------------------------------- */
typedef enum {
    CPU_6502   = 0,   /* NMOS 6502 with undocumented opcodes */
    CPU_65C02  = 1,   /* CMOS 65C02 */
    CPU_65816  = 2,   /* WDC 65C816S */
} cpu_type_t;

/* ---------------------------------------------------------------------------
 * Status register flags
 * ------------------------------------------------------------------------- */
#define FLAG_C  0x01   /* Carry */
#define FLAG_Z  0x02   /* Zero */
#define FLAG_I  0x04   /* IRQ disable */
#define FLAG_D  0x08   /* Decimal */
#define FLAG_B  0x10   /* Break (6502/65C02, not a real register bit on 65816) */
#define FLAG_X  0x10   /* 65816: index register width (0=16-bit, 1=8-bit) */
#define FLAG_U  0x20   /* Unused/always 1 (6502/65C02) */
#define FLAG_M  0x20   /* 65816: accumulator width (0=16-bit, 1=8-bit) */
#define FLAG_V  0x40   /* Overflow */
#define FLAG_N  0x80   /* Negative */

/* ---------------------------------------------------------------------------
 * CPU state
 * ------------------------------------------------------------------------- */
typedef struct cpu_state {
    /* Registers — use 16-bit for 65816 compatibility */
    uint16_t A;    /* Accumulator (8-bit on 6502/65C02, 8 or 16 on 65816) */
    uint16_t X;    /* Index X */
    uint16_t Y;    /* Index Y */
    uint16_t SP;   /* Stack pointer (8-bit on 6502/65C02) */
    uint8_t  P;    /* Processor status */

    /* Program counter — 16-bit on 6502/65C02, 24-bit on 65816 */
    uint32_t PC;   /* bits 23:16 = PBR on 65816 */

    /* 65816 only */
    uint8_t  PBR;  /* Program bank register */
    uint8_t  DBR;  /* Data bank register */
    uint16_t D;    /* Direct page register */
    bool     emulation; /* true = emulation mode (acts like 65C02) */

    /* CPU type */
    cpu_type_t type;

    /* Cycle counter — incremented each phi2 cycle */
    uint64_t cycles;

    /* Interrupt lines — set externally, polled by cpu_step */
    bool irq;      /* IRQ line asserted (level sensitive) */
    bool nmi;      /* NMI line asserted (edge sensitive) */
    bool reset;    /* RESET line asserted */

    /* Internal NMI edge detection */
    bool _nmi_prev;
    bool _nmi_pending;

    /* Stopped / waiting */
    bool stopped;      /* STP instruction — halts CPU */
    bool stp_on_db;    /* treat $DB as STP even on 6502 */
    bool waiting;  /* WAI instruction (65C02/65816) */

    /* JAM/KIL — illegal opcode on 6502 that halts the CPU */
    bool jammed;
} cpu_state_t;

/* ---------------------------------------------------------------------------
 * Memory bus callbacks — the caller provides these
 * mem_read(addr, user_data)  → byte
 * mem_write(addr, byte, user_data)
 * addr is 32-bit to accommodate 65816 24-bit address space
 * ------------------------------------------------------------------------- */
typedef uint8_t (*cpu_read_fn) (uint32_t addr, void *ud);
typedef void    (*cpu_write_fn)(uint32_t addr, uint8_t val, void *ud);

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/* Initialise cpu_state to power-on defaults for the given type */
void cpu_init(cpu_state_t *cpu, cpu_type_t type);

/* Execute one instruction. Reads/writes via the supplied callbacks.
 * Returns number of cycles consumed (already added to cpu->cycles). */
int cpu_step(cpu_state_t *cpu, cpu_read_fn rd, cpu_write_fn wr, void *ud);

/* Reset — pulses the RESET vector */
void cpu_reset(cpu_state_t *cpu, cpu_read_fn rd, void *ud);

/* Disassemble one instruction at addr into buf (max buf_len bytes).
 * Returns the number of instruction bytes consumed.
 * For 65816, bank is the program bank register value. */
int cpu_disasm(cpu_type_t type, uint32_t addr, uint8_t bank,
               cpu_read_fn rd, void *ud,
               char *buf, size_t buf_len,
               bool m8, bool x8); /* 65816: accumulator/index width flags */

/* Human-readable status register string, e.g. "NV-BDIZC" */
void cpu_flags_str(uint8_t P, cpu_type_t type, bool emulation, char *out);
