#pragma once
#include <spirv-tools/optimizer.hpp>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics {
// Extracted unchanged pass order from pipelineCache.cpp:407. Mode1=Size,
// mode2=Performance. Production caller handles None and failure fallback.
inline bool RunSpirvOptimizationRecipe(std::span<const uint32_t> input, uint32_t mode,
                                       bool cooperative_wave64, std::vector<uint32_t>& output,
                                       std::string& messages) {
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_3);
    optimizer.SetMessageConsumer([&](spv_message_level_t, const char*, const spv_position_t& position,
                                    const char* message) {
        messages += std::to_string(position.line) + ": " + std::to_string(position.column) +
                    " (" + std::to_string(position.index) + ") " + message + "\n";
    });
    if (cooperative_wave64) {
        optimizer.RegisterPass(spvtools::CreateSimplificationPass())
            .RegisterPass(spvtools::CreateLocalRedundancyEliminationPass())
            .RegisterPass(spvtools::CreateBlockMergePass())
            .RegisterPass(spvtools::CreateCompactIdsPass());
    } else {
        optimizer.RegisterPass(spvtools::CreateDeadBranchElimPass())
            .RegisterPass(spvtools::CreateEliminateDeadFunctionsPass())
            .RegisterPass(spvtools::CreateLocalSingleBlockLoadStoreElimPass())
            .RegisterPass(spvtools::CreateLocalSingleStoreElimPass())
            .RegisterPass(spvtools::CreateAggressiveDCEPass(true))
            .RegisterPass(spvtools::CreateSimplificationPass())
            .RegisterPass(spvtools::CreateRedundancyEliminationPass())
            .RegisterPass(spvtools::CreateBlockMergePass());
    }
    if (mode == 1u) optimizer.RegisterPass(spvtools::CreateStripDebugInfoPass());
    return optimizer.Run(input.data(), input.size(), &output) && !output.empty();
}
} // namespace Libs::Graphics
