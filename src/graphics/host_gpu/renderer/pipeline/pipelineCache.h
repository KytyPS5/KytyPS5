#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/threads.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <type_traits>
#include <unordered_map>

namespace Libs::Graphics {

struct GraphicContext;
struct RenderColorInfo;
struct RenderDepthInfo;
struct ShaderParams;
class CommandBuffer;

namespace HW {
class Context;
class Shader;
class UserConfig;
struct ComputeShaderInfo;
} // namespace HW

namespace ShaderRecompiler::IR {
class ImageHeapSource;
} // namespace ShaderRecompiler::IR

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	bool                       alpha_to_coverage_enable = false;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
	bool                       cull_front                                         = false;
	bool                       cull_back                                          = false;
	bool                       face                                               = false;
	bool                       provoking_vtx_last                                 = false;
	vk::PolygonMode            polygon_mode                                       = vk::PolygonMode::eFill;
	uint8_t                    color_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    color_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	uint8_t                    alpha_srcblend[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_comb_fcn[RENDER_COLOR_ATTACHMENTS_MAX]       = {};
	uint8_t                    alpha_destblend[RENDER_COLOR_ATTACHMENTS_MAX]      = {};
	bool                       separate_alpha_blend[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       blend_enable[RENDER_COLOR_ATTACHMENTS_MAX]         = {};

	bool operator==(const PipelineStaticParameters& other) const noexcept;
};

#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<PipelineStaticParameters>);
static_assert(std::is_standard_layout_v<PipelineStaticParameters>);
static_assert(alignof(PipelineStaticParameters) == 1);
static_assert(sizeof(PipelineStaticParameters) == 150);

struct FragmentLibraryKey {
	// 0 for a depth-only pipeline, whose fragment subset has no shader stage.
	uint64_t ps_shader_id = 0;
	// The layout's push-constant stages, which every library of one pipeline must agree on.
	uint32_t                   push_stages              = 0;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	bool                       alpha_to_coverage_enable = false;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	vk::Format                 depth_format   = vk::Format::eUndefined;
	vk::Format                 stencil_format = vk::Format::eUndefined;

	bool operator==(const FragmentLibraryKey& other) const noexcept;
};

struct FragmentLibraryKeyHash {
	std::size_t operator()(const FragmentLibraryKey& key) const noexcept;
};

// Rect-list tessellation stages are generated from the pixel shader's inputs.
struct PreRasterLibraryKey {
	std::array<uint64_t, 3> vertex_shader_ids {};
	uint64_t                rect_list_ps_shader_id = 0;
	uint32_t                push_stages            = 0;
	uint32_t                patch_control_points   = 0;
	bool                    rect_list              = false;
	bool                    negative_one_to_one    = false;
	bool                    depth_clip_enable      = true;
	bool                    cull_front             = false;
	bool                    cull_back              = false;
	bool                    face                   = false;
	bool                    provoking_vtx_last     = false;
	vk::PolygonMode         polygon_mode           = vk::PolygonMode::eFill;

	bool operator==(const PreRasterLibraryKey& other) const noexcept = default;
};

struct PreRasterLibraryKeyHash {
	std::size_t operator()(const PreRasterLibraryKey& key) const noexcept;
};

struct PipelineRenderingState {
	std::array<vk::Format, RENDER_COLOR_ATTACHMENTS_MAX> color_formats {};
	vk::Format                                           depth_format   = vk::Format::eUndefined;
	vk::Format                                           stencil_format = vk::Format::eUndefined;
	uint32_t                                             color_count    = 0;

	bool operator==(const PipelineRenderingState&) const = default;
};

struct PipelineVertexInputState {
	struct Binding {
		uint32_t stride                           = 0;
		bool     instance                         = false;
		bool     operator==(const Binding&) const = default;
	};
	struct Attribute {
		uint32_t offset                             = 0;
		uint8_t  binding                            = 0;
		bool     operator==(const Attribute&) const = default;
	};

	std::array<Binding, ShaderVertexInputInfo::RES_MAX>   bindings {};
	std::array<Attribute, ShaderVertexInputInfo::RES_MAX> attributes {};
	uint8_t                                               binding_count   = 0;
	uint8_t                                               attribute_count = 0;

	bool operator==(const PipelineVertexInputState&) const = default;
};

struct ShaderProgram {
	uint64_t         id     = 0;
	vk::ShaderModule module = nullptr;

	explicit operator bool() const { return id != 0 && module != nullptr; }
};

template <typename Key, typename KeyHash>
class PipelineLibraryCache {
public:
	explicit PipelineLibraryCache(GraphicContext& graphics): m_graphics(graphics) {}
	~PipelineLibraryCache();
	KYTY_CLASS_NO_COPY(PipelineLibraryCache);

	struct Entry {
		vk::Pipeline            library    = nullptr;
		vk::PipelineLayout      layout     = nullptr;
		vk::DescriptorSetLayout set_layout = nullptr;
	};

	[[nodiscard]] const Entry* Find(const Key& key);
	// A race loser's entry is destroyed; returns the published one.
	const Entry* Insert(const Key& key, const Entry& entry);

private:
	GraphicContext&                                          m_graphics;
	std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> m_entries;
	Common::Mutex                                            m_mutex;
};

using FragmentLibraryCache  = PipelineLibraryCache<FragmentLibraryKey, FragmentLibraryKeyHash>;
using PreRasterLibraryCache = PipelineLibraryCache<PreRasterLibraryKey, PreRasterLibraryKeyHash>;

struct PipelineLibraries {
	explicit PipelineLibraries(GraphicContext& graphics)
	    : fragment(graphics), pre_raster(graphics) {}

	FragmentLibraryCache  fragment;
	PreRasterLibraryCache pre_raster;
};

// The owning renderer serializes access, including saves while the GPU is running.
class PipelineCache {
public:
	PipelineCache(GraphicContext& graphics, ShaderRecompiler::IR::ImageHeapSource* image_heap);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	void Save();

	// Null when the driver cache is disabled, which Vulkan accepts as "no cache".
	[[nodiscard]] vk::PipelineCache DriverCache() const { return m_driver_cache; }

	struct Pipeline {
		vk::PipelineLayout pipeline_layout = nullptr;
		vk::Pipeline       pipeline        = nullptr;
		// Owned by this pipeline; the shared pre-raster/fragment libraries live in
		// PipelineLibraries.
		vk::Pipeline library_vertex_input    = nullptr;
		vk::Pipeline library_fragment_output = nullptr;
		// Set 0: vertex, mesh or compute stage. Set 1: image heap. Set 2: pixel stage.
		vk::DescriptorSetLayout descriptor_set_layout       = nullptr;
		vk::DescriptorSetLayout pixel_descriptor_set_layout = nullptr;
		// Only set 0 can be pushed: Vulkan allows one push-descriptor set layout per pipeline
		// layout.
		bool     uses_push_descriptors = false;
		bool     uses_image_heap       = false;
		uint64_t layout_signature      = 0;
		bool     task_front            = false;
	};

	struct GraphicsPrograms {
		std::array<ShaderProgram, 3>      vertex;
		ShaderProgram                     pixel;
		std::array<ShaderProgram, 8>      stencil_bit_pixel;
		std::array<ShaderStageRuntime, 8> stencil_bit_stage;
		uint8_t                           stencil_bit_mask = 0;

		[[nodiscard]] uint32_t VertexStageCount() const { return vertex[1] ? 3u : 1u; }
	};

	GraphicsPrograms
	GetGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                    const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                    const HW::Context& context, const HW::UserConfig& user_config,
	                    std::span<const Prospero::ColorComponentMapping, 8> target_export_mapping,
	                    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                    ShaderPixelInputInfo&         pixel_info,
	                    const std::array<uint8_t, 3>* payload_user_sgprs = nullptr);
	ShaderProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                const HW::ShaderRegisters&   sh,
	                                ShaderComputeInputInfo&      input_info);

