// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "loader/guestInstructionPatcher.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/virtualMemory.h"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <array>
#include <bitset>
#include <climits>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>
#if !defined(__APPLE__)
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#endif

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#if !defined(__APPLE__)
using namespace Xbyak::util;
#endif

namespace Loader {

using u8  = uint8_t;
using s8  = int8_t;
using u16 = uint16_t;
using s16 = int16_t;
using u32 = uint32_t;
using s32 = int32_t;
using u64 = uint64_t;
using s64 = int64_t;

#define ASSERT(condition) EXIT_IF(!(condition))

constexpr size_t NearJumpSize = 5;

#if !defined(__APPLE__)

struct PatchModule {
	std::mutex           mutex {};
	u8*                  start = nullptr;
	u8*                  end   = nullptr;
	std::set<u8*>        patched;
	Xbyak::CodeGenerator patch_gen;
	Xbyak::CodeGenerator trampoline_gen;
	bool                 trampoline_exhaustion_reported = false;

	PatchModule(u8* module_ptr, u64 module_size, u8* trampoline_ptr, u64 trampoline_size)
	    : start(module_ptr), end(module_ptr + module_size), patch_gen(module_size, module_ptr),
	      trampoline_gen(trampoline_size, trampoline_ptr) {}
};

static std::map<u64, PatchModule> g_patch_modules;

static PatchModule* GetContainingModule(const void* ptr) {
	auto upper = g_patch_modules.upper_bound(reinterpret_cast<u64>(ptr));
	if (upper == g_patch_modules.begin()) {
		return nullptr;
	}
	auto* module  = &std::prev(upper)->second;
	auto* address = static_cast<const u8*>(ptr);
	return address >= module->start && address < module->end ? module : nullptr;
}

static bool HandleTrampolineError(PatchModule* module, int error) {
	if (error != Xbyak::ERR_CODE_IS_TOO_BIG) {
		return false;
	}
	if (!module->trampoline_exhaustion_reported) {
		LOGF("Guest instruction trampoline space exhausted for module %p\n",
		     static_cast<void*>(module->start));
		module->trampoline_exhaustion_reported = true;
	}
	return true;
}

static ZydisDecoder& GetDecoder() {
	static ZydisDecoder decoder = [] {
		ZydisDecoder value {};
		EXIT_IF(!ZYAN_SUCCESS(
		    ZydisDecoderInit(&value, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)));
		return value;
	}();
	return decoder;
}

namespace {

constexpr size_t GuestRedZoneSize = 128;
constexpr size_t ShortJumpSize    = 2;
using RedZoneMask                 = std::bitset<GuestRedZoneSize>;

struct DecodedCodeInstruction {
	uintptr_t                                                address {};
	ZydisDecodedInstruction                                  instruction {};
	std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands {};
	RedZoneMask                                              red_zone_use {};
	RedZoneMask                                              red_zone_def {};
	RedZoneMask                                              red_zone_live {};
	bool                                                     accesses_memory {};
	bool                                                     uses_stack_pointer {};
	bool                                                     has_red_zone_operand {};
	bool                                                     has_unmodeled_red_zone_operand {};
	bool                                                     changes_stack_pointer {};
	bool                                                     replaces_stack_pointer {};
	std::optional<s64>                                       stack_pointer_delta;
};

struct DecodedFunction {
	std::map<uintptr_t, DecodedCodeInstruction> instructions;
	std::set<uintptr_t>                         branch_targets;
	bool                                        uses_red_zone {};
	bool                                        has_indirect_branch {};
	bool                                        requires_conservative_red_zone_tracking {};
};

enum class InstructionReplacement { None, ReciprocalSquareRoot, ExtractQ };

struct InstructionRewrite {
	bool                   protect_red_zone {};
	bool                   protected_indirect_call {};
	InstructionReplacement replacement {};
};

InstructionPatchCounts& ReplacementCounts(GuestInstructionPatchResult& result,
                                          InstructionReplacement       replacement) {
	ASSERT(replacement != InstructionReplacement::None);
	return replacement == InstructionReplacement::ReciprocalSquareRoot ? result.reciprocal_sqrt
	                                                                   : result.extrq;
}

bool IsStackPointerRegister(ZydisRegister reg) {
	return reg != ZYDIS_REGISTER_NONE &&
	       ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg) == ZYDIS_REGISTER_RSP;
}

bool IsControlFlowTerminator(const ZydisDecodedInstruction& instruction) {
	return instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
	       instruction.meta.category == ZYDIS_CATEGORY_RET ||
	       instruction.meta.category == ZYDIS_CATEGORY_INTERRUPT ||
	       instruction.meta.category == ZYDIS_CATEGORY_SYSRET ||
	       instruction.mnemonic == ZYDIS_MNEMONIC_UD2;
}

uintptr_t GetRelativeTarget(const DecodedCodeInstruction& decoded) {
	for (u8 index = 0; index < decoded.instruction.operand_count_visible; ++index) {
		const auto& operand = decoded.operands[index];
		if (operand.type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operand.imm.is_relative) {
			continue;
		}

		ZyanU64 target {};
		if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &operand, decoded.address,
		                                          &target))) {
			return target;
		}
	}
	return 0;
}

DecodedCodeInstruction DecodeCodeInstruction(uintptr_t address, uintptr_t end) {
	DecodedCodeInstruction decoded {.address = address};
	const auto status = ZydisDecoderDecodeFull(&GetDecoder(), reinterpret_cast<void*>(address),
	                                           end - address, &decoded.instruction,
	                                           decoded.operands.data());
	if (!ZYAN_SUCCESS(status)) {
		decoded.instruction.length = 0;
		return decoded;
	}

	for (u8 index = 0; index < decoded.instruction.operand_count; ++index) {
		const auto& operand = decoded.operands[index];
		if (operand.type == ZYDIS_OPERAND_TYPE_REGISTER) {
			decoded.uses_stack_pointer |= IsStackPointerRegister(operand.reg.value);
			if (IsStackPointerRegister(operand.reg.value) &&
			    (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0 &&
			    decoded.instruction.meta.category != ZYDIS_CATEGORY_CALL &&
			    decoded.instruction.meta.category != ZYDIS_CATEGORY_RET) {
				decoded.changes_stack_pointer = true;
			}
			continue;
		}
		if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY) {
			continue;
		}

		const bool stack_relative =
		    IsStackPointerRegister(operand.mem.base) || IsStackPointerRegister(operand.mem.index);
		decoded.uses_stack_pointer |= stack_relative;
		constexpr ZydisOperandActions MemoryAccessMask =
		    ZYDIS_OPERAND_ACTION_MASK_READ | ZYDIS_OPERAND_ACTION_MASK_WRITE;
		if (decoded.instruction.mnemonic != ZYDIS_MNEMONIC_LEA &&
		    decoded.instruction.mnemonic != ZYDIS_MNEMONIC_NOP && !stack_relative &&
		    (operand.actions & MemoryAccessMask) != 0) {
			decoded.accesses_memory = true;
		}
	}

	if (decoded.changes_stack_pointer) {
		const auto& operands = decoded.operands;
		if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
		    operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		    IsStackPointerRegister(operands[0].reg.value) &&
		    operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY) {
			decoded.replaces_stack_pointer = true;
		} else if ((decoded.instruction.mnemonic == ZYDIS_MNEMONIC_ADD ||
		            decoded.instruction.mnemonic == ZYDIS_MNEMONIC_SUB) &&
		           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		           IsStackPointerRegister(operands[0].reg.value) &&
		           operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
			const s64 immediate = operands[1].imm.is_signed
			                          ? operands[1].imm.value.s
			                          : static_cast<s64>(operands[1].imm.value.u);
			decoded.stack_pointer_delta =
			    decoded.instruction.mnemonic == ZYDIS_MNEMONIC_ADD ? immediate : -immediate;
		} else if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_LEA &&
		           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		           IsStackPointerRegister(operands[0].reg.value) &&
		           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		           IsStackPointerRegister(operands[1].mem.base) &&
		           operands[1].mem.index == ZYDIS_REGISTER_NONE) {
			decoded.stack_pointer_delta = operands[1].mem.disp.value;
		} else if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSH ||
		           decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSHF ||
		           decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSHFD ||
		           decoded.instruction.mnemonic == ZYDIS_MNEMONIC_PUSHFQ) {
			decoded.stack_pointer_delta = -static_cast<s64>(sizeof(u64));
		} else if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POP ||
		           decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POPF ||
		           decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POPFD ||
		           decoded.instruction.mnemonic == ZYDIS_MNEMONIC_POPFQ) {
			decoded.stack_pointer_delta = sizeof(u64);
		}
	}

	for (u8 index = 0; index < decoded.instruction.operand_count_visible; ++index) {
		const auto& operand = decoded.operands[index];
		if (operand.type != ZYDIS_OPERAND_TYPE_MEMORY ||
		    !IsStackPointerRegister(operand.mem.base) || operand.mem.disp.size == 0 ||
		    operand.mem.disp.value >= 0) {
			continue;
		}

		decoded.has_red_zone_operand = true;
		if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_LEA) {
			if (decoded.stack_pointer_delta.has_value()) {
				decoded.has_red_zone_operand = false;
				continue;
			}
			decoded.has_unmodeled_red_zone_operand = true;
			continue;
		}

		const s64 access_start = operand.mem.disp.value;
		const s64 access_size  = std::max<s64>(operand.size / 8, 1);
		const s64 range_start  = std::max(access_start, -static_cast<s64>(GuestRedZoneSize));
		const s64 range_end    = std::min<s64>(access_start + access_size, 0);
		for (s64 offset = range_start; offset < range_end; ++offset) {
			const size_t bit = static_cast<size_t>(offset + static_cast<s64>(GuestRedZoneSize));
			if ((operand.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0) {
				decoded.red_zone_use.set(bit);
			}
			if ((operand.actions & ZYDIS_OPERAND_ACTION_WRITE) != 0) {
				decoded.red_zone_def.set(bit);
			}
		}
	}
	return decoded;
}

