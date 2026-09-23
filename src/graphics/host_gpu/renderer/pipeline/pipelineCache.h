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

// The raw read the descriptor evaluator runs for every word of a descriptor chain. The address is
// guest data and may point anywhere, so an unmapped one refuses rather than faulting the caller.

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	bool                       stencil_test_enable      = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX] = {};
	bool                       cull_front                               = false;
	bool                       cull_back                                = false;
	bool                       face                                     = false;
	bool                       provoking_vtx_last                       = false;
	vk::PolygonMode            polygon_mode                             = vk::PolygonMode::eFill;
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
static_assert(sizeof(PipelineStaticParameters) == 149);

// A graphics pipeline is built as four libraries and fast-linked wherever the device supports it,
// falling back to the monolithic path otherwise. Linking costs 0.01 ms against a 75.86 ms
// monolithic build (note 155), and measured against this title it moves the median from 14 to 33
// fps. Link-time optimization is deliberately not used: it costs 3.25 ms per link for a gain this
// renderer has never needed.

// What a fragment-shader pipeline library is keyed on, and nothing more.
//
// This is the Vulkan "fragment shader state" subset and it is deliberately narrow: every field left
// out is one that would split a pixel shader across more libraries than it needs and give back the
// reuse this exists for. What is *in* it:
//
//   - the pixel shader permutation id, which is the module, and which also fixes the set 1
//     descriptor layout the library is built against (a permutation's bindings are what that
//     layout is made of);
//   - the multisample state the subset carries;
//   - the depth/stencil state, less the depth bounds, which are dynamic;
//   - the depth and stencil attachment formats, which VkPipelineRenderingCreateInfo puts in this
//     subset.
//
// What is out, and why it is safe: the vertex shader and anything about it (that is the whole point
// of the descriptor split - with independent sets this library's layout names only set 1); blend,
// colour masks and colour formats (fragment *output* state, its own library, measured at 0.01 ms);
// topology and vertex input (0.00 ms); cull, polygon mode and viewport (pre-rasterization). The
// guest shader hash is out too: two permutations of one shader are two modules.
//
// One field is in it that does not look like fragment state at all: `uses_push_descriptors`. This
// renderer decides pushing once for a whole pipeline, over the descriptor counts of *both* sets
// together, and then stamps that decision into the set 1 layout as ePushDescriptorKHR. So the same
// pixel shader paired with a heavy vertex shader and with a light one wants two different set 1
// layouts, and a library built against one of them is not compatible with the other. It is the last
// of the layout couplings note 157 catalogued, and the only one the descriptor split did not
// remove. Keying on it is correct and costs only the reuse between those two pairings; removing it
// would mean deciding pushing per set, which changes the draw path and is its own change.
//
// Compared by value in full, so a near-match is a miss. That is the point - a library reused under
// a key that omitted, say, the sample count would draw at the wrong sample rate and validate
// cleanly, which is the failure mode this codebase is worst at attributing.
struct FragmentLibraryKey {
	// 0 for a depth-only pipeline, whose fragment subset has no shader stage.
	uint64_t ps_shader_id = 0;
	// The layout's push-constant stages, which every library of one pipeline must agree on.
	uint32_t                   push_stages           = 0;
	uint32_t                   samples               = 1;
	bool                       sample_shading_enable = false;
	bool                       stencil_test_enable   = false;
	PipelineStencilStaticState stencil_front;
	PipelineStencilStaticState stencil_back;
	vk::Format                 depth_format   = vk::Format::eUndefined;
	vk::Format                 stencil_format = vk::Format::eUndefined;

	bool operator==(const FragmentLibraryKey& other) const noexcept;
};

struct FragmentLibraryKeyHash {
	std::size_t operator()(const FragmentLibraryKey& key) const noexcept;
};

// The pre-rasterization subset: the vertex, tessellation or mesh stages and the rasterizer state.
// The rect-list tessellation stages are generated from the pixel shader's inputs, so a rect-list
// library also keys on the pixel shader.
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
	// Carried for diagnostics only: the guest shader hash and the SPIR-V handed to the driver.
	uint64_t hash        = 0;
	uint32_t spirv_words = 0;

	explicit operator bool() const { return id != 0 && module != nullptr; }
};

