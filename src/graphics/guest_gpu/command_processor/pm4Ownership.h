#ifndef GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_OWNERSHIP_H
#define GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_OWNERSHIP_H

#include "common/abi.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {

// Reads exactly `size` bytes from the guest virtual address `vaddr` into `data`.
// Must return false, without touching `data`, if any part of the range is not mapped.
using Pm4GuestReader = std::function<bool(uint64_t vaddr, void* data, uint64_t size)>;

// Why a guest reference could not be owned at submit time.
enum class Pm4CaptureMissReason {
	NoReader,     // no reader is installed (capture disabled)
	ReadFailed,   // the reader rejected the range (unmapped, or not host-readable)
	TooManyRanges // the capture budget was exhausted before this range was reached
};

// A single missed reference, retained (bounded) purely for diagnostics.
struct Pm4CaptureMiss {
	uint64_t             address   = 0;
	uint32_t             num_dw    = 0;
	uint32_t             header    = 0; // packet header of the referencing packet
	uint32_t             offset_dw = 0; // referencing packet's offset within its stream
	uint32_t             depth     = 0; // 0 = root stream, 1 = first-level indirect, ...
	Pm4CaptureMissReason reason    = Pm4CaptureMissReason::NoReader;
};

// Submit-time ownership of every guest-memory region that the PM4 parser dereferences after a
// submission has been queued: the register/value pair arrays consumed by the three
// IT_SET_*_REG_INDIRECT packets and every nested indirect command buffer
// (IT_INDIRECT_BUFFER, including both targets of the predicated 14-dword branch variant).
//
// The parser used to read those regions straight out of guest memory on the GPU thread, long
// after the guest had moved on and rewritten them for a later frame. This class walks an
// emulator-owned copy of a root command stream *at submit time* and takes owned copies of every
// referenced region, so the later parse never depends on guest memory that the guest may have
// rewritten in the meantime.
//
// The walk is deliberately a linear decode that advances by Pm4::PacketSizeDw (the parser's own
// packet-length function) and never interprets conditions. It therefore visits a superset of the
// packets the deferred parse can reach - a COND_EXEC body is contiguous with its skip target, and a
// chained indirect buffer merely makes the parent's remaining packets unreachable. Most handlers
// return the packet's header length minus one, so "handler return + 1" equals the declared length
// and the walk stays in step. Two handlers diverge and are mirrored explicitly below: the
// SH_NOP/CX_NOP wrapper forms advance three dwords regardless of the declared length, and the
// AA-sample-location context packet consumes an embedded sub-packet that is itself a well-formed
// packet, so the plain linear decode lands on the same offset the parser computes. Copying a
// superset is always safe; a miss is a hard error reported by the caller.
//
// Besides the packet references above, the walk also snapshots the vertex-input descriptor tables
// that the deferred *draw* path dereferences. Those tables (the V# buffer table and the attribute
// table) are reached through 64-bit pointers stored in the GS/HS user-data registers, and the draw
// is emitted from the deferred parse up to a frame after the submission was queued. The user-data
// values themselves already live in the owned stream, but the tables they point at do not, so the
// guest can recycle them before the draw runs. Because the register *indices* come from the shader
// binary (see ShaderReadVertexMetadata), the pointers cannot be identified by slot here; every
// even-aligned 64-bit candidate in a written GS/HS user-data window is captured optimistically.
// Those snapshots are optional (a slot may hold anything), so an unreadable candidate or an
// exhausted optional budget is not a miss; they only ever add coverage.
class Pm4Ownership {
public:
	Pm4Ownership() = default;
	explicit Pm4Ownership(Pm4GuestReader reader) : m_reader(std::move(reader)) {}

	// Dwords captured for each GS/HS user-data table pointer candidate. Covers the worst case the
	// vertex-input path can read from either table: ShaderVertexInputInfo::RES_MAX (32) V#
	// descriptors of four dwords each, and 32 attribute entries.
	static constexpr uint32_t kUserDataTableDwords = 128u;