bool IsSameRegister(ZydisRegister lhs, ZydisRegister rhs) {
	return lhs != ZYDIS_REGISTER_NONE && rhs != ZYDIS_REGISTER_NONE &&
	       ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, lhs) ==
	           ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, rhs);
}

bool WritesRegister(const DecodedCodeInstruction& decoded, ZydisRegister reg) {
	return std::ranges::any_of(
	    std::span {decoded.operands}.first(decoded.instruction.operand_count),
	    [reg](const ZydisDecodedOperand& operand) {
		    return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
		           (operand.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0 &&
		           IsSameRegister(operand.reg.value, reg);
	    });
}

std::optional<std::vector<uintptr_t>>
ResolveBoundedJumpTable(const DecodedFunction& function, uintptr_t branch_address,
                        uintptr_t function_start, uintptr_t function_end, uintptr_t segment_start,
                        uintptr_t segment_end) {
	const auto branch = function.instructions.find(branch_address);
	if (branch == function.instructions.end() ||
	    branch->second.instruction.mnemonic != ZYDIS_MNEMONIC_JMP ||
	    branch->second.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER) {
		return std::nullopt;
	}
	const ZydisRegister target_reg = branch->second.operands[0].reg.value;

	const auto previous_contiguous = [&function](auto instruction) {
		if (instruction == function.instructions.begin()) {
			return function.instructions.end();
		}
		const auto previous = std::prev(instruction);
		return previous->first + previous->second.instruction.length == instruction->first
		           ? previous
		           : function.instructions.end();
	};

	constexpr size_t MaxInterveningInstructions = 4;
	auto             add                        = function.instructions.end();
	auto             pattern_cursor             = branch;
	for (size_t count = 0; count <= MaxInterveningInstructions; ++count) {
		const auto candidate = previous_contiguous(pattern_cursor);
		if (candidate == function.instructions.end()) {
			break;
		}
		const auto& decoded = candidate->second;
		if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_ADD &&
		    decoded.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		    IsSameRegister(decoded.operands[0].reg.value, target_reg) &&
		    decoded.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
			add = candidate;
			break;
		}
		if (WritesRegister(decoded, target_reg) || IsControlFlowTerminator(decoded.instruction)) {
			return std::nullopt;
		}
		pattern_cursor = candidate;
	}
	if (add == function.instructions.end()) {
		return std::nullopt;
	}
	const ZydisRegister table_reg = add->second.operands[1].reg.value;

	const auto load = previous_contiguous(add);
	if (load == function.instructions.end() ||
	    load->second.instruction.mnemonic != ZYDIS_MNEMONIC_MOVSXD ||
	    load->second.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
	    !IsSameRegister(load->second.operands[0].reg.value, target_reg) ||
	    load->second.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY ||
	    !IsSameRegister(load->second.operands[1].mem.base, table_reg) ||
	    load->second.operands[1].mem.index == ZYDIS_REGISTER_NONE ||
	    load->second.operands[1].mem.scale != sizeof(s32)) {
		return std::nullopt;
	}
	const ZydisRegister index_reg = load->second.operands[1].mem.index;

	constexpr size_t         MaxPatternInstructions = 64;
	std::optional<size_t>    table_size;
	std::optional<uintptr_t> guarded_path_start;
	auto                     cursor = load;
	for (size_t count = 0; count < MaxPatternInstructions; ++count) {
		cursor = previous_contiguous(cursor);
		if (cursor == function.instructions.end()) {
			break;
		}
		const auto& decoded = cursor->second;
		if (decoded.instruction.mnemonic == ZYDIS_MNEMONIC_CMP &&
		    decoded.operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		    IsSameRegister(decoded.operands[0].reg.value, index_reg) &&
		    decoded.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
			const uintptr_t next_address  = decoded.address + decoded.instruction.length;
			const auto      bounds_branch = function.instructions.find(next_address);
			if (bounds_branch == function.instructions.end()) {
				return std::nullopt;
			}
			const s64 bound = decoded.operands[1].imm.is_signed
			                      ? decoded.operands[1].imm.value.s
			                      : static_cast<s64>(decoded.operands[1].imm.value.u);
			if (bound < 0) {
				return std::nullopt;
			}
			if (bounds_branch->second.instruction.mnemonic == ZYDIS_MNEMONIC_JNBE) {
				table_size = static_cast<size_t>(bound) + 1;
			} else if (bounds_branch->second.instruction.mnemonic == ZYDIS_MNEMONIC_JNB) {
				table_size = static_cast<size_t>(bound);
			} else {
				return std::nullopt;
			}
			guarded_path_start = next_address + bounds_branch->second.instruction.length;
			break;
		}
		if (WritesRegister(decoded, index_reg)) {
			return std::nullopt;
		}
	}
	if (!table_size) {
		return std::nullopt;
	}
	if (std::ranges::any_of(function.branch_targets, [&](uintptr_t target) {
		    return target >= *guarded_path_start && target <= branch_address;
	    })) {
		return std::nullopt;
	}

	const auto decode_table_address =
	    [table_reg](const DecodedCodeInstruction& decoded) -> std::optional<uintptr_t> {
		if (decoded.instruction.mnemonic != ZYDIS_MNEMONIC_LEA ||
		    decoded.operands[0].type != ZYDIS_OPERAND_TYPE_REGISTER ||
		    !IsSameRegister(decoded.operands[0].reg.value, table_reg) ||
		    decoded.operands[1].type != ZYDIS_OPERAND_TYPE_MEMORY) {
			return std::nullopt;
		}
		ZyanU64 absolute_address {};
		if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &decoded.operands[1],
		                                           decoded.address, &absolute_address))) {
			return std::nullopt;
		}
		return absolute_address;
	};

	std::set<uintptr_t> table_candidates;
	cursor = load;
	for (size_t count = 0; count < MaxPatternInstructions; ++count) {
		cursor = previous_contiguous(cursor);
		if (cursor == function.instructions.end()) {
			break;
		}
		if (!WritesRegister(cursor->second, table_reg)) {
			continue;
		}
		if (const auto address = decode_table_address(cursor->second)) {
			table_candidates.insert(*address);
		}
		break;
	}
	if (table_candidates.empty()) {
		for (const auto& [address, decoded]: function.instructions) {
			if (address >= branch_address) {
				break;
			}
			if (const auto table_address = decode_table_address(decoded)) {
				table_candidates.insert(*table_address);
			}
		}
	}
	constexpr size_t MaxJumpTableEntries = 4096;
	if (*table_size == 0 || *table_size > MaxJumpTableEntries) {
		return std::nullopt;
	}

	std::optional<std::vector<uintptr_t>> resolved_targets;
	for (const uintptr_t table_address: table_candidates) {
		if (table_address < segment_start || table_address > segment_end ||
		    *table_size > (segment_end - table_address) / sizeof(s32)) {
			continue;
		}

		std::vector<uintptr_t> targets;
		targets.reserve(*table_size);
		bool valid = true;
		for (size_t index = 0; index < *table_size; ++index) {
			s32 offset;
			std::memcpy(&offset,
			            reinterpret_cast<const void*>(table_address + index * sizeof(offset)),
			            sizeof(offset));
			const s64 target = static_cast<s64>(table_address) + offset;
			if (target < static_cast<s64>(function_start) ||
			    target >= static_cast<s64>(function_end)) {
				valid = false;
				break;
			}
			targets.push_back(static_cast<uintptr_t>(target));
		}
		if (!valid) {
			continue;
		}
		std::ranges::sort(targets);
		targets.erase(std::ranges::unique(targets).begin(), targets.end());
		if (resolved_targets) {
			return std::nullopt;
		}
		resolved_targets = std::move(targets);
	}
	return resolved_targets;
}

