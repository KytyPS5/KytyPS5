// A refused unfoldable rebuild must fall back to the previous permutation, not disable the shader.

#include "graphics/host_gpu/renderer/pipeline/unfoldableSet.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using Libs::Graphics::Learn;
using Libs::Graphics::RefuseRebuild;
using Libs::Graphics::UnfoldableCodeSize;
using Libs::Graphics::UnfoldableSet;
using Libs::Graphics::UnfoldableSets;

int g_failures = 0;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "UnfoldableRebuildTests: failed: %s\n", text);
		g_failures++;
	}
}

// Mirrors ProgramCache::Get's key/learn/compile/refuse loop.
class FakeCache {
public:
	// `refuse_when_claimed` stands in for MaxDispatcherSpirvWords.
	struct Draw {
		uint64_t              hash = 0;
		std::vector<uint32_t> reported;
		bool                  refuse_when_claimed = false;
	};

	// The handle the draw renders with; empty means the draw drew nothing.
	std::optional<uint32_t> Run(const Draw& draw) {
		if (m_skipped.contains(draw.hash)) {
			return {};
		}
		auto&      proven = m_unfoldable[draw.hash];
		const auto key    = Key {draw.hash, proven.generation};
		Learn(proven, draw.reported);
		if (const auto found = m_permutations.find(key); found != m_permutations.end()) {
			return found->second;
		}
		const bool channel_rebuild = !proven.pcs.empty();
		if (channel_rebuild && draw.refuse_when_claimed) {
			if (RefuseRebuild(proven)) {
				m_refused.insert(draw.hash);
				return {};
			}
			m_skipped.insert(draw.hash);
			return {};
		}
		const auto handle = ++m_next_handle;
		m_permutations.emplace(Key {draw.hash, proven.generation}, handle);
		return handle;
	}

	[[nodiscard]] bool Skipped(uint64_t hash) const { return m_skipped.contains(hash); }
	[[nodiscard]] bool Refused(uint64_t hash) const { return m_refused.contains(hash); }
	[[nodiscard]] uint32_t Compiles() const { return m_next_handle; }
	[[nodiscard]] const UnfoldableSet& Proven(uint64_t hash) { return m_unfoldable[hash]; }

private:
	struct Key {
		uint64_t hash;
		uint32_t generation;
		bool     operator==(const Key&) const = default;
	};
	struct KeyHash {
		size_t operator()(const Key& key) const {
			return std::hash<uint64_t> {}(key.hash) ^ (key.generation * 0x9e3779b9u);
		}
	};

	std::unordered_map<uint64_t, UnfoldableSet> m_unfoldable;
	std::unordered_map<Key, uint32_t, KeyHash>  m_permutations;
	std::unordered_set<uint64_t>                m_skipped;
	std::unordered_set<uint64_t>                m_refused;
	uint32_t                                    m_next_handle = 0;
};

constexpr uint64_t Aba519 = 0xaba519e634a11781ull;

// Compile generation 0, report the degrade, have the host refuse the claimed module, draw again.
std::optional<uint32_t> RunRefusalSequence(FakeCache& cache) {
	const FakeCache::Draw clean {.hash = Aba519};
	const FakeCache::Draw degrading {
	    .hash = Aba519, .reported = {0x8f8u}, .refuse_when_claimed = true};
	Check(cache.Run(clean).has_value(), "the first draw did not compile a permutation");
	cache.Run(degrading);
	cache.Run(degrading);
	return cache.Run(degrading);
}

void TestRefusedRebuildFallsBackToThePreviousPermutation() {
	FakeCache  cache;
	const auto after = RunRefusalSequence(cache);
	Check(after.has_value(), "a refused rebuild stopped the shader from drawing");
	Check(!cache.Skipped(Aba519), "a refused rebuild disabled the shader");
	Check(cache.Refused(Aba519), "the refusal was not reported");
	Check(after.value_or(0u) == 1u, "the shader did not fall back to its first permutation");
}

// After the rollback the set is frozen, so the same report never compiles again.
void TestAFrozenSetNeverRebuildsAgain() {
	FakeCache             cache;
	(void)RunRefusalSequence(cache);
	const auto after_refusal = cache.Compiles();
	const FakeCache::Draw degrading {
	    .hash = Aba519, .reported = {0x8f8u}, .refuse_when_claimed = true};
	bool every_draw_rendered = true;
	for (int i = 0; i < 32; i++) {
		every_draw_rendered = cache.Run(degrading).has_value() && every_draw_rendered;
	}
	Check(every_draw_rendered, "a later draw stopped rendering");
	Check(cache.Compiles() == after_refusal, "a frozen set rebuilt the shader again");
	Check(cache.Proven(Aba519).frozen, "the set was not frozen by the refusal");
	Check(cache.Proven(Aba519).generation == 0, "the generation did not roll back");
	Check(cache.Proven(Aba519).pcs.empty(), "the rolled-back set kept the claimed pc");
}

