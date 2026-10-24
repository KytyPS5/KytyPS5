#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"

#include <algorithm>
#include <optional>

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

bool IsTessellationInput(IR::TessellationAttribute kind) {
	using Attribute = IR::TessellationAttribute;
	return kind == Attribute::ControlInput || kind == Attribute::EvaluationInput ||
	       kind == Attribute::PatchInput;
}

bool IsTessellationPatch(IR::TessellationAttribute kind) {
	using Attribute = IR::TessellationAttribute;
	return kind == Attribute::PatchOutput || kind == Attribute::PatchInput;
}

// The offset of a ring address inside its control point record when it is the same for every
// lane that passes the access predicate: control point terms are multiples of the stride. Only
// the final ring address is known not to wrap, so immediates count as signed integers.
std::optional<uint32_t> RecordOffset(const IR::Value& value, uint32_t stride,
                                     const IR::Value& predicate) {
	const auto resolved = value.Resolve();
	if (resolved.IsImmediate()) {
		const auto residue = int64_t {static_cast<int32_t>(resolved.U32())} % stride;
		return static_cast<uint32_t>(residue < 0 ? residue + stride : residue);
	}
	const auto* inst = resolved.TryInstruction();
	if (inst == nullptr) return std::nullopt;
	const auto operand = [&](size_t index) {
		return RecordOffset(inst->Arg(index), stride, predicate);
	};
	switch (inst->GetOpcode()) {
		case IR::ValueOpcode::IAdd32: {
			const auto left = operand(0), right = operand(1);
			if (!left || !right) return std::nullopt;
			return (*left + *right) % stride;
		}
		case IR::ValueOpcode::IMul32: {
			const auto left = operand(0), right = operand(1);
			if ((left && *left == 0) || (right && *right == 0)) return 0u;
			if (!left || !right) return std::nullopt;
			return static_cast<uint32_t>(uint64_t {*left} * *right % stride);
		}
		case IR::ValueOpcode::ShiftLeftLogical32: {
			const auto shift = inst->Arg(1).Resolve();
			if (!shift.IsImmediate() || shift.U32() >= 32u) return std::nullopt;
			if ((uint64_t {1} << shift.U32()) % stride == 0u) return 0u;
			const auto left = operand(0);
			if (!left) return std::nullopt;
			return static_cast<uint32_t>((uint64_t {*left} << shift.U32()) % stride);
		}
		case IR::ValueOpcode::SelectU32:
			// Lanes that pass the predicate see the selected operand.
			if (inst->Arg(0).Resolve() == predicate.Resolve()) return operand(1);
			return std::nullopt;
		default: return std::nullopt;
	}
}

uint32_t TessellationPointer(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto& state          = ctx.state;
	using Attribute      = IR::TessellationAttribute;
	const auto  kind     = static_cast<Attribute>(inst.Arg(0).U32());
	const auto& tess     = state.input_info.vertex->tess;
	const auto  variable = state.tess_variables.at(static_cast<uint32_t>(kind));
	EXIT_IF(variable == 0);
	const bool patch = IsTessellationPatch(kind);
	const auto storage =
	    IsTessellationInput(kind) ? spv::StorageClassInput : spv::StorageClassOutput;
	const auto pointer = state.builder.AllocateId();
	if (kind == Attribute::Factor) {
		EXIT_NOT_IMPLEMENTED(!inst.Arg(1).IsImmediate());
		const auto index = inst.Arg(1).U32() / 4u;
		const auto outer = index < 3u;
		EXIT_NOT_IMPLEMENTED(index >= 4u);
		state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, TypeF32(state)),
		                          pointer, outer ? variable : state.tess_inner_variable,
		                          ConstantU32(state, outer ? index : index - 3u));
		return pointer;
	}
	auto address = ctx.Arg(inst, 1);
	if (patch) {
		address = EmitBinaryU32(state, spv::OpISub, address, ConstantU32(state, tess.patch_base));
	}
	const bool local  = kind == Attribute::LocalOutput || kind == Attribute::ControlInput;
	const auto stride = local ? tess.ls_stride : tess.hs_stride;
	// A constant record offset keeps the attribute and component static, as the guest code
	// addresses them; only the control point index varies.
	const auto record =
	    patch ? std::nullopt : RecordOffset(inst.Arg(1), stride, inst.Arg(inst.NumArgs() - 1u));
	const auto offset =
	    patch    ? address
	    : record ? ConstantU32(state, *record)
	             : EmitBinaryU32(state, spv::OpUMod, address, ConstantU32(state, stride));
	const auto attribute =
	    EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 4));
	const auto component =
	    EmitBinaryU32(state, spv::OpBitwiseAnd,
	                  EmitBinaryU32(state, spv::OpShiftRightLogical, offset, ConstantU32(state, 2)),
	                  ConstantU32(state, 3));
	const auto type = TypePointer(state, storage, TypeU32(state));
	if (kind == Attribute::LocalOutput || patch) {
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, attribute,
		                          component);
	} else {
		// An HS writes only its own control point but may read any other.
		const auto vertex =
		    kind == Attribute::ControlOutput &&
		            inst.GetOpcode() == IR::ValueOpcode::SetTessellationAttribute
		        ? EmitLaneId(state)
		        : EmitBinaryU32(state, spv::OpUDiv, address, ConstantU32(state, stride));
		state.builder.AddFunction(spv::OpAccessChain, type, pointer, variable, vertex, attribute,
		                          component);
	}
	return pointer;
}

} // namespace