	Pipeline& GetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              const GraphicsPrograms& programs, bool task_verify = false,
	                              bool task_indexed = false);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info,
	                             const ShaderProgram&          compute_program);

private:
	struct ProgramCache;

	void GetStencilBitPrograms(const ShaderParams&         pixel_params,
	                           const ShaderPixelInputInfo& pixel_info, uint32_t pixel_cursor,
	                           uint32_t next_cursor, uint8_t bits, GraphicsPrograms& result);

	struct GraphicsPipelineKey {
		PipelineRenderingState   rendering;
		std::array<uint64_t, 3>  vertex_shader_ids {};
		uint64_t                 ps_shader_id = 0;
		PipelineVertexInputState vertex_input;
		PipelineStaticParameters static_params;
		bool                     task_verify  = false;
		bool                     task_indexed = false;

		bool operator==(const GraphicsPipelineKey& other) const {
			return rendering == other.rendering && vertex_shader_ids == other.vertex_shader_ids &&
			       ps_shader_id == other.ps_shader_id && vertex_input == other.vertex_input &&
			       static_params == other.static_params && task_verify == other.task_verify &&
			       task_indexed == other.task_indexed;
		}
	};

	struct PipelineKeyHash {
		static void Mix(std::size_t& hash, std::size_t value) {
			hash ^= value + static_cast<std::size_t>(0x9e3779b97f4a7c15ull) + (hash << 6u) +
			        (hash >> 2u);
		}
	};

	struct GraphicsPipelineKeyHash {
		std::size_t operator()(const GraphicsPipelineKey& key) const;
	};

	GraphicContext&               m_graphics;
	std::unique_ptr<ProgramCache> m_program_cache;
	std::unique_ptr<PipelineLibraries>    m_libraries;
	vk::PipelineCache                     m_driver_cache = nullptr;
	std::filesystem::path                 m_driver_cache_path;
	std::chrono::steady_clock::time_point m_next_driver_cache_save;
	bool                                  m_driver_cache_dirty = false;
	uint64_t                              m_build_hash         = 0;
	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<Pipeline>, GraphicsPipelineKeyHash>
	                                                        m_graphics_pipelines;
	std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> m_compute_pipelines;

	static constexpr std::chrono::seconds CfgCacheFlushPeriod {20};
	std::chrono::steady_clock::time_point m_last_cfg_flush     = std::chrono::steady_clock::now();
	uint64_t                              m_cfg_flushed_misses = 0;
	bool                                  m_cfg_cache_enabled  = false;

	void InitializeDriverCache();
	void CheckpointDriverCache();
	void InitializeCfgCache(const std::filesystem::path& folder, const std::string& title_id);
	void FlushCfgCacheIfDirtyLocked();
	void MaybeFlushCfgCacheLocked();
	void DestroyPipelineObjects(const Pipeline& pipeline);
};

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id);
// Null `libraries` builds a monolithic pipeline.
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache driver_cache, PipelineLibraries* libraries = nullptr,
                            bool task_verify = false, bool task_indexed = false);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
