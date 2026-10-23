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
#include <span>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler {

namespace {

const char* GetDumpLabel(const CompileOptions& options) {
	return options.dump_label != nullptr ? options.dump_label : "ShaderRecompiler";
}

std::string MakeIrDump(std::string_view cfg, const IR::Program& ir) {
	std::string dump = "CFG:\n";
	dump += cfg;
	dump += "\nIR:\n";
	dump += fmt::format("mode={} scratch_dwords={}\n",
	                    ir.dispatcher_fallback ? "dispatcher" : "structured", ir.scratch_dwords);
	dump += IR::ProgramToString(ir);
	return dump;
}

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

void LogDispatcherFallback(const CompileOptions& options, const CFG::Graph& cfg, const char* phase) {
	const auto* block        = cfg.FindBlock(cfg.failure_block);
	const auto  start        = block != nullptr ? block->start_pc : UINT32_MAX;
	const auto  end          = block != nullptr ? block->end_pc : UINT32_MAX;
	const auto  predecessors = block != nullptr ? block->predecessors.size() : 0u;
	const auto  successors   = block != nullptr ? block->successors.size() : 0u;
	LOGF("%s CFG dispatcher fallback: stage=%s hash=0x%016" PRIx64
	     " phase=%s failure=%s block=%" PRIu32 " pc=0x%08" PRIx32 "..0x%08" PRIx32 " preds=%" PRIu64
	     " succs=%" PRIu64 " blocks=%" PRIu64 " loops=%" PRIu64 " back_edges=%" PRIu64
	     " reason=%s\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash, phase,
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

// Folds an expression of U32 immediates (the translator masks shift counts with BitwiseAnd, and
// constant propagation has not run yet when call targets are prepared).
std::optional<uint32_t> ConstantU32(IR::Value value, uint32_t depth = 0) {
	using namespace IR;
	value = value.Resolve();
	if (value.IsImmediate()) {
		if (value.GetType() != Type::U32) return std::nullopt;
		return value.U32();
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || depth > 4u || inst->NumArgs() != 2u) return std::nullopt;
	const auto a = ConstantU32(inst->Arg(0), depth + 1u);
	const auto b = ConstantU32(inst->Arg(1), depth + 1u);
	if (!a || !b) return std::nullopt;
	switch (inst->GetOpcode()) {
		case ValueOpcode::BitwiseAnd32: return *a & *b;
		case ValueOpcode::IAdd32: return *a + *b;
		case ValueOpcode::IMul32: return *a * *b;
		case ValueOpcode::ShiftLeftLogical32: return *b < 32u ? *a << *b : 0u;
		default: return std::nullopt;
	}
}

// Recognizes a call target pair read as ReadConstBuffer(V#, index * stride) + immediate from one
// runtime-evaluable buffer descriptor: a function table selected by a GPU value.
std::optional<ShaderSource::Call::Table> MatchCallTable(const IR::Program& query,
                                                        const std::array<const IR::Inst*, 2>& reads) {
	using namespace IR;
	if (reads[0] == nullptr || reads[1] == nullptr) return std::nullopt;
	// Both target words must come from the same buffer at the same dynamic offset; they differ
	// only in their immediate component offset (low word, then high word).
	const auto* handle = reads[0]->Arg(0).ResolveInstruction();
	if (handle == nullptr || handle->NumArgs() != 4u || reads[1]->Arg(0).ResolveInstruction() != handle ||
	    reads[0]->Arg(1).Resolve() != reads[1]->Arg(1).Resolve())
		return std::nullopt;
	ShaderSource::Call::Table table;
	// The table itself must be found without GPU data: its V# is re-read on every refresh.
	for (uint32_t i = 0; i < 4u; ++i) {
		const auto word = handle->Arg(i).Resolve();
		if (!ValidateRuntimeValue(query, word, RuntimeValueType::Integer)) return std::nullopt;
		table.descriptor[i] = word;
	}
	// The lane-selected offset: index << s or index * c, optionally plus a constant.
	auto     offset   = reads[0]->Arg(1).Resolve();
	uint32_t constant = 0;
	if (const auto* add = offset.TryInstruction(); add != nullptr && add->GetOpcode() == ValueOpcode::IAdd32) {
		if (const auto b = ConstantU32(add->Arg(1))) {
			constant = *b; offset = add->Arg(0).Resolve();
		} else if (const auto a = ConstantU32(add->Arg(0))) {
			constant = *a; offset = add->Arg(1).Resolve();
		}
	}
	const auto* scale = offset.TryInstruction();
	if (scale == nullptr || scale->NumArgs() != 2u) return std::nullopt;
	const auto amount = ConstantU32(scale->Arg(1));
	if (!amount) return std::nullopt;
	if (scale->GetOpcode() == ValueOpcode::ShiftLeftLogical32 && *amount >= 2u && *amount < 16u) {
		table.stride = 1u << *amount;
	} else if (scale->GetOpcode() == ValueOpcode::IMul32 && *amount >= 8u && *amount <= 0x10000u &&
	           *amount % 4u == 0u) {
		table.stride = *amount;
	} else {
		return std::nullopt;
	}
	// Byte position of each target word inside one entry. The pair must be adjacent and fit
	// inside the stride, otherwise entries would overlap and the scan would read garbage.
	std::array<uint32_t, 2> position {};
	for (uint32_t i = 0; i < 2u; ++i) {
		const auto index = reads[i]->Flags<MemoryFlags>().index;
		if (index >= query.memory_info.size()) return std::nullopt;
		position[i] = (query.memory_info[index].offset & ~3u) + (constant & ~3u);
	}
	if (position[1] != position[0] + 4u || position[1] + 4u > table.stride) return std::nullopt;
	table.low_offset = position[0];
	return table;
}

void PrepareCallTargets(ShaderSource& source, const CompileOptions& options) {
	using namespace IR;
	EXIT_NOT_IMPLEMENTED(options.stage != ShaderType::Compute);
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
	// Set when an operand feeding a call target has several reaching definitions. Only the
	// target registers themselves can carry several candidates, so such shaders fall back.
	uint32_t ambiguous_pc = UINT32_MAX;
	uint32_t ambiguous_reg = 0;
	// Register(before, code): the value of scalar register `code` just before instruction
	//   `before`, which must have exactly one reaching definition (else the source is marked
	//   ambiguous).
	// Produce(index, code): the value instruction `index` writes to `code`, translated once and
	//   with its own operands resolved recursively through Register. UINT32_MAX = user data.
	// Definitions(before, code): all reaching writes; records registers that must survive any
	//   call crossed on the way, so linked leaves are checked not to clobber them.
	std::function<Value(uint32_t, uint32_t)> Register;
	std::function<Value(uint32_t, uint32_t)> Produce;
	const auto Definitions = [&](uint32_t before, uint32_t code) {
		std::vector<uint32_t> crossed_calls;
		auto definitions = CFG::FindScalarDefinitions(source.decoded, graph, before, code, crossed_calls,
		                                              options.wave_size == 64u);
		for (const auto crossed: crossed_calls) {
			std::ranges::find(source.calls, crossed, &ShaderSource::Call::instruction)->preserved_registers.set(code);
		}
		return definitions;
	};
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
		const auto definitions = Definitions(before, code);
		if (definitions.size() > 1u) {
			// The placeholder is never used: the whole source takes the fallback below. Not
			// producing a definition here also avoids recursing into a loop-carried producer.
			if (ambiguous_pc == UINT32_MAX) {
				ambiguous_pc  = source.decoded.instructions[before].pc;
				ambiguous_reg = code;
			}
			return Value(0u);
		}
		return Produce(definitions.front(), code);
	};
	Produce = [&](uint32_t index, uint32_t code) -> Value {
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
	for (auto& site: source.calls) {
		const auto& call = source.decoded.instructions[site.instruction];
		EXIT_NOT_IMPLEMENTED(call.src0.kind != Decoder::OperandKind::Sgpr ||
		                     call.dst.kind != Decoder::OperandKind::Sgpr ||
		                     call.src0.reg != call.dst.reg || call.dst.reg >= 105u);
		// For each target word: every reaching definition becomes a candidate value that the CPU
		// evaluates at dispatch. A single definition that the CPU cannot evaluate (it depends on
		// GPU data) may still be a read from a function table; remember it for MatchCallTable.
		std::array<const Inst*, 2> table_reads {};
		for (uint32_t i = 0; i < 2u; ++i) {
			const auto definitions = Definitions(site.instruction, call.src0.reg + i);
			if (definitions.size() > 1u) {
				LOGF("ShaderRecompiler CS call at pc 0x%08x: s%u has %u reaching definitions\n", call.pc,
				     call.src0.reg + i, static_cast<uint32_t>(definitions.size()));
			}
			for (const auto definition: definitions) {
				const auto value = Produce(definition, call.src0.reg + i);
				if (ambiguous_pc != UINT32_MAX) break;
				if (!ValidateRuntimeValue(query, value, RuntimeValueType::Integer)) {
					// Only a single buffer read can be a function table; any other target that
					// depends on GPU data takes the dispatch-skip fallback.
					const auto* read = value.TryInstruction();
					if (definitions.size() == 1u && read != nullptr &&
					    read->GetOpcode() == ValueOpcode::ReadConstBuffer) {
						table_reads[i] = read;
						continue;
					}
					LOGF("ShaderRecompiler CS call at pc 0x%08x: s%u depends on GPU data and is not a "
					     "function-table read\n", call.pc, call.src0.reg + i);
					source.unsupported_call_pc = call.pc;
					source.calls.clear();
					return;
				}
				auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
				root.SetArg(0, value);
				site.target[i].push_back(Value(&root));
			}
			if (ambiguous_pc != UINT32_MAX) break;
		}
		if (ambiguous_pc != UINT32_MAX) break;
		if (table_reads[0] != nullptr || table_reads[1] != nullptr) {
			auto table = MatchCallTable(query, table_reads);
			if (!table || !options.function_table_calls) {
				LOGF("ShaderRecompiler CS call at pc 0x%08x: GPU-selected target is %s\n", call.pc,
				     table ? "a function table (linking disabled; see --function-table-calls)"
				           : "not a recognized function table");
				source.unsupported_call_pc = call.pc;
				source.calls.clear();
				return;
			}
			// Root the descriptor words like targets so the cleanup passes below keep them alive;
			// the per-entry targets are read from memory at refresh time instead.
			for (auto& word: table->descriptor) {
				auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
				root.SetArg(0, word);
				word = Value(&root);
			}
			site.target = {};
			site.table  = std::move(table);
			LOGF("ShaderRecompiler CS call at pc 0x%08x: linked through a function table "
			     "(stride %u, low word at +%u)\n", call.pc, site.table->stride, site.table->low_offset);
		}
	}
	if (ambiguous_pc != UINT32_MAX) {
		LOGF("ShaderRecompiler CS call target operand s%u at pc 0x%08x has several reaching "
		     "definitions; using the dispatch-skip fallback\n", ambiguous_reg, ambiguous_pc);
		source.unsupported_call_pc = ambiguous_pc;
		source.calls.clear();
		return;
	}
	RewriteToSsa(query.blocks);
	ConstantPropagationPass(query.blocks, query.wave_size);
	RemoveIdentities(query);
	EliminateDeadCode(query.blocks);
	// Strip the ReferenceU32 roots: keep the simplified values they point at.
	for (auto& site: source.calls) {
		for (auto& candidates: site.target)
			for (auto& value: candidates) value = value.Instruction()->Arg(0).Resolve();
		if (site.table)
			for (auto& word: site.table->descriptor) word = word.Instruction()->Arg(0).Resolve();
	}
	query.value_storage.clear();
	for (auto& inst: block) {
		inst.SetParent(nullptr);
		if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 || inst.GetOpcode() == ValueOpcode::ReadConstBuffer)
			inst.SetFlags(SrtReadFlags {.index = inst.Flags<MemoryFlags>().index});
	}
	query.value_storage.splice(query.value_storage.end(), block.Instructions());
	source.call_targets = std::move(static_cast<ResourcePlan&>(query));
}

} // namespace

ShaderSource PrepareShaderSource(std::span<const uint32_t> code, const CompileOptions& options) {
	EXIT_IF(code.empty());
	ShaderSource source;
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
	if (source.decoded.has_swap_pc) {
		PrepareCallTargets(source, options);
		source.code.assign(code.begin(), code.end());
		source.decoded.code = source.code;
	}
	return source;
}

namespace {

// Encodings for the synthesized table-call dispatcher (GFX10 SOPP / SOPC).
constexpr uint32_t SoppBranch      = 0x02u;
constexpr uint32_t SoppCbranchScc1 = 0x05u;
constexpr uint32_t SopcCmpLgU32    = 0x07u;
constexpr uint64_t MaxTableBytes   = uint64_t {64} << 20u;
constexpr uint64_t TableChunkBytes = uint64_t {1} << 20u;
constexpr uint32_t MaxTableCallees = 64u;

uint32_t EncodeSoppBranch(uint32_t op, uint32_t pc, uint32_t target) {
	const auto words = (static_cast<int64_t>(target) - static_cast<int64_t>(pc) - 4) / 4;
	EXIT_IF(words < INT16_MIN || words > INT16_MAX);
	return 0xbf800000u | (op << 16u) | (static_cast<uint32_t>(words) & 0xffffu);
}

void AppendSynthesized(ShaderSource::Linked& linked, std::initializer_list<uint32_t> words, uint32_t pc,
                       Decoder::Opcode expected, uint32_t branch_target = UINT32_MAX) {
	Decoder::Instruction inst;
	const std::vector<uint32_t> code(words);
	Decoder::DecodeInstruction(code, 0u, inst);
	EXIT_IF(inst.opcode != expected || inst.word_count != code.size());
	inst.pc = pc;
	if (branch_target != UINT32_MAX) inst.branch_target = branch_target;
	linked.code.insert(linked.code.end(), code.begin(), code.end());
	linked.decoded.instructions.push_back(std::move(inst));
}

} // namespace

const Decoder::Program& RefreshShaderSource(ShaderSource& source, const IR::SrtRuntime& runtime) {
	EXIT_IF(source.unsupported_call_pc != UINT32_MAX);
	auto& reads = source.reads;
	reads.clear();
	IR::SrtReadCapture capture(runtime, reads);
	const auto clean = IR::CleanRuntime(capture.ObservedRuntime());
	IR::SrtWalker walker(source.call_targets, clean);
	for (auto& site: source.calls) {
		const auto pc = source.decoded.instructions[site.instruction].pc;
		if (site.table) {
			// Read every entry of the function table; each distinct target becomes a callee.
			std::array<uint32_t, 4> descriptor {};
			for (uint32_t i = 0; i < 4u; ++i)
				if (!walker.Evaluate(site.table->descriptor[i], descriptor[i]))
					EXIT("shader call at pc 0x%08x has an unavailable function table", pc);
			// V# layout: base[47:0] in dwords 0-1, stride in dword 1 [29:16], num_records in dword 2.
			// Scalar buffer loads count records in bytes when the stride is zero.
			const auto base   = ((uint64_t {descriptor[1] & 0xffffu} << 32u) | descriptor[0]) & ~uint64_t {3};
			const auto stride = (descriptor[1] >> 16u) & 0x3fffu;
			const auto size   = stride == 0u ? uint64_t {descriptor[2]} : uint64_t {stride} * descriptor[2];
			const auto& table = *site.table;
			site.table_targets.clear();
			// Tables can be large (megabytes, with most entries empty), so read them in chunks: one
			// recorded read per chunk instead of one per entry.
			if (size > MaxTableBytes)
				EXIT("shader call at pc 0x%08x: function table is 0x%llx bytes", pc, static_cast<unsigned long long>(size));
			std::vector<uint32_t> chunk;
			for (uint64_t chunk_start = 0; chunk_start < size; chunk_start += TableChunkBytes) {
				const auto chunk_bytes = std::min<uint64_t>(TableChunkBytes, size - chunk_start) & ~uint64_t {3};
				if (chunk_bytes == 0u) break;
				chunk.resize(chunk_bytes / 4u);
				if (!clean.read_memory(clean.userdata, base + chunk_start, chunk))
					EXIT("shader call at pc 0x%08x cannot read its function table at +0x%llx", pc,
					     static_cast<unsigned long long>(chunk_start));
				// Each entry belongs to the chunk where its target pair starts, so every pair is
				// visited exactly once: the first entry here is the first whose pair starts at or
				// after chunk_start (the entry itself may start in the previous chunk). A pair that
				// straddles the chunk end is re-read on its own. Both cases need a stride that does
				// not divide the chunk size.
				const auto first_entry =
				    chunk_start <= table.low_offset
				        ? 0u
				        : (chunk_start - table.low_offset + table.stride - 1u) / table.stride * table.stride;
				for (uint64_t entry = first_entry; entry + table.low_offset + 8u <= size &&
				                                   entry + table.low_offset < chunk_start + chunk_bytes;
				     entry += table.stride) {
					const auto at = entry + table.low_offset - chunk_start;
					std::array<uint32_t, 2> words {};
					if (at + 8u <= chunk_bytes) {
						words = {chunk[at / 4u], chunk[at / 4u + 1u]};
					} else if (!clean.read_memory(clean.userdata, base + entry + table.low_offset, words)) {
						EXIT("shader call at pc 0x%08x cannot read its function table", pc);
					}
					// Zero entries are unused. Targets are kept raw (with their low bits):
					// the dispatcher compares the exact register values the shader loads.
					const auto target = (uint64_t {words[1]} << 32u) | words[0];
					if ((target & ~uint64_t {3}) == 0u ||
					    std::ranges::find(site.table_targets, target) != site.table_targets.end())
						continue;
					site.table_targets.push_back(target);
					if (site.table_targets.size() > MaxTableCallees)
						EXIT("shader call at pc 0x%08x: function table has over %u callees", pc, MaxTableCallees);
				}
			}
			site.address = 0;
			continue;
		}
		// Plain call: evaluate every candidate of each target word; they must all agree.
		std::array<uint32_t, 2> words {};
		for (uint32_t i = 0; i < 2u; ++i) {
			EXIT_IF(site.target[i].empty());
			for (size_t c = 0; c < site.target[i].size(); ++c) {
				uint32_t word = 0;
				if (!walker.Evaluate(site.target[i][c], word))
					EXIT("shader call at pc 0x%08x has an unavailable scalar target", pc);
				// Distinct candidates are fine as long as they name the same callee.
				if (c != 0 && word != words[i])
					EXIT("shader call at pc 0x%08x selects between callees at runtime "
					     "(s%u candidates 0x%08x and 0x%08x)", pc,
					     source.decoded.instructions[site.instruction].src0.reg + i, words[i], word);
				words[i] = word;
			}
		}
		const auto low = words[0], high = words[1];
		// SWAPPC reads the old aliased pair and ignores the target's low two bits.
		const auto address = ((uint64_t {high} << 32u) | low) & ~uint64_t {3};
		EXIT_IF(address == 0u);
		site.address = address;
	}
	// Callees in link order: one per plain call, one per distinct table target.
	std::vector<uint64_t> callees;
	for (const auto& site: source.calls) {
		if (!site.table) {
			callees.push_back(site.address);
			continue;
		}
		for (const auto target: site.table_targets) callees.push_back(target & ~uint64_t {3});
	}
	const auto query_reads = reads.size();
	if (source.linked) {
		auto& linked = *source.linked;
		// Plain callees may move if their code is unchanged; table targets are compared by value
		// because the dispatcher compares them.
		bool unchanged = linked.callees.size() == callees.size() && linked.tables.size() == source.calls.size();
		for (size_t i = 0; unchanged && i < source.calls.size(); ++i)
			unchanged = linked.tables[i] == source.calls[i].table_targets;
		// Then every linked body must still match guest memory. A callee seen earlier in the list
		// is compared against that earlier body instead of being re-read.
		for (size_t i = 0; unchanged && i < callees.size(); ++i) {
			const auto& function = linked.functions[i];
			const auto expected = std::span(linked.code).subspan(function.offset, function.size);
			const auto preceding = std::span(callees).first(i);
			const auto previous = std::ranges::find(preceding, callees[i]);
			if (previous != preceding.end()) {
				const auto& observed = linked.functions[previous - preceding.begin()];
				if (std::ranges::equal(expected, std::span(linked.code).subspan(observed.offset, observed.size))) continue;
				unchanged = false;
				break;
			}
			linked.observed_function.resize(function.size);
			if (!clean.read_memory(clean.userdata, callees[i], linked.observed_function) ||
			    !std::ranges::equal(expected, linked.observed_function)) {
				unchanged = false;
				break;
			}
		}
		if (unchanged) {
			linked.callees = std::move(callees);
			return linked.decoded;
		}
		reads.resize(query_reads);
	}
	ShaderSource::Linked linked {.code = source.code, .decoded = source.decoded};
	const auto& last = source.decoded.instructions.back();
	EXIT_NOT_IMPLEMENTED(last.opcode != Decoder::Opcode::S_ENDPGM);
	// Keep the caller's data/footer at native offsets. Expand leaf CFGs per call site
	// before SSA, so return edges neither require a dispatcher nor merge resource roots.
	uint32_t function_pc = last.pc + last.word_count * sizeof(uint32_t);
	linked.decoded.native_bytes = function_pc;
	std::map<uint64_t, std::vector<Decoder::Instruction>> functions;
	// Decodes the leaf at `address` once; returns its instructions (pcs relative to the leaf).
	const auto load = [&](const ShaderSource::Call& site, uint64_t address) -> const std::vector<Decoder::Instruction>& {
		const auto& call = source.decoded.instructions[site.instruction];
		auto [entry, inserted] = functions.try_emplace(address);
		auto& function = entry->second;
		if (!inserted) return function;
		for (uint32_t index = 0;;) {
			std::array<uint32_t, Decoder::MaxInstructionRawWords> words {};
			EXIT_IF(index > UINT32_MAX / sizeof(uint32_t) - words.size() ||
			        address > UINT64_MAX - (uint64_t {index} + words.size()) * sizeof(uint32_t));
			uint32_t count = 1;
			Decoder::Instruction inst;
			for (;;) {
				if (!clean.read_memory(clean.userdata, address + uint64_t {index} * sizeof(uint32_t),
				                       std::span(words).first(count)))
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
		return function;
	};
	const auto byte_size = [](const std::vector<Decoder::Instruction>& function) {
		return function.back().pc + function.back().word_count * static_cast<uint32_t>(sizeof(uint32_t));
	};
	// Appends the leaf at `address` at function_pc; its return branches to return_pc.
	const auto append_body = [&](const ShaderSource::Call& site, uint64_t address, uint32_t return_pc) {
		const auto& call     = source.decoded.instructions[site.instruction];
		const auto& function = load(site, address);
		const uint32_t function_size = byte_size(function);
		EXIT_IF(function_pc > UINT32_MAX - function_size);
		const auto function_offset = static_cast<uint32_t>(linked.code.size());
		const auto body_pc         = function_pc;
		// Copy the leaf, rebasing its pcs and branches to function_pc. The leaf must not write the
		// return-address pair or any register the caller still needs after the call.
		for (auto inst: function) {
			EXIT_IF(CFG::MayWriteScalarRegister(inst, call.dst.reg) ||
			        CFG::MayWriteScalarRegister(inst, call.dst.reg + 1u));
			for (uint32_t code = 0; code < site.preserved_registers.size(); ++code) {
				if (site.preserved_registers[code] && CFG::MayWriteScalarRegister(inst, code))
					EXIT("shader call at pc 0x%08x clobbers scalar target dependency s%u", call.pc, code);
			}
			linked.code.insert(linked.code.end(), inst.raw, inst.raw + inst.word_count);
			if (inst.opcode == Decoder::Opcode::S_SETPC_B64) {
				EXIT_NOT_IMPLEMENTED(inst.src0.kind != Decoder::OperandKind::Sgpr || inst.src0.reg != call.dst.reg);
				inst.opcode = Decoder::Opcode::S_BRANCH;
				inst.branch_target = return_pc;
			} else if (Decoder::IsDirectBranch(inst.opcode)) {
				EXIT_IF(inst.branch_target >= function_size);
				inst.branch_target += function_pc;
			}
			inst.pc += function_pc;
			linked.decoded.instructions.push_back(std::move(inst));
		}
		linked.functions.push_back({function_offset, function_size / 4u});
		linked.callees.push_back(address);
		function_pc += function_size;
		return body_pc;
	};
	for (const auto& site: source.calls) {
		const auto& call = source.decoded.instructions[site.instruction];
		const auto return_pc = call.pc + call.word_count * static_cast<uint32_t>(sizeof(uint32_t));
		if (!site.table) {
			linked.decoded.instructions[site.instruction].branch_target = append_body(site, site.address, return_pc);
			continue;
		}
		// Dispatcher, appended after the caller, followed by one body per target:
		//   per target k:  s_cmp_lg_u32 s[lo], k.low  ; s_cbranch_scc1 next
		//                  s_cmp_lg_u32 s[hi], k.high ; s_cbranch_scc1 next
		//                  s_swappc_b64 (linked to body k, which returns to the s_mov below)
		//                  s_mov_b32 s[lo], k.low ; s_mov_b32 s[hi], k.high
		//                  s_branch end
		//   end:           s_branch return_pc
		// The structurizer turns "s_branch end" into a guarded goto, so later compares see the
		// pair through a phi. Restoring the matched target there keeps the return PC (native
		// shader address) from flowing into them; callers never read the return PC back.
		// SWAPPC ignores the target's low two bits, and the table holds the exact pairs compared
		// here; a target outside the table (impossible while it is unchanged) skips the call.
		EXIT_NOT_IMPLEMENTED(call.word_count != 1u);
		constexpr uint32_t EntryBytes = 48u;
		const auto& targets  = site.table_targets;
		const auto  chain_pc = function_pc;
		const auto  end_pc   = chain_pc + static_cast<uint32_t>(targets.size()) * EntryBytes;
		// Bodies are laid out after the dispatcher, so their pcs are known before any is appended
		// and the chain's SWAPPCs can name them directly.
		std::vector<uint32_t> body_pcs;
		uint32_t next_body = end_pc + 4u;
		for (const auto target: targets) {
			body_pcs.push_back(next_body);
			next_body += byte_size(load(site, target & ~uint64_t {3}));
		}
		// Raw encodings: SOPC s_cmp_lg_u32 sN, literal and SOP1 s_mov_b32 sN, literal
		// (ssrc 0xff = literal dword follows).
		const auto low_sgpr = call.src0.reg, high_sgpr = call.src0.reg + 1u;
		const auto compare  = [](uint32_t sgpr) { return 0xbf000000u | (SopcCmpLgU32 << 16u) | (0xffu << 8u) | sgpr; };
		const auto move     = [](uint32_t sgpr) { return 0xbe8003ffu | (sgpr << 16u); }; // s_mov_b32 sgpr, literal
		for (size_t k = 0; k < targets.size(); ++k) {
			const auto entry = chain_pc + static_cast<uint32_t>(k) * EntryBytes;
			const auto next  = entry + EntryBytes; // the last entry's next is end_pc
			AppendSynthesized(linked, {compare(low_sgpr), static_cast<uint32_t>(targets[k])}, entry,
			                  Decoder::Opcode::S_CMP_LG_U32);
			AppendSynthesized(linked, {EncodeSoppBranch(SoppCbranchScc1, entry + 8u, next)}, entry + 8u,
			                  Decoder::Opcode::S_CBRANCH_SCC1, next);
			AppendSynthesized(linked, {compare(high_sgpr), static_cast<uint32_t>(targets[k] >> 32u)},
			                  entry + 12u, Decoder::Opcode::S_CMP_LG_U32);
			AppendSynthesized(linked, {EncodeSoppBranch(SoppCbranchScc1, entry + 20u, next)}, entry + 20u,
			                  Decoder::Opcode::S_CBRANCH_SCC1, next);
			AppendSynthesized(linked, {call.raw[0]}, entry + 24u, Decoder::Opcode::S_SWAPPC_B64, body_pcs[k]);
			AppendSynthesized(linked, {move(low_sgpr), static_cast<uint32_t>(targets[k])}, entry + 28u,
			                  Decoder::Opcode::S_MOV_B32);
			AppendSynthesized(linked, {move(high_sgpr), static_cast<uint32_t>(targets[k] >> 32u)}, entry + 36u,
			                  Decoder::Opcode::S_MOV_B32);
			AppendSynthesized(linked, {EncodeSoppBranch(SoppBranch, entry + 44u, end_pc)}, entry + 44u,
			                  Decoder::Opcode::S_BRANCH, end_pc);
		}
		// No entry matched (or a matched callee returned and branched here): continue after the call.
		AppendSynthesized(linked, {EncodeSoppBranch(SoppBranch, end_pc, return_pc)}, end_pc,
		                  Decoder::Opcode::S_BRANCH, return_pc);
		function_pc = end_pc + 4u;
		for (size_t k = 0; k < targets.size(); ++k) {
			const auto return_to = chain_pc + static_cast<uint32_t>(k) * EntryBytes + 28u; // the s_mov
			EXIT_IF(append_body(site, targets[k] & ~uint64_t {3}, return_to) != body_pcs[k]);
		}
		// The native call jumps to the dispatcher instead.
		const auto jump = EncodeSoppBranch(SoppBranch, call.pc, chain_pc);
		const std::vector<uint32_t> jump_code {jump};
		Decoder::Instruction branch;
		Decoder::DecodeInstruction(jump_code, 0u, branch);
		EXIT_IF(branch.opcode != Decoder::Opcode::S_BRANCH);
		branch.pc            = call.pc;
		branch.branch_target = chain_pc;
		linked.decoded.instructions[site.instruction] = branch;
		EXIT_IF(call.pc / 4u >= linked.code.size());
		linked.code[call.pc / 4u] = jump;
	}
	// Record each distinct body as a source read, so a later write to the leaf code (or the
	// table, read above) invalidates this link.
	reads.resize(query_reads);
	for (size_t i = 0; i < linked.callees.size(); ++i) {
		const auto preceding = std::span(linked.callees).first(i);
		if (std::ranges::find(preceding, linked.callees[i]) == preceding.end())
			reads.emplace_back(linked.callees[i], linked.functions[i].size * sizeof(uint32_t));
	}
	linked.tables.clear();
	for (const auto& site: source.calls) linked.tables.push_back(site.table_targets);
	linked.decoded.code = linked.code;
	if (!linked.functions.empty())
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

namespace {
// S_GETPC_B64 in the native caller yields shader base + pc, a real guest address (embedded constant data).
// Only PCs inside linked leaves or call returns are synthetic.
bool IsNativePcRead(const IR::Inst& base, uint32_t native_bytes) {
	if (native_bytes == 0u) return false;
	// Each use must be "base + immediate" with the immediate inside the native code: the
	// translator emits exactly that for S_GETPC_B64 (pc + 4). Return addresses written by linked
	// SWAPPCs and pcs inside appended bodies are beyond native_bytes.
	for (const auto& use: base.Uses()) {
		const auto* user = use.user;
		if (user->GetOpcode() != IR::ValueOpcode::IAdd64 || !user->Arg(1).IsImmediate() ||
		    use.operand != 0u || user->Arg(1).U64() > native_bytes)
			return false;
	}
	return true;
}
} // namespace

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

	const auto compile_begin = std::chrono::steady_clock::now();
	const auto phase_ms = [&compile_begin]() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::steady_clock::now() - compile_begin).count());
	};

