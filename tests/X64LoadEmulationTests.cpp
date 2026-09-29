// Loads from a no-access page, completed by X64InstructionEmulator::TryEmulateLoad in a vectored
// exception handler, must leave the same registers and flags as the same instruction on readable
// memory. This is the path the emulator takes for guest reads of GPU-clean bytes on protected pages.

#include "loader/x64InstructionEmulator.h"

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {

uint8_t* g_guarded = nullptr; // no access: every load faults
uint8_t* g_shadow  = nullptr; // the bytes the guarded page stands for
uint32_t g_emulated = 0;
uint32_t g_declined = 0;

bool ReadShadow(uint64_t /*fault_vaddr*/, uint64_t vaddr, void* data, uint64_t size) {
	const auto base = reinterpret_cast<uint64_t>(g_guarded);
	if (vaddr < base || vaddr + size > base + 4096) {
		return false;
	}
	std::memcpy(data, g_shadow + (vaddr - base), size);
	return true;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* info) {
	const auto* record = info->ExceptionRecord;
	if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || record->NumberParameters < 2) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	const auto address = static_cast<uint64_t>(record->ExceptionInformation[1]);
	const auto base    = reinterpret_cast<uint64_t>(g_guarded);
	if (address < base || address >= base + 4096) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	if (record->ExceptionInformation[0] == 0 &&
	    Loader::X64InstructionEmulator::TryEmulateLoad(info->ContextRecord, address, ReadShadow)) {
		g_emulated++;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	// Not emulated: let the instruction run on readable memory, as the slow path would.
	g_declined++;
	DWORD old = 0;
	VirtualProtect(g_guarded, 4096, PAGE_READWRITE, &old);
	std::memcpy(g_guarded, g_shadow, 4096);
	return EXCEPTION_CONTINUE_EXECUTION;
}

void Reguard() {
	DWORD old = 0;
	VirtualProtect(g_guarded, 4096, PAGE_NOACCESS, &old);
}

struct Result {
	uint64_t gpr[2] {};
	uint64_t flags = 0;
	alignas(32) uint8_t vec[32] {};
};

constexpr uint64_t ArithmeticFlags = 0x8d5; // OF SF ZF AF PF CF
constexpr uint64_t NoAdjustFlags   = 0x8c5; // AF is undefined after TEST

using Case = void (*)(const uint8_t* p, Result& r);

// Each case starts from known register contents, so merge rules (8/16-bit writes keep the rest,
// 32-bit writes clear the upper half, VEX clears the upper YMM) are visible in the result.
#define GPR_CASE(name, insn)                                                                   \
	void name(const uint8_t* p, Result& r) {                                                   \
		uint64_t a = 0x1111111111111111ull;                                                    \
		asm volatile(insn : "+a"(a) : "S"(p) : "memory", "cc");                                \
		r.gpr[0] = a;                                                                          \
	}

GPR_CASE(MovzxWord, "movzwl 12(%%rsi), %%eax")
GPR_CASE(MovzxByte, "movzbl 5(%%rsi), %%eax")
GPR_CASE(MovsxByteToQ, "movsbq 5(%%rsi), %%rax")
GPR_CASE(MovsxWordToL, "movswl 2(%%rsi), %%eax")
GPR_CASE(MovsxdToQ, "movslq 8(%%rsi), %%rax")
GPR_CASE(MovByte, "movb 7(%%rsi), %%al")
GPR_CASE(MovHighByte, "movb 7(%%rsi), %%ah")
GPR_CASE(MovWord, "movw 6(%%rsi), %%ax")
GPR_CASE(MovDword, "movl 4(%%rsi), %%eax")
GPR_CASE(MovQword, "movq 8(%%rsi), %%rax")
GPR_CASE(MovQwordIndexed, "movq $3, %%rax\n\tmovq 8(%%rsi,%%rax,8), %%rax")

#define FLAGS_CASE(name, insn, value)                                                          \
	void name(const uint8_t* p, Result& r) {                                                   \
		uint64_t flags = 0;                                                                    \
		uint64_t b     = value;                                                                \
		asm volatile(insn "\n\tpushfq\n\tpopq %0" : "=r"(flags) : "S"(p), "d"(b) : "memory", "cc"); \
		r.flags = flags;                                                                       \
	}

FLAGS_CASE(CmpQwordEqual, "cmpq %%rdx, 16(%%rsi)", 0x8877665544332211ull)
FLAGS_CASE(CmpQwordBelow, "cmpq %%rdx, 16(%%rsi)", 0xffffffffffffffffull)
FLAGS_CASE(CmpQwordAbove, "cmpq %%rdx, 16(%%rsi)", 1)
FLAGS_CASE(CmpQwordOverflow, "cmpq %%rdx, 24(%%rsi)", 0x8000000000000000ull)
FLAGS_CASE(CmpRegFirst, "cmpq 16(%%rsi), %%rdx", 5)
FLAGS_CASE(CmpDwordImm8, "cmpl $-1, 4(%%rsi)", 0)
FLAGS_CASE(CmpByteImm, "cmpb $0x7f, 7(%%rsi)", 0)
FLAGS_CASE(CmpWordReg, "cmpw %%dx, 6(%%rsi)", 0x1234)
FLAGS_CASE(CmpByteAdjust, "cmpb %%dl, 0(%%rsi)", 0x1f)
FLAGS_CASE(TestByteImm, "testb $0x80, 7(%%rsi)", 0)
FLAGS_CASE(TestDwordReg, "testl %%edx, 4(%%rsi)", 0x0f0f0f0f)
FLAGS_CASE(TestQwordZero, "testq %%rdx, 16(%%rsi)", 0)

#define VEC_CASE(name, prepare, insn, store)                                                   \
	void name(const uint8_t* p, Result& r) {                                                   \
		static const uint8_t ones[32] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,       \
		                                 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,       \
		                                 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,       \
		                                 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};      \
		asm volatile(prepare "\n\t" insn "\n\t" store                                          \
		             :                                                                         \
		             : "S"(p), "D"(r.vec), "d"(ones)                                          \
		             : "memory", "xmm3", "xmm9");                                              \
	}

