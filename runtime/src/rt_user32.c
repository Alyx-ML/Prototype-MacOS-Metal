/*
 * USER32: window classes, top-level windows, message queues, keyboard and
 * mouse input, and the small set of system settings that the game uses.
 *
 * The native windows and events are in rt_window.m (rt_window.h). This
 * file keeps the Win32 state and sends the Win32 messages.
 *
 * Model:
 *   - Windows are top-level windows of the main thread. Child windows,
 *     menus and windows of other threads are not supported (explicit stop).
 *   - The main thread has one posted-message queue and one input queue
 *     (10000 entries each, the Windows default limit). PeekMessageA
 *     returns posted messages first, then input, then WM_QUIT, as Windows.
 *   - A message that Windows sends (SendMessage semantics) is a direct
 *     call of the window procedure with rt_call. No lock is held during a
 *     call into guest code.
 *   - GetKeyState gives the key state at the time of the last message that
 *     PeekMessageA removed, as on Windows.
 *   - Coordinates are points. The screen origin is the top left corner of
 *     the main display.
 *   - Keys map by physical position to the US keyboard layout (virtual-key
 *     codes and scan codes). GetKeyboardLayout reports 0x04090409.
 *     The Option key is Alt, the Command key is the Windows key.
 */
#include "rt.h"
#include "rt_module.h"
#include "rt_dinput.h"
#include "rt_thread.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <time.h>

/* ---------------------------------------------------------------- constants */

#define WM_NULL 0x0000u
#define WM_CREATE 0x0001u
#define WM_DESTROY 0x0002u
#define WM_MOVE 0x0003u
#define WM_SIZE 0x0005u
#define WM_ACTIVATE 0x0006u
#define WM_SETFOCUS 0x0007u
#define WM_KILLFOCUS 0x0008u
#define WM_ENABLE 0x000Au
#define WM_SETTEXT 0x000Cu
#define WM_GETTEXT 0x000Du
#define WM_GETTEXTLENGTH 0x000Eu
#define WM_PAINT 0x000Fu
#define WM_CLOSE 0x0010u
#define WM_QUIT 0x0012u
#define WM_ERASEBKGND 0x0014u
#define WM_SHOWWINDOW 0x0018u
#define WM_ACTIVATEAPP 0x001Cu
#define WM_SETCURSOR 0x0020u
#define WM_MOUSEACTIVATE 0x0021u
#define WM_GETMINMAXINFO 0x0024u
#define WM_WINDOWPOSCHANGING 0x0046u
#define WM_WINDOWPOSCHANGED 0x0047u
#define WM_NCCREATE 0x0081u
#define WM_NCDESTROY 0x0082u
#define WM_NCCALCSIZE 0x0083u
#define WM_NCHITTEST 0x0084u
#define WM_NCPAINT 0x0085u
#define WM_NCACTIVATE 0x0086u
#define WM_KEYDOWN 0x0100u
#define WM_KEYUP 0x0101u
#define WM_CHAR 0x0102u
#define WM_DEADCHAR 0x0103u
#define WM_SYSKEYDOWN 0x0104u
#define WM_SYSKEYUP 0x0105u
#define WM_SYSCHAR 0x0106u
#define WM_SYSDEADCHAR 0x0107u
#define WM_SYSCOMMAND 0x0112u
#define WM_TIMER 0x0113u
#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_LBUTTONDBLCLK 0x0203u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u
#define WM_MOUSEWHEEL 0x020Au
#define WM_USER 0x0400u

#define SC_SIZE 0xF000u
#define SC_MOVE 0xF010u
#define SC_MINIMIZE 0xF020u
#define SC_MAXIMIZE 0xF030u
#define SC_CLOSE 0xF060u
#define SC_KEYMENU 0xF100u
#define SC_RESTORE 0xF120u
#define SC_SCREENSAVE 0xF140u
#define SC_MONITORPOWER 0xF170u

#define WS_OVERLAPPED 0x00000000u
#define WS_MAXIMIZEBOX 0x00010000u
#define WS_MINIMIZEBOX 0x00020000u
#define WS_THICKFRAME 0x00040000u
#define WS_SYSMENU 0x00080000u
#define WS_HSCROLL 0x00100000u
#define WS_VSCROLL 0x00200000u
#define WS_DLGFRAME 0x00400000u
#define WS_BORDER 0x00800000u
#define WS_CAPTION 0x00C00000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_DISABLED 0x08000000u
#define WS_VISIBLE 0x10000000u
#define WS_MINIMIZE 0x20000000u
#define WS_CHILD 0x40000000u
#define WS_POPUP 0x80000000u

#define WS_EX_TOPMOST 0x00000008u
#define WS_EX_WINDOWEDGE 0x00000100u
#define WS_EX_APPWINDOW 0x00040000u

#define CW_USEDEFAULT 0x80000000u

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10

#define SIZE_RESTORED 0u
#define SIZE_MINIMIZED 1u

#define WA_INACTIVE 0u
#define WA_ACTIVE 1u

#define PM_REMOVE 1u
#define PM_NOYIELD 2u

#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_ID (-12)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)

#define MK_LBUTTON 0x01u
#define MK_RBUTTON 0x02u
#define MK_SHIFT 0x04u
#define MK_CONTROL 0x08u
#define MK_MBUTTON 0x10u

#define VK_LBUTTON 0x01u
#define VK_RBUTTON 0x02u
#define VK_MBUTTON 0x04u
#define VK_BACK 0x08u
#define VK_TAB 0x09u
#define VK_CLEAR 0x0Cu
#define VK_RETURN 0x0Du
#define VK_SHIFT 0x10u
#define VK_CONTROL 0x11u
#define VK_MENU 0x12u
#define VK_CAPITAL 0x14u
#define VK_ESCAPE 0x1Bu
#define VK_F4 0x73u
#define VK_F10 0x79u
#define VK_NUMLOCK 0x90u
#define VK_SCROLL 0x91u
#define VK_LSHIFT 0xA0u
#define VK_RSHIFT 0xA1u
#define VK_LCONTROL 0xA2u
#define VK_RCONTROL 0xA3u
#define VK_LMENU 0xA4u
#define VK_RMENU 0xA5u

#define ERROR_ACCESS_DENIED_U 5u
#define ERROR_INVALID_PARAMETER_U 87u
#define ERROR_INVALID_WINDOW_HANDLE 1400u
#define ERROR_INVALID_INDEX 1413u
#define ERROR_CLASS_ALREADY_EXISTS 1410u
#define ERROR_CLASS_DOES_NOT_EXIST 1411u
#define ERROR_CLASS_HAS_WINDOWS 1412u
#define ERROR_CANNOT_FIND_WND_CLASS 1407u
#define ERROR_NOT_ENOUGH_QUOTA 1816u

#define US_KEYBOARD_LAYOUT 0x04090409u

/* ---------------------------------------------------------------- state */

#define MAX_CLASSES 64
#define MAX_WINDOWS 256
#define HWND_BASE 0x00010000u
#define HWND_STEP 0x10u
#define HWND_DESKTOP_VALUE (HWND_BASE + HWND_STEP) /* slot 0 */
#define QUEUE_LIMIT 10000u

typedef struct wclass {
    bool used;
    char name[256];
    uint16_t atom;
    uint32_t style, wndproc, cls_extra, wnd_extra, hinstance, icon, cursor, brush, icon_sm;
} wclass;

typedef struct wnd {
    bool used;
    bool destroying;
    bool visible;
    bool minimized;
    int cls;
    uint32_t style, exstyle, wndproc, hinstance, id, userdata;
    unsigned kind;
    uint8_t *extra;
    uint32_t extra_size;
    char title[256];
    void *ns;
    rt_rect wrect, crect; /* screen coordinates */
} wnd;

typedef struct qmsg {
    uint32_t hwnd, msg, wparam, lparam, time;
    int32_t x, y;
    uint16_t chars[16];
    uint8_t nchars;
} qmsg;

typedef struct queue {
    qmsg *buf;
    uint32_t head, count;
} queue;

static pthread_mutex_t user_lock = PTHREAD_MUTEX_INITIALIZER;
static wclass classes[MAX_CLASSES];
static wnd windows[MAX_WINDOWS]; /* slot 0 is the desktop window */
static queue posted_q, input_q;
static uint32_t active_hwnd;   /* active (key) window, 0 if the application is not active */
static int32_t cursor_count;   /* ShowCursor display count */
static int32_t mouse_x, mouse_y; /* last cursor position, screen coordinates */
static uint8_t input_down[256]; /* key and button state as the events arrive (main thread) */
static uint8_t sync_state[256]; /* GetKeyState state: 0x80 down, 0x01 toggled */
static uint32_t last_mods;      /* last NSEvent modifier flags */

/* The last key message that PeekMessageA removed, for TranslateMessage. */
static qmsg last_key;
static bool last_key_valid;

static _Thread_local bool quit_posted;
static _Thread_local uint32_t quit_code;

/* Accessibility settings (SystemParametersInfoA). */
static uint32_t sticky_flags = 0x1FEu;  /* STICKYKEYS: Windows XP default flags */
static uint32_t toggle_flags = 0x1Eu;   /* TOGGLEKEYS */
static uint32_t filter_keys[6] = { 24u, 0x7Eu, 1000u, 1000u, 500u, 0u }; /* FILTERKEYS */

