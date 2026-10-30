#include <napi.h>

#include "audio_capture.h"
#include "ring_buffer.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class AudioCaptureWrapper;

namespace {

const char* CodeForStatus(int status) {
  switch (status) {
    case AUDIO_CAPTURE_PERMISSION_DENIED:
      return "permission-denied";
    case AUDIO_CAPTURE_NO_DISPLAY:
      return "no-display";
    case AUDIO_CAPTURE_TIMEOUT:
      return "timeout";
    default:
      return "stream-failed";
  }
}

Napi::Error MakeAudioError(Napi::Env env, int status, const std::string& message) {
  const char* code = CodeForStatus(status);
  std::string text = message.empty() ? code : message;
  Napi::Error error = Napi::Error::New(env, text);
  error.Set("code", Napi::String::New(env, code));
  error.Set("name", Napi::String::New(env, "AudioCaptureError"));
  return error;
}

int ReadIntOption(Napi::Env env, const Napi::Object& options, const char* key, int fallback, int min,
                  int max) {
  if (!options.Has(key) || options.Get(key).IsUndefined() || options.Get(key).IsNull()) {
    return fallback;
  }
  Napi::Value value = options.Get(key);
  if (!value.IsNumber()) {
    throw Napi::TypeError::New(env, std::string(key) + " must be an integer.");
  }
  double number = value.As<Napi::Number>().DoubleValue();
  if (!std::isfinite(number) || std::floor(number) != number) {
    throw Napi::TypeError::New(env, std::string(key) + " must be an integer.");
  }
  int parsed = static_cast<int>(number);
  if (parsed < min || parsed > max) {
    throw Napi::RangeError::New(env, std::string(key) + " is outside the supported range.");
  }
  return parsed;
}

int BatchFramesFor(int sampleRate, int batchMs) {
  int frames = sampleRate * batchMs / 1000;
  return frames < 1 ? 1 : frames;
}

int CapacityFramesFor(int sampleRate, int batchMs) {
  int halfSecond = sampleRate / 2;
  if (halfSecond < 1) halfSecond = 1;
  int twoBatches = BatchFramesFor(sampleRate, batchMs) * 2;
  return halfSecond > twoBatches ? halfSecond : twoBatches;
}

struct DataPayload {
  std::vector<float> samples;
  int channels = 0;
  int frames = 0;
  int sampleRate = 0;
  double timestamp = 0;
  std::shared_ptr<std::atomic<bool>> pending;
};

struct StoppedPayload {
  AudioCaptureWrapper* self = nullptr;
  int status = AUDIO_CAPTURE_STREAM_FAILED;
  std::string message;
};

}  // namespace

class AudioCaptureWrapper : public Napi::ObjectWrap<AudioCaptureWrapper> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports);
  AudioCaptureWrapper(const Napi::CallbackInfo& info);
  ~AudioCaptureWrapper();

  friend class StartTask;
  friend class StopTask;

 private:
  static Napi::FunctionReference constructor;
  static void OnNativeData(const AudioCaptureChunk* chunk, void* userData);
  static void OnNativeStopped(int status, const char* message, void* userData);

  Napi::Value Start(const Napi::CallbackInfo& info);
  Napi::Value Stop(const Napi::CallbackInfo& info);
  Napi::Value On(const Napi::CallbackInfo& info);
  Napi::Value Close(const Napi::CallbackInfo& info);

  void ScheduleBatch(int sampleRate, double timestamp);
  void MarkRunning(Napi::Env env);
  void MarkStopped(Napi::Env env);
  void ReleaseListeners(Napi::Env env);
  Napi::Promise::Deferred QueueStop(Napi::Env env);

  void* handle_ = nullptr;
  std::unique_ptr<AudioRingBuffer> ring_;
  std::mutex callbackMu_;
  std::mutex opMutex_;
  std::shared_ptr<std::atomic<bool>> pending_;
  Napi::ThreadSafeFunction dataTsfn_;
  Napi::ThreadSafeFunction stoppedTsfn_;
  Napi::FunctionReference stoppedFn_;
  bool hasData_ = false;
  bool dataHeld_ = false;
  bool hasStopped_ = false;
  bool stoppedHeld_ = false;
  std::atomic<bool> acceptAudio_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> running_{false};
  std::atomic<bool> sessionEnded_{false};
  std::atomic<bool> published_{false};
  int sampleRate_ = 48000;
  int channels_ = 2;
  int batchMs_ = 100;
  int batchFrames_ = 4800;
  int capacityFrames_ = 24000;
  int observedRate_ = 48000;
  double latestTimestamp_ = 0;
};

