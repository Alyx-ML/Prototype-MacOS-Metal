/*
 * Input events for DINPUT8 (rt_dinput.c).
 *
 * The window system calls these functions on the main thread when native
 * input arrives. DINPUT8 keeps the keyboard and mouse state for its device
 * objects. The functions lock the DINPUT8 state; guest threads read the
 * state through IDirectInputDevice8A::GetDeviceState.
 */
#ifndef RT_DINPUT_H
#define RT_DINPUT_H

#include <stdbool.h>
#include <stdint.h>

/* A key changed. dik: the DirectInput key code (the set 1 scan code, with
   0x80 for an extended key). */
void rt_dinput_on_key(uint8_t dik, bool down);

/* The mouse moved. dx, dy: the native relative motion (NSEvent deltaX and
   deltaY; y increases downward). */
void rt_dinput_on_mouse_motion(double dx, double dy);

/* A mouse button changed. button: 0 left, 1 right, 2 middle, 3 and up the
   other buttons (NSEvent buttonNumber). */
void rt_dinput_on_mouse_button(int button, bool down);

/* The wheel turned. delta: WHEEL_DELTA units (120 for one notch; a
   positive value is a turn away from the user). */
void rt_dinput_on_wheel(int32_t delta);

/* Top-level window hwnd became the key window (active) or stopped being
   the key window. */
void rt_dinput_on_focus(uint32_t hwnd, bool active);

#endif
