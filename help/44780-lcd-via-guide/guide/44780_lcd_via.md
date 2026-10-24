---
title: "HD44780 LCD Simulator VIA Guide"
---

*MJD 2026*

A practical companion guide for the **6502caEd HD44780 LCD simulator**.

This guide shows how to control a DMC / HD44780-style character LCD from a
6502 using a **6522 VIA mapped at `$9000`**. It covers both 8-bit and 4-bit LCD
interfaces, timing, initialisation, writing, commands, reads, character ROM,
CG RAM, command timings, and common controller quirks.

This is related to 6502caEd, but is intended to be packaged as a **separate
LCD simulator guide**, not as part of the main product help.

---

## Connection Assumptions

### VIA Base Address

The 6522 VIA is mapped at **`$9000`**.

| VIA register | Address | Use |
|---|---:|---|
| ORB / IRB | `$9000` | LCD control lines on port B |
| ORA / IRA | `$9001` | LCD data bus on port A |
| DDRB | `$9002` | Port B direction |
| DDRA | `$9003` | Port A direction |

The examples use the normal 6522 register order:

```asm
PORTB   = $9000
PORTA   = $9001
DDRB    = $9002
DDRA    = $9003
```

### Control Pins

For both 4-bit and 8-bit modes:

| LCD signal | VIA pin | Meaning |
|---|---|---|
| RS | PB0 | Register select: command/status or data |
| R/W | PB1 | Read/write select |
| E | PB2 | Enable strobe |

```asm
LCD_RS  = %00000001
LCD_RW  = %00000010
LCD_E   = %00000100
LCD_CTL = LCD_RS | LCD_RW | LCD_E
```

### 8-bit Data Bus

| LCD signal | VIA pin |
|---|---|
| D0-D7 | PA0-PA7 |

Use this mode when the simulator or hardware has the full LCD data bus wired to
port A.

### 4-bit Data Bus

| LCD signal | VIA pin |
|---|---|
| D4-D7 | PA4-PA7 |

In 4-bit mode, LCD DB0-DB3 are not used. On real hardware they should be tied
low rather than left floating. In the simulator, only PA4-PA7 are meaningful for
data transfers.

---

## LCD Bus Basics

The controller has two main 8-bit registers:

| RS | R/W | Operation |
|---:|---:|---|
| 0 | 0 | Write instruction / command |
| 0 | 1 | Read busy flag and address counter |
| 1 | 0 | Write display or CG RAM data |
| 1 | 1 | Read display or CG RAM data |

A write is latched by pulsing **E** high and then low. Data and control lines
must be stable before the falling edge of E.

In **8-bit mode**, one E pulse transfers one byte.

In **4-bit mode**, one byte is transferred as two E pulses:

1. high nibble first
2. low nibble second

The busy flag should only be checked after both nibbles of a byte have been
sent or read.

---

## Timing Rules — Complete Reference

The LCD controller is much slower than a 6502 and a 6522 VIA. This guide therefore
uses two separate kinds of timing:

1. **bus timing**, which is the width and setup/hold timing of each `E` pulse;
2. **instruction execution timing**, which is the time the LCD takes internally
   after it has accepted a command or data byte.

The values below are taken from the Optrex DMC/HD44780-compatible manual for
`Vcc = 5.0 V ±5%`, `Vss = 0 V`, and `Ta = 0..50 °C`. The manual notes that
module-specific sheets may give more detail, so these are the guide values for
the simulator and typical DMC-style modules.

### Power-on and Reset Timing

