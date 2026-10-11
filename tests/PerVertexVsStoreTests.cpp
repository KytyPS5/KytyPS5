// VS-store planner, record model and pass ring (CPU). The model plays the VS store and
// the PS corner read of perVertexVsStore.h bit for bit against a direct read of the vertex array.
// The mutants (tests/PerVertexVsStoreMutants.cmake) change one line of the header; each must fail this test.
#include "graphics/host_gpu/renderer/perVertexVsStore.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace Libs::Graphics;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
	if (!ok) {
		std::fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

struct Vec4 {
	uint32_t w[4];
	bool     operator==(const Vec4&) const = default;
};

// Bits chosen to catch float canonicalisation: NaN with payload, denormal, -0, inf, plain values.
Vec4 SourceVertexBase(uint32_t vertex) {
	switch (vertex % 6) {
		case 0: return {{0x7FFF0001u, 0x00000001u, 0x80000000u, vertex}};
		case 1: return {{0x7F800000u, 0xFF800000u, 0x007FFFFFu, vertex * 3u}};
		default: return {{0x3F800000u + vertex, vertex * 0x9E3779B9u, 0xA5A5A5A5u ^ vertex, vertex}};
	}
}

// What the guest VS outputs for (vertex, instance).
Vec4 SourceVertex(uint32_t vertex, uint32_t instance) {
	Vec4 v = SourceVertexBase(vertex);
	v.w[3] ^= instance * 0x01000193u;
	return v;
}

struct Case {
	const char* name;
	bool        index32;
	uint32_t    first_index;
	int32_t     vertex_offset;
	uint32_t    instances;
	uint32_t    first_instance;
	uint32_t    prims;
};

// Plays one draw. Returns the number of corner reads that differ from the source vertex.
uint32_t RunCase(const Case& c, uint32_t ring_base) {
	const uint32_t stride_dummy = 1;
	const uint32_t vertex_count = 40;
	// Index values stay in [0, 24); the shaders see index + vertex_offset (vertex_offset may be < 0).
	std::vector<uint32_t> indices(c.first_index + c.prims * 3);
	for (size_t i = 0; i < indices.size(); i++) indices[i] = static_cast<uint32_t>((i * 7 + 3) % 24);
	std::vector<uint32_t> words;
	if (c.index32) {
		words = indices;
	} else {
		words.assign((indices.size() + 1) / 2, 0);
		for (size_t i = 0; i < indices.size(); i++) words[i >> 1] |= indices[i] << ((i & 1) * 16);
	}
	const auto vertex_of = [&](uint32_t index) { return index + static_cast<uint32_t>(c.vertex_offset); };

	uint32_t lo = UINT32_MAX, hi = 0;
	for (size_t i = c.first_index; i < indices.size(); i++) {
		lo = std::min(lo, vertex_of(indices[i]));
		hi = std::max(hi, vertex_of(indices[i]) + 1);
	}
	PvVsStoreRequest request;
	request.enabled = true;
	request.triangle_list = true;
	request.indexed = true;
	request.index_size = c.index32 ? 4 : 2;
	request.range_known = true;
	request.range_first = lo;
	request.range_end = hi;
	request.index_count = c.prims * 3;
	request.instance_count = c.instances;
	request.record_stride_vec4 = stride_dummy;
	const auto plan = PlanPvVsStore(request);
	if (!plan.Use()) return 0xffffffffu;

	PvVsStoreDraw draw;
	draw.base = ring_base;
	draw.span = plan.span;
	draw.first = plan.first;
	draw.first_instance = c.first_instance;
	draw.first_index = c.first_index;
	draw.vertex_offset = c.vertex_offset;
	draw.index32 = c.index32;

	std::vector<Vec4> cap(ring_base + plan.records, Vec4 {{0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu}});
	// VS side: every (instance, vertex) the pipeline invokes writes its record.
	for (uint32_t inst = 0; inst < c.instances; inst++) {
		for (size_t i = c.first_index; i < indices.size(); i++) {
			const uint32_t v = vertex_of(indices[i]);
			const uint32_t record = PvVsStoreVertexRecord(draw.base, draw.span, draw.first, draw.first_instance,
			                                              draw.first_instance + inst, v);
			if (record >= cap.size()) return 0xffffffffu; // a store outside the region
			cap[record] = SourceVertex(v, draw.first_instance + inst);
		}
	}
	// PS side.
	uint32_t bad = 0;
	for (uint32_t inst = 0; inst < c.instances; inst++) {
		for (uint32_t prim = 0; prim < c.prims; prim++) {
			for (uint32_t k = 0; k < kPvVsStoreCorners; k++) {
				const uint32_t expected_vertex = vertex_of(indices[c.first_index + prim * 3 + k]);
				const uint32_t record = PvVsStoreCornerRecord(draw, words, inst, prim, k);
				if (record >= cap.size() || !(cap[record] == SourceVertex(expected_vertex, draw.first_instance + inst))) bad++;
			}
		}
	}
	(void)vertex_count;
	return bad;
}

void TestModel() {
	const Case cases[] = {
	    {"idx32 plain", true, 0, 0, 1, 0, 9},
	    {"idx16 plain", false, 0, 0, 1, 0, 9},
	    {"idx32 first_index", true, 6, 0, 1, 0, 8},
	    {"idx16 first_index odd", false, 3, 0, 1, 0, 8},
	    {"idx16 first_index even", false, 6, 0, 1, 0, 8},
	    {"idx32 vertex_offset", true, 0, 5, 1, 0, 9},
	    {"idx16 vertex_offset", false, 0, 12, 1, 0, 9},
	    {"idx32 2 instances", true, 0, 0, 2, 0, 9},
	    {"idx16 2 instances first_instance", false, 0, 0, 2, 3, 9},
	    {"idx32 everything", true, 9, 7, 2, 5, 11},
	    {"idx16 everything", false, 5, 2, 2, 1, 11},
	};
	for (const auto& c: cases) {
		for (const uint32_t base: {0u, 1000u}) {
			const auto bad = RunCase(c, base);
			if (bad != 0) {
				std::fprintf(stderr, "FAIL: model case '%s' base %u: %u wrong corner reads\n", c.name, base, bad);
				g_failures++;
			}
		}
	}
}

PvVsStoreRequest GoodRequest() {
	PvVsStoreRequest r;
	r.enabled = true;
	r.triangle_list = true;
	r.indexed = true;
	r.index_size = 2;
	r.range_known = true;
	r.range_first = 10;
	r.range_end = 50;
	r.index_count = 30;
	r.instance_count = 2;
	r.record_stride_vec4 = 3;
	return r;
}

void TestPlan() {
	using V = PvVsStoreVerdict;
	auto r = GoodRequest();
	auto plan = PlanPvVsStore(r);
	Check(plan.Use() && plan.first == 10 && plan.span == 40 && plan.records == 80, "plan of a plain draw");
	r.enabled = false;
	Check(PlanPvVsStore(r).verdict == V::Disabled, "disabled switch");
	r = GoodRequest();
	r.triangle_list = false;
	Check(PlanPvVsStore(r).verdict == V::NotTriangleList, "rect list stays on PerVertexKHR");
	r = GoodRequest();
	r.indexed = false;
	Check(PlanPvVsStore(r).verdict == V::NotIndexed, "non-indexed stays on PerVertexKHR");
	r = GoodRequest();
	r.primitive_restart = true;
	Check(PlanPvVsStore(r).verdict == V::PrimitiveRestart, "restart stays on PerVertexKHR");
	r = GoodRequest();
	r.index_size = 1;
	Check(PlanPvVsStore(r).verdict == V::BadIndexSize, "8-bit index buffer rejected");
	r = GoodRequest();
	r.range_known = false;
	Check(PlanPvVsStore(r).verdict == V::NoRange, "unknown range rejected");
	r = GoodRequest();
	r.range_dropped = true;
	Check(PlanPvVsStore(r).verdict == V::Dropped, "dropped corner rejected");
	r = GoodRequest();
	r.index_count = 2;
	Check(PlanPvVsStore(r).verdict == V::Empty, "less than one triangle");
	r = GoodRequest();
	r.range_end = 0x100000000ull;
	Check(PlanPvVsStore(r).verdict == V::TooLarge, "32-bit overflow rejected");
	// records * stride * 4 wraps around 2^64 to 0: the record count itself has to be bounded
	r = GoodRequest();
	r.range_first = 0;
	r.range_end = 0x40000000ull;
	r.instance_count = 0x40000000u;
	r.record_stride_vec4 = 4;
	Check(PlanPvVsStore(r).verdict == V::TooLarge, "record count wrapping the 64-bit product accepted");
	// no stride, nothing to store (the product is 0 whatever the draw size)
	r = GoodRequest();
	r.record_stride_vec4 = 0;
	Check(PlanPvVsStore(r).verdict == V::Empty, "zero record stride accepted");
}

void TestRing() {
	PvVsStoreRing ring(100);
	uint32_t a = 0, b = 0, c = 0;
	Check(ring.Allocate(40, a) && a == 0, "first region at 0");
	Check(ring.Allocate(40, b) && b == 40, "second region does not overlap the first");
	Check(!ring.Allocate(40, c), "full ring refuses");
	Check(ring.Allocate(20, c) && c == 80, "exact fit");
	Check(ring.Used() == 100 && ring.Draws() == 3, "ring counters");
	ring.EndPass();
	Check(ring.Allocate(40, a) && a == 0, "ring restarts after the pass");
	uint32_t zero = 7;
	Check(!ring.Allocate(0, zero), "empty region refused");
}

void TestSlots() {
	const uint32_t locations[] = {7, 2, 7, 30};
	const auto     slots       = PvVsStoreSlots(locations);
	Check(slots.size() == 3 && slots.at(2) == 0 && slots.at(7) == 1 && slots.at(30) == 2, "dense slots by location");
}

// interface of a VS/PS pair, push block, index binding, slab pool.
void TestInterface() {
	using V = PvVsStoreInterfaceVerdict;
	Check(PvVsStoreStrideFor(0) == 0 && PvVsStoreStrideFor(1) == 1 && PvVsStoreStrideFor(0b10100) == 5 &&
	          PvVsStoreStrideFor(1u << 30) == 31,
	      "stride is the highest written location + 1");
	Check(ClassifyPvVsStoreInterface(0b111, 0b101) == V::Use, "PS subset of the VS outputs");
	Check(ClassifyPvVsStoreInterface(0b111, 0b1000) == V::MissingOutput, "PS reads a location the VS lacks");
	Check(ClassifyPvVsStoreInterface(0b111, 0) == V::NoPerVertex, "no per-vertex input");
	Check(ClassifyPvVsStoreInterface(1u << 31 | 1u, 1u) == V::Param31, "VS writes location 31");
	Check(ClassifyPvVsStoreInterface(0b11, 1u << 31) == V::Param31, "PS reads location 31");
}

void TestPushBlock() {
	PvVsStoreDraw draw;
	draw.base = 11; draw.span = 22; draw.first = 33; draw.first_instance = 44;
	draw.first_index = 55; draw.vertex_offset = -66; draw.index32 = false;
	const auto block = PvVsStorePushBlock(draw, 7);
	Check(block[kPvPushBase] == 11 && block[kPvPushSpan] == 22 && block[kPvPushFirst] == 33 &&
	          block[kPvPushFirstInstance] == 44 && block[kPvPushFirstIndex] == 55 &&
	          block[kPvPushVertexOffset] == static_cast<uint32_t>(-66) && block[kPvPushIndex32] == 0 &&
	          block[kPvPushStride] == 7,
	      "push block dwords");
	draw.index32 = true;
	Check(PvVsStorePushBlock(draw, 7)[kPvPushIndex32] == 1, "32-bit flag");
}

void TestIndexBinding() {
	auto b = PvVsStoreBindIndex(256, 2, 16);
	Check(b.ok && b.offset == 256 && b.first_index == 0, "aligned offset needs no adjustment");
	b = PvVsStoreBindIndex(270, 2, 16);
	Check(b.ok && b.offset == 256 && b.first_index == 7, "16-bit: 14 bytes = 7 elements");
	b = PvVsStoreBindIndex(268, 4, 16);
	Check(b.ok && b.offset == 256 && b.first_index == 3, "32-bit: 12 bytes = 3 elements");
	Check(!PvVsStoreBindIndex(257, 2, 16).ok, "misaligned to the element size");
	Check(!PvVsStoreBindIndex(256, 1, 16).ok && !PvVsStoreBindIndex(256, 2, 12).ok, "bad size / alignment");
}

void TestIndexRange() {
	Check(PvVsStoreIndexRange(0, 2, 24) == 24, "aligned 16-bit range");
	Check(PvVsStoreIndexRange(1, 2, 24) == 28, "16-bit odd start: 26 bytes rounded to a whole word");
	Check(PvVsStoreIndexRange(3, 2, 18) == 24, "16-bit: 24 bytes");
	Check(PvVsStoreIndexRange(0, 2, 6) == 8, "16-bit, 3 indices");
	Check(PvVsStoreIndexRange(3, 4, 12) == 24, "32-bit unchanged");
}

// The index descriptor stays inside the buffer. The test mutants (KYTY_PVVS_TEST_MUTANT_*) are seams
// of this file only: they call the production functions with the check switched off.
#if defined(KYTY_PVVS_TEST_MUTANT_NO_CLAMP)
constexpr bool kClamp = false;
#else
constexpr bool kClamp = true;
#endif
#if defined(KYTY_PVVS_TEST_MUTANT_NO_TAIL_CHECK)
constexpr bool kCheckTail = false;
#else
constexpr bool kCheckTail = true;
#endif

void TestIndexDescriptor() {
	// 16-bit, 5 indices (10 bytes) at the very end of a 10-byte buffer: the last word (bytes 8..11) does not fit
	auto d = PvVsStoreIndexDescriptorFor(0, 2, 16, 10, 10, kClamp);
	Check(d.ok && !d.tail_fits, "16-bit odd end at the buffer end: the last word does not fit");
	Check(d.offset + d.range <= 10, "descriptor range never passes the buffer end");
	// the same draw with a 12-byte buffer fits and is rounded to a whole word
	d = PvVsStoreIndexDescriptorFor(0, 2, 16, 10, 12, kClamp);
	Check(d.ok && d.tail_fits && d.range == 12 && d.offset + d.range <= 12, "word-padded buffer: range 12");
	// aligned-down offset: byte 36 of a 64-byte buffer, 24 bytes, alignment 16 -> offset 32, first_index 1 (32-bit)
	d = PvVsStoreIndexDescriptorFor(36, 4, 16, 24, 64, kClamp);
	Check(d.ok && d.tail_fits && d.offset == 32 && d.first_index == 1 && d.range == 28, "aligned-down offset keeps the delta in the range");
	// the delta moves the end: 36..60 inside 60 bytes would need offset 32 + 28 = 60 (fits), a 59-byte buffer does not
	d = PvVsStoreIndexDescriptorFor(36, 4, 16, 24, 59, kClamp);
	Check(d.ok && !d.tail_fits && d.offset + d.range <= 59, "range reaching past the buffer is clamped and reported");
	// a buffer smaller than the offset
	d = PvVsStoreIndexDescriptorFor(32, 4, 16, 8, 16, kClamp);
	Check(d.ok && !d.tail_fits && d.range == 0, "buffer ends before the aligned offset");
	// 32-bit, exact fit
	d = PvVsStoreIndexDescriptorFor(0, 4, 16, 12, 12, kClamp);
	Check(d.ok && d.tail_fits && d.range == 12, "32-bit exact fit");
	Check(!PvVsStoreIndexDescriptorFor(0, 1, 16, 12, 12, kClamp).ok, "bad element size");
}

void TestIndexTailDecision() {
	const auto place_ok = [](const PvVsStorePlan&) { return true; };
	PvVsStoreRequest r;
	r.enabled = r.triangle_list = r.indexed = r.range_known = true;
	r.index_size = 2; r.range_first = 0; r.range_end = 12; r.index_count = 12; r.instance_count = 1; r.record_stride_vec4 = 2;
	auto d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, r, true, place_ok, false, kCheckTail);
	Check(!d.use && d.fallback == PvVsStoreFallback::IndexTail, "index tail that does not fit stays on PerVertexKHR (pv_fallback_index_tail)");
	int asked = 0;
	(void)DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, r, true, [&](const PvVsStorePlan&) { asked++; return true; }, false, kCheckTail);
	Check(asked == 0, "no ring region is reserved for a draw rejected by the index tail");
	d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, r, true, place_ok, true, kCheckTail);
	Check(d.use, "a fitting tail is taken");
}

