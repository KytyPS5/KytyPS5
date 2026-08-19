#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/ValueProgram.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "graphics/shader/recompiler/ir/passes/SsaRewrite.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fmt/format.h>
#include <map>
#include <span>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler {

namespace {

bool ReadZeroMemory(void*, uint64_t, uint32_t* value) {
	if (value == nullptr) {
		return false;
	}
	*value = 0;
	return true;
}

const char* GetDumpLabel(const CompileOptions& options) {
	return options.dump_label != nullptr ? options.dump_label : "ShaderRecompiler";
}

std::string MakeIrDump(const CFG::Graph& cfg, const IR::Program& ir) {
	std::string dump = "CFG:\n";
	dump += CFG::GraphToString(cfg);
	dump += "\nIR:\n";
	dump += fmt::format("mode={}\n", ir.dispatcher_fallback ? "dispatcher" : "structured");
	dump += ir.values != nullptr ? IR::ValueProgramToString(*ir.values) : IR::ProgramToString(ir);
	return dump;
}

const char* StageName(ShaderType stage) {
	switch (stage) {
		case ShaderType::Compute: return "CS";
		case ShaderType::Vertex: return "VS";
		case ShaderType::Pixel: return "PS";
		default: return "unknown";
	}
}

void LogDispatcherFallback(const CompileOptions& options, const CFG::Graph& cfg, const char* phase,
                           const std::string& reason) {
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
	     static_cast<uint64_t>(cfg.back_edges.size()), reason.c_str());
}

enum class EmbeddedFetchValueType {
	Unknown,
	Constant,
	AttribTable,
	Attrib,
	BufferTable,
	Buffer,
	Index
};

struct EmbeddedFetchSgprInfo {
	EmbeddedFetchValueType type      = EmbeddedFetchValueType::Unknown;
	int                    attrib_id = 0;
	uint32_t               value     = 0;
	std::vector<uint32_t>  prolog_loads;
};

