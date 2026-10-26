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
#  include <termios.h>
#  include <sys/ioctl.h>
#  include <poll.h>
#  define close_socket(s) close(s)
#  define INVALID_SOCKET  (-1)
#  define SOCKET_ERROR    (-1)
   typedef int SOCKET;
#endif

#ifdef _WIN32
#  include <windows.h>
typedef struct { DCB dcb; COMMTIMEOUTS timeouts; } acia_win_saved_t;
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
    a->baud = (a->model == ACIA_MODEL_W65C51N && baud_sel == 0)
        ? 115200 : BAUD_TABLE[baud_sel];

    int wl = (a->CTRL >> 5) & 0x03;
    a->data_bits = 8 - wl;  /* 00=8, 01=7, 10=6, 11=5 */

    /* Command bit 5 enables parity; bits 6..7 select O/E/M/S. */
    static const char parity_modes[] = {'O', 'E', 'M', 'S'};
    a->parity = (a->model == ACIA_MODEL_W65C51N || !(a->CMD & 0x20))
        ? 'N' : parity_modes[(a->CMD >> 6) & 3];
    a->stop_half_bits = 2;
    if (a->CTRL & 0x80) {
        if (a->data_bits == 5 && a->parity == 'N') a->stop_half_bits = 3;
        else if (a->data_bits == 8 && a->parity != 'N') a->stop_half_bits = 2;
        else a->stop_half_bits = 4;
    }
    a->stop_bits = (a->stop_half_bits + 1) / 2; /* Legacy integer field. */

    /* Cycles per bit at current baud */
    if (a->baud > 0 && a->cpu_hz > 0)
        a->cycles_per_bit = a->cpu_hz / a->baud;
    else
        a->cycles_per_bit = 0;
}

static void update_irq(acia_t *a) {
    bool rx_irq = (a->CMD & 0x01) && !(a->CMD & 0x02) && (a->STATUS & ACIA_ST_RDRF);
    bool tx_irq = a->model != ACIA_MODEL_W65C51N && (a->CMD & 0x01) &&
        ((a->CMD >> 2) & 0x03) == 1 && (a->STATUS & ACIA_ST_TDRE);
    bool asserted = rx_irq || tx_irq;
    if (asserted) a->STATUS |= ACIA_ST_IRQ;
    else          a->STATUS &= ~ACIA_ST_IRQ;
    if (asserted != a->_irq_state) {
        a->_irq_state = asserted;
        if (a->irq_fn) a->irq_fn(asserted, a->irq_ud);
    }
}

/* Native serial backend. serial_fd stores a POSIX fd or a Windows HANDLE.
 * Existing TCP interfaces remain source-compatible with older callers. */