| Timing item | Symbol / condition | Value | Meaning |
|---|---:|---:|---|
| Power rise time for internal reset | `trcc` | `0.1 ms <= trcc <= 10 ms` | Vcc rise condition needed for the LCD internal reset circuit |
| Momentary power-off time | `toff` | `>= 1 ms` | Required off-time before power is considered cycled |
| Wait after Vcc reaches 4.5 V | software init delay | `> 15 ms` | No data should be transferred during this time |
| First wake-up delay | after first `$30` / nibble `$3` | `> 4.1 ms` | Busy flag cannot be checked yet |
| Second wake-up delay | after second `$30` / nibble `$3` | `> 100 us` | Busy flag still not reliable |
| Third wake-up delay | after third `$30` / nibble `$3` | `> 100 us` | Busy flag becomes usable after this write completes |
| 4-bit select delay | after nibble `$2` | `> 100 us` recommended | Allows the interface-width change to settle before full-byte commands |

### Write Bus Timing

These timings apply to each write transfer. In 8-bit mode, one byte is one
transfer. In 4-bit mode, one byte is two transfers, high nibble first and low
nibble second.

| Timing item | Symbol | Minimum | Maximum | Beginner meaning |
|---|---:|---:|---:|---|
| Enable cycle time | `tcycle` | `1000 ns` | — | Time from one `E` pulse to the next |
| Enable high pulse width | `PWEH` | `450 ns` | — | `E` must stay high at least this long |
| Enable rise/fall time | `tEr`, `tEf` | — | `25 ns` | Edge speed limit on real hardware |
| RS/RW setup before E | `tAS` | `140 ns` | — | Set `RS` and `R/W` before raising `E` |
| Address hold after E | `tAH` | `10 ns` | — | Keep `RS` and `R/W` stable just after `E` changes |
| Write data setup | `tDSW` | `195 ns` | — | Put data on PA before the LCD samples it |
| Write data hold | `tH` | `10 ns` | — | Keep data stable just after the transfer |

### Read Bus Timing

These timings apply to busy-flag reads and data reads. In 4-bit mode every read
uses two `E` pulses.

| Timing item | Symbol | Minimum | Maximum | Beginner meaning |
|---|---:|---:|---:|---|
| Enable cycle time | `tcycle` | `1000 ns` | — | Time from one `E` pulse to the next |
| Enable high pulse width | `PWEH` | `450 ns` | — | `E` must stay high while the MPU samples data |
| Enable rise/fall time | `tEr`, `tEf` | — | `25 ns` | Edge speed limit on real hardware |
| RS/RW setup before E | `tAS` | `140 ns` | — | Set `RS` and `R/W` before raising `E` |
| Data delay after E high | `tDDR` | — | `320 ns` | Wait until LCD output data is valid |
| Read data hold | `tH` | `20 ns` | — | Data remains valid briefly after `E` falls |

### Instruction Execution Timing

If you do **not** poll the busy flag, wait at least the time shown here after
each command or data access. The manual gives these times for an LCD clock of
`250 kHz`; if the LCD oscillator changes, scale the time by `250 / fosc(kHz)`.
For example, at `270 kHz`, a `40 us` command becomes approximately `37 us`.

| Operation | Code / selector | Execution time at 250 kHz | Description |
|---|---:|---:|---|
| Clear display | `$01` | `1.64 ms` | Clears DD RAM to spaces, homes cursor, resets display shift |
| Return home | `$02` or `$03` | `1.64 ms` | Homes cursor/display shift without clearing DD RAM |
| Entry mode set | `$04-$07` | `40 us` | Sets increment/decrement and optional display shift |
| Display ON/OFF control | `$08-$0F` | `40 us` | Controls display, cursor, blink |
| Cursor/display shift | `$10-$1F` | `40 us` | Moves cursor or shifts visible display |
| Function set | `$20-$3F` | `40 us` | Selects 4/8-bit interface, line count, font |
| Set CG RAM address | `$40-$7F` | `40 us` | Selects CG RAM address |
| Set DD RAM address | `$80-$FF` | `40 us` | Selects DD RAM address |
| Read busy flag and address | `RS=0`, `R/W=1` | bus read; BF reports internal state | Reads BF and address counter |
| Write data to CG/DD RAM | `RS=1`, `R/W=0` | `40 us` | Writes data to selected RAM |
| Read data from CG/DD RAM | `RS=1`, `R/W=1` | `40 us` internal timing; plus bus read timing | Reads data from selected RAM |

