/* Dumps the ARM64 code the JIT emits for a few guest addresses as llvm-mc-readable hex.
   Build with -D__aarch64__ on any host to inspect the output with llvm-mc; the generated
   code is never executed here. Usage: jit_dump file.exe rva [rva...] */
#include "../ExeRunner/Core/jit.c"
#include <stdio.h>
#include <stdlib.h>

static void nop_out(const char *d, size_t n, int s, void *u) { (void)d; (void)n; (void)s; (void)u; }

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    Machine *m = er_create(nop_out, NULL);
    char err[256];
    if (!m || er_load_exe(m, buf, (size_t)n, err, sizeof err)) { fprintf(stderr, "load: %s\n", err); return 1; }
    if (jit_init(m) != 0) { fprintf(stderr, "jit_init failed\n"); return 1; }

    for (int i = 2; i < argc; i++) {
        uint32_t eip = m->image_base + (uint32_t)strtoul(argv[i], NULL, 0);
        size_t before = m->jit->used_words;
        jit_fn fn = translate(m, m->jit, eip);
        fprintf(stderr, "== block at 0x%08X: %s, %zu words\n", eip, fn ? "translated" : "interpret",
                m->jit->used_words - before);
        for (size_t w = before; w < m->jit->used_words; w++) {
            uint32_t x = m->jit->code[w];
            printf("0x%02X 0x%02X 0x%02X 0x%02X\n", x & 0xFF, (x >> 8) & 0xFF, (x >> 16) & 0xFF, x >> 24);
        }
        printf("\n");
    }
    return 0;
}
