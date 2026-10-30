'use strict';

const fs = require('fs');
const path = require('path');

function unsupported(message) {
  const error = new Error(message);
  error.code = 'unsupported-platform';
  error.name = 'AudioCaptureError';
  return error;
}

if (process.platform !== 'darwin') {
  throw unsupported('helpie-audio-bridge supports only macOS 13 or newer.');
}

const arch = process.arch;
let bindingPath;
if (arch === 'arm64') {
  bindingPath = path.join(__dirname, 'bin', 'arm64', 'audio_capture_arm64.node');
} else if (arch === 'x64') {
  bindingPath = path.join(__dirname, 'bin', 'x86_64', 'audio_capture_x86_64.node');
} else {
  throw unsupported(
    `Unsupported architecture: ${arch}. helpie-audio-bridge supports arm64 and x64 on macOS.`
  );
}

if (!fs.existsSync(bindingPath)) {
  throw unsupported(
    `Native audio bridge for ${arch} is missing at ${bindingPath}. Run npm run build:arm64 or ./build.sh.`
  );
}

try {
  module.exports = require(bindingPath);
} catch (err) {
  const error = new Error(
    `Failed to load the native audio bridge for ${arch}: ${err.message}`
  );
  error.code = 'unsupported-platform';
  error.name = 'AudioCaptureError';
  error.cause = err;
  throw error;
}
