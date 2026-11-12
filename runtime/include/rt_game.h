/* The interface between the loader (runtime/loader) and the game bundle.
 *
 * The game bundle holds the runtime and the translated code of the game.
 * The loader finds and checks the game files, makes the bundle (the
 * converter) when necessary, opens the bundle with dlopen and calls
 * RT_GAME_MAIN_SYMBOL on the main thread.
 */
#ifndef RT_GAME_H
#define RT_GAME_H

/* The dlsym name of rt_game_main. */
#define RT_GAME_MAIN_SYMBOL "rt_game_main"

/* Start the runtime and run the game. game_dir is the host directory that
   holds prototypef.exe and prototypeenginef.dll; the loader checked their
   SHA-256 values. Does not return: the process ends when the game ends. */
typedef void (*rt_game_main_fn)(const char *game_dir);

#endif
