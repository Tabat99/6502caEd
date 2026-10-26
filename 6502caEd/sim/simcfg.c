#include "simcfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void simcfg_defaults(simcfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->cpu_type       = CPU_6502;
    cfg->speed          = SIM_SPEED_1MHZ;
    cfg->mem_size       = 65536;
    cfg->via1_enable    = true;
    cfg->via1_base      = 0x9000;
    cfg->via2_enable    = true;
    cfg->via2_base      = 0x9010;
    cfg->acia_enable    = true;
    cfg->acia_base      = 0x9020;
    cfg->acia_tcp_port  = 6551;
    cfg->acia_transport = 0;
    cfg->acia_w65c51n_tx_fault = true;
    cfg->acia_model = ACIA_MODEL_GENERIC_6551;
#ifdef _WIN32
    snprintf(cfg->acia_serial_device,sizeof(cfg->acia_serial_device),"COM3");
#else
    snprintf(cfg->acia_serial_device,sizeof(cfg->acia_serial_device),"/dev/ttyUSB0");
#endif
    cfg->simio_enable   = true;
    cfg->simio_tx_addr  = 0x9030;
    cfg->simio_rx_addr  = 0x9031;
    cfg->debug_window   = true;
    cfg->on_rts         = 0;   /* push FFFC return address */
    cfg->stp_on_db      = false;
    cfg->on_brk         = 0;   /* normal BRK vector */
}

/* Strip leading/trailing whitespace + CR in-place, return pointer */
static char *strip(char *s) {
    while (*s && (isspace((unsigned char)*s) || *s == '\r')) s++;
    char *end = s + strlen(s);
    while (end > s && (isspace((unsigned char)end[-1]) || end[-1] == '\r'))
        *--end = 0;
    return s;
}

static uint32_t parse_hex(const char *s) {
    return (uint32_t)strtoul(s, NULL, 16);
}
static uint32_t parse_uint(const char *s) {
    return (uint32_t)strtoul(s, NULL, 0);
}
static bool parse_bool(const char *s) {
    return atoi(s) != 0;
}

bool simcfg_load(simcfg_t *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;

    bool explicit_model = false, legacy_present = false, legacy_fault = false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *s = strip(line);
        if (!*s || *s == '#') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = strip(s);
        char *val = strip(eq + 1);

        /* Strip inline comment */
        char *comment = strchr(val, '#');
        if (comment) { *comment = 0; val = strip(val); }

        if      (!strcmp(key,"cpu")) {
            if      (!strcasecmp(val,"65c02"))  cfg->cpu_type = CPU_65C02;
            else if (!strcasecmp(val,"65816"))  cfg->cpu_type = CPU_65816;
            else                                cfg->cpu_type = CPU_6502;
        }
        else if (!strcmp(key,"speed")) {
            /* Try numeric index first (0-12), then named values */
            char *end;
            long idx = strtol(val, &end, 10);
            if (*end == '\0' && idx >= 0 && idx <= 12)
                cfg->speed = (sim_speed_t)idx;
            else if (!strcasecmp(val,"max"))    cfg->speed = SIM_SPEED_MAX;
            else                                cfg->speed = SIM_SPEED_1MHZ;
        }
        else if (!strcmp(key,"mem_size"))       cfg->mem_size      = parse_uint(val);
        else if (!strcmp(key,"bank_enable"))       cfg->bank_enable      = parse_bool(val);
        else if (!strcmp(key,"bank_addr"))         cfg->bank_addr        = parse_hex(val);
        else if (!strcmp(key,"bank_window"))       cfg->bank_window      = parse_hex(val);
        else if (!strcmp(key,"bank_window_size"))  cfg->bank_window_size = parse_hex(val);
        else if (!strcmp(key,"bank_num_banks"))    cfg->bank_num_banks   = (uint8_t)atoi(val);
        else if (!strcmp(key,"via1_enable"))    cfg->via1_enable   = parse_bool(val);
        else if (!strcmp(key,"via1_base"))      cfg->via1_base     = parse_hex(val);
        else if (!strcmp(key,"via2_enable"))    cfg->via2_enable   = parse_bool(val);
        else if (!strcmp(key,"via2_base"))      cfg->via2_base     = parse_hex(val);
        else if (!strcmp(key,"acia_enable"))    cfg->acia_enable   = parse_bool(val);
        else if (!strcmp(key,"acia_base"))      cfg->acia_base     = parse_hex(val);
        else if (!strcmp(key,"acia_tcp_port"))  cfg->acia_tcp_port = atoi(val);
        else if (!strcmp(key,"acia_model")) {
            if (!strcasecmp(val,"w65c51s")) { cfg->acia_model = ACIA_MODEL_W65C51S; explicit_model = true; }
            else if (!strcasecmp(val,"w65c51n")) { cfg->acia_model = ACIA_MODEL_W65C51N; explicit_model = true; }
            else if (!strcasecmp(val,"generic6551") || !strcasecmp(val,"generic")) {
                cfg->acia_w65c51n_tx_fault = true;
    cfg->acia_model = ACIA_MODEL_GENERIC_6551; explicit_model = true;
            }
        }
        else if (!strcmp(key,"acia_w65c51n_tx_fault")) {
            legacy_present = true; legacy_fault = parse_bool(val);
            cfg->acia_w65c51n_tx_fault = legacy_fault;
        }
        else if (!strcmp(key,"acia_transport")) cfg->acia_transport = !strcmp(val,"serial") ? 1 : 0;
        else if (!strcmp(key,"acia_serial_device")) snprintf(cfg->acia_serial_device,sizeof(cfg->acia_serial_device),"%s",val);
        else if (!strcmp(key,"simio_enable"))   cfg->simio_enable  = parse_bool(val);
        else if (!strcmp(key,"simio_tx_addr"))  cfg->simio_tx_addr = parse_hex(val);
        else if (!strcmp(key,"simio_rx_addr"))  cfg->simio_rx_addr = parse_hex(val);
        else if (!strcmp(key,"debug_window"))   cfg->debug_window  = parse_bool(val);
        else if (!strcmp(key,"on_rts"))         cfg->on_rts        = atoi(val);
        else if (!strcmp(key,"on_brk"))         cfg->on_brk        = atoi(val);
        else if (!strcmp(key,"stp_on_db"))      cfg->stp_on_db     = parse_bool(val);
        else if (!strcmp(key,"last_profile"))   snprintf(cfg->last_profile,
                                                     sizeof(cfg->last_profile),
                                                     "%s", val);
        else {
            /* ROM slots: rom0_file, rom0_addr, rom0_bank, rom0_enable, etc. */
            for (int i = 0; i < SIMCFG_MAX_ROMS; i++) {
                char prefix[8];
                snprintf(prefix, sizeof(prefix), "rom%d_", i);
                if (strncmp(key, prefix, strlen(prefix)) != 0) continue;
                const char *sub = key + strlen(prefix);
                if      (!strcmp(sub,"file"))   snprintf(cfg->roms[i].path, 512, "%s", val);
                else if (!strcmp(sub,"addr"))   cfg->roms[i].load_addr = parse_hex(val);
                else if (!strcmp(sub,"bank"))   cfg->roms[i].bank      = parse_hex(val);
                else if (!strcmp(sub,"enable")) cfg->roms[i].enabled   = parse_bool(val);
                break;
            }
        }
    }
    fclose(f);
    if (!explicit_model && legacy_present)
        cfg->acia_model = legacy_fault ? ACIA_MODEL_W65C51N : ACIA_MODEL_GENERIC_6551;
    return true;
}

