#include "libs/libCes.cpp"

#include <algorithm>
#include <string>
#include <vector>

namespace Loader {
void SymbolDatabase::Add(const SymbolResolve&, uint64_t, const std::string&) {}
namespace Timer {
double GetTimeMs() {
	return 0.0;
}
} // namespace Timer
} // namespace Loader

namespace {
using namespace Libs::LibCes;

int failures = 0;

#define CHECK(condition)                                                                           \
	do {                                                                                           \
		if (!(condition)) {                                                                        \
			std::fprintf(stderr, "CesTests:%d: %s\n", __LINE__, #condition);                       \
			++failures;                                                                            \
		}                                                                                          \
	} while (false)

void CheckConversion(CesContext* context, const std::vector<uint8_t>& source,
                     const std::string& expected) {
	std::vector<uint8_t> output(expected.size() + 2, 0xcc);
	uint32_t             consumed = 0;
	uint32_t             produced = 0;
	CHECK(CesMbcsStrToUtf8Str(context, source.data(), 0, &consumed, output.data(),
	                          static_cast<uint32_t>(expected.size() + 1), &produced) == 0);
	CHECK(consumed == source.size() - 1);
	CHECK(produced == expected.size());
	CHECK(std::memcmp(output.data(), expected.data(), expected.size()) == 0);
	CHECK(output[expected.size()] == 0);
	CHECK(output.back() == 0xcc);
	CHECK(CesMbcsStrGetUtf8Len(context, source.data(), 0, &consumed, &produced) == 0);
	CHECK(consumed == source.size() - 1);
	CHECK(produced == expected.size());
}

void TestMappingsAndMeasurement(CesContext* context) {
	CheckConversion(context, {0}, "");
	CheckConversion(context, {'S', 'a', 'v', 'e', ' ', 0x93, 0xfa, 0x96, 0x7b, 0xb6, 0},
	                "Save \xe6\x97\xa5\xe6\x9c\xac\xef\xbd\xb6");
	// CP932 maps this character to U+FF5E, unlike strict Shift-JIS (U+301C).
	CheckConversion(context, {0x81, 0x60, 0}, "\xef\xbd\x9e");
	std::vector<uint8_t> source;
	std::string          expected;
	for (int i = 0; i < 200; ++i) {
		source.insert(source.end(), {0x82, 0xa0});
		expected += "\xe3\x81\x82";
	}
	source.push_back(0);
	CheckConversion(context, source, expected);
}

void TestPartialOutput(CesContext* context) {
	const uint8_t source[] = {'A', 0x82, 0xa0, 'B', 0};
	for (uint32_t capacity = 1; capacity <= 7; ++capacity) {
		std::array<uint8_t, 8> output;
		output.fill(0xcc);
		uint32_t  consumed = 99;
		uint32_t  produced = 99;
		const int status =
		    CesMbcsStrToUtf8Str(context, source, 0, &consumed, output.data(), capacity, &produced);
		const uint32_t expected_consumed = capacity == 1   ? 0
		                                   : capacity < 5  ? 1
		                                   : capacity == 5 ? 3
		                                                   : 4;
		const uint32_t expected_produced = capacity == 1   ? 0
		                                   : capacity < 5  ? 1
		                                   : capacity == 5 ? 4
		                                                   : 5;
		CHECK(status == (capacity < 6 ? CES_ERROR_DST_BUFFER_END : 0));
		CHECK(consumed == expected_consumed);
		CHECK(produced == expected_produced);
		CHECK(std::memcmp(output.data(),
		                  "A\xe3\x81\x82"
		                  "B",
		                  expected_produced) == 0);
		CHECK(output[expected_produced] == 0);
		CHECK(output[capacity] == 0xcc);
	}
}

void TestErrors(CesContext* context) {
	struct Case {
		std::vector<uint8_t> source;
		uint32_t             limit;
		int                  error;
	};
	const Case cases[] = {
	    {{'A', 0x80, 0}, 0, CES_ERROR_INVALID_ENCODE},
	    {{'A', 0x82, 0}, 0, CES_ERROR_INVALID_ENCODE},
	    {{'A', 0x82, 0xa0, 0}, 2, CES_ERROR_SRC_BUFFER_END},
	    {{'A', 0x82, 0x20, 0}, 0, CES_ERROR_INVALID_ENCODE},
	    {{'A', 0x81, 0xad, 0}, 0, CES_ERROR_UNASSIGNED_CODE},
	};
	for (const auto& test: cases) {
		std::array<uint8_t, 16> output {};
		uint32_t                consumed = 99;
		uint32_t                produced = 99;
		CHECK(CesMbcsStrToUtf8Str(context, test.source.data(), test.limit, &consumed, output.data(),
		                          output.size(), &produced) == test.error);
		CHECK(consumed == 1 && produced == 1);
		CHECK(output[0] == 'A' && output[1] == 0);
		CHECK(CesMbcsStrGetUtf8Len(context, test.source.data(), test.limit, &consumed, &produced) ==
		      test.error);
		CHECK(consumed == 1 && produced == 1);
	}
}
void CheckUtf16Conversion(CesContext* context, const std::vector<uint8_t>& source,
                          const std::u16string& expected) {
	std::vector<uint16_t> output(expected.size() + 2, 0xcccc);
	uint32_t              consumed = 0;
	uint32_t              produced = 0;
	CHECK(CesMbcsStrToUtf16Str(context, source.data(), 0, &consumed, output.data(),
	                           static_cast<uint32_t>(expected.size() + 1), &produced) == 0);
	CHECK(consumed == source.size() - 1);
	CHECK(produced == expected.size());
	CHECK(std::equal(expected.begin(), expected.end(), output.begin()));
	CHECK(output[expected.size()] == 0);
	CHECK(output.back() == 0xcccc);
	CHECK(CesMbcsStrGetUtf16Len(context, source.data(), 0, &consumed, &produced) == 0);
	CHECK(consumed == source.size() - 1);
	CHECK(produced == expected.size());
}

void TestUtf16MappingsAndMeasurement(CesContext* context) {
	CheckUtf16Conversion(context, {0}, u"");
	CheckUtf16Conversion(context, {'S', 'a', 'v', 'e', ' ', 0x93, 0xfa, 0x96, 0x7b, 0xb6, 0},
	                     u"Save \u65e5\u672c\uff76");
	CheckUtf16Conversion(context, {0x81, 0x60, 0}, u"\uff5e");
	std::vector<uint8_t> source;
	std::u16string       expected;
	for (int i = 0; i < 200; ++i) {
		source.insert(source.end(), {0x82, 0xa0});
		expected += u'\u3042';
	}
	source.push_back(0);
	CheckUtf16Conversion(context, source, expected);
}

void TestUtf16PartialOutput(CesContext* context) {
	const uint8_t  source[]   = {'A', 0x82, 0xa0, 'B', 0};
	const uint16_t expected[] = {'A', 0x3042, 'B'};
	for (uint32_t capacity = 1; capacity <= 5; ++capacity) {
		std::array<uint16_t, 6> output;
		output.fill(0xcccc);
		uint32_t  consumed = 99;
		uint32_t  produced = 99;
		const int status =
		    CesMbcsStrToUtf16Str(context, source, 0, &consumed, output.data(), capacity, &produced);
		const uint32_t expected_produced = capacity - 1 < 3 ? capacity - 1 : 3;
		const uint32_t expected_consumed = expected_produced == 0   ? 0
		                                   : expected_produced == 1 ? 1
		                                   : expected_produced == 2 ? 3
		                                                            : 4;
		CHECK(status == (capacity < 4 ? CES_ERROR_DST_BUFFER_END : 0));
		CHECK(consumed == expected_consumed);
		CHECK(produced == expected_produced);
		CHECK(std::equal(expected, expected + expected_produced, output.begin()));
		CHECK(output[expected_produced] == 0);
		CHECK(output[capacity] == 0xcccc);
	}
}

void TestUtf16Errors(CesContext* context) {
	std::array<uint16_t, 8> output {};
	uint32_t                consumed = 99;
	uint32_t                produced = 99;
	const uint8_t           invalid[] = {'A', 0x80, 0};
	CHECK(CesMbcsStrToUtf16Str(context, invalid, 0, &consumed, output.data(), output.size(),
	                           &produced) == CES_ERROR_INVALID_ENCODE);
	CHECK(consumed == 1 && produced == 1);
	CHECK(output[0] == 'A' && output[1] == 0);
	const uint8_t truncated[] = {'A', 0x82, 0xa0, 0};
	CHECK(CesMbcsStrGetUtf16Len(context, truncated, 2, &consumed, &produced) ==
	      CES_ERROR_SRC_BUFFER_END);
	CHECK(consumed == 1 && produced == 1);
	CHECK(CesMbcsStrToUtf16Str(context, invalid, 0, &consumed, nullptr, 4, &produced) ==
	      CES_ERROR_INVALID_DST_BUFFER);
	CHECK(CesMbcsStrToUtf16Str(nullptr, invalid, 0, &consumed, output.data(), output.size(),
	                           &produced) == CES_ERROR_INVALID_PARAMETER);
}

uint8_t ToSbc(const uint8_t* profile, const char* utf8) {
	uint8_t  sbc      = 0;
	uint32_t consumed = 0;
	CHECK(CesUtf8ToSbc(reinterpret_cast<const uint8_t*>(utf8),
	                   static_cast<uint32_t>(std::strlen(utf8)), &consumed, profile, &sbc) == 0);
	CHECK(consumed == std::strlen(utf8));
	return sbc;
}

std::string FromSbc(const uint8_t* profile, uint8_t sbc) {
	uint8_t  utf8[4] {};
	uint32_t len = 0;
	CHECK(CesSbcToUtf8(profile, sbc, utf8, sizeof(utf8), &len) == 0);
	return {reinterpret_cast<const char*>(utf8), len};
}

void TestSbcProfiles() {
	const auto* cp1252 = CesRefersUcsProfileCp1252();
	const auto* latin9 = CesRefersUcsProfileIso8859_15();
	CHECK(cp1252 != nullptr && latin9 != nullptr && cp1252 != latin9);

	// The euro sign lives in a different byte in each code page.
	CHECK(ToSbc(cp1252, "\xe2\x82\xac") == 0x80);
	CHECK(ToSbc(latin9, "\xe2\x82\xac") == 0xa4);
	CHECK(FromSbc(cp1252, 0x80) == "\xe2\x82\xac");
	CHECK(FromSbc(latin9, 0xa4) == "\xe2\x82\xac");
	CHECK(FromSbc(cp1252, 0xa4) == "\xc2\xa4");

	// ISO-8859-15 keeps C1 controls and Latin-1 outside its eight replaced positions.
	CHECK(FromSbc(latin9, 0x80) == "\xc2\x80");
	CHECK(FromSbc(latin9, 0xe9) == "\xc3\xa9");
	CHECK(FromSbc(latin9, 'A') == "A");
	CHECK(ToSbc(latin9, "\xc3\xa9") == 0xe9);
	const struct {
		uint8_t     sbc;
		const char* utf8;
	} replaced[] = {
	    {0xa6, "\xc5\xa0"}, {0xa8, "\xc5\xa1"}, {0xb4, "\xc5\xbd"}, {0xb8, "\xc5\xbe"},
	    {0xbc, "\xc5\x92"}, {0xbd, "\xc5\x93"}, {0xbe, "\xc5\xb8"},
	};
	for (const auto& map: replaced) {
		CHECK(FromSbc(latin9, map.sbc) == map.utf8);
		CHECK(ToSbc(latin9, map.utf8) == map.sbc);
	}

	// Characters without a byte in the code page fall back to '?'.
	CHECK(ToSbc(latin9, "\xc2\xa4") == '?');
	CHECK(ToSbc(latin9, "\xe3\x81\x82") == '?');
	CHECK(ToSbc(cp1252, "\xe3\x81\x82") == '?');
}
} // namespace

int main() {
	CesProfile profile {};
	CesContext context {};
	CHECK(CesUcsProfileInitSJis1997Cp932(&profile) == &profile);
	CHECK(CesMbcsUcsContextInit(&context, &profile) == 0);
	TestMappingsAndMeasurement(&context);
	TestPartialOutput(&context);
	TestErrors(&context);
	TestUtf16MappingsAndMeasurement(&context);
	TestUtf16PartialOutput(&context);
	TestUtf16Errors(&context);
	TestSbcProfiles();
	return failures == 0 ? 0 : 1;
}
