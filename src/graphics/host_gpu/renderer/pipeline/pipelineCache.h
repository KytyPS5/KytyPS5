#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/fileLock.h"
#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "graphics/host_gpu/pipelineUse.h"
#include "graphics/host_gpu/renderer/pipeline/programList.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/shader.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <type_traits>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;
struct RenderColorInfo;
struct RenderDepthInfo;
class CommandBuffer;

namespace HW {
class Context;
class Shader;
class UserConfig;
struct ComputeShaderInfo;
} // namespace HW

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
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
static_assert(sizeof(PipelineStaticParameters) == 116);

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
	// An emulated mesh program: `module` is its compute shader, and this draws what it wrote.
	vk::ShaderModule mesh_vertex_module = nullptr;
	uint32_t         mesh_slot_words    = 0;

	explicit operator bool() const { return id != 0 && module != nullptr; }
};

// The owning renderer serializes access, including saves while the GPU is running.
class PipelineCache {
public:
	explicit PipelineCache(GraphicContext& graphics);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	void Save();
	// For the window title. Safe to read without the renderer's lock.
	[[nodiscard]] const AheadBuildProgress& BuiltAhead() const { return m_built_ahead; }

	struct Pipeline {
		vk::PipelineLayout      pipeline_layout       = nullptr;
		vk::Pipeline            pipeline              = nullptr;
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		bool                    uses_push_descriptors = false;
		// The stages of the one push constant range of the layout. Every push to the layout names
		// exactly these stages (VUID-vkCmdPushConstants-offset-01796).
		vk::ShaderStageFlags push_constant_stages;
		// An emulated mesh draw first runs this compute pipeline, with its own layout over the same
		// descriptor set layout, and `pipeline` then draws what it wrote.
		vk::Pipeline       mesh_compute        = nullptr;
		vk::PipelineLayout mesh_compute_layout = nullptr;
		uint32_t           mesh_slot_words     = 0;
	};

	struct GraphicsPrograms {
		std::array<ShaderProgram, 3> vertex;
		ShaderProgram pixel;

		[[nodiscard]] uint32_t VertexStageCount() const { return vertex[1] ? 3u : 1u; }
	};

	GraphicsPrograms
	GetGraphicsPrograms(const HW::VertexShaderInfo& vertex_regs,
	                    const HW::PixelShaderInfo& pixel_regs, const HW::ShaderRegisters& sh,
	                    const HW::Context& context, const HW::UserConfig& user_config,
	                    std::span<const ShaderColorTarget, 8> color_targets,
	                    bool pixel_active, std::array<ShaderVertexInputInfo, 3>& vertex_info,
	                    ShaderPixelInputInfo& pixel_info);
	ShaderProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                const HW::ShaderRegisters&   sh,
	                                ShaderComputeInputInfo&      input_info);

	// A pipeline that is not known yet is built on a worker thread. Null when it is still being
	// built and the draw is left out for now; see PlanPipelineUse for which draws wait instead.
	// `frame`: the game's frame the draw belongs to, for the budget of waits (FrameWaitBudget).
	Pipeline* GetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              const GraphicsPrograms& programs, const DrawEffects& effects,
	                              uint64_t frame);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info,
	                             const ShaderProgram&          compute_program);

