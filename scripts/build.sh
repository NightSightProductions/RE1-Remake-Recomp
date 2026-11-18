#!/bin/bash
#
# Builds "Resident Evil HD Remaster.app" from YOUR copy of Resident Evil HD Remaster (the Windows release on Steam).
# Nothing from the game is stored in this repository; everything is generated locally from your install.
#
# Usage: scripts/build.sh [path/to/Resident Evil Biohazard HD REMASTER]   (the folder with bhd.exe and nativePC)
#
# Needs Apple's Command Line Tools. Python packages (the translator's capstone, meson, ninja and cmake for the shader
# converter) go into .venv. The shaders become one precompiled metallib when Apple's Metal Toolchain is installed
# (it comes with Xcode), else Metal source the app compiles once at first launch (the installer does it).

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "error: $*" >&2; exit 1; }
step() { echo "==> [$1/6] $2"; }

find_game() {
    local c
    for c in \
        "$HOME/Library/Application Support/Steam/steamapps/common/Resident Evil Biohazard HD REMASTER" \
        "$HOME/Library/Application Support/CrossOver/Bottles/"*"/drive_c/Program Files (x86)/Steam/steamapps/common/Resident Evil Biohazard HD REMASTER"; do
        [[ -f "$c/bhd.exe" && -d "$c/nativePC" ]] && { echo "$c"; return; }
    done
}
GAME="${1:-$(find_game)}"
[[ -n "$GAME" ]] || die "the game was not found. Pass the folder that contains bhd.exe and nativePC"

# The Steam release this project is written against (Makefile SHA_WRAPPED)
SHA_EXE="39b840663e4b1a6835c60cbd176e4ee6c21b9197fc2c2dd733f2a7c36a0b429a"

step 1 "Checking requirements"
for t in clang make python3 codesign ar git; do
    command -v "$t" >/dev/null 2>&1 || die "'$t' was not found. Install Apple's Command Line Tools: xcode-select --install"
done
[[ "$(uname -m)" == "arm64" ]] || die "this build targets Apple Silicon Macs"

step 2 "Verifying your copy of the game"
[[ -f "$GAME/bhd.exe" ]] || die "$GAME/bhd.exe was not found"
sha="$(shasum -a 256 "$GAME/bhd.exe" | awk '{print $1}')"
[[ "$sha" == "$SHA_EXE" ]] || die "bhd.exe is not from the supported Steam release (SHA-256 $sha)"
[[ -d "$GAME/nativePC/sa/DX9" ]] || die "the game's shaders (nativePC/sa/DX9) were not found"

step 3 "Preparing the tools"
[[ -x "$ROOT/.venv/bin/python" ]] || python3 -m venv "$ROOT/.venv"
"$ROOT/.venv/bin/python" -m pip install --quiet --disable-pip-version-check "capstone==5.0.6" "lief==0.16.6" meson ninja cmake
export PATH="$ROOT/.venv/bin:$PATH"   # meson, ninja and cmake for the shader converter's libraries

cd "$ROOT"
mkdir -p build
step 4 "Translating the game's code to C"
make -s unwrap GAME_DIR="$GAME"
make -s gen GAME_DIR="$GAME"

step 5 "Compiling"
make -s -j"$(sysctl -n hw.ncpu)" boot GAME_DIR="$GAME"

step 6 "Packaging the app"
make -s app GAME_DIR="$GAME"
echo "Done: build/Resident Evil HD Remaster.app"
