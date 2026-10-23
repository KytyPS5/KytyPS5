#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_UNFOLDABLESET_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_UNFOLDABLESET_H_

#include <algorithm>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Libs::Graphics {

// The first_use_pc of every resource a materialization proved GPU-selected, for one shader.
struct UnfoldableSet {
	// Sorted, unique.
	std::vector<uint32_t> pcs;
	uint32_t              generation = 0;
	// The set before the growth the current generation names; at most one growth is outstanding.
	std::vector<uint32_t> previous_pcs;
	uint32_t              previous_generation = 0;
	// Set by a rollback: the host refused the module the proof produces.
	bool frozen = false;
};

// True when `pcs` grew; the set never shrinks, so one rebuild per (shader, descriptor).
inline bool Learn(UnfoldableSet& set, std::span<const uint32_t> reported) {
	if (set.frozen) {
		return false;
	}
	bool grew = false;
	for (const auto pc: reported) {
		const auto at = std::ranges::lower_bound(set.pcs, pc);
		if (at != set.pcs.end() && *at == pc) {
			continue;
		}
		if (!grew) {
			set.previous_pcs        = set.pcs;
			set.previous_generation = set.generation;
		}
		set.pcs.insert(std::ranges::lower_bound(set.pcs, pc), pc);
		grew = true;
	}
	if (grew) {
		set.generation++;
	}
	return grew;
}

// Undoes the growth whose rebuild the host refused; false when there is nothing to fall back to.
inline bool RefuseRebuild(UnfoldableSet& set) {
	if (set.generation == 0 || set.generation == set.previous_generation) {
		return false;
	}
	set.pcs        = std::move(set.previous_pcs);
	set.generation = set.previous_generation;
	set.previous_pcs.clear();
	set.frozen = true;
	return true;
}

// Equal code has equal sizes, so a size no set was ever grown for names only empty sets.
inline uint64_t UnfoldableCodeSize(std::span<const uint32_t> code,
                                   std::span<const uint32_t> back_code) {
	return static_cast<uint64_t>(code.size()) | (static_cast<uint64_t>(back_code.size()) << 32u);
}

class UnfoldableSets {
public:
	// `code_key` is called only when a set of this size can be non-empty.
	template <typename CodeKey>
	[[nodiscard]] const UnfoldableSet& Find(uint64_t code_size, CodeKey&& code_key) {
		static const UnfoldableSet empty;
		return m_learned_sizes.contains(code_size) ? m_sets[code_key()] : empty;
	}

	[[nodiscard]] UnfoldableSet& Get(uint64_t code_key) { return m_sets[code_key]; }

	bool Learn(uint64_t code_size, uint64_t code_key, std::span<const uint32_t> reported) {
		if (!Graphics::Learn(m_sets[code_key], reported)) {
			return false;
		}
		m_learned_sizes.insert(code_size);
		return true;
	}

private:
	std::unordered_map<uint64_t, UnfoldableSet> m_sets;
	std::unordered_set<uint64_t>                m_learned_sizes;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_UNFOLDABLESET_H_
