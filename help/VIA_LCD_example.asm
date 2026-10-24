; test_lcd_8bit.asm — HD44780 8-bit interface, correct init + BF polling
;
; Hardware:
;   RS = VIA1 PB0   (1B0)
;   RW = VIA1 PB1   (1B1)
;   E  = VIA1 PB2   (1B2)
;   D0 = VIA1 PA0   (1A0)  ...  D7 = VIA1 PA7  (1A7)
;
; Init sequence per datasheet section 2.2.2.1:
;   Wait >15ms after power-on  (simulator: immediate, permissive mode handles it)
;   Send Function Set ($30) — no BF check, wait >4.1ms
;   Send Function Set ($30) — no BF check, wait >100us
;   Send Function Set ($30) — BF valid after this
;   From here: poll BF before every command
;   Function Set ($38, 8-bit 2-line)
;   Display OFF ($08)
;   Clear Display ($01)   ← 15.2ms in strict mode
;   Entry Mode Set ($06)
;   Display ON ($0C)
;
;[profile]
;[rom0]
;enabled  = 1
;bank     = -1
;from     = C000
;to       = FFFF
;[io]
;via1_enable = 1
;via1_addr   = 9000
;[lcd]
;enabled    = 1
;bus_width  = 8
;cols       = 20
;rows       = 4
;strict     = 1
;rs         = 1B0
;rw         = 1B1
;e          = 1B2
;d0         = 1A0
;d1         = 1A1
;d2         = 1A2
;d3         = 1A3
;d4         = 1A4
;d5         = 1A5
;d6         = 1A6
;d7         = 1A7
;[/profile]

        .cpu    6502

; VIA1 registers
VIA1_ORB    = $9000     ; Port B output register (RS, RW, E)
VIA1_ORA    = $9001     ; Port A output register (D0-D7)
VIA1_DDRB   = $9002     ; Port B data direction
VIA1_DDRA   = $9003     ; Port A data direction
SIMIO       = $9030     ; Simulator terminal output

; Control pin masks (Port B)
LCD_RS      = %00000001 ; PB0
LCD_RW      = %00000010 ; PB1
LCD_E       = %00000100 ; PB2

; LCD commands
LCD_FUNC8   = $38       ; Function Set: 8-bit, 2-line, 5x7
LCD_DISPOFF = $08       ; Display OFF
LCD_CLEAR   = $01       ; Clear Display (15.2ms)
LCD_ENTRY   = $06       ; Entry Mode: increment, no shift
LCD_DISPON  = $0C       ; Display ON, cursor off, blink off
LCD_DDRAM   = $80       ; Set DDRAM address command

; DDRAM row start addresses (20x4)
ROW0        = $00
ROW1        = $40
ROW2        = $14
ROW3        = $54

; Zero page
ZP_TMP      = $00       ; scratch byte
ZP_PTR      = $02       ; 16-bit string pointer
ZP_LEN      = $04       ; string length

STR_LEN     = 20

;----------------------------------------------------------------------
        .org    $FFFA
        .word   IRQNMI
        .word   START
        .word   IRQNMI

;----------------------------------------------------------------------
        .org    $C000

; Display strings (exactly 20 chars each)
STR0:   .byte   "8-bit mode working! "
STR1:   .byte   "HD44780 correct init"
STR2:   .byte   "BF polled each cmd  "
STR3:   .byte   "15.2ms clear timing "

;----------------------------------------------------------------------
; Delay constants for init sequence at any speed
; These loops burn cycles — at 2MHz: 1 cycle = 500ns
; DELAY_4MS   = 4.1ms  = 8200 cycles @ 2MHz -> outer=32, inner=255 = 8160 cycles (close enough)
; DELAY_100US = 100us  =  200 cycles @ 2MHz -> simple 66-iteration loop
DELAY_4MS_COUNT   = 32
DELAY_100US_COUNT = 66

START:
        cld
        sei
        ldx     #$FF
        txs

        ; PB0-PB2 = outputs (RS, RW, E); rest don't care
        lda     #$07
        sta     VIA1_DDRB
        ; PA0-PA7 = outputs for write
        lda     #$FF
        sta     VIA1_DDRA
        ; All control lines low
        lda     #$00
        sta     VIA1_ORB

        ; ---- Init phase 1: three Function Set without BF check ----
        ; Datasheet section 2.2.2.1 timing:
        ;   After 1st: wait >4.1ms  (BF not valid)
        ;   After 2nd: wait >100us  (BF not valid)
        ;   After 3rd: BF now valid
        lda     #$30        ; Function Set — 8-bit mode force
        jsr     LCD_CMD_RAW
        jsr     DELAY_4MS   ; >4.1ms
        lda     #$30
        jsr     LCD_CMD_RAW
        jsr     DELAY_100US ; >100us
        lda     #$30
        jsr     LCD_CMD_RAW
        ; BF is now valid — switch to BF-checked commands

        ; ---- Init phase 2: configure display ----
        lda     #LCD_FUNC8  ; $38: 8-bit, 2-line, 5x7 font
        jsr     LCD_CMD_BF
        lda     #LCD_DISPOFF
        jsr     LCD_CMD_BF
        lda     #LCD_CLEAR  ; 15.2ms busy in strict mode
        jsr     LCD_CMD_BF
        lda     #LCD_ENTRY
        jsr     LCD_CMD_BF
        lda     #LCD_DISPON
        jsr     LCD_CMD_BF

        ; ---- Write four rows ----
        lda     #LCD_DDRAM|ROW0
        jsr     LCD_CMD_BF
        lda     #<STR0
        sta     ZP_PTR
        lda     #>STR0
        sta     ZP_PTR+1
        lda     #STR_LEN
        sta     ZP_LEN
        jsr     LCD_PUTN

        lda     #LCD_DDRAM|ROW2     ; physical line 3
        jsr     LCD_CMD_BF
        lda     #<STR1
        sta     ZP_PTR
        lda     #>STR1
        sta     ZP_PTR+1
        lda     #STR_LEN
        sta     ZP_LEN
        jsr     LCD_PUTN

        lda     #LCD_DDRAM|ROW1     ; physical line 2
        jsr     LCD_CMD_BF
        lda     #<STR2
        sta     ZP_PTR
        lda     #>STR2
        sta     ZP_PTR+1
        lda     #STR_LEN
        sta     ZP_LEN
        jsr     LCD_PUTN

        lda     #LCD_DDRAM|ROW3     ; physical line 4
        jsr     LCD_CMD_BF
        lda     #<STR3
        sta     ZP_PTR
        lda     #>STR3
        sta     ZP_PTR+1
        lda     #STR_LEN
        sta     ZP_LEN
        jsr     LCD_PUTN

        ; Signal completion to terminal
        lda     #'O'
        sta     SIMIO
        lda     #'K'
        sta     SIMIO
        lda     #$0A
        sta     SIMIO
