//
// Original algorithm is from:
// 	https://github.com/mpaland/printf
// 	Marco Paland (info@paland.com)
// 	2014-2019, PALANDesign Hannover, Germany
//  licensed under The MIT License (MIT)

#include "libs/guestPrintf.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "libs/vaContext.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Libs {

constexpr uint32_t FLAGS_ZEROPAD   = (1U << 0U);
constexpr uint32_t FLAGS_LEFT      = (1U << 1U);
constexpr uint32_t FLAGS_PLUS      = (1U << 2U);
constexpr uint32_t FLAGS_SPACE     = (1U << 3U);
constexpr uint32_t FLAGS_HASH      = (1U << 4U);
constexpr uint32_t FLAGS_UPPERCASE = (1U << 5U);
constexpr uint32_t FLAGS_CHAR      = (1U << 6U);
constexpr uint32_t FLAGS_SHORT     = (1U << 7U);
constexpr uint32_t FLAGS_LONG      = (1U << 8U);
constexpr uint32_t FLAGS_LONG_LONG = (1U << 9U);
constexpr uint32_t FLAGS_PRECISION = (1U << 10U);

constexpr size_t PRINTF_NTOA_BUFFER_SIZE = 32U;

using out_fct_type = void (*)(char character, std::vector<char>* buffer, size_t idx,
                              size_t /*maxlen*/);

// internal null output
static inline void _out_null(char character, std::vector<char>* buffer, size_t /*idx*/,
                             size_t /*maxlen*/) {
	buffer->push_back(character);
}

static inline bool _is_digit(char ch) {
	return (ch >= '0') && (ch <= '9');
}

static unsigned int _atoi(const char** str) {
	unsigned int i = 0U;
	while (_is_digit(**str)) {
		i = i * 10U + static_cast<unsigned int>(*((*str)++) - '0');
	}
	return i;
}

static size_t _out_rev(out_fct_type out, std::vector<char>* buffer, size_t idx, size_t maxlen,
                       const char* buf, size_t len, unsigned int width, unsigned int flags) {
	const size_t start_idx = idx;

	// pad spaces up to given width
	if ((flags & FLAGS_LEFT) == 0 && (flags & FLAGS_ZEROPAD) == 0) {
		for (size_t i = len; i < width; i++) {
			out(' ', buffer, idx++, maxlen);
		}
	}

	// reverse string
	while (len != 0u) {
		out(buf[--len], buffer, idx++, maxlen);
	}

	// append pad spaces up to given width
	if ((flags & FLAGS_LEFT) != 0u) {
		while (idx - start_idx < width) {
			out(' ', buffer, idx++, maxlen);
		}
	}

	return idx;
}

// internal itoa format
static size_t _ntoa_format(out_fct_type out, std::vector<char>* buffer, size_t idx, size_t maxlen,
                           char* buf, size_t len, bool negative, unsigned int base,
                           unsigned int prec, unsigned int width, unsigned int flags) {
	// pad leading zeros
	if ((flags & FLAGS_LEFT) == 0u) {
		if ((width != 0u) && ((flags & FLAGS_ZEROPAD) != 0u) &&
		    (negative || ((flags & (FLAGS_PLUS | FLAGS_SPACE)) != 0u))) {
			width--;
		}
		while ((len < prec) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
			buf[len++] = '0';
		}
		while (((flags & FLAGS_ZEROPAD) != 0u) && (len < width) &&
		       (len < PRINTF_NTOA_BUFFER_SIZE)) {
			buf[len++] = '0';
		}
	}

	// handle hash
	if ((flags & FLAGS_HASH) != 0u) {
		if (((flags & FLAGS_PRECISION) == 0u) && (len != 0u) && ((len == prec) || (len == width))) {
			len--;
			if ((len != 0u) && (base == 16U)) {
				len--;
			}
		}
		if ((base == 16U) && ((flags & FLAGS_UPPERCASE) == 0u) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
			buf[len++] = 'x';
		} else if ((base == 16U) && ((flags & FLAGS_UPPERCASE) != 0u) &&
		           (len < PRINTF_NTOA_BUFFER_SIZE)) {
			buf[len++] = 'X';
		} else if ((base == 2U) && (len < PRINTF_NTOA_BUFFER_SIZE)) {
			buf[len++] = 'b';
		}
		if (len < PRINTF_NTOA_BUFFER_SIZE) {
			buf[len++] = '0';
		}
	}

	if (len < PRINTF_NTOA_BUFFER_SIZE) {
		if (negative) {
			buf[len++] = '-';
		} else if ((flags & FLAGS_PLUS) != 0u) {
			buf[len++] = '+'; // ignore the space if the '+' exists
		} else if ((flags & FLAGS_SPACE) != 0u) {
			buf[len++] = ' ';
		}
	}

	return _out_rev(out, buffer, idx, maxlen, buf, len, width, flags);
}

