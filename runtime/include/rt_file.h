/*
 * The emulated file system.
 *
 * The guest sees one drive, C:. Guest paths are case-insensitive. Each
 * mount maps one guest directory to one host directory:
 *   C:\Prototype      the game directory (the directory of the game files)
 *   C:\Users\Player   the user directory of the port (saved games and
 *                     settings), on the host in
 *                     ~/Library/Application Support/<port name>/User
 *   C:\ProgramData    ~/Library/Application Support/<port name>/ProgramData
 * Other guest paths do not exist (ERROR_PATH_NOT_FOUND).
 */
#ifndef RT_FILE_H
#define RT_FILE_H

#include "rt.h"

#include <stdbool.h>
#include <sys/stat.h>

#define RT_MAX_PATH 260
#define RT_HOST_PATH 1024

/* Set the host directories. The game directory must exist; the other
   directories are created when necessary. */
void rt_fs_init(const char *game_dir, const char *user_dir, const char *programdata_dir);

/* Make an absolute, normalized guest path ("C:\dir\file") from a guest
   path. Returns 0 or a Win32 error code. */
uint32_t rt_path_full(const char *in, char *out, size_t n);

/* Map a guest path to a host path. Each existing component is found
   without regard to case. Returns 0 or a Win32 error code:
   ERROR_PATH_NOT_FOUND if a directory component does not exist (the last
   component can be missing; *exists tells whether it exists). */
uint32_t rt_path_host(const char *guest, char *host, size_t n, bool *exists);

/* Win32 error code for an errno value. not_found is the code for ENOENT. */
uint32_t rt_errno_to_win32(int e, uint32_t not_found);

/* FILETIME (100 ns intervals since 1601) for a host time. */
uint64_t rt_filetime_from_timespec(struct timespec ts);
/* Win32 file attributes of a host file. */
uint32_t rt_file_attributes(const struct stat *st, const char *name);

/* Host file descriptor of a file handle, or -1 if h is not a file handle. */
int rt_file_fd(uint32_t h);

/* Wildcard match of a Windows file name pattern (case-insensitive). */
bool rt_wildcard_match(const char *pattern, const char *name);

#endif
