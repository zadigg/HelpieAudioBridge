#import "audio_capture.h"
#import "pcm.h"

#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <os/log.h>

#define LOG(fmt, ...) os_log(OS_LOG_DEFAULT, "[SCKAudioCapture] " fmt, ##__VA_ARGS__)

static const int64_t kContentTimeoutSec = 60;
static const int64_t kStartTimeoutSec = 20;
static const int64_t kStopTimeoutSec = 20;

static BOOL WaitFor(dispatch_semaphore_t sem, int64_t seconds) {
  if (!sem) return NO;
  return dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, seconds * NSEC_PER_SEC)) == 0;
}

static void CopyMessage(char* buffer, size_t len, NSString* text) {
  if (!buffer || len == 0) return;
  buffer[0] = '\0';
  if (text.length == 0) return;
  if (![text getCString:buffer maxLength:len encoding:NSUTF8StringEncoding]) {
    buffer[0] = '\0';
  }
}

static int StatusForError(NSError* error) {
  if (!error) return AUDIO_CAPTURE_STREAM_FAILED;
  if ([error.domain isEqualToString:SCStreamErrorDomain]) {
    switch (error.code) {
      case SCStreamErrorUserDeclined:
        return AUDIO_CAPTURE_PERMISSION_DENIED;
      case SCStreamErrorNoDisplayList:
      case SCStreamErrorNoCaptureSource:
        return AUDIO_CAPTURE_NO_DISPLAY;
      default:
        break;
    }
  }
  NSString* desc = error.localizedDescription.lowercaseString ?: @"";
  if ([desc containsString:@"permission"] || [desc containsString:@"declined"] ||
      [desc containsString:@"not authorized"] || [desc containsString:@"tcc"] ||
      [desc containsString:@"screen recording"]) {
    return AUDIO_CAPTURE_PERMISSION_DENIED;
  }
  return AUDIO_CAPTURE_STREAM_FAILED;
}

@interface SCKAudioCapture : NSObject <SCStreamDelegate, SCStreamOutput>
@property(nonatomic, assign) AudioCaptureDataFn dataCallback;
@property(nonatomic, assign) AudioCaptureStoppedFn stoppedCallback;
@property(nonatomic, assign) void* userData;
@property(nonatomic, strong) SCStream* stream;
@property(nonatomic, strong) SCContentFilter* filter;
@property(nonatomic, strong) dispatch_queue_t queue;
@property(atomic) BOOL isCapturing;
@property(nonatomic) int sampleRate;
@property(nonatomic) int channels;
@property(nonatomic) BOOL excludesCurrentProcessAudio;
@property(atomic) uint64_t activeGeneration;
- (instancetype)initWithSampleRate:(int)sampleRate
                           channels:(int)channels
     excludesCurrentProcessAudio:(BOOL)excludes;
- (int)startWithMessage:(char*)message length:(size_t)length;
- (void)stop;
@end

@implementation SCKAudioCapture

- (instancetype)initWithSampleRate:(int)sampleRate
                           channels:(int)channels
     excludesCurrentProcessAudio:(BOOL)excludes {
  self = [super init];
  if (self) {
    _sampleRate = sampleRate > 0 ? sampleRate : 48000;
    _channels = channels > 0 ? channels : 2;
    _excludesCurrentProcessAudio = excludes;
    dispatch_queue_attr_t attr =
        dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);
    _queue = dispatch_queue_create("com.helpie.audio.capture", attr);
    _isCapturing = NO;
    _activeGeneration = 0;
  }
  return self;
}

- (int)fail:(int)status message:(char*)message length:(size_t)length text:(NSString*)text {
  CopyMessage(message, length, text);
  LOG("%{public}@", text);
  return status;
}

- (void)invalidateStream {
  self.activeGeneration = 0;
  self.isCapturing = NO;
  if (!_stream) {
    _filter = nil;
    return;
  }

  SCStream* stream = _stream;
  _stream = nil;
  _filter = nil;

  if (_queue) {
    dispatch_sync(_queue, ^{
    });
  }

  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [stream stopCaptureWithCompletionHandler:^(NSError* _Nullable error) {
    if (error) {
      LOG("stopCapture error: %{public}@", error.localizedDescription);
    }
    NSError* removeError = nil;
    if (@available(macOS 13.0, *)) {
      [stream removeStreamOutput:self type:SCStreamOutputTypeAudio error:&removeError];
    }
    dispatch_semaphore_signal(sem);
  }];
  if (!WaitFor(sem, kStopTimeoutSec)) {
    LOG("Timed out waiting for stopCapture");
  }

  // Drop the stream on this thread. Node does not run the main queue while
  // libuv is blocked, so handing the last reference to that queue leaks it.
  stream = nil;
}

