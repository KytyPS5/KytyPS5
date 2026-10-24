// Regression tests for submit-time ownership of the guest memory that the PM4 parser dereferences
// after a submission has been queued: the IT_SET_*_REG_INDIRECT register/value pair arrays and
// nested indirect command buffers (IT_INDIRECT_BUFFER).
//
// The bug these cover: the parser used to read those regions straight out of guest memory on the
// GPU thread, long after the guest had rewritten them for a later frame, which produced torn
// register arrays and foreign commands. Kyty now snapshots every referenced region at submit time
// (Pm4Ownership) and parses the snapshot.

#include "graphics/guest_gpu/command_processor/pm4Ownership.h"
#include "graphics/guest_gpu/pm4.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace {

// The KYTY_PM4 macro references Pm4::R_NUM unqualified.
namespace Pm4 = Libs::Graphics::Pm4;

using Libs::Graphics::Pm4GuestReader;
using Libs::Graphics::Pm4Ownership;

// A tiny model of guest memory: a flat, mapped byte range at a fixed base address. The reader
// fails (like Kernel::TryReadBacking) for any range that leaves the mapped window, so the tests
// can exercise unmapped/stale references without touching real guest memory.
class FakeGuestMemory {
public:
	explicit FakeGuestMemory(uint64_t base, size_t bytes) : m_base(base), m_bytes(bytes, 0) {}

	[[nodiscard]] uint64_t Base() const { return m_base; }

	// Guest address of the `dword_index`-th dword in the mapped window.
	[[nodiscard]] uint64_t AddressOf(uint32_t dword_index) const {
		return m_base + static_cast<uint64_t>(dword_index) * sizeof(uint32_t);
	}

	// A guest address that is deliberately outside the mapped window.
	[[nodiscard]] uint64_t UnmappedAddress() const { return m_base + m_bytes.size() + 0x1000u; }

	void WriteDwords(uint64_t address, std::span<const uint32_t> values) {
		for (size_t i = 0; i < values.size(); i++) {
			WriteDword(address + i * sizeof(uint32_t), values[i]);
		}
	}

