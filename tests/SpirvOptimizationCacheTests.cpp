#include "graphics/host_gpu/renderer/pipeline/SpirvOptimizationCache.h"
#include "graphics/host_gpu/renderer/pipeline/SpirvOptimizationRecipe.h"
#include <spirv-tools/libspirv.hpp>
#include <iostream>
#include <stdexcept>

using namespace Libs::Graphics;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    std::filesystem::path scratch;
    try {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        scratch = std::filesystem::absolute("_Build/opt-cache-test-" + std::to_string(suffix));
        Check(std::filesystem::create_directory(scratch), "scratch directory already exists");
        std::cout << "scratch=" << scratch.string() << '\n';
        spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
        std::vector<uint32_t> input;
        const std::string assembly = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%one = OpConstant %uint 1
%main = OpFunction %void None %fn
%entry = OpLabel
%unused = OpIAdd %uint %one %one
OpReturn
OpFunctionEnd
)";
        Check(tools.Assemble(assembly, &input) && tools.Validate(input), "independent SPIR-V fixture invalid");
        SpirvOptimizationCache cache(scratch);
        SpirvOptimizationCache::State state{std::string(64u, 'a'), 2u, SPV_ENV_VULKAN_1_3, false};
        size_t optimizer_calls = 0;
        std::vector<uint32_t> oracle, observed{0xdeadbeef};
        std::string messages;
        Check(RunSpirvOptimizationRecipe(input, state.mode, state.cooperative_wave64, oracle, messages) &&
              tools.Validate(oracle), "uncached optimizer oracle failed");
        const auto compile = [&] {
            if (cache.Load(input, state, observed)) return true;
            ++optimizer_calls;
            if (!RunSpirvOptimizationRecipe(input, state.mode, state.cooperative_wave64, observed, messages)) return false;
            return cache.Store(input, state, observed);
        };
        Check(compile() && optimizer_calls == 1u && observed == oracle, "cold cache changes compiler output");
        Check(compile() && optimizer_calls == 1u && observed == oracle, "warm cache executes optimizer or changes output");
        SpirvOptimizationCache reopened(scratch);
        observed = {0xdeadbeef};
        Check(reopened.Load(input, state, observed) && observed == oracle, "cache did not persist across objects");
        const auto miss = [&](std::span<const uint32_t> words, const SpirvOptimizationCache::State& changed) {
            observed = {0xdeadbeef};
            Check(!reopened.Load(words, changed, observed) && observed == std::vector<uint32_t>{0xdeadbeef},
                  "cache miss fabricated or changed output");
        };
        for (int field = 0; field < 4; ++field) {
            auto changed = state;
            if (field == 0) changed.mode = 1;
            if (field == 1) changed.cooperative_wave64 = true;
            if (field == 2) changed.environment = SPV_ENV_VULKAN_1_2;
            if (field == 3) changed.implementation[0] = 'b';
            miss(input, changed);
        }
        auto changed_assembly = assembly;
        changed_assembly.replace(changed_assembly.find("%uint 1"), 7u, "%uint 2");
        std::vector<uint32_t> changed_input;
        Check(tools.Assemble(changed_assembly, &changed_input) && tools.Validate(changed_input),
              "neighboring independent SPIR-V fixture invalid");
        miss(changed_input, state);
        // Simulate a lookup-hash collision: filename alone cannot grant a hit.
        std::filesystem::copy_file(cache.EntryPath(input, state), cache.EntryPath(changed_input, state));
        miss(changed_input, state);
        const auto path = cache.EntryPath(input, state);
        {
            std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
            stream.seekp(-1, std::ios::end); char corrupt = '\x7f'; stream.write(&corrupt, 1);
        }
        miss(input, state);
        Check(cache.Store(input, state, oracle) && reopened.Load(input, state, observed) && observed == oracle,
              "corrupt cache entry cannot be replaced atomically");
        { std::ofstream stream(path, std::ios::binary | std::ios::trunc); stream << "short"; }
        miss(input, state);
        Check(cache.Store(input, state, oracle), "truncated cache entry cannot be repaired");
        SpirvOptimizationCache too_small(scratch / "budget", 16u);
        Check(!too_small.Store(input, state, oracle), "disk-byte budget exceeded");
        SpirvOptimizationCache single(scratch / "entries", 1024u * 1024u, 1u);
        Check(single.Store(input, state, oracle) && single.Store(input, state, oracle) &&
              !single.Store(changed_input, state, oracle), "entry budget or replacement accounting failed");
        const std::vector<uint32_t> too_big(SpirvOptimizationCache::MaxWords + 1u);
        Check(!cache.Store(too_big, state, oracle) && !cache.Store(input, state, too_big), "module size budget exceeded");
        std::ofstream blocker(scratch / "blocked"); blocker << "file"; blocker.close();
        SpirvOptimizationCache unavailable(scratch / "blocked");
        Check(!unavailable.Store(input, state, oracle), "unwritable cache path changed compile success");
        for (uint32_t mode : {1u, 2u}) for (bool cooperative : {false, true}) {
            auto variation = state; variation.mode = mode; variation.cooperative_wave64 = cooperative;
            Check(RunSpirvOptimizationRecipe(input, mode, cooperative, oracle, messages) && tools.Validate(oracle) &&
                  cache.Store(input, variation, oracle) && cache.Load(input, variation, observed) && observed == oracle,
                  "mode/cooperative neighboring recipe does not preserve exact result");
        }
        std::cout << "KYTY_SPIRV_OPTIMIZATION_CACHE_PASS optimizer_calls=" << optimizer_calls << '\n';
        // Keep this small, isolated scratch artifact for native verification records.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "optimizer cache test failed: " << error.what() << '\n';
        return 1;
    }
}
