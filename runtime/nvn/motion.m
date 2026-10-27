// Motion vectors (motion.h): the shader variants and the per-draw history of vertex constant buffers.
#import <Metal/Metal.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../settings.h"
#include "motion.h"

bool mv_enabled(void) {  // (LARECOMP_MV=0 / 1: instead of frame generation's need)
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_MV") ? atoi(getenv("LARECOMP_MV")) != 0 : -1;
    extern bool framegen_wants_motion(void);
    bool on = env >= 0 ? env : framegen_wants_motion();
    static bool was;
    if (on != was) LOG("[mv] motion vectors %s\n", on ? "on" : "off");
    was = on;
    return on;
}

// ---------------------------------------------------------------- sources
// The translator's entry points are one line each (maxwell/translate.c): `vertex VOut main0(<params>) {` ending with
// `  return out;\n}\n`, `fragment FOut main0(...)` ending with `  return o;\n}\n`; the structs start
// `struct VOut {\n  float4 pos [[position, invariant]];\n` and `struct FIn {\n  float4 pos [[position]];\n`.

typedef struct { char *buf; size_t n, cap; } Buf;
static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->buf = realloc(b->buf, b->cap); }
    memcpy(b->buf + b->n, s, n);
    b->n += n;
    b->buf[b->n] = 0;
}
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }

// s with `after` (which must occur once) followed by `ins`
static char *insert_after(const char *s, const char *after, const char *ins) {
    const char *p = strstr(s, after);
    if (!p || strstr(p + 1, after)) return NULL;
    Buf b = {0};
    put(&b, s, (size_t)(p - s) + strlen(after));
    puts_(&b, ins);
    puts_(&b, p + strlen(after));
    return b.buf;
}

// The parameters of an entry line (between "main0(" and ") {"), split at top-level commas: each with and without its
// [[attribute]], and its name.
typedef struct { char full[192], bare[160], name[64]; } Param;
static int split_params(const char *open, const char *close, Param *ps, int max) {
    int n = 0, depth = 0;
    const char *a = open;
    for (const char *p = open; p <= close && n < max; p++) {
        if (p < close && *p == '<') depth++;
        if (p < close && *p == '>') depth--;
        if (p < close && !(*p == ',' && depth == 0)) continue;
        while (a < p && *a == ' ') a++;
        size_t len = (size_t)(p - a);
        if (!len || len >= sizeof ps[n].full) return -1;
        memcpy(ps[n].full, a, len);
        ps[n].full[len] = 0;
        const char *at = strstr(ps[n].full, " [[");
        size_t blen = at ? (size_t)(at - ps[n].full) : len;
        memcpy(ps[n].bare, ps[n].full, blen);
        ps[n].bare[blen] = 0;
        const char *nm = ps[n].bare + blen;
        while (nm > ps[n].bare && (nm[-1] == '_' || (nm[-1] >= '0' && nm[-1] <= '9') || (nm[-1] >= 'a' && nm[-1] <= 'z') ||
                                   (nm[-1] >= 'A' && nm[-1] <= 'Z')))
            nm--;
        snprintf(ps[n].name, sizeof ps[n].name, "%s", nm);
        n++;
        a = p + 1;
    }
    return n;
}

char *mv_vs_msl(const char *msl, uint32_t cbuf_mask, int arena_slot, int off_slot) {
    (void)cbuf_mask;
    static const char head[] = "vertex VOut main0(", tail[] = "  return out;\n}\n";
    const char *entry = strstr(msl, head);
    if (!entry || strstr(entry + 1, head)) return NULL;
    const char *open = entry + strlen(head), *close = strstr(open, ") {\n");
    const char *end = strstr(entry, tail);
    if (!close || !end || end[strlen(tail)] != 0) return NULL;  // (the entry is the last function)
    Param ps[64];
    int n = split_params(open, close, ps, 64);
    if (n <= 0) return NULL;
    char *s1 = insert_after(msl, "struct VOut {\n  float4 pos [[position, invariant]];\n",
                            "  float4 mvc [[user(mvc)]];\n  float4 mvp [[user(mvp)]];\n");
    if (!s1) return NULL;
    // the game's code as a function of its parameters, called twice by the new entry
    entry = strstr(s1, head);
    open = entry + strlen(head);
    close = strstr(open, ") {\n");
    Buf b = {0};
    put(&b, s1, (size_t)(entry - s1));
    puts_(&b, "static VOut mv_body(");
    for (int i = 0; i < n; i++) { if (i) puts_(&b, ", "); puts_(&b, ps[i].bare); }
    puts_(&b, close);  // ") {\n" and the body
    char tmp[256];
    puts_(&b, "vertex VOut main0(");
    for (int i = 0; i < n; i++) { if (i) puts_(&b, ", "); puts_(&b, ps[i].full); }
    snprintf(tmp, sizeof tmp, ", constant uchar *mv_arena [[buffer(%d)]], constant uint *mv_off [[buffer(%d)]]) {\n",
             arena_slot, off_slot);
    puts_(&b, tmp);
    for (int pass = 0; pass < 2; pass++) {
        puts_(&b, pass ? "  VOut prev = mv_body(" : "  VOut out = mv_body(");
        for (int i = 0; i < n; i++) {
            if (i) puts_(&b, ", ");
            int k;
            if (pass && sscanf(ps[i].name, "cb%d", &k) == 1 && k >= 3 && k < 18) {
                snprintf(tmp, sizeof tmp, "(constant uint4 *)(mv_arena + mv_off[%d])", k);
                puts_(&b, tmp);
            } else puts_(&b, ps[i].name);
        }
        puts_(&b, ");\n");
    }
    puts_(&b, "  out.mvc = out.pos;\n  out.mvp = prev.pos;\n  return out;\n}\n");
    free(s1);
    return b.buf;
}

