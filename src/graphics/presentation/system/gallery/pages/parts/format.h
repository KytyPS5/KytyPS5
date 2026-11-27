#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_FORMAT_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_FORMAT_H_

#include "graphics/presentation/system/core.h"

#include <string>

// How the gallery writes a token's value under its name. Text copies what it is given into the
// frame, so these return strings that need only last for the call that shows them.
namespace Libs::Graphics::SystemUi::Gallery {

/// `color` as #RRGGBB, followed by its opacity as a whole percentage when it isn't opaque, such
/// as "#1E232E 96%".
[[nodiscard]] std::string HexText(Color color);

/// A length in design units as a whole number when it is one, such as "16", and to one place
/// otherwise, such as "0.5".
[[nodiscard]] std::string LengthText(float length);

/// A text style's size and line height, its weight and the least size it may shrink to, such as
/// "22 / 32 Regular, at least 14".
[[nodiscard]] std::string TypeMetricsText(const TypeStyle& style);

/// A duration in milliseconds, such as "180 ms".
[[nodiscard]] std::string MsText(float milliseconds);

/// A fraction from 0 to 1 as a whole percentage, such as "38%".
[[nodiscard]] std::string PercentText(float fraction);

/// A contrast ratio cut down to one place, never rounded up, so a ratio just under a target never
/// shows as reaching it, such as "4.4:1" for 4.478.
[[nodiscard]] std::string RatioText(float ratio);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_PAGES_PARTS_FORMAT_H_