DONE:   jmp     DONE

;----------------------------------------------------------------------
; LCD_WAIT_BF: poll busy flag (DB7) until LCD is ready
;   - Switches DDRA to input ($00), RW=1 RS=0
;   - Reads PA after E falling edge (simulator latches BF on E↓)
;   - Restores DDRA to $FF, RW=0
;----------------------------------------------------------------------
LCD_WAIT_BF:
        lda     #$00
        sta     VIA1_DDRA       ; PA = all inputs
        lda     #LCD_RW
        sta     VIA1_ORB        ; RS=0 RW=1 E=0
@poll:
        lda     #LCD_E|LCD_RW
        sta     VIA1_ORB        ; E=1
        lda     #LCD_RW
        sta     VIA1_ORB        ; E=0 — simulator writes BF to PA_in here
        lda     VIA1_ORA        ; read DB7..DB0 — DB7 = busy flag
        and     #$80
        bne     @poll           ; DB7=1: still busy
        lda     #$00
        sta     VIA1_ORB        ; RS=0 RW=0 E=0
        lda     #$FF
        sta     VIA1_DDRA       ; PA = all outputs
        rts

;----------------------------------------------------------------------
; LCD_CMD_BF: wait for ready, then send command in A (RS=0)
;----------------------------------------------------------------------
LCD_CMD_BF:
        sta     ZP_TMP
        jsr     LCD_WAIT_BF
        lda     #$00
        sta     VIA1_ORB        ; RS=0 RW=0 E=0
        lda     ZP_TMP
        sta     VIA1_ORA        ; put data on bus
        lda     #LCD_E
        sta     VIA1_ORB        ; E=1
        lda     #$00
        sta     VIA1_ORB        ; E=0 — latch command
        rts

;----------------------------------------------------------------------
; LCD_CMD_RAW: send command in A without BF check (init phase only)
;----------------------------------------------------------------------
LCD_CMD_RAW:
        sta     ZP_TMP
        lda     #$00
        sta     VIA1_ORB
        lda     ZP_TMP
        sta     VIA1_ORA
        lda     #LCD_E
        sta     VIA1_ORB
        lda     #$00
        sta     VIA1_ORB
        rts

;----------------------------------------------------------------------
; LCD_DATA_BF: wait for ready, then send data byte in A (RS=1)
;----------------------------------------------------------------------
LCD_DATA_BF:
        sta     ZP_TMP
        jsr     LCD_WAIT_BF
        lda     #LCD_RS
        sta     VIA1_ORB        ; RS=1 RW=0 E=0
        lda     ZP_TMP
        sta     VIA1_ORA
        lda     #LCD_E|LCD_RS
        sta     VIA1_ORB        ; E=1
        lda     #LCD_RS
        sta     VIA1_ORB        ; E=0
        lda     #$00
        sta     VIA1_ORB        ; RS=0
        rts

;----------------------------------------------------------------------
; LCD_PUTN: write ZP_LEN bytes from (ZP_PTR) using BF polling
;----------------------------------------------------------------------
LCD_PUTN:
        ldy     #$00
@loop:  lda     (ZP_PTR),y
        jsr     LCD_DATA_BF
        iny
        cpy     ZP_LEN
        bne     @loop
        rts

;----------------------------------------------------------------------
; DELAY_4MS: delay approximately 4.1ms
;   Uses nested loops — calibrated for worst case (2MHz)
;   At lower speeds this is longer but that's fine for init
;----------------------------------------------------------------------
DELAY_4MS:
        ldx     #DELAY_4MS_COUNT
@outer: ldy     #$FF
@inner: dey
        bne     @inner
        dex
        bne     @outer
        rts

;----------------------------------------------------------------------
; DELAY_100US: delay approximately 100us
;----------------------------------------------------------------------
DELAY_100US:
        ldx     #DELAY_100US_COUNT
@loop:  dex
        bne     @loop
        rts

IRQNMI: rti