// The fragment-shader pipeline libraries a run has built, keyed on the fragment subset alone.
//
// This is the object note 156 predicted would pay: 95.7 % of a graphics pipeline's build cost is
// its fragment library (note 155), and 34-47 % of the fragment libraries a run needs were estimated
// to be repeats. An entry owns everything the library was built against, because a library outlives
// the pipeline that first needed it:
//
//   - the library VkPipeline itself;
//   - the set-1 VkDescriptorSetLayout the pixel stage binds through; and
//   - the INDEPENDENT_SETS VkPipelineLayout naming only that set.
//
// The layout is the cache's own rather than the borrowed one of whichever pipeline built it, so
// entry lifetime does not depend on a pipeline's. Vulkan defines layout compatibility by content
// and not by handle, so a pipeline links this library against its own two-set layout regardless.
//
// The same class caches pre-rasterization libraries, whose entry owns the set-0 layout instead.
template <typename Key, typename KeyHash>
class PipelineLibraryCache {
public:
	PipelineLibraryCache(GraphicContext& graphics, const char* name)
	    : m_graphics(graphics), m_name(name) {}
	~PipelineLibraryCache();
	KYTY_CLASS_NO_COPY(PipelineLibraryCache);

	struct Entry {
		vk::Pipeline            library    = nullptr;
		vk::PipelineLayout      layout     = nullptr;
		vk::DescriptorSetLayout set_layout = nullptr;
	};

	// The library for `key`, or null if none has been built yet. Counts a hit.
	[[nodiscard]] const Entry* Find(const Key& key);

	// Publishes `entry` under `key` and returns what callers should use.
	//
	// Losing a race is handled by destroying the loser rather than by holding a build gate: a
	// duplicate library build is rare by construction and gating would serialise unrelated work.
	const Entry* Insert(const Key& key, const Entry& entry);

	// hits / (hits + misses). This is what turns note 156's estimated 34-47 % reuse into a number
	// measured on a real run.
	void LogHitRate() const;

private:
	void MaybeLogHitRateLocked() const;

	GraphicContext&                                          m_graphics;
	const char*                                              m_name;
	std::unordered_map<Key, std::unique_ptr<Entry>, KeyHash> m_entries;
	mutable Common::Mutex                                    m_mutex;
	uint64_t                                                 m_hits            = 0;
	uint64_t                                                 m_misses          = 0;
	mutable uint64_t                                         m_reported        = 0;
	mutable uint64_t                                         m_reported_hits   = 0;
	mutable uint64_t                                         m_reported_misses = 0;
};

using FragmentLibraryCache  = PipelineLibraryCache<FragmentLibraryKey, FragmentLibraryKeyHash>;
using PreRasterLibraryCache = PipelineLibraryCache<PreRasterLibraryKey, PreRasterLibraryKeyHash>;

struct PipelineLibraries {
	explicit PipelineLibraries(GraphicContext& graphics)
	    : fragment(graphics, "FragmentLibraries"), pre_raster(graphics, "PreRasterLibraries") {}

	FragmentLibraryCache  fragment;
	PreRasterLibraryCache pre_raster;
};

// The owning renderer serializes access, including saves while the GPU is running.
class PipelineCache {
public:
	explicit PipelineCache(GraphicContext& graphics);
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	void Save();

	// Null before InitializeDriverCache and after Save(), which Vulkan accepts as "no cache".
	[[nodiscard]] vk::PipelineCache DriverCache() const { return m_driver_cache; }

