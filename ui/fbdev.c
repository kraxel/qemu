/*
 * linux fbdev output driver.
 *
 * Author: Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/kd.h>
#include <linux/vt.h>
#include <linux/fb.h>

#include "qemu-common.h"
#include "keymaps.h"
#include "trace.h"
#include "ui/qemu-pixman.h"
#include "ui/console.h"
#include "sysemu/sysemu.h"

/*
 * must be last so we get the linux input layer
 * KEY_* defines, not the ncurses ones.
 */
#include <linux/input.h>

/* -------------------------------------------------------------------- */

typedef struct FBDevState {
    /* file handles */
    int                        tty, fb, mice;

    /* saved state, for restore on exit */
    int                        orig_vtno;
    int                        kd_omode;
    struct vt_mode             vt_omode;
    struct fb_var_screeninfo   fb_ovar;

    /* framebuffer */
    char                       *device;
    struct fb_fix_screeninfo   fb_fix;
    struct fb_var_screeninfo   fb_var;
    uint8_t                    *fb_mem;
    int                        fb_mem_offset;

    /* linux console */
    int                        vtno;
    struct vt_mode             vt_mode;
    struct termios             tty_attributes;
    unsigned long              tty_mode;
    unsigned int               tty_flags;
    bool                       tty_mediumraw;
    bool                       key_down[KEY_CNT];

    /* qemu windup */
    DisplayChangeListener      dcl;
    int                        resize_screen;
    int                        redraw_screen;
    int                        cx, cy, cw, ch;
    Notifier                   exit_notifier;
    DisplaySurface             *surface;
    pixman_image_t             *sref, *swork;
    pixman_image_t             *framebuffer;
    pixman_transform_t         transform;
    pixman_region16_t          dirty;
    double                     scale;

    QEMUCursor                 *ptr_cursor;
    pixman_image_t             *ptr_image;
    int                        ptr_refresh;
    int                        px, py, pw, ph;
    int                        mx, my, mon, ax, ay;

    /* options */
    int                        use_scale;
    pixman_filter_t            pfilter;
} FBDevState;

static FBDevState *fb;

/* console switching */
#define SIG_ACQ      (SIGRTMIN+6)
#define SIG_REL      (SIGRTMIN+7)
#define FB_ACTIVE    0
#define FB_REL_REQ   1
#define FB_INACTIVE  2
#define FB_ACQ_REQ   3
int fb_switch_state;

/* fwd decls */
static int fbdev_activate_vt(int tty, int vtno, bool wait);

/* -------------------------------------------------------------------- */
/* pixman helpers                                                       */

static pixman_image_t *pixman_from_framebuffer(FBDevState *s)
{
    pixman_format_code_t format;
    pixman_image_t *image;
    int type;

    type = qemu_pixman_get_type(s->fb_var.red.offset,
                                s->fb_var.green.offset,
                                s->fb_var.blue.offset);
    format = PIXMAN_FORMAT(s->fb_var.bits_per_pixel, type,
                           s->fb_var.transp.length,
                           s->fb_var.red.length,
                           s->fb_var.green.length,
                           s->fb_var.blue.length);
    image = pixman_image_create_bits(format, s->fb_var.xres, s->fb_var.yres,
                                     (void *)s->fb_mem, s->fb_fix.line_length);
    return image;
}

static pixman_image_t *pixman_image_clone(pixman_image_t *i)
{
    return pixman_image_create_bits(pixman_image_get_format(i),
                                    pixman_image_get_width(i),
                                    pixman_image_get_height(i),
                                    pixman_image_get_data(i),
                                    pixman_image_get_stride(i));
}

/* -------------------------------------------------------------------- */
/* mouse                                                                */

static void read_mouse(void *opaque)
{
    FBDevState *s = opaque;
    int8_t buf[3];
    int rc, x, y, b;

    rc = read(s->mice, buf, sizeof(buf));
    if (rc != sizeof(buf)) {
        return;
    }

    if (fb_switch_state != FB_ACTIVE) {
        return;
    }

    x = buf[1];
    y = -buf[2];
    b = buf[0] & 0x7;

    if (kbd_mouse_is_absolute()) {
        s->ax += x; s->ay += y;
        if (s->ax < 0) {
            s->ax = 0;
        }
        if (s->ay < 0) {
            s->ay = 0;
        }
        if (s->ax >= s->cw * s->scale) {
            s->ax = s->cw * s->scale - 1;
        }
        if (s->ay >= s->ch * s->scale) {
            s->ay = s->ch * s->scale-1;
        }
        kbd_mouse_event(s->ax * 0x7FFF / (s->cw * s->scale),
                        s->ay * 0x7FFF / (s->ch * s->scale), 0, b);
    } else {
        kbd_mouse_event(x, y, 0, b);
    }
}

static int init_mouse(FBDevState *s)
{
    s->mice = open("/dev/input/mice", O_RDONLY);
    if (s->mice == -1) {
        return -1;
    }
    qemu_set_fd_handler(s->mice, read_mouse, NULL, s);
    return 0;
}

