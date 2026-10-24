#include "sim.h"
#include <stdio.h>
#include "via.h"
#include "acia.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * Ring buffer helpers
 * ------------------------------------------------------------------------- */
static void rb_init(sim_ringbuf_t *rb) {
    rb->head = rb->tail = 0;
    pthread_mutex_init(&rb->lock, NULL);
}
static void rb_free(sim_ringbuf_t *rb) {
    pthread_mutex_destroy(&rb->lock);
}
static bool rb_push(sim_ringbuf_t *rb, char c) {
    pthread_mutex_lock(&rb->lock);
    int next = (rb->head + 1) % SIM_TERM_BUF;
    bool ok = (next != rb->tail);
    if (ok) { rb->buf[rb->head] = c; rb->head = next; }
    pthread_mutex_unlock(&rb->lock);
    return ok;
}
static int rb_pop(sim_ringbuf_t *rb, char *c) {
    pthread_mutex_lock(&rb->lock);
    int ok = (rb->head != rb->tail);
    if (ok) { *c = rb->buf[rb->tail]; rb->tail = (rb->tail+1)%SIM_TERM_BUF; }
    pthread_mutex_unlock(&rb->lock);
    return ok;
}
static bool rb_empty(sim_ringbuf_t *rb) {
    pthread_mutex_lock(&rb->lock);
    bool e = (rb->head == rb->tail);
    pthread_mutex_unlock(&rb->lock);
    return e;
}

/* ---------------------------------------------------------------------------
 * Simple I/O dispatch
 * ------------------------------------------------------------------------- */
static uint8_t simio_read(uint32_t addr, void *ud) {
    sim_t *s = (sim_t *)ud;
    if (s->simio.rx_enable && addr == s->simio.rx_addr) {
        /* Block until a key is available */
        char c = 0;
        while (!rb_pop(&s->term_in, &c)) {
            if (!s->running) return 0;
            struct timespec ts = {0, 1000000}; /* 1ms */
            nanosleep(&ts, NULL);
        }
        return (uint8_t)c;
    }
    return 0xFF;
}
static void simio_write(uint32_t addr, uint8_t val, void *ud) {
    sim_t *s = (sim_t *)ud;
    if (s->simio.tx_enable && addr == s->simio.tx_addr) {
        rb_push(&s->term_out, (char)val);
    }
}

/* ---------------------------------------------------------------------------
 * Emulator thread
 * ------------------------------------------------------------------------- */
static void nanosleep_ns(long ns) {
    struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
    nanosleep(&ts, NULL);
}

