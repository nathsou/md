// Frame generation (framegen.h): images between a 30 fps game's frames, from the optical flow between them.
//
// Each new game image is copied (the game reuses its window textures) and reduced to a luminance pyramid of five
// levels from a quarter of its size. The flow is found coarse to fine, both ways (previous -> current and back): at each
// level every pixel tries the flow of the level above, its neighbours' and none, each refined by a step of one texel,
// by the summed difference of 5x5 patches (no motion is preferred a little: the HUD and still scenery stay put). An
// in-between image at time t samples both frames along the flows (Super SloMo's linear approximation of the flow from
// t) and blends them by time and by visibility: a pixel whose flow there and back doesn't return is taken from the other
// frame. When the frames differ too much for the flow (a scene cut, a menu opening), a GPU-side check shows the previous
// or the new frame instead of a blend.
#import <Metal/Metal.h>
#include <stdlib.h>

#include "../hle.h"
#include "../settings.h"
#include "framegen.h"

#define LEVELS 5
#define MAX_IMAGES 4

static const char *fg_src =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "constexpr sampler lin(filter::linear, address::clamp_to_edge, coord::normalized);\n"
    // the source image's luminance, a quarter of its size (16 texels averaged by 4 bilinear samples)
    "kernel void fg_luma(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
    "                    uint2 p [[thread_position_in_grid]]) {\n"
    "    if (p.x >= dst.get_width() || p.y >= dst.get_height()) return;\n"
    "    float2 s = 1.0 / float2(dst.get_width(), dst.get_height()), c = (float2(p) + 0.5) * s;\n"
    "    float3 w = float3(0.299, 0.587, 0.114), sum = 0;\n"
    "    for (int k = 0; k < 4; k++) sum += src.sample(lin, c + s * (float2(k & 1, k >> 1) - 0.5) * 0.5).rgb;\n"
    "    dst.write(float4(dot(sum * 0.25, w)), p);\n"
    "}\n"
    "kernel void fg_down(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
    "                    uint2 p [[thread_position_in_grid]]) {\n"
    "    if (p.x >= dst.get_width() || p.y >= dst.get_height()) return;\n"
    "    float2 c = (float2(p) + 0.5) / float2(dst.get_width(), dst.get_height());\n"
    "    dst.write(src.sample(lin, c), p);\n"
    "}\n"
    "static float cost(texture2d<float> a, texture2d<float> b, int2 p, float2 f) {\n"
    "    float2 sz = float2(a.get_width(), a.get_height());\n"
    "    float sum = 0;\n"
    "    for (int y = -2; y <= 2; y++) for (int x = -2; x <= 2; x++) {\n"
    "        float2 q = float2(p + int2(x, y)) + 0.5;\n"
    "        sum += fabs(a.sample(lin, q / sz).r - b.sample(lin, (q + f) / sz).r);\n"
    "    }\n"
    "    return sum;\n"
    "}\n"
    // flow from a to b at one level: the candidates (the coarser level's, x2, around p; none) each refined by a step
    "kernel void fg_flow(texture2d<float> a [[texture(0)]], texture2d<float> b [[texture(1)]],\n"
    "                    texture2d<float> coarse [[texture(2)]], texture2d<float, access::write> out [[texture(3)]],\n"
    "                    constant float &has_coarse [[buffer(0)]], device atomic_uint *err [[buffer(1)]],\n"
    "                    uint2 gp [[thread_position_in_grid]]) {\n"
    "    if (gp.x >= out.get_width() || gp.y >= out.get_height()) return;\n"
    "    int2 p = int2(gp);\n"
    "    float2 csz = float2(coarse.get_width(), coarse.get_height()), uv = (float2(p) + 0.5) / float2(out.get_width(), out.get_height());\n"
    "    float2 cand[6];\n"
    "    int n = 0;\n"
    "    cand[n++] = 0;\n"
    "    if (has_coarse > 0) {\n"
    "        cand[n++] = coarse.sample(lin, uv).xy * 2;\n"
    "        for (int k = 0; k < 4; k++) {\n"
    "            float2 o = float2(k == 0 ? -2 : k == 1 ? 2 : 0, k == 2 ? -2 : k == 3 ? 2 : 0) / csz;\n"
    "            cand[n++] = coarse.sample(lin, uv + o).xy * 2;\n"
    "        }\n"
    "    }\n"
    "    float best = cost(a, b, p, 0) * 0.9 - 0.02, bc;\n"  // (no motion preferred)
    "    float2 bf = 0;\n"
    "    for (int i = 1; i < n; i++) { bc = cost(a, b, p, cand[i]); if (bc < best) { best = bc; bf = cand[i]; } }\n"
    "    for (int it = 0; it < 2; it++) {\n"  // (two rounds of one-texel steps, then a half-texel one)
    "        float2 c0 = bf;\n"
    "        for (int k = 0; k < 8; k++) {\n"
    "            float2 d = float2(k < 3 ? -1 : k < 5 ? 0 : 1, (k == 0 || k == 3 || k == 5) ? -1 : (k == 1 || k == 6) ? 0 : 1);\n"
    "            bc = cost(a, b, p, c0 + d);\n"
    "            if (bc < best) { best = bc; bf = c0 + d; }\n"
    "        }\n"
    "    }\n"
    "    float2 c0 = bf;\n"
    "    for (int k = 0; k < 8; k++) {\n"
    "        float2 d = 0.5 * float2(k < 3 ? -1 : k < 5 ? 0 : 1, (k == 0 || k == 3 || k == 5) ? -1 : (k == 1 || k == 6) ? 0 : 1);\n"
    "        bc = cost(a, b, p, c0 + d);\n"
    "        if (bc < best) { best = bc; bf = c0 + d; }\n"
    "    }\n"
    "    out.write(float4(bf, best, 0), gp);\n"
    "    if (has_coarse > 0) atomic_fetch_add_explicit(err, uint(min(best, 25.0) * 40.0), memory_order_relaxed);\n"
    "}\n"
    // the flow smoothed: the median of the 3x3 neighbourhood per component (an outlier's match doesn't spread)
    "kernel void fg_median(texture2d<float> src [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],\n"
    "                      uint2 p [[thread_position_in_grid]]) {\n"
    "    if (p.x >= dst.get_width() || p.y >= dst.get_height()) return;\n"
    "    float vx[9], vy[9];\n"
    "    int2 m = int2(dst.get_width(), dst.get_height()) - 1;\n"
    "    for (int i = 0; i < 9; i++) { float2 f = src.read(uint2(clamp(int2(p) + int2(i % 3 - 1, i / 3 - 1), 0, m))).xy; vx[i] = f.x; vy[i] = f.y; }\n"
    "    for (int i = 0; i < 9; i++) for (int j = i + 1; j < 9; j++) {\n"
    "        if (vx[j] < vx[i]) { float t = vx[i]; vx[i] = vx[j]; vx[j] = t; }\n"
    "        if (vy[j] < vy[i]) { float t = vy[i]; vy[i] = vy[j]; vy[j] = t; }\n"
    "    }\n"
    "    dst.write(float4(vx[4], vy[4], src.read(p).z, 0), p);\n"
    "}\n"
    "struct V { float4 pos [[position]]; float2 uv; };\n"
    "vertex V fg_vs(uint id [[vertex_id]]) {\n"
    "    float2 uv = float2((id << 1) & 2, id & 2);\n"
    "    V v; v.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); v.uv = uv; return v;\n"
    "}\n"
    "struct P { float t, sigma, cut; uint pixels; };\n"
    // flows in level-0 texels (a quarter of the image): their uv offsets are f / flow size
    "fragment float4 fg_blend(V in [[stage_in]], texture2d<float> i0 [[texture(0)]], texture2d<float> i1 [[texture(1)]],\n"
    "                         texture2d<float> f01 [[texture(2)]], texture2d<float> f10 [[texture(3)]],\n"
    "                         constant P &prm [[buffer(0)]], device const uint *err [[buffer(1)]]) {\n"
    "    float t = prm.t;\n"
    "    float avg = float(err[0]) / 40.0 / float(prm.pixels);\n"
    "    if (avg > prm.cut) return t < 0.5 ? i0.sample(lin, in.uv) : i1.sample(lin, in.uv);\n"
    "    float2 fs = float2(f01.get_width(), f01.get_height());\n"
    "    float2 a = f01.sample(lin, in.uv).xy / fs, b = f10.sample(lin, in.uv).xy / fs;\n"
    "    float2 ft0 = -(1 - t) * t * a + t * t * b, ft1 = (1 - t) * (1 - t) * a - t * (1 - t) * b;\n"
    "    float2 p0 = in.uv + ft0, p1 = in.uv + ft1;\n"
    "    float2 a0 = f01.sample(lin, p0).xy / fs, b1 = f10.sample(lin, p1).xy / fs;\n"
    "    float e0 = length((a0 + f10.sample(lin, p0 + a0).xy / fs) * fs);\n"  // (there and back from frame 0)
    "    float e1 = length((b1 + f01.sample(lin, p1 + b1).xy / fs) * fs);\n"
    "    float w0 = (1 - t) * exp(-e0 * e0 * prm.sigma), w1 = t * exp(-e1 * e1 * prm.sigma);\n"
    "    if (w0 + w1 < 1e-4) { w0 = 1 - t; w1 = t; }\n"
    "    return (i0.sample(lin, p0) * w0 + i1.sample(lin, p1) * w1) / (w0 + w1);\n"
    "}\n";

