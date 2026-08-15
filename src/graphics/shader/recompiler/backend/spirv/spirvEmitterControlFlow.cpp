#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

bool UserDataDwordIndex(const EmitterState& state, IR::Register reg, uint32_t& dword_index) {
	if (reg.file != IR::RegisterFile::Scalar) {
		return false;
	}
	const auto& registers = state.program.bindings.user_data_registers;
	const auto  found     = std::lower_bound(registers.begin(), registers.end(), reg.index);
	if (found == registers.end() || *found != reg.index) {
		return false;
	}
	dword_index = static_cast<uint32_t>(found - registers.begin());
	return true;
}

uint32_t EmitShaderDataDwordLoad(EmitterState& state, uint32_t dword_index) {
	if (state.push_constant_variable != 0) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, state.ptr_push_constant_uint, pointer,
		                           state.push_constant_variable, ConstantU32(state, 0),
		                           ConstantU32(state, dword_index)});
		state.builder.AddFunction({OpLoad, state.uint_type, value, pointer});
		return value;
	}
	if (state.vsharp_storage_variable != 0) {
		const auto pointer = state.builder.AllocateId();
		const auto value   = state.builder.AllocateId();
		state.builder.AddFunction({OpAccessChain, state.ptr_storage_buffer_uint, pointer,
		                           state.vsharp_storage_variable, ConstantU32(state, 0),
		                           ConstantU32(state, dword_index)});
		state.builder.AddFunction({OpLoad, state.uint_type, value, pointer});
		return value;
	}
	return ConstantU32(state, 0);
}

uint32_t InitialRegisterValue(const EmitterState& state, IR::Register reg) {
	if (reg.file != IR::RegisterFile::Exec) {
		return 0;
	}
	if (state.per_invocation_masks) {
		return reg.index == 0 ? 1u : 0u;
	}
	return reg.index == 0 || state.wave_size == 64u ? 0xffffffffu : 0u;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
