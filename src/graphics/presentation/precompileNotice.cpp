#include "graphics/presentation/precompileNotice.h"

#include "imgui.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <fmt/format.h>
#include <mutex>
#include <string>

namespace Libs::Graphics {

namespace {

std::mutex                g_mutex;
PrecompileNotice          g_notice;
std::atomic<bool>         g_visible {false};
std::atomic<uint32_t>     g_state {static_cast<uint32_t>(PrecompileNoticeState::Paused)};
std::atomic<uint64_t>     g_revision {0};
std::atomic<uint32_t>     g_answer {static_cast<uint32_t>(PrecompileAnswer::None)};
// The manifest's bar: no lock, the present thread reads it on every frame the game presents.
std::atomic<bool>         g_bar_visible {false};
std::atomic<uint64_t>     g_bar_progress {0}; // done << 32 | total
std::atomic<uint64_t>     g_bar_left {0};     // pipelines << 32 | shaders

uint64_t Pack(uint32_t high, uint32_t low) {
	return (static_cast<uint64_t>(high) << 32u) | low;
}

void Answer(PrecompileAnswer answer) {
	g_answer.store(static_cast<uint32_t>(answer), std::memory_order_release);
}

// The button's symbol, in a square of `size` at the cursor, as a line of text would take it.
void ButtonIcon(bool cross, float size) {
	const ImVec2 at = ImGui::GetCursorScreenPos();
	ImGui::Dummy({size, size});
	auto*        draw   = ImGui::GetWindowDrawList();
	const ImVec2 center = {at.x + size * 0.5f, at.y + size * 0.5f};
	const float  radius = size * 0.5f;
	draw->AddCircleFilled(center, radius, IM_COL32(40, 42, 48, 255));
	const float r = radius * 0.5f;
	if (cross) {
		const ImU32 color = IM_COL32(140, 170, 255, 255);
		draw->AddLine({center.x - r, center.y - r}, {center.x + r, center.y + r}, color, size * 0.1f);
		draw->AddLine({center.x + r, center.y - r}, {center.x - r, center.y + r}, color, size * 0.1f);
	} else {
		draw->AddCircle(center, r * 1.1f, IM_COL32(255, 110, 110, 255), 0, size * 0.1f);
	}
}

void Hint(bool cross, const char* key, const char* action, float size) {
	ButtonIcon(cross, size);
	ImGui::SameLine();
	ImGui::TextDisabled("%s", key);
	ImGui::SameLine();
	ImGui::TextUnformatted(action);
}

std::string TimeWaited(uint32_t seconds) {
	if (seconds < 60) {
		return fmt::format("waiting for {} s", seconds);
	}
	return fmt::format("waiting for {} min {} s", seconds / 60, seconds % 60);
}

} // namespace

void ShowPrecompileNotice(const PrecompileNotice& notice) {
	{
		std::scoped_lock lock(g_mutex);
		g_notice         = notice;
		g_notice.visible = true;
	}
	g_state.store(static_cast<uint32_t>(notice.state), std::memory_order_release);
	g_visible.store(true, std::memory_order_release);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

void HidePrecompileNotice() {
	if (g_visible.exchange(false, std::memory_order_acq_rel)) {
		g_revision.fetch_add(1, std::memory_order_acq_rel);
	}
}

PrecompileAnswer TakePrecompileAnswer() noexcept {
	return static_cast<PrecompileAnswer>(
	    g_answer.exchange(static_cast<uint32_t>(PrecompileAnswer::None), std::memory_order_acq_rel));
}

bool PrecompileNoticeVisible() noexcept {
	return g_visible.load(std::memory_order_acquire);
}

uint64_t PrecompileNoticeRevision() noexcept {
	return g_revision.load(std::memory_order_acquire);
}

void ShowManifestProgress(const ManifestProgress& progress) {
	g_bar_progress.store(Pack(progress.done, progress.total), std::memory_order_relaxed);
	g_bar_left.store(Pack(progress.pipelines, progress.shaders), std::memory_order_relaxed);
	// Only showing it asks for a repaint: its numbers follow on the game's next frames.
	if (!g_bar_visible.exchange(true, std::memory_order_acq_rel)) {
		g_revision.fetch_add(1, std::memory_order_acq_rel);
	}
}

void HideManifestProgress() {
	if (g_bar_visible.exchange(false, std::memory_order_acq_rel)) {
		g_revision.fetch_add(1, std::memory_order_acq_rel);
	}
}

bool ManifestProgressVisible() noexcept {
	return g_bar_visible.load(std::memory_order_acquire);
}

bool ProcessPrecompileNoticeInput(const SDL_Event& event) {
	const auto state = static_cast<PrecompileNoticeState>(g_state.load(std::memory_order_acquire));
	if (!g_visible.load(std::memory_order_acquire) || state == PrecompileNoticeState::Waiting ||
	    state == PrecompileNoticeState::Compiling) {
		return false;
	}
	const bool question = state == PrecompileNoticeState::Question;
	switch (event.type) {
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP: {
			const auto key   = event.key.key;
			const bool enter = key == SDLK_RETURN || key == SDLK_KP_ENTER;
			// Without the question, Enter is the game's (it may run under the notice for a while).
			if ((!enter || !question) && key != SDLK_ESCAPE) {
				return false;
			}
			if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
				if (!enter) {
					Answer(PrecompileAnswer::Play);
				} else if (question) {
					Answer(PrecompileAnswer::Wait);
				}
			}
			return true;
		}
		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP: {
			const auto button = event.gbutton.button;
			const bool cross  = button == SDL_GAMEPAD_BUTTON_SOUTH;
			if ((!cross || !question) && button != SDL_GAMEPAD_BUTTON_EAST) {
				return false;
			}
			if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
				if (!cross) {
					Answer(PrecompileAnswer::Play);
				} else if (question) {
					Answer(PrecompileAnswer::Wait);
				}
			}
			return true;
		}
		default: return false;
	}
}