static size_t _ntoa_long_long(out_fct_type out, std::vector<char>* buffer, size_t idx,
                              size_t maxlen, uint64_t value, bool negative, uint64_t base,
                              unsigned int prec, unsigned int width, unsigned int flags) {
	char   buf[PRINTF_NTOA_BUFFER_SIZE];
	size_t len = 0U;

	// no hash for 0 values
	if (value == 0u) {
		flags &= ~FLAGS_HASH;
	}

	// write if precision != 0 and value is != 0
	if (((flags & FLAGS_PRECISION) == 0u) || (value != 0u)) {
		do {
			const char digit = static_cast<char>(value % base);
			// NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
			buf[len++] = digit < 10 ? '0' + digit
			                        : ((flags & FLAGS_UPPERCASE) != 0u ? 'A' : 'a') + digit - 10;
			value /= base;
		} while ((value != 0u) && (len < PRINTF_NTOA_BUFFER_SIZE));
	}

	return _ntoa_format(out, buffer, idx, maxlen, buf, len, negative,
	                    static_cast<unsigned int>(base), prec, width, flags);
}

// internal itoa for 'long' type
static size_t _ntoa_long(out_fct_type out, std::vector<char>* buffer, size_t idx, size_t maxlen,
                         uint32_t value, bool negative, uint32_t base, unsigned int prec,
                         unsigned int width, unsigned int flags) {
	char   buf[PRINTF_NTOA_BUFFER_SIZE];
	size_t len = 0U;

	// no hash for 0 values
	if (value == 0u) {
		flags &= ~FLAGS_HASH;
	}

	// write if precision != 0 and value is != 0
	if (((flags & FLAGS_PRECISION) == 0u) || (value != 0u)) {
		do {
			char digit = static_cast<char>(value % base);
			// NOLINTNEXTLINE(bugprone-narrowing-conversions,cppcoreguidelines-narrowing-conversions)
			buf[len++] = digit < 10 ? '0' + digit
			                        : ((flags & FLAGS_UPPERCASE) != 0u ? 'A' : 'a') + digit - 10;
			value /= base;
		} while ((value != 0u) && (len < PRINTF_NTOA_BUFFER_SIZE));
	}

	return _ntoa_format(out, buffer, idx, maxlen, buf, len, negative,
	                    static_cast<unsigned int>(base), prec, width, flags);
}

// Formats one floating-point conversion (f, F, e, E, g, G) with the host printf. The guest
// and the host share the x86-64 IEEE-754 double, so this reproduces the guest libc output:
// every digit of large %f values, correct %g significant-digit handling and %e normalisation.
static size_t _host_ftoa(out_fct_type out, std::vector<char>* buffer, size_t idx, size_t maxlen,
                         char conversion, double value, unsigned int prec, unsigned int width,
                         unsigned int flags) {
	char   spec[16];
	size_t n  = 0;
	spec[n++] = '%';
	if ((flags & FLAGS_LEFT) != 0u) {
		spec[n++] = '-';
	}
	if ((flags & FLAGS_PLUS) != 0u) {
		spec[n++] = '+';
	}
	if ((flags & FLAGS_SPACE) != 0u) {
		spec[n++] = ' ';
	}
	if ((flags & FLAGS_HASH) != 0u) {
		spec[n++] = '#';
	}
	if ((flags & FLAGS_ZEROPAD) != 0u) {
		spec[n++] = '0';
	}
	spec[n++] = '*';
	spec[n++] = '.';
	spec[n++] = '*';
	spec[n++] = conversion;
	spec[n]   = 0;

	// printf treats a negative precision argument as an omitted precision.
	const int host_width = static_cast<int>(std::min<unsigned int>(width, INT_MAX));
	const int host_prec  = (flags & FLAGS_PRECISION) != 0u
	                           ? static_cast<int>(std::min<unsigned int>(prec, INT_MAX))
	                           : -1;

	const int len = std::snprintf(nullptr, 0, spec, host_width, host_prec, value);
	if (len <= 0) {
		return idx;
	}
	std::vector<char> text(static_cast<size_t>(len) + 1);
	std::snprintf(text.data(), text.size(), spec, host_width, host_prec, value);
	for (int i = 0; i < len; i++) {
		out(text[static_cast<size_t>(i)], buffer, idx++, maxlen);
	}
	return idx;
}