	struct Pipeline {
		vk::PipelineLayout pipeline_layout = nullptr;
		vk::Pipeline       pipeline        = nullptr;
		// The pipeline libraries this pipeline linked and owns, when it was built through the
		// library path; null on the monolithic path. The pre-rasterization and fragment libraries
		// are deliberately not here: they are shared, owned by PipelineLibraries, and outlive any
		// one pipeline.
		//
		// They are kept rather than destroyed right after linking because the specification does
		// not clearly say a linked pipeline may outlive its libraries. Destroyed with the pipeline.
		vk::Pipeline library_vertex_input    = nullptr;
		vk::Pipeline library_fragment_output = nullptr;
		// Set 0: the vertex, mesh or compute stage. Set 1: the pixel stage, null when there is no
		// pixel stage (a depth-only draw, or any compute pipeline).
		vk::DescriptorSetLayout descriptor_set_layout       = nullptr;
		vk::DescriptorSetLayout pixel_descriptor_set_layout = nullptr;
		// Whether set 0 is pushed rather than allocated. Set 1 never is: Vulkan allows one
		// push-descriptor set layout per pipeline layout.
		bool uses_push_descriptors = false;
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
	                    ShaderPixelInputInfo& pixel_info);
	ShaderProgram GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                const HW::ShaderRegisters&   sh,
	                                ShaderComputeInputInfo&      input_info);

	Pipeline& GetGraphicsPipeline(std::span<const RenderColorInfo>       colors,
	                              const RenderDepthInfo&                 depth,
	                              std::span<const ShaderVertexInputInfo> vertex_info,
	                              CommandBuffer& command, const ShaderPixelInputInfo* ps_input_info,
	                              vk::PrimitiveTopology topology, bool primitive_restart_enable,
	                              const GraphicsPrograms& programs);
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
	std::unique_ptr<ProgramCache> m_program_cache;
	// Null unless the library path is switched on, which is what CreatePipelineInternal branches
	// on.
	std::unique_ptr<PipelineLibraries> m_libraries;
	vk::PipelineCache                  m_driver_cache = nullptr;
	std::filesystem::path              m_driver_cache_path;
	// Identity of the running binary; 0 when it could not be fingerprinted.
	uint64_t m_build_hash = 0;
	std::unordered_map<GraphicsPipelineKey, std::unique_ptr<Pipeline>, GraphicsPipelineKeyHash>
	                                                        m_graphics_pipelines;
	std::unordered_map<uint64_t, std::unique_ptr<Pipeline>> m_compute_pipelines;
	std::chrono::steady_clock::time_point m_last_save = std::chrono::steady_clock::now();

	// This emulator dies by device loss, abort or a closed window far more often than it exits
	// cleanly, so the cache is written while the run is still alive.
	static constexpr std::chrono::seconds DriverCacheSavePeriod {20};

	static constexpr std::chrono::seconds CfgCacheFlushPeriod {20};
	std::chrono::steady_clock::time_point m_last_cfg_flush = std::chrono::steady_clock::now();
	// The recompiler's miss count as of the last flush; unchanged means the file already holds
	// everything this session has.
	uint64_t m_cfg_flushed_misses = 0;
	bool     m_cfg_cache_enabled  = false;

	void InitializeDriverCache();
	void InitializeCfgCache(const std::filesystem::path& folder, const std::string& title_id);
	bool WriteDriverCacheLocked();
	void MaybeSaveDriverCacheLocked();
	void FlushCfgCacheIfDirtyLocked();
	void MaybeFlushCfgCacheLocked();
	void DestroyPipelineObjects(const Pipeline& pipeline);
};

void LogPipelineTrace(const char* phase, uint64_t vertex_program_id, uint64_t pixel_program_id);
// `libraries` null builds the monolithic pipeline this renderer has always built. Non-null links
// every graphics pipeline from pipeline libraries.
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const PipelineRenderingState&          rendering,
                            const PipelineVertexInputState&        vertex_input,
                            std::span<const ShaderVertexInputInfo> vertex_info,
                            const ShaderPixelInputInfo*            ps_input_info,
                            const PipelineCache::GraphicsPrograms& programs,
                            const PipelineStaticParameters&        static_params,
                            vk::PipelineCache driver_cache, PipelineLibraries* libraries = nullptr);

void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderComputeInputInfo& input_info,
                            vk::ShaderModule compute_module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
