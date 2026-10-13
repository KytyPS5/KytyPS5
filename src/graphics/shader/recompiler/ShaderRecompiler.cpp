#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include "common/assert.h"
#include "common/logging/log.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include "graphics/shader/recompiler/Tessellation.h"
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
#include <cstdio>
#include <fmt/format.h>
#include <fmt/printf.h>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler {

namespace {

// Helper per accesso sicuro agli std::span
template <typename T>
constexpr auto safe_at(std::span<T> s, size_t index,
                       typename std::span<T>::value_type default_value = {}) {
	if (index < s.size()) {
		return s[index];
	}
	return default_value;
}

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

void LogDispatcherFallback(const CompileOptions& options, const CFG::Graph& cfg,
                           const char* phase) {
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

enum class EmbeddedFetchValueType { Unknown, Constant, AttribTable, Attrib, BufferTable, Buffer };

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

Frontend::EmbeddedFetchPlan
DetectEmbeddedVertexFetch(const Decoder::Program& decoded, const ShaderVertexInputInfo* input_info,
                          uint32_t user_data_base, uint32_t user_data_count, uint32_t wave_size) {
	const uint32_t vertex_index_reg   = input_info->logical_stage == ShaderType::Local ? 2u : 5u;
	const uint32_t instance_index_reg = input_info->logical_stage == ShaderType::Local ? 5u : 8u;
	Frontend::EmbeddedFetchPlan data;
	data.loads.reserve(input_info->resources_num);
	int32_t vertex_offset_candidate   = -1;
	int32_t instance_offset_candidate = -1;
	bool    vertex_offset_conflict    = false;
	bool    instance_offset_conflict  = false;

	const int shift_regs = 8;
	const int attrib_reg = input_info->fetch_attrib_reg + shift_regs;
	const int buffer_reg = input_info->fetch_buffer_reg + shift_regs;

	std::array<EmbeddedFetchSgprInfo, 108> sgprs {};
	std::array<bool, 256>                  vgpr_is_index {};
	EmbeddedFetchVectorLanes               vector_lanes;
	const bool                             track_vector_lanes = std::none_of(
	    decoded.instructions.begin(), decoded.instructions.end(), [](const auto& inst) {
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
				auto& candidate =
				    vertex_index_accumulator ? vertex_offset_candidate : instance_offset_candidate;
				auto& conflict =
				    vertex_index_accumulator ? vertex_offset_conflict : instance_offset_conflict;
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
								auto& dst     = sgprs[register_id + i];
								dst.type      = EmbeddedFetchValueType::Attrib;
								dst.attrib_id = index + static_cast<int>(i);
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
								auto& dst     = sgprs[register_id + i];
								dst.type      = EmbeddedFetchValueType::Buffer;
								dst.attrib_id = sgprs[DecodedSgprReg(inst.src1)].attrib_id;
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
					    vgpr_is_index[inst.src0.reg] && IsDecodedSgpr(inst.src1) &&
					    DecodedSgprReg(inst.src1) < sgprs.size() &&
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
						auto& load      = data.loads.emplace_back();
						load.pc         = inst.pc;
						load.attrib_id  = buffer.attrib_id;
						load.components = DecodedDstSize(inst);
					}
				}
				break;
		}
		if (inst.opcode == Decoder::Opcode::V_MOVRELD_B32) {
			vector_lanes.clear();
		} else if (inst.opcode != Decoder::Opcode::V_WRITELANE_B32 && IsDecodedVgpr(inst.dst)) {
			for (uint32_t i = 0;
			     i < EmbeddedFetchDstSize(inst) && inst.dst.reg + i < vgpr_is_index.size(); i++) {
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

// ---- Helpers for S_SWAPPC_B64 diagnostics ----

bool TableScalarCode(const Decoder::Operand& op, uint32_t& code) {
	switch (op.kind) {
		case Decoder::OperandKind::Sgpr: code = op.reg; return true;
		case Decoder::OperandKind::VccLo: code = 106u; return true;
		case Decoder::OperandKind::VccHi: code = 107u; return true;
		case Decoder::OperandKind::M0: code = 124u; return true;
		default: return false;
	}
}

bool IsTableLoad(Decoder::Opcode op) {
	return op == Decoder::Opcode::S_BUFFER_LOAD_DWORD ||
	       op == Decoder::Opcode::S_BUFFER_LOAD_DWORDX2 ||
	       op == Decoder::Opcode::S_BUFFER_LOAD_DWORDX4 ||
	       op == Decoder::Opcode::S_BUFFER_LOAD_DWORDX8 ||
	       op == Decoder::Opcode::S_BUFFER_LOAD_DWORDX16;
}

// Risale s[reg] dentro il blocco della call, attraverso S_MOV_B32, fino a una S_BUFFER_LOAD_DWORD*.
bool FindTableLoad(const Decoder::Program& program, const CFG::Graph& graph, uint32_t call_index,
                   uint32_t reg, uint32_t& load_index) {
	const auto block = std::ranges::find_if(graph.blocks, [&](const auto& b) {
		return b.inst_begin <= call_index && call_index < b.inst_end;
	});
	if (block == graph.blocks.end()) return false;
	uint32_t code = reg;
	for (uint32_t i = call_index; i > block->inst_begin;) {
		const uint32_t inst_index = --i;
		// Bounds check for instructions array access
		if (inst_index >= program.instructions.size()) return false;
		const auto& inst = program.instructions[inst_index];
		if (!CFG::MayWriteScalarRegister(inst, code)) continue;
		uint32_t dst = 0;
		if (inst.opcode == Decoder::Opcode::S_MOV_B32 && TableScalarCode(inst.dst, dst) &&
		    dst == code && inst.src0.kind == Decoder::OperandKind::Sgpr) {
			code = inst.src0.reg;
			continue;
		}
		if (IsTableLoad(inst.opcode) && TableScalarCode(inst.dst, dst) && code >= dst &&
		    code < dst + std::max(1u, inst.data_dwords)) {
			load_index = i;
			return true;
		}
		return false;
	}
	return false;
}

// Il registro-selettore non deve essere riscritto tra la load e la call.
bool SelectorUnchanged(const Decoder::Program& program, uint32_t load_index, uint32_t call_index,
                       uint32_t code) {
	for (uint32_t i = load_index + 1u; i < call_index; ++i) {
		// Bounds check for instructions array access
		if (i >= program.instructions.size()) return false;
		if (CFG::MayWriteScalarRegister(program.instructions[i], code)) return false;
	}
	return true;
}

// Raccoglie i ReadFirstLane non validi (candidati a variabile libera).
void CollectFreeSelectors(const IR::ResourcePlan& plan, IR::Value value,
                          std::set<const IR::Inst*>& seen, std::vector<const IR::Inst*>& found) {
	value            = value.Resolve();
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || !seen.insert(inst).second) return;
	if (IR::ValidateRuntimeValue(plan, value, IR::RuntimeValueType::Integer)) return;
	if (inst->GetOpcode() == IR::ValueOpcode::ReadFirstLane) {
		found.push_back(inst);
		return;
	}
	for (size_t i = 0; i < inst->NumArgs(); ++i)
		CollectFreeSelectors(plan, inst->Arg(i), seen, found);
}

// Returns a human-readable name for a Decoder::Opcode via magic_enum.
// This covers every opcode automatically without a manual switch.
static std::string SwappcOpcodeStr(Decoder::Opcode op) {
	auto name = magic_enum::enum_name(op);
	if (!name.empty()) return std::string(name);
	return "opcode_" + std::to_string(static_cast<int>(op));
}

// Returns a human-readable string for a decoded Operand (kind + value).
static std::string DumpOperand(const Decoder::Operand& op) {
	switch (op.kind) {
		case Decoder::OperandKind::Sgpr: return "s" + std::to_string(op.reg);
		case Decoder::OperandKind::Vgpr: return "v" + std::to_string(op.reg);
		case Decoder::OperandKind::VccLo: return "vcc_lo";
		case Decoder::OperandKind::VccHi: return "vcc_hi";
		case Decoder::OperandKind::ExecLo: return "exec_lo";
		case Decoder::OperandKind::ExecHi: return "exec_hi";
		case Decoder::OperandKind::Scc: return "scc";
		case Decoder::OperandKind::M0: return "m0";
		case Decoder::OperandKind::Null: return "null";
		case Decoder::OperandKind::VccZ: return "vccz";
		case Decoder::OperandKind::ExecZ: return "execz";
		case Decoder::OperandKind::SharedBase: return "shared_base";
		case Decoder::OperandKind::PrivateBase: return "private_base";
		case Decoder::OperandKind::PopsExitingWaveId: return "pops_exiting";
		case Decoder::OperandKind::LiteralConstant:
			return "lit:0x" + [&] {
				char b[16];
				std::snprintf(b, sizeof(b), "%08x", op.value);
				return std::string(b);
			}();
		case Decoder::OperandKind::IntegerInlineConstant:
			return "imm:" + std::to_string(op.signed_val);
		case Decoder::OperandKind::FloatInlineConstant: return "fimm:" + std::to_string(op.value);
		default: return "?";
	}
}

// SWAPPC diagnostics must go through Log::WriteToConsoleAndLog. Raw stdout prints never
// reach _kyty.txt (that file is filled by LOGF / Log::Write when printf_direction=File).
struct SwappcDiag {
	bool        enabled = false;
	std::string pending;

	explicit SwappcDiag(bool on): enabled(on) {}
	SwappcDiag(const SwappcDiag&)            = delete;
	SwappcDiag& operator=(const SwappcDiag&) = delete;

	template <typename... Args>
	void operator()(const Args&... args) {
		if (!enabled) return;
		pending += ::fmt::sprintf(args...);
		for (;;) {
			const auto pos = pending.find('\n');
			if (pos == std::string::npos) break;
			::Log::WriteToConsoleAndLog(pending.substr(0, pos + 1));
			pending.erase(0, pos + 1);
		}
	}

	~SwappcDiag() {
		if (enabled && !pending.empty()) {
			::Log::WriteToConsoleAndLog(pending);
			pending.clear();
		}
	}
};

void PrepareCallTarget(ShaderSource& source, const CompileOptions& options) {
	using namespace IR;
	SwappcDiag diag(options.diag_swappc);
	source.calls.clear();
	for (uint32_t i = 0; i < source.decoded.instructions.size(); ++i) {
		// Bounds check is implicit in the loop condition, but explicit check for safety
		if (i >= source.decoded.instructions.size()) break;
		if (source.decoded.instructions[i].opcode != Decoder::Opcode::S_SWAPPC_B64) continue;
		source.calls.push_back(ShaderSource::Call {.instruction = i});
		diag("S_SWAPPC_B64 @%u (stage=%d, total so far=%zu)\n", i, (int)options.stage,
		     source.calls.size());
		EXIT_NOT_IMPLEMENTED(options.stage != ShaderType::Compute);
	}
	if (source.calls.empty()) return;
	diag("=== PrepareCallTarget decoded RDNA2 (%zu S_SWAPPC_B64, stage=%d, hash=0x%016" PRIx64
	     ") ===\n%s",
	     source.calls.size(), (int)options.stage, options.shader_hash,
	     Decoder::ProgramToString(source.decoded).c_str());
	{
		const auto code = source.decoded.code;
		diag("--- raw words (%zu dwords) ---\n", code.size());
		for (size_t w = 0; w < code.size(); ++w) {
			if (w % 8u == 0u) diag(" +%04zx:", w * 4u);
			diag(" %08x", code[w]);
			if (w % 8u == 7u || w + 1u == code.size()) diag("\n");
		}
	}
	EXIT_IF(options.input_info.compute == nullptr);
	const auto graph = CFG::BuildGraph(source.decoded);

	// --- DIAGNOSTIC: CFG block dump ---
	if (options.diag_swappc) {
		diag("CFG: %zu blocks, entry=%u\n", graph.blocks.size(), (unsigned)graph.entry_block);
		for (const auto& b: graph.blocks) {
			diag("  block id=%u [%u,%u) preds=", (unsigned)b.id, b.inst_begin, b.inst_end);
			for (auto p: b.predecessors)
				diag(" %u", (unsigned)p);
			diag("\n");
		}
	}

	// DumpInst: prints a single decoded instruction with opcode name and full operands.
	// For immediates (LiteralConstant / InlineConstant), the actual value is shown.
	auto DumpInst = [&](uint32_t k) {
		// Bounds check for instructions array access
		if (k >= source.decoded.instructions.size()) {
			diag("   %5u: <out of bounds>\n", k);
			return;
		}
		const auto& x        = source.decoded.instructions[k];
		const auto  op_name  = SwappcOpcodeStr(x.opcode);
		const auto  dst_str  = DumpOperand(x.dst);
		const auto  src0_str = DumpOperand(x.src0);
		const auto  src1_str = DumpOperand(x.src1);
		diag("   %5u: %-30s  dst=%-12s  src0=%-12s  src1=%s\n", k, op_name.c_str(), dst_str.c_str(),
		     src0_str.c_str(), src1_str.c_str());

		// Print special operand details for certain opcodes
		if (x.opcode == Decoder::Opcode::S_BUFFER_LOAD_DWORD ||
		    x.opcode == Decoder::Opcode::S_BUFFER_LOAD_DWORDX2 ||
		    x.opcode == Decoder::Opcode::S_BUFFER_LOAD_DWORDX4 ||
		    x.opcode == Decoder::Opcode::S_BUFFER_LOAD_DWORDX8 ||
		    x.opcode == Decoder::Opcode::S_BUFFER_LOAD_DWORDX16) {
			diag("         S_BUFFER_LOAD src1: kind=%d reg=%u", static_cast<int>(x.src1.kind),
			     x.src1.reg);
			if (x.src1.kind == Decoder::OperandKind::LiteralConstant) {
				diag(" lit=0x%08x", x.src1.value);
			} else if (x.src1.kind == Decoder::OperandKind::IntegerInlineConstant) {
				diag(" imm=%d", x.src1.signed_val);
			}
			diag("\n");
		} else if (x.opcode == Decoder::Opcode::S_LSHL_B32) {
			diag("         S_LSHL_B32: ");
			if (x.src1.kind == Decoder::OperandKind::LiteralConstant) {
				diag("shift imm=0x%08x / %u\n", x.src1.value, x.src1.value);
			} else if (x.src1.kind == Decoder::OperandKind::IntegerInlineConstant) {
				diag("shift imm=%d\n", x.src1.signed_val);
			} else {
				diag("shift src1 kind=%d reg=%u\n", (int)x.src1.kind, x.src1.reg);
			}
		}
	};

	Program query;
	query.stage           = options.stage;
	query.user_data_base  = options.user_data_base;
	query.user_data_count = static_cast<uint32_t>(options.user_data.size());
	query.wave_size       = options.wave_size;
	auto& block           = *query.block_storage.emplace_back(std::make_unique<Block>());
	query.blocks.push_back(&block);

	struct Producer {
		bool                      ready = false;
		std::map<uint32_t, Value> values;
	};
	std::map<uint32_t, Producer>             producers;
	std::function<Value(uint32_t, uint32_t)> Register;
	std::function<Producer&(uint32_t)>       EnsureProducer;
	std::function<Value(uint32_t, Value)>    Resolve = [&](uint32_t before, Value value) -> Value {
		value      = value.Resolve();
		auto* inst = value.TryInstruction();
		if (inst == nullptr) return value;
		uint32_t code = UINT32_MAX;
		switch (inst->GetOpcode()) {
			case ValueOpcode::GetScalarRegister:
				code = static_cast<uint32_t>(inst->Arg(0).ScalarRegister());
				break;
			case ValueOpcode::GetVccLo: code = 106u; break;
			case ValueOpcode::GetVccHi: code = 107u; break;
			default: break;
		}
		if (code != UINT32_MAX) {
			const auto result = Register(before, code);
			inst->ReplaceUsesWith(result);
			return result;
		}
		for (size_t i = 0; i < inst->NumArgs(); ++i)
			inst->SetArg(i, Resolve(before, inst->Arg(i)));
		return value;
	};

	EnsureProducer = [&](uint32_t index) -> Producer& {
		auto [entry, inserted] = producers.try_emplace(index);
		auto& producer         = entry->second;
		if (inserted) {
			// Bounds check for instructions array access
			if (index >= source.decoded.instructions.size()) {
				EXIT("instruction index %u out of bounds (size=%zu)", index,
				     source.decoded.instructions.size());
			}
			EXIT_NOT_IMPLEMENTED(source.decoded.instructions[index].opcode ==
			                     Decoder::Opcode::S_SWAPPC_B64);
			Block                translated;
			Frontend::Translator translator(query, &translated, 1u,
			                                (options.input_info.compute->float_mode & 0x10u) == 0u,
			                                !options.input_info.compute->async_compute);
			translator.TranslateInstruction(source.decoded.instructions[index]);
			for (const auto& inst: translated) {
				uint32_t destination = UINT32_MAX;
				Value    value;
				switch (inst.GetOpcode()) {
					case ValueOpcode::SetScalarRegister:
						destination = static_cast<uint32_t>(inst.Arg(0).ScalarRegister());
						value       = inst.Arg(1);
						break;
					case ValueOpcode::SetVccLo:
						destination = 106u;
						value       = inst.Arg(0);
						break;
					case ValueOpcode::SetVccHi:
						destination = 107u;
						value       = inst.Arg(0);
						break;
					default: break;
				}
				if (destination != UINT32_MAX) producer.values.insert_or_assign(destination, value);
			}
			for (auto& [destination, value]: producer.values)
				value = Resolve(index, value);
			for (auto& inst: translated)
				inst.SetParent(&block);
			block.Instructions().splice(block.end(), translated.Instructions());
			producer.ready = true;
		}
		return producer;
	};

	Register = [&](uint32_t before, uint32_t code) -> Value {
		const auto index =
		    CFG::FindScalarDefinition(source.decoded, graph, before, code, [&](uint32_t i) {
			    const auto& p = EnsureProducer(i);
			    EXIT_NOT_IMPLEMENTED(!p.ready); // circular dependency
			    return p.values.contains(code);
		    });
		if (index == UINT32_MAX) {
			EXIT_IF(code < query.user_data_base ||
			        code - query.user_data_base >= query.user_data_count);
			return IREmitter(&block).GetUserData(static_cast<ScalarReg>(code));
		}
		auto& producer = EnsureProducer(index);
		EXIT_IF(!producer.ready || !producer.values.contains(code));
		return producer.values.at(code).Resolve();
	};

	// Summary table accumulated across all calls.
	struct CallSummary {
		uint32_t    instr_idx;
		uint32_t    dst_reg;
		uint32_t    src_reg;
		uint32_t    wave_size;
		bool        valid_lo;
		bool        valid_hi;
		const char* classification;
	};
	std::vector<CallSummary> call_summaries;
	bool                     all_valid = true;

	for (auto& c: source.calls) {
		const auto& call = source.decoded.instructions[c.instruction];

		// --- DIAGNOSTIC: call context header ---
		if (options.diag_swappc) {
			diag("=== CALL @%u: dst=(kind=%d,reg=%u) src0=(kind=%d,reg=%u) wave_size=%u\n",
			     c.instruction, (int)call.dst.kind, (unsigned)call.dst.reg, (int)call.src0.kind,
			     (unsigned)call.src0.reg, (unsigned)options.wave_size);
			// Print 14 instructions before the call (inclusive of the call itself)
			const uint32_t ctx_start = (c.instruction >= 14u ? c.instruction - 14u : 0u);
			for (uint32_t k = ctx_start; k <= c.instruction; ++k)
				DumpInst(k);
		}

		EXIT_NOT_IMPLEMENTED(call.src0.kind != Decoder::OperandKind::Sgpr ||
		                     call.dst.kind != Decoder::OperandKind::Sgpr || call.src0.reg >= 105u ||
		                     call.dst.reg >= 105u);

		// --- Riconoscimento "call via tabella" ---
		const IR::Inst* table_selector = nullptr;
		IR::Value       table_offset;
		uint32_t        table_sgpr = UINT32_MAX;
		{
			std::set<const IR::Inst*>    seen;
			std::vector<const IR::Inst*> found;
			const auto                   lo = Register(c.instruction, call.src0.reg);
			const auto                   hi = Register(c.instruction, call.src0.reg + 1u);
			CollectFreeSelectors(query, lo, seen, found);
			CollectFreeSelectors(query, hi, seen, found);
			const auto* lo_top  = lo.Resolve().TryInstruction();
			const auto* hi_top  = hi.Resolve().TryInstruction();
			uint32_t    load_lo = 0, load_hi = 0, sel = 0;
			if (found.size() == 1u && lo_top != nullptr && hi_top != nullptr &&
			    lo_top->GetOpcode() == ValueOpcode::ReadConstBuffer &&
			    hi_top->GetOpcode() == ValueOpcode::ReadConstBuffer && lo_top->NumArgs() == 2u &&
			    hi_top->NumArgs() == 2u && lo_top->Arg(1).Resolve() == hi_top->Arg(1).Resolve() &&
			    FindTableLoad(source.decoded, graph, c.instruction, call.src0.reg, load_lo) &&
			    FindTableLoad(source.decoded, graph, c.instruction, call.src0.reg + 1u, load_hi) &&
			    load_lo == load_hi &&
			    // Bounds check for load_lo before accessing instructions
			    (load_lo < source.decoded.instructions.size() &&
			     TableScalarCode(source.decoded.instructions[load_lo].src1, sel)) &&
			    SelectorUnchanged(source.decoded, load_lo, c.instruction, sel) &&
			    ValidateRuntimeValue(query, lo, RuntimeValueType::Integer, found[0]) &&
			    ValidateRuntimeValue(query, hi, RuntimeValueType::Integer, found[0])) {
				table_selector = found[0];
				table_offset   = lo_top->Arg(1).Resolve();
				table_sgpr     = sel;
			}
			if (options.diag_swappc) {
				diag("  table-call detection: free selectors=%zu selector=%p sgpr=%u -> %s\n",
				     found.size(), static_cast<const void*>(table_selector), table_sgpr,
				     table_selector != nullptr ? "OK" : "no");
			}
		}

		// DumpValue: recursive IR tree dump up to depth 12.
		// Safe version: only prints opcode and arguments, never queries the value directly.
		auto PrintLeaf = [&](const IR::Value& v) {
			if (auto* i = v.TryInstruction()) {
				diag("<inst %d>", static_cast<int>(i->GetOpcode()));
				return;
			}
			switch (v.GetType()) {
				case IR::Type::U32: diag("U32(0x%08x)", v.U32()); break;
				case IR::Type::U64: diag("U64(0x%016" PRIx64 ")", v.U64()); break;
				case IR::Type::U1: diag("U1(%d)", static_cast<int>(v.U1())); break;
				case IR::Type::U8: diag("U8(0x%02x)", v.U8()); break;
				case IR::Type::U16: diag("U16(0x%04x)", v.U16()); break;
				case IR::Type::ScalarReg:
					diag("s%u", static_cast<uint32_t>(IR::RegIndex(v.ScalarRegister())));
					break;
				case IR::Type::VectorReg:
					diag("v%u", static_cast<uint32_t>(IR::RegIndex(v.VectorRegister())));
					break;
				default: diag("<type %d>", static_cast<int>(v.GetType())); break;
			}
		};

		std::function<void(Value, int)> DumpValue = [&](Value v, int depth) {
			v          = v.Resolve();
			auto* inst = v.TryInstruction();
			if (inst == nullptr) {
				diag("%*s", depth * 2, "");
				PrintLeaf(v);
				diag("\n");
				return;
			}
			// For GetUserData, show which scalar register it reads from Arg(0).
			if (inst->GetOpcode() == ValueOpcode::GetUserData && inst->NumArgs() == 1) {
				const auto sr = inst->Arg(0).Resolve();
				diag("%*sGetUserData(", depth * 2, "");
				PrintLeaf(sr);
				diag(")\n");
				return;
			}
			// For GetVectorRegister, show the register from Arg(0).
			if (inst->GetOpcode() == ValueOpcode::GetVectorRegister && inst->NumArgs() == 1) {
				const auto sr = inst->Arg(0).Resolve();
				diag("%*sGetVectorRegister(", depth * 2, "");
				PrintLeaf(sr);
				diag(")\n");
				return;
			}
			// For memory ops, print the MemoryFlags index.
			const auto op = inst->GetOpcode();
			if (op == ValueOpcode::LoadAddressU32 || op == ValueOpcode::ReadConstBuffer ||
			    op == ValueOpcode::LoadBufferU32) {
				const auto mflags = inst->Flags<MemoryFlags>();
				diag("%*s%s nargs=%zu mem_idx=%u mem_pc=0x%x", depth * 2, "",
				     IR::ValueOpcodeName(op).data(), inst->NumArgs(), mflags.index, mflags.pc);
				if (inst->NumArgs() > 0) {
					diag(" args[0]=");
					PrintLeaf(inst->Arg(0).Resolve());
				}
				if (inst->NumArgs() > 1) {
					diag(" args[1]=");
					PrintLeaf(inst->Arg(1).Resolve());
				}
				diag("\n");
			} else if (op == ValueOpcode::BitwiseAnd32 && inst->NumArgs() == 2) {
				diag("%*sBitwiseAnd32(", depth * 2, "");
				PrintLeaf(inst->Arg(0).Resolve());
				diag(", ");
				PrintLeaf(inst->Arg(1).Resolve());
				diag(")\n");
			} else if (op == ValueOpcode::ShiftLeftLogical32 && inst->NumArgs() == 2) {
				diag("%*sShiftLeftLogical32(", depth * 2, "");
				PrintLeaf(inst->Arg(0).Resolve());
				diag(", ");
				PrintLeaf(inst->Arg(1).Resolve());
				diag(")\n");
			} else {
				diag("%*s%s nargs=%zu", depth * 2, "", IR::ValueOpcodeName(op).data(),
				     inst->NumArgs());
				for (size_t i = 0; i < inst->NumArgs(); ++i) {
					diag(" args[%zu]=", i);
					PrintLeaf(inst->Arg(i).Resolve());
				}
				diag("\n");
			}
			if (depth < 12) {
				for (size_t i = 0; i < inst->NumArgs(); ++i) {
					DumpValue(inst->Arg(i), depth + 1);
				}
			}
		};

		// Print the full CFG block that contains this call, and its immediate successors.
		if (options.diag_swappc) {
			const CFG::BasicBlock* call_block = nullptr;
			for (const auto& b: graph.blocks) {
				if (c.instruction >= b.inst_begin && c.instruction < b.inst_end) {
					call_block = &b;
					break;
				}
			}
			if (call_block != nullptr) {
				diag("  CFG block id=%u [%u,%u) containing call @%u:\n", call_block->id,
				     call_block->inst_begin, call_block->inst_end, c.instruction);
				for (uint32_t k = call_block->inst_begin; k < call_block->inst_end; ++k) {
					// Bounds check is implicit in loop range, but explicit check for safety
					if (k >= source.decoded.instructions.size()) break;
					DumpInst(k);
				}
				diag("  Successors of block %u:", call_block->id);
				for (auto s: call_block->successors) {
					diag(" %u", s);
					const auto* sb = graph.FindBlock(s);
					if (sb != nullptr) {
						diag("([%u,%u))", sb->inst_begin, sb->inst_end);
					}
				}
				diag("\n");

				// Print back-edge target block if any
				for (const auto& be: graph.back_edges) {
					if (be.to == call_block->id) {
						const auto* back_block = graph.FindBlock(be.from);
						if (back_block != nullptr) {
							diag("  Back-edge from block %u [%u,%u):\n", back_block->id,
							     back_block->inst_begin, back_block->inst_end);
							for (uint32_t k = back_block->inst_begin; k < back_block->inst_end;
							     ++k) {
								// Bounds check for instructions array access
								if (k >= source.decoded.instructions.size()) break;
								DumpInst(k);
							}
						}
					}
				}
			} else {
				diag("  call @%u not found in any CFG block\n", c.instruction);
			}

			// If options.user_data is non-empty, print its contents.
			// user_data[i] holds the runtime value of s(user_data_base + i).
			if (!options.user_data.empty()) {
				diag("  user_data[0..%zu) = ", options.user_data.size());
				for (size_t ud = 0; ud < options.user_data.size(); ++ud)
					diag("%s0x%08x", ud ? " " : "", options.user_data[ud]);
				diag("\n");
			}
		}

		// Classify based on IR opcode of the resolved src0 value.
		// We peek at the lo-half before committing to targets.
		const char* classification = "ALTRO";
		bool        valid_lo       = false;
		bool        valid_hi       = false;
		bool        call_ok        = true;

		for (uint32_t i = 0; i < 2u; ++i) {
			const auto value = Register(c.instruction, call.src0.reg + i);
			const bool valid =
			    ValidateRuntimeValue(query, value, RuntimeValueType::Integer, table_selector);
			if (i == 0)
				valid_lo = valid;
			else
				valid_hi = valid;

			// --- DIAGNOSTIC: ValidateRuntimeValue result per half ---
			if (options.diag_swappc) {
				diag("  ValidateRuntimeValue(s%u)=%s\n", call.src0.reg + i,
				     valid ? "true" : "false");
				diag("  IR tree for s%u:\n", call.src0.reg + i);
				DumpValue(value, 2);
			}

			// Guard stub: check for sentinel values in the register value
			const auto resolved = value.Resolve();
			if (resolved.IsImmediate()) {
				uint64_t imm_value = 0;
				switch (resolved.GetType()) {
					case IR::Type::U32: imm_value = resolved.U32(); break;
					case IR::Type::U64: imm_value = resolved.U64(); break;
					default: break;
				}
				// Check for NULL pointer
				if (imm_value == 0x0000000000000000ull) {
					if (options.diag_swappc) {
						diag("  GUARD: NULL pointer detected in s%u - will use guard stub\n",
						     call.src0.reg + i);
					}
					call_ok = false;
					break;
				}
				// Check for AGC placeholder addresses (0x0020XXXXXXXX)
				if ((imm_value & 0xFFFF000000000000ull) == 0x0020000000000000ull) {
					if (options.diag_swappc) {
						diag("  GUARD: AGC placeholder address detected in s%u (0x%016" PRIx64
						     ") - will use guard stub\n",
						     call.src0.reg + i, imm_value);
					}
					call_ok = false;
					break;
				}
				// Check for sentinel values (32-bit sentinel in low/high halves)
				uint32_t sentinel_check = static_cast<uint32_t>(imm_value);
				if (sentinel_check == 0xFFFFFFFF || sentinel_check == 0xBAADF00D ||
				    sentinel_check == 0xCDCDCDCD || sentinel_check == 0xDEADBEEF) {
					if (options.diag_swappc) {
						diag("  GUARD: Sentinel value detected in s%u (0x%08x) - will use guard "
						     "stub\n",
						     call.src0.reg + i, sentinel_check);
					}
					call_ok = false;
					break;
				}
			}

			// Derive classification from the lo-half IR tree root opcode.
			if (i == 0) {
				const auto  resolved = value.Resolve();
				const auto* top      = resolved.TryInstruction();
				if (top == nullptr) {
					// Immediate / constant folded value.
					classification = "COSTANTE";
				} else {
					const auto top_op = top->GetOpcode();
					if (top_op == ValueOpcode::GetUserData) {
						classification = "SRT-ONLY";
					} else if (top_op == ValueOpcode::ReadConstBuffer ||
					           top_op == ValueOpcode::LoadAddressU32 ||
					           top_op == ValueOpcode::ReadConst ||
					           top_op == ValueOpcode::LoadBufferU32) {
						classification = "TABELLA-INDICIZZATA";
					} else {
						classification = "ALTRO";
					}
				}
			}

			if (!valid) {
				// Do NOT exit here: continue collecting diagnostics for all calls.
				if (options.diag_swappc) {
					diag("  TARGET NOT VALID: call@%u reg=s%u – skipping target assignment\n",
					     c.instruction, call.src0.reg + i);
				}
				call_ok   = false;
				all_valid = false;
				continue; // Skip target assignment for this half.
			}
			auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
			root.SetArg(0, value);
			c.target[i] = Value(&root);
		}

		// Guard stub handling: if the call is invalid due to sentinel values or NULL,
		// replace with a NO-OP or fallback to interpreter
		if (!call_ok) {
			if (options.diag_swappc) {
				diag("  GUARD STUB: call@%u invalid - using guard stub (NO-OP)\n", c.instruction);
			}
			// Mark the call as needing guard stub treatment
			c.guard_stub = true;
			// Set targets to invalid/placeholder values that will be caught later
			c.target[0] = IR::Value {};
			c.target[1] = IR::Value {};
			all_valid   = false;
			call_summaries.push_back({
			    .instr_idx      = c.instruction,
			    .dst_reg        = call.dst.reg,
			    .src_reg        = call.src0.reg,
			    .wave_size      = options.wave_size,
			    .valid_lo       = false,
			    .valid_hi       = false,
			    .classification = "GUARD_STUB",
			});
			continue;
		}

		// If the lo-half holds a table pointer and user_data is available, attempt to
		// evaluate the V# descriptor address from user_data at compile time (host read).
		// This is pure diagnostics: no side effects, no functional change.
		if (options.diag_swappc && !options.user_data.empty()) {
			const auto lo_val = Register(c.instruction, call.src0.reg);
			// Walk the lo-half IR to find a GetUserData leaf and use it as the SRT base.
			std::function<const IR::Inst*(const IR::Value&)> FindUserData =
			    [&](const IR::Value& v) -> const IR::Inst* {
				const auto  rv = v.Resolve();
				const auto* ip = rv.TryInstruction();
				if (ip == nullptr) return nullptr;
				if (ip->GetOpcode() == ValueOpcode::GetUserData) return ip;
				for (size_t a = 0; a < ip->NumArgs(); ++a) {
					const auto* found = FindUserData(ip->Arg(a));
					if (found != nullptr) return found;
				}
				return nullptr;
			};
			const auto* ud_inst = FindUserData(lo_val);
			if (ud_inst != nullptr && ud_inst->NumArgs() == 1 &&
			    ud_inst->Arg(0).Resolve().GetType() == IR::Type::ScalarReg) {
				const auto ud_abs =
				    static_cast<uint32_t>(IR::RegIndex(ud_inst->Arg(0).Resolve().ScalarRegister()));
				if (ud_abs >= query.user_data_base) {
					const auto ud_rel = ud_abs - query.user_data_base;
					// Use safe_at for bounds-checked access
					const uint32_t lo = safe_at(options.user_data, ud_rel, 0u);
					const uint32_t hi = safe_at(options.user_data, ud_rel + 1, 0u);
					const uint64_t base_addr =
					    static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32u);
					diag("  SRT base from s%u:s%u = 0x%016" PRIx64 "\n", ud_abs, ud_abs + 1,
					     base_addr);
					// Attempt direct host read of a 16-byte V# descriptor at base_addr.
					// The emulator maps guest memory directly so a bare pointer cast is valid
					// if base_addr is within the mapped VA window (same approach as
					// SrtReadCapture).
					if (base_addr >= 0x1000ull && base_addr <= 0x0000ffffffffffffull) {
						uint32_t desc[4]  = {};
						bool     readable = false;
#ifdef _WIN32
						if (!IsBadReadPtr(reinterpret_cast<const void*>(base_addr), 16)) {
							std::memcpy(desc, reinterpret_cast<const void*>(base_addr), 16);
							readable = true;
						}
#else
						// On Linux, we assume valid addresses are readable for diagnostic purposes
						std::memcpy(desc, reinterpret_cast<const void*>(base_addr), 16);
						readable = true;
#endif
						if (readable) {
							const uint64_t buf_base =
							    (static_cast<uint64_t>(desc[1] & 0x0000ffffu) << 32u) | desc[0];
							const uint32_t stride_bytes = (desc[1] >> 16u) & 0x3fffu;
							const uint32_t num_records  = desc[2];
							const uint32_t flags        = desc[3];
							diag("  V# at 0x%016" PRIx64 ": base=0x%012" PRIx64
							     " stride=%u bytes num_records=%u flags=0x%08x\n",
							     base_addr, buf_base, stride_bytes, num_records, flags);
							const uint32_t n_entries = num_records;
							diag("  N_entries=%u\n", n_entries);
							// Print first min(n_entries, 16) entry pointers.
							const uint32_t to_print = std::min(n_entries, 16u);
							for (uint32_t e = 0; e < to_print; ++e) {
								const uint64_t entry_addr = buf_base + uint64_t {e} * stride_bytes;
								if ((entry_addr < 0x1000ull) ||
								    (entry_addr > 0x0000ffffffffffffull)) {
									break;
								}
								uint32_t entry[4] = {};
								bool     entry_ok = false;
#ifdef _WIN32
								if (!IsBadReadPtr(reinterpret_cast<const void*>(entry_addr), 16)) {
									std::memcpy(entry, reinterpret_cast<const void*>(entry_addr),
									            16);
									entry_ok = true;
								}
#else
								std::memcpy(entry, reinterpret_cast<const void*>(entry_addr), 16);
								entry_ok = true;
#endif
								if (!entry_ok) {
									diag("  entry[%u]: unreadable\n", e);
									break;
								}
								const uint64_t ptr =
								    (static_cast<uint64_t>(entry[1] & 0x0000ffffu) << 32u) |
								    entry[0];
								diag("  entry[%u]: addr=0x%012" PRIx64
								     " dw0=0x%08x dw1=0x%08x dw2=0x%08x dw3=0x%08x\n",
								     e, ptr, entry[0], entry[1], entry[2], entry[3]);

								if (ptr >= 0x1000ull && ptr <= 0x0000ffffffffffffull) {
									diag(" entry[%u] code @0x%012" PRIx64 ":", e, ptr);
									for (uint32_t w = 0; w < 64u; ++w) {
										uint32_t word = 0;
#ifdef _WIN32
										if (!IsBadReadPtr(
										        reinterpret_cast<const void*>(ptr + w * 4ull), 4)) {
											std::memcpy(
											    &word,
											    reinterpret_cast<const void*>(ptr + w * 4ull), 4);
											if (w % 8 == 0) diag("\n +%03x:", w * 4);
											diag(" %08x", word);
										} else {
											diag(" [unreadable]");
											break;
										}
#else
										std::memcpy(&word,
										            reinterpret_cast<const void*>(ptr + w * 4ull),
										            4);
										if (w % 8 == 0) diag("\n +%03x:", w * 4);
										diag(" %08x", word);
#endif
									}
									diag("\n");
								}
							}
						} else {
							diag("  V# at 0x%016" PRIx64
							     ": host read failed in PrepareCallTarget\n",
							     base_addr);
							diag("  Note: V# becomes readable in RefreshShaderSource via "
							     "SrtReadCapture\n");
						}
					}
				}
			}

			// Extract shift and offset register from IR tree
			std::function<void(const IR::Value&, uint32_t*, uint32_t*)> find_shift_and_offset =
			    [&](const IR::Value& v, uint32_t* shift, uint32_t* offset_reg) {
				    const auto  resolved = v.Resolve();
				    const auto* inst     = resolved.TryInstruction();
				    if (inst == nullptr) return;
				    if (inst->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
				        inst->NumArgs() == 2) {
					    const auto arg1 = inst->Arg(1).Resolve();
					    if (!arg1.TryInstruction() && arg1.GetType() == IR::Type::U32) {
						    *shift = arg1.U32();
					    }
				    }
				    for (size_t i = 0; i < inst->NumArgs(); ++i) {
					    const auto arg = inst->Arg(i).Resolve();
					    if (!arg.TryInstruction() && arg.GetType() == IR::Type::ScalarReg) {
						    *offset_reg = IR::RegIndex(arg.ScalarRegister());
					    }
					    find_shift_and_offset(inst->Arg(i), shift, offset_reg);
				    }
			    };
			uint32_t shift      = UINT32_MAX;
			uint32_t offset_reg = UINT32_MAX;
			find_shift_and_offset(lo_val, &shift, &offset_reg);
			if (shift != UINT32_MAX || offset_reg != UINT32_MAX) {
				diag("  Shift=%u OffsetReg=%s\n", shift,
				     offset_reg != UINT32_MAX ? std::to_string(offset_reg).c_str() : "N/A");
			}
		}

		if (options.diag_swappc) {
			diag("  Classification: %s  call_ok=%s\n", classification, call_ok ? "yes" : "NO");
		}
		if (table_selector != nullptr && call_ok) {
			auto& sel_root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
			sel_root.SetArg(0, Value(const_cast<Inst*>(table_selector)));
			auto& off_root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
			off_root.SetArg(0, table_offset);
			c.selector        = Value(&sel_root);
			c.selector_offset = Value(&off_root);
			c.selector_sgpr   = table_sgpr;
		}
		call_summaries.push_back({c.instruction, call.dst.reg, call.src0.reg, options.wave_size,
		                          valid_lo, valid_hi, classification});
	}

	// Check if all calls were valid - stop before any further processing if not
	if (!all_valid) {
		if (options.diag_swappc) {
			diag("PrepareCallTarget: almeno un target non risolvibile staticamente\n");
		}
		EXIT_NOT_IMPLEMENTED(true);
	}

	RewriteToSsa(query.blocks);
	ConstantPropagationPass(query.blocks, query.wave_size);
	RemoveIdentities(query.blocks);
	EliminateDeadCode(query.blocks);
	for (auto& c: source.calls) {
		for (auto& value: c.target) {
			EXIT_IF(value.IsEmpty());
			value = value.Instruction()->Arg(0).Resolve();
		}
		if (!c.selector.IsEmpty()) {
			c.selector        = c.selector.Instruction()->Arg(0).Resolve();
			c.selector_offset = c.selector_offset.Instruction()->Arg(0).Resolve();
			EXIT_IF(c.selector.TryInstruction() == nullptr ||
			        c.selector.Instruction()->GetOpcode() != ValueOpcode::ReadFirstLane);
		}
	}
	query.value_storage.clear();
	for (auto& inst: block) {
		inst.SetParent(nullptr);
		if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 ||
		    inst.GetOpcode() == ValueOpcode::ReadConstBuffer)
			inst.SetFlags(SrtReadFlags {.index = inst.Flags<MemoryFlags>().index});
	}
	query.value_storage.splice(query.value_storage.end(), block.Instructions());
	source.call_targets = std::move(static_cast<ResourcePlan&>(query));

	// --- DIAGNOSTIC: summary table of all SWAPPC calls ---
	if (options.diag_swappc) {
		diag("\n=== S_SWAPPC_B64 CALL SUMMARY ===\n");
		diag("%-8s %-8s %-8s %-9s %-9s %-9s %-22s\n", "PC_idx", "dst_reg", "src_reg", "wave_size",
		     "valid_lo", "valid_hi", "classification");
		for (const auto& s: call_summaries) {
			diag("%-8u %-8u %-8u %-9u %-9s %-9s %-22s\n", s.instr_idx, s.dst_reg, s.src_reg,
			     s.wave_size, s.valid_lo ? "true" : "false", s.valid_hi ? "true" : "false",
			     s.classification);
		}
		diag("=================================\n\n");
	}
}

} // namespace

