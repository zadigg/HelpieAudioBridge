#import "audio_capture.h"
#import <CoreAudio/CoreAudio.h>
#import <AudioToolbox/AudioToolbox.h>
#include <os/log.h>

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[SCKAudioCapture v2] " fmt, ##__VA_ARGS__)

@interface SCKAudioCapture ()
@property (nonatomic, strong) SCContentFilter* filter;
@end

@implementation SCKAudioCapture

- (instancetype)initWithSampleRate:(int)sr channels:(int)ch {
  self = [super init];
  if (self) {
    _sampleRate = sr > 0 ? sr : 48000;
    _channels = ch > 0 ? ch : 2;
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    _queue = dispatch_queue_create("com.helpie.audio.capture.v2", attr);
    _isCapturing = NO;
  }
  return self;
}

- (BOOL)start {
  if (_isCapturing) {
    [self stop];
  }

  @autoreleasepool {
    __block SCShareableContent* content = nil;
    __block NSError* contentError = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);

    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent * _Nullable c, NSError * _Nullable error) {
      content = c;
      contentError = error;
      dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));

    if (contentError || !content || content.displays.count == 0) {
      LOG("Failed to get shareable content: %{public}@", contentError ? contentError.localizedDescription : @"no displays");
      return NO;
    }

    SCDisplay* display = content.displays.firstObject;
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display excludingApplications:@[] exceptingWindows:@[]];
    self.filter = filter;

    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.width = 2;  // Minimal video for audio-only capture
    config.height = 2;
    config.capturesAudio = YES;
    config.sampleRate = _sampleRate;
    config.channelCount = _channels;
    config.excludesCurrentProcessAudio = YES;
    config.minimumFrameInterval = CMTimeMake(1, 5); // 5 FPS video

    _stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];

    if (!_stream) {
      LOG("Failed to create SCStream");
      _filter = nil;
      return NO;
    }

    NSError* audioError = nil;
    BOOL ok = NO;
    if (@available(macOS 13.0, *)) {
      ok = [_stream addStreamOutput:self type:SCStreamOutputTypeAudio sampleHandlerQueue:_queue error:&audioError];
    } else {
      ok = [_stream addStreamOutput:self type:SCStreamOutputTypeAudio sampleHandlerQueue:_queue error:&audioError];
    }
    if (!ok) {
      LOG("Failed to add audio output: %{public}@", audioError ? audioError.localizedDescription : @"unknown");
      _stream = nil;
      _filter = nil;
      return NO;
    }

    dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
    __block BOOL startSuccess = NO;
    [_stream startCaptureWithCompletionHandler:^(NSError * _Nullable error) {
      if (error) {
        LOG("startCapture failed: %{public}@", error.localizedDescription);
      } else {
        startSuccess = YES;
        LOG("Capture started sr=%d ch=%d", self->_sampleRate, self->_channels);
      }
      dispatch_semaphore_signal(startSem);
    }];
    dispatch_semaphore_wait(startSem, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));

    if (!startSuccess) {
      _stream = nil;
      _filter = nil;
      return NO;
    }

    _isCapturing = YES;
    return YES;
  }
}

- (void)stop {
  if (!_isCapturing) return;
  _isCapturing = NO;

  @autoreleasepool {
    if (_stream) {
      SCStream* streamToStop = _stream;
      _stream = nil;
      dispatch_semaphore_t sem = dispatch_semaphore_create(0);
      [streamToStop stopCaptureWithCompletionHandler:^(NSError * _Nullable error) {
        if (error) {
          LOG("stopCapture error: %{public}@", error.localizedDescription);
        }
        dispatch_semaphore_signal(sem);
      }];
      dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
    }
    _filter = nil;
  }
  LOG("Capture stopped");
}

#pragma mark - SCStreamDelegate

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
  LOG("Stream stopped with error: %{public}@", error.localizedDescription);
  _isCapturing = NO;
}

