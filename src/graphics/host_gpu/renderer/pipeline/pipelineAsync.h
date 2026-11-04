#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEASYNC_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEASYNC_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include <array>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

// The state --async-shader-compile adds to PipelineCache. Only pipelineCache.cpp and
// pipelineAsync.cpp include this.
namespace Libs::Graphics {

// Threads that run queued jobs in the order they were posted, urgent ones first.
class ShaderWorkers {
public:
	enum class Priority {
		// Translating a shader nothing has started on yet.
		Normal,
		// Finishing a shader that is already translated (module, pipeline). It makes a draw
		// drawable, so it does not wait behind the translations of other shaders.
		Urgent,
	};

	explicit ShaderWorkers(uint32_t threads);
	~ShaderWorkers() { Stop(); }
	KYTY_CLASS_NO_COPY(ShaderWorkers);

	// Ignored once stopped.
	void Post(Common::UniqueFunction<void>&& job, Priority priority = Priority::Normal);
	// Drops the jobs that have not started and waits for the ones that have.
	void Stop();

private:
	std::mutex                               m_mutex;
	std::condition_variable_any              m_ready;
	std::queue<Common::UniqueFunction<void>> m_jobs;
	std::queue<Common::UniqueFunction<void>> m_urgent_jobs;
	bool                                     m_stopped = false;
	// Declared last, so the threads exist only once everything they use does.
	std::vector<std::jthread> m_threads;
};

struct PipelineCache::StageInputs {
	ProgramKey                                                key;
	std::variant<ShaderVertexInputInfo, ShaderPixelInputInfo> info;
	std::vector<uint32_t>                                     code;
	std::vector<uint32_t>                                     back_code;
	std::array<uint32_t, 40>                                  user_data {};
	ShaderRecompiler::Spirv::Profile                          profile;
	// Points into the members above, so a StageInputs is never copied or moved.
	ShaderRecompiler::CompileOptions                          options;

	StageInputs()                              = default;
	StageInputs(const StageInputs&)            = delete;
	StageInputs& operator=(const StageInputs&) = delete;
};

struct PipelineCache::Async {
	explicit Async(uint32_t threads): workers(threads) {}

	// A translation waiting for a draw to name the resources it is compiled for.
	struct Translation {
		std::shared_ptr<StageInputs>                       inputs;
		std::unique_ptr<ShaderRecompiler::TranslateResult> translated;
	};

	// A stage compiled on a worker; frees the host module unless the cache took it.
	struct PendingStage {
		vk::Device    device;
		CompiledStage stage;
		bool          taken = false;

		PendingStage(vk::Device device, CompiledStage&& stage)
		    : device(device), stage(std::move(stage)) {}
		~PendingStage() {
			if (!taken && stage.module.module) {
				device.destroyShaderModule(stage.module.module, nullptr);
			}
		}
		PendingStage(const PendingStage&)            = delete;
		PendingStage& operator=(const PendingStage&) = delete;
	};

	// What a worker finished, to be applied by the render thread.
	void Finish(Common::UniqueFunction<void>&& apply) {
		std::scoped_lock lock(done_mutex);
		done.push_back(std::move(apply));
	}

	ShaderWorkers                             workers;
	std::mutex                                done_mutex;
	std::vector<Common::UniqueFunction<void>> done;
	// Render thread only: the ids of the translations, compilations and pipelines being made.
	std::unordered_set<uint64_t>                                 in_flight;
	std::unordered_map<ProgramKey, Translation, ProgramKeyHash> translations;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEASYNC_H_
