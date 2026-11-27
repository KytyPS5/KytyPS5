#include "graphics/presentation/system/element/primitives/text/layout.h"

#include "graphics/presentation/system/core/units.h"
#include "graphics/presentation/system/element/frame/frameBuilder.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string_view>

namespace Libs::Graphics::SystemUi {

namespace {

// A text laid out for a width this close to the one it is given needs no new layout. Arranging
// snaps widths to whole pixels, so anything under half a pixel is rounding and not a new width.
constexpr float RELAYOUT_EPSILON = 0.5f;

// How wide `text` is in `layout`'s font and size, on one line.
float LineWidth(const TextLayout& layout, std::string_view text) {
	// Empty text may have no data, and ImGui would measure that up to a terminator it doesn't have.
	if (text.empty()) {
		return 0.0f;
	}
	return layout.font
	    ->CalcTextSizeA(layout.font_size, FLT_MAX, 0.0f, text.data(), text.data() + text.size())
	    .x;
}

// How wide the widest unbreakable word of `text` is. Words end at spaces and line breaks, so this
// is the least width the text can take without a word being cut. ImGui also breaks at tabs, after
// punctuation including CJK punctuation, and inside a word too long for a line, which this doesn't
// follow yet, so such text reports a larger least width than it could wrap to.
float LongestWord(const TextLayout& layout, std::string_view text) {
	float widest = 0.0f;
	for (size_t start = 0; start < text.size();) {
		const size_t end = std::min(text.find_first_of(" \n", start), text.size());
		if (end > start) {
			widest = std::max(widest, LineWidth(layout, text.substr(start, end - start)));
		}
		start = end + 1;
	}
	return widest;
}

// The least width a truncated text can take, its ellipsis, unless its first line is narrower still,
// as a text that fits is never cut short. Its own width is measured rather than taken from the
// layout, which is empty at a limit of nothing.
float TruncatedWidth(const TextLayout& layout, std::string_view text) {
	const std::string_view first_line = text.substr(0, text.find('\n'));
	return std::min(EllipsisWidth(layout.font, layout.font_size), LineWidth(layout, first_line));
}

// The least width a text can take, which depends on how it gives way. Wrapped text can't go below
// its longest word. Truncated text can go down to its ellipsis, and clipped text to nothing, as CSS
// lets text that truncates or clips shrink below its content.
float MinimumWidth(const TextLayout& layout, const TextProps& props) {
	if (layout.font == nullptr) {
		return 0.0f;
	}
	switch (props.self.styles.overflow) {
		case TextOverflow::Wrap: return LongestWord(layout, props.text);
		case TextOverflow::Ellipsis: return TruncatedWidth(layout, props.text);
		case TextOverflow::Clip: break;
	}
	return 0.0f;
}

// The element at `index` of `items`, growing it to reach that far.
template <class T>
T& Grown(std::vector<T>& items, uint32_t index) {
	if (index >= items.size()) {
		items.resize(index + 1);
	}
	return items[index];
}

// Makes the layout of `props` in `limit` pixels the one the text at `payload` uses this frame,
// reusing one its slot kept when the text and everything it depends on are the same, and returns
// it. Making one is the only step that allocates and measures glyphs, so a steady frame does
// neither.
const CachedTextLayout& LayOut(FrameBuilder& frame, uint32_t payload, const TextProps& props,
                               float limit) {
	const DrawTextProps draw = ToDrawTextProps(props);
	const TextLayoutKey key  = MakeTextLayoutKey(draw, limit);
	TextSlot&           slot = Grown(frame.Workspace<TextCache>().slots, payload);
	TextLaidOut&        row  = Grown(frame.FrameWorkspace<TextLaidOutRows>().rows, payload);
	row.laid_out             = true;
	row.laid_out_width       = limit;
	++slot.uses;
	// A miss replaces the entry used longest ago, and an entry never filled was never used.
	uint8_t stalest = 0;
	for (uint8_t index = 0; index < slot.entries.size(); ++index) {
		CachedTextLayout& entry = slot.entries[index];
		if (entry.used && entry.key == key && entry.text == props.text) {
			entry.last_use = slot.uses;
			row.entry      = index;
			return entry;
		}
		if (entry.last_use < slot.entries[stalest].last_use) {
			stalest = index;
		}
	}
	row.entry              = stalest;
	CachedTextLayout& made = slot.entries[stalest];
	made.used              = true;
	made.last_use          = slot.uses;
	made.key               = key;
	made.text.assign(props.text);
	made.layout    = LayoutText(draw, limit);
	made.min_width = MinimumWidth(made.layout, props);
	return made;
}

} // namespace

const TextLayout* FindLaidOut(const FrameBuilder& frame, AnyElement text) {
	const FrameView::ElementMetadata* const metadata = frame.Find(text);
	const TextLaidOutRows* const            rows     = frame.FindFrameWorkspace<TextLaidOutRows>();
	const TextCache* const                  cache    = frame.FindWorkspace<TextCache>();
	if (metadata == nullptr || rows == nullptr || cache == nullptr ||
	    metadata->payload >= rows->rows.size() || metadata->payload >= cache->slots.size()) {
		return nullptr;
	}
	const TextLaidOut& row = rows->rows[metadata->payload];
	return row.laid_out ? &cache->slots[metadata->payload].entries[row.entry].layout : nullptr;
}

DrawTextProps ToDrawTextProps(const TextProps& props) {
	return {.text = props.text, .styles = props.self.styles};
}

Measured MeasureText(FrameBuilder& frame, AnyElement text, Vec2 max) {
	const TextProps* const                  props    = frame.FindAs<TextProps>(text);
	const FrameView::ElementMetadata* const metadata = frame.Find(text);
	if (props == nullptr || metadata == nullptr) {
		return {};
	}
	// A copy, as Box takes, since the frame's tables stay put only while nothing is added.
	const ItemLayout        item     = props->self.layout;
	const float             limit    = item.width ? ToPixels(*item.width) : max.x;
	const CachedTextLayout& laid_out = LayOut(frame, metadata->payload, *props, limit);
	// Widths rounded up to a whole pixel. A container rounds each edge of its children to a whole
	// pixel, which keeps a whole width exactly but can take most of a pixel off a fractional one,
	// and text arranged even that much narrower lays out again and is cut short.
	const float  taken = std::ceil(laid_out.layout.size.x);
	const float  least = std::ceil(laid_out.min_width);
	const float  width = std::isfinite(limit) ? std::min(limit, taken) : taken;
	TextLaidOut& row   = frame.FrameWorkspace<TextLaidOutRows>().rows[metadata->payload];
	// Text narrower than its limit lays out the same at its own width, so recording that width
	// spares Arrange a new layout when it gives the text exactly the width it took. A text with an
	// explicit width keeps that width, which Arrange gives it again.
	if (!item.width) {
		row.laid_out_width = width;
	}
	Vec2 size {width, laid_out.layout.size.y};
	// Text can't shrink vertically, so its least height is the height it laid out to.
	Vec2 min {least, size.y};
	if (item.width) {
		size.x = min.x = limit;
	}
	if (item.height) {
		size.y = min.y = ToPixels(*item.height);
	}
	return {size, min};
}

bool ArrangeText(FrameBuilder& frame, AnyElement text, const Rect& rect) {
	const TextProps* const                  props    = frame.FindAs<TextProps>(text);
	const FrameView::ElementMetadata* const metadata = frame.Find(text);
	if (props == nullptr || metadata == nullptr) {
		return true;
	}
	const std::vector<TextLaidOut>& rows = frame.FrameWorkspace<TextLaidOutRows>().rows;
	const bool measured = metadata->payload < rows.size() && rows[metadata->payload].laid_out;
	if (measured &&
	    std::fabs(rect.width - rows[metadata->payload].laid_out_width) <= RELAYOUT_EPSILON) {
		return true;
	}
	(void)LayOut(frame, metadata->payload, *props, rect.width);
	return true;
}

} // namespace Libs::Graphics::SystemUi
