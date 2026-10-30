#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/Tessellation.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/frontend/translate/Translator.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/MaskedSelectElimination.h"
#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler {

namespace {

std::string MakeIrDump(std::string_view cfg, const IR::Program& ir) {
	std::string dump = "CFG:\n";
	dump += cfg;
	dump += "\nIR:\n";
	dump += fmt::format("mode={} scratch_dwords={}\n",
	                    ir.dispatcher_fallback ? "dispatcher" : "structured", ir.scratch_dwords);
	dump += IR::ProgramToString(ir);
	return dump;
}

void LogDispatcherFallback(const CompileOptions& options, const CFG::Graph& cfg, const char* phase) {
	const auto* block        = cfg.FindBlock(cfg.failure_block);
	const auto  start        = block != nullptr ? block->start_pc : UINT32_MAX;
	const auto  end          = block != nullptr ? block->end_pc : UINT32_MAX;
	const auto  predecessors = block != nullptr ? block->predecessors.size() : 0u;
	const auto  successors   = block != nullptr ? block->successors.size() : 0u;
	LOGF("ShaderRecompiler %s CFG dispatcher fallback: hash=0x%016" PRIx64
	     " phase=%s failure=%s block=%" PRIu32 " pc=0x%08" PRIx32 "..0x%08" PRIx32 " preds=%" PRIu64
	     " succs=%" PRIu64 " blocks=%" PRIu64 " loops=%" PRIu64 " back_edges=%" PRIu64
	     " reason=%s\n",
	     StageName(options.stage), options.shader_hash, phase,
	     CFG::FailureKindToString(cfg.failure_kind).c_str(), cfg.failure_block, start, end,
	     static_cast<uint64_t>(predecessors), static_cast<uint64_t>(successors),
	     static_cast<uint64_t>(cfg.blocks.size()), static_cast<uint64_t>(cfg.natural_loops.size()),
	     static_cast<uint64_t>(cfg.back_edges.size()), cfg.unsupported_reason.c_str());
}

enum class EmbeddedFetchValueType {
	Unknown,
	Constant,
	AttribTable,
	Attrib,
	BufferTable,
	Buffer
};

struct EmbeddedFetchSgprInfo {
	EmbeddedFetchValueType type      = EmbeddedFetchValueType::Unknown;
	int                    attrib_id = 0;
	uint32_t               value     = 0;
};

using EmbeddedFetchVectorLanes = std::map<uint64_t, EmbeddedFetchSgprInfo>;

uint64_t EmbeddedFetchVectorLaneKey(uint32_t reg, uint32_t lane) {
	return (static_cast<uint64_t>(reg) << 32u) | lane;
}

uint32_t EmbeddedFetchLane(uint32_t lane, uint32_t wave_size) {
	return wave_size == 32 || wave_size == 64 ? lane % wave_size : lane;
}

void ClearEmbeddedFetchVectorLanes(EmbeddedFetchVectorLanes* lanes, uint32_t reg) {
	const auto first = lanes->lower_bound(EmbeddedFetchVectorLaneKey(reg, 0));
	const auto last  = lanes->lower_bound(EmbeddedFetchVectorLaneKey(reg + 1u, 0));
	lanes->erase(first, last);
}

bool IsDecodedSgpr(const Decoder::Operand& op) {
	return op.kind == Decoder::OperandKind::Sgpr || op.kind == Decoder::OperandKind::VccLo ||
	       op.kind == Decoder::OperandKind::VccHi;
}

uint32_t DecodedSgprReg(const Decoder::Operand& op) {
	switch (op.kind) {
		case Decoder::OperandKind::VccLo: return 106u;
		case Decoder::OperandKind::VccHi: return 107u;
		default: return op.reg;
	}
}

bool IsDecodedVgpr(const Decoder::Operand& op) {
	return op.kind == Decoder::OperandKind::Vgpr;
}

uint32_t DecodedDstSize(const Decoder::Instruction& inst) {
	return std::max(inst.data_dwords, 1u);
}

uint32_t EmbeddedFetchDstSize(const Decoder::Instruction& inst) {
	return inst.opcode == Decoder::Opcode::V_MAD_U64_U32 ? 2u : DecodedDstSize(inst);
}

void ClearEmbeddedFetchSgprs(std::array<EmbeddedFetchSgprInfo, 108>& sgprs,
                             const Decoder::Operand& dst, uint32_t size) {
	if (!IsDecodedSgpr(dst)) {
		return;
	}
	const auto register_id = DecodedSgprReg(dst);
	for (uint32_t i = 0; i < size && register_id + i < sgprs.size(); i++) {
		sgprs[register_id + i] = {};
	}
}

bool TryDecodedOperandConstant(const std::array<EmbeddedFetchSgprInfo, 108>& sgprs,
                               const Decoder::Operand& op, uint32_t& value) {
	switch (op.kind) {
		case Decoder::OperandKind::LiteralConstant:
		case Decoder::OperandKind::IntegerInlineConstant:
		case Decoder::OperandKind::FloatInlineConstant: value = op.value; return true;
		case Decoder::OperandKind::Null: value = 0; return true;
		default: break;
	}
	if (IsDecodedSgpr(op) && DecodedSgprReg(op) < sgprs.size() &&
	    sgprs[DecodedSgprReg(op)].type == EmbeddedFetchValueType::Constant) {
		value = sgprs[DecodedSgprReg(op)].value;
		return true;
	}
	return false;
}

bool TryDecodedSmemOffset(const std::array<EmbeddedFetchSgprInfo, 108>& sgprs,
                          const Decoder::Instruction& inst, uint32_t& raw_offset) {
	uint32_t base = 0;
	if (!TryDecodedOperandConstant(sgprs, inst.src1, base)) {
		return false;
	}
	const auto value = static_cast<uint64_t>(base) + inst.offset;
	if (value > 0xffffffffull) {
		return false;
	}
	raw_offset = static_cast<uint32_t>(value);
	return true;
}

bool IsEmbeddedFetchSLoad(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::S_LOAD_DWORD:
		case Decoder::Opcode::S_LOAD_DWORDX2:
		case Decoder::Opcode::S_LOAD_DWORDX4:
		case Decoder::Opcode::S_LOAD_DWORDX8:
		case Decoder::Opcode::S_LOAD_DWORDX16: return true;
		default: return false;
	}
}

bool IsEmbeddedFetchBufferLoad(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_X:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XY:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZ:
		case Decoder::Opcode::BUFFER_LOAD_FORMAT_XYZW: return true;
		default: return false;
	}
}

bool IsEmbeddedFetchAttribPropagationAlu(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::S_BFE_U32:
		case Decoder::Opcode::S_AND_B32:
		case Decoder::Opcode::S_ADD_I32:
		case Decoder::Opcode::S_ADD_U32:
		case Decoder::Opcode::S_LSHL_B32: return true;
		default: return false;
	}
}

int BufferTableAttribFromOffset(uint32_t raw_offset, int dword) {
	return static_cast<int>((raw_offset + static_cast<uint32_t>(dword) * 4u) / 16u);
}