DecodedFunction DecodeFunction(uintptr_t function_start, uintptr_t function_end,
                               uintptr_t segment_start, uintptr_t segment_end) {
	DecodedFunction               function;
	std::vector<uintptr_t>        blocks {function_start};
	std::unordered_set<uintptr_t> visited;
	std::set<uintptr_t>           indirect_branches;
	std::set<uintptr_t>           resolved_indirect_branches;

	while (true) {
		while (!blocks.empty()) {
			uintptr_t address = blocks.back();
			blocks.pop_back();

			while (address >= function_start && address < function_end &&
			       !visited.contains(address)) {
				visited.insert(address);
				auto decoded = DecodeCodeInstruction(address, function_end);
				if (decoded.instruction.length == 0) {
					break;
				}

				function.uses_red_zone |= decoded.has_red_zone_operand;
				function.requires_conservative_red_zone_tracking |=
				    decoded.has_unmodeled_red_zone_operand ||
				    (decoded.changes_stack_pointer && !decoded.stack_pointer_delta.has_value());

				const uintptr_t next_address  = address + decoded.instruction.length;
				const uintptr_t branch_target = GetRelativeTarget(decoded);
				if (branch_target >= function_start && branch_target < function_end) {
					function.branch_targets.insert(branch_target);
				}
				function.instructions.emplace(address, decoded);

				if (decoded.instruction.meta.category == ZYDIS_CATEGORY_COND_BR) {
					if (branch_target >= function_start && branch_target < function_end) {
						blocks.push_back(branch_target);
					}
				} else if (IsControlFlowTerminator(decoded.instruction)) {
					if (decoded.instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
					    branch_target >= function_start && branch_target < function_end) {
						blocks.push_back(branch_target);
					} else if (decoded.instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR &&
					           branch_target == 0) {
						indirect_branches.insert(address);
					}
					break;
				}
				address = next_address;
			}
		}

		bool discovered_block = false;
		for (const uintptr_t branch_address: indirect_branches) {
			if (resolved_indirect_branches.contains(branch_address)) {
				continue;
			}
			const auto targets = ResolveBoundedJumpTable(function, branch_address, function_start,
			                                             function_end, segment_start, segment_end);
			if (!targets) {
				continue;
			}
			resolved_indirect_branches.insert(branch_address);
			for (const uintptr_t target: *targets) {
				function.branch_targets.insert(target);
				if (!visited.contains(target)) {
					blocks.push_back(target);
					discovered_block = true;
				}
			}
		}
		if (!discovered_block) {
			break;
		}
	}
	function.has_indirect_branch = indirect_branches.size() != resolved_indirect_branches.size();
	return function;
}

RedZoneMask TranslateRedZoneMask(const RedZoneMask& mask, s64 stack_pointer_delta) {
	RedZoneMask translated;
	for (size_t bit = 0; bit < GuestRedZoneSize; ++bit) {
		if (!mask.test(bit)) {
			continue;
		}
		const s64 after_offset  = static_cast<s64>(bit) - static_cast<s64>(GuestRedZoneSize);
		const s64 before_offset = after_offset + stack_pointer_delta;
		if (before_offset >= -static_cast<s64>(GuestRedZoneSize) && before_offset < 0) {
			translated.set(static_cast<size_t>(before_offset + static_cast<s64>(GuestRedZoneSize)));
		}
	}
	return translated;
}

void AnalyzeRedZoneLiveness(DecodedFunction& function) {
	if (!function.uses_red_zone) {
		return;
	}

	if (function.has_indirect_branch || function.requires_conservative_red_zone_tracking ||
	    std::ranges::any_of(function.branch_targets, [&function](uintptr_t target) {
		    return !function.instructions.contains(target);
	    })) {
		for (auto& [_, decoded]: function.instructions) {
			decoded.red_zone_live.set();
		}
		return;
	}

	std::vector<DecodedCodeInstruction*> instructions;
	instructions.reserve(function.instructions.size());
	std::map<uintptr_t, size_t> indices;
	for (auto& [address, decoded]: function.instructions) {
		indices.emplace(address, instructions.size());
		instructions.push_back(&decoded);
	}

	std::vector<RedZoneMask> live_in(instructions.size());
	bool                     changed;
	do {
		changed = false;
		for (size_t reverse_index = instructions.size(); reverse_index-- > 0;) {
			const auto& decoded = *instructions[reverse_index];
			RedZoneMask live_out;

			const auto add_successor = [&](uintptr_t address) {
				if (const auto successor = indices.find(address); successor != indices.end()) {
					live_out |= live_in[successor->second];
				}
			};

			const uintptr_t next_address  = decoded.address + decoded.instruction.length;
			const uintptr_t branch_target = GetRelativeTarget(decoded);
			if (decoded.instruction.meta.category == ZYDIS_CATEGORY_COND_BR) {
				add_successor(next_address);
				add_successor(branch_target);
			} else if (decoded.instruction.meta.category == ZYDIS_CATEGORY_UNCOND_BR) {
				add_successor(branch_target);
			} else if (!IsControlFlowTerminator(decoded.instruction)) {
				add_successor(next_address);
			}

			const RedZoneMask translated_live_out =
			    decoded.stack_pointer_delta.has_value()
			        ? TranslateRedZoneMask(live_out, *decoded.stack_pointer_delta)
			        : live_out;
			const RedZoneMask new_live_in =
			    decoded.red_zone_use | (translated_live_out & ~decoded.red_zone_def);
			if (new_live_in != live_in[reverse_index]) {
				live_in[reverse_index] = new_live_in;
				changed                = true;
			}
		}
	} while (changed);

	for (size_t index = 0; index < instructions.size(); ++index) {
		instructions[index]->red_zone_live = live_in[index];
	}
}

bool EncodeRelocatedInstruction(const DecodedCodeInstruction& decoded,
                                Xbyak::CodeGenerator&         generator) {
	ZydisEncoderRequest request;
	if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
	        &decoded.instruction, decoded.operands.data(),
	        decoded.instruction.operand_count_visible, &request))) {
		return false;
	}

	for (u8 index = 0; index < decoded.instruction.operand_count_visible; ++index) {
		const auto& operand = decoded.operands[index];
		if (operand.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && operand.imm.is_relative) {
			ZyanU64 absolute_address {};
			if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &operand,
			                                           decoded.address, &absolute_address))) {
				return false;
			}
			request.operands[index].imm.u = absolute_address;
		} else if (operand.type == ZYDIS_OPERAND_TYPE_MEMORY &&
		           (operand.mem.base == ZYDIS_REGISTER_RIP ||
		            operand.mem.base == ZYDIS_REGISTER_EIP)) {
			ZyanU64 absolute_address {};
			if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &operand,
			                                           decoded.address, &absolute_address))) {
				return false;
			}
			request.operands[index].mem.displacement = static_cast<ZyanI64>(absolute_address);
		}
	}

	std::array<u8, ZYDIS_MAX_INSTRUCTION_LENGTH> encoded {};
	ZyanUSize                                    encoded_size = encoded.size();
	if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(
	        &request, encoded.data(), &encoded_size,
	        reinterpret_cast<ZyanU64>(generator.getCurr())))) {
		return false;
	}
	generator.db(encoded.data(), encoded_size);
	return true;
}

