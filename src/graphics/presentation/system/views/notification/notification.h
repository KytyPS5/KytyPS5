#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_NOTIFICATION_NOTIFICATION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_NOTIFICATION_NOTIFICATION_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

#include <string>

namespace Libs::Graphics::SystemUi {

/// What a notice says.
struct NotificationProps {
	// How it went, which sets the colour of its band. A notice always has one, as it needs the band
	// and padding a plain toast lacks.
	Status      status = Status::Info;
	std::string text;
};

/// The sizes a notice draws with beyond its toast's, in one place, so anything that shows or
/// matches the notice reads the size its glyph is really drawn at.
struct NotificationTokens {
	// The status glyph's side, as tall as the text's first line so it fills that line without
	// reaching into the padding. The text keeps clear of it, and of the gap beside it.
	Dp glyph_size;
};

/// The theme's notice.
[[nodiscard]] NotificationTokens NotificationDefaults(const ThemeContext& theme);

/// A notice, its status glyph beside its text in the theme's small body text on a toast with its
/// status, the text wrapping to three lines and cut short after them. It reads the current theme,
/// so it needs an open FrameScope. If the frame can't build it, the result is a none handle.
[[nodiscard]] BoxElement Notification(const NotificationProps& props);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_VIEWS_NOTIFICATION_NOTIFICATION_H_
