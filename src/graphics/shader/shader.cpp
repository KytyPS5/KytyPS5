#include "graphics/shader/shader.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderVertexMetadata.h"
#include "libs/errno.h"
#include "spirv-tools/libspirv.h"
#include "spirv-tools/libspirv.hpp"
#include "spirv-tools/optimizer.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fmt/format.h>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace Libs::Graphics {

struct ShaderBinaryInfo {
	uint8_t  signature[7];
	uint8_t  version;
	uint32_t pssl_or_cg  : 1;
	uint32_t cached      : 1;
	uint32_t type        : 4;
	uint32_t source_type : 2;
	uint32_t length      : 24;
	uint8_t  chunk_usage_base_offset_dw;
	uint8_t  num_input_usage_slots;
	uint8_t  is_srt                 : 1;
	uint8_t  is_srt_used_info_valid : 1;
	uint8_t  is_extended_usage_info : 1;
	uint8_t  reserved2              : 5;
	uint8_t  reserved3;
	uint32_t hash0;
	uint32_t hash1;
	uint32_t crc32;
};

static std::unique_ptr<std::unordered_map<uint64_t, ShaderMappedData>> g_shader_map;
static std::mutex                                                      g_shader_map_mutex;

struct ShaderStageProgramKey {
	ShaderType                     stage          = ShaderType::Unknown;
	ShaderLaneMaskMode             lane_mask_mode = ShaderLaneMaskMode::NativeWave;
	uint64_t                       shader_hash    = 0;
	ShaderId                       program_id;
	Config::ShaderOptimizationType optimization_type = Config::ShaderOptimizationType::None;
	bool                           validation        = false;

	bool operator==(const ShaderStageProgramKey& other) const {
		return stage == other.stage && lane_mask_mode == other.lane_mask_mode &&
		       shader_hash == other.shader_hash && program_id == other.program_id &&
		       optimization_type == other.optimization_type && validation == other.validation;
	}
};

struct ShaderStageProgramKeyHash {
	size_t operator()(const ShaderStageProgramKey& key) const {
		uint32_t hash = 0;
		auto     mix  = [&hash](uint32_t value) {
			hash ^= Common::hash32(value) + 0x9e3779b9u + (hash << 6u) + (hash >> 2u);
		};

		mix(static_cast<uint32_t>(key.stage));
		mix(static_cast<uint32_t>(key.lane_mask_mode));
		mix(Common::hash64(key.shader_hash));
		mix(key.program_id.hash0);
		mix(key.program_id.crc32);
		mix(static_cast<uint32_t>(key.program_id.ids.size()));
		for (auto value: key.program_id.ids) {
			mix(value);
		}
		mix(static_cast<uint32_t>(key.optimization_type));
		mix(key.validation ? 1u : 0u);
		return hash;
	}
};

struct ShaderProgramPermutation {
	std::vector<uint32_t>                                spirv;
	std::shared_ptr<const ShaderRecompiler::IR::Program> program;
};

static std::unordered_map<ShaderStageProgramKey,
                          std::vector<std::unique_ptr<ShaderProgramPermutation>>,
                          ShaderStageProgramKeyHash>
                  g_shader_program_cache;
static std::mutex g_shader_program_cache_mutex;

static constexpr uint32_t ShaderMaxPermutationsPerProgram = 64;

static std::span<const uint32_t> MakeShaderSpirvView(const std::vector<uint32_t>& spirv) {
	return {spirv.data(), spirv.size()};
}

void ShaderInit() {
	EXIT_IF(g_shader_map != nullptr);

	g_shader_map = std::make_unique<std::unordered_map<uint64_t, ShaderMappedData>>();
}

void ShaderMapUserData(uint64_t addr, const ShaderMappedData& data) {
	EXIT_IF(g_shader_map == nullptr);

	std::scoped_lock lock(g_shader_map_mutex);

	(*g_shader_map)[addr] = data;
}

static bool ShaderGetMappedData(uint64_t addr, ShaderMappedData* data) {
	EXIT_IF(g_shader_map == nullptr);
	EXIT_IF(data == nullptr);

	std::scoped_lock lock(g_shader_map_mutex);

	if (auto iter = g_shader_map->find(addr); iter != g_shader_map->end()) {
		*data = iter->second;
		return true;
	}

	return false;
}

static bool SpirvDisassemble(const uint32_t* src_binary, size_t src_binary_size,
                             std::string* dst_disassembly) {
	if (dst_disassembly != nullptr) {
		spvtools::SpirvTools core(SPV_ENV_VULKAN_1_2);

		std::string disassembly;
		if (!core.Disassemble(src_binary, src_binary_size, &disassembly,
		                      static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_NO_HEADER) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COMMENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_INDENT) |
		                          static_cast<uint32_t>(SPV_BINARY_TO_TEXT_OPTION_COLOR))) {
			*dst_disassembly = disassembly.c_str();

			LOGF("Disassemble failed\n");
			return false;
		}

		*dst_disassembly = disassembly.c_str();
	}
	return true;
}

static bool SpirvValidateBinary(const char* label, uint64_t shader_hash,
                                const std::vector<uint32_t>& spirv) {
	if (!Config::ShaderValidationEnabled()) {
		return true;
	}

	spvtools::SpirvTools core(SPV_ENV_VULKAN_1_3);
	std::string          messages;
	core.SetMessageConsumer([&messages](spv_message_level_t /*level*/, const char* /*source*/,
	                                    const spv_position_t& position, const char* message) {
		messages += fmt::format("{}: {} ({}) {}\n", static_cast<int>(position.line),
		                        static_cast<int>(position.column), static_cast<int>(position.index),
		                        message);
	});

	if (core.Validate(spirv)) {
		return true;
	}

	std::string disassembly;
	SpirvDisassemble(spirv.data(), spirv.size(), &disassembly);
	LOGF_COLOR(Log::Color::BrightRed, "%s SPIR-V validation failed hash=0x%016" PRIx64 ":\n%s",
	           label, shader_hash, messages.c_str());
	LOGF("%s\n", disassembly.c_str());
	return false;
}

static void ExitShaderRecompilerFailure(const char* label, uint64_t shader_hash,
                                        const char* reason) {
	EXIT("%s failed hash=0x%016" PRIx64 ": %s\n", label, shader_hash,
	     reason != nullptr ? reason : "");
}

static const ShaderBinaryInfo* GetBinaryInfo(const uint32_t* code) {
	EXIT_IF(code == nullptr);

	if (code[0] == 0xBEEB03FF) {
		return reinterpret_cast<const ShaderBinaryInfo*>(code +
		                                                 static_cast<size_t>(code[1] + 1) * 2);
	}

	return nullptr;
}

static std::span<const uint32_t> ShaderGetMappedCode(uint64_t shader_addr, const char* label,
                                                     uint64_t shader_hash) {
	ShaderMappedData data;
	if (!ShaderGetMappedData(shader_addr, &data)) {
		EXIT("%s hash=0x%016" PRIx64 " shader=0x%016" PRIx64 " is missing from ShaderMap\n", label,
		     shader_hash, shader_addr);
	}
	if (data.code_size_bytes == 0 || data.code_size_bytes % sizeof(uint32_t) != 0) {
		EXIT("%s hash=0x%016" PRIx64 " shader=0x%016" PRIx64
		     " has invalid AGC shader_size=0x%08" PRIx32 "\n",
		     label, shader_hash, shader_addr, data.code_size_bytes);
	}
	const auto code_words = data.code_size_bytes / sizeof(uint32_t);
	LOGF("%s hash=0x%016" PRIx64 " shader=0x%016" PRIx64 " using AGC shader_size=0x%08" PRIx32
	     " (%" PRIu32 " dwords)\n",
	     label, shader_hash, shader_addr, data.code_size_bytes, code_words);
	return {reinterpret_cast<const uint32_t*>(shader_addr), code_words};
}

#if 0
// Kept as disabled debugging guards for investigating unusual stage register state.
static void vs_check(const HW::VertexShaderInfo& vs, const HW::ShaderRegisters& sh) {
	const auto is_zero_or_wave64_subgroup = [](uint32_t value) {
		return value == 0 || value <= 0x40;
	};
	const auto is_known_gs_out_prim_type = [](uint32_t value) {
		switch (static_cast<Prospero::GsOutputPrimitiveType>(value)) {
			case Prospero::GsOutputPrimitiveType::kPoints:
			case Prospero::GsOutputPrimitiveType::kLines:
			case Prospero::GsOutputPrimitiveType::kTriangles:
			case Prospero::GsOutputPrimitiveType::k2dRectangle:
			case Prospero::GsOutputPrimitiveType::kRectList: return true;
		}

		return false;
	};
	const bool ps5_ngg_passthrough_triangle_path =
	    vs.es_regs.data_addr != 0 && vs.gs_regs.data_addr == vs.es_regs.data_addr &&
	    vs.gs_regs.chksum != 0 &&
	    sh.m_geNggSubgrpCntl == 0x00000001 && sh.m_vgtGsMaxVertOut == 0x00000003 &&
	    sh.m_vgtGsOutPrimType == 0x00000002 && sh.m_geMaxOutputPerSubgroup <= 0x000000c0;

	if (vs.es_regs.data_addr != 0 || vs.gs_regs.data_addr != 0) {
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.priority != 0);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.float_mode != 192);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.dx10_clamp != true);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.debug_mode != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.ieee_mode != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.cu_group_enable != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.require_forward_progress != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.lds_configuration != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.gs_vgpr_component_count != 3);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.fp16_overflow != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.scratch_en != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.offchip_lds != false);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.es_vgpr_component_count != 3);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.lds_size != 0);
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc2.shared_vgprs != 0);
	}

	for (uint32_t value = sh.m_spiShaderPosFormat; value != 0; value >>= 4u) {
		EXIT_NOT_IMPLEMENTED((value & 0xfu) != 0 && (value & 0xfu) != 0x4u);
	}
	if (sh.m_paClVsOutCntl != 0x00000000) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: accepting PA_CL_VS_OUT_CNTL = 0x%08" PRIx32 "\n",
			     sh.m_paClVsOutCntl);
			logged = true;
		}
	}

	EXIT_NOT_IMPLEMENTED(sh.m_spiShaderIdxFormat != 0x00000000 &&
	                     sh.m_spiShaderIdxFormat != 0x00000001);
	EXIT_NOT_IMPLEMENTED(sh.m_geNggSubgrpCntl != 0x00000000 && sh.m_geNggSubgrpCntl != 0x00000001);
	EXIT_NOT_IMPLEMENTED(sh.m_vgtGsInstanceCnt != 0x00000000);
	EXIT_NOT_IMPLEMENTED(!is_zero_or_wave64_subgroup(sh.GetEsVertsPerSubgrp()));
	EXIT_NOT_IMPLEMENTED(!is_zero_or_wave64_subgroup(sh.GetGsPrimsPerSubgrp()));
	EXIT_NOT_IMPLEMENTED(!is_zero_or_wave64_subgroup(sh.GetGsInstPrimsInSubgrp()));
	EXIT_NOT_IMPLEMENTED(!is_zero_or_wave64_subgroup(sh.m_geMaxOutputPerSubgroup) &&
	                     !ps5_ngg_passthrough_triangle_path);
	EXIT_NOT_IMPLEMENTED(sh.m_vgtEsgsRingItemsize != 0x00000000 &&
	                     sh.m_vgtEsgsRingItemsize != 0x00000004);
	EXIT_NOT_IMPLEMENTED(sh.m_vgtGsMaxVertOut != 0x00000000 && !ps5_ngg_passthrough_triangle_path);
	EXIT_NOT_IMPLEMENTED(!is_known_gs_out_prim_type(sh.m_vgtGsOutPrimType));
}

static void ps_check(const HW::PsStageRegisters& ps, const HW::ShaderRegisters& sh) {
	if (sh.target_output_mode[0] != 0 && sh.target_output_mode[0] != 2 &&
	    sh.target_output_mode[0] != 4 && sh.target_output_mode[0] != 5 &&
	    sh.target_output_mode[0] != 7 && sh.target_output_mode[0] != 9) {
		EXIT("Not implemented (sh.target_output_mode[0] != 0 && sh.target_output_mode[0] != 2 && "
		     "sh.target_output_mode[0] != 4 && sh.target_output_mode[0] != 5 && "
		     "sh.target_output_mode[0] != 7 && sh.target_output_mode[0] != 9)\n");
	}
	EXIT_NOT_IMPLEMENTED(sh.db_shader_control.conservative_z_export_value != 0x00000000);
	EXIT_NOT_IMPLEMENTED(sh.db_shader_control.shader_z_behavior != 0x00000001 &&
	                     sh.db_shader_control.shader_z_behavior != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(ps.shader_kill_enable != false);
	// EXIT_NOT_IMPLEMENTED(ps.shader_execute_on_noop != false);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc1Ps != 0x002c0000);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiShaderPgmRsrc2Ps != 0x00000000);
	// EXIT_NOT_IMPLEMENTED(ps.vgprs != 0x00 && ps.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(ps.sgprs != 0x00 && ps.sgprs != 0x01);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.priority != 0);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.float_mode != 192);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.dx10_clamp != true);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.debug_mode != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.ieee_mode != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.cu_group_disable != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.require_forward_progress != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc1.fp16_overflow != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.scratch_en != false);
	// EXIT_NOT_IMPLEMENTED(ps.user_sgpr != 0 && ps.user_sgpr != 4 && ps.user_sgpr != 12);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.wave_cnt_en != false);
	if (ps.rsrc2.extra_lds_size != 0) {
		static std::atomic_uint log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("\t PS extra LDS reservation = 0x%02" PRIx8 ", continuing\n",
			     ps.rsrc2.extra_lds_size);
		}
	}
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.raster_ordered_shading != false);
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.shared_vgprs != 0);

	if (sh.shader_z_format != 0x00000000 && sh.shader_z_format != 0x00000001 &&
	    !sh.db_shader_control.shader_z_export_enable) {
		static std::atomic_uint log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("\t shader_z_format = 0x%08" PRIx32
			     " with z export disabled, ignoring depth export format\n",
			     sh.shader_z_format);
		}
	}
	EXIT_NOT_IMPLEMENTED(sh.db_shader_control.shader_z_export_enable &&
	                     sh.shader_z_format != 0x00000000 && sh.shader_z_format != 0x00000001);
	constexpr uint32_t ps_input_linear_center = 0x00000020u;
	constexpr uint32_t ps_input_pos_w         = 0x00000800u;
	constexpr uint32_t ps_input_front_face    = 0x00001000u;
	constexpr uint32_t supported_ps_input_bits =
	    0x00000702u | ps_input_linear_center | ps_input_pos_w | ps_input_front_face;
	EXIT_NOT_IMPLEMENTED((sh.ps_input_ena & ~supported_ps_input_bits) != 0);
	EXIT_NOT_IMPLEMENTED((sh.ps_input_addr & ~supported_ps_input_bits) != 0);
	EXIT_NOT_IMPLEMENTED(sh.ps_input_ena != sh.ps_input_addr);
	// EXIT_NOT_IMPLEMENTED(ps.m_spiPsInControl != 0x00000000);
	constexpr uint32_t baryc_persp_mask =
	    0x00000003u | 0x00000030u | 0x00000300u | 0x00003000u;
	constexpr uint32_t baryc_linear_mask = 0x00030000u | 0x00300000u | 0x03000000u;
	constexpr uint32_t baryc_known_mask  = baryc_persp_mask | baryc_linear_mask;
	EXIT_NOT_IMPLEMENTED((sh.baryc_cntl & ~baryc_known_mask) != 0);
	EXIT_NOT_IMPLEMENTED((sh.baryc_cntl & baryc_persp_mask) != 0);
	if ((sh.ps_input_ena & ps_input_linear_center) == 0 && (sh.baryc_cntl & baryc_linear_mask) != 0) {
		static std::atomic_uint log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("\t ignoring inactive linear SPI_BARYC_CNTL bits: 0x%08" PRIx32 "\n",
			     sh.baryc_cntl & baryc_linear_mask);
		}
	} else {
		EXIT_NOT_IMPLEMENTED((sh.baryc_cntl & baryc_linear_mask) != 0x00000000 &&
		                     (sh.baryc_cntl & baryc_linear_mask) != 0x01000000);
	}
	if ((sh.m_cbShaderMask & 0x0000000f) != 0x0000000f) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: accepting partial CB_SHADER_MASK = 0x%08" PRIx32 "\n",
			     sh.m_cbShaderMask);
			logged = true;
		}
	}
	if ((sh.m_cbShaderMask & ~0x0000000fu) != 0) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: ignoring extra CB_SHADER_MASK MRT bits: 0x%08" PRIx32 "\n",
			     sh.m_cbShaderMask);
			logged = true;
		}
	}

	if (sh.db_shader_control.other_bits != 0x00000000) {
		static std::atomic_uint log_count {0};
		if (log_count.fetch_add(1, std::memory_order_relaxed) < 32) {
			LOGF("\t temporary: ignoring unsupported DB_SHADER_CONTROL bits 0x%08" PRIx32 "\n",
			     sh.db_shader_control.other_bits);
		}
	}
	EXIT_NOT_IMPLEMENTED(sh.m_paScShaderControl != 0x00000000);
}

static void cs_check(const HW::CsStageRegisters& cs, const HW::ShaderRegisters& /*sh*/) {
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_x != 0x00000040);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_y != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.num_thread_z != 0x00000001);
	// EXIT_NOT_IMPLEMENTED(cs.vgprs != 0x00 && cs.vgprs != 0x01);
	// EXIT_NOT_IMPLEMENTED(cs.sgprs != 0x01 && cs.sgprs != 0x02);
	EXIT_NOT_IMPLEMENTED(cs.bulky != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.scratch_en != 0x00);
	// EXIT_NOT_IMPLEMENTED(cs.user_sgpr != 0x0c);
	if (cs.tgid_x_en == 0x00) {
		static bool logged = false;
		if (!logged) {
			LOGF("\t temporary: compute shader has TGID X disabled\n");
			logged = true;
		}
	} else {
		EXIT_NOT_IMPLEMENTED(cs.tgid_x_en != 0x01);
	}
	// EXIT_NOT_IMPLEMENTED(cs.tgid_y_en != 0x00);
	// EXIT_NOT_IMPLEMENTED(cs.tgid_z_en != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.tg_size_en != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.tidig_comp_cnt > 2);

	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc1 != 0x002c0040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computePgmRsrc2 != 0x00000098);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadX != 0x00000040);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadY != 0x00000001);
	//	EXIT_NOT_IMPLEMENTED(cs.m_computeNumThreadZ != 0x00000001);
}
#endif

static void GetNextGenFallbackShaderId(uint64_t addr, uint32_t* hash0, uint32_t* crc32) {
	EXIT_IF(hash0 == nullptr);
	EXIT_IF(crc32 == nullptr);

	auto x = addr;
	x ^= x >> 33u;
	x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33u;
	x *= 0xc4ceb9fe1a85ec53ull;
	x ^= x >> 33u;

	*hash0 = static_cast<uint32_t>((x >> 32u) & 0xffffffffu);
	*crc32 = static_cast<uint32_t>(x & 0xffffffffu);
}

static void ShaderDetectBuffers(ShaderVertexInputInfo* info) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr);

	info->buffers_num = 0;

	for (int ri = 0; ri < info->resources_num; ri++) {
		const auto& r = info->resources[ri];

		bool merged = false;
		for (int bi = 0; bi < info->buffers_num; bi++) {
			auto& b = info->buffers[bi];

			uint64_t stride = b.stride;

			if (stride == r.Stride() &&
			    b.fetch_index == static_cast<uint32_t>(info->resources_dst[ri].fetch_index)) {
				uint64_t rbase   = r.Base48();
				uint64_t base    = std::min(rbase, b.addr);
				uint64_t offset1 = rbase - base;
				uint64_t offset2 = b.addr - base;

				if (offset1 < stride && offset2 < stride) {
					EXIT_NOT_IMPLEMENTED(b.num_records != r.NumRecords());
					b.addr = base;
					EXIT_NOT_IMPLEMENTED(b.attr_num >= ShaderVertexInputBuffer::ATTR_MAX);
					b.attr_indices[b.attr_num++] = ri;
					merged                       = true;
					break;
				}
			}
		}

		if (!merged) {
			EXIT_NOT_IMPLEMENTED(info->buffers_num >= ShaderVertexInputInfo::RES_MAX);
			int bi                            = info->buffers_num++;
			info->buffers[bi].addr            = r.Base48();
			info->buffers[bi].stride          = r.Stride();
			info->buffers[bi].num_records     = r.NumRecords();
			info->buffers[bi].fetch_index     = info->resources_dst[ri].fetch_index;
			info->buffers[bi].attr_num        = 1;
			info->buffers[bi].attr_indices[0] = ri;
		}
	}

	for (int bi = 0; bi < info->buffers_num; bi++) {
		auto& b = info->buffers[bi];
		for (int ri = 0; ri < b.attr_num; ri++) {
			b.attr_offsets[ri] = info->resources[b.attr_indices[ri]].Base48() - b.addr;
		}
	}
}

