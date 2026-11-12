/*
 * DINPUT8: IDirectInput8A and IDirectInputDevice8A for the system keyboard
 * and the system mouse.
 *
 * The engine creates the object with CoCreateInstance(CLSID_DirectInput8,
 * IID_IDirectInput8A) and calls:
 *   IDirectInput8A::Initialize(hinst, 0x0800)
 *   IDirectInput8A::EnumDevices(DI8DEVCLASS_ALL, callback, ref, DIEDFL_ATTACHEDONLY)
 * The enumeration callback makes a device for each keyboard, mouse and game
 * controller that is not an XInput controller:
 *   IDirectInput8A::CreateDevice(guidInstance)
 *   IDirectInputDevice8A::EnumObjects(callback, ref, DIDFT_ALL)
 *   IDirectInputDevice8A::SetDataFormat (one DWORD for each object; pguid
 *       NULL, dwType = the dwType of the object)
 *   IDirectInputDevice8A::SetCooperativeLevel(hwnd, DISCL_NONEXCLUSIVE |
 *       DISCL_FOREGROUND)
 * Each frame: Acquire, Poll, GetDeviceState. The engine reads a button as
 * (byte & 0x80) and a relative axis as a LONG.
 *
 * The runtime enumerates two devices: the system mouse (GUID_SysMouse) and
 * the system keyboard (GUID_SysKeyboard). Game controllers are XInput
 * controllers (rt_xinput.m), which the engine reads through XInput only.
 *
 * Input comes from the window system (rt_dinput.h): keys from USER32 with
 * the scan code of the Mac key, mouse motion, buttons and the wheel from
 * the content view. A key or button press that is released again before
 * the next GetDeviceState call is reported down by that call, so that a
 * short press (and Caps Lock, which macOS reports as one change) is not
 * lost between two reads.
 *
 * Exclusive cooperative level: a keyboard needs no host change (only the
 * key window gets key events on macOS). While an exclusive mouse is
 * acquired, the cursor is hidden and does not move (rt_cocoa_mouse_capture),
 * as Windows hides and holds the cursor for an exclusive mouse. Exclusive
 * and background together give DIERR_UNSUPPORTED for these devices.
 * UNCONFIRMED: that result value (Wine's value).
 *
 * Not supported (the call stops the process with a message):
 * buffered data (GetDeviceData), properties, effects,
 * action maps and the methods that com.spec marks "-". The engine does not
 * call them for a keyboard or a mouse.
 *
 * UNCONFIRMED (not compared with Windows): the object names, the mouse
 * object instance numbers (axes 0-2, buttons from 3, as Wine has them),
 * the device type subtypes (0x413 keyboard, 0x112 mouse), guidProduct
 * equal to guidInstance, the enumeration order (mouse first), and the
 * dwOfs value that EnumObjects reports for an object that is not in the
 * data format (the native offset). The mouse motion unit is the macOS
 * event delta (points, with the system acceleration), not raw mickeys.
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"
#include "rt_dinput.h"
#include "rt_thread.h"
#include "rt_user32.h"
#include "rt_window.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* HRESULT values. */
#define DI_OK 0x00000000u
#define DI_NOEFFECT 0x00000001u
#define DIERR_OLDDIRECTINPUTVERSION 0x8007047Eu
#define DIERR_BETADIRECTINPUTVERSION 0x80070481u
#define DIERR_NOTINITIALIZED 0x80070015u
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_ACQUIRED 0x800700AAu
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_INPUTLOST 0x8007001Eu
#define DIERR_OTHERAPPHASPRIO 0x80070005u
#define DIERR_OUTOFMEMORY 0x8007000Eu
#define DIERR_UNSUPPORTED 0x80004001u
#define DIERR_NOAGGREGATION 0x80040110u

#define DIRECTINPUT_VERSION 0x0800u

/* EnumDevices device classes and types. */
#define DI8DEVCLASS_ALL 0u
#define DI8DEVCLASS_DEVICE 1u
#define DI8DEVCLASS_POINTER 2u
#define DI8DEVCLASS_KEYBOARD 3u
#define DI8DEVCLASS_GAMECTRL 4u
#define DI8DEVTYPE_FIRST 0x11u
#define DI8DEVTYPE_LAST 0x1Du
#define DI8DEVTYPE_MOUSE 0x12u
#define DI8DEVTYPE_KEYBOARD 0x13u

/* EnumDevices flags. */
#define DIEDFL_ATTACHEDONLY 0x00000001u
#define DIEDFL_FORCEFEEDBACK 0x00000100u
#define DIEDFL_INCLUDEALIASED 0x00010000u
#define DIEDFL_INCLUDEPHANTOMS 0x00020000u
#define DIEDFL_INCLUDEHIDDEN 0x00040000u