static void uninit_mouse(FBDevState *s)
{
    if (s->mice == -1) {
        return;
    }
    qemu_set_fd_handler(s->mice, NULL, NULL, NULL);
    close(s->mice);
    s->mice = -1;
}

/* -------------------------------------------------------------------- */
/* keyboard                                                             */

static const char *keynames[] = {
#include "linux-keynames.h"
};

static const int scancode_map[KEY_CNT] = {
    [KEY_ESC]           = 0x01,
    [KEY_1]             = 0x02,
    [KEY_2]             = 0x03,
    [KEY_3]             = 0x04,
    [KEY_4]             = 0x05,
    [KEY_5]             = 0x06,
    [KEY_6]             = 0x07,
    [KEY_7]             = 0x08,
    [KEY_8]             = 0x09,
    [KEY_9]             = 0x0a,
    [KEY_0]             = 0x0b,
    [KEY_MINUS]         = 0x0c,
    [KEY_EQUAL]         = 0x0d,
    [KEY_BACKSPACE]     = 0x0e,

    [KEY_TAB]           = 0x0f,
    [KEY_Q]             = 0x10,
    [KEY_W]             = 0x11,
    [KEY_E]             = 0x12,
    [KEY_R]             = 0x13,
    [KEY_T]             = 0x14,
    [KEY_Y]             = 0x15,
    [KEY_U]             = 0x16,
    [KEY_I]             = 0x17,
    [KEY_O]             = 0x18,
    [KEY_P]             = 0x19,
    [KEY_LEFTBRACE]     = 0x1a,
    [KEY_RIGHTBRACE]    = 0x1b,
    [KEY_ENTER]         = 0x1c,

    [KEY_A]             = 0x1e,
    [KEY_S]             = 0x1f,
    [KEY_D]             = 0x20,
    [KEY_F]             = 0x21,
    [KEY_G]             = 0x22,
    [KEY_H]             = 0x23,
    [KEY_J]             = 0x24,
    [KEY_K]             = 0x25,
    [KEY_L]             = 0x26,
    [KEY_SEMICOLON]     = 0x27,
    [KEY_APOSTROPHE]    = 0x28,
    [KEY_GRAVE]         = 0x29,
    [KEY_LEFTSHIFT]     = 0x2a,
    [KEY_BACKSLASH]     = 0x2b,

    [KEY_Z]             = 0x2c,
    [KEY_X]             = 0x2d,
    [KEY_C]             = 0x2e,
    [KEY_V]             = 0x2f,
    [KEY_B]             = 0x30,
    [KEY_N]             = 0x31,
    [KEY_M]             = 0x32,
    [KEY_COMMA]         = 0x33,
    [KEY_DOT]           = 0x34,
    [KEY_SLASH]         = 0x35,
    [KEY_RIGHTSHIFT]    = 0x36,
    [KEY_SPACE]         = 0x39,

    [KEY_F1]            = 0x3b,
    [KEY_F2]            = 0x3c,
    [KEY_F3]            = 0x3d,
    [KEY_F4]            = 0x3e,
    [KEY_F5]            = 0x3f,
    [KEY_F6]            = 0x40,
    [KEY_F7]            = 0x41,
    [KEY_F8]            = 0x42,
    [KEY_F9]            = 0x43,
    [KEY_F10]           = 0x44,
    [KEY_F11]           = 0x57,
    [KEY_F12]           = 0x58,

    [KEY_SYSRQ]         = 0xb7,
    [KEY_SCROLLLOCK]    = 0x46,
#if 0
    [KEY_PAUSE]         = FIXME,
#endif
    [KEY_CAPSLOCK]      = 0x3a,
    [KEY_102ND]         = 0x56,

    [KEY_LEFTCTRL]      = 0x1d,
    [KEY_LEFTMETA]      = 0xdb,
    [KEY_LEFTALT]       = 0x38,
    [KEY_RIGHTALT]      = 0xb8,
    [KEY_RIGHTMETA]     = 0xdc,
    [KEY_RIGHTCTRL]     = 0x9d,
    [KEY_COMPOSE]       = 0xdd,

    [KEY_INSERT]        = 0xd2,
    [KEY_DELETE]        = 0xd3,
    [KEY_HOME]          = 0xc7,
    [KEY_END]           = 0xcf,
    [KEY_PAGEUP]        = 0xc9,
    [KEY_PAGEDOWN]      = 0xd1,

    [KEY_UP]            = 0xc8,
    [KEY_LEFT]          = 0xcb,
    [KEY_RIGHT]         = 0xcd,
    [KEY_DOWN]          = 0xd0,

    [KEY_NUMLOCK]       = 0x45,
    [KEY_KPSLASH]       = 0xb5,
    [KEY_KPASTERISK]    = 0x37,
    [KEY_KP7]           = 0x47,
    [KEY_KP8]           = 0x48,
    [KEY_KP9]           = 0x49,
    [KEY_KPMINUS]       = 0x4a,
    [KEY_KP4]           = 0x4b,
    [KEY_KP5]           = 0x4c,
    [KEY_KP6]           = 0x4d,
    [KEY_KPPLUS]        = 0x4e,
    [KEY_KP1]           = 0x4f,
    [KEY_KP2]           = 0x50,
    [KEY_KP3]           = 0x51,
    [KEY_KP0]           = 0x52,
    [KEY_KPDOT]         = 0x53,
    [KEY_KPENTER]       = 0x9c,
};

