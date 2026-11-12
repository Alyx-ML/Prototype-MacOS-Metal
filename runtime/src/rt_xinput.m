/*
 * XINPUT1_3 on GameController.framework: XInputGetState, XInputSetState
 * and XInputGetCapabilities.
 *
 * The first four controllers with an extended gamepad profile get the
 * XInput user indices 0-3 in connection order. Each controller reports its
 * values on a private serial queue; the handler stores a snapshot under a
 * lock, so that XInputGetState works on any thread.
 *
 * Vibration: the left motor (low frequency) is the haptic engine of the
 * left handle, the right motor (high frequency) the right handle. A
 * controller without haptics accepts XInputSetState without vibration, as
 * a Windows controller without motors does.
 */
#import <CoreHaptics/CoreHaptics.h>
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>

#include "rt.h"

#include <pthread.h>

#define XUSER_MAX_COUNT 4
#define ERROR_DEVICE_NOT_CONNECTED 1167u
#define ERROR_BAD_ARGUMENTS 160u

typedef struct pad_state {
    bool connected;
    uint32_t packet;
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
    bool haptics;
} pad_state;

static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static pad_state pads[XUSER_MAX_COUNT];
static GCController *__strong pad_ctl[XUSER_MAX_COUNT];
static CHHapticEngine *__strong engines[XUSER_MAX_COUNT][2];
static id<CHHapticAdvancedPatternPlayer> __strong players[XUSER_MAX_COUNT][2];
static dispatch_queue_t pad_queue;

static void plock(void)
{
    if (pthread_mutex_lock(&pad_lock) != 0) rt_fatal("XInput lock failed");
}

static void punlock(void)
{
    if (pthread_mutex_unlock(&pad_lock) != 0) rt_fatal("XInput unlock failed");
}

static int16_t axis(float v)
{
    double d = (double)v * 32767.0;
    if (d > 32767.0) d = 32767.0;
    if (d < -32768.0) d = -32768.0;
    return (int16_t)lrint(d);
}

static uint8_t trigger(float v)
{
    double d = (double)v * 255.0;
    if (d > 255.0) d = 255.0;
    if (d < 0.0) d = 0.0;
    return (uint8_t)lrint(d);
}

/* Store the values of gamepad g for slot i (pad queue). */
static void update(int i, GCExtendedGamepad *g)
{
    pad_state s;
    uint16_t b = 0;
    if (g.dpad.up.pressed) b |= 0x0001;
    if (g.dpad.down.pressed) b |= 0x0002;
    if (g.dpad.left.pressed) b |= 0x0004;
    if (g.dpad.right.pressed) b |= 0x0008;
    if (g.buttonMenu.pressed) b |= 0x0010;          /* START */
    if (g.buttonOptions.pressed) b |= 0x0020;       /* BACK */
    if (g.leftThumbstickButton.pressed) b |= 0x0040;
    if (g.rightThumbstickButton.pressed) b |= 0x0080;
    if (g.leftShoulder.pressed) b |= 0x0100;
    if (g.rightShoulder.pressed) b |= 0x0200;
    if (g.buttonA.pressed) b |= 0x1000;
    if (g.buttonB.pressed) b |= 0x2000;
    if (g.buttonX.pressed) b |= 0x4000;
    if (g.buttonY.pressed) b |= 0x8000;
    plock();
    s = pads[i];
    s.buttons = b;
    s.lt = trigger(g.leftTrigger.value);
    s.rt = trigger(g.rightTrigger.value);
    s.lx = axis(g.leftThumbstick.xAxis.value);
    s.ly = axis(g.leftThumbstick.yAxis.value);
    s.rx = axis(g.rightThumbstick.xAxis.value);
    s.ry = axis(g.rightThumbstick.yAxis.value);
    if (memcmp(&s, &pads[i], sizeof s) != 0) s.packet = pads[i].packet + 1;
    pads[i] = s;
    punlock();
}