### Practical 6502/VIA Rule

A 6502 `STA` to the 6522 is already much slower than the nanosecond bus timings
on this table, especially at 1 MHz. The dangerous part is not the `E` pulse; it
is sending the next LCD instruction before the previous one has finished. Use
busy-flag polling after initialisation, or use conservative delays.

## VIA Setup

The control lines PB0-PB2 are outputs. Other PB lines are left as inputs by
these examples.

```asm
via_init:
        ; Configure the three LCD control pins as outputs.
        ; PB0 = RS, PB1 = R/W, PB2 = E.
        ; The upper five bits of port B are left alone as inputs.
        lda #LCD_CTL
        sta DDRB

        ; Put the LCD bus into a safe idle state:
        ; RS=0 selects the instruction/status register,
        ; RW=0 selects write mode,
        ; E=0 means no transfer is currently taking place.
        lda #$00
        sta PORTB
        rts
```

---

## 8-bit Interface Code

Use this section when LCD D0-D7 are wired to PA0-PA7.

### Low-level 8-bit Write

Entry:

- `A` = byte to write
- `X` = control bits: `$00` for command, `LCD_RS` for data

```asm
lcd8_write_raw:
        ; Entry:
        ;   A = byte to send to the LCD
        ;   X = control setting: 0 for command, LCD_RS for data
        ;
        ; The LCD samples the data bus when E is pulsed.
        ; This routine always writes, so RW is forced low.

        pha                   ; Save the byte while A is used for setup.

        lda #$ff              ; In 8-bit mode all PA pins are data outputs.
        sta DDRA

        pla                   ; Restore the byte to send.
        sta PORTA             ; Put D0..D7 on PA0..PA7 before E rises.

        txa                   ; Copy caller's RS setting into A.
        and #LCD_RS           ; Keep only RS; force RW=0 and E=0.
        sta PORTB             ; Present RS/RW setup before the E pulse.

        ora #LCD_E            ; Set E high: LCD sees a transfer in progress.
        sta PORTB

        and #%11111011        ; Clear E again.
        sta PORTB             ; Falling E completes/latches the byte.

        rts
```

### 8-bit Command and Data Helpers

```asm
lcd8_cmd:
        ldx #$00
        jsr lcd8_write_raw
        jsr lcd_wait_short
        rts

lcd8_data:
        ldx #LCD_RS
        jsr lcd8_write_raw
        jsr lcd_wait_short
        rts

lcd8_cmd_slow:
        ldx #$00
        jsr lcd8_write_raw
        jsr lcd_wait_long
        rts
```

### 8-bit Busy Flag Read

Returns:

- `A` = busy flag plus address counter
- bit 7 = busy flag
- N flag follows bit 7, so `BMI` means busy

```asm
lcd8_read_bf:
        lda #$00
        sta DDRA              ; PA0..PA7 inputs

        lda #LCD_RW           ; RS=0,RW=1,E=0
        sta PORTB
        ora #LCD_E
        sta PORTB             ; E=1
        lda PORTA             ; DB7=BF, DB0-DB6=AC
        pha

        lda #LCD_RW
        sta PORTB             ; E=0
        lda #$ff
        sta DDRA              ; restore data bus as output
        lda #$00
        sta PORTB             ; RW=0,E=0

        pla
        rts

lcd8_wait_bf:
        jsr lcd8_read_bf
        bmi lcd8_wait_bf
        rts
```

---

## 4-bit Interface Code

Use this section when LCD D4-D7 are wired to PA4-PA7.

The nibble is placed on the upper half of port A:

