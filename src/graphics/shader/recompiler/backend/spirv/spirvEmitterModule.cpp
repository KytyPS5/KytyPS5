#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {

uint32_t TypeVoid(EmitterState& state) {
	return state.builder.Type(spv::OpTypeVoid);
}

uint32_t TypeBool(EmitterState& state) {
	return state.builder.Type(spv::OpTypeBool);
}

uint32_t TypeBoolVector(EmitterState& state, uint32_t components) {
	return state.builder.Type(spv::OpTypeVector, TypeBool(state), components);
}

uint32_t TypeU32(EmitterState& state) {
	return state.builder.Type(spv::OpTypeInt, 32, 0);
}

uint32_t TypeU64(EmitterState& state) {
	return TypeU32Vector(state, 2);
}

uint32_t TypeScalarU64(EmitterState& state) {
	return state.builder.Type(spv::OpTypeInt, 64, 0);
}

uint32_t TypeU32Pair(EmitterState& state) {
	const auto element = TypeU32(state);
	return state.builder.Type(spv::OpTypeStruct, element, element);
}

uint32_t TypeI32(EmitterState& state) {
	return state.builder.Type(spv::OpTypeInt, 32, 1);
}

uint32_t TypeI32Pair(EmitterState& state) {
	const auto element = TypeI32(state);
	return state.builder.Type(spv::OpTypeStruct, element, element);
}

uint32_t TypeF32(EmitterState& state) {
	return state.builder.Type(spv::OpTypeFloat, 32);
}

uint32_t TypeU32Vector(EmitterState& state, uint32_t components) {
	return state.builder.Type(spv::OpTypeVector, TypeU32(state), components);
}

uint32_t TypeU32Composite(EmitterState& state, uint32_t components) {
	EXIT_IF(components < 2u || components > 4u);
	return components == 2u ? TypeU32Pair(state) : TypeU32Vector(state, components);
}

uint32_t TypeI32Vector(EmitterState& state, uint32_t components) {
	return state.builder.Type(spv::OpTypeVector, TypeI32(state), components);
}

uint32_t TypeF32Vector(EmitterState& state, uint32_t components) {
	return state.builder.Type(spv::OpTypeVector, TypeF32(state), components);
}

uint32_t TypePointer(EmitterState& state, spv::StorageClass storage_class, uint32_t pointee) {
	return state.builder.Type(spv::OpTypePointer, storage_class, pointee);
}

uint32_t TypeFunction(EmitterState& state) {
	return state.builder.Type(spv::OpTypeFunction, TypeVoid(state));
}

uint32_t StorageRuntimeArrayType(EmitterState& state) {
	return state.builder.DecoratedType(
	    spv::OpTypeRuntimeArray,
	    {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint32_t)}}}, TypeU32(state));
}

uint32_t StorageBufferType(EmitterState& state) {
	return state.builder.DecoratedType(spv::OpTypeStruct,
	                                   {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                    {spv::OpDecorate, {spv::DecorationBlock}}},
	                                   StorageRuntimeArrayType(state));
}

uint32_t TypeStorageBufferPointer(EmitterState& state) {
	return TypePointer(state, spv::StorageClassStorageBuffer, StorageBufferType(state));
}

uint32_t TypeStorageBufferElementPointer(EmitterState& state) {
	return TypePointer(state, spv::StorageClassStorageBuffer, TypeU32(state));
}

uint32_t StorageU64RuntimeArrayType(EmitterState& state) {
	return state.builder.DecoratedType(
	    spv::OpTypeRuntimeArray,
	    {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint64_t)}}}, TypeScalarU64(state));
}

uint32_t StorageBufferU64Type(EmitterState& state) {
	return state.builder.DecoratedType(spv::OpTypeStruct,
	                                   {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                    {spv::OpDecorate, {spv::DecorationBlock}}},
	                                   StorageU64RuntimeArrayType(state));
}

uint32_t TypeStorageBufferU64Pointer(EmitterState& state) {
	return TypePointer(state, spv::StorageClassStorageBuffer, StorageBufferU64Type(state));
}

