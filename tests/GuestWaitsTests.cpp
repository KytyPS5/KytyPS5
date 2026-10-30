#include "common/guestWaits.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace {

using Common::GuestWaits::Format;
using Common::GuestWaits::Kind;
using Common::GuestWaits::NowNs;
using Common::GuestWaits::ParseDumpSeconds;
using Common::GuestWaits::Scope;
using Common::GuestWaits::SetCurrentThreadName;
using Common::GuestWaits::SetEnabled;
using Common::GuestWaits::Snapshot;
using Common::GuestWaits::ThreadState;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "GuestWaitsTests: failed: %s\n", text);
    std::abort();
  }
}

const ThreadState *FindThread(const std::vector<ThreadState> &states,
                              const std::string &name) {
  for (const auto &state : states) {
    if (state.name == name) {
      return &state;
    }
  }
  return nullptr;
}

// Runs body on a named thread, holding it until release is fulfilled.
struct Worker {
  std::promise<void> ready;
  std::promise<void> release;
  std::thread thread;

  template <typename Body> Worker(const char *name, Body body) {
    auto ready_future = ready.get_future();
    auto release_future = release.get_future().share();
    thread = std::thread([this, name, body, release_future] {
      SetCurrentThreadName(name);
      body([&] {
        ready.set_value();
        release_future.wait();
      });
    });
    ready_future.wait();
  }

  void Finish() {
    release.set_value();
    thread.join();
  }
};

void TestDisabledRecordsNothing() {
  SetEnabled(false);
  std::thread([] {
    SetCurrentThreadName("disabled");
    const Scope scope(Kind::Mutex, nullptr, "m", 1);
    Check(Snapshot(NowNs()).empty(), "a disabled tracker registers no thread");
  }).join();
  Check(Snapshot(NowNs()).empty(), "a disabled tracker keeps no state");
}

void TestBlockedWaitIsReported() {
  int object = 0;
  Worker worker("loader", [&](auto block) {
    const Scope scope(Kind::Cond, &object, "load_done", 0x1234);
    block();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  auto states = Snapshot(NowNs());
  const auto *state = FindThread(states, "loader");
  Check(state != nullptr, "a waiting thread appears in the snapshot");
  Check(state->kind == Kind::Cond, "the wait kind is recorded");
  Check(state->object == reinterpret_cast<uint64_t>(&object),
        "the wait object is recorded");
  Check(state->object_name == "load_done", "the object name is recorded");
  Check(state->caller == 0x1234, "the guest caller is recorded");
  Check(state->waiting_ns >= 10'000'000, "the wait duration grows");
  Check(state->completed_waits == 0, "an unfinished wait is not counted");

  const auto text = Format(states, 10'000'000);
  Check(text.find("loader") != std::string::npos, "dump names the thread");
  Check(text.find("BLOCKED cond 'load_done'") != std::string::npos,
        "dump marks a long wait as blocked");
  Check(text.find("caller=0x0000000000001234") != std::string::npos,
        "dump shows the caller");

  worker.Finish();
}

void TestFinishedWaitIsKeptAsLast() {
  Worker worker("poller", [&](auto block) {
    for (int i = 0; i < 3; i++) {
      const Scope scope(Kind::Sleep, nullptr, nullptr, 0x5678);
    }
    block();
  });

  auto states = Snapshot(NowNs());
  const auto *state = FindThread(states, "poller");
  Check(state != nullptr, "a running thread stays in the snapshot");
  Check(state->kind == Kind::None, "a thread outside waits is running");
  Check(state->completed_waits == 3, "finished waits are counted");
  Check(state->last_kind == Kind::Sleep, "the last wait kind is kept");
  Check(state->last_caller == 0x5678, "the last wait caller is kept");

  const auto text = Format(states, 1'000'000'000);
  Check(text.find("poller [") != std::string::npos &&
            text.find("running; completed=3 last=sleep") != std::string::npos,
        "dump shows a running thread with its last wait");

  worker.Finish();
}

void TestOutermostWaitWins() {
  Worker worker("nested", [&](auto block) {
    const Scope outer(Kind::Cond, nullptr, "outer", 0xaaaa);
    {
      const Scope inner(Kind::Mutex, nullptr, "inner", 0xbbbb);
    }
    block();
  });

  auto states = Snapshot(NowNs());
  const auto *state = FindThread(states, "nested");
  Check(state != nullptr, "nested thread is present");
  Check(state->kind == Kind::Cond && state->caller == 0xaaaa,
        "an inner wait does not replace the outer one");
  Check(state->completed_waits == 0,
        "an inner wait skipped while nested is not counted");

  worker.Finish();
}

void TestLongObjectNameIsTruncated() {
  const std::string long_name(200, 'x');
  Worker worker("long_name", [&](auto block) {
    const Scope scope(Kind::Equeue, nullptr, long_name.c_str(), 1);
    block();
  });

  auto states = Snapshot(NowNs());
  const auto *state = FindThread(states, "long_name");
  Check(state != nullptr, "long-name thread is present");
  Check(state->object_name == std::string(63, 'x'),
        "object names are truncated to the slot size");

  worker.Finish();
}

void TestExitedThreadIsRemoved() {
  std::thread([] {
    SetCurrentThreadName("short_lived");
    const Scope scope(Kind::Join, nullptr, nullptr, 1);
  }).join();
  Check(FindThread(Snapshot(NowNs()), "short_lived") == nullptr,
        "an exited thread leaves the snapshot");
}

void TestParseDumpSeconds() {
  Check(ParseDumpSeconds(nullptr) == 0, "unset disables the dump");
  Check(ParseDumpSeconds("") == 0, "empty disables the dump");
  Check(ParseDumpSeconds("30") == 30, "a number of seconds is accepted");
  Check(ParseDumpSeconds("0") == 0, "zero disables the dump");
  Check(ParseDumpSeconds("abc") == 0, "text is rejected");
  Check(ParseDumpSeconds("5s") == 0, "trailing text is rejected");
  Check(ParseDumpSeconds("-5") == 0, "negative values are rejected");
  Check(ParseDumpSeconds("3601") == 0, "intervals over an hour are rejected");
}

} // namespace

int main() {
  TestDisabledRecordsNothing();
  TestParseDumpSeconds();

  SetEnabled(true);
  TestBlockedWaitIsReported();
  TestFinishedWaitIsKeptAsLast();
  TestOutermostWaitWins();
  TestLongObjectNameIsTruncated();
  TestExitedThreadIsRemoved();

  std::printf("GuestWaitsTests: all tests passed\n");
  return 0;
}
