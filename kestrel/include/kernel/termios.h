/* =============================================================================
 *  termios.h -- terminal attributes and ioctls, Linux x86_64 ABI
 *
 *  The kernel's struct termios (TCGETS/TCSETS*) and struct winsize, the
 *  flag bits the pseudo-terminal line discipline (fs/pty.c) implements, and
 *  the tty ioctl numbers. musl's tcgetattr()/tcsetattr(), isatty(),
 *  posix_openpt()/ptsname()/unlockpt() and openpty() use exactly these.
 * ============================================================================= */
#ifndef KESTREL_TERMIOS_H
#define KESTREL_TERMIOS_H

#include <stdint.h>

#define KNCCS 19

struct ktermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[KNCCS];
};

struct kwinsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

/* c_iflag */
#define TI_ISTRIP   0000040
#define TI_INLCR    0000100
#define TI_IGNCR    0000200
#define TI_ICRNL    0000400
#define TI_IXON     0002000
#define TI_IUTF8    0040000
/* c_oflag */
#define TO_OPOST    0000001
#define TO_ONLCR    0000004
#define TO_OCRNL    0000010
/* c_cflag */
#define TC_B38400   0000017
#define TC_CS8      0000060
#define TC_CREAD    0000200
#define TC_HUPCL    0002000
/* c_lflag */
#define TL_ISIG     0000001
#define TL_ICANON   0000002
#define TL_ECHO     0000010
#define TL_ECHOE    0000020
#define TL_ECHOK    0000040
#define TL_ECHONL   0000100
#define TL_NOFLSH   0000200
#define TL_ECHOCTL  0001000
#define TL_ECHOKE   0004000
#define TL_IEXTEN   0100000
/* c_cc indices */
#define VINTR       0
#define VQUIT       1
#define VERASE      2
#define VKILL       3
#define VEOF        4
#define VTIME       5
#define VMIN        6
#define VSTART      8
#define VSTOP       9
#define VSUSP       10
#define VEOL        11
#define VREPRINT    12
#define VDISCARD    13
#define VWERASE     14
#define VLNEXT      15
#define VEOL2       16

/* ioctls */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TCSBRK      0x5409
#define TCXONC      0x540A
#define TCFLSH      0x540B
#define TIOCSCTTY   0x540E
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410
#define TIOCOUTQ    0x5411
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define FIONREAD_T  0x541B
#define FIONBIO     0x5421
#define TIOCNOTTY   0x5422
#define TIOCGETD    0x5424
#define TIOCSETD    0x5423
#define TCSBRKP     0x5425
#define TIOCGSID    0x5429
#define TIOCGPTN    0x80045430
#define TIOCSPTLCK  0x40045431

#endif
