#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ResourceTracking.h"
#include "graphics/shader/recompiler/ScalarProvenance.h"
#include "graphics/shader/recompiler/ShaderCFG.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderIR.h"
#include "graphics/shader/recompiler/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/SpirvEmitter.h"
#include "graphics/shader/recompiler/SrtPatcher.h"
#include "graphics/shader/recompiler/SrtWalker.h"

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
	dump += IR::ProgramToString(ir);
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

std::string FormatCfgFailure(const CFG::Graph& cfg, const CompileOptions& options,
                             const std::string& reason) {
	const auto block_id = cfg.failure_block != UINT32_MAX ? cfg.failure_block : cfg.entry_block;
	return CFG::FormatBlockDiagnostic(cfg, block_id, StageName(options.stage), reason);
}

bool InstructionMaySplitSpirvBlock(const IR::Instruction& inst) {
	switch (inst.op) {
		case IR::Opcode::SLoadDword:
		case IR::Opcode::SBufferLoadDword:
		case IR::Opcode::BufferLoadUbyte:
		case IR::Opcode::BufferLoadSbyte:
		case IR::Opcode::BufferLoadUshort:
		case IR::Opcode::BufferLoadSshort:
		case IR::Opcode::BufferLoadDword:
		case IR::Opcode::BufferStoreByte:
		case IR::Opcode::BufferStoreShort:
		case IR::Opcode::BufferStoreDword:
		case IR::Opcode::AtomicSwapU32:
		case IR::Opcode::AtomicAddU32:
		case IR::Opcode::AtomicSubU32:
		case IR::Opcode::AtomicSMinI32:
		case IR::Opcode::AtomicUMinU32:
		case IR::Opcode::AtomicSMaxI32:
		case IR::Opcode::AtomicUMaxU32:
		case IR::Opcode::AtomicAndU32:
		case IR::Opcode::AtomicOrU32:
		case IR::Opcode::AtomicXorU32:
		case IR::Opcode::FlatLoadUbyte:
		case IR::Opcode::FlatLoadSbyte:
		case IR::Opcode::FlatLoadUshort:
		case IR::Opcode::FlatLoadSshort:
		case IR::Opcode::FlatLoadDword:
		case IR::Opcode::FlatStoreByte:
		case IR::Opcode::FlatStoreShort:
		case IR::Opcode::FlatStoreDword:
		case IR::Opcode::DsMinF32:
		case IR::Opcode::DsMaxF32:
		case IR::Opcode::DsWriteByte:
		case IR::Opcode::DsWriteShort:
		case IR::Opcode::DsWriteB32:
		case IR::Opcode::DsWriteAddtidB32:
		case IR::Opcode::DsAppend:
		case IR::Opcode::DsConsume:
		case IR::Opcode::ImageStore:
		case IR::Opcode::Export: return true;
		default: return false;
	}
}

bool NeedsDispatcherForStructuredLoopHeader(const IR::Program& ir, std::string* reason) {
	for (const auto& block: ir.blocks) {
		if (!block.terminator.loop_header) {
			continue;
		}
		for (const auto& inst: block.instructions) {
			if (!InstructionMaySplitSpirvBlock(inst)) {
				continue;
			}
			if (reason != nullptr) {
				*reason = fmt::format("loop header block {} contains an instruction whose SPIR-V "
				                      "lowering emits internal control flow: {}",
				                      block.id, IR::InstructionToString(inst).c_str());
			}
			return true;
		}
	}
	return false;
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
	return op.kind == Decoder::OperandKind::VccLo
	           ? 106u
	           : (op.kind == Decoder::OperandKind::VccHi ? 107u : op.reg);
}

bool IsDecodedVgpr(const Decoder::Operand& op) {
	return op.kind == Decoder::OperandKind::Vgpr;
}

uint32_t DecodedDstSize(const Decoder::Instruction& inst) {
	return std::max(inst.data_dwords, 1u);
}

