# Helpie Audio Bridge

Native macOS audio capture bridge for Electron/Node, built with Objective-C++, N-API, and Apple ScreenCaptureKit. No Swift runtime, single binary per arch.

## What It Contains

- `audio_capture.mm` - ScreenCaptureKit audio capture implementation (2×2 video, audio-only)
- `wrapper.cc` - N-API bridge with lock-free ring buffer and batched callbacks
- `ring_buffer.h` - SPSC lock-free ring buffer for float audio
- `audio_capture.h` - C ABI interface
- `index.js` - arch-aware loader (arm64 vs x86_64)
- `binding.gyp` - node-gyp build config (macOS 13+, ARC)
- `build.sh` - reproducible arm64 and x86_64 build script
- `bin/arm64/audio_capture_arm64.node` - Apple Silicon binary
- `bin/x86_64/audio_capture_x86_64.node` - Intel Mac binary

## Requirements

- **macOS 13.0+** — ScreenCaptureKit `SCStreamOutputTypeAudio` requires 13+
- **Xcode Command Line Tools** — `xcode-select --install`
- **Node 18+** and **npm 8+**
- **Rosetta 2** — only needed to build x86_64 on Apple Silicon Macs (for `arch -x86_64` build step). Install via `softwareupdate --install-rosetta` if `arch -x86_64` fails.

## Install & Build

```bash
# Fresh clone
git clone <repo>
cd HelpieAudioBridge
npm install

# Build both architectures (cleans build/ automatically)
./build.sh
```

Build writes signed, ad-hoc binaries to:

- `bin/arm64/audio_capture_arm64.node` (Apple Silicon, ~120-140K)
- `bin/x86_64/audio_capture_x86_64.node` (Intel, ~80-100K)

Build verifies arch via `file` and rejects unexpected deps (`libswift`, `libAudioCapture`, `@rpath`).

## JavaScript API

```js
// Auto-selects correct binary for current arch
const { AudioCapture } = require("./");

const capture = new AudioCapture();

// Register callback first (does NOT keep Node alive thanks to Unref)
capture.onData((float32Samples, channels) => {
  console.log(float32Samples.length, channels); // e.g. 9600 samples, 2 channels per 100ms batch
});

// Start capture — requires Screen Recording permission on macOS
// System Settings → Privacy & Security → Screen Recording → enable your Terminal/Electron app
const ok = capture.startCapture();
console.log("started:", ok); // false if permission denied or no display

// Stop — keeps callback for start/stop/start cycle
capture.stopCapture();

// Start again — callback still valid
capture.startCapture();
capture.stopCapture();

// Clear callback safely — allows process to exit
capture.clearCallback();
```

### Lifecycle rules (important for release)

- `onData(cb)` — registers callback, replaces previous (no leak), calls `Unref()` so it does NOT keep Node process alive by itself. Safe to call without `startCapture()`.
- `startCapture()` → `stopCapture()` → `startCapture()` works — callback preserved across stops. Ring buffer reset on each start.
- `clearCallback()` releases ThreadSafeFunction and allows Node to exit cleanly. Destructor also releases safely under mutex.
- Backpressure: bounded queue size 2 + pending-batch flag — only one JS callback queued at a time, intermediate batches dropped (ring buffer keeps newest audio, 10 sec capacity).

For Electron main process, you likely want:

```js
const { AudioCapture } = require("helpie-audio-bridge");
const cap = new AudioCapture();
cap.onData((buffer, channels) => {
  // buffer is Float32Array, transfer to renderer via IPC or process
  mainWindow.webContents.send('audio-chunk', { buffer: buffer.buffer, channels });
});
cap.startCapture();
```

## Verify

```bash
npm run check          # should print: function function function function
./build.sh             # rebuild both archs
file bin/arm64/*.node bin/x86_64/*.node
otool -L bin/arm64/audio_capture_arm64.node
otool -L bin/x86_64/audio_capture_x86_64.node
```

Expected `otool -L` — only Apple frameworks, no Swift, no dylib:

```
ScreenCaptureKit, AVFoundation, CoreAudio, AudioToolbox, Foundation, libc++, libSystem, CoreFoundation, libobjc
```

Checks that should PASS (build.sh does them automatically):

- `file` shows `arm64` for arm64 binary, `x86_64` for x64 binary
- No `libswift`, no `libAudioCapture`, no `@rpath`

## Troubleshooting

**Screen Recording permission:**
- macOS requires Screen Recording permission for system audio capture.
- On first `startCapture()`, macOS shows prompt. If denied, `startCapture()` returns `false`.
- Fix: System Settings → Privacy & Security → Screen Recording → enable Terminal (for Node) or your Electron app.
- Check: `systemPreferences.getMediaAccessStatus('screen')` in Electron should be `granted`.

**x86_64 build fails on Apple Silicon:**
- Need Rosetta: `softwareupdate --install-rosetta`
- Then `arch -x86_64` will work.

**Build from clean checkout fails:**
- Ensure `node_modules` exists: `npm install` (requires `node-addon-api`)
- Ensure Xcode CLT: `xcode-select --install` and `xcodebuild` works
- Deployment target is 13.0 — older macOS will fail at compile (expected)

**Process hangs after onData:**
- Fixed in v2: TSFN is `Unref()`ed, so `onData()` alone does not keep Node alive.
- Test: `node -e "require('./').AudioCapture; new (require('./').AudioCapture)().onData(()=>{})"` should exit immediately.
- If you see hang, ensure you called `clearCallback()` or let instance GC.

**No audio data:**
- ScreenCaptureKit captures system audio excluding current process (`excludesCurrentProcessAudio=YES`). Play audio from another app.
- Check `availableFrames()` via debugging or add logging in `audio_capture.mm`.

## Performance Notes

- Minimal video 2×2 (not full screen) — huge GPU saving vs old Swift version
- No Swift runtime — binary ~120K vs old 247K, no libswift deps
- Lock-free SPSC ring (10 sec, drops oldest on overflow), batched 100ms (4800 frames) → 10Hz IPC vs 50Hz
- Mutex-protected TSFN replacement/release + pending-batch flag for backpressure
- Single malloc per audio chunk in native, one ArrayBuffer alloc per batch in JS (could be optimized further with thread_local buffer)

## Packaging as reusable library

- `index.js` auto-selects arch via `process.arch`
- `package.json` `main: index.js`, `files: [index.js, bin/, source]` for npm
- `private:true` keeps it internal for now — remove to publish
- Binaries committed in `bin/` for Helpie convenience; for public reuse, consider GitHub releases or `prebuild`
- `package-lock.json` committed for reproducible builds
