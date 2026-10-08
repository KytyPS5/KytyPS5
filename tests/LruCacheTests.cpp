#include "common/lruCache.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using OwnershipCache = Common::LeastRecentlyUsedCache<std::string, uint64_t>;
static_assert(!std::is_copy_constructible_v<OwnershipCache>);
static_assert(!std::is_copy_assignable_v<OwnershipCache>);
static_assert(std::is_move_constructible_v<OwnershipCache>);
static_assert(std::is_nothrow_move_assignable_v<OwnershipCache>);

/// Keep regression checks active in release builds where assert may be disabled.
void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "LruCacheTests: failed: %s\n", message);
    std::abort();
  }
}

// Collect the objects reachable through ForEachItemBelow(), in visit order.
std::vector<std::string> Collect(Common::LeastRecentlyUsedCache<std::string, uint64_t> &cache,
                                 uint64_t tick) {
  std::vector<std::string> visited;
  cache.ForEachItemBelow(tick, [&](const std::string &object) { visited.push_back(object); });
  return visited;
}

void TestInsertAndVisitOrder() {
  Common::LeastRecentlyUsedCache<std::string, uint64_t> cache;
  const auto a = cache.Insert("a", 10);
  const auto b = cache.Insert("b", 20);
  const auto c = cache.Insert("c", 30);

  Check(a != b && b != c && a != c, "Insert returned duplicate ids");

  const auto all = Collect(cache, UINT64_MAX);
  Check(all.size() == 3 && all[0] == "a" && all[1] == "b" && all[2] == "c",
        "inserted items are not visited in insertion order");

  const auto below = Collect(cache, 20);
  Check(below.size() == 2 && below[0] == "a" && below[1] == "b",
        "ForEachItemBelow visited an item above the tick");

  const auto none = Collect(cache, 5);
  Check(none.empty(), "ForEachItemBelow visited items below an empty tick");
}

void TestEarlyExitCallback() {
  Common::LeastRecentlyUsedCache<std::string, uint64_t> cache;
  (void)cache.Insert("a", 10);
  (void)cache.Insert("b", 20);
  (void)cache.Insert("c", 30);

  std::vector<std::string> visited;
  cache.ForEachItemBelow(UINT64_MAX, [&](const std::string &object) {
    visited.push_back(object);
    return true; // stop after the first item
  });
  Check(visited.size() == 1 && visited[0] == "a", "boolean callback did not stop the walk");

  std::vector<std::string> all_visited;
  cache.ForEachItemBelow(UINT64_MAX, [&](const std::string &object) {
    all_visited.push_back(object);
    return false;
  });
  Check(all_visited.size() == 3, "false-returning callback did not visit every item");
}

void TestTouchReordersAndSkips() {
  Common::LeastRecentlyUsedCache<std::string, uint64_t> cache;
  const auto a = cache.Insert("a", 10);
  const auto b = cache.Insert("b", 20);
  const auto c = cache.Insert("c", 30);

  // Touching the last item must not change the order.
  cache.Touch(c, 35);
  auto order = Collect(cache, UINT64_MAX);
  Check(order.size() == 3 && order[0] == "a" && order[1] == "b" && order[2] == "c",
        "touching the newest item changed the order");

  // An older or equal tick must be ignored.
  cache.Touch(a, 10);
  cache.Touch(a, 5);
  order = Collect(cache, UINT64_MAX);
  Check(order[0] == "a" && order[1] == "b" && order[2] == "c",
        "an ignored touch changed the order");

  // Touching the oldest item moves it to the newest end.
  cache.Touch(a, 40);
  order = Collect(cache, UINT64_MAX);
  Check(order.size() == 3 && order[0] == "b" && order[1] == "c" && order[2] == "a",
        "touching an item did not move it to the newest end");

  // The touched item now carries its new tick for tick filtering
  // (a=40, b=20, c=35 after the touches above). A cutoff of 30 detects
  // a stale c tick: c is visited only if Touch(c, 35) updated it.
  const auto below = Collect(cache, 30);
  Check(below.size() == 1 && below[0] == "b",
        "touched item did not adopt its new tick");

  // Touch the middle item.
  cache.Touch(b, 45);
  order = Collect(cache, UINT64_MAX);
  Check(order.size() == 3 && order[0] == "c" && order[1] == "a" && order[2] == "b",
        "touching a middle item did not move it to the end");
}

void TestFreeAndIdReuse() {
  Common::LeastRecentlyUsedCache<std::string, uint64_t> cache;
  const auto a = cache.Insert("a", 10);
  const auto b = cache.Insert("b", 20);
  const auto c = cache.Insert("c", 30);

  // Free the first and the last items.
  cache.Free(a);
  cache.Free(c);
  auto order = Collect(cache, UINT64_MAX);
  Check(order.size() == 1 && order[0] == "b", "Free left a freed item in the walk");

  // Freed ids must be reused by later inserts.
  const auto d = cache.Insert("d", 40);
  Check(d == a || d == c, "Insert did not reuse a freed id");
  const auto e = cache.Insert("e", 50);
  Check(e != d && (e == a || e == c), "Insert reused the same freed id twice");

  order = Collect(cache, UINT64_MAX);
  Check(order.size() == 3 && order[0] == "b" && order[1] == "d" && order[2] == "e",
        "reused ids are not visited in tick order");

  // Touching a reused id must not disturb the other items.
  cache.Touch(d, 60);
  order = Collect(cache, UINT64_MAX);
  Check(order.size() == 3 && order[0] == "b" && order[1] == "e" && order[2] == "d",
        "touching a reused id changed the wrong items");

  // Free everything, then rebuild.
  cache.Free(b);
  cache.Free(d);
  cache.Free(e);
  Check(Collect(cache, UINT64_MAX).empty(), "freeing every item left items in the walk");

  const auto again = cache.Insert("again", 70);
  const auto order2 = Collect(cache, UINT64_MAX);
  Check(order2.size() == 1 && order2[0] == "again", "insert after a full free failed");
  cache.Touch(again, 70);
  Check(Collect(cache, 70).size() == 1, "touching a rebuilt item failed");
}

