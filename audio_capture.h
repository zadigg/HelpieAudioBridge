#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AudioCaptureStatus {
  AUDIO_CAPTURE_OK = 0,
  AUDIO_CAPTURE_PERMISSION_DENIED = 1,
  AUDIO_CAPTURE_NO_DISPLAY = 2,
  AUDIO_CAPTURE_TIMEOUT = 3,
  AUDIO_CAPTURE_STREAM_FAILED = 4,
} AudioCaptureStatus;

typedef struct AudioCaptureChunk {
  const float* samples;
  int32_t frames;
  int32_t channels;
  int32_t sampleRate;
  double timestamp;
} AudioCaptureChunk;

typedef void (*AudioCaptureDataFn)(const AudioCaptureChunk* chunk, void* userData);
typedef void (*AudioCaptureStoppedFn)(int status, const char* message, void* userData);

void* AudioCapture_create(int sampleRate, int channels, int excludesCurrentProcessAudio);
void AudioCapture_destroy(void* handle);
void AudioCapture_setCallbacks(void* handle, AudioCaptureDataFn onData,
                               AudioCaptureStoppedFn onStopped, void* userData);
int AudioCapture_start(void* handle, char* message, size_t messageLen);
int AudioCapture_stop(void* handle);
int AudioCapture_isCapturing(void* handle);

#ifdef __cplusplus
}
#endif
