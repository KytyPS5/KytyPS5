#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/RtFeedback.h"
#include "graphics/shader/recompiler/Tessellation.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
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
#include "graphics/shader/recompiler/ir/passes/DynamicBuffer.h"
#include "graphics/shader/recompiler/ir/passes/PhiWebs.h"
#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#include <algorithm>
#include <unordered_set>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <array>
#include <chrono>
#include <fmt/format.h>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <set>
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
    const Decoder::Program& decoded, const ShaderVertexInputInfo* input_info,
    uint32_t user_data_base, uint32_t user_data_count, uint32_t wave_size) {
	const uint32_t    vertex_index_reg   = input_info->logical_stage == ShaderType::Local ? 2u : 5u;
	const uint32_t    instance_index_reg = input_info->logical_stage == ShaderType::Local ? 5u : 8u;
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

void PrepareCallTarget(ShaderSource& source, const CompileOptions& options) {
	using namespace IR;
	for (uint32_t i = 0; i < source.decoded.instructions.size(); ++i) {
		if (source.decoded.instructions[i].opcode != Decoder::Opcode::S_SWAPPC_B64) continue;
		EXIT_NOT_IMPLEMENTED(source.call.has_value() || options.stage != ShaderType::Compute);
		source.call = ShaderSource::Call {.instruction = i};
	}
	EXIT_IF(options.input_info.compute == nullptr);
	const auto& call = source.decoded.instructions[source.call->instruction];
	EXIT_NOT_IMPLEMENTED(call.src0.kind != Decoder::OperandKind::Sgpr ||
	                     call.dst.kind != Decoder::OperandKind::Sgpr ||
	                     call.src0.reg != call.dst.reg || call.dst.reg >= 105u);
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
		const auto index = CFG::FindScalarDefinition(source.decoded, graph, before, code);
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
	for (uint32_t i = 0; i < 2u; ++i) {
		const auto value = Register(source.call->instruction, call.src0.reg + i);
		EXIT_IF(!ValidateRuntimeValue(query, value, RuntimeValueType::Integer));
		auto& root = query.value_storage.emplace_back(ValueOpcode::ReferenceU32);
		root.SetArg(0, value);
		source.call->target[i] = Value(&root);
	}
	RewriteToSsa(query.blocks);
	ConstantPropagationPass(query.blocks, query.wave_size);
	RemoveIdentities(query);
	EliminateDeadCode(query.blocks);
	for (auto& value: source.call->target) value = value.Instruction()->Arg(0).Resolve();
	query.value_storage.clear();
	for (auto& inst: block) {
		inst.SetParent(nullptr);
		if (inst.GetOpcode() == ValueOpcode::LoadAddressU32 || inst.GetOpcode() == ValueOpcode::ReadConstBuffer)
			inst.SetFlags(SrtReadFlags {.index = inst.Flags<MemoryFlags>().index});
	}
	query.value_storage.splice(query.value_storage.end(), block.Instructions());
	source.call_targets = std::move(static_cast<ResourcePlan&>(query));
}

// --raytracing: a call through S_SWAPPC_B64 s[14:15], s[14:15] reaches a function whose address is
// read from a table at run time (the hit shaders of a ray tracing kernel). The table and the
// functions are guest data, so they are resolved here, when the program is translated: the
// function bodies are appended to the code, the call becomes a branch to a small compare chain on
// the target address, and each function's own return (S_SETPC_B64 s[14:15]) becomes a branch
// back to the instruction after the call. Only a single call site is supported.
constexpr uint32_t SwapPcWordS14S14 = 0xbe8e210eu;
constexpr uint32_t SetPcWordS14     = 0xbe80200eu;
// SOPC s_cmp_eq_u32 (opcode 0x06) with SSRC0 = 255, so the literal is the first operand and follows in
// the next dword, and SSRC1 = 14, the register the call put the function address in.
constexpr uint32_t CmpEqLiteralS14  = 0xbf060effu;
constexpr uint32_t CmpEqLiteralS16  = 0xbf0610ffu;
constexpr uint32_t MovLiteralS16    = 0xbe9003ffu;
constexpr uint32_t MovLiteralS17    = 0xbe9103ffu;
// SOP1 S_MOV_B32 (opcode 0x03) with a plain SGPR source: one word, no literal follows.
constexpr uint32_t MovRegSS(uint32_t dst, uint32_t src) {
	return 0xbe800000u | (3u << 8) | ((dst & 0x7fu) << 16) | (src & 0xffu);
}

// Diagnostics about the ray tracing call table must survive the fatal exit that usually follows them:
// the log is buffered and loses its tail.
void RtDiag(const char* format, ...) {
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fflush(stderr);
}

bool IsSwapPcCall(const Decoder::Instruction& inst) {
	// The decoder knows S_SWAPPC_B64 now; a call whose target it could not link has no direct branch target.
	return inst.family == Decoder::Family::SOP1 && inst.opcode_id == 0x21u &&
	       (inst.opcode == Decoder::Opcode::UNSUPPORTED ||
	        (inst.opcode == Decoder::Opcode::S_SWAPPC_B64 && inst.branch_target == UINT32_MAX));
}

bool ReadGuestWords(uint64_t address, uint32_t* destination, size_t words) {
	__try {
		std::memcpy(destination, reinterpret_cast<const void*>(address), words * sizeof(uint32_t));
		return true;
	} __except (1) {
		return false;
	}
}

// Reads as much of a function body as is mapped, in chunks.
std::vector<uint32_t> ReadGuestCode(uint64_t address, size_t max_words) {
	std::vector<uint32_t> words;
	constexpr size_t chunk = 64;
	while (words.size() < max_words) {
		uint32_t buffer[chunk];
		if (!ReadGuestWords(address + words.size() * sizeof(uint32_t), buffer, chunk)) break;
		words.insert(words.end(), buffer, buffer + chunk);
	}
	return words;
}

// The function ends at the S_SETPC_B64 / S_ENDPGM that no branch of it jumps past.
size_t FindFunctionExtent(std::span<const uint32_t> body) {
	uint32_t max_target = 0;
	for (uint32_t index = 0; index < body.size();) {
		Decoder::Instruction inst;
		Decoder::DecodeInstruction(body, index, inst);
		index += inst.word_count;
		if (Decoder::IsDirectBranch(inst.opcode)) max_target = std::max(max_target, inst.branch_target);
		if ((inst.raw[0] == SetPcWordS14 || inst.opcode == Decoder::Opcode::S_ENDPGM) &&
		    index * sizeof(uint32_t) > max_target) {
			return index;
		}
	}
	return 0;
}

// Inlines the functions behind ONE call site (S_SWAPPC_B64 s[14:15], s[14:15]). The target comes from a table of
// 16-byte records: either indexed by a lane value out of a buffer the SRT describes (a waterfall over a V#), or
// a single record read straight out of the SRT. The program is rewritten so the call becomes a compare chain
// that branches into inlined copies of the candidate functions.
// With --raytracing a call table of a kernel without ray traversal is split into this many groups of functions. Each
// group is its own program (so the driver compiles a few functions at a time) and its own dispatch; a lane runs only
// in the dispatch of the group its function belongs to.
constexpr uint32_t RtCallGroups = 12u;
// A group inlines at least this many functions: a program with a single inlined function does not get its
// descriptors resolved (resource tracking fails).
constexpr size_t RtCallGroupMinimum = 4u;

uint32_t CallGroupCount() {
	return Config::RaytracingEnabled() ? RtCallGroups : 0u;
}

bool ExpandOneCall(const CompileOptions& options, std::span<const uint32_t> code,
                   const Decoder::Program& decoded, size_t call_index, std::vector<uint32_t>& out,
                   std::vector<CallInputRead>& call_inputs, std::vector<RtFeedbackSite>& sites) {
	const auto read_call_words = [&](uint64_t address, uint32_t* destination, size_t count, bool relative_to_srt = false) {
		if (!ReadGuestWords(address, destination, count)) return false;
		const uint64_t srt = options.user_data[0] | (uint64_t(options.user_data[1]) << 32u);
		call_inputs.push_back({relative_to_srt ? address - srt : address, relative_to_srt, UINT32_MAX,
		                       std::vector<uint32_t>(destination, destination + count)});
		return true;
	};
	const auto& insts = decoded.instructions;
	if (insts[call_index].raw[0] != SwapPcWordS14S14 || options.user_data.size() < 2u) {
		RtDiag("RT calls: unsupported call form at pc=0x%x\n", static_cast<unsigned>(insts[call_index].pc));
		return false;
	}
	const auto last_writer = [&](size_t before, auto&& match) -> const Decoder::Instruction* {
		for (size_t i = before; i-- > 0;) {
			if (match(insts[i])) return &insts[i];
		}
		return nullptr;
	};
	using Op = Decoder::Opcode;
	const auto* mov = last_writer(call_index, [](const Decoder::Instruction& i) {
		return i.opcode == Op::S_MOV_B32 && i.dst.kind == Decoder::OperandKind::Sgpr && i.dst.reg == 14u;
	});
	// The table record is read either by a buffer load through a V# (S_BUFFER_LOAD_DWORDX4) or by one scalar load
	// straight from the SRT (S_LOAD_DWORDX4 from s[0:1]).
	const auto* table_load = mov == nullptr ? nullptr : last_writer(static_cast<size_t>(mov - insts.data()),
	    [&](const Decoder::Instruction& i) {
		    return (i.opcode == Op::S_BUFFER_LOAD_DWORDX4 || (i.opcode == Op::S_LOAD_DWORDX4 && i.src0.reg == 0u)) &&
		           i.dst.reg <= mov->src0.reg && mov->src0.reg < i.dst.reg + 4u;
	    });
	if (table_load == nullptr) {
		RtDiag("RT calls: the call target is not read from a table in a known shape\n");
		return false;
	}
	const uint64_t srt = options.user_data[0] | (uint64_t(options.user_data[1]) << 32);
	std::vector<uint32_t> table;
	uint32_t              count = 0;
	uint32_t              vsharp_srt_offset = UINT32_MAX;
	size_t                count_input = SIZE_MAX;
	uint64_t              table_base_address = 0;
	if (table_load->opcode == Op::S_LOAD_DWORDX4) {
		table.resize(4u);
		if (!ReadGuestWords(srt + table_load->offset, table.data(), 4u)) {
			RtDiag("RT calls: the call record in the SRT is not readable\n");
			return false;
		}
		// Only the function address is baked in; the record's pointer dwords are read again at run time.
		call_inputs.push_back({table_load->offset, true, UINT32_MAX, {table[0], table[1]}});
		count = 1u;
	} else {
		const auto* vsharp_load = last_writer(static_cast<size_t>(table_load - insts.data()),
		    [&](const Decoder::Instruction& i) {
			    const uint32_t width = i.opcode == Op::S_LOAD_DWORDX4 ? 4u
			                           : i.opcode == Op::S_LOAD_DWORDX8 ? 8u
			                           : i.opcode == Op::S_LOAD_DWORDX16 ? 16u : 0u;
			    return width != 0u && i.src0.reg == 0u && i.dst.reg <= table_load->src0.reg &&
			           table_load->src0.reg - i.dst.reg + 4u <= width;
		    });
		if (vsharp_load == nullptr) {
			RtDiag("RT calls: the call table descriptor is not read from the SRT\n");
			return false;
		}
		// A wider scalar load can contain several descriptors; use the four words selected by the table.
		vsharp_srt_offset = vsharp_load->offset + (table_load->src0.reg - vsharp_load->dst.reg) * 4u;
		uint32_t vsharp[4] {};
		if (!ReadGuestWords(srt + vsharp_srt_offset, vsharp, 4)) {
			RtDiag("RT calls: the call table descriptor is not readable\n");
			return false;
		}
		const uint64_t base   = vsharp[0] | (uint64_t(vsharp[1] & 0xffffu) << 32);
		const uint32_t stride = (vsharp[1] >> 16) & 0x3fffu;
		count                 = vsharp[2];
		if (stride != 16u || count == 0u || count > 4096u) {
			RtDiag("RT calls: unexpected call table stride=%u records=%u\n", stride, count);
			return false;
		}
		table_base_address = base;
		table.resize(count * 4u);
		if (!ReadGuestWords(base, table.data(), table.size())) {
			RtDiag("RT calls: the call table is not readable\n");
			return false;
		}
		// What the translated program depends on: the record count and each function address. The table
		// moves between frames and its pointer dwords change with it, so neither is part of the key.
		count_input = call_inputs.size();
		call_inputs.push_back({vsharp_srt_offset + 8u, true, UINT32_MAX, {vsharp[2]}});

	}
	// The call copies the table record into s14..s17. s14:s15 is the target; when s16/s17 take record dwords
	// they are the function's own pointer (its SRT: descriptors, constants). The record is known here, so every
	// inlined copy starts by loading its own pointer as a constant: the descriptors behind it are then plain
	// constant-address reads instead of a pointer that merges the call sites and the traversal loop.
	uint32_t record_dword[4] = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
	for (uint32_t j = 0; j < 4u; ++j) {
		const auto* writer = last_writer(call_index, [&](const Decoder::Instruction& i) {
			return i.opcode == Op::S_MOV_B32 && i.dst.kind == Decoder::OperandKind::Sgpr && i.dst.reg == 14u + j;
		});
		if (writer != nullptr && writer > table_load && writer->src0.kind == Decoder::OperandKind::Sgpr &&
		    writer->src0.reg >= table_load->dst.reg && writer->src0.reg < table_load->dst.reg + 4u) {
			record_dword[j] = writer->src0.reg - table_load->dst.reg;
		}
	}
	// The record's pointer is not baked in as a constant (the game moves its per-frame data, and a kernel
	// that embeds addresses would be a new kernel every frame). Each copy knows which record it belongs to
	// and reads that record's pointer dwords from the table when it runs; the table is a plain V# (or the
	// SRT), so the resource tracker resolves the descriptors behind the pointer at dispatch time.
	const bool patches_pointer = record_dword[2] != UINT32_MAX || record_dword[3] != UINT32_MAX;
	// The register that held the record index (as a byte offset) for the table load; it tells copies of
	// the same function apart. A single record read from the SRT has none.
	const bool     table_is_buffer = table_load->opcode != Op::S_LOAD_DWORDX4;
	const uint32_t index_code      = table_is_buffer ? (table_load->raw[1] >> 25u) & 0x7fu : UINT32_MAX;
	// With --raytracing one copy of each function is inlined; each copy loads its own record's pointer
	// from the table through the record index register, so the descriptors behind it are selected at run time.
	// The shared form needs the direct-base pointer sequence (the resource tracker keys the descriptors behind the
	// pointer by the record number) and feedback (the functions to inline are the ones the GPU is seen to call).
	const bool direct_base = table_is_buffer && record_dword[2] == 2u && record_dword[3] == 3u &&
	                         vsharp_srt_offset != UINT32_MAX;
	const bool share_bodies = direct_base && index_code <= 106u &&
	                          Config::RaytracingEnabled() &&
	                          RtFeedback::GetBuffer().host.load() != nullptr;
	const uint32_t group_count = options.input_info.compute != nullptr && options.input_info.compute->call_pass != UINT32_MAX
	                                 ? CallGroupCount() : 0u;
	const uint32_t group_pass  = group_count != 0u ? options.input_info.compute->call_pass : 0u;
	// The SGPR pair the kernel restores EXEC from once the waterfall loop around the call has finished. A lane whose
	// function is not in this group leaves that pair, so it stays off for everything that follows the loop.
	uint32_t outer_exec_sgpr = UINT32_MAX;
	if (group_count != 0u) {
		if (!share_bodies) {
			RtDiag("RT calls: call groups need the shared-body form\n");
			return false;
		}
		for (size_t i = call_index + 1u; i + 1u < insts.size(); ++i) {
			if (insts[i].opcode != Op::S_CBRANCH_EXECNZ || insts[i].branch_target > insts[call_index].pc) continue;
			const auto& next = insts[i + 1u];
			if (next.opcode == Op::S_MOV_B64 && next.dst.kind == Decoder::OperandKind::ExecLo &&
			    next.src0.kind == Decoder::OperandKind::Sgpr && (next.src0.reg & 1u) == 0u && next.src0.reg < 104u)
				outer_exec_sgpr = next.src0.reg;
			break;
		}
		if (outer_exec_sgpr == UINT32_MAX) {
			RtDiag("RT calls: call groups need the exec restore after the call loop in a known shape (hash=%016llx pass=%u "
			       "thread_dims=%d)\n",
			       static_cast<unsigned long long>(options.shader_hash), group_pass,
			       options.input_info.compute->dispatch_thread_dimensions ? 1 : 0);
			return false;
		}
	}
	// What a shared program depends on is the code of its functions, not the table: the record count and the
	// order of the records change from scene to scene.
	if (share_bodies && count_input != SIZE_MAX && count_input + 1u == call_inputs.size()) call_inputs.pop_back();
	if (table_is_buffer) {
		for (size_t i = static_cast<size_t>(table_load - insts.data()) + 1u; i < call_index; ++i) {
			if ((insts[i].dst.kind == table_load->src1.kind && insts[i].dst.reg == table_load->src1.reg) ||
			    (insts[i].dst2.kind == table_load->src1.kind && insts[i].dst2.reg == table_load->src1.reg)) {
				RtDiag("RT calls: the table index register is overwritten before the call\n");
				return false;
			}
		}
	}
	// Long tables are inlined by feedback: only the records the GPU has been seen to call. A call to any
	// other record logs its index and returns; the host adds the record and retranslates.
	bool feedback = share_bodies ||
	                      (table_is_buffer && index_code <= 106u && patches_pointer &&
	                       count > RtFeedback::FeedbackMinRecords &&
	                       RtFeedback::GetBuffer().host.load() != nullptr);
	// Track a conservative union of used indices for this call-site shape. Tables can
	// rearrange uncalled records every frame; those changes must not reset learning.
	// Compiled targets are still captured per variant below, so this union never
	// substitutes a function body from another table.
	const uint64_t site_hash = share_bodies
	    ? (options.shader_hash ^ (uint64_t {vsharp_srt_offset} * 0xbf58476d1ce4e5b9ull) ^ 0x7274666273686172ull)
	    : (options.shader_hash ^ (uint64_t {count} * 0x9e3779b97f4a7c15ull) ^
	       (uint64_t {vsharp_srt_offset} * 0xbf58476d1ce4e5b9ull) ^ 0x7274666261636b32ull);
	const uint32_t feedback_slot = feedback ? RtFeedback::SlotFor(site_hash, insts[call_index].pc) : 0u;
	if (feedback_slot == UINT32_MAX) {
		if (share_bodies) {
			RtDiag("RT calls: feedback regions exhausted; the shared form needs one\n");
			return false;
		}
		RtDiag("RT calls: feedback regions exhausted; expanding the complete table\n");
		feedback = false;
	}	std::vector<uint32_t> used_records;
	if (feedback && !share_bodies && options.fixed_sites != nullptr) {
		for (const auto& candidate: *options.fixed_sites)
			if (candidate.pc == insts[call_index].pc && !candidate.shared) used_records = candidate.records;
	} else if (feedback && !share_bodies) {
		used_records = RtFeedback::UsedRecords(feedback_slot);
		const auto& persisted = RtFeedback::LoadPersisted(site_hash, insts[call_index].pc);
		used_records.insert(used_records.end(), persisted.begin(), persisted.end());
		std::ranges::sort(used_records);
		used_records.erase(std::unique(used_records.begin(), used_records.end()), used_records.end());
		RtFeedback::SavePersisted(site_hash, insts[call_index].pc, used_records);
	}
	struct Entry {
		uint32_t low            = 0;
		uint32_t record         = 0;
		bool     compare_record = false;
	};
	std::vector<Entry> entries;
	const RtFeedbackSite* fixed_site = nullptr;
	if (options.fixed_sites != nullptr) {
		for (const auto& candidate: *options.fixed_sites)
			if (candidate.pc == insts[call_index].pc && candidate.shared == share_bodies) fixed_site = &candidate;
	}
	if (share_bodies) {
		// One copy per function the GPU has been seen to call; a table that fits is inlined whole to start with.
		auto functions = RtFeedback::LearnedFunctions(site_hash, insts[call_index].pc);
		if (fixed_site != nullptr) functions = std::set<uint32_t>(fixed_site->functions.begin(), fixed_site->functions.end());
		if (fixed_site == nullptr && count <= RtFeedback::FeedbackMinRecords)
			for (uint32_t i = 0; i < count; ++i) functions.insert(table[i * 4u]);
		// A function an earlier run saw the GPU call is inlined from the start, found by the hash of its body.
		static std::map<uint64_t, uint64_t> body_hashes; // address -> hash, 0 when it is not a function
		const auto body_hash = [&](uint32_t low) {
			const uint64_t address = (uint64_t(table[1]) << 32) | low;
			if (const auto found = body_hashes.find(address); found != body_hashes.end()) return found->second;
			uint64_t hash = 0;
			auto     words = ReadGuestCode(address, 8192);
			if (const auto extent = FindFunctionExtent(words); extent != 0u) {
				hash = 1469598103934665603ull;
				for (size_t i = 0; i < extent; ++i) hash = (hash ^ words[i]) * 1099511628211ull;
			}
			return body_hashes.emplace(address, hash).first->second;
		};
		if (fixed_site == nullptr) {
			const auto persisted = RtFeedback::PersistedFunctionHashes(site_hash, insts[call_index].pc);
			if (!persisted.empty()) {
				std::set<uint32_t> distinct;
				for (uint32_t i = 0; i < count; ++i) distinct.insert(table[i * 4u]);
				size_t hashed = 0, matched = 0;
				for (const auto low: distinct) {
					if ((low & 3u) != 0u) continue;
					const auto hash = body_hash(low);
					hashed += hash != 0u;
					if (hash != 0u && persisted.contains(hash)) {
						functions.insert(low);
						++matched;
					}
				}
			}
		}
		if (fixed_site == nullptr) {
			// What the GPU has called so far (this run's learned set) is remembered for the next run.
			std::vector<uint64_t> learned_hashes;
			for (const auto low: RtFeedback::LearnedFunctions(site_hash, insts[call_index].pc)) {
				if ((low & 3u) == 0u) {
					if (const auto hash = body_hash(low); hash != 0u) learned_hashes.push_back(hash);
				}
			}
			if (!learned_hashes.empty())
				RtFeedback::AddPersistedFunctionHashes(site_hash, insts[call_index].pc, learned_hashes);
		}
		if (group_count != 0u) {
			const auto all_functions = functions;
			std::erase_if(functions, [&](uint32_t low) { return CallGroupOf(low, group_count) != group_pass; });
			// A program with a single inlined function does not get its descriptors resolved (resource tracking
			// fails); a second function, owned by another group, keeps it a table. Its lanes then run in both
			// groups, which stores the same values twice.
			for (const auto low: all_functions) {
				if (functions.size() >= RtCallGroupMinimum) break;
				functions.insert(low);
			}
			std::set<uint32_t> table_functions;
			for (uint32_t i = 0; i < count; ++i) table_functions.insert(table[i * 4u]);
			RtDiag("RT calls: group %u/%u inlines %zu of %zu known functions (table of %zu)\n", group_pass, group_count,
			       functions.size(), all_functions.size(), table_functions.size());
		}
		for (const auto low: functions) {
			if ((low & 3u) == 0u) entries.push_back({low, 0u, false});
		}
	}
	for (uint32_t i = 0; i < count && !share_bodies; ++i) {
		if (feedback && !std::ranges::binary_search(used_records, i)) continue;
		const uint64_t target = table[i * 4u] | (uint64_t(table[i * 4u + 1u]) << 32);
		if ((target >> 32) != (uint64_t(table[1]) & 0xffffffffu) || (target & 3u) != 0u) {
			RtDiag("RT calls: unexpected call target 0x%llx\n", static_cast<unsigned long long>(target));
			return false;
		}
		Entry entry;
		entry.low    = table[i * 4u];
		entry.record = i;
		// Without a pointer to patch, copies of one function are identical: keep one.
		if (!patches_pointer &&
		    std::ranges::any_of(entries, [&](const Entry& other) { return other.low == entry.low; })) {
			continue;
		}
		entries.push_back(entry);
		if (table_is_buffer) {
			// Only targets actually embedded in this expansion are code dependencies.
			call_inputs.push_back({uint64_t {i} * 16u, false, vsharp_srt_offset,
			                       {table[i * 4u], table[i * 4u + 1u]}});
		}
	}
	// Copies of one function are told apart by the record index the call loaded the record with.
	for (auto& entry: entries) {
		const auto others = std::ranges::count_if(entries, [&](const Entry& other) { return other.low == entry.low; });
		if (others > 1) {
			if (!table_is_buffer || index_code > 106u) {
				RtDiag("RT calls: one function is called through several records and the record index is not in a register\n");
				return false;
			}
			entry.compare_record = true;
		}
	}
	const size_t sites_before = sites.size();
	if (feedback) {
		RtFeedbackSite site {feedback_slot, {}};
		site.pc = insts[call_index].pc;
		if (share_bodies) {
			site.shared           = true;
			site.site_hash        = site_hash;
			site.pc               = insts[call_index].pc;
			site.table_srt_offset = vsharp_srt_offset;
			site.group_pass       = group_pass;
			site.group_count      = group_count;
		}
		for (const auto& entry: entries) (share_bodies ? site.functions : site.records).push_back(share_bodies ? entry.low : entry.record);
		std::ranges::sort(site.functions);
		sites.push_back(std::move(site));
	}
	const uint64_t high = uint64_t(table[1]) << 32;

	struct Function {
		uint32_t              address_low;
		bool                  compare_pointer; // tell copies apart by the record index register
		uint32_t              pointer_low;     // that register's expected value: the record's byte offset
		std::vector<uint32_t> body;
	};
	std::vector<Function>                          functions;
	std::map<uint32_t, std::vector<uint32_t>>      bodies;
	for (size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
		const auto& entry = entries[entry_index];
		auto& words = bodies[entry.low];
		if (words.empty()) {
			words = ReadGuestCode(high | entry.low, 8192);
			const auto extent = FindFunctionExtent(words);
			bool       usable = extent != 0u;
			if (usable) {
				words.resize(extent);
				// A branch out of the function would leave the copy.
				for (uint32_t index = 0; index < words.size() && usable;) {
					Decoder::Instruction inst;
					Decoder::DecodeInstruction(words, index, inst);
					index += inst.word_count;
					if (Decoder::IsDirectBranch(inst.opcode) && inst.branch_target > words.size() * sizeof(uint32_t)) usable = false;
				}
			}
			if (!usable) {
				RtDiag("RT calls: the function at 0x%llx is not a self-contained function\n",
				       static_cast<unsigned long long>(high | entry.low));
				// A learned function can be stale (its code moved); one bad entry must not lose the others.
				if (!share_bodies) return false;
				bodies.erase(entry.low);
				entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(entry_index));
				--entry_index;
				continue;
			}
		}
	}
	if (share_bodies && feedback) {
		std::vector<uint32_t> kept;
		for (const auto& entry: entries) kept.push_back(entry.low);
		std::ranges::sort(kept);
		sites.back().functions = std::move(kept);
	}
	// Callee-saved temps: pick SGPRs no guest code touches, so the save/restore below cannot
	// corrupt anything. Widths matter: descriptor bases and vector loads cover ranges whose other
	// members are never named (e.g. s83 inside an s76-based X8 load), and 64-bit SALU ops read and
	// write register pairs. Over-marking only wastes registers; under-marking corrupts guest state.
	// The call protocol (s14-s17) and the clobbered set (s81-s83) are excluded explicitly: s83 is
	// never named yet is live, and s1 looked free but is user data (GetUserData s1) in these kernels.
	struct SavedReg {
		uint32_t clobbered, saved;
	};
	std::vector<SavedReg> saved_regs;
	uint32_t              table_temp = UINT32_MAX;
	{
		bool used_sgpr[104] = {};
		const auto mark = [&](uint32_t reg, uint32_t width) {
			for (uint32_t k = reg; k < reg + width && k < 104u; ++k) used_sgpr[k] = true;
		};
		for (uint32_t reg: {14u, 15u, 16u, 17u, 81u, 82u, 83u}) mark(reg, 1u);
		const auto is_pair_op = [](Op opcode) {
			switch (opcode) {
				case Op::S_MOV_B64:
				case Op::S_CMOV_B64:
				case Op::S_CSELECT_B64:
				case Op::S_BREV_B64:
				case Op::S_NOT_B64:
				case Op::S_AND_B64:
				case Op::S_OR_B64:
				case Op::S_XOR_B64:
				case Op::S_NAND_B64:
				case Op::S_NOR_B64:
				case Op::S_XNOR_B64:
				case Op::S_ANDN2_B64:
				case Op::S_ORN2_B64:
				case Op::S_LSHL_B64:
				case Op::S_LSHR_B64:
				case Op::S_BFE_U64:
				case Op::S_BFM_B64:
				case Op::S_GETPC_B64:
				case Op::S_CMP_EQ_U64:
				case Op::S_CMP_LG_U64:
				case Op::S_BCNT1_I32_B64:
				case Op::S_FF1_I32_B64:
				case Op::S_FLBIT_I32_B64:
				case Op::S_BITCMP0_B64:
				case Op::S_BITCMP1_B64:
				case Op::S_WQM_B64: return true;
				default: return false;
			}
		};
		const auto mark_inst = [&](const Decoder::Instruction& inst) {
			const Decoder::Operand* operands[] = {&inst.dst,  &inst.dst2, &inst.src0,
			                                      &inst.src1, &inst.src2, &inst.src3};
			for (const auto* operand: operands) {
				if (operand->kind != Decoder::OperandKind::Sgpr || operand->reg > 103u) continue;
				uint32_t width = 1u;
				const bool is_dst = (operand == &inst.dst || operand == &inst.dst2);
				if (!is_dst) {
					switch (inst.family) {
						case Decoder::Family::MIMG: width = 8u; break;
						case Decoder::Family::MUBUF:
						case Decoder::Family::MTBUF: width = 4u; break;
						case Decoder::Family::FLAT:
						case Decoder::Family::DS: width = 2u; break;
						default:
							if (is_pair_op(inst.opcode)) width = 2u;
							break;
					}
				} else if (inst.family == Decoder::Family::SMEM) {
					width = std::max(inst.data_dwords, 1u);
				} else if (is_pair_op(inst.opcode)) {
					width = 2u;
				}
				mark(operand->reg, width);
			}
		};
		for (const auto& inst: insts) mark_inst(inst);
		for (const auto& [low, words]: bodies) {
			for (uint32_t index = 0; index < words.size();) {
				Decoder::Instruction inst;
				Decoder::DecodeInstruction(words, index, inst);
				if (inst.word_count == 0u) break;
				mark_inst(inst);
				index += inst.word_count;
			}
		}
		constexpr uint32_t kClobbered[] = {81u, 82u, 83u};
		for (uint32_t reg = 103u; reg >= 16u; --reg) {
			if (!used_sgpr[reg] && saved_regs.size() < 3u) {
				saved_regs.push_back({kClobbered[saved_regs.size()], reg});
			}
			if (reg == 16u) break;
		}
		// A free aligned SGPR quad for the table's V#, reloaded from the SRT by every copy (below).
		for (const auto& reg: saved_regs) used_sgpr[reg.saved] = true;
		for (uint32_t base = 100u; base >= 16u && table_temp == UINT32_MAX; base -= 4u) {
			if (!used_sgpr[base] && !used_sgpr[base + 1u] && !used_sgpr[base + 2u] && !used_sgpr[base + 3u])
				table_temp = base;
		}
		if (saved_regs.size() < 3u) {
			RtDiag("RT calls: only %zu free SGPRs for the call saves (s%s not preserved)\n",
			       saved_regs.size(), saved_regs.size() < 2u ? "81/s82/s83" : "83");
		}
	}
	for (const auto& entry: entries) {
		const auto low = entry.low;
		auto&      words = bodies[low];
		Function function {low, entry.compare_record, entry.record * 16u, {}};
		// Callee-saved regs: inlined bodies reload s76..s83 for their own descriptors, clobbering
		// the caller's ones (the dwords that differ across the arms, e.g. s81/s82). The temps above
		// preserve the caller's values around each body so its image descriptors survive.
		for (const auto& reg: saved_regs) function.body.push_back(MovRegSS(reg.saved, reg.clobbered));
		// s16/s17 take their record dwords again, this time straight from the table: SMEM word 0 is
		// 0xf4000000 | opcode << 18 | sdst << 6 | sbase pair, word 1 is the byte offset with SOFFSET = null.
		// S_LOAD_DWORD (0x00) reads the SRT (s[0:1]); S_BUFFER_LOAD_DWORD (0x08) reads through the
		// table's V#. Both are scalar-memory loads, so wait for them before the body uses the result.
		bool loaded = false;
		// The V# register may hold different definitions on different paths into this call, which the
		// resource tracker cannot see through; load the V# again from where the kernel got it (the SRT).
		uint32_t vsharp_reg = table_load->src0.reg;
		if (direct_base) {
			// No spare SGPRs needed: s[16:17] first hold the table's base (V# dwords 0-1 from the SRT, base
			// address masked to 48 bits), then the record's pointer read through that base.
			function.body.push_back(0xf4000000u | (0x01u << 18u) | (16u << 6u)); // s_load_dwordx2 s[16:17], s[0:1]
			function.body.push_back((0x7du << 25u) | (vsharp_srt_offset & 0x1fffffu));
			function.body.push_back(0xbf8cc07fu);                                  // s_waitcnt lgkmcnt(0)
			function.body.push_back(0x8711ff11u);                                  // s_and_b32 s17, s17, literal
			function.body.push_back(0x0000ffffu);
			function.body.push_back(0xf4000000u | (0x01u << 18u) | (16u << 6u) | 8u); // s_load_dwordx2 s[16:17], s[16:17]
			function.body.push_back(((share_bodies ? index_code : 0x7du) << 25u) |
			                        ((share_bodies ? 8u : entry.record * 16u + 8u) & 0x1fffffu));
			function.body.push_back(0xbf8cc07fu);
		}
		for (uint32_t pointer = 0; pointer < 2u; ++pointer) {
			const auto dword = record_dword[2u + pointer];
			if (dword == UINT32_MAX || direct_base) continue;
			const uint32_t opcode = table_is_buffer ? 0x08u : 0x00u;
			const uint32_t base   = table_is_buffer ? vsharp_reg / 2u : 0u;
			const uint32_t offset = table_is_buffer ? (share_bodies ? 0u : entry.record * 16u) + dword * 4u
			                                        : table_load->offset + dword * 4u;
			function.body.push_back(0xf4000000u | (opcode << 18u) | ((16u + pointer) << 6u) | base);
			function.body.push_back(((share_bodies ? index_code : 0x7du) << 25u) | (offset & 0x1fffffu));
			loaded = true;
		}
		if (loaded) function.body.push_back(0xbf8cc07fu); // s_waitcnt lgkmcnt(0)
		// Map old instruction boundaries to new ones before inserting return restores.
		// A branch targeting a return must land on its restores, not on S_SETPC itself.
		std::vector<size_t> relocated(words.size() + 1u, SIZE_MAX);
		struct BodyBranch { size_t instruction; uint32_t target; };
		std::vector<BodyBranch> branches;
		uint32_t returns = 0;
		for (uint32_t index = 0; index < words.size();) {
			Decoder::Instruction inst;
			Decoder::DecodeInstruction(words, index, inst);
			if (inst.word_count == 0u || index + inst.word_count > words.size()) {
				RtDiag("RT calls: invalid instruction in body 0x%x at word %u\n", low, index);
				return false;
			}
			relocated[index] = function.body.size();
			if (inst.raw[0] == SetPcWordS14) {
				for (const auto& reg: saved_regs)
					function.body.push_back(MovRegSS(reg.clobbered, reg.saved));
				++returns;
			}
			const auto at = function.body.size();
			if (Decoder::IsDirectBranch(inst.opcode)) branches.push_back({at, inst.branch_target});
			function.body.insert(function.body.end(), words.begin() + index,
			                     words.begin() + index + inst.word_count);
			index += inst.word_count;
		}
		relocated[words.size()] = function.body.size();
		for (const auto& branch: branches) {
			const auto target = branch.target / sizeof(uint32_t);
			if (branch.target % sizeof(uint32_t) != 0u || target >= words.size() ||
			    relocated[target] == SIZE_MAX) {
				RtDiag("RT calls: invalid branch target 0x%x in body 0x%x\n", branch.target, low);
				return false;
			}
			const auto delta = static_cast<int64_t>(relocated[target]) -
			                   static_cast<int64_t>(branch.instruction) - 1;
			if (delta < INT16_MIN || delta > INT16_MAX) {
				RtDiag("RT calls: relocated branch out of range in body 0x%x\n", low);
				return false;
			}
			function.body[branch.instruction] = (function.body[branch.instruction] & 0xffff0000u) |
			                                    (static_cast<uint32_t>(delta) & 0xffffu);
		}
		functions.push_back(std::move(function));
	}

	// The real end of the decoded program; anything behind it is padding.
	const size_t original_words = insts.back().pc / sizeof(uint32_t) + insts.back().word_count;
	const size_t return_index   = (insts[call_index].pc / sizeof(uint32_t)) + 1u;

	// Every branch into and out of a function carries a signed 16-bit dword offset, so a function tens
	// of thousands of words away from both the call and the return point cannot be reached. The table is
	// therefore laid out in chunks, and each chunk gets its own entry chain and its own return relay,
	// with the chunks chained to each other. Nothing then depends on the total size, which is what lets
	// the whole table be inlined.
	struct Chunk {
		uint32_t first = 0;
		uint32_t count = 0;
		size_t   entry = 0;
		size_t   ret   = 0;
		size_t   body  = 0;
		size_t   miss  = 0;
	};
	constexpr size_t ChunkWords = 12000u;
	std::vector<Chunk> chunks;
	{
		Chunk   current;
		size_t  used = 0;
		for (uint32_t k = 0; k < functions.size(); ++k) {
			// Three words of compare chain per function, two of relays, one of slack.
			const size_t cost = functions[k].body.size() + 9u;
			if (current.count != 0u && used + cost > ChunkWords) {
				chunks.push_back(current);
				current = Chunk {};
				used    = 0;
			}
			if (current.count == 0u) current.first = k;
			current.count++;
			used += cost;
		}
		if (current.count != 0u) chunks.push_back(current);
	}
	// A feedback table with no used record yet has no functions: one empty chunk, every call logs and returns.
	if (chunks.empty() && feedback) chunks.push_back(Chunk {});
	if (chunks.empty()) {
		RtDiag("RT calls: no chunks for %zu call targets\n", functions.size());
		return false;
	}
	// Feedback: the last miss relay logs the record index and returns to the caller, as the call would have
	// done had it found nothing to run (instead of ending the invocation). Compare + branch + a stop.
	const size_t last_miss_words = feedback ? (group_count != 0u ? 5u : 4u) : 1u;
	{
		// A chunk is [compare chain][jump to miss][return relay][functions][miss relay]. The miss
		// relays chain forward and the last one is the single s_endpgm of the program: the decoder
		// stops at the first s_endpgm, so nothing may follow it.
		size_t cursor = original_words;
		for (auto& chunk: chunks) {
			chunk.entry = cursor;
			size_t chain = 0;
			for (uint32_t k = 0; k < chunk.count; ++k) chain += functions[chunk.first + k].compare_pointer ? 6u : 3u;
			chunk.ret   = cursor + chain + 1u;
			chunk.body  = chunk.ret + 1u;
			cursor      = chunk.body;
			for (uint32_t k = 0; k < chunk.count; ++k) cursor += functions[chunk.first + k].body.size();
			chunk.miss = cursor;
			cursor     = cursor + (&chunk == &chunks.back() ? last_miss_words : 1u);
		}
	}

	out.assign(code.begin(), code.begin() + original_words);
	const auto branch_word = [](uint32_t opcode_word, size_t from, size_t to) {
		const auto delta = static_cast<int64_t>(to) - static_cast<int64_t>(from) - 1;
		return opcode_word | (static_cast<uint32_t>(delta) & 0xffffu);
	};
	// the call becomes a branch to the first chunk's entry chain
	out[return_index - 1u] = branch_word(0xbf820000u, return_index - 1u, chunks[0].entry);
	for (size_t c = 0; c < chunks.size(); ++c) {
		const auto& chunk = chunks[c];
		if (out.size() != chunk.entry) {
			RtDiag("RT calls: chunk %zu starts at %zu, laid out at %zu\n", c, out.size(), chunk.entry);
			return false;
		}
		size_t body = chunk.body;
		for (uint32_t k = 0; k < chunk.count; ++k) {
			// s_cmp_eq_u32 <literal>, s14. SOPC carries the literal in the first operand as SSRC 255
			// and reads it from the following dword (frontend/decode/ScalarAluOps.cpp:267,
			// ShaderDecoder.cpp:267), so the compare is two words; PSSL emits the same form.
			const auto& function = functions[chunk.first + k];
			out.push_back(CmpEqLiteralS14);
			out.push_back(function.address_low);
			if (function.compare_pointer) {
				// Same target reached through several records: also match the record's pointer (s16).
				out.push_back(branch_word(0xbf840000u, out.size(), out.size() + 4u)); // s_cbranch_scc0: next entry
				out.push_back(0xbf0600ffu | (index_code << 8u)); // s_cmp_eq_u32 <literal>, record index register
				out.push_back(function.pointer_low);
			}
			out.push_back(branch_word(0xbf850000u, out.size(), body)); // s_cbranch_scc1
			body += function.body.size();
		}
		// No function of this chunk matched: try the next chunk's compare chain, or, after the last
		// one, end the invocation.
		out.push_back(branch_word(0xbf820000u, out.size(),
		                          c + 1u < chunks.size() ? chunks[c + 1u].entry : chunk.miss));
		out.push_back(
		    branch_word(0xbf820000u, out.size(), c == 0u ? return_index : chunks[c - 1u].ret));
		if (out.size() != chunk.body) {
			RtDiag("RT calls: chunk %zu body starts at %zu, laid out at %zu\n", c, out.size(),
			       chunk.body);
			return false;
		}
		// The bodies follow their own chunk's chains, so the next chunk's entry is where the miss
		// relay ends (the layout above is per chunk, not "all chains first").
		for (uint32_t k = 0; k < chunk.count; ++k) {
			const auto&  body  = functions[chunk.first + k].body;
			const size_t first = out.size();
			out.insert(out.end(), body.begin(), body.end());
			for (uint32_t index = 0; index < body.size();) {
				Decoder::Instruction inst;
				Decoder::DecodeInstruction(body, index, inst);
				const size_t at = first + index;
				if (inst.raw[0] == SetPcWordS14) {
					out[at] = branch_word(0xbf820000u, at, chunk.ret);
				}
				index += inst.word_count;
			}
		}
		if (out.size() != chunk.miss) {
			RtDiag("RT calls: chunk miss relay at %zu, laid out at %zu\n", out.size(), chunk.miss);
			return false;
		}
		// The last miss relay is the only s_endpgm in the program: the decoder stops at the first one.
		if (c + 1u < chunks.size()) {
			out.push_back(branch_word(0xbf820000u, out.size(), chunks[c + 1u].miss));
		} else if (feedback) {
			// Group mode: the lanes of this record belong to another group (or to a function not inlined yet);
			// they are taken out of the set that continues after the call loop.
			if (group_count != 0u) {
				out.push_back(0x80000000u | (0x15u << 23u) | (outer_exec_sgpr << 16u) | (126u << 8u) | outer_exec_sgpr); // s_andn2_b64
			}
			out.push_back(0xbf0600ffu | (index_code << 8u)); // s_cmp_eq_u32 <marker>, <record index register>
			out.push_back(RtFeedback::LogMagic | feedback_slot);
			out.push_back(branch_word(0xbf820000u, out.size(), chunk.ret)); // return to the caller
			out.push_back(0xbf810000u);                                      // s_endpgm, never reached
		} else {
			out.push_back(0xbf810000u);
		}
	}
	return true;
}

