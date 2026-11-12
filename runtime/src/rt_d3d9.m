/*
 * Direct3D 9 on Metal: Direct3DCreate9, IDirect3D9 (adapter, formats,
 * capabilities, CreateDevice) and the IDirect3DDevice9 methods for device
 * state, resources and presentation. Drawing is in rt_d3d9_draw.m and the
 * resources are in rt_d3d9_res.m. See rt_d3d9_int.h.
 *
 * The runtime reports one adapter (the Metal device) of type D3DDEVTYPE_HAL.
 * The display modes are the modes of the main display in points (the
 * logical size that macOS shows). A full-screen device does not change the
 * display mode: its window covers the display and Present scales the back
 * buffer with its aspect ratio kept.
 *
 * Reference counts: a device holds one reference to each object that is
 * bound to it (render targets, depth-stencil surface, textures, streams,
 * index buffer, declaration, shaders) and to the IDirect3D9 object.
 * Resources do not hold references to the device; a resource that is used
 * after the last device reference is gone still works, except GetDevice.
 */
#import "rt_d3d9_int.h"

#import <CoreGraphics/CoreGraphics.h>

#include "rt_user32.h"

#include <dispatch/dispatch.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define D3D_SDK_VERSION 32u
#define D3DDEVTYPE_HAL 1u
#define D3DADAPTER_DEFAULT 0u
#define MAX_MODES 128u
#define UPLOAD_BLOCK (4u << 20)
#define MAX_UPLOAD_FREE 8u
#define MAX_FVF_DECLS 256u

#define D3DCREATE_MULTITHREADED 0x4u
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x20u
#define D3DCREATE_HARDWARE_VERTEXPROCESSING 0x40u
#define D3DCREATE_MIXED_VERTEXPROCESSING 0x80u

#define D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 1u

/* UNCONFIRMED: recalled IIDs (d3d9.h); QueryInterface with an unknown IID
   stops the process with the IID text. */
static const rt_guid IID_IDirect3D9 = { 0x81BDCBCAu, 0x64D4, 0x426D, { 0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C } };
static const rt_guid IID_IDirect3DDevice9 = { 0xD0223B96u, 0xBF7A, 0x43FD, { 0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB } };

static void device_destroy(rt_com_object *o);

static const rt_com_iface d3d9_ifaces[] = { { &IID_IDirect3D9, 0 } };
static const rt_com_iface device_ifaces[] = { { &IID_IDirect3DDevice9, 0 } };
static const rt_com_class d3d9_class = { "IDirect3D9", 1, { &rt_vt_Direct3D9 }, d3d9_ifaces, 1, NULL };
const rt_com_class d3d_device_class = { "IDirect3DDevice9", 1, { &rt_vt_D3DDevice9 }, device_ifaces, 1, device_destroy };

@implementation D3DDevice
@end

/* ---------------------------------------------------------------- display modes */

typedef struct d3d_mode {
    uint32_t w, h, refresh;
} d3d_mode;

static d3d_mode modes[MAX_MODES];
static uint32_t nmodes;
static d3d_mode desktop_mode;

static uint32_t refresh_of(CGDisplayModeRef m)
{
    double r = CGDisplayModeGetRefreshRate(m);
    /* A display without a fixed rate (built-in panels) reports 0. */
    return r < 1.0 ? 60u : (uint32_t)lround(r);
}

static int mode_cmp(const void *a, const void *b)
{
    const d3d_mode *x = a, *y = b;
    if (x->w != y->w) return x->w < y->w ? -1 : 1;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    if (x->refresh != y->refresh) return x->refresh < y->refresh ? -1 : 1;
    return 0;
}

static void modes_init(void *ctx)
{
    CGDirectDisplayID disp = CGMainDisplayID();
    CFArrayRef all = CGDisplayCopyAllDisplayModes(disp, NULL);
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(disp);
    CFIndex i, n;
    (void)ctx;
    if (all == NULL || cur == NULL) rt_fatal("D3D9: the display modes of the main display are not available");
    n = CFArrayGetCount(all);
    for (i = 0; i < n && nmodes < MAX_MODES; i++)
    {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
        d3d_mode md;
        uint32_t k;
        bool dup = false;
        if (!CGDisplayModeIsUsableForDesktopGUI(m)) continue;
        md.w = (uint32_t)CGDisplayModeGetWidth(m);
        md.h = (uint32_t)CGDisplayModeGetHeight(m);
        md.refresh = refresh_of(m);
        for (k = 0; k < nmodes; k++)
            if (modes[k].w == md.w && modes[k].h == md.h && modes[k].refresh == md.refresh) dup = true;
        if (!dup) modes[nmodes++] = md;
    }
    desktop_mode.w = (uint32_t)CGDisplayModeGetWidth(cur);
    desktop_mode.h = (uint32_t)CGDisplayModeGetHeight(cur);
    desktop_mode.refresh = refresh_of(cur);
    CGDisplayModeRelease(cur);
    CFRelease(all);
    if (nmodes == 0) modes[nmodes++] = desktop_mode;
    qsort(modes, nmodes, sizeof modes[0], mode_cmp);
}

static void modes_once(void)
{
    static dispatch_once_t once;
    dispatch_once_f(&once, NULL, modes_init);
}

static void write_mode(uint32_t p, const d3d_mode *m)
{
    wr32(p, m->w);
    wr32(p + 4, m->h);
    wr32(p + 8, m->refresh);
    wr32(p + 12, D3DFMT_X8R8G8B8);
}

/* ---------------------------------------------------------------- Direct3DCreate9 and IDirect3D9 */

/* IDirect3D9 *Direct3DCreate9(UINT sdk_version) */
void rt_Direct3DCreate9(_cpu *cpu)
{
    rt_com_object *o;
    if (ARG(0) != D3D_SDK_VERSION)
    {
        rt_log("D3D9: Direct3DCreate9 with SDK version %u (the runtime supports %u)", ARG(0), D3D_SDK_VERSION);
        RET(0);
        return;
    }
    if (rt_gpu_device() == nil) rt_fatal("no Metal device is available");
    o = rt_com_create(&d3d9_class, NULL);
    if (o == NULL) rt_fatal("out of guest memory (IDirect3D9)");
    RET(rt_com_ptr(o, 0));
}

static void d3d9_this(_cpu *cpu)
{
    (void)rt_com_this(ARG(0), 0, &d3d9_class);
}

void rt_com_Direct3D9_GetAdapterCount(_cpu *cpu)
{
    d3d9_this(cpu);
    RET(1);
}

