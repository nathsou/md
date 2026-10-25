// movie::Decoder (nnMedia) on FFmpeg: an Android MediaCodec-style API. Adapted from LASRecomp's movie.c.
//
// Dread's movies are VP9 WebM without audio (romfs/system/video, plus cutscene inserts). The SDK's movie library
// (subsdk0) is recompiled and runs as is - its Extractor (libwebm) demuxes, its Buffer and MediaData are the real
// objects - except the Decoder, whose implementation drives the Switch's NVDEC hardware: every Decoder method is
// replaced here, by name for the game's direct calls (Start, Configure, ... are imports of main) and through our
// own vtable for the virtual ones (the Acquire* calls are virtual only). The game owns the Decoder object
// (vtable at +0); we keep host state at +8.
//
// Decoder vtable slots (subsdk0's _ZTVN5movie7DecoderE): 0 ~Decoder (D1), 1 ~Decoder (D0), 2 Configure,
//   3 RegisterClientEvents, 4 GetOutputFormat, 5 Start, 6 Stop, 7 Reset, 8 Flush, 9 AcquireInputBufferIndex,
//   10 GetInputBuffer, 11 SendInputBufferForDecode, 12 AcquireOutputBufferIndex, 13 GetOutputBuffer,
//   14 GetOutputBufferSize, 15 ReleaseOutputBufferIndex, 16 GetLastError, 17 GetOutputBuffer(+MediaData),
//   18 GetOutputBufferFormat.
//
// Output is in native-texture mode (DecoderMode 2): the client's Buffer carries a guest NVNtexture as its data
// (capacity 8) and an NVNsync (SetNvnSync); a frame is uploaded into that texture and the sync signalled.
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"

#define MOVIE_EOS 4u

// movie::Buffer (subsdk0's accessors): +0 offset, +4 size, +0x10 owns its memory, +0x18 capacity, +0x28 data,
// +0x30 NVN device, +0x38 NVN sync
typedef struct {
    int32_t offset, size;
    uint64_t _8;
    uint8_t owned, _11[7];
    uint64_t capacity, _20, base, nvn_dev, nvn_sync;
} MBuffer;
_Static_assert(offsetof(MBuffer, capacity) == 0x18 && offsetof(MBuffer, base) == 0x28 &&
               offsetof(MBuffer, nvn_sync) == 0x38, "movie::Buffer layout");
#define BUF(a) ((MBuffer *)(uintptr_t)(a))

// ------------------------------------------------------------------ guest calls
// A guest function with up to four integer arguments, from inside an HLE call: the registers the guest's caller
// expects preserved are callee-saved, so only x0-x3 change here (our own result goes to x0 afterwards anyway).
static uint64_t gcall(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    c->x[0] = a0, c->x[1] = a1, c->x[2] = a2, c->x[3] = a3;
    guest_call(c, fn);
    return c->x[0];
}
static bool md_find_int(Cpu *c, uint64_t md, const char *key, int32_t *out) {
    return md && (gcall(c, guest_sym("_ZNK5movie9MediaData9FindInt32EPKcPi"), md, (uintptr_t)key, (uintptr_t)out, 0) & 1);
}
static const char *md_find_str(Cpu *c, uint64_t md, const char *key) {
    const char *s = NULL;
    if (!md || !(gcall(c, guest_sym("_ZNK5movie9MediaData10FindStringEPKcPS2_"), md, (uintptr_t)key, (uintptr_t)&s, 0) & 1))
        return NULL;
    return s;
}
static void md_set_int(Cpu *c, uint64_t md, const char *key, int32_t v) {
    gcall(c, guest_sym("_ZN5movie9MediaData8SetInt32EPKci"), md, (uintptr_t)key, (uint32_t)v, 0);
}
static void md_set_str(Cpu *c, uint64_t md, const char *key, const char *v) {
    gcall(c, guest_sym("_ZN5movie9MediaData9SetStringEPKcS2_"), md, (uintptr_t)key, (uintptr_t)v, 0);
}

static void signal_guest_event(Cpu *c, uint64_t event) {
    if (!event) return;
    uint64_t saved[4];
    memcpy(saved, c->x, sizeof saved);
    gcall(c, guest_sym("_ZN2nn2os11SignalEventEPNS0_9EventTypeE"), event, 0, 0, 0);
    memcpy(c->x, saved, sizeof saved);
}

