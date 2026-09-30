#include "libs/libCes.cpp"

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
} // namespace

int main() {
	CesProfile profile {};
	CesContext context {};
	CHECK(CesUcsProfileInitSJis1997Cp932(&profile) == &profile);
	CHECK(CesMbcsUcsContextInit(&context, &profile) == 0);
	TestMappingsAndMeasurement(&context);
	TestPartialOutput(&context);
	TestErrors(&context);
	return failures == 0 ? 0 : 1;
}
