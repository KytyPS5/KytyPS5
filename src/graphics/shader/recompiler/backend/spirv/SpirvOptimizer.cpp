#include "graphics/shader/recompiler/backend/spirv/SpirvOptimizer.h"

#include <spirv-tools/optimizer.hpp>
#include <utility>

namespace Libs::Graphics::ShaderRecompiler::Spirv {

bool Optimize(std::vector<uint32_t>& code, Config::ShaderOptimizationType mode,
              std::string& diagnostics) {
	diagnostics.clear();
	if (mode == Config::ShaderOptimizationType::None) {
		return true;
	}
	if (mode != Config::ShaderOptimizationType::Size &&
	    mode != Config::ShaderOptimizationType::Performance) {
		diagnostics = "Unknown shader optimization mode";
		return false;
	}

	const auto consume = [&diagnostics](spv_message_level_t, const char*,
	                                    const spv_position_t& position, const char* message) {
		diagnostics += std::to_string(position.index) + ": " + message + "\n";
	};
	spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
	optimizer.SetMessageConsumer(consume);
	optimizer.RegisterPass(spvtools::CreateRemoveDuplicatesPass());
	if (mode == Config::ShaderOptimizationType::Performance) {
		optimizer.RegisterPass(spvtools::CreateLocalSingleStoreElimPass());
		optimizer.RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass());
		optimizer.RegisterPass(spvtools::CreateLocalRedundancyEliminationPass());
		optimizer.RegisterPass(spvtools::CreateSimplificationPass());
	}
	optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass());
	optimizer.RegisterPass(spvtools::CreateBlockMergePass());
	// Preserve descriptors, push constants and stage outputs consumed by other shaders.
	// Avoid the stock -O/-Os recipes: inlining and unrolling can expand BVH helpers.
	optimizer.RegisterPass(spvtools::CreateAggressiveDCEPass(true));
	optimizer.RegisterPass(spvtools::CreateEliminateDeadFunctionsPass());
	if (mode == Config::ShaderOptimizationType::Size) {
		optimizer.RegisterPass(spvtools::CreateCompactIdsPass());
	}

	spvtools::OptimizerOptions options;
	options.set_preserve_bindings(true);
	options.set_preserve_spec_constants(true);
	std::vector<uint32_t> optimized;
	if (!optimizer.Run(code.data(), code.size(), &optimized, options)) {
		return false;
	}
	spvtools::SpirvTools validator(SPV_ENV_VULKAN_1_3);
	validator.SetMessageConsumer(consume);
	if (!validator.Validate(optimized)) {
		return false;
	}
	code = std::move(optimized);
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv
