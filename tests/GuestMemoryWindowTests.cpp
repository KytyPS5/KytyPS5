// Tests for the windows in which guest direct memory is imported into the GPU.

#include "graphics/host_gpu/guestMemoryWindows.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <tuple>

namespace {

using Libs::Graphics::GuestMemoryWindowIndex;
using Libs::Graphics::GuestMemoryWindowsCovering;

int g_failures = 0;

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    g_failures++;
  }
}

constexpr uint64_t MiB = 1024 * 1024;
constexpr uint64_t Window = 64 * MiB;
constexpr uint64_t File = 1024 * MiB;

std::optional<uint64_t> Index(uint64_t offset, uint64_t size,
                              uint64_t file = File) {
  return GuestMemoryWindowIndex(offset, size, Window, file);
}

// Any range of up to half a window lies in one window, wherever it starts.
void TestHalfWindowAlwaysFits() {
  for (uint64_t offset = 0; offset + 32 * MiB <= File;
       offset += 3 * MiB + 16384) {
    const auto index = Index(offset, 32 * MiB);
    Check(index.has_value(), "a half-window range found no window");
    if (index) {
      const auto begin = *index * 32 * MiB;
      Check(begin <= offset && offset + 32 * MiB <= begin + Window,
            "the window does not hold the range");
    }
  }
}

// A larger range fits only where it starts early enough in a window.
void TestLargerRanges() {
  Check(Index(0, Window) == 0, "a whole first window");
  Check(Index(32 * MiB, Window) == 1, "a whole second window");
  Check(!Index(16 * MiB, Window).has_value(),
        "a window-sized range across two windows");
  Check(!Index(0, Window + 1).has_value(), "a range larger than a window");
  Check(Index(40 * MiB, 50 * MiB) == 1, "a range in the second window");
}

// The last windows end with the file, and nothing lies past it.
void TestFileEnd() {
  constexpr uint64_t file = 100 * MiB;
  Check(Index(96 * MiB, 4 * MiB, file) == 3, "a range at the end of the file");
  Check(!Index(96 * MiB, 5 * MiB, file).has_value(),
        "a range past the end of the file");
  Check(!Index(file, 1, file).has_value(), "a range starting at the end");
  Check(!Index(0, 0, file).has_value(), "an empty range");
}

// The windows built ahead for a mapping are the ones that any part of it can be
// placed in.
void TestCoveringWindows() {
  using Case = std::tuple<uint64_t, uint64_t, uint64_t>;
  for (const auto &[offset, size, file] :
       {Case{0, 16 * MiB, File}, Case{40 * MiB, 50 * MiB, File},
        Case{33 * MiB, 200 * MiB, File}, Case{96 * MiB, 4 * MiB, 100 * MiB}}) {
    const auto covering =
        GuestMemoryWindowsCovering(offset, size, Window, file);
    Check(covering.has_value(),
          "a mapping in the file has no covering windows");
    if (!covering) {
      continue;
    }
    for (uint64_t part = offset; part < offset + size; part += MiB) {
      for (const uint64_t part_size : {uint64_t{16384}, MiB, 32 * MiB}) {
        const auto index =
            Index(part, std::min(part_size, offset + size - part), file);
        Check(!index ||
                  (covering->first <= *index && *index <= covering->second),
              "a part of a mapping was placed in a window that was not built "
              "ahead");
      }
    }
    Check(covering->first == offset / (Window / 2) &&
              covering->second == (offset + size - 1) / (Window / 2),
          "the covering windows are more than the mapping can use");
  }
  Check(!GuestMemoryWindowsCovering(File, MiB, Window, File).has_value(),
        "a mapping past the end of the file has covering windows");
  Check(!GuestMemoryWindowsCovering(0, 0, Window, File).has_value(),
        "an empty mapping has covering windows");
}

} // namespace

int main() {
  TestCoveringWindows();
  TestHalfWindowAlwaysFits();
  TestLargerRanges();
  TestFileEnd();
  if (g_failures != 0) {
    std::cerr << g_failures << " guest memory window check(s) failed\n";
    return 1;
  }
  std::cout << "guest memory window tests passed\n";
  return 0;
}
