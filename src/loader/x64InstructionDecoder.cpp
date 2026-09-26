#include "loader/x64InstructionDecoder.h"

#include <Zydis/Zydis.h>

#include <array>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>

namespace Loader {

namespace {

// One decoder, shared with the rest of the loader the same way redZonePatcher keeps its own.
// ZydisDecoderInit only fills a plain struct, so constructing it lazily from the fault reporter
// is cheap and cannot allocate.
ZydisDecoder& GuestDecoder() {
	static ZydisDecoder decoder = [] {
		ZydisDecoder value {};
		ZydisDecoderInit(&value, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
		return value;
	}();
	return decoder;
}

// Read a general purpose guest register. Anything else (segments, flags, SIMD) is not addressable
// through a legacy memory operand, so reporting 0 for it cannot make a real fault address wrong.
uint64_t ReadGuestRegister(ZydisRegister reg, const GuestX64Registers& registers) {
	const auto wide =
	    ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
	switch (wide) {
		case ZYDIS_REGISTER_RAX: return registers.rax;
		case ZYDIS_REGISTER_RBX: return registers.rbx;
		case ZYDIS_REGISTER_RCX: return registers.rcx;
		case ZYDIS_REGISTER_RDX: return registers.rdx;
		case ZYDIS_REGISTER_RSI: return registers.rsi;
		case ZYDIS_REGISTER_RDI: return registers.rdi;
		case ZYDIS_REGISTER_RBP: return registers.rbp;
		case ZYDIS_REGISTER_RSP: return registers.rsp;
		case ZYDIS_REGISTER_R8: return registers.r8;
		case ZYDIS_REGISTER_R9: return registers.r9;
		case ZYDIS_REGISTER_R10: return registers.r10;
		case ZYDIS_REGISTER_R11: return registers.r11;
		case ZYDIS_REGISTER_R12: return registers.r12;
		case ZYDIS_REGISTER_R13: return registers.r13;
		case ZYDIS_REGISTER_R14: return registers.r14;
		case ZYDIS_REGISTER_R15: return registers.r15;
		case ZYDIS_REGISTER_RIP: return registers.rip;
		default: return 0;
	}
}

bool IsGeneralPurposeRegister(ZydisRegister reg) {
	switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg)) {
		case ZYDIS_REGISTER_RAX:
		case ZYDIS_REGISTER_RBX:
		case ZYDIS_REGISTER_RCX:
		case ZYDIS_REGISTER_RDX:
		case ZYDIS_REGISTER_RSI:
		case ZYDIS_REGISTER_RDI:
		case ZYDIS_REGISTER_RBP:
		case ZYDIS_REGISTER_RSP:
		case ZYDIS_REGISTER_R8:
		case ZYDIS_REGISTER_R9:
		case ZYDIS_REGISTER_R10:
		case ZYDIS_REGISTER_R11:
		case ZYDIS_REGISTER_R12:
		case ZYDIS_REGISTER_R13:
		case ZYDIS_REGISTER_R14:
		case ZYDIS_REGISTER_R15:
		case ZYDIS_REGISTER_RIP: return true;
		default: return false;
	}
}

// ZydisCalcAbsoluteAddress only covers relative immediates and RIP-relative or absolute memory, so
// a base+index*scale+disp operand has to be resolved from the register file by hand.
uint64_t ResolveMemoryOperand(const ZydisDecodedInstruction& instruction,
                              const ZydisDecodedOperand&         operand,
                              const GuestX64Registers&            registers) {
	// RIP-relative operands are already anchored past the end of the instruction.
	const auto base =
	    operand.mem.base == ZYDIS_REGISTER_RIP
	        ? registers.rip + instruction.length
	        : ReadGuestRegister(operand.mem.base, registers);
	uint64_t index = 0;
	if (operand.mem.index != ZYDIS_REGISTER_NONE &&
	    operand.mem.index != ZYDIS_REGISTER_RIP) {
		index = ReadGuestRegister(operand.mem.index, registers);
	}
	return base + index * static_cast<uint64_t>(operand.mem.scale) +
	       static_cast<uint64_t>(operand.mem.disp.value);
}

// The AT&T-style width prefix, so a memory operand keeps the size a reader needs. Without it
// "[0x1000]" is ambiguous between a byte load and a 16-byte one.
const char* OperandWidthName(uint16_t bits) {
	switch (bits) {
		case 8: return "byte ptr ";
		case 16: return "word ptr ";
		case 32: return "dword ptr ";
		case 64: return "qword ptr ";
		case 128: return "xmmword ptr ";
		case 256: return "ymmword ptr ";
		case 512: return "zmmword ptr ";
		default: return "";
	}
}

// Bounded append into a fixed buffer: this runs on the fatal path, where a truncation is fine but
// an overrun is not.
size_t Append(char* text, size_t capacity, size_t used, const char* format, ...) {
	if (used >= capacity) {
		return used;
	}
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(text + used, capacity - used, format, args);
	va_end(args);
	if (written < 0) {
		return used;
	}
	const auto advance = static_cast<size_t>(written);
	return advance < capacity - used ? used + advance : capacity - 1;
}

} // namespace