constexpr uint32_t kMaxTableEntries = 16u;   // candidati massimi per call site
constexpr uint32_t kMaxCalleeWords  = 1024u; // finestra massima di codice per callee (4 KB)

struct Candidate {
	uint32_t offset;  // valore del registro-selettore che sceglie questo callee
	uint64_t address; // VA guest del callee
};

struct Callee {
	std::vector<Decoder::Instruction> instructions; // pc e branch_target relativi all'entry
	std::vector<uint32_t>             observed;     // finestra [0, extent) letta dalla guest memory
};

std::vector<Candidate> EvaluateCandidates(const ShaderSource& source, const ShaderSource::Call& c,
                                          const IR::SrtRuntime& clean) {
	std::vector<Candidate> out;
	const auto&            call       = source.decoded.instructions[c.instruction];
	const auto             to_address = [](uint32_t low, uint32_t high) {
		// SWAPPC ignora i due bit bassi del target.
		return ((uint64_t {high} << 32u) | low) & ~uint64_t {3};
	};
	if (c.selector.IsEmpty()) {
		IR::SrtWalker walker(source.call_targets, clean);
		uint32_t      low = 0, high = 0;
		if (!walker.Evaluate(c.target[0], low) || !walker.Evaluate(c.target[1], high))
			EXIT("shader call at pc 0x%08x has an unavailable scalar target", call.pc);
		const auto address = to_address(low, high);
		if (address == 0u) {
			LOGF("[SWAPPC] call pc 0x%08x: target statico nullo, chiamata omessa\n", call.pc);
			return out;
		}
		out.push_back({0u, address});
		return out;
	}
	const auto* selector = c.selector.Instruction();
	uint32_t    k_end    = 0;
	uint32_t    nulls    = 0;
	for (uint32_t k = 0;; ++k) {
		IR::SrtWalker walker(source.call_targets, clean);
		walker.SetOverride(selector, k);
		uint32_t low = 0, high = 0, offset = 0;
		// La lettura fuori dai limiti del V# fallisce: e' il segnale di fine tabella.
		if (!walker.Evaluate(c.target[0], low) || !walker.Evaluate(c.target[1], high) ||
		    !walker.Evaluate(c.selector_offset, offset)) {
			k_end = k;
			break;
		}
		if (k >= kMaxTableEntries)
			EXIT("shader call at pc 0x%08x: call table has more than %u valid entries\n", call.pc,
			     kMaxTableEntries);
		const auto address = to_address(low, high);
		if (address == 0u) {
			++nulls;
			continue;
		}
		out.push_back({offset, address});
	}
	if (out.empty()) {
		LOGF("[SWAPPC] call pc 0x%08x: tabella senza entry valide (fine a k=%u, slot nulli=%u)\n",
		     call.pc, k_end, nulls);
	}
	return out;
}

