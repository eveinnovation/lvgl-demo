/**
 * @file wayland.c
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "wayland.h"

#if USE_WAYLAND

#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <sys/mman.h>

#include <linux/input.h>
#include <linux/input-event-codes.h>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#if !(LV_WAYLAND_XDG_SHELL || LV_WAYLAND_WL_SHELL)
#error "Please select at least one shell integration for Wayland driver"
#endif

#if LV_WAYLAND_XDG_SHELL
#include "protocols/wayland-xdg-shell-client-protocol.h"
#endif

/*********************
 *      DEFINES
 *********************/

#define BYTES_PER_PIXEL ((LV_COLOR_DEPTH + 7) / 8)

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
#define TITLE_BAR_HEIGHT 24
#define BORDER_SIZE 2
#define BUTTON_MARGIN LV_MAX((TITLE_BAR_HEIGHT / 6), BORDER_SIZE)
#define BUTTON_PADDING LV_MAX((TITLE_BAR_HEIGHT / 8), BORDER_SIZE)
#define BUTTON_SIZE (TITLE_BAR_HEIGHT - (2 * BUTTON_MARGIN))
#endif

#ifndef LV_WAYLAND_CYCLE_PERIOD
#define LV_WAYLAND_CYCLE_PERIOD LV_MIN(LV_DISP_DEF_REFR_PERIOD,1)
#endif

/**********************
 *      TYPEDEFS
 **********************/

enum object_type {
    OBJECT_TITLEBAR,
    OBJECT_BUTTON_CLOSE,
#if LV_WAYLAND_XDG_SHELL
    OBJECT_BUTTON_MAXIMIZE,
    OBJECT_BUTTON_MINIMIZE,
#endif
    OBJECT_BORDER_TOP,
    OBJECT_BORDER_BOTTOM,
    OBJECT_BORDER_LEFT,
    OBJECT_BORDER_RIGHT,
    FIRST_DECORATION = OBJECT_TITLEBAR,
    LAST_DECORATION = OBJECT_BORDER_RIGHT,
    OBJECT_WINDOW,
};

#define NUM_DECORATIONS (LAST_DECORATION-FIRST_DECORATION+1)

struct window;
struct input
{
    struct
    {
        lv_coord_t x;
        lv_coord_t y;
        lv_indev_state_t left_button;
        lv_indev_state_t right_button;
        lv_indev_state_t wheel_button;
        int16_t wheel_diff;
    } pointer;

    struct
    {
        lv_key_t key;
        lv_indev_state_t state;
    } keyboard;

    struct
    {
        lv_coord_t x;
        lv_coord_t y;
        lv_indev_state_t state;
    } touch;
};

struct seat
{
    struct wl_touch *wl_touch;
    struct wl_pointer *wl_pointer;
    struct wl_keyboard *wl_keyboard;

    struct
    {
        struct xkb_keymap *keymap;
        struct xkb_state *state;
    } xkb;
};

struct buffer_hdl
{
    void *base;
    int size;
    struct wl_buffer *wl_buffer;
};

struct buffer_allocator
{
    int shm_mem_fd;
    int shm_mem_size;
    int shm_file_free_size;
    struct wl_shm_pool *shm_pool;
};

struct graphic_object
{
    struct window *window;

    struct wl_surface *surface;
    struct wl_subsurface *subsurface;

    enum object_type type;
    int width;
    int height;

    struct buffer_hdl buffer;

    struct input input;
};

struct application
{
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct wl_seat *wl_seat;

    struct wl_cursor_theme *cursor_theme;
    struct wl_surface *cursor_surface;

#if LV_WAYLAND_WL_SHELL
    struct wl_shell *wl_shell;
#endif

#if LV_WAYLAND_XDG_SHELL
    struct xdg_wm_base *xdg_wm;
#endif

    const char *xdg_runtime_dir;

#ifdef LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    bool opt_disable_decorations;
#endif

    uint32_t format;

    struct xkb_context *xkb_context;

    struct seat seat;

    struct graphic_object *touch_obj;
    struct graphic_object *pointer_obj;
    struct graphic_object *keyboard_obj;

    lv_ll_t window_ll;
    lv_timer_t * cycle_timer;

    bool cursor_flush_pending;
};

struct window
{
    lv_disp_drv_t lv_disp_drv;
    lv_disp_draw_buf_t lv_disp_draw_buf;
    lv_disp_t *lv_disp;

    lv_indev_drv_t lv_indev_drv_pointer;
    lv_indev_t * lv_indev_pointer;

    lv_indev_drv_t lv_indev_drv_pointeraxis;
    lv_indev_t * lv_indev_pointeraxis;

    lv_indev_drv_t lv_indev_drv_touch;
    lv_indev_t * lv_indev_touch;

    lv_indev_drv_t lv_indev_drv_keyboard;
    lv_indev_t * lv_indev_keyboard;

    lv_wayland_display_close_f_t close_cb;

    struct application *application;

#if LV_WAYLAND_WL_SHELL
    struct wl_shell_surface *wl_shell_surface;
#endif

#if LV_WAYLAND_XDG_SHELL
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
#endif

    struct buffer_allocator allocator;

    struct graphic_object * body;

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    struct graphic_object * decoration[NUM_DECORATIONS];
#endif

    int width;
    int height;

    bool flush_pending;
    bool shall_close;
    bool closed;
    bool maximized;
};

/*********************************
 *   STATIC VARIABLES and FUNTIONS
 *********************************/

static bool resize_window(struct window *window, int width, int height);

static struct application application;

static inline bool _is_digit(char ch)
{
    return (ch >= '0') && (ch <= '9');
}

static unsigned int _atoi(const char ** str)
{
    unsigned int i = 0U;
    while (_is_digit(**str))
    {
        i = i * 10U + (unsigned int)(*((*str)++) - '0');
    }
    return i;
}

static void shm_format(void *data, struct wl_shm *wl_shm, uint32_t format)
{
    struct application *app = data;

    switch (format)
    {
#if (LV_COLOR_DEPTH == 32)
    case WL_SHM_FORMAT_ARGB8888:
        app->format = format;
        break;
    case WL_SHM_FORMAT_XRGB8888:
        if (app->format != WL_SHM_FORMAT_ARGB8888)
        {
            app->format = format;
        }
        break;
#elif (LV_COLOR_DEPTH == 16)
    case WL_SHM_FORMAT_RGB565:
        app->format = format;
        break;
#elif (LV_COLOR_DEPTH == 8)
    case WL_SHM_FORMAT_RGB332:
        app->format = format;
        break;
#elif (LV_COLOR_DEPTH == 1)
    case WL_SHM_FORMAT_RGB332:
        app->format = format;
        break;
#endif
    default:
        break;
    }
}

static const struct wl_shm_listener shm_listener = {
    shm_format
};

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct application *app = data;
    const char * cursor = "left_ptr";
    int pos_x = wl_fixed_to_int(sx);
    int pos_y = wl_fixed_to_int(sy);

    if (!surface)
    {
        app->pointer_obj = NULL;
        return;
    }

    app->pointer_obj = wl_surface_get_user_data(surface);

    app->pointer_obj->input.pointer.x = pos_x;
    app->pointer_obj->input.pointer.y = pos_y;

#if (LV_WAYLAND_CLIENT_SIDE_DECORATIONS && LV_WAYLAND_XDG_SHELL)
    if (!app->pointer_obj->window->xdg_toplevel || app->opt_disable_decorations)
    {
        return;
    }

    struct window *window = app->pointer_obj->window;

    switch (app->pointer_obj->type)
    {
    case OBJECT_BORDER_TOP:
        if (window->maximized)
        {
            // do nothing
        }
        else if (pos_x < (BORDER_SIZE * 5))
        {
            cursor = "top_left_corner";
        }
        else if (pos_x >= (window->width + BORDER_SIZE - (BORDER_SIZE * 5)))
        {
            cursor = "top_right_corner";
        }
        else
        {
            cursor = "top_side";
        }
        break;
    case OBJECT_BORDER_BOTTOM:
        if (window->maximized)
        {
            // do nothing
        }
        else if (pos_x < (BORDER_SIZE * 5))
        {
            cursor = "bottom_left_corner";
        }
        else if (pos_x >= (window->width + BORDER_SIZE - (BORDER_SIZE * 5)))
        {
            cursor = "bottom_right_corner";
        }
        else
        {
            cursor = "bottom_side";
        }
        break;
    case OBJECT_BORDER_LEFT:
        if (window->maximized)
        {
            // do nothing
        }
        else if (pos_y < (BORDER_SIZE * 5))
        {
            cursor = "top_left_corner";
        }
        else if (pos_y >= (window->height + BORDER_SIZE - (BORDER_SIZE * 5)))
        {
            cursor = "bottom_left_corner";
        }
        else
        {
            cursor = "left_side";
        }
        break;
    case OBJECT_BORDER_RIGHT:
        if (window->maximized)
        {
            // do nothing
        }
        else if (pos_y < (BORDER_SIZE * 5))
        {
            cursor = "top_right_corner";
        }
        else if (pos_y >= (window->height + BORDER_SIZE - (BORDER_SIZE * 5)))
        {
            cursor = "bottom_right_corner";
        }
        else
        {
            cursor = "right_side";
        }
        break;
    default:
        break;
    }
#endif

    if (app->cursor_surface)
    {
        struct wl_cursor_image *cursor_image = wl_cursor_theme_get_cursor(app->cursor_theme, cursor)->images[0];
        wl_pointer_set_cursor(pointer, serial, app->cursor_surface, cursor_image->hotspot_x, cursor_image->hotspot_y);
        wl_surface_attach(app->cursor_surface, wl_cursor_image_get_buffer(cursor_image), 0, 0);
        wl_surface_damage(app->cursor_surface, 0, 0, cursor_image->width, cursor_image->height);
        wl_surface_commit(app->cursor_surface);
        app->cursor_flush_pending = true;
    }
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface)
{
    struct application *app = data;

    if (!surface || (app->pointer_obj == wl_surface_get_user_data(surface)))
    {
        app->pointer_obj = NULL;
    }
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct application *app = data;
    int max_x, max_y;

    if (!app->pointer_obj)
    {
        return;
    }

    app->pointer_obj->input.pointer.x = LV_MAX(0, LV_MIN(wl_fixed_to_int(sx), app->pointer_obj->width - 1));
    app->pointer_obj->input.pointer.y = LV_MAX(0, LV_MIN(wl_fixed_to_int(sy), app->pointer_obj->height - 1));
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    struct application *app = data;
    const lv_indev_state_t lv_state =
        (state == WL_POINTER_BUTTON_STATE_PRESSED) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;

    if (!app->pointer_obj)
    {
        return;
    }

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    struct window *window = app->pointer_obj->window;
    int pos_x = app->pointer_obj->input.pointer.x;
    int pos_y = app->pointer_obj->input.pointer.y;
#endif

    switch (app->pointer_obj->type)
    {
    case OBJECT_WINDOW:
        switch (button)
        {
        case BTN_LEFT:
            app->pointer_obj->input.pointer.left_button = lv_state;
            break;
        case BTN_RIGHT:
            app->pointer_obj->input.pointer.right_button = lv_state;
            break;
        case BTN_MIDDLE:
            app->pointer_obj->input.pointer.wheel_button = lv_state;
            break;
        default:
            break;
        }
        break;
#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    case OBJECT_TITLEBAR:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_PRESSED))
        {
#if LV_WAYLAND_XDG_SHELL
            if (window->xdg_toplevel)
            {
                xdg_toplevel_move(window->xdg_toplevel, app->wl_seat, serial);
                window->flush_pending = true;
            }
#endif
#if LV_WAYLAND_WL_SHELL
            if (window->wl_shell_surface)
            {
                wl_shell_surface_move(window->wl_shell_surface, app->wl_seat, serial);
                window->flush_pending = true;
            }
#endif
        }
        break;
    case OBJECT_BUTTON_CLOSE:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_RELEASED))
        {
            window->shall_close = true;
        }
        break;
#if LV_WAYLAND_XDG_SHELL
    case OBJECT_BUTTON_MAXIMIZE:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_RELEASED))
        {
            if (window->xdg_toplevel)
            {
                if (window->maximized)
                {
                    xdg_toplevel_unset_maximized(window->xdg_toplevel);
                }
                else
                {
                    xdg_toplevel_set_maximized(window->xdg_toplevel);
                }
                window->maximized ^= true;
            }
        }
        break;
    case OBJECT_BUTTON_MINIMIZE:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_RELEASED))
        {
            if (window->xdg_toplevel)
            {
                xdg_toplevel_set_minimized(window->xdg_toplevel);
                window->flush_pending = true;
            }
        }
        break;
    case OBJECT_BORDER_TOP:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_PRESSED))
        {
            if (window->xdg_toplevel && !window->maximized)
            {
                uint32_t edge;
                if (pos_x < (BORDER_SIZE * 5))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
                }
                else if (pos_x >= (window->width + BORDER_SIZE - (BORDER_SIZE * 5)))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
                }
                else
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP;
                }
                xdg_toplevel_resize(window->xdg_toplevel,
                                    window->application->wl_seat, serial, edge);
                window->flush_pending = true;
            }
        }
        break;
    case OBJECT_BORDER_BOTTOM:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_PRESSED))
        {
            if (window->xdg_toplevel && !window->maximized)
            {
                uint32_t edge;
                if (pos_x < (BORDER_SIZE * 5))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
                }
                else if (pos_x >= (window->width + BORDER_SIZE - (BORDER_SIZE * 5)))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
                }
                else
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
                }
                xdg_toplevel_resize(window->xdg_toplevel,
                                    window->application->wl_seat, serial, edge);
                window->flush_pending = true;
            }
        }
        break;
    case OBJECT_BORDER_LEFT:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_PRESSED))
        {
            if (window->xdg_toplevel && !window->maximized)
            {
                uint32_t edge;
                if (pos_y < (BORDER_SIZE * 5))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
                }
                else if (pos_y >= (window->height + BORDER_SIZE - (BORDER_SIZE * 5)))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
                }
                else
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
                }
                xdg_toplevel_resize(window->xdg_toplevel,
                                    window->application->wl_seat, serial, edge);
                window->flush_pending = true;
            }
        }
        break;
    case OBJECT_BORDER_RIGHT:
        if ((button == BTN_LEFT) && (state == WL_POINTER_BUTTON_STATE_PRESSED))
        {
            if (window->xdg_toplevel && !window->maximized)
            {
                uint32_t edge;
                if (pos_y < (BORDER_SIZE * 5))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
                }
                else if (pos_y >= (window->height + BORDER_SIZE - (BORDER_SIZE * 5)))
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
                }
                else
                {
                    edge = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
                }
                xdg_toplevel_resize(window->xdg_toplevel,
                                    window->application->wl_seat, serial, edge);
                window->flush_pending = true;
            }
        }
        break;
#endif // LV_WAYLAND_XDG_SHELL
#endif // LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    default:
        break;
    }
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct application *app = data;
    const int diff = wl_fixed_to_int(value);

    if (!app->pointer_obj)
    {
        return;
    }

    if (axis == 0)
    {
        if (diff > 0)
        {
            app->pointer_obj->input.pointer.wheel_diff++;
        }
        else if (diff < 0)
        {
            app->pointer_obj->input.pointer.wheel_diff--;
        }
    }
}

static const struct wl_pointer_listener pointer_listener = {
    .enter  = pointer_handle_enter,
    .leave  = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis   = pointer_handle_axis,
};

static lv_key_t keycode_xkb_to_lv(xkb_keysym_t xkb_key)
{
    lv_key_t key = 0;

    if (((xkb_key >= XKB_KEY_space) && (xkb_key <= XKB_KEY_asciitilde)))
    {
        key = xkb_key;
    }
    else if (((xkb_key >= XKB_KEY_KP_0) && (xkb_key <= XKB_KEY_KP_9)))
    {
        key = (xkb_key & 0x003f);
    }
    else
    {
        switch (xkb_key)
        {
        case XKB_KEY_BackSpace:
            key = LV_KEY_BACKSPACE;
            break;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            key = LV_KEY_ENTER;
            break;
        case XKB_KEY_Escape:
            key = LV_KEY_ESC;
            break;
        case XKB_KEY_Delete:
        case XKB_KEY_KP_Delete:
            key = LV_KEY_DEL;
            break;
        case XKB_KEY_Home:
        case XKB_KEY_KP_Home:
            key = LV_KEY_HOME;
            break;
        case XKB_KEY_Left:
        case XKB_KEY_KP_Left:
            key = LV_KEY_LEFT;
            break;
        case XKB_KEY_Up:
        case XKB_KEY_KP_Up:
            key = LV_KEY_UP;
            break;
        case XKB_KEY_Right:
        case XKB_KEY_KP_Right:
            key = LV_KEY_RIGHT;
            break;
        case XKB_KEY_Down:
        case XKB_KEY_KP_Down:
            key = LV_KEY_DOWN;
            break;
        case XKB_KEY_Prior:
        case XKB_KEY_KP_Prior:
            key = LV_KEY_PREV;
            break;
        case XKB_KEY_Next:
        case XKB_KEY_KP_Next:
        case XKB_KEY_Tab:
        case XKB_KEY_KP_Tab:
            key = LV_KEY_NEXT;
            break;
        case XKB_KEY_End:
        case XKB_KEY_KP_End:
            key = LV_KEY_END;
            break;
        default:
            break;
        }
    }

    return key;
}

static void keyboard_handle_keymap(void *data, struct wl_keyboard *keyboard,
                                   uint32_t format, int fd, uint32_t size)
{
    struct application *app = data;

    struct xkb_keymap *keymap;
    struct xkb_state *state;
    char *map_str;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
    {
        close(fd);
        return;
    }

    map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED)
    {
        close(fd);
        return;
    }

    /* Set up XKB keymap */
    keymap = xkb_keymap_new_from_string(app->xkb_context, map_str,
                                        XKB_KEYMAP_FORMAT_TEXT_V1, 0);
    munmap(map_str, size);
    close(fd);

    if (!keymap)
    {
        LV_LOG_ERROR("failed to compile keymap\n");
        return;
    }

    /* Set up XKB state */
    state = xkb_state_new(keymap);
    if (!state)
    {
        LV_LOG_ERROR("failed to create XKB state\n");
        xkb_keymap_unref(keymap);
        return;
    }

    xkb_keymap_unref(app->seat.xkb.keymap);
    xkb_state_unref(app->seat.xkb.state);
    app->seat.xkb.keymap = keymap;
    app->seat.xkb.state = state;
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys)
{
    struct application *app = data;

    if (!surface)
    {
        app->keyboard_obj = NULL;
    }
    else
    {
        app->keyboard_obj = wl_surface_get_user_data(surface);
    }
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *keyboard,
                                  uint32_t serial, struct wl_surface *surface)
{
    struct application *app = data;

    if (!surface || (app->keyboard_obj == wl_surface_get_user_data(surface)))
    {
        app->keyboard_obj = NULL;
    }
}

static void keyboard_handle_key(void *data, struct wl_keyboard *keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    struct application *app = data;
    const uint32_t code = (key + 8);
    const xkb_keysym_t *syms;
    xkb_keysym_t sym = XKB_KEY_NoSymbol;

    if (!app->keyboard_obj || !app->seat.xkb.state)
    {
        return;
    }

    if (xkb_state_key_get_syms(app->seat.xkb.state, code, &syms) == 1)
    {
        sym = syms[0];
    }

    const lv_key_t lv_key = keycode_xkb_to_lv(sym);
    const lv_indev_state_t lv_state =
        (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;

    if (lv_key != 0)
    {
        app->keyboard_obj->input.keyboard.key = lv_key;
        app->keyboard_obj->input.keyboard.state = lv_state;
    }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group)
{
    struct application *app = data;

    /* If we're not using a keymap, then we don't handle PC-style modifiers */
    if (!app->seat.xkb.keymap)
    {
        return;
    }

    xkb_state_update_mask(app->seat.xkb.state,
                          mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap     = keyboard_handle_keymap,
    .enter      = keyboard_handle_enter,
    .leave      = keyboard_handle_leave,
    .key        = keyboard_handle_key,
    .modifiers  = keyboard_handle_modifiers,
};

static void touch_handle_down(void *data, struct wl_touch *wl_touch,
                              uint32_t serial, uint32_t time, struct wl_surface *surface,
                              int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct application *app = data;

    if (!surface)
    {
        app->touch_obj = NULL;
        return;
    }

    app->touch_obj = wl_surface_get_user_data(surface);

    app->touch_obj->input.touch.x = wl_fixed_to_int(x_w);
    app->touch_obj->input.touch.y = wl_fixed_to_int(y_w);
    app->touch_obj->input.touch.state = LV_INDEV_STATE_PR;

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    struct window *window = app->touch_obj->window;
    switch (app->touch_obj->type)
    {
    case OBJECT_TITLEBAR:
#if LV_WAYLAND_XDG_SHELL
        if (window->xdg_toplevel)
        {
            xdg_toplevel_move(window->xdg_toplevel, app->wl_seat, serial);
            window->flush_pending = true;
        }
#endif
#if LV_WAYLAND_WL_SHELL
        if (window->wl_shell_surface)
        {
            wl_shell_surface_move(window->wl_shell_surface, app->wl_seat, serial);
            window->flush_pending = true;
        }
#endif
        break;
    default:
        break;
    }
#endif
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch,
                            uint32_t serial, uint32_t time, int32_t id)
{
    struct application *app = data;

    if (!app->touch_obj)
    {
        return;
    }

    app->touch_obj->input.touch.state = LV_INDEV_STATE_REL;

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    struct window *window = app->touch_obj->window;
    switch (app->touch_obj->type)
    {
    case OBJECT_BUTTON_CLOSE:
        window->shall_close = true;
        break;
#if LV_WAYLAND_XDG_SHELL
    case OBJECT_BUTTON_MAXIMIZE:
        if (window->xdg_toplevel)
        {
            if (window->maximized)
            {
                xdg_toplevel_unset_maximized(window->xdg_toplevel);
            }
            else
            {
                xdg_toplevel_set_maximized(window->xdg_toplevel);
            }
            window->maximized ^= true;
        }
        break;
    case OBJECT_BUTTON_MINIMIZE:
        if (window->xdg_toplevel)
        {
            xdg_toplevel_set_minimized(window->xdg_toplevel);
            window->flush_pending = true;
        }
#endif // LV_WAYLAND_XDG_SHELL
    default:
        break;
    }
#endif // LV_WAYLAND_CLIENT_SIDE_DECORATIONS

    app->touch_obj = NULL;
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
                                uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct application *app = data;

    if (!app->touch_obj)
    {
        return;
    }

    app->touch_obj->input.touch.x = wl_fixed_to_int(x_w);
    app->touch_obj->input.touch.y = wl_fixed_to_int(y_w);
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
}

static const struct wl_touch_listener touch_listener = {
    .down   = touch_handle_down,
    .up     = touch_handle_up,
    .motion = touch_handle_motion,
    .frame  = touch_handle_frame,
    .cancel = touch_handle_cancel,
};

static void seat_handle_capabilities(void *data, struct wl_seat *wl_seat, enum wl_seat_capability caps)
{
    struct application *app = data;
    struct seat *seat = &app->seat;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !seat->wl_pointer)
    {
        seat->wl_pointer = wl_seat_get_pointer(wl_seat);
        wl_pointer_add_listener(seat->wl_pointer, &pointer_listener, app);
        app->cursor_surface = wl_compositor_create_surface(app->compositor);
        if (!app->cursor_surface)
        {
            LV_LOG_WARN("failed to create cursor surface");
        }
    }
    else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && seat->wl_pointer)
    {
        wl_pointer_destroy(seat->wl_pointer);
        if (app->cursor_surface)
        {
            wl_surface_destroy(app->cursor_surface);
        }
        seat->wl_pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !seat->wl_keyboard)
    {
        seat->wl_keyboard = wl_seat_get_keyboard(wl_seat);
        wl_keyboard_add_listener(seat->wl_keyboard, &keyboard_listener, app);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && seat->wl_keyboard)
    {
        wl_keyboard_destroy(seat->wl_keyboard);
        seat->wl_keyboard = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !seat->wl_touch)
    {
        seat->wl_touch = wl_seat_get_touch(wl_seat);
        wl_touch_add_listener(seat->wl_touch, &touch_listener, app);
    }
    else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && seat->wl_touch)
    {
        wl_touch_destroy(seat->wl_touch);
        seat->wl_touch = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_handle_capabilities,
};

