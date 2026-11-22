# Host-side build of the emulator core for quick testing (no Xcode needed).
CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -IExeRunner/Core
SRC      = $(wildcard ExeRunner/Core/*.c)

build/erc: $(SRC) tests/cli.c
	@mkdir -p build
	$(CC) $(CFLAGS) $^ -o $@

build/hello.exe: tools/make_test_pe.py
	@mkdir -p build
	python3 tools/make_test_pe.py $@

test: build/erc build/hello.exe
	./build/erc build/hello.exe

# Type-check the ARM64 JIT path on any host (the code is compiled but never run)
check-jit:
	$(CC) -D__aarch64__ -fsyntax-only -Wall -Wextra -IExeRunner/Core ExeRunner/Core/jit.c

clean:
	rm -rf build

.PHONY: test check-jit clean