/* Object types (dwType). */
#define DIDFT_RELAXIS 0x00000001u
#define DIDFT_ABSAXIS 0x00000002u
#define DIDFT_AXIS 0x00000003u
#define DIDFT_PSHBUTTON 0x00000004u
#define DIDFT_BUTTON 0x0000000Cu
#define DIDFT_TYPEMASK 0x000000FFu
#define DIDFT_INSTANCEMASK 0x00FFFF00u
#define DIDFT_ANYINSTANCE 0x00FFFF00u
#define DIDFT_ATTRMASK 0xFF000000u
#define DIDFT_OPTIONAL 0x80000000u
#define DIDFT_MAKEINSTANCE(n) ((uint32_t)(n) << 8)
#define DIDFT_GETINSTANCE(t) (((t) >> 8) & 0xFFFFu)

/* Data format flags. */
#define DIDF_ABSAXIS 0x00000001u
#define DIDF_RELAXIS 0x00000002u

/* Cooperative levels. */
#define DISCL_EXCLUSIVE 0x00000001u
#define DISCL_NONEXCLUSIVE 0x00000002u
#define DISCL_FOREGROUND 0x00000004u
#define DISCL_BACKGROUND 0x00000008u
#define DISCL_NOWINKEY 0x00000010u

#define DIDC_ATTACHED 0x00000001u

/* Structure sizes. */
#define DIDEVICEINSTANCE_SIZE 0x244u      /* DIDEVICEINSTANCEA (DirectX 5 and later) */
#define DIDEVICEINSTANCE_DX3_SIZE 0x204u
#define DIDEVICEOBJECTINSTANCE_SIZE 0x13Cu /* DIDEVICEOBJECTINSTANCEA (DirectX 5 and later) */
#define DIDATAFORMAT_SIZE 0x18u
#define DIOBJECTDATAFORMAT_SIZE 0x10u
#define DIDEVCAPS_SIZE 0x2Cu
#define DIDEVCAPS_DX3_SIZE 0x18u

/* Limits for a data format from the guest. */
#define MAX_FORMAT_OBJECTS 1024u
#define MAX_FORMAT_SIZE 65536u

#define MOUSE_BUTTONS 8
#define MOUSE_OBJECTS (3 + MOUSE_BUTTONS)

