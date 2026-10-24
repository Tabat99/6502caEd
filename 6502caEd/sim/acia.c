#include "acia.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
#  define SOCK_NONBLOCK 0
#  define close_socket(s) closesocket(s)
   typedef int socklen_t;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  define close_socket(s) close(s)
#  define INVALID_SOCKET  (-1)
#  define SOCKET_ERROR    (-1)
   typedef int SOCKET;
#endif

/* ---------------------------------------------------------------------------
 * Baud rate table — CTRL bits 0-3
 * ------------------------------------------------------------------------- */
static const int BAUD_TABLE[16] = {
      0,    50,    75,   110,   135,   150,   300,   600,
   1200,  1800,  2400,  3600,  4800,  7200,  9600, 19200
};

/* ---------------------------------------------------------------------------
 * Socket helpers
 * ------------------------------------------------------------------------- */
static void set_nonblocking(SOCKET fd) {
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}


/* ---------------------------------------------------------------------------
 * Baud / format decode
 * ------------------------------------------------------------------------- */
static void decode_ctrl(acia_t *a) {
    int baud_sel = a->CTRL & 0x0F;
    a->baud = BAUD_TABLE[baud_sel];

    int wl = (a->CTRL >> 5) & 0x03;
    a->data_bits = 8 - wl;  /* 00=8, 01=7, 10=6, 11=5 */

    a->stop_bits = (a->CTRL & 0x80) ? 2 : 1;

    /* Parity from CMD bits 5-7 */
    int par = (a->CMD >> 5) & 0x07;
    if      (par == 0 || par == 4) a->parity = 'N';
    else if (par == 1)             a->parity = 'O';
    else                           a->parity = 'E';

    /* Cycles per bit at current baud */
    if (a->baud > 0 && a->cpu_hz > 0)
        a->cycles_per_bit = a->cpu_hz / a->baud;
    else
        a->cycles_per_bit = 0;
}