	// Walk `root` (an emulator-owned copy of a submitted command stream) and take owned copies of
	// every guest region it references, recursing into nested command buffers.
	void Capture(std::span<const uint32_t> root);

	// Owned dwords for the guest range starting at `address`, or an empty span if the range was
	// not captured (or was captured shorter than requested). An empty result is a miss: the
	// caller must treat it as an uncovered reference, not as "read it live".
	[[nodiscard]] std::span<const uint32_t> Find(uint64_t address, uint32_t num_dw) const;

	[[nodiscard]] bool     Empty() const noexcept { return m_index.empty(); }
	[[nodiscard]] size_t   RangeCount() const noexcept { return m_index.size(); }
	[[nodiscard]] uint32_t MissCount() const noexcept { return m_misses; }
	[[nodiscard]] uint32_t MalformedCount() const noexcept { return m_malformed; }

	// Human-readable summary of the missed references (bounded to kMaxMissRecords). Empty string
	// when there were none. Intended for diagnostics only.
	[[nodiscard]] std::string DescribeMisses() const;

private:
	// Sentinel index meaning "nothing captured".
	static constexpr size_t kNoRange = static_cast<size_t>(-1);

	// Upper bounds that keep a corrupt stream from making the capture unbounded.
	static constexpr uint32_t kMaxWalkDepth      = 16u;
	static constexpr size_t   kMaxCapturedRanges = 8192u;
	static constexpr size_t   kMaxMissRecords    = 8u;

	// Upper bound on the optimistic user-data table snapshots. They are speculative, so they are
	// additionally capped (and never recorded as misses) to keep a stream that writes many
	// distinguishable user-data windows from crowding out the required captures above.
	static constexpr size_t kMaxOptionalRanges = 4096u;

	[[nodiscard]] size_t CaptureRange(uint64_t address, uint32_t num_dw, uint32_t header,
	                                  uint32_t offset_dw, uint32_t depth);
	// Optimistic capture used for the user-data table candidates. Returns the owned range index,
	// an existing (possibly shorter) range, or kNoRange. Never counts a miss: a user-data slot may
	// legitimately hold a non-pointer, so a failed candidate read is not an error.
	[[nodiscard]] size_t CaptureOptionalRange(uint64_t address, uint32_t num_dw);
	// For an IT_SET_SH_REG packet, capture the 64-bit pointer candidates in the GS/HS user-data
	// windows it writes. `buf` points at the first written register (`buf[0]` is the register
	// offset) and `payload_dw` is the number of value dwords after it.
	void CaptureUserDataTables(const uint32_t* buf, uint32_t payload_dw);
	void RecordMiss(uint64_t address, uint32_t num_dw, uint32_t header,
	                uint32_t offset_dw, uint32_t depth, Pm4CaptureMissReason reason);
	void                 Walk(std::span<const uint32_t> stream, uint32_t depth);
	void WalkNested(uint64_t address, uint32_t num_dw, uint32_t header, uint32_t offset_dw,
	                uint32_t depth);

	Pm4GuestReader m_reader;

	// Element storage. std::deque never relocates existing elements, so a span handed out by Find
	// (or held by a cursor while a nested stream is walked) stays valid even as more ranges are
	// captured. An existing range is never modified in place; a longer capture appends a new
	// element and repoints the index, which also keeps any span already handed out valid.
	std::deque<std::vector<uint32_t>> m_storage;
	std::map<uint64_t, size_t>        m_index;

	uint32_t m_misses    = 0;
	uint32_t m_malformed = 0;

	// Number of optimistic user-data table ranges captured so far (bounded by kMaxOptionalRanges).
	size_t m_optional_ranges = 0;

	// Bounded records of the first few misses, for DescribeMisses.
	std::vector<Pm4CaptureMiss> m_miss_records;
};

} // namespace Libs::Graphics

#endif // GRAPHICS_GUEST_GPU_COMMAND_PROCESSOR_PM4_OWNERSHIP_H