	void WriteDword(uint64_t address, uint32_t value) {
		CheckMapped(address, sizeof(uint32_t));
		const auto offset = static_cast<size_t>(address - m_base);
		m_bytes[offset + 0] = static_cast<uint8_t>(value & 0xffu);
		m_bytes[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
		m_bytes[offset + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
		m_bytes[offset + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
	}

	[[nodiscard]] std::vector<uint32_t> ReadDwords(uint64_t address, uint32_t count) const {
		std::vector<uint32_t> out(count);
		CheckMapped(address, static_cast<size_t>(count) * sizeof(uint32_t));
		for (uint32_t i = 0; i < count; i++) {
			const auto offset = static_cast<size_t>(address - m_base) + i * sizeof(uint32_t);
			out[i] = static_cast<uint32_t>(m_bytes[offset + 0]) |
			         (static_cast<uint32_t>(m_bytes[offset + 1]) << 8u) |
			         (static_cast<uint32_t>(m_bytes[offset + 2]) << 16u) |
			         (static_cast<uint32_t>(m_bytes[offset + 3]) << 24u);
		}
		return out;
	}

	[[nodiscard]] Pm4GuestReader Reader() const {
		return [this](uint64_t address, void* dst, uint64_t size) {
			if (size == 0 || dst == nullptr || address < m_base) {
				return false;
			}
			const auto offset = static_cast<uint64_t>(address - m_base);
			if (offset > m_bytes.size() || size > m_bytes.size() - offset) {
				return false;
			}
			for (uint64_t i = 0; i < size; i++) {
				static_cast<uint8_t*>(dst)[i] = m_bytes[static_cast<size_t>(offset + i)];
			}
			return true;
		};
	}

private:
	void CheckMapped(uint64_t address, size_t size) const {
		if (address < m_base) {
			std::fprintf(stderr, "Pm4OwnershipTests: fake guest access below base\n");
			std::abort();
		}
		const auto offset = static_cast<uint64_t>(address - m_base);
		if (offset > m_bytes.size() || size > m_bytes.size() - offset) {
			std::fprintf(stderr, "Pm4OwnershipTests: fake guest access past end\n");
			std::abort();
		}
	}

	uint64_t             m_base;
	std::vector<uint8_t> m_bytes;
};

constexpr uint32_t kR = 0u;

// Builds a single IT_SET_*_REG_INDIRECT packet (5 dwords) that points at `address` and declares
// `num_pairs` register/value pairs, followed by `pairs` (2 dwords per pair).
[[nodiscard]] std::vector<uint32_t> MakeRegisterArrayPacket(uint32_t opcode, uint64_t address,
                                                            uint32_t num_pairs,
                                                            std::span<const uint32_t> pairs) {
	std::vector<uint32_t> packet;
	packet.push_back(KYTY_PM4(5u, opcode, kR));
	packet.push_back(static_cast<uint32_t>(address & 0xffffffffu));
	packet.push_back(static_cast<uint32_t>(address >> 32u));
	packet.push_back(0u); // unused by the parser
	packet.push_back(num_pairs & 0x3fffu);
	packet.insert(packet.end(), pairs.begin(), pairs.end());
	return packet;
}

// Builds a 4-dword IT_INDIRECT_BUFFER packet pointing at `address` / `num_dw`.
[[nodiscard]] std::vector<uint32_t> MakeIndirectBufferPacket(uint64_t address, uint32_t num_dw,
                                                             bool chain) {
	std::vector<uint32_t> packet;
	packet.push_back(KYTY_PM4(4u, Libs::Graphics::Pm4::IT_INDIRECT_BUFFER, kR));
	packet.push_back(static_cast<uint32_t>(address & 0xffffffffu));
	packet.push_back(static_cast<uint32_t>(address >> 32u));
	packet.push_back((num_dw & 0xfffffu) | (chain ? (1u << 20u) : 0u));
	return packet;
}

// Builds a 14-dword predicated IT_INDIRECT_BUFFER branch packet. Field layout follows the parser's
// `buffer` view, which starts one dword after the header.
[[nodiscard]] std::vector<uint32_t> MakeBranchPacket(uint64_t compare_address, uint64_t then_address,
                                                     uint32_t then_num_dw, uint64_t else_address,
                                                     uint32_t else_num_dw) {
	std::vector<uint32_t> packet(14u, 0u);
	packet[0] = KYTY_PM4(14u, Libs::Graphics::Pm4::IT_INDIRECT_BUFFER, kR);
	packet[1] = 1u; // buffer[0]: mode = 1, function = 0
	packet[2] = static_cast<uint32_t>(compare_address & 0xfffffff8u); // buffer[1]
	packet[3] = static_cast<uint32_t>(compare_address >> 32u);         // buffer[2]
	packet[4] = 0xffffffffu;                                          // buffer[3]: mask
	packet[5] = 0xffffffffu;                                          // buffer[4]
	packet[6] = 0u;                                                   // buffer[5]: reference
	packet[7] = 0u;                                                   // buffer[6]
	packet[8] = static_cast<uint32_t>(then_address & 0xfffffffcu);    // buffer[7]
	packet[9] = static_cast<uint32_t>(then_address >> 32u);           // buffer[8]
	packet[10] = then_num_dw & 0xfffffu;                              // buffer[9]
	packet[11] = static_cast<uint32_t>(else_address & 0xfffffffcu);   // buffer[10]
	packet[12] = static_cast<uint32_t>(else_address >> 32u);          // buffer[11]
	packet[13] = else_num_dw & 0xfffffu;                              // buffer[12]
	return packet;
}

// Builds the AA-sample-location context packet (header 0xc0106900, declared length 18) whose first
// register is PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0. HwCtxSetAaSampleControl reads the packet through a
// buffer that starts two dwords after the header (the op handler passes its own buffer + 1 to the
// context handler), so the embedded SET_CONTEXT_REG sub-packet it looks for - header 0xc0026900
// followed by PA_SC_CENTROID_PRIORITY_0 - lives at packet[18]/packet[19], not at packet[17]/[18].
// When `with_embedded_centroid` is set the returned packet is 22 dwords long, which matches the
// parser's advance of 22 (18 for the outer packet plus the embedded sub-packet's own 4); without it
// the packet is exactly its declared 18 dwords and the handler would not take that branch.
[[nodiscard]] std::vector<uint32_t> MakeAaSampleControlPacket(bool with_embedded_centroid) {
	std::vector<uint32_t> packet(with_embedded_centroid ? 22u : 18u, 0u);
	packet[0] = 0xc0106900u; // IT_SET_CONTEXT_REG, 16 register/value pairs
	packet[1] = Libs::Graphics::Pm4::PA_SC_AA_SAMPLE_LOCS_PIXEL_X0Y0_0; // op handler buffer[0]
	for (uint32_t i = 2; i < 18u; i++) {
		packet[i] = 0x00010000u + i; // context handler buffer[0..15]: sample-location words
	}
	if (with_embedded_centroid) {
		packet[18] = 0xc0026900u;                                    // context handler buffer[16]
		packet[19] = Libs::Graphics::Pm4::PA_SC_CENTROID_PRIORITY_0; // context handler buffer[17]
		packet[20] = 0x00c0ffeeu;                                    // context handler buffer[18]
		packet[21] = 0x00000000u;                                    // context handler buffer[19]
	}
	return packet;
}

void Append(std::vector<uint32_t>& dst, const std::vector<uint32_t>& src) {
	dst.insert(dst.end(), src.begin(), src.end());
}

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "Pm4OwnershipTests: failed: %s\n", text);
		std::abort();
	}
}

