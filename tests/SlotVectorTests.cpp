#include "common/slotVector.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "SlotVectorTests: failed: %s\n", message);
    std::abort();
  }
}

// Non-movable like the cached Image/Buffer; counts live objects.
struct Probe {
  Probe(int *live, uint32_t value) : live(live), value(value) { ++*live; }
  ~Probe() { --*live; }
  KYTY_CLASS_NO_COPY(Probe);

  int *live;
  uint32_t value;
};

static_assert(sizeof(Common::SlotId) == sizeof(uint32_t));

void TestInsertEraseReuse() {
  int live = 0;
  Common::SlotVector<Probe> slots;
  Check(!slots.is_allocated(Common::SlotId {}) && slots.try_get(Common::SlotId {}) == nullptr &&
            !slots.is_allocated(Common::SlotId {0}),
        "an empty vector reported an allocated slot");

  const auto a = slots.insert(&live, 0u);
  const auto b = slots.insert(&live, 1u);
  const auto c = slots.insert(&live, 2u);
  Check(a.index == 0 && b.index == 1 && c.index == 2 && live == 3,
        "fresh slots were not handed out in order");
  Check(slots.is_allocated(b) && slots.try_get(b) == &slots[b] && slots[b].value == 1,
        "an inserted slot did not expose its object");
  Check(!slots.is_allocated(Common::SlotId {3}), "an unused slot reported an object");

  slots.erase(b);
  slots.erase(a);
  Check(live == 1 && !slots.is_allocated(a) && !slots.is_allocated(b) &&
            slots.try_get(b) == nullptr && slots.is_allocated(c),
        "erase did not destroy and free exactly its slot");

  // Freed indices are reused most recently freed first, with the same plain id.
  const auto reused_a = slots.insert(&live, 0u);
  const auto reused_b = slots.insert(&live, 1u);
  const auto d = slots.insert(&live, 3u);
  Check(reused_a == a && reused_b == b && d.index == 3 && live == 4,
        "freed slots were not reused in LIFO order");
  Check(slots[reused_a].value == 0 && slots[reused_b].value == 1,
        "a reused slot did not expose its new object");
}

void TestForEachAndStability() {
  int live = 0;
  {
    Common::SlotVector<Probe> slots;
    const auto first = slots.insert(&live, 0u);
    Probe *const first_address = &slots[first];

    // Cross several chunks; earlier objects must keep their address.
    std::vector<Common::SlotId> ids {first};
    for (uint32_t i = 1; i < 1000; ++i) {
      ids.push_back(slots.insert(&live, i));
    }
    Check(&slots[first] == first_address && live == 1000,
          "growing the vector moved a stored object");

    std::vector<uint32_t> expected;
    for (uint32_t i = 0; i < 1000; ++i) {
      if (i % 3 == 0) {
        slots.erase(ids[i]);
      } else {
        expected.push_back(i);
      }
    }
    std::vector<uint32_t> visited;
    std::as_const(slots).ForEach([&](Common::SlotId id, const Probe &probe) {
      Check(probe.value == id.index, "ForEach passed an id that does not match its object");
      visited.push_back(id.index);
    });
    Check(visited == expected && live == static_cast<int>(expected.size()),
          "ForEach did not visit exactly the live slots in index order");
  }
  Check(live == 0, "destroying the vector did not destroy every live object exactly once");
}

} // namespace

int main() {
  TestInsertEraseReuse();
  TestForEachAndStability();
  std::puts("SlotVectorTests: all cases passed");
  return 0;
}