static const struct keysym_map {
    int  normal, shifted;
} keysym_map_en_us[KEY_CNT] = {
    [KEY_A] = { .normal = 'a', .shifted = 'A' },
    [KEY_B] = { .normal = 'b', .shifted = 'B' },
    [KEY_C] = { .normal = 'c', .shifted = 'C' },
    [KEY_D] = { .normal = 'd', .shifted = 'D' },
    [KEY_E] = { .normal = 'e', .shifted = 'E' },
    [KEY_F] = { .normal = 'f', .shifted = 'F' },
    [KEY_G] = { .normal = 'g', .shifted = 'G' },
    [KEY_H] = { .normal = 'h', .shifted = 'H' },
    [KEY_I] = { .normal = 'i', .shifted = 'I' },
    [KEY_J] = { .normal = 'j', .shifted = 'J' },
    [KEY_K] = { .normal = 'k', .shifted = 'K' },
    [KEY_L] = { .normal = 'l', .shifted = 'L' },
    [KEY_M] = { .normal = 'm', .shifted = 'M' },
    [KEY_N] = { .normal = 'n', .shifted = 'N' },
    [KEY_O] = { .normal = 'o', .shifted = 'O' },
    [KEY_P] = { .normal = 'p', .shifted = 'P' },
    [KEY_Q] = { .normal = 'q', .shifted = 'Q' },
    [KEY_R] = { .normal = 'r', .shifted = 'R' },
    [KEY_S] = { .normal = 's', .shifted = 'S' },
    [KEY_T] = { .normal = 't', .shifted = 'T' },
    [KEY_U] = { .normal = 'u', .shifted = 'U' },
    [KEY_V] = { .normal = 'v', .shifted = 'V' },
    [KEY_W] = { .normal = 'w', .shifted = 'W' },
    [KEY_X] = { .normal = 'x', .shifted = 'X' },
    [KEY_Y] = { .normal = 'y', .shifted = 'Y' },
    [KEY_Z] = { .normal = 'z', .shifted = 'Z' },

    [KEY_1] = { .normal = '1', .shifted = '!' },
    [KEY_2] = { .normal = '2', .shifted = '@' },
    [KEY_3] = { .normal = '3', .shifted = '#' },
    [KEY_4] = { .normal = '4', .shifted = '$' },
    [KEY_5] = { .normal = '5', .shifted = '%' },
    [KEY_6] = { .normal = '6', .shifted = '^' },
    [KEY_7] = { .normal = '7', .shifted = '&' },
    [KEY_8] = { .normal = '8', .shifted = '*' },
    [KEY_9] = { .normal = '9', .shifted = '(' },
    [KEY_0] = { .normal = '0', .shifted = ')' },

    [KEY_MINUS]       = { .normal = '-',  .shifted = '_'  },
    [KEY_EQUAL]       = { .normal = '=',  .shifted = '+'  },
    [KEY_TAB]         = { .normal = '\t'  },
    [KEY_LEFTBRACE]   = { .normal = '[',  .shifted = '{'  },
    [KEY_RIGHTBRACE]  = { .normal = ']',  .shifted = '}'  },
    [KEY_ENTER]       = { .normal = '\n', },
    [KEY_SEMICOLON]   = { .normal = ';',  .shifted = ':'  },
    [KEY_APOSTROPHE]  = { .normal = '"',  .shifted = '\'' },
    [KEY_BACKSLASH]   = { .normal = '\\', .shifted = '|'  },
    [KEY_COMMA]       = { .normal = ',',  .shifted = '<'  },
    [KEY_DOT]         = { .normal = '.',  .shifted = '>'  },
    [KEY_SLASH]       = { .normal = '/',  .shifted = '?'  },
    [KEY_SPACE]       = { .normal = ' '   },

    [KEY_BACKSPACE]   = { .normal = QEMU_KEY_BACKSPACE  },
    [KEY_UP]          = { .normal = QEMU_KEY_UP         },
    [KEY_DOWN]        = { .normal = QEMU_KEY_DOWN       },
    [KEY_LEFT]        = { .normal = QEMU_KEY_LEFT       },
    [KEY_RIGHT]       = { .normal = QEMU_KEY_RIGHT      },
};