// Scopre il callee seguendo il CFG dall'entry (non si assume piu' una foglia scalare lineare).
Callee DiscoverCallee(const IR::SrtRuntime& clean, uint64_t address,
                      const Decoder::Instruction& call) {
	std::vector<uint32_t> window;
	bool                  ok = false;
	for (uint32_t words = kMaxCalleeWords; words >= 16u && !ok; words /= 2u) {
		window.assign(words, 0u);
		ok = clean.read_memory(clean.userdata, address, window);
	}
	if (!ok)
		EXIT("shader call at pc 0x%08x cannot read callee code at 0x%016" PRIx64 "\n", call.pc,
		     address);

	std::map<uint32_t, Decoder::Instruction> found; // per indice di parola
	std::vector<uint32_t>                    work {0u};
	uint32_t                                 extent = 0;
	while (!work.empty()) {
		uint32_t index = work.back();
		work.pop_back();
		while (!found.contains(index)) {
			EXIT_NOT_IMPLEMENTED(index >= window.size());
			std::array<uint32_t, 2> words {window[index],
			                               index + 1u < window.size() ? window[index + 1u] : 0u};
			Decoder::Instruction    inst;
			Decoder::DecodeInstruction(words, 0u, inst);
			EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::UNKNOWN ||
			                     inst.opcode == Decoder::Opcode::UNSUPPORTED);
			inst.pc = index * sizeof(uint32_t);
			if (Decoder::IsDirectBranch(inst.opcode)) inst.branch_target += inst.pc;
			const uint32_t end = index + inst.word_count;
			EXIT_NOT_IMPLEMENTED(end > window.size());
			extent = std::max(extent, end);
			// Chiamate annidate, S_GETPC (richiederebbe il VA reale) e S_ENDPGM non sono
			// supportati.
			EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::S_SWAPPC_B64 ||
			                     inst.opcode == Decoder::Opcode::S_GETPC_B64 ||
			                     inst.opcode == Decoder::Opcode::S_ENDPGM);
			bool falls_through = true;
			if (inst.opcode == Decoder::Opcode::S_SETPC_B64) {
				EXIT_NOT_IMPLEMENTED(inst.src0.kind != Decoder::OperandKind::Sgpr ||
				                     inst.src0.reg != call.dst.reg);
				falls_through = false;
			} else if (Decoder::IsDirectBranch(inst.opcode)) {
				EXIT_NOT_IMPLEMENTED(inst.branch_target % 4u != 0u ||
				                     inst.branch_target / 4u >= window.size());
				work.push_back(inst.branch_target / 4u);
				falls_through = inst.opcode != Decoder::Opcode::S_BRANCH;
			}
			found.emplace(index, inst);
			if (!falls_through) break;
			index = end;
		}
	}
	Callee callee;
	for (auto& [index, inst]: found)
		callee.instructions.push_back(std::move(inst));
	callee.observed.assign(window.begin(), window.begin() + extent);
	return callee;
}

