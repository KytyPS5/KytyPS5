// The load-time instruction patcher scans a module's functions on several threads, then patches the
// functions with rewrite sites in address order. Its output must be the same as a single-threaded
// pass: same code bytes, same trampolines, same counters.
#include "loader/guestInstructionPatcher.h"

#include <sys/mman.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "GuestInstructionPatchScanTests: %s\n", message);
		g_failures++;
	}
}

constexpr size_t FunctionSize    = 32;
constexpr size_t FunctionCount   = 2048;
constexpr size_t CodeSize        = FunctionSize * FunctionCount;
constexpr size_t TrampolineSize  = 1024 * 1024;
constexpr size_t SwitchFunction  = 1009;  // and its jump table in function 1010
constexpr size_t RedZoneFunction = 1013;

// 2048 small functions; every 7th uses VRSQRTPS, every 11th EXTRQ, every 13th INSERTQ, every 17th
// MFENCE (CLWB's opcode, register form: filtered out), every 19th RDTSC and every 23rd RDTSCP.
std::vector<uint8_t> MakeCode() {
	std::vector<uint8_t> code(CodeSize, 0xcc);
	for (size_t i = 0; i < FunctionCount; ++i) {
		uint8_t*             f = code.data() + i * FunctionSize;
		std::vector<uint8_t> body;
		if (i % 7 == 0) {
			body = {0xc5, 0xf8, 0x52, 0xc1, 0x01, 0xc8, 0xc3};  // vrsqrtps xmm0, xmm1; add eax, ecx; ret
		} else if (i % 11 == 0) {
			body = {0x66, 0x0f, 0x78, 0xc0, 0x04, 0x08, 0xc3};  // extrq xmm0, 4, 8; ret
		} else if (i % 13 == 0) {
			body = {0x90, 0xf2, 0x0f, 0x79, 0xc1, 0xc3};        // nop; insertq xmm0, xmm1; ret
		} else if (i % 17 == 0) {
			body = {0x0f, 0xae, 0xf0, 0xc3};                    // mfence; ret (0F AE /6, no site)
		} else if (i % 19 == 0) {
			body = {0x0f, 0x31, 0x01, 0xc8, 0xc3};              // rdtsc; add eax, ecx; ret
		} else if (i % 23 == 0) {
			body = {0x0f, 0x01, 0xf9, 0x01, 0xc8, 0xc3};        // rdtscp; add eax, ecx; ret
		} else {
			body = {0xb8, static_cast<uint8_t>(i), static_cast<uint8_t>(i >> 8), 0x00, 0x00, 0xc3};  // mov eax, i; ret
		}
		std::memcpy(f, body.data(), body.size());
	}
	// A switch whose jump table lies in the next function's bytes; its VRSQRTPS is reachable only
	// through the table (the parallel pass decodes such a function again in address order).
	const std::vector<uint8_t> switch_body {
	    0x83, 0xff, 0x01,                          // cmp edi, 1
	    0x77, 0x15,                                // ja default
	    0x48, 0x8d, 0x0d, 0x14, 0x00, 0x00, 0x00,  // lea rcx, [rip + table] (the next function)
	    0x48, 0x63, 0x04, 0xb9,                    // movsxd rax, dword [rcx + rdi*4]
	    0x48, 0x01, 0xc8,                          // add rax, rcx
	    0xff, 0xe0,                                // jmp rax
	    0xc5, 0xf8, 0x52, 0xc1,                    // case 0: vrsqrtps xmm0, xmm1
	    0xc3,                                      // ret
	    0xc3};                                     // case 1, default: ret
	const std::vector<uint8_t> table {0xf5, 0xff, 0xff, 0xff, 0xfa, 0xff, 0xff, 0xff};  // -11, -6
	std::memcpy(code.data() + SwitchFunction * FunctionSize, switch_body.data(), switch_body.size());
	std::memset(code.data() + (SwitchFunction + 1) * FunctionSize, 0xcc, FunctionSize);
	std::memcpy(code.data() + (SwitchFunction + 1) * FunctionSize, table.data(), table.size());
	// Red-zone stores and loads, for the red-zone protection pass.
	const std::vector<uint8_t> red_zone_body {0x48, 0x89, 0x44, 0x24, 0xf8,   // mov [rsp - 8], rax
	                                          0x48, 0x8b, 0x4c, 0x24, 0xf8,   // mov rcx, [rsp - 8]
	                                          0xc3};                          // ret
	std::memset(code.data() + RedZoneFunction * FunctionSize, 0xcc, FunctionSize);
	std::memcpy(code.data() + RedZoneFunction * FunctionSize, red_zone_body.data(), red_zone_body.size());
	return code;
}

