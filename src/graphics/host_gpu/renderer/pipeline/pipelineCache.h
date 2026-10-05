#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/file.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/rectListShader.h"
#include "graphics/shader/shader.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

// Guest memory that a resolution ahead of execution read. The execution thread repeats the reads
// before it uses the resolution: guest memory may have changed in between (operations that
// execute before the draw write it), and ahead of execution GPU-owned bytes cannot be read.
class GuestReadLog {
public:
	void Clear() {
		m_entries.clear();
		m_bytes.clear();
		m_failed = false;
	}
	// Reads the backing store without faulting. Strict reads are checked the way specialization
	// memory is read (GPU-owned bytes fail), the others as plain loads.
	bool               Read(uint64_t address, void* data, uint64_t size, bool strict);
	// A plain load the SRT walker made.
	void               RecordLoad(uint64_t address, uint32_t value);
	void               Fail() { m_failed = true; }
	[[nodiscard]] bool Failed() const { return m_failed; }
	// Execution thread: whether every read still gives the recorded bytes.
	[[nodiscard]] bool StillValid() const;

private:
	static constexpr uint64_t StrictCheckBytes = 64;
	struct Entry {
		uint64_t address = 0;
		uint32_t size    = 0;
		uint32_t offset  = 0;
		bool     strict  = false;
	};
	void                 Append(uint64_t address, const void* data, uint32_t size, bool strict);
	std::vector<Entry>   m_entries;
	std::vector<uint8_t> m_bytes;
	bool                 m_failed = false;
};

// Storage of the shader programs a draw resolved ahead of its execution: the resource snapshots
// and specializations its stages point to, and the guest memory they read.
struct ProgramResolution {
	static constexpr uint32_t MaxStages = 4;

	std::array<ShaderRecompiler::IR::ResourceSnapshot, MaxStages>       resources;
	std::array<ShaderRecompiler::IR::ResourceSpecialization, MaxStages> specializations;
	GuestReadLog                                                        reads;
	uint32_t                                                            stages = 0;

	void Reset() {
		reads.Clear();
		stages = 0;
	}
};

struct GraphicContext;
struct ShaderParams;
class CommandBuffer;

namespace HW {
class UserConfig;
class Shader;
} // namespace HW

namespace ShaderRecompiler {
struct CompileOptions;
struct ShaderSource;
struct TranslateResult;
} // namespace ShaderRecompiler

namespace ShaderRecompiler::IR {
class ImageHeapSource;
} // namespace ShaderRecompiler::IR

namespace ShaderRecompiler::Spirv {
struct Profile;
} // namespace ShaderRecompiler::Spirv

#pragma pack(push, 1)

struct PipelineStaticParameters {
	bool                       negative_one_to_one      = false;
	bool                       depth_clip_enable        = true;
	vk::PrimitiveTopology      topology                 = vk::PrimitiveTopology::ePointList;
	// 0 without tessellation, 3 for a rectangle list, else the input control points.
	uint32_t                   patch_control_points     = 0;
	bool                       primitive_restart_enable = false;
	uint32_t                   samples                  = 1;
	bool                       sample_shading_enable    = false;
	bool                       alpha_to_coverage_enable = false;
	uint32_t                   color_mask[RENDER_COLOR_ATTACHMENTS_MAX]           = {};
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
};

struct PipelineRenderingState {
	std::array<vk::Format, RENDER_COLOR_ATTACHMENTS_MAX> color_formats {};
	vk::Format                                           depth_format   = vk::Format::eUndefined;
	vk::Format                                           stencil_format = vk::Format::eUndefined;
	uint32_t                                             color_count    = 0;
};

struct PipelineVertexInputState {
	struct Binding {
		uint32_t stride   = 0;
		uint8_t  instance = 0;
	};
	struct Attribute {
		uint32_t offset  = 0;
		uint8_t  binding = 0;
	};

	std::array<Binding, ShaderVertexInputInfo::RES_MAX>   bindings {};
	std::array<Attribute, ShaderVertexInputInfo::RES_MAX> attributes {};
	uint8_t                                               binding_count   = 0;
	uint8_t                                               attribute_count = 0;
};

// Everything a graphics pipeline is made of besides the stage modules its ids name. Aligned so
// that rendering.color_formats can be passed to Vulkan.
struct alignas(4) GraphicsPipelineKey {
	// VS, LS or mesh; HS; TES; PS. 0 marks an absent stage.
	std::array<uint64_t, 4>  stage_ids {};
	PipelineRenderingState   rendering;
	PipelineVertexInputState vertex_input;
	PipelineStaticParameters static_params;

