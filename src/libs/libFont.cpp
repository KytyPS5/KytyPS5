#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267 4701)
#endif
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wsign-compare"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

extern "C" {
extern const uint8_t avpriv_vga16_font[4096];
}

namespace Libs {

LIB_VERSION("Font", 1, "Font", 1, 1);
namespace Font {

using FontLibrarySelection      = void*;
using FontLibrary               = void*;
using FontHandle                = void*;
using FontRendererSelection     = void*;
using FontRenderer              = void*;
using FontMemoryDestroyCallback = void (*)(void* font_memory, void* object, void* destroy_arg);
using FontMemoryInterface       = void;
using FontOpenDetail            = void;
using FontGenerateGlyphDetail   = void;
using FontString                = void*;
using FontTextCharacter         = const void*;
using FontRenderCharacter       = const void*;
using FontWritingLine           = void*;

struct FontTextSource;
union FontTextParseResult;
using FontTextParseFunction = KYTY_SYSV_ABI int (*)(FontTextSource* font_text_source, void** order,
                                                    FontTextParseResult* result);
using FontCreateStringOrdersFunction = KYTY_SYSV_ABI void (*)(void*       order_callback_object,
                                                              uint32_t    order_sheet_form,
                                                              const void* order_sheet);

constexpr int SCE_FONT_TEXT_PARSER_RESULT_FONT_CODE = 1;
constexpr int SCE_FONT_TEXT_PARSER_RESULT_TERMINATE = 0;
constexpr int SCE_FONT_TEXT_PARSER_RESULT_ERROR     = -1;
constexpr int SCE_FONT_WRITING_FORM_HORIZONTAL      = 0x10;
constexpr int FONT_BITMAP_MAX_DIM                   = 128;

struct FontMemory {
	uint16_t                   type;
	uint16_t                   attr;
	uint32_t                   size;
	void*                      address;
	void*                      mspace_object;
	const FontMemoryInterface* mem_interface;
	FontMemoryDestroyCallback  destroy_callback;
	void*                      destroy_object;
	void*                      user_object;
	void*                      parent_object;
};

struct LibraryState {
	const FontMemory* memory;
	uint64_t          edition;
};

struct RendererState {
	const FontMemory*     memory;
	FontRendererSelection selection;
	uint64_t              edition;
};

struct FontGlyphMetrics {
	float width;
	float height;
	struct {
		float bearing_x;
		float bearing_y;
		float advance;
	} horizontal;
	struct {
		float bearing_x;
		float bearing_y;
		float advance;
	} vertical;
};

struct FontRenderSurface {
	void*   buffer;
	int32_t width_byte;
	int8_t  pixel_size_byte;
	uint8_t system_ext0;
	uint8_t system_ext1;
	uint8_t system_ext2;
	int32_t width;
	int32_t height;
	struct {
		uint32_t x0;
		uint32_t y0;
		uint32_t x1;
		uint32_t y1;
	} scissor;
	uint32_t system_use[22];
};

struct FontTransImage {
	uint8_t* address;
	uint32_t width_byte;
	uint32_t image_width;
	uint32_t image_height;
};

struct FontSurfaceImage {
	uint8_t* address;
	uint32_t width_byte;
	uint8_t  pixel_size_byte;
	uint8_t  pixel_format;
};

struct FontGlyphImageMetrics {
	float    bearing_x;
	float    bearing_y;
	float    advance;
	float    stride;
	uint32_t width;
	uint32_t height;
};

struct FontRenderResult {
	const FontTransImage* trans_image;
	FontSurfaceImage      surface_image;
	struct {
		uint32_t x;
		uint32_t y;
		uint32_t w;
		uint32_t h;
	} update_rect;
	FontGlyphImageMetrics image_metrics;
};

struct FontState {
	FontLibrary                                                    library;
	const void*                                                    data;
	uint32_t                                                       size;
	uint32_t                                                       font_set_type;
	uint32_t                                                       open_mode;
	int                                                            attribute;
	FontRenderer                                                   renderer;
	float                                                          scale_w;
	float                                                          scale_h;
	float                                                          effect_weight_x;
	float                                                          effect_weight_y;
	uint32_t                                                       effect_weight_mode;
	float                                                          effect_slant;
	float                                                          render_scale_w;
	float                                                          render_scale_h;
	float                                                          render_effect_weight_x;
	float                                                          render_effect_weight_y;
	uint32_t                                                       render_effect_weight_mode;
	float                                                          render_effect_slant;
	stbtt_fontinfo                                                 font_info;
	const uint8_t*                                                 ttf_data;
	uint32_t                                                       ttf_size;
	bool                                                           has_ttf;
	std::array<uint8_t, FONT_BITMAP_MAX_DIM * FONT_BITMAP_MAX_DIM> fallback_image;
	FontTransImage                                                 trans_image;
};

struct FontHorizontalLayout {
	float base_line_y;
	float line_height;
	float effect_height;
};

struct FontVerticalLayout {
	float base_line_x;
	float line_width;
	float effect_width;
};

struct GlyphState {
	FontState*                                                     font;
	uint32_t                                                       code;
	int                                                            attribute;
	FontGlyphMetrics                                               metrics;
	std::array<uint8_t, FONT_BITMAP_MAX_DIM * FONT_BITMAP_MAX_DIM> image;
	FontTransImage                                                 trans_image;
};

union FontTextParseResult {
	void* reserved[4];

	struct {
		FontHandle font;
		uint32_t   code;
	} font_code;

	struct {
		void*    reserved;
		uint32_t terminate_code;
	} terminate;

