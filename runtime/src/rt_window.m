/*
 * Native windows and events for USER32 (see rt_window.h).
 *
 * Each Win32 top-level window is one NSWindow with one content view. The
 * content view is backed by a CAMetalLayer for the renderer. The view is
 * flipped, so view coordinates are Win32 client coordinates (points).
 *
 * The guest message loop calls rt_cocoa_pump (from PeekMessageA). The pump
 * takes every waiting NSEvent and gives it to NSApp; the view and the
 * window delegate then report keys, mouse input, activation, moves,
 * resizes and close requests to USER32.
 */
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "rt.h"
#include "rt_dinput.h"
#include "rt_window.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void need_main(const char *fn)
{
    if (![NSThread isMainThread]) rt_fatal("%s: the window system works only on the main thread", fn);
}

bool rt_cocoa_is_main_thread(void)
{
    return [NSThread isMainThread];
}

static CGFloat main_height(void)
{
    NSScreen *s = NSScreen.screens.firstObject;
    if (s == nil) rt_fatal("no display is available");
    return s.frame.size.height;
}

static NSRect to_ns(rt_rect r)
{
    CGFloat h = main_height();
    return NSMakeRect(r.left, h - r.bottom, r.right - r.left, r.bottom - r.top);
}

static int32_t to_i32(CGFloat v)
{
    double d = round(v);
    if (d < INT32_MIN || d > INT32_MAX) rt_fatal("window coordinate %f is out of range", d);
    return (int32_t)d;
}

static rt_rect from_ns(NSRect f)
{
    CGFloat h = main_height();
    rt_rect r;
    r.left = to_i32(f.origin.x);
    r.right = to_i32(f.origin.x + f.size.width);
    r.top = to_i32(h - (f.origin.y + f.size.height));
    r.bottom = to_i32(h - f.origin.y);
    return r;
}

static NSWindowStyleMask mask_for(unsigned kind)
{
    NSWindowStyleMask m;
    if (kind & RT_WIN_FULLSCREEN) return NSWindowStyleMaskBorderless;
    if (!(kind & RT_WIN_TITLED)) return NSWindowStyleMaskBorderless;
    m = NSWindowStyleMaskTitled;
    if (kind & RT_WIN_CLOSABLE) m |= NSWindowStyleMaskClosable;
    if (kind & RT_WIN_MINIMIZABLE) m |= NSWindowStyleMaskMiniaturizable;
    if (kind & RT_WIN_RESIZABLE) m |= NSWindowStyleMaskResizable;
    return m;
}

/* ---------------------------------------------------------------- view */

@interface RTView : NSView
@property(nonatomic) uint32_t hwnd;
@property(nonatomic) double scrollRest; /* precise scrolling not yet sent, in WHEEL_DELTA units */
@end

@implementation RTView

- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent *)event { (void)event; return YES; }
- (BOOL)wantsUpdateLayer { return YES; }

- (CALayer *)makeBackingLayer
{
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = nil; /* the renderer sets the device */
    return l;
}

static void key_event(RTView *v, NSEvent *e, bool down)
{
    uint16_t chars[16];
    int n = 0;
    if (down)
    {
        NSString *s = e.characters;
        NSUInteger len = s.length, i;
        for (i = 0; i < len && n < 16; i++) chars[n++] = [s characterAtIndex:i];
    }
    rt_user32_on_key(v.hwnd, down, down && e.ARepeat, e.keyCode, chars, n);
}

- (void)keyDown:(NSEvent *)e { key_event(self, e, true); }
- (void)keyUp:(NSEvent *)e { key_event(self, e, false); }
- (void)flagsChanged:(NSEvent *)e { rt_user32_on_modifiers(self.hwnd, (uint32_t)e.modifierFlags); }

/* Command-key combinations go to the main menu first (Command-Q). Other
   key equivalents are game input. */
- (BOOL)performKeyEquivalent:(NSEvent *)e
{
    (void)e;
    return NO;
}

static void mouse_event(RTView *v, NSEvent *e, uint32_t msg, int32_t wheel)
{
    NSPoint p = [v convertPoint:e.locationInWindow fromView:nil];
    rt_user32_on_mouse(v.hwnd, msg, to_i32(floor(p.x)), to_i32(floor(p.y)), wheel);
}

