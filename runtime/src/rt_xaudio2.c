/*
 * XAudio2 2.3 (DirectX SDK of March 2009) on a CoreAudio output unit.
 *
 * The engine creates the object with CoCreateInstance(CLSID_XAudio2 2.3,
 * IID_IXAudio2) and calls:
 *   IXAudio2::Initialize(0, XAUDIO2_ANY_PROCESSOR)
 *   IXAudio2::GetDeviceCount, GetDeviceDetails(0)
 *   IXAudio2::CreateMasteringVoice(0 channels, 0 rate, 0, device 0, NULL)
 *   IXAudio2::CreateSourceVoice(16-bit PCM in the device format,
 *       flags 0, MaxFrequencyRatio 1.0, callback, NULL, NULL)
 *   IXAudio2SourceVoice::SubmitSourceBuffer, Start, Stop, DestroyVoice
 *   IXAudio2MasteringVoice::DestroyVoice, IXAudio2::Release
 * The engine mixes its own sound into a ring of buffers and refills a
 * buffer when the voice callback OnBufferEnd comes.
 *
 * Structure:
 * - The device is the default CoreAudio output device. GetDeviceCount is 1
 *   (0 if the system has no output device). The device details give the
 *   device UID, the device name, the role "global default device" and a
 *   16-bit PCM WAVEFORMATEXTENSIBLE with the device channel count and
 *   nominal sample rate.
 * - The mastering voice opens a default output audio unit (it follows the
 *   default device) with 32-bit float samples at the mastering voice
 *   channel count and rate.
 * - A processing thread with a guest thread state runs 10 ms passes. A
 *   pass calls OnVoiceProcessingPassStart for each running source voice,
 *   reads and resamples (4-point Hermite) the queued buffers, mixes them
 *   into the mastering voice, calls the buffer, loop and stream callbacks,
 *   calls OnVoiceProcessingPassEnd, and writes the pass into a ring of 4
 *   passes. The audio unit render callback reads the ring; when the ring
 *   is empty it plays silence and counts the missing frames.
 * - The XAudio2 lock is not held during a guest callback. The callbacks
 *   of a pass come after the mixing of the pass: a buffer submitted in a
 *   callback plays from the next pass.
 *
 * Not supported (the call stops the process with a message): submix
 * voices, effects (XAPO), filters, volumes and matrices other than the
 * defaults, a source channel count other than the mastering voice
 * channel count, frequency ratios other than 1.0, xWMA and ADPCM data,
 * deferred operation sets, engine callbacks, voice flags, and the debug
 * engine. The engine does not call these methods.
 *
 * UNCONFIRMED (not compared with Windows): the IXAudio2 2.3 method order
 * and argument counts (taken from the engine's calls and the 2.3
 * headers as remembered), the XAUDIO2_BUFFER validation rules, the order
 * OnBufferEnd before OnStreamEnd, the value of BytesRequired, and the
 * OutputFormat that Windows reports (the engine changes only the sample
 * size, so the runtime reports PCM).
 */
#include "rt.h"
#include "rt_com.h"
#include "rt_com_thunks.h"
#include "rt_thread.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define XAUDIO2_E_INVALID_CALL 0x88960001u
#define XAUDIO2_E_DEVICE_INVALIDATED 0x88960004u

#define XAUDIO2_COMMIT_NOW 0u
#define XAUDIO2_PLAY_TAILS 0x20u
#define XAUDIO2_END_OF_STREAM 0x40u
#define XAUDIO2_LOOP_INFINITE 255u
#define XAUDIO2_MAX_LOOP_COUNT 254u
#define XAUDIO2_MAX_QUEUED_BUFFERS 64u
#define XAUDIO2_MAX_BUFFER_BYTES 0x80000000u
#define XAUDIO2_MIN_SAMPLE_RATE 1000u
#define XAUDIO2_MAX_SAMPLE_RATE 200000u
#define XAUDIO2_MAX_AUDIO_CHANNELS 64u
#define XAUDIO2_MIN_FREQ_RATIO (1.0f / 1024.0f)
#define XAUDIO2_MAX_FREQ_RATIO 1024.0f

#define WAVE_FORMAT_PCM 1u
#define WAVE_FORMAT_IEEE_FLOAT 3u
#define WAVE_FORMAT_EXTENSIBLE 0xFFFEu

/* XAUDIO2_DEVICE_DETAILS (2.3): DeviceID WCHAR[256], DisplayName
   WCHAR[256], Role, OutputFormat (WAVEFORMATEXTENSIBLE). */
#define DETAILS_ROLE 0x400u
#define DETAILS_FORMAT 0x404u
#define DETAILS_SIZE 0x42Cu
#define GLOBAL_DEFAULT_DEVICE 0xFu

/* Voice callback vtable slots (IXAudio2VoiceCallback, not COM). */
enum {
    CB_PASS_START, CB_PASS_END, CB_STREAM_END, CB_BUFFER_START, CB_BUFFER_END, CB_LOOP_END, CB_VOICE_ERROR
};

#define RING_PASSES 4u
#define PROCESS_STACK_SIZE (256u * 1024u)

static const rt_guid IID_IXAudio2 = { 0x8BCF1F58, 0x9FE7, 0x4583, { 0x8A, 0xC6, 0xE2, 0xAD, 0xC4, 0x65, 0xC8, 0xBB } };
static const rt_guid SUBTYPE_PCM = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
static const rt_guid SUBTYPE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

/* ---------------------------------------------------------------- types */

