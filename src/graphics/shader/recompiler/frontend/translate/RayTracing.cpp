#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <array>

// Ray/BVH-node intersection (MIMG 230/231). The instruction takes no sampler; its SGPR operand
// is a 128-bit BVH T# whose base address is bits 39:0 in units of 256 bytes. A node pointer
// carries the node kind in its low three bits and the node index above them, addressing 64-byte
// units. Box nodes return the four child pointers ordered by intersection distance, misses
// replaced by the invalid-node sentinel; triangle nodes return the intersection parameters.
//
// The traversal is emitted as ordinary IR rather than hand-written SPIR-V: GetAddressResource
// plus LoadAddressU32 already give per-lane 64-bit addressed reads, so a node fetch is sixteen
// loads and everything after it is plain arithmetic the existing backend lowers.

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

constexpr uint32_t BvhInvalidNode   = 0xffffffffu;
constexpr uint32_t BvhBoundBits     = 18u;
constexpr uint32_t BvhPlaneBit      = 80u;
constexpr uint32_t BvhChildBaseBits = 29u;
constexpr uint32_t BvhChildTypeBit  = 29u;
constexpr uint32_t BvhExpBit        = 56u;
constexpr uint32_t BvhNodeDwords    = 16u;

// A child occupies this many 64-byte units, which is what makes the child pointers of a
// shared-exponent node implicit: they are a running sum over the four child kinds.
uint32_t BvhChildUnits(uint32_t kind) {
	switch (kind) {
		case 2u: return 4u; // big fp32 box
		case 3u:            // big shared-exponent box
		case 6u: return 2u; // instance
		default: return 1u;
	}
}

} // namespace

IR::U32 Translator::BvhNodeField(const std::array<IR::U32, 16>& node, uint32_t offset,
                                 uint32_t size) {
	const auto lo_index  = offset / 32u;
	const auto lo_offset = offset % 32u;
	const auto lo_size   = std::min(32u - lo_offset, size);
	const auto lo_mask   = lo_size == 32u ? 0xffffffffu : ((1u << lo_size) - 1u);
	auto       value =
	    ir.BitwiseAnd(ir.ShiftRightLogical(node[lo_index], IR::U32(IR::Value(lo_offset))),
	                  IR::U32(IR::Value(lo_mask)));
	if (lo_size != size) {
		const auto hi_size = size - lo_size;
		const auto hi_mask = (1u << hi_size) - 1u;
		value              = ir.BitwiseOr(
            value, ir.ShiftLeftLogical(
                       ir.BitwiseAnd(node[lo_index + 1u], IR::U32(IR::Value(hi_mask))),
                       IR::U32(IR::Value(lo_size))));
	}
	return value;
}