static uint32_t VertexAttribFormatToBufferFormat(uint32_t format) {
	struct FormatMap {
		Prospero::VertexAttribFormat vertex;
		Prospero::BufferFormat       buffer;
	};

	static constexpr FormatMap format_map[] = {
	    {Prospero::VertexAttribFormat::kInvalid, Prospero::BufferFormat::kInvalid},
	    {Prospero::VertexAttribFormat::k8UNorm, Prospero::BufferFormat::k8UNorm},
	    {Prospero::VertexAttribFormat::k8SNorm, Prospero::BufferFormat::k8SNorm},
	    {Prospero::VertexAttribFormat::k8UScaled, Prospero::BufferFormat::k8UScaled},
	    {Prospero::VertexAttribFormat::k8SScaled, Prospero::BufferFormat::k8SScaled},
	    {Prospero::VertexAttribFormat::k8UInt, Prospero::BufferFormat::k8UInt},
	    {Prospero::VertexAttribFormat::k8SInt, Prospero::BufferFormat::k8SInt},
	    {Prospero::VertexAttribFormat::k16UNorm, Prospero::BufferFormat::k16UNorm},
	    {Prospero::VertexAttribFormat::k16SNorm, Prospero::BufferFormat::k16SNorm},
	    {Prospero::VertexAttribFormat::k16UScaled, Prospero::BufferFormat::k16UScaled},
	    {Prospero::VertexAttribFormat::k16SScaled, Prospero::BufferFormat::k16SScaled},
	    {Prospero::VertexAttribFormat::k16UInt, Prospero::BufferFormat::k16UInt},
	    {Prospero::VertexAttribFormat::k16SInt, Prospero::BufferFormat::k16SInt},
	    {Prospero::VertexAttribFormat::k16Float, Prospero::BufferFormat::k16Float},
	    {Prospero::VertexAttribFormat::k8_8UNorm, Prospero::BufferFormat::k8_8UNorm},
	    {Prospero::VertexAttribFormat::k8_8SNorm, Prospero::BufferFormat::k8_8SNorm},
	    {Prospero::VertexAttribFormat::k8_8UScaled, Prospero::BufferFormat::k8_8UScaled},
	    {Prospero::VertexAttribFormat::k8_8SScaled, Prospero::BufferFormat::k8_8SScaled},
	    {Prospero::VertexAttribFormat::k8_8UInt, Prospero::BufferFormat::k8_8UInt},
	    {Prospero::VertexAttribFormat::k8_8SInt, Prospero::BufferFormat::k8_8SInt},
	    {Prospero::VertexAttribFormat::k32UInt, Prospero::BufferFormat::k32UInt},
	    {Prospero::VertexAttribFormat::k32SInt, Prospero::BufferFormat::k32SInt},
	    {Prospero::VertexAttribFormat::k32Float, Prospero::BufferFormat::k32Float},
	    {Prospero::VertexAttribFormat::k16_16UNorm, Prospero::BufferFormat::k16_16UNorm},
	    {Prospero::VertexAttribFormat::k16_16SNorm, Prospero::BufferFormat::k16_16SNorm},
	    {Prospero::VertexAttribFormat::k16_16UScaled, Prospero::BufferFormat::k16_16UScaled},
	    {Prospero::VertexAttribFormat::k16_16SScaled, Prospero::BufferFormat::k16_16SScaled},
	    {Prospero::VertexAttribFormat::k16_16UInt, Prospero::BufferFormat::k16_16UInt},
	    {Prospero::VertexAttribFormat::k16_16SInt, Prospero::BufferFormat::k16_16SInt},
	    {Prospero::VertexAttribFormat::k16_16Float, Prospero::BufferFormat::k16_16Float},
	    {Prospero::VertexAttribFormat::k11_11_10UNorm, Prospero::BufferFormat::k11_11_10UNorm},
	    {Prospero::VertexAttribFormat::k11_11_10SNorm, Prospero::BufferFormat::k11_11_10SNorm},
	    {Prospero::VertexAttribFormat::k11_11_10UScaled, Prospero::BufferFormat::k11_11_10UScaled},
	    {Prospero::VertexAttribFormat::k11_11_10SScaled, Prospero::BufferFormat::k11_11_10SScaled},
	    {Prospero::VertexAttribFormat::k11_11_10UInt, Prospero::BufferFormat::k11_11_10UInt},
	    {Prospero::VertexAttribFormat::k11_11_10SInt, Prospero::BufferFormat::k11_11_10SInt},
	    {Prospero::VertexAttribFormat::k11_11_10Float, Prospero::BufferFormat::k11_11_10Float},
	    {Prospero::VertexAttribFormat::k10_11_11UNorm, Prospero::BufferFormat::k10_11_11UNorm},
	    {Prospero::VertexAttribFormat::k10_11_11SNorm, Prospero::BufferFormat::k10_11_11SNorm},
	    {Prospero::VertexAttribFormat::k10_11_11UScaled, Prospero::BufferFormat::k10_11_11UScaled},
	    {Prospero::VertexAttribFormat::k10_11_11SScaled, Prospero::BufferFormat::k10_11_11SScaled},
	    {Prospero::VertexAttribFormat::k10_11_11UInt, Prospero::BufferFormat::k10_11_11UInt},
	    {Prospero::VertexAttribFormat::k10_11_11SInt, Prospero::BufferFormat::k10_11_11SInt},
	    {Prospero::VertexAttribFormat::k10_11_11Float, Prospero::BufferFormat::k10_11_11Float},
	    {Prospero::VertexAttribFormat::k2_10_10_10UNorm, Prospero::BufferFormat::k2_10_10_10UNorm},
	    {Prospero::VertexAttribFormat::k2_10_10_10SNorm, Prospero::BufferFormat::k2_10_10_10SNorm},
	    {Prospero::VertexAttribFormat::k2_10_10_10UScaled,
	     Prospero::BufferFormat::k2_10_10_10UScaled},
	    {Prospero::VertexAttribFormat::k2_10_10_10SScaled,
	     Prospero::BufferFormat::k2_10_10_10SScaled},
	    {Prospero::VertexAttribFormat::k2_10_10_10UInt, Prospero::BufferFormat::k2_10_10_10UInt},
	    {Prospero::VertexAttribFormat::k2_10_10_10SInt, Prospero::BufferFormat::k2_10_10_10SInt},
	    {Prospero::VertexAttribFormat::k10_10_10_2UNorm, Prospero::BufferFormat::k10_10_10_2UNorm},
	    {Prospero::VertexAttribFormat::k10_10_10_2SNorm, Prospero::BufferFormat::k10_10_10_2SNorm},
	    {Prospero::VertexAttribFormat::k10_10_10_2UScaled,
	     Prospero::BufferFormat::k10_10_10_2UScaled},
	    {Prospero::VertexAttribFormat::k10_10_10_2SScaled,
	     Prospero::BufferFormat::k10_10_10_2SScaled},
	    {Prospero::VertexAttribFormat::k10_10_10_2UInt, Prospero::BufferFormat::k10_10_10_2UInt},
	    {Prospero::VertexAttribFormat::k10_10_10_2SInt, Prospero::BufferFormat::k10_10_10_2SInt},
	    {Prospero::VertexAttribFormat::k8_8_8_8UNorm, Prospero::BufferFormat::k8_8_8_8UNorm},
	    {Prospero::VertexAttribFormat::k8_8_8_8SNorm, Prospero::BufferFormat::k8_8_8_8SNorm},
	    {Prospero::VertexAttribFormat::k8_8_8_8UScaled, Prospero::BufferFormat::k8_8_8_8UScaled},
	    {Prospero::VertexAttribFormat::k8_8_8_8SScaled, Prospero::BufferFormat::k8_8_8_8SScaled},
	    {Prospero::VertexAttribFormat::k8_8_8_8UInt, Prospero::BufferFormat::k8_8_8_8UInt},
	    {Prospero::VertexAttribFormat::k8_8_8_8SInt, Prospero::BufferFormat::k8_8_8_8SInt},
	    {Prospero::VertexAttribFormat::k32_32UInt, Prospero::BufferFormat::k32_32UInt},
	    {Prospero::VertexAttribFormat::k32_32SInt, Prospero::BufferFormat::k32_32SInt},
	    {Prospero::VertexAttribFormat::k32_32Float, Prospero::BufferFormat::k32_32Float},
	    {Prospero::VertexAttribFormat::k16_16_16_16UNorm,
	     Prospero::BufferFormat::k16_16_16_16UNorm},
	    {Prospero::VertexAttribFormat::k16_16_16_16SNorm,
	     Prospero::BufferFormat::k16_16_16_16SNorm},
	    {Prospero::VertexAttribFormat::k16_16_16_16UScaled,
	     Prospero::BufferFormat::k16_16_16_16UScaled},
	    {Prospero::VertexAttribFormat::k16_16_16_16SScaled,
	     Prospero::BufferFormat::k16_16_16_16SScaled},
	    {Prospero::VertexAttribFormat::k16_16_16_16UInt, Prospero::BufferFormat::k16_16_16_16UInt},
	    {Prospero::VertexAttribFormat::k16_16_16_16SInt, Prospero::BufferFormat::k16_16_16_16SInt},
	    {Prospero::VertexAttribFormat::k16_16_16_16Float,
	     Prospero::BufferFormat::k16_16_16_16Float},
	    {Prospero::VertexAttribFormat::k32_32_32UInt, Prospero::BufferFormat::k32_32_32UInt},
	    {Prospero::VertexAttribFormat::k32_32_32SInt, Prospero::BufferFormat::k32_32_32SInt},
	    {Prospero::VertexAttribFormat::k32_32_32Float, Prospero::BufferFormat::k32_32_32Float},
	    {Prospero::VertexAttribFormat::k32_32_32_32UInt, Prospero::BufferFormat::k32_32_32_32UInt},
	    {Prospero::VertexAttribFormat::k32_32_32_32SInt, Prospero::BufferFormat::k32_32_32_32SInt},
	    {Prospero::VertexAttribFormat::k32_32_32_32Float,
	     Prospero::BufferFormat::k32_32_32_32Float},
	};

	for (const auto& entry: format_map) {
		if (format == Prospero::GpuEnumValue(entry.vertex)) {
			return Prospero::GpuEnumValue(entry.buffer);
		}
	}

	return format;
}

