#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>
#include <limits>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

bool Translator::IMAGE_BVH_INTERSECT_RAY(const Decoder::Instruction& inst) {
	// RTIP 1.1 reference: AMD GPURT IntersectCommon.hlsl (fast_intersect_triangle,
	// SwizzleBarycentrics, IntersectNodeBvh4). Triangle return mode 1 returns numerators.
	EXIT_IF(inst.opcode != Decoder::Opcode::IMAGE_BVH_INTERSECT_RAY || inst.data_bits != 32 ||
	        inst.dmask != 15 || !inst.image_r128 ||
	        inst.image_dimension != Decoder::ImageDimension::Dim1D ||
	        (inst.image_sample_flags & Decoder::ImageSampleFlagA16) != 0);
	using IR::Value;
	using IR::ValueOpcode;
	const auto u      = [](uint32_t value) { return IR::U32(Value(value)); };
	const auto f      = [](float value) { return IR::F32(Value::F32(value)); };
	const auto binary = [&](ValueOpcode opcode, Value a, Value b) {
		return ir.Emit(opcode, {a, b});
	};
	const auto add = [&](IR::F32 a, IR::F32 b) {
		return IR::F32(binary(ValueOpcode::FPAdd32, a, b));
	};
	const auto sub = [&](IR::F32 a, IR::F32 b) {
		return IR::F32(binary(ValueOpcode::FPSub32, a, b));
	};
	const auto mul = [&](IR::F32 a, IR::F32 b) {
		return IR::F32(binary(ValueOpcode::FPMul32, a, b));
	};
	const auto cmp = [&](ValueOpcode op, IR::F32 a, IR::F32 b) { return IR::U1(binary(op, a, b)); };
	const auto select = [&](IR::U1 test, IR::F32 a, IR::F32 b) {
		return IR::F32(ir.Emit(ValueOpcode::SelectF32, {test, a, b}));
	};
	const auto minimum = [&](IR::F32 a, IR::F32 b) {
		return IR::F32(binary(ValueOpcode::FPMin32, a, b));
	};
	const auto maximum = [&](IR::F32 a, IR::F32 b) {
		return IR::F32(binary(ValueOpcode::FPMax32, a, b));
	};
	const auto zero = f(0), one = f(1), infinity = f(std::numeric_limits<float>::infinity());
	std::array<IR::U32, 11> address;
	for (uint32_t i = 0; i < address.size(); ++i) {
		address[i] = i != 0 && i - 1 < inst.image_nsa_dwords * 4
		                 ? ir.GetVectorReg(static_cast<IR::VectorReg>(inst.image_nsa_addr[i - 1]))
		                 : ReadRawU32(OffsetOperand(PlainOperand(inst.src0), i));
	}
	std::array<IR::U32, 4> descriptor;
	for (uint32_t i = 0; i < descriptor.size(); ++i)
		descriptor[i] = ReadRawU32(OffsetOperand(PlainOperand(inst.src1), i));
	ir.Emit(ValueOpcode::ValidateBvhDescriptor,
	        {descriptor[0], descriptor[1], descriptor[2], descriptor[3]}, inst.pc);
	const auto node = address[0];
	const auto kind = ir.BitwiseAnd(node, u(7));
	const auto tri0 = ir.IEqual(kind, u(0)), tri1 = ir.IEqual(kind, u(1));
	const auto triangle = ir.LogicalOr(tri0, tri1);
	const auto box16 = ir.IEqual(kind, u(4)), box32 = ir.IEqual(kind, u(5));
	const auto box      = ir.LogicalOr(box16, box32);
	const auto index    = ir.ShiftRightLogical(node, u(3));
	const auto large    = ir.INotEqual(ir.BitwiseAnd(descriptor[3], u(0x3ff)), u(0));
	const auto in_range = ir.LogicalOr(large, ir.LogicalNot(ir.UGreaterThan(index, descriptor[2])));
	const auto wide_in_range = ir.LogicalOr(large, ir.ULessThan(index, descriptor[2]));
	const auto nonnull =
	    ir.INotEqual(ir.BitwiseOr(descriptor[0], ir.BitwiseAnd(descriptor[1], u(255))), u(0));
	const auto valid = ir.LogicalAnd(
	    ir.GetExec(), ir.LogicalAnd(nonnull, ir.LogicalAnd(in_range, ir.LogicalOr(triangle, box))));
	const auto valid_box32 = ir.LogicalAnd(valid, ir.LogicalAnd(box32, wide_in_range));
	const auto base_lo     = ir.ShiftLeftLogical(descriptor[0], u(8));
	const auto base_hi =
	    ir.BitwiseOr(ir.ShiftRightLogical(descriptor[0], u(24)),
	                 ir.ShiftLeftLogical(ir.BitwiseAnd(descriptor[1], u(255)), u(8)));
	const auto offset   = ir.ShiftLeftLogical(ir.BitwiseAnd(node, u(~7u)), u(3));
	const auto low      = ir.IAdd(base_lo, offset);
	const auto carry    = ir.Select(ir.ULessThan(low, base_lo), u(1), u(0));
	const auto high     = ir.IAdd(ir.IAdd(base_hi, ir.ShiftRightLogical(node, u(29))), carry);
	const auto resource = GetAddressResource(base_lo, base_hi);
	std::array<IR::U32, 28> words;
	for (uint32_t i = 0; i < words.size(); ++i) {
		IR::MemoryInfo memory;
		memory.kind            = IR::ResourceKind::Flat;
		memory.address_is_full = true;
		memory.offset          = i * 4;
		words[i] = IR::U32(ir.Emit(ValueOpcode::LoadAddressU32,
		                           {resource, low, high, i < 16 ? valid : valid_box32},
		                           AddMemoryInfo(memory, inst.pc)));
	}
	using Vec3 = std::array<IR::F32, 3>;
	Vec3 origin, direction, inverse, edge1, edge2, relative;
	for (uint32_t axis = 0; axis < 3; ++axis) {
		origin[axis]    = ir.BitCastF32(address[2 + axis]);
		direction[axis] = ir.BitCastF32(address[5 + axis]);
		inverse[axis]   = ir.BitCastF32(address[8 + axis]);
		const auto v0   = ir.BitCastF32(ir.Select(tri1, words[3 + axis], words[axis]));
		const auto v1   = ir.BitCastF32(ir.Select(tri1, words[9 + axis], words[3 + axis]));
		edge1[axis]     = sub(v1, v0);
		edge2[axis]     = sub(ir.BitCastF32(words[6 + axis]), v0);
		relative[axis]  = sub(origin[axis], v0);
	}
	const auto cross = [&](const Vec3& a, const Vec3& b) {
		return Vec3 {sub(mul(a[1], b[2]), mul(a[2], b[1])), sub(mul(a[2], b[0]), mul(a[0], b[2])),
		             sub(mul(a[0], b[1]), mul(a[1], b[0]))};
	};
	const auto dot = [&](const Vec3& a, const Vec3& b) {
		return add(add(mul(a[0], b[0]), mul(a[1], b[1])), mul(a[2], b[2]));
	};
	const auto p = cross(direction, edge2), q = cross(relative, edge1);
	const auto numerator = dot(edge2, q), determinant = dot(p, edge1), i_num = dot(relative, p),
	           j_num      = dot(direction, q);
	const auto reciprocal = IR::F32(ir.Emit(ValueOpcode::FPRecip32, {determinant}));
	const auto t = mul(numerator, reciprocal), bary_i = mul(i_num, reciprocal),
	           bary_j  = mul(j_num, reciprocal);
	const auto outside = [&](IR::F32 a) { return cmp(ValueOpcode::FPOrdLessThan32, a, zero); };
	auto       miss    = ir.LogicalOr(outside(t), ir.LogicalOr(outside(bary_i), outside(bary_j)));
	miss               = ir.LogicalOr(
	    miss, ir.LogicalOr(cmp(ValueOpcode::FPOrdGreaterThan32, bary_i, one),
	                       cmp(ValueOpcode::FPOrdGreaterThan32, add(bary_i, bary_j), one)));
	const auto denom = select(miss, one, determinant);
	const auto remap = [&](uint32_t shift) {
		const auto selector = ir.BitwiseAnd(
		    ir.ShiftRightLogical(words[15], ir.IAdd(ir.ShiftLeftLogical(kind, u(3)), u(shift))),
		    u(3));
		return select(ir.IEqual(selector, u(1)), i_num,
		              select(ir.IEqual(selector, u(2)), j_num, sub(sub(denom, i_num), j_num)));
	};
	const std::array<IR::U32, 4> triangle_result {ir.BitCastU32(select(miss, infinity, numerator)),
	                                              ir.BitCastU32(denom), ir.BitCastU32(remap(0)),
	                                              ir.BitCastU32(remap(2))};
	std::array<IR::U32, 4>       result;
	std::array<IR::F32, 4>       distance;
	const auto grow = ir.BitwiseAnd(ir.ShiftRightLogical(descriptor[1], u(23)), u(255));
	const auto factor =
	    add(one, mul(IR::F32(ir.Emit(ValueOpcode::ConvertF32U32, {grow})), f(0x1p-24f)));
	for (uint32_t child = 0; child < 4; ++child) {
		Vec3       near, far;
		const auto bound = [&](uint32_t component) {
			const auto half_bits =
			    ir.ShiftRightLogical(words[4 + child * 3 + component / 2], u(16 * (component % 2)));
			const auto half = ir.Emit(ValueOpcode::BitCastF16U16,
			                          {ir.Emit(ValueOpcode::ConvertU16U32, {half_bits})});
			return select(box16, IR::F32(ir.Emit(ValueOpcode::ConvertF32F16, {half})),
			              ir.BitCastF32(words[4 + child * 6 + component]));
		};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const auto a        = mul(sub(bound(axis), origin[axis]), inverse[axis]);
			const auto b        = mul(sub(bound(axis + 3), origin[axis]), inverse[axis]);
			const auto positive = cmp(ValueOpcode::FPOrdGreaterThanEqual32, inverse[axis], zero);
			near[axis]          = select(positive, a, b);
			far[axis]           = select(positive, b, a);
		}
		const auto entry         = maximum(maximum(near[0], near[1]), near[2]);
		const auto leave         = minimum(minimum(far[0], far[1]), far[2]);
		const auto clipped_entry = maximum(entry, zero);
		const auto clipped_leave = minimum(leave, ir.BitCastF32(address[1]));
		const auto finite_interval =
		    ir.LogicalNot(ir.LogicalOr(IR::U1(ir.Emit(ValueOpcode::FPIsNan32, {entry})),
		                               IR::U1(ir.Emit(ValueOpcode::FPIsNan32, {leave}))));
		const auto hit =
		    ir.LogicalAnd(finite_interval, cmp(ValueOpcode::FPOrdLessThanEqual32, clipped_entry,
		                                       mul(clipped_leave, factor)));
		const auto valid_box =
		    ir.LogicalAnd(box, ir.LogicalAnd(in_range, ir.LogicalOr(box16, wide_in_range)));
		result[child]   = ir.Select(ir.LogicalAnd(valid_box, hit), words[child], u(0xffffffff));
		distance[child] = clipped_entry;
	}
	const auto sort = ir.INotEqual(ir.BitwiseAnd(descriptor[1], u(0x80000000)), u(0));
	const auto swap = [&](uint32_t a, uint32_t b) {
		const auto after = ir.LogicalOr(
		    ir.IEqual(result[a], u(0xffffffff)),
		    ir.LogicalAnd(ir.INotEqual(result[b], u(0xffffffff)),
		                  cmp(ValueOpcode::FPOrdGreaterThan32, distance[a], distance[b])));
		const auto exchange = ir.LogicalAnd(sort, after);
		const auto left = result[a], right = result[b];
		const auto near = distance[a], far = distance[b];
		result[a]   = ir.Select(exchange, right, left);
		result[b]   = ir.Select(exchange, left, right);
		distance[a] = select(exchange, far, near);
		distance[b] = select(exchange, near, far);
	};
	swap(0, 2);
	swap(1, 3);
	swap(0, 1);
	swap(2, 3);
	swap(1, 2);
	for (uint32_t i = 0; i < result.size(); ++i) {
		const auto value = ir.Select(
		    nonnull, ir.Select(ir.LogicalAnd(triangle, in_range), triangle_result[i], result[i]),
		    u(0xffffffff));
		WriteRawU32(OffsetOperand(PlainOperand(inst.dst), i), value);
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