// Appende il callee ai dati collegati; ritorna il pc virtuale della sua entry.
uint32_t LinkCallee(ShaderSource::Linked& linked, const Callee& callee, uint32_t return_pc) {
	const auto&    prev        = linked.decoded.instructions.back();
	const uint32_t function_pc = prev.pc + prev.word_count * sizeof(uint32_t);
	const uint32_t function_end =
	    function_pc + static_cast<uint32_t>(callee.observed.size()) * sizeof(uint32_t);
	for (auto inst: callee.instructions) {
		linked.code.insert(linked.code.end(), inst.raw, inst.raw + inst.word_count);
		if (inst.opcode == Decoder::Opcode::S_SETPC_B64) {
			inst.opcode        = Decoder::Opcode::S_BRANCH; // ritorno statico al call site
			inst.branch_target = return_pc;
		} else if (Decoder::IsDirectBranch(inst.opcode)) {
			EXIT_IF(inst.branch_target > UINT32_MAX - function_pc);
			inst.branch_target += function_pc;
			EXIT_IF(inst.branch_target < function_pc || inst.branch_target >= function_end);
		}
		inst.pc += function_pc;
		linked.decoded.instructions.push_back(std::move(inst));
	}
	return function_pc; // la prima istruzione (indice 0) e' l'entry
}

