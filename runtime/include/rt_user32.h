/* USER32 functions that other parts of the runtime use (rt_user32.c). */
#ifndef RT_USER32_H
#define RT_USER32_H

#include <stdbool.h>
#include <stdint.h>

/* PostMessageA semantics: put a message into the queue of the thread that
   owns window hwnd. Returns false if hwnd is not a window (the message is
   then not posted). Any thread can call this function. */
bool rt_post_message(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam);

/* Start the window system (main thread, before guest code runs). */
void rt_user32_init(void);

/* The CAMetalLayer of the content view of top-level window hwnd (main
   thread only). Returns NULL if hwnd is not a top-level window. */
void *rt_user32_window_layer(uint32_t hwnd);

/* The client size of top-level window hwnd. Returns false if hwnd is not a
   top-level window. Any thread can call this function. */
bool rt_user32_client_size(uint32_t hwnd, int32_t *w, int32_t *h);

/* The DirectInput key codes (set 1 scan codes, 0x80 for an extended key)
   that the Mac keyboard can give, in ascending order, without
   duplicates. Returns the number of codes written to out. */
int rt_user32_dik_codes(uint8_t out[256]);

#endif
