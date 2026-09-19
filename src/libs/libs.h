#ifndef EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_
#define EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_

#include "common/abi.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "loader/timer.h" // IWYU pragma: keep

#include <atomic>
#include <cstddef>

namespace Libs {

// Key for the tracing flag below. A library is usually split over two translation units, one
// holding its LIB_DEFINE and one holding the traced functions, so the flag cannot live in
// either of them. Keying a variable template on the library name gives both the same object.
template <std::size_t N>
struct PrintNameKey {
	char value[N] {};
	consteval PrintNameKey(const char (&text)[N]) { // NOLINT(google-explicit-constructor)
		for (std::size_t i = 0; i < N; i++) {
			value[i] = text[i];
		}
	}
};

template <std::size_t N>
PrintNameKey(const char (&)[N]) -> PrintNameKey<N>;

// Shared by every translation unit naming the same library.
template <PrintNameKey Library>
inline std::atomic<bool> g_print_name_shared {false};

} // namespace Libs

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLED g_print_name

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLE(flag) PRINT_NAME_ENABLED = flag;

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_SHARED ::Libs::g_print_name_shared<g_print_name_key>

// Enable tracing for the whole library. LIB_DEFINE runs on the initialising thread and often
// sits in a different translation unit from the traced functions, so a per-thread per-file
// flag never reaches them. This sets the value every thread and unit starts from.
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME_ENABLE_ALL(flag)                                                                \
	do {                                                                                           \
		PRINT_NAME_SHARED.store((flag), std::memory_order_relaxed);                                \
		PRINT_NAME_ENABLE(flag)                                                                    \
	} while (false)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_DEFINE(name) void name(Loader::SymbolDatabase* s)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_NAME(l, m)                                                                             \
	static constexpr auto                     g_print_name_key = ::Libs::PrintNameKey {l};         \
	[[maybe_unused]] static thread_local bool PRINT_NAME_ENABLED =                                 \
	    PRINT_NAME_SHARED.load(std::memory_order_relaxed);                                         \
	static constexpr char g_library[] = l;                                                         \
	static constexpr char g_module[]  = m;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_VERSION(l, lv, m, mv1, mv2)                                                            \
	LIB_NAME(l, m);                                                                                \
	static constexpr int g_library_version      = lv;                                              \
	static constexpr int g_module_version_major = mv1;                                             \
	static constexpr int g_module_version_minor = mv2;
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_ADD(n, f, t)                                                                           \
	{                                                                                              \
		Loader::SymbolResolve sr {};                                                               \
		sr.name                 = n;                                                               \
		sr.library              = g_library;                                                       \
		sr.library_version      = g_library_version;                                               \
		sr.module               = g_module;                                                        \
		sr.module_version_major = g_module_version_major;                                          \
		sr.module_version_minor = g_module_version_minor;                                          \
		sr.type                 = t;                                                               \
		auto        func        = reinterpret_cast<uint64_t>(f);                                   \
		const char* dbg_name    = "" #f;                                                           \
		s->Add(sr, func, dbg_name);                                                                \
	}
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_OBJECT(n, f) LIB_ADD(n, f, Loader::SymbolType::Object)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define LIB_FUNC(n, f) LIB_ADD(n, f, Loader::SymbolType::Func)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define PRINT_NAME()                                                                               \
	if (PRINT_NAME_ENABLED) {                                                                      \
		if (Log::GetDirection() != Log::Direction::Silent) {                                       \
			const auto print_name_time = Loader::Timer::GetTime().ToString("HH24:MI:SS.FFF");      \
			LOGF_COLOR(Log::Color::Cyan, "[%d][%s] %s::%s::%s()\n",                                \
			           Common::Thread::GetThreadIdUnique(), print_name_time.c_str(), g_library,    \
			           g_module, __func__);                                                        \
		}                                                                                          \
	}

namespace Loader {
class SymbolDatabase;
} // namespace Loader

namespace Libs {

void InitAll(Loader::SymbolDatabase* s);

} // namespace Libs
#endif /* EMULATOR_INCLUDE_EMULATOR_LIBS_LIBS_H_ */