static void attach(GCController *c)
{
    int i, slot = -1;
    GCExtendedGamepad *g = c.extendedGamepad;
    if (g == nil)
    {
        rt_log("XInput: controller \"%s\" has no extended gamepad profile; it is not used", c.vendorName.UTF8String ?: "");
        return;
    }
    plock();
    for (i = 0; i < XUSER_MAX_COUNT; i++)
    {
        if (pad_ctl[i] == c)
        {
            punlock();
            return;
        }
        if (pad_ctl[i] == nil && slot < 0) slot = i;
    }
    if (slot < 0)
    {
        punlock();
        rt_log("XInput: controller \"%s\" is not used (four controllers are connected)", c.vendorName.UTF8String ?: "");
        return;
    }
    pad_ctl[slot] = c;
    memset(&pads[slot], 0, sizeof pads[slot]);
    pads[slot].connected = true;
    pads[slot].haptics = c.haptics != nil;
    punlock();
    c.playerIndex = (GCControllerPlayerIndex)slot;
    c.handlerQueue = pad_queue;
    g.valueChangedHandler = ^(GCExtendedGamepad *pad, GCControllerElement *e) {
        (void)e;
        update(slot, pad);
    };
    dispatch_async(pad_queue, ^{
        update(slot, g);
    });
}

static void detach(GCController *c)
{
    int i, k;
    plock();
    for (i = 0; i < XUSER_MAX_COUNT; i++)
    {
        if (pad_ctl[i] != c) continue;
        pad_ctl[i] = nil;
        memset(&pads[i], 0, sizeof pads[i]);
        for (k = 0; k < 2; k++)
        {
            [engines[i][k] stopWithCompletionHandler:nil];
            engines[i][k] = nil;
            players[i][k] = nil;
        }
    }
    punlock();
}

static void init_once(void *ctx)
{
    (void)ctx;
    pad_queue = dispatch_queue_create("rt.xinput", DISPATCH_QUEUE_SERIAL);
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification object:nil queue:nil
                                                usingBlock:^(NSNotification *n) { attach(n.object); }];
    [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidDisconnectNotification object:nil queue:nil
                                                usingBlock:^(NSNotification *n) { detach(n.object); }];
    for (GCController *c in GCController.controllers) attach(c);
}

static void init_pads(void)
{
    static dispatch_once_t once;
    dispatch_once_f(&once, NULL, init_once);
}

/* DWORD XInputGetState(DWORD user, XINPUT_STATE *)
   XINPUT_STATE (16 bytes): dwPacketNumber, wButtons, bLeftTrigger,
   bRightTrigger, sThumbLX, sThumbLY, sThumbRX, sThumbRY. */