| Nibble | Value written to PORTA |
|---:|---:|
| `$3` | `$30` |
| `$2` | `$20` |
| High nibble of `$41` | `$40` |
| Low nibble of `$41` | `$10` |

### Low-level 4-bit Nibble Write

Entry:

- `A` = nibble already aligned to PA4-PA7
- `X` = control bits: `$00` for command, `LCD_RS` for data

```asm
lcd4_write_nibble:
        pha
        lda #$f0
        sta DDRA              ; PA4..PA7 outputs
        pla
        and #$f0
        sta PORTA

        txa
        and #LCD_RS           ; keep RS only, force RW=0,E=0
        sta PORTB
        ora #LCD_E
        sta PORTB             ; E=1
        and #%11111011
        sta PORTB             ; E=0
        rts
```

### Write a Full Byte in 4-bit Mode

Entry:

- `A` = byte
- `X` = control bits

```asm
lcd4_write_raw:
        ; Entry:
        ;   A = full 8-bit command or data byte
        ;   X = control setting: 0 for command, LCD_RS for data
        ;
        ; In 4-bit mode the LCD requires TWO transfers per byte:
        ;   1. high nibble, bits 7..4
        ;   2. low nibble, bits 3..0, shifted up onto PA7..PA4

        pha                   ; Keep a copy for the second nibble.

        and #$f0              ; Mask off the low nibble.
                              ; The high nibble is already on PA7..PA4.
        jsr lcd4_write_nibble ; Send bits 7..4 first.

        pla                   ; Get the original byte back.
        asl a                 ; Move bit 3 into bit 7.
        asl a                 ; Move bit 2 into bit 6.
        asl a                 ; Move bit 1 into bit 5.
        asl a                 ; Move bit 0 into bit 4.
        jsr lcd4_write_nibble ; Send original bits 3..0 second.

        rts
```

### 4-bit Command and Data Helpers

```asm
lcd4_cmd:
        ldx #$00
        jsr lcd4_write_raw
        jsr lcd_wait_short
        rts

lcd4_data:
        ldx #LCD_RS
        jsr lcd4_write_raw
        jsr lcd_wait_short
        rts

lcd4_cmd_slow:
        ldx #$00
        jsr lcd4_write_raw
        jsr lcd_wait_long
        rts
```

### 4-bit Busy Flag Read

A 4-bit read still needs two E strobes. The first nibble contains BF and the
upper address bits. The second nibble contains the lower address bits.

Returns:

- `A` = reconstructed busy flag plus address counter
- bit 7 = busy flag
- N flag follows bit 7, so `BMI` means busy

```asm
lcd4_read_nibble:
        lda #$00
        sta DDRA              ; PA inputs

        txa                   ; caller supplies RS/RW, E clear
        sta PORTB
        ora #LCD_E
        sta PORTB             ; E=1
        lda PORTA
        and #$f0              ; read PA4..PA7
        pha

        txa
        sta PORTB             ; E=0
        pla
        rts

lcd4_read_bf:
        ldx #LCD_RW           ; RS=0,RW=1,E=0
        jsr lcd4_read_nibble
        sta tmp               ; BF + AC6..AC4, already in bits 7..4

        jsr lcd4_read_nibble  ; AC3..AC0 in bits 7..4
        lsr a
        lsr a
        lsr a
        lsr a                 ; move to bits 3..0
        ora tmp
        pha

        lda #$f0
        sta DDRA              ; restore PA4..PA7 outputs
        lda #$00
        sta PORTB             ; RW=0,E=0

        pla
        rts

tmp:    .byte 0

lcd4_wait_bf:
        jsr lcd4_read_bf
        bmi lcd4_wait_bf
        rts
```

---

## Initialising the LCD

Software initialisation is recommended even if the LCD has an internal reset
circuit. The early wake-up writes must use fixed delays because the busy flag is
not reliable yet.

### 8-bit Initialisation

