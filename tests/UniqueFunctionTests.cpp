#include "common/uniqueFunction.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

namespace {

/// Stop the suite with a diagnostic when an expectation fails.
void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "UniqueFunctionTests: %s\n", message);
    std::abort();
  }
}

/// Verify that void wrappers invoke callbacks and discard non-void results.
void TestDiscardedResult() {
  int calls = 0;
  Common::UniqueFunction<void, int> callback([&](int value) {
    calls += value;
    return std::make_unique<int>(value);
  });
  callback(3);
  Check(calls == 3, "void wrapper did not invoke its value-returning callback");
}

/// Verify void callbacks still accept and consume move-only arguments.
void TestVoidCallback() {
  int value = 0;
  Common::UniqueFunction<void, std::unique_ptr<int>> callback(
      [&](std::unique_ptr<int> argument) { value = *argument; });
  callback(std::make_unique<int>(7));
  Check(value == 7, "void callback did not receive its move-only argument");
}

/// Verify result conversion and mutable move-only captures survive wrapper moves.
void TestReturnedResultAndMove() {
  Common::UniqueFunction<long, int> callback(
      [value = std::make_unique<int>(4)](int increment) mutable {
        *value += increment;
        return *value;
      });
  auto moved = std::move(callback);
  Check(!callback && static_cast<bool>(moved), "move did not transfer the callable");
  Check(moved(2) == 6 && moved(3) == 9, "non-void result or captured state was lost");
}

/// Verify non-void wrappers preserve references rather than copying their results.
void TestReferenceResult() {
  int value = 1;
  Common::UniqueFunction<int&> callback([&]() -> int& { return value; });
  callback() = 5;
  Check(value == 5, "reference result was not preserved");
}

} // namespace

/// Run the focused UniqueFunction regression cases.
int main() {
  TestDiscardedResult();
  TestVoidCallback();
  TestReturnedResultAndMove();
  TestReferenceResult();
  std::puts("UniqueFunctionTests: all cases passed");
  return 0;
}
