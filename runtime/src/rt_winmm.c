/*
 * WINMM: multimedia timers (timeSetEvent, timeKillEvent) and waveform
 * audio output (waveOut*) on CoreAudio, and DSOUND DirectSoundEnumerateA.
 *
 * Callbacks into guest code run on runtime threads that have a guest
 * thread state, as the system threads of WINMM do on Windows:
 *   - one thread for each multimedia timer;
 *   - one thread for each open waveOut device. CoreAudio reports finished
 *     buffers on its own thread; that thread only queues the buffer, and
 *     the device thread sets WHDR_DONE and calls the guest callback.
 *
 * waveOut devices are the CoreAudio output devices; device 0 is the
 * default output device. The data format of the audio queue is always
 * 16-bit signed PCM: 8-bit unsigned PCM is converted when the buffer is
 * written.
 */
#include "rt.h"
#include "rt_object.h"
#include "rt_thread.h"
#include "rt_user32.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MMSYSERR_NOERROR 0u
#define MMSYSERR_BADDEVICEID 2u
#define MMSYSERR_NOMEM 7u
#define MMSYSERR_INVALHANDLE 5u
#define MMSYSERR_INVALFLAG 10u
#define MMSYSERR_INVALPARAM 11u
#define MMSYSERR_NODRIVER 6u
#define WAVERR_BADFORMAT 32u
#define WAVERR_STILLPLAYING 33u
#define WAVERR_UNPREPARED 34u

#define WAVE_MAPPER 0xFFFFFFFFu
#define WAVE_FORMAT_QUERY 0x0001u
#define WAVE_ALLOWSYNC 0x0002u
#define WAVE_MAPPED 0x0004u
#define WAVE_FORMAT_DIRECT 0x0008u
#define CALLBACK_TYPEMASK 0x00070000u
#define CALLBACK_NULL 0x00000000u
#define CALLBACK_WINDOW 0x00010000u
#define CALLBACK_THREAD 0x00020000u
#define CALLBACK_FUNCTION 0x00030000u
#define CALLBACK_EVENT 0x00050000u

#define WOM_OPEN 0x3BBu
#define WOM_CLOSE 0x3BCu
#define WOM_DONE 0x3BDu

#define WHDR_DONE 0x01u
#define WHDR_PREPARED 0x02u
#define WHDR_BEGINLOOP 0x04u
#define WHDR_ENDLOOP 0x08u
#define WHDR_INQUEUE 0x10u

#define TIME_PERIODIC 0x0001u
#define TIME_CALLBACK_FUNCTION 0x0000u
#define TIME_KILL_SYNCHRONOUS 0x0100u

#define CALLBACK_STACK_SIZE (256u * 1024u)

static uint64_t uptime_ns(void)
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void mlock(pthread_mutex_t *m)
{
    if (pthread_mutex_lock(m) != 0) rt_fatal("WINMM lock failed");
}

static void munlock(pthread_mutex_t *m)
{
    if (pthread_mutex_unlock(m) != 0) rt_fatal("WINMM unlock failed");
}

static void cond_init(pthread_cond_t *c)
{
    if (pthread_cond_init(c, NULL) != 0) rt_fatal("WINMM condition variable creation failed");
}

/* Wait on c for at most ns nanoseconds (relative). */
static void cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, uint64_t ns)
{
    struct timespec ts;
    int r;
    ts.tv_sec = (time_t)(ns / 1000000000u);
    ts.tv_nsec = (long)(ns % 1000000000u);
    r = pthread_cond_timedwait_relative_np(c, m, &ts);
    if (r != 0 && r != ETIMEDOUT) rt_fatal("WINMM condition wait failed (%d)", r);
}

static void cond_wait(pthread_cond_t *c, pthread_mutex_t *m)
{
    if (pthread_cond_wait(c, m) != 0) rt_fatal("WINMM condition wait failed");
}

/* ---------------------------------------------------------------- callback threads */

typedef struct cbthread {
    void (*body)(void *ctx, _cpu *cpu);
    void *ctx;
} cbthread;

static void *cb_main(void *arg)
{
    cbthread c = *(cbthread *)arg;
    rt_thread *t;
    free(arg);
    t = rt_thread_create_state(CALLBACK_STACK_SIZE, 0);
    rt_module_thread_attach(t->cpu);
    c.body(c.ctx, t->cpu);
    rt_module_thread_detach(t->cpu);
    rt_thread_destroy_state(t);
    return NULL;
}

static pthread_t start_cb_thread(void (*body)(void *, _cpu *), void *ctx)
{
    pthread_t pt;
    cbthread *c = malloc(sizeof *c);
    if (c == NULL) rt_fatal("out of host memory (callback thread)");
    c->body = body;
    c->ctx = ctx;
    if (pthread_create(&pt, NULL, cb_main, c) != 0) rt_fatal("callback thread creation failed");
    return pt;
}

/* ---------------------------------------------------------------- multimedia timers */

#define MAX_TIMERS 16

typedef struct mmtimer {
    bool used, stop, periodic, self_killed;
    uint32_t id, delay_ms, proc, user;
    pthread_t pt;
    pthread_cond_t cv;
} mmtimer;

static pthread_mutex_t tm_lock = PTHREAD_MUTEX_INITIALIZER;
static mmtimer timers[MAX_TIMERS];
static uint32_t next_timer_id = 1;

static void timer_body(void *ctx, _cpu *cpu)
{
    mmtimer *t = ctx;
    uint64_t period, deadline;
    mlock(&tm_lock);
    period = (uint64_t)t->delay_ms * 1000000u;
    deadline = uptime_ns() + period;
    while (!t->stop)
    {
        uint64_t now = uptime_ns();
        uint32_t id = t->id, proc = t->proc, user = t->user;
        if (now < deadline)
        {
            cond_wait_ns(&t->cv, &tm_lock, deadline - now);
            continue;
        }
        munlock(&tm_lock);
        rt_call(cpu, proc, true, 0, 5, (const uint32_t[]){ id, 0, user, 0, 0 });
        mlock(&tm_lock);
        if (!t->periodic) break;
        deadline += period;
        /* After a long callback, start the next period from now; Windows
           also does not call the callback again for each missed period. */
        now = uptime_ns();
        if (deadline + period < now) deadline = now + period;
    }
    /* A timer that ends by itself (one-shot) or that its own callback
       killed releases its slot here; otherwise timeKillEvent joins. */
    if (!t->stop || t->self_killed)
    {
        if (pthread_detach(t->pt) != 0) rt_fatal("timer thread detach failed");
        if (pthread_cond_destroy(&t->cv) != 0) rt_fatal("timer condition variable destroy failed");
        memset(t, 0, sizeof *t);
    }
    munlock(&tm_lock);
}

/* MMRESULT timeSetEvent(UINT delay, UINT resolution, LPTIMECALLBACK,
   DWORD_PTR user, UINT flags): returns the timer id or 0. */
void rt_timeSetEvent(_cpu *cpu)
{
    uint32_t delay = ARG(0), proc = ARG(2), user = ARG(3), flags = ARG(4);
    int i, slot = -1;
    mmtimer *t;
    if (flags & ~(TIME_PERIODIC | TIME_KILL_SYNCHRONOUS))
        rt_fatal("timeSetEvent: flags 0x%x are not supported (only callback functions)", flags);
    if (delay < 1 || delay > 1000000 || proc == 0)
    {
        RET(0);
        return;
    }
    mlock(&tm_lock);
    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (!timers[i].used)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        munlock(&tm_lock);
        RET(0);
        return;
    }
    t = &timers[slot];
    memset(t, 0, sizeof *t);
    t->used = true;
    t->id = next_timer_id++;
    if (next_timer_id == 0) next_timer_id = 1;
    t->delay_ms = delay;
    t->proc = proc;
    t->user = user;
    t->periodic = (flags & TIME_PERIODIC) != 0;
    cond_init(&t->cv);
    t->pt = start_cb_thread(timer_body, t);
    RET(t->id);
    munlock(&tm_lock);
}

