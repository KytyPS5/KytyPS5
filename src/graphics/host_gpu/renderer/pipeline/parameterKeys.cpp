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

// Past this many results the cache starts over rather than scan more entries per fault.
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

// Watches cover whole pages, so a write next to a range may also mean a write within it.
bool PagesOverlap(const std::pair<uint64_t, uint64_t>& range, uint64_t address, uint64_t size) {
	const auto begin = Common::AlignDown(range.first, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(range.first + range.second, TRACKER_PAGE_SIZE);
	return address < end && begin < address + size;
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
	for (const auto& [key, entry]: m_entries) {
		if (entry.valid) Unwatch(entry.ranges);
	}
}

void ParameterKeyCache::Watch(const std::vector<Range>& ranges) {
	for (const auto& [address, size]: ranges)
		m_page_manager.UpdatePageWatchers<true>(address, size);
}

void ParameterKeyCache::Unwatch(const std::vector<Range>& ranges) {
	for (const auto& [address, size]: ranges)
		m_page_manager.UpdatePageWatchers<false>(address, size);
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
		if (m_entries.size() >= MaxEntries && !m_entries.contains(key)) {
			for (const auto& [old_key, entry]: m_entries) {
				if (entry.valid) Unwatch(entry.ranges);
			}
			m_entries.clear();
		}
		auto& entry = m_entries[key];
		if (entry.valid) Unwatch(entry.ranges);
		entry = {.keys = result, .ranges = ranges, .valid = true};
		Watch(entry.ranges);
	}
	std::vector<Range> again;
	const bool evaluated = IR::EvaluateParameterKeys(plan, runtime, parameter, result, again);
	Coalesce(again);
	{
		std::scoped_lock lock(m_mutex);
		auto&            entry = m_entries[key];
		if (entry.valid && (!evaluated || again != entry.ranges)) {
			Unwatch(entry.ranges);
			entry.valid = false;
			if (evaluated) {
				entry.ranges = std::move(again);
				entry.valid  = true;
				Watch(entry.ranges);
			}
		}
		if (entry.valid) entry.keys = result;
	}
	if (!evaluated) return false;
	keys = std::move(result);
	return true;
}

void ParameterKeyCache::InvalidateMemory(uint64_t address, uint64_t size) {
	std::scoped_lock lock(m_mutex);
	for (auto& [key, entry]: m_entries) {
		if (!entry.valid || std::ranges::none_of(entry.ranges, [&](const Range& range) {
			    return PagesOverlap(range, address, size);
		    }))
			continue;
		Unwatch(entry.ranges);
		entry.valid = false;
	}
}

} // namespace Libs::Graphics
