#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/ShaderCaptureMetadata.h"
#include "graphics/shader/recompiler/ResourceReadReplay.h"
#include "graphics/shader/recompiler/ResourceReadSnapshot.h"
#include "graphics/shader/recompiler/ComputeExecution.h"
#include "graphics/shader/recompiler/frontend/cfg/ShaderCFG.h"
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
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

ShaderFloatingPointState ReadInitialFloatingPointState(const Json& input) {
  if (!input.contains("initial_fp_state")) {
    return {}; // Legacy manifests did not capture these registers.
  }
  const auto& state = input.at("initial_fp_state");
  if (!state.is_object() || !state.contains("known") || !state.at("known").is_boolean()) {
    throw std::runtime_error("initial_fp_state requires an object with boolean known");
  }
  const bool known = state.at("known").get<bool>();
  for (const char* field : {"ieee_mode", "dx10_clamp"}) {
    if ((known || state.contains(field)) &&
        (!state.contains(field) || !state.at(field).is_boolean())) {
      throw std::runtime_error(std::string("initial_fp_state requires boolean ") + field);
    }
  }
  if (known || state.contains("float_mode")) {
    if (!state.contains("float_mode") || !state.at("float_mode").is_number_integer() ||
        state.at("float_mode") < 0 || state.at("float_mode") > 255) {
      throw std::runtime_error("initial_fp_state.float_mode must be an integer in [0,255]");
    }
  }
  if (!known) {
    return {}; // Validate supplied fields, then canonicalize the unknown state.
  }
  return {true, state.at("float_mode").get<uint8_t>(), state.at("ieee_mode").get<bool>(),
          state.at("dx10_clamp").get<bool>()};
}

uint32_t ReadPositiveHostLimit(const Json& value, const char* name) {
  if (!value.is_number_integer() || value <= 0 || value > UINT32_MAX) {
    throw std::runtime_error(std::string("compute host profile requires positive U32 ") + name);
  }
  return value.get<uint32_t>();
}

ShaderRecompiler::ComputeWorkgroupLimits ReadComputeHostProfile(const Json& input) {
  if (!input.is_object() || input.at("schema_version") != 1 ||
      !input.at("max_size").is_array() || input.at("max_size").size() != 3 ||
      !input.at("can_require_subgroup_size_64").is_boolean()) {
    throw std::runtime_error("invalid compute host profile schema");
  }
  ShaderRecompiler::ComputeWorkgroupLimits limits;
  for (size_t axis = 0; axis < 3; ++axis) {
    limits.max_size[axis] = ReadPositiveHostLimit(input.at("max_size").at(axis), "max_size");
  }
  limits.max_invocations = ReadPositiveHostLimit(input.at("max_invocations"), "max_invocations");
  limits.native_subgroup_size = ReadPositiveHostLimit(input.at("native_subgroup_size"), "native_subgroup_size");
  limits.max_shared_memory_bytes = ReadPositiveHostLimit(input.at("max_shared_memory_bytes"), "max_shared_memory_bytes");
  limits.can_require_subgroup_size_64 = input.at("can_require_subgroup_size_64").get<bool>();
  return limits;
}

Json PrecheckComputeExecution(ShaderRecompiler::IR::Program& program,
                             ShaderStageInputInfo input,
                             const ShaderRecompiler::ComputeWorkgroupLimits& limits) {
  namespace IR = ShaderRecompiler::IR;
  // This is not AnalyzeProgramRequirements: bounded tables do not have dense
  // candidates until materialization. Feed only the fields consumed by the
  // pure execution planner, without fabricating any resource specialization.
  IR::SpirvRequirements requirements{};
  for (const auto* block : program.blocks) for (const auto& inst : *block) {
    requirements.compute_derivatives |= inst.GetOpcode() == IR::ValueOpcode::ImageQueryLod;
  }
  const bool unknown_add_tid = std::ranges::any_of(program.memory_info, [](const auto& memory) {
    return memory.kind == IR::ResourceKind::Buffer && !memory.planning_only;
  });
  Json attempts = Json::array();
  Json errors = Json::array();
  bool all_rejected = true;
  const auto saved_requirements = program.spirv_requirements;
  for (unsigned variant = 0; variant < (unknown_add_tid ? 2u : 1u); ++variant) {
    // Existing wave operations are also inspected directly by the planner.
    // ADD_TID is the remaining descriptor-dependent source of LaneId demand.
    requirements.subgroup_local_invocation_id = variant != 0u;
    program.spirv_requirements = requirements;
    const auto plan = ShaderRecompiler::PlanComputeExecution(program, input, limits);
    all_rejected &= !plan.error.empty();
    if (!plan.error.empty() && std::ranges::find(errors, Json(plan.error)) == errors.end()) {
      errors.push_back(plan.error);
    }
    attempts.push_back({{"assumed_buffer_add_tid", variant != 0u},
                        {"status", plan.error.empty() ? "not_rejected" : "rejected"},
                        {"error", plan.error}, {"guest_size", plan.layout.guest_size},
                        {"host_size", plan.layout.host_size},
                        {"wave_partition_factor", plan.wave_partition_factor},
                        {"split_wave64", plan.IsSplitWave64()},
                        {"cooperative_wave64", plan.IsCooperativeWave64()}});
  }
  program.spirv_requirements = saved_requirements;
  Json unknown = Json::array();
  if (unknown_add_tid) unknown.push_back("buffer ADD_TID before descriptor materialization");
  return {{"status", all_rejected ? "rejected" : "not_rejected"},
          {"errors", all_rejected ? errors : Json::array()},
          {"possible_errors", errors}, {"requirements_variants", attempts},
          {"unknown_requirements", unknown}, {"input_phase", "pre_specialization"},
          {"post_specialization_checked", false}, {"spirv_emitted", false}};
}

