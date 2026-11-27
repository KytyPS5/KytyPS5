#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_PAGE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_PAGE_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/element.h"

#include <cstddef>
#include <span>
#include <string_view>

// A gallery page and the pieces every page is built from, so the pages read as one library. Each
// needs an open FrameScope, as it builds elements in the current frame.
namespace Libs::Graphics::SystemUi::Gallery {

/// What a gallery page says about itself above its sections.
struct PageProps {
	// The page's name, as the sidebar lists it.
	std::string_view title;
	// The rule the page's subject follows, in a sentence or two.
	std::string_view subtitle;
};

/// A gallery page, its title and subtitle over `children`, padded and growing to fill the pane it
/// sits in, so every page starts the same way.
[[nodiscard]] BoxElement Page(const PageProps& props, const Children& children);

/// A section's title in the theme's label style.
[[nodiscard]] AnyElement SectionTitle(const ThemeContext& theme, std::string_view title);

/// A section of a page, its title over `content`.
[[nodiscard]] AnyElement Section(const ThemeContext& theme, std::string_view title,
                                 AnyElement content);

/// `shown` with `caption` centred below it in `caption_color`, so a caption on a bright frame can
/// take a dark colour and still match every other caption.
[[nodiscard]] AnyElement Captioned(const ThemeContext& theme, AnyElement shown,
                                   std::string_view caption, Color caption_color);

/// `shown` with `caption` centred below it in secondary text.
[[nodiscard]] AnyElement Captioned(const ThemeContext& theme, AnyElement shown,
                                   std::string_view caption);

/// A token's name in secondary caption text over its value in tertiary caption text, both at
/// `width`. The name keeps to one line and is clipped at the edge if it is too long, so values
/// line up side by side, and the value is cut short with an ellipsis if it doesn't fit.
[[nodiscard]] AnyElement NameAndValue(const ThemeContext& theme, std::string_view name,
                                      std::string_view value, float width);

/// How wide a token's name is in a token line, in design units, room for the longest names.
inline constexpr float TOKEN_NAME_WIDTH = 320.0f;

/// `text` in caption type and `color`, laid out at `width` and cut short with an ellipsis if it
/// doesn't fit, as a table's cells are.
[[nodiscard]] AnyElement CaptionCell(const ThemeContext& theme, std::string_view text, float width,
                                     Color color);

/// A token's name in secondary caption text at `name_width`, then its value in primary caption
/// text, on one line.
[[nodiscard]] AnyElement TokenLine(const ThemeContext& theme, std::string_view name,
                                   std::string_view value, float name_width = TOKEN_NAME_WIDTH);

/// `lines` stacked in a column, a small gap apart.
[[nodiscard]] AnyElement Lines(const ThemeContext& theme, const Children& lines);

/// `items` in rows of at most `columns`, top to bottom, as the layout has no wrapping. Fewer than
/// one column is taken as one.
[[nodiscard]] AnyElement Grid(const ThemeContext& theme, const Children& items, size_t columns);

/// `specimen` beside `tokens`, the values that make it, with room between.
[[nodiscard]] AnyElement BesideTokens(const ThemeContext& theme, AnyElement specimen,
                                      AnyElement tokens);

/// How far below an element `shadow` reaches, the most any layer's offset, blur and spread add
/// up to, in design units.
[[nodiscard]] float ShadowReach(const Shadow& shadow);

/// `shadow` under a sample of `size` filled with `fill`, captioned `caption`, beside the same
/// sample with none, in a well of the overlay surface, as the near black shadow barely shows on
/// the gallery's dark backdrop. The well leaves room below for as far as the shadow reaches, so it
/// stays inside it.
[[nodiscard]] AnyElement ShadowWell(const ThemeContext& theme, const Shadow& shadow,
                                    std::string_view caption, Color fill, const ItemLayout& size);

} // namespace Libs::Graphics::SystemUi::Gallery

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_GALLERY_COMPONENTS_PAGE_H_
