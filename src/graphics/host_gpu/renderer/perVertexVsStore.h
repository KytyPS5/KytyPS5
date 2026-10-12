#ifndef KYTY_GRAPHICS_HOST_GPU_RENDERER_PERVERTEXVSSTORE_H_
#define KYTY_GRAPHICS_HOST_GPU_RENDERER_PERVERTEXVSSTORE_H_

// VS-store mode (default off). Instead of PerVertexKHR, the guest vertex
// shader writes its PerVertex parameters (uint4, bits unchanged) to an SSBO record
// cap[record(instance, vertex)] and the pixel shader reads the three corners of its primitive back
// from there: cap[record(instance, index[first_index + primitive * 3 + k] + vertex_offset)].
// Pure helpers (no Vulkan, no globals) shared by the renderer, the SPIR-V emitter, the CPU tests
// and the GPU harness: the plan for a draw, the record numbering both shader sides must agree on,
// and the per-pass region ring. The emitter mirrors the arithmetic below in SPIR-V.

#include <array>
#include <bit>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace Libs::Graphics {

// Guest parameters 0..30; location 31 carries the instance index from the VS to the PS (flat).
constexpr uint32_t kPvVsStoreInstanceLocation = 31;
constexpr uint32_t kPvVsStoreCorners          = 3;

// Dword layout of the push block that follows the native push data in both stages.
enum PvVsStorePush : uint32_t {
	kPvPushBase          = 0, // record of (instance 0, vertex `first`) in the pass ring
	kPvPushSpan          = 1, // records per instance
	kPvPushFirst         = 2, // first vertex index (index + vertex_offset) of the record range
	kPvPushFirstInstance = 3, // first instance of the draw
	kPvPushFirstIndex    = 4, // element offset into the index buffer
	kPvPushVertexOffset  = 5, // signed
	kPvPushIndex32       = 6, // 1: 32-bit indices, 0: 16-bit
	kPvPushStride        = 7, // vec4 slots per record (slot == host parameter location)
};
constexpr uint32_t kPvVsStorePushDwords = 8;

// Which index element corner k of a triangle reads (the PerVertexKHR order of the guest PS).
constexpr uint32_t PvVsStoreCornerElement(uint32_t k) {
	return k;
}

// Record written by the VS invocation (instance, vertex_index); vertex_index is what the host
// shader sees as gl_VertexIndex (index + vertex_offset for indexed draws).
constexpr uint32_t PvVsStoreVertexRecord(uint32_t base, uint32_t span, uint32_t first,
                                         uint32_t first_instance, uint32_t instance,
                                         uint32_t vertex_index) {
	return base + (instance - first_instance) * span + (vertex_index - first);
}

// Index element `element` of the bound index buffer (little endian dwords).
constexpr uint32_t PvVsStoreFetchIndex(std::span<const uint32_t> words, bool index32,
                                       uint32_t element) {
	if (index32) return element < words.size() ? words[element] : 0u;
	const uint32_t word = (element >> 1u) < words.size() ? words[element >> 1u] : 0u;
	return (word >> ((element & 1u) * 16u)) & 0xffffu;
}

// Draw parameters both sides use (the push block as a struct).
struct PvVsStoreDraw {
	uint32_t base           = 0;
	uint32_t span           = 0;
	uint32_t first          = 0;
	uint32_t first_instance = 0;
	uint32_t first_index    = 0;
	int32_t  vertex_offset  = 0;
	bool     index32        = true;
};

// Record the PS reads for corner k of `primitive` in `instance`.
constexpr uint32_t PvVsStoreCornerRecord(const PvVsStoreDraw&      draw,
                                         std::span<const uint32_t> index_words, uint32_t instance,
                                         uint32_t primitive, uint32_t k) {
	const uint32_t element =
	    draw.first_index + primitive * kPvVsStoreCorners + PvVsStoreCornerElement(k);
	const uint32_t index        = PvVsStoreFetchIndex(index_words, draw.index32, element);
	const uint32_t vertex_index = index + static_cast<uint32_t>(draw.vertex_offset);
	return PvVsStoreVertexRecord(draw.base, draw.span, draw.first, draw.first_instance,
	                             draw.first_instance + instance, vertex_index);
}

