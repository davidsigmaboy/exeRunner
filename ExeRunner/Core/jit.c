/* jit.c - x86 -> ARM64 basic-block translator.

   Design
   - Guest registers live in CpuState (x19 = CpuState*, x20 = guest memory base).
     Each translated instruction loads/stores them directly: simple and correct,
     not fast. A register allocator is the obvious next step.
   - Flags are lazy: arithmetic stores (op, a, b, result) into CpuState and Jcc
     calls cpu_eval_cond(). Same representation the interpreter uses.
   - A block ends at any branch/call/ret, at an unsupported instruction, or after
     MAX_INSNS. It returns the next guest EIP in w0; the dispatcher in cpu.c does
     the rest. Unsupported instructions simply run in the interpreter, so the
     two engines can be mixed freely at block boundaries.
   - Guest pointers are used as 32-bit offsets from x20. With a full 4 GiB window
     that needs no checks; with a smaller window (iPhones usually grant ~1 GiB) every
     address is first masked into the window (Em.mask). Unmapped accesses fault on
     the host. */

#include "er_internal.h"

#if defined(__aarch64__)

#include <stdlib.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <unistd.h>
#endif

#define CODE_BYTES     (8u << 20)
#define CACHE_SLOTS    8192u
#define MAX_INSNS      48
#define HEADROOM_WORDS 4096u

typedef struct { uint32_t eip; uint32_t valid; jit_fn fn; } Slot;

struct JitState {
    uint32_t *code;
    size_t    cap_words, used_words;
    Slot      slots[CACHE_SLOTS];
};

/* W^X handling: macOS arm64 needs MAP_JIT + per-thread toggling. On iOS the process
   is made writable+executable by the JIT enabler (debugger), so nothing to toggle. */
#if defined(__APPLE__) && TARGET_OS_OSX
#define JIT_WRITE_BEGIN() pthread_jit_write_protect_np(0)
#define JIT_WRITE_END()   pthread_jit_write_protect_np(1)
#else
#define JIT_WRITE_BEGIN() ((void)0)
#define JIT_WRITE_END()   ((void)0)
#endif

static void icache_flush(void *start, size_t bytes) {
#ifdef __APPLE__
    sys_icache_invalidate(start, bytes);
#else
    __builtin___clear_cache((char *)start, (char *)start + bytes);
#endif
}

#if defined(__APPLE__) && TARGET_OS_IPHONE
extern int csops(int pid, unsigned int ops, void *useraddr, size_t usersize);
#endif

int jit_supported(void) {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    /* iOS only lets us run generated code while a debugger/JIT enabler is attached:
       the kernel then sets CS_DEBUGGED on the process. */
    uint32_t flags = 0;
    if (csops(getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof flags) != 0) return 0;
    return (flags & 0x10000000u /* CS_DEBUGGED */) != 0;
#else
    return 1;
#endif
}

