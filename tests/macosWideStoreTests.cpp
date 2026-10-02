// Checks X64InstructionEmulator::SplitWideStores under Rosetta: a 256-bit store that crosses
// from a writable into a write-protected page aborts the process ("unexpectedly need to
// EmulateForward on a synchronous exception"), while the split 128-bit form faults cleanly.

#include "loader/x64InstructionEmulator.h"

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		g_failures++;
	}
}

long g_page_size = 0;

void UnprotectOnFault(int /*signal*/, siginfo_t* info, void* /*context*/) {
	auto page = reinterpret_cast<uintptr_t>(info->si_addr) & ~static_cast<uintptr_t>(g_page_size - 1);
	mprotect(reinterpret_cast<void*>(page), g_page_size, PROT_READ | PROT_WRITE);
}

// void copy(uint8_t* dst /*rdi*/, const uint8_t* src /*rsi*/):
// the store to [rdi + 0x40] is the third instruction of the block and crosses the page boundary.
constexpr std::array<uint8_t, 24> CopyCode = {
    0xc5, 0xfc, 0x10, 0x06,       // vmovups ymm0, [rsi]
    0xc5, 0xfc, 0x10, 0x4e, 0x20, // vmovups ymm1, [rsi + 0x20]
    0xc5, 0xfc, 0x11, 0x47, 0x40, // vmovups [rdi + 0x40], ymm0
    0xc5, 0xfc, 0x11, 0x4f, 0x60, // vmovups [rdi + 0x60], ymm1
    0xc5, 0xf8, 0x77,             // vzeroupper
    0xc3,                         // ret
    0xcc,
};

using CopyFunc = void (*)(uint8_t*, const uint8_t*);

struct CodeArea {
	uint8_t* code        = nullptr;
	uint64_t trampolines = 0;
	uint64_t end         = 0;
};

CodeArea MakeCode() {
	constexpr size_t size = 64 * 1024;
	auto* base = static_cast<uint8_t*>(
	    mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0));
	if (base == MAP_FAILED) {
		base = static_cast<uint8_t*>(
		    mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0));
	}
	if (base == MAP_FAILED) {
		std::perror("mmap");
		std::exit(2);
	}
	std::memcpy(base, CopyCode.data(), CopyCode.size());
	return {base, reinterpret_cast<uint64_t>(base) + 4096, reinterpret_cast<uint64_t>(base) + size};
}