typedef struct xa_buffer {
    uint32_t flags, data, ctx;
    uint32_t play_begin, play_end;  /* frames */
    uint32_t loop_begin, loop_end;  /* frames */
    uint32_t loops_left;            /* XAUDIO2_LOOP_INFINITE: no end */
    uint32_t cur;                   /* next frame */
    bool begun;
} xa_buffer;

typedef struct xa_event {
    int slot;
    uint32_t ctx;
} xa_event;

typedef struct xa_engine xa_engine;

typedef struct xa_voice xa_voice;
struct xa_voice {
    xa_engine *e;
    rt_com_object *obj;
    bool master;
    /* source voices */
    bool is_float;
    uint32_t bits, channels, rate, block;
    uint32_t callback;
    bool started;
    xa_buffer queue[XAUDIO2_MAX_QUEUED_BUFFERS];
    uint32_t head, count;
    float *fifo;          /* decoded frames; frame 0 is history */
    uint32_t fifo_cap, fifo_len;
    double pos;           /* read position in fifo frames */
    double step;          /* source frames for one output frame */
    xa_event *events;
    uint32_t nevents, events_cap;
    uint32_t bytes_required;
    xa_voice *next;
};

struct xa_engine {
    pthread_mutex_t lock;
    pthread_cond_t pass_cv;
    bool initialized;
    xa_voice *master;
    xa_voice *sources;
    uint32_t nsources;
    /* processing */
    uint32_t channels, rate, quantum;
    pthread_t thread;
    bool thread_running;
    atomic_bool quit;
    bool in_pass;
    pthread_t pass_thread;
    dispatch_semaphore_t space;
    float *mix;
    xa_voice **pass_voices;
    uint32_t pass_cap;
    /* output */
    AudioComponentInstance au;
    bool au_open, au_started;
    float *ring;
    uint32_t ring_frames;
    _Atomic uint64_t wpos, rpos;   /* frames written and read */
    uint64_t render_signaled;      /* render thread only: passes signaled */
    _Atomic uint64_t underflow;    /* frames of silence after the first write */
    uint64_t underflow_logged;
};

static void elock(xa_engine *e)
{
    if (pthread_mutex_lock(&e->lock) != 0) rt_fatal("XAudio2: mutex lock failed");
}

static void eunlock(xa_engine *e)
{
    if (pthread_mutex_unlock(&e->lock) != 0) rt_fatal("XAudio2: mutex unlock failed");
}

static float f32_arg(uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

/* ---------------------------------------------------------------- CoreAudio device */

typedef struct dev_info {
    AudioObjectID id;
    uint32_t channels;
    uint32_t rate;
} dev_info;

static bool default_device(dev_info *d)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 size = sizeof d->id;
    Float64 rate = 0;
    AudioBufferList *bl;
    UInt32 i, ch = 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &d->id) != noErr ||
        d->id == kAudioObjectUnknown)
        return false;
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    size = sizeof rate;
    if (AudioObjectGetPropertyData(d->id, &a, 0, NULL, &size, &rate) != noErr || !(rate >= XAUDIO2_MIN_SAMPLE_RATE) ||
        rate > XAUDIO2_MAX_SAMPLE_RATE)
    {
        rt_log("XAudio2: the default output device reports no usable sample rate (%f)", rate);
        return false;
    }
    a.mSelector = kAudioDevicePropertyStreamConfiguration;
    a.mScope = kAudioObjectPropertyScopeOutput;
    if (AudioObjectGetPropertyDataSize(d->id, &a, 0, NULL, &size) != noErr || size < sizeof(AudioBufferList))
        return false;
    bl = malloc(size);
    if (bl == NULL) rt_fatal("XAudio2: out of host memory");
    if (AudioObjectGetPropertyData(d->id, &a, 0, NULL, &size, bl) != noErr)
    {
        free(bl);
        return false;
    }
    for (i = 0; i < bl->mNumberBuffers; i++) ch += bl->mBuffers[i].mNumberChannels;
    free(bl);
    if (ch == 0) return false;
    if (ch > XAUDIO2_MAX_AUDIO_CHANNELS)
        rt_fatal("XAudio2: the default output device has %u channels; XAudio2 supports at most %u", ch,
                 XAUDIO2_MAX_AUDIO_CHANNELS);
    d->channels = ch;
    d->rate = (uint32_t)lround(rate);
    return true;
}

/* Copy a CoreAudio string property into a guest WCHAR[256] field. */
static void device_string(AudioObjectID id, AudioObjectPropertySelector sel, uint32_t g, bool must_fit, const char *what)
{
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 size = sizeof s;
    UniChar chars[256];
    CFIndex n;
    memset(G2H(g), 0, 512);
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &size, &s) != noErr || s == NULL)
    {
        rt_log("XAudio2: the default output device has no %s", what);
        return;
    }
    n = CFStringGetLength(s);
    if (n > 255)
    {
        if (must_fit) rt_fatal("XAudio2: the device %s has %ld characters; the limit is 255", what, (long)n);
        n = 255; /* a display name may be cut */
    }
    CFStringGetCharacters(s, CFRangeMake(0, n), chars);
    CFRelease(s);
    memcpy(G2H(g), chars, (size_t)n * 2);
}

static uint32_t channel_mask(uint32_t ch)
{
    switch (ch)
    {
    case 1: return 0x4u;    /* front center */
    case 2: return 0x3u;    /* front left, front right */
    case 4: return 0x33u;   /* quadraphonic */
    case 6: return 0x3Fu;   /* 5.1 */
    case 8: return 0x63Fu;  /* 7.1 with side speakers */
    default: return 0;      /* unspecified layout */
    }
}

/* ---------------------------------------------------------------- output unit */

