#include "graphics/presentation/system/service/toast/queue.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

namespace Libs::Graphics::SystemUi {

namespace {

// Holds past this are taken as infinite, as adding them to a time point would overflow it.
constexpr float LONGEST_FINITE_HOLD_MS = 24.0f * 60.0f * 60.0f * 1000.0f;

// When a toast held for `hold_ms` from `now` starts leaving. An infinite hold, or one longer than
// a day, never does, and a hold that isn't a number, or is below zero, leaves at once rather than
// staying for ever.
TimePoint HideAt(float hold_ms, TimePoint now) {
	if (std::isnan(hold_ms) || hold_ms <= 0.0f) {
		return now;
	}
	if (hold_ms > LONGEST_FINITE_HOLD_MS) {
		return TimePoint::max();
	}
	return now + std::chrono::duration_cast<ClockContext::Clock::duration>(
	                 std::chrono::duration<float, std::milli>(hold_ms));
}

// The entry in `entries` with `key`, or null. An empty key matches nothing.
template <class Entries>
ToastEntry* FindKey(Entries& entries, const std::string& key) {
	if (key.empty()) {
		return nullptr;
	}
	const auto found =
	    std::find_if(entries.begin(), entries.end(),
	                 [&key](const ToastEntry& entry) { return entry.toast.options.key == key; });
	return found != entries.end() ? &*found : nullptr;
}

} // namespace

ToastQueue::ToastQueue(size_t visible_limit): m_visible_limit(std::max<size_t>(visible_limit, 1)) {}

uint64_t ToastQueue::Add(ToastRequest toast, TimePoint now) {
	return Add(std::move(toast), now, ++m_last_id);
}

uint64_t ToastQueue::Add(ToastRequest toast, TimePoint now, uint64_t id) {
	if (ToastEntry* shown = FindKey(m_on_screen, toast.options.key)) {
		shown->toast   = std::move(toast);
		shown->shown   = true;
		shown->hide_at = HideAt(shown->toast.options.hold_ms, now);
		return shown->id;
	}
	if (ToastEntry* waiting = FindKey(m_waiting, toast.options.key)) {
		waiting->toast = std::move(toast);
		return waiting->id;
	}
	ToastEntry entry {.id = id, .toast = std::move(toast)};
	if (m_on_screen.size() < m_visible_limit) {
		Show(std::move(entry), now);
	} else {
		if (m_waiting.size() >= MAX_WAITING_TOASTS) {
			m_waiting.pop_front();
		}
		m_waiting.push_back(std::move(entry));
	}
	return id;
}

bool ToastQueue::Contains(uint64_t id) const {
	const auto matches = [id](const ToastEntry& entry) { return entry.id == id; };
	return std::any_of(m_on_screen.begin(), m_on_screen.end(), matches) ||
	       std::any_of(m_waiting.begin(), m_waiting.end(), matches);
}

bool ToastQueue::Dismiss(uint64_t id) {
	const auto shown = std::find_if(m_on_screen.begin(), m_on_screen.end(),
	                                [id](const ToastEntry& entry) { return entry.id == id; });
	if (shown != m_on_screen.end()) {
		shown->shown = false;
		return true;
	}
	return std::erase_if(m_waiting, [id](const ToastEntry& entry) { return entry.id == id; }) > 0;
}

void ToastQueue::Update(TimePoint now) {
	for (ToastEntry& entry: m_on_screen) {
		if (entry.shown && now >= entry.hide_at) {
			entry.shown = false;
		}
	}
}

void ToastQueue::Remove(uint64_t id, TimePoint now) {
	std::erase_if(m_on_screen, [id](const ToastEntry& entry) { return entry.id == id; });
	while (m_on_screen.size() < m_visible_limit && !m_waiting.empty()) {
		ToastEntry next = std::move(m_waiting.front());
		m_waiting.pop_front();
		Show(std::move(next), now);
	}
}

void ToastQueue::Show(ToastEntry entry, TimePoint now) {
	entry.shown   = true;
	entry.hide_at = HideAt(entry.toast.options.hold_ms, now);
	m_newly_shown.push_back(entry.id);
	m_on_screen.push_back(std::move(entry));
}

void ToastQueue::TakeNewlyShown(std::vector<uint64_t>& out) {
	out.insert(out.end(), m_newly_shown.begin(), m_newly_shown.end());
	m_newly_shown.clear();
}

} // namespace Libs::Graphics::SystemUi
