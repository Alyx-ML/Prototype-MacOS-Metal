/*
 * The Metal device and the GPU description for the guest.
 *
 * The emulated system reports the Metal device as its video controller.
 * The video memory size is the recommended working set size of the device
 * (the memory that the GPU can use without a loss of performance), limited
 * to 0x7FF00000 bytes (2047 MiB). The guest reads the size as a signed
 * 32-bit value (WMI AdapterRAM is returned as VT_I4), so a larger value
 * would be negative.
 */
#import <Metal/Metal.h>

#include "rt.h"
#include "rt_com.h"

#include <dispatch/dispatch.h>
#include <stdio.h>

#define MAX_ADAPTER_RAM 0x7FF00000u

static id<MTLDevice> gpu_device; /* kept for the life of the process */
static char gpu_name[256];
static uint32_t gpu_ram;

static void gpu_init(void *ctx)
{
    (void)ctx;
    @autoreleasepool
    {
        uint64_t ws;
        gpu_device = MTLCreateSystemDefaultDevice();
        if (gpu_device == nil) rt_fatal("no Metal device is available");
        snprintf(gpu_name, sizeof gpu_name, "%s", [[gpu_device name] UTF8String]);
        ws = [gpu_device recommendedMaxWorkingSetSize];
        gpu_ram = ws > MAX_ADAPTER_RAM ? MAX_ADAPTER_RAM : (uint32_t)ws;
    }
}

static void gpu_once(void)
{
    static dispatch_once_t once;
    dispatch_once_f(&once, NULL, gpu_init);
}

uint32_t rt_gpu_adapter_ram(void)
{
    gpu_once();
    return gpu_ram;
}

const char *rt_gpu_name(void)
{
    gpu_once();
    return gpu_name;
}

id<MTLDevice> rt_gpu_device(void)
{
    gpu_once();
    return gpu_device;
}