namespace {

// In the bottom-right corner, off the middle of the game's picture. Sized for 1280x720 and scaled
// with the window, with text no smaller than can be read; the spacing follows the text.
struct NoticeLayout {
	ImVec2 display;
	float  body   = 0.0f;
	float  title  = 0.0f;
	float  unit   = 0.0f;
	float  margin = 0.0f;
	float  width  = 0.0f;

	explicit NoticeLayout(vk::Extent2D extent)
	    : display(static_cast<float>(extent.width), static_cast<float>(extent.height)) {
		const float scale = std::min(display.x / 1280.0f, display.y / 720.0f);
		body              = std::max(14.0f * scale, 12.0f);
		title             = std::max(18.0f * scale, 14.0f);
		unit              = body / 14.0f;
		margin            = 16.0f * unit;
		width             = std::max(std::min(380.0f * unit, display.x - 2.0f * margin), 1.0f);
	}

	// A window whose bottom-right corner is `above` over the corner's margin. End() follows it,
	// whatever it returns.
	bool Begin(const char* name, float above, float alpha) const {
		ImGui::SetNextWindowPos({display.x - margin, display.y - margin - above}, ImGuiCond_Always,
		                        {1.0f, 1.0f});
		ImGui::SetNextWindowSize({width, 0.0f}, ImGuiCond_Always);
		ImGui::SetNextWindowBgAlpha(alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16.0f * unit, 12.0f * unit});
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {7.0f * unit, 8.0f * unit});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * unit);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * unit);
		constexpr ImGuiWindowFlags flags =
		    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
		    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
		    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;
		return ImGui::Begin(name, nullptr, flags);
	}

	static void End() {
		ImGui::End();
		ImGui::PopStyleVar(4);
	}
};

std::string Counted(uint32_t count, const char* what) {
	return fmt::format("{} {}{}", count, what, count == 1 ? "" : "s");
}