static void lock(void)
{
    if (pthread_mutex_lock(&user_lock) != 0) rt_fatal("USER32 lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&user_lock) != 0) rt_fatal("USER32 unlock failed");
}

static uint32_t now_ms(void)
{
    return (uint32_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000u);
}

static void need_main(const char *fn)
{
    if (!rt_cocoa_is_main_thread())
        rt_fatal("%s: windows are supported on the main thread only", fn);
}

static uint32_t hwnd_of(int i)
{
    return HWND_BASE + HWND_STEP * (uint32_t)(i + 1);
}

/* The window slot of hwnd (lock held), or -1. Slot 0 is the desktop. */
static int slot_of(uint32_t hwnd)
{
    uint32_t i;
    if (hwnd < HWND_BASE + HWND_STEP || (hwnd - HWND_BASE) % HWND_STEP != 0) return -1;
    i = (hwnd - HWND_BASE) / HWND_STEP - 1;
    if (i == 0) return 0;
    if (i >= MAX_WINDOWS || !windows[i].used) return -1;
    return (int)i;
}

/* A top-level window (not the desktop), or NULL (lock held). */
static wnd *window_of(uint32_t hwnd)
{
    int i = slot_of(hwnd);
    return i > 0 ? &windows[i] : NULL;
}

static uint32_t make_lparam(int32_t lo, int32_t hi)
{
    if (lo < -32768) lo = -32768;
    if (lo > 32767) lo = 32767;
    if (hi < -32768) hi = -32768;
    if (hi > 32767) hi = 32767;
    return (uint32_t)(uint16_t)(int16_t)lo | ((uint32_t)(uint16_t)(int16_t)hi << 16);
}

/* ---------------------------------------------------------------- queues */

static void queue_init(queue *q)
{
    if (q->buf != NULL) return;
    q->buf = calloc(QUEUE_LIMIT, sizeof *q->buf);
    if (q->buf == NULL) rt_fatal("out of host memory (message queue)");
}

/* Lock held. */
static bool queue_push(queue *q, const qmsg *m, bool front)
{
    queue_init(q);
    if (q->count >= QUEUE_LIMIT) return false;
    if (front)
    {
        q->head = (q->head + QUEUE_LIMIT - 1) % QUEUE_LIMIT;
        q->buf[q->head] = *m;
    }
    else
    {
        q->buf[(q->head + q->count) % QUEUE_LIMIT] = *m;
    }
    q->count++;
    return true;
}

static qmsg *queue_at(queue *q, uint32_t i)
{
    return &q->buf[(q->head + i) % QUEUE_LIMIT];
}

/* Remove entry i (lock held). */
static void queue_remove(queue *q, uint32_t i)
{
    uint32_t j;
    for (j = i; j + 1 < q->count; j++) *queue_at(q, j) = *queue_at(q, j + 1);
    q->count--;
    if (q->count == 0) q->head = 0;
}

/* Remove the messages of a destroyed window (lock held). */
static void queue_drop_window(queue *q, uint32_t hwnd)
{
    uint32_t i = 0;
    while (i < q->count)
    {
        if (queue_at(q, i)->hwnd == hwnd)
            queue_remove(q, i);
        else
            i++;
    }
}

bool rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    qmsg m;
    bool ok;
    memset(&m, 0, sizeof m);
    m.hwnd = hwnd;
    m.msg = msg;
    m.wparam = wparam;
    m.lparam = lparam;
    m.time = now_ms();
    lock();
    m.x = mouse_x;
    m.y = mouse_y;
    if (window_of(hwnd) == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        return false;
    }
    ok = queue_push(&posted_q, &m, false);
    unlock();
    if (!ok) rt_set_last_error(ERROR_NOT_ENOUGH_QUOTA);
    return ok;
}

/* Add an input message (main thread). Mouse moves are combined, as on
   Windows: a new WM_MOUSEMOVE replaces a WM_MOUSEMOVE at the end of the
   queue. */
static void post_input(const qmsg *m)
{
    bool ok;
    lock();
    if (m->msg == WM_MOUSEMOVE && input_q.count > 0)
    {
        qmsg *last = queue_at(&input_q, input_q.count - 1);
        if (last->msg == WM_MOUSEMOVE && last->hwnd == m->hwnd)
        {
            *last = *m;
            unlock();
            return;
        }
    }
    ok = queue_push(&input_q, m, false);
    unlock();
    if (!ok) rt_log("USER32: the input queue is full (%u messages); input message 0x%04x was discarded", QUEUE_LIMIT, m->msg);
}

/* ---------------------------------------------------------------- window procedure calls */

/* Send a message: call the window procedure (no lock held). */
static uint32_t send_msg(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam)
{
    uint32_t proc;
    wnd *w;
    lock();
    w = window_of(hwnd);
    proc = w != NULL ? w->wndproc : 0;
    unlock();
    if (w == NULL) return 0;
    if (proc == 0) rt_fatal("window 0x%08x has no window procedure", hwnd);
    return rt_call(rt_self()->cpu, proc, true, 0, 4, (const uint32_t[]){ hwnd, msg, wparam, lparam });
}

/* ---------------------------------------------------------------- text */

