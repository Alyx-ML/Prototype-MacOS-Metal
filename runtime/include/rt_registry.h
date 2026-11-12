/* The emulated registry (runtime/src/rt_registry.c). */
#ifndef RT_REGISTRY_H
#define RT_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

/* Load the registry from the host file at path, or make the first-start
   values when the file does not exist. Changes are saved to this file. */
void rt_registry_init(const char *path);

/* Read a REG_SZ value for the runtime. root is a predefined key
   (0x80000000 HKEY_CLASSES_ROOT to 0x80000003 HKEY_USERS), path is a
   backslash-separated subkey path and name is the value name ("" for the
   default value). Returns 0 on success, ERROR_FILE_NOT_FOUND if the key or
   value does not exist or is not REG_SZ, and ERROR_MORE_DATA if the string
   with its terminator does not fit into n bytes. */
uint32_t rt_registry_get_sz(uint32_t root, const char *path, const char *name, char *out, size_t n);

#endif
