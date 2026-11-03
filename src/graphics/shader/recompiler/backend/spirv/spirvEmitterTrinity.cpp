#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t Bits(EmitterState& s, uint32_t value, uint32_t offset, uint32_t count) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(s), result, value,
	                      ConstantU32(s, offset), ConstantU32(s, count));
	return result;
}

uint32_t SetBits(EmitterState& s, uint32_t value, uint32_t field, uint32_t offset, uint32_t count) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpBitFieldInsert, TypeU32(s), result, value, field,
	                      ConstantU32(s, offset), ConstantU32(s, count));
	return result;
}

uint32_t Pair(EmitterState& s, uint32_t first, uint32_t second) {
	const auto result = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Pair(s), result, first, second);
	return result;
}

uint32_t StackIndex(EmitterState& s, uint32_t control, uint32_t ring) {
	return EmitAddU32(s, Bits(s, control, 15, 14),
	    Binary(s, spv::OpShiftLeftLogical, TypeU32(s), ring, ConstantU32(s, 5)));
}

uint32_t StackPush(EmitterState& s, const MemoryResourceAccess& memory,
                   uint32_t control, uint32_t node, uint32_t capacity) {
	const auto u = TypeU32(s), b = TypeBool(s), zero = ConstantU32(s, 0), one = ConstantU32(s, 1);
	const auto count = Bits(s, control, 0, 5), parent = Bits(s, control, 5, 5), ring = Bits(s, control, 10, 5);
	const auto index = StackIndex(s, control, ring);
	EmitIfCondition(s, EmitMemoryElementInBounds(s, memory, index), [&] {
		s.builder.AddFunction(spv::OpStore, EmitMemoryElementPointer(s, memory, index), node);
	});
	control = Binary(s, spv::OpBitwiseOr, u, control,
	    Select(s, u, Binary(s, spv::OpIEqual, b, count, capacity), ConstantU32(s, 1u << 30), zero));
	control = Select(s, u, Binary(s, spv::OpIEqual, b, parent, capacity),
	    Binary(s, spv::OpBitwiseAnd, u, control, ConstantU32(s, ~(1u << 29))), control);
	control = SetBits(s, control, EmitGlsl<GLSLstd450UMin, IR::Type::U32>(s,
	    EmitAddU32(s, count, one), capacity), 0, 5);
	control = SetBits(s, control, EmitGlsl<GLSLstd450UMin, IR::Type::U32>(s,
	    EmitAddU32(s, parent, one), capacity), 5, 5);
	const auto next = EmitAddU32(s, ring, one);
	return SetBits(s, control, Select(s, u, Binary(s, spv::OpIEqual, b, next, capacity), zero, next), 10, 5);
}

uint32_t StackPop(EmitterState& s, const MemoryResourceAccess& memory,
                  uint32_t control, uint32_t capacity) {
	const auto u = TypeU32(s), b = TypeBool(s), zero = ConstantU32(s, 0), one = ConstantU32(s, 1);
	const auto count = Bits(s, control, 0, 5), parent = Bits(s, control, 5, 5), ring = Bits(s, control, 10, 5);
	const auto has_parent = Binary(s, spv::OpINotEqual, b, Bits(s, control, 29, 1), zero);
	const auto returning = Binary(s, spv::OpLogicalAnd, b, has_parent,
	    Binary(s, spv::OpIEqual, b, parent, zero));
	control = SetBits(s, control, Select(s, u, returning, one, zero), 31, 1);
	control = Select(s, u, returning,
	    Binary(s, spv::OpBitwiseAnd, u, control, ConstantU32(s, ~(1u << 29))), control);
	const auto overflow = Binary(s, spv::OpINotEqual, b, Bits(s, control, 30, 1), zero);
	const auto empty = Pair(s,
	    Binary(s, spv::OpBitwiseAnd, u, control, ConstantU32(s, 0x5fffffff)),
	    Select(s, u, overflow, ConstantU32(s, 0xffffffff), ConstantU32(s, 0xfffffffe)));
	return EmitValueOrDefaultIfCondition(s, Binary(s, spv::OpINotEqual, b, count, zero),
	    TypeU32Pair(s), empty, [&] {
		control = SetBits(s, control, Binary(s, spv::OpISub, u, count, one), 0, 5);
		control = SetBits(s, control, Binary(s, spv::OpISub, u, parent, one), 5, 5);
		const auto previous = Select(s, u, Binary(s, spv::OpIEqual, b, ring, zero),
		    Binary(s, spv::OpISub, u, capacity, one), Binary(s, spv::OpISub, u, ring, one));
		control = SetBits(s, control, previous, 10, 5);
		const auto index = StackIndex(s, control, previous);
		const auto node = EmitValueOrDefaultIfCondition(s,
		    EmitMemoryElementInBounds(s, memory, index), u, zero, [&] {
			const auto value = s.builder.AllocateId();
			s.builder.AddFunction(spv::OpLoad, u, value, EmitMemoryElementPointer(s, memory, index));
			return value;
		});
		const auto instance = Binary(s, spv::OpIEqual, b, Bits(s, node, 0, 3), ConstantU32(s, 6));
		control = EmitValueOrDefaultIfCondition(s, instance, u, control, [&] {
			const auto nested = Binary(s, spv::OpINotEqual, b, Bits(s, control, 29, 1), zero);
			const auto discarded = Binary(s, spv::OpBitwiseOr, u,
			    SetBits(s, control, Bits(s, control, 5, 5), 0, 5), ConstantU32(s, 1u << 30));
			return Binary(s, spv::OpBitwiseOr, u,
			    SetBits(s, Select(s, u, nested, discarded, control), zero, 5, 5), ConstantU32(s, 1u << 29));
		});
		return Pair(s, control, node);
	});
}