// Stub di dispatch: s_cmp_eq_u32 sel, offset_k ; s_cbranch_scc1 callee_k ; ... ; s_branch return
uint32_t LinkStub(ShaderSource::Linked& linked, uint32_t selector_code,
                  std::span<const std::pair<uint32_t, uint32_t>> cases, uint32_t return_pc) {
	const auto& prev    = linked.decoded.instructions.back();
	uint32_t    pc      = prev.pc + prev.word_count * sizeof(uint32_t);
	const auto  stub_pc = pc;
	const auto  emit    = [&](uint32_t w0, uint32_t w1, uint32_t target_pc) {
		if (target_pc != UINT32_MAX) {
			const int32_t rel =
			    (static_cast<int32_t>(target_pc) - static_cast<int32_t>(pc + 4u)) / 4;
			EXIT_IF(rel < INT16_MIN || rel > INT16_MAX);
			w0 = (w0 & 0xffff0000u) | (static_cast<uint32_t>(rel) & 0xffffu);
		}
		std::array<uint32_t, 2> words {w0, w1};
		Decoder::Instruction    inst;
		Decoder::DecodeInstruction(words, 0u, inst);
		inst.pc = pc;
		if (target_pc != UINT32_MAX) inst.branch_target = target_pc;
		linked.code.insert(linked.code.end(), inst.raw, inst.raw + inst.word_count);
		pc += inst.word_count * sizeof(uint32_t);
		linked.decoded.instructions.push_back(std::move(inst));
	};
	for (const auto& [offset, callee_pc]: cases) {
		emit(0xbf06ff00u | selector_code, offset, UINT32_MAX); // s_cmp_eq_u32 sel, literal
		emit(0xbf850000u, 0u, callee_pc);                      // s_cbranch_scc1 callee
	}
	emit(0xbf820000u, 0u, return_pc); // default: salta la chiamata (indice fuori tabella)
	return stub_pc;
}

