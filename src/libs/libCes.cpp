#include "common/abi.h"
#include "common/assert.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <SDL3/SDL_stdinc.h>
#include <array>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Libs {

namespace LibCes {

LIB_NAME("Ces", "Ces");

// Single-byte profiles are opaque to the game; their address selects the code page.
// Distinct values keep identical-constant folding from merging the two addresses.
static const uint8_t CES_SBC_PROFILE_CP1252     = 0;
static const uint8_t CES_SBC_PROFILE_ISO8859_15 = 1;

static const uint8_t* KYTY_SYSV_ABI CesRefersUcsProfileCp1252() {
	PRINT_NAME();
	return &CES_SBC_PROFILE_CP1252;
}

static const uint8_t* KYTY_SYSV_ABI CesRefersUcsProfileIso8859_15() {
	PRINT_NAME();
	return &CES_SBC_PROFILE_ISO8859_15;
}

static uint32_t CesDecodeUtf8(const uint8_t* utf8, uint32_t utf8max, uint32_t* utf8_len) {
	if (utf8 == nullptr || utf8max == 0) {
		if (utf8_len != nullptr) {
			*utf8_len = 0;
		}
		return 0xfffd;
	}

	const uint8_t c0 = utf8[0];
	if (c0 < 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 1;
		}
		return c0;
	}
	if ((c0 & 0xe0) == 0xc0 && utf8max >= 2 && (utf8[1] & 0xc0) == 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 2;
		}
		return ((c0 & 0x1f) << 6u) | (utf8[1] & 0x3fu);
	}
	if ((c0 & 0xf0) == 0xe0 && utf8max >= 3 && (utf8[1] & 0xc0) == 0x80 &&
	    (utf8[2] & 0xc0) == 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 3;
		}
		return ((c0 & 0x0f) << 12u) | ((utf8[1] & 0x3fu) << 6u) | (utf8[2] & 0x3fu);
	}
	if ((c0 & 0xf8) == 0xf0 && utf8max >= 4 && (utf8[1] & 0xc0) == 0x80 &&
	    (utf8[2] & 0xc0) == 0x80 && (utf8[3] & 0xc0) == 0x80) {
		if (utf8_len != nullptr) {
			*utf8_len = 4;
		}
		return ((c0 & 0x07) << 18u) | ((utf8[1] & 0x3fu) << 12u) | ((utf8[2] & 0x3fu) << 6u) |
		       (utf8[3] & 0x3fu);
	}

	if (utf8_len != nullptr) {
		*utf8_len = 1;
	}
	return 0xfffd;
}

struct Cp1252Mapping {
	uint8_t  cp1252;
	uint32_t unicode;
};

static constexpr Cp1252Mapping CP1252_EXTENDED_MAP[] = {
    {0x80, 0x20ac}, {0x82, 0x201a}, {0x83, 0x0192}, {0x84, 0x201e}, {0x85, 0x2026}, {0x86, 0x2020},
    {0x87, 0x2021}, {0x88, 0x02c6}, {0x89, 0x2030}, {0x8a, 0x0160}, {0x8b, 0x2039}, {0x8c, 0x0152},
    {0x8e, 0x017d}, {0x91, 0x2018}, {0x92, 0x2019}, {0x93, 0x201c}, {0x94, 0x201d}, {0x95, 0x2022},
    {0x96, 0x2013}, {0x97, 0x2014}, {0x98, 0x02dc}, {0x99, 0x2122}, {0x9a, 0x0161}, {0x9b, 0x203a},
    {0x9c, 0x0153}, {0x9e, 0x017e}, {0x9f, 0x0178},
};

static uint8_t CesUnicodeToCp1252(uint32_t code) {
	if (code <= 0x7f || (code >= 0xa0 && code <= 0xff)) {
		return static_cast<uint8_t>(code);
	}

	for (const auto& map: CP1252_EXTENDED_MAP) {
		if (map.unicode == code) {
			return map.cp1252;
		}
	}
	return '?';
}

