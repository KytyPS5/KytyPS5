#include "graphics/shader/shaderMergedGeometry.h"

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <algorithm>
#include <fmt/format.h>

namespace Libs::Graphics {

namespace {

using ShaderRecompiler::Decoder::Opcode;

// The IR layer that used to answer this was removed upstream, but the scan still has to know
// which scalar results claim a register pair.
bool ScalarResultIs64Bit(Opcode opcode) {
	switch (opcode) {
		case Opcode::S_AND_B64:
		case Opcode::S_ANDN2_B64:
		case Opcode::S_NOT_B64:
		case Opcode::S_OR_B64:
		case Opcode::S_ORN2_B64:
		case Opcode::S_XOR_B64:
		case Opcode::S_NAND_B64:
		case Opcode::S_NOR_B64:
		case Opcode::S_XNOR_B64:
		case Opcode::S_LSHL_B64:
		case Opcode::S_LSHR_B64:
		case Opcode::S_BFE_U64:
		case Opcode::S_WQM_B64:
		case Opcode::S_QUADMASK_B64: return true;
		default: return false;
	}
}

bool Fail(std::string* error, const std::string& message) {
	if (error != nullptr) {
		*error = message;
	}
	return false;
}

bool FindGsEnd(std::span<const uint32_t> gs_code, uint32_t& end_word, std::string* error) {
	uint32_t word_index = 0;
	while (word_index < gs_code.size()) {
		ShaderRecompiler::Decoder::Instruction inst {};
		std::string                            decode_error;
		if (!ShaderRecompiler::Decoder::DecodeInstruction(gs_code, word_index, inst,
		                                                 &decode_error)) {
			return Fail(error, fmt::format("GS decode failed at word {}: {}", word_index,
			                               decode_error));
		}
		word_index += std::max(inst.word_count, 1u);
		if (inst.opcode == Opcode::S_ENDPGM) {
			end_word = word_index;
			return true;
		}
	}
	return Fail(error, fmt::format("GS has no s_endpgm in {} words", gs_code.size()));
}

bool FindEsExit(std::span<const uint32_t> es_code, uint32_t& exit_word, std::string* error) {
	uint32_t word_index = 0;
	uint32_t decoded    = 0;
	while (word_index < es_code.size()) {
		ShaderRecompiler::Decoder::Instruction inst {};
		std::string                            decode_error;
		if (!ShaderRecompiler::Decoder::DecodeInstruction(es_code, word_index, inst,
		                                                 &decode_error)) {
			return Fail(error, fmt::format("ES decode failed at word {}: {}", word_index,
			                               decode_error));
		}
		if (inst.opcode == Opcode::S_SETPC_B64) {
			exit_word = word_index;
			return true;
		}
		decoded++;
		word_index += std::max(inst.word_count, 1u);
	}
	return Fail(error, fmt::format("ES has no s_setpc_b64 in {} decoded instructions ({} words): "
	                               "it does not end the way a merged ES+GS pair does",
	                               decoded, es_code.size()));
}

using ShaderRecompiler::Decoder::Operand;
using ShaderRecompiler::Decoder::OperandKind;

} // namespace

std::string MergedGeometryLaunchState::Describe() const {
	std::string text;
	for (uint32_t i = 0; i < ScalarCount; i++) {
		if (scalar[i]) {
			text += fmt::format("{}s{}", text.empty() ? "" : " ", i);
		}
	}
	for (uint32_t i = 0; i < VectorCount; i++) {
		if (vector[i]) {
			text += fmt::format("{}v{}", text.empty() ? "" : " ", i);
		}
	}
	if (reads_m0) {
		text += text.empty() ? "m0" : " m0";
	}
	if (scalar_out_of_range || vector_out_of_range) {
		text += " (registers outside the tracked window were used)";
	}
	return text.empty() ? "none" : text;
}

bool ShaderAnalyzeMergedLaunchState(std::span<const uint32_t> code,
                                    MergedGeometryLaunchState& out, std::string* error) {
	out = {};
	bool written_scalar[MergedGeometryLaunchState::ScalarCount] = {};
	bool written_vector[MergedGeometryLaunchState::VectorCount] = {};

	uint32_t word_index = 0;
	while (word_index < code.size()) {
		ShaderRecompiler::Decoder::Instruction inst {};
		std::string                            decode_error;
		if (!ShaderRecompiler::Decoder::DecodeInstruction(code, word_index, inst, &decode_error)) {
			return Fail(error, fmt::format("launch-state scan failed at word {}: {}", word_index,
			                               decode_error));
		}

		const auto note_read = [&](const Operand& src, uint32_t reg_count) {
			for (uint32_t i = 0; i < reg_count; i++) {
				const auto reg = src.reg + i;
				if (src.kind == OperandKind::Sgpr) {
					if (reg < MergedGeometryLaunchState::ScalarCount) {
						if (!written_scalar[reg]) {
							out.scalar[reg] = true;
						}
					} else {
						out.scalar_out_of_range = true;
					}
				}
				if (src.kind == OperandKind::Vgpr) {
					if (reg < MergedGeometryLaunchState::VectorCount) {
						if (!written_vector[reg]) {
							out.vector[reg] = true;
						}
					} else {
						out.vector_out_of_range = true;
					}
				}
			}
			if (src.kind == OperandKind::M0) {
				out.reads_m0 = true;
			}
		};

		const Operand* sources[] = {&inst.src0, &inst.src1, &inst.src2, &inst.src3};
		for (uint32_t i = 0; i < inst.src_count && i < std::size(sources); i++) {
			note_read(*sources[i], ShaderRecompiler::Decoder::InstructionSourceRegisterCount(inst, i));
		}
		if (ShaderRecompiler::Decoder::InstructionReadsDestination(inst)) {
			note_read(inst.dst, std::max(inst.data_dwords, 1u));
		}

		if (inst.opcode == Opcode::S_ENDPGM) {
			break;
		}

		bool vector_sourced = false;
		for (uint32_t i = 0; i < inst.src_count && i < std::size(sources); i++) {
			vector_sourced = vector_sourced || sources[i]->kind == OperandKind::Vgpr;
		}
		const bool writes_lane_mask = inst.dst.kind == OperandKind::Sgpr && vector_sourced;
		const bool     writes_dst    = ShaderRecompiler::Decoder::InstructionWritesDestination(inst);
		const uint32_t load_dst_regs = std::max(inst.data_dwords, 1u);
		const uint32_t scalar_dst_regs =
		    std::max(load_dst_regs, (ScalarResultIs64Bit(inst.opcode) ||
		                             writes_lane_mask)
		                                ? 2u
		                                : 1u);
		if (writes_dst && inst.dst.kind == OperandKind::Sgpr) {
			for (uint32_t i = 0; i < scalar_dst_regs; i++) {
				const auto reg = inst.dst.reg + i;
				if (reg < MergedGeometryLaunchState::ScalarCount) {
					written_scalar[reg] = true;
				}
			}
		}
		if (writes_dst && inst.dst.kind == OperandKind::Vgpr) {
			for (uint32_t i = 0; i < load_dst_regs; i++) {
				const auto reg = inst.dst.reg + i;
				if (reg < MergedGeometryLaunchState::VectorCount) {
					written_vector[reg] = true;
				}
			}
		}
		if (inst.dst2.kind == OperandKind::Sgpr &&
		    inst.dst2.reg < MergedGeometryLaunchState::ScalarCount) {
			written_scalar[inst.dst2.reg] = true;
		}

		word_index += std::max(inst.word_count, 1u);
	}
	return true;
}

bool ShaderValidateMeshLds(uint32_t lds_dwords, uint32_t max_bytes, std::string* error) {
	const uint64_t bytes = static_cast<uint64_t>(lds_dwords) * sizeof(uint32_t);
	if (max_bytes == 0) {
		return Fail(error, "the host reported no mesh shared memory at all");
	}
	if (bytes > max_bytes) {
		return Fail(error, fmt::format("the merged wave wants {} bytes of LDS but a mesh workgroup "
		                               "may use {}",
		                               bytes, max_bytes));
	}
	return true;
}

bool ShaderComputeMeshDispatch(uint32_t total_primitives, uint32_t vertices_per_primitive,
                               uint32_t output_vertices_per_primitive, uint32_t vertex_group_size,
                               uint32_t primitive_group_size, uint32_t max_output_per_subgroup,
                               MeshDispatch& out, std::string* error) {
	out = {};
	if (vertices_per_primitive == 0) {
		return Fail(error, "a primitive needs at least one vertex");
	}
	if (output_vertices_per_primitive == 0) {
		return Fail(error, "a primitive that emits no vertices cannot be drawn");
	}
	if (vertex_group_size == 0 || primitive_group_size == 0) {
		return Fail(error, "the GE group sizes must both be non-zero");
	}

	if (total_primitives == 0) {
		return true; // Nothing to draw; zero workgroups is the right answer.
	}

	uint32_t primitives = std::min(primitive_group_size, MeshWaveLanes);
	primitives          = std::min(primitives, vertex_group_size / vertices_per_primitive);
	primitives          = std::min(primitives, MeshWaveLanes / vertices_per_primitive);
	if (max_output_per_subgroup != 0) {
		primitives = std::min(primitives, max_output_per_subgroup / output_vertices_per_primitive);
	}
	if (primitives == 0) {
		return Fail(error, fmt::format("no primitive fits a workgroup: vert_group={} prim_group={} "
		                               "max_out={} verts_per_prim={}",
		                               vertex_group_size, primitive_group_size,
		                               max_output_per_subgroup, vertices_per_primitive));
	}

	out.primitives_per_workgroup = primitives;
	out.vertices_per_workgroup   = primitives * vertices_per_primitive;
	out.output_vertices_per_workgroup = primitives * output_vertices_per_primitive;
	out.output_primitives_per_workgroup =
	    primitives * MeshOutputPrimitivesPerPrimitive(output_vertices_per_primitive);
	out.workgroup_count = (total_primitives + primitives - 1) / primitives;

	const uint32_t remainder = total_primitives % primitives;
	out.last_primitives       = remainder == 0 ? primitives : remainder;
	out.last_vertices         = out.last_primitives * vertices_per_primitive;

	if (out.vertices_per_workgroup > 0xFFu || out.primitives_per_workgroup > 0xFFu) {
		return Fail(error, fmt::format("workgroup counts do not fit the merged wave info: {} "
		                               "vertices, {} primitives",
		                               out.vertices_per_workgroup, out.primitives_per_workgroup));
	}
	return true;
}

bool ShaderAssembleMergedGeometry(std::span<const uint32_t> es_code,
                                  std::span<const uint32_t> gs_code,
                                  MergedGeometryProgram& out, std::string* error) {
	if (es_code.empty() || gs_code.empty()) {
		return Fail(error, "merged geometry needs both an ES and a GS program");
	}

	uint32_t exit_word = 0;
	if (!FindEsExit(es_code, exit_word, error)) {
		return false;
	}
	uint32_t gs_end_word = 0;
	if (!FindGsEnd(gs_code, gs_end_word, error)) {
		return false;
	}

	out.code.clear();
	out.code.reserve(exit_word + gs_end_word);
	out.code.insert(out.code.end(), es_code.begin(), es_code.begin() + exit_word);
	out.gs_word_offset = exit_word;
	out.code.insert(out.code.end(), gs_code.begin(), gs_code.begin() + gs_end_word);
	return true;
}

} // namespace Libs::Graphics