/* MMRESULT timeKillEvent(UINT id) */
void rt_timeKillEvent(_cpu *cpu)
{
    uint32_t id = ARG(0);
    int i;
    mmtimer *t = NULL;
    pthread_t pt;
    mlock(&tm_lock);
    for (i = 0; i < MAX_TIMERS; i++)
    {
        if (timers[i].used && timers[i].id == id && !timers[i].stop)
        {
            t = &timers[i];
            break;
        }
    }
    if (t == NULL)
    {
        munlock(&tm_lock);
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    t->stop = true;
    if (pthread_cond_signal(&t->cv) != 0) rt_fatal("timer signal failed");
    if (pthread_equal(t->pt, pthread_self()))
    {
        /* Called from the timer's own callback: the thread ends after the
           callback returns. */
        t->self_killed = true;
        munlock(&tm_lock);
        RET(MMSYSERR_NOERROR);
        return;
    }
    pt = t->pt;
    munlock(&tm_lock);
    /* Wait for a callback that is running (Windows TIME_KILL_SYNCHRONOUS
       behavior; it is also safe without the flag). */
    if (pthread_join(pt, NULL) != 0) rt_fatal("timer thread join failed");
    mlock(&tm_lock);
    if (pthread_cond_destroy(&t->cv) != 0) rt_fatal("timer condition variable destroy failed");
    memset(t, 0, sizeof *t);
    munlock(&tm_lock);
    RET(MMSYSERR_NOERROR);
}

/* ---------------------------------------------------------------- output devices */

#define MAX_DEVICES 32

typedef struct audio_device {
    AudioObjectID id;
    char name[64];  /* Windows-1252 */
    char uid[256];  /* UTF-8 */
} audio_device;

static uint32_t cf_to_ansi(CFStringRef s, char *out, size_t cap)
{
    CFIndex used = 0, len;
    if (s == NULL)
    {
        out[0] = 0;
        return 0;
    }
    len = CFStringGetLength(s);
    CFStringGetBytes(s, CFRangeMake(0, len), kCFStringEncodingWindowsLatin1, '?', false, (UInt8 *)out, (CFIndex)cap - 1, &used);
    out[used] = 0;
    return (uint32_t)used;
}

static bool device_has_output(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr) return false;
    return size >= sizeof(AudioStreamID);
}

static void device_info(AudioObjectID dev, audio_device *d)
{
    AudioObjectPropertyAddress a = { kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFStringRef s = NULL;
    UInt32 size = sizeof s;
    d->id = dev;
    d->name[0] = 0;
    d->uid[0] = 0;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &s) == noErr && s != NULL)
    {
        cf_to_ansi(s, d->name, sizeof d->name);
        CFRelease(s);
    }
    a.mSelector = kAudioDevicePropertyDeviceUID;
    s = NULL;
    size = sizeof s;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &s) == noErr && s != NULL)
    {
        if (!CFStringGetCString(s, d->uid, sizeof d->uid, kCFStringEncodingUTF8)) d->uid[0] = 0;
        CFRelease(s);
    }
}

/* The output devices, default device first. Returns the count. */
static int list_devices(audio_device *out)
{
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID def = kAudioObjectUnknown, ids[MAX_DEVICES];
    UInt32 size = sizeof def, i;
    int n = 0;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &def) != noErr) def = kAudioObjectUnknown;
    if (def != kAudioObjectUnknown && device_has_output(def)) device_info(def, &out[n++]);
    a.mSelector = kAudioHardwarePropertyDevices;
    size = sizeof ids;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, ids) != noErr) size = 0;
    for (i = 0; i < size / sizeof ids[0] && n < MAX_DEVICES; i++)
        if (ids[i] != def && device_has_output(ids[i])) device_info(ids[i], &out[n++]);
    return n;
}

/* UINT waveOutGetNumDevs(void) */
void rt_waveOutGetNumDevs(_cpu *cpu)
{
    audio_device d[MAX_DEVICES];
    RET((uint32_t)list_devices(d));
}

/* MMRESULT waveOutGetDevCapsA(UINT_PTR id, LPWAVEOUTCAPSA, UINT size)
   WAVEOUTCAPSA (52 bytes): wMid, wPid, vDriverVersion, szPname[32],
   dwFormats, wChannels, wReserved1, dwSupport. */
