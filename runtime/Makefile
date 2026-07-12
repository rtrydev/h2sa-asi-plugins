# mac/Linux: mingw-w64 cross compiler (brew install mingw-w64).
# Windows: MSYS2's 32-bit native gcc (pacman -S mingw-w64-i686-gcc); run
# make with C:\msys64\mingw32\bin on PATH or via ..\build.ps1.
ifeq ($(OS),Windows_NT)
CC := gcc
else
CC := i686-w64-mingw32-gcc
endif
CFLAGS := -O2 -Wall -Wextra -msse2 -static-libgcc
LDFLAGS := -shared -Wl,--kill-at -static -lkernel32

.PHONY: all clean

all: ../dist/d3d8.dll ../dist/h2sa_core.asi \
     ../dist/h2sa_reduced_x87.asi ../dist/h2sa_reduced_x87_diag.asi

../dist/d3d8.dll: asiloader.c d3d8.def h2sa_d3d8.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ asiloader.c d3d8.def $(LDFLAGS) \
	      -Wl,--enable-stdcall-fixup -luser32 -lgdi32

# core plugin: widescreen/startup/mouse fixes + UI scaling + the profiler
# overlay
../dist/h2sa_core.asi: widescreen.c profiler.c uiscale.c h2sa_d3d8.h h2sa_core.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ widescreen.c profiler.c uiscale.c $(LDFLAGS) \
	      -luser32 -lgdi32 -lwinmm

../dist/h2sa_reduced_x87.asi: h2sax87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/h2sa_reduced_x87_diag.asi: h2sax87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -DH2SA_DIAG -o $@ $^ $(LDFLAGS)

# memory dumper for the packed hitman2.exe (not part of `all`)
dump: ../dist/h2sa_dump.asi

../dist/h2sa_dump.asi: dumper.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f ../dist/d3d8.dll ../dist/h2sa_core.asi \
	      ../dist/h2sa_reduced_x87.asi ../dist/h2sa_reduced_x87_diag.asi \
	      ../dist/h2sa_dump.asi
