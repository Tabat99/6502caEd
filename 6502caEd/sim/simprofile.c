/* simprofile.c — Simulator memory map profile I/O and management */
#include "simprofile.h"
#include "spi_cfg.h"
#include "lcd_cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>

#ifdef _WIN32
#  include <direct.h>
#  define MKDIR(p) _mkdir(p)
#  define PATH_SEP "\\"
#else
#  define MKDIR(p) mkdir(p, 0755)
#  define PATH_SEP "/"
#endif

/* -----------------------------------------------------------------------
 * Directory helpers
 */
const char *simprofile_global_dir(void) {
    static char buf[512];
    const char *home = getenv("HOME");
    if (!home) home = ".";
    snprintf(buf, sizeof(buf), "%s" PATH_SEP ".6502caed" PATH_SEP "profiles",
             home);
    return buf;
}

const char *simprofile_project_dir(const char *project_dir) {
    return project_dir ? project_dir : simprofile_global_dir();
}

static void ensure_dir_recursive(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            struct stat st;
            if (stat(tmp, &st) != 0)
                MKDIR(tmp);
            *p = '/';
        }
    }
    struct stat st;
    if (stat(tmp, &st) != 0)
        MKDIR(tmp);
}

static void ensure_dir(const char *path) {
    ensure_dir_recursive(path);
}

/* -----------------------------------------------------------------------
 * Defaults
 */
void simprofile_defaults(simprofile_t *p) {
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "New Profile");
    snprintf(p->description, sizeof(p->description),
             "Default memory map profile");
    p->cpu_type  = PROFILE_CPU_6502;
    p->is_global = false;

    /* ROM ranges: all disabled by default */
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        p->rom[i].enabled  = false;
        p->rom[i].bank     = PROFILE_BANK_ALL;
        p->rom[i].from     = 0x0000;
        p->rom[i].to       = 0x0000;
        p->rom[i].rw_bit   = 0;
        p->rom[i].lock_bit = 0;
    }

    /* ROM control register: disabled by default */
    p->rom_ctrl.enabled = false;
    p->rom_ctrl.bank    = PROFILE_BANK_ALL;
    p->rom_ctrl.addr    = 0x7FFF;

    /* IO devices with standard addresses */
    p->via1.enabled = true;
    p->via1.bank    = PROFILE_BANK_ALL;
    p->via1.addr    = 0x9000;

    p->via2.enabled = true;
    p->via2.bank    = PROFILE_BANK_ALL;
    p->via2.addr    = 0x9010;

    p->acia.enabled = true;
    p->acia.bank    = PROFILE_BANK_ALL;
    p->acia.addr    = 0x9020;

    p->simio_tx.enabled = true;
    p->simio_tx.bank    = PROFILE_BANK_ALL;
    p->simio_tx.addr    = 0x9030;

    p->simio_rx.enabled = true;
    p->simio_rx.bank    = PROFILE_BANK_ALL;
    p->simio_rx.addr    = 0x9031;

    /* LCD: disabled by default */
    lcd_cfg_defaults(&p->lcd);
    spi_cfg_defaults(&p->sd);

    /* Banking: disabled by default */
    p->bank_reg.enabled = false;
    p->bank_reg.bank    = PROFILE_BANK_ALL;
    p->bank_reg.addr    = 0x8000;
    p->bank_window      = 0x8000;
    p->bank_window_sz   = 0x8000;
    p->bank_num         = 0;
}

/* -----------------------------------------------------------------------
 * INI parser helpers
 */
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)*(e-1))) *--e = 0;
    return s;
}

static bool parse_bool(const char *v) {
    return atoi(v) != 0 || strcasecmp(v, "true") == 0
                        || strcasecmp(v, "yes")  == 0;
}

static uint16_t parse_hex16(const char *v) {
    return (uint16_t)strtoul(v, NULL, 16);
}

static int parse_bank(const char *v) {
    if (strcmp(v, "-1") == 0 || strcasecmp(v, "all") == 0)
        return PROFILE_BANK_ALL;
    return atoi(v);
}

static int parse_bit(const char *v) {
    int n = atoi(v);
    return (n < 0) ? -1 : n;
}

/* -----------------------------------------------------------------------
 * Load
 */
