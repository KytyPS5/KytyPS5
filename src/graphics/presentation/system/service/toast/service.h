#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_SERVICE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_SERVICE_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/fonts.h"
#include "graphics/presentation/system/core/motion.h"
#include "graphics/presentation/system/events/consumer.h"
#include "graphics/presentation/system/service/toast/options.h"
#include "graphics/presentation/system/service/toast/queue.h"

#include <cstdint>
#include <memory>
#include <vector>

struct ImDrawList;

namespace Libs::Graphics::SystemUi {

class ImageCache;
struct ToastState;

/// Where toasts may be drawn, in pixels.
struct ToastArea {
	float width  = 0.0f;
	float height = 0.0f;
	// The bottom of whatever already sits at the top right, such as the performance panel, so the
	// toasts stack below it. Zero when nothing does.
	float top = 0.0f;
};

/// What one frame of toasts is drawn with, all of it owned by whoever draws, as it lives only as
/// long as their window, fonts and textures do.
struct ToastFrame {
	ImDrawList&         list;
	ToastArea           area;
	const ThemeContext& theme;
	const FontsContext& fonts;
	// Decodes the images in toasts, made current by Draw for the frame. Null when the toasts show
	// no images, and then images in toasts draw nothing, whatever cache the caller has open, as the
	// service never relies on a scope its caller opened.
	ImageCache*  images = nullptr;
	ClockContext clock;
	// How much the player lets toasts move. With reduced motion a toast appears in its place
	// rather than sliding in, and its fades are shortened.
	MotionContext motion = {};
};

/// A set of toasts drawn over the game. Whoever draws it owns it and hands it to the code that adds
/// toasts, such as its consumers, so nothing reaches it through a global. Anyone holding it adds a
/// toast as a builder of its element with options, and the service owns the toast from then on.
/// Each frame it lets its consumers handle their events, ends the holds that are over, then builds
/// every toast on screen from its builder, stacked at the top right below whatever sits there
/// already, each fading in and out, and forgets each once it has faded out. Toasts and consumers
/// may be added from any thread. Drawing happens on one thread, and whether it needs frames may
/// be asked from any. It can't be copied or moved, as its consumers hold on to it.
class ToastService {
public:
	/// An empty set of toasts with no consumers.
	ToastService();
	~ToastService();
	ToastService(const ToastService&)            = delete;
	ToastService& operator=(const ToastService&) = delete;
	ToastService(ToastService&&)                 = delete;
	ToastService& operator=(ToastService&&)      = delete;

	/// Adds a toast built by `build` each frame it is drawn, behaving as `options` say, from `now`.
	/// It reaches the queue at the next frame drawn, then shows, or waits for room, and a toast
	/// with the key of one already there replaces it. Returns the toast's id, which dismisses it
	/// even when it merged into another by key, though the merged toast keeps the id it had in
	/// OnScreen and in what Draw returns. Each repeat by key is remembered until its toast has
	/// gone. Safe from any thread and never waits for a frame to be drawn, but not from a builder.
	uint64_t Add(ToastBuilder build, ToastOptions options, TimePoint now);

	/// Starts the toast with `id` leaving at the next frame drawn, or forgets it if it was still
	/// waiting, in order with the adds before it. Nothing happens when there is none, for example
	/// because it already left. Safe from any thread and never waits for a frame to be drawn, but
	/// not from a builder.
	void Dismiss(uint64_t id);

	/// Adds `consumer`, which is handed the time each frame drawn from the next one. Safe from any
	/// thread.
	void AddConsumer(std::unique_ptr<EventConsumerBase> consumer);

	/// Whether a toast is on screen or waiting, or a consumer has events waiting, so frames must be
	/// drawn. Safe from any thread.
	[[nodiscard]] bool NeedsFrames() const;

	/// Draws one frame of toasts as `frame` says. Returns the toasts that came on screen in this
	/// frame, so the caller can greet each once, such as with its sound, valid until the next
	/// Draw. Called on the one thread that draws.
	const std::vector<ToastEntry>& Draw(const ToastFrame& frame);

	/// A copy of the toasts on screen, oldest first, with any added since the last frame put in
	/// place first, such as for a test to read what a consumer added. Safe from any thread, but
	/// not from a builder, and may wait while a frame of toasts is drawn.
	[[nodiscard]] std::vector<ToastEntry> OnScreen();

	/// How many toasts wait for room, with any added since the last frame put in place first.
	/// Safe from any thread, but not from a builder, and may wait while a frame of toasts is drawn.
	[[nodiscard]] size_t Waiting();

private:
	std::unique_ptr<ToastState> m_state;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_SERVICE_H_
