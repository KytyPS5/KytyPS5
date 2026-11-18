#include "graphics/host_gpu/renderer/pipeline/parameterKeys.h"

#include "common/alignment.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/regionDefinitions.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/shader/recompiler/ir/passes/ParameterKeys.h"

#include <algorithm>
#include <xxhash.h>

namespace Libs::Graphics {
namespace {

namespace IR = ShaderRecompiler::IR;

// Past this many results the cache starts over rather than keep growing.
constexpr size_t MaxEntries = 8192;

void Coalesce(std::vector<std::pair<uint64_t, uint64_t>>& ranges) {
	std::ranges::sort(ranges);
	size_t out = 0;
	for (const auto& [address, size]: ranges) {
		if (size == 0u) continue;
		if (out != 0u && address <= ranges[out - 1u].first + ranges[out - 1u].second) {
			auto& last  = ranges[out - 1u];
			last.second = std::max(last.first + last.second, address + size) - last.first;
		} else {
			ranges[out++] = {address, size};
		}
	}
	ranges.resize(out);
}

// The first tracker page a watch of the range covers and the one past its last.
std::pair<uint64_t, uint64_t> WatchedPages(uint64_t address, uint64_t size) {
	return {address / TRACKER_PAGE_SIZE,
	        Common::AlignUp(address + size, TRACKER_PAGE_SIZE) / TRACKER_PAGE_SIZE};
}

// The page manager updates each run of consecutive pages at once.
template <bool track>
void UpdateWatches(PageManager& page_manager, const std::vector<uint64_t>& pages) {
	for (size_t first = 0; first < pages.size();) {
		size_t last = first + 1u;
		while (last < pages.size() && pages[last] == pages[last - 1u] + 1u)
			++last;
		page_manager.UpdatePageWatchers<track>(pages[first] * TRACKER_PAGE_SIZE,
		                                       (last - first) * TRACKER_PAGE_SIZE);
		first = last;
	}
}

} // namespace

size_t ParameterKeyCache::KeyHash::operator()(const Key& key) const noexcept {
	const auto words = XXH3_64bits(key.signature.data(), key.signature.size() * sizeof(uint32_t));
	return static_cast<size_t>(words ^ (reinterpret_cast<uintptr_t>(key.plan) * 31u) ^
	                           key.parameter);
}

ParameterKeyCache::ParameterKeyCache(RenderContext& context, PageManager& page_manager)
    : m_context(context), m_page_manager(page_manager) {}

ParameterKeyCache::~ParameterKeyCache() {
	Clear();
}

// One page-manager watch per indexed page, however many entries read it (it counts 255 at most).
void ParameterKeyCache::Watch(Entry& entry) {
	for (const auto& [address, size]: entry.ranges) {
		const auto [first, end] = WatchedPages(address, size);
		for (auto page = first; page < end; ++page) {
			auto& entries = m_pages[page];
			if (entries.empty()) m_changed_pages.push_back(page);
			entries.insert(&entry);
		}
	}
	UpdateWatches<true>(m_page_manager, m_changed_pages);
	m_changed_pages.clear();
	entry.valid = true;
}

void ParameterKeyCache::Unwatch(Entry& entry) {
	for (const auto& [address, size]: entry.ranges) {
		const auto [first, end] = WatchedPages(address, size);
		for (auto page = m_pages.lower_bound(first); page != m_pages.end() && page->first < end;) {
			page->second.erase(&entry);
			if (page->second.empty()) {
				m_changed_pages.push_back(page->first);
				page = m_pages.erase(page);
			} else {
				++page;
			}
		}
	}
	UpdateWatches<false>(m_page_manager, m_changed_pages);
	m_changed_pages.clear();
	entry.valid = false;
}

void ParameterKeyCache::Clear() {
	for (auto& [key, entry]: m_entries) {
		if (entry.valid) Unwatch(entry);
	}
	m_entries.clear();
}

bool ParameterKeyCache::Keys(const IR::ResourcePlan& plan, const IR::SrtRuntime& runtime,
                             uint32_t parameter, std::vector<uint32_t>& keys) {
	if (!IR::ParameterKeySignature(plan, runtime, parameter, m_signature)) return false;
	Key                key {.plan = &plan, .parameter = parameter, .signature = m_signature};
	std::vector<Range> ranges;
	{
		std::scoped_lock lock(m_mutex);
		if (const auto found = m_entries.find(key);
		    found != m_entries.end() && found->second.valid) {
			keys   = found->second.keys;
			ranges = found->second.ranges;
		}
	}
	// GPU writes leave no fault; evaluating again reads them back.
	auto& buffers = m_context.GetBufferCache();
	if (!ranges.empty() && std::ranges::none_of(ranges, [&](const Range& range) {
		    return buffers.HasGpuDirtyBytes(range.first, range.second);
	    }))
		return true;
	// Watch what the first evaluation read, then evaluate again: a CPU write that landed before
	// the watch shows in the second result, and one after it invalidates the entry.
	std::vector<uint32_t> result;
	ranges.clear();
	if (!IR::EvaluateParameterKeys(plan, runtime, parameter, result, ranges)) return false;
	Coalesce(ranges);
	{
		std::scoped_lock lock(m_mutex);
		if (m_entries.size() >= MaxEntries && !m_entries.contains(key)) Clear();
		auto& entry = m_entries[key];
		if (entry.valid) Unwatch(entry);
		entry = {.keys = result, .ranges = ranges};
		Watch(entry);
	}
	std::vector<Range> again;
	const bool evaluated = IR::EvaluateParameterKeys(plan, runtime, parameter, result, again);
	Coalesce(again);
	{
		std::scoped_lock lock(m_mutex);
		auto&            entry = m_entries[key];
		if (entry.valid && (!evaluated || again != entry.ranges)) {
			Unwatch(entry);
			if (evaluated) {
				entry.ranges = std::move(again);
				Watch(entry);
			}
		}
		if (entry.valid) entry.keys = result;
	}
	if (!evaluated) return false;
	keys = std::move(result);
	return true;
}

void ParameterKeyCache::InvalidateMemory(uint64_t address, uint64_t size) {
	const auto [first, end] = WatchedPages(address, size);
	std::scoped_lock lock(m_mutex);
	// Watches cover whole pages, so a write next to a range may also mean a write within it.
	for (auto page = m_pages.lower_bound(first); page != m_pages.end() && page->first < end; ++page)
		m_hits.insert(m_hits.end(), page->second.begin(), page->second.end());
	for (auto* entry: m_hits) {
		if (entry->valid) Unwatch(*entry);
	}
	m_hits.clear();
}

} // namespace Libs::Graphics
