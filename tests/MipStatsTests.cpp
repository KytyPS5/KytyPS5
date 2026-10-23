#include "graphics/guest_gpu/mipStats.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

namespace MipStats = Libs::Graphics::MipStats;

constexpr uint64_t NotSampled = uint64_t {0xf} << 56u;
constexpr uint32_t BlockSize  = MipStats::kHeaderSize + MipStats::kCounterCount * sizeof(uint64_t);

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "MipStatsTests: failed: %s\n", message);
    std::abort();
  }
}

// A GET_LOD_STATS report of `size` bytes, written into a larger buffer to catch overruns.
std::vector<uint8_t> Report(uint32_t size) {
  std::vector<uint8_t> block(size + 16, 0xcd);
  MipStats::Report(block.data(), size);
  for (uint32_t i = size; i < block.size(); i++) {
    Check(block[i] == 0xcd, "report wrote past its size");
  }
  block.resize(size);
  return block;
}

uint32_t Header(const std::vector<uint8_t> &block) {
  uint32_t value = 0;
  std::memcpy(&value, block.data(), sizeof(value));
  return value;
}

uint64_t Word(const std::vector<uint8_t> &block, uint32_t counter) {
  uint64_t word = 0;
  std::memcpy(&word, block.data() + MipStats::kHeaderSize + counter * sizeof(uint64_t),
              sizeof(word));
  return word;
}

bool IsSampledAtMip0(uint64_t word) {
  return ((word >> 56u) & 0xfu) == 0 && (word & 0xffffffu) != 0;
}

void TestReportsBoundCounters() {
  MipStats::Reset();
  MipStats::NoteSampled(0);
  MipStats::NoteSampled(77);
  MipStats::NoteSampled(77);
  MipStats::NoteSampled(255);
  const auto block = Report(BlockSize);
  Check(Header(block) == 1, "header dword is not 1");
  for (uint32_t i = sizeof(uint32_t); i < MipStats::kHeaderSize; i++) {
    Check(block[i] == 0, "rest of the header is not zero");
  }
  for (uint32_t counter = 0; counter < MipStats::kCounterCount; counter++) {
    const bool bound = counter == 0 || counter == 77 || counter == 255;
    if (bound) {
      Check(IsSampledAtMip0(Word(block, counter)), "bound counter not reported as sampled at mip 0");
    } else {
      Check(Word(block, counter) == NotSampled, "unbound counter not reported as not sampled");
    }
  }
}

void TestOnlyResetClearsCounters() {
  MipStats::Reset();
  MipStats::NoteSampled(5);
  (void)Report(BlockSize);
  Check(IsSampledAtMip0(Word(Report(BlockSize), 5)), "a report without reset cleared a counter");
  MipStats::Reset();
  Check(Word(Report(BlockSize), 5) == NotSampled, "reset kept a counter");
}

void TestShortBuffers() {
  MipStats::Reset();
  MipStats::NoteSampled(1);
  Check(Header(Report(MipStats::kHeaderSize)) == 1, "header-only report has no header");
  const auto two = Report(MipStats::kHeaderSize + 2 * sizeof(uint64_t));
  Check(Word(two, 0) == NotSampled && IsSampledAtMip0(Word(two, 1)),
        "short report has wrong counters");
  // Counters beyond the 256 the hardware has stay zero.
  const auto large = Report(BlockSize + 4 * sizeof(uint64_t));
  for (uint32_t counter = MipStats::kCounterCount; counter < MipStats::kCounterCount + 4; counter++) {
    Check(Word(large, counter) == 0, "report filled counters past the last one");
  }
}

} // namespace

int main() {
#ifdef _WIN32
  _putenv_s("KYTY_LOD_STATS", "1");
#else
  setenv("KYTY_LOD_STATS", "1", 1);
#endif
  Check(MipStats::Enabled(), "KYTY_LOD_STATS=1 does not enable the counters");
  TestReportsBoundCounters();
  TestOnlyResetClearsCounters();
  TestShortBuffers();
  std::printf("MipStatsTests: all tests passed\n");
  return 0;
}
