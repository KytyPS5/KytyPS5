#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

// A snapshot is built for every shader stage of every draw call and released again as soon as the
// draw is recorded, so allocating one each time meant a control block and a fresh set of vectors
// per draw. Hand out recycled snapshots instead: an entry is free once the pool holds the only
// reference to it, and reusing it keeps the vector capacity from its previous life.
std::shared_ptr<ShaderRecompiler::IR::ResourceSnapshot> AcquireSnapshot() {
	// Only a handful are ever live at once (the stages of the draw being recorded), so a linear
	// scan is cheaper than bookkeeping that would keep a free list exact.
	constexpr size_t kMaxPooled = 16;
	thread_local std::vector<std::shared_ptr<ShaderRecompiler::IR::ResourceSnapshot>> pool;

	for (const auto& entry: pool) {
		if (entry.use_count() == 1) {
			return entry;
		}
	}
	auto created = std::make_shared<ShaderRecompiler::IR::ResourceSnapshot>();
	if (pool.size() < kMaxPooled) {
		pool.push_back(created);
	}
	return created;
}

} // namespace

bool ShaderMaterializeStageRuntime(std::shared_ptr<const ShaderRecompiler::IR::Program> program,
                                   std::span<const uint32_t> user_data, uint64_t shader_base,
                                   ShaderStageRuntime& stage,
                                   ShaderSpecializationMemoryReader read_specialization_memory,
                                   void*                            read_memory_data) {
	if (program == nullptr) {
		return false;
	}
	ShaderRecompiler::IR::SrtRuntime runtime;
	runtime.user_data                  = user_data;
	runtime.shader_base                = shader_base;
	runtime.read_specialization_memory = read_specialization_memory;
	runtime.userdata                   = read_memory_data;
	auto  resources = AcquireSnapshot();
	auto& snapshot  = *resources;
	if (!ShaderRecompiler::IR::MaterializeResources(*program, runtime, snapshot) ||
	    !ShaderRecompiler::IR::ValidateResourceSpecialization(*program, snapshot)) {
		return false;
	}
	stage = {std::move(program), std::move(resources)};
	return true;
}

} // namespace Libs::Graphics