static OSStatus render(void *ctx, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *io)
{
    xa_engine *e = ctx;
    float *out;
    uint64_t r, w, avail, n, i, passes;
    uint32_t ch = e->channels;
    (void)flags;
    (void)ts;
    (void)bus;
    if (io->mNumberBuffers != 1 || io->mBuffers[0].mData == NULL) return noErr;
    out = io->mBuffers[0].mData;
    r = atomic_load_explicit(&e->rpos, memory_order_relaxed);
    w = atomic_load_explicit(&e->wpos, memory_order_acquire);
    avail = w - r;
    n = avail < frames ? avail : frames;
    for (i = 0; i < n; i++)
        memcpy(out + i * ch, e->ring + ((r + i) % e->ring_frames) * ch, ch * sizeof(float));
    if (n < frames)
    {
        memset(out + n * ch, 0, (size_t)(frames - n) * ch * sizeof(float));
        if (w != 0) atomic_fetch_add_explicit(&e->underflow, frames - n, memory_order_relaxed);
    }
    atomic_store_explicit(&e->rpos, r + n, memory_order_release);
    passes = (r + n) / e->quantum;
    while (e->render_signaled < passes)
    {
        e->render_signaled++;
        dispatch_semaphore_signal(e->space);
    }
    return noErr;
}

static void close_unit(xa_engine *e)
{
    if (e->au_started && AudioOutputUnitStop(e->au) != noErr) rt_log("XAudio2: AudioOutputUnitStop failed");
    e->au_started = false;
    if (e->au_open)
    {
        if (AudioUnitUninitialize(e->au) != noErr) rt_log("XAudio2: AudioUnitUninitialize failed");
        if (AudioComponentInstanceDispose(e->au) != noErr) rt_log("XAudio2: AudioComponentInstanceDispose failed");
    }
    e->au_open = false;
}

