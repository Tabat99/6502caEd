#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
  DIAG_WARNING = 0,
  DIAG_ERROR   = 1
} diag_kind_t;

typedef struct {
  size_t line1;     // 1-based
  size_t col1;      // 1-based
  diag_kind_t kind;
  char* message;    // heap
} diag_item_t;

typedef struct {
  diag_item_t* items;
  size_t count;
  size_t cap;
  // cached summary
  size_t num_errors;
  size_t num_warnings;
} diagdb_t;

void diagdb_init(diagdb_t* db);
void diagdb_clear(diagdb_t* db);
void diagdb_free(diagdb_t* db);

// Add a diagnostic directly (used by editor-side checks).
// line1/col1 are 1-based. message is copied.
bool diagdb_add(diagdb_t* db, size_t line1, size_t col1, diag_kind_t kind, const char* message);

// return pointer to first diag item on given 1-based line, else NULL
const diag_item_t* diagdb_first_on_line(const diagdb_t* db, size_t line1);

// navigation: find next/prev item index relative to cur (or SIZE_MAX if none)
size_t diagdb_next_index(const diagdb_t* db, size_t cur_idx);
size_t diagdb_prev_index(const diagdb_t* db, size_t cur_idx);
