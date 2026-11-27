#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_CHANNEL_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_CHANNEL_H_

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

// Typed events from the emulator to whoever shows them. A publisher hands over a plain value on any
// thread, and each subscriber takes its copies later on its own thread, so nothing a subscriber
// does ever runs on the publisher's thread. Each event type has its own channel, found by its type,
// so adding a type touches nothing shared. Only the standard library is used, so the emulator's
// libraries can publish without depending on the system UI.
namespace Libs::Graphics::SystemUi {

template <class T>
class EventChannel;

/// One subscriber's copies of the events published to a channel, waiting to be taken. It holds at
/// most its capacity, dropping the oldest when a new event arrives at a full queue, so a subscriber
/// that stops taking can't grow without bound. It stops receiving when destroyed. It is moved,
/// never copied, so each event is taken once.
template <class T>
class EventSubscription {
public:
	EventSubscription()                                    = default;
	EventSubscription(EventSubscription&&) noexcept        = default;
	EventSubscription& operator=(EventSubscription&&)      = default;
	EventSubscription(const EventSubscription&)            = delete;
	EventSubscription& operator=(const EventSubscription&) = delete;
	~EventSubscription()                                   = default;

	/// Moves every waiting event onto the end of `out`, oldest first, and empties the queue. A
	/// subscriber reuses one vector each frame, so a steady frame doesn't allocate. Safe while
	/// events are published from other threads, though one subscription is drained from one thread
	/// at a time.
	void DrainInto(std::vector<T>& out) {
		if (!m_mailbox) {
			return;
		}
		std::scoped_lock lock(m_mailbox->mutex);
		for (T& event: m_mailbox->queue) {
			out.push_back(std::move(event));
		}
		m_mailbox->queue.clear();
		m_mailbox->pending.store(false, std::memory_order_release);
	}

	/// Whether events are waiting, cheap enough to ask every frame from any thread.
	[[nodiscard]] bool HasPending() const noexcept {
		return m_mailbox && m_mailbox->pending.load(std::memory_order_acquire);
	}

	/// How many events were dropped because the queue was full. Safe from any thread.
	[[nodiscard]] uint64_t Dropped() const noexcept {
		return m_mailbox ? m_mailbox->dropped.load(std::memory_order_relaxed) : 0;
	}

	/// Whether it is subscribed, which a default-made or moved-from subscription isn't.
	[[nodiscard]] bool Subscribed() const noexcept { return m_mailbox != nullptr; }

private:
	friend class EventChannel<T>;

	/// The queue a channel delivers into. The channel holds it weakly, so destroying the
	/// subscription is all it takes to stop receiving.
	struct Mailbox {
		/// An empty mailbox that holds at most `limit` events.
		explicit Mailbox(size_t limit): capacity(limit) {}

		// Guards the queue between publishers and the subscriber.
		std::mutex mutex;
		// The waiting events, oldest first.
		std::deque<T> queue;
		// The most events the queue holds before it drops the oldest.
		size_t capacity;
		// Whether the queue holds events, read without the lock.
		std::atomic<bool> pending {false};
		// How many events were dropped at a full queue.
		std::atomic<uint64_t> dropped {0};
	};

	/// A subscription delivered through `mailbox`. Only the channel makes one.
	explicit EventSubscription(std::shared_ptr<Mailbox> mailbox): m_mailbox(std::move(mailbox)) {}

	std::shared_ptr<Mailbox> m_mailbox;
};

/// Delivers each event of type `T` to every subscriber. Publishing copies the event into each
/// subscriber's queue and returns, calling nothing of the subscriber's. An event published while
/// nobody is subscribed is dropped, so a channel nobody listens to costs a lock and nothing more.
/// Safe to use from any number of threads.
template <class T>
class EventChannel {
public:
	/// The queue each subscriber is delivered through.
	using Mailbox = typename EventSubscription<T>::Mailbox;

	/// Delivers `event` to every current subscriber.
	void Publish(const T& event) {
		// The channel's lock, then each mailbox's, is the only order the two are taken in.
		std::scoped_lock lock(m_mutex);
		std::erase_if(m_mailboxes,
		              [](const std::weak_ptr<Mailbox>& weak) { return weak.expired(); });
		for (const std::weak_ptr<Mailbox>& weak: m_mailboxes) {
			if (const std::shared_ptr<Mailbox> mailbox = weak.lock()) {
				Deliver(*mailbox, event);
			}
		}
	}

	/// A new subscription holding at most `capacity` waiting events, at least one. It receives the
	/// events published from now on.
	[[nodiscard]] EventSubscription<T> Subscribe(size_t capacity) {
		auto             mailbox = std::make_shared<Mailbox>(std::max<size_t>(capacity, 1));
		std::scoped_lock lock(m_mutex);
		m_mailboxes.push_back(mailbox);
		return EventSubscription<T>(std::move(mailbox));
	}

private:
	/// Adds `event` to `mailbox`, dropping its oldest when it is full.
	static void Deliver(Mailbox& mailbox, const T& event) {
		std::scoped_lock lock(mailbox.mutex);
		if (mailbox.queue.size() >= mailbox.capacity) {
			mailbox.queue.pop_front();
			mailbox.dropped.fetch_add(1, std::memory_order_relaxed);
		}
		mailbox.queue.push_back(event);
		mailbox.pending.store(true, std::memory_order_release);
	}

	std::mutex                          m_mutex;
	std::vector<std::weak_ptr<Mailbox>> m_mailboxes;
};

/// The program's one channel for events of type `T`, made the first time it is asked for. Safe from
/// any thread, as it is made once even when several threads ask at the same moment.
template <class T>
[[nodiscard]] EventChannel<T>& Channel() {
	static EventChannel<T> channel;
	return channel;
}

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_EVENTS_CHANNEL_H_
