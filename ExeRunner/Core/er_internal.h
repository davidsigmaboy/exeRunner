#ifndef ER_INTERNAL_H
#define ER_INTERNAL_H

#include "er_api.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ---------- guest CPU ---------- */

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };

/* Lazy flag evaluation: the last flag-setting op is remembered, flags are
   computed only when a Jcc needs them. */
enum { CC_NONE, CC_ADD, CC_SUB, CC_LOGIC, CC_INC, CC_DEC };

/* The JIT addresses these fields by offsetof(); keep it a plain struct. */
typedef struct CpuState {
    uint32_t r[8];
    uint32_t eip;
    uint32_t cc_op, cc_a, cc_b, cc_res;
    uint32_t cf_saved;          /* CF preserved across INC/DEC */
    uint8_t *mem;               /* host base of the guest address space */
} CpuState;

/* ---------- guest address-space layout ---------- */

#define THUNK_BASE    0x00010000u   /* fake addresses that stand in for imported functions */
#define THUNK_STRIDE  16u
#define THUNK_MAX     4096u
#define HEAP_BASE     0x10000000u
#define HEAP_LIMIT    0x18000000u
#define STACK_TOP     0x1FF00000u
#define STACK_SIZE    0x00100000u
#define TEB_BASE      0x1FD00000u   /* fs:[0] points here */
#define PEB_BASE      0x1FD01000u

typedef struct ShimEntry ShimEntry;
struct JitState;

struct Machine {
    CpuState cpu;                       /* keep first */
    size_t   arena_size;
    uint8_t *commit_bits;               /* 1 bit per 4 KiB guest page */

    er_output_cb out;
    void    *out_user;

    volatile int stop;
    int      halted;
    int      exit_code;
    int      loaded;
    int      use_jit;

    uint32_t image_base, image_size, entry;

    const ShimEntry *thunks[THUNK_MAX];
    char    *thunk_label[THUNK_MAX];
    uint32_t thunk_count;

    uint32_t heap_brk, heap_committed;
    uint32_t last_error;
    uint32_t cmdline_addr;
    uint32_t tls[64], tls_next;

    struct JitState *jit;
};

/* ---------- logging / output ---------- */
void er_write(Machine *m, int stream, const char *data, size_t len);
void er_log(Machine *m, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* ---------- guest memory ---------- */
int  mem_reserve(Machine *m);
void mem_release(Machine *m);
int  mem_commit(Machine *m, uint32_t addr, uint32_t size);

static inline int mem_ok(const Machine *m, uint32_t a, uint32_t len) {
    if (len == 0) return 1;
    uint64_t end = (uint64_t)a + len;
    if (end > m->arena_size) return 0;
    uint32_t p0 = a >> 12, p1 = (uint32_t)((end - 1) >> 12);
    for (uint32_t p = p0; p <= p1; p++)
        if (!(m->commit_bits[p >> 3] & (1u << (p & 7)))) return 0;
    return 1;
}
int mem_r8 (Machine *m, uint32_t a, uint32_t *v);
int mem_r16(Machine *m, uint32_t a, uint32_t *v);
int mem_r32(Machine *m, uint32_t a, uint32_t *v);
int mem_w8 (Machine *m, uint32_t a, uint32_t v);
int mem_w32(Machine *m, uint32_t a, uint32_t v);

/* ---------- PE loader ---------- */
int pe_load(Machine *m, const uint8_t *data, size_t len, char *err, size_t err_len);

/* ---------- Win32 shim ---------- */
uint32_t shim_resolve(Machine *m, const char *dll, const char *name);
int      shim_is_thunk(uint32_t eip);
void     shim_dispatch(Machine *m);

/* ---------- CPU ---------- */
int      cpu_run(Machine *m);
uint32_t cpu_eval_cond(const CpuState *c, unsigned cc);

/* ---------- JIT ---------- */
typedef uint32_t (*jit_fn)(CpuState *ctx, uint8_t *mem);   /* returns next guest EIP */
int      jit_supported(void);
int      jit_init(Machine *m);
void     jit_free(Machine *m);
jit_fn   jit_get_block(Machine *m, uint32_t eip);           /* NULL => interpret this instruction */

#endif /* ER_INTERNAL_H */
