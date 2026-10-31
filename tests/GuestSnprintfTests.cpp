#include "libs/guestPrintf.h"
#include "libs/vaContext.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

using Libs::VaContext;

// Marks bytes the formatter must not touch.
constexpr unsigned char GUARD_BYTE  = 0xAA;
constexpr size_t        GUARD_SIZE  = 8;
constexpr size_t        BUFFER_SIZE = 16;

/// A destination buffer with guard bytes on both sides so an overrun or underrun shows.
struct GuardedBuffer {
	unsigned char before[GUARD_SIZE];
	char          data[BUFFER_SIZE];
	unsigned char after[GUARD_SIZE];

	GuardedBuffer() {
		std::memset(before, GUARD_BYTE, sizeof(before));
		std::memset(data, GUARD_BYTE, sizeof(data));
		std::memset(after, GUARD_BYTE, sizeof(after));
	}

	/// True when every guard byte before and after the data is unchanged.
	bool GuardsIntact() const {
		for (size_t i = 0; i < GUARD_SIZE; i++) {
			if (before[i] != GUARD_BYTE || after[i] != GUARD_BYTE) {
				return false;
			}
		}
		return true;
	}

	/// True when data bytes from first up to the end are still untouched.
	bool UntouchedFrom(size_t first) const {
		for (size_t i = first; i < BUFFER_SIZE; i++) {
			if (static_cast<unsigned char>(data[i]) != GUARD_BYTE) {
				return false;
			}
		}
		return true;
	}
};

/// Aborts the test run with a message when a condition does not hold.
void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "GuestSnprintfTests: failed: %s\n", text);
		std::abort();
	}
}

/// Calls the guest snprintf with the arguments laid out as the SysV ABI passes them. The
/// first variadic argument lands in gp[3], so up to three arguments fit in registers.
int Snprintf(char* dest, size_t dest_size, const char* format, uint64_t arg0 = 0, uint64_t arg1 = 0,
             uint64_t arg2 = 0) {
	alignas(16) VaContext ctx {};
	uint64_t              overflow_area = 0;

	ctx.reg_save_area.gp[0]       = reinterpret_cast<uint64_t>(dest);
	ctx.reg_save_area.gp[1]       = dest_size;
	ctx.reg_save_area.gp[2]       = reinterpret_cast<uint64_t>(format);
	ctx.reg_save_area.gp[3]       = arg0;
	ctx.reg_save_area.gp[4]       = arg1;
	ctx.reg_save_area.gp[5]       = arg2;
	ctx.va_list.reg_save_area     = &ctx.reg_save_area;
	ctx.va_list.gp_offset         = offsetof(Libs::VaRegSave, gp);
	ctx.va_list.fp_offset         = offsetof(Libs::VaRegSave, fp);
	ctx.va_list.overflow_arg_area = &overflow_area;

	return Libs::GetGuestSnprintfCtxFunc()(&ctx);
}

/// Output shorter than the buffer is written whole with a terminator. This is the baseline
/// that the truncation cases are compared against.
void TestFits() {
	GuardedBuffer buffer;
	const int     length = Snprintf(buffer.data, BUFFER_SIZE, "abc%d", 7);

	Check(length == 4, "fitting output returns its length");
	Check(std::strcmp(buffer.data, "abc7") == 0, "fitting output is written whole");
	Check(buffer.GuardsIntact(), "fitting output stays inside the buffer");
}

/// Output of exactly n-1 characters fits with its terminator in the last byte.
void TestExactlyNMinusOne() {
	GuardedBuffer buffer;
	const int     length = Snprintf(buffer.data, 6, "hello");

	Check(length == 5, "n-1 output returns its length");
	Check(std::memcmp(buffer.data, "hello", 6) == 0, "n-1 output is not cut and is terminated");
	Check(buffer.UntouchedFrom(6) && buffer.GuardsIntact(), "n-1 output writes nothing past n");
}

/// Output longer than n-1 is cut to n-1 characters plus a terminator, the return value stays
/// the untruncated length and nothing past n is written.
void TestTruncated() {
	GuardedBuffer buffer;
	const int     length = Snprintf(buffer.data, 4, "hello");

	Check(length == 5, "truncated output returns the full length");
	Check(std::memcmp(buffer.data, "hel\0", 4) == 0, "truncated output keeps n-1 characters");
	Check(buffer.UntouchedFrom(4) && buffer.GuardsIntact(), "truncation writes nothing past n");
}

/// With n of 0 nothing is written, the destination may be null and the full length returns.
void TestSizeZero() {
	Check(Snprintf(nullptr, 0, "hello") == 5, "n of 0 with a null buffer returns the length");

	GuardedBuffer buffer;
	Check(Snprintf(buffer.data, 0, "hello") == 5, "n of 0 returns the length");
	Check(buffer.UntouchedFrom(0) && buffer.GuardsIntact(), "n of 0 writes nothing");
}

/// With n of 1 only the terminator fits, which gives an empty string.
void TestSizeOne() {
	GuardedBuffer buffer;
	const int     length = Snprintf(buffer.data, 1, "hello");

	Check(length == 5, "n of 1 returns the full length");
	Check(buffer.data[0] == '\0', "n of 1 gives an empty string");
	Check(buffer.UntouchedFrom(1) && buffer.GuardsIntact(), "n of 1 writes only the terminator");
}

/// A %c of 0 puts a NUL byte in the output. The bytes after it are still copied and counted.
void TestEmbeddedNul() {
	GuardedBuffer buffer;
	const int     length = Snprintf(buffer.data, BUFFER_SIZE, "a%cb", 0);

	Check(length == 3, "embedded NUL is counted");
	Check(std::memcmp(buffer.data, "a\0b\0", 4) == 0, "bytes after an embedded NUL are copied");

	GuardedBuffer cut;
	Check(Snprintf(cut.data, 3, "a%cbcd", 0) == 5, "truncation after a NUL returns the length");
	Check(std::memcmp(cut.data, "a\0\0", 3) == 0, "truncation after a NUL keeps n-1 bytes");
	Check(cut.UntouchedFrom(3) && cut.GuardsIntact(), "truncation after a NUL stays inside n");
}

} // namespace

int main() {
	TestFits();
	TestExactlyNMinusOne();
	TestTruncated();
	TestSizeZero();
	TestSizeOne();
	TestEmbeddedNul();
	std::puts("GuestSnprintfTests: ok");
	return 0;
}
