# Cross-compilation Makefile for ap2p (v2.0 single-binary).
# Toolchain: arm-augentix-linux-uclibcgnueabihf (GCC 10.3.0)
#
# Layout (this Makefile lives at src/ap2p/Makefile):
#   <repo>/
#     src/ap2p/                ← $(CURDIR)
#     src/mqtt-vcamclient/deps ← libpaho-mqtt3cs.so + libcurl.so + cJSON
#     third_party/libsrt       ← cmake project; build-arm-…/libsrt.a expected
#     toolchain/               ← default $(TOOLCHAIN_DIR) location
#     bin/                     ← cross-compiled output
#
# Override TOOLCHAIN_DIR / BIN_DIR for CI:
#   make all REPO_ROOT=$GITHUB_WORKSPACE \
#            TOOLCHAIN_DIR=/opt/augentix-tc/arm-augentix-linux-uclibcgnueabihf \
#            BIN_DIR=$GITHUB_WORKSPACE/build/binaries-arm-augentix-linux-uclibcgnueabihf
#
# Phase A note: links libsrt.a STATICALLY and libpaho-mqtt3cs / libcurl /
# OpenSSL DYNAMICALLY (matches the v1.13 provider_srt + MQTT_vcamclient
# behaviour).  Phase B will flip everything static for a single, no-dep
# binary.

# Repo root (two levels up from src/ap2p/).
REPO_ROOT ?= $(CURDIR)/../..

# Toolchain dir — override-friendly.
TOOLCHAIN_DIR    ?= $(REPO_ROOT)/toolchain/arm-augentix-linux-uclibcgnueabihf
TOOLCHAIN_PREFIX ?= arm-augentix-linux-uclibcgnueabihf
SYSROOT           = $(TOOLCHAIN_DIR)/$(TOOLCHAIN_PREFIX)/sysroot
CC                = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-gcc --sysroot=$(SYSROOT)
CXX               = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-g++ --sysroot=$(SYSROOT)
STRIP             = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-strip

# Output binary
BIN_DIR ?= $(REPO_ROOT)/bin
TARGET   = $(BIN_DIR)/ap2p

# ---- third-party paths ----------------------------------------------------
LIBSRT_DIR        ?= $(REPO_ROOT)/third_party/libsrt
LIBSRT_BUILD_ARM   = $(LIBSRT_DIR)/build-arm-augentix-linux-uclibcgnueabihf
LIBSRT_LIB_ARM     = $(LIBSRT_BUILD_ARM)/libsrt.a

# SRT header shim — libsrt's public srt.h lives in srtcore/, but the source
# files include it as <srt/srt.h>.  Mirror the headers under a srt/ subdir.
SRT_SHIM        = $(CURDIR)/.srt_shim
SRT_SHIM_STAMP  = $(SRT_SHIM)/.shim_built

MQTT_DEPS_DIR  = $(REPO_ROOT)/src/mqtt-vcamclient/deps
MQTT_INC       = $(MQTT_DEPS_DIR)/include
MQTT_LIB       = $(MQTT_DEPS_DIR)/lib

# Source files.  cJSON.c is pulled in from the MQTT vcamclient tree because
# config.c uses a simple JSON helper (not cJSON), but if a future iteration
# wants real cJSON parsing the symbol is already in the link.
SRC = main.c \
      state.c \
      config.c \
      mqtt_thread.c \
      srt_thread.c \
      boot_timing.c \
      legacy_cases.c \
      n_https.c \
      cJSON.c
# NB: stream_timing.c intentionally NOT in this list (v2.0.6-rc2 revert).
# Module exists in-tree for v2.0.6.2 — see srt_thread.c top-of-file note.

# ---- compile flags --------------------------------------------------------
# Aggressive size + dead-code elimination.
# -DSRT_STATIC: tells srt.h not to mark its symbols as imported.
# ARCH_CFLAGS: override for toolchains that don't support -mthumb (e.g. armv5/Anyka).
ARCH_CFLAGS ?= -mthumb
CFLAGS  = $(ARCH_CFLAGS) -Os \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -std=c11 -D_GNU_SOURCE -DSRT_STATIC \
          -I$(CURDIR) \
          -I$(SRT_SHIM) -I$(LIBSRT_DIR)/srtcore -I$(LIBSRT_BUILD_ARM) \
          -I$(MQTT_INC)

# ---- link flags -----------------------------------------------------------
# --gc-sections drops every unreferenced .text.* / .data.* section.
# -static-libgcc avoids dragging libgcc_s.so.1 as a NEEDED entry.
# rpath/-rpath-link so the camera-side dynamic loader finds libpaho-mqtt3cs
# and libcurl / libssl in /mny/mtd/ipc/ambicam at runtime.
# --unresolved-symbols=ignore-in-shared-libs lets the link succeed even
# when a recursively-pulled-in symbol from a shared dep is missing on the
# build host — the camera firmware provides them at runtime.
LDFLAGS = -Wl,--gc-sections -static-libgcc \
          -L$(MQTT_LIB) \
          -Wl,-rpath,/mny/mtd/ipc/ambicam \
          -Wl,-rpath-link,$(MQTT_LIB) \
          -Wl,--unresolved-symbols=ignore-in-shared-libs