// Expands every call site of the original program, one after the other: each expansion appends its inlined
// functions to the end, so the next one is searched for by its pc in the freshly decoded result.
bool ExpandIndirectCalls(const CompileOptions& options, std::span<const uint32_t> code,
                         const Decoder::Program& decoded, std::vector<uint32_t>& out,
                         std::vector<CallInputRead>& call_inputs, std::vector<RtFeedbackSite>& sites) {
	std::vector<uint32_t> call_pcs;
	for (const auto& inst: decoded.instructions) {
		if (IsSwapPcCall(inst)) call_pcs.push_back(inst.pc);
	}
	if (call_pcs.empty()) {
		RtDiag("RT calls: no call site\n");
		return false;
	}
	std::vector<uint32_t> current(code.begin(), code.end());
	Decoder::Program      program = decoded;
	for (size_t n = 0; n < call_pcs.size(); ++n) {
		if (n != 0) Decoder::DecodeProgram(current, program);
		size_t index = SIZE_MAX;
		for (size_t i = 0; i < program.instructions.size(); ++i) {
			if (program.instructions[i].pc == call_pcs[n] && IsSwapPcCall(program.instructions[i])) index = i;
		}
		if (index == SIZE_MAX) {
			RtDiag("RT calls: call site %zu at pc=0x%x was not found again\n", n, static_cast<unsigned>(call_pcs[n]));
			return false;
		}
		std::vector<uint32_t> next;
		if (!ExpandOneCall(options, current, program, index, next, call_inputs, sites)) return false;
		current = std::move(next);
	}
	out = std::move(current);
	return true;
}

} // namespace

