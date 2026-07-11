# Helpie Audio Bridge

Native macOS audio capture bridge for Electron/Node, built with Objective-C++,
N-API, and Apple ScreenCaptureKit.

## What It Contains

- `audio_capture.mm` - ScreenCaptureKit audio capture implementation
- `wrapper.cc` - N-API bridge exposed to JavaScript
- `ring_buffer.h` - native audio ring buffer and batching
- `audio_capture.h` - native C/Objective-C interface
- `binding.gyp` - node-gyp build config
- `build.sh` - reproducible arm64 and x86_64 build script
- `bin/arm64/audio_capture_arm64.node` - Apple Silicon binary
- `bin/x86_64/audio_capture_x86_64.node` - Intel Mac binary

## JavaScript API

```js
const { AudioCapture } = require("./bin/arm64/audio_capture_arm64.node");

const capture = new AudioCapture();
capture.onData((float32Samples, channels) => {
  console.log(float32Samples.length, channels);
});

capture.startCapture();
capture.stopCapture();
capture.clearCallback();
```

## Build

```bash
npm install
./build.sh
```

The build writes signed binaries to:

- `bin/arm64/audio_capture_arm64.node`
- `bin/x86_64/audio_capture_x86_64.node`

## Verify

```bash
npm run check
otool -L bin/arm64/audio_capture_arm64.node
otool -L bin/x86_64/audio_capture_x86_64.node
```

The binaries should depend only on Apple frameworks and standard system
libraries. There is no Swift runtime and no separate dylib.