Frontend::EmbeddedFetchPlan DetectEmbeddedVertexFetch(
    const Decoder::Program& decoded, const ShaderVertexInputInfo& input_info,
    uint32_t user_data_base, uint32_t user_data_count, uint32_t wave_size) {
	const uint32_t    vertex_index_reg   = input_info.logical_stage == ShaderType::Local ? 2u : 5u;
	const uint32_t    instance_index_reg = input_info.logical_stage == ShaderType::Local ? 5u : 8u;
	Frontend::EmbeddedFetchPlan data;
	data.loads.reserve(input_info.resources_num);
	int32_t vertex_offset_candidate   = -1;
	int32_t instance_offset_candidate = -1;
	bool    vertex_offset_conflict    = false;
	bool    instance_offset_conflict  = false;

	const int shift_regs = 8;
	const int attrib_reg = input_info.fetch_attrib_reg + shift_regs;
	const int buffer_reg = input_info.fetch_buffer_reg + shift_regs;

	std::array<EmbeddedFetchSgprInfo, 108> sgprs {};
	std::array<bool, 256>                 vgpr_is_index {};
	EmbeddedFetchVectorLanes               vector_lanes;
	const bool                             track_vector_lanes =
	    std::none_of(decoded.instructions.begin(), decoded.instructions.end(),
	                 [](const auto& inst) {
		                 return Decoder::IsDirectBranch(inst.opcode) ||
		                        inst.opcode == Decoder::Opcode::S_SETPC_B64;
	                 });

	if (attrib_reg >= 0 && attrib_reg < static_cast<int>(sgprs.size())) {
		sgprs[attrib_reg].type = EmbeddedFetchValueType::AttribTable;
	}
	if (attrib_reg + 1 >= 0 && attrib_reg + 1 < static_cast<int>(sgprs.size())) {
		sgprs[attrib_reg + 1].type = EmbeddedFetchValueType::AttribTable;
	}
	if (buffer_reg >= 0 && buffer_reg < static_cast<int>(sgprs.size())) {
		sgprs[buffer_reg].type = EmbeddedFetchValueType::BufferTable;
	}
	if (buffer_reg + 1 >= 0 && buffer_reg + 1 < static_cast<int>(sgprs.size())) {
		sgprs[buffer_reg + 1].type = EmbeddedFetchValueType::BufferTable;
	}

	for (const auto& inst: decoded.instructions) {
		// Fetch shaders accumulate the draw's vertex offset in v0. The PS5 NGG ABI
		// seeds S_NGG_VERTEX_INDEX in v5 and S_NGG_INSTANCE_INDEX in v8, then applies the
		// corresponding direct-draw offsets before fetching.
		const bool vertex_index_accumulator =
		    IsDecodedVgpr(inst.dst) &&
		    (inst.dst.reg == 0 || (user_data_base == 8 && inst.dst.reg == vertex_index_reg));
		const bool instance_index_accumulator =
		    IsDecodedVgpr(inst.dst) &&
		    (inst.dst.reg == (user_data_base == 8 ? instance_index_reg : 3u));
		uint32_t   sad_zero = 0;
		const bool index_offset_add =
		    (vertex_index_accumulator || instance_index_accumulator) && IsDecodedSgpr(inst.src0) &&
		    ((inst.opcode == Decoder::Opcode::V_ADD_I32 && IsDecodedVgpr(inst.src1) &&
		      inst.src1.reg == inst.dst.reg) ||
		     (user_data_base == 8 &&
		      (inst.dst.reg == vertex_index_reg || inst.dst.reg == instance_index_reg) &&
		      inst.opcode == Decoder::Opcode::V_SAD_U32 && IsDecodedVgpr(inst.src2) &&
		      inst.src2.reg == inst.dst.reg &&
		      TryDecodedOperandConstant(sgprs, inst.src1, sad_zero) && sad_zero == 0));
		if (data.loads.empty() && index_offset_add) {
			const auto reg = DecodedSgprReg(inst.src0);
			if (reg >= user_data_base && reg - user_data_base < user_data_count) {
				auto& candidate = vertex_index_accumulator ? vertex_offset_candidate
				                                           : instance_offset_candidate;
				auto& conflict  = vertex_index_accumulator ? vertex_offset_conflict
				                                           : instance_offset_conflict;
				if (candidate >= 0 && candidate != static_cast<int32_t>(reg)) {
					conflict = true;
				} else {
					candidate = static_cast<int32_t>(reg);
				}
			}
		}
		switch (inst.opcode) {
			case Decoder::Opcode::V_WRITELANE_B32: {
				uint32_t lane = 0;
				if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgpr_is_index.size()) {
					vgpr_is_index[inst.dst.reg] = false;
				}
				if (track_vector_lanes && IsDecodedVgpr(inst.dst) && IsDecodedSgpr(inst.src0) &&
				    DecodedSgprReg(inst.src0) < sgprs.size() &&
				    TryDecodedOperandConstant(sgprs, inst.src1, lane)) {
					vector_lanes[EmbeddedFetchVectorLaneKey(inst.dst.reg,
					                                        EmbeddedFetchLane(lane, wave_size))] =
					    sgprs[DecodedSgprReg(inst.src0)];
				} else if (IsDecodedVgpr(inst.dst)) {
					ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg);
				}
				break;
			}
			case Decoder::Opcode::V_READLANE_B32: {
				uint32_t lane = 0;
				if (track_vector_lanes && IsDecodedSgpr(inst.dst) &&
				    DecodedSgprReg(inst.dst) < sgprs.size() && IsDecodedVgpr(inst.src0) &&
				    TryDecodedOperandConstant(sgprs, inst.src1, lane)) {
					const auto found = vector_lanes.find(EmbeddedFetchVectorLaneKey(
					    inst.src0.reg, EmbeddedFetchLane(lane, wave_size)));
					sgprs[DecodedSgprReg(inst.dst)] =
					    found != vector_lanes.end() ? found->second : EmbeddedFetchSgprInfo {};
				} else if (IsDecodedSgpr(inst.dst)) {
					ClearEmbeddedFetchSgprs(sgprs, inst.dst, 1);
				}
				break;
			}
			case Decoder::Opcode::S_MOV_B32:
				if (IsDecodedSgpr(inst.dst) && IsDecodedSgpr(inst.src0) &&
				    DecodedSgprReg(inst.src0) < sgprs.size()) {
					sgprs[DecodedSgprReg(inst.dst)] = sgprs[DecodedSgprReg(inst.src0)];
				} else if (IsDecodedSgpr(inst.dst)) {
					uint32_t value = 0;
					if (TryDecodedOperandConstant(sgprs, inst.src0, value)) {
						auto& dst = sgprs[DecodedSgprReg(inst.dst)];
						dst.type  = EmbeddedFetchValueType::Constant;
						dst.value = value;
					} else {
						ClearEmbeddedFetchSgprs(sgprs, inst.dst, 1);
					}
				}
				break;
			case Decoder::Opcode::S_MOVK_I32:
				if (IsDecodedSgpr(inst.dst)) {
					auto& dst = sgprs[DecodedSgprReg(inst.dst)];
					dst.type  = EmbeddedFetchValueType::Constant;
					dst.value = inst.src0.value;
				}
				break;
			default:
				if (IsEmbeddedFetchSLoad(inst)) {
					if (IsDecodedSgpr(inst.src0) && DecodedSgprReg(inst.src0) < sgprs.size() &&
					    sgprs[DecodedSgprReg(inst.src0)].type ==
					        EmbeddedFetchValueType::AttribTable) {
						uint32_t raw_offset = 0;
						if (TryDecodedSmemOffset(sgprs, inst, raw_offset)) {
							const auto register_id = DecodedSgprReg(inst.dst);
							const int  index       = static_cast<int>(raw_offset / 4u);
							for (uint32_t i = 0;
							     i < DecodedDstSize(inst) && register_id + i < sgprs.size(); i++) {
								auto& dst        = sgprs[register_id + i];
								dst.type         = EmbeddedFetchValueType::Attrib;
								dst.attrib_id    = index + static_cast<int>(i);
							}
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
						}
					} else if (IsDecodedSgpr(inst.src0) &&
					           DecodedSgprReg(inst.src0) < sgprs.size() &&
					           sgprs[DecodedSgprReg(inst.src0)].type ==
					               EmbeddedFetchValueType::BufferTable) {
						const auto register_id = DecodedSgprReg(inst.dst);
						uint32_t   raw_offset  = 0;
						if (TryDecodedSmemOffset(sgprs, inst, raw_offset)) {
							for (uint32_t i = 0;
							     i < DecodedDstSize(inst) && register_id + i < sgprs.size(); i++) {
								auto& dst = sgprs[register_id + i];
								dst.type  = EmbeddedFetchValueType::Buffer;
								dst.attrib_id =
								    BufferTableAttribFromOffset(raw_offset, static_cast<int>(i));
							}
						} else if (IsDecodedSgpr(inst.src1) &&
						           DecodedSgprReg(inst.src1) < sgprs.size() &&
						           sgprs[DecodedSgprReg(inst.src1)].type ==
						               EmbeddedFetchValueType::Attrib &&
						           (inst.offset & 0x3u) == 0) {
							for (uint32_t i = 0;
							     i < DecodedDstSize(inst) && register_id + i < sgprs.size(); i++) {
								auto& dst        = sgprs[register_id + i];
								dst.type         = EmbeddedFetchValueType::Buffer;
								dst.attrib_id    = sgprs[DecodedSgprReg(inst.src1)].attrib_id;
							}
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
						}
					} else {
						ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
					}
				} else if (inst.opcode == Decoder::Opcode::V_CNDMASK_B32) {
					if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgpr_is_index.size()) {
						ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg);
					}
					if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgpr_is_index.size() &&
					    IsDecodedVgpr(inst.src0) && inst.src0.reg == instance_index_reg &&
					    IsDecodedVgpr(inst.src1) && inst.src1.reg == vertex_index_reg) {
						vgpr_is_index[inst.dst.reg] = true;
					}
				} else if (IsEmbeddedFetchAttribPropagationAlu(inst)) {
					if (IsDecodedSgpr(inst.dst) && IsDecodedSgpr(inst.src0) &&
					    DecodedSgprReg(inst.src0) < sgprs.size() &&
					    sgprs[DecodedSgprReg(inst.src0)].type == EmbeddedFetchValueType::Attrib) {
						sgprs[DecodedSgprReg(inst.dst)] = sgprs[DecodedSgprReg(inst.src0)];
					} else if (IsDecodedSgpr(inst.dst)) {
						uint32_t src0 = 0;
						uint32_t src1 = 0;
						if (TryDecodedOperandConstant(sgprs, inst.src0, src0) &&
						    TryDecodedOperandConstant(sgprs, inst.src1, src1)) {
							auto& dst = sgprs[DecodedSgprReg(inst.dst)];
							dst.type  = EmbeddedFetchValueType::Constant;
							switch (inst.opcode) {
								case Decoder::Opcode::S_AND_B32: dst.value = src0 & src1; break;
								case Decoder::Opcode::S_LSHL_B32:
									dst.value = src0 << (src1 & 31u);
									break;
								case Decoder::Opcode::S_BFE_U32:
									dst.value = src0 >> (src1 & 31u);
									break;
								default: dst.value = src0 + src1; break;
							}
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, 1);
						}
					}
				} else if (IsEmbeddedFetchBufferLoad(inst)) {
					if (IsDecodedVgpr(inst.src0) && inst.src0.reg < vgpr_is_index.size() &&
					    vgpr_is_index[inst.src0.reg] &&
					    IsDecodedSgpr(inst.src1) && DecodedSgprReg(inst.src1) < sgprs.size() &&
					    sgprs[DecodedSgprReg(inst.src1)].type == EmbeddedFetchValueType::Buffer) {
						const auto& buffer = sgprs[DecodedSgprReg(inst.src1)];
						if (data.loads.empty()) {
							if (!vertex_offset_conflict) {
								data.vertex_offset_sgpr = vertex_offset_candidate;
							}
							if (!instance_offset_conflict) {
								data.instance_offset_sgpr = instance_offset_candidate;
							}
						}
						auto& load        = data.loads.emplace_back();
						load.pc           = inst.pc;
						load.attrib_id    = buffer.attrib_id;
						load.components   = DecodedDstSize(inst);
					}
				}
				break;
		}
		if (inst.opcode == Decoder::Opcode::V_MOVRELD_B32) {
			vector_lanes.clear();
		} else if (inst.opcode != Decoder::Opcode::V_WRITELANE_B32 && IsDecodedVgpr(inst.dst)) {
			for (uint32_t i = 0;
			     i < EmbeddedFetchDstSize(inst) && inst.dst.reg + i < vgpr_is_index.size();
			     i++) {
				ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg + i);
			}
		}
	}

	return data;
}

