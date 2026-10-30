#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Helpie Audio Bridge Build ==="
echo "Node: $(node --version), npm: $(npm --version), arch: $(uname -m)"

if ! command -v xcodebuild >/dev/null; then
  echo "Xcode Command Line Tools are required."
  exit 1
fi

if [ ! -d "node_modules/node-addon-api" ]; then
  echo "Installing dev dependencies..."
  npm install
fi

build_one() {
  local arch="$1"
  local out="$2"
  local expected="$3"

  echo ""
  echo "--- Building $arch -> $out ---"
  rm -rf build/
  npx node-gyp rebuild --arch="$arch" --release
  test -f build/Release/audio_capture.node
  mkdir -p "$(dirname "$out")"
  cp build/Release/audio_capture.node "$out"
  codesign --force --sign - "$out"
  file "$out"
  if ! file "$out" | grep -q "$expected"; then
    echo "Built binary has the wrong architecture."
    exit 1
  fi
  if otool -L "$out" | grep -E 'libswift|libAudioCapture|@rpath'; then
    echo "Unexpected dependency found."
    exit 1
  fi
}

build_one "arm64" "bin/arm64/audio_capture_arm64.node" "arm64"

echo ""
if arch -x86_64 node -p "process.arch" 2>/dev/null | grep -q x64; then
  echo "--- Building x86_64 via Rosetta ---"
  rm -rf build/
  arch -x86_64 /bin/zsh -c "cd '$SCRIPT_DIR' && npx node-gyp rebuild --arch=x64 --release"
  test -f build/Release/audio_capture.node
  mkdir -p bin/x86_64
  cp build/Release/audio_capture.node bin/x86_64/audio_capture_x86_64.node
  codesign --force --sign - bin/x86_64/audio_capture_x86_64.node
  file bin/x86_64/audio_capture_x86_64.node
  if ! file bin/x86_64/audio_capture_x86_64.node | grep -q "x86_64"; then
    echo "Built x64 binary has the wrong architecture."
    exit 1
  fi
  if otool -L bin/x86_64/audio_capture_x86_64.node | grep -E 'libswift|libAudioCapture|@rpath'; then
    echo "Unexpected x64 dependency found."
    exit 1
  fi
else
  echo "Skipping x86_64 build: this Node binary cannot run as x64."
  echo "Install a universal or x64 Node, then run ./build.sh again."
  rm -f bin/x86_64/audio_capture_x86_64.node
fi

rm -rf build/
echo ""
echo "Build complete."

