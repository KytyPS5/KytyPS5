#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/shader/shader.h"

#include <cstddef>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan_core.h>

namespace Libs::Graphics {

struct VulkanFramebuffer;

namespace HW {
class Context;
class Shader;
struct ComputeShaderInfo;
} // namespace HW

class PipelineCache {
public:
	PipelineCache() { EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread()); }
	virtual ~PipelineCache() { KYTY_NOT_IMPLEMENTED; }
	KYTY_CLASS_NO_COPY(PipelineCache);

	struct Pipeline {
		VkPipelineLayout pipeline_layout = nullptr;
		VkPipeline       pipeline        = nullptr;
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
	    HW::Shader* sh_ctx, ShaderPixelInputInfo* ps_input_info, VkPrimitiveTopology topology,
	    bool ps_active, std::span<const uint32_t> vs_spirv, std::span<const uint32_t> ps_spirv);
	ComputePipeline* CreateComputePipeline(ShaderComputeInputInfo*      input_info,
	                                       const HW::ComputeShaderInfo* cs_regs,
	                                       std::span<const uint32_t>    cs_spirv);
	void             DeletePipeline(Pipeline* pipeline);
	void             DeleteAllPipelines();

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

	void DeletePipelineInternal(Pipeline* p);
	void DumpToFile(Common::File* f, const Pipeline& p);
	void DumpPipeline(const char* action, const Pipeline& p);

	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<GraphicsPipeline>,
	                   GraphicsPipelineKeyHash>
	    m_graphics_pipelines;
	std::unordered_map<ComputePipelineKey, std::unique_ptr<ComputePipeline>, ComputePipelineKeyHash>
	              m_compute_pipelines;
	Common::Mutex m_mutex;
};

void LogPipelineTrace(const char* phase, uint32_t vs_hash0, uint32_t vs_crc32, uint32_t ps_hash0,
                      uint32_t ps_crc32);
void CreatePipelineInternal(PipelineCache::GraphicsPipeline* pipeline, VkRenderPass render_pass,
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