void rt_waveOutGetDevCapsA(_cpu *cpu)
{
    audio_device d[MAX_DEVICES];
    uint32_t id = ARG(0), caps = ARG(1), size = ARG(2), buf[13];
    uint8_t *b = (uint8_t *)buf;
    int n = list_devices(d);
    if (caps == 0)
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    if (id == WAVE_MAPPER) id = 0;
    if (id >= (uint32_t)n)
    {
        RET(n == 0 ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID);
        return;
    }
    memset(buf, 0, sizeof buf);
    b[0] = 1;                                 /* wMid: MM_MICROSOFT */
    b[2] = 100;                               /* wPid: MM_WAVE_MAPPER */
    buf[1] = 0x0500;                          /* vDriverVersion 5.0 */
    memcpy(b + 8, d[id].name, strnlen(d[id].name, 31));
    memcpy(b + 40, &(uint32_t){ 0x000FFFFFu }, 4); /* dwFormats: every WAVE_FORMAT_* PCM format */
    b[44] = 2;                                /* wChannels */
    if (size > 52) size = 52;
    memcpy(G2H(caps), buf, size);
    RET(MMSYSERR_NOERROR);
}

/* BOOL DSEnumCallbackA(LPGUID, LPCSTR description, LPCSTR module, LPVOID) */
/* HRESULT DirectSoundEnumerateA(LPDSENUMCALLBACKA, LPVOID context)
   The first entry is the primary sound driver (GUID NULL), then each
   CoreAudio output device. A device GUID is a hash of the device UID, so
   it is the same in each session. */
void rt_DirectSoundEnumerateA(_cpu *cpu)
{
    audio_device d[MAX_DEVICES];
    uint32_t cb = ARG(0), ctx = ARG(1), g, desc, mod;
    int n = list_devices(d), i;
    if (cb == 0)
    {
        RET(0x80070057u); /* DSERR_INVALIDPARAM */
        return;
    }
    g = rt_heap_calloc(1, 16);
    desc = rt_heap_calloc(1, 64);
    mod = rt_heap_calloc(1, 4);
    if (g == 0 || desc == 0 || mod == 0) rt_fatal("out of guest memory (DirectSoundEnumerateA)");
    memcpy(G2H(desc), "Primary Sound Driver", 21);
    if (rt_call(cpu, cb, true, 0, 4, (const uint32_t[]){ 0, desc, mod, ctx }) != 0)
    {
        for (i = 0; i < n; i++)
        {
            uint64_t h1 = 0xcbf29ce484222325u, h2 = 0x84222325cbf29ce4u;
            size_t k;
            for (k = 0; d[i].uid[k] != 0; k++)
            {
                h1 = (h1 ^ (uint8_t)d[i].uid[k]) * 0x100000001b3u;
                h2 = (h2 ^ (uint8_t)d[i].uid[k]) * 0x100000001b3u + k;
            }
            h1 = (h1 & ~0x000000000000F000u) | 0x0000000000004000u; /* version 4 layout */
            h2 = (h2 & ~0xC0u) | 0x80u;                              /* variant */
            memcpy(G2H(g), &h1, 8);
            memcpy(G2H(g + 8), &h2, 8);
            memset(G2H(desc), 0, 64);
            memcpy(G2H(desc), d[i].name, strnlen(d[i].name, 63));
            if (rt_call(cpu, cb, true, 0, 4, (const uint32_t[]){ g, desc, mod, ctx }) == 0) break;
        }
    }
    rt_heap_free(g);
    rt_heap_free(desc);
    rt_heap_free(mod);
    RET(0); /* DS_OK */
}

/* ---------------------------------------------------------------- waveOut */

#define MAX_WAVEOUT 8
#define MAX_PENDING 256
#define POOL_SIZE 32
#define HWAVEOUT_BASE 0x00050000u

typedef struct wrec {
    AudioQueueBufferRef buf;
    uint32_t pwh;
} wrec;

typedef struct wout {
    bool used, eight_bit, started, thread_stop, delivering;
    uint32_t handle, cb_type, cb, instance, channels;
    AudioQueueRef aq;
    wrec pending[MAX_PENDING];
    int npending;
    uint32_t done[MAX_PENDING];
    int ndone;
    AudioQueueBufferRef pool[POOL_SIZE];
    int npool;
    pthread_t thread;
    pthread_cond_t cv;      /* work for the device thread */
    pthread_cond_t idle_cv; /* the device thread delivered all done buffers */
} wout;