struct Run {
	std::vector<uint8_t>                code;
	std::vector<uint8_t>                trampolines;
	Loader::GuestInstructionPatchResult result {};
};

Run Patch(uint8_t* area, const std::vector<uint8_t>& code, size_t threads, bool protect_memory,
          bool emulate_amd, uint64_t tsc_multiplier) {
	std::memset(area, 0xcc, CodeSize + TrampolineSize);
	std::memcpy(area, code.data(), code.size());
	Loader::RegisterGuestInstructionPatchModule(area, CodeSize, area + CodeSize, TrampolineSize);
	std::vector<uintptr_t> starts;
	for (size_t i = 0; i < FunctionCount; ++i) {
		starts.push_back(reinterpret_cast<uintptr_t>(area) + i * FunctionSize);
	}
	Loader::SetGuestInstructionPatchScanThreads(threads);
	const Loader::GuestInstructionHostFeatures intel {false, false, false};
	Run run;
	run.result = Loader::PatchGuestInstructions(reinterpret_cast<uintptr_t>(area), CodeSize, starts,
	                                            protect_memory, emulate_amd, intel, tsc_multiplier);
	Loader::SetGuestInstructionPatchScanThreads(0);
	Loader::UnregisterGuestInstructionPatchModule(area);
	run.code.assign(area, area + CodeSize);
	run.trampolines.assign(area + CodeSize, area + CodeSize + TrampolineSize);
	return run;
}

bool SameCounts(const Loader::InstructionPatchCounts& a, const Loader::InstructionPatchCounts& b) {
	return a.found == b.found && a.native == b.native && a.trapped == b.trapped;
}

bool SameResult(const Loader::GuestInstructionPatchResult& a, const Loader::GuestInstructionPatchResult& b) {
	return a.function_count == b.function_count && a.instruction_count == b.instruction_count &&
	       a.red_zone_function_count == b.red_zone_function_count &&
	       a.memory_instruction_count == b.memory_instruction_count &&
	       a.short_memory_instruction_count == b.short_memory_instruction_count &&
	       a.patched_memory_instruction_count == b.patched_memory_instruction_count &&
	       a.stack_dependent_memory_instruction_count == b.stack_dependent_memory_instruction_count &&
	       a.control_flow_memory_instruction_count == b.control_flow_memory_instruction_count &&
	       a.unrelocatable_memory_instruction_count == b.unrelocatable_memory_instruction_count &&
	       a.indirect_red_zone_function_count == b.indirect_red_zone_function_count &&
	       SameCounts(a.reciprocal_sqrt, b.reciprocal_sqrt) && SameCounts(a.extrq, b.extrq) &&
	       SameCounts(a.insertq, b.insertq) && SameCounts(a.rdpid, b.rdpid) && SameCounts(a.clwb, b.clwb) &&
	       SameCounts(a.rdtsc, b.rdtsc);
}

void CheckByteFilter() {
	struct Case {
		std::vector<uint8_t> bytes;
		bool                 match;
		const char*          name;
	};
	const std::array<Case, 19> cases {{
	    {{0xc5, 0xf8, 0x52, 0xc1}, true, "vrsqrtps xmm0, xmm1"},
	    {{0xc4, 0x41, 0x78, 0x52, 0xc1}, true, "vrsqrtps xmm8, xmm9 (3-byte VEX)"},
	    {{0x66, 0x0f, 0x78, 0xc0, 0x04, 0x08}, true, "extrq xmm0, 4, 8"},
	    {{0x66, 0x41, 0x0f, 0x79, 0xc1}, true, "extrq xmm0, xmm9 (REX)"},
	    {{0xf2, 0x0f, 0x79, 0xc1}, true, "insertq xmm0, xmm1"},
	    {{0xf2, 0x0f, 0x78, 0xc1, 0x04, 0x08}, true, "insertq xmm0, xmm1, 4, 8"},
	    {{0xf3, 0x0f, 0xc7, 0xf8}, true, "rdpid rax"},
	    {{0x66, 0x0f, 0xae, 0x30}, true, "clwb [rax]"},
	    {{0x66, 0x41, 0x0f, 0xae, 0x74, 0x24, 0x08}, true, "clwb [r12+8] (REX, SIB)"},
	    {{0xc5, 0xf8, 0x52, 0x00}, false, "vrsqrtps xmm0, [rax] (memory form, not rewritten)"},
	    {{0xc5, 0xfc, 0x52, 0xc1}, false, "vrsqrtps ymm0, ymm1 (VEX.256, not rewritten)"},
	    {{0xc5, 0xfa, 0x52, 0xc1}, false, "vrsqrtss xmm0, xmm0, xmm1"},
	    {{0x0f, 0xae, 0xf0}, false, "mfence"},
	    {{0x0f, 0xae, 0xe8}, false, "lfence"},
	    {{0x0f, 0xc7, 0xf0}, false, "rdrand eax"},
	    {{0x52, 0xc3}, false, "push rdx; ret"},
	    {{0xb8, 0x52, 0x00, 0x00, 0x00}, false, "mov eax, 0x52"},
	    {{0x90, 0xc5, 0xf8, 0x52}, false, "VEX instruction cut off by the end"},
	    {{0x0f}, false, "escape byte at the end"},
	}};
	for (const auto& c: cases) {
		const bool match = Loader::MayContainAmdInstruction(c.bytes.data(), c.bytes.data() + c.bytes.size());
		if (match != c.match) {
			std::fprintf(stderr, "GuestInstructionPatchScanTests: byte filter %s for %s\n",
			             match ? "matches" : "misses", c.name);
			g_failures++;
		}
	}
}
} // namespace