- (int)startWithMessage:(char*)message length:(size_t)length {
  if (message && length > 0) message[0] = '\0';
  [self invalidateStream];

  __block SCShareableContent* content = nil;
  __block NSError* contentError = nil;
  dispatch_semaphore_t contentSem = dispatch_semaphore_create(0);
  [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* _Nullable value,
                                                                  NSError* _Nullable error) {
    content = value;
    contentError = error;
    dispatch_semaphore_signal(contentSem);
  }];
  if (!WaitFor(contentSem, kContentTimeoutSec)) {
    return [self fail:AUDIO_CAPTURE_TIMEOUT
               message:message
                length:length
                  text:@"Timed out waiting for screen capture permission or shareable content."];
  }
  if (contentError) {
    return [self fail:StatusForError(contentError)
               message:message
                length:length
                  text:contentError.localizedDescription ?: @"Failed to read shareable content."];
  }
  if (!content || content.displays.count == 0) {
    return [self fail:AUDIO_CAPTURE_NO_DISPLAY
               message:message
                length:length
                  text:@"No display is available to capture."];
  }

  SCDisplay* display = content.displays.firstObject;
  SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:display
                                                excludingApplications:@[]
                                                     exceptingWindows:@[]];
  self.filter = filter;

  SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
  config.width = 2;
  config.height = 2;
  config.capturesAudio = YES;
  config.sampleRate = _sampleRate;
  config.channelCount = _channels;
  config.excludesCurrentProcessAudio = _excludesCurrentProcessAudio;
  config.minimumFrameInterval = CMTimeMake(1, 5);

  SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
  if (!stream) {
    _filter = nil;
    return [self fail:AUDIO_CAPTURE_STREAM_FAILED
               message:message
                length:length
                  text:@"Failed to create the capture stream."];
  }
  _stream = stream;

  NSError* outputError = nil;
  BOOL added = NO;
  if (@available(macOS 13.0, *)) {
    added = [stream addStreamOutput:self
                                type:SCStreamOutputTypeAudio
                  sampleHandlerQueue:_queue
                               error:&outputError];
  }
  if (!added) {
    [self invalidateStream];
    return [self fail:StatusForError(outputError)
               message:message
                length:length
                  text:outputError.localizedDescription ?: @"Failed to add the audio stream output."];
  }

  dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
  __block NSError* startError = nil;
  __block BOOL started = NO;
  [stream startCaptureWithCompletionHandler:^(NSError* _Nullable error) {
    startError = error;
    started = (error == nil);
    dispatch_semaphore_signal(startSem);
  }];
  if (!WaitFor(startSem, kStartTimeoutSec)) {
    [self invalidateStream];
    return [self fail:AUDIO_CAPTURE_TIMEOUT
               message:message
                length:length
                  text:@"Timed out waiting for the capture stream to start."];
  }
  if (!started) {
    int status = StatusForError(startError);
    NSString* text = startError.localizedDescription ?: @"Failed to start capture.";
    [self invalidateStream];
    return [self fail:status message:message length:length text:text];
  }

  self.activeGeneration += 1;
  self.isCapturing = YES;
  LOG("Capture started sr=%d ch=%d", self.sampleRate, self.channels);
  return AUDIO_CAPTURE_OK;
}

- (void)stop {
  self.activeGeneration = 0;
  self.isCapturing = NO;
  [self invalidateStream];
  LOG("Capture stopped");
}