uint32_t TypeStorageBufferU64ElementPointer(EmitterState& state) {
	return TypePointer(state, spv::StorageClassStorageBuffer, TypeScalarU64(state));
}

uint32_t TypePhysicalU32Pointer(EmitterState& state) {
	return TypePointer(state, spv::StorageClassPhysicalStorageBuffer, TypeU32(state));
}

uint32_t TypePushConstantElementPointer(EmitterState& state) {
	return TypePointer(state, spv::StorageClassPushConstant, TypeU32(state));
}

uint32_t TypeU32ArrayPointer(EmitterState& state, spv::StorageClass storage_class,
                             uint32_t dwords) {
	const auto count = ConstantU32(state, std::max(dwords, 1u));
	const auto array = state.builder.Type(spv::OpTypeArray, TypeU32(state), count);
	return TypePointer(state, storage_class, array);
}

uint32_t TypeU32ElementPointer(EmitterState& state, spv::StorageClass storage_class) {
	return TypePointer(state, storage_class, TypeU32(state));
}

namespace {

uint32_t PushConstantArrayType(EmitterState& state) {
	const auto count = ConstantU32(state, IR::PushData::DwordCount);
	return state.builder.DecoratedType(
	    spv::OpTypeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint32_t)}}},
	    TypeU32(state), count);
}

uint32_t PushConstantBlockType(EmitterState& state) {
	return state.builder.DecoratedType(spv::OpTypeStruct,
	                                   {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                    {spv::OpDecorate, {spv::DecorationBlock}}},
	                                   PushConstantArrayType(state));
}

uint32_t PerVertexType(EmitterState& state) {
	return state.builder.DecoratedType(
	    spv::OpTypeStruct,
	    {{spv::OpMemberDecorate, {0, spv::DecorationBuiltIn, spv::BuiltInPosition}},
	     {spv::OpDecorate, {spv::DecorationBlock}}},
	    TypeF32Vector(state, 4));
}

uint32_t SampleMaskArrayType(EmitterState& state) {
	return state.builder.Type(spv::OpTypeArray, TypeI32(state), ConstantU32(state, 1));
}

uint32_t F32ArrayType(EmitterState& state, uint32_t count) {
	return state.builder.Type(spv::OpTypeArray, TypeF32(state), ConstantU32(state, count));
}

void DefineDescriptors(EmitterState& state) {
	if (state.program.bindings.UsesPushData() || state.program.stage == ShaderType::Mesh) {
		const auto type              = PushConstantBlockType(state);
		state.push_constant_variable = state.builder.DefineGlobalVariable(
		    TypePointer(state, spv::StorageClassPushConstant, type), spv::StorageClassPushConstant);
		state.builder.AddName(type, "BufferResource");
		state.builder.AddName(state.push_constant_variable, "vsharp");
	}
	for (const auto& binding: state.program.bindings.descriptors) {
		const auto Define = [&](uint32_t type, const char* name,
		                        spv::StorageClass storage = spv::StorageClassStorageBuffer) {
			const auto variable =
			    state.builder.DefineGlobalVariable(TypePointer(state, storage, type), storage);
			state.builder.AddName(variable, name);
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationDescriptorSet, 0);
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBinding,
			                            IR::NativeBinding(state.program.stage, binding.kind));
			return variable;
		};
		const auto ArrayType = [&](uint32_t type) {
			return state.builder.Type(
			    spv::OpTypeArray, type,
			    ConstantU32(state, static_cast<uint32_t>(binding.resources.size())));
		};
		switch (binding.kind) {
			case IR::DescriptorBindingKind::Buffers:
				state.storage_buffer_variable =
				    Define(ArrayType(StorageBufferType(state)), "buffers");
				if (state.requirements.buffer_int64_atomics) {
					state.storage_buffer_u64_variable =
					    Define(ArrayType(StorageBufferU64Type(state)), "buffers_u64");
					state.builder.AddAnnotation(spv::OpDecorate, state.storage_buffer_variable,
					                            spv::DecorationAliased);
					state.builder.AddAnnotation(spv::OpDecorate, state.storage_buffer_u64_variable,
					                            spv::DecorationAliased);
				}
				break;
			case IR::DescriptorBindingKind::BdaPagetable:
				state.bda_pagetable_variable = Define(StorageBufferU64Type(state), "bda_pagetable");
				break;
			case IR::DescriptorBindingKind::FaultBuffer:
				state.fault_buffer_variable = Define(StorageBufferType(state), "fault_buffer");
				break;
			case IR::DescriptorBindingKind::ShaderData:
				state.shader_data_storage_variable =
				    Define(StorageBufferType(state), "shader_data");
				break;
			case IR::DescriptorBindingKind::FlattenedSrt:
				state.flattened_srt_variable = Define(StorageBufferType(state), "flattened_srt");
				break;
			case IR::DescriptorBindingKind::Samplers:
				state.sampler_variable = Define(ArrayType(state.builder.Type(spv::OpTypeSampler)),
				                                "samplers", spv::StorageClassUniformConstant);
				break;
			case IR::DescriptorBindingKind::Gds:
				state.gds_variable = Define(StorageBufferType(state), "gds");
				break;
			default: {
				EXIT_IF(IR::ImageBindingResourceClass(binding.kind) ==
				        IR::ImageResourceClass::None);
				const auto& image = state.program.info.images.at(binding.resources.front());
				const auto  name  = "image_" + std::to_string(static_cast<uint32_t>(binding.kind));
				state.image_variables[IR::ImageBindingIndex(binding.kind)] =
				    Define(ArrayType(ImageType(state, image)), name.c_str(),
				           spv::StorageClassUniformConstant);
				if (image.dimension == ImageDimension::Dim1D ||
				    image.dimension == ImageDimension::Dim1DArray) {
					state.builder.RequireCapability(image.resource_class ==
					                                        IR::ImageResourceClass::Sampled
					                                    ? spv::CapabilitySampled1D
					                                    : spv::CapabilityImage1D);
				}
				break;
			}
		}
	}
}

} // namespace