char *mv_fs_msl(const char *msl) {
    static const char tail[] = "  return o;\n}\n";
    const char *end = strstr(msl, tail);
    if (!end || end[strlen(tail)] != 0 || strstr(msl, "[[color(7)]]")) return NULL;
    char *s1 = insert_after(msl, "struct FIn {\n  float4 pos [[position]];\n",
                            "  float4 mvc [[user(mvc)]];\n  float4 mvp [[user(mvp)]];\n");
    if (!s1) return NULL;
    char *s2 = insert_after(s1, "struct FOut {\n", "  float2 mvo [[color(7)]];\n");
    free(s1);
    if (!s2) return NULL;
    // (the uv offset from here to where this point was: clip space y up, uv down)
    char *s3 = insert_after(s2, "using namespace metal;\n",
                            "static inline float2 mv_motion(float4 c, float4 p) {\n"
                            "  return (p.xy / p.w - c.xy / c.w) * float2(0.5f, -0.5f);\n}\n");
    free(s2);
    if (!s3) return NULL;
    size_t len = strlen(s3), tl = strlen(tail);
    Buf b = {0};
    put(&b, s3, len - tl);
    puts_(&b, "  o.mvo = mv_motion(in.mvc, in.mvp);\n");
    puts_(&b, tail);
    free(s3);
    return b.buf;
}

// The HUD's variant: colour target 0's output to the UI layer too (MV_TARGET of the HUD's passes), by function
// constants - 0 blended like target 0 (a coverage alpha), 1 opaque (alpha 1), 2 cleared (a composite of the scene)
char *mv_fs_ui_msl(const char *msl) {
    static const char tail[] = "  return o;\n}\n";
    const char *end = strstr(msl, tail);
    if (!end || end[strlen(tail)] != 0 || strstr(msl, "[[color(7)]]") || !strstr(msl, "  float4 c0 [[color(0)]];\n")) return NULL;
    char *s1 = insert_after(msl, "struct FOut {\n", "  float4 mvui [[color(7)]];\n");
    if (!s1) return NULL;
    char *s2 = insert_after(s1, "using namespace metal;\n", "constant int mv_ui_mode [[function_constant(0)]];\n");
    free(s1);
    if (!s2) return NULL;
    size_t len = strlen(s2), tl = strlen(tail);
    Buf b = {0};
    put(&b, s2, len - tl);
    puts_(&b, "  o.mvui = mv_ui_mode == 2 ? float4(0.0f) : float4(o.c0.rgb, mv_ui_mode == 1 ? 1.0f : o.c0.a);\n");
    puts_(&b, tail);
    free(s2);
    return b.buf;
}

// ---------------------------------------------------------------- history
// Frame f's copies go to arena f % NARENA (the GPU may still read frame f - 1's while f + 1 is translated, two frames
// queued at most). Each frame's draws are kept by group (shaders, mesh, range: the key), in order, with a hash of the
// start of each uniform buffer. The game double-buffers its uniform buffers (a draw's are at one of two addresses,
// frame to frame) and draws many things with one mesh (the terrain's chunks), so a draw is matched by contents: the
// previous frame's draw of its group with the most uniform buffers equal to its own (its world matrix: everything
// standing still; the camera's buffer differs for all), the earliest unmatched one among equals (moving things).
#define NARENA 4
#define ARENA_SIZE (32u << 20)
#define MAXE 16384
#define GTAB 8192
#define HASH_BYTES 512
typedef struct { uint64_t group, h[18]; uint32_t offs[18]; int next; bool used; } Entry;
typedef struct { Entry e[MAXE]; int n; struct { uint64_t group; int head, tail; } g[GTAB]; } Frame;
static id<MTLBuffer> arenas[NARENA];
static uint32_t arena_used;
static Frame *fcur, *fprev;
static uint64_t cur_frame = ~0ull;
static int prev_arena = -1;
static struct { uint64_t id, h; uint32_t size, off; uint64_t frame; } dedup[4096];
static uint64_t st_draws, st_matched, st_bytes;