static uint32_t CesCp1252ToUnicode(uint8_t sbc) {
	if (sbc <= 0x7f || sbc >= 0xa0) {
		return sbc;
	}

	for (const auto& map: CP1252_EXTENDED_MAP) {
		if (map.cp1252 == sbc) {
			return map.unicode;
		}
	}
	return sbc;
}

struct Iso8859_15Mapping {
	uint8_t  iso8859_15;
	uint32_t unicode;
};

// ISO-8859-15 matches ISO-8859-1 except for these eight positions.
static constexpr Iso8859_15Mapping ISO8859_15_REPLACED_MAP[] = {
    {0xa4, 0x20ac}, {0xa6, 0x0160}, {0xa8, 0x0161}, {0xb4, 0x017d},
    {0xb8, 0x017e}, {0xbc, 0x0152}, {0xbd, 0x0153}, {0xbe, 0x0178},
};

static uint8_t CesUnicodeToIso8859_15(uint32_t code) {
	for (const auto& map: ISO8859_15_REPLACED_MAP) {
		if (map.unicode == code) {
			return map.iso8859_15;
		}
		if (map.iso8859_15 == code) {
			// The Latin-1 character that used to live here is not representable.
			return '?';
		}
	}
	return code <= 0xff ? static_cast<uint8_t>(code) : '?';
}

static uint32_t CesIso8859_15ToUnicode(uint8_t sbc) {
	for (const auto& map: ISO8859_15_REPLACED_MAP) {
		if (map.iso8859_15 == sbc) {
			return map.unicode;
		}
	}
	return sbc;
}

static uint8_t CesUnicodeToSbc(const uint8_t* profile, uint32_t code) {
	return profile == &CES_SBC_PROFILE_ISO8859_15 ? CesUnicodeToIso8859_15(code)
	                                              : CesUnicodeToCp1252(code);
}

static uint32_t CesSbcToUnicode(const uint8_t* profile, uint8_t sbc) {
	return profile == &CES_SBC_PROFILE_ISO8859_15 ? CesIso8859_15ToUnicode(sbc)
	                                              : CesCp1252ToUnicode(sbc);
}

static uint32_t CesEncodeUtf8(uint32_t code, uint8_t* utf8, uint32_t utf8max) {
	uint8_t  tmp[4] {};
	uint32_t len = 0;

	if (code <= 0x7f) {
		tmp[0] = static_cast<uint8_t>(code);
		len    = 1;
	} else if (code <= 0x7ff) {
		tmp[0] = static_cast<uint8_t>(0xc0u | ((code >> 6u) & 0x1fu));
		tmp[1] = static_cast<uint8_t>(0x80u | (code & 0x3fu));
		len    = 2;
	} else if (code <= 0xffff) {
		tmp[0] = static_cast<uint8_t>(0xe0u | ((code >> 12u) & 0x0fu));
		tmp[1] = static_cast<uint8_t>(0x80u | ((code >> 6u) & 0x3fu));
		tmp[2] = static_cast<uint8_t>(0x80u | (code & 0x3fu));
		len    = 3;
	} else {
		tmp[0] = static_cast<uint8_t>(0xf0u | ((code >> 18u) & 0x07u));
		tmp[1] = static_cast<uint8_t>(0x80u | ((code >> 12u) & 0x3fu));
		tmp[2] = static_cast<uint8_t>(0x80u | ((code >> 6u) & 0x3fu));
		tmp[3] = static_cast<uint8_t>(0x80u | (code & 0x3fu));
		len    = 4;
	}

	if (utf8 != nullptr && utf8max >= len) {
		std::memcpy(utf8, tmp, len);
	}

	return len;
}

static int KYTY_SYSV_ABI CesUtf8ToSbc(const uint8_t* utf8, uint32_t utf8max, uint32_t* utf8_len,
                                      const uint8_t* profile, uint8_t* sbc) {
	PRINT_NAME();

	if (sbc == nullptr || profile == nullptr) {
		return -1;
	}

	uint32_t   local_len = 0;
	const auto code      = CesDecodeUtf8(utf8, utf8max, &local_len);
	if (utf8_len != nullptr) {
		*utf8_len = local_len;
	}
	*sbc = CesUnicodeToSbc(profile, code);

	return 0;
}