bool simcfg_save(const simcfg_t *cfg, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return false;

    const char *cpu_str[] = {"6502","65c02","65816"};
    fprintf(f, "# 6502caEd simulator configuration\n\n");
    fprintf(f, "cpu           = %s\n", cpu_str[cfg->cpu_type]);
    fprintf(f, "speed         = %d\n", (int)cfg->speed);
    fprintf(f, "mem_size      = %u\n\n", cfg->mem_size);

    for (int i = 0; i < SIMCFG_MAX_ROMS; i++) {
        if (cfg->roms[i].path[0]) {
            fprintf(f, "rom%d_file     = %s\n", i, cfg->roms[i].path);
            fprintf(f, "rom%d_addr     = %04X\n", i, cfg->roms[i].load_addr);
            fprintf(f, "rom%d_bank     = %02X\n", i, cfg->roms[i].bank);
            fprintf(f, "rom%d_enable   = %d\n\n", i, cfg->roms[i].enabled ? 1 : 0);
        }
    }

    fprintf(f, "bank_enable       = %d\n",   cfg->bank_enable ? 1 : 0);
    fprintf(f, "bank_addr         = %04X\n",  cfg->bank_addr);
    fprintf(f, "bank_window       = %04X\n",  cfg->bank_window);
    fprintf(f, "bank_window_size  = %04X\n",  cfg->bank_window_size);
    fprintf(f, "bank_num_banks    = %d\n\n",  cfg->bank_num_banks);

    fprintf(f, "via1_enable   = %d\n", cfg->via1_enable ? 1 : 0);
    fprintf(f, "via1_base     = %04X\n", cfg->via1_base);
    fprintf(f, "via2_enable   = %d\n", cfg->via2_enable ? 1 : 0);
    fprintf(f, "via2_base     = %04X\n\n", cfg->via2_base);

    fprintf(f, "acia_enable   = %d\n", cfg->acia_enable ? 1 : 0);
    fprintf(f, "acia_base     = %04X\n", cfg->acia_base);
    fprintf(f, "acia_tcp_port = %d\n", cfg->acia_tcp_port);
    fprintf(f, "acia_w65c51n_tx_fault = %d\n", cfg->acia_w65c51n_tx_fault);
    fprintf(f, "acia_model = %s\n", cfg->acia_model == ACIA_MODEL_W65C51N ? "w65c51n" :
        cfg->acia_model == ACIA_MODEL_W65C51S ? "w65c51s" : "generic6551");
    fprintf(f, "acia_transport = %s\n", cfg->acia_transport ? "serial" : "tcp");
    fprintf(f, "acia_serial_device = %s\n\n", cfg->acia_serial_device);

    fprintf(f, "simio_enable  = %d\n", cfg->simio_enable ? 1 : 0);
    fprintf(f, "simio_tx_addr = %04X\n", cfg->simio_tx_addr);
    fprintf(f, "simio_rx_addr = %04X\n\n", cfg->simio_rx_addr);

    fprintf(f, "debug_window  = %d\n", cfg->debug_window ? 1 : 0);
    fprintf(f, "on_rts        = %d\n", cfg->on_rts);  /* 0=push-reset-vec, 1=halt */
    fprintf(f, "on_brk        = %d\n", cfg->on_brk);  /* 0=normal, 1=halt */
    fprintf(f, "stp_on_db     = %d\n", cfg->stp_on_db ? 1 : 0);
    if (cfg->last_profile[0])
        fprintf(f, "last_profile  = %s\n", cfg->last_profile);

    fclose(f);
    return true;
}
