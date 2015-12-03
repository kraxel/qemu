
#define EGL_SOCKPATH "/tmp/qemu-egl-sock-%s"

enum egl_type {
    EGL_UNDEF,

    /* qemu -> eglview */
    EGL_NEWBUF = 100,
    EGL_UPDATE,
    EGL_POINTER_SET,

    /* eglview -> qemu */
    EGL_MOTION = 200,
    EGL_BUTTON_PRESS,
    EGL_BUTTON_RELEASE,
    EGL_KEY_PRESS,
    EGL_KEY_RELEASE,
    EGL_DRAW_DONE,
};

typedef struct egl_msg {
    enum egl_type type;
    uint32_t display;
    union {
        struct egl_newbuf {
            uint32_t width;
            uint32_t height;
            uint32_t stride;
            uint32_t fourcc;
            bool     y0_top;
        } newbuf;
        struct egl_ptr_set {
            uint32_t x;
            uint32_t y;
            uint32_t on;
        } ptr_set;
        struct egl_motion {
            uint32_t x;
            uint32_t y;
            uint32_t w;
            uint32_t h;
        } motion;
        struct egl_button {
            uint32_t button;
        } button;
        struct egl_key {
            uint32_t keycode;
        } key;
    } u;
} egl_msg;
