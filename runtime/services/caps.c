// caps:su / caps:u (the album): a game's own screenshots - Animal Crossing's camera app - are written as PNG files to
// <save dir>/album/ (save/<game>/album), where the Switch would keep them in its album.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "../hle.h"
#include "../ipc.h"

#define SHOT_W 1280
#define SHOT_H 720

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_table[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n) {
    uint8_t head[8];
    put32(head, n);
    memcpy(head + 4, type, 4);
    fwrite(head, 1, 8, f);
    if (n) fwrite(data, 1, n, f);
    uint32_t crc = crc32_update(crc32_update(0xFFFFFFFFu, (const uint8_t *)type, 4), data, n) ^ 0xFFFFFFFFu;
    uint8_t tail[4];
    put32(tail, crc);
    fwrite(tail, 1, 4, f);
}

// An RGB PNG of the RGBA image (the alpha is the screen's: dropped), its zlib stream made of stored blocks: no
// compressor needed, a few MB a photo.
static bool write_png(const char *path, const uint8_t *rgba, int w, int h) {
    size_t raw_n = (size_t)h * (1 + w * 3);
    uint8_t *raw = malloc(raw_n);
    if (!raw) return false;
    uint8_t *q = raw;
    for (int y = 0; y < h; y++) {
        *q++ = 0;  // (filter: none)
        for (int x = 0; x < w; x++, rgba += 4, q += 3) memcpy(q, rgba, 3);
    }
    size_t blocks = (raw_n + 65534) / 65535;
    size_t z_n = 2 + raw_n + blocks * 5 + 4;
    uint8_t *z = malloc(z_n), *o = z;
    if (!z) { free(raw); return false; }
    *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        uint16_t n = raw_n - off > 65535 ? 65535 : (uint16_t)(raw_n - off);
        *o++ = off + n == raw_n;
        *o++ = n & 0xFF; *o++ = n >> 8; *o++ = ~n & 0xFF; *o++ = (uint16_t)~n >> 8;
        memcpy(o, raw + off, n);
        o += n;
    }
    put32(o, b << 16 | a);
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return false; }
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = {0};
    put32(ihdr, w); put32(ihdr + 4, h);
    ihdr[8] = 8; ihdr[9] = 2;  // 8-bit RGB
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)z_n);
    chunk(f, "IEND", NULL, 0);
    free(z);
    return fclose(f) == 0;
}

// SaveScreenShot / SaveScreenShotEx0..2: the image is the request's largest buffer (1280x720 RGBA). Answers an
// ApplicationAlbumEntry (its contents only matter to the album applet, which we don't have).
static void save_screenshot(IpcReq *req, IpcResp *resp) {
    IpcBuf *img = NULL;
    for (int i = 0; i < req->na; i++)
        if (!img || req->a[i].size > img->size) img = &req->a[i];
    memset(resp->data, 0, 0x20);
    resp_size(resp, 0x20);
    if (!img || img->size < (uint64_t)SHOT_W * SHOT_H * 4) {
        LOG("[caps] screenshot cmd %u: no %dx%d image (largest buffer %llu bytes)\n", req->cmd, SHOT_W, SHOT_H,
            img ? (unsigned long long)img->size : 0ull);
        return;
    }
    extern const char *g_save_dir;  // services/fs.c: save/<game>, or the test run's
    char dir[1100], path[1200];
    snprintf(dir, sizeof dir, "%s/album", g_save_dir);
    mkdir(g_save_dir, 0755);
    mkdir(dir, 0755);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
    struct stat st;
    snprintf(path, sizeof path, "%s/%s.png", dir, stamp);
    for (int n = 2; !stat(path, &st) && n < 100; n++) snprintf(path, sizeof path, "%s/%s_%d.png", dir, stamp, n);
    bool ok = write_png(path, (const uint8_t *)(uintptr_t)img->addr, SHOT_W, SHOT_H);
    LOG("[caps] %s %s\n", ok ? "photo saved to" : "could not write", path);
    *(uint64_t *)resp->data = (uint64_t)SHOT_W * SHOT_H * 4;  // entry.size
}

SERVICE("caps:su", caps_su) {
    (void)s;
    switch (req->cmd) {
    case 32: break;  // SetShimLibraryVersion
    case 201: case 203: case 205: case 210: save_screenshot(req, resp); break;
    default: LOG("[caps] caps:su cmd %u (ok)\n", req->cmd);
    }
}

SERVICE("caps:u", caps_u) {
    (void)s;
    switch (req->cmd) {
    case 32: break;  // SetShimLibraryVersion
    default: LOG("[caps] caps:u cmd %u (ok)\n", req->cmd);
    }
}