void TestSlabs() {
	const auto never = [](uint64_t) { return false; };
	const auto done  = [](uint64_t) { return true; };
	PvVsStoreSlabs slabs(100, 2);
	auto a = slabs.Place(60, 5, never);
	Check(a.ok && a.created && a.slab == 0 && a.base == 0, "first region creates slab 0");
	auto b = slabs.Place(30, 5, never);
	Check(b.ok && !b.created && b.slab == 0 && b.base == 60, "same tick shares the slab, regions do not overlap");
	auto c = slabs.Place(60, 5, never);
	Check(c.ok && c.created && c.slab == 1 && c.base == 0, "a full slab moves to a new one");
	auto d = slabs.Place(60, 5, never);
	Check(!d.ok, "no free slab: the caller falls back");
	auto e = slabs.Place(10, 6, never);
	Check(!e.ok, "slabs of an unfinished tick are not recycled");
	auto f = slabs.Place(10, 7, done);
	Check(f.ok && !f.created && f.base == 0, "a finished tick recycles a slab from the start");
	Check(!slabs.Place(101, 8, done).ok, "region larger than a slab");
	Check(slabs.Size() == 2, "slab count capped");
	// A flush between Place() and the recording moves the draw to a newer tick.
	PvVsStoreSlabs touched(100, 1);
	auto t = touched.Place(10, 10, never);
	touched.Touch(t.slab, 11);
	const auto finished_10 = [](uint64_t tick) { return tick <= 10; };
	Check(!touched.Place(10, 12, finished_10).ok, "a slab touched in tick 11 is busy while only tick 10 finished");
	Check(touched.Place(10, 12, done).ok, "... and free once tick 11 has finished");
}

void TestFallbackClass() {
	using V = PvVsStoreVerdict;
	using F = PvVsStoreFallback;
	Check(ClassifyPvVsStoreFallback(V::Use, 1) == F::None && ClassifyPvVsStoreFallback(V::Disabled, 1) == F::None, "no fallback");
	Check(ClassifyPvVsStoreFallback(V::NotTriangleList, 1) == F::Topology &&
	          ClassifyPvVsStoreFallback(V::PrimitiveRestart, 3) == F::Topology,
	      "topology class");
	Check(ClassifyPvVsStoreFallback(V::NoRange, 1) == F::Range && ClassifyPvVsStoreFallback(V::TooLarge, 1) == F::Range,
	      "range class");
	Check(ClassifyPvVsStoreFallback(V::TooLarge, 4) == F::Instanced && ClassifyPvVsStoreFallback(V::RingFull, 2) == F::Instanced,
	      "instanced size failures");
	Check(ClassifyPvVsStoreFallback(V::RingFull, 1) == F::Ring, "ring class");
	Check(ClassifyPvVsStoreFallback(PvVsStoreInterfaceVerdict::Param31) == F::Param31 &&
	          ClassifyPvVsStoreFallback(PvVsStoreInterfaceVerdict::MissingOutput) == F::Interface,
	      "interface classes");
}

void TestPushRange() {
	Check(PvVsStorePushRangeBytes(128, false) == 128, "default pipelines keep the native push range");
	Check(PvVsStorePushRangeBytes(128, true) == 128 + kPvVsStorePushDwords * 4, "VS-store variants add the block");
}

void TestDecision() {
	using F = PvVsStoreFallback;
	const auto place_ok   = [](const PvVsStorePlan&) { return true; };
	const auto place_full = [](const PvVsStorePlan&) { return false; };
	const auto good       = [] {
        PvVsStoreRequest r;
        r.enabled = r.triangle_list = r.indexed = r.range_known = true;
        r.index_size = 2; r.range_first = 0; r.range_end = 12; r.index_count = 12; r.instance_count = 1; r.record_stride_vec4 = 2;
        return r;
	};
	auto d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, good(), true, place_ok);
	Check(d.use && d.fallback == F::None && d.plan.records == 12, "a plain draw is taken");
	d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Param31, good(), true, place_ok);
	Check(!d.use && d.fallback == F::Param31, "location 31 stays on PerVertexKHR");
	d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, good(), false, place_ok);
	Check(!d.use && d.fallback == F::Range, "an index buffer that cannot be bound stays on PerVertexKHR");
	d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, good(), true, place_full);
	Check(!d.use && d.fallback == F::Ring, "no ring room stays on PerVertexKHR");
	auto r = good();
	r.instance_count = 3;
	d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, r, true, place_full);
	Check(!d.use && d.fallback == F::Instanced, "instanced draw without ring room is counted as instanced");
	r = good();
	r.triangle_list = false;
	d = DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, r, true, place_ok);
	Check(!d.use && d.fallback == F::Topology, "rect list stays on PerVertexKHR");
	// the ring is only asked for a draw the plan accepts
	int asked = 0;
	r = good();
	r.primitive_restart = true;
	(void)DecidePvVsStore(PvVsStoreInterfaceVerdict::Use, r, true, [&](const PvVsStorePlan&) { asked++; return true; });
	Check(asked == 0, "no region is reserved for a rejected draw");
}

void TestCapability() {
	using S = PvVsStoreSwitch;
	Check(PvVsStoreEnabledFor(false, true, S::Auto), "MoltenVK without PerVertexKHR uses the VS store");
	Check(!PvVsStoreEnabledFor(true, true, S::Auto), "a device with fragmentShaderBarycentric keeps PerVertexKHR");
	Check(!PvVsStoreEnabledFor(false, false, S::Auto), "another driver is not switched");
	Check(!PvVsStoreEnabledFor(false, true, S::Off), "KYTY_PV_VSSTORE=0 turns it off");
	Check(PvVsStoreEnabledFor(true, false, S::On), "KYTY_PV_VSSTORE=1 forces it on");
	Check(ParsePvVsStoreSwitch(nullptr) == S::Auto, "unset is auto");
	Check(ParsePvVsStoreSwitch("") == S::Auto, "empty is auto");
	Check(ParsePvVsStoreSwitch("0") == S::Off, "0 is off");
	Check(ParsePvVsStoreSwitch("1") == S::On, "1 is on");
	Check(ParsePvVsStoreSwitch("10") == S::Auto, "10 is not a switch value");
	Check(ParsePvVsStoreSwitch("x") == S::Auto, "x is not a switch value");
}

void TestScanRange() {
	const uint16_t i16[] = {5, 7, 6, 7, 9, 8};
	auto           r     = ScanPvVsStoreVertexRange(i16, 2, 6, 0);
	Check(r.known && !r.dropped && r.first == 5 && r.end == 10, "16-bit range [5, 10)");
	r = ScanPvVsStoreVertexRange(i16, 2, 6, 100);
	Check(r.known && r.first == 105 && r.end == 110, "positive vertex_offset shifts the range");
	r = ScanPvVsStoreVertexRange(i16, 2, 6, -5);
	Check(r.known && !r.dropped && r.first == 0 && r.end == 5, "negative vertex_offset down to vertex 0");
	r = ScanPvVsStoreVertexRange(i16, 2, 6, -6);
	Check(r.known && r.dropped && r.first == 0 && r.end == 4, "a corner on vertex -1 is dropped, the rest is the range");
	// 32-bit indices read from an odd address (the guest does not align them).
	uint8_t raw[1 + 3 * 4] = {};
	const uint32_t i32[]   = {30, 12, 21};
	std::memcpy(raw + 1, i32, sizeof(i32));
	r = ScanPvVsStoreVertexRange(raw + 1, 4, 3, 0);
	Check(r.known && r.first == 12 && r.end == 31, "32-bit range from an unaligned source");
	r = ScanPvVsStoreVertexRange(i16, 2, 0, 0);
	Check(r.known && !r.dropped && r.first == r.end, "no indices, no vertex");
	const uint32_t big[] = {0x7fffffffu};
	r = ScanPvVsStoreVertexRange(big, 4, 1, 1);
	Check(!r.known, "a vertex number past 32-bit signed math is not derivable");
	r = ScanPvVsStoreVertexRange(big, 4, 1, 0);
	Check(r.known && r.first == 0x7fffffffu && r.end == 0x80000000ull, "the largest representable vertex");
	Check(!ScanPvVsStoreVertexRange(nullptr, 2, 1, 0).known, "no data");
	Check(!ScanPvVsStoreVertexRange(i16, 1, 1, 0).known, "1-byte indices are widened before this point");
}

} // namespace

int main() {
	TestModel();
	TestPlan();
	TestRing();
	TestSlots();
	TestInterface();
	TestPushBlock();
	TestIndexBinding();
	TestIndexRange();
	TestIndexDescriptor();
	TestIndexTailDecision();
	TestSlabs();
	TestFallbackClass();
	TestDecision();
	TestPushRange();
	TestCapability();
	TestScanRange();
	if (g_failures != 0) {
		std::fprintf(stderr, "per_vertex_vsstore_tests: %d failure(s)\n", g_failures);
		return EXIT_FAILURE;
	}
	std::printf("per_vertex_vsstore_tests: PASS\n");
	return EXIT_SUCCESS;
}
