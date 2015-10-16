#include "qemu/osdep.h"
#include "qemu-common.h"
#include "qemu/sockets.h"
#include "qemu/main-loop.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/egl-proto.h"
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"

#include "sysemu/sysemu.h"

#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#ifndef EGL_MESA_image_dma_buf_export
# error missing EGL_MESA_image_dma_buf_export, your mesa is too old
#endif

typedef struct egl_ui egl_ui;
typedef struct egl_conn egl_conn;
typedef struct egl_dpy egl_dpy;

struct egl_ui {
    QTAILQ_HEAD(, egl_dpy) displays;

    int listen_sock;
    QTAILQ_HEAD(, egl_conn) clients;
};

struct egl_conn {
    egl_ui *egl;
    int sock;
    QTAILQ_ENTRY(egl_conn) node;
};

struct egl_dpy {
    egl_ui *egl;
    int idx;
    DisplayChangeListener dcl;
    QTAILQ_ENTRY(egl_dpy) node;

    /* current surface */
    DisplaySurface *ds;
    ConsoleGLState *gls;
    egl_msg newbuf;
    int dmabuf_fd;
    int updates;
};

#define GL_CHECK_ERROR() do {                           \
        GLint err = glGetError();                       \
        if (err != GL_NO_ERROR) {                       \
            fprintf(stderr, "%s:%d: gl error 0x%x\n",   \
                    __func__, __LINE__, err);           \
        }                                               \
    } while (0)

/* ---------------------------------------------------------------------- */

static ssize_t write_fd(int fd, void *buf, size_t count, int msgfd)
{
    char msgbuf[CMSG_SPACE(sizeof(msgfd))];
    struct msghdr msg = {
        .msg_control = msgbuf,
        .msg_controllen = sizeof(msgbuf),
    };
    struct cmsghdr *cmsg;
    struct iovec iov;

    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    msg.msg_controllen = cmsg->cmsg_len;

    iov.iov_base = buf;
    iov.iov_len = count;

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    memcpy(CMSG_DATA(cmsg), &msgfd, sizeof(msgfd));

    return sendmsg(fd, &msg, 0);
}

static void egl_sock_close(egl_conn *econn)
{
    fprintf(stderr, "%s/%d:\n", __func__, econn->sock);
    QTAILQ_REMOVE(&econn->egl->clients, econn, node);
    qemu_set_fd_handler(econn->sock, NULL, NULL, NULL);
    close(econn->sock);
    g_free(econn);
}

static int egl_send_one(egl_conn *econn, egl_msg *msg, int msgfd)
{
    int ret;

    if (msgfd < 0) {
        ret = write(econn->sock, msg, sizeof(*msg));
    } else {
        ret = write_fd(econn->sock, msg, sizeof(*msg), msgfd);
    }
    if (ret != sizeof(*msg)) {
        egl_sock_close(econn);
        return -1;
    }
    return 0;
}

static void egl_send_all(egl_ui *egl, egl_msg *msg, int msgfd)
{
    egl_conn *econn, *tmp;

    QTAILQ_FOREACH_SAFE(econn, &egl->clients, node, tmp) {
        egl_send_one(econn, msg, msgfd);
    }
}

static void egl_sock_read(void *opaque)
{
    egl_conn *econn = opaque;
    egl_dpy *edpy = QTAILQ_FIRST(&econn->egl->displays);
    egl_msg msg;
    int ret, btn;

    for (;;) {
        ret = read(econn->sock, &msg, sizeof(msg));
        if (ret == -1 && errno == EAGAIN) {
            return;
        }
        if (ret != sizeof(msg)) {
            graphic_hw_gl_block(edpy->dcl.con, false);
            egl_sock_close(econn);
            return;
        }

        switch (msg.type) {
        case EGL_MOTION:
            qemu_input_queue_abs(NULL, INPUT_AXIS_X,
                                 msg.u.motion.x, msg.u.motion.w);
            qemu_input_queue_abs(NULL, INPUT_AXIS_Y,
                                 msg.u.motion.y, msg.u.motion.h);
            qemu_input_event_sync();
            break;
        case EGL_BUTTON_PRESS:
        case EGL_BUTTON_RELEASE:
            if (msg.u.button.button == 1) {
                btn = INPUT_BUTTON_LEFT;
            } else if (msg.u.button.button == 2) {
                btn = INPUT_BUTTON_MIDDLE;
            } else if (msg.u.button.button == 3) {
                btn = INPUT_BUTTON_RIGHT;
            } else {
                break;
            }
            qemu_input_queue_btn(NULL, btn, msg.type == EGL_BUTTON_PRESS);
            qemu_input_event_sync();
            break;
        case EGL_KEY_PRESS:
        case EGL_KEY_RELEASE:
            qemu_input_event_send_key_number(NULL, msg.u.key.keycode,
                                             msg.type == EGL_KEY_PRESS);
            break;
        case EGL_DRAW_DONE:
            graphic_hw_gl_block(edpy->dcl.con, false);
            break;
        default:
            fprintf(stderr, "%s/%d: unhandled msg type %d\n",
                    __func__, econn->sock, msg.type);
            break;
        }
    }
}

