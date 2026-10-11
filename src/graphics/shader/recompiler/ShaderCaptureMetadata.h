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
enum class CompileCaptureEvent { Translate, Emit, CachedReuse, MaterializationFailure };
struct CompileCaptureLimits {
    size_t compiler_inputs = 4096u, runtime_inputs = 4089u, failure_inputs = 7u;
    uint64_t compiler_bytes = 128u * 1024u * 1024u;
    uint64_t runtime_bytes = 64u * 1024u * 1024u, failure_bytes = 64u * 1024u * 1024u;
};
// Shared diagnostic admission used by ProgramCache and the CPU-only capture tests.
// A cached success is not a generation attempt and consumes no persistent quota.
class CompileCaptureCoordinator {
public:
    explicit CompileCaptureCoordinator(const CompileCaptureLimits& limits = {})
        : compiler(limits.compiler_inputs, limits.compiler_bytes),
          runtime(limits.runtime_inputs, limits.runtime_bytes),
          failures(limits.failure_inputs, limits.failure_bytes) {}
    std::optional<CompileCaptureAttempt> Capture(
        CompileCaptureEvent event, const std::filesystem::path& directory,
        const ShaderParams& params, const CompileOptions& options,
        std::span<const uint32_t> static_state,
        std::optional<std::array<uint32_t, 3>> guest_workgroups,
        std::string_view revision, std::string_view worktree,
        const nlohmann::ordered_json* runtime_capture,
        const nlohmann::ordered_json& compile_policy, uint32_t push_data_start_dword);
    [[nodiscard]] nlohmann::ordered_json Summary() const;
private:
    CompileCaptureArchive compiler, runtime, failures;
    uint64_t translate_requests = 0, emit_requests = 0, cached_reuses = 0, failure_requests = 0;
    uint64_t compiler_incomplete = 0, runtime_incomplete = 0, failure_incomplete = 0;
};
// Existing independent snapshot budget: one selected success and seven failures.
class ResourceCaptureAdmission {
public:
    std::optional<uint32_t> Acquire(bool failed) {
        if (failed) {
            if (failures < 7u) return ++failures;
            ++failures_omitted; return {};
        }
        if (successes != 0u) { ++successes_omitted; return {}; }
        ++successes;
        return 0u;
    }
    [[nodiscard]] nlohmann::ordered_json Summary() const {
        return {{"successes", successes}, {"failures", failures},
                {"successes_omitted", successes_omitted}, {"failures_omitted", failures_omitted}};
    }
private:
    uint32_t successes = 0, failures = 0;
    uint64_t successes_omitted = 0, failures_omitted = 0;
};
nlohmann::ordered_json BuildCompileCaptureMetadata(
    const ShaderParams& params, const CompileOptions& options,
    std::span<const uint32_t> static_state,
    std::optional<std::array<uint32_t, 3>> guest_workgroups,
    const nlohmann::ordered_json* runtime_capture,
    const nlohmann::ordered_json& compile_policy, uint32_t push_data_start_dword);
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