// dst + 0x40 is 16 bytes before a write-protected page, so the first store straddles it.
bool RunCrossingCopy(CopyFunc copy) {
	auto* data = static_cast<uint8_t*>(
	    mmap(nullptr, 4 * g_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
	uint8_t source[64];
	for (int i = 0; i < 64; i++) {
		source[i] = static_cast<uint8_t>(i + 1);
	}
	uint8_t* boundary = data + 2 * g_page_size;
	uint8_t* dst      = boundary - 0x50;
	mprotect(boundary, g_page_size, PROT_READ);
	copy(dst, source);
	return std::memcmp(dst + 0x40, source, 64) == 0;
}

void TestUnpatchedAbortsUnderRosetta() {
	const pid_t child = fork();
	if (child == 0) {
		auto area = MakeCode();
		RunCrossingCopy(reinterpret_cast<CopyFunc>(area.code));
		_exit(0);
	}
	int status = 0;
	waitpid(child, &status, 0);
	// Documents the Rosetta behaviour this patch works around; native x86-64 runs exit cleanly.
	if (WIFSIGNALED(status)) {
		std::printf("unpatched crossing store: child killed by signal %d (expected under Rosetta)\n",
		            WTERMSIG(status));
	} else {
		std::printf("unpatched crossing store: child exited %d (native x86-64?)\n",
		            WEXITSTATUS(status));
	}
}

void TestPatchedCrossingStore() {
	auto area   = MakeCode();
	auto cursor = area.trampolines;
	const auto result = Loader::X64InstructionEmulator::SplitWideStores(
	    reinterpret_cast<uint64_t>(area.code), CopyCode.size(), &cursor, area.end);
	Check(result.candidates == 2, "two 256-bit stores found");
	Check(result.patched == 2, "both stores patched");
	Check(area.code[9] == 0xe9 && area.code[14] == 0xe9, "stores replaced by rel32 jumps");
	Check(RunCrossingCopy(reinterpret_cast<CopyFunc>(area.code)), "patched copy wrote all bytes");
}

void TestShortStoreWithoutPaddingTraps() {
  auto area = MakeCode();
  // vmovups [rdi], ymm0 (4 bytes, too short for a rel32 jump); ret
  const std::array<uint8_t, 5> code = {0xc5, 0xfc, 0x11, 0x07, 0xc3};
  std::memcpy(area.code, code.data(), code.size());
  auto cursor = area.trampolines;
  const auto result = Loader::X64InstructionEmulator::SplitWideStores(
      reinterpret_cast<uint64_t>(area.code), code.size(), &cursor, area.end);
  // No padding in reach: the store becomes ud2, which the SIGILL handler
  // redirects.
  Check(result.candidates == 1 && result.trapped == 1,
        "4-byte store without padding trapped");
  Check(area.code[0] == 0x0f && area.code[1] == 0x0b && area.code[4] == 0xc3,
        "store replaced by ud2, ret kept");
}

void TestShortStoreViaInt3Padding() {
  auto area = MakeCode();
  // vmovups ymm0, [rsi]; vmovups [rdi], ymm0 (4 bytes); vzeroupper; ret; int3
  // padding
  const std::array<uint8_t, 20> code = {
      0xc5, 0xfc, 0x10, 0x06, 0xc5, 0xfc, 0x11, 0x07, 0xc5, 0xf8,
      0x77, 0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
  std::memcpy(area.code, code.data(), code.size());
  auto cursor = area.trampolines;
  const auto result = Loader::X64InstructionEmulator::SplitWideStores(
      reinterpret_cast<uint64_t>(area.code), code.size(), &cursor, area.end);
  Check(result.candidates == 1 && result.patched == 1 && result.via_cave == 1,
        "4-byte store patched through int3 padding");
  Check(area.code[4] == 0xeb && area.code[12] == 0xe9,
        "rel8 jump to a rel32 jump in padding");
  // The store straddles a write-protected page, like the crossing copy above.
  auto *data = static_cast<uint8_t *>(mmap(nullptr, 4 * g_page_size,
                                           PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANON, -1, 0));
  uint8_t source[32];
  for (int i = 0; i < 32; i++) {
    source[i] = static_cast<uint8_t>(0x40 + i);
  }
  uint8_t *boundary = data + 2 * g_page_size;
  mprotect(boundary, g_page_size, PROT_READ);
  reinterpret_cast<CopyFunc>(area.code)(boundary - 16, source);
  Check(std::memcmp(boundary - 16, source, 32) == 0,
        "short store wrote all bytes");
}

void TestShortStoreViaNopPadding() {
  auto area = MakeCode();
  // vmovups [rdi], ymm0 (4 bytes); ret; 5-byte nop (alignment padding after
  // ret)
  const std::array<uint8_t, 10> code = {0xc5, 0xfc, 0x11, 0x07, 0xc3,
                                        0x0f, 0x1f, 0x44, 0x00, 0x00};
  std::memcpy(area.code, code.data(), code.size());
  auto cursor = area.trampolines;
  const auto result = Loader::X64InstructionEmulator::SplitWideStores(
      reinterpret_cast<uint64_t>(area.code), code.size(), &cursor, area.end);
  Check(result.via_cave == 1 && area.code[5] == 0xe9,
        "nop padding after ret used as a cave");
  // A nop that execution falls into is not padding.
  const std::array<uint8_t, 10> live = {0xc5, 0xfc, 0x11, 0x07, 0x0f,
                                        0x1f, 0x44, 0x00, 0x00, 0xc3};
  std::memcpy(area.code, live.data(), live.size());
  cursor = area.trampolines;
  const auto live_result = Loader::X64InstructionEmulator::SplitWideStores(
      reinterpret_cast<uint64_t>(area.code), live.size(), &cursor, area.end);
  Check(live_result.via_cave == 0 && live_result.trapped == 1,
        "reachable nop not used as a cave");
}

void TestShortStoreViaLongStoreDeadBytes() {
  auto area = MakeCode();
  // f: vmovups ymm0, [rsi]; vmovups [rdi], ymm0 (4 bytes); vzeroupper; ret
  // g: vmovups [rdi + rax + 0x100], ymm0 (VEX3, 10 bytes: 5 dead bytes once
  // patched); ret
  const std::array<uint8_t, 23> code = {
      0xc5, 0xfc, 0x10, 0x06, 0xc5, 0xfc, 0x11, 0x07, 0xc5, 0xf8, 0x77, 0xc3,
      0xc4, 0xe1, 0x7c, 0x11, 0x84, 0x07, 0x00, 0x01, 0x00, 0x00, 0xc3};
  std::memcpy(area.code, code.data(), code.size());
  auto cursor = area.trampolines;
  const auto result = Loader::X64InstructionEmulator::SplitWideStores(
      reinterpret_cast<uint64_t>(area.code), code.size(), &cursor, area.end);
  Check(result.patched == 2 && result.via_cave == 1 && result.trapped == 0,
        "short store reaches its trampoline through a long store's dead bytes");
  Check(area.code[4] == 0xeb && area.code[12] == 0xe9 && area.code[17] == 0xe9,
        "rel8 into the dead bytes of the patched long store");
  uint8_t source[32];
  uint8_t destination[40]{};
  for (int i = 0; i < 32; i++) {
    source[i] = static_cast<uint8_t>(0x10 + i);
  }
  reinterpret_cast<CopyFunc>(area.code)(destination, source);
  Check(std::memcmp(destination, source, 32) == 0 && destination[32] == 0,
        "chained short store wrote its 32 bytes");
}

void TestHotRelocationSkipsBranchTargets() {
  // Direct branch targets must be found: a loop whose head is the instruction
  // after the store. store; L: add rbx, 0x20; dec ecx; jne L; ret
  auto area = MakeCode();
  const std::array<uint8_t, 13> code = {0xc5, 0xfc, 0x11, 0x03, 0x48,
                                        0x83, 0xc3, 0x20, 0xff, 0xc9,
                                        0x75, 0xf8, 0xc3};
  std::memcpy(area.code, code.data(), code.size());
  char path[] = "/tmp/kyty_hot_XXXXXX";
  const int fd = mkstemp(path);
  dprintf(
      fd, "0x%llx\n",
      static_cast<unsigned long long>(reinterpret_cast<uint64_t>(area.code)));
  close(fd);
  setenv("KYTY_WIDE_STORE_HOT", path, 1);
  auto cursor = area.trampolines;
  const auto result = Loader::X64InstructionEmulator::SplitWideStores(
      reinterpret_cast<uint64_t>(area.code), code.size(), &cursor, area.end);
  Check(result.relocated == 0 && result.trapped == 1,
        "loop head after a hot store not moved");
  unlink(path);
}

void TestRipRelativeStore() {
	auto area = MakeCode();
	// vmovups ymm0, [rsi]; vmovups [rip + 0x7f4], ymm0; vzeroupper; ret
	// The store ends at offset 12, so it targets offset 12 + 0x7f4 = 0x800.
	const std::array<uint8_t, 17> code = {0xc5, 0xfc, 0x10, 0x06, 0xc5, 0xfc, 0x11, 0x05, 0xf4,
	                                      0x07, 0x00, 0x00, 0xc5, 0xf8, 0x77, 0xc3, 0xcc};
	std::memcpy(area.code, code.data(), code.size());
	auto       cursor = area.trampolines;
	const auto result = Loader::X64InstructionEmulator::SplitWideStores(
	    reinterpret_cast<uint64_t>(area.code), code.size(), &cursor, area.end);
	Check(result.candidates == 1 && result.patched == 1, "RIP-relative store patched");
	uint8_t source[32];
	for (int i = 0; i < 32; i++) {
		source[i] = static_cast<uint8_t>(0xa0 + i);
	}
	std::memset(area.code + 0x800, 0, 64);
	reinterpret_cast<CopyFunc>(area.code)(nullptr, source);
	Check(std::memcmp(area.code + 0x800, source, 32) == 0, "RIP-relative store hit its target");
	Check(area.code[0x800 + 32] == 0, "RIP-relative store wrote nothing past its target");
}

} // namespace

int main() {
	g_page_size = sysconf(_SC_PAGESIZE);
	struct sigaction action {};
	action.sa_sigaction = UnprotectOnFault;
	action.sa_flags     = SA_SIGINFO;
	sigaction(SIGSEGV, &action, nullptr);
	sigaction(SIGBUS, &action, nullptr);

        // Reads KYTY_WIDE_STORE_HOT, which the splitter loads once per process:
        // run it first.
        TestHotRelocationSkipsBranchTargets();
        TestUnpatchedAbortsUnderRosetta();
	TestPatchedCrossingStore();
        TestShortStoreWithoutPaddingTraps();
        TestShortStoreViaInt3Padding();
        TestShortStoreViaNopPadding();
        TestShortStoreViaLongStoreDeadBytes();
        TestRipRelativeStore();

	if (g_failures != 0) {
		std::fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("macos_wide_store_tests: all checks passed\n");
	return 0;
}