bool GenerateProtectedIndirectCall(const DecodedCodeInstruction& decoded,
                                   Xbyak::CodeGenerator&         generator) {
	ASSERT(decoded.instruction.meta.category == ZYDIS_CATEGORY_CALL);
	const auto& target = decoded.operands[0];
	ASSERT(target.type == ZYDIS_OPERAND_TYPE_MEMORY && target.size == sizeof(uintptr_t) * CHAR_BIT);

	ZydisEncoderRequest request {};
	request.machine_mode          = ZYDIS_MACHINE_MODE_LONG_64;
	request.mnemonic              = ZYDIS_MNEMONIC_MOV;
	request.operand_count         = 2;
	request.operands[0].type      = ZYDIS_OPERAND_TYPE_REGISTER;
	request.operands[0].reg.value = ZYDIS_REGISTER_R11;

	ZydisEncoderRequest original_request {};
	if (!ZYAN_SUCCESS(ZydisEncoderDecodedInstructionToEncoderRequest(
	        &decoded.instruction, decoded.operands.data(),
	        decoded.instruction.operand_count_visible, &original_request))) {
		return false;
	}
	request.prefixes             = original_request.prefixes & ZYDIS_ATTRIB_HAS_SEGMENT;
	request.address_size_hint    = original_request.address_size_hint;
	request.operands[1]          = original_request.operands[0];
	request.operands[1].mem.size = sizeof(uintptr_t);

	if (target.mem.base == ZYDIS_REGISTER_RIP || target.mem.base == ZYDIS_REGISTER_EIP) {
		ZyanU64 absolute_address {};
		if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&decoded.instruction, &target, decoded.address,
		                                           &absolute_address))) {
			return false;
		}
		request.operands[1].mem.displacement = static_cast<ZyanI64>(absolute_address);
	}

	generator.lea(rsp, ptr[rsp - GuestRedZoneSize]);
	generator.push(r11);

	std::array<u8, ZYDIS_MAX_INSTRUCTION_LENGTH> encoded {};
	ZyanUSize                                    encoded_size = encoded.size();
	if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(
	        &request, encoded.data(), &encoded_size,
	        reinterpret_cast<ZyanU64>(generator.getCurr())))) {
		return false;
	}
	generator.db(encoded.data(), encoded_size);

	generator.xchg(r11, ptr[rsp]);
	generator.lea(rsp, ptr[rsp + GuestRedZoneSize + sizeof(uintptr_t)]);
	generator.call(ptr[rsp - GuestRedZoneSize - sizeof(uintptr_t)]);
	return true;
}

void CollectRedZoneMemoryInstructions(const DecodedFunction&                   function,
                                      std::map<uintptr_t, InstructionRewrite>& rewrite_sites,
                                      GuestInstructionPatchResult&             result) {
	if (!function.uses_red_zone) {
		return;
	}
	for (const auto& [address, decoded]: function.instructions) {
		if (!decoded.accesses_memory || !decoded.red_zone_live.any() ||
		    rewrite_sites.contains(address)) {
			continue;
		}
		++result.memory_instruction_count;
		if (decoded.instruction.length < NearJumpSize) {
			++result.short_memory_instruction_count;
		}
		if (decoded.instruction.meta.category == ZYDIS_CATEGORY_CALL &&
		    decoded.operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY &&
		    decoded.operands[0].size == sizeof(uintptr_t) * CHAR_BIT &&
		    !IsStackPointerRegister(decoded.operands[0].mem.base) &&
		    !IsStackPointerRegister(decoded.operands[0].mem.index)) {
			rewrite_sites[address] = {
			    .protect_red_zone        = true,
			    .protected_indirect_call = true,
			};
			continue;
		}
		if (decoded.uses_stack_pointer && !decoded.replaces_stack_pointer) {
			++result.stack_dependent_memory_instruction_count;
			continue;
		}
		if (decoded.instruction.meta.category == ZYDIS_CATEGORY_CALL ||
		    IsControlFlowTerminator(decoded.instruction)) {
			++result.control_flow_memory_instruction_count;
			continue;
		}
		rewrite_sites[address].protect_red_zone = true;
	}
}

// Compute 1/sqrt in double precision, rounded to float, with denormals treated as
// signed zero. Preserve guest MXCSR, RFLAGS and the scratch register's full YMM.
void GenerateReciprocalSquareRoot(const DecodedCodeInstruction& decoded,
                                  Xbyak::CodeGenerator&         generator) {
	const int destination   = decoded.operands[0].reg.value - ZYDIS_REGISTER_XMM0;
	const int source        = decoded.operands[1].reg.value - ZYDIS_REGISTER_XMM0;
	int       scratch_index = 0;
	while (scratch_index == destination || scratch_index == source) {
		++scratch_index;
	}
	const Xbyak::Ymm scratch(scratch_index);
	Xbyak::Label     one;
	Xbyak::Label     done;
	constexpr size_t SpillSize = 48;

	// Guest code uses the SysV red zone on both hosts; put our spills below it.
	// LEA/MOV/SIMD leave RFLAGS intact, including when the guest red zone is live.
	generator.lea(rsp, ptr[rsp - GuestRedZoneSize - SpillSize]);
	generator.vmovdqu(ptr[rsp], scratch);
	generator.stmxcsr(ptr[rsp + 32]);
	generator.mov(dword[rsp + 36], 0x1fc0); // round to nearest, exceptions masked, DAZ
	generator.ldmxcsr(ptr[rsp + 36]);
	generator.vcvtps2pd(scratch, Xbyak::Xmm(source));
	generator.vsqrtpd(scratch, scratch);
	generator.vbroadcastsd(Xbyak::Ymm(destination), ptr[rip + one]);
	generator.vdivpd(scratch, Xbyak::Ymm(destination), scratch);
	generator.vcvtpd2ps(Xbyak::Xmm(destination), scratch); // Clears upper YMM lanes.
	generator.ldmxcsr(ptr[rsp + 32]);
	generator.vmovdqu(scratch, ptr[rsp]);
	generator.lea(rsp, ptr[rsp + GuestRedZoneSize + SpillSize]);
	generator.jmp(done);
	// A failed label reference can have an incomplete displacement. Do not bind it.
	if (Xbyak::GetError() != 0) {
		return;
	}
	generator.L(one);
	generator.dq(0x3ff0000000000000ULL);
	generator.L(done);
}

