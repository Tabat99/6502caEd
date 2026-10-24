
Required to build this project:

sudo apt update
sudo apt install -y pkg-config libgtk-4-dev libgtksourceview-5-dev



Licensing:

Project code: MIT. See LICENSE and THIRD_PARTY_NOTICES.txt.
The About window provides informational licence and dependency-notice viewers.
There is no startup acceptance prompt or acceptance marker requirement.

Command: vectors(vect_nmi,vect_reset,vect_irq (,vect_cop,vect_brk,vect_abort,vect_nmi,vect_irq))
This takes existing labels and does this:
This adds:
  .org $ffe4                  ; native mode vectors
  equw vect_cop
  equw vect_brk
  equw vect_abort
  equw vect_nmi
  equw 0
  equw vect_irq
  .org $fffa                  ; emulation mode vectors
  equw vect_nmi
  equw vect_reset
  equw vect_irq
The same labels can be used for multiple parts
This can be used to set justb 65(C)02 vactors or 65816 full vectors


the following objects are supported:
  r.a = accumluator
  r.x = x index register
  r.y = y index register
  regs = a,x,y registers
  f.c = c flag
  f.z = z flag
  f.n = n flag
  f.v = v flag
  flags = flag register

  register and address compare:
    ==  <=  >=  !=
  flags:
    !
  
if r.x==<value>
begin
    <command block>
else
    <command block>
end

if <address or address variable>==<value>
begin
    <command block>
else
    <command block>
end

if f.c
begin
    <command block>
else
    <command block>
end

if !f.c
begin
    <command block>
else
    <command block>
end

if f.c and $4000=#$aa
begin
    <command block>
else
    <command block>
end

if !f.c or $4000=$7aca
begin
    <command block>
else
    <command block>
end

push(r.a), push(r.x), push(r.y), push(flags), push(all)
pull(r.a), pull(r.x), pull(r.y), pull(flags), pull(all)

function name 
preserve r.a,r.x,r.y,flags
begin
  <command block>
end

calc(<equation>)
  <equation> can be any combination containing
    $address
    #value
    brackets can be used to indicate process order
    + - * / 

  


6502caEd

* What it is *
A 6502/65C02/65816 assembler suite, written in C11. It has three deliverables
sharing one assembler core:
  Binary       Source      Description
  6502ca-cli   cli/main.c  Stand-alone CLI assembler
  6502caEd     editor/     ncurses TUI editor with assembler built in
  6502caEdGtk  gui/main.c  GTK4 + GtkSourceView-5 GUI editor

* Build System *
CMake, two-level. The outer CMakeLists.txt (root) sets MYAPP=6502caEd and adds the 6502caEd/ subdirectory.
The inner CMakeLists.txt defines:
  asmcore       static library (all core/ files)
  6502ca-cli    executable linked against asmcore
  6502caEd TUI  executable linked against asmcore + ncurses
  6502caEdGtk   optional (flag ED_BUILD_GTK=ON), links GTK4 + GtkSourceView-5
Build IDs: TUI = ed3patch2, GTK = gtkstep1.

* Assembler Core (core/) *
  File       Role
  asm.c      Two-pass assembler, instruction encoding, listing output
  preproc.c  Preprocessor: #def/$name macros, .include, conditionals
  expr.c     Expression evaluator (numeric & string, operators, labels)
  sym.c      FNV-1a open-addressing hash table for labels/constants
  diag.c     Diagnostic collector + pretty-print with caret underlines
  source.c   File loading, comment stripping (;, //, /* */)
  util.c     sv_t string views, integer literal parsing, identifier helpers

Key types: asm_settings_t (all assembler options), diags_t (error/warning list),
symtab_t (case-insensitive label map), sv_t (non-owning string slice used
everywhere to avoid malloc). The assembler supports three CPUs (ASM_CPU_6502,
ASM_CPU_65C02, ASM_CPU_65816), undocumented opcodes, BBC BASIC-style listing
output (hex prefix $/&/none, address/bytes columns), symbol dumping, and 
overwrite/guard warnings.

* TUI Editor (editor/) *
File         Role
  tui.c      Everything: main loop, drawing, input, menus, dialogs
  runner.c   Invokes assembler (check-only or full build), collects runner_result_t
  buffer.c   Line-array document model (insert/backspace/newline/replace)
  lexer.c    Ncurses syntax highlighter (purely lexical, no regex)
  symdb.c    Editor-side symbol DB rebuilt on each keystroke for highlighting
  diagdb.c   Diagnostic DB with parse/add/navigate; separate from core diags_t
  fold.c     Code folding (comment blocks ≥2 lines; macro bodies)
  theme.c    Colour theme loader from 6502caEd.theme file
  main.c     Entry point: arg parse, buffer load/create, calls tui_run()

prefs_t (global g_prefs, persisted to ~/.config/6502caEd/prefs):
tab_width, comment_col, show_problems, dark_mode, auto_indent, clear_mem_on_asm,
error_underline, keep_comment_aligned, allow_undoc, fold_comments, fold_macros,
autocomplete.

Key TUI features: undo/redo stack (snapshots), selection with copy/cut/paste (system
clipboard via xclip/wl-clipboard), jump history (jump_buf_t), bookmarks (saved to disk),
live background assembly check (50 ms debounce), memory map dialog, file selector dialog,
menubar (F10), context menu, Ctrl+K chord commands, mouse support (xterm any-event mode),
SIGWINCH resize handling.

* GTK Editor (gui/main.c) *
At 21,266 lines it's the largest file. Structured around an AppState struct.
Features beyond TUI: multi-tab, include manager tree (IncludeNode GObject, GtkTreeStore),
GTK4 dark-mode hinting, project-level assembly (assembles all open tabs in order),
colour file (6502caEd.colours), prefs loaded from user config / exe dir / project dir
(layered), session restore (gtk_session_path), zoom.

* Shared / Cross-cutting *
  - license_text.h - embedded MIT licence text for the GUI and CLI.
  - runner.h/c — shared between TUI and GTK; wraps the core assembler for editor use. Produces
    runner_result_t with diag list, address ranges, byte count.
  - fold.c — shared between TUI and GTK (both in CMakeLists.txt).
  - Config/theme files searched in: current dir -> ~/.config/6502caEd/ -> /usr/share/6502caEd/.

* File size summary *
~44,000 lines total. The two largest files (tui.c at ~10K and gui/main.c at ~21K) each contain
an entire editor's UI logic monolithically.