```asm
lcd8_init:
        jsr via_init
        lda #$ff
        sta DDRA

        jsr lcd_wait_power     ; wait more than 15 ms after power is valid

        lda #$30              ; wake-up: function set, 8-bit
        ldx #$00
        jsr lcd8_write_raw
        jsr lcd_wait_5ms

        lda #$30              ; repeat
        ldx #$00
        jsr lcd8_write_raw
        jsr lcd_wait_200us

        lda #$30              ; repeat; BF usable after this completes
        ldx #$00
        jsr lcd8_write_raw
        jsr lcd_wait_200us

        lda #$38              ; 8-bit, 2-line, 5x8 font
        jsr lcd8_cmd
        lda #$08              ; display off
        jsr lcd8_cmd
        lda #$01              ; clear display
        jsr lcd8_cmd_slow
        lda #$06              ; entry mode: increment, no shift
        jsr lcd8_cmd
        lda #$0c              ; display on, cursor off, blink off
        jsr lcd8_cmd
        rts
```

### 4-bit Initialisation

Before the LCD is switched into 4-bit mode, it is still receiving only the high
half of what it thinks is an 8-bit bus. The first transfers are therefore single
nibbles, not full 4-bit bytes.

```asm
lcd4_init:
        jsr via_init
        lda #$f0
        sta DDRA

        jsr lcd_wait_power     ; wait more than 15 ms after power is valid

        lda #$30              ; wake-up as 8-bit function set nibble
        ldx #$00
        jsr lcd4_write_nibble
        jsr lcd_wait_5ms

        lda #$30              ; repeat
        ldx #$00
        jsr lcd4_write_nibble
        jsr lcd_wait_200us

        lda #$30              ; repeat
        ldx #$00
        jsr lcd4_write_nibble
        jsr lcd_wait_200us

        lda #$20              ; select 4-bit interface
        ldx #$00
        jsr lcd4_write_nibble
        jsr lcd_wait_200us

        lda #$28              ; 4-bit, 2-line, 5x8 font
        jsr lcd4_cmd
        lda #$08              ; display off
        jsr lcd4_cmd
        lda #$01              ; clear display
        jsr lcd4_cmd_slow
        lda #$06              ; entry mode: increment, no shift
        jsr lcd4_cmd
        lda #$0c              ; display on, cursor off, blink off
        jsr lcd4_cmd
        rts
```

### Initialisation Summary

| Mode | Transfer | Meaning | Wait |
|---|---:|---|---:|
| 8-bit | `$30` | Function set wake-up | >4.1 ms |
| 8-bit | `$30` | Function set wake-up | >100 µs |
| 8-bit | `$30` | Function set wake-up | >100 µs |
| 8-bit | `$38` | 8-bit, 2-line, 5x8 | 40 µs or BF |
| 4-bit | nibble `$3` | Function set wake-up | >4.1 ms |
| 4-bit | nibble `$3` | Function set wake-up | >100 µs |
| 4-bit | nibble `$3` | Function set wake-up | >100 µs |
| 4-bit | nibble `$2` | Select 4-bit interface | >100 µs |
| 4-bit | `$28` | 4-bit, 2-line, 5x8 | 40 µs or BF |

Then for either mode:

| Command | Meaning | Wait |
|---:|---|---:|
| `$08` | Display off | 40 µs or BF |
| `$01` | Clear display | 1.64 ms or BF |
| `$06` | Entry mode: increment, no shift | 40 µs or BF |
| `$0C` | Display on, cursor off, blink off | 40 µs or BF |

---

## Sending Commands and Writing Text

### Send a Command

Commands use `RS=0`, `R/W=0`.

```asm
        lda #$80              ; set DD RAM address 0
        jsr lcd4_cmd          ; or lcd8_cmd
```

### Write a Character

Display data uses `RS=1`, `R/W=0`.

```asm
        lda #'A'
        jsr lcd4_data         ; or lcd8_data
```

### Set Cursor Position

