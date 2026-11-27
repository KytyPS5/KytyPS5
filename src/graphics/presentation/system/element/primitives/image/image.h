#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_IMAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_IMAGE_H_

#include "graphics/presentation/system/common/props/imageStyles.h"
#include "graphics/presentation/system/common/props/itemLayout.h"
#include "graphics/presentation/system/core/encodedImage.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/element/element.h"

struct ImTextureData;

namespace Libs::Graphics::SystemUi {

struct ImageProps;

/// An image's element, what Image returns.
using ImageElement = Element<ImageProps>;

/// Which image an Image element shows, and how. The texture is only pointed to, so whoever owns it
/// must keep it alive until the frame is drawn. An image may instead be given its encoded source,
/// which the open ImageCache decodes, so whoever builds it needs no texture of its own.
struct ImageProps {
	/// What an image asks of its container and how it looks, kept apart from which image it is as
	/// BoxProps::Self is kept apart from a box's children.
	struct Self {
		// An explicit width or height sets the image's size on that axis, and the other follows
		// the image's proportions. With neither, the image is its own pixel size at the reference
		// resolution, scaled as everything else is, so an image meant to stay sharp on a large
		// screen is drawn bigger and given an explicit size. Its own min_main or max_main,
		// or stretching on the cross axis, can still change one length alone, which Contain shows
		// as spare space and Fill as a stretched image.
		ItemLayout  layout;
		ImageStyles styles;
	};

	ImTextureData* texture = nullptr;
	// The image's size in pixels, which gives its proportions and its own size. It should be the
	// texture's real size, since a different one skews how the image is fitted.
	Vec2 size;
	Self self;
	// The encoded image to show when there is no texture, decoded by the ImageCache open around
	// the call, which then also gives its size. Only read while the element is built,
	// so it need only live that long. Null, or no cache open, shows nothing.
	const EncodedImage* source = nullptr;
};

/// An image element showing `props`, built in the current frame. It has no children and never
/// shrinks below its size. A source with no texture is decoded by the current ImageCache. A null
/// texture, or a source that can't be shown, takes its space and draws nothing. A refused image is
/// a none handle, and a RefusalScope open around the call says why.
[[nodiscard]] ImageElement Image(const ImageProps& props);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_IMAGE_IMAGE_H_