VEC_CASE(Movups, "vmovdqu (%%rdx), %%ymm3", "movups 32(%%rsi), %%xmm3", "vmovdqu %%ymm3, (%%rdi)")
VEC_CASE(Movss, "vmovdqu (%%rdx), %%ymm3", "movss 36(%%rsi), %%xmm3", "vmovdqu %%ymm3, (%%rdi)")
VEC_CASE(Movsd, "vmovdqu (%%rdx), %%ymm3", "movsd 40(%%rsi), %%xmm3", "vmovdqu %%ymm3, (%%rdi)")
VEC_CASE(Movq, "vmovdqu (%%rdx), %%ymm3", "movq 40(%%rsi), %%xmm3", "vmovdqu %%ymm3, (%%rdi)")
VEC_CASE(Movd, "vmovdqu (%%rdx), %%ymm3", "movd 44(%%rsi), %%xmm3", "vmovdqu %%ymm3, (%%rdi)")
VEC_CASE(Vmovups128, "vmovdqu (%%rdx), %%ymm9", "vmovups 32(%%rsi), %%xmm9",
         "vmovdqu %%ymm9, (%%rdi)")
VEC_CASE(Vmovups256, "vmovdqu (%%rdx), %%ymm9", "vmovups 32(%%rsi), %%ymm9",
         "vmovdqu %%ymm9, (%%rdi)")
VEC_CASE(Vmovss, "vmovdqu (%%rdx), %%ymm3", "vmovss 36(%%rsi), %%xmm3", "vmovdqu %%ymm3, (%%rdi)")
VEC_CASE(Vmovq, "vmovdqu (%%rdx), %%ymm3", "vmovq 40(%%rsi), %%xmm3", "vmovdqu %%ymm3, (%%rdi)")

struct NamedCase {
	const char* name;
	Case        run;
	uint64_t    flag_mask;
	bool        emulated; // whether TryEmulateLoad must handle it (rather than decline)
};

// A store and a read-modify-write must be declined: they are not pure loads.
void AddToMemory(const uint8_t* p, Result& r) {
	uint64_t a = 1;
	asm volatile("addq %0, 48(%%rsi)" : : "r"(a), "S"(p) : "memory", "cc");
	r.gpr[0] = 0;
}

} // namespace

