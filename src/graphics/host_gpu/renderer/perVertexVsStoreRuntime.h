#pragma once

// Renderer side of the VS-store path (the math and the shader side are in perVertexVsStore.h and
// the SPIR-V emitter). On a device that cannot run PerVertexKHR, the VS and the per-vertex pixel
// shader of a pair are emitted again as VS-store variants when the guest shaders are compiled,
// paired into pipeline programs on first use, and the cap regions of the draws come from slabs that
// are recycled only after the GPU has finished them.

#include "graphics/host_gpu/renderer/cache/streamBuffer.h"
#include "graphics/host_gpu/renderer/perVertexVsStore.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics {
class CommandScheduler;
struct GraphicContext;
namespace ShaderRecompiler {
struct CompileResult;
struct CompileOptions;
} // namespace ShaderRecompiler

// Decides once, when the device is created, whether the VS-store path is used (see
// PvVsStoreEnabledFor) and logs why.
bool               ResolvePvVsStoreEnabled(bool fragment_shader_barycentric, bool moltenvk);
[[nodiscard]] bool PvVsStoreEnabled();

// Compile-time hook (next to the shader cache): emits the VS-store variant of a vertex shader and
// of a pixel shader with per-vertex inputs. No-op unless the device uses the VS-store path.
void RememberPvVsStoreShader(const ShaderProgram& program, ShaderRecompiler::CompileResult& result,
                             const ShaderRecompiler::CompileOptions& options,
                             uint32_t                                push_data_start_dword);

struct PvVsStorePrograms {
	PvVsStoreInterfaceVerdict       verdict     = PvVsStoreInterfaceVerdict::NoPerVertex;
	uint32_t                        stride_vec4 = 0;
	PipelineCache::GraphicsPrograms graphics; // valid when verdict == Use
};

// The VS-store programs of a VS/PS pair; nullptr when the pair is not a candidate at all (device
// does not use the path, PS without per-vertex input, a variant failed to emit or validate, more
// stages).
const PvVsStorePrograms* GetPvVsStorePrograms(GraphicContext&                        graphics,
                                              const PipelineCache::GraphicsPrograms& programs);

// Set 1 of the VS-store pipelines: binding 0 = cap (VS write, PS read), binding 1 = guest indices
// (PS).
vk::DescriptorSetLayout PvVsStoreSetLayout(GraphicContext& graphics);

// Push constant offset of the VS-store block (it follows the native push data).
uint32_t PvVsStorePushOffset();

// Draw counters, logged every kPvVsStoreStatsInterval candidate draws. Index = PvVsStoreFallback;
// index 0 (None) counts the draws that took the path.
struct PvVsStoreStats {
	std::array<std::atomic<uint64_t>, 8> by_fallback {};
	std::atomic<uint64_t>                instanced_draws {};
	std::atomic<uint64_t>                scanned_indices {};
	std::atomic<uint64_t>                records {};
};
PvVsStoreStats& PvVsStoreStatsRef();
void            CountPvVsStoreDraw(PvVsStoreFallback fallback);

// Cap slabs of one render executor. A region is valid until the command buffer it was placed in has
// finished on the GPU; a slab is reused only then (PvVsStoreSlabs).
class PvVsStoreRuntime {
public:
	PvVsStoreRuntime(GraphicContext& graphics, CommandScheduler& scheduler);
	~PvVsStoreRuntime();
	PvVsStoreRuntime(const PvVsStoreRuntime&)            = delete;
	PvVsStoreRuntime& operator=(const PvVsStoreRuntime&) = delete;

	struct Region {
		bool       ok          = false;
		uint32_t   slab        = 0;
		uint32_t   base_record = 0; // first record (stride_vec4 vec4s each) of the region
		vk::Buffer buffer      = nullptr;
		uint64_t   buffer_size = 0;
	};
	// `records` records of `stride_vec4` vec4s, for the command buffer that is open now.
	Region Place(uint64_t records, uint32_t stride_vec4);
	// Call when the draw is recorded (after resource preparation, which may flush the scheduler).
	void Touch(const Region& region);

private:
	GraphicContext&                      m_graphics;
	CommandScheduler&                    m_scheduler;
	PvVsStoreSlabs                       m_slabs;
	std::vector<std::unique_ptr<Buffer>> m_buffers;
};

// A draw that was accepted for the VS-store path (decided before any resource of the draw is
// prepared).
struct PvVsStoreDrawSetup {
	const PvVsStorePrograms* programs = nullptr;
	PvVsStoreDraw            draw;
	uint32_t                 index_element_size = 0;
	PvVsStoreRuntime::Region region;
};

} // namespace Libs::Graphics
