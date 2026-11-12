/*
 * Direct3D 9 on Metal: declarations shared by rt_d3d9.m (IDirect3D9, the
 * device, state and drawing) and rt_d3d9_res.m (formats and resources).
 *
 * Objects:
 *   - Each D3D9 COM object is a runtime COM object (rt_com.h). Its class
 *     data is an Objective-C object (retained with __bridge_retained and
 *     released by the class destroy function).
 *   - One recursive mutex for each device protects the device and all of
 *     its resources. Every method takes it.
 *   - The device records into one command buffer at a time. Each command
 *     buffer has a serial number; a completion handler publishes the last
 *     completed serial. A resource remembers the serial of the last command
 *     buffer that used it, so a CPU write waits only when the GPU can still
 *     read the old data.
 *
 * Only shader-based drawing is supported: a draw without a vertex shader
 * and a pixel shader stops the process with a message.
 */
#ifndef RT_D3D9_INT_H
#define RT_D3D9_INT_H

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"
#include "rt_d3d9_shader.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>

/* ---------------------------------------------------------------- constants */

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_OUTOFVIDEOMEMORY 0x8876017Cu
#define D3DERR_NOTFOUND 0x88760866u
#define D3DERR_WASSTILLDRAWING 0x8876021Cu
#define D3DOK_NOAUTOGEN 0x0876086Fu

#define D3DRTYPE_SURFACE 1u
#define D3DRTYPE_VOLUME 2u
#define D3DRTYPE_TEXTURE 3u
#define D3DRTYPE_VOLUMETEXTURE 4u
#define D3DRTYPE_CUBETEXTURE 5u
#define D3DRTYPE_VERTEXBUFFER 6u
#define D3DRTYPE_INDEXBUFFER 7u

#define D3DPOOL_DEFAULT 0u
#define D3DPOOL_MANAGED 1u
#define D3DPOOL_SYSTEMMEM 2u
#define D3DPOOL_SCRATCH 3u

#define D3DUSAGE_RENDERTARGET 0x1u
#define D3DUSAGE_DEPTHSTENCIL 0x2u
#define D3DUSAGE_WRITEONLY 0x8u
#define D3DUSAGE_SOFTWAREPROCESSING 0x10u
#define D3DUSAGE_DONOTCLIP 0x20u
#define D3DUSAGE_POINTS 0x40u
#define D3DUSAGE_DYNAMIC 0x200u
#define D3DUSAGE_AUTOGENMIPMAP 0x400u
#define D3DUSAGE_QUERY_SRGBREAD 0x10000u
#define D3DUSAGE_QUERY_FILTER 0x20000u
#define D3DUSAGE_QUERY_SRGBWRITE 0x40000u
#define D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING 0x80000u
#define D3DUSAGE_QUERY_VERTEXTEXTURE 0x100000u
#define D3DUSAGE_QUERY_WRAPANDMIP 0x200000u

#define D3DLOCK_READONLY 0x10u
#define D3DLOCK_NOSYSLOCK 0x800u
#define D3DLOCK_NOOVERWRITE 0x1000u
#define D3DLOCK_DISCARD 0x2000u
#define D3DLOCK_DONOTWAIT 0x4000u
#define D3DLOCK_NO_DIRTY_UPDATE 0x8000u

