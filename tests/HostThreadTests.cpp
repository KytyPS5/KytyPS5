#include "common/threads.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "HostThreadTests: failed: %s\n", message);
    std::abort();
  }
}

struct StackProbe {
  const volatile char *base = nullptr;
  size_t target = 0;
  size_t used = 0;
  uint32_t depth = 0;
};

// Uses stack until `target` bytes lie between the thread's first frame and this one. Each frame's
// address escapes into `probe`, so the frames stay distinct; a loop would never reach `target`.
[[gnu::noinline]] uint32_t UseStack(StackProbe &probe, uint32_t depth) {
  volatile char frame[4096];
  frame[0] = static_cast<char>(depth);
  frame[sizeof(frame) - 1] = 0;
  probe.used = reinterpret_cast<uintptr_t>(probe.base) - reinterpret_cast<uintptr_t>(&frame[0]);
  probe.depth = depth;
  if (probe.used >= probe.target) {
    return depth;
  }
  return UseStack(probe, depth + 1) + static_cast<uint32_t>(frame[sizeof(frame) - 1]);
}

void UseStackThread(void *data) {
  auto *probe = static_cast<StackProbe *>(data);
  volatile char base = 0;
  probe->base = &base;
  UseStack(*probe, 0);
}

void TestThreadWithStackHasItsStack() {
  // 32 MiB is past every host's default thread stack (1 MiB on Windows, 512 KiB for secondary
  // threads on macOS, usually 8 MiB on Linux), so this only passes with the requested stack.
  StackProbe probe{.target = size_t{32} << 20u};
  Common::ThreadWithStack thread(UseStackThread, &probe, size_t{64} << 20u);
  thread.Join();
  Check(probe.used >= probe.target && probe.depth > 1000u,
        "thread did not get the requested stack");
}

void TestThreadWithStackJoinsOnce() {
  StackProbe probe{.target = 0};
  Common::ThreadWithStack thread(UseStackThread, &probe, size_t{1} << 20u);
  thread.Join();
  thread.Join();
  Check(probe.base != nullptr, "thread function did not run");
}

} // namespace

int main() {
  TestThreadWithStackHasItsStack();
  TestThreadWithStackJoinsOnce();
  std::printf("HostThreadTests: ok\n");
  return 0;
}
