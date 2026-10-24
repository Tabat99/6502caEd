#pragma once

#include "theme.h"
#include "symdb.h"
#include <stddef.h>

// Render a single line with lightweight syntax highlighting.
// This is a purely lexical highlighter (no regex, no full parse).
// suppress_diag: if non-zero, diagnostic error highlights are suppressed
// (used to avoid false positives on the line currently being edited).
void lexer_render_line(int y, int x, const char* line, size_t left, int width,
                       const theme_t* theme, int highlight_line,
                       const symdb_t* symdb, size_t line_idx0, int suppress_diag);
