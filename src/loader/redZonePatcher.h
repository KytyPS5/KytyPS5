// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef KYTY_LOADER_RED_ZONE_PATCHER_H_
#define KYTY_LOADER_RED_ZONE_PATCHER_H_

#include "common/common.h"

#include <span>
#include <vector>

namespace Loader {

struct RedZonePatchResult {
	uint64_t function_count                           = 0;
	uint64_t instruction_count                        = 0;
	uint64_t red_zone_function_count                   = 0;
	uint64_t memory_instruction_count                 = 0;
	uint64_t short_memory_instruction_count           = 0;
	uint64_t patched_memory_instruction_count         = 0;
	uint64_t stack_dependent_memory_instruction_count = 0;
	uint64_t control_flow_memory_instruction_count    = 0;
	uint64_t unrelocatable_memory_instruction_count   = 0;
	uint64_t indirect_red_zone_function_count         = 0;
	uint64_t reciprocal_sqrt_instruction_count        = 0;
	uint64_t frame_pointer_red_zone_function_count    = 0;
	uint64_t unwind_function_count                    = 0;
	uint64_t call_target_function_count               = 0;
	uint64_t code_pointer_function_count              = 0;
	uint64_t padding_function_count                   = 0;
	uint64_t analyzed_function_count                  = 0;
	uint64_t restricted_function_count                = 0;
	uint64_t swept_instruction_count                  = 0;
	uint64_t sweep_decode_failure_count               = 0;
	uint64_t misaligned_unwind_start_count            = 0;
	uint64_t misaligned_call_target_count             = 0;
	uint64_t trampoline_bytes                         = 0;
	bool     sweep_trusted                            = false;
};

struct RedZoneFunctionRange {
	uintptr_t start = 0;
	uint64_t  size  = 0;
};

struct RedZoneCodeHints {
	std::span<const RedZoneFunctionRange> unwind_functions;
	std::span<const uintptr_t>            code_pointers;
	bool                                  execute_only = false;
};

void RegisterRedZonePatchModule(void* module_ptr, uint64_t module_size, void* trampoline_area_ptr,
                                uint64_t trampoline_area_size);
void UnregisterRedZonePatchModule(void* module_ptr);

// Analyze and protect fault sites before replacing reciprocal roots with traps.
RedZonePatchResult PatchGuestInstructions(uint64_t segment_addr, uint64_t segment_size,
                                          const RedZoneCodeHints& hints, bool protect_memory,
                                          bool emulate_rsqrt);

bool DecodeEhFrameFunctions(uint64_t eh_frame_header_addr, uint64_t eh_frame_header_size,
                            std::vector<RedZoneFunctionRange>* functions);

} // namespace Loader

#endif /* KYTY_LOADER_RED_ZONE_PATCHER_H_ */
