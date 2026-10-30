#include "pcm.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>

static void expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL %s\n", message);
    std::exit(1);
  }
}

int main() {
  float interleaved[] = {1, 2, 3, 4, 5};
  float dest[6];
  expect(CopyInterleavedFloat32(interleaved, 5 * sizeof(float), 2, 2, dest), "copy");
  expect(dest[0] == 1 && dest[1] == 2 && dest[2] == 3 && dest[3] == 4, "interleaved values");

  float shortDest[4];
  expect(CopyInterleavedFloat32(interleaved, 3 * sizeof(float), 2, 2, shortDest), "partial frame");
  expect(shortDest[0] == 1 && shortDest[1] == 2 && shortDest[2] == 0 && shortDest[3] == 0,
         "partial frame is zero filled");
  expect(!CopyInterleavedFloat32(interleaved, sizeof(float), 2, 2, shortDest),
         "less than one frame fails");

  float left[] = {1, 3, 5};
  float right[] = {2, 4};
  FloatPlane planes[2];
  planes[0] = {left, sizeof(left)};
  planes[1] = {right, sizeof(right)};
  float mixed[6];
  expect(InterleaveFloat32(planes, 2, 3, 2, mixed), "interleave");
  expect(mixed[0] == 1 && mixed[1] == 2 && mixed[2] == 3 && mixed[3] == 4, "plane values");
  expect(mixed[4] == 5 && mixed[5] == 0, "missing plane sample is zero");

  float oneChannel[6];
  expect(InterleaveFloat32(planes, 1, 3, 2, oneChannel), "single plane");
  expect(oneChannel[0] == 1 && oneChannel[1] == 0 && oneChannel[2] == 3 && oneChannel[3] == 0,
         "absent channel is zero");

  std::puts("pcm ok");
  return 0;
}
