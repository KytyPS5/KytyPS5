#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_TOAST_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_TOAST_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/core/patch.h"
#include "graphics/presentation/system/element.h"

#include <optional>

namespace Libs::Graphics::SystemUi {

/// A toast's tokens, its sizes from the theme by ToastDefaults, so every toast matches and a theme
/// change reaches them all. Its look is the card's, changed in Toast.
struct ToastTokens {
	// The toast's width, between these, hugging short content and wrapping long text.
	Dp min_width;
	Dp max_width;
	// The room between the edge and the content, for a toast with a band. A plain toast's content
	// may use them for its own padding, so all toasts match.
	Dp padding_horizontal;
	Dp padding_vertical;
	// How tall the band along the top edge that tells the statuses apart is.
	Dp edge_width;
	// How far from the right a toast slides in from as it arrives, a short nudge with its fade
	// rather than a long travel across the game. Keep it within the toast stack's margin from the
	// screen's edge, s6, so a toast starts on screen. The service slides the toast, so only the
	// theme's value is used, and patching it on one toast changes nothing.
	Dp slide_distance;
};

/// The theme's toast sizes, its width limits, padding and band width.
[[nodiscard]] ToastTokens ToastDefaults(const ThemeContext& theme);

/// What a toast says about how something went, and how it differs from the theme's toast.
struct ToastProps {
	/// What the toast asks of the stack or row it sits in, following the element kinds' Self
	/// convention.
	struct Self {
		ItemLayout layout;
	};

	// How it went, which sets the colour of the band along its top edge. A toast without a status
	// is plain, with no band and no padding, as what it holds brings its own.
	std::optional<Status> status;
	// Fields of the toast's tokens the caller changes, patched over ToastDefaults. A field left at
	// zero keeps the theme's, so the band can't be patched away.
	ToastTokens tokens = {};
	Self        self;
};

/// The theme's toast card holding `children` in a column, stretched across its width, built in
/// the current frame. It is a raised card with a stronger hairline border, the card's low shadow
/// and smaller corners than a card, so it reads on bright and dark game frames as a passing notice.
/// The card knows nothing of what it holds. A toast with a status has a band of the status colour
/// along its top edge and padding below it. Colour alone tells the statuses apart, so what it
/// holds must say what happened. Along the top, the band reads the
/// same for right to left text. It reads the current theme, so it needs an open FrameScope. If the
/// frame can't build it, the result is a none handle.
[[nodiscard]] BoxElement Toast(const ToastProps& props, const Children& children = {});

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_COMPONENTS_TOAST_H_
