#include "common/bitArray.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>

namespace {

using Bits = Common::BitArray<128>;

static_assert(sizeof(Common::BitArray<1024>) == 128);

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "BitArrayTests: failed: %s\n", message);
    std::abort();
  }
}

void TestPointAndRangeOperations() {
  Bits bits;
  Check(bits.None() && !bits.Any(), "default state is not empty");

  for (const auto index : {size_t{0}, size_t{63}, size_t{64}, size_t{127}}) {
    bits.Set(index);
    Check(bits.Get(index), "Set did not set a boundary bit");
    bits.Unset(index);
    Check(!bits.Get(index), "Unset did not clear a boundary bit");
  }

  bits.SetRange(60, 68);
  for (size_t index = 0; index < 128; index++) {
    Check(bits.Get(index) == (index >= 60 && index < 68),
          "cross-word SetRange changed the wrong bits");
  }
  bits.Fill();
  bits.UnsetRange(60, 68);
  for (size_t index = 0; index < 128; index++) {
    Check(bits.Get(index) == !(index >= 60 && index < 68),
          "cross-word UnsetRange changed the wrong bits");
  }
  bits.Clear();
  bits.SetRange(0, 128);
  Check(!bits.None(), "full SetRange left the array empty");
  bits.UnsetRange(0, 128);
  Check(bits.None(), "full UnsetRange left set bits");

  bits.Set(7);
  bits.SetRange(9, 9);
  bits.SetRange(0, 129);
  bits.UnsetRange(9, 9);
  bits.UnsetRange(0, 129);
  Check(bits.Get(7), "invalid or empty range modified the array");
}

void TestMaskedConstructionAndBitwiseOperations() {
  Bits source;
  source.Fill();
  const Bits masked(source, 31, 97);
  for (size_t index = 0; index < 128; index++) {
    Check(masked.Get(index) == (index >= 31 && index < 97),
          "masked constructor retained a bit outside its range");
  }
  Check(Bits(source, 12, 12).None(), "empty masked constructor produced bits");
  Check(Bits(source, 0, 129).None(),
        "invalid masked constructor produced bits");

  Bits left;
  left.SetRange(0, 80);
  Bits right;
  right.SetRange(40, 120);
  const auto exclusive = left ^ right;
  for (size_t index = 0; index < 128; index++) {
    const bool expected = (index < 80) != (index >= 40 && index < 120);
    Check(exclusive.Get(index) == expected, "XOR produced the wrong bit");
  }
  const auto inverted = ~left;
  for (size_t index = 0; index < 128; index++) {
    Check(inverted.Get(index) == (index >= 80), "NOT produced the wrong bit");
  }
}

void TestRangeDiscoveryAndIteration() {
  Bits bits;
  Check(bits.FirstRange() == Bits::Range{128, 128},
        "empty FirstRange is wrong");
  Check(bits.LastRange() == Bits::Range{0, 0}, "empty LastRange is wrong");

  bits.SetRange(3, 8);
  bits.SetRange(63, 70);
  bits.Set(127);
  Check(bits.FirstRange() == Bits::Range{3, 8}, "FirstRange is wrong");
  Check(bits.FirstRangeFrom(5) == Bits::Range{5, 8},
        "FirstRangeFrom inside a run is wrong");
  Check(bits.FirstRangeFrom(8) == Bits::Range{63, 70},
        "FirstRangeFrom gap is wrong");
  Check(bits.LastRange() == Bits::Range{127, 128}, "LastRange is wrong");
  Check(bits.LastRangeFrom(69) == Bits::Range{63, 69},
        "LastRangeFrom inside a run is wrong");
  Check(bits.LastRangeFrom(63) == Bits::Range{3, 8},
        "LastRangeFrom gap is wrong");

  constexpr std::array expected{Bits::Range{3, 8}, Bits::Range{63, 70},
                                Bits::Range{127, 128}};
  size_t range_index = 0;
  for (const auto range : bits) {
    Check(range_index < expected.size() && range == expected[range_index],
          "range iterator produced the wrong run");
    range_index++;
  }
  Check(range_index == expected.size(), "range iterator omitted a run");
}

