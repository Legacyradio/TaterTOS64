/*
 * TaterTOS64v3 — <termios.h>
 *
 * POSIX terminal control. Backed by tcgetattr/tcsetattr/cfsetispeed/
 * cfsetospeed in src/user/libc/posix.c. TaterTOS's terminal model is
 * simple — the ps2_kbd line discipline gives basic canonical/raw
 * modes; most flags are accepted but ignored.
 *
 * Origin log: logs/fry838.txt
 */

#ifndef _TATERTOS_TOPLEVEL_TERMIOS_H
#define _TATERTOS_TOPLEVEL_TERMIOS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

#define NCCS 32

/* c_iflag */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000

/* c_oflag */
#define OPOST   0x0001
#define ONLCR   0x0004

/* c_cflag */
#define CSIZE   0x0030
#define CS5     0x0000
#define CS6     0x0010
#define CS7     0x0020
#define CS8     0x0030
#define CSTOPB  0x0040
#define CREAD   0x0080
#define PARENB  0x0100
#define PARODD  0x0200
#define HUPCL   0x0400
#define CLOCAL  0x0800

/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define IEXTEN  0x8000

/* c_cc indexes */
#define VINTR     0
#define VQUIT     1
#define VERASE    2
#define VKILL     3
#define VEOF      4
#define VTIME     5
#define VMIN      6

/* tcsetattr action */
#define TCSANOW    0
#define TCSADRAIN  1
#define TCSAFLUSH  2

/* tcflush queue selector */
#define TCIFLUSH   0
#define TCOFLUSH   1
#define TCIOFLUSH  2

/* baud rate constants */
#define B0       0
#define B50      1
#define B75      2
#define B110     3
#define B134     4
#define B150     5
#define B200     6
#define B300     7
#define B600     8
#define B1200    9
#define B1800   10
#define B2400   11
#define B4800   12
#define B9600   13
#define B19200  14
#define B38400  15

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);
int cfsetispeed(struct termios *t, speed_t s);
int cfsetospeed(struct termios *t, speed_t s);
speed_t cfgetispeed(const struct termios *t);
speed_t cfgetospeed(const struct termios *t);
int tcflush(int fd, int queue);
int tcdrain(int fd);
int tcsendbreak(int fd, int duration);
int tcflow(int fd, int action);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _TATERTOS_TOPLEVEL_TERMIOS_H */