// True when the workgroup z index flows into a descriptor (image, sampler, buffer or address
// resource): the host resolves descriptors once per dispatch and cannot know z at that point.
bool WorkgroupZSelectsDescriptor(const IR::Program& ir) {
	std::vector<const IR::Inst*> work;
	std::unordered_set<const IR::Inst*> seen;
	for (auto* block: ir.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == IR::ValueOpcode::GetBuiltin &&
			    inst.Arg(0) == IR::Value(static_cast<uint32_t>(IR::StageInputKind::WorkgroupId)) &&
			    inst.Arg(1) == IR::Value(2u)) {
				work.push_back(&inst);
			}
		}
	}
	while (!work.empty()) {
		const auto* inst = work.back();
		work.pop_back();
		if (!seen.insert(inst).second) continue;
		for (const auto& use: inst->Uses()) {
			switch (use.user->GetOpcode()) {
				case IR::ValueOpcode::GetImageResource:
				case IR::ValueOpcode::GetSamplerResource:
				case IR::ValueOpcode::GetBufferResource:
				case IR::ValueOpcode::GetAddressResource: return true;
				default: work.push_back(use.user); break;
			}
		}
	}
	return false;
}

// With --raytracing every compute kernel that calls through S_SWAPPC_B64 is a ray tracing kernel: its call target is
// read from a table at run time and is resolved when the program is translated (ExpandIndirectCalls), not linked
// statically as a scalar leaf call.
static bool RtOwnsCalls(const CompileOptions& options) {
	return Config::RaytracingEnabled() && options.stage == ShaderType::Compute;
}

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
	if (source.decoded.has_swap_pc && !RtOwnsCalls(options)) {
		PrepareCallTarget(source, options);
		source.code.assign(code.begin(), code.end());
		source.decoded.code = source.code;
	}
	return source;
}