static bool native_serial_configure(acia_t *a) {
#ifdef _WIN32
    DCB dcb; memset(&dcb,0,sizeof(dcb));dcb.DCBlength=sizeof(dcb);
    HANDLE h=(HANDLE)a->serial_fd;
    if(a->baud<=0 || !GetCommState(h,&dcb)) return false;
    dcb.BaudRate=(DWORD)a->baud;dcb.ByteSize=(BYTE)a->data_bits;
    dcb.Parity=a->parity=='N'?NOPARITY:a->parity=='O'?ODDPARITY:
        a->parity=='E'?EVENPARITY:a->parity=='M'?MARKPARITY:SPACEPARITY;
    dcb.StopBits=a->stop_half_bits==3?ONE5STOPBITS:
        a->stop_half_bits==4?TWOSTOPBITS:ONESTOPBIT;
    dcb.fBinary=TRUE;dcb.fParity=a->parity!='N';
    dcb.fOutxCtsFlow=FALSE;dcb.fOutxDsrFlow=FALSE;
    dcb.fDtrControl=(a->CMD&1)?DTR_CONTROL_ENABLE:DTR_CONTROL_DISABLE;
    dcb.fRtsControl=RTS_CONTROL_ENABLE;dcb.fDsrSensitivity=FALSE;
    dcb.fTXContinueOnXoff=TRUE;dcb.fOutX=FALSE;dcb.fInX=FALSE;
    dcb.fErrorChar=FALSE;dcb.fNull=FALSE;dcb.fAbortOnError=FALSE;
    return SetCommState(h,&dcb)!=0;
#else
    speed_t speed;
    switch(a->baud) {
        case 50:speed=B50;break;case 75:speed=B75;break;
        case 110:speed=B110;break;case 150:speed=B150;break;
        case 300:speed=B300;break;case 600:speed=B600;break;
        case 1200:speed=B1200;break;case 1800:speed=B1800;break;
        case 2400:speed=B2400;break;case 4800:speed=B4800;break;
        case 9600:speed=B9600;break;case 19200:speed=B19200;break;
        case 115200:speed=B115200;break;
        default:errno=EINVAL;return false;
    }
    struct termios t;
    if(tcgetattr((int)a->serial_fd,&t)!=0) return false;
    cfmakeraw(&t);
    t.c_iflag &= ~(IXON|IXOFF|IXANY);
    t.c_cflag&=~(CSIZE|PARENB|PARODD|CSTOPB);
#ifdef CMSPAR
    t.c_cflag&=~CMSPAR;
#endif
    t.c_cflag|=CLOCAL|CREAD;
    switch(a->data_bits) {case 5:t.c_cflag|=CS5;break;case 6:t.c_cflag|=CS6;break;
        case 7:t.c_cflag|=CS7;break;default:t.c_cflag|=CS8;break;}
    if(a->parity!='N') t.c_cflag|=PARENB;
    if(a->parity=='O'||a->parity=='M') t.c_cflag|=PARODD;
    if(a->parity=='M'||a->parity=='S') {
#ifdef CMSPAR
        t.c_cflag|=CMSPAR;
#else
        errno=EINVAL;return false;
#endif
    }
    if(a->stop_half_bits>2) t.c_cflag|=CSTOPB;
#ifdef CRTSCTS
    t.c_cflag&=~CRTSCTS;
#endif
    t.c_cc[VMIN]=0;t.c_cc[VTIME]=0;
    if(cfsetispeed(&t,speed)!=0 || cfsetospeed(&t,speed)!=0 ||
       tcsetattr((int)a->serial_fd,TCSANOW,&t)!=0) return false;
#ifdef TIOCMBIS
    int bit=TIOCM_DTR;
    /* PTYs need not implement modem outputs. These are best-effort signals. */
    ioctl((int)a->serial_fd,(a->CMD&1)?TIOCMBIS:TIOCMBIC,&bit);
    bit=TIOCM_RTS;ioctl((int)a->serial_fd,TIOCMBIS,&bit);
#endif
    return true;
#endif
}

static void native_serial_close(acia_t *a) {
    if(a->serial_fd==(intptr_t)-1) return;
#ifdef _WIN32
    PurgeComm((HANDLE)a->serial_fd, PURGE_TXABORT|PURGE_RXABORT|PURGE_TXCLEAR|PURGE_RXCLEAR);
    if(a->serial_saved) {
        acia_win_saved_t *old=a->serial_saved;
        SetCommState((HANDLE)a->serial_fd,&old->dcb);
        SetCommTimeouts((HANDLE)a->serial_fd,&old->timeouts);
    }
    CloseHandle((HANDLE)a->serial_fd);
#else
    tcflush((int)a->serial_fd,TCIOFLUSH);
    if(a->serial_saved) tcsetattr((int)a->serial_fd,TCSANOW,a->serial_saved);
#ifdef TIOCNXCL
    ioctl((int)a->serial_fd,TIOCNXCL);
#endif
    close((int)a->serial_fd);
#endif
    free(a->serial_saved);a->serial_saved=NULL;
    a->serial_fd=(intptr_t)-1;
}

void acia_disconnect(acia_t *a) {
    if(a->client_fd!=INVALID_SOCKET) {
        close_socket((SOCKET)a->client_fd);a->client_fd=INVALID_SOCKET;
    }
    native_serial_close(a);
    a->TDR_full=false;
    a->STATUS&=~ACIA_ST_RDRF;
    a->STATUS|=ACIA_ST_TDRE|ACIA_ST_DCD|ACIA_ST_DSR;
    update_irq(a);
}

static void native_serial_failed(acia_t *a,const char *operation) {
#ifdef _WIN32
    snprintf(a->transport_error,sizeof(a->transport_error),"%s (Windows error %lu)",operation,(unsigned long)GetLastError());
#else
    snprintf(a->transport_error,sizeof(a->transport_error),"%s: %s",operation,strerror(errno));
#endif
    acia_disconnect(a);
}