enum class PvVsStoreVerdict : uint32_t {
	Disabled,         // switch off: nothing counted, PerVertexKHR as before
	Use,              // the draw goes through the VS store
	NotTriangleList,  // stays on PerVertexKHR (RectList and the rest)
	NotIndexed,       // stays on PerVertexKHR
	PrimitiveRestart, // stays on PerVertexKHR
	BadIndexSize,     // index buffer is neither 16 nor 32 bit
	NoRange,          // the vertex range of the draw is not known
	Dropped,          // a corner points outside the vertex buffer: the draw stays on PerVertexKHR
	Empty,            // nothing to draw
	TooLarge,         // record count overflows the 32-bit math of the shaders
	RingFull,         // the pass ring has no room for a new region
};

struct PvVsStoreRequest {
	bool     enabled            = false; // KYTY_PV_VSSTORE=1
	bool     triangle_list      = false;
	bool     indexed            = false;
	bool     primitive_restart  = false;
	uint32_t index_size         = 0; // bytes
	bool     range_known        = false;
	bool     range_dropped      = false;
	uint64_t range_first        = 0;
	uint64_t range_end          = 0; // [first, end) vertex indices the draw reads
	uint32_t index_count        = 0;
	uint32_t instance_count     = 0;
	uint32_t record_stride_vec4 = 0;
};

struct PvVsStorePlan {
	PvVsStoreVerdict   verdict = PvVsStoreVerdict::Disabled;
	uint32_t           first   = 0; // first vertex of the range
	uint32_t           span    = 0; // records per instance
	uint64_t           records = 0; // span * instances
	[[nodiscard]] bool Use() const { return verdict == PvVsStoreVerdict::Use; }
};

constexpr PvVsStorePlan PlanPvVsStore(const PvVsStoreRequest& request) {
	using V = PvVsStoreVerdict;
	PvVsStorePlan plan;
	if (!request.enabled) return plan;
	const auto reject = [&](V verdict) {
		plan.verdict = verdict;
		return plan;
	};
	if (!request.triangle_list) return reject(V::NotTriangleList);
	if (!request.indexed) return reject(V::NotIndexed);
	if (request.primitive_restart) return reject(V::PrimitiveRestart);
	if (request.index_size != 2 && request.index_size != 4) return reject(V::BadIndexSize);
	if (!request.range_known) return reject(V::NoRange);
	if (request.range_dropped) return reject(V::Dropped);
	// A zero stride leaves nothing to store (and would hide the size of the draw from the check
	// below).
	if (request.index_count < kPvVsStoreCorners || request.instance_count == 0 ||
	    request.range_end <= request.range_first || request.record_stride_vec4 == 0)
		return reject(V::Empty);
	const uint64_t span    = request.range_end - request.range_first;
	const uint64_t records = span * request.instance_count;
	// The record base is a uint32_t and record * stride * 4 dwords must stay below 2^32 in the
	// shaders.
	if (request.range_end > 0xffffffffull || records > 0xffffffffull ||
	    records * request.record_stride_vec4 * 4ull > 0xffffffffull)
		return reject(V::TooLarge);
	plan.verdict = V::Use;
	plan.first   = static_cast<uint32_t>(request.range_first);
	plan.span    = static_cast<uint32_t>(span);
	plan.records = records;
	return plan;
}

// Regions of one render pass: every draw gets its own, none is reused until the pass ends (the
// a shared region would be overwritten by the next draw).
class PvVsStoreRing {
public:
	explicit PvVsStoreRing(uint64_t capacity_records = 0): m_capacity(capacity_records) {}
	void SetCapacity(uint64_t capacity_records) { m_capacity = capacity_records; }
	// Returns false when the pass ring is full (the caller falls back to PerVertexKHR).
	bool Allocate(uint64_t records, uint32_t& base) {
		if (records == 0 || records > m_capacity - m_used) return false;
		base = static_cast<uint32_t>(m_used);
		m_used += records;
		m_draws++;
		return true;
	}
	void                   EndPass() { m_used = 0; }
	[[nodiscard]] uint64_t Used() const { return m_used; }
	[[nodiscard]] uint64_t Draws() const { return m_draws; }

private:
	uint64_t m_capacity = 0;
	uint64_t m_used     = 0;
	uint64_t m_draws    = 0;
};

