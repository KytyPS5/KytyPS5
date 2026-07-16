#include "graphics/shader/recompiler/spirvEmitter/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

const IR::DescriptorBinding* DescriptorBinding(const EmitterState&       state,
                                               IR::DescriptorBindingKind kind) {
	return IR::FindBinding(state.program->bindings, kind);
}

uint32_t DescriptorCount(const EmitterState& state, IR::DescriptorBindingKind kind) {
	const auto* binding = DescriptorBinding(state, kind);
	return binding != nullptr ? static_cast<uint32_t>(binding->resources.size()) : 0;
}

uint32_t ConstantU32(EmitterState* state, uint32_t value) {
	if (auto it = state->constants.find(value); it != state->constants.end()) {
		return it->second;
	}

	const auto id = state->builder.AllocateId();
	state->constants.emplace(value, id);
	state->builder.AddType({OpConstant, state->uint_type, id, value});
	return id;
}

uint32_t ConstantI32(EmitterState* state, int32_t value) {
	const auto bits = static_cast<uint32_t>(value);
	if (auto it = state->signed_constants.find(bits); it != state->signed_constants.end()) {
		return it->second;
	}

	const auto id = state->builder.AllocateId();
	state->signed_constants.emplace(bits, id);
	state->builder.AddType({OpConstant, state->int_type, id, bits});
	return id;
}

uint32_t ConstantF32(EmitterState* state, uint32_t bits) {
	if (auto it = state->float_constants.find(bits); it != state->float_constants.end()) {
		return it->second;
	}

	const auto id = state->builder.AllocateId();
	state->float_constants.emplace(bits, id);
	state->builder.AddType({OpConstant, state->float_type, id, bits});
	return id;
}

uint32_t FloatBits(float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	return bits;
}

uint32_t ConstantF32Value(EmitterState* state, float value) {
	return ConstantF32(state, FloatBits(value));
}

VertexInputScalarKind VertexParameterScalarKind(const EmitterState& state, uint32_t location) {
	if (state.stage != ShaderType::Vertex || state.vertex_input_info == nullptr ||
	    location >= ShaderVertexInputInfo::RES_MAX ||
	    location >= static_cast<uint32_t>(state.vertex_input_info->resources_num)) {
		return VertexInputScalarKind::Float;
	}

	switch (state.vertex_input_info->resources[location].Format()) {
		case 5:
		case 11:
		case 18:
		case 20:
		case 27:
		case 60:
		case 62:
		case 69:
		case 72:
		case 75: return VertexInputScalarKind::Uint;
		case 6:
		case 12:
		case 19:
		case 21:
		case 28:
		case 61:
		case 63:
		case 70:
		case 73:
		case 76: return VertexInputScalarKind::Sint;
		default: return VertexInputScalarKind::Float;
	}
}

uint32_t VertexParameterComponentCount(const EmitterState& state, const InputBinding& input) {
	uint32_t count = input.component_count;
	if (state.stage == ShaderType::Vertex && state.vertex_input_info != nullptr &&
	    input.location < ShaderVertexInputInfo::RES_MAX &&
	    input.location < static_cast<uint32_t>(state.vertex_input_info->resources_num) &&
	    state.vertex_input_info->resources_dst[input.location].registers_num > 0) {
		count = static_cast<uint32_t>(
		    state.vertex_input_info->resources_dst[input.location].registers_num);
	}
	return std::clamp(count, 1u, 4u);
}

uint32_t VertexParameterScalarType(const EmitterState& state, VertexInputScalarKind kind) {
	switch (kind) {
		case VertexInputScalarKind::Sint: return state.int_type;
		case VertexInputScalarKind::Uint: return state.uint_type;
		case VertexInputScalarKind::Float:
		default: return state.float_type;
	}
}

uint32_t VertexParameterScalarPointerType(const EmitterState& state, VertexInputScalarKind kind) {
	switch (kind) {
		case VertexInputScalarKind::Sint: return state.ptr_input_int;
		case VertexInputScalarKind::Uint: return state.ptr_input_uint;
		case VertexInputScalarKind::Float:
		default: return state.ptr_input_float;
	}
}

uint32_t VertexParameterVectorOrScalarType(const EmitterState& state, VertexInputScalarKind kind,
                                           uint32_t components) {
	switch (kind) {
		case VertexInputScalarKind::Sint:
			switch (components) {
				case 1: return state.int_type;
				case 2: return state.vec2_int_type;
				case 3: return state.vec3_int_type;
				default: return state.vec4_int_type;
			}
		case VertexInputScalarKind::Uint:
			switch (components) {
				case 1: return state.uint_type;
				case 2: return state.vec2_uint_type;
				case 3: return state.vec3_uint_type;
				default: return state.vec4_uint_type;
			}
		case VertexInputScalarKind::Float:
		default:
			switch (components) {
				case 1: return state.float_type;
				case 2: return state.vec2_float_type;
				case 3: return state.vec3_float_type;
				default: return state.vec4_float_type;
			}
	}
}

uint32_t VertexParameterInputPointerType(const EmitterState& state, VertexInputScalarKind kind,
                                         uint32_t components) {
	switch (kind) {
		case VertexInputScalarKind::Sint:
			switch (components) {
				case 1: return state.ptr_input_int;
				case 2: return state.ptr_input_vec2_int;
				case 3: return state.ptr_input_vec3_int;
				default: return state.ptr_input_vec4_int;
			}
		case VertexInputScalarKind::Uint:
			switch (components) {
				case 1: return state.ptr_input_uint;
				case 2: return state.ptr_input_vec2_uint;
				case 3: return state.ptr_input_vec3_uint;
				default: return state.ptr_input_vec4_uint;
			}
		case VertexInputScalarKind::Float:
		default:
			switch (components) {
				case 1: return state.ptr_input_float;
				case 2: return state.ptr_input_vec2_float;
				case 3: return state.ptr_input_vec3_float;
				default: return state.ptr_input_vec4_float;
			}
	}
}

