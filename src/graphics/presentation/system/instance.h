#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_INSTANCE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_INSTANCE_H_

#include "graphics/presentation/system/core/fonts.h"
#include "graphics/presentation/system/core/theme.h"
#include "graphics/presentation/system/imgui/imageCache.h"
#include "graphics/presentation/system/service/toast/options.h"
#include "graphics/presentation/system/service/toast/service.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace Libs::Graphics::SystemUi {

/// What the system UI is set up with. Whoever owns it knows its install folder, its fallback
/// fonts, its descriptor pool and its settings, so the library reads none of them itself.
struct InstanceOptions {
	/// Where the theme's fonts are found.
	struct FontOptions {
		// The folder the theme's font paths are relative to, ending in a separator.
		std::string assets_folder;
		// Run after each font is added, to merge in fonts for scripts it lacks. Optional.
		std::function<void()> merge_fallbacks;
	};

	/// How many decoded images are held.
	struct ImageOptions {
		// How many decoded images are held at once. The renderer's descriptor pool needs a slot for
		// each, so whoever sizes the pool passes the same number.
		size_t capacity = DEFAULT_IMAGE_CACHE_CAPACITY;
	};

	/// What a trophy toast says and how long it stays.
	struct TrophyOptions {
		// The line under a trophy's name, localised and ready for the toast font to draw.
		std::string subtitle;
		// How long a trophy toast stays once shown.
		float hold_ms = DEFAULT_TOAST_HOLD_MS;
	};

	FontOptions   fonts;
	ImageOptions  images;
	TrophyOptions trophy;
};

/// The system UI, set up once. It owns the toasts and the consumers that turn trophy and
/// notification events into them, which see only what is published after it is made, so its owner
/// picks when the system UI starts listening. The fonts and decoded images it draws with are made
/// the first time they are asked for, as they need the drawing thread with an ImGui context
/// current, which may not exist yet when it is made.
///
/// The toasts may be asked whether they need frames and added to from any thread. The fonts and
/// images are used on the drawing thread only, and released with ReleaseRendering after the
/// renderer backend shuts down and before the ImGui context goes, as the image cache asks. It can't
/// be copied or moved, as its consumers hold on to its toasts.
class Instance {
public:
	/// Sets up the system UI with `theme` and `options`, subscribing its consumers now. The theme's
	/// font paths are read now too, so the theme need not outlive it. Safe from any thread, as it
	/// touches no ImGui state.
	Instance(const ThemeContext& theme, InstanceOptions options);

	Instance(const Instance&)            = delete;
	Instance& operator=(const Instance&) = delete;
	Instance(Instance&&)                 = delete;
	Instance& operator=(Instance&&)      = delete;
	~Instance()                          = default;

	/// The toasts, which its consumers add to and its owner draws. Safe from any thread, and the
	/// service says which of its own calls are.
	[[nodiscard]] ToastService& Toasts() noexcept { return m_toasts; }

	/// The theme's fonts, loaded into the current ImGui context's atlas the first time the fonts or
	/// images are asked for. A weight is null when its file couldn't be loaded, and text then falls
	/// back to the other weight or to ImGui's own font.
	[[nodiscard]] const FontsContext& Fonts();

	/// The cache Image elements decode from, made the first time the fonts or images are asked for.
	/// Start its frame with BeginFrame, then hand it to whatever draws, such as ToastService::Draw.
	[[nodiscard]] ImageCache& Images();

	/// Drops the fonts and the image cache. Call it after the renderer backend shuts down and
	/// before the ImGui context goes. Asking for them again afterwards loads them afresh.
	void ReleaseRendering();

private:
	/// Loads the fonts and makes the image cache, unless they are already made.
	void PrepareRendering();

	// The full paths of the theme's regular and bold fonts. Resolved when it is made, as the
	// theme's own paths are views that may not last until the fonts load.
	std::string                 m_regular_path;
	std::string                 m_bold_path;
	InstanceOptions             m_options;
	ToastService                m_toasts;
	std::optional<FontsContext> m_fonts;
	std::optional<ImageCache>   m_images;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_INSTANCE_H_
