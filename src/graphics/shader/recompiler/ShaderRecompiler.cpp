#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/stringUtils.h"
#include "graphics/shader/recompiler/backend/spirv/SpirvEmitter.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/frontend/translate/Translate.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
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
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fmt/format.h>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

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
		case ShaderType::Mesh: return "MS";
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

void DumpFailedStructurizeShader(const CompileOptions& options, std::span<const uint32_t> code,
                                 const CFG::Graph& cfg) {
	// Tiny unit-test fixtures are not useful captures. The UFC pixel shaders are thousands of
	// words; keep those so the CFG can be replayed without running the match.
	constexpr uint64_t kMinDumpWords = 2048;
	if (code.size() < kMinDumpWords) {
		return;
	}

	const auto dir  = Config::GetShaderLogFolder() / "cfg_fail";
	Common::File::CreateDirectories(dir);
	auto bin_path = dir / fmt::format("{}_{:016x}.bin", StageName(options.stage), options.shader_hash);
	auto txt_path = dir / fmt::format("{}_{:016x}.cfg.txt", StageName(options.stage),
	                                  options.shader_hash);

	{
		Common::File file(bin_path);
		if (file.IsInvalid()) {
			LOGF_COLOR(Log::Color::BrightRed, "Can't create CFG-fail dump: %s\n",
			           Common::PathToString(bin_path).c_str());
		} else {
			file.Write(code.data(), static_cast<uint32_t>(code.size_bytes()));
			LOGF("%s CFG fail dump: %s words=%" PRIu64 "\n", GetDumpLabel(options),
			     Common::PathToString(bin_path).c_str(), static_cast<uint64_t>(code.size()));
		}
	}
	{
		const auto text = fmt::format("stage={} hash=0x{:016x} words={} blocks={} loops={}\n"
		                              "reason={}\n\n{}",
		                              StageName(options.stage), options.shader_hash, code.size(),
		                              cfg.blocks.size(), cfg.natural_loops.size(),
		                              cfg.unsupported_reason, CFG::GraphToString(cfg));
		Common::File file(txt_path);
		if (!file.IsInvalid()) {
			file.Write(text.data(), static_cast<uint32_t>(text.size()));
		}
	}
}

constexpr uint64_t kUfcHangCsHash = 0xea0aceac518ec52dull;

bool EmbeddedFetchHasBranch(Decoder::Opcode opcode);

bool ParseHexU64(const char* text, uint64_t* out) {
	if (text == nullptr || out == nullptr) {
		return false;
	}
	while (*text == ' ' || *text == '\t') {
		++text;
	}
	if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
		text += 2;
	}
	if (*text == '\0') {
		return false;
	}
	errno                            = 0;
	char*                    end     = nullptr;
	const unsigned long long value   = std::strtoull(text, &end, 16);
	if (end == text || errno == ERANGE) {
		return false;
	}
	*out = static_cast<uint64_t>(value);
	return true;
}

bool EnvListContainsHash(const char* env_name, uint64_t hash) {
	const char* env = std::getenv(env_name);
	if (env == nullptr || env[0] == '\0') {
		return false;
	}
	const char* cursor = env;
	while (*cursor != '\0') {
		while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
			++cursor;
		}
		if (*cursor == '\0') {
			break;
		}
		const char* start = cursor;
		while (*cursor != '\0' && *cursor != ',') {
			++cursor;
		}
		std::string token(start, static_cast<size_t>(cursor - start));
		while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
			token.pop_back();
		}
		uint64_t parsed = 0;
		if (ParseHexU64(token.c_str(), &parsed) && parsed == hash) {
			return true;
		}
	}
	return false;
}

bool ShouldDumpShader(const CompileOptions& options) {
	return options.shader_hash == kUfcHangCsHash ||
	       EnvListContainsHash("KYTY_DUMP_SHADER_HASH", options.shader_hash);
}

std::filesystem::path ShaderHangDumpDir() {
	if (const char* env = std::getenv("KYTY_SHADER_DUMP_DIR"); env != nullptr && env[0] != '\0') {
		return env;
	}
	return Config::GetShaderLogFolder() / "hang_cs";
}

