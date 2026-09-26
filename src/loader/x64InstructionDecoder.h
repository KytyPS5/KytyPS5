#ifndef KYTY_LOADER_X64_INSTRUCTION_DECODER_H_
#define KYTY_LOADER_X64_INSTRUCTION_DECODER_H_

#include <cstddef>
#include <cstdint>

namespace Loader {

// The general purpose guest register file, in the order the fault reporter already prints it.
struct GuestX64Registers {
	uint64_t rip = 0;
	uint64_t rax = 0;
	uint64_t rbx = 0;
	uint64_t rcx = 0;
	uint64_t rdx = 0;
	uint64_t rsi = 0;
	uint64_t rdi = 0;
	uint64_t rbp = 0;
	uint64_t rsp = 0;
	uint64_t r8  = 0;
	uint64_t r9  = 0;
	uint64_t r10 = 0;
	uint64_t r11 = 0;
	uint64_t r12 = 0;
	uint64_t r13 = 0;
	uint64_t r14 = 0;
	uint64_t r15 = 0;
};

// Everything the fatal fault reporter learned about the instruction it stopped on.
struct GuestFaultingInstruction {
	// False when the bytes could not be decoded, e.g. an empty or truncated window. The text
	// then says so rather than describing a guess.
	bool     decoded = false;
	uint32_t length = 0;
	// True when the instruction actually touches memory. LEA and NOP name an address without
	// reading it, so neither sets this.
	bool     reads_memory = false;
	bool     writes_memory = false;
	// The resolved address of the first memory operand, valid when reads_memory or
	// writes_memory is set. This is the number a reader of a crash log actually wants: not
	// "[rbx-0x1]" but the address that register combination pointed at.
	uint64_t effective_address = 0;
	// "mov rbx, qword ptr [0x00000000ffffffff]", or a note when the instruction was not decoded.
	char     text[112] = {};
};

// Decode the instruction at `code` and render it, resolving any memory operand to its effective
// address from `registers`. `available` bounds the bytes that may be read, so this can be handed a
// window it is not allowed to run past. It allocates nothing and never traps, because it runs from
// the fatal fault reporter.
GuestFaultingInstruction DecodeGuestFaultingInstruction(const uint8_t* code, size_t available,
                                                        const GuestX64Registers& registers);

} // namespace Loader

#endif /* KYTY_LOADER_X64_INSTRUCTION_DECODER_H_ */