uint32_t ConstantU32(EmitterState& state, uint32_t value) {
	return state.builder.Constant(spv::OpConstant, TypeU32(state), value);
}

uint32_t ConstantI32(EmitterState& state, int32_t value) {
	return state.builder.Constant(spv::OpConstant, TypeI32(state), static_cast<uint32_t>(value));
}

uint32_t ConstantF32(EmitterState& state, uint32_t bits) {
	return state.builder.Constant(spv::OpConstant, TypeF32(state), bits);
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
	return state.builder.Constant(value ? spv::OpConstantTrue : spv::OpConstantFalse,
	                              TypeBool(state));
}

uint32_t ConstantU64(EmitterState& state, uint64_t value) {
	return state.builder.Constant(spv::OpConstantComposite, TypeU64(state),
	                              ConstantU32(state, static_cast<uint32_t>(value)),
	                              ConstantU32(state, static_cast<uint32_t>(value >> 32u)));
}

uint32_t ConstantU32CompositeZero(EmitterState& state, uint32_t components) {
	EXIT_IF(components < 2u || components > 4u);
	const auto            zero = ConstantU32(state, 0);
	std::vector<uint32_t> values(components, zero);
	return state.builder.Constant(spv::OpConstantComposite, TypeU32Composite(state, components),
	                              values);
}

uint32_t GlslStd450(EmitterState& state) {
	return state.builder.Import("GLSL.std.450");
}