#define WM_MOUSEMOVE 0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP 0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP 0x0205u
#define WM_MBUTTONDOWN 0x0207u
#define WM_MBUTTONUP 0x0208u
#define WM_MOUSEWHEEL 0x020Au

/* Motion and buttons go to USER32 (window messages) and to DINPUT8
   (device state). DINPUT8 gets the relative motion of the event, also
   when the cursor cannot move further (at a screen edge). */
static void motion_event(RTView *v, NSEvent *e)
{
    rt_dinput_on_mouse_motion(e.deltaX, e.deltaY);
    mouse_event(v, e, WM_MOUSEMOVE, 0);
}

static void button_event(RTView *v, NSEvent *e, bool down)
{
    NSInteger b = e.buttonNumber;
    if (b >= 0 && b <= 31) rt_dinput_on_mouse_button((int)b, down);
    if (b == 0) mouse_event(v, e, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, 0);
    else if (b == 1) mouse_event(v, e, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, 0);
    else if (b == 2) mouse_event(v, e, down ? WM_MBUTTONDOWN : WM_MBUTTONUP, 0);
}

- (void)mouseMoved:(NSEvent *)e { motion_event(self, e); }
- (void)mouseDragged:(NSEvent *)e { motion_event(self, e); }
- (void)rightMouseDragged:(NSEvent *)e { motion_event(self, e); }
- (void)otherMouseDragged:(NSEvent *)e { motion_event(self, e); }
- (void)mouseDown:(NSEvent *)e { button_event(self, e, true); }
- (void)mouseUp:(NSEvent *)e { button_event(self, e, false); }
- (void)rightMouseDown:(NSEvent *)e { button_event(self, e, true); }
- (void)rightMouseUp:(NSEvent *)e { button_event(self, e, false); }
- (void)otherMouseDown:(NSEvent *)e { button_event(self, e, true); }
- (void)otherMouseUp:(NSEvent *)e { button_event(self, e, false); }

/* WM_MOUSEWHEEL: 120 (WHEEL_DELTA) for one wheel notch. A trackpad gives
   precise deltas in points; 10 points are one notch, and the rest is kept
   for the next event. */
- (void)scrollWheel:(NSEvent *)e
{
    double units, whole;
    if (e.hasPreciseScrollingDeltas)
        units = self.scrollRest + e.scrollingDeltaY * 12.0;
    else
        units = self.scrollRest + e.scrollingDeltaY * 120.0;
    whole = trunc(units);
    self.scrollRest = units - whole;
    if (whole > 32767.0) whole = 32767.0;
    if (whole < -32768.0) whole = -32768.0;
    if (whole != 0.0)
    {
        rt_dinput_on_wheel((int32_t)whole);
        mouse_event(self, e, WM_MOUSEWHEEL, (int32_t)whole);
    }
}

@end

/* ---------------------------------------------------------------- window */

/* The guest window of the key native window (0 when no runtime window is
   key). The input script sends its keys to it when a window is shown. */
static uint32_t key_hwnd;

@interface RTWindow : NSWindow <NSWindowDelegate>
@property(nonatomic) uint32_t hwnd;
@property(nonatomic) BOOL fullscreen;
@end

@implementation RTWindow

/* Borderless windows can become key only if the class allows it. */
- (BOOL)canBecomeKeyWindow { return YES; }
- (BOOL)canBecomeMainWindow { return YES; }

- (BOOL)windowShouldClose:(NSWindow *)sender
{
    (void)sender;
    rt_user32_on_close(self.hwnd);
    return NO; /* DestroyWindow closes the native window */
}

- (void)windowDidBecomeKey:(NSNotification *)n
{
    (void)n;
    if (self.fullscreen)
        NSApp.presentationOptions = NSApplicationPresentationHideDock | NSApplicationPresentationHideMenuBar;
    key_hwnd = self.hwnd;
    rt_user32_on_active(self.hwnd, true);
}

- (void)windowDidResignKey:(NSNotification *)n
{
    (void)n;
    if (self.fullscreen) NSApp.presentationOptions = NSApplicationPresentationDefault;
    if (key_hwnd == self.hwnd) key_hwnd = 0;
    rt_user32_on_active(self.hwnd, false);
}

