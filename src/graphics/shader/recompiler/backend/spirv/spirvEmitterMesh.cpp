#include "graphics/shader/capturedVertexLayout.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, spv::StorageClass storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, spv::StorageClass storage,
                     uint32_t type, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, type), pointer,
	                          variable, index);
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, spv::StorageClass storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, value, pointer);
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::Layer: return TypeU32(state);
		case IR::StageOutputKind::ClipDistance:
		case IR::StageOutputKind::CullDistance: return TypeF32(state);
		case IR::StageOutputKind::Position:
		case IR::StageOutputKind::Parameter: return TypeF32Vector(state, 4);
		default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
}

// What the guest function leaves for the entry point: each invocation's outputs, the workgroup's
// vertex and primitive counts, and each invocation's packed primitive.
void DefineMeshCaptureData(EmitterState& state) {
	const auto& mesh = state.input_info.vertex->mesh;
	for (auto& output: state.outputs) {
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared         = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable = MeshArray(
		    state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate,
		    MeshOutputType(state, output.kind), shared ? mesh.max_vertices : state.lane_count);
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
}

} // namespace

void DefineMeshOutputs(EmitterState& state, uint32_t clip_distance_count,
                       uint32_t cull_distance_count) {
	const auto& mesh = state.input_info.vertex->mesh;
	if (MeshEmulated(state)) {
		// The outputs go to the record buffer, not to host mesh output arrays.
		DefineMeshCaptureData(state);
		return;
	}
	for (auto& output: state.outputs) {
		const auto type = MeshOutputType(state, output.kind);
		const bool clip = output.kind == IR::StageOutputKind::ClipDistance;
		const bool cull = output.kind == IR::StageOutputKind::CullDistance;
		auto& variable = clip ? state.clip_distance_variable
		                 : cull ? state.cull_distance_variable : output.variable_id;
		if (variable == 0) {
			const auto element_type = clip || cull
			                              ? state.builder.Type(spv::OpTypeArray, type,
			                                                   ConstantU32(state, clip ? clip_distance_count
			                                                                           : cull_distance_count))
			                              : type;
			variable = MeshArray(state, spv::StorageClassOutput, element_type,
			                     output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives
			                                                             : mesh.max_vertices);
			state.interface_variables.push_back(variable);
			state.builder.AddName(variable, output.debug_name.c_str());
			if (output.kind == IR::StageOutputKind::Parameter) {
				state.builder.AddAnnotation(spv::OpDecorate, variable,
				                            spv::DecorationLocation, output.location);
			} else {
				const auto builtin = clip ? spv::BuiltInClipDistance
				                     : cull ? spv::BuiltInCullDistance
				                     : output.kind == IR::StageOutputKind::Layer ? spv::BuiltInLayer
				                                                                 : spv::BuiltInPosition;
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
			}
			if (output.kind == IR::StageOutputKind::Layer) {
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPerPrimitiveEXT);
			}
		}
		output.variable_id = variable;
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? mesh.max_vertices : state.lane_count);
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
	state.mesh_primitives =
	    MeshArray(state, spv::StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.mesh_cull =
	    MeshArray(state, spv::StorageClassOutput, TypeBool(state), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.interface_variables.push_back(state.mesh_cull);
	state.builder.AddAnnotation(
	    spv::OpDecorate, state.mesh_primitives, spv::DecorationBuiltIn,
	    spv::BuiltInPrimitiveTriangleIndicesEXT); // PrimitiveTriangleIndicesEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationBuiltIn,
	                            spv::BuiltInCullPrimitiveEXT); // CullPrimitiveEXT
	state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationPerPrimitiveEXT);
}

uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index) {
	const auto output = std::ranges::find_if(state.outputs, [=](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
	if (output == state.outputs.end()) {
		EXIT("mesh export has no output binding: kind=%u index=%u\n", static_cast<uint32_t>(kind),
		     index);
	}
	const bool shared = kind == IR::StageOutputKind::Layer;
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

// `value`, at most `most`.
uint32_t MeshClampU32(EmitterState& state, uint32_t value, uint32_t most) {
	const auto clamped = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeU32(state), clamped, GlslStd450(state),
	                          GLSLstd450UMin, value, ConstantU32(state, most));
	return clamped;
}

// The guest's counts are clamped to the stage's maximums: the outputs and the capture records
// have room for those only.
void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto& mesh = state.input_info.vertex->mesh;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
	                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), value, allocation,
			                          ConstantU32(state, field * 12u),
			                          ConstantU32(state, field == 0 ? 10u : 11u));
			const auto pointer =
			    MeshElement(state, state.mesh_allocation, spv::StorageClassWorkgroup,
			                TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction(
			    spv::OpStore, pointer,
			    MeshClampU32(state, value, field == 0 ? mesh.max_vertices : mesh.max_primitives));
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	if (MeshEmulated(state)) {
		EmitMeshComputeEntryPoint(state);
		return;
	}
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
	                          primitives); // OpSetMeshOutputsEXT
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				uint32_t pointer;
				if (output.kind == IR::StageOutputKind::ClipDistance ||
				    output.kind == IR::StageOutputKind::CullDistance) {
					pointer = state.builder.AllocateId();
					state.builder.AddFunction(
					    spv::OpAccessChain, TypePointer(state, spv::StorageClassOutput, type), pointer,
					    output.variable_id, index, ConstantU32(state, output.index));
				} else {
					pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput, type, index);
				}
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), vertex[component], packed,
				    ConstantU32(state, component * 10u), ConstantU32(state, 9));
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                          vertex[0], vertex[1], vertex[2]);
			const auto triangle_pointer =
			    MeshElement(state, state.mesh_primitives, spv::StorageClassOutput,
			                TypeU32Vector(state, 3), index);
			state.builder.AddFunction(spv::OpStore, triangle_pointer, triangle);
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			state.builder.AddFunction(spv::OpStore,
			                          MeshElement(state, state.mesh_cull, spv::StorageClassOutput,
			                                      TypeBool(state), index),
			                          culled);
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto layer = MeshLoad(state, output.mesh_data_variable,
				                            spv::StorageClassWorkgroup, TypeU32(state),
				                            vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction(spv::OpStore, pointer, layer);
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

namespace {

uint32_t MeshPushDword(EmitterState& state, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(state), pointer,
	                          state.push_constant_variable, ConstantU32(state, 0),
	                          ConstantU32(state, index));
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), value, pointer);
	return value;
}

