#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

constexpr uint32_t Candidate = spv::RayQueryIntersectionRayQueryCandidateIntersectionKHR;

uint32_t RayQuery(EmitterState& s) {
	if (s.ray_query_variable == 0) {
		s.ray_query_variable = s.builder.DefineGlobalVariable(
		    TypePointer(s, spv::StorageClassPrivate, s.builder.Type(spv::OpTypeRayQueryKHR)),
		    spv::StorageClassPrivate);
	}
	return s.ray_query_variable;
}

// The context address the host writes into shader data; 0 when the dispatch has no context.
uint32_t ContextAddress(EmitterState& s) {
	const auto dword = s.program.bindings.ray_query_dword;
	return PackU64(s, EmitShaderDataDwordLoad(s, dword), EmitShaderDataDwordLoad(s, dword + 1));
}

uint32_t LoadContext(EmitterState& s, uint32_t address, uint32_t offset, uint32_t type,
                     uint32_t alignment) {
	const auto at      = Binary(s, spv::OpIAdd, TypeU64(s), address, ConstantU64(s, offset));
	const auto pointer = Unary(s, spv::OpConvertUToPtr,
	                           TypePointer(s, spv::StorageClassPhysicalStorageBuffer, type), at);
	const auto value   = s.builder.AllocateId();
	s.builder.AddFunction(spv::OpLoad, type, value, pointer, spv::MemoryAccessAlignedMask,
	                      alignment);
	return value;
}

} // namespace

uint32_t EmitRayQueryValid(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      s       = ctx.state;
	const auto address = ContextAddress(s);
	const auto present = Binary(s, spv::OpINotEqual, TypeBool(s), address, ConstantU64(s, 0));
	return EmitValueOrDefaultIfCondition(s, present, TypeBool(s), ConstantBool(s, false), [&] {
		return Binary(s, spv::OpINotEqual, TypeBool(s), LoadContext(s, address, 0, TypeU32(s), 4),
		              ConstantU32(s, 0));
	});
}

// Only a finite ray with a nonnegative t_max is traced: any other starts a query that culls every
// instance, a miss as in the guest's traversal.
void EmitRayQueryInitialize(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& s = ctx.state;
	EmitIfCondition(s, ctx.Arg(inst, 8), [&] {
		const auto              f = TypeF32(s), b = TypeBool(s), zero = ConstantF32Value(s, 0.0f);
		std::array<uint32_t, 7> ray {};
		for (uint32_t i = 0; i < ray.size(); ++i) {
			ray[i] = Unary(s, spv::OpBitcast, f, ctx.Arg(inst, i));
		}
		auto traced = Binary(s, spv::OpFOrdGreaterThanEqual, b, ray[6], zero);
		for (uint32_t i = 0; i < 6; ++i) {
			const auto finite = Binary(s, spv::OpFOrdLessThan, b, EmitFAbsValue(s, ray[i]),
			                           ConstantF32(s, 0x7f800000u));
			traced            = Binary(s, spv::OpLogicalAnd, b, traced, finite);
		}
		for (auto& value: ray) {
			value = Select(s, f, traced, value, zero);
		}
		const auto vector = [&](uint32_t first) {
			const auto result = s.builder.AllocateId();
			s.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(s, 3), result,
			                      ray[first], ray[first + 1], ray[first + 2]);
			return result;
		};
		const auto structure = Unary(s, spv::OpConvertUToAccelerationStructureKHR,
		                             s.builder.Type(spv::OpTypeAccelerationStructureKHR),
		                             LoadContext(s, ContextAddress(s), 16, TypeU64(s), 8));
		s.builder.AddFunction(spv::OpRayQueryInitializeKHR, RayQuery(s), structure,
		                      ConstantU32(s, 0),
		                      Select(s, TypeU32(s), traced, ctx.Arg(inst, 7), ConstantU32(s, 0)),
		                      vector(0), zero, vector(3), ray[6]);
	});
}

uint32_t EmitRayQueryProceed(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& s = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    s, ctx.Arg(inst, 0), TypeBool(s), ConstantBool(s, false),
	    [&] { return Unary(s, spv::OpRayQueryProceedKHR, TypeBool(s), RayQuery(s)); });
}

// The mirror's SBT offset of an instance is its first prim record.
uint32_t EmitRayQueryCandidate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& s = ctx.state;
	return EmitValueOrDefaultIfCondition(
	    s, ctx.Arg(inst, 0), TypeU32Vector(s, 4), ConstantU32CompositeZero(s, 4), [&] {
		    const auto query = RayQuery(s), candidate = ConstantU32(s, Candidate);
		    const auto get = [&](spv::Op op, uint32_t type) {
			    return Binary(s, op, type, query, candidate);
		    };
		    const auto bits =
		        Unary(s, spv::OpBitcast, TypeU32Vector(s, 2),
				      get(spv::OpRayQueryGetIntersectionBarycentricsKHR, TypeF32Vector(s, 2)));
		    const auto record =
		        Binary(s, spv::OpIAdd, TypeU32(s),
				       get(spv::OpRayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetKHR,
				           TypeU32(s)),
				       get(spv::OpRayQueryGetIntersectionPrimitiveIndexKHR, TypeU32(s)));
		    const auto u = s.builder.AllocateId(), v = s.builder.AllocateId();
		    s.builder.AddFunction(spv::OpCompositeExtract, TypeU32(s), u, bits, 0u);
		    s.builder.AddFunction(spv::OpCompositeExtract, TypeU32(s), v, bits, 1u);
		    const auto t      = Unary(s, spv::OpBitcast, TypeU32(s),
			                          get(spv::OpRayQueryGetIntersectionTKHR, TypeF32(s)));
		    const auto result = s.builder.AllocateId();
		    s.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(s, 4), result, t, u, v,
			                      record);
		    return result;
	    });
}

// Instance i of the mirror has custom index i: the guest record at i * 128 in the instance array.
uint32_t EmitRayQueryCandidateInstance(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& s = ctx.state;
	return EmitValueOrDefaultIfCondition(s, ctx.Arg(inst, 0), TypeU64(s), ConstantU64(s, 0), [&] {
		const auto index  = Binary(s, spv::OpRayQueryGetIntersectionInstanceCustomIndexKHR,
		                           TypeU32(s), RayQuery(s), ConstantU32(s, Candidate));
		const auto offset = Binary(s, spv::OpShiftLeftLogical, TypeU64(s),
		                           Unary(s, spv::OpUConvert, TypeU64(s), index), ConstantU32(s, 7));
		return Binary(s, spv::OpIAdd, TypeU64(s),
		              LoadContext(s, ContextAddress(s), 24, TypeU64(s), 8), offset);
	});
}

void EmitRayQueryConfirm(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& s = ctx.state;
	EmitIfCondition(s, ctx.Arg(inst, 0), [&] {
		s.builder.AddFunction(spv::OpRayQueryConfirmIntersectionKHR, RayQuery(s));
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