Decoder::Program DecodeFusedProgram(std::span<const uint32_t> front, std::span<const uint32_t> back,
                                    std::vector<uint32_t>& joined_code) {
	EXIT_IF(back.empty());
	auto       result      = Decoder::DecodeFrontProgram(front);
	const auto front_words = static_cast<uint32_t>(result.code.size());
	joined_code.assign(result.code.begin(), result.code.end());
	joined_code.insert(joined_code.end(), back.begin(), back.end());
	// The merged-stage ABI passes the back shader in s[6:7]. Give that handoff an
	// ordinary CFG edge, retaining both bodies in one register and LDS lifetime.
	joined_code[front_words - 1u] = 0xbf820000u; // s_branch to the following instruction
	result.instructions.back()    = {};
	Decoder::DecodeInstruction(joined_code, front_words - 1u, result.instructions.back());
	Decoder::Program back_program;
	Decoder::DecodeProgram(back, back_program);
	result.has_swap_pc |= back_program.has_swap_pc;
	const auto back_pc = front_words * sizeof(uint32_t);
	for (auto& inst: back_program.instructions) {
		// A back-stage PC-relative data reference requires its guest code address.
		EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::S_GETPC_B64);
		inst.pc += back_pc;
		inst.branch_target += back_pc;
		result.instructions.push_back(std::move(inst));
	}
	result.code = joined_code;
	return result;
}

bool ScalarCode(const Decoder::Operand& operand, uint32_t& code) {
	switch (operand.kind) {
		case Decoder::OperandKind::Sgpr: code = operand.reg; return true;
		case Decoder::OperandKind::VccLo: code = 106u; return true;
		case Decoder::OperandKind::VccHi: code = 107u; return true;
		case Decoder::OperandKind::M0: code = 124u; return true;
		default: return false;
	}
}

bool ConstantOperand(const Decoder::Operand& operand, uint32_t& value) {
	if (operand.kind != Decoder::OperandKind::IntegerInlineConstant &&
	    operand.kind != Decoder::OperandKind::LiteralConstant)
		return false;
	value = operand.value;
	return true;
}

// The call's target comes from a table when its two halves are adjacent dwords of one scalar
// buffer load whose offset is a register: the entry each call picks.
std::optional<ShaderSource::Call::Table> FindCallTable(const Decoder::Program& program,
                                                       const CFG::Graph& graph, uint32_t call_index,
                                                       uint32_t wave_size) {
	using Decoder::Opcode;
	const auto& call      = program.instructions[call_index];
	const auto  source_of = [&](uint32_t before, uint32_t code) -> std::optional<std::pair<uint32_t, uint32_t>> {
		for (uint32_t depth = 0; depth < 16u; ++depth) {
			const auto found = CFG::TryFindScalarDefinition(program, graph, before, code, nullptr, wave_size);
			if (!found.has_value() || *found == UINT32_MAX) return std::nullopt;
			const auto& inst = program.instructions[*found];
			if ((inst.opcode == Opcode::S_MOV_B32 || inst.opcode == Opcode::S_MOV_B64) &&
			    inst.src0.kind == Decoder::OperandKind::Sgpr && inst.dst.kind == Decoder::OperandKind::Sgpr) {
				code   = inst.src0.reg + (code - inst.dst.reg);
				before = *found;
				continue;
			}
			return std::pair {*found, code};
		}
		return std::nullopt;
	};
	const auto low  = source_of(call_index, call.src0.reg);
	const auto high = source_of(call_index, call.src0.reg + 1u);
	if (!low || !high || low->first != high->first) return std::nullopt;
	const auto& load = program.instructions[low->first];
	if (load.opcode != Opcode::S_BUFFER_LOAD_DWORDX2 && load.opcode != Opcode::S_BUFFER_LOAD_DWORDX4 &&
	    load.opcode != Opcode::S_BUFFER_LOAD_DWORDX8 && load.opcode != Opcode::S_BUFFER_LOAD_DWORDX16)
		return std::nullopt;
	if (load.dst.kind != Decoder::OperandKind::Sgpr || low->second < load.dst.reg ||
	    high->second != low->second + 1u || high->second >= load.dst.reg + load.data_dwords)
		return std::nullopt;
	uint32_t index_code = 0;
	if (!ScalarCode(load.src1, index_code)) return std::nullopt;
	ShaderSource::Call::Table table {.load   = low->first,
	                                 .dword  = low->second - load.dst.reg,
	                                 .stride = load.data_dwords * 4u,
	                                 .offset = static_cast<int32_t>(load.offset)};
	// The entry's index is scaled to its offset: that scale is the stride of the table.
	if (const auto scale = CFG::TryFindScalarDefinition(program, graph, low->first, index_code, nullptr, wave_size);
	    scale.has_value() && *scale != UINT32_MAX) {
		const auto& inst  = program.instructions[*scale];
		uint32_t    value = 0;
		if (inst.opcode == Opcode::S_LSHL_B32 && ConstantOperand(inst.src1, value) && value < 16u) {
			table.stride = 1u << value;
		} else if (inst.opcode == Opcode::S_MUL_I32 && ConstantOperand(inst.src1, value) && value != 0u &&
		           value <= 0x10000u) {
			table.stride = value;
		}
	}
	return table;
}