void WriteDumpBytes(const std::filesystem::path& path, const void* data, size_t size) {
	Common::File::CreateDirectories(path.parent_path());
	Common::File file(path);
	if (file.IsInvalid()) {
		LOGF_COLOR(Log::Color::BrightRed, "Can't create shader dump: %s\n",
		           Common::PathToString(path).c_str());
		return;
	}
	if (size != 0) {
		file.Write(data, static_cast<uint32_t>(size));
	}
	LOGF("shader dump: %s bytes=%" PRIu64 "\n", Common::PathToString(path).c_str(),
	     static_cast<uint64_t>(size));
}

void WriteDumpText(const std::filesystem::path& path, std::string_view text) {
	WriteDumpBytes(path, text.data(), text.size());
}

std::filesystem::path SuspectDumpBase(const CompileOptions& options) {
	return ShaderHangDumpDir() /
	       fmt::format("{}_{:016x}", StageName(options.stage), options.shader_hash);
}

void LogDecodedOpcodeSummary(const CompileOptions& options, const Decoder::Program& decoded) {
	std::array<uint32_t, static_cast<size_t>(Decoder::Family::EXP) + 1u> family {};
	std::map<Decoder::Opcode, uint32_t>                                  opcodes;
	uint32_t ds = 0;
	uint32_t ds_gds = 0;
	uint32_t ds_inc = 0;
	uint32_t ds_atomic_rtn = 0;
	uint32_t branches = 0;
	uint32_t barriers = 0;
	uint32_t waitcnt = 0;
	uint32_t buffer_atomic = 0;
	uint32_t image_atomic = 0;
	uint32_t unsupported = 0;
	for (const auto& inst: decoded.instructions) {
		const auto family_index = static_cast<size_t>(inst.family);
		if (family_index < family.size()) {
			family[family_index]++;
		}
		opcodes[inst.opcode]++;
		if (inst.family == Decoder::Family::DS) {
			++ds;
			if (inst.gds) {
				++ds_gds;
			}
			switch (inst.opcode) {
				case Decoder::Opcode::DS_INC_U32:
				case Decoder::Opcode::DS_INC_RTN_U32:
				case Decoder::Opcode::DS_DEC_U32:
				case Decoder::Opcode::DS_DEC_RTN_U32:
				case Decoder::Opcode::DS_RSUB_U32:
				case Decoder::Opcode::DS_RSUB_RTN_U32: ++ds_inc; break;
				default: break;
			}
			switch (inst.opcode) {
				case Decoder::Opcode::DS_ADD_RTN_U32:
				case Decoder::Opcode::DS_SUB_RTN_U32:
				case Decoder::Opcode::DS_RSUB_RTN_U32:
				case Decoder::Opcode::DS_INC_RTN_U32:
				case Decoder::Opcode::DS_DEC_RTN_U32: ++ds_atomic_rtn; break;
				default: break;
			}
		}
		if (EmbeddedFetchHasBranch(inst.opcode)) {
			++branches;
		}
		if (inst.opcode == Decoder::Opcode::S_BARRIER) {
			++barriers;
		}
		if (inst.opcode == Decoder::Opcode::S_WAITCNT ||
		    inst.opcode == Decoder::Opcode::S_WAITCNT_DEPCTR) {
			++waitcnt;
		}
		if (inst.opcode >= Decoder::Opcode::BUFFER_ATOMIC_SWAP &&
		    inst.opcode <= Decoder::Opcode::BUFFER_ATOMIC_FMAX) {
			++buffer_atomic;
		}
		switch (inst.opcode) {
			case Decoder::Opcode::IMAGE_ATOMIC_SWAP:
			case Decoder::Opcode::IMAGE_ATOMIC_ADD:
			case Decoder::Opcode::IMAGE_ATOMIC_UMIN:
			case Decoder::Opcode::IMAGE_ATOMIC_UMAX:
			case Decoder::Opcode::IMAGE_ATOMIC_AND:
			case Decoder::Opcode::IMAGE_ATOMIC_OR:
			case Decoder::Opcode::IMAGE_ATOMIC_XOR: ++image_atomic; break;
			case Decoder::Opcode::UNSUPPORTED: ++unsupported; break;
			default: break;
		}
	}
	LOGF("%s dump summary: stage=%s hash=0x%016" PRIx64
	     " insts=%" PRIu64 " ds=%u gds=%u wrap_inc_dec_rsub=%u ds_rtn=%u "
	     "branches=%u barriers=%u waitcnt=%u buffer_atomic=%u image_atomic=%u unsupported=%u "
	     "wave=%u scratch=%u\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(decoded.instructions.size()), ds, ds_gds, ds_inc, ds_atomic_rtn,
	     branches, barriers, waitcnt, buffer_atomic, image_atomic, unsupported, options.wave_size,
	     options.scratch_dwords);
	if (options.stage == ShaderType::Compute && options.input_info.compute != nullptr) {
		const auto& cs = *options.input_info.compute;
		LOGF("%s compute input: local=%ux%ux%u wave=%u lds_dwords=%u tg_size_en=%s "
		     "workgroup_sgpr=%d\n",
		     GetDumpLabel(options), cs.threads_num[0], cs.threads_num[1], cs.threads_num[2],
		     cs.wave_size, cs.lds_size_dwords, cs.tg_size_en ? "true" : "false",
		     cs.workgroup_register);
	}
	for (size_t i = 0; i < family.size(); ++i) {
		if (family[i] == 0) {
			continue;
		}
		LOGF("%s family %s: %u\n", GetDumpLabel(options),
		     std::string(magic_enum::enum_name(static_cast<Decoder::Family>(i))).c_str(),
		     family[i]);
	}
	for (const auto& [opcode, count]: opcodes) {
		if (count == 0) {
			continue;
		}
		const auto name = magic_enum::enum_name(opcode);
		if (count < 4 && !(opcode >= Decoder::Opcode::DS_ADD_U32 &&
		                    opcode <= Decoder::Opcode::DS_WRXCHG_RTN_B32) &&
		    opcode != Decoder::Opcode::S_BARRIER) {
			continue;
		}
		LOGF("%s opcode %s: %u\n", GetDumpLabel(options), std::string(name).c_str(), count);
	}
}

