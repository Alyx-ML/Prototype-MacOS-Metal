/*
 * Process start in the game bundle: start the runtime and run the entry
 * point of prototypef.exe on the main thread.
 *
 * The loader (runtime/loader/rt_loader.m) finds the game directory, checks
 * the SHA-256 values of the game files, opens the game bundle and calls
 * rt_game_main (see rt_game.h).
 *
 * Host directories: ~/Library/Application Support/Prototype Mac Port/
 *   User          the guest user directory (C:\Users\Player)
 *   ProgramData   C:\ProgramData
 *   registry.txt  the emulated registry
 *
 * The guest runs on the main thread, because the window system works only
 * on the main thread. The guest message loop handles the native events
 * (PeekMessageA calls rt_cocoa_pump).
 */
#include "rt.h"
#include "rt_crt.h"
#include "rt_file.h"
#include "rt_game.h"
#include "rt_module.h"
#include "rt_registry.h"
#include "rt_thread.h"
#include "rt_user32.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define PORT_NAME "Prototype Mac Port"

_Noreturn void rt_game_main(const char *game_dir) __attribute__((visibility("default")));

static bool join_path(char *out, size_t n, const char *dir, const char *name)
{
    int r = snprintf(out, n, "%s/%s", dir, name);
    return r > 0 && (size_t)r < n;
}

_Noreturn void rt_game_main(const char *game_dir)
{
    char base[RT_HOST_PATH], user[RT_HOST_PATH], data[RT_HOST_PATH], reg[RT_HOST_PATH];
    const char *home = getenv("HOME");
    rt_thread *t;
    uint32_t code;
    rt_game_main_fn check = rt_game_main;

    (void)check; /* rt_game_main must agree with rt_game_main_fn */
    if (pthread_main_np() == 0) rt_fatal("rt_game_main must run on the main thread");
    if (game_dir == NULL || game_dir[0] != '/') rt_fatal("the game directory must be an absolute path");
    if (home == NULL || home[0] != '/') rt_fatal("HOME is not set");
    if (!join_path(base, sizeof base, home, "Library/Application Support/" PORT_NAME) ||
        !join_path(user, sizeof user, base, "User") || !join_path(data, sizeof data, base, "ProgramData") ||
        !join_path(reg, sizeof reg, base, "registry.txt"))
        rt_fatal("the home directory path is too long");
    rt_log("game directory %s", game_dir);

    rt_mem_init();
    /* The data of the translated modules holds pointers (vtables, function
       tables, the C++ initializer tables). They get their guest values
       here; before this call they are 0. */
    if (ptr_initialize_pointers == NULL) rt_fatal("the translated modules have no pointer initialization");
    ptr_initialize_pointers(rt_ptrofs);
    t = rt_thread_init_main();
    rt_modules_init(game_dir);
    rt_fs_init(game_dir, user, data);
    rt_registry_init(reg);
    rt_crt_set_command_line("\"" RT_GUEST_GAME_DIR "\\prototypef.exe\"");
    rt_user32_init();

    /* The entry point of prototypef.exe (the C runtime start code) calls
       ExitProcess. A return also ends the process with its value. The main
       thread gets no DLL_THREAD_ATTACH notification, as on Windows. */
    code = rt_call(t->cpu, H2G((const void *)&launcher_entry), true, 0, 0, NULL);
    rt_process_exit(code);
}
