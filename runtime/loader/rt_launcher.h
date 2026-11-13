/*
 * The launcher window of the loader (rt_launcher.m).
 *
 * The window opens before the game starts. The player can:
 *   - select the game folder (the window shows if the folder can be used),
 *   - change the Mac enhancement settings (the settings file of rt_enh.c),
 *   - restore the default settings,
 *   - start the game (Play) or close the window (quit).
 */
#ifndef RT_LAUNCHER_H
#define RT_LAUNCHER_H

#import <Foundation/Foundation.h>

#include <stdbool.h>
#include <stddef.h>

/* The problem of a game folder for the player, or nil if the folder can be
   used. */
typedef NSString *_Nullable (*rt_launcher_check_fn)(const char *_Nonnull dir);

/* Show the window and wait for the player. settings is the path of the
   settings file. initial is a game folder that the loader found, or NULL.
   For Play: the settings file has the values of the window, out has the
   game folder, and the result is true. For a closed window the result is
   false. */
bool rt_launcher_run(const char *_Nonnull settings, const char *_Nullable initial, rt_launcher_check_fn _Nonnull check,
                     char *_Nonnull out, size_t n);

#endif
