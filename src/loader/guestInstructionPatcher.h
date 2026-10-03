// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef KYTY_LOADER_GUEST_INSTRUCTION_PATCHER_H_
#define KYTY_LOADER_GUEST_INSTRUCTION_PATCHER_H_

#include "common/common.h"

#include <span>
#include <vector>

namespace Loader {

struct GuestInstructionPatchResult {
	uint64_t function_count                            = 0;
	uint64_t instruction_count                         = 0;
	uint64_t red_zone_function_count                   = 0;
	uint64_t memory_instruction_count                  = 0;
	uint64_t short_memory_instruction_count            = 0;
	uint64_t patched_memory_instruction_count          = 0;
	uint64_t stack_dependent_memory_instruction_count  = 0;
	uint64_t control_flow_memory_instruction_count     = 0;
	uint64_t unrelocatable_memory_instruction_count    = 0;
	uint64_t indirect_red_zone_function_count          = 0;
	uint64_t reciprocal_sqrt_instruction_count         = 0;
	uint64_t patched_reciprocal_sqrt_instruction_count = 0;
	uint64_t trapped_reciprocal_sqrt_instruction_count = 0;
};

void RegisterGuestInstructionPatchModule(void* module_ptr, uint64_t module_size,
                                         void* trampoline_area_ptr, uint64_t trampoline_area_size);
void UnregisterGuestInstructionPatchModule(void* module_ptr);

// Apply enabled instruction fixes using native trampolines or safe trap fallbacks.
GuestInstructionPatchResult PatchGuestInstructions(uint64_t segment_addr, uint64_t segment_size,
                                                   std::span<const uintptr_t> function_starts,
                                                   bool protect_memory, bool emulate_rsqrt);

bool DecodeEhFrameFunctionStarts(uint64_t eh_frame_header_addr, uint64_t eh_frame_header_size,
                                 std::vector<uintptr_t>* function_starts);

} // namespace Loader

#endif /* KYTY_LOADER_GUEST_INSTRUCTION_PATCHER_H_ */