	struct {
		void* reserved;
		int   error_code;
	} error;
};

struct FontTextSource {
	uint64_t              system_use0;
	const void*           start;
	const void*           end;
	const void*           current;
	FontTextParseFunction text_parser;
	void*                 text_object;
	FontHandle            default_font;
	void*                 system_use[5];
};

struct FontCreateStringDetail {
	uint16_t   detail_id;
	uint8_t    detail_type;
	uint8_t    detections;
	uint32_t   orders_option;
	FontHandle default_font;
	struct {
		FontCreateStringOrdersFunction function;
		void*                          object;
	} orders;
};

struct FontStringState;

struct FontCharacterState {
	FontStringState* string;
	uint32_t         index;
	FontHandle       font;
	uint32_t         code;
	void*            order;
};

struct FontStringState {
	const FontMemory*               memory;
	int                             writing_form;
	uint32_t                        terminate_code;
	void*                           terminate_order;
	std::vector<FontCharacterState> characters;
};

union FontTextCodes {
	struct {
		void*    order;
		uint32_t code;
		uint32_t reserved;
	} text;
	void* system_reserved[8];
};

struct FontWriting {
	void* system_use[32];
};

struct FontWritingExtent {
	float top;
	float bottom;
	float left;
	float right;
};

struct FontWritingMetrics {
	float             advance_x;
	float             advance_y;
	FontWritingExtent extent;
};

struct FontWritingStep {
	float      x;
	float      y;
	float      advance_x;
	float      advance_y;
	FontHandle font;
	struct {
		uint32_t character_count : 8;
		uint32_t invisible_glyph : 1;
	} profile;
	uint32_t glyph_code;
	struct {
		float x;
		float y;
	} positioning;
	FontGlyphMetrics glyph_metrics;
};

struct FontWritingLetterStep {
	float x;
	float y;
	float advance_x;
	float advance_y;
	struct {
		uint32_t texts_count;
		uint32_t texts_index;
		uint32_t glyphs_count;
		uint32_t glyphs_index;
		uint8_t  base_components_count;
		uint8_t  base_components_index;
		uint8_t  base_focus_count;
		uint8_t  base_focus_index;
		uint8_t  opposite_direction;
		uint8_t  character_text_count;
		uint8_t  base_text_count;
		uint8_t  mark_text_count;
		uint32_t marks_text_count;
		uint32_t marks_text_number;
		uint32_t reserved[3];
		int32_t  text_letter_offset;
	} components;
};

struct FontWritingLineStep {
	float x;
	float y;
	float advance_x;
	float advance_y;
	float spacing_progress;
	void* writing_orderer;
	struct {
		float x;
		float y;
	} adjusting;
	FontWritingMetrics metrics;
};

struct FontWritingState {
	FontStringState*      string;
	FontCharacterState*   character;
	FontWritingStep       step;
	FontWritingLetterStep letter_step;
	FontWritingMetrics    metrics;
	int                   invisible_mask;
};

struct FontWritingLineState {
	const FontMemory*                memory;
	int                              writing_form;
	std::vector<FontWritingLineStep> steps;
	size_t                           cursor;
	FontWritingMetrics               metrics;
};

static std::mutex                g_glyph_mutex;
static std::unordered_set<void*> g_glyphs;
static std::mutex                g_writing_mutex;
static std::unordered_set<void*> g_writing_states;

static void register_glyph(GlyphState* glyph) {
	std::scoped_lock lock(g_glyph_mutex);
	g_glyphs.insert(glyph);
}

static bool unregister_glyph(void* glyph) {
	std::scoped_lock lock(g_glyph_mutex);
	return g_glyphs.erase(glyph) != 0;
}

static bool is_known_glyph(void* glyph) {
	std::scoped_lock lock(g_glyph_mutex);
	return g_glyphs.find(glyph) != g_glyphs.end();
}

static void register_writing_state(FontWritingState* state) {
	std::scoped_lock lock(g_writing_mutex);
	g_writing_states.insert(state);
}

static bool unregister_writing_state(void* state) {
	std::scoped_lock lock(g_writing_mutex);
	return g_writing_states.erase(state) != 0;
}

static bool is_known_writing_state(void* state) {
	std::scoped_lock lock(g_writing_mutex);
	return g_writing_states.find(state) != g_writing_states.end();
}

static int get_text_source_writing_form(const FontTextSource* source) {
	if (source == nullptr) {
		return SCE_FONT_WRITING_FORM_HORIZONTAL;
	}

	const auto value = reinterpret_cast<uintptr_t>(source->system_use[0]);
	return value != 0 ? static_cast<int>(value) : SCE_FONT_WRITING_FORM_HORIZONTAL;
}

static void set_text_source_writing_form(FontTextSource* source, int writing_form) {
	if (source != nullptr) {
		source->system_use[0] = reinterpret_cast<void*>(static_cast<uintptr_t>(writing_form));
	}
}

static void append_character(FontStringState* string, FontHandle font, uint32_t code, void* order) {
	if (string == nullptr) {
		return;
	}

	const auto index = static_cast<uint32_t>(string->characters.size());
	string->characters.push_back(FontCharacterState {string, index, font, code, order});
}

static FontHandle get_string_default_font(const FontTextSource*         source,
                                          const FontCreateStringDetail* detail) {
	if (detail != nullptr && detail->default_font != nullptr) {
		return detail->default_font;
	}
	return source != nullptr ? source->default_font : nullptr;
}

static void parse_text_bytes(FontTextSource* source, FontHandle default_font,
                             FontStringState* string) {
	if (source == nullptr || source->start == nullptr || source->end == nullptr) {
		return;
	}

	auto* ptr = static_cast<const uint8_t*>(source->start);
	auto* end = static_cast<const uint8_t*>(source->end);

	while (ptr < end) {
		const auto* code_start = ptr;
		uint32_t    code       = *ptr++;

		if ((code & 0x80u) != 0u) {
			const uint32_t first = code;
			if ((first & 0xe0u) == 0xc0u && ptr < end) {
				code = ((first & 0x1fu) << 6u) | (*ptr++ & 0x3fu);
			} else if ((first & 0xf0u) == 0xe0u && ptr + 1 < end) {
				code = ((first & 0x0fu) << 12u) | ((ptr[0] & 0x3fu) << 6u) | (ptr[1] & 0x3fu);
				ptr += 2;
			} else if ((first & 0xf8u) == 0xf0u && ptr + 2 < end) {
				code = ((first & 0x07u) << 18u) | ((ptr[0] & 0x3fu) << 12u) |
				       ((ptr[1] & 0x3fu) << 6u) | (ptr[2] & 0x3fu);
				ptr += 3;
			}
		}

		append_character(string, default_font, code, const_cast<uint8_t*>(code_start));
	}

	source->current = end;
}

static int parse_text_with_callback(FontTextSource* source, FontHandle default_font,
                                    FontStringState* string) {
	if (source == nullptr || source->text_parser == nullptr) {
		parse_text_bytes(source, default_font, string);
		return OK;
	}

	constexpr uint32_t max_font_string_characters = 65536;

	for (uint32_t i = 0; i < max_font_string_characters; i++) {
		FontTextParseResult result {};
		void*               order = nullptr;
		const int           ret   = source->text_parser(source, &order, &result);

		if (ret == SCE_FONT_TEXT_PARSER_RESULT_FONT_CODE) {
			auto font = result.font_code.font != nullptr ? result.font_code.font : default_font;
			append_character(string, font, result.font_code.code, order);
			continue;
		}

		if (ret == SCE_FONT_TEXT_PARSER_RESULT_TERMINATE) {
			string->terminate_code  = result.terminate.terminate_code;
			string->terminate_order = order != nullptr ? order : result.terminate.reserved;
			if (string->characters.empty() && source->start != source->end) {
				parse_text_bytes(source, default_font, string);
			}
			return OK;
		}

		if (ret == SCE_FONT_TEXT_PARSER_RESULT_ERROR) {
			return result.error.error_code != 0 ? result.error.error_code : -1;
		}

		return -1;
	}

	return OK;
}

static FontCharacterState* get_character(FontTextCharacter character) {
	return const_cast<FontCharacterState*>(static_cast<const FontCharacterState*>(character));
}

static FontCharacterState* get_next_character(FontCharacterState* character) {
	if (character == nullptr || character->string == nullptr) {
		return nullptr;
	}

	const auto next = character->index + 1;
	return next < character->string->characters.size() ? &character->string->characters[next]
	                                                   : nullptr;
}

static FontCharacterState* get_prev_character(FontCharacterState* character) {
	if (character == nullptr || character->string == nullptr || character->index == 0) {
		return nullptr;
	}

	return &character->string->characters[character->index - 1];
}

static uint32_t count_characters_until(FontCharacterState* first, FontCharacterState* last) {
	if (first == nullptr || first->string == nullptr) {
		return 0;
	}

	uint32_t count = 0;
	for (auto* c = first; c != nullptr; c = get_next_character(c)) {
		if (last != nullptr && c == last) {
			break;
		}
		count++;
	}
	return count;
}

static void fill_text_codes(FontTextCodes* text_codes, FontCharacterState* character,
                            FontCharacterState* term_character) {
	if (text_codes == nullptr || character == nullptr) {
		return;
	}

	std::memset(text_codes, 0, sizeof(FontTextCodes));
	text_codes->text.order         = character->order;
	text_codes->text.code          = character->code;
	text_codes->system_reserved[2] = character;
	text_codes->system_reserved[3] = term_character;
}

static void fill_metrics(FontGlyphMetrics* metrics, const FontState* font = nullptr);
static void fill_metrics_for_code(FontGlyphMetrics* metrics, const FontState* font, uint32_t code);

static void fill_writing_metrics(FontWritingMetrics* metrics, uint32_t character_count) {
	if (metrics == nullptr) {
		return;
	}

	const float advance    = static_cast<float>(character_count) * 8.0f;
	metrics->advance_x     = advance;
	metrics->advance_y     = 0.0f;
	metrics->extent.top    = -12.0f;
	metrics->extent.bottom = 4.0f;
	metrics->extent.left   = 0.0f;
	metrics->extent.right  = advance;
}

static void fill_writing_step(FontWritingStep* step, FontCharacterState* character) {
	if (step == nullptr) {
		return;
	}

	std::memset(step, 0, sizeof(FontWritingStep));
	step->advance_x               = 8.0f;
	step->advance_y               = 0.0f;
	step->font                    = character != nullptr ? character->font : nullptr;
	step->profile.character_count = character != nullptr ? 1u : 0u;
	step->profile.invisible_glyph = 0u;
	step->glyph_code              = character != nullptr ? character->code : 0u;
	fill_metrics_for_code(&step->glyph_metrics,
	                      static_cast<FontState*>(character != nullptr ? character->font : nullptr),
	                      character != nullptr ? character->code : static_cast<uint32_t>('?'));
}

static void fill_writing_letter_step(FontWritingLetterStep* letter_step,
                                     FontCharacterState*    character) {
	if (letter_step == nullptr) {
		return;
	}

	std::memset(letter_step, 0, sizeof(FontWritingLetterStep));
	letter_step->advance_x = 8.0f;
	letter_step->advance_y = 0.0f;

	if (character == nullptr) {
		return;
	}

	letter_step->components.texts_count           = 1;
	letter_step->components.texts_index           = character->index;
	letter_step->components.glyphs_count          = 1;
	letter_step->components.glyphs_index          = 0;
	letter_step->components.base_components_count = 1;
	letter_step->components.base_focus_count      = 1;
	letter_step->components.character_text_count  = 1;
	letter_step->components.base_text_count       = 1;
}

static FontWritingState* get_writing_state(FontWriting* font_writing) {
	if (font_writing == nullptr) {
		return nullptr;
	}

	auto* state = static_cast<FontWritingState*>(font_writing->system_use[0]);
	return is_known_writing_state(state) ? state : nullptr;
}

static void clear_writing_line(FontWritingLineState* line) {
	if (line == nullptr) {
		return;
	}

	line->steps.clear();
	line->cursor = 0;
	fill_writing_metrics(&line->metrics, 0);
}

static bool font_is_bold(const FontState* font) {
	if (font == nullptr) {
		return false;
	}
	return (font->font_set_type & 0x0fu) >= 0x07u || font->effect_weight_x > 0.0f ||
	       font->render_effect_weight_x > 0.0f;
}

static bool font_is_italic(const FontState* font) {
	if (font == nullptr) {
		return false;
	}
	return (font->font_set_type & 0x00100000u) != 0 || font->effect_slant != 0.0f ||
	       font->render_effect_slant != 0.0f;
}

static const char* fallback_font_file_name(bool bold, bool italic) {
	if (bold && italic) {
		return "Roboto-BoldItalic.ttf";
	}
	if (bold) {
		return "Roboto-Bold.ttf";
	}
	if (italic) {
		return "Roboto-Italic.ttf";
	}
	return "Roboto-Regular.ttf";
}

static std::vector<uint8_t> read_file_bytes(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file) {
		return {};
	}

	const auto size = file.tellg();
	if (size <= 0) {
		return {};
	}

	std::vector<uint8_t> data(static_cast<size_t>(size));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(data.data()), size);
	if (!file) {
		return {};
	}

	return data;
}

static std::vector<std::filesystem::path> fallback_font_candidates(const char* file_name) {
	const auto rel =
	    std::filesystem::path("3rdparty") / "tracy" / "profiler" / "src" / "font" / file_name;
	std::vector<std::filesystem::path> paths;

	auto source_root = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
	paths.push_back(source_root / rel);

	std::error_code ec;
	auto            cwd = std::filesystem::current_path(ec);
	if (!ec) {
		paths.push_back(cwd / rel);
		paths.push_back(cwd / ".." / rel);
		paths.push_back(cwd / ".." / ".." / rel);
	}

	return paths;
}

static std::vector<uint8_t> load_fallback_font_data(bool bold, bool italic) {
	for (const auto& path: fallback_font_candidates(fallback_font_file_name(bold, italic))) {
		auto data = read_file_bytes(path);
		if (!data.empty()) {
			return data;
		}
	}

	return {};
}