bool simprofile_load(simprofile_t *p, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    simprofile_defaults(p);
    snprintf(p->filepath, sizeof(p->filepath), "%s", path);

    /* Determine is_global from path */
    const char *gdir = simprofile_global_dir();
    p->is_global = (strncmp(path, gdir, strlen(gdir)) == 0);

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || *s == ';' || *s == 0) continue;

        /* Section header */
        if (*s == '[') {
            char *e = strchr(s, ']');
            if (e) { *e = 0; snprintf(section, sizeof(section), "%s", s+1); }
            continue;
        }

        /* Key = value */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(s);
        char *val = trim(eq + 1);

        /* [profile] section */
        if (strcmp(section, "profile") == 0) {
            if      (!strcmp(key, "name"))        snprintf(p->name, sizeof(p->name), "%s", val);
            else if (!strcmp(key, "description")) snprintf(p->description, sizeof(p->description), "%s", val);
            else if (!strcmp(key, "global"))      p->is_global = parse_bool(val);
            else if (!strcmp(key, "cpu")) {
                if      (!strcasecmp(val, "6502u"))  p->cpu_type = PROFILE_CPU_6502U;
                else if (!strcasecmp(val, "65c02"))  p->cpu_type = PROFILE_CPU_65C02;
                else if (!strcasecmp(val, "65816"))  p->cpu_type = PROFILE_CPU_65816;
                else                                  p->cpu_type = PROFILE_CPU_6502;
            }
        }

        /* [romN] sections */
        else if (strncmp(section, "rom", 3) == 0 && isdigit((unsigned char)section[3])) {
            int i = atoi(section + 3);
            if (i >= 0 && i < PROFILE_ROM_MAX) {
                if      (!strcmp(key, "enabled"))  p->rom[i].enabled  = parse_bool(val);
                else if (!strcmp(key, "bank"))     p->rom[i].bank     = parse_bank(val);
                else if (!strcmp(key, "from"))     p->rom[i].from     = parse_hex16(val);
                else if (!strcmp(key, "to"))       p->rom[i].to       = parse_hex16(val);
                else if (!strcmp(key, "rw_bit"))   p->rom[i].rw_bit   = parse_bit(val);
                else if (!strcmp(key, "lock_bit")) p->rom[i].lock_bit = parse_bit(val);
            }
        }

        /* [rom_ctrl] section */
        else if (strcmp(section, "rom_ctrl") == 0) {
            if      (!strcmp(key, "enabled")) p->rom_ctrl.enabled = parse_bool(val);
            else if (!strcmp(key, "bank"))    p->rom_ctrl.bank    = parse_bank(val);
            else if (!strcmp(key, "addr"))    p->rom_ctrl.addr    = parse_hex16(val);
        }

        /* [io] section */
        else if (strcmp(section, "io") == 0) {
#define IO_PARSE(dev) \
            if      (!strcmp(key, #dev "_enable")) p->dev.enabled = parse_bool(val); \
            else if (!strcmp(key, #dev "_bank"))   p->dev.bank    = parse_bank(val); \
            else if (!strcmp(key, #dev "_addr"))   p->dev.addr    = parse_hex16(val);
            IO_PARSE(via1)
            IO_PARSE(via2)
            IO_PARSE(acia)
            IO_PARSE(simio_tx)
            IO_PARSE(simio_rx)
        } else if (strcmp(section, "lcd") == 0) {
            if      (!strcmp(key, "enabled"))   p->lcd.enabled    = parse_bool(val);
            else if (!strcmp(key, "bus_width")) p->lcd.bus_width  = atoi(val);
            else if (!strcmp(key, "cols"))      p->lcd.cols       = atoi(val);
            else if (!strcmp(key, "rows"))      p->lcd.rows       = atoi(val);
            else if (!strcmp(key, "strict"))    p->lcd.strict     = parse_bool(val);
            else if (!strcmp(key, "rs"))        lcd_pin_parse(val, &p->lcd.rs);
            else if (!strcmp(key, "rw"))        lcd_pin_parse(val, &p->lcd.rw);
            else if (!strcmp(key, "e"))         lcd_pin_parse(val, &p->lcd.e);
            else if (key[0] == 'd' && key[1] >= '0' && key[1] <= '7') {
                int pin_num = key[1] - '0';
                /* In 4-bit mode, d4-d7 map to data[0-3] */
                int idx = (p->lcd.bus_width == 4 && pin_num >= 4)
                          ? pin_num - 4 : pin_num;
                lcd_pin_parse(val, &p->lcd.data[idx]);
            }
#undef IO_PARSE
        }

        /* [sd] section */
        else if (strcmp(section, "sd") == 0) {
            if      (!strcmp(key, "enabled"))  p->sd.enabled = parse_bool(val);
            else if (!strcmp(key, "strict"))   p->sd.strict  = parse_bool(val);
            else if (!strcmp(key, "read_us")) {
                char *end; unsigned long n = strtoul(val, &end, 10);
                if (*val && !*end && val[0] != '-' && n <= 1000000UL)
                    p->sd.timing.read_us = (uint32_t)n;
            }
            else if (!strcmp(key, "write_us")) {
                char *end; unsigned long n = strtoul(val, &end, 10);
                if (*val && !*end && val[0] != '-' && n <= 1000000UL)
                    p->sd.timing.write_us = (uint32_t)n;
            }
            else if (!strcmp(key, "erase_us")) {
                char *end; unsigned long n = strtoul(val, &end, 10);
                if (*val && !*end && val[0] != '-' && n <= 10000000UL)
                    p->sd.timing.erase_us = (uint32_t)n;
            }
            else if (!strcmp(key, "max_spi_hz")) {
                char *end; unsigned long n = strtoul(val, &end, 10);
                if (*val && !*end && val[0] != '-' && n <= 25000000UL)
                    p->sd.timing.max_spi_hz = (uint32_t)n;
            }
            else if (!strcmp(key, "card_type")) {
                if (!strcasecmp(val, "sdsc"))      p->sd.card_mode = SD_CARD_SDSC;
                else if (!strcasecmp(val, "sdhc")) p->sd.card_mode = SD_CARD_SDHC;
                else                                p->sd.card_mode = SD_CARD_AUTO;
            }
            else if (!strcmp(key, "sck"))      spi_pin_parse(val, &p->sd.sck);
            else if (!strcmp(key, "mosi"))     spi_pin_parse(val, &p->sd.mosi);
            else if (!strcmp(key, "miso"))     spi_pin_parse(val, &p->sd.miso);
            else if (!strcmp(key, "cs"))       spi_pin_parse(val, &p->sd.cs);
            else if (!strcmp(key, "image"))    snprintf(p->sd.image_path,
                                                   sizeof(p->sd.image_path),
                                                   "%s", val);
        }

        /* [banking] section */
        else if (strcmp(section, "banking") == 0) {
            if      (!strcmp(key, "enable"))     p->bank_reg.enabled = parse_bool(val);
            else if (!strcmp(key, "bank"))       p->bank_reg.bank    = parse_bank(val);
            else if (!strcmp(key, "addr"))       p->bank_reg.addr    = parse_hex16(val);
            else if (!strcmp(key, "window"))     p->bank_window      = parse_hex16(val);
            else if (!strcmp(key, "window_sz"))  p->bank_window_sz   = parse_hex16(val);
            else if (!strcmp(key, "num_banks"))  p->bank_num         = (uint8_t)atoi(val);
        }
    }
    fclose(f);
    return true;
}

/* -----------------------------------------------------------------------
 * Save
 */
static void write_io(FILE *f, const char *name, const profile_io_t *io) {
    fprintf(f, "%s_enable = %d\n", name, io->enabled ? 1 : 0);
    if (io->bank == PROFILE_BANK_ALL)
        fprintf(f, "%s_bank   = -1\n", name);
    else
        fprintf(f, "%s_bank   = %d\n", name, io->bank);
    fprintf(f, "%s_addr   = %04X\n\n", name, io->addr);
}

bool simprofile_save_as(simprofile_t *p, const char *path) {
    /* Ensure parent directory exists */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sl = strrchr(dir, '/');
    if (!sl) sl = strrchr(dir, '\\');
    if (sl) { *sl = 0; ensure_dir(dir); }

    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "; 6502caEd simulator memory map profile\n\n");
    fprintf(f, "[profile]\n");
    fprintf(f, "name        = %s\n", p->name);
    fprintf(f, "description = %s\n", p->description);
    fprintf(f, "global      = %d\n", p->is_global ? 1 : 0);
    const char *cpu_str[] = {"6502", "6502u", "65c02", "65816"};
    fprintf(f, "cpu         = %s\n\n", cpu_str[p->cpu_type & 3]);

    fprintf(f, "; ROM ranges (0-%d)\n", PROFILE_ROM_MAX - 1);
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        const profile_rom_t *r = &p->rom[i];
        fprintf(f, "[rom%d]\n", i);
        fprintf(f, "enabled  = %d\n", r->enabled ? 1 : 0);
        if (r->bank == PROFILE_BANK_ALL) fprintf(f, "bank     = -1\n");
        else                             fprintf(f, "bank     = %d\n",  r->bank);
        fprintf(f, "from     = %04X\n", r->from);
        fprintf(f, "to       = %04X\n", r->to);
        fprintf(f, "rw_bit   = %d\n",   r->rw_bit);
        fprintf(f, "lock_bit = %d\n\n", r->lock_bit);
    }

    fprintf(f, "[rom_ctrl]\n");
    fprintf(f, "enabled = %d\n", p->rom_ctrl.enabled ? 1 : 0);
    if (p->rom_ctrl.bank == PROFILE_BANK_ALL) fprintf(f, "bank    = -1\n");
    else                                       fprintf(f, "bank    = %d\n", p->rom_ctrl.bank);
    fprintf(f, "addr    = %04X\n\n", p->rom_ctrl.addr);

    fprintf(f, "[io]\n");
    write_io(f, "via1",     &p->via1);
    write_io(f, "via2",     &p->via2);
    write_io(f, "acia",     &p->acia);
    write_io(f, "simio_tx", &p->simio_tx);
    write_io(f, "simio_rx", &p->simio_rx);

    fprintf(f, "[banking]\n");
    fprintf(f, "enable    = %d\n",   p->bank_reg.enabled ? 1 : 0);
    if (p->bank_reg.bank == PROFILE_BANK_ALL) fprintf(f, "bank      = -1\n");
    else                                       fprintf(f, "bank      = %d\n", p->bank_reg.bank);
    fprintf(f, "addr      = %04X\n", p->bank_reg.addr);
    fprintf(f, "window    = %04X\n", p->bank_window);
    fprintf(f, "window_sz = %04X\n", p->bank_window_sz);
    fprintf(f, "num_banks = %d\n",   p->bank_num);

    /* LCD section */
    fprintf(f, "\n[lcd]\n");
    fprintf(f, "enabled    = %d\n", p->lcd.enabled ? 1 : 0);
    fprintf(f, "bus_width  = %d\n", p->lcd.bus_width);
    fprintf(f, "cols       = %d\n", p->lcd.cols);
    fprintf(f, "rows       = %d\n", p->lcd.rows);
    fprintf(f, "strict     = %d\n", p->lcd.strict ? 1 : 0);
    {
        char pb[8];
        lcd_pin_format(&p->lcd.rs, pb, sizeof(pb));
        fprintf(f, "rs         = %s\n", pb);
        lcd_pin_format(&p->lcd.rw, pb, sizeof(pb));
        fprintf(f, "rw         = %s\n", pb);
        lcd_pin_format(&p->lcd.e,  pb, sizeof(pb));
        fprintf(f, "e          = %s\n", pb);
        if (p->lcd.bus_width == 4) {
            /* 4-bit mode: data[0-3] = D4-D7 */
            for (int i = 0; i < 4; i++) {
                lcd_pin_format(&p->lcd.data[i], pb, sizeof(pb));
                fprintf(f, "d%d         = %s\n", i + 4, pb);
            }
        } else {
            /* 8-bit mode: data[0-7] = D0-D7 */
            for (int i = 0; i < 8; i++) {
                lcd_pin_format(&p->lcd.data[i], pb, sizeof(pb));
                fprintf(f, "d%d         = %s\n", i, pb);
            }
        }
    }

    /* SD card section */
    fprintf(f, "\n[sd]\n");
    fprintf(f, "enabled    = %d\n", p->sd.enabled ? 1 : 0);
    fprintf(f, "strict     = %d\n", p->sd.strict  ? 1 : 0);
    fprintf(f, "read_us = %lu\n", (unsigned long)p->sd.timing.read_us);
    fprintf(f, "write_us = %lu\n", (unsigned long)p->sd.timing.write_us);
    fprintf(f, "erase_us = %lu\n", (unsigned long)p->sd.timing.erase_us);
    fprintf(f, "max_spi_hz = %lu\n", (unsigned long)p->sd.timing.max_spi_hz);
    fprintf(f, "card_type  = %s\n",
            p->sd.card_mode == SD_CARD_SDSC ? "sdsc" :
            p->sd.card_mode == SD_CARD_SDHC ? "sdhc" : "auto");
    {
        char pb[8];
        spi_pin_format(&p->sd.sck,  pb, sizeof(pb)); fprintf(f, "sck        = %s\n", pb);
        spi_pin_format(&p->sd.mosi, pb, sizeof(pb)); fprintf(f, "mosi       = %s\n", pb);
        spi_pin_format(&p->sd.miso, pb, sizeof(pb)); fprintf(f, "miso       = %s\n", pb);
        spi_pin_format(&p->sd.cs,   pb, sizeof(pb)); fprintf(f, "cs         = %s\n", pb);
    }
    if (p->sd.image_path[0])
        fprintf(f, "image      = %s\n", p->sd.image_path);

    fclose(f);
    snprintf(p->filepath, sizeof(p->filepath), "%s", path);
    return true;
}

bool simprofile_save(const simprofile_t *p) {
    if (!p->filepath[0]) return false;
    simprofile_t tmp = *p;
    return simprofile_save_as(&tmp, p->filepath);
}

bool simprofile_delete(const simprofile_t *p) {
    if (!p->filepath[0]) return false;
    return remove(p->filepath) == 0;
}

void simprofile_copy(simprofile_t *dst, const simprofile_t *src) {
    *dst = *src;
    dst->filepath[0] = 0;  /* unsaved copy */
}

/* -----------------------------------------------------------------------
 * Profile discovery
 */
static bool has_suffix(const char *name, const char *suffix) {
    size_t nl = strlen(name), sl = strlen(suffix);
    return nl >= sl && strcmp(name + nl - sl, suffix) == 0;
}

static simprofile_list_t *list_new(void) {
    simprofile_list_t *l = calloc(1, sizeof(*l));
    l->capacity = 16;
    l->items    = calloc(l->capacity, sizeof(simprofile_t *));
    return l;
}

static void list_push(simprofile_list_t *l, simprofile_t *p) {
    if (l->count >= l->capacity) {
        l->capacity *= 2;
        l->items = realloc(l->items, l->capacity * sizeof(simprofile_t *));
    }
    l->items[l->count++] = p;
}

static void scan_dir(simprofile_list_t *list, const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!has_suffix(e->d_name, ".simprofile")) continue;
        char path[768];
        snprintf(path, sizeof(path), "%s" PATH_SEP "%s", dir, e->d_name);
        simprofile_t *p = calloc(1, sizeof(*p));
        if (simprofile_load(p, path))
            list_push(list, p);
        else
            free(p);
    }
    closedir(d);
}