class StartTask : public Napi::AsyncWorker {
 public:
  StartTask(Napi::Env env, AudioCaptureWrapper* self, Napi::Promise::Deferred deferred)
      : Napi::AsyncWorker(env, "AudioCapture.start"), self_(self), deferred_(deferred) {}

  void Execute() override {
    std::lock_guard<std::mutex> lock(self_->opMutex_);
    self_->acceptAudio_.store(false, std::memory_order_release);
    if (self_->handle_) AudioCapture_stop(self_->handle_);
    if (self_->ring_) self_->ring_->reset();
    if (self_->closed_.load()) {
      status_ = AUDIO_CAPTURE_STREAM_FAILED;
      message_ = "AudioCapture is closed.";
      return;
    }
    self_->sessionEnded_.store(false);
    self_->published_.store(false);
    self_->observedRate_ = self_->sampleRate_;
    self_->batchFrames_ = BatchFramesFor(self_->sampleRate_, self_->batchMs_);
    self_->latestTimestamp_ = 0;
    self_->pending_->store(false);
    self_->acceptAudio_.store(true, std::memory_order_release);

    char message[512];
    message[0] = '\0';
    status_ = self_->handle_ ? AudioCapture_start(self_->handle_, message, sizeof(message))
                             : AUDIO_CAPTURE_STREAM_FAILED;
    message_ = message;
    if (self_->closed_.load() || self_->sessionEnded_.load() || status_ != AUDIO_CAPTURE_OK ||
        !AudioCapture_isCapturing(self_->handle_)) {
      self_->acceptAudio_.store(false, std::memory_order_release);
      if (self_->handle_) AudioCapture_stop(self_->handle_);
      if (status_ == AUDIO_CAPTURE_OK) {
        status_ = AUDIO_CAPTURE_STREAM_FAILED;
        if (message_.empty()) {
          message_ = self_->closed_.load() ? "AudioCapture is closed."
                                           : "Capture stopped before it was ready.";
        }
      }
    }
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    if (status_ == AUDIO_CAPTURE_OK && !self_->sessionEnded_.load() && !self_->closed_.load()) {
      self_->published_.store(true);
      self_->MarkRunning(Env());
      deferred_.Resolve(Env().Undefined());
    } else {
      self_->published_.store(false);
      if (status_ == AUDIO_CAPTURE_OK) {
        status_ = AUDIO_CAPTURE_STREAM_FAILED;
        if (message_.empty()) {
          message_ = self_->closed_.load() ? "AudioCapture is closed."
                                           : "Capture stopped before it was ready.";
        }
      }
      self_->MarkStopped(Env());
      deferred_.Reject(MakeAudioError(Env(), status_, message_).Value());
    }
    self_->Unref();
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    self_->acceptAudio_.store(false, std::memory_order_release);
    self_->MarkStopped(Env());
    deferred_.Reject(error.Value());
    self_->Unref();
  }

 private:
  AudioCaptureWrapper* self_;
  Napi::Promise::Deferred deferred_;
  int status_ = AUDIO_CAPTURE_STREAM_FAILED;
  std::string message_;
};

class StopTask : public Napi::AsyncWorker {
 public:
  StopTask(Napi::Env env, AudioCaptureWrapper* self, Napi::Promise::Deferred deferred)
      : Napi::AsyncWorker(env, "AudioCapture.stop"), self_(self), deferred_(deferred) {}

  void Execute() override {
    std::lock_guard<std::mutex> lock(self_->opMutex_);
    self_->acceptAudio_.store(false, std::memory_order_release);
    if (self_->handle_) AudioCapture_stop(self_->handle_);
    if (self_->ring_) self_->ring_->reset();
    self_->pending_->store(false);
  }

  void OnOK() override {
    Napi::HandleScope scope(Env());
    self_->MarkStopped(Env());
    deferred_.Resolve(Env().Undefined());
    self_->Unref();
  }

  void OnError(const Napi::Error& error) override {
    Napi::HandleScope scope(Env());
    self_->MarkStopped(Env());
    deferred_.Reject(error.Value());
    self_->Unref();
  }

