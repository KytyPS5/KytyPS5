#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FENCERETIREMENT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FENCERETIREMENT_H_

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <vector>

// Tracks one fence per submission of a queue and tells when a submission has retired: when its
// fence, and so the fence of every earlier submission, is signalled.
//
// `Ops` supplies the fence calls: Create(), IsSignaled(fence), Reset(span of fences), Wait(fence)
// and Destroy(fence). Wait may return before the fence is signalled; the state is checked again.
// Resetting needs exclusive access to a fence, so a fence is reset only while no thread waits on
// one. Acquire is called by the submitting thread; IsRetired and WaitRetired by any thread.

namespace Libs::Graphics {

template <typename Fence, typename Ops>
class FenceRetirement {
public:
	explicit FenceRetirement(Ops ops): m_ops(std::move(ops)) {}
	~FenceRetirement() {
		for (const auto& pending: m_pending) {
			m_ops.Destroy(pending.fence);
		}
		for (const auto fence: m_retired) {
			m_ops.Destroy(fence);
		}
		for (const auto fence: m_free) {
			m_ops.Destroy(fence);
		}
	}
	FenceRetirement(const FenceRetirement&)            = delete;
	FenceRetirement& operator=(const FenceRetirement&) = delete;

	// The fence to pass to the submission with this tick. Ticks must increase.
	[[nodiscard]] Fence Acquire(uint64_t tick) {
		std::scoped_lock lock {m_mutex};
		if (m_waiters == 0 && !m_retired.empty()) {
			m_ops.Reset(std::span<const Fence> {m_retired});
			m_free.insert(m_free.end(), m_retired.begin(), m_retired.end());
			m_retired.clear();
		}
		Fence fence {};
		if (!m_free.empty()) {
			fence = m_free.back();
			m_free.pop_back();
		} else {
			fence = m_ops.Create();
		}
		m_pending.push_back({tick, fence});
		return fence;
	}

	[[nodiscard]] bool IsRetired(uint64_t tick) {
		if (m_retired_tick.load(std::memory_order_acquire) >= tick) {
			return true;
		}
		std::scoped_lock lock {m_mutex};
		while (!m_pending.empty()) {
			const auto front = m_pending.front();
			if (!m_ops.IsSignaled(front.fence)) {
				break;
			}
			m_retired.push_back(front.fence);
			m_pending.pop_front();
			m_retired_tick.store(front.tick, std::memory_order_release);
		}
		return m_retired_tick.load(std::memory_order_acquire) >= tick;
	}

	// Blocks until the submission with this tick has retired. The tick must have been acquired.
	void WaitRetired(uint64_t tick) {
		while (!IsRetired(tick)) {
			Fence oldest {};
			{
				std::scoped_lock lock {m_mutex};
				if (m_pending.empty()) {
					// Another thread retired the last fence after our check; check again.
					continue;
				}
				oldest = m_pending.front().fence;
				m_waiters++;
			}
			// Another thread may retire this fence meanwhile. It then stays signalled: no fence
			// is reset while a thread waits.
			m_ops.Wait(oldest);
			std::scoped_lock lock {m_mutex};
			m_waiters--;
		}
	}

private:
	struct Pending {
		uint64_t tick = 0;
		Fence    fence {};
	};

	Ops                   m_ops;
	std::mutex            m_mutex;
	std::deque<Pending>   m_pending; // oldest first; one queue completes in order
	std::vector<Fence>    m_retired; // signalled, not yet reset
	std::vector<Fence>    m_free;
	uint32_t              m_waiters = 0;
	std::atomic<uint64_t> m_retired_tick {0};
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_FENCERETIREMENT_H_