#define D3DFMT_UNKNOWN 0u
#define D3DFMT_R8G8B8 20u
#define D3DFMT_A8R8G8B8 21u
#define D3DFMT_X8R8G8B8 22u
#define D3DFMT_R5G6B5 23u
#define D3DFMT_X1R5G5B5 24u
#define D3DFMT_A1R5G5B5 25u
#define D3DFMT_A4R4G4B4 26u
#define D3DFMT_A8 28u
#define D3DFMT_X4R4G4B4 30u
#define D3DFMT_A2B10G10R10 31u
#define D3DFMT_A8B8G8R8 32u
#define D3DFMT_X8B8G8R8 33u
#define D3DFMT_G16R16 34u
#define D3DFMT_A2R10G10B10 35u
#define D3DFMT_A16B16G16R16 36u
#define D3DFMT_L8 50u
#define D3DFMT_A8L8 51u
#define D3DFMT_V8U8 60u
#define D3DFMT_Q8W8V8U8 63u
#define D3DFMT_V16U16 64u
#define D3DFMT_D16_LOCKABLE 70u
#define D3DFMT_D32 71u
#define D3DFMT_D24S8 75u
#define D3DFMT_D24X8 77u
#define D3DFMT_D16 80u
#define D3DFMT_L16 81u
#define D3DFMT_D24FS8 83u
#define D3DFMT_INDEX16 101u
#define D3DFMT_INDEX32 102u
#define D3DFMT_R16F 111u
#define D3DFMT_G16R16F 112u
#define D3DFMT_A16B16G16R16F 113u
#define D3DFMT_R32F 114u
#define D3DFMT_G32R32F 115u
#define D3DFMT_A32B32G32R32F 116u
#define D3DFMT_DXT1 0x31545844u
#define D3DFMT_DXT2 0x32545844u
#define D3DFMT_DXT3 0x33545844u
#define D3DFMT_DXT4 0x34545844u
#define D3DFMT_DXT5 0x35545844u

/* Conversions on upload (formats that Metal does not have). */
#define D3DCONV_NONE 0
#define D3DCONV_R8G8B8 1  /* 24-bit BGR to BGRA8 */
#define D3DCONV_A4R4G4B4 2 /* to BGRA8 */
#define D3DCONV_X4R4G4B4 3 /* to BGRA8, alpha 1 */

typedef struct d3d_format {
    uint32_t d3d;
    MTLPixelFormat pf;      /* storage format */
    MTLPixelFormat pf_srgb; /* sRGB view format, or MTLPixelFormatInvalid */
    uint8_t bytes;          /* bytes per pixel, or per 4x4 block */
    uint8_t conv;           /* D3DCONV_* */
    bool block;             /* 4x4 block compressed */
    bool depth, stencil;
    bool renderable, blendable, filterable;
    bool swizzle;           /* sampling needs swz */
    MTLTextureSwizzleChannels swz;
    uint8_t guest_bytes;    /* bytes per pixel or block in guest (lock) memory */
} d3d_format;

/* NULL if the format is not supported. */
const d3d_format *d3d_format_find(uint32_t fmt);

/* ---------------------------------------------------------------- objects */

@class D3DDevice;

/* A cache of Metal state objects with fixed-size byte keys (the D3D9
   state that selects the object). Open addressing; a full cache is
   emptied, as the earlier dictionary caches were. See rt_d3d9_draw.m. */
@interface D3DStateCache : NSObject {
@public
    const char *name;
    uint32_t key_size, limit, count, mask;
    uint32_t *slots;   /* entry index + 1, 0 = empty */
    uint64_t *hashes;  /* per entry */
    uint8_t *keys;     /* limit * key_size bytes */
    void **objs;       /* per entry, retained (CFBridgingRetain) */
}
@end

/* A retired GPU buffer that may be reused after its serial completes. */
@interface D3DRetired : NSObject {
@public
    id<MTLBuffer> buf;
    id<MTLTexture> tex;
    uint64_t serial;
}
@end

@interface D3DTexture : NSObject {
@public
    D3DDevice *dev;
    rt_com_object *com;
    bool cube;
    bool volume;                   /* IDirect3DVolumeTexture9 (3D texture) */
    uint32_t fmt, usage, pool, width, height, levels;
    uint32_t depth;                /* 1 for 2D and cube textures */
    const d3d_format *f;
    id<MTLTexture> tex;            /* storage; nil for SYSTEMMEM and SCRATCH */
    id<MTLTexture> view, view_srgb; /* sampling views (swizzle) */
    id<MTLTexture> rt_srgb;         /* sRGB render target view (no swizzle) */
    NSMutableArray<D3DRetired *> *retired;
    uint32_t mem[6][16];           /* guest copy of each level (lock memory), 0 = none */
    uint32_t pitch[16], rows[16], lsize[16], lw[16], lh[16];
    uint32_t ld[16];               /* level depth (1 for 2D and cube) */
    uint32_t slice_pitch[16];      /* pitch * rows: bytes of one depth slice */
    uint16_t locked[6];            /* bit per level */
    uint32_t lock_flags[6][16];
    rt_com_object *surf[6][16];    /* child surfaces (GetSurfaceLevel) */
    uint64_t last_use;
    uint32_t lod, priority, autogen_filter;
}
@end

