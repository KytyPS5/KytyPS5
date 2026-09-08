#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, uint32_t storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(OpTypeArray, {type, ConstantU32(state, count)});
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, uint32_t storage, uint32_t type,
                     uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpAccessChain, TypePointer(state, storage, type), pointer, variable, index});
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, uint32_t storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction({OpLoad, type, value, pointer});
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	return kind == IR::StageOutputKind::Layer ? TypeU32(state) : TypeF32Vector(state, 4);
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
		    state, StorageClassOutput, type,
		    output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives : mesh.max_vertices);
		// Only Layer is read by another invocation, through the primitive's provoking vertex.
		const bool shared = output.kind == IR::StageOutputKind::Layer;
		output.mesh_data_variable = MeshArray(
		    state, shared ? StorageClassWorkgroup : StorageClassPrivate, type,
		    shared ? mesh.max_vertices : state.lane_count);
		state.interface_variables.push_back(output.variable_id);
		state.builder.AddName(output.variable_id, output.debug_name.c_str());
		if (output.kind == IR::StageOutputKind::Parameter) {
			state.builder.AddAnnotation(
			    {OpDecorate, output.variable_id, DecorationLocation, output.location});
		} else {
			state.builder.AddAnnotation(
			    {OpDecorate, output.variable_id, DecorationBuiltIn,
			     output.kind == IR::StageOutputKind::Layer ? BuiltInLayer : BuiltInPosition});
		}
		if (output.kind == IR::StageOutputKind::Layer) {
			state.builder.AddAnnotation({OpDecorate, output.variable_id, 5271u}); // PerPrimitiveEXT
		}
	}
	state.mesh_allocation = MeshArray(state, StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, StorageClassPrivate, TypeU32(state), state.lane_count);
	state.mesh_primitives =
	    MeshArray(state, StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.mesh_cull = MeshArray(state, StorageClassOutput, TypeBool(state), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.interface_variables.push_back(state.mesh_cull);
	state.builder.AddAnnotation({OpDecorate, state.mesh_primitives, DecorationBuiltIn,
	                             5296u}); // PrimitiveTriangleIndicesEXT
	state.builder.AddAnnotation(
	    {OpDecorate, state.mesh_cull, DecorationBuiltIn, 5299u}); // CullPrimitiveEXT
	state.builder.AddAnnotation({OpDecorate, state.mesh_cull, 5271u});
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
	return MeshElement(state, output->mesh_data_variable,
	                   shared ? StorageClassWorkgroup : StorageClassPrivate,
	                   MeshOutputType(state, kind),
	                   shared ? EmitLocalInvocationIndex(state)
	                          : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(
	    {OpIEqual, TypeBool(state), first, EmitLocalInvocationIndex(state), ConstantU32(state, 0)});
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction({OpBitFieldUExtract, TypeU32(state), value, allocation,
			                           ConstantU32(state, field * 12u),
			                           ConstantU32(state, field == 0 ? 10u : 11u)});
			const auto pointer = MeshElement(state, state.mesh_allocation, StorageClassWorkgroup,
			                                 TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction({OpStore, pointer, value});
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	state.builder.AddFunction(
	    {OpFunction, TypeVoid(state), state.main_func, FunctionControlNone, TypeFunction(state)});
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(
	    {OpFunctionCall, TypeVoid(state), state.builder.AllocateId(), state.mesh_guest_func});
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	state.builder.AddFunction(
	    {OpControlBarrier, ConstantU32(state, ScopeWorkgroup), ConstantU32(state, ScopeWorkgroup),
	     ConstantU32(state, MemorySemanticsAcquireRelease | MemorySemanticsWorkgroupMemory)});
	const auto vertices = MeshLoad(state, state.mesh_allocation, StorageClassWorkgroup,
	                               TypeU32(state), ConstantU32(state, 0));
	const auto primitives = MeshLoad(state, state.mesh_allocation, StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 1));
	state.builder.AddFunction({5295u, vertices, primitives}); // OpSetMeshOutputsEXT
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction({OpULessThan, TypeBool(state), is_vertex, index, vertices});
		EmitIfCondition(state, is_vertex, [&] {
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value = MeshLoad(state, output.mesh_data_variable, StorageClassPrivate,
				                            type, ConstantU32(state, half));
				const auto pointer =
				    MeshElement(state, output.variable_id, StorageClassOutput, type, index);
				state.builder.AddFunction({OpStore, pointer, value});
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction({OpULessThan, TypeBool(state), is_primitive, index, primitives});
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction({OpBitFieldUExtract, TypeU32(state), vertex[component],
				                           packed, ConstantU32(state, component * 10u),
				                           ConstantU32(state, 10)});
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction({OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                           vertex[0], vertex[1], vertex[2]});
			const auto triangle_pointer = MeshElement(
			    state, state.mesh_primitives, StorageClassOutput, TypeU32Vector(state, 3), index);
			state.builder.AddFunction({OpStore, triangle_pointer, triangle});
			const auto null_bit =
			    EmitBinaryU32(state, OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(
			    {OpINotEqual, TypeBool(state), culled, null_bit, ConstantU32(state, 0)});
			state.builder.AddFunction(
			    {OpStore,
			     MeshElement(state, state.mesh_cull, StorageClassOutput, TypeBool(state), index),
			     culled});
			for (const auto& output: state.outputs) {
				if (output.kind != IR::StageOutputKind::Layer) {
					continue;
				}
				const auto layer = MeshLoad(
				    state, output.mesh_data_variable, StorageClassWorkgroup, TypeU32(state),
				    vertex[state.input_info.vertex->mesh.provoking_vertex]);
				const auto pointer = MeshElement(state, output.variable_id, StorageClassOutput,
				                                 TypeU32(state), index);
				state.builder.AddFunction({OpStore, pointer, layer});
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction({OpReturn});
	state.builder.AddFunction({OpFunctionEnd});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
