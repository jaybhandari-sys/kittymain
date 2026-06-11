# Makefile for P2P libjuice (Provider + Consumer API + Signaling Server)

# Paths
# You can override LIBJUICE_DIR if libjuice is in a different location
LIBJUICE_DIR ?= $(CURDIR)/../libjuice
LIBJUICE_BUILD ?= $(LIBJUICE_DIR)/build
LIBJUICE_INCLUDE ?= $(LIBJUICE_DIR)/include
LIBJUICE_LIB = $(LIBJUICE_BUILD)/libjuice.a

# Cross-compiler for Augentix ARM camera.
# We auto-detect between the two common locations:
#   - /root/P2PV4_extracted/toolchains/arm-augentix-...   (cloud server)
#   - /opt/augentix-tc/arm-augentix-...                   (developer WSL)
# Override with TOOLCHAIN_DIR / TOOLCHAIN_PREFIX if you use a different one:
#   make provider-arm TOOLCHAIN_DIR=/path TOOLCHAIN_PREFIX=arm-linux-gnueabihf
TOOLCHAIN_PREFIX ?= arm-augentix-linux-uclibcgnueabihf
TOOLCHAIN_DIR ?= $(shell \
    if [ -x /opt/augentix-tc/$(TOOLCHAIN_PREFIX)/bin/$(TOOLCHAIN_PREFIX)-gcc ]; then \
        echo /opt/augentix-tc/$(TOOLCHAIN_PREFIX); \
    elif [ -x /root/P2PV4_extracted/toolchains/$(TOOLCHAIN_PREFIX)/$(TOOLCHAIN_PREFIX)/bin/$(TOOLCHAIN_PREFIX)-gcc ]; then \
        echo /root/P2PV4_extracted/toolchains/$(TOOLCHAIN_PREFIX)/$(TOOLCHAIN_PREFIX); \
    else \
        echo /opt/augentix-tc/$(TOOLCHAIN_PREFIX); \
    fi)
CC_ARM = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-gcc
CXX_ARM = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-g++
AR_ARM = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ar
RANLIB_ARM = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ranlib
STRIP_ARM = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-strip
LIBJUICE_BUILD_ARM = $(LIBJUICE_DIR)/build-arm-augentix-linux-uclibcgnueabihf
LIBJUICE_LIB_ARM = $(LIBJUICE_BUILD_ARM)/libjuice.a
ARM_BIN_DIR = binaries-arm-augentix-linux-uclibcgnueabihf
PROVIDER_ARM_BIN = $(ARM_BIN_DIR)/provider
ARM_CFLAGS ?= -march=armv7-a

# Compiler settings
# Explicitly use native compilers (not cross-compilers from PATH).
# -B/usr/bin/ forces GCC to find 'as', 'ld', etc. in /usr/bin/ first, which
# prevents the ARM cross-toolchain's plain 'as' symlink from being picked up
# when that toolchain's bin directory is in PATH.
CC = /usr/bin/gcc
CXX = /usr/bin/g++
NATIVE_B = -B/usr/bin/

# Standard build flags (optimized for speed)
CFLAGS = -Wall -Wextra -O2 $(NATIVE_B) -I$(LIBJUICE_INCLUDE) -isystem /usr/include
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 $(NATIVE_B)
LDFLAGS = $(LIBJUICE_LIB) -lpthread -lm -L/usr/lib/x86_64-linux-gnu
LDFLAGS_DYNAMIC = $(LIBJUICE_LIB) -lpthread -lm -L/usr/lib/x86_64-linux-gnu

# Size-optimized build flags (for embedded/hardware with size constraints)
CFLAGS_SMALL = -Wall -Os $(NATIVE_B) -I$(LIBJUICE_INCLUDE) -isystem /usr/include \
               -ffunction-sections -fdata-sections \
               -fno-unwind-tables -fno-asynchronous-unwind-tables
CXXFLAGS_SMALL = -std=c++17 -Wall -Os $(NATIVE_B) \
                 -ffunction-sections -fdata-sections \
                 -fno-unwind-tables -fno-asynchronous-unwind-tables
LDFLAGS_SMALL = $(LIBJUICE_LIB) -lpthread -lm -static \
                -L/usr/lib/x86_64-linux-gnu \
                -Wl,--gc-sections -Wl,--strip-all -s

