#ifndef KYTY_TESTS_SRTWALKERTESTMEMORY_H_
#define KYTY_TESTS_SRTWALKERTESTMEMORY_H_

#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <xbyak/xbyak.h>

#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <ucontext.h>
#elif defined(__APPLE__)
#include <sys/ucontext.h>
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
#if defined(__linux__) || defined(__APPLE__)
    struct sigaction action {};
    action.sa_sigaction = Handle;
    action.sa_flags = SA_SIGINFO;
    for (size_t index = 0; index < std::size(kSignals); ++index)
      sigaction(kSignals[index], &action, &m_previous[index]);
#elif defined(_WIN32)
    m_handler = AddVectoredExceptionHandler(1, Handle);
#endif
  }
  ~FakeGuestMemory() {
#if defined(__linux__) || defined(__APPLE__)
    for (size_t index = 0; index < std::size(kSignals); ++index)
      sigaction(kSignals[index], &m_previous[index], nullptr);
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
#if defined(__linux__) || defined(__APPLE__)
#if defined(__APPLE__)
  // macOS raises SIGBUS for some protection faults and SIGSEGV for others.
  static constexpr int kSignals[] = {SIGSEGV, SIGBUS};
#else
  static constexpr int kSignals[] = {SIGSEGV};
#endif
  static void Handle(int, siginfo_t *, void *context) {
    auto *native = static_cast<ucontext_t *>(context);
#if defined(__APPLE__)
    auto &rip = native->uc_mcontext->__ss.__rip;
    auto &rax = native->uc_mcontext->__ss.__rax;
    const auto r10 = native->uc_mcontext->__ss.__r10;
#else
    auto &rip = native->uc_mcontext.gregs[REG_RIP];
    auto &rax = native->uc_mcontext.gregs[REG_RAX];
    const auto r10 = native->uc_mcontext.gregs[REG_R10];
#endif
    const auto length = Libs::Graphics::ShaderRecompiler::IR::SrtWalkerLoadLength(rip);
    if (length == 0) std::abort();
    rax = Read(r10);
    rip += length;
  }
  struct sigaction m_previous[std::size(kSignals)] {};
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

inline Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  result->id = static_cast<uint32_t>(program.blocks.size() - 1u);
  program.block_storage.push_back(std::move(block));
  return *result;
}

// A complete compute program whose resource plan loads the DWORD at `address`.
inline Libs::Graphics::ShaderRecompiler::IR::Program SrtProgram(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return program;
}

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
