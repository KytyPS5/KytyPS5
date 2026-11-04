#include "common/debugCounters.h"
#include "common/gpuStall.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {

using Common::GpuStall::Clock;
using Common::GpuStall::StallTracker;
using std::chrono::milliseconds;

constexpr milliseconds kThreshold {250};
// How long a waiter thread may take to start and park before the test gives up.
constexpr std::chrono::seconds kParkLimit {10};
// Guards against a missed signal leaving the waiter parked until some later event. Real wakeups
// take a few milliseconds, and the bound is loose so a busy test machine does not fail it.
constexpr milliseconds kReleaseLimit {500};
// A threshold no test reaches, for checking that a short Scope never holds.
constexpr std::chrono::hours kNeverReached {1};

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "GpuStallTests: failed: %s\n", message);
    std::abort();
  }
}

void TestTrackerNothingActive() {
  StallTracker tracker;
  Check(!tracker.IsStalled(Clock::time_point {} + kNeverReached, kThreshold),
        "an empty tracker reported a stall");
}

void TestTrackerThreshold() {
  StallTracker tracker;
  const auto start = Clock::time_point {} + milliseconds(1000);
  tracker.Begin(start);
  Check(!tracker.IsStalled(start + kThreshold - milliseconds(1), kThreshold),
        "a stall held before reaching the threshold");
  Check(tracker.IsStalled(start + kThreshold, kThreshold), "a stall at the threshold did not hold");
  Check(tracker.IsStalled(start + kThreshold * 4, kThreshold), "a long stall did not hold");
  // A stall that ends exactly at the threshold releases at once.
  tracker.End(start);
  Check(!tracker.IsStalled(start + kThreshold, kThreshold), "an ended stall still held");
}

// The oldest stall decides, so a newer short one does not hold once the old one ends.
void TestTrackerOverlapping() {
  StallTracker tracker;
  const auto old_start = Clock::time_point {} + milliseconds(1000);
  const auto new_start = old_start + milliseconds(200);
  tracker.Begin(old_start);
  tracker.Begin(new_start);
  Check(tracker.IsStalled(old_start + kThreshold, kThreshold),
        "the oldest overlapping stall did not hold");
  tracker.End(old_start);
  Check(!tracker.IsStalled(old_start + kThreshold, kThreshold),
        "a newer stall under the threshold held");
  Check(tracker.IsStalled(new_start + kThreshold, kThreshold),
        "the remaining stall did not hold at its own threshold");
  tracker.End(new_start);
}

// Short compiles one after another restart the clock and never add up to a hold.
void TestTrackerBackToBack() {
  StallTracker tracker;
  auto start = Clock::time_point {} + milliseconds(1000);
  for (int compile = 0; compile < 10; ++compile) {
    tracker.Begin(start);
    const auto end = start + kThreshold - milliseconds(1);
    Check(!tracker.IsStalled(end, kThreshold), "back-to-back short stalls held");
    tracker.End(start);
    start = end;
  }
}

void TestDisabledNeverWaits() {
  Common::GpuStall::Configure(false, milliseconds(0));
  Common::GpuStall::Scope scope;
  Check(!Common::GpuStall::IsStalled(), "a disabled module reported a stall");
  Check(!Common::GpuStall::WaitWhileStalled(), "a disabled module held");
}

void TestShortScopeNeverWaits() {
  Common::GpuStall::Configure(true, kNeverReached);
  Common::GpuStall::Scope scope;
  bool held = true;
  std::thread waiter([&] { held = Common::GpuStall::WaitWhileStalled(); });
  waiter.join();
  Check(!held, "a Scope under the threshold held");
  Common::GpuStall::Configure(false);
}

// The GPU thread would deadlock if it waited for its own Scope to end.
void TestScopeOwnerNeverWaits() {
  Common::GpuStall::Configure(true, milliseconds(0));
  Common::GpuStall::Scope scope;
  Check(Common::GpuStall::IsStalled(), "a Scope at the threshold was not reported");
  Check(!Common::GpuStall::WaitWhileStalled(), "the thread owning a Scope waited on itself");
  Common::GpuStall::Configure(false);
}

// Turning holding on while a compile is already running holds the next push once it is past
// the threshold, which is zero here.
void TestEnableWithActiveScope() {
  Common::GpuStall::Configure(false, milliseconds(0));
  Common::GpuStall::Scope scope;
  Check(!Common::GpuStall::IsStalled(), "a disabled module reported a stall");
  Common::GpuStall::Configure(true, milliseconds(0));
  Check(Common::GpuStall::IsStalled(), "enabling with an old Scope did not stall");
  Common::GpuStall::Configure(false);
}

