#include "graphics/presentation/system/views/notification/notification.h"

#include "graphics/presentation/system/components/glyphs/status.h"
#include "graphics/presentation/system/components/toast.h"
#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

namespace Libs::Graphics::SystemUi {

namespace {

// Enough lines for a long localized sentence, and no more, so a toast stays a notice.
constexpr int MAX_NOTICE_LINES = 3;

// The status glyph, filled, centred in a slot as tall as the text's first line, so it sits beside
// that line however many the text wraps to.
AnyElement GlyphSlot(const ThemeContext& theme, const NotificationTokens& notice, Status status) {
	return Box(
	    {.layout = {.justify = Justify::Center, .align = Align::Center},
	     .self   = {.layout = {.width = notice.glyph_size, .height = theme.type.body_small.line}}},
	    {StatusGlyph({.status = status, .size = notice.glyph_size, .fill = true})});
}

// The notice's text in a column between the toast's width limits less its padding, the glyph and
// the gap, so it wraps where the toast would without the glyph beside it. The padding is the
// banded toast's, the theme's on both sides, so a notice that patched the toast's tokens would
// need them here too.
AnyElement NoticeText(const ThemeContext& theme, const NotificationTokens& notice,
                      const std::string& text) {
	const ToastTokens tokens = ToastDefaults(theme);
	const Dp          inset = 2.0f * tokens.padding_horizontal + notice.glyph_size + theme.space.s3;
	const ItemLayout  width {.min_main = tokens.min_width - inset,
	                         .max_main = tokens.max_width - inset};
	return Box({.layout = {.direction = FlexDirection::Column, .align = Align::Stretch},
	            .self   = {.layout = width}},
	           {Text({.text = text,
	                  .self = {.styles = {.type      = theme.type.body_small,
	                                      .color     = theme.colors.text.primary,
	                                      .overflow  = TextOverflow::Wrap,
	                                      .max_lines = MAX_NOTICE_LINES}}})});
}

} // namespace

NotificationTokens NotificationDefaults(const ThemeContext& theme) {
	// As tall as the text's first line, the largest the glyph can be without reaching into the
	// toast's padding, and it follows the theme's type if that changes.
	return {.glyph_size = theme.type.body_small.line};
}

BoxElement Notification(const NotificationProps& props) {
	const ThemeContext&      theme  = CurrentThemeContext();
	const NotificationTokens notice = NotificationDefaults(theme);
	// The glyph leads, at the start edge, and keeps to the first line rather than the block's
	// middle, so it reads with the start of the sentence.
	return Toast(
	    {.status = props.status},
	    {Box({.layout = {.align = Align::Start, .gap = theme.space.s3}},
	         {GlyphSlot(theme, notice, props.status), NoticeText(theme, notice, props.text)})});
}

} // namespace Libs::Graphics::SystemUi
