// Bit-exact check of the code the AMD CPU patcher puts into a guest module, run for real on the CPU
// (x86-64, i.e. under Rosetta on Apple silicon).
//  - EXTRQ native trampoline and INSERTQ (trap + signal emulator), immediate and register forms, against a
//    bit-by-bit model of the AMD APM (vol. 4: EXTRQ/INSERTQ).
//  - VRSQRTPS replacement against the signal emulator, the host's rsqrtps and an exact long double.
//  - KYTY_AMD_PATCH selection: which instruction groups the patcher finds.
// Where the APM leaves a result undefined (idx + len > 64, upper quadword), the test pins the convention of
// the emulator (zero fill above bit 63; EXTRQ clears the upper quadword, INSERTQ keeps it) and reports the
// number of such inputs separately.
#include "common/amdCpuMode.h"
#include "common/hostException.h"
#include "loader/guestInstructionPatcher.h"
#include "loader/x64InstructionEmulator.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <vector>

namespace {

using Common::HostException::ExceptionInfo;
using Common::HostException::ExceptionType;

constexpr size_t kPage = 4096;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "amdTrampolineBitExactTests: failed: %s\n", text);
		std::exit(1); // not abort: ctest WILL_FAIL does not invert a crash
	}
}

bool Handler(const ExceptionInfo& info) {
	if (info.type != ExceptionType::IllegalInstruction) return false;
	if (Loader::X64InstructionEmulator::TryEmulate(info.native_context)) return true;
	static const char message[] = "amdTrampolineBitExactTests: failed: unexpected emulator refusal\n";
	::write(STDERR_FILENO, message, sizeof(message) - 1);
	::_exit(1);
}

using Bytes = std::vector<uint8_t>;

Bytes Cat(std::initializer_list<Bytes> parts) {
	Bytes all;
	for (const auto& part: parts) all.insert(all.end(), part.begin(), part.end());
	return all;
}

// Every blob takes (out = rdi, in = rsi). Input layout in `in`: qwords 0..1 destination xmm, 2..3 source xmm.
using Fn = void (*)(uint64_t* out, const uint64_t* in);

struct Mapped {
	void*                               area = nullptr;
	Loader::GuestInstructionPatchResult result {};
	Fn                                  fn() const { return reinterpret_cast<Fn>(area); }
};