/* Windows-1252 text to UTF-8 (out has room for 3 bytes per input byte). */
static void ansi_to_utf8(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    const uint8_t *s = (const uint8_t *)in;
    for (; *s != 0; s++)
    {
        uint16_t c = rt_cp1252_to_unicode(*s);
        if (c < 0x80)
        {
            if (o + 1 >= cap) break;
            out[o++] = (char)c;
        }
        else if (c < 0x800)
        {
            if (o + 2 >= cap) break;
            out[o++] = (char)(0xC0 | (c >> 6));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
        else
        {
            if (o + 3 >= cap) break;
            out[o++] = (char)(0xE0 | (c >> 12));
            out[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (c & 0x3F));
        }
    }
    out[o] = 0;
}

/* Unicode to a Windows-1252 byte; -1 if the character has no byte. */
static int unicode_to_ansi(uint16_t c)
{
    int b;
    if (c < 0x80 || (c >= 0xA0 && c <= 0xFF)) return c;
    for (b = 0x80; b <= 0x9F; b++)
        if (rt_cp1252_to_unicode((uint8_t)b) == c) return b;
    return -1;
}

/* Read a guest string argument (NULL allowed) into buf. Stops the process
   for text that is too long. */
static void gtext(const char *what, uint32_t g, char *buf, size_t n)
{
    buf[0] = 0;
    if (g != 0 && rt_gstr(buf, n, g) != 0) rt_fatal("%s: text is longer than %zu bytes", what, n - 1);
}

/* ---------------------------------------------------------------- classes */

/* Find a class by name or atom (lock held). */
static int find_class(uint32_t name_or_atom, uint32_t hinstance, bool any_instance)
{
    int i;
    char name[256];
    bool by_atom = name_or_atom != 0 && name_or_atom < 0x10000u;
    if (name_or_atom == 0) return -1;
    if (!by_atom && rt_gstr(name, sizeof name, name_or_atom) != 0) return -1;
    for (i = 0; i < MAX_CLASSES; i++)
    {
        if (!classes[i].used) continue;
        if (!any_instance && classes[i].hinstance != hinstance) continue;
        if (by_atom ? classes[i].atom == name_or_atom : strcasecmp(classes[i].name, name) == 0) return i;
    }
    return -1;
}

static uint32_t register_class(uint32_t style, uint32_t proc, uint32_t cls_extra, uint32_t wnd_extra,
                               uint32_t hinst, uint32_t icon, uint32_t cursor, uint32_t brush,
                               uint32_t menu, uint32_t name, uint32_t icon_sm)
{
    char text[256];
    int i, free_slot = -1;
    if (menu != 0) rt_fatal("RegisterClass: class menus are not supported");
    if (proc == 0 || name == 0 || (int32_t)cls_extra < 0 || (int32_t)wnd_extra < 0 || cls_extra > 40 || wnd_extra > 40)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        return 0;
    }
    if (name < 0x10000u) rt_fatal("RegisterClass: a class name given as an atom is not supported");
    gtext("RegisterClass", name, text, sizeof text);
    lock();
    for (i = 0; i < MAX_CLASSES; i++)
    {
        if (classes[i].used && classes[i].hinstance == hinst && strcasecmp(classes[i].name, text) == 0)
        {
            unlock();
            rt_set_last_error(ERROR_CLASS_ALREADY_EXISTS);
            return 0;
        }
        if (!classes[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) rt_fatal("RegisterClass: more than %d window classes", MAX_CLASSES);
    memset(&classes[free_slot], 0, sizeof classes[free_slot]);
    classes[free_slot].used = true;
    memcpy(classes[free_slot].name, text, sizeof text);
    classes[free_slot].atom = (uint16_t)(0xC000u + (uint32_t)free_slot);
    classes[free_slot].style = style;
    classes[free_slot].wndproc = proc;
    classes[free_slot].cls_extra = cls_extra;
    classes[free_slot].wnd_extra = wnd_extra;
    classes[free_slot].hinstance = hinst;
    classes[free_slot].icon = icon;
    classes[free_slot].cursor = cursor;
    classes[free_slot].brush = brush;
    classes[free_slot].icon_sm = icon_sm;
    unlock();
    return classes[free_slot].atom;
}

/* ATOM RegisterClassA(const WNDCLASSA *) */
void rt_RegisterClassA(_cpu *cpu)
{
    uint32_t c = ARG(0);
    if (c == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    RET(register_class(rd32(c), rd32(c + 4), rd32(c + 8), rd32(c + 12), rd32(c + 16), rd32(c + 20),
                       rd32(c + 24), rd32(c + 28), rd32(c + 32), rd32(c + 36), 0));
}

/* ATOM RegisterClassExA(const WNDCLASSEXA *) */
void rt_RegisterClassExA(_cpu *cpu)
{
    uint32_t c = ARG(0);
    if (c == 0 || rd32(c) != 48)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    RET(register_class(rd32(c + 4), rd32(c + 8), rd32(c + 12), rd32(c + 16), rd32(c + 20), rd32(c + 24),
                       rd32(c + 28), rd32(c + 32), rd32(c + 36), rd32(c + 40), rd32(c + 44)));
}

/* BOOL UnregisterClassA(LPCSTR name, HINSTANCE) */
void rt_UnregisterClassA(_cpu *cpu)
{
    int i, j;
    lock();
    i = find_class(ARG(0), ARG(1), false);
    if (i < 0)
    {
        unlock();
        rt_set_last_error(ERROR_CLASS_DOES_NOT_EXIST);
        RET(0);
        return;
    }
    for (j = 1; j < MAX_WINDOWS; j++)
    {
        if (windows[j].used && windows[j].cls == i)
        {
            unlock();
            rt_set_last_error(ERROR_CLASS_HAS_WINDOWS);
            RET(0);
            return;
        }
    }
    classes[i].used = false;
    unlock();
    RET(1);
}

/* ---------------------------------------------------------------- geometry */

static unsigned kind_of(uint32_t style, int32_t w, int32_t h)
{
    unsigned k = 0;
    int32_t sw, sh;
    if ((style & WS_CAPTION) == WS_CAPTION)
    {
        k |= RT_WIN_TITLED;
        if (style & WS_SYSMENU) k |= RT_WIN_CLOSABLE;
        if (style & WS_MINIMIZEBOX) k |= RT_WIN_MINIMIZABLE;
        if (style & WS_THICKFRAME) k |= RT_WIN_RESIZABLE;
        return k;
    }
    /* A pop-up window without a title bar that is maximized or as large as
       the screen covers the screen. */
    rt_cocoa_screen_size(&sw, &sh);
    if ((style & WS_POPUP) && ((style & WS_MAXIMIZE) || (w >= sw && h >= sh))) k |= RT_WIN_FULLSCREEN;
    return k;
}

/* Update the rectangles of w from the native window (main thread, lock
   not held during the native call). */
static void sync_rects(uint32_t hwnd)
{
    void *ns;
    unsigned kind;
    rt_rect c, f;
    wnd *w;
    lock();
    w = window_of(hwnd);
    ns = w != NULL ? w->ns : NULL;
    kind = w != NULL ? w->kind : 0;
    unlock();
    if (ns == NULL) return;
    c = rt_cocoa_window_client(ns);
    f = (kind & RT_WIN_FULLSCREEN) ? c : rt_cocoa_frame_for_client(c, kind);
    lock();
    w = window_of(hwnd);
    if (w != NULL)
    {
        w->crect = c;
        w->wrect = f;
    }
    unlock();
}

static void send_size_move(uint32_t hwnd)
{
    rt_rect c;
    bool minimized;
    wnd *w;
    lock();
    w = window_of(hwnd);
    if (w == NULL)
    {
        unlock();
        return;
    }
    c = w->crect;
    minimized = w->minimized;
    unlock();
    if (minimized)
        send_msg(hwnd, WM_SIZE, SIZE_MINIMIZED, 0);
    else
        send_msg(hwnd, WM_SIZE, SIZE_RESTORED, make_lparam(c.right - c.left, c.bottom - c.top));
    send_msg(hwnd, WM_MOVE, 0, make_lparam(c.left, c.top));
}

/* BOOL AdjustWindowRect(LPRECT, DWORD style, BOOL menu) */
void rt_AdjustWindowRect(_cpu *cpu)
{
    uint32_t r = ARG(0), style = ARG(1);
    rt_rect c, f;
    unsigned kind;
    need_main(__func__);
    if (ARG(2) != 0) rt_fatal("AdjustWindowRect: windows with menus are not supported");
    if (r == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    c.left = (int32_t)rd32(r);
    c.top = (int32_t)rd32(r + 4);
    c.right = (int32_t)rd32(r + 8);
    c.bottom = (int32_t)rd32(r + 12);
    kind = kind_of(style, c.right - c.left, c.bottom - c.top);
    f = (kind & (RT_WIN_TITLED | RT_WIN_RESIZABLE)) ? rt_cocoa_frame_for_client(c, kind) : c;
    wr32(r, (uint32_t)f.left);
    wr32(r + 4, (uint32_t)f.top);
    wr32(r + 8, (uint32_t)f.right);
    wr32(r + 12, (uint32_t)f.bottom);
    RET(1);
}

static void write_rect(uint32_t g, rt_rect r)
{
    wr32(g, (uint32_t)r.left);
    wr32(g + 4, (uint32_t)r.top);
    wr32(g + 8, (uint32_t)r.right);
    wr32(g + 12, (uint32_t)r.bottom);
}

static rt_rect screen_rect(void)
{
    rt_rect r = { 0, 0, 0, 0 };
    rt_cocoa_screen_size(&r.right, &r.bottom);
    return r;
}

/* BOOL GetClientRect(HWND, LPRECT) */
void rt_GetClientRect(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), r = ARG(1);
    rt_rect c;
    int i;
    if (r == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    if (rt_cocoa_is_main_thread()) sync_rects(hwnd);
    lock();
    i = slot_of(hwnd);
    if (i < 0)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    if (i > 0) c = windows[i].crect;
    unlock();
    if (i == 0) c = screen_rect();
    write_rect(r, (rt_rect){ 0, 0, c.right - c.left, c.bottom - c.top });
    RET(1);
}

/* BOOL GetWindowRect(HWND, LPRECT) */
void rt_GetWindowRect(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), r = ARG(1);
    rt_rect f;
    int i;
    if (r == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    if (rt_cocoa_is_main_thread()) sync_rects(hwnd);
    lock();
    i = slot_of(hwnd);
    if (i < 0)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    if (i > 0) f = windows[i].wrect;
    unlock();
    if (i == 0) f = screen_rect();
    write_rect(r, f);
    RET(1);
}

/* BOOL ScreenToClient(HWND, LPPOINT) */
void rt_ScreenToClient(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), p = ARG(1);
    rt_rect c = { 0, 0, 0, 0 };
    int i;
    if (p == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    if (rt_cocoa_is_main_thread()) sync_rects(hwnd);
    lock();
    i = slot_of(hwnd);
    if (i > 0) c = windows[i].crect;
    unlock();
    if (i < 0)
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    wr32(p, (uint32_t)((int32_t)rd32(p) - c.left));
    wr32(p + 4, (uint32_t)((int32_t)rd32(p + 4) - c.top));
    RET(1);
}

/* ---------------------------------------------------------------- activation */

static void clear_input_state(void)
{
    int i;
    lock();
    memset(input_down, 0, sizeof input_down);
    for (i = 0; i < 256; i++) sync_state[i] &= 0x01;
    unlock();
}

/* Activate or deactivate hwnd with the Windows message order. */
static void set_active(uint32_t hwnd, bool active)
{
    uint32_t old, minimized = 0;
    wnd *w;
    lock();
    old = active_hwnd;
    w = window_of(hwnd);
    if (w == NULL || w->destroying || (active && old == hwnd) || (!active && old != hwnd))
    {
        unlock();
        return;
    }
    if (w->minimized) minimized = 1;
    active_hwnd = active ? hwnd : 0;
    unlock();
    if (active)
    {
        if (old != 0)
        {
            send_msg(old, WM_NCACTIVATE, 0, 0);
            send_msg(old, WM_ACTIVATE, WA_INACTIVE, hwnd);
            send_msg(old, WM_KILLFOCUS, hwnd, 0);
        }
        else
        {
            send_msg(hwnd, WM_ACTIVATEAPP, 1, 0);
        }
        send_msg(hwnd, WM_NCACTIVATE, 1, 0);
        send_msg(hwnd, WM_ACTIVATE, WA_ACTIVE | (minimized << 16), old);
        send_msg(hwnd, WM_SETFOCUS, old, 0);
    }
    else
    {
        /* Key-up events do not arrive after the focus is lost; release all
           keys so that no key stays down. */
        clear_input_state();
        send_msg(hwnd, WM_NCACTIVATE, 0, 0);
        send_msg(hwnd, WM_ACTIVATE, WA_INACTIVE | (minimized << 16), 0);
        send_msg(hwnd, WM_ACTIVATEAPP, 0, 0);
        send_msg(hwnd, WM_KILLFOCUS, 0, 0);
    }
}

/* HWND GetForegroundWindow(void) */
void rt_GetForegroundWindow(_cpu *cpu)
{
    lock();
    RET(active_hwnd);
    unlock();
}

/* BOOL SetForegroundWindow(HWND) */
void rt_SetForegroundWindow(_cpu *cpu)
{
    uint32_t hwnd = ARG(0);
    void *ns;
    wnd *w;
    need_main(__func__);
    lock();
    w = window_of(hwnd);
    ns = w != NULL ? w->ns : NULL;
    unlock();
    if (ns == NULL)
    {
        RET(0);
        return;
    }
    rt_cocoa_activate(ns);
    set_active(hwnd, true);
    RET(1);
}

/* HWND GetDesktopWindow(void) */
void rt_GetDesktopWindow(_cpu *cpu)
{
    RET(HWND_DESKTOP_VALUE);
}

/* ---------------------------------------------------------------- windows */

static void destroy_window(uint32_t hwnd);
static int show_window(uint32_t hwnd, int cmd);

/* HWND CreateWindowExA(DWORD exstyle, LPCSTR class, LPCSTR title,
   DWORD style, int x, int y, int w, int h, HWND parent, HMENU menu,
   HINSTANCE, LPVOID param) */
void rt_CreateWindowExA(_cpu *cpu)
{
    uint32_t exstyle = ARG(0), cname = ARG(1), title = ARG(2), style = ARG(3);
    uint32_t ax = ARG(4), ay = ARG(5), aw = ARG(6), ah = ARG(7), parent = ARG(8), menu = ARG(9), hinst = ARG(10), param = ARG(11);
    char text[256], utf8[3 * 256];
    int ci, i, slot = -1;
    int32_t sw, sh, w, h;
    rt_rect frame, client;
    unsigned kind;
    uint32_t hwnd, cs, cls_proc, cls_extra;
    void *ns;
    need_main(__func__);
    if ((parent != 0 && parent != HWND_DESKTOP_VALUE) || (style & WS_CHILD))
        rt_fatal("CreateWindowExA: child windows are not supported");
    if (menu != 0) rt_fatal("CreateWindowExA: windows with menus are not supported");
    if (exstyle & ~(WS_EX_WINDOWEDGE | WS_EX_APPWINDOW))
        rt_fatal("CreateWindowExA: extended style 0x%08x is not supported", exstyle);
    if (style & (WS_HSCROLL | WS_VSCROLL | WS_DISABLED | WS_MINIMIZE))
        rt_fatal("CreateWindowExA: style 0x%08x is not supported", style);
    gtext("CreateWindowExA", title, text, sizeof text);
    lock();
    ci = find_class(cname, hinst, true);
    if (ci < 0)
    {
        unlock();
        rt_set_last_error(ERROR_CANNOT_FIND_WND_CLASS);
        RET(0);
        return;
    }
    cls_proc = classes[ci].wndproc;
    cls_extra = classes[ci].wnd_extra;
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        if (!windows[i].used)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0) rt_fatal("CreateWindowExA: more than %d windows", MAX_WINDOWS - 1);
    unlock();

    /* An overlapped window always has a title bar and a border. */
    if (!(style & WS_POPUP)) style |= WS_CAPTION | WS_CLIPSIBLINGS;

    rt_cocoa_screen_size(&sw, &sh);
    if (aw == CW_USEDEFAULT)
    {
        w = (style & WS_POPUP) ? 0 : sw * 3 / 4;
        h = (style & WS_POPUP) ? 0 : sh * 3 / 4;
    }
    else
    {
        w = (int32_t)aw;
        h = (int32_t)ah;
    }
    if (w < 0 || h < 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    kind = kind_of(style, w, h);
    if (kind & RT_WIN_FULLSCREEN)
    {
        frame = (rt_rect){ 0, 0, sw, sh };
        client = frame;
    }
    else
    {
        int32_t x = ax == CW_USEDEFAULT ? (sw - w) / 2 : (int32_t)ax;
        int32_t y = ax == CW_USEDEFAULT ? (sh - h) / 2 : (int32_t)ay;
        frame = (rt_rect){ x, y, x + w, y + h };
        client = (kind & (RT_WIN_TITLED | RT_WIN_RESIZABLE)) ? rt_cocoa_client_for_frame(frame, kind) : frame;
    }

    hwnd = hwnd_of(slot);
    ansi_to_utf8(text, utf8, sizeof utf8);
    ns = rt_cocoa_window_create(hwnd, client, kind, utf8);
    lock();
    memset(&windows[slot], 0, sizeof windows[slot]);
    windows[slot].used = true;
    windows[slot].cls = ci;
    windows[slot].style = style & ~WS_VISIBLE;
    windows[slot].exstyle = exstyle;
    windows[slot].wndproc = cls_proc;
    windows[slot].hinstance = hinst;
    windows[slot].kind = kind;
    windows[slot].ns = ns;
    windows[slot].crect = client;
    windows[slot].wrect = frame;
    memcpy(windows[slot].title, text, sizeof text);
    if (cls_extra != 0)
    {
        windows[slot].extra = calloc(1, cls_extra);
        if (windows[slot].extra == NULL) rt_fatal("out of host memory (window extra bytes)");
        windows[slot].extra_size = cls_extra;
    }
    unlock();
    sync_rects(hwnd);

    /* CREATESTRUCTA (48 bytes) for WM_NCCREATE and WM_CREATE. */
    cs = rt_heap_calloc(1, 48);
    if (cs == 0) rt_fatal("out of guest memory (CREATESTRUCTA)");
    wr32(cs, param);
    wr32(cs + 4, hinst);
    wr32(cs + 8, menu);
    wr32(cs + 12, parent);
    wr32(cs + 16, (uint32_t)h);
    wr32(cs + 20, (uint32_t)w);
    wr32(cs + 24, (uint32_t)frame.top);
    wr32(cs + 28, (uint32_t)frame.left);
    wr32(cs + 32, style);
    wr32(cs + 36, title);
    wr32(cs + 40, cname);
    wr32(cs + 44, exstyle);
    if (send_msg(hwnd, WM_NCCREATE, 0, cs) == 0)
    {
        rt_heap_free(cs);
        destroy_window(hwnd);
        RET(0);
        return;
    }
    if ((int32_t)send_msg(hwnd, WM_CREATE, 0, cs) == -1)
    {
        rt_heap_free(cs);
        destroy_window(hwnd);
        RET(0);
        return;
    }
    rt_heap_free(cs);
    send_size_move(hwnd);
    if (style & WS_VISIBLE) show_window(hwnd, (style & WS_MAXIMIZE) ? SW_SHOWMAXIMIZED : SW_SHOW);
    RET(hwnd);
}

static void destroy_window(uint32_t hwnd)
{
    void *ns;
    uint8_t *extra;
    wnd *w;
    lock();
    w = window_of(hwnd);
    if (w == NULL || w->destroying)
    {
        unlock();
        return;
    }
    w->destroying = true;
    unlock();
    if (active_hwnd == hwnd)
    {
        lock();
        active_hwnd = 0;
        unlock();
        clear_input_state();
    }
    {
        uint32_t proc;
        lock();
        proc = w->wndproc;
        unlock();
        rt_call(rt_self()->cpu, proc, true, 0, 4, (const uint32_t[]){ hwnd, WM_DESTROY, 0, 0 });
        rt_call(rt_self()->cpu, proc, true, 0, 4, (const uint32_t[]){ hwnd, WM_NCDESTROY, 0, 0 });
    }
    lock();
    ns = w->ns;
    extra = w->extra;
    queue_drop_window(&posted_q, hwnd);
    queue_drop_window(&input_q, hwnd);
    if (last_key_valid && last_key.hwnd == hwnd) last_key_valid = false;
    memset(w, 0, sizeof *w);
    unlock();
    free(extra);
    if (ns != NULL) rt_cocoa_window_destroy(ns);
}

/* BOOL DestroyWindow(HWND) */
void rt_DestroyWindow(_cpu *cpu)
{
    uint32_t hwnd = ARG(0);
    bool ok;
    if (!rt_cocoa_is_main_thread())
    {
        rt_set_last_error(ERROR_ACCESS_DENIED_U);
        RET(0);
        return;
    }
    lock();
    ok = window_of(hwnd) != NULL;
    unlock();
    if (!ok)
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    destroy_window(hwnd);
    RET(1);
}

/* ShowWindow steps. Returns 1 if the window was visible, 0 if not, -1 if
   hwnd is not a window. */
static int show_window(uint32_t hwnd, int cmd)
{
    bool was_visible, minimized, show, activate, minimize;
    void *ns;
    wnd *w;
    need_main(__func__);
    lock();
    w = window_of(hwnd);
    if (w == NULL)
    {
        unlock();
        return -1;
    }
    was_visible = w->visible;
    minimized = w->minimized;
    ns = w->ns;
    unlock();
    switch (cmd)
    {
    case SW_HIDE: show = false; activate = false; minimize = false; break;
    case SW_SHOWNORMAL: case SW_SHOW: case SW_SHOWDEFAULT: case SW_SHOWMAXIMIZED: case SW_RESTORE:
        show = true; activate = true; minimize = false; break;
    case SW_SHOWNOACTIVATE: case SW_SHOWNA: show = true; activate = false; minimize = false; break;
    case SW_SHOWMINIMIZED: case SW_MINIMIZE: case SW_SHOWMINNOACTIVE: show = true; activate = false; minimize = true; break;
    default: rt_fatal("ShowWindow: command %d is not supported", cmd);
    }
    if (show != was_visible) send_msg(hwnd, WM_SHOWWINDOW, show ? 1u : 0u, 0);
    lock();
    w = window_of(hwnd);
    if (w != NULL)
    {
        w->visible = show;
        w->style = show ? (w->style | WS_VISIBLE) : (w->style & ~WS_VISIBLE);
    }
    unlock();
    if (w == NULL) return was_visible;
    rt_cocoa_window_show(ns, show, activate);
    if (!show)
        set_active(hwnd, false);
    else if (minimize && !minimized)
        rt_cocoa_window_minimize(ns);
    else if (activate)
        set_active(hwnd, true);
    return was_visible;
}

/* BOOL ShowWindow(HWND, int cmd): returns whether the window was visible. */
void rt_ShowWindow(_cpu *cpu)
{
    int r = show_window(ARG(0), (int)ARG(1));
    if (r < 0)
    {
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    RET((uint32_t)r);
}

/* HWND FindWindowExA(HWND parent, HWND after, LPCSTR class, LPCSTR title)
   Only the windows of this process exist for the runtime. */
void rt_FindWindowExA(_cpu *cpu)
{
    uint32_t parent = ARG(0), after = ARG(1), cname = ARG(2), title = ARG(3), r = 0;
    char text[256];
    int i, start = 1, ci = -1;
    if (parent != 0 && parent != HWND_DESKTOP_VALUE) rt_fatal("FindWindowExA: child window search is not supported");
    gtext("FindWindowExA", title, text, sizeof text);
    lock();
    if (after != 0)
    {
        int a = slot_of(after);
        if (a <= 0)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
            RET(0);
            return;
        }
        start = a + 1;
    }
    if (cname != 0)
    {
        ci = find_class(cname, 0, true);
        if (ci < 0)
        {
            unlock();
            RET(0);
            return;
        }
    }
    for (i = start; i < MAX_WINDOWS; i++)
    {
        if (!windows[i].used || windows[i].destroying) continue;
        if (ci >= 0 && windows[i].cls != ci) continue;
        if (title != 0 && strcasecmp(windows[i].title, text) != 0) continue;
        r = hwnd_of(i);
        break;
    }
    unlock();
    RET(r);
}

/* LONG GetWindowLongA(HWND, int index) */
void rt_GetWindowLongA(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), v = 0;
    int32_t index = (int32_t)ARG(1);
    wnd *w;
    lock();
    w = window_of(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    switch (index)
    {
    case GWL_WNDPROC: v = w->wndproc; break;
    case GWL_HINSTANCE: v = w->hinstance; break;
    case GWL_HWNDPARENT: v = 0; break;
    case GWL_ID: v = w->id; break;
    case GWL_STYLE: v = w->style; break;
    case GWL_EXSTYLE: v = w->exstyle; break;
    case GWL_USERDATA: v = w->userdata; break;
    default:
        if (index < 0 || (uint32_t)index + 4u > w->extra_size)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_INDEX);
            RET(0);
            return;
        }
        memcpy(&v, w->extra + index, 4);
    }
    unlock();
    RET(v);
}

/* LONG SetWindowLongA(HWND, int index, LONG value): returns the old value. */
void rt_SetWindowLongA(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), value = ARG(2), old = 0;
    int32_t index = (int32_t)ARG(1);
    wnd *w;
    if (index == GWL_STYLE || index == GWL_EXSTYLE || index == GWL_HWNDPARENT)
        rt_fatal("SetWindowLongA: index %d (style or parent change) is not supported", index);
    lock();
    w = window_of(hwnd);
    if (w == NULL)
    {
        unlock();
        rt_set_last_error(ERROR_INVALID_WINDOW_HANDLE);
        RET(0);
        return;
    }
    switch (index)
    {
    case GWL_WNDPROC:
        if (value == 0)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_PARAMETER_U);
            RET(0);
            return;
        }
        old = w->wndproc;
        w->wndproc = value;
        break;
    case GWL_HINSTANCE: old = w->hinstance; w->hinstance = value; break;
    case GWL_ID: old = w->id; w->id = value; break;
    case GWL_USERDATA: old = w->userdata; w->userdata = value; break;
    default:
        if (index < 0 || (uint32_t)index + 4u > w->extra_size)
        {
            unlock();
            rt_set_last_error(ERROR_INVALID_INDEX);
            RET(0);
            return;
        }
        memcpy(&old, w->extra + index, 4);
        memcpy(w->extra + index, &value, 4);
    }
    unlock();
    rt_set_last_error(0);
    RET(old);
}