std::filesystem::path WithDumpSuffix(std::filesystem::path base, const char* suffix) {
	base += suffix;
	return base;
}

void DumpSuspectDecoded(const CompileOptions& options, std::span<const uint32_t> code,
                        const Decoder::Program& decoded, std::string_view decoded_dump) {
	const auto base = SuspectDumpBase(options);
	WriteDumpBytes(WithDumpSuffix(base, ".bin"), code.data(), code.size_bytes());
	WriteDumpText(WithDumpSuffix(base, ".rdna2"), decoded_dump);
	LogDecodedOpcodeSummary(options, decoded);
}

void DumpSuspectCfg(const CompileOptions& options, const CFG::Graph& cfg) {
	LOGF("%s cfg loops=%" PRIu64 " back_edges=%" PRIu64 " blocks=%" PRIu64 " entry=%u\n",
	     GetDumpLabel(options), static_cast<uint64_t>(cfg.natural_loops.size()),
	     static_cast<uint64_t>(cfg.back_edges.size()), static_cast<uint64_t>(cfg.blocks.size()),
	     cfg.entry_block);
	for (uint32_t i = 0; i < cfg.natural_loops.size(); ++i) {
		const auto& loop = cfg.natural_loops[i];
		const auto* header = cfg.FindBlock(loop.header);
		const auto* latch  = cfg.FindBlock(loop.latch);
		LOGF("%s loop[%u]: header=%u pc=0x%08x latch=%u pc=0x%08x body=%" PRIu64
		     " exits=%" PRIu64 " merge=%u continue=%u\n",
		     GetDumpLabel(options), i, loop.header, header != nullptr ? header->start_pc : 0u,
		     loop.latch, latch != nullptr ? latch->start_pc : 0u,
		     static_cast<uint64_t>(loop.body_blocks.size()),
		     static_cast<uint64_t>(loop.exit_blocks.size()), loop.merge, loop.continue_block);
	}
	WriteDumpText(WithDumpSuffix(SuspectDumpBase(options), ".cfg.txt"), CFG::GraphToString(cfg));
}