bool acia_serial_open(acia_t *a,const char *path) {
    /* Keep a copy: callers may pass a->serial_path. */
    char copy[sizeof(a->serial_path)];
    if(!path || !*path || strlen(path)>=sizeof(copy)) {
        snprintf(a->transport_error,sizeof(a->transport_error),"Enter a valid serial device path");return false;
    }
    snprintf(copy,sizeof(copy),"%s",path);
    acia_tcp_close(a);a->transport=ACIA_TRANSPORT_SERIAL;
    snprintf(a->serial_path,sizeof(a->serial_path),"%s",copy);
    a->transport_error[0]=0;
#ifdef _WIN32
    char device[sizeof(copy)+8];
    if(!strncmp(copy,"\\\\.\\",4)) snprintf(device,sizeof(device),"%s",copy);
    else snprintf(device,sizeof(device),"\\\\.\\%s",copy);
    HANDLE h=CreateFileA(device,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE) {native_serial_failed(a,"Cannot open serial port");return false;}
    a->serial_fd=(intptr_t)h;
    acia_win_saved_t *old=calloc(1,sizeof(*old));
    if(!old) {acia_disconnect(a);return false;}
    old->dcb.DCBlength=sizeof(old->dcb);
    if(!GetCommState(h,&old->dcb)||!GetCommTimeouts(h,&old->timeouts)) {
        free(old);native_serial_failed(a,"Cannot read serial configuration");return false;
    }
    a->serial_saved=old;
    COMMTIMEOUTS timeouts={MAXDWORD,0,0,0,1};
    if(!SetCommTimeouts(h,&timeouts)) {native_serial_failed(a,"Cannot set serial timeouts");return false;}
#else
    int fd=open(copy,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0) {native_serial_failed(a,"Cannot open serial device");return false;}
    a->serial_fd=fd;
    struct termios *old=malloc(sizeof(*old));
    if(!old) {acia_disconnect(a);return false;}
    if(tcgetattr(fd,old)!=0) {free(old);native_serial_failed(a,"Not a serial/TTY device");return false;}
    a->serial_saved=old;
    /* Advisory exclusivity where supported; no flow control is enabled. */
#ifdef TIOCEXCL
    if(ioctl(fd,TIOCEXCL)!=0 && errno!=ENOTTY) {
        native_serial_failed(a,"Serial device is busy");return false;
    }
#endif
#endif
    if(!native_serial_configure(a)) {native_serial_failed(a,"Unsupported serial settings or configuration failure");return false;}
    a->STATUS&=~(ACIA_ST_DCD|ACIA_ST_DSR);update_irq(a);
    return true;
}

/* Result: 1 byte transferred, 0 would block/no data, -1 fatal/disconnected. */
static int transport_write(acia_t *a,uint8_t byte) {
    if(a->transport==ACIA_TRANSPORT_SERIAL) {
#ifdef _WIN32
        DWORD n=0;
        BOOL ok=WriteFile((HANDLE)a->serial_fd,&byte,1,&n,NULL);
        if(n==1) return 1;
        if(!ok) {
            DWORD error=GetLastError();
            if(error==ERROR_SEM_TIMEOUT || error==ERROR_TIMEOUT) return 0;
            return -1;
        }
        return 0;
#else
        ssize_t n=write((int)a->serial_fd,&byte,1);
        if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) return 0;
        return n<0?-1:(int)n;
#endif
    }
#ifdef _WIN32
    int n=send((SOCKET)a->client_fd,(const char*)&byte,1,0);
    if(n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) return 0;
#else
    ssize_t n=send((int)a->client_fd,&byte,1,MSG_NOSIGNAL);
    if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) return 0;
#endif
    return n<=0?-1:1;
}
static int transport_read(acia_t *a,uint8_t *byte) {
    if(a->transport==ACIA_TRANSPORT_SERIAL) {
#ifdef _WIN32
        DWORD n=0;
        if(!ReadFile((HANDLE)a->serial_fd,byte,1,&n,NULL)) return -1;
        return (int)n;
#else
        struct pollfd p={(int)a->serial_fd,POLLIN,0};
        int ready=poll(&p,1,0);
        if(ready<0) return errno==EINTR?0:-1;
        if(p.revents&(POLLERR|POLLHUP|POLLNVAL)) {errno=EIO;return -1;}
        if(!(p.revents&POLLIN)) return 0;
        ssize_t n=read((int)a->serial_fd,byte,1);
        if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) return 0;
        return n<0?-1:(int)n;
#endif
    }