void DefineBvhStack(EmitterState& s) {
	if (!s.program.info.bvh_stack) return;
	const auto u = TypeU32(s), b = TypeBool(s), vec4 = TypeU32Vector(s, 4), pair = TypeU32Pair(s);
	const auto zero = ConstantU32(s, 0), one = ConstantU32(s, 1);
	const bool local = s.lds_storage_class == spv::StorageClassFunction;
	const bool buffer = s.lds_storage_class == spv::StorageClassStorageBuffer;
	std::vector<uint32_t> types {u, u, vec4, vec4, u};
	if (local) types.push_back(TypeU32ArrayPointer(s, spv::StorageClassFunction, LdsDwordCount(s)));
	if (buffer) types.push_back(u);
	const auto signature = s.builder.Type(spv::OpTypeFunction, pair, types);
	s.bvh_stack_function = s.builder.AllocateId();
	s.builder.AddName(s.bvh_stack_function, "trinity_bvh_stack");
	s.builder.AddFunction(spv::OpFunction, pair, s.bvh_stack_function, spv::FunctionControlMaskNone, signature);
	std::vector<uint32_t> args;
	for (const auto type: types) {
		args.push_back(s.builder.AllocateId());
		s.builder.AddFunction(spv::OpFunctionParameter, type, args.back());
	}
	const auto old_lds = s.lds_variable, old_base = s.lds_base_dwords;
	if (local) s.lds_variable = args.back();
	if (buffer) s.lds_base_dwords = args.back();
	const auto memory = PrepareMemoryResourceAccess(s, {.kind = IR::ResourceKind::Lds});
	const auto entry = s.builder.AllocateId();
	EmitLabel(s, entry);
	const auto last = args[1], capacity = args[4];
	const auto walkback = Binary(s, spv::OpINotEqual, b, Bits(s, last, 3, 29), ConstantU32(s, 0x1fffffff));
	const auto last4 = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeConstruct, vec4, last4, last, last, last, last);
	const auto found = [&](uint32_t values) {
		return Unary(s, spv::OpAny, b, Binary(s, spv::OpIEqual, TypeBoolVector(s, 4), values, last4));
	};
	const auto initial_valid = Binary(s, spv::OpLogicalOr, b, Unary(s, spv::OpLogicalNot, b, walkback),
	    Binary(s, spv::OpLogicalOr, b, found(args[2]), found(args[3])));
	const auto header = s.builder.AllocateId(), body = s.builder.AllocateId();
	const auto cont = s.builder.AllocateId(), merge = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpBranch, header);
	EmitLabel(s, header);
	const auto index = s.builder.AllocateId(), control = s.builder.AllocateId();
	const auto valid = s.builder.AllocateId(), skip_low = s.builder.AllocateId(), skip_high = s.builder.AllocateId();
	const auto index_phi = s.builder.AddDeferredPhi(u, index, 2);
	const auto control_phi = s.builder.AddDeferredPhi(u, control, 2);
	const auto valid_phi = s.builder.AddDeferredPhi(b, valid, 2);
	const auto low_phi = s.builder.AddDeferredPhi(b, skip_low, 2);
	const auto high_phi = s.builder.AddDeferredPhi(b, skip_high, 2);
	const auto more = Binary(s, spv::OpLogicalAnd, b, valid,
	    Binary(s, spv::OpULessThan, b, index, ConstantU32(s, 8)));
	s.builder.AddFunction(spv::OpLoopMerge, merge, cont, spv::LoopControlDontUnrollMask);
	s.builder.AddFunction(spv::OpBranchConditional, more, body, merge);
	EmitLabel(s, body);
	const auto high = Binary(s, spv::OpUGreaterThanEqual, b, index, ConstantU32(s, 4));
	const auto high4 = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpCompositeConstruct, TypeBoolVector(s, 4), high4, high, high, high, high);
	const auto values = Select(s, vec4, high4, args[3], args[2]);
	const auto node = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpVectorExtractDynamic, u, node, values, Bits(s, index, 0, 2));
	const auto sentinel = Binary(s, spv::OpIEqual, b, Bits(s, node, 3, 29), ConstantU32(s, 0x1fffffff));
	const auto enabled = Unary(s, spv::OpLogicalNot, b, Select(s, b, high, skip_high, skip_low));
	const auto marker = Binary(s, spv::OpLogicalAnd, b, sentinel, enabled);
	const auto next_low = Binary(s, spv::OpLogicalOr, b, skip_low,
	    Binary(s, spv::OpLogicalAnd, b, marker, Binary(s, spv::OpIEqual, b, Bits(s, node, 1, 1), zero)));
	const auto next_high = Binary(s, spv::OpLogicalOr, b, skip_high,
	    Binary(s, spv::OpLogicalAnd, b, high,
	        Binary(s, spv::OpLogicalAnd, b, marker, Binary(s, spv::OpIEqual, b, Bits(s, node, 2, 1), zero))));
	const auto next_valid = Unary(s, spv::OpLogicalNot, b,
	    Binary(s, spv::OpLogicalAnd, b, walkback, Binary(s, spv::OpIEqual, b, node, last)));
	const auto push = Binary(s, spv::OpLogicalAnd, b, next_valid,
	    Unary(s, spv::OpLogicalNot, b,
	        Binary(s, spv::OpLogicalOr, b, sentinel, Select(s, b, high, next_high, next_low))));
	const auto next_control = EmitValueOrDefaultIfCondition(s, push, u, control, [&] {
		return StackPush(s, memory, control, node, capacity);
	});
	s.builder.AddFunction(spv::OpBranch, cont);
	EmitLabel(s, cont);
	const auto next_index = Binary(s, spv::OpISub, u, index, one);
	s.builder.AddFunction(spv::OpBranch, header);
	s.builder.PatchDeferredPhi(index_phi, 0, ConstantU32(s, 7), entry);
	s.builder.PatchDeferredPhi(index_phi, 1, next_index, cont);
	s.builder.PatchDeferredPhi(control_phi, 0, args[0], entry);
	s.builder.PatchDeferredPhi(control_phi, 1, next_control, cont);
	s.builder.PatchDeferredPhi(valid_phi, 0, initial_valid, entry);
	s.builder.PatchDeferredPhi(valid_phi, 1, next_valid, cont);
	s.builder.PatchDeferredPhi(low_phi, 0, ConstantBool(s, false), entry);
	s.builder.PatchDeferredPhi(low_phi, 1, next_low, cont);
	s.builder.PatchDeferredPhi(high_phi, 0, ConstantBool(s, false), entry);
	s.builder.PatchDeferredPhi(high_phi, 1, next_high, cont);
	EmitLabel(s, merge);
	s.builder.AddFunction(spv::OpReturnValue, StackPop(s, memory, control, capacity));
	s.builder.AddFunction(spv::OpFunctionEnd);
	if (local) s.lds_variable = old_lds;
	if (buffer) s.lds_base_dwords = old_base;
}

} // namespace

