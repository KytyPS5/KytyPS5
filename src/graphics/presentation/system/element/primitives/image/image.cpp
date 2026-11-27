#include "graphics/presentation/system/element/primitives/image/image.h"

#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/primitives/image/layout.h"
#include "graphics/presentation/system/element/primitives/image/paint.h"
#include "graphics/presentation/system/imgui/imageCache.h"

namespace Libs::Graphics::SystemUi {

namespace {

// How images are measured and painted, set on every frame an image is built in. An image has
// nothing below it to place, so it needs no arrange hook.
constexpr FrameHooks IMAGE_FRAME_HOOKS {.measure = &MeasureImage, .paint = &PaintImage};

} // namespace

ImageElement Image(const ImageProps& props) {
	FrameBuilder& frame = CurrentBuilder();
	// Set on every call, as Box and Text do, so any frame an image is built in can lay it out.
	frame.SetFrameHooks<ImageProps>(IMAGE_FRAME_HOOKS);
	ImageProps shown = props;
	ImageCache* const cache = CurrentImageCache();
	if (shown.texture == nullptr && shown.source != nullptr && cache != nullptr) {
		// Only an image that decoded replaces the props, so one that can't be shown keeps the size
		// it was given and lays out as it would with no cache open.
		const DecodedImage decoded = cache->Use(*shown.source);
		if (decoded.texture != nullptr) {
			shown.texture = decoded.texture;
			// The decoded image's own size, as a different one would skew how it is fitted.
			shown.size = decoded.size;
		}
	}
	// The source is only read here, so the frame never keeps a pointer that may not outlive it.
	shown.source = nullptr;
	return frame.AddNode(shown, {}, shown.self.layout);
}

} // namespace Libs::Graphics::SystemUi
