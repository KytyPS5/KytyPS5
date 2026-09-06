#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using namespace Libs::Graphics;

void Progress(const char* phase) {
  std::printf("KYTY_SHADER_AUDIT_PHASE %s\n", phase);
  std::fflush(stdout);
}

uint64_t HexValue(const Json& value) {
  return value.is_string() ? std::stoull(value.get<std::string>(), nullptr, 16)
                           : value.get<uint64_t>();
}

template <typename T>
void ReadTriple(const Json& json, const char* name, T (&target)[3]) {
  const auto values = json.at(name).get<std::array<T, 3>>();
  std::copy(values.begin(), values.end(), target);
}
} // namespace

// One manifest per process: production fatal exits and timeouts stay isolated.
// This worker never reads captured guest addresses or executes guest GPU code.
int RunShaderBatchAudit(int argc, char* argv[]) {
  const char* phase = "input";
  try {
    if (argc != 3) {
      throw std::runtime_error("usage: shader_cfg_tests --audit-shader <manifest.json>");
    }
    const auto manifest_path = std::filesystem::u8path(argv[2]);
    std::ifstream manifest_file(manifest_path);
    const auto manifest = Json::parse(manifest_file);
    if (manifest.at("schema_version") != 1) {
      throw std::runtime_error("unsupported shader capture schema");
    }
    auto binary_path = manifest_path;
    binary_path.replace_extension(".bin");
    if (manifest.contains("code_file")) {
      const auto filename = std::filesystem::u8path(manifest.at("code_file").get<std::string>());
      if (filename.has_parent_path()) {
        throw std::runtime_error("code_file must be a sibling filename");
      }
      binary_path = manifest_path.parent_path() / filename;
    }
    const auto size = std::filesystem::file_size(binary_path);
    if (size == 0 || size % 4u != 0 || size > 64u * 1024u * 1024u) {
      throw std::runtime_error("invalid or oversized shader code capture");
    }
    std::vector<uint32_t> code(size / 4u);
    std::ifstream binary(binary_path, std::ios::binary);
    if (!binary.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(size))) {
      throw std::runtime_error("incomplete shader capture");
    }
    if (manifest.contains("content_hash_xxh3_64") &&
        XXH3_64bits(code.data(), size) != HexValue(manifest.at("content_hash_xxh3_64"))) {
      throw std::runtime_error("shader capture content hash mismatch");
    }

    static Common::Subsystems subsystems;
    Common::InitializeThreads();
    subsystems.Initialize<Config::Lifecycle>();
    Config::ConfigOptions config;
    config.printf_direction = Config::OutputDirection::Silent;
    Config::Load(config);
    subsystems.Initialize<Log::Lifecycle>();
    ShaderInit();

    const auto stage = manifest.at("stage").get<std::string>();
    Json result{{"status", "passed"}, {"stage", stage}, {"code_bytes", size},
                {"gpu_execution_checked", false}, {"resource_materialization_checked", false}};
    phase = "decode";
    Progress(phase);
    ShaderRecompiler::Decoder::Program decoded;
    ShaderRecompiler::Decoder::DecodeProgram(code, decoded);
    Json unknown = Json::array();
    Json unsupported = Json::array();
    for (const auto& inst: decoded.instructions) {
      if (inst.opcode == ShaderRecompiler::Decoder::Opcode::UNKNOWN ||
          inst.opcode == ShaderRecompiler::Decoder::Opcode::UNSUPPORTED) {
        unknown.push_back(inst.pc);
        unsupported.push_back({{"pc", inst.pc}, {"reason", inst.unsupported_reason},
                                {"family", static_cast<uint32_t>(inst.family)},
                                {"opcode_id", inst.opcode_id},
                                {"instruction", ShaderRecompiler::Decoder::InstructionToString(inst)}});
      }
    }
    result["instructions"] = decoded.instructions.size();
    result["unknown_instruction_pcs"] = unknown;
    result["unsupported_instructions"] = unsupported;
    if (!unknown.empty()) {
      result["status"] = "failed";
      result["checked_through"] = "decode";
      std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
      std::fflush(stdout);
      return 1;
    }
    phase = "input";
    const bool compute = stage == "compute" || stage == "cs";
    const bool header_profile = manifest.value("metadata_provenance", "") == "agc_header_profile";
    if (compute && (manifest.value("metadata_complete", false) || header_profile) && manifest.contains("compute")) {
      const auto& input = manifest.at("compute");
      ShaderComputeInputInfo info;
      ReadTriple(input, "threads_num", info.threads_num);
      ReadTriple(input, "dispatch_threads_num", info.dispatch_threads_num);
      ReadTriple(input, "group_id", info.group_id);
      info.lds_size_dwords = input.at("lds_size_dwords");
      info.scratch_size_dwords = input.at("scratch_size_dwords");
      info.dispatch_thread_dimensions = input.at("dispatch_thread_dimensions");
      info.needs_lds_barriers = input.at("needs_lds_barriers");
      info.wave_size = input.at("wave_size");
      info.thread_ids_num = input.at("thread_ids_num");
      info.workgroup_register = input.at("workgroup_register");
      info.tg_size_en = input.at("tg_size_en");
      const uint32_t user_count = manifest.at("user_data_count");
      const uint32_t user_base = manifest.at("user_data_base");
      const uint32_t wave_size = manifest.at("wave_size");
      if (user_count > 104u || user_base > 104u - user_count || wave_size != info.wave_size ||
          (info.wave_size != 32u && info.wave_size != 64u)) {
        throw std::runtime_error("invalid compute capture metadata");
      }
      // Translation needs the register count, not runtime descriptor payloads.
      std::vector<uint32_t> user_data(user_count);
      ShaderRecompiler::CompileOptions options;
      options.stage = ShaderType::Compute;
      options.wave_size = wave_size;
      options.user_data_base = user_base;
      options.user_data = user_data;
      options.shader_hash = manifest.contains("shader_hash") ? HexValue(manifest.at("shader_hash"))
                            : XXH3_64bits(code.data(), size);
      options.scratch_dwords = manifest.at("scratch_dwords");
      options.dump_ir = false;
      options.input_info.compute = &info;
      Json profiles = Json::array();
      const bool initial_barriers = info.needs_lds_barriers;
      for (unsigned variant = 0; variant < (header_profile ? 2u : 1u); variant++) {
        info.needs_lds_barriers = variant == 0u ? initial_barriers : !initial_barriers;
        phase = "translate_resources";
        Progress(header_profile ? (info.needs_lds_barriers ? "translate_profile_barriers_on" : "translate_profile_barriers_off") : phase);
        auto translated = ShaderRecompiler::TranslateProgram(code, options);
        const auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
        profiles.push_back({{"needs_lds_barriers", info.needs_lds_barriers},
                            {"blocks", translated.program.blocks.size()},
                            {"images", plan.info.images.size()}, {"samplers", plan.info.samplers.size()},
                            {"requires_specialization_memory", plan.requires_specialization_memory}});
      }
      result["checked_through"] = "resource_tracking";
      result["profiles_checked"] = profiles;
      result["metadata_provenance"] = header_profile ? "agc_header_profile" : "dispatched";
      result["runtime_context_complete"] = !header_profile;
      if (header_profile) {
        result["profile_assumptions"] = manifest.value("profile_assumptions", Json::object());
      }
    } else {
        phase = "cfg";
        Progress(phase);
        auto graph = ShaderRecompiler::CFG::BuildGraph(decoded);
        const bool structured = !graph.unsupported && ShaderRecompiler::CFG::Structurize(graph);
        result["checked_through"] = "cfg";
        result["blocks"] = graph.blocks.size();
        result["dispatcher_fallback_required"] = !structured;
        result["translation_skipped_reason"] = "complete stage runtime metadata was not captured";
    }
    std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
    std::fflush(stdout);
    return result.at("status") == "passed" ? 0 : 1;
  } catch (const std::exception& error) {
    const Json result{{"status", std::string_view(phase) == "input" ? "input_error" : "failed"},
                       {"phase", phase}, {"error", error.what()}};
    std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
    return 2;
  }
}
