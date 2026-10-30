#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <vector>

// Interleaved float audio. push, pop, and reset are safe to call from
// different threads. Capacity is a fixed number of frames.
class AudioRingBuffer {
 public:
  AudioRingBuffer(size_t capacityFrames, size_t channels) {
    reconfigure(capacityFrames, channels);
  }

  void reconfigure(size_t capacityFrames, size_t channels) {
    std::lock_guard<std::mutex> lock(mu_);
    channels_ = channels == 0 ? 1 : channels;
    if (capacityFrames == 0) capacityFrames = 1;
    capacity_ = capacityFrames * channels_;
    buffer_.assign(capacity_, 0.0f);
    head_ = 0;
    tail_ = 0;
    size_ = 0;
  }

  size_t push(const float* data, size_t frames) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!data || frames == 0 || channels_ == 0 || capacity_ == 0) return 0;
    size_t samples = frames * channels_;
    if (samples >= capacity_) {
      size_t keep = capacity_ - (capacity_ % channels_);
      if (keep == 0) return 0;
      data += samples - keep;
      samples = keep;
      frames = keep / channels_;
      head_ = 0;
      tail_ = 0;
      size_ = 0;
    } else if (size_ + samples > capacity_) {
      DropOldestUnlocked(size_ + samples - capacity_);
    }
    size_t tail = tail_;
    size_t first = std::min(samples, capacity_ - tail);
    std::memcpy(buffer_.data() + tail, data, first * sizeof(float));
    if (first < samples) {
      std::memcpy(buffer_.data(), data + first, (samples - first) * sizeof(float));
    }
    tail_ = (tail + samples) % capacity_;
    size_ += samples;
    return frames;
  }

  // Drops older frames until only maxFrames remain, then copies those.
  size_t popLatest(float* out, size_t maxFrames) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!out || maxFrames == 0 || channels_ == 0) return 0;
    size_t available = size_ / channels_;
    if (available == 0) return 0;
    if (available > maxFrames) {
      DropOldestUnlocked((available - maxFrames) * channels_);
    }
    return PopUnlocked(out, maxFrames);
  }

  size_t availableFrames() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (channels_ == 0) return 0;
    return size_ / channels_;
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mu_);
    head_ = 0;
    tail_ = 0;
    size_ = 0;
  }

  size_t channels() const {
    std::lock_guard<std::mutex> lock(mu_);
    return channels_;
  }

 private:
  void DropOldestUnlocked(size_t samples) {
    if (samples > size_) samples = size_;
    samples -= samples % channels_;
    head_ = (head_ + samples) % capacity_;
    size_ -= samples;
  }

  size_t PopUnlocked(float* out, size_t frames) {
    size_t available = size_ / channels_;
    size_t n = std::min(frames, available);
    if (n == 0) return 0;
    size_t samples = n * channels_;
    size_t head = head_;
    size_t first = std::min(samples, capacity_ - head);
    std::memcpy(out, buffer_.data() + head, first * sizeof(float));
    if (first < samples) {
      std::memcpy(out + first, buffer_.data(), (samples - first) * sizeof(float));
    }
    head_ = (head + samples) % capacity_;
    size_ -= samples;
    return n;
  }

  mutable std::mutex mu_;
  size_t capacity_ = 0;
  size_t channels_ = 0;
  std::vector<float> buffer_;
  size_t head_ = 0;
  size_t tail_ = 0;
  size_t size_ = 0;
};
