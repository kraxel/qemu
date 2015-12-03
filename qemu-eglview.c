#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <glob.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>

#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include "ui/egl-proto.h"
#include "ui/egl-helpers.h"
#include "ui/shader.h"
#include "ui/x_keymap.h"

#include "config-host.h"
#ifdef CONFIG_PRAGMA_DIAGNOSTIC_AVAILABLE
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

/* ---------------------------------------------------------------------- */

#define APPNAME "qemu-eglview"

static GtkWidget *top, *vbox, *draw;
static int width = 640;
static int height = 480;
static int debug;

static EGLContext egl_ctx;
static EGLSurface egl_surface;

static int sock;
static GIOChannel *ioc;

static uint32_t buf_width;
static uint32_t buf_height;
static bool buf_y0_top;
static EGLImageKHR buf_image = EGL_NO_IMAGE_KHR;
static GLuint buf_tex_id;

static GLint texture_blit_prog;
static GLint texture_blit_flip_prog;
static GLint texture_blit_vao;

#define GL_CHECK_ERROR() do {                                   \
        GLint err = glGetError();                               \
        if (err != GL_NO_ERROR) {                               \
            fprintf(stderr, APPNAME ":%s:%d:ERROR:GL: 0x%x\n",  \
                    __func__, __LINE__, err);                   \
        }                                                       \
    } while (0)

#include "ui/shader/texture-blit-vert.h"
#include "ui/shader/texture-blit-flip-vert.h"
#include "ui/shader/texture-blit-oes-frag.h"

/* ---------------------------------------------------------------------- */

static void egl_init(void)
{
    GdkDisplay *gdk_display = gtk_widget_get_display(draw);
    Display *x11_display = gdk_x11_display_get_xdisplay(gdk_display);
    GdkWindow *gdk_window = gtk_widget_get_window(draw);
    Window x11_window = gdk_x11_window_get_xid(gdk_window);

    if (qemu_egl_init_dpy((EGLNativeDisplayType)x11_display,
                          true, false) < 0) {
        fprintf(stderr, "%s: qemu_egl_init_dpy failed\n", __func__);
        exit(1);
    }

    egl_ctx = qemu_egl_init_ctx();
    egl_surface = qemu_egl_init_surface_x11(egl_ctx, x11_window);
}

static gboolean egl_draw(GtkWidget *widget, cairo_t *cr, void *opaque)
{
    GdkWindow *win = gtk_widget_get_window(draw);
    egl_msg msg = {
        .type = EGL_DRAW_DONE,
        .display = 0,
    };
    int ret;
#if 0
    float sw, sh;
    int stripe;
#endif

    width = gdk_window_get_width(win);
    height = gdk_window_get_height(win);

#if 0
    /* TODO: this needs mouse motion fixups */
    sw = (float)width/buf_width;
    sh = (float)height/buf_height;
    if (sw < sh) {
        stripe = height - height*sw/sh;
        glViewport(0, stripe / 2, width, height - stripe);
    } else {
        stripe = width - width*sh/sw;
        glViewport(stripe / 2, 0, width - stripe, height);
    }
#else
    glViewport(0, 0, width, height);
#endif

    glClearColor(0.1f, 0.1f, 0.1f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (buf_y0_top) {
        qemu_gl_run_texture_blit(texture_blit_flip_prog,
                                 texture_blit_vao);
    } else {
        qemu_gl_run_texture_blit(texture_blit_prog,
                                 texture_blit_vao);
    }
    eglSwapBuffers(qemu_egl_display, egl_surface);

    ret = write(sock, &msg, sizeof(msg));
    if (ret != sizeof(msg)) {
        fprintf(stderr, "socket error\n");
        exit(1);
    }

    return TRUE;
}

static void egl_delbuf(void)
{
    if (buf_image != EGL_NO_IMAGE_KHR) {
        glDeleteTextures(1, &buf_tex_id);
        eglDestroyImageKHR(qemu_egl_display, buf_image);
        buf_image = EGL_NO_IMAGE_KHR;
    }
}

static void egl_newbuf(egl_msg *msg, int msgfd)
{
    EGLint attrs[13];

    egl_delbuf();

    fprintf(stderr, APPNAME ": %s, fd %d, %dx%d\n", __func__,
            msgfd, msg->u.newbuf.width, msg->u.newbuf.height);
    buf_width = msg->u.newbuf.width;
    buf_height = msg->u.newbuf.height;
    buf_y0_top = msg->u.newbuf.y0_top;

    gtk_widget_set_size_request(draw, buf_width, buf_height);

    attrs[0] = EGL_DMA_BUF_PLANE0_FD_EXT;
    attrs[1] = msgfd;
    attrs[2] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
    attrs[3] = msg->u.newbuf.stride;
    attrs[4] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
    attrs[5] = 0;
    attrs[6] = EGL_WIDTH;
    attrs[7] = buf_width;
    attrs[8] = EGL_HEIGHT;
    attrs[9] = buf_height;
    attrs[10] = EGL_LINUX_DRM_FOURCC_EXT;
    attrs[11] = msg->u.newbuf.fourcc;
    attrs[12] = EGL_NONE;
    buf_image = eglCreateImageKHR(qemu_egl_display,
                                  EGL_NO_CONTEXT,
                                  EGL_LINUX_DMA_BUF_EXT,
                                  NULL, attrs);
    if (buf_image == EGL_NO_IMAGE_KHR) {
        error(1, 0, "failed to import image dma-buf\n");
    }
    close(msgfd);

    glGenTextures(1, &buf_tex_id);
    GL_CHECK_ERROR();
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, buf_tex_id);
    GL_CHECK_ERROR();
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
                    GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_CHECK_ERROR();
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES,
                    GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_CHECK_ERROR();

    glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES,
                                 (GLeglImageOES)buf_image);
    GL_CHECK_ERROR();
}