IR::F32 Translator::BvhDecompressBound(IR::U32 shared_exp, IR::U32 field, bool is_min) {
	const auto u = [&](uint32_t value) { return IR::U32(IR::Value(value)); };

	const auto sign     = ir.ShiftRightLogical(field, u(17u));
	const auto mantissa = ir.BitwiseAnd(field, u(~(1u << 17u)));
	const auto empty    = IR::U1(ir.Emit(IR::ValueOpcode::IEqual32, {mantissa, IR::Value(0u)}));
	const auto msb      = IR::U32(ir.Emit(IR::ValueOpcode::FindUMsb32, {mantissa}));

	// Minima round down and maxima round up, so a decompressed box never clips a child it
	// should have contained. A negative bound rounds the other way, hence the sign test.
	const auto sign_set = IR::U1(ir.Emit(IR::ValueOpcode::IEqual32, {sign, IR::Value(1u)}));
	const auto round_up =
	    is_min ? sign_set : IR::U1(ir.Emit(IR::ValueOpcode::LogicalNot, {sign_set}));

	// mantissa == 0: the value collapses to zero, or to the largest representable magnitude
	// below the shared exponent when rounding away from it.
	const auto exp_big   = IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThan32, {shared_exp, IR::Value(17u)}));
	const auto empty_ok  = IR::U1(ir.Emit(IR::ValueOpcode::LogicalAnd, {round_up, exp_big}));
	const auto empty_exp = ir.Select(empty_ok, ir.ISub(shared_exp, u(17u)), u(0u));
	const auto empty_man = ir.Select(round_up, u((1u << 23u) - 1u), u(0u));

	// mantissa != 0: renormalise so the leading one lands in the float mantissa, clamping the
	// shift to the shared exponent, then fill the vacated low bits when rounding away.
	const auto raw_diff  = ir.ISub(u(16u), msb);
	const auto exp_diff  = IR::U32(ir.Emit(IR::ValueOpcode::UMin32, {raw_diff, shared_exp}));
	const auto exp       = ir.ISub(shared_exp, exp_diff);
	const auto shift     = ir.IAdd(u(7u), exp_diff);
	const auto exp_max   = IR::U1(ir.Emit(IR::ValueOpcode::IEqual32, {exp, IR::Value(0xffu)}));
	const auto no_max    = IR::U1(ir.Emit(IR::ValueOpcode::LogicalNot, {exp_max}));
	const auto fill      = IR::U1(ir.Emit(IR::ValueOpcode::LogicalAnd, {round_up, no_max}));
	const auto trail     = ir.Select(fill, ir.ISub(ir.ShiftLeftLogical(u(1u), shift), u(1u)), u(0u));
	const auto shifted   = ir.ShiftLeftLogical(mantissa, shift);
	const auto man       = ir.BitwiseAnd(ir.BitwiseOr(shifted, trail), u((1u << 23u) - 1u));

	const auto final_exp = ir.Select(empty, empty_exp, exp);
	const auto final_man = ir.Select(empty, empty_man, man);
	const auto bits      = ir.BitwiseOr(ir.BitwiseOr(final_man, ir.ShiftLeftLogical(final_exp, u(23u))),
                                   ir.ShiftLeftLogical(sign, u(31u)));
	return ir.BitCastF32(bits);
}

Translator::BvhHit Translator::BvhSlab(const IR::F32 bmin[3], const IR::F32 bmax[3],
                                       const IR::F32 origin[3], const IR::F32 inv_dir[3],
                                       IR::F32 extent, IR::U32 child) {
	const auto sub = [&](IR::F32 a, IR::F32 b) {
		return IR::F32(ir.Emit(IR::ValueOpcode::FPSub32, {a, b}));
	};
	const auto mul = [&](IR::F32 a, IR::F32 b) {
		return IR::F32(ir.Emit(IR::ValueOpcode::FPMul32, {a, b}));
	};

	IR::F32 lo[3];
	IR::F32 hi[3];
	for (uint32_t axis = 0; axis < 3u; axis++) {
		const auto l1 = mul(sub(bmin[axis], origin[axis]), inv_dir[axis]);
		const auto l2 = mul(sub(bmax[axis], origin[axis]), inv_dir[axis]);
		lo[axis]      = IR::F32(ir.Emit(IR::ValueOpcode::FPMin32, {l1, l2}));
		hi[axis]      = IR::F32(ir.Emit(IR::ValueOpcode::FPMax32, {l1, l2}));
	}
	const auto tfar =
	    IR::F32(ir.Emit(IR::ValueOpcode::FPMinTri32, {hi[0], hi[1], hi[2]}));
	const auto tnear =
	    IR::F32(ir.Emit(IR::ValueOpcode::FPMaxTri32, {lo[0], lo[1], lo[2]}));

	const auto zero    = IR::F32(IR::Value::F32(0.0f));
	const auto ordered = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {tfar, tnear}));
	const auto ahead   = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdGreaterThanEqual32, {tfar, zero}));
	const auto within  = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {tnear, extent}));
	const auto hit     = IR::U1(ir.Emit(
        IR::ValueOpcode::LogicalAnd,
        {IR::Value(ir.Emit(IR::ValueOpcode::LogicalAnd, {ordered, ahead})), within}));

	const auto clamped  = IR::F32(ir.Emit(IR::ValueOpcode::FPMax32, {tnear, zero}));
	const auto infinity = IR::F32(IR::Value::F32(std::numeric_limits<float>::infinity()));
	return {IR::F32(ir.Emit(IR::ValueOpcode::SelectF32, {hit, clamped, infinity})),
	        ir.Select(hit, child, IR::U32(IR::Value(BvhInvalidNode)))};
}

