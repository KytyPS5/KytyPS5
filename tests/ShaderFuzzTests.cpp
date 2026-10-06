// Deterministic crash-fuzzing for the shader frontend (decoder + translator).
//
// Invariant under test: no input, however malformed, may crash the frontend.
// Clean outcomes are translating the program or rejecting it (EXIT 321, the
// same code the negative decoder tests expect). A signal death or any other
// exit status is a bug: an out-of-bounds read, an unchecked assumption, or a
// missing rejection.
//
// Two levels, both seeded so every CI run executes the exact same cases:
// - Decode: arbitrary word streams, including truncated multi-word
//   instructions. Exercises length accounting and unknown-opcode paths.
// - Translate: small single-block programs built from valid scalar/vector ALU
//   instructions with randomized operands (including inline constants and
//   special registers) over a compute profile. Unterminated programs and
//   trailing garbage are included on purpose.
//
// Each case runs isolated in a forked child on POSIX (mirroring ExpectFatal);
// a crashing case is reported with its seed and index, which reproduce it
// exactly. Windows has no fork, so it runs the decode level in-process only.

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shader.h"
#include "graphics/shader/shaderCompiler.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
#include <cerrno>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif
// execinfo.h exists on glibc and Apple targets but not on musl.
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS &&     (defined(__GLIBC__) || defined(__APPLE__))
#define SHADER_FUZZ_HAS_EXECINFO 1
#include <execinfo.h>
#endif

namespace Libs::Graphics {
namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ShaderFuzzTests: failed: %s\n", text);
    std::abort();
  }
}

void EnsureConfigInitialized() {
  static bool config_initialized = false;
  if (!config_initialized) {
    static Common::Subsystems subsystems;
    Common::InitializeThreads();
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    subsystems.Initialize<Log::Lifecycle>();
    ShaderInit();
    config_initialized = true;
  }
}

ShaderRecompiler::CompileOptions MakeFuzzOptions() {
  static const ShaderComputeInputInfo compute{};
  ShaderRecompiler::CompileOptions options;
  options.input_info.compute = &compute;
  options.dump_ir = false;
  return options;
}

// xorshift64*: deterministic, no stdlib RNG state to manage.
uint64_t FuzzNext(uint64_t &state) {
  state ^= state >> 12u;
  state ^= state << 25u;
  state ^= state >> 27u;
  return state * 0x2545F4914F6CDD1Du;
}

constexpr uint32_t EncodeSop2(uint32_t opcode, uint32_t dst, uint32_t src0,
                              uint32_t src1) {
  return 0x80000000u | ((opcode & 0x7fu) << 23u) | ((dst & 0x7fu) << 16u) |
         ((src1 & 0xffu) << 8u) | (src0 & 0xffu);
}

constexpr uint32_t EncodeSoppEnd() {
  return 0x80000000u | (0x7fu << 23u) | (0x01u << 16u);
}

constexpr uint32_t EncodeVop2(uint32_t opcode, uint32_t dst, uint32_t src0,
                              uint32_t src1) {
  return ((opcode & 0x3fu) << 25u) | ((dst & 0xffu) << 17u) |
         ((src1 & 0xffu) << 9u) | (src0 & 0x1ffu);
}

constexpr uint32_t EncodeVop1Mov(uint32_t dst, uint32_t src0) {
  return (0x3fu << 25u) | ((dst & 0xffu) << 17u) | (0x01u << 9u) |
         (src0 & 0x1ffu);
}

constexpr uint32_t kSop2Ops[] = {0x00u, 0x01u, 0x02u, 0x03u, 0x07u, 0x09u,
                                 0x0eu, 0x10u, 0x12u, 0x1eu, 0x26u};
constexpr uint32_t kVop2Ops[] = {0x03u, 0x08u, 0x1bu, 0x1cu, 0x1du};

std::vector<uint32_t> MakeDecodeCase(uint64_t &rng) {
  const size_t words = 1u + FuzzNext(rng) % 16u;
  std::vector<uint32_t> code(words);
  for (auto &word : code) {
    word = static_cast<uint32_t>(FuzzNext(rng));
  }
  return code;
}

std::vector<uint32_t> MakeTranslateCase(uint64_t &rng) {
  std::vector<uint32_t> code;
  const size_t ops = 2u + FuzzNext(rng) % 20u;
  for (size_t i = 0; i < ops; i++) {
    const uint32_t kind = static_cast<uint32_t>(FuzzNext(rng) % 3u);
    const uint32_t dst = static_cast<uint32_t>(FuzzNext(rng) % 16u);
    const uint32_t src0 = static_cast<uint32_t>(FuzzNext(rng) % 256u);
    const uint32_t src1 = static_cast<uint32_t>(FuzzNext(rng) % 256u);
    if (kind == 0u) {
      code.push_back(EncodeSop2(kSop2Ops[FuzzNext(rng) % 11u], dst, src0, src1));
    } else if (kind == 1u) {
      code.push_back(EncodeVop2(kVop2Ops[FuzzNext(rng) % 5u], dst, src0, src1));
    } else {
      code.push_back(EncodeVop1Mov(dst, src0));
    }
  }
  if (FuzzNext(rng) % 4u != 0u) {
    code.push_back(EncodeSoppEnd());
    const size_t tail = FuzzNext(rng) % 4u;
    for (size_t i = 0; i < tail; i++) {
      code.push_back(static_cast<uint32_t>(FuzzNext(rng)));
    }
  }
  return code;
}

void RunDecodeCase(const std::vector<uint32_t> &code) {
  ShaderRecompiler::Decoder::Program program;
  ShaderRecompiler::Decoder::DecodeProgram(code, program);
}