void CheckSpanEquals(std::span<const uint32_t> actual, std::span<const uint32_t> expected,
                     const char* text) {
	if (actual.size() != expected.size()) {
		std::fprintf(stderr, "Pm4OwnershipTests: failed: %s: size %zu != %zu\n", text, actual.size(),
		             expected.size());
		std::abort();
	}
	for (size_t i = 0; i < actual.size(); i++) {
		if (actual[i] != expected[i]) {
			std::fprintf(stderr,
			             "Pm4OwnershipTests: failed: %s: [%zu] 0x%08X != 0x%08X\n", text, i, actual[i],
			             expected[i]);
			std::abort();
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Top-level stream ownership: the register array a root-stream packet points at is snapshotted
// and survives a later guest rewrite.
// ---------------------------------------------------------------------------------------------
void TestRegisterArraySnapshot() {
	FakeGuestMemory guest(0x100000000ull, 0x4000);

	const uint64_t array_address = guest.AddressOf(0x100);
	const std::vector<uint32_t> submitted_pairs = {0x00000001u, 0x11111111u, 0x00000002u,
	                                               0x22222222u, 0x00000003u, 0x33333333u};
	guest.WriteDwords(array_address, submitted_pairs);

	const auto root =
	    MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT, array_address, 3u,
	                            submitted_pairs);

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MissCount() == 0u, "register-array snapshot has no miss");
	Check(ownership.MalformedCount() == 0u, "register-array snapshot is well formed");

	// The guest rewrites the array for a later frame.
	const std::vector<uint32_t> rewritten_pairs = {0x00000009u, 0x99999999u, 0x0000000au,
	                                               0xaaaaaaaa};
	guest.WriteDwords(array_address, rewritten_pairs);

	CheckSpanEquals(ownership.Find(array_address, 6u), submitted_pairs,
	                "register array keeps submit-time contents");
	Check(ownership.Find(array_address, 7u).empty(), "over-long register array lookup misses");
	Check(ownership.Find(guest.UnmappedAddress(), 2u).empty(), "unmapped lookup misses");
}

void TestShAndUcRegisterArraySnapshot() {
	FakeGuestMemory guest(0x200000000ull, 0x4000);

	for (const auto opcode: {Libs::Graphics::Pm4::IT_SET_SH_REG_INDIRECT,
	                         Libs::Graphics::Pm4::IT_SET_UCONFIG_REG_INDIRECT}) {
		const uint64_t array_address     = guest.AddressOf(0x80);
		const std::vector<uint32_t> orig = {0x00000010u, 0x40000000u, 0x00000011u, 0x00000001u};
		guest.WriteDwords(array_address, orig);

		const auto root = MakeRegisterArrayPacket(opcode, array_address, 2u, orig);
		Pm4Ownership ownership(guest.Reader());
		ownership.Capture(root);

		const std::vector<uint32_t> rewritten = {0xdeadbeefu, 0xdeadbeefu, 0xdeadbeefu};
		guest.WriteDwords(array_address, rewritten);
		CheckSpanEquals(ownership.Find(array_address, 4u), orig,
		                "SH/UC register array keeps submit-time contents");
	}
}

// ---------------------------------------------------------------------------------------------
// Nested indirect buffer ownership, including a registered array referenced from inside it.
// ---------------------------------------------------------------------------------------------
void TestNestedIndirectBufferSnapshot() {
	FakeGuestMemory guest(0x300000000ull, 0x8000);

	const uint64_t nested_address = guest.AddressOf(0x200);
	const uint64_t array_address  = guest.AddressOf(0x400);
	const std::vector<uint32_t> array_orig = {0x00000020u, 0xabcdef01u};
	guest.WriteDwords(array_address, array_orig);

	auto nested = MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                      array_address, 1u, array_orig);
	guest.WriteDwords(nested_address, nested);

	auto root = MakeIndirectBufferPacket(nested_address, static_cast<uint32_t>(nested.size()), false);

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MissCount() == 0u, "nested snapshot has no miss");

	// Rewrite both the nested stream and the array it references.
	guest.WriteDwords(nested_address, std::vector<uint32_t>(nested.size(), 0xccccccccu));
	guest.WriteDwords(array_address, std::vector<uint32_t>(array_orig.size(), 0xddddddddu));

	CheckSpanEquals(ownership.Find(nested_address, static_cast<uint32_t>(nested.size())), nested,
	                "nested buffer keeps submit-time contents");
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "array referenced from nested buffer keeps submit-time contents");
}