const Decoder::Program& RefreshShaderSource(ShaderSource& source, const IR::SrtRuntime& runtime) {
	auto& reads = source.reads;
	reads.clear();
	IR::SrtReadCapture capture(runtime, reads);
	const auto clean = IR::CleanRuntime(capture.ObservedRuntime());
	IR::SrtWalker walker(source.call_targets, clean);
	uint32_t low = 0, high = 0;
	const auto& call = source.decoded.instructions[source.call->instruction];
	if (!walker.Evaluate(source.call->target[0], low) || !walker.Evaluate(source.call->target[1], high))
		EXIT("shader call at pc 0x%08x has an unavailable scalar target", call.pc);
	// SWAPPC reads the old aliased pair and ignores the target's low two bits.
	const auto address = ((uint64_t {high} << 32u) | low) & ~uint64_t {3};
	EXIT_IF(address == 0u);
	const auto query_reads = reads.size();
	if (source.linked) {
		auto& linked = *source.linked;
		const auto function = std::span(linked.code).subspan(source.code.size());
		if (clean.read_memory(clean.userdata, address, linked.observed_function) &&
		    std::ranges::equal(function, linked.observed_function)) {
			return linked.decoded;
		}
		reads.resize(query_reads);
	}
	ShaderSource::Linked linked {.code = source.code,
	                             .decoded = source.decoded};
	const auto& last = source.decoded.instructions.back();
	EXIT_NOT_IMPLEMENTED(last.opcode != Decoder::Opcode::S_ENDPGM);
	// Keep the caller's data/footer bytes at their native offsets. The leaf's virtual CFG
	// PCs follow decoded instructions; its physical code is independently read and owned.
	const auto function_pc = last.pc + last.word_count * sizeof(uint32_t);
	for (uint32_t index = 0;;) {
		// The observed queue helper is a scalar leaf. Scalar encodings need at most one literal.
		EXIT_IF(index >= 64u || address > UINT64_MAX - (uint64_t {index} + 2u) * sizeof(uint32_t));
		std::array<uint32_t, 2> words {};
		if (!clean.read_memory(clean.userdata, address + uint64_t {index} * sizeof(uint32_t),
		                       std::span(words).first(1)))
			EXIT("shader call at pc 0x%08x cannot read leaf code", call.pc);
		const auto family = Decoder::GetInstructionFamily(words[0]);
		EXIT_NOT_IMPLEMENTED(family != Decoder::Family::SOP1 && family != Decoder::Family::SOP2 &&
		                     family != Decoder::Family::SOPC && family != Decoder::Family::SOPK &&
		                     family != Decoder::Family::SOPP);
		Decoder::Instruction inst;
		Decoder::DecodeInstruction(words, 0u, inst);
		if (inst.word_count == 2u) {
			if (!clean.read_memory(clean.userdata, address + uint64_t {index + 1u} * sizeof(uint32_t),
			                       std::span(words).last(1)))
				EXIT("shader call at pc 0x%08x cannot read leaf literal", call.pc);
			inst = {};
			Decoder::DecodeInstruction(words, 0u, inst);
		}
		inst.pc = index * sizeof(uint32_t);
		if (Decoder::IsDirectBranch(inst.opcode)) inst.branch_target += inst.pc;
		linked.code.insert(linked.code.end(), inst.raw, inst.raw + inst.word_count);
		index += inst.word_count;
		EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::S_SWAPPC_B64 ||
		                     inst.opcode == Decoder::Opcode::S_GETPC_B64 ||
		                     inst.opcode == Decoder::Opcode::S_ENDPGM);
		EXIT_IF(CFG::MayWriteScalarRegister(inst, call.dst.reg) || CFG::MayWriteScalarRegister(inst, call.dst.reg + 1u));
		const bool returns = inst.opcode == Decoder::Opcode::S_SETPC_B64;
		if (returns) {
			EXIT_NOT_IMPLEMENTED(inst.src0.kind != Decoder::OperandKind::Sgpr || inst.src0.reg != call.dst.reg);
			inst.opcode = Decoder::Opcode::S_BRANCH;
			inst.branch_target = call.pc + call.word_count * sizeof(uint32_t);
		} else if (Decoder::IsDirectBranch(inst.opcode)) {
			EXIT_IF(inst.branch_target > UINT32_MAX - function_pc);
			inst.branch_target += function_pc;
		}
		inst.pc += function_pc;
		linked.decoded.instructions.push_back(std::move(inst));
		if (returns) break;
	}
	const auto function_end = linked.decoded.instructions.back().pc +
	                          linked.decoded.instructions.back().word_count * sizeof(uint32_t);
	for (size_t i = source.decoded.instructions.size(); i + 1u < linked.decoded.instructions.size(); ++i) {
		const auto& inst = linked.decoded.instructions[i];
		if (Decoder::IsDirectBranch(inst.opcode))
			EXIT_IF(inst.branch_target < function_pc || inst.branch_target >= function_end);
	}
	reads.resize(query_reads);
	reads.emplace_back(address, (linked.code.size() - source.code.size()) * sizeof(uint32_t));
	linked.decoded.instructions[source.call->instruction].branch_target = function_pc;
	linked.decoded.code = linked.code;
	linked.observed_function.resize(linked.code.size() - source.code.size());
	source.linked = std::move(linked);
	++source.revision;
	return source.linked->decoded;
}