uint32_t EmbeddedFetchDstSize(const Decoder::Instruction& inst) {
	return inst.opcode == Decoder::Opcode::VMadU64U32 ? 2u : DecodedDstSize(inst);
}

bool EmbeddedFetchHasBranch(Decoder::Opcode opcode) {
	switch (opcode) {
		case Decoder::Opcode::SSetpcB64:
		case Decoder::Opcode::SBranch:
		case Decoder::Opcode::SCbranchScc0:
		case Decoder::Opcode::SCbranchScc1:
		case Decoder::Opcode::SCbranchVccz:
		case Decoder::Opcode::SCbranchVccnz:
		case Decoder::Opcode::SCbranchExecz:
		case Decoder::Opcode::SCbranchExecnz: return true;
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
		case Decoder::Opcode::SLoadDword:
		case Decoder::Opcode::SLoadDwordx2:
		case Decoder::Opcode::SLoadDwordx4:
		case Decoder::Opcode::SLoadDwordx8:
		case Decoder::Opcode::SLoadDwordx16: return true;
		default: return false;
	}
}

bool IsEmbeddedFetchBufferLoad(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::BufferLoadFormatX:
		case Decoder::Opcode::BufferLoadFormatXy:
		case Decoder::Opcode::BufferLoadFormatXyz:
		case Decoder::Opcode::BufferLoadFormatXyzw: return true;
		default: return false;
	}
}

bool IsEmbeddedFetchAttribPropagationAlu(const Decoder::Instruction& inst) {
	switch (inst.opcode) {
		case Decoder::Opcode::SBfeU32:
		case Decoder::Opcode::SAndB32:
		case Decoder::Opcode::SAddI32:
		case Decoder::Opcode::SAddU32:
		case Decoder::Opcode::SLshlB32: return true;
		default: return false;
	}
}

int BufferTableAttribFromOffset(uint32_t raw_offset, int dword) {
	return static_cast<int>((raw_offset + static_cast<uint32_t>(dword) * 4u) / 16u);
}

void AppendUniquePcs(std::vector<uint32_t>* dst, const std::vector<uint32_t>& src) {
	EXIT_IF(dst == nullptr);
	dst->reserve(dst->size() + src.size());
	for (auto pc: src) {
		if (std::find(dst->begin(), dst->end(), pc) == dst->end()) {
			dst->push_back(pc);
		}
	}
}

