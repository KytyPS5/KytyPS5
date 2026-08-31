#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <utility>

namespace Libs::Graphics {

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
	ShaderRecompiler::IR::ResourceSnapshot snapshot;
	if (!ShaderRecompiler::IR::MaterializeResources(*program, runtime, snapshot) ||
	    !ShaderRecompiler::IR::ValidateResourceSpecialization(*program, snapshot)) {
		return false;
	}
	auto resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(std::move(snapshot));
	stage = {std::move(program), std::move(resources)};
	return true;
}

} // namespace Libs::Graphics