int main() {
	auto* area = static_cast<uint8_t*>(mmap(nullptr, CodeSize + TrampolineSize, PROT_READ | PROT_WRITE | PROT_EXEC,
	                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	Check(area != MAP_FAILED, "allocate the patch area");
	if (area == MAP_FAILED) {
		return 1;
	}
	CheckByteFilter();
	const auto code   = MakeCode();
	size_t extrq = 0, insertq = 0, rdtsc = 0;
	for (size_t i = 0; i < FunctionCount; ++i) {
		extrq += i % 7 != 0 && i % 11 == 0;
		insertq += i % 7 != 0 && i % 11 != 0 && i % 13 == 0;
		rdtsc += i % 7 != 0 && i % 11 != 0 && i % 13 != 0 && i % 17 != 0 && (i % 19 == 0 || i % 23 == 0);
	}
	// AMD instructions only, AMD instructions and RDTSC/RDTSCP at a guest rate, RDTSC/RDTSCP only, and
	// red-zone protection with AMD instructions.
	constexpr uint64_t GuestRate = 0x9000'0000ull;
	struct Mode {
		bool     protect_memory;
		bool     emulate_amd;
		uint64_t tsc_multiplier;
	};
	const std::array<Mode, 4> modes {{{false, true, 0}, {false, true, GuestRate}, {false, false, GuestRate}, {true, true, 0}}};
	Run                       serial;
	for (const auto& [protect_memory, emulate_amd, tsc_multiplier]: modes) {
		serial = Patch(area, code, 1, protect_memory, emulate_amd, tsc_multiplier);
		Check(serial.code != code, "the serial pass patches the code");
		Check(serial.result.rdtsc.found == (tsc_multiplier != 0 ? rdtsc : 0), "every RDTSC/RDTSCP is found");
		if (emulate_amd) {
			Check(serial.result.function_count == FunctionCount, "every function is scanned");
			Check(serial.result.reciprocal_sqrt.found == (FunctionCount + 6) / 7 + 1,
			      "every VRSQRTPS is found, also behind a jump table in another function");
			Check(serial.result.extrq.found == extrq && serial.result.insertq.found == insertq,
			      "every EXTRQ and INSERTQ is found");
		} else {
			Check(serial.result.function_count == rdtsc, "only functions with RDTSC/RDTSCP are scanned");
		}
		if (protect_memory) {
			Check(serial.result.red_zone_function_count != 0, "the red-zone function is analyzed");
		}
		for (const size_t threads: std::array<size_t, 3> {2, 5, 16}) {
			const Run parallel = Patch(area, code, threads, protect_memory, emulate_amd, tsc_multiplier);
			Check(parallel.code == serial.code, "parallel scan: same patched code");
			Check(parallel.trampolines == serial.trampolines, "parallel scan: same trampolines");
			Check(SameResult(parallel.result, serial.result), "parallel scan: same counters");
		}
	}
	munmap(area, CodeSize + TrampolineSize);
	if (g_failures != 0) {
		std::fprintf(stderr, "GuestInstructionPatchScanTests: %d checks failed\n", g_failures);
		return 1;
	}
	std::printf("GuestInstructionPatchScanTests: all passed (%zu functions, VRSQRTPS, EXTRQ, INSERTQ and %zu RDTSC/RDTSCP sites)\n",
	            FunctionCount, rdtsc);
	return 0;
}
