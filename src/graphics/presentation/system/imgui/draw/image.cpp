#include "graphics/presentation/system/imgui/draw/image.h"

#include "graphics/presentation/system/core/color.h"
#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"

#include <algorithm>

namespace Libs::Graphics::SystemUi {

namespace {

// Where the image goes and which part of it shows. A contained image shrinks its rectangle and
// shows all of itself, a covering one keeps the rectangle and shows only the middle of itself.
struct Placement {
	Rect   rect;
	ImVec2 uv_min {0.0f, 0.0f};
	ImVec2 uv_max {1.0f, 1.0f};
};

// Places an image of `size` in `rect` as `fit` says.
Placement Place(Vec2 size, const Rect& rect, ImageFit fit) {
	if (fit == ImageFit::Fill || size.x <= 0.0f || size.y <= 0.0f || rect.width <= 0.0f ||
	    rect.height <= 0.0f) {
		return {rect};
	}
	const float image_aspect = size.x / size.y;
	const float rect_aspect  = rect.width / rect.height;
	if (fit == ImageFit::Contain) {
		const float width  = image_aspect > rect_aspect ? rect.width : rect.height * image_aspect;
		const float height = image_aspect > rect_aspect ? rect.width / image_aspect : rect.height;
		return {{SnapPixel(rect.x + (rect.width - width) / 2.0f),
		         SnapPixel(rect.y + (rect.height - height) / 2.0f), width, height}};
	}
	// Cover keeps the share of the image that has the rectangle's shape, centred.
	const float shown_x = image_aspect > rect_aspect ? rect_aspect / image_aspect : 1.0f;
	const float shown_y = image_aspect > rect_aspect ? 1.0f : image_aspect / rect_aspect;
	return {rect,
	        {(1.0f - shown_x) / 2.0f, (1.0f - shown_y) / 2.0f},
	        {(1.0f + shown_x) / 2.0f, (1.0f + shown_y) / 2.0f}};
}

} // namespace

Rect PlacedImageRect(Vec2 size, const Rect& rect, ImageFit fit) {
	return Place(size, rect, fit).rect;
}

void DrawImage(ImDrawList& list, ImTextureData* texture, Vec2 size, const Rect& rect,
               const ImageStyles& styles, float opacity) {
	const Color tint = Rgb(0xFFFFFF).ScaleAlpha(opacity);
	if (texture == nullptr || tint.a == 0) {
		return;
	}
	const auto  placed  = Place(size, rect, styles.fit);
	const float corners = std::min(CurrentScaleContext().Px(styles.radius.value),
	                               std::min(placed.rect.width, placed.rect.height) / 2.0f);

	const auto [image_start, image_end] = ToImRect(placed.rect);
	list.AddImageRounded(texture->GetTexRef(), image_start, image_end, placed.uv_min, placed.uv_max,
	                     ToImU32(tint), corners);
}

} // namespace Libs::Graphics::SystemUi
