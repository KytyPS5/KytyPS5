#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

const IR::DescriptorBinding* DescriptorBinding(const EmitterState&       state,
                                               IR::DescriptorBindingKind kind);
uint32_t DescriptorCount(const EmitterState& state, IR::DescriptorBindingKind kind);

uint32_t TypeVoid(EmitterState& state) {
	return state.builder.Type(OpTypeVoid);
}

uint32_t TypeBool(EmitterState& state) {
	return state.builder.Type(OpTypeBool);
}

uint32_t TypeU32(EmitterState& state) {
	return state.builder.Type(OpTypeInt, {32, 0});
}

uint32_t TypeU32Pair(EmitterState& state) {
	const auto element = TypeU32(state);
	return state.builder.Type(OpTypeStruct, {element, element});
}

uint32_t TypeI32(EmitterState& state) {
	return state.builder.Type(OpTypeInt, {32, 1});
}

uint32_t TypeI32Pair(EmitterState& state) {
	const auto element = TypeI32(state);
	return state.builder.Type(OpTypeStruct, {element, element});
}

uint32_t TypeF32(EmitterState& state) {
	return state.builder.Type(OpTypeFloat, {32});
}

uint32_t TypeU32Vector(EmitterState& state, uint32_t components) {
	return state.builder.Type(OpTypeVector, {TypeU32(state), components});
}

uint32_t TypeI32Vector(EmitterState& state, uint32_t components) {
	return state.builder.Type(OpTypeVector, {TypeI32(state), components});
}

uint32_t TypeF32Vector(EmitterState& state, uint32_t components) {
	return state.builder.Type(OpTypeVector, {TypeF32(state), components});
}

uint32_t TypePointer(EmitterState& state, uint32_t storage_class, uint32_t pointee) {
	return state.builder.Type(OpTypePointer, {storage_class, pointee});
}

uint32_t TypeFunction(EmitterState& state) {
	return state.builder.Type(OpTypeFunction, {TypeVoid(state)});
}

uint32_t StorageRuntimeArrayType(EmitterState& state) {
	return state.builder.DecoratedType(OpTypeRuntimeArray, {TypeU32(state)},
	                                   {{OpDecorate, {DecorationArrayStride, sizeof(uint32_t)}}});
}

uint32_t StorageBufferType(EmitterState& state) {
	return state.builder.DecoratedType(
	    OpTypeStruct, {StorageRuntimeArrayType(state)},
	    {{OpMemberDecorate, {0, DecorationOffset, 0}}, {OpDecorate, {DecorationBlock}}});
}

uint32_t TypeStorageBufferPointer(EmitterState& state) {
	return TypePointer(state, StorageClassStorageBuffer, StorageBufferType(state));
}

uint32_t TypeStorageBufferElementPointer(EmitterState& state) {
	return TypePointer(state, StorageClassStorageBuffer, TypeU32(state));
}

uint32_t TypePushConstantElementPointer(EmitterState& state) {
	return TypePointer(state, StorageClassPushConstant, TypeU32(state));
}

uint32_t TypeLdsArrayPointer(EmitterState& state, uint32_t storage_class) {
	const auto count = ConstantU32(state, std::max(LdsDwordCount(state), 1u));
	const auto array = state.builder.Type(OpTypeArray, {TypeU32(state), count});
	return TypePointer(state, storage_class, array);
}

uint32_t TypeLdsElementPointer(EmitterState& state, uint32_t storage_class) {
	return TypePointer(state, storage_class, TypeU32(state));
}