For a normal 16x2 display:

```asm
lcd_line1:
        lda #$80              ; DD RAM $00
        jsr lcd4_cmd
        rts

lcd_line2:
        lda #$c0              ; DD RAM $40
        jsr lcd4_cmd
        rts
```

### Write a Zero-terminated String

This example uses a zero-page pointer named `strptr`.

```asm
lcd4_puts:
        ; Print a zero-terminated string using the 4-bit LCD routines.
        ;
        ; strptr points at the first character.
        ; Each character is written to DD RAM.
        ; The LCD entry mode then advances the cursor automatically.

        ldy #0                ; Y is the index into the string.
.next:  lda (strptr),y        ; Load one character from the string.
        beq .done             ; A zero byte marks the end of the string.
        jsr lcd4_data         ; Send this character to the LCD as data.
        iny                   ; Move to the next character.
        bne .next             ; Continue until zero terminator or Y wraps.
.done:  rts

strptr: .word hello           ; Pointer used by lcd4_puts.
hello:  .byte "HELLO, 6502!",0 ; Text followed by required terminator.
```

---

## Reading the Buffer

The LCD can read back both DD RAM and CG RAM. This is useful for testing the
simulator because the display contents can be verified through the same bus the
6502 uses to write them.

### Busy Flag and Address Counter

Read with `RS=0`, `R/W=1`.

```text
bit 7    = busy flag
bits 6-0 = address counter
```

If BF is 1, the LCD is still executing the previous command and must not accept
another command or data write.

### Reading DD RAM or CG RAM Data

To read display data:

1. send **Set DD RAM Address**: `$80 | address`
2. perform a data read with `RS=1`, `R/W=1`
3. discard the first read after setting the address on controllers that use the
   documented data-register preload behaviour
4. read again to get the byte from the requested address

### 8-bit Data Read Helper

```asm
lcd8_read_data:
        lda #$00
        sta DDRA              ; PA inputs

        lda #(LCD_RS | LCD_RW)
        sta PORTB
        ora #LCD_E
        sta PORTB
        lda PORTA
        pha

        lda #(LCD_RS | LCD_RW)
        sta PORTB             ; E=0
        lda #$ff
        sta DDRA              ; restore output bus
        lda #$00
        sta PORTB

        pla
        rts
```

### 4-bit Data Read Helper

```asm
lcd4_read_data:
        ldx #(LCD_RS | LCD_RW)
        jsr lcd4_read_nibble
        sta tmp

        jsr lcd4_read_nibble
        lsr a
        lsr a
        lsr a
        lsr a
        ora tmp
        pha

        lda #$f0
        sta DDRA
        lda #$00
        sta PORTB

        pla
        rts
```

---

## DD RAM Addressing

DD RAM stores up to 80 character codes. The visible display is only a window
onto that memory.

### Common 16x2 and 20x2 Layout

| Line | First visible address | Command byte |
|---|---:|---:|
| 1 | `$00` | `$80` |
| 2 | `$40` | `$C0` |

For 16x2, visible line 1 is usually `$00-$0F`; visible line 2 is usually
`$40-$4F`.

### Common 20x4 Layout

A 20x4 display is normally organised like a 40x2 internal display:

| Line | First visible address | Command byte |
|---|---:|---:|
| 1 | `$00` | `$80` |
| 2 | `$40` | `$C0` |
| 3 | `$14` | `$94` |
| 4 | `$54` | `$D4` |

### 16x1 Quirk

Some 16x1 modules are internally treated like an 8x2 display:

| Visible columns | DD RAM range |
|---|---|
| 1-8 | `$00-$07` |
| 9-16 | `$40-$47` |

This is one of the most common surprises when writing simulator tests.

---

## Character Set and CG RAM

The built-in character ROM is broadly ASCII-like for the printable range, but
it is not a modern Unicode font. Character ROMs vary between modules.