 private:
  AudioCaptureWrapper* self_;
  Napi::Promise::Deferred deferred_;
};

Napi::FunctionReference AudioCaptureWrapper::constructor;

Napi::Object AudioCaptureWrapper::Init(Napi::Env env, Napi::Object exports) {
  Napi::Function func = DefineClass(env, "AudioCapture", {
    InstanceMethod("start", &AudioCaptureWrapper::Start),
    InstanceMethod("stop", &AudioCaptureWrapper::Stop),
    InstanceMethod("on", &AudioCaptureWrapper::On),
    InstanceMethod("close", &AudioCaptureWrapper::Close),
  });
  constructor = Napi::Persistent(func);
  constructor.SuppressDestruct();
  exports.Set("AudioCapture", func);
  return exports;
}

AudioCaptureWrapper::AudioCaptureWrapper(const Napi::CallbackInfo& info)
    : Napi::ObjectWrap<AudioCaptureWrapper>(info),
      pending_(std::make_shared<std::atomic<bool>>(false)) {
  Napi::Env env = info.Env();
  bool excludes = true;
  if (info.Length() >= 1 && !info[0].IsUndefined() && !info[0].IsNull()) {
    if (!info[0].IsObject()) {
      throw Napi::TypeError::New(env, "Options must be an object.");
    }
    Napi::Object options = info[0].As<Napi::Object>();
    sampleRate_ = ReadIntOption(env, options, "sampleRate", 48000, 8000, 96000);
    channels_ = ReadIntOption(env, options, "channels", 2, 1, 2);
    batchMs_ = ReadIntOption(env, options, "batchMs", 100, 20, 500);
    if (options.Has("excludesCurrentProcessAudio") &&
        !options.Get("excludesCurrentProcessAudio").IsUndefined()) {
      Napi::Value flag = options.Get("excludesCurrentProcessAudio");
      if (!flag.IsBoolean()) {
        throw Napi::TypeError::New(env, "excludesCurrentProcessAudio must be a boolean.");
      }
      excludes = flag.As<Napi::Boolean>().Value();
    }
  }

  batchFrames_ = BatchFramesFor(sampleRate_, batchMs_);
  capacityFrames_ = CapacityFramesFor(sampleRate_, batchMs_);
  observedRate_ = sampleRate_;
  ring_ = std::make_unique<AudioRingBuffer>(static_cast<size_t>(capacityFrames_),
                                            static_cast<size_t>(channels_));
  handle_ = AudioCapture_create(sampleRate_, channels_, excludes ? 1 : 0);
  if (!handle_) {
    throw Napi::Error::New(env, "Failed to create the audio capture.");
  }
  AudioCapture_setCallbacks(handle_, &AudioCaptureWrapper::OnNativeData,
                            &AudioCaptureWrapper::OnNativeStopped, this);

  Napi::Function noop = Napi::Function::New(env, [](const Napi::CallbackInfo& cbInfo) {
    return cbInfo.Env().Undefined();
  });
  stoppedTsfn_ = Napi::ThreadSafeFunction::New(env, noop, "AudioCaptureStopped", 4, 1);
  stoppedTsfn_.Unref(env);
  hasStopped_ = true;
}

AudioCaptureWrapper::~AudioCaptureWrapper() {
  closed_.store(true);
  acceptAudio_.store(false, std::memory_order_release);
  if (handle_) {
    AudioCapture_stop(handle_);
    AudioCapture_destroy(handle_);
    handle_ = nullptr;
  }
  std::lock_guard<std::mutex> lock(callbackMu_);
  if (hasData_) {
    dataTsfn_.Release();
    hasData_ = false;
  }
  if (hasStopped_) {
    stoppedTsfn_.Release();
    hasStopped_ = false;
  }
  if (!stoppedFn_.IsEmpty()) stoppedFn_.Reset();
}