void TestNestedRecursion() {
	FakeGuestMemory guest(0x400000000ull, 0x8000);

	const uint64_t outer_address = guest.AddressOf(0x100);
	const uint64_t inner_address = guest.AddressOf(0x800);
	const uint64_t array_address = guest.AddressOf(0x1000);

	const std::vector<uint32_t> array_orig = {0x00000030u, 0x0000beefu};
	guest.WriteDwords(array_address, array_orig);

	auto inner = MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_SH_REG_INDIRECT, array_address,
	                                     1u, array_orig);
	guest.WriteDwords(inner_address, inner);

	auto outer = MakeIndirectBufferPacket(inner_address, static_cast<uint32_t>(inner.size()), true);
	guest.WriteDwords(outer_address, outer);

	auto root = MakeIndirectBufferPacket(outer_address, static_cast<uint32_t>(outer.size()), true);

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);

	guest.WriteDwords(outer_address, std::vector<uint32_t>(outer.size(), 0u));
	guest.WriteDwords(inner_address, std::vector<uint32_t>(inner.size(), 0u));
	guest.WriteDwords(array_address, std::vector<uint32_t>(array_orig.size(), 0u));

	CheckSpanEquals(ownership.Find(outer_address, static_cast<uint32_t>(outer.size())), outer,
	                "outer nested buffer is owned");
	CheckSpanEquals(ownership.Find(inner_address, static_cast<uint32_t>(inner.size())), inner,
	                "inner nested buffer is owned");
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "array reached through two levels of nesting is owned");
}

