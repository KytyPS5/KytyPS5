#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"

#include <algorithm>
#include <array>

namespace Libs::Graphics::ShaderRecompiler::Frontend {

namespace {

IR::ExportTargetKind ExportTargetKindFromTarget(uint32_t target, uint32_t& index) {
	index = 0;
	switch (target) {
		case 0x08u: return IR::ExportTargetKind::MrtZ;
		case 0x09u: return IR::ExportTargetKind::Null;
		case 0x14u: return IR::ExportTargetKind::Primitive;
		default: break;
	}
	if (target <= 0x07u) {
		index = target;
		return IR::ExportTargetKind::Mrt;
	}
	if (target >= 0x0cu && target <= 0x0fu) {
		index = target - 0x0cu;
		return IR::ExportTargetKind::Position;
	}
	if (target >= 0x20u && target <= 0x3fu) {
		index = target - 0x20u;
		return IR::ExportTargetKind::Parameter;
	}
	return IR::ExportTargetKind::Unknown;
}

// Maps a V_INTERP_P2_F32 VSRC operand (the VGPR actually holding the barycentric J weight for
// this specific interpolated attribute) back to which PsBarycentricMode it belongs to, mirroring
// shadPS4's vgpr_to_interp table (shader_recompiler/frontend/translate/vector_interpolation.cpp).
// P2's VSRC is the pair's SECOND vgpr (J); P1 (a no-op here, see below) would use the first (I).
// ShaderCalcPsSystemInputBase/ShaderGetStaticInputInfoPS (shader.cpp) allocate each enabled mode
// a consecutive {I, J} VGPR pair in that order, so barycentric_vgpr[mode] + 1 == this VSRC.
uint8_t ResolveBarycentricModeForVgpr(const ShaderPixelInputInfo& info, uint32_t vgpr) {
	for (size_t mode = 0; mode < info.barycentric_vgpr.size(); mode++) {
		const auto base = info.barycentric_vgpr[mode];
		if (base != UINT32_MAX && base + 1u == vgpr) {
			return static_cast<uint8_t>(mode);
		}
	}
	return UINT8_MAX;
}

} // namespace

IR::ExportFlags Translator::AddExportInfo(const Decoder::Instruction& inst) {
	IR::ExportInfo info;
	info.kind        = ExportTargetKindFromTarget(inst.exp.target, info.index);
	info.target      = inst.exp.target;
	info.en          = inst.exp.en;
	info.done        = inst.exp.done;
	info.compr       = inst.exp.compr;
	info.vm          = inst.exp.vm;
	const auto index = static_cast<uint32_t>(program.export_info.size());
	program.export_info.push_back(info);
	return {.index = index, .pc = inst.pc};
}

// ASTRO's Playroom black-draw investigation, session 25/26 (2026-09-10): the compile-time
// decision this function makes -- per destination component, whether a vertex attribute comes
// from real guest memory or is baked to a constant 0.0/1.0 -- was invisible to every prior
// session's diagnostics (draw-time logs only ever saw the ALREADY-BAKED shader, not the
// resolution decision itself). If a component that should read real per-vertex data is instead
// baked to a wrong constant, the resulting shader is deterministically wrong on every draw,
// which matches the observed symptom (uniform black on every frame of the defect window,
// confirmed session 25) far better than a data-dependent bug would. Rate-limited on shader hash
// (this fires once per distinct static_state at compile time, so the cap is generous) and
// gated the same way as this session's other targeted diagnostics via --shader-log-filter-hash.
static void LogEmbeddedFetchResolutionIfNeeded(uint64_t shader_hash, uint32_t attribute,
                                               uint32_t component, Prospero::BufferFormat raw_format,
                                               uint32_t component_count, uint32_t dst_sel,
                                               Format::FormattedSourceKind kind,
                                               uint32_t resolved_component) {
	if (!Config::ShaderLogHashAllowed(shader_hash)) {
		return;
	}
	static Log::RateLimit limiter {"EmbeddedFetchResolution", 256};
	const auto hit = limiter.Hit();
	if (!hit) {
		return;
	}
	const char* kind_name = kind == Format::FormattedSourceKind::Memory   ? "Memory"
	                        : kind == Format::FormattedSourceKind::Zero   ? "Zero"
	                        : kind == Format::FormattedSourceKind::One    ? "One"
	                                                                     : "Invalid";
	LOGF("EmbeddedFetchResolution[%llu]: hash=0x%016llx attr=%u component=%u "
	     "raw_format=0x%02x format_component_count=%u dst_sel=0x%x -> kind=%s"
	     " resolved_component=%u\n",
	     *hit, static_cast<unsigned long long>(shader_hash), attribute, component,
	     static_cast<uint32_t>(raw_format), component_count, dst_sel, kind_name,
	     resolved_component);
}

void Translator::TranslateEmbeddedFetch(const Decoder::Instruction& inst, uint32_t attribute,
                                        uint32_t component_count,
                                        const ShaderBufferResource& resource) {
	const auto format = Format::GetFormatInfo(resource.Format());
	for (uint32_t component = 0; component < component_count; component++) {
		auto source = Format::FormattedSource {Format::FormattedSourceKind::Memory, component};
		if (inst.formatted && !inst.typed) {
			const auto dst_sel = GetDstSel(resource.DstSelXYZW(), component);
			source             = Format::ResolveFormattedSource(format, dst_sel);
			if (source.kind == Format::FormattedSourceKind::Invalid) {
				EXIT("invalid formatted vertex input %u at pc 0x%08x", attribute, inst.pc);
			}
			LogEmbeddedFetchResolutionIfNeeded(program.shader_hash, attribute, component,
			                                   resource.Format(), format.component_count,
			                                   dst_sel, source.kind, source.component);
		}
		IR::Value value;
		if (source.kind == Format::FormattedSourceKind::Memory) {
			value = ir.Emit(IR::ValueOpcode::GetAttribute,
			                {IR::Value(attribute), IR::Value(source.component)});
			auto& required = program.info.vertex_fetch_components[attribute];
			required = static_cast<uint8_t>(std::max<uint32_t>(required, source.component + 1u));
		} else {
			value = IR::Value(Format::FormattedConstantBits(format, source.kind));
		}
		WriteOperand(OffsetOperand(inst.dst, component), value);
	}
}

void Translator::V_INTERP_P1_F32() {}

void Translator::V_INTERP_P2_F32(const Decoder::Instruction& inst) {
	const auto value = ir.Emit(IR::ValueOpcode::GetAttribute,
	                           {IR::Value(inst.src1.value), IR::Value(inst.src2.value)});
	WriteOperand(inst.dst, value);
	// Record which barycentric mode this specific attribute (inst.src1 = ATTR, 0-31) was
	// actually interpolated with, so the SPIR-V emitter can decorate its Parameter input with
	// the real Centroid/Sample/NoPerspective qualifier per-attribute instead of the single
	// shader-wide ps_no_perspective flag (see ShaderInfo::ps_param_interp_mode, ShaderIR.h).
	if (pixel_input != nullptr && inst.src1.value < 32u) {
		const auto mode = ResolveBarycentricModeForVgpr(*pixel_input, inst.src0.reg);
		if (mode != UINT8_MAX) {
			program.info.ps_param_interp_mode[inst.src1.value] = mode;
		}
	}
}

void Translator::V_INTERP_MOV_F32(const Decoder::Instruction& inst) {
	if (inst.src0.value >= 3u) {
		EXIT("v_interp_mov_f32 mode %u is reserved at pc 0x%08x", inst.src0.value, inst.pc);
	}
	const auto value = ir.Emit(
	    IR::ValueOpcode::GetInterpolationParameter,
	    {IR::Value(inst.src1.value), IR::Value(inst.src2.value), IR::Value(inst.src0.value)});
	WriteOperand(inst.dst, value);
}

void Translator::EXP(const Decoder::Instruction& inst) {
	uint32_t index = 0;
	if (ExportTargetKindFromTarget(inst.exp.target, index) == IR::ExportTargetKind::Unknown) {
		EXIT("unsupported EXP target 0x%02x at pc 0x%08x", inst.exp.target, inst.pc);
	}
	std::array<IR::Value, 4> components {IR::Value(0u), IR::Value(0u), IR::Value(0u),
	                                     IR::Value(0u)};
	for (uint32_t source = 0; source < std::min(inst.src_count, 4u); source++) {
		components[source] = ReadRawU32(PlainOperand(SourceAt(inst, source)));
	}
	const auto data = ir.Emit(IR::ValueOpcode::CompositeConstructU32x4,
	                          {components[0], components[1], components[2], components[3]});
	ir.Emit(IR::ValueOpcode::SetAttribute, {data, ir.GetExec()}, AddExportInfo(inst));
}

bool Translator::EmitInterpolation(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::V_INTERP_P1_F32: V_INTERP_P1_F32(); return true;
		case Decoder::Opcode::V_INTERP_P2_F32: V_INTERP_P2_F32(inst); return true;
		case Decoder::Opcode::V_INTERP_MOV_F32: V_INTERP_MOV_F32(inst); return true;
		default: return false;
	}
}

} // namespace Libs::Graphics::ShaderRecompiler::Frontend