int main() {
	auto* pages = static_cast<uint8_t*>(
	    VirtualAlloc(nullptr, 3 * 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
	if (pages == nullptr) {
		std::printf("VirtualAlloc failed\n");
		return 1;
	}
	g_guarded     = pages;
	g_shadow      = pages + 4096;
	auto* plain   = pages + 2 * 4096;
	for (int i = 0; i < 4096; i++) {
		g_shadow[i] = static_cast<uint8_t>(i * 37 + 11);
	}
	const uint64_t q16 = 0x8877665544332211ull;
	const uint64_t q24 = 0x0000000000000001ull;
	std::memcpy(g_shadow + 16, &q16, 8);
	std::memcpy(g_shadow + 24, &q24, 8);
	g_shadow[0] = 0x10;
	g_shadow[7] = 0x80;
	std::memcpy(plain, g_shadow, 4096);
	AddVectoredExceptionHandler(1, Handler);

	const NamedCase cases[] = {
	    {"movzx word", MovzxWord, 0, true},
	    {"movzx byte", MovzxByte, 0, true},
	    {"movsx byte to qword", MovsxByteToQ, 0, true},
	    {"movsx word to dword", MovsxWordToL, 0, true},
	    {"movsxd", MovsxdToQ, 0, true},
	    {"mov al", MovByte, 0, true},
	    {"mov ah", MovHighByte, 0, true},
	    {"mov ax", MovWord, 0, true},
	    {"mov eax", MovDword, 0, true},
	    {"mov rax", MovQword, 0, true},
	    {"mov rax indexed", MovQwordIndexed, 0, true},
	    {"cmp qword equal", CmpQwordEqual, ArithmeticFlags, true},
	    {"cmp qword below", CmpQwordBelow, ArithmeticFlags, true},
	    {"cmp qword above", CmpQwordAbove, ArithmeticFlags, true},
	    {"cmp qword overflow", CmpQwordOverflow, ArithmeticFlags, true},
	    {"cmp register first", CmpRegFirst, ArithmeticFlags, true},
	    {"cmp dword imm8", CmpDwordImm8, ArithmeticFlags, true},
	    {"cmp byte imm", CmpByteImm, ArithmeticFlags, true},
	    {"cmp word reg", CmpWordReg, ArithmeticFlags, true},
	    {"cmp byte adjust", CmpByteAdjust, ArithmeticFlags, true},
	    {"test byte imm", TestByteImm, NoAdjustFlags, true},
	    {"test dword reg", TestDwordReg, NoAdjustFlags, true},
	    {"test qword zero", TestQwordZero, NoAdjustFlags, true},
	    {"movups", Movups, 0, true},
	    {"movss", Movss, 0, true},
	    {"movsd", Movsd, 0, true},
	    {"movq xmm", Movq, 0, true},
	    {"movd xmm", Movd, 0, true},
	    {"vmovups xmm", Vmovups128, 0, true},
	    {"vmovups ymm", Vmovups256, 0, true},
	    {"vmovss", Vmovss, 0, true},
	    {"vmovq", Vmovq, 0, true},
	    {"add to memory (declined)", AddToMemory, 0, false},
	};

	int failures = 0;
	for (const auto& test: cases) {
		Result expected {};
		Result actual {};
		test.run(plain, expected);
		Reguard();
		const auto emulated_before = g_emulated;
		const auto declined_before = g_declined;
		test.run(g_guarded, actual);
		const bool was_emulated = g_emulated != emulated_before;
		const bool was_declined = g_declined != declined_before;
		bool       ok           = was_emulated == test.emulated && was_declined != test.emulated;
		ok = ok && expected.gpr[0] == actual.gpr[0] &&
		     (expected.flags & test.flag_mask) == (actual.flags & test.flag_mask) &&
		     std::memcmp(expected.vec, actual.vec, sizeof(expected.vec)) == 0;
		if (!ok) {
			failures++;
			std::printf("FAIL %s: emulated=%d declined=%d gpr 0x%016" PRIx64 " vs 0x%016" PRIx64
			            " flags 0x%03" PRIx64 " vs 0x%03" PRIx64 "\n",
			            test.name, was_emulated ? 1 : 0, was_declined ? 1 : 0, actual.gpr[0],
			            expected.gpr[0], actual.flags & test.flag_mask,
			            expected.flags & test.flag_mask);
		}
		// Restore the guarded page's bytes (the declined read-modify-write ran on it).
		DWORD old = 0;
		VirtualProtect(g_guarded, 4096, PAGE_READWRITE, &old);
		std::memcpy(g_guarded, g_shadow, 4096);
		std::memcpy(plain, g_shadow, 4096);
	}
	std::printf("x64 load emulation: %zu cases, %d failures\n", std::size(cases), failures);
	return failures == 0 ? 0 : 1;
}