void PrepareCallTargets(ShaderSource& source, const CompileOptions& options) {
	using namespace IR;
	EXIT_NOT_IMPLEMENTED(options.stage != ShaderType::Compute);
	source.wave_size = options.wave_size;
	for (uint32_t i = 0; i < source.decoded.instructions.size(); ++i) {
		if (source.decoded.instructions[i].opcode != Decoder::Opcode::S_SWAPPC_B64) continue;
		source.calls.push_back({.instruction = i});
	}
	const auto graph = CFG::BuildGraph(source.decoded);
	Program query;
	query.stage = options.stage;
	query.user_data_base = options.user_data_base;
	query.user_data_count = static_cast<uint32_t>(options.user_data.size());
	query.wave_size = options.wave_size;
	auto& block = *query.block_storage.emplace_back(std::make_unique<Block>());
	query.blocks.push_back(&block);
	struct Producer { bool ready = false; std::map<uint32_t, Value> values; };
	std::map<uint32_t, Producer> producers;
	std::function<Value(uint32_t, uint32_t)> Register;
	std::function<Value(uint32_t, Value)> Resolve = [&](uint32_t before, Value value) -> Value {
		value = value.Resolve();
		auto* inst = value.TryInstruction();
		if (inst == nullptr) return value;
		uint32_t code = UINT32_MAX;
		switch (inst->GetOpcode()) {
			case ValueOpcode::GetScalarRegister: code = static_cast<uint32_t>(inst->Arg(0).ScalarRegister()); break;
			case ValueOpcode::GetVccLo: code = 106u; break;
			case ValueOpcode::GetVccHi: code = 107u; break;
			default: break;
		}
		if (code != UINT32_MAX) {
			const auto result = Register(before, code);
			inst->ReplaceUsesWith(result);
			return result;
		}
		for (size_t i = 0; i < inst->NumArgs(); ++i) inst->SetArg(i, Resolve(before, inst->Arg(i)));
		return value;
	};
	Register = [&](uint32_t before, uint32_t code) -> Value {
		std::vector<uint32_t> crossed_calls;
		const auto index = CFG::FindScalarDefinition(source.decoded, graph, before, code, crossed_calls, options.wave_size);
		for (const auto crossed: crossed_calls) {
			std::ranges::find(source.calls, crossed, &ShaderSource::Call::instruction)->preserved_registers.set(code);
		}
		if (index == UINT32_MAX) {
			EXIT_IF(code < query.user_data_base || code - query.user_data_base >= query.user_data_count);
			return IREmitter(&block).GetUserData(static_cast<ScalarReg>(code));
		}
		auto [entry, inserted] = producers.try_emplace(index);
		auto& producer = entry->second;
		if (inserted) {
			Block translated;
			Frontend::Translator translator(query, &translated, 1u,
			    (options.input_info.compute->float_mode & 0x10u) == 0u,
			    !options.input_info.compute->async_compute);
			translator.TranslateInstruction(source.decoded.instructions[index]);
			for (const auto& inst: translated) {
				uint32_t destination = UINT32_MAX;
				Value value;
				switch (inst.GetOpcode()) {
					case ValueOpcode::SetScalarRegister:
						destination = static_cast<uint32_t>(inst.Arg(0).ScalarRegister()); value = inst.Arg(1); break;
					case ValueOpcode::SetVccLo: destination = 106u; value = inst.Arg(0); break;
					case ValueOpcode::SetVccHi: destination = 107u; value = inst.Arg(0); break;
					default: break;
				}
				if (destination != UINT32_MAX) producer.values.insert_or_assign(destination, value);
			}
			for (auto& [destination, value]: producer.values) value = Resolve(index, value);
			for (auto& inst: translated) inst.SetParent(&block);
			block.Instructions().splice(block.end(), translated.Instructions());
			producer.ready = true;
		}
		EXIT_IF(!producer.ready || !producer.values.contains(code));
		return producer.values.at(code).Resolve();
	};
	const auto root_of = [&](uint32_t before, uint32_t code) {
		const auto value = Register(before, code);
		EXIT_IF(!ValidateRuntimeValue(query, value, RuntimeValueType::Integer));
		auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
		root.SetArg(0, value);
		return Value(&root);
	};
	for (auto& site: source.calls) {
		const auto& call = source.decoded.instructions[site.instruction];
		EXIT_NOT_IMPLEMENTED(call.src0.kind != Decoder::OperandKind::Sgpr ||
		                     call.dst.kind != Decoder::OperandKind::Sgpr ||
		                     call.src0.reg != call.dst.reg || call.dst.reg >= 105u);
		// A target from a table of functions is a buffer read: before the check below, which
		// gives up on calls whose target the scalar walk cannot follow into a buffer.
		site.table = FindCallTable(source.decoded, graph, site.instruction, options.wave_size);
		if (auto& table = site.table) {
			EXIT_NOT_IMPLEMENTED(source.calls.size() != 1u);
			const auto& load = source.decoded.instructions[table->load];
			for (uint32_t i = 0; i < 4u; ++i) table->descriptor[i] = root_of(table->load, load.src0.reg + i);
			continue;
		}
		for (uint32_t i = 0; i < 2u; ++i) {
			const auto value = Register(site.instruction, call.src0.reg + i);
			if (!ValidateRuntimeValue(query, value, RuntimeValueType::Integer)) {
				EXIT_IF(value.Instruction()->GetOpcode() != ValueOpcode::ReadConstBuffer);
				source.unsupported_call_pc = call.pc;
				source.calls.clear();
				return;
			}
			auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
			root.SetArg(0, value);
			site.target[i] = Value(&root);
		}
	}
	RewriteToSsa(query.blocks);
	ConstantPropagationPass(query.blocks, query.wave_size);
	RemoveIdentities(query);
	EliminateDeadCode(query.blocks);
	for (auto& site: source.calls) {
		if (auto& table = site.table) {
			for (auto& value: table->descriptor) value = value.Instruction()->Arg(0).Resolve();
		} else {
			for (auto& value: site.target) value = value.Instruction()->Arg(0).Resolve();
		}
	}
	query.value_storage.clear();
	for (auto& inst: block) {
		inst.SetParent(nullptr);
		if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 || inst.GetOpcode() == ValueOpcode::ReadConstBuffer)
			inst.SetFlags(MemoryFlags {.index = inst.Flags<MemoryFlags>().index});
	}
	query.value_storage.splice(query.value_storage.end(), block.Instructions());
	source.call_targets = std::move(static_cast<ResourcePlan&>(query));
	std::vector<Value> targets;
	for (const auto& site: source.calls) {
		if (site.table) {
			targets.insert(targets.end(), site.table->descriptor.begin(), site.table->descriptor.end());
		} else {
			targets.insert(targets.end(), site.target.begin(), site.target.end());
		}
	}
	CompileSrtWalker(source.call_targets, targets);
}

// A call inside a loop takes its target from data that changes per iteration. A waterfall over a
// table of functions (as ray tracing calls its any-hit and intersection shaders) links them all;
// any other such call has no single target to link, so it is skipped: it still writes its return
// address, as S_GETPC_B64 does, and the loop sees the registers as they were before the call.
bool SkipCallsInLoops(Decoder::Program& program, uint64_t shader_hash, uint32_t wave_size) {
	const auto graph = CFG::BuildGraph(program);
	bool       skipped_all = true;
	for (uint32_t i = 0; i < program.instructions.size(); ++i) {
		auto& inst = program.instructions[i];
		if (inst.opcode != Decoder::Opcode::S_SWAPPC_B64) continue;
		const auto home = std::ranges::find_if(graph.blocks, [i](const auto& block) {
			return block.inst_begin <= i && i < block.inst_end;
		});
		EXIT_IF(home == graph.blocks.end());
		std::vector<bool>     seen(graph.blocks.size(), false);
		std::vector<uint32_t> pending(home->successors.begin(), home->successors.end());
		bool                  in_loop = false;
		while (!pending.empty() && !in_loop) {
			const auto* block = graph.FindBlock(pending.back());
			pending.pop_back();
			EXIT_IF(block == nullptr);
			const auto index = static_cast<size_t>(block - graph.blocks.data());
			if (seen[index]) continue;
			seen[index] = true;
			in_loop     = block->id == home->id;
			pending.insert(pending.end(), block->successors.begin(), block->successors.end());
		}
		if (!in_loop || FindCallTable(program, graph, i, wave_size).has_value()) {
			skipped_all = false;
			continue;
		}
		LOGF("Shader %016" PRIx64 ": the call at pc 0x%08x is in a loop; it is skipped\n", shader_hash,
		     inst.pc);
		inst.opcode = Decoder::Opcode::S_GETPC_B64;
	}
	return skipped_all;
}

} // namespace

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Compute: return "CS";
		case ShaderType::Vertex: return "VS";
		case ShaderType::Local: return "LS";
		case ShaderType::TessellationControl: return "HS";
		case ShaderType::TessellationEvaluation: return "TES";
		case ShaderType::Mesh: return "MS";
		case ShaderType::Pixel: return "PS";
		default: return "unknown";
	}
}

ShaderSource PrepareShaderSource(std::span<const uint32_t> code, const CompileOptions& options) {
	EXIT_IF(code.empty());
	ShaderSource source;
	source.wave_size = options.wave_size;
	if (!options.back_code.empty()) {
		source.decoded = DecodeFusedProgram(code, options.back_code, source.code);
	} else if (options.stage == ShaderType::Local) {
		source.decoded = Decoder::DecodeFrontProgram(code);
		auto& handoff = source.decoded.instructions.back();
		handoff.opcode = Decoder::Opcode::S_ENDPGM;
		handoff.src_count = 0;
	} else {
		Decoder::DecodeProgram(code, source.decoded);
	}
	if (source.decoded.has_swap_pc && SkipCallsInLoops(source.decoded, options.shader_hash, options.wave_size)) {
		source.decoded.has_swap_pc = false;
	}
	if (source.decoded.has_swap_pc) {
		PrepareCallTargets(source, options);
		source.code.assign(code.begin(), code.end());
		source.decoded.code = source.code;
	}
	return source;
}