namespace {

uint32_t PushConstantArrayType(EmitterState& state) {
	const auto count =
	    ConstantU32(state, state.program.bindings.push_constant_size / sizeof(uint32_t));
	return state.builder.DecoratedType(OpTypeArray, {TypeU32(state), count},
	                                   {{OpDecorate, {DecorationArrayStride, sizeof(uint32_t)}}});
}

uint32_t PushConstantBlockType(EmitterState& state) {
	return state.builder.DecoratedType(
	    OpTypeStruct, {PushConstantArrayType(state)},
	    {{OpMemberDecorate, {0, DecorationOffset, state.program.bindings.push_constant_offset}},
	     {OpDecorate, {DecorationBlock}}});
}

uint32_t PerVertexType(EmitterState& state) {
	return state.builder.DecoratedType(OpTypeStruct, {TypeF32Vector(state, 4)},
	                                   {{OpMemberDecorate, {0, DecorationBuiltIn, BuiltInPosition}},
	                                    {OpDecorate, {DecorationBlock}}});
}

uint32_t SampleMaskArrayType(EmitterState& state) {
	return state.builder.Type(OpTypeArray, {TypeI32(state), ConstantU32(state, 1)});
}

void DefineDescriptorVariables(EmitterState& state) {
	if (DescriptorBinding(state, IR::DescriptorBindingKind::Buffers) != nullptr) {
		const auto count =
		    ConstantU32(state, DescriptorCount(state, IR::DescriptorBindingKind::Buffers));
		const auto array_type = state.builder.Type(OpTypeArray, {StorageBufferType(state), count});
		const auto pointer_type = TypePointer(state, StorageClassStorageBuffer, array_type);
		state.storage_buffer_variable =
		    state.builder.DefineGlobalVariable(pointer_type, StorageClassStorageBuffer);
	}
	if (DescriptorBinding(state, IR::DescriptorBindingKind::AddressMemory) != nullptr) {
		const auto count =
		    ConstantU32(state, DescriptorCount(state, IR::DescriptorBindingKind::AddressMemory));
		const auto array_type = state.builder.Type(OpTypeArray, {StorageBufferType(state), count});
		const auto pointer_type = TypePointer(state, StorageClassStorageBuffer, array_type);
		state.address_memory_variable =
		    state.builder.DefineGlobalVariable(pointer_type, StorageClassStorageBuffer);
	}
	if (state.program.bindings.push_constant_size != 0) {
		const auto pointer_type =
		    TypePointer(state, StorageClassPushConstant, PushConstantBlockType(state));
		state.push_constant_variable =
		    state.builder.DefineGlobalVariable(pointer_type, StorageClassPushConstant);
	}
	if (DescriptorBinding(state, IR::DescriptorBindingKind::UserData) != nullptr) {
		state.vsharp_storage_variable = state.builder.DefineGlobalVariable(
		    TypeStorageBufferPointer(state), StorageClassStorageBuffer);
	}
	if (DescriptorBinding(state, IR::DescriptorBindingKind::FlattenedSrt) != nullptr) {
		state.flattened_srt_variable = state.builder.DefineGlobalVariable(
		    TypeStorageBufferPointer(state), StorageClassStorageBuffer);
	}
	for (uint32_t i = 0; i < state.sampled_image_variables.size(); i++) {
		const auto view    = static_cast<ImageViewKind>(i % SampledImageViewKindCount);
		const bool integer = i >= SampledImageViewKindCount;
		const auto kind    = SampledBindingKind(integer, view);
		if (DescriptorBinding(state, kind) == nullptr) {
			continue;
		}
		const auto count        = ConstantU32(state, DescriptorCount(state, kind));
		const auto image_type   = ImageViewImageType(state, view, integer);
		const auto array_type   = state.builder.Type(OpTypeArray, {image_type, count});
		const auto pointer_type = TypePointer(state, StorageClassUniformConstant, array_type);
		state.sampled_image_variables[i] =
		    state.builder.DefineGlobalVariable(pointer_type, StorageClassUniformConstant);
		if (view == ImageViewKind::Dim1D || view == ImageViewKind::Dim1DArray) {
			state.builder.RequireCapability(CapabilitySampled1D);
		}
	}
	for (uint32_t i = 0; i < state.storage_image_variables.size(); i++) {
		const auto view    = static_cast<ImageViewKind>(i % StorageImageViewKindCount);
		const bool integer = i >= StorageImageViewKindCount;
		const auto kind    = StorageBindingKind(integer, view);
		if (DescriptorBinding(state, kind) == nullptr) {
			continue;
		}
		const auto count        = ConstantU32(state, DescriptorCount(state, kind));
		const auto image_type   = StorageImageType(state, integer, view);
		const auto array_type   = state.builder.Type(OpTypeArray, {image_type, count});
		const auto pointer_type = TypePointer(state, StorageClassUniformConstant, array_type);
		state.storage_image_variables[i] =
		    state.builder.DefineGlobalVariable(pointer_type, StorageClassUniformConstant);
		if (view == ImageViewKind::Dim1D || view == ImageViewKind::Dim1DArray) {
			state.builder.RequireCapability(CapabilityImage1D);
		}
	}
	if (DescriptorBinding(state, IR::DescriptorBindingKind::Samplers) != nullptr) {
		const auto sampler_type = state.builder.Type(OpTypeSampler);
		const auto count =
		    ConstantU32(state, DescriptorCount(state, IR::DescriptorBindingKind::Samplers));
		const auto array_type   = state.builder.Type(OpTypeArray, {sampler_type, count});
		const auto pointer_type = TypePointer(state, StorageClassUniformConstant, array_type);
		state.sampler_variable =
		    state.builder.DefineGlobalVariable(pointer_type, StorageClassUniformConstant);
	}
	if (DescriptorBinding(state, IR::DescriptorBindingKind::Gds) != nullptr) {
		state.gds_variable = state.builder.DefineGlobalVariable(TypeStorageBufferPointer(state),
		                                                        StorageClassStorageBuffer);
	}
}

} // namespace