uint64_t SnapshotUnsigned(const Json& value, uint64_t limit) {
  uint64_t result;
  if (value.is_number_unsigned()) result = value.get<uint64_t>();
  else if (value.is_number_integer() && value.get<int64_t>() >= 0)
    result = static_cast<uint64_t>(value.get<int64_t>());
  else throw std::runtime_error("snapshot metadata requires an unsigned integer");
  if (result > limit) throw std::runtime_error("snapshot metadata integer exceeds range");
  return result;
}

bool SnapshotBoolean(const Json& value) {
  if (!value.is_boolean()) throw std::runtime_error("snapshot metadata requires a boolean");
  return value.get<bool>();
}

Json ImportTextureSnapshot(const Json& runtime, const ShaderRecompiler::IR::ResourcePlan& plan,
                          ShaderRecompiler::Capture::ResourceReadSnapshot& snapshot) {
  Json result{{"supplemental_texture_entries", 0}, {"supplemental_texture_entries_skipped", 0},
              {"ownership_proof_from_texture_capture", false}};
  if (!runtime.contains("additional_texture_inputs")) return result;
  const auto& input = runtime.at("additional_texture_inputs");
  if (!input.is_object() || input.at("kind") != "diagnostic_texture_inputs")
    throw std::runtime_error("unsupported supplemental texture snapshot");
  result["capture_claims_specialization_proof"] = SnapshotBoolean(input.at("specialization_proof"));
  const auto logical = SnapshotUnsigned(input.at("logical_image"), UINT32_MAX);
  if (logical >= plan.info.images.size()) throw std::runtime_error("snapshot logical image is unavailable");
  const auto source_index = plan.info.images[static_cast<size_t>(logical)].source;
  if (source_index >= plan.descriptor_sources.size()) throw std::runtime_error("snapshot descriptor source is unavailable");
  const auto& source = plan.descriptor_sources[source_index];
  if (!source.inline_descriptor || source.inline_descriptor->image_table || source.inline_descriptor->address_table)
    throw std::runtime_error("snapshot requires a direct inline image descriptor");
  const auto descriptor_words = source.inline_descriptor->descriptor_dwords;
  if (descriptor_words != 4u && descriptor_words != 8u)
    throw std::runtime_error("snapshot image descriptor width is unsupported");
  const auto& entries = input.at("entries");
  if (!entries.is_array() || entries.size() > 4096u)
    throw std::runtime_error("snapshot texture entry count exceeds bound");
  size_t imported = 0, skipped = 0;
  for (const auto& entry : entries) {
    const bool captured = SnapshotBoolean(entry.at("payload_captured"));
    const bool stable = SnapshotBoolean(entry.at("descriptor_stable"));
    if (!captured || !stable) { ++skipped; continue; }
    const auto address = SnapshotUnsigned(entry.at("address"), UINT64_MAX);
    const auto byte_count = SnapshotUnsigned(entry.at("bytes"), ShaderRecompiler::Capture::ResourceReadSnapshot::ByteLimit);
    const auto descriptor_address = SnapshotUnsigned(entry.at("descriptor_address"), UINT64_MAX);
    const auto& descriptor = entry.at("descriptor");
    const auto& payload = entry.at("words");
    if (!descriptor.is_array() || descriptor.size() != 8u || !payload.is_array() ||
        byte_count % 4u || payload.size() != byte_count / 4u)
      throw std::runtime_error("invalid supplemental snapshot payload");
    ShaderTextureResource texture;
    for (size_t word = 0; word < 8u; ++word)
      texture.fields[word] = static_cast<uint32_t>(SnapshotUnsigned(descriptor.at(word), UINT32_MAX));
    if (texture.Base40() != address) throw std::runtime_error("snapshot texture address differs from descriptor");
    std::vector<uint32_t> words(payload.size());
    for (size_t word = 0; word < words.size(); ++word)
      words[word] = static_cast<uint32_t>(SnapshotUnsigned(payload.at(word), UINT32_MAX));
    // Compact image SRDs have only four captured words. The remaining array
    // entries are diagnostic defaults, never evidence of additional memory.
    snapshot.AddStrictWords(descriptor_address, std::span(texture.fields).first(descriptor_words));
    snapshot.AddStrictWords(address, words);
    ++imported;
  }
  result["supplemental_texture_entries"] = imported;
  result["supplemental_texture_entries_skipped"] = skipped;
  return result;
}

