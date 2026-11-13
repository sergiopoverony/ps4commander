# Сборка PKG-приложения PS4 Commander (экран + геймпад, оболочка для службы ps4commander).
# Нужен OpenOrbis PS4 Toolchain и переменная OO_PS4_TOOLCHAIN. Сборка здесь: make.
#
# Частая ошибка - после правки версии/кода службы собрать только тут (make) и получить PKG
# со старым (не пересобранным) payload внутри, хотя сам интерфейс приложения уже новый.
# Чтобы собрать и службу, и приложение одной командой и гарантированно свежими:
#   make full
# (нужен и PS4_PAYLOAD_SDK для службы в корне проекта, и OO_PS4_TOOLCHAIN для этой сборки -
# оба должны быть экспортированы заранее).

export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT := 1
export DOTNET_ROLL_FORWARD := LatestMajor

TITLE       := PS4 Commander
TITLE_ID    := BREW00090
CONTENT_ID  := IV0000-BREW00090_00-PS4COMMANDER0000
LIBS        := -lc -lkernel -lc++ -lSceNet -lSceVideoOut -lSceSysmodule -lSceFreeType -lScePad -lSceUserService -lSceSystemService -lSceImeDialog -lSceCommonDialog
EXTRAFLAGS  := -O2

# Версия приложения = версия службы (из ../src/version.h): приложение сравнивает их при запуске
# (см. /api/roots.version) и, если они разные, переотправляет ELF через BinLoader.
APPVER      := $(shell sed -n 's/.*APP_VERSION "\(.*\)".*/\1/p' ../src/version.h)

ASSETS      := assets/fonts/DejaVuSansMono.ttf assets/fonts/LICENSE-DejaVu.txt \
               assets/images/qr_sbp.png assets/images/qr_paypal.png \
               assets/misc/lang_en.json assets/misc/lang_ru.json assets/misc/ps4commander.elf

# Системные модули для приложения (без libc.prx приложение сразу вылетает). Берутся из набора OpenOrbis.
LIBMODULES  := sce_module/libc.prx sce_module/libSceFios2.prx

TOOLCHAIN   := $(OO_PS4_TOOLCHAIN)
PROJDIR     := src
INTDIR      := build/obj

CPPFILES    := $(wildcard $(PROJDIR)/*.cpp)
OBJS        := $(patsubst $(PROJDIR)/%.cpp, $(INTDIR)/%.o, $(CPPFILES))

CXXFLAGS    := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c $(EXTRAFLAGS) -DAPP_VERSION=\"$(APPVER)\" \
               -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -isystem $(TOOLCHAIN)/include/c++/v1
LDFLAGS     := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib $(LIBS) $(TOOLCHAIN)/lib/crt1.o

_unused     := $(shell mkdir -p $(INTDIR) sce_sys/about sce_module assets/misc)

UNAME_S     := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
		CCX     := clang++
		LD      := ld.lld
		CDIR    := linux
endif
ifeq ($(UNAME_S),Darwin)
		CCX     := /usr/local/opt/llvm/bin/clang++
		LD      := /usr/local/opt/llvm/bin/ld.lld
		CDIR    := macos
endif
ifneq (,$(findstring MSYS_NT,$(UNAME_S)))
		CCX     := clang++
		LD      := ld.lld
		CDIR    := windows
endif

all: $(CONTENT_ID).pkg

$(CONTENT_ID).pkg: pkg.gp4
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core pkg_build $< .

pkg.gp4: eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_sys/pic1.png $(LIBMODULES) $(ASSETS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-gp4 -out $@ --content-id=$(CONTENT_ID) --files "$^"

sce_module/%.prx:
	@src="$$(find '$(TOOLCHAIN)' -name '$*.prx' 2>/dev/null | head -n 1)"; \
	if [ -z "$$src" ]; then echo "Не найден $*.prx в $(TOOLCHAIN). Скопируйте его вручную в pkg/sce_module/"; exit 1; fi; \
	echo "cp $$src $@"; cp "$$src" $@

# Служба берётся из корня проекта.
assets/misc/ps4commander.elf: ../release/ps4commander.elf
	cp $< $@

# Переводы - те же файлы, что и у веб-страницы (lang/en.json, lang/ru.json из корня проекта).
assets/misc/lang_en.json: ../lang/en.json
	cp $< $@
assets/misc/lang_ru.json: ../lang/ru.json
	cp $< $@

# Иконка и фон запуска - из корневого sce_sys (имя Icon0.png там с заглавной буквы, PS4 ждёт icon0.png).
sce_sys/icon0.png: ../sce_sys/Icon0.png
	cp $< $@

sce_sys/pic1.png: ../sce_sys/pic1.png
	cp $< $@

sce_sys/about/right.sprx:
	cp $(TOOLCHAIN)/samples/piglet/sce_sys/about/right.sprx $@

sce_sys/param.sfo: Makefile
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_new $@
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 1
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '01.00'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(TOOLCHAIN)/bin/$(CDIR)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '01.00'

eboot.bin: $(OBJS)
	$(LD) $(INTDIR)/*.o -o $(INTDIR)/ps4commander_app.elf $(LDFLAGS)
	$(TOOLCHAIN)/bin/$(CDIR)/create-fself -in=$(INTDIR)/ps4commander_app.elf -out=$(INTDIR)/ps4commander_app.oelf --eboot "eboot.bin" --paid 0x3800000000000010

$(INTDIR)/%.o: $(PROJDIR)/%.cpp
	$(CCX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(CONTENT_ID).pkg pkg.gp4 eboot.bin build sce_sys sce_module assets/misc/ps4commander.elf assets/misc/lang_en.json assets/misc/lang_ru.json

# Пересобирает службу в корне проекта (нужен PS4_PAYLOAD_SDK), обновляет её копию в
# ../release/ps4commander.elf, затем чисто пересобирает сам PKG. Используйте вместо простого
# "make", когда меняли код или версию - иначе легко получить PKG со старым payload внутри.
.PHONY: full
full:
	$(MAKE) -C ..
	mkdir -p ../release
	cp ../build/ps4commander.elf ../release/ps4commander.elf
	$(MAKE) clean
	$(MAKE) all

.PHONY: all clean