#if LV_WAYLAND_WL_SHELL
static void wl_shell_handle_ping(void *data, struct wl_shell_surface *shell_surface, uint32_t serial)
{
    return wl_shell_surface_pong(shell_surface, serial);
}

static void wl_shell_handle_configure(void *data, struct wl_shell_surface *shell_surface,
                                      uint32_t edges, int32_t width, int32_t height)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
    .ping       = wl_shell_handle_ping,
    .configure  =  wl_shell_handle_configure,
};
#endif

#if LV_WAYLAND_XDG_SHELL
static void xdg_surface_handle_configure(void *data, struct xdg_surface *xdg_surface, uint32_t serial)
{
    return xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_handle_configure,
};

void xdg_toplevel_handle_configure(void *data, struct xdg_toplevel *xdg_toplevel,
                                   int32_t width, int32_t height, struct wl_array *states)
{
    struct window *window = (struct window *)data;

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    if (!window->application->opt_disable_decorations)
    {
        width -= (2 * BORDER_SIZE);
        height -= (TITLE_BAR_HEIGHT + (2 * BORDER_SIZE));
    }
#endif

    if ((width <= 0) || (height <= 0))
    {
        return;
    }

    if ((width != window->width) || (height != window->height))
    {
        resize_window(window, width, height);
    }
}

static void xdg_toplevel_handle_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    struct window *window = (struct window *)data;
    window->shall_close = true;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_handle_configure,
    .close = xdg_toplevel_handle_close,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    return xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping
};
#endif

static void handle_global(void *data, struct wl_registry *registry,
                          uint32_t name, const char *interface, uint32_t version)
{
    struct application *app = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0)
    {
        app->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 1);
    }
    else if (strcmp(interface, wl_subcompositor_interface.name) == 0)
    {
        app->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    }
    else if (strcmp(interface, wl_shm_interface.name) == 0)
    {
        app->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(app->shm, &shm_listener, app);
        app->cursor_theme = wl_cursor_theme_load(NULL, 32, app->shm);
    }
    else if (strcmp(interface, wl_seat_interface.name) == 0)
    {
        app->wl_seat = wl_registry_bind(app->registry, name, &wl_seat_interface, 1);
        wl_seat_add_listener(app->wl_seat, &seat_listener, app);
    }
#if LV_WAYLAND_WL_SHELL
    else if (strcmp(interface, wl_shell_interface.name) == 0)
    {
        app->wl_shell = wl_registry_bind(registry, name, &wl_shell_interface, 1);
    }
#endif
#if LV_WAYLAND_XDG_SHELL
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0)
    {
        app->xdg_wm = wl_registry_bind(app->registry, name, &xdg_wm_base_interface, version);
        xdg_wm_base_add_listener(app->xdg_wm, &xdg_wm_base_listener, app);
    }
#endif
}

static void handle_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{

}

static const struct wl_registry_listener registry_listener = {
    .global         = handle_global,
    .global_remove  = handle_global_remove
};

static bool initialize_allocator(struct buffer_allocator *allocator, const char *dir)
{
    static const char template[] = "/lvgl-wayland-XXXXXX";
    char *name;

    // Create file for shared memory allocation
    name = lv_mem_alloc(strlen(dir) + sizeof(template));
    LV_ASSERT_MSG(name, "cannot allocate memory for name\n");
    if (!name)
    {
        return false;
    }

    strcpy(name, dir);
    strcat(name, template);

    allocator->shm_mem_fd = mkstemp(name);

    lv_mem_free(name);

    LV_ASSERT_MSG((allocator->shm_mem_fd >= 0), "cannot create tmpfile\n");
    if (allocator->shm_mem_fd < 0)
    {
        return false;
    }

    allocator->shm_mem_size = 0;
    allocator->shm_file_free_size = 0;

    return true;
}

static void deinitialize_allocator(struct buffer_allocator *allocator)
{
    if (allocator->shm_pool)
    {
        wl_shm_pool_destroy(allocator->shm_pool);
    }

    if (allocator->shm_mem_fd >= 0)
    {
        close(allocator->shm_mem_fd);
        allocator->shm_mem_fd = -1;
    }
}

static bool initialize_buffer(struct window *window, struct buffer_hdl *buffer_hdl,
                              int width, int height)
{
    struct application *app = window->application;
    struct buffer_allocator *allocator = &window->allocator;
    int allocated_size = 0;
    int ret;
    long sz = sysconf(_SC_PAGESIZE);

    buffer_hdl->size = (((width * height * BYTES_PER_PIXEL) + sz - 1) / sz) * sz;

    LV_LOG_TRACE("initializing buffer %dx%d (alloc size: %d)\n",
                 width, height, buffer_hdl->size);

    if (allocator->shm_file_free_size < buffer_hdl->size)
    {
        do
        {
            ret = ftruncate(allocator->shm_mem_fd,
                            allocator->shm_mem_size + (buffer_hdl->size - allocator->shm_file_free_size));
        }
        while ((ret < 0) && (errno == EINTR));

        if (ret < 0)
        {
            LV_LOG_ERROR("ftruncate failed: %s\n", strerror(errno));
            goto err_out;
        }
        else
        {
            allocated_size = (buffer_hdl->size - allocator->shm_file_free_size);
        }

        LV_ASSERT_MSG((allocated_size >= 0), "allocated_size is negative");
    }

    buffer_hdl->base = mmap(NULL, buffer_hdl->size,
                            PROT_READ | PROT_WRITE, MAP_SHARED,
                            allocator->shm_mem_fd,
                            allocator->shm_mem_size - allocator->shm_file_free_size);
    if (buffer_hdl->base == MAP_FAILED)
    {
        LV_LOG_ERROR("mmap failed: %s\n", strerror(errno));
        goto err_inc_free;
    }

    if (!allocator->shm_pool)
    {
        // Create SHM pool
        allocator->shm_pool = wl_shm_create_pool(app->shm,
                                                 allocator->shm_mem_fd,
                                                 allocator->shm_mem_size + allocated_size);
        if (!allocator->shm_pool)
        {
            LV_LOG_ERROR("cannot create shm pool\n");
            goto err_unmap;
        }
    }
    else if (allocated_size > 0)
    {
        // Resize SHM pool
        wl_shm_pool_resize(allocator->shm_pool,
                           allocator->shm_mem_size + allocated_size);
    }

    // Create buffer
    buffer_hdl->wl_buffer = wl_shm_pool_create_buffer(allocator->shm_pool,
                                                      allocator->shm_mem_size - allocator->shm_file_free_size,
                                                      width, height,
                                                      width * BYTES_PER_PIXEL,
                                                      app->format);
    if (!buffer_hdl->wl_buffer)
    {
        LV_LOG_ERROR("cannot create shm buffer\n");
        goto err_unmap;
    }

    /* Update size of SHM */
    allocator->shm_mem_size += allocated_size;
    allocator->shm_file_free_size = LV_MAX(0, (allocator->shm_file_free_size - buffer_hdl->size));

    lv_memset_00(buffer_hdl->base, buffer_hdl->size);

    return true;

err_unmap:
    munmap(buffer_hdl->base, buffer_hdl->size);

err_inc_free:
    allocator->shm_file_free_size += allocated_size;

err_out:
    return false;
}

