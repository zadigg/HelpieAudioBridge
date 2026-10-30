#include "ring_buffer.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

static void expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    std::exit(1);
  }
}

int main() {
  AudioRingBuffer ring(4, 1);
  float first[] = {1, 2, 3};
  expect(ring.push(first, 3) == 3, "push 3");
  float second[] = {4, 5, 6};
  expect(ring.push(second, 3) == 3, "push overflow");
  expect(ring.availableFrames() == 4, "full after overflow");
  float out[4];
  expect(ring.popLatest(out, 4) == 4, "pop full");
  expect(out[0] == 3 && out[1] == 4 && out[2] == 5 && out[3] == 6, "kept newest");

  float batch[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  ring.reset();
  expect(ring.push(batch, 10) == 4, "push larger than capacity keeps tail");
  expect(ring.popLatest(out, 4) == 4, "pop tail");
  expect(out[0] == 7 && out[3] == 10, "tail is the newest frames");

  AudioRingBuffer live(10, 1);
  expect(live.push(batch, 10) == 10, "push 10");
  float latest[3];
  expect(live.popLatest(latest, 3) == 3, "pop latest 3");
  expect(latest[0] == 8 && latest[1] == 9 && latest[2] == 10, "dropped older frames");
  expect(live.availableFrames() == 0, "buffer drained");

  AudioRingBuffer stereo(4, 2);
  float interleaved[] = {1, 10, 2, 20, 3, 30, 4, 40, 5, 50};
  expect(stereo.push(interleaved, 5) == 4, "stereo capacity");
  float stereoOut[8];
  expect(stereo.popLatest(stereoOut, 4) == 4, "pop stereo");
  expect(stereoOut[0] == 2 && stereoOut[1] == 20 && stereoOut[6] == 5 && stereoOut[7] == 50,
         "stereo frames stay aligned");

  AudioRingBuffer concurrent(1000, 1);
  std::atomic<bool> done{false};
  std::thread producer([&concurrent, &done]() {
    for (int i = 0; i < 1000; i++) {
      float sample = static_cast<float>(i);
      concurrent.push(&sample, 1);
    }
    done.store(true);
  });
  std::thread consumer([&concurrent, &done]() {
    float buffer[32];
    while (!done.load() || concurrent.availableFrames() > 0) {
      concurrent.popLatest(buffer, 32);
    }
  });
  producer.join();
  consumer.join();
  concurrent.reset();
  expect(concurrent.availableFrames() == 0, "reset clears");

  std::puts("ring buffer ok");
  return 0;
}
