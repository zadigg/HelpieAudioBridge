export type AudioCaptureErrorCode =
  | 'permission-denied'
  | 'no-display'
  | 'timeout'
  | 'stream-failed'
  | 'unsupported-platform';

export interface AudioCaptureError extends Error {
  name: 'AudioCaptureError';
  code: AudioCaptureErrorCode;
}

export interface AudioCaptureOptions {
  sampleRate?: number;
  channels?: 1 | 2;
  batchMs?: number;
  excludesCurrentProcessAudio?: boolean;
}

export interface AudioChunk {
  samples: Float32Array;
  channels: number;
  sampleRate: number;
  frames: number;
  timestamp: number;
}

export type AudioCaptureEvent = 'data' | 'stopped';

export class AudioCapture {
  constructor(options?: AudioCaptureOptions);
  start(): Promise<void>;
  stop(): Promise<void>;
  on(event: 'data', listener: (chunk: AudioChunk) => void): void;
  on(event: 'stopped', listener: (error: AudioCaptureError) => void): void;
  close(): void;
}