- (void)windowDidResize:(NSNotification *)n { (void)n; rt_user32_on_frame(self.hwnd, true); }
- (void)windowDidMove:(NSNotification *)n { (void)n; rt_user32_on_frame(self.hwnd, false); }
- (void)windowDidMiniaturize:(NSNotification *)n { (void)n; rt_user32_on_minimized(self.hwnd, true); }
- (void)windowDidDeminiaturize:(NSNotification *)n { (void)n; rt_user32_on_minimized(self.hwnd, false); }

@end

/* ---------------------------------------------------------------- application */

@interface RTAppTarget : NSObject
- (void)quit:(id)sender;
@end

@implementation RTAppTarget
- (void)quit:(id)sender
{
    (void)sender;
    rt_user32_on_quit();
}
@end

static RTAppTarget *app_target;

/* ---------------------------------------------------------------- headless mode */

static bool headless;
static uint32_t headless_hwnd; /* the guest window reported as active */

bool rt_cocoa_headless(void)
{
    return headless;
}

static void headless_activate_cb(void *ctx)
{
    uint32_t hwnd = (uint32_t)(uintptr_t)ctx;
    if (hwnd == headless_hwnd) return;
    headless_hwnd = hwnd;
    rt_user32_on_active(hwnd, true);
}

/* Report hwnd as the active window after the current guest call returns
   (the main dispatch queue runs in rt_cocoa_pump). */
static void headless_activate(uint32_t hwnd)
{
    dispatch_async_f(dispatch_get_main_queue(), (void *)(uintptr_t)hwnd, headless_activate_cb);
}

/* RT_INPUT_SCRIPT: comma-separated steps, times in seconds from the start
   of the process.
     T:key          press and release key at T
     T1-T2/I:key    press and release key at T1, T1+I, ... up to T2
     T1-T2:key      hold key down from T1 to T2
   Keys: return escape space tab up down left right shift, or one letter
   or digit. The step "quit" does what the Quit menu item does (SC_CLOSE to
   every guest window) at the press time.
   The keys go to the active guest window: in headless mode the window
   that the guest shows, else the window that is key. While no window is
   active, the script waits. */
typedef struct script_event {
    double t;
    uint16_t keycode;
    uint16_t ch;
    bool down;
} script_event;

#define SCRIPT_MAX 4096
#define SCRIPT_QUIT 0xFFFFu /* keycode of the "quit" step */
static script_event script[SCRIPT_MAX];
static unsigned script_n, script_next;
static double script_start;
static dispatch_source_t script_timer;

static bool script_key(const char *name, uint16_t *keycode, uint16_t *ch)
{
    static const struct { const char *name; uint16_t code, ch; } named[] = {
        { "return", 36, '\r' }, { "escape", 53, 0x1b }, { "space", 49, ' ' }, { "tab", 48, '\t' },
        { "up", 126, 0xF700 }, { "down", 125, 0xF701 }, { "left", 123, 0xF702 }, { "right", 124, 0xF703 },
        { "shift", 56, 0 },
    };
    static const uint16_t letter[26] = { 0, 11, 8, 2, 14, 3, 5, 4, 34, 38, 40, 37, 46, 45, 31, 35, 12, 15, 1, 17, 32, 9, 13, 7, 16, 6 };
    static const uint16_t digit[10] = { 29, 18, 19, 20, 21, 23, 22, 26, 28, 25 };
    unsigned i;
    if (strcmp(name, "quit") == 0)
    {
        *keycode = SCRIPT_QUIT;
        *ch = 0;
        return true;
    }
    for (i = 0; i < sizeof named / sizeof named[0]; i++)
    {
        if (strcmp(name, named[i].name) == 0)
        {
            *keycode = named[i].code;
            *ch = named[i].ch;
            return true;
        }
    }
    if (name[0] >= 'a' && name[0] <= 'z' && name[1] == 0)
    {
        *keycode = letter[name[0] - 'a'];
        *ch = (uint16_t)name[0];
        return true;
    }
    if (name[0] >= '0' && name[0] <= '9' && name[1] == 0)
    {
        *keycode = digit[name[0] - '0'];
        *ch = (uint16_t)name[0];
        return true;
    }
    return false;
}

