/* api.c - public C API used by the Swift app. */
#include "er_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void er_write(Machine *m, int stream, const char *data, size_t len) {
    if (m && m->out && len) m->out(data, len, stream, m->out_user);
}

void er_log(Machine *m, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof buf) n = (int)sizeof buf - 1;
    er_write(m, 2, buf, (size_t)n);
}

Machine *er_create(er_output_cb cb, void *user) {
    Machine *m = (Machine *)calloc(1, sizeof *m);
    if (!m) return NULL;
    m->out = cb;
    m->out_user = user;
    if (mem_reserve(m) != 0) { free(m); return NULL; }
    if (mem_commit(m, STACK_TOP - STACK_SIZE, STACK_SIZE + 0x1000) != 0) {
        mem_release(m);
        free(m);
        return NULL;
    }
    m->heap_brk = m->heap_committed = HEAP_BASE;
    return m;
}

void er_destroy(Machine *m) {
    if (!m) return;
    jit_free(m);
    for (uint32_t i = 0; i < m->thunk_count; i++) free(m->thunk_label[i]);
    mem_release(m);
    free(m);
}

int er_load_exe(Machine *m, const uint8_t *data, size_t len, char *err, size_t err_len) {
    if (!m) return -1;
    return pe_load(m, data, len, err, err_len);
}

int er_run(Machine *m, int use_jit) {
    if (!m || !m->loaded) return -1;
    m->use_jit = 0;
    if (use_jit) {
        if (m->arena_size < 0x100000000ull)
            er_log(m, "[jit] only a %u MiB guest window was available; using interpreter\n",
                   (unsigned)(m->arena_size >> 20));
        else if (!jit_supported())
            er_log(m, "[jit] not enabled for this process; using interpreter\n");
        else if (jit_init(m) != 0)
            er_log(m, "[jit] could not allocate code memory; using interpreter\n");
        else
            m->use_jit = 1;
    }
    return cpu_run(m);
}

void er_stop(Machine *m) { if (m) m->stop = 1; }
int  er_jit_active(Machine *m) { return m ? m->use_jit : 0; }
int  er_jit_available(void) { return jit_supported(); }
