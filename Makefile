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

all: $(TARGET)

$(ASSETS): web/index.html src/version.h tools/embed.py
	mkdir -p build
	python3 tools/embed.py $(ASSETS)

$(TARGET): $(SOURCES) $(HEADERS) $(ASSETS)
	$(CXX) $(CXXFLAGS) -Isrc -Ibuild -o $@ $(SOURCES)

send: $(TARGET)
	$(PS4_DEPLOY) -h $(PS4_HOST) -p $(PS4_PORT) $(TARGET)

clean:
	rm -rf build

.PHONY: all clean send