static void script_add(double t, uint16_t keycode, uint16_t ch, bool down)
{
    if (script_n == SCRIPT_MAX) rt_fatal("RT_INPUT_SCRIPT has more than %u key events", SCRIPT_MAX);
    script[script_n].t = t;
    script[script_n].keycode = keycode;
    script[script_n].ch = ch;
    script[script_n].down = down;
    script_n++;
}

static int script_cmp(const void *a, const void *b)
{
    const script_event *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t ? 1 : 0;
}

static double now_seconds(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) rt_fatal("clock_gettime failed");
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void script_tick(void *ctx)
{
    double t = now_seconds() - script_start;
    uint32_t target = headless ? headless_hwnd : key_hwnd;
    (void)ctx;
    while (script_next < script_n && script[script_next].t <= t && target != 0)
    {
        const script_event *e = &script[script_next++];
        if (e->keycode == SCRIPT_QUIT)
        {
            if (e->down)
            {
                rt_log("input script: %.2f s quit", e->t);
                rt_user32_on_quit();
            }
            continue;
        }
        rt_log("input script: %.2f s key %u %s", e->t, e->keycode, e->down ? "down" : "up");
        rt_user32_on_key(target, e->down, false, e->keycode, &e->ch, e->down && e->ch != 0 ? 1 : 0);
    }
    if (script_next == script_n) dispatch_source_cancel(script_timer);
}

static void script_parse(const char *text)
{
    char buf[8192], *step, *save = NULL;
    if (strlen(text) >= sizeof buf) rt_fatal("RT_INPUT_SCRIPT is longer than %zu characters", sizeof buf - 1);
    memcpy(buf, text, strlen(text) + 1);
    for (step = strtok_r(buf, ",", &save); step != NULL; step = strtok_r(NULL, ",", &save))
    {
        char *colon = strchr(step, ':'), *end;
        double t1, t2, iv = 0.0;
        uint16_t code, ch;
        if (colon == NULL) rt_fatal("RT_INPUT_SCRIPT step \"%s\" has no ':'", step);
        *colon = 0;
        if (!script_key(colon + 1, &code, &ch)) rt_fatal("RT_INPUT_SCRIPT: unknown key \"%s\"", colon + 1);
        t1 = strtod(step, &end);
        if (end == step || t1 < 0.0) rt_fatal("RT_INPUT_SCRIPT: bad time in \"%s\"", step);
        if (*end == 0)
        {
            script_add(t1, code, ch, true);
            script_add(t1 + 0.08, code, ch, false);
            continue;
        }
        if (*end != '-') rt_fatal("RT_INPUT_SCRIPT: bad time in \"%s\"", step);
        t2 = strtod(end + 1, &end);
        if (t2 < t1) rt_fatal("RT_INPUT_SCRIPT: end time before start time in \"%s\"", step);
        if (*end == 0)
        {
            script_add(t1, code, ch, true);
            script_add(t2, code, ch, false);
            continue;
        }
        if (*end != '/') rt_fatal("RT_INPUT_SCRIPT: bad interval in \"%s\"", step);
        iv = strtod(end + 1, &end);
        if (*end != 0 || iv < 0.1) rt_fatal("RT_INPUT_SCRIPT: the interval in \"%s\" must be at least 0.1 s", step);
        for (double t = t1; t <= t2; t += iv)
        {
            script_add(t, code, ch, true);
            script_add(t + 0.08, code, ch, false);
        }
    }
    qsort(script, script_n, sizeof script[0], script_cmp);
}

static void script_start_timer(void)
{
    const char *text = getenv("RT_INPUT_SCRIPT");
    if (text == NULL || text[0] == 0) return;
    script_parse(text);
    script_start = now_seconds();
    script_timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    if (script_timer == NULL) rt_fatal("the input script timer could not be made");
    dispatch_source_set_timer(script_timer, dispatch_time(DISPATCH_TIME_NOW, 0), 20 * NSEC_PER_MSEC, 5 * NSEC_PER_MSEC);
    dispatch_set_context(script_timer, NULL);
    dispatch_source_set_event_handler_f(script_timer, script_tick);
    dispatch_resume(script_timer);
}