VertexInputScalarKind VertexParameterScalarKind(const EmitterState& state, uint32_t location) {
	if (state.program.stage != ShaderType::Vertex || location >= ShaderVertexInputInfo::RES_MAX ||
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

uint32_t VertexParameterComponentCount(const InputBinding& input) {
	return std::clamp(input.component_count, 1u, 4u);
}

uint32_t VertexParameterScalarType(EmitterState& state, VertexInputScalarKind kind) {
	switch (kind) {
		case VertexInputScalarKind::Sint: return TypeI32(state);
		case VertexInputScalarKind::Uint: return TypeU32(state);
		case VertexInputScalarKind::Float:
		default: return TypeF32(state);
	}
}

namespace {

uint32_t DefineInterfaceVariable(EmitterState& state, uint32_t type, spv::StorageClass storage,
                                 const char* name) {
	const auto variable =
	    state.builder.DefineGlobalVariable(TypePointer(state, storage, type), storage);
	state.interface_variables.push_back(variable);
	state.builder.AddName(variable, name);
	return variable;
}

uint32_t BuiltInForInput(IR::StageInputKind kind) {
	switch (kind) {
		case IR::StageInputKind::VertexIndex: return spv::BuiltInVertexIndex;
		case IR::StageInputKind::InstanceIndex: return spv::BuiltInInstanceIndex;
		case IR::StageInputKind::FragCoord: return spv::BuiltInFragCoord;
		case IR::StageInputKind::FrontFacing: return spv::BuiltInFrontFacing;
		case IR::StageInputKind::Layer: return spv::BuiltInLayer;
		case IR::StageInputKind::SampleId: return spv::BuiltInSampleId;
		case IR::StageInputKind::BaryCoordSmooth: return spv::BuiltInBaryCoordKHR;
		case IR::StageInputKind::BaryCoordNoPerspective: return spv::BuiltInBaryCoordNoPerspKHR;
		case IR::StageInputKind::WorkgroupId: return spv::BuiltInWorkgroupId;
		case IR::StageInputKind::LocalInvocationId: return spv::BuiltInLocalInvocationId;
		case IR::StageInputKind::LocalInvocationIndex: return spv::BuiltInLocalInvocationIndex;
		case IR::StageInputKind::GlobalInvocationId: return spv::BuiltInGlobalInvocationId;
		default: return UINT32_MAX;
	}
}

void DefineInputs(EmitterState& state) {
	state.inputs.reserve(state.program.info.inputs.size());
	for (const auto& input: state.program.info.inputs) {
		state.inputs.push_back({input});
	}
	if (state.lane_count == 2) {
		const auto add_builtin = [&](IR::StageInputKind kind, uint32_t components,
		                             const char* name) {
			if (std::ranges::none_of(state.inputs, [kind](const InputBinding& input) {
				    return input.kind == kind;
			    })) {
				state.inputs.push_back({{kind, 0, components, name}});
			}
		};
		add_builtin(IR::StageInputKind::LocalInvocationIndex, 1, "gl_LocalInvocationIndex");
		if (std::ranges::any_of(state.inputs, [](const InputBinding& input) {
			    return input.kind == IR::StageInputKind::GlobalInvocationId;
		    })) {
			add_builtin(IR::StageInputKind::WorkgroupId, 3, "gl_WorkGroupID");
		}
	}
	for (auto& input: state.inputs) {
		uint32_t type = TypeU32(state);
		switch (input.kind) {
			case IR::StageInputKind::VertexIndex:
			case IR::StageInputKind::InstanceIndex:
			case IR::StageInputKind::Layer:
			case IR::StageInputKind::SampleId: type = TypeI32(state); break;
			case IR::StageInputKind::WorkgroupId:
			case IR::StageInputKind::LocalInvocationId:
			case IR::StageInputKind::GlobalInvocationId: type = TypeU32Vector(state, 3); break;
			case IR::StageInputKind::FragCoord: type = TypeF32Vector(state, 4); break;
			case IR::StageInputKind::BaryCoordSmooth:
			case IR::StageInputKind::BaryCoordNoPerspective: type = TypeF32Vector(state, 3); break;
			case IR::StageInputKind::FrontFacing: type = TypeBool(state); break;
			case IR::StageInputKind::Parameter:
				if (state.program.stage == ShaderType::Vertex) {
					type = VertexParameterScalarType(
					    state, VertexParameterScalarKind(state, input.location));
					const auto components = VertexParameterComponentCount(input);
					if (components > 1u) {
						type = state.builder.Type(spv::OpTypeVector, type, components);
					}
				} else if (input.per_vertex) {
					type = state.builder.Type(spv::OpTypeArray, TypeF32Vector(state, 4),
					                          ConstantU32(state, 3));
				} else {
					type = TypeF32Vector(state, 4);
				}
				break;
			default: break;
		}
		input.variable_id =
		    DefineInterfaceVariable(state, type, spv::StorageClassInput, input.debug_name.c_str());
		if (input.kind == IR::StageInputKind::Layer || input.kind == IR::StageInputKind::SampleId) {
			state.builder.AddAnnotation(spv::OpDecorate, input.variable_id, spv::DecorationFlat);
		}
		if (input.kind == IR::StageInputKind::Parameter) {
			const auto flat = PixelParameterIsFlat(state, input.location);
			if (input.per_vertex) {
				state.builder.AddAnnotation(spv::OpDecorate, input.variable_id,
				                            spv::DecorationPerVertexKHR);
			} else if (flat) {
				state.builder.AddAnnotation(spv::OpDecorate, input.variable_id,
				                            spv::DecorationFlat);
			}
			if (state.program.stage == ShaderType::Pixel &&
			    state.input_info.pixel->ps_no_perspective && !flat && !input.per_vertex) {
				state.builder.AddAnnotation(spv::OpDecorate, input.variable_id,
				                            spv::DecorationNoPerspective);
			}
			state.builder.AddAnnotation(spv::OpDecorate, input.variable_id, spv::DecorationLocation,
			                            PixelParameterLocation(state, input.location));
		} else if (const auto builtin = BuiltInForInput(input.kind); builtin != UINT32_MAX) {
			state.builder.AddAnnotation(spv::OpDecorate, input.variable_id, spv::DecorationBuiltIn,
			                            builtin);
		}
	}
	if (state.requirements.subgroup_local_invocation_id) {
		const auto variable = DefineInterfaceVariable(state, TypeU32(state), spv::StorageClassInput,
		                                              "gl_SubgroupInvocationID");
		state.subgroup_local_invocation_id_variable = variable;
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn,
		                            spv::BuiltInSubgroupLocalInvocationId);
		if (state.program.stage == ShaderType::Pixel) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationFlat);
		}
	}
}

