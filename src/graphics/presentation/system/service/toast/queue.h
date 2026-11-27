#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_QUEUE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_QUEUE_H_

#include "graphics/presentation/system/core.h"
#include "graphics/presentation/system/service/toast/options.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace Libs::Graphics::SystemUi {

/// How many toasts are on screen at once. More wait their turn, so a burst doesn't cover the game.
inline constexpr size_t DEFAULT_VISIBLE_TOASTS = 3;

/// How many toasts wait for room. A burst past it drops the oldest waiting, as they are stale by
/// the time room comes, so a flood can't queue toasts for minutes.
inline constexpr size_t MAX_WAITING_TOASTS = 16;

/// A toast on screen, either shown or on its way out.
struct ToastEntry {
	uint64_t     id = 0;
	ToastRequest toast;
	// False once its hold has ended or it was dismissed, so it fades out. Its fade is played where
	// it is drawn, which removes it once the fade has finished.
	bool      shown = true;
	TimePoint hide_at {};
};

/// The toasts on screen and those waiting for room. It decides which are shown and for how long,
/// and leaves fading them in and out to whoever draws them, so there is one clock for each fade.
/// The drawer builds every entry each frame, fading it in while shown and out once not, and calls
/// Remove once the fade out has finished. Not thread safe, as it is used on the thread that draws.
class ToastQueue {
public:
	/// An empty list showing at most `visible_limit` toasts at once, at least one.
	explicit ToastQueue(size_t visible_limit = DEFAULT_VISIBLE_TOASTS);

	/// Shows `toast` from `now`, or queues it when the screen is full, dropping the oldest waiting
	/// toast when MAX_WAITING_TOASTS already wait. A toast with the key of one
	/// already on screen replaces it and starts its hold again, fading it back in if it was
	/// leaving, and one with the key of a waiting toast replaces that. Returns the toast's id.
	uint64_t Add(ToastRequest toast, TimePoint now);

	/// Adds `toast` as Add does, giving a new toast the id `id`, for a caller that hands out ids
	/// of its own before the toast reaches the queue. A repeat by key keeps the id of the toast
	/// already there and returns it, so the caller can tell `id` merged. The queue's own ids
	/// aren't advanced, so use one Add or the other on a queue, never both.
	uint64_t Add(ToastRequest toast, TimePoint now, uint64_t id);

	/// Whether the toast with `id` is on screen or waiting.
	[[nodiscard]] bool Contains(uint64_t id) const;

	/// Starts the toast with `id` leaving, or forgets it if it was still waiting. Returns false
	/// when there is none, for example because it already left.
	bool Dismiss(uint64_t id);

	/// Starts leaving each shown toast whose hold has ended at `now`.
	void Update(TimePoint now);

	/// Forgets the toast with `id` once its fade out has finished, and shows waiting toasts in the
	/// room it leaves, each from `now`.
	void Remove(uint64_t id, TimePoint now);

	/// The toasts on screen, oldest first.
	[[nodiscard]] const std::vector<ToastEntry>& OnScreen() const { return m_on_screen; }

	/// How many toasts are waiting for room.
	[[nodiscard]] size_t Waiting() const { return m_waiting.size(); }

	/// Whether there are no toasts, shown, leaving or waiting.
	[[nodiscard]] bool Empty() const { return m_on_screen.empty() && m_waiting.empty(); }

	/// Moves the ids of the toasts that came on screen since it was last asked onto the end of
	/// `out`, in the order they came, and forgets them. A toast comes on screen once, when it is
	/// shown or takes the room of one that left, and not again when a repeat refreshes it, so the
	/// caller can greet each toast once, such as with a trophy's sound. The ids gather until taken,
	/// so whoever shows toasts takes them every frame, as the toast service does.
	void TakeNewlyShown(std::vector<uint64_t>& out);

private:
	/// Puts `entry` on screen from `now`.
	void Show(ToastEntry entry, TimePoint now);

	std::vector<ToastEntry> m_on_screen;
	std::deque<ToastEntry>  m_waiting;
	std::vector<uint64_t>   m_newly_shown;
	size_t                  m_visible_limit;
	uint64_t                m_last_id = 0;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_SERVICE_TOAST_QUEUE_H_