Json TextureRowProofMetadata(const ShaderRecompiler::IR::DescriptorSource::InlineDescriptor::AddressTable& table) {
  if (!table.texture_row_domain) return nullptr;
  Json origins = Json::array();
  for (const auto& origin : table.texture_row_domain->images)
    origins.push_back({{"logical_image", origin.image}, {"channel_mask", origin.channels}});
  return {{"images", std::move(origins)}, {"constants", table.texture_row_domain->constants}};
}

Json DescribeCapturedTextureRows(const ShaderRecompiler::IR::ResourcePlan& plan, const Json& runtime) {
  struct ObservedTexture {
    uint32_t image, selector;
    ShaderTextureResource texture;
    std::vector<uint32_t> words;
  };
  std::vector<ObservedTexture> textures;
  if (runtime.contains("additional_texture_inputs")) {
    const auto& input = runtime.at("additional_texture_inputs");
    if (input.at("kind") != "diagnostic_texture_inputs")
      throw std::runtime_error("unsupported texture row diagnostic capture");
    const auto logical = static_cast<uint32_t>(SnapshotUnsigned(input.at("logical_image"), UINT32_MAX));
    const auto& entries = input.at("entries");
    if (!entries.is_array() || entries.size() > 4096u)
      throw std::runtime_error("texture row diagnostic entry count exceeds bound");
    size_t remaining_bytes = ShaderRecompiler::Capture::ResourceReadSnapshot::ByteLimit;
    for (const auto& entry : entries) {
      if (!SnapshotBoolean(entry.at("payload_captured")) || !SnapshotBoolean(entry.at("descriptor_stable"))) continue;
      const auto& descriptor = entry.at("descriptor");
      const auto& words = entry.at("words");
      const auto byte_count = SnapshotUnsigned(entry.at("bytes"), remaining_bytes);
      if (!descriptor.is_array() || descriptor.size() != 8u || !words.is_array() ||
          byte_count % 4u || words.size() != byte_count / 4u)
        throw std::runtime_error("invalid texture row diagnostic payload");
      remaining_bytes -= static_cast<size_t>(byte_count);
      ObservedTexture observed{};
      observed.image = logical;
      observed.selector = static_cast<uint32_t>(SnapshotUnsigned(entry.at("selector_key"), UINT32_MAX));
      for (size_t word = 0; word < 8u; ++word)
        observed.texture.fields[word] = static_cast<uint32_t>(SnapshotUnsigned(descriptor.at(word), UINT32_MAX));
      if (observed.texture.Base40() != SnapshotUnsigned(entry.at("address"), UINT64_MAX))
        throw std::runtime_error("texture row diagnostic address differs from descriptor");
      observed.words.resize(words.size());
      for (size_t word = 0; word < words.size(); ++word)
        observed.words[word] = static_cast<uint32_t>(SnapshotUnsigned(words.at(word), UINT32_MAX));
      textures.push_back(std::move(observed));
    }
  }
  // Repeated wave-table offsets share the same captured byte-domain analysis.
  // A union describes observations only; it does not grant runtime ownership
  // or imply that uncaptured descriptor candidates have matching contents.
  std::map<std::pair<uint32_t, uint32_t>, Json> domains;
  Json tables = Json::array();
  for (uint32_t logical = 0; logical < plan.info.images.size(); ++logical) {
    const auto& image = plan.info.images[logical];
    if (image.source >= plan.descriptor_sources.size()) continue;
    const auto& source = plan.descriptor_sources[image.source];
    if (!source.inline_descriptor || !source.inline_descriptor->address_table) continue;
    const auto& table = *source.inline_descriptor;
    const auto& address = *table.address_table;
    Json item{{"logical_image", logical}, {"source", image.source}, {"pc", image.first_use_pc},
              {"descriptor_offset", table.descriptor_offset}, {"pointer_offset", address.pointer_offset},
              {"row_stride", address.row_stride}, {"row_limit", address.row_limit},
              {"texture_row_domain", TextureRowProofMetadata(address)},
              {"specialization_proof", false}, {"ownership_verified", false},
              {"observation_scope", "captured_texture_entries_only"}};
    if (address.texture_row_domain) {
      Json contents = Json::array();
      std::vector<uint32_t> rows = address.texture_row_domain->constants;
      for (const auto& origin : address.texture_row_domain->images) {
        const auto key = std::pair{origin.image, origin.channels};
        auto found = domains.find(key);
        if (found == domains.end()) {
          Json entries = Json::array();
          std::vector<uint32_t> values;
          for (const auto& observed : textures) {
            if (observed.image != origin.image) continue;
            std::vector<uint32_t> entry_values;
            const bool valid = ShaderRecompiler::Capture::CollectCapturedTextureBytes(
                observed.texture, observed.words, origin.channels, entry_values);
            entries.push_back({{"selector_key", observed.selector}, {"layout_and_channels_supported", valid},
                               {"observed_values", entry_values}});
            if (valid) values.insert(values.end(), entry_values.begin(), entry_values.end());
          }
          std::ranges::sort(values);
          values.erase(std::unique(values.begin(), values.end()), values.end());
          found = domains.emplace(key, Json{{"logical_image", origin.image}, {"channel_mask", origin.channels},
              {"captured_entries", std::move(entries)}, {"observed_union", values}}).first;
        }
        contents.push_back(found->second);
        const auto values = found->second.at("observed_union").get<std::vector<uint32_t>>();
        rows.insert(rows.end(), values.begin(), values.end());
      }
      std::ranges::sort(rows);
      rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
      item["captured_content_domains"] = std::move(contents);
      item["observed_rows_with_constants"] = rows;
      std::erase_if(rows, [&](uint32_t row) { return row >= address.row_limit; });
      item["observed_rows_below_guard"] = std::move(rows);
    }
    tables.push_back(std::move(item));
  }
  return tables;
}
} // namespace

