#include "buffer.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>

/* editor/main.c — TUI editor entry point.
 * Parses the optional filename argument, loads (or creates) the buffer,
 * and hands control to tui_run which does not return until the user exits.
 * The editor executable is also used as the CLI assembler: if argv[0] ends
 * with "cli" the assembler entry point is called instead. */


/* Return non-zero if path exists and is a regular file. */
static int file_exists(const char* path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}


/* Return non-zero if path has a file extension (a dot after the last slash). */
static int has_suffix(const char* path) {
  if (!path) return 0;
  const char* slash = strrchr(path, '/');
  const char* base = slash ? slash + 1 : path;
  const char* dot = strrchr(base, '.');
  return (dot && dot[1] != 0);
}

/* Set the terminal window title to "app — file" via ANSI escape sequence. */
static void set_term_title(const char* app, const char* file) {
  if (!app) app = "6502caEd";
  if (!file) file = "";
  /* OSC 0: set window title */
  fprintf(stdout, "\033]0;%s - %s\007", app, file);
  fflush(stdout);
}

int main(int argc, char** argv) {
  char path_buf[1024];
  path_buf[0] = 0;
  char suggested_buf[1024];
  suggested_buf[0] = 0;
  const char* suggested_name = NULL;

  if (argc >= 2) {
    const char* in_path = argv[1];
    if (strlen(in_path) >= sizeof(path_buf) - 5) { /* -5: 4 for ".asm" + 1 NUL */
      fprintf(stderr, "error: file name too long\n");
      return 1;
    }
    snprintf(path_buf, sizeof(path_buf), "%s", in_path);
    if (!has_suffix(path_buf)) strncat(path_buf, ".asm", sizeof(path_buf) - strlen(path_buf) - 1);

    if (!file_exists(path_buf)) {
      /* File specified but doesn't exist — create it empty so buffer_load works,
         and keep the path as the suggestion for Save As.  */
      FILE* nf = fopen(path_buf, "wb");
      if (!nf) {
        fprintf(stderr, "error: cannot create '%s'\n", path_buf);
        return 1;
      }
      fclose(nf);
    }
    strncpy(suggested_buf, path_buf, sizeof(suggested_buf) - 1);
    suggested_buf[sizeof(suggested_buf) - 1] = 0;
    suggested_name = suggested_buf;
  }
  /* No file argument: start with an empty unnamed buffer.
     The user can open or save via the File menu / file selector. */

  set_term_title("6502caEd", path_buf[0] ? path_buf : "(untitled)");

  buffer_t buf;
  if (path_buf[0]) {
    if (!buffer_load(&buf, path_buf)) {
      fprintf(stderr, "error: cannot open '%s'\n", path_buf);
      return 1;
    }
  } else {
    if (!buffer_init_empty(&buf)) {
      fprintf(stderr, "error: out of memory\n");
      return 1;
    }
  }

  tui_run(argv[0], path_buf, sizeof(path_buf), suggested_name, &buf);
  buffer_free(&buf);
  return 0;
}
