#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEHELPERS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEHELPERS_H_

#include "common/emulatorConfig.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace Libs::Graphics {

struct GraphicContext;

// Compute pipelines compiled in other processes. Cold, Demon's Souls needs thousands of compute
// pipelines from its pipeline manifest, ~1 s of NVIDIA's compiler each. In the emulator's process
// the threads that create them run off the cores of the pinned GPU threads (its default CPU sets)
// and below the game's own busy threads, which leave them about two cores: the game reached its
// world after 821 s, and then stalled for seconds on pipelines. Helpers run at the priority of the
// emulator's threads on all of its processors, the second threads of the reserved cores included:
// ~6 pipelines/s, the world after 463 s, and no stall over 0.2 s there.
//
// A helper is this executable started with --pipeline-helper: it creates a device like the
// emulator's (CreateHeadlessVulkan) and, for each module it is sent, the pipeline in an empty
// pipeline cache, whose data it sends back. Creating the pipeline from that data takes the
// emulator ~2 ms. Each helper has a driver disk cache of its own, removed when it exits, and exits
// after a number of pipelines: once its disk cache holds many, NVIDIA's driver may compress the
// binaries with a dictionary kept there, which the emulator does not have. Windows only.
class PipelineHelpers {
public:
	struct Result {
		uint64_t             ticket = 0;
		bool                 ok     = false;
		// The pipeline cache data that holds the pipeline.
		std::vector<uint8_t> cache_data;
	};
	// Called on a helper's reader thread, for every queued job that is not withdrawn: with ok
	// false when the helper failed or exited without it.
	using ResultCallback = std::function<void(Result&&)>;

	// Null without helpers (`processes` 0, or another platform than Windows).
	static std::unique_ptr<PipelineHelpers> Start(const GraphicContext& graphics, uint32_t processes,
	                                              ResultCallback on_result);
	~PipelineHelpers();
	KYTY_CLASS_NO_COPY(PipelineHelpers);

	// Queues the compute pipeline of `module` (EncodeComputeModule()); jobs are sent in order,
	// urgent ones (a dispatch will want them soon) before the others.
	void Queue(uint64_t ticket, std::vector<uint8_t>&& module, bool urgent);
	// Takes back a job no helper was sent yet; false when there is none (sent, or done).
	bool Withdraw(uint64_t ticket);
	// Writes the counts to the log.
	void LogProgress() const;

	struct Impl;

private:
	explicit PipelineHelpers(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> m_impl;
};

// How many helpers the emulator starts: KYTY_PIPELINE_HELPERS=<n> (0 turns them off), else, with
// NVIDIA's driver, two fewer than half the logical processors (10 of 24); none with other drivers.
[[nodiscard]] uint32_t PipelineHelperProcesses(const GraphicContext& graphics);

// The helper process: --pipeline-helper <spec>, then the emulator's own options (`config`, for
// the device). Returns the exit code.
int RunPipelineHelper(std::string_view spec, Config::ConfigOptions config);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINEHELPERS_H_