void Translator::BvhSortHits(BvhHit hits[4]) {
	// Sorting network over four (distance, pointer) pairs: the destination registers are the
	// child pointers in intersection-time order.
	constexpr uint32_t pairs[5][2] = {{0, 1}, {2, 3}, {0, 2}, {1, 3}, {1, 2}};
	for (const auto& pair: pairs) {
		auto&      a       = hits[pair[0]];
		auto&      b       = hits[pair[1]];
		const auto swap    = IR::U1(ir.Emit(IR::ValueOpcode::FPOrdLessThan32, {b.distance, a.distance}));
		const auto near    = IR::F32(ir.Emit(IR::ValueOpcode::SelectF32, {swap, b.distance, a.distance}));
		const auto far     = IR::F32(ir.Emit(IR::ValueOpcode::SelectF32, {swap, a.distance, b.distance}));
		const auto near_id = ir.Select(swap, b.child, a.child);
		const auto far_id  = ir.Select(swap, a.child, b.child);
		a                  = {near, near_id};
		b                  = {far, far_id};
	}
}

bool Translator::IMAGE_BVH_INTERSECT_RAY(const Decoder::Instruction& inst) {
	const bool wide = inst.opcode == Decoder::Opcode::IMAGE_BVH64_INTERSECT_RAY;
	const auto u    = [&](uint32_t value) { return IR::U32(IR::Value(value)); };

	// Address registers, honouring the NSA encoding the same way image addresses do.
	const auto nsa_components =
	    std::min(inst.image_nsa_dwords * 4u, Decoder::MaxImageNsaAddressComponents);
	const auto address = [&](uint32_t index) -> IR::U32 {
		if (index != 0u && index - 1u < nsa_components) {
			return ir.GetVectorReg(static_cast<IR::VectorReg>(inst.image_nsa_addr[index - 1u]));
		}
		return ReadRawU32(OffsetOperand(PlainOperand(inst.src0), index));
	};

	// BVH T#: base address is bits 39:0 in units of 256 bytes. MIMG puts the resource in
	// consecutive SGPRs starting at src1.
	const auto desc0   = ReadScalarCode(inst.src1.reg);
	const auto desc1   = ReadScalarCode(inst.src1.reg + 1u);
	const auto base_lo = ir.ShiftLeftLogical(desc0, u(8u));
	const auto base_hi = ir.BitwiseOr(ir.ShiftRightLogical(desc0, u(24u)),
	                                  ir.ShiftLeftLogical(ir.BitwiseAnd(desc1, u(0xffu)), u(8u)));

	// node_pointer: kind in the low three bits, index above it, addressing 64-byte units.
	const auto pointer_lo = address(0);
	const auto kind       = ir.BitwiseAnd(pointer_lo, u(7u));
	auto       offset_lo  = ir.ShiftLeftLogical(ir.BitwiseAnd(pointer_lo, u(~7u)), u(3u));
	auto       offset_hi  = ir.ShiftRightLogical(pointer_lo, u(29u));
	if (wide) {
		offset_hi = ir.BitwiseOr(offset_hi, ir.ShiftLeftLogical(address(1), u(3u)));
	}

	const auto node_lo = ir.IAdd(base_lo, offset_lo);
	const auto carry   = ir.Select(IR::U1(ir.Emit(IR::ValueOpcode::ULessThan32, {node_lo, base_lo})),
	                               u(1u), u(0u));
	const auto node_hi = ir.IAdd(ir.IAdd(base_hi, offset_hi), carry);

	const auto ray = wide ? 2u : 1u;
	const auto extent = ir.BitCastF32(address(ray));
	IR::F32    origin[3];
	IR::F32    inv_dir[3];
	for (uint32_t axis = 0; axis < 3u; axis++) {
		origin[axis]  = ir.BitCastF32(address(ray + 1u + axis));
		inv_dir[axis] = ir.BitCastF32(address(ray + 7u + axis));
	}

	// Fetch the node. Sixteen dwords covers a 64-byte node; a 128-byte one is two of these and
	// only its first half carries the fields the box test needs.
	const auto resource = GetAddressResource(node_lo, node_hi);
	const auto active   = ir.GetExec();
	std::array<IR::U32, 16> node {};
	for (uint32_t dword = 0; dword < BvhNodeDwords; dword++) {
		// Issued as four dwordx4-shaped groups: an address load carries at most four
		// components of metadata, and the node address is per-lane so it cannot use the
		// scalar-address form that allows wider groups.
		IR::MemoryInfo component;
		component.kind            = IR::ResourceKind::Flat;
		component.offset          = dword * sizeof(uint32_t);
		component.data_dwords     = 1u;
		component.data_bits       = 32u;
		component.component_index = dword % 4u;
		component.component_count = 4u;
		component.address_is_full = true;
		node[dword] = IR::U32(ir.Emit(IR::ValueOpcode::LoadAddressU32,
		                              {resource, node_lo, node_hi, active},
		                              AddMemoryInfo(component, inst.pc)));
	}

	// Shared-exponent box node: implicit child pointers plus 18-bit bounds against a per-axis
	// shared exponent.
	const auto child_base  = BvhNodeField(node, 0u, BvhChildBaseBits);
	const auto child_types = BvhNodeField(node, BvhChildTypeBit, 12u);
	IR::U32    shared_exp[3];
	for (uint32_t axis = 0; axis < 3u; axis++) {
		shared_exp[axis] = BvhNodeField(node, BvhExpBit + axis * 8u, 8u);
	}

	BvhHit hits[4];
	auto   running = child_base;
	for (uint32_t child = 0; child < 4u; child++) {
		const auto child_kind =
		    ir.BitwiseAnd(ir.ShiftRightLogical(child_types, u(child * 3u)), u(7u));
		const auto pointer = ir.BitwiseOr(ir.ShiftLeftLogical(running, u(3u)), child_kind);

		// Advance by this child's footprint for the next slot's implicit pointer.
		auto units = u(1u);
		for (uint32_t candidate = 2u; candidate <= 6u; candidate++) {
			const auto matches =
			    IR::U1(ir.Emit(IR::ValueOpcode::IEqual32, {child_kind, IR::Value(candidate)}));
			units = ir.Select(matches, u(BvhChildUnits(candidate)), units);
		}
		running = ir.IAdd(running, units);

		IR::F32 bmin[3];
		IR::F32 bmax[3];
		for (uint32_t axis = 0; axis < 3u; axis++) {
			const auto min_bit =
			    BvhPlaneBit + BvhBoundBits * (6u * child + axis);
			const auto max_bit =
			    BvhPlaneBit + BvhBoundBits * (6u * child + 3u + axis);
			bmin[axis] = BvhDecompressBound(shared_exp[axis],
			                                BvhNodeField(node, min_bit, BvhBoundBits), true);
			bmax[axis] = BvhDecompressBound(shared_exp[axis],
			                                BvhNodeField(node, max_bit, BvhBoundBits), false);
		}
		hits[child] = BvhSlab(bmin, bmax, origin, inv_dir, extent, pointer);
	}
	BvhSortHits(hits);

	// Triangle leaves still have to be intersected; until that lands they report a miss, which
	// keeps traversal terminating rather than following an undecoded leaf.
	const auto is_box = IR::U1(ir.Emit(IR::ValueOpcode::UGreaterThan32, {kind, IR::Value(1u)}));
	const auto miss   = IR::U32(IR::Value(BvhInvalidNode));
	// DMASK is always 0xf here, so the four results land in consecutive destination registers.
	for (uint32_t component = 0; component < 4u; component++) {
		WriteOperand(OffsetOperand(inst.dst, component),
		             ir.Select(is_box, hits[component].child, miss));
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
