#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PARAMETERKEYS_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PARAMETERKEYS_H_

#include "common/common.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Libs::Graphics {

class PageManager;
class RenderContext;

// The values vertex stages export to pixel inputs, evaluated from the vertex records they read.
// A result is keyed by the stage's plan, the parameter and the uniform values it depends on, and
// lasts until the CPU writes its record ranges or the GPU modifies them.
class ParameterKeyCache final {
public:
	ParameterKeyCache(RenderContext& context, PageManager& page_manager);
	~ParameterKeyCache();
	KYTY_CLASS_NO_COPY(ParameterKeyCache);

	// GPU thread. False when the values are not known exactly.
	bool Keys(const ShaderRecompiler::IR::ResourcePlan& plan,
	          const ShaderRecompiler::IR::SrtRuntime& runtime, uint32_t parameter,
	          std::vector<uint32_t>& keys);
	// CPU write fault, on any thread.
	void InvalidateMemory(uint64_t address, uint64_t size);

private:
	using Range = std::pair<uint64_t, uint64_t>; // Address, bytes.

	struct Key {
		const ShaderRecompiler::IR::ResourcePlan* plan      = nullptr;
		uint32_t                                  parameter = 0;
		std::vector<uint32_t>                     signature;

		bool operator==(const Key& other) const = default;
	};
	struct KeyHash {
		size_t operator()(const Key& key) const noexcept;
	};
	struct Entry {
		std::vector<uint32_t> keys;
		std::vector<Range>    ranges; // Watched while valid.
		bool                  valid = false;
	};

	void Watch(const std::vector<Range>& ranges);
	void Unwatch(const std::vector<Range>& ranges);

	RenderContext&                          m_context;
	PageManager&                            m_page_manager;
	std::mutex                              m_mutex; // Entries, for faults on other threads.
	std::unordered_map<Key, Entry, KeyHash> m_entries;
	std::vector<uint32_t>                   m_signature;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PARAMETERKEYS_H_