static const std::vector<uint8_t>& fallback_font_data(bool bold, bool italic) {
	static const auto regular     = load_fallback_font_data(false, false);
	static const auto bold_data   = load_fallback_font_data(true, false);
	static const auto italic_data = load_fallback_font_data(false, true);
	static const auto bold_italic = load_fallback_font_data(true, true);

	if (bold && italic && !bold_italic.empty()) {
		return bold_italic;
	}
	if (bold && !bold_data.empty()) {
		return bold_data;
	}
	if (italic && !italic_data.empty()) {
		return italic_data;
	}
	return regular;
}

static bool ttf_data_has_supported_magic(const uint8_t* data, uint32_t size) {
	if (data == nullptr || size < 12) {
		return false;
	}

	return (data[0] == 0x00 && data[1] == 0x01 && data[2] == 0x00 && data[3] == 0x00) ||
	       (data[0] == 't' && data[1] == 'r' && data[2] == 'u' && data[3] == 'e');
}

static bool init_stb_font(FontState* font, const uint8_t* data, uint32_t size) {
	if (font == nullptr || !ttf_data_has_supported_magic(data, size)) {
		return false;
	}

	stbtt_fontinfo info {};
	if (stbtt_InitFont(&info, data, 0) == 0) {
		return false;
	}

	font->font_info = info;
	font->ttf_data  = data;
	font->ttf_size  = size;
	font->has_ttf   = true;

	return true;
}

static void init_font_face(FontState* font) {
	if (font == nullptr) {
		return;
	}

	font->ttf_data = nullptr;
	font->ttf_size = 0;
	font->has_ttf  = false;

	if (init_stb_font(font, static_cast<const uint8_t*>(font->data), font->size)) {
		return;
	}

	const auto& data = fallback_font_data(font_is_bold(font), font_is_italic(font));
	if (!data.empty()) {
		(void)init_stb_font(font, data.data(), static_cast<uint32_t>(data.size()));
	}
}

static bool ensure_stb_font(FontState* font) {
	if (font == nullptr) {
		return false;
	}
	if (!font->has_ttf) {
		init_font_face(font);
	}
	return font->has_ttf;
}

static float stb_font_pixel_height(const FontState* font) {
	const float scale = (font != nullptr && font->scale_h > 1.0f ? font->scale_h : 16.0f);
	return static_cast<float>(std::clamp(static_cast<int>(scale + 0.5f), 8, FONT_BITMAP_MAX_DIM));
}

static float stb_font_scale(FontState* font) {
	return stbtt_ScaleForPixelHeight(&font->font_info, stb_font_pixel_height(font));
}

static uint32_t stb_supported_codepoint(FontState* font, uint32_t code) {
	if (font == nullptr || !ensure_stb_font(font)) {
		return static_cast<uint32_t>('?');
	}
	if (code > 0x10ffffu || stbtt_FindGlyphIndex(&font->font_info, static_cast<int>(code)) == 0) {
		return static_cast<uint32_t>('?');
	}
	return code;
}

static bool get_stb_metrics(FontState* font, uint32_t code, FontGlyphMetrics* metrics) {
	if (metrics == nullptr || !ensure_stb_font(font)) {
		return false;
	}

	code              = stb_supported_codepoint(font, code);
	const float scale = stb_font_scale(font);

	int advance = 0;
	int lsb     = 0;
	int x0      = 0;
	int y0      = 0;
	int x1      = 0;
	int y1      = 0;
	stbtt_GetCodepointHMetrics(&font->font_info, static_cast<int>(code), &advance, &lsb);
	stbtt_GetCodepointBitmapBox(&font->font_info, static_cast<int>(code), scale, scale, &x0, &y0,
	                            &x1, &y1);

	const float width     = static_cast<float>(std::max(x1 - x0, 0));
	const float height    = static_cast<float>(std::max(y1 - y0, 0));
	const float advance_f = std::max(static_cast<float>(advance) * scale, width);

	metrics->width                = width;
	metrics->height               = height;
	metrics->horizontal.bearing_x = static_cast<float>(x0);
	metrics->horizontal.bearing_y = static_cast<float>(-y0);
	metrics->horizontal.advance   = advance_f;
	metrics->vertical.bearing_x   = 0.0f;
	metrics->vertical.bearing_y   = 0.0f;
	metrics->vertical.advance     = stb_font_pixel_height(font);

	return true;
}

static bool init_stb_image(std::array<uint8_t, FONT_BITMAP_MAX_DIM * FONT_BITMAP_MAX_DIM>* image,
                           FontTransImage* trans_image, uint32_t code, FontState* font) {
	if (image == nullptr || trans_image == nullptr || !ensure_stb_font(font)) {
		return false;
	}

	code              = stb_supported_codepoint(font, code);
	const float scale = stb_font_scale(font);

	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	stbtt_GetCodepointBitmapBox(&font->font_info, static_cast<int>(code), scale, scale, &x0, &y0,
	                            &x1, &y1);

	image->fill(0);

	const auto width          = static_cast<uint32_t>(std::clamp(x1 - x0, 0, FONT_BITMAP_MAX_DIM));
	const auto height         = static_cast<uint32_t>(std::clamp(y1 - y0, 0, FONT_BITMAP_MAX_DIM));
	trans_image->address      = image->data();
	trans_image->width_byte   = FONT_BITMAP_MAX_DIM;
	trans_image->image_width  = width;
	trans_image->image_height = height;

	if (width == 0 || height == 0) {
		return true;
	}

	stbtt_MakeCodepointBitmap(&font->font_info, image->data(), static_cast<int>(width),
	                          static_cast<int>(height), FONT_BITMAP_MAX_DIM, scale, scale,
	                          static_cast<int>(code));

	return true;
}

static int scaled_font_height(const FontState* font) {
	const float scale = (font != nullptr && font->scale_h > 1.0f ? font->scale_h : 16.0f);
	return std::clamp(static_cast<int>(scale + 0.5f), 8, FONT_BITMAP_MAX_DIM);
}

static int scaled_font_width(const FontState* font) {
	return std::clamp((scaled_font_height(font) + 1) / 2, 4, FONT_BITMAP_MAX_DIM);
}

static void fill_bitmap_metrics(FontGlyphMetrics* metrics, const FontState* font) {
	if (metrics == nullptr) {
		return;
	}

	const float width  = static_cast<float>(scaled_font_width(font));
	const float height = static_cast<float>(scaled_font_height(font));
	const float ascent = height * 0.75f;

	metrics->width                = width;
	metrics->height               = height;
	metrics->horizontal.bearing_x = 0.0f;
	metrics->horizontal.bearing_y = ascent;
	metrics->horizontal.advance   = width;
	metrics->vertical.bearing_x   = 0.0f;
	metrics->vertical.bearing_y   = 0.0f;
	metrics->vertical.advance     = height;
}

static void fill_metrics_for_code(FontGlyphMetrics* metrics, const FontState* font, uint32_t code) {
	if (metrics == nullptr) {
		return;
	}

	if (get_stb_metrics(const_cast<FontState*>(font), code, metrics)) {
		return;
	}

	fill_bitmap_metrics(metrics, font);
}

static void fill_metrics(FontGlyphMetrics* metrics, const FontState* font) {
	fill_metrics_for_code(metrics, font, static_cast<uint32_t>('?'));
}

static uint32_t normalize_bitmap_font_code(uint32_t code) {
	if (code < 256) {
		return code;
	}
	return static_cast<uint32_t>('?');
}

static void init_image(std::array<uint8_t, FONT_BITMAP_MAX_DIM * FONT_BITMAP_MAX_DIM>* image,
                       FontTransImage* trans_image, uint32_t code, const FontState* font) {
	EXIT_NOT_IMPLEMENTED(image == nullptr);
	EXIT_NOT_IMPLEMENTED(trans_image == nullptr);

	if (init_stb_image(image, trans_image, code, const_cast<FontState*>(font))) {
		return;
	}

	image->fill(0);
	code = normalize_bitmap_font_code(code);

	const auto width  = static_cast<uint32_t>(scaled_font_width(font));
	const auto height = static_cast<uint32_t>(scaled_font_height(font));

	for (uint32_t y = 0; y < height; y++) {
		const uint32_t src_y = std::min<uint32_t>((y * 16u) / height, 15u);
		const uint8_t  row   = avpriv_vga16_font[code * 16 + src_y];
		for (uint32_t x = 0; x < width; x++) {
			const uint32_t src_x = std::min<uint32_t>((x * 8u) / width, 7u);
			if ((row & (0x80u >> src_x)) != 0) {
				(*image)[y * FONT_BITMAP_MAX_DIM + x] = 0xff;
			}
		}
	}

	trans_image->address      = image->data();
	trans_image->width_byte   = FONT_BITMAP_MAX_DIM;
	trans_image->image_width  = width;
	trans_image->image_height = height;
}

