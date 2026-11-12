/*
 * The Cocoa side of the window system (rt_window.m). USER32 (rt_user32.c)
 * owns the Win32 state (window handles, styles, rectangles, message
 * queues). This interface creates and changes the native windows, and it
 * reports native events back to USER32 through the rt_user32_on_* functions.
 *
 * Coordinates: Win32 screen coordinates in points. The origin is the top
 * left corner of the main screen; y increases downward.
 *
 * All rt_cocoa_* functions except rt_cocoa_is_main_thread must be called
 * on the main thread. They stop the process with a message on any other
 * thread.
 */
#ifndef RT_WINDOW_H
#define RT_WINDOW_H

#include <stdbool.h>
#include <stdint.h>

typedef struct rt_rect {
    int32_t left, top, right, bottom;
} rt_rect;

/* Native window kinds (from the Win32 style). */
#define RT_WIN_TITLED 1u       /* WS_CAPTION: title bar */
#define RT_WIN_CLOSABLE 2u     /* WS_SYSMENU: close button */
#define RT_WIN_MINIMIZABLE 4u  /* WS_MINIMIZEBOX */
#define RT_WIN_RESIZABLE 8u    /* WS_THICKFRAME */
#define RT_WIN_FULLSCREEN 16u  /* borderless window that covers the screen */

bool rt_cocoa_is_main_thread(void);

/* Start the application object (activation policy, main menu). */
void rt_cocoa_init(void);

/* The main screen size in points. */
void rt_cocoa_screen_size(int32_t *w, int32_t *h);

/* The outer frame (screen coordinates) for a client rectangle and the
   given window kind. */
rt_rect rt_cocoa_frame_for_client(rt_rect client, unsigned kind);
/* The client rectangle for an outer frame and the given window kind. */
rt_rect rt_cocoa_client_for_frame(rt_rect frame, unsigned kind);

/* Create a hidden native window for hwnd with the client rectangle
   (screen coordinates). title is UTF-8. Returns the native window
   reference. */
void *rt_cocoa_window_create(uint32_t hwnd, rt_rect client, unsigned kind, const char *title);
void rt_cocoa_window_destroy(void *w);
void rt_cocoa_window_show(void *w, bool visible, bool activate);
void rt_cocoa_window_minimize(void *w);
/* The client rectangle (screen coordinates) of a native window. */
rt_rect rt_cocoa_window_client(void *w);
/* The CAMetalLayer of the window content view (for the renderer). */
void *rt_cocoa_window_layer(void *w);

/* Handle all native events that are waiting. Native events become
   rt_user32_on_* calls. */
void rt_cocoa_pump(void);

/* Activate the application and make w the key window. */
void rt_cocoa_activate(void *w);
/* ShowCursor: hide or show the cursor (called on display count changes).
   Any thread can call this function. */
void rt_cocoa_cursor_visible(bool visible);
/* DINPUT8 exclusive mouse: when on, hide the cursor, move it to the centre
   of the key window and stop mouse motion from moving it (the events still
   report the motion). Any thread can call this function. */
void rt_cocoa_mouse_capture(bool on);
/* Headless mode (environment variable RT_HEADLESS=1): no window is shown,
   the application never becomes active and never takes the cursor, and
   Present does not show frames. A guest window that is shown is reported
   to USER32 and DINPUT8 as the active window. RT_INPUT_SCRIPT gives the
   key input (rt_window.m describes the format). */
bool rt_cocoa_headless(void);
/* True if Caps Lock is on. */
bool rt_cocoa_caps_lock(void);

/* Callbacks into USER32 (main thread). */
void rt_user32_on_key(uint32_t hwnd, bool down, bool is_repeat, uint16_t mac_keycode, const uint16_t *chars, int nchars);
void rt_user32_on_modifiers(uint32_t hwnd, uint32_t mac_flags);
/* msg: WM_MOUSEMOVE, WM_[LRM]BUTTON(DOWN|UP), WM_MOUSEWHEEL. x, y: client
   coordinates. wheel: WM_MOUSEWHEEL delta (120 for one notch). */
void rt_user32_on_mouse(uint32_t hwnd, uint32_t msg, int32_t x, int32_t y, int32_t wheel);
void rt_user32_on_active(uint32_t hwnd, bool active);
void rt_user32_on_close(uint32_t hwnd);
void rt_user32_on_frame(uint32_t hwnd, bool resized);
void rt_user32_on_minimized(uint32_t hwnd, bool minimized);
/* The application menu Quit command. */
void rt_user32_on_quit(void);

#endif
