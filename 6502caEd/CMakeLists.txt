cmake_minimum_required(VERSION 3.16)
project(${MYAPP} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(asmcore
  core/asm.c
  core/expr.c
  core/preproc.c
  core/source.c
  core/sym.c
  core/diag.c
  core/util.c
)

target_include_directories(asmcore PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/core
)

add_executable(6502ca-cli
  cli/main.c
)
target_link_libraries(6502ca-cli PRIVATE asmcore)
target_compile_definitions(6502ca-cli PRIVATE APP_VERSION="${VER_CLI}")

option(ED_BUILD_TUI "Build ncurses TUI editor (6502caEd)" ON)
if(ED_BUILD_TUI)
  add_executable(6502caEd
    editor/main.c
    editor/buffer.c
    editor/tui.c
    editor/fold.c
    editor/diagdb.c
    editor/runner.c
    editor/theme.c
    editor/lexer.c
    editor/symdb.c
  )

  if(MINGW OR MSYS OR WIN32)
    # PDCurses on MSYS2 UCRT64 — bypass FindCurses which picks up ncurses.
    # Headers: /ucrt64/include/pdcurses/curses.h  (include as <pdcurses/curses.h>)
    # Library: /ucrt64/lib/libpdcurses.dll.a
    find_path(PDCURSES_INCLUDE_DIR pdcurses/curses.h
      HINTS /ucrt64/include ${CMAKE_SYSROOT}/include
    )
    find_library(PDCURSES_LIBRARY
      NAMES libpdcurses_wincon.a pdcurses_wincon
      HINTS /ucrt64/lib ${CMAKE_SYSROOT}/lib
    )
    if(NOT PDCURSES_LIBRARY)
      # fallback to generic
      find_library(PDCURSES_LIBRARY
        NAMES libpdcurses.a pdcurses
        HINTS /ucrt64/lib ${CMAKE_SYSROOT}/lib
      )
    endif()
    if(NOT PDCURSES_INCLUDE_DIR OR NOT PDCURSES_LIBRARY)
      message(FATAL_ERROR "PDCurses not found. Run: pacman -S mingw-w64-ucrt-x86_64-pdcurses")
    endif()
    message(STATUS "TUI: PDCurses include=${PDCURSES_INCLUDE_DIR} lib=${PDCURSES_LIBRARY}")
    target_include_directories(6502caEd PRIVATE ${PDCURSES_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/editor)
    target_link_libraries(6502caEd PRIVATE asmcore ${PDCURSES_LIBRARY} winmm)
    # PDC_WIDE + PDC_FORCE_UTF8 matches the _u64 symbol variant in the library
    target_compile_definitions(6502caEd PRIVATE ED_BUILD_ID="ed3patch2" PDC_WIDE PDC_FORCE_UTF8 APP_VERSION="${VER_TUI}")
  else()
    find_package(Curses REQUIRED)
    target_include_directories(6502caEd PRIVATE ${CURSES_INCLUDE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/editor)
    target_link_libraries(6502caEd PRIVATE asmcore ${CURSES_LIBRARIES})
    target_compile_definitions(6502caEd PRIVATE ED_BUILD_ID="ed3patch2" APP_VERSION="${VER_TUI}")
  endif()
endif()


option(ED_BUILD_GTK "Build GTK GUI frontend (6502caEdGtk)" OFF)
if(ED_BUILD_GTK)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
  pkg_check_modules(GTSV REQUIRED IMPORTED_TARGET gtksourceview-5)

  if(WIN32)
    add_executable(6502caEdGtk WIN32
      gui/main.c
      editor/runner.c
      editor/fold.c
    )
  else()
    add_executable(6502caEdGtk
      gui/main.c
      editor/runner.c
      editor/fold.c
    )
  endif()

  target_link_libraries(6502caEdGtk PRIVATE asmcore PkgConfig::GTK4 PkgConfig::GTSV)
  target_include_directories(6502caEdGtk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/editor)

  # ✅ NOW the target exists, so this works:
  target_compile_definitions(6502caEdGtk PRIVATE
    ED_BUILD_ID="gtkstep1"
    ED_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    APP_VERSION="${VER_GTK}"
  )
endif()


# ---------------------------------------------------------------------------
# Simulator library — built as a static lib so both GTK and future TUI
# targets can link it without recompiling.
# ---------------------------------------------------------------------------
option(ED_BUILD_SIM "Build cycle-accurate simulator (requires ED_BUILD_GTK)" OFF)
if(ED_BUILD_SIM)
  if(NOT ED_BUILD_GTK)
    message(FATAL_ERROR "ED_BUILD_SIM requires ED_BUILD_GTK to be ON")
  endif()

  # Simulator core static library
  add_library(simcore STATIC
    sim/cpu.c
    sim/mem.c
    sim/via.c
    sim/acia.c
    sim/sim.c
    sim/simcfg.c
    sim/sim_gtk.c
    sim/simprofile.c
    sim/lcd.c
    sim/lcd_cfg.c
    sim/lcd_gtk.c
    sim/spi_cfg.c
    sim/sd.c
    sim/sd_gtk.c
  )

  target_include_directories(simcore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/sim
    ${CMAKE_CURRENT_SOURCE_DIR}/editor
  )

  # simcore includes gtk/gtk.h via sim_gtk.h so needs GTK headers
  target_link_libraries(simcore PUBLIC PkgConfig::GTK4)

  target_compile_definitions(simcore PRIVATE
    APP_VERSION="${VER_GTK}"
  )

  if(WIN32)
    target_link_libraries(simcore PRIVATE ws2_32)
  endif()

  # pthread — needed by sim.c
  find_package(Threads REQUIRED)
  target_link_libraries(simcore PRIVATE Threads::Threads)

  # Link simcore into the GTK editor
  target_link_libraries(6502caEdGtk PRIVATE simcore)
  target_compile_definitions(6502caEdGtk PRIVATE ED_BUILD_SIM)

  message(STATUS "Simulator: enabled")
endif()

# Embed the application icon in Windows executables.
if(WIN32)
  enable_language(RC)
  foreach(_app_target IN ITEMS 6502ca-cli 6502caEd 6502caEdGtk)
    if(TARGET ${_app_target})
      target_sources(${_app_target} PRIVATE resources/6502caEd.rc)
      target_include_directories(${_app_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/resources")
    endif()
  endforeach()
endif()