# libsrt (used by provider_srt + consumer_srt_p2p — the SRT P2P pipeline).
# Same sibling-of-the-repo layout as libjuice.  Cloud has these at:
#   /P2PV4_extracted/Signaling_Server/libsrt/build-x86_64-linux/libsrt.a
#   /P2PV4_extracted/Signaling_Server/libsrt/build-arm-augentix-linux-uclibcgnueabihf/libsrt.a
LIBSRT_DIR        ?= $(CURDIR)/../libsrt
LIBSRT_BUILD_X64   = $(LIBSRT_DIR)/build-x86_64-linux
LIBSRT_LIB_X64     = $(LIBSRT_BUILD_X64)/libsrt.a
LIBSRT_BUILD_ARM   = $(LIBSRT_DIR)/build-arm-augentix-linux-uclibcgnueabihf
LIBSRT_LIB_ARM     = $(LIBSRT_BUILD_ARM)/libsrt.a
# libsrt prebuilts bundled with this tree are sometimes built with GCC 13+/glibc ≥2.38,
# which reference __isoc23_sscanf / __isoc23_strtol absent on older Ubuntu (glibc 2.35).
# $(LIBSRT_NATIVE_OK) detects that and invokes libsrt-x64 before linking native SRT apps.
LIBSRT_NATIVE_OK = $(LIBSRT_BUILD_X64)/.libsrt_x64_host_checked

# Source files
# SRC_ROOT lets each component land in its own subdir under src/ while
# keeping the Makefile recipes simple.  In this repo:
#   src/provider-srt/provider_srt.c, http_api.h, stun_lite.h
#   src/signaling-server/signaling_server.cpp
#   src/consumer-srt-cloud/consumer_srt.c (+ legacy libjuice consumer_api.c)
#   src/consumer-srt-android/consumer_srt_p2p.c
# (the legacy libjuice provider_configurable.c lives next to provider_srt.c
#  under src/provider-srt/ as _legacy_libjuice_provider_configurable.c)
SRC_ROOT              ?= ../src
PROVIDER_SRC          = $(SRC_ROOT)/provider-srt/_legacy_libjuice_provider_configurable.c
SIGNALING_SERVER_SRC  = $(SRC_ROOT)/signaling-server/signaling_server.cpp
CONSUMER_SRC          = $(SRC_ROOT)/consumer-srt-cloud/_legacy_libjuice_consumer_api.c
CONSUMER_HEADERS      = $(SRC_ROOT)/provider-srt/http_api.h
PROVIDER_SRT_SRC      = $(SRC_ROOT)/provider-srt/provider_srt.c
CONSUMER_SRT_P2P_SRC  = $(SRC_ROOT)/consumer-srt-android/consumer_srt_p2p.c
SRT_HEADERS           = $(SRC_ROOT)/provider-srt/stun_lite.h

# Output binaries
BIN_DIR = binaries
PROVIDER_BIN                = $(BIN_DIR)/provider
CONSUMER_BIN                = $(BIN_DIR)/consumer_api
SIGNALING_SERVER_BIN        = $(BIN_DIR)/signaling_server
SIGNALING_SERVER_STATIC_BIN = $(BIN_DIR)/signaling_server.static
PROVIDER_SRT_BIN            = $(BIN_DIR)/provider_srt
CONSUMER_SRT_P2P_BIN        = $(BIN_DIR)/consumer_srt_p2p
CONSUMER_SRT_P2P_STATIC_BIN = $(BIN_DIR)/consumer_srt_p2p.static
PROVIDER_SRT_ARM_BIN        = $(ARM_BIN_DIR)/provider_srt

# SRT include shim — libsrt's public srt.h lives in srtcore/, but the source
# files include it as <srt/srt.h>.  We mirror the headers under a srt/ subdir
# in a shim that's added to the include path.  Built once per `make`.
SRT_SHIM       = $(SRC_ROOT)/consumer-srt-android/srt-include-shim
SRT_SHIM_STAMP = $(SRT_SHIM)/.stamp

.PHONY: all clean clean-arm clean-srt clean-libsrt-x64 strip help provider consumer consumer_api signaling \
        signaling-static libsrt-x64 libsrt-verify-x64 provider-srt consumer-srt-p2p provider-srt-arm \
        check-deps install uninstall provider-small provider-arm libjuice-arm \
        debug-asan debug-tsan

# 'make all' builds every binary: the three native x86_64 artifacts plus the
# ARM provider cross-compiled with the Augentix toolchain that ships under
# toolchains/ in this repository.  Toolchain paths can be overridden via
# TOOLCHAIN_DIR / TOOLCHAIN_PREFIX on the command line.
ALL_BINS = $(PROVIDER_BIN) $(CONSUMER_BIN) \
           $(SIGNALING_SERVER_BIN) $(SIGNALING_SERVER_STATIC_BIN) \
           $(PROVIDER_ARM_BIN) \
           $(PROVIDER_SRT_BIN) $(CONSUMER_SRT_P2P_BIN) $(CONSUMER_SRT_P2P_STATIC_BIN) \
           $(PROVIDER_SRT_ARM_BIN)