// ---------------------------------------------------------------------------------------------
// Control flow: both targets of a predicated branch must be owned (the branch result is unknown
// at capture time), and an inline COND_EXEC body must be scanned.
// ---------------------------------------------------------------------------------------------
void TestBranchCapturesBothTargets() {
	FakeGuestMemory guest(0x500000000ull, 0x8000);

	const uint64_t compare_address = guest.AddressOf(0x40);
	const uint64_t then_address    = guest.AddressOf(0x200);
	const uint64_t else_address    = guest.AddressOf(0x400);
	const std::vector<uint32_t> compare_orig = {0u};
	guest.WriteDwords(compare_address, compare_orig);

	std::vector<uint32_t> then_stream = {0x12345678u};
	std::vector<uint32_t> else_stream = {0x87654321u};
	guest.WriteDwords(then_address, then_stream);
	guest.WriteDwords(else_address, else_stream);

	const auto root =
	    MakeBranchPacket(compare_address, then_address, static_cast<uint32_t>(then_stream.size()),
	                     else_address, static_cast<uint32_t>(else_stream.size()));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MissCount() == 0u, "branch capture has no miss");

	guest.WriteDwords(then_address, std::vector<uint32_t>{0u});
	guest.WriteDwords(else_address, std::vector<uint32_t>{0u});

	CheckSpanEquals(ownership.Find(then_address, 1u), then_stream, "branch then-buffer is owned");
	CheckSpanEquals(ownership.Find(else_address, 1u), else_stream, "branch else-buffer is owned");
}

void TestCondExecInlineBodyIsScanned() {
	FakeGuestMemory guest(0x600000000ull, 0x8000);

	// A COND_EXEC packet (6 dwords) followed by an inline body containing a register-array packet.
	// The capture walks straight through the body (a superset of both take and skip outcomes).
	std::vector<uint32_t> root;
	root.push_back(KYTY_PM4(6u, Libs::Graphics::Pm4::IT_COND_EXEC, kR));
	root.push_back(0u); // predicate address low
	root.push_back(0u); // predicate address high
	root.push_back(0u); // control
	root.push_back(4u); // exec_count = body length in dwords
	root.push_back(0u);

	const uint64_t array_address = guest.AddressOf(0x200);
	const std::vector<uint32_t> array_orig = {0x00000040u, 0x01020304u};
	guest.WriteDwords(array_address, array_orig);
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 1u, array_orig));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);

	guest.WriteDwords(array_address, std::vector<uint32_t>{0u, 0u});
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "array inside an inline COND_EXEC body is owned");
}

// ---------------------------------------------------------------------------------------------
// Multiple / overlapping references, including a longer capture of an already-owned range.
// ---------------------------------------------------------------------------------------------
void TestMultipleAndOverlappingReferences() {
	FakeGuestMemory guest(0x700000000ull, 0x8000);

	const uint64_t array_address = guest.AddressOf(0x200);
	std::vector<uint32_t> six = {0u, 1u, 2u, 3u, 4u, 5u};
	guest.WriteDwords(array_address, six);

	// First a short reference (2 pairs), then a longer one (3 pairs) to the same address.
	std::vector<uint32_t> root;
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 2u, std::vector<uint32_t>(4u)));
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_SH_REG_INDIRECT, array_address,
	                                     3u, std::vector<uint32_t>(6u)));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);

	guest.WriteDwords(array_address, std::vector<uint32_t>(six.size(), 0xffffffffu));
	CheckSpanEquals(ownership.Find(array_address, 6u), six,
	                "longer capture of a previously captured range keeps submit-time contents");
}

// ---------------------------------------------------------------------------------------------
// Bounds, malformed inputs, and failure behaviour: no crash, misses/malformed are counted, and a
// miss never silently yields submit-time-looking data.
// ---------------------------------------------------------------------------------------------
void TestUnmappedReferenceIsReported() {
	FakeGuestMemory guest(0x800000000ull, 0x1000);

	const auto root = MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                          guest.UnmappedAddress(), 2u,
	                                          std::vector<uint32_t>(4u));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MissCount() == 1u, "unmapped register array is counted as a miss");
	Check(ownership.Find(guest.UnmappedAddress(), 4u).empty(), "unmapped array lookup is empty");
}

void TestUnmappedNestedBufferIsReported() {
	FakeGuestMemory guest(0x900000000ull, 0x1000);

	const auto root = MakeIndirectBufferPacket(guest.UnmappedAddress(), 16u, true);

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MissCount() == 1u, "unmapped nested buffer is counted as a miss");
}

void TestMalformedPacketLengthStopsCleanly() {
	FakeGuestMemory guest(0xa00000000ull, 0x1000);

	const uint64_t array_address = guest.AddressOf(0x100);
	const std::vector<uint32_t> array_orig = {0x00000050u, 0x0badf00du};
	guest.WriteDwords(array_address, array_orig);

	std::vector<uint32_t> root;
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 1u, array_orig));
	// A packet that claims a length far longer than the remaining stream.
	root.push_back(KYTY_PM4(100u, Libs::Graphics::Pm4::IT_NOP, kR));
	root.push_back(0u);
	root.push_back(0u);

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);

	Check(ownership.MalformedCount() != 0u, "over-long packet is counted as malformed");
	guest.WriteDwords(array_address, std::vector<uint32_t>{0u, 0u});
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "a valid reference before a malformed packet is still owned");
}

void TestZeroRegisterCountCapturesNothing() {
	FakeGuestMemory guest(0xb00000000ull, 0x1000);

	const auto root = MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                          guest.AddressOf(0x10), 0u, {});
	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.RangeCount() == 0u, "zero-length register array captures nothing");
	Check(ownership.MissCount() == 0u, "zero-length register array is not a miss");
}

void TestSpecialHeaderFormsDoNotDesync() {
	FakeGuestMemory guest(0xc00000000ull, 0x1000);

	const uint64_t array_address = guest.AddressOf(0x100);
	const std::vector<uint32_t> array_orig = {0x00000060u, 0x00112233u};
	guest.WriteDwords(array_address, array_orig);

	std::vector<uint32_t> root;
	root.push_back(0x80000000u);                 // bare skip dword
	root.push_back(0x3fff1000u);                 // size-1 special header
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 1u, array_orig));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	guest.WriteDwords(array_address, std::vector<uint32_t>{0u, 0u});
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "references after special headers are still owned");
}

void TestAaSampleControlEmbeddedPacketKeepsAlignment() {
	FakeGuestMemory guest(0x1000000000ull, 0x4000);

	const uint64_t array_address = guest.AddressOf(0x200);
	const std::vector<uint32_t> array_orig = {0x00000070u, 0x0f0f0f0fu};
	guest.WriteDwords(array_address, array_orig);

	std::vector<uint32_t> root = MakeAaSampleControlPacket(true);
	// The reference-bearing packet begins at dword 22: the walk's linear decode advances 18 for the
	// outer packet and then 4 for the embedded sub-packet. Advancing only the declared 18, or the
	// old hard-coded 21, would fall out of step here and never reach the array packet.
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 1u, array_orig));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MalformedCount() == 0u, "AA-sample-control walk stays well formed");

	guest.WriteDwords(array_address, std::vector<uint32_t>{0u, 0u});
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "reference after an embedded AA-sample sub-packet is owned");
}

void TestAaSampleControlWithoutEmbeddedPatternDoesNotOverAdvance() {
	FakeGuestMemory guest(0x2000000000ull, 0x4000);

	const uint64_t array_address = guest.AddressOf(0x200);
	const std::vector<uint32_t> array_orig = {0x00000071u, 0x11112222u};
	guest.WriteDwords(array_address, array_orig);

	// A declared-length context packet with no embedded sub-packet: the walk must advance exactly 18
	// dwords, so a reference-bearing packet placed immediately after it is reached.
	std::vector<uint32_t> root = MakeAaSampleControlPacket(false);
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 1u, array_orig));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	guest.WriteDwords(array_address, std::vector<uint32_t>{0u, 0u});
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "reference after a declared-length context packet is owned");
}

// ---------------------------------------------------------------------------------------------
// Wrapper forms whose handler advance differs from the declared packet length.
// ---------------------------------------------------------------------------------------------
void TestSetShaderRegNopWrapperAdvancesThree() {
	FakeGuestMemory guest(0x1100000000ull, 0x4000);

	const uint64_t array_address = guest.AddressOf(0x200);
	const std::vector<uint32_t> array_orig = {0x00000080u, 0x55667788u};
	guest.WriteDwords(array_address, array_orig);

	// CpOpSetShaderReg returns 2 (advance 3) as soon as the first register is SH_NOP, regardless of
	// the declared length. The reference-bearing packet therefore begins at dword 3.
	std::vector<uint32_t> root;
	root.push_back(KYTY_PM4(6u, Libs::Graphics::Pm4::IT_SET_SH_REG, kR)); // declares 6 dwords
	root.push_back(Pm4::SH_NOP);                                         // first register
	root.push_back(0xdead0001u);                                         // inside the declared packet
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 1u, array_orig));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MalformedCount() == 0u, "SH_NOP wrapper walk stays well formed");

	guest.WriteDwords(array_address, std::vector<uint32_t>{0u, 0u});
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "reference after an SH_NOP wrapper packet is owned");
}

void TestSetContextRegNopWrapperAdvancesThree() {
	FakeGuestMemory guest(0x1200000000ull, 0x4000);

	const uint64_t array_address = guest.AddressOf(0x200);
	const std::vector<uint32_t> array_orig = {0x00000090u, 0x99aabbccu};
	guest.WriteDwords(array_address, array_orig);

	// CpOpSetContextReg returns 2 (advance 3) when the first register normalizes to the fake
	// CX_NOP. The AGC register-selector bits must be masked off, so use one of them here.
	std::vector<uint32_t> root;
	root.push_back(KYTY_PM4(6u, Libs::Graphics::Pm4::IT_SET_CONTEXT_REG, kR)); // declares 6 dwords
	root.push_back(Pm4::CX_NOP | 0x10000000u);                                // first register
	root.push_back(0xdead0002u);                                             // inside the packet
	Append(root, MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     array_address, 1u, array_orig));

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(root);
	Check(ownership.MalformedCount() == 0u, "CX_NOP wrapper walk stays well formed");

	guest.WriteDwords(array_address, std::vector<uint32_t>{0u, 0u});
	CheckSpanEquals(ownership.Find(array_address, 2u), array_orig,
	                "reference after a CX_NOP wrapper packet is owned");
}

// ---------------------------------------------------------------------------------------------
// Miss diagnostics: the capture records enough context to explain a "was not captured" failure.
// ---------------------------------------------------------------------------------------------
void TestDescribeMissesReportsReferenceContext() {
	FakeGuestMemory guest(0x1300000000ull, 0x1000);

	const auto root = MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                          guest.UnmappedAddress(), 2u,
	                                          std::vector<uint32_t>(4u));

	Pm4Ownership ownership(guest.Reader());
	Check(ownership.DescribeMisses().empty(), "no misses yields an empty description");
	ownership.Capture(root);

	const auto description = ownership.DescribeMisses();
	Check(!description.empty(), "a miss produces a description");
	Check(description.find("misses=1") != std::string::npos, "description counts the miss");
	Check(description.find("read-failed") != std::string::npos, "description names the reason");
	Check(description.find("hdr=0xc0039f00") != std::string::npos,
	      "description carries the referencing packet header");
	Check(description.find("off=0") != std::string::npos,
	      "description carries the referencing packet offset");
}

void TestEmptyStreamIsNoOp() {
	FakeGuestMemory guest(0xd00000000ull, 0x100);
	Pm4Ownership ownership(guest.Reader());
	ownership.Capture({});
	Check(ownership.Empty(), "empty stream captures nothing");
}

// ---------------------------------------------------------------------------------------------
// Lifetime / stability: a span handed out by Find stays valid and correct while further ranges are
// captured (the parser holds such spans across nested-stream processing).
// ---------------------------------------------------------------------------------------------
void TestSpanStabilityAcrossFurtherCaptures() {
	FakeGuestMemory guest(0xe00000000ull, 0x4000);

	const uint64_t first_address  = guest.AddressOf(0x100);
	const uint64_t second_address = guest.AddressOf(0x800);
	const std::vector<uint32_t> first_orig  = {0x11111111u, 0x22222222u};
	const std::vector<uint32_t> second_orig = {0x33333333u, 0x44444444u};
	guest.WriteDwords(first_address, first_orig);
	guest.WriteDwords(second_address, second_orig);

	Pm4Ownership ownership(guest.Reader());

	auto root1 = MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_CONTEXT_REG_INDIRECT,
	                                     first_address, 1u, first_orig);
	ownership.Capture(root1);
	const auto first_span = ownership.Find(first_address, 2u);
	CheckSpanEquals(first_span, first_orig, "first span before further captures");

	// Capture more ranges; the previously handed-out span must remain valid.
	auto root2 = MakeRegisterArrayPacket(Libs::Graphics::Pm4::IT_SET_SH_REG_INDIRECT, second_address,
	                                     1u, second_orig);
	ownership.Capture(root2);

	guest.WriteDwords(first_address, std::vector<uint32_t>{0xdeadbeefu, 0xdeadbeefu});
	CheckSpanEquals(first_span, first_orig, "first span remains valid after further captures");
	CheckSpanEquals(ownership.Find(second_address, 2u), second_orig,
	                "second span is captured correctly");
}

// ---------------------------------------------------------------------------------------------
// Depth guard: a chain of nested buffers deeper than the limit does not recurse without bound.
// ---------------------------------------------------------------------------------------------
void TestDepthLimitIsBounded() {
	FakeGuestMemory guest(0xf00000000ull, 0x20000);

	// Build a self-referential chain: each buffer is an IT_INDIRECT_BUFFER pointing at itself.
	const uint64_t self_address = guest.AddressOf(0x100);
	const auto     self_packet  = MakeIndirectBufferPacket(self_address, 4u, true);
	guest.WriteDwords(self_address, self_packet);

	Pm4Ownership ownership(guest.Reader());
	ownership.Capture(self_packet); // root also points at the same self-referential buffer
	Check(ownership.RangeCount() <= 2u, "self-referential nesting stays bounded");
	Check(ownership.MalformedCount() != 0u, "depth limit is reported as malformed");
}

} // namespace

int main() {
	TestRegisterArraySnapshot();
	TestShAndUcRegisterArraySnapshot();
	TestNestedIndirectBufferSnapshot();
	TestNestedRecursion();
	TestBranchCapturesBothTargets();
	TestCondExecInlineBodyIsScanned();
	TestMultipleAndOverlappingReferences();
	TestUnmappedReferenceIsReported();
	TestUnmappedNestedBufferIsReported();
	TestMalformedPacketLengthStopsCleanly();
	TestZeroRegisterCountCapturesNothing();
	TestSpecialHeaderFormsDoNotDesync();
	TestAaSampleControlEmbeddedPacketKeepsAlignment();
	TestAaSampleControlWithoutEmbeddedPatternDoesNotOverAdvance();
	TestSetShaderRegNopWrapperAdvancesThree();
	TestSetContextRegNopWrapperAdvancesThree();
	TestDescribeMissesReportsReferenceContext();
	TestEmptyStreamIsNoOp();
	TestSpanStabilityAcrossFurtherCaptures();
	TestDepthLimitIsBounded();

	std::printf("Pm4OwnershipTests: all checks passed\n");
	return 0;
}
