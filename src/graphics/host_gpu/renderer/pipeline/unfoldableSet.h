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

// What draws have proved about one shader's descriptors: the `first_use_pc` of every resource a
// materialization had to bind null because its source is selected on the GPU. A pc here is proof,
// not a guess, so resource tracking may claim it as a runtime table on the next translation.
//
// Split out of PipelineCache so the rollback below can be tested without a Vulkan device.
//
// The reason a rollback exists at all: claiming a proven pc changes what the backend emits, and
// for RESIDENT EVIL REQUIEM's clustered-lighting pixel shader it emits a dispatcher module of
// 384046 words that this host's driver will not build - measured twice at over 200 s inside
// vkCreateGraphicsPipelines, once ending in a dead process. MaxDispatcherSpirvWords refuses that
// module, which is right, but a refusal used to travel to `skipped_shaders`, and that set is keyed
// on the shader hash alone with no generation in it. So the refusal disabled the shader outright -
// including the permutation from the generation before, which was drawing perfectly well, just
// without the one resource the channel was trying to recover. Losing an effect is the bug the
// channel was written to fix; losing the shader is worse than the bug.
struct UnfoldableSet {
	// Sorted, unique. Copied into ResourcePlan::unfoldable_pcs on the next translation.
	std::vector<uint32_t> pcs;
	// Bumped only when `pcs` grows, which is the only condition that costs a re-translation.
	uint32_t generation = 0;
	// The set as it stood before the growth the current generation names. At most one growth is
	// ever outstanding: a growth bumps the generation, so the very next draw of this shader misses
	// its key and compiles. The compile a rollback answers is therefore always the one this stash
	// was taken for.
	std::vector<uint32_t> previous_pcs;
	uint32_t              previous_generation = 0;
	// Set by a rollback. The proof still stands - the source really is unfoldable - but claiming
	// it produces a module this host will not build, so the channel stops offering it instead of
	// rebuilding into the same refusal on every draw.
	bool frozen = false;
};

// True when `pcs` gained anything, which is the only condition that may bump a generation and so
// the only condition that costs a re-translation. The set never shrinks, so a shader whose
// descriptor the tracker cannot serve reports the same pc again, adds nothing, and rebuilds
// nothing: one rebuild per (shader, descriptor), not one per draw. Pipeline builds for RE9's
// heaviest pixel shaders run to tens of seconds, so a loop here would be far worse than the
// missing effect it is trying to fix.
//
// A frozen set learns nothing more. That is not a claim the report was wrong - it is a claim that
// acting on it costs more than the effect is worth on this host.
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
			// Stash before the first insert, so the stash is the set the current generation's
			// permutations were built from.
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

// A permutation the channel asked for and the host refused. Undo the growth that asked for it and
// stop the channel, so the next draw of this shader keys back to the generation that still has a
// permutation which draws.
//
// Returns false when there is nothing to fall back to: generation 0 was refused on its own merits,
// not because of anything the channel did, and the caller must disable the shader as it always
// did. Idempotent for a frozen set with nothing outstanding, which is what a second refusal on the
// same shader looks like.
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

// The sets keyed by code hash; a set Learn has grown never reads as empty again.
class UnfoldableSets {
public:
	// `code_key` hashes the code, and is called only when a set of this size can be non-empty.
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