template <typename Char>
static size_t _strnlen_s(const Char* str, size_t maxsize) {
	size_t size = 0;
	while (size < maxsize && str[size] != 0) {
		++size;
	}
	return size;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static int kyty_printf_internal(bool sn, char* sn_s, size_t sn_n, const char* format,
                                VaList* va_list) {
	std::vector<char> buffer;

	uint32_t flags     = 0;
	uint32_t width     = 0;
	uint32_t precision = 0;
	uint32_t n         = 0;
	size_t   idx       = 0U;
	auto     maxlen    = static_cast<size_t>(-1);

	// use null output function
	auto out = _out_null;

	while (*format != 0) {
		// format specifier?  %[flags][width][.precision][length]
		if (*format != '%') {
			// no
			out(*format, &buffer, idx++, maxlen);
			format++;
			continue;
		}

		// yes, evaluate it
		format++;

		// evaluate flags
		flags = 0U;
		do {
			switch (*format) {
				case '0':
					flags |= FLAGS_ZEROPAD;
					format++;
					n = 1U;
					break;
				case '-':
					flags |= FLAGS_LEFT;
					format++;
					n = 1U;
					break;
				case '+':
					flags |= FLAGS_PLUS;
					format++;
					n = 1U;
					break;
				case ' ':
					flags |= FLAGS_SPACE;
					format++;
					n = 1U;
					break;
				case '#':
					flags |= FLAGS_HASH;
					format++;
					n = 1U;
					break;
				default: n = 0U; break;
			}
		} while (n != 0u);

		// evaluate width field
		width = 0U;
		if (_is_digit(*format)) {
			width = _atoi(&format);
		} else if (*format == '*') {
			// const int w = va_arg(va, int);
			const int w = VaArg_int(va_list);
			if (w < 0) {
				flags |= FLAGS_LEFT; // reverse padding
				width = static_cast<unsigned int>(-w);
			} else {
				width = static_cast<unsigned int>(w);
			}
			format++;
		}

		// evaluate precision field
		precision = 0U;
		if (*format == '.') {
			flags |= FLAGS_PRECISION;
			format++;
			if (_is_digit(*format)) {
				precision = _atoi(&format);
			} else if (*format == '*') {
				// const int prec = (int)va_arg(va, int);
				const int prec = VaArg_int(va_list);
				if (prec < 0) {
					// A negative dynamic precision is treated as omitted.
					flags &= ~FLAGS_PRECISION;
				} else {
					precision = static_cast<unsigned int>(prec);
				}
				format++;
			}
		}

		// evaluate length field
		switch (*format) {
			case 'l':
				flags |= FLAGS_LONG;
				format++;
				if (*format == 'l') {
					flags |= FLAGS_LONG_LONG;
					format++;
				}
				break;
			case 'h':
				flags |= FLAGS_SHORT;
				format++;
				if (*format == 'h') {
					flags |= FLAGS_CHAR;
					format++;
				}
				break;
			case 't':
				flags |= (sizeof(ptrdiff_t) == sizeof(int32_t) ? FLAGS_LONG : FLAGS_LONG_LONG);
				format++;
				break;
			case 'j':
				flags |= (sizeof(intmax_t) == sizeof(int32_t) ? FLAGS_LONG : FLAGS_LONG_LONG);
				format++;
				break;
			case 'z':
				flags |= (sizeof(size_t) == sizeof(int32_t) ? FLAGS_LONG : FLAGS_LONG_LONG);
				format++;
				break;
			default: break;
		}

		// evaluate specifier
		switch (*format) {
			case 'd':
			case 'i':
			case 'u':
			case 'x':
			case 'X':
			case 'o':
			case 'b': {
				// set the base
				unsigned int base = 0;
				if (*format == 'x' || *format == 'X') {
					base = 16U;
				} else if (*format == 'o') {
					base = 8U;
				} else if (*format == 'b') {
					base = 2U;
				} else {
					base = 10U;
					flags &= ~FLAGS_HASH; // no hash for dec format
				}
				// uppercase
				if (*format == 'X') {
					flags |= FLAGS_UPPERCASE;
				}

				// no plus or space flag for u, x, X, o, b
				if ((*format != 'i') && (*format != 'd')) {
					flags &= ~(FLAGS_PLUS | FLAGS_SPACE);
				}

				// ignore '0' flag when precision is given
				if ((flags & FLAGS_PRECISION) != 0u) {
					flags &= ~FLAGS_ZEROPAD;
				}

				// convert the integer
				if ((*format == 'i') || (*format == 'd')) {
					// signed
					if ((flags & FLAGS_LONG_LONG) != 0u || (flags & FLAGS_LONG) != 0u) {
						// const long long value = va_arg(va, long long);
						auto value = VaArg_long_long(va_list);
						idx = _ntoa_long_long(out, &buffer, idx, maxlen,
						                      static_cast<uint64_t>(value > 0 ? value : 0 - value),
						                      value < 0, base, precision, width, flags);
					} else if ((flags & FLAGS_LONG) != 0u) {
						// const long value = va_arg(va, long);
						auto value = VaArg_long(va_list);
						idx = _ntoa_long(out, &buffer, idx, maxlen,
						                 static_cast<uint32_t>(value > 0 ? value : 0 - value),
						                 value < 0, base, precision, width, flags);
					} else {
						// const int value = (flags & FLAGS_CHAR)    ? (char)va_arg(va, int)
						//                 : (flags & FLAGS_SHORT) ? (short int)va_arg(va, int)
						//                                       : va_arg(va, int);
						int value =
						    (flags & FLAGS_CHAR) != 0u    ? static_cast<char>(VaArg_int(va_list))
						    : (flags & FLAGS_SHORT) != 0u ? static_cast<int16_t>(VaArg_int(va_list))
						                                  : VaArg_int(va_list);
						idx = _ntoa_long(out, &buffer, idx, maxlen,
						                 static_cast<unsigned int>(value > 0 ? value : 0 - value),
						                 value < 0, base, precision, width, flags);
					}
				} else {
					// unsigned
					if ((flags & FLAGS_LONG_LONG) != 0u || (flags & FLAGS_LONG) != 0u) {
						idx = _ntoa_long_long(out, &buffer, idx, maxlen,
						                      static_cast<uint64_t>(VaArg_long_long(va_list)),
						                      false, base, precision, width, flags);
					} else if ((flags & FLAGS_LONG) != 0u) {
						idx = _ntoa_long(out, &buffer, idx, maxlen,
						                 static_cast<uint32_t>(VaArg_long(va_list)), false, base,
						                 precision, width, flags);
					} else {
						const unsigned int value =
						    (flags & FLAGS_CHAR) != 0u
						        ? static_cast<unsigned char>(VaArg_int(va_list))
						    : (flags & FLAGS_SHORT) != 0u
						        ? static_cast<uint16_t>(VaArg_int(va_list))
						        : static_cast<unsigned int>(VaArg_int(va_list));
						idx = _ntoa_long(out, &buffer, idx, maxlen, value, false, base, precision,
						                 width, flags);
					}
				}
				format++;
				break;
			}
			case 'f':
			case 'F':
			case 'e':
			case 'E':
			case 'g':
			case 'G':
				idx = _host_ftoa(out, &buffer, idx, maxlen, *format, VaArg_double(va_list),
				                 precision, width, flags);
				format++;
				break;
			case 'c': {
				unsigned int l = 1U;
				// pre padding
				if ((flags & FLAGS_LEFT) == 0u) {
					while (l++ < width) {
						out(' ', &buffer, idx++, maxlen);
					}
				}
				// char output
				out(static_cast<char>(VaArg_int(va_list)), &buffer, idx++, maxlen);
				// post padding
				if ((flags & FLAGS_LEFT) != 0u) {
					while (l++ < width) {
						out(' ', &buffer, idx++, maxlen);
					}
				}
				format++;
				break;
			}

			case 's': {
				const size_t limit = (flags & FLAGS_PRECISION) != 0u ? precision : maxlen;
				const char*  p     = VaArg_ptr<const char>(va_list);
				if (p == nullptr) {
					// FreeBSD prints "(null)" for a null string argument.
					p = "(null)";
					flags &= ~FLAGS_LONG;
				}
				std::string converted;
				if ((flags & FLAGS_LONG) != 0u) {
					// The guest ABI uses a 16-bit code unit for wchar_t.
					const auto*         wide = reinterpret_cast<const char16_t*>(p);
					std::u16string_view text(wide, _strnlen_s(wide, limit));
					if (text.size() == limit && !text.empty() && text.back() >= 0xd800 &&
					    text.back() <= 0xdbff) {
						text.remove_suffix(1);
					}
					converted = Common::Utf16ToUtf8(text);
					p         = converted.c_str();
				}
				size_t length = _strnlen_s(p, limit);
				if ((flags & FLAGS_LONG) != 0u && length < converted.size()) {
					// A wide-string precision cannot split a multibyte character.
					while (length != 0 && (static_cast<uint8_t>(p[length]) & 0xc0) == 0x80) {
						--length;
					}
				}
				if ((flags & FLAGS_LEFT) == 0u) {
					for (size_t i = length; i < width; ++i) {
						out(' ', &buffer, idx++, maxlen);
					}
				}
				for (size_t i = 0; i < length; ++i) {
					out(p[i], &buffer, idx++, maxlen);
				}
				if ((flags & FLAGS_LEFT) != 0u) {
					for (size_t i = length; i < width; ++i) {
						out(' ', &buffer, idx++, maxlen);
					}
				}
				format++;
				break;
			}

			case 'p': {
				width = sizeof(void*) * 2U;
				flags |= FLAGS_ZEROPAD | FLAGS_UPPERCASE;
				const bool is_ll = sizeof(uintptr_t) == sizeof(int64_t);
				if (is_ll) {
					idx = _ntoa_long_long(out, &buffer, idx, maxlen,
					                      reinterpret_cast<uintptr_t>(VaArg_ptr<void>(va_list)),
					                      false, 16U, precision, width, flags);
				} else {
					idx = _ntoa_long(out, &buffer, idx, maxlen,
					                 static_cast<uint32_t>(
					                     reinterpret_cast<uintptr_t>(VaArg_ptr<void>(va_list))),
					                 false, 16U, precision, width, flags);
				}
				format++;
				break;
			}

			case '%':
				out('%', &buffer, idx++, maxlen);
				format++;
				break;

			default:
				out(*format, &buffer, idx++, maxlen);
				format++;
				break;
		}
	}

	// termination
	out(static_cast<char>(0), &buffer, idx < maxlen ? idx : maxlen - 1U, maxlen);

	if (sn) {
		// C semantics: copy what fits, always terminate, and report the untruncated length.
		// memcpy (not %s) keeps embedded NULs produced by %c with a zero argument.
		if (sn_n != 0 && sn_s != nullptr) {
			const size_t count = std::min(idx, sn_n - 1);
			std::memcpy(sn_s, buffer.data(), count);
			sn_s[count] = 0;
		}
	} else {
		LOGF_COLOR(Log::Color::BrightMagenta, "%s", buffer.data());
	}

	// return written chars without terminating \0
	return static_cast<int>(idx);
}

static int kyty_vprintf(const char* format, VaList* va_list) {
	return kyty_printf_internal(false, nullptr, 0, format, va_list);
}

static int kyty_printf_ctx(VaContext* ctx) {
	const char* format = VaArg_ptr<const char>(&ctx->va_list);

	return kyty_printf_internal(false, nullptr, 0, format, &ctx->va_list);
}

static int kyty_snprintf_ctx(VaContext* ctx) {
	char*       s      = VaArg_ptr<char>(&ctx->va_list);
	size_t      n      = VaArg_size_t(&ctx->va_list);
	const char* format = VaArg_ptr<const char>(&ctx->va_list);

	return kyty_printf_internal(true, s, n, format, &ctx->va_list);
}

static int KYTY_SYSV_ABI kyty_printf_std(VA_ARGS) {
	VA_CONTEXT(ctx); // NOLINT(cppcoreguidelines-pro-type-member-init,hicpp-member-init)

	return kyty_printf_ctx(&ctx);
}

guest_printf_std_func_t GetGuestPrintfStdFunc() {
	return reinterpret_cast<guest_printf_std_func_t>(kyty_printf_std);
}

guest_printf_ctx_func_t GetGuestPrintfCtxFunc() {
	return kyty_printf_ctx;
}

guest_snprintf_ctx_func_t GetGuestSnprintfCtxFunc() {
	return kyty_snprintf_ctx;
}

guest_vprintf_func_t GetGuestVprintfFunc() {
	return kyty_vprintf;
}

} // namespace Libs