EmbeddedFetchData DetectEmbeddedVertexFetch(const Decoder::Program&      decoded,
                                            const ShaderVertexInputInfo* input_info,
                                            uint32_t user_data_base, uint32_t user_data_count,
                                            uint32_t wave_size) {
	EmbeddedFetchData data;
	if (input_info == nullptr || !input_info->fetch_embedded) {
		return data;
	}
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
		if (data.loads.empty() && inst.opcode == Decoder::Opcode::VAddI32 &&
		    IsDecodedVgpr(inst.dst) && inst.dst.reg == 0 && IsDecodedSgpr(inst.src0) &&
		    IsDecodedVgpr(inst.src1) && inst.src1.reg == 0) {
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
			case Decoder::Opcode::VWritelaneB32: {
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
			case Decoder::Opcode::VReadlaneB32: {
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
			case Decoder::Opcode::SMovB32:
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
			case Decoder::Opcode::SMovkI32:
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
				} else if (inst.opcode == Decoder::Opcode::VCndmaskB32) {
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
								case Decoder::Opcode::SAndB32: dst.value = src0 & src1; break;
								case Decoder::Opcode::SLshlB32:
									dst.value = src0 << (src1 & 31u);
									break;
								case Decoder::Opcode::SBfeU32:
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
		if (inst.opcode == Decoder::Opcode::VMovreldB32) {
			vector_lanes.clear();
		} else if (inst.opcode != Decoder::Opcode::VWritelaneB32 && IsDecodedVgpr(inst.dst)) {
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
	if (input_info == nullptr) {
		return -1;
	}
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

uint32_t RewriteEmbeddedVertexFetches(IR::Program* ir, const ShaderVertexInputInfo* input_info,
                                      const std::vector<EmbeddedFetchLoad>& loads) {
	if (ir == nullptr || input_info == nullptr || loads.empty()) {
		return 0;
	}

	std::vector<uint32_t> prolog_pcs;
	prolog_pcs.reserve(loads.size());
	for (const auto& load: loads) {
		AppendUniquePcs(&prolog_pcs, load.prolog_loads);
	}

	auto*    mutable_input_info = const_cast<ShaderVertexInputInfo*>(input_info);
	uint32_t rewritten          = 0;
	for (auto& block: ir->blocks) {
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
			if (load == nullptr || inst.memory.component_index >= load->components) {
				continue;
			}

			const auto resource_id = ResolveEmbeddedFetchResource(input_info, *load);
			if (resource_id < 0 || resource_id >= input_info->resources_num) {
				LOGF("ShaderRecompiler VS embedded fetch remap failed: pc=0x%08" PRIx32
				     " attrib=%d resources=%d\n",
				     inst.pc, load->attrib_id, input_info->resources_num);
				continue;
			}

			inst.op              = IR::Opcode::LoadInputF32;
			inst.input_info.attr = static_cast<uint32_t>(resource_id);
			inst.input_info.chan = inst.memory.component_index;
			inst.memory          = {};
			inst.src_count       = 0;

			mutable_input_info->resource_fetch_components[resource_id] =
			    std::max(mutable_input_info->resource_fetch_components[resource_id],
			             static_cast<int>(inst.input_info.chan + 1u));
			rewritten++;
		}
	}

	return rewritten;
}

} // namespace

bool TryRecompile(std::span<const uint32_t> code, const CompileOptions& options,
                  CompileResult* result, std::string* error) {
	if (code.empty() || result == nullptr) {
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
	if (!Decoder::DecodeProgram(code, &decoded, error)) {
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
	if (!CFG::BuildGraph(decoded, &cfg, error)) {
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
		dispatcher_fallback   = true;
		dispatcher_reason     = cfg.unsupported_reason;
		const auto diagnostic = FormatCfgFailure(cfg, options, cfg.unsupported_reason);
		LOGF("%s irreducible CFG detected: %s\n", GetDumpLabel(options), diagnostic.c_str());
	} else {
		std::string structure_error;
		const auto  unstructured_cfg = cfg;
		LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG Structurize\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
		if (!CFG::Structurize(&cfg, &structure_error)) {
			const auto diagnostic = FormatCfgFailure(cfg, options, structure_error);
			LOGF("%s structured CFG bug/failure: %s\n", GetDumpLabel(options), diagnostic.c_str());
			dispatcher_fallback      = true;
			dispatcher_reason        = structure_error;
			const auto failure_kind  = cfg.failure_kind;
			const auto failure_block = cfg.failure_block;
			cfg                      = unstructured_cfg;
			cfg.unsupported          = true;
			cfg.failure_kind         = failure_kind;
			cfg.failure_block        = failure_block;
			cfg.unsupported_reason   = structure_error;
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
	if (!IR::LowerProgram(decoded, cfg, options.stage, options.wave_size, &ir, error)) {
		return false;
	}
	ir.lane_mask_mode  = options.lane_mask_mode;
	ir.shader_hash     = options.shader_hash;
	ir.user_data_base  = options.user_data_base;
	ir.user_data_count = options.user_data_count;
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " IR LowerProgram blocks=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(ir.blocks.size()), phase_ms());
	EmbeddedFetchData embedded_fetch;
	if (options.stage == ShaderType::Vertex && options.vertex_input_info != nullptr &&
	    options.vertex_input_info->fetch_embedded) {
		embedded_fetch =
		    DetectEmbeddedVertexFetch(decoded, options.vertex_input_info, ir.user_data_base,
		                              ir.user_data_count, options.wave_size);
		auto rewritten =
		    RewriteEmbeddedVertexFetches(&ir, options.vertex_input_info, embedded_fetch.loads);
		if (rewritten > 0 || !embedded_fetch.loads.empty()) {
			LOGF("%s embedded vertex fetch rewrite: detected=%" PRIu64 " rewritten=%" PRIu32 "\n",
			     GetDumpLabel(options), static_cast<uint64_t>(embedded_fetch.loads.size()),
			     rewritten);
		}
	}
	if (!IR::BuildScalarProvenance(&ir, error)) {
		return false;
	}
	std::string srt_error;
	if (!IR::BuildSrtPlan(&ir, &srt_error)) {
		LOGF("%s SRT planning failed: %s\n", GetDumpLabel(options), srt_error.c_str());
		if (error != nullptr) {
			*error = std::move(srt_error);
		}
		return false;
	}
	ir.dispatcher_fallback = dispatcher_fallback;
	if (!dispatcher_reason.empty()) {
		ir.fallback_reason = dispatcher_reason;
	}

	if (!IR::PatchSrtReads(&ir, error) || !IR::TrackResources(&ir, error)) {
		return false;
	}
	if (options.stage == ShaderType::Vertex) {
		ir.info.vertex_offset_sgpr = embedded_fetch.vertex_offset_sgpr;
	}
	if (!dispatcher_fallback) {
		std::string emitter_reason;
		if (NeedsDispatcherForStructuredLoopHeader(ir, &emitter_reason)) {
			dispatcher_fallback    = true;
			dispatcher_reason      = emitter_reason;
			ir.dispatcher_fallback = true;
			ir.cfg_failure_kind    = CFG::FailureKind::StructuredControlFlow;
			ir.fallback_reason     = emitter_reason;
			LOGF("%s structured CFG emitter fallback: %s\n", GetDumpLabel(options),
			     emitter_reason.c_str());
		}
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
		runtime.read_memory = options.read_memory != nullptr ? options.read_memory
		                      : options.user_data == nullptr ? ReadZeroMemory
		                                                     : nullptr;
		runtime.userdata    = options.read_memory_data;
		runtime.flat_memory_base = options.flat_memory_base;
		if (!IR::MaterializeResources(ir, runtime, &resources, error)) {
			return false;
		}
	}
	if (!IR::SpecializeResources(&ir, resources, error)) {
		return false;
	}

	ShaderVertexInputInfo  default_vertex {};
	ShaderPixelInputInfo   default_pixel {};
	ShaderComputeInputInfo default_compute {};
	IR::ShaderInfoOptions  info_options;
	info_options.vertex =
	    options.vertex_input_info != nullptr ? options.vertex_input_info : &default_vertex;
	info_options.pixel =
	    options.pixel_input_info != nullptr ? options.pixel_input_info : &default_pixel;
	info_options.compute =
	    options.compute_input_info != nullptr ? options.compute_input_info : &default_compute;
	if (!IR::CollectShaderInfo(&ir, info_options, error)) {
		return false;
	}
	IR::BindingLayoutOptions layout_options;
	layout_options.descriptor_set       = options.descriptor_set;
	layout_options.push_constant_offset = options.push_constant_offset;
	if (!IR::AllocateBindings(&ir, layout_options, error)) {
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
	if (!Spirv::EmitProgram(ir, resources, options.vertex_input_info, options.pixel_input_info,
	                        options.compute_input_info, &spirv, &emit_error)) {
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
	if (dispatcher_fallback) {
		LOGF("%s dispatcher fallback used: %s\n", GetDumpLabel(options), dispatcher_reason.c_str());
	}

	result->spirv     = std::move(spirv);
	result->program   = std::move(ir);
	result->resources = std::move(resources);
	if (options.dump_ir) {
		result->decoded_dump = std::move(decoded_dump);
		result->ir_dump      = std::move(ir_dump);
	} else {
		result->decoded_dump.clear();
		result->ir_dump.clear();
	}
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler
