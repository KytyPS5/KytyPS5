#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_NAV_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_NAV_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/pages/registry.h"
#include "graphics/presentation/system/gallery/pointer.h"

#include <array>
#include <cstdint>

namespace Libs::Graphics::SystemUi {
class FrameBuilder;
} // namespace Libs::Graphics::SystemUi

namespace Libs::Graphics::SystemUi::Gallery {

/// How wide the sidebar is, in design units.
inline constexpr Dp SIDEBAR_WIDTH = Dp(350.0f);

/// What the sidebar shows and where its controls were placed last frame. The system UI has no
/// pointer input of its own yet, so the gallery tests the mouse against these itself, a frame
/// behind, which is not noticeable at frame rate.
struct NavState {
	GalleryPage page           = GalleryPage::Colours;
	bool        reduced_motion = false;
	// How many times the reduced motion switch has flipped, which keys its knob's slide, so each
	// flip slides the knob in again.
	uint32_t                             flips = 0;
	std::array<Rect, GALLERY_PAGE_COUNT> row_rects {};
	Rect                                 toggle_rect;
};

/// The sidebar's elements built in a frame, so where they land can be read once it is arranged.
struct NavElements {
	AnyElement                                 root;
	std::array<AnyElement, GALLERY_PAGE_COUNT> rows {};
	AnyElement                                 toggle;
};

/// Takes in a click of `pointer` on last frame's rows or switch, changing the page or flipping
/// reduced motion. Call it before the frame is built, so the frame shows the change.
void UpdateNav(NavState& state, const PointerInput& pointer);

/// What the sidebar is built from this frame, the page and switch it shows with last frame's
/// rects, and the mouse that hovers and presses them.
struct NavProps {
	// Held for the call only, as Nav reads it while it builds.
	const NavState& state;
	PointerInput    pointer;
};

/// Builds the sidebar in the current frame, the gallery's title over the pages under their group
/// captions, with the reduced motion switch at its foot, each hovered and pressed by the pointer
/// against last frame's rects. The selected page shows the accent tint and the selection bar, so
/// it reads without relying on colour alone. Its root is SIDEBAR_WIDTH wide, and a row that
/// stretches it gives it the full height. Pass what it returns to RememberNav once the frame is
/// arranged, so next frame's clicks land. It needs an open FrameScope.
[[nodiscard]] NavElements Nav(const NavProps& props);

/// Keeps where the sidebar's controls were placed in `frame`, for next frame's pointer tests. Call
/// it once the frame is arranged.
void RememberNav(NavState& state, const FrameBuilder& frame, const NavElements& built);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_NAV_H_