static void ShaderApplyAttribSemantics(ShaderVertexInputInfo* info,
                                       const ShaderSemantic*  input_semantics,
                                       uint32_t num_input_semantics, const uint32_t* attrib,
                                       const uint32_t* buffer) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || attrib == nullptr || buffer == nullptr);

	for (uint32_t i = 0; i < num_input_semantics; i++) {
		const auto& in = input_semantics[i];

		EXIT_NOT_IMPLEMENTED(in.static_vb_index == 1 || in.static_attribute == 1);

		uint32_t reg  = in.hardware_mapping;
		uint32_t size = in.size_in_elements;

		LOGF("reg = %u, size = %u, va[%u] = 0x%08" PRIx32 "\n", reg, size, i, attrib[in.semantic]);

		size_t   index       = attrib[in.semantic] & 0x1fu;
		uint32_t format      = (attrib[in.semantic] >> 5u) & 0x1ffu;
		uint32_t offset      = (attrib[in.semantic] >> 14u) & 0xfffu;
		uint32_t fetch_index = (attrib[in.semantic] >> 26u) & 0x1u;

		if (fetch_index != 0) {
			static std::atomic<uint64_t> log_count = 0;
			auto                         log_id    = log_count.fetch_add(1);
			if (log_id < 64) {
				LOGF("\t temporary: PS5 vertex attrib semantic %u uses fetch index %u, buffer "
				     "index %zu\n",
				     static_cast<uint32_t>(in.semantic), fetch_index, index);
			}
		}

		EXIT_NOT_IMPLEMENTED(index >= ShaderVertexInputInfo::RES_MAX);

		const auto* sharp = &buffer[index * 4];

		EXIT_NOT_IMPLEMENTED(info->resources_num >= ShaderVertexInputInfo::RES_MAX);

		auto& r           = info->resources[info->resources_num];
		auto& rd          = info->resources_dst[info->resources_num];
		rd.register_start = static_cast<int>(reg);
		rd.registers_num  = static_cast<int>(size);
		rd.attr_id        = static_cast<int>(in.semantic);
		rd.fetch_index    = fetch_index;
		r.fields[0]       = sharp[0];
		r.fields[1]       = sharp[1];
		r.fields[2]       = sharp[2];
		r.fields[3]       = sharp[3];
		if (format != 0) {
			auto                         buffer_format = VertexAttribFormatToBufferFormat(format);
			static std::atomic<uint64_t> log_count     = 0;
			auto                         log_id        = log_count.fetch_add(1);
			if (log_id < 64) {
				LOGF("\t temporary: PS5 vertex attrib semantic %u uses attrib format %u -> buffer "
				     "format %u, offset %u, buffer index %zu\n",
				     static_cast<uint32_t>(in.semantic), format, buffer_format, offset, index);
			}
			r.fields[3] = (r.fields[3] & ~((0x7fu << 12u) | 0xfffu)) |
			              ((buffer_format & 0x7fu) << 12u) | DstSel(4, 5, 6, 7);
		}
		if (offset != 0) {
			r.UpdateAddress48(r.Base48() + offset);
		}

		info->resources_num++;
	}
}

static uint32_t ShaderCalcPsSystemInputBase(const HW::ShaderRegisters& regs) {
	constexpr uint32_t ps_input_persp_sample    = 0x00000001u;
	constexpr uint32_t ps_input_persp_center    = 0x00000002u;
	constexpr uint32_t ps_input_persp_centroid  = 0x00000004u;
	constexpr uint32_t ps_input_persp_pull      = 0x00000008u;
	constexpr uint32_t ps_input_linear_sample   = 0x00000010u;
	constexpr uint32_t ps_input_linear_center   = 0x00000020u;
	constexpr uint32_t ps_input_linear_centroid = 0x00000040u;
	constexpr uint32_t ps_input_line_stipple    = 0x00000080u;
	constexpr uint32_t ps_input_pos_xy          = 0x00000300u;
	constexpr uint32_t ps_input_pos_z           = 0x00000400u;
	constexpr uint32_t ps_input_pos_w           = 0x00000800u;
	constexpr uint32_t ps_input_front_face      = 0x00001000u;
	constexpr uint32_t ps_input_ancillary       = 0x00002000u;
	constexpr uint32_t ps_input_sample_coverage = 0x00004000u;
	constexpr uint32_t ps_input_pos_fixed_pt    = 0x00008000u;
	constexpr uint32_t supported_ps_input_bits =
	    ps_input_persp_sample | ps_input_persp_center | ps_input_persp_centroid |
	    ps_input_persp_pull | ps_input_linear_sample | ps_input_linear_center |
	    ps_input_linear_centroid | ps_input_line_stipple | ps_input_pos_xy | ps_input_pos_z |
	    ps_input_pos_w | ps_input_front_face | ps_input_ancillary | ps_input_sample_coverage |
	    ps_input_pos_fixed_pt;

	EXIT_NOT_IMPLEMENTED((regs.ps_input_ena & ~supported_ps_input_bits) != 0);
	EXIT_NOT_IMPLEMENTED((regs.ps_input_addr & ~supported_ps_input_bits) != 0);
	EXIT_NOT_IMPLEMENTED(regs.ps_input_ena != regs.ps_input_addr);

	const uint32_t inputs = regs.ps_input_addr;
	uint32_t       reg    = 0;
	if ((inputs & ps_input_persp_sample) != 0) {
		reg += 2;
	}
	if ((inputs & ps_input_persp_center) != 0) {
		reg += 2;
	}
	if ((inputs & ps_input_persp_centroid) != 0) {
		reg += 2;
	}
	if ((inputs & ps_input_persp_pull) != 0) {
		reg += 3;
	}
	if ((inputs & ps_input_linear_sample) != 0) {
		reg += 2;
	}
	if ((inputs & ps_input_linear_center) != 0) {
		reg += 2;
	}
	if ((inputs & ps_input_linear_centroid) != 0) {
		reg += 2;
	}
	if ((inputs & ps_input_line_stipple) != 0) {
		reg += 1;
	}
	return reg;
}

static bool ShaderGetStaticInputInfoVS(const HW::VertexShaderInfo* regs,
                                       const HW::ShaderRegisters* sh, ShaderVertexInputInfo* info) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(info == nullptr || regs == nullptr || sh == nullptr);

	*info = {};

	info->export_count = static_cast<int>(sh->GetExportCount());

	EXIT_NOT_IMPLEMENTED(regs->es_regs.data_addr == 0 || regs->gs_regs.chksum == 0);

	uint64_t                shader_addr   = regs->es_regs.data_addr;
	const HW::UserSgprInfo& user_sgpr     = regs->gs_user_sgpr;
	auto                    user_sgpr_num = regs->gs_regs.rsrc2.user_sgpr;
	ShaderMappedData        data;
	if (!ShaderGetMappedData(shader_addr, &data)) {
		LOGF("ShaderGetInputInfoVS(): shader=0x%016" PRIx64 " is missing from ShaderMap\n",
		     shader_addr);
		return false;
	}

	if (data.user_data == nullptr) {
		LOGF("ShaderGetInputInfoVS(): no AGC user data for shader=0x%016" PRIx64 " es=0x%016" PRIx64
		     " gs=0x%016" PRIx64 " chksum=0x%016" PRIx64 " user_sgpr_num=%u\n",
		     shader_addr, regs->es_regs.data_addr, regs->gs_regs.data_addr, regs->gs_regs.chksum,
		     static_cast<uint32_t>(user_sgpr_num));
	}
	ShaderVertexMetadata metadata;
	std::string          metadata_error;
	if (!ShaderReadVertexMetadata(data, HW::UserSgprInfo::SGPRS_MAX, &metadata, &metadata_error)) {
		LOGF("ShaderGetInputInfoVS(): invalid AGC metadata shader=0x%016" PRIx64 ": %s\n",
		     shader_addr, metadata_error.c_str());
		return false;
	}

	if (metadata.vertex_buffer_reg >= 0) {
		info->fetch_external   = false;
		info->fetch_embedded   = true;
		info->fetch_attrib_reg = metadata.vertex_attrib_reg;
		info->fetch_buffer_reg = metadata.vertex_buffer_reg;

		const auto* attrib = reinterpret_cast<const uint32_t*>(
		    static_cast<uint64_t>(user_sgpr.value[metadata.vertex_attrib_reg]) |
		    (static_cast<uint64_t>(user_sgpr.value[metadata.vertex_attrib_reg + 1]) << 32u));
		const auto* buffer = reinterpret_cast<const uint32_t*>(
		    static_cast<uint64_t>(user_sgpr.value[metadata.vertex_buffer_reg]) |
		    (static_cast<uint64_t>(user_sgpr.value[metadata.vertex_buffer_reg + 1]) << 32u));

		if (attrib == nullptr || buffer == nullptr) {
			LOGF("ShaderGetInputInfoVS(): null vertex table pointer shader=0x%016" PRIx64 "\n",
			     shader_addr);
			return false;
		}
		ShaderApplyAttribSemantics(info, metadata.input_semantics.data(),
		                           metadata.input_semantics_count, attrib, buffer);
		ShaderDetectBuffers(info);
	}
	return true;
}

static void ShaderGetStaticInputInfoPS(
    const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh,
    const ShaderVertexInputInfo*                        vs_info,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    ShaderPixelInputInfo*                               ps_info) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(vs_info == nullptr);
	EXIT_IF(ps_info == nullptr);
	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);

	*ps_info = {};

	ps_info->input_num              = sh->ps_in_control;
	ps_info->ps_system_input_base   = ShaderCalcPsSystemInputBase(*sh);
	const uint32_t active_inputs    = sh->ps_input_ena & sh->ps_input_addr;
	ps_info->ps_pos_x               = (active_inputs & 0x00000100u) != 0;
	ps_info->ps_pos_y               = (active_inputs & 0x00000200u) != 0;
	ps_info->ps_pos_xy              = ps_info->ps_pos_x && ps_info->ps_pos_y;
	ps_info->ps_pos_z               = (active_inputs & 0x00000400u) != 0;
	ps_info->ps_pos_w               = (active_inputs & 0x00000800u) != 0;
	ps_info->ps_front_face          = (active_inputs & 0x00001000u) != 0;
	ps_info->ps_no_perspective      = (sh->ps_input_ena & sh->ps_input_addr & 0x00000020u) != 0;
	ps_info->ps_pixel_kill_enable   = sh->db_shader_control.shader_kill_enable;
	ps_info->ps_depth_export_enable = sh->db_shader_control.shader_z_export_enable;
	ps_info->ps_sample_mask_export_enable = sh->db_shader_control.shader_mask_export_enable;
	ps_info->ps_early_z                   = (sh->db_shader_control.shader_z_behavior == 1 &&
	                                         !sh->db_shader_control.shader_kill_enable &&
	                                         !sh->db_shader_control.shader_z_export_enable &&
	                                         !sh->db_shader_control.shader_mask_export_enable);
	ps_info->ps_execute_on_noop           = sh->db_shader_control.shader_execute_on_noop;

	for (uint32_t i = 0; i < ps_info->input_num; i++) {
		ps_info->interpolator_settings[i] = sh->ps_interpolator_settings[i];
	}

	ps_info->descriptor_set =
	    vs_info->stage.program != nullptr && !vs_info->stage.program->bindings.descriptors.empty()
	        ? 1
	        : 0;

	for (int i = 0; i < 8; i++) {
		ps_info->target_output_mode[i]    = sh->target_output_mode[i];
		ps_info->target_export_mapping[i] = sh->target_output_mode[i] != 0
		                                        ? target_export_mapping[i]
		                                        : Prospero::ColorComponentMapping {};
	}
	ps_info->mrt_output_mask = 0;
}

static void ShaderGetStaticInputInfoCS(const HW::ComputeShaderInfo* regs,
                                       const HW::ShaderRegisters* /*sh*/,
                                       ShaderComputeInputInfo* info) {
	EXIT_IF(info == nullptr);
	EXIT_IF(regs == nullptr);

	*info = {};

	info->threads_num[0] = regs->cs_regs.num_thread_x;
	info->threads_num[1] = regs->cs_regs.num_thread_y;
	info->threads_num[2] = regs->cs_regs.num_thread_z;
	info->group_id[0]    = regs->cs_regs.tgid_x_en != 0;
	info->group_id[1]    = regs->cs_regs.tgid_y_en != 0;
	info->group_id[2]    = regs->cs_regs.tgid_z_en != 0;
	info->wave_size      = regs->cs_regs.wave_size;
	info->thread_ids_num = regs->cs_regs.tidig_comp_cnt + 1;
	info->tg_size_en     = regs->cs_regs.tg_size_en != 0;

	info->workgroup_register = regs->cs_regs.user_sgpr;
}

static ShaderStageProgramKey MakeShaderStageProgramKey(ShaderType stage, uint64_t shader_hash,
                                                       const ShaderId&    program_id,
                                                       ShaderLaneMaskMode lane_mask_mode) {
	ShaderStageProgramKey key {};
	key.stage             = stage;
	key.lane_mask_mode    = lane_mask_mode;
	key.shader_hash       = shader_hash;
	key.program_id        = program_id;
	key.optimization_type = Config::GetShaderOptimizationType();
	key.validation        = Config::ShaderValidationEnabled();
	return key;
}

static void ApplyVertexOutputs(ShaderVertexInputInfo*               info,
                               const ShaderRecompiler::IR::Program& program) {
	info->export_count      = 0;
	info->param_export_mask = 0;
	for (const auto& output: program.info.outputs) {
		if (output.kind == ShaderRecompiler::IR::StageOutputKind::Parameter && output.index < 32) {
			info->param_export_mask |= 1u << output.index;
			info->export_count = std::max(info->export_count, static_cast<int>(output.index + 1));
		}
	}
}

static void ApplyPixelOutputs(ShaderPixelInputInfo*                info,
                              const ShaderRecompiler::IR::Program& program) {
	info->mrt_output_mask = 0;
	for (const auto& output: program.info.outputs) {
		if (output.kind == ShaderRecompiler::IR::StageOutputKind::Mrt && output.index < 8) {
			info->mrt_output_mask |= 1u << output.index;
		}
	}
}

static std::string ShaderDescribeSpecialization(const ShaderRecompiler::IR::Program& program);

static bool LogPermutationMismatch(const ShaderProgramPermutation& permutation, const char* stage,
                                   uint64_t shader_hash, const std::string& error) {
	static std::atomic<uint32_t> log_count {0};
	if (Config::GraphicsDebugDumpEnabled() &&
	    log_count.fetch_add(1, std::memory_order_relaxed) < 64) {
		LOGF("ShaderProgramCache native runtime mismatch %s shader=0x%016" PRIx64
		     ": %s\n  specialization: %s\n",
		     stage, shader_hash, error.c_str(),
		     ShaderDescribeSpecialization(*permutation.program).c_str());
	}
	return false;
}

static bool TryUseVertexPermutation(const ShaderProgramPermutation& permutation,
                                    const HW::VertexShaderInfo* regs, ShaderVertexInputInfo* info,
                                    uint64_t shader_hash) {
	EXIT_IF(regs == nullptr || info == nullptr);
	std::string error;
	if (!ShaderMaterializeStageRuntime(
	        permutation.program,
	        std::span<const uint32_t>(regs->gs_user_sgpr.value, regs->gs_regs.rsrc2.user_sgpr),
	        regs->es_regs.data_addr, &info->stage, &error)) {
		return LogPermutationMismatch(permutation, "VS", shader_hash, error);
	}
	ApplyVertexOutputs(info, *permutation.program);
	return true;
}

static bool TryUsePixelPermutation(const ShaderProgramPermutation& permutation,
                                   const HW::PixelShaderInfo* regs, ShaderPixelInputInfo* info,
                                   uint64_t shader_hash) {
	EXIT_IF(regs == nullptr || info == nullptr);
	std::string error;
	if (!ShaderMaterializeStageRuntime(
	        permutation.program,
	        std::span<const uint32_t>(regs->ps_user_sgpr.value, regs->ps_regs.rsrc2.user_sgpr),
	        regs->ps_regs.data_addr, &info->stage, &error)) {
		return LogPermutationMismatch(permutation, "PS", shader_hash, error);
	}
	ApplyPixelOutputs(info, *permutation.program);
	return true;
}

static bool TryUseComputePermutation(const ShaderProgramPermutation& permutation,
                                     const HW::ComputeShaderInfo*    regs,
                                     ShaderComputeInputInfo* info, uint64_t shader_hash) {
	EXIT_IF(regs == nullptr || info == nullptr);
	std::string error;
	if (!ShaderMaterializeStageRuntime(
	        permutation.program,
	        std::span<const uint32_t>(regs->cs_user_sgpr.value, regs->cs_regs.user_sgpr),
	        regs->cs_regs.data_addr, &info->stage, &error)) {
		return LogPermutationMismatch(permutation, "CS", shader_hash, error);
	}
	return true;
}

static void LogShaderProgramCacheHit(const char* stage, uint64_t shader_hash, uint64_t words) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}

	static std::atomic<uint32_t> log_count {0};
	if (log_count.fetch_add(1, std::memory_order_relaxed) >= 512) {
		return;
	}

	LOGF("ShaderProgramCache: reused %s shader=0x%016" PRIx64 " words=%" PRIu64 "\n", stage,
	     shader_hash, words);
}

static std::string ShaderDescribeSpecialization(const ShaderRecompiler::IR::Program& program) {
	std::string ret = fmt::format(
	    "set={} push={} groups={} user={} buffers={} images={} samplers={} addresses={} srt={}",
	    program.bindings.descriptor_set, program.bindings.push_constant_size,
	    program.bindings.descriptors.size(), program.bindings.user_data_registers.size(),
	    program.info.buffers.size(), program.info.images.size(), program.info.samplers.size(),
	    program.info.addresses.size(), program.srt.reads.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		const auto& buffer = program.info.buffers[i];
		ret += fmt::format(" b{}[stride={} format={}]", i, buffer.packed_stride,
		                   buffer.descriptor_format);
	}
	for (uint32_t i = 0; i < program.info.images.size(); i++) {
		const auto& image = program.info.images[i];
		ret += fmt::format(" i{}[kind={} dim={} swizzle=0x{:03x}]", i,
		                   static_cast<uint32_t>(image.kind),
		                   static_cast<uint32_t>(image.dimension), image.storage_swizzle);
	}
	for (uint32_t i = 0; i < program.info.addresses.size(); i++) {
		ret += fmt::format(" a{}[base=0x{:x}]", i, program.info.addresses[i].specialized_base);
	}
	return ret;
}

static void ShaderAppendNativeSpecialization(std::vector<uint32_t>*               ids,
                                             const ShaderRecompiler::IR::Program& program) {
	EXIT_IF(ids == nullptr || !program.binding_layout_complete);
	ids->push_back(static_cast<uint32_t>(program.lane_mask_mode));
	ids->push_back(program.bindings.descriptor_set);
	ids->push_back(program.bindings.push_constant_offset);
	ids->push_back(program.bindings.push_constant_size);
	ids->push_back(static_cast<uint32_t>(program.bindings.user_data_registers.size()));
	ids->insert(ids->end(), program.bindings.user_data_registers.begin(),
	            program.bindings.user_data_registers.end());
	ids->push_back(static_cast<uint32_t>(program.bindings.descriptors.size()));
	for (const auto& binding: program.bindings.descriptors) {
		ids->push_back(static_cast<uint32_t>(binding.kind));
		ids->push_back(binding.binding);
		ids->push_back(static_cast<uint32_t>(binding.resources.size()));
		ids->insert(ids->end(), binding.resources.begin(), binding.resources.end());
	}
	ids->push_back(static_cast<uint32_t>(program.info.buffers.size()));
	for (const auto& buffer: program.info.buffers) {
		ids->push_back(buffer.packed_stride);
		ids->push_back(buffer.descriptor_format);
	}
	ids->push_back(static_cast<uint32_t>(program.info.images.size()));
	for (const auto& image: program.info.images) {
		ids->push_back(static_cast<uint32_t>(image.kind));
		ids->push_back(static_cast<uint32_t>(image.dimension));
		ids->push_back(static_cast<uint32_t>(image.mip_mode));
		ids->push_back(image.storage_swizzle);
	}
	ids->push_back(static_cast<uint32_t>(program.info.addresses.size()));
	for (const auto& address: program.info.addresses) {
		ids->push_back(static_cast<uint32_t>(address.specialized_base));
		ids->push_back(static_cast<uint32_t>(address.specialized_base >> 32u));
	}
}

static std::span<const uint32_t> AddShaderProgramPermutation(const char* stage,
                                                             uint64_t    shader_hash,
                                                             const ShaderStageProgramKey& key,
                                                             ShaderProgramPermutation permutation) {
	static std::atomic<int> compiled {0};
	const auto              compiled_count = compiled.fetch_add(1, std::memory_order_relaxed) + 1;
	std::printf("Num compiled %d shaders\n", compiled_count);

	std::scoped_lock lock(g_shader_program_cache_mutex);
	auto&            permutations = g_shader_program_cache[key];

	if (permutations.size() >= ShaderMaxPermutationsPerProgram) {
		LOGF("ShaderProgramCache overflow: %s\n",
		     ShaderDescribeSpecialization(*permutation.program).c_str());
		for (uint64_t i = 0; i < static_cast<uint64_t>(permutations.size()); i++) {
			LOGF("ShaderProgramCache overflow existing[%" PRIu64 "]: %s\n", i,
			     ShaderDescribeSpecialization(*permutations[static_cast<size_t>(i)]->program)
			         .c_str());
		}
		EXIT("ShaderProgramCache: more than %u permutations for same %s shader: "
		     "shader=0x%016" PRIx64 " existing=%" PRIu64 " incoming_words=%" PRIu64
		     " program_hash0=0x%08" PRIx32 " program_crc32=0x%08" PRIx32 " program_ids=%" PRIu64
		     "\n",
		     ShaderMaxPermutationsPerProgram, stage, shader_hash,
		     static_cast<uint64_t>(permutations.size()),
		     static_cast<uint64_t>(permutation.program->bindings.descriptors.size()),
		     key.program_id.hash0, key.program_id.crc32,
		     static_cast<uint64_t>(key.program_id.ids.size()));
	}

	auto cached = std::make_unique<ShaderProgramPermutation>(std::move(permutation));
	auto spirv  = MakeShaderSpirvView(cached->spirv);
	permutations.push_back(std::move(cached));
	return spirv;
}

bool ShaderCompileInfoVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh,
                         ShaderLaneMaskMode lane_mask_mode, ShaderVertexInputInfo* info,
                         std::span<const uint32_t>* spirv) {
	EXIT_IF(spirv == nullptr);
	*spirv = {};

	if (!ShaderGetStaticInputInfoVS(regs, sh, info)) {
		return false;
	}
	const auto shader_hash = regs->gs_regs.chksum;
	const auto program_id  = ShaderGetIdVS(regs, info, false);
	const auto key =
	    MakeShaderStageProgramKey(ShaderType::Vertex, shader_hash, program_id, lane_mask_mode);

	{
		std::scoped_lock lock(g_shader_program_cache_mutex);
		if (auto iter = g_shader_program_cache.find(key); iter != g_shader_program_cache.end()) {
			for (const auto& permutation: iter->second) {
				if (TryUseVertexPermutation(*permutation, regs, info, shader_hash)) {
					*spirv = MakeShaderSpirvView(permutation->spirv);
					LogShaderProgramCacheHit("VS", shader_hash,
					                         static_cast<uint64_t>(spirv->size()));
					return true;
				}
			}
		}
	}

	std::vector<uint32_t> compiled_spirv;
	if (!ShaderCompileSpirvVS(regs, sh, lane_mask_mode, info, &compiled_spirv)) {
		return false;
	}

	ShaderProgramPermutation permutation {};
	permutation.spirv   = std::move(compiled_spirv);
	permutation.program = info->stage.program;
	*spirv = AddShaderProgramPermutation("VS", shader_hash, key, std::move(permutation));
	return true;
}

bool ShaderCompileInfoPS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh,
                         ShaderLaneMaskMode lane_mask_mode, const ShaderVertexInputInfo* vs_info,
                         std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
                         ShaderPixelInputInfo* ps_info, std::span<const uint32_t>* spirv) {
	EXIT_IF(spirv == nullptr);
	*spirv = {};

	ShaderGetStaticInputInfoPS(regs, sh, vs_info, target_export_mapping, ps_info);
	const auto shader_hash =
	    regs->ps_regs.chksum != 0 ? regs->ps_regs.chksum : regs->ps_regs.data_addr;
	const auto program_id = ShaderGetIdPS(regs, ps_info, false);
	const auto key =
	    MakeShaderStageProgramKey(ShaderType::Pixel, shader_hash, program_id, lane_mask_mode);

	{
		std::scoped_lock lock(g_shader_program_cache_mutex);
		if (auto iter = g_shader_program_cache.find(key); iter != g_shader_program_cache.end()) {
			for (const auto& permutation: iter->second) {
				if (TryUsePixelPermutation(*permutation, regs, ps_info, shader_hash)) {
					*spirv = MakeShaderSpirvView(permutation->spirv);
					LogShaderProgramCacheHit("PS", shader_hash,
					                         static_cast<uint64_t>(spirv->size()));
					return true;
				}
			}
		}
	}

	std::vector<uint32_t> compiled_spirv;
	if (!ShaderCompileSpirvPS(regs, sh, lane_mask_mode, ps_info, &compiled_spirv)) {
		return false;
	}

	ShaderProgramPermutation permutation {};
	permutation.spirv   = std::move(compiled_spirv);
	permutation.program = ps_info->stage.program;
	*spirv = AddShaderProgramPermutation("PS", shader_hash, key, std::move(permutation));
	return true;
}

bool ShaderCompileInfoCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh,
                         ShaderComputeInputInfo* info, std::span<const uint32_t>* spirv) {
	EXIT_IF(spirv == nullptr);
	*spirv = {};

	ShaderGetStaticInputInfoCS(regs, sh, info);
	const auto shader_hash = regs->cs_regs.data_addr;
	const auto program_id  = ShaderGetIdCS(regs, info, false);
	const auto key         = MakeShaderStageProgramKey(ShaderType::Compute, shader_hash, program_id,
	                                                   ShaderLaneMaskMode::NativeWave);

	{
		std::scoped_lock lock(g_shader_program_cache_mutex);
		if (auto iter = g_shader_program_cache.find(key); iter != g_shader_program_cache.end()) {
			for (const auto& permutation: iter->second) {
				if (TryUseComputePermutation(*permutation, regs, info, shader_hash)) {
					*spirv = MakeShaderSpirvView(permutation->spirv);
					LogShaderProgramCacheHit("CS", shader_hash,
					                         static_cast<uint64_t>(spirv->size()));
					return true;
				}
			}
		}
	}

	std::vector<uint32_t> compiled_spirv;
	if (!ShaderCompileSpirvCS(regs, sh, info, &compiled_spirv)) {
		return false;
	}

	ShaderProgramPermutation permutation {};
	permutation.spirv   = std::move(compiled_spirv);
	permutation.program = info->stage.program;
	*spirv = AddShaderProgramPermutation("CS", shader_hash, key, std::move(permutation));
	return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderDbgDumpInputInfo(const ShaderVertexInputInfo* info) {
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Vs)");

	LOGF("ShaderDbgDumpInputInfo()\n");

	LOGF("\t fetch_external = %s\n"
	     "\t fetch_embedded = %s\n"
	     "\t param_export_mask = 0x%08" PRIx32 "\n"
	     "\t export_count   = %d\n",
	     info->fetch_external ? "true" : "false", info->fetch_embedded ? "true" : "false",
	     info->param_export_mask, info->export_count);

	for (int i = 0; i < info->resources_num; i++) {
		LOGF("\t input %d\n", i);

		const auto& r  = info->resources[i];
		const auto& rd = info->resources_dst[i];

		LOGF("\t\t register_start   = %d\n"
		     "\t\t registers_num    = %d\n"
		     "\t\t fetch_index      = %" PRIu32 "\n",
		     rd.register_start, rd.registers_num, rd.fetch_index);
		LOGF("\t\t fields           = %08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "\n",
		     r.fields[3], r.fields[2], r.fields[1], r.fields[0]);
		LOGF("\t\t Base()           = %" PRIx64 "\n"
		     "\t\t Stride()         = %" PRIu16 "\n"
		     "\t\t SwizzleEnabled() = %s\n"
		     "\t\t NumRecords()     = %" PRIu32 "\n"
		     "\t\t DstSelX()        = %" PRIu8 "\n"
		     "\t\t DstSelY()        = %" PRIu8 "\n"
		     "\t\t DstSelZ()        = %" PRIu8 "\n"
		     "\t\t DstSelW()        = %" PRIu8 "\n",
		     r.Base48(), r.Stride(), r.SwizzleEnabled() ? "true" : "false", r.NumRecords(),
		     r.DstSelX(), r.DstSelY(), r.DstSelZ(), r.DstSelW());
		LOGF("\t\t Format()         = %" PRIu8 "\n"
		     "\t\t OutOfBounds()    = %" PRIu8 "\n",
		     r.Format(), r.OutOfBounds());
		LOGF("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
	}

	for (int i = 0; i < info->buffers_num; i++) {
		LOGF("\t buffer %d\n", i);

		const auto& r = info->buffers[i];
		LOGF("\t\t addr        = %" PRIx64 "\n"
		     "\t\t stride      = %" PRIu32 "\n"
		     "\t\t num_records = %" PRIu32 "\n"
		     "\t\t fetch_index = %" PRIu32 "\n"
		     "\t\t attr_num    = %" PRId32 "\n",
		     r.addr, r.stride, r.num_records, r.fetch_index, r.attr_num);
		for (int j = 0; j < r.attr_num; j++) {
			LOGF("\t\t attr_indices[%d]  = %d\n"
			     "\t\t attr_offsets[%d]  = %u\n",
			     j, r.attr_indices[j], j, r.attr_offsets[j]);
		}
	}
}

void ShaderDbgDumpInputInfo(const ShaderPixelInputInfo* info) {
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Ps)");

	LOGF("ShaderDbgDumpInputInfo()\n");

	LOGF("\t input_num            = %u\n"
	     "\t ps_system_input_base = %u\n"
	     "\t ps_pos_x             = %s\n"
	     "\t ps_pos_y             = %s\n"
	     "\t ps_pos_z             = %s\n"
	     "\t ps_pos_w             = %s\n"
	     "\t ps_front_face        = %s\n"
	     "\t ps_no_perspective    = %s\n"
	     "\t ps_pixel_kill_enable = %s\n"
	     "\t ps_early_z           = %s\n"
	     "\t ps_execute_on_noop   = %s\n",
	     info->input_num, info->ps_system_input_base, info->ps_pos_x ? "true" : "false",
	     info->ps_pos_y ? "true" : "false", info->ps_pos_z ? "true" : "false",
	     info->ps_pos_w ? "true" : "false", info->ps_front_face ? "true" : "false",
	     info->ps_no_perspective ? "true" : "false", info->ps_pixel_kill_enable ? "true" : "false",
	     info->ps_early_z ? "true" : "false", info->ps_execute_on_noop ? "true" : "false");

	for (uint32_t i = 0; i < info->input_num; i++) {
		LOGF("\t interpolator_settings[%u] = %u\n", i, info->interpolator_settings[i]);
	}
}

void ShaderDbgDumpInputInfo(const ShaderComputeInputInfo* info) {
	LOGF("ShaderDbgDumpInputInfo()\n");

	LOGF("\t workgroup_register = %d\n"
	     "\t thread_ids_num     = %d\n"
	     "\t wave_size          = %u\n"
	     "\t threads_num        = {%u, %u, %u}\n"
	     "\t tg_size_en         = %s\n",
	     info->workgroup_register, info->thread_ids_num, info->wave_size, info->threads_num[0],
	     info->threads_num[1], info->threads_num[2], info->tg_size_en ? "true" : "false");
	LOGF("\t threadgroup_id     = {%s, %s, %s}\n", info->group_id[0] ? "true" : "false",
	     info->group_id[1] ? "true" : "false", info->group_id[2] ? "true" : "false");
}

static bool ShaderRecompilerTextDumpEnabled() {
	// Graphics debug dump already writes SPIR-V binaries to _Shaders. Keep the very large
	// disassembly/IR text behind the shader-log switch so file logging cannot stall boot.
	return Config::GetShaderLogDirection() != Config::ShaderLogDirection::Silent;
}

static void DumpShaderRecompilerSpirv(const char* type, uint64_t shader_hash,
                                      const std::vector<uint32_t>& bin) {
	if (!Config::GraphicsDebugDumpEnabled()) {
		return;
	}

	static std::atomic_int id = 0;

	const auto base_name = Config::GetShaderLogFolder() /
	                       fmt::format("{:04d}_{:04d}_new_shader_{}_{:016x}",
	                                   GraphicsRunGetFrameNum(), id++, type, shader_hash);
	Common::File::CreateDirectories(base_name.parent_path());

	Common::File spv_file;
	auto         spv_name = base_name;
	spv_name += ".spv";
	spv_file.Create(spv_name);
	if (spv_file.IsInvalid()) {
		auto spv_name_text = Common::PathToString(spv_name);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", spv_name_text.c_str());
	} else {
		spv_file.Write(bin.data(), bin.size() * 4);
		spv_file.Close();
	}

	return;

	std::string text;
	if (!SpirvDisassemble(bin.data(), bin.size(), &text)) {
		auto spv_name_text = Common::PathToString(spv_name);
		LOGF_COLOR(Log::Color::BrightRed, "SpirvDisassemble() failed for %s\n",
		           spv_name_text.c_str());
		return;
	}

	Common::File asm_file;
	auto         asm_name = base_name;
	asm_name += ".spvasm";
	asm_file.Create(asm_name);
	if (asm_file.IsInvalid()) {
		auto asm_name_text = Common::PathToString(asm_name);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", asm_name_text.c_str());
	} else {
		asm_file.Printf("%s", text.c_str());
		asm_file.Close();
	}
}

static void DumpShaderRecompilerOriginal(const char* type, uint64_t shader_hash,
                                         std::span<const uint32_t> code,
                                         const std::string&        decoded_dump) {
	// if (!Config::GraphicsDebugDumpEnabled()) {
	//	return;
	// }
	return;
	EXIT_IF(code.empty());

	static std::atomic_int id = 0;

	const auto base_name = Config::GetShaderLogFolder() / "original" /
	                       fmt::format("{:04d}_{:04d}_new_shader_{}_{:016x}",
	                                   GraphicsRunGetFrameNum(), id++, type, shader_hash);
	Common::File::CreateDirectories(base_name.parent_path());

	Common::File bin_file;
	auto         bin_name = base_name;
	bin_name += ".bin";
	bin_file.Create(bin_name);
	if (bin_file.IsInvalid()) {
		auto bin_name_text = Common::PathToString(bin_name);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", bin_name_text.c_str());
	} else {
		bin_file.Write(code.data(), code.size_bytes());
		bin_file.Close();
	}

	Common::File text_file;
	auto         text_name = base_name;
	text_name += ".rdna2";
	text_file.Create(text_name);
	if (text_file.IsInvalid()) {
		auto text_name_text = Common::PathToString(text_name);
		LOGF_COLOR(Log::Color::BrightRed, "Can't create file: %s\n", text_name_text.c_str());
	} else {
		text_file.Printf("%s", decoded_dump.c_str());
		text_file.Close();
	}
}

bool ShaderCompileSpirvVS(const HW::VertexShaderInfo* regs, const HW::ShaderRegisters* sh,
                          ShaderLaneMaskMode lane_mask_mode, ShaderVertexInputInfo* input_info,
                          std::vector<uint32_t>* spirv) {
	KYTY_PROFILER_FUNCTION(profiler::colors::Amber300);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);
	EXIT_IF(input_info == nullptr);
	EXIT_IF(spirv == nullptr);

	EXIT_NOT_IMPLEMENTED(regs->es_regs.data_addr == 0 || regs->gs_regs.chksum == 0);

	const uint64_t shader_addr = regs->es_regs.data_addr;
	const auto code = ShaderGetMappedCode(shader_addr, "ShaderRecompiler VS", regs->gs_regs.chksum);

	ShaderRecompiler::CompileOptions options;
	options.stage                = ShaderType::Vertex;
	options.lane_mask_mode       = lane_mask_mode;
	options.shader_hash          = regs->gs_regs.chksum;
	options.shader_base          = shader_addr;
	options.user_data_base       = 8;
	options.user_data_count      = regs->gs_regs.rsrc2.user_sgpr;
	options.user_data            = regs->gs_user_sgpr.value;
	options.descriptor_set       = 0;
	options.push_constant_offset = 0;
	options.vertex_input_info    = input_info;
	options.dump_ir              = ShaderRecompilerTextDumpEnabled();
	options.early_dump           = options.dump_ir;
	options.dump_label           = "ShaderRecompiler VS";

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	if (!ShaderRecompiler::TryRecompile(code, options, &result, &error)) {
		ExitShaderRecompilerFailure("ShaderRecompiler VS", options.shader_hash, error.c_str());
	}
	DumpShaderRecompilerOriginal("vs", options.shader_hash, code, result.decoded_dump);
	if (!SpirvValidateBinary("ShaderRecompiler VS", options.shader_hash, result.spirv)) {
		DumpShaderRecompilerSpirv("vs", options.shader_hash, result.spirv);
		ExitShaderRecompilerFailure("ShaderRecompiler VS", options.shader_hash,
		                            "SPIR-V validation failed");
	}

	input_info->stage.program =
	    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(result.program));
	input_info->stage.resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(std::move(result.resources));
	ApplyVertexOutputs(input_info, *input_info->stage.program);
	*spirv = std::move(result.spirv);
	DumpShaderRecompilerSpirv("vs", options.shader_hash, *spirv);

	if (options.dump_ir) {
		if (!options.early_dump) {
			LOGF("ShaderRecompiler VS decoded RDNA2:\n%s", result.decoded_dump.c_str());
			LOGF("ShaderRecompiler VS IR:\n%s", result.ir_dump.c_str());
		}
		LOGF("ShaderRecompiler VS SPIR-V words=%" PRIu64 "\n",
		     static_cast<uint64_t>(spirv->size()));
	}
	return true;
}

bool ShaderCompileSpirvPS(const HW::PixelShaderInfo* regs, const HW::ShaderRegisters* sh,
                          ShaderLaneMaskMode lane_mask_mode, ShaderPixelInputInfo* input_info,
                          std::vector<uint32_t>* spirv) {
	KYTY_PROFILER_FUNCTION(profiler::colors::Blue300);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);
	EXIT_IF(input_info == nullptr);
	EXIT_IF(spirv == nullptr);

	const uint64_t shader_addr = regs->ps_regs.data_addr;
	const uint64_t shader_hash = regs->ps_regs.chksum != 0 ? regs->ps_regs.chksum : shader_addr;
	const auto     code = ShaderGetMappedCode(shader_addr, "ShaderRecompiler PS", shader_hash);

	ShaderRecompiler::CompileOptions options;
	options.stage                = ShaderType::Pixel;
	options.lane_mask_mode       = lane_mask_mode;
	options.shader_hash          = shader_hash;
	options.shader_base          = shader_addr;
	options.user_data_count      = regs->ps_regs.rsrc2.user_sgpr;
	options.user_data            = regs->ps_user_sgpr.value;
	options.descriptor_set       = input_info->descriptor_set;
	options.push_constant_offset = 0;
	options.pixel_input_info     = input_info;
	options.dump_ir              = ShaderRecompilerTextDumpEnabled();
	options.early_dump           = options.dump_ir;
	options.dump_label           = "ShaderRecompiler PS";

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	if (!ShaderRecompiler::TryRecompile(code, options, &result, &error)) {
		ExitShaderRecompilerFailure("ShaderRecompiler PS", options.shader_hash, error.c_str());
	}
	DumpShaderRecompilerOriginal("ps", options.shader_hash, code, result.decoded_dump);
	if (!SpirvValidateBinary("ShaderRecompiler PS", options.shader_hash, result.spirv)) {
		DumpShaderRecompilerSpirv("ps", options.shader_hash, result.spirv);
		ExitShaderRecompilerFailure("ShaderRecompiler PS", options.shader_hash,
		                            "SPIR-V validation failed");
	}
	input_info->stage.program =
	    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(result.program));
	input_info->stage.resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(std::move(result.resources));
	ApplyPixelOutputs(input_info, *input_info->stage.program);
	*spirv = std::move(result.spirv);
	DumpShaderRecompilerSpirv("ps", options.shader_hash, *spirv);

	if (options.dump_ir) {
		if (!options.early_dump) {
			LOGF("ShaderRecompiler PS decoded RDNA2:\n%s", result.decoded_dump.c_str());
			LOGF("ShaderRecompiler PS IR:\n%s", result.ir_dump.c_str());
		}
		LOGF("ShaderRecompiler PS SPIR-V words=%" PRIu64 "\n",
		     static_cast<uint64_t>(spirv->size()));
	}
	return true;
}

bool ShaderCompileSpirvCS(const HW::ComputeShaderInfo* regs, const HW::ShaderRegisters* sh,
                          ShaderComputeInputInfo* input_info, std::vector<uint32_t>* spirv) {
	KYTY_PROFILER_FUNCTION(profiler::colors::CyanA700);

	EXIT_IF(regs == nullptr);
	EXIT_IF(sh == nullptr);
	EXIT_IF(input_info == nullptr);
	EXIT_IF(spirv == nullptr);

	const uint64_t shader_addr = regs->cs_regs.data_addr;
	const auto     code = ShaderGetMappedCode(shader_addr, "ShaderRecompiler CS", shader_addr);

	ShaderRecompiler::CompileOptions options;
	options.stage                = ShaderType::Compute;
	options.shader_hash          = shader_addr;
	options.shader_base          = shader_addr;
	options.user_data_count      = regs->cs_regs.user_sgpr;
	options.user_data            = regs->cs_user_sgpr.value;
	options.descriptor_set       = 0;
	options.push_constant_offset = 0;
	options.compute_input_info   = input_info;
	options.wave_size            = input_info->wave_size;
	options.dump_ir              = ShaderRecompilerTextDumpEnabled();
	options.early_dump           = options.dump_ir;
	options.dump_label           = "ShaderRecompiler CS";

	ShaderRecompiler::CompileResult result;
	std::string                     error;
	if (!ShaderRecompiler::TryRecompile(code, options, &result, &error)) {
		ExitShaderRecompilerFailure("ShaderRecompiler CS", options.shader_hash, error.c_str());
	}
	DumpShaderRecompilerOriginal("cs", options.shader_hash, code, result.decoded_dump);
	if (!SpirvValidateBinary("ShaderRecompiler CS", options.shader_hash, result.spirv)) {
		DumpShaderRecompilerSpirv("cs", options.shader_hash, result.spirv);
		ExitShaderRecompilerFailure("ShaderRecompiler CS", options.shader_hash,
		                            "SPIR-V validation failed");
	}
	input_info->stage.program =
	    std::make_shared<const ShaderRecompiler::IR::Program>(std::move(result.program));
	input_info->stage.resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(std::move(result.resources));
	*spirv = std::move(result.spirv);
	DumpShaderRecompilerSpirv("cs", options.shader_hash, *spirv);

	if (options.dump_ir) {
		if (!options.early_dump) {
			LOGF("ShaderRecompiler CS decoded RDNA2:\n%s", result.decoded_dump.c_str());
			LOGF("ShaderRecompiler CS IR:\n%s", result.ir_dump.c_str());
		}
		LOGF("ShaderRecompiler CS SPIR-V words=%" PRIu64 " wave_size=%u\n",
		     static_cast<uint64_t>(spirv->size()), options.wave_size);
	}
	return true;
}

ShaderId ShaderGetIdVS(const HW::VertexShaderInfo* regs, const ShaderVertexInputInfo* input_info,
                       bool include_bind_specialization) {
	KYTY_PROFILER_FUNCTION();

	ShaderId ret;

	ret.ids.reserve(64);

	EXIT_NOT_IMPLEMENTED(regs->es_regs.data_addr == 0 || regs->gs_regs.chksum == 0);

	ret.hash0 = (regs->gs_regs.chksum >> 32u) & 0xffffffffu;
	ret.crc32 = regs->gs_regs.chksum & 0xffffffffu;

	ret.ids.push_back(static_cast<uint32_t>(input_info->fetch_external));
	ret.ids.push_back(static_cast<uint32_t>(input_info->fetch_embedded));
	ret.ids.push_back(input_info->resources_num);
	ret.ids.push_back(input_info->export_count);

	for (int i = 0; i < input_info->resources_num; i++) {
		const auto& r  = input_info->resources[i];
		const auto& rd = input_info->resources_dst[i];

		ret.ids.push_back(rd.register_start);
		ret.ids.push_back(rd.registers_num);
		ret.ids.push_back(rd.fetch_index);
		ret.ids.push_back(r.Stride());
		ret.ids.push_back(static_cast<uint32_t>(r.SwizzleEnabled()));
		ret.ids.push_back(r.DstSelX());
		ret.ids.push_back(r.DstSelY());
		ret.ids.push_back(r.DstSelZ());
		ret.ids.push_back(r.DstSelW());
		ret.ids.push_back(r.Format());
		ret.ids.push_back(r.OutOfBounds());
		ret.ids.push_back(static_cast<uint32_t>(r.AddTid()));
	}

	ret.ids.push_back(input_info->buffers_num);

	for (int i = 0; i < input_info->buffers_num; i++) {
		const auto& r = input_info->buffers[i];
		ret.ids.push_back(r.attr_num);
		ret.ids.push_back(r.stride);
		ret.ids.push_back(r.fetch_index);
		for (int j = 0; j < r.attr_num; j++) {
			ret.ids.push_back(r.attr_indices[j]);
			ret.ids.push_back(r.attr_offsets[j]);
		}
	}

	if (include_bind_specialization) {
		EXIT_IF(!input_info->stage);
		ShaderAppendNativeSpecialization(&ret.ids, *input_info->stage.program);
	}

	return ret;
}

ShaderId ShaderGetIdPS(const HW::PixelShaderInfo* regs, const ShaderPixelInputInfo* input_info,
                       bool include_bind_specialization) {
	KYTY_PROFILER_FUNCTION();

	ShaderId ret;

	ret.ids.reserve(64);

	ret.hash0 = (regs->ps_regs.chksum >> 32u) & 0xffffffffu;
	ret.crc32 = regs->ps_regs.chksum & 0xffffffffu;

	ret.ids.push_back(input_info->descriptor_set);
	ret.ids.push_back(input_info->input_num);
	ret.ids.push_back(input_info->ps_system_input_base);
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_pos_x));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_pos_y));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_pos_z));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_pos_w));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_front_face));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_pixel_kill_enable));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_sample_mask_export_enable));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_early_z));
	ret.ids.push_back(static_cast<uint32_t>(input_info->ps_execute_on_noop));

	for (auto mode: input_info->target_output_mode) {
		ret.ids.push_back(mode);
	}
	for (uint32_t base = 0; base < input_info->target_export_mapping.size(); base += 4u) {
		uint32_t packed = 0;
		for (uint32_t i = 0; i < 4u; i++) {
			packed |= static_cast<uint32_t>(input_info->target_export_mapping[base + i].packed)
			          << (i * 8u);
		}
		ret.ids.push_back(packed);
	}
	ret.ids.push_back(input_info->mrt_output_mask);

	for (uint32_t i = 0; i < input_info->input_num; i++) {
		ret.ids.push_back(input_info->interpolator_settings[i]);
	}

	if (include_bind_specialization) {
		EXIT_IF(!input_info->stage);
		ShaderAppendNativeSpecialization(&ret.ids, *input_info->stage.program);
	}

	return ret;
}

ShaderId ShaderGetIdCS(const HW::ComputeShaderInfo* regs, const ShaderComputeInputInfo* input_info,
                       bool include_bind_specialization) {
	const auto* src = reinterpret_cast<const uint32_t*>(regs->cs_regs.data_addr);

	EXIT_NOT_IMPLEMENTED(src == nullptr);

	const auto* header = GetBinaryInfo(src);

	ShaderId ret;
	ret.ids.reserve(64);

	if (header != nullptr) {
		ret.hash0 = header->hash0;
		ret.crc32 = header->crc32;
		ret.ids.push_back(header->length);
	} else {
		GetNextGenFallbackShaderId(regs->cs_regs.data_addr, &ret.hash0, &ret.crc32);
		ret.ids.push_back(static_cast<uint32_t>(regs->cs_regs.data_addr & 0xffffffffu));
		ret.ids.push_back(static_cast<uint32_t>((regs->cs_regs.data_addr >> 32u) & 0xffffffffu));
	}

	ret.ids.push_back(input_info->workgroup_register);
	ret.ids.push_back(input_info->wave_size);
	ret.ids.push_back(input_info->thread_ids_num);

	for (int i = 0; i < 3; i++) {
		ret.ids.push_back(input_info->threads_num[i]);
		ret.ids.push_back(static_cast<uint32_t>(input_info->group_id[i]));
	}
	ret.ids.push_back(static_cast<uint32_t>(input_info->dispatch_thread_dimensions));
	for (uint32_t dim: input_info->dispatch_threads_num) {
		ret.ids.push_back(dim);
	}

	if (include_bind_specialization) {
		EXIT_IF(!input_info->stage);
		ShaderAppendNativeSpecialization(&ret.ids, *input_info->stage.program);
	}

	return ret;
}

bool ShaderAddressValid(uint64_t addr) {
	return reinterpret_cast<const uint32_t*>(addr) != nullptr;
}

} // namespace Libs::Graphics