simprofile_list_t *simprofile_list_load(const char *project_dir) {
    simprofile_list_t *list = list_new();
    scan_dir(list, simprofile_global_dir());
    if (project_dir && strcmp(project_dir, simprofile_global_dir()) != 0)
        scan_dir(list, project_dir);
    return list;
}

void simprofile_list_free(simprofile_list_t *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) free(list->items[i]);
    free(list->items);
    free(list);
}

simprofile_t *simprofile_find_default(const char *project_dir) {
    char path[768];
    simprofile_t *p = calloc(1, sizeof(*p));

    /* 1. project default */
    if (project_dir) {
        snprintf(path, sizeof(path), "%s" PATH_SEP "default.simprofile",
                 project_dir);
        if (simprofile_load(p, path)) return p;
    }

    /* 2. global default */
    snprintf(path, sizeof(path), "%s" PATH_SEP "default.simprofile",
             simprofile_global_dir());
    if (simprofile_load(p, path)) return p;

    /* 3. first profile in project dir */
    if (project_dir) {
        DIR *d = opendir(project_dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!has_suffix(e->d_name, ".simprofile")) continue;
                snprintf(path, sizeof(path), "%s" PATH_SEP "%s",
                         project_dir, e->d_name);
                if (simprofile_load(p, path)) { closedir(d); return p; }
            }
            closedir(d);
        }
    }

    /* 4. first global profile */
    {
        DIR *d = opendir(simprofile_global_dir());
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!has_suffix(e->d_name, ".simprofile")) continue;
                snprintf(path, sizeof(path), "%s" PATH_SEP "%s",
                         simprofile_global_dir(), e->d_name);
                if (simprofile_load(p, path)) { closedir(d); return p; }
            }
            closedir(d);
        }
    }

    /* 5. no profiles found — create and save a default to the global dir */
    simprofile_defaults(p);
    snprintf(p->name, sizeof(p->name), "Default");
    p->is_global = true;
    {
        /* Ensure ~/.6502caed/profiles/ exists */
        const char *gdir = simprofile_global_dir();
        char parent[512];
        snprintf(parent, sizeof(parent), "%s", gdir);
        /* mkdir parents — walk back to find ~/.6502caed */
        char *slash = strrchr(parent, PATH_SEP[0]);
        if (slash) {
            *slash = 0;
            MKDIR(parent);  /* ~/.6502caed */
            *slash = PATH_SEP[0];
        }
        MKDIR(gdir);        /* ~/.6502caed/profiles */
        snprintf(path, sizeof(path), "%s" PATH_SEP "default.simprofile", gdir);
        snprintf(p->filepath, sizeof(p->filepath), "%s", path);
        simprofile_save_as(p, path);
    }
    return p;
}

