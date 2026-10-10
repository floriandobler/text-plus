#!/usr/bin/env bash
# Baut KonturText.bundle als Universal Binary (Apple Silicon + Intel).
#
#   ./scripts/build_mac.sh            # nur bauen
#   ./scripts/build_mac.sh --install  # bauen und nach "Extra Effects" kopieren
#
# Voraussetzungen: Xcode Command Line Tools (xcode-select --install) und CMake >= 3.20
# (brew install cmake). Die Abhängigkeiten (FFGL SDK, FreeType, HarfBuzz, msdfgen)
# lädt CMake beim ersten Lauf per git herunter.
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v cmake >/dev/null 2>&1; then
	echo "CMake fehlt. Installieren mit: brew install cmake" >&2
	exit 1
fi
if ! xcode-select -p >/dev/null 2>&1; then
	echo "Xcode Command Line Tools fehlen. Installieren mit: xcode-select --install" >&2
	exit 1
fi

BUILD_DIR="build-mac"
cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build "$BUILD_DIR" --config Release --parallel

BUNDLE="$BUILD_DIR/KonturText.bundle"
if [[ ! -d "$BUNDLE" ]]; then
	echo "Build fertig, aber $BUNDLE wurde nicht gefunden." >&2
	exit 1
fi

# Ad-hoc-Signatur: reicht für selbst gebaute Plugins auf dem eigenen Rechner.
codesign --force --sign - "$BUNDLE"
echo
lipo -info "$BUNDLE/Contents/MacOS/KonturText"

if [[ "${1:-}" == "--install" ]]; then
	DEST="$HOME/Documents/Resolume Arena/Extra Effects"
	mkdir -p "$DEST"
	rm -rf "$DEST/KonturText.bundle"
	cp -R "$BUNDLE" "$DEST/"
	echo "Installiert nach: $DEST"
	echo "Resolume neu starten. Das Plugin heißt \"Kontur Text\" und liegt unter Sources."
else
	echo "Fertig: $BUNDLE"
	echo "Installieren: ./scripts/build_mac.sh --install"
fi