// Slot of every VS Parameter output in a record (sorted by location, dense from 0).
inline std::map<uint32_t, uint32_t> PvVsStoreSlots(std::span<const uint32_t> parameter_locations) {
	std::map<uint32_t, uint32_t> sorted;
	for (const auto location: parameter_locations)
		sorted.emplace(location, 0);
	uint32_t slot = 0;
	for (auto& [location, value]: sorted)
		value = slot++;
	return sorted;
}

// the two shader variants are emitted at compile time, independently (the PS does not know its
// VS), so a Parameter output of host location L lives in slot L of a record and the record stride
// (push dword kPvPushStride) is the highest location the VS writes plus one.
constexpr uint32_t PvVsStoreStrideFor(uint32_t vs_location_mask) {
	return vs_location_mask == 0 ? 0u
	                             : 32u - static_cast<uint32_t>(std::countl_zero(vs_location_mask));
}

enum class PvVsStoreInterfaceVerdict : uint32_t {
	Use,
	Param31,       // the VS or the PS uses host location 31, which carries the instance index
	MissingOutput, // the PS reads a per-vertex location the VS does not write
	NoPerVertex,   // the PS has no per-vertex input at all
};

constexpr PvVsStoreInterfaceVerdict ClassifyPvVsStoreInterface(uint32_t vs_location_mask,
                                                               uint32_t ps_location_mask) {
	using V = PvVsStoreInterfaceVerdict;
	if (ps_location_mask == 0) return V::NoPerVertex;
	if (((vs_location_mask | ps_location_mask) >> kPvVsStoreInstanceLocation) & 1u)
		return V::Param31;
	if ((ps_location_mask & ~vs_location_mask) != 0) return V::MissingOutput;
	return V::Use;
}

// The push block both stages read after the native push data (dword order = PvVsStorePush).
constexpr std::array<uint32_t, kPvVsStorePushDwords> PvVsStorePushBlock(const PvVsStoreDraw& draw,
                                                                        uint32_t stride_vec4) {
	std::array<uint32_t, kPvVsStorePushDwords> block {};
	block[kPvPushBase]          = draw.base;
	block[kPvPushSpan]          = draw.span;
	block[kPvPushFirst]         = draw.first;
	block[kPvPushFirstInstance] = draw.first_instance;
	block[kPvPushFirstIndex]    = draw.first_index;
	block[kPvPushVertexOffset]  = static_cast<uint32_t>(draw.vertex_offset);
	block[kPvPushIndex32]       = draw.index32 ? 1u : 0u;
	block[kPvPushStride]        = stride_vec4;
	return block;
}

// Push constant range of a graphics pipeline layout: the native data, plus the VS-store block only
// for the VS-store variants (every other pipeline keeps the layout it always had).
constexpr uint32_t PvVsStorePushRangeBytes(uint32_t native_bytes, bool vs_store_variant) {
	return native_bytes + (vs_store_variant ? kPvVsStorePushDwords * 4u : 0u);
}

// Storage-buffer descriptors must start at minStorageBufferOffsetAlignment: the draw binds the
// index buffer from the aligned-down offset and moves the difference into first_index.
struct PvVsStoreIndexBinding {
	uint64_t offset      = 0; // descriptor offset (aligned)
	uint32_t first_index = 0; // element offset inside the descriptor range
	bool     ok          = false;
};
constexpr PvVsStoreIndexBinding PvVsStoreBindIndex(uint64_t byte_offset, uint32_t element_size,
                                                   uint64_t alignment) {
	if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
	    (element_size != 2 && element_size != 4))
		return {};
	const uint64_t aligned = byte_offset & ~(alignment - 1);
	const uint64_t delta   = byte_offset - aligned;
	if (delta % element_size != 0) return {};
	return {aligned, static_cast<uint32_t>(delta / element_size), true};
}