static bool deinitialize_buffer(struct window *window, struct buffer_hdl *buffer_hdl)
{
    struct buffer_allocator *allocator = &window->allocator;

    if (buffer_hdl->wl_buffer)
    {
        wl_buffer_destroy(buffer_hdl->wl_buffer);
        buffer_hdl->wl_buffer = NULL;
    }

    if (buffer_hdl->size > 0)
    {
        munmap(buffer_hdl->base, buffer_hdl->size);
        allocator->shm_file_free_size += buffer_hdl->size;
        buffer_hdl->base = 0;
        buffer_hdl->size = 0;
    }
}

static struct graphic_object * create_graphic_obj(struct application *app, struct window *window,
                                                  enum object_type type,
                                                  struct graphic_object *parent)
{
    struct graphic_object *obj;

    obj = lv_mem_alloc(sizeof(*obj));
    LV_ASSERT_MALLOC(obj);
    if (!obj)
    {
        return NULL;
    }

    lv_memset(obj, 0x00, sizeof(struct graphic_object));

    obj->window = window;
    obj->type = type;

    obj->surface = wl_compositor_create_surface(app->compositor);
    if (!obj->surface)
    {
        LV_LOG_ERROR("cannot create surface for graphic object");
        goto err_out;
    }

    wl_surface_set_user_data(obj->surface, obj);

    if (parent != NULL)
    {
        obj->subsurface = wl_subcompositor_get_subsurface(app->subcompositor,
                                                          obj->surface,
                                                          parent->surface);
        if (!obj->subsurface)
        {
            LV_LOG_ERROR("cannot get subsurface for graphic object");
            goto err_destroy_surface;
        }

        wl_subsurface_set_desync(obj->subsurface);
    }

    return obj;

err_destroy_surface:
    wl_surface_destroy(obj->surface);

err_free:
    lv_mem_free(obj);

err_out:
    return NULL;
}

static void destroy_graphic_obj(struct graphic_object * obj)
{
    wl_surface_destroy(obj->surface);

    lv_mem_free(obj);
}

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
static bool create_and_attach_decoration(struct window *window,
                                         struct graphic_object * decoration)
{
    int pos_x, pos_y;
    int x, y;

    switch (decoration->type)
    {
    case OBJECT_TITLEBAR:
        decoration->width = window->width;
        decoration->height = TITLE_BAR_HEIGHT;
        pos_x = 0;
        pos_y = -TITLE_BAR_HEIGHT;
        break;
    case OBJECT_BUTTON_CLOSE:
        decoration->width = BUTTON_SIZE;
        decoration->height = BUTTON_SIZE;
        pos_x = window->width - 1 * (BUTTON_MARGIN + BUTTON_SIZE);
        pos_y = -1 * (BUTTON_MARGIN + BUTTON_SIZE + (BORDER_SIZE / 2));
        break;
#if LV_WAYLAND_XDG_SHELL
    case OBJECT_BUTTON_MAXIMIZE:
        decoration->width = BUTTON_SIZE;
        decoration->height = BUTTON_SIZE;
        pos_x = window->width - 2 * (BUTTON_MARGIN + BUTTON_SIZE);
        pos_y = -1 * (BUTTON_MARGIN + BUTTON_SIZE + (BORDER_SIZE / 2));
        break;
    case OBJECT_BUTTON_MINIMIZE:
        decoration->width = BUTTON_SIZE;
        decoration->height = BUTTON_SIZE;
        pos_x = window->width - 3 * (BUTTON_MARGIN + BUTTON_SIZE);
        pos_y = -1 * (BUTTON_MARGIN + BUTTON_SIZE + (BORDER_SIZE / 2));
        break;
#endif
    case OBJECT_BORDER_TOP:
        decoration->width = window->width + 2 * (BORDER_SIZE);
        decoration->height = BORDER_SIZE;
        pos_x = -BORDER_SIZE;
        pos_y = -(BORDER_SIZE + TITLE_BAR_HEIGHT);
        break;
    case OBJECT_BORDER_BOTTOM:
        decoration->width = window->width + 2 * (BORDER_SIZE);
        decoration->height = BORDER_SIZE;
        pos_x = -BORDER_SIZE;
        pos_y = window->height;
        break;
    case OBJECT_BORDER_LEFT:
        decoration->width = BORDER_SIZE;
        decoration->height = window->height + TITLE_BAR_HEIGHT;
        pos_x = -BORDER_SIZE;
        pos_y = -TITLE_BAR_HEIGHT;
        break;
    case OBJECT_BORDER_RIGHT:
        decoration->width = BORDER_SIZE;
        decoration->height = window->height + TITLE_BAR_HEIGHT;
        pos_x = window->width;
        pos_y = -TITLE_BAR_HEIGHT;
        break;
    default:
        LV_ASSERT_MSG(0, "Invalid object type");
        return false;
    }

    if (!initialize_buffer(window, &decoration->buffer, decoration->width, decoration->height))
    {
        LV_LOG_ERROR("cannot create buffer for decoration");
        return false;
    }