static id<MTLComputePipelineState> p_luma, p_down, p_flow, p_median;
static id<MTLRenderPipelineState> p_blend;
static MTLPixelFormat blend_format;
static bool failed;

static id<MTLTexture> hist[2];          // the game images: [prev], [cur] alternate
static id<MTLTexture> pyr[2][LEVELS];   // their luminance pyramids
static id<MTLTexture> flow[2][LEVELS];  // previous -> current, current -> previous
static id<MTLTexture> smooth[2];        // level 0, median-filtered
static id<MTLTexture> images[MAX_IMAGES];
static id<MTLBuffer> err;               // the level-0 match error, summed (scene cuts)
static int cur_i = -1;                  // hist[cur_i] is the newest; -1: none yet
static bool have_prev;

void framegen_reset(void) { cur_i = -1; have_prev = false; }

int framegen_factor(int interval) {
    static int env = -2;
    if (env == -2) env = getenv("LARECOMP_FRAMEGEN") ? atoi(getenv("LARECOMP_FRAMEGEN")) : -1;
    int target = env >= 0 ? env : g_settings.frame_gen;
    if (target <= 0 || interval < 1) return 1;
    extern volatile int g_display_hz;  // (metal.m: the window's display)
    if (target > g_display_hz) target = g_display_hz;
    int source = 60 / interval, n = target / source;
    return n < 1 ? 1 : n > MAX_IMAGES ? MAX_IMAGES : n;
}

