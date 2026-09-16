#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

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

bool MeshPerPrimitive(IR::StageOutputKind kind) {
	return kind == IR::StageOutputKind::Layer || kind == IR::StageOutputKind::ViewportIndex;
}

bool MeshDistance(IR::StageOutputKind kind) {
	return kind == IR::StageOutputKind::ClipDistance || kind == IR::StageOutputKind::CullDistance;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	if (MeshPerPrimitive(kind)) {
		return TypeU32(state);
	}
	if (MeshDistance(kind) || kind == IR::StageOutputKind::PointSize) {
		return TypeF32(state);
	}
	return TypeF32Vector(state, 4);
}

spv::BuiltIn MeshOutputBuiltIn(IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::Position: return spv::BuiltInPosition;
		case IR::StageOutputKind::PointSize: return spv::BuiltInPointSize;
		case IR::StageOutputKind::ClipDistance: return spv::BuiltInClipDistance;
		case IR::StageOutputKind::CullDistance: return spv::BuiltInCullDistance;
		case IR::StageOutputKind::Layer: return spv::BuiltInLayer;
		case IR::StageOutputKind::ViewportIndex: return spv::BuiltInViewportIndex;
		default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
	return spv::BuiltInMax;
}

uint32_t* MeshBuiltInVariable(EmitterState& state, IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::PointSize: return &state.point_size_variable;
		case IR::StageOutputKind::ClipDistance: return &state.clip_distance_variable;
		case IR::StageOutputKind::CullDistance: return &state.cull_distance_variable;
		case IR::StageOutputKind::ViewportIndex: return &state.viewport_index_variable;
		default: return nullptr;
	}
}

} // namespace

void DefineMeshOutputs(EmitterState& state) {
	const auto& mesh = state.input_info.vertex->mesh;
	uint32_t    clip_count = 0;
	uint32_t    cull_count = 0;
	for (const auto& output: state.outputs) {
		if (output.kind == IR::StageOutputKind::ClipDistance) {
			clip_count = std::max(clip_count, output.index + 1);
		} else if (output.kind == IR::StageOutputKind::CullDistance) {
			cull_count = std::max(cull_count, output.index + 1);
		}
	}
	for (auto& output: state.outputs) {
		const auto builtin = output.kind == IR::StageOutputKind::Parameter
		                         ? spv::BuiltInMax
		                         : MeshOutputBuiltIn(output.kind);
		const auto type    = MeshOutputType(state, output.kind);
		auto*      shared  = MeshBuiltInVariable(state, output.kind);
		if (shared != nullptr && *shared != 0) {
			// Clip and cull distances share one per-vertex array builtin across their indices.
			output.variable_id = *shared;
		} else {
			auto element = type;
			if (MeshDistance(output.kind)) {
				const auto count =
				    output.kind == IR::StageOutputKind::ClipDistance ? clip_count : cull_count;
				element = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
			}
			output.variable_id =
			    MeshArray(state, spv::StorageClassOutput, element,
			              MeshPerPrimitive(output.kind) ? mesh.max_primitives : mesh.max_vertices);
			if (shared != nullptr) {
				*shared = output.variable_id;
			}
			state.interface_variables.push_back(output.variable_id);
			state.builder.AddName(output.variable_id, output.debug_name.c_str());
			if (output.kind == IR::StageOutputKind::Parameter) {
				state.builder.AddAnnotation(spv::OpDecorate, output.variable_id,
				                            spv::DecorationLocation, output.location);
			} else {
				state.builder.AddAnnotation(spv::OpDecorate, output.variable_id,
				                            spv::DecorationBuiltIn, builtin);
			}
			if (MeshPerPrimitive(output.kind)) {
				state.builder.AddAnnotation(spv::OpDecorate, output.variable_id,
				                            spv::DecorationPerPrimitiveEXT); // PerPrimitiveEXT
			}
		}
		// Per-primitive outputs are read by another invocation, through the provoking vertex.
		const bool per_primitive = MeshPerPrimitive(output.kind);
		output.mesh_data_variable =
		    MeshArray(state, per_primitive ? spv::StorageClassWorkgroup : spv::StorageClassPrivate,
		              type, per_primitive ? mesh.max_vertices : state.lane_count);
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
	const bool shared = MeshPerPrimitive(kind);
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
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
			state.builder.AddFunction(spv::OpStore, pointer, value);
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	state.builder.AddFunction(spv::OpControlBarrier, ConstantU32(state, spv::ScopeWorkgroup),
	                          ConstantU32(state, spv::ScopeWorkgroup),
	                          ConstantU32(state, spv::MemorySemanticsAcquireReleaseMask |
	                                                 spv::MemorySemanticsWorkgroupMemoryMask));
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
				if (MeshPerPrimitive(output.kind)) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable, spv::StorageClassPrivate, type,
				             ConstantU32(state, half));
				uint32_t pointer = 0;
				if (MeshDistance(output.kind)) {
					pointer = state.builder.AllocateId();
					state.builder.AddFunction(
					    spv::OpAccessChain, TypePointer(state, spv::StorageClassOutput, type),
					    pointer, output.variable_id, index, ConstantU32(state, output.index));
				} else {
					pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput, type,
					                      index);
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
				    ConstantU32(state, component * 10u), ConstantU32(state, 10));
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
				if (!MeshPerPrimitive(output.kind)) {
					continue;
				}
				const auto value   = MeshLoad(state, output.mesh_data_variable,
				                              spv::StorageClassWorkgroup, TypeU32(state),
				                              vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
