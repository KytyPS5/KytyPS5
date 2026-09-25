#include "graphics/host_gpu/renderer/pipeline/pipelineCompileProgress.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Check(bool value, const char* text) {
    if (!value) {
        std::printf("FAILED: %s\n", text);
        std::fflush(stdout);
        std::abort();
    }
}

namespace Progress = Libs::Graphics::PipelineCompileProgress;

using Progress::OverlayCompletedOfTotal;
using Progress::OverlayRevision;
using Progress::Snapshot;

// The panel is drawn into a presented frame and only repainted when the revision changes, so
// every state the overlay renders differently has to carry a different revision than the one
// before it. Replays the whole precompile phase and checks that holds at each step.
void ReplayPhaseChangesRevisionEveryStep() {
    constexpr uint64_t Total = 20;

    Snapshot progress {};
    progress.estimated_total = Total;
    progress.compiling       = true;

    uint64_t presented = OverlayRevision(progress);

    for (uint64_t i = 0; i < Total; i++) {
        progress.pending++; // ReportCompileStarted
        const uint64_t started = OverlayRevision(progress);
        Check(started != presented, "queueing a compile must change the revision");
        presented = started;

        progress.compiled++; // ReportCompileFinished
        progress.pending--;
        const uint64_t finished = OverlayRevision(progress);
        Check(finished != presented, "finishing a compile must change the revision");
        presented = finished;
    }

    Check(progress.compiled == Total, "every compile is accounted for");
    Check(progress.pending == 0, "nothing is left in flight");

    // The regression: compiled + pending alone is invariant across a finish, so the phase
    // clearing here would leave the revision matching the presented one and the card would stay
    // painted over the first frames of the game.
    progress.compiling = false; // SetPrecompiling(false)
    Check(OverlayRevision(progress) != presented,
          "leaving the compile phase must change the revision");
}

// A cold run records rather than replays: there is no estimate and the panel shows progress with
// no denominator, but the same repaint rule applies.
void ColdRunWithoutAnEstimateBehavesTheSame() {
    Snapshot progress {};
    progress.compiling = true;

    const uint64_t idle = OverlayRevision(progress);

    progress.pending++;
    progress.compiled++;
    progress.pending--;
    const uint64_t compiled = OverlayRevision(progress);
    Check(compiled != idle, "a compile on a cold run must change the revision");

    progress.compiling = false;
    Check(OverlayRevision(progress) != compiled,
          "leaving the compile phase must change the revision");
}

// The panel shows a denominator whenever there is an estimate, so the count it displays has to
// stay inside that estimate no matter how the run lands against it.
void CompletedCountNeverPassesTheEstimate() {
    Snapshot progress {};
    progress.estimated_total = 20;

    progress.compiled = 19;
    Check(OverlayCompletedOfTotal(progress) == 19, "mid-run the real count is shown");

    progress.compiled = 20;
    Check(OverlayCompletedOfTotal(progress) == 20, "the last record reads as complete");

    // A run that needs more shaders than the last one still reads as complete rather than
    // overflowing the denominator or underflowing the remaining-time subtraction.
    progress.compiled = 25;
    Check(OverlayCompletedOfTotal(progress) == 20, "an overshoot is clamped to the estimate");

    progress.estimated_total = 0;
    Check(OverlayCompletedOfTotal(progress) == 0, "no estimate leaves nothing to show against");
}

} // namespace

int main() {
    ReplayPhaseChangesRevisionEveryStep();
    ColdRunWithoutAnEstimateBehavesTheSame();
    CompletedCountNeverPassesTheEstimate();
    std::printf("OverlayRevisionTests: OK\n");
    return 0;
}