static void update_irq(acia_t *a) {
    bool rx_irq = (a->CMD & 0x02) && (a->STATUS & ACIA_ST_RDRF);
    bool tx_irq = ((a->CMD >> 2) & 0x03) == 1 && (a->STATUS & ACIA_ST_TDRE);
    bool asserted = rx_irq || tx_irq;
    if (asserted) a->STATUS |= ACIA_ST_IRQ;
    else          a->STATUS &= ~ACIA_ST_IRQ;
    if (asserted != a->_irq_state) {
        a->_irq_state = asserted;
        if (a->irq_fn) a->irq_fn(asserted, a->irq_ud);
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void acia_init(acia_t *a, int tcp_port, int cpu_hz,
               acia_irq_fn irq_fn, void *ud) {
    memset(a, 0, sizeof(*a));
    a->tcp_port  = tcp_port;
    a->cpu_hz    = cpu_hz;
    a->irq_fn    = irq_fn;
    a->irq_ud    = ud;
    a->server_fd = INVALID_SOCKET;
    a->client_fd = INVALID_SOCKET;

    /* Power-on defaults */
    a->STATUS    = ACIA_ST_TDRE | ACIA_ST_DCD | ACIA_ST_DSR;
    a->CTRL      = 0x1E;  /* 9600 baud, 8 bits, 1 stop */
    a->CMD       = 0x02;  /* DTR active, rx IRQ enabled */
    decode_ctrl(a);

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
#endif
}

bool acia_tcp_open(acia_t *a) {
    if (a->tcp_port <= 0) return false;

    a->server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (a->server_fd == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(a->server_fd, SOL_SOCKET, SO_REUSEADDR,
               (const char*)&opt, sizeof(opt));
    /* Disable Nagle for low-latency */
    setsockopt(a->server_fd, IPPROTO_TCP, TCP_NODELAY,
               (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)a->tcp_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost only */

    if (bind(a->server_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close_socket(a->server_fd);
        a->server_fd = INVALID_SOCKET;
        return false;
    }
    if (listen(a->server_fd, 1) != 0) {
        close_socket(a->server_fd);
        a->server_fd = INVALID_SOCKET;
        return false;
    }
    set_nonblocking(a->server_fd);
    return true;
}

void acia_tcp_close(acia_t *a) {
    if (a->client_fd != INVALID_SOCKET) {
        close_socket(a->client_fd);
        a->client_fd = INVALID_SOCKET;
    }
    if (a->server_fd != INVALID_SOCKET) {
        close_socket(a->server_fd);
        a->server_fd = INVALID_SOCKET;
    }
    /* Mark as disconnected */
    a->STATUS |= ACIA_ST_DCD | ACIA_ST_DSR;
}

void acia_reset(acia_t *a) {
    a->STATUS    = ACIA_ST_TDRE;
    if (a->client_fd == INVALID_SOCKET)
        a->STATUS |= ACIA_ST_DCD | ACIA_ST_DSR;
    a->CMD       = 0x00;
    a->TDR_full  = false;
    a->_irq_state = false;
    decode_ctrl(a);
    update_irq(a);
}

void acia_tick(acia_t *a) {
    a->cycle++;

    /* Only poll TCP every 1000 cycles to avoid syscall overhead */
    if ((a->cycle & 0x3FF) != 0) {
        /* Still do baud timing for transmit */
        if (a->TDR_full && a->cycles_per_bit > 0) {
            if ((int64_t)(a->cycle - a->tx_ready_cycle) >= 0) {
                if (a->client_fd != INVALID_SOCKET) {
                    uint8_t b = a->TDR;
#ifdef _WIN32
                    send(a->client_fd, (const char*)&b, 1, 0);
#else
                    { ssize_t _w = write(a->client_fd, &b, 1); (void)_w; }
#endif
                }
                a->TDR_full = false;
                a->STATUS  |= ACIA_ST_TDRE;
                update_irq(a);
            }
        }
        return;
    }

    /* --- Accept new TCP connection if none active --- */
    if (a->server_fd != INVALID_SOCKET && a->client_fd == INVALID_SOCKET) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        SOCKET fd = accept(a->server_fd, (struct sockaddr*)&caddr, &clen);
        if (fd != INVALID_SOCKET) {
            a->client_fd = fd;
            set_nonblocking(fd);
            int opt = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                       (const char*)&opt, sizeof(opt));
            /* Connected — clear DCD/DSR (active low logic: 0 = connected) */
            a->STATUS &= ~(ACIA_ST_DCD | ACIA_ST_DSR);
        }
    }

    /* --- Check if client disconnected --- */
    if (a->client_fd != INVALID_SOCKET) {
        /* Peek to detect disconnect */
        char buf;
#ifdef _WIN32
        int r = recv(a->client_fd, &buf, 1, MSG_PEEK);
        if (r == 0) {
#else
        ssize_t r = recv(a->client_fd, &buf, 1, MSG_PEEK | MSG_DONTWAIT);
        if (r == 0) {
#endif
            close_socket(a->client_fd);
            a->client_fd = INVALID_SOCKET;
            a->STATUS |= ACIA_ST_DCD | ACIA_ST_DSR;
            a->STATUS &= ~ACIA_ST_RDRF;
            update_irq(a);
        }
    }

    /* --- Transmit: handled every cycle above --- */

    /* --- Receive: poll TCP for incoming byte --- */
    if (!(a->STATUS & ACIA_ST_RDRF) && a->client_fd != INVALID_SOCKET) {
        uint8_t b;
#ifdef _WIN32
        int n = recv(a->client_fd, (char*)&b, 1, 0);
#else
        ssize_t n = recv(a->client_fd, &b, 1, MSG_DONTWAIT);
#endif
        if (n == 1) {
            a->RDR     = b;
            a->STATUS |= ACIA_ST_RDRF;
            update_irq(a);
        }
    }
}

uint8_t acia_read(acia_t *a, uint8_t reg) {
    switch (reg & 0x03) {
    case ACIA_DATA:
        a->STATUS &= ~ACIA_ST_RDRF;
        update_irq(a);
        return a->RDR;
    case ACIA_STATUS:
        return a->STATUS;
    case ACIA_CMD:
        return a->CMD;
    case ACIA_CTRL:
        return a->CTRL;
    }
    return 0xFF;
}

void acia_write(acia_t *a, uint8_t reg, uint8_t val) {
    switch (reg & 0x03) {
    case ACIA_DATA:
        if (a->STATUS & ACIA_ST_TDRE) {
            a->TDR     = val;
            a->TDR_full = true;
            a->STATUS &= ~ACIA_ST_TDRE;
            /* Schedule transmission after one frame worth of cycles */
            int frame_bits = 1 + a->data_bits + a->stop_bits;
            a->tx_ready_cycle = a->cycle + (uint64_t)(frame_bits * a->cycles_per_bit);
            update_irq(a);
        }
        break;
    case ACIA_STATUS: /* soft reset */
        acia_reset(a);
        break;
    case ACIA_CMD:
        a->CMD = val;
        decode_ctrl(a);
        update_irq(a);
        break;
    case ACIA_CTRL:
        a->CTRL = val;
        decode_ctrl(a);
        break;
    }
}

bool acia_connected(acia_t *a) {
    return a->client_fd != INVALID_SOCKET;
}

void acia_set_cpu_hz(acia_t *a, long cpu_hz) {
    a->cpu_hz = cpu_hz;
    /* Recalculate cycles_per_bit from current baud rate */
    if (a->baud > 0 && cpu_hz > 0)
        a->cycles_per_bit = (int)(cpu_hz / a->baud);
    else
        a->cycles_per_bit = 0;
}

void acia_format_settings(acia_t *a, char *buf, int buf_len) {
    /* e.g. "9600 8N1"  or  "2400 7E2" */
    snprintf(buf, (size_t)buf_len, "%d %d%c%d",
             a->baud, a->data_bits, a->parity, a->stop_bits);
}

/* Return a description of the frame timing for display */
void acia_format_timing(acia_t *a, char *buf, int buf_len) {
    if (a->baud <= 0) {
        snprintf(buf, (size_t)buf_len, "cpb: ext");
        return;
    }
    /* frame = 1 start + data + stop bits */
    int frame_bits = 1 + a->data_bits + a->stop_bits;
    /* bytes per second = baud / frame_bits */
    int bps = (a->baud > 0) ? a->baud / frame_bits : 0;
    snprintf(buf, (size_t)buf_len, "cpb:%d ~%dB/s",
             a->cycles_per_bit, bps);
}
