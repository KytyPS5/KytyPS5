#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace Libs::Graphics {

struct RenderColorInfo;
struct RenderDepthInfo;
struct VulkanFramebuffer;

namespace HW {
class Context;
class Shader;
struct ComputeShaderInfo;
} // namespace HW

#pragma pack(push, 1)

struct PipelineStaticParameters {
	float                      viewport_scale[3]        = {};
	float                      viewport_offset[3]       = {};
	bool                       negative_one_to_one      = false;
	int                        scissor_ltrb[4]          = {0};
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	bool                       with_depth               = false;
	bool                       depth_test_enable        = false;
	bool                       depth_write_enable       = false;
	vk::CompareOp              depth_compare_op         = vk::CompareOp::eNever;
	bool                       depth_bounds_test_enable = false;
	float                      depth_min_bounds         = 0.0f;
	float                      depth_max_bounds         = 0.0f;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	uint32_t                   color_count                                        = 1;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
	bool                       cull_front                                         = false;
	bool                       cull_back                                          = false;
	bool                       face                                               = false;
	uint8_t                    color_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	uint8_t                    alpha_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	bool                       separate_alpha_blend[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       blend_enable[RENDER_COLOR_ATTACHMENTS_MAX]         = {};
	bool                       blend_bypass[RENDER_COLOR_ATTACHMENTS_MAX]         = {};
	float                      blend_color_red                                    = 0.0f;
	float                      blend_color_green                                  = 0.0f;
	float                      blend_color_blue                                   = 0.0f;
	float                      blend_color_alpha                                  = 0.0f;

	bool operator==(const PipelineStaticParameters& other) const noexcept;
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PipelineStaticParameters>);
static_assert(std::is_standard_layout_v<PipelineStaticParameters>);
static_assert(alignof(PipelineStaticParameters) == 1);
static_assert(sizeof(PipelineStaticParameters) ==
              sizeof(float[3]) + sizeof(float[3]) + sizeof(bool) + sizeof(int[4]) +
                  sizeof(vk::PrimitiveTopology) + sizeof(bool) * 3 + sizeof(vk::CompareOp) +
                  sizeof(bool) + sizeof(float) * 2 + sizeof(bool) +
                  sizeof(PipelineStencilStaticState) * 2 + sizeof(uint32_t) +
                  sizeof(uint32_t[RENDER_COLOR_ATTACHMENTS_MAX]) + sizeof(bool) * 3 +
                  sizeof(uint8_t[RENDER_COLOR_ATTACHMENTS_MAX]) * 6 +
                  sizeof(bool[RENDER_COLOR_ATTACHMENTS_MAX]) * 3 + sizeof(float) * 4);

class PipelineCache {
public:
	PipelineCache() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	~PipelineCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(PipelineCache);

	struct Pipeline {
		vk::PipelineLayout pipeline_layout = nullptr;
		vk::Pipeline       pipeline        = nullptr;
	};

	struct GraphicsPipeline: Pipeline {
		uint64_t render_pass_id = 0;
		ShaderId vs_shader_id;
		ShaderId ps_shader_id;
	};

	struct ComputePipeline: Pipeline {
		ShaderId cs_shader_id;
	};

	GraphicsPipeline* CreateGraphicsPipeline(
	    VulkanFramebuffer* framebuffer, RenderColorInfo* colors, uint32_t color_count,
	    RenderDepthInfo* depth, ShaderVertexInputInfo* vs_input_info, HW::Context* ctx,
	    HW::Shader* sh_ctx, ShaderPixelInputInfo* ps_input_info, vk::PrimitiveTopology topology,
	    bool ps_active, std::span<const uint32_t> vs_spirv, std::span<const uint32_t> ps_spirv);
	ComputePipeline* CreateComputePipeline(ShaderComputeInputInfo*      input_info,
	                                       const HW::ComputeShaderInfo* cs_regs,
	                                       std::span<const uint32_t>    cs_spirv);

private:
	struct GraphicsPipelineKey {
		uint64_t                 render_pass_id = 0;
		ShaderId                 vs_shader_id;
		ShaderId                 ps_shader_id;
		PipelineStaticParameters static_params;

		bool operator==(const GraphicsPipelineKey& other) const {
			return render_pass_id == other.render_pass_id && vs_shader_id == other.vs_shader_id &&
			       ps_shader_id == other.ps_shader_id && static_params == other.static_params;
		}
	};

	struct ComputePipelineKey {
		ShaderId cs_shader_id;

		bool operator==(const ComputePipelineKey& other) const {
			return cs_shader_id == other.cs_shader_id;
		}
	};

	struct PipelineKeyHash {
		static void Mix(std::size_t* hash, std::size_t value) {
			*hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (*hash << 6u) +
			         (*hash >> 2u);
		}

		static void MixShaderId(std::size_t* hash, const ShaderId& id) {
			Mix(hash, id.hash0);
			Mix(hash, id.crc32);
			Mix(hash, id.ids.size());
			for (auto value: id.ids) {
				Mix(hash, value);
			}
		}

		static void MixStaticParams(std::size_t* hash, const PipelineStaticParameters& params) {
			const auto* bytes = reinterpret_cast<const uint8_t*>(&params);
			for (std::size_t i = 0; i < sizeof(params); i++) {
				Mix(hash, bytes[i]);
			}
		}
	};

	struct GraphicsPipelineKeyHash {
		std::size_t operator()(const GraphicsPipelineKey& key) const {
			std::size_t hash = 0;
			PipelineKeyHash::Mix(&hash, key.render_pass_id);
			PipelineKeyHash::MixShaderId(&hash, key.vs_shader_id);
			PipelineKeyHash::MixShaderId(&hash, key.ps_shader_id);
			PipelineKeyHash::MixStaticParams(&hash, key.static_params);
			return hash;
		}
	};

	struct ComputePipelineKeyHash {
		std::size_t operator()(const ComputePipelineKey& key) const {
			std::size_t hash = 0;
			PipelineKeyHash::MixShaderId(&hash, key.cs_shader_id);
			return hash;
		}
	};

	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>,
	                   GraphicsPipelineKeyHash>
	    m_graphics_pipelines;
	std::unordered_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>, ComputePipelineKeyHash>
	              m_compute_pipelines;
	Common::Mutex m_mutex;
};

void LogPipelineTrace(const char* phase, uint32_t vs_hash0, uint32_t vs_crc32, uint32_t ps_hash0,
                      uint32_t ps_crc32);
void CreatePipelineInternal(PipelineCache::GraphicsPipeline* pipeline, vk::RenderPass render_pass,
                            const ShaderVertexInputInfo*    vs_input_info,
                            std::span<const uint32_t>       vs_shader,
                            const ShaderPixelInputInfo*     ps_input_info,
                            std::span<const uint32_t>       ps_shader,
                            const PipelineStaticParameters& static_params, uint32_t vs_hash0,
                            uint32_t vs_crc32, uint32_t ps_hash0, uint32_t ps_crc32,
                            bool ps_active);
void CreatePipelineInternal(PipelineCache::ComputePipeline* pipeline,
                            const ShaderComputeInputInfo*   input_info,
                            std::span<const uint32_t>       cs_shader);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