// Descriptor range of the index buffer: the PS reads it as uint32 words, so a range that ends
// inside a word (16-bit indices, odd end) makes the last word out of range and the last index reads
// wrong.
constexpr uint64_t PvVsStoreIndexRange(uint32_t first_index, uint32_t element_size,
                                       uint64_t source_size) {
	const uint64_t bytes = uint64_t {first_index} * element_size + source_size;
	return (bytes + 3u) & ~uint64_t {3};
}

// What the draw binds for the index buffer: the aligned descriptor offset, first_index, and the
// range clamped so that offset + range never exceeds the buffer (a descriptor past the end is
// invalid). When the buffer ends inside the last word the PS reads, that word does not fit:
// `tail_fits` is false and the draw stays on PerVertexKHR (pv_fallback_index_tail). `clamp` is a
// test seam.
struct PvVsStoreIndexDescriptor {
	uint64_t offset      = 0;
	uint32_t first_index = 0;
	uint64_t range       = 0;
	bool     ok          = false; // the offset can be bound at all
	bool     tail_fits   = false; // the whole last word is inside the buffer
};
constexpr PvVsStoreIndexDescriptor
PvVsStoreIndexDescriptorFor(uint64_t byte_offset, uint32_t element_size, uint64_t alignment,
                            uint64_t source_size, uint64_t buffer_size, bool clamp = true) {
	PvVsStoreIndexDescriptor out;
	const auto               bind = PvVsStoreBindIndex(byte_offset, element_size, alignment);
	if (!bind.ok) return out;
	out.ok                   = true;
	out.offset               = bind.offset;
	out.first_index          = bind.first_index;
	const uint64_t wanted    = PvVsStoreIndexRange(bind.first_index, element_size, source_size);
	const uint64_t available = buffer_size > bind.offset ? buffer_size - bind.offset : 0;
	out.range                = clamp && wanted > available ? available : wanted;
	out.tail_fits            = !clamp || wanted <= available;
	return out;
}

// Cap slabs of the renderer. A region lives until the command buffer of its draw has finished on
// the GPU, so a slab is recycled only after the tick it was last used in is free; regions inside a
// slab are never reused (PvVsStoreRing). The buffers themselves are the renderer's; this is the
// bookkeeping.
class PvVsStoreSlabs {
public:
	struct Placement {
		bool     ok      = false;
		bool     created = false; // the caller must create the buffer of slab `slab`
		uint32_t slab    = 0;
		uint32_t base    = 0;
	};
	PvVsStoreSlabs(uint64_t slab_records, uint32_t max_slabs)
	    : m_slab_records(slab_records), m_max_slabs(max_slabs) {}

	template <class IsFree>
	Placement Place(uint64_t records, uint64_t tick, IsFree&& is_free) {
		Placement out;
		if (records == 0 || records > m_slab_records) return out;
		const auto try_slab = [&](uint32_t index) {
			if (!m_slabs[index].ring.Allocate(records, out.base)) return false;
			m_slabs[index].tick = tick;
			m_current           = index;
			out.ok              = true;
			out.slab            = index;
			return true;
		};
		if (m_current < m_slabs.size() && m_slabs[m_current].tick == tick && try_slab(m_current))
			return out;
		for (uint32_t i = 0; i < m_slabs.size(); ++i) {
			if (i == m_current) continue;
			auto&      slab     = m_slabs[i];
			const bool reusable = slab.tick == tick || !slab.used || is_free(slab.tick);
			if (!reusable) continue;
			if (slab.tick != tick) slab.ring.EndPass();
			slab.used = true;
			if (try_slab(i)) return out;
		}
		if (m_current < m_slabs.size() && m_slabs[m_current].tick != tick &&
		    (!m_slabs[m_current].used || is_free(m_slabs[m_current].tick))) {
			m_slabs[m_current].ring.EndPass();
			if (try_slab(m_current)) return out;
		}
		if (m_slabs.size() >= m_max_slabs) return out;
		m_slabs.push_back({PvVsStoreRing(m_slab_records), tick, true});
		out.created = true;
		if (!try_slab(static_cast<uint32_t>(m_slabs.size() - 1))) out.created = false;
		return out;
	}
	// The draw of a placed region is recorded in `tick`, which can be newer than the tick of
	// Place() when resource preparation flushed the scheduler in between: the slab is busy until
	// that tick finishes.
	void Touch(uint32_t slab, uint64_t tick) {
		if (slab < m_slabs.size() && m_slabs[slab].tick < tick) m_slabs[slab].tick = tick;
	}
	[[nodiscard]] size_t   Size() const { return m_slabs.size(); }
	[[nodiscard]] uint64_t SlabRecords() const { return m_slab_records; }

private:
	struct Slab {
		PvVsStoreRing ring;
		uint64_t      tick = 0;
		bool          used = false;
	};
	std::vector<Slab> m_slabs;
	uint64_t          m_slab_records;
	uint32_t          m_max_slabs;
	uint32_t          m_current = UINT32_MAX;
};

