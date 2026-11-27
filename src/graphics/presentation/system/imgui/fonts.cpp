#include "graphics/presentation/system/imgui/fonts.h"

#include "common/logging/log.h"
#include "imgui.h"

#include <cfloat>
#include <string>

namespace Libs::Graphics::SystemUi {

namespace {

// Adds the font at `path`, then its fallbacks. Returns null, after saying why, when the file is
// missing or ImGui can't load it. ImGui opens the path as UTF-8, so install folders with
// non-ASCII names work, and its no-error flag makes a missing file return null.
ImFont* LoadFont(ImFontAtlas& atlas, const std::string& path,
                 const std::function<void()>& merge_fallbacks) {
	ImFontConfig config;
	config.Flags = ImFontFlags_NoLoadError;
	ImFont* font = atlas.AddFontFromFileTTF(path.c_str(), 0.0f, &config);
	if (font == nullptr) {
		LOGF("[SystemUi] font could not be loaded: %s\n", path.c_str());
		return nullptr;
	}
	if (merge_fallbacks) {
		merge_fallbacks();
	}
	return font;
}

// Printable ASCII, then the ellipsis that truncated text ends with.
constexpr char     FIRST_PRINTABLE = ' ';
constexpr char     LAST_PRINTABLE  = '~';
constexpr char     ELLIPSIS[]      = "\xE2\x80\xA6";
constexpr unsigned STYLE_COUNT     = 9;

} // namespace

FontsContext LoadFontsContext(ImFontAtlas& atlas, const std::string& regular_path,
                              const std::string&           bold_path,
                              const std::function<void()>& merge_fallbacks) {
	FontsContext fonts;
	fonts.regular = LoadFont(atlas, regular_path, merge_fallbacks);
	fonts.bold    = LoadFont(atlas, bold_path, merge_fallbacks);
	return fonts;
}

void PrebakeGlyphs() {
	const ContextContainer& context = CurrentContext();
	const FontsContext&     fonts   = context.fonts;
	const TypeRamp&         ramp    = context.theme.type;
	const ScaleContext&     scale   = context.scale;

	std::string glyphs;
	for (char glyph = FIRST_PRINTABLE; glyph <= LAST_PRINTABLE; glyph++) {
		glyphs.push_back(glyph);
	}
	glyphs += ELLIPSIS;

	const TypeStyle styles[STYLE_COUNT] = {ramp.display, ramp.title,      ramp.heading,
	                                       ramp.body,    ramp.body_small, ramp.label,
	                                       ramp.caption, ramp.dense,      ramp.dense_small};

	for (const TypeStyle& style: styles) {
		ImFont* const font = fonts.For(style.weight);
		if (font != nullptr) {
			// Measuring is what bakes the glyphs, so the width isn't needed.
			(void)font->CalcTextSizeA(scale.FontSize(style), FLT_MAX, 0.0f, glyphs.data(),
			                          glyphs.data() + glyphs.size());
		}
	}
}

} // namespace Libs::Graphics::SystemUi
