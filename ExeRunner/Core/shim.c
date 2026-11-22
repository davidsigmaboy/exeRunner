/* shim.c - the "translation layer": Win32 / CRT calls implemented on the host side.
   Imports resolve to fake addresses in the thunk range; when the guest jumps there,
   shim_dispatch() reads the arguments off the guest stack, runs the host function,
   and unwinds the guest call. */
#include "er_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct ShimEntry {
    const char *name;
    int nargs;                      /* stdcall: args popped by callee. -1 = cdecl */
    uint32_t (*fn)(Machine *m);
};

/* ---------- helpers ---------- */

static uint32_t A(Machine *m, int i) {
    uint32_t v = 0;
    mem_r32(m, m->cpu.r[R_ESP] + 4u + 4u * (uint32_t)i, &v);
    return v;
}

static const char *gstr(Machine *m, uint32_t a) {
    for (uint32_t i = 0; i < (1u << 20); i++) {
        if (!mem_ok(m, a + i, 1)) return NULL;
        if (m->cpu.mem[a + i] == 0) return (const char *)(m->cpu.mem + a);
    }
    return NULL;
}

static uint32_t guest_alloc(Machine *m, uint32_t size, uint32_t align) {
    if (!size) size = 1;
    if (align < 16) align = 16;
    uint64_t a = ((uint64_t)m->heap_brk + align - 1) & ~(uint64_t)(align - 1);
    uint64_t end = a + ((size + 15u) & ~15u);
    if (end > HEAP_LIMIT) return 0;
    if (end > m->heap_committed) {
        uint64_t want = (end + 0xFFFFFu) & ~0xFFFFFull;
        if (want > HEAP_LIMIT) want = HEAP_LIMIT;
        if (mem_commit(m, m->heap_committed, (uint32_t)(want - m->heap_committed)) != 0) return 0;
        m->heap_committed = (uint32_t)want;
    }
    m->heap_brk = (uint32_t)end;
    return (uint32_t)a;                  /* fresh pages are zero-filled */
}

/* printf-style formatting that pulls its varargs from the guest stack (cdecl). */
static size_t gformat(Machine *m, const char *fmt, int ai, char *out, size_t cap) {
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < cap) out[o] = (ch); o++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        if (*fmt == '%') { PUT('%'); continue; }
        char spec[24]; int sp = 0;
        spec[sp++] = '%';
        while ((*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ') && sp < 6) spec[sp++] = *fmt++;
        while (*fmt >= '0' && *fmt <= '9' && sp < 10) spec[sp++] = *fmt++;
        if (*fmt == '.') {
            spec[sp++] = *fmt++;
            while (*fmt >= '0' && *fmt <= '9' && sp < 14) spec[sp++] = *fmt++;
        }
        while (*fmt == 'l' || *fmt == 'h') fmt++;
        char conv = *fmt;
        if (!conv) break;
        char t[512]; int k = 0;
        switch (conv) {
        case 'd': case 'i':
            spec[sp++] = 'd'; spec[sp] = 0;
            k = snprintf(t, sizeof t, spec, (int)A(m, ai++)); break;
        case 'u': case 'x': case 'X':
            spec[sp++] = conv; spec[sp] = 0;
            k = snprintf(t, sizeof t, spec, (unsigned)A(m, ai++)); break;
        case 'c':
            spec[sp++] = 'c'; spec[sp] = 0;
            k = snprintf(t, sizeof t, spec, (int)A(m, ai++)); break;
        case 's': {
            const char *s = gstr(m, A(m, ai++));
            spec[sp++] = 's'; spec[sp] = 0;
            k = snprintf(t, sizeof t, spec, s ? s : "(null)"); break;
        }
        case 'p':
            k = snprintf(t, sizeof t, "0x%08X", (unsigned)A(m, ai++)); break;
        default:
            k = snprintf(t, sizeof t, "%%%c", conv); break;
        }
        if (k < 0) k = 0;
        if (k >= (int)sizeof t) k = (int)sizeof t - 1;
        for (int i = 0; i < k; i++) PUT(t[i]);
    }
#undef PUT
    if (cap) out[o < cap ? o : cap - 1] = 0;
    return o;
}

