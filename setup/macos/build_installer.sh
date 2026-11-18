#!/bin/bash
#
# Builds the macOS distribution of Resident Evil HD Remaster Recomp:
#
#   RE1-Recomp-macOS.dmg
#     Install RE1 Recomp.app   installer wizard (Installer.swift) carrying
#       source/                        this project's source, including the framework submodule
#                                      (git ls-files --recurse-submodules) - no game files
#     License.txt
#
# The installer builds the game on the user's Mac from their own copy of Resident Evil HD Remaster (the Windows
# release from Steam), by running scripts/build.sh. It renders natively on Metal, so no graphics driver is bundled.
#
# Requirements: Xcode Command Line Tools (swiftc, git). Optional: docs/logo.png (256 x 256) for the installer's icon.
# Usage: setup/macos/build_installer.sh [output dir]

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SETUP="${ROOT}/setup/macos"
OUT="${1:-${ROOT}/build-macos-dist}"
STAGE="${OUT}/stage"
MIN_MACOS="14.0"   # the runtime's minimum (framework win32.mk MACOS_MIN)

APP_NAME="RE1 Recomp"
INSTALLER="${STAGE}/dmg/Install ${APP_NAME}.app"
DMG="${OUT}/RE1-Recomp-macOS.dmg"

die() { echo "error: $*" >&2; exit 1; }

git -C "${ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "run this from a git checkout"
VERSION="$(git -C "${ROOT}" describe --tags --always 2>/dev/null || echo 1.0)"
VERSION="${VERSION#v}"

rm -rf "${STAGE}"
mkdir -p "${INSTALLER}/Contents/MacOS" "${INSTALLER}/Contents/Resources/source"

# Only tracked files: the repository never contains game code or data.
# (DXVK isn't used: the game renders on Metal, D3D9_BACKEND=none)
(cd "${ROOT}" && git ls-files --recurse-submodules | grep -v -e "^docs/" -e "^framework/third_party/dxvk/" | while read -r f; do
    mkdir -p "${INSTALLER}/Contents/Resources/source/$(dirname "$f")"
    cp -p "$f" "${INSTALLER}/Contents/Resources/source/$f"
done)

sed -e "s/@VERSION@/${VERSION}/g" -e "s/@MIN_MACOS@/${MIN_MACOS}/g" "${SETUP}/InstallerInfo.plist" > "${INSTALLER}/Contents/Info.plist"
xcrun swiftc -O -swift-version 5 -target "arm64-apple-macos${MIN_MACOS}" "${SETUP}/Installer.swift" -o "${INSTALLER}/Contents/MacOS/installer"
[[ -f "${SETUP}/WelcomeImage.png" ]] && cp "${SETUP}/WelcomeImage.png" "${INSTALLER}/Contents/Resources/"
# The installer's icon, from docs/logo.png (256 x 256) when the project has one; else the default app icon
if [[ -f "${ROOT}/docs/logo.png" ]]; then
    ICONSET="${STAGE}/AppIcon.iconset"
    rm -rf "${ICONSET}" && mkdir -p "${ICONSET}"
    for size in 16 32 128 256; do
        sips -z "${size}" "${size}" "${ROOT}/docs/logo.png" --out "${ICONSET}/icon_${size}x${size}.png" >/dev/null
        if [[ ${size} -le 128 ]]; then
            sips -z $((size * 2)) $((size * 2)) "${ROOT}/docs/logo.png" --out "${ICONSET}/icon_${size}x${size}@2x.png" >/dev/null
        fi
    done
    iconutil -c icns "${ICONSET}" -o "${INSTALLER}/Contents/Resources/AppIcon.icns"
fi

# Ad hoc signature: Apple Silicon only runs signed code. Distribution outside this Mac
# needs a Developer ID signature and notarization; set SIGN_ID to use one.
SIGN_ID="${SIGN_ID:--}"
SIGN_OPTS=()
[[ "${SIGN_ID}" != "-" ]] && SIGN_OPTS=(--options runtime --timestamp)
codesign --force --sign "${SIGN_ID}" "${SIGN_OPTS[@]+"${SIGN_OPTS[@]}"}" "${INSTALLER}"

[[ -f "${ROOT}/LICENSE" ]] && cp "${ROOT}/LICENSE" "${STAGE}/dmg/License.txt"   # (RE1 Recomp has no license yet)
rm -f "${DMG}"
hdiutil create -volname "${APP_NAME}" -srcfolder "${STAGE}/dmg" -ov -format UDZO "${DMG}" >/dev/null
echo "Created ${DMG}"