void RunTranslateCase(const std::vector<uint32_t> &code) {
  auto options = MakeFuzzOptions();
  auto translated = ShaderRecompiler::TranslateProgram(code, options);
  (void)translated;
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
// Runs one case in a child. Exit 0 (handled) and 321 (clean rejection, the
// EXIT code) pass; anything else fails the suite with the case identity.
void RunIsolated(void (*body)(const std::vector<uint32_t> &),
                 const std::vector<uint32_t> &code, const char *level,
                 uint64_t seed, size_t index, size_t &failures) {
  const pid_t pid = ::fork();
  Check(pid >= 0, "fork failed while starting fuzz case");
  if (pid == 0) {
    // A crashing case should identify itself: print the crashing stack,
    // then die by the same signal so the parent attributes it correctly.
    struct CrashTracer {
      static void Handle(int sig) {
#ifdef SHADER_FUZZ_HAS_EXECINFO
        void *frames[32];
        const int count = ::backtrace(frames, 32);
        char header[64];
        const int len = std::snprintf(header, sizeof(header),
                                      "ShaderFuzzTests: crashed with signal %d\n", sig);
        (void)::write(STDERR_FILENO, header, static_cast<size_t>(len));
        ::backtrace_symbols_fd(frames, count, STDERR_FILENO);
#endif
        ::signal(sig, SIG_DFL);
        ::raise(sig);
      }
    };
    ::signal(SIGSEGV, CrashTracer::Handle);
    ::signal(SIGABRT, CrashTracer::Handle);
    ::signal(SIGBUS, CrashTracer::Handle);
    ::signal(SIGILL, CrashTracer::Handle);
    body(code);
    ::_exit(0);
  }
  int status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  Check(waited == pid, "waitpid failed while collecting fuzz case");
  const bool crashed = !WIFEXITED(status);
  const int exit_code = crashed ? -WTERMSIG(status) : WEXITSTATUS(status);
  if (crashed || (exit_code != 0 && exit_code != (321 & 0xff))) {
    std::fprintf(stderr,
                 "ShaderFuzzTests: %s case %zu (seed 0x%016llx) %s (exit %d)\n",
                 level, index, (unsigned long long)seed,
                 crashed ? "crashed" : "exited unexpectedly", exit_code);
    std::fprintf(stderr, "ShaderFuzzTests: words:");
    for (const uint32_t word : code) {
      std::fprintf(stderr, " 0x%08x", word);
    }
    std::fprintf(stderr, "\n");
    failures++;
  }
}
#else
void RunIsolated(void (*body)(const std::vector<uint32_t> &),
                 const std::vector<uint32_t> &code, const char *,
                 uint64_t, size_t, size_t &) {
  // No fork on Windows: decode-level only, which rejects via SetUnsupported
  // instead of fatal exits.
  body(code);
}
#endif

} // namespace
} // namespace Libs::Graphics

int main() {
  using namespace Libs::Graphics;
  EnsureConfigInitialized();

  constexpr uint64_t kDecodeSeed = 0x9E3779B97F4A7C15u;
  constexpr uint64_t kTranslateSeed = 0xBF58476D1CE4E5B9u;
  constexpr size_t kDecodeCases = 3000;
  constexpr size_t kTranslateCases = 300;

  // SHADER_FUZZ_REPRO=<index> reruns one case in-process (no fork) so a
  // debugger catches the crash directly, e.g.:
  //   SHADER_FUZZ_REPRO=75 lldb -b -o run -o bt -- ./shader_fuzz_tests
  // SHADER_FUZZ_WORDS=0x..,0x.. replays exact words instead, e.g. from a
  // reported "words:" line.
  // SHADER_FUZZ_REPRO replays decode cases only; translate cases are
  // identified by seed + index in failure reports.
  size_t repro = SIZE_MAX;
  if (const char *only = std::getenv("SHADER_FUZZ_REPRO")) {
    repro = static_cast<size_t>(std::strtoull(only, nullptr, 10));
    if (repro >= kDecodeCases) {
      std::fprintf(stderr,
                   "ShaderFuzzTests: SHADER_FUZZ_REPRO supports decode cases 0..%zu "
                   "only\n",
                   kDecodeCases - 1u);
      return 2;
    }
  }
  if (const char *hex = std::getenv("SHADER_FUZZ_WORDS")) {
    std::vector<uint32_t> code;
    const char *cursor = hex;
    while (*cursor != '\0') {
      code.push_back(static_cast<uint32_t>(std::strtoull(cursor, nullptr, 16)));
      while (*cursor != '\0' && *cursor != ',') cursor++;
      if (*cursor == ',') cursor++;
    }
    RunDecodeCase(code);
    std::printf("ShaderFuzzTests: word replay returned cleanly\n");
    return 0;
  }
  size_t failures = 0;
  uint64_t rng = kDecodeSeed;
  for (size_t i = 0; i < kDecodeCases; i++) {
    auto code = MakeDecodeCase(rng);
    if (i == repro) {
      RunDecodeCase(code);
      std::printf("ShaderFuzzTests: repro case %zu returned cleanly\n", i);
      return 0;
    }
    RunIsolated(RunDecodeCase, code, "decode", kDecodeSeed, i, failures);
  }
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
  rng = kTranslateSeed;
  for (size_t i = 0; i < kTranslateCases; i++) {
    RunIsolated(RunTranslateCase, MakeTranslateCase(rng), "translate",
                kTranslateSeed, i, failures);
  }
#endif
  if (failures != 0) {
    std::fprintf(stderr, "ShaderFuzzTests: %zu failing cases\n", failures);
    return 1;
  }
  std::printf("ShaderFuzzTests: %zu decode + %zu translate cases clean\n",
              kDecodeCases,
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
              kTranslateCases
#else
              0u
#endif
  );
  return 0;
}
