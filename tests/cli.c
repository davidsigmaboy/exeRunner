/* Host-side test driver: runs a PE32 .exe through the same C core the iOS app uses. */
#include "er_api.h"
#include <stdio.h>
#include <stdlib.h>

static void out(const char *d, size_t n, int stream, void *u) {
    (void)u;
    fwrite(d, 1, n, stream == 2 ? stderr : stdout);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.exe [--jit]\n", argv[0]); return 2; }
    int jit = argc > 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)n);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); return 2; }
    fclose(f);

    Machine *m = er_create(out, NULL);
    if (!m) { fprintf(stderr, "er_create failed\n"); return 2; }
    char err[256];
    if (er_load_exe(m, buf, (size_t)n, err, sizeof err) != 0) { fprintf(stderr, "load error: %s\n", err); return 1; }
    int code = er_run(m, jit);
    printf("[exit code %d, jit %s]\n", code, er_jit_active(m) ? "on" : "off");
    er_destroy(m);
    free(buf);
    return code == 115 ? 0 : 1;
}
