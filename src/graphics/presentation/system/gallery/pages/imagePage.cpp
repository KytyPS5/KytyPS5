#include "graphics/presentation/system/gallery/pages/imagePage.h"

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"
#include "graphics/presentation/system/gallery/components/page.h"

#include <array>
#include <string_view>

namespace Libs::Graphics::SystemUi::Gallery {

namespace {

constexpr std::string_view RULE =
    "Cover for thumbnails and posters. Contain when nothing may be cropped. Fill only for "
    "gradients. Always give a surface behind an image, as a missing one draws nothing.";

// A slot wider than it is tall, so a square image fits it differently with each fit.
constexpr float WIDE_WIDTH  = 160.0f;
constexpr float WIDE_HEIGHT = 64.0f;
// A square slot for the rounded sample.
constexpr float SQUARE_SIDE = 64.0f;

// A sample of the image in a slot of `width` by `height` with `styles`, captioned below. The slot
// is filled in behind the image, so the space a contained image leaves shows.
AnyElement Sample(const ThemeContext& theme, ImTextureData* texture, Vec2 size, float width,
                  float height, const ImageStyles& styles, std::string_view caption) {
	const ItemLayout                slot {.width = Dp(width), .height = Dp(height)};
	const std::array<AnyElement, 1> picture {
	    Image({.texture = texture, .size = size, .self = {.layout = slot, .styles = styles}}),
	};
	const std::array<AnyElement, 2> parts {
	    Box({.self = {.layout = slot, .styles = {.fill = theme.colors.surface.inset}}}, picture),
	    Text({.text = caption,
	          .self = {.styles = {.type  = theme.type.caption,
	                              .color = theme.colors.text.secondary}}}),
	};
	return Box({.layout = {.direction = FlexDirection::Column,
	                       .align     = Align::Center,
	                       .gap       = theme.space.s2}},
	           parts);
}

} // namespace

AnyElement BuildImagePage(const ImagePageProps& props) {
	ImTextureData* const            texture = props.texture;
	const Vec2                      size    = props.size;
	const ThemeContext&             theme   = CurrentThemeContext();
	const std::array<AnyElement, 4> samples {
	    Sample(theme, texture, size, WIDE_WIDTH, WIDE_HEIGHT, {.fit = ImageFit::Fill}, "Fill"),
	    Sample(theme, texture, size, WIDE_WIDTH, WIDE_HEIGHT, {.fit = ImageFit::Contain},
	           "Contain"),
	    Sample(theme, texture, size, WIDE_WIDTH, WIDE_HEIGHT, {.fit = ImageFit::Cover}, "Cover"),
	    Sample(theme, texture, size, SQUARE_SIDE, SQUARE_SIDE,
	           {.fit = ImageFit::Contain, .radius = theme.radius.lg}, "Radius"),
	};
	return Page(
	    {.title = "Image", .subtitle = RULE},
	    {
	        Section(theme, "Fits and corners", Box({.layout = {.gap = theme.space.s4}}, samples)),
	    });
}

} // namespace Libs::Graphics::SystemUi::Gallery
