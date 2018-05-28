/*
 * udmabuf helper functions.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "ui/console.h"

#ifdef CONFIG_LINUX

#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include "standard-headers/linux/udmabuf.h"

int udmabuf_fd(void)
{
    static bool first = true;
    static int udmabuf;

    if (!first) {
        return udmabuf;
    }
    first = false;

    udmabuf = open("/dev/udmabuf", O_RDWR);
    if (udmabuf < 0) {
        warn_report("open /dev/udmabuf: %s", strerror(errno));
    } else {
        info_report("udmabuf: init ok");
    }
    return udmabuf;
}

#else

int udmabuf_fd(void)
{
    return -1;
}

#endif
