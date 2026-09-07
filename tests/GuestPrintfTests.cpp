#include "libs/guestPrintf.h"
#include "libs/vaContext.h"

#include <algorithm>
#include <array>
#include <climits>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>

namespace {

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "GuestPrintfTests: %s\n", message);
		std::abort();
	}
}

// Model the guest SysV register-save area, independently of the host va_list.
// snprintf's three fixed arguments occupy GP slots 0..2. Integer/pointer args
// use the remaining GP slots; doubles use the low eight bytes of separate
// 16-byte FP slots. Once a class is exhausted its arguments go on the stack,
// in argument order. Only these scalar argument types are needed by this test.
struct GuestArguments {
	alignas(16) Libs::VaContext context {};
	alignas(16) std::array<uint64_t, 64> stack {};
	size_t gp_count = 0;
	size_t fp_count = 0;
	size_t stack_count = 0;

	GuestArguments() {
		context.va_list.reg_save_area = &context.reg_save_area;
		context.va_list.gp_offset = offsetof(Libs::VaRegSave, gp);
		context.va_list.fp_offset = offsetof(Libs::VaRegSave, fp);
		context.va_list.overflow_arg_area = stack.data();
	}

	template <typename T>
	void Add(T value) {
		static_assert(std::is_integral_v<T> || std::is_pointer_v<T> ||
		              std::is_same_v<T, double>);
		static_assert(sizeof(T) <= sizeof(uint64_t));
		void* slot = nullptr;
		if constexpr (std::is_same_v<T, double>) {
			if (fp_count < 8) {
				slot = &context.reg_save_area.fp[fp_count];
			}
			++fp_count;
		} else {
			if (gp_count < 6) {
				slot = &context.reg_save_area.gp[gp_count];
			}
			++gp_count;
		}
		if (slot == nullptr) {
			Check(stack_count < stack.size(), "argument fixture stack overflow");
			slot = &stack[stack_count++];
		}
		std::memcpy(slot, &value, sizeof(value));
	}

	void CheckConsumed() const {
		Check(context.va_list.gp_offset == std::min(gp_count, size_t {6}) * 8,
		      "wrong GP argument consumption");
		Check(context.va_list.fp_offset == offsetof(Libs::VaRegSave, fp) +
		                                      std::min(fp_count, size_t {8}) * 16,
		      "wrong FP argument consumption");
		Check(context.va_list.overflow_arg_area == stack.data() + stack_count,
		      "wrong overflow argument consumption");
	}
};

size_t g_cases = 0;
size_t g_failures = 0;

template <typename... Args>
void Compare(const char* format, Args... args) {
	// Keep enough room for every result: truncation and n == 0 have separate
	// known limitations and are not part of the precision-parser regression.
	std::array<char, 512> expected;
	std::array<char, 512> actual;
	expected.fill('~');
	actual.fill('~');
	const int expected_length = std::snprintf(expected.data(), expected.size(), format, args...);
	Check(expected_length >= 0 && static_cast<size_t>(expected_length) < expected.size(),
	      "host snprintf failed or fixture buffer too small");

	GuestArguments guest;
	guest.Add(actual.data());
	guest.Add(actual.size());
	guest.Add(format);
	(guest.Add(args), ...);
	const int actual_length = Libs::GetGuestSnprintfCtxFunc()(&guest.context);
	guest.CheckConsumed();

	++g_cases;
	// Compare the terminator and untouched tail as well as the output and
	// returned byte count. Do not rely on assert(), which Release disables.
	if (actual_length != expected_length || actual != expected) {
		++g_failures;
		std::fprintf(stderr,
		             "case %zu format=\"%s\": expected length=%d \"%s\", "
		             "got length=%d \"%.*s\"\n",
		             g_cases, format, expected_length, expected.data(), actual_length,
		             static_cast<int>(actual.size()), actual.data());
	}
}

