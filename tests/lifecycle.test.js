'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const path = require('node:path');
const test = require('node:test');

const root = path.join(__dirname, '..');

function runScript(script, timeoutMs = 4000) {
  return new Promise((resolve) => {
    const child = spawn(process.execPath, ['-e', script], {
      cwd: root,
      env: { ...process.env, NODE_CHANNEL_FD: '', NODE_TEST_CONTEXT: '' },
    });
    let stdout = '';
    let stderr = '';
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      resolve({ code: null, stdout, stderr, timedOut: true });
    }, timeoutMs);
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
    });
    child.stderr.on('data', (chunk) => {
      stderr += chunk;
    });
    child.on('close', (code) => {
      clearTimeout(timer);
      resolve({ code, stdout, stderr, timedOut: false });
    });
  });
}

test('options and events reject bad input', () => {
  const { AudioCapture } = require('..');
  const capture = new AudioCapture();
  assert.equal(typeof capture.start, 'function');
  assert.equal(typeof capture.stop, 'function');
  assert.equal(typeof capture.on, 'function');
  assert.equal(typeof capture.close, 'function');
  assert.throws(() => new AudioCapture({ sampleRate: 1000 }), /sampleRate/);
  assert.throws(() => new AudioCapture({ channels: 5 }), /channels/);
  assert.throws(() => new AudioCapture('nope'), /object/);
  assert.throws(() => capture.on('data', null), /function/);
  assert.throws(() => capture.on('volume', () => {}), /Unknown event/);
  capture.close();
  assert.throws(() => capture.on('data', () => {}), /closed/);
});

test('registering a listener does not keep the process alive', async () => {
  const result = await runScript(`
    const { AudioCapture } = require('./');
    const capture = new AudioCapture();
    capture.on('data', () => {});
    capture.on('stopped', () => {});
    capture.close();
  `);
  assert.equal(result.timedOut, false, result.stderr);
  assert.equal(result.code, 0, result.stderr);
});

test('a running capture keeps the process alive until it is stopped', async () => {
  const started = Date.now();
  const result = await runScript(`
    const { AudioCapture } = require('./');
    const capture = new AudioCapture();
    capture.on('data', () => {});
    capture.start()
      .then(() => new Promise((resolve) => setTimeout(resolve, 700)))
      .then(() => capture.stop())
      .then(() => capture.close())
      .then(() => process.exit(0))
      .catch((error) => {
        console.error(error && error.message ? error.message : error);
        process.exit(2);
      });
  `, 8000);
  assert.equal(result.timedOut, false, result.stderr);
  assert.equal(result.code, 0, result.stderr);
  assert.ok(Date.now() - started >= 600, 'capture should keep the process alive');
});
