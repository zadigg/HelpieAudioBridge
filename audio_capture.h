#pragma once
#include <functional>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif

// C ABI - pure C, safe for C++ inclusion
typedef void (*AudioDataCallback)(const float* data, int32_t frames, int32_t channels, void* userData);

void* AudioCapture_create(void);
void AudioCapture_destroy(void* handle);
int AudioCapture_startCapture(void* handle);
int AudioCapture_stopCapture(void* handle);
void AudioCapture_setCallback(void* handle, AudioDataCallback cb, void* userData);

#ifdef __cplusplus
}
#endif

#ifdef __OBJC__
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <AVFoundation/AVFoundation.h>

@interface SCKAudioCapture : NSObject <SCStreamDelegate, SCStreamOutput>
@property (nonatomic, assign) AudioDataCallback callback;
@property (nonatomic, assign) void* userData;
@property (nonatomic, strong) SCStream* stream;
@property (nonatomic, strong) dispatch_queue_t queue;
@property (atomic) BOOL isCapturing;
@property (nonatomic) int sampleRate;
@property (nonatomic) int channels;
- (instancetype)initWithSampleRate:(int)sr channels:(int)ch;
- (BOOL)start;
- (void)stop;
@end
#endif
