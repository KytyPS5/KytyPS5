#include "graphics/presentation/system/imgui/draw/shadow.h"

#include "graphics/presentation/system/core/context.h"
#include "graphics/presentation/system/imgui/convert.h"
#include "graphics/presentation/system/imgui/draw/box.h"
#include "imgui.h"
#include "imgui_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace Libs::Graphics::SystemUi {

namespace {

// The fade is split into bands about this many pixels wide, so it follows its curve as closely at
// 4K as at 1080p. The GPU blends each band's alpha between its edges, so the bands don't show.
constexpr float BAND_WIDTH = 6.0f;
constexpr int   MIN_BANDS  = 3;
constexpr int   MAX_BANDS  = 12;
// A corner of the outer edge is cut into segments about this many pixels long. The edge fades to
// nothing, so it needs far fewer points than a shape with a crisp edge would.
constexpr float CORNER_SEGMENT_LENGTH = 8.0f;
constexpr int   MIN_CORNER_SEGMENTS   = 4;
constexpr int   MAX_CORNER_SEGMENTS   = 24;
constexpr int   CORNERS               = 4;
constexpr float QUARTER_TURN          = std::numbers::pi_v<float> / 2.0f;
// The most points an outline can have, one more than its segments at each corner.
constexpr int MAX_OUTLINE_POINTS = CORNERS * (MAX_CORNER_SEGMENTS + 1);
// Each band is a strip of two triangles per outline point, and the centre a fan.
constexpr int TRIANGLES_PER_QUAD   = 2;
constexpr int INDICES_PER_TRIANGLE = 3;

// How many pieces of `piece` pixels fit `length` pixels, within the limits. The clamp comes before
// the conversion, since a huge length would not fit an integer. `length` must not be NaN, which the
// clamp would pass through.
int PieceCount(float length, float piece, int minimum, int maximum) {
	return static_cast<int>(std::ceil(
	    std::clamp(length / piece, static_cast<float>(minimum), static_cast<float>(maximum))));
}

// How strong the shadow is a fraction `t` of the way out, from 1 at the element to 0 at the outer
// edge. It is a smoothstep. A straight ramp would meet the background at an angle, which the eye
// picks out as a line, while this one arrives flat at both ends.
float Falloff(float t) {
	return 1.0f - t * t * (3.0f - 2.0f * t);
}

// A rounded rectangle's outline as points that can be pushed outward together. Each point moves
// along its own direction from a fixed corner centre, so every expansion of the outline has the
// same number of points, and neighbouring rings can be joined by a strip without any gaps.
struct HaloOutline {
	std::array<ImVec2, MAX_OUTLINE_POINTS> centres;
	std::array<ImVec2, MAX_OUTLINE_POINTS> directions;
	int                                    count  = 0;
	float                                  radius = 0.0f;

