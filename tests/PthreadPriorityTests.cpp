#include "common/common.h"

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#ifndef _TIMESPEC_DEFINED
#define _TIMESPEC_DEFINED
#endif
#include <pthread.h>

namespace {

using namespace Libs::LibKernel;

void Expect(int actual, int expected, const char *text) {
  if (actual != expected) {
    std::fprintf(stderr, "PthreadPriorityTests: %s: got %d, expected %d\n",
                 text, actual, expected);
    std::abort();
  }
}

struct Expected {
  int guest, host, kernel;
};

struct Worker {
  Pthread thread = nullptr;
  std::atomic<int> request{0}, complete{0};
  int guest = 0, host = 0, kernel = 0;
  alignas(16) unsigned char stack[1024 * 1024];
};

void *KYTY_SYSV_ABI RunWorker(void *arg) {
  auto &worker = *static_cast<Worker *>(arg);
  int seen = 0;
  for (;;) {
    int ticket;
    while ((ticket = worker.request.load()) == seen) {
      Sleep(1);
    }
    if (ticket < 0) {
      return nullptr;
    }
    const auto self = PthreadSelf();
    worker.host = GetThreadPriority(GetCurrentThread());
    worker.kernel = PthreadGetPriorityForKernel(self);
    Expect(PthreadGetprio(self, &worker.guest), OK, "PthreadGetprio");
    seen = ticket;
    worker.complete.store(ticket);
  }
}

void Sample(Worker &worker, Expected expected) {
  const int ticket = worker.request.load() + 1;
  worker.request.store(ticket);
  const auto deadline = GetTickCount64() + 30000;
  while (worker.complete.load() != ticket) {
    Expect(GetTickCount64() < deadline, 1,
           "sample completed within 30 seconds");
    Sleep(1);
  }
  Expect(worker.host, expected.host, "Windows priority");
  Expect(worker.kernel, expected.kernel, "kernel priority class");
  Expect(worker.guest, expected.guest, "exact guest priority");
}

void TestPriorities() {
  constexpr Expected initial[] = {{460, +2, 256}, {461, 0, 256},
                                  {478, 0, 256},  {479, 0, 700},
                                  {732, 0, 700},  {733, -2, 767}};
  constexpr Expected changed[] = {
      {700, 0, 700}, {400, +2, 256}, {470, 0, 256}, {800, -2, 767}};
  static Worker workers[6];

  for (int i = 0; i < 6; ++i) {
    auto &worker = workers[i];
    PthreadAttr attr = nullptr;
    KernelSchedParam param{};
    param.sched_priority = initial[i].guest;
    Expect(PthreadAttrInit(&attr), OK, "PthreadAttrInit");
    Expect(PthreadAttrSetstack(&attr, worker.stack, sizeof(worker.stack)), OK,
           "PthreadAttrSetstack");
    Expect(PthreadAttrSetschedparam(&attr, &param), OK,
           "PthreadAttrSetschedparam");
    Expect(PthreadCreate(&worker.thread, &attr, RunWorker, &worker,
                         "PriorityTest"),
           OK, "PthreadCreate");
    Expect(PthreadAttrDestroy(&attr), OK, "PthreadAttrDestroy");
  }

  for (int i = 0; i < 6; ++i) {
    Sample(workers[i], initial[i]);
  }
  for (const auto expected : changed) {
    Expect(PthreadSetprio(workers[0].thread, expected.guest), OK,
           "PthreadSetprio");
    Sample(workers[0], expected);
  }
  for (auto &worker : workers) {
    worker.request.store(-1);
  }
  for (auto &worker : workers) {
    Expect(PthreadJoin(worker.thread, nullptr), OK, "PthreadJoin");
  }
}

}
#endif

int main() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();
  Common::Singleton<Loader::RuntimeLinker>::Instance();
  Libs::LibKernel::Initialize();
  Libs::LibKernel::PthreadInitSelfForMainThread();
  TestPriorities();
  subsystems.Destroy();
  std::printf("PthreadPriorityTests: all cases passed\n");
#endif
  return 0;
}
