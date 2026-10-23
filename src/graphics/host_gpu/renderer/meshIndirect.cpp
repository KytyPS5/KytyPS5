#include "graphics/host_gpu/renderer/meshIndirect.h"

#include "common/assert.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace Libs::Graphics {

static_assert(MeshIndirectVerifyEntryDwords >=
              10 + ShaderRecompiler::IR::MeshTaskPayloadDwordCount);

bool MeshIndirectEntryAgrees(const MeshIndirectVerifyPacket& packet, uint32_t index,
                             std::span<const uint32_t> entry, bool& empty) {
	EXIT_IF(entry.size() < MeshIndirectVerifyEntryDwords);
	constexpr auto slots = ShaderRecompiler::IR::PushData::MeshDrawDwordCount;
	empty                = false;
	const auto drawn     = packet.counted ? std::min(entry[2], packet.max_count) : packet.max_count;
	const auto record    = entry.subspan(3, 5);
	const auto groups    = entry.subspan(8, 2);
	const auto payload   = entry.subspan(10, ShaderRecompiler::IR::MeshTaskPayloadDwordCount);
	std::array<uint32_t, 2>                                               expected_groups {};
	std::array<uint32_t, ShaderRecompiler::IR::MeshTaskPayloadDwordCount> expected_payload {};
	bool                                                                  record_ok = true;
	if (index < drawn) {
		const auto plan =
		    packet.indexed ? DeriveMeshIndexedRecord(packet.shape, {record[0], record[1], record[2],
		                                                            record[3], record[4]})
		                   : DeriveMeshIndirectRecord(packet.shape,
		                                              {record[0], record[1], record[2], record[3]});
		empty = plan.action != DispatchPlanAction::Record;
		if (!empty) {
			expected_groups = {plan.groups_x, plan.groups_y};
		}
		expected_payload[0] = record[0];
		if (packet.indexed) {
			const auto window = IndirectIndexWindow(packet.index_base, packet.index_element_size,
			                                        packet.index_buffer_size, record[2]);
			expected_payload[ShaderRecompiler::IR::MeshTaskPayloadIndexBound] = window.readable;
			expected_payload[3]         = packet.index_element_size;
			expected_payload[4]         = static_cast<uint32_t>(window.address);
			expected_payload[5]         = static_cast<uint32_t>(window.address >> 32u);
			expected_payload[slots]     = record[3];
			expected_payload[slots + 1] = record[4];
			expected_payload[slots + 2] = record[2];
		} else {
			expected_payload[slots]     = record[2];
			expected_payload[slots + 1] = record[3];
		}
		record_ok = packet.indexed || record[4] == 0;
	} else {
		record_ok = std::ranges::all_of(record, [](uint32_t word) { return word == 0; });
	}
	return entry[0] == index + 1u && entry[1] == drawn && (packet.counted || entry[2] == 0) &&
	       record_ok && std::ranges::equal(groups, expected_groups) &&
	       std::ranges::equal(payload, expected_payload);
}

MeshIndirectVerifier::MeshIndirectVerifier(RenderContext& context)
    : m_context(context),
      m_arena(context.GetGraphics(), context.GetCommandScheduler(), MemoryUsage::Download, 0,
              vk::BufferUsageFlagBits::eStorageBuffer |
                  vk::BufferUsageFlagBits::eShaderDeviceAddress,
              uint64_t {ArenaDwords} * sizeof(uint32_t)) {
	EXIT_IF(m_arena.Mapped().size() < uint64_t {ArenaDwords} * sizeof(uint32_t));
}

MeshIndirectVerifier::~MeshIndirectVerifier() = default;

vk::DeviceAddress MeshIndirectVerifier::Begin(const MeshIndirectVerifyPacket& packet,
                                              vk::DeviceAddress records, vk::DeviceAddress count,
                                              uint32_t stride, uint64_t args_addr) {
	std::lock_guard lock(m_mutex);
	if (m_blocks.empty()) {
		m_head = 0;
	}
	const auto tail  = m_blocks.empty() ? ArenaDwords : m_blocks.front().offset;
	uint32_t   begin = m_head;
	uint32_t   end   = tail;
	if (m_blocks.empty() || m_head > tail) {
		end = ArenaDwords;
		if (!m_blocks.empty() && ArenaDwords - m_head < tail) {
			begin = 0;
			end   = tail;
		}
	}
	if (end - begin < MeshIndirectVerifyHeaderDwords) {
		m_totals.unverified += packet.max_count;
		return 0;
	}
	Block block {.packet = packet, .args_addr = args_addr, .offset = begin};
	block.entries = std::min(packet.max_count, (end - begin - MeshIndirectVerifyHeaderDwords) /
	                                               MeshIndirectVerifyEntryDwords);
	block.dwords  = MeshIndirectVerifyHeaderDwords + block.entries * MeshIndirectVerifyEntryDwords;
	auto* words   = reinterpret_cast<uint32_t*>(m_arena.Mapped().data()) + block.offset;
	std::memset(words, 0, block.dwords * sizeof(uint32_t));
	const auto header = MeshIndirectHeaderOf(packet, records, count, stride, block.entries);
	std::memcpy(words, &header, sizeof(header));
	m_arena.Flush(uint64_t {block.offset} * sizeof(uint32_t),
	              uint64_t {block.dwords} * sizeof(uint32_t));
	m_head = block.offset + block.dwords;
	m_blocks.push_back(block);
	return m_arena.BufferDeviceAddress() + uint64_t {block.offset} * sizeof(uint32_t);
}

void MeshIndirectVerifier::Commit() {
	m_context.GetCommandScheduler().DeferOperation([this] { Check(); });
}

MeshIndirectVerifier::Totals MeshIndirectVerifier::GetTotals() const {
	std::lock_guard lock(m_mutex);
	return m_totals;
}

void MeshIndirectVerifier::Report(const char* what, const Block& block, uint32_t index,
                                  std::span<const uint32_t> entry) const {
	std::printf("MeshIndirectVerify: %s args=0x%016" PRIx64 " record=%u/%u drawn=%u count=%u "
	            "words=%08x %08x %08x %08x %08x groups=%ux%u checked=%" PRIu64
	            " mismatched=%" PRIu64 " unverified=%" PRIu64 " empty=%" PRIu64 "\n",
	            what, block.args_addr, index, block.packet.max_count, entry[1], entry[2], entry[3],
	            entry[4], entry[5], entry[6], entry[7], entry[8], entry[9], m_totals.checked,
	            m_totals.mismatched, m_totals.unverified, m_totals.empty);
}

void MeshIndirectVerifier::Check() {
	std::lock_guard lock(m_mutex);
	EXIT_IF(m_blocks.empty());
	const auto block = m_blocks.front();
	m_blocks.pop_front();
	m_arena.Invalidate(uint64_t {block.offset} * sizeof(uint32_t),
	                   uint64_t {block.dwords} * sizeof(uint32_t));
	const auto* words = reinterpret_cast<const uint32_t*>(m_arena.Mapped().data()) + block.offset +
	                    MeshIndirectVerifyHeaderDwords;
	for (uint32_t index = 0; index < block.entries; index++) {
		const std::span<const uint32_t> entry(words + index * MeshIndirectVerifyEntryDwords,
		                                      MeshIndirectVerifyEntryDwords);
		bool                            empty = false;
		const bool agrees = MeshIndirectEntryAgrees(block.packet, index, entry, empty);
		m_totals.checked++;
		m_totals.empty += empty ? 1u : 0u;
		if (!agrees) {
			m_totals.mismatched++;
			if ((m_totals.mismatched & (m_totals.mismatched - 1u)) == 0) {
				Report("mismatch", block, index, entry);
			}
		}
	}
	m_totals.unverified += block.packet.max_count - block.entries;
}

} // namespace Libs::Graphics