// ------------------------------------------------------------------ Decoder
#define NIN 4
#define NOUT 6
#define NPENDING 16

typedef struct { int idx; int64_t time; uint32_t flags; } OutReady;

typedef struct {
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    // The game's DecoderEvents (input available, output available, format changed, error): Dread overwrites the
    // struct with its next movie's, so each signal reads the entry through the registered pointer (as the SDK does),
    // falling back to the value at registration when the entry isn't a plausible pointer (nxnative's finding)
    uint64_t events_ptr, events[4];
    uint8_t *in_mem[NIN];
    uint32_t in_cap;
    bool in_busy[NIN];
    uint8_t *out_mem[NOUT];
    uint32_t out_cap, out_size[NOUT];
    bool out_busy[NOUT];
    OutReady ready[NOUT];
    int nready;
    int width, height, stride;
    struct SwsContext *sws;
    uint8_t *rgba;
    AVFrame *pending[NPENDING];
    int64_t pending_time[NPENDING];
    int npending;
    bool eos_pending;
    int64_t eos_time;
    bool stopped;  // Stop / Flush / destroyed: no indices handed out, no more events
    pthread_mutex_t lock;
} Decoder;

static Decoder *self_state(Cpu *c) { return (Decoder *)(uintptr_t)((uint64_t *)(uintptr_t)c->x[0])[1]; }

static uint64_t dec_event(Decoder *d, int i) {
    if (d->events_ptr) {
        uint64_t v = ((const uint64_t *)(uintptr_t)d->events_ptr)[i];
        if (v >= GUEST_LO && v < GUEST_HI && !(v & 7)) return v;
    }
    return d->events[i];
}

// Signal one of the game's decoder events unless the decoder is stopped (under the lock: a Stop on another thread
// can't slip between the check and the signal).
static void dec_signal(Cpu *c, Decoder *d, int i) {
    pthread_mutex_lock(&d->lock);
    if (!d->stopped) signal_guest_event(c, dec_event(d, i));
    pthread_mutex_unlock(&d->lock);
}

// Input / output buffers live in the HLE heap (guest-visible: the game reads input buffers through movie::Buffer),
// which only a reboot frees: buffers of destroyed decoders are kept for the next movie.
static struct { uint8_t *mem; uint32_t size; } g_buf_pool[64];
static int g_nbuf_pool;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *buf_get(uint32_t size) {
    pthread_mutex_lock(&g_pool_lock);
    for (int i = 0; i < g_nbuf_pool; i++)
        if (g_buf_pool[i].size == size) {
            uint8_t *m = g_buf_pool[i].mem;
            g_buf_pool[i] = g_buf_pool[--g_nbuf_pool];
            pthread_mutex_unlock(&g_pool_lock);
            return m;
        }
    pthread_mutex_unlock(&g_pool_lock);
    return hle_alloc(size);
}
static void buf_put(uint8_t *m, uint32_t size) {
    if (!m) return;
    pthread_mutex_lock(&g_pool_lock);
    if (g_nbuf_pool < 64) g_buf_pool[g_nbuf_pool].mem = m, g_buf_pool[g_nbuf_pool++].size = size;
    pthread_mutex_unlock(&g_pool_lock);
}

typedef struct NvnTexture NvnTexture;
void renderer_upload_rgba(NvnTexture *t, const void *rgba, uint32_t w, uint32_t h, uint32_t stride);
bool renderer_upload_nv12(NvnTexture *t, const void *nv12, uint32_t w, uint32_t h, uint32_t stride, uint32_t uv_off);

static void ok(Cpu *c) { c->x[0] = 0; }