// Starts a waiter during a stall, lets release() end it, and checks it waited and woke promptly.
template <typename Release>
void CheckWaiterReleased(Release release, const char *what) {
  Common::GpuStall::Configure(true, milliseconds(0));
  auto scope = std::make_unique<Common::GpuStall::Scope>();
  std::atomic<bool> done {false};
  bool held = false;
  Clock::time_point woke;
  std::thread waiter([&] {
    held = Common::GpuStall::WaitWhileStalled();
    woke = Clock::now();
    done = true;
  });
  // Waits until the waiter is parked, so the release below cannot race ahead of it.
  const auto park_deadline = Clock::now() + kParkLimit;
  while (Common::GpuStall::WaiterCount() == 0 && !done && Clock::now() < park_deadline)
    std::this_thread::yield();
  Check(!done && Common::GpuStall::WaiterCount() == 1, what);
  const auto released = Clock::now();
  release(scope);
  waiter.join();
  Check(held, "the waiter did not report that it held");
  Check(woke - released < kReleaseLimit, "the waiter was released too slowly");
  Common::GpuStall::Configure(false);
}

void TestScopeEndReleasesWaiter() {
  CheckWaiterReleased([](auto &scope) { scope.reset(); }, "a waiter did not hold during a stall");
}

void TestDisableReleasesWaiter() {
  CheckWaiterReleased([](auto &) { Common::GpuStall::Configure(false); },
                      "a waiter did not hold before the module was disabled");
}

// Only the call that waited reports resuming from a hold. The next call on the same thread, with
// no stall, clears it again.
void TestResumedFromHold() {
  Check(!Common::GpuStall::ResumedFromHold(), "a thread with no call that waited reported a hold");
  Common::GpuStall::Configure(true, milliseconds(0));
  auto scope = std::make_unique<Common::GpuStall::Scope>();
  std::atomic<bool> held_once {false};
  bool resumed = false;
  bool cleared = false;
  std::thread waiter([&] {
    Common::GpuStall::WaitWhileStalled();
    resumed = Common::GpuStall::ResumedFromHold();
    held_once = true;
    Common::GpuStall::WaitWhileStalled(); // The Scope has ended, so this does not wait.
    cleared = !Common::GpuStall::ResumedFromHold();
  });
  const auto park_deadline = Clock::now() + kParkLimit;
  while (Common::GpuStall::WaiterCount() == 0 && !held_once && Clock::now() < park_deadline)
    std::this_thread::yield();
  Check(!held_once && Common::GpuStall::WaiterCount() == 1, "the waiter did not hold");
  scope.reset();
  waiter.join();
  Common::GpuStall::Configure(false);
  Check(resumed, "a call that waited did not report resuming from a hold");
  Check(cleared, "a call with no stall reported resuming from a hold");
}

// Starts the given number of waiter threads during a stall and waits until all are parked.
std::vector<std::thread> StartParkedWaiters(uint32_t count) {
  std::vector<std::thread> waiters;
  for (uint32_t waiter = 0; waiter < count; ++waiter)
    waiters.emplace_back([] { Common::GpuStall::WaitWhileStalled(); });
  const auto park_deadline = Clock::now() + kParkLimit;
  while (Common::GpuStall::WaiterCount() < count && Clock::now() < park_deadline)
    std::this_thread::yield();
  Check(Common::GpuStall::WaiterCount() == count, "waiters did not park during the stall");
  return waiters;
}

// Holds are counted once per stall, however many audio threads waited, and only when one did.
// Overlapping Scopes form a single stall.
void TestHoldCounters() {
  using Common::DebugCounters::Counter;
  const auto holds = [] { return Common::DebugCounters::Get(Counter::AudioHolds); };
  Common::DebugCounters::SetEnabled(true);
  Common::GpuStall::Configure(true, milliseconds(0));
  const auto before = holds();
  const auto held_ns_before = Common::DebugCounters::Get(Counter::AudioHoldNs);
  {
    auto scope = std::make_unique<Common::GpuStall::Scope>();
    auto waiters = StartParkedWaiters(2);
    scope.reset();
    for (auto &waiter : waiters) waiter.join();
  }
  Check(holds() == before + 1, "one stall holding two threads was not counted once");
  Check(Common::DebugCounters::Get(Counter::AudioHoldNs) > held_ns_before,
        "a counted hold added no held time");
  { Common::GpuStall::Scope scope; }
  Check(holds() == before + 1, "a stall that held nothing was counted");
  {
    auto outer = std::make_unique<Common::GpuStall::Scope>();
    auto inner = std::make_unique<Common::GpuStall::Scope>();
    auto waiters = StartParkedWaiters(1);
    inner.reset();
    Check(holds() == before + 1, "a stall was counted before its last Scope ended");
    outer.reset();
    for (auto &waiter : waiters) waiter.join();
  }
  Check(holds() == before + 2, "overlapping Scopes were not counted as one stall");
  Common::GpuStall::Configure(false);
  Common::DebugCounters::SetEnabled(false);
}

} // namespace

int main() {
  TestTrackerNothingActive();
  TestTrackerThreshold();
  TestTrackerOverlapping();
  TestTrackerBackToBack();
  TestDisabledNeverWaits();
  TestShortScopeNeverWaits();
  TestScopeOwnerNeverWaits();
  TestEnableWithActiveScope();
  TestScopeEndReleasesWaiter();
  TestDisableReleasesWaiter();
  TestResumedFromHold();
  TestHoldCounters();
  std::puts("GpuStallTests: all cases passed");
  return 0;
}