struct EmbeddedFetchVgprInfo {
	EmbeddedFetchValueType type = EmbeddedFetchValueType::Unknown;
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

struct EmbeddedFetchLoad {
	uint32_t              pc         = 0;
	int                   attrib_id  = -1;
	uint32_t              components = 0;
	std::vector<uint32_t> prolog_loads;
};

struct EmbeddedFetchData {
	std::vector<EmbeddedFetchLoad> loads;
	int32_t                        vertex_offset_sgpr = -1;
};

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

bool EmbeddedFetchHasBranch(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::S_SETPC_B64:
		case Decoder::Opcode::S_BRANCH:
		case Decoder::Opcode::S_CBRANCH_SCC0:
		case Decoder::Opcode::S_CBRANCH_SCC1:
		case Decoder::Opcode::S_CBRANCH_VCCZ:
		case Decoder::Opcode::S_CBRANCH_VCCNZ:
		case Decoder::Opcode::S_CBRANCH_EXECZ:
		case Decoder::Opcode::S_CBRANCH_EXECNZ: return true;
		default: return false;
	}
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

void AppendUniquePcs(std::vector<uint32_t>& dst, const std::vector<uint32_t>& src) {
	dst.reserve(dst.size() + src.size());
	for (auto pc: src) {
		if (std::find(dst.begin(), dst.end(), pc) == dst.end()) {
			dst.push_back(pc);
		}
	}
}

EmbeddedFetchData DetectEmbeddedVertexFetch(const Decoder::Program&      decoded,
                                            const ShaderVertexInputInfo* input_info,
                                            uint32_t user_data_base, uint32_t user_data_count,
                                            uint32_t wave_size) {
	EmbeddedFetchData data;
	data.loads.reserve(input_info->resources_num);
	int32_t offset_candidate = -1;
	bool    offset_conflict  = false;

	const int shift_regs = 8;
	const int attrib_reg = input_info->fetch_attrib_reg + shift_regs;
	const int buffer_reg = input_info->fetch_buffer_reg + shift_regs;

	std::array<EmbeddedFetchSgprInfo, 108> sgprs {};
	std::array<EmbeddedFetchVgprInfo, 256> vgprs {};
	EmbeddedFetchVectorLanes               vector_lanes;
	const bool                             track_vector_lanes =
	    std::none_of(decoded.instructions.begin(), decoded.instructions.end(),
	                 [](const auto& inst) { return EmbeddedFetchHasBranch(inst.opcode); });

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
		// seeds S_NGG_VERTEX_INDEX in v5 and applies the same offset there before fetching.
		const bool vertex_index_accumulator =
		    IsDecodedVgpr(inst.dst) &&
		    (inst.dst.reg == 0 || (user_data_base == 8 && inst.dst.reg == 5));
		uint32_t   sad_zero = 0;
		const bool vertex_offset_add =
		    vertex_index_accumulator && IsDecodedSgpr(inst.src0) &&
		    ((inst.opcode == Decoder::Opcode::V_ADD_I32 && IsDecodedVgpr(inst.src1) &&
		      inst.src1.reg == inst.dst.reg) ||
		     (user_data_base == 8 && inst.dst.reg == 5 &&
		      inst.opcode == Decoder::Opcode::V_SAD_U32 && IsDecodedVgpr(inst.src2) &&
		      inst.src2.reg == inst.dst.reg &&
		      TryDecodedOperandConstant(sgprs, inst.src1, sad_zero) && sad_zero == 0));
		if (data.loads.empty() && vertex_offset_add) {
			const auto reg = DecodedSgprReg(inst.src0);
			if (reg >= user_data_base && reg - user_data_base < user_data_count) {
				if (offset_candidate >= 0 && offset_candidate != static_cast<int32_t>(reg)) {
					offset_conflict = true;
				} else {
					offset_candidate = static_cast<int32_t>(reg);
				}
			}
		}
		switch (inst.opcode) {
			case Decoder::Opcode::V_WRITELANE_B32: {
				uint32_t lane = 0;
				if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgprs.size()) {
					vgprs[inst.dst.reg] = {};
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
						dst.prolog_loads.clear();
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
					dst.prolog_loads.clear();
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
								dst.prolog_loads = {inst.pc};
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
								dst.prolog_loads = {inst.pc};
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
								dst.prolog_loads = sgprs[DecodedSgprReg(inst.src1)].prolog_loads;
								dst.prolog_loads.push_back(inst.pc);
							}
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
						}
					} else {
						ClearEmbeddedFetchSgprs(sgprs, inst.dst, DecodedDstSize(inst));
					}
				} else if (inst.opcode == Decoder::Opcode::V_CNDMASK_B32) {
					if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgprs.size()) {
						ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg);
					}
					if (IsDecodedVgpr(inst.dst) && inst.dst.reg < vgprs.size() &&
					    IsDecodedVgpr(inst.src0) && inst.src0.reg == 8 &&
					    IsDecodedVgpr(inst.src1) && inst.src1.reg == 5) {
						vgprs[inst.dst.reg].type = EmbeddedFetchValueType::Index;
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
							dst.prolog_loads.clear();
						} else {
							ClearEmbeddedFetchSgprs(sgprs, inst.dst, 1);
						}
					}
				} else if (IsEmbeddedFetchBufferLoad(inst)) {
					if (IsDecodedVgpr(inst.src0) && inst.src0.reg < vgprs.size() &&
					    vgprs[inst.src0.reg].type == EmbeddedFetchValueType::Index &&
					    IsDecodedSgpr(inst.src1) && DecodedSgprReg(inst.src1) < sgprs.size() &&
					    sgprs[DecodedSgprReg(inst.src1)].type == EmbeddedFetchValueType::Buffer) {
						const auto&       buffer = sgprs[DecodedSgprReg(inst.src1)];
						EmbeddedFetchLoad load;
						load.pc           = inst.pc;
						load.attrib_id    = buffer.attrib_id;
						load.components   = DecodedDstSize(inst);
						load.prolog_loads = buffer.prolog_loads;
						if (data.loads.empty() && !offset_conflict) {
							data.vertex_offset_sgpr = offset_candidate;
						}
						data.loads.push_back(load);
					}
				}
				break;
		}
		if (inst.opcode == Decoder::Opcode::V_MOVRELD_B32) {
			vector_lanes.clear();
		} else if (inst.opcode != Decoder::Opcode::V_WRITELANE_B32 && IsDecodedVgpr(inst.dst)) {
			for (uint32_t i = 0; i < EmbeddedFetchDstSize(inst) && inst.dst.reg + i < vgprs.size();
			     i++) {
				ClearEmbeddedFetchVectorLanes(&vector_lanes, inst.dst.reg + i);
			}
		}
	}

	return data;
}

const EmbeddedFetchLoad* FindEmbeddedFetchLoad(const std::vector<EmbeddedFetchLoad>& loads,
                                               uint32_t                              pc) {
	for (const auto& load: loads) {
		if (load.pc == pc) {
			return &load;
		}
	}
	return nullptr;
}

