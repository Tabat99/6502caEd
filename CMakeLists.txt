cmake_minimum_required(VERSION 3.20)

# Enable Hot Reload for MSVC compilers if supported.
if (POLICY CMP0141)
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
    "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,\
      $<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,\
      $<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

# ---------------------------------------------------------------------------
# Read version numbers from versions.txt
# Rules: blank lines ignored, lines starting with # ignored,
#        all whitespace/CR stripped, split on =
# ---------------------------------------------------------------------------
function(read_version FILE KEY OUT_VAR)
  file(STRINGS "${FILE}" _lines)
  foreach(_line IN LISTS _lines)
    # Strip whitespace and CR
    string(REGEX REPLACE "[ \t\r]" "" _line "${_line}")
    # Skip blank lines and comments
    if(_line STREQUAL "" OR _line MATCHES "^#")
      continue()
    endif()
    # Match key=value
    if(_line MATCHES "^${KEY}=(.+)$")
      set(${OUT_VAR} "${CMAKE_MATCH_1}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${OUT_VAR} "0.00" PARENT_SCOPE)
endfunction()

set(_VER_FILE "${CMAKE_CURRENT_SOURCE_DIR}/versions.txt")
read_version("${_VER_FILE}" "cli" VER_CLI)
read_version("${_VER_FILE}" "tui" VER_TUI)
read_version("${_VER_FILE}" "gtk" VER_GTK)
message(STATUS "Versions: cli=${VER_CLI}  tui=${VER_TUI}  gtk=${VER_GTK}")

# Set the app name here by assigning it to the MYAPP variable
set(MYAPP 6502caEd)

project(${MYAPP} LANGUAGES C)

# For clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# When CMAKE_SYSTEM_NAME=Windows is set explicitly (cross-compile mode),
# CMake does not automatically add .exe. Set it here so all executables
# get the correct Windows extension.
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(CMAKE_EXECUTABLE_SUFFIX ".exe")
endif()

add_subdirectory(${MYAPP})


# 6502caEd.colors