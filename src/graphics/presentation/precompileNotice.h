#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_PRECOMPILENOTICE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_PRECOMPILENOTICE_H_

#include "graphics/host_gpu/vulkanCommon.h"

#include <cstdint>

union SDL_Event;

namespace Libs::Graphics {

// What the game is doing while the notice shows.
enum class PrecompileNoticeState : uint32_t {
	// Stopped, and the player is asked whether to wait for the pipelines or to play at once.
	Question,
	// Stopped until the pipelines are ready (the player waits, or did not answer).
	Paused,
	// Not stopped for them, but its GPU work waits for pipelines: nothing to answer.
	Waiting,
	// The pipelines are ready and the game goes on; the notice stays a moment, in case it stops
	// again for more.
	Ready,
	// The game runs, and the driver is compiling a pipeline that takes long (or did a moment ago):
	// nothing to answer, and no progress to show.
	Compiling,
};

// The notice in the bottom-right corner of the window while the game waits for the pipelines
// PipelineCache has the driver compile (--pipeline-precompile): one notice whatever made the game
// wait, whose progress counts from when it appeared.
struct PrecompileNotice {
	bool                  visible = false;
	PrecompileNoticeState state   = PrecompileNoticeState::Paused;
	// Pipelines compiled since the notice appeared, of `done` and those known to be left.
	uint32_t done     = 0;
	uint32_t total    = 0;
	// Shaders still to translate, whose pipelines are not counted yet.
	uint32_t shaders  = 0;
	// Waiting: the pipelines the game's GPU work waits for, and for how long it has waited.
	// Compiling: the slow pipelines being compiled now, and the longest of them so far.
	uint32_t waited_pipelines = 0;
	uint32_t seconds_waited   = 0;
};

enum class PrecompileAnswer : uint32_t { None, Wait, Play };

void ShowPrecompileNotice(const PrecompileNotice& notice);
void HidePrecompileNotice();
// The player's answer since the last call: Cross or Enter waits, Circle or Esc plays at once.
[[nodiscard]] PrecompileAnswer TakePrecompileAnswer() noexcept;

[[nodiscard]] bool     PrecompileNoticeVisible() noexcept;
// Changes whenever the notice needs a repaint.
[[nodiscard]] uint64_t PrecompileNoticeRevision() noexcept;

// The window thread: takes the keys and buttons that answer while the notice is shown (none while
// the game waits).
bool ProcessPrecompileNoticeInput(const SDL_Event& event);

// Must run between ImGui::NewFrame() and ImGui::Render(), on the present thread.
void DrawPrecompileNotice(vk::Extent2D extent);

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_PRECOMPILENOTICE_H_
