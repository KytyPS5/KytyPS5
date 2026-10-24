#include "graphics/guest_gpu/command_processor/pm4Ownership.h"

#include "graphics/guest_gpu/pm4.h"

#include <cinttypes>
#include <cstdio>
#include <utility>

namespace Libs::Graphics {

namespace {

// The AGC register-selector bits that NormalizeRegisterOffset (pm4Handlers.cpp) strips.
constexpr uint32_t kRegisterSelectorMask = 0x70000000u;

} // namespace

void Pm4Ownership::Capture(std::span<const uint32_t> root) {
	if (root.empty()) {
		return;
	}
	Walk(root, 0);
}

std::span<const uint32_t> Pm4Ownership::Find(uint64_t address, uint32_t num_dw) const {
	if (num_dw == 0) {
		return {};
	}
	auto it = m_index.find(address);
	if (it == m_index.end()) {
		return {};
	}
	const auto& data = m_storage[it->second];
	if (data.size() < num_dw) {
		return {};
	}
	return std::span<const uint32_t>(data.data(), num_dw);
}

void Pm4Ownership::RecordMiss(uint64_t address, uint32_t num_dw, uint32_t header,
                              uint32_t offset_dw, uint32_t depth, Pm4CaptureMissReason reason) {
	if (m_miss_records.size() >= kMaxMissRecords) {
		return;
	}
	m_miss_records.push_back(Pm4CaptureMiss {address, num_dw, header, offset_dw, depth, reason});
}

std::string Pm4Ownership::DescribeMisses() const {
	if (m_misses == 0u && m_malformed == 0u) {
		return {};
	}

	const auto reason_name = [](Pm4CaptureMissReason reason) {
		switch (reason) {
			case Pm4CaptureMissReason::NoReader: return "no-reader";
			case Pm4CaptureMissReason::ReadFailed: return "read-failed";
			case Pm4CaptureMissReason::TooManyRanges: return "too-many-ranges";
			default: return "unknown";
		}
	};

	std::string result;
	char        buffer[256];

	snprintf(buffer, sizeof(buffer), "misses=%" PRIu32 " malformed=%" PRIu32 " recorded=%zu",
	         m_misses, m_malformed, m_miss_records.size());
	result += buffer;

	for (size_t i = 0; i < m_miss_records.size(); i++) {
		const auto& miss = m_miss_records[i];
		snprintf(buffer, sizeof(buffer),
		         " [%zu] addr=0x%016" PRIx64 " dw=%" PRIu32 " hdr=0x%08" PRIx32 " off=%" PRIu32
		         " depth=%" PRIu32 " reason=%s",
		         i, miss.address, miss.num_dw, miss.header, miss.offset_dw, miss.depth,
		         reason_name(miss.reason));
		result += buffer;
	}

	return result;
}

size_t Pm4Ownership::CaptureRange(uint64_t address, uint32_t num_dw, uint32_t header,
                                  uint32_t offset_dw, uint32_t depth) {
	if (address == 0 || num_dw == 0) {
		return kNoRange;
	}

	auto it = m_index.find(address);
	if (it != m_index.end() && m_storage[it->second].size() >= num_dw) {
		return it->second;
	}
	if (m_index.size() >= kMaxCapturedRanges) {
		m_malformed++;
		RecordMiss(address, num_dw, header, offset_dw, depth, Pm4CaptureMissReason::TooManyRanges);
		return it != m_index.end() ? it->second : kNoRange;
	}
	if (!m_reader) {
		m_misses++;
		RecordMiss(address, num_dw, header, offset_dw, depth, Pm4CaptureMissReason::NoReader);
		return it != m_index.end() ? it->second : kNoRange;
	}

	std::vector<uint32_t> data(num_dw);
	if (!m_reader(address, data.data(), static_cast<uint64_t>(num_dw) * sizeof(uint32_t))) {
		m_misses++;
		RecordMiss(address, num_dw, header, offset_dw, depth, Pm4CaptureMissReason::ReadFailed);
		return it != m_index.end() ? it->second : kNoRange;
	}

	const size_t index = m_storage.size();
	m_storage.emplace_back(std::move(data));
	if (it != m_index.end()) {
		it->second = index;
	} else {
		m_index.emplace(address, index);
	}
	return index;
}

size_t Pm4Ownership::CaptureOptionalRange(uint64_t address, uint32_t num_dw) {
	if (address == 0 || num_dw == 0) {
		return kNoRange;
	}

	auto it = m_index.find(address);
	if (it != m_index.end() && m_storage[it->second].size() >= num_dw) {
		return it->second;
	}
	// Speculative: unlike CaptureRange, an unreadable candidate or an exhausted budget is not an
	// error and is not recorded. A user-data slot may hold a non-pointer, so a miss here would be
	// a false positive that inflates the miss count the caller treats as failures.
	if (m_index.size() >= kMaxCapturedRanges || m_optional_ranges >= kMaxOptionalRanges ||
	    !m_reader) {
		return it != m_index.end() ? it->second : kNoRange;
	}

	std::vector<uint32_t> data(num_dw);
	if (!m_reader(address, data.data(), static_cast<uint64_t>(num_dw) * sizeof(uint32_t))) {
		return it != m_index.end() ? it->second : kNoRange;
	}

	const size_t index = m_storage.size();
	m_storage.emplace_back(std::move(data));
	if (it != m_index.end()) {
		it->second = index;
	} else {
		m_index.emplace(address, index);
	}
	m_optional_ranges++;
	return index;
}

void Pm4Ownership::CaptureUserDataTables(const uint32_t* buf, uint32_t payload_dw) {
	// The two user-data windows whose 64-bit slots the vertex-input path dereferences
	// (ShaderGetStaticVertexInputInfo reads the V# buffer table and the attribute table from the
	// GS or HS user SGPRs). A window is [first, first + 31].
	static constexpr uint32_t kWindows[][2] = {
	    {Pm4::SPI_SHADER_USER_DATA_GS_0, Pm4::SPI_SHADER_USER_DATA_GS_0 + 31u},
	    {Pm4::SPI_SHADER_USER_DATA_HS_0, Pm4::SPI_SHADER_USER_DATA_HS_0 + 31u},
	};

	// `buf` is the packet's first written register; `buf[i]` is register `reg + i`.
	if (payload_dw == 0u) {
		return;
	}
	const uint32_t reg = buf[0];

	for (const auto& window : kWindows) {
		const uint32_t window_first = window[0];
		const uint32_t window_last  = window[1];

		// Slots this packet writes, clipped to the window. slot_first is the first even slot
		// (64-bit user SGPRs are even-aligned).
		uint32_t slot_first = reg > window_first ? reg : window_first;
		uint32_t slot_last  = reg + payload_dw - 1u;
		if (slot_last > window_last) {
			slot_last = window_last;
		}
		if (slot_first & 1u) {
			slot_first++;
		}

		for (uint32_t slot = slot_first; slot + 1u <= slot_last; slot += 2u) {
			// buf[0] is the first written register offset; the value for register `r` is
			// buf[1 + (r - reg)], matching HwShSetGsUserSgpr/HwShSetHsUserSgpr.
			const uint32_t* pair = buf + 1u + (slot - reg);
			const uint64_t  address =
			    static_cast<uint64_t>(pair[0]) | (static_cast<uint64_t>(pair[1]) << 32u);
			(void)CaptureOptionalRange(address, kUserDataTableDwords);
		}
	}
}

void Pm4Ownership::WalkNested(uint64_t address, uint32_t num_dw, uint32_t header, uint32_t offset_dw,
                              uint32_t depth) {
	if (num_dw == 0) {
		return;
	}
	if (address == 0) {
		m_malformed++;
		return;
	}
	const auto index = CaptureRange(address, num_dw, header, offset_dw, depth);
	if (index == kNoRange) {
		// The nested stream is not mapped (or not readable). The deferred parse cannot read it
		// either, so there is nothing more to capture here; the miss has been recorded for
		// diagnostics.
		return;
	}
	Walk(m_storage[index], depth + 1u);
}

// The parser advances a packet by "handler return + 1" dwords. For almost every handler that
// equals the packet's declared header length, so a linear decode by Pm4::PacketSizeDw stays in
// step. Two wrapper forms diverge and are mirrored in Walk below:
//
//  * IT_SET_SH_REG whose first register is SH_NOP, and IT_SET_CONTEXT_REG whose first
//    (normalized) register is the fake CX_NOP: CpOpSetShaderReg/CpOpSetContextReg return 2 as soon
//    as they see it, so the parser advances three dwords no matter what the declared length says.
//
//  * The AA-sample-location context packet (0xc0106900) is where the old code carried a special
//    case; it is not needed. HwCtxSetAaSampleControl consumes an embedded 4-dword sub-packet that
//    begins at packet[18] (handler buffer[16]), so the parser advances 22 dwords. A plain linear
//    decode reaches exactly the same offset - 18 for the outer packet plus 4 for the embedded
//    sub-packet, which is itself well formed - so no override is required. (The old case tested
//    packet[17]/packet[18] and advanced 21, one dword off in both detection and advance.)
void Pm4Ownership::Walk(std::span<const uint32_t> stream, uint32_t depth) {
	if (depth > kMaxWalkDepth) {
		m_malformed++;
		return;
	}

	const auto size = static_cast<uint32_t>(stream.size());
	uint32_t   offset = 0;

	while (offset < size) {
		const uint32_t header = stream[offset];

		// Mirrors CommandProcessor::ProcessPm4: a bare 0x80000000 and the special size-1 header are
		// skipped one dword at a time without being treated as packets.
		if (header == 0x80000000u || Pm4::PacketSizeDw(header) == 1u) {
			offset++;
			continue;
		}

		// ProcessPm4 requires at least two remaining dwords for a real packet.
		if (offset + 1u >= size) {
			m_malformed++;
			break;
		}

		const uint32_t len = Pm4::PacketSizeDw(header);
		if (len < 2u || len > size - offset) {
			// The deferred parse would reject this packet; stop rather than wander.
			m_malformed++;
			break;
		}

		const uint32_t  opcode = (header >> 8u) & 0xffu;
		const uint32_t* buf    = stream.data() + offset + 1u;

		switch (opcode) {
			case Pm4::IT_SET_SH_REG: {
				// Snapshot the vertex-input descriptor tables the draw path reaches through the
				// GS/HS user-data registers. `buf[0]` is the first written register offset; the
				// remaining `len - 2` dwords are its values. The SH_NOP wrapper (buf[0] ==
				// SH_NOP) is outside both user-data windows, so CaptureUserDataTables ignores it.
				CaptureUserDataTables(buf, len - 2u);
				break;
			}
			case Pm4::IT_SET_CONTEXT_REG_INDIRECT:
			case Pm4::IT_SET_SH_REG_INDIRECT:
			case Pm4::IT_SET_UCONFIG_REG_INDIRECT: {
				// The handlers only read the pair array when the packet is exactly 5 dwords.
				if (len == 5u) {
					const auto address = (static_cast<uint64_t>(buf[0]) & 0xfffffffcu) |
					                     (static_cast<uint64_t>(buf[1]) << 32u);
					const uint32_t num_pairs = buf[3] & 0x3fffu;
					if (num_pairs != 0u) {
						(void)CaptureRange(address, num_pairs * 2u, header, offset, depth);
					}
				}
				break;
			}
			case Pm4::IT_INDIRECT_BUFFER: {
				if (len == 14u) {
					// Predicated branch: the parser picks then/else, we cannot know which (and must
					// not guess), so own both.
					const auto then_address = (static_cast<uint64_t>(buf[7]) & 0xfffffffcu) |
					                          (static_cast<uint64_t>(buf[8]) << 32u);
					const uint32_t then_num_dw = buf[9] & 0xfffffu;
					const auto     else_address = (static_cast<uint64_t>(buf[10]) & 0xfffffffcu) |
					                              (static_cast<uint64_t>(buf[11]) << 32u);
					const uint32_t else_num_dw = buf[12] & 0xfffffu;
					WalkNested(then_address, then_num_dw, header, offset, depth);
					if (else_num_dw != 0u) {
						WalkNested(else_address, else_num_dw, header, offset, depth);
					}
				} else if (len == 4u) {
					const auto address = static_cast<uint64_t>(buf[0]) |
					                     (static_cast<uint64_t>(buf[1]) << 32u);
					const uint32_t num_dw = buf[2] & 0xfffffu;
					WalkNested(address, num_dw, header, offset, depth);
				}
				break;
			}
			default: break;
		}

		// Advance by the declared length, except for the two NOP-wrapper forms where the parser's
		// handler advances three dwords regardless of it. A wrong advance here would desynchronise
		// the walk from the parser and make it skip later references without owning them.
		uint32_t advance = len;
		if (opcode == Pm4::IT_SET_SH_REG && buf[0] == Pm4::SH_NOP) {
			// CpOpSetShaderReg returns 2 as soon as the first register is SH_NOP.
			advance = 3u;
		} else if (opcode == Pm4::IT_SET_CONTEXT_REG &&
		           (buf[0] & ~kRegisterSelectorMask) == Pm4::CX_NOP) {
			// CpOpSetContextReg returns 2 when the first register normalizes to the fake CX_NOP.
			advance = 3u;
		}
		offset += advance;
	}
}

} // namespace Libs::Graphics