static pthread_mutex_t wo_lock = PTHREAD_MUTEX_INITIALIZER;
static wout wouts[MAX_WAVEOUT];

static wout *wout_of(uint32_t h)
{
    uint32_t i;
    if (h < HWAVEOUT_BASE + 4 || (h - HWAVEOUT_BASE) % 4 != 0) return NULL;
    i = (h - HWAVEOUT_BASE) / 4 - 1;
    if (i >= MAX_WAVEOUT || !wouts[i].used) return NULL;
    return &wouts[i];
}

/* Set an event object (CALLBACK_EVENT). */
static void set_event(uint32_t h)
{
    rt_object *o = rt_handle_get(h, OBJ_EVENT);
    if (o == NULL)
    {
        rt_log("waveOut: callback event handle 0x%08x is not an event", h);
        return;
    }
    rt_obj_lock();
    o->signaled = true;
    rt_obj_signal(o, o->manual_reset ? 0u : 1u);
    rt_obj_unlock();
    rt_object_unref(o);
}

static void notify(_cpu *cpu, uint32_t cb_type, uint32_t cb, uint32_t handle, uint32_t instance, uint32_t msg, uint32_t p1)
{
    switch (cb_type)
    {
    case CALLBACK_NULL:
        break;
    case CALLBACK_FUNCTION:
        rt_call(cpu, cb, true, 0, 5, (const uint32_t[]){ handle, msg, instance, p1, 0 });
        break;
    case CALLBACK_WINDOW:
        if (!rt_post_message(cb, msg, handle, p1)) rt_log("waveOut: message 0x%x to window 0x%08x was not posted", msg, cb);
        break;
    case CALLBACK_EVENT:
        set_event(cb);
        break;
    default:
        rt_fatal("waveOut: callback type 0x%x is not supported", cb_type);
    }
}

static void wave_body(void *ctx, _cpu *cpu)
{
    wout *w = ctx;
    uint32_t list[MAX_PENDING];
    int n, i;
    mlock(&wo_lock);
    for (;;)
    {
        while (w->ndone == 0 && !w->thread_stop) cond_wait(&w->cv, &wo_lock);
        if (w->ndone == 0 && w->thread_stop) break;
        n = w->ndone;
        memcpy(list, w->done, sizeof list[0] * (size_t)n);
        w->ndone = 0;
        w->delivering = true;
        munlock(&wo_lock);
        for (i = 0; i < n; i++)
        {
            uint32_t pwh = list[i];
            wr32(pwh + 16, (rd32(pwh + 16) & ~WHDR_INQUEUE) | WHDR_DONE);
            notify(cpu, w->cb_type, w->cb, w->handle, w->instance, WOM_DONE, pwh);
        }
        mlock(&wo_lock);
        w->delivering = false;
        if (pthread_cond_broadcast(&w->idle_cv) != 0) rt_fatal("waveOut signal failed");
    }
    munlock(&wo_lock);
}

/* CoreAudio thread: a buffer finished playing. */
static void aq_done(void *ctx, AudioQueueRef aq, AudioQueueBufferRef buf)
{
    wout *w = ctx;
    int i;
    (void)aq;
    mlock(&wo_lock);
    for (i = 0; i < w->npending; i++)
    {
        if (w->pending[i].buf == buf)
        {
            w->done[w->ndone++] = w->pending[i].pwh;
            memmove(&w->pending[i], &w->pending[i + 1], sizeof w->pending[0] * (size_t)(w->npending - i - 1));
            w->npending--;
            if (pthread_cond_signal(&w->cv) != 0) rt_fatal("waveOut signal failed");
            break;
        }
    }
    /* The buffer goes back to the pool; a reset buffer (not pending) too. */
    if (w->npool < POOL_SIZE)
        w->pool[w->npool++] = buf;
    else if (AudioQueueFreeBuffer(aq, buf) != noErr)
        rt_log("waveOut: AudioQueueFreeBuffer failed");
    munlock(&wo_lock);
}

/* MMRESULT waveOutOpen(LPHWAVEOUT, UINT id, LPCWAVEFORMATEX, DWORD_PTR
   callback, DWORD_PTR instance, DWORD flags) */
