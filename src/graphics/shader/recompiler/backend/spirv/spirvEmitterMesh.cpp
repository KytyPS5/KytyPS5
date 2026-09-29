#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include "common/logging/log.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

// KYTY_MESH_CULL selects how much of the primitive guard below is emitted, for A/B runs:
// "off" emits none of it, "zero" only the zero-position test, anything else everything.
enum class MeshCull { Off, Zero, Full };

MeshCull MeshCullMode() {
	static const MeshCull mode = [] {
		const char* value = std::getenv("KYTY_MESH_CULL");
		if (value != nullptr && std::strcmp(value, "off") == 0) {
			return MeshCull::Off;
		}
		if (value != nullptr && std::strcmp(value, "zero") == 0) {
			return MeshCull::Zero;
		}
		return MeshCull::Full;
	}();
	return mode;
}

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

bool IsDistance(IR::StageOutputKind kind) {
	return kind == IR::StageOutputKind::ClipDistance || kind == IR::StageOutputKind::CullDistance;
}

// The per-lane value a guest export stores: a whole vector, or one clip or cull distance.
uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	if (kind == IR::StageOutputKind::Layer) {
		return TypeU32(state);
	}
	return IsDistance(kind) ? TypeF32(state) : TypeF32Vector(state, 4);
}

// Where the copy-out stores an output of the vertex at index: distances are elements of the
// vertex's float[N] ClipDistance or CullDistance array.
uint32_t MeshVertexOutputPointer(EmitterState& state, const OutputBinding& output, uint32_t index) {
	if (!IsDistance(output.kind)) {
		return MeshElement(state, output.variable_id, spv::StorageClassOutput,
		                   MeshOutputType(state, output.kind), index);
	}
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain,
	                          TypePointer(state, spv::StorageClassOutput, TypeF32(state)), pointer,
	                          output.variable_id, index, ConstantU32(state, output.index));
	return pointer;
}

// The guest's allocation clamped to the declared output size: larger counts are undefined.
uint32_t MeshCount(EmitterState& state, uint32_t field, uint32_t maximum) {
	const auto count  = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                             TypeU32(state), ConstantU32(state, field));
	if (MeshCullMode() != MeshCull::Full) {
		return count;
	}
	const auto limit  = ConstantU32(state, maximum);
	const auto within = Binary(state, spv::OpULessThanEqual, TypeBool(state), count, limit);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), result, within, count, limit);
	return result;
}

uint32_t ZeroPositionMaskWord(EmitterState& state, uint32_t vertex) {
	return MeshElement(
	    state, state.mesh_zero_position_mask, spv::StorageClassWorkgroup, TypeU32(state),
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), vertex, ConstantU32(state, 5)));
}

void MarkZeroPosition(EmitterState& state, uint32_t position, uint32_t vertex) {
	const auto equal = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFOrdEqual, TypeBoolVector(state, 4), equal, position,
	                          state.builder.Constant(spv::OpConstantNull, TypeF32Vector(state, 4)));
	const auto zero = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAll, TypeBool(state), zero, equal);
	EmitIfCondition(state, zero, [&] {
		const auto bit = Binary(
		    state, spv::OpShiftLeftLogical, TypeU32(state), ConstantU32(state, 1),
		    Binary(state, spv::OpBitwiseAnd, TypeU32(state), vertex, ConstantU32(state, 31)));
		state.builder.AddFunction(spv::OpAtomicOr, TypeU32(state), state.builder.AllocateId(),
		                          ZeroPositionMaskWord(state, vertex),
		                          ConstantU32(state, spv::ScopeWorkgroup),
		                          ConstantU32(state, spv::MemorySemanticsMaskNone), bit);
	});
}

// True when a primitive's vertex index is past the allocation or its vertex sits at (0,0,0,0).
uint32_t UnusableVertex(EmitterState& state, uint32_t vertex, uint32_t vertices) {
	const auto in_range = Binary(state, spv::OpULessThan, TypeBool(state), vertex, vertices);
	const auto safe     = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpSelect, TypeU32(state), safe, in_range, vertex,
	                          ConstantU32(state, 0));
	const auto pointer = ZeroPositionMaskWord(state, safe);
	const auto word    = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, TypeU32(state), word, pointer);
	const auto shifted =
	    Binary(state, spv::OpShiftRightLogical, TypeU32(state), word,
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), safe, ConstantU32(state, 31)));
	const auto marked =
	    Binary(state, spv::OpINotEqual, TypeBool(state),
	           Binary(state, spv::OpBitwiseAnd, TypeU32(state), shifted, ConstantU32(state, 1)),
	           ConstantU32(state, 0));
	if (MeshCullMode() != MeshCull::Full) {
		return marked;
	}
	const auto out_of_range = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLogicalNot, TypeBool(state), out_of_range, in_range);
	return Binary(state, spv::OpLogicalOr, TypeBool(state), marked, out_of_range);
}

} // namespace

