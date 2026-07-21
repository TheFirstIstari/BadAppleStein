# BadAppleStein Makefile - Build C matching library

CC ?= gcc
CFLAGS = -shared -fPIC -O3

# Platform detection
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
	# macOS - no OpenMP by default, but support libomp
	LIBNAME = libmatch.dylib
	HAVE_OMP = $(shell pkg-config --exists omp 2>/dev/null && echo yes)
	ifeq ($(HAVE_OMP),yes)
		CFLAGS += -fopenmp
	endif
else
	# Linux
	LIBNAME = libmatch.so
	HAVE_OMP = $(shell pkg-config --exists omp 2>/dev/null && echo yes)
	ifeq ($(HAVE_OMP),yes)
		CFLAGS += -fopenmp
	endif
endif

.PHONY: all clean

all: $(LIBNAME)

$(LIBNAME): match.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f *.o *.so *.dylib

# Benchmark targets for performance measurement
.PHONY: bench-arrange bench-render

bench-arrange:
	@which hyperfine > /dev/null || (echo "Install hyperfine: cargo install hyperfine" && exit 1)
	hyperfine --warmup 1 --show-parameter-times --export-json benchmark-arrange.json 'python job1_greedy_arrange.py'

bench-render:
	@which hyperfine > /dev/null || (echo "Install hyperfine: cargo install hyperfine" && exit 1)
	hyperfine --warmup 1 --show-parameter-times --export-json benchmark-render.json 'python job2_greedy_render.py'