    switch (decoration->type)
    {
    case OBJECT_TITLEBAR:
        lv_color_fill((lv_color_t *)decoration->buffer.base,
                      lv_color_make(0x66, 0x66, 0x66), (decoration->width * decoration->height));
        break;
    case OBJECT_BUTTON_CLOSE:
        lv_color_fill((lv_color_t *)decoration->buffer.base,
                      lv_color_make(0xCC, 0xCC, 0xCC), (decoration->width * decoration->height));
        for (y = 0; y < decoration->height; y++)
        {
            for (x = 0; x < decoration->width; x++)
            {
                lv_color_t *pixel = ((lv_color_t *)decoration->buffer.base + (y * decoration->width) + x);
                if ((x >= BUTTON_PADDING) && (x < decoration->width - BUTTON_PADDING))
                {
                    if ((x == y) || (x == decoration->width - 1 - y))
                    {
                        *pixel = lv_color_make(0x33, 0x33, 0x33);
                    }
                    else if ((x == y - 1) || (x == decoration->width - y))
                    {
                        *pixel = lv_color_make(0x66, 0x66, 0x66);
                    }
                }
            }
        }
        break;
#if LV_WAYLAND_XDG_SHELL
    case OBJECT_BUTTON_MAXIMIZE:
        lv_color_fill((lv_color_t *)decoration->buffer.base,
                      lv_color_make(0xCC, 0xCC, 0xCC), (decoration->width * decoration->height));
        for (y = 0; y < decoration->height; y++)
        {
            for (x = 0; x < decoration->width; x++)
            {
                lv_color_t *pixel = ((lv_color_t *)decoration->buffer.base + (y * decoration->width) + x);
                if (((x == BUTTON_PADDING) && (y >= BUTTON_PADDING) && (y < decoration->height - BUTTON_PADDING)) ||
                    ((x == (decoration->width - BUTTON_PADDING)) && (y >= BUTTON_PADDING) && (y <= decoration->height - BUTTON_PADDING)) ||
                    ((y == BUTTON_PADDING) && (x >= BUTTON_PADDING) && (x < decoration->width - BUTTON_PADDING)) ||
                    ((y == (BUTTON_PADDING + 1)) && (x >= BUTTON_PADDING) && (x < decoration->width - BUTTON_PADDING)) ||
                    ((y == (decoration->height - BUTTON_PADDING)) && (x >= BUTTON_PADDING) && (x < decoration->width - BUTTON_PADDING)))
                {
                    *pixel = lv_color_make(0x33, 0x33, 0x33);
                }
            }
        }
        break;
    case OBJECT_BUTTON_MINIMIZE:
        lv_color_fill((lv_color_t *)decoration->buffer.base,
                      lv_color_make(0xCC, 0xCC, 0xCC), (decoration->width * decoration->height));
        for (y = 0; y < decoration->height; y++)
        {
            for (x = 0; x < decoration->width; x++)
            {
                lv_color_t *pixel = ((lv_color_t *)decoration->buffer.base + (y * decoration->width) + x);
                if ((x >= BUTTON_PADDING) && (x < decoration->width - BUTTON_PADDING) &&
                    (y > decoration->height - (2 * BUTTON_PADDING)) && (y < decoration->height - BUTTON_PADDING))
                {
                    *pixel = lv_color_make(0x33, 0x33, 0x33);
                }
            }
        }
        break;
#endif
    case OBJECT_BORDER_TOP:
        /* fallthrough */
    case OBJECT_BORDER_BOTTOM:
        /* fallthrough */
    case OBJECT_BORDER_LEFT:
        /* fallthrough */
    case OBJECT_BORDER_RIGHT:
        lv_color_fill((lv_color_t *)decoration->buffer.base,
                      lv_color_make(0x66, 0x66, 0x66), (decoration->width * decoration->height));
        break;
    default:
        LV_ASSERT_MSG(0, "Invalid object type");
        return false;
    }

    wl_surface_attach(decoration->surface, decoration->buffer.wl_buffer, 0, 0);
    wl_surface_commit(decoration->surface);

    wl_subsurface_set_position(decoration->subsurface, pos_x, pos_y);

    return true;
}
#endif

static bool resize_window(struct window *window, int width, int height)
{
    LV_LOG_TRACE("resize window %dx%d\n", width, height);

    // De-initialize previous buffers
#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    int b;
    for (b = 0; b < NUM_DECORATIONS; b++)
    {
        if (window->decoration[b])
        {
            deinitialize_buffer(window, &window->decoration[b]->buffer);
        }
    }
#endif
    deinitialize_buffer(window, &window->body->buffer);

    // Initialize backing buffer
    if (!initialize_buffer(window, &window->body->buffer, width, height))
    {
        LV_LOG_ERROR("failed to initialize window buffer\n");
        return false;
    }

    window->width = width;
    window->height = height;

    window->body->width = width;
    window->body->height = height;

    wl_surface_attach(window->body->surface, window->body->buffer.wl_buffer, 0, 0);

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    if (!window->application->opt_disable_decorations)
    {
        for (b = 0; b < NUM_DECORATIONS; b++)
        {
            if (!create_and_attach_decoration(window, window->decoration[b]))
            {
                LV_LOG_ERROR("failed to create decoration %d\n", b);
            }
        }
    }
#endif

    if (window->lv_disp != NULL)
    {
        // Propagate resize to upper layers
        window->lv_disp_drv.hor_res = width;
        window->lv_disp_drv.ver_res = height;
        lv_disp_drv_update(window->lv_disp, &window->lv_disp_drv);

        window->body->input.pointer.x = LV_MIN(window->body->input.pointer.x, (width - 1));
        window->body->input.pointer.y = LV_MIN(window->body->input.pointer.y, (height - 1));
    }

    return true;
}

static struct window *create_window(struct application *app, int width, int height, const char *title)
{
    struct window *window;

    window = _lv_ll_ins_tail(&app->window_ll);
    LV_ASSERT_MALLOC(window);
    if (!window)
    {
        return NULL;
    }

    lv_memset(window, 0x00, sizeof(struct window));

    window->application = app;

    // Initialize buffer allocator
    if (!initialize_allocator(&window->allocator, app->xdg_runtime_dir))
    {
        LV_LOG_ERROR("cannot init memory allocator");
        goto err_free_window;
    }

    // Create wayland buffer and surface
    window->body = create_graphic_obj(app, window, OBJECT_WINDOW, NULL);
    if (!window->body)
    {
        LV_LOG_ERROR("cannot create window body");
        goto err_deinit_allocator;
    }

    // Create shell surface
     if (0)
    {
        // Needed for #if madness below
    }
#if LV_WAYLAND_XDG_SHELL
    else if (app->xdg_wm)
    {
        window->xdg_surface = xdg_wm_base_get_xdg_surface(app->xdg_wm, window->body->surface);
        if (!window->xdg_surface)
        {
            LV_LOG_ERROR("cannot create XDG surface");
            goto err_destroy_surface;
        }

        xdg_surface_add_listener(window->xdg_surface, &xdg_surface_listener, window);

        window->xdg_toplevel = xdg_surface_get_toplevel(window->xdg_surface);
        if (!window->xdg_toplevel)
        {
            LV_LOG_ERROR("cannot get XDG toplevel surface");
            goto err_destroy_shell_surface;
        }

        xdg_toplevel_add_listener(window->xdg_toplevel, &xdg_toplevel_listener, window);
        xdg_toplevel_set_title(window->xdg_toplevel, title);
        xdg_toplevel_set_app_id(window->xdg_toplevel, title);
    }
#endif
#if LV_WAYLAND_WL_SHELL
    else if (app->wl_shell)
    {
        window->wl_shell_surface = wl_shell_get_shell_surface(app->wl_shell, window->body->surface);
        if (!window->wl_shell_surface)
        {
            LV_LOG_ERROR("cannot create WL shell surface");
            goto err_destroy_surface;
        }

        wl_shell_surface_add_listener(window->wl_shell_surface, &shell_surface_listener, &window);
        wl_shell_surface_set_toplevel(window->wl_shell_surface);
        wl_shell_surface_set_title(window->wl_shell_surface, title);
    }
#endif
    else
    {
        LV_LOG_ERROR("No shell available");
        goto err_destroy_surface;
    }

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    if (!app->opt_disable_decorations)
    {
        int d;
        for (d = 0; d < NUM_DECORATIONS; d++)
        {
            window->decoration[d] = create_graphic_obj(app, window, (FIRST_DECORATION+d), window->body);
            if (!window->decoration[d])
            {
                LV_LOG_ERROR("Failed to create decoration %d", d);
            }
        }
    }
#endif

    if (!resize_window(window, width, height))
    {
        LV_LOG_ERROR("Failed to resize window");
        goto err_destroy_shell_surface2;
    }

    return window;

err_destroy_shell_surface2:
#if LV_WAYLAND_XDG_SHELL
    if (window->xdg_toplevel)
    {
        xdg_toplevel_destroy(window->xdg_toplevel);
    }
#endif

err_destroy_shell_surface:
#if LV_WAYLAND_WL_SHELL
    if (window->wl_shell_surface)
    {
        wl_shell_surface_destroy(window->wl_shell_surface);
    }
#endif
#if LV_WAYLAND_XDG_SHELL
    if (window->xdg_surface)
    {
        xdg_surface_destroy(window->xdg_surface);
    }
#endif

err_destroy_surface:
    wl_surface_destroy(window->body->surface);

err_deinit_allocator:
    deinitialize_allocator(&window->allocator);

err_free_window:
    _lv_ll_remove(&app->window_ll, window);
    lv_mem_free(window);
    return NULL;
}

