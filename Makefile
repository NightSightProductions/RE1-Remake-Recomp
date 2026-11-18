# Resident Evil HD Remaster (2015, Steam app 304240) on Recomp-Framework (framework/, a git submodule):
# static recompilation of the 32-bit Windows bhd.exe to native arm64 macOS, rendering natively on Metal.
#   make unwrap      remove SteamStub from the user's bhd.exe -> orig.exe (build time only, never distributed)
#   make gen         translate orig.exe -> gen/*.c
#   make -j8 boot    build build/re1
#   make app         build/Resident Evil HD Remaster.app (double-click to play)
FW        = framework
PYTHON    = .venv/bin/python
BUILD     = build
GAME_DIR ?= $(HOME)/Library/Application Support/Steam/steamapps/common/Resident Evil Biohazard HD REMASTER
GAME_EXE  = orig.exe
APP_NAME  = re1
GAME_SRC  = src/game.c $(wildcard src/render/*.c)
NATIVE    = native.txt
PATCHES   = patches.txt
# make app -> "build/Resident Evil HD Remaster.app"
APP_FILE     = Resident Evil HD Remaster
BUNDLE_ID    = com.nightsightproductions.re1-recomp
APP_CATEGORY = public.app-category.action-games
# the exe's icon is 16x16: use the cover Steam keeps for the game, from the user's own library (build time only)
STEAM_COVER  = $(HOME)/Library/Application Support/Steam/appcache/librarycache/304240/library_600x900.jpg
APP_ICON     = $(if $(wildcard $(subst $(space),\ ,$(STEAM_COVER))),$(STEAM_COVER))
space := $(subst ,, )

# SHA-256 of the supported Steam release (wrapped as shipped, and after unwrapping)
SHA_WRAPPED = 39b840663e4b1a6835c60cbd176e4ee6c21b9197fc2c2dd733f2a7c36a0b429a
SHA_UNWRAPPED = fc098aeb3dc6892daa128ae5608db640e18eb4f0a0620b16867351ef2b17bbf0

# the game renders natively on Metal (src/render): no D3D9 backend in the build or the app
D3D9_BACKEND = none
# the shaders as one precompiled metallib when Apple's Metal Toolchain is installed (it comes with Xcode), else as
# Metal source the app compiles at first launch (the installer does it: re1 --compile-shaders). SHADERS=metallib|msl
SHADERS ?= $(shell xcrun -sdk macosx -f metal >/dev/null 2>&1 && echo metallib || echo msl)
ifeq ($(SHADERS),metallib)
APP_RESOURCES = $(BUILD)/shaders/shaders.metallib
else
APP_RESOURCES = $(BUILD)/shaders/msl=shaders_msl
endif
include $(FW)/win32/win32.mk

# the native Metal renderer's shaders: every shader the game has (its SM3 bytecode is in nativePC/sa/DX9/*.arc),
# extracted from the user's own copy and converted at build time (tools/arc_shaders.py, framework sm3msl.mk).
SHADER_SRC   = $(BUILD)/shader_src
SHADER_ARCS  = $(wildcard $(subst $(space),\ ,$(GAME_DIR))/nativePC/sa/DX9/*.arc)
$(SHADER_SRC)/.extracted: tools/arc_shaders.py
	@test -d "$(GAME_DIR)/nativePC/sa" || { echo "error: no nativePC/sa in GAME_DIR=$(GAME_DIR)" >&2; exit 1; }
	$(PYTHON) -I tools/arc_shaders.py "$(GAME_DIR)/nativePC/sa" $(SHADER_SRC)
	@touch $@
$(BUILD)/shaders/sm3_shaders.inc: $(SHADER_SRC)/.extracted $(SM3MSL)
	$(MAKE) $(if $(filter metallib,$(SHADERS)),shaders,shaders-msl) SHADER_SRC=$(SHADER_SRC)
$(BUILD)/shaders/msl: $(BUILD)/shaders/sm3_shaders.inc
	@test -d $@ || python3 $(FW)/win32/tools/msl_batches.py $(BUILD)/shaders $@
CFLAGS += -I$(BUILD)/shaders -Isrc/render

unwrap:
	@sha=$$(shasum -a 256 "$(GAME_DIR)/bhd.exe" | cut -d' ' -f1); \
	  [ "$$sha" = $(SHA_WRAPPED) ] || { echo "error: bhd.exe is not the supported Steam release (SHA-256 $$sha)" >&2; exit 1; }
	$(PYTHON) tools/unwrap_steamstub32.py "$(GAME_DIR)/bhd.exe" $(GAME_EXE)
	@sha=$$(shasum -a 256 $(GAME_EXE) | cut -d' ' -f1); [ "$$sha" = $(SHA_UNWRAPPED) ] || { echo "error: unexpected unwrapped image $$sha" >&2; exit 1; }

gen: $(GAME_EXE)
$(GAME_EXE):
	$(MAKE) unwrap

.PHONY: unwrap

# The engine synchronizes through integer locks and flags (CRITICAL_SECTIONs, interlocked ops), so float and vector
# accesses need no x86 ordering (framework README, "Memory ordering"); DOOM-Recomp makes the same choice
GEN_CFLAGS += -DRT_RELAX_FP

# the shader table is compiled into the renderer
$(BUILD)/game/src/render/rn_frame.o: $(BUILD)/shaders/sm3_shaders.inc

# the app needs the converted shaders
app: $(BUILD)/shaders/sm3_shaders.inc
