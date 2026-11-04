#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEASYNC_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEASYNC_H_

#include "common/common.h"
#include "common/uniqueFunction.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// The state --async-shader-compile adds to PipelineCache. Only pipelineCache.cpp and
// pipelineAsync.cpp include this.
namespace Libs::Graphics {

// Milliseconds on a steady clock: when a draw last asked for a job.
[[nodiscard]] inline uint64_t AskClockMs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

// Threads that run queued jobs at below-normal priority, so that the game's own threads go first.
// Urgent jobs go before normal ones. Within each kind a worker takes the job a draw asked for last,
// the scene on screen, instead of the oldest: first-come-first-served let a pipeline drawn every
// frame starve behind the new ones each frame brought. Worker 0 takes the oldest, so that a job
// that was asked for once is not left behind for good. Jobs no draw has asked for in StaleAfterMs
// are taken by the first BackgroundWorkers workers only, so that work nobody sees any more does
// not take the cores the game uses. (The scheduling follows usa-ratt/KytyPS5-GT7's pipeline
// compiler.)
class ShaderWorkers {
public:
	enum class Priority {
		// Translating a shader nothing has started on yet.
		Normal,
		// Finishing a shader that is already translated (module, pipeline). It makes a draw
		// drawable, so it does not wait behind the translations of other shaders.
		Urgent,
	};

	// A queued job; a draw that wants it again calls Touch.
	struct Job {
		Common::UniqueFunction<void> run;
		Priority                     priority = Priority::Normal;
		uint64_t                     sequence = 0;
		std::atomic<uint64_t>        asked {0};

		void Touch() noexcept { asked.store(AskClockMs(), std::memory_order_relaxed); }
	};
	using JobPtr = std::shared_ptr<Job>;

	static constexpr uint32_t BackgroundWorkers = 2;
	static constexpr uint64_t StaleAfterMs      = 2000;

	explicit ShaderWorkers(uint32_t threads);
	~ShaderWorkers() { Stop(); }
	KYTY_CLASS_NO_COPY(ShaderWorkers);

	// Null once stopped.
	JobPtr Post(Common::UniqueFunction<void>&& job, Priority priority = Priority::Normal);
	// The jobs that have not started.
	[[nodiscard]] size_t Queued();
	// Drops the jobs that have not started and waits for the ones that have.
	void Stop();

private:
	// Under m_mutex: whether worker `index` may take a job now, and takes it.
	[[nodiscard]] bool   CanPick(uint32_t index) const;
	[[nodiscard]] JobPtr Pick(uint32_t index);

	std::mutex                  m_mutex;
	std::condition_variable_any m_ready;
	std::vector<JobPtr>         m_jobs;
	std::vector<JobPtr>         m_urgent_jobs;
	uint64_t                    m_next_sequence = 0;
	bool                        m_stopped       = false;
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
		// When the translation finished, for the status line.
		uint64_t since = 0;
	};

	// A job a worker has or will run for a program, and what it does.
	struct Flight {
		ShaderWorkers::JobPtr job;
		bool                  translating = false;
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
	// Render thread only: the translations and compilations being made, by id.
	std::unordered_map<uint64_t, Flight>                         in_flight;
	std::unordered_map<ProgramKey, Translation, ProgramKeyHash> translations;
	// Render thread only: pipelines being created on workers.
	uint32_t pipelines_building = 0;

	// Render thread only: every two seconds while anything is pending, one line on the console
	// with what the workers are doing and how many draws were skipped meanwhile.
	void     ReportStatus();
	uint64_t last_report_ms  = 0;
	uint64_t reported_skipped = 0;
	bool     was_busy         = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINEASYNC_H_