static void *sim_thread(void *ud) {
    sim_t *s = (sim_t *)ud;

    /* Cycle budget per real second for speed control */
    /* Cycles per second for each speed setting.
     * 0 = run as fast as possible (MAX). */
    const long cycles_per_sec[] = {
        2000000L,   /* 2MHz   */
        1000000L,   /* 1MHz   */
        512000L,    /* 512KHz */
        128000L,    /* 128KHz */
        32000L,     /* 32KHz  */
        8000L,      /* 8KHz   */
        2000L,      /* 2KHz   */
        512L,       /* 512Hz  */
        128L,       /* 128Hz  */
        32L,        /* 32Hz   */
        8L,         /* 8Hz    */
        2L,         /* 2Hz    */
        1L,         /* 1Hz    */
        0L,         /* MAX    */
    };
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long   budget_cycles = 0;
    long   real_cycles   = 0;

    while (s->running) {
        pthread_mutex_lock(&s->state_lock);

        /* Wait if paused and no step request */
        while (s->paused && !s->do_step && !s->do_reset && s->running) {
            pthread_cond_wait(&s->step_cond, &s->state_lock);
        }
        /* Re-snap t0 to now after any pause so accumulated wall-clock time
         * while paused doesn't count as "ahead of schedule".  Without this,
         * t0 can end up in the future (because the slow-speed path advances
         * it by target_ns even when the sleep was cut short), causing
         * behind_ns to blow up on the next instruction and freezing the
         * thread for many seconds at slow speeds. */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        real_cycles = 0;

        if (!s->running) { pthread_mutex_unlock(&s->state_lock); break; }

        /* Reset */
        if (s->do_reset) {
            s->do_reset = false;
            cpu_reset(&s->cpu, mem_read, &s->mem);
            s->cpu.stopped = false;  /* allow running again after reset */
            pthread_mutex_unlock(&s->state_lock);
            continue;
        }

        /* Check breakpoints */
        uint32_t pc = s->cpu.PC & 0xFFFF;
        for (int i = 0; i < s->n_breakpoints; i++) {
            if (s->breakpoints[i] == pc) {
                s->hit_breakpoint = true;
                s->paused = true;
                s->do_step = false;
                pthread_mutex_unlock(&s->state_lock);
                goto next_iter;
            }
        }

        /* STP halt — CPU stopped by STP instruction.
         * On first hit: pause. On subsequent iterations (do_step or run):
         * clear stopped so execution can continue. */
        if (s->cpu.stopped) {
            if (!s->do_step) {
                /* First time — pause here */
                s->paused = true;
                s->hit_breakpoint = true;
                pthread_mutex_unlock(&s->state_lock);
                goto next_iter;
            }
            /* User pressed Step or Run — clear stopped and continue */
            s->cpu.stopped = false;
        }

        /* BRK halt mode */
        uint8_t opcode = mem_read(s->cpu.PC & 0xFFFF, &s->mem);
        if (opcode == 0x00 && s->on_brk == 1) {
            s->paused = true;
            s->hit_breakpoint = true;
            pthread_mutex_unlock(&s->state_lock);
            goto next_iter;
        }

        /* RTS behaviour */
        if (opcode == 0x60) {
            if (s->on_rts == 1) {
                /* Halt mode — only pause on top-level RTS (stack empty).
                 * SP=$FF means the stack is empty; the next RTS would
                 * underflow, which is the "program finished" condition.
                 * Halting on every RTS would break any use of subroutines. */
                if ((s->cpu.SP & 0xFF) == 0xFF) {
                    s->paused = true;
                    s->hit_breakpoint = true;
                    pthread_mutex_unlock(&s->state_lock);
                    goto next_iter;
                }
            } else if (s->cpu.type != CPU_65816) {
                /* Restart mode (6502/65C02) — if stack is empty (SP=$FF),
                 * re-push sentinel so RTS returns to the reset vector */
                if ((s->cpu.SP & 0xFF) == 0xFF) {
                    uint16_t rv = (uint16_t)(mem_read(0xFFFC, &s->mem)
                                | (mem_read(0xFFFD, &s->mem) << 8));
                    uint16_t sentinel = rv - 1;
                    mem_write(0x0100 | (s->cpu.SP & 0xFF),
                              (sentinel >> 8) & 0xFF, &s->mem);
                    s->cpu.SP = (s->cpu.SP - 1) & 0xFF;
                    mem_write(0x0100 | (s->cpu.SP & 0xFF),
                              sentinel & 0xFF, &s->mem);
                    s->cpu.SP = (s->cpu.SP - 1) & 0xFF;
                }
            } else {
                /* 65816 restart mode — if SP is at reset value ($01FF),
                 * directly jump to reset vector instead of RTS */
                if (s->cpu.SP == 0x01FF) {
                    uint16_t rv = (uint16_t)(mem_read(0xFFFC, &s->mem)
                                | (mem_read(0xFFFD, &s->mem) << 8));
                    s->cpu.PC = rv;
                    pthread_mutex_unlock(&s->state_lock);
                    goto next_iter;
                }
            }
        }

        /* Execute one instruction */
        int cyc = cpu_step(&s->cpu, mem_read, mem_write, &s->mem);

        /* Tick peripherals once per CPU cycle */
        for (int _c = 0; _c < cyc; _c++) {
            if (s->via_enable[0]) via_tick(&s->via[0]);
            if (s->via_enable[1]) via_tick(&s->via[1]);
            if (s->acia_enable)   acia_tick(&s->acia);
            if (s->tick_cb)       s->tick_cb(s->tick_ud);
        }
        real_cycles += cyc;

        if (s->do_step) {
            s->do_step = false;
            s->paused = true;
        }
        pthread_mutex_unlock(&s->state_lock);

        /* Speed throttle */
        if (s->speed != SIM_SPEED_MAX && cycles_per_sec[s->speed] > 0) {
            long cps = cycles_per_sec[s->speed];
            /* Check interval: at high speeds check every 10000 cycles,
             * at slow speeds check every instruction */
            long check_interval = (cps >= 10000L) ? 10000L : 1L;
            if (real_cycles >= check_interval) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ns = (now.tv_sec - t0.tv_sec) * 1000000000L
                                + (now.tv_nsec - t0.tv_nsec);
                long target_ns  = (real_cycles * 1000000000L) / cps;
                long behind_ns  = target_ns - elapsed_ns;
                if (behind_ns > 100000L) { /* more than 0.1ms ahead */
                    /* Sleep in short chunks so we respond to pause quickly */
                    long sleep_ns = behind_ns - 100000L;
                    long chunk    = 5000000L; /* 5ms chunks */
                    while (sleep_ns > 0 && !s->paused && s->running) {
                        long nap = sleep_ns < chunk ? sleep_ns : chunk;
                        nanosleep_ns(nap);
                        sleep_ns -= nap;
                    }
                }
                /* Reset counters periodically.
                 * At slow speeds: advance t0 by target_ns so we track
                 * wall-clock precisely without drift accumulation.
                 * BUT only if we weren't paused mid-sleep — if we were,
                 * advancing t0 by the full target puts it in the future,
                 * causing the next wake to sleep for far too long. */
                if (cps < 10000L) {
                    if (s->paused) {
                        /* Sleep was cut short — re-snap to now.
                         * The cond_wait at the top of the loop will also
                         * do this, but guard here too for clarity. */
                        clock_gettime(CLOCK_MONOTONIC, &t0);
                    } else {
                        /* Sleep completed — advance baseline precisely */
                        long adv = target_ns;
                        t0.tv_nsec += (int)(adv % 1000000000L);
                        t0.tv_sec  += (int)(adv / 1000000000L);
                        if (t0.tv_nsec >= 1000000000L) {
                            t0.tv_nsec -= 1000000000L;
                            t0.tv_sec++;
                        }
                    }
                    budget_cycles = real_cycles = 0;
                } else if (elapsed_ns > 1000000000L) {
                    clock_gettime(CLOCK_MONOTONIC, &t0);
                    budget_cycles = real_cycles = 0;
                }
            }
        }
        (void)budget_cycles;
        continue;

    next_iter:;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
bool sim_init(sim_t *s, cpu_type_t cpu_type, size_t mem_size) {
    memset(s, 0, sizeof(*s));
    cpu_init(&s->cpu, cpu_type);
    if (!mem_init(&s->mem, mem_size)) return false;
    rb_init(&s->term_out);
    rb_init(&s->term_in);
    pthread_mutex_init(&s->state_lock, NULL);
    pthread_cond_init(&s->step_cond, NULL);
    s->speed = SIM_SPEED_1MHZ;
    s->paused = true;

    /* Default simple I/O */
    s->simio.tx_enable = true;  s->simio.tx_addr = 0xF000;
    s->simio.rx_enable = true;  s->simio.rx_addr = 0xF001;

    return true;
}

void sim_free(sim_t *s) {
    sim_stop(s);
    mem_free(&s->mem);
    free(s->rom_image);
    rb_free(&s->term_out);
    rb_free(&s->term_in);
    pthread_mutex_destroy(&s->state_lock);
    pthread_cond_destroy(&s->step_cond);
}

bool sim_start(sim_t *s) {
    s->running = true;
    return pthread_create(&s->thread, NULL, sim_thread, s) == 0;
}

void sim_stop(sim_t *s) {
    /* Signal running=false so simio_rd blocking loop exits */
    s->running = false;
    pthread_mutex_lock(&s->state_lock);
    s->paused = false;
    pthread_cond_broadcast(&s->step_cond);
    pthread_mutex_unlock(&s->state_lock);
    /* Give simio_rd time to notice running=false and exit */
    if (s->thread) {
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, NULL);
        pthread_join(s->thread, NULL);
        s->thread = 0;
    }
}