	bool operator==(const GraphicsPipelineKey& other) const;
};

#pragma pack(pop)

// Keys are compared and hashed as bytes.
static_assert(std::has_unique_object_representations_v<GraphicsPipelineKey>);

// A compiled stage with what pipeline creation needs from it besides the key.
struct ShaderModule {
	uint64_t                                     id     = 0;
	vk::ShaderModule                             module = nullptr;
	ShaderRecompiler::IR::CompiledShaderInfo     info;
	ShaderRecompiler::IR::ResourceSpecialization specialization;
	std::vector<uint32_t>                        function_code;
	// VS or LS: the host format of each vertex input attribute.
	std::vector<vk::Format>                      vertex_formats;
	// Rectangle-list PS: the parameters it reads.
	std::vector<RectListParameter>               rect_parameters;
	// Indirect mesh draw: a task shader reads the records and launches the groups.
	bool                                         mesh_task = false;
};

// A draw's depth target and its first color_count color targets.
struct DrawTargets {
	RenderDepthInfo depth;
	RenderColorInfo colors[RENDER_COLOR_ATTACHMENTS_MAX];
	uint32_t        color_count = 0;
};

// A draw's topology and shader stages (from PipelineCache::GetGraphicsPrograms).
struct DrawRenderState {
	// Color targets the pixel program writes.
	uint32_t                             mrt_mask = 0;
	vk::PrimitiveTopology                topology = vk::PrimitiveTopology::ePointList;
	bool                                 primitive_restart_enable = false;
	bool                                 ps_active                = false;
	std::array<ShaderVertexInputInfo, 3> vertex_info;
	ShaderPixelInputInfo                 ps_input_info;
	// Input: the user-data slots a GPU-read indirect draw patches.
	ShaderIndirectOffsets                indirect_offsets;
	bool                                 indirect_draw = false;

	[[nodiscard]] std::span<const ShaderVertexInputInfo> VertexStages() const {
		return {vertex_info.data(), vertex_info[1].stage ? 3u : 1u};
	}
};

// Writes the shader store on its own thread, running jobs in the order they were posted.
class ShaderStoreWriter {
public:
	ShaderStoreWriter();
	// Keeps the first `kept` bytes of the store at `path`, or starts it over with `header`.
	void Open(const std::filesystem::path& path, std::span<const uint8_t> header, uint64_t kept);
	// Whether the store is open.
	explicit operator bool() const { return !m_file.IsInvalid(); }
	// Compresses `record` and appends it on the store thread, in call order.
	void Append(std::vector<uint8_t>&& record);
	void Post(Common::UniqueFunction<void>&& job);
	// Runs the queued jobs and stops the thread; jobs posted later never run. Safe to call again.
	void Finish() { m_thread = {}; }

private:
	friend struct PipelineCacheTestAccess;

	Common::File                             m_file;
	std::mutex                               m_mutex;
	std::condition_variable_any              m_ready;
	std::queue<Common::UniqueFunction<void>> m_jobs;
	// Declared last, so it drains the queue and joins before the rest is destroyed.
	std::jthread m_thread;
};

// The owning renderer serializes access, including saves while the GPU is running.
class PipelineCache {
public:
	// Without a shader store path nothing is kept across runs.
	PipelineCache(GraphicContext& graphics, ShaderRecompiler::IR::ImageHeapSource* image_heap,
	              const std::filesystem::path& shader_store = {});
	~PipelineCache();
	KYTY_CLASS_NO_COPY(PipelineCache);
	// Finishes the queued writes and stops the store thread, then writes the driver cache if it
	// changed.
	void Save();

	struct Pipeline {
		vk::PipelineLayout      pipeline_layout       = nullptr;
		vk::Pipeline            pipeline              = nullptr;
		vk::DescriptorSetLayout descriptor_set_layout = nullptr;
		// The push constant range's stages, which every update names.
		vk::ShaderStageFlags    push_stages;
		bool                    uses_push_descriptors = false;
		bool                    uses_image_heap       = false;
	};

	// Reads the shaders and registers bound in `command` into the state's shader stages.
	void GetGraphicsPrograms(const CommandBuffer& command, DrawRenderState& state);
	// The resolve thread: resolves the programs of a draw ahead of its execution, with programs
	// and modules that exist, into `ahead`, reading guest memory through its log. False when the
	// execution thread resolves the draw itself; programs it misses are queued to the workers.
	bool GetGraphicsProgramsAhead(const HW::Context& context, const HW::UserConfig& user_config,
	                              const HW::Shader& shaders, DrawRenderState& state,
	                              ProgramResolution& ahead);
	// Null skips the dispatch.
	const ShaderModule* GetComputeProgram(const HW::ComputeShaderInfo& regs,
	                                      ShaderComputeInputInfo&      input_info);
	// The resolve thread: as GetGraphicsProgramsAhead, for a dispatch; null when the execution
	// thread resolves it itself.
	const ShaderModule* GetComputeProgramAhead(const HW::ComputeShaderInfo& regs,
	                                           ShaderComputeInputInfo&      input_info,
	                                           ProgramResolution&           ahead);