@interface D3DSurface : NSObject {
@public
    D3DDevice *dev;
    rt_com_object *com;
    rt_com_object *container; /* texture COM object of a child surface, or NULL */
    D3DTexture *ctex;         /* its data */
    uint32_t face, level;
    uint32_t fmt, usage, pool, width, height, msaa, msaa_quality;
    const d3d_format *f;
    bool lockable;
    id<MTLTexture> tex;       /* standalone surfaces; child surfaces use ctex->tex */
    id<MTLTexture> rt_srgb;   /* sRGB render target view of tex */
    uint32_t mem;             /* guest lock memory of a standalone surface */
    uint32_t pitch;
    bool locked;
    uint32_t lock_flags;
    uint64_t last_use;
}
@end

@interface D3DBuffer : NSObject {
@public
    D3DDevice *dev;
    rt_com_object *com;
    bool index;
    uint32_t size, usage, pool, format_or_fvf;
    id<MTLBuffer> buf;
    NSMutableArray<D3DRetired *> *retired;
    uint32_t staging;          /* guest memory of the current lock */
    uint32_t staging_size;
    bool locked;
    uint32_t lock_off, lock_size, lock_flags;
    uint64_t last_use;
}
@end

typedef struct d3d_velem {
    uint16_t stream, offset;
    uint8_t type, method, usage, index;
} d3d_velem;

@interface D3DDecl : NSObject {
@public
    D3DDevice *dev;
    rt_com_object *com;
    uint32_t id, n, fvf;
    d3d_velem el[64];
}
@end

@interface D3DShader : NSObject {
@public
    D3DDevice *dev;
    rt_com_object *com;
    uint32_t id;
    d3d_shader sh;
    id<MTLFunction> vs_func;
    NSMutableDictionary<NSNumber *, id<MTLFunction>> *ps_funcs; /* key: vs id << 16 | depth compare mask */
}
@end

@interface D3DQuery : NSObject {
@public
    D3DDevice *dev;
    rt_com_object *com;
    uint32_t type;
    uint64_t serial;
    bool issued;
}
@end

#define D3D_MAX_RT 4
#define D3D_MAX_STREAMS 16
#define D3D_PS_SAMPLERS 16
#define D3D_VS_SAMPLERS 4
#define D3D_SAMPLERS (D3D_PS_SAMPLERS + D3D_VS_SAMPLERS) /* index 16..19: D3DVERTEXTEXTURESAMPLER0..3 */
#define D3D_RS_COUNT 256
#define D3D_SS_COUNT 14
#define D3D_TSS_COUNT 33
#define D3D_FRAMES_IN_FLIGHT 2

typedef struct d3d_stream {
    rt_com_object *buf;
    uint32_t offset, stride, freq;
} d3d_stream;

@interface D3DDevice : NSObject {
@public
    rt_com_object *com;
    rt_com_object *d3d9; /* the IDirect3D9 object (one reference) */
    id<MTLDevice> mtl;
    id<MTLCommandQueue> queue;
    CAMetalLayer *layer;
    pthread_mutex_t lock;
    pthread_mutex_t done_lock;
    pthread_cond_t done_cv;
    _Atomic uint64_t completed;
    uint64_t serial; /* serial of the command buffer that is being recorded */
    id<MTLCommandBuffer> cb;
    id<MTLRenderCommandEncoder> enc;
    bool enc_draws;
    /* Attachments of the render pass in enc. */
    MTLPixelFormat pass_color[D3D_MAX_RT], pass_depth, pass_stencil;
    uint32_t pass_samples, pass_w, pass_h;
    bool pass_srgb;
    uint64_t present_serial[D3D_FRAMES_IN_FLIGHT];
    uint32_t present_count;

