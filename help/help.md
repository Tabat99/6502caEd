---
title: "6502caEd Help"
---

*MJD 2026*

A 6502/65C02/65816 assembler and editor available in two interfaces: a GTK
graphical editor and a TUI (terminal) editor. Both share the same assembler core.

---

## Introduction

The 6502caEd suite is designed for writing 6502-based assembly programs,
released for general use.

It consists of:

- **Core assembler** — command-line assembler, also used by both editors
- **TUI editor** — terminal-based editor with the assembler built in
- **GTK editor** — full graphical editor with the assembler built in

All three share exactly the same assembler core, so assembled output is
identical regardless of which interface you use.

The entire suite supports:

- 6502 instruction set
- 6502 undocumented instruction set (stable opcodes only)
- WDC 65C02 instruction set
- WDC 65816 instruction set

Both editors feature customisable colour coding for instructions, labels,
definitions, the memory viewer, and assembler output.

---

## Getting Started

### Opening a File

**GTK:** Use **File → Open** (Ctrl+O) or drag a file onto the editor window.
Recent files are listed under **File → Open Recent**.

**TUI:** Press **F10** to open the menu bar, navigate to **File → Open**, or
press **Ctrl+O** directly.

### Your First Assembly

Create a new file (**Ctrl+N**) and write some code. The `.cpu` directive
selects the target processor (`6502`, `6502u`, `65c02`, or `65816`):

```asm
    .cpu 6502
    .org $0800

start:
    LDA #$01
    STA $00
    BRK
```

Press **F5** to check for errors, or **F9** to assemble and produce a binary.
Errors and warnings appear in the Problems panel.

### Saving

- **Ctrl+S** — Save
- **Ctrl+Shift+S** — Save As

---

## The GTK Editor

The GTK editor is the full graphical interface. It supports multiple tabs,
syntax highlighting, find/replace with scope, a memory viewer, an include
manager, and code folding.

### Window Layout

- **Menu bar** — File, Edit, View, Assembly, Preferences, Help
- **Tab bar** — one tab per open file; click × to close
- **Editor area** — main editing area with line numbers and syntax colouring
- **Problems panel** — shows assembler errors and warnings (toggle with **F8**)

### Tabs

Open multiple files simultaneously using tabs. Switch between them by clicking
the tab labels. Each tab maintains its own undo history, fold state, and cursor
position.

When you click an entry in the Problems panel, the editor automatically switches
to the correct tab (or opens the file if not already open) and jumps to the
relevant line.

### Syntax Highlighting

The editor colours code automatically:

- **Instructions** — highlighted in the instruction colour
- **Directives** — `.org`, `.byte`, `.macro`, `.pragma`, `.halt` etc.
- **Labels** — definitions and references
- **Comments** — `;` or `//` to end of line
- **Numbers** — decimal, `$hex`, `&hex`, `%binary`
- **Strings** — double-quoted