void DefineMeshOutputs(EmitterState& state) {
	const auto& mesh = state.input_info.vertex->mesh;
	for (auto& output: state.outputs) {
		if (output.kind != IR::StageOutputKind::Position &&
		    output.kind != IR::StageOutputKind::Parameter &&
		    output.kind != IR::StageOutputKind::Layer) {
			EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(output.kind));
		}
		const auto type    = MeshOutputType(state, output.kind);
		output.variable_id = MeshArray(
		    state, spv::StorageClassOutput, type,
		    output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives : mesh.max_vertices);
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? mesh.max_vertices : state.lane_count);
		state.interface_variables.push_back(output.variable_id);
		state.builder.AddName(output.variable_id, output.debug_name.c_str());
		if (output.kind == IR::StageOutputKind::Parameter) {
			state.builder.AddAnnotation(spv::OpDecorate, output.variable_id,
			                            spv::DecorationLocation, output.location);
		} else {
			state.builder.AddAnnotation(spv::OpDecorate, output.variable_id, spv::DecorationBuiltIn,
			                            output.kind == IR::StageOutputKind::Layer
			                                ? spv::BuiltInLayer
			                                : spv::BuiltInPosition);
		}
		if (output.kind == IR::StageOutputKind::Layer) {
			state.builder.AddAnnotation(spv::OpDecorate, output.variable_id,
			                            spv::DecorationPerPrimitiveEXT); // PerPrimitiveEXT
		}
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
	// A vertex at (0,0,0,0), which an out-of-bounds vertex fetch exports, leaves nothing of its
	// primitives on AMD hardware; NVIDIA rasterises them as screen-wide wedges. The vertex-shader
	// path clips them with its zero-position guard; here a bit per vertex culls the primitives.
	if (MeshCullMode() != MeshCull::Off &&
	    std::ranges::any_of(state.outputs, [](const OutputBinding& output) {
		    return output.kind == IR::StageOutputKind::Position;
	    })) {
		state.mesh_zero_position_words = (mesh.max_vertices + 31u) / 32u;
		state.mesh_zero_position_mask  = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state),
		                                           state.mesh_zero_position_words);
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			Log::WriteToConsoleAndLog("Shader: emitted mesh zero-position primitive cull\n");
		}
	}
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

// The copy-out reads the lanes of each pass in turn; EmitLocalInvocationIndex reads the pass.
static void SelectMeshPass(EmitterState& state, uint32_t pass) {
	if (state.mesh_pass_variable != 0) {
		state.builder.AddFunction(spv::OpStore, state.mesh_pass_variable, ConstantU32(state, pass));
	}
}

void EmitMeshEntryPoint(EmitterState& state) {
	const auto& mesh = state.input_info.vertex->mesh;
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.lane_half = 0;
	SelectMeshPass(state, 0);
	if (state.mesh_zero_position_mask != 0) {
		// Cleared before the guest code: the barrier after it orders the clear before the marks.
		const auto first = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
		                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
		EmitIfCondition(state, first, [&] {
			for (uint32_t word = 0; word < state.mesh_zero_position_words; word++) {
				state.builder.AddFunction(
				    spv::OpStore,
				    MeshElement(state, state.mesh_zero_position_mask, spv::StorageClassWorkgroup,
				                TypeU32(state), ConstantU32(state, word)),
				    ConstantU32(state, 0));
			}
		});
	}
	if (state.mesh_segment_funcs.empty()) {
		state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
		                          state.mesh_guest_func);
	} else {
		// Every pass runs a segment before any pass runs the next: the guest barrier between
		// them then orders the same accesses as on hardware, where all waves run at once.
		for (size_t segment = 0; segment < state.mesh_segment_funcs.size(); segment++) {
			if (segment != 0) {
				EmitBarrier(state);
			}
			for (uint32_t pass = 0; pass < state.mesh_passes; pass++) {
				state.builder.AddFunction(spv::OpStore, state.mesh_pass_variable,
				                          ConstantU32(state, pass));
				state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state),
				                          state.builder.AllocateId(),
				                          state.mesh_segment_funcs[segment]);
			}
		}
	}
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshCount(state, 0, mesh.max_vertices);
	const auto primitives = MeshCount(state, 1, mesh.max_primitives);
	state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
	                          primitives); // OpSetMeshOutputsEXT
	for (uint32_t slot = 0; slot < state.lane_count * state.mesh_passes; slot++) {
		const auto half = slot % state.lane_count;
		SelectMeshPass(state, slot / state.lane_count);
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
				             ConstantU32(state, slot));
				state.builder.AddFunction(spv::OpStore, MeshVertexOutputPointer(state, output, index),
				                          value);
				if (output.kind == IR::StageOutputKind::Position &&
				    state.mesh_zero_position_mask != 0) {
					MarkZeroPosition(state, value, index);
				}
			}
		});
	}
	if (state.mesh_zero_position_mask != 0) {
		// Primitives test the marks of vertices that other invocations stored.
		EmitBarrier(state);
	}
	for (uint32_t slot = 0; slot < state.lane_count * state.mesh_passes; slot++) {
		SelectMeshPass(state, slot / state.lane_count);
		state.lane_half         = slot % state.lane_count;
		const auto index        = EmitLocalInvocationIndex(state);
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, slot));
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
			auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			if (state.mesh_zero_position_mask != 0) {
				for (const auto component: vertex) {
					culled = Binary(state, spv::OpLogicalOr, TypeBool(state), culled,
					                UnusableVertex(state, component, vertices));
				}
			}
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

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
