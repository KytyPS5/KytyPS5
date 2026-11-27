#include "graphics/presentation/system/components/glyphs/trophy.h"

#include <array>

namespace Libs::Graphics::SystemUi {

namespace {

// The cup on a square grid of 22 units, the shape the overlay drew before the element framework,
// each part a rectangle on the grid. The shape is 21 units tall, so it sits half a unit down to be
// centred in the square, and each part's top is that half unit plus its place on the shape. The
// handles overlap the bowl's sides, so only their outer halves show.
constexpr float GRID_SIZE    = 22.0f;
constexpr float SHAPE_HEIGHT = 21.0f;
constexpr float SHAPE_TOP    = (GRID_SIZE - SHAPE_HEIGHT) / 2.0f;
// How round the bowl's bottom and the handles' outer corners are, how thick the handles' rings
// are, and how round the base is, in grid units.
constexpr float CORNER        = 2.0f;
constexpr float HANDLE_STROKE = 1.8f;
constexpr float BASE_CORNER   = 1.5f;
// The bowl's place on the shape, which its two shapes below share.
constexpr float BOWL_LEFT   = 3.0f;
constexpr float BOWL_WIDTH  = 16.0f;
constexpr float BOWL_HEIGHT = 13.0f;

// A shape rounds every corner alike, so the bowl, square at its top and round at its bottom, is a
// square shape over all of it but its bottom corners, and a rounded strip along its bottom twice a
// corner tall, whose own top corners the square one hides. The two overlap by one corner only, so
// a translucent cup blends twice over as little of the bowl as can be. Each handle's ring is
// rounded as the bowl's bottom is, and its inner corners lie inside the bowl, so only its outer,
// rounded side shows. The ring is drawn inside its rectangle, where the overlay's stroke straddled
// the edge, so it reaches a little less far out than it did. Handles first, so the bowl covers
// their inner halves.
constexpr std::array<GlyphShape, 6> TROPHY_SHAPES {{
    // The handles.
    {.x            = 0.0f,
     .y            = SHAPE_TOP + 2.0f,
     .width        = 5.0f,
     .height       = 6.0f,
     .radius       = CORNER,
     .border_width = HANDLE_STROKE},
    {.x            = 17.0f,
     .y            = SHAPE_TOP + 2.0f,
     .width        = 5.0f,
     .height       = 6.0f,
     .radius       = CORNER,
     .border_width = HANDLE_STROKE},
    // The bowl's rounded bottom, then its square top.
    {.x      = BOWL_LEFT,
     .y      = SHAPE_TOP + BOWL_HEIGHT - 2.0f * CORNER,
     .width  = BOWL_WIDTH,
     .height = 2.0f * CORNER,
     .fill   = true,
     .radius = CORNER},
    {.x      = BOWL_LEFT,
     .y      = SHAPE_TOP,
     .width  = BOWL_WIDTH,
     .height = BOWL_HEIGHT - CORNER,
     .fill   = true},
    // The stem and the base.
    {.x = 9.5f, .y = SHAPE_TOP + BOWL_HEIGHT, .width = 3.0f, .height = 5.0f, .fill = true},
    {.x      = 6.0f,
     .y      = SHAPE_TOP + 18.0f,
     .width  = 10.0f,
     .height = 3.0f,
     .fill   = true,
     .radius = BASE_CORNER},
}};

} // namespace

GlyphElement TrophyGlyph(const TrophyGlyphProps& props) {
	return Glyph(
	    {.shapes = TROPHY_SHAPES,
	     .grid   = GRID_SIZE,
	     .size   = props.size,
	     .self   = {.layout = props.self.layout, .styles = {.color = props.self.styles.color}}});
}

} // namespace Libs::Graphics::SystemUi