static void fill_render_result(FontState* font, FontRenderSurface* surf, float x, float y,
                               FontGlyphMetrics* metrics, FontRenderResult* result) {
	FontGlyphMetrics local_metrics {};
	auto*            out_metrics = (metrics != nullptr ? metrics : &local_metrics);
	if (metrics == nullptr) {
		fill_metrics(out_metrics, font);
	}

	if (font == nullptr || result == nullptr) {
		return;
	}

	const auto top_x = std::max(static_cast<int>(x + out_metrics->horizontal.bearing_x), 0);
	const auto top_y = std::max(static_cast<int>(y - out_metrics->horizontal.bearing_y), 0);

	result->trans_image = &font->trans_image;
	result->surface_image.address =
	    (surf != nullptr ? static_cast<uint8_t*>(surf->buffer) : nullptr);
	result->surface_image.width_byte =
	    (surf != nullptr ? static_cast<uint32_t>(std::max(surf->width_byte, 0)) : 0);
	result->surface_image.pixel_size_byte =
	    (surf != nullptr ? static_cast<uint8_t>(std::max<int8_t>(surf->pixel_size_byte, 0)) : 0);
	result->surface_image.pixel_format = 0;
	result->update_rect.x              = static_cast<uint32_t>(top_x);
	result->update_rect.y              = static_cast<uint32_t>(top_y);
	result->update_rect.w              = font->trans_image.image_width;
	result->update_rect.h              = font->trans_image.image_height;
	result->image_metrics.bearing_x    = 0.0f;
	result->image_metrics.bearing_y    = out_metrics->horizontal.bearing_y;
	result->image_metrics.advance      = out_metrics->horizontal.advance;
	result->image_metrics.stride       = out_metrics->horizontal.advance;
	result->image_metrics.width        = font->trans_image.image_width;
	result->image_metrics.height       = font->trans_image.image_height;
}

static void draw_to_surface(const FontTransImage& image, FontRenderSurface* surf, float x,
                            float y) {
	if (surf == nullptr || surf->buffer == nullptr || surf->width <= 0 || surf->height <= 0 ||
	    surf->width_byte <= 0 || surf->pixel_size_byte <= 0) {
		return;
	}

	auto*          dst        = static_cast<uint8_t*>(surf->buffer);
	const int      pixel_size = surf->pixel_size_byte;
	const int      start_x    = std::max(static_cast<int>(x), 0);
	const int      start_y    = std::max(static_cast<int>(y), 0);
	const int      end_x = std::min(start_x + static_cast<int>(image.image_width), surf->width);
	const int      end_y = std::min(start_y + static_cast<int>(image.image_height), surf->height);
	const uint32_t sx0 =
	    std::min(surf->scissor.x0, static_cast<uint32_t>(std::max(surf->width, 0)));
	const uint32_t sy0 =
	    std::min(surf->scissor.y0, static_cast<uint32_t>(std::max(surf->height, 0)));
	const uint32_t sx1 =
	    std::min(surf->scissor.x1, static_cast<uint32_t>(std::max(surf->width, 0)));
	const uint32_t sy1 =
	    std::min(surf->scissor.y1, static_cast<uint32_t>(std::max(surf->height, 0)));

	for (int yy = start_y; yy < end_y; yy++) {
		if (static_cast<uint32_t>(yy) < sy0 || static_cast<uint32_t>(yy) >= sy1) {
			continue;
		}
		for (int xx = start_x; xx < end_x; xx++) {
			if (static_cast<uint32_t>(xx) < sx0 || static_cast<uint32_t>(xx) >= sx1) {
				continue;
			}
			const auto src = image.address[(yy - start_y) * image.width_byte + (xx - start_x)];
			if (src == 0) {
				continue;
			}
			uint8_t* pixel = dst + yy * surf->width_byte + xx * pixel_size;
			for (int i = 0; i < pixel_size; i++) {
				pixel[i] = src;
			}
		}
	}
}

int KYTY_SYSV_ABI FontMemoryInit(FontMemory* font_memory, void* address, uint32_t size_byte,
                                 const FontMemoryInterface* mem_interface, void* mspace_object,
                                 FontMemoryDestroyCallback destroy_callback, void* destroy_object) {
	PRINT_NAME();

	if (font_memory == nullptr) {
		return -1;
	}

	font_memory->type             = 1;
	font_memory->attr             = 0;
	font_memory->size             = size_byte;
	font_memory->address          = address;
	font_memory->mspace_object    = mspace_object;
	font_memory->mem_interface    = mem_interface;
	font_memory->destroy_callback = destroy_callback;
	font_memory->destroy_object   = destroy_object;
	font_memory->user_object      = nullptr;
	font_memory->parent_object    = nullptr;

	LOGF("\t address = 0x%016" PRIx64 ", size = %" PRIu32 "\n", reinterpret_cast<uint64_t>(address),
	     size_byte);

	return OK;
}

int KYTY_SYSV_ABI FontMemoryTerm(FontMemory* font_memory) {
	PRINT_NAME();

	if (font_memory != nullptr) {
		font_memory->type = 0;
	}

	return OK;
}

int KYTY_SYSV_ABI FontCreateLibraryWithEdition(const FontMemory*    memory,
                                               FontLibrarySelection selection, uint64_t edition,
                                               FontLibrary* library) {
	PRINT_NAME();

	if (library == nullptr) {
		return -1;
	}

	auto* state    = new LibraryState {};
	state->memory  = memory;
	state->edition = edition;

	*library = state;

	LOGF("\t memory = 0x%016" PRIx64 ", selection = 0x%016" PRIx64 ", edition = 0x%016" PRIx64
	     ", library = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(memory), reinterpret_cast<uint64_t>(selection), edition,
	     reinterpret_cast<uint64_t>(*library));

	return OK;
}

int KYTY_SYSV_ABI FontCreateLibrary(const FontMemory* memory, FontLibrarySelection selection,
                                    FontLibrary* library) {
	return FontCreateLibraryWithEdition(memory, selection, 0, library);
}

int KYTY_SYSV_ABI FontDestroyLibrary(FontLibrary* library) {
	PRINT_NAME();

	if (library != nullptr) {
		delete static_cast<LibraryState*>(*library);
		*library = nullptr;
	}

	return OK;
}

int KYTY_SYSV_ABI FontCreateRendererWithEdition(const FontMemory*     memory,
                                                FontRendererSelection selection, uint64_t edition,
                                                FontRenderer* renderer) {
	PRINT_NAME();

	if (renderer == nullptr) {
		return -1;
	}

	auto* state      = new RendererState {};
	state->memory    = memory;
	state->selection = selection;
	state->edition   = edition;

	*renderer = state;

	LOGF("\t memory = 0x%016" PRIx64 ", selection = 0x%016" PRIx64 ", edition = 0x%016" PRIx64
	     ", renderer = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(memory), reinterpret_cast<uint64_t>(selection), edition,
	     reinterpret_cast<uint64_t>(*renderer));

	return OK;
}

int KYTY_SYSV_ABI FontDestroyRenderer(FontRenderer* renderer) {
	PRINT_NAME();

	if (renderer != nullptr) {
		delete static_cast<RendererState*>(*renderer);
		*renderer = nullptr;
	}

	return OK;
}

static int open_font(FontLibrary library, const void* data, uint32_t size, uint32_t font_set_type,
                     uint32_t open_mode, FontHandle* handle) {
	if (handle == nullptr) {
		return -1;
	}

	auto* state                      = new FontState {};
	state->library                   = library;
	state->data                      = data;
	state->size                      = size;
	state->font_set_type             = font_set_type;
	state->open_mode                 = open_mode;
	state->attribute                 = 0;
	state->renderer                  = nullptr;
	state->scale_w                   = 8.0f;
	state->scale_h                   = 16.0f;
	state->effect_weight_x           = 0.0f;
	state->effect_weight_y           = 0.0f;
	state->effect_weight_mode        = 0;
	state->effect_slant              = 0.0f;
	state->render_scale_w            = 8.0f;
	state->render_scale_h            = 16.0f;
	state->render_effect_weight_x    = 0.0f;
	state->render_effect_weight_y    = 0.0f;
	state->render_effect_weight_mode = 0;
	state->render_effect_slant       = 0.0f;
	state->ttf_data                  = nullptr;
	state->ttf_size                  = 0;
	state->has_ttf                   = false;
	init_font_face(state);
	init_image(&state->fallback_image, &state->trans_image, static_cast<uint32_t>('?'), state);

	*handle = state;

	LOGF("\t library = 0x%016" PRIx64 ", data = 0x%016" PRIx64 ", size = %" PRIu32
	     ", font_set = %" PRIu32 ", open_mode = 0x%08" PRIx32 ", handle = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(library), reinterpret_cast<uint64_t>(data), size, font_set_type,
	     open_mode, reinterpret_cast<uint64_t>(*handle));

	return OK;
}

int KYTY_SYSV_ABI FontOpenFontSet(FontLibrary library, uint32_t font_set_type, uint32_t open_mode,
                                  const FontOpenDetail* detail, FontHandle* handle) {
	PRINT_NAME();

	return open_font(library, detail, 0, font_set_type, open_mode, handle);
}

int KYTY_SYSV_ABI FontOpenFontMemory(FontLibrary library, const void* font_address,
                                     uint32_t    font_size, const FontOpenDetail* /*detail*/,
                                     FontHandle* handle) {
	PRINT_NAME();

	return open_font(library, font_address, font_size, 0, 0, handle);
}

int KYTY_SYSV_ABI FontOpenFontInstance(FontHandle font_handle, void* setup_font,
                                       FontHandle* out_font_handle) {
	PRINT_NAME();

	if (out_font_handle == nullptr) {
		return -1;
	}

	if (font_handle == nullptr) {
		return open_font(nullptr, setup_font, 0, 0, 0, out_font_handle);
	}

	const auto* source = static_cast<FontState*>(font_handle);
	return open_font(source->library, source->data, source->size, source->font_set_type,
	                 source->open_mode, out_font_handle);
}

int KYTY_SYSV_ABI FontCloseFont(FontHandle font_handle) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	delete font;

	return OK;
}

int KYTY_SYSV_ABI FontDefineAttribute(FontHandle font_handle, int attribute, int* old_attribute) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (old_attribute != nullptr) {
		*old_attribute = (font != nullptr ? font->attribute : 0);
	}
	if (font != nullptr) {
		font->attribute = attribute;
	}

	LOGF("\t handle = 0x%016" PRIx64 ", attribute = %d\n", reinterpret_cast<uint64_t>(font_handle),
	     attribute);

	return OK;
}