// The manifest's bar, `above` over the corner's margin.
void DrawManifestProgress(const NoticeLayout& layout, float above) {
	const uint64_t progress  = g_bar_progress.load(std::memory_order_relaxed);
	const uint64_t left      = g_bar_left.load(std::memory_order_relaxed);
	const auto     done      = static_cast<uint32_t>(progress >> 32u);
	const auto     total     = std::max(static_cast<uint32_t>(progress), done);
	const auto     pipelines = static_cast<uint32_t>(left >> 32u);
	const auto     shaders   = static_cast<uint32_t>(left);
	if (layout.Begin("##kyty_manifest_progress", above, 0.9f)) {
		ImGui::PushFont(nullptr, layout.body);
		ImGui::PushTextWrapPos(0.0f);
		ImGui::TextUnformatted("Preparing pipelines: performance may drop until they are ready.");
		const float fraction = total != 0 ? static_cast<float>(done) / static_cast<float>(total) : 0.0f;
		const auto  percent  = fmt::format("{} %", static_cast<uint32_t>(fraction * 100.0f));
		ImGui::ProgressBar(fraction, {-1.0f, ImGui::GetTextLineHeight() + 6.0f * layout.unit},
		                   percent.c_str());
		if (pipelines != 0 && shaders != 0) {
			ImGui::TextDisabled("%s left, %s to translate", Counted(pipelines, "pipeline").c_str(),
			                    Counted(shaders, "shader").c_str());
		} else if (pipelines != 0) {
			ImGui::TextDisabled("%s left", Counted(pipelines, "pipeline").c_str());
		} else if (shaders != 0) {
			ImGui::TextDisabled("%s to translate", Counted(shaders, "shader").c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
	}
	NoticeLayout::End();
}

// Returns its height.
float DrawNotice(const NoticeLayout& layout) {
	PrecompileNotice notice;
	{
		std::scoped_lock lock(g_mutex);
		notice = g_notice;
	}
	const float body   = layout.body;
	const float title  = layout.title;
	const float unit   = layout.unit;
	float       height = 0.0f;
	if (layout.Begin("##kyty_precompile_notice", 0.0f, 0.96f)) {
		height               = ImGui::GetWindowHeight();
		using State          = PrecompileNoticeState;
		const uint32_t total = std::max(notice.total, notice.done);
		// One title whatever made the game wait: the notice stays as the game goes from one to
		// another, and only its text follows.
		ImGui::PushFont(nullptr, title);
		ImGui::TextUnformatted("Compiling pipelines");
		ImGui::PopFont();
		ImGui::PushFont(nullptr, body);
		ImGui::PushTextWrapPos(0.0f);
		switch (notice.state) {
			case State::Question:
				ImGui::TextUnformatted(
				    "The GPU driver is compiling the pipelines this game will need. Wait for them to "
				    "finish, or play now and expect stutters while they compile in the background.");
				break;
			case State::Paused:
				ImGui::TextUnformatted("The game is paused while the GPU driver compiles the pipelines it "
				                       "will need. It goes on by itself when they are ready.");
				break;
			case State::Waiting: {
				const uint32_t waited = std::max(notice.waited_pipelines, 1u);
				ImGui::TextUnformatted(fmt::format("The game is waiting for {} the GPU driver is compiling. "
				                                   "It goes on by itself when {} ready.",
				                                   waited == 1 ? std::string("a pipeline")
				                                               : fmt::format("{} pipelines", waited),
				                                   waited == 1 ? "it is" : "they are")
				                           .c_str());
				break;
			}
			case State::Ready:
				ImGui::TextUnformatted("The pipelines are ready and the game goes on. It pauses again if it "
				                       "needs more.");
				break;
			case State::Compiling:
				if (notice.waited_pipelines == 0) {
					ImGui::TextUnformatted("The GPU driver has compiled the pipelines. The game runs as usual.");
				} else {
					ImGui::TextUnformatted(
					    fmt::format("The GPU driver is compiling {} the game uses. The game keeps running, "
					                "but it may stutter or skip some draws until {} ready.",
					                notice.waited_pipelines == 1
					                    ? std::string("a pipeline")
					                    : fmt::format("{} pipelines", notice.waited_pipelines),
					                notice.waited_pipelines == 1 ? "it is" : "they are")
					        .c_str());
				}
				break;
		}

		if (notice.state == State::Compiling) {
			// Nothing to count: these pipelines are not on a list.
			if (notice.waited_pipelines != 0) {
				ImGui::TextDisabled("compiling for %u s", notice.seconds_waited);
			}
		} else {
			const float fraction =
			    total != 0 ? static_cast<float>(notice.done) / static_cast<float>(total) : 0.0f;
			const auto progress = fmt::format("{} of {} ({} %)", notice.done, total,
			                                  static_cast<uint32_t>(fraction * 100.0f));
			ImGui::ProgressBar(fraction, {-1.0f, ImGui::GetTextLineHeight() + 6.0f * unit}, progress.c_str());
			// No time left: the game maps more shaders as it goes, whose pipelines are not known yet.
			if (notice.state == State::Waiting) {
				ImGui::TextUnformatted(TimeWaited(notice.seconds_waited).c_str());
			} else {
				if (notice.shaders != 0) {
					ImGui::TextDisabled("%u shaders to translate", notice.shaders);
				}

				ImGui::Separator();
				const float icon = ImGui::GetTextLineHeight();
				if (notice.state == State::Question) {
					Hint(true, "Enter", "Wait for them to finish", icon);
				}
				Hint(false, "Esc",
				     notice.state == State::Ready ? "Do not pause again (expect stutters)"
				                                  : "Play now (expect stutters)",
				     icon);
			}
		}
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
	}
	NoticeLayout::End();
	return height;
}

} // namespace

void DrawPrecompileNotice(vk::Extent2D extent) {
	const bool notice = g_visible.load(std::memory_order_acquire);
	const bool bar    = g_bar_visible.load(std::memory_order_acquire);
	if (!notice && !bar) {
		return;
	}
	const NoticeLayout layout(extent);
	const float  height = notice ? DrawNotice(layout) : 0.0f;
	// Over the notice.
	if (bar) {
		DrawManifestProgress(layout, height > 0.0f ? height + 8.0f * layout.unit : 0.0f);
	}
}

} // namespace Libs::Graphics