static void dec_configure(Cpu *c) {
    Decoder *d = self_state(c);
    uint64_t md = c->x[1];
    const char *mime = md_find_str(c, md, "mime");
    int32_t w = 0, h = 0;
    md_find_int(c, md, "width", &w);
    md_find_int(c, md, "height", &h);
    enum AVCodecID id = !mime ? AV_CODEC_ID_VP9 : strstr(mime, "vp9") ? AV_CODEC_ID_VP9 : strstr(mime, "vp8") ? AV_CODEC_ID_VP8
                      : strstr(mime, "avc") ? AV_CODEC_ID_H264 : AV_CODEC_ID_NONE;
    const AVCodec *codec = id != AV_CODEC_ID_NONE ? avcodec_find_decoder(id) : NULL;
    if (!codec || w <= 0 || h <= 0) {
        LOG("[movie] Configure: unsupported track (mime %s, %dx%d)\n", mime ? mime : "?", w, h);
        c->x[0] = 1;
        return;
    }
    d->ctx = avcodec_alloc_context3(codec);
    d->ctx->width = w, d->ctx->height = h;
    d->ctx->thread_count = 4;
    if (avcodec_open2(d->ctx, codec, NULL) < 0) { LOG("[movie] cannot open decoder %s\n", codec->name); c->x[0] = 1; return; }
    d->frame = av_frame_alloc();
    d->pkt = av_packet_alloc();
    d->width = w, d->height = h;
    d->stride = (w + 255) & ~255;
    d->in_cap = 4 << 20;
    d->out_cap = (uint32_t)(d->stride * ((h + 15) & ~15) * 3 / 2);
    for (int i = 0; i < NIN; i++) if (!d->in_mem[i]) d->in_mem[i] = buf_get(d->in_cap);
    for (int i = 0; i < NOUT; i++) if (!d->out_mem[i]) d->out_mem[i] = buf_get(d->out_cap);
    LOG("[movie] decoder %s configured: %dx%d (%s)\n", codec->name, w, h, mime ? mime : "no mime");
    c->x[0] = 0;
}

static void dec_register_events(Cpu *c) {
    Decoder *d = self_state(c);
    d->events_ptr = c->x[1];
    if (d->events_ptr) memcpy(d->events, (void *)(uintptr_t)d->events_ptr, sizeof d->events);
    c->x[0] = 0;
}

static void dec_output_format(Cpu *c, Decoder *d, uint64_t md) {
    if (!md) return;
    gcall(c, guest_sym("_ZN5movie9MediaData5ClearEv"), md, 0, 0, 0);
    md_set_str(c, md, "mime", "video/raw");
    md_set_int(c, md, "width", d->width);
    md_set_int(c, md, "height", d->height);
    md_set_int(c, md, "stride", d->stride);
    md_set_int(c, md, "slice-height", (d->height + 15) & ~15);
    md_set_int(c, md, "crop-left", 0);
    md_set_int(c, md, "crop-top", 0);
    md_set_int(c, md, "crop-right", d->width - 1);
    md_set_int(c, md, "crop-bottom", d->height - 1);
}
static void dec_get_output_format(Cpu *c) {
    uint64_t md = c->x[1];
    dec_output_format(c, self_state(c), md);
    c->x[0] = 0;
}
static void dec_get_output_buffer_format(Cpu *c) {
    uint64_t md = c->x[2];
    dec_output_format(c, self_state(c), md);
    c->x[0] = 0;
}

static void dec_start(Cpu *c) {
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    d->stopped = false;
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);  // every input buffer is free
    c->x[0] = 0;
}

// Stop and Flush (the game ends a movie with Flush, Flush, Stop, destroy, and calls Start after a Flush to loop):
// nothing is signalled or handed out until the next Start, and what is set stays cleared. A stopped hardware
// decoder signals nothing; one of our events left set would wake the game's decoder thread into a decoder it has
// already deleted.
static void dec_halt(Cpu *c, Decoder *d, const char *why) {
    static uint64_t clear_event;
    if (!clear_event) clear_event = guest_sym("_ZN2nn2os10ClearEventEPNS0_9EventTypeE");
    pthread_mutex_lock(&d->lock);
    if (!d->stopped) {
        d->stopped = true;
        uint64_t saved[4];
        memcpy(saved, c->x, sizeof saved);
        for (int i = 0; i < 4; i++)
            if (dec_event(d, i)) gcall(c, clear_event, dec_event(d, i), 0, 0, 0);
        memcpy(c->x, saved, sizeof saved);
    }
    // Flush drops what is in flight: queued frames, ready outputs, inputs not yet decoded
    for (int i = 0; i < d->npending; i++) av_frame_free(&d->pending[i]);
    d->npending = d->nready = 0;
    d->eos_pending = false;
    for (int i = 0; i < NIN; i++) d->in_busy[i] = false;
    for (int i = 0; i < NOUT; i++) d->out_busy[i] = false;
    if (d->ctx) avcodec_flush_buffers(d->ctx);
    pthread_mutex_unlock(&d->lock);
    if (ENV("LARECOMP_LOG_MOVIE")) LOG("[movie] decoder %s\n", why);
}
static void dec_stop(Cpu *c) { dec_halt(c, self_state(c), "stopped"); c->x[0] = 0; }
static void dec_flush(Cpu *c) { dec_halt(c, self_state(c), "flushed"); c->x[0] = 0; }