int KYTY_SYSV_ABI FontBindRenderer(FontHandle font_handle, FontRenderer renderer) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->renderer = renderer;

	LOGF("\t handle = 0x%016" PRIx64 ", renderer = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), reinterpret_cast<uint64_t>(renderer));

	return OK;
}

int KYTY_SYSV_ABI FontUnbindRenderer(FontHandle font_handle) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->renderer = nullptr;

	LOGF("\t handle = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_handle));

	return OK;
}

int KYTY_SYSV_ABI FontRebindRenderer(FontHandle font_handle) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	LOGF("\t handle = 0x%016" PRIx64 ", renderer = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), reinterpret_cast<uint64_t>(font->renderer));

	return OK;
}

int KYTY_SYSV_ABI FontSetScalePixel(FontHandle font_handle, float w, float h) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->scale_w = w;
	font->scale_h = h;

	LOGF("\t handle = 0x%016" PRIx64 ", w = %f, h = %f\n", reinterpret_cast<uint64_t>(font_handle),
	     static_cast<double>(w), static_cast<double>(h));

	return OK;
}

int KYTY_SYSV_ABI FontSetEffectSlant(FontHandle font_handle, float slant_ratio) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->effect_slant = slant_ratio;

	LOGF("\t handle = 0x%016" PRIx64 ", slant = %f\n", reinterpret_cast<uint64_t>(font_handle),
	     static_cast<double>(slant_ratio));

	return OK;
}

int KYTY_SYSV_ABI FontSetEffectWeight(FontHandle font_handle, float weight_x_scale,
                                      float weight_y_scale, uint32_t mode) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->effect_weight_x    = weight_x_scale;
	font->effect_weight_y    = weight_y_scale;
	font->effect_weight_mode = mode;

	LOGF("\t handle = 0x%016" PRIx64 ", weight_x = %f, weight_y = %f, mode = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(font_handle), static_cast<double>(weight_x_scale),
	     static_cast<double>(weight_y_scale), mode);

	return OK;
}

int KYTY_SYSV_ABI FontSetupRenderScalePixel(FontHandle font_handle, float w, float h) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->render_scale_w = w;
	font->render_scale_h = h;

	LOGF("\t handle = 0x%016" PRIx64 ", w = %f, h = %f\n", reinterpret_cast<uint64_t>(font_handle),
	     static_cast<double>(w), static_cast<double>(h));

	return OK;
}

int KYTY_SYSV_ABI FontSetupRenderEffectSlant(FontHandle font_handle, float slant_ratio) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->render_effect_slant = slant_ratio;

	LOGF("\t handle = 0x%016" PRIx64 ", slant = %f\n", reinterpret_cast<uint64_t>(font_handle),
	     static_cast<double>(slant_ratio));

	return OK;
}

int KYTY_SYSV_ABI FontSetupRenderEffectWeight(FontHandle font_handle, float weight_x_scale,
                                              float weight_y_scale, uint32_t mode) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font == nullptr) {
		return -1;
	}

	font->render_effect_weight_x    = weight_x_scale;
	font->render_effect_weight_y    = weight_y_scale;
	font->render_effect_weight_mode = mode;

	LOGF("\t handle = 0x%016" PRIx64 ", weight_x = %f, weight_y = %f, mode = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(font_handle), static_cast<double>(weight_x_scale),
	     static_cast<double>(weight_y_scale), mode);

	return OK;
}