	// Point `index` of the outline grown by `expansion` pixels.
	[[nodiscard]] ImVec2 At(int index, float expansion) const {
		const float reach = radius + expansion;
		return {centres[index].x + directions[index].x * reach,
		        centres[index].y + directions[index].y * reach};
	}
};

// The outline of `rect` with corners of `rounding` pixels, already held to what the rectangle can
// take, with corners fine enough for the outer edge `spread` pixels further out. It runs clockwise
// from the top right corner.
HaloOutline MakeOutline(const Rect& rect, float rounding, float spread) {
	HaloOutline outline;
	// Written so a NaN rounding gives no corners, as a NaN would reach the point counts through a
	// clamp.
	outline.radius     = rounding > 0.0f ? rounding : 0.0f;
	const int segments = PieceCount((outline.radius + spread) * QUARTER_TURN, CORNER_SEGMENT_LENGTH,
	                                MIN_CORNER_SEGMENTS, MAX_CORNER_SEGMENTS);
	const std::array<ImVec2, CORNERS> corners {
	    ImVec2 {rect.Right() - outline.radius, rect.y + outline.radius},
	    ImVec2 {rect.Right() - outline.radius, rect.Bottom() - outline.radius},
	    ImVec2 {rect.x + outline.radius, rect.Bottom() - outline.radius},
	    ImVec2 {rect.x + outline.radius, rect.y + outline.radius}};
	for (int corner = 0; corner < CORNERS; corner++) {
		// The top right corner's arc starts pointing straight up, and each corner turns on from
		// there.
		const float start = QUARTER_TURN * static_cast<float>(corner - 1);
		for (int step = 0; step <= segments; step++) {
			const float angle =
			    start + QUARTER_TURN * static_cast<float>(step) / static_cast<float>(segments);
			outline.centres[outline.count]    = corners[corner];
			outline.directions[outline.count] = {std::cos(angle), std::sin(angle)};
			outline.count++;
		}
	}
	return outline;
}

// Whether a halo covers the element it surrounds. A shadow is filled under it, as the element in
// front may be translucent and a shadow behind it still darkens what shows through. A glow is
// hollow, as coloured light under a translucent fill would tint it.
enum class HaloFill : uint8_t { Filled, Hollow };

// Writes the triangles of a halo whose vertices start at `first`, ring after ring from the inside.
// A filled halo's innermost ring is filled as a fan, and each pair of neighbouring rings is joined
// by a strip, wrapping round to the first point.
void WriteHaloIndices(ImDrawList& list, unsigned int first, int points, int bands, HaloFill fill) {
	const auto write = [&list](unsigned int index) {
		list.PrimWriteIdx(static_cast<ImDrawIdx>(index));
	};
	for (int point = 1; fill == HaloFill::Filled && point + 1 < points; point++) {
		write(first);
		write(first + point);
		write(first + point + 1);
	}
	for (int band = 0; band < bands; band++) {
		const unsigned int inner = first + static_cast<unsigned int>(band * points);
		const unsigned int outer = inner + static_cast<unsigned int>(points);
		for (int point = 0; point < points; point++) {
			const int next = (point + 1) % points;
			write(inner + point);
			write(outer + point);
			write(outer + next);
			write(inner + point);
			write(outer + next);
			write(inner + next);
		}
	}
}

// Draws a soft halo around `rect`, `spread` pixels wide, as one mesh. It reaches `peak` times the
// colour's own alpha at the element, filled under it or hollow as `fill` says, and alpha is set at
// each vertex, so the GPU blends it smoothly across each band. Nothing overlaps, so it has none of
// the steps that stacked shapes show. The shadow and the glow share it, which is why it takes no
// offset of its own. `rounding` must already be held to half the shorter side, as BoxRounding
// does, or the corner centres cross and the mesh folds over itself.
void DrawSoftHalo(ImDrawList& list, const Rect& rect, float rounding, float spread, float peak,
                  Color color, HaloFill fill) {
	if (!(spread > 0.0f) || !std::isfinite(spread) || !(peak > 0.0f) || color.a == 0) {
		return;
	}
	const HaloOutline outline = MakeOutline(rect, rounding, spread);
	const int         points  = outline.count;
	const int         bands   = PieceCount(spread, BAND_WIDTH, MIN_BANDS, MAX_BANDS);
	const int fan_indices     = fill == HaloFill::Filled ? (points - 2) * INDICES_PER_TRIANGLE : 0;
	const int band_indices    = points * TRIANGLES_PER_QUAD * INDICES_PER_TRIANGLE;
	list.PrimReserve(fan_indices + bands * band_indices, points * (bands + 1));
	// Read after reserving, which can start a new vertex range when the list is nearly full.
	WriteHaloIndices(list, list._VtxCurrentIdx, points, bands, fill);
	// From the list's own shared data, as ImGui's own shapes do, so it matches the list drawn into.
	const ImVec2 white = list._Data->TexUvWhitePixel;
	for (int band = 0; band <= bands; band++) {
		const float t    = static_cast<float>(band) / static_cast<float>(bands);
		const ImU32 ring = ToImU32(color.ScaleAlpha(peak * Falloff(t)));
		for (int point = 0; point < points; point++) {
			list.PrimWriteVtx(outline.At(point, spread * t), white, ring);
		}
	}
}

} // namespace

void DrawShadow(ImDrawList& list, const Rect& rect, float radius, const ShadowStyle& style,
                float opacity) {
	const ScaleContext& scale  = CurrentScaleContext();
	const float         offset = scale.Px(style.offset_y);
	const float         across = scale.Px(style.offset_x);
	// An offset that isn't a number would put every vertex at NaN, so nothing is drawn.
	if (!std::isfinite(offset) || !std::isfinite(across)) {
		return;
	}
	const Rect  moved = {rect.x + across, rect.y + offset, rect.width, rect.height};
	const float peak  = ClampUnit(style.peak_alpha) * ClampUnit(opacity);
	DrawSoftHalo(list, moved, BoxRounding(rect, radius), scale.Px(style.blur), peak, style.color,
	             HaloFill::Filled);
}

void DrawGlow(ImDrawList& list, const Rect& rect, float radius, const GlowStyle& style,
              float opacity) {
	const float peak = ClampUnit(style.peak_alpha) * ClampUnit(opacity);
	DrawSoftHalo(list, rect, BoxRounding(rect, radius), CurrentScaleContext().Px(style.radius),
	             peak, style.color, HaloFill::Hollow);
}

} // namespace Libs::Graphics::SystemUi