	std::string decoded_dump;
	if (options.dump_ir) {
		decoded_dump = Decoder::ProgramToString(decoded);
		if (options.early_dump) {
			LOGF("%s decoded RDNA2 (early):\n%s", GetDumpLabel(options), decoded_dump.c_str());
		}
	}

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph\n", GetDumpLabel(options),
	     StageName(options.stage), options.shader_hash);
	auto native_cfg = CFG::BuildGraph(decoded);
	CFG::Graph structured_cfg;
	auto* selected_cfg = &native_cfg;
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph blocks=%" PRIu64
	     " loops=%" PRIu64 " back_edges=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(native_cfg.blocks.size()),
	     static_cast<uint64_t>(native_cfg.natural_loops.size()),
	     static_cast<uint64_t>(native_cfg.back_edges.size()), phase_ms());
	if (native_cfg.irreducible) {
		LogDispatcherFallback(options, native_cfg, "build");
	} else {
		LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG Structurize\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
		structured_cfg = CFG::Structurize(native_cfg);
		if (structured_cfg.unsupported) {
			native_cfg.unsupported = true;
			native_cfg.failure_kind = structured_cfg.failure_kind;
			native_cfg.failure_block = structured_cfg.failure_block;
			native_cfg.unsupported_reason = structured_cfg.unsupported_reason;
			LogDispatcherFallback(options, native_cfg, "structurize");
		} else {
			selected_cfg = &structured_cfg;
			LOGF("%s structured CFG success: blocks=%" PRIu64 "\n", GetDumpLabel(options),
			     static_cast<uint64_t>(selected_cfg->blocks.size()));
		}
		LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " CFG Structurize blocks=%" PRIu64
		     " loops=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
		     static_cast<uint64_t>(selected_cfg->blocks.size()),
		     static_cast<uint64_t>(selected_cfg->natural_loops.size()), phase_ms());
	}

	const auto& cfg = *selected_cfg;
	Frontend::EmbeddedFetchPlan embedded_fetch;
	if ((options.stage == ShaderType::Vertex || options.stage == ShaderType::Local) &&
	    options.input_info.vertex->fetch_embedded) {
		embedded_fetch = DetectEmbeddedVertexFetch(
		    decoded, *options.input_info.vertex, options.user_data_base,
		    static_cast<uint32_t>(options.user_data.size()), options.wave_size);
		if (!embedded_fetch.loads.empty()) {
			LOGF("%s embedded vertex fetch plan: detected=%" PRIu64 "\n", GetDumpLabel(options),
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
	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
	auto ir = Frontend::TranslateProgram(decoded, cfg, translate_options);
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram blocks=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(ir.blocks.size()), phase_ms());
	IR::RewriteToSsa(ir.blocks);
	const auto simplify = [&ir] {
		IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
		IR::RemoveIdentities(ir);
		IR::EliminateDeadCode(ir.blocks);
	};
	simplify();
	const auto read_lane_stats = IR::EliminateReadLane(ir, ir.wave_size);
	if (read_lane_stats.rewritten_reads != 0 || read_lane_stats.rewritten_reductions != 0) {
		LOGF("%s read-lane elimination: reads=%" PRIu32 " reductions=%" PRIu32 "\n",
		     GetDumpLabel(options), read_lane_stats.rewritten_reads, read_lane_stats.rewritten_reductions);
	}
	// Cleanup also exposes profitable folds by removing dead predicate consumers.
	simplify();
	if (decoded.has_swap_pc) {
		// Leaf returns are resolved CFG edges; native PC values must not escape into GPU data.
		for (const auto* block: ir.blocks)
			for (const auto& inst: *block)
				EXIT_NOT_IMPLEMENTED(inst.GetOpcode() == IR::ValueOpcode::GetShaderBase &&
				                     !IsNativePcRead(inst, decoded.native_bytes));
	}
	LowerTessellationMemory(ir, options);
	std::string cfg_dump;
	if (options.dump_ir) {
		cfg_dump = CFG::GraphToString(cfg);
		if (options.early_dump) {
			LOGF("%s native IR before resource tracking:\n%s", GetDumpLabel(options),
			     MakeIrDump(cfg_dump, ir).c_str());
		}
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
		if (options.early_dump) {
			LOGF("%s native IR (early):\n%s", GetDumpLabel(options), ir_dump.c_str());
		}
	}

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram\n",
	     GetDumpLabel(options), StageName(ir.stage), ir.shader_hash);
	auto spirv = Spirv::EmitProgram(ir, options.input_info, push_data_start_dword, *options.profile,
	                                options.emit_debug_info);
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram words=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(ir.stage), ir.shader_hash,
	     static_cast<uint64_t>(spirv.size()),
	     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                               std::chrono::steady_clock::now() - emit_begin)
	                               .count()));
	CompileResult result;
	result.spirv   = std::move(spirv);
	result.program = std::move(ir);
	if (options.dump_ir) {
		result.decoded_dump = std::move(translated.decoded_dump);
		result.ir_dump      = std::move(ir_dump);
	}
	return result;
}

} // namespace Libs::Graphics::ShaderRecompiler