/* ---------------------------------------------------------------------- */

static ssize_t read_fd(int fd, void *buf, size_t count, int *msgfd)
{
    struct msghdr msg = { NULL, };
    struct iovec iov[1];
    union {
        struct cmsghdr cmsg;
        char control[CMSG_SPACE(sizeof(int))];
    } msg_control;
    struct cmsghdr *cmsg;
    ssize_t ret;

    iov[0].iov_base = buf;
    iov[0].iov_len = count;

    msg.msg_iov = iov;
    msg.msg_iovlen = 1;
    msg.msg_control = &msg_control;
    msg.msg_controllen = sizeof(msg_control);

    ret = recvmsg(fd, &msg, 0);
    if (ret > 0) {
        for (cmsg = CMSG_FIRSTHDR(&msg);
             cmsg;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
                cmsg->cmsg_level != SOL_SOCKET ||
                cmsg->cmsg_type != SCM_RIGHTS) {
                continue;
            }

            *msgfd = *((int *)CMSG_DATA(cmsg));
            if (*msgfd < 0) {
                continue;
            }
        }
    }
    return ret;
}

static gboolean egl_sock_read(GIOChannel *source,
                              GIOCondition condition,
                              gpointer data)
{
    egl_msg msg;
    int ret, msgfd;

    if (condition != G_IO_IN) {
        fprintf(stderr, APPNAME ": %s: socked error or closed\n", __func__);
        exit(0);
    }

    for (;;) {
        msgfd = -1;
        ret = read_fd(sock, &msg, sizeof(msg), &msgfd);
        if (ret == -1 && errno == EAGAIN) {
            return TRUE;
        }
        if (ret != sizeof(msg)) {
            fprintf(stderr, APPNAME ": %s: socked error or closed\n", __func__);
            exit(0);
        }

        switch (msg.type) {
        case EGL_NEWBUF:
            egl_newbuf(&msg, msgfd);
            break;
        case EGL_UPDATE:
            gtk_widget_queue_draw_area(draw, 0, 0, width, height);
            break;
        case EGL_POINTER_SET:
            fprintf(stderr, APPNAME ": %s: ptr set +%d+%d %s [TODO]\n",
                    __func__, msg.u.ptr_set.x, msg.u.ptr_set.y,
                    msg.u.ptr_set.on ? "on" : "off");
            break;
        default:
            fprintf(stderr, APPNAME ": %s: unhandled msg type %d\n",
                    __func__, msg.type);
            break;
        }
    }
}

/* ---------------------------------------------------------------------- */

/* from ui/gtk.c, simplified a bit */
static int map_keycode(GdkDisplay *dpy, int gdk_keycode)
{
    int qemu_keycode;

    if (gdk_keycode < 9) {
        qemu_keycode = 0;
    } else if (gdk_keycode < 97) {
        qemu_keycode = gdk_keycode - 8;
#ifdef GDK_WINDOWING_X11
    } else if (GDK_IS_X11_DISPLAY(dpy) && gdk_keycode < 158) {
        qemu_keycode = translate_evdev_keycode(gdk_keycode - 97);
#endif
    } else if (gdk_keycode == 208) { /* Hiragana_Katakana */
        qemu_keycode = 0x70;
    } else if (gdk_keycode == 211) { /* backslash */
        qemu_keycode = 0x73;
    } else {
        qemu_keycode = 0;
    }

    return qemu_keycode;
}

