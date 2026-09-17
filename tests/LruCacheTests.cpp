#include "common/lruCache.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

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
  // (a=40, b=20, c=35 after the touches above).
  const auto below = Collect(cache, 35);
  Check(below.size() == 2 && below[0] == "b" && below[1] == "c",
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

} // namespace

int main() {
  TestInsertAndVisitOrder();
  TestEarlyExitCallback();
  TestTouchReordersAndSkips();
  TestFreeAndIdReuse();
  std::puts("LruCacheTests: all cases passed");
  return 0;
}
