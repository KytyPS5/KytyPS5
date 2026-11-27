#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_PUBLISH_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_PUBLISH_H_

#include "graphics/presentation/system/events/channel.h"

// The publishing half of the event bus, all a publisher needs, kept apart so publishers don't
// include the free Subscribe.
namespace Libs::Graphics::SystemUi {

/// Delivers `event` to everyone subscribed to its type. Safe from any thread.
template <class T>
void Publish(const T& event) {
	Channel<T>().Publish(event);
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_PUBLISH_H_
