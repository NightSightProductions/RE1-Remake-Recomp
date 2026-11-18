#!/bin/bash
# Development loop: retranslate (unchanged files are kept), rebuild, run. Extra args go to the game binary.
set -e
cd "$(dirname "$0")/.."
[[ "${SKIP_GEN:-}" ]] || .venv/bin/python framework/win32/tools/recomp_w32.py orig.exe gen | grep -v "files$"
make -s -j"$(sysctl -n hw.ncpu)" boot 2>&1 | grep -E "error" | head -20 || true
# d3d9 backend: DXVK on the KosmicKrisp from `make kosmickrisp` (build/kosmickrisp), found by the runtime
RT_VERBOSE=${RT_VERBOSE-1} timeout "${TIMEOUT:-60}" ./build/re1 --image "$PWD/orig.exe" "$@" 2>&1 | grep -v "^\[k32\] CreateThread" | head -${LINES_MAX:-60}
