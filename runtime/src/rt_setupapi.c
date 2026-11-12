/*
 * SETUPAPI: device information sets.
 *
 * The engine enumerates kernel streaming audio interfaces
 * (KSCATEGORY_AUDIO and KSCATEGORY_RENDER) for its WDM kernel streaming
 * audio driver. The emulated system gives audio output only through the
 * audio interfaces that the runtime implements (XAudio2), not through
 * kernel streaming. So every device information set is empty: the
 * enumeration ends at once with ERROR_NO_MORE_ITEMS.
 *
 * The functions that take a device or interface of a set
 * (SetupDiGetDeviceInterfaceDetailA, SetupDiGetDeviceInterfaceAlias,
 * SetupDiGetDeviceRegistryPropertyA) cannot get a valid element of an
 * empty set; a call stops the process with a message.
 */
#include "rt.h"
#include "rt_object.h"

#include <stdbool.h>

#define INVALID_HANDLE_VALUE 0xFFFFFFFFu
#define ERROR_INVALID_FLAGS 1004u
#define ERROR_INVALID_USER_BUFFER 1784u

#define DIGCF_DEFAULT 0x01u
#define DIGCF_PRESENT 0x02u
#define DIGCF_ALLCLASSES 0x04u
#define DIGCF_PROFILE 0x08u
#define DIGCF_DEVICEINTERFACE 0x10u

/* SP_DEVICE_INTERFACE_DATA size for 32-bit programs. */
#define SP_DEVICE_INTERFACE_DATA_SIZE 28u

static bool valid_set(uint32_t h)
{
    rt_object *o = rt_handle_get(h, OBJ_DEVINFO);
    if (o == NULL) return false;
    rt_object_unref(o);
    return true;
}

/* HDEVINFO SetupDiGetClassDevsA(const GUID *, PCSTR enumerator, HWND,
   DWORD flags) */
void rt_SetupDiGetClassDevsA(_cpu *cpu)
{
    uint32_t guid = ARG(0), flags = ARG(3), h;
    rt_object *o;
    if (flags & ~(DIGCF_DEFAULT | DIGCF_PRESENT | DIGCF_ALLCLASSES | DIGCF_PROFILE | DIGCF_DEVICEINTERFACE))
    {
        rt_set_last_error(ERROR_INVALID_FLAGS);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    if (guid == 0 && (flags & DIGCF_ALLCLASSES) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    o = rt_object_new(OBJ_DEVINFO);
    h = rt_handle_new(o);
    rt_object_unref(o);
    if (h == 0)
    {
        rt_set_last_error(ERROR_NOT_ENOUGH_MEMORY);
        RET(INVALID_HANDLE_VALUE);
        return;
    }
    RET(h);
}

/* BOOL SetupDiDestroyDeviceInfoList(HDEVINFO) */
void rt_SetupDiDestroyDeviceInfoList(_cpu *cpu)
{
    uint32_t h = ARG(0);
    if (!valid_set(h) || !rt_handle_close(h))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    RET(1);
}

/* BOOL SetupDiEnumDeviceInterfaces(HDEVINFO, PSP_DEVINFO_DATA,
   const GUID *, DWORD index, PSP_DEVICE_INTERFACE_DATA) */
void rt_SetupDiEnumDeviceInterfaces(_cpu *cpu)
{
    uint32_t h = ARG(0), data = ARG(4);
    if (!valid_set(h))
    {
        rt_set_last_error(ERROR_INVALID_HANDLE);
        RET(0);
        return;
    }
    if (ARG(1) != 0) rt_fatal("SetupDiEnumDeviceInterfaces with device information: the set has no devices");
    if (ARG(2) == 0)
    {
        rt_set_last_error(ERROR_INVALID_PARAMETER);
        RET(0);
        return;
    }
    if (data == 0 || rd32(data) != SP_DEVICE_INTERFACE_DATA_SIZE)
    {
        rt_set_last_error(ERROR_INVALID_USER_BUFFER);
        RET(0);
        return;
    }
    rt_set_last_error(ERROR_NO_MORE_ITEMS);
    RET(0);
}

void rt_SetupDiGetDeviceInterfaceDetailA(_cpu *cpu)
{
    (void)cpu;
    rt_fatal("SetupDiGetDeviceInterfaceDetailA: the device information sets of the emulated system have no interfaces");
}

void rt_SetupDiGetDeviceInterfaceAlias(_cpu *cpu)
{
    (void)cpu;
    rt_fatal("SetupDiGetDeviceInterfaceAlias: the device information sets of the emulated system have no interfaces");
}

void rt_SetupDiGetDeviceRegistryPropertyA(_cpu *cpu)
{
    (void)cpu;
    rt_fatal("SetupDiGetDeviceRegistryPropertyA: the device information sets of the emulated system have no devices");
}
