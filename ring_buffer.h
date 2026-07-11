#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

// SPSC lock-free ring buffer for float audio, optimized for single producer (SCStream thread)
// and single consumer (JS callback thread / timer). No locks, cache-friendly.
class AudioRingBuffer {
public:
  explicit AudioRingBuffer(size_t capacityFrames, size_t channels)
    : capacity_(capacityFrames * channels),
      channels_(channels),
      buffer_(capacity_),
      head_(0),
      tail_(0),
      size_(0) {}

  // Producer: push interleaved float samples, returns number of frames actually written (drops oldest if full)
  size_t push(const float* data, size_t frames) {
    size_t samples = frames * channels_;
    if (samples == 0) return 0;
    if (samples > capacity_) {
      // If chunk larger than capacity, keep only tail
      data += samples - capacity_;
      samples = capacity_;
      frames = capacity_ / channels_;
    }
    size_t currentSize = size_.load(std::memory_order_relaxed);
    // If would overflow, drop oldest
    if (currentSize + samples > capacity_) {
      size_t overflow = currentSize + samples - capacity_;
      // Advance head by overflow (rounded to frame boundary)
      size_t overflowFrames = (overflow + channels_ - 1) / channels_;
      head_.store((head_.load(std::memory_order_relaxed) + overflowFrames * channels_) % capacity_, std::memory_order_release);
      size_.store(capacity_ - samples, std::memory_order_release);
    }
    size_t tail = tail_.load(std::memory_order_relaxed);
    size_t firstChunk = std::min(samples, capacity_ - tail);
    std::memcpy(buffer_.data() + tail, data, firstChunk * sizeof(float));
    if (firstChunk < samples) {
      std::memcpy(buffer_.data(), data + firstChunk, (samples - firstChunk) * sizeof(float));
    }
    tail_.store((tail + samples) % capacity_, std::memory_order_release);
    size_.fetch_add(samples, std::memory_order_release);
    return frames;
  }

  // Consumer: pop up to maxFrames, returns frames popped
  size_t pop(float* out, size_t maxFrames) {
    size_t availableSamples = size_.load(std::memory_order_acquire);
    size_t availableFrames = availableSamples / channels_;
    size_t toReadFrames = std::min(maxFrames, availableFrames);
    if (toReadFrames == 0) return 0;
    size_t toReadSamples = toReadFrames * channels_;
    size_t head = head_.load(std::memory_order_relaxed);
    size_t firstChunk = std::min(toReadSamples, capacity_ - head);
    std::memcpy(out, buffer_.data() + head, firstChunk * sizeof(float));
    if (firstChunk < toReadSamples) {
      std::memcpy(out + firstChunk, buffer_.data(), (toReadSamples - firstChunk) * sizeof(float));
    }
    head_.store((head + toReadSamples) % capacity_, std::memory_order_release);
    size_.fetch_sub(toReadSamples, std::memory_order_release);
    return toReadFrames;
  }

  size_t availableFrames() const {
    return size_.load(std::memory_order_acquire) / channels_;
  }

  void reset() {
    head_.store(0, std::memory_order_release);
    tail_.store(0, std::memory_order_release);
    size_.store(0, std::memory_order_release);
  }

  size_t channels() const { return channels_; }

private:
  size_t capacity_;
  size_t channels_;
  std::vector<float> buffer_;
  std::atomic<size_t> head_;
  std::atomic<size_t> tail_;
  std::atomic<size_t> size_;
};
