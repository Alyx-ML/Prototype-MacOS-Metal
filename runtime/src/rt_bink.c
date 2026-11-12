/*
 * BINKW32: the Bink 1 video API that the engine uses, on the FFmpeg
 * decoders binkvideo, binkaudio_dct and binkaudio_rdft.
 *
 * The engine supplies all file access (BinkSetIO: a BINKIO reader over its
 * own file system), all sound output (BinkSetSoundSystem: a BINKSND
 * driver), the memory callbacks (BinkSetMemory) and the frame buffers
 * (BinkRegisterFrameBuffers, with BINKNOFRAMEBUFFERS). This module parses
 * the Bink 1 container, reads each frame through the BINKIO callbacks,
 * decodes video into the registered YUV planes and gives decoded 16-bit
 * PCM to the BINKSND callbacks.
 *
 * Structure offsets and callback order come from the engine code
 * (static analysis of the callers and of the engine's BINKIO and BINKSND
 * implementations) and from the RAD Bink 1 SDK header layout. Items that
 * are only recalled from the SDK and not proved by the engine code are
 * marked UNCONFIRMED.
 *
 * All calls for one movie come from one thread (BINKNOTHREADEDIO); the
 * handle table has a lock.
 */
#include "rt.h"
#include "rt_thread.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* BinkOpen flags (RAD bink.h). */
#define BINKNOFRAMEBUFFERS 0x00000400u
#define BINKPRELOADALL 0x00002000u
#define BINKSNDTRACK 0x00004000u
#define BINKNOSKIP 0x00080000u
#define BINKALPHA 0x00100000u
#define BINKNOFILLIOBUF 0x00200000u
#define BINKIOPROCESSOR 0x02000000u
#define BINKNOTHREADEDIO 0x08000000u
#define SUPPORTED_OPEN_FLAGS (BINKNOFRAMEBUFFERS | BINKPRELOADALL | BINKSNDTRACK | BINKNOSKIP | BINKALPHA | \
                              BINKNOFILLIOBUF | BINKIOPROCESSOR | BINKNOTHREADEDIO)

/* Bink 1 header flags. */
#define BINK_VID_ALPHA 0x00100000u
#define BINK_AUD_USEDCT 0x1000u
#define BINK_AUD_STEREO 0x2000u

/* BINKIO callback offsets. */
#define BIO_READHEADER 0x00u
#define BIO_READFRAME 0x04u
#define BIO_GETBUFFERSIZE 0x08u
#define BIO_SETINFO 0x0Cu
#define BIO_IDLE 0x10u
#define BIO_CLOSE 0x14u
#define BIO_SIZE 0x200u /* the engine's open callback clears 0x144 bytes */

/* BINKSND callback offsets. Pause is before SetOnOff in the SDK order
   (UNCONFIRMED); only Pause is called here. */
#define BS_SIZE 0x180u
#define BS_READY 0x158u
#define BS_LOCK 0x15Cu
#define BS_UNLOCK 0x160u
#define BS_VOLUME 0x164u
#define BS_PAUSE 0x16Cu
#define BS_CLOSE 0x174u
#define BS_MIXBINS 0x178u

/* BINK structure. The engine reads Frames (+8) and FrameNum (+0xC);
   the other fields follow the SDK layout (UNCONFIRMED). FrameNum is
   1-based as in the SDK (UNCONFIRMED). */
#define BINK_SIZE 0x400u
#define BK_WIDTH 0x00u
#define BK_HEIGHT 0x04u
#define BK_FRAMES 0x08u
#define BK_FRAMENUM 0x0Cu
#define BK_LASTFRAMENUM 0x10u
#define BK_FRAMERATE 0x14u
#define BK_FRAMERATEDIV 0x18u

/* BINKFRAMEBUFFERS: header of 0x18 bytes, then 2 frames of 4 planes
   (Y, cR, cB, A), each plane {Allocate, Buffer, Pitch}. */
#define FB_FRAMES 0x18u
#define FB_FRAME_SIZE 0x30u
#define FB_PLANE_SIZE 0x0Cu
#define FB_MAX_FRAMES 2u
#define PLANE_Y 0
#define PLANE_CR 1
#define PLANE_CB 2
#define PLANE_A 3

#define HEADER_SIZE 44u
#define MAX_BINKS 8
#define MAX_REQUESTED_TRACKS 16u
#define MAX_FILE_TRACKS 256u
#define MAX_FRAMES 1000000u
#define MAX_DIMENSION 8192u
#define MAX_FRAME_BYTES (64u << 20)
#define AUDIO_RING_SECONDS 4u

typedef struct track {
    uint32_t id, rate, chans, flags;
    bool open;
    uint32_t bs; /* guest BINKSND */
    AVCodecContext *dec;
    int16_t *ring; /* interleaved 16-bit PCM */
    uint32_t ring_cap, ring_head, ring_len; /* in samples (int16 values) */
    bool overflow_logged;
} track;