void rt_waveOutOpen(_cpu *cpu)
{
    audio_device d[MAX_DEVICES];
    uint32_t phwo = ARG(0), id = ARG(1), fmt = ARG(2), cb = ARG(3), inst = ARG(4), flags = ARG(5);
    uint32_t cb_type = flags & CALLBACK_TYPEMASK, tag, ch, rate, align, bits;
    AudioStreamBasicDescription f;
    CFStringRef uid;
    int n, i, slot = -1;
    wout *w;
    OSStatus st;
    if (flags & ~(CALLBACK_TYPEMASK | WAVE_FORMAT_QUERY | WAVE_ALLOWSYNC | WAVE_MAPPED | WAVE_FORMAT_DIRECT))
    {
        RET(MMSYSERR_INVALFLAG);
        return;
    }
    if (cb_type == CALLBACK_THREAD) rt_fatal("waveOutOpen: CALLBACK_THREAD is not supported");
    if (cb_type != CALLBACK_NULL && cb_type != CALLBACK_FUNCTION && cb_type != CALLBACK_WINDOW && cb_type != CALLBACK_EVENT)
    {
        RET(MMSYSERR_INVALFLAG);
        return;
    }
    if (fmt == 0)
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    n = list_devices(d);
    if (id == WAVE_MAPPER) id = 0;
    if (id >= (uint32_t)n)
    {
        RET(n == 0 ? MMSYSERR_NODRIVER : MMSYSERR_BADDEVICEID);
        return;
    }
    tag = rd16(fmt);
    ch = rd16(fmt + 2);
    rate = rd32(fmt + 4);
    align = rd16(fmt + 12);
    bits = rd16(fmt + 14);
    if (tag != 1 || (ch != 1 && ch != 2) || (bits != 8 && bits != 16) || align != ch * bits / 8 || rate < 1000 || rate > 192000)
    {
        RET(WAVERR_BADFORMAT);
        return;
    }
    if (flags & WAVE_FORMAT_QUERY)
    {
        RET(MMSYSERR_NOERROR);
        return;
    }
    if (phwo == 0)
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    mlock(&wo_lock);
    for (i = 0; i < MAX_WAVEOUT; i++)
    {
        if (!wouts[i].used)
        {
            slot = i;
            break;
        }
    }
    if (slot < 0)
    {
        munlock(&wo_lock);
        RET(MMSYSERR_NOMEM);
        return;
    }
    w = &wouts[slot];
    memset(w, 0, sizeof *w);
    w->used = true;
    w->handle = HWAVEOUT_BASE + 4u * (uint32_t)(slot + 1);
    w->cb_type = cb_type;
    w->cb = cb;
    w->instance = inst;
    w->eight_bit = bits == 8;
    w->channels = ch;
    munlock(&wo_lock);

    memset(&f, 0, sizeof f);
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mChannelsPerFrame = ch;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 2 * ch;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 2 * ch;
    st = AudioQueueNewOutput(&f, aq_done, w, NULL, NULL, 0, &w->aq);
    if (st != noErr)
    {
        mlock(&wo_lock);
        memset(w, 0, sizeof *w);
        munlock(&wo_lock);
        rt_log("waveOutOpen: AudioQueueNewOutput failed (%d)", (int)st);
        RET(MMSYSERR_NODRIVER);
        return;
    }
    uid = CFStringCreateWithCString(NULL, d[id].uid, kCFStringEncodingUTF8);
    if (uid == NULL) rt_fatal("waveOutOpen: device UID conversion failed");
    st = AudioQueueSetProperty(w->aq, kAudioQueueProperty_CurrentDevice, &uid, sizeof uid);
    CFRelease(uid);
    if (st != noErr)
    {
        AudioQueueDispose(w->aq, true);
        mlock(&wo_lock);
        memset(w, 0, sizeof *w);
        munlock(&wo_lock);
        rt_log("waveOutOpen: selecting output device \"%s\" failed (%d)", d[id].name, (int)st);
        RET(MMSYSERR_NODRIVER);
        return;
    }
    cond_init(&w->cv);
    cond_init(&w->idle_cv);
    w->thread = start_cb_thread(wave_body, w);
    wr32(phwo, w->handle);
    notify(cpu, cb_type, cb, w->handle, inst, WOM_OPEN, 0);
    RET(MMSYSERR_NOERROR);
}