namespace {

// Reads a function the table points to, up to the return that ends it: past every branch it
// makes. Nothing when it is not code a linked call can run.
bool ReadTableFunction(const IR::SrtRuntime& clean, uint64_t address, const Decoder::Instruction& call,
                       uint32_t descriptor_code, uint32_t wave_size,
                       std::vector<Decoder::Instruction>& function, std::vector<uint32_t>& words) {
	constexpr uint32_t MaxWords = 64u * 1024u;
	constexpr uint32_t Chunk    = 256u;
	function.clear();
	words.clear();
	const auto have = [&](uint32_t count) {
		while (words.size() < count) {
			const auto at = static_cast<uint32_t>(words.size());
			if (at >= MaxWords) return false;
			std::array<uint32_t, Chunk> chunk {};
			uint32_t got = Chunk;
			// The function can end close to unmapped memory: fall back to what it needs.
			while (!clean.read_specialization_memory(clean.userdata, address + uint64_t {at} * 4u, std::span(chunk).first(got))) {
				if (got <= count - at) return false;
				got = count - at;
			}
			words.insert(words.end(), chunk.begin(), chunk.begin() + got);
		}
		return true;
	};
	uint32_t index = 0;
	uint32_t reach = 0;
	for (;;) {
		if (!have(index + 1u)) return false;
		if (Decoder::GetInstructionFamily(words[index]) == Decoder::Family::Unknown) return false;
		if (!have(index + Decoder::MaxInstructionRawWords) && !have(index + 2u) && !have(index + 1u))
			return false;
		Decoder::Instruction inst;
		const auto window = std::span<const uint32_t>(words).first(
		    std::min<size_t>(words.size(), index + Decoder::MaxInstructionRawWords));
		Decoder::DecodeInstruction(window, index, inst);
		if (inst.opcode == Decoder::Opcode::UNSUPPORTED || inst.opcode == Decoder::Opcode::UNKNOWN ||
		    inst.opcode == Decoder::Opcode::S_SWAPPC_B64 || inst.opcode == Decoder::Opcode::S_GETPC_B64)
			return false;
		for (uint32_t i = 0; i < 4u; ++i) {
			if (CFG::MayWriteScalarRegister(inst, descriptor_code + i, wave_size)) return false;
		}
		const bool returns = inst.opcode == Decoder::Opcode::S_SETPC_B64;
		if (returns && (inst.src0.kind != Decoder::OperandKind::Sgpr || inst.src0.reg != call.dst.reg))
			return false;
		index += inst.word_count;
		if (Decoder::IsDirectBranch(inst.opcode)) {
			if (inst.branch_target % 4u != 0u || inst.branch_target / 4u >= MaxWords) return false;
			reach = std::max(reach, inst.branch_target / 4u);
		}
		const bool ends = returns || inst.opcode == Decoder::Opcode::S_ENDPGM ||
		                  inst.opcode == Decoder::Opcode::S_BRANCH;
		function.push_back(std::move(inst));
		if (ends && index > reach) break;
	}
	words.resize(index);
	// Every branch lands on an instruction of the function.
	for (const auto& inst: function) {
		if (!Decoder::IsDirectBranch(inst.opcode)) continue;
		if (std::ranges::find(function, inst.branch_target, &Decoder::Instruction::pc) == function.end())
			return false;
	}
	return true;
}

Decoder::Instruction EncodeLinked(std::initializer_list<uint32_t> raw, uint32_t pc) {
	std::array<uint32_t, Decoder::MaxInstructionRawWords> words {};
	std::ranges::copy(raw, words.begin());
	Decoder::Instruction inst;
	Decoder::DecodeInstruction(std::span(words).first(raw.size()), 0u, inst);
	inst.pc = pc;
	return inst;
}

// Links every function of the table, and replaces the call with a chain that compares the target
// with each entry's and calls its function. A target that is not in the table calls nothing.
// The walk of the program's call targets (RefreshShaderSource) holds the table's descriptor.
const Decoder::Program& LinkCallTable(ShaderSource& source, const IR::SrtRuntime& clean) {
	auto&       reads = source.reads;
	const auto& site  = source.calls[0];
	const auto& table = *site.table;
	const auto& call  = source.decoded.instructions[site.instruction];
	const auto& load  = source.decoded.instructions[table.load];
	std::array<uint32_t, 4> descriptor {};
	for (uint32_t i = 0; i < 4u; ++i) {
		if (!IR::WalkedValue(source.call_targets, table.descriptor[i], descriptor[i]))
			EXIT("shader call at pc 0x%08x has an unavailable table", call.pc);
	}
	const uint64_t base        = (uint64_t {descriptor[1] & 0xffffu} << 32u) | descriptor[0];
	// With a stride, the descriptor counts records of that many bytes; without one, bytes.
	const uint64_t stride_field = (descriptor[1] >> 16u) & 0x3fffu;
	const uint64_t records      = stride_field != 0u ? uint64_t {descriptor[2]} * stride_field : descriptor[2];
	const auto     query_reads = reads.size();
	constexpr uint32_t MaxEntries = 4096u;
	// The table's words, from its first entry's offset to its end.
	const uint64_t first = static_cast<uint64_t>(std::max(table.offset, 0));
	const uint64_t last  = std::min(records, first + uint64_t {MaxEntries} * table.stride);
	std::vector<uint32_t> entries(last > first ? (last - first) / 4u : 0u);
	if (base == 0u || entries.empty() ||
	    !clean.read_specialization_memory(clean.userdata, base + first, std::span(entries)))
		entries.clear();
	// The entries' targets, as the call compares them.
	std::vector<std::pair<uint32_t, uint32_t>> targets;
	for (uint64_t at = table.dword * 4u; at + 8u <= entries.size() * 4u; at += table.stride) {
		const std::pair target {entries[at / 4u], entries[at / 4u + 1u]};
		if ((target.first & ~3u) == 0u && target.second == 0u) continue;
		if (std::ranges::find(targets, target) == targets.end()) targets.push_back(target);
	}
	std::ranges::sort(targets);
	const auto read_table = [&] {
		reads.resize(query_reads);
		if (!entries.empty()) reads.emplace_back(base + first, entries.size() * 4u);
	};

	// A linking holds if the targets are those of the table and every function still reads the same.
	const auto holds = [&](const ShaderSource::Linked& linked) {
		if (linked.table_targets != targets) return false;
		std::vector<uint32_t> observed;
		for (const auto& [low, high]: targets) observed.insert(observed.end(), {low, high});
		for (const auto& [address, count]: linked.observed_ranges) {
			const auto at = observed.size();
			observed.resize(at + count);
			if (!clean.read_specialization_memory(clean.userdata, address, std::span(observed).subspan(at))) return false;
		}
		return observed == linked.observed_function;
	};
	const auto read_linked = [&](const ShaderSource::Linked& linked) {
		read_table();
		for (const auto& [address, count]: linked.observed_ranges) reads.emplace_back(address, uint64_t {count} * 4u);
	};
	if (source.linked && holds(*source.linked)) {
		read_linked(*source.linked);
		return source.linked->decoded;
	}
	for (auto& parked: source.parked_links) {
		if (!holds(parked)) continue;
		std::swap(*source.linked, parked);
		++source.revision;
		read_linked(*source.linked);
		return source.linked->decoded;
	}

	// Read every function first: the chain before them needs their count.
	std::vector<uint32_t> observed;
	for (const auto& [low, high]: targets) observed.insert(observed.end(), {low, high});
	std::vector<std::pair<uint64_t, uint32_t>> ranges;
	struct Function {
		uint64_t                          address = 0;
		std::vector<Decoder::Instruction> code;
		std::vector<uint32_t>             words;
		uint32_t                          pc = 0;
	};
	std::vector<Function> functions;
	uint32_t failed = 0;
	for (const auto& target: targets) {
		const auto address = ((uint64_t {target.second} << 32u) | target.first) & ~uint64_t {3};
		if (std::ranges::find(functions, address, &Function::address) != functions.end()) continue;
		Function function {.address = address};
		if (!ReadTableFunction(clean, address, call, load.src0.reg, source.wave_size, function.code, function.words)) {
			++failed;
			continue;
		}
		ranges.emplace_back(address, static_cast<uint32_t>(function.words.size()));
		observed.insert(observed.end(), function.words.begin(), function.words.end());
		functions.push_back(std::move(function));
	}
	size_t cases = 0;
	for (const auto& target: targets) {
		const auto address = ((uint64_t {target.second} << 32u) | target.first) & ~uint64_t {3};
		cases += std::ranges::find(functions, address, &Function::address) != functions.end() ? 1u : 0u;
	}
	// The linked code goes right after the call, so that the program keeps its shape: the chain
	// and the functions only branch forward to the code after the call. The code after the call
	// moves; a program that reads its own PC cannot move, and gets the linked code at its end.
	const auto& main = source.decoded.instructions;
	EXIT_NOT_IMPLEMENTED(main.back().opcode != Decoder::Opcode::S_ENDPGM);
	const bool inline_code = std::ranges::none_of(main, [](const auto& inst) {
		return inst.opcode == Decoder::Opcode::S_GETPC_B64 || inst.opcode == Decoder::Opcode::S_SETPC_B64;
	});
	constexpr uint32_t CaseBytes = (2u + 1u + 2u + 1u + 1u) * sizeof(uint32_t);
	uint32_t region_bytes = static_cast<uint32_t>(cases) * CaseBytes + call.word_count * sizeof(uint32_t);
	for (const auto& function: functions) region_bytes += static_cast<uint32_t>(function.words.size() * 4u);
	const auto call_end = call.pc + call.word_count * sizeof(uint32_t);
	const auto region   = inline_code ? call_end : main.back().pc + main.back().word_count * 4u;
	const auto shift    = inline_code ? region_bytes : 0u;
	const auto continuation = call_end + shift;
	{
		auto pc = region + static_cast<uint32_t>(cases) * CaseBytes + call.word_count * 4u;
		for (auto& function: functions) {
			function.pc = pc;
			pc += static_cast<uint32_t>(function.words.size() * 4u);
		}
	}
	std::vector<Decoder::Instruction> linked_code;
	std::vector<uint32_t>             linked_words;
	auto next_pc = region;
	const auto add = [&](Decoder::Instruction inst) {
		linked_words.insert(linked_words.end(), inst.raw, inst.raw + inst.word_count);
		next_pc += inst.word_count * sizeof(uint32_t);
		linked_code.push_back(std::move(inst));
	};
	const auto call_to = [&](uint32_t target_pc) {
		auto inst          = call;
		inst.pc            = next_pc;
		inst.return_pc     = call_end;
		inst.branch_target = target_pc;
		add(std::move(inst));
	};
	// The chain: per entry, compare both halves of the target and call its function.
	for (const auto& target: targets) {
		const auto address = ((uint64_t {target.second} << 32u) | target.first) & ~uint64_t {3};
		const auto found = std::ranges::find(functions, address, &Function::address);
		if (found == functions.end()) continue;
		const auto skip = next_pc + CaseBytes;
		for (uint32_t half = 0; half < 2u; ++half) {
			// s_cmp_eq_u32 sN, literal; s_cbranch_scc0 to the next entry.
			add(EncodeLinked({0xbf06ff00u | (call.src0.reg + half), half == 0u ? target.first : target.second},
			                 next_pc));
			const auto offset = static_cast<uint16_t>((static_cast<int64_t>(skip) - (next_pc + 4)) / 4);
			auto branch = EncodeLinked({0xbf840000u | offset}, next_pc);
			branch.branch_target = skip;
			add(std::move(branch));
		}
		call_to(found->pc);
	}
	call_to(continuation);
	for (const auto& function: functions) {
		EXIT_IF(next_pc != function.pc);
		for (auto inst: function.code) {
			if (inst.opcode == Decoder::Opcode::S_SETPC_B64) {
				inst.opcode        = Decoder::Opcode::S_BRANCH;
				inst.branch_target = continuation;
			} else if (Decoder::IsDirectBranch(inst.opcode)) {
				inst.branch_target += function.pc;
			}
			inst.pc += function.pc;
			linked_code.push_back(std::move(inst));
		}
		linked_words.insert(linked_words.end(), function.words.begin(), function.words.end());
		next_pc += static_cast<uint32_t>(function.words.size() * 4u);
	}
	EXIT_IF(next_pc != region + region_bytes);

	ShaderSource::Linked linked;
	const auto call_index = site.instruction;
	const auto split      = inline_code ? call_index + 1u : main.size();
	const auto word_split = std::min<size_t>(inline_code ? call_end / 4u : source.code.size(), source.code.size());
	linked.decoded = source.decoded;
	auto& out = linked.decoded.instructions;
	out.assign(main.begin(), main.begin() + split);
	out.insert(out.end(), linked_code.begin(), linked_code.end());
	out.insert(out.end(), main.begin() + split, main.end());
	for (size_t i = 0; i < main.size(); ++i) {
		auto& inst = out[i < split ? i : i + linked_code.size()];
		if (i >= split) inst.pc += shift;
		if (Decoder::IsDirectBranch(inst.opcode) && inst.branch_target > call.pc) inst.branch_target += shift;
	}
	out[call_index].opcode        = Decoder::Opcode::S_BRANCH;
	out[call_index].branch_target = region;
	linked.code.assign(source.code.begin(), source.code.begin() + word_split);
	linked.code.insert(linked.code.end(), linked_words.begin(), linked_words.end());
	linked.code.insert(linked.code.end(), source.code.begin() + word_split, source.code.end());
	linked.decoded.code = linked.code;
	LOGF("Shader call at pc 0x%08x: %zu functions linked from a table of %zu targets, %u not linkable%s\n",
	     call.pc, functions.size(), targets.size(), failed, inline_code ? "" : " (after the program)");
	linked.observed_function = std::move(observed);
	linked.observed_ranges   = ranges;
	linked.table_targets     = targets;
	read_table();
	for (const auto& [address, count]: ranges) reads.emplace_back(address, uint64_t {count} * 4u);
	linked.id = ++source.link_count;
	if (source.linked) {
		constexpr size_t MaxParkedLinks = 64u;
		if (source.parked_links.size() >= MaxParkedLinks) source.parked_links.erase(source.parked_links.begin());
		source.parked_links.push_back(std::move(*source.linked));
	}
	source.linked = std::move(linked);
	++source.revision;
	return source.linked->decoded;
}

} // namespace