/* ---------- kernel32 ---------- */

static uint32_t k_ExitProcess(Machine *m) { m->exit_code = (int)A(m, 0); m->halted = 1; return 0; }

static uint32_t k_GetStdHandle(Machine *m) {
    switch ((int32_t)A(m, 0)) {
    case -10: return 0x100;     /* stdin  */
    case -11: return 0x101;     /* stdout */
    case -12: return 0x102;     /* stderr */
    default:  return 0xFFFFFFFFu;
    }
}

static uint32_t write_common(Machine *m, uint32_t h, uint32_t buf, uint32_t len, uint32_t pw) {
    if (h != 0x101 && h != 0x102) { m->last_error = 6; return 0; }      /* ERROR_INVALID_HANDLE */
    if (!mem_ok(m, buf, len)) { m->last_error = 998; return 0; }        /* ERROR_NOACCESS */
    er_write(m, (int)(h - 0x100), (const char *)m->cpu.mem + buf, len);
    if (pw) mem_w32(m, pw, len);
    return 1;
}
static uint32_t k_WriteFile(Machine *m)     { return write_common(m, A(m, 0), A(m, 1), A(m, 2), A(m, 3)); }
static uint32_t k_WriteConsoleA(Machine *m) { return write_common(m, A(m, 0), A(m, 1), A(m, 2), A(m, 3)); }

static uint32_t k_GetModuleHandleA(Machine *m) { return A(m, 0) == 0 ? m->image_base : 0; }

static uint32_t k_GetCommandLineA(Machine *m) {
    if (!m->cmdline_addr) {
        static const char s[] = "ExeRunner.exe";
        m->cmdline_addr = guest_alloc(m, sizeof s, 16);
        if (m->cmdline_addr) memcpy(m->cpu.mem + m->cmdline_addr, s, sizeof s);
    }
    return m->cmdline_addr;
}

static uint32_t k_GetProcessHeap(Machine *m)  { (void)m; return 0x1000; }
static uint32_t k_HeapAlloc(Machine *m)       { return guest_alloc(m, A(m, 2), 16); }
static uint32_t k_HeapFree(Machine *m)        { (void)m; return 1; }      /* bump allocator: free is a no-op */
static uint32_t k_VirtualAlloc(Machine *m)    { return guest_alloc(m, A(m, 1), 0x1000); }
static uint32_t k_VirtualFree(Machine *m)     { (void)m; return 1; }
static uint32_t k_GetLastError(Machine *m)    { return m->last_error; }
static uint32_t k_SetLastError(Machine *m)    { m->last_error = A(m, 0); return 0; }
static uint32_t k_GetCurrentProcessId(Machine *m) { (void)m; return 1234; }
static uint32_t k_Sleep(Machine *m)           { (void)m; return 0; }