static int KYTY_SYSV_ABI CesSbcToUtf8(const uint8_t* profile, uint8_t sbc, uint8_t* utf8,
                                      uint32_t utf8max, uint32_t* utf8_len) {
	PRINT_NAME();

	if (profile == nullptr || utf8 == nullptr) {
		return -1;
	}

	const auto code = CesSbcToUnicode(profile, sbc);
	const auto len  = CesEncodeUtf8(code, utf8, utf8max);
	if (utf8_len != nullptr) {
		*utf8_len = len;
	}

	return utf8max >= len ? 0 : -1;
}

// Profile sheets and contexts each reserve 32 pointers for library-private state.
struct CesProfile {
	const char* encoding;
	const void* reserved[31];
};

struct CesContext {
	const CesProfile* profile;
	const void*       reserved[31];
};

static_assert(sizeof(CesProfile) == 256 && alignof(CesProfile) == 8);
static_assert(sizeof(CesContext) == 256 && alignof(CesContext) == 8);

static constexpr char CP932[]                      = "CP932";
static constexpr int  CES_ERROR_INVALID_PARAMETER  = static_cast<int>(0x805c0001u);
static constexpr int  CES_ERROR_INVALID_PROFILE    = static_cast<int>(0x805c0004u);
static constexpr int  CES_ERROR_INVALID_SRC_BUFFER = static_cast<int>(0x805c0010u);
static constexpr int  CES_ERROR_SRC_BUFFER_END     = static_cast<int>(0x805c0011u);
static constexpr int  CES_ERROR_INVALID_ENCODE     = static_cast<int>(0x805c0014u);
static constexpr int  CES_ERROR_UNASSIGNED_CODE    = static_cast<int>(0x805c0020u);
static constexpr int  CES_ERROR_INVALID_DST_BUFFER = static_cast<int>(0x805c0030u);
static constexpr int  CES_ERROR_DST_BUFFER_END     = static_cast<int>(0x805c0031u);

static CesProfile* KYTY_SYSV_ABI CesUcsProfileInitSJis1997Cp932(CesProfile* sheet) {
	PRINT_NAME();
	if (sheet != nullptr) {
		*sheet          = {};
		sheet->encoding = CP932;
	}
	return sheet;
}

static int KYTY_SYSV_ABI CesMbcsUcsContextInit(CesContext* context, const CesProfile* profile) {
	PRINT_NAME();

	if (context == nullptr || profile == nullptr) {
		return CES_ERROR_INVALID_PARAMETER;
	}
	*context         = {};
	context->profile = profile;

	return 0;
}

#if defined(_WIN32)

static size_t ConvertCp932(const char** inbuf, size_t* inbytesleft, char** outbuf,
                           size_t* outbytesleft, bool utf16) {
	const auto* in = reinterpret_cast<const uint8_t*>(*inbuf);
	while (*inbytesleft != 0) {
		const uint8_t lead = in[0];
		// Win32 accepts 0x80 as U+0080; the SJis1997 profile rejects this byte.
		if (lead == 0x80) {
			*inbuf = reinterpret_cast<const char*>(in);
			return SDL_ICONV_EILSEQ;
		}
		const bool is_lead_byte = (lead >= 0x81 && lead <= 0x9f) || (lead >= 0xe0 && lead <= 0xfc);
		const size_t char_len   = is_lead_byte ? 2 : 1;
		if (*inbytesleft < char_len) {
			*inbuf = reinterpret_cast<const char*>(in);
			return SDL_ICONV_EINVAL;
		}

		wchar_t   wide[2];
		const int wide_len =
		    MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(in),
		                        static_cast<int>(char_len), wide, 2);
		if (wide_len <= 0) {
			*inbuf = reinterpret_cast<const char*>(in);
			return SDL_ICONV_EILSEQ;
		}

		char encoded[8];
		int  encoded_len = 0;
		if (utf16) {
			// wchar_t is UTF-16LE on Windows, which is the guest byte order as well.
			encoded_len = wide_len * static_cast<int>(sizeof(wchar_t));
			std::memcpy(encoded, wide, static_cast<size_t>(encoded_len));
		} else {
			encoded_len = WideCharToMultiByte(CP_UTF8, 0, wide, wide_len, encoded, sizeof(encoded),
			                                  nullptr, nullptr);
		}
		if (encoded_len <= 0 || static_cast<size_t>(encoded_len) > *outbytesleft) {
			*inbuf = reinterpret_cast<const char*>(in);
			return SDL_ICONV_E2BIG;
		}

		std::memcpy(*outbuf, encoded, static_cast<size_t>(encoded_len));
		*outbuf += encoded_len;
		*outbytesleft -= static_cast<size_t>(encoded_len);
		in += char_len;
		*inbytesleft -= char_len;
	}
	*inbuf = reinterpret_cast<const char*>(in);
	return 0;
}