// Which pv_fallback_* counter a draw that stays on PerVertexKHR is counted in. Instanced draws that
// fail for a size reason (range / region too large / no ring room) are counted as instanced.
enum class PvVsStoreFallback : uint32_t {
	None,
	Topology,
	Range,
	Instanced,
	Ring,
	Param31,
	Interface,
	IndexTail
};

constexpr PvVsStoreFallback ClassifyPvVsStoreFallback(PvVsStoreVerdict verdict,
                                                      uint32_t         instance_count) {
	using V = PvVsStoreVerdict;
	using F = PvVsStoreFallback;
	switch (verdict) {
		case V::Disabled:
		case V::Use: return F::None;
		case V::NotTriangleList:
		case V::NotIndexed:
		case V::PrimitiveRestart:
		case V::BadIndexSize: return F::Topology;
		case V::RingFull: return instance_count > 1 ? F::Instanced : F::Ring;
		case V::NoRange:
		case V::Dropped:
		case V::Empty:
		case V::TooLarge: return instance_count > 1 ? F::Instanced : F::Range;
	}
	return F::Range;
}

constexpr PvVsStoreFallback ClassifyPvVsStoreFallback(PvVsStoreInterfaceVerdict verdict) {
	using V = PvVsStoreInterfaceVerdict;
	switch (verdict) {
		case V::Use:
		case V::NoPerVertex: return PvVsStoreFallback::None;
		case V::Param31: return PvVsStoreFallback::Param31;
		case V::MissingOutput: return PvVsStoreFallback::Interface;
	}
	return PvVsStoreFallback::Interface;
}

// The whole decision of a draw, in the order the renderer applies it: the shader pair must be a
// candidate, the plan must accept the draw, the index buffer must be bindable as an SSBO, and the
// cap ring must have room (`place(plan)` reserves the region). Anything else stays on PerVertexKHR
// and is counted under `fallback`.
struct PvVsStoreDecision {
	bool              use      = false;
	PvVsStoreFallback fallback = PvVsStoreFallback::None;
	PvVsStorePlan     plan;
};

template <class Place>
PvVsStoreDecision DecidePvVsStore(PvVsStoreInterfaceVerdict interface_verdict,
                                  const PvVsStoreRequest& request, bool index_aligned,
                                  Place&& place, bool index_tail_fits = true,
                                  bool check_tail = true) {
	PvVsStoreDecision decision;
	if (interface_verdict != PvVsStoreInterfaceVerdict::Use) {
		decision.fallback = ClassifyPvVsStoreFallback(interface_verdict);
		return decision;
	}
	decision.plan = PlanPvVsStore(request);
	if (!decision.plan.Use()) {
		decision.fallback =
		    ClassifyPvVsStoreFallback(decision.plan.verdict, request.instance_count);
		return decision;
	}
	if (!index_aligned) {
		decision.fallback = PvVsStoreFallback::Range;
		return decision;
	}
	if (check_tail &&
	    !index_tail_fits) { // check before the ring is touched; check_tail=false is a test seam
		decision.fallback = PvVsStoreFallback::IndexTail;
		return decision;
	}
	if (!place(decision.plan)) {
		decision.fallback =
		    ClassifyPvVsStoreFallback(PvVsStoreVerdict::RingFull, request.instance_count);
		return decision;
	}
	decision.use = true;
	return decision;
}

} // namespace Libs::Graphics

#endif
