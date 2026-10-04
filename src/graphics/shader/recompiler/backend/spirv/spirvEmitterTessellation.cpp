#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <algorithm>
#include <bit>
#include <cstdlib>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

// Triangles store outer[3] + inner[1]; quads store outer[4] + inner[2]. A compile-time-known
// index picks its array and element directly. A dynamic one (e.g. a loop writing TF[i]) can't
// be routed to one of the two separate builtin arrays at compile time, so branch on which side
// it lands in at runtime and use the value as the access chain's index into that array -- SPIR-V
// access chains accept a non-constant index, just not a non-constant choice of *which* variable.
void EmitSetTessellationFactor(ValueEmitContext& ctx, const IR::Inst& inst, uint32_t value) {
	auto&      state   = ctx.state;
	const auto variable = state.tess_variables.at(static_cast<uint32_t>(IR::TessellationAttribute::Factor));
	EXIT_IF(variable == 0);
	const bool     quad  = state.input_info.vertex->tess.domain == 2u;
	const uint32_t outer = quad ? 4u : 3u;
	const uint32_t inner = quad ? 2u : 1u;
	const auto     type  = TypePointer(state, spv::StorageClassOutput, TypeF32(state));
	const auto store_at = [&](uint32_t array_variable, uint32_t index) {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, array_variable,
		                          ConstantU32(state, index));
		state.builder.AddFunction(spv::OpStore, pointer, value);
	};
	if (inst.Arg(1).IsImmediate()) {
		const auto index = inst.Arg(1).U32() / 4u;
		EXIT_NOT_IMPLEMENTED(index >= outer + (quad ? 2u : 1u));
		index < outer ? store_at(variable, index) : store_at(state.tess_inner_variable, index - outer);
		return;
	}
	const auto index =
	    EmitBinaryU32(state, spv::OpShiftRightLogical, ctx.Arg(inst, 1), ConstantU32(state, 2));
	EmitIfCondition(state, EmitCompareU32Constant(state, spv::OpULessThan, index, outer), [&] {
		const auto pointer = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, index);
		state.builder.AddFunction(spv::OpStore, pointer, value);
	});
	EmitIfCondition(state, EmitCompareU32Constant(state, spv::OpUGreaterThanEqual, index, outer), [&] {
		const auto inner_index = EmitBinaryU32(state, spv::OpISub, index, ConstantU32(state, outer));
		// An index past the inner factors writes nothing.
		EmitIfCondition(state, EmitCompareU32Constant(state, spv::OpULessThan, inner_index, inner), [&] {
			const auto pointer = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpAccessChain, type, pointer, state.tess_inner_variable,
			                          inner_index);
			state.builder.AddFunction(spv::OpStore, pointer, value);
		});
	});
}