bool EmbeddedFetchPcInList(const std::vector<uint32_t>& pcs, uint32_t pc) {
	return std::find(pcs.begin(), pcs.end(), pc) != pcs.end();
}

bool IsIrFetchPrologLoad(const IR::Instruction& inst) {
	return inst.op == IR::Opcode::SLoadDword || inst.op == IR::Opcode::SBufferLoadDword;
}

int ResolveEmbeddedFetchResource(const ShaderVertexInputInfo* input_info,
                                 const EmbeddedFetchLoad&     load) {
	if (load.attrib_id >= 0 && load.attrib_id < input_info->resources_num &&
	    input_info->resources_dst[load.attrib_id].attr_id == load.attrib_id) {
		return load.attrib_id;
	}
	for (int i = 0; i < input_info->resources_num; i++) {
		const auto& dst = input_info->resources_dst[i];
		if (dst.attr_id == load.attrib_id &&
		    load.components <= static_cast<uint32_t>(std::max(dst.registers_num, 1))) {
			return i;
		}
	}
	for (int i = 0; i < input_info->resources_num; i++) {
		if (input_info->resources_dst[i].attr_id == load.attrib_id) {
			return i;
		}
	}
	return -1;
}

uint32_t RewriteEmbeddedVertexFetches(IR::Program& ir, const ShaderVertexInputInfo* input_info,
                                      const std::vector<EmbeddedFetchLoad>& loads) {
	if (loads.empty()) {
		return 0;
	}

	std::vector<uint32_t> prolog_pcs;
	prolog_pcs.reserve(loads.size());
	for (const auto& load: loads) {
		AppendUniquePcs(prolog_pcs, load.prolog_loads);
	}

	auto*    mutable_input_info = const_cast<ShaderVertexInputInfo*>(input_info);
	uint32_t rewritten          = 0;
	for (auto& block: ir.blocks) {
		for (auto& inst: block.instructions) {
			if (IsIrFetchPrologLoad(inst) && EmbeddedFetchPcInList(prolog_pcs, inst.pc)) {
				auto pc = inst.pc;
				inst    = {};
				inst.pc = pc;
				inst.op = IR::Opcode::ControlNop;
				continue;
			}

			if (inst.op != IR::Opcode::BufferLoadDword) {
				continue;
			}
			const auto* load = FindEmbeddedFetchLoad(loads, inst.pc);
			if (load == nullptr || inst.memory.data_dwords != load->components ||
			    inst.dst.kind != IR::OperandKind::Register ||
			    inst.dst.reg.file != IR::RegisterFile::Vector) {
				continue;
			}

			const auto resource_id = ResolveEmbeddedFetchResource(input_info, *load);
			if (resource_id < 0 || resource_id >= input_info->resources_num) {
				LOGF("ShaderRecompiler VS embedded fetch remap failed: pc=0x%08" PRIx32
				     " attrib=%d resources=%d\n",
				     inst.pc, load->attrib_id, input_info->resources_num);
				continue;
			}

			inst.op                         = IR::Opcode::LoadInputF32;
			inst.input_info.attr            = static_cast<uint32_t>(resource_id);
			inst.input_info.chan            = 0;
			inst.input_info.component_count = load->components;
			inst.memory                     = {};
			inst.src_count                  = 0;
			mutable_input_info->resource_fetch_components[resource_id] =
			    std::max(mutable_input_info->resource_fetch_components[resource_id],
			             static_cast<int>(load->components));
			rewritten += load->components;
		}
	}

	return rewritten;
}

} // namespace

