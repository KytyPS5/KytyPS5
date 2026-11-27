#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_NOTIFICATION_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_NOTIFICATION_H_

#include "graphics/presentation/system/core/status.h"
#include "graphics/presentation/system/events/publish.h"

#include <string>
#include <utility>

// What the emulator hands the system UI to show as a notice. Only the standard library, the event
// channel and the status, so the emulator's libraries can publish without depending on the system
// UI.
namespace Libs::Graphics::SystemUi {

/// A notice for the player, already in their language, such as a controller connecting.
struct NotificationEvent {
	std::string text;
	// How it went, which sets the colour of its band.
	Status status = Status::Info;
};

/// Publishes a notice saying `text` with `status`, from any thread. It is dropped when nothing
/// subscribes, such as while the game boots or with system notices off.
inline void PublishNotification(std::string text, Status status = Status::Info) {
	Publish(NotificationEvent {.text = std::move(text), .status = status});
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_NOTIFICATION_H_
