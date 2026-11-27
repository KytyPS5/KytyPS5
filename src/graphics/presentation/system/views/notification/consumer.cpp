#include "graphics/presentation/system/views/notification/consumer.h"

#include "graphics/presentation/system/service.h"
#include "graphics/presentation/system/views/notification/notification.h"

#include <utility>

namespace Libs::Graphics::SystemUi {

NotificationEventConsumer::NotificationEventConsumer(ToastService& toasts, size_t capacity)
    : EventConsumer<NotificationEvent>(capacity), m_toasts(toasts) {}

void NotificationEventConsumer::OnEvent(NotificationEvent& event, TimePoint now) {
	// The props are held by the builder, so the notice keeps its text for every frame it is drawn.
	m_toasts.Add(
	    [props = NotificationProps {.status = event.status, .text = std::move(event.text)}] {
		    return Notification(props);
	    },
	    {}, now);
}

} // namespace Libs::Graphics::SystemUi