static id<MTLTexture> tex(id<MTLDevice> dev, id<MTLTexture> t, MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage u) {
    if (t && t.width == w && t.height == h && t.pixelFormat == f) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = u;
    d.storageMode = MTLStorageModePrivate;
    return [dev newTextureWithDescriptor:d];
}

static bool build(id<MTLDevice> dev) {
    if (p_flow || failed) return !failed;
    NSError *e = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(fg_src) options:nil error:&e];
    if (!lib) { LOG("[framegen] shaders: %s\n", e.localizedDescription.UTF8String); failed = true; return false; }
    p_luma = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fg_luma"] error:&e];
    p_down = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fg_down"] error:&e];
    p_median = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fg_median"] error:&e];
    p_flow = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fg_flow"] error:&e];
    if (!p_luma || !p_down || !p_median || !p_flow) { LOG("[framegen] pipelines: %s\n", e.localizedDescription.UTF8String); failed = true; return false; }
    err = [dev newBufferWithLength:16 options:MTLResourceStorageModePrivate];
    return true;
}

static void dispatch(id<MTLComputeCommandEncoder> c, id<MTLComputePipelineState> p, id<MTLTexture> out) {
    [c setComputePipelineState:p];
    [c dispatchThreads:MTLSizeMake(out.width, out.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
}

bool framegen_prepare(id<MTLDevice> dev, id<MTLCommandBuffer> cb, id<MTLTexture> cur, int n) {
    if (!build(dev) || n < 2) return false;
    int ni = cur_i < 0 ? 0 : cur_i ^ 1;
    bool fits = cur_i >= 0 && hist[cur_i].width == cur.width && hist[cur_i].height == cur.height && hist[cur_i].pixelFormat == cur.pixelFormat;
    hist[ni] = tex(dev, hist[ni], cur.pixelFormat, cur.width, cur.height, MTLTextureUsageShaderRead);
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:cur sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
             sourceSize:MTLSizeMake(cur.width, cur.height, 1) toTexture:hist[ni] destinationSlice:0 destinationLevel:0
      destinationOrigin:MTLOriginMake(0, 0, 0)];
    [bl fillBuffer:err range:NSMakeRange(0, 16) value:0];
    [bl endEncoding];
    // the luminance pyramid of the new image
    MTLTextureUsage rw = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    NSUInteger w = MAX(cur.width / 4, 16u), h = MAX(cur.height / 4, 16u);
    for (int l = 0; l < LEVELS; l++) {
        NSUInteger lw = MAX(w >> l, 4u), lh = MAX(h >> l, 4u);
        pyr[ni][l] = tex(dev, pyr[ni][l], MTLPixelFormatR16Float, lw, lh, rw);
        for (int d = 0; d < 2; d++) flow[d][l] = tex(dev, flow[d][l], MTLPixelFormatRGBA16Float, lw, lh, rw);
    }
    for (int d = 0; d < 2; d++) smooth[d] = tex(dev, smooth[d], MTLPixelFormatRGBA16Float, w, h, rw);
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setTexture:hist[ni] atIndex:0];
    [c setTexture:pyr[ni][0] atIndex:1];
    dispatch(c, p_luma, pyr[ni][0]);
    for (int l = 1; l < LEVELS; l++) {
        [c setTexture:pyr[ni][l - 1] atIndex:0];
        [c setTexture:pyr[ni][l] atIndex:1];
        dispatch(c, p_down, pyr[ni][l]);
    }
    int pi = ni ^ 1;
    bool ok = fits && have_prev;
    if (ok) {  // the flows, coarse to fine: [0] previous -> current, [1] current -> previous
        for (int d = 0; d < 2; d++)
            for (int l = LEVELS - 1; l >= 0; l--) {
                [c setTexture:pyr[d ? ni : pi][l] atIndex:0];
                [c setTexture:pyr[d ? pi : ni][l] atIndex:1];
                [c setTexture:l < LEVELS - 1 ? flow[d][l + 1] : smooth[d] atIndex:2];  // (the top level has none)
                [c setTexture:flow[d][l] atIndex:3];
                float has = l < LEVELS - 1;
                [c setBytes:&has length:4 atIndex:0];
                [c setBuffer:err offset:d * 4 atIndex:1];
                dispatch(c, p_flow, flow[d][l]);
            }
        for (int d = 0; d < 2; d++) {
            [c setTexture:flow[d][0] atIndex:0];
            [c setTexture:smooth[d] atIndex:1];
            dispatch(c, p_median, smooth[d]);
        }
    }
    [c endEncoding];
    cur_i = ni;
    have_prev = true;
    if (!ok) return false;
    if (!p_blend || blend_format != cur.pixelFormat) {
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:@(fg_src) options:nil error:&e];
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"fg_vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fg_blend"];
        pd.colorAttachments[0].pixelFormat = cur.pixelFormat;
        p_blend = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        blend_format = cur.pixelFormat;
        if (!p_blend) { LOG("[framegen] blend pipeline: %s\n", e.localizedDescription.UTF8String); failed = true; return false; }
        LOG("[framegen] %lux%lu, flow %lux%lu, %d images per game frame\n", (unsigned long)cur.width,
            (unsigned long)cur.height, (unsigned long)w, (unsigned long)h, n);
    }
    // the summed error covers every level below the top: about 1.33 x level 0's pixels, both directions (the first
    // counter only is read: previous -> current)
    uint32_t pixels = (uint32_t)(w * h * 4 / 3);
    for (int k = 1; k < n; k++) {
        images[k] = tex(dev, images[k], cur.pixelFormat, cur.width, cur.height, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = images[k];
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:p_blend];
        struct { float t, sigma, cut; uint32_t pixels; } prm = {(float)k / n, 0.15f, 3.0f, pixels};
        [e setFragmentBytes:&prm length:sizeof prm atIndex:0];
        [e setFragmentBuffer:err offset:0 atIndex:1];
        [e setFragmentTexture:hist[pi] atIndex:0];
        [e setFragmentTexture:hist[ni] atIndex:1];
        [e setFragmentTexture:smooth[0] atIndex:2];
        [e setFragmentTexture:smooth[1] atIndex:3];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [e endEncoding];
    }
    return true;
}

id<MTLTexture> framegen_image(int k) { return k > 0 && k < MAX_IMAGES ? images[k] : nil; }