typedef struct plane {
    uint32_t buffer;
    int32_t pitch;
} plane;

typedef struct bink {
    bool used;
    uint32_t g, bio, iobuf, framebuf, lockout;
    uint32_t mem_alloc, mem_free;
    uint32_t flags, width, height, frames, fps_num, fps_den, vflags, largest;
    uint32_t *index; /* frames + 1 file offsets; bit 0 = keyframe */
    uint32_t ntracks;
    track *tracks;
    AVCodecContext *vdec;
    AVPacket *pkt;
    AVFrame *frm;
    uint8_t *hostbuf; /* largest + AV_INPUT_BUFFER_PADDING_SIZE */
    bool alpha;
    bool registered;
    plane planes[4];
    uint32_t frame_num, decoded;
    bool paused;
    uint64_t t0_ns, pause_ns;
} bink;

static pthread_mutex_t bk_lock = PTHREAD_MUTEX_INITIALIZER;
static bink binks[MAX_BINKS];
static uint32_t cfg_mem_alloc, cfg_mem_free, cfg_io_open, cfg_snd_open;
static uint32_t cfg_track_ids[MAX_REQUESTED_TRACKS], cfg_track_count;

static void blk(void)
{
    if (pthread_mutex_lock(&bk_lock) != 0) rt_fatal("BINK lock failed");
}

static void bunlk(void)
{
    if (pthread_mutex_unlock(&bk_lock) != 0) rt_fatal("BINK unlock failed");
}

static uint64_t now_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static const char *averr(int e, char *buf, size_t n)
{
    if (av_strerror(e, buf, n) < 0) snprintf(buf, n, "error %d", e);
    return buf;
}

/* ---------------------------------------------------------------- guest memory */

/* Bink allocates through the BinkSetMemory callbacks; without them, from
   the process heap (the guest heap here), as the SDK does. */
static uint32_t galloc(_cpu *cpu, const bink *b, uint32_t size)
{
    uint32_t p = b->mem_alloc ? rt_call_std(cpu, b->mem_alloc, 1, size) : rt_heap_alloc(size);
    if (p != 0) memset(G2H(p), 0, size);
    return p;
}

static void gfree(_cpu *cpu, const bink *b, uint32_t p)
{
    if (p == 0) return;
    if (b->mem_free)
        rt_call_std(cpu, b->mem_free, 1, p);
    else
        rt_heap_free(p);
}

/* ---------------------------------------------------------------- handles */

static bink *lookup(uint32_t g, const char *fn)
{
    bink *b = NULL;
    int i;
    blk();
    for (i = 0; i < MAX_BINKS; i++)
    {
        if (binks[i].used && binks[i].g == g)
        {
            b = &binks[i];
            break;
        }
    }
    bunlk();
    if (b == NULL) rt_fatal("%s: unknown Bink handle 0x%08x", fn, g);
    return b;
}

static track *find_track(bink *b, uint32_t id)
{
    uint32_t i;
    for (i = 0; i < b->ntracks; i++)
        if (b->tracks[i].open && b->tracks[i].id == id) return &b->tracks[i];
    return NULL;
}

static void write_guest_state(const bink *b, uint32_t last)
{
    wr32(b->g + BK_FRAMENUM, b->frame_num);
    wr32(b->g + BK_LASTFRAMENUM, last);
}

/* Release everything that the handle owns. The BINKSND and BINKIO close
   callbacks run only when their open callbacks succeeded. */
static void destroy(_cpu *cpu, bink *b, bool io_open)
{
    uint32_t i;
    if (b->tracks != NULL)
    {
        for (i = 0; i < b->ntracks; i++)
        {
            track *t = &b->tracks[i];
            if (t->bs != 0)
            {
                if (t->open) rt_call_std(cpu, rd32(t->bs + BS_CLOSE), 1, t->bs);
                gfree(cpu, b, t->bs);
            }
            avcodec_free_context(&t->dec);
            free(t->ring);
        }
        free(b->tracks);
    }
    if (io_open) rt_call_std(cpu, rd32(b->bio + BIO_CLOSE), 1, b->bio);
    avcodec_free_context(&b->vdec);
    av_packet_free(&b->pkt);
    av_frame_free(&b->frm);
    free(b->hostbuf);
    free(b->index);
    gfree(cpu, b, b->framebuf);
    gfree(cpu, b, b->iobuf);
    gfree(cpu, b, b->lockout);
    gfree(cpu, b, b->bio);
    gfree(cpu, b, b->g);
    blk();
    memset(b, 0, sizeof *b);
    bunlk();
}

/* ---------------------------------------------------------------- decoders */

