#include "common/assert.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/ShaderIRInternal.h"

#include <algorithm>
#include <fmt/format.h>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::IR {

std::string RegisterToString(Register reg) {
	switch (reg.file) {
		case RegisterFile::Scalar: return fmt::format("s{}", reg.index);
		case RegisterFile::Vector: return fmt::format("v{}", reg.index);
		case RegisterFile::Vcc:
			return reg.index == 0 ? std::string("vcc_lo") : std::string("vcc_hi");
		case RegisterFile::Exec:
			return reg.index == 0 ? std::string("exec_lo") : std::string("exec_hi");
		case RegisterFile::Scc:
			if (reg.index == static_cast<uint32_t>(Decoder::OperandKind::Scc)) {
				return "scc";
			}
			if (reg.index == static_cast<uint32_t>(Decoder::OperandKind::VccZ)) {
				return "vccz";
			}
			if (reg.index == static_cast<uint32_t>(Decoder::OperandKind::ExecZ)) {
				return "execz";
			}
			return fmt::format("scc_like_{}", reg.index);
		case RegisterFile::M0: return "m0";
		default: return "unknown_reg";
	}
}

std::string OperandToString(const Operand& operand) {
	std::string text;
	switch (operand.kind) {
		case OperandKind::Register: text = RegisterToString(operand.reg); break;
		case OperandKind::ImmediateU32: text = fmt::format("0x{:08x}", operand.imm); break;
		case OperandKind::PcRelativeU32: text = fmt::format("pc_rel(0x{:08x})", operand.imm); break;
		case OperandKind::Null: text = "null"; break;
		default: text = "unknown"; break;
	}
	if (operand.sdwa_sel != 6 || operand.sdwa_sext) {
		text += fmt::format(".sdwa(sel={},sext={})", operand.sdwa_sel, operand.sdwa_sext ? 1u : 0u);
	}
	if (operand.absolute) {
		text += ".abs";
	}
	if (operand.negate) {
		text += ".neg";
	}
	if (operand.op_sel || operand.op_sel_hi || operand.negate_hi) {
		text += fmt::format(".opsel(lo={},hi={},neghi={})", operand.op_sel ? 1u : 0u,
		                    operand.op_sel_hi ? 1u : 0u, operand.negate_hi ? 1u : 0u);
	}
	if (operand.omod != 0) {
		text += fmt::format(".omod({})", operand.omod);
	}
	if (operand.clamp) {
		text += ".clamp";
	}
	if (operand.dpp) {
		text += fmt::format(".dpp(ctrl=0x{:x},fi={},bc={})", operand.dpp_ctrl,
		                    operand.dpp_fetch_inactive ? 1u : 0u, operand.dpp_bound_ctrl ? 1u : 0u);
	}
	return text;
}

std::string ResourceKindToString(ResourceKind kind) {
	switch (kind) {
		case ResourceKind::ScalarBuffer: return "scalar_buffer";
		case ResourceKind::Buffer: return "buffer";
		case ResourceKind::Flat: return "flat";
		case ResourceKind::Global: return "global";
		case ResourceKind::Scratch: return "scratch";
		case ResourceKind::Lds: return "lds";
		case ResourceKind::Gds: return "gds";
		case ResourceKind::Image: return "image";
		case ResourceKind::ImageUint: return "image_uint";
		case ResourceKind::StorageImage: return "storage_image";
		case ResourceKind::StorageImageUint: return "storage_image_uint";
		case ResourceKind::Sampler: return "sampler";
		default: return "none";
	}
}

std::string MemoryInfoToString(const MemoryInfo& mem) {
	if (mem.kind == ResourceKind::None) {
		return "";
	}
	std::string text = fmt::format(
	    " ; {} resource={} sampler={} offset={} offset2={} dmask=0x{:x} data_dwords={} "
	    "data_bits={} component={}/{} dfmt={} nfmt={} signed={} typed={} formatted={} segment={} "
	    "glc={} slc={} idxen={} offen={} image_flags=0x{:x} image_dim={} image_addr={} "
	    "image_mip={}",
	    ResourceKindToString(mem.kind).c_str(), mem.resource, mem.sampler, mem.offset,
	    mem.secondary_offset, mem.dmask, mem.data_dwords, mem.data_bits, mem.component_index,
	    mem.component_count, mem.data_format, mem.number_format, mem.data_signed ? 1u : 0u,
	    mem.typed ? 1u : 0u, mem.formatted ? 1u : 0u, mem.memory_segment, mem.glc ? 1u : 0u,
	    mem.slc ? 1u : 0u, mem.idxen ? 1u : 0u, mem.offen ? 1u : 0u, mem.image_sample_flags,
	    Decoder::ImageDimensionToString(mem.image_dimension), mem.image_address_components,
	    mem.image_has_mip ? 1u : 0u);
	if (mem.image_nsa_dwords != 0) {
		text += fmt::format(" image_nsa_dwords={} image_nsa_addr=", mem.image_nsa_dwords);
		const auto count =
		    std::min<uint32_t>(mem.image_nsa_dwords * 4u, Decoder::MaxImageNsaAddressComponents);
		for (uint32_t i = 0; i < count; i++) {
			if (i != 0) {
				text += ",";
			}
			text += fmt::format("{}", mem.image_nsa_addr[i]);
		}
	}
	return text;
}

