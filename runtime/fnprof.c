// Function-entry profiler. LARECOMP_FNPROF=<from>:<to> counts guest function entries (every generated
// function calls FN_ENTER) between those presented frames, then writes build/fnprof.txt (addr count, most
// called first). Diffing two runs shows which game functions a change brings in; counts also show hot code.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "hle.h"
#include "platform.h"

volatile int g_fnprof;

#define NSLOT (1 << 18)
static struct { _Atomic uint64_t addr; _Atomic uint64_t count; } slots[NSLOT];

// LARECOMP_FNPROF_BT=<hex addr>[,...]: also print the host backtrace (guest functions are f_<addr>) the first time
// each listed function is entered inside the window - who calls it
static uint64_t bt_addrs[256];
static int bt_n = -1;
static _Atomic uint8_t bt_done[256];

uint64_t g_bt_first_x0;

static void bt_check(uint64_t a, uint64_t lr, uint64_t x0) {
    if (bt_n < 0) {
        bt_n = 0;
        const char *e = getenv("LARECOMP_FNPROF_BT");
        for (const char *p = e; p && *p && bt_n < 256;) {
            bt_addrs[bt_n++] = strtoull(p, (char **)&p, 16);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    for (int i = 0; i < bt_n; i++)
        if (bt_addrs[i] == a && !atomic_exchange(&bt_done[i], 1)) {
            if (i == 0) g_bt_first_x0 = x0;
            uint64_t vt = x0 >= 0x7000000000ull && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)x0 : 0;
            uint64_t v100 = vt >= 0x7000000000ull && vt < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(vt + 0x100) : 0;
            uint64_t p8 = x0 >= 0x7000000000ull && x0 < 0x7800000000ull ? *(uint64_t *)(uintptr_t)(x0 + 8) : 0;
            fprintf(stderr, "[fnprof-bt] %llx entered from guest lr %llx x0 %llx (vtable %llx, +0x100 %llx, [x0+8] %llx):\n",
                    (unsigned long long)a, (unsigned long long)lr, (unsigned long long)x0, (unsigned long long)vt,
                    (unsigned long long)v100, (unsigned long long)p8);
            plat_print_backtrace();  // (execinfo is macOS / glibc only)
        }
}

// LARECOMP_TRACE=<hex addr>[,...] (debugging): every entry of these guest functions is logged with its first
// arguments, its return address and its thread (at most LARECOMP_TRACE_MAX lines, default 2000). Turns the entry hook
// on for the whole run (a little slower).
static uint64_t trace_addrs[64];
static int trace_n = -1;
static _Atomic int trace_lines, trace_max;

static void trace_check(uint64_t a, Cpu *c) {
    for (int i = 0; i < trace_n; i++) {
        if (trace_addrs[i] != a) continue;
        if (atomic_fetch_add(&trace_lines, 1) >= trace_max) return;
        extern uint32_t kernel_current_thread_handle(void);
        LOG("[trace] %llx(%llx, %llx, %llx, %llx, %llx) lr %llx thread %#x\n", (unsigned long long)a,
            (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2], (unsigned long long)c->x[3],
            (unsigned long long)c->x[4], (unsigned long long)c->x[30], kernel_current_thread_handle());
        // LARECOMP_TRACE_DUMP=<n>:<bytes>: also the memory argument x<n> points to (guest memory only); with a hex
        // address instead of n, that memory
        static long long dreg = -2;
        static int dlen;
        if (dreg == -2) {
            const char *d = getenv("LARECOMP_TRACE_DUMP");
            if (!d || sscanf(d, "%lli:%i", &dreg, &dlen) != 2) dreg = -1;
        }
        uint64_t p = dreg >= 0 && dreg < 8 ? c->x[dreg] : dreg >= 8 ? (uint64_t)dreg : 0;
        for (int o = 0; p >= GUEST_LO && p + dlen < GUEST_HI && o < dlen; o += 32) {
            const uint64_t *q = (const uint64_t *)(uintptr_t)(p + o);
            LOG("  %llx+%#04x: %016llx %016llx %016llx %016llx\n", (unsigned long long)p, o, (unsigned long long)q[0], (unsigned long long)q[1],
                (unsigned long long)q[2], (unsigned long long)q[3]);
        }
    }
}

void fnprof_hit(uint64_t a, Cpu *c) {
    if (trace_n > 0) trace_check(a, c);
    if (g_fnprof < 2) return;  // (1: only tracing)
    uint64_t lr = c->x[30], x0 = c->x[0];
    if (bt_n) bt_check(a, lr, x0);
    uint32_t h = (uint32_t)((a >> 2) * 0x9E3779B1u) & (NSLOT - 1);
    for (int n = 0; n < NSLOT; n++, h = (h + 1) & (NSLOT - 1)) {
        uint64_t cur = atomic_load_explicit(&slots[h].addr, memory_order_relaxed);
        if (cur == a || (cur == 0 && atomic_compare_exchange_strong(&slots[h].addr, &cur, a)) || cur == a) {
            atomic_fetch_add_explicit(&slots[h].count, 1, memory_order_relaxed);
            return;
        }
    }
}

static int cmp(const void *x, const void *y) {
    uint64_t a = ((const uint64_t *)x)[1], b = ((const uint64_t *)y)[1];
    return a < b ? 1 : a > b ? -1 : 0;
}

void fnprof_frame(uint64_t frame) {
    static long from = -2, to;
    if (from == -2) {
        from = -1;
        const char *e = getenv("LARECOMP_FNPROF");
        if (e && sscanf(e, "%ld:%ld", &from, &to) != 2) from = -1;
    }
    if (from < 0) return;
    if ((long)frame == from) { g_fnprof = 2; LOG("[fnprof] on at frame %llu\n", (unsigned long long)frame); }
    if ((long)frame != to) return;
    g_fnprof = trace_n > 0;
    uint64_t (*rows)[2] = malloc(sizeof(uint64_t[2]) * NSLOT);
    int n = 0;
    for (int i = 0; i < NSLOT; i++)
        if (slots[i].addr) { rows[n][0] = slots[i].addr; rows[n][1] = slots[i].count; n++; }
    qsort(rows, n, sizeof rows[0], cmp);
    const char *out = getenv("LARECOMP_FNPROF_OUT") ? getenv("LARECOMP_FNPROF_OUT") : "build/fnprof.txt";  // (parallel runs)
    FILE *f = fopen(out, "w");
    for (int i = 0; f && i < n; i++) fprintf(f, "%llx %llu\n", (unsigned long long)rows[i][0], (unsigned long long)rows[i][1]);
    if (f) fclose(f);
    free(rows);
    LOG("[fnprof] %d functions, written %s (frames %ld-%ld)\n", n, out, from, to);
    from = -1;
}

// LARECOMP_FNPROF=0:<to>: profile from process start (threads the game starts before the first frame)
__attribute__((constructor)) static void fnprof_early(void) {
    const char *e = getenv("LARECOMP_FNPROF");
    if (e && e[0] == '0' && e[1] == ':') g_fnprof = 2;
    const char *t = getenv("LARECOMP_TRACE");
    trace_n = 0;
    for (char *p = (char *)t; p && *p && trace_n < 64;) {
        trace_addrs[trace_n++] = strtoull(p, &p, 16);
        while (*p == ',' || *p == ' ') p++;
    }
    trace_max = getenv("LARECOMP_TRACE_MAX") ? atoi(getenv("LARECOMP_TRACE_MAX")) : 2000;
    if (trace_n > 0 && !g_fnprof) g_fnprof = 1;
}