int KYTY_SYSV_ABI FontGetHorizontalLayout(FontHandle font_handle, FontHorizontalLayout* layout) {
	PRINT_NAME();

	if (font_handle == nullptr || layout == nullptr) {
		return -1;
	}

	auto* font = static_cast<FontState*>(font_handle);
	if (ensure_stb_font(font)) {
		const float scale    = stb_font_scale(font);
		int         ascent   = 0;
		int         descent  = 0;
		int         line_gap = 0;
		stbtt_GetFontVMetrics(&font->font_info, &ascent, &descent, &line_gap);
		layout->base_line_y   = static_cast<float>(ascent) * scale;
		layout->line_height   = static_cast<float>(ascent - descent + line_gap) * scale;
		layout->effect_height = layout->line_height;
	} else {
		layout->base_line_y   = static_cast<float>(scaled_font_height(font)) * 0.75f;
		layout->line_height   = static_cast<float>(scaled_font_height(font));
		layout->effect_height = layout->line_height;
	}

	LOGF("\t handle = 0x%016" PRIx64 ", layout = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), reinterpret_cast<uint64_t>(layout));

	return OK;
}

int KYTY_SYSV_ABI FontGetVerticalLayout(FontHandle font_handle, FontVerticalLayout* layout) {
	PRINT_NAME();

	if (font_handle == nullptr || layout == nullptr) {
		return -1;
	}

	layout->base_line_x  = 0.0f;
	layout->line_width   = 16.0f;
	layout->effect_width = 16.0f;

	LOGF("\t handle = 0x%016" PRIx64 ", layout = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), reinterpret_cast<uint64_t>(layout));

	return OK;
}

int KYTY_SYSV_ABI FontSupportSystemFonts(FontLibrary library) {
	PRINT_NAME();

	LOGF("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));

	return OK;
}

int KYTY_SYSV_ABI FontSupportExternalFonts(FontLibrary library, uint32_t font_max,
                                           uint32_t formats) {
	PRINT_NAME();

	LOGF("\t library = 0x%016" PRIx64 ", font_max = %" PRIu32 ", formats = 0x%08" PRIx32 "\n",
	     reinterpret_cast<uint64_t>(library), font_max, formats);

	return OK;
}

int KYTY_SYSV_ABI FontAttachDeviceCacheBuffer(FontLibrary library, void* buffer, uint32_t size) {
	PRINT_NAME();

	LOGF("\t library = 0x%016" PRIx64 ", buffer = 0x%016" PRIx64 ", size = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(library), reinterpret_cast<uint64_t>(buffer), size);

	return OK;
}

int KYTY_SYSV_ABI FontTextSourceInit(FontTextSource* font_text_source, const void* text_address,
                                     uint32_t text_size_byte, FontTextParseFunction text_parser,
                                     void* text_object) {
	PRINT_NAME();

	if (font_text_source == nullptr) {
		return -1;
	}

	std::memset(font_text_source, 0, sizeof(FontTextSource));

	uint32_t resolved_text_size = text_size_byte;
	if (text_address != nullptr && resolved_text_size == 0) {
		constexpr uint32_t max_terminated_text_size = 4096;
		const auto*        text                     = static_cast<const uint8_t*>(text_address);
		while (resolved_text_size < max_terminated_text_size && text[resolved_text_size] != 0) {
			resolved_text_size++;
		}
	}

	font_text_source->start   = text_address;
	font_text_source->end     = text_address != nullptr
	                                ? static_cast<const uint8_t*>(text_address) + resolved_text_size
	                                : nullptr;
	font_text_source->current = text_address;
	font_text_source->text_parser = text_parser;
	font_text_source->text_object = text_object;
	font_text_source->system_use[1] =
	    reinterpret_cast<void*>(static_cast<uintptr_t>(resolved_text_size));
	set_text_source_writing_form(font_text_source, SCE_FONT_WRITING_FORM_HORIZONTAL);

	LOGF("\t source = 0x%016" PRIx64 ", text = 0x%016" PRIx64 ", size = %" PRIu32
	     ", resolved_size = %" PRIu32 ", parser = 0x%016" PRIx64 ", object = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_text_source), reinterpret_cast<uint64_t>(text_address),
	     text_size_byte, resolved_text_size, reinterpret_cast<uint64_t>(text_parser),
	     reinterpret_cast<uint64_t>(text_object));

	return OK;
}

int KYTY_SYSV_ABI FontTextSourceRewind(FontTextSource* font_text_source) {
	PRINT_NAME();

	if (font_text_source == nullptr) {
		return -1;
	}

	font_text_source->current = font_text_source->start;

	LOGF("\t source = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_text_source));

	return OK;
}

int KYTY_SYSV_ABI FontTextSourceSetWritingForm(FontTextSource* font_text_source, int writing_form) {
	PRINT_NAME();

	if (font_text_source == nullptr) {
		return -1;
	}

	set_text_source_writing_form(font_text_source, writing_form);

	LOGF("\t source = 0x%016" PRIx64 ", writing_form = 0x%08x\n",
	     reinterpret_cast<uint64_t>(font_text_source), writing_form);

	return OK;
}

int KYTY_SYSV_ABI FontTextSourceSetDefaultFont(FontTextSource* font_text_source,
                                               FontHandle      default_font) {
	PRINT_NAME();

	if (font_text_source == nullptr) {
		return -1;
	}

	font_text_source->default_font = default_font;

	LOGF("\t source = 0x%016" PRIx64 ", default_font = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_text_source), reinterpret_cast<uint64_t>(default_font));

	return OK;
}

int KYTY_SYSV_ABI FontCreateString(const FontMemory* memory, FontTextSource* font_text_source,
                                   const FontCreateStringDetail* string_detail,
                                   FontString*                   font_string) {
	PRINT_NAME();

	if (font_string == nullptr) {
		return -1;
	}

	auto* state            = new FontStringState {};
	state->memory          = memory;
	state->writing_form    = get_text_source_writing_form(font_text_source);
	state->terminate_code  = 0;
	state->terminate_order = nullptr;

	auto      default_font = get_string_default_font(font_text_source, string_detail);
	const int ret          = parse_text_with_callback(font_text_source, default_font, state);
	if (ret != OK) {
		delete state;
		*font_string = nullptr;
		return ret;
	}

	*font_string = state;

	LOGF("\t memory = 0x%016" PRIx64 ", source = 0x%016" PRIx64 ", detail = 0x%016" PRIx64
	     ", string = 0x%016" PRIx64 ", chars = %zu\n",
	     reinterpret_cast<uint64_t>(memory), reinterpret_cast<uint64_t>(font_text_source),
	     reinterpret_cast<uint64_t>(string_detail), reinterpret_cast<uint64_t>(*font_string),
	     state->characters.size());

	return OK;
}

int KYTY_SYSV_ABI FontDestroyString(FontString* font_string) {
	PRINT_NAME();

	LOGF("\t string_ptr = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_string));

	if (font_string != nullptr) {
		delete static_cast<FontStringState*>(*font_string);
		*font_string = nullptr;
	}

	return OK;
}

int KYTY_SYSV_ABI FontStringGetWritingForm(FontString font_string) {
	PRINT_NAME();

	const auto* state = static_cast<const FontStringState*>(font_string);
	return state != nullptr ? state->writing_form : SCE_FONT_WRITING_FORM_HORIZONTAL;
}

uint32_t KYTY_SYSV_ABI FontStringGetTerminateCode(FontString font_string) {
	PRINT_NAME();

	const auto* state = static_cast<const FontStringState*>(font_string);
	return state != nullptr ? state->terminate_code : 0;
}

void* KYTY_SYSV_ABI FontStringGetTerminateOrder(FontString font_string) {
	PRINT_NAME();

	const auto* state = static_cast<const FontStringState*>(font_string);
	return state != nullptr ? state->terminate_order : nullptr;
}

FontTextCharacter KYTY_SYSV_ABI FontStringRefersTextCharacters(FontString font_string,
                                                               uint32_t*  character_count) {
	PRINT_NAME();

	auto* state = static_cast<FontStringState*>(font_string);
	if (character_count != nullptr) {
		*character_count = state != nullptr ? static_cast<uint32_t>(state->characters.size()) : 0;
	}

	LOGF("\t string = 0x%016" PRIx64 ", count = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(font_string),
	     state != nullptr ? static_cast<uint32_t>(state->characters.size()) : 0);

	return state != nullptr && !state->characters.empty() ? &state->characters.front() : nullptr;
}

FontRenderCharacter KYTY_SYSV_ABI
FontStringRefersRenderCharacters(FontString font_string, FontTextCharacter start_character,
                                 FontTextCharacter last_character, uint32_t* character_count) {
	PRINT_NAME();

	auto* state = static_cast<FontStringState*>(font_string);
	auto* first = get_character(start_character);
	auto* last  = get_character(last_character);
	if (first == nullptr && state != nullptr && !state->characters.empty()) {
		first = &state->characters.front();
	}

	if (character_count != nullptr) {
		*character_count = count_characters_until(first, last);
	}

	LOGF("\t string = 0x%016" PRIx64 ", start = 0x%016" PRIx64 ", last = 0x%016" PRIx64
	     ", count = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(font_string), reinterpret_cast<uint64_t>(start_character),
	     reinterpret_cast<uint64_t>(last_character),
	     character_count != nullptr ? *character_count : count_characters_until(first, last));

	return first;
}

FontTextCharacter KYTY_SYSV_ABI FontCharacterRefersTextNext(FontTextCharacter text_character) {
	PRINT_NAME();

	return get_next_character(get_character(text_character));
}

FontTextCharacter KYTY_SYSV_ABI FontCharacterRefersTextBack(FontTextCharacter text_character) {
	PRINT_NAME();

	return get_prev_character(get_character(text_character));
}

FontTextCodes* KYTY_SYSV_ABI FontCharactersRefersTextCodes(FontTextCharacter text_character,
                                                           FontTextCharacter term_character,
                                                           FontTextCodes*    text_codes) {
	PRINT_NAME();

	auto* character = get_character(text_character);
	fill_text_codes(text_codes, character, get_character(term_character));

	return text_codes;
}

FontTextCodes* KYTY_SYSV_ABI FontTextCodesStepNext(FontTextCodes* text_codes_step) {
	PRINT_NAME();

	if (text_codes_step == nullptr) {
		return nullptr;
	}

	auto* current = static_cast<FontCharacterState*>(text_codes_step->system_reserved[2]);
	auto* term    = static_cast<FontCharacterState*>(text_codes_step->system_reserved[3]);
	auto* next    = get_next_character(current);
	if (next == nullptr || next == term) {
		return nullptr;
	}

	fill_text_codes(text_codes_step, next, term);
	return text_codes_step;
}

FontTextCodes* KYTY_SYSV_ABI FontTextCodesStepBack(FontTextCodes* text_codes_step) {
	PRINT_NAME();

	if (text_codes_step == nullptr) {
		return nullptr;
	}

	auto* current = static_cast<FontCharacterState*>(text_codes_step->system_reserved[2]);
	auto* term    = static_cast<FontCharacterState*>(text_codes_step->system_reserved[3]);
	auto* prev    = get_prev_character(current);
	if (prev == nullptr) {
		return nullptr;
	}

	fill_text_codes(text_codes_step, prev, term);
	return text_codes_step;
}

int KYTY_SYSV_ABI FontCharacterGetBidiLevel(FontTextCharacter text_character, int* bidi_level) {
	PRINT_NAME();

	if (text_character == nullptr || bidi_level == nullptr) {
		return -1;
	}

	*bidi_level = 0;
	return OK;
}

uint32_t KYTY_SYSV_ABI FontCharacterLooksWhiteSpace(FontTextCharacter text_character) {
	PRINT_NAME();

	auto* character = get_character(text_character);
	if (character == nullptr) {
		return 0;
	}

	const auto code = character->code;
	return (code == ' ' || code == '\t' || code == '\n' || code == '\r') ? 1u : 0u;
}

uint32_t KYTY_SYSV_ABI FontCharacterLooksFormatCharacters(FontTextCharacter text_character) {
	PRINT_NAME();

	return text_character != nullptr ? 0u : 0u;
}

int KYTY_SYSV_ABI FontCharacterGetTextFontCode(FontTextCharacter text_character,
                                               FontHandle* font_handle, uint32_t* text_code) {
	PRINT_NAME();

	auto* character = get_character(text_character);
	if (character == nullptr) {
		return -1;
	}

	if (font_handle != nullptr) {
		*font_handle = character->font;
	}
	if (text_code != nullptr) {
		*text_code = character->code;
	}

	return OK;
}

int KYTY_SYSV_ABI FontCharacterGetTextOrder(FontTextCharacter text_character, void** text_order) {
	PRINT_NAME();

	auto* character = get_character(text_character);
	if (character == nullptr || text_order == nullptr) {
		return -1;
	}

	*text_order = character->order;
	return OK;
}

int KYTY_SYSV_ABI FontCharacterGetSyllableStringState(FontTextCharacter text_character,
                                                      int*              syllable_string_state) {
	PRINT_NAME();

	if (text_character == nullptr || syllable_string_state == nullptr) {
		return -1;
	}

	*syllable_string_state = 0;
	return OK;
}

int KYTY_SYSV_ABI FontWritingInit(FontWriting* font_writing, FontString font_string,
                                  FontRenderCharacter font_character) {
	PRINT_NAME();

	if (font_writing == nullptr) {
		return -1;
	}

	if (auto* old_state = get_writing_state(font_writing); old_state != nullptr) {
		unregister_writing_state(old_state);
		delete old_state;
	}
	std::memset(font_writing, 0, sizeof(FontWriting));

	auto* state           = new FontWritingState {};
	state->string         = static_cast<FontStringState*>(font_string);
	state->character      = get_character(font_character);
	state->invisible_mask = 0;
	if (state->character == nullptr && state->string != nullptr &&
	    !state->string->characters.empty()) {
		state->character = &state->string->characters.front();
	}

	fill_writing_step(&state->step, state->character);
	fill_writing_letter_step(&state->letter_step, state->character);
	fill_writing_metrics(&state->metrics, state->step.profile.character_count);
	register_writing_state(state);

	font_writing->system_use[0] = state;
	font_writing->system_use[1] = &state->step;
	font_writing->system_use[2] = state->character;
	font_writing->system_use[3] = &state->letter_step;
	font_writing->system_use[4] = &state->metrics;

	LOGF("\t writing = 0x%016" PRIx64 ", string = 0x%016" PRIx64 ", character = 0x%016" PRIx64
	     ", step = 0x%016" PRIx64 ", letter_step = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_writing), reinterpret_cast<uint64_t>(font_string),
	     reinterpret_cast<uint64_t>(font_character), reinterpret_cast<uint64_t>(&state->step),
	     reinterpret_cast<uint64_t>(&state->letter_step));

	return OK;
}

int KYTY_SYSV_ABI FontWritingSetMaskInvisible(FontWriting* font_writing, int mask) {
	PRINT_NAME();

	auto* state = get_writing_state(font_writing);
	if (state == nullptr) {
		return -1;
	}

	state->invisible_mask = mask;
	return OK;
}

const FontWritingStep* KYTY_SYSV_ABI FontWritingRefersRenderStep(FontWriting* font_writing) {
	PRINT_NAME();

	auto* state = get_writing_state(font_writing);
	auto* step  = state != nullptr ? &state->step : nullptr;
	LOGF("\t writing = 0x%016" PRIx64 ", step = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_writing), reinterpret_cast<uint64_t>(step));
	return step;
}

FontTextCharacter KYTY_SYSV_ABI FontWritingRefersRenderStepCharacter(FontWriting* font_writing,
                                                                     const void** letter_step) {
	PRINT_NAME();

	auto* state = get_writing_state(font_writing);
	if (letter_step != nullptr) {
		*letter_step = state != nullptr ? &state->letter_step : nullptr;
	}
	LOGF("\t writing = 0x%016" PRIx64 ", character = 0x%016" PRIx64 ", letter_step = 0x%016" PRIx64
	     "\n",
	     reinterpret_cast<uint64_t>(font_writing),
	     reinterpret_cast<uint64_t>(state != nullptr ? state->character : nullptr),
	     reinterpret_cast<uint64_t>(letter_step != nullptr ? *letter_step : nullptr));
	return state != nullptr ? state->character : nullptr;
}

int KYTY_SYSV_ABI FontWritingGetRenderMetrics(FontWriting*        font_writing,
                                              FontWritingMetrics* writing_metrics) {
	PRINT_NAME();

	auto* state = get_writing_state(font_writing);
	if (state == nullptr || writing_metrics == nullptr) {
		return -1;
	}

	*writing_metrics = state->metrics;
	return OK;
}

int KYTY_SYSV_ABI FontCreateWritingLine(const FontMemory* memory, int writing_form,
                                        const void*      writing_line_detail,
                                        FontWritingLine* writing_line) {
	PRINT_NAME();

	if (writing_line == nullptr) {
		return -1;
	}

	auto* state         = new FontWritingLineState {};
	state->memory       = memory;
	state->writing_form = writing_form;
	clear_writing_line(state);
	*writing_line = state;

	LOGF("\t memory = 0x%016" PRIx64 ", writing_form = 0x%08x"
	     ", detail = 0x%016" PRIx64 ", line = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(memory), writing_form,
	     reinterpret_cast<uint64_t>(writing_line_detail),
	     reinterpret_cast<uint64_t>(*writing_line));

	return OK;
}

int KYTY_SYSV_ABI FontDestroyWritingLine(FontWritingLine* writing_line) {
	PRINT_NAME();

	if (writing_line != nullptr) {
		delete static_cast<FontWritingLineState*>(*writing_line);
		*writing_line = nullptr;
	}

	return OK;
}

int KYTY_SYSV_ABI FontWritingLineClear(FontWritingLine writing_line) {
	PRINT_NAME();

	auto* state = static_cast<FontWritingLineState*>(writing_line);
	if (state == nullptr) {
		return -1;
	}

	clear_writing_line(state);
	return OK;
}

int KYTY_SYSV_ABI FontWritingLineWritesOrder(FontWritingLine           writing_line,
                                             uint64_t                  writing_attribute,
                                             const FontWritingMetrics* writing_metrics,
                                             void*                     writing_orderer) {
	PRINT_NAME();

	auto* state = static_cast<FontWritingLineState*>(writing_line);
	if (state == nullptr) {
		return -1;
	}

	FontWritingMetrics order_metrics {};
	if (writing_metrics != nullptr) {
		order_metrics = *writing_metrics;
	} else {
		fill_writing_metrics(&order_metrics, 1);
	}

	FontWritingLineStep step {};
	step.x                = state->metrics.advance_x;
	step.y                = 0.0f;
	step.advance_x        = order_metrics.advance_x;
	step.advance_y        = order_metrics.advance_y;
	step.spacing_progress = 0.0f;
	step.writing_orderer  = writing_orderer;
	step.metrics          = order_metrics;
	state->steps.push_back(step);
	state->cursor = 0;

	const float old_advance = state->metrics.advance_x;
	state->metrics.advance_x += order_metrics.advance_x;
	state->metrics.advance_y += order_metrics.advance_y;
	if (state->steps.size() == 1) {
		state->metrics.extent.top    = order_metrics.extent.top;
		state->metrics.extent.bottom = order_metrics.extent.bottom;
		state->metrics.extent.left   = old_advance + order_metrics.extent.left;
		state->metrics.extent.right  = old_advance + order_metrics.extent.right;
	} else {
		state->metrics.extent.top = std::min(state->metrics.extent.top, order_metrics.extent.top);
		state->metrics.extent.bottom =
		    std::max(state->metrics.extent.bottom, order_metrics.extent.bottom);
		state->metrics.extent.left =
		    std::min(state->metrics.extent.left, old_advance + order_metrics.extent.left);
		state->metrics.extent.right =
		    std::max(state->metrics.extent.right, old_advance + order_metrics.extent.right);
	}

	LOGF("\t line = 0x%016" PRIx64 ", attr = 0x%016" PRIx64 ", metrics = 0x%016" PRIx64
	     ", orderer = 0x%016" PRIx64 ", steps = %zu, advance = %f\n",
	     reinterpret_cast<uint64_t>(writing_line), writing_attribute,
	     reinterpret_cast<uint64_t>(writing_metrics), reinterpret_cast<uint64_t>(writing_orderer),
	     state->steps.size(), static_cast<double>(state->metrics.advance_x));

	return OK;
}

int KYTY_SYSV_ABI FontWritingLineGetOrderingSpace(FontWritingLine writing_line, float* head_space,
                                                  float* inline_space, float* tail_space,
                                                  float* advance_space) {
	PRINT_NAME();

	auto* state = static_cast<FontWritingLineState*>(writing_line);
	if (state == nullptr) {
		return -1;
	}

	const float advance = state->metrics.advance_x;
	if (head_space != nullptr) {
		*head_space = advance;
	}
	if (inline_space != nullptr) {
		*inline_space = 0.0f;
	}
	if (tail_space != nullptr) {
		*tail_space = advance;
	}
	if (advance_space != nullptr) {
		*advance_space = 0.0f;
	}

	return OK;
}

FontWritingLineStep* KYTY_SYSV_ABI FontWritingLineRefersRenderStep(FontWritingLine writing_line) {
	PRINT_NAME();

	auto* state = static_cast<FontWritingLineState*>(writing_line);
	if (state == nullptr || state->cursor >= state->steps.size()) {
		LOGF("\t line = 0x%016" PRIx64 ", step = 0x%016" PRIx64 "\n",
		     reinterpret_cast<uint64_t>(writing_line), uint64_t {0});
		return nullptr;
	}

	auto* step = &state->steps[state->cursor++];
	LOGF("\t line = 0x%016" PRIx64 ", step = 0x%016" PRIx64
	     ", index = %zu/%zu, orderer = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(writing_line), reinterpret_cast<uint64_t>(step), state->cursor,
	     state->steps.size(), reinterpret_cast<uint64_t>(step->writing_orderer));
	return step;
}

int KYTY_SYSV_ABI FontWritingLineGetRenderMetrics(FontWritingLine     writing_line,
                                                  FontWritingMetrics* writing_metrics) {
	PRINT_NAME();

	auto* state = static_cast<FontWritingLineState*>(writing_line);
	if (state == nullptr || writing_metrics == nullptr) {
		return -1;
	}

	*writing_metrics = state->metrics;
	return OK;
}

int KYTY_SYSV_ABI FontGetRenderCharGlyphMetrics(FontHandle font_handle, uint32_t code,
                                                FontGlyphMetrics* metrics) {
	PRINT_NAME();

	LOGF("\t handle = 0x%016" PRIx64 ", code = 0x%08" PRIx32 ", metrics = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), code, reinterpret_cast<uint64_t>(metrics));

	fill_metrics_for_code(metrics, static_cast<FontState*>(font_handle), code);

	return OK;
}

int KYTY_SYSV_ABI FontGetCharGlyphMetrics(FontHandle font_handle, uint32_t code,
                                          FontGlyphMetrics* metrics) {
	PRINT_NAME();

	LOGF("\t handle = 0x%016" PRIx64 ", code = 0x%08" PRIx32 ", metrics = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), code, reinterpret_cast<uint64_t>(metrics));

	fill_metrics_for_code(metrics, static_cast<FontState*>(font_handle), code);

	return OK;
}

void KYTY_SYSV_ABI FontRenderSurfaceInit(FontRenderSurface* surf, void* buffer, int buf_width_byte,
                                         int pixel_size_byte, int width, int height) {
	PRINT_NAME();

	if (surf == nullptr) {
		return;
	}

	std::memset(surf, 0, sizeof(FontRenderSurface));

	surf->buffer          = buffer;
	surf->width_byte      = buf_width_byte;
	surf->pixel_size_byte = static_cast<int8_t>(pixel_size_byte);
	surf->width           = std::max(width, 0);
	surf->height          = std::max(height, 0);
	surf->scissor.x0      = 0;
	surf->scissor.y0      = 0;
	surf->scissor.x1      = static_cast<uint32_t>(surf->width);
	surf->scissor.y1      = static_cast<uint32_t>(surf->height);

	LOGF("\t surf = 0x%016" PRIx64 ", buffer = 0x%016" PRIx64
	     ", width_byte = %d, pixel_size = %d, width = %d, height = %d\n",
	     reinterpret_cast<uint64_t>(surf), reinterpret_cast<uint64_t>(buffer), buf_width_byte,
	     pixel_size_byte, width, height);
}

void KYTY_SYSV_ABI FontRenderSurfaceSetScissor(FontRenderSurface* surf, uint32_t x0, uint32_t y0,
                                               uint32_t x1, uint32_t y1) {
	PRINT_NAME();

	if (surf == nullptr) {
		return;
	}

	surf->scissor.x0 = x0;
	surf->scissor.y0 = y0;
	surf->scissor.x1 = x1;
	surf->scissor.y1 = y1;

	LOGF("\t surf = 0x%016" PRIx64 ", x0 = %" PRIu32 ", y0 = %" PRIu32 ", x1 = %" PRIu32
	     ", y1 = %" PRIu32 "\n",
	     reinterpret_cast<uint64_t>(surf), x0, y0, x1, y1);
}

int KYTY_SYSV_ABI FontGenerateCharGlyph(FontHandle font_handle, uint32_t code,
                                        const FontGenerateGlyphDetail* detail, void** font_glyph) {
	PRINT_NAME();

	if (font_handle == nullptr || font_glyph == nullptr) {
		return -1;
	}

	auto* font  = static_cast<FontState*>(font_handle);
	auto* glyph = new GlyphState {};
	glyph->font = font;
	glyph->code = code;
	fill_metrics_for_code(&glyph->metrics, font, code);
	init_image(&glyph->image, &glyph->trans_image, code, font);
	register_glyph(glyph);

	*font_glyph = glyph;

	LOGF("\t handle = 0x%016" PRIx64 ", code = 0x%08" PRIx32 ", detail = 0x%016" PRIx64
	     ", glyph = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), code, reinterpret_cast<uint64_t>(detail),
	     reinterpret_cast<uint64_t>(*font_glyph));

	return OK;
}

int KYTY_SYSV_ABI FontDeleteGlyph(const FontMemory* memory, void** font_glyph) {
	PRINT_NAME();

	LOGF("\t memory = 0x%016" PRIx64 ", glyph_ptr = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(memory), reinterpret_cast<uint64_t>(font_glyph));

	if (font_glyph != nullptr) {
		if (unregister_glyph(*font_glyph)) {
			delete static_cast<GlyphState*>(*font_glyph);
		}
		*font_glyph = nullptr;
	}

	return OK;
}

int KYTY_SYSV_ABI FontGlyphDefineAttribute(void* font_glyph, int attribute, int* old_attribute) {
	PRINT_NAME();

	auto* glyph = (is_known_glyph(font_glyph) ? static_cast<GlyphState*>(font_glyph) : nullptr);
	if (old_attribute != nullptr) {
		*old_attribute = (glyph != nullptr ? glyph->attribute : 0);
	}
	if (glyph != nullptr) {
		glyph->attribute = attribute;
	}

	LOGF("\t glyph = 0x%016" PRIx64 ", attribute = %d\n", reinterpret_cast<uint64_t>(font_glyph),
	     attribute);

	return OK;
}

int KYTY_SYSV_ABI FontRenderCharGlyphImageHorizontal(FontHandle font_handle, uint32_t code,
                                                     FontRenderSurface* surf, float x, float y,
                                                     FontGlyphMetrics* metrics,
                                                     FontRenderResult* result) {
	PRINT_NAME();

	auto* font = static_cast<FontState*>(font_handle);
	if (font_handle == nullptr) {
		return -1;
	}

	init_image(&font->fallback_image, &font->trans_image, code, font);
	FontGlyphMetrics local_metrics {};
	auto*            draw_metrics = (metrics != nullptr ? metrics : &local_metrics);
	fill_metrics_for_code(draw_metrics, font, code);
	fill_render_result(font, surf, x, y, draw_metrics, result);
	const auto top_x = x + draw_metrics->horizontal.bearing_x;
	const auto top_y = y - draw_metrics->horizontal.bearing_y;
	draw_to_surface(font->trans_image, surf, top_x, top_y);

	LOGF("\t handle = 0x%016" PRIx64 ", code = 0x%08" PRIx32 ", surf = 0x%016" PRIx64
	     ", x = %f, y = %f, result = 0x%016" PRIx64 "\n",
	     reinterpret_cast<uint64_t>(font_handle), code, reinterpret_cast<uint64_t>(surf),
	     static_cast<double>(x), static_cast<double>(y), reinterpret_cast<uint64_t>(result));

	return OK;
}

} // namespace Font

LIB_DEFINE(InitFont_1) {
	LIB_FUNC("CUKn5pX-NVY", Font::FontAttachDeviceCacheBuffer);
	LIB_FUNC("vzHs3C8lWJk", Font::FontCloseFont);
	LIB_FUNC("WaSFJoRWXaI", Font::FontCreateRendererWithEdition);
	LIB_FUNC("exAxkyVLt0s", Font::FontDestroyRenderer);
	LIB_FUNC("3OdRkSjOcog", Font::FontBindRenderer);
	LIB_FUNC("Z2cdsqJH+5k", Font::FontRebindRenderer);
	LIB_FUNC("1QjhKxrsOB8", Font::FontUnbindRenderer);
	LIB_FUNC("N1EBMeGhf7E", Font::FontSetScalePixel);
	LIB_FUNC("TMtqoFQjjbA", Font::FontSetEffectSlant);
	LIB_FUNC("v0phZwa4R5o", Font::FontSetEffectWeight);
	LIB_FUNC("6vGCkkQJOcI", Font::FontSetupRenderScalePixel);
	LIB_FUNC("lz9y9UFO2UU", Font::FontSetupRenderEffectSlant);
	LIB_FUNC("XIGorvLusDQ", Font::FontSetupRenderEffectWeight);
	LIB_FUNC("imxVx8lm+KM", Font::FontGetHorizontalLayout);
	LIB_FUNC("3BrWWFU+4ts", Font::FontGetVerticalLayout);
	LIB_FUNC("8h-SOB-asgk", Font::FontDefineAttribute);
	LIB_FUNC("nWrfPI4Okmg", Font::FontCreateLibrary);
	LIB_FUNC("n590hj5Oe-k", Font::FontCreateLibraryWithEdition);
	LIB_FUNC("MO24vDhmS4E", Font::FontCreateString);
	LIB_FUNC("7rogx92EEyc", Font::FontCreateWritingLine);
	LIB_FUNC("SSCaczu2aMQ", Font::FontDestroyString);
	LIB_FUNC("PEjv7CVDRYs", Font::FontDestroyWritingLine);
	LIB_FUNC("LHDoRWVFGqk", Font::FontDeleteGlyph);
	LIB_FUNC("FXP359ygujs", Font::FontDestroyLibrary);
	LIB_FUNC("C-4Qw5Srlyw", Font::FontGenerateCharGlyph);
	LIB_FUNC("L97d+3OgMlE", Font::FontGetCharGlyphMetrics);
	LIB_FUNC("IQtleGLL5pQ", Font::FontGetRenderCharGlyphMetrics);
	LIB_FUNC("8-zmgsxkBek", Font::FontGlyphDefineAttribute);
	LIB_FUNC("whrS4oksXc4", Font::FontMemoryInit);
	LIB_FUNC("h6hIgxXEiEc", Font::FontMemoryTerm);
	LIB_FUNC("JzCH3SCFnAU", Font::FontOpenFontInstance);
	LIB_FUNC("KXUpebrFk1U", Font::FontOpenFontMemory);
	LIB_FUNC("cKYtVmeSTcw", Font::FontOpenFontSet);
	LIB_FUNC("3G4zhgKuxE8", Font::FontRenderCharGlyphImageHorizontal);
	LIB_FUNC("kAenWy1Zw5o", Font::FontRenderCharGlyphImageHorizontal);
	LIB_FUNC("i6UNdSig1uE", Font::FontRenderCharGlyphImageHorizontal);
	LIB_FUNC("gdUCnU0gHdI", Font::FontRenderSurfaceInit);
	LIB_FUNC("vRxf4d0ulPs", Font::FontRenderSurfaceSetScissor);
	LIB_FUNC("ObkDGDBsVtw", Font::FontStringGetTerminateCode);
	LIB_FUNC("+B-xlbiWDJ4", Font::FontStringGetTerminateOrder);
	LIB_FUNC("o1vIEHeb6tw", Font::FontStringGetWritingForm);
	LIB_FUNC("hq5LffQjz-s", Font::FontStringRefersRenderCharacters);
	LIB_FUNC("Avv7OApgCJk", Font::FontStringRefersTextCharacters);
	LIB_FUNC("oaJ1BpN2FQk", Font::FontTextSourceInit);
	LIB_FUNC("VRFd3diReec", Font::FontTextSourceRewind);
	LIB_FUNC("eCRMCSk96NU", Font::FontTextSourceSetDefaultFont);
	LIB_FUNC("OqQKX0h5COw", Font::FontTextSourceSetWritingForm);
	LIB_FUNC("6DFUkCwQLa8", Font::FontCharacterGetBidiLevel);
	LIB_FUNC("coCrV6IWplE", Font::FontCharacterGetSyllableStringState);
	LIB_FUNC("zN3+nuA0SFQ", Font::FontCharacterGetTextFontCode);
	LIB_FUNC("mxgmMj-Mq-o", Font::FontCharacterGetTextOrder);
	LIB_FUNC("-P6X35Rq2-E", Font::FontCharacterLooksFormatCharacters);
	LIB_FUNC("SaRlqtqaCew", Font::FontCharacterLooksWhiteSpace);
	LIB_FUNC("6Gqlv5KdTbU", Font::FontCharacterRefersTextBack);
	LIB_FUNC("BkjBP+YC19w", Font::FontCharacterRefersTextNext);
	LIB_FUNC("lVSR5ftvNag", Font::FontCharactersRefersTextCodes);
	LIB_FUNC("IPoYwwlMx-g", Font::FontTextCodesStepBack);
	LIB_FUNC("olSmXY+XP1E", Font::FontTextCodesStepNext);
	LIB_FUNC("fljdejMcG1c", Font::FontWritingGetRenderMetrics);
	LIB_FUNC("fD5rqhEXKYQ", Font::FontWritingInit);
	LIB_FUNC("1+DgKL0haWQ", Font::FontWritingLineClear);
	LIB_FUNC("JQKWIsS9joE", Font::FontWritingLineGetOrderingSpace);
	LIB_FUNC("nlU2VnfpqTM", Font::FontWritingLineGetRenderMetrics);
	LIB_FUNC("+FYcYefsVX0", Font::FontWritingLineRefersRenderStep);
	LIB_FUNC("wyKFUOWdu3Q", Font::FontWritingLineWritesOrder);
	LIB_FUNC("W-2WOXEHGck", Font::FontWritingRefersRenderStep);
	LIB_FUNC("f4Onl7efPEY", Font::FontWritingRefersRenderStepCharacter);
	LIB_FUNC("BbCZjJizU4A", Font::FontWritingSetMaskInvisible);
	LIB_FUNC("mz2iTY0MK4A", Font::FontSupportExternalFonts);
	LIB_FUNC("SsRbbCiWoGw", Font::FontSupportSystemFonts);
}

} // namespace Libs