void AllocateInputVariables(EmitterState* state) {
	for (auto& binding: state->inputs) {
		binding.variable_id = state->builder.AllocateId();
		state->interface_variables.push_back(binding.variable_id);
	}
	if (state->needs_subgroup_local_invocation_id) {
		state->subgroup_local_invocation_id_variable = state->builder.AllocateId();
		state->interface_variables.push_back(state->subgroup_local_invocation_id_variable);
	}
}

static uint32_t AllocateInterfaceVariable(EmitterState* state) {
	const auto variable = state->builder.AllocateId();
	state->interface_variables.push_back(variable);
	return variable;
}

static uint32_t AllocateSharedOutputVariable(EmitterState* state, uint32_t& variable) {
	if (variable == 0) {
		variable = AllocateInterfaceVariable(state);
	}
	return variable;
}

void AllocateOutputVariables(EmitterState* state) {
	for (auto& binding: state->outputs) {
		switch (binding.kind) {
			case IR::StageOutputKind::Position:
				binding.variable_id =
				    AllocateSharedOutputVariable(state, state->per_vertex_variable);
				break;
			case IR::StageOutputKind::Depth:
				binding.variable_id = AllocateSharedOutputVariable(state, state->depth_variable);
				break;
			case IR::StageOutputKind::SampleMask:
				binding.variable_id =
				    AllocateSharedOutputVariable(state, state->sample_mask_variable);
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

void AddInputAnnotationsAndNames(EmitterState* state) {
	if (state->subgroup_local_invocation_id_variable != 0) {
		state->builder.AddName(state->subgroup_local_invocation_id_variable,
		                       "gl_SubgroupInvocationID");
		state->builder.AddAnnotation({OpDecorate, state->subgroup_local_invocation_id_variable,
		                              DecorationBuiltIn, BuiltInSubgroupLocalInvocationId});
		if (state->stage == ShaderType::Pixel) {
			state->builder.AddAnnotation(
			    {OpDecorate, state->subgroup_local_invocation_id_variable, DecorationFlat});
		}
	}
	for (const auto& input: state->inputs) {
		state->builder.AddName(input.variable_id, input.debug_name.c_str());
		if (input.kind == IR::StageInputKind::Parameter) {
			const auto flat = PixelParameterIsFlat(*state, input.location);
			if (flat) {
				state->builder.AddAnnotation({OpDecorate, input.variable_id, DecorationFlat});
			}
			if (state->stage == ShaderType::Pixel && state->pixel_input_info != nullptr &&
			    state->pixel_input_info->ps_no_perspective && !flat) {
				state->builder.AddAnnotation(
				    {OpDecorate, input.variable_id, DecorationNoPerspective});
			}
			const auto location = PixelParameterLocation(*state, input.location);
			state->builder.AddAnnotation(
			    {OpDecorate, input.variable_id, DecorationLocation, location});
			continue;
		}
		const auto builtin = BuiltInForInput(input.kind);
		if (builtin != UINT32_MAX) {
			state->builder.AddAnnotation(
			    {OpDecorate, input.variable_id, DecorationBuiltIn, builtin});
		}
	}
}

void AddOutputAnnotationsAndNames(EmitterState* state) {
	if (state->per_vertex_variable != 0) {
		state->builder.AddName(state->per_vertex_type, "gl_PerVertex");
		state->builder.AddName(state->per_vertex_variable, "outPerVertex");
		state->builder.AddAnnotation(
		    {OpMemberDecorate, state->per_vertex_type, 0, DecorationBuiltIn, BuiltInPosition});
		state->builder.AddAnnotation({OpDecorate, state->per_vertex_type, DecorationBlock});
	}
	if (state->depth_variable != 0) {
		state->builder.AddName(state->depth_variable, "gl_FragDepth");
		state->builder.AddAnnotation(
		    {OpDecorate, state->depth_variable, DecorationBuiltIn, BuiltInFragDepth});
	}
	if (state->sample_mask_variable != 0) {
		state->builder.AddName(state->sample_mask_variable, "gl_SampleMask");
		state->builder.AddAnnotation(
		    {OpDecorate, state->sample_mask_variable, DecorationBuiltIn, BuiltInSampleMask});
	}
	for (const auto& binding: state->outputs) {
		if (binding.kind == IR::StageOutputKind::Parameter ||
		    binding.kind == IR::StageOutputKind::Mrt) {
			state->builder.AddName(binding.variable_id, binding.debug_name.c_str());
			state->builder.AddAnnotation(
			    {OpDecorate, binding.variable_id, DecorationLocation, binding.location});
		}
	}
}

void DecorateDescriptor(EmitterState* state, uint32_t variable, const char* name, uint32_t set,
                        uint32_t binding) {
	if (variable == 0) {
		return;
	}
	state->builder.AddName(variable, name);
	state->builder.AddAnnotation({OpDecorate, variable, DecorationDescriptorSet, set});
	state->builder.AddAnnotation({OpDecorate, variable, DecorationBinding, binding});
}

void AddDescriptorAnnotationsAndNames(EmitterState* state) {
	const auto set      = state->program->bindings.descriptor_set;
	auto       Decorate = [&](uint32_t variable, const char* name, IR::DescriptorBindingKind kind) {
		const auto* binding = DescriptorBinding(*state, kind);
		if (binding != nullptr) {
			DecorateDescriptor(state, variable, name, set, binding->binding);
		}
	};
	if (state->storage_buffer_variable != 0) {
		Decorate(state->storage_buffer_variable, "buffers", IR::DescriptorBindingKind::Buffers);
	}
	if (state->address_memory_variable != 0) {
		Decorate(state->address_memory_variable, "address_memory",
		         IR::DescriptorBindingKind::AddressMemory);
	}
	constexpr const char* SampledNames[] = {
	    "sampled_2d",      "sampled_2d_array",      "sampled_3d",
	    "sampled_uint_2d", "sampled_uint_2d_array", "sampled_uint_3d"};
	for (uint32_t i = 0; i < state->sampled_images.size(); i++) {
		const auto view = static_cast<ImageViewKind>(i % 3u);
		Decorate(state->sampled_images[i].variable, SampledNames[i],
		         SampledBindingKind(i >= 3u, view));
	}
	if (state->storage_image_variable != 0) {
		Decorate(state->storage_image_variable, "textures2D_L",
		         IR::DescriptorBindingKind::Storage2D);
	}
	if (state->storage_image_uint_variable != 0) {
		Decorate(state->storage_image_uint_variable, "textures2D_L_U",
		         IR::DescriptorBindingKind::StorageUint2D);
	}
	if (state->storage_image_2d_array_variable != 0) {
		Decorate(state->storage_image_2d_array_variable, "textures2D_L_A",
		         IR::DescriptorBindingKind::Storage2DArray);
	}
	if (state->storage_image_uint_2d_array_variable != 0) {
		Decorate(state->storage_image_uint_2d_array_variable, "textures2D_L_U_A",
		         IR::DescriptorBindingKind::StorageUint2DArray);
	}
	if (state->storage_image_3d_variable != 0) {
		Decorate(state->storage_image_3d_variable, "textures2D_L_3D",
		         IR::DescriptorBindingKind::Storage3D);
	}
	if (state->storage_image_uint_3d_variable != 0) {
		Decorate(state->storage_image_uint_3d_variable, "textures2D_L_U_3D",
		         IR::DescriptorBindingKind::StorageUint3D);
	}
	if (state->sampler_variable != 0) {
		Decorate(state->sampler_variable, "samplers", IR::DescriptorBindingKind::Samplers);
	}
	if (state->gds_variable != 0) {
		Decorate(state->gds_variable, "gds", IR::DescriptorBindingKind::Gds);
	}
	if (state->flattened_srt_variable != 0) {
		Decorate(state->flattened_srt_variable, "flattened_srt",
		         IR::DescriptorBindingKind::FlattenedSrt);
	}
}

void AddVsharpAnnotationsAndNames(EmitterState* state) {
	const auto& bind = state->program->bindings;
	if (state->push_constant_variable != 0) {
		state->builder.AddName(state->push_constant_block_type, "BufferResource");
		state->builder.AddName(state->push_constant_variable, "vsharp");
		state->builder.AddAnnotation(
		    {OpDecorate, state->push_constant_u32x4_array_type, DecorationArrayStride, 4});
		state->builder.AddAnnotation(
		    {OpDecorate, state->push_constant_rows_array_type, DecorationArrayStride, 16});
		state->builder.AddAnnotation({OpMemberDecorate, state->push_constant_block_type, 0,
		                              DecorationOffset, bind.push_constant_offset});
		state->builder.AddAnnotation(
		    {OpDecorate, state->push_constant_block_type, DecorationBlock});
	}
	if (state->vsharp_storage_variable != 0) {
		const auto* storage = DescriptorBinding(*state, IR::DescriptorBindingKind::UserData);
		if (storage != nullptr) {
			DecorateDescriptor(state, state->vsharp_storage_variable, "user_data",
			                   bind.descriptor_set, storage->binding);
		}
	}
}

void EmitHeaderAndTypes(EmitterState* state) {
	state->void_type                    = state->builder.AllocateId();
	state->bool_type                    = state->builder.AllocateId();
	state->uint_type                    = state->builder.AllocateId();
	state->uint_pair_type               = state->builder.AllocateId();
	state->int_type                     = state->builder.AllocateId();
	state->int_pair_type                = state->builder.AllocateId();
	state->float_type                   = state->builder.AllocateId();
	state->vec2_uint_type               = state->builder.AllocateId();
	state->vec3_uint_type               = state->builder.AllocateId();
	state->vec4_uint_type               = state->builder.AllocateId();
	state->vec2_int_type                = state->builder.AllocateId();
	state->vec3_int_type                = state->builder.AllocateId();
	state->vec4_int_type                = state->builder.AllocateId();
	state->vec2_float_type              = state->builder.AllocateId();
	state->vec3_float_type              = state->builder.AllocateId();
	state->vec4_float_type              = state->builder.AllocateId();
	state->ptr_func_uint                = state->builder.AllocateId();
	state->ptr_input_float              = state->builder.AllocateId();
	state->ptr_input_bool               = state->builder.AllocateId();
	state->ptr_input_int                = state->builder.AllocateId();
	state->ptr_input_uint               = state->builder.AllocateId();
	state->ptr_input_vec2_float         = state->builder.AllocateId();
	state->ptr_input_vec3_float         = state->builder.AllocateId();
	state->ptr_input_vec2_int           = state->builder.AllocateId();
	state->ptr_input_vec3_int           = state->builder.AllocateId();
	state->ptr_input_vec4_int           = state->builder.AllocateId();
	state->ptr_input_vec2_uint          = state->builder.AllocateId();
	state->ptr_input_vec3_uint          = state->builder.AllocateId();
	state->ptr_input_vec4_uint          = state->builder.AllocateId();
	state->ptr_input_vec4_float         = state->builder.AllocateId();
	state->sample_mask_array_type       = state->builder.AllocateId();
	state->ptr_output_int               = state->builder.AllocateId();
	state->ptr_output_sample_mask_array = state->builder.AllocateId();
	state->ptr_output_float             = state->builder.AllocateId();
	state->ptr_output_vec4_float        = state->builder.AllocateId();
	state->per_vertex_type              = state->builder.AllocateId();
	state->ptr_output_per_vertex        = state->builder.AllocateId();
	state->storage_runtime_array_type   = state->builder.AllocateId();
	state->storage_buffer_type          = state->builder.AllocateId();
	state->ptr_storage_buffer           = state->builder.AllocateId();
	state->ptr_storage_buffer_uint      = state->builder.AllocateId();
	state->storage_buffer_array_type    = state->builder.AllocateId();
	state->ptr_storage_buffer_array     = state->builder.AllocateId();
	if (state->address_memory_variable != 0) {
		state->address_memory_array_type = state->builder.AllocateId();
		state->ptr_address_memory_array  = state->builder.AllocateId();
	}
	if (state->push_constant_variable != 0) {
		state->push_constant_u32x4_array_type = state->builder.AllocateId();
		state->push_constant_rows_array_type  = state->builder.AllocateId();
		state->push_constant_block_type       = state->builder.AllocateId();
		state->ptr_push_constant_block        = state->builder.AllocateId();
		state->ptr_push_constant_uint         = state->builder.AllocateId();
	}
	state->lds_array_type      = state->builder.AllocateId();
	state->ptr_workgroup_array = state->builder.AllocateId();
	state->ptr_workgroup_uint  = state->builder.AllocateId();
	state->lds_variable        = state->builder.AllocateId();
	for (auto& image: state->sampled_images) {
		image.image_type         = state->builder.AllocateId();
		image.sampled_image_type = state->builder.AllocateId();
		image.pointer_type       = state->builder.AllocateId();
		image.array_type         = state->builder.AllocateId();
		image.array_pointer_type = state->builder.AllocateId();
	}
	state->sampler_type                                  = state->builder.AllocateId();
	state->sampler_array_type                            = state->builder.AllocateId();
	state->ptr_uniform_sampler                           = state->builder.AllocateId();
	state->ptr_uniform_sampler_array                     = state->builder.AllocateId();
	state->storage_image_type                            = state->builder.AllocateId();
	state->ptr_uniform_storage_image                     = state->builder.AllocateId();
	state->storage_image_array_type                      = state->builder.AllocateId();
	state->ptr_uniform_storage_image_array               = state->builder.AllocateId();
	state->storage_image_2d_array_type                   = state->builder.AllocateId();
	state->ptr_uniform_storage_image_2d_array            = state->builder.AllocateId();
	state->storage_image_2d_array_array_type             = state->builder.AllocateId();
	state->ptr_uniform_storage_image_2d_array_array      = state->builder.AllocateId();
	state->storage_image_3d_type                         = state->builder.AllocateId();
	state->ptr_uniform_storage_image_3d                  = state->builder.AllocateId();
	state->storage_image_3d_array_type                   = state->builder.AllocateId();
	state->ptr_uniform_storage_image_3d_array            = state->builder.AllocateId();
	state->storage_image_uint_type                       = state->builder.AllocateId();
	state->ptr_uniform_storage_image_uint                = state->builder.AllocateId();
	state->storage_image_uint_array_type                 = state->builder.AllocateId();
	state->ptr_uniform_storage_image_uint_array          = state->builder.AllocateId();
	state->storage_image_uint_2d_array_type              = state->builder.AllocateId();
	state->ptr_uniform_storage_image_uint_2d_array       = state->builder.AllocateId();
	state->storage_image_uint_2d_array_array_type        = state->builder.AllocateId();
	state->ptr_uniform_storage_image_uint_2d_array_array = state->builder.AllocateId();
	state->storage_image_uint_3d_type                    = state->builder.AllocateId();
	state->ptr_uniform_storage_image_uint_3d             = state->builder.AllocateId();
	state->storage_image_uint_3d_array_type              = state->builder.AllocateId();
	state->ptr_uniform_storage_image_uint_3d_array       = state->builder.AllocateId();
	state->ptr_image_uint                                = state->builder.AllocateId();
	state->func_type                                     = state->builder.AllocateId();
	state->main_func                                     = state->builder.AllocateId();
	state->entry_label                                   = state->builder.AllocateId();
	state->glsl_std450                                   = state->builder.AllocateId();

	state->builder.AddCapability({CapabilityShader});
	state->builder.AddCapability({CapabilityImageQuery});
	if (state->needs_image_gather_extended) {
		state->builder.AddCapability({CapabilityImageGatherExtended});
	}
	if (state->storage_image_variable != 0 || state->storage_image_2d_array_variable != 0 ||
	    state->storage_image_3d_variable != 0) {
		state->builder.AddCapability({CapabilityStorageImageReadWithoutFormat});
		state->builder.AddCapability({CapabilityStorageImageWriteWithoutFormat});
	}
	if (state->needs_subgroup_ballot || state->needs_subgroup_shuffle ||
	    state->needs_subgroup_local_invocation_id) {
		state->builder.AddCapability({CapabilityGroupNonUniform});
	}
	if (state->needs_subgroup_ballot) {
		state->builder.AddCapability({CapabilityGroupNonUniformBallot});
	}
	if (state->needs_subgroup_shuffle) {
		state->builder.AddCapability({CapabilityGroupNonUniformShuffle});
	}
	if (state->needs_compute_derivatives && state->stage == ShaderType::Compute) {
		state->builder.AddCapability({CapabilityComputeDerivativeGroupQuadsKHR});
		state->builder.AddExtension("SPV_KHR_compute_shader_derivatives");
	}
	state->builder.AddExtInstImport(state->glsl_std450, "GLSL.std.450");
	state->builder.AddMemoryModel({AddressingModelLogical, MemoryModelGLSL450});
	state->builder.AddEntryPoint(ExecutionModelForStage(state->stage), state->main_func, "main",
	                             state->interface_variables);
	if (state->stage == ShaderType::Compute) {
		uint32_t local_x = state->needs_compute_derivatives ? 2u : 1u;
		uint32_t local_y = state->needs_compute_derivatives ? 2u : 1u;
		uint32_t local_z = 1u;
		if (state->compute_input_info != nullptr) {
			const auto* cs = state->compute_input_info;
			local_x        = cs->threads_num[0] != 0u ? cs->threads_num[0] : local_x;
			local_y        = cs->threads_num[1] != 0u ? cs->threads_num[1] : local_y;
			local_z        = cs->threads_num[2] != 0u ? cs->threads_num[2] : local_z;
		}
		state->builder.AddExecutionMode(
		    {state->main_func, ExecutionModeLocalSize, local_x, local_y, local_z});
	}
	if (state->stage == ShaderType::Pixel) {
		state->builder.AddExecutionMode({state->main_func, ExecutionModeOriginUpperLeft});
		if (state->depth_variable != 0) {
			state->builder.AddExecutionMode({state->main_func, ExecutionModeDepthReplacing});
		}
		if (state->pixel_input_info != nullptr && state->pixel_input_info->ps_early_z &&
		    !state->pixel_input_info->ps_pixel_kill_enable &&
		    !state->pixel_input_info->ps_depth_export_enable &&
		    !state->pixel_input_info->ps_sample_mask_export_enable) {
			state->builder.AddExecutionMode({state->main_func, ExecutionModeEarlyFragmentTests});
		}
	}
	if (state->needs_compute_derivatives && state->stage == ShaderType::Compute) {
		state->builder.AddExecutionMode({state->main_func, ExecutionModeDerivativeGroupQuadsKHR});
	}
	state->builder.AddName(state->main_func, "main");
	if (state->stage == ShaderType::Compute || state->needs_function_lds) {
		state->builder.AddName(state->lds_variable, "lds_dwords");
	}
	AddInputAnnotationsAndNames(state);
	AddOutputAnnotationsAndNames(state);
	AddDescriptorAnnotationsAndNames(state);
	AddVsharpAnnotationsAndNames(state);
	state->builder.AddAnnotation(
	    {OpDecorate, state->storage_runtime_array_type, DecorationArrayStride, 4});
	state->builder.AddAnnotation(
	    {OpMemberDecorate, state->storage_buffer_type, 0, DecorationOffset, 0});
	state->builder.AddAnnotation({OpDecorate, state->storage_buffer_type, DecorationBlock});

	state->builder.AddType({OpTypeVoid, state->void_type});
	state->builder.AddType({OpTypeBool, state->bool_type});
	state->builder.AddType({OpTypeInt, state->uint_type, 32, 0});
	state->builder.AddType(
	    {OpTypeStruct, state->uint_pair_type, state->uint_type, state->uint_type});
	state->builder.AddType({OpTypeInt, state->int_type, 32, 1});
	state->builder.AddType({OpTypeStruct, state->int_pair_type, state->int_type, state->int_type});
	state->builder.AddType({OpTypeFloat, state->float_type, 32});
	state->builder.AddType({OpTypeVector, state->vec2_uint_type, state->uint_type, 2});
	state->builder.AddType({OpTypeVector, state->vec3_uint_type, state->uint_type, 3});
	state->builder.AddType({OpTypeVector, state->vec4_uint_type, state->uint_type, 4});
	state->builder.AddType({OpTypeVector, state->vec2_int_type, state->int_type, 2});
	state->builder.AddType({OpTypeVector, state->vec3_int_type, state->int_type, 3});
	state->builder.AddType({OpTypeVector, state->vec4_int_type, state->int_type, 4});
	state->builder.AddType({OpTypeVector, state->vec2_float_type, state->float_type, 2});
	state->builder.AddType({OpTypeVector, state->vec3_float_type, state->float_type, 3});
	state->builder.AddType({OpTypeVector, state->vec4_float_type, state->float_type, 4});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_func_uint, StorageClassFunction, state->uint_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_float, StorageClassInput, state->float_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_bool, StorageClassInput, state->bool_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_int, StorageClassInput, state->int_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_uint, StorageClassInput, state->uint_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec2_float, StorageClassInput, state->vec2_float_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec3_float, StorageClassInput, state->vec3_float_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec2_int, StorageClassInput, state->vec2_int_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec3_int, StorageClassInput, state->vec3_int_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec4_int, StorageClassInput, state->vec4_int_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec2_uint, StorageClassInput, state->vec2_uint_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec3_uint, StorageClassInput, state->vec3_uint_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec4_uint, StorageClassInput, state->vec4_uint_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_input_vec4_float, StorageClassInput, state->vec4_float_type});
	if (state->subgroup_local_invocation_id_variable != 0) {
		state->builder.AddType({OpVariable, state->ptr_input_uint,
		                        state->subgroup_local_invocation_id_variable, StorageClassInput});
	}
	for (const auto& input: state->inputs) {
		uint32_t ptr_type = state->ptr_input_uint;
		switch (input.kind) {
			case IR::StageInputKind::VertexIndex:
			case IR::StageInputKind::InstanceIndex: ptr_type = state->ptr_input_int; break;
			case IR::StageInputKind::WorkgroupId:
			case IR::StageInputKind::LocalInvocationId:
			case IR::StageInputKind::GlobalInvocationId:
				ptr_type = state->ptr_input_vec3_uint;
				break;
			case IR::StageInputKind::FragCoord: ptr_type = state->ptr_input_vec4_float; break;
			case IR::StageInputKind::FrontFacing: ptr_type = state->ptr_input_bool; break;
			case IR::StageInputKind::Parameter:
				if (state->stage == ShaderType::Vertex) {
					const auto kind       = VertexParameterScalarKind(*state, input.location);
					const auto components = VertexParameterComponentCount(*state, input);
					ptr_type = VertexParameterInputPointerType(*state, kind, components);
				} else {
					ptr_type = state->ptr_input_vec4_float;
				}
				break;
			default: break;
		}
		state->builder.AddType({OpVariable, ptr_type, input.variable_id, StorageClassInput});
	}
	state->builder.AddType(
	    {OpTypePointer, state->ptr_output_float, StorageClassOutput, state->float_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_output_int, StorageClassOutput, state->int_type});
	state->builder.AddType(
	    {OpTypePointer, state->ptr_output_vec4_float, StorageClassOutput, state->vec4_float_type});
	if (state->per_vertex_variable != 0) {
		state->builder.AddType({OpTypeStruct, state->per_vertex_type, state->vec4_float_type});
		state->builder.AddType({OpTypePointer, state->ptr_output_per_vertex, StorageClassOutput,
		                        state->per_vertex_type});
		state->builder.AddType({OpVariable, state->ptr_output_per_vertex,
		                        state->per_vertex_variable, StorageClassOutput});
	}
	for (const auto& binding: state->outputs) {
		if (binding.kind == IR::StageOutputKind::Parameter ||
		    binding.kind == IR::StageOutputKind::Mrt) {
			state->builder.AddType({OpVariable, state->ptr_output_vec4_float, binding.variable_id,
			                        StorageClassOutput});
		}
	}
	if (state->depth_variable != 0) {
		state->builder.AddType(
		    {OpVariable, state->ptr_output_float, state->depth_variable, StorageClassOutput});
	}
	if (state->sample_mask_variable != 0) {
		state->builder.AddType(
		    {OpTypeArray, state->sample_mask_array_type, state->int_type, ConstantU32(state, 1)});
		state->builder.AddType({OpTypePointer, state->ptr_output_sample_mask_array,
		                        StorageClassOutput, state->sample_mask_array_type});
		state->builder.AddType({OpVariable, state->ptr_output_sample_mask_array,
		                        state->sample_mask_variable, StorageClassOutput});
	}
	state->builder.AddType(
	    {OpTypeRuntimeArray, state->storage_runtime_array_type, state->uint_type});
	state->builder.AddType(
	    {OpTypeStruct, state->storage_buffer_type, state->storage_runtime_array_type});
	state->builder.AddType({OpTypePointer, state->ptr_storage_buffer, StorageClassStorageBuffer,
	                        state->storage_buffer_type});
	state->builder.AddType({OpTypePointer, state->ptr_storage_buffer_uint,
	                        StorageClassStorageBuffer, state->uint_type});
	if (state->storage_buffer_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::Buffers));
		state->builder.AddType(
		    {OpTypeArray, state->storage_buffer_array_type, state->storage_buffer_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_storage_buffer_array,
		                        StorageClassStorageBuffer, state->storage_buffer_array_type});
		state->builder.AddType({OpVariable, state->ptr_storage_buffer_array,
		                        state->storage_buffer_variable, StorageClassStorageBuffer});
	}
	if (state->gds_variable != 0) {
		state->builder.AddType({OpVariable, state->ptr_storage_buffer, state->gds_variable,
		                        StorageClassStorageBuffer});
	}
	if (state->push_constant_variable != 0) {
		const auto row_width = ConstantU32(state, 4);
		const auto row_count = ConstantU32(
		    state, std::max((state->program->bindings.push_constant_size + 15u) / 16u, 1u));
		state->builder.AddType(
		    {OpTypeArray, state->push_constant_u32x4_array_type, state->uint_type, row_width});
		state->builder.AddType({OpTypeArray, state->push_constant_rows_array_type,
		                        state->push_constant_u32x4_array_type, row_count});
		state->builder.AddType(
		    {OpTypeStruct, state->push_constant_block_type, state->push_constant_rows_array_type});
		state->builder.AddType({OpTypePointer, state->ptr_push_constant_block,
		                        StorageClassPushConstant, state->push_constant_block_type});
		state->builder.AddType({OpTypePointer, state->ptr_push_constant_uint,
		                        StorageClassPushConstant, state->uint_type});
		state->builder.AddType({OpVariable, state->ptr_push_constant_block,
		                        state->push_constant_variable, StorageClassPushConstant});
	}
	if (state->vsharp_storage_variable != 0) {
		state->builder.AddType({OpVariable, state->ptr_storage_buffer,
		                        state->vsharp_storage_variable, StorageClassStorageBuffer});
	}
	if (state->address_memory_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::AddressMemory));
		state->builder.AddType(
		    {OpTypeArray, state->address_memory_array_type, state->storage_buffer_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_address_memory_array,
		                        StorageClassStorageBuffer, state->address_memory_array_type});
		state->builder.AddType({OpVariable, state->ptr_address_memory_array,
		                        state->address_memory_variable, StorageClassStorageBuffer});
	}
	if (state->flattened_srt_variable != 0) {
		state->builder.AddType({OpVariable, state->ptr_storage_buffer,
		                        state->flattened_srt_variable, StorageClassStorageBuffer});
	}
	if (state->stage == ShaderType::Compute || state->needs_function_lds) {
		const auto storage_class =
		    state->needs_function_lds ? StorageClassFunction : StorageClassWorkgroup;
		const auto lds_size = ConstantU32(state, state->needs_function_lds ? 8192u : 1024u);
		state->builder.AddType({OpTypeArray, state->lds_array_type, state->uint_type, lds_size});
		state->builder.AddType(
		    {OpTypePointer, state->ptr_workgroup_array, storage_class, state->lds_array_type});
		state->builder.AddType(
		    {OpTypePointer, state->ptr_workgroup_uint, storage_class, state->uint_type});
		if (state->stage == ShaderType::Compute) {
			state->builder.AddType({OpVariable, state->ptr_workgroup_array, state->lds_variable,
			                        StorageClassWorkgroup});
		}
	}
	for (uint32_t i = 0; i < state->sampled_images.size(); i++) {
		auto&      image     = state->sampled_images[i];
		const auto view      = static_cast<ImageViewKind>(i % 3u);
		const auto component = i >= 3u ? state->uint_type : state->float_type;
		const auto dimension = view == ImageViewKind::Dim3D ? Dim3D : Dim2D;
		const auto arrayed   = view == ImageViewKind::Dim2DArray ? 1u : 0u;
		state->builder.AddType({OpTypeImage, image.image_type, component, dimension, 0, arrayed, 0,
		                        1, ImageFormatUnknown});
		state->builder.AddType({OpTypeSampledImage, image.sampled_image_type, image.image_type});
		state->builder.AddType(
		    {OpTypePointer, image.pointer_type, StorageClassUniformConstant, image.image_type});
		if (image.variable != 0) {
			const auto kind  = SampledBindingKind(i >= 3u, view);
			const auto count = ConstantU32(state, DescriptorCount(*state, kind));
			state->builder.AddType({OpTypeArray, image.array_type, image.image_type, count});
			state->builder.AddType({OpTypePointer, image.array_pointer_type,
			                        StorageClassUniformConstant, image.array_type});
			state->builder.AddType({OpVariable, image.array_pointer_type, image.variable,
			                        StorageClassUniformConstant});
		}
	}
	state->builder.AddType({OpTypeSampler, state->sampler_type});
	state->builder.AddType({OpTypePointer, state->ptr_uniform_sampler, StorageClassUniformConstant,
	                        state->sampler_type});
	if (state->sampler_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::Samplers));
		state->builder.AddType(
		    {OpTypeArray, state->sampler_array_type, state->sampler_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_uniform_sampler_array,
		                        StorageClassUniformConstant, state->sampler_array_type});
		state->builder.AddType({OpVariable, state->ptr_uniform_sampler_array,
		                        state->sampler_variable, StorageClassUniformConstant});
	}
	state->builder.AddType({OpTypeImage, state->storage_image_type, state->float_type, Dim2D, 0, 0,
	                        0, 2, ImageFormatUnknown});
	state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image,
	                        StorageClassUniformConstant, state->storage_image_type});
	if (state->storage_image_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::Storage2D));
		state->builder.AddType(
		    {OpTypeArray, state->storage_image_array_type, state->storage_image_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_array,
		                        StorageClassUniformConstant, state->storage_image_array_type});
		state->builder.AddType({OpVariable, state->ptr_uniform_storage_image_array,
		                        state->storage_image_variable, StorageClassUniformConstant});
	}
	state->builder.AddType({OpTypeImage, state->storage_image_2d_array_type, state->float_type,
	                        Dim2D, 0, 1, 0, 2, ImageFormatUnknown});
	state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_2d_array,
	                        StorageClassUniformConstant, state->storage_image_2d_array_type});
	if (state->storage_image_2d_array_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::Storage2DArray));
		state->builder.AddType({OpTypeArray, state->storage_image_2d_array_array_type,
		                        state->storage_image_2d_array_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_2d_array_array,
		                        StorageClassUniformConstant,
		                        state->storage_image_2d_array_array_type});
		state->builder.AddType({OpVariable, state->ptr_uniform_storage_image_2d_array_array,
		                        state->storage_image_2d_array_variable,
		                        StorageClassUniformConstant});
	}
	state->builder.AddType({OpTypeImage, state->storage_image_3d_type, state->float_type, Dim3D, 0,
	                        0, 0, 2, ImageFormatUnknown});
	state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_3d,
	                        StorageClassUniformConstant, state->storage_image_3d_type});
	if (state->storage_image_3d_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::Storage3D));
		state->builder.AddType(
		    {OpTypeArray, state->storage_image_3d_array_type, state->storage_image_3d_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_3d_array,
		                        StorageClassUniformConstant, state->storage_image_3d_array_type});
		state->builder.AddType({OpVariable, state->ptr_uniform_storage_image_3d_array,
		                        state->storage_image_3d_variable, StorageClassUniformConstant});
	}
	state->builder.AddType({OpTypeImage, state->storage_image_uint_type, state->uint_type, Dim2D, 0,
	                        0, 0, 2, ImageFormatR32ui});
	state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_uint,
	                        StorageClassUniformConstant, state->storage_image_uint_type});
	if (state->storage_image_uint_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::StorageUint2D));
		state->builder.AddType({OpTypeArray, state->storage_image_uint_array_type,
		                        state->storage_image_uint_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_uint_array,
		                        StorageClassUniformConstant, state->storage_image_uint_array_type});
		state->builder.AddType({OpVariable, state->ptr_uniform_storage_image_uint_array,
		                        state->storage_image_uint_variable, StorageClassUniformConstant});
	}
	state->builder.AddType({OpTypeImage, state->storage_image_uint_2d_array_type, state->uint_type,
	                        Dim2D, 0, 1, 0, 2, ImageFormatR32ui});
	state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_uint_2d_array,
	                        StorageClassUniformConstant, state->storage_image_uint_2d_array_type});
	if (state->storage_image_uint_2d_array_variable != 0) {
		const auto count = ConstantU32(
		    state, DescriptorCount(*state, IR::DescriptorBindingKind::StorageUint2DArray));
		state->builder.AddType({OpTypeArray, state->storage_image_uint_2d_array_array_type,
		                        state->storage_image_uint_2d_array_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_uint_2d_array_array,
		                        StorageClassUniformConstant,
		                        state->storage_image_uint_2d_array_array_type});
		state->builder.AddType({OpVariable, state->ptr_uniform_storage_image_uint_2d_array_array,
		                        state->storage_image_uint_2d_array_variable,
		                        StorageClassUniformConstant});
	}
	state->builder.AddType({OpTypeImage, state->storage_image_uint_3d_type, state->uint_type, Dim3D,
	                        0, 0, 0, 2, ImageFormatR32ui});
	state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_uint_3d,
	                        StorageClassUniformConstant, state->storage_image_uint_3d_type});
	if (state->storage_image_uint_3d_variable != 0) {
		const auto count =
		    ConstantU32(state, DescriptorCount(*state, IR::DescriptorBindingKind::StorageUint3D));
		state->builder.AddType({OpTypeArray, state->storage_image_uint_3d_array_type,
		                        state->storage_image_uint_3d_type, count});
		state->builder.AddType({OpTypePointer, state->ptr_uniform_storage_image_uint_3d_array,
		                        StorageClassUniformConstant,
		                        state->storage_image_uint_3d_array_type});
		state->builder.AddType({OpVariable, state->ptr_uniform_storage_image_uint_3d_array,
		                        state->storage_image_uint_3d_variable,
		                        StorageClassUniformConstant});
	}
	state->builder.AddType(
	    {OpTypePointer, state->ptr_image_uint, StorageClassImage, state->uint_type});
	state->builder.AddType({OpTypeFunction, state->func_type, state->void_type});
}