const Decoder::Program& RefreshShaderSource(ShaderSource& source, const IR::SrtRuntime& runtime) {
	EXIT_IF(source.unsupported_call_pc != UINT32_MAX);
	auto& reads = source.reads;
	reads.clear();
	std::vector<uint32_t> flat;
	const bool walked = IR::WalkSrt(source.call_targets, runtime, flat, &reads);
	// A call through a table is the program's only call (PrepareCallTargets).
	if (source.calls.size() == 1u && source.calls[0].table) {
		if (!walked) {
			EXIT("shader call at pc 0x%08x has an unavailable table",
			     source.decoded.instructions[source.calls[0].instruction].pc);
		}
		return LinkCallTable(source, runtime);
	}
	const auto read_code = [&](uint64_t address, std::span<uint32_t> words) {
		if (runtime.read_specialization_memory == nullptr ||
		    !runtime.read_specialization_memory(runtime.userdata, address, words)) return false;
		reads.emplace_back(address, words.size_bytes());
		return true;
	};
	for (auto& site: source.calls) {
		uint32_t low = 0, high = 0;
		if (!walked || !IR::WalkedValue(source.call_targets, site.target[0], low) ||
		    !IR::WalkedValue(source.call_targets, site.target[1], high))
			EXIT("shader call at pc 0x%08x has an unavailable scalar target",
			     source.decoded.instructions[site.instruction].pc);
		// SWAPPC reads the old aliased pair and ignores the target's low two bits.
		const auto address = ((uint64_t {high} << 32u) | low) & ~uint64_t {3};
		EXIT_IF(address == 0u);
		site.address = address;
	}
	const auto query_reads = reads.size();
	if (source.linked) {
		auto& linked = *source.linked;
		bool unchanged = true;
		for (size_t i = 0; i < source.calls.size(); ++i) {
			const auto& function = linked.functions[i];
			const auto expected = std::span(linked.code).subspan(function.offset, function.size);
			const auto preceding = std::span(source.calls).first(i);
			const auto previous = std::ranges::find(preceding, source.calls[i].address, &ShaderSource::Call::address);
			if (previous != preceding.end()) {
				const auto& observed = linked.functions[previous - preceding.begin()];
				if (std::ranges::equal(expected, std::span(linked.code).subspan(observed.offset, observed.size))) continue;
				unchanged = false;
				break;
			}
			linked.observed_function.resize(function.size);
			if (!read_code(source.calls[i].address, linked.observed_function) ||
			    !std::ranges::equal(expected, linked.observed_function)) {
				unchanged = false;
				break;
			}
		}
		if (unchanged) return linked.decoded;
		reads.resize(query_reads);
	}
	ShaderSource::Linked linked {.code = source.code, .decoded = source.decoded};
	const auto& last = source.decoded.instructions.back();
	EXIT_NOT_IMPLEMENTED(last.opcode != Decoder::Opcode::S_ENDPGM);
	// Keep the caller's data/footer at native offsets. Expand leaf CFGs per call site
	// before SSA, so return edges neither require a dispatcher nor merge resource roots.
	uint32_t function_pc = last.pc + last.word_count * sizeof(uint32_t);
	std::map<uint64_t, std::vector<Decoder::Instruction>> functions;
	for (size_t i = 0; i < source.calls.size(); ++i) {
		const auto& site = source.calls[i];
		const auto& call = source.decoded.instructions[site.instruction];
		const auto address = source.calls[i].address;
		auto [entry, inserted] = functions.try_emplace(address);
		auto& function = entry->second;
		if (inserted) {
			for (uint32_t index = 0;;) {
				std::array<uint32_t, Decoder::MaxInstructionRawWords> words {};
				EXIT_IF(index > UINT32_MAX / sizeof(uint32_t) - words.size() ||
				        address > UINT64_MAX - (uint64_t {index} + words.size()) * sizeof(uint32_t));
				uint32_t count = 1;
				Decoder::Instruction inst;
				for (;;) {
					if (!read_code(address + uint64_t {index} * sizeof(uint32_t), std::span(words).first(count)))
						EXIT("shader call at pc 0x%08x cannot read leaf code", call.pc);
					inst = {};
					Decoder::DecodeInstruction(words, 0u, inst);
					if (inst.word_count <= count) break;
					count = inst.word_count;
				}
				inst.pc = index * sizeof(uint32_t);
				if (Decoder::IsDirectBranch(inst.opcode)) inst.branch_target += inst.pc;
				index += inst.word_count;
				EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::S_SWAPPC_B64 ||
				                     inst.opcode == Decoder::Opcode::S_GETPC_B64 ||
				                     inst.opcode == Decoder::Opcode::S_ENDPGM);
				function.push_back(inst);
				if (inst.opcode == Decoder::Opcode::S_SETPC_B64) break;
			}
		}
		const uint32_t function_size = function.back().pc + function.back().word_count * sizeof(uint32_t);
		EXIT_IF(function_pc > UINT32_MAX - function_size);
		const auto function_offset = static_cast<uint32_t>(linked.code.size());
		for (auto inst: function) {
			EXIT_IF(CFG::MayWriteScalarRegister(inst, call.dst.reg, source.wave_size) ||
			        CFG::MayWriteScalarRegister(inst, call.dst.reg + 1u, source.wave_size));
			for (uint32_t code = 0; code < site.preserved_registers.size(); ++code) {
				if (site.preserved_registers[code] && CFG::MayWriteScalarRegister(inst, code, source.wave_size))
					EXIT("shader call at pc 0x%08x clobbers scalar target dependency s%u", call.pc, code);
			}
			linked.code.insert(linked.code.end(), inst.raw, inst.raw + inst.word_count);
			if (inst.opcode == Decoder::Opcode::S_SETPC_B64) {
				EXIT_NOT_IMPLEMENTED(inst.src0.kind != Decoder::OperandKind::Sgpr || inst.src0.reg != call.dst.reg);
				inst.opcode = Decoder::Opcode::S_BRANCH;
				inst.branch_target = call.pc + call.word_count * sizeof(uint32_t);
			} else if (Decoder::IsDirectBranch(inst.opcode)) {
				EXIT_IF(inst.branch_target >= function_size);
				inst.branch_target += function_pc;
			}
			inst.pc += function_pc;
			linked.decoded.instructions.push_back(std::move(inst));
		}
		linked.functions.push_back({function_offset, function_size / 4u});
		linked.decoded.instructions[site.instruction].branch_target = function_pc;
		function_pc += function_size;
	}
	reads.resize(query_reads);
	for (size_t i = 0; i < source.calls.size(); ++i) {
		const auto preceding = std::span(source.calls).first(i);
		if (std::ranges::find(preceding, source.calls[i].address, &ShaderSource::Call::address) == preceding.end())
			reads.emplace_back(source.calls[i].address, linked.functions[i].size * sizeof(uint32_t));
	}
	linked.decoded.code = linked.code;
	linked.observed_function.resize(std::ranges::max_element(linked.functions, {},
	    &ShaderSource::Linked::Function::size)->size);

	source.linked = std::move(linked);
	++source.revision;
	return source.linked->decoded;
}