std::string ExportInfoToString(const ExportInfo& exp) {
	return fmt::format(" ; target={} index={} raw=0x{:02x} en=0x{:x} done={} compr={} vm={}",
	                   ExportTargetKindToString(exp.kind).c_str(), exp.index, exp.target, exp.en,
	                   exp.done ? 1u : 0u, exp.compr ? 1u : 0u, exp.vm ? 1u : 0u);
}

std::string InputInfoToString(Opcode op, const InputInfo& input) {
	if (op != Opcode::LoadInputF32) {
		return "";
	}
	return fmt::format(" ; input_attr={} input_chan={}", input.attr, input.chan);
}

std::string TerminatorToString(const CFG::Terminator& term) {
	return fmt::format(
	    "term={} cond={} true={} false={} merge={} continue={} loop={} indirect_sgpr={} "
	    "indirect_selector={} indirect_targets={} selector_values={}",
	    static_cast<uint32_t>(term.kind), CFG::BranchConditionToString(term.condition).c_str(),
	    term.true_block, term.false_block, term.merge_block, term.continue_block,
	    term.loop_header ? 1u : 0u, term.indirect_pc_sgpr, term.indirect_selector_code,
	    static_cast<uint32_t>(term.indirect_targets.size()),
	    static_cast<uint32_t>(term.indirect_selector_values.size()));
}

std::string ExportSourcesToString(const Instruction& inst) {
	std::string text;
	for (uint32_t i = 0; i < inst.src_count && i < 4u; i++) {
		text += i == 0 ? " " : ", ";
		text += OperandToString(inst.src[i]);
	}
	return text;
}

std::string InstructionToString(const Instruction& inst) {
	std::string text = fmt::format("0x{:08x}: ", inst.pc);
	if (inst.op == Opcode::Export) {
		text += "Export";
		text += ExportSourcesToString(inst);
		text += ExportInfoToString(inst.export_info);
		return text;
	}
	text += OpcodeName(inst.op);
	text += " ";
	text += OperandToString(inst.dst);
	if (inst.dst2.kind != OperandKind::Null) {
		text += ", ";
		text += OperandToString(inst.dst2);
	}
	for (uint32_t i = 0; i < inst.src_count; i++) {
		text += i == 0 ? ", " : ", ";
		text += OperandToString(inst.src[i]);
	}
	text += MemoryInfoToString(inst.memory);
	text += InputInfoToString(inst.op, inst.input_info);
	return text;
}

std::string ExportTargetKindToString(ExportTargetKind kind) {
	switch (kind) {
		case ExportTargetKind::Null: return "null";
		case ExportTargetKind::Position: return "position";
		case ExportTargetKind::Primitive: return "primitive";
		case ExportTargetKind::Parameter: return "parameter";
		case ExportTargetKind::Mrt: return "mrt";
		case ExportTargetKind::MrtZ: return "mrtz";
		default: return "unknown";
	}
}

std::string ProgramToString(const Program& program) {
	std::string text;
	text += fmt::format("mode={} masks={} fallback={} reason={}\n",
	                    program.dispatcher_fallback ? "dispatcher" : "structured",
	                    program.lane_mask_mode == ShaderLaneMaskMode::NativeWave ? "native-wave"
	                                                                             : "per-invocation",
	                    CFG::FailureKindToString(program.cfg_failure_kind).c_str(),
	                    program.fallback_reason.c_str());
	for (const auto& block: program.blocks) {
		text += fmt::format("block_{} pc=0x{:08x} end=0x{:08x} preds={} succs={} {}\n", block.id,
		                    block.start_pc, block.end_pc,
		                    static_cast<uint32_t>(block.predecessors.size()),
		                    static_cast<uint32_t>(block.successors.size()),
		                    TerminatorToString(block.terminator).c_str());
		for (const auto& inst: block.instructions) {
			text += "  ";
			text += InstructionToString(inst);
			text += "\n";
		}
	}
	return text;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