const IR::DescriptorBinding* DescriptorBinding(const EmitterState&       state,
                                               IR::DescriptorBindingKind kind) {
	return IR::FindBinding(state.program.bindings, kind);
}

uint32_t DescriptorCount(const EmitterState& state, IR::DescriptorBindingKind kind) {
	const auto* binding = DescriptorBinding(state, kind);
	return binding != nullptr ? static_cast<uint32_t>(binding->resources.size()) : 0;
}

uint32_t ConstantU32(EmitterState& state, uint32_t value) {
	return state.builder.Constant(OpConstant, TypeU32(state), {value});
}

uint32_t ConstantI32(EmitterState& state, int32_t value) {
	return state.builder.Constant(OpConstant, TypeI32(state), {static_cast<uint32_t>(value)});
}

uint32_t ConstantF32(EmitterState& state, uint32_t bits) {
	return state.builder.Constant(OpConstant, TypeF32(state), {bits});
}

uint32_t FloatBits(float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

uint32_t ConstantF32Value(EmitterState& state, float value) {
	return ConstantF32(state, FloatBits(value));
}

uint32_t ConstantBool(EmitterState& state, bool value) {
	return state.builder.Constant(value ? OpConstantTrue : OpConstantFalse, TypeBool(state));
}

uint32_t ConstantU64(EmitterState& state, uint64_t value) {
	return state.builder.Constant(OpConstantComposite, TypeU32Pair(state),
	                              {ConstantU32(state, static_cast<uint32_t>(value)),
	                               ConstantU32(state, static_cast<uint32_t>(value >> 32u))});
}

uint32_t ConstantU32x4Zero(EmitterState& state) {
	const auto zero = ConstantU32(state, 0);
	return state.builder.Constant(OpConstantComposite, TypeU32Vector(state, 4),
	                              {zero, zero, zero, zero});
}

uint32_t GlslStd450(EmitterState& state) {
	return state.builder.Import("GLSL.std.450");
}

VertexInputScalarKind VertexParameterScalarKind(const EmitterState& state, uint32_t location) {
	if (state.stage != ShaderType::Vertex || location >= ShaderVertexInputInfo::RES_MAX ||
	    location >= static_cast<uint32_t>(state.input_info.vertex->resources_num)) {
		return VertexInputScalarKind::Float;
	}

	switch (state.input_info.vertex->resources[location].Format()) {
		case Prospero::BufferFormat::k8UInt:
		case Prospero::BufferFormat::k16UInt:
		case Prospero::BufferFormat::k8_8UInt:
		case Prospero::BufferFormat::k32UInt:
		case Prospero::BufferFormat::k16_16UInt:
		case Prospero::BufferFormat::k8_8_8_8UInt:
		case Prospero::BufferFormat::k32_32UInt:
		case Prospero::BufferFormat::k16_16_16_16UInt:
		case Prospero::BufferFormat::k32_32_32UInt:
		case Prospero::BufferFormat::k32_32_32_32UInt: return VertexInputScalarKind::Uint;
		case Prospero::BufferFormat::k8SInt:
		case Prospero::BufferFormat::k16SInt:
		case Prospero::BufferFormat::k8_8SInt:
		case Prospero::BufferFormat::k32SInt:
		case Prospero::BufferFormat::k16_16SInt:
		case Prospero::BufferFormat::k8_8_8_8SInt:
		case Prospero::BufferFormat::k32_32SInt:
		case Prospero::BufferFormat::k16_16_16_16SInt:
		case Prospero::BufferFormat::k32_32_32SInt:
		case Prospero::BufferFormat::k32_32_32_32SInt: return VertexInputScalarKind::Sint;
		default: return VertexInputScalarKind::Float;
	}
}

uint32_t VertexParameterComponentCount(const EmitterState& state, const InputBinding& input) {
	uint32_t count = input.component_count;
	if (state.stage == ShaderType::Vertex && input.location < ShaderVertexInputInfo::RES_MAX &&
	    input.location < static_cast<uint32_t>(state.input_info.vertex->resources_num) &&
	    state.input_info.vertex->resources_dst[input.location].registers_num > 0) {
		count = static_cast<uint32_t>(
		    state.input_info.vertex->resources_dst[input.location].registers_num);
	}
	return std::clamp(count, 1u, 4u);
}

uint32_t VertexParameterScalarType(EmitterState& state, VertexInputScalarKind kind) {
	switch (kind) {
		case VertexInputScalarKind::Sint: return TypeI32(state);
		case VertexInputScalarKind::Uint: return TypeU32(state);
		case VertexInputScalarKind::Float:
		default: return TypeF32(state);
	}
}

uint32_t VertexParameterScalarPointerType(EmitterState& state, VertexInputScalarKind kind) {
	switch (kind) {
		case VertexInputScalarKind::Sint:
			return TypePointer(state, StorageClassInput, TypeI32(state));
		case VertexInputScalarKind::Uint:
			return TypePointer(state, StorageClassInput, TypeU32(state));
		case VertexInputScalarKind::Float:
		default: return TypePointer(state, StorageClassInput, TypeF32(state));
	}
}

uint32_t VertexParameterVectorOrScalarType(EmitterState& state, VertexInputScalarKind kind,
                                           uint32_t components) {
	switch (kind) {
		case VertexInputScalarKind::Sint:
			switch (components) {
				case 1: return TypeI32(state);
				case 2: return TypeI32Vector(state, 2);
				case 3: return TypeI32Vector(state, 3);
				default: return TypeI32Vector(state, 4);
			}
		case VertexInputScalarKind::Uint:
			switch (components) {
				case 1: return TypeU32(state);
				case 2: return TypeU32Vector(state, 2);
				case 3: return TypeU32Vector(state, 3);
				default: return TypeU32Vector(state, 4);
			}
		case VertexInputScalarKind::Float:
		default:
			switch (components) {
				case 1: return TypeF32(state);
				case 2: return TypeF32Vector(state, 2);
				case 3: return TypeF32Vector(state, 3);
				default: return TypeF32Vector(state, 4);
			}
	}
}

uint32_t VertexParameterInputPointerType(EmitterState& state, VertexInputScalarKind kind,
                                         uint32_t components) {
	switch (kind) {
		case VertexInputScalarKind::Sint:
			switch (components) {
				case 1: return TypePointer(state, StorageClassInput, TypeI32(state));
				case 2: return TypePointer(state, StorageClassInput, TypeI32Vector(state, 2));
				case 3: return TypePointer(state, StorageClassInput, TypeI32Vector(state, 3));
				default: return TypePointer(state, StorageClassInput, TypeI32Vector(state, 4));
			}
		case VertexInputScalarKind::Uint:
			switch (components) {
				case 1: return TypePointer(state, StorageClassInput, TypeU32(state));
				case 2: return TypePointer(state, StorageClassInput, TypeU32Vector(state, 2));
				case 3: return TypePointer(state, StorageClassInput, TypeU32Vector(state, 3));
				default: return TypePointer(state, StorageClassInput, TypeU32Vector(state, 4));
			}
		case VertexInputScalarKind::Float:
		default:
			switch (components) {
				case 1: return TypePointer(state, StorageClassInput, TypeF32(state));
				case 2: return TypePointer(state, StorageClassInput, TypeF32Vector(state, 2));
				case 3: return TypePointer(state, StorageClassInput, TypeF32Vector(state, 3));
				default: return TypePointer(state, StorageClassInput, TypeF32Vector(state, 4));
			}
	}
}

static bool MrtUsesUintOutput(const EmitterState& state, uint32_t index) {
	return state.stage == ShaderType::Pixel &&
	       index < std::size(state.input_info.pixel->target_output_mode) &&
	       state.input_info.pixel->target_output_mode[index] == 7u;
}

void AllocateInputVariables(EmitterState& state) {
	for (auto& binding: state.inputs) {
		binding.variable_id = state.builder.AllocateId();
		state.interface_variables.push_back(binding.variable_id);
	}
	if (state.requirements.subgroup_local_invocation_id) {
		state.subgroup_local_invocation_id_variable = state.builder.AllocateId();
		state.interface_variables.push_back(state.subgroup_local_invocation_id_variable);
	}
}

static uint32_t AllocateInterfaceVariable(EmitterState& state) {
	const auto variable = state.builder.AllocateId();
	state.interface_variables.push_back(variable);
	return variable;
}

static uint32_t AllocateSharedOutputVariable(EmitterState& state, uint32_t& variable) {
	if (variable == 0) {
		variable = AllocateInterfaceVariable(state);
	}
	return variable;
}

void AllocateOutputVariables(EmitterState& state) {
	for (auto& binding: state.outputs) {
		switch (binding.kind) {
			case IR::StageOutputKind::Position:
				binding.variable_id =
				    AllocateSharedOutputVariable(state, state.per_vertex_variable);
				break;
			case IR::StageOutputKind::Depth:
				binding.variable_id = AllocateSharedOutputVariable(state, state.depth_variable);
				break;
			case IR::StageOutputKind::SampleMask:
				binding.variable_id =
				    AllocateSharedOutputVariable(state, state.sample_mask_variable);
				break;
			case IR::StageOutputKind::Parameter:
			case IR::StageOutputKind::Mrt:
				binding.variable_id = AllocateInterfaceVariable(state);
				break;
		}
	}
}

uint32_t BuiltInForInput(IR::StageInputKind kind) {
	switch (kind) {
		case IR::StageInputKind::VertexIndex: return BuiltInVertexIndex;
		case IR::StageInputKind::InstanceIndex: return BuiltInInstanceIndex;
		case IR::StageInputKind::FragCoord: return BuiltInFragCoord;
		case IR::StageInputKind::FrontFacing: return BuiltInFrontFacing;
		case IR::StageInputKind::WorkgroupId: return BuiltInWorkgroupId;
		case IR::StageInputKind::LocalInvocationId: return BuiltInLocalInvocationId;
		case IR::StageInputKind::LocalInvocationIndex: return BuiltInLocalInvocationIndex;
		case IR::StageInputKind::GlobalInvocationId: return BuiltInGlobalInvocationId;
		default: return UINT32_MAX;
	}
}

void AddInputAnnotationsAndNames(EmitterState& state) {
	if (state.subgroup_local_invocation_id_variable != 0) {
		state.builder.AddName(state.subgroup_local_invocation_id_variable,
		                      "gl_SubgroupInvocationID");
		state.builder.AddAnnotation({OpDecorate, state.subgroup_local_invocation_id_variable,
		                             DecorationBuiltIn, BuiltInSubgroupLocalInvocationId});
		if (state.stage == ShaderType::Pixel) {
			state.builder.AddAnnotation(
			    {OpDecorate, state.subgroup_local_invocation_id_variable, DecorationFlat});
		}
	}
	for (const auto& input: state.inputs) {
		state.builder.AddName(input.variable_id, input.debug_name.c_str());
		if (input.kind == IR::StageInputKind::Parameter) {
			const auto flat = PixelParameterIsFlat(state, input.location);
			if (flat) {
				state.builder.AddAnnotation({OpDecorate, input.variable_id, DecorationFlat});
			}
			if (state.stage == ShaderType::Pixel && state.input_info.pixel->ps_no_perspective &&
			    !flat) {
				state.builder.AddAnnotation(
				    {OpDecorate, input.variable_id, DecorationNoPerspective});
			}
			const auto location = PixelParameterLocation(state, input.location);
			state.builder.AddAnnotation(
			    {OpDecorate, input.variable_id, DecorationLocation, location});
			continue;
		}
		const auto builtin = BuiltInForInput(input.kind);
		if (builtin != UINT32_MAX) {
			state.builder.AddAnnotation(
			    {OpDecorate, input.variable_id, DecorationBuiltIn, builtin});
		}
	}
}

void AddOutputAnnotationsAndNames(EmitterState& state) {
	if (state.per_vertex_variable != 0) {
		state.builder.AddName(PerVertexType(state), "gl_PerVertex");
		state.builder.AddName(state.per_vertex_variable, "outPerVertex");
	}
	if (state.depth_variable != 0) {
		state.builder.AddName(state.depth_variable, "gl_FragDepth");
		state.builder.AddAnnotation(
		    {OpDecorate, state.depth_variable, DecorationBuiltIn, BuiltInFragDepth});
	}
	if (state.sample_mask_variable != 0) {
		state.builder.AddName(state.sample_mask_variable, "gl_SampleMask");
		state.builder.AddAnnotation(
		    {OpDecorate, state.sample_mask_variable, DecorationBuiltIn, BuiltInSampleMask});
	}
	for (const auto& binding: state.outputs) {
		if (binding.kind == IR::StageOutputKind::Parameter ||
		    binding.kind == IR::StageOutputKind::Mrt) {
			state.builder.AddName(binding.variable_id, binding.debug_name.c_str());
			state.builder.AddAnnotation(
			    {OpDecorate, binding.variable_id, DecorationLocation, binding.location});
		}
	}
}

void DecorateDescriptor(EmitterState& state, uint32_t variable, const char* name, uint32_t set,
                        uint32_t binding) {
	if (variable == 0) {
		return;
	}
	state.builder.AddName(variable, name);
	state.builder.AddAnnotation({OpDecorate, variable, DecorationDescriptorSet, set});
	state.builder.AddAnnotation({OpDecorate, variable, DecorationBinding, binding});
}

void AddDescriptorAnnotationsAndNames(EmitterState& state) {
	const auto set      = state.program.bindings.descriptor_set;
	auto       Decorate = [&](uint32_t variable, const char* name, IR::DescriptorBindingKind kind) {
		const auto* binding = DescriptorBinding(state, kind);
		if (binding != nullptr) {
			DecorateDescriptor(state, variable, name, set, binding->binding);
		}
	};
	if (state.storage_buffer_variable != 0) {
		Decorate(state.storage_buffer_variable, "buffers", IR::DescriptorBindingKind::Buffers);
	}
	if (state.address_memory_variable != 0) {
		Decorate(state.address_memory_variable, "address_memory",
		         IR::DescriptorBindingKind::AddressMemory);
	}
	constexpr const char* SampledNames[] = {"sampled_1d",
	                                        "sampled_1d_array",
	                                        "sampled_2d",
	                                        "sampled_2d_array",
	                                        "sampled_3d",
	                                        "sampled_2d_msaa",
	                                        "sampled_2d_msaa_array",
	                                        "sampled_uint_1d",
	                                        "sampled_uint_1d_array",
	                                        "sampled_uint_2d",
	                                        "sampled_uint_2d_array",
	                                        "sampled_uint_3d",
	                                        "sampled_uint_2d_msaa",
	                                        "sampled_uint_2d_msaa_array"};
	for (uint32_t i = 0; i < state.sampled_image_variables.size(); i++) {
		const auto view = static_cast<ImageViewKind>(i % SampledImageViewKindCount);
		Decorate(state.sampled_image_variables[i], SampledNames[i],
		         SampledBindingKind(i >= SampledImageViewKindCount, view));
	}
	constexpr const char* StorageNames[] = {"storage_1d",
	                                        "storage_1d_array",
	                                        "storage_2d",
	                                        "storage_2d_array",
	                                        "storage_3d",
	                                        "storage_uint_1d",
	                                        "storage_uint_1d_array",
	                                        "storage_uint_2d",
	                                        "storage_uint_2d_array",
	                                        "storage_uint_3d"};
	for (uint32_t i = 0; i < state.storage_image_variables.size(); i++) {
		const auto view = static_cast<ImageViewKind>(i % StorageImageViewKindCount);
		Decorate(state.storage_image_variables[i], StorageNames[i],
		         StorageBindingKind(i >= StorageImageViewKindCount, view));
	}
	if (state.sampler_variable != 0) {
		Decorate(state.sampler_variable, "samplers", IR::DescriptorBindingKind::Samplers);
	}
	if (state.gds_variable != 0) {
		Decorate(state.gds_variable, "gds", IR::DescriptorBindingKind::Gds);
	}
	if (state.flattened_srt_variable != 0) {
		Decorate(state.flattened_srt_variable, "flattened_srt",
		         IR::DescriptorBindingKind::FlattenedSrt);
	}
}

void AddVsharpAnnotationsAndNames(EmitterState& state) {
	const auto& bind = state.program.bindings;
	if (state.push_constant_variable != 0) {
		state.builder.AddName(PushConstantBlockType(state), "BufferResource");
		state.builder.AddName(state.push_constant_variable, "vsharp");
	}
	if (state.vsharp_storage_variable != 0) {
		const auto* storage = DescriptorBinding(state, IR::DescriptorBindingKind::UserData);
		if (storage != nullptr) {
			DecorateDescriptor(state, state.vsharp_storage_variable, "user_data",
			                   bind.descriptor_set, storage->binding);
		}
	}
}

void DefineModule(EmitterState& state) {
	DefineDescriptorVariables(state);
	if (state.requirements.function_lds) {
		state.lds_variable = state.builder.AllocateId();
	}
	state.main_func   = state.builder.AllocateId();
	state.entry_label = state.builder.AllocateId();

	state.builder.RequireCapability(CapabilityShader);
	state.builder.RequireCapability(CapabilitySignedZeroInfNanPreserve);
	if (state.requirements.image_gather_extended) {
		state.builder.RequireCapability(CapabilityImageGatherExtended);
	}
	if (state.requirements.subgroup_ballot || state.requirements.subgroup_shuffle ||
	    state.requirements.subgroup_local_invocation_id) {
		state.builder.RequireCapability(CapabilityGroupNonUniform);
	}
	if (state.requirements.subgroup_ballot) {
		state.builder.RequireCapability(CapabilityGroupNonUniformBallot);
	}
	if (state.requirements.subgroup_shuffle) {
		state.builder.RequireCapability(CapabilityGroupNonUniformShuffle);
	}
	if (state.requirements.compute_derivatives && state.stage == ShaderType::Compute) {
		state.builder.RequireCapability(CapabilityComputeDerivativeGroupQuadsKHR);
		state.builder.RequireExtension("SPV_KHR_compute_shader_derivatives");
	}
	state.builder.RequireExtension("SPV_KHR_float_controls");
	state.builder.AddMemoryModel({AddressingModelLogical, MemoryModelGLSL450});
	state.builder.AddEntryPoint(ExecutionModelForStage(state.stage), state.main_func, "main",
	                            state.interface_variables);
	// GCN/RDNA arithmetic preserves 32-bit signed zero, infinity, and NaN. Declaring that
	// contract prevents host compilers from treating synthesized IEEE values as finite.
	state.builder.AddExecutionMode({state.main_func, ExecutionModeSignedZeroInfNanPreserve, 32u});
	if (state.stage == ShaderType::Compute) {
		uint32_t    local_x = state.requirements.compute_derivatives ? 2u : 1u;
		uint32_t    local_y = state.requirements.compute_derivatives ? 2u : 1u;
		uint32_t    local_z = 1u;
		const auto* cs      = state.input_info.compute;
		local_x             = cs->threads_num[0] != 0u ? cs->threads_num[0] : local_x;
		local_y             = cs->threads_num[1] != 0u ? cs->threads_num[1] : local_y;
		local_z             = cs->threads_num[2] != 0u ? cs->threads_num[2] : local_z;
		state.builder.AddExecutionMode(
		    {state.main_func, ExecutionModeLocalSize, local_x, local_y, local_z});
	}
	if (state.stage == ShaderType::Pixel) {
		state.builder.AddExecutionMode({state.main_func, ExecutionModeOriginUpperLeft});
		if (state.depth_variable != 0) {
			state.builder.AddExecutionMode({state.main_func, ExecutionModeDepthReplacing});
		}
		if (state.input_info.pixel->ps_early_z && !state.input_info.pixel->ps_pixel_kill_enable &&
		    !state.input_info.pixel->ps_depth_export_enable &&
		    !state.input_info.pixel->ps_sample_mask_export_enable) {
			state.builder.AddExecutionMode({state.main_func, ExecutionModeEarlyFragmentTests});
		}
	}
	if (state.requirements.compute_derivatives && state.stage == ShaderType::Compute) {
		state.builder.AddExecutionMode({state.main_func, ExecutionModeDerivativeGroupQuadsKHR});
	}
	state.builder.AddName(state.main_func, "main");
	if (state.requirements.function_lds) {
		state.builder.AddName(state.lds_variable, "lds_dwords");
	}
	AddInputAnnotationsAndNames(state);
	AddOutputAnnotationsAndNames(state);
	AddDescriptorAnnotationsAndNames(state);
	AddVsharpAnnotationsAndNames(state);

	if (state.subgroup_local_invocation_id_variable != 0) {
		state.builder.DefineGlobalVariable(state.subgroup_local_invocation_id_variable,
		                                   TypePointer(state, StorageClassInput, TypeU32(state)),
		                                   StorageClassInput);
	}
	for (const auto& input: state.inputs) {
		uint32_t ptr_type = TypePointer(state, StorageClassInput, TypeU32(state));
		switch (input.kind) {
			case IR::StageInputKind::VertexIndex:
			case IR::StageInputKind::InstanceIndex:
				ptr_type = TypePointer(state, StorageClassInput, TypeI32(state));
				break;
			case IR::StageInputKind::WorkgroupId:
			case IR::StageInputKind::LocalInvocationId:
			case IR::StageInputKind::GlobalInvocationId:
				ptr_type = TypePointer(state, StorageClassInput, TypeU32Vector(state, 3));
				break;
			case IR::StageInputKind::FragCoord:
				ptr_type = TypePointer(state, StorageClassInput, TypeF32Vector(state, 4));
				break;
			case IR::StageInputKind::FrontFacing:
				ptr_type = TypePointer(state, StorageClassInput, TypeBool(state));
				break;
			case IR::StageInputKind::Parameter:
				if (state.stage == ShaderType::Vertex) {
					const auto kind       = VertexParameterScalarKind(state, input.location);
					const auto components = VertexParameterComponentCount(state, input);
					ptr_type = VertexParameterInputPointerType(state, kind, components);
				} else {
					ptr_type = TypePointer(state, StorageClassInput, TypeF32Vector(state, 4));
				}
				break;
			default: break;
		}
		state.builder.DefineGlobalVariable(input.variable_id, ptr_type, StorageClassInput);
	}
	if (state.per_vertex_variable != 0) {
		state.builder.DefineGlobalVariable(
		    state.per_vertex_variable, TypePointer(state, StorageClassOutput, PerVertexType(state)),
		    StorageClassOutput);
	}
	for (const auto& binding: state.outputs) {
		if (binding.kind == IR::StageOutputKind::Parameter ||
		    binding.kind == IR::StageOutputKind::Mrt) {
			const auto pointer_type =
			    binding.kind == IR::StageOutputKind::Mrt && MrtUsesUintOutput(state, binding.index)
			        ? TypePointer(state, StorageClassOutput, TypeU32Vector(state, 4))
			        : TypePointer(state, StorageClassOutput, TypeF32Vector(state, 4));
			state.builder.DefineGlobalVariable(binding.variable_id, pointer_type,
			                                   StorageClassOutput);
		}
	}
	if (state.depth_variable != 0) {
		state.builder.DefineGlobalVariable(state.depth_variable,
		                                   TypePointer(state, StorageClassOutput, TypeF32(state)),
		                                   StorageClassOutput);
	}
	if (state.sample_mask_variable != 0) {
		state.builder.DefineGlobalVariable(
		    state.sample_mask_variable,
		    TypePointer(state, StorageClassOutput, SampleMaskArrayType(state)), StorageClassOutput);
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