void AudioCaptureWrapper::OnNativeData(const AudioCaptureChunk* chunk, void* userData) {
  auto* self = static_cast<AudioCaptureWrapper*>(userData);
  if (!self || !chunk || !chunk->samples) return;
  if (!self->acceptAudio_.load(std::memory_order_acquire) || self->closed_.load()) return;
  if (chunk->frames <= 0 || chunk->channels <= 0 || chunk->channels > 8 || chunk->sampleRate <= 0) {
    return;
  }

  if (self->ring_->channels() != static_cast<size_t>(chunk->channels)) {
    self->ring_->reconfigure(static_cast<size_t>(self->capacityFrames_),
                             static_cast<size_t>(chunk->channels));
  }
  if (chunk->sampleRate != self->observedRate_) {
    self->observedRate_ = chunk->sampleRate;
    self->batchFrames_ = BatchFramesFor(self->observedRate_, self->batchMs_);
  }
  self->latestTimestamp_ = chunk->timestamp;
  self->ring_->push(chunk->samples, static_cast<size_t>(chunk->frames));
  self->ScheduleBatch(chunk->sampleRate, self->latestTimestamp_);
}

void AudioCaptureWrapper::OnNativeStopped(int status, const char* message, void* userData) {
  auto* self = static_cast<AudioCaptureWrapper*>(userData);
  if (!self) return;
  self->sessionEnded_.store(true);
  self->acceptAudio_.store(false, std::memory_order_release);
  if (self->closed_.load()) return;

  auto* payload = new StoppedPayload();
  payload->self = self;
  payload->status = status == AUDIO_CAPTURE_OK ? AUDIO_CAPTURE_STREAM_FAILED : status;
  payload->message = message ? message : "The system stopped audio capture.";

  std::lock_guard<std::mutex> lock(self->callbackMu_);
  if (!self->hasStopped_ || self->closed_.load()) {
    delete payload;
    return;
  }
  napi_status call = self->stoppedTsfn_.NonBlockingCall(
      payload, [](Napi::Env env, Napi::Function /*noop*/, StoppedPayload* payload) {
        Napi::HandleScope scope(env);
        struct Guard {
          StoppedPayload* payload;
          ~Guard() { delete payload; }
        } guard{payload};

        AudioCaptureWrapper* self = payload->self;
        if (!self || self->closed_.load() || !self->published_.load()) return;
        int status = payload->status;
        std::string message = payload->message;
        self->published_.store(false);
        self->MarkStopped(env);
        if (self->stoppedFn_.IsEmpty()) return;
        self->stoppedFn_.Call({MakeAudioError(env, status, message).Value()});
      });
  if (call != napi_ok) delete payload;
}

void AudioCaptureWrapper::ScheduleBatch(int sampleRate, double timestamp) {
  if (ring_->availableFrames() < static_cast<size_t>(batchFrames_)) return;
  bool expected = false;
  if (!pending_->compare_exchange_strong(expected, true)) return;

  size_t channels = ring_->channels();
  std::vector<float> samples(static_cast<size_t>(batchFrames_) * channels);
  size_t frames = ring_->popLatest(samples.data(), static_cast<size_t>(batchFrames_));
  if (frames == 0) {
    pending_->store(false);
    return;
  }
  samples.resize(frames * channels);

  auto* payload = new DataPayload();
  payload->samples = std::move(samples);
  payload->channels = static_cast<int>(channels);
  payload->frames = static_cast<int>(frames);
  payload->sampleRate = sampleRate;
  payload->timestamp = timestamp;
  payload->pending = pending_;

  napi_status status = napi_generic_failure;
  {
    std::lock_guard<std::mutex> lock(callbackMu_);
    if (!hasData_ || closed_.load()) {
      pending_->store(false);
      delete payload;
      return;
    }
    status = dataTsfn_.NonBlockingCall(
        payload, [](Napi::Env env, Napi::Function jsCallback, DataPayload* payload) {
          struct Guard {
            std::shared_ptr<std::atomic<bool>> pending;
            DataPayload* payload;
            ~Guard() {
              if (pending) pending->store(false);
              delete payload;
            }
          } guard{payload->pending, payload};

          Napi::HandleScope scope(env);
          size_t count = payload->samples.size();
          Napi::ArrayBuffer buffer = Napi::ArrayBuffer::New(env, count * sizeof(float));
          if (count > 0) {
            std::memcpy(buffer.Data(), payload->samples.data(), count * sizeof(float));
          }
          Napi::Object chunk = Napi::Object::New(env);
          chunk.Set("samples", Napi::Float32Array::New(env, count, buffer, 0));
          chunk.Set("channels", Napi::Number::New(env, payload->channels));
          chunk.Set("sampleRate", Napi::Number::New(env, payload->sampleRate));
          chunk.Set("frames", Napi::Number::New(env, payload->frames));
          chunk.Set("timestamp", Napi::Number::New(env, payload->timestamp));
          jsCallback.Call({chunk});
        });
  }
  if (status != napi_ok) {
    pending_->store(false);
    delete payload;
  }
}

