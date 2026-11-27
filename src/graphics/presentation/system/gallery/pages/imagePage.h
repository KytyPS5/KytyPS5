#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_IMAGE_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_IMAGE_PAGE_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

struct ImTextureData;

namespace Libs::Graphics::SystemUi::Gallery {

/// The image the Image page shows.
struct ImagePageProps {
	// The image's texture, or null to leave the samples empty.
	ImTextureData* texture = nullptr;
	// The texture's size in pixels.
	Vec2 size;
};

/// Builds the Image page in the current frame and returns its root, its head over a
/// row of samples of the props' texture. Three show how it fits a wide slot, filled, contained and
/// covering, and one shows rounded corners, each captioned. A null texture leaves the slots empty.
/// It reads the current theme and fonts, so it needs an open FrameScope.
[[nodiscard]] AnyElement BuildImagePage(const ImagePageProps& props);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_IMAGE_PAGE_H_