bool IsSupportedExtractQ(const DecodedCodeInstruction& decoded) {
	if (decoded.instruction.mnemonic != ZYDIS_MNEMONIC_EXTRQ) {
		return false;
	}
	const auto is_xmm = [](const ZydisDecodedOperand& operand) {
		return operand.type == ZYDIS_OPERAND_TYPE_REGISTER &&
		       operand.reg.value >= ZYDIS_REGISTER_XMM0 &&
		       operand.reg.value <= ZYDIS_REGISTER_XMM15;
	};
	const bool immediate = decoded.instruction.operand_count_visible == 3 &&
	                       decoded.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
	                       decoded.operands[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
	if (!is_xmm(decoded.operands[0]) ||
	    (!immediate &&
	     !(decoded.instruction.operand_count_visible == 2 && is_xmm(decoded.operands[1])))) {
		return false;
	}
	// Match the trap handler's encoding support: 66 [REX] 0F 78/79 ModRM [imm8 imm8].
	const auto*  code   = reinterpret_cast<const u8*>(decoded.address);
	const size_t opcode = (code[1] & 0xf0u) == 0x40u ? 2 : 1;
	return code[0] == 0x66 && code[opcode] == 0x0f &&
	       code[opcode + 1] == (immediate ? 0x78 : 0x79) &&
	       decoded.instruction.length == opcode + (immediate ? 5 : 3);
}

void GenerateExtractQ(const DecodedCodeInstruction& decoded, Xbyak::CodeGenerator& generator) {
	const Xbyak::Xmm destination(decoded.operands[0].reg.value - ZYDIS_REGISTER_XMM0);
	// Keep spills below the guest red zone on both hosts, and restore all modified flags/GPRs.
	generator.lea(rsp, ptr[rsp - GuestRedZoneSize]);
	generator.pushfq();
	generator.push(rax);
	generator.push(rcx);
	generator.push(rdx);
	if (decoded.operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
		const u32 length = decoded.operands[1].imm.value.u & 63u;
		const u32 index  = decoded.operands[2].imm.value.u & 63u;
		const u64 mask   = length == 0 ? UINT64_MAX : (u64 {1} << length) - 1;
		generator.movq(rax, destination);
		if (index != 0) {
			generator.shr(rax, index);
		}
		generator.mov(rdx, mask);
	} else {
		const Xbyak::Xmm source(decoded.operands[1].reg.value - ZYDIS_REGISTER_XMM0);
		generator.movq(rax, source);
		generator.mov(ecx, eax);
		generator.neg(ecx);
		generator.mov(rdx, UINT64_MAX);
		// CL shifts use six bits: -length gives 64-length, with zero retaining all 64 bits.
		generator.shr(rdx, cl);
		generator.shr(rax, 8);
		generator.mov(ecx, eax);
		generator.movq(rax, destination);
		generator.shr(rax, cl);
	}
	generator.and_(rax, rdx);
	// Match the emulator: clear the upper XMM half, preserving the upper YMM half.
	generator.movq(destination, rax);
	generator.pop(rdx);
	generator.pop(rcx);
	generator.pop(rax);
	generator.popfq();
	generator.lea(rsp, ptr[rsp + GuestRedZoneSize]);
}

void CollectAmdInstructions(const DecodedFunction&                   function,
                            std::map<uintptr_t, InstructionRewrite>& rewrite_sites,
                            GuestInstructionPatchResult&             result) {
	static const bool host_lacks_sse4a = !Xbyak::util::Cpu().has(Xbyak::util::Cpu::tSSE4a);
	for (const auto& [address, decoded]: function.instructions) {
		const auto&            instruction = decoded.instruction;
		InstructionReplacement replacement {};
		if (instruction.mnemonic == ZYDIS_MNEMONIC_VRSQRTPS &&
		    instruction.encoding == ZYDIS_INSTRUCTION_ENCODING_VEX &&
		    instruction.raw.vex.offset == 0 && decoded.operands[0].size == 128 &&
		    decoded.operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER) {
			replacement = InstructionReplacement::ReciprocalSquareRoot;
		} else if (host_lacks_sse4a && IsSupportedExtractQ(decoded)) {
			replacement = InstructionReplacement::ExtractQ;
		} else {
			continue;
		}
		++ReplacementCounts(result, replacement).found;
		rewrite_sites[address].replacement = replacement;
	}
}

void MarkInstructionTrap(u8* code, const ZydisDecodedInstruction& instruction,
                         InstructionReplacement replacement) {
	if (replacement == InstructionReplacement::ReciprocalSquareRoot) {
		// Clear a reserved VEX.vvvv bit, retaining both register operands for the handler.
		code[instruction.raw.vex.size - 1] &= ~0x08u;
	}
	// EXTRQ already raises #UD on hosts without SSE4a; keep its original encoding.
}

void GenerateInstructionTrap(const DecodedCodeInstruction& decoded,
                             InstructionReplacement replacement, Xbyak::CodeGenerator& generator) {
	std::array<u8, ZYDIS_MAX_INSTRUCTION_LENGTH> code {};
	std::memcpy(code.data(), reinterpret_cast<const void*>(decoded.address),
	            decoded.instruction.length);
	MarkInstructionTrap(code.data(), decoded.instruction, replacement);
	generator.db(code.data(), decoded.instruction.length);
}

bool NeedsTrapRedZoneProtection(const DecodedCodeInstruction& decoded) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	return decoded.red_zone_live.any();
#else
	return false;
#endif
}

void TrapUnrelocatedInstructions(const PatchModule& module, const DecodedFunction& function,
                                 const std::map<uintptr_t, InstructionRewrite>& rewrite_sites,
                                 GuestInstructionPatchResult&                   result) {
	for (const auto& [address, rewrite]: rewrite_sites) {
		if (rewrite.replacement == InstructionReplacement::None ||
		    module.patched.contains(reinterpret_cast<u8*>(address))) {
			continue;
		}
		const auto& decoded = function.instructions.at(address);
		if (NeedsTrapRedZoneProtection(decoded)) {
			if (rewrite.replacement == InstructionReplacement::ExtractQ) {
				// Unlike VRSQRTPS, leaving EXTRQ unchanged would still trap and corrupt the red
				// zone.
				EXIT("AMD CPU compatibility: cannot safely trap EXTRQ at %p (guest red zone is "
				     "live)\n",
				     reinterpret_cast<void*>(address));
			}
			continue;
		}
		MarkInstructionTrap(reinterpret_cast<u8*>(address), decoded.instruction,
		                    rewrite.replacement);
		++ReplacementCounts(result, rewrite.replacement).trapped;
	}
}

