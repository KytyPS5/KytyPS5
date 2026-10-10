#pragma once
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "graphics/shader/recompiler/ResourceReadCapture.h"

#include <nlohmann/json.hpp>
namespace Libs::Graphics::ShaderRecompiler::Capture {
nlohmann::ordered_json
     BuildCompileMetadata(const ShaderParams& params, const CompileOptions& options,
                          std::span<const uint32_t>              static_state,
                          std::optional<std::array<uint32_t, 3>> guest_workgroups);
void ReadPixelInputMetadata(const nlohmann::ordered_json& input, ShaderPixelInputInfo& result);
void ReadComputeInputMetadata(const nlohmann::ordered_json& input, ShaderComputeInputInfo& result);
void ReadHostProfileMetadata(const nlohmann::ordered_json& input, ShaderHostProfile& result);
void ReadWorkgroupLimitsMetadata(const nlohmann::ordered_json& input, ComputeWorkgroupLimits& result);
nlohmann::ordered_json BuildResourceReadMetadata(const IR::SrtRuntime& runtime,
                                               const ResourceReadCapture& capture,
                                               bool succeeded, std::string_view reason);
std::vector<ReadEvent> ReadResourceReadMetadata(const nlohmann::ordered_json& input,
                                              IR::SrtRuntime& runtime);
std::vector<uint32_t> ReadCapturedUserData(const nlohmann::ordered_json& manifest);
} // namespace Libs::Graphics::ShaderRecompiler::Capture
