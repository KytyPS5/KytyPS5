#include "common/hostException.h"
#include "loader/x64InstructionEmulator.h"

#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>

namespace {

using Common::HostException::ExceptionInfo;
using Common::HostException::ExceptionType;

// The SSE4a bytes are the point of this test, so blobs are spelled out byte by
// byte instead of going through an encoder. Every blob is a complete routine:
// the instruction under test, a store of the result, and ret. Encodings were
// disassembled with otool to confirm the operand roles: EXTRQ immediate is
// unary (ModRM.r/m is both source and destination, the reg field is unused),
// INSERTQ immediate reads ModRM.r/m into the reg field.
constexpr uint8_t kExtrqXmm2Len40Index0[] = {
	0x66, 0x0f, 0x78, 0xc2, 0x28, 0x00, // extrq xmm2, 40, 0
	0xf3, 0x0f, 0x7f, 0x17,             // movdqu %xmm2, (%rdi)
	0xc3,                               // ret
};

constexpr uint8_t kExtrqXmm2Len64Index0[] = {
	0x66, 0x0f, 0x78, 0xc2, 0x40, 0x00, // extrq xmm2, 64, 0
	0xf3, 0x0f, 0x7f, 0x17,             // movdqu %xmm2, (%rdi)
	0xc3,                               // ret
};

constexpr uint8_t kInsertqXmm2Xmm0Len32Index0[] = {
	0xf2, 0x0f, 0x78, 0xd0, 0x20, 0x00, // insertq xmm2, xmm0, 32, 0
	0xf3, 0x0f, 0x7f, 0x17,             // movdqu %xmm2, (%rdi)
	0xc3,                               // ret
};

// REX.B moves the operand into the high register file. xmm10 is not an argument
// register, so the blob loads it from memory. EXTRQ has a single operand, so
// only the destination needs loading.
constexpr uint8_t kExtrqXmm10Len40Index0[] = {
	0xf3, 0x44, 0x0f, 0x6f, 0x16,             // movdqu (%rsi), %xmm10
	0x66, 0x41, 0x0f, 0x78, 0xc2, 0x28, 0x00, // extrq xmm10, 40, 0
	0xf3, 0x44, 0x0f, 0x7f, 0x17,             // movdqu %xmm10, (%rdi)
	0xc3,                                     // ret
};

// REX.R keeps the destination in xmm10 while REX.B puts the source in xmm9, and
// the inserted field sits at a nonzero index.
constexpr uint8_t kInsertqXmm10Xmm9Len16Index8[] = {
	0xf3, 0x44, 0x0f, 0x6f, 0x0e,             // movdqu (%rsi), %xmm9
	0xf3, 0x44, 0x0f, 0x6f, 0x12,             // movdqu (%rdx), %xmm10
	0xf2, 0x45, 0x0f, 0x78, 0xd1, 0x10, 0x08, // insertq xmm10, xmm9, 16, 8
	0xf3, 0x44, 0x0f, 0x7f, 0x17,             // movdqu %xmm10, (%rdi)
	0xc3,                                     // ret
};

constexpr uint8_t kUd2[] = {
	0x0f, 0x0b, // ud2
	0xc3,       // ret
};

constexpr size_t kPageSize = 4096;

std::atomic<int>    g_illegal_hits {0};
std::atomic<bool>   g_refused {false};
std::atomic<size_t> g_refuse_skip {0};

static_assert(decltype(g_illegal_hits)::is_always_lock_free);
static_assert(decltype(g_refused)::is_always_lock_free);
static_assert(decltype(g_refuse_skip)::is_always_lock_free);

void Check(bool value, const char *text) {
	if (!value) {
		std::fprintf(stderr, "macosSse4aTests: failed: %s\n", text);
		std::abort();
	}
}

bool Handler(const ExceptionInfo &info) {
	if (info.type != ExceptionType::IllegalInstruction) {
		return false;
	}
	g_illegal_hits.fetch_add(1, std::memory_order_relaxed);
	if (Loader::X64InstructionEmulator::TryEmulate(info.native_context)) {
		return true;
	}
	// A test that expects a refusal arms g_refuse_skip so the host-illegal
	// instruction can be stepped over. Any other refusal is outside the
	// emulator's contract, and returning there would re-run the faulting
	// instruction forever, so end the run instead.
	const size_t skip = g_refuse_skip.load(std::memory_order_relaxed);
	if (skip == 0) {
		static const char message[] = "macosSse4aTests: failed: unexpected emulator refusal\n";
		::write(STDERR_FILENO, message, sizeof(message) - 1);
		::_exit(1);
	}
	g_refused.store(true, std::memory_order_relaxed);
	auto *saved_context = static_cast<ucontext_t *>(info.native_context);
	saved_context->uc_mcontext->__ss.__rip += static_cast<uint64_t>(skip);
	return true;
}

template <typename Fn, size_t N>
Fn MapCode(const uint8_t (&code)[N]) {
	void *page = ::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	Check(page != MAP_FAILED, "map test code page");
	std::memcpy(page, code, N);
	Check(::mprotect(page, kPageSize, PROT_READ | PROT_EXEC) == 0, "make test code page executable");
	return reinterpret_cast<Fn>(page);
}

// Blobs receive their inputs through the SysV ABI, so the xmm argument registers
// are reachable as ordinary function parameters.
using BlobFn     = void (*)(uint64_t *out, double xmm0, double xmm1, double xmm2);
using LoadBlobFn = void (*)(uint64_t *out, const uint64_t *in);
using PairBlobFn = void (*)(uint64_t *out, const uint64_t *src, const uint64_t *dest);
using VoidBlobFn = void (*)();

int IllegalHits() {
	return g_illegal_hits.load(std::memory_order_relaxed);
}

void TestRefusals() {
	Check(!Loader::X64InstructionEmulator::TryEmulate(nullptr), "TryEmulate refuses a null native context");
	ucontext_t saved_context {};
	saved_context.uc_mcontext = nullptr;
	Check(!Loader::X64InstructionEmulator::TryEmulate(&saved_context), "TryEmulate refuses a saved context without mcontext");
}

void TestExtrqImmediate() {
	auto     fn      = MapCode<BlobFn>(kExtrqXmm2Len40Index0);
	uint64_t out[2] {};
	// The pattern is already in xmm2; the emulator keeps bits [39:0] and zeroes
	// the upper half.
	const double pattern = std::bit_cast<double>(0xaabbccddeeff1122ull);
	const int    before  = IllegalHits();
	fn(out, 0.0, 0.0, pattern);
	Check(IllegalHits() > before, "host raises SIGILL for EXTRQ");
	Check(out[0] == 0x000000ddeeff1122ull, "extrq xmm2, 40, 0 keeps bits [39:0] of xmm2");
	Check(out[1] == 0, "extrq xmm2, 40, 0 zero-extends the upper 64 bits");
}

void TestExtrqFullWidth() {
	auto     fn        = MapCode<BlobFn>(kExtrqXmm2Len64Index0);
	uint64_t out[2] {};
	const double pattern = std::bit_cast<double>(0x0123456789abcdefull);
	const int    before  = IllegalHits();
	fn(out, 0.0, 0.0, pattern);
	Check(IllegalHits() > before, "host raises SIGILL for a full width EXTRQ");
	Check(out[0] == 0x0123456789abcdefull, "extrq xmm2, 64, 0 keeps the whole low 64 bits");
	Check(out[1] == 0, "extrq xmm2, 64, 0 zero-extends the upper 64 bits");
}

void TestInsertqImmediate() {
	auto     fn        = MapCode<BlobFn>(kInsertqXmm2Xmm0Len32Index0);
	uint64_t out[2] {};
	const double source = std::bit_cast<double>(0x1122334455667788ull);
	const double dest   = std::bit_cast<double>(0x0123456789abcdefull);
	const int    before = IllegalHits();
	fn(out, source, 0.0, dest);
	Check(IllegalHits() > before, "host raises SIGILL for INSERTQ");
	Check(out[0] == 0x0123456755667788ull, "insertq xmm2, xmm0, 32, 0 inserts bits [31:0] of xmm0");
}

void TestRexHighRegisterExtrq() {
	auto           fn     = MapCode<LoadBlobFn>(kExtrqXmm10Len40Index0);
	uint64_t       out[2] {};
	const uint64_t in[2] {0xaabbccddeeff1122ull, 0};
	const int      before = IllegalHits();
	fn(out, in);
	Check(IllegalHits() > before, "host raises SIGILL for a REX.B EXTRQ");
	Check(out[0] == 0x000000ddeeff1122ull, "REX.B destination xmm10 is emulated through the saved context");
	Check(out[1] == 0, "REX.B destination xmm10 zero-extends the upper 64 bits");
}

void TestRexHighRegisterInsertqWithIndex() {
	auto           fn     = MapCode<PairBlobFn>(kInsertqXmm10Xmm9Len16Index8);
	uint64_t       out[2] {};
	const uint64_t src[2] {0x8877665544332211ull, 0xdeadbeefcafebabeull};
	const uint64_t dest[2] {0xaaaaaaaaaaaaaaaaull, 0xbbbbbbbbbbbbbbbbull};
	const int      before = IllegalHits();
	fn(out, src, dest);
	Check(IllegalHits() > before, "host raises SIGILL for a REX.R/REX.B INSERTQ");
	Check(out[0] == 0xaaaaaaaaaa2211aaull, "insertq xmm10, xmm9, 16, 8 inserts bits [15:0] at index 8");
	Check(out[1] == 0xbbbbbbbbbbbbbbbbull, "insertq xmm10, xmm9, 16, 8 leaves the upper 64 bits untouched");
}

void TestUnknownInstructionRefused() {
	g_refused.store(false, std::memory_order_relaxed);
	g_refuse_skip.store(2, std::memory_order_relaxed);
	auto      fn     = MapCode<VoidBlobFn>(kUd2);
	const int before = IllegalHits();
	fn();
	Check(IllegalHits() > before, "host raises SIGILL for the unknown instruction");
	Check(g_refused.load(std::memory_order_relaxed), "TryEmulate refuses an unknown instruction");
	g_refuse_skip.store(0, std::memory_order_relaxed);
}

} // namespace

int main() {
	Check(Common::HostException::InstallHandler(Handler), "install host exception handler");
	TestRefusals();
	TestExtrqImmediate();
	TestExtrqFullWidth();
	TestInsertqImmediate();
	TestRexHighRegisterExtrq();
	TestRexHighRegisterInsertqWithIndex();
	TestUnknownInstructionRefused();
	std::printf("macosSse4aTests: all passed\n");
	return 0;
}