uint32_t TessellationPointer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state          = ctx.state;
	using Attribute      = IR::TessellationAttribute;
	const auto  kind     = static_cast<Attribute>(inst.Arg(0).U32());
	const auto& tess     = state.input_info.vertex->tess;
	const auto  variable = state.tess_variables.at(static_cast<uint32_t>(kind));
	EXIT_IF(variable == 0);
	const auto input   = kind == Attribute::ControlInput || kind == Attribute::EvaluationInput ||
	                     (kind == Attribute::PatchOutput &&
	                      state.program.stage == ShaderType::TessellationEvaluation);
	const auto storage = input ? spv::StorageClassInput : spv::StorageClassOutput;
	const auto pointer = state.builder.AllocateId();
	if (kind == Attribute::Factor) {
		// Only a compile-time-known Factor index reaches here (GetTessellationAttribute, or a
		// SetTessellationAttribute whose dynamic index EmitSetTessellationAttribute already
		// special-cased below); it still needs a single pointer for a Load/Store the caller
		// performs itself.
		EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
		// Triangles store outer[3] + inner[1]; quads store outer[4] + inner[2].
		const bool     quad  = tess.domain == 2u;
		const uint32_t outer = quad ? 4u : 3u;
		const auto     index = inst.Arg(1).U32() / 4u;
		EXIT_NOT_IMPLEMENTED(index >= outer + (quad ? 2u : 1u));
		const bool is_outer = index < outer;
		state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, TypeF32(state)),
		                          pointer, is_outer ? variable : state.tess_inner_variable,
		                          ConstantU32(state, is_outer ? index : index - outer));
		return pointer;
	}
	auto address = ctx.Arg(inst, 1);
	if (kind == Attribute::PatchOutput) {
		address =
		    EmitBinaryU32(state, spv::OpISub, address, ConstantU32(state, state.tess_patch_base));
	}
	const bool local  = kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
	const auto stride = local ? tess.ls_stride : tess.hs_stride;
	const auto offset = kind == Attribute::PatchOutput ? address
	                                                   : EmitBinaryU32(state, spv::OpUMod, address,
	                                                                   ConstantU32(state, stride));
	const auto attribute =
	    EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 4));
	const auto component =
	    EmitBinaryU32(state, spv::OpBitwiseAnd,
	                  EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 2)),
	                  ConstantU32(state, 3));
	const auto type = TypePointer(state, storage, TypeU32(state));
	if (kind == Attribute::LocalOutput || kind == Attribute::PatchOutput) {
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, attribute,
		                          component);
	} else {
		// A ControlOutput write always targets the writing invocation's own control point
		// (its lane ID); a ControlOutput read is either a DS/TessellationEvaluation-side
		// EvaluationInput read or a hull shader reading back another invocation's
		// already-written control point (see LowerTessellationMemory's self_read case) --
		// both index by the byte address the guest computed, same as PatchOutput/LocalOutput.
		const bool is_write = inst.GetOpcode() == IR::ValueOpcode::SetTessellationAttribute;
		// The control point comes from the address the guest computed (its invocation index is
		// folded into v1, so the address already carries control_point * stride). The subgroup
		// lane is not the control point: one subgroup holds several patches, and the lane of
		// the second patch's first invocation is 3, past the end of a 3-element output array,
		// so every patch but the first lost its writes and its attributes read back as zero.
		(void)is_write;
		auto vertex = EmitBinaryU32(state, spv::OpUDiv, address, ConstantU32(state, stride));
		if (kind == Attribute::ControlInput) {
			// The guest addresses the LS ring of the whole thread group, and a gather table can name
			// a vertex of the draw rather than of this patch; Vulkan's input array holds one patch.
			vertex = EmitBinaryU32(state, spv::OpUMod, vertex,
			                       ConstantU32(state, std::max(tess.input_control_points, 1u)));
		}
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, vertex, attribute,
		                          component);
	}
	return pointer;
}

} // namespace

void DefineTessellationInterfaces(EmitterState& state) {
	using Attribute = IR::TessellationAttribute;
	std::array<bool, 6> used {};
	uint32_t            patch_begin = UINT32_MAX, patch_end = 0;
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() != IR::ValueOpcode::GetTessellationAttribute &&
			    inst.GetOpcode() != IR::ValueOpcode::SetTessellationAttribute)
				continue;
			const auto kind = inst.Arg(0).U32();
			used.at(kind)   = true;
			if (kind == static_cast<uint32_t>(Attribute::PatchOutput)) {
				EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
				patch_begin = std::min(patch_begin, inst.Arg(1).U32());
				patch_end   = std::max(patch_end, inst.Arg(1).U32() + 4u);
			}
		}
	}
	if (std::ranges::none_of(used, [](bool value) { return value; })) return;
	const auto& tess  = state.input_info.vertex->tess;
	const auto  array = [&](uint32_t type, uint32_t count) {
		return state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	};
	for (uint32_t index = 0; index < used.size(); index++) {
		if (!used[index]) continue;
		const auto kind    = static_cast<Attribute>(index);
		const bool input   = kind == Attribute::ControlInput || kind == Attribute::EvaluationInput ||
		                     (kind == Attribute::PatchOutput &&
		                      state.program.stage == ShaderType::TessellationEvaluation);
		const auto storage = input ? spv::StorageClassInput : spv::StorageClassOutput;
		uint32_t   type;
		if (kind == Attribute::Factor) {
			// BuiltInTessLevelOuter is exactly float[4] whatever the domain (triangles use three of
			// them); the inner levels live in their own float[2] variable below.
			type = array(TypeF32(state), 4u);
		} else if (kind == Attribute::PatchOutput) {
			// The patch-constant region can sit far above the control points (a hull shader that
			// keeps it at 0x7fc0 would otherwise need a ~2000 element array, far past the host
			// per-patch output limit, and the driver then overlaps it with the per-vertex outputs
			// the first control point just wrote). Index it from its own lowest address, rounded
			// down to 64 bytes so the hull and evaluation stages of one program agree on the base.
			state.tess_patch_base = patch_begin & ~63u;
			type = array(TypeU32Vector(state, 4), (patch_end - state.tess_patch_base + 15u) / 16u);
		} else {
			const bool local  = kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
			const auto stride = local ? tess.ls_stride : tess.hs_stride;
			type              = array(TypeU32Vector(state, 4), (stride + 15u) / 16u);
			if (kind != Attribute::LocalOutput) {
				type = array(type, local ? tess.input_control_points : tess.output_control_points);
			}
		}
		auto& variable = state.tess_variables[index];
		variable       = DefineInterfaceVariable(state, type, storage, "tess_attributes");
		if (kind == Attribute::Factor) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn,
			                            spv::BuiltInTessLevelOuter);
			state.tess_inner_variable =
			    DefineInterfaceVariable(state, array(TypeF32(state), 2u), storage, "tess_inner");
			state.builder.AddAnnotation(spv::OpDecorate, state.tess_inner_variable,
			                            spv::DecorationBuiltIn, spv::BuiltInTessLevelInner);
			state.builder.AddAnnotation(spv::OpDecorate, state.tess_inner_variable,
			                            spv::DecorationPatch);
		} else {
			state.builder.AddAnnotation(
			    spv::OpDecorate, variable, spv::DecorationLocation,
			    kind == Attribute::PatchOutput ? (tess.hs_stride + 15u) / 16u : 0u);
		}
		if (kind == Attribute::Factor || kind == Attribute::PatchOutput) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPatch);
		}
	}
}

