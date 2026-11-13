#pragma once

#include "loader/systemContent.h"

#include <string>

namespace Loader::DemonsSoulsTitle {
// Demon's Souls (PPSA01340, PPSA01341, PPSA01342), from the loaded game's metadata.
inline bool IsSupportedGame() {
	static const bool supported = [] {
		std::string title;
		return SystemContentParamSfoGetString("TITLE_ID", &title) &&
		       (title == "PPSA01340" || title == "PPSA01341" || title == "PPSA01342");
	}();
	return supported;
}
} // namespace Loader::DemonsSoulsTitle
