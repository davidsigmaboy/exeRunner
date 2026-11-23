/* cpu.c - 32-bit x86 interpreter and the main dispatch loop.
   The interpreter is the reference implementation and the fallback for anything
   the JIT does not translate. Flags are evaluated lazily (see CC_* in er_internal.h). */
#include "er_internal.h"

/* ---------- lazy flags ---------- */

static uint32_t flag_cf(const CpuState *c) {
    switch (c->cc_op) {
    case CC_ADD: return c->cc_res < c->cc_a;
    case CC_SUB: return c->cc_a < c->cc_b;
    case CC_INC: case CC_DEC: return c->cf_saved;
    default: return 0;
    }
}
static uint32_t flag_zf(const CpuState *c) { return c->cc_res == 0; }
static uint32_t flag_sf(const CpuState *c) { return c->cc_res >> 31; }
static uint32_t flag_of(const CpuState *c) {
    switch (c->cc_op) {
    case CC_ADD: return ((c->cc_a ^ c->cc_res) & (c->cc_b ^ c->cc_res)) >> 31;
    case CC_SUB: return ((c->cc_a ^ c->cc_b) & (c->cc_a ^ c->cc_res)) >> 31;
    case CC_INC: return c->cc_res == 0x80000000u;
    case CC_DEC: return c->cc_res == 0x7FFFFFFFu;
    default: return 0;
    }
}
static uint32_t flag_pf(const CpuState *c) { return !(__builtin_popcount(c->cc_res & 0xFFu) & 1); }

/* cc = low nibble of the Jcc opcode (0x70+cc / 0x0F 0x80+cc) */
uint32_t cpu_eval_cond(const CpuState *c, unsigned cc) {
    uint32_t v;
    switch ((cc >> 1) & 7) {
    case 0: v = flag_of(c); break;
    case 1: v = flag_cf(c); break;
    case 2: v = flag_zf(c); break;
    case 3: v = flag_cf(c) | flag_zf(c); break;
    case 4: v = flag_sf(c); break;
    case 5: v = flag_pf(c); break;
    case 6: v = flag_sf(c) != flag_of(c); break;
    default: v = flag_zf(c) | (flag_sf(c) != flag_of(c)); break;
    }
    return (cc & 1) ? !v : v;
}

/* ---------- decoding helpers ---------- */

typedef struct { Machine *m; uint32_t pc; int bad; uint32_t seg; } Dec;
typedef struct { int isreg; int reg; uint32_t addr; } Op;

static uint32_t f8(Dec *d)  { uint32_t v = 0; if (!mem_r8(d->m, d->pc, &v))  d->bad = 1; d->pc += 1; return v; }
static uint32_t f32(Dec *d) { uint32_t v = 0; if (!mem_r32(d->m, d->pc, &v)) d->bad = 1; d->pc += 4; return v; }

static int modrm(Dec *d, int *regf, Op *o) {
    CpuState *c = &d->m->cpu;
    uint32_t b = f8(d);
    unsigned mod = b >> 6, rm = b & 7;
    *regf = (int)((b >> 3) & 7);
    if (mod == 3) { o->isreg = 1; o->reg = (int)rm; o->addr = 0; return !d->bad; }
    o->isreg = 0; o->reg = 0;
    uint32_t addr;
    if (rm == 4) {
        uint32_t sib = f8(d);
        unsigned scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        addr = (base == 5 && mod == 0) ? f32(d) : c->r[base];
        if (idx != 4) addr += c->r[idx] << scale;
    } else if (rm == 5 && mod == 0) {
        addr = f32(d);
    } else {
        addr = c->r[rm];
    }
    if (mod == 1) addr += (uint32_t)(int32_t)(int8_t)f8(d);
    else if (mod == 2) addr += f32(d);
    o->addr = addr + d->seg;
    return !d->bad;
}