    /* Creation and presentation. */
    uint32_t adapter, devtype, focus_hwnd, behavior;
    uint32_t pp[14]; /* D3DPRESENT_PARAMETERS */
    rt_com_object *backbuffer, *auto_ds;
    uint16_t gamma[3][256];
    bool gamma_set;
    id<MTLTexture> gamma_tex;
    id<MTLRenderPipelineState> present_pso;
    id<MTLSamplerState> present_linear, present_point;
    id<MTLLibrary> builtin_lib;
    uint32_t cursor_x, cursor_y;
    bool cursor_shown;

    /* State. */
    uint32_t rs[D3D_RS_COUNT];
    uint32_t ss[D3D_SAMPLERS][D3D_SS_COUNT];
    uint32_t tss[8][D3D_TSS_COUNT];
    rt_com_object *rt[D3D_MAX_RT], *ds;
    rt_com_object *tex[D3D_SAMPLERS];
    d3d_stream streams[D3D_MAX_STREAMS];
    rt_com_object *ib, *decl, *vs, *ps;
    uint32_t fvf;
    NSMutableDictionary<NSNumber *, NSValue *> *fvf_decls; /* FVF to D3DDecl COM object (rt_com_object *) */
    float vs_c[D3DSH_VS_CONST_F][4];
    int32_t vs_i[D3DSH_CONST_I][4];
    uint32_t vs_b[D3DSH_CONST_B];
    float ps_c[D3DSH_PS_CONST_F][4];
    int32_t ps_i[D3DSH_CONST_I][4];
    uint32_t ps_b[D3DSH_CONST_B];
    uint32_t viewport[6]; /* X, Y, Width, Height, MinZ, MaxZ (float bits) */
    int32_t scissor[4];   /* left, top, right, bottom */
    float transforms[512][16];
    uint8_t material[68];
    NSMutableDictionary<NSNumber *, NSData *> *lights;
    NSMutableSet<NSNumber *> *lights_on;
    float clip_planes[6][4];
    uint32_t clip_status[2];
    uint32_t npatch_mode;
    bool software_vp;
    bool in_scene;

    /* Pending clears for the next render pass. */
    bool clear_color[D3D_MAX_RT], clear_depth, clear_stencil;
    float clear_rgba[4], clear_z;
    uint32_t clear_s;

    /* Encoder state that do_draw set on enc (rt_d3d9_draw.m). A draw sets
       only the state that is different. enc_state_reset clears the record
       for a new encoder and after clear_quads. The objects are strong
       references, so that the address of an object in the record cannot
       become the address of a new object. es_valid false means that no
       state is recorded. */
    bool es_valid;
    id<MTLRenderPipelineState> es_pipe;
    id<MTLDepthStencilState> es_ds;
    uint32_t es_sref, es_blendf, es_cull, es_fill, es_slope;
    MTLViewport es_vp;
    MTLScissorRect es_sr;
    float es_vs_misc[4], es_ps_misc[4];
    uint32_t es_vs_shader, es_ps_shader; /* D3DShader id of the bound constants */
    uint64_t es_vs_ver, es_ps_ver;       /* constant versions of the bound constants */
    id<MTLTexture> es_tex[D3D_SAMPLERS];
    id<MTLSamplerState> es_smp[D3D_SAMPLERS];
    id<MTLBuffer> es_vb[D3D_MAX_STREAMS];
    NSUInteger es_vb_off[D3D_MAX_STREAMS];
    bool es_null_vb;
    /* Incremented when a Set*ShaderConstant* call or a reset changes the
       vertex or pixel shader constants. */
    uint64_t vs_const_ver, ps_const_ver;

