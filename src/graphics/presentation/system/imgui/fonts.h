#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_FONTS_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_FONTS_H_

#include "graphics/presentation/system/core/context.h"

#include <functional>
#include <string>

struct ImFontAtlas;

namespace Libs::Graphics::SystemUi {

/// Adds the theme's regular and bold fonts from `regular_path` and `bold_path` to `atlas`. The
/// fonts are sized when drawn, so each is added once for every size. After each font is added,
/// `merge_fallbacks`, when given, may add fonts in merge mode to cover scripts the font lacks,
/// such as Chinese, Japanese, Korean, Thai or Arabic. A file that is missing or can't be loaded
/// leaves that weight null, and is reported in the log rather than failing.
[[nodiscard]] FontsContext LoadFontsContext(ImFontAtlas& atlas, const std::string& regular_path,
                                            const std::string&           bold_path,
                                            const std::function<void()>& merge_fallbacks = {});

/// Bakes the printable ASCII glyphs and the ellipsis of every type style in the current theme, each
/// in its own weight from the current fonts, at the pixel sizes the current scale gives them, in a
/// fixed order. It needs an open ContextScope. ImGui otherwise bakes a glyph the first time it is
/// measured or drawn, and packs it into the atlas in that order, so the pixels around a glyph's
/// edges depend on which text happened to be asked for first. Call it before the first frame where
/// two renderings must match exactly, as the tests and screenshots do. The shipping overlay
/// doesn't, since nothing there compares frames.
void PrebakeGlyphs();

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_FONTS_H_
