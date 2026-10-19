#ifndef EMULATOR_SRC_GRAPHICS_GUEST_GPU_PENDINGLABELS_H_
#define EMULATOR_SRC_GRAPHICS_GUEST_GPU_PENDINGLABELS_H_

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace Libs::Graphics {

// End-of-pipe label writes (EVENT_WRITE_EOP/EOS, RELEASE_MEM) the host GPU has not completed.
//
// The guest learns from a label that the GPU finished the work before it: it then reads what
// the work wrote, or reuses the memory the work read. So a label reaches guest memory only when
// the host GPU completed the work recorded before it. Until then the command processor still
// sees it: the work after a WAIT_REG_MEM on it runs after it on the host GPU anyway, as the
// host has one queue in the order the GPU thread records.
//
// The GPU thread records a label (Add); the host GPU's completion of it marks it completed
// (Complete, any thread) and wakes the GPU thread, which takes the completed labels in the order
// they were recorded (TakeCompleted), writes them to guest memory with their interrupts, and
// retires them (Retire). A label without bytes is an interrupt alone, kept in order with the
// labels.
class PendingLabels final {
public:
	struct Label {
		uint64_t address           = 0;
		uint64_t value             = 0;
		uint32_t bytes             = 0; // 4, 8, or 0 for an interrupt alone
		bool     interrupt         = false;
		int      interrupt_event   = 0;
		uint32_t interrupt_context = 0;
		uint64_t serial            = 0;
	};

	// GPU thread. Returns the label's serial.
	uint64_t Add(Label label) {
		std::scoped_lock lock {m_mutex};
		label.serial = ++m_last_serial;
		m_recorded.push_back({label, false});
		if (label.bytes != 0) {
			m_latest[label.address] = label;
		}
		return label.serial;
	}

	// Any thread. The host GPU completed the work recorded before the label.
	void Complete(uint64_t serial) {
		std::function<void()> wake;
		{
			std::scoped_lock lock {m_mutex};
			for (auto& entry: m_recorded) {
				if (entry.label.serial == serial) {
					entry.completed = true;
					break;
				}
			}
			wake = m_wake;
		}
		if (wake) {
			wake();
		}
	}

	// GPU thread. Appends the completed labels in recorded order, up to the first that is not
	// completed.
	void TakeCompleted(std::vector<Label>& labels) {
		std::scoped_lock lock {m_mutex};
		while (!m_recorded.empty() && m_recorded.front().completed) {
			labels.push_back(m_recorded.front().label);
			m_recorded.pop_front();
		}
	}

	// GPU thread. A taken label is in guest memory now.
	void Retire(const Label& label) {
		if (label.bytes == 0) {
			return;
		}
		std::scoped_lock lock {m_mutex};
		const auto latest = m_latest.find(label.address);
		if (latest != m_latest.end() && latest->second.serial == label.serial) {
			m_latest.erase(latest);
		}
	}

	// GPU thread. Reads a 32- or 64-bit value as the command processor sees it: guest memory
	// with the labels not retired yet written over it, the newest last.
	template <typename T>
	[[nodiscard]] T Read(const T* address) {
		static_assert(sizeof(T) == sizeof(uint32_t) || sizeof(T) == sizeof(uint64_t));
		// Another guest thread or the host GPU can write it: one load, not one the compiler keeps.
		T value = *static_cast<const volatile T*>(address);
		const auto begin = reinterpret_cast<uint64_t>(address);
		const auto end   = begin + sizeof(T);
		std::scoped_lock lock {m_mutex};
		if (m_latest.empty()) {
			return value;
		}
		std::vector<const Label*> overlapping;
		for (auto it = m_latest.lower_bound(begin >= 7 ? begin - 7 : 0);
		     it != m_latest.end() && it->first < end; ++it) {
			if (it->first + it->second.bytes > begin) {
				overlapping.push_back(&it->second);
			}
		}
		std::sort(overlapping.begin(), overlapping.end(),
		          [](const Label* a, const Label* b) { return a->serial < b->serial; });
		auto* bytes = reinterpret_cast<uint8_t*>(&value);
		for (const auto* label: overlapping) {
			const auto* source = reinterpret_cast<const uint8_t*>(&label->value);
			for (uint32_t i = 0; i < label->bytes; i++) {
				const auto at = label->address + i;
				if (at >= begin && at < end) {
					bytes[at - begin] = source[i];
				}
			}
		}
		return value;
	}

	// The GPU thread's wake-up for completions; null when it no longer takes them.
	void SetWake(std::function<void()> wake) {
		std::scoped_lock lock {m_mutex};
		m_wake = std::move(wake);
	}

	[[nodiscard]] bool Empty() {
		std::scoped_lock lock {m_mutex};
		return m_recorded.empty() && m_latest.empty();
	}

private:
	struct Entry {
		Label label;
		bool  completed = false;
	};

	std::mutex                m_mutex;
	std::deque<Entry>         m_recorded;
	// The newest label of each address that is not retired.
	std::map<uint64_t, Label> m_latest;
	uint64_t                  m_last_serial = 0;
	std::function<void()>     m_wake;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_GUEST_GPU_PENDINGLABELS_H_ */