static int *group_slot(Frame *f, uint64_t group, bool add) {  // (&head; NULL: not there / full)
    uint32_t i = (uint32_t)(group ^ (group >> 29)) & (GTAB - 1);
    for (int probe = 0; probe < GTAB; probe++, i = (i + 1) & (GTAB - 1)) {
        if (f->g[i].group == group) return &f->g[i].head;
        if (!f->g[i].group) {
            if (!add) return NULL;
            f->g[i].group = group, f->g[i].head = f->g[i].tail = -1;
            return &f->g[i].head;
        }
    }
    return NULL;
}

static void frame_clear(Frame *f) {
    f->n = 0;
    memset(f->g, 0, sizeof f->g);
}

void mv_reset(void) {
    if (fcur) frame_clear(fcur);
    if (fprev) frame_clear(fprev);
    prev_arena = -1;
    cur_frame = ~0ull;
}

static uint64_t hash_bytes(const uint8_t *p, uint32_t n) {
    uint64_t h = 1469598103934665603ull;
    const uint64_t *w = (const uint64_t *)p;
    for (uint32_t i = 0; i < n / 8; i++) h = (h ^ w[i]) * 1099511628211ull;
    return h ? h : 1;
}

bool mv_record(uint64_t frame, uint64_t group, uint32_t mask, const uint8_t *const data[18], const uint32_t size[18],
               const uint64_t ids[18], id<MTLBuffer> *arena, uint32_t offs[18], bool *matched) {
    if (!group) group = 1;
    if (!fcur) { fcur = calloc(1, sizeof(Frame)); fprev = calloc(1, sizeof(Frame)); }
    if (frame != cur_frame) {
        if (cur_frame != ~0ull && frame == cur_frame + 1) {  // (a frame skipped: nothing to compare with)
            Frame *t = fprev; fprev = fcur; fcur = t;
            prev_arena = (int)(cur_frame % NARENA);
        } else prev_arena = -1;
        frame_clear(fcur);
        cur_frame = frame;
        arena_used = 0;
    }
    int ca = (int)(frame % NARENA);
    if (!arenas[ca]) {
        extern id<MTLDevice> renderer_device(void);
        arenas[ca] = [renderer_device() newBufferWithLength:ARENA_SIZE options:MTLResourceStorageModeShared];
    }
    if (fcur->n >= MAXE) return false;
    int *head = group_slot(fcur, group, true);
    if (!head) return false;
    Entry *e = &fcur->e[fcur->n];
    memset(e, 0, sizeof *e);
    e->group = group;
    e->next = -1;
    uint8_t *base = arenas[ca].contents;
    for (int k = 3; k < 18; k++) {
        if (!(mask & (1u << k))) continue;
        uint32_t d = (uint32_t)((ids[k] ^ (ids[k] >> 21) ^ size[k]) & 4095);
        if (dedup[d].frame == frame + 1 && dedup[d].id == ids[k] && dedup[d].size == size[k]) {
            e->offs[k] = dedup[d].off, e->h[k] = dedup[d].h;
            continue;
        }
        uint32_t at = (arena_used + 255) & ~255u;
        if (at + size[k] > ARENA_SIZE) return false;
        memcpy(base + at, data[k], size[k]);
        arena_used = at + size[k];
        st_bytes += size[k];
        e->offs[k] = at;
        e->h[k] = hash_bytes(data[k], size[k] < HASH_BYTES ? size[k] : HASH_BYTES);
        dedup[d].frame = frame + 1, dedup[d].id = ids[k], dedup[d].size = size[k], dedup[d].off = at, dedup[d].h = e->h[k];
    }
    // (appended to its group's list)
    int idx = fcur->n++;
    int *tail = head + 1;
    if (*head < 0) *head = idx;
    else fcur->e[*tail].next = idx;
    *tail = idx;
    // the previous frame's draw of the group with the most equal uniform buffers, unmatched ones first
    Entry *best = NULL;
    int best_score = -1;
    int *ph = prev_arena >= 0 ? group_slot(fprev, group, false) : NULL;
    for (int i = ph ? *ph : -1, n = 0; i >= 0 && n < 1024; i = fprev->e[i].next, n++) {
        Entry *p = &fprev->e[i];
        int score = p->used ? 0 : 1;
        for (int k = 3; k < 18; k++) if ((mask & (1u << k)) && p->h[k] == e->h[k]) score += 2;
        if (score > best_score) best = p, best_score = score;
    }
    st_draws++;
    *matched = best != NULL;
    if (best) {
        st_matched++;
        best->used = true;
        *arena = arenas[prev_arena];
        memcpy(offs, best->offs, sizeof best->offs);
    } else {
        *arena = arenas[ca];
        memcpy(offs, e->offs, sizeof e->offs);
    }
    return true;
}