static void destroy_window(struct window *window)
{
    if (!window)
    {
        return;
    }

#if LV_WAYLAND_WL_SHELL
    if (window->wl_shell_surface)
    {
        wl_shell_surface_destroy(window->wl_shell_surface);
    }
#endif
#if LV_WAYLAND_XDG_SHELL
    if (window->xdg_toplevel)
    {
        xdg_toplevel_destroy(window->xdg_toplevel);
        xdg_surface_destroy(window->xdg_surface);
    }
#endif

#if LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    int b;
    for (b = 0; b < NUM_DECORATIONS; b++)
    {
        if (window->decoration[b])
        {
            deinitialize_buffer(window, &window->decoration[b]->buffer);
            destroy_graphic_obj(window->decoration[b]);
            window->decoration[b] = NULL;
        }
    }
#endif

    deinitialize_buffer(window, &window->body->buffer);
    destroy_graphic_obj(window->body);

    deinitialize_allocator(&window->allocator);
}

static void _lv_wayland_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p)
{
    struct window *window = disp_drv->user_data;
    lv_coord_t hres = (disp_drv->rotated == 0) ? (disp_drv->hor_res) : (disp_drv->ver_res);
    lv_coord_t vres = (disp_drv->rotated == 0) ? (disp_drv->ver_res) : (disp_drv->hor_res);

    /* If private data is not set, it means window has not been initialized */
    if (!window)
    {
        LV_LOG_ERROR("please intialize wayland display using lv_wayland_create_window()\n");
        return;
    }
    /* If window has been / is being closed, or is not visible, skip rendering */
    else if (window->closed || window->shall_close)
    {
        lv_disp_flush_ready(disp_drv);
        return;
    }
    /* Return if the area is out the screen */
    else if ((area->x2 < 0) || (area->y2 < 0) || (area->x1 > hres - 1) || (area->y1 > vres - 1))
    {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    int32_t x;
    int32_t y;

    for (y = area->y1; y <= area->y2 && y < disp_drv->ver_res; y++)
    {
        for (x = area->x1; x <= area->x2 && x < disp_drv->hor_res; x++)
        {
            int offset = (y * disp_drv->hor_res) + x;
#if (LV_COLOR_DEPTH == 32)
            uint32_t * const buf = (uint32_t *)window->body->buffer.base + offset;
            *buf = color_p->full;
#elif (LV_COLOR_DEPTH == 16)
            uint16_t * const buf = (uint16_t *)window->body->buffer.base + offset;
            *buf = color_p->full;
#elif (LV_COLOR_DEPTH == 8)
            uint8_t * const buf = (uint8_t *)window->body->buffer.base + offset;
            *buf = color_p->full;
#elif (LV_COLOR_DEPTH == 1)
            uint8_t * const buf = (uint8_t *)window->body->buffer.base + offset;
            *buf = ((0x07 * color_p->ch.red)   << 5) |
                   ((0x07 * color_p->ch.green) << 2) |
                   ((0x03 * color_p->ch.blue)  << 0);
#endif
            color_p++;
        }
    }

    wl_surface_damage(window->body->surface, area->x1, area->y1,
                      (area->x2 - area->x1 + 1), (area->y2 - area->y1 + 1));

    if (lv_disp_flush_is_last(disp_drv))
    {
        wl_surface_commit(window->body->surface);
        window->flush_pending = true;
    }

    lv_disp_flush_ready(disp_drv);
}

static void _lv_wayland_cycle(lv_timer_t * tmr)
{
    struct window *window = NULL;
    bool shall_flush = application.cursor_flush_pending;

    LV_UNUSED(tmr);

    while (wl_display_prepare_read(application.display) != 0)
    {
        wl_display_dispatch_pending(application.display);
    }

    _LV_LL_READ(&application.window_ll, window)
    {
        if ((window->shall_close) && (window->close_cb != NULL))
        {
            window->shall_close = window->close_cb(window->lv_disp);
        }

        if (window->shall_close)
        {
            destroy_window(window);
            window->closed = true;
            window->shall_close = false;
            shall_flush = true;

            window->body->input.touch.x = 0;
            window->body->input.touch.y = 0;
            window->body->input.touch.state = LV_INDEV_STATE_RELEASED;
            if (window->application->touch_obj == window->body)
            {
                window->application->touch_obj = NULL;
            }

            window->body->input.pointer.x = 0;
            window->body->input.pointer.y = 0;
            window->body->input.pointer.left_button = LV_INDEV_STATE_RELEASED;
            window->body->input.pointer.right_button = LV_INDEV_STATE_RELEASED;
            window->body->input.pointer.wheel_button = LV_INDEV_STATE_RELEASED;
            window->body->input.pointer.wheel_diff = 0;
            if (window->application->pointer_obj == window->body)
            {
                window->application->pointer_obj = NULL;
            }

            window->body->input.keyboard.key = 0;
            window->body->input.keyboard.state = LV_INDEV_STATE_RELEASED;
            if (window->application->keyboard_obj == window->body)
            {
                window->application->keyboard_obj = NULL;
            }
        }
        else if (!window->closed)
        {
            shall_flush |= window->flush_pending;
        }
        window->flush_pending = false;
    }

    if (shall_flush)
    {
        wl_display_flush(application.display);
        application.cursor_flush_pending = false;
    }

    wl_display_read_events(application.display);
    wl_display_dispatch_pending(application.display);
}

static void _lv_wayland_pointer_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    struct window *window = drv->disp->driver->user_data;
    if (!window)
    {
        return;
    }

    data->point.x = window->body->input.pointer.x;
    data->point.y = window->body->input.pointer.y;
    data->state = window->body->input.pointer.left_button;
}

static void _lv_wayland_pointeraxis_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    struct window *window = drv->disp->driver->user_data;
    if (!window)
    {
        return;
    }

    data->state = window->body->input.pointer.wheel_button;
    data->enc_diff = window->body->input.pointer.wheel_diff;

    window->body->input.pointer.wheel_diff = 0;
}

static void _lv_wayland_keyboard_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    struct window *window = drv->disp->driver->user_data;
    if (!window)
    {
        return;
    }

    data->key = window->body->input.keyboard.key;
    data->state = window->body->input.keyboard.state;
}

static void _lv_wayland_touch_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    struct window *window = drv->disp->driver->user_data;
    if (!window)
    {
        return;
    }

    data->point.x = window->body->input.touch.x;
    data->point.y = window->body->input.touch.y;
    data->state = window->body->input.touch.state;
}

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

/**
 * Initialize Wayland driver
 */
void lv_wayland_init(void)
{
    application.xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
    LV_ASSERT_MSG(application.xdg_runtime_dir, "cannot get XDG_RUNTIME_DIR\n");

    // Create XKB context
    application.xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    LV_ASSERT_MSG(application.xkb_context, "failed to create XKB context");
    if (application.xkb_context == NULL)
    {
        return;
    }

    // Connect to Wayland display
    application.display = wl_display_connect(NULL);
    LV_ASSERT_MSG(application.display, "failed to connect to Wayland server");
    if (application.display == NULL)
    {
        return;
    }

    /* Add registry listener and wait for registry reception */
    application.format = 0xFFFFFFFF;
    application.registry = wl_display_get_registry(application.display);
    wl_registry_add_listener(application.registry, &registry_listener, &application);
    wl_display_dispatch(application.display);
    wl_display_roundtrip(application.display);

    LV_ASSERT_MSG(application.compositor, "Wayland compositor not available");
    if (application.compositor == NULL)
    {
        return;
    }

    LV_ASSERT_MSG(application.shm, "Wayland SHM not available");
    if (application.shm == NULL)
    {
        return;
    }

    LV_ASSERT_MSG((application.format != 0xFFFFFFFF), "WL_SHM_FORMAT not available");
    if (application.format == 0xFFFFFFFF)
    {
        return;
    }

#ifdef LV_WAYLAND_CLIENT_SIDE_DECORATIONS
    const char * env_disable_decorations = getenv("LV_WAYLAND_DISABLE_WINDOWDECORATION");
    application.opt_disable_decorations = ((env_disable_decorations != NULL) &&
                                           (env_disable_decorations[0] != '0'));
#endif

    _lv_ll_init(&application.window_ll, sizeof(struct window));

    application.cycle_timer = lv_timer_create(_lv_wayland_cycle, LV_WAYLAND_CYCLE_PERIOD, NULL);
    LV_ASSERT_MSG(application.cycle_timer, "failed to create cycle timer");
    if (!application.cycle_timer)
    {
        return;
    }
}