Colours are customisable via the `.colours` config file (see [Configuration Files](#configuration-files)).

### Live Error Checking

Both editors run a background assembly check as you type. Errors and warnings
appear as inline underlines and in the Problems panel within 50 ms of you
stopping typing. The line currently being edited is suppressed to avoid false
errors mid-keystroke; errors reappear as soon as the cursor moves off that line.

### Zoom

- **Ctrl++** or **Ctrl+=** — Zoom in
- **Ctrl+-** — Zoom out
- **Ctrl+0** — Reset zoom

Hold **Ctrl** and scroll the mouse wheel to zoom.

### Dark and Light Mode

Toggle between dark and light themes with **Ctrl+Shift+L** or via
**Preferences → Dark mode**.

---

## The TUI Editor

The TUI editor runs entirely in the terminal using ncurses. Launched with:

```
6502caEd [filename]
```

### Navigation

- **F1** — Open the menu bar
- **Arrow keys** — Move cursor
- **Ctrl+Arrow** — Move by word
- **Home / End** — Start/end of line
- **Ctrl+Home / Ctrl+End** — Start/end of file
- **Page Up / Page Down** — Scroll by page

### Status Bar

The bottom of the screen shows the current file name, line/column position,
and status messages (errors, save confirmations, search results).

---

## Editing

### Selection

Click and drag, or hold **Shift** and press arrow keys, to select text.
**Backspace**, **Delete**, or typing any character while a selection is active
replaces the selection — standard editor behaviour.

### Undo and Redo

- **Ctrl+Z** — Undo
- **Ctrl+Y** (or **Ctrl+Shift+Z** in GTK) — Redo

Undo history is maintained per file and persists while the file is open.

### Cut, Copy, Paste

- **Ctrl+X** — Cut selection
- **Ctrl+C** — Copy selection
- **Ctrl+V** — Paste

Right-clicking in the GTK editor shows a context menu with Cut, Copy,
Copy without Comments, and Paste. These work correctly with the current
text selection.

### Comments

Toggle line comments with:

- **GTK:** Ctrl+/ (toggles the comment marker on the current or selected lines)
- **TUI:** Ctrl+K, then C to comment or U to uncomment

The comment marker (`;` or `//`) is configurable in
**Preferences → Comment marker**.

### Comment Alignment

When **Keep comment column aligned** is enabled, inline comments are
automatically aligned to a fixed column. The target column is set via
**Preferences → Comment align column** (default: column 40).

### Auto Indent

When enabled (**Preferences → Auto indent**, Ctrl+Shift+I), pressing Enter
automatically indents the new line to match the current line's indentation.

### Tab Width

Set the tab stop width via **Preferences → Tab width** (Ctrl+Alt+T).

### Format Layout

**Edit → Format layout** re-formats the current file: aligns operands,
normalises spacing, and optionally aligns comments.

### Case Conversion

**Edit → Auto formatter → Upper case / Lower case** converts the selected
text or current line to upper or lower case.

### Duplicate Line Removal

**Edit → Remove duplicate lines** removes any lines that are exact duplicates
of earlier lines in the file.

### Unicode Replacement

**Edit → Replace common Unicode** replaces typographic characters (smart
quotes, dashes, etc.) with their ASCII equivalents.

---

## Assembly

### Checking vs Assembling

There are two assembly actions:

- **F5 — Check:** runs the full assembler pipeline, reports all errors and
  warnings, but writes no binary output. Use this while editing to validate
  your code quickly.

- **F9 — Assemble:** runs the full assembler pipeline and, on success, offers
  to save the binary output.

### Project Assembly (GTK, multiple tabs)

When more than one file is open, pressing **F9** presents a choice:

- **Assemble Project** — finds the project root file (the file that includes
  all others, even if it is not currently open as a tab) and assembles from
  there. This is the correct choice for multi-file projects.
- **Assemble This Tab** — assembles only the current tab's content, ignoring
  other files.

The project root is located automatically using the Include Manager's root
detection (see [Include Manager](#include-manager)).

### Errors and Warnings

Errors prevent binary output. Warnings are informational and do not stop
assembly. Both appear in the Problems panel (**F8**) and as inline underlines
in the editor.

Clicking an entry in the Problems panel jumps directly to the relevant line,
switching tabs or opening the file if necessary.

### Binary Output

After successful assembly, you are prompted to save the binary:

- **Assembled range** — only the bytes that were written
- **Specify range** — enter a custom start/end address
- **Entire 64KB** — the full 64KB address space
- **16K page** — a 16KB region (shown when applicable)

The default binary extension is configurable via
**Assembly → Default extensions → Binary extension**.

### Memory Viewer

After assembly, **Assembly → Memory Viewer** (Ctrl+M) opens a hex viewer
showing the assembled memory image. You can inspect any address in the 64KB
space and see which bytes were written by the assembler.

### Allow Undocumented Opcodes

**Assembly → Allow undocumented opcodes** enables support for undocumented
6502 instructions (e.g. LAX, SAX, DCP). Disabled by default.

### Jump to Definition

Place the cursor on a label and press **Ctrl+F12** to jump to where that label
is defined.

### Find References

Place the cursor on a label and press **Shift+F11** (GTK) or **Ctrl+F11** (TUI)
to list all references to that label in the current file.

### Jump History

- **F12** — Jump forward in navigation history
- **Shift+F12** — Jump back in navigation history

---

## Include Manager

**Assembly → Includes Manager** (Ctrl+I) shows the complete include tree for
the current project — all files brought in via `.include` directives, their
nesting relationships, and whether each file is currently open in the editor.

### Root Detection

The include manager automatically finds the project root file. It first checks
open tabs, then scans sibling `.asm`/`.s` files on disk to find which file
includes the current one. This means the tree is correctly rooted on
`os_init.asm` (for example) even if that file is not open as a tab.

### Opening Files

Double-clicking any file in the tree opens it in the editor. If the file is
already open in a tab, that tab is activated instead of opening a duplicate.

### Open File Highlighting

Files that are currently open as tabs are highlighted in the tree so you can
see at a glance which project files you have loaded.

---

## Find and Replace

Open the find bar with **Ctrl+F** (Find) or **Ctrl+H** (Find and Replace).

### Scope

The find bar includes a **scope selector** that controls which files are searched:

| Scope | Description |
|-------|-------------|
| **This Tab** | Search only the currently active editor tab |
| **All Tabs** | Search across every open tab |
| **All Linked Files** | Search the current file and all files reachable via `.include` from the project root |

The scope selector applies to both Find (Prev/Next) and Replace All.

- **Find with scope > This Tab:** when the search reaches the end of the
  current tab, it continues automatically in the next tab.
- **Replace All with All Linked Files:** files that are open as tabs are
  modified in their live buffers; files not open are modified on disk directly.

### Find Options

- **Match case** — toggles case-sensitive matching
- **Prev / Next** — step through matches
- **Enter** — find next; **Shift+Enter** — find previous

### Replace

- **Replace** — replace the current match and advance
- **Replace All** — replace all matches in the current scope

---

## Memory Map

After assembly, the Memory Map popup (**Assembly → Memory Map**) shows a
visual overview of the 64KB address space.

### Chart

The chart displays the full 64KB space as a grid of pages (256 bytes per row,
256 rows = 65536 bytes total). Each pixel represents one byte:

- **Blue** — byte written by the assembler with an explicit instruction or
  data directive (`.byte`, `.word`, `EQUS`, etc.) after a `.org` directive
- **Empty** — byte not written, or written only as `.res` placeholder fill

Importantly, the map only shows bytes from **intentional** code and data
placement. Variable reservations (`.res`) and any bytes emitted before the
first `.org` are excluded, so the map reflects what actually ends up in ROM
rather than zero-page bookkeeping.

### Range Table

Below the chart, a table lists each contiguous range of written bytes with
its start address, end address, and byte count, plus a total.

### Navigation

The chart scrolls vertically. It automatically scrolls to bring the first
written range into view when opened.

---

## Directives Reference

Directives are case-insensitive. The leading `.` is optional for most.

### Origin and CPU

| Directive | Description |
|-----------|-------------|
| `.cpu 6502` | Select NMOS 6502 CPU type. |
| `.cpu 6502u` | Select NMOS 6502 with stable undocumented instructions. |
| `.cpu 65c02` | Select CMOS 65C02 CPU type. |
| `.cpu 65816` | Select CMOS 65816 (aka `65c816`) type. |
| `.longa on/off` | 16-bit / 8-bit accumulator immediate mode (65816 only). |
| `.longi on/off` | 16-bit / 8-bit index register immediate mode (65816 only). |
| `.org $addr` | Set the program counter to `$addr`. Also `*=` or `.pc=`. |

### Data

| Directive | Example | Description |
|-----------|---------|-------------|
| `.byte` | `.byte $01, $02, "hello"` | Define one or more bytes. Accepts numbers, strings, expressions. |
| `.word` | `.word $1234, label` | Define 16-bit little-endian words. |
| `.dword` | `.dword $12345678` | Define 32-bit little-endian doublewords. |
| `.res` | `.res 256, $00` | Reserve N bytes, filled with optional value (default 0). |
| `.align` | `.align 256` | Advance PC to the next multiple of N. |
| `.incbin` | `.incbin "file.bin"` | Include raw binary file. Optional offset and length: `.incbin "f.bin", 0, 128` |

### Guard Directives

Guard directives let you define address ranges that trigger an error or
warning when the assembler tries to write a byte there. This is useful for
catching accidental writes into reserved or ROM-mapped regions.

```asm
.halt  $C000-$CFFF "write to OS ROM: $%s"
.alert $0200-$02FF "caution: write to page 2: $%s"
.unhalt  $C000         ; remove the halt guard starting at $C000
.unalert $0200         ; remove the alert guard starting at $0200
```

- **`.halt`** — emits an **error** (halts assembly) if a byte is written to
  the specified range
- **`.alert`** — emits a **warning** (assembly continues) if a byte is written
  to the specified range
- **`.unhalt` / `.unalert`** — remove a guard by its start address (either
  keyword removes either type)

The address range is `start-end` (inclusive). A single address with no `-end`
guards a single byte.

**Message format specifiers:**

| Specifier | Expands to |
|-----------|------------|
| `%s` | 4-digit hex of the actual write address |
| `%r` | Range as `XXXX-YYYY` |

Guards only fire during an explicit **F5 check** or **F9 assemble** — they are
silent during the live background syntax check. Each guard range emits at most
5 diagnostics before suppressing further hits with a `...` message.

### Include Guards

Use `.pragma once` at the top of any file that may be included more than once.
The preprocessor ensures the file's content is only assembled once per build,
no matter how many times it appears in `.include` chains:

```asm
; definitions.asm
.pragma once

MY_CONST = $1234
OTHER    = $5678
```

This is the preferred alternative to the `.ifndef` / `.def` / `.endif` guard
pattern, which causes the editor to grey out the file contents. `.pragma once`
has no visual side-effects.

`#pragma once` (C-style prefix) is also accepted.

### Symbols

```asm
NAME = value        ; define a constant (EQU-style)
NAME .= value       ; define only if not already defined
```

The current program counter can be captured as a symbol:

```asm
block_start = *     ; * = current PC
block_end   = *
block_size  = block_end - block_start
```

### Definitions (Preprocessor Symbols)

Preprocessor symbols are distinct from assembler constants (`NAME = value`).
They are handled before assembly and used mainly to drive conditional assembly.
Both `#` and `.` prefixes work identically for all preprocessor directives,
and all keywords are case-insensitive.

```asm
.def NAME value      ; define NAME with a text value
.def NAME            ; define NAME with value 1 (true flag)
#def NAME value      ; identical — # and . are interchangeable

.define NAME value   ; longer spelling, same effect
#define NAME value   ; identical

.undef NAME          ; remove a definition
#undef NAME          ; identical
```

Notes:
- `cpu` is a reserved name — use `.cpu` to set it, not `.def cpu`.
- Redefining the same name is an error; the first definition wins.
- A `.def` without a value defaults to `1`.

### Conditionals

All conditional directives accept either a `.` or `#` prefix. All keywords
are case-insensitive.

#### Existence tests

```asm
.ifdef  NAME         ; assemble block if NAME has been .def'd
.ifndef NAME         ; assemble block if NAME has NOT been .def'd
```

#### Expression test

```asm
.if expression       ; assemble block if expression evaluates to non-zero
```

#### Chaining and else

```asm
.elif  expression    ; else-if
.else                ; unconditional else branch
```

#### Closing

```asm
.endif               ; end the conditional block
```

#### Full example

```asm
.def PLATFORM 2
.def DEBUG

.ifdef DEBUG
    nop             ; only assembled when DEBUG is defined
.endif

.if PLATFORM == 1
    lda #$01
.elif PLATFORM == 2
    lda #$02
.else
    lda #$FF
.endif
```

#### Rules and errors

- `.elif` / `.else` after a `.else` is an error.
- `.elif` / `.else` / `.endif` with no matching `.if` is an error.
- Nesting is supported up to 64 levels.

### Conditional Expressions

`.if` and `.elif` evaluate a full numeric expression.

#### `defined()` function

```asm
.if defined(NAME)    ; 1 if NAME has been .def'd, 0 otherwise
.if !defined(NAME)   ; inverse
```

#### Operators (highest precedence first)

| Operator | Description |
|----------|-------------|
| `!` `~` `-` `+` | Unary logical NOT, bitwise NOT, negate, identity |
| `<` `>` | Unary low byte / high byte |
| `*` `/` `%` | Multiply, divide, modulo |
| `+` `-` | Add, subtract |
| `<<` `>>` | Shift left, shift right |
| `&` | Bitwise AND |
| `^` | Bitwise XOR |
| `\|` | Bitwise OR |
| `==` `!=` `<` `>` `<=` `>=` | Comparison (result 0 or 1) |
| `&&` | Logical AND |
| `\|\|` | Logical OR |

Natural-language aliases: `.or`, `.and`, `.not` (also without dot).

#### CPU type in conditionals

| `.cpu` setting | `cpu` value |
|----------------|-------------|
| `6502` | `6502` |
| `6502u` | `6502u` |
| `65c02` | `6503` |
| `65816` | `65816` |

```asm
.cpu 65c02
.if cpu == 6503      ; 65C02 uses numeric value 6503
    stz $00
.endif
```

### Procedures and Scopes

```asm
.proc name          ; begin procedure — labels inside are local to name
    local_label:    ; accessible as name::local_label from outside
.endproc

.scope name         ; same as .proc but no entry-point assumption
.endscope
```

### Local Labels

Labels beginning with `@` are local to the surrounding `.proc`/`.scope`:

```asm
.proc myroutine
@loop:
    DEX
    BNE @loop
.endproc
```

### Macros

```asm
.macro MYMACRO arg1, arg2
    LDA arg1
    STA arg2
.endmacro

    MYMACRO $10, $20    ; invoke macro
```

### Repeat

```asm
.repeat 4
    NOP
.endrepeat
```

Repeats the enclosed block N times (N must be a constant, 0–65536).

### Include

```asm
.include "filename.asm"     ; include relative to the current file's directory
```

Includes another source file. The path is resolved relative to the directory
of the **current** file.

```asm
.includeroot "directory/"   ; set the root include search directory (root file only)
```

`.includeroot` sets a base directory that subsequent `.include` directives will
search when the path has no directory separator. May only appear in the root file.

For multi-file projects, place `.pragma once` at the top of each included file
so it is safe to include from multiple places without double-assembling:

```asm
; io.asm
.pragma once
.include "definitions.asm"

; ... io routines ...
```

### Assertions and Diagnostics

```asm
.assert condition, "message"    ; halt assembly with message if condition is false
.error "message"                ; always emit an error
.warning "message"              ; always emit a warning
```

---

## Expression Syntax

Expressions can appear anywhere a value is expected.

### Number Literals

| Format | Example | Notes |
|--------|---------|-------|
| Decimal | `255` | Plain integer |
| Hex | `$FF` or `&FF` | Both forms accepted |
| Binary | `%11001100` | |
| Character | `'A'` | ASCII value of character |

### Operators

| Operator | Description |
|----------|-------------|
| `+` `-` `*` `/` | Arithmetic |
| `%` | Modulo |
| `<<` `>>` | Bit shift left / right |
| `&` `\|` `^` | Bitwise AND, OR, XOR |
| `~` | Bitwise NOT (unary) |
| `==` `!=` `<` `>` `<=` `>=` | Comparison (result is 0 or 1) |
| `&&` `\|\|` `!` | Logical AND, OR, NOT |
| `(` `)` | Grouping |

### Special Symbols

| Symbol | Description |
|--------|-------------|
| `*` or `.` | Current program counter |
| `<expr` | Low byte of expression (bits 0–7) |
| `>expr` | High byte of expression (bits 8–15) |
| `.lobyte(expr)` | Low byte of expression |
| `.hibyte(expr)` | High byte of expression |
| `.bankbyte(expr)` | Bank byte (bits 16–23) |

### Examples

```asm
block_start = *             ; capture current PC
LDA #<address               ; low byte of address
LDA #>address               ; high byte of address
LDA #(end-start)            ; length of a region
patch_target = * + 1        ; address of next instruction's operand byte
```

---

## Addressing Modes

### 6502 Modes

| Mode | Syntax | Example |
|------|--------|---------|
| Implied | — | `CLC` |
| Accumulator | `A` | `LSR A` or just `LSR` |
| Immediate | `#value` | `LDA #$FF` |
| Zero page | `$addr` (0..255) | `LDA $10` |
| Zero page, X | `$addr,X` | `LDA $10,X` |
| Zero page, Y | `$addr,Y` | `LDX $10,Y` |
| Absolute | `$addr` (256+) | `LDA $1000` |
| Absolute, X | `$addr,X` | `LDA $1000,X` |
| Absolute, Y | `$addr,Y` | `LDA $1000,Y` |
| Indirect | `($addr)` | `JMP ($FFFC)` |
| (Indirect, X) | `($addr,X)` | `LDA ($10,X)` |
| (Indirect), Y | `($addr),Y` | `LDA ($10),Y` |
| Relative | label | `BNE loop` |

### 65C02 Additional Modes

| Mode | Syntax | Example |
|------|--------|---------|
| Zero page indirect | `($addr)` | `LDA ($10)` |
| Absolute indexed indirect | `($addr,X)` | `JMP ($1000,X)` |
| Relative (always) | label | `BRA target` |

### 65816 Additional Modes

| Mode | Syntax | Example | Notes |
|------|--------|---------|-------|
| Absolute long | `$AABBCC` | `LDA $018000` | 24-bit address |
| Absolute long, X | `$AABBCC,X` | `LDA $018000,X` | |
| Direct page indirect long | `[$dp]` | `LDA [$20]` | |
| Direct page indirect long, Y | `[$dp],Y` | `LDA [$20],Y` | |
| Stack relative | `offset,S` | `LDA 3,S` | |
| Stack relative indirect, Y | `(offset,S),Y` | `LDA (3,S),Y` | |
| Long relative | label | `BRL target` | 16-bit relative |
| Block move | `src,dst` | `MVN $01,$02` | Bank numbers |

---

## 65816 Support

Enable 65816 mode with `.cpu 65816`. The 65816 is fully backwards-compatible
with 6502 and 65C02.

### Native Mode

```asm
    CLC
    XCE             ; switch to native mode
    REP #$30        ; 16-bit accumulator and index registers
```

### Register Width

| Instruction | Effect |
|-------------|--------|
| `REP #$20` | M=0 — accumulator is 16-bit |
| `SEP #$20` | M=1 — accumulator is 8-bit (default) |
| `REP #$10` | X=0 — index registers are 16-bit |
| `SEP #$10` | X=1 — index registers are 8-bit (default) |
| `REP #$30` | Both 16-bit |
| `SEP #$30` | Both 8-bit |

The assembler tracks `REP`/`SEP` through the source to size immediate
operands correctly. Use `.longa on/off` and `.longi on/off` to override
when the assembler cannot infer the width state from the code flow.

### Long Addressing

```asm
    LDA $018000         ; 24-bit address: bank 1, offset $8000
    JSL $019000         ; call subroutine in bank 1
    RTL                 ; return from long subroutine
    BRL far_label       ; ±32KB relative branch
```

### Block Move

```asm
    REP #$30
    LDA #$00FF          ; count-1
    LDX #$2000          ; source offset
    LDY #$1000          ; destination offset
    MVN $02,$01         ; move bank $02 → bank $01
```

---

## Code Folding

Code folding hides blocks of comment lines or macro definitions, replacing
them with a summary line. The file is always saved in fully unfolded form.

### What Can Be Folded

- **Comment blocks** — consecutive comment-only lines (`;` or `//`)
- **Macro definitions** — from `.macro` to `.endmacro`

### Toggling

**GTK:** Click the line number of a fold-start line, or press **Ctrl+]**
with the cursor on that line.

**TUI:** Press **Ctrl+]** with the cursor on the first line of a foldable block.

### Enable/Disable All

**GTK — Preferences → Fold:**

- **Fold Comments** / **Unfold Comments**
- **Fold Macros** / **Unfold Macros**

---

## Memory Viewer

**Assembly → Memory Viewer** (Ctrl+M) opens a hex viewer showing the full
64KB assembled memory image.

### Navigation

- **Arrow keys** — move one byte at a time
- **Page Up / Page Down** — scroll by page
- **Ctrl+Home / Ctrl+End** — jump to start/end

### Display

Bytes written by the assembler are shown normally. Unwritten bytes are dimmed.
The address of the selected byte is shown in the header.

### Editing

Type hex digits to edit bytes directly. Changed bytes are marked dirty.
Save the modified image from the viewer's own save function.

---

## Simulator

The 6502caEd GTK editor includes an integrated 6502/65C02/65816 simulator. Press **F5** (or Assembly → Run in Simulator) to assemble and launch.

### Toolbar

The simulator toolbar has two rows. The top row contains transport controls and the speed selector. The bottom row contains profile management and CPU options.

| Button | Action |
|--------|--------|
| Run (F5) | Start or resume execution |
| Stop (F6) | Pause execution |
| Step (F7) | Execute one instruction |
| Reset (Ctrl+R) | Reset CPU and restart |
| Reload ROM | Reload last assembled binary without closing |
| Speed | 8Hz to MAX (uncapped) |
| $DB=STP | (6502 only) treat opcode $DB as a halt |

### Simulator Profiles

Profiles define the hardware configuration: CPU type, ROM ranges, IO device addresses, banking, and LCD display. They are stored as `.simprofile` files in `~/.6502caed/profiles/` (global) or alongside your source file (project-local).

The profile toolbar shows the active profile name. Use the ⚙ button to edit it, **+** to create a new one, and 🗑 to delete it.

Profile fields include:

- **CPU** — 6502, 6502 (undoc), 65C02, or 65816
- **$DB=STP** — (6502 only) map opcode $DB to a halt
- **ROM Ranges** — up to 4 ROM regions with address range, bank, R/W and lock bits
- **IO Devices** — enable/disable VIA1, VIA2, ACIA, Console TX/RX and their base addresses
- **Banking** — bank register address, window, window size, number of banks
- **LCD Display** — see [LCD Display](#lcd-display) below

### Embedded Profiles in Source Files

You can embed a profile override block directly in your `.asm` file using comment syntax. When you run the file, the simulator merges these settings with the currently selected profile — only the keys you specify are overridden.

```
;[profile]
;[rom0]
;enabled  = 1
;bank     = -1
;from     = C000
;to       = FFFF
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
;d7         = 1A7
;stp_on_db  = 1
;[/profile]
```

The block must start with `;[profile]` and end with `;[/profile]` (with or without a space after the semicolon). Section headers like `;[lcd]` and key=value lines work exactly like a `.simprofile` file. The active profile in the dropdown is shown as `[src] profilename` while an embedded profile is in use.

### VIA Panels

Each enabled VIA is shown as a panel with bit number headers (7–0), Port A and Port B rows. Red buttons are outputs, uncoloured buttons are inputs. Click an input bit to toggle it. T1, T2 timer counts and the IFR flags are shown below.

### ACIA Panel

The ACIA panel shows the baud rate, data format, and TCP port. Connect a serial terminal to the shown port to communicate with the simulated ACIA. The Console TX/RX addresses provide a simpler text I/O path — write a character to the TX address to print it in the terminal pane.

### LCD Display (HD44780) {#lcd-display}

The simulator emulates an HD44780-compatible LCD display. Enable it in the profile editor under **LCD Display**.

#### Pin Mapping

Each LCD signal is mapped to a specific VIA pin using the format `VIA Port Bit`, e.g. `1B2` = VIA1 port B bit 2. Click the **…** button next to each entry to use the pin picker.

| Pin | Function |
|-----|----------|
| RS | Register Select (0=command, 1=data) |
| RW | Read/Write (0=write, 1=read) |
| E  | Enable strobe (data latched on falling edge) |
| D0–D7 | 8-bit data bus (8-bit mode) |
| D4–D7 | 4-bit data bus (4-bit mode, upper nibble first) |

#### 8-bit Interface

All 8 data pins (D0–D7) connected. Each command or data byte is sent as a single E strobe. The standard initialisation sequence sends Function Set ($38) three times, then Display Off, Clear, Entry Mode, Display On.

#### 4-bit Interface

Only D4–D7 connected. Each byte requires two E strobes — high nibble first, then low nibble. The initialisation sequence sends nibble $3 three times (forces 8-bit sync), then nibble $2 to switch to 4-bit mode.

```asm
; 4-bit init sequence
lda #$30        ; nibble $3 on PA4-PA7
jsr LCD_NIB     ; first  — function set 8-bit
jsr LCD_NIB     ; second
jsr LCD_NIB     ; third
lda #$20        ; nibble $2 — switch to 4-bit
jsr LCD_NIB
```

#### Busy Flag

With **Strict timing** enabled, the LCD enforces real HD44780 timing (~37µs per command, ~1.52ms for Clear/Home). Your code must either poll the busy flag or insert delays.

To poll the busy flag: set RW=1, RS=0, pulse E high then low, then read the data bus. In 8-bit mode bit 7 is the busy flag. In 4-bit mode you must complete both nibble strobes — the busy flag is DB7, which arrives in the high nibble on D7.

With **Permissive** mode (default), the busy flag is always clear and commands are accepted instantly at any speed.

#### 20×4 DDRAM Layout

The HD44780 DDRAM address space is non-contiguous. For a 20×4 display:

| Physical line | DDRAM start | DDRAM end |
|---------------|-------------|-----------|
| Line 1 | $00 | $13 |
| Line 2 | $40 | $53 |
| Line 3 | $14 | $27 |
| Line 4 | $54 | $67 |

Always use `LCD_DDRAM | ROW0/ROW1/ROW2/ROW3` to reposition before writing each line.

#### Custom Characters (CGRAM)

The HD44780 supports 8 user-defined 5×8 characters (codes 0–7). Write 8 bitmap rows per character to CGRAM using Set CGRAM Address ($40) followed by data writes.

**Note:** character code $00 is also the C null terminator. Use a length-based write routine rather than null-terminated strings when displaying custom characters.

### Halt Instructions

None of the supported CPUs have a dedicated halt instruction, but the simulator provides several ways to stop execution cleanly:

- **`JMP *`** — infinite loop to self, works on all CPU types. Press Stop (F6) to pause.
- **`STP` ($DB)** — native on 65C02 and 65816. Halts the CPU; the simulator pauses automatically.
- **$DB=STP on 6502** — enable the *$DB=STP* checkbox (or set `stp_on_db = 1` in the profile or embedded profile block) to make opcode $DB halt the 6502. Press Run (F5) or Reset to resume.
- **BRK=Halt** — set BRK mode to Halt in the toolbar to pause on BRK instead of vectoring.


---

## Configuration Files

6502caEd uses plain-text configuration files for colours and theme, found in:

- **Installed (deb):** `/etc/6502caEd/`
- **User override:** `~/.config/6502caEd/`

User files take precedence over system files.

### Colour File (`6502caEd.colours`)

Controls syntax highlighting colours. Each line is `token_kind = #RRGGBB`:

```
instruction = #4fc3f7
directive   = #ce93d8
comment     = #546e7a
label       = #a5d6a7
number      = #ffb74d
string      = #ef9a9a
```

### Theme File (`6502caEd.theme`)

Controls the editor's overall appearance:

```
background  = #1e1e1e
foreground  = #d4d4d4
gutter_bg   = #252526
gutter_fg   = #858585
```

### Config Files Dialog

**GTK: Preferences → Config Files** — view, edit, copy, and create
configuration files without leaving the editor.

---

## Keyboard Shortcuts — GTK

### File

| Action | Shortcut |
|--------|----------|
| New file | Ctrl+N |
| Open file | Ctrl+O |
| Save | Ctrl+S |
| Save As | Ctrl+Shift+S |
| Quit | Ctrl+Q |

### Edit

| Action | Shortcut |
|--------|----------|
| Undo | Ctrl+Z |
| Redo | Ctrl+Shift+Z or Ctrl+Y |
| Cut | Ctrl+X |
| Copy | Ctrl+C |
| Paste | Ctrl+V |
| Find | Ctrl+F |
| Find and Replace | Ctrl+H |
| Go to line | Ctrl+G |

### View

| Action | Shortcut |
|--------|----------|
| Toggle Problems panel | F8 |
| Zoom in | Ctrl++ |
| Zoom out | Ctrl+- |
| Reset zoom | Ctrl+0 |
| Toggle dark mode | Ctrl+Shift+L |

### Assembly

| Action | Shortcut |
|--------|----------|
| Check (no binary) | F5 |
| Assemble (with binary) | F9 |
| Memory Viewer | Ctrl+M |
| Includes Manager | Ctrl+I |

### Navigation

| Action | Shortcut |
|--------|----------|
| Jump to definition | Ctrl+F12 |
| Find references | Shift+F11 |
| Jump forward | F12 |
| Jump back | Shift+F12 |

### Preferences

| Action | Shortcut |
|--------|----------|
| Toggle auto indent | Ctrl+Shift+I |
| Toggle error underline | Ctrl+Shift+U |
| Toggle comment aligned | Ctrl+Shift+K |
| Toggle comment marker | Ctrl+/ |
| Set tab width | Ctrl+Alt+T |
| Set comment align column | Ctrl+Alt+C |

### Folding

| Action | Shortcut |
|--------|----------|
| Toggle fold at cursor | Ctrl+] |

---

## Keyboard Shortcuts — TUI

### File

| Action | Shortcut |
|--------|----------|
| New file | Ctrl+N |
| Open file | Ctrl+O |
| Save | Ctrl+S |
| Save As | Ctrl+Shift+S |
| Quit | Ctrl+Q |

### Edit

| Action | Shortcut |
|--------|----------|
| Undo | Ctrl+Z |
| Redo | Ctrl+Y |
| Find | Ctrl+F |
| Find next | F3 |
| Find previous | Shift+F3 |
| Replace | Ctrl+R |
| Go to line | Ctrl+G |
| Comment line(s) | Ctrl+K, then C |
| Uncomment line(s) | Ctrl+K, then U |

### Navigation

| Action | Shortcut |
|--------|----------|
| Menu bar | F10 |
| Next error/warning | F8 |
| Previous error/warning | Shift+F8 |
| Jump to definition | Ctrl+F12 |
| Find references | Ctrl+F11 |
| Jump forward | F12 |
| Jump back | Shift+F12 |

### Assembly

| Action | Shortcut |
|--------|----------|
| Check (no binary) | F5 |
| Assemble (with binary) | F9 |

### Folding

| Action | Shortcut |
|--------|----------|
| Toggle fold at cursor | Ctrl+] |

---

## Installation

### GTK Editor — .deb Package

```bash
sudo dpkg -i 6502caEdGtk_1.0.0_amd64.deb
6502caEdGtk
```

### TUI Editor — .deb Package

```bash
sudo dpkg -i 6502caEd_1.0.0_amd64.deb
6502caEd myfile.asm
```

To uninstall either package:

```bash
sudo apt remove 6502caedgtk
sudo apt remove 6502caed
```

### Building from Source

Dependencies:

```bash
sudo apt install cmake ninja-build pkg-config \
    libgtk-4-dev libgtksourceview-5-dev libncurses-dev
```

Build:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DED_BUILD_GTK=ON
cmake --build build
```

Binaries produced in `build/6502caEd/`:

- `6502caEdGtk` — GTK editor
- `6502caEd` — TUI editor
- `6502ca-cli` — command-line assembler only

---

*6502caEd — 6502/65C02/65816 Assembler and Editor*


<script>
/* Syntax highlighting fix for 6502caEd help pages.
 *
 * Pandoc uses fasm syntax highlighting which has two problems:
 *
 * 1. Partially recognised keywords: pandoc wraps the word but leaves the
 *    leading dot as a bare text node, e.g.  "."<span class="pp">if</span>
 *    The dot ends up white. We find those bare dots and colour them too.
 *
 * 2. Unrecognised keywords: .ifdef .ifndef .elif .elseif .endif .endifdef
 *    .def .define .undef .cpu — appear as plain text with no span at all.
 *    We wrap them in <span class="pp">.
 */
(function () {

  /* ── Step 1: colour bare dots preceding an existing .pp span ─────────── */
  document.querySelectorAll('pre code span.pp').forEach(function (span) {
    var prev = span.previousSibling;
    if (prev && prev.nodeType === Node.TEXT_NODE) {
      var t = prev.textContent;
      /* The bare text node often ends with "." possibly preceded by spaces */
      if (t.slice(-1) === '.') {
        var dot = document.createElement('span');
        dot.className = 'pp';
        dot.textContent = '.';
        prev.textContent = t.slice(0, -1);
        span.parentNode.insertBefore(dot, span);
      }
    }
  });

  /* ── Step 2: wrap unrecognised directive keywords in plain text nodes ── */
  var DIRECTIVES = [
    '.ifdef', '.ifndef', '.elif', '.elseif',
    '.endif', '.endifdef', '.endmacro', '.endproc', '.endscope', '.endrepeat',
    '.def', '.define', '.undef',
    '.cpu', '.pragma',
    '.halt', '.unhalt', '.alert', '.unalert',
    '#ifdef', '#ifndef', '#elif', '#elseif',
    '#endif', '#endifdef',
    '#def', '#define', '#undef',
    '#if', '#else', '#pragma',
  ];

  /* Sort longest-first so .endifdef matches before .endif etc. */
  DIRECTIVES.sort(function (a, b) { return b.length - a.length; });

  /* Build regex: match any directive followed by whitespace, end-of-string,
     comma, semicolon, or open-paren.  Case-insensitive. */
  var escaped = DIRECTIVES.map(function (d) {
    return d.replace(/\./g, '\\.').replace(/#/g, '#');
  });
  var RE = new RegExp('(' + escaped.join('|') + ')(?=[\\s,;(\\r\\n]|$)', 'gi');

  function wrapTextNode(node) {
    var text = node.textContent;
    RE.lastIndex = 0;
    if (!RE.test(text)) return;
    RE.lastIndex = 0;

    var frag = document.createDocumentFragment();
    var last = 0, m;
    while ((m = RE.exec(text)) !== null) {
      if (m.index > last) {
        frag.appendChild(document.createTextNode(text.slice(last, m.index)));
      }
      var sp = document.createElement('span');
      sp.className = 'pp';
      sp.textContent = m[1];
      frag.appendChild(sp);
      last = m.index + m[1].length;
    }
    if (last < text.length) {
      frag.appendChild(document.createTextNode(text.slice(last)));
    }
    node.parentNode.replaceChild(frag, node);
  }

  function walkCode(node) {
    /* Skip existing spans — only process raw text nodes */
    if (node.nodeType === Node.TEXT_NODE) {
      wrapTextNode(node);
    } else if (node.nodeType === Node.ELEMENT_NODE && node.tagName !== 'SPAN') {
      /* Clone childNodes list before iterating — DOM mutates during walk */
      Array.from(node.childNodes).forEach(walkCode);
    }
  }

  document.querySelectorAll('pre code').forEach(function (code) {
    Array.from(code.childNodes).forEach(walkCode);
  });

})();
</script>