void RelocateGuestInstructions(PatchModule* module, const DecodedFunction& function,
                               const std::map<uintptr_t, InstructionRewrite>& rewrite_sites,
                               GuestInstructionPatchResult&                   result) {
	struct RelocationSpan {
		std::vector<const DecodedCodeInstruction*> instructions;
		uintptr_t                                  patch_start {};
		uintptr_t                                  continuation {};
		size_t                                     patch_size {};
		bool                                       trap_replacements {};
	};

	const auto emit_span = [&](RelocationSpan& span) -> std::optional<size_t> {
		auto&        generator         = module->trampoline_gen;
		const size_t trampoline_offset = generator.getSize();
		const bool   has_replacements =
		    std::ranges::any_of(span.instructions, [&](const auto* decoded) {
			    const auto rewrite = rewrite_sites.find(decoded->address);
			    return rewrite != rewrite_sites.end() &&
			           rewrite->second.replacement != InstructionReplacement::None;
		    });
		for (bool trap_replacements: {false, true}) {
			span.trap_replacements = trap_replacements;
			Xbyak::ClearError();
			bool encoded = true;
			for (const auto* decoded: span.instructions) {
				const auto rewrite     = rewrite_sites.find(decoded->address);
				const auto replacement = rewrite != rewrite_sites.end()
				                             ? rewrite->second.replacement
				                             : InstructionReplacement::None;
				const bool protected_indirect_call =
				    rewrite != rewrite_sites.end() && rewrite->second.protected_indirect_call;
				const bool protect_red_zone =
				    (rewrite != rewrite_sites.end() && rewrite->second.protect_red_zone &&
				     !protected_indirect_call) ||
				    (replacement != InstructionReplacement::None && trap_replacements &&
				     NeedsTrapRedZoneProtection(*decoded));
				if (protect_red_zone) {
					generator.lea(rsp, ptr[rsp - GuestRedZoneSize]);
				}
				if (replacement != InstructionReplacement::None) {
					if (trap_replacements) {
						GenerateInstructionTrap(*decoded, replacement, generator);
					} else if (replacement == InstructionReplacement::ReciprocalSquareRoot) {
						GenerateReciprocalSquareRoot(*decoded, generator);
					} else {
						GenerateExtractQ(*decoded, generator);
					}
				} else if (protected_indirect_call) {
					encoded = GenerateProtectedIndirectCall(*decoded, generator);
				} else {
					encoded = EncodeRelocatedInstruction(*decoded, generator);
				}
				if (protect_red_zone && !decoded->replaces_stack_pointer) {
					generator.lea(rsp, ptr[rsp + GuestRedZoneSize]);
				}
				if (!encoded || Xbyak::GetError() != 0) {
					break;
				}
			}
			if (encoded && Xbyak::GetError() == 0) {
				generator.jmp(reinterpret_cast<void*>(span.continuation));
				if (Xbyak::GetError() == 0) {
					return trampoline_offset;
				}
			}
			const int error = Xbyak::GetError();
			// Roll back before retrying this uncommitted span with smaller trap replacements.
			generator.reset();
			generator.setSize(trampoline_offset);
			if (error != 0 && !HandleTrampolineError(module, error)) {
				EXIT("Guest instruction trampoline generation failed: %s\n",
				     Xbyak::ConvertErrorToString(error));
			}
			if (error != Xbyak::ERR_CODE_IS_TOO_BIG || !has_replacements) {
				break;
			}
		}
		return std::nullopt;
	};

	const auto record_rewrites = [&](const RelocationSpan& span) {
		for (const auto* decoded: span.instructions) {
			module->patched.insert(reinterpret_cast<u8*>(decoded->address));
			const auto rewrite = rewrite_sites.find(decoded->address);
			if (rewrite == rewrite_sites.end()) {
				continue;
			}
			if (rewrite->second.protect_red_zone && decoded->accesses_memory) {
				++result.patched_memory_instruction_count;
			}
			if (rewrite->second.replacement != InstructionReplacement::None) {
				auto& counts = ReplacementCounts(result, rewrite->second.replacement);
				if (span.trap_replacements) {
					++counts.trapped;
				} else {
					++counts.native;
				}
			}
		}
	};

	std::vector<std::pair<uintptr_t, uintptr_t>> patched_spans;
	std::vector<uintptr_t>                       relay_slots;
	std::vector<uintptr_t>                       short_relay_slots;
	std::vector<uintptr_t>                       unresolved_sites;
	uintptr_t                                    covered_until {};
	for (auto site_it = rewrite_sites.begin(); site_it != rewrite_sites.end(); ++site_it) {
		const uintptr_t site = site_it->first;
		if (site < covered_until) {
			continue;
		}

		const auto collect_forward_span = [&]() -> std::optional<RelocationSpan> {
			RelocationSpan span {.patch_start = site, .continuation = site};
			while (span.patch_size < NearJumpSize) {
				const auto decoded_it = function.instructions.find(span.continuation);
				if (decoded_it == function.instructions.end() ||
				    (span.continuation != site &&
				     function.branch_targets.contains(span.continuation))) {
					return std::nullopt;
				}

				const auto& decoded = decoded_it->second;
				span.instructions.push_back(&decoded);
				span.patch_size += decoded.instruction.length;
				span.continuation += decoded.instruction.length;
				if (span.patch_size < NearJumpSize &&
				    IsControlFlowTerminator(decoded.instruction)) {
					return std::nullopt;
				}
			}
			return span;
		};

		const auto collect_backward_span = [&]() -> std::optional<RelocationSpan> {
			const auto site_instruction = function.instructions.find(site);
			ASSERT(site_instruction != function.instructions.end());
			RelocationSpan span {
			    .instructions = {&site_instruction->second},
			    .patch_start  = site,
			    .continuation = site + site_instruction->second.instruction.length,
			    .patch_size   = site_instruction->second.instruction.length,
			};

			while (span.patch_size < NearJumpSize) {
				const auto previous_end = function.instructions.lower_bound(span.patch_start);
				if (previous_end == function.instructions.begin() ||
				    function.branch_targets.contains(span.patch_start)) {
					return std::nullopt;
				}

				const auto  previous = std::prev(previous_end);
				const auto& decoded  = previous->second;
				if (previous->first + decoded.instruction.length != span.patch_start ||
				    previous->first < covered_until ||
				    IsControlFlowTerminator(decoded.instruction)) {
					return std::nullopt;
				}

				span.instructions.insert(span.instructions.begin(), &decoded);
				span.patch_start = previous->first;
				span.patch_size += decoded.instruction.length;
			}
			return span;
		};

		std::optional<RelocationSpan> selected_span;
		std::optional<size_t>         trampoline_offset;
		const auto&                   site_instruction       = function.instructions.at(site);
		const bool                    can_relocate_neighbors = !function.has_indirect_branch;
		if (can_relocate_neighbors || site_instruction.instruction.length >= NearJumpSize) {
			auto forward_span = collect_forward_span();
			if (forward_span) {
				trampoline_offset = emit_span(*forward_span);
				if (trampoline_offset) {
					selected_span = std::move(forward_span);
				}
			}
		}
		if (!selected_span && can_relocate_neighbors) {
			if (auto backward_span = collect_backward_span()) {
				trampoline_offset = emit_span(*backward_span);
				if (trampoline_offset) {
					selected_span = std::move(backward_span);
				}
			}
		}
		if (!selected_span) {
			unresolved_sites.push_back(site);
			continue;
		}

		const auto& span       = *selected_span;
		const auto* trampoline = module->trampoline_gen.getCode() + *trampoline_offset;

		auto& patch_gen = module->patch_gen;
		patch_gen.reset();
		patch_gen.setSize(span.patch_start - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
		patch_gen.jmp(trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
		patch_gen.nop(span.patch_size - NearJumpSize);
		EXIT_IF(Xbyak::GetError() != 0);

		record_rewrites(span);
		patched_spans.emplace_back(span.patch_start, span.continuation);
		if (span.patch_size >= NearJumpSize * 2) {
			relay_slots.push_back(span.patch_start + NearJumpSize);
		}
		if (span.patch_size >= NearJumpSize + ShortJumpSize) {
			short_relay_slots.push_back(span.patch_start + NearJumpSize);
		}
		covered_until = span.continuation;
	}

	const auto record_unsupported = [&](uintptr_t site) {
		const auto& decoded = function.instructions.at(site);
		const auto& rewrite = rewrite_sites.at(site);
		if (rewrite.protect_red_zone && decoded.accesses_memory) {
			++result.unrelocatable_memory_instruction_count;
		}
	};
	const auto overlaps_patched_span = [&patched_spans](uintptr_t start, uintptr_t end) {
		return std::ranges::any_of(patched_spans, [start, end](const auto& patched) {
			return start < patched.second && patched.first < end;
		});
	};

	for (const uintptr_t site: unresolved_sites) {
		if (module->patched.contains(reinterpret_cast<u8*>(site))) {
			continue;
		}

		const auto site_instruction = function.instructions.find(site);
		ASSERT(site_instruction != function.instructions.end());
		if (function.has_indirect_branch ||
		    site_instruction->second.instruction.length < ShortJumpSize) {
			record_unsupported(site);
			continue;
		}

		RelocationSpan site_span {
		    .instructions = {&site_instruction->second},
		    .patch_start  = site,
		    .continuation = site + site_instruction->second.instruction.length,
		    .patch_size   = site_instruction->second.instruction.length,
		};
		const size_t trampoline_start       = module->trampoline_gen.getSize();
		const auto   site_trampoline_offset = emit_span(site_span);
		if (!site_trampoline_offset) {
			record_unsupported(site);
			continue;
		}

		constexpr s64 ShortJumpMin = std::numeric_limits<s8>::min();
		constexpr s64 ShortJumpMax = std::numeric_limits<s8>::max();
		const auto    relay_slot = std::ranges::find_if(relay_slots, [site](uintptr_t address) {
			const s64 displacement =
			    static_cast<s64>(address) - static_cast<s64>(site + ShortJumpSize);
			return displacement >= ShortJumpMin && displacement <= ShortJumpMax;
		});
		if (relay_slot != relay_slots.end()) {
			const auto* site_trampoline =
			    module->trampoline_gen.getCode() + *site_trampoline_offset;

			auto& patch_gen = module->patch_gen;
			patch_gen.reset();
			patch_gen.setSize(*relay_slot - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
			patch_gen.jmp(site_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
			EXIT_IF(Xbyak::GetError() != 0);

			patch_gen.reset();
			patch_gen.setSize(site - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
			patch_gen.jmp(reinterpret_cast<void*>(*relay_slot),
			              Xbyak::CodeGenerator::LabelType::T_SHORT);
			patch_gen.nop(site_span.patch_size - ShortJumpSize);
			EXIT_IF(Xbyak::GetError() != 0);

			record_rewrites(site_span);
			patched_spans.emplace_back(site_span.patch_start, site_span.continuation);
			std::erase(short_relay_slots, *relay_slot);
			relay_slots.erase(relay_slot);
			continue;
		}

		const auto find_host = [&](uintptr_t short_jump_address) {
			std::pair<std::optional<RelocationSpan>, std::optional<size_t>> result;
			const s64 minimum_host = static_cast<s64>(short_jump_address) +
			                         static_cast<s64>(ShortJumpSize) -
			                         static_cast<s64>(NearJumpSize) + ShortJumpMin;
			const s64 maximum_host = static_cast<s64>(short_jump_address) +
			                         static_cast<s64>(ShortJumpSize) -
			                         static_cast<s64>(NearJumpSize) + ShortJumpMax;

			auto candidate = function.instructions.lower_bound(
			    static_cast<uintptr_t>(std::max<s64>(minimum_host, 0)));
			for (; candidate != function.instructions.end() &&
			       static_cast<s64>(candidate->first) <= maximum_host;
			     ++candidate) {
				RelocationSpan span {.patch_start  = candidate->first,
				                     .continuation = candidate->first};
				while (span.patch_size < NearJumpSize * 2) {
					const auto instruction = function.instructions.find(span.continuation);
					if (instruction == function.instructions.end() ||
					    (span.continuation != span.patch_start &&
					     function.branch_targets.contains(span.continuation))) {
						span.instructions.clear();
						break;
					}
					span.instructions.push_back(&instruction->second);
					span.patch_size += instruction->second.instruction.length;
					span.continuation += instruction->second.instruction.length;
					if (span.patch_size < NearJumpSize * 2 &&
					    IsControlFlowTerminator(instruction->second.instruction)) {
						span.instructions.clear();
						break;
					}
				}
				if (span.instructions.empty() ||
				    !(span.continuation <= site ||
				      span.patch_start >= site_span.continuation) ||
				    overlaps_patched_span(span.patch_start, span.continuation)) {
					continue;
				}

				const uintptr_t relay_address = span.patch_start + NearJumpSize;
				const s64 displacement = static_cast<s64>(relay_address) -
				                         static_cast<s64>(short_jump_address + ShortJumpSize);
				if (displacement < ShortJumpMin || displacement > ShortJumpMax) {
					continue;
				}

				const auto offset = emit_span(span);
				if (!offset) {
					continue;
				}
				result.first  = std::move(span);
				result.second = offset;
				break;
			}
			return result;
		};

		auto [host_span, host_trampoline_offset] = find_host(site);
		std::optional<uintptr_t>       final_relay_slot;
		uintptr_t                      final_short_jump = site;
		std::map<uintptr_t, uintptr_t> relay_parent {{site, site}};
		std::vector<uintptr_t>         relay_queue {site};
		for (size_t queue_index = 0;
		     !host_span && !final_relay_slot && queue_index < relay_queue.size();
		     ++queue_index) {
			const uintptr_t current = relay_queue[queue_index];
			if (current != site) {
				if (std::ranges::find(relay_slots, current) != relay_slots.end()) {
					final_relay_slot = current;
					final_short_jump = relay_parent.at(current);
					break;
				}
				const auto near_slot =
				    std::ranges::find_if(relay_slots, [current](uintptr_t address) {
					    const s64 displacement = static_cast<s64>(address) -
					                             static_cast<s64>(current + ShortJumpSize);
					    return address != current && displacement >= ShortJumpMin &&
					           displacement <= ShortJumpMax;
				    });
				if (near_slot != relay_slots.end()) {
					final_relay_slot = *near_slot;
					final_short_jump = current;
					break;
				}

				auto bridge_host = find_host(current);
				if (bridge_host.first) {
					host_span              = std::move(bridge_host.first);
					host_trampoline_offset = bridge_host.second;
					final_short_jump       = current;
					break;
				}
			}

			for (const uintptr_t slot: short_relay_slots) {
				if (relay_parent.contains(slot)) {
					continue;
				}
				const s64 displacement =
				    static_cast<s64>(slot) - static_cast<s64>(current + ShortJumpSize);
				if (displacement < ShortJumpMin || displacement > ShortJumpMax) {
					continue;
				}
				relay_parent.emplace(slot, current);
				relay_queue.push_back(slot);
			}
		}

		if (!host_span && !final_relay_slot) {
			module->trampoline_gen.setSize(trampoline_start);
			record_unsupported(site);
			continue;
		}

		const auto* site_trampoline =
		    module->trampoline_gen.getCode() + *site_trampoline_offset;
		auto&     patch_gen = module->patch_gen;
		uintptr_t relay_address {};
		if (final_relay_slot) {
			relay_address = *final_relay_slot;
			patch_gen.reset();
			patch_gen.setSize(relay_address - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
			patch_gen.jmp(site_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
			EXIT_IF(Xbyak::GetError() != 0);
			std::erase(relay_slots, relay_address);
			std::erase(short_relay_slots, relay_address);
		} else {
			ASSERT(host_span && host_trampoline_offset);
			const auto* host_trampoline =
			    module->trampoline_gen.getCode() + *host_trampoline_offset;
			relay_address = host_span->patch_start + NearJumpSize;

			patch_gen.reset();
			patch_gen.setSize(host_span->patch_start -
			                  reinterpret_cast<uintptr_t>(patch_gen.getCode()));
			patch_gen.jmp(host_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
			patch_gen.jmp(site_trampoline, Xbyak::CodeGenerator::LabelType::T_NEAR);
			patch_gen.nop(host_span->patch_size - NearJumpSize * 2);
			EXIT_IF(Xbyak::GetError() != 0);
		}

		uintptr_t jump_target = relay_address;
		while (final_short_jump != site) {
			patch_gen.reset();
			patch_gen.setSize(final_short_jump -
			                  reinterpret_cast<uintptr_t>(patch_gen.getCode()));
			patch_gen.jmp(reinterpret_cast<void*>(jump_target),
			              Xbyak::CodeGenerator::LabelType::T_SHORT);
			EXIT_IF(Xbyak::GetError() != 0);
			jump_target       = final_short_jump;
			const auto parent = relay_parent.find(final_short_jump);
			ASSERT(parent != relay_parent.end());
			final_short_jump = parent->second;
			std::erase(relay_slots, jump_target);
			std::erase(short_relay_slots, jump_target);
		}

		patch_gen.reset();
		patch_gen.setSize(site - reinterpret_cast<uintptr_t>(patch_gen.getCode()));
		patch_gen.jmp(reinterpret_cast<void*>(jump_target),
		              Xbyak::CodeGenerator::LabelType::T_SHORT);
		patch_gen.nop(site_span.patch_size - ShortJumpSize);
		EXIT_IF(Xbyak::GetError() != 0);

		if (host_span) {
			record_rewrites(*host_span);
			patched_spans.emplace_back(host_span->patch_start, host_span->continuation);
			if (host_span->patch_size >= NearJumpSize * 3) {
				relay_slots.push_back(host_span->patch_start + NearJumpSize * 2);
			}
			if (host_span->patch_size >= NearJumpSize * 2 + ShortJumpSize) {
				short_relay_slots.push_back(host_span->patch_start + NearJumpSize * 2);
			}
		}
		record_rewrites(site_span);
		patched_spans.emplace_back(site_span.patch_start, site_span.continuation);
	}
}
} // namespace

GuestInstructionPatchResult PatchGuestInstructions(u64 segment_addr, u64 segment_size,
                                                   std::span<const uintptr_t> function_starts,
                                                   bool protect_memory, bool emulate_amd) {
	GuestInstructionPatchResult result {};
	if (!protect_memory && !emulate_amd) {
		return result;
	}
	auto*              module = GetContainingModule(reinterpret_cast<void*>(segment_addr));
	if (module == nullptr || function_starts.empty()) {
		return result;
	}

	const uintptr_t        segment_end = segment_addr + segment_size;
	std::vector<uintptr_t> starts;
	starts.reserve(function_starts.size());
	for (const uintptr_t start: function_starts) {
		if (start >= segment_addr && start < segment_end) {
			starts.push_back(start);
		}
	}
	std::ranges::sort(starts);
	const auto unique_end = std::ranges::unique(starts).begin();
	starts.erase(unique_end, starts.end());

	std::unique_lock lock {module->mutex};
	const size_t     trampoline_begin = module->trampoline_gen.getSize();
	bool analyze_red_zone = protect_memory;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	analyze_red_zone |= emulate_amd;
#endif
	for (size_t function_index = 0; function_index < starts.size(); ++function_index) {
		const uintptr_t function_start = starts[function_index];
		const uintptr_t function_end =
		    function_index + 1 < starts.size() ? starts[function_index + 1] : segment_end;
		if (function_end <= function_start) {
			continue;
		}

		++result.function_count;
		auto function = DecodeFunction(function_start, function_end, segment_addr, segment_end);
		if (analyze_red_zone) {
			AnalyzeRedZoneLiveness(function);
		}
		result.instruction_count += function.instructions.size();

		std::map<uintptr_t, InstructionRewrite> rewrite_sites;

		if (function.uses_red_zone) {
			++result.red_zone_function_count;
			result.indirect_red_zone_function_count += function.has_indirect_branch;
		}
		if (protect_memory) {
			CollectRedZoneMemoryInstructions(function, rewrite_sites, result);
		}
		if (emulate_amd) {
			CollectAmdInstructions(function, rewrite_sites, result);
		}
		if (!rewrite_sites.empty()) {
			RelocateGuestInstructions(module, function, rewrite_sites, result);
			TrapUnrelocatedInstructions(*module, function, rewrite_sites, result);
		}
	}
	const auto trampoline_addr =
	    reinterpret_cast<u64>(module->trampoline_gen.getCode()) + trampoline_begin;
	const auto trampoline_size = module->trampoline_gen.getSize() - trampoline_begin;
	Common::VirtualMemory::FlushInstructionCache(segment_addr, segment_size);
	if (trampoline_size != 0) {
		Common::VirtualMemory::FlushInstructionCache(trampoline_addr, trampoline_size);
	}
	return result;
}

#else

GuestInstructionPatchResult PatchGuestInstructions(u64, u64, std::span<const uintptr_t>, bool,
                                                   bool) {
	return {};
}

#endif

namespace {

constexpr uint8_t DW_EH_PE_FORMAT_MASK      = 0x0f;
constexpr uint8_t DW_EH_PE_APPLICATION_MASK = 0x70;
constexpr uint8_t DW_EH_PE_PCREL            = 0x10;
constexpr uint8_t DW_EH_PE_DATAREL          = 0x30;
constexpr uint8_t DW_EH_PE_INDIRECT         = 0x80;
constexpr uint8_t DW_EH_PE_OMIT             = 0xff;

template <typename T>
bool ReadEhValue(const uint8_t** cursor, const uint8_t* end, T* value) {
	if (cursor == nullptr || *cursor == nullptr || value == nullptr ||
	    static_cast<size_t>(end - *cursor) < sizeof(T)) {
		return false;
	}
	std::memcpy(value, *cursor, sizeof(T));
	*cursor += sizeof(T);
	return true;
}

bool ReadEncodedEhPointer(const uint8_t** cursor, const uint8_t* end, uint8_t encoding,
                          uintptr_t datarel_base, uintptr_t* value) {
	if (cursor == nullptr || *cursor == nullptr || value == nullptr || encoding == DW_EH_PE_OMIT) {
		return false;
	}

	const auto field_address = reinterpret_cast<uintptr_t>(*cursor);
	uint64_t   raw           = 0;
	bool       is_signed     = false;
	switch (encoding & DW_EH_PE_FORMAT_MASK) {
		case 0x00: {
			uintptr_t decoded = 0;
			if (!ReadEhValue(cursor, end, &decoded)) {
				return false;
			}
			raw = decoded;
			break;
		}
		case 0x02: {
			uint16_t decoded = 0;
			if (!ReadEhValue(cursor, end, &decoded)) {
				return false;
			}
			raw = decoded;
			break;
		}
		case 0x03: {
			uint32_t decoded = 0;
			if (!ReadEhValue(cursor, end, &decoded)) {
				return false;
			}
			raw = decoded;
			break;
		}
		case 0x04:
			if (!ReadEhValue(cursor, end, &raw)) {
				return false;
			}
			break;
		case 0x0a: {
			int16_t decoded = 0;
			if (!ReadEhValue(cursor, end, &decoded)) {
				return false;
			}
			raw       = static_cast<uint64_t>(static_cast<int64_t>(decoded));
			is_signed = true;
			break;
		}
		case 0x0b: {
			int32_t decoded = 0;
			if (!ReadEhValue(cursor, end, &decoded)) {
				return false;
			}
			raw       = static_cast<uint64_t>(static_cast<int64_t>(decoded));
			is_signed = true;
			break;
		}
		case 0x0c: {
			int64_t decoded = 0;
			if (!ReadEhValue(cursor, end, &decoded)) {
				return false;
			}
			raw       = static_cast<uint64_t>(decoded);
			is_signed = true;
			break;
		}
		default: return false;
	}

	uint64_t base = 0;
	switch (encoding & DW_EH_PE_APPLICATION_MASK) {
		case 0x00: break;
		case DW_EH_PE_PCREL: base = field_address; break;
		case DW_EH_PE_DATAREL: base = datarel_base; break;
		default: return false;
	}

	uint64_t decoded = 0;
	if (is_signed) {
		decoded = static_cast<uint64_t>(static_cast<int64_t>(base) + static_cast<int64_t>(raw));
	} else {
		if (raw > UINT64_MAX - base) {
			return false;
		}
		decoded = base + raw;
	}
	if ((encoding & DW_EH_PE_INDIRECT) != 0) {
		const auto* indirect = reinterpret_cast<const uint8_t*>(decoded);
		uintptr_t   target   = 0;
		std::memcpy(&target, indirect, sizeof(target));
		decoded = target;
	}
	*value = static_cast<uintptr_t>(decoded);
	return true;
}

} // namespace

bool DecodeEhFrameFunctionStarts(uint64_t eh_frame_header_addr, uint64_t eh_frame_header_size,
                                 std::vector<uintptr_t>* function_starts) {
	if (function_starts == nullptr) {
		return false;
	}
	function_starts->clear();
	if (eh_frame_header_addr == 0 || eh_frame_header_size < 4 ||
	    eh_frame_header_size > UINTPTR_MAX - eh_frame_header_addr) {
		return false;
	}

	const auto* start             = reinterpret_cast<const uint8_t*>(eh_frame_header_addr);
	const auto* end               = start + eh_frame_header_size;
	const auto* cursor            = start;
	uint8_t     version           = 0;
	uint8_t     eh_frame_encoding = 0;
	uint8_t     count_encoding    = 0;
	uint8_t     table_encoding    = 0;
	if (!ReadEhValue(&cursor, end, &version) || !ReadEhValue(&cursor, end, &eh_frame_encoding) ||
	    !ReadEhValue(&cursor, end, &count_encoding) ||
	    !ReadEhValue(&cursor, end, &table_encoding) || version != 1) {
		return false;
	}

	uintptr_t ignored_eh_frame = 0;
	if (!ReadEncodedEhPointer(&cursor, end, eh_frame_encoding, eh_frame_header_addr,
	                          &ignored_eh_frame)) {
		return false;
	}
	if (count_encoding == DW_EH_PE_OMIT || table_encoding == DW_EH_PE_OMIT) {
		return true;
	}

	uintptr_t fde_count = 0;
	if (!ReadEncodedEhPointer(&cursor, end, count_encoding, eh_frame_header_addr, &fde_count)) {
		return false;
	}
	if (fde_count > eh_frame_header_size / 2u) {
		return false;
	}

	function_starts->reserve(static_cast<size_t>(fde_count));
	for (uintptr_t index = 0; index < fde_count; ++index) {
		uintptr_t function_start = 0;
		uintptr_t ignored_fde    = 0;
		if (!ReadEncodedEhPointer(&cursor, end, table_encoding, eh_frame_header_addr,
		                          &function_start) ||
		    !ReadEncodedEhPointer(&cursor, end, table_encoding, eh_frame_header_addr,
		                          &ignored_fde)) {
			function_starts->clear();
			return false;
		}
		function_starts->push_back(function_start);
	}
	return true;
}

void RegisterGuestInstructionPatchModule(void* module_ptr, uint64_t module_size,
                                         void* trampoline_area_ptr, uint64_t trampoline_area_size) {
#if !defined(__APPLE__)
	EXIT_IF(module_ptr == nullptr || module_size == 0 || trampoline_area_ptr == nullptr ||
	        trampoline_area_size == 0);
	const auto module_addr = reinterpret_cast<u64>(module_ptr);
	g_patch_modules.erase(module_addr);
	g_patch_modules.emplace(std::piecewise_construct, std::forward_as_tuple(module_addr),
	                        std::forward_as_tuple(static_cast<u8*>(module_ptr), module_size,
	                                              static_cast<u8*>(trampoline_area_ptr),
	                                              trampoline_area_size));
#else
	(void)module_ptr;
	(void)module_size;
	(void)trampoline_area_ptr;
	(void)trampoline_area_size;
#endif
}

void UnregisterGuestInstructionPatchModule(void* module_ptr) {
#if !defined(__APPLE__)
	g_patch_modules.erase(reinterpret_cast<u64>(module_ptr));
#else
	(void)module_ptr;
#endif
}

#undef ASSERT

} // namespace Loader
