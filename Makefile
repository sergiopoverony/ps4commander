# Сборка: export PS4_PAYLOAD_SDK=/путь/к/sdk; make
ifdef PS4_PAYLOAD_SDK
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else ifneq ($(wildcard /opt/ps4-payload-sdk/toolchain/orbis.mk),)
    PS4_PAYLOAD_SDK := /opt/ps4-payload-sdk
    include $(PS4_PAYLOAD_SDK)/toolchain/orbis.mk
else
    $(error PS4_PAYLOAD_SDK is undefined)
endif

# orbis-ld берёт путь к линкер-скрипту из переменной окружения, поэтому её нужно экспортировать.
export PS4_PAYLOAD_SDK

PS4_HOST ?= 192.168.0.24
PS4_PORT ?= 9090
TARGET   := build/ps4commander.elf
SOURCES  := $(wildcard src/*.cpp)
HEADERS  := $(wildcard src/*.h)
ASSETS   := build/assets_gen.h
# Библиотеки держим минимальными: libSceNetCtl подключать нельзя (на части консолей payload не запускается).
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread -lSceSystemService

# SMB-клиент libsmb2 (LGPL-2.1), исходники лежат в third_party/libsmb2
SMB_DIR  := third_party/libsmb2
SMB_OBJ  := $(addprefix build/smb2/,$(patsubst %.c,%.o,$(shell cat $(SMB_DIR)/SOURCES.txt)))
SMB_CFLAGS := -O2 -w -include stddef.h "-D_U_=__attribute__((unused))" -DHAVE_CONFIG_H \
              -I$(SMB_DIR) -I$(SMB_DIR)/include -I$(SMB_DIR)/include/smb2 -I$(SMB_DIR)/lib

# libfuse 2.9 (LGPL-2.1) с портом под PS4 из Itemzflow (GPL-3.0): third_party/libfuse
FUSE_DIR := third_party/libfuse
FUSE_OBJ := $(patsubst $(FUSE_DIR)/source/%.c,build/fuse/%.o,$(wildcard $(FUSE_DIR)/source/*.c))
FUSE_CFLAGS := -O2 -w -DFUSE_USE_VERSION=26 -D_FILE_OFFSET_BITS=64 -I$(FUSE_DIR)/include

all: $(TARGET)

build/smb2/%.o: $(SMB_DIR)/lib/%.c
	mkdir -p build/smb2
	$(CC) $(SMB_CFLAGS) -c -o $@ $<

build/fuse/%.o: $(FUSE_DIR)/source/%.c
	mkdir -p build/fuse
	$(CC) $(FUSE_CFLAGS) -c -o $@ $<

$(ASSETS): web/index.html src/version.h tools/embed.py
	mkdir -p build
	python3 tools/embed.py $(ASSETS)

$(TARGET): $(SOURCES) $(HEADERS) $(ASSETS) $(SMB_OBJ) $(FUSE_OBJ)
	$(CXX) $(CXXFLAGS) -D_FILE_OFFSET_BITS=64 -Isrc -Ibuild -I$(SMB_DIR)/include -I$(FUSE_DIR)/include -o $@ $(SOURCES) $(SMB_OBJ) $(FUSE_OBJ)

send: $(TARGET)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $(TARGET)

clean:
	rm -rf build

.PHONY: all clean send