static int rd32op(Dec *d, const Op *o, uint32_t *v) {
    if (o->isreg) { *v = d->m->cpu.r[o->reg]; return 1; }
    return mem_r32(d->m, o->addr, v);
}
static int wr32op(Dec *d, const Op *o, uint32_t v) {
    if (o->isreg) { d->m->cpu.r[o->reg] = v; return 1; }
    return mem_w32(d->m, o->addr, v);
}
static uint32_t get8(const CpuState *c, int i) { return i < 4 ? (c->r[i] & 0xFFu) : ((c->r[i - 4] >> 8) & 0xFFu); }
static void set8(CpuState *c, int i, uint32_t v) {
    v &= 0xFFu;
    if (i < 4) c->r[i] = (c->r[i] & ~0xFFu) | v;
    else       c->r[i - 4] = (c->r[i - 4] & ~0xFF00u) | (v << 8);
}

static int push32(Machine *m, uint32_t v) {
    uint32_t sp = m->cpu.r[R_ESP] - 4;
    if (!mem_w32(m, sp, v)) return 0;
    m->cpu.r[R_ESP] = sp;
    return 1;
}
static int pop32(Machine *m, uint32_t *v) {
    uint32_t sp = m->cpu.r[R_ESP];
    if (!mem_r32(m, sp, v)) return 0;
    m->cpu.r[R_ESP] = sp + 4;
    return 1;
}

/* aop: 0 add, 1 or, 4 and, 5 sub, 6 xor, 7 cmp (adc/sbb not implemented) */
static int alu(CpuState *c, int aop, uint32_t a, uint32_t b, uint32_t *res) {
    uint32_t r; int cc;
    switch (aop) {
    case 0: r = a + b; cc = CC_ADD;   break;
    case 1: r = a | b; cc = CC_LOGIC; break;
    case 4: r = a & b; cc = CC_LOGIC; break;
    case 5: case 7: r = a - b; cc = CC_SUB; break;
    case 6: r = a ^ b; cc = CC_LOGIC; break;
    default: return 0;
    }
    c->cc_op = (uint32_t)cc; c->cc_a = a; c->cc_b = b; c->cc_res = r;
    *res = r;
    return 1;
}

static void fault(Machine *m, const char *why, uint32_t eip, unsigned op) {
    er_log(m, "[cpu] %s at eip=0x%08X (opcode 0x%02X)\n", why, eip, op);
    m->halted = 1;
    m->exit_code = -2;
}

#define MEMFAULT()  do { fault(m, "memory access fault", start, op); return; } while (0)
#define NEED(x)     do { if (!(x) || d.bad) MEMFAULT(); } while (0)
#define UNSUPPORTED() do { fault(m, "unsupported instruction", start, op); return; } while (0)

/* ---------- one instruction ---------- */