| Code range | Meaning |
|---|---|
| `$00-$07` | CG RAM custom characters in 5x8 mode |
| `$08-$0F` | Usually mirrors `$00-$07` custom characters |
| `$20-$7E` | Printable ASCII-like characters |
| `$80-$FF` | Symbols, Japanese kana, or module-specific glyphs |

The Optrex manual notes that some high-order character regions may be
inadequate or variant on particular ROMs. For a simulator, it is best to define
which ROM image or fallback glyph policy is being used.

### CG RAM Custom Characters

CG RAM is 64 bytes. In 5x8 mode this gives eight user-defined characters. Each
character uses eight rows. Only bits 0-4 are visible pixels; bits 5-7 are not
shown.

Character code `$00` displays CG RAM character 0, `$01` displays character 1,
and so on up to `$07`.

```asm
make_char0:
        lda #$40              ; CG RAM address 0
        jsr lcd4_cmd

        ldy #0
.loop:  lda glyph0,y
        jsr lcd4_data
        iny
        cpy #8
        bne .loop

        lda #$80              ; back to DD RAM address 0
        jsr lcd4_cmd
        lda #$00              ; display custom character 0
        jsr lcd4_data
        rts

glyph0:
        .byte %00000
        .byte %00100
        .byte %01110
        .byte %11111
        .byte %00100
        .byte %00100
        .byte %00100
        .byte %00000
```

---

## Command List

| Command | Bits | Hex | Time | Description | Notes / quirks |
|---|---|---:|---:|---|---|
| Clear display | `00000001` | `$01` | 1.64 ms | Clears DD RAM to spaces and homes cursor/display shift | Slow; also returns display shift to original position |
| Return home | `0000001x` | `$02-$03` | 1.64 ms | Sets DD RAM address to 0 and homes shifted display | Slow; DD RAM contents unchanged |
| Entry mode set | `000001IS` | `$04-$07` | 40 µs | Sets increment/decrement and optional display shift after writes | `I=1` increments; `S=1` shifts display on writes |
| Display on/off | `00001DCB` | `$08-$0F` | 40 µs | Display, cursor, and blink control | Display off does not erase DD RAM |
| Cursor/display shift | `0001SCRLxx` | `$10-$1F` | 40 µs | Shifts cursor or display left/right | Display shift does not change AC |
| Function set | `001DLNFxx` | `$20-$3F` | 40 µs | Selects 4/8 bit, line count, and font | Use during initialisation |
| Set CG RAM address | `01AAAAAA` | `$40-$7F` | 40 µs | Selects CG RAM address | 64 byte CG RAM |
| Set DD RAM address | `1AAAAAAA` | `$80-$FF` | 40 µs | Selects DD RAM address | Layout depends on module geometry |
| Read busy/address | `RS=0,RW=1` | n/a | bus read | Reads BF and AC | In 4-bit mode read two nibbles |
| Write data | `RS=1,RW=0` | `$00-$FF` | 40 µs | Writes to selected DD RAM or CG RAM | Address changes according to entry mode |
| Read data | `RS=1,RW=1` | `$00-$FF` | 40 µs internal | Reads selected DD RAM or CG RAM | First read after address set may be dummy |

### Command Bit Meanings

| Bit | Meaning |
|---|---|
| `I` | 1 = increment address counter, 0 = decrement |
| `S` | 1 = shift display after data write |
| `D` | 1 = display on |
| `C` | 1 = cursor on |
| `B` | 1 = blink on |
| `S/C` | 0 = cursor shift, 1 = display shift |
| `R/L` | 0 = left, 1 = right |
| `DL` | 1 = 8-bit interface, 0 = 4-bit interface |
| `N` | 1 = 2-line mode, 0 = 1-line mode |
| `F` | 1 = 5x10 font, 0 = 5x8 / 5x7 font |

---

## Delay Routines