static gboolean draw_event(GtkWidget *widget, GdkEvent *event, void *opaque)
{
    egl_msg msg = {
        .type = 0,
        .display = 0,
    };
    int ret;

    switch (event->type) {
    case GDK_MOTION_NOTIFY:
        msg.type = EGL_MOTION;
        msg.u.motion.x = event->motion.x;
        msg.u.motion.y = event->motion.y;
        msg.u.motion.w = width;
        msg.u.motion.h = height;
        break;
    case GDK_BUTTON_PRESS:
        msg.type = EGL_BUTTON_PRESS;
        msg.u.button.button = event->button.button;
        break;
    case GDK_BUTTON_RELEASE:
        msg.type = EGL_BUTTON_RELEASE;
        msg.u.button.button = event->button.button;
        break;
    case GDK_KEY_PRESS:
        msg.type = EGL_KEY_PRESS;
        msg.u.key.keycode = map_keycode(gtk_widget_get_display(draw),
                                        event->key.hardware_keycode);
        break;
    case GDK_KEY_RELEASE:
        msg.type = EGL_KEY_RELEASE;
        msg.u.key.keycode = map_keycode(gtk_widget_get_display(draw),
                                        event->key.hardware_keycode);
        break;
    default:
        return FALSE;
    }

    ret = write(sock, &msg, sizeof(msg));
    if (ret != sizeof(msg)) {
        fprintf(stderr, "socket error\n");
        exit(1);
    }
    return TRUE;
}

/* ---------------------------------------------------------------------- */

static void usage(FILE *fp)
{
    fprintf(fp,
            "This is a virtual machine viewer.\n"
            "\n"
            "usage: %s [ options ] name\n"
            "options:\n"
            "   -h          Print this text.\n"
            "   -d          Enable debugging.\n"
            "\n"
            "--\n"
            "(c) 2015 Gerd Hoffmann <kraxel@redhat.com>\n",
            APPNAME);
}

int main(int argc, char *argv[])
{
    struct sockaddr_un un;
    const char *name;
    int c, f, ret;

    /* parse args */
    gtk_init(&argc, &argv);
    for (;;) {
        c = getopt(argc, argv, "hd");
        if (c < 0) {
            break;
        }
        switch (c) {
        case 'd':
            debug++;
            break;
        case 'h':
            usage(stdout);
            exit(0);
        default:
            usage(stderr);
            exit(1);
        }
    }

    /* connect socket */
    name = (optind == argc) ? "noname" : argv[optind];
    sock = socket(PF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        error(1, errno, "socket(PF_UNIX) failed");
    }

    memset(&un, 0, sizeof(un));
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof(un.sun_path), EGL_SOCKPATH, name);
    ret = connect(sock, (struct sockaddr *) &un, sizeof(un));
    if (ret < 0) {
        error(1, errno, "connect to %s\n", un.sun_path);
    }

    f = fcntl(sock, F_GETFL);
    fcntl(sock, F_SETFL, f | O_NONBLOCK);

    ioc = g_io_channel_unix_new(sock);
    g_io_add_watch(ioc, G_IO_IN | G_IO_ERR | G_IO_HUP,
                   egl_sock_read, NULL);

    /* setup gtk window */
    top = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    draw = gtk_drawing_area_new();

    g_signal_connect_swapped(G_OBJECT(top), "destroy",
                             G_CALLBACK(gtk_main_quit), NULL);
    gtk_widget_set_size_request(draw, width, height);

    g_signal_connect(draw, "event",
                     G_CALLBACK(draw_event), NULL);
    gtk_widget_add_events(draw,
                          GDK_POINTER_MOTION_MASK |
                          GDK_BUTTON_PRESS_MASK |
                          GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON_MOTION_MASK |
                          GDK_ENTER_NOTIFY_MASK |
                          GDK_LEAVE_NOTIFY_MASK |
                          GDK_SCROLL_MASK |
                          GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus(draw, TRUE);

    gtk_container_add(GTK_CONTAINER(top), vbox);
    gtk_box_pack_start(GTK_BOX(vbox), draw, TRUE, TRUE, 0);

    gtk_widget_show_all(top);

    egl_init();
    gtk_widget_set_double_buffered(draw, FALSE);
    texture_blit_prog = qemu_gl_create_compile_link_program
        (texture_blit_vert_src, texture_blit_oes_frag_src);
    texture_blit_flip_prog = qemu_gl_create_compile_link_program
        (texture_blit_flip_vert_src, texture_blit_oes_frag_src);
    if (!texture_blit_prog) {
        fprintf(stderr, "shader compile/link failure\n");
        exit(1);
    }

    texture_blit_vao =
        qemu_gl_init_texture_blit(texture_blit_prog);

    g_signal_connect(draw, "draw", G_CALLBACK(egl_draw), NULL);

    gtk_main();
    return 0;
}
