/* SPDX-License-Identifier: GPL-3.0-only */
/* Arbitrary UART rates (3..6 Mbaud on a CH343) need termios2 with BOTHER,
 * which cannot share a translation unit with <termios.h>. */
#ifdef __linux__
#include <asm/termbits.h>
#include <sys/ioctl.h>
int lrh_linux_baud(int fd, unsigned baud);
int lrh_linux_baud(int fd, unsigned baud)
{
    struct termios2 t;
    if(ioctl(fd,TCGETS2,&t))return -1;
    t.c_cflag&=~(tcflag_t)CBAUD; t.c_cflag|=BOTHER;
    t.c_ispeed=baud; t.c_ospeed=baud;
    return ioctl(fd,TCSETS2,&t);
}
#else
typedef int lrh_linux_baud_unused;
#endif