// Generation 0 has no earlier permutation to fall back to.
void TestGenerationZeroRefusalStillDisablesTheShader() {
	UnfoldableSet set;
	Check(!RefuseRebuild(set), "generation 0 claimed a fallback it does not have");
	Check(!set.frozen, "a refusal with nothing to roll back froze the set");
}

// A rollback undoes only the last growth, not every pc ever proven.
void TestRollbackUndoesOnlyTheLastGrowth() {
	UnfoldableSet set;
	Check(Learn(set, std::vector<uint32_t> {0x100u}), "the first report did not grow the set");
	Check(set.generation == 1, "the first growth did not bump the generation");
	Check(!Learn(set, std::vector<uint32_t> {0x100u}), "a repeated pc grew the set");
	Check(set.generation == 1, "a repeated pc bumped the generation");
	Check(Learn(set, std::vector<uint32_t> {0x200u}), "the second report did not grow the set");
	Check(set.generation == 2, "the second growth did not bump the generation");
	Check(RefuseRebuild(set), "a refusal at generation 2 found no fallback");
	Check(set.generation == 1, "the rollback did not return to the previous generation");
	Check(set.pcs == std::vector<uint32_t> {0x100u},
	      "the rollback undid more than the last growth");
	Check(!Learn(set, std::vector<uint32_t> {0x300u}), "a frozen set learned a new pc");
	Check(set.generation == 1, "a frozen set bumped its generation");
}

// Code of a size nothing was learned for skips the hash and reads the empty set.
void TestUnlearnedSizesSkipTheCodeHash() {
  UnfoldableSets sets;
  uint32_t hashed = 0;
  const auto key = [&](uint64_t value) {
    return [&hashed, value] {
      hashed++;
      return value;
    };
  };
  const auto empty = [](const UnfoldableSet &set) {
    return set.pcs.empty() && set.generation == 0 && set.previous_pcs.empty() &&
           !set.frozen;
  };
  const std::vector<uint32_t> code(10, 0u);
  const std::vector<uint32_t> back(3, 0u);
  const auto size = UnfoldableCodeSize(code, {});
  Check(size != UnfoldableCodeSize(code, back) &&
            UnfoldableCodeSize(std::vector<uint32_t>(3, 0u),
                               std::vector<uint32_t>(10, 0u)) !=
                UnfoldableCodeSize(code, back),
        "the code size key does not tell the front and back halves apart");
  Check(empty(sets.Find(size, key(1))) && hashed == 0,
        "an unlearned size hashed its code");
  Check(!sets.Learn(size, 1, {}) && empty(sets.Find(size, key(1))) &&
            hashed == 0,
        "an empty report made a size count as learned");
  Check(sets.Learn(size, 1, std::vector<uint32_t>{0x100u}),
        "a report did not grow the set");
  const auto &learned = sets.Find(size, key(1));
  Check(hashed == 1 && learned.pcs == std::vector<uint32_t>{0x100u} &&
            learned.generation == 1,
        "a learned size did not hash its code to find the grown set");
  Check(empty(sets.Find(size, key(2))) && hashed == 2,
        "other code of a learned size did not read its own, empty, set");
  Check(empty(sets.Find(size + 1u, key(1))) && hashed == 2,
        "code of another size was hashed or read a set it cannot have");
  Check(RefuseRebuild(sets.Get(1)) && sets.Find(size, key(1)).frozen,
        "a rolled-back set stopped being found under its size");
}

} // namespace

int main() {
	TestRefusedRebuildFallsBackToThePreviousPermutation();
	TestAFrozenSetNeverRebuildsAgain();
	TestGenerationZeroRefusalStillDisablesTheShader();
	TestRollbackUndoesOnlyTheLastGrowth();
	TestUnlearnedSizesSkipTheCodeHash();
	if (g_failures != 0) {
		std::fprintf(stderr, "UnfoldableRebuildTests: %d check(s) failed\n", g_failures);
		return 1;
	}
	std::printf("UnfoldableRebuildTests: all checks passed\n");
	return 0;
}