bool TryRecompile(std::span<const uint32_t> code, const CompileOptions& options,
                  CompileResult& result, std::string* error) {
	if (code.empty()) {
		if (error != nullptr) {
			*error = "invalid shader recompiler input";
		}
		return false;
	}
	if (options.stage != ShaderType::Compute && options.stage != ShaderType::Vertex &&
	    options.stage != ShaderType::Pixel) {
		if (error != nullptr) {
			*error = "shader recompiler supports compute, vertex, and pixel stages";
		}
		return false;
	}

	const auto compile_begin = std::chrono::steady_clock::now();
	const auto phase_ms      = [&compile_begin]() {
		return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
		                                 std::chrono::steady_clock::now() - compile_begin)
		                                 .count());
	};

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " code_words=%" PRIu64 " decode\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(code.size()));

	Decoder::Program decoded;
	if (!Decoder::DecodeProgram(code, decoded, error)) {
		return false;
	}
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

	CFG::Graph cfg;
	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph\n", GetDumpLabel(options),
	     StageName(options.stage), options.shader_hash);
	if (!CFG::BuildGraph(decoded, cfg, error)) {
		return false;
	}
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph blocks=%" PRIu64
	     " loops=%" PRIu64 " back_edges=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(cfg.blocks.size()), static_cast<uint64_t>(cfg.natural_loops.size()),
	     static_cast<uint64_t>(cfg.back_edges.size()), phase_ms());
	bool        dispatcher_fallback = false;
	std::string dispatcher_reason;
	if (cfg.irreducible) {
		dispatcher_fallback = true;
		dispatcher_reason   = cfg.unsupported_reason;
		LogDispatcherFallback(options, cfg, "build", dispatcher_reason);
	} else {
		std::string structure_error;
		const auto  unstructured_cfg = cfg;
		LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG Structurize\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
		if (!CFG::Structurize(cfg, &structure_error)) {
			dispatcher_fallback      = true;
			dispatcher_reason        = structure_error;
			const auto failure_kind  = cfg.failure_kind;
			const auto failure_block = cfg.failure_block;
			LogDispatcherFallback(options, cfg, "structurize", dispatcher_reason);
			cfg                    = unstructured_cfg;
			cfg.unsupported        = true;
			cfg.failure_kind       = failure_kind;
			cfg.failure_block      = failure_block;
			cfg.unsupported_reason = structure_error;
		} else {
			LOGF("%s structured CFG success: blocks=%" PRIu64 "\n", GetDumpLabel(options),
			     static_cast<uint64_t>(cfg.blocks.size()));
		}
		LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " CFG Structurize blocks=%" PRIu64
		     " loops=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
		     static_cast<uint64_t>(cfg.blocks.size()),
		     static_cast<uint64_t>(cfg.natural_loops.size()), phase_ms());
	}

	IR::Program ir;
	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " IR LowerProgram\n", GetDumpLabel(options),
	     StageName(options.stage), options.shader_hash);
	const auto lower_program = [&](IR::Program& output) {
		output = {};
		if (!IR::LowerProgram(decoded, cfg, options.stage, options.wave_size, output, error)) {
			return false;
		}
		output.lane_mask_mode  = options.lane_mask_mode;
		output.shader_hash     = options.shader_hash;
		output.user_data_base  = options.user_data_base;
		output.user_data_count = options.user_data_count;
		return true;
	};
	if (!lower_program(ir)) {
		return false;
	}

	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " IR LowerProgram blocks=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(ir.blocks.size()), phase_ms());
	const ShaderVertexInputInfo*  vertex  = nullptr;
	const ShaderPixelInputInfo*   pixel   = nullptr;
	const ShaderComputeInputInfo* compute = nullptr;
	switch (options.stage) {
		case ShaderType::Vertex: vertex = options.input_info.vertex; break;
		case ShaderType::Pixel: pixel = options.input_info.pixel; break;
		case ShaderType::Compute: compute = options.input_info.compute; break;
		default: break;
	}
	EmbeddedFetchData embedded_fetch;
	if (options.stage == ShaderType::Vertex && vertex->fetch_embedded) {
		embedded_fetch = DetectEmbeddedVertexFetch(decoded, vertex, ir.user_data_base,
		                                           ir.user_data_count, options.wave_size);
		auto rewritten = RewriteEmbeddedVertexFetches(ir, vertex, embedded_fetch.loads);
		if (rewritten > 0 || !embedded_fetch.loads.empty()) {
			LOGF("%s embedded vertex fetch rewrite: detected=%" PRIu64 " rewritten=%" PRIu32 "\n",
			     GetDumpLabel(options), static_cast<uint64_t>(embedded_fetch.loads.size()),
			     rewritten);
		}
	}
	ir.dispatcher_fallback = dispatcher_fallback;
	if (!dispatcher_reason.empty()) {
		ir.fallback_reason = dispatcher_reason;
	}

	ir.values = std::make_shared<IR::ValueProgram>();
	if (!Frontend::TranslateProgram(ir, *ir.values, vertex, pixel, compute, error)) {
		return false;
	}
	ir.blocks.clear();
	IR::RewriteToSsa(ir.values->blocks);
	IR::ConstantPropagationPass(ir.values->blocks);
	for (auto& info: ir.values->block_info) {
		info.condition       = info.condition.Resolve();
		info.indirect_target = info.indirect_target.Resolve();
	}
	IR::RemoveIdentities(ir.values->blocks);
	IR::EliminateDeadCode(ir.values->blocks);
	const auto read_lane_stats = IR::EliminateReadLane(*ir.values, ir.wave_size);
	if (read_lane_stats.rewritten_reads != 0) {
		LOGF("%s read-lane elimination: reads=%" PRIu32 "\n", GetDumpLabel(options),
		     read_lane_stats.rewritten_reads);
		IR::ConstantPropagationPass(ir.values->blocks);
		IR::RemoveIdentities(ir.values->blocks);
		IR::EliminateDeadCode(ir.values->blocks);
	}
	std::string srt_error;
	if (!IR::BuildSrtPlan(ir, &srt_error)) {
		LOGF("%s SRT planning failed: %s\n", GetDumpLabel(options), srt_error.c_str());
		if (error != nullptr) {
			*error = std::move(srt_error);
		}
		return false;
	}
	IR::EliminateDeadCode(ir.values->blocks);
	if (!IR::TrackResources(ir, error)) {
		return false;
	}
	IR::EliminateDeadCode(ir.values->blocks);
	if (options.stage == ShaderType::Vertex) {
		ir.info.vertex_offset_sgpr = embedded_fetch.vertex_offset_sgpr;
	}

	IR::ResourceSnapshot resources;
	if (options.resource_snapshot != nullptr) {
		resources = *options.resource_snapshot;
	} else {
		IR::SrtRuntime           runtime;
		std::array<uint32_t, 64> zero_user_data {};
		if (options.user_data == nullptr) {
			for (uint32_t i = 2; i < zero_user_data.size(); i += 4) {
				zero_user_data[i] = UINT32_MAX;
			}
		}
		const auto* user_data =
		    options.user_data != nullptr ? options.user_data : zero_user_data.data();
		runtime.user_data   = std::span<const uint32_t>(user_data, options.user_data_count);
		runtime.shader_base = options.shader_base != 0 ? options.shader_base
		                                               : reinterpret_cast<uint64_t>(code.data());
		runtime.read_memory = options.read_memory;
		runtime.read_specialization_memory = options.read_specialization_memory;
		if (runtime.read_memory == nullptr && options.user_data == nullptr) {
			runtime.read_memory = ReadZeroMemory;
		}
		runtime.userdata         = options.read_memory_data;
		runtime.flat_memory_base = options.flat_memory_base;
		if (!IR::MaterializeResources(ir, runtime, resources, error)) {
			return false;
		}
	}
	if (!IR::SpecializeResources(ir, resources, error)) {
		return false;
	}

	IR::ShaderInfoOptions info_options;
	info_options.vertex  = vertex;
	info_options.pixel   = pixel;
	info_options.compute = compute;
	if (!IR::CollectShaderInfo(ir, info_options, error)) {
		return false;
	}
	IR::BindingLayoutOptions layout_options;
	layout_options.descriptor_set       = options.descriptor_set;
	layout_options.push_constant_offset = options.push_constant_offset;
	if (!IR::AllocateBindings(ir, layout_options, error)) {
		return false;
	}
	if (!Spirv::AnalyzeProgramRequirements(ir, error)) {
		return false;
	}
	std::string ir_dump;
	if (options.dump_ir) {
		ir_dump = MakeIrDump(cfg, ir);
		if (options.early_dump) {
			LOGF("%s native IR and bindings (early):\n%s", GetDumpLabel(options), ir_dump.c_str());
		}
	}

	std::vector<uint32_t> spirv;
	std::string           emit_error;
	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
	if (!Spirv::EmitProgram(ir, resources, options.input_info, spirv, &emit_error)) {
		LOGF("%s typed SPIR-V emit failed: %s\n", GetDumpLabel(options), emit_error.c_str());
		if (dispatcher_fallback && error != nullptr) {
			*error = fmt::format("dispatcher fallback failed after {}: {}",
			                     dispatcher_reason.c_str(), emit_error.c_str());
			LOGF("%s dispatcher fallback emit failed: %s\n", GetDumpLabel(options), error->c_str());
		} else if (error != nullptr) {
			*error = emit_error;
		}
		return false;
	}
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram words=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(spirv.size()), phase_ms());
	result.spirv     = std::move(spirv);
	result.program   = std::move(ir);
	result.resources = std::move(resources);
	if (options.dump_ir) {
		result.decoded_dump = std::move(decoded_dump);
		result.ir_dump      = std::move(ir_dump);
	} else {
		result.decoded_dump.clear();
		result.ir_dump.clear();
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler
