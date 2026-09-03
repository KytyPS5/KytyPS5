#include "graphics/shader/shader.h"

#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/magicEnum.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/shaderCompiler.h"
#include "graphics/shader/shaderVertexMetadata.h"
#include "libs/errno.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <xxhash.h>

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

void ShaderInit() {
	EXIT_IF(g_shader_map != nullptr);

	g_shader_map = std::make_unique<std::unordered_map<uint64_t, ShaderMappedData>>();
}

void ShaderMapUserData(uint64_t addr, const ShaderMappedData& data) {
	EXIT_IF(g_shader_map == nullptr);

	std::scoped_lock lock(g_shader_map_mutex);

	(*g_shader_map)[addr] = data;
}

static ShaderMappedData ShaderGetMappedData(uint64_t addr, const char* label) {
	EXIT_IF(g_shader_map == nullptr);

	std::scoped_lock lock(g_shader_map_mutex);

	if (auto iter = g_shader_map->find(addr); iter != g_shader_map->end()) {
		return iter->second;
	}

	EXIT("%s shader=0x%016" PRIx64 " is missing from ShaderMap\n", label, addr);
}

static const ShaderBinaryInfo* GetBinaryInfo(const uint32_t* code) {
	EXIT_IF(code == nullptr);

	if (code[0] == 0xBEEB03FF) {
		return reinterpret_cast<const ShaderBinaryInfo*>(code +
		                                                 static_cast<size_t>(code[1] + 1) * 2);
	}

	return nullptr;
}

static uint64_t GetDeclaredShaderHash(uint64_t shader_addr) {
	const auto* header = GetBinaryInfo(reinterpret_cast<const uint32_t*>(shader_addr));
	return header != nullptr ? (static_cast<uint64_t>(header->hash1) << 32u) | header->hash0 : 0;
}

static ShaderParams GetShaderParams(uint64_t shader_addr, const char* label, uint64_t declared_hash,
	                                std::span<const uint32_t> user_data,
	                                const ShaderMappedData& data) {
	if (data.code_size_bytes == 0 || data.code_size_bytes % sizeof(uint32_t) != 0) {
		EXIT("%s hash=0x%016" PRIx64 " shader=0x%016" PRIx64
		     " has invalid AGC shader_size=0x%08" PRIx32 "\n",
		     label, declared_hash, shader_addr, data.code_size_bytes);
	}
	const auto code_words = data.code_size_bytes / sizeof(uint32_t);
	const auto code = std::span {reinterpret_cast<const uint32_t*>(shader_addr), code_words};
	return {
	    .code      = code,
	    .user_data = user_data,
	    .hash      = declared_hash != 0 ? declared_hash
	                                    : XXH3_64bits(code.data(), code.size_bytes()),
	};
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
		EXIT_NOT_IMPLEMENTED(vs.gs_regs.rsrc1.threadgroup_configuration != false);
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
	EXIT_NOT_IMPLEMENTED(ps.rsrc2.raster_ordered_shading != 0);
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
	EXIT_NOT_IMPLEMENTED(cs.priority != 0x00);
	EXIT_NOT_IMPLEMENTED(cs.debug_mode != false);
	EXIT_NOT_IMPLEMENTED(cs.require_forward_progress != false);
	EXIT_NOT_IMPLEMENTED(cs.shared_vgprs != 0x00);
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

static void ShaderDetectBuffers(ShaderVertexInputInfo& info) {
	KYTY_PROFILER_FUNCTION();

	info.buffers_num = 0;

	for (int ri = 0; ri < info.resources_num; ri++) {
		const auto& r = info.resources[ri];

		bool merged = false;
		for (int bi = 0; bi < info.buffers_num; bi++) {
			auto& b = info.buffers[bi];

			uint64_t stride = b.stride;

			if (stride == r.Stride() &&
			    b.fetch_index == static_cast<uint32_t>(info.resources_dst[ri].fetch_index)) {
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
			EXIT_NOT_IMPLEMENTED(info.buffers_num >= ShaderVertexInputInfo::RES_MAX);
			int bi                           = info.buffers_num++;
			info.buffers[bi].addr            = r.Base48();
			info.buffers[bi].stride          = r.Stride();
			info.buffers[bi].num_records     = r.NumRecords();
			info.buffers[bi].fetch_index     = info.resources_dst[ri].fetch_index;
			info.buffers[bi].attr_num        = 1;
			info.buffers[bi].attr_indices[0] = ri;
		}
	}

	for (int bi = 0; bi < info.buffers_num; bi++) {
		auto& b = info.buffers[bi];
		for (int ri = 0; ri < b.attr_num; ri++) {
			b.attr_offsets[ri] = info.resources[b.attr_indices[ri]].Base48() - b.addr;
		}
	}
}

static Prospero::BufferFormat
VertexAttribFormatToBufferFormat(Prospero::VertexAttribFormat format) {
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
		if (format == entry.vertex) {
			return entry.buffer;
		}
	}

	return static_cast<Prospero::BufferFormat>(static_cast<uint32_t>(format));
}

static void ShaderApplyAttribSemantics(ShaderVertexInputInfo& info,
                                       const ShaderSemantic*  input_semantics,
                                       uint32_t num_input_semantics, const uint32_t* attrib,
                                       const uint32_t* buffer) {
	KYTY_PROFILER_FUNCTION();

	EXIT_IF(attrib == nullptr || buffer == nullptr);

	for (uint32_t i = 0; i < num_input_semantics; i++) {
		const auto& in = input_semantics[i];

		EXIT_NOT_IMPLEMENTED(in.static_vb_index == 1 || in.static_attribute == 1);

		uint32_t reg  = in.hardware_mapping;
		uint32_t size = in.size_in_elements;

		if (Config::GraphicsDebugDumpEnabled()) {
			LOGF("reg = %u, size = %u, va[%u] = 0x%08" PRIx32 "\n", reg, size, i,
			     attrib[in.semantic]);
		}

		size_t index = attrib[in.semantic] & 0x1fu;
		auto   format =
		    static_cast<Prospero::VertexAttribFormat>((attrib[in.semantic] >> 5u) & 0x1ffu);
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

		EXIT_NOT_IMPLEMENTED(info.resources_num >= ShaderVertexInputInfo::RES_MAX);

		auto& r           = info.resources[info.resources_num];
		auto& rd          = info.resources_dst[info.resources_num];
		rd.register_start = static_cast<int>(reg);
		rd.registers_num  = static_cast<int>(size);
		rd.attr_id        = static_cast<int>(in.semantic);
		rd.fetch_index    = fetch_index;
		r.fields[0]       = sharp[0];
		r.fields[1]       = sharp[1];
		r.fields[2]       = sharp[2];
		r.fields[3]       = sharp[3];
		if (format != Prospero::VertexAttribFormat::kInvalid) {
			auto                         buffer_format = VertexAttribFormatToBufferFormat(format);
			static std::atomic<uint64_t> log_count     = 0;
			auto                         log_id        = log_count.fetch_add(1);
			if (log_id < 64) {
				LOGF("\t temporary: PS5 vertex attrib semantic %u uses attrib format %u -> buffer "
				     "format %u, offset %u, buffer index %zu\n",
				     static_cast<uint32_t>(in.semantic), static_cast<uint32_t>(format),
				     static_cast<uint32_t>(buffer_format), offset, index);
			}
			const auto buffer_format_raw = static_cast<uint32_t>(buffer_format);
			r.fields[3] = (r.fields[3] & ~((0x7fu << 12u) | 0xfffu)) |
			              ((buffer_format_raw & 0x7fu) << 12u) | DstSel(4, 5, 6, 7);
		}
		if (offset != 0) {
			r.UpdateAddress48(r.Base48() + offset);
		}

		info.resources_num++;
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

static bool ShaderGetStaticInputInfoVS(const HW::VertexShaderInfo& regs,
	                                   const HW::ShaderRegisters& sh,
	                                   const ShaderMappedData& data,
	                                   ShaderVertexInputInfo& info) {
	KYTY_PROFILER_FUNCTION();

	info = {};

	info.pa_cl_vs_out_cntl = sh.m_paClVsOutCntl;

	EXIT_NOT_IMPLEMENTED(regs.es_regs.data_addr == 0);

	uint64_t                shader_addr   = regs.es_regs.data_addr;
	const HW::UserSgprInfo& user_sgpr     = regs.gs_user_sgpr;
	auto                    user_sgpr_num = regs.gs_regs.rsrc2.user_sgpr;
	info.scratch_size_dwords = data.scratch_size_dwords;

	if (data.user_data == nullptr) {
		LOGF("ShaderGetInputInfoVS(): no AGC user data for shader=0x%016" PRIx64 " es=0x%016" PRIx64
		     " gs=0x%016" PRIx64 " user_sgpr_num=%u\n",
		     shader_addr, regs.es_regs.data_addr, regs.gs_regs.data_addr,
		     static_cast<uint32_t>(user_sgpr_num));
	}
	ShaderVertexMetadata metadata;
	std::string          metadata_error;
	if (!ShaderReadVertexMetadata(data, HW::UserSgprInfo::SGPRS_MAX, metadata, &metadata_error)) {
		LOGF("ShaderGetInputInfoVS(): invalid AGC metadata shader=0x%016" PRIx64 ": %s\n",
		     shader_addr, metadata_error.c_str());
		return false;
	}

	if (metadata.vertex_attrib_reg >= 0) {
		info.fetch_external   = false;
		info.fetch_embedded   = true;
		info.fetch_attrib_reg = metadata.vertex_attrib_reg;
		info.fetch_buffer_reg = metadata.vertex_buffer_reg;

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
    const HW::PixelShaderInfo& regs, const HW::ShaderRegisters& sh,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	const ShaderMappedData& data, ShaderPixelInputInfo& ps_info) {
	KYTY_PROFILER_FUNCTION();

	ps_info = {};
	ps_info.scratch_size_dwords = data.scratch_size_dwords;

	// SPI_PS_IN_CONTROL.NUM_INTERP occupies bits 5:0. Keep the remaining control
	// flags in the hardware state and extract only the input count here.
	ps_info.input_num            = sh.ps_in_control & 0x3fu;
	EXIT_NOT_IMPLEMENTED(ps_info.input_num > std::size(ps_info.interpolator_settings));
	ps_info.ps_system_input_base = ShaderCalcPsSystemInputBase(sh);
	const uint32_t active_inputs = sh.ps_input_ena & sh.ps_input_addr;
	if ((active_inputs & 0x00000002u) != 0) {
		ps_info.ps_perspective_center_vgpr = (active_inputs & 0x00000001u) != 0 ? 2u : 0u;
	}
	for (uint32_t i = 0; i < data.num_input_semantics && i < ps_info.input_num && i < 32u; i++) {
		const auto& semantic = data.input_semantics[i];
		if (semantic.is_custom != 0 && semantic.is_f16 == 0) {
			ps_info.custom_interpolation_mask |= 1u << i;
		}
	}
	ps_info.ps_pos_x                     = (active_inputs & 0x00000100u) != 0;
	ps_info.ps_pos_y                     = (active_inputs & 0x00000200u) != 0;
	ps_info.ps_pos_xy                    = ps_info.ps_pos_x && ps_info.ps_pos_y;
	ps_info.ps_pos_z                     = (active_inputs & 0x00000400u) != 0;
	ps_info.ps_pos_w                     = (active_inputs & 0x00000800u) != 0;
	ps_info.ps_front_face                = (active_inputs & 0x00001000u) != 0;
	ps_info.ps_sample_shading            = (active_inputs & 0x00000011u) != 0;
	ps_info.ps_no_perspective            = (sh.ps_input_ena & sh.ps_input_addr & 0x00000020u) != 0;
	ps_info.ps_pixel_kill_enable         = sh.db_shader_control.shader_kill_enable;
	ps_info.ps_depth_export_enable       = sh.db_shader_control.shader_z_export_enable;
	ps_info.ps_sample_mask_export_enable = sh.db_shader_control.shader_mask_export_enable;
	ps_info.ps_early_z =
	    (sh.db_shader_control.shader_z_behavior == 1 && !sh.db_shader_control.shader_kill_enable &&
	     !sh.db_shader_control.shader_z_export_enable &&
	     !sh.db_shader_control.shader_mask_export_enable);
	ps_info.ps_execute_on_noop = sh.db_shader_control.shader_execute_on_noop;

	for (uint32_t i = 0; i < ps_info.input_num; i++) {
		ps_info.interpolator_settings[i] = sh.ps_interpolator_settings[i];
	}

	for (int i = 0; i < 8; i++) {
		ps_info.target_output_mode[i]    = sh.target_output_mode[i];
		ps_info.target_export_mapping[i] = sh.target_output_mode[i] != 0
		                                       ? target_export_mapping[i]
		                                       : Prospero::ColorComponentMapping {};
	}
}

static void ShaderGetStaticInputInfoCS(const HW::ComputeShaderInfo& regs,
	                                   const HW::ShaderRegisters& /*sh*/,
	                                   const ShaderMappedData& data,
	                                   ShaderComputeInputInfo& info) {
	const bool dispatch_thread_dimensions = info.dispatch_thread_dimensions;
	const bool needs_lds_barriers          = info.needs_lds_barriers;
	info = {};
	info.dispatch_thread_dimensions = dispatch_thread_dimensions;
	info.needs_lds_barriers          = needs_lds_barriers;
	info.threads_num[0]      = regs.cs_regs.num_thread_x;
	info.threads_num[1]      = regs.cs_regs.num_thread_y;
	info.threads_num[2]      = regs.cs_regs.num_thread_z;
	info.lds_size_dwords     = static_cast<uint32_t>(regs.cs_regs.lds_size) * 128u;
	info.scratch_size_dwords = data.scratch_size_dwords;
	info.group_id[0]         = regs.cs_regs.tgid_x_en != 0;
	info.group_id[1]         = regs.cs_regs.tgid_y_en != 0;
	info.group_id[2]         = regs.cs_regs.tgid_z_en != 0;
	info.wave_size           = regs.cs_regs.wave_size;
	info.thread_ids_num      = regs.cs_regs.tidig_comp_cnt + 1;
	info.tg_size_en          = regs.cs_regs.tg_size_en != 0;

	info.workgroup_register = regs.cs_regs.user_sgpr;
}

void BuildStageStaticKey(const ShaderVertexInputInfo& info, std::vector<uint32_t>& key) {
	EXIT_IF(info.resources_num < 0 || info.resources_num > ShaderVertexInputInfo::RES_MAX);
	key.clear();
	key.push_back(static_cast<uint32_t>(info.fetch_embedded));
	key.push_back(static_cast<uint32_t>(info.fetch_attrib_reg));
	key.push_back(static_cast<uint32_t>(info.fetch_buffer_reg));
	key.push_back(info.resources_num);
	key.push_back(info.scratch_size_dwords);
	key.push_back(info.pa_cl_vs_out_cntl);
	key.push_back(static_cast<uint32_t>(info.clip_space.enabled));
	if (info.clip_space.enabled) {
		for (const float value: info.clip_space.scale) {
			key.push_back(std::bit_cast<uint32_t>(value));
		}
		for (const float value: info.clip_space.offset) {
			key.push_back(std::bit_cast<uint32_t>(value));
		}
		for (const float value: info.clip_space.half_extent) {
			key.push_back(std::bit_cast<uint32_t>(value));
		}
	}

	for (int i = 0; i < info.resources_num; i++) {
		const auto& resource    = info.resources[i];
		const auto& destination = info.resources_dst[i];
		key.push_back(destination.register_start);
		key.push_back(destination.registers_num);
		key.push_back(destination.fetch_index);
		key.push_back(static_cast<uint32_t>(destination.attr_id));
		key.push_back(resource.Stride());
		key.push_back(static_cast<uint32_t>(resource.SwizzleEnabled()));
		key.push_back(resource.DstSelX());
		key.push_back(resource.DstSelY());
		key.push_back(resource.DstSelZ());
		key.push_back(resource.DstSelW());
		key.push_back(resource.RawFormat());
		key.push_back(resource.OutOfBounds());
		key.push_back(static_cast<uint32_t>(resource.AddTid()));
	}
}

void BuildStageStaticKey(const ShaderPixelInputInfo& info, std::vector<uint32_t>& key) {
	EXIT_IF(info.input_num > std::size(info.interpolator_settings));
	key.clear();
	key.push_back(info.scratch_size_dwords);
	key.push_back(info.input_num);
	key.push_back(info.ps_system_input_base);
	key.push_back(info.custom_interpolation_mask);
	key.push_back(info.ps_perspective_center_vgpr);
	key.push_back(static_cast<uint32_t>(info.ps_pos_x));
	key.push_back(static_cast<uint32_t>(info.ps_pos_y));
	key.push_back(static_cast<uint32_t>(info.ps_pos_z));
	key.push_back(static_cast<uint32_t>(info.ps_pos_w));
	key.push_back(static_cast<uint32_t>(info.ps_front_face));
	key.push_back(static_cast<uint32_t>(info.ps_no_perspective));
	key.push_back(static_cast<uint32_t>(info.ps_pixel_kill_enable));
	key.push_back(static_cast<uint32_t>(info.ps_depth_export_enable));
	key.push_back(static_cast<uint32_t>(info.ps_sample_mask_export_enable));
	key.push_back(static_cast<uint32_t>(info.ps_early_z));
	key.insert(key.end(), std::begin(info.target_output_mode), std::end(info.target_output_mode));
	for (uint32_t base = 0; base < info.target_export_mapping.size(); base += 4u) {
		uint32_t packed = 0;
		for (uint32_t i = 0; i < 4u; i++) {
			packed |= static_cast<uint32_t>(info.target_export_mapping[base + i].packed)
			          << (i * 8u);
		}
		key.push_back(packed);
	}
	key.insert(key.end(), std::begin(info.interpolator_settings),
	           std::begin(info.interpolator_settings) + info.input_num);
}

void BuildStageStaticKey(const ShaderComputeInputInfo& info, std::vector<uint32_t>& key) {
	key.clear();
	key.push_back(info.workgroup_register);
	key.push_back(info.wave_size);
	key.push_back(info.thread_ids_num);
	key.push_back(info.lds_size_dwords);
	key.push_back(info.scratch_size_dwords);
	key.push_back(static_cast<uint32_t>(info.needs_lds_barriers));
	key.push_back(static_cast<uint32_t>(info.dispatch_thread_dimensions));
	for (int i = 0; i < 3; i++) {
		key.push_back(info.threads_num[i]);
		key.push_back(static_cast<uint32_t>(info.group_id[i]));
	}
	key.push_back(static_cast<uint32_t>(info.tg_size_en));
}

ShaderParams PrepareProgram(const HW::VertexShaderInfo& regs, const HW::ShaderRegisters& sh,
                            ShaderVertexInputInfo& info) {
	const auto data = ShaderGetMappedData(regs.es_regs.data_addr, "ShaderGetInputInfoVS():");
	if (!ShaderGetStaticInputInfoVS(regs, sh, data, info)) {
		EXIT("failed to prepare vertex shader program\n");
	}
	return GetShaderParams(
	    regs.es_regs.data_addr, "ShaderRecompiler VS", GetDeclaredShaderHash(regs.es_regs.data_addr),
	    std::span<const uint32_t>(regs.gs_user_sgpr.value, regs.gs_regs.rsrc2.user_sgpr), data);
}

ShaderParams PrepareProgram(
    const HW::PixelShaderInfo& regs, const HW::ShaderRegisters& sh,
    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
    ShaderPixelInputInfo&                               ps_info) {
	const auto data = ShaderGetMappedData(regs.ps_regs.data_addr, "ShaderGetInputInfoPS():");
	ShaderGetStaticInputInfoPS(regs, sh, target_export_mapping, data, ps_info);
	return GetShaderParams(
	    regs.ps_regs.data_addr, "ShaderRecompiler PS", GetDeclaredShaderHash(regs.ps_regs.data_addr),
	    std::span<const uint32_t>(regs.ps_user_sgpr.value, regs.ps_regs.rsrc2.user_sgpr), data);
}

ShaderParams PrepareProgram(const HW::ComputeShaderInfo& regs, const HW::ShaderRegisters& sh,
                            ShaderComputeInputInfo& info) {
	const auto data = ShaderGetMappedData(regs.cs_regs.data_addr, "ShaderGetInputInfoCS():");
	ShaderGetStaticInputInfoCS(regs, sh, data, info);
	return GetShaderParams(
	    regs.cs_regs.data_addr, "ShaderRecompiler CS", GetDeclaredShaderHash(regs.cs_regs.data_addr),
	    std::span<const uint32_t>(regs.cs_user_sgpr.value, regs.cs_regs.user_sgpr), data);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ShaderDbgDumpInputInfo(const ShaderVertexInputInfo& info) {
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Vs)");

	LOGF("ShaderDbgDumpInputInfo()\n");

	LOGF("\t fetch_external = %s\n"
	     "\t fetch_embedded = %s\n",
	     info.fetch_external ? "true" : "false", info.fetch_embedded ? "true" : "false");

	for (int i = 0; i < info.resources_num; i++) {
		LOGF("\t input %d\n", i);

		const auto& r  = info.resources[i];
		const auto& rd = info.resources_dst[i];

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
		     r.RawFormat(), r.OutOfBounds());
		LOGF("\t\t AddTid()         = %s\n", r.AddTid() ? "true" : "false");
	}

	for (int i = 0; i < info.buffers_num; i++) {
		LOGF("\t buffer %d\n", i);

		const auto& r = info.buffers[i];
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

void ShaderDbgDumpInputInfo(const ShaderPixelInputInfo& info) {
	KYTY_PROFILER_BLOCK("ShaderDbgDumpInputInfo(Ps)");

	LOGF("ShaderDbgDumpInputInfo()\n");

	LOGF("\t input_num            = %u\n"
	     "\t ps_system_input_base = %u\n"
	     "\t custom_interpolation_mask = 0x%08" PRIx32 "\n"
	     "\t ps_perspective_center_vgpr = %" PRIu32 "\n"
	     "\t ps_pos_x             = %s\n"
	     "\t ps_pos_y             = %s\n"
	     "\t ps_pos_z             = %s\n"
	     "\t ps_pos_w             = %s\n"
	     "\t ps_front_face        = %s\n"
	     "\t ps_sample_shading    = %s\n"
	     "\t ps_no_perspective    = %s\n"
	     "\t ps_pixel_kill_enable = %s\n"
	     "\t ps_early_z           = %s\n"
	     "\t ps_execute_on_noop   = %s\n",
	     info.input_num, info.ps_system_input_base, info.custom_interpolation_mask,
	     info.ps_perspective_center_vgpr, info.ps_pos_x ? "true" : "false",
	     info.ps_pos_y ? "true" : "false", info.ps_pos_z ? "true" : "false",
	     info.ps_pos_w ? "true" : "false", info.ps_front_face ? "true" : "false",
	     info.ps_sample_shading ? "true" : "false", info.ps_no_perspective ? "true" : "false",
	     info.ps_pixel_kill_enable ? "true" : "false", info.ps_early_z ? "true" : "false",
	     info.ps_execute_on_noop ? "true" : "false");

	for (uint32_t i = 0; i < info.input_num; i++) {
		LOGF("\t interpolator_settings[%u] = %u\n", i, info.interpolator_settings[i]);
	}
}

void ShaderDbgDumpInputInfo(const ShaderComputeInputInfo& info) {
	LOGF("ShaderDbgDumpInputInfo()\n");

	LOGF("\t workgroup_register = %d\n"
	     "\t thread_ids_num     = %d\n"
	     "\t wave_size          = %u\n"
	     "\t lds_size_dwords    = %u\n"
	     "\t needs_lds_barriers = %s\n"
	     "\t threads_num        = {%u, %u, %u}\n"
	     "\t tg_size_en         = %s\n",
	     info.workgroup_register, info.thread_ids_num, info.wave_size, info.lds_size_dwords,
	     info.needs_lds_barriers ? "true" : "false",
	     info.threads_num[0], info.threads_num[1], info.threads_num[2],
	     info.tg_size_en ? "true" : "false");
	LOGF("\t threadgroup_id     = {%s, %s, %s}\n", info.group_id[0] ? "true" : "false",
	     info.group_id[1] ? "true" : "false", info.group_id[2] ? "true" : "false");
}

bool ShaderAddressValid(uint64_t addr) {
	return reinterpret_cast<const uint32_t*>(addr) != nullptr;
}

} // namespace Libs::Graphics