    /* Caches. */
    D3DStateCache *pipelines;
    D3DStateCache *ds_states;
    D3DStateCache *samplers;
    uint32_t next_shader_id, next_decl_id;

    /* Upload memory for draws with user pointers and generated indices. */
    NSMutableArray<D3DRetired *> *upload_free;
    NSMutableArray<D3DRetired *> *upload_used;
    id<MTLBuffer> upload;
    uint32_t upload_off;

    /* Built-in objects: (0,0,0,1) texture for unbound samplers, zero vertex
       data for shader inputs that the declaration does not supply, the
       resolve texture of a multisampled back buffer. */
    id<MTLTexture> null_tex2d, null_texcube, null_tex3d;
    id<MTLBuffer> null_vertex;
    id<MTLTexture> present_resolve;
}
@end

/* Render states (D3DRENDERSTATETYPE). */
#define RS_ZENABLE 7u
#define RS_FILLMODE 8u
#define RS_SHADEMODE 9u
#define RS_ZWRITEENABLE 14u
#define RS_ALPHATESTENABLE 15u
#define RS_LASTPIXEL 16u
#define RS_SRCBLEND 19u
#define RS_DESTBLEND 20u
#define RS_CULLMODE 22u
#define RS_ZFUNC 23u
#define RS_ALPHAREF 24u
#define RS_ALPHAFUNC 25u
#define RS_DITHERENABLE 26u
#define RS_ALPHABLENDENABLE 27u
#define RS_FOGENABLE 28u
#define RS_SPECULARENABLE 29u
#define RS_FOGCOLOR 34u
#define RS_FOGTABLEMODE 35u
#define RS_FOGSTART 36u
#define RS_FOGEND 37u
#define RS_FOGDENSITY 38u
#define RS_RANGEFOGENABLE 48u
#define RS_STENCILENABLE 52u
#define RS_STENCILFAIL 53u
#define RS_STENCILZFAIL 54u
#define RS_STENCILPASS 55u
#define RS_STENCILFUNC 56u
#define RS_STENCILREF 57u
#define RS_STENCILMASK 58u
#define RS_STENCILWRITEMASK 59u
#define RS_TEXTUREFACTOR 60u
#define RS_WRAP0 128u
#define RS_CLIPPING 136u
#define RS_LIGHTING 137u
#define RS_AMBIENT 139u
#define RS_FOGVERTEXMODE 140u
#define RS_COLORVERTEX 141u
#define RS_LOCALVIEWER 142u
#define RS_NORMALIZENORMALS 143u
#define RS_DIFFUSEMATERIALSOURCE 145u
#define RS_SPECULARMATERIALSOURCE 146u
#define RS_AMBIENTMATERIALSOURCE 147u
#define RS_EMISSIVEMATERIALSOURCE 148u
#define RS_VERTEXBLEND 151u
#define RS_CLIPPLANEENABLE 152u
#define RS_POINTSIZE 154u
#define RS_POINTSIZE_MIN 155u
#define RS_POINTSPRITEENABLE 156u
#define RS_POINTSCALEENABLE 157u
#define RS_POINTSCALE_A 158u
#define RS_POINTSCALE_B 159u
#define RS_POINTSCALE_C 160u
#define RS_MULTISAMPLEANTIALIAS 161u
#define RS_MULTISAMPLEMASK 162u
#define RS_PATCHEDGESTYLE 163u
#define RS_DEBUGMONITORTOKEN 165u
#define RS_POINTSIZE_MAX 166u
#define RS_INDEXEDVERTEXBLENDENABLE 167u
#define RS_COLORWRITEENABLE 168u
#define RS_TWEENFACTOR 170u
#define RS_BLENDOP 171u
#define RS_POSITIONDEGREE 172u
#define RS_NORMALDEGREE 173u
#define RS_SCISSORTESTENABLE 174u
#define RS_SLOPESCALEDEPTHBIAS 175u
#define RS_ANTIALIASEDLINEENABLE 176u
#define RS_MINTESSELLATIONLEVEL 178u
#define RS_MAXTESSELLATIONLEVEL 179u
#define RS_ADAPTIVETESS_X 180u
#define RS_ADAPTIVETESS_Y 181u
#define RS_ADAPTIVETESS_Z 182u
#define RS_ADAPTIVETESS_W 183u
#define RS_ENABLEADAPTIVETESSELLATION 184u
#define RS_TWOSIDEDSTENCILMODE 185u
#define RS_CCW_STENCILFAIL 186u
#define RS_CCW_STENCILZFAIL 187u
#define RS_CCW_STENCILPASS 188u
#define RS_CCW_STENCILFUNC 189u
#define RS_COLORWRITEENABLE1 190u
#define RS_COLORWRITEENABLE2 191u
#define RS_COLORWRITEENABLE3 192u
#define RS_BLENDFACTOR 193u
#define RS_SRGBWRITEENABLE 194u
#define RS_DEPTHBIAS 195u
#define RS_WRAP8 198u
#define RS_SEPARATEALPHABLENDENABLE 206u
#define RS_SRCBLENDALPHA 207u
#define RS_DESTBLENDALPHA 208u
#define RS_BLENDOPALPHA 209u