void DefineTessellationInterfaces(EmitterState& state) {
	using Attribute = IR::TessellationAttribute;
	std::array<bool, IR::NumTessellationAttributes> used {};
	for (const auto* block: state.program.blocks) {
		for (const auto& inst: *block) {
			if (inst.GetOpcode() == IR::ValueOpcode::GetTessellationAttribute ||
			    inst.GetOpcode() == IR::ValueOpcode::SetTessellationAttribute)
				used.at(inst.Arg(0).U32()) = true;
		}
	}
	if (std::ranges::none_of(used, [](bool value) { return value; })) return;
	const auto& tess  = state.input_info.vertex->tess;
	const auto  array = [&](uint32_t type, uint32_t count) {
		return state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	};
	for (uint32_t index = 0; index < used.size(); index++) {
		if (!used[index]) continue;
		const auto kind  = static_cast<Attribute>(index);
		const bool patch = IsTessellationPatch(kind);
		const auto storage =
		    IsTessellationInput(kind) ? spv::StorageClassInput : spv::StorageClassOutput;
		uint32_t type;
		if (kind == Attribute::Factor) {
			type = array(TypeF32(state), 4u);
		} else if (patch) {
			// HS and TES share the reflected patch layout so their interfaces match.
			type = array(TypeU32Vector(state, 4), (tess.patch_size + 15u) / 16u);
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
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation,
			                            patch ? (tess.hs_stride + 15u) / 16u : 0u);
		}
		if (kind == Attribute::Factor || patch) {
			state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPatch);
		}
	}
}

void DefineTessellationExecutionModes(EmitterState& state) {
	const auto& tess = state.input_info.vertex->tess;
	// CxVgtTfParam: triangles, integer or fractional-odd partitioning, clockwise output.
	EXIT_NOT_IMPLEMENTED(tess.domain != 1u ||
	                     (tess.partitioning != 0u && tess.partitioning != 2u) ||
	                     tess.output_topology != 2u);
	state.builder.RequireCapability(spv::CapabilityTessellation);
	if (state.program.stage == ShaderType::TessellationControl) {
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeOutputVertices,
		                               tess.output_control_points);
	} else {
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeTriangles);
		state.builder.AddExecutionMode(
		    state.main_func, tess.partitioning == 0u ? spv::ExecutionModeSpacingEqual
		                                             : spv::ExecutionModeSpacingFractionalOdd);
		state.builder.AddExecutionMode(state.main_func, spv::ExecutionModeVertexOrderCw);
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
	EmitIfCondition(ctx.state, ctx.Arg(inst, 3), [&] {
		auto value = ctx.Arg(inst, 2);
		if (inst.Arg(0).U32() == static_cast<uint32_t>(IR::TessellationAttribute::Factor)) {
			const auto floating = ctx.state.builder.AllocateId();
			ctx.state.builder.AddFunction(spv::OpBitcast, TypeF32(ctx.state), floating, value);
			value = floating;
		}
		ctx.state.builder.AddFunction(spv::OpStore, TessellationPointer(ctx, inst), value);
	});
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
