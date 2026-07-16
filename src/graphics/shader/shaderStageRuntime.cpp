#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <utility>

namespace Libs::Graphics {

bool ShaderMaterializeStageRuntime(std::shared_ptr<const ShaderRecompiler::IR::Program> program,
                                   std::span<const uint32_t> user_data, uint64_t shader_base,
                                   ShaderStageRuntime* stage, std::string* error) {
	if (program == nullptr || stage == nullptr) {
		if (error != nullptr) {
			*error =
			    program == nullptr ? "missing native shader plan" : "missing shader stage output";
		}
		return false;
	}
	ShaderRecompiler::IR::SrtRuntime runtime;
	runtime.user_data   = user_data;
	runtime.shader_base = shader_base;
	ShaderRecompiler::IR::ResourceSnapshot snapshot;
	if (!ShaderRecompiler::IR::MaterializeResources(*program, runtime, &snapshot, error) ||
	    !ShaderRecompiler::IR::ValidateResourceSpecialization(*program, snapshot, error)) {
		return false;
	}
	auto resources =
	    std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(std::move(snapshot));
	*stage = {std::move(program), std::move(resources)};
	return true;
}

} // namespace Libs::Graphics
