#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECOMPILEPROGRESS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECOMPILEPROGRESS_H_

#include <cstdint>
#include <string>

namespace Libs::Graphics::PipelineCompileProgress {

// Cross-cutting host-side signal, same shape as libs/ime.h and libs/dialog.h's own
// GetHostSnapshot free functions: PipelineCache reports into it, SystemOverlay reads it, with
// no direct reference between the two needed either way.

struct Snapshot {
	uint64_t    compiled           = 0;     // finished this run
	uint64_t    pending            = 0;     // started but not yet finished
	uint64_t    estimated_total    = 0;     // records queued for replay; 0 when there are none
	bool        compiling          = false; // the precompile phase is running
	uint64_t    compile_elapsed_ms = 0;     // since the first compile of this run
	std::string title;                      // from the SFO, empty until the window knows it
};

void ReportCompileStarted();
void ReportCompileFinished();
void SetEstimatedTotal(uint64_t total);
void SetTitleName(std::string name);

// True for exactly the span the precompile worker runs. The guest is held at its first shader
// lookup until it clears, so the panel covers one well-defined phase: it cannot overlap
// gameplay and cannot flicker between compile bursts the way a rate or activity test would.
void SetPrecompiling(bool active);

Snapshot GetSnapshot();

inline bool ShouldShowOverlay(const Snapshot& progress) {
	return progress.compiling;
}

// The overlay only repaints when its revision changes. A finished compile moves one unit from
// pending to compiled, so their plain sum is invariant across it; weighting compiled makes every
// report a change, and folding in the phase makes leaving it one too, so the last panel frame is
// always superseded.
inline uint64_t OverlayRevision(const Snapshot& progress) {
	return progress.compiled * 2 + progress.pending + (ShouldShowOverlay(progress) ? 1 : 0);
}

// The numerator the panel shows against estimated_total. The estimate comes from the previous
// run, so this one can overshoot it; clamping keeps the last record at "n / n" instead of
// tipping the panel back to its no-estimate form for the final frames.
inline uint64_t OverlayCompletedOfTotal(const Snapshot& progress) {
	return progress.compiled < progress.estimated_total ? progress.compiled
	                                                    : progress.estimated_total;
}

} // namespace Libs::Graphics::PipelineCompileProgress

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_PIPELINECOMPILEPROGRESS_H_