#ifdef _WIN32
    int n=recv((SOCKET)a->client_fd,(char*)byte,1,0);
    if(n==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK) return 0;
#else
    ssize_t n=recv((int)a->client_fd,byte,1,MSG_DONTWAIT);
    if(n<0 && (errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) return 0;
#endif
    return n<=0?-1:1;
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void acia_init(acia_t *a, int tcp_port, int cpu_hz,
               acia_irq_fn irq_fn, void *ud) {
    memset(a, 0, sizeof(*a));
    a->serial_fd = (intptr_t)-1;
    a->transport = ACIA_TRANSPORT_TCP;
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
    acia_tcp_close(a);
    a->transport = ACIA_TRANSPORT_TCP;
    a->transport_error[0]=0;
    if (a->tcp_port <= 0 || a->tcp_port > 65535) return false;

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
    acia_disconnect(a);
    if(a->server_fd!=INVALID_SOCKET) {
        close_socket((SOCKET)a->server_fd);a->server_fd=INVALID_SOCKET;
    }
}

void acia_reset(acia_t *a) {
    a->STATUS    = ACIA_ST_TDRE;
    if (!acia_connected(a))
        a->STATUS |= ACIA_ST_DCD | ACIA_ST_DSR;
    a->CMD       = 0x00;
    a->TDR_full  = false;
    a->tx_overwrites = 0;
    /* Preserve previous IRQ state until update_irq can notify deassertion. */
    decode_ctrl(a);
    if(a->transport==ACIA_TRANSPORT_SERIAL && acia_connected(a) && !native_serial_configure(a))
        native_serial_failed(a,"Cannot reset serial configuration");
    update_irq(a);
}

void acia_tick(acia_t *a) {
    a->cycle++;
    if(a->TDR_full && a->cycles_per_bit>0 &&
       (int64_t)(a->cycle-a->tx_ready_cycle)>=0) {
        int result=acia_connected(a)?transport_write(a,a->TDR):1;
        if(result<0) {
            native_serial_failed(a,"Serial/TCP transmit failed");
        } else if(result==1) {
            a->TDR_full=false;a->STATUS|=ACIA_ST_TDRE;update_irq(a);
        } else a->tx_ready_cycle=a->cycle+1024;
    }
    if((a->cycle&0x3ff)!=0) return;
    if(a->transport==ACIA_TRANSPORT_TCP && a->server_fd!=INVALID_SOCKET &&
       a->client_fd==INVALID_SOCKET) {
        struct sockaddr_in peer;socklen_t len=sizeof(peer);
        SOCKET fd=accept((SOCKET)a->server_fd,(struct sockaddr*)&peer,&len);
        if(fd!=INVALID_SOCKET) {
            a->client_fd=fd;set_nonblocking(fd);int on=1;
            setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(const char*)&on,sizeof(on));
            a->STATUS&=~(ACIA_ST_DCD|ACIA_ST_DSR);a->transport_error[0]=0;update_irq(a);
        }
    }
    if(!acia_connected(a)) return;
    /* Detect a TCP disconnect even while the receive register is occupied. */
    if(a->transport==ACIA_TRANSPORT_TCP) {
        char byte;
#ifdef _WIN32
        int n=recv((SOCKET)a->client_fd,&byte,1,MSG_PEEK);
        bool failed=n==SOCKET_ERROR && WSAGetLastError()!=WSAEWOULDBLOCK;
#else
        ssize_t n=recv((int)a->client_fd,&byte,1,MSG_PEEK|MSG_DONTWAIT);
        bool failed=n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR;
#endif
        if(n==0 || failed) {acia_disconnect(a);return;}
    }
    if(!(a->STATUS&ACIA_ST_RDRF)) {
        uint8_t byte;int n=transport_read(a,&byte);
        if(n<0) native_serial_failed(a,"Serial/TCP receive failed");
        else if(n==1) {a->RDR=byte & ((1u << a->data_bits) - 1);a->STATUS|=ACIA_ST_RDRF;update_irq(a);}
    }
}

