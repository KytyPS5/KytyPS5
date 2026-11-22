#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <vector>

namespace Libs {
void InitFont_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

namespace {

struct GlyphDetail {
	uint16_t    detail_id;
	uint16_t    reserved;
	uint16_t    form_options;
	uint8_t     glyph_form;
	uint8_t     metrics_form;
	const void* memory;
	void*       reserved2;
	void*       reserved1;
};

struct GlyphOutline {
	int16_t  contours_count;
	int16_t  points_count;
	uint32_t flags;
	float (*points)[2];
	uint8_t*  point_tags;
	uint16_t* contour_indexes;
};

struct GlyphMetrics {
	float width;
	float height;
	float horizontal_bearing_x;
	float horizontal_bearing_y;
	float horizontal_advance;
	float vertical[3];
};

TEST(Font, GlyphRefersOutlineOnlyForOutlineGlyphs) {
	std::ifstream              file(KYTY_NOTO_SANS_DIR "/NotoSans-Regular.ttf", std::ios::binary);
	const std::vector<uint8_t> data {std::istreambuf_iterator<char>(file), {}};
	ASSERT_FALSE(data.empty());

	Loader::SymbolDatabase symbols;
	Libs::InitFont_1(&symbols);
	const auto find = [&](const char* nid) {
		return symbols.FindByNid(nid, Loader::SymbolType::Func)->vaddr;
	};
	const auto open_memory =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(void*, const void*, uint32_t, const void*, void**)>(
	        find("KXUpebrFk1U"));
	const auto set_scale =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(void*, float, float)>(find("N1EBMeGhf7E"));
	const auto get_metrics =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(void*, uint32_t, GlyphMetrics*)>(find("L97d+3OgMlE"));
	const auto generate =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(void*, uint32_t, const GlyphDetail*, void**)>(
	        find("C-4Qw5Srlyw"));
	const auto refers_outline =
	    reinterpret_cast<const GlyphOutline*(KYTY_SYSV_ABI*)(void*)>(find("R1T4i+DOhNY"));

	void* font = nullptr;
	ASSERT_EQ(open_memory(nullptr, data.data(), static_cast<uint32_t>(data.size()), nullptr, &font),
	          0);
	ASSERT_EQ(set_scale(font, 48.0f, 64.0f), 0);

	const GlyphDetail outline_detail {0x0fd3, 0, 0, 1, 1};
	const GlyphDetail private_detail {0x0fd3, 0, 0, 0, 0};
	void*             l_glyph       = nullptr;
	void*             o_glyph       = nullptr;
	void*             null_glyph    = nullptr;
	void*             private_glyph = nullptr;
	ASSERT_EQ(generate(font, 'L', &outline_detail, &l_glyph), 0);
	ASSERT_EQ(generate(font, 'O', &outline_detail, &o_glyph), 0);
	ASSERT_EQ(generate(font, 'L', nullptr, &null_glyph), 0);
	ASSERT_EQ(generate(font, 'L', &private_detail, &private_glyph), 0);

	// L is one contour of six on-curve corners, without its start point repeated.
	const auto* l = refers_outline(l_glyph);
	ASSERT_NE(l, nullptr);
	EXPECT_EQ(l->flags, 0u);
	ASSERT_EQ(l->contours_count, 1);
	ASSERT_EQ(l->points_count, 6);
	EXPECT_EQ(l->contour_indexes[0], 5);
	EXPECT_TRUE(std::all_of(l->point_tags, l->point_tags + 6, [](uint8_t t) { return t == 1; }));

	// Pixels with y up from the baseline origin: the corners span the glyph's metrics box.
	GlyphMetrics metrics {};
	ASSERT_EQ(get_metrics(font, 'L', &metrics), 0);
	const auto [min_x, max_x] = std::minmax_element(
	    l->points, l->points + 6, [](const auto& a, const auto& b) { return a[0] < b[0]; });
	const auto [min_y, max_y] = std::minmax_element(
	    l->points, l->points + 6, [](const auto& a, const auto& b) { return a[1] < b[1]; });
	EXPECT_FLOAT_EQ((*min_x)[0], metrics.horizontal_bearing_x);
	EXPECT_FLOAT_EQ((*max_y)[1], metrics.horizontal_bearing_y);
	EXPECT_FLOAT_EQ((*max_x)[0] - (*min_x)[0], metrics.width);
	EXPECT_FLOAT_EQ((*max_y)[1] - (*min_y)[1], metrics.height);

	// O is two curved contours that start on-curve and do not repeat their start point.
	const auto* o = refers_outline(o_glyph);
	ASSERT_NE(o, nullptr);
	ASSERT_EQ(o->contours_count, 2);
	EXPECT_LT(o->contour_indexes[0], o->contour_indexes[1]);
	EXPECT_EQ(o->contour_indexes[1], o->points_count - 1);
	EXPECT_NE(std::find(o->point_tags, o->point_tags + o->points_count, 0),
	          o->point_tags + o->points_count);
	const int begins[] = {0, o->contour_indexes[0] + 1};
	for (int c = 0; c < 2; c++) {
		const auto& first = o->points[begins[c]];
		const auto& last  = o->points[o->contour_indexes[c]];
		EXPECT_EQ(o->point_tags[begins[c]], 1);
		EXPECT_FALSE(first[0] == last[0] && first[1] == last[1]);
	}

	EXPECT_EQ(refers_outline(null_glyph), nullptr);
	EXPECT_EQ(refers_outline(private_glyph), nullptr);
	EXPECT_EQ(refers_outline(&metrics), nullptr);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