void DefineTessellationExecutionModes(EmitterState& state) {
	const auto& tess = state.input_info.vertex->tess;
	// LowerTessellationMemory rejects domains and output topologies this emitter has no mode for.
	EXIT_NOT_IMPLEMENTED((tess.domain != 1u && tess.domain != 2u) ||
	                     (tess.output_topology != 2u && tess.output_topology != 3u));
	state.builder.RequireCapability(spv::CapabilityTessellation);
	if (state.program.stage == ShaderType::TessellationControl) {
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeOutputVertices,
		                               tess.output_control_points);
	} else {
		state.builder.AddExecutionMode(
		    state.main_func, tess.domain == 2u ? spv::ExecutionModeQuads
		                                       : spv::ExecutionModeTriangles);
		// VGT_TF_PARAM partitioning: 0 integer, 1 pow2 (no Vulkan equivalent; equal spacing is the
		// nearest), 2 fractional odd, 3 fractional even.
		state.builder.AddExecutionMode(state.main_func,
		                               tess.partitioning == 3u   ? spv::ExecutionModeSpacingFractionalEven
		                               : tess.partitioning == 2u ? spv::ExecutionModeSpacingFractionalOdd
		                                                         : spv::ExecutionModeSpacingEqual);
		state.builder.AddExecutionMode(state.main_func, tess.output_topology == 3u
		                                                    ? spv::ExecutionModeVertexOrderCcw
		                                                    : spv::ExecutionModeVertexOrderCw);
	}
}

uint32_t EmitGetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	return EmitValueOrZeroIfCondition(ctx.state, ctx.Arg(inst, 2), [&] {
		const auto value = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpLoad, TypeU32(ctx.state), value,
		                              TessellationPointer(ctx, inst));
		return value;
	});
}

void EmitSetTessellationAttribute(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto condition = ctx.Arg(inst, 3);
	if (inst.Arg(0).U32() == static_cast<uint32_t>(IR::TessellationAttribute::ControlOutput)) {
		// A hull shader's patch-constant stores sit far above the control points (0x8000 - 96 *
		// (patch + 1) in My First Gran Turismo) and, when the patch ordinal is not a compile-time constant, are not
		// recognised as patch outputs. Indexed as a control point they land far past the array,
		// which is undefined and trashes the first control point's data. Drop them.
		const auto& tess   = ctx.state.input_info.vertex->tess;
		const auto  vertex = EmitBinaryU32(ctx.state, spv::OpUDiv, ctx.Arg(inst, 1),
		                                   ConstantU32(ctx.state, tess.hs_stride));
		const auto  inside = ctx.state.builder.AllocateId();
		const auto  both   = ctx.state.builder.AllocateId();
		ctx.state.builder.AddFunction(spv::OpULessThan, TypeBool(ctx.state), inside, vertex,
		                              ConstantU32(ctx.state, tess.output_control_points));
		ctx.state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(ctx.state), both, condition, inside);
		condition = both;
	}
	EmitIfCondition(ctx.state, condition, [&] {
		auto value = ctx.Arg(inst, 2);
		if (inst.Arg(0).U32() == static_cast<uint32_t>(IR::TessellationAttribute::Factor)) {
			auto floating = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpBitcast, TypeF32(ctx.state), floating, value);
			// Factor stores go through the helper: it also handles an index only known at runtime.
			EmitSetTessellationFactor(ctx, inst, floating);
			return;
		}
		ctx.state.builder.AddFunction(spv::OpStore, TessellationPointer(ctx, inst), value);
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