These routines are deliberately conservative placeholders. Adjust them for the
clock speed used by the emulator or target machine.

```asm
lcd_wait_short:                 ; roughly 40 us or more at 1 MHz if padded
        ldy #20
.loop:  dey
        bne .loop
        rts

lcd_wait_long:                  ; roughly 1.6 ms or more at 1 MHz if padded
        ldx #8
.outer: ldy #255
.inner: dey
        bne .inner
        dex
        bne .outer
        rts

lcd_wait_200us:
        ldy #100
.loop:  dey
        bne .loop
        rts

lcd_wait_5ms:
        ldx #25
.loop:  jsr lcd_wait_200us
        dex
        bne .loop
        rts

lcd_wait_power:
        ldx #4
.loop:  jsr lcd_wait_5ms
        dex
        bne .loop
        rts
```

For simulator testing, delays are still useful because many real-world LCD
examples do not wire R/W and therefore cannot poll the busy flag.

---

## Quirks and Simulator Notes

- Software initialisation is recommended because the internal reset depends on
  the LCD power ramp.
- Busy flag reads are invalid during the early wake-up writes.
- In 4-bit mode, all full-byte writes and reads are high nibble first, low
  nibble second.
- During 4-bit initialisation, the `$3,$3,$3,$2` sequence is sent as single
  nibbles.
- The first DD RAM or CG RAM data read after setting an address may be a dummy
  read because the controller preloads the data register internally.
- Display off does not erase DD RAM.
- Clear display and return home are much slower than ordinary commands.
- 16x1 and 20x4 modules often have non-linear DD RAM layouts.
- CG RAM rows use only five visible bits.
- In 4-bit hardware, DB0-DB3 should be tied low.
- The simulator should document whether unsupported character ROM codes appear
  as blanks, replacement glyphs, or exact ROM glyphs.

---

## Minimal Demonstration Program, 4-bit Mode

```asm
        .org $0800

start:
        ; Bring the VIA and LCD into a known state.
        ; lcd4_init performs the special 4-bit wake-up sequence
        ; and then turns the display on.
        jsr lcd4_init

        ; Put the cursor at DD RAM address $00.
        ; Command $80 means: set DD RAM address to $00.
        ; On a normal 16x2 display this is line 1, column 1.
        lda #$80
        jsr lcd4_cmd

        ; X will count through the message bytes.
        ldx #0

.msg:   ; Load the next character from the message table.
        lda message,x

        ; The string is terminated by a zero byte.
        ; When A is zero, BEQ branches to the end.
        beq .done

        ; Send the character in A to the LCD data register.
        ; The LCD then stores it in DD RAM and moves the cursor.
        jsr lcd4_data

        ; Advance to the next character and loop.
        inx
        bne .msg

.done:  ; Stop here forever so the text remains on screen.
        jmp .done

message:
        ; Text bytes to display, followed by a zero terminator.
        .byte "HD44780 READY",0
```

---

## Minimal Demonstration Program, 8-bit Mode

```asm
        .org $0800

start:
        ; Initialise the VIA and the LCD for an 8-bit data bus.
        ; lcd8_init uses PA0..PA7 for LCD D0..D7.
        jsr lcd8_init

        ; Command $80 sets the LCD address counter to DD RAM $00.
        ; This is the first visible character on line 1 for common modules.
        lda #$80
        jsr lcd8_cmd

        ; X indexes through the bytes in the message.
        ldx #0

.msg:   ; Fetch the next ASCII character.
        lda message,x

        ; Zero means end of string.
        beq .done

        ; Write A as display data.
        ; RS is set by lcd8_data, so this is a character write, not a command.
        jsr lcd8_data

        ; Move to the next message byte.
        inx
        bne .msg

.done:  ; Endless loop: useful in a simple emulator demo.
        jmp .done

message:
        ; Text bytes to display, followed by a zero terminator.
        .byte "8-BIT LCD OK",0
```
