#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Plane of packed float32 samples. bytes is the readable size of data.
struct FloatPlane {
  const float* data;
  size_t bytes;
};

inline bool CopyInterleavedFloat32(const void* src, size_t srcBytes, size_t frames,
                                    size_t channels, float* dest) {
  if (!src || !dest || frames == 0 || channels == 0) return false;
  if (frames > SIZE_MAX / channels) return false;
  const size_t samples = frames * channels;
  const size_t need = samples * sizeof(float);
  size_t copyBytes = srcBytes < need ? srcBytes : need;
  size_t fullSamples = copyBytes / sizeof(float);
  fullSamples -= fullSamples % channels;
  if (fullSamples == 0) return false;
  std::memcpy(dest, src, fullSamples * sizeof(float));
  if (fullSamples < samples) {
    std::memset(dest + fullSamples, 0, (samples - fullSamples) * sizeof(float));
  }
  return true;
}

// Missing or short planes are zero-filled so a partial layout cannot leak garbage.
inline bool InterleaveFloat32(const FloatPlane* planes, size_t planeCount, size_t frames,
                               size_t channels, float* dest) {
  if (!dest || frames == 0 || channels == 0) return false;
  if (frames > SIZE_MAX / channels) return false;
  const size_t samples = frames * channels;
  std::memset(dest, 0, samples * sizeof(float));
  size_t count = planeCount < channels ? planeCount : channels;
  for (size_t channel = 0; channel < count; channel++) {
    const float* src = planes ? planes[channel].data : nullptr;
    if (!src) continue;
    size_t available = planes[channel].bytes / sizeof(float);
    size_t n = available < frames ? available : frames;
    for (size_t frame = 0; frame < n; frame++) {
      dest[frame * channels + channel] = src[frame];
    }
  }
  return true;
}