void TestRandomizedDifferential() {
  Bits bits;
  std::array<bool, 128> reference{};
  uint64_t random = 0x53a9'7f11'ced4'29b5ull;
  const auto next_random = [&random] {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    return random;
  };

  for (size_t operation = 0; operation < 10000; operation++) {
    const auto first = static_cast<size_t>(next_random() % 128);
    const auto last =
        first + 1 + static_cast<size_t>(next_random() % (128 - first));
    if ((next_random() & 1) != 0) {
      bits.SetRange(first, last);
      for (auto index = first; index < last; index++) {
        reference[index] = true;
      }
    } else {
      bits.UnsetRange(first, last);
      for (auto index = first; index < last; index++) {
        reference[index] = false;
      }
    }

    bool any = false;
    for (size_t index = 0; index < reference.size(); index++) {
      Check(bits.Get(index) == reference[index],
            "randomized bit state diverged");
      any |= reference[index];
    }
    Check(bits.Any() == any && bits.None() == !any,
          "randomized Any/None diverged");

    const auto range_start = static_cast<size_t>(next_random() % 129);
    auto expected_first_begin = range_start;
    while (expected_first_begin < reference.size() &&
           !reference[expected_first_begin]) {
      expected_first_begin++;
    }
    if (expected_first_begin == reference.size()) {
      Check(bits.FirstRangeFrom(range_start) == Bits::Range{128, 128},
            "randomized FirstRangeFrom empty suffix diverged");
    } else {
      auto expected_first_end = expected_first_begin;
      while (expected_first_end < reference.size() &&
             reference[expected_first_end]) {
        expected_first_end++;
      }
      Check(bits.FirstRangeFrom(range_start) ==
                Bits::Range{expected_first_begin, expected_first_end},
            "randomized FirstRangeFrom diverged");
    }

    const auto range_end = static_cast<size_t>(next_random() % 129);
    auto expected_last_end = range_end;
    while (expected_last_end != 0 && !reference[expected_last_end - 1]) {
      expected_last_end--;
    }
    if (expected_last_end == 0) {
      Check(bits.LastRangeFrom(range_end) == Bits::Range{0, 0},
            "randomized LastRangeFrom empty prefix diverged");
    } else {
      auto expected_last_begin = expected_last_end;
      while (expected_last_begin != 0 && reference[expected_last_begin - 1]) {
        expected_last_begin--;
      }
      Check(bits.LastRangeFrom(range_end) ==
                Bits::Range{expected_last_begin, expected_last_end},
            "randomized LastRangeFrom diverged");
    }

    const auto masked_start = static_cast<size_t>(next_random() % 129);
    const auto masked_end =
        masked_start +
        static_cast<size_t>(next_random() % (129 - masked_start));
    const Bits masked(bits, masked_start, masked_end);
    for (size_t index = 0; index < reference.size(); index++) {
      Check(masked.Get(index) == (index >= masked_start && index < masked_end &&
                                  reference[index]),
            "randomized masked constructor diverged");
    }

    size_t first_begin = 0;
    while (first_begin < reference.size() && !reference[first_begin]) {
      first_begin++;
    }
    if (first_begin == reference.size()) {
      Check(bits.FirstRange() == Bits::Range{128, 128},
            "randomized empty FirstRange diverged");
      Check(bits.LastRange() == Bits::Range{0, 0},
            "randomized empty LastRange diverged");
    } else {
      auto first_end = first_begin;
      while (first_end < reference.size() && reference[first_end]) {
        first_end++;
      }
      Check(bits.FirstRange() == Bits::Range{first_begin, first_end},
            "randomized FirstRange diverged");

      auto last_end = reference.size();
      while (!reference[last_end - 1]) {
        last_end--;
      }
      auto last_begin = last_end;
      while (last_begin != 0 && reference[last_begin - 1]) {
        last_begin--;
      }
      Check(bits.LastRange() == Bits::Range{last_begin, last_end},
            "randomized LastRange diverged");
    }

    size_t expected_begin = 0;
    for (const auto [begin, end] : bits) {
      while (expected_begin < reference.size() && !reference[expected_begin]) {
        expected_begin++;
      }
      Check(begin == expected_begin, "randomized iterator run start diverged");
      while (expected_begin < reference.size() && reference[expected_begin]) {
        expected_begin++;
      }
      Check(end == expected_begin, "randomized iterator run end diverged");
    }
    while (expected_begin < reference.size() && !reference[expected_begin]) {
      expected_begin++;
    }
    Check(expected_begin == reference.size(),
          "randomized iterator omitted a run");
  }
}

