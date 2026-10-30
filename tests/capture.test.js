'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const test = require('node:test');

const { AudioCapture } = require('..');

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

// ScreenCaptureKit keeps a client for this process after the first capture.
// Another process of the same application cannot start until this one exits,
// so this check has to run before any capture in this file.
test('child process can start and stop without hanging', async () => {
  const result = await new Promise((resolve) => {
    const child = spawn(process.execPath, ['-e', `
      console.log('boot');
      const { AudioCapture } = require('./');
      const capture = new AudioCapture();
      capture.on('data', () => {});
      capture.start()
        .then(() => { console.log('started'); return capture.stop(); })
        .then(() => { console.log('stopped'); capture.close(); console.log('closed'); process.exit(0); })
        .catch((error) => {
          console.error('fail', error && error.stack ? error.stack : error);
          process.exit(error && (error.code === 'permission-denied' || error.code === 'no-display') ? 0 : 1);
        });
    `], {
      cwd: require('node:path').join(__dirname, '..'),
      env: { ...process.env, NODE_CHANNEL_FD: '', NODE_TEST_CONTEXT: '' },
    });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      resolve({ code: null, stdout, stderr });
    }, 8000);
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
    });
    child.stderr.on('data', (chunk) => {
      stderr += chunk;
    });
    child.on('close', (code) => {
      clearTimeout(timer);
      resolve({ code, stdout, stderr });
    });
  });
  assert.equal(result.code, 0, `${result.stderr}\n${result.stdout}`);
});

test('start, stop, and start again deliver float frames', async () => {
  const capture = new AudioCapture({ batchMs: 100 });
  let chunks = 0;
  process.on('uncaughtException', (error) => {
    if (!error || error.message !== 'listener failed once') throw error;
  });
  let thrown = 0;
  capture.on('data', (chunk) => {
    chunks += 1;
    assert.ok(chunk.samples instanceof Float32Array);
    assert.equal(chunk.samples.length, chunk.frames * chunk.channels);
    assert.ok(chunk.sampleRate >= 8000);
    assert.ok(chunk.channels >= 1);
    assert.equal(typeof chunk.timestamp, 'number');
    if (chunks === 1) {
      thrown += 1;
      throw new Error('listener failed once');
    }
  });
  capture.on('stopped', () => {
    thrown += 10;
  });

  try {
    await capture.start();
  } catch (error) {
    capture.close();
    if (error && (error.code === 'permission-denied' || error.code === 'no-display')) {
      return;
    }
    throw error;
  }

  await sleep(700);
  assert.ok(chunks >= 3, `expected audio batches, got ${chunks}`);
  const afterThrow = chunks;
  await capture.stop();
  await sleep(250);
  assert.equal(chunks, afterThrow, 'stop should end the batches');
  assert.equal(thrown, 1, 'a user stop should not emit stopped');

  await capture.start();
  await sleep(400);
  assert.ok(chunks > afterThrow, 'start after stop should deliver audio again');
  await capture.stop();
  capture.close();
});

test('stop resolves when capture was never started', async () => {
  const capture = new AudioCapture();
  await capture.stop();
  capture.close();
});