void AllocateRegisterVariables(EmitterState* state) {
	if (state->stage == ShaderType::Pixel && state->needs_pixel_valid_mask) {
		state->pixel_valid_mask_variable = state->builder.AllocateId();
		state->builder.AddName(state->pixel_valid_mask_variable, "pixel_valid_mask_active");
	}
	for (auto& binding: state->registers) {
		binding.pointer_id = state->builder.AllocateId();
		const auto name    = IR::RegisterToString(binding.reg);
		state->builder.AddName(binding.pointer_id, name.c_str());
	}
}

void AllocateDescriptorVariables(EmitterState* state) {
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::Buffers) != nullptr) {
		state->storage_buffer_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::AddressMemory) != nullptr) {
		state->address_memory_variable = state->builder.AllocateId();
	}
	if (state->program->bindings.push_constant_size != 0) {
		state->push_constant_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::UserData) != nullptr) {
		state->vsharp_storage_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::FlattenedSrt) != nullptr) {
		state->flattened_srt_variable = state->builder.AllocateId();
	}
	for (uint32_t i = 0; i < state->sampled_images.size(); i++) {
		const auto view = static_cast<ImageViewKind>(i % 3u);
		if (DescriptorBinding(*state, SampledBindingKind(i >= 3u, view)) != nullptr) {
			state->sampled_images[i].variable = state->builder.AllocateId();
		}
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::Storage2D) != nullptr) {
		state->storage_image_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::StorageUint2D) != nullptr) {
		state->storage_image_uint_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::Storage2DArray) != nullptr) {
		state->storage_image_2d_array_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::StorageUint2DArray) != nullptr) {
		state->storage_image_uint_2d_array_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::Storage3D) != nullptr) {
		state->storage_image_3d_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::StorageUint3D) != nullptr) {
		state->storage_image_uint_3d_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::Samplers) != nullptr) {
		state->sampler_variable = state->builder.AllocateId();
	}
	if (DescriptorBinding(*state, IR::DescriptorBindingKind::Gds) != nullptr) {
		state->gds_variable = state->builder.AllocateId();
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
