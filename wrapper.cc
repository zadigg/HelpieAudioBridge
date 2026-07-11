#include <napi.h>
#include "audio_capture.h"
#include "ring_buffer.h"
#include <atomic>
#include <memory>
#include <vector>
#include <mutex>

class AudioCaptureWrapper : public Napi::ObjectWrap<AudioCaptureWrapper> {
public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  AudioCaptureWrapper(const Napi::CallbackInfo& info);
  ~AudioCaptureWrapper();

private:
  static Napi::FunctionReference constructor;
  Napi::Value StartCapture(const Napi::CallbackInfo& info);
  Napi::Value StopCapture(const Napi::CallbackInfo& info);
  Napi::Value OnData(const Napi::CallbackInfo& info);
  Napi::Value ClearCallback(const Napi::CallbackInfo& info);

  void* captureHandle_;
  std::unique_ptr<AudioRingBuffer> ringBuffer_;
  Napi::ThreadSafeFunction tsfn_;
  std::mutex tsfnMutex_; // protects tsfn_ replacement, release, and NonBlockingCall
  std::atomic<bool> hasCallback_;
  std::atomic<bool> pendingBatch_; // true if one JS callback queued, drop intermediate
  int sampleRate_;
  int channels_;
  int batchFrames_;

  static void AudioCallback(const float* data, int32_t frames, int32_t channels, void* userData);
  void ProcessBatched();
};

Napi::FunctionReference AudioCaptureWrapper::constructor;

Napi::Object AudioCaptureWrapper::Init(Napi::Env env, Napi::Object exports) {
  Napi::HandleScope scope(env);
  Napi::Function func = DefineClass(env, "AudioCapture", {
    InstanceMethod("startCapture", &AudioCaptureWrapper::StartCapture),
    InstanceMethod("stopCapture", &AudioCaptureWrapper::StopCapture),
    InstanceMethod("onData", &AudioCaptureWrapper::OnData),
    InstanceMethod("clearCallback", &AudioCaptureWrapper::ClearCallback),
  });
  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();
  exports.Set("AudioCapture", func);
  return exports;
}

AudioCaptureWrapper::AudioCaptureWrapper(const Napi::CallbackInfo& info)
  : Napi::ObjectWrap<AudioCaptureWrapper>(info),
    captureHandle_(nullptr),
    hasCallback_(false),
    pendingBatch_(false),
    sampleRate_(48000),
    channels_(2),
    batchFrames_(4800) {
  Napi::Env env = info.Env();
  ringBuffer_ = std::make_unique<AudioRingBuffer>(sampleRate_ * 10, 2);
  captureHandle_ = AudioCapture_create();
  if (!captureHandle_) {
    Napi::Error::New(env, "Failed to create audio capture").ThrowAsJavaScriptException();
  }
}

AudioCaptureWrapper::~AudioCaptureWrapper() {
  {
    std::lock_guard<std::mutex> lock(tsfnMutex_);
    if (hasCallback_.load() && tsfn_) {
      hasCallback_.store(false);
      tsfn_.Release();
    }
  }
  if (captureHandle_) {
    AudioCapture_stopCapture(captureHandle_);
    AudioCapture_destroy(captureHandle_);
    captureHandle_ = nullptr;
  }
}

Napi::Value AudioCaptureWrapper::StartCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (!captureHandle_) return Napi::Boolean::New(env, false);
  ringBuffer_->reset();
  pendingBatch_.store(false);
  int result = AudioCapture_startCapture(captureHandle_);
  return Napi::Boolean::New(env, result != 0);
}

Napi::Value AudioCaptureWrapper::StopCapture(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (captureHandle_) {
    AudioCapture_stopCapture(captureHandle_);
    ringBuffer_->reset();
    pendingBatch_.store(false);
  }
  // Do NOT release TSFN on stop - keep callback for start/stop/start cycle
  return env.Undefined();
}