static void dec_acquire_input(Cpu *c) {
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    int out_free = 0;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) out_free++;
    if (d->stopped || out_free < 2 || d->npending) { pthread_mutex_unlock(&d->lock); c->x[0] = 1; return; }  // backpressure
    int idx = -1, free_n = 0;
    for (int i = 0; i < NIN; i++)
        if (!d->in_busy[i]) { if (idx < 0) idx = i; else free_n++; }
    if (idx >= 0) d->in_busy[idx] = true;
    pthread_mutex_unlock(&d->lock);
    if (idx < 0) { c->x[0] = 1; return; }
    *(int32_t *)(uintptr_t)c->x[1] = idx;
    if (c->x[2]) *(int32_t *)(uintptr_t)c->x[2] = free_n;
    c->x[0] = 0;
}

static void dec_get_input_buffer(Cpu *c) {
    Decoder *d = self_state(c);
    int i = (int)c->x[1];
    MBuffer *b = BUF(c->x[2]);
    if (i < 0 || i >= NIN || !b) { c->x[0] = 1; return; }
    b->offset = b->size = 0;
    b->owned = 0;  // (ours: the Buffer's destructor must not free it)
    b->base = (uint64_t)(uintptr_t)d->in_mem[i];
    b->capacity = d->in_cap;
    c->x[0] = 0;
}

// A decoded frame into a free output slot as NV12; the caller holds the lock.
static bool emit_frame(Decoder *d, AVFrame *f, int64_t time, uint32_t flags) {
    int slot = -1;
    for (int i = 0; i < NOUT; i++) if (!d->out_busy[i]) { slot = i; break; }
    if (slot < 0) return false;
    uint8_t *o = d->out_mem[slot];
    uint32_t size = 0;
    if (!(flags & MOVIE_EOS)) {
        int h = d->height, w = d->width, uv_off = d->stride * ((h + 15) & ~15);
        for (int y = 0; y < h; y++) memcpy(o + y * d->stride, f->data[0] + y * f->linesize[0], w);
        for (int y = 0; y < h / 2; y++) {  // interleave U and V
            uint8_t *uv = o + uv_off + y * d->stride;
            const uint8_t *u = f->data[1] + y * f->linesize[1], *v = f->data[2] + y * f->linesize[2];
            for (int x = 0; x < w / 2; x++) { uv[2 * x] = u[x]; uv[2 * x + 1] = v[x]; }
        }
        size = (uint32_t)(uv_off + d->stride * (h / 2));
    }
    d->out_busy[slot] = true;
    d->out_size[slot] = size;
    d->ready[d->nready++] = (OutReady){slot, time, flags};
    return true;
}

static bool flush_pending(Decoder *d) {
    bool any = false;
    while (d->npending && emit_frame(d, d->pending[0], d->pending_time[0], 0)) {
        av_frame_free(&d->pending[0]);
        memmove(d->pending, d->pending + 1, (d->npending - 1) * sizeof d->pending[0]);
        memmove(d->pending_time, d->pending_time + 1, (d->npending - 1) * sizeof d->pending_time[0]);
        d->npending--;
        any = true;
    }
    if (!d->npending && d->eos_pending && emit_frame(d, NULL, d->eos_time, MOVIE_EOS)) {
        d->eos_pending = false;
        any = true;
    }
    return any;
}

