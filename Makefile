# BadAppleStein — Makefile
# Builds the badapplestein unified binary plus legacy arrange/render tools.
# Portable across macOS arm64/x86_64 and Linux x86_64/arm64.

# ──────────────────────────────────────────────────────────────
# User-overridable variables
# ──────────────────────────────────────────────────────────────

PREFIX    ?= /usr/local
DESTDIR   ?=
VERSION   ?= 1.0.0
SRCDIR    := src

# Compiler: clang on macOS, gcc on Linux
UNAME_S   := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
  CC      ?= clang
else
  CC      ?= gcc
endif

CFLAGS    ?= -O3 -funroll-loops

# ──────────────────────────────────────────────────────────────
# FFmpeg — pkg-config with Homebrew fallback
# ──────────────────────────────────────────────────────────────

HB_FFMPEG_PC := $(shell brew --cellar ffmpeg 2>/dev/null | head -1)/$(shell brew info --json=v2 ffmpeg 2>/dev/null | python3 -c "import sys,json;print(json.load(sys.stdin)['formulae'][0]['versions']['stable'])" 2>/dev/null)/lib/pkgconfig

AV_CFLAGS := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH):$(HB_FFMPEG_PC):/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig" \
               pkg-config --cflags libavformat libavcodec libavutil libswscale 2>/dev/null)
AV_LIBS   := $(shell PKG_CONFIG_PATH="$(PKG_CONFIG_PATH):$(HB_FFMPEG_PC):/opt/homebrew/lib/pkgconfig:/usr/local/lib/pkgconfig" \
               pkg-config --libs   libavformat libavcodec libavutil libswscale 2>/dev/null)

ifeq ($(AV_LIBS),)
  ifeq ($(UNAME_S),Darwin)
    ifneq ($(wildcard /opt/homebrew/include/libavformat/avformat.h),)
      AV_CFLAGS := -I/opt/homebrew/include
      AV_LIBS   := -L/opt/homebrew/lib -lavformat -lavcodec -lavutil -lswscale
    else ifneq ($(wildcard /usr/local/include/libavformat/avformat.h),)
      AV_CFLAGS := -I/usr/local/include
      AV_LIBS   := -L/usr/local/lib -lavformat -lavcodec -lavutil -lswscale
    endif
  endif
  ifeq ($(AV_LIBS),)
    $(error ffmpeg/libav not found. Install with: brew install ffmpeg  or  apt install libavformat-dev)
  endif
endif

# ──────────────────────────────────────────────────────────────
# OpenMP — platform-dependent
# ──────────────────────────────────────────────────────────────

ifeq ($(UNAME_S),Darwin)
  OMP_PREFIX := $(shell brew --prefix libomp 2>/dev/null)
  ifneq ($(wildcard $(OMP_PREFIX)/include/omp.h),)
    OMP_CFLAGS := -Xpreprocessor -fopenmp -I$(OMP_PREFIX)/include
    OMP_LIBS   := -L$(OMP_PREFIX)/lib -lomp
  else
    OMP_CFLAGS :=
    OMP_LIBS   :=
  endif
else
  OMP_CFLAGS := -fopenmp
  OMP_LIBS   := -fopenmp
endif

# ──────────────────────────────────────────────────────────────
# MuPDF — pkg-config with Homebrew fallback (optional)
# ──────────────────────────────────────────────────────────────

MUPDF_CFLAGS := $(shell pkg-config --cflags mupdf 2>/dev/null)
MUPDF_LIBS   := $(shell pkg-config --libs   mupdf 2>/dev/null)

ifeq ($(MUPDF_LIBS),)
  MUPDF_PREFIX := $(shell brew --prefix mupdf 2>/dev/null)
  ifneq ($(wildcard $(MUPDF_PREFIX)/include/mupdf),)
    MUPDF_CFLAGS := -I$(MUPDF_PREFIX)/include
    MUPDF_LIBS   := -L$(MUPDF_PREFIX)/lib -lmupdf -lmupdf-third
  endif
endif

ifneq ($(MUPDF_LIBS),)
  MUPDF_DEFINE := -DHAVE_MUPDF
else
  MUPDF_DEFINE :=
endif

ALL_CFLAGS  := $(CFLAGS) $(OMP_CFLAGS) $(AV_CFLAGS) $(MUPDF_CFLAGS) \
               -DVERSION=\"$(VERSION)\"
ALL_LDFLAGS := $(OMP_LIBS) $(AV_LIBS) $(MUPDF_LIBS) -lm

# ──────────────────────────────────────────────────────────────
# Source lists
# ──────────────────────────────────────────────────────────────

COMMON_SRC := $(SRCDIR)/imgops.c $(SRCDIR)/video.c $(SRCDIR)/cli.c

ARRANGE_SRC := $(SRCDIR)/arrange.c $(SRCDIR)/match.c $(COMMON_SRC)

RENDER_SRC  := $(SRCDIR)/render.c $(SRCDIR)/pdf.c $(SRCDIR)/system_detect.c \
                $(COMMON_SRC)