void DumpSuspectIrAndSpirv(const CompileOptions& options, const IR::Program& ir,
                           std::span<const uint32_t> spirv, std::string_view ir_dump) {
	std::array<uint32_t, static_cast<size_t>(IR::ValueOpcode::Count)> counts {};
	uint32_t insts = 0;
	uint32_t shared_atomic = 0;
	uint32_t buffer_atomic = 0;
	uint32_t image_atomic = 0;
	uint32_t barriers = 0;
	for (const auto* block: ir.blocks) {
		for (const auto& inst: *block) {
			++insts;
			const auto opcode = inst.GetOpcode();
			const auto index  = static_cast<size_t>(opcode);
			if (index < counts.size()) {
				counts[index]++;
			}
			if (IR::SharedAccessOf(opcode) == IR::SharedAccess::Atomic) {
				++shared_atomic;
			}
			if (IR::BufferAccessOf(opcode) == IR::BufferAccess::Atomic) {
				++buffer_atomic;
			}
			if (IR::ImageOpcodeInfoOf(opcode).access == IR::ImageAccess::Atomic) {
				++image_atomic;
			}
			if (opcode == IR::ValueOpcode::Barrier) {
				++barriers;
			}
		}
	}
	LOGF("%s ir summary: blocks=%" PRIu64 " insts=%u shared_atomic=%u buffer_atomic=%u "
	     "image_atomic=%u barriers=%u spirv_words=%" PRIu64 " dma=%s xor=%s\n",
	     GetDumpLabel(options), static_cast<uint64_t>(ir.blocks.size()), insts, shared_atomic,
	     buffer_atomic, image_atomic, barriers, static_cast<uint64_t>(spirv.size()),
	     ir.info.uses_dma ? "true" : "false", ir.info.has_bitwise_xor ? "true" : "false");
	for (size_t i = 0; i < counts.size(); ++i) {
		if (counts[i] == 0) {
			continue;
		}
		const auto opcode = static_cast<IR::ValueOpcode>(i);
		if (counts[i] < 8 && IR::SharedAccessOf(opcode) == IR::SharedAccess::None &&
		    IR::BufferAccessOf(opcode) != IR::BufferAccess::Atomic &&
		    IR::ImageOpcodeInfoOf(opcode).access != IR::ImageAccess::Atomic &&
		    opcode != IR::ValueOpcode::Barrier) {
			continue;
		}
		LOGF("%s ir opcode %s: %u\n", GetDumpLabel(options),
		     std::string(IR::ValueOpcodeName(opcode)).c_str(), counts[i]);
	}
	const auto base = SuspectDumpBase(options);
	WriteDumpText(WithDumpSuffix(base, ".ir.txt"), ir_dump);
	WriteDumpBytes(WithDumpSuffix(base, ".spv"), spirv.data(),
	               spirv.size() * sizeof(uint32_t));
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

using EmbeddedFetchLoad = Frontend::EmbeddedFetchLoad;
using EmbeddedFetchData = Frontend::EmbeddedFetchPlan;

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

EmbeddedFetchData DetectEmbeddedVertexFetch(const Decoder::Program&      decoded,
                                            const ShaderVertexInputInfo* input_info,
                                            uint32_t user_data_base, uint32_t user_data_count,
                                            uint32_t wave_size) {
	EmbeddedFetchData data;
	data.loads.reserve(input_info->resources_num);
	int32_t vertex_offset_candidate   = -1;
	int32_t instance_offset_candidate = -1;
	bool    vertex_offset_conflict    = false;
	bool    instance_offset_conflict  = false;

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
		// seeds S_NGG_VERTEX_INDEX in v5 and S_NGG_INSTANCE_INDEX in v8, then applies the
		// corresponding direct-draw offsets before fetching.
		const bool vertex_index_accumulator =
		    IsDecodedVgpr(inst.dst) &&
		    (inst.dst.reg == 0 || (user_data_base == 8 && inst.dst.reg == 5));
		const bool instance_index_accumulator =
		    IsDecodedVgpr(inst.dst) && (inst.dst.reg == (user_data_base == 8 ? 8u : 3u));
		uint32_t   sad_zero = 0;
		const bool index_offset_add =
		    (vertex_index_accumulator || instance_index_accumulator) &&
		    IsDecodedSgpr(inst.src0) &&
		    ((inst.opcode == Decoder::Opcode::V_ADD_I32 && IsDecodedVgpr(inst.src1) &&
		      inst.src1.reg == inst.dst.reg) ||
		     (user_data_base == 8 && (inst.dst.reg == 5 || inst.dst.reg == 8) &&
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
						if (data.loads.empty()) {
							if (!vertex_offset_conflict) {
								data.vertex_offset_sgpr = vertex_offset_candidate;
							}
							if (!instance_offset_conflict) {
								data.instance_offset_sgpr = instance_offset_candidate;
							}
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

Decoder::Program DecodeFusedProgram(std::span<const uint32_t> front, std::span<const uint32_t> back,
                                    std::vector<uint32_t>& joined_code) {
	EXIT_IF(back.empty());
	Decoder::Program result;
	uint32_t         front_words = 0;
	while (front_words < front.size()) {
		auto& inst = result.instructions.emplace_back();
		Decoder::DecodeInstruction(front, front_words, inst);
		front_words += inst.word_count;
		if (inst.opcode == Decoder::Opcode::S_SETPC_B64) {
			EXIT_NOT_IMPLEMENTED(inst.src0.kind != Decoder::OperandKind::Sgpr ||
			                     inst.src0.reg != 6u);
			break;
		}
		EXIT_NOT_IMPLEMENTED(inst.opcode == Decoder::Opcode::S_ENDPGM);
	}
	EXIT_IF(result.instructions.empty() ||
	        result.instructions.back().opcode != Decoder::Opcode::S_SETPC_B64);
	joined_code.assign(front.begin(), front.begin() + front_words);
	joined_code.insert(joined_code.end(), back.begin(), back.end());
	// The merged-stage ABI passes the back shader in s[6:7]. Give that handoff an
	// ordinary CFG edge, retaining both bodies in one register and LDS lifetime.
	joined_code[front_words - 1u] = 0xbf820000u; // s_branch to the following instruction
	result.instructions.back()    = {};
	Decoder::DecodeInstruction(joined_code, front_words - 1u, result.instructions.back());
	Decoder::Program back_program;
	Decoder::DecodeProgram(back, back_program);
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

} // namespace

TranslateResult TranslateProgram(std::span<const uint32_t> code, const CompileOptions& options) {
	if (code.empty()) {
		EXIT("shader recompiler input is empty\n");
	}
	if (options.stage != ShaderType::Compute && options.stage != ShaderType::Vertex &&
	    options.stage != ShaderType::Pixel && options.stage != ShaderType::Mesh) {
		EXIT("shader recompiler received unsupported stage %u\n",
		     static_cast<unsigned>(options.stage));
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
	std::vector<uint32_t> joined_code;
	if (options.stage == ShaderType::Mesh) {
		decoded = DecodeFusedProgram(code, options.back_code, joined_code);
	} else {
		Decoder::DecodeProgram(code, decoded);
	}
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " decode instructions=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(decoded.instructions.size()), phase_ms());

	const bool dump_suspect = ShouldDumpShader(options);
	std::string decoded_dump;
	if (options.dump_ir || dump_suspect) {
		decoded_dump = Decoder::ProgramToString(decoded);
		if (options.early_dump) {
			LOGF("%s decoded RDNA2 (early):\n%s", GetDumpLabel(options), decoded_dump.c_str());
		}
	}
	if (dump_suspect) {
		DumpSuspectDecoded(options, code, decoded, decoded_dump);
	}

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph\n", GetDumpLabel(options),
	     StageName(options.stage), options.shader_hash);
	auto cfg = CFG::BuildGraph(decoded);
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " CFG BuildGraph blocks=%" PRIu64
	     " loops=%" PRIu64 " back_edges=%" PRIu64 " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(cfg.blocks.size()), static_cast<uint64_t>(cfg.natural_loops.size()),
	     static_cast<uint64_t>(cfg.back_edges.size()), phase_ms());
	bool        dispatcher_fallback = false;
	std::string dispatcher_reason;
	CFG::Graph  unstructured_cfg;
	if (cfg.irreducible) {
		dispatcher_fallback = true;
		dispatcher_reason   = cfg.unsupported_reason;
		LogDispatcherFallback(options, cfg, "build", dispatcher_reason);
	} else {
		unstructured_cfg = cfg;
		LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " CFG Structurize\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
		if (!CFG::Structurize(cfg)) {
			dispatcher_fallback      = true;
			dispatcher_reason        = cfg.unsupported_reason;
			const auto failure_kind  = cfg.failure_kind;
			const auto failure_block = cfg.failure_block;
			LogDispatcherFallback(options, cfg, "structurize", dispatcher_reason);
			DumpFailedStructurizeShader(options, code, cfg);
			cfg                    = unstructured_cfg;
			cfg.unsupported        = true;
			cfg.failure_kind       = failure_kind;
			cfg.failure_block      = failure_block;
			cfg.unsupported_reason = dispatcher_reason;
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
	if (dump_suspect) {
		DumpSuspectCfg(options, cfg);
	}

	const ShaderVertexInputInfo*  vertex  = nullptr;
	const ShaderPixelInputInfo*   pixel   = nullptr;
	const ShaderComputeInputInfo* compute = nullptr;
	switch (options.stage) {
		case ShaderType::Vertex:
		case ShaderType::Mesh: vertex = options.input_info.vertex; break;
		case ShaderType::Pixel:
			pixel = options.input_info.pixel;
			break;
		case ShaderType::Compute:
			compute = options.input_info.compute;
			break;
		default: break;
	}
	EmbeddedFetchData embedded_fetch;
	if (options.stage == ShaderType::Vertex && vertex->fetch_embedded) {
		embedded_fetch = DetectEmbeddedVertexFetch(decoded, vertex, options.user_data_base,
		                                           static_cast<uint32_t>(options.user_data.size()),
		                                           options.wave_size);
		if (!embedded_fetch.loads.empty()) {
			LOGF("%s embedded vertex fetch plan: detected=%" PRIu64 "\n", GetDumpLabel(options),
			     static_cast<uint64_t>(embedded_fetch.loads.size()));
		}
	}
	Frontend::TranslateOptions translate_options {
	    .stage               = options.stage,
	    .wave_size           = options.wave_size,
	    .shader_hash         = options.shader_hash,
	    .user_data_base      = options.user_data_base,
	    .user_data_count     = static_cast<uint32_t>(options.user_data.size()),
	    .scratch_dwords      = options.scratch_dwords,
	    .dispatcher_fallback = dispatcher_fallback,
	    .cfg_failure_kind    = cfg.failure_kind,
	    .fallback_reason     = dispatcher_reason.empty() ? cfg.unsupported_reason
	                                                    : dispatcher_reason,
	    .vertex              = vertex,
	    .pixel               = pixel,
	    .compute             = compute,
	    .embedded_fetch      = embedded_fetch.loads.empty() ? nullptr : &embedded_fetch,
	};
	const auto finish_value_ir = [&](IR::Program& ir) {
		IR::RewriteToSsa(ir.blocks);
		IR::ConstantPropagationPass(ir.blocks);
		IR::ResolveControlFlowIdentities(ir);
		IR::RemoveIdentities(ir.blocks);
		IR::EliminateDeadCode(ir.blocks);
		const auto read_lane_stats = IR::EliminateReadLane(ir, ir.wave_size);
		if (read_lane_stats.rewritten_reads != 0) {
			LOGF("%s read-lane elimination: reads=%" PRIu32 "\n", GetDumpLabel(options),
			     read_lane_stats.rewritten_reads);
			IR::ConstantPropagationPass(ir.blocks);
			IR::ResolveControlFlowIdentities(ir);
			IR::RemoveIdentities(ir.blocks);
			IR::EliminateDeadCode(ir.blocks);
		}
	};
	bool uses_dispatch_structurize = false;
	if (!dispatcher_fallback) {
		for (const auto& block: cfg.blocks) {
			if (block.terminator.kind == CFG::TerminatorKind::DispatchSwitch) {
				uses_dispatch_structurize = true;
				break;
			}
		}
	}
	if (uses_dispatch_structurize) {
		LOGF("%s using DispatcherFull resource import: stage=%s hash=0x%016" PRIx64
		     " blocks=%" PRIu64 "\n",
		     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
		     static_cast<uint64_t>(cfg.blocks.size()));
	}

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
	IR::Program ir;
	if (uses_dispatch_structurize) {
		// DispatcherFull merges every guest block at one loop join, so SGPR descriptor
		// dwords are no longer invariant. Track on the original CFG and import the plan.
		auto track_ir =
		    Frontend::TranslateProgram(decoded, unstructured_cfg, translate_options);
		finish_value_ir(track_ir);
		IR::BuildSrtPlan(track_ir);
		IR::TrackResources(track_ir);
		ir = Frontend::TranslateProgram(decoded, cfg, translate_options);
		finish_value_ir(ir);
		IR::ImportTrackedResources(ir, track_ir);
		IR::EliminateDeadCode(ir.blocks);
	} else {
		ir = Frontend::TranslateProgram(decoded, cfg, translate_options);
		finish_value_ir(ir);
		IR::BuildSrtPlan(ir);
		IR::EliminateDeadCode(ir.blocks);
		IR::TrackResources(ir);
		IR::EliminateDeadCode(ir.blocks);
	}
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " IR TranslateProgram blocks=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(ir.blocks.size()), phase_ms());
	if (options.stage == ShaderType::Vertex) {
		ir.info.vertex_offset_sgpr   = embedded_fetch.vertex_offset_sgpr;
		ir.info.instance_offset_sgpr = embedded_fetch.instance_offset_sgpr;
	}

	TranslateResult result;
	result.program = std::move(ir);
	if (options.dump_ir || dump_suspect) {
		result.decoded_dump = std::move(decoded_dump);
		result.cfg_dump     = CFG::GraphToString(cfg);
	}
	return result;
}

CompileResult CompileProgram(TranslateResult translated, const CompileOptions& options,
                             const IR::ResourceSpecialization& specialization,
                             uint32_t push_data_start_dword) {
	const auto emit_begin = std::chrono::steady_clock::now();
	auto& ir = translated.program;
	IR::ApplyResourceSpecialization(ir, specialization);
	IR::RemoveIdentities(ir.blocks);
	IR::EliminateDeadCode(ir.blocks);

	const ShaderVertexInputInfo*  vertex  = nullptr;
	const ShaderPixelInputInfo*   pixel   = nullptr;
	const ShaderComputeInputInfo* compute = nullptr;
	switch (options.stage) {
		case ShaderType::Vertex:
		case ShaderType::Mesh: vertex = options.input_info.vertex; break;
		case ShaderType::Pixel: pixel = options.input_info.pixel; break;
		case ShaderType::Compute: compute = options.input_info.compute; break;
		default: EXIT("invalid shader stage\n");
	}

	IR::ShaderInfoOptions info_options;
	info_options.vertex  = vertex;
	info_options.pixel   = pixel;
	info_options.compute = compute;
	IR::CollectShaderInfo(ir, info_options);
	IR::AllocateBindings(ir, push_data_start_dword);
	Spirv::AnalyzeProgramRequirements(ir);
	std::string ir_dump;
	const bool dump_suspect = ShouldDumpShader(options);
	if (options.dump_ir || dump_suspect) {
		ir_dump = MakeIrDump(translated.cfg_dump, ir);
		if (options.early_dump) {
			LOGF("%s native IR and bindings (early):\n%s", GetDumpLabel(options), ir_dump.c_str());
		}
	}

	LOGF("%s phase begin: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash);
	auto spirv = Spirv::EmitProgram(ir, options.input_info);
	LOGF("%s phase end: stage=%s hash=0x%016" PRIx64 " SPIR-V EmitProgram words=%" PRIu64
	     " elapsed_ms=%" PRIu64 "\n",
	     GetDumpLabel(options), StageName(options.stage), options.shader_hash,
	     static_cast<uint64_t>(spirv.size()),
	     static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                               std::chrono::steady_clock::now() - emit_begin)
	                               .count()));
	if (dump_suspect) {
		DumpSuspectIrAndSpirv(options, ir, spirv, ir_dump);
	}
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