/* LRESULT CallWindowProcA(WNDPROC, HWND, UINT, WPARAM, LPARAM) */
void rt_CallWindowProcA(_cpu *cpu)
{
    uint32_t proc = ARG(0);
    if (proc == 0)
    {
        RET(0);
        return;
    }
    RET(rt_call(cpu, proc, true, 0, 4, (const uint32_t[]){ ARG(1), ARG(2), ARG(3), ARG(4) }));
}

/* ---------------------------------------------------------------- default window procedure */

static void minimize_window(uint32_t hwnd, bool minimize)
{
    void *ns;
    wnd *w;
    lock();
    w = window_of(hwnd);
    ns = w != NULL ? w->ns : NULL;
    unlock();
    if (ns == NULL) return;
    if (minimize)
        rt_cocoa_window_minimize(ns);
    else
        rt_cocoa_window_show(ns, true, true);
}

/* LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM) */
void rt_DefWindowProcA(_cpu *cpu)
{
    uint32_t hwnd = ARG(0), msg = ARG(1), wp = ARG(2), lp = ARG(3);
    static bool logged[WM_USER];
    switch (msg)
    {
    case WM_NCCREATE:
        RET(1);
        return;
    case WM_CLOSE:
        if (rt_cocoa_is_main_thread()) destroy_window(hwnd);
        RET(0);
        return;
    case WM_SYSCOMMAND:
        switch (wp & 0xFFF0u)
        {
        case SC_CLOSE: send_msg(hwnd, WM_CLOSE, 0, 0); break;
        case SC_MINIMIZE: minimize_window(hwnd, true); break;
        case SC_RESTORE: minimize_window(hwnd, false); break;
        case SC_KEYMENU: break;       /* the windows have no menus */
        case SC_SCREENSAVE: break;    /* macOS starts the screen saver itself */
        case SC_MONITORPOWER: break;  /* macOS controls display sleep itself */
        default: rt_fatal("DefWindowProcA: WM_SYSCOMMAND 0x%04x is not supported", wp & 0xFFF0u);
        }
        RET(0);
        return;
    case WM_SYSKEYDOWN:
        /* Alt+F4 closes the window. */
        if (wp == VK_F4 && (lp & (1u << 29))) send_msg(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
        RET(0);
        return;
    case WM_NCACTIVATE:
    case WM_ERASEBKGND:
        RET(1);
        return;
    case WM_MOUSEACTIVATE:
        RET(1); /* MA_ACTIVATE */
        return;
    case WM_NCHITTEST:
        RET(1); /* HTCLIENT */
        return;
    case WM_GETTEXTLENGTH:
    {
        wnd *w;
        size_t n = 0;
        lock();
        w = window_of(hwnd);
        if (w != NULL) n = strlen(w->title);
        unlock();
        RET(n);
        return;
    }
    case WM_GETTEXT:
    {
        wnd *w;
        size_t n = 0;
        if (wp == 0 || lp == 0)
        {
            RET(0);
            return;
        }
        lock();
        w = window_of(hwnd);
        if (w != NULL)
        {
            n = strlen(w->title);
            if (n > wp - 1) n = wp - 1;
            memcpy(G2H(lp), w->title, n);
        }
        unlock();
        wr8(lp + (uint32_t)n, 0);
        RET(n);
        return;
    }
    case WM_SETTEXT:
        rt_fatal("DefWindowProcA: WM_SETTEXT (title change) is not supported");
    case WM_CREATE: case WM_DESTROY: case WM_NCDESTROY: case WM_MOVE: case WM_SIZE:
    case WM_ACTIVATE: case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_PAINT:
    case WM_QUIT: case WM_SHOWWINDOW: case WM_ACTIVATEAPP: case WM_SETCURSOR:
    case WM_GETMINMAXINFO: case WM_WINDOWPOSCHANGING: case WM_WINDOWPOSCHANGED:
    case WM_NCCALCSIZE: case WM_NCPAINT:
    case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR: case WM_DEADCHAR:
    case WM_SYSKEYUP: case WM_SYSCHAR: case WM_SYSDEADCHAR: case WM_TIMER:
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
    case WM_MOUSEWHEEL: case WM_NULL:
        RET(0); /* no default action */
        return;
    default:
        if (msg < WM_USER && !logged[msg])
        {
            logged[msg] = true;
            rt_log("DefWindowProcA: message 0x%04x has no default action in the runtime (result 0)", msg);
        }
        RET(0);
        return;
    }
}

/* ---------------------------------------------------------------- messages */

/* MSG (28 bytes): hwnd, message, wParam, lParam, time, pt.x, pt.y. */
static void write_msg(uint32_t g, const qmsg *m)
{
    wr32(g, m->hwnd);
    wr32(g + 4, m->msg);
    wr32(g + 8, m->wparam);
    wr32(g + 12, m->lparam);
    wr32(g + 16, m->time);
    wr32(g + 20, (uint32_t)m->x);
    wr32(g + 24, (uint32_t)m->y);
}

static bool msg_matches(const qmsg *m, uint32_t hwnd, uint32_t lo, uint32_t hi)
{
    if (hwnd == 0xFFFFFFFFu ? m->hwnd != 0 : (hwnd != 0 && m->hwnd != hwnd)) return false;
    if (lo == 0 && hi == 0) return true;
    return m->msg >= lo && m->msg <= hi;
}

/* GetKeyState state for a removed message (lock held). */
static void sync_key_state(const qmsg *m)
{
    uint32_t vk = 0;
    bool down = false;
    switch (m->msg)
    {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: vk = m->wparam & 0xFFu; down = true; break;
    case WM_KEYUP: case WM_SYSKEYUP: vk = m->wparam & 0xFFu; down = false; break;
    case WM_LBUTTONDOWN: vk = VK_LBUTTON; down = true; break;
    case WM_LBUTTONUP: vk = VK_LBUTTON; down = false; break;
    case WM_RBUTTONDOWN: vk = VK_RBUTTON; down = true; break;
    case WM_RBUTTONUP: vk = VK_RBUTTON; down = false; break;
    case WM_MBUTTONDOWN: vk = VK_MBUTTON; down = true; break;
    case WM_MBUTTONUP: vk = VK_MBUTTON; down = false; break;
    default: return;
    }
    if (down && !(m->lparam & (1u << 30)) && !(sync_state[vk] & 0x80)) sync_state[vk] ^= 0x01;
    sync_state[vk] = (uint8_t)((sync_state[vk] & 0x01) | (down ? 0x80 : 0));
    /* The left and right modifier keys: the extended bit selects right. */
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
    {
        uint32_t scan = (m->lparam >> 16) & 0xFFu;
        bool right = vk == VK_SHIFT ? scan == 0x36 : (m->lparam & (1u << 24)) != 0;
        uint32_t side = vk == VK_SHIFT ? (right ? VK_RSHIFT : VK_LSHIFT)
                      : vk == VK_CONTROL ? (right ? VK_RCONTROL : VK_LCONTROL)
                      : (right ? VK_RMENU : VK_LMENU);
        sync_state[side] = (uint8_t)((sync_state[side] & 0x01) | (down ? 0x80 : 0));
        if (!down)
        {
            uint32_t other = vk == VK_SHIFT ? (right ? VK_LSHIFT : VK_RSHIFT)
                           : vk == VK_CONTROL ? (right ? VK_LCONTROL : VK_RCONTROL)
                           : (right ? VK_LMENU : VK_RMENU);
            if (sync_state[other] & 0x80) sync_state[vk] |= 0x80; /* the other key is still down */
        }
    }
}

/* BOOL PeekMessageA(LPMSG, HWND, UINT min, UINT max, UINT remove) */
void rt_PeekMessageA(_cpu *cpu)
{
    uint32_t pmsg = ARG(0), hwnd = ARG(1), lo = ARG(2), hi = ARG(3), flags = ARG(4);
    bool remove = (flags & PM_REMOVE) != 0;
    queue *qs[2] = { &posted_q, &input_q };
    int k;
    if (flags & ~(PM_REMOVE | PM_NOYIELD)) rt_fatal("PeekMessageA: flags 0x%x are not supported", flags);
    if (pmsg == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    if (rt_cocoa_is_main_thread())
    {
        rt_cocoa_pump();
        lock();
        for (k = 0; k < 2; k++)
        {
            uint32_t i;
            for (i = 0; i < qs[k]->count; i++)
            {
                qmsg m = *queue_at(qs[k], i);
                if (!msg_matches(&m, hwnd, lo, hi)) continue;
                if (remove)
                {
                    queue_remove(qs[k], i);
                    sync_key_state(&m);
                    if (m.msg == WM_KEYDOWN || m.msg == WM_SYSKEYDOWN)
                    {
                        last_key = m;
                        last_key_valid = true;
                    }
                }
                unlock();
                write_msg(pmsg, &m);
                RET(1);
                return;
            }
        }
        unlock();
    }
    if (quit_posted && hwnd != 0xFFFFFFFFu ? (lo == 0 && hi == 0) || (WM_QUIT >= lo && WM_QUIT <= hi) : false)
    {
        qmsg m;
        memset(&m, 0, sizeof m);
        m.msg = WM_QUIT;
        m.wparam = quit_code;
        m.time = now_ms();
        lock();
        m.x = mouse_x;
        m.y = mouse_y;
        unlock();
        if (remove) quit_posted = false;
        write_msg(pmsg, &m);
        RET(1);
        return;
    }
    RET(0);
}

/* void PostQuitMessage(int code) */
void rt_PostQuitMessage(_cpu *cpu)
{
    quit_posted = true;
    quit_code = ARG(0);
}

/* BOOL TranslateMessage(const MSG *): WM_CHAR or WM_SYSCHAR for the
   characters of a key-down message. */
void rt_TranslateMessage(_cpu *cpu)
{
    uint32_t p = ARG(0), msg;
    qmsg key, ch;
    int i;
    if (p == 0)
    {
        RET(0);
        return;
    }
    msg = rd32(p + 4);
    if (msg != WM_KEYDOWN && msg != WM_SYSKEYDOWN && msg != WM_KEYUP && msg != WM_SYSKEYUP)
    {
        RET(0);
        return;
    }
    if (msg == WM_KEYUP || msg == WM_SYSKEYUP)
    {
        RET(1);
        return;
    }
    lock();
    if (!last_key_valid || last_key.hwnd != rd32(p) || last_key.msg != msg || last_key.wparam != rd32(p + 8) ||
        last_key.lparam != rd32(p + 12) || last_key.time != rd32(p + 16))
    {
        unlock();
        RET(1); /* not a message of the queue: no characters */
        return;
    }
    key = last_key;
    last_key_valid = false;
    /* Insert at the front in reverse order, so that the characters come
       next and in order. */
    for (i = key.nchars - 1; i >= 0; i--)
    {
        uint16_t c = key.chars[i];
        int b;
        if (c >= 0xF700 && c <= 0xF8FF) continue; /* function keys: no character */
        if (c == 0x7F) c = 0x08;                  /* the Delete key is Backspace */
        if (c == 0x03) c = 0x0D;                  /* keypad Enter */
        b = unicode_to_ansi(c);
        if (b < 0) b = '?';                       /* no Windows-1252 byte: the default character */
        ch = key;
        ch.msg = msg == WM_SYSKEYDOWN ? WM_SYSCHAR : WM_CHAR;
        ch.wparam = (uint32_t)b;
        ch.nchars = 0;
        if (!queue_push(&posted_q, &ch, true))
            rt_log("USER32: the message queue is full; character message 0x%02x was discarded", (unsigned)b);
    }
    unlock();
    RET(1);
}

/* LRESULT DispatchMessageA(const MSG *) */
void rt_DispatchMessageA(_cpu *cpu)
{
    uint32_t p = ARG(0), hwnd, msg;
    if (p == 0)
    {
        RET(0);
        return;
    }
    hwnd = rd32(p);
    msg = rd32(p + 4);
    if (msg == WM_TIMER && rd32(p + 12) != 0) rt_fatal("DispatchMessageA: WM_TIMER callbacks are not supported");
    if (hwnd == 0)
    {
        RET(0);
        return;
    }
    RET(send_msg(hwnd, msg, rd32(p + 8), rd32(p + 12)));
}

/* ---------------------------------------------------------------- keyboard */

typedef struct keymap {
    uint8_t vk, scan, ext, ch; /* ch: MAPVK_VK_TO_CHAR value (US layout) */
} keymap;

/* macOS virtual key code (kVK_*) to the US-layout Windows key. */
static const keymap keys[128] = {
    [0x00] = { 'A', 0x1E, 0, 'A' }, [0x01] = { 'S', 0x1F, 0, 'S' }, [0x02] = { 'D', 0x20, 0, 'D' },
    [0x03] = { 'F', 0x21, 0, 'F' }, [0x04] = { 'H', 0x23, 0, 'H' }, [0x05] = { 'G', 0x22, 0, 'G' },
    [0x06] = { 'Z', 0x2C, 0, 'Z' }, [0x07] = { 'X', 0x2D, 0, 'X' }, [0x08] = { 'C', 0x2E, 0, 'C' },
    [0x09] = { 'V', 0x2F, 0, 'V' }, [0x0A] = { 0xE2, 0x56, 0, '\\' }, [0x0B] = { 'B', 0x30, 0, 'B' },
    [0x0C] = { 'Q', 0x10, 0, 'Q' }, [0x0D] = { 'W', 0x11, 0, 'W' }, [0x0E] = { 'E', 0x12, 0, 'E' },
    [0x0F] = { 'R', 0x13, 0, 'R' }, [0x10] = { 'Y', 0x15, 0, 'Y' }, [0x11] = { 'T', 0x14, 0, 'T' },
    [0x12] = { '1', 0x02, 0, '1' }, [0x13] = { '2', 0x03, 0, '2' }, [0x14] = { '3', 0x04, 0, '3' },
    [0x15] = { '4', 0x05, 0, '4' }, [0x16] = { '6', 0x07, 0, '6' }, [0x17] = { '5', 0x06, 0, '5' },
    [0x18] = { 0xBB, 0x0D, 0, '=' }, [0x19] = { '9', 0x0A, 0, '9' }, [0x1A] = { '7', 0x08, 0, '7' },
    [0x1B] = { 0xBD, 0x0C, 0, '-' }, [0x1C] = { '8', 0x09, 0, '8' }, [0x1D] = { '0', 0x0B, 0, '0' },
    [0x1E] = { 0xDD, 0x1B, 0, ']' }, [0x1F] = { 'O', 0x18, 0, 'O' }, [0x20] = { 'U', 0x16, 0, 'U' },
    [0x21] = { 0xDB, 0x1A, 0, '[' }, [0x22] = { 'I', 0x17, 0, 'I' }, [0x23] = { 'P', 0x19, 0, 'P' },
    [0x24] = { VK_RETURN, 0x1C, 0, '\r' }, [0x25] = { 'L', 0x26, 0, 'L' }, [0x26] = { 'J', 0x24, 0, 'J' },
    [0x27] = { 0xDE, 0x28, 0, '\'' }, [0x28] = { 'K', 0x25, 0, 'K' }, [0x29] = { 0xBA, 0x27, 0, ';' },
    [0x2A] = { 0xDC, 0x2B, 0, '\\' }, [0x2B] = { 0xBC, 0x33, 0, ',' }, [0x2C] = { 0xBF, 0x35, 0, '/' },
    [0x2D] = { 'N', 0x31, 0, 'N' }, [0x2E] = { 'M', 0x32, 0, 'M' }, [0x2F] = { 0xBE, 0x34, 0, '.' },
    [0x30] = { VK_TAB, 0x0F, 0, '\t' }, [0x31] = { ' ', 0x39, 0, ' ' }, [0x32] = { 0xC0, 0x29, 0, '`' },
    [0x33] = { VK_BACK, 0x0E, 0, '\b' }, [0x35] = { VK_ESCAPE, 0x01, 0, 0x1B },
    [0x36] = { 0x5C, 0x5C, 1, 0 },       /* right Command: VK_RWIN */
    [0x37] = { 0x5B, 0x5B, 1, 0 },       /* left Command: VK_LWIN */
    [0x38] = { VK_SHIFT, 0x2A, 0, 0 }, [0x39] = { VK_CAPITAL, 0x3A, 0, 0 }, [0x3A] = { VK_MENU, 0x38, 0, 0 },
    [0x3B] = { VK_CONTROL, 0x1D, 0, 0 }, [0x3C] = { VK_SHIFT, 0x36, 0, 0 }, [0x3D] = { VK_MENU, 0x38, 1, 0 },
    [0x3E] = { VK_CONTROL, 0x1D, 1, 0 },
    [0x41] = { 0x6E, 0x53, 0, '.' }, [0x43] = { 0x6A, 0x37, 0, '*' }, [0x45] = { 0x6B, 0x4E, 0, '+' },
    [0x47] = { VK_CLEAR, 0x4C, 0, 0 },   /* keypad Clear (the NumLock position) */
    [0x4B] = { 0x6F, 0x35, 1, '/' }, [0x4C] = { VK_RETURN, 0x1C, 1, '\r' }, [0x4E] = { 0x6D, 0x4A, 0, '-' },
    [0x52] = { 0x60, 0x52, 0, '0' }, [0x53] = { 0x61, 0x4F, 0, '1' }, [0x54] = { 0x62, 0x50, 0, '2' },
    [0x55] = { 0x63, 0x51, 0, '3' }, [0x56] = { 0x64, 0x4B, 0, '4' }, [0x57] = { 0x65, 0x4C, 0, '5' },
    [0x58] = { 0x66, 0x4D, 0, '6' }, [0x59] = { 0x67, 0x47, 0, '7' }, [0x5B] = { 0x68, 0x48, 0, '8' },
    [0x5C] = { 0x69, 0x49, 0, '9' },
    [0x60] = { 0x74, 0x3F, 0, 0 }, [0x61] = { 0x75, 0x40, 0, 0 }, [0x62] = { 0x76, 0x41, 0, 0 },
    [0x63] = { 0x72, 0x3D, 0, 0 }, [0x64] = { 0x77, 0x42, 0, 0 }, [0x65] = { 0x78, 0x43, 0, 0 },
    [0x67] = { 0x7A, 0x57, 0, 0 }, [0x69] = { 0x7C, 0x64, 0, 0 }, [0x6B] = { 0x7D, 0x65, 0, 0 },
    [0x6D] = { 0x79, 0x44, 0, 0 }, [0x6F] = { 0x7B, 0x58, 0, 0 }, [0x71] = { 0x7E, 0x66, 0, 0 },
    [0x72] = { 0x2D, 0x52, 1, 0 },       /* Help: VK_INSERT */
    [0x73] = { 0x24, 0x47, 1, 0 }, [0x74] = { 0x21, 0x49, 1, 0 }, [0x75] = { 0x2E, 0x53, 1, 0 },
    [0x76] = { 0x73, 0x3E, 0, 0 }, [0x77] = { 0x23, 0x4F, 1, 0 }, [0x78] = { 0x71, 0x3C, 0, 0 },
    [0x79] = { 0x22, 0x51, 1, 0 }, [0x7A] = { 0x70, 0x3B, 0, 0 }, [0x7B] = { 0x25, 0x4B, 1, 0 },
    [0x7C] = { 0x27, 0x4D, 1, 0 }, [0x7D] = { 0x28, 0x50, 1, 0 }, [0x7E] = { 0x26, 0x48, 1, 0 },
};

/* The specific key (left or right) for the generic modifier codes. */
static uint32_t side_vk(const keymap *k)
{
    if (k->vk == VK_SHIFT) return k->scan == 0x36 ? VK_RSHIFT : VK_LSHIFT;
    if (k->vk == VK_CONTROL) return k->ext ? VK_RCONTROL : VK_LCONTROL;
    if (k->vk == VK_MENU) return k->ext ? VK_RMENU : VK_LMENU;
    return k->vk;
}

static bool alt_down(void)
{
    return (input_down[VK_LMENU] | input_down[VK_RMENU]) != 0;
}

static bool ctrl_down(void)
{
    return (input_down[VK_LCONTROL] | input_down[VK_RCONTROL]) != 0;
}

static void key_input(uint32_t hwnd, bool down, bool is_repeat, uint16_t code, const uint16_t *chars, int nchars)
{
    const keymap *k;
    qmsg m;
    uint32_t side, prev, lp;
    bool sys;
    if (code >= 128 || keys[code].vk == 0)
    {
        static bool logged[256];
        if (!logged[code & 0xFFu])
        {
            logged[code & 0xFFu] = true;
            rt_log("USER32: macOS key code 0x%02x has no Windows key; the key is ignored", code);
        }
        return;
    }
    k = &keys[code];
    rt_dinput_on_key((uint8_t)(k->scan | (k->ext ? 0x80u : 0u)), down);
    side = side_vk(k);
    lock();
    prev = input_down[side] ? 1u : 0u;
    input_down[side] = down ? 1 : 0;
    if (side != k->vk)
    {
        /* The generic code is down while one of the two keys is down. */
        uint32_t a = k->vk == VK_SHIFT ? VK_LSHIFT : k->vk == VK_CONTROL ? VK_LCONTROL : VK_LMENU;
        input_down[k->vk] = (input_down[a] | input_down[a + 1]) ? 1 : 0;
    }
    /* Alt (without Ctrl) and F10 give system key messages. */
    sys = (alt_down() && !ctrl_down()) || k->vk == VK_F10 || (k->vk == VK_MENU && !ctrl_down());
    lp = 1u | ((uint32_t)k->scan << 16) | ((uint32_t)k->ext << 24);
    if (alt_down()) lp |= 1u << 29;
    if (down ? (prev || is_repeat) : true) lp |= 1u << 30;
    if (!down) lp |= 1u << 31;
    memset(&m, 0, sizeof m);
    m.hwnd = hwnd;
    m.msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    m.wparam = k->vk;
    m.lparam = lp;
    m.time = now_ms();
    m.x = mouse_x;
    m.y = mouse_y;
    if (down && chars != NULL)
    {
        int i;
        for (i = 0; i < nchars && i < 16; i++) m.chars[i] = chars[i];
        m.nchars = (uint8_t)(nchars < 16 ? nchars : 16);
    }
    unlock();
    post_input(&m);
}

int rt_user32_dik_codes(uint8_t out[256])
{
    bool seen[256] = { false };
    int i, n = 0;
    for (i = 0; i < 128; i++)
        if (keys[i].vk != 0) seen[keys[i].scan | (keys[i].ext ? 0x80u : 0u)] = true;
    for (i = 0; i < 256; i++)
        if (seen[i]) out[n++] = (uint8_t)i;
    return n;
}

void rt_user32_on_key(uint32_t hwnd, bool down, bool is_repeat, uint16_t mac_keycode, const uint16_t *chars, int nchars)
{
    key_input(hwnd, down, is_repeat, mac_keycode, chars, nchars);
}

/* NSEvent device-dependent modifier bits and their macOS key codes. */
static const struct { uint32_t bit; uint16_t code; } mod_keys[] = {
    { 0x00000001u, 0x3B }, /* left Control */
    { 0x00000002u, 0x38 }, /* left Shift */
    { 0x00000004u, 0x3C }, /* right Shift */
    { 0x00000008u, 0x37 }, /* left Command */
    { 0x00000010u, 0x36 }, /* right Command */
    { 0x00000020u, 0x3A }, /* left Option */
    { 0x00000040u, 0x3D }, /* right Option */
    { 0x00002000u, 0x3E }, /* right Control */
};
#define MAC_CAPS_LOCK 0x00010000u

void rt_user32_on_modifiers(uint32_t hwnd, uint32_t flags)
{
    uint32_t changed;
    size_t i;
    lock();
    changed = flags ^ last_mods;
    last_mods = flags;
    unlock();
    for (i = 0; i < sizeof mod_keys / sizeof mod_keys[0]; i++)
        if (changed & mod_keys[i].bit) key_input(hwnd, (flags & mod_keys[i].bit) != 0, false, mod_keys[i].code, NULL, 0);
    /* macOS reports one event for each Caps Lock change: a press and a
       release on Windows. */
    if (changed & MAC_CAPS_LOCK)
    {
        key_input(hwnd, true, false, 0x39, NULL, 0);
        key_input(hwnd, false, false, 0x39, NULL, 0);
    }
}

/* SHORT GetKeyState(int vk) */
void rt_GetKeyState(_cpu *cpu)
{
    uint32_t vk = ARG(0) & 0xFFu;
    uint16_t r;
    if (vk == VK_CAPITAL)
    {
        lock();
        r = (uint16_t)((sync_state[vk] & 0x80) ? 0xFF80u : 0);
        unlock();
        if (rt_cocoa_caps_lock()) r |= 1;
    }
    else if (vk == VK_NUMLOCK)
    {
        r = 1; /* Mac keypads always give digits: Num Lock is on */
    }
    else if (vk == VK_SCROLL)
    {
        r = 0;
    }
    else
    {
        lock();
        r = (uint16_t)(((sync_state[vk] & 0x80) ? 0xFF80u : 0) | (sync_state[vk] & 0x01));
        unlock();
    }
    RET((uint32_t)(int32_t)(int16_t)r);
}

/* HKL GetKeyboardLayout(DWORD thread) */
void rt_GetKeyboardLayout(_cpu *cpu)
{
    RET(US_KEYBOARD_LAYOUT);
}

/* UINT MapVirtualKeyExA(UINT code, UINT type, HKL) */
void rt_MapVirtualKeyExA(_cpu *cpu)
{
    uint32_t code = ARG(0), type = ARG(1), hkl = ARG(2), i;
    if (hkl != US_KEYBOARD_LAYOUT && hkl != 0) rt_fatal("MapVirtualKeyExA: keyboard layout 0x%08x is not supported", hkl);
    switch (type)
    {
    case 0: /* MAPVK_VK_TO_VSC */
        if (code == VK_LSHIFT || code == VK_SHIFT) { RET(0x2A); return; }
        if (code == VK_RSHIFT) { RET(0x36); return; }
        if (code == VK_LCONTROL || code == VK_RCONTROL) { RET(0x1D); return; }
        if (code == VK_LMENU || code == VK_RMENU) { RET(0x38); return; }
        for (i = 0; i < 128; i++)
            if (keys[i].vk != 0 && keys[i].vk == code) { RET(keys[i].scan); return; }
        RET(0);
        return;
    case 1: /* MAPVK_VSC_TO_VK */
    case 3: /* MAPVK_VSC_TO_VK_EX */
        if (type == 3 && code == 0x2A) { RET(VK_LSHIFT); return; }
        if (type == 3 && code == 0x36) { RET(VK_RSHIFT); return; }
        if (type == 3 && code == 0x1D) { RET(VK_LCONTROL); return; }
        if (type == 3 && code == 0x38) { RET(VK_LMENU); return; }
        for (i = 0; i < 128; i++)
            if (keys[i].vk != 0 && keys[i].scan == code && !keys[i].ext) { RET(keys[i].vk); return; }
        RET(0);
        return;
    case 2: /* MAPVK_VK_TO_CHAR */
        for (i = 0; i < 128; i++)
            if (keys[i].vk != 0 && keys[i].vk == code) { RET(keys[i].ch); return; }
        RET(0);
        return;
    default:
        rt_fatal("MapVirtualKeyExA: map type %u is not supported", type);
    }
}

/* ---------------------------------------------------------------- mouse */

static uint32_t mouse_keys(void)
{
    uint32_t mk = 0;
    if (input_down[VK_LBUTTON]) mk |= MK_LBUTTON;
    if (input_down[VK_RBUTTON]) mk |= MK_RBUTTON;
    if (input_down[VK_MBUTTON]) mk |= MK_MBUTTON;
    if (input_down[VK_LSHIFT] | input_down[VK_RSHIFT]) mk |= MK_SHIFT;
    if (ctrl_down()) mk |= MK_CONTROL;
    return mk;
}

void rt_user32_on_mouse(uint32_t hwnd, uint32_t msg, int32_t x, int32_t y, int32_t wheel)
{
    qmsg m;
    rt_rect c;
    wnd *w;
    lock();
    w = window_of(hwnd);
    if (w == NULL)
    {
        unlock();
        return;
    }
    c = w->crect;
    mouse_x = c.left + x;
    mouse_y = c.top + y;
    switch (msg)
    {
    case WM_LBUTTONDOWN: input_down[VK_LBUTTON] = 1; break;
    case WM_LBUTTONUP: input_down[VK_LBUTTON] = 0; break;
    case WM_RBUTTONDOWN: input_down[VK_RBUTTON] = 1; break;
    case WM_RBUTTONUP: input_down[VK_RBUTTON] = 0; break;
    case WM_MBUTTONDOWN: input_down[VK_MBUTTON] = 1; break;
    case WM_MBUTTONUP: input_down[VK_MBUTTON] = 0; break;
    default: break;
    }
    memset(&m, 0, sizeof m);
    m.hwnd = hwnd;
    m.msg = msg;
    m.time = now_ms();
    m.x = mouse_x;
    m.y = mouse_y;
    if (msg == WM_MOUSEWHEEL)
    {
        /* WM_MOUSEWHEEL has screen coordinates. */
        m.wparam = mouse_keys() | ((uint32_t)(uint16_t)(int16_t)wheel << 16);
        m.lparam = make_lparam(mouse_x, mouse_y);
    }
    else
    {
        m.wparam = mouse_keys();
        m.lparam = make_lparam(x, y);
    }
    unlock();
    post_input(&m);
}

/* int ShowCursor(BOOL show): returns the new display count. */
void rt_ShowCursor(_cpu *cpu)
{
    int32_t before, after;
    lock();
    before = cursor_count;
    if (ARG(0) != 0)
    {
        if (cursor_count == INT32_MAX) rt_fatal("ShowCursor count overflow");
        cursor_count++;
    }
    else
    {
        if (cursor_count == INT32_MIN) rt_fatal("ShowCursor count overflow");
        cursor_count--;
    }
    after = cursor_count;
    unlock();
    if (before >= 0 && after < 0) rt_cocoa_cursor_visible(false);
    if (before < 0 && after >= 0) rt_cocoa_cursor_visible(true);
    RET((uint32_t)after);
}

/* ---------------------------------------------------------------- native events */

void rt_user32_on_active(uint32_t hwnd, bool active)
{
    set_active(hwnd, active);
    rt_dinput_on_focus(hwnd, active);
}

void rt_user32_on_close(uint32_t hwnd)
{
    send_msg(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
}

void rt_user32_on_quit(void)
{
    int i;
    for (i = 1; i < MAX_WINDOWS; i++)
    {
        uint32_t hwnd = 0;
        lock();
        if (windows[i].used && !windows[i].destroying) hwnd = hwnd_of(i);
        unlock();
        if (hwnd != 0) send_msg(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
    }
}

void rt_user32_on_frame(uint32_t hwnd, bool resized)
{
    rt_rect c;
    wnd *w;
    sync_rects(hwnd);
    lock();
    w = window_of(hwnd);
    if (w == NULL || w->minimized)
    {
        unlock();
        return;
    }
    c = w->crect;
    unlock();
    if (resized) send_msg(hwnd, WM_SIZE, SIZE_RESTORED, make_lparam(c.right - c.left, c.bottom - c.top));
    send_msg(hwnd, WM_MOVE, 0, make_lparam(c.left, c.top));
}

void rt_user32_on_minimized(uint32_t hwnd, bool minimized)
{
    wnd *w;
    lock();
    w = window_of(hwnd);
    if (w != NULL) w->minimized = minimized;
    unlock();
    if (w == NULL) return;
    sync_rects(hwnd);
    send_size_move(hwnd);
}

/* ---------------------------------------------------------------- resources and system settings */

#define IDI_FIRST 32512u
#define IDI_LAST 32518u
#define IDC_ARROW 32512u
#define MAX_ICONS 32

static struct { uint32_t module, id; } icons[MAX_ICONS];

/* HICON LoadIconA(HINSTANCE, LPCSTR name)
   The icon is not drawn: macOS shows the application bundle icon. */
void rt_LoadIconA(_cpu *cpu)
{
    uint32_t hinst = ARG(0), name = ARG(1), i;
    if (name >= 0x10000u) rt_fatal("LoadIconA: icon names given as strings are not supported");
    if (hinst == 0 && (name < IDI_FIRST || name > IDI_LAST)) rt_fatal("LoadIconA: system icon %u is not supported", name);
    if (hinst != 0 && hinst != rt_exe_module()) rt_fatal("LoadIconA: icons of module 0x%08x are not supported", hinst);
    lock();
    for (i = 0; i < MAX_ICONS; i++)
    {
        if (icons[i].id == 0)
        {
            icons[i].module = hinst;
            icons[i].id = name;
        }
        if (icons[i].module == hinst && icons[i].id == name)
        {
            unlock();
            RET(0x00030000u + 4u * (i + 1));
            return;
        }
    }
    unlock();
    rt_fatal("LoadIconA: more than %d icons", MAX_ICONS);
}

/* HCURSOR LoadCursorA(HINSTANCE, LPCSTR name): the standard arrow cursor. */
void rt_LoadCursorA(_cpu *cpu)
{
    if (ARG(0) != 0 || ARG(1) != IDC_ARROW) rt_fatal("LoadCursorA: only the system arrow cursor (IDC_ARROW) is supported");
    RET(0x00040000u + 4u);
}

/* int GetSystemMetrics(int index) */
void rt_GetSystemMetrics(_cpu *cpu)
{
    int32_t w, h;
    uint32_t i = ARG(0);
    switch (i)
    {
    case 0: /* SM_CXSCREEN */
    case 1: /* SM_CYSCREEN */
        need_main(__func__);
        rt_cocoa_screen_size(&w, &h);
        RET((uint32_t)(i == 0 ? w : h));
        return;
    case 0x17: /* SM_SWAPBUTTON: macOS swaps the buttons before the events arrive */
        RET(0);
        return;
    default:
        rt_fatal("GetSystemMetrics: index %u is not supported", i);
    }
}

/* BOOL SystemParametersInfoA(UINT action, UINT param, PVOID pv, UINT ini)
   The keyboard accessibility settings (StickyKeys, ToggleKeys,
   FilterKeys) have no macOS equivalent. The runtime keeps the values for
   the process; SPIF_UPDATEINIFILE does not store them. */
void rt_SystemParametersInfoA(_cpu *cpu)
{
    uint32_t action = ARG(0), param = ARG(1), pv = ARG(2), i;
    uint32_t need = (action == 0x32 || action == 0x33) ? 24u : 8u;
    if (action != 0x32 && action != 0x33 && action != 0x34 && action != 0x35 && action != 0x3A && action != 0x3B)
        rt_fatal("SystemParametersInfoA: action 0x%x is not supported", action);
    if (pv == 0 || param != need || rd32(pv) != need)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER_U);
        RET(0);
        return;
    }
    lock();
    switch (action)
    {
    case 0x3A: wr32(pv + 4, sticky_flags); break;  /* SPI_GETSTICKYKEYS */
    case 0x3B: sticky_flags = rd32(pv + 4); break; /* SPI_SETSTICKYKEYS */
    case 0x34: wr32(pv + 4, toggle_flags); break;  /* SPI_GETTOGGLEKEYS */
    case 0x35: toggle_flags = rd32(pv + 4); break; /* SPI_SETTOGGLEKEYS */
    case 0x32:                                     /* SPI_GETFILTERKEYS */
        for (i = 1; i < 6; i++) wr32(pv + 4 * i, filter_keys[i]);
        break;
    case 0x33:                                     /* SPI_SETFILTERKEYS */
        for (i = 1; i < 6; i++) filter_keys[i] = rd32(pv + 4 * i);
        break;
    }
    unlock();
    RET(1);
}

/* DWORD CharLowerBuffA(LPSTR, DWORD len): Windows-1252 lower case. */
void rt_CharLowerBuffA(_cpu *cpu)
{
    uint32_t p = ARG(0), n = ARG(1), i;
    if (p == 0 || (n != 0 && !rt_va_is_committed(p, n)))
    {
        RET(0);
        return;
    }
    for (i = 0; i < n; i++)
    {
        uint8_t c = rd8(p + i);
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7))
            c = (uint8_t)(c + 0x20);
        else if (c == 0x8A || c == 0x8C || c == 0x8E)
            c = (uint8_t)(c + 0x10);
        else if (c == 0x9F)
            c = 0xFF;
        wr8(p + i, c);
    }
    RET(n);
}

/* ---------------------------------------------------------------- MessageBoxA */

#define MB_TYPEMASK 0x0000000Fu
#define MB_ICONMASK 0x000000F0u
#define MB_DEFMASK 0x00000F00u
#define MB_HELP 0x00004000u

/* int MessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type)
   CFUserNotification shows the alert; it works on any thread. */
void rt_MessageBoxA(_cpu *cpu)
{
    static const struct { const char *label[3]; uint32_t id[3]; int n; } sets[7] = {
        { { "OK", NULL, NULL }, { 1, 0, 0 }, 1 },                 /* MB_OK */
        { { "OK", "Cancel", NULL }, { 1, 2, 0 }, 2 },             /* MB_OKCANCEL */
        { { "Abort", "Retry", "Ignore" }, { 3, 4, 5 }, 3 },       /* MB_ABORTRETRYIGNORE */
        { { "Yes", "No", "Cancel" }, { 6, 7, 2 }, 3 },            /* MB_YESNOCANCEL */
        { { "Yes", "No", NULL }, { 6, 7, 0 }, 2 },                /* MB_YESNO */
        { { "Retry", "Cancel", NULL }, { 4, 2, 0 }, 2 },          /* MB_RETRYCANCEL */
        { { "Cancel", "Try Again", "Continue" }, { 2, 10, 11 }, 3 }, /* MB_CANCELTRYCONTINUE */
    };
    uint32_t type = ARG(3), set = type & MB_TYPEMASK, icon = type & MB_ICONMASK, def = (type & MB_DEFMASK) >> 8;
    char *text, *caption;
    size_t tn = 0, cn = 0;
    CFStringRef t, c, b[3] = { NULL, NULL, NULL };
    CFOptionFlags level, resp = 0;
    int order[3], i, n;
    SInt32 rc;
    if (set > 6) rt_fatal("MessageBoxA: type 0x%x is not supported", type);
    if (type & MB_HELP) rt_fatal("MessageBoxA: MB_HELP is not supported");
    n = sets[set].n;
    if ((int)def >= n) def = 0;
    /* The default button comes first (CFUserNotification default button). */
    order[0] = (int)def;
    for (i = 0, n = 1; i < sets[set].n; i++)
        if (i != (int)def) order[n++] = i;
    if (ARG(1) != 0) while (rd8(ARG(1) + (uint32_t)tn) != 0) tn++;
    if (ARG(2) != 0) while (rd8(ARG(2) + (uint32_t)cn) != 0) cn++;
    text = malloc(3 * tn + 1);
    caption = malloc(3 * cn + 6);
    if (text == NULL || caption == NULL) rt_fatal("out of host memory (MessageBoxA)");
    if (ARG(1) != 0) ansi_to_utf8(G2H(ARG(1)), text, 3 * tn + 1); else text[0] = 0;
    if (ARG(2) != 0) ansi_to_utf8(G2H(ARG(2)), caption, 3 * cn + 6); else memcpy(caption, "Error", 6);
    t = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    c = CFStringCreateWithCString(NULL, caption, kCFStringEncodingUTF8);
    free(text);
    free(caption);
    for (i = 0; i < n; i++)
        b[i] = CFStringCreateWithCString(NULL, sets[set].label[order[i]], kCFStringEncodingUTF8);
    if (t == NULL || c == NULL) rt_fatal("MessageBoxA: text conversion failed");
    level = icon == 0x10 ? kCFUserNotificationStopAlertLevel
          : icon == 0x30 ? kCFUserNotificationCautionAlertLevel
          : icon == 0x20 || icon == 0x40 ? kCFUserNotificationNoteAlertLevel
          : kCFUserNotificationPlainAlertLevel;
    rc = CFUserNotificationDisplayAlert(0, level, NULL, NULL, NULL, c, t, b[0], b[1], b[2], &resp);
    CFRelease(t);
    CFRelease(c);
    for (i = 0; i < n; i++) CFRelease(b[i]);
    if (rc != 0) rt_fatal("MessageBoxA: the alert could not be shown (error %d)", (int)rc);
    resp &= 3;
    if (resp == kCFUserNotificationDefaultResponse) { RET(sets[set].id[order[0]]); return; }
    if (resp == kCFUserNotificationAlternateResponse && n > 1) { RET(sets[set].id[order[1]]); return; }
    if (resp == kCFUserNotificationOtherResponse && n > 2) { RET(sets[set].id[order[2]]); return; }
    rt_fatal("MessageBoxA: unexpected alert response %lu", (unsigned long)resp);
}