# libsrt is statically linked because it's not on the camera; libstdc++ is
# pulled in because libsrt is C++.  libpaho-mqtt3cs / libcurl / libssl /
# libcrypto live in /mny/mtd/ipc/ambicam at runtime so we keep them dynamic.
LIBS = $(LIBSRT_LIB_ARM) \
       -static-libstdc++ \
       -lpaho-mqtt3cs -lcurl -lssl -lcrypto \
       -lpthread -lm -ldl

# ---- build rules ----------------------------------------------------------
# Our sources are C11 (use <stdatomic.h>, etc.) — compile each .c with the
# C compiler.  Link with the C++ compiler so that libsrt's libstdc++ runtime
# is pulled in correctly without our C code being run through g++ (which
# rejects <stdatomic.h> and other C-only features).
OBJ_DIR = $(CURDIR)/.objs
OBJS    = $(addprefix $(OBJ_DIR)/, $(SRC:.c=.o))

all: $(BIN_DIR) $(SRT_SHIM_STAMP) $(LIBSRT_LIB_ARM) $(TARGET)
	@echo "Build successful: $(TARGET)"

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CXX) $(OBJS) -o $@ $(LDFLAGS) $(LIBS)
	# Phase C: post-link strip — drop debug_info, .comment, .note.*,
	# .gnu.version*.  Together these are ~70% of the unstripped size.
	$(STRIP) --strip-all $@
	$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-objcopy \
	    --remove-section=.comment \
	    --remove-section=.note.GNU-stack \
	    --remove-section=.note.gnu.build-id \
	    --remove-section=.gnu.version \
	    --remove-section=.gnu.version_r \
	    $@
	# rpath so the camera's dynamic loader finds the .so files next to the
	# binary in /mny/mtd/ipc/ambicam.
	patchelf --set-rpath '/mny/mtd/ipc/ambicam' $@ 2>/dev/null || true
	# Phase D: zero-fill upstream-library banner strings + legacy config-key
	# vocabulary in .rodata so `strings ap2p` reveals nothing about the
	# underlying tech stack.  See branding/scrub.py.
	#
	# IMPORTANT — we do NOT patchelf --replace-needed the DT_NEEDED entries
	# to branded names.  We tried, and uClibc's dynamic loader rejects the
	# SONAME mismatch when loading the symlinked .so (it expects the file's
	# DT_SONAME to equal the requested name; symlinking libap2p_msg.so.1 →
	# libpaho-mqtt3cs.so.1.3.14 leaves the SONAME as the original).  Fixing
	# this properly requires either shipping branded .so copies with their
	# SONAMEs patched (Phase B follow-up) or full static linking.  For
	# v2.0.x we accept that `readelf -d ap2p` exposes the upstream lib
	# names; the brand-verify gate's allowlist permits them.
	python3 $(REPO_ROOT)/branding/scrub.py $@

$(OBJ_DIR)/%.o: %.c $(SRT_SHIM_STAMP) | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# One-time SRT include shim so #include <srt/srt.h> resolves.
$(SRT_SHIM_STAMP):
	@mkdir -p $(SRT_SHIM)/srt
	@cp -f $(LIBSRT_DIR)/srtcore/srt.h            $(SRT_SHIM)/srt/srt.h
	@cp -f $(LIBSRT_DIR)/srtcore/access_control.h $(SRT_SHIM)/srt/access_control.h 2>/dev/null || true
	@cp -f $(LIBSRT_DIR)/srtcore/logging_api.h    $(SRT_SHIM)/srt/logging_api.h    2>/dev/null || true
	@cp -f $(LIBSRT_DIR)/srtcore/platform_sys.h   $(SRT_SHIM)/srt/platform_sys.h   2>/dev/null || true
	@cp -f $(LIBSRT_DIR)/srtcore/udt.h            $(SRT_SHIM)/srt/udt.h            2>/dev/null || true
	@touch $(SRT_SHIM_STAMP)

# libsrt cross-compile is delegated to the top-level build/Makefile, which
# already knows how to invoke cmake with the Augentix toolchain.  We sanity-
# check the .a exists at link time and emit a hint if it doesn't.
$(LIBSRT_LIB_ARM):
	@test -f $(LIBSRT_LIB_ARM) || (echo "✗ libsrt.a missing at $(LIBSRT_LIB_ARM)"; \
	                                echo "  build with: make -C $(REPO_ROOT)/build provider-srt-arm"; \
	                                exit 1)

clean:
	rm -f $(TARGET)
	rm -rf $(SRT_SHIM)

.PHONY: all clean
