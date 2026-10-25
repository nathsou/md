// setjmp / longjmp across recompiled code. A guest longjmp restores the registers its setjmp saved and returns from
// that setjmp again - into the middle of a recompiled function, which lives in a host C frame: the dispatcher can't
// enter a function there. So tools/gen.py turns every call of setjmp (_setjmp, __setjmp; through the PLT or not) into a
// host _setjmp in the calling function's own frame, recorded here by the guest jmp_buf's address with the guest's
// callee-saved registers; longjmp (hle_symbols.txt: the SDK's own is replaced) restores those and host-longjmps back
// into that frame, which goes on as the guest would after setjmp returned. Dread's Lua raises its errors this way
// (luaD_throw), and the SDK's libpng too.
#include <setjmp.h>
#include <string.h>

#include "../hle.h"

typedef struct {
    uint64_t guest;        // the guest jmp_buf (0: free)
    uint64_t x[12];        // x19..x30
    uint64_t sp;
    __uint128_t v[8];      // v8..v15 (d8..d15 are callee-saved; the whole registers, as cheap)
    uint64_t fpcr;
    jmp_buf jb;
} JmpSlot;

#define NSLOTS 64
static __thread JmpSlot t_slots[NSLOTS];
static __thread unsigned t_next;

static JmpSlot *find(uint64_t guest) {
    for (int i = 0; i < NSLOTS; i++) if (t_slots[i].guest == guest) return &t_slots[i];
    return NULL;
}

// The host jmp_buf for a guest one (a setjmp on the same guest buffer again reuses its slot)
void *rt_setjmp_slot(Cpu *c, uint64_t guest) {
    (void)c;
    JmpSlot *s = find(guest);
    if (!s) { s = &t_slots[t_next++ % NSLOTS]; s->guest = guest; }
    return &s->jb;
}

// setjmp's own return (0): what a longjmp to this buffer restores
void rt_setjmp_save(Cpu *c, uint64_t guest) {
    JmpSlot *s = find(guest);
    if (!s) FATAL("setjmp: no slot for %llx\n", (unsigned long long)guest);
    memcpy(s->x, &c->x[19], sizeof s->x);
    s->sp = c->sp;
    memcpy(s->v, &c->v[8], sizeof s->v);
    s->fpcr = c->fpcr;
}

static void do_longjmp(Cpu *c) {
    JmpSlot *s = find(c->x[0]);
    if (!s) FATAL("longjmp to %llx: no setjmp of this thread recorded it\n", (unsigned long long)c->x[0]);
    int val = (int)c->x[1];
    memcpy(&c->x[19], s->x, sizeof s->x);
    c->sp = s->sp;
    memcpy(&c->v[8], s->v, sizeof s->v);
    c->fpcr = s->fpcr;
    c->x[0] = (uint64_t)(uint32_t)(val ? val : 1);
    _longjmp(s->jb, 1);
}

void hle_longjmp(Cpu *c) { do_longjmp(c); }
void hle__longjmp(Cpu *c) { do_longjmp(c); }