void mv_stats(uint64_t *draws, uint64_t *matched, uint64_t *bytes) {
    *draws = st_draws, *matched = st_matched, *bytes = st_bytes;
    st_draws = st_matched = st_bytes = 0;
}

// (debug) the UI layer's coverage as a grey image
void mv_save_coverage(id<MTLTexture> ui, const char *path) {
    extern id<MTLDevice> renderer_device(void);
    extern id<MTLCommandQueue> renderer_queue(void);
    extern void renderer_save_texture(id<MTLTexture> tex, const char *path);
    static id<MTLComputePipelineState> pipe;
    id<MTLDevice> dev = renderer_device();
    if (!pipe) {
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void cov(texture2d<float> m [[texture(0)]], texture2d<float, access::write> o [[texture(1)]],\n"
             "                uint2 p [[thread_position_in_grid]]) {\n"
             "  if (p.x >= o.get_width() || p.y >= o.get_height()) return;\n"
             "  o.write(float4(float3(saturate(m.read(p).a)), 1.0), p);\n}\n" options:nil error:&err];
        pipe = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"cov"] error:&err] : nil;
        if (!pipe) { LOG("[mv] coverage view: %s\n", err.localizedDescription.UTF8String); return; }
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:ui.width height:ui.height mipmapped:NO];
    td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    id<MTLTexture> out = [dev newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cb = [renderer_queue() commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pipe];
    [e setTexture:ui atIndex:0];
    [e setTexture:out atIndex:1];
    [e dispatchThreads:MTLSizeMake(ui.width, ui.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding];
    [cb commit];
    renderer_save_texture(out, path);
}

// (debug) the motion target as an image: offsets of +-1/128 of the screen at full red / green (grey: none)
void mv_save(id<MTLTexture> mv, const char *path) {
    extern id<MTLDevice> renderer_device(void);
    extern id<MTLCommandQueue> renderer_queue(void);
    extern void renderer_save_texture(id<MTLTexture> tex, const char *path);
    static id<MTLComputePipelineState> pipe;
    id<MTLDevice> dev = renderer_device();
    if (!pipe) {
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void mv_vis(texture2d<float> m [[texture(0)]], texture2d<float, access::write> o [[texture(1)]],\n"
             "                   uint2 p [[thread_position_in_grid]]) {\n"
             "  if (p.x >= o.get_width() || p.y >= o.get_height()) return;\n"
             "  float2 v = m.read(p).xy * 64.0;\n"
             "  o.write(float4(clamp(0.5 + v, 0.0, 1.0), 0.5, 1.0), p);\n}\n" options:nil error:&err];
        pipe = lib ? [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"mv_vis"] error:&err] : nil;
        if (!pipe) { LOG("[mv] view: %s\n", err.localizedDescription.UTF8String); return; }
    }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:mv.width height:mv.height mipmapped:NO];
    td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
    id<MTLTexture> out = [dev newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cb = [renderer_queue() commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:pipe];
    [e setTexture:mv atIndex:0];
    [e setTexture:out atIndex:1];
    [e dispatchThreads:MTLSizeMake(mv.width, mv.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [e endEncoding];
    // (and the values: <path>.rg16, the width and height as two uint32 then the half floats)
    id<MTLBuffer> raw = [dev newBufferWithLength:mv.width * mv.height * 4 options:MTLResourceStorageModeShared];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:mv sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(mv.width, mv.height, 1)
               toBuffer:raw destinationOffset:0 destinationBytesPerRow:mv.width * 4 destinationBytesPerImage:mv.width * mv.height * 4];
    [bl endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    char rp[1200];
    snprintf(rp, sizeof rp, "%s.rg16", path);
    FILE *f = fopen(rp, "wb");
    if (f) {
        uint32_t wh[2] = {(uint32_t)mv.width, (uint32_t)mv.height};
        fwrite(wh, 4, 2, f);
        fwrite(raw.contents, 4, mv.width * mv.height, f);
        fclose(f);
    }
    renderer_save_texture(out, path);
}