uint8_t acia_read(acia_t *a, uint8_t reg) {
    switch (reg & 0x03) {
    case ACIA_DATA:
        a->STATUS &= ~ACIA_ST_RDRF;
        update_irq(a);
        return a->RDR;
    case ACIA_STATUS:
        /* Keep internal readiness accurate for timing and IRQ accounting.
         * The faulty chip exposes TDRE=1 even while its transmitter is busy. */
        return a->STATUS | ((a->model == ACIA_MODEL_W65C51N && a->tx_fault_enabled) ? ACIA_ST_TDRE : 0);
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
        if ((a->model == ACIA_MODEL_W65C51N && a->tx_fault_enabled) || (a->STATUS & ACIA_ST_TDRE)) {
            /* Reloading an active shift register disrupts the old frame.
             * At byte transport resolution discard that unfinished byte
             * and start timing the replacement. This is not a wire trace. */
            if (a->TDR_full && a->model == ACIA_MODEL_W65C51N && a->tx_fault_enabled) a->tx_overwrites++;
            a->TDR     = val & ((1u << a->data_bits) - 1);
            a->TDR_full = true;
            a->STATUS &= ~ACIA_ST_TDRE;
            /* Schedule transmission after one frame worth of cycles */
            int frame_half_bits = 2 * (1 + a->data_bits + (a->parity != 'N')) + a->stop_half_bits;
            a->tx_ready_cycle = a->cycle +
                ((uint64_t)frame_half_bits * a->cycles_per_bit + 1) / 2;
            update_irq(a);
        }
        break;
    case ACIA_STATUS: /* soft reset */
        acia_reset(a);
        break;
    case ACIA_CMD:
        a->CMD = val;
        decode_ctrl(a);
        if(a->transport==ACIA_TRANSPORT_SERIAL && acia_connected(a) && !native_serial_configure(a))
            native_serial_failed(a,"Cannot apply ACIA command to serial port");
        update_irq(a);
        break;
    case ACIA_CTRL:
        a->CTRL = val;
        decode_ctrl(a);
        if(a->transport==ACIA_TRANSPORT_SERIAL && acia_connected(a) && !native_serial_configure(a))
            native_serial_failed(a,"Unsupported serial baud/framing");
        break;
    }
}

bool acia_connected(acia_t *a) {
    return a->transport==ACIA_TRANSPORT_SERIAL ? a->serial_fd!=(intptr_t)-1 : a->client_fd!=INVALID_SOCKET;
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
    if (a->stop_half_bits == 3)
        snprintf(buf, (size_t)buf_len, "%d %d%c1.5", a->baud, a->data_bits, a->parity);
    else
        snprintf(buf, (size_t)buf_len, "%d %d%c%d",
                 a->baud, a->data_bits, a->parity, a->stop_half_bits / 2);
}

/* Return a description of the frame timing for display */
void acia_format_timing(acia_t *a, char *buf, int buf_len) {
    if (a->baud <= 0) {
        snprintf(buf, (size_t)buf_len, "cpb: ext");
        return;
    }
    /* frame = 1 start + data + stop bits */
    int frame_half_bits = 2 * (1 + a->data_bits + (a->parity != 'N')) + a->stop_half_bits;
    int bps = (a->baud > 0) ? (2 * a->baud) / frame_half_bits : 0;
    snprintf(buf, (size_t)buf_len, "cpb:%d ~%dB/s",
             a->cycles_per_bit, bps);
}

const char *acia_model_name(acia_model_t model) {
    return model == ACIA_MODEL_W65C51N ? "W65C51N" :
        model == ACIA_MODEL_W65C51S ? "W65C51S" : "Generic 6551";
}

void acia_set_model(acia_t *a, acia_model_t model) {
    a->model = (model == ACIA_MODEL_W65C51N || model == ACIA_MODEL_W65C51S)
        ? model : ACIA_MODEL_GENERIC_6551;
    a->tx_fault_enabled = a->model == ACIA_MODEL_W65C51N;
    decode_ctrl(a);
    if(a->transport == ACIA_TRANSPORT_SERIAL && acia_connected(a) && !native_serial_configure(a))
        native_serial_failed(a, "Cannot apply ACIA model framing");
    update_irq(a);
}

void acia_set_w65c51n_tx_fault(acia_t *a, bool enabled) {
    a->tx_fault_enabled = enabled;
}