	Pipeline& GetGraphicsPipeline(const CommandBuffer& command, const DrawRenderState& state,
	                              const DrawTargets& targets);
	Pipeline& GetComputePipeline(const ShaderComputeInputInfo& input_info);

private:
	friend struct PipelineCacheTestAccess;

	struct Program {
		std::unique_ptr<ShaderRecompiler::ShaderSource> call_source;
		ShaderRecompiler::IR::ResourcePlan           resource_plan;
		ShaderRecompiler::IR::ResourceSnapshot       resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		// A deque: resolutions ahead of execution keep pointers to modules while new ones are
		// added.
		std::deque<ShaderModule>                     modules;
		bool                                         skip_dispatch = false;

		[[nodiscard]] std::span<const uint32_t> FunctionCode() const;
		bool RefreshCallSource(const ShaderRecompiler::IR::SrtRuntime& runtime);
		template <bool HasCalls>
		ShaderModule* FindModule(const ShaderRecompiler::IR::SrtRuntime& runtime,
		                         uint32_t                                push_data_cursor);
	};

	struct ProgramKey {
		ShaderType            stage           = ShaderType::Unknown;
		uint64_t              hash            = 0;
		uint32_t              user_data_count = 0;
		std::vector<uint32_t> static_state;

		bool operator==(const ProgramKey&) const = default;
	};

	// Variants of one source share a bucket; ProgramKey equality compares the state.
	struct ProgramKeyHash {
		std::size_t operator()(const ProgramKey& key) const { return key.hash; }
	};

	struct GraphicsPipelineKeyHash {
		std::size_t operator()(const GraphicsPipelineKey& key) const;
	};

	using ModuleMap = std::unordered_map<uint64_t, const ShaderModule*>;

	GraphicContext&                        m_graphics;
	ShaderRecompiler::IR::ImageHeapSource* m_image_heap;
	std::unordered_map<ProgramKey, Program, ProgramKeyHash> m_program_cache;
	// Modules compiled per stage.
	std::array<uint32_t, static_cast<size_t>(ShaderType::TessellationEvaluation) + 1> m_compiled {};
	ProgramKey                             m_lookup_key;
	vk::PipelineCache             m_driver_cache = nullptr;
	std::filesystem::path         m_driver_cache_path;
	// Guards the driver cache state below, which the timer thread also reads.
	std::mutex                    m_driver_cache_mutex;
	std::chrono::steady_clock::time_point m_next_driver_cache_save;
	// When the store thread was last given the driver cache to write.
	std::chrono::steady_clock::time_point m_driver_cache_posted;
	bool                          m_driver_cache_dirty = false;
	std::unordered_map<GraphicsPipelineKey, Pipeline, GraphicsPipelineKeyHash> m_graphics_pipelines;
	std::unordered_map<uint64_t, Pipeline>                                     m_compute_pipelines;
	std::unordered_map<uint64_t, ShaderTessellationInputInfo>                  m_tess_layouts;
	// The pipeline the last lookup found; pipelines are never removed while the cache lives.
	GraphicsPipelineKey m_last_graphics_key {};
	Pipeline*           m_last_graphics_pipeline = nullptr;
	// Draws resolve their programs on the resolve thread and, when a resolution is not usable,
	// on the execution thread, which also resolves dispatches. Compile workers add programs.
	std::atomic_flag m_program_lock;
	ProgramKey       m_ahead_key;

	// Graphics programs compiled on worker threads: the resolve thread queues the programs and
	// modules its draws miss, so the shaders of a new area compile in parallel instead of one
	// after another on the execution thread. Workers only add programs; the execution thread
	// still resolves every draw itself and compiles what they did not add.
	struct CompileJob;
	static constexpr uint32_t CompileWorkers = 6;
	// Under the program lock: programs a worker is compiling, with their number of jobs.
	std::unordered_map<ProgramKey, uint32_t, ProgramKeyHash> m_in_flight;
	std::mutex                                               m_job_mutex;
	std::condition_variable                                  m_job_available;
	std::deque<std::unique_ptr<CompileJob>>                  m_jobs;
	bool                                                     m_stopping = false;
	std::vector<std::jthread>                                m_workers;

