// Box/triangle intersection and compressed vertex ordering adapted from AMD GPURT:
// src/shaders/IntersectCommon.hlsl and src/shadersClean/common/Common.hlsl.
// Copyright (c) 2018-2025 Advanced Micro Devices, Inc. All Rights Reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <array>
#include <limits>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

// Research: IMAGE_BVH_INTERSECT_RAY in software, reached only with KYTY_TRANSLATE_BVH=1 (the
// decoder otherwise stops at the instruction and the dispatch is skipped). Address words:
// node pointer, ray extent, origin xyz, direction xyz, inverse direction xyz (A16 packs the last
// six into three words). A box node returns its four child pointers (sorted by distance when the
// T# asks for it, 0xffffffff for a miss); a triangle node returns t numerator, t denominator and
// either the triangle id and hit flag or the two barycentrics.
void Translator::IMAGE_BVH_INTERSECT_RAY(const Decoder::Instruction& inst) {
	using O           = IR::ValueOpcode;
	using Vec3        = std::array<IR::F32, 3>;
	const auto u      = [](uint32_t value) { return IR::U32(IR::Value(value)); };
	const auto f      = [](float value) { return IR::F32(IR::Value::F32(value)); };
	const auto fp     = [&](O op, IR::F32 a, IR::F32 b) { return IR::F32(ir.Emit(op, {a, b})); };
	const auto cmp    = [&](O op, IR::F32 a, IR::F32 b) { return IR::U1(ir.Emit(op, {a, b})); };
	const auto choose = [&](IR::U1 condition, IR::F32 a, IR::F32 b) {
		return IR::F32(ir.Emit(O::SelectF32, {condition, a, b}));
	};
	const auto half = [&](IR::U32 word, uint32_t shift) {
		const auto bits = ir.BitwiseAnd(ir.ShiftRightLogical(word, u(shift)), u(0xffffu));
		return IR::F32(ir.Emit(O::ConvertF32F16, {ir.BitCastF16(bits)}));
	};
	std::array<IR::U32, 11> ray;
	for (uint32_t i = 0; i < inst.image_address_components; ++i) {
		const auto reg = i != 0u && inst.image_nsa_dwords != 0u ? inst.image_nsa_addr[i - 1u]
		                                                        : inst.src0.reg + i;
		ray[i]         = ir.GetVectorReg(static_cast<IR::VectorReg>(reg));
	}
	const auto extent = ir.BitCastF32(ray[1]);
	Vec3       origin, direction, inverse;
	for (uint32_t axis = 0; axis < 3; ++axis) {
		origin[axis] = ir.BitCastF32(ray[2u + axis]);
		if ((inst.image_sample_flags & Decoder::ImageSampleFlagA16) != 0u) {
			direction[axis] = half(ray[5u + axis / 2u], (axis % 2u) * 16u);
			inverse[axis]   = half(ray[5u + (axis + 3u) / 2u], ((axis + 3u) % 2u) * 16u);
		} else {
			direction[axis] = ir.BitCastF32(ray[5u + axis]);
			inverse[axis]   = ir.BitCastF32(ray[8u + axis]);
		}
	}
	std::array<IR::U32, 4> descriptor;
	for (uint32_t i = 0; i < 4; ++i)
		descriptor[i] = ReadU32(OffsetOperand(inst.src1, i));
	const auto node_type   = ir.BitwiseAnd(ray[0], u(7));
	const auto box16       = ir.IEqual(node_type, u(4));
	const auto box32       = ir.IEqual(node_type, u(5));
	const auto is_box      = ir.LogicalOr(box16, box32);
	const auto is_triangle = ir.ULessThan(node_type, u(4));
	const auto active      = ir.GetExec();
	const auto node_index  = ir.ShiftRightLogical(ray[0], u(3));
	const auto base_low    = ir.ShiftLeftLogical(descriptor[0], u(8));
	const auto base_high =
	    ir.BitwiseOr(ir.ShiftRightLogical(descriptor[0], u(24)),
	                 ir.ShiftLeftLogical(ir.BitwiseAnd(descriptor[1], u(0xff)), u(8)));
	const auto node_offset = ir.ShiftLeftLogical(ir.BitwiseAnd(ray[0], u(~7u)), u(3));
	const auto node_low    = ir.IAdd(base_low, node_offset);
	const auto node_high =
	    ir.IAdd(base_high, ir.IAdd(ir.ShiftRightLogical(ray[0], u(29)),
	                               ir.Select(ir.ULessThan(node_low, base_low), u(1), u(0))));
	const auto resource  = GetAddressResource(node_low, node_high);
	const auto in_bounds = [&](IR::U32 byte_offset) {
		const auto index = ir.IAdd(node_index, ir.ShiftRightLogical(byte_offset, u(6)));
		return ir.LogicalOr(ir.INotEqual(ir.BitwiseAnd(descriptor[3], u(0x3ff)), u(0)),
		                    ir.LogicalNot(ir.UGreaterThan(index, descriptor[2])));
	};
	const auto load = [&](IR::U32 offset, IR::U1 predicate) {
		IR::MemoryInfo memory;
		memory.kind            = IR::ResourceKind::Global;
		memory.data_bits       = 32;
		memory.data_dwords     = 1;
		memory.address_is_full = false;
		return IR::U32(ir.Emit(O::LoadAddressU32,
		                       {resource, offset, u(0),
		                        ir.LogicalAnd(active, ir.LogicalAnd(predicate, in_bounds(offset)))},
		                       AddMemoryInfo(memory, inst.pc)));
	};
	const auto infinity = f(std::numeric_limits<float>::infinity());
	const auto invalid  = u(UINT32_MAX);
	const auto grow     = ir.BitwiseAnd(ir.ShiftRightLogical(descriptor[1], u(23)), u(0xff));
	const auto growth = fp(O::FPAdd32, f(1.0f),
	                       fp(O::FPMul32, IR::F32(ir.Emit(O::ConvertF32U32, {grow})), f(0x1p-24f)));
	std::array<IR::U32, 4> children;
	std::array<IR::F32, 4> distances;
	for (uint32_t child = 0; child < 4; ++child) {
		auto near_t = f(-std::numeric_limits<float>::infinity());
		auto far_t  = infinity;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			std::array<IR::F32, 2> bounds;
			for (uint32_t side = 0; side < 2; ++side) {
				const uint32_t component = side * 3u + axis;
				const auto     packed = load(u(16u + child * 12u + (component / 2u) * 4u), box16);
				const auto     full   = load(u(16u + child * 24u + component * 4u), box32);
				bounds[side] =
				    choose(box16, half(packed, (component % 2u) * 16u), ir.BitCastF32(full));
			}
			const auto a = fp(O::FPMul32, fp(O::FPSub32, bounds[0], origin[axis]), inverse[axis]);
			const auto b = fp(O::FPMul32, fp(O::FPSub32, bounds[1], origin[axis]), inverse[axis]);
			const auto forward   = cmp(O::FPOrdGreaterThanEqual32, inverse[axis], f(0.0f));
			const auto axis_near = choose(forward, a, b);
			const auto axis_far  = choose(forward, b, a);
			near_t               = axis == 0u ? axis_near : fp(O::FPMax32, near_t, axis_near);
			far_t                = axis == 0u ? axis_far : fp(O::FPMin32, far_t, axis_far);
		}
		const auto nan = ir.LogicalOr(IR::U1(ir.Emit(O::FPIsNan32, {near_t})),
		                              IR::U1(ir.Emit(O::FPIsNan32, {far_t})));
		near_t         = choose(nan, infinity, fp(O::FPMax32, near_t, f(0.0f)));
		far_t =
		    choose(nan, f(-std::numeric_limits<float>::infinity()), fp(O::FPMin32, far_t, extent));
		const auto hit =
		    ir.LogicalAnd(in_bounds(ir.Select(box32, u(127), u(63))),
		                  cmp(O::FPOrdLessThanEqual32, near_t, fp(O::FPMul32, far_t, growth)));
		children[child]  = ir.Select(hit, load(u(child * 4u), is_box), invalid);
		distances[child] = near_t;
	}
	const auto sort = ir.INotEqual(ir.BitwiseAnd(descriptor[1], u(0x80000000u)), u(0));
	for (const auto pair:
	     std::array<std::array<uint32_t, 2>, 5> {{{0, 2}, {1, 3}, {0, 1}, {2, 3}, {1, 2}}}) {
		const auto a = pair[0], b = pair[1];
		const auto swap = ir.LogicalAnd(
		    sort, ir.LogicalOr(ir.IEqual(children[a], invalid),
		                       ir.LogicalAnd(ir.INotEqual(children[b], invalid),
		                                     cmp(O::FPOrdLessThan32, distances[b], distances[a]))));
		const auto old_child    = children[a];
		const auto old_distance = distances[a];
		children[a]             = ir.Select(swap, children[b], children[a]);
		children[b]             = ir.Select(swap, old_child, children[b]);
		distances[a]            = choose(swap, distances[b], distances[a]);
		distances[b]            = choose(swap, old_distance, distances[b]);
	}
	// Four triangles share up to five vertices. Triangle ID remaps barycentrics.
	const auto mapping =
	    ir.Select(ir.IEqual(node_type, u(0)), u(0x210),
	              ir.Select(ir.IEqual(node_type, u(1)), u(0x231),
	                        ir.Select(ir.IEqual(node_type, u(2)), u(0x432), u(0x042))));
	std::array<Vec3, 3> vertices;
	for (uint32_t vertex = 0; vertex < 3; ++vertex) {
		const auto index = ir.BitwiseAnd(ir.ShiftRightLogical(mapping, u(vertex * 4u)), u(0xf));
		for (uint32_t axis = 0; axis < 3; ++axis)
			vertices[vertex][axis] =
			    ir.BitCastF32(load(ir.IAdd(ir.IMul(index, u(12)), u(axis * 4u)), is_triangle));
	}
	const auto subtract = [&](const Vec3& a, const Vec3& b) {
		return Vec3 {fp(O::FPSub32, a[0], b[0]), fp(O::FPSub32, a[1], b[1]),
		             fp(O::FPSub32, a[2], b[2])};
	};
	const auto cross = [&](const Vec3& a, const Vec3& b) {
		Vec3 result;
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const auto j = (axis + 1u) % 3u, k = (axis + 2u) % 3u;
			result[axis] = fp(O::FPSub32, fp(O::FPMul32, a[j], b[k]), fp(O::FPMul32, a[k], b[j]));
		}
		return result;
	};
	const auto dot = [&](const Vec3& a, const Vec3& b) {
		return fp(O::FPAdd32,
		          fp(O::FPAdd32, fp(O::FPMul32, a[0], b[0]), fp(O::FPMul32, a[1], b[1])),
		          fp(O::FPMul32, a[2], b[2]));
	};
	const auto e1 = subtract(vertices[1], vertices[0]);
	const auto e2 = subtract(vertices[2], vertices[0]);
	const auto e3 = subtract(origin, vertices[0]);
	const auto s1 = cross(direction, e2), s2 = cross(e3, e1);
	const auto t = dot(e2, s2), denominator = dot(s1, e1);
	const auto i = dot(e3, s1), j = dot(direction, s2);
	const auto positive = cmp(O::FPOrdGreaterThan32, denominator, f(0.0f));
	const auto orient   = [&](IR::F32 value) {
		return choose(positive, value, fp(O::FPSub32, f(0.0f), value));
	};
	const auto d = orient(denominator), ti = orient(i), tj = orient(j);
	auto       hit = ir.LogicalAnd(in_bounds(u(63)), cmp(O::FPOrdGreaterThan32, d, f(0.0f)));
	hit            = ir.LogicalAnd(hit, cmp(O::FPOrdGreaterThanEqual32, orient(t), f(0.0f)));
	hit            = ir.LogicalAnd(hit, cmp(O::FPOrdGreaterThanEqual32, ti, f(0.0f)));
	hit            = ir.LogicalAnd(hit, cmp(O::FPOrdGreaterThanEqual32, tj, f(0.0f)));
	hit            = ir.LogicalAnd(hit, cmp(O::FPOrdLessThanEqual32, fp(O::FPAdd32, ti, tj), d));
	const auto triangle_id = load(u(60), is_triangle);
	const auto id =
	    ir.ShiftRightLogical(triangle_id, ir.IMul(ir.BitwiseAnd(node_type, u(3)), u(8)));
	const auto out_d       = choose(hit, denominator, f(1.0f));
	const auto remaining   = fp(O::FPSub32, fp(O::FPSub32, out_d, i), j);
	const auto barycentric = [&](IR::U32 index) {
		return choose(ir.IEqual(index, u(0)), remaining, choose(ir.IEqual(index, u(1)), i, j));
	};
	const auto bary_mode = ir.INotEqual(ir.BitwiseAnd(descriptor[3], u(1u << 24u)), u(0));
	std::array<IR::U32, 4> triangle {
	    ir.BitCastU32(choose(hit, t, infinity)), ir.BitCastU32(out_d),
	    ir.Select(bary_mode, ir.BitCastU32(barycentric(ir.BitwiseAnd(id, u(3)))), triangle_id),
	    ir.Select(bary_mode,
	              ir.BitCastU32(barycentric(ir.BitwiseAnd(ir.ShiftRightLogical(id, u(2)), u(3)))),
	              ir.Select(hit, u(1), u(0)))};
	for (uint32_t component = 0; component < 4; ++component) {
		WriteOperand(OffsetOperand(inst.dst, component),
		             ir.Select(is_box, children[component],
		                       ir.Select(is_triangle, triangle[component], invalid)));
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
