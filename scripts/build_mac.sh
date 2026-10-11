#!/usr/bin/env bash
# Builds TextPlus.bundle as a universal binary (Apple Silicon + Intel).
#
#   ./scripts/build_mac.sh            # build only
#   ./scripts/build_mac.sh --install  # build and copy to Resolume's "Extra Effects" folder
#
# Requirements: Xcode Command Line Tools (xcode-select --install) and CMake >= 3.20
# (brew install cmake). CMake fetches the dependencies (FFGL SDK, FreeType,
# HarfBuzz, msdfgen) with git on the first run.
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v cmake >/dev/null 2>&1; then
	echo "CMake is missing. Install it with: brew install cmake" >&2
	exit 1
fi
if ! xcode-select -p >/dev/null 2>&1; then
	echo "Xcode Command Line Tools are missing. Install them with: xcode-select --install" >&2
	exit 1
fi

BUILD_DIR="build-mac"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build "$BUILD_DIR" --config Release --parallel

BUNDLE="$BUILD_DIR/TextPlus.bundle"
if [[ ! -d "$BUNDLE" ]]; then
	echo "Build finished, but $BUNDLE was not found." >&2
	exit 1
fi

# Ad-hoc signature: enough for a plugin built on this Mac.
codesign --force --sign - "$BUNDLE"
echo
lipo -info "$BUNDLE/Contents/MacOS/TextPlus"

if [[ "${1:-}" == "--install" ]]; then
	for APP in "Resolume Arena" "Resolume Avenue"; do
		DEST="$HOME/Documents/$APP/Extra Effects"
		[[ -d "$HOME/Documents/$APP" ]] || continue
		mkdir -p "$DEST"
		rm -rf "$DEST/TextPlus.bundle" "$DEST/KonturText.bundle"
		cp -R "$BUNDLE" "$DEST/"
		echo "Installed to: $DEST"
	done
	echo "Restart Resolume. The plugin is called \"Text+\" and is listed under Sources."
else
	echo "Done: $BUNDLE"
	echo "Install: ./scripts/build_mac.sh --install"
fi