static void dec_send_input(Cpu *c) {
    Decoder *d = self_state(c);
    int idx = (int)c->x[1], off = (int)c->x[2], size = (int)c->x[3];
    int64_t time = (int64_t)c->x[4];
    uint32_t flags = (uint32_t)c->x[5];
    pthread_mutex_lock(&d->lock);
    if (d->ctx && size > 0 && idx >= 0 && idx < NIN) {
        av_packet_unref(d->pkt);
        d->pkt->data = d->in_mem[idx] + off;
        d->pkt->size = size;
        d->pkt->pts = time;
        avcodec_send_packet(d->ctx, d->pkt);
    }
    if (d->ctx && (flags & MOVIE_EOS)) avcodec_send_packet(d->ctx, NULL);
    bool produced = false;
    while (d->ctx && avcodec_receive_frame(d->ctx, d->frame) == 0) {
        int64_t t = d->frame->pts != AV_NOPTS_VALUE ? d->frame->pts : time;
        if (d->npending || !emit_frame(d, d->frame, t, 0)) {  // (in order: behind anything already queued)
            if (d->npending < NPENDING) {
                d->pending[d->npending] = av_frame_clone(d->frame);
                d->pending_time[d->npending++] = t;
            } else {
                LOG("[movie] output queue full, dropping a frame\n");
            }
        }
        produced = true;
    }
    if (flags & MOVIE_EOS) { d->eos_pending = true; d->eos_time = time; }
    produced |= flush_pending(d);
    if (idx >= 0 && idx < NIN) d->in_busy[idx] = false;
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);
    if (produced) dec_signal(c, d, 1);
    c->x[0] = 0;
}

static void dec_acquire_output(Cpu *c) {
    Decoder *d = self_state(c);
    pthread_mutex_lock(&d->lock);
    if (d->stopped || !d->nready) { pthread_mutex_unlock(&d->lock); c->x[0] = 1; return; }
    OutReady r = d->ready[0];
    memmove(d->ready, d->ready + 1, --d->nready * sizeof *d->ready);
    int remaining = d->nready;
    pthread_mutex_unlock(&d->lock);
    *(int32_t *)(uintptr_t)c->x[1] = r.idx;
    if (c->x[2]) *(int64_t *)(uintptr_t)c->x[2] = r.time;
    if (c->x[3]) *(uint32_t *)(uintptr_t)c->x[3] = r.flags;
    if (c->x[4]) *(int32_t *)(uintptr_t)c->x[4] = remaining;
    c->x[0] = 0;
}