# Default target: build all binaries (native + ARM cross-compile)
all: $(ALL_BINS)
	@echo ""
	@echo "════════════════════════════════════════════"
	@echo " Build complete!"
	@echo "════════════════════════════════════════════"
	@echo ""
	@echo "Native binaries:"
	@ls -lh $(BIN_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "ARM cross-compiled binaries:"
	@ls -lh $(ARM_BIN_DIR)/ 2>/dev/null || true
	@echo ""
	@echo "Available binaries:"
	@echo "  $(PROVIDER_BIN)         - Provider (x86_64)"
	@echo "  $(CONSUMER_BIN)         - Consumer with API (x86_64)"
	@echo "  $(SIGNALING_SERVER_BIN) - Signaling Server (x86_64)"
	@echo "  $(PROVIDER_ARM_BIN) - Provider (ARM, Augentix)"
	@echo ""

# Provider (native)
$(PROVIDER_BIN): $(PROVIDER_SRC)
	@mkdir -p $(BIN_DIR)
	@echo "Building provider..."
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
	@echo "✓ Provider built"

# Signaling Server (native)
$(SIGNALING_SERVER_BIN): $(SIGNALING_SERVER_SRC)
	@mkdir -p $(BIN_DIR)
	@echo "Building signaling server..."
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread
	@echo "✓ Signaling server built"

# Consumer with API (native)
$(CONSUMER_BIN): $(CONSUMER_SRC) $(CONSUMER_HEADERS)
	@mkdir -p $(BIN_DIR)
	@echo "Building consumer with API..."
	$(CC) $(CFLAGS) $(CONSUMER_SRC) -o $@ $(LDFLAGS_DYNAMIC)
	@echo "✓ Consumer with API built"

# Cross-compile libjuice.a for the Augentix toolchain.
# Runs only when libjuice.a for this target is missing.  Uses CMake with an
# in-line toolchain override so the user does not have to edit any cmake
# toolchain file.
$(LIBJUICE_LIB_ARM):
	@echo "Cross-compiling libjuice for $(TOOLCHAIN_PREFIX)..."
	@test -x $(CC_ARM) || (echo "✗ Toolchain gcc not executable: $(CC_ARM)" && exit 1)
	@command -v cmake >/dev/null 2>&1 || (echo "✗ cmake not installed (apt-get install -y cmake)" && exit 1)
	@chmod +x "$(TOOLCHAIN_DIR)"/bin/* 2>/dev/null || true
	@chmod +x "$(TOOLCHAIN_DIR)"/libexec/gcc/*/*/* 2>/dev/null || true
	@ln -sf "$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-as" "$(TOOLCHAIN_DIR)/bin/as" 2>/dev/null || true
	@ln -sf "$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ld" "$(TOOLCHAIN_DIR)/bin/ld" 2>/dev/null || true
	@mkdir -p $(LIBJUICE_BUILD_ARM)
	cd $(LIBJUICE_BUILD_ARM) && PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" cmake $(LIBJUICE_DIR) \
	    -DCMAKE_SYSTEM_NAME=Linux \
	    -DCMAKE_SYSTEM_PROCESSOR=arm \
	    -DCMAKE_C_COMPILER=$(CC_ARM) \
	    -DCMAKE_CXX_COMPILER=$(CXX_ARM) \
	    -DCMAKE_AR=$(AR_ARM) \
	    -DCMAKE_RANLIB=$(RANLIB_ARM) \
	    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
	    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
	    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DNO_TESTS=1 -DNO_EXAMPLES=1 \
	    -DDISABLE_CONSENT_FRESHNESS=ON \
	    -DCMAKE_C_FLAGS="-Os -ffunction-sections -fdata-sections $(ARM_CFLAGS)"
	cd $(LIBJUICE_BUILD_ARM) && PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(MAKE) -j$$(nproc)
	@test -f $(LIBJUICE_LIB_ARM) || (echo "✗ libjuice.a not produced at $(LIBJUICE_LIB_ARM)" && exit 1)
	@echo "✓ libjuice.a built: $(LIBJUICE_LIB_ARM)"

# ARM provider for Augentix camera (cross-compiled).
# Depends on libjuice.a so the first run transparently builds libjuice.
$(PROVIDER_ARM_BIN): $(PROVIDER_SRC) $(LIBJUICE_LIB_ARM)
	@mkdir -p $(ARM_BIN_DIR)
	@echo "Building ARM provider for Augentix camera..."
	@chmod +x "$(TOOLCHAIN_DIR)"/bin/* 2>/dev/null || true
	@chmod +x "$(TOOLCHAIN_DIR)"/libexec/gcc/*/*/* 2>/dev/null || true
	@ln -sf "$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-as" "$(TOOLCHAIN_DIR)/bin/as" 2>/dev/null || true
	@ln -sf "$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ld" "$(TOOLCHAIN_DIR)/bin/ld" 2>/dev/null || true
	PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(CC_ARM) -B"$(TOOLCHAIN_DIR)/bin/" -Wall -Os \
	    $(ARM_CFLAGS) \
	    -I$(LIBJUICE_INCLUDE) \
	    -ffunction-sections -fdata-sections \
	    -fno-unwind-tables -fno-asynchronous-unwind-tables \
	    $(PROVIDER_SRC) \
	    -o $(PROVIDER_ARM_BIN) \
	    -L$(LIBJUICE_BUILD_ARM) -ljuice -lpthread -lm -static \
	    -Wl,--gc-sections -Wl,--strip-all -s
	PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(STRIP_ARM) $(PROVIDER_ARM_BIN) 2>/dev/null || true
	@echo "✓ ARM provider built: $(PROVIDER_ARM_BIN)"
	@ls -lh $(PROVIDER_ARM_BIN)
	@file $(PROVIDER_ARM_BIN) 2>/dev/null || true

provider-arm: $(PROVIDER_ARM_BIN)

# Convenience alias: build libjuice.a for ARM only.
libjuice-arm: $(LIBJUICE_LIB_ARM)
	@echo "✓ libjuice.a ready: $(LIBJUICE_LIB_ARM)"

# Build only provider (native)
provider: $(PROVIDER_BIN)
	@echo "✓ Provider ready: $(PROVIDER_BIN)"
	@ls -lh $(PROVIDER_BIN)

# Build only consumer (consumer_api binary)
consumer: $(CONSUMER_BIN)
	@echo "✓ Consumer with API ready: $(CONSUMER_BIN)"
consumer_api: consumer

# Build size-optimized provider (for hardware with size constraints)
provider-small: $(PROVIDER_SRC)
	@mkdir -p $(BIN_DIR)
	@echo "Building size-optimized provider..."
	@echo "Using size optimization flags: -Os, -ffunction-sections, -fdata-sections, --gc-sections, -s"
	$(CC) $(CFLAGS_SMALL) $< -o $(PROVIDER_BIN) $(LDFLAGS_SMALL)
	@echo "✓ Size-optimized provider built"
	@echo ""
	@ls -lh $(PROVIDER_BIN)
	@echo ""
	@echo "Size comparison:"
	@echo "  Standard build:  ~6MB (with -O2)"
	@echo "  Optimized build: $(shell ls -lh $(PROVIDER_BIN) | awk '{print $$5}')"

# Static-linked signaling server — this is the binary that actually gets
# deployed to the cloud (Ubuntu 22) so it runs across glibc versions without
# surprises.  deploy_signaling.sh ships this and renames it on the cloud.
$(SIGNALING_SERVER_STATIC_BIN): $(SIGNALING_SERVER_SRC)
	@mkdir -p $(BIN_DIR)
	@echo "Building signaling server (static, for cloud deploy)..."
	$(CXX) -std=c++17 -O2 -Wall $(NATIVE_B) \
	    -static -static-libgcc -static-libstdc++ \
	    $< -lpthread \
	    -o $@
	@echo "✓ Signaling server (static) built"
	@ls -lh $@

# Build only signaling server (dynamic — local dev/testing)
signaling: $(SIGNALING_SERVER_BIN)
	@echo "✓ Signaling server ready: $(SIGNALING_SERVER_BIN)"

# Build only the static signaling server
signaling-static: $(SIGNALING_SERVER_STATIC_BIN)
	@echo "✓ Signaling server (static) ready: $(SIGNALING_SERVER_STATIC_BIN)"

# ─────────────────────────────────────────────────────────────────────────────
#  SRT pipeline targets (provider_srt + consumer_srt_p2p, x86_64 + ARM)
# ─────────────────────────────────────────────────────────────────────────────
# libsrt's public header lives at $(LIBSRT_DIR)/srtcore/srt.h, but our source
# files include it as <srt/srt.h>.  This shim mirrors the headers under a
# srt/ subdir.  Stamp file makes the shim a one-time setup per build.
$(SRT_SHIM_STAMP): | $(LIBSRT_DIR)/srtcore/srt.h
	@mkdir -p $(SRT_SHIM)/srt
	@cp -f $(LIBSRT_DIR)/srtcore/srt.h            $(SRT_SHIM)/srt/srt.h
	@cp -f $(LIBSRT_DIR)/srtcore/access_control.h $(SRT_SHIM)/srt/access_control.h 2>/dev/null || true
	@cp -f $(LIBSRT_DIR)/srtcore/logging_api.h    $(SRT_SHIM)/srt/logging_api.h    2>/dev/null || true
	@cp -f $(LIBSRT_DIR)/srtcore/platform_sys.h   $(SRT_SHIM)/srt/platform_sys.h   2>/dev/null || true
	@cp -f $(LIBSRT_DIR)/srtcore/udt.h            $(SRT_SHIM)/srt/udt.h            2>/dev/null || true
	@touch $(SRT_SHIM_STAMP)

# Refresh native libsrt.a from source with this machine's GCC so libc symbols match glibc here.
.PHONY: libsrt-x64 libsrt-verify-x64
# Optional: `make libsrt-verify-x64` — same as updating $(LIBSRT_NATIVE_OK).
libsrt-verify-x64: $(LIBSRT_NATIVE_OK)

$(LIBSRT_NATIVE_OK): $(LIBSRT_LIB_X64)
	@mkdir -p $(LIBSRT_BUILD_X64)
	@if nm $(LIBSRT_LIB_X64) 2>/dev/null | grep -q '__isoc23_'; then \
	    echo >&2 ''; \
	    echo >&2 'Rebuilding libsrt.a: bundled archive targets newer glibc (≥2.38), incompatible with host.'; \
	    echo >&2 '          Use: make libsrt-x64'; \
	    $(MAKE) libsrt-x64; \
	fi
	@touch $@

# After clean-libsrt-x64 this rule repopulates libsrt.a whenever it is absent or CMakeLists changed.
$(LIBSRT_LIB_X64): $(LIBSRT_DIR)/CMakeLists.txt
	$(MAKE) libsrt-x64

libsrt-x64:
	@test -f $(LIBSRT_DIR)/CMakeLists.txt || (echo "✗ libsrt sources not found at $(LIBSRT_DIR)" && exit 1)
	@command -v cmake >/dev/null 2>&1 || (echo "✗ cmake not installed (apt-get install -y cmake)" && exit 1)
	@echo "Reconfiguring libsrt for native x86_64 ($(CC) / $(CXX))..."
	@rm -rf "$(LIBSRT_BUILD_X64)"
	@mkdir -p "$(LIBSRT_BUILD_X64)"
	@# Force host /usr/bin tools: an ARM toolchain earlier in PATH makes `as` wrong for x86 CMake tests.
	PATH="/usr/bin:/bin:$$PATH" cmake -S "$(LIBSRT_DIR)" -B "$(LIBSRT_BUILD_X64)" \
	    -DCMAKE_C_COMPILER="$(CC)" \
	    -DCMAKE_CXX_COMPILER="$(CXX)" \
	    -DCMAKE_C_FLAGS="$(NATIVE_B)" \
	    -DCMAKE_CXX_FLAGS="$(NATIVE_B)" \
	    -DCMAKE_BUILD_TYPE=Release \
	    -DENABLE_APPS=OFF \
	    -DENABLE_SHARED=OFF \
	    -DENABLE_STATIC=ON \
	    -DENABLE_TESTING=OFF \
	    -DENABLE_UNITTESTS=OFF \
	    -DENABLE_ENCRYPTION=OFF
	PATH="/usr/bin:/bin:$$PATH" cmake --build "$(LIBSRT_BUILD_X64)" -j $$(nproc)
	@echo "✓ libsrt built: $(LIBSRT_LIB_X64)"
	@rm -f "$(LIBSRT_NATIVE_OK)"

SRT_CFLAGS_X64 = -O2 -Wall -Wno-missing-field-initializers -DSRT_STATIC -std=c99 \
                 -I$(SRT_SHIM) -I$(LIBSRT_DIR)/srtcore -I$(LIBSRT_BUILD_X64) -Isrc

# =============================================================================
# TLS posture
# =============================================================================
# First rollout intentionally defaults to KITTY_NO_TLS — keep the existing
# plain-TCP behaviour bit-for-bit while we validate the v1.13 changes
# (multi-thread restructure, watchdog, log rotation, …) end-to-end on a
# real camera.  Once that's stable on the fleet, a follow-up release will
# flip the default to KITTY_TLS and roll out the mTLS path camera-by-camera.
#
# To build the TLS-on variant for testing now:
#     make provider-srt     KITTY_TLS_FLAG=-DKITTY_TLS
#     make provider-srt-arm KITTY_TLS_FLAG=-DKITTY_TLS
#
# To audit the source — note the TLS code is always present in the .c file;
# only the link step and the matching #ifdef KITTY_TLS blocks get omitted
# when KITTY_NO_TLS is set.  Compile-checked clean under both modes with
# -Wall -Wextra -Werror.
KITTY_TLS_FLAG ?= -DKITTY_NO_TLS
KITTY_TLS_LIBS ?= -lssl -lcrypto

$(PROVIDER_SRT_BIN): $(LIBSRT_NATIVE_OK) $(PROVIDER_SRT_SRC) $(SRT_HEADERS) $(SRT_SHIM_STAMP) $(LIBSRT_LIB_X64)
	@mkdir -p $(BIN_DIR)
	@echo "Building provider_srt (x86_64, $(if $(filter -DKITTY_NO_TLS,$(KITTY_TLS_FLAG)),plain,TLS+mTLS))..."
	@$(CC) $(NATIVE_B) $(SRT_CFLAGS_X64) $(KITTY_TLS_FLAG) -c $(PROVIDER_SRT_SRC) -o /tmp/provider_srt.o
	@$(CXX) $(NATIVE_B) -O2 /tmp/provider_srt.o $(LIBSRT_LIB_X64) \
	    $(if $(filter -DKITTY_NO_TLS,$(KITTY_TLS_FLAG)),,$(KITTY_TLS_LIBS)) \
	    -lpthread -ldl -o $@
	@echo "✓ provider_srt (x86_64) built"

provider-srt: $(PROVIDER_SRT_BIN)

# x86_64 consumer_srt_p2p (dynamic — local dev / regression testing)
$(CONSUMER_SRT_P2P_BIN): $(LIBSRT_NATIVE_OK) $(CONSUMER_SRT_P2P_SRC) $(SRT_HEADERS) $(SRT_SHIM_STAMP) $(LIBSRT_LIB_X64)
	@mkdir -p $(BIN_DIR)
	@echo "Building consumer_srt_p2p (x86_64, dynamic)..."
	@$(CC) $(NATIVE_B) $(SRT_CFLAGS_X64) -c $(CONSUMER_SRT_P2P_SRC) -o /tmp/consumer_srt_p2p.o
	@$(CXX) $(NATIVE_B) -O2 /tmp/consumer_srt_p2p.o $(LIBSRT_LIB_X64) -lpthread -ldl -o $@
	@echo "✓ consumer_srt_p2p (x86_64, dynamic) built"

# x86_64 consumer_srt_p2p (static — push to cloud for regression test of pipe)
$(CONSUMER_SRT_P2P_STATIC_BIN): $(LIBSRT_NATIVE_OK) $(CONSUMER_SRT_P2P_SRC) $(SRT_HEADERS) $(SRT_SHIM_STAMP) $(LIBSRT_LIB_X64)
	@mkdir -p $(BIN_DIR)
	@echo "Building consumer_srt_p2p (x86_64, static)..."
	@$(CC) $(NATIVE_B) $(SRT_CFLAGS_X64) -c $(CONSUMER_SRT_P2P_SRC) -o /tmp/consumer_srt_p2p_static.o
	@$(CXX) $(NATIVE_B) -O2 /tmp/consumer_srt_p2p_static.o $(LIBSRT_LIB_X64) \
	    -static -static-libgcc -static-libstdc++ -lpthread -ldl -o $@ 2>&1 | tail -5
	@echo "✓ consumer_srt_p2p.static built"

consumer-srt-p2p: $(CONSUMER_SRT_P2P_BIN) $(CONSUMER_SRT_P2P_STATIC_BIN)

# ARM provider_srt (the actual binary that runs on the camera).
# Cross-compiled with the Augentix toolchain and statically linked against
# libsrt + libstdc++ since the camera firmware doesn't ship with them.
$(PROVIDER_SRT_ARM_BIN): $(PROVIDER_SRT_SRC) $(SRT_HEADERS) $(SRT_SHIM_STAMP) $(LIBSRT_LIB_ARM)
	@mkdir -p $(ARM_BIN_DIR)
	@test -x $(CC_ARM)  || (echo "✗ Toolchain gcc not executable: $(CC_ARM)" && exit 1)
	@test -x $(CXX_ARM) || (echo "✗ Toolchain g++ not executable: $(CXX_ARM)" && exit 1)
	@echo "Building provider_srt (ARM Augentix)..."
	@chmod +x "$(TOOLCHAIN_DIR)"/bin/* 2>/dev/null || true
	@chmod +x "$(TOOLCHAIN_DIR)"/libexec/gcc/*/*/* 2>/dev/null || true
	@ln -sf "$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-as" "$(TOOLCHAIN_DIR)/bin/as" 2>/dev/null || true
	@ln -sf "$(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-ld" "$(TOOLCHAIN_DIR)/bin/ld" 2>/dev/null || true
	@PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(CC_ARM) -B"$(TOOLCHAIN_DIR)/bin/" \
	    -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	    -Os -Wall -Wno-missing-field-initializers \
	    -ffunction-sections -fdata-sections \
	    -fno-unwind-tables -fno-asynchronous-unwind-tables \
	    -DSRT_STATIC -std=c99 $(KITTY_TLS_FLAG) \
	    -I$(SRT_SHIM) -I$(LIBSRT_DIR)/srtcore -I$(LIBSRT_BUILD_ARM) -Isrc \
	    -c $(PROVIDER_SRT_SRC) -o /tmp/provider_srt.arm.o
	@# ARM link line.  For TLS-on builds we link AGAINST -lssl -lcrypto
	@# dynamically — the camera firmware already ships libssl.so.3 +
	@# libcrypto.so.3 in /usr/lib/.  We deliberately do NOT static-link
	@# OpenSSL on ARM: the static libs aren't in our toolchain sysroot,
	@# and dynamic linking saves ~2 MB on the camera's flash.
	@PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(CXX_ARM) -Os \
	    -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard \
	    /tmp/provider_srt.arm.o $(LIBSRT_LIB_ARM) \
	    -static-libgcc -static-libstdc++ \
	    $(if $(filter -DKITTY_NO_TLS,$(KITTY_TLS_FLAG)),,-lssl -lcrypto) \
	    -lpthread -lm -ldl \
	    -Wl,--gc-sections -Wl,--strip-all \
	    -o $@
	@PATH="$(TOOLCHAIN_DIR)/bin:$$PATH" $(STRIP_ARM) $@ 2>/dev/null || true
	@echo "✓ provider_srt (ARM) built"
	@ls -lh $@
	@file $@ 2>/dev/null || true

provider-srt-arm: $(PROVIDER_SRT_ARM_BIN)


# Strip debug symbols (native binaries only)
strip: all
	@echo "Stripping debug symbols..."
	strip $(PROVIDER_BIN) $(CONSUMER_BIN) $(SIGNALING_SERVER_BIN)
	@echo "✓ Binaries stripped"
	@echo ""
	@ls -lh $(BIN_DIR)/
	@echo ""

# Clean all binaries (native + ARM, libjuice + SRT).
# Does NOT wipe the cross-compiled libjuice/libsrt build dirs; use
# 'make clean-arm' (libjuice ARM) or 'make clean-srt' (SRT shim) for that.
clean:
	@echo "Cleaning build artifacts..."
	rm -f $(PROVIDER_BIN) $(CONSUMER_BIN) \
	      $(SIGNALING_SERVER_BIN) $(SIGNALING_SERVER_STATIC_BIN) \
	      $(PROVIDER_ARM_BIN) \
	      $(PROVIDER_SRT_BIN) $(CONSUMER_SRT_P2P_BIN) $(CONSUMER_SRT_P2P_STATIC_BIN) \
	      $(PROVIDER_SRT_ARM_BIN)
	@echo "✓ Cleaned all binaries"

# Remove the ARM libjuice build directory to force a fresh cross-compile.
clean-arm:
	@echo "Cleaning ARM libjuice artifacts..."
	rm -rf $(LIBJUICE_BUILD_ARM)
	rm -f $(PROVIDER_ARM_BIN)
	@echo "✓ Cleaned ARM libjuice build"

# Remove the SRT include shim so the next build refreshes it.  Use this if
# you've upgraded libsrt and need the shim to pick up new headers.
clean-srt:
	@echo "Cleaning SRT shim..."
	rm -rf $(SRT_SHIM)
	rm -f $(PROVIDER_SRT_BIN) $(CONSUMER_SRT_P2P_BIN) $(CONSUMER_SRT_P2P_STATIC_BIN) \
	      $(PROVIDER_SRT_ARM_BIN)
	@echo "✓ Cleaned SRT artifacts + shim"

clean-libsrt-x64:
	@echo "Removing native libsrt build + host check stamp..."
	rm -rf $(LIBSRT_BUILD_X64)
	@echo "✓ Next x86 SRT link will reinstall cmake build (run make libsrt-x64 or make all)"

# Check if dependencies are installed
check-deps:
	@echo "Checking dependencies..."
	@which gcc > /dev/null || (echo "✗ gcc not found" && exit 1)
	@echo "✓ gcc found"
	@which g++ > /dev/null || (echo "✗ g++ not found" && exit 1)
	@echo "✓ g++ found"
	@test -d $(LIBJUICE_DIR) || (echo "✗ libjuice directory not found at $(LIBJUICE_DIR)" && exit 1)
	@echo "✓ libjuice directory found"
	@test -f $(LIBJUICE_LIB) || (echo "✗ libjuice library not found at $(LIBJUICE_LIB)" && exit 1)
	@echo "✓ libjuice library found"
	@test -x $(CC_ARM) && echo "✓ ARM toolchain gcc found: $(CC_ARM)" \
	    || echo "⚠ ARM toolchain gcc not found at $(CC_ARM) (provider-arm will fail)"
	@echo "✓ All dependencies satisfied"

# Install binaries to /usr/local/bin (native only)
install: all
	@echo "Installing binaries to /usr/local/bin..."
	sudo cp $(PROVIDER_BIN) /usr/local/bin/provider
	sudo cp $(CONSUMER_BIN) /usr/local/bin/consumer_api
	sudo cp $(SIGNALING_SERVER_BIN) /usr/local/bin/signaling_server
	sudo chmod +x /usr/local/bin/provider
	sudo chmod +x /usr/local/bin/consumer_api
	sudo chmod +x /usr/local/bin/signaling_server
	@echo "✓ Installation complete"
	@echo ""
	@echo "You can now run:"
	@echo "  provider <signaling_server_ip> [config_file]"
	@echo "  consumer_api <config_file>"
	@echo "  signaling_server <port>"

# Uninstall binaries
uninstall:
	@echo "Uninstalling binaries..."
	sudo rm -f /usr/local/bin/provider
	sudo rm -f /usr/local/bin/consumer_api
	sudo rm -f /usr/local/bin/signaling_server
	@echo "✓ Uninstall complete"

# AddressSanitizer debug build — catches heap-use-after-free, buffer overflows,
# double-free.  Run the resulting binaries under normal test traffic to detect
# memory bugs that would be silent in a release build.
debug-asan: $(PROVIDER_SRC)
	@mkdir -p $(BIN_DIR)
	@echo "Building provider with AddressSanitizer..."
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wall -Wextra -I$(LIBJUICE_INCLUDE) \
	      $(PROVIDER_SRC) -o $(BIN_DIR)/provider_asan \
	      $(LIBJUICE_LIB) -lpthread -lm
	@echo "Building consumer with AddressSanitizer..."
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wall -Wextra -I$(LIBJUICE_INCLUDE) \
	      $(CONSUMER_SRC) -o $(BIN_DIR)/consumer_asan \
	      $(LIBJUICE_LIB) -lpthread -lm
	@echo "✓ ASAN builds: $(BIN_DIR)/provider_asan  $(BIN_DIR)/consumer_asan"
	@echo "  Run with:  ASAN_OPTIONS=detect_leaks=1 ./binaries/provider_asan provider.conf"

# ThreadSanitizer debug build — catches data races and lock-order inversions.
debug-tsan: $(PROVIDER_SRC)
	@mkdir -p $(BIN_DIR)
	@echo "Building provider with ThreadSanitizer..."
	$(CC) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
	      -Wall -Wextra -I$(LIBJUICE_INCLUDE) \
	      $(PROVIDER_SRC) -o $(BIN_DIR)/provider_tsan \
	      $(LIBJUICE_LIB) -lpthread -lm
	@echo "Building consumer with ThreadSanitizer..."
	$(CC) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
	      -Wall -Wextra -I$(LIBJUICE_INCLUDE) \
	      $(CONSUMER_SRC) -o $(BIN_DIR)/consumer_tsan \
	      $(LIBJUICE_LIB) -lpthread -lm
	@echo "✓ TSAN builds: $(BIN_DIR)/provider_tsan  $(BIN_DIR)/consumer_tsan"

# Help
help:
	@echo "P2P libjuice Makefile"
	@echo ""
	@echo "Available targets:"
	@echo "  make                 - Build everything: libjuice (x86+ARM) + SRT (x86+ARM) + signaling (dynamic+static)"
	@echo "  make all             - Same as 'make'"
	@echo ""
	@echo "  ── libjuice path (legacy) ──"
	@echo "  make provider        - libjuice provider (x86_64, standard)"
	@echo "  make provider-small  - size-optimized libjuice provider"
	@echo "  make provider-arm    - libjuice ARM provider (Augentix) — auto-builds libjuice.a"
	@echo "  make libjuice-arm    - cross-compile libjuice.a for ARM only"
	@echo "  make consumer        - libjuice consumer_api"
	@echo ""
	@echo "  ── signaling server ──"
	@echo "  make signaling        - signaling_server (dynamic, local dev)"
	@echo "  make signaling-static - signaling_server.static (the cloud-deployable one)"
	@echo ""
	@echo "  ── SRT P2P pipeline (current production path) ──"
	@echo "  make provider-srt        - x86_64 provider_srt (local dev/test)"
	@echo "  make consumer-srt-p2p    - x86_64 consumer_srt_p2p (dynamic + static)"
	@echo "  make provider-srt-arm    - ARM provider_srt (the actual camera binary)"
	@echo ""
	@echo "  ── debug / utility ──"
	@echo "  make strip        - Build and strip debug symbols (native)"
	@echo "  make debug-asan   - Build provider+consumer with AddressSanitizer"
	@echo "  make debug-tsan   - Build provider+consumer with ThreadSanitizer"
	@echo "  make clean        - Remove all output binaries"
	@echo "  make clean-arm    - Also remove cross-compiled libjuice ARM build dir"
	@echo "  make clean-srt       - Remove SRT shim (forces refresh from libsrt headers)"
	@echo "  make clean-libsrt-x64 - Drop build-x86_64-linux so libsrt matches host libc"
	@echo "  make libsrt-x64      - Reconfigure + build libsrt.a for native x86_64 (fixes __isoc23_* link)"
	@echo "  make check-deps   - Check if dependencies are installed"
	@echo "  make install      - Install native binaries to /usr/local/bin"
	@echo "  make uninstall    - Remove binaries from /usr/local/bin"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "ARM toolchain (override with TOOLCHAIN_DIR / TOOLCHAIN_PREFIX):"
	@echo "  TOOLCHAIN_DIR    = $(TOOLCHAIN_DIR)"
	@echo "  TOOLCHAIN_PREFIX = $(TOOLCHAIN_PREFIX)"
	@echo ""
	@echo "Examples:"
	@echo "  make                    # Build everything (native + ARM)"
	@echo "  make clean && make      # Full clean rebuild"
	@echo "  make provider-arm       # Only ARM provider"
	@echo "  make install            # Install native binaries system-wide"
	@echo ""
	@echo "Note: consumer_api replaces older consumer variants"
