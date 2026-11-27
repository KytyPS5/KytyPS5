#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_OPTIONS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_OPTIONS_H_

#include "graphics/presentation/system/element.h"

#include <cstdint>
#include <functional>
#include <string>

namespace Libs::Graphics::SystemUi {

/// How long a toast stays once shown, long enough to read a short line without hurrying.
inline constexpr float DEFAULT_TOAST_HOLD_MS = 4000.0f;

/// Builds a toast's element in the current frame. A toast lasts many frames and an element only
/// one, so the service keeps the builder and calls it every frame the toast is drawn. It runs on
/// the thread that draws, with an open FrameScope and ImageCacheScope, so it holds plain data, such
/// as a view's props. It runs while the service holds its toasts, so it must not call into the
/// toast service at all.
using ToastBuilder = std::function<BoxElement()>;

/// The sound played when a toast comes on screen, so whoever plays sounds needs nothing of what the
/// toast shows.
enum class ToastSound : uint8_t { None, Unlock };

/// How a toast behaves in the queue, apart from what it shows.
struct ToastOptions {
	// A repeat with the same key refreshes the toast already there rather than stacking another,
	// such as a controller that keeps reconnecting. An empty key never merges.
	std::string key;
	// How long it stays once shown. An infinite hold stays until dismissed.
	float      hold_ms = DEFAULT_TOAST_HOLD_MS;
	ToastSound sound   = ToastSound::None;
	// Whether a band of light sweeps across it once it has come on screen, after the theme's
	// motion.shine_delay, for a moment worth marking such as a trophy. Off by default, so an
	// everyday notice doesn't catch the eye.
	bool shine = false;
};

/// A toast as the queue holds it, what it shows and how it behaves.
struct ToastRequest {
	ToastBuilder build;
	ToastOptions options;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_OPTIONS_H_