Napi::Value AudioCaptureWrapper::ClearCallback(const Napi::CallbackInfo& info) {
  std::lock_guard<std::mutex> lock(tsfnMutex_);
  if (hasCallback_.load() && tsfn_) {
    tsfn_.Release();
  }
  hasCallback_.store(false);
  pendingBatch_.store(false);
  return info.Env().Undefined();
}

void AudioCaptureWrapper::AudioCallback(const float* data, int32_t frames, int32_t channels, void* userData) {
  AudioCaptureWrapper* self = static_cast<AudioCaptureWrapper*>(userData);
  if (!self || !self->hasCallback_.load()) return;
  if (!data || frames <= 0) return;
  self->ringBuffer_->push(data, frames);
  if (self->ringBuffer_->availableFrames() >= (size_t)self->batchFrames_) {
    self->ProcessBatched();
  }
}

void AudioCaptureWrapper::ProcessBatched() {
  // Backpressure: only one pending JS callback at a time, drop intermediate
  bool expected = false;
  if (!pendingBatch_.compare_exchange_strong(expected, true)) {
    // Already one queued, drop this batch - ring buffer keeps newest
    // Pop and discard to avoid growing indefinitely? No, keep in ring, will be picked up next time JS drains
    return;
  }

  size_t framesToPop = batchFrames_;
  std::vector<float> temp;
  temp.resize(framesToPop * channels_);
  size_t popped = ringBuffer_->pop(temp.data(), framesToPop);
  if (popped == 0) {
    pendingBatch_.store(false);
    return;
  }

  struct CallbackData {
    std::vector<float> samples;
    int channels;
    int frames;
    std::atomic<bool>* pendingFlag;
  };
  CallbackData* cbData = new CallbackData();
  cbData->samples = std::move(temp);
  cbData->samples.resize(popped * channels_);
  cbData->channels = channels_;
  cbData->frames = popped;
  cbData->pendingFlag = &pendingBatch_;

  napi_status status = napi_generic_failure;
  {
    std::lock_guard<std::mutex> lock(tsfnMutex_);
    if (!hasCallback_.load() || !tsfn_) {
      pendingBatch_.store(false);
      delete cbData;
      return;
    }

    status = tsfn_.NonBlockingCall(cbData, [](Napi::Env env, Napi::Function jsCallback, CallbackData* data) {
      size_t byteLength = data->samples.size() * sizeof(float);
      Napi::ArrayBuffer arrayBuffer = Napi::ArrayBuffer::New(env, byteLength);
      memcpy(arrayBuffer.Data(), data->samples.data(), byteLength);
      Napi::Float32Array floatArray = Napi::Float32Array::New(env, data->samples.size(), arrayBuffer, 0);
      jsCallback.Call({floatArray, Napi::Number::New(env, data->channels)});
      data->pendingFlag->store(false);
      delete data;
    });
  }

  if (status != napi_ok) {
    pendingBatch_.store(false);
    delete cbData;
  }
}

Napi::Value AudioCaptureWrapper::OnData(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "Function expected").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Function cb = info[0].As<Napi::Function>();

  {
    std::lock_guard<std::mutex> lock(tsfnMutex_);
    if (hasCallback_.load() && tsfn_) {
      tsfn_.Release();
      hasCallback_.store(false);
    }
    // Bounded queue 2 - backpressure: NonBlockingCall will return napi_queue_full if 2 pending
    tsfn_ = Napi::ThreadSafeFunction::New(
      env,
      cb,
      "AudioDataCallback",
      2, // maxQueueSize bounded - fixes unlimited queue
      1,
      [](Napi::Env) {}
    );
    // Unref so TSFN alone does not keep Node alive - fixes process hang on onData() without startCapture
    tsfn_.Unref(env);
    hasCallback_.store(true);
    pendingBatch_.store(false);
  }

  if (captureHandle_) {
    AudioCapture_setCallback(captureHandle_,
      [](const float* data, int32_t frames, int32_t channels, void* userData) {
        AudioCallback(data, frames, channels, userData);
      },
      this);
  }

  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  return AudioCaptureWrapper::Init(env, exports);
}

NODE_API_MODULE(audio_capture, Init)
