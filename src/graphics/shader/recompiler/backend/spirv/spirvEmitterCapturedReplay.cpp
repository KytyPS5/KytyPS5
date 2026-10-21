#include "common/assert.h"
#include "graphics/shader/capturedVertexLayout.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <bit>
#include <vector>

// The vertex shader that draws vertex outputs captured by a compute pass (capturedVertexLayout.h).

namespace Libs::Graphics::ShaderRecompiler::Spirv {

CapturedVertexLayout MeshCaptureLayout(const IR::Program&         program,
                                       const ShaderMeshInputInfo& mesh) {
	CapturedVertexLayout layout {.vertices = mesh.max_vertices, .primitives = mesh.max_primitives};
	bool                 has_position = false;
	for (const auto& output: program.info.outputs) {
		switch (output.kind) {
			case IR::StageOutputKind::Position: has_position = true; break;
			case IR::StageOutputKind::Parameter: layout.parameters++; break;
			case IR::StageOutputKind::ClipDistance:
				layout.clip_distances = std::max(layout.clip_distances, output.index + 1u);
				break;
			case IR::StageOutputKind::CullDistance:
				layout.cull_distances = std::max(layout.cull_distances, output.index + 1u);
				break;
			case IR::StageOutputKind::Layer: break;
			default:
				EXIT("mesh output kind=%u cannot be captured: hash=0x%016" PRIx64 "\n",
				     static_cast<uint32_t>(output.kind), program.shader_hash);
		}
	}
	if (!has_position || !layout.Valid()) {
		EXIT("mesh program cannot be captured: position=%d vertices=%u primitives=%u clip=%u "
		     "cull=%u hash=0x%016" PRIx64 "\n",
		     has_position ? 1 : 0, layout.vertices, layout.primitives, layout.clip_distances,
		     layout.cull_distances, program.shader_hash);
	}
	return layout;
}

MeshEmulationVertexProgram EmitMeshEmulationVertexProgram(const IR::Program&   program,
                                                          ShaderStageInputInfo input_info) {
	const auto& mesh   = input_info.vertex->mesh;
	const auto  layout = MeshCaptureLayout(program, mesh);
	// The location of each parameter, in record order.
	std::vector<uint32_t> locations;
	bool                  has_layer = false;
	for (const auto& output: program.info.outputs) {
		if (output.kind == IR::StageOutputKind::Parameter) {
			locations.push_back(output.location);
		}
		has_layer |= output.kind == IR::StageOutputKind::Layer;
	}

	Builder b(0x00010300u);
	if (has_layer) {
		b.RequireVersion(0x00010500u);
		b.RequireCapability(spv::CapabilityShaderLayer);
	}
	b.RequireCapability(spv::CapabilityShader);
	b.RequireCapability(spv::CapabilityInt64);
	b.RequireCapability(spv::CapabilityPhysicalStorageBufferAddresses);
	b.RequireExtension("SPV_KHR_physical_storage_buffer");
	if (layout.clip_distances != 0) {
		b.RequireCapability(spv::CapabilityClipDistance);
	}
	b.AddMemoryModel(spv::AddressingModelPhysicalStorageBuffer64, spv::MemoryModelGLSL450);

	const auto t_void  = b.Type(spv::OpTypeVoid);
	const auto t_fn    = b.Type(spv::OpTypeFunction, t_void);
	const auto t_bool  = b.Type(spv::OpTypeBool);
	const auto t_u32   = b.Type(spv::OpTypeInt, 32, 0);
	const auto t_i32   = b.Type(spv::OpTypeInt, 32, 1);
	const auto t_u64   = b.Type(spv::OpTypeInt, 64, 0);
	const auto t_f32   = b.Type(spv::OpTypeFloat, 32);
	const auto t_v4    = b.Type(spv::OpTypeVector, t_f32, 4);
	const auto t_v2u   = b.Type(spv::OpTypeVector, t_u32, 2);
	const auto pointer = [&](spv::StorageClass storage, uint32_t type) {
		return b.Type(spv::OpTypePointer, storage, type);
	};
	const auto u32c = [&](uint32_t value) { return b.Constant(spv::OpConstant, t_u32, value); };
	const auto u64c = [&](uint64_t value) {
		return b.Constant(spv::OpConstant, t_u64, static_cast<uint32_t>(value),
		                  static_cast<uint32_t>(value >> 32u));
	};
	const auto f32c = [&](float value) {
		return b.Constant(spv::OpConstant, t_f32, std::bit_cast<uint32_t>(value));
	};
	const auto binary = [&](spv::Op op, uint32_t type, uint32_t lhs, uint32_t rhs) {
		const auto result = b.AllocateId();
		b.AddFunction(op, type, result, lhs, rhs);
		return result;
	};
	const auto unary = [&](spv::Op op, uint32_t type, uint32_t value) {
		const auto result = b.AllocateId();
		b.AddFunction(op, type, result, value);
		return result;
	};
	const auto select = [&](uint32_t type, uint32_t condition, uint32_t yes, uint32_t no) {
		const auto result = b.AllocateId();
		b.AddFunction(spv::OpSelect, type, result, condition, yes, no);
		return result;
	};

	// Parameters: the record buffer address (two dwords) is all this shader needs.
	const auto push_array = b.DecoratedType(
	    spv::OpTypeArray, {{spv::OpDecorate, {spv::DecorationArrayStride, sizeof(uint32_t)}}},
	    t_u32, u32c(IR::PushData::MeshEmulatedDrawDwordCount));
	const auto push_block = b.DecoratedType(spv::OpTypeStruct,
	                                        {{spv::OpMemberDecorate, {0, spv::DecorationOffset, 0}},
	                                         {spv::OpDecorate, {spv::DecorationBlock}}},
	                                        push_array);
	const auto push = b.DefineGlobalVariable(pointer(spv::StorageClassPushConstant, push_block),
	                                         spv::StorageClassPushConstant);
	std::vector<uint32_t> interface;
	const auto            vertex_index =
	    b.DefineGlobalVariable(pointer(spv::StorageClassInput, t_i32), spv::StorageClassInput);
	b.AddAnnotation(spv::OpDecorate, vertex_index, spv::DecorationBuiltIn, spv::BuiltInVertexIndex);
	interface.push_back(vertex_index);
	const auto position_variable =
	    b.DefineGlobalVariable(pointer(spv::StorageClassOutput, t_v4), spv::StorageClassOutput);
	b.AddAnnotation(spv::OpDecorate, position_variable, spv::DecorationBuiltIn,
	                spv::BuiltInPosition);
	interface.push_back(position_variable);
	uint32_t clip_variable = 0;
	if (layout.clip_distances != 0) {
		const auto t_clip = b.Type(spv::OpTypeArray, t_f32, u32c(layout.clip_distances));
		clip_variable     = b.DefineGlobalVariable(pointer(spv::StorageClassOutput, t_clip),
		                                           spv::StorageClassOutput);
		b.AddAnnotation(spv::OpDecorate, clip_variable, spv::DecorationBuiltIn,
		                spv::BuiltInClipDistance);
		interface.push_back(clip_variable);
	}
	std::vector<uint32_t> parameters(locations.size(), 0);
	for (size_t i = 0; i < locations.size(); i++) {
		parameters[i] =
		    b.DefineGlobalVariable(pointer(spv::StorageClassOutput, t_v4), spv::StorageClassOutput);
		b.AddAnnotation(spv::OpDecorate, parameters[i], spv::DecorationLocation, locations[i]);
		interface.push_back(parameters[i]);
	}
	uint32_t layer_variable = 0;
	if (has_layer) {
		layer_variable = b.DefineGlobalVariable(pointer(spv::StorageClassOutput, t_u32),
		                                        spv::StorageClassOutput);
		b.AddAnnotation(spv::OpDecorate, layer_variable, spv::DecorationBuiltIn, spv::BuiltInLayer);
		interface.push_back(layer_variable);
	}

	const auto main = b.AllocateId();
	b.AddFunction(spv::OpFunction, t_void, main, spv::FunctionControlMaskNone, t_fn);
	b.AddFunction(spv::OpLabel, b.AllocateId());
	const auto push_dword = [&](uint32_t index) {
		const auto access = b.AllocateId();
		const auto value  = b.AllocateId();
		b.AddFunction(spv::OpAccessChain, pointer(spv::StorageClassPushConstant, t_u32), access,
		              push, u32c(0), u32c(index));
		b.AddFunction(spv::OpLoad, t_u32, value, access);
		return value;
	};
	uint32_t   record    = 0;
	const auto load_word = [&](uint32_t word) {
		const auto address = binary(spv::OpIAdd, t_u64, record,
		                            binary(spv::OpIMul, t_u64, unary(spv::OpUConvert, t_u64, word),
		                                   u64c(sizeof(uint32_t))));
		const auto access  = b.AllocateId();
		const auto value   = b.AllocateId();
		b.AddFunction(spv::OpConvertUToPtr, pointer(spv::StorageClassPhysicalStorageBuffer, t_u32),
		              access, address);
		constexpr uint32_t alignment = sizeof(uint32_t);
		b.AddFunction(spv::OpLoad, t_u32, value, access, spv::MemoryAccessAlignedMask, alignment);
		return value;
	};
	// The word of a value of vertex `vertex`: `first` is its word for vertex 0.
	const auto vertex_word = [&](uint32_t first, uint32_t vertex) {
		return binary(spv::OpIAdd, t_u32, u32c(first),
		              binary(spv::OpIMul, t_u32, vertex, u32c(layout.VertexWords())));
	};

	// Which record, primitive and corner this invocation draws.
	const auto raw_index = b.AllocateId();
	b.AddFunction(spv::OpLoad, t_i32, raw_index, vertex_index);
	const auto index        = unary(spv::OpBitcast, t_u32, raw_index);
	const auto per_record   = u32c(layout.VerticesPerRecord());
	const auto slot         = binary(spv::OpUDiv, t_u32, index, per_record);
	const auto rest         = binary(spv::OpUMod, t_u32, index, per_record);
	const auto prim         = binary(spv::OpUDiv, t_u32, rest, u32c(3));
	const auto corner       = binary(spv::OpUMod, t_u32, rest, u32c(3));
	const auto address_pair = b.AllocateId();
	b.AddFunction(spv::OpCompositeConstruct, t_v2u, address_pair,
	              push_dword(IR::PushData::MeshEmulationAddressDword),
	              push_dword(IR::PushData::MeshEmulationAddressDword + 1));
	record = binary(spv::OpIAdd, t_u64, unary(spv::OpBitcast, t_u64, address_pair),
	                binary(spv::OpIMul, t_u64, unary(spv::OpUConvert, t_u64, slot),
	                       u64c(layout.RecordBytes())));

	const auto primitive_count = load_word(u32c(1));
	const auto word =
	    binary(spv::OpIAdd, t_u32, u32c(layout.PrimitiveWord(0)),
	           binary(spv::OpIMul, t_u32, prim, u32c(CapturedVertexLayout::kPrimitiveWords)));
	const auto packed   = load_word(word);
	const auto index_of = [&](uint32_t which) {
		return binary(
		    spv::OpBitwiseAnd, t_u32,
		    binary(spv::OpShiftRightLogical, t_u32, packed,
		           binary(spv::OpIMul, t_u32, which, u32c(CapturedVertexLayout::kIndexBits))),
		    u32c((1u << CapturedVertexLayout::kIndexBits) - 1u));
	};
	// The guest's provoking vertex is drawn first, which is the vertex the host takes a flat
	// value from. The rotation keeps the winding.
	const auto which =
	    binary(spv::OpUMod, t_u32, binary(spv::OpIAdd, t_u32, corner, u32c(mesh.provoking_vertex)),
	           u32c(3));
	const auto vertex = index_of(which);

	// A primitive beyond the workgroup's count, one the program culled, and one with an index
	// outside the record draw nothing.
	const auto culled = binary(
	    spv::OpINotEqual, t_bool,
	    binary(spv::OpBitwiseAnd, t_u32, packed, u32c(CapturedVertexLayout::kCullFlag)), u32c(0));
	auto visible =
	    binary(spv::OpLogicalAnd, t_bool, binary(spv::OpULessThan, t_bool, prim, primitive_count),
	           unary(spv::OpLogicalNot, t_bool, culled));
	uint32_t corners[3] {};
	for (uint32_t c = 0; c < 3; c++) {
		corners[c] = index_of(u32c(c));
		visible    = binary(spv::OpLogicalAnd, t_bool, visible,
		                    binary(spv::OpULessThan, t_bool, corners[c], u32c(layout.vertices)));
	}
	// An absent primitive holds no data: read vertex 0 of the record for it.
	const auto source = select(t_u32, visible, vertex, u32c(0));
	// A cull distance rejects the primitive when it is negative at all three vertices. Zero and
	// NaN are not negative.
	for (uint32_t plane = 0; plane < layout.cull_distances; plane++) {
		auto outside = b.Constant(spv::OpConstantTrue, t_bool);
		for (const auto corner_vertex: corners) {
			const auto safe = select(t_u32, visible, corner_vertex, u32c(0));
			const auto distance =
			    unary(spv::OpBitcast, t_f32,
			          load_word(vertex_word(layout.CullDistanceWord(0, plane), safe)));
			outside = binary(spv::OpLogicalAnd, t_bool, outside,
			                 binary(spv::OpFOrdLessThan, t_bool, distance, f32c(0.0f)));
		}
		visible =
		    binary(spv::OpLogicalAnd, t_bool, visible, unary(spv::OpLogicalNot, t_bool, outside));
	}

	const auto load_vec4 = [&](uint32_t first, bool position) {
		uint32_t lanes[4] {};
		for (uint32_t c = 0; c < 4; c++) {
			lanes[c] = unary(spv::OpBitcast, t_f32, load_word(vertex_word(first + c, source)));
			if (position) {
				// One position for every corner of a rejected primitive: it has no area and
				// covers no sample.
				lanes[c] = select(t_f32, visible, lanes[c], f32c(c == 3 ? 1.0f : 0.0f));
			}
		}
		const auto value = b.AllocateId();
		b.AddFunction(spv::OpCompositeConstruct, t_v4, value, lanes[0], lanes[1], lanes[2],
		              lanes[3]);
		return value;
	};
	b.AddFunction(spv::OpStore, position_variable, load_vec4(layout.PositionWord(0), true));
	for (uint32_t i = 0; i < parameters.size(); i++) {
		b.AddFunction(spv::OpStore, parameters[i], load_vec4(layout.ParameterWord(0, i), false));
	}
	for (uint32_t plane = 0; plane < layout.clip_distances; plane++) {
		const auto distance =
		    unary(spv::OpBitcast, t_f32,
		          load_word(vertex_word(layout.ClipDistanceWord(0, plane), source)));
		const auto access = b.AllocateId();
		b.AddFunction(spv::OpAccessChain, pointer(spv::StorageClassOutput, t_f32), access,
		              clip_variable, u32c(plane));
		// A rejected primitive must not be clipped into something else: keep it inside.
		b.AddFunction(spv::OpStore, access, select(t_f32, visible, distance, f32c(0.0f)));
	}
	if (has_layer) {
		const auto layer = load_word(binary(spv::OpIAdd, t_u32, word, u32c(1)));
		b.AddFunction(spv::OpStore, layer_variable, select(t_u32, visible, layer, u32c(0)));
	}
	b.AddFunction(spv::OpReturn);
	b.AddFunction(spv::OpFunctionEnd);
	b.AddEntryPoint(spv::ExecutionModelVertex, main, "main", interface);
	return {b.Build(), layout.RecordWords()};
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