static const rt_guid IID_IDirectInput8A = { 0xBF798030, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };
static const rt_guid IID_IDirectInputDevice8A = { 0x54D41080, 0xDC15, 0x4833, { 0xA4, 0x1B, 0x74, 0x8F, 0x73, 0xA3, 0x81, 0x79 } };
static const rt_guid GUID_SysMouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const rt_guid GUID_SysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const rt_guid GUID_XAxis = { 0xA36D02E0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const rt_guid GUID_YAxis = { 0xA36D02E1, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const rt_guid GUID_ZAxis = { 0xA36D02E2, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const rt_guid GUID_Button = { 0xA36D02F0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const rt_guid GUID_Key = { 0x55728220, 0xD33C, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };

/* ---------------------------------------------------------------- devices and objects */

enum { DEV_MOUSE, DEV_KEYBOARD, DEV_COUNT };

typedef struct di_object {
    const rt_guid *guid;
    uint32_t native_ofs; /* offset in the native data format */
    uint32_t type;       /* dwType */
    uint8_t code;        /* DIK code, mouse button number, or axis 0-2 */
    bool axis;
    char name[24];
} di_object;

typedef struct di_device_desc {
    const rt_guid *guid;
    uint32_t devtype;
    const char *name;
    di_object *objects;
    int nobjects;
    int naxes, nbuttons;
} di_device_desc;

static pthread_mutex_t di_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t objects_once = PTHREAD_ONCE_INIT;
static di_object mouse_objects[MOUSE_OBJECTS];
static di_object key_objects[256];
static di_device_desc devices[DEV_COUNT] = {
    [DEV_MOUSE] = { &GUID_SysMouse, DI8DEVTYPE_MOUSE | 0x100u, "Mouse", mouse_objects, 0, 0, 0 },
    [DEV_KEYBOARD] = { &GUID_SysKeyboard, DI8DEVTYPE_KEYBOARD | 0x400u, "Keyboard", key_objects, 0, 0, 0 },
};

/* US key names by DIK code. */
static const char *const key_names[256] = {
    [0x01] = "Esc", [0x02] = "1", [0x03] = "2", [0x04] = "3", [0x05] = "4", [0x06] = "5", [0x07] = "6",
    [0x08] = "7", [0x09] = "8", [0x0A] = "9", [0x0B] = "0", [0x0C] = "-", [0x0D] = "=", [0x0E] = "Backspace",
    [0x0F] = "Tab", [0x10] = "Q", [0x11] = "W", [0x12] = "E", [0x13] = "R", [0x14] = "T", [0x15] = "Y",
    [0x16] = "U", [0x17] = "I", [0x18] = "O", [0x19] = "P", [0x1A] = "[", [0x1B] = "]", [0x1C] = "Enter",
    [0x1D] = "Ctrl", [0x1E] = "A", [0x1F] = "S", [0x20] = "D", [0x21] = "F", [0x22] = "G", [0x23] = "H",
    [0x24] = "J", [0x25] = "K", [0x26] = "L", [0x27] = ";", [0x28] = "'", [0x29] = "`", [0x2A] = "Shift",
    [0x2B] = "\\", [0x2C] = "Z", [0x2D] = "X", [0x2E] = "C", [0x2F] = "V", [0x30] = "B", [0x31] = "N",
    [0x32] = "M", [0x33] = ",", [0x34] = ".", [0x35] = "/", [0x36] = "Right Shift", [0x37] = "Num *",
    [0x38] = "Alt", [0x39] = "Space", [0x3A] = "Caps Lock", [0x3B] = "F1", [0x3C] = "F2", [0x3D] = "F3",
    [0x3E] = "F4", [0x3F] = "F5", [0x40] = "F6", [0x41] = "F7", [0x42] = "F8", [0x43] = "F9", [0x44] = "F10",
    [0x45] = "Num Lock", [0x46] = "Scroll Lock", [0x47] = "Num 7", [0x48] = "Num 8", [0x49] = "Num 9",
    [0x4A] = "Num -", [0x4B] = "Num 4", [0x4C] = "Num 5", [0x4D] = "Num 6", [0x4E] = "Num +", [0x4F] = "Num 1",
    [0x50] = "Num 2", [0x51] = "Num 3", [0x52] = "Num 0", [0x53] = "Num Del", [0x56] = "<", [0x57] = "F11",
    [0x58] = "F12", [0x64] = "F13", [0x65] = "F14", [0x66] = "F15",
    [0x9C] = "Num Enter", [0x9D] = "Right Ctrl", [0xB5] = "Num /", [0xB8] = "Right Alt", [0xC7] = "Home",
    [0xC8] = "Up", [0xC9] = "Page Up", [0xCB] = "Left", [0xCD] = "Right", [0xCF] = "End", [0xD0] = "Down",
    [0xD1] = "Page Down", [0xD2] = "Insert", [0xD3] = "Delete", [0xDB] = "Left Windows", [0xDC] = "Right Windows",
};

static void set_name(di_object *o, const char *s)
{
    size_t n = strlen(s);
    if (n >= sizeof o->name) rt_fatal("DINPUT8: object name \"%s\" is too long", s);
    memcpy(o->name, s, n + 1);
}

static void build_objects(void)
{
    static const char *const axis_names[3] = { "X-axis", "Y-axis", "Wheel" };
    static const rt_guid *const axis_guids[3] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis };
    uint8_t codes[256];
    int i, n;
    for (i = 0; i < 3; i++)
    {
        di_object *o = &mouse_objects[i];
        o->guid = axis_guids[i];
        o->native_ofs = (uint32_t)i * 4u;
        o->type = DIDFT_RELAXIS | DIDFT_MAKEINSTANCE(i);
        o->code = (uint8_t)i;
        o->axis = true;
        set_name(o, axis_names[i]);
    }
    for (i = 0; i < MOUSE_BUTTONS; i++)
    {
        di_object *o = &mouse_objects[3 + i];
        char name[16] = "Button 0";
        name[7] = (char)('0' + i);
        o->guid = &GUID_Button;
        o->native_ofs = 12u + (uint32_t)i; /* DIMOUSESTATE2 */
        o->type = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(3 + i);
        o->code = (uint8_t)i;
        o->axis = false;
        set_name(o, name);
    }
    devices[DEV_MOUSE].nobjects = MOUSE_OBJECTS;
    devices[DEV_MOUSE].naxes = 3;
    devices[DEV_MOUSE].nbuttons = MOUSE_BUTTONS;
    n = rt_user32_dik_codes(codes);
    for (i = 0; i < n; i++)
    {
        di_object *o = &key_objects[i];
        if (key_names[codes[i]] == NULL) rt_fatal("DINPUT8: the key with code 0x%02x has no name", codes[i]);
        o->guid = &GUID_Key;
        o->native_ofs = codes[i];
        o->type = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(codes[i]);
        o->code = codes[i];
        o->axis = false;
        set_name(o, key_names[codes[i]]);
    }
    devices[DEV_KEYBOARD].nobjects = n;
    devices[DEV_KEYBOARD].naxes = 0;
    devices[DEV_KEYBOARD].nbuttons = n;
}

static void need_objects(void)
{
    if (pthread_once(&objects_once, build_objects) != 0) rt_fatal("DINPUT8: pthread_once failed");
}

/* ---------------------------------------------------------------- device state */

typedef struct di_map {
    int object;
    uint32_t ofs;
} di_map;

typedef struct di_dev di_dev;
struct di_dev {
    int kind;
    uint32_t hwnd;
    uint32_t coop;    /* 0 until SetCooperativeLevel */
    bool acquired;
    bool lost;        /* unacquired by a focus change; reported once */
    bool has_format;
    bool relative;    /* DIDF_RELAXIS */
    uint32_t data_size;
    di_map *map;
    uint32_t nmap;
    int32_t obj_ofs[256]; /* data format offset of each object, or -1 */
    uint8_t latch[256];   /* keys or buttons pressed since the last read */
    int64_t rel[3];       /* mouse motion since the last read */
    int64_t abs[3];       /* mouse position (absolute axis mode) */
    di_dev *next;
};

/* Global input state (protected by di_lock). */
static uint8_t key_down[256];
static uint8_t button_down[MOUSE_BUTTONS];
static double motion_rest[2];
static uint32_t focus_hwnd;
static di_dev *dev_list;
static bool mouse_captured; /* rt_cocoa_mouse_capture(true) is in effect */

static void lock(void)
{
    if (pthread_mutex_lock(&di_lock) != 0) rt_fatal("DINPUT8: mutex lock failed");
}

static void unlock(void)
{
    if (pthread_mutex_unlock(&di_lock) != 0) rt_fatal("DINPUT8: mutex unlock failed");
}

/* Call with di_lock held after a change of the acquired devices: capture
   the mouse while an exclusive mouse device is acquired. */
static void update_capture(void)
{
    bool want = false;
    di_dev *d;
    for (d = dev_list; d != NULL; d = d->next)
        if (d->kind == DEV_MOUSE && d->acquired && (d->coop & DISCL_EXCLUSIVE)) want = true;
    if (want != mouse_captured)
    {
        mouse_captured = want;
        rt_cocoa_mouse_capture(want);
    }
}

static void add_motion(int axis, int64_t v)
{
    di_dev *d;
    for (d = dev_list; d != NULL; d = d->next)
    {
        if (d->kind != DEV_MOUSE || !d->acquired) continue;
        d->rel[axis] += v;
        d->abs[axis] += v;
    }
}

void rt_dinput_on_key(uint8_t dik, bool down)
{
    di_dev *d;
    lock();
    key_down[dik] = down ? 1 : 0;
    if (down)
        for (d = dev_list; d != NULL; d = d->next)
            if (d->kind == DEV_KEYBOARD && d->acquired) d->latch[dik] = 1;
    unlock();
}

void rt_dinput_on_mouse_motion(double dx, double dy)
{
    double v[2];
    int i;
    if (!isfinite(dx) || !isfinite(dy)) return;
    lock();
    v[0] = motion_rest[0] + dx;
    v[1] = motion_rest[1] + dy;
    for (i = 0; i < 2; i++)
    {
        double whole = trunc(v[i]);
        if (whole > 1e6) whole = 1e6;
        if (whole < -1e6) whole = -1e6;
        motion_rest[i] = v[i] - trunc(v[i]);
        if (whole != 0.0) add_motion(i, (int64_t)whole);
    }
    unlock();
}

void rt_dinput_on_mouse_button(int button, bool down)
{
    di_dev *d;
    if (button < 0 || button >= MOUSE_BUTTONS)
    {
        static bool logged;
        if (!logged)
        {
            logged = true;
            rt_log("DINPUT8: mouse button %d is above the %d buttons of the DirectInput mouse; it is ignored",
                   button, MOUSE_BUTTONS);
        }
        return;
    }
    lock();
    button_down[button] = down ? 1 : 0;
    if (down)
        for (d = dev_list; d != NULL; d = d->next)
            if (d->kind == DEV_MOUSE && d->acquired) d->latch[button] = 1;
    unlock();
}

void rt_dinput_on_wheel(int32_t delta)
{
    lock();
    add_motion(2, delta);
    unlock();
}

void rt_dinput_on_focus(uint32_t hwnd, bool active)
{
    di_dev *d;
    lock();
    if (active)
    {
        focus_hwnd = hwnd;
    }
    else if (focus_hwnd == hwnd)
    {
        focus_hwnd = 0;
        /* macOS sends no key-up or button-up events to a window that is not
           the key window: release all keys and buttons. */
        memset(key_down, 0, sizeof key_down);
        memset(button_down, 0, sizeof button_down);
        for (d = dev_list; d != NULL; d = d->next)
        {
            if (!d->acquired || !(d->coop & DISCL_FOREGROUND)) continue;
            d->acquired = false;
            d->lost = true;
        }
        update_capture();
    }
    unlock();
}

/* ---------------------------------------------------------------- COM classes */

typedef struct di_main {
    bool initialized;
} di_main;

static void main_destroy(rt_com_object *o)
{
    free(o->data);
}

static void dev_destroy(rt_com_object *o)
{
    di_dev *d = o->data, **pp;
    lock();
    for (pp = &dev_list; *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == d)
        {
            *pp = d->next;
            break;
        }
    }
    update_capture();
    unlock();
    free(d->map);
    free(d);
}

static const rt_com_iface main_ifaces[] = { { &IID_IDirectInput8A, 0 } };
static const rt_com_iface dev_ifaces[] = { { &IID_IDirectInputDevice8A, 0 } };
static const rt_com_class main_class = {
    "IDirectInput8A", 1, { &rt_vt_DirectInput8A, NULL, NULL, NULL }, main_ifaces, 1, main_destroy
};
static const rt_com_class dev_class = {
    "IDirectInputDevice8A", 1, { &rt_vt_DirectInputDevice8A, NULL, NULL, NULL }, dev_ifaces, 1, dev_destroy
};

rt_com_object *rt_dinput_create(void)
{
    di_main *m = calloc(1, sizeof *m);
    rt_com_object *o;
    if (m == NULL) return NULL;
    o = rt_com_create(&main_class, m);
    if (o == NULL) free(m);
    return o;
}

static di_main *main_this(_cpu *cpu)
{
    return rt_com_this(ARG(0), 0, &main_class)->data;
}

static di_dev *dev_this(_cpu *cpu)
{
    return rt_com_this(ARG(0), 0, &dev_class)->data;
}

static int device_for_guid(const rt_guid *g)
{
    int i;
    for (i = 0; i < DEV_COUNT; i++)
        if (rt_guid_eq(g, devices[i].guid)) return i;
    return -1;
}

/* Write a guest null-terminated string into a fixed field of n bytes. */
static void put_str(uint32_t g, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (len >= n) rt_fatal("DINPUT8: the text \"%s\" does not fit a %zu-byte field", s, n);
    memset(G2H(g), 0, n);
    memcpy(G2H(g), s, len);
}

/* DIDEVICEINSTANCEA: dwSize, guidInstance, guidProduct, dwDevType,
   tszInstanceName[260], tszProductName[260], guidFFDriver, wUsagePage,
   wUsage. */
static void write_instance(uint32_t p, uint32_t size, int kind)
{
    const di_device_desc *dd = &devices[kind];
    memset(G2H(p), 0, size);
    wr32(p, size);
    rt_guid_write(p + 0x04, dd->guid);
    rt_guid_write(p + 0x14, dd->guid);
    wr32(p + 0x24, dd->devtype);
    put_str(p + 0x28, dd->name, 260);
    put_str(p + 0x12C, dd->name, 260);
    if (size >= DIDEVICEINSTANCE_SIZE)
    {
        /* HID usage of a generic desktop mouse (2) or keyboard (6). */
        wr16(p + 0x240, 0x01);
        wr16(p + 0x242, kind == DEV_MOUSE ? 0x02 : 0x06);
    }
}

/* HRESULT IDirectInput8A::Initialize(HINSTANCE, DWORD version) */
void rt_com_DirectInput8A_Initialize(_cpu *cpu)
{
    di_main *m = main_this(cpu);
    uint32_t ver = ARG(2);
    if (ver < DIRECTINPUT_VERSION)
    {
        RET(DIERR_OLDDIRECTINPUTVERSION);
        return;
    }
    if (ver > DIRECTINPUT_VERSION)
    {
        RET(DIERR_BETADIRECTINPUTVERSION);
        return;
    }
    m->initialized = true;
    RET(DI_OK);
}

/* HRESULT IDirectInput8A::EnumDevices(DWORD type, callback, void *ref,
   DWORD flags). callback: BOOL __stdcall (const DIDEVICEINSTANCEA *, void *) */
void rt_com_DirectInput8A_EnumDevices(_cpu *cpu)
{
    di_main *m = main_this(cpu);
    uint32_t type = ARG(1), cb = ARG(2), ref = ARG(3), flags = ARG(4);
    const uint32_t known = DIEDFL_ATTACHEDONLY | DIEDFL_FORCEFEEDBACK | DIEDFL_INCLUDEALIASED |
                           DIEDFL_INCLUDEPHANTOMS | DIEDFL_INCLUDEHIDDEN;
    static const int order[DEV_COUNT] = { DEV_MOUSE, DEV_KEYBOARD };
    uint32_t buf;
    int i;
    if (!m->initialized)
    {
        RET(DIERR_NOTINITIALIZED);
        return;
    }
    if (cb == 0 || (flags & ~known) != 0 ||
        !(type <= DI8DEVCLASS_GAMECTRL || (type >= DI8DEVTYPE_FIRST && type <= DI8DEVTYPE_LAST)))
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    need_objects();
    buf = rt_heap_calloc(1, DIDEVICEINSTANCE_SIZE);
    if (buf == 0)
    {
        RET(DIERR_OUTOFMEMORY);
        return;
    }
    for (i = 0; i < DEV_COUNT; i++)
    {
        int kind = order[i];
        uint32_t args[2], cls = kind == DEV_MOUSE ? DI8DEVCLASS_POINTER : DI8DEVCLASS_KEYBOARD;
        uint32_t dt = devices[kind].devtype & 0xFFu;
        if (flags & DIEDFL_FORCEFEEDBACK) continue; /* no force feedback */
        if (type != DI8DEVCLASS_ALL && type != cls && type != dt) continue;
        write_instance(buf, DIDEVICEINSTANCE_SIZE, kind);
        args[0] = buf;
        args[1] = ref;
        if (rt_call(cpu, cb, true, 0, 2, args) == 0) break; /* DIENUM_STOP */
    }
    rt_heap_free(buf);
    RET(DI_OK);
}

/* HRESULT IDirectInput8A::CreateDevice(REFGUID, IDirectInputDevice8A **,
   IUnknown *outer) */
void rt_com_DirectInput8A_CreateDevice(_cpu *cpu)
{
    di_main *m = main_this(cpu);
    uint32_t pguid = ARG(1), pp = ARG(2), outer = ARG(3);
    rt_guid g;
    di_dev *d;
    rt_com_object *o;
    int kind, i;
    if (pp == 0 || pguid == 0)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    wr32(pp, 0);
    if (!m->initialized)
    {
        RET(DIERR_NOTINITIALIZED);
        return;
    }
    if (outer != 0)
    {
        RET(DIERR_NOAGGREGATION);
        return;
    }
    g = rt_guid_read(pguid);
    kind = device_for_guid(&g);
    if (kind < 0)
    {
        RET(DIERR_DEVICENOTREG);
        return;
    }
    need_objects();
    d = calloc(1, sizeof *d);
    if (d == NULL)
    {
        RET(DIERR_OUTOFMEMORY);
        return;
    }
    d->kind = kind;
    for (i = 0; i < 256; i++) d->obj_ofs[i] = -1;
    o = rt_com_create(&dev_class, d);
    if (o == NULL)
    {
        free(d);
        RET(DIERR_OUTOFMEMORY);
        return;
    }
    lock();
    d->next = dev_list;
    dev_list = d;
    unlock();
    wr32(pp, rt_com_ptr(o, 0));
    RET(DI_OK);
}

/* HRESULT IDirectInput8A::GetDeviceStatus(REFGUID) */
void rt_com_DirectInput8A_GetDeviceStatus(_cpu *cpu)
{
    di_main *m = main_this(cpu);
    rt_guid g;
    if (!m->initialized)
    {
        RET(DIERR_NOTINITIALIZED);
        return;
    }
    if (ARG(1) == 0)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    g = rt_guid_read(ARG(1));
    RET(device_for_guid(&g) >= 0 ? DI_OK : DIERR_DEVICENOTREG);
}

/* ---------------------------------------------------------------- IDirectInputDevice8A */

/* HRESULT GetCapabilities(DIDEVCAPS *) */
void rt_com_DirectInputDevice8A_GetCapabilities(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    const di_device_desc *dd = &devices[d->kind];
    uint32_t p = ARG(1), size;
    if (p == 0)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    size = rd32(p);
    if (size != DIDEVCAPS_SIZE && size != DIDEVCAPS_DX3_SIZE)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    memset(G2H(p + 4), 0, size - 4);
    wr32(p + 0x04, DIDC_ATTACHED);
    wr32(p + 0x08, dd->devtype);
    wr32(p + 0x0C, (uint32_t)dd->naxes);
    wr32(p + 0x10, (uint32_t)dd->nbuttons);
    wr32(p + 0x14, 0); /* POVs */
    RET(DI_OK);
}

/* HRESULT GetDeviceInfo(DIDEVICEINSTANCEA *) */
void rt_com_DirectInputDevice8A_GetDeviceInfo(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    uint32_t p = ARG(1), size;
    if (p == 0)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    size = rd32(p);
    if (size != DIDEVICEINSTANCE_SIZE && size != DIDEVICEINSTANCE_DX3_SIZE)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    write_instance(p, size, d->kind);
    RET(DI_OK);
}

/* HRESULT EnumObjects(callback, void *ref, DWORD flags).
   callback: BOOL __stdcall (const DIDEVICEOBJECTINSTANCEA *, void *) */
void rt_com_DirectInputDevice8A_EnumObjects(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    const di_device_desc *dd = &devices[d->kind];
    uint32_t cb = ARG(1), ref = ARG(2), flags = ARG(3);
    uint32_t buf, want = flags & DIDFT_TYPEMASK, attr = flags & DIDFT_ATTRMASK & ~DIDFT_OPTIONAL;
    int i;
    if (cb == 0)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    buf = rt_heap_calloc(1, DIDEVICEOBJECTINSTANCE_SIZE);
    if (buf == 0)
    {
        RET(DIERR_OUTOFMEMORY);
        return;
    }
    for (i = 0; i < dd->nobjects; i++)
    {
        const di_object *o = &dd->objects[i];
        uint32_t args[2], ofs;
        if (want != 0 && (o->type & want) == 0) continue;
        if (attr != 0) continue; /* no object has force feedback or other attributes */
        lock();
        ofs = d->obj_ofs[i] >= 0 ? (uint32_t)d->obj_ofs[i] : o->native_ofs;
        unlock();
        /* DIDEVICEOBJECTINSTANCEA: dwSize, guidType, dwOfs, dwType, dwFlags,
           tszName[260], FF and HID fields (zero). */
        memset(G2H(buf), 0, DIDEVICEOBJECTINSTANCE_SIZE);
        wr32(buf, DIDEVICEOBJECTINSTANCE_SIZE);
        rt_guid_write(buf + 0x04, o->guid);
        wr32(buf + 0x14, ofs);
        wr32(buf + 0x18, o->type);
        wr32(buf + 0x1C, 0);
        put_str(buf + 0x20, o->name, 260);
        args[0] = buf;
        args[1] = ref;
        if (rt_call(cpu, cb, true, 0, 2, args) == 0) break; /* DIENUM_STOP */
    }
    rt_heap_free(buf);
    RET(DI_OK);
}

/* True if the object matches the format entry (DirectInput rules: the GUID
   when given, the type bits, the instance unless DIDFT_ANYINSTANCE, and the
   attributes). */
static bool format_match(const di_object *o, const rt_guid *g, uint32_t type)
{
    uint32_t attr = type & DIDFT_ATTRMASK & ~DIDFT_OPTIONAL;
    if (g != NULL && !rt_guid_eq(g, o->guid)) return false;
    if ((type & DIDFT_TYPEMASK) != 0 && (o->type & type & DIDFT_TYPEMASK) == 0) return false;
    if ((type & DIDFT_INSTANCEMASK) != DIDFT_ANYINSTANCE &&
        DIDFT_GETINSTANCE(type) != DIDFT_GETINSTANCE(o->type)) return false;
    if (attr != 0) return false; /* no object has these attributes */
    return true;
}

/* HRESULT SetDataFormat(const DIDATAFORMAT *) */
void rt_com_DirectInputDevice8A_SetDataFormat(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    const di_device_desc *dd = &devices[d->kind];
    uint32_t p = ARG(1), dflags, size, n, rg, i;
    int32_t ofs_of[256];
    uint8_t used[MAX_FORMAT_SIZE / 8];
    di_map *map;
    uint32_t nmap = 0;
    int k;
    if (p == 0 || rd32(p) != DIDATAFORMAT_SIZE || rd32(p + 4) != DIOBJECTDATAFORMAT_SIZE)
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    dflags = rd32(p + 8);
    size = rd32(p + 12);
    n = rd32(p + 16);
    rg = rd32(p + 20);
    if ((dflags & ~(DIDF_ABSAXIS | DIDF_RELAXIS)) != 0 || dflags == (DIDF_ABSAXIS | DIDF_RELAXIS) ||
        size == 0 || size % 4 != 0 || size > MAX_FORMAT_SIZE || n > MAX_FORMAT_OBJECTS || (n != 0 && rg == 0))
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    if (n != 0 && !rt_va_is_committed(rg, n * DIOBJECTDATAFORMAT_SIZE))
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    lock();
    if (d->acquired)
    {
        unlock();
        RET(DIERR_ACQUIRED);
        return;
    }
    unlock();
    map = n != 0 ? calloc(n, sizeof *map) : NULL;
    if (n != 0 && map == NULL)
    {
        RET(DIERR_OUTOFMEMORY);
        return;
    }
    for (k = 0; k < 256; k++) ofs_of[k] = -1;
    memset(used, 0, sizeof used);
    for (i = 0; i < n; i++)
    {
        uint32_t e = rg + i * DIOBJECTDATAFORMAT_SIZE;
        uint32_t pg = rd32(e), ofs = rd32(e + 4), type = rd32(e + 8), width, b;
        rt_guid g;
        int found = -1;
        if (pg != 0) g = rt_guid_read(pg);
        for (k = 0; k < dd->nobjects; k++)
        {
            if (ofs_of[k] >= 0) continue;
            if (format_match(&dd->objects[k], pg != 0 ? &g : NULL, type))
            {
                found = k;
                break;
            }
        }
        if (found < 0)
        {
            if (type & DIDFT_OPTIONAL) continue;
            free(map);
            RET(DIERR_INVALIDPARAM);
            return;
        }
        width = dd->objects[found].axis ? 4u : 1u;
        if ((width == 4 && ofs % 4 != 0) || ofs > size - width)
        {
            free(map);
            RET(DIERR_INVALIDPARAM);
            return;
        }
        for (b = ofs; b < ofs + width; b++)
        {
            if (used[b / 8] & (1u << (b % 8)))
            {
                free(map);
                RET(DIERR_INVALIDPARAM);
                return;
            }
            used[b / 8] |= (uint8_t)(1u << (b % 8));
        }
        ofs_of[found] = (int32_t)ofs;
        map[nmap].object = found;
        map[nmap].ofs = ofs;
        nmap++;
    }
    lock();
    if (d->acquired)
    {
        unlock();
        free(map);
        RET(DIERR_ACQUIRED);
        return;
    }
    free(d->map);
    d->map = map;
    d->nmap = nmap;
    d->data_size = size;
    /* No axis flag: the native mode of the device (relative for a mouse). */
    d->relative = (dflags & DIDF_RELAXIS) != 0 || (d->kind == DEV_MOUSE && (dflags & DIDF_ABSAXIS) == 0);
    memcpy(d->obj_ofs, ofs_of, sizeof ofs_of);
    d->has_format = true;
    unlock();
    RET(DI_OK);
}

/* HRESULT SetCooperativeLevel(HWND, DWORD flags) */
void rt_com_DirectInputDevice8A_SetCooperativeLevel(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    uint32_t hwnd = ARG(1), flags = ARG(2), excl = flags & (DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE),
             fg = flags & (DISCL_FOREGROUND | DISCL_BACKGROUND);
    int32_t w, h;
    if ((flags & ~(DISCL_EXCLUSIVE | DISCL_NONEXCLUSIVE | DISCL_FOREGROUND | DISCL_BACKGROUND | DISCL_NOWINKEY)) != 0 ||
        (excl != DISCL_EXCLUSIVE && excl != DISCL_NONEXCLUSIVE) || (fg != DISCL_FOREGROUND && fg != DISCL_BACKGROUND))
    {
        RET(DIERR_INVALIDPARAM);
        return;
    }
    if (excl == DISCL_EXCLUSIVE && fg == DISCL_BACKGROUND)
    {
        RET(DIERR_UNSUPPORTED);
        return;
    }
    if (flags & DISCL_NOWINKEY)
        rt_fatal("DINPUT8: DISCL_NOWINKEY is not supported");
    if (!rt_user32_client_size(hwnd, &w, &h))
    {
        RET(DIERR_INVALIDPARAM); /* not a top-level window */
        return;
    }
    lock();
    if (d->acquired)
    {
        unlock();
        RET(DIERR_ACQUIRED);
        return;
    }
    d->hwnd = hwnd;
    d->coop = flags;
    unlock();
    RET(DI_OK);
}

/* HRESULT Acquire(void) */
void rt_com_DirectInputDevice8A_Acquire(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    uint32_t r;
    lock();
    if (!d->has_format)
        r = DIERR_INVALIDPARAM;
    else if (d->acquired)
        r = DI_NOEFFECT;
    else if ((d->coop & DISCL_FOREGROUND) && (focus_hwnd == 0 || focus_hwnd != d->hwnd))
        r = DIERR_OTHERAPPHASPRIO;
    else
    {
        d->acquired = true;
        d->lost = false;
        memset(d->latch, 0, sizeof d->latch);
        memset(d->rel, 0, sizeof d->rel);
        update_capture();
        r = DI_OK;
    }
    unlock();
    RET(r);
}

/* HRESULT Unacquire(void) */
void rt_com_DirectInputDevice8A_Unacquire(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    uint32_t r;
    lock();
    r = d->acquired ? DI_OK : DI_NOEFFECT;
    d->acquired = false;
    d->lost = false;
    update_capture();
    unlock();
    RET(r);
}

/* HRESULT Poll(void): the keyboard and the mouse are not polled devices. */
void rt_com_DirectInputDevice8A_Poll(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    uint32_t r;
    lock();
    r = d->acquired ? DI_NOEFFECT : DIERR_NOTACQUIRED;
    unlock();
    RET(r);
}

/* HRESULT GetDeviceState(DWORD cb, void *data) */
void rt_com_DirectInputDevice8A_GetDeviceState(_cpu *cpu)
{
    di_dev *d = dev_this(cpu);
    const di_device_desc *dd = &devices[d->kind];
    uint32_t cb = ARG(1), data = ARG(2), i;
    uint8_t *out;
    lock();
    if (!d->acquired)
    {
        uint32_t r = d->lost ? DIERR_INPUTLOST : DIERR_NOTACQUIRED;
        d->lost = false;
        unlock();
        RET(r);
        return;
    }
    if (data == 0 || cb != d->data_size || !rt_va_is_committed(data, cb))
    {
        unlock();
        RET(DIERR_INVALIDPARAM);
        return;
    }
    out = G2H(data);
    memset(out, 0, cb);
    for (i = 0; i < d->nmap; i++)
    {
        const di_object *o = &dd->objects[d->map[i].object];
        uint32_t ofs = d->map[i].ofs;
        if (o->axis)
        {
            int64_t v = d->relative ? d->rel[o->code] : d->abs[o->code];
            int32_t x;
            if (v > INT32_MAX) v = INT32_MAX;
            if (v < INT32_MIN) v = INT32_MIN;
            x = (int32_t)v;
            memcpy(out + ofs, &x, 4);
        }
        else
        {
            bool down;
            if (d->kind == DEV_KEYBOARD)
                down = key_down[o->code] || d->latch[o->code];
            else
                down = button_down[o->code] || d->latch[o->code];
            out[ofs] = down ? 0x80 : 0x00;
        }
    }
    memset(d->latch, 0, sizeof d->latch);
    memset(d->rel, 0, sizeof d->rel);
    unlock();
    RET(DI_OK);
}
