#ifndef KYTY_TESTS_SRTWALKERTESTMEMORY_H_
#define KYTY_TESTS_SRTWALKERTESTMEMORY_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <xbyak/xbyak.h>

#include <csignal>
#include <cstdlib>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <ucontext.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

// Serves SRT walker loads from fake guest addresses: a walker load that faults completes with
// the test reader's DWORD, or with zero like the emulator when the reader fails or is null.
class FakeGuestMemory {
public:
  explicit FakeGuestMemory(Libs::Graphics::ShaderRecompiler::IR::SrtMemoryReader reader = nullptr,
                           void *userdata = nullptr) {
    Current() = {reader, userdata};
#if defined(__linux__)
    struct sigaction action {};
    action.sa_sigaction = Handle;
    action.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &action, &m_previous);
#elif defined(_WIN32)
    m_handler = AddVectoredExceptionHandler(1, Handle);
#endif
  }
  ~FakeGuestMemory() {
#if defined(__linux__)
    sigaction(SIGSEGV, &m_previous, nullptr);
#elif defined(_WIN32)
    RemoveVectoredExceptionHandler(m_handler);
#endif
    Current() = {};
  }
  FakeGuestMemory(const FakeGuestMemory &) = delete;
  FakeGuestMemory &operator=(const FakeGuestMemory &) = delete;

private:
  using Reader = std::pair<Libs::Graphics::ShaderRecompiler::IR::SrtMemoryReader, void *>;
  static Reader &Current() {
    static thread_local Reader reader;
    return reader;
  }
  static uint32_t Read(uint64_t address) {
    uint32_t word = 0;
    const auto [reader, userdata] = Current();
    return reader != nullptr && reader(userdata, address, {&word, 1}) ? word : 0u;
  }
#if defined(__linux__)
  static void Handle(int, siginfo_t *, void *context) {
    auto &registers = static_cast<ucontext_t *>(context)->uc_mcontext.gregs;
    const auto length =
        Libs::Graphics::ShaderRecompiler::IR::SrtWalkerLoadLength(registers[REG_RIP]);
    if (length == 0) std::abort();
    registers[REG_RAX] = Read(registers[REG_R10]);
    registers[REG_RIP] += length;
  }
  struct sigaction m_previous {};
#elif defined(_WIN32)
  static LONG CALLBACK Handle(EXCEPTION_POINTERS *info) {
    auto &context = *info->ContextRecord;
    const auto length = Libs::Graphics::ShaderRecompiler::IR::SrtWalkerLoadLength(context.Rip);
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || length == 0)
      return EXCEPTION_CONTINUE_SEARCH;
    context.Rax = Read(context.R10);
    context.Rip += length;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  void *m_handler = nullptr;
#endif
};

// Evaluates one value through a walker compiled for it, keeping the plan's resource walker.
inline bool EvaluateSrtValue(Libs::Graphics::ShaderRecompiler::IR::ResourcePlan &plan,
                             const Libs::Graphics::ShaderRecompiler::IR::SrtRuntime &runtime,
                             Libs::Graphics::ShaderRecompiler::IR::Value value, uint32_t &result) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const auto *inst = value.Resolve().TryInstruction();
  const auto output = inst != nullptr ? inst->WalkOutput() : UINT32_MAX;
  auto code = std::move(plan.srt_code);
  auto scratch = std::move(plan.srt_scratch);
  const auto user_data = plan.srt_user_data;
  const auto captures = plan.srt_captures;
  CompileSrtWalker(plan, {&value, 1});
  std::vector<uint32_t> flat;
  const bool known = WalkSrt(plan, runtime, flat, nullptr) && WalkedValue(plan, value, result);
  plan.srt_code = std::move(code);
  plan.srt_scratch = std::move(scratch);
  plan.srt_user_data = user_data;
  plan.srt_captures = captures;
  if (inst != nullptr) inst->SetWalkOutput(output);
  return known;
}

// Walks the plan's resources and reads back one descriptor.
inline bool EvaluateSrtDescriptor(const Libs::Graphics::ShaderRecompiler::IR::ResourcePlan &plan,
                                  const Libs::Graphics::ShaderRecompiler::IR::SrtRuntime &runtime,
                                  uint32_t source,
                                  Libs::Graphics::ShaderRecompiler::IR::DescriptorValue &result) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  std::vector<uint32_t> flat;
  return WalkSrt(plan, runtime, flat, nullptr) && WalkedDescriptor(plan, source, result);
}

#endif // KYTY_TESTS_SRTWALKERTESTMEMORY_H_