static AVCodecContext *open_decoder(enum AVCodecID id, uint32_t tag, uint32_t extra, int width, int height,
                                    int rate, int chans)
{
    char eb[128];
    const AVCodec *c = avcodec_find_decoder(id);
    AVCodecContext *ctx;
    int r;
    if (c == NULL) rt_fatal("BinkOpen: FFmpeg has no decoder for codec %d", (int)id);
    ctx = avcodec_alloc_context3(c);
    if (ctx == NULL) rt_fatal("BinkOpen: out of host memory (decoder context)");
    /* binkvideo reads the header video flags from extradata; binkaudio
       reads the format version from extradata[3]. Both take the version
       from the file signature (UNCONFIRMED: FFmpeg demuxer convention). */
    ctx->extradata = av_mallocz(4 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (ctx->extradata == NULL) rt_fatal("BinkOpen: out of host memory (extradata)");
    memcpy(ctx->extradata, &extra, 4);
    ctx->extradata_size = 4;
    ctx->codec_tag = tag;
    if (width > 0)
    {
        ctx->width = width;
        ctx->height = height;
    }
    if (rate > 0)
    {
        ctx->sample_rate = rate;
        av_channel_layout_default(&ctx->ch_layout, chans);
    }
    r = avcodec_open2(ctx, c, NULL);
    if (r < 0) rt_fatal("BinkOpen: opening the FFmpeg %s decoder failed: %s", c->name, averr(r, eb, sizeof eb));
    return ctx;
}

static void ring_push(track *t, const int16_t *s, uint32_t n)
{
    uint32_t space = t->ring_cap - t->ring_len, tail, first;
    if (n > space)
    {
        /* The sound driver did not take the audio in time. Bink also drops
           audio in this case (SoundDroppedOut in the SDK). */
        if (!t->overflow_logged)
        {
            rt_log("Bink: sound track %u: audio buffer full, %u samples dropped", t->id, n - space);
            t->overflow_logged = true;
        }
        n = space;
    }
    tail = (t->ring_head + t->ring_len) % t->ring_cap;
    first = t->ring_cap - tail;
    if (first > n) first = n;
    memcpy(t->ring + tail, s, (size_t)first * 2u);
    memcpy(t->ring, s + first, (size_t)(n - first) * 2u);
    t->ring_len += n;
}

static int16_t to_s16(float v)
{
    float x = v * 32767.0f;
    if (x > 32767.0f) return 32767;
    if (x < -32768.0f) return -32768;
    return (int16_t)x;
}

/* Convert one decoded audio frame to interleaved 16-bit PCM and queue it. */
static void queue_audio(track *t, const AVFrame *f)
{
    int16_t buf[2048];
    uint32_t ch = t->chans, ns, i, c, done = 0, per = (uint32_t)(sizeof buf / sizeof buf[0]) / ch;
    if (f->nb_samples < 0) rt_fatal("Bink: decoder gave a negative sample count");
    ns = (uint32_t)f->nb_samples;
    if ((uint32_t)f->ch_layout.nb_channels != ch)
        rt_fatal("Bink: decoder gave %d channels for a %u-channel track", f->ch_layout.nb_channels, ch);
    while (done < ns)
    {
        uint32_t n = ns - done, k = 0;
        if (n > per) n = per;
        for (i = done; i < done + n; i++)
        {
            for (c = 0; c < ch; c++)
            {
                int16_t v;
                switch (f->format)
                {
                case AV_SAMPLE_FMT_FLTP: v = to_s16(((const float *)f->extended_data[c])[i]); break;
                case AV_SAMPLE_FMT_FLT: v = to_s16(((const float *)f->extended_data[0])[i * ch + c]); break;
                case AV_SAMPLE_FMT_S16P: v = ((const int16_t *)f->extended_data[c])[i]; break;
                case AV_SAMPLE_FMT_S16: v = ((const int16_t *)f->extended_data[0])[i * ch + c]; break;
                default: rt_fatal("Bink: unsupported decoded sample format %d", f->format);
                }
                buf[k++] = v;
            }
        }
        ring_push(t, buf, k);
        done += n;
    }
}

static void decode_audio(bink *b, track *t, uint8_t *data, uint32_t size)
{
    char eb[128];
    int r;
    b->pkt->data = data;
    b->pkt->size = (int)size;
    r = avcodec_send_packet(t->dec, b->pkt);
    b->pkt->data = NULL;
    b->pkt->size = 0;
    if (r < 0) rt_fatal("Bink: audio decode of track %u failed: %s", t->id, averr(r, eb, sizeof eb));
    for (;;)
    {
        r = avcodec_receive_frame(t->dec, b->frm);
        if (r == AVERROR(EAGAIN)) break;
        if (r < 0) rt_fatal("Bink: audio decode of track %u failed: %s", t->id, averr(r, eb, sizeof eb));
        queue_audio(t, b->frm);
        av_frame_unref(b->frm);
    }
}

static void copy_plane(uint32_t dst, int32_t pitch, const uint8_t *src, int src_pitch, uint32_t w, uint32_t h)
{
    uint32_t y;
    for (y = 0; y < h; y++)
    {
        uint32_t row = dst + (uint32_t)((int64_t)y * pitch);
        memcpy(G2H(row), src + (ptrdiff_t)y * src_pitch, w);
    }
}

static void decode_video(bink *b, uint8_t *data, uint32_t size, bool key)
{
    char eb[128];
    AVFrame *f = b->frm;
    uint32_t cw = (b->width + 1) / 2, chh = (b->height + 1) / 2;
    int r;
    b->pkt->data = data;
    b->pkt->size = (int)size;
    b->pkt->flags = key ? AV_PKT_FLAG_KEY : 0;
    r = avcodec_send_packet(b->vdec, b->pkt);
    b->pkt->data = NULL;
    b->pkt->size = 0;
    b->pkt->flags = 0;
    if (r < 0) rt_fatal("Bink: video decode failed: %s", averr(r, eb, sizeof eb));
    r = avcodec_receive_frame(b->vdec, f);
    if (r < 0) rt_fatal("Bink: video decode gave no picture: %s", averr(r, eb, sizeof eb));
    if (f->width != (int)b->width || f->height != (int)b->height)
        rt_fatal("Bink: decoded picture is %dx%d, movie is %ux%u", f->width, f->height, b->width, b->height);
    if (f->format != AV_PIX_FMT_YUV420P && f->format != AV_PIX_FMT_YUVA420P)
        rt_fatal("Bink: unsupported decoded pixel format %d", f->format);
    if (b->registered)
    {
        copy_plane(b->planes[PLANE_Y].buffer, b->planes[PLANE_Y].pitch, f->data[0], f->linesize[0], b->width,
                   b->height);
        copy_plane(b->planes[PLANE_CB].buffer, b->planes[PLANE_CB].pitch, f->data[1], f->linesize[1], cw, chh);
        copy_plane(b->planes[PLANE_CR].buffer, b->planes[PLANE_CR].pitch, f->data[2], f->linesize[2], cw, chh);
        if (b->planes[PLANE_A].buffer != 0)
        {
            if (f->format != AV_PIX_FMT_YUVA420P) rt_fatal("Bink: alpha plane registered but movie has no alpha");
            copy_plane(b->planes[PLANE_A].buffer, b->planes[PLANE_A].pitch, f->data[3], f->linesize[3], b->width,
                       b->height);
        }
    }
    av_frame_unref(f);
}

/* Read frame n (1-based) through BINKIO and decode its audio and video. */
static void decode_frame(_cpu *cpu, bink *b, uint32_t n)
{
    uint32_t off = b->index[n - 1] & ~1u, end = b->index[n] & ~1u, size = end - off, got, p = 0, i;
    bool key = (b->index[n - 1] & 1u) != 0;
    got = rt_call_std(cpu, rd32(b->bio + BIO_READFRAME), 5, b->bio, n, off, b->framebuf, size);
    if (got != size) rt_fatal("Bink: reading frame %u failed (%u of %u bytes)", n, got, size);
    memcpy(b->hostbuf, G2H(b->framebuf), size);
    memset(b->hostbuf + size, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    for (i = 0; i < b->ntracks; i++)
    {
        uint32_t asz;
        if (size - p < 4) rt_fatal("Bink: frame %u is truncated (audio track %u)", n, i);
        memcpy(&asz, b->hostbuf + p, 4);
        p += 4;
        if (asz > size - p) rt_fatal("Bink: frame %u audio track %u size %u is too large", n, i, asz);
        if (b->tracks[i].open && asz >= 4) decode_audio(b, &b->tracks[i], b->hostbuf + p, asz);
        p += asz;
    }
    decode_video(b, b->hostbuf + p, size - p, key);
}

/* ---------------------------------------------------------------- playback helpers */

static uint64_t due_ns(const bink *b, uint32_t n)
{
    unsigned __int128 t = (unsigned __int128)(n - 1u) * b->fps_den * 1000000000u / b->fps_num;
    return b->t0_ns + (uint64_t)t;
}

/* Give queued PCM to the engine's sound driver while it wants more. */
static void feed_audio(_cpu *cpu, bink *b)
{
    uint32_t i;
    for (i = 0; i < b->ntracks; i++)
    {
        track *t = &b->tracks[i];
        if (!t->open) continue;
        while (t->ring_len > 0)
        {
            uint32_t addr, len, frame_bytes = 2u * t->chans, n, first, half;
            if (rt_call_std(cpu, rd32(t->bs + BS_READY), 1, t->bs) == 0) break;
            rt_call_std(cpu, rd32(t->bs + BS_LOCK), 3, t->bs, b->lockout, b->lockout + 4u);
            addr = rd32(b->lockout);
            len = rd32(b->lockout + 4u);
            n = len / frame_bytes * frame_bytes;
            if (n > t->ring_len * 2u) n = t->ring_len * 2u;
            half = n / 2u;
            first = t->ring_cap - t->ring_head;
            if (first > half) first = half;
            if (n > 0)
            {
                memcpy(G2H(addr), t->ring + t->ring_head, (size_t)first * 2u);
                memcpy(G2H(addr + first * 2u), t->ring, (size_t)(half - first) * 2u);
            }
            t->ring_head = (t->ring_head + half) % t->ring_cap;
            t->ring_len -= half;
            rt_call_std(cpu, rd32(t->bs + BS_UNLOCK), 2, t->bs, n);
            if (n == 0) break;
        }
    }
}

static void idle_io(_cpu *cpu, bink *b)
{
    rt_call_std(cpu, rd32(b->bio + BIO_IDLE), 1, b->bio);
}

/* ---------------------------------------------------------------- configuration imports */

/* void BinkSetMemory(BINKMEMALLOC alloc, BINKMEMFREE free) */
void rt_BinkSetMemory(_cpu *cpu)
{
    blk();
    cfg_mem_alloc = ARG(0);
    cfg_mem_free = ARG(1);
    bunlk();
}

/* void BinkSetIO(BINKIOOPEN open) */
void rt_BinkSetIO(_cpu *cpu)
{
    blk();
    cfg_io_open = ARG(0);
    bunlk();
}

/* S32 BinkSetSoundSystem(BINKSNDSYSOPEN open, UINTa param): the system
   open function returns the BINKSNDOPEN function. */
void rt_BinkSetSoundSystem(_cpu *cpu)
{
    uint32_t open = ARG(0), param = ARG(1), snd = 0;
    if (open != 0) snd = rt_call_std(cpu, open, 1, param);
    blk();
    cfg_snd_open = snd;
    bunlk();
    RET(snd != 0);
}

/* void BinkSetSoundTrack(U32 total, U32 *track_ids): the tracks that the
   next BinkOpen with BINKSNDTRACK plays. */
void rt_BinkSetSoundTrack(_cpu *cpu)
{
    uint32_t total = ARG(0), ids = ARG(1), i;
    if (total > MAX_REQUESTED_TRACKS) rt_fatal("BinkSetSoundTrack: %u tracks (at most %u)", total, MAX_REQUESTED_TRACKS);
    if (total > 0 && ids == 0) rt_fatal("BinkSetSoundTrack: NULL track list");
    blk();
    for (i = 0; i < total; i++)
        cfg_track_ids[i] = rd32(ids + 4u * i);
    cfg_track_count = total;
    bunlk();
}

/* U32 RADTimerRead(void): milliseconds. */
void rt_RADTimerRead(_cpu *cpu)
{
    RET((uint32_t)(now_ns() / 1000000u));
}

/* ---------------------------------------------------------------- BinkOpen */

static bool read_header(_cpu *cpu, bink *b, int32_t offset, uint32_t dest, uint32_t size)
{
    uint32_t got = rt_call_std(cpu, rd32(b->bio + BIO_READHEADER), 4, b->bio, (uint32_t)offset, dest, size);
    return got == size;
}

/* Parse and check the Bink 1 header. Returns false (with a log line) for a
   file that is not a valid Bink 1 movie. */
static bool parse(_cpu *cpu, bink *b, uint32_t name, uint32_t *sig_out)
{
    char nm[260];
    uint32_t hdr, sig, rest, rest_size, i, file_size;
    uint8_t ver;
    if (rt_gstr(nm, sizeof nm, name) != 0) snprintf(nm, sizeof nm, "(long name)");
    hdr = galloc(cpu, b, HEADER_SIZE);
    if (hdr == 0) rt_fatal("BinkOpen: out of guest memory (header)");
    if (!read_header(cpu, b, 0, hdr, HEADER_SIZE))
    {
        rt_log("BinkOpen: %s: header read failed", nm);
        gfree(cpu, b, hdr);
        return false;
    }
    sig = rd32(hdr);
    ver = (uint8_t)(sig >> 24);
    if ((sig & 0x00FFFFFFu) != 0x004B4942u /* "BIK" */)
        rt_fatal("BinkOpen: %s is not a Bink 1 movie (signature 0x%08x); only Bink 1 is supported", nm, sig);
    if (ver < 'b' || ver > 'k') rt_fatal("BinkOpen: %s: unsupported Bink 1 version '%c'", nm, ver);
    file_size = rd32(hdr + 4u);
    b->frames = rd32(hdr + 8u);
    b->largest = rd32(hdr + 12u);
    b->width = rd32(hdr + 20u);
    b->height = rd32(hdr + 24u);
    b->fps_num = rd32(hdr + 28u);
    b->fps_den = rd32(hdr + 32u);
    b->vflags = rd32(hdr + 36u);
    b->ntracks = rd32(hdr + 40u);
    gfree(cpu, b, hdr);
    if (file_size > UINT32_MAX - 8u || b->frames == 0 || b->frames > MAX_FRAMES || b->width == 0 ||
        b->width > MAX_DIMENSION || b->height == 0 || b->height > MAX_DIMENSION || b->fps_num == 0 ||
        b->fps_den == 0 || b->ntracks > MAX_FILE_TRACKS || b->largest == 0 || b->largest > MAX_FRAME_BYTES)
    {
        rt_log("BinkOpen: %s: invalid header (%u frames, %ux%u, %u/%u fps, %u tracks, largest frame %u)", nm,
               b->frames, b->width, b->height, b->fps_num, b->fps_den, b->ntracks, b->largest);
        return false;
    }
    file_size += 8u;
    /* Per track: max decoded size (u32), rate and flags (2 x u16), id (u32);
       then frames + 1 frame offsets. */
    rest_size = 12u * b->ntracks + 4u * (b->frames + 1u);
    rest = galloc(cpu, b, rest_size);
    if (rest == 0) rt_fatal("BinkOpen: out of guest memory (frame index)");
    if (!read_header(cpu, b, -1, rest, rest_size))
    {
        rt_log("BinkOpen: %s: header read failed", nm);
        gfree(cpu, b, rest);
        return false;
    }
    if (b->ntracks > 0)
    {
        b->tracks = calloc(b->ntracks, sizeof *b->tracks);
        if (b->tracks == NULL) rt_fatal("BinkOpen: out of host memory (tracks)");
    }
    for (i = 0; i < b->ntracks; i++)
    {
        track *t = &b->tracks[i];
        t->rate = rd16(rest + 4u * b->ntracks + 4u * i);
        t->flags = rd16(rest + 4u * b->ntracks + 4u * i + 2u);
        t->id = rd32(rest + 8u * b->ntracks + 4u * i);
        t->chans = (t->flags & BINK_AUD_STEREO) ? 2u : 1u;
    }
    b->index = malloc(sizeof(uint32_t) * (b->frames + 1u));
    if (b->index == NULL) rt_fatal("BinkOpen: out of host memory (frame index)");
    for (i = 0; i <= b->frames; i++)
        b->index[i] = rd32(rest + 12u * b->ntracks + 4u * i);
    gfree(cpu, b, rest);
    for (i = 0; i < b->frames; i++)
    {
        uint32_t a = b->index[i] & ~1u, e = b->index[i + 1] & ~1u;
        if (e <= a || e - a > b->largest || e > file_size)
        {
            rt_log("BinkOpen: %s: invalid frame index at frame %u", nm, i + 1u);
            return false;
        }
    }
    *sig_out = sig;
    (void)file_size;
    return true;
}

/* HBINK BinkOpen(const char *name, U32 flags) */
void rt_BinkOpen(_cpu *cpu)
{
    uint32_t name = ARG(0), flags = ARG(1), sig = 0, io_open, snd_open, req_ids[MAX_REQUESTED_TRACKS], req_count;
    uint32_t want, bufsize, file_size, i;
    bink *b = NULL;
    int slot;
    if (flags & ~SUPPORTED_OPEN_FLAGS) rt_fatal("BinkOpen: flags 0x%08x are not supported", flags & ~SUPPORTED_OPEN_FLAGS);
    if (!(flags & BINKIOPROCESSOR)) rt_fatal("BinkOpen: only BINKIOPROCESSOR (BinkSetIO) file access is supported");
    if (!(flags & BINKNOFRAMEBUFFERS)) rt_fatal("BinkOpen: only BINKNOFRAMEBUFFERS (application frame buffers) is supported");
    blk();
    for (slot = 0; slot < MAX_BINKS; slot++)
        if (!binks[slot].used) break;
    if (slot == MAX_BINKS)
    {
        bunlk();
        rt_fatal("BinkOpen: more than %d movies open", MAX_BINKS);
    }
    b = &binks[slot];
    memset(b, 0, sizeof *b);
    b->used = true;
    b->mem_alloc = cfg_mem_alloc;
    b->mem_free = cfg_mem_free;
    io_open = cfg_io_open;
    snd_open = cfg_snd_open;
    req_count = cfg_track_count;
    memcpy(req_ids, cfg_track_ids, sizeof req_ids);
    bunlk();
    if (io_open == 0) rt_fatal("BinkOpen: BINKIOPROCESSOR without BinkSetIO");
    b->flags = flags;
    b->g = galloc(cpu, b, BINK_SIZE);
    b->bio = galloc(cpu, b, BIO_SIZE);
    b->lockout = galloc(cpu, b, 8);
    if (b->g == 0 || b->bio == 0 || b->lockout == 0) rt_fatal("BinkOpen: out of guest memory");
    if (rt_call_std(cpu, io_open, 3, b->bio, name, flags) == 0)
    {
        destroy(cpu, b, false);
        RET(0);
        return;
    }
    if (!parse(cpu, b, name, &sig))
    {
        destroy(cpu, b, true);
        RET(0);
        return;
    }
    file_size = (b->index[b->frames] & ~1u);
    /* The I/O ring buffer. The SDK sizes it from the data rate; four
       largest frames (at least 1 MiB) keep several frames ahead. */
    want = (flags & BINKPRELOADALL) ? file_size : (b->largest > (1u << 20) / 4u ? b->largest * 4u : 1u << 20);
    if (want > file_size) want = file_size;
    bufsize = rt_call_std(cpu, rd32(b->bio + BIO_GETBUFFERSIZE), 2, b->bio, want);
    if (bufsize == 0) rt_fatal("BinkOpen: BINKIO GetBufferSize returned 0");
    b->iobuf = galloc(cpu, b, bufsize);
    if (b->iobuf == 0) rt_fatal("BinkOpen: out of guest memory (I/O buffer of %u bytes)", bufsize);
    rt_call_std(cpu, rd32(b->bio + BIO_SETINFO), 5, b->bio, b->iobuf, bufsize, file_size, 0);
    b->framebuf = galloc(cpu, b, (b->largest + 15u) & ~15u);
    b->hostbuf = malloc((size_t)b->largest + AV_INPUT_BUFFER_PADDING_SIZE);
    b->pkt = av_packet_alloc();
    b->frm = av_frame_alloc();
    if (b->framebuf == 0 || b->hostbuf == NULL || b->pkt == NULL || b->frm == NULL)
        rt_fatal("BinkOpen: out of memory (frame buffers)");
    b->alpha = (b->vflags & BINK_VID_ALPHA) != 0;
    b->vdec = open_decoder(AV_CODEC_ID_BINKVIDEO, sig, b->vflags, (int)b->width, (int)b->height, 0, 0);

    /* Sound tracks: the BinkSetSoundTrack list with BINKSNDTRACK, else the
       first track of the file (UNCONFIRMED SDK default). An id that is not
       in the file is not played, as in the SDK. */
    for (i = 0; i < b->ntracks; i++)
    {
        track *t = &b->tracks[i];
        uint32_t k;
        if (flags & BINKSNDTRACK)
        {
            for (k = 0; k < req_count; k++)
                if (req_ids[k] == t->id) t->open = true;
        }
        else
            t->open = (i == 0);
        if (!t->open) continue;
        if (snd_open == 0) rt_fatal("BinkOpen: movie has sound but no sound system is set (BinkSetSoundSystem)");
        if (t->rate == 0) rt_fatal("BinkOpen: sound track %u has sample rate 0", t->id);
        t->dec = open_decoder((t->flags & BINK_AUD_USEDCT) ? AV_CODEC_ID_BINKAUDIO_DCT : AV_CODEC_ID_BINKAUDIO_RDFT,
                              sig, sig, 0, 0, (int)t->rate, (int)t->chans);
        t->ring_cap = t->rate * t->chans * AUDIO_RING_SECONDS;
        t->ring = malloc((size_t)t->ring_cap * sizeof(int16_t));
        if (t->ring == NULL) rt_fatal("BinkOpen: out of host memory (audio buffer)");
        t->bs = galloc(cpu, b, BS_SIZE);
        if (t->bs == 0) rt_fatal("BinkOpen: out of guest memory (sound)");
        t->open = false; /* until the driver open succeeds */
        if (rt_call_std(cpu, snd_open, 6, t->bs, t->rate, 16, t->chans, 0, b->g) == 0)
            rt_fatal("BinkOpen: the sound driver could not open track %u (%u Hz, %u channels)", t->id, t->rate,
                     t->chans);
        t->open = true;
    }

    wr32(b->g + BK_WIDTH, b->width);
    wr32(b->g + BK_HEIGHT, b->height);
    wr32(b->g + BK_FRAMES, b->frames);
    wr32(b->g + BK_FRAMERATE, b->fps_num);
    wr32(b->g + BK_FRAMERATEDIV, b->fps_den);
    b->frame_num = 1;
    b->decoded = 0;
    write_guest_state(b, 0);
    b->t0_ns = now_ns();
    RET(b->g);
}

/* ---------------------------------------------------------------- frame buffers */

/* void BinkGetFrameBuffersInfo(HBINK, BINKFRAMEBUFFERS *) */
void rt_BinkGetFrameBuffersInfo(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkGetFrameBuffersInfo");
    uint32_t fb = ARG(1), f0 = fb + FB_FRAMES;
    if (fb == 0) rt_fatal("BinkGetFrameBuffersInfo: NULL buffer");
    memset(G2H(fb), 0, FB_FRAMES + FB_MAX_FRAMES * FB_FRAME_SIZE);
    /* One frame set: the decoder keeps its reference frame itself, so the
       application never has to select a buffer (FrameNum stays 0). */
    wr32(fb + 0x00u, 1);
    wr32(fb + 0x04u, b->width);
    wr32(fb + 0x08u, b->height);
    wr32(fb + 0x0Cu, (b->width + 1u) / 2u);
    wr32(fb + 0x10u, (b->height + 1u) / 2u);
    wr32(fb + 0x14u, 0);
    wr32(f0 + PLANE_Y * FB_PLANE_SIZE, 1);
    wr32(f0 + PLANE_CR * FB_PLANE_SIZE, 1);
    wr32(f0 + PLANE_CB * FB_PLANE_SIZE, 1);
    wr32(f0 + PLANE_A * FB_PLANE_SIZE, (b->alpha && (b->flags & BINKALPHA)) ? 1u : 0u);
}

/* void BinkRegisterFrameBuffers(HBINK, BINKFRAMEBUFFERS *) */
void rt_BinkRegisterFrameBuffers(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkRegisterFrameBuffers");
    uint32_t fb = ARG(1), f0 = fb + FB_FRAMES, p;
    if (fb == 0) rt_fatal("BinkRegisterFrameBuffers: NULL buffer");
    if (rd32(fb) != 1) rt_fatal("BinkRegisterFrameBuffers: %u frame sets (1 was reported)", rd32(fb));
    for (p = 0; p < 4; p++)
    {
        uint32_t a = f0 + p * FB_PLANE_SIZE, w = (p == PLANE_Y || p == PLANE_A) ? b->width : (b->width + 1u) / 2u;
        int32_t pitch = (int32_t)rd32(a + 8u);
        b->planes[p].buffer = rd32(a + 4u);
        b->planes[p].pitch = pitch;
        if (p == PLANE_A && b->planes[p].buffer == 0) continue;
        if (b->planes[p].buffer == 0) rt_fatal("BinkRegisterFrameBuffers: plane %u has no buffer", p);
        if ((pitch < 0 ? -(int64_t)pitch : (int64_t)pitch) < (int64_t)w)
            rt_fatal("BinkRegisterFrameBuffers: plane %u pitch %d is less than its width %u", p, pitch, w);
    }
    b->registered = true;
}

/* ---------------------------------------------------------------- playback */

/* S32 BinkDoFrame(HBINK): decodes the current frame. Returns nonzero when
   playback is behind, so that the caller skips the next frame (the engine
   loop uses it so; inferred from the engine code). */
void rt_BinkDoFrame(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkDoFrame");
    uint32_t n;
    bool behind;
    idle_io(cpu, b);
    if (b->frame_num != b->decoded)
    {
        n = b->decoded + 1u;
        if (b->frame_num < n)
        {
            /* Back to an earlier frame (loop): restart from frame 1. */
            uint32_t i;
            avcodec_flush_buffers(b->vdec);
            for (i = 0; i < b->ntracks; i++)
                if (b->tracks[i].open) avcodec_flush_buffers(b->tracks[i].dec);
            n = 1;
        }
        for (; n <= b->frame_num; n++)
            decode_frame(cpu, b, n);
        b->decoded = b->frame_num;
    }
    feed_audio(cpu, b);
    behind = !(b->flags & BINKNOSKIP) && !b->paused && b->frame_num < b->frames &&
             now_ns() > due_ns(b, b->frame_num + 1u);
    RET(behind ? 1u : 0u);
}

/* void BinkNextFrame(HBINK): after the last frame, back to frame 1. */
void rt_BinkNextFrame(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkNextFrame");
    uint32_t last = b->frame_num;
    if (b->frame_num >= b->frames)
    {
        b->frame_num = 1;
        b->t0_ns = b->paused ? b->pause_ns : now_ns();
    }
    else
        b->frame_num++;
    write_guest_state(b, last);
}

/* S32 BinkWait(HBINK): nonzero while it is not yet time for the current
   frame (and while paused). */
void rt_BinkWait(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkWait");
    idle_io(cpu, b);
    feed_audio(cpu, b);
    if (b->paused || now_ns() < due_ns(b, b->frame_num))
        RET(1);
    else
        RET(0);
}

/* S32 BinkPause(HBINK, S32 pause): returns the new pause state. */
void rt_BinkPause(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkPause");
    bool pause = ARG(1) != 0;
    uint32_t i;
    if (pause != b->paused)
    {
        uint64_t t = now_ns();
        if (pause)
            b->pause_ns = t;
        else
            b->t0_ns += t - b->pause_ns;
        b->paused = pause;
        for (i = 0; i < b->ntracks; i++)
        {
            track *tr = &b->tracks[i];
            if (tr->open) rt_call_std(cpu, rd32(tr->bs + BS_PAUSE), 2, tr->bs, pause ? 1u : 0u);
        }
    }
    RET(b->paused ? 1u : 0u);
}

/* void BinkSetVolume(HBINK, U32 trackid, S32 volume) */
void rt_BinkSetVolume(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkSetVolume");
    track *t = find_track(b, ARG(1));
    if (t != NULL) rt_call_std(cpu, rd32(t->bs + BS_VOLUME), 2, t->bs, ARG(2));
}

/* void BinkSetMixBins(HBINK, U32 trackid, U32 *bins, U32 total) */
void rt_BinkSetMixBins(_cpu *cpu)
{
    bink *b = lookup(ARG(0), "BinkSetMixBins");
    track *t = find_track(b, ARG(1));
    if (t != NULL) rt_call_std(cpu, rd32(t->bs + BS_MIXBINS), 3, t->bs, ARG(2), ARG(3));
}

/* void BinkClose(HBINK) */
void rt_BinkClose(_cpu *cpu)
{
    uint32_t g = ARG(0);
    if (g == 0) return;
    destroy(cpu, lookup(g, "BinkClose"), true);
}