#endif

enum class CesUnicodeForm { Utf8, Utf16 };

static void WriteTerminator(void* destination, size_t unit, CesUnicodeForm form) {
	if (form == CesUnicodeForm::Utf16) {
		static_cast<uint16_t*>(destination)[unit] = 0;
	} else {
		static_cast<uint8_t*>(destination)[unit] = 0;
	}
}

// destination_max and destination_len count code units of the requested form (bytes for
// UTF-8, 16-bit units for UTF-16); one unit is always reserved for the terminator.
static int ConvertMbcs(CesContext* context, const uint8_t* source, uint32_t source_max,
                       uint32_t* source_len, void* destination, uint32_t destination_max,
                       uint32_t* destination_len, bool measure, CesUnicodeForm form) {
	const size_t unit_size = form == CesUnicodeForm::Utf16 ? sizeof(uint16_t) : sizeof(uint8_t);

	if (source_len != nullptr) {
		*source_len = 0;
	}
	if (destination_len != nullptr) {
		*destination_len = 0;
	}
	if (destination != nullptr && destination_max != 0) {
		WriteTerminator(destination, 0, form);
	}
	if (context == nullptr) {
		return CES_ERROR_INVALID_PARAMETER;
	}
	if (context->profile == nullptr || context->profile->encoding != CP932) {
		return CES_ERROR_INVALID_PROFILE;
	}
	if (source == nullptr) {
		return CES_ERROR_INVALID_SRC_BUFFER;
	}
	if (!measure && destination == nullptr) {
		return CES_ERROR_INVALID_DST_BUFFER;
	}
	if (!measure && destination_max == 0) {
		return CES_ERROR_DST_BUFFER_END;
	}

	size_t source_size = 0;
	while ((source_max == 0 || source_size < source_max) && source[source_size] != 0) {
		++source_size;
	}
	const bool bounded_end = source_max != 0 && source_size == source_max;
#if !defined(_WIN32)
	const auto converter = SDL_iconv_open(form == CesUnicodeForm::Utf16 ? "UTF-16LE" : "UTF-8",
	                                      context->profile->encoding);
	EXIT_IF(converter == reinterpret_cast<SDL_iconv_t>(-1));
#endif

	const char*           input      = reinterpret_cast<const char*>(source);
	size_t                input_left = source_size;
	size_t                produced   = 0;
	int                   result     = 0;
	std::array<char, 256> scratch;
	do {
		char* output = measure ? scratch.data() : static_cast<char*>(destination) + produced;
		const size_t capacity =
		    measure ? scratch.size() : (destination_max - 1) * unit_size - produced;
		size_t output_left = capacity;
#if defined(_WIN32)
		const auto status = ConvertCp932(&input, &input_left, &output, &output_left,
		                                 form == CesUnicodeForm::Utf16);
#else
		const auto status = SDL_iconv(converter, &input, &input_left, &output, &output_left);
#endif
		produced += capacity - output_left;
		if (status == SDL_ICONV_E2BIG) {
			if (measure) {
				continue;
			}
			result = CES_ERROR_DST_BUFFER_END;
		} else if (status == SDL_ICONV_EINVAL) {
			result = bounded_end ? CES_ERROR_SRC_BUFFER_END : CES_ERROR_INVALID_ENCODE;
		} else if (status == SDL_ICONV_EILSEQ || status == SDL_ICONV_ERROR) {
			const auto* code = reinterpret_cast<const uint8_t*>(input);
			const bool  lead =
			    (code[0] >= 0x81 && code[0] <= 0x9f) || (code[0] >= 0xe0 && code[0] <= 0xfc);
			const bool trail = input_left >= 2 && ((code[1] >= 0x40 && code[1] <= 0x7e) ||
			                                       (code[1] >= 0x80 && code[1] <= 0xfc));
			result           = lead && trail ? CES_ERROR_UNASSIGNED_CODE : CES_ERROR_INVALID_ENCODE;
		}
		break;
	} while (input_left != 0);
#if !defined(_WIN32)
	SDL_iconv_close(converter);
#endif

	if (!measure) {
		WriteTerminator(destination, produced / unit_size, form);
	}
	if (source_len != nullptr) {
		*source_len = static_cast<uint32_t>(source_size - input_left);
	}
	if (destination_len != nullptr) {
		*destination_len = static_cast<uint32_t>(produced / unit_size);
	}
	return result;
}