// One manifest per process: production fatal exits and timeouts stay isolated.
// This worker never reads captured guest addresses or executes guest GPU code.
int RunShaderBatchAudit(int argc, char* argv[]) {
  const char* phase = "input";
  const bool snapshot_mode = argc > 1 && std::string_view(argv[1]) == "--audit-shader-snapshot";
  Json snapshot_diagnostics = Json::object();
  try {
    if (argc != 3 && !(argc == 5 && std::string_view(argv[3]) == "--compute-host-profile")) {
      throw std::runtime_error("usage: shader_cfg_tests --audit-shader <manifest.json> [--compute-host-profile <host.json>] or --audit-shader-snapshot <pixel-manifest.json>");
    }
    if (snapshot_mode && argc != 3) throw std::runtime_error("snapshot replay requires an exact pixel capture");
    std::optional<ShaderRecompiler::ComputeWorkgroupLimits> host_limits;
    Json host_profile;
    if (argc == 5) {
      std::ifstream host_file(std::filesystem::u8path(argv[4]));
      host_profile = Json::parse(host_file);
      host_limits = ReadComputeHostProfile(host_profile);
    }
    const auto manifest_path = std::filesystem::u8path(argv[2]);
    std::ifstream manifest_file(manifest_path);
    const auto manifest = Json::parse(manifest_file);
    const uint32_t capture_schema = manifest.at("schema_version");
    if (capture_schema != 1u && capture_schema != 2u) {
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
    config.printf_direction = Config::LogDirection::Silent;
    Config::Load(config);
    subsystems.Initialize<Log::Lifecycle>();
    ShaderInit();

    const auto stage = manifest.at("stage").get<std::string>();
    if (snapshot_mode && (capture_schema != 2u || (stage != "pixel" && stage != "ps") || !manifest.contains("runtime")))
      throw std::runtime_error("snapshot replay requires schema2 pixel runtime inputs");
    Json result{{"status", "passed"}, {"stage", stage}, {"code_bytes", size},
                {"gpu_execution_checked", false}, {"resource_materialization_checked", false}};
    if (host_limits) {
      result["compute_host_profile"] = host_profile;
      result["runtime_descriptor_values_captured"] = false;
      result["compute_execution_precheck"] = {
          {"status", "not_checked"}, {"reason", "translation did not reach compute resource tracking"},
          {"post_specialization_checked", false}, {"spirv_emitted", false}};
    }
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
    if (capture_schema == 2u && (stage == "pixel" || stage == "ps")) {
      if (!manifest.value("metadata_complete", false) || !manifest.contains("pixel"))
        throw std::runtime_error("complete pixel compiler inputs required");
      ShaderPixelInputInfo info;
      ShaderRecompiler::Capture::ReadPixelInputMetadata(manifest.at("pixel"), info);
      const uint32_t user_count = manifest.at("user_data_count");
      const uint32_t user_base = manifest.at("user_data_base");
      const uint32_t wave = manifest.at("wave_size");
      auto user_data = ShaderRecompiler::Capture::ReadCapturedUserData(manifest);
      if (user_count > 104u || user_base > 104u - user_count || user_data.size() != user_count || wave != info.wave_size)
        throw std::runtime_error("invalid pixel capture userdata or wave");
      ShaderRecompiler::CompileOptions options;
      options.stage = ShaderType::Pixel; options.wave_size = wave;
      options.user_data = user_data; options.user_data_base = user_base;
      options.shader_hash = HexValue(manifest.at("shader_hash"));
      options.input_info.pixel = &info;
      const bool dump_pixel_ir = std::getenv("KYTY_SHADER_AUDIT_DUMP_IR") != nullptr;
      options.dump_ir = dump_pixel_ir;
      options.early_dump = dump_pixel_ir;
      ShaderRecompiler::Capture::ReadHostProfileMetadata(manifest.at("host_profile"), options.host_profile);
      if (manifest.contains("compute_workgroup_limits"))
        ShaderRecompiler::Capture::ReadWorkgroupLimitsMetadata(manifest.at("compute_workgroup_limits"), options.compute_workgroup_limits);
      // Early local schema2 PS captures predate the device-limits field. The
      // pixel frontend does not consume native_subgroup_size; no GPU is replayed.
      result["compiler_device_limits_captured"] = manifest.contains("compute_workgroup_limits");
      if (manifest.value("back_code_size_bytes", 0u) != 0u)
        throw std::runtime_error("pixel back-code replay pending; refusing incomplete input");
      phase = "translate_resources"; Progress(phase);
      auto translated = ShaderRecompiler::TranslateProgram(code, options);
      if (dump_pixel_ir) {
        std::printf("KYTY_SHADER_AUDIT_PIXEL_IR_BEGIN\n%sKYTY_SHADER_AUDIT_PIXEL_IR_END\n",
                    ShaderRecompiler::IR::ProgramToString(translated.program).c_str());
        std::fflush(stdout);
      }
      const auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
      if (dump_pixel_ir) {
        Json images = Json::array();
        for (uint32_t logical = 0; logical < plan.info.images.size(); ++logical) {
          const auto& image = plan.info.images[logical];
          Json item{{"logical", logical}, {"source", image.source}, {"pc", image.first_use_pc}};
          if (image.source < plan.descriptor_sources.size()) {
            const auto& source = plan.descriptor_sources[image.source];
            if (source.inline_descriptor) {
              const auto& table = *source.inline_descriptor;
              item["inline"] = {{"buffer_source", table.buffer_source}, {"stride", table.selector_stride},
                                 {"offset", table.descriptor_offset}, {"limit", table.selector_limit},
                                 {"dwords", table.descriptor_dwords}};
              item["inline"]["static_selector_values"] = table.selector_static_values
                  ? Json(*table.selector_static_values) : Json(nullptr);
              if (table.address_table) {
                const auto& address = *table.address_table;
                item["address_table"] = {{"pointer_offset", address.pointer_offset},
                    {"row_stride", address.row_stride}, {"row_limit", address.row_limit},
                    {"row_key_arg", address.row_key_arg},
                    {"texture_row_domain", TextureRowProofMetadata(address)}};
              }
            }
          }
          images.push_back(std::move(item));
        }
        std::printf("KYTY_SHADER_AUDIT_PIXEL_IMAGE_PLAN %s\n", images.dump().c_str());
        std::fflush(stdout);
      }
      result["metadata_complete"] = true;
      result["checked_through"] = "pixel_translate_resources";
      result["runtime_resources_captured"] = manifest.value("runtime_resources_captured", false);
      result["tracked_images"] = plan.info.images.size();
      result["instructions"] = decoded.instructions.size();
      if (snapshot_mode) {
        phase = "input";
        result["texture_wave_tables"] = DescribeCapturedTextureRows(plan, manifest.at("runtime"));
        snapshot_diagnostics["texture_wave_tables"] = result.at("texture_wave_tables");
        std::printf("KYTY_SHADER_AUDIT_TEXTURE_WAVE_TABLES %s\n", result.at("texture_wave_tables").dump().c_str());
        std::fflush(stdout);
      }
      if (manifest.contains("runtime")) {
        phase = "input";
        ShaderRecompiler::IR::SrtRuntime runtime;
        runtime.user_data = user_data;
        auto events = ShaderRecompiler::Capture::ReadResourceReadMetadata(manifest.at("runtime"), runtime);
        ShaderRecompiler::IR::ResourceSnapshot snapshot;
        ShaderRecompiler::IR::ResourceSpecialization specialization;
        bool ok;
        std::string reason;
        if (snapshot_mode) {
          ShaderRecompiler::Capture::ResourceReadSnapshot memory(runtime, events);
          snapshot_diagnostics.update(ImportTextureSnapshot(manifest.at("runtime"), plan, memory));
          snapshot_diagnostics["ordinary_bytes"] = memory.CapturedBytes(ShaderRecompiler::Capture::ReadKind::Normal);
          snapshot_diagnostics["strict_bytes"] = memory.CapturedBytes(ShaderRecompiler::Capture::ReadKind::Strict);
          snapshot_diagnostics["read_only_range_callback_captured"] = runtime.specialization_range_read_only != nullptr;
          snapshot_diagnostics["ownership_query_count"] = std::count_if(events.begin(), events.end(), [](const auto& event) {
            return event.kind == ShaderRecompiler::Capture::ReadKind::ReadOnlyRange;
          });
          phase = "materialize_resources"; Progress(phase);
          ok = ShaderRecompiler::IR::MaterializeResources(plan, memory.Runtime(), snapshot, specialization);
          reason = ShaderRecompiler::IR::LastResourceSpecializationError();
          snapshot_diagnostics["requests"] = memory.Requests();
          snapshot_diagnostics["missing_requests"] = memory.MissingRequests();
          result["snapshot_diagnostics"] = snapshot_diagnostics;
          result["runtime_replay"] = "diagnostic_immutable_address_snapshot";
          result["captured_outcome_reproduced"] = false;
          if (!memory.Error().empty()) {
            phase = "input";
            throw std::runtime_error(memory.Error());
          }
        } else {
          ShaderRecompiler::Capture::ResourceReadReplay replay(runtime, std::move(events));
          phase = "materialize_resources"; Progress(phase);
          ok = ShaderRecompiler::IR::MaterializeResources(plan, replay.Runtime(), snapshot, specialization);
          reason = ShaderRecompiler::IR::LastResourceSpecializationError();
          if (!replay.Error().empty() || !replay.Consumed()) {
            phase = "input";
            throw std::runtime_error(replay.Error().empty() ? "unconsumed resource callbacks" : replay.Error());
          }
          result["captured_outcome_reproduced"] = ok == manifest.at("runtime").at("materialization_succeeded").get<bool>() &&
              (ok || reason == manifest.at("runtime").at("materialization_error").get<std::string>());
        }
        result["resource_materialization_checked"] = true;
        result["checked_through"] = "pixel_materialize_resources";
        result["status"] = ok ? "passed" : "failed";
        result["materialization_error"] = reason;
      }
      std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
      return result.at("status") == "passed" ? 0 : 1;
    }
    if (compute && (manifest.value("metadata_complete", false) || header_profile) && manifest.contains("compute")) {
      const auto& input = manifest.at("compute");
      ShaderComputeInputInfo info;
      info.initial_fp_state = ReadInitialFloatingPointState(input);
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
      if (capture_schema == 2u) {
        ShaderRecompiler::Capture::ReadComputeInputMetadata(input, info);
      }
      const uint32_t user_count = manifest.at("user_data_count");
      const uint32_t user_base = manifest.at("user_data_base");
      const uint32_t wave_size = manifest.at("wave_size");
      if (user_count > 104u || user_base > 104u - user_count || wave_size != info.wave_size ||
          (info.wave_size != 32u && info.wave_size != 64u)) {
        throw std::runtime_error("invalid compute capture metadata");
      }
      // Report the parsed compiler input, not the untrusted manifest object.
      // Legacy captures must retain the default unknown state.
      result["initial_fp_state"] = {
          {"known", info.initial_fp_state.known},
          {"float_mode", info.initial_fp_state.float_mode},
          {"ieee_mode", info.initial_fp_state.ieee_mode},
          {"dx10_clamp", info.initial_fp_state.dx10_clamp}};
      // Translation needs the register count, not runtime descriptor payloads.
      std::vector<uint32_t> user_data(user_count);
      if (capture_schema == 2u) {
        user_data = ShaderRecompiler::Capture::ReadCapturedUserData(manifest);
        if (user_data.size() != user_count) throw std::runtime_error("capture userdata length mismatch");
      }
      ShaderRecompiler::CompileOptions options;
      options.stage = ShaderType::Compute;
      options.wave_size = wave_size;
      options.user_data_base = user_base;
      options.user_data = user_data;
      options.shader_hash = manifest.contains("shader_hash") ? HexValue(manifest.at("shader_hash"))
                            : XXH3_64bits(code.data(), size);
      info.scratch_size_dwords = manifest.at("scratch_dwords");
      const bool dump_ir = std::getenv("KYTY_SHADER_AUDIT_DUMP_IR") != nullptr;
      options.dump_ir = dump_ir;
      options.early_dump = dump_ir;
      options.input_info.compute = &info;
      if (capture_schema == 2u) {
        ShaderRecompiler::Capture::ReadHostProfileMetadata(manifest.at("host_profile"), options.host_profile);
        ShaderRecompiler::Capture::ReadWorkgroupLimitsMetadata(manifest.at("compute_workgroup_limits"), options.compute_workgroup_limits);
        if (manifest.value("back_code_size_bytes", 0u) != 0u)
          throw std::runtime_error("compute back-code replay pending; refusing incomplete input");
        if (manifest.contains("runtime"))
          throw std::runtime_error("compute runtime replay pending; use complete pixel replay for materialization");
      }
      Json profiles = Json::array();
      const bool initial_barriers = info.needs_lds_barriers;
      Json precheck_errors = Json::array();
      for (unsigned variant = 0; variant < (header_profile ? 2u : 1u); variant++) {
        info.needs_lds_barriers = variant == 0u ? initial_barriers : !initial_barriers;
        phase = "translate_resources";
        Progress(header_profile ? (info.needs_lds_barriers ? "translate_profile_barriers_on" : "translate_profile_barriers_off") : phase);
        auto translated = ShaderRecompiler::TranslateProgram(code, options);
        if (dump_ir) {
          std::printf("KYTY_SHADER_AUDIT_IR_BEGIN barriers=%u\n%sKYTY_SHADER_AUDIT_IR_END\n",
                      info.needs_lds_barriers ? 1u : 0u,
                      ShaderRecompiler::IR::ProgramToString(translated.program).c_str());
          std::fflush(stdout);
        }
        const auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
        if (dump_ir) {
          std::printf("KYTY_SHADER_AUDIT_TRACKED_IR_BEGIN barriers=%u\n%sKYTY_SHADER_AUDIT_TRACKED_IR_END\n",
                      info.needs_lds_barriers ? 1u : 0u,
                      ShaderRecompiler::IR::ProgramToString(translated.program).c_str());
          std::fflush(stdout);
        }
        if (dump_ir) {
          Json buffers = Json::array();
          for (uint32_t logical = 0; logical < plan.info.buffers.size(); ++logical) {
            const auto& buffer = plan.info.buffers[logical];
            Json item{{"logical", logical}, {"source", buffer.source},
                      {"first_use_pc", buffer.first_use_pc}, {"read", buffer.read},
                      {"written", buffer.written}, {"atomic", buffer.atomic}};
            if (buffer.source < plan.descriptor_sources.size()) {
              const auto& source = plan.descriptor_sources[buffer.source];
              item["dword_count"] = source.dword_count;
              if (source.bounded_buffer.has_value()) {
                const auto& bounded = *source.bounded_buffer;
                item["bounded"] = {{"expression", bounded.expression},
                                   {"selector_group", bounded.selector_group},
                                   {"key_arg", bounded.key_arg},
                                   {"reads", bounded.reads},
                                   {"dependencies", bounded.dependencies}};
              }
            }
            buffers.push_back(std::move(item));
          }
          Json bounded_reads = Json::array();
          for (uint32_t index = 0; index < plan.bounded_srt_reads.size(); ++index) {
            const auto& read = plan.bounded_srt_reads[index];
            bounded_reads.push_back({{"index", index}, {"address_source", read.address_source},
                                     {"count_source", read.count_source},
                                     {"offset_scale", read.offset_scale},
                                     {"offset_bias", read.offset_bias},
                                     {"memory_offset", read.memory_offset},
                                     {"workgroup_axis", read.workgroup_axis}});
          }
          std::printf("KYTY_SHADER_AUDIT_RESOURCE_PLAN %s\n",
                      Json{{"buffers", std::move(buffers)},
                           {"bounded_srt_reads", std::move(bounded_reads)}}.dump().c_str());
          std::fflush(stdout);
          Json images = Json::array();
          for (uint32_t logical = 0; logical < plan.info.images.size(); ++logical) {
            const auto& image = plan.info.images[logical];
            Json item{{"logical", logical}, {"source", image.source},
                      {"pc", image.first_use_pc}, {"class", static_cast<uint32_t>(image.resource_class)},
                      {"dimension", static_cast<uint32_t>(image.dimension)}};
            if (image.source < plan.descriptor_sources.size()) {
              const auto& source = plan.descriptor_sources[image.source];
              if (source.inline_descriptor) {
                const auto& table = *source.inline_descriptor;
                item["inline"] = {{"buffer_source", table.buffer_source},
                                  {"stride", table.selector_stride},
                                  {"offset", table.descriptor_offset},
                                  {"limit", table.selector_limit},
                                  {"dwords", table.descriptor_dwords}};
              }
            }
            images.push_back(std::move(item));
          }
          std::printf("KYTY_SHADER_AUDIT_IMAGE_PLAN %s\n", images.dump().c_str());
          std::fflush(stdout);
        }
        Json profile{{"needs_lds_barriers", info.needs_lds_barriers},
                     {"blocks", translated.program.blocks.size()},
                     {"buffers", plan.info.buffers.size()},
                     {"images", plan.info.images.size()}, {"samplers", plan.info.samplers.size()},
                     {"sampled_pairs", plan.info.sampled_pairs.size()},
                     {"requires_specialization_memory", plan.requires_specialization_memory}};
        if (host_limits) {
          phase = "compute_execution_precheck";
          Progress(phase);
          auto precheck = PrecheckComputeExecution(translated.program, options.input_info, *host_limits);
          for (const auto& error : precheck.at("errors")) {
            if (std::ranges::find(precheck_errors, error) == precheck_errors.end()) precheck_errors.push_back(error);
          }
          profile["compute_execution_precheck"] = std::move(precheck);
        }
        profiles.push_back(std::move(profile));
      }
      result["checked_through"] = host_limits ? "compute_execution_precheck" : "resource_tracking";
      if (host_limits) {
        result["compute_execution_precheck"] = {
            {"status", precheck_errors.empty() ? "not_rejected" : "rejected"},
            {"errors", precheck_errors}, {"input_phase", "pre_specialization"},
            {"post_specialization_checked", false}, {"spirv_emitted", false}};
        if (!precheck_errors.empty()) {
          result["status"] = "failed";
          std::string errors;
          for (const auto& error : precheck_errors) {
            if (!errors.empty()) errors += "\n";
            errors += error.get<std::string>();
          }
          result["error"] = errors;
        }
      }
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
        const bool dump_cfg = std::getenv("KYTY_SHADER_AUDIT_DUMP_CFG") != nullptr;
        if (dump_cfg) {
          std::printf("KYTY_SHADER_AUDIT_CFG_BEFORE_BEGIN\n%sKYTY_SHADER_AUDIT_CFG_BEFORE_END\n",
                      ShaderRecompiler::CFG::GraphToString(graph).c_str());
          std::fflush(stdout);
        }
        graph = ShaderRecompiler::CFG::Structurize(graph);
        const bool structured = !graph.unsupported;
        if (dump_cfg) {
          std::printf("KYTY_SHADER_AUDIT_CFG_AFTER_BEGIN\n%sKYTY_SHADER_AUDIT_CFG_AFTER_END\n",
                      ShaderRecompiler::CFG::GraphToString(graph).c_str());
          std::fflush(stdout);
        }
        result["checked_through"] = "cfg";
        result["blocks"] = graph.blocks.size();
        result["dispatcher_fallback_required"] = !structured;
        result["translation_skipped_reason"] = "complete stage runtime metadata was not captured";
        if (host_limits) {
          result["compute_execution_precheck"]["reason"] = compute
              ? "compute stage metadata was not captured"
              : "non-compute stage; only CFG checked";
        }
    }
    std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
    std::fflush(stdout);
    return result.at("status") == "passed" ? 0 : 1;
  } catch (const std::exception& error) {
    Json result{{"status", std::string_view(phase) == "input" ? "input_error" : "failed"},
                       {"phase", phase}, {"error", error.what()}};
    if (snapshot_mode) {
      result["runtime_replay"] = "diagnostic_immutable_address_snapshot";
      result["snapshot_diagnostics"] = snapshot_diagnostics;
      result["gpu_execution_checked"] = false;
    }
    std::printf("KYTY_SHADER_AUDIT_RESULT %s\n", result.dump().c_str());
    return 2;
  }
}