/* Sampler states (D3DSAMPLERSTATETYPE). */
#define SS_ADDRESSU 1u
#define SS_ADDRESSV 2u
#define SS_ADDRESSW 3u
#define SS_BORDERCOLOR 4u
#define SS_MAGFILTER 5u
#define SS_MINFILTER 6u
#define SS_MIPFILTER 7u
#define SS_MIPMAPLODBIAS 8u
#define SS_MAXMIPLEVEL 9u
#define SS_MAXANISOTROPY 10u
#define SS_SRGBTEXTURE 11u
#define SS_ELEMENTINDEX 12u
#define SS_DMAPOFFSET 13u

/* Clear flags. */
#define D3DCLEAR_TARGET 1u
#define D3DCLEAR_ZBUFFER 2u
#define D3DCLEAR_STENCIL 4u

/* D3DPRESENT_PARAMETERS dword indices. */
#define PP_WIDTH 0
#define PP_HEIGHT 1
#define PP_FORMAT 2
#define PP_COUNT 3
#define PP_MSAA 4
#define PP_MSAA_QUALITY 5
#define PP_SWAPEFFECT 6
#define PP_HWND 7
#define PP_WINDOWED 8
#define PP_AUTODS 9
#define PP_AUTODS_FORMAT 10
#define PP_FLAGS 11
#define PP_REFRESH 12
#define PP_INTERVAL 13

#define D3DPRESENT_INTERVAL_IMMEDIATE 0x80000000u