bool simprofile_set_default(simprofile_t *p, const char *project_dir) {
    char dest[768];
    const char *dir = p->is_global ? simprofile_global_dir() : project_dir;
    if (!dir) dir = simprofile_global_dir();
    ensure_dir(dir);
    snprintf(dest, sizeof(dest), "%s" PATH_SEP "default.simprofile", dir);
    return simprofile_save_as(p, dest);
}

/* -----------------------------------------------------------------------
 * Runtime helpers
 */
/* Returns a description of the first overlapping ROM range pair, or NULL.
 * Caller must not free the returned string (static buffer). */
const char *simprofile_check_overlaps(const simprofile_t *p) {
    static char buf[128];
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        if (!p->rom[i].enabled) continue;
        for (int j = i + 1; j < PROFILE_ROM_MAX; j++) {
            if (!p->rom[j].enabled) continue;
            /* Check if banks overlap (either is ALL, or same bank) */
            bool banks_overlap =
                (p->rom[i].bank == PROFILE_BANK_ALL) ||
                (p->rom[j].bank == PROFILE_BANK_ALL) ||
                (p->rom[i].bank == p->rom[j].bank);
            if (!banks_overlap) continue;
            /* Check address overlap */
            if (p->rom[i].from <= p->rom[j].to &&
                p->rom[j].from <= p->rom[i].to) {
                snprintf(buf, sizeof(buf),
                    "ROM%d ($%04X-$%04X) overlaps ROM%d ($%04X-$%04X)",
                    i, p->rom[i].from, p->rom[i].to,
                    j, p->rom[j].from, p->rom[j].to);
                return buf;
            }
        }
    }
    return NULL;
}

