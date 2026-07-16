#ifndef KYTY_COMMON_COMMON_H_
#define KYTY_COMMON_COMMON_H_

#include "common/config.h" // IWYU pragma: export

#if __cplusplus < 201703L
#undef __cplusplus
#define __cplusplus 201703L
#endif

#if defined(__MINGW32__) || defined(__MINGW64__)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage,cert-dcl51-cpp,cert-dcl37-c,bugprone-reserved-identifier)
#define __USE_MINGW_ANSI_STDIO 1
#endif

// IWYU pragma: begin_exports
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
// IWYU pragma: end_exports

#define KYTY_CLASS_NO_COPY(name)                                                                   \
public:                                                                                            \
	name(const name&)                = delete; /* NOLINT(bugprone-macro-parentheses) */            \
	name& operator=(const name&)     = delete; /* NOLINT(bugprone-macro-parentheses) */            \
	name(name&&) noexcept            = delete; /* NOLINT(bugprone-macro-parentheses) */            \
	name& operator=(name&&) noexcept = delete; /* NOLINT(bugprone-macro-parentheses) */

#define KYTY_CLASS_DEFAULT_COPY(name)                                                              \
public:                                                                                            \
	name(const name&)                = default; /* NOLINT(bugprone-macro-parentheses) */           \
	name& operator=(const name&)     = default; /* NOLINT(bugprone-macro-parentheses) */           \
	name(name&&) noexcept            = default; /* NOLINT(bugprone-macro-parentheses) */           \
	name& operator=(name&&) noexcept = default; /* NOLINT(bugprone-macro-parentheses) */

#if KYTY_COMPILER == KYTY_COMPILER_GCC
#define KYTY_FORMAT_PRINTF(a, b) __attribute__((format(gnu_printf, a, b)))
#elif KYTY_COMPILER == KYTY_COMPILER_CLANG
#define KYTY_FORMAT_PRINTF(a, b) __attribute__((format(printf, a, b)))
#endif

#endif /* KYTY_COMMON_COMMON_H_ */