bool LinkedStillValid(const ShaderSource::Linked&                linked,
                      const std::vector<std::vector<Candidate>>& candidates,
                      const IR::SrtRuntime&                      clean) {
	if (linked.entries.size() != candidates.size()) return false;
	std::vector<uint32_t> scratch;
	for (size_t call = 0; call < candidates.size(); ++call) {
		if (call >= linked.entries.size()) return false;
		const auto& entries = linked.entries[call];
		if (entries.size() != candidates[call].size()) return false;
		for (size_t j = 0; j < entries.size(); ++j) {
			// Bounds check for functions array access
			if (entries[j].function >= linked.functions.size()) return false;
			const auto& fn = linked.functions[entries[j].function];
			if (entries[j].offset != candidates[call][j].offset ||
			    fn.address != candidates[call][j].address)
				return false;
			scratch.resize(fn.observed.size());
			if (!clean.read_memory(clean.userdata, fn.address, scratch) || scratch != fn.observed)
				return false;
		}
	}
	return true;
}

ShaderSource PrepareShaderSource(std::span<const uint32_t> code, const CompileOptions& options) {
	EXIT_IF(code.empty());
	ShaderSource source;
	if (!options.back_code.empty()) {
		source.decoded = DecodeFusedProgram(code, options.back_code, source.code);
	} else if (options.stage == ShaderType::Local) {
		source.decoded    = Decoder::DecodeFrontProgram(code);
		auto& handoff     = source.decoded.instructions.back();
		handoff.opcode    = Decoder::Opcode::S_ENDPGM;
		handoff.src_count = 0;
	} else {
		Decoder::DecodeProgram(code, source.decoded);
	}
	if (source.decoded.has_swap_pc) {
		PrepareCallTarget(source, options);
		source.code.assign(code.begin(), code.end());
		source.decoded.code = source.code;
	}
	return source;
}