void rt_cocoa_init(void)
{
    NSMenu *bar, *menu;
    NSMenuItem *item, *quit;
    NSString *name;
    need_main(__func__);
    if (app_target != nil) return;
    [NSApplication sharedApplication];
    {
        const char *h = getenv("RT_HEADLESS");
        headless = h != NULL && strcmp(h, "1") == 0;
    }
    if (headless)
    {
        /* No Dock icon, no menu bar, never the active application. */
        if (![NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited])
            rt_fatal("the application could not become a background application");
        rt_log("headless mode: no window is shown");
    }
    /* An application bundle starts with the regular policy. Change the
       policy only when it is different. */
    else if (NSApp.activationPolicy != NSApplicationActivationPolicyRegular &&
        ![NSApp setActivationPolicy:NSApplicationActivationPolicyRegular])
        rt_fatal("the application could not become a regular application (activation policy %ld)",
                 (long)NSApp.activationPolicy);
    app_target = [RTAppTarget new];
    name = NSProcessInfo.processInfo.processName;
    bar = [NSMenu new];
    item = [NSMenuItem new];
    [bar addItem:item];
    menu = [NSMenu new];
    quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name] action:@selector(quit:) keyEquivalent:@"q"];
    quit.target = app_target;
    [menu addItem:quit];
    item.submenu = menu;
    NSApp.mainMenu = bar;
    [NSApp finishLaunching];
    script_start_timer();
}

void rt_cocoa_screen_size(int32_t *w, int32_t *h)
{
    NSScreen *s;
    need_main(__func__);
    s = NSScreen.screens.firstObject;
    if (s == nil) rt_fatal("no display is available");
    *w = to_i32(s.frame.size.width);
    *h = to_i32(s.frame.size.height);
}

rt_rect rt_cocoa_frame_for_client(rt_rect client, unsigned kind)
{
    need_main(__func__);
    return from_ns([NSWindow frameRectForContentRect:to_ns(client) styleMask:mask_for(kind)]);
}

void *rt_cocoa_window_create(uint32_t hwnd, rt_rect client, unsigned kind, const char *title)
{
    RTWindow *w;
    RTView *v;
    NSRect content;
    need_main(__func__);
    if (app_target == nil) rt_fatal("rt_cocoa_init was not called");
    if (kind & RT_WIN_FULLSCREEN)
    {
        NSScreen *s = NSScreen.screens.firstObject;
        if (s == nil) rt_fatal("no display is available");
        content = s.frame;
    }
    else
    {
        content = to_ns(client);
    }
    w = [[RTWindow alloc] initWithContentRect:content styleMask:mask_for(kind) backing:NSBackingStoreBuffered defer:NO];
    if (w == nil) rt_fatal("NSWindow creation failed");
    w.hwnd = hwnd;
    w.fullscreen = (kind & RT_WIN_FULLSCREEN) != 0;
    w.releasedWhenClosed = NO;
    w.delegate = w;
    w.acceptsMouseMovedEvents = YES;
    w.title = [NSString stringWithUTF8String:title] ?: @"";
    w.backgroundColor = NSColor.blackColor;
    if (w.fullscreen) w.collectionBehavior = NSWindowCollectionBehaviorFullScreenNone;
    v = [[RTView alloc] initWithFrame:NSMakeRect(0, 0, content.size.width, content.size.height)];
    v.hwnd = hwnd;
    v.wantsLayer = YES;
    v.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    w.contentView = v;
    [w makeFirstResponder:v];
    return (void *)CFBridgingRetain(w);
}

static RTWindow *win(void *w)
{
    if (w == NULL) rt_fatal("native window reference is NULL");
    return (__bridge RTWindow *)w;
}

void rt_cocoa_window_destroy(void *w)
{
    RTWindow *x;
    need_main(__func__);
    x = (RTWindow *)CFBridgingRelease(w);
    if (x.fullscreen && x.isKeyWindow) NSApp.presentationOptions = NSApplicationPresentationDefault;
    x.delegate = nil;
    [x orderOut:nil];
    [x close];
}

