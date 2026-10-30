# Helpie Audio Bridge

Native macOS system-audio capture for Node.js. It uses ScreenCaptureKit from Objective-C++, with no Swift runtime.

Requires macOS 13 or newer, Xcode Command Line Tools, and Node.js 18 or newer.

## Build

```bash
npm install
npm run build:arm64
```

`./build.sh` also builds x64 when this machine's Node binary can run as x64. Otherwise it removes a stale x64 binary so it cannot be loaded by mistake.

## API

```js
const { AudioCapture } = require('./');

const capture = new AudioCapture({
  sampleRate: 48000,
  channels: 2,
  batchMs: 100,
  excludesCurrentProcessAudio: true,
});

capture.on('data', (chunk) => {
  // chunk.samples is an interleaved Float32Array
  // chunk.frames, chunk.channels, chunk.sampleRate
  // chunk.timestamp is the presentation time, in seconds, of the newest audio in the batch
  console.log(chunk.frames, chunk.channels, chunk.sampleRate);
});

capture.on('stopped', (error) => {
  // macOS ended the stream. error.code is permission-denied, no-display,
  // timeout, or stream-failed.
  console.error(error.code, error.message);
});

await capture.start();
await capture.stop();
capture.close();
```

`start()` and `stop()` return promises and do not block the JavaScript thread. `start()` rejects with an `AudioCaptureError` whose `code` is `permission-denied`, `no-display`, `timeout`, or `stream-failed`. Screen Recording permission is required. A permission prompt can keep `start()` pending for up to 60 seconds.

`close()` drops the listeners and stops capture. Call it before releasing the object.

While capture is running, the instance stays alive and the process stays alive. After `stop()` or `close()`, a listener by itself does not keep the process running.

The ring buffer holds about half a second. If the listener falls behind, older audio is dropped and the next batch is the newest one.

## Check

```bash
npm test
```

That builds nothing by itself. Run `npm run build:arm64` first. The capture test records system audio, so Screen Recording permission has to be granted to the terminal. It skips only when permission is denied or no display is available.
