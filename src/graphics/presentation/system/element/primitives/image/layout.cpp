#include "graphics/presentation/system/element/primitives/image/layout.h"

#include "graphics/presentation/system/core/units.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "graphics/presentation/system/element/primitives/image/image.h"

#include <algorithm>

namespace Libs::Graphics::SystemUi {

Vec2 ImageSize(const ItemLayout& item, Vec2 size) {
	const bool shaped = size.x > 0.0f && size.y > 0.0f;
	if (item.width && item.height) {
		return {ToPixels(*item.width), ToPixels(*item.height)};
	}
	if (item.width) {
		const float width = ToPixels(*item.width);
		return {width, shaped ? SnapPixel(width * size.y / size.x) : 0.0f};
	}
	if (item.height) {
		const float height = ToPixels(*item.height);
		return {shaped ? SnapPixel(height * size.x / size.y) : 0.0f, height};
	}
	// Its pixels count as design units, so an icon drawn for 1080p keeps its size on screen. A
	// negative or NaN size becomes 0, so it can't spoil the sums of the image's siblings.
	return {std::max(0.0f, ToPixels(Dp(size.x))), std::max(0.0f, ToPixels(Dp(size.y)))};
}

Measured MeasureImage(FrameBuilder& frame, AnyElement image, Vec2 /*max*/) {
	const ImageProps* const props = frame.FindAs<ImageProps>(image);
	if (props == nullptr) {
		return {};
	}
	const Vec2 size = ImageSize(props->self.layout, props->size);
	return {size, size};
}

} // namespace Libs::Graphics::SystemUi