static uint32_t k_GetTickCount(Machine *m) {
    (void)m;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* ---------- user32 ---------- */

static uint32_t u_MessageBoxA(Machine *m) {
    const char *text = gstr(m, A(m, 1)), *cap = gstr(m, A(m, 2));
    char buf[1024];
    int n = snprintf(buf, sizeof buf, "[MessageBox] %s: %s\n", cap ? cap : "", text ? text : "");
    if (n > 0) er_write(m, 1, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
    return 1;   /* IDOK */
}

/* ---------- msvcrt / ucrt (cdecl) ---------- */

static uint32_t c_puts(Machine *m) {
    const char *s = gstr(m, A(m, 0));
    if (!s) return (uint32_t)-1;
    er_write(m, 1, s, strlen(s));
    er_write(m, 1, "\n", 1);
    return 1;
}
static uint32_t c_putchar(Machine *m) { char ch = (char)A(m, 0); er_write(m, 1, &ch, 1); return (uint32_t)(unsigned char)ch; }
static uint32_t c_printf(Machine *m) {
    const char *fmt = gstr(m, A(m, 0));
    if (!fmt) return (uint32_t)-1;
    char buf[4096];
    size_t n = gformat(m, fmt, 1, buf, sizeof buf);
    if (n >= sizeof buf) n = sizeof buf - 1;
    er_write(m, 1, buf, n);
    return (uint32_t)n;
}
static uint32_t c_malloc(Machine *m) { return guest_alloc(m, A(m, 0), 16); }
static uint32_t c_free(Machine *m)   { (void)m; return 0; }
static uint32_t c_strlen(Machine *m) { const char *s = gstr(m, A(m, 0)); return s ? (uint32_t)strlen(s) : 0; }
static uint32_t c_exit(Machine *m)   { m->exit_code = (int)A(m, 0); m->halted = 1; return 0; }

/* ---------- table ---------- */

static const ShimEntry TABLE[] = {
    { "ExitProcess",         1, k_ExitProcess },
    { "GetStdHandle",        1, k_GetStdHandle },
    { "WriteFile",           5, k_WriteFile },
    { "WriteConsoleA",       5, k_WriteConsoleA },
    { "GetModuleHandleA",    1, k_GetModuleHandleA },
    { "GetCommandLineA",     0, k_GetCommandLineA },
    { "GetProcessHeap",      0, k_GetProcessHeap },
    { "HeapAlloc",           3, k_HeapAlloc },
    { "HeapFree",            3, k_HeapFree },
    { "VirtualAlloc",        4, k_VirtualAlloc },
    { "VirtualFree",         3, k_VirtualFree },
    { "GetLastError",        0, k_GetLastError },
    { "SetLastError",        1, k_SetLastError },
    { "GetCurrentProcessId", 0, k_GetCurrentProcessId },
    { "GetTickCount",        0, k_GetTickCount },
    { "Sleep",               1, k_Sleep },
    { "MessageBoxA",         4, u_MessageBoxA },
    { "puts",               -1, c_puts },
    { "putchar",            -1, c_putchar },
    { "printf",             -1, c_printf },
    { "malloc",             -1, c_malloc },
    { "free",               -1, c_free },
    { "strlen",             -1, c_strlen },
    { "exit",               -1, c_exit },
    { "_exit",              -1, c_exit },
};

uint32_t shim_resolve(Machine *m, const char *dll, const char *name) {
    for (uint32_t i = 0; i < m->thunk_count; i++) {
        const char *bang = strchr(m->thunk_label[i], '!');
        if (bang && strcmp(bang + 1, name) == 0) return THUNK_BASE + i * THUNK_STRIDE;
    }
    if (m->thunk_count >= THUNK_MAX) return 0;
    const ShimEntry *e = NULL;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++)
        if (strcmp(TABLE[i].name, name) == 0) { e = &TABLE[i]; break; }
    size_t L = strlen(dll) + strlen(name) + 2;
    char *label = (char *)malloc(L);
    if (!label) return 0;
    snprintf(label, L, "%s!%s", dll, name);
    uint32_t idx = m->thunk_count++;
    m->thunks[idx] = e;
    m->thunk_label[idx] = label;
    return THUNK_BASE + idx * THUNK_STRIDE;
}

int shim_is_thunk(uint32_t eip) {
    return eip >= THUNK_BASE && eip < THUNK_BASE + THUNK_MAX * THUNK_STRIDE;
}

void shim_dispatch(Machine *m) {
    CpuState *c = &m->cpu;
    uint32_t idx = (c->eip - THUNK_BASE) / THUNK_STRIDE;
    uint32_t ret = 0;
    if (idx >= m->thunk_count || !mem_r32(m, c->r[R_ESP], &ret)) {
        er_log(m, "[shim] bad call into thunk area (eip=0x%08X)\n", c->eip);
        m->halted = 1; m->exit_code = -2;
        return;
    }
    const ShimEntry *e = m->thunks[idx];
    if (!e) {
        er_log(m, "[shim] unimplemented import: %s\n", m->thunk_label[idx]);
        m->halted = 1; m->exit_code = -2;
        return;
    }
    uint32_t res = e->fn(m);
    if (m->halted) return;
    c->r[R_EAX] = res;
    c->r[R_ESP] += 4u + (e->nargs > 0 ? 4u * (uint32_t)e->nargs : 0u);
    c->eip = ret;
}