TranslateResult TranslateProgram(std::span<const uint32_t> code, const CompileOptions& options) {
	auto source = PrepareShaderSource(code, options);
	return TranslateProgram(source.decoded, options);
}

TranslateResult TranslateProgram(const Decoder::Program& decoded, const CompileOptions& options) {
	EXIT_IF(decoded.instructions.empty());
	if (decoded.has_swap_pc) {
		for (const auto& inst: decoded.instructions) {
			if (inst.opcode == Decoder::Opcode::S_SWAPPC_B64 && inst.branch_target == UINT32_MAX)
				EXIT("shader call at pc 0x%08x must be linked before translation", inst.pc);
		}
	}
	if (options.stage != ShaderType::Compute && options.stage != ShaderType::Vertex &&
	    options.stage != ShaderType::Pixel && options.stage != ShaderType::Mesh &&
	    options.stage != ShaderType::Local && options.stage != ShaderType::TessellationControl &&
	    options.stage != ShaderType::TessellationEvaluation) {
		EXIT("shader recompiler received unsupported stage %u\n",
		     static_cast<unsigned>(options.stage));
	}

	const auto* stage        = StageName(options.stage);
	const auto compile_begin = std::chrono::steady_clock::now();
	const auto phase_ms = [&compile_begin]() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now() - compile_begin).count());
	};

	std::string decoded_dump;
	if (options.dump_ir) {
		decoded_dump = Decoder::ProgramToString(decoded);
		LOGF("ShaderRecompiler %s decoded RDNA2:\n%s", stage, decoded_dump.c_str());
	}

	LOGF("ShaderRecompiler %s phase begin: hash=0x%016" PRIx64 " CFG BuildGraph\n", stage,
	     options.shader_hash);
	auto native_cfg = CFG::BuildGraph(decoded);
	CFG::Graph structured_cfg;
	auto* selected_cfg = &native_cfg;
	LOGF("ShaderRecompiler %s phase end: hash=0x%016" PRIx64 " CFG BuildGraph blocks=%" PRIu64
	     " loops=%" PRIu64 " back_edges=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
	     stage, options.shader_hash,
	     static_cast<uint64_t>(native_cfg.blocks.size()),
	     static_cast<uint64_t>(native_cfg.natural_loops.size()),
	     static_cast<uint64_t>(native_cfg.back_edges.size()), phase_ms());
	if (native_cfg.irreducible) {
		LogDispatcherFallback(options, native_cfg, "build");
	} else {
		LOGF("ShaderRecompiler %s phase begin: hash=0x%016" PRIx64 " CFG Structurize\n", stage,
		     options.shader_hash);
		structured_cfg = CFG::Structurize(native_cfg);
		if (structured_cfg.unsupported) {
			native_cfg.unsupported = true;
			native_cfg.failure_kind = structured_cfg.failure_kind;
			native_cfg.failure_block = structured_cfg.failure_block;
			native_cfg.unsupported_reason = structured_cfg.unsupported_reason;
			LogDispatcherFallback(options, native_cfg, "structurize");
		} else {
			selected_cfg = &structured_cfg;
			LOGF("ShaderRecompiler %s structured CFG success: blocks=%" PRIu64 "\n", stage,
			     static_cast<uint64_t>(selected_cfg->blocks.size()));
		}
		LOGF("ShaderRecompiler %s phase end: hash=0x%016" PRIx64 " CFG Structurize blocks=%" PRIu64
		     " loops=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
		     stage, options.shader_hash,
		     static_cast<uint64_t>(selected_cfg->blocks.size()),
		     static_cast<uint64_t>(selected_cfg->natural_loops.size()), phase_ms());
	}

	const auto& cfg = *selected_cfg;
	Frontend::EmbeddedFetchPlan embedded_fetch;
	if ((options.stage == ShaderType::Vertex || options.stage == ShaderType::Local) &&
	    options.input_info.vertex->has_dynamic_vertex_fetch) {
		embedded_fetch = DetectEmbeddedVertexFetch(
		    decoded, *options.input_info.vertex, options.user_data_base,
		    static_cast<uint32_t>(options.user_data.size()), options.wave_size);
		if (!embedded_fetch.loads.empty()) {
			LOGF("ShaderRecompiler %s embedded vertex fetch plan: detected=%" PRIu64 "\n", stage,
			     static_cast<uint64_t>(embedded_fetch.loads.size()));
		}
	}
	Frontend::TranslateOptions translate_options {
	    .stage            = options.stage,
	    .wave_size        = options.wave_size,
	    .shader_hash      = options.shader_hash,
	    .user_data_base   = options.user_data_base,
	    .user_data_count  = static_cast<uint32_t>(options.user_data.size()),
	    .input_info       = options.input_info,
	    .embedded_fetch   = embedded_fetch.loads.empty() ? nullptr : &embedded_fetch,
	};
	LOGF("ShaderRecompiler %s phase begin: hash=0x%016" PRIx64 " IR TranslateProgram\n", stage,
	     options.shader_hash);
	auto ir = Frontend::TranslateProgram(decoded, cfg, translate_options);
	LOGF("ShaderRecompiler %s phase end: hash=0x%016" PRIx64 " IR TranslateProgram blocks=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     stage, options.shader_hash,
	     static_cast<uint64_t>(ir.blocks.size()), phase_ms());
	IR::RewriteToSsa(ir.blocks);
	const auto simplify = [&ir] {
		IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
		IR::RemoveIdentities(ir);
		IR::EliminateDeadCode(ir.blocks);
	};
	// Wave reductions are recognized before constant propagation too. In a pixel shader, constant
	// propagation folds away the select that puts the identity outside the live pixels, because
	// EXEC is true in every invocation the host has. Without that select the reduction cannot be
	// proven, and lane-by-lane it reads lanes the host lacks: EA SPORTS FC 27's light loop got a
	// garbage bound. The run after constant propagation finds the forms that folding makes visible.
	// The subgroup reduction combines every host invocation; whether the driver counts the helper
	// pixels of a quad in it is up to the driver (the guest leaves them out through its live mask).
	const bool lane_model = !Spirv::UsesSingleLaneModel(options.stage, options.host);
	if (lane_model) {
		(void)IR::RecognizeWaveReductions(ir);
	}
	simplify();
	if (lane_model && IR::RecognizeWaveReductions(ir) != 0u) {
		IR::EliminateDeadCode(ir.blocks);
	}
	const auto read_lane_stats = IR::EliminateReadLane(ir, ir.wave_size);
	if (read_lane_stats.rewritten_reads != 0 || read_lane_stats.rewritten_reductions != 0) {
		LOGF("ShaderRecompiler %s read-lane elimination: reads=%" PRIu32
		     " reductions=%" PRIu32 "\n",
		     stage, read_lane_stats.rewritten_reads, read_lane_stats.rewritten_reductions);
	}
	if (Spirv::UsesSingleLaneModel(options.stage, options.host) &&
	    IR::LowerLaneOpsToSingleLane(ir) != 0u) {
		IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
		IR::RemoveIdentities(ir);
		IR::EliminateDeadCode(ir.blocks);
	}
	// Read-lane elimination matches EXEC selects, so the masked ones go only after it.
	(void)IR::EliminateMaskedSelects(ir.blocks);
	// Cleanup also exposes profitable folds by removing dead predicate consumers.
	simplify();
	if (decoded.has_swap_pc) {
		// Leaf returns are resolved CFG edges; the return address of a call (the pc after its
		// S_SWAPPC_B64) must not escape into GPU data. A shader base from S_GETPC_B64 may stay: it
		// is shader-relative in SPIR-V, and the resource plan reads the descriptors built on it
		// (constants the shader keeps in its own code).
		std::set<uint64_t> return_addresses;
		for (const auto& inst: decoded.instructions)
			if (inst.opcode == Decoder::Opcode::S_SWAPPC_B64) return_addresses.insert(uint64_t {inst.pc} + 4u);
		for (const auto* block: ir.blocks) {
			for (const auto& inst: *block) {
				if (inst.GetOpcode() != IR::ValueOpcode::IAdd64) continue;
				const auto* base = inst.Arg(0).Resolve().TryInstruction();
				const auto  step = inst.Arg(1).Resolve();
				EXIT_NOT_IMPLEMENTED(base != nullptr && base->GetOpcode() == IR::ValueOpcode::GetShaderBase &&
				                     step.IsImmediate() && return_addresses.contains(step.U64()));
			}
		}
	}
	LowerTessellationMemory(ir, options);
	std::string cfg_dump;
	if (options.dump_ir) {
		cfg_dump = CFG::GraphToString(cfg);
		LOGF("ShaderRecompiler %s native IR before resource tracking:\n%s", stage,
		     MakeIrDump(cfg_dump, ir).c_str());
	}
	IR::TrackResources(ir, decoded, native_cfg, options.input_info);
	TranslateResult result;
	result.program = std::move(ir);
	if (options.dump_ir) {
		result.decoded_dump = std::move(decoded_dump);
		result.cfg_dump     = std::move(cfg_dump);
	}
	return result;
}