static void start_mediumraw(FBDevState *s)
{
    struct termios tattr;

    if (s->tty_mediumraw) {
        return;
    }
    trace_fbdev_kbd_raw(1);

    /* save state */
    tcgetattr(s->tty, &s->tty_attributes);
    ioctl(s->tty, KDGKBMODE, &s->tty_mode);
    s->tty_flags = fcntl(s->tty, F_GETFL, NULL);

    /* setup */
    tattr = s->tty_attributes;
    tattr.c_cflag &= ~(IXON|IXOFF);
    tattr.c_lflag &= ~(ICANON|ECHO|ISIG);
    tattr.c_iflag = 0;
    tattr.c_cc[VMIN] = 1;
    tattr.c_cc[VTIME] = 0;
    tcsetattr(s->tty, TCSAFLUSH, &tattr);
    ioctl(s->tty, KDSKBMODE, K_MEDIUMRAW);
    fcntl(s->tty, F_SETFL, s->tty_flags | O_NONBLOCK);

    s->tty_mediumraw = true;
}

static void stop_mediumraw(FBDevState *s)
{
    if (!s->tty_mediumraw) {
        return;
    }
    trace_fbdev_kbd_raw(0);

    /* restore state */
    tcsetattr(s->tty, TCSANOW, &s->tty_attributes);
    ioctl(s->tty, KDSKBMODE, s->tty_mode);
    fcntl(s->tty, F_SETFL, s->tty_flags);

    s->tty_mediumraw = false;
}

static void send_scancode(int keycode, int up)
{
    int scancode = scancode_map[keycode];

    if (!scancode) {
        fprintf(stderr, "%s: unmapped key: 0x%x %s\n",
                __func__, keycode, keynames[keycode]);
        return;
    }
    if (scancode & SCANCODE_GREY) {
        kbd_put_keycode(SCANCODE_EMUL0);
    }
    if (up) {
        kbd_put_keycode(scancode | SCANCODE_UP);
    } else {
        kbd_put_keycode(scancode & SCANCODE_KEYCODEMASK);
    }
}

static void send_keysym(int keycode, int shift)
{
    const struct keysym_map *keysym_map = keysym_map_en_us;
    int keysym;

    if (shift && keysym_map[keycode].shifted) {
        keysym = keysym_map[keycode].shifted;
    } else if (keysym_map[keycode].normal) {
        keysym = keysym_map[keycode].normal;
    } else {
        fprintf(stderr, "%s: unmapped key: 0x%x %s\n",
                __func__, keycode, keynames[keycode]);
        return;
    }
    kbd_put_keysym(keysym);
}

static void reset_keys(FBDevState *s)
{
    int keycode;

    for (keycode = 0; keycode < KEY_MAX; keycode++) {
        if (s->key_down[keycode]) {
            if (qemu_console_is_graphic(NULL)) {
                send_scancode(keycode, 1);
            }
            s->key_down[keycode] = false;
        }
    }
}

static void read_mediumraw(void *opaque)
{
    FBDevState *s = opaque;
    uint8_t buf[32];
    int i, rc, up, keycode;
    bool ctrl, alt, shift;

    rc = read(s->tty, buf, sizeof(buf));
    switch (rc) {
    case -1:
        perror("read tty");
        goto err;
    case 0:
        fprintf(stderr, "%s: eof\n", __func__);
        goto err;
    default:
        for (i = 0; i < rc; i++) {
            up      = buf[i] & 0x80;
            keycode = buf[i] & 0x7f;
            if (keycode == 0) {
                keycode  = (buf[i+1] & 0x7f) << 7;
                keycode |= buf[i+2] & 0x7f;
                i += 2;
            }
            if (keycode > KEY_MAX) {
                continue;
            }

            if (up) {
                if (!s->key_down[keycode]) {
                    continue;
                }
                s->key_down[keycode] = false;
            } else {
                s->key_down[keycode] = true;
            }

            trace_fbdev_kbd_event(keycode, keynames[keycode], !up);

            alt   = s->key_down[KEY_LEFTALT]   || s->key_down[KEY_RIGHTALT];
            ctrl  = s->key_down[KEY_LEFTCTRL]  || s->key_down[KEY_RIGHTCTRL];
            shift = s->key_down[KEY_LEFTSHIFT] || s->key_down[KEY_RIGHTSHIFT];

            if (ctrl && alt && !up) {
                if (keycode == KEY_ESC) {
                    fprintf(stderr, "=== fbdev emergency escape "
                            "(ctrl-alt-esc) ===\n");
                    exit(1);
                }
                if (keycode == KEY_S) {
                    s->use_scale = !s->use_scale;
                    s->resize_screen++;
                    s->redraw_screen++;
                    continue;
                }
                if (keycode >= KEY_F1 && keycode <= KEY_F10) {
                    fbdev_activate_vt(s->tty, keycode+1-KEY_F1, false);
                    s->key_down[keycode] = false;
                    continue;
                }
                if (keycode >= KEY_1 && keycode <= KEY_9) {
                    console_select(keycode-KEY_1);
                    reset_keys(s);
                    continue;
                }
            }

            if (qemu_console_is_graphic(NULL)) {
                /* send scancode to guest kbd emulation */
                send_scancode(keycode, up);
            } else if (!up) {
                /* send keysym to text console (aka '-chardev vc') */
                send_keysym(keycode, shift);
            }
        }
    }
    return;

err:
    exit(1);
}

/* -------------------------------------------------------------------- */

static void fbdev_cls(FBDevState *s)
{
    memset(s->fb_mem + s->fb_mem_offset, 0,
           s->fb_fix.line_length * s->fb_var.yres);
}

static int fbdev_activate_vt(int tty, int vtno, bool wait)
{
    trace_fbdev_vt_activate(vtno, wait);

    if (ioctl(tty, VT_ACTIVATE, vtno) < 0) {
        perror("ioctl VT_ACTIVATE");
        return -1;
    }

    if (wait) {
        if (ioctl(tty, VT_WAITACTIVE, vtno) < 0) {
            perror("ioctl VT_WAITACTIVE");
            return -1;
        }
        trace_fbdev_vt_activated();
    }

    return 0;
}

static void fbdev_cleanup(FBDevState *s)
{
    trace_fbdev_cleanup();

    /* release pixman stuff */
    pixman_region_fini(&s->dirty);
    if (s->framebuffer) {
        pixman_image_unref(s->framebuffer);
        s->framebuffer = NULL;
    }
    if (s->sref) {
        pixman_image_unref(s->sref);
        s->sref = NULL;
    }
    if (s->swork) {
        pixman_image_unref(s->swork);
        s->swork = NULL;
    }

    /* restore console */
    if (s->fb_mem != NULL) {
        munmap(s->fb_mem, s->fb_fix.smem_len+s->fb_mem_offset);
        s->fb_mem = NULL;
    }
    if (s->fb != -1) {
        if (ioctl(s->fb, FBIOPUT_VSCREENINFO, &s->fb_ovar) < 0) {
            perror("ioctl FBIOPUT_VSCREENINFO");
        }
        close(s->fb);
        s->fb = -1;
    }

    if (s->tty != -1) {
        stop_mediumraw(s);
        if (ioctl(s->tty, KDSETMODE, s->kd_omode) < 0) {
            perror("ioctl KDSETMODE");
        }
        if (ioctl(s->tty, VT_SETMODE, &s->vt_omode) < 0) {
            perror("ioctl VT_SETMODE");
        }
        if (s->orig_vtno) {
            fbdev_activate_vt(s->tty, s->orig_vtno, true);
        }
        qemu_set_fd_handler(s->tty, NULL, NULL, NULL);
        close(s->tty);
        s->tty = -1;
    }

    g_free(s->device);
    s->device = NULL;
}