void rt_XInputGetState(_cpu *cpu)
{
    uint32_t user = ARG(0), p = ARG(1);
    pad_state s;
    init_pads();
    if (user >= XUSER_MAX_COUNT || p == 0)
    {
        RET(ERROR_BAD_ARGUMENTS);
        return;
    }
    plock();
    s = pads[user];
    punlock();
    if (!s.connected)
    {
        RET(ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    wr32(p, s.packet);
    wr16(p + 4, s.buttons);
    wr8(p + 6, s.lt);
    wr8(p + 7, s.rt);
    wr16(p + 8, (uint16_t)s.lx);
    wr16(p + 10, (uint16_t)s.ly);
    wr16(p + 12, (uint16_t)s.rx);
    wr16(p + 14, (uint16_t)s.ry);
    RET(0);
}

/* Set the intensity of one motor (pad lock held). */
static void motor(int user, int k, uint16_t speed)
{
    NSError *err = nil;
    GCController *c = pad_ctl[user];
    float v = (float)speed / 65535.0f;
    if (c.haptics == nil) return;
    if (speed == 0)
    {
        if (players[user][k] != nil && ![players[user][k] stopAtTime:CHHapticTimeImmediate error:&err])
            rt_log("XInput: vibration stop failed: %s", err.localizedDescription.UTF8String ?: "");
        players[user][k] = nil;
        return;
    }
    if (engines[user][k] == nil)
    {
        engines[user][k] = [c.haptics createEngineWithLocality:k == 0 ? GCHapticsLocalityLeftHandle : GCHapticsLocalityRightHandle];
        if (engines[user][k] == nil) engines[user][k] = [c.haptics createEngineWithLocality:GCHapticsLocalityDefault];
        if (engines[user][k] == nil)
        {
            rt_log("XInput: the controller has no haptic engine for motor %d", k);
            return;
        }
        if (![engines[user][k] startAndReturnError:&err])
        {
            rt_log("XInput: haptic engine start failed: %s", err.localizedDescription.UTF8String ?: "");
            engines[user][k] = nil;
            return;
        }
    }
    if (players[user][k] == nil)
    {
        CHHapticEvent *e = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                         parameters:@[ [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:1.0f] ]
                                                       relativeTime:0
                                                           duration:30.0];
        CHHapticPattern *pat = [[CHHapticPattern alloc] initWithEvents:@[ e ] parameters:@[] error:&err];
        id<CHHapticAdvancedPatternPlayer> pl = pat != nil ? [engines[user][k] createAdvancedPlayerWithPattern:pat error:&err] : nil;
        if (pl == nil)
        {
            rt_log("XInput: vibration pattern creation failed: %s", err.localizedDescription.UTF8String ?: "");
            return;
        }
        pl.loopEnabled = YES;
        if (![pl startAtTime:CHHapticTimeImmediate error:&err])
        {
            rt_log("XInput: vibration start failed: %s", err.localizedDescription.UTF8String ?: "");
            return;
        }
        players[user][k] = pl;
    }
    if (![players[user][k] sendParameters:@[ [[CHHapticDynamicParameter alloc] initWithParameterID:CHHapticDynamicParameterIDHapticIntensityControl value:v relativeTime:0] ]
                                   atTime:CHHapticTimeImmediate
                                    error:&err])
        rt_log("XInput: vibration change failed: %s", err.localizedDescription.UTF8String ?: "");
}

/* DWORD XInputSetState(DWORD user, XINPUT_VIBRATION *)
   XINPUT_VIBRATION: wLeftMotorSpeed, wRightMotorSpeed. */
void rt_XInputSetState(_cpu *cpu)
{
    uint32_t user = ARG(0), p = ARG(1);
    init_pads();
    if (user >= XUSER_MAX_COUNT || p == 0)
    {
        RET(ERROR_BAD_ARGUMENTS);
        return;
    }
    plock();
    if (!pads[user].connected)
    {
        punlock();
        RET(ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    @autoreleasepool
    {
        motor((int)user, 0, rd16(p));
        motor((int)user, 1, rd16(p + 2));
    }
    punlock();
    RET(0);
}

/* DWORD XInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES *)
   XINPUT_CAPABILITIES (20 bytes): Type, SubType, Flags, Gamepad (the
   resolution of each control), Vibration. */
void rt_XInputGetCapabilities(_cpu *cpu)
{
    uint32_t user = ARG(0), flags = ARG(1), p = ARG(2);
    pad_state s;
    init_pads();
    if (user >= XUSER_MAX_COUNT || p == 0 || (flags & ~1u))
    {
        RET(ERROR_BAD_ARGUMENTS);
        return;
    }
    plock();
    s = pads[user];
    punlock();
    if (!s.connected)
    {
        RET(ERROR_DEVICE_NOT_CONNECTED);
        return;
    }
    memset(G2H(p), 0, 20);
    wr8(p, 1);           /* XINPUT_DEVTYPE_GAMEPAD */
    wr8(p + 1, 1);       /* XINPUT_DEVSUBTYPE_GAMEPAD */
    wr16(p + 4, 0xF3FF); /* every button */
    wr8(p + 6, 0xFF);
    wr8(p + 7, 0xFF);
    wr16(p + 8, 0xFFC0);
    wr16(p + 10, 0xFFC0);
    wr16(p + 12, 0xFFC0);
    wr16(p + 14, 0xFFC0);
    if (s.haptics)
    {
        wr16(p + 16, 0xFFFF);
        wr16(p + 18, 0xFFFF);
    }
    RET(0);
}