/**
 * De-initialize Wayland driver
 */
void lv_wayland_deinit(void)
{
    struct window *window = NULL;

    _LV_LL_READ(&application.window_ll, window)
    {
        if (!window->closed)
        {
            destroy_window(window);
        }
    }

    if (application.shm)
    {
        wl_shm_destroy(application.shm);
    }

#if LV_WAYLAND_XDG_SHELL
    if (application.xdg_wm)
    {
        xdg_wm_base_destroy(application.xdg_wm);
    }
#endif

#if LV_WAYLAND_WL_SHELL
    if (application.wl_shell)
    {
        wl_shell_destroy(application.wl_shell);
    }
#endif

    if (application.wl_seat)
    {
        wl_seat_destroy(application.wl_seat);
    }

    if (application.subcompositor)
    {
        wl_subcompositor_destroy(application.subcompositor);
    }

    if (application.compositor)
    {
        wl_compositor_destroy(application.compositor);
    }

    wl_registry_destroy(application.registry);
    wl_display_flush(application.display);
    wl_display_disconnect(application.display);

    _lv_ll_clear(&application.window_ll);
}

/**
 * Create wayland window
 * @param hor_res initial horizontal window size in pixels
 * @param ver_res initial vertical window size in pixels
 * @param title window title
 * @param close_cb function to be called when the window gets closed by the user (optional)
 * @return new display backed by a Wayland window, or NULL on error
 */
lv_disp_t * lv_wayland_create_window(lv_coord_t hor_res, lv_coord_t ver_res, char *title,
                                     lv_wayland_display_close_f_t close_cb)
{
    lv_color_t * buf1 = NULL;
    struct window *window;

    window = create_window(&application, hor_res, ver_res, title);
    if (!window)
    {
        LV_LOG_ERROR("failed to create wayland window\n");
        return NULL;
    }

    window->close_cb = close_cb;

    /* Initialize draw buffer */
    buf1 = lv_mem_alloc(hor_res * ver_res * sizeof(lv_color_t));
    if (!buf1)
    {
        LV_LOG_ERROR("failed to allocate draw buffer\n");
        destroy_window(window);
        return NULL;
    }

    lv_disp_draw_buf_init(&window->lv_disp_draw_buf, buf1, NULL, hor_res * ver_res);

    /* Initialize display driver */
    lv_disp_drv_init(&window->lv_disp_drv);
    window->lv_disp_drv.draw_buf = &window->lv_disp_draw_buf;
    window->lv_disp_drv.hor_res = hor_res;
    window->lv_disp_drv.ver_res = ver_res;
    window->lv_disp_drv.flush_cb = _lv_wayland_flush;
    window->lv_disp_drv.user_data = window;

    /* Register display */
    window->lv_disp = lv_disp_drv_register(&window->lv_disp_drv);

    /* Register input */
    lv_indev_drv_init(&window->lv_indev_drv_pointer);
    window->lv_indev_drv_pointer.type = LV_INDEV_TYPE_POINTER;
    window->lv_indev_drv_pointer.read_cb = _lv_wayland_pointer_read;
    window->lv_indev_drv_pointer.disp = window->lv_disp;
    window->lv_indev_pointer = lv_indev_drv_register(&window->lv_indev_drv_pointer);
    if (!window->lv_indev_pointer)
    {
        LV_LOG_ERROR("failed to register pointer indev\n");
    }

    lv_indev_drv_init(&window->lv_indev_drv_pointeraxis);
    window->lv_indev_drv_pointeraxis.type = LV_INDEV_TYPE_ENCODER;
    window->lv_indev_drv_pointeraxis.read_cb = _lv_wayland_pointeraxis_read;
    window->lv_indev_drv_pointeraxis.disp = window->lv_disp;
    window->lv_indev_pointeraxis = lv_indev_drv_register(&window->lv_indev_drv_pointeraxis);
    if (!window->lv_indev_pointeraxis)
    {
        LV_LOG_ERROR("failed to register pointeraxis indev\n");
    }

    lv_indev_drv_init(&window->lv_indev_drv_touch);
    window->lv_indev_drv_touch.type = LV_INDEV_TYPE_POINTER;
    window->lv_indev_drv_touch.read_cb = _lv_wayland_touch_read;
    window->lv_indev_drv_touch.disp = window->lv_disp;
    window->lv_indev_touch = lv_indev_drv_register(&window->lv_indev_drv_touch);
    if (!window->lv_indev_touch)
    {
        LV_LOG_ERROR("failed to register touch indev\n");
    }

    lv_indev_drv_init(&window->lv_indev_drv_keyboard);
    window->lv_indev_drv_keyboard.type = LV_INDEV_TYPE_KEYPAD;
    window->lv_indev_drv_keyboard.read_cb = _lv_wayland_keyboard_read;
    window->lv_indev_drv_keyboard.disp = window->lv_disp;
    window->lv_indev_keyboard = lv_indev_drv_register(&window->lv_indev_drv_keyboard);
    if (!window->lv_indev_keyboard)
    {
        LV_LOG_ERROR("failed to register keyboard indev\n");
    }

    return window->lv_disp;
}

/**
 * Close wayland window
 * @param disp LVGL display using window to be closed
 */
void lv_wayland_close_window(lv_disp_t * disp)
{
    struct window *window = disp->driver->user_data;
    if (!window || window->closed)
    {
        return;
    }
    window->shall_close = true;
    window->close_cb = NULL;
}

/**
 * Get pointer input device for given LVGL display
 * @param disp LVGL display
 * @return input device connected to pointer events, or NULL on error
 */
lv_indev_t * lv_wayland_get_pointer(lv_disp_t * disp)
{
    struct window *window = disp->driver->user_data;
    if (!window)
    {
        return NULL;
    }
    return window->lv_indev_pointer;
}

/**
 * Get pointer axis input device for given LVGL display
 * @param disp LVGL display
 * @return input device connected to pointer axis events, or NULL on error
 */
lv_indev_t * lv_wayland_get_pointeraxis(lv_disp_t * disp)
{
    struct window *window = disp->driver->user_data;
    if (!window)
    {
        return NULL;
    }
    return window->lv_indev_pointeraxis;
}

/**
 * Get keyboard input device for given LVGL display
 * @param disp LVGL display
 * @return input device connected to keyboard, or NULL on error
 */
lv_indev_t * lv_wayland_get_keyboard(lv_disp_t * disp)
{
    struct window *window = disp->driver->user_data;
    if (!window)
    {
        return NULL;
    }
    return window->lv_indev_keyboard;
}

/**
 * Get touchscreen input device for given LVGL display
 * @param disp LVGL display
 * @return input device connected to touchscreen, or NULL on error
 */
lv_indev_t * lv_wayland_get_touchscreen(lv_disp_t * disp)
{
    struct window *window = disp->driver->user_data;
    if (!window)
    {
        return NULL;
    }
    return window->lv_indev_touch;
}

#endif // USE_WAYLAND