static int fbdev_init(FBDevState *s, const char *device, Error **err)
{
    struct vt_stat vts;
    unsigned long page_mask;
    char ttyname[32];

    /* open framebuffer */
    if (device == NULL) {
        device = "/dev/fb0";
    }
    s->fb = open(device, O_RDWR);
    if (s->fb == -1) {
        error_setg_file_open(err, errno, device);
        return -1;
    }

    /* open virtual console */
    s->tty = 0;
    if (ioctl(s->tty, VT_GETSTATE, &vts) < 0) {
        fprintf(stderr, "Not started from virtual terminal, "
                "trying to open one.\n");

        snprintf(ttyname, sizeof(ttyname), "/dev/tty0");
        s->tty = open(ttyname, O_RDWR);
        if (s->tty == -1) {
            error_setg(err, "open %s: %s\n", ttyname, strerror(errno));
            goto err_early;
        }
        if (ioctl(s->tty, VT_OPENQRY, &s->vtno) < 0) {
            error_setg(err, "ioctl VT_OPENQRY: %s\n", strerror(errno));
            goto err_early;
        }
        if (ioctl(s->tty, VT_GETSTATE, &vts) < 0) {
            error_setg(err, "ioctl VT_GETSTATE: %s\n", strerror(errno));
            goto err_early;
        }
        close(s->tty);

        snprintf(ttyname, sizeof(ttyname), "/dev/tty%d", s->vtno);
        s->tty = open(ttyname, O_RDWR);
        if (s->tty == -1) {
            error_setg(err, "open %s: %s\n", ttyname, strerror(errno));
            goto err_early;
        }
        s->orig_vtno = vts.v_active;
        fprintf(stderr, "Switching to vt %d (current %d).\n",
                s->vtno, s->orig_vtno);
    } else {
        s->orig_vtno = 0;
        s->vtno = vts.v_active;
        fprintf(stderr, "Started at vt %d, using it.\n", s->vtno);
    }
    fbdev_activate_vt(s->tty, s->vtno, true);

    /* get current settings (which we have to restore) */
    if (ioctl(s->fb, FBIOGET_VSCREENINFO, &s->fb_ovar) < 0) {
        error_setg(err, "ioctl FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        goto err_early;
    }
    if (ioctl(s->tty, KDGETMODE, &s->kd_omode) < 0) {
        error_setg(err, "ioctl KDGETMODE: %s\n", strerror(errno));
        goto err_early;
    }
    if (ioctl(s->tty, VT_GETMODE, &s->vt_omode) < 0) {
        error_setg(err, "ioctl VT_GETMODE: %s\n", strerror(errno));
        goto err_early;
    }

    /* checks & initialisation */
    if (ioctl(s->fb, FBIOGET_FSCREENINFO, &s->fb_fix) < 0) {
        error_setg(err, "ioctl : %s\n", strerror(errno));
        perror("ioctl FBIOGET_FSCREENINFO");
        goto err;
    }
    if (ioctl(s->fb, FBIOGET_VSCREENINFO, &s->fb_var) < 0) {
        error_setg(err, "ioctl FBIOGET_VSCREENINFO: %s\n", strerror(errno));
        goto err;
    }
    if (s->fb_fix.type != FB_TYPE_PACKED_PIXELS) {
        error_setg(err, "can handle only packed pixel frame buffers\n");
        goto err;
    }
    switch (s->fb_var.bits_per_pixel) {
    case 32:
        break;
    default:
        error_setg(err, "can't handle %d bpp frame buffers\n",
                   s->fb_var.bits_per_pixel);
        goto err;
    }

    page_mask = getpagesize()-1;
    fb_switch_state = FB_ACTIVE;
    s->fb_mem_offset = (unsigned long)(s->fb_fix.smem_start) & page_mask;
    s->fb_mem = mmap(NULL, s->fb_fix.smem_len+s->fb_mem_offset,
                     PROT_READ|PROT_WRITE, MAP_SHARED, s->fb, 0);
    if (s->fb_mem == MAP_FAILED) {
        error_setg(err, "mmap: %s\n", strerror(errno));
        goto err;
    }
    /* move viewport to upper left corner */
    if (s->fb_var.xoffset != 0 || s->fb_var.yoffset != 0) {
        s->fb_var.xoffset = 0;
        s->fb_var.yoffset = 0;
        if (ioctl(s->fb, FBIOPAN_DISPLAY, &s->fb_var) < 0) {
            error_setg(err, "ioctl FBIOPAN_DISPLAY: %s\n", strerror(errno));
            goto err;
        }
    }
    if (ioctl(s->tty, KDSETMODE, KD_GRAPHICS) < 0) {
        error_setg(err, "ioctl KDSETMODE: %s\n", strerror(errno));
        goto err;
    }
    /* some fb drivers need this again after switching to graphics ... */
    fbdev_activate_vt(s->tty, s->vtno, true);

    fbdev_cls(s);

    start_mediumraw(s);
    qemu_set_fd_handler(s->tty, read_mediumraw, NULL, s);

    s->framebuffer = pixman_from_framebuffer(s);
    pixman_region_init(&s->dirty);
    s->device = g_strdup(device);
    return 0;

err_early:
    if (s->tty > 0) {
        close(s->tty);
    }
    close(s->fb);
    return -1;

err:
    fbdev_cleanup(s);
    return -1;
}

static void
fbdev_catch_fatal_signal(int signr)
{
    fprintf(stderr, "%s: %s, restoring linux console state ...\n",
            __func__, strsignal(signr));
    fbdev_cleanup(fb);
    signal(SIGABRT, SIG_DFL);
    fprintf(stderr, "%s: ... done, going abort() now.\n", __func__);
    abort();
}

static void fbdev_catch_exit_signals(void)
{
    static const int signals[] = {
        SIGQUIT, SIGILL, SIGABRT, SIGFPE, SIGSEGV, SIGBUS
    };
    struct sigaction act, old;
    int i;

    memset(&act, 0, sizeof(act));
    act.sa_handler = fbdev_catch_fatal_signal;
    act.sa_flags = SA_RESETHAND;
    sigemptyset(&act.sa_mask);
    for (i = 0; i < ARRAY_SIZE(signals); i++) {
        sigaction(signals[i], &act, &old);
    }
}

/* -------------------------------------------------------------------- */
/* console switching                                                    */

static void fbdev_switch_signal(int signal)
{
    if (signal == SIG_REL) {
        /* release */
        trace_fbdev_vt_release_request();
        fb_switch_state = FB_REL_REQ;
    }
    if (signal == SIG_ACQ) {
        /* acquisition */
        trace_fbdev_vt_aquire_request();
        fb_switch_state = FB_ACQ_REQ;
    }
}

static void fbdev_switch_release(FBDevState *s)
{
    stop_mediumraw(s);
    ioctl(s->tty, KDSETMODE, s->kd_omode);
    ioctl(s->tty, VT_RELDISP, 1);
    fb_switch_state = FB_INACTIVE;
    trace_fbdev_vt_released();
}

static void fbdev_switch_acquire(FBDevState *s)
{
    ioctl(s->tty, VT_RELDISP, VT_ACKACQ);
    start_mediumraw(s);
    reset_keys(s);
    ioctl(s->tty, KDSETMODE, KD_GRAPHICS);
    fb_switch_state = FB_ACTIVE;
    trace_fbdev_vt_aquired();
}

static int fbdev_switch_init(FBDevState *s)
{
    struct sigaction act, old;

    memset(&act, 0, sizeof(act));
    act.sa_handler  = fbdev_switch_signal;
    sigemptyset(&act.sa_mask);
    sigaction(SIG_REL, &act, &old);
    sigaction(SIG_ACQ, &act, &old);

    if (ioctl(s->tty, VT_GETMODE, &s->vt_mode) < 0) {
        perror("ioctl VT_GETMODE");
        exit(1);
    }
    s->vt_mode.mode   = VT_PROCESS;
    s->vt_mode.waitv  = 0;
    s->vt_mode.relsig = SIG_REL;
    s->vt_mode.acqsig = SIG_ACQ;

    if (ioctl(s->tty, VT_SETMODE, &s->vt_mode) < 0) {
        perror("ioctl VT_SETMODE");
        exit(1);
    }
    return 0;
}

/* -------------------------------------------------------------------- */
/* rendering                                                            */

static void fbdev_render(FBDevState *s)
{
    assert(s->surface);

    pixman_image_set_clip_region(s->swork, &s->dirty);
    pixman_image_composite(PIXMAN_OP_SRC, s->swork, NULL, s->framebuffer,
                           0, 0, 0, 0, 0, 0, s->fb_var.xres, s->fb_var.yres);
    pixman_region_fini(&s->dirty);
    pixman_region_init(&s->dirty);
}

static void fbdev_unrender_ptr(FBDevState *s)
{
    if (!s->pw && !s->ph) {
        return;
    }
    pixman_region_union_rect(&s->dirty, &s->dirty,
                             s->px, s->py, s->pw, s->ph);
    s->ph = s->pw = 0;
}

static void fbdev_render_ptr(FBDevState *s)
{
    pixman_region16_t region;
    pixman_transform_t transform;

    if (!s->mon || !s->ptr_image) {
        return;
    }
    if (s->mx < 0 || s->mx >= s->cw || s->my < 0 || s->my >= s->ch) {
        return;
    }

    s->px = s->mx - s->ptr_cursor->hot_x;
    s->py = s->my - s->ptr_cursor->hot_y;
    s->pw = s->ptr_cursor->width;
    s->ph = s->ptr_cursor->height;

    pixman_transform_init_identity(&transform);
    pixman_transform_translate(&transform, NULL,
                               pixman_int_to_fixed(-s->cx),
                               pixman_int_to_fixed(-s->cy));
    if (s->use_scale) {
        pixman_transform_scale(&transform, NULL,
                               pixman_double_to_fixed(1/s->scale),
                               pixman_double_to_fixed(1/s->scale));
    }
    pixman_transform_translate(&transform, NULL,
                               pixman_int_to_fixed(-s->px),
                               pixman_int_to_fixed(-s->py));
    pixman_image_set_transform(s->ptr_image, &transform);

    pixman_region_init_rect(&region, 0, 0, s->pw, s->ph);
    pixman_image_set_clip_region(s->ptr_image, &region);

    pixman_image_composite(PIXMAN_OP_OVER, s->ptr_image, NULL, s->framebuffer,
                           0, 0, 0, 0, 0, 0, s->fb_var.xres, s->fb_var.yres);

    pixman_region_fini(&region);
    s->ptr_refresh = 0;
}

/* -------------------------------------------------------------------- */
/* qemu interfaces                                                      */

static void fbdev_update(DisplayChangeListener *dcl,
                         int x, int y, int w, int h)
{
    FBDevState *s = container_of(dcl, FBDevState, dcl);

    if (fb_switch_state != FB_ACTIVE) {
        return;
    }

    if (s->resize_screen) {
        double xs, ys;

        trace_fbdev_dpy_resize(surface_width(s->surface),
                               surface_height(s->surface));
        s->resize_screen = 0;
        s->cx = 0; s->cy = 0;
        s->cw = surface_width(s->surface);
        s->ch = surface_height(s->surface);

        if (s->use_scale) {
            xs = (double)s->fb_var.xres / s->cw;
            ys = (double)s->fb_var.yres / s->ch;
            if (xs > ys) {
                s->scale = ys;
                s->cx = (s->fb_var.xres -
                         surface_width(s->surface)*s->scale) / 2;
            } else {
                s->scale = xs;
                s->cy = (s->fb_var.yres -
                         surface_height(s->surface)*s->scale) / 2;
            }
        } else {
            s->scale = 1;
            if (surface_width(s->surface) < s->fb_var.xres) {
                s->cx = (s->fb_var.xres - surface_width(s->surface)) / 2;
            }
            if (surface_height(s->surface) < s->fb_var.yres) {
                s->cy = (s->fb_var.yres - surface_height(s->surface)) / 2;
            }
        }
        if (s->sref) {
            pixman_image_unref(s->sref);
        }
        s->sref = pixman_image_ref(s->surface->image);

        if (s->swork) {
            pixman_image_unref(s->swork);
        }
        s->swork = pixman_image_clone(s->sref);

        pixman_transform_init_identity(&s->transform);
        pixman_transform_translate(&s->transform, NULL,
                                   pixman_int_to_fixed(-s->cx),
                                   pixman_int_to_fixed(-s->cy));
        if (s->use_scale) {
            pixman_transform_scale(&s->transform, NULL,
                                   pixman_double_to_fixed(1/s->scale),
                                   pixman_double_to_fixed(1/s->scale));
        }
        pixman_image_set_transform(s->swork, &s->transform);

        pixman_image_set_filter(s->swork, s->pfilter, NULL, 0);
    }

    if (s->redraw_screen) {
        trace_fbdev_dpy_redraw();
        s->redraw_screen = 0;
        fbdev_cls(s);
        x = 0; y = 0;
        w = surface_width(s->surface);
        h = surface_height(s->surface);
    }

    pixman_region_union_rect(&s->dirty, &s->dirty, x, y, w, h);
    if (s->ptr_image && s->mon && s->pw && s->ph) {
        s->ptr_refresh++;
    }
}

static void fbdev_switch(DisplayChangeListener *dcl,
                         DisplaySurface *new_surface)
{
    FBDevState *s = container_of(dcl, FBDevState, dcl);

    s->surface = new_surface;
    s->resize_screen++;
    s->redraw_screen++;
}

static void fbdev_refresh(DisplayChangeListener *dcl)
{
    FBDevState *s = container_of(dcl, FBDevState, dcl);

    switch (fb_switch_state) {
    case FB_REL_REQ:
        fbdev_switch_release(s);
        /* fall though */
    case FB_INACTIVE:
        return;
    case FB_ACQ_REQ:
        fbdev_switch_acquire(s);
        s->redraw_screen++;
        /* fall though */
    case FB_ACTIVE:
        break;
    }

    graphic_hw_update(NULL);
    if (s->redraw_screen) {
        fbdev_update(dcl, 0, 0, 0, 0);
    }

    if (s->ptr_refresh) {
        fbdev_unrender_ptr(s);
    }
    if (pixman_region_not_empty(&s->dirty)) {
        fbdev_render(s);
    }
    if (s->ptr_refresh) {
        fbdev_render_ptr(s);
    }
}

static void fbdev_mouse_set(DisplayChangeListener *dcl, int x, int y, int on)
{
    FBDevState *s = container_of(dcl, FBDevState, dcl);

    s->ptr_refresh++;
    s->mx = x;
    s->my = y;
    s->mon = on;
}

static void fbdev_cursor_define(DisplayChangeListener *dcl, QEMUCursor *cursor)
{
    FBDevState *s = container_of(dcl, FBDevState, dcl);

    s->ptr_refresh++;

    if (s->ptr_cursor) {
        cursor_put(s->ptr_cursor);
        s->ptr_cursor = NULL;
    }
    if (s->ptr_image) {
        pixman_image_unref(s->ptr_image);
        s->ptr_image = NULL;
    }

    if (!cursor) {
        return;
    }

    s->ptr_cursor = cursor;
    cursor_get(s->ptr_cursor);
    s->ptr_image = pixman_image_create_bits(PIXMAN_a8r8g8b8,
                                            cursor->width, cursor->height,
                                            cursor->data,
                                            cursor->width * 4);
    pixman_image_set_filter(s->ptr_image, s->pfilter, NULL, 0);
}

static const DisplayChangeListenerOps fbdev_ops = {
    .dpy_name          = "fbdev",
    .dpy_gfx_update    = fbdev_update,
    .dpy_gfx_switch    = fbdev_switch,
    .dpy_refresh       = fbdev_refresh,
    .dpy_mouse_set     = fbdev_mouse_set,
    .dpy_cursor_define = fbdev_cursor_define,
};

static void fbdev_exit_notifier(Notifier *notifier, void *data)
{
    FBDevState *s = container_of(notifier, FBDevState, exit_notifier);
    fbdev_cleanup(s);
}

int fbdev_display_init(const char *device, bool scale, Error **err)
{
    FBDevState *s;

    if (fb != NULL) {
        return 0;
    }

    s = g_new0(FBDevState, 1);
    s->tty = -1;
    s->fb = -1;
    s->mice = -1;
    s->pfilter = PIXMAN_FILTER_GOOD;

    if (fbdev_init(s, device, err) != 0) {
        g_free(s);
        return -1;
    }

    s->exit_notifier.notify = fbdev_exit_notifier;
    qemu_add_exit_notifier(&s->exit_notifier);
    fbdev_switch_init(s);
    fbdev_catch_exit_signals();
    init_mouse(s);
    s->use_scale = scale;

    s->dcl.ops = &fbdev_ops;
    register_displaychangelistener(&s->dcl);

    trace_fbdev_enabled();
    fb = s;
    return 0;
}

void fbdev_display_uninit(void)
{
    FBDevState *s = fb;

    if (s == NULL) {
        return;
    }

    unregister_displaychangelistener(&s->dcl);
    qemu_remove_exit_notifier(&s->exit_notifier);
    fbdev_cleanup(s);
    uninit_mouse(s);
    g_free(s);
    fb = NULL;
}