- (void)emitSampleBuffer:(CMSampleBufferRef)sampleBuffer {
  if (!CMSampleBufferIsValid(sampleBuffer) || !_dataCallback) return;

  CMFormatDescriptionRef format = CMSampleBufferGetFormatDescription(sampleBuffer);
  const AudioStreamBasicDescription* asbd =
      format ? CMAudioFormatDescriptionGetStreamBasicDescription(format) : NULL;
  if (!asbd) return;
  if (asbd->mFormatID != kAudioFormatLinearPCM) return;
  if ((asbd->mFormatFlags & kAudioFormatFlagIsFloat) == 0) return;
  if (asbd->mBitsPerChannel != 32) return;
  if ((asbd->mFormatFlags & kAudioFormatFlagIsBigEndian) != 0) return;

  uint32_t channels = (uint32_t)asbd->mChannelsPerFrame;
  if (channels == 0 || channels > 8) return;
  CMItemCount frames = CMSampleBufferGetNumSamples(sampleBuffer);
  if (frames <= 0 || frames > 1000000) return;
  if ((size_t)frames > SIZE_MAX / channels) return;

  const uint32_t flags = kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment;
  size_t listSize = 0;
  OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
      sampleBuffer, &listSize, NULL, 0, NULL, NULL, flags, NULL);
  if (status != noErr || listSize == 0) return;

  AudioBufferList* list = (AudioBufferList*)malloc(listSize);
  if (!list) return;
  CMBlockBufferRef block = NULL;
  status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
      sampleBuffer, NULL, list, listSize, NULL, NULL, flags, &block);
  if (status != noErr) {
    free(list);
    if (block) CFRelease(block);
    return;
  }

  size_t total = (size_t)frames * channels;
  float* temp = (float*)malloc(total * sizeof(float));
  if (!temp) {
    free(list);
    if (block) CFRelease(block);
    return;
  }

  BOOL converted = NO;
  BOOL nonInterleaved =
      ((asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0) || list->mNumberBuffers > 1;
  if (!nonInterleaved && list->mNumberBuffers > 0) {
    AudioBuffer buf = list->mBuffers[0];
    converted = CopyInterleavedFloat32(buf.mData, buf.mDataByteSize, (size_t)frames, channels, temp);
  } else {
    FloatPlane planes[8];
    std::memset(planes, 0, sizeof(planes));
    UInt32 planeCount = list->mNumberBuffers;
    if (planeCount > 8) planeCount = 8;
    if (planeCount > channels) planeCount = channels;
    for (UInt32 i = 0; i < planeCount; i++) {
      planes[i].data = (const float*)list->mBuffers[i].mData;
      planes[i].bytes = list->mBuffers[i].mDataByteSize;
    }
    converted = InterleaveFloat32(planes, planeCount, (size_t)frames, channels, temp);
  }

  if (converted && _dataCallback) {
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    double timestamp = 0;
    if (CMTIME_IS_NUMERIC(pts)) {
      timestamp = CMTimeGetSeconds(pts);
      if (!std::isfinite(timestamp)) timestamp = 0;
    }
    AudioCaptureChunk chunk;
    chunk.samples = temp;
    chunk.frames = (int32_t)frames;
    chunk.channels = (int32_t)channels;
    chunk.sampleRate = (int32_t)llround(asbd->mSampleRate);
    chunk.timestamp = timestamp;
    _dataCallback(&chunk, _userData);
  }

  free(temp);
  free(list);
  if (block) CFRelease(block);
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
  uint64_t generation = self.activeGeneration;
  if (generation == 0) return;
  self.activeGeneration = 0;
  self.isCapturing = NO;
  NSString* text = error.localizedDescription ?: @"The system stopped audio capture.";
  LOG("Stream stopped: %{public}@", text);
  if (_stoppedCallback) {
    char message[512];
    CopyMessage(message, sizeof(message), text);
    int status = StatusForError(error);
    _stoppedCallback(status, message, _userData);
  }
  (void)stream;
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  if (@available(macOS 13.0, *)) {
    if (type != SCStreamOutputTypeAudio) return;
  }
  if (!self.isCapturing) return;
  [self emitSampleBuffer:sampleBuffer];
  (void)stream;
}

@end

namespace {

struct AudioCaptureContext {
  void* retained;
};

SCKAudioCapture* CaptureObject(void* handle) {
  if (!handle) return nil;
  return (__bridge SCKAudioCapture*)((AudioCaptureContext*)handle)->retained;
}

}  // namespace

void* AudioCapture_create(int sampleRate, int channels, int excludesCurrentProcessAudio) {
  @autoreleasepool {
    AudioCaptureContext* ctx = (AudioCaptureContext*)calloc(1, sizeof(AudioCaptureContext));
    if (!ctx) return NULL;
    SCKAudioCapture* obj = [[SCKAudioCapture alloc] initWithSampleRate:sampleRate
                                                               channels:channels
                                         excludesCurrentProcessAudio:excludesCurrentProcessAudio ? YES : NO];
    if (!obj) {
      free(ctx);
      return NULL;
    }
    ctx->retained = (__bridge_retained void*)obj;
    return ctx;
  }
}

void AudioCapture_destroy(void* handle) {
  @autoreleasepool {
    if (!handle) return;
    AudioCaptureContext* ctx = (AudioCaptureContext*)handle;
    SCKAudioCapture* obj = (__bridge_transfer SCKAudioCapture*)ctx->retained;
    ctx->retained = nullptr;
    if (obj) [obj stop];
    obj = nil;
    free(ctx);
  }
}

void AudioCapture_setCallbacks(void* handle, AudioCaptureDataFn onData, AudioCaptureStoppedFn onStopped,
                               void* userData) {
  SCKAudioCapture* obj = CaptureObject(handle);
  if (!obj) return;
  obj.dataCallback = onData;
  obj.stoppedCallback = onStopped;
  obj.userData = userData;
}

int AudioCapture_start(void* handle, char* message, size_t messageLen) {
  @autoreleasepool {
    SCKAudioCapture* obj = CaptureObject(handle);
    if (!obj) return AUDIO_CAPTURE_STREAM_FAILED;
    return [obj startWithMessage:message length:messageLen];
  }
}

int AudioCapture_stop(void* handle) {
  @autoreleasepool {
    SCKAudioCapture* obj = CaptureObject(handle);
    if (!obj) return AUDIO_CAPTURE_OK;
    [obj stop];
    return AUDIO_CAPTURE_OK;
  }
}

int AudioCapture_isCapturing(void* handle) {
  SCKAudioCapture* obj = CaptureObject(handle);
  return obj && obj.isCapturing ? 1 : 0;
}