/* ---------------------------------------------------------------- start */

/* Called by main before guest code runs. */
void rt_user32_init(void)
{
    need_main(__func__);
    rt_cocoa_init();
    queue_init(&posted_q);
    queue_init(&input_q);
    windows[0].used = true; /* the desktop window */
    if (rt_cocoa_caps_lock()) sync_state[VK_CAPITAL] = 0x01;
}

/* The CAMetalLayer of the content view of top-level window hwnd (main
   thread). Returns NULL if hwnd is not a top-level window. */
void *rt_user32_window_layer(uint32_t hwnd)
{
    void *ns;
    wnd *w;
    need_main(__func__);
    lock();
    w = window_of(hwnd);
    ns = w != NULL ? w->ns : NULL;
    unlock();
    return ns != NULL ? rt_cocoa_window_layer(ns) : NULL;
}

/* The client size of top-level window hwnd. Returns false if hwnd is not a
   top-level window. */
bool rt_user32_client_size(uint32_t hwnd, int32_t *w, int32_t *h)
{
    wnd *wp;
    bool ok;
    lock();
    wp = window_of(hwnd);
    ok = wp != NULL;
    if (ok)
    {
        *w = wp->crect.right - wp->crect.left;
        *h = wp->crect.bottom - wp->crect.top;
    }
    unlock();
    return ok;
}
