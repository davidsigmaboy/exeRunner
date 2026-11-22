# ExeRunner

An iOS app that loads 32-bit Windows `.exe` files and runs them through a translation layer:

```
 .exe (PE32/x86)
    │  pe.c      parse headers, map sections, relocate, bind imports
    ▼
 guest CPU
    │  jit.c     x86 → ARM64 block translator (needs JIT)
    │  cpu.c     x86 interpreter (reference + fallback for anything the JIT skips)
    ▼
 Win32 layer
       shim.c    kernel32 / user32 / CRT calls implemented on the host
```

SwiftUI front end: pick an `.exe`, tap Run, console output streams into the app.

## Build

**GitHub Actions (no Mac needed):** every push builds an unsigned `ExeRunner.ipa` (Actions tab → latest *iOS build* run → Artifacts). Sideload it with SideStore / AltStore, which sign it with your Apple ID. Pushes also run the core tests on x86 and on a native ARM64 Linux runner, which executes the JIT for real.

**Locally:**

```sh
brew install xcodegen
xcodegen                 # generates ExeRunner.xcodeproj from project.yml
open ExeRunner.xcodeproj # set your Team + a unique bundle ID, run on a device
```

The simulator is x86_64/arm64 macOS, so JIT works there without extra steps; a real device needs the JIT step below.

## JIT on iOS

iOS only lets an app execute generated code while a debugger is attached. Options:

- Run from Xcode (debugger attached automatically).
- Sideload with SideStore / AltStore and enable JIT with its enabler, or use StikDebug.

The app checks the `CS_DEBUGGED` flag at runtime (`jit_supported()` in `jit.c`). If it is not set, **Run** falls back to the interpreter and says so in the console. Newer iOS versions on recent chips add hardware-enforced code-signing rules, so check that your enabler supports your iOS version.

This cannot ship on the App Store (it executes downloaded code). Sideload or TestFlight only.

## Test the core without Xcode

```sh
make test          # builds the C core on your host, generates build/hello.exe, runs it
make check-jit     # syntax-checks the ARM64 JIT path (clang)
```

`tools/make_test_pe.py` assembles a tiny PE by hand (loop, call/ret, `WriteFile`, `ExitProcess`); expected output is `Hello from x86!` and exit code `115`.

## What works

- PE32 console-style programs: sections, base relocations, import binding
- Interpreter: mov/lea/push/pop/call/ret/jmp/jcc, add/or/and/sub/xor/cmp/test, inc/dec, movzx, byte moves, `call [mem]` for imports
- JIT: register-form ALU, mov (reg/reg, reg/mem without SIB, imm), push/pop, call/jmp/ret, jcc, with lazy flags. Everything else runs in the interpreter at block boundaries
- Shim: ExitProcess, GetStdHandle, WriteFile, WriteConsoleA, heap/VirtualAlloc (bump allocator), GetLastError, GetTickCount, MessageBoxA (logged), `printf`/`puts`/`putchar`/`malloc`/`free`/`strlen`/`exit`

## What does not (yet)

- 64-bit (PE32+) executables, DLL loading
- FPU/SSE, string instructions, shifts, `mul`/`div`, `adc`/`sbb`, 16-bit operand prefix, segment registers (`fs:` / TEB / PEB), so MSVC-built programs with the full CRT startup will stop early
- Any GUI: user32/gdi32 windows, DirectX
- Threads, files, registry, SEH
- JIT: no register allocation, no block chaining, no self-modifying-code invalidation, no host signal handler for guest faults (an unmapped access in JIT code crashes the app; the interpreter reports a clean fault)
- The JIT was verified by disassembling its output, not by executing it on an ARM64 device; expect bugs on first run

Missing imports halt with `[shim] unimplemented import: DLL!Name`, which is the to-do list for whatever program you try. Add them to the table in `shim.c`.

## Good next steps

1. `fs:[0x30]` TEB/PEB stubs and the 0x66 prefix so MinGW/MSVC CRT startup gets through
2. More opcodes (shifts, imul/div, setcc, cmov, string ops), then x87
3. JIT register caching and block chaining
4. A SIGSEGV/SIGBUS handler that maps faults back to guest EIP
5. Swap in a Wine-style user32/gdi32 to a framebuffer view for windowed apps