void simprofile_reset_locks(simprofile_t *p) {
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        p->rom[i].rw_state   = false;
        p->rom[i].lock_state = false;
    }
}

uint8_t simprofile_ctrl_read(const simprofile_t *p) {
    uint8_t val = 0;
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        if (!p->rom[i].enabled) continue;
        if (p->rom[i].rw_bit   >= 0 && p->rom[i].rw_state)
            val |= (1u << p->rom[i].rw_bit);
        if (p->rom[i].lock_bit >= 0 && p->rom[i].lock_state)
            val |= (1u << p->rom[i].lock_bit);
    }
    return val;
}

void simprofile_ctrl_write(simprofile_t *p, uint8_t val) {
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        profile_rom_t *r = &p->rom[i];
        if (!r->enabled) continue;
        if (r->lock_state) continue;
        if (r->rw_bit >= 0)
            r->rw_state = (val >> r->rw_bit) & 1;
        if (r->lock_bit >= 0 && ((val >> r->lock_bit) & 1))
            r->lock_state = true;
    }
}

static bool bank_match(int rom_bank, int query_bank) {
    return rom_bank == PROFILE_BANK_ALL || rom_bank == query_bank;
}

bool simprofile_is_rom(const simprofile_t *p, uint16_t addr, int bank) {
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        const profile_rom_t *r = &p->rom[i];
        if (!r->enabled) continue;
        if (!bank_match(r->bank, bank)) continue;
        if (addr < r->from || addr > r->to) continue;
        /* ROM read if rw_state == 0 */
        return !r->rw_state;
    }
    return false;
}

bool simprofile_is_ram_overlay(const simprofile_t *p, uint16_t addr, int bank) {
    for (int i = 0; i < PROFILE_ROM_MAX; i++) {
        const profile_rom_t *r = &p->rom[i];
        if (!r->enabled) continue;
        if (!bank_match(r->bank, bank)) continue;
        if (addr < r->from || addr > r->to) continue;
        /* RAM overlay if rw_state == 1 */
        return r->rw_state;
    }
    return false;
}
