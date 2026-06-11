# Cross-compilation Makefile for MQTT Camera Client.
# Toolchain: arm-augentix-linux-uclibcgnueabihf (GCC 10.3.0)
#
# Layout (this Makefile lives at src/mqtt-vcamclient/Makefile):
#   <repo>/
#     src/mqtt-vcamclient/   ← $(CURDIR)
#     toolchain/             ← default $(TOOLCHAIN_DIR) location
#     bin/                   ← cross-compiled output
#
# Override TOOLCHAIN_DIR for CI (toolchain lives outside the worktree):
#   make TOOLCHAIN_DIR=/opt/augentix-tc/arm-augentix-linux-uclibcgnueabihf

# Repo root (two levels up from src/mqtt-vcamclient/).
REPO_ROOT ?= $(CURDIR)/../..

# Toolchain dir — override-friendly.
TOOLCHAIN_DIR ?= $(REPO_ROOT)/toolchain/arm-augentix-linux-uclibcgnueabihf
TOOLCHAIN_PREFIX ?= arm-augentix-linux-uclibcgnueabihf
SYSROOT = $(TOOLCHAIN_DIR)/$(TOOLCHAIN_PREFIX)/sysroot
CC = $(TOOLCHAIN_DIR)/bin/$(TOOLCHAIN_PREFIX)-gcc --sysroot=$(SYSROOT)

# Binary output goes to <repo>/bin/.
BIN_DIR ?= $(REPO_ROOT)/bin
TARGET = $(BIN_DIR)/MQTT_vcamclient_Augentix

# Source files
SRC = HTTP.c \
      N_HTTPS.c \
      deps/include/cJSON.c

# Include directories
INCLUDES = -I$(CURDIR)/deps/include

# Library directories
LIB_PATH = -L$(CURDIR)/deps/lib

# Libraries
LIBS = -lpaho-mqtt3cs -lcurl -lssl -lcrypto -lpthread -ldl

# rpath (where shared libs live on the camera)
# --unresolved-symbols: shared lib deps (OCSP, ENGINE) resolve at runtime on camera
RPATH = -Wl,-rpath,/mny/mtd/ipc/ambicam -Wl,--unresolved-symbols=ignore-in-shared-libs

# Build rule
all: $(BIN_DIR)
	$(CC) $(RPATH) $(INCLUDES) $(LIB_PATH) $(SRC) -o $(TARGET) $(LIBS)
	@echo "Build successful: $(TARGET)"

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Clean rule
clean:
	rm -f $(TARGET)
