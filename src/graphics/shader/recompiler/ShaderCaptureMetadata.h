#pragma once
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/shaderCompiler.h"
#include "graphics/shader/recompiler/ResourceReadCapture.h"

#include <nlohmann/json.hpp>
#include <filesystem>
namespace Libs::Graphics::ShaderRecompiler::Capture {
// A pending phase records an attempted operation, never a proven compiler failure.
struct CompileCaptureAttempt {
    std::filesystem::path manifest;
    nlohmann::ordered_json metadata;
};
class CompileCaptureArchive {
public:
    explicit CompileCaptureArchive(size_t max_inputs = 4096u,
                                   uint64_t max_bytes = 256u * 1024u * 1024u)
        : max_inputs(max_inputs), max_bytes(max_bytes) {}
    std::optional<CompileCaptureAttempt> Begin(
        const std::filesystem::path& directory, const ShaderParams& params,
        const CompileOptions& options, std::span<const uint32_t> static_state,
        std::optional<std::array<uint32_t, 3>> guest_workgroups,
        std::string_view revision = {}, std::string_view worktree = {},
        const nlohmann::ordered_json* runtime_capture = nullptr,
        const nlohmann::ordered_json* compile_policy = nullptr,
        uint32_t push_data_start_dword = 0u);
    static bool Update(CompileCaptureAttempt& attempt, std::string_view phase,
                       std::string_view phase_status, std::string_view reason = {});
    [[nodiscard]] size_t Inputs() const { return entries.size(); }
    [[nodiscard]] uint64_t Bytes() const { return bytes; }
private:
    struct Entry {
        std::filesystem::path directory;
        std::string identity;
        std::vector<uint32_t> code, back_code;
        CompileCaptureAttempt attempt;
    };
    size_t max_inputs;
    uint64_t max_bytes, bytes = 0u;
    std::vector<Entry> entries;
};
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
nlohmann::ordered_json CaptureTextureInputs(const IR::ResourcePlan& plan,
                                           const IR::SrtRuntime& runtime, uint32_t logical);
// Diagnostic analysis of captured logical texels. No draw-time ownership or
// selector-provenance proof is implied; padding is deliberately excluded.
bool CollectCapturedTextureBytes(const ShaderTextureResource& texture,
                                 std::span<const uint32_t> words, uint32_t channels,
                                 std::vector<uint32_t>& values);
} // namespace Libs::Graphics::ShaderRecompiler::Capture