void AudioCaptureWrapper::MarkRunning(Napi::Env env) {
  if (closed_.load()) return;
  if (running_.exchange(true)) return;
  {
    std::lock_guard<std::mutex> lock(callbackMu_);
    if (hasData_ && !dataHeld_) {
      dataTsfn_.Ref(env);
      dataHeld_ = true;
    }
    if (hasStopped_ && !stoppedHeld_) {
      stoppedTsfn_.Ref(env);
      stoppedHeld_ = true;
    }
  }
  Ref();
}

void AudioCaptureWrapper::MarkStopped(Napi::Env env) {
  if (!running_.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lock(callbackMu_);
    if (dataHeld_) {
      dataTsfn_.Unref(env);
      dataHeld_ = false;
    }
    if (stoppedHeld_) {
      stoppedTsfn_.Unref(env);
      stoppedHeld_ = false;
    }
  }
  Unref();
}

void AudioCaptureWrapper::ReleaseListeners(Napi::Env env) {
  std::lock_guard<std::mutex> lock(callbackMu_);
  if (dataHeld_) {
    dataTsfn_.Unref(env);
    dataHeld_ = false;
  }
  if (hasData_) {
    dataTsfn_.Release();
    hasData_ = false;
  }
  if (stoppedHeld_) {
    stoppedTsfn_.Unref(env);
    stoppedHeld_ = false;
  }
  if (hasStopped_) {
    stoppedTsfn_.Release();
    hasStopped_ = false;
  }
  if (!stoppedFn_.IsEmpty()) stoppedFn_.Reset();
}

Napi::Promise::Deferred AudioCaptureWrapper::QueueStop(Napi::Env env) {
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  Ref();
  StopTask* task = new StopTask(env, this, deferred);
  task->Queue();
  return deferred;
}

Napi::Value AudioCaptureWrapper::Start(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Promise::Deferred deferred = Napi::Promise::Deferred::New(env);
  if (closed_.load()) {
    deferred.Reject(MakeAudioError(env, AUDIO_CAPTURE_STREAM_FAILED, "AudioCapture is closed.").Value());
    return deferred.Promise();
  }
  Ref();
  StartTask* task = new StartTask(env, this, deferred);
  task->Queue();
  return deferred.Promise();
}

Napi::Value AudioCaptureWrapper::Stop(const Napi::CallbackInfo& info) {
  return QueueStop(info.Env()).Promise();
}

Napi::Value AudioCaptureWrapper::On(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (closed_.load()) {
    throw Napi::Error::New(env, "AudioCapture is closed.");
  }
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
    throw Napi::TypeError::New(env, "Expected an event name and a function.");
  }
  std::string event = info[0].As<Napi::String>().Utf8Value();
  Napi::Function callback = info[1].As<Napi::Function>();

  if (event == "stopped") {
    stoppedFn_.Reset(callback, 1);
    return env.Undefined();
  }
  if (event != "data") {
    throw Napi::TypeError::New(env, "Unknown event. Use \"data\" or \"stopped\".");
  }

  std::lock_guard<std::mutex> lock(callbackMu_);
  if (hasData_) {
    if (dataHeld_) {
      dataTsfn_.Unref(env);
      dataHeld_ = false;
    }
    dataTsfn_.Release();
    hasData_ = false;
  }
  dataTsfn_ = Napi::ThreadSafeFunction::New(env, callback, "AudioCaptureData", 2, 1);
  dataTsfn_.Unref(env);
  hasData_ = true;
  if (running_.load() && !dataHeld_) {
    dataTsfn_.Ref(env);
    dataHeld_ = true;
  }
  return env.Undefined();
}

Napi::Value AudioCaptureWrapper::Close(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  bool already = closed_.exchange(true);
  acceptAudio_.store(false, std::memory_order_release);
  published_.store(false);
  if (!already) {
    ReleaseListeners(env);
    if (running_.exchange(false)) {
      Unref();
    }
    QueueStop(env);
  }
  return env.Undefined();
}

Napi::Object Init(Napi::Env env, Napi::Object exports) {
  return AudioCaptureWrapper::Init(env, exports);
}

NODE_API_MODULE(audio_capture, Init)
