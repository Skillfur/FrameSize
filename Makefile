# Host build uses the system compiler. The Steam Frame binary is a static
# aarch64 musl executable so Gaming Mode does not need extra libraries.
# CI installs Zig 0.16.0 and expects `zig` on PATH. Override with ZIG= if needed.
ZIG ?= zig

.PHONY: host aarch64

host: framesize-host
aarch64: FrameSize-aarch64

SRC = x11.c engine.c main.c
HDR = font.h x11.h engine.h

framesize-host: $(SRC) $(HDR)
	gcc -O2 -std=c11 -D_GNU_SOURCE -o $@ $(SRC)

FrameSize-aarch64: $(SRC) $(HDR)
	$(ZIG) cc -target aarch64-linux-musl -static -O2 -std=c11 -s -o $@ $(SRC)
