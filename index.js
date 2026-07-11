'use strict';

const arch = process.arch;
let bindingPath;

if (arch === 'arm64') {
  bindingPath = './bin/arm64/audio_capture_arm64.node';
} else if (arch === 'x64') {
  // x64 for Intel Mac or x64 Node running under Rosetta.
  bindingPath = './bin/x86_64/audio_capture_x86_64.node';
} else {
  throw new Error(
    `Unsupported architecture: ${arch}. HelpieAudioBridge supports only arm64 (Apple Silicon) and x64 (Intel) on macOS 13+.`
  );
}

try {
  module.exports = require(bindingPath);
} catch (err) {
  // Provide clear error if binary missing
  const msg = `Failed to load native audio bridge for arch ${arch} at ${bindingPath}: ${err.message}. ` +
    `Did you run ./build.sh? Requires macOS 13+, Xcode Command Line Tools, Node 18+.`;
  const e = new Error(msg);
  e.cause = err;
  throw e;
}