private:
	struct ProgramCache;

	struct GraphicsPipelineKey {
		PipelineRenderingState   rendering;
		std::array<uint64_t, 3>  vertex_shader_ids {};
		uint64_t                 ps_shader_id = 0;
		PipelineVertexInputState vertex_input;
		PipelineStaticParameters static_params;

		bool operator==(const GraphicsPipelineKey& other) const {
			return rendering == other.rendering && vertex_shader_ids == other.vertex_shader_ids &&
			       ps_shader_id == other.ps_shader_id && vertex_input == other.vertex_input &&
			       static_params == other.static_params;
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
	AheadBuildProgress            m_built_ahead;
	std::unique_ptr<ProgramCache> m_program_cache;
	vk::PipelineCache             m_driver_cache = nullptr;
	// One for each worker thread, when the driver builds the pipelines of a cache one at a time
	// (DriverFaults::pipeline_cache_serializes_builds).
	std::vector<vk::PipelineCache> m_builder_caches;
	std::filesystem::path         m_driver_cache_path;
	// Save wrote the driver cache: the destructor does not write it again.
	bool                          m_driver_cache_saved = false;
	// A pipeline and whether it is built yet: false while a worker thread is still building it,
	// and the pipeline is read only once it is true.
	struct PipelineEntry {
		std::atomic<bool> ready {false};
		Pipeline          pipeline;
		// The frame of the last short wait for this pipeline (GraceWaitUs). GPU thread only.
		uint64_t          grace_wait_frame = UINT64_MAX;
	};
	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<PipelineEntry>, GraphicsPipelineKeyHash>
	                                                             m_graphics_pipelines;
	std::unordered_map<uint64_t, std::unique_ptr<PipelineEntry>> m_compute_pipelines;

	// What a worker needs to build one pipeline, copied from the draw or dispatch. A compute
	// job has `compute` set and no vertex stages.
	struct PipelineJob {
		PipelineEntry*                       target = nullptr;
		bool                                 listed = false; // from the game's list, built ahead
		std::optional<ShaderComputeInputInfo> compute;
		PipelineRenderingState               rendering;
		PipelineVertexInputState             vertex_input;
		std::array<ShaderVertexInputInfo, 3> vertex_info;
		uint32_t                             vertex_count = 0;
		std::optional<ShaderPixelInputInfo>  pixel;
		GraphicsPrograms                     programs;
		PipelineStaticParameters             static_params;
	};
	std::mutex                  m_jobs_mutex;
	std::condition_variable_any m_job_added;
	std::condition_variable     m_job_done;
	std::deque<PipelineJob>     m_jobs;
	std::vector<std::jthread>   m_builders;
	bool                        m_always_wait = false;
	// Counters for the log at exit.
	uint64_t m_left_out_draws = 0;
	// Held while this emulator keeps the game's lists (several copies share the folder).
	Common::FileLock m_list_lock;
	bool             m_lists_writable = false;
	// Draws that waited only because a target of theirs was reset in this frame (pipelineUse.h).
	uint64_t        m_reset_target_waits = 0;
	FrameWaitBudget m_reset_target_budget;
	FrameWaitBudget m_grace_wait_budget {GRACE_WAIT_BUDGET_US};
	uint64_t m_builds         = 0;
	uint64_t m_build_us       = 0; // under m_jobs_mutex
	uint64_t m_longest_us     = 0; // under m_jobs_mutex

	// The pipeline list: the pipelines a game used, on disk, so the next start builds them
	// before a draw asks. A record names its programs by their identity on the program list.
	std::vector<PipelineRecord>  m_listed_pipelines; // read at start, not built yet
	ListedPipelineScan           m_listed_scan;
	std::unordered_set<uint64_t> m_pipelines_on_list;
	std::FILE*                   m_pipeline_list = nullptr;

	void                    InitializeDriverCache();
	void OpenPipelineList(const std::filesystem::path& path, bool writable);
	void                    QueueListedPipelines();
	void                    RememberPipeline(const GraphicsPipelineKey& key, const PipelineJob& job);
	// A compute record: no stages, no pixel program; the program is first, the input info is
	// the fixed state.
	void                    RememberComputePipeline(const PipelineJob& job);
	void                    WriteRecord(const PipelineRecord& record);
	void                    QueueBuild(GraphicsPipelineKey key, PipelineJob job);
	void                    QueueComputeBuild(uint64_t program_id, PipelineJob job);
	void                    Enqueue(PipelineJob job);
	// A draw or dispatch waits for this build: it goes first.
	void                    Promote(const PipelineEntry& entry);
	void                    BuildPipelines(std::stop_token stop, uint32_t builder);
	[[nodiscard]] Pipeline* WhenReady(PipelineEntry& entry, const DrawEffects& effects,
	                                   uint64_t frame);
};

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache                      driver_cache);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