static void egl_sock_accept(void *opaque)
{
    egl_ui *egl = opaque;
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    egl_dpy *edpy;
    egl_conn *econn;
    int sock;

    sock = qemu_accept(egl->listen_sock, (struct sockaddr *)&addr,
                       &addrlen);
    if (sock == -1) {
        return;
    }

    econn = g_new0(egl_conn, 1);
    econn->egl = egl;
    econn->sock = sock;
    QTAILQ_INSERT_TAIL(&egl->clients, econn, node);

    qemu_set_nonblock(econn->sock);
    qemu_set_fd_handler(econn->sock, egl_sock_read, NULL, econn);

    QTAILQ_FOREACH(edpy, &egl->displays, node) {
        if (egl_send_one(econn, &edpy->newbuf, edpy->dmabuf_fd) != 0) {
            break;
        }
    }
}

/* ---------------------------------------------------------------------- */


/* ---------------------------------------------------------------------- */

static void egl_refresh(DisplayChangeListener *dcl)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    if (!edpy->ds) {
        return;
    }

    graphic_hw_update(dcl->con);

    if (edpy->updates) {
        egl_msg msg = {
            .type = EGL_UPDATE,
            .display = edpy->idx,
        };
        egl_send_all(edpy->egl, &msg, -1);
        edpy->updates = 0;
    }
}

static void egl_gfx_update(DisplayChangeListener *dcl,
                           int x, int y, int w, int h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);

    surface_gl_update_texture(edpy->gls, edpy->ds, x, y, w, h);
    edpy->updates++;
}

static void egl_gfx_switch(DisplayChangeListener *dcl,
                           struct DisplaySurface *new_surface)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    EGLint stride, fourcc;
    int fd;

    surface_gl_destroy_texture(edpy->gls, edpy->ds);
    edpy->ds = new_surface;
    if (edpy->ds) {
        surface_gl_create_texture(edpy->gls, edpy->ds);
        fd = egl_get_fd_for_texture(edpy->ds->texture,
                                    &stride, &fourcc);
        if (fd < 0) {
            surface_gl_destroy_texture(edpy->gls, edpy->ds);
            return;
        }

        if (edpy->dmabuf_fd != -1) {
            close(edpy->dmabuf_fd);
        }
        edpy->dmabuf_fd = fd;

        fprintf(stderr, "%s: %dx%d (stride %d/%d, fourcc 0x%x)\n", __func__,
                surface_width(edpy->ds), surface_height(edpy->ds),
                surface_stride(edpy->ds), stride, fourcc);

        edpy->newbuf.type = EGL_NEWBUF;
        edpy->newbuf.display = edpy->idx;
        edpy->newbuf.u.newbuf.width = surface_width(edpy->ds);
        edpy->newbuf.u.newbuf.height = surface_height(edpy->ds);
        edpy->newbuf.u.newbuf.stride = stride;
        edpy->newbuf.u.newbuf.fourcc = fourcc;
        edpy->newbuf.u.newbuf.y0_top = false;

        egl_send_all(edpy->egl, &edpy->newbuf, edpy->dmabuf_fd);
    }
}

static void egl_mouse_set(DisplayChangeListener *dcl,
                          int x, int y, int on)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    egl_msg msg = {
        .type = EGL_POINTER_SET,
        .display      = edpy->idx,
        .u.ptr_set.x  = x,
        .u.ptr_set.y  = y,
        .u.ptr_set.on = on,
    };

    egl_send_all(edpy->egl, &msg, -1);
}

