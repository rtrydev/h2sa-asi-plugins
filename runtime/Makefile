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

all: ../dist/d3d8.dll ../dist/H2SAWidescreen.asi \
     ../dist/H2SAReducedX87.asi ../dist/H2SAReducedX87-diag.asi \
     ../dist/H2SAProfiler.asi

../dist/d3d8.dll: asiloader.c d3d8.def h2sa_d3d8.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ asiloader.c d3d8.def $(LDFLAGS) \
	      -Wl,--enable-stdcall-fixup -luser32 -lgdi32

../dist/H2SAWidescreen.asi: widescreen.c h2sa_d3d8.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ widescreen.c $(LDFLAGS) -luser32 -lgdi32 -lwinmm

../dist/H2SAReducedX87.asi: h2sax87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

../dist/H2SAReducedX87-diag.asi: h2sax87.c helpers.S
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -DH2SA_DIAG -o $@ $^ $(LDFLAGS)

../dist/H2SAProfiler.asi: profiler.c h2sa_d3d8.h
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ profiler.c $(LDFLAGS) -ld3d8

# memory dumper for the packed hitman2.exe (not part of `all`)
dump: ../dist/H2SADump.asi

../dist/H2SADump.asi: dumper.c
	@mkdir -p ../dist
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f ../dist/d3d8.dll ../dist/H2SAWidescreen.asi \
	      ../dist/H2SAReducedX87.asi ../dist/H2SAReducedX87-diag.asi \
	      ../dist/H2SAProfiler.asi ../dist/H2SADump.asi