void rt_cocoa_window_show(void *w, bool visible, bool activate)
{
    RTWindow *x;
    need_main(__func__);
    x = win(w);
    if (headless)
    {
        if (visible) headless_activate(x.hwnd);
        return;
    }
    if (!visible)
    {
        [x orderOut:nil];
        return;
    }
    if (activate)
    {
        [NSApp activateIgnoringOtherApps:YES];
        [x makeKeyAndOrderFront:nil];
    }
    else
    {
        [x orderFront:nil];
    }
}

void rt_cocoa_window_minimize(void *w)
{
    need_main(__func__);
    [win(w) miniaturize:nil];
}

rt_rect rt_cocoa_window_client(void *w)
{
    RTWindow *x;
    need_main(__func__);
    x = win(w);
    return from_ns([x contentRectForFrameRect:x.frame]);
}

void *rt_cocoa_window_layer(void *w)
{
    CALayer *l;
    need_main(__func__);
    l = win(w).contentView.layer;
    if (![l isKindOfClass:[CAMetalLayer class]]) rt_fatal("window content layer is not a CAMetalLayer");
    return (__bridge void *)l;
}

void rt_cocoa_pump(void)
{
    need_main(__func__);
    @autoreleasepool
    {
        for (;;)
        {
            NSEvent *e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
            if (e == nil) break;
            [NSApp sendEvent:e];
        }
    }
}

void rt_cocoa_activate(void *w)
{
    need_main(__func__);
    if (headless)
    {
        headless_activate(win(w).hwnd);
        return;
    }
    [NSApp activateIgnoringOtherApps:YES];
    [win(w) makeKeyAndOrderFront:nil];
}

rt_rect rt_cocoa_client_for_frame(rt_rect frame, unsigned kind)
{
    need_main(__func__);
    return from_ns([NSWindow contentRectForFrameRect:to_ns(frame) styleMask:mask_for(kind)]);
}

static void cursor_show(void *ctx)
{
    (void)ctx;
    [NSCursor unhide];
}

static void cursor_hide(void *ctx)
{
    (void)ctx;
    [NSCursor hide];
}

/* Any thread: the change runs on the main thread (the main dispatch queue
   runs while rt_cocoa_pump waits for events). */
void rt_cocoa_cursor_visible(bool visible)
{
    if (headless) return;
    if ([NSThread isMainThread])
        visible ? cursor_show(NULL) : cursor_hide(NULL);
    else
        dispatch_async_f(dispatch_get_main_queue(), NULL, visible ? cursor_show : cursor_hide);
}

static void capture_on(void *ctx)
{
    NSWindow *k = NSApp.keyWindow;
    CGError e;
    (void)ctx;
    [NSCursor hide];
    if (k != nil)
    {
        /* Quartz display coordinates: the origin is the top left of the
           main display, y goes down. */
        NSRect f = k.frame;
        CGFloat top = NSScreen.screens.count > 0 ? NSMaxY(NSScreen.screens[0].frame) : NSMaxY(f);
        e = CGWarpMouseCursorPosition(CGPointMake(NSMidX(f), top - NSMidY(f)));
        if (e != kCGErrorSuccess) rt_fatal("the cursor could not move to the window centre (CGError %d)", (int)e);
    }
    e = CGAssociateMouseAndMouseCursorPosition(false);
    if (e != kCGErrorSuccess) rt_fatal("the mouse could not be separated from the cursor (CGError %d)", (int)e);
}

static void capture_off(void *ctx)
{
    CGError e;
    (void)ctx;
    e = CGAssociateMouseAndMouseCursorPosition(true);
    if (e != kCGErrorSuccess) rt_fatal("the mouse could not be attached to the cursor again (CGError %d)", (int)e);
    [NSCursor unhide];
}

void rt_cocoa_mouse_capture(bool on)
{
    if (headless) return;
    if ([NSThread isMainThread])
        on ? capture_on(NULL) : capture_off(NULL);
    else
        dispatch_async_f(dispatch_get_main_queue(), NULL, on ? capture_on : capture_off);
}

bool rt_cocoa_caps_lock(void)
{
    return (NSEvent.modifierFlags & NSEventModifierFlagCapsLock) != 0;
}