static int KYTY_SYSV_ABI CesMbcsStrGetUtf8Len(CesContext* context, const uint8_t* source,
                                              uint32_t source_max, uint32_t* source_len,
                                              uint32_t* destination_len) {
	PRINT_NAME();
	return ConvertMbcs(context, source, source_max, source_len, nullptr, 0, destination_len, true,
	                   CesUnicodeForm::Utf8);
}

static int KYTY_SYSV_ABI CesMbcsStrToUtf8Str(CesContext* context, const uint8_t* source,
                                             uint32_t source_max, uint32_t* source_len,
                                             uint8_t* destination, uint32_t destination_max,
                                             uint32_t* destination_len) {
	PRINT_NAME();
	return ConvertMbcs(context, source, source_max, source_len, destination, destination_max,
	                   destination_len, false, CesUnicodeForm::Utf8);
}

static int KYTY_SYSV_ABI CesMbcsStrGetUtf16Len(CesContext* context, const uint8_t* source,
                                               uint32_t source_max, uint32_t* source_len,
                                               uint32_t* destination_len) {
	PRINT_NAME();
	return ConvertMbcs(context, source, source_max, source_len, nullptr, 0, destination_len, true,
	                   CesUnicodeForm::Utf16);
}

static int KYTY_SYSV_ABI CesMbcsStrToUtf16Str(CesContext* context, const uint8_t* source,
                                              uint32_t source_max, uint32_t* source_len,
                                              uint16_t* destination, uint32_t destination_max,
                                              uint32_t* destination_len) {
	PRINT_NAME();
	return ConvertMbcs(context, source, source_max, source_len, destination, destination_max,
	                   destination_len, false, CesUnicodeForm::Utf16);
}

LIB_DEFINE(InitCes_1) {
	LIB_FUNC("ZiDCxUUGbec", LibCes::CesUcsProfileInitSJis1997Cp932);
	LIB_FUNC("538bRGc6Zo8", LibCes::CesMbcsUcsContextInit);
	LIB_FUNC("r7Sr1i7KLus", LibCes::CesMbcsStrGetUtf8Len);
	LIB_FUNC("yGKn6vdYInc", LibCes::CesMbcsStrToUtf8Str);
	LIB_FUNC("KkcyBpHeV6Q", LibCes::CesMbcsStrGetUtf16Len);
	LIB_FUNC("RO-MRURI76g", LibCes::CesMbcsStrToUtf16Str);
	LIB_FUNC("LPzYZ+FR0BI", LibCes::CesRefersUcsProfileCp1252);
	LIB_FUNC("NLW0QcvJY-E", LibCes::CesRefersUcsProfileIso8859_15);
	LIB_FUNC("3Q1gOWWarcw", LibCes::CesUtf8ToSbc);
	LIB_FUNC("xTd54EEL1Ao", LibCes::CesSbcToUtf8);
}

} // namespace LibCes

} // namespace Libs
