#include "graphics/shader/recompiler/backend/spirv/SpirvOptimizer.h"

#include <spirv-tools/libspirv.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using Config::ShaderOptimizationType;
using Libs::Graphics::ShaderRecompiler::Spirv::Optimize;

void Check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "ShaderOptimizerTests: %s\n", message);
    std::abort();
  }
}

const char *ComputeSource = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %buffer %unused
OpExecutionMode %main LocalSize 1 1 1
OpDecorate %block Block
OpMemberDecorate %block 0 Offset 0
OpDecorate %buffer DescriptorSet 0
OpDecorate %buffer Binding 3
OpDecorate %unused DescriptorSet 0
OpDecorate %unused Binding 9
OpDecorate %special SpecId 7
OpDecorate %unused_special SpecId 8
%void = OpTypeVoid
%uint = OpTypeInt 32 0
%bool = OpTypeBool
%block = OpTypeStruct %uint
%ptr_block = OpTypePointer StorageBuffer %block
%ptr_uint = OpTypePointer StorageBuffer %uint
%buffer = OpVariable %ptr_block StorageBuffer
%unused = OpVariable %ptr_block StorageBuffer
%fn = OpTypeFunction %void
%helper_fn = OpTypeFunction %uint %uint
%zero = OpConstant %uint 0
%one = OpConstant %uint 1
%semantics = OpConstant %uint 72
%true = OpConstantTrue %bool
%special = OpSpecConstant %uint 42
%unused_special = OpSpecConstant %uint 19
%helper = OpFunction %uint None %helper_fn
%arg = OpFunctionParameter %uint
%helper_entry = OpLabel
%sum = OpIAdd %uint %arg %zero
OpReturnValue %sum
OpFunctionEnd
%dead_helper = OpFunction %void None %fn
%dead_entry = OpLabel
OpReturn
OpFunctionEnd
%main = OpFunction %void None %fn
%entry = OpLabel
OpSelectionMerge %merge None
OpBranchConditional %true %yes %no
%yes = OpLabel
%value = OpFunctionCall %uint %helper %special
%ptr = OpAccessChain %ptr_uint %buffer %zero
OpStore %ptr %value
%atomic = OpAtomicIAdd %uint %ptr %one %zero %one
OpMemoryBarrier %one %semantics
OpBranch %merge
%no = OpLabel
%other = OpAccessChain %ptr_uint %buffer %zero
OpStore %other %zero
OpBranch %merge
%merge = OpLabel
OpReturn
OpFunctionEnd
)";

const char *FragmentSource = R"(
OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %color %extra
OpExecutionMode %main OriginUpperLeft
OpDecorate %color Location 0
OpDecorate %extra Location 1
%void = OpTypeVoid
%float = OpTypeFloat 32
%vec4 = OpTypeVector %float 4
%ptr_out = OpTypePointer Output %vec4
%color = OpVariable %ptr_out Output
%extra = OpVariable %ptr_out Output
%fn = OpTypeFunction %void
%one = OpConstant %float 1
%white = OpConstantComposite %vec4 %one %one %one %one
%main = OpFunction %void None %fn
%entry = OpLabel
OpStore %color %white
OpReturn
OpFunctionEnd
)";

std::vector<uint32_t> Assemble(const char *source) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
  tools.SetMessageConsumer(
      [](spv_message_level_t, const char *, const spv_position_t &,
         const char *message) { std::fprintf(stderr, "%s\n", message); });
  std::vector<uint32_t> code;
  Check(tools.Assemble(source, &code), "could not assemble fixture");
  Check(tools.Validate(code), "fixture is invalid");
  return code;
}

std::string Disassemble(const std::vector<uint32_t> &code) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_3);
  Check(tools.Validate(code), "optimized shader is invalid");
  std::string text;
  Check(tools.Disassemble(code, &text), "could not disassemble shader");
  return text;
}

} // namespace

int main() {
  const auto original = Assemble(ComputeSource);
  std::string diagnostics;
  auto unchanged = original;
  Check(Optimize(unchanged, ShaderOptimizationType::None, diagnostics),
        "None failed");
  Check(unchanged == original, "None changed shader bytes");
  for (const auto mode :
       {ShaderOptimizationType::Size, ShaderOptimizationType::Performance}) {
    auto optimized = original;
    const auto begin = std::chrono::steady_clock::now();
    Check(Optimize(optimized, mode, diagnostics), diagnostics.c_str());
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    Check(optimized.size() < original.size(),
          "constant branch was not reduced");
    const auto text = Disassemble(optimized);
    for (const auto *expected :
         {"Binding 3", "Binding 9", "SpecId 7", "SpecId 8", "OpFunctionCall",
          "OpAtomicIAdd", "OpMemoryBarrier"}) {
      Check(text.find(expected) != std::string::npos, expected);
    }
    Check(text.find("OpBranchConditional") == std::string::npos,
          "dead branch survived");
    std::printf("%s fixture: %zu -> %zu words, %lld us\n",
                mode == ShaderOptimizationType::Size ? "Size" : "Performance",
                original.size(), optimized.size(),
                static_cast<long long>(elapsed));
    auto fragment = Assemble(FragmentSource);
    Check(Optimize(fragment, mode, diagnostics), diagnostics.c_str());
    const auto fragment_text = Disassemble(fragment);
    Check(fragment_text.find("Location 0") != std::string::npos,
          "fragment output removed");
    Check(fragment_text.find("Location 1") != std::string::npos,
          "unused stage output removed");

    for (size_t size = 0; size < original.size(); ++size) {
      auto broken =
          std::vector<uint32_t>(original.begin(), original.begin() + size);
      const auto copy = broken;
      Check(!Optimize(broken, mode, diagnostics), "truncated shader accepted");
      Check(broken == copy, "failure changed the original bytes");
      Check(!diagnostics.empty(), "failure lacks diagnostic");
    }
  }
  auto invalid_mode = original;
  Check(!Optimize(invalid_mode, static_cast<ShaderOptimizationType>(99),
                  diagnostics),
        "unknown mode accepted");
  Check(invalid_mode == original, "unknown mode changed input");
  std::puts("ShaderOptimizerTests: ok");
}
