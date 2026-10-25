// Offline check of the shader translator against every shader the game ships: each graphics shader blob in
// romfs/system/shd/*.bshdat (found by the NVN code header magic 0x12345678; the stage from the Maxwell SPH after it)
// is translated to MSL (runtime/nvn/maxwell/translate.c) and compiled with Metal. Reports translation failures and
// compile errors, one line per unique blob, plus totals.
//
// build: clang -O1 -fobjc-arc -framework Metal -framework Foundation -Iruntime/nvn/maxwell \
//            tools/shader_corpus.m runtime/nvn/maxwell/translate.c -o build/shader_corpus
// usage: build/shader_corpus <romfs/system/shd> [--no-compile] [--glsl] [--dump dir]
//   --glsl: the Vulkan backend's GLSL instead (translation only: compiling it needs shaderc)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"

static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

#define MAX_SEEN 65536
static uint64_t seen[MAX_SEEN];
static int nseen;
static bool seen_add(uint64_t h) {
    for (int i = 0; i < nseen; i++) if (seen[i] == h) return false;
    if (nseen < MAX_SEEN) seen[nseen++] = h;
    return true;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s <shd dir> [--no-compile] [--dump dir]\n", argv[0]); return 2; }
        const char *dir = argv[1], *dump = NULL;
        bool compile = true, glsl = false;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--no-compile")) compile = false;
            else if (!strcmp(argv[i], "--glsl")) glsl = true, compile = false;
            else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        }
        id<MTLDevice> dev = compile ? MTLCreateSystemDefaultDevice() : nil;
        MTLCompileOptions *vopts = [MTLCompileOptions new];
        vopts.preserveInvariance = YES;
        DIR *d = opendir(dir);
        if (!d) { perror(dir); return 2; }
        int files = 0, blobs = 0, unique = 0, tfail = 0, cfail = 0, by_stage[6] = {0};
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t ln = strlen(e->d_name);
            if (ln < 7 || strcmp(e->d_name + ln - 7, ".bshdat")) continue;
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
            NSData *data = [NSData dataWithContentsOfFile:@(path)];
            if (!data) continue;
            files++;
            const uint8_t *p = data.bytes;
            size_t n = data.length;
            // blob starts (4-byte aligned magic with a plausible SPH), each running to the next start or the file end
            size_t starts[4096];
            int ns = 0;
            for (size_t o = 0; o + 0x80 <= n && ns < 4096; o += 4) {
                if (*(const uint32_t *)(p + o) != 0x12345678u) continue;
                uint32_t sph0 = *(const uint32_t *)(p + o + 0x30);
                uint32_t type = (sph0 >> 10) & 0xF, ver = (sph0 >> 5) & 0x1F;
                if ((type == 1 || type == 5) && ver == 3) starts[ns++] = o;
            }
            for (int i = 0; i < ns; i++) {
                size_t o = starts[i], end = i + 1 < ns ? starts[i + 1] : n;
                uint32_t type = (*(const uint32_t *)(p + o + 0x30) >> 10) & 0xF;
                int stage = type == 1 ? STAGE_VERTEX : STAGE_FRAGMENT;
                blobs++;
                if (!seen_add(fnv(p + o, end - o > 0x4000 ? 0x4000 : end - o))) continue;
                unique++;
                by_stage[stage]++;
                ShaderKey key;
                memset(&key, 0, sizeof key);
                key.glsl = glsl;
                ShaderInfo *si = maxwell_translate(p + o, end - o, stage, &key);
                if (!si->msl) {
                    tfail++;
                    printf("TRANSLATE %s+%#zx %s: %s\n", e->d_name, o, stage == STAGE_VERTEX ? "vs" : "fs", si->error[0] ? si->error : "?");
                    maxwell_free(si);
                    continue;
                }
                if (dump) {
                    char dp[2048];
                    snprintf(dp, sizeof dp, glsl ? "%s/%s_%zx_%s.glsl" : "%s/%s_%zx_%s.metal", dump, e->d_name, o, stage == STAGE_VERTEX ? "vs" : "fs");
                    FILE *f = fopen(dp, "w");
                    if (f) { fputs(si->msl, f); fclose(f); }
                }
                if (compile) {
                    NSError *err = nil;
                    id<MTLLibrary> lib = [dev newLibraryWithSource:@(si->msl) options:stage == STAGE_VERTEX ? vopts : nil error:&err];
                    if (!lib) {
                        cfail++;
                        NSString *msg = err.localizedDescription;
                        NSRange r = [msg rangeOfString:@"error:"];
                        NSString *first = r.location != NSNotFound ? [msg substringFromIndex:r.location] : msg;
                        NSRange nl = [first rangeOfString:@"\n"];
                        if (nl.location != NSNotFound) first = [first substringToIndex:nl.location];
                        printf("COMPILE %s+%#zx %s: %s\n", e->d_name, o, stage == STAGE_VERTEX ? "vs" : "fs", first.UTF8String);
                    }
                }
                maxwell_free(si);
            }
        }
        closedir(d);
        printf("files %d, blobs %d, unique %d (vs %d, fs %d): translate failures %d, compile failures %d\n", files, blobs,
               unique, by_stage[STAGE_VERTEX], by_stage[STAGE_FRAGMENT], tfail, cfail);
        return tfail || cfail ? 1 : 0;
    }
}
