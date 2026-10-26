// Offline check of the shader translator against every shader a game ships: each graphics shader blob in its romfs
// (any file, Yaz0- or zstd-compressed ones decompressed first: Dread's system/shd/*.bshdat, ACNH's Shader/*.szs and
// Layout/*.zs ...; found by the NVN code header magic 0x12345678, the stage from the Maxwell SPH after it) is translated
// to MSL (runtime/nvn/maxwell/translate.c) and compiled with Metal. Reports translation failures and compile errors,
// one line per unique blob, plus totals.
//
// build: make build/shader_corpus
// usage: build/shader_corpus <romfs> [--no-compile] [--glsl] [--dump dir]
//   --glsl: the Vulkan backend's GLSL instead (translation only: compiling it needs shaderc)
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shader.h"
#include "yaz0.h"
#include <sys/stat.h>
#include <zstd.h>

static uint64_t fnv(const uint8_t *p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

#define MAX_SEEN (1 << 20)
static uint64_t seen[MAX_SEEN];
static int nseen;
static bool seen_add(uint64_t h) {
    for (int i = 0; i < nseen; i++) if (seen[i] == h) return false;
    if (nseen < MAX_SEEN) seen[nseen++] = h;
    return true;
}

static id<MTLDevice> dev;
static MTLCompileOptions *vopts;
static bool compile = true, glsl = false;
static const char *dump;
static int files, blobs, unique, tfail, cfail, by_stage[6];

// The file's bytes, decompressed when it is Yaz0 or zstd (NULL: unreadable)
static uint8_t *load(const char *path, size_t *n) {
    NSData *data = [NSData dataWithContentsOfFile:@(path)];
    if (!data) return NULL;
    const uint8_t *p = data.bytes;
    size_t len = data.length;
    if (yaz0_size(p, len)) return yaz0_decompress(p, len, n);
    if (len >= 4 && !memcmp(p, "\x28\xb5\x2f\xfd", 4)) {
        unsigned long long sz = ZSTD_getFrameContentSize(p, len);
        if (sz == ZSTD_CONTENTSIZE_ERROR || sz == ZSTD_CONTENTSIZE_UNKNOWN || sz > (1u << 30)) return NULL;
        uint8_t *out = malloc(sz ? sz : 1);
        size_t got = ZSTD_decompress(out, sz, p, len);
        if (ZSTD_isError(got)) { free(out); return NULL; }
        *n = got;
        return out;
    }
    uint8_t *out = malloc(len ? len : 1);
    memcpy(out, p, len);
    *n = len;
    return out;
}

static void scan_file(const char *path, const char *name) {
    size_t n = 0;
    uint8_t *p = NULL;
    @autoreleasepool { p = load(path, &n); }
    if (!p) return;
    // blob starts (4-byte aligned magic with a plausible SPH), each running to the next start or the file end
    size_t *starts = malloc(sizeof *starts * 65536);
    int ns = 0;
    for (size_t o = 0; o + 0x80 <= n && ns < 65536; o += 4) {
        if (*(const uint32_t *)(p + o) != 0x12345678u) continue;
        uint32_t sph0 = *(const uint32_t *)(p + o + 0x30);
        uint32_t type = (sph0 >> 10) & 0xF, ver = (sph0 >> 5) & 0x1F;
        if ((type == 1 || type == 5) && ver == 3) starts[ns++] = o;
    }
    if (ns) files++;
    if (getenv("CORPUS_LIST")) {  // (which files hold shaders: names and counts only)
        if (ns) printf("FILE %s %d\n", name, ns);
        free(starts);
        free(p);
        return;
    }
    for (int i = 0; i < ns; i++) @autoreleasepool {
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
            printf("TRANSLATE %s+%#zx %s: %s\n", name, o, stage == STAGE_VERTEX ? "vs" : "fs", si->error[0] ? si->error : "?");
            maxwell_free(si);
            continue;
        }
        if (dump) {
            char dp[2048];
            snprintf(dp, sizeof dp, glsl ? "%s/%s_%zx_%s.glsl" : "%s/%s_%zx_%s.metal", dump, name, o, stage == STAGE_VERTEX ? "vs" : "fs");
            for (char *q = dp + strlen(dump) + 1; *q; q++) if (*q == '/') *q = '_';
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
                printf("COMPILE %s+%#zx %s: %s\n", name, o, stage == STAGE_VERTEX ? "vs" : "fs", first.UTF8String);
            }
        }
        maxwell_free(si);
    }
    free(starts);
    free(p);
}

static void walk(const char *dir, const char *rel, int depth) {
    DIR *d = depth < 12 ? opendir(dir) : NULL;
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char path[2048], r[2048];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        struct stat sb;
        if (stat(path, &sb)) continue;
        if (S_ISDIR(sb.st_mode)) walk(path, r, depth + 1);
        else scan_file(path, r);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s <romfs> [--no-compile] [--glsl] [--dump dir]\n", argv[0]); return 2; }
        const char *dir = argv[1];
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--no-compile")) compile = false;
            else if (!strcmp(argv[i], "--glsl")) glsl = true, compile = false;
            else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = argv[++i];
        }
        dev = compile ? MTLCreateSystemDefaultDevice() : nil;
        vopts = [MTLCompileOptions new];
        vopts.preserveInvariance = YES;
        walk(dir, "", 0);
        printf("files %d, blobs %d, unique %d (vs %d, fs %d): translate failures %d, compile failures %d\n", files, blobs,
               unique, by_stage[STAGE_VERTEX], by_stage[STAGE_FRAGMENT], tfail, cfail);
        return tfail || cfail ? 1 : 0;
    }
}