static void dec_get_output_buffer(Cpu *c) {
    Decoder *d = self_state(c);
    int i = (int)c->x[1];
    MBuffer *b = BUF(c->x[2]);
    if (i < 0 || i >= NOUT || !b) { c->x[0] = 1; return; }
    if (b->base && b->capacity == 8) {
        // native-texture mode: the buffer's data is the game's NVNtexture (its host object at +0)
        NvnTexture *t = *(NvnTexture **)(uintptr_t)b->base;
        int w = d->width, h = d->height;
        if (t && d->out_size[i]) {
            uint32_t uv_off = (uint32_t)(d->stride * ((h + 15) & ~15));
            if (!renderer_upload_nv12(t, d->out_mem[i], (uint32_t)w, (uint32_t)h, (uint32_t)d->stride, uv_off)) {
                if (!d->sws) d->sws = sws_getContext(w, h, AV_PIX_FMT_NV12, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
                if (!d->rgba) d->rgba = malloc((size_t)w * h * 4);
                const uint8_t *src[2] = {d->out_mem[i], d->out_mem[i] + uv_off};
                int src_stride[2] = {d->stride, d->stride};
                uint8_t *dst[1] = {d->rgba};
                int dst_stride[1] = {w * 4};
                sws_scale(d->sws, src, src_stride, 0, h, dst, dst_stride);
                renderer_upload_rgba(t, d->rgba, (uint32_t)w, (uint32_t)h, (uint32_t)w * 4);
            }
        }
        if (b->nvn_sync) {  // (the frame is the texture's now: the game may sample it)
            extern void nvn_sync_signal(uint64_t guest);  // runtime/nvn/objects.c
            nvn_sync_signal(b->nvn_sync);
        }
        c->x[0] = 0;
        return;
    }
    b->offset = 0;
    b->owned = 0;
    b->base = (uint64_t)(uintptr_t)d->out_mem[i];
    b->capacity = d->out_cap;
    b->size = (int32_t)d->out_size[i];
    c->x[0] = 0;
}

static void dec_release_output(Cpu *c) {
    Decoder *d = self_state(c);
    int i = (int)c->x[1];
    pthread_mutex_lock(&d->lock);
    if (i >= 0 && i < NOUT) d->out_busy[i] = false;
    bool produced = flush_pending(d);
    pthread_mutex_unlock(&d->lock);
    dec_signal(c, d, 0);  // input may be accepted again
    if (produced) dec_signal(c, d, 1);
    c->x[0] = 0;
}

static void dec_get_output_buffer_size(Cpu *c) {  // (int* count, int* size)
    Decoder *d = self_state(c);
    if (c->x[1]) *(int32_t *)(uintptr_t)c->x[1] = NOUT;
    if (c->x[2]) *(int32_t *)(uintptr_t)c->x[2] = (int32_t)d->out_cap;
    c->x[0] = 0;
}

// Movie objects of this boot: a reboot mid-movie frees their host state.
#define MAX_MOVIE_OBJS 32
static Decoder *g_decoders[MAX_MOVIE_OBJS];
static pthread_mutex_t g_movie_lock = PTHREAD_MUTEX_INITIALIZER;
static void movie_track(Decoder *d, bool add) {
    pthread_mutex_lock(&g_movie_lock);
    for (int i = 0; i < MAX_MOVIE_OBJS; i++)
        if (g_decoders[i] == (add ? NULL : d)) { g_decoders[i] = add ? d : NULL; break; }
    pthread_mutex_unlock(&g_movie_lock);
}

static void dec_free(Decoder *d) {
    if (d->ctx) avcodec_free_context(&d->ctx);
    for (int i = 0; i < d->npending; i++) av_frame_free(&d->pending[i]);
    d->npending = d->nready = 0;
    av_frame_free(&d->frame);
    av_packet_free(&d->pkt);
    if (d->sws) sws_freeContext(d->sws), d->sws = NULL;
    free(d->rgba), d->rgba = NULL;
}

// ~Decoder (D1, the complete-object destructor the game calls itself; D2 is the same here)
static void dec_dtor(Cpu *c) {
    Decoder *d = self_state(c);
    if (!d) { c->x[0] = 0; return; }
    dec_halt(c, d, "destroyed");
    pthread_mutex_lock(&d->lock);
    dec_free(d);
    for (int i = 0; i < NIN; i++) buf_put(d->in_mem[i], d->in_cap), d->in_mem[i] = NULL;
    for (int i = 0; i < NOUT; i++) buf_put(d->out_mem[i], d->out_cap), d->out_mem[i] = NULL;
    pthread_mutex_unlock(&d->lock);
    movie_track(d, false);  // (d itself stays: the game's object may still point at it)
    c->x[0] = 0;
}
// ~Decoder (D0, deleting): the destructor, then the object's memory back through the SDK's operator delete
static void dec_dtor_delete(Cpu *c) {
    uint64_t self = c->x[0];
    dec_dtor(c);
    gcall(c, guest_sym("_ZdlPv"), self, 0, 0, 0);
    c->x[0] = 0;
}

static void dec_unknown(Cpu *c) {
    static int n;
    if (n++ < 20) LOG("[movie] unimplemented Decoder virtual (lr=%llx)\n", (unsigned long long)c->x[30]);
    c->x[0] = 0;
}

RT_ON_RESET(movie_reset) {
    for (int i = 0; i < MAX_MOVIE_OBJS; i++)
        if (g_decoders[i]) dec_free(g_decoders[i]), g_decoders[i] = NULL;
    g_nbuf_pool = 0;  // (the HLE heap starts over)
}

static uint64_t g_dec_vt;
RT_ON_RESET(movie_vt_reset) { g_dec_vt = 0; }  // (it lives in the HLE heap)

static void dec_construct(Cpu *c) {
    if (!g_dec_vt) {
        static const HostFn f[19] = {dec_dtor, dec_dtor_delete, dec_configure, dec_register_events,
                                     dec_get_output_format, dec_start, dec_stop, ok, dec_flush, dec_acquire_input,
                                     dec_get_input_buffer, dec_send_input, dec_acquire_output, dec_get_output_buffer,
                                     dec_get_output_buffer_size, dec_release_output, ok, dec_get_output_buffer,
                                     dec_get_output_buffer_format};
        uint64_t *t = hle_alloc(32 * 8);
        char name[64];
        for (int i = 0; i < 32; i++) {
            snprintf(name, sizeof name, "<movie::Decoder slot %d>", i);
            t[i] = hle_register(name, i < 19 ? f[i] : dec_unknown);
        }
        g_dec_vt = (uint64_t)(uintptr_t)t;
    }
    uint64_t *self = (uint64_t *)(uintptr_t)c->x[0];
    Decoder *d = calloc(1, sizeof *d);
    pthread_mutex_init(&d->lock, NULL);
    d->stopped = true;  // (until Start)
    movie_track(d, true);
    self[0] = g_dec_vt;
    self[1] = (uint64_t)(uintptr_t)d;
    LOG("[movie] decoder created (type %d, output format %d, mode %d)\n", (int)c->x[1], (int)c->x[2], (int)c->x[3]);
}

// The game's direct calls (imports of main), by name
HLE_FUNC("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", movie_decoder_c1) { dec_construct(c); }
HLE_FUNC("_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", movie_decoder_c2) { dec_construct(c); }
HLE_FUNC("_ZN5movie7DecoderD1Ev", movie_decoder_d1) { dec_dtor(c); }
HLE_FUNC("_ZN5movie7DecoderD2Ev", movie_decoder_d2) { dec_dtor(c); }
HLE_FUNC("_ZN5movie7DecoderD0Ev", movie_decoder_d0) { dec_dtor_delete(c); }
HLE_FUNC("_ZN5movie7Decoder9ConfigureEPNS_9MediaDataE", movie_decoder_configure) { dec_configure(c); }
HLE_FUNC("_ZN5movie7Decoder20RegisterClientEventsEPNS_13DecoderEventsE", movie_decoder_events) { dec_register_events(c); }
HLE_FUNC("_ZN5movie7Decoder15GetOutputFormatEPNS_9MediaDataE", movie_decoder_out_format) { dec_get_output_format(c); }
HLE_FUNC("_ZN5movie7Decoder5StartEv", movie_decoder_start) { dec_start(c); }
HLE_FUNC("_ZN5movie7Decoder4StopEv", movie_decoder_stop) { dec_stop(c); }
HLE_FUNC("_ZN5movie7Decoder5ResetEv", movie_decoder_reset) { ok(c); }
HLE_FUNC("_ZN5movie7Decoder5FlushEv", movie_decoder_flush) { dec_flush(c); }
HLE_FUNC("_ZN5movie7Decoder23AcquireInputBufferIndexEPiS1_", movie_decoder_acq_in) { dec_acquire_input(c); }
HLE_FUNC("_ZN5movie7Decoder14GetInputBufferEiPNS_6BufferE", movie_decoder_get_in) { dec_get_input_buffer(c); }
HLE_FUNC("_ZN5movie7Decoder24SendInputBufferForDecodeEiiilj", movie_decoder_send) { dec_send_input(c); }
HLE_FUNC("_ZN5movie7Decoder24AcquireOutputBufferIndexEPiPlPjS1_", movie_decoder_acq_out) { dec_acquire_output(c); }
HLE_FUNC("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferE", movie_decoder_get_out) { dec_get_output_buffer(c); }
HLE_FUNC("_ZN5movie7Decoder15GetOutputBufferEiPNS_6BufferEPNS_9MediaDataE", movie_decoder_get_out_md) { dec_get_output_buffer(c); }
HLE_FUNC("_ZN5movie7Decoder19GetOutputBufferSizeEPiS1_", movie_decoder_out_size) { dec_get_output_buffer_size(c); }
HLE_FUNC("_ZN5movie7Decoder24ReleaseOutputBufferIndexEi", movie_decoder_release) { dec_release_output(c); }
HLE_FUNC("_ZN5movie7Decoder12GetLastErrorEv", movie_decoder_last_error) { ok(c); }
HLE_FUNC("_ZN5movie7Decoder21GetOutputBufferFormatEiPNS_9MediaDataE", movie_decoder_buf_format) { dec_get_output_buffer_format(c); }