/// Moving a populated cache must preserve IDs and leave an independent empty source.
void TestMoveConstruction() {
  using Cache = Common::LeastRecentlyUsedCache<std::string, uint64_t>;
  Cache source;
  const auto a = source.Insert("a", 10);
  const auto hole = source.Insert("hole", 20);
  const auto b = source.Insert("b", 30);
  source.Free(hole);
  Cache destination(std::move(source));
  Check(Collect(destination, UINT64_MAX) == std::vector<std::string>{"a", "b"},
        "move construction lost live entries");
  Check(Collect(source, UINT64_MAX).empty(), "moved-from cache still visits transferred entries");
  (void)source.Insert("source", 5);
  Check(Collect(source, UINT64_MAX) == std::vector<std::string>{"source"},
        "moved-from cache cannot be reused independently");
  Check(destination.Insert("reused", 40) == hole, "move lost reusable IDs");
  destination.Touch(a, 50);
  destination.Free(b);
  Check(Collect(destination, UINT64_MAX) == std::vector<std::string>{"reused", "a"},
        "transferred IDs do not address the destination entries");
  Check(Collect(source, UINT64_MAX) == std::vector<std::string>{"source"},
        "destination mutation changed the source");
}

/// Move assignment replaces old contents and survives source destruction and self-move.
void TestMoveAssignment() {
  using Cache = Common::LeastRecentlyUsedCache<std::string, uint64_t>;
  Cache destination;
  (void)destination.Insert("discarded", 1);
  size_t live = 0;
  size_t hole = 0;
  {
    Cache source;
    live = source.Insert("live", 10);
    hole = source.Insert("hole", 20);
    source.Free(hole);
    destination = std::move(source);
    Check(Collect(source, UINT64_MAX).empty(), "move assignment retained source links");
    (void)source.Insert("independent", 1);
  }
  Check(Collect(destination, UINT64_MAX) == std::vector<std::string>{"live"},
        "source destruction invalidated transferred contents");
  Check(destination.Insert("reused", 20) == hole, "move assignment lost free IDs");
  auto* self = &destination;
  destination = std::move(*self);
  destination.Touch(live, 30);
  Check(Collect(destination, UINT64_MAX) == std::vector<std::string>{"reused", "live"},
        "self-move corrupted the cache");
  destination.Free(live);
  destination.Free(hole);
  Check(Collect(destination, UINT64_MAX).empty(), "transferred IDs cannot be freed");
}

/// Destroying the destination must not leave dangling links in the moved-from cache.
void TestDestinationLifetime() {
  Common::LeastRecentlyUsedCache<std::string, uint64_t> source;
  (void)source.Insert("transferred", 10);
  {
    auto destination = std::move(source);
    Check(Collect(destination, UINT64_MAX).size() == 1, "move lost the entry");
  }
  Check(Collect(source, UINT64_MAX).empty(), "source links outlived destination storage");
  (void)source.Insert("fresh", 20);
  Check(Collect(source, UINT64_MAX) == std::vector<std::string>{"fresh"},
        "source reuse depends on destination lifetime");
}

/// Empty and fully freed caches remain reusable after moving in either direction.
void TestMoveEmptyCaches() {
  using Cache = Common::LeastRecentlyUsedCache<std::string, uint64_t>;
  Cache source;
  Cache destination(std::move(source));
  const auto id = source.Insert("freed", 1);
  source.Free(id);
  destination = std::move(source);
  Check(destination.Insert("reused", 2) == id, "moving an empty cache lost free slots");
  Cache empty;
  destination = std::move(empty);
  Check(Collect(destination, UINT64_MAX).empty(), "empty move did not replace old entries");
  (void)empty.Insert("source", 1);
  (void)destination.Insert("destination", 1);
  Check(Collect(empty, UINT64_MAX) == std::vector<std::string>{"source"},
        "empty source is not reusable");
  Check(Collect(destination, UINT64_MAX) == std::vector<std::string>{"destination"},
        "empty destination is not reusable");
}

} // namespace

int main() {
  TestMoveConstruction();
  TestMoveAssignment();
  TestMoveEmptyCaches();
  TestDestinationLifetime();
  TestInsertAndVisitOrder();
  TestEarlyExitCallback();
  TestTouchReordersAndSkips();
  TestFreeAndIdReuse();
  std::puts("LruCacheTests: all cases passed");
  return 0;
}