void sim_pause(sim_t *s) {
    pthread_mutex_lock(&s->state_lock);
    s->paused = true;
    pthread_mutex_unlock(&s->state_lock);
}

void sim_resume(sim_t *s) {
    pthread_mutex_lock(&s->state_lock);
    s->paused = false;
    s->hit_breakpoint = false;
    s->cpu.stopped = false;  /* clear STP halt so Run works after STP */
    pthread_cond_signal(&s->step_cond);
    pthread_mutex_unlock(&s->state_lock);
}

void sim_step(sim_t *s) {
    pthread_mutex_lock(&s->state_lock);
    s->do_step = true;
    s->paused  = false;
    pthread_cond_signal(&s->step_cond);
    pthread_mutex_unlock(&s->state_lock);
}

void sim_reset(sim_t *s) {
    pthread_mutex_lock(&s->state_lock);
    s->do_reset = true;
    s->paused   = false;
    pthread_cond_signal(&s->step_cond);
    pthread_mutex_unlock(&s->state_lock);
}

void sim_add_breakpoint(sim_t *s, uint32_t addr) {
    pthread_mutex_lock(&s->state_lock);
    if (s->n_breakpoints < SIM_MAX_BREAKPOINTS)
        s->breakpoints[s->n_breakpoints++] = addr;
    pthread_mutex_unlock(&s->state_lock);
}

void sim_clear_breakpoints(sim_t *s) {
    pthread_mutex_lock(&s->state_lock);
    s->n_breakpoints = 0;
    s->hit_breakpoint = false;
    pthread_mutex_unlock(&s->state_lock);
}

int sim_term_read(sim_t *s, char *buf, int max) {
    int n = 0;
    char c;
    while (n < max && rb_pop(&s->term_out, &c)) buf[n++] = c;
    return n;
}

void sim_term_write(sim_t *s, char ch) {
    rb_push(&s->term_in, ch);
    /* Wake thread if it's blocking on rx */
}

void sim_term_out_push(sim_t *s, char ch) {
    rb_push(&s->term_out, ch);
}

int sim_term_in_pop(sim_t *s, char *ch) {
    return rb_pop(&s->term_in, ch);
}

void sim_get_cpu(sim_t *s, cpu_state_t *out) {
    pthread_mutex_lock(&s->state_lock);
    *out = s->cpu;
    pthread_mutex_unlock(&s->state_lock);
}
