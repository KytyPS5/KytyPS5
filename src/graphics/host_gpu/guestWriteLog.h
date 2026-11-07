#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTWRITELOG_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTWRITELOG_H_

#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

// Where guest memory changed outside the host GPU since the last upload pass
// (RenderContext::PrepareBda): by the guest CPU, or by the GPU thread itself. A shader that reads
// through device addresses can read any cached buffer, so before it runs every buffer must hold
// the latest bytes, and the pass synchronizes only the logged ranges.
//
// Two kinds of ranges come in:
// - A write: a fault on a watched page, a write by the GPU thread itself (WRITE_DATA, a label),
//   a buffer created over written memory. The next pass must see it.
// - A hot range: uploaded and left open (writeHeat.h), so the CPU's next writes to it neither
//   fault nor reach the log. It is due again once the next guest submission starts. The CPU's
//   writes to it while the GPU processes a submission race with that submission on the console
//   too; the GPU thread's own writes come in as writes.
//
// Safe to call from several threads.

namespace Libs::Graphics {

class GuestWriteLog final {
public:
	// `capacity`: the writes a pass takes before a full pass is cheaper.
	explicit GuestWriteLog(size_t capacity): m_capacity(capacity) {}

	void Record(GuestRange range) {
		std::scoped_lock lock {m_mutex};
		if (m_full_pass) {
			return;
		}
		if (m_writes.size() >= m_capacity) {
			m_writes.clear();
			m_full_pass = true;
			return;
		}
		m_writes.push_back(range);
	}

	// Past the capacity the next submission starts a full pass, as for writes.
	void RecordHot(GuestRange range) {
		std::scoped_lock lock {m_mutex};
		if (m_hot_overflow) {
			return;
		}
		if (m_hot_next.size() >= m_capacity && !m_hot_next.contains(range.address)) {
			m_hot_next.clear();
			m_hot_overflow = true;
			return;
		}
		auto& size = m_hot_next[range.address];
		size       = std::max(size, range.size);
	}

	// A guest submission starts: the hot ranges uploaded before it are due again.
	void BeginSubmission() {
		std::scoped_lock lock {m_mutex};
		for (const auto& [address, size]: m_hot_next) {
			auto& due = m_hot_due[address];
			due       = std::max(due, size);
		}
		m_hot_next.clear();
		if (m_hot_overflow || m_hot_due.size() > m_capacity) {
			m_hot_overflow = false;
			m_hot_due.clear();
			m_writes.clear();
			m_full_pass = true;
		}
	}

	// Every buffer must be synchronized at the next pass (the mapping changed).
	void RequestFullPass() {
		std::scoped_lock lock {m_mutex};
		m_writes.clear();
		m_full_pass = true;
	}

	// The due ranges, sorted by address, overlapping and adjacent ones merged. False when the next
	// pass must synchronize every buffer instead; `out` is empty then.
	[[nodiscard]] bool Take(std::vector<GuestRange>& out) {
		out.clear();
		std::scoped_lock lock {m_mutex};
		if (m_full_pass) {
			m_full_pass = false;
			m_writes.clear();
			m_hot_due.clear();
			return false;
		}
		out.swap(m_writes);
		for (const auto& [address, size]: m_hot_due) {
			out.push_back({address, size});
		}
		m_hot_due.clear();
		std::ranges::sort(out, {}, &GuestRange::address);
		size_t merged = 0;
		for (const auto& range: out) {
			if (merged != 0) {
				auto& last = out[merged - 1];
				if (range.address <= last.End()) {
					last.size = std::max(last.End(), range.End()) - last.address;
					continue;
				}
			}
			out[merged++] = range;
		}
		out.resize(merged);
		return true;
	}

private:
	size_t                       m_capacity;
	std::mutex                   m_mutex;
	std::vector<GuestRange>      m_writes;
	std::map<uint64_t, uint64_t> m_hot_next; // address -> size, due after the next submission
	std::map<uint64_t, uint64_t> m_hot_due;  // address -> size, due at the next pass
	// More hot ranges than the capacity came before the next submission.
	bool                         m_hot_overflow = false;
	bool                         m_full_pass    = true;
};

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_GUESTWRITELOG_H_ */
