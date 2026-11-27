#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_SUBSCRIBE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_SUBSCRIBE_H_

#include "graphics/presentation/system/events/channel.h"

#include <cstddef>

// The subscribing half of the event bus, kept apart so publishers outside the system UI don't
// include it.
namespace Libs::Graphics::SystemUi {

/// A new subscription to events of type `T`, holding at most `capacity` waiting events. Safe from
/// any thread.
template <class T>
[[nodiscard]] EventSubscription<T> Subscribe(size_t capacity) {
	return Channel<T>().Subscribe(capacity);
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_SUBSCRIBE_H_