// Stores one 32-bit word of this workgroup's record. `word` is the word index within the record.
void StoreRecordWord(EmitterState& state, uint32_t record, uint32_t word, uint32_t value) {
	const auto offset  = Binary(state, spv::OpIMul, TypeU64(state),
	                            Unary(state, spv::OpUConvert, TypeU64(state), word),
	                            ConstantU64(state, sizeof(uint32_t)));
	const auto address = Binary(state, spv::OpIAdd, TypeU64(state), record, offset);
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpConvertUToPtr, TypePhysicalU32Pointer(state), pointer,
	                          address);
	constexpr uint32_t alignment = sizeof(uint32_t);
	state.builder.AddFunction(spv::OpStore, pointer, value, spv::MemoryAccessAlignedMask,
	                          alignment);
}

// The word of a value of vertex or primitive `index`: `first` is its word for index 0.
uint32_t WordIndex(EmitterState& state, uint32_t first, uint32_t index, uint32_t stride) {
	return EmitAddU32(state, ConstantU32(state, first),
	                  EmitBinaryU32(state, spv::OpIMul, index, ConstantU32(state, stride)));
}

} // namespace

// A mesh program on a host without mesh shaders: the guest code runs as usual, in a compute
// shader. Then each invocation stores its vertex and its primitive in this workgroup's record of
// the buffer the draw provides (capturedVertexLayout.h).
void EmitMeshComputeEntryPoint(EmitterState& state) {
	const auto layout = MeshCaptureLayout(state.program, state.input_info.vertex->mesh);
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));

	// One record per workgroup of every instance.
	const auto group    = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 0);
	const auto instance = EmitInputComponentU32(state, IR::StageInputKind::WorkgroupId, 1);
	const auto groups   = MeshPushDword(state, IR::PushData::MeshEmulationGroupsDword);
	const auto slot = EmitAddU32(state, group, EmitBinaryU32(state, spv::OpIMul, instance, groups));
	const auto buffer =
	    PackU64(state, MeshPushDword(state, IR::PushData::MeshEmulationAddressDword),
	                           MeshPushDword(state, IR::PushData::MeshEmulationAddressDword + 1));
	const auto record = Binary(state, spv::OpIAdd, TypeU64(state), buffer,
	                           Binary(state, spv::OpIMul, TypeU64(state),
	                                  Unary(state, spv::OpUConvert, TypeU64(state), slot),
	                                  ConstantU64(state, layout.RecordBytes())));

	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half  = half;
		const auto index = EmitLocalInvocationIndex(state);
		if (half == 0) {
			const auto first = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first, index,
			                          ConstantU32(state, 0));
			EmitIfCondition(state, first, [&] {
				StoreRecordWord(state, record, ConstantU32(state, 0), vertices);
				StoreRecordWord(state, record, ConstantU32(state, 1), primitives);
			});
		}
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			uint32_t parameter = 0;
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				const auto store = [&](uint32_t first_word, uint32_t scalar) {
					StoreRecordWord(state, record,
					                WordIndex(state, first_word, index, layout.VertexWords()),
					                Unary(state, spv::OpBitcast, TypeU32(state), scalar));
				};
				if (output.kind == IR::StageOutputKind::ClipDistance) {
					store(layout.ClipDistanceWord(0, output.index), value);
					continue;
				}
				if (output.kind == IR::StageOutputKind::CullDistance) {
					store(layout.CullDistanceWord(0, output.index), value);
					continue;
				}
				const auto first_word = output.kind == IR::StageOutputKind::Position
				                            ? layout.PositionWord(0)
				                            : layout.ParameterWord(0, parameter++);
				for (uint32_t component = 0; component < 4; component++) {
					const auto scalar = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), scalar,
					                          value, component);
					store(first_word + component, scalar);
				}
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			const auto word   = WordIndex(state, layout.PrimitiveWord(0), index,
			                              CapturedVertexLayout::kPrimitiveWords);
			StoreRecordWord(state, record, word, packed);
			// The layer of a primitive is the one at its provoking vertex.
			uint32_t layer = ConstantU32(state, 0);
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto provoking = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), provoking, packed,
				    ConstantU32(state, state.input_info.vertex->mesh.provoking_vertex *
				                           CapturedVertexLayout::kIndexBits),
				    ConstantU32(state, CapturedVertexLayout::kIndexBits));
				// The shared array has room for the stage's maximum vertices.
				layer = MeshLoad(
				    state, output.mesh_data_variable, spv::StorageClassWorkgroup, TypeU32(state),
				    MeshClampU32(state, provoking,
				                 state.input_info.vertex->mesh.max_vertices - 1u));
			}
			StoreRecordWord(state, record, EmitAddU32(state, word, ConstantU32(state, 1)), layer);
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
