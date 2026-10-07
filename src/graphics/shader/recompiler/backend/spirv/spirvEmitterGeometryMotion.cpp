#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInternal.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {
uint32_t Push(EmitterState& s, uint32_t index) {
	const auto base = s.program.stage == ShaderType::Vertex
	                      ? s.input_info.vertex->geometry_motion_dword
	                      : s.input_info.pixel->geometry_motion_dword;
	const auto ptr = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpAccessChain, TypePushConstantElementPointer(s), ptr,
	                      s.push_constant_variable, ConstantU32(s, 0), ConstantU32(s, base + index));
	return Unary(s, spv::OpLoad, TypeU32(s), ptr);
}
uint32_t FloatPush(EmitterState& s, uint32_t index) {
	return Unary(s, spv::OpBitcast, TypeF32(s), Push(s, index));
}
uint32_t Component(EmitterState& s, uint32_t value, uint32_t index) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeExtract, TypeF32(s), result, value, index);
	return result;
}
uint32_t Vector(EmitterState& s, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(s, 4), result, x, y, z, w);
	return result;
}
uint32_t Address(EmitterState& s, uint32_t offset) {
	const auto pair = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(s, 2), pair,
	                      Push(s, offset), Push(s, offset + 1));
	return Unary(s, spv::OpBitcast, TypeU64(s), pair);
}
uint32_t Pointer(EmitterState& s, uint32_t address, uint32_t record, uint32_t word) {
	const auto offset = Binary(s, spv::OpIAdd, TypeU32(s),
	                          Binary(s, spv::OpIMul, TypeU32(s), record, ConstantU32(s, 20)),
	                          ConstantU32(s, word * 4));
	return Unary(s, spv::OpConvertUToPtr, TypePhysicalU32Pointer(s),
	             Binary(s, spv::OpIAdd, TypeU64(s), address,
	                    Unary(s, spv::OpUConvert, TypeU64(s), offset)));
}
uint32_t LoadWord(EmitterState& s, uint32_t pointer) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpLoad, TypeU32(s), result, pointer, spv::MemoryAccessAlignedMask, 4u);
	return result;
}
void EmitVertex(EmitterState& s) {
	const auto zero = ConstantF32Value(s, 0);
	s.builder.AddFunction(spv::OpStore, s.motion_previous_variable, Vector(s, zero, zero, zero, zero));
	s.builder.AddFunction(spv::OpStore, s.motion_valid_variable, zero);
	const auto vertex = Unary(s, spv::OpBitcast, TypeU32(s),
	                          Unary(s, spv::OpLoad, TypeI32(s), InputVariableForKind(s, IR::StageInputKind::VertexIndex)));
	const auto instance = Binary(s, spv::OpISub, TypeU32(s),
	                             Unary(s, spv::OpBitcast, TypeU32(s),
	                                   Unary(s, spv::OpLoad, TypeI32(s), InputVariableForKind(s, IR::StageInputKind::InstanceIndex))),
	                             Push(s, 5));
	const auto capacity = Push(s, 4);
	const auto valid = Binary(s, spv::OpLogicalAnd, TypeBool(s),
	                          Binary(s, spv::OpULessThan, TypeBool(s), vertex, capacity),
	                          Binary(s, spv::OpULessThan, TypeBool(s), instance, Push(s, 6)));
	EmitIfCondition(s, valid, [&] {
		const auto record = Binary(s, spv::OpIAdd, TypeU32(s), vertex,
		                           Binary(s, spv::OpIMul, TypeU32(s), instance, capacity));
		const auto current = Address(s, 0);
		// Records are tagged with the frame that wrote them, so buffers need no
		// per-draw clearing. Indexed draws may invoke the same vertex more than
		// once; only the first invocation this frame writes its position.
		const auto tag = Binary(s, spv::OpShiftRightLogical, TypeU32(s), Push(s, 7), ConstantU32(s, 2));
		const auto claimed = s.builder.AllocateId();
		s.builder.AddFunction(spv::OpAtomicUMax, TypeU32(s), claimed,
		                      Pointer(s, current, record, 4), ConstantU32(s, 1), ConstantU32(s, 0), tag);
		EmitIfCondition(s, Binary(s, spv::OpULessThan, TypeBool(s), claimed, tag), [&] {
			const auto ptr = s.builder.AllocateId();
			s.builder.AddFunction(spv::OpAccessChain, TypePointer(s, spv::StorageClassOutput, TypeF32Vector(s, 4)),
			                      ptr, s.per_vertex_variable, ConstantU32(s, 0));
			const auto position = Unary(s, spv::OpLoad, TypeF32Vector(s, 4), ptr);
			for (uint32_t i = 0; i < 4; ++i) {
				s.builder.AddFunction(spv::OpStore, Pointer(s, current, record, i),
				                      Unary(s, spv::OpBitcast, TypeU32(s), Component(s, position, i)),
				                      spv::MemoryAccessAlignedMask, 4u);
			}
		});
		EmitIfCondition(s, Binary(s, spv::OpINotEqual, TypeBool(s),
		                          Binary(s, spv::OpBitwiseAnd, TypeU32(s), Push(s, 7), ConstantU32(s, 1)), ConstantU32(s, 0)), [&] {
			const auto previous = Address(s, 2);
			const auto previous_tag = Binary(s, spv::OpBitwiseAnd, TypeU32(s),
			                                 Binary(s, spv::OpISub, TypeU32(s), tag, ConstantU32(s, 1)),
			                                 ConstantU32(s, 0x3fffffffu));
			EmitIfCondition(s, Binary(s, spv::OpIEqual, TypeBool(s),
			                          LoadWord(s, Pointer(s, previous, record, 4)), previous_tag), [&] {
				uint32_t components[4];
				for (uint32_t i = 0; i < 4; ++i) {
					components[i] = Unary(s, spv::OpBitcast, TypeF32(s), LoadWord(s, Pointer(s, previous, record, i)));
				}
				s.builder.AddFunction(spv::OpStore, s.motion_previous_variable,
				                      Vector(s, components[0], components[1], components[2], components[3]));
				s.builder.AddFunction(spv::OpStore, s.motion_valid_variable, ConstantF32Value(s, 1));
			});
		});
	});
}
void EmitPixel(EmitterState& s) {
	const auto previous = Unary(s, spv::OpLoad, TypeF32Vector(s, 4), s.motion_previous_variable);
	const auto fragment = Unary(s, spv::OpLoad, TypeF32Vector(s, 4), InputVariableForKind(s, IR::StageInputKind::FragCoord));
	const auto w = Component(s, previous, 3);
	auto valid = Binary(s, spv::OpLogicalAnd, TypeBool(s),
	                          Binary(s, spv::OpFOrdGreaterThanEqual, TypeBool(s),
	                                 Unary(s, spv::OpLoad, TypeF32(s), s.motion_valid_variable), ConstantF32Value(s, .999999f)),
	                          Binary(s, spv::OpFOrdGreaterThan, TypeBool(s), w, ConstantF32Value(s, .00001f)));
	const auto nonfinite = Binary(s, spv::OpLogicalOr, TypeBoolVector(s, 4),
	                              Unary(s, spv::OpIsNan, TypeBoolVector(s, 4), previous),
	                              Unary(s, spv::OpIsInf, TypeBoolVector(s, 4), previous));
	valid = Binary(s, spv::OpLogicalAnd, TypeBool(s), valid,
	               Unary(s, spv::OpLogicalNot, TypeBool(s), Unary(s, spv::OpAny, TypeBool(s), nonfinite)));
	uint32_t motion[2];
	for (uint32_t i = 0; i < 2; ++i) {
		const auto ndc = Binary(s, spv::OpFDiv, TypeF32(s), Component(s, previous, i),
		                        Select(s, TypeF32(s), valid, w, ConstantF32Value(s, 1)));
		const auto old_uv = Binary(s, spv::OpFAdd, TypeF32(s), FloatPush(s, 8 + i),
		                          Binary(s, spv::OpFMul, TypeF32(s), FloatPush(s, 10 + i),
		                                 Binary(s, spv::OpFMul, TypeF32(s), ConstantF32Value(s, .5f),
		                                        Binary(s, spv::OpFAdd, TypeF32(s), ndc, ConstantF32Value(s, 1)))));
		const auto uv = Binary(s, spv::OpFMul, TypeF32(s), Component(s, fragment, i), FloatPush(s, 12 + i));
		motion[i] = Select(s, TypeF32(s), valid, Binary(s, spv::OpFSub, TypeF32(s), old_uv, uv), ConstantF32Value(s, 0));
	}
	const auto z = Component(s, fragment, 2);
	const auto inverted = Binary(s, spv::OpINotEqual, TypeBool(s),
	                             Binary(s, spv::OpBitwiseAnd, TypeU32(s), Push(s, 7), ConstantU32(s, 2)), ConstantU32(s, 0));
	const auto depth = Select(s, TypeF32(s), inverted, Binary(s, spv::OpFSub, TypeF32(s), ConstantF32Value(s, 1), z), z);
	s.builder.AddFunction(spv::OpStore, s.motion_output_variable,
	                      Vector(s, motion[0], motion[1], depth,
	                             Select(s, TypeF32(s), Binary(s, spv::OpINotEqual, TypeBool(s), Push(s, 4), ConstantU32(s, 0)),
	                                    Select(s, TypeF32(s), valid, ConstantF32Value(s, 1), ConstantF32Value(s, -1)), ConstantF32Value(s, 0))));
}
} // namespace