static void step(Machine *m) {
    CpuState *c = &m->cpu;
    uint32_t start = c->eip;
    Dec d = { m, start, 0, 0 };
    unsigned op = f8(&d);
    if (op == 0x64) { d.seg = TEB_BASE; op = f8(&d); }      /* fs: override */
    if (d.bad) MEMFAULT();

    if (op >= 0x40 && op <= 0x4F) {                         /* inc/dec r32 */
        int r = (int)(op & 7), dec = op >= 0x48;
        uint32_t v = c->r[r], nv = dec ? v - 1 : v + 1;
        c->cf_saved = flag_cf(c);
        c->cc_op = dec ? CC_DEC : CC_INC; c->cc_a = v; c->cc_b = 0; c->cc_res = nv;
        c->r[r] = nv;
        c->eip = d.pc; return;
    }
    if (op >= 0x50 && op <= 0x57) { NEED(push32(m, c->r[op & 7])); c->eip = d.pc; return; }
    if (op >= 0x58 && op <= 0x5F) { uint32_t v; NEED(pop32(m, &v)); c->r[op & 7] = v; c->eip = d.pc; return; }
    if (op >= 0xB8 && op <= 0xBF) { c->r[op & 7] = f32(&d); NEED(1); c->eip = d.pc; return; }
    if (op >= 0x70 && op <= 0x7F) {                         /* jcc rel8 */
        int32_t rel = (int8_t)f8(&d); NEED(1);
        c->eip = cpu_eval_cond(c, op & 15) ? d.pc + (uint32_t)rel : d.pc;
        return;
    }

    /* ALU: add/or/and/sub/xor/cmp  r/m32,r32 | r32,r/m32 | eax,imm32 */
    if (op < 0x40 && (op & 7) <= 5 && (op & 7) != 4 && (op & 7) != 0 && (op & 7) != 2) {
        int aop = (int)(op >> 3);
        uint32_t a, b, res;
        switch (op & 7) {
        case 1: { int r; Op o; NEED(modrm(&d, &r, &o)); NEED(rd32op(&d, &o, &a)); b = c->r[r];
                  if (!alu(c, aop, a, b, &res)) UNSUPPORTED();
                  if (aop != 7) { NEED(wr32op(&d, &o, res)); }
                  break; }
        case 3: { int r; Op o; NEED(modrm(&d, &r, &o)); a = c->r[r]; NEED(rd32op(&d, &o, &b));
                  if (!alu(c, aop, a, b, &res)) UNSUPPORTED();
                  if (aop != 7) c->r[r] = res;
                  break; }
        default: { b = f32(&d); NEED(1); a = c->r[R_EAX];
                  if (!alu(c, aop, a, b, &res)) UNSUPPORTED();
                  if (aop != 7) c->r[R_EAX] = res;
                  break; }
        }
        c->eip = d.pc; return;
    }

    switch (op) {
    case 0x90: break;                                       /* nop */
    case 0xCC: fault(m, "int3 breakpoint", start, op); return;
    case 0xF4: m->halted = 1; m->exit_code = 0; return;     /* hlt */

    case 0x68: { uint32_t v = f32(&d); NEED(push32(m, v)); break; }
    case 0x6A: { uint32_t v = (uint32_t)(int32_t)(int8_t)f8(&d); NEED(push32(m, v)); break; }

    case 0x81: case 0x83: {                                 /* group 1: op r/m32, imm */
        int aop; Op o; uint32_t a, b, res;
        NEED(modrm(&d, &aop, &o));
        b = (op == 0x81) ? f32(&d) : (uint32_t)(int32_t)(int8_t)f8(&d);
        NEED(rd32op(&d, &o, &a));
        if (!alu(c, aop, a, b, &res)) UNSUPPORTED();
        if (aop != 7) NEED(wr32op(&d, &o, res));
        break;
    }
    case 0x85: {                                            /* test r/m32, r32 */
        int r; Op o; uint32_t a, res;
        NEED(modrm(&d, &r, &o)); NEED(rd32op(&d, &o, &a));
        alu(c, 4, a, c->r[r], &res);
        break;
    }
    case 0x88: { int r; Op o; NEED(modrm(&d, &r, &o));      /* mov r/m8, r8 */
                 if (o.isreg) set8(c, o.reg, get8(c, r)); else NEED(mem_w8(m, o.addr, get8(c, r)));
                 break; }
    case 0x8A: { int r; Op o; uint32_t v; NEED(modrm(&d, &r, &o));  /* mov r8, r/m8 */
                 if (o.isreg) v = get8(c, o.reg); else NEED(mem_r8(m, o.addr, &v));
                 set8(c, r, v); break; }
    case 0x89: { int r; Op o; NEED(modrm(&d, &r, &o)); NEED(wr32op(&d, &o, c->r[r])); break; }
    case 0x8B: { int r; Op o; uint32_t v; NEED(modrm(&d, &r, &o)); NEED(rd32op(&d, &o, &v)); c->r[r] = v; break; }
    case 0x8D: { int r; Op o; NEED(modrm(&d, &r, &o)); if (o.isreg) UNSUPPORTED(); c->r[r] = o.addr; break; }
    case 0xC6: { int r; Op o; NEED(modrm(&d, &r, &o)); uint32_t v = f8(&d); NEED(1);
                 if (r != 0) UNSUPPORTED();
                 if (o.isreg) set8(c, o.reg, v); else NEED(mem_w8(m, o.addr, v));
                 break; }
    case 0xC7: { int r; Op o; NEED(modrm(&d, &r, &o)); uint32_t v = f32(&d); NEED(1);
                 if (r != 0) UNSUPPORTED();
                 NEED(wr32op(&d, &o, v)); break; }
    case 0xC9: { c->r[R_ESP] = c->r[R_EBP]; uint32_t v; NEED(pop32(m, &v)); c->r[R_EBP] = v; break; }   /* leave */

    case 0xE8: { int32_t rel = (int32_t)f32(&d); NEED(push32(m, d.pc)); c->eip = d.pc + (uint32_t)rel; return; }
    case 0xE9: { int32_t rel = (int32_t)f32(&d); NEED(1); c->eip = d.pc + (uint32_t)rel; return; }
    case 0xEB: { int32_t rel = (int8_t)f8(&d);   NEED(1); c->eip = d.pc + (uint32_t)rel; return; }
    case 0xC3: { uint32_t v; NEED(pop32(m, &v)); c->eip = v; return; }
    case 0xC2: { uint32_t n = f8(&d); n |= f8(&d) << 8; NEED(1);
                 uint32_t v; NEED(pop32(m, &v)); c->r[R_ESP] += n; c->eip = v; return; }

    case 0xFF: {
        int r; Op o; uint32_t v;
        NEED(modrm(&d, &r, &o));
        switch (r) {
        case 0: case 1: {
            NEED(rd32op(&d, &o, &v));
            uint32_t nv = (r == 0) ? v + 1 : v - 1;
            c->cf_saved = flag_cf(c);
            c->cc_op = (r == 0) ? CC_INC : CC_DEC; c->cc_a = v; c->cc_b = 0; c->cc_res = nv;
            NEED(wr32op(&d, &o, nv));
            break;
        }
        case 2: NEED(rd32op(&d, &o, &v)); NEED(push32(m, d.pc)); c->eip = v; return;   /* call r/m32 */
        case 4: NEED(rd32op(&d, &o, &v)); c->eip = v; return;                           /* jmp  r/m32 */
        case 6: NEED(rd32op(&d, &o, &v)); NEED(push32(m, v)); break;                    /* push r/m32 */
        default: UNSUPPORTED();
        }
        break;
    }

    case 0x0F: {
        unsigned op2 = f8(&d); NEED(1);
        if (op2 >= 0x80 && op2 <= 0x8F) {                   /* jcc rel32 */
            int32_t rel = (int32_t)f32(&d); NEED(1);
            c->eip = cpu_eval_cond(c, op2 & 15) ? d.pc + (uint32_t)rel : d.pc;
            return;
        }
        if (op2 == 0xB6 || op2 == 0xB7) {                   /* movzx r32, r/m8 | r/m16 */
            int r; Op o; uint32_t v;
            NEED(modrm(&d, &r, &o));
            if (op2 == 0xB6) { if (o.isreg) v = get8(c, o.reg); else NEED(mem_r8(m, o.addr, &v)); }
            else             { if (o.isreg) v = c->r[o.reg] & 0xFFFFu; else NEED(mem_r16(m, o.addr, &v)); }
            c->r[r] = v;
            break;
        }
        UNSUPPORTED();
    }

    default:
        UNSUPPORTED();
    }

    if (d.bad) MEMFAULT();
    c->eip = d.pc;
}

/* ---------- main loop ---------- */

int cpu_run(Machine *m) {
    CpuState *c = &m->cpu;
    while (!m->halted && !m->stop) {
        if (shim_is_thunk(c->eip)) { shim_dispatch(m); continue; }
        if (m->use_jit) {
            jit_fn fn = jit_get_block(m, c->eip);
            if (fn) { c->eip = fn(c, c->mem); continue; }
        }
        step(m);
    }
    if (m->stop && !m->halted) {
        er_log(m, "[cpu] stopped by user\n");
        m->exit_code = -3;
    }
    return m->exit_code;
}