BUILD_SRC   := $(SRCDIR)/build_library.c $(SRCDIR)/pdf.c $(SRCDIR)/imgops.c \
                $(SRCDIR)/cli.c

UNIFIED_SRC := $(SRCDIR)/main.c $(SRCDIR)/arrange.c $(SRCDIR)/render.c \
               $(SRCDIR)/build_library.c $(SRCDIR)/match.c \
               $(SRCDIR)/pdf.c $(SRCDIR)/system_detect.c $(COMMON_SRC)

# ──────────────────────────────────────────────────────────────
# vt_prores compilation (platform-specific)
# ──────────────────────────────────────────────────────────────

ifeq ($(UNAME_S),Darwin)
VT_SRC := $(SRCDIR)/vt_prores.m
VT_FLAGS := -fobjc-arc -framework Foundation -framework AVFoundation -framework CoreMedia -framework CoreVideo
VT_COMPILE_FLAGS :=
else
VT_SRC := $(SRCDIR)/vt_prores.m
VT_FLAGS :=
VT_COMPILE_FLAGS := -x c
endif

# ──────────────────────────────────────────────────────────────
# Targets
# ──────────────────────────────────────────────────────────────

.PHONY: all badapplestein arrange render build-library clean install uninstall

all: badapplestein

# Unified binary (primary target)
badapplestein: $(SRCDIR)/badapplestein

$(SRCDIR)/badapplestein: $(UNIFIED_SRC) $(VT_SRC)
	$(CC) $(ALL_CFLAGS) $(MUPDF_DEFINE) $(VT_COMPILE_FLAGS) $(UNIFIED_SRC) $(VT_SRC) $(ALL_LDFLAGS) $(VT_FLAGS) -o $@

# Legacy standalone binaries (still useful for development/debugging)
arrange: $(SRCDIR)/arrange
render:  $(SRCDIR)/render
build-library: $(SRCDIR)/build_library

$(SRCDIR)/arrange: $(ARRANGE_SRC)
	$(CC) $(ALL_CFLAGS) $^ $(ALL_LDFLAGS) -o $@

$(SRCDIR)/render: $(RENDER_SRC) $(VT_SRC)
	$(CC) $(ALL_CFLAGS) $(MUPDF_DEFINE) $(VT_COMPILE_FLAGS) $^ $(ALL_LDFLAGS) $(VT_FLAGS) -o $@

$(SRCDIR)/build_library: $(BUILD_SRC)
	$(CC) $(ALL_CFLAGS) $^ $(ALL_LDFLAGS) $(MUPDF_DEFINE) -o $@

# ──────────────────────────────────────────────────────────────
# Man pages
# ──────────────────────────────────────────────────────────────

MAN_DIR     := $(DESTDIR)$(PREFIX)/share/man/man1
MAN_PAGES   := man/badapplestein.1 \
               man/badapplestein-build.1 \
               man/badapplestein-encode.1

# ──────────────────────────────────────────────────────────────
# Install / Uninstall
# ──────────────────────────────────────────────────────────────

INSTALL_BIN  := $(DESTDIR)$(PREFIX)/bin
INSTALL_MAN  := $(DESTDIR)$(PREFIX)/share/man/man1

install: all
	install -d $(INSTALL_BIN)
	install -m 755 $(SRCDIR)/badapplestein $(INSTALL_BIN)/badapplestein
	# Legacy symlinks for backward compatibility
	cd $(INSTALL_BIN) && ln -sf badapplestein arrange
	cd $(INSTALL_BIN) && ln -sf badapplestein render
	# Man pages (if present)
	if [ -d "man" ]; then \
		install -d $(INSTALL_MAN); \
		install -m 644 $(MAN_PAGES) $(INSTALL_MAN)/ 2>/dev/null || true; \
	fi
	@echo "Installed badapplestein to $(INSTALL_BIN)/badapplestein"

uninstall:
	rm -f $(INSTALL_BIN)/badapplestein $(INSTALL_BIN)/arrange $(INSTALL_BIN)/render
	rm -f $(INSTALL_MAN)/badapplestein.1
	rm -f $(INSTALL_MAN)/badapplestein-build.1
	rm -f $(INSTALL_MAN)/badapplestein-encode.1
	@echo "Removed badapplestein from $(INSTALL_BIN)"

# ──────────────────────────────────────────────────────────────
# Clean
# ──────────────────────────────────────────────────────────────

clean:
	rm -f $(SRCDIR)/badapplestein $(SRCDIR)/arrange $(SRCDIR)/render $(SRCDIR)/build_library
	rm -rf $(SRCDIR)/manifests_greedy/
	rm -f $(SRCDIR)/*.mov $(SRCDIR)/*.mp4
	rm -f $(SRCDIR)/temp_master.mov
	rm -rf $(SRCDIR)/test_manifests $(SRCDIR)/test_lib $(SRCDIR)/test_output*
	rm -f benchmark-*.json