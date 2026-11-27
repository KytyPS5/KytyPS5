#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TOAST_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TOAST_PAGE_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/encodedImage.h"
#include "graphics/presentation/system/core/fonts.h"
#include "graphics/presentation/system/core/motion.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/service.h"

#include <cstdint>
#include <string_view>

struct ImDrawList;

namespace Libs::Graphics::SystemUi::Gallery {

/// The line under a demo trophy's name, in the specimens and the live demo alike.
inline constexpr std::string_view DEMO_TROPHY_SUBTITLE = "Trophy earned";

/// How long a live demo trophy stays, the emulator's default trophy duration.
inline constexpr float DEMO_TROPHY_HOLD_MS = 6000.0f;

/// What the Toast page shows its trophies with.
struct ToastPageProps {
	// The trophies' icon, or none to show them without one.
	EncodedImage icon;
};

/// Builds the Toast page in the current frame and returns its root. It shows the bare toast
/// component in each type beside its layout tokens, then the notices built on it.
/// They show each type, long text wrapping and cut short, the narrowest toast, trophies of each
/// grade with the props' icon, a long name and none, a stack mixing a trophy with notices, and
/// toasts over a bright and a mid grey frame. It reads the current theme and fonts, so it needs an
/// open FrameScope, and an ImageCacheScope for the icon. The caller measures, arranges and paints
/// it.
[[nodiscard]] AnyElement BuildToastPage(const ToastPageProps& props);

/// A live run of real toasts at the top right of the window, as they sit in game. It publishes a
/// test event on a steady rhythm through a real event channel, and every fourth a trophy unlock
/// with `icon_png` through the trophy event, and consumers turn each into a toast, so the hold, the
/// fade, the queue, the icons and the event path can be watched with no input. It draws the toasts
/// of the system UI it is given, whose trophy consumer shows the demo's trophies, and adds its own
/// consumer of the test events to them. The consumers subscribe to the program's event channels,
/// so a second demo would show this one's events too, and one demo is enough.
class ToastDemo {
public:
	/// Starts the demo, adding its consumer of the test events to `toasts`, which must outlive it.
	ToastDemo(ToastService& toasts, EncodedImage icon_png);

	ToastDemo(const ToastDemo&)            = delete;
	ToastDemo& operator=(const ToastDemo&) = delete;
	ToastDemo(ToastDemo&&)                 = delete;
	ToastDemo& operator=(ToastDemo&&)      = delete;
	~ToastDemo()                           = default;

	/// Publishes the next event once its time has come, then draws its toasts into `list` in
	/// `area`, below whatever sits at its top, in `theme` with `fonts`, at `clock`'s time and with
	/// the player's `motion`. Their icons come from the ImageCache the caller has open.
	void Draw(ImDrawList& list, const ToastArea& area, const ThemeContext& theme,
	          const FontsContext& fonts, ClockContext clock, MotionContext motion);

private:
	EncodedImage  m_icon_png;
	TimePoint     m_next {};
	uint32_t      m_published = 0;
	uint32_t      m_notices   = 0;
	ToastService& m_toasts;
};

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_TOAST_PAGE_H_
