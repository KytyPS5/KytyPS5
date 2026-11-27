#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_LAYOUT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_LAYOUT_H_

#include "graphics/presentation/system/element/element.h"
#include "graphics/presentation/system/element/frame/frameHooks.h"
#include "graphics/presentation/system/element/primitives/text/text.h"
#include "graphics/presentation/system/imgui/draw/text.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The Text element's measuring and arranging, and what they leave for painting. Internal to the
// text primitive, so components don't include it.
namespace Libs::Graphics::SystemUi {

/// A text layout kept from frame to frame, and what it was made from. The key holds the font's
/// address, which is safe while fonts are loaded once, and would need the cache cleared should they
/// ever be reloaded.
struct CachedTextLayout {
	// An entry never filled matches nothing, as a default key could.
	bool          used = false;
	TextLayoutKey key;
	// The bytes and not a view, as the frame's interned text changes under the same address.
	std::string text;
	TextLayout  layout;
	// The least width this text can take at this key, which measuring again would only repeat.
	float min_width = 0.0f;
	// When the entry was last used, by its slot's count, so a new layout replaces the one used
	// longest ago. A width that changes every frame then costs one layout a frame, where replacing
	// the oldest made would push out the widths still in use one after another.
	uint64_t last_use = 0;
};

/// How many layouts each text keeps. A text wrapped into a column sees about four widths in a
/// frame, unbounded, the room across, its final length and the width it is arranged at, and a text
/// that needs more widths in a frame than this misses every frame.
inline constexpr size_t TEXT_LAYOUTS_KEPT = 4;
static_assert(TEXT_LAYOUTS_KEPT <= UINT8_MAX, "a row names its entry in eight bits");

/// The layouts kept for the text at one row of the text table. A text has no identity of its own,
/// so nothing but the key and bytes says a layout is right, and a tree that changes only misses
/// until it settles.
struct TextSlot {
	std::array<CachedTextLayout, TEXT_LAYOUTS_KEPT> entries;
	// Counts every use of the slot, to stamp the entry used.
	uint64_t uses = 0;
};

/// Every text row's kept layouts. A kept workspace, so a steady frame finds its layouts and makes
/// none, which is the only step that allocates or measures glyphs. It holds a slot for as many
/// texts as the busiest frame had, for as long as the builder lives, which stays small for system
/// UI.
struct TextCache {
	std::vector<TextSlot> slots;
};

/// Which kept layout a text uses this frame, and at what width.
struct TextLaidOut {
	// The width the layout was made for, so Arrange can tell when the width it gives differs.
	float laid_out_width = 0.0f;
	// The entry of the text's slot, which is only meaningful once it was laid out this frame.
	uint8_t entry = 0;
	// Without it, a text not measured this frame would show what its slot kept from another.
	bool laid_out = false;
};

/// What every text laid out this frame uses, by the text's row in the text table. A frame workspace
/// of plain values, so Begin empties it and nothing from an earlier frame is read, without freeing
/// anything.
struct TextLaidOutRows {
	// One per text laid out this frame, at its row.
	std::vector<TextLaidOut> rows;
	/// Empties the rows and keeps their memory, as the frame asks of a frame workspace at Begin.
	void clear() { rows.clear(); }
	/// Whether no text has been laid out this frame.
	[[nodiscard]] bool empty() const { return rows.empty(); }
};

/// The layout `text` was laid out to this frame, or null for one not laid out this frame.
[[nodiscard]] const TextLayout* FindLaidOut(const FrameBuilder& frame, AnyElement text);

/// The drawing layer's view of `props`, which is all of it but what it asks of its container.
[[nodiscard]] DrawTextProps ToDrawTextProps(const TextProps& props);

/// Measures a text by laying it out in the width on offer, or in its own width when it sets one.
/// The width is the layout's rounded up to a whole pixel, and the least width depends on how the
/// text gives way.
[[nodiscard]] Measured MeasureText(FrameBuilder& frame, AnyElement text, Vec2 max);

/// Lays a text out again when the width it is given differs from the one it was measured at, so
/// ellipsis and wrapping follow the final width in the same frame. A text has nothing below it, so
/// this always succeeds.
bool ArrangeText(FrameBuilder& frame, AnyElement text, const Rect& rect);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_ELEMENT_PRIMITIVES_TEXT_LAYOUT_H_