void DefineOutputs(EmitterState& state) {
	state.outputs.reserve(state.program.info.outputs.size());
	uint32_t clip_distance_count = 0;
	uint32_t cull_distance_count = 0;
	for (const auto& output: state.program.info.outputs) {
		state.outputs.push_back({output});
		if (output.kind == IR::StageOutputKind::ClipDistance) {
			clip_distance_count = std::max(clip_distance_count, output.index + 1);
		} else if (output.kind == IR::StageOutputKind::CullDistance) {
			cull_distance_count = std::max(cull_distance_count, output.index + 1);
		}
	}
	if (state.program.stage == ShaderType::Mesh) {
		DefineMeshOutputs(state);
		return;
	}
	const auto BuiltIn = [&](uint32_t& variable, uint32_t type, const char* name,
	                         spv::BuiltIn builtin) {
		if (variable == 0) {
			variable = DefineInterfaceVariable(state, type, spv::StorageClassOutput, name);
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
		}
		return variable;
	};
	for (auto& binding: state.outputs) {
		switch (binding.kind) {
			case IR::StageOutputKind::Position:
				if (state.per_vertex_variable == 0) {
					const auto type = PerVertexType(state);
					state.builder.AddName(type, "gl_PerVertex");
					state.per_vertex_variable = DefineInterfaceVariable(
					    state, type, spv::StorageClassOutput, "outPerVertex");
				}
				binding.variable_id = state.per_vertex_variable;
				break;
			case IR::StageOutputKind::PointSize:
				binding.variable_id = BuiltIn(state.point_size_variable, TypeF32(state),
				                              "gl_PointSize", spv::BuiltInPointSize);
				break;
			case IR::StageOutputKind::ClipDistance:
				binding.variable_id =
				    BuiltIn(state.clip_distance_variable, F32ArrayType(state, clip_distance_count),
				            "gl_ClipDistance", spv::BuiltInClipDistance);
				break;
			case IR::StageOutputKind::CullDistance:
				binding.variable_id =
				    BuiltIn(state.cull_distance_variable, F32ArrayType(state, cull_distance_count),
				            "gl_CullDistance", spv::BuiltInCullDistance);
				break;
			case IR::StageOutputKind::Layer:
				binding.variable_id =
				    BuiltIn(state.layer_variable, TypeU32(state), "gl_Layer", spv::BuiltInLayer);
				break;
			case IR::StageOutputKind::ViewportIndex:
				binding.variable_id = BuiltIn(state.viewport_index_variable, TypeU32(state),
				                              "gl_ViewportIndex", spv::BuiltInViewportIndex);
				break;
			case IR::StageOutputKind::Depth:
				binding.variable_id = BuiltIn(state.depth_variable, TypeF32(state), "gl_FragDepth",
				                              spv::BuiltInFragDepth);
				break;
			case IR::StageOutputKind::SampleMask:
				binding.variable_id =
				    BuiltIn(state.sample_mask_variable, SampleMaskArrayType(state), "gl_SampleMask",
				            spv::BuiltInSampleMask);
				break;
			case IR::StageOutputKind::Parameter:
			case IR::StageOutputKind::Mrt: {
				const bool uint_output =
				    binding.kind == IR::StageOutputKind::Mrt &&
				    state.program.stage == ShaderType::Pixel &&
				    binding.index < std::size(state.input_info.pixel->target_output_mode) &&
				    state.input_info.pixel->target_output_mode[binding.index] == 7u;
				const auto type = uint_output ? TypeU32Vector(state, 4) : TypeF32Vector(state, 4);
				binding.variable_id = DefineInterfaceVariable(state, type, spv::StorageClassOutput,
				                                              binding.debug_name.c_str());
				state.builder.AddAnnotation(spv::OpDecorate, binding.variable_id,
				                            spv::DecorationLocation, binding.location);
				break;
			}
		}
	}
}

} // namespace