void DefineTrinityFunctions(EmitterState& s) {
	DefineBvhStack(s);
	DefineTrinityBvhIntersect(s);
}

uint32_t EmitBvhStack(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& s = ctx.state;
	const auto capacity = ctx.Memory(inst).offset;
	if (s.bvh_stack_function == 0 || capacity < 8 || capacity > 31)
		ctx.Fail(inst, "invalid BVH stack function or capacity");
	const std::array values {ctx.Arg(inst, 0), ctx.Arg(inst, 1), ctx.Arg(inst, 2), ctx.Arg(inst, 3)};
	return EmitValueOrDefaultIfCondition(s, ctx.Arg(inst, 4), TypeU32Pair(s),
	    ConstantU32CompositeZero(s, 2), [&] {
		const auto result = s.builder.AllocateId();
		std::vector<uint32_t> call {spv::OpFunctionCall, TypeU32Pair(s), result,
		    s.bvh_stack_function, values[0], values[1], values[2], values[3], ConstantU32(s, capacity)};
		if (s.lds_storage_class == spv::StorageClassFunction) call.push_back(s.lds_variable);
		if (s.lds_storage_class == spv::StorageClassStorageBuffer) call.push_back(s.lds_base_dwords);
		s.builder.AddFunction(call);
		return result;
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