static bool open_unit(xa_engine *e)
{
    AudioComponentDescription desc = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                       kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    AudioStreamBasicDescription f;
    AURenderCallbackStruct cb = { render, e };
    OSStatus st;
    if (comp == NULL)
    {
        rt_log("XAudio2: no default output audio unit");
        return false;
    }
    st = AudioComponentInstanceNew(comp, &e->au);
    if (st != noErr)
    {
        rt_log("XAudio2: AudioComponentInstanceNew failed (%d)", (int)st);
        return false;
    }
    e->au_open = true;
    memset(&f, 0, sizeof f);
    f.mSampleRate = e->rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mBytesPerPacket = 4u * e->channels;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4u * e->channels;
    f.mChannelsPerFrame = e->channels;
    f.mBitsPerChannel = 32;
    st = AudioUnitSetProperty(e->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (st == noErr)
        st = AudioUnitSetProperty(e->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (st == noErr) st = AudioUnitInitialize(e->au);
    if (st != noErr)
    {
        rt_log("XAudio2: the output audio unit does not accept %u channels at %u Hz (%d)", e->channels, e->rate, (int)st);
        if (AudioComponentInstanceDispose(e->au) != noErr) rt_log("XAudio2: AudioComponentInstanceDispose failed");
        e->au_open = false;
        return false;
    }
    st = AudioOutputUnitStart(e->au);
    if (st != noErr)
    {
        rt_log("XAudio2: AudioOutputUnitStart failed (%d)", (int)st);
        close_unit(e);
        return false;
    }
    e->au_started = true;
    return true;
}

/* ---------------------------------------------------------------- mixing */

static void add_event(xa_voice *v, int slot, uint32_t ctx)
{
    if (v->nevents >= v->events_cap) rt_fatal("XAudio2: too many voice callbacks in one pass (%u)", v->nevents);
    v->events[v->nevents].slot = slot;
    v->events[v->nevents].ctx = ctx;
    v->nevents++;
}

static float sample(const uint8_t *p, uint32_t bits, bool is_float)
{
    if (is_float)
    {
        float f;
        memcpy(&f, p, 4);
        return f;
    }
    switch (bits)
    {
    case 8: return (float)((int)p[0] - 128) * (1.0f / 128.0f);
    case 16: { int16_t s; memcpy(&s, p, 2); return (float)s * (1.0f / 32768.0f); }
    case 24: {
        int32_t s = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
        return (float)s * (1.0f / 8388608.0f);
    }
    default: { int32_t s; memcpy(&s, p, 4); return (float)((double)s * (1.0 / 2147483648.0)); }
    }
}

/* Frames that the queue can still give (loops counted once). */
static uint64_t queued_frames(const xa_voice *v)
{
    uint64_t n = 0;
    uint32_t i;
    for (i = 0; i < v->count; i++)
    {
        const xa_buffer *b = &v->queue[(v->head + i) % XAUDIO2_MAX_QUEUED_BUFFERS];
        n += b->play_end - b->cur;
    }
    return n;
}

static uint32_t frames_needed(const xa_voice *v, uint32_t quantum)
{
    double last = floor(v->pos + v->step * (double)(quantum - 1)) + 2.0;
    return (uint32_t)last + 1u;
}

/* Decode queued frames into the FIFO until it holds want frames or the
   queue is empty. Records the buffer callbacks. */
static void decode(xa_voice *v, uint32_t want)
{
    uint32_t ch = v->channels, bps = v->block / v->channels;
    while (v->fifo_len < want && v->count > 0)
    {
        xa_buffer *b = &v->queue[v->head];
        bool looping = b->loops_left > 0;
        uint32_t end = looping ? b->loop_end : b->play_end, n, f, c;
        const uint8_t *src;
        if (!b->begun)
        {
            b->begun = true;
            add_event(v, CB_BUFFER_START, b->ctx);
        }
        n = end - b->cur;
        if (n > want - v->fifo_len) n = want - v->fifo_len;
        src = (const uint8_t *)G2H(b->data) + (size_t)b->cur * v->block;
        for (f = 0; f < n; f++)
            for (c = 0; c < ch; c++)
                v->fifo[(size_t)(v->fifo_len + f) * ch + c] = sample(src + (size_t)f * v->block + (size_t)c * bps, v->bits,
                                                                    v->is_float);
        v->fifo_len += n;
        b->cur += n;
        if (b->cur < end) continue;
        if (looping)
        {
            if (b->loops_left != XAUDIO2_LOOP_INFINITE) b->loops_left--;
            b->cur = b->loop_begin;
            add_event(v, CB_LOOP_END, b->ctx);
            continue;
        }
        add_event(v, CB_BUFFER_END, b->ctx);
        if (b->flags & XAUDIO2_END_OF_STREAM) add_event(v, CB_STREAM_END, 0);
        v->head = (v->head + 1) % XAUDIO2_MAX_QUEUED_BUFFERS;
        v->count--;
    }
}

static float hermite(float xm1, float x0, float x1, float x2, float t)
{
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

static void mix_voice(xa_engine *e, xa_voice *v)
{
    uint32_t q = e->quantum, ch = v->channels, want = frames_needed(v, q), f, c, drop;
    if (want > v->fifo_cap) rt_fatal("XAudio2: internal error: resampler buffer too small (%u > %u)", want, v->fifo_cap);
    decode(v, want);
    if (v->fifo_len <= 1) return; /* starved: nothing but history */
    if (v->fifo_len < want)
    {
        /* Starvation inside the pass: the rest is silence. */
        memset(v->fifo + (size_t)v->fifo_len * ch, 0, (size_t)(want - v->fifo_len) * ch * sizeof(float));
        v->fifo_len = want;
    }
    for (f = 0; f < q; f++)
    {
        double p = v->pos + v->step * (double)f;
        uint32_t i = (uint32_t)p;
        float t = (float)(p - (double)i);
        const float *s = v->fifo + (size_t)(i - 1) * ch;
        float *out = e->mix + (size_t)f * ch;
        for (c = 0; c < ch; c++) out[c] += hermite(s[c], s[ch + c], s[2 * ch + c], s[3 * ch + c], t);
    }
    v->pos += v->step * (double)q;
    drop = (uint32_t)v->pos - 1u;
    if (drop > v->fifo_len) drop = v->fifo_len;
    if (drop > 0)
    {
        memmove(v->fifo, v->fifo + (size_t)drop * ch, (size_t)(v->fifo_len - drop) * ch * sizeof(float));
        v->fifo_len -= drop;
        v->pos -= drop;
    }
}

static void call_cb(_cpu *cpu, uint32_t cb, int slot, int nargs, uint32_t a1, uint32_t a2)
{
    uint32_t args[3] = { cb, a1, a2 };
    uint32_t fn = rd32(rd32(cb) + 4u * (uint32_t)slot);
    (void)rt_call(cpu, fn, true, 0, nargs, args);
}

static void run_pass(xa_engine *e, _cpu *cpu)
{
    uint32_t n = 0, i, total;
    xa_voice *v;
    float *mix = e->mix;
    uint64_t w;

    elock(e);
    e->in_pass = true;
    e->pass_thread = pthread_self();
    for (v = e->sources; v != NULL; v = v->next)
    {
        uint64_t have, need;
        if (!v->started) continue;
        if (n >= e->pass_cap) rt_fatal("XAudio2: internal error: pass voice list too small");
        e->pass_voices[n++] = v;
        have = queued_frames(v) + v->fifo_len;
        need = frames_needed(v, e->quantum);
        v->bytes_required = need > have ? (uint32_t)((need - have) * v->block) : 0;
        v->nevents = 0;
    }
    eunlock(e);

    for (i = 0; i < n; i++)
        if (e->pass_voices[i]->callback != 0)
            call_cb(cpu, e->pass_voices[i]->callback, CB_PASS_START, 2, e->pass_voices[i]->bytes_required, 0);

    elock(e);
    total = e->quantum * e->channels;
    memset(mix, 0, (size_t)total * sizeof(float));
    for (i = 0; i < n; i++) mix_voice(e, e->pass_voices[i]);
    eunlock(e);

    for (i = 0; i < n; i++)
    {
        xa_voice *pv = e->pass_voices[i];
        uint32_t k;
        if (pv->callback == 0) continue;
        for (k = 0; k < pv->nevents; k++)
        {
            if (pv->events[k].slot == CB_STREAM_END)
                call_cb(cpu, pv->callback, CB_STREAM_END, 1, 0, 0);
            else
                call_cb(cpu, pv->callback, pv->events[k].slot, 2, pv->events[k].ctx, 0);
        }
        call_cb(cpu, pv->callback, CB_PASS_END, 1, 0, 0);
    }

    elock(e);
    e->in_pass = false;
    if (pthread_cond_broadcast(&e->pass_cv) != 0) rt_fatal("XAudio2: condition broadcast failed");
    eunlock(e);

    w = atomic_load_explicit(&e->wpos, memory_order_relaxed);
    for (i = 0; i < e->quantum; i++)
    {
        float *dst = e->ring + ((w + i) % e->ring_frames) * e->channels;
        uint32_t c;
        for (c = 0; c < e->channels; c++)
        {
            float s = mix[(size_t)i * e->channels + c];
            dst[c] = s > 1.0f ? 1.0f : s < -1.0f ? -1.0f : s;
        }
    }
    atomic_store_explicit(&e->wpos, w + e->quantum, memory_order_release);
}

static void *process_main(void *arg)
{
    xa_engine *e = arg;
    rt_thread *t;
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) != 0)
        rt_log("XAudio2: the processing thread priority could not be set");
    t = rt_thread_create_state(PROCESS_STACK_SIZE, 0);
    rt_module_thread_attach(t->cpu);
    for (;;)
    {
        uint64_t u;
        dispatch_semaphore_wait(e->space, DISPATCH_TIME_FOREVER);
        if (atomic_load(&e->quit)) break;
        run_pass(e, t->cpu);
        u = atomic_load_explicit(&e->underflow, memory_order_relaxed);
        if (u > e->underflow_logged)
        {
            rt_log("XAudio2: the output had %llu frames of silence because the mix was late",
                   (unsigned long long)u);
            /* log again after one more second of silence */
            e->underflow_logged = u + e->rate;
        }
    }
    rt_module_thread_detach(t->cpu);
    rt_thread_destroy_state(t);
    return NULL;
}

/* Stop the processing thread and the output (lock not held). */
static void stop_output(xa_engine *e)
{
    if (e->thread_running)
    {
        if (pthread_equal(pthread_self(), e->thread))
            rt_fatal("XAudio2: the mastering voice or the engine was destroyed from a voice callback");
        atomic_store(&e->quit, true);
        dispatch_semaphore_signal(e->space);
        if (pthread_join(e->thread, NULL) != 0) rt_fatal("XAudio2: pthread_join failed");
        e->thread_running = false;
    }
    close_unit(e);
    free(e->ring);
    free(e->mix);
    e->ring = NULL;
    e->mix = NULL;
    if (e->space != NULL) dispatch_release(e->space);
    e->space = NULL;
}

/* ---------------------------------------------------------------- COM classes */

static void voice_destroy(rt_com_object *o)
{
    xa_voice *v = o->data;
    free(v->fifo);
    free(v->events);
    free(v);
}

static void engine_destroy(rt_com_object *o);

static const rt_com_iface engine_ifaces[] = { { &IID_IXAudio2, 0 } };
static const rt_com_class engine_class = {
    "IXAudio2", 1, { &rt_vt_XAudio2, NULL, NULL, NULL }, engine_ifaces, 1, engine_destroy
};
/* The voices are not COM objects: their vtables have no IUnknown. */
static const rt_com_class source_class = {
    "IXAudio2SourceVoice", 1, { &rt_vt_XAudio2SourceVoice, NULL, NULL, NULL }, NULL, 0, voice_destroy
};
static const rt_com_class master_class = {
    "IXAudio2MasteringVoice", 1, { &rt_vt_XAudio2MasteringVoice, NULL, NULL, NULL }, NULL, 0, voice_destroy
};

static void engine_destroy(rt_com_object *o)
{
    xa_engine *e = o->data;
    xa_voice *v, *next;
    /* The final Release destroys all voices. */
    stop_output(e);
    for (v = e->sources; v != NULL; v = next)
    {
        next = v->next;
        (void)rt_com_release(v->obj);
    }
    if (e->master != NULL) (void)rt_com_release(e->master->obj);
    free(e->pass_voices);
    if (pthread_cond_destroy(&e->pass_cv) != 0) rt_log("XAudio2: condition variable destroy failed");
    if (pthread_mutex_destroy(&e->lock) != 0) rt_log("XAudio2: mutex destroy failed");
    free(e);
}

rt_com_object *rt_xaudio2_create(void)
{
    xa_engine *e = calloc(1, sizeof *e);
    rt_com_object *o;
    if (e == NULL) return NULL;
    if (pthread_mutex_init(&e->lock, NULL) != 0 || pthread_cond_init(&e->pass_cv, NULL) != 0)
        rt_fatal("XAudio2: mutex creation failed");
    atomic_init(&e->quit, false);
    atomic_init(&e->wpos, 0);
    atomic_init(&e->rpos, 0);
    atomic_init(&e->underflow, 0);
    o = rt_com_create(&engine_class, e);
    if (o == NULL)
    {
        pthread_cond_destroy(&e->pass_cv);
        pthread_mutex_destroy(&e->lock);
        free(e);
    }
    return o;
}

static xa_engine *engine_this(_cpu *cpu)
{
    return rt_com_this(ARG(0), 0, &engine_class)->data;
}

/* HRESULT Initialize(UINT32 flags, XAUDIO2_PROCESSOR) */
void rt_com_XAudio2_Initialize(_cpu *cpu)
{
    xa_engine *e = engine_this(cpu);
    uint32_t flags = ARG(1), r;
    if (flags != 0) rt_fatal("IXAudio2::Initialize with flags 0x%x (debug engine) is not supported", flags);
    elock(e);
    r = e->initialized ? XAUDIO2_E_INVALID_CALL : S_OK;
    e->initialized = true;
    eunlock(e);
    RET(r);
}

/* HRESULT GetDeviceCount(UINT32 *) */
void rt_com_XAudio2_GetDeviceCount(_cpu *cpu)
{
    xa_engine *e = engine_this(cpu);
    dev_info d;
    if (!e->initialized)
    {
        RET(XAUDIO2_E_INVALID_CALL);
        return;
    }
    if (ARG(1) == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    wr32(ARG(1), default_device(&d) ? 1u : 0u);
    RET(S_OK);
}

/* HRESULT GetDeviceDetails(UINT32 index, XAUDIO2_DEVICE_DETAILS *) */
void rt_com_XAudio2_GetDeviceDetails(_cpu *cpu)
{
    xa_engine *e = engine_this(cpu);
    uint32_t index = ARG(1), p = ARG(2), fmt = p + DETAILS_FORMAT, block;
    dev_info d;
    if (!e->initialized)
    {
        RET(XAUDIO2_E_INVALID_CALL);
        return;
    }
    if (p == 0 || index != 0 || !default_device(&d))
    {
        RET(E_INVALIDARG);
        return;
    }
    memset(G2H(p), 0, DETAILS_SIZE);
    device_string(d.id, kAudioDevicePropertyDeviceUID, p, true, "UID");
    device_string(d.id, kAudioObjectPropertyName, p + 0x200, false, "name");
    wr32(p + DETAILS_ROLE, GLOBAL_DEFAULT_DEVICE);
    block = d.channels * 2u;
    wr16(fmt + 0x00, WAVE_FORMAT_EXTENSIBLE);
    wr16(fmt + 0x02, (uint16_t)d.channels);
    wr32(fmt + 0x04, d.rate);
    wr32(fmt + 0x08, d.rate * block);
    wr16(fmt + 0x0C, (uint16_t)block);
    wr16(fmt + 0x0E, 16);
    wr16(fmt + 0x10, 22);
    wr16(fmt + 0x12, 16);
    wr32(fmt + 0x14, channel_mask(d.channels));
    rt_guid_write(fmt + 0x18, &SUBTYPE_PCM);
    RET(S_OK);
}

/* HRESULT CreateMasteringVoice(IXAudio2MasteringVoice **, UINT32 channels,
   UINT32 rate, UINT32 flags, UINT32 device, const XAUDIO2_EFFECT_CHAIN *) */
void rt_com_XAudio2_CreateMasteringVoice(_cpu *cpu)
{
    xa_engine *e = engine_this(cpu);
    uint32_t pp = ARG(1), ch = ARG(2), rate = ARG(3), flags = ARG(4), dev = ARG(5), chain = ARG(6);
    dev_info d;
    xa_voice *v;
    rt_com_object *o;
    if (pp == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    wr32(pp, 0);
    if (flags != 0) rt_fatal("IXAudio2::CreateMasteringVoice with flags 0x%x is not supported", flags);
    if (chain != 0) rt_fatal("IXAudio2::CreateMasteringVoice with an effect chain is not supported");
    /* The lock is held until the voice is published (no guest call
       happens before that). */
    elock(e);
    if (!e->initialized || e->master != NULL)
    {
        eunlock(e);
        RET(XAUDIO2_E_INVALID_CALL);
        return;
    }
    if (dev != 0 || !default_device(&d))
    {
        eunlock(e);
        RET(E_INVALIDARG);
        return;
    }
    if (ch == 0) ch = d.channels;
    if (rate == 0) rate = d.rate;
    if (ch > XAUDIO2_MAX_AUDIO_CHANNELS || rate < XAUDIO2_MIN_SAMPLE_RATE || rate > XAUDIO2_MAX_SAMPLE_RATE)
    {
        eunlock(e);
        RET(E_INVALIDARG);
        return;
    }
    v = calloc(1, sizeof *v);
    if (v == NULL)
    {
        eunlock(e);
        RET(E_OUTOFMEMORY);
        return;
    }
    v->e = e;
    v->master = true;
    v->channels = ch;
    v->rate = rate;
    o = rt_com_create(&master_class, v);
    if (o == NULL)
    {
        eunlock(e);
        free(v);
        RET(E_OUTOFMEMORY);
        return;
    }
    v->obj = o;

    /* Output: 10 ms passes and a ring of RING_PASSES passes. */
    e->channels = ch;
    e->rate = rate;
    e->quantum = rate / 100u;
    e->ring_frames = e->quantum * RING_PASSES;
    e->ring = calloc((size_t)e->ring_frames * ch, sizeof(float));
    e->mix = calloc((size_t)e->quantum * ch, sizeof(float));
    if (e->ring == NULL || e->mix == NULL) rt_fatal("XAudio2: out of host memory");
    atomic_store(&e->wpos, 0);
    atomic_store(&e->rpos, 0);
    atomic_store(&e->underflow, 0);
    atomic_store(&e->quit, false);
    e->render_signaled = 0;
    e->underflow_logged = 0;
    /* libdispatch stops the process when a semaphore is released with a
       count below its creation value, so the semaphore starts at 0 and
       gets its RING_PASSES free passes from signals. */
    e->space = dispatch_semaphore_create(0);
    if (e->space == NULL) rt_fatal("XAudio2: semaphore creation failed");
    for (uint32_t k = 0; k < RING_PASSES; k++) dispatch_semaphore_signal(e->space);
    if (!open_unit(e))
    {
        free(e->ring);
        free(e->mix);
        e->ring = NULL;
        e->mix = NULL;
        dispatch_release(e->space);
        e->space = NULL;
        eunlock(e);
        (void)rt_com_release(o);
        RET(XAUDIO2_E_DEVICE_INVALIDATED);
        return;
    }
    e->master = v;
    if (pthread_create(&e->thread, NULL, process_main, e) != 0) rt_fatal("XAudio2: thread creation failed");
    e->thread_running = true;
    eunlock(e);
    wr32(pp, rt_com_ptr(o, 0));
    RET(S_OK);
}

/* Read and check a WAVEFORMATEX / WAVEFORMATEXTENSIBLE. */
static bool read_format(uint32_t f, xa_voice *v)
{
    uint32_t tag = rd16(f), ch = rd16(f + 2), rate = rd32(f + 4), block = rd16(f + 12), bits = rd16(f + 14);
    bool is_float;
    if (tag == WAVE_FORMAT_EXTENSIBLE)
    {
        rt_guid sub;
        if (rd16(f + 16) < 22) return false;
        sub = rt_guid_read(f + 24);
        if (rt_guid_eq(&sub, &SUBTYPE_PCM))
            tag = WAVE_FORMAT_PCM;
        else if (rt_guid_eq(&sub, &SUBTYPE_FLOAT))
            tag = WAVE_FORMAT_IEEE_FLOAT;
        else
        {
            char text[39];
            rt_guid_format(&sub, text, true, false);
            rt_fatal("XAudio2: source voices with the sample subtype %s are not supported", text);
        }
        if (rd16(f + 18) > bits) return false;
    }
    if (tag == WAVE_FORMAT_PCM)
    {
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;
        is_float = false;
    }
    else if (tag == WAVE_FORMAT_IEEE_FLOAT)
    {
        if (bits != 32) return false;
        is_float = true;
    }
    else
    {
        rt_fatal("XAudio2: source voices with the format tag 0x%04x (compressed data) are not supported", tag);
    }
    if (ch == 0 || ch > XAUDIO2_MAX_AUDIO_CHANNELS || rate < XAUDIO2_MIN_SAMPLE_RATE ||
        rate > XAUDIO2_MAX_SAMPLE_RATE || block != ch * bits / 8u)
        return false;
    v->is_float = is_float;
    v->bits = bits;
    v->channels = ch;
    v->rate = rate;
    v->block = block;
    return true;
}

/* HRESULT CreateSourceVoice(IXAudio2SourceVoice **, const WAVEFORMATEX *,
   UINT32 flags, float max_ratio, IXAudio2VoiceCallback *,
   const XAUDIO2_VOICE_SENDS *, const XAUDIO2_EFFECT_CHAIN *) */
void rt_com_XAudio2_CreateSourceVoice(_cpu *cpu)
{
    xa_engine *e = engine_this(cpu);
    uint32_t pp = ARG(1), fmt = ARG(2), flags = ARG(3), cb = ARG(5), sends = ARG(6), chain = ARG(7);
    float max_ratio = f32_arg(ARG(4));
    xa_voice *v;
    xa_voice **pv;
    rt_com_object *o;
    if (pp == 0 || fmt == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    wr32(pp, 0);
    if (flags != 0) rt_fatal("IXAudio2::CreateSourceVoice with flags 0x%x is not supported", flags);
    if (sends != 0) rt_fatal("IXAudio2::CreateSourceVoice with a send list is not supported");
    if (chain != 0) rt_fatal("IXAudio2::CreateSourceVoice with an effect chain is not supported");
    if (!(max_ratio >= XAUDIO2_MIN_FREQ_RATIO && max_ratio <= XAUDIO2_MAX_FREQ_RATIO))
    {
        RET(E_INVALIDARG);
        return;
    }
    v = calloc(1, sizeof *v);
    if (v == NULL)
    {
        RET(E_OUTOFMEMORY);
        return;
    }
    if (!read_format(fmt, v))
    {
        free(v);
        RET(E_INVALIDARG);
        return;
    }
    elock(e);
    if (e->master == NULL)
    {
        eunlock(e);
        free(v);
        RET(XAUDIO2_E_INVALID_CALL);
        return;
    }
    if (v->channels != e->channels)
        rt_fatal("XAudio2: a source voice with %u channels into a mastering voice with %u channels (default "
                 "channel matrix) is not supported", v->channels, e->channels);
    v->e = e;
    v->callback = cb;
    v->step = (double)v->rate / (double)e->rate;
    /* The FIFO holds one pass of source frames plus the interpolation
       neighbours. */
    v->fifo_cap = (uint32_t)ceil(v->step * (double)e->quantum) + 16u;
    v->fifo = calloc((size_t)v->fifo_cap * v->channels, sizeof(float));
    /* Callbacks of one pass: at most one loop end for each decoded frame,
       and three callbacks for each queued buffer. */
    v->events_cap = v->fifo_cap + 3u * XAUDIO2_MAX_QUEUED_BUFFERS + 8u;
    v->events = calloc(v->events_cap, sizeof *v->events);
    /* The processing thread reads pass_voices without the lock during a
       pass: another thread waits for the end of the pass. A callback on the
       processing thread itself may grow the array (run_pass reads the
       array pointer again after each callback). */
    while (e->in_pass && !pthread_equal(pthread_self(), e->pass_thread))
        if (pthread_cond_wait(&e->pass_cv, &e->lock) != 0) rt_fatal("XAudio2: condition wait failed");
    pv = realloc(e->pass_voices, (size_t)(e->nsources + 1) * sizeof *pv);
    if (v->fifo == NULL || v->events == NULL || pv == NULL) rt_fatal("XAudio2: out of host memory");
    e->pass_voices = pv;
    e->pass_cap = e->nsources + 1;
    v->fifo_len = 1; /* one history frame of silence */
    v->pos = 1.0;
    o = rt_com_create(&source_class, v);
    if (o == NULL)
    {
        eunlock(e);
        free(v->fifo);
        free(v->events);
        free(v);
        RET(E_OUTOFMEMORY);
        return;
    }
    v->obj = o;
    v->next = e->sources;
    e->sources = v;
    e->nsources++;
    eunlock(e);
    wr32(pp, rt_com_ptr(o, 0));
    RET(S_OK);
}

/* ---------------------------------------------------------------- voices */

static xa_voice *source_this(_cpu *cpu)
{
    return rt_com_this(ARG(0), 0, &source_class)->data;
}

static xa_voice *master_this(_cpu *cpu)
{
    return rt_com_this(ARG(0), 0, &master_class)->data;
}

/* Wait until no pass runs (lock held). A voice callback must not destroy
   a voice. */
static void wait_no_pass(xa_engine *e)
{
    while (e->in_pass)
    {
        if (pthread_equal(pthread_self(), e->pass_thread))
            rt_fatal("XAudio2: DestroyVoice was called from a voice callback");
        if (pthread_cond_wait(&e->pass_cv, &e->lock) != 0) rt_fatal("XAudio2: condition wait failed");
    }
}

/* HRESULT Start(UINT32 flags, UINT32 operation_set) */
void rt_com_XAudio2SourceVoice_Start(_cpu *cpu)
{
    xa_voice *v = source_this(cpu);
    if (ARG(1) != 0) rt_fatal("IXAudio2SourceVoice::Start with flags 0x%x is not supported", ARG(1));
    if (ARG(2) != XAUDIO2_COMMIT_NOW) rt_fatal("XAudio2: deferred operation sets are not supported (Start)");
    elock(v->e);
    v->started = true;
    eunlock(v->e);
    RET(S_OK);
}

/* HRESULT Stop(UINT32 flags, UINT32 operation_set). Without effects,
   XAUDIO2_PLAY_TAILS has nothing to play. */
void rt_com_XAudio2SourceVoice_Stop(_cpu *cpu)
{
    xa_voice *v = source_this(cpu);
    if ((ARG(1) & ~XAUDIO2_PLAY_TAILS) != 0) rt_fatal("IXAudio2SourceVoice::Stop with flags 0x%x is not supported", ARG(1));
    if (ARG(2) != XAUDIO2_COMMIT_NOW) rt_fatal("XAudio2: deferred operation sets are not supported (Stop)");
    elock(v->e);
    v->started = false;
    eunlock(v->e);
    RET(S_OK);
}

/* HRESULT SubmitSourceBuffer(const XAUDIO2_BUFFER *, reserved). The
   2.3 XAUDIO2_BUFFER: Flags, AudioBytes, pAudioData, PlayBegin,
   PlayLength, LoopBegin, LoopLength, LoopCount, pContext. */
void rt_com_XAudio2SourceVoice_SubmitSourceBuffer(_cpu *cpu)
{
    xa_voice *v = source_this(cpu);
    uint32_t p = ARG(1), flags, bytes, data, pb, pl, lb, ll, lc, frames;
    xa_buffer b;
    if (ARG(2) != 0) rt_fatal("IXAudio2SourceVoice::SubmitSourceBuffer with a second buffer argument is not supported");
    if (p == 0)
    {
        RET(E_INVALIDARG);
        return;
    }
    flags = rd32(p);
    bytes = rd32(p + 4);
    data = rd32(p + 8);
    pb = rd32(p + 12);
    pl = rd32(p + 16);
    lb = rd32(p + 20);
    ll = rd32(p + 24);
    lc = rd32(p + 28);
    if ((flags & ~XAUDIO2_END_OF_STREAM) != 0 || bytes == 0 || bytes > XAUDIO2_MAX_BUFFER_BYTES ||
        bytes % v->block != 0 || data == 0 || !rt_va_is_committed(data, bytes))
    {
        RET(XAUDIO2_E_INVALID_CALL);
        return;
    }
    frames = bytes / v->block;
    memset(&b, 0, sizeof b);
    b.flags = flags;
    b.data = data;
    b.ctx = rd32(p + 32);
    if (pb >= frames || (pl != 0 && pl > frames - pb))
    {
        RET(XAUDIO2_E_INVALID_CALL);
        return;
    }
    b.play_begin = pb;
    b.play_end = pl != 0 ? pb + pl : frames;
    if (lc == 0)
    {
        if (lb != 0 || ll != 0)
        {
            RET(XAUDIO2_E_INVALID_CALL);
            return;
        }
        b.loop_begin = b.play_begin;
        b.loop_end = b.play_end;
    }
    else
    {
        if (lc > XAUDIO2_MAX_LOOP_COUNT && lc != XAUDIO2_LOOP_INFINITE)
        {
            RET(XAUDIO2_E_INVALID_CALL);
            return;
        }
        b.loop_begin = lb;
        b.loop_end = ll != 0 ? lb + ll : b.play_end;
        if (lb < b.play_begin || lb >= b.play_end || ll > b.play_end - lb)
        {
            RET(XAUDIO2_E_INVALID_CALL);
            return;
        }
    }
    b.loops_left = lc;
    b.cur = b.play_begin;
    elock(v->e);
    if (v->count >= XAUDIO2_MAX_QUEUED_BUFFERS)
    {
        eunlock(v->e);
        RET(XAUDIO2_E_INVALID_CALL);
        return;
    }
    v->queue[(v->head + v->count) % XAUDIO2_MAX_QUEUED_BUFFERS] = b;
    v->count++;
    eunlock(v->e);
    RET(S_OK);
}

/* void DestroyVoice(void) */
void rt_com_XAudio2SourceVoice_DestroyVoice(_cpu *cpu)
{
    xa_voice *v = source_this(cpu), **pp;
    xa_engine *e = v->e;
    elock(e);
    wait_no_pass(e);
    for (pp = &e->sources; *pp != NULL; pp = &(*pp)->next)
    {
        if (*pp == v)
        {
            *pp = v->next;
            break;
        }
    }
    e->nsources--;
    eunlock(e);
    (void)rt_com_release(v->obj);
}

/* void DestroyVoice(void) */
void rt_com_XAudio2MasteringVoice_DestroyVoice(_cpu *cpu)
{
    xa_voice *v = master_this(cpu);
    xa_engine *e = v->e;
    elock(e);
    if (e->sources != NULL)
        rt_fatal("XAudio2: the mastering voice was destroyed while source voices still send to it");
    eunlock(e);
    stop_output(e);
    elock(e);
    e->master = NULL;
    eunlock(e);
    (void)rt_com_release(v->obj);
}