static TranslateResult TranslateProgramImpl(const Decoder::Program& decoded_input, const CompileOptions& options) {
	EXIT_IF(decoded_input.instructions.empty());
	if (decoded_input.has_swap_pc && !RtOwnsCalls(options)) {
		for (const auto& inst: decoded_input.instructions) {
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

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " code_words=%" PRIu64 " decode\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(decoded_input.code.size()));

	// Inlining the call table exists for the ray tracing kernels (--raytracing): a compute kernel that calls through
	// S_SWAPPC_B64 reaches a function whose address is read from a table at run time, so the table and the functions
	// are guest data resolved here. A shader that calls functions without ever touching the BVH instruction is
	// something else, and running it with a rewritten program is how a title ended up jumping to a heap pointer during
	// module load: the guest read output it never got. Those keep the old behaviour and are skipped.
	bool has_bvh = false;
	bool has_inlined_calls = false;
	std::vector<CallInputRead> call_inputs;
	std::vector<RtFeedbackSite> rt_sites;
	std::vector<uint32_t> joined_code;
	Decoder::Program rt_decoded;
	const Decoder::Program* decoded_source = &decoded_input;
	const auto code = decoded_input.code;
	if (options.back_code.empty() && options.stage != ShaderType::Local) {
		has_bvh = std::ranges::any_of(decoded_input.instructions, [](const Decoder::Instruction& i) {
			return i.opcode == Decoder::Opcode::IMAGE_BVH_INTERSECT_RAY;
		});
		if (Config::RaytracingEnabled() && options.stage == ShaderType::Compute && !has_bvh &&
		    CallGroupCount() > 1u && options.input_info.compute != nullptr &&
		    options.input_info.compute->call_pass == UINT32_MAX &&
		    std::ranges::any_of(decoded_input.instructions, IsSwapPcCall)) {
			// The host asks for one program per group of the call table; this request only reports how many.
			TranslateResult groups_result;
			groups_result.needs_call_passes = CallGroupCount();
			return groups_result;
		} else if (Config::RaytracingEnabled() && options.stage == ShaderType::Compute &&
		           std::ranges::any_of(decoded_input.instructions, IsSwapPcCall) &&
		           ExpandIndirectCalls(options, code, decoded_input, joined_code, call_inputs, rt_sites)) {
			Decoder::DecodeProgram(joined_code, rt_decoded);
			decoded_source = &rt_decoded;
			has_inlined_calls = true;
		}
	}
	const Decoder::Program& decoded = *decoded_source;
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " decode instructions=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(decoded.instructions.size()), phase_ms());
	std::string decoded_dump;
	if (options.dump_ir) {
		decoded_dump = Decoder::ProgramToString(decoded);
		if (options.early_dump) {
			LOGF("%s decoded RDNA2 (early):\n%s", GetDumpLabel(options), decoded_dump.c_str());
		}
	}

	// S_SWAPPC_B64 with NULL as the destination is a plain jump (decoded as S_SETPC_B64). With a
	// real destination it is a call that returns; the shaders seen doing that are ray tracing ones
	// (their BVH traversal calls a subroutine around IMAGE_BVH_INTERSECT_RAY). Without ray tracing
	// support the program is not produced and its dispatches are skipped. The ray tracing work
	// replaces this with a check of the --raytracing option, so the skip only applies while that
	// option is off.
	for (const auto& inst: decoded.instructions) {
		if (IsSwapPcCall(inst)) {
			// A dynamic call that was not expanded (ray tracing off, or a call shape the expansion does not take) cannot be
			// translated correctly: the program is not produced and its dispatches are skipped.
			if (!Config::RaytracingEnabled() || !has_inlined_calls) {
				LOGF("RT calls: not expanded: raytracing=%d has_bvh=%d hash=0x%016" PRIx64 "\n",
				     Config::RaytracingEnabled() ? 1 : 0, has_bvh ? 1 : 0, options.shader_hash);
				TranslateResult calls_result;
				calls_result.uses_function_calls = true;
				return calls_result;
			}
			LOGF("%s call (S_SWAPPC_B64 with a destination) at pc=0x%08" PRIx32 " in hash=0x%016" PRIx64 "\n",
			     GetDumpLabel(options), inst.pc, options.shader_hash);
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
	    .stage            = options.stage,
	    .wave_size        = options.wave_size,
	    .shader_hash      = options.shader_hash,
	    .user_data_base   = options.user_data_base,
	    .user_data_count  = static_cast<uint32_t>(options.user_data.size()),
	    .input_info       = options.input_info,
	    .embedded_fetch   = embedded_fetch.loads.empty() ? nullptr : &embedded_fetch,
	    .rt_call_kernel   = has_inlined_calls,
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
	if (decoded.has_swap_pc && !has_inlined_calls) {
		// Leaf returns are resolved CFG edges; native PC values must not escape into GPU data.
		for (const auto* block: ir.blocks)
			for (const auto& inst: *block)
				EXIT_NOT_IMPLEMENTED(inst.GetOpcode() == IR::ValueOpcode::GetShaderBase);
	}
	LowerTessellationMemory(ir, options);
	if (const auto lowered = has_inlined_calls
	                             ? IR::LowerDynamicBufferReads(ir) : IR::DynamicReadStats {};
	    lowered.const_buffer_reads != 0 || lowered.address_reads != 0) {
		LOGF("%s dynamic buffer lowering: stage=%s hash=0x%016" PRIx64
		     " vnumber_reads=%" PRIu32 " address_reads=%" PRIu32 "\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
		     lowered.const_buffer_reads, lowered.address_reads);
		IR::RemoveIdentities(ir);
		IR::EliminateDeadCode(ir.blocks);
	}
	if (has_inlined_calls) {
		// Ray tracing kernels keep descriptor dwords in registers across loops and calls.
		if (const auto folded = IR::SimplifyPhiWebs(ir); folded != 0) {
			LOGF("%s phi web simplification: hash=0x%016" PRIx64 " phis=%" PRIu32 "\n",
			     GetDumpLabel(options), options.shader_hash, folded);
			IR::RemoveIdentities(ir);
			IR::EliminateDeadCode(ir.blocks);
		}
	}
	if (has_inlined_calls) {
		const auto removed = IR::EliminateDuplicateBitcasts(ir.blocks);
		if (removed != 0u) {
			IR::EliminateDeadCode(ir.blocks);
		}
	}
	std::string cfg_dump;
	if (options.dump_ir) {
		cfg_dump = CFG::GraphToString(cfg);
		if (options.early_dump) {
			LOGF("%s native IR before resource tracking:\n%s", GetDumpLabel(options),
			     MakeIrDump(cfg_dump, ir).c_str());
		}
	}
	if (options.stage == ShaderType::Compute && options.input_info.compute != nullptr &&
	    options.input_info.compute->fixed_group_z == UINT32_MAX && WorkgroupZSelectsDescriptor(ir)) {
		TranslateResult slice_result;
		slice_result.needs_group_z = true;
		return slice_result;
	}
	if (options.stage == ShaderType::Compute && options.input_info.compute != nullptr &&
	    options.input_info.compute->fixed_group_z != UINT32_MAX) {
		// Fold the workgroup z index in, so descriptors selected by it resolve on the host.
		const auto z = IR::Value(options.input_info.compute->fixed_group_z);
		for (auto* block: ir.blocks) {
			for (auto& inst: *block) {
				if (inst.GetOpcode() != IR::ValueOpcode::GetBuiltin ||
				    inst.Arg(0) != IR::Value(static_cast<uint32_t>(IR::StageInputKind::WorkgroupId)) ||
				    inst.Arg(1) != IR::Value(2u)) continue;
				const auto uses = inst.Uses();
				for (const auto& use: uses) use.user->SetArg(use.operand, z);
			}
		}
	}
	ir.rt_plain_formatted_indirect = has_inlined_calls;
	ir.gpu_selected_call_payloads = has_inlined_calls;
	IR::TrackResources(ir, decoded, native_cfg, options.input_info);

	TranslateResult result;
	result.program = std::move(ir);
	if (has_inlined_calls) result.call_inputs = std::move(call_inputs);
	if (has_inlined_calls) result.rt_sites = std::move(rt_sites);
	if (options.dump_ir) {
		result.decoded_dump = std::move(decoded_dump);
		result.cfg_dump     = std::move(cfg_dump);
	}
	return result;
}

static CompileResult CompileProgramImpl(TranslateResult translated, const CompileOptions& options,
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
	auto spirv = Spirv::EmitProgram(ir, options.input_info, push_data_start_dword,
	                                options.maintenance8_enabled, options.emit_debug_info);
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

// A ray tracing kernel with its hit functions inlined is a CFG of hundreds of blocks; the passes
// that walk it recurse along the dominator tree and overflow a default 1 MB stack. Under
// --raytracing the compute translation and emission therefore run on a thread with a large stack.
} // namespace Libs::Graphics::ShaderRecompiler

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif
#include <functional>

namespace Libs::Graphics::ShaderRecompiler {

namespace {

void RunOnLargeStack(const std::function<void()>& work) {
	constexpr size_t stack_size = 1024u * 1024u * 1024u;
#ifdef _WIN32
	// An exception must not leave the thread procedure (that terminates the process from a foreign thread and
	// hung it); it is carried back and rethrown on the caller's thread.
	struct Context {
		const std::function<void()>* work;
		std::exception_ptr           error;
	} context {&work, nullptr};
	const auto thread = ::CreateThread(
	    nullptr, stack_size,
	    [](LPVOID parameter) -> DWORD {
		    auto* context = static_cast<Context*>(parameter);
		    try {
			    (*context->work)();
		    } catch (...) {
			    context->error = std::current_exception();
		    }
		    return 0;
	    },
	    &context, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	if (thread == nullptr) {
		work();
		return;
	}
	::WaitForSingleObject(thread, INFINITE);
	::CloseHandle(thread);
	if (context.error) std::rethrow_exception(context.error);
#else
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, stack_size);
	pthread_t thread;
	if (pthread_create(
	        &thread, &attr,
	        [](void* parameter) -> void* {
		        (*static_cast<const std::function<void()>*>(parameter))();
		        return nullptr;
	        },
	        const_cast<std::function<void()>*>(&work)) != 0) {
		pthread_attr_destroy(&attr);
		work();
		return;
	}
	pthread_attr_destroy(&attr);
	pthread_join(thread, nullptr);
#endif
}

} // namespace

TranslateResult TranslateProgram(std::span<const uint32_t> code, const CompileOptions& options) {
	auto source = PrepareShaderSource(code, options);
	return TranslateProgram(source.decoded, options);
}

TranslateResult TranslateProgram(const Decoder::Program& decoded, const CompileOptions& options) {
	// The large stack is only needed because the ray tracing kernels turn into CFGs of hundreds of
	// blocks. Running *every* compute shader on that thread is a far broader change.
	if (!Config::RaytracingEnabled() || options.stage != ShaderType::Compute) {
		return TranslateProgramImpl(decoded, options);
	}
	std::optional<TranslateResult> result;
	RunOnLargeStack([&] { result = TranslateProgramImpl(decoded, options); });
	return std::move(*result);
}

CompileResult CompileProgram(TranslateResult translated, const CompileOptions& options,
                             const IR::ResourceSpecialization& specialization,
                             uint32_t push_data_start_dword) {
	if (!Config::RaytracingEnabled() || options.stage != ShaderType::Compute) {
		return CompileProgramImpl(std::move(translated), options, specialization, push_data_start_dword);
	}
	std::optional<CompileResult> result;
	RunOnLargeStack([&] {
		result = CompileProgramImpl(std::move(translated), options, specialization, push_data_start_dword);
	});
	return std::move(*result);
}

} // namespace Libs::Graphics::ShaderRecompiler