/* HRESULT GetAdapterIdentifier(UINT adapter, DWORD flags, D3DADAPTER_IDENTIFIER9 *id) */
void rt_com_Direct3D9_GetAdapterIdentifier(_cpu *cpu)
{
    uint32_t p = ARG(3);
    char *base;
    d3d9_this(cpu);
    if (ARG(1) != D3DADAPTER_DEFAULT || p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    base = G2H(p);
    memset(base, 0, 1100);
    snprintf(base, 512, "%s", "metal");                /* Driver */
    snprintf(base + 512, 512, "%s", rt_gpu_name());    /* Description */
    snprintf(base + 1024, 32, "%s", "\\\\.\\DISPLAY1"); /* DeviceName */
    wr32(p + 1056, 0);                                  /* DriverVersion (LARGE_INTEGER) */
    wr32(p + 1060, 0);
    wr32(p + 1064, 0x106Bu); /* VendorId: Apple */
    wr32(p + 1068, 0);       /* DeviceId */
    wr32(p + 1072, 0);       /* SubSysId */
    wr32(p + 1076, 0);       /* Revision */
    /* DeviceIdentifier (GUID at 1080) stays zero; WHQLLevel (1096) 0. */
    RET(D3D_OK);
}

/* UINT GetAdapterModeCount(UINT adapter, D3DFORMAT format) */
void rt_com_Direct3D9_GetAdapterModeCount(_cpu *cpu)
{
    d3d9_this(cpu);
    modes_once();
    RET(ARG(1) == D3DADAPTER_DEFAULT && ARG(2) == D3DFMT_X8R8G8B8 ? nmodes : 0u);
}

/* HRESULT EnumAdapterModes(UINT adapter, D3DFORMAT format, UINT mode, D3DDISPLAYMODE *out) */
void rt_com_Direct3D9_EnumAdapterModes(_cpu *cpu)
{
    uint32_t mode = ARG(3), p = ARG(4);
    d3d9_this(cpu);
    modes_once();
    if (ARG(1) != D3DADAPTER_DEFAULT || p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (ARG(2) != D3DFMT_X8R8G8B8 || mode >= nmodes)
    {
        RET(D3DERR_NOTAVAILABLE);
        return;
    }
    write_mode(p, &modes[mode]);
    RET(D3D_OK);
}

/* HRESULT GetAdapterDisplayMode(UINT adapter, D3DDISPLAYMODE *out) */
void rt_com_Direct3D9_GetAdapterDisplayMode(_cpu *cpu)
{
    d3d9_this(cpu);
    modes_once();
    if (ARG(1) != D3DADAPTER_DEFAULT || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    write_mode(ARG(2), &desktop_mode);
    RET(D3D_OK);
}

static bool display_format(uint32_t f)
{
    return f == D3DFMT_X8R8G8B8;
}

static bool backbuffer_format(uint32_t f)
{
    return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8;
}

/* HRESULT CheckDeviceType(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT backbuffer, BOOL windowed) */
void rt_com_Direct3D9_CheckDeviceType(_cpu *cpu)
{
    uint32_t bb = ARG(4);
    d3d9_this(cpu);
    if (ARG(1) != D3DADAPTER_DEFAULT)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (ARG(5) && bb == D3DFMT_UNKNOWN) bb = ARG(3);
    RET(ARG(2) == D3DDEVTYPE_HAL && display_format(ARG(3)) && backbuffer_format(bb) ? D3D_OK : D3DERR_NOTAVAILABLE);
}

/* HRESULT CheckDeviceFormat(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, DWORD usage, D3DRESOURCETYPE rtype,
   D3DFORMAT format) */
void rt_com_Direct3D9_CheckDeviceFormat(_cpu *cpu)
{
    uint32_t usage = ARG(4), rtype = ARG(5), fmt = ARG(6);
    const d3d_format *f = d3d_format_find(fmt);
    bool ok = true;
    d3d9_this(cpu);
    if (ARG(1) != D3DADAPTER_DEFAULT || ARG(2) != D3DDEVTYPE_HAL || !display_format(ARG(3)))
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (rtype == D3DRTYPE_VERTEXBUFFER || rtype == D3DRTYPE_INDEXBUFFER)
    {
        RET(D3D_OK);
        return;
    }
    if (f == NULL || (rtype != D3DRTYPE_SURFACE && rtype != D3DRTYPE_TEXTURE && rtype != D3DRTYPE_CUBETEXTURE))
    {
        RET(D3DERR_NOTAVAILABLE);
        return;
    }
    if (f->block && ![rt_gpu_device() supportsBCTextureCompression]) ok = false;
    if ((usage & D3DUSAGE_RENDERTARGET) && !f->renderable) ok = false;
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && !f->depth) ok = false;
    if (f->depth && !(usage & D3DUSAGE_DEPTHSTENCIL) && rtype != D3DRTYPE_TEXTURE) ok = false;
    if (f->depth && rtype == D3DRTYPE_CUBETEXTURE) ok = false;
    if ((usage & D3DUSAGE_QUERY_FILTER) && !f->filterable) ok = false;
    if ((usage & D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING) && !f->blendable) ok = false;
    if ((usage & D3DUSAGE_QUERY_SRGBREAD) && f->pf_srgb == MTLPixelFormatInvalid) ok = false;
    if ((usage & D3DUSAGE_QUERY_SRGBWRITE) && (f->pf_srgb == MTLPixelFormatInvalid || !f->renderable)) ok = false;
    if ((usage & D3DUSAGE_QUERY_VERTEXTEXTURE) && (f->depth || f->block)) ok = false;
    if (!ok)
    {
        RET(D3DERR_NOTAVAILABLE);
        return;
    }
    /* Metal makes mipmaps of color-renderable, filterable formats. */
    if ((usage & D3DUSAGE_AUTOGENMIPMAP) && (!f->renderable || !f->filterable || f->conv != D3DCONV_NONE))
    {
        RET(D3DOK_NOAUTOGEN);
        return;
    }
    RET(D3D_OK);
}

/* HRESULT CheckDeviceMultiSampleType(UINT adapter, D3DDEVTYPE type, D3DFORMAT format, BOOL windowed,
   D3DMULTISAMPLE_TYPE ms, DWORD *quality_levels) */
void rt_com_Direct3D9_CheckDeviceMultiSampleType(_cpu *cpu)
{
    uint32_t ms = ARG(5), q = ARG(6);
    const d3d_format *f = d3d_format_find(ARG(3));
    uint32_t samples = ms == 1 ? 4u : ms;
    d3d9_this(cpu);
    if (ARG(1) != D3DADAPTER_DEFAULT || ARG(2) != D3DDEVTYPE_HAL || ms > 16)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (f == NULL || (!f->renderable && !f->depth) ||
        (samples > 1 && ![rt_gpu_device() supportsTextureSampleCount:samples]))
    {
        RET(D3DERR_NOTAVAILABLE);
        return;
    }
    if (q != 0) wr32(q, 1);
    RET(D3D_OK);
}

/* HRESULT CheckDepthStencilMatch(UINT adapter, D3DDEVTYPE type, D3DFORMAT display, D3DFORMAT rt, D3DFORMAT ds) */
void rt_com_Direct3D9_CheckDepthStencilMatch(_cpu *cpu)
{
    const d3d_format *rt = d3d_format_find(ARG(4)), *ds = d3d_format_find(ARG(5));
    d3d9_this(cpu);
    if (ARG(1) != D3DADAPTER_DEFAULT || ARG(2) != D3DDEVTYPE_HAL)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    RET(rt != NULL && rt->renderable && ds != NULL && ds->depth ? D3D_OK : D3DERR_NOTAVAILABLE);
}

/* HRESULT CheckDeviceFormatConversion(UINT adapter, D3DDEVTYPE type, D3DFORMAT src, D3DFORMAT dst) */
void rt_com_Direct3D9_CheckDeviceFormatConversion(_cpu *cpu)
{
    const d3d_format *s = d3d_format_find(ARG(3)), *t = d3d_format_find(ARG(4));
    d3d9_this(cpu);
    if (ARG(1) != D3DADAPTER_DEFAULT || ARG(2) != D3DDEVTYPE_HAL)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    RET(s != NULL && t != NULL && !s->depth && !t->depth && !s->block && !t->block && t->renderable ? D3D_OK
                                                                                                    : D3DERR_NOTAVAILABLE);
}

/* D3DCAPS9 (304 bytes). The values describe what this runtime implements:
   shader model 3 and no fixed-function pipeline. UNCONFIRMED: the flag
   values are recalled from d3d9caps.h. */
static void write_caps(uint32_t p)
{
    uint8_t *c = G2H(p);
    memset(c, 0, 304);
#define W(off, v) wr32(p + (off), (uint32_t)(v))
#define WF(off, v) wr32(p + (off), d3d_fbits((float)(v)))
    W(0, D3DDEVTYPE_HAL);
    W(4, 0);                       /* AdapterOrdinal */
    W(8, 0);                       /* Caps */
    W(12, 0x20000000u | 0x40000000u | 0x00020000u); /* Caps2: DYNAMICTEXTURES, CANAUTOGENMIPMAP, FULLSCREENGAMMA */
    W(16, 0x20u | 0x100u | 0x200u); /* Caps3: ALPHA_FULLSCREEN_FLIP_OR_DISCARD, COPY_TO_VIDMEM, COPY_TO_SYSTEMMEM */
    W(20, 0x80000000u | 1u);        /* PresentationIntervals: IMMEDIATE, ONE */
    W(24, 0);                       /* CursorCaps */
    W(28, 0x001BBEF0u);             /* DevCaps: HAL, T&L, hardware rasterization, pure device */
    W(32, 0x000E48F2u);             /* PrimitiveMiscCaps: MASKZ, CULL*, COLORWRITEENABLE, BLENDOP, independent masks,
                                       separate alpha blend, MRT bit depths, MRT blending */
    W(36, 0x07422011u);             /* RasterCaps: DITHER, ZTEST, MIPMAPLODBIAS, ANISOTROPY, COLORPERSPECTIVE, SCISSORTEST,
                                       SLOPESCALEDEPTHBIAS, DEPTHBIAS */
    W(40, 0xFFu);                   /* ZCmpCaps */
    W(44, 0x3FFFu);                 /* SrcBlendCaps */
    W(48, 0x3FFFu);                 /* DestBlendCaps */
    W(52, 0xFFu);                   /* AlphaCmpCaps */
    W(56, 0x00084208u);             /* ShadeCaps: Gouraud color, specular, alpha, fog */
    W(60, 0x00014C45u);             /* TextureCaps: PERSPECTIVE, ALPHA, TEXREPEATNOTSCALEDBYSIZE, PROJECTED, CUBEMAP, MIPMAP,
                                       MIPCUBEMAP */
    W(64, 0x07030700u);             /* TextureFilterCaps */
    W(68, 0x07030700u);             /* CubeTextureFilterCaps */
    W(72, 0);                       /* VolumeTextureFilterCaps */
    W(76, 0x3Fu);                   /* TextureAddressCaps */
    W(80, 0);                       /* VolumeTextureAddressCaps */
    W(84, 0xFu);                    /* LineCaps */
    W(88, 8192);                    /* MaxTextureWidth */
    W(92, 8192);                    /* MaxTextureHeight */
    W(96, 0);                       /* MaxVolumeExtent */
    W(100, 8192);                   /* MaxTextureRepeat */
    W(104, 8192);                   /* MaxTextureAspectRatio */
    W(108, 16);                     /* MaxAnisotropy */
    WF(112, 1e10);                  /* MaxVertexW */
    WF(116, -32768.0);              /* GuardBandLeft */
    WF(120, -32768.0);              /* GuardBandTop */
    WF(124, 32767.0);               /* GuardBandRight */
    WF(128, 32767.0);               /* GuardBandBottom */
    WF(132, 0.0);                   /* ExtentsAdjust */
    W(136, 0x1FFu);                 /* StencilCaps: all operations, two-sided */
    W(140, 0x00100008u);            /* FVFCaps: PSIZE, 8 texture coordinates */
    W(144, 0);                      /* TextureOpCaps: no fixed-function pipeline */
    W(148, 8);                      /* MaxTextureBlendStages */
    W(152, 8);                      /* MaxSimultaneousTextures */
    W(156, 0);                      /* VertexProcessingCaps */
    W(160, 0);                      /* MaxActiveLights */
    W(164, 0);                      /* MaxUserClipPlanes */
    W(168, 0);                      /* MaxVertexBlendMatrices */
    W(172, 0);                      /* MaxVertexBlendMatrixIndex */
    WF(176, 256.0);                 /* MaxPointSize */
    W(180, 0x00FFFFFFu);            /* MaxPrimitiveCount */
    W(184, 0x00FFFFFFu);            /* MaxVertexIndex */
    W(188, D3D_MAX_STREAMS);        /* MaxStreams */
    W(192, 255);                    /* MaxStreamStride */
    W(196, 0xFFFE0300u);            /* VertexShaderVersion 3.0 */
    W(200, D3DSH_VS_CONST_F);       /* MaxVertexShaderConst */
    W(204, 0xFFFF0300u);            /* PixelShaderVersion 3.0 */
    WF(208, 65504.0);               /* PixelShader1xMaxValue */
    W(212, 0x51u);                  /* DevCaps2: STREAMOFFSET, CAN_STRETCHRECT_FROM_TEXTURES,
                                       VERTEXELEMENTSCANSHARESTREAMOFFSET */
    WF(216, 0.0);                   /* MaxNpatchTessellationLevel */
    W(220, 0);                      /* Reserved5 */
    W(224, 0);                      /* MasterAdapterOrdinal */
    W(228, 0);                      /* AdapterOrdinalInGroup */
    W(232, 1);                      /* NumberOfAdaptersInGroup */
    W(236, 0x3BFu);                 /* DeclTypes: all except UDEC3 */
    W(240, D3D_MAX_RT);             /* NumSimultaneousRTs */
    W(244, 0x03000300u);            /* StretchRectFilterCaps */
    W(248, 1);                      /* VS20Caps.Caps: PREDICATION */
    W(252, 24);                     /* DynamicFlowControlDepth */
    W(256, 32);                     /* NumTemps */
    W(260, 4);                      /* StaticFlowControlDepth */
    W(264, 0x1Fu);                  /* PS20Caps.Caps */
    W(268, 24);
    W(272, 32);
    W(276, 4);
    W(280, 512);                    /* NumInstructionSlots */
    W(284, 0x03000300u);            /* VertexTextureFilterCaps */
    W(288, 65535);                  /* MaxVShaderInstructionsExecuted */
    W(292, 65535);                  /* MaxPShaderInstructionsExecuted */
    W(296, 4096);                   /* MaxVertexShader30InstructionSlots */
    W(300, 4096);                   /* MaxPixelShader30InstructionSlots */
#undef W
#undef WF
}

/* HRESULT GetDeviceCaps(UINT adapter, D3DDEVTYPE type, D3DCAPS9 *caps) */
void rt_com_Direct3D9_GetDeviceCaps(_cpu *cpu)
{
    d3d9_this(cpu);
    if (ARG(1) != D3DADAPTER_DEFAULT || ARG(3) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (ARG(2) != D3DDEVTYPE_HAL)
    {
        RET(D3DERR_NOTAVAILABLE);
        return;
    }
    write_caps(ARG(3));
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- device core */

void d3d_lock(D3DDevice *d)
{
    if (pthread_mutex_lock(&d->lock) != 0) rt_fatal("D3D9: device lock failed");
}

void d3d_unlock(D3DDevice *d)
{
    if (pthread_mutex_unlock(&d->lock) != 0) rt_fatal("D3D9: device unlock failed");
}

D3DDevice *d3d_device_of(uint32_t this_ptr)
{
    return (__bridge D3DDevice *)rt_com_this(this_ptr, 0, &d3d_device_class)->data;
}

bool d3d_serial_done(D3DDevice *d, uint64_t serial)
{
    return atomic_load(&d->completed) >= serial;
}

/* Start recording command buffer d->serial. Command buffers of one queue
   run in commit order, so the completed serial only grows. */
static void new_command_buffer(D3DDevice *d)
{
    uint64_t serial = d->serial;
    D3DDevice *dev = d;
    id<MTLCommandBuffer> cb = [d->queue commandBuffer];
    if (cb == nil) rt_fatal("D3D9: command buffer creation failed");
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        if (b.status == MTLCommandBufferStatusError)
            rt_fatal("D3D9: a GPU command buffer failed: %s", b.error.localizedDescription.UTF8String);
        if (pthread_mutex_lock(&dev->done_lock) != 0) rt_fatal("D3D9: completion lock failed");
        atomic_store(&dev->completed, serial);
        if (pthread_cond_broadcast(&dev->done_cv) != 0) rt_fatal("D3D9: completion signal failed");
        if (pthread_mutex_unlock(&dev->done_lock) != 0) rt_fatal("D3D9: completion unlock failed");
    }];
    d->cb = cb;
}

/* Move upload buffers whose command buffers are complete to the free list. */
static void recycle_uploads(D3DDevice *d)
{
    NSUInteger i = 0;
    while (i < d->upload_used.count)
    {
        D3DRetired *r = d->upload_used[i];
        if (d3d_serial_done(d, r->serial))
        {
            [d->upload_used removeObjectAtIndex:i];
            if (d->upload_free.count < MAX_UPLOAD_FREE && r->buf.length == UPLOAD_BLOCK) [d->upload_free addObject:r];
        }
        else
            i++;
    }
}

static void retire_upload(D3DDevice *d)
{
    if (d->upload != nil)
    {
        [d->upload_used addObject:d3d_retired_new(d->upload, nil, d->serial)];
        d->upload = nil;
        d->upload_off = 0;
    }
}

void d3d_flush(D3DDevice *d)
{
    @autoreleasepool
    {
        d3d_end_pass(d);
        retire_upload(d);
        [d->cb commit];
        d->serial++;
        new_command_buffer(d);
        recycle_uploads(d);
    }
}

void d3d_wait_serial(D3DDevice *d, uint64_t serial)
{
    if (serial >= d->serial) d3d_flush(d);
    if (pthread_mutex_lock(&d->done_lock) != 0) rt_fatal("D3D9: completion lock failed");
    while (atomic_load(&d->completed) < serial)
        if (pthread_cond_wait(&d->done_cv, &d->done_lock) != 0) rt_fatal("D3D9: completion wait failed");
    if (pthread_mutex_unlock(&d->done_lock) != 0) rt_fatal("D3D9: completion unlock failed");
}

id<MTLBuffer> d3d_upload(D3DDevice *d, const void *data, uint32_t size, uint32_t *offset)
{
    uint32_t need = (size + 255u) & ~255u;
    if (need < size) rt_fatal("D3D9: upload of %u bytes is too large", size);
    if (d->upload == nil || d->upload_off + need > d->upload.length)
    {
        retire_upload(d);
        recycle_uploads(d);
        if (need <= UPLOAD_BLOCK && d->upload_free.count > 0)
        {
            d->upload = d->upload_free.lastObject->buf;
            [d->upload_free removeLastObject];
        }
        else
        {
            d->upload = [d->mtl newBufferWithLength:need > UPLOAD_BLOCK ? need : UPLOAD_BLOCK
                                            options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
            if (d->upload == nil) rt_fatal("D3D9: out of memory (upload buffer of %u bytes)", need);
        }
        d->upload_off = 0;
    }
    *offset = d->upload_off;
    memcpy((uint8_t *)d->upload.contents + d->upload_off, data, size);
    d->upload_off += need;
    return d->upload;
}

/* The object that holds the references of interface o: a surface of a
   texture shares the reference count of the texture. */
static rt_com_object *ref_owner(rt_com_object *o)
{
    if (o != NULL && o->cls == &d3d_surface_class && d3d_surf(o)->container != NULL) return d3d_surf(o)->container;
    return o;
}

static void hold(rt_com_object *o)
{
    if (o != NULL) rt_com_addref(ref_owner(o));
}

static void drop(rt_com_object *o)
{
    if (o != NULL) rt_com_release(ref_owner(o));
}

/* Replace a bound object (one reference is held). */
static void bind(rt_com_object **slot, rt_com_object *o)
{
    hold(o);
    drop(*slot);
    *slot = o;
}

/* Write interface pointer o with a new reference to out (0 if o is NULL). */
static void give(rt_com_object *o, uint32_t out)
{
    if (o == NULL)
    {
        wr32(out, 0);
        return;
    }
    hold(o);
    wr32(out, rt_com_ptr(o, 0));
}

static void release_bindings(D3DDevice *d)
{
    uint32_t i;
    for (i = 0; i < D3D_MAX_RT; i++) bind(&d->rt[i], NULL);
    bind(&d->ds, NULL);
    for (i = 0; i < D3D_SAMPLERS; i++) bind(&d->tex[i], NULL);
    for (i = 0; i < D3D_MAX_STREAMS; i++)
    {
        bind(&d->streams[i].buf, NULL);
        d->streams[i].offset = 0;
        d->streams[i].stride = 0;
        d->streams[i].freq = 1;
    }
    bind(&d->ib, NULL);
    bind(&d->decl, NULL);
    bind(&d->vs, NULL);
    bind(&d->ps, NULL);
}

static void identity(float m[16])
{
    int i;
    for (i = 0; i < 16; i++) m[i] = (i % 5) == 0 ? 1.0f : 0.0f;
}

/* The default state of a new or reset device. */
static void default_state(D3DDevice *d)
{
    uint32_t i, s;
    D3DSurface *bb = d3d_surf(d->backbuffer);
    memset(d->rs, 0, sizeof d->rs);
    d->rs[RS_ZENABLE] = d->auto_ds != NULL ? 1u : 0u;
    d->rs[RS_FILLMODE] = 3;
    d->rs[RS_SHADEMODE] = 2;
    d->rs[RS_ZWRITEENABLE] = 1;
    d->rs[RS_LASTPIXEL] = 1;
    d->rs[RS_SRCBLEND] = 2;
    d->rs[RS_DESTBLEND] = 1;
    d->rs[RS_CULLMODE] = 3;
    d->rs[RS_ZFUNC] = 4;
    d->rs[RS_ALPHAFUNC] = 8;
    d->rs[RS_FOGEND] = d3d_fbits(1.0f);
    d->rs[RS_FOGDENSITY] = d3d_fbits(1.0f);
    d->rs[RS_STENCILFAIL] = 1;
    d->rs[RS_STENCILZFAIL] = 1;
    d->rs[RS_STENCILPASS] = 1;
    d->rs[RS_STENCILFUNC] = 8;
    d->rs[RS_STENCILMASK] = 0xFFFFFFFFu;
    d->rs[RS_STENCILWRITEMASK] = 0xFFFFFFFFu;
    d->rs[RS_TEXTUREFACTOR] = 0xFFFFFFFFu;
    d->rs[RS_CLIPPING] = 1;
    d->rs[RS_LIGHTING] = 1;
    d->rs[RS_COLORVERTEX] = 1;
    d->rs[RS_LOCALVIEWER] = 1;
    d->rs[RS_DIFFUSEMATERIALSOURCE] = 1;
    d->rs[RS_SPECULARMATERIALSOURCE] = 2;
    d->rs[RS_POINTSIZE] = d3d_fbits(1.0f);
    d->rs[RS_POINTSIZE_MIN] = d3d_fbits(1.0f);
    d->rs[RS_POINTSCALE_A] = d3d_fbits(1.0f);
    d->rs[RS_MULTISAMPLEANTIALIAS] = 1;
    d->rs[RS_MULTISAMPLEMASK] = 0xFFFFFFFFu;
    d->rs[RS_DEBUGMONITORTOKEN] = 0xBAADCAFEu;
    d->rs[RS_POINTSIZE_MAX] = d3d_fbits(256.0f);
    d->rs[RS_COLORWRITEENABLE] = 0xFu;
    d->rs[RS_BLENDOP] = 1;
    d->rs[RS_POSITIONDEGREE] = 3;
    d->rs[RS_NORMALDEGREE] = 1;
    d->rs[RS_MINTESSELLATIONLEVEL] = d3d_fbits(1.0f);
    d->rs[RS_MAXTESSELLATIONLEVEL] = d3d_fbits(1.0f);
    d->rs[RS_ADAPTIVETESS_Z] = d3d_fbits(1.0f);
    d->rs[RS_CCW_STENCILFAIL] = 1;
    d->rs[RS_CCW_STENCILZFAIL] = 1;
    d->rs[RS_CCW_STENCILPASS] = 1;
    d->rs[RS_CCW_STENCILFUNC] = 8;
    d->rs[RS_COLORWRITEENABLE1] = 0xFu;
    d->rs[RS_COLORWRITEENABLE2] = 0xFu;
    d->rs[RS_COLORWRITEENABLE3] = 0xFu;
    d->rs[RS_BLENDFACTOR] = 0xFFFFFFFFu;
    d->rs[RS_SRCBLENDALPHA] = 2;
    d->rs[RS_DESTBLENDALPHA] = 1;
    d->rs[RS_BLENDOPALPHA] = 1;
    for (s = 0; s < D3D_SAMPLERS; s++)
    {
        memset(d->ss[s], 0, sizeof d->ss[s]);
        d->ss[s][SS_ADDRESSU] = 1;
        d->ss[s][SS_ADDRESSV] = 1;
        d->ss[s][SS_ADDRESSW] = 1;
        d->ss[s][SS_MAGFILTER] = 1;
        d->ss[s][SS_MINFILTER] = 1;
        d->ss[s][SS_MAXANISOTROPY] = 1;
    }
    for (i = 0; i < 8; i++)
    {
        memset(d->tss[i], 0, sizeof d->tss[i]);
        d->tss[i][1] = i == 0 ? 4u : 1u;  /* COLOROP: MODULATE, DISABLE */
        d->tss[i][2] = 2;                  /* COLORARG1: TEXTURE */
        d->tss[i][3] = 1;                  /* COLORARG2: CURRENT */
        d->tss[i][4] = i == 0 ? 2u : 1u;  /* ALPHAOP: SELECTARG1, DISABLE */
        d->tss[i][5] = 2;
        d->tss[i][6] = 1;
        d->tss[i][11] = i;                 /* TEXCOORDINDEX */
        d->tss[i][26] = 1;                 /* COLORARG0 */
        d->tss[i][27] = 1;                 /* ALPHAARG0 */
        d->tss[i][28] = 1;                 /* RESULTARG */
    }
    release_bindings(d);
    bind(&d->rt[0], d->backbuffer);
    bind(&d->ds, d->auto_ds);
    d->fvf = 0;
    memset(d->vs_c, 0, sizeof d->vs_c);
    memset(d->vs_i, 0, sizeof d->vs_i);
    memset(d->vs_b, 0, sizeof d->vs_b);
    memset(d->ps_c, 0, sizeof d->ps_c);
    memset(d->ps_i, 0, sizeof d->ps_i);
    memset(d->ps_b, 0, sizeof d->ps_b);
    d->vs_const_ver++;
    d->ps_const_ver++;
    d->viewport[0] = 0;
    d->viewport[1] = 0;
    d->viewport[2] = bb->width;
    d->viewport[3] = bb->height;
    d->viewport[4] = d3d_fbits(0.0f);
    d->viewport[5] = d3d_fbits(1.0f);
    d->scissor[0] = 0;
    d->scissor[1] = 0;
    d->scissor[2] = (int32_t)bb->width;
    d->scissor[3] = (int32_t)bb->height;
    for (i = 0; i < 512; i++) identity(d->transforms[i]);
    memset(d->material, 0, sizeof d->material);
    [d->lights removeAllObjects];
    [d->lights_on removeAllObjects];
    memset(d->clip_planes, 0, sizeof d->clip_planes);
    memset(d->clip_status, 0, sizeof d->clip_status);
    d->npatch_mode = 0;
    d->in_scene = false;
    for (i = 0; i < D3D_MAX_RT; i++) d->clear_color[i] = false;
    d->clear_depth = false;
    d->clear_stencil = false;
}

/* Check and complete D3DPRESENT_PARAMETERS (copied to d->pp). */
static uint32_t check_pp(D3DDevice *d, uint32_t hwnd)
{
    uint32_t *pp = d->pp;
    if (pp[PP_WINDOWED])
    {
        if (pp[PP_WIDTH] == 0 || pp[PP_HEIGHT] == 0)
        {
            int32_t w, h;
            if (!rt_user32_client_size(hwnd, &w, &h)) return D3DERR_INVALIDCALL;
            if (pp[PP_WIDTH] == 0) pp[PP_WIDTH] = w > 0 ? (uint32_t)w : 1u;
            if (pp[PP_HEIGHT] == 0) pp[PP_HEIGHT] = h > 0 ? (uint32_t)h : 1u;
        }
        if (pp[PP_FORMAT] == D3DFMT_UNKNOWN) pp[PP_FORMAT] = D3DFMT_X8R8G8B8;
    }
    if (pp[PP_WIDTH] == 0 || pp[PP_HEIGHT] == 0 || pp[PP_WIDTH] > 16384u || pp[PP_HEIGHT] > 16384u ||
        !backbuffer_format(pp[PP_FORMAT]) || pp[PP_COUNT] > 3 || pp[PP_SWAPEFFECT] < 1 || pp[PP_SWAPEFFECT] > 3 ||
        (pp[PP_INTERVAL] != 0 && pp[PP_INTERVAL] != 1 && pp[PP_INTERVAL] != D3DPRESENT_INTERVAL_IMMEDIATE) ||
        pp[PP_MSAA] > 16)
        return D3DERR_INVALIDCALL;
    if (pp[PP_COUNT] == 0) pp[PP_COUNT] = 1;
    if (pp[PP_MSAA] != 0 && pp[PP_SWAPEFFECT] != 1) return D3DERR_INVALIDCALL;
    return D3D_OK;
}

/* Create the back buffer and the automatic depth-stencil surface. */
static uint32_t create_swap_surfaces(D3DDevice *d)
{
    uint32_t *pp = d->pp, r;
    r = d3d_create_surface(d, pp[PP_WIDTH], pp[PP_HEIGHT], pp[PP_FORMAT], D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT,
                           pp[PP_MSAA], pp[PP_MSAA_QUALITY], (pp[PP_FLAGS] & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0,
                           &d->backbuffer);
    if (r != D3D_OK) return r;
    if (pp[PP_AUTODS])
    {
        r = d3d_create_surface(d, pp[PP_WIDTH], pp[PP_HEIGHT], pp[PP_AUTODS_FORMAT], D3DUSAGE_DEPTHSTENCIL,
                               D3DPOOL_DEFAULT, pp[PP_MSAA], pp[PP_MSAA_QUALITY], false, &d->auto_ds);
        if (r != D3D_OK)
        {
            rt_com_release(d->backbuffer);
            d->backbuffer = NULL;
            return r;
        }
    }
    d->layer.displaySyncEnabled = pp[PP_INTERVAL] != D3DPRESENT_INTERVAL_IMMEDIATE;
    return D3D_OK;
}

/* HRESULT CreateDevice(UINT adapter, D3DDEVTYPE type, HWND focus, DWORD behavior, D3DPRESENT_PARAMETERS *pp,
   IDirect3DDevice9 **out) */
void rt_com_Direct3D9_CreateDevice(_cpu *cpu)
{
    rt_com_object *self = rt_com_this(ARG(0), 0, &d3d9_class);
    uint32_t focus = ARG(3), behavior = ARG(4), ppp = ARG(5), out = ARG(6), hwnd, i, r;
    D3DDevice *d;
    pthread_mutexattr_t ma;
    void *layer;
    if (ARG(1) != D3DADAPTER_DEFAULT || ppp == 0 || out == 0 ||
        !(behavior & (D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_HARDWARE_VERTEXPROCESSING |
                      D3DCREATE_MIXED_VERTEXPROCESSING)))
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (ARG(2) != D3DDEVTYPE_HAL)
    {
        RET(D3DERR_NOTAVAILABLE);
        return;
    }
    @autoreleasepool
    {
        d = [D3DDevice new];
        for (i = 0; i < 14; i++) d->pp[i] = rd32(ppp + 4u * i);
        hwnd = d->pp[PP_HWND] != 0 ? d->pp[PP_HWND] : focus;
        r = check_pp(d, hwnd);
        if (r != D3D_OK)
        {
            RET(r);
            return;
        }
        layer = rt_user32_window_layer(hwnd);
        if (layer == NULL)
        {
            RET(D3DERR_INVALIDCALL);
            return;
        }
        d->layer = (__bridge CAMetalLayer *)layer;
        d->mtl = rt_gpu_device();
        d->queue = [d->mtl newCommandQueue];
        if (d->queue == nil) rt_fatal("D3D9: command queue creation failed");
        d->layer.device = d->mtl;
        d->layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        d->layer.framebufferOnly = YES;
        d->layer.maximumDrawableCount = 3;
        if (pthread_mutexattr_init(&ma) != 0 || pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE) != 0 ||
            pthread_mutex_init(&d->lock, &ma) != 0 || pthread_mutexattr_destroy(&ma) != 0 ||
            pthread_mutex_init(&d->done_lock, NULL) != 0 || pthread_cond_init(&d->done_cv, NULL) != 0)
            rt_fatal("D3D9: device lock creation failed");
        atomic_store(&d->completed, 0);
        d->serial = 1;
        new_command_buffer(d);
        d->adapter = ARG(1);
        d->devtype = ARG(2);
        d->focus_hwnd = focus;
        d->behavior = behavior;
        d->software_vp = (behavior & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0;
        d->fvf_decls = [NSMutableDictionary new];
        d->lights = [NSMutableDictionary new];
        d->lights_on = [NSMutableSet new];
        d3d_init_caches(d);
        d->upload_free = [NSMutableArray new];
        d->upload_used = [NSMutableArray new];
        for (i = 0; i < D3D_MAX_STREAMS; i++) d->streams[i].freq = 1;
        for (i = 0; i < 256; i++)
        {
            uint16_t v = (uint16_t)(i * 257u);
            d->gamma[0][i] = v;
            d->gamma[1][i] = v;
            d->gamma[2][i] = v;
        }
        d3d_draw_init(d);
        d3d_lock(d);
        r = create_swap_surfaces(d);
        if (r != D3D_OK)
        {
            d3d_unlock(d);
            RET(r);
            return;
        }
        d->com = rt_com_create(&d3d_device_class, (__bridge_retained void *)d);
        if (d->com == NULL) rt_fatal("out of guest memory (IDirect3DDevice9)");
        rt_com_addref(self);
        d->d3d9 = self;
        default_state(d);
        d3d_unlock(d);
        /* The completed parameters go back to the caller. */
        for (i = 0; i < 14; i++) wr32(ppp + 4u * i, d->pp[i]);
        wr32(out, rt_com_ptr(d->com, 0));
    }
    RET(D3D_OK);
}

static void device_destroy(rt_com_object *o)
{
    D3DDevice *d = (__bridge_transfer D3DDevice *)o->data;
    NSArray<NSValue *> *decls;
    d3d_lock(d);
    @autoreleasepool
    {
        d3d_wait_serial(d, d->serial);
        release_bindings(d);
        decls = d->fvf_decls.allValues;
        [d->fvf_decls removeAllObjects];
        for (NSValue *v in decls) rt_com_release((rt_com_object *)v.pointerValue);
        if (d->backbuffer != NULL) rt_com_release(d->backbuffer);
        if (d->auto_ds != NULL) rt_com_release(d->auto_ds);
        d->backbuffer = NULL;
        d->auto_ds = NULL;
        rt_com_release(d->d3d9);
        d->d3d9 = NULL;
        d->com = NULL;
    }
    d3d_unlock(d);
}

/* ---------------------------------------------------------------- device methods: general */

#define DEV D3DDevice *d = d3d_device_of(ARG(0))

void rt_com_D3DDevice9_TestCooperativeLevel(_cpu *cpu)
{
    DEV;
    (void)d;
    /* A Metal device is never lost. */
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetAvailableTextureMem(_cpu *cpu)
{
    DEV;
    (void)d;
    RET(rt_gpu_adapter_ram() & ~0xFFFFFu);
}

/* Managed resources keep no separate video memory copy. */
void rt_com_D3DDevice9_EvictManagedResources(_cpu *cpu)
{
    DEV;
    (void)d;
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetDirect3D(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    rt_com_addref(d->d3d9);
    wr32(ARG(1), rt_com_ptr(d->d3d9, 0));
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetDeviceCaps(_cpu *cpu)
{
    DEV;
    (void)d;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    write_caps(ARG(1));
    RET(D3D_OK);
}

/* HRESULT GetDisplayMode(UINT swapchain, D3DDISPLAYMODE *mode) */
void rt_com_D3DDevice9_GetDisplayMode(_cpu *cpu)
{
    DEV;
    d3d_mode m;
    if (ARG(1) != 0 || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    modes_once();
    if (d->pp[PP_WINDOWED])
        m = desktop_mode;
    else
    {
        /* The full-screen mode that the application set. */
        m.w = d->pp[PP_WIDTH];
        m.h = d->pp[PP_HEIGHT];
        m.refresh = d->pp[PP_REFRESH] ? d->pp[PP_REFRESH] : desktop_mode.refresh;
    }
    write_mode(ARG(2), &m);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetCreationParameters(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(1);
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    wr32(p, d->adapter);
    wr32(p + 4, d->devtype);
    wr32(p + 8, d->focus_hwnd);
    wr32(p + 12, d->behavior);
    RET(D3D_OK);
}

/* The device cursor needs SetCursorProperties, which the runtime does not
   implement; the position and the visibility are kept for the getters. */
void rt_com_D3DDevice9_SetCursorPosition(_cpu *cpu)
{
    DEV;
    d3d_lock(d);
    d->cursor_x = ARG(1);
    d->cursor_y = ARG(2);
    d3d_unlock(d);
}

void rt_com_D3DDevice9_ShowCursor(_cpu *cpu)
{
    DEV;
    bool old;
    d3d_lock(d);
    old = d->cursor_shown;
    d->cursor_shown = ARG(1) != 0;
    d3d_unlock(d);
    RET(old ? 1u : 0u);
}

void rt_com_D3DDevice9_GetNumberOfSwapChains(_cpu *cpu)
{
    DEV;
    (void)d;
    RET(1);
}

/* HRESULT Reset(D3DPRESENT_PARAMETERS *pp) */
void rt_com_D3DDevice9_Reset(_cpu *cpu)
{
    DEV;
    uint32_t ppp = ARG(1), i, r, old[14];
    rt_com_object *bb, *ds;
    if (ppp == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    @autoreleasepool
    {
        memcpy(old, d->pp, sizeof old);
        for (i = 0; i < 14; i++) d->pp[i] = rd32(ppp + 4u * i);
        r = check_pp(d, d->pp[PP_HWND] != 0 ? d->pp[PP_HWND] : d->focus_hwnd);
        if (r != D3D_OK)
        {
            memcpy(d->pp, old, sizeof old);
            RET(r);
            d3d_unlock(d);
            return;
        }
        d3d_wait_serial(d, d->serial);
        release_bindings(d);
        bb = d->backbuffer;
        ds = d->auto_ds;
        d->backbuffer = NULL;
        d->auto_ds = NULL;
        r = create_swap_surfaces(d);
        if (r != D3D_OK)
        {
            /* The old swap chain stays. */
            memcpy(d->pp, old, sizeof old);
            d->backbuffer = bb;
            d->auto_ds = ds;
            default_state(d);
            RET(r);
            d3d_unlock(d);
            return;
        }
        if (bb != NULL) rt_com_release(bb);
        if (ds != NULL) rt_com_release(ds);
        d->present_resolve = nil;
        default_state(d);
        for (i = 0; i < 14; i++) wr32(ppp + 4u * i, d->pp[i]);
    }
    RET(D3D_OK);
    d3d_unlock(d);
}

/* HRESULT Present(const RECT *src, const RECT *dst, HWND override, const RGNDATA *dirty) */
void rt_com_D3DDevice9_Present(_cpu *cpu)
{
    DEV;
    if (ARG(1) != 0 || ARG(2) != 0 || ARG(3) != 0 || ARG(4) != 0)
        rt_fatal("D3D9: Present with rectangles, a window or a dirty region is not supported");
    d3d_lock(d);
    if (d->in_scene)
    {
        d3d_unlock(d);
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_present(d);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* HRESULT GetBackBuffer(UINT swapchain, UINT index, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out) */
void rt_com_D3DDevice9_GetBackBuffer(_cpu *cpu)
{
    DEV;
    uint32_t out = ARG(4);
    if (ARG(1) != 0 || out == 0 || ARG(2) >= d->pp[PP_COUNT])
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (ARG(2) != 0) rt_fatal("D3D9: GetBackBuffer index %u (the runtime has one back buffer surface)", ARG(2));
    d3d_lock(d);
    give(d->backbuffer, out);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* The caps do not report D3DCAPS_READ_SCANLINE. UNCONFIRMED: D3D9 then
   returns D3DERR_INVALIDCALL. */
void rt_com_D3DDevice9_GetRasterStatus(_cpu *cpu)
{
    DEV;
    (void)d;
    RET(D3DERR_INVALIDCALL);
}

/* void SetGammaRamp(UINT swapchain, DWORD flags, const D3DGAMMARAMP *ramp) */
void rt_com_D3DDevice9_SetGammaRamp(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(3), i;
    uint16_t px[256][4];
    if (ARG(1) != 0 || p == 0) return;
    d3d_lock(d);
    @autoreleasepool
    {
        MTLTextureDescriptor *td;
        memcpy(d->gamma, G2H(p), sizeof d->gamma);
        for (i = 0; i < 256; i++)
        {
            px[i][0] = d->gamma[0][i];
            px[i][1] = d->gamma[1][i];
            px[i][2] = d->gamma[2][i];
            px[i][3] = 0xFFFFu;
        }
        /* A new texture: frames in flight keep the old one. */
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Unorm width:256 height:1
                                                            mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        td.usage = MTLTextureUsageShaderRead;
        d->gamma_tex = [d->mtl newTextureWithDescriptor:td];
        if (d->gamma_tex == nil) rt_fatal("D3D9: gamma texture creation failed");
        [d->gamma_tex replaceRegion:MTLRegionMake2D(0, 0, 256, 1) mipmapLevel:0 withBytes:px bytesPerRow:sizeof px];
        d->gamma_set = true;
    }
    d3d_unlock(d);
}

void rt_com_D3DDevice9_GetGammaRamp(_cpu *cpu)
{
    DEV;
    if (ARG(1) != 0 || ARG(2) == 0) return;
    d3d_lock(d);
    memcpy(G2H(ARG(2)), d->gamma, sizeof d->gamma);
    d3d_unlock(d);
}

/* ---------------------------------------------------------------- device methods: resources */

/* Shared handles are a D3D9Ex feature; D3D9 requires NULL. */
static bool no_shared(uint32_t handle)
{
    return handle == 0;
}

/* CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9 **out,
   HANDLE *shared) */
void rt_com_D3DDevice9_CreateTexture(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = no_shared(ARG(8)) ? d3d_create_texture(d, false, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7))
                              : D3DERR_INVALIDCALL;
    }
    d3d_unlock(d);
    RET(r);
}

/* CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9 **out,
   HANDLE *shared) */
void rt_com_D3DDevice9_CreateCubeTexture(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = no_shared(ARG(7)) ? d3d_create_texture(d, true, ARG(1), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6))
                              : D3DERR_INVALIDCALL;
    }
    d3d_unlock(d);
    RET(r);
}

/* CreateVolumeTexture(UINT w, UINT h, UINT depth, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
   IDirect3DVolumeTexture9 **out, HANDLE *shared) */
void rt_com_D3DDevice9_CreateVolumeTexture(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = no_shared(ARG(9)) ? d3d_create_volume_texture(d, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8))
                              : D3DERR_INVALIDCALL;
    }
    d3d_unlock(d);
    RET(r);
}

/* CreateVertexBuffer(UINT length, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out, HANDLE *shared) */
void rt_com_D3DDevice9_CreateVertexBuffer(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = no_shared(ARG(6)) ? d3d_create_buffer(d, false, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)) : D3DERR_INVALIDCALL;
    }
    d3d_unlock(d);
    RET(r);
}

/* CreateIndexBuffer(UINT length, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out, HANDLE *shared) */
void rt_com_D3DDevice9_CreateIndexBuffer(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = no_shared(ARG(6)) ? d3d_create_buffer(d, true, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)) : D3DERR_INVALIDCALL;
    }
    d3d_unlock(d);
    RET(r);
}

static void create_surface_common(_cpu *cpu, D3DDevice *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage,
                                  uint32_t pool, uint32_t msaa, uint32_t quality, bool lockable, uint32_t out,
                                  uint32_t shared)
{
    rt_com_object *o = NULL;
    uint32_t r;
    if (out == 0 || !no_shared(shared))
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    @autoreleasepool
    {
        r = d3d_create_surface(d, w, h, fmt, usage, pool, msaa, quality, lockable, &o);
    }
    d3d_unlock(d);
    if (r == D3D_OK) wr32(out, rt_com_ptr(o, 0));
    RET(r);
}

/* CreateRenderTarget(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL lockable,
   IDirect3DSurface9 **out, HANDLE *shared) */
void rt_com_D3DDevice9_CreateRenderTarget(_cpu *cpu)
{
    DEV;
    create_surface_common(cpu, d, ARG(1), ARG(2), ARG(3), D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT, ARG(4), ARG(5),
                          ARG(6) != 0, ARG(7), ARG(8));
}

/* CreateDepthStencilSurface(UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL discard,
   IDirect3DSurface9 **out, HANDLE *shared) */
void rt_com_D3DDevice9_CreateDepthStencilSurface(_cpu *cpu)
{
    DEV;
    create_surface_common(cpu, d, ARG(1), ARG(2), ARG(3), D3DUSAGE_DEPTHSTENCIL, D3DPOOL_DEFAULT, ARG(4), ARG(5), false,
                          ARG(7), ARG(8));
}

/* CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *shared) */
void rt_com_D3DDevice9_CreateOffscreenPlainSurface(_cpu *cpu)
{
    DEV;
    if (ARG(4) == D3DPOOL_MANAGED)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    create_surface_common(cpu, d, ARG(1), ARG(2), ARG(3), 0, ARG(4), 0, 0, true, ARG(5), ARG(6));
}

/* UpdateSurface(IDirect3DSurface9 *src, const RECT *src_rect, IDirect3DSurface9 *dst, const POINT *dst_point) */
void rt_com_D3DDevice9_UpdateSurface(_cpu *cpu)
{
    DEV;
    uint32_t r;
    if (ARG(1) == 0 || ARG(3) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    @autoreleasepool
    {
        r = d3d_update_surface(d, d3d_surf(d3d_surface_from_ptr(ARG(1))), ARG(2), d3d_surf(d3d_surface_from_ptr(ARG(3))),
                               ARG(4));
    }
    d3d_unlock(d);
    RET(r);
}

static D3DTexture *any_texture(uint32_t p)
{
    rt_com_object *o = rt_com_this(p, 0, NULL);
    if (!d3d_is_texture(o))
        rt_fatal("0x%08x is not a runtime texture (%s)", p, o->cls->name);
    return d3d_tex(o);
}

/* UpdateTexture(IDirect3DBaseTexture9 *src, IDirect3DBaseTexture9 *dst) */
void rt_com_D3DDevice9_UpdateTexture(_cpu *cpu)
{
    DEV;
    uint32_t r;
    if (ARG(1) == 0 || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    @autoreleasepool
    {
        r = d3d_update_texture(d, any_texture(ARG(1)), any_texture(ARG(2)));
    }
    d3d_unlock(d);
    RET(r);
}

/* GetRenderTargetData(IDirect3DSurface9 *rt, IDirect3DSurface9 *dst) */
void rt_com_D3DDevice9_GetRenderTargetData(_cpu *cpu)
{
    DEV;
    uint32_t r;
    if (ARG(1) == 0 || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    @autoreleasepool
    {
        r = d3d_readback(d, d3d_surf(d3d_surface_from_ptr(ARG(1))), d3d_surf(d3d_surface_from_ptr(ARG(2))));
    }
    d3d_unlock(d);
    RET(r);
}

/* ---------------------------------------------------------------- device methods: render targets */

/* SetRenderTarget(DWORD index, IDirect3DSurface9 *surface) */
void rt_com_D3DDevice9_SetRenderTarget(_cpu *cpu)
{
    DEV;
    uint32_t idx = ARG(1), p = ARG(2);
    rt_com_object *o = p != 0 ? d3d_surface_from_ptr(p) : NULL;
    if (idx >= D3D_MAX_RT || (idx == 0 && o == NULL) ||
        (o != NULL && !(d3d_surf(o)->usage & D3DUSAGE_RENDERTARGET)))
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    if (d->rt[idx] != o)
    {
        d3d_end_pass(d);
        bind(&d->rt[idx], o);
    }
    if (idx == 0)
    {
        /* Render target 0 sets the viewport and the scissor rectangle to the
           whole surface. */
        D3DSurface *s = d3d_surf(o);
        d->viewport[0] = 0;
        d->viewport[1] = 0;
        d->viewport[2] = s->width;
        d->viewport[3] = s->height;
        d->viewport[4] = d3d_fbits(0.0f);
        d->viewport[5] = d3d_fbits(1.0f);
        d->scissor[0] = 0;
        d->scissor[1] = 0;
        d->scissor[2] = (int32_t)s->width;
        d->scissor[3] = (int32_t)s->height;
    }
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetRenderTarget(_cpu *cpu)
{
    DEV;
    uint32_t idx = ARG(1), out = ARG(2);
    if (idx >= D3D_MAX_RT || out == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->rt[idx], out);
    RET(d->rt[idx] != NULL ? D3D_OK : D3DERR_NOTFOUND);
    d3d_unlock(d);
}

void rt_com_D3DDevice9_SetDepthStencilSurface(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(1);
    rt_com_object *o = p != 0 ? d3d_surface_from_ptr(p) : NULL;
    if (o != NULL && !(d3d_surf(o)->usage & D3DUSAGE_DEPTHSTENCIL))
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    if (d->ds != o)
    {
        d3d_end_pass(d);
        bind(&d->ds, o);
    }
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetDepthStencilSurface(_cpu *cpu)
{
    DEV;
    uint32_t out = ARG(1);
    if (out == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->ds, out);
    RET(d->ds != NULL ? D3D_OK : D3DERR_NOTFOUND);
    d3d_unlock(d);
}

void rt_com_D3DDevice9_BeginScene(_cpu *cpu)
{
    DEV;
    d3d_lock(d);
    RET(d->in_scene ? D3DERR_INVALIDCALL : D3D_OK);
    d->in_scene = true;
    d3d_unlock(d);
}

void rt_com_D3DDevice9_EndScene(_cpu *cpu)
{
    DEV;
    d3d_lock(d);
    RET(d->in_scene ? D3D_OK : D3DERR_INVALIDCALL);
    d->in_scene = false;
    d3d_unlock(d);
}

/* ---------------------------------------------------------------- device methods: fixed-function state */

/* Transform state index: VIEW 2, PROJECTION 3, TEXTURE0..7 16..23,
   WORLDMATRIX(n) 256 + n. */
static bool transform_index(uint32_t t)
{
    return t == 2 || t == 3 || (t >= 16 && t <= 23) || (t >= 256 && t < 512);
}

void rt_com_D3DDevice9_SetTransform(_cpu *cpu)
{
    DEV;
    uint32_t t = ARG(1), p = ARG(2);
    if (!transform_index(t) || p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    memcpy(d->transforms[t], G2H(p), 64);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetTransform(_cpu *cpu)
{
    DEV;
    uint32_t t = ARG(1), p = ARG(2);
    if (!transform_index(t) || p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    memcpy(G2H(p), d->transforms[t], 64);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* The new matrix is matrix * current. UNCONFIRMED: the D3D9 order. */
void rt_com_D3DDevice9_MultiplyTransform(_cpu *cpu)
{
    DEV;
    uint32_t t = ARG(1), p = ARG(2), i, j, k;
    float a[16], r[16];
    if (!transform_index(t) || p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    memcpy(a, G2H(p), 64);
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
        {
            float s = 0.0f;
            for (k = 0; k < 4; k++) s += a[i * 4 + k] * d->transforms[t][k * 4 + j];
            r[i * 4 + j] = s;
        }
    memcpy(d->transforms[t], r, 64);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* SetViewport(const D3DVIEWPORT9 *vp) */
void rt_com_D3DDevice9_SetViewport(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(1), v[6], i;
    D3DSurface *rt0;
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    for (i = 0; i < 6; i++) v[i] = rd32(p + 4u * i);
    d3d_lock(d);
    rt0 = d3d_surf(d->rt[0]);
    if (v[2] == 0 || v[3] == 0 || v[0] > rt0->width || v[1] > rt0->height || v[2] > rt0->width - v[0] ||
        v[3] > rt0->height - v[1] || !(d3d_f(v[4]) >= 0.0f && d3d_f(v[5]) <= 1.0f && d3d_f(v[4]) <= d3d_f(v[5])))
    {
        d3d_unlock(d);
        RET(D3DERR_INVALIDCALL);
        return;
    }
    memcpy(d->viewport, v, sizeof v);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetViewport(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(1), i;
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    for (i = 0; i < 6; i++) wr32(p + 4u * i, d->viewport[i]);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetMaterial(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    memcpy(d->material, G2H(ARG(1)), sizeof d->material);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetMaterial(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    memcpy(G2H(ARG(1)), d->material, sizeof d->material);
    d3d_unlock(d);
    RET(D3D_OK);
}

#define D3DLIGHT9_SIZE 104u

void rt_com_D3DDevice9_SetLight(_cpu *cpu)
{
    DEV;
    uint32_t idx = ARG(1), p = ARG(2), type;
    if (p == 0 || (type = rd32(p)) < 1 || type > 3)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    if (d->lights.count >= 4096 && d->lights[@(idx)] == nil)
    {
        d3d_unlock(d);
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d->lights[@(idx)] = [NSData dataWithBytes:G2H(p) length:D3DLIGHT9_SIZE];
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetLight(_cpu *cpu)
{
    DEV;
    uint32_t idx = ARG(1), p = ARG(2);
    NSData *l;
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    l = d->lights[@(idx)];
    if (l != nil) memcpy(G2H(p), l.bytes, D3DLIGHT9_SIZE);
    d3d_unlock(d);
    RET(l != nil ? D3D_OK : D3DERR_INVALIDCALL);
}

/* LightEnable(DWORD index, BOOL enable): a light that was never set gets
   the default light (directional, white, direction +z). */
void rt_com_D3DDevice9_LightEnable(_cpu *cpu)
{
    DEV;
    uint32_t idx = ARG(1);
    d3d_lock(d);
    if (d->lights[@(idx)] == nil)
    {
        float l[26];
        if (d->lights.count >= 4096)
        {
            d3d_unlock(d);
            RET(D3DERR_INVALIDCALL);
            return;
        }
        /* D3DLIGHT9: Type (dword 0), Diffuse (1..4), Specular (5..8),
           Ambient (9..12), Position (13..15), Direction (16..18), ... */
        memset(l, 0, sizeof l);
        memcpy(&l[0], &(uint32_t){ 3u }, 4); /* D3DLIGHT_DIRECTIONAL */
        l[1] = l[2] = l[3] = 1.0f;          /* Diffuse (1, 1, 1, 0) */
        l[18] = 1.0f;                       /* Direction (0, 0, 1) */
        d->lights[@(idx)] = [NSData dataWithBytes:l length:D3DLIGHT9_SIZE];
    }
    if (ARG(2))
        [d->lights_on addObject:@(idx)];
    else
        [d->lights_on removeObject:@(idx)];
    d3d_unlock(d);
    RET(D3D_OK);
}

/* UNCONFIRMED: D3D9 returns 128 for an enabled light. */
void rt_com_D3DDevice9_GetLightEnable(_cpu *cpu)
{
    DEV;
    uint32_t idx = ARG(1), p = ARG(2);
    bool set;
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    set = d->lights[@(idx)] != nil;
    if (set) wr32(p, [d->lights_on containsObject:@(idx)] ? 128u : 0u);
    d3d_unlock(d);
    RET(set ? D3D_OK : D3DERR_INVALIDCALL);
}

void rt_com_D3DDevice9_SetClipPlane(_cpu *cpu)
{
    DEV;
    if (ARG(1) >= 6 || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    memcpy(d->clip_planes[ARG(1)], G2H(ARG(2)), 16);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetClipPlane(_cpu *cpu)
{
    DEV;
    if (ARG(1) >= 6 || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    memcpy(G2H(ARG(2)), d->clip_planes[ARG(1)], 16);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetRenderState(_cpu *cpu)
{
    DEV;
    uint32_t s = ARG(1);
    if (s >= D3D_RS_COUNT)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    d->rs[s] = ARG(2);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetRenderState(_cpu *cpu)
{
    DEV;
    uint32_t s = ARG(1);
    if (s >= D3D_RS_COUNT || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    wr32(ARG(2), d->rs[s]);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetClipStatus(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    d->clip_status[0] = rd32(ARG(1));
    d->clip_status[1] = rd32(ARG(1) + 4);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetClipStatus(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    wr32(ARG(1), d->clip_status[0]);
    wr32(ARG(1) + 4, d->clip_status[1]);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* Sampler stage: 0..15 pixel samplers, D3DVERTEXTEXTURESAMPLER0..3
   (257..260) vertex samplers. Returns -1 for other stages. */
static int sampler_slot(uint32_t stage)
{
    if (stage < D3D_PS_SAMPLERS) return (int)stage;
    if (stage >= 257 && stage < 257 + D3D_VS_SAMPLERS) return (int)(D3D_PS_SAMPLERS + stage - 257);
    return -1;
}

void rt_com_D3DDevice9_GetTexture(_cpu *cpu)
{
    DEV;
    int s = sampler_slot(ARG(1));
    if (s < 0 || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->tex[s], ARG(2));
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetTexture(_cpu *cpu)
{
    DEV;
    int s = sampler_slot(ARG(1));
    rt_com_object *o = NULL;
    if (s < 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    if (ARG(2) != 0)
    {
        o = rt_com_this(ARG(2), 0, NULL);
        if (!d3d_is_texture(o))
            rt_fatal("D3D9: SetTexture with a %s object", o->cls->name);
    }
    d3d_lock(d);
    bind(&d->tex[s], o);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetTextureStageState(_cpu *cpu)
{
    DEV;
    if (ARG(1) >= 8 || ARG(2) >= D3D_TSS_COUNT || ARG(3) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    wr32(ARG(3), d->tss[ARG(1)][ARG(2)]);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* Texture stage states are fixed-function state; they are kept for the
   getter (a fixed-function draw stops the process). */
void rt_com_D3DDevice9_SetTextureStageState(_cpu *cpu)
{
    DEV;
    if (ARG(1) >= 8 || ARG(2) >= D3D_TSS_COUNT)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    d->tss[ARG(1)][ARG(2)] = ARG(3);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetSamplerState(_cpu *cpu)
{
    DEV;
    int s = sampler_slot(ARG(1));
    if (s < 0 || ARG(2) == 0 || ARG(2) >= D3D_SS_COUNT || ARG(3) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    wr32(ARG(3), d->ss[s][ARG(2)]);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetSamplerState(_cpu *cpu)
{
    DEV;
    int s = sampler_slot(ARG(1));
    if (s < 0 || ARG(2) == 0 || ARG(2) >= D3D_SS_COUNT)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    d->ss[s][ARG(2)] = ARG(3);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* ValidateDevice(DWORD *passes): every shader state draws in one pass. */
void rt_com_D3DDevice9_ValidateDevice(_cpu *cpu)
{
    DEV;
    (void)d;
    if (ARG(1) != 0) wr32(ARG(1), 1);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetScissorRect(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(1), i;
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    for (i = 0; i < 4; i++) d->scissor[i] = (int32_t)rd32(p + 4u * i);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetScissorRect(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(1), i;
    if (p == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    for (i = 0; i < 4; i++) wr32(p + 4u * i, (uint32_t)d->scissor[i]);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* Vertex processing is always done by the GPU vertex shader; the flag is
   kept for the getter. */
void rt_com_D3DDevice9_SetSoftwareVertexProcessing(_cpu *cpu)
{
    DEV;
    d3d_lock(d);
    d->software_vp = ARG(1) != 0;
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetSoftwareVertexProcessing(_cpu *cpu)
{
    DEV;
    RET(d->software_vp ? 1u : 0u);
}

void rt_com_D3DDevice9_SetNPatchMode(_cpu *cpu)
{
    DEV;
    d3d_lock(d);
    d->npatch_mode = ARG(1);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetNPatchMode(_cpu *cpu)
{
    DEV;
    RETF(d3d_f(d->npatch_mode));
}

/* ---------------------------------------------------------------- device methods: vertex input */

/* CreateVertexDeclaration(const D3DVERTEXELEMENT9 *elements, IDirect3DVertexDeclaration9 **out) */
void rt_com_D3DDevice9_CreateVertexDeclaration(_cpu *cpu)
{
    DEV;
    uint32_t p = ARG(1), out = ARG(2), n = 0, r;
    d3d_velem el[64];
    rt_com_object *o = NULL;
    if (p == 0 || out == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    for (;;)
    {
        uint32_t q = p + 8u * n;
        if (rd16(q) == 0xFFu) break; /* D3DDECL_END */
        if (n == 64)
        {
            RET(D3DERR_INVALIDCALL);
            return;
        }
        el[n].stream = rd16(q);
        el[n].offset = rd16(q + 2);
        el[n].type = rd8(q + 4);
        el[n].method = rd8(q + 5);
        el[n].usage = rd8(q + 6);
        el[n].index = rd8(q + 7);
        n++;
    }
    d3d_lock(d);
    r = d3d_create_decl(d, el, n, 0, &o);
    d3d_unlock(d);
    if (r == D3D_OK) wr32(out, rt_com_ptr(o, 0));
    RET(r);
}

void rt_com_D3DDevice9_SetVertexDeclaration(_cpu *cpu)
{
    DEV;
    rt_com_object *o = ARG(1) != 0 ? rt_com_this(ARG(1), 0, &d3d_decl_class) : NULL;
    d3d_lock(d);
    bind(&d->decl, o);
    d->fvf = o != NULL ? d3d_decl(o)->fvf : 0;
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetVertexDeclaration(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->decl, ARG(1));
    d3d_unlock(d);
    RET(D3D_OK);
}

static void add_el(d3d_velem *el, uint32_t *n, uint32_t *off, uint32_t type, uint32_t size, uint32_t usage,
                   uint32_t index)
{
    el[*n].stream = 0;
    el[*n].offset = (uint16_t)*off;
    el[*n].type = (uint8_t)type;
    el[*n].method = 0;
    el[*n].usage = (uint8_t)usage;
    el[*n].index = (uint8_t)index;
    (*n)++;
    *off += size;
}

/* The declaration of an FVF code. Returns false for codes that are not
   valid. */
static bool fvf_decl(uint32_t fvf, d3d_velem *el, uint32_t *n)
{
    uint32_t off = 0, pos = fvf & 0x400Eu, tc, i, weights = 0;
    *n = 0;
    switch (pos)
    {
    case 0: break;
    case 0x2: add_el(el, n, &off, 2, 12, D3DDECLUSAGE_POSITION, 0); break;
    case 0x4: add_el(el, n, &off, 3, 16, D3DDECLUSAGE_POSITIONT, 0); break;
    case 0x4002: add_el(el, n, &off, 3, 16, D3DDECLUSAGE_POSITION, 0); break;
    case 0x6: case 0x8: case 0xA: case 0xC: case 0xE:
        add_el(el, n, &off, 2, 12, D3DDECLUSAGE_POSITION, 0);
        weights = (pos - 0x4u) / 2u; /* XYZB1 = 1 ... XYZB5 = 5 */
        break;
    default: return false;
    }
    if (weights > 0)
    {
        bool last_ubyte4 = (fvf & 0x1000u) != 0, last_color = (fvf & 0x8000u) != 0;
        uint32_t nf = weights - ((last_ubyte4 || last_color) ? 1u : 0u);
        if (nf > 0) add_el(el, n, &off, nf - 1u, 4u * nf, D3DDECLUSAGE_BLENDWEIGHT, 0);
        if (last_ubyte4) add_el(el, n, &off, 5, 4, D3DDECLUSAGE_BLENDINDICES, 0);
        if (last_color) add_el(el, n, &off, 4, 4, D3DDECLUSAGE_BLENDINDICES, 0);
    }
    if (fvf & 0x10u) add_el(el, n, &off, 2, 12, D3DDECLUSAGE_NORMAL, 0);
    if (fvf & 0x20u) add_el(el, n, &off, 0, 4, D3DDECLUSAGE_PSIZE, 0);
    if (fvf & 0x40u) add_el(el, n, &off, 4, 4, D3DDECLUSAGE_COLOR, 0);
    if (fvf & 0x80u) add_el(el, n, &off, 4, 4, D3DDECLUSAGE_COLOR, 1);
    tc = (fvf >> 8) & 0xFu;
    if (tc > 8) return false;
    for (i = 0; i < tc; i++)
    {
        static const uint32_t types[4] = { 1, 2, 3, 0 }, sizes[4] = { 8, 12, 16, 4 };
        uint32_t f = (fvf >> (16 + 2 * i)) & 3u;
        add_el(el, n, &off, types[f], sizes[f], D3DDECLUSAGE_TEXCOORD, i);
    }
    return true;
}

void rt_com_D3DDevice9_SetFVF(_cpu *cpu)
{
    DEV;
    uint32_t fvf = ARG(1), n, r;
    d3d_velem el[64];
    rt_com_object *o;
    NSValue *v;
    if (fvf == 0)
    {
        d3d_lock(d);
        d->fvf = 0;
        d3d_unlock(d);
        RET(D3D_OK);
        return;
    }
    if (!fvf_decl(fvf, el, &n))
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    @autoreleasepool
    {
        v = d->fvf_decls[@(fvf)];
        if (v != nil)
            o = (rt_com_object *)v.pointerValue;
        else
        {
            if (d->fvf_decls.count >= MAX_FVF_DECLS)
            {
                NSArray<NSValue *> *all = d->fvf_decls.allValues;
                [d->fvf_decls removeAllObjects];
                for (NSValue *x in all) rt_com_release((rt_com_object *)x.pointerValue);
            }
            r = d3d_create_decl(d, el, n, fvf, &o);
            if (r != D3D_OK)
            {
                d3d_unlock(d);
                RET(r);
                return;
            }
            d->fvf_decls[@(fvf)] = [NSValue valueWithPointer:o];
        }
        bind(&d->decl, o);
        d->fvf = fvf;
    }
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetFVF(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    wr32(ARG(1), d->fvf);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *vb, UINT offset, UINT stride) */
void rt_com_D3DDevice9_SetStreamSource(_cpu *cpu)
{
    DEV;
    uint32_t s = ARG(1);
    rt_com_object *o = ARG(2) != 0 ? rt_com_this(ARG(2), 0, &d3d_vb_class) : NULL;
    if (s >= D3D_MAX_STREAMS)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    bind(&d->streams[s].buf, o);
    d->streams[s].offset = ARG(3);
    d->streams[s].stride = ARG(4);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetStreamSource(_cpu *cpu)
{
    DEV;
    uint32_t s = ARG(1);
    if (s >= D3D_MAX_STREAMS || ARG(2) == 0 || ARG(3) == 0 || ARG(4) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->streams[s].buf, ARG(2));
    wr32(ARG(3), d->streams[s].offset);
    wr32(ARG(4), d->streams[s].stride);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetStreamSourceFreq(_cpu *cpu)
{
    DEV;
    uint32_t s = ARG(1), v = ARG(2);
    if (s >= D3D_MAX_STREAMS || ((v & (3u << 30)) == (3u << 30)) || (s == 0 && (v & (2u << 30))))
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    d->streams[s].freq = v;
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetStreamSourceFreq(_cpu *cpu)
{
    DEV;
    uint32_t s = ARG(1);
    if (s >= D3D_MAX_STREAMS || ARG(2) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    wr32(ARG(2), d->streams[s].freq);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetIndices(_cpu *cpu)
{
    DEV;
    rt_com_object *o = ARG(1) != 0 ? rt_com_this(ARG(1), 0, &d3d_ib_class) : NULL;
    d3d_lock(d);
    bind(&d->ib, o);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetIndices(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->ib, ARG(1));
    d3d_unlock(d);
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- device methods: shaders */

void rt_com_D3DDevice9_CreateVertexShader(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = d3d_create_shader(d, false, ARG(1), ARG(2));
    }
    d3d_unlock(d);
    RET(r);
}

void rt_com_D3DDevice9_CreatePixelShader(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = d3d_create_shader(d, true, ARG(1), ARG(2));
    }
    d3d_unlock(d);
    RET(r);
}

void rt_com_D3DDevice9_SetVertexShader(_cpu *cpu)
{
    DEV;
    rt_com_object *o = ARG(1) != 0 ? rt_com_this(ARG(1), 0, &d3d_vs_class) : NULL;
    d3d_lock(d);
    bind(&d->vs, o);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetVertexShader(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->vs, ARG(1));
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetPixelShader(_cpu *cpu)
{
    DEV;
    rt_com_object *o = ARG(1) != 0 ? rt_com_this(ARG(1), 0, &d3d_ps_class) : NULL;
    d3d_lock(d);
    bind(&d->ps, o);
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_GetPixelShader(_cpu *cpu)
{
    DEV;
    if (ARG(1) == 0)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    give(d->ps, ARG(1));
    d3d_unlock(d);
    RET(D3D_OK);
}

/* Constant copies: count registers of elem bytes from start, at most max
   registers. set copies guest to host, get copies host to guest. */
static void constants(_cpu *cpu, D3DDevice *d, void *base, uint32_t max, uint32_t elem, bool set)
{
    uint32_t start = ARG(1), p = ARG(2), count = ARG(3);
    if (p == 0 || start > max || count > max - start)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    if (set)
    {
        memcpy((uint8_t *)base + (size_t)start * elem, G2H(p), (size_t)count * elem);
        if (base == (void *)d->vs_c || base == (void *)d->vs_i || base == (void *)d->vs_b) d->vs_const_ver++;
        else d->ps_const_ver++;
    }
    else
        memcpy(G2H(p), (uint8_t *)base + (size_t)start * elem, (size_t)count * elem);
    d3d_unlock(d);
    RET(D3D_OK);
}

/* BOOL constants are stored as 0 or 1. */
static void set_bools(_cpu *cpu, D3DDevice *d, uint32_t *base)
{
    uint32_t start = ARG(1), p = ARG(2), count = ARG(3), i;
    if (p == 0 || start > D3DSH_CONST_B || count > D3DSH_CONST_B - start)
    {
        RET(D3DERR_INVALIDCALL);
        return;
    }
    d3d_lock(d);
    for (i = 0; i < count; i++) base[start + i] = rd32(p + 4u * i) != 0;
    if (base == d->vs_b) d->vs_const_ver++;
    else d->ps_const_ver++;
    d3d_unlock(d);
    RET(D3D_OK);
}

void rt_com_D3DDevice9_SetVertexShaderConstantF(_cpu *cpu) { DEV; constants(cpu, d, d->vs_c, D3DSH_VS_CONST_F, 16, true); }
void rt_com_D3DDevice9_GetVertexShaderConstantF(_cpu *cpu) { DEV; constants(cpu, d, d->vs_c, D3DSH_VS_CONST_F, 16, false); }
void rt_com_D3DDevice9_SetVertexShaderConstantI(_cpu *cpu) { DEV; constants(cpu, d, d->vs_i, D3DSH_CONST_I, 16, true); }
void rt_com_D3DDevice9_GetVertexShaderConstantI(_cpu *cpu) { DEV; constants(cpu, d, d->vs_i, D3DSH_CONST_I, 16, false); }
void rt_com_D3DDevice9_SetVertexShaderConstantB(_cpu *cpu) { DEV; set_bools(cpu, d, d->vs_b); }
void rt_com_D3DDevice9_GetVertexShaderConstantB(_cpu *cpu) { DEV; constants(cpu, d, d->vs_b, D3DSH_CONST_B, 4, false); }
void rt_com_D3DDevice9_SetPixelShaderConstantF(_cpu *cpu) { DEV; constants(cpu, d, d->ps_c, D3DSH_PS_CONST_F, 16, true); }
void rt_com_D3DDevice9_GetPixelShaderConstantF(_cpu *cpu) { DEV; constants(cpu, d, d->ps_c, D3DSH_PS_CONST_F, 16, false); }
void rt_com_D3DDevice9_SetPixelShaderConstantI(_cpu *cpu) { DEV; constants(cpu, d, d->ps_i, D3DSH_CONST_I, 16, true); }
void rt_com_D3DDevice9_GetPixelShaderConstantI(_cpu *cpu) { DEV; constants(cpu, d, d->ps_i, D3DSH_CONST_I, 16, false); }
void rt_com_D3DDevice9_SetPixelShaderConstantB(_cpu *cpu) { DEV; set_bools(cpu, d, d->ps_b); }
void rt_com_D3DDevice9_GetPixelShaderConstantB(_cpu *cpu) { DEV; constants(cpu, d, d->ps_b, D3DSH_CONST_B, 4, false); }

/* CreateQuery(D3DQUERYTYPE type, IDirect3DQuery9 **out) */
void rt_com_D3DDevice9_CreateQuery(_cpu *cpu)
{
    DEV;
    uint32_t r;
    d3d_lock(d);
    @autoreleasepool
    {
        r = d3d_create_query(d, ARG(1), ARG(2));
    }
    d3d_unlock(d);
    RET(r);
}