int jit_init(Machine *m) {
    if (m->jit) return 0;
    struct JitState *j = (struct JitState *)calloc(1, sizeof *j);
    if (!j) return -1;
    int prot = PROT_READ | PROT_WRITE | PROT_EXEC;
    void *p = MAP_FAILED;
#if defined(__APPLE__)
    p = mmap(NULL, CODE_BYTES, prot, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
#endif
    if (p == MAP_FAILED) p = mmap(NULL, CODE_BYTES, prot, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { free(j); return -1; }
    j->code = (uint32_t *)p;
    j->cap_words = CODE_BYTES / 4;
    m->jit = j;
    return 0;
}

void jit_free(Machine *m) {
    if (!m->jit) return;
    munmap(m->jit->code, CODE_BYTES);
    free(m->jit);
    m->jit = NULL;
}

/* ---------- ARM64 encoders ---------- */

typedef struct { uint32_t *p, *end; uint32_t mask; } Em;
static void E(Em *e, uint32_t w) { if (e->p < e->end) *e->p++ = w; }

#define OFF_R(i)   ((uint32_t)(offsetof(CpuState, r) + 4u * (unsigned)(i)))
#define OFF_CC_OP  ((uint32_t)offsetof(CpuState, cc_op))
#define OFF_CC_A   ((uint32_t)offsetof(CpuState, cc_a))
#define OFF_CC_B   ((uint32_t)offsetof(CpuState, cc_b))
#define OFF_CC_RES ((uint32_t)offsetof(CpuState, cc_res))

static uint32_t a_movz(int rd, uint32_t imm, int hw)   { return 0x52800000u | ((uint32_t)hw << 21) | ((imm & 0xFFFFu) << 5) | (uint32_t)rd; }
static uint32_t a_movk(int rd, uint32_t imm, int hw)   { return 0x72800000u | ((uint32_t)hw << 21) | ((imm & 0xFFFFu) << 5) | (uint32_t)rd; }
static uint32_t a_ldr_ctx(int rt, uint32_t off)        { return 0xB9400000u | ((off / 4u) << 10) | (19u << 5) | (uint32_t)rt; }   /* ldr wt,[x19,#off] */
static uint32_t a_str_ctx(int rt, uint32_t off)        { return 0xB9000000u | ((off / 4u) << 10) | (19u << 5) | (uint32_t)rt; }   /* str wt,[x19,#off] */
static uint32_t a_ldr_mem(int rt, int rm)              { return 0xB8604800u | ((uint32_t)rm << 16) | (20u << 5) | (uint32_t)rt; } /* ldr wt,[x20,wm,uxtw] */
static uint32_t a_str_mem(int rt, int rm)              { return 0xB8204800u | ((uint32_t)rm << 16) | (20u << 5) | (uint32_t)rt; } /* str wt,[x20,wm,uxtw] */
static uint32_t a_add(int rd, int rn, int rm)          { return 0x0B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static uint32_t a_sub(int rd, int rn, int rm)          { return 0x4B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static uint32_t a_and(int rd, int rn, int rm)          { return 0x0A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static uint32_t a_orr(int rd, int rn, int rm)          { return 0x2A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static uint32_t a_eor(int rd, int rn, int rm)          { return 0x4A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static uint32_t a_add_i(int rd, int rn, uint32_t imm)  { return 0x11000000u | ((imm & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }
static uint32_t a_sub_i(int rd, int rn, uint32_t imm)  { return 0x51000000u | ((imm & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rd; }

/* fixed 2-instruction 32-bit constant load (the Jcc sequence relies on fixed sizes) */
static void mov32(Em *e, int rd, uint32_t v) { E(e, a_movz(rd, v & 0xFFFFu, 0)); E(e, a_movk(rd, v >> 16, 1)); }

/* dst = src, masked into the guest window when it is smaller than 4 GiB (clobbers w6) */
static void emit_mask(Em *e, int dst, int src) {
    if (e->mask) { mov32(e, 6, e->mask); E(e, a_and(dst, src, 6)); }
    else if (dst != src) E(e, a_orr(dst, 31, src));         /* mov wdst, wsrc */
}

static void prologue(Em *e) {
    E(e, 0xA9800000u | ((0x7Fu & (uint32_t)-4) << 15) | (30u << 10) | (31u << 5) | 29u);   /* stp x29,x30,[sp,#-32]! */
    E(e, 0xA9000000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);                       /* stp x19,x20,[sp,#16]   */
    E(e, 0xAA0003F3u);                                                                     /* mov x19, x0 (ctx)      */
    E(e, 0xAA0103F4u);                                                                     /* mov x20, x1 (guest mem)*/
}
static void epilogue(Em *e) {
    E(e, 0xA9400000u | (2u << 15) | (20u << 10) | (31u << 5) | 19u);                       /* ldp x19,x20,[sp,#16]   */
    E(e, 0xA8C00000u | (4u << 15) | (30u << 10) | (31u << 5) | 29u);                       /* ldp x29,x30,[sp],#32   */
    E(e, 0xD65F03C0u);                                                                     /* ret                    */
}
static void ret_eip(Em *e, uint32_t eip) { mov32(e, 0, eip); epilogue(e); }                 /* 5 words */

static uint32_t jit_cond_helper(CpuState *c, uint32_t cc) { return cpu_eval_cond(c, cc); }

/* ---------- instruction translation ---------- */

typedef struct { Machine *m; uint32_t pc; int bad; } F;
static uint32_t fb(F *f)  { uint32_t v = 0; if (!mem_r8(f->m, f->pc, &v))  f->bad = 1; f->pc += 1; return v; }
static uint32_t f32(F *f) { uint32_t v = 0; if (!mem_r32(f->m, f->pc, &v)) f->bad = 1; f->pc += 4; return v; }

typedef struct { int mod, reg, rm, abs; int32_t disp; } MR;

/* ModRM without SIB; anything fancier is left to the interpreter. */
static int mr_decode(F *f, MR *o) {
    uint32_t b = fb(f);
    o->mod = (int)(b >> 6); o->reg = (int)((b >> 3) & 7); o->rm = (int)(b & 7); o->abs = 0; o->disp = 0;
    if (o->mod == 3) return !f->bad;
    if (o->rm == 4) return 0;
    if (o->mod == 0 && o->rm == 5) { o->abs = 1; o->disp = (int32_t)f32(f); return !f->bad; }
    if (o->mod == 1) o->disp = (int8_t)fb(f);
    else if (o->mod == 2) o->disp = (int32_t)f32(f);
    return !f->bad;
}

/* w3 = effective address */
static void emit_addr(Em *e, const MR *o) {
    if (o->abs) { mov32(e, 3, (uint32_t)o->disp); emit_mask(e, 3, 3); return; }
    E(e, a_ldr_ctx(3, OFF_R(o->rm)));
    if (o->disp) { mov32(e, 5, (uint32_t)o->disp); E(e, a_add(3, 3, 5)); }
    emit_mask(e, 3, 3);
}

/* w2 = a, w3 = b on entry. aop: 0 add,1 or,4 and,5 sub,6 xor,7 cmp. dst<0 => no write-back. */
static void emit_alu(Em *e, int aop, int dst) {
    int ccop;
    switch (aop) {
    case 0: E(e, a_add(4, 2, 3)); ccop = CC_ADD;   break;
    case 1: E(e, a_orr(4, 2, 3)); ccop = CC_LOGIC; break;
    case 4: E(e, a_and(4, 2, 3)); ccop = CC_LOGIC; break;
    case 6: E(e, a_eor(4, 2, 3)); ccop = CC_LOGIC; break;
    default: E(e, a_sub(4, 2, 3)); ccop = CC_SUB;  break;   /* 5 and 7 */
    }
    if (aop != 7 && dst >= 0) E(e, a_str_ctx(4, OFF_R(dst)));
    E(e, a_str_ctx(2, OFF_CC_A));
    E(e, a_str_ctx(3, OFF_CC_B));
    E(e, a_str_ctx(4, OFF_CC_RES));
    E(e, a_movz(5, (uint32_t)ccop, 0));
    E(e, a_str_ctx(5, OFF_CC_OP));
}

static void emit_push_w3(Em *e) {
    E(e, a_ldr_ctx(2, OFF_R(R_ESP)));
    E(e, a_sub_i(2, 2, 4));
    E(e, a_str_ctx(2, OFF_R(R_ESP)));
    emit_mask(e, 2, 2);
    E(e, a_str_mem(3, 2));
}

static void emit_jcc(Em *e, unsigned cc, uint32_t target, uint32_t fall) {
    uint64_t fn = (uint64_t)(uintptr_t)&jit_cond_helper;
    E(e, 0xAA1303E0u);                                                  /* mov x0, x19     */
    E(e, a_movz(1, cc, 0));                                             /* mov w1, #cc     */
    E(e, 0xD2800000u | ((uint32_t)( fn        & 0xFFFF) << 5) | 16u);   /* movz x16        */
    E(e, 0xF2A00000u | ((uint32_t)((fn >> 16) & 0xFFFF) << 5) | 16u);   /* movk x16, lsl16 */
    E(e, 0xF2C00000u | ((uint32_t)((fn >> 32) & 0xFFFF) << 5) | 16u);   /* movk x16, lsl32 */
    E(e, 0xF2E00000u | ((uint32_t)((fn >> 48) & 0xFFFF) << 5) | 16u);   /* movk x16, lsl48 */
    E(e, 0xD63F0200u);                                                  /* blr x16         */
    E(e, 0x340000C0u);                                                  /* cbz w0, +6      */
    ret_eip(e, target);                                                 /* taken (5 words) */
    ret_eip(e, fall);                                                   /* not taken       */
}

/* Returns 0 = not translatable (caller rewinds), 1 = ok, 2 = ok and ends the block. */
static int xlate_one(Machine *m, Em *e, uint32_t pc, uint32_t *next) {
    F f = { m, pc, 0 };
    uint32_t op = fb(&f);
    if (f.bad) return 0;

    /* add/or/and/sub/xor/cmp: r/m32,r32 | r32,r/m32 | eax,imm32 (register forms only) */
    if (op < 0x40 && ((op & 7) == 1 || (op & 7) == 3 || (op & 7) == 5)) {
        int aop = (int)(op >> 3);
        if (aop == 2 || aop == 3) return 0;                 /* adc / sbb */
        if ((op & 7) == 5) {
            uint32_t imm = f32(&f); if (f.bad) return 0;
            E(e, a_ldr_ctx(2, OFF_R(R_EAX))); mov32(e, 3, imm);
            emit_alu(e, aop, R_EAX);
        } else {
            MR mr;
            if (!mr_decode(&f, &mr) || mr.mod != 3) return 0;
            int dst = ((op & 7) == 1) ? mr.rm : mr.reg;
            int src = ((op & 7) == 1) ? mr.reg : mr.rm;
            E(e, a_ldr_ctx(2, OFF_R(dst))); E(e, a_ldr_ctx(3, OFF_R(src)));
            emit_alu(e, aop, dst);
        }
        *next = f.pc; return 1;
    }

    if (op >= 0x50 && op <= 0x57) {                         /* push r32 */
        E(e, a_ldr_ctx(3, OFF_R(op & 7)));
        emit_push_w3(e);
        *next = f.pc; return 1;
    }
    if (op >= 0x58 && op <= 0x5F) {                         /* pop r32 */
        E(e, a_ldr_ctx(2, OFF_R(R_ESP)));
        emit_mask(e, 7, 2);
        E(e, a_ldr_mem(3, 7));
        E(e, a_add_i(2, 2, 4));
        E(e, a_str_ctx(2, OFF_R(R_ESP)));
        E(e, a_str_ctx(3, OFF_R(op & 7)));                  /* after esp update: pop esp keeps popped value */
        *next = f.pc; return 1;
    }
    if (op >= 0xB8 && op <= 0xBF) {                         /* mov r32, imm32 */
        uint32_t imm = f32(&f); if (f.bad) return 0;
        mov32(e, 2, imm);
        E(e, a_str_ctx(2, OFF_R(op & 7)));
        *next = f.pc; return 1;
    }
    if (op >= 0x70 && op <= 0x7F) {                         /* jcc rel8 */
        int32_t rel = (int8_t)fb(&f); if (f.bad) return 0;
        emit_jcc(e, op & 15, f.pc + (uint32_t)rel, f.pc);
        *next = f.pc; return 2;
    }

    switch (op) {
    case 0x90: *next = f.pc; return 1;

    case 0x68: case 0x6A: {                                 /* push imm */
        uint32_t imm = (op == 0x68) ? f32(&f) : (uint32_t)(int32_t)(int8_t)fb(&f);
        if (f.bad) return 0;
        mov32(e, 3, imm);
        emit_push_w3(e);
        *next = f.pc; return 1;
    }

    case 0x81: case 0x83: {                                 /* group 1, register operand only */
        MR mr;
        if (!mr_decode(&f, &mr) || mr.mod != 3) return 0;
        uint32_t imm = (op == 0x81) ? f32(&f) : (uint32_t)(int32_t)(int8_t)fb(&f);
        if (f.bad) return 0;
        if (mr.reg == 2 || mr.reg == 3) return 0;           /* adc / sbb */
        E(e, a_ldr_ctx(2, OFF_R(mr.rm))); mov32(e, 3, imm);
        emit_alu(e, mr.reg, mr.rm);
        *next = f.pc; return 1;
    }

    case 0x85: {                                            /* test r/m32(reg), r32 */
        MR mr;
        if (!mr_decode(&f, &mr) || mr.mod != 3) return 0;
        E(e, a_ldr_ctx(2, OFF_R(mr.rm))); E(e, a_ldr_ctx(3, OFF_R(mr.reg)));
        emit_alu(e, 4, -1);
        *next = f.pc; return 1;
    }

    case 0x89: case 0x8B: {                                 /* mov r/m32 <-> r32 (no SIB) */
        MR mr;
        if (!mr_decode(&f, &mr)) return 0;
        if (mr.mod == 3) {
            int src = (op == 0x89) ? mr.reg : mr.rm, dst = (op == 0x89) ? mr.rm : mr.reg;
            E(e, a_ldr_ctx(2, OFF_R(src))); E(e, a_str_ctx(2, OFF_R(dst)));
        } else {
            emit_addr(e, &mr);
            if (op == 0x89) { E(e, a_ldr_ctx(2, OFF_R(mr.reg))); E(e, a_str_mem(2, 3)); }
            else            { E(e, a_ldr_mem(2, 3)); E(e, a_str_ctx(2, OFF_R(mr.reg))); }
        }
        *next = f.pc; return 1;
    }

    case 0xE8: {                                            /* call rel32 */
        int32_t rel = (int32_t)f32(&f); if (f.bad) return 0;
        mov32(e, 3, f.pc);
        emit_push_w3(e);
        ret_eip(e, f.pc + (uint32_t)rel);
        *next = f.pc; return 2;
    }
    case 0xE9: case 0xEB: {                                 /* jmp */
        int32_t rel = (op == 0xE9) ? (int32_t)f32(&f) : (int32_t)(int8_t)fb(&f);
        if (f.bad) return 0;
        ret_eip(e, f.pc + (uint32_t)rel);
        *next = f.pc; return 2;
    }
    case 0xC3: case 0xC2: {                                 /* ret [imm16] */
        uint32_t imm = 0;
        if (op == 0xC2) { imm = fb(&f); imm |= fb(&f) << 8; }
        if (f.bad) return 0;
        E(e, a_ldr_ctx(2, OFF_R(R_ESP)));
        emit_mask(e, 7, 2);
        E(e, a_ldr_mem(0, 7));
        E(e, a_add_i(2, 2, 4));
        if (imm) { mov32(e, 3, imm); E(e, a_add(2, 2, 3)); }
        E(e, a_str_ctx(2, OFF_R(R_ESP)));
        epilogue(e);
        *next = f.pc; return 2;
    }

    case 0x0F: {
        uint32_t op2 = fb(&f);
        if (!f.bad && op2 >= 0x80 && op2 <= 0x8F) {         /* jcc rel32 */
            int32_t rel = (int32_t)f32(&f); if (f.bad) return 0;
            emit_jcc(e, op2 & 15, f.pc + (uint32_t)rel, f.pc);
            *next = f.pc; return 2;
        }
        return 0;
    }
    default:
        return 0;
    }
}

static jit_fn translate(Machine *m, struct JitState *j, uint32_t eip) {
    Em e = { j->code + j->used_words, j->code + j->cap_words,
             m->arena_size >= 0x100000000ull ? 0u : (uint32_t)(m->arena_size - 1) };
    uint32_t *start = e.p;
    prologue(&e);

    uint32_t pc = eip;
    int n = 0, done = 0;
    while (!done && n < MAX_INSNS) {
        uint32_t *save = e.p, next = pc;
        int r = xlate_one(m, &e, pc, &next);
        if (r == 0) { e.p = save; break; }
        pc = next; n++;
        if (r == 2) done = 1;
    }
    if (n == 0) return NULL;                                /* first instruction not supported */
    if (!done) ret_eip(&e, pc);                             /* fall through into the interpreter */

    j->used_words = (size_t)(e.p - j->code);
    icache_flush(start, (size_t)(e.p - start) * 4);
    return (jit_fn)(void *)start;
}

jit_fn jit_get_block(Machine *m, uint32_t eip) {
    struct JitState *j = m->jit;
    if (!j) return NULL;
    if (j->cap_words - j->used_words < HEADROOM_WORDS) {    /* code buffer full: flush everything */
        memset(j->slots, 0, sizeof j->slots);
        j->used_words = 0;
    }
    Slot *s = &j->slots[(eip ^ (eip >> 12)) & (CACHE_SLOTS - 1)];
    if (s->valid && s->eip == eip) return s->fn;

    JIT_WRITE_BEGIN();
    jit_fn fn = translate(m, j, eip);
    JIT_WRITE_END();

    s->eip = eip; s->valid = 1; s->fn = fn;                 /* fn == NULL caches "interpret this" */
    return fn;
}

#else  /* not arm64: interpreter only */

int    jit_supported(void)                    { return 0; }
int    jit_init(Machine *m)                   { (void)m; return -1; }
void   jit_free(Machine *m)                   { (void)m; }
jit_fn jit_get_block(Machine *m, uint32_t e)  { (void)m; (void)e; return NULL; }

#endif
