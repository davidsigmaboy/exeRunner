#ifndef ER_API_H
#define ER_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque virtual machine: CPU state + 32-bit guest address space + Win32 shim. */
typedef struct Machine Machine;

/* Console output from the guest. stream: 1 = stdout, 2 = stderr / emulator log. */
typedef void (*er_output_cb)(const char *data, size_t len, int stream, void *user);

Machine *er_create(er_output_cb cb, void *user);
void     er_destroy(Machine *m);

/* Parse a PE32 (x86) .exe, map it into guest memory and resolve imports.
   Returns 0 on success; on failure returns -1 and writes a message into err. */
int      er_load_exe(Machine *m, const uint8_t *data, size_t len, char *err, size_t err_len);

/* Run until the guest exits, faults, or er_stop() is called. Returns the exit code.
   use_jit = 1 asks for the ARM64 JIT; falls back to the interpreter if unavailable. */
int      er_run(Machine *m, int use_jit);

/* Thread-safe request to stop a running guest. */
void     er_stop(Machine *m);

/* 1 if this process can currently execute JIT-generated code. */
int      er_jit_available(void);

/* 1 if the last/current er_run() is actually using the JIT. */
int      er_jit_active(Machine *m);

#ifdef __cplusplus
}
#endif

#endif /* ER_API_H */
