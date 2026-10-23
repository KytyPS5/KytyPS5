#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEHEAT_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEHEAT_H_

#include "common/bitArray.h"

#include <array>
#include <cstddef>
#include <cstdint>

// Which pages of a tracked region the guest writes again and again.
//
// A clean page is write-protected, so that the first write to it is seen. A game that rewrites a
// buffer many times per frame then pays, for each page and each time: a fault, a protection
// change in the fault handler, and another one when the renderer has uploaded the page. A page
// that keeps coming back like this is "hot": it stays open and counts as written, and the
// renderer uploads it at every use: at every bind, and once per guest submission for shaders
// that read through device addresses (guestWriteLog.h). That is always correct, since an upload of
// unchanged bytes changes nothing; it trades the faults for copies.
//
// The heat is a lease of LeaseFrames guest frames (RegionManager::NoteFrame). At the first upload
// after the lease, the page is clean again: it is protected before its bytes are copied, so a
// later write faults and is seen. A page the game still writes faults once and is hot again at
// once; one it stopped writing stays protected. A game that writes a buffer once per frame then
// pays one fault per lease instead of one or two per frame, and a ring buffer touched once per lap
// is not uploaded for long after its last write.
//
// Heat is also forgotten from time to time (CoolAll), for pages that no pass uploads.

namespace Libs::Graphics {

template <size_t Pages>
class WriteHeat final {
public:
	using Bits = Common::BitArray<Pages>;

	// Writes to a clean page, since the last cooling, that make it hot.
	static constexpr uint8_t HotAfter = 2;
	// Guest frames a page stays hot for: about one to two seconds.
	static constexpr uint32_t LeaseFrames = 32;

	// The CPU wrote to pages [start, end) that the tracker had as clean, in guest frame `frame`.
	constexpr void NoteWrite(size_t start, size_t end, uint32_t frame) {
		for (size_t page = start; page < end; page++) {
			if (m_writes[page] < HotAfter && ++m_writes[page] == HotAfter) {
				m_hot.Set(page);
				m_lease_end[page] = frame + LeaseFrames;
			}
		}
	}

	[[nodiscard]] constexpr bool IsHot(size_t page) const { return m_hot.Get(page); }
	[[nodiscard]] constexpr const Bits& Hot() const { return m_hot; }

	// After the caller cleared [start, end) in `dirty` for an upload in guest frame `frame`: the
	// hot pages stay dirty. A page whose lease has ended loses its heat and stays clean; its next
	// write makes it hot again.
	constexpr void KeepHotDirty(Bits& dirty, size_t start, size_t end, uint32_t frame) {
		if (m_hot.None()) {
			return;
		}
		for (size_t page = start; page < end; page++) {
			if (!m_hot.Get(page)) {
				continue;
			}
			// The counter wraps; the difference tells whether `frame` reached the end.
			if (static_cast<int32_t>(frame - m_lease_end[page]) >= 0) {
				m_hot.Unset(page);
				m_writes[page] = HotAfter - 1;
			} else {
				dirty.Set(page);
			}
		}
	}

	// The pages are no longer the CPU's to write at will, for example because the GPU wrote
	// them. Returns true when any of them was hot.
	constexpr bool Cool(size_t start, size_t end) {
		bool any = false;
		for (size_t page = start; page < end; page++) {
			any            = any || m_hot.Get(page);
			m_writes[page] = 0;
		}
		if (any) {
			m_hot.UnsetRange(start, end);
		}
		return any;
	}

	constexpr void CoolAll() {
		m_writes.fill(0);
		m_hot.Clear();
	}

private:
	std::array<uint8_t, Pages>  m_writes {};
	std::array<uint32_t, Pages> m_lease_end {};
	Bits                        m_hot;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_WRITEHEAT_H_