#pragma mark - SCStreamOutput

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
  if (@available(macOS 13.0, *)) {
    if (type != SCStreamOutputTypeAudio) return;
  } else {
    // Fallback: check raw value for audio (1 = audio on older SDKs)
    if ((NSInteger)type != 1) return;
  }
  if (!_isCapturing || !_callback) return;
  if (!CMSampleBufferIsValid(sampleBuffer)) return;

  @autoreleasepool {
    CMItemCount numSamples = CMSampleBufferGetNumSamples(sampleBuffer);
    if (numSamples == 0) return;

    // Get audio buffer list
    AudioBufferList* bufferList = NULL;
    size_t bufferListSize = 0;
    OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
      sampleBuffer, &bufferListSize, NULL, 0, NULL, NULL, 0, NULL
    );
    if (status != noErr || bufferListSize == 0) return;

    bufferList = (AudioBufferList*)malloc(bufferListSize);
    CMBlockBufferRef blockBuffer = NULL;
    status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
      sampleBuffer, NULL, bufferList, bufferListSize, NULL, NULL, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &blockBuffer
    );

    if (status == noErr && bufferList->mNumberBuffers > 0) {
      // Fast path: deinterleave if needed to interleaved float
      int totalSamples = numSamples * _channels;
      float* temp = (float*)malloc(totalSamples * sizeof(float));
      if (temp) {
        if (bufferList->mNumberBuffers == 1) {
          AudioBuffer buf = bufferList->mBuffers[0];
          size_t bytesToCopy = buf.mDataByteSize < totalSamples * sizeof(float) ? buf.mDataByteSize : totalSamples * sizeof(float);
          if (buf.mData) {
            memcpy(temp, buf.mData, bytesToCopy);
            if (bytesToCopy < totalSamples * sizeof(float)) {
              memset((uint8_t*)temp + bytesToCopy, 0, totalSamples * sizeof(float) - bytesToCopy);
            }
          }
        } else {
          // Non-interleaved deinterleave
          for (int i = 0; i < numSamples; i++) {
            for (int ch = 0; ch < _channels && ch < bufferList->mNumberBuffers; ch++) {
              float* src = (float*)bufferList->mBuffers[ch].mData;
              temp[i * _channels + ch] = src ? src[i] : 0.0f;
            }
          }
        }
        if (_callback) {
          _callback(temp, numSamples, _channels, _userData);
        }
        free(temp);
      }
    }

    if (bufferList) free(bufferList);
    if (blockBuffer) CFRelease(blockBuffer);
  }
}

@end

// C ABI
typedef struct {
  SCKAudioCapture* obj;
} AudioCaptureContext;

void* AudioCapture_create() {
  @autoreleasepool {
    AudioCaptureContext* ctx = (AudioCaptureContext*)calloc(1, sizeof(AudioCaptureContext));
    ctx->obj = [[SCKAudioCapture alloc] initWithSampleRate:48000 channels:2];
    return ctx;
  }
}

void AudioCapture_destroy(void* handle) {
  @autoreleasepool {
    if (!handle) return;
    AudioCaptureContext* ctx = (AudioCaptureContext*)handle;
    [ctx->obj stop];
    ctx->obj = nil;
    free(ctx);
  }
}

int AudioCapture_startCapture(void* handle) {
  @autoreleasepool {
    if (!handle) return 0;
    AudioCaptureContext* ctx = (AudioCaptureContext*)handle;
    return [ctx->obj start] ? 1 : 0;
  }
}

int AudioCapture_stopCapture(void* handle) {
  @autoreleasepool {
    if (!handle) return 0;
    AudioCaptureContext* ctx = (AudioCaptureContext*)handle;
    [ctx->obj stop];
    return 1;
  }
}

void AudioCapture_setCallback(void* handle, AudioDataCallback cb, void* userData) {
  if (!handle) return;
  AudioCaptureContext* ctx = (AudioCaptureContext*)handle;
  ctx->obj.callback = cb;
  ctx->obj.userData = userData;
}