void DefineModule(EmitterState& state) {
	state.interface_variables.reserve(state.program.info.inputs.size() +
	                                  state.program.info.outputs.size());
	DefineInputs(state);
	DefineOutputs(state);
	DefineDescriptors(state);
	if (state.requirements.function_lds) {
		state.lds_variable = state.builder.AllocateId();
	}
	if (state.requirements.function_scratch) {
		for (uint32_t half = 0; half < state.lane_count; half++) {
			state.scratch_variable[half] = state.builder.AllocateId();
		}
	}
	state.main_func = state.builder.AllocateId();
	if (state.program.stage == ShaderType::Mesh) {
		state.mesh_guest_func = state.builder.AllocateId();
		state.builder.RequireCapability(spv::CapabilityMeshShadingEXT); // MeshShadingEXT
		state.builder.RequireExtension("SPV_EXT_mesh_shader");
		state.builder.AddExecutionMode(state.main_func,
		                               spv::ExecutionModeOutputTrianglesEXT); // OutputTrianglesEXT
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeOutputVertices,
		                               state.input_info.vertex->mesh.max_vertices);
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeOutputPrimitivesEXT,
		                               state.input_info.vertex->mesh.max_primitives);
	}
	state.entry_label = state.builder.AllocateId();

	state.builder.RequireCapability(spv::CapabilityShader);
	state.builder.RequireCapability(spv::CapabilitySignedZeroInfNanPreserve);
	if (state.program.info.uses_dma) {
		state.builder.RequireCapability(spv::CapabilityInt64);
		state.builder.RequireCapability(spv::CapabilityPhysicalStorageBufferAddresses);
		state.builder.RequireExtension("SPV_KHR_physical_storage_buffer");
	}
	if (state.requirements.buffer_int64_atomics) {
		state.builder.RequireCapability(spv::CapabilityInt64);
		state.builder.RequireCapability(spv::CapabilityInt64Atomics);
	}
	if (state.clip_distance_variable != 0) {
		state.builder.RequireCapability(spv::CapabilityClipDistance);
	}
	if (state.cull_distance_variable != 0) {
		state.builder.RequireCapability(spv::CapabilityCullDistance);
	}
	if (state.layer_variable != 0 || InputVariableForKind(state, IR::StageInputKind::Layer) != 0) {
		state.builder.RequireVersion(0x00010500u);
		state.builder.RequireCapability(spv::CapabilityShaderLayer);
	}
	if (state.viewport_index_variable != 0) {
		state.builder.RequireVersion(0x00010500u);
		state.builder.RequireCapability(spv::CapabilityShaderViewportIndex);
	}
	if (InputVariableForKind(state, IR::StageInputKind::SampleId) != 0) {
		state.builder.RequireCapability(spv::CapabilitySampleRateShading);
	}
	if (state.requirements.image_gather_extended) {
		state.builder.RequireCapability(spv::CapabilityImageGatherExtended);
	}
	if (state.lane_count == 2 || state.requirements.subgroup_ballot ||
	    state.requirements.subgroup_shuffle || state.requirements.subgroup_local_invocation_id) {
		state.builder.RequireCapability(spv::CapabilityGroupNonUniform);
	}
	if (state.lane_count == 2 || state.requirements.subgroup_ballot) {
		state.builder.RequireCapability(spv::CapabilityGroupNonUniformBallot);
	}
	if (state.requirements.subgroup_shuffle) {
		state.builder.RequireCapability(spv::CapabilityGroupNonUniformShuffle);
	}
	if (state.requirements.compute_derivatives && state.program.stage == ShaderType::Compute) {
		state.builder.RequireCapability(spv::CapabilityComputeDerivativeGroupQuadsKHR);
		state.builder.RequireExtension("SPV_KHR_compute_shader_derivatives");
	}
	const bool fragment_barycentric =
	    state.program.stage == ShaderType::Pixel &&
	    std::any_of(state.inputs.begin(), state.inputs.end(), [](const InputBinding& input) {
		    return input.per_vertex || input.kind == IR::StageInputKind::BaryCoordSmooth ||
		           input.kind == IR::StageInputKind::BaryCoordNoPerspective;
	    });
	if (fragment_barycentric) {
		state.builder.RequireCapability(spv::CapabilityFragmentBarycentricKHR);
		state.builder.RequireExtension("SPV_KHR_fragment_shader_barycentric");
	}
	state.builder.RequireExtension("SPV_KHR_float_controls");
	state.builder.AddMemoryModel(state.program.info.uses_dma
	                                 ? spv::AddressingModelPhysicalStorageBuffer64
	                                 : spv::AddressingModelLogical,
	                             spv::MemoryModelGLSL450);
	// GCN/RDNA arithmetic preserves 32-bit signed zero, infinity, and NaN. Declaring that
	// contract prevents host compilers from treating synthesized IEEE values as finite.
	state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeSignedZeroInfNanPreserve,
	                               32u);
	if (const auto* cs = ShaderWorkgroupInput(state.program.stage, state.input_info)) {
		uint32_t    local_x = state.requirements.compute_derivatives ? 2u : 1u;
		uint32_t    local_y = state.requirements.compute_derivatives ? 2u : 1u;
		uint32_t    local_z = 1u;
		local_x             = cs->threads_num[0] != 0u ? cs->threads_num[0] : local_x;
		local_y             = cs->threads_num[1] != 0u ? cs->threads_num[1] : local_y;
		local_z             = cs->threads_num[2] != 0u ? cs->threads_num[2] : local_z;
		if (state.lane_count == 2) {
			local_x = ((local_x * local_y * local_z + 63u) / 64u) * 32u;
			local_y = local_z = 1u;
			if (state.requirements.compute_derivatives) {
				local_y = local_x / 2u;
				local_x = 2u;
			}
		}
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeLocalSize, local_x,
		                               local_y, local_z);
	}
	if (state.program.stage == ShaderType::Pixel) {
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeOriginUpperLeft);
		if (state.depth_variable != 0) {
			state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeDepthReplacing);
		}
		if (state.input_info.pixel->ps_early_z && !state.input_info.pixel->ps_pixel_kill_enable &&
		    !state.input_info.pixel->ps_depth_export_enable &&
		    !state.input_info.pixel->ps_sample_mask_export_enable) {
			state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeEarlyFragmentTests);
		}
	}
	if (state.requirements.compute_derivatives && state.program.stage == ShaderType::Compute) {
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeDerivativeGroupQuadsKHR);
	}
	state.builder.AddName(state.main_func, "main");
	if (state.requirements.function_lds) {
		state.builder.AddName(state.lds_variable, "lds_dwords");
	}
	if (state.requirements.function_scratch) {
		for (uint32_t half = 0; half < state.lane_count; half++) {
			state.builder.AddName(state.scratch_variable[half], "scratch_dwords");
		}
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
