#ifndef KYTY_COMMON_MAGICENUM_H_
#define KYTY_COMMON_MAGICENUM_H_

#include "common/common.h"
#include "common/stringUtils.h"
#include "magic_enum.hpp" // IWYU pragma: export

namespace Common {

template <typename E>
inline std::string EnumName(E v) {
	auto str = magic_enum::enum_name(v);
	return std::string(str.data(), str.length());
}

template <typename E>
inline std::string EnumName8(E v) {
	auto str = magic_enum::enum_name(v);
	return std::string(str.data(), str.length());
}

template <typename E>
inline E EnumValue(const std::string& str, E default_value) {
	auto v = magic_enum::enum_cast<E>(str.c_str());
	if (v.has_value()) {
		return v.value();
	}
	return default_value;
}

#define KYTY_ENUM_RANGE(e, mx, mn)                                                                 \
	namespace magic_enum::customize {                                                              \
	template <>                                                                                    \
	struct enum_range<e> {                                                                         \
		static constexpr int min = (mx);                                                           \
		static constexpr int max = (mn);                                                           \
	};                                                                                             \
	}

} // namespace Common

#endif /* KYTY_COMMON_MAGICENUM_H_ */