/* MMRESULT waveOutPrepareHeader(HWAVEOUT, LPWAVEHDR, UINT size)
   WAVEHDR (32 bytes): lpData, dwBufferLength, dwBytesRecorded, dwUser,
   dwFlags, dwLoops, lpNext, reserved. */
void rt_waveOutPrepareHeader(_cpu *cpu)
{
    uint32_t pwh = ARG(1);
    bool ok;
    mlock(&wo_lock);
    ok = wout_of(ARG(0)) != NULL;
    munlock(&wo_lock);
    if (!ok)
    {
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (pwh == 0 || ARG(2) < 32 || rd32(pwh) == 0)
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    if (rd32(pwh + 16) & WHDR_PREPARED)
    {
        RET(MMSYSERR_NOERROR);
        return;
    }
    if (!rt_va_is_committed(rd32(pwh), rd32(pwh + 4)))
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    wr32(pwh + 16, (rd32(pwh + 16) & ~WHDR_DONE) | WHDR_PREPARED);
    RET(MMSYSERR_NOERROR);
}

/* MMRESULT waveOutUnprepareHeader(HWAVEOUT, LPWAVEHDR, UINT size) */
void rt_waveOutUnprepareHeader(_cpu *cpu)
{
    uint32_t pwh = ARG(1);
    bool ok;
    mlock(&wo_lock);
    ok = wout_of(ARG(0)) != NULL;
    munlock(&wo_lock);
    if (!ok)
    {
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (pwh == 0 || ARG(2) < 32)
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    if (rd32(pwh + 16) & WHDR_INQUEUE)
    {
        RET(WAVERR_STILLPLAYING);
        return;
    }
    wr32(pwh + 16, rd32(pwh + 16) & ~WHDR_PREPARED);
    RET(MMSYSERR_NOERROR);
}

/* MMRESULT waveOutWrite(HWAVEOUT, LPWAVEHDR, UINT size) */
void rt_waveOutWrite(_cpu *cpu)
{
    uint32_t pwh = ARG(1), data, len, flags, need, i;
    AudioQueueBufferRef buf = NULL;
    wout *w;
    OSStatus st;
    int k;
    if (pwh == 0 || ARG(2) < 32)
    {
        RET(MMSYSERR_INVALPARAM);
        return;
    }
    flags = rd32(pwh + 16);
    if (!(flags & WHDR_PREPARED))
    {
        RET(WAVERR_UNPREPARED);
        return;
    }
    if (flags & (WHDR_BEGINLOOP | WHDR_ENDLOOP)) rt_fatal("waveOutWrite: buffer loops are not supported");
    if (flags & WHDR_INQUEUE)
    {
        RET(WAVERR_STILLPLAYING);
        return;
    }
    data = rd32(pwh);
    len = rd32(pwh + 4);
    mlock(&wo_lock);
    w = wout_of(ARG(0));
    if (w == NULL)
    {
        munlock(&wo_lock);
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (w->npending >= MAX_PENDING)
    {
        munlock(&wo_lock);
        RET(MMSYSERR_NOMEM);
        return;
    }
    len -= len % (w->eight_bit ? w->channels : 2u * w->channels);
    need = w->eight_bit ? 2u * len : len;
    for (k = 0; k < w->npool; k++)
    {
        if (w->pool[k]->mAudioDataBytesCapacity >= need)
        {
            buf = w->pool[k];
            w->pool[k] = w->pool[--w->npool];
            break;
        }
    }
    munlock(&wo_lock);
    if (buf == NULL)
    {
        st = AudioQueueAllocateBuffer(w->aq, need > 0 ? need : 2u, &buf);
        if (st != noErr)
        {
            rt_log("waveOutWrite: AudioQueueAllocateBuffer failed (%d)", (int)st);
            RET(MMSYSERR_NOMEM);
            return;
        }
    }
    if (w->eight_bit)
    {
        int16_t *o = buf->mAudioData;
        const uint8_t *s = G2H(data);
        for (i = 0; i < len; i++) o[i] = (int16_t)(((int)s[i] - 128) * 256);
    }
    else if (len > 0)
    {
        memcpy(buf->mAudioData, G2H(data), len);
    }
    buf->mAudioDataByteSize = need;
    wr32(pwh + 16, (flags & ~WHDR_DONE) | WHDR_INQUEUE);
    mlock(&wo_lock);
    w->pending[w->npending].buf = buf;
    w->pending[w->npending].pwh = pwh;
    w->npending++;
    munlock(&wo_lock);
    if (need == 0)
    {
        /* An empty buffer is done at once. */
        aq_done(w, w->aq, buf);
        RET(MMSYSERR_NOERROR);
        return;
    }
    st = AudioQueueEnqueueBuffer(w->aq, buf, 0, NULL);
    if (st != noErr) rt_fatal("waveOutWrite: AudioQueueEnqueueBuffer failed (%d)", (int)st);
    if (!w->started)
    {
        st = AudioQueueStart(w->aq, NULL);
        if (st != noErr) rt_fatal("waveOutWrite: AudioQueueStart failed (%d)", (int)st);
        w->started = true;
    }
    RET(MMSYSERR_NOERROR);
}

/* Wait until the device thread delivered every done buffer (lock held). */
static void wait_idle(wout *w)
{
    while (w->ndone > 0 || w->delivering) cond_wait(&w->idle_cv, &wo_lock);
}

/* MMRESULT waveOutReset(HWAVEOUT): stop playback; every queued buffer is
   marked done and returned to the application. */
void rt_waveOutReset(_cpu *cpu)
{
    wout *w;
    OSStatus st;
    int i;
    mlock(&wo_lock);
    w = wout_of(ARG(0));
    if (w == NULL)
    {
        munlock(&wo_lock);
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (pthread_equal(w->thread, pthread_self()))
        rt_fatal("waveOutReset was called from a waveOut callback (Windows deadlocks in this case)");
    munlock(&wo_lock);
    st = AudioQueueStop(w->aq, true);
    if (st != noErr) rt_fatal("waveOutReset: AudioQueueStop failed (%d)", (int)st);
    mlock(&wo_lock);
    w->started = false;
    for (i = 0; i < w->npending; i++) w->done[w->ndone++] = w->pending[i].pwh;
    w->npending = 0;
    if (pthread_cond_signal(&w->cv) != 0) rt_fatal("waveOut signal failed");
    wait_idle(w);
    munlock(&wo_lock);
    RET(MMSYSERR_NOERROR);
}

/* MMRESULT waveOutClose(HWAVEOUT) */
void rt_waveOutClose(_cpu *cpu)
{
    wout *w;
    uint32_t cb_type, cb, handle, inst;
    AudioQueueRef aq;
    pthread_t thread;
    mlock(&wo_lock);
    w = wout_of(ARG(0));
    if (w == NULL)
    {
        munlock(&wo_lock);
        RET(MMSYSERR_INVALHANDLE);
        return;
    }
    if (pthread_equal(w->thread, pthread_self()))
        rt_fatal("waveOutClose was called from a waveOut callback (Windows deadlocks in this case)");
    if (w->npending > 0)
    {
        munlock(&wo_lock);
        RET(WAVERR_STILLPLAYING);
        return;
    }
    wait_idle(w);
    w->thread_stop = true;
    if (pthread_cond_signal(&w->cv) != 0) rt_fatal("waveOut signal failed");
    thread = w->thread;
    munlock(&wo_lock);
    if (pthread_join(thread, NULL) != 0) rt_fatal("waveOut thread join failed");
    mlock(&wo_lock);
    aq = w->aq;
    cb_type = w->cb_type;
    cb = w->cb;
    handle = w->handle;
    inst = w->instance;
    munlock(&wo_lock);
    /* AudioQueueDispose also frees the buffers of the pool. */
    if (AudioQueueDispose(aq, true) != noErr) rt_log("waveOutClose: AudioQueueDispose failed");
    mlock(&wo_lock);
    if (pthread_cond_destroy(&w->cv) != 0 || pthread_cond_destroy(&w->idle_cv) != 0)
        rt_fatal("waveOut condition variable destroy failed");
    memset(w, 0, sizeof *w);
    munlock(&wo_lock);
    notify(cpu, cb_type, cb, handle, inst, WOM_CLOSE, 0);
    RET(MMSYSERR_NOERROR);
}