static void egl_cursor_define(DisplayChangeListener *dcl,
                              QEMUCursor *cursor)
{
}

static void egl_scanout(DisplayChangeListener *dcl,
                        uint32_t backing_id, bool backing_y_0_top,
                        uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    EGLint stride, fourcc;
    int fd;

    if (!w || !h) {
        return;
    }

    fd = egl_get_fd_for_texture(backing_id,
                                &stride, &fourcc);
    if (fd < 0) {
        return;
    }

    if (edpy->dmabuf_fd != -1) {
        close(edpy->dmabuf_fd);
    }
    edpy->dmabuf_fd = fd;

    fprintf(stderr, "%s: %dx%d (stride %d, fourcc 0x%x)\n", __func__,
            w, h, stride, fourcc);
    edpy->newbuf.type = EGL_NEWBUF;
    edpy->newbuf.display = edpy->idx;
    edpy->newbuf.u.newbuf.width = surface_width(edpy->ds);
    edpy->newbuf.u.newbuf.height = surface_height(edpy->ds);
    edpy->newbuf.u.newbuf.stride = stride;
    edpy->newbuf.u.newbuf.fourcc = fourcc;
    edpy->newbuf.u.newbuf.y0_top = backing_y_0_top;

    egl_send_all(edpy->egl, &edpy->newbuf, edpy->dmabuf_fd);
}

static void egl_scanout_flush(DisplayChangeListener *dcl,
                              uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    egl_dpy *edpy = container_of(dcl, egl_dpy, dcl);
    egl_msg msg = {
        .type = EGL_UPDATE,
        .display = edpy->idx,
    };

    if (!QTAILQ_EMPTY(&edpy->egl->clients)) {
        graphic_hw_gl_block(edpy->dcl.con, true);
    }
    egl_send_all(edpy->egl, &msg, -1);
}

static const DisplayChangeListenerOps egl_ops = {
    .dpy_name          = "egl",
    .dpy_refresh       = egl_refresh,
    .dpy_gfx_update    = egl_gfx_update,
    .dpy_gfx_switch    = egl_gfx_switch,
    .dpy_mouse_set     = egl_mouse_set,
    .dpy_cursor_define = egl_cursor_define,

    .dpy_gl_ctx_create       = qemu_egl_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
    .dpy_gl_ctx_get_current  = qemu_egl_get_current_context,

    .dpy_gl_scanout          = egl_scanout,
    .dpy_gl_update           = egl_scanout_flush,
};

int egl_init(void)
{
    char sockpath[128];
    QemuConsole *con;
    egl_dpy *edpy;
    egl_ui *egl;
    int idx;

#if 0
    setenv("EGL_LOG_LEVEL", "debug", false);
    setenv("MESA_DEBUG", "1", false);
#endif

    egl = g_new0(egl_ui, 1);
    egl->listen_sock = -1;
    QTAILQ_INIT(&egl->clients);
    QTAILQ_INIT(&egl->displays);

    if (egl_rendernode_init() < 0) {
        fprintf(stderr, "egl: render node init failed\n");
        goto err;
    }

    snprintf(sockpath, sizeof(sockpath), EGL_SOCKPATH,
             qemu_get_vm_name() ?: "noname");
    egl->listen_sock = unix_listen(sockpath, NULL, 0, NULL);
    if (egl->listen_sock == -1) {
        fprintf(stderr, "egl: creating unix socket %s failed\n",
                sockpath);
        goto err;
    }
    qemu_set_fd_handler(egl->listen_sock, egl_sock_accept, NULL, egl);
    chmod(sockpath, 0777);

    for (idx = 0;; idx++) {
        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        edpy = g_new0(egl_dpy, 1);
        edpy->egl = egl;
        edpy->idx = idx;
        edpy->dcl.con = con;
        edpy->dcl.ops = &egl_ops;
        edpy->gls = console_gl_init_context();
        edpy->dmabuf_fd = -1;
        QTAILQ_INSERT_TAIL(&egl->displays, edpy, node);

        register_displaychangelistener(&edpy->dcl);
#if 1
        /* FIXME: qemu-eglview can handle one display only */
        break;
#endif
    }

    display_opengl = 1;
    return 0;

err:
    g_free(egl);
    return -1;
}