	// Records everything first created if a shader store is open, and writes the driver cache.
	ShaderStoreWriter m_store;
	// Checks every quarter second whether the driver cache is due; stopped by Save().
	std::jthread      m_driver_cache_timer;

	bool ResolveGraphicsPrograms(const HW::Context& context, const HW::UserConfig& user_config,
	                             const HW::Shader& shaders, DrawRenderState& state,
	                             ProgramResolution* ahead);
	template <typename InputInfo>
	const ShaderModule* GetProgram(const ShaderParams& params, InputInfo& input_info,
	                               uint32_t& push_data_cursor);
	template <typename InputInfo>
	const ShaderModule* GetAhead(const ShaderParams& params, InputInfo& input_info,
	                             uint32_t& push_data_cursor, ProgramResolution& ahead);
	ShaderModule& CompileModule(Program& program, const ProgramKey& key,
	                            ShaderRecompiler::CompileOptions  options,
	                            ShaderRecompiler::TranslateResult translated,
	                            uint32_t                          push_data_cursor);
	// Compiles a module without touching the cache; `spirv` receives its code.
	ShaderModule BuildModule(const ProgramKey& key, const ShaderRecompiler::CompileOptions& options,
	                         ShaderRecompiler::TranslateResult                   translated,
	                         const ShaderRecompiler::IR::ResourceSpecialization& specialization,
	                         std::span<const uint32_t> function_code, uint32_t push_data_cursor,
	                         std::vector<uint32_t>& spirv) const;
	// Under the program lock: counts a module added to `program` and records it in the store.
	void AddedModule(const ProgramKey& key, const Program& program, const ShaderModule& module,
	                 const std::vector<uint32_t>& spirv);
	void LockPrograms();
	void UnlockPrograms();
	// Under the program lock (resolve thread): queues the program of m_ahead_key, or one of its
	// modules, to the compile workers.
	template <typename InputInfo>
	void QueueCompile(const ShaderParams& params, const InputInfo& input_info,
	                  uint32_t                                            push_data_cursor,
	                  const ShaderRecompiler::IR::ResourceSpecialization* specialization);
	// Execution thread, under the program lock: waits without the lock until no worker compiles
	// the program of `key`. False when none did.
	bool WaitForCompile(const ProgramKey& key);
	template <typename InputInfo>
	void RunCompileJob(CompileJob& job, InputInfo& input_info);
	void CompileWorker();
	void StopWorkers();
	void InitializeDriverCache();
	// Marks the driver cache changed and, once per interval, has the store thread write it; sooner
	// after a slow creation.
	void CheckpointDriverCache(bool slow_creation = false);
	// Checkpoints the driver cache after a pipeline whose creation began at `started`.
	void NoteCreatedPipeline(std::chrono::steady_clock::time_point started);
	// Has the store thread write the driver cache if it changed and is due. The caller holds
	// m_driver_cache_mutex.
	void PostDueDriverCache();
	void WriteDriverCache() const;

	// pipelineSerialization.cpp
	void WarmUp(const std::filesystem::path& path);
	bool LoadRecord(std::span<const uint8_t>& records);
	void StoreModule(const ProgramKey& key, const Program& program, const ShaderModule& module,
	                 const std::vector<uint32_t>& spirv);
	void StorePipeline(const GraphicsPipelineKey& key);
	void StoreLayout(uint64_t key, const ShaderTessellationInputInfo& layout);
	// Every module by id, with id 0 naming an absent stage.
	[[nodiscard]] ModuleMap MapModules() const;
	// False when a stage id of the key is not in `modules`.
	bool CreateGraphicsPipeline(Pipeline& pipeline, const GraphicsPipelineKey& key,
	                            const ModuleMap& modules);
};

// The shader store of the running title; empty unless --shader-cache is on in a Release build
// without debug dumps, which change the SPIR-V.
std::filesystem::path ShaderStorePath();
// Host capabilities that change the emitted SPIR-V.
ShaderRecompiler::Spirv::Profile SpirvProfile(const GraphicContext& graphics);

// Stages are indexed like GraphicsPipelineKey::stage_ids.
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const GraphicsPipelineKey&                 key,
                            const std::array<const ShaderModule*, 4>& stages,
                            vk::PipelineCache                          driver_cache);
void CreatePipelineInternal(GraphicContext& graphics, PipelineCache::Pipeline& pipeline,
                            const ShaderModule& module, vk::PipelineCache driver_cache);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINECACHE_H_