static inline float d3d_f(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static inline uint32_t d3d_fbits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

/* ---------------------------------------------------------------- shared functions */

/* The process Metal device (rt_gpu_info.m). */
id<MTLDevice> rt_gpu_device(void);

void d3d_lock(D3DDevice *d);
void d3d_unlock(D3DDevice *d);
/* End the render encoder (if any). Pending full-surface clears are done
   first (a render pass that only clears). */
void d3d_end_pass(D3DDevice *d);
/* Commit the command buffer that is being recorded and start a new one. */
void d3d_flush(D3DDevice *d);
/* Create the pipeline, depth-stencil state and sampler caches. */
void d3d_init_caches(D3DDevice *d);
/* Wait until the command buffer with this serial is complete (it is
   committed first if it is the current one). */
void d3d_wait_serial(D3DDevice *d, uint64_t serial);
bool d3d_serial_done(D3DDevice *d, uint64_t serial);
/* A shared Metal buffer of at least size bytes, reused after completion. */
id<MTLBuffer> d3d_upload(D3DDevice *d, const void *data, uint32_t size, uint32_t *offset);
/* The device of a device COM interface pointer. */
D3DDevice *d3d_device_of(uint32_t this_ptr);

/* Device class (rt_d3d9.m). */
extern const rt_com_class d3d_device_class;

/* Built-in Metal objects of a device: library, present pipeline, null
   textures and vertex data (rt_d3d9_draw.m). */
void d3d_draw_init(D3DDevice *d);
/* Present the back buffer to the window layer and commit the frame. */
void d3d_present(D3DDevice *d);
/* Resource classes (rt_d3d9_res.m). */
extern const rt_com_class d3d_texture_class, d3d_cube_class, d3d_volume_class, d3d_surface_class, d3d_vb_class, d3d_ib_class,
    d3d_decl_class, d3d_vs_class, d3d_ps_class, d3d_query_class;

/* Create resources (device lock held). Each returns D3D_OK and writes the
   new interface pointer to the guest address out. */
uint32_t d3d_create_texture(D3DDevice *d, bool cube, uint32_t w, uint32_t h, uint32_t levels, uint32_t usage,
                            uint32_t fmt, uint32_t pool, uint32_t out);
/* CreateVolumeTexture: a 3D texture of w x h x depth texels. */
uint32_t d3d_create_volume_texture(D3DDevice *d, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels,
                                   uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out);
/* Is o a texture object (2D, cube or volume)? */
bool d3d_is_texture(const rt_com_object *o);
uint32_t d3d_create_surface(D3DDevice *d, uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool,
                            uint32_t msaa, uint32_t quality, bool lockable, rt_com_object **out_obj);
uint32_t d3d_create_buffer(D3DDevice *d, bool index, uint32_t size, uint32_t usage, uint32_t format_or_fvf,
                           uint32_t pool, uint32_t out);
uint32_t d3d_create_decl(D3DDevice *d, const d3d_velem *el, uint32_t n, uint32_t fvf, rt_com_object **out_obj);
uint32_t d3d_create_shader(D3DDevice *d, bool ps, uint32_t function, uint32_t out);
uint32_t d3d_create_query(D3DDevice *d, uint32_t type, uint32_t out);

/* Data of resource COM objects. */
static inline D3DTexture *d3d_tex(rt_com_object *o) { return (__bridge D3DTexture *)o->data; }
static inline D3DSurface *d3d_surf(rt_com_object *o) { return (__bridge D3DSurface *)o->data; }
static inline D3DBuffer *d3d_buf(rt_com_object *o) { return (__bridge D3DBuffer *)o->data; }
static inline D3DDecl *d3d_decl(rt_com_object *o) { return (__bridge D3DDecl *)o->data; }
static inline D3DShader *d3d_shader_obj(rt_com_object *o) { return (__bridge D3DShader *)o->data; }

/* The Metal texture of a surface and its slice/level. */
id<MTLTexture> d3d_surface_texture(D3DSurface *s, NSUInteger *slice, NSUInteger *level);
/* The texture view for sampling (swizzle and sRGB as requested). */
id<MTLTexture> d3d_texture_view(D3DTexture *t, bool srgb);
/* Mark a texture, surface or buffer as used by the current command buffer. */
void d3d_use_surface(D3DSurface *s);
/* A surface object from an interface pointer (stops the process if the
   pointer is not a runtime surface). */
rt_com_object *d3d_surface_from_ptr(uint32_t p);
/* Read the surface pixels into the guest memory of a system-memory
   surface (GetRenderTargetData). */
uint32_t d3d_readback(D3DDevice *d, D3DSurface *src, D3DSurface *dst);
/* Copy a system-memory texture to a default-pool texture (UpdateTexture). */
uint32_t d3d_update_texture(D3DDevice *d, D3DTexture *src, D3DTexture *dst);
/* Copy a rectangle of a system-memory surface to a surface (UpdateSurface). */
uint32_t d3d_update_surface(D3DDevice *d, D3DSurface *src, uint32_t src_rect, D3DSurface *dst, uint32_t dst_point);

/* Retire buffers and textures. */
D3DRetired *d3d_retired_new(id<MTLBuffer> b, id<MTLTexture> t, uint64_t serial);

#endif