CompileResult CompileProgram(TranslateResult translated, const CompileOptions& options,
                             const IR::ResourceSpecialization& specialization,
                             uint32_t push_data_start_dword) {
	const auto emit_begin = std::chrono::steady_clock::now();
	auto& ir = translated.program;
	const auto* stage = StageName(ir.stage);
	IR::ApplyResourceSpecialization(ir, specialization);
	// The resource plan owns host descriptor evaluation now. Keep only dependencies consumed
	// by GPU memory operations; bound descriptor dwords must not retain shader instructions.
	for (auto& inst: ir.value_storage) {
		inst.Invalidate();
	}
	for (auto* block: ir.blocks) {
		for (auto& inst: *block) {
			const auto op = inst.GetOpcode();
			uint32_t first = 0;
			if (op == IR::ValueOpcode::GetBufferResource) {
				if (std::ranges::any_of(inst.Uses(), [&](const IR::Use& use) {
					return ir.memory_info[use.user->Flags<IR::MemoryFlags>().index].kind ==
					       IR::ResourceKind::IndirectBuffer;
				})) {
					continue;
				}
				const auto resource = inst.Flags<uint32_t>();
				first               = resource < ir.info.buffers.size() &&
				                              ir.info.buffers[resource].indirect_root == resource
				                          ? 1u
				                          : 0u;
			} else if (op == IR::ValueOpcode::GetImageResource) {
				const auto resource = inst.Flags<uint32_t>();
				first = resource < ir.info.images.size() &&
				                ir.info.images[resource].indirect_root == resource ? 1u : 0u;
			} else if (op == IR::ValueOpcode::GetSamplerResource) {
				const auto resource = inst.Flags<uint32_t>();
				first = resource < ir.info.samplers.size() &&
				                !ir.info.samplers[resource].indirect_resources.empty() ? 1u : 0u;
			} else {
				continue;
			}
			for (size_t index = first; index < inst.NumArgs(); index++) {
				inst.SetArg(index, IR::Value(0u));
			}
		}
	}
	ir.value_storage.clear();
	IR::RemoveIdentities(ir);
	IR::EliminateDeadCode(ir.blocks);

	IR::CollectShaderInfo(ir, options.input_info);
	std::string ir_dump;
	if (options.dump_ir) {
		ir_dump = MakeIrDump(translated.cfg_dump, ir);
		LOGF("ShaderRecompiler %s native IR:\n%s", stage, ir_dump.c_str());
	}

	LOGF("ShaderRecompiler %s phase begin: hash=0x%016" PRIx64 " SPIR-V EmitProgram\n", stage,
	     ir.shader_hash);
	ir.mesh_emulated = ir.stage == ShaderType::Mesh && options.input_info.vertex->mesh.emulated;
	auto spirv = Spirv::EmitProgram(ir, options.input_info, push_data_start_dword, options.host,
	                                options.emit_debug_info);
	LOGF("ShaderRecompiler %s phase end: hash=0x%016" PRIx64 " SPIR-V EmitProgram words=%" PRIu64
	     " wave_size=%u elapsed_ms=%" PRIu64 "\n",
	     stage, ir.shader_hash, static_cast<uint64_t>(spirv.size()), ir.wave_size,
	     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                               std::chrono::steady_clock::now() - emit_begin)
	                               .count()));
	CompileResult result;
	result.spirv = std::move(spirv);
	if (ir.stage == ShaderType::Mesh && options.input_info.vertex->mesh.emulated) {
		auto vertex              = Spirv::EmitMeshEmulationVertexProgram(ir, options.input_info);
		result.mesh_vertex_spirv = std::move(vertex.spirv);
		result.mesh_slot_words   = vertex.slot_words;
	}
	result.program = std::move(ir);
	if (options.dump_ir) {
		result.decoded_dump = std::move(translated.decoded_dump);
		result.ir_dump      = std::move(ir_dump);
	}
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler
