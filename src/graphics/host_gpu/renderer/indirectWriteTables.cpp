#include "graphics/host_gpu/renderer/indirectWriteTables.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "kernel/memory.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace Libs::Graphics {

GuestRange IndirectWriteTables::TargetRange(const ShaderBufferResource& target) {
	if (target.Type() != 0 || target.RawFormat() == 0) {
		return {};
	}
	const GuestRange range {target.Base48(), target.GetSize()};
	if (!range.Valid()) {
		return {};
	}
	// Stores through the table write whole DWORDs (the two low address bits are ignored), so an
	// unaligned target also covers the bytes of its first and last DWORDs.
	const auto begin = Common::AlignDown(range.address, sizeof(uint32_t));
	const auto end   = Common::AlignUp(range.End(), sizeof(uint32_t));
	const GuestRange aligned {begin, end - begin};
	return aligned.Valid() ? aligned : GuestRange {};
}

std::vector<GuestRange> IndirectWriteTables::CoalesceTargets(std::vector<GuestRange> ranges) {
	std::ranges::sort(ranges);
	std::vector<GuestRange> groups;
	for (const auto& range: ranges) {
		const auto begin = Common::AlignDown(range.address, BufferCache::CACHING_PAGESIZE);
		const auto end   = Common::AlignUp(range.End(), BufferCache::CACHING_PAGESIZE);
		if (!groups.empty() && begin <= groups.back().End()) {
			groups.back().size = std::max(groups.back().End(), end) - groups.back().address;
		} else {
			groups.push_back({begin, end - begin});
		}
	}
	return groups;
}

void IndirectWriteTables::Scan(Table& table, uint64_t first, uint64_t last, bool changed_only,
                               std::vector<GuestRange>& targets) {
	std::vector<ShaderBufferResource> entries(last - first);
	const auto address = table.address + first * sizeof(ShaderBufferResource);
	if (!LibKernel::Memory::TryReadBacking(address, entries.data(),
	                                       entries.size() * sizeof(ShaderBufferResource))) {
		static bool logged = false;
		if (!logged) {
			logged = true;
			LOGF("Compute: V# table 0x%016" PRIx64 " is unreadable at 0x%016" PRIx64 "\n",
			     table.address, address);
		}
		return;
	}
	for (uint64_t index = 0; index < entries.size(); index++) {
		auto&       cached = table.entries[first + index];
		const auto& entry  = entries[index];
		if (changed_only && std::memcmp(&cached, &entry, sizeof(entry)) == 0) {
			continue;
		}
		const auto previous = TargetRange(cached);
		const auto range    = TargetRange(entry);
		table.target_count -= previous.size != 0 ? 1 : 0;
		table.target_count += range.size != 0 ? 1 : 0;
		cached = entry;
		if (range.size != 0) {
			targets.push_back(range);
		}
	}
}

void IndirectWriteTables::Cover(Table& table, std::vector<GuestRange>& targets) {
	std::erase_if(targets, [&](const GuestRange& range) {
		// Creating a buffer copies its guest memory, so only mapped targets can be cached. A store
		// through another one faults on the GPU as it would on the console.
		if (!m_context.IsMapped(range.address, range.size)) {
			table.unmapped.push_back(range);
			return true;
		}
		m_targets.Add(range.address, range.size);
		return false;
	});
	auto& buffer_cache = m_context.GetBufferCache();
	// Tables name many small neighbouring targets; caching their union at once avoids merging
	// buffers again for each of them.
	for (const auto& group: CoalesceTargets(std::move(targets))) {
		(void)buffer_cache.FindBuffer(group.address, group.size);
	}
	targets.clear();
}

bool IndirectWriteTables::Prepare(uint64_t address, uint64_t size) {
	constexpr uint64_t EntrySize = sizeof(ShaderBufferResource);
	const auto         count     = size / EntrySize;
	if (count == 0 || !GuestRange {address, count * EntrySize}.Valid()) {
		return false;
	}
	const auto bytes = count * EntrySize;
	auto       table = std::ranges::find_if(m_tables, [&](const Table& candidate) {
		return candidate.address == address && candidate.size == bytes;
	});
	bool       full  = false;
	if (table == m_tables.end()) {
		if (m_tables.size() == MaxTables) {
			m_tables.erase(std::ranges::min_element(m_tables, {}, &Table::last_use));
		}
		auto& created   = m_tables.emplace_back();
		created.address = address;
		created.size    = bytes;
		created.entries.resize(count);
		table = m_tables.end() - 1;
		full  = true;
	}
	table->last_use = ++m_use_count;

	auto& buffer_cache = m_context.GetBufferCache();
	if (buffer_cache.HasGpuDirtyBytes(address, bytes)) {
		// The guest copy of a table the GPU wrote is stale: download it before scanning.
		buffer_cache.ReadMemory(address, bytes);
		full = true;
	}
	std::vector<GuestRange> targets;
	// Pages the CPU did not write since the last scan still hold the snapshot's entries.
	buffer_cache.ConsumeCpuWrites(address, bytes, [&](uint64_t run, uint64_t run_size) {
		if (!full) {
			const auto first = (run - address) / EntrySize;
			const auto last =
			    std::min(Common::AlignUp(run + run_size - address, EntrySize) / EntrySize, count);
			Scan(*table, first, last, true, targets);
		}
	});
	if (full) {
		Scan(*table, 0, count, true, targets);
	}
	// Cache again the targets whose buffers the garbage collector released.
	buffer_cache.ConsumeReleasedRanges([&](uint64_t start, uint64_t end) {
		m_targets.ForEachInRange(start, end - start, [&](uint64_t begin, uint64_t finish) {
			targets.push_back({begin, finish - begin});
		});
	});
	if (const auto version = m_context.MappedRangesVersion(); version != table->mapped_version) {
		table->mapped_version = version;
		targets.insert(targets.end(), table->unmapped.begin(), table->unmapped.end());
		table->unmapped.clear();
	}
	if (!targets.empty()) {
		Cover(*table, targets);
		std::ranges::sort(table->unmapped);
		table->unmapped.erase(std::unique(table->unmapped.begin(), table->unmapped.end()),
		                      table->unmapped.end());
	}
	if (table->target_count == 0) {
		return false;
	}
	m_pending_writes = true;
	return true;
}

void IndirectWriteTables::NoteWrites(std::span<const uint64_t> pages) {
	RangeSet written;
	for (const auto page: pages) {
		written.Add(page, BufferCache::CACHING_PAGESIZE);
	}
	auto&      buffer_cache  = m_context.GetBufferCache();
	auto&      texture_cache = m_context.GetTextureCache();
	const auto note          = [&](uint64_t begin, uint64_t end) {
		buffer_cache.NoteGpuWrites(begin, end - begin);
		// As for any other buffer store, images over the written bytes are stale.
		texture_cache.InvalidateMemoryFromGPU(begin, end - begin);
	};
	written.ForEach([&](uint64_t start, uint64_t end) {
		bool targeted = false;
		m_targets.ForEachInRange(start, end - start, [&](uint64_t begin, uint64_t finish) {
			targeted = true;
			note(begin, finish);
		});
		if (!targeted) {
			note(start, end);
		}
	});
}

} // namespace Libs::Graphics