// Code in the first page, trampolines in the second, like a mapped guest module. The host is presented as a
// CPU without SSE4a, RDPID and CLWB (what Rosetta reports).
Mapped MapPatched(const Bytes& blob, bool patch, uint32_t kinds = Common::AmdPatchAll) {
	Mapped mapped;
	mapped.area = ::mmap(nullptr, 2 * kPage, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
	Check(mapped.area != MAP_FAILED, "map RWX area");
	std::memcpy(mapped.area, blob.data(), blob.size());
	if (patch) {
		auto* trampoline = static_cast<uint8_t*>(mapped.area) + kPage;
		Loader::RegisterGuestInstructionPatchModule(mapped.area, kPage, trampoline, kPage);
		const std::array<uintptr_t, 1> starts {reinterpret_cast<uintptr_t>(mapped.area)};
		mapped.result = Loader::PatchGuestInstructions(reinterpret_cast<uintptr_t>(mapped.area), blob.size(),
		                                               starts, false, true, {false, false, false}, kinds);
		Loader::UnregisterGuestInstructionPatchModule(mapped.area);
	}
	return mapped;
}

void Unmap(const Mapped& mapped) {
	::munmap(mapped.area, 2 * kPage);
}

struct Rng {
	uint64_t s;
	uint64_t Next() {
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return s * 0x2545f4914f6cdd1dull;
	}
};

// ---- Model from the AMD APM, written bit by bit ----------------------------------------------------------

struct Field {
	unsigned len;     // 1..64
	unsigned idx;     // 0..63
	bool     defined; // APM: idx + len > 64 is undefined (a zero length field means 64)
};

Field Decode(unsigned length_field, unsigned index_field) {
	Field f {};
	f.len     = (length_field & 63u) == 0 ? 64u : (length_field & 63u);
	f.idx     = index_field & 63u;
	f.defined = f.idx + f.len <= 64;
	return f;
}

uint64_t ModelExtract(uint64_t data, Field f) {
	uint64_t result = 0;
	for (unsigned bit = 0; bit < f.len; ++bit) {
		if (f.idx + bit < 64) result |= ((data >> (f.idx + bit)) & 1u) << bit;
	}
	return result;
}

uint64_t ModelInsert(uint64_t dest, uint64_t source, Field f) {
	for (unsigned bit = 0; bit < f.len; ++bit) {
		if (f.idx + bit >= 64) break;
		const uint64_t mask = uint64_t {1} << (f.idx + bit);
		dest                = (dest & ~mask) | (((source >> bit) & 1u) << (f.idx + bit));
	}
	return dest;
}

// ---- Blobs -----------------------------------------------------------------------------------------------

const Bytes kLoadDest2  = {0xf3, 0x0f, 0x6f, 0x16};       // movdqu (%rsi), %xmm2
const Bytes kLoadSrc0   = {0xf3, 0x0f, 0x6f, 0x46, 0x10}; // movdqu 0x10(%rsi), %xmm0
const Bytes kStore2     = {0xf3, 0x0f, 0x7f, 0x17};       // movdqu %xmm2, (%rdi)
const Bytes kRet        = {0xc3};

Bytes ExtrqImm(uint8_t length, uint8_t index) {
	return Cat({kLoadDest2, {0x66, 0x0f, 0x78, 0xc2, length, index}, kStore2, kRet}); // extrq xmm2, len, idx
}
Bytes ExtrqReg() {
	return Cat({kLoadDest2, kLoadSrc0, {0x66, 0x0f, 0x79, 0xd0}, kStore2, kRet}); // extrq xmm2, xmm0
}
Bytes ExtrqRegRex() { // extrq xmm10, xmm9
	return {0xf3, 0x44, 0x0f, 0x6f, 0x16, 0xf3, 0x44, 0x0f, 0x6f, 0x4e, 0x10, 0x66, 0x45, 0x0f, 0x79, 0xd1,
	        0xf3, 0x44, 0x0f, 0x7f, 0x17, 0xc3};
}
Bytes ExtrqSameReg() { // extrq xmm9, xmm9: the controls are the data
	return {0xf3, 0x44, 0x0f, 0x6f, 0x0e, 0x66, 0x45, 0x0f, 0x79, 0xc9, 0xf3, 0x44, 0x0f, 0x7f, 0x0f, 0xc3};
}
Bytes InsertqImm(uint8_t length, uint8_t index) {
	return Cat({kLoadDest2, kLoadSrc0, {0xf2, 0x0f, 0x78, 0xd0, length, index}, kStore2, kRet}); // insertq xmm2, xmm0, len, idx
}
Bytes InsertqReg() {
	return Cat({kLoadDest2, kLoadSrc0, {0xf2, 0x0f, 0x79, 0xd0}, kStore2, kRet}); // insertq xmm2, xmm0
}

// EXTRQ imm with the upper YMM half dirty, known GPRs/flags/red zone, to see what the trampoline preserves.
// Output: out[0..1] xmm2, out[2..3] ymm2 upper half, out[4] rax, out[5] rcx, out[6] rdx, out[7] CF, out[8] red zone.
Bytes ExtrqPreserve() {
	return {0x48, 0xb8, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, // movabs $0x1111.., %rax
	        0x48, 0xb9, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, // movabs $0x2222.., %rcx
	        0x48, 0xba, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, // movabs $0x3333.., %rdx
	        0x48, 0x89, 0x44, 0x24, 0xf8,                               // mov %rax, -8(%rsp): red zone
	        0xc5, 0xfc, 0x10, 0x16,                                     // vmovups (%rsi), %ymm2
	        0xf9,                                                       // stc
	        0x66, 0x0f, 0x78, 0xc2, 0x28, 0x04,                         // extrq xmm2, 40, 4
	        0x41, 0x0f, 0x92, 0xc0,                                     // setc %r8b
	        0xc5, 0xfc, 0x11, 0x17,                                     // vmovups %ymm2, (%rdi)
	        0x48, 0x89, 0x47, 0x20,                                     // mov %rax, 0x20(%rdi)
	        0x48, 0x89, 0x4f, 0x28,                                     // mov %rcx, 0x28(%rdi)
	        0x48, 0x89, 0x57, 0x30,                                     // mov %rdx, 0x30(%rdi)
	        0x4d, 0x0f, 0xb6, 0xc0,                                     // movzbq %r8b, %r8
	        0x4c, 0x89, 0x47, 0x38,                                     // mov %r8, 0x38(%rdi)
	        0x48, 0x8b, 0x44, 0x24, 0xf8,                               // mov -8(%rsp), %rax
	        0x48, 0x89, 0x47, 0x40,                                     // mov %rax, 0x40(%rdi)
	        0xc5, 0xf8, 0x77,                                           // vzeroupper
	        0xc3};
}

// 4 floats in in[0..3], 4 more in in[4..7] (upper half of the dirtied ymm1), MXCSR value at in + 0x40.
// Output: 4 floats, then the upper half of ymm1 (vextractf128), MXCSR at out + 0x40.
Bytes Rsqrt(bool plain_vex, bool host_sse) {
	Bytes code = {0x0f, 0xae, 0x56, 0x40,                         // ldmxcsr 0x40(%rsi)
	              0x0f, 0x10, 0x06,                               // movups (%rsi), %xmm0
	              0xc5, 0xfc, 0x10, 0x0e};                        // vmovups (%rsi), %ymm1
	if (host_sse) {
		code.insert(code.end(), {0x0f, 0x52, 0xc8}); // rsqrtps %xmm0, %xmm1 (legacy SSE, the host's own)
	} else {
		// vvvv = 1111 is the plain form the patcher finds; 1110 is a reserved bit pattern: SIGILL on the host.
		code.insert(code.end(), {0xc5, static_cast<uint8_t>(plain_vex ? 0xf8 : 0xf0), 0x52, 0xc8});
	}
	for (uint8_t b: {0x0f, 0x11, 0x0f,                                   // movups %xmm1, (%rdi)
	                 0xc4, 0xe3, 0x7d, 0x19, 0x4f, 0x10, 0x01,           // vextractf128 $1, %ymm1, 0x10(%rdi)
	                 0x0f, 0xae, 0x5f, 0x40,                             // stmxcsr 0x40(%rdi)
	                 0xc5, 0xf8, 0x77,                                   // vzeroupper
	                 0xc3}) {
		code.push_back(b);
	}
	return code;
}

// ---- EXTRQ / INSERTQ -------------------------------------------------------------------------------------

struct Stats {
	uint64_t inputs      = 0;
	uint64_t undefined   = 0; // inputs where the APM leaves the result undefined
	uint64_t mismatches  = 0;
};

constexpr uint64_t kEdgeData[] = {0, ~0ull, 1, 1ull << 63, 0x8000000000000001ull, 0xaaaaaaaaaaaaaaaaull,
                                  0x5555555555555555ull, 0x0123456789abcdefull};

uint64_t PickData(Rng& rng, size_t i) {
	return i < std::size(kEdgeData) ? kEdgeData[i] : rng.Next();
}

void RunExtrqImm(Stats& stats, Rng& rng, unsigned length_byte, unsigned index_byte, size_t samples) {
	const auto code = Mapped(MapPatched(ExtrqImm(static_cast<uint8_t>(length_byte), static_cast<uint8_t>(index_byte)), true));
	Check(code.result.extrq.found == 1 && code.result.extrq.native == 1, "EXTRQ imm: found and relocated natively");
	const Field f = Decode(length_byte, index_byte);
	for (size_t i = 0; i < samples; ++i) {
		const uint64_t in[4] {PickData(rng, i), rng.Next(), 0, 0};
		uint64_t       out[2] {};
		code.fn()(out, in);
		++stats.inputs;
		stats.undefined += !f.defined;
		if (out[0] != ModelExtract(in[0], f) || out[1] != 0) {
			if (++stats.mismatches <= 3) {
				std::fprintf(stderr, "EXTRQ imm len=%u idx=%u data=%016llx -> %016llx:%016llx want %016llx\n",
				             length_byte, index_byte, (unsigned long long)in[0], (unsigned long long)out[1],
				             (unsigned long long)out[0], (unsigned long long)ModelExtract(in[0], f));
			}
		}
	}
	Unmap(code);
}

void RunInsertqImm(Stats& stats, Rng& rng, unsigned length_byte, unsigned index_byte, size_t samples) {
	const auto code = MapPatched(InsertqImm(static_cast<uint8_t>(length_byte), static_cast<uint8_t>(index_byte)), true);
	Check(code.result.insertq.found == 1 && code.result.insertq.trapped == 1, "INSERTQ imm: found and trapped");
	const Field f = Decode(length_byte, index_byte);
	for (size_t i = 0; i < samples; ++i) {
		const uint64_t in[4] {PickData(rng, i), rng.Next(), PickData(rng, samples - i), rng.Next()};
		uint64_t       out[2] {};
		code.fn()(out, in);
		++stats.inputs;
		stats.undefined += !f.defined;
		if (out[0] != ModelInsert(in[0], in[2], f) || out[1] != in[1]) {
			if (++stats.mismatches <= 3) {
				std::fprintf(stderr, "INSERTQ imm len=%u idx=%u dest=%016llx src=%016llx -> %016llx want %016llx\n",
				             length_byte, index_byte, (unsigned long long)in[0], (unsigned long long)in[2],
				             (unsigned long long)out[0], (unsigned long long)ModelInsert(in[0], in[2], f));
			}
		}
	}
	Unmap(code);
}

// Register forms: controls come from the source operand (EXTRQ: low quadword bits 5:0 length, 13:8 index;
// INSERTQ: the same fields of the upper quadword). Bits 7:6 and 63:14 are ignored.
uint64_t RandomControls(Rng& rng, size_t i) {
	const uint64_t noise = rng.Next();
	switch (i % 4) {
		case 0: return noise; // anything, including the ignored bits
		case 1: return (noise & ~0x3f3full) | ((i / 4) & 0x3f) | ((((i / 4) >> 6) & 0x3f) << 8); // sweep len/idx
		case 2: return (noise & ~0x3f3full) | (rng.Next() & 63) | ((rng.Next() & 63) << 8);
		default: return (noise & ~0x3f3full) | ((i >> 2) & 1 ? 0 : 0) | (63ull << 8) | (rng.Next() & 63);
	}
}

void RunExtrqReg(Stats& stats, Rng& rng, const Mapped& code, bool same_register, size_t samples) {
	Check(code.result.extrq.found == 1 && code.result.extrq.native == 1, "EXTRQ reg: found and relocated natively");
	for (size_t i = 0; i < samples; ++i) {
		const uint64_t controls = RandomControls(rng, i);
		const uint64_t data     = same_register ? controls : PickData(rng, i % 64);
		const uint64_t in[4] {data, rng.Next(), controls, rng.Next()};
		uint64_t       out[2] {};
		code.fn()(out, in);
		const Field    f    = Decode(static_cast<unsigned>(controls & 0xff), static_cast<unsigned>((controls >> 8) & 0xff));
		const uint64_t want = ModelExtract(data, f);
		++stats.inputs;
		stats.undefined += !f.defined;
		if (out[0] != want || out[1] != 0) {
			if (++stats.mismatches <= 3) {
				std::fprintf(stderr, "EXTRQ reg ctl=%016llx data=%016llx -> %016llx:%016llx want %016llx\n",
				             (unsigned long long)controls, (unsigned long long)data, (unsigned long long)out[1],
				             (unsigned long long)out[0], (unsigned long long)want);
			}
		}
	}
}

void RunInsertqReg(Stats& stats, Rng& rng, const Mapped& code, size_t samples) {
	Check(code.result.insertq.found == 1 && code.result.insertq.trapped == 1, "INSERTQ reg: found and trapped");
	for (size_t i = 0; i < samples; ++i) {
		const uint64_t controls = RandomControls(rng, i);
		const uint64_t in[4] {PickData(rng, i % 64), rng.Next(), PickData(rng, (i + 3) % 64), controls};
		uint64_t       out[2] {};
		code.fn()(out, in);
		const Field    f    = Decode(static_cast<unsigned>(controls & 0xff), static_cast<unsigned>((controls >> 8) & 0xff));
		const uint64_t want = ModelInsert(in[0], in[2], f);
		++stats.inputs;
		stats.undefined += !f.defined;
		if (out[0] != want || out[1] != in[1]) {
			if (++stats.mismatches <= 3) {
				std::fprintf(stderr, "INSERTQ reg ctl=%016llx dest=%016llx src=%016llx -> %016llx want %016llx\n",
				             (unsigned long long)controls, (unsigned long long)in[0], (unsigned long long)in[2],
				             (unsigned long long)out[0], (unsigned long long)want);
			}
		}
	}
}

void Report(const char* name, const Stats& stats) {
	std::printf("  %-12s inputs=%llu undefined-per-APM=%llu mismatches=%llu\n", name,
	            (unsigned long long)stats.inputs, (unsigned long long)stats.undefined,
	            (unsigned long long)stats.mismatches);
	Check(stats.mismatches == 0, name);
}

void TestBitFields() {
	Rng rng {0x9e3779b97f4a7c15ull};
	// Immediate forms: every (length byte, index byte) pair of the low six bits, plus every pair with the
	// ignored high bits set. 4096 + 4096 blobs, 128 inputs each (EXTRQ native), 16 for INSERTQ (a signal each).
	Stats extrq_imm, insertq_imm;
	for (unsigned length = 0; length < 64; ++length) {
		for (unsigned index = 0; index < 64; ++index) {
			RunExtrqImm(extrq_imm, rng, length, index, 128);
			RunExtrqImm(extrq_imm, rng, length | 0xc0u, index | 0xc0u, 128);
			RunInsertqImm(insertq_imm, rng, length, index, 8);
			RunInsertqImm(insertq_imm, rng, length | 0x40u, index | 0x80u, 8);
		}
	}
	Report("EXTRQ imm", extrq_imm);
	Report("INSERTQ imm", insertq_imm);

	// Named edge cases: len=0 (64), idx=63, idx+len == 64, idx+len > 64, upper quadword.
	{
		const auto code = MapPatched(ExtrqImm(0, 0), true);
		const uint64_t in[4] {0x0123456789abcdefull, 0xfedcba9876543210ull, 0, 0};
		uint64_t       out[2] {};
		code.fn()(out, in);
		Check(out[0] == 0x0123456789abcdefull && out[1] == 0, "EXTRQ len=0 means 64 bits and clears the upper quadword");
		Unmap(code);
		const auto top = MapPatched(ExtrqImm(1, 63), true);
		top.fn()(out, in);
		Check(out[0] == 0 && out[1] == 0, "EXTRQ idx=63 len=1 takes bit 63");
		Unmap(top);
		const auto end = MapPatched(ExtrqImm(32, 32), true);
		end.fn()(out, in);
		Check(out[0] == 0x01234567 && out[1] == 0, "EXTRQ idx+len == 64 takes the top half");
		Unmap(end);
	}

	// Register forms: 1,048,576 EXTRQ inputs through the native trampoline per operand layout (low registers,
	// REX high registers, source == destination); 1,048,576 INSERTQ through the trap path.
	Stats extrq_reg, extrq_rex, extrq_same, insertq_reg;
	{
		const auto code = MapPatched(ExtrqReg(), true);
		RunExtrqReg(extrq_reg, rng, code, false, 1u << 20);
		Unmap(code);
		const auto rex = MapPatched(ExtrqRegRex(), true);
		RunExtrqReg(extrq_rex, rng, rex, false, 1u << 18);
		Unmap(rex);
		const auto same = MapPatched(ExtrqSameReg(), true);
		Check(same.result.extrq.found == 1 && same.result.extrq.native == 1, "EXTRQ same register: native");
		for (size_t i = 0; i < (1u << 18); ++i) {
			const uint64_t controls = RandomControls(rng, i);
			const uint64_t in[4] {controls, rng.Next(), 0, 0};
			uint64_t       out[2] {};
			same.fn()(out, in);
			const Field f = Decode(static_cast<unsigned>(controls & 0xff), static_cast<unsigned>((controls >> 8) & 0xff));
			++extrq_same.inputs;
			extrq_same.undefined += !f.defined;
			extrq_same.mismatches += out[0] != ModelExtract(controls, f) || out[1] != 0;
		}
		Unmap(same);
		const auto insert = MapPatched(InsertqReg(), true);
		RunInsertqReg(insertq_reg, rng, insert, 1u << 20);
		Unmap(insert);
	}
	Report("EXTRQ reg", extrq_reg);
	Report("EXTRQ reg rex", extrq_rex);
	Report("EXTRQ same", extrq_same);
	Report("INSERTQ reg", insertq_reg);
}

void TestExtrqPreservesState() {
	const auto code = MapPatched(ExtrqPreserve(), true);
	Check(code.result.extrq.found == 1 && code.result.extrq.native == 1, "EXTRQ in the preserve blob is native");
	alignas(32) uint64_t in[4] {0xaabbccddeeff1122ull, 0x5555555555555555ull, 0x0123456789abcdefull, 0xfedcba9876543210ull};
	alignas(32) uint64_t out[10] {};
	code.fn()(out, in);
	Check(out[0] == ModelExtract(in[0], Decode(40, 4)) && out[1] == 0, "EXTRQ result in the preserve blob");
	Check(out[2] == in[2] && out[3] == in[3], "EXTRQ keeps the upper YMM half");
	Check(out[4] == 0x1111111111111111ull && out[5] == 0x2222222222222222ull && out[6] == 0x3333333333333333ull,
	      "EXTRQ trampoline keeps rax, rcx and rdx");
	Check(out[7] == 1, "EXTRQ trampoline keeps the carry flag");
	Check(out[8] == 0x1111111111111111ull, "EXTRQ trampoline keeps the SysV red zone");
	Unmap(code);
	std::printf("  EXTRQ trampoline preserves upper YMM, rax/rcx/rdx, CF and the red zone\n");
}

// ---- VRSQRTPS --------------------------------------------------------------------------------------------

struct RsqrtOut {
	uint32_t lanes[4];
	uint32_t upper[4];
	uint32_t mxcsr;
};

RsqrtOut RunRsqrt(const Mapped& code, const uint32_t (&lanes)[4], uint32_t mxcsr) {
	alignas(32) uint32_t in[32] {};
	alignas(32) uint32_t out[32] {};
	std::memcpy(in, lanes, sizeof(lanes));
	in[4] = in[5] = in[6] = in[7] = 0x3f800000u; // dirty upper half of ymm1
	in[16]                          = mxcsr;
	code.fn()(reinterpret_cast<uint64_t*>(out), reinterpret_cast<const uint64_t*>(in));
	_mm_setcsr(0x1f80);
	RsqrtOut result {};
	std::memcpy(result.lanes, out, sizeof(result.lanes));
	std::memcpy(result.upper, out + 4, sizeof(result.upper));
	result.mxcsr = out[16];
	return result;
}

uint32_t RandomFloatBits(Rng& rng, size_t i) {
	static const uint32_t edges[] = {0x00000000u, 0x80000000u, 0x00000001u, 0x007fffffu, 0x80000001u, 0x807fffffu,
	                                 0x00800000u, 0x80800000u, 0x7f7fffffu, 0xff7fffffu, 0x7f800000u, 0xff800000u,
	                                 0x7fc00000u, 0xffc00000u, 0x7f800001u, 0xff800001u, 0x7fbfffffu, 0x7fffffffu,
	                                 0x3f800000u, 0x3e800000u, 0x40800000u, 0x7f000000u, 0x01000000u};
	if (i < std::size(edges)) return edges[i];
	switch (i % 3) {
		case 0: return static_cast<uint32_t>(rng.Next()); // any bit pattern
		case 1: return static_cast<uint32_t>(rng.Next()) & 0x7fffffffu; // non-negative
		default: { // positive normal, uniform in the exponent
			const uint32_t exponent = 1 + static_cast<uint32_t>(rng.Next() % 254);
			return (exponent << 23) | (static_cast<uint32_t>(rng.Next()) & 0x7fffffu);
		}
	}
}

double RelativeError(uint32_t bits, uint32_t result) {
	const float x = std::bit_cast<float>(bits);
	if (!(x > 0) || std::isinf(x) || (bits & 0x7f800000u) == 0) return 0; // classes are checked separately
	const long double exact = 1.0L / std::sqrt(static_cast<long double>(x));
	return static_cast<double>(std::fabs((static_cast<long double>(std::bit_cast<float>(result)) - exact) / exact));
}

void TestReciprocalSquareRoot() {
	Rng        rng {0x1234567887654321ull};
	const auto trampoline = MapPatched(Rsqrt(true, false), true);
	const auto emulated   = MapPatched(Rsqrt(false, false), false); // vvvv = 1110: SIGILL, signal emulator
	const auto host       = MapPatched(Rsqrt(true, true), false);
	Check(trampoline.result.reciprocal_sqrt.found == 1 && trampoline.result.reciprocal_sqrt.native == 1,
	      "VRSQRTPS replaced natively");

	double   max_trampoline = 0, max_emulated = 0, max_host = 0;
	uint64_t lanes = 0, differs_emulator = 0, differs_host = 0, host_class_differs = 0, emulated_lanes = 0;
	// Mxcsr: the replacement must leave the guest's value alone, including a non-default rounding mode.
	const uint32_t mxcsr_values[] = {0x1f80u, 0x7f80u, 0x1fc0u, 0x9fc0u};
	const size_t   groups         = (1u << 20) / 4;
	for (size_t g = 0; g < groups; ++g) {
		uint32_t in[4];
		for (size_t k = 0; k < 4; ++k) in[k] = RandomFloatBits(rng, g * 4 + k);
		const uint32_t mxcsr = mxcsr_values[g % 4];
		const auto     t     = RunRsqrt(trampoline, in, mxcsr);
		const auto     h     = RunRsqrt(host, in, mxcsr);
		Check(t.mxcsr == mxcsr, "VRSQRTPS replacement preserves MXCSR");
		Check(std::memcmp(t.upper, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0, "VEX.128 form zeroes the upper YMM half");
		RsqrtOut e {};
		const bool emulate_this = g % 4 == 0;
		if (emulate_this) {
			e = RunRsqrt(emulated, in, mxcsr);
			emulated_lanes += 4;
		}
		for (size_t k = 0; k < 4; ++k) {
			++lanes;
			max_trampoline = std::max(max_trampoline, RelativeError(in[k], t.lanes[k]));
			max_host       = std::max(max_host, RelativeError(in[k], h.lanes[k]));
			differs_host += t.lanes[k] != h.lanes[k];
			const bool special = (in[k] & 0x7f800000u) == 0 || (in[k] & 0x7fffffffu) >= 0x7f800000u || (in[k] >> 31);
			host_class_differs += special && t.lanes[k] != h.lanes[k];
			if (emulate_this) {
				max_emulated = std::max(max_emulated, RelativeError(in[k], e.lanes[k]));
				differs_emulator += t.lanes[k] != e.lanes[k];
			}
		}
	}
	std::printf("  VRSQRTPS lanes=%llu (emulator %llu): max rel error trampoline=%.3e emulator=%.3e host rsqrtps=%.3e"
	            " (2^-24=%.3e, AMD/Intel bound 1.5*2^-12=%.3e)\n",
	            (unsigned long long)lanes, (unsigned long long)emulated_lanes, max_trampoline, max_emulated, max_host,
	            std::ldexp(1.0, -24), 1.5 * std::ldexp(1.0, -12));
	std::printf("  VRSQRTPS lanes whose bits differ: trampoline vs emulator=%llu, trampoline vs host rsqrtps=%llu"
	            " (of which zero/denormal/negative/Inf/NaN inputs=%llu)\n",
	            (unsigned long long)differs_emulator, (unsigned long long)differs_host,
	            (unsigned long long)host_class_differs);
	Check(differs_emulator == 0, "the replacement and the signal emulator agree bit for bit");
	Check(max_trampoline <= std::ldexp(1.0, -23), "the replacement is an accurate (correctly rounded) estimate");
	Check(max_host <= 1.5 * std::ldexp(1.0, -12) * 1.001, "host rsqrtps is within the architectural bound");

	// Special classes against the architectural definition (SDM/APM): denormal as zero, 0 -> Inf with sign,
	// negative -> default NaN, +Inf -> +0, QNaN passes through, SNaN is quieted.
	struct Case { uint32_t in, out; const char* name; };
	const Case cases[] = {
	    {0x00000000u, 0x7f800000u, "+0"},          {0x80000000u, 0xff800000u, "-0"},
	    {0x00000001u, 0x7f800000u, "+denormal"},   {0x807fffffu, 0xff800000u, "-denormal"},
	    {0x7f800000u, 0x00000000u, "+Inf"},        {0xff800000u, 0xffc00000u, "-Inf"},
	    {0xbf800000u, 0xffc00000u, "-1"},          {0x7fc00000u, 0x7fc00000u, "QNaN"},
	    {0x7f800001u, 0x7fc00001u, "SNaN"},        {0xffc12345u, 0xffc12345u, "-QNaN payload"},
	};
	for (const auto& c: cases) {
		const uint32_t in[4] {c.in, c.in, c.in, c.in};
		const auto     t = RunRsqrt(trampoline, in, 0x1f80u);
		const auto     e = RunRsqrt(emulated, in, 0x1f80u);
		const auto     h = RunRsqrt(host, in, 0x1f80u);
		std::printf("    %-14s in=%08x replacement=%08x emulator=%08x host=%08x expected=%08x\n", c.name, c.in,
		            t.lanes[0], e.lanes[0], h.lanes[0], c.out);
		Check(t.lanes[0] == c.out, c.name);
	}
	Unmap(trampoline);
	Unmap(emulated);
	Unmap(host);
}

// ---- KYTY_AMD_PATCH selection ----------------------------------------------------------------------------

void TestPatchKinds() {
	const Bytes blob = {
	    0x66, 0x0f, 0x78, 0xc2, 0x28, 0x00,             // extrq xmm2, 40, 0
	    0xc5, 0xf8, 0x52, 0xc8,                         // vrsqrtps xmm1, xmm0
	    0xf3, 0x0f, 0xc7, 0xf8,                         // rdpid rax
	    0x66, 0x0f, 0xae, 0x36,                         // clwb (%rsi)
	    0xc3,
	};
	const struct { uint32_t kinds; uint64_t extrq, rsqrt, rdpid, clwb; const char* name; } cases[] = {
	    {Common::AmdPatchAll, 1, 1, 1, 1, "all"},
	    {0, 0, 0, 0, 0, "none"},
	    {Common::AmdPatchSse4a, 1, 0, 0, 0, "sse4a"},
	    {Common::AmdPatchRsqrt, 0, 1, 0, 0, "rsqrt"},
	    {Common::AmdPatchCpuExt, 0, 0, 1, 1, "cpuext"},
	    {Common::AmdPatchSse4a | Common::AmdPatchRsqrt, 1, 1, 0, 0, "sse4a,rsqrt"},
	};
	for (const auto& c: cases) {
		const auto mapped = MapPatched(blob, true, c.kinds);
		const auto& r     = mapped.result;
		std::printf("  KYTY_AMD_PATCH=%-12s extrq=%llu vrsqrtps=%llu rdpid=%llu clwb=%llu\n", c.name,
		            (unsigned long long)r.extrq.found, (unsigned long long)r.reciprocal_sqrt.found,
		            (unsigned long long)r.rdpid.found, (unsigned long long)r.clwb.found);
		Check(r.extrq.found == c.extrq, c.name);
		Check(r.reciprocal_sqrt.found == c.rsqrt, c.name);
		Check(r.rdpid.found == c.rdpid, c.name);
		Check(r.clwb.found == c.clwb, c.name);
		if (c.kinds == 0) Check(std::memcmp(mapped.area, blob.data(), blob.size()) == 0, "kinds=none leaves the code untouched");
		Unmap(mapped);
	}
}

} // namespace

int main() {
	Check(Common::HostException::InstallHandler(Handler), "install host exception handler");
	std::puts("amdTrampolineBitExactTests: patch kinds");
	TestPatchKinds();
	std::puts("amdTrampolineBitExactTests: EXTRQ / INSERTQ");
	TestBitFields();
	TestExtrqPreservesState();
	std::puts("amdTrampolineBitExactTests: VRSQRTPS");
	TestReciprocalSquareRoot();
	std::puts("amdTrampolineBitExactTests: all passed");
	return 0;
}