const Decoder::Program& RefreshShaderSource(ShaderSource& source, const IR::SrtRuntime& runtime) {
	if (source.calls.empty()) return source.decoded;
	auto& reads = source.reads;
	reads.clear();
	IR::SrtReadCapture capture(runtime, reads);
	const auto         clean = IR::CleanRuntime(capture.ObservedRuntime());

	std::vector<std::vector<Candidate>> candidates;
	candidates.reserve(source.calls.size());
	for (const auto& c: source.calls)
		candidates.push_back(EvaluateCandidates(source, c, clean));
	const auto query_reads = reads.size();

	if (source.linked && LinkedStillValid(*source.linked, candidates, clean)) {
		return source.linked->decoded;
	}
	reads.resize(query_reads);

	ShaderSource::Linked linked {.code = source.code, .decoded = source.decoded};
	const auto&          last = source.decoded.instructions.back();
	EXIT_NOT_IMPLEMENTED(last.opcode != Decoder::Opcode::S_ENDPGM);
	linked.entries.resize(source.calls.size());

	for (size_t call_idx = 0; call_idx < source.calls.size(); ++call_idx) {
		const auto&    c         = source.calls[call_idx];
		const auto&    call      = source.decoded.instructions[c.instruction];
		const uint32_t return_pc = call.pc + call.word_count * sizeof(uint32_t);

		if (candidates[call_idx].empty()) {
			// Nessun callee: lo stub e' solo s_branch return_pc (indice fuori tabella).
			linked.decoded.instructions[c.instruction].branch_target =
			    LinkStub(linked, c.selector_sgpr, {}, return_pc);
			continue;
		}

		std::map<uint64_t, std::pair<uint32_t, uint32_t>>
		                                           by_address; // address -> (entry_pc, function)
		std::vector<std::pair<uint32_t, uint32_t>> cases;      // (offset, entry_pc)
		// Bounds check for entries array access
		if (call_idx >= linked.entries.size()) {
			EXIT("call_idx %zu out of bounds for entries.size() %zu", call_idx,
			     linked.entries.size());
		}
		for (const auto& cand: candidates[call_idx]) {
			uint32_t entry_pc = 0, function_index = 0;
			if (const auto it = by_address.find(cand.address); it != by_address.end()) {
				std::tie(entry_pc, function_index) = it->second;
			} else {
				const auto mark   = reads.size();
				auto       callee = DiscoverCallee(clean, cand.address, call);
				reads.resize(mark); // sostituisce la finestra letta con l'estensione reale
				reads.emplace_back(cand.address, callee.observed.size() * sizeof(uint32_t));
				entry_pc       = LinkCallee(linked, callee, return_pc);
				function_index = static_cast<uint32_t>(linked.functions.size());
				linked.functions.push_back({cand.address, std::move(callee.observed)});
				by_address.emplace(cand.address, std::pair {entry_pc, function_index});
			}
			linked.entries[call_idx].push_back({cand.offset, function_index});
			cases.emplace_back(cand.offset, entry_pc);
		}
		// Un solo candidato: salto diretto, nessuno stub (come prima per le chiamate statiche).
		const uint32_t target_pc = (c.selector.IsEmpty() || cases.size() == 1u)
		                               ? cases.front().second
		                               : LinkStub(linked, c.selector_sgpr, cases, return_pc);
		linked.decoded.instructions[c.instruction].branch_target = target_pc;
	}
	linked.decoded.code = linked.code;
	source.linked       = std::move(linked);
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

	const auto compile_begin = std::chrono::steady_clock::now();
	const auto phase_ms      = [&compile_begin]() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		                                 std::chrono::steady_clock::now() - compile_begin)
		                                 .count());
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
	auto       native_cfg = CFG::BuildGraph(decoded);
	CFG::Graph structured_cfg;
	auto*      selected_cfg = &native_cfg;
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
			native_cfg.unsupported        = true;
			native_cfg.failure_kind       = structured_cfg.failure_kind;
			native_cfg.failure_block      = structured_cfg.failure_block;
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

	const auto&                 cfg = *selected_cfg;
	Frontend::EmbeddedFetchPlan embedded_fetch;
	if ((options.stage == ShaderType::Vertex || options.stage == ShaderType::Local) &&
	    options.input_info.vertex != nullptr && options.input_info.vertex->fetch_embedded) {
		embedded_fetch = DetectEmbeddedVertexFetch(
		    decoded, options.input_info.vertex, options.user_data_base,
		    static_cast<uint32_t>(options.user_data.size()), options.wave_size);
		if (!embedded_fetch.loads.empty()) {
			LOGF("%s embedded vertex fetch plan: detected=%" PRIu64 "\n", GetDumpLabel(options),
			     static_cast<uint64_t>(embedded_fetch.loads.size()));
		}
	}
	Frontend::TranslateOptions translate_options {
	    .stage           = options.stage,
	    .wave_size       = options.wave_size,
	    .shader_hash     = options.shader_hash,
	    .user_data_base  = options.user_data_base,
	    .user_data_count = static_cast<uint32_t>(options.user_data.size()),
	    .input_info      = options.input_info,
	    .embedded_fetch  = embedded_fetch.loads.empty() ? nullptr : &embedded_fetch,
	};
	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
	auto ir = Frontend::TranslateProgram(decoded, cfg, translate_options);
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram blocks=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(ir.blocks.size()), phase_ms());
	IR::RewriteToSsa(ir.blocks);
	IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
	IR::ResolveControlFlowIdentities(ir);
	IR::RemoveIdentities(ir.blocks);
	IR::EliminateDeadCode(ir.blocks);
	const auto read_lane_stats = IR::EliminateReadLane(ir, ir.wave_size);
	if (read_lane_stats.rewritten_reads != 0) {
		LOGF("%s read-lane elimination: reads=%" PRIu32 "\n", GetDumpLabel(options),
		     read_lane_stats.rewritten_reads);
		IR::ConstantPropagationPass(ir.blocks, ir.wave_size);
		IR::ResolveControlFlowIdentities(ir);
		IR::RemoveIdentities(ir.blocks);
		IR::EliminateDeadCode(ir.blocks);
	}
	if (decoded.has_swap_pc) {
		// Leaf returns are resolved CFG edges; native PC values must not escape into GPU data.
		for (const auto* block: ir.blocks)
			for (const auto& inst: *block)
				EXIT_NOT_IMPLEMENTED(inst.GetOpcode() == IR::ValueOpcode::GetShaderBase);
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
	IR::TrackResources(ir, decoded, native_cfg);
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
                             uint32_t                          push_data_start_dword) {
	const auto emit_begin = std::chrono::steady_clock::now();
	auto&      ir         = translated.program;
	IR::ApplyResourceSpecialization(ir, specialization);
	// The resource plan owns host descriptor evaluation now. Keep only dependencies consumed
	// by GPU memory operations; bound descriptor dwords must not retain shader instructions.
	for (auto& inst: ir.value_storage) {
		inst.Invalidate();
	}
	for (auto* block: ir.blocks) {
		for (auto& inst: *block) {
			const auto op    = inst.GetOpcode();
			uint32_t   first = 0;
			if (op == IR::ValueOpcode::GetBufferResource) {
				if (std::ranges::any_of(inst.Uses(), [&](const IR::Use& use) {
					    const auto mem_index = use.user->Flags<IR::MemoryFlags>().index;
					    if (mem_index >= ir.memory_info.size()) return false;
					    return ir.memory_info[mem_index].kind == IR::ResourceKind::IndirectBuffer;
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
				first               = resource < ir.info.images.size() &&
				                              ir.info.images[resource].indirect_root == resource
				                          ? 1u
				                          : 0u;
			} else if (op == IR::ValueOpcode::GetSamplerResource) {
				const auto resource = inst.Flags<uint32_t>();
				first               = resource < ir.info.samplers.size() &&
				                              !ir.info.samplers[resource].indirect_resources.empty()
				                          ? 1u
				                          : 0u;
			} else {
				continue;
			}
			for (size_t index = first; index < inst.NumArgs(); index++) {
				inst.SetArg(index, IR::Value(0u));
			}
		}
	}
	ir.value_storage.clear();
	IR::RemoveIdentities(ir.blocks);
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
	auto spirv = Spirv::EmitProgram(ir, options.input_info, push_data_start_dword);
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