void CheckPrecisionMatrix() {
	// INT_MIN ensures a negative precision is ignored, not negated or cast to
	// an enormous unsigned precision. Zero and positive values are controls.
	for (int precision: {-1, -7, INT_MIN, 0, 1, 3, 6}) {
		std::printf("precision=%d\n", precision);
		for (const char* format: {"%.*d", "%08.*d", "%+08.*d", "%8.*i"}) {
			for (int value: {0, 42, -42}) {
				Compare(format, precision, value);
			}
		}
		for (const char* format: {"%.*u", "%08.*u", "%.*x", "%08.*X", "%.*o"}) {
			for (unsigned int value: {0U, 42U}) {
				Compare(format, precision, value);
			}
		}
		for (const char* format: {"%.*lld", "%012.*lld"}) {
			for (long long value: {0LL, 123456789LL, -123456789LL}) {
				Compare(format, precision, value);
			}
		}
		Compare("%.*hhd|%.*hd", precision, 0, precision, 0);
		for (const char* format: {"%.*s", "[%8.*s]", "[%-8.*s]"}) {
			for (const char* value: {"", "abcdef"}) {
				Compare(format, precision, value);
			}
		}
		for (const char* format: {"%.*f", "%.*F", "%012.*f", "%-12.*f"}) {
			for (double value: {0.0, 1.25, -1.25, 12.375}) {
				Compare(format, precision, value);
			}
		}
		for (const char* format: {"%.*e", "%.*E", "%014.*e"}) {
			Compare(format, precision, 1.25);
			Compare(format, precision, -1.25);
		}
		// Width and precision are GP arguments even for a floating conversion.
		Compare("[%0*.*d]", 8, precision, 0);
		Compare("[%*.*s]", -8, precision, "abcdef");
		Compare("[%0*.*f]", 12, precision, 1.25);
	}
}

void CheckArgumentPlacement() {
	for (int precision: {-1, 0, 3}) {
		// Precision in the last GP register, then precision on the stack.
		// The following conversion verifies that .* always consumes its int.
		Compare("%d:%d:%.*d:%d", 11, 22, precision, 0, 77);
		Compare("%d:%d:%d:%.*s:%d", 11, 22, 33, precision, "abcdef", 77);
		// GP exhaustion must not cause a double to be read from the stack
		// while there are FP registers left.
		Compare("%d:%d:%d:%0*.*f:%d", 11, 22, 33, 12, precision, 1.25, 77);
		// Exhaust both classes. The ninth double follows a stack precision;
		// the next integer follows that double in the same overflow area.
		Compare("%.*f|%.*f|%.*f|%.*f|%.*f|%.*f|%.*f|%.*f|%.*f|%d",
		        precision, 1.25, precision, 2.25, precision, 3.25,
		        precision, 4.25, precision, 5.25, precision, 6.25,
		        precision, 7.25, precision, 8.25, precision, 9.25, 77);
	}
	// Precision state is per conversion, not sticky across a format string.
	Compare("%.*s|%.*s|%.*s|%s", 2, "abcdef", -1, "abcdef", 0, "abcdef", "tail");
	Compare("%.*f|%.*f|%.*f|%f", 2, 1.25, -1, 1.25, 0, 1.25, 1.25);
	Compare("%08.*d|%08.*d|%08.*d|%08d", 3, 42, -1, 42, 0, 0, 42);
}

} // namespace

int main() {
	Check(std::setlocale(LC_ALL, "C") != nullptr, "could not select C locale");
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	Compare("");
	Compare("literal %%");
	Compare("%d|%s|%f", 0, "", 0.0);
	Compare("%.d|%.s|%.f", 0, "abcdef", 1.25);
	Compare("%.0d|%.0s|%.0f", 0, "abcdef", 1.25);
	CheckPrecisionMatrix();
	CheckArgumentPlacement();
	std::printf("GuestPrintfTests: %zu cases, %zu failures\n", g_cases, g_failures);
	return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
