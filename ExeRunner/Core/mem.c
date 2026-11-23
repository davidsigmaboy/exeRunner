/* mem.c - guest address space: one big PROT_NONE reservation, committed on demand. */
#include "er_internal.h"
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

int mem_reserve(Machine *m) {
    /* A full 4 GiB window means every 32-bit guest pointer is valid (or faults)
       with no bounds checks in JIT code. Smaller windows work for the interpreter only. */
    static const uint64_t sizes[] = { 0x100000000ull, 0x80000000ull, 0x40000000ull, 0x20000000ull };
    /* ER_ARENA_MB=1024 (or 512/2048) forces a smaller window; CI uses it to test masked JIT. */
    const char *env = getenv("ER_ARENA_MB");
    uint64_t forced = env ? ((uint64_t)strtoull(env, NULL, 10) << 20) : 0;
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        if (forced && sizes[i] != forced) continue;
        if (sizes[i] > (uint64_t)SIZE_MAX) continue;
        void *p = mmap(NULL, (size_t)sizes[i], PROT_NONE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) continue;
        uint8_t *bits = calloc((size_t)(sizes[i] >> 12) / 8, 1);
        if (!bits) { munmap(p, (size_t)sizes[i]); return -1; }
        m->cpu.mem = (uint8_t *)p;
        m->arena_size = (size_t)sizes[i];
        m->commit_bits = bits;
        return 0;
    }
    return -1;
}

void mem_release(Machine *m) {
    if (m->cpu.mem) munmap(m->cpu.mem, m->arena_size);
    free(m->commit_bits);
    m->cpu.mem = NULL;
    m->commit_bits = NULL;
}

int mem_commit(Machine *m, uint32_t addr, uint32_t size) {
    if (size == 0) return 0;
    uint64_t end = (uint64_t)addr + size;
    if (end > m->arena_size) return -1;
    uint64_t hp = (uint64_t)sysconf(_SC_PAGESIZE);   /* 16 KiB on iOS */
    uint64_t s = addr & ~(hp - 1);
    uint64_t e = (end + hp - 1) & ~(hp - 1);
    if (e > m->arena_size) e = m->arena_size;
    if (mprotect(m->cpu.mem + s, (size_t)(e - s), PROT_READ | PROT_WRITE) != 0) return -1;
    for (uint64_t p = addr >> 12; p <= ((end - 1) >> 12); p++)
        m->commit_bits[p >> 3] |= (uint8_t)(1u << (p & 7));
    return 0;
}

int mem_r8(Machine *m, uint32_t a, uint32_t *v) {
    if (!mem_ok(m, a, 1)) return 0;
    *v = m->cpu.mem[a];
    return 1;
}
int mem_r16(Machine *m, uint32_t a, uint32_t *v) {
    uint16_t t;
    if (!mem_ok(m, a, 2)) return 0;
    memcpy(&t, m->cpu.mem + a, 2);
    *v = t;
    return 1;
}
int mem_r32(Machine *m, uint32_t a, uint32_t *v) {
    if (!mem_ok(m, a, 4)) return 0;
    memcpy(v, m->cpu.mem + a, 4);
    return 1;
}
int mem_w8(Machine *m, uint32_t a, uint32_t v) {
    if (!mem_ok(m, a, 1)) return 0;
    m->cpu.mem[a] = (uint8_t)v;
    return 1;
}
int mem_w32(Machine *m, uint32_t a, uint32_t v) {
    if (!mem_ok(m, a, 4)) return 0;
    memcpy(m->cpu.mem + a, &v, 4);
    return 1;
}
