#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_TROPHY_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_TROPHY_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// A trophy the player just unlocked, published once per unlock. It carries what the game gave and
/// no text of the UI's own, so whoever shows it decides what it says.
struct TrophyEvent {
	// The trophy's name, as the game gives it.
	std::string name;
	// The game's grade, 1 platinum, 2 gold, 3 silver, 4 bronze. Anything else is unknown.
	int32_t grade = 0;
	// The trophy's icon, still encoded, or null when the game gave none. Shared, so each
	// subscriber's copy of the event doesn't copy the image.
	std::shared_ptr<const std::vector<std::byte>> icon_png;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_TROPHY_H_
