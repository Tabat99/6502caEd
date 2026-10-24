#pragma once
#include "buffer.h"
#include <stddef.h>

typedef struct {
  int row;      /* viewport row */
  int col;      /* document column */
  size_t top;   /* first visible line */
  size_t left;  /* horizontal scroll (document col) */
} view_t;

// editor_exe_path is argv[0] from main; used to locate sibling 6502ca-cli
/*
 * Runs the editor TUI.
 * io_path: current file path; may be empty for an unnamed buffer.
 * suggested_path: optional suggested name for Save As when io_path is empty (e.g. from CLI arg that didn't exist).
 */
void tui_run(const char* editor_exe_path, char* io_path, size_t io_path_cap, const char* suggested_path, buffer_t* buf);