bool GeometryMotionEnabled(const EmitterState& s) {
	return (s.program.stage == ShaderType::Vertex && s.input_info.vertex->geometry_motion_dword != UINT32_MAX) ||
	       (s.program.stage == ShaderType::Pixel && s.input_info.pixel->geometry_motion_dword != UINT32_MAX);
}
void DefineGeometryMotion(EmitterState& s) {
	if (!GeometryMotionEnabled(s)) return;
	const auto storage = s.program.stage == ShaderType::Vertex ? spv::StorageClassOutput : spv::StorageClassInput;
	s.motion_previous_variable = DefineInterfaceVariable(s, TypeF32Vector(s, 4), storage, "kyty_previous_clip_position");
	s.motion_valid_variable = DefineInterfaceVariable(s, TypeF32(s), storage, "kyty_position_history_valid");
	s.builder.AddAnnotation(spv::OpDecorate, s.motion_previous_variable, spv::DecorationLocation, 27u);
	s.builder.AddAnnotation(spv::OpDecorate, s.motion_valid_variable, spv::DecorationLocation, 26u);
	if (s.program.stage == ShaderType::Pixel) {
		s.motion_output_variable = DefineInterfaceVariable(s, TypeF32Vector(s, 4), spv::StorageClassOutput, "kyty_geometry_motion_depth");
		s.builder.AddAnnotation(spv::OpDecorate, s.motion_output_variable, spv::DecorationLocation, 7u);
	}
}
void EmitGeometryMotion(EmitterState& s) {
	if (!GeometryMotionEnabled(s)) return;
	if (s.program.stage == ShaderType::Vertex) EmitVertex(s);
	else EmitPixel(s);
}
} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