void TestTrackerSizedRandomizedDifferential() {
  using TrackerBits = Common::BitArray<1024>;
  TrackerBits bits;
  std::array<bool, 1024> reference{};
  uint64_t random = 0x9e37'79b9'7f4a'7c15ull;
  const auto next_random = [&random] {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    return random;
  };

  for (size_t operation = 0; operation < 4096; operation++) {
    const auto first = static_cast<size_t>(next_random() % reference.size());
    const auto last =
        first + 1 +
        static_cast<size_t>(next_random() % (reference.size() - first));
    const bool set = (next_random() & 1) != 0;
    if (set) {
      bits.SetRange(first, last);
    } else {
      bits.UnsetRange(first, last);
    }
    for (auto index = first; index < last; index++) {
      reference[index] = set;
    }

    for (size_t query = 0; query < 8; query++) {
      const auto begin = static_cast<size_t>(next_random() % 1025);
      const auto end = begin + static_cast<size_t>(next_random() % (1025 - begin));
      bool expected_any = false;
      for (auto index = begin; index < end; index++) {
        expected_any |= reference[index];
      }
      Check(bits.AnyInRange(begin, end) == expected_any,
            "tracker-sized randomized AnyInRange diverged");
      if (begin < end) {
        Check(bits.AnyInRange(begin, end) ==
                  (bits.FirstRangeFrom(begin).first < end),
              "bounded query differs from previous tracker query");
      }
      Check(bits.AnyInRange(begin, end) == TrackerBits(bits, begin, end).Any(),
            "bounded query differs from masked conflict check");
    }

    for (size_t index = 0; index < reference.size(); index++) {
      Check(bits.Get(index) == reference[index],
            "tracker-sized randomized bit state diverged");
    }

    size_t expected = 0;
    for (const auto [begin, end] : bits) {
      while (expected < reference.size() && !reference[expected]) {
        expected++;
      }
      Check(begin == expected, "tracker-sized randomized range start diverged");
      while (expected < reference.size() && reference[expected]) {
        expected++;
      }
      Check(end == expected, "tracker-sized randomized range end diverged");
    }
    while (expected < reference.size() && !reference[expected]) {
      expected++;
    }
    Check(expected == reference.size(),
          "tracker-sized randomized iterator omitted a run");
  }
}

template <size_t N>
void TestAnyInRange() {
  using Array = Common::BitArray<N>;
  for (size_t pattern = 0; pattern < 12; pattern++) {
    Array bits;
    std::array<size_t, N + 1> prefix{};
    for (size_t index = 0; index < N; index++) {
      const bool set =
          pattern == 1 || (pattern == 2 && index % 2 == 0) ||
          (pattern == 3 && index >= 31 && index < N - 1) ||
          (pattern == 4 && index == 0) ||
          (pattern == 5 && index == 63) ||
          (pattern == 6 && index == 64) ||
          (pattern == 7 && index == 127) ||
          (pattern == 8 && index == 128) ||
          (pattern == 9 && index == N - 1) ||
          (pattern == 10 && index % 64 == 0) ||
          (pattern == 11 && index % 64 == 63);
      if (set) bits.Set(index);
      prefix[index + 1] = prefix[index] + set;
    }
    for (size_t begin = 0; begin <= N; begin++) {
      for (size_t end = 0; end <= N; end++) {
        const bool expected = begin < end && prefix[end] != prefix[begin];
        Check(bits.AnyInRange(begin, end) == expected,
              "AnyInRange differs from independent prefix-count oracle");
      }
    }
    const auto huge = std::numeric_limits<size_t>::max();
    Check(!bits.AnyInRange(0, N + 1) && !bits.AnyInRange(N + 1, N + 2) &&
              !bits.AnyInRange(0, huge) && !bits.AnyInRange(huge, huge),
          "AnyInRange accepted an invalid range");
  }
}

constexpr bool TestConstexprAnyInRange() {
  Common::BitArray<128> bits;
  bits.Set(63);
  bits.Set(64);
  return bits.AnyInRange(63, 64) && bits.AnyInRange(64, 65) &&
         bits.AnyInRange(0, 128) && !bits.AnyInRange(0, 63) &&
         !bits.AnyInRange(65, 128) && !bits.AnyInRange(64, 64) &&
         !bits.AnyInRange(0, 129);
}
static_assert(TestConstexprAnyInRange());

} // namespace

int main() {
  TestAnyInRange<64>();
  TestAnyInRange<128>();
  TestAnyInRange<1024>();
  TestPointAndRangeOperations();
  TestMaskedConstructionAndBitwiseOperations();
  TestRangeDiscoveryAndIteration();
  TestRandomizedDifferential();
  TestTrackerSizedRandomizedDifferential();
  std::puts("BitArrayTests: all cases passed");
  return 0;
}