GuestFaultingInstruction DecodeGuestFaultingInstruction(const uint8_t* code, size_t available,
                                                        const GuestX64Registers& registers) {
	GuestFaultingInstruction result;
	if (code == nullptr || available == 0) {
		std::snprintf(result.text, sizeof(result.text), "<no readable instruction bytes>");
		return result;
	}

	ZydisDecodedInstruction instruction {};
	std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands {};
	const auto status = ZydisDecoderDecodeFull(&GuestDecoder(), code, available, &instruction,
	                                           operands.data());
	if (!ZYAN_SUCCESS(status) || instruction.length == 0) {
		std::snprintf(result.text, sizeof(result.text), "<undecodable instruction>");
		return result;
	}

	result.decoded = true;
	result.length  = instruction.length;

	size_t used = 0;
	used = Append(result.text, sizeof(result.text), used, "%s",
	              ZydisMnemonicGetString(instruction.mnemonic));

	// LEA and the padding NOPs name an address without touching it, so they must not be reported
	// as the memory access that faulted.
	const bool computes_address_only = instruction.mnemonic == ZYDIS_MNEMONIC_LEA ||
	                                   instruction.mnemonic == ZYDIS_MNEMONIC_NOP;
	constexpr ZydisOperandActions memory_access =
	    ZYDIS_OPERAND_ACTION_MASK_READ | ZYDIS_OPERAND_ACTION_MASK_WRITE;

	for (uint8_t index = 0; index < instruction.operand_count_visible; index++) {
		const auto& operand = operands[index];
		used = Append(result.text, sizeof(result.text), used, index == 0 ? " " : ", ");

		switch (operand.type) {
			case ZYDIS_OPERAND_TYPE_REGISTER:
				used = Append(result.text, sizeof(result.text), used, "%s",
				              ZydisRegisterGetString(operand.reg.value));
				break;

			case ZYDIS_OPERAND_TYPE_IMMEDIATE: {
				if (operand.imm.is_relative) {
					uint64_t target = 0;
					if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&instruction, &operand,
					                                          registers.rip, &target))) {
						used = Append(result.text, sizeof(result.text), used,
						              "0x%016" PRIx64, target);
						break;
					}
				}
				used = Append(result.text, sizeof(result.text), used, "0x%" PRIx64,
				              operand.imm.value.u);
			} break;

			case ZYDIS_OPERAND_TYPE_MEMORY: {
				const uint64_t address = ResolveMemoryOperand(instruction, operand, registers);
				const bool     resolved =
				    IsGeneralPurposeRegister(operand.mem.base) &&
				    (operand.mem.index == ZYDIS_REGISTER_NONE ||
				     IsGeneralPurposeRegister(operand.mem.index));
				used = Append(result.text, sizeof(result.text), used, "%s[0x%016" PRIx64 "]%s",
				              OperandWidthName(operand.size), address, resolved ? "" : " unresolved");
				if (!computes_address_only && (operand.actions & memory_access) != 0) {
					result.reads_memory |= (operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0;
					result.writes_memory |=
					    (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0;
					if (!result.reads_memory && !result.writes_memory) {
						// Some encodings mark an operand as read-write as a whole rather than
						// per direction; treat that as both so the address is still reported.
						result.reads_memory  = true;
						result.writes_memory = true;
					}
					result.effective_address = address;
				}
			} break;

			case ZYDIS_OPERAND_TYPE_POINTER:
				used = Append(result.text, sizeof(result.text), used, "0x%04" PRIx16 ":0x%08" PRIx32,
				              operand.ptr.segment, operand.ptr.offset);
				break;

			default: used = Append(result.text, sizeof(result.text), used, "?"); break;
		}
	}
	result.text[sizeof(result.text) - 1] = '\0';
	return result;
}

} // namespace Loader
