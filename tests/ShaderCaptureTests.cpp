#include "graphics/shader/recompiler/ResourceReadCapture.h"
#include "graphics/shader/recompiler/ResourceReadReplay.h"
#include "graphics/shader/recompiler/ShaderCaptureMetadata.h"
#include <array>
#include <bit>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <stdexcept>
using namespace Libs::Graphics;
using namespace Libs::Graphics::ShaderRecompiler;
static void Check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  try {
    const std::array<uint32_t, 1> code{0xbf810000u};
    ShaderParams params;
    params.code = code;
    params.hash = 0x1234u;
    params.user_data_count = 3u;
    params.user_data[0] = 17u;
    params.user_data[1] = 0x7fffffffu;
    params.user_data[2] = 0xf00dcafeu;
    CompileOptions options;
    options.shader_hash = params.hash;
    options.user_data = std::span(params.user_data).first(3u);
    options.wave_size = 64u;
    if (argc == 2 && std::string_view(argv[1]) == "--compile-capture-only") {
      ShaderComputeInputInfo compute;
      compute.host_subgroup_size = 32u;
      compute.wave_size = 64u;
      options.stage = ShaderType::Compute;
      options.input_info.compute = &compute;
      const std::array<uint32_t, 2> back{0xbe800000u, 0xbf810000u};
      options.back_code = back;
      const std::array<uint32_t, 2> static_state{0x1234u, 0xfedcba98u};
      const auto directory = std::getenv("KYTY_CAPTURE_TEST_DIR")
          ? std::filesystem::u8path(std::getenv("KYTY_CAPTURE_TEST_DIR"))
          : std::filesystem::temp_directory_path() /
            ("kyty-capture-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      Capture::CompileCaptureArchive archive(32u, 1024u * 1024u);
      const auto first = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u}, "rev-a", "tree-a");
      Check(first.has_value() && std::filesystem::exists(first->manifest), "pre-generation manifest not published");
      const auto duplicate = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u}, "rev-b", "tree-b");
      Check(duplicate && duplicate->manifest == first->manifest && archive.Inputs() == 1u,
            "observational revision or repeated input defeated exact capture reuse");
      const auto read_words = [&](const std::string& filename) {
        std::ifstream file(directory / filename, std::ios::binary | std::ios::ate);
        Check(file.good() && file.tellg() >= 0 && static_cast<uint64_t>(file.tellg()) % 4u == 0u,
              "captured binary unavailable or truncated");
        std::vector<uint32_t> result(static_cast<size_t>(file.tellg()) / 4u);
        file.seekg(0);
        file.read(reinterpret_cast<char*>(result.data()), result.size() * sizeof(uint32_t));
        Check(file.good(), "captured binary read incomplete");
        return result;
      };
      Check(read_words(first->metadata.at("code_file").get<std::string>()) == std::vector<uint32_t>(code.begin(), code.end()) &&
            read_words(first->metadata.at("back_code_file").get<std::string>()) == std::vector<uint32_t>(back.begin(), back.end()),
            "main/back shader capture changed binary input");
      params.user_data[0] = 18u;
      auto changed_user = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u});
      Check(changed_user && changed_user->manifest != first->manifest && first->metadata.at("user_data").at(0) == 17u,
            "same shader hash overwrote distinct runtime userdata");
      params.user_data[0] = 17u;
      const std::array<uint32_t, 2> other_back{0xbe800001u, 0xbf810000u};
      options.back_code = other_back;
      auto changed_back = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u});
      Check(changed_back && changed_back->manifest != first->manifest, "distinct back code reused the prior capture");
      options.back_code = back;
      const std::array<uint32_t, 1> other_code{0xbf800000u};
      params.code = other_code;
      auto changed_code = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u});
      Check(changed_code && changed_code->manifest != first->manifest, "same shader hash hid different binary code");
      params.code = code;
      options.host_profile.known = true;
      options.host_profile.float64 = true;
      auto changed_host = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u});
      Check(changed_host && changed_host->manifest != first->manifest, "different host capabilities reused compiler capture");
      options.host_profile = {};
      nlohmann::ordered_json policy{{"shader_optimization_type", 0u},
                                    {"shader_validation_enabled", false},
                                    {"gpu_assisted_validation_enabled", false}};
      auto policy_none = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u}, {}, {}, nullptr, &policy);
      policy["shader_optimization_type"] = 2u;
      auto policy_performance = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u}, {}, {}, nullptr, &policy);
      Check(policy_none && policy_performance && policy_none->manifest != policy_performance->manifest &&
            policy_none->metadata.at("captured_compile_policy").at("shader_optimization_type") == 0u,
            "changed compilation policy reused or overwrote an earlier input");
      auto changed_push = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u}, {}, {}, nullptr, nullptr, 16u);
      Check(changed_push && changed_push->manifest != first->manifest &&
            changed_push->metadata.at("push_data_start_dword") == 16u,
            "different graphics push-data origin reused an incompatible input");
      IR::SrtRuntime runtime;
      runtime.read_memory = +[](void*, uint64_t, std::span<uint32_t> words) { words[0] = 0x12345678u; return true; };
      Capture::ResourceReadCapture recorder(runtime);
      auto observed = recorder.Runtime();
      std::array<uint32_t,1> word{};
      Check(observed.read_memory(observed.userdata, 0x1000u, word), "synthetic runtime read failed");
      auto reads = Capture::BuildResourceReadMetadata(runtime, recorder, true, "");
      auto captured_runtime = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u}, {}, {}, &reads);
      Check(captured_runtime && captured_runtime->manifest != first->manifest &&
            captured_runtime->metadata.at("runtime_resources_captured") == true &&
            captured_runtime->metadata.at("runtime").at("events").size() == 1u,
            "successful materialization inputs not independently preserved");
      reads["events"][0]["words"][0] = 0x87654321u;
      auto changed_runtime = archive.Begin(directory, params, options, static_state, std::array<uint32_t,3>{3u,5u,7u}, {}, {}, &reads);
      Check(changed_runtime && changed_runtime->manifest != captured_runtime->manifest &&
            captured_runtime->metadata.at("runtime").at("events").at(0).at("words").at(0) == 0x12345678u,
            "later callback contents overwrote a successful replay input");
      Check(Capture::CompileCaptureArchive::Update(*captured_runtime, "compile_emit", "pending"), "pending capture phase not persisted");
      nlohmann::ordered_json saved;
      { std::ifstream file(captured_runtime->manifest); file >> saved; }
      Check(saved.at("capture_attempt").at("phase_status") == "pending" &&
            saved.at("capture_attempt").at("reason") == "" && saved.at("runtime") == captured_runtime->metadata.at("runtime"),
            "in-flight phase invents failure or loses callback inputs");
      Check(!Capture::CompileCaptureArchive::Update(*captured_runtime, "compile_emit", "assumed_failed"),
            "unsupported capture outcome accepted");
      Check(Capture::CompileCaptureArchive::Update(*captured_runtime, "spirv_validate", "failed", "synthetic known failure"),
            "known capture failure not persisted");
      { std::ifstream file(captured_runtime->manifest); file >> saved; }
      Check(saved.at("capture_attempt").at("reason") == "synthetic known failure" &&
            saved.at("user_data") == first->metadata.at("user_data") &&
            read_words(saved.at("back_code_file").get<std::string>()) == std::vector<uint32_t>(back.begin(), back.end()),
            "phase update altered exact compiler inputs");
      Capture::CompileCaptureArchive count_limited(1u, 1024u * 1024u);
      auto bounded = count_limited.Begin(directory / "bounded", params, options, {}, std::nullopt);
      Check(bounded.has_value(), "bounded capture rejected first valid input");
      params.user_data[0] = 19u;
      Check(!count_limited.Begin(directory / "bounded", params, options, {}, std::nullopt), "capture input count limit ignored");
      params.user_data[0] = 17u;
      const auto bounded_duplicate = count_limited.Begin(directory / "bounded", params, options, {}, std::nullopt);
      Check(bounded_duplicate && bounded_duplicate->manifest == bounded->manifest,
            "exact capture reuse rejected after count limit");
      Capture::CompileCaptureArchive byte_limited(1u, 0u);
      Check(!byte_limited.Begin(directory / "no-budget", params, options, {}, std::nullopt) &&
            !std::filesystem::exists(directory / "no-budget"), "zero capture budget wrote an advertised artifact");
      // Separate recorder traffic must not consume the compiler input reservation.
      Capture::CompileCaptureArchive compiler_pool(1u, 1024u * 1024u), runtime_pool(1u, 1024u * 1024u);
      auto runtime_slot = runtime_pool.Begin(directory / "runtime-pool", params, options, {}, std::nullopt, {}, {}, &reads);
      Check(runtime_slot.has_value(), "runtime pool rejected first transcript");
      reads["events"][0]["words"][0] = 1u;
      Check(!runtime_pool.Begin(directory / "runtime-pool", params, options, {}, std::nullopt, {}, {}, &reads),
            "runtime reservation count boundary ignored");
      Check(compiler_pool.Begin(directory / "compiler-pool", params, options, {}, std::nullopt).has_value(),
            "exhausted runtime reservation blocked an independent compiler input");

      // Exercise the same admission coordinator as ProgramCache, not merely
      // archive dedup: changed userdata on cached reuse must not crowd generation.
      Capture::CompileCaptureLimits tiny;
      tiny.compiler_inputs = 2u; tiny.runtime_inputs = 1u; tiny.failure_inputs = 1u;
      tiny.compiler_bytes = tiny.runtime_bytes = tiny.failure_bytes = 1024u * 1024u;
      Capture::CompileCaptureCoordinator coordinator(tiny);
      const auto admit = [&](Capture::CompileCaptureEvent event, const nlohmann::ordered_json* callbacks = nullptr) {
        return coordinator.Capture(event, directory / "admission", params, options, static_state,
            std::array<uint32_t,3>{3u,5u,7u}, "test-revision", "test-worktree", callbacks, policy, 16u);
      };
      auto generation_a = admit(Capture::CompileCaptureEvent::Translate, &reads);
      Check(generation_a.has_value(), "first real generation was not captured before translation");
      for (uint32_t request = 0; request < 64u; ++request) {
        params.user_data[0] = 100u + request;
        std::array<uint32_t,1> callback_word{};
        Capture::ResourceReadCapture cached_recorder(runtime);
        auto cached_observed = cached_recorder.Runtime();
        Check(cached_observed.read_memory(cached_observed.userdata, 0x1000u, callback_word) && callback_word[0] == 0x12345678u,
              "cached observation changed original callback result");
        auto cached_reads = Capture::BuildResourceReadMetadata(runtime, cached_recorder, true, "");
        Check(!admit(Capture::CompileCaptureEvent::CachedReuse, &cached_reads), "cached success consumed a persistent generation record");
      }
      params.user_data[0] = 200u;
      auto generation_b = admit(Capture::CompileCaptureEvent::Translate, &reads);
      Check(generation_b && std::filesystem::exists(generation_b->manifest) &&
            generation_b->metadata.at("user_data").at(0) == 200u && coordinator.Summary().at("compiler_inputs") == 2u,
            "cached traffic crowded the late real generation before backend entry");
      Check(generation_b->metadata.at("capture_attempt").at("phase_status") == "pending",
            "a pending backend attempt was mislabeled as failure");
      // An existing plan with a permutation miss is a real generation: retain
      // the locally held original transcript before both translation and Emit.
      tiny.runtime_inputs = 2u;
      Capture::CompileCaptureCoordinator miss_coordinator(tiny);
      auto held_reads = reads;
      held_reads["events"][0]["words"][0] = 0x12345678u;
      auto miss_before_translate = miss_coordinator.Capture(Capture::CompileCaptureEvent::Translate,
          directory / "cached-plan-miss", params, options, static_state, std::nullopt,
          "test-revision", "test-worktree", &held_reads, policy, 16u);
      auto miss_before_emit = miss_coordinator.Capture(Capture::CompileCaptureEvent::Emit,
          directory / "cached-plan-miss", params, options, static_state, std::nullopt,
          "test-revision", "test-worktree", &held_reads, policy, 16u);
      Check(miss_before_translate && miss_before_emit && miss_before_translate->metadata.at("runtime") == held_reads &&
            miss_before_emit->manifest == miss_before_translate->manifest && miss_coordinator.Summary().at("compiler_inputs") == 1u,
            "cached-plan generation lost callbacks or duplicated an identical emission input");
      // A regular pool can be full; a cached materialization failure still has
      // exact policy/push/back/callback inputs in the protected reservation.
      params.user_data[0] = 201u;
      auto failed_reads = reads;
      failed_reads["materialization_succeeded"] = false;
      failed_reads["materialization_error"] = "synthetic cached materialization rejection";
      auto cached_failure = admit(Capture::CompileCaptureEvent::MaterializationFailure, &failed_reads);
      Check(cached_failure && cached_failure->metadata.at("runtime") == failed_reads &&
            cached_failure->metadata.at("captured_compile_policy") == policy &&
            cached_failure->metadata.at("push_data_start_dword") == 16u &&
            cached_failure->metadata.at("back_code_size_bytes") == back.size() * sizeof(uint32_t),
            "regular pool exhaustion lost complete cached failure inputs");
      params.user_data[0] = 202u;
      Check(!admit(Capture::CompileCaptureEvent::MaterializationFailure, &failed_reads) &&
            coordinator.Summary().at("failure_incomplete") == 1u, "protected failure reservation did not report exhaustion");
      const auto fallback = Capture::BuildCompileCaptureMetadata(params, options, static_state,
          std::array<uint32_t,3>{3u,5u,7u}, &failed_reads, policy, 16u);
      Check(fallback.at("runtime") == failed_reads && fallback.at("captured_compile_policy") == policy &&
            fallback.at("push_data_start_dword") == 16u && fallback.at("back_code_hash_xxh3_64") == first->metadata.at("back_code_hash_xxh3_64"),
            "independent failure fallback omitted full compiler identity");
      Capture::ResourceCaptureAdmission legacy_reserve;
      Check(legacy_reserve.Acquire(false) == 0u, "selected positive reservation missing");
      for (uint32_t request = 0; request < 64u; ++request)
        Check(!legacy_reserve.Acquire(false), "positive traffic consumed failure reservation");
      for (uint32_t failure = 1; failure <= 7u; ++failure)
        Check(legacy_reserve.Acquire(true) == failure, "positive traffic crowded a fatal snapshot");
      Check(!legacy_reserve.Acquire(true) && legacy_reserve.Summary().at("failures") == 7u,
            "independent snapshot reservation exceeded eight total slots");
      params.user_data[0] = 17u;

      // Independent ISA fixtures for the real offline Emit/validator runner.
      // SOP1 encoding matches EncodeSop1 in shaderCfgTests: front SETPC s[6:7]
      // is required by DecodeFrontProgram whenever a back body is present.
      constexpr auto sop1 = [](uint32_t opcode, uint32_t dst, uint32_t src) {
        return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) |
               ((opcode & 0xffu) << 8u) | (src & 0xffu);
      };
      const std::array<uint32_t, 2> fused_front{sop1(0x03u, 12u, 129u), sop1(0x20u, 0u, 6u)};
      const std::array<uint32_t, 2> fused_back{sop1(0x03u, 13u, 12u), 0xbf810000u};
      const auto fixture_directory = directory / "emit-fixtures";
      Capture::CompileCaptureArchive fixtures(2u, 1024u * 1024u);
      const auto export_fixture = [&](const ShaderParams& fixture_params, const CompileOptions& fixture_options,
                                      std::optional<std::array<uint32_t,3>> groups, const char* name) {
        std::vector<uint32_t> fixture_static_state;
        if (fixture_options.stage == ShaderType::Pixel)
          BuildStageStaticKey(*fixture_options.input_info.pixel, fixture_static_state);
        else BuildStageStaticKey(*fixture_options.input_info.compute, fixture_static_state);
        IR::SrtRuntime empty_runtime;
        empty_runtime.user_data = fixture_options.user_data;
        empty_runtime.compute_workgroups = groups;
        Capture::ResourceReadCapture empty_capture(empty_runtime);
        const auto empty_reads = Capture::BuildResourceReadMetadata(empty_runtime, empty_capture, true, "");
        nlohmann::ordered_json fixture_policy{{"shader_optimization_type", 0u},
                                             {"shader_validation_enabled", true},
                                             {"gpu_assisted_validation_enabled", false},
                                             {"gpuav_shader_instrumentation", false}};
        auto fixture = fixtures.Begin(fixture_directory, fixture_params, fixture_options,
            fixture_static_state, groups, {}, {}, &empty_reads, &fixture_policy, 16u);
        Check(fixture && fixture->metadata.at("metadata_complete") == true &&
              fixture->metadata.at("runtime_resources_captured") == true &&
              fixture->metadata.at("runtime").at("events").empty(), "complete synthetic Emit fixture not captured");
        fixture->metadata["capture_attempt"]["synthetic_fixture"] = true;
        Check(Capture::CompileCaptureArchive::Update(*fixture, "synthetic_fixture_ready", "completed"),
              "synthetic fixture manifest update failed");
        auto alias = fixture_directory / (std::string(name) + "-emit.json");
        for (uint32_t index = 1u; std::filesystem::exists(alias); ++index)
          alias = fixture_directory / (std::string(name) + "-emit-" + std::to_string(index) + ".json");
        std::ofstream manifest(alias);
        manifest << fixture->metadata.dump(2) << '\n';
        manifest.flush();
        Check(manifest.good(), "named synthetic Emit fixture not written");
        std::cout << "KYTY_CAPTURE_EMIT_FIXTURE_" << name << '=' << alias.string() << '\n';
      };
      ShaderParams pixel_fixture;
      pixel_fixture.code = code;
      pixel_fixture.hash = 0xc001u;
      ShaderPixelInputInfo pixel_input;
      pixel_input.wave_size = 32u;
      pixel_input.initial_fp_state = {true, 0xc0u, false, false};
      CompileOptions fixture_options;
      fixture_options.stage = ShaderType::Pixel;
      fixture_options.wave_size = 32u;
      fixture_options.shader_hash = pixel_fixture.hash;
      fixture_options.host_profile.known = true;
      fixture_options.compute_workgroup_limits = {{1024u,1024u,64u}, 1024u, 32u, false, 65536u};
      fixture_options.input_info.pixel = &pixel_input;
      export_fixture(pixel_fixture, fixture_options, std::nullopt, "pixel");
      ShaderParams compute_fixture;
      compute_fixture.code = fused_front;
      compute_fixture.back_code = fused_back;
      compute_fixture.hash = 0xc002u;
      ShaderComputeInputInfo compute_input;
      compute_input.wave_size = compute_input.host_subgroup_size = 32u;
      compute_input.threads_num[0] = compute_input.dispatch_threads_num[0] = 32u;
      compute_input.threads_num[1] = compute_input.threads_num[2] = 1u;
      compute_input.dispatch_threads_num[1] = compute_input.dispatch_threads_num[2] = 1u;
      compute_input.initial_fp_state = {true, 0xc0u, false, false};
      fixture_options.stage = ShaderType::Compute;
      fixture_options.shader_hash = compute_fixture.hash;
      fixture_options.back_code = fused_back;
      fixture_options.input_info.compute = &compute_input;
      export_fixture(compute_fixture, fixture_options, std::array<uint32_t,3>{1u,1u,1u}, "compute");
      std::cout << "KYTY_CAPTURE_ARTIFACT_DIR=" << directory.string() << '\n';
    } else if (argc == 2 && std::string_view(argv[1]) == "--texture-capture-only") {
      ShaderTextureResource texture;
      texture.fields[1] = (60u << 20u) | (1u << 30u);
      texture.fields[2] = 1u << 14u;
      texture.fields[3] = (9u << 28u) | (5u << 20u) | 0xfacu;
      std::vector<uint32_t> pixels(1024u, 0x40404040u);
      // Independent Standard4KB 2x2 offsets: (0,0)=0, (1,0)=4,
      // (0,1)=16, (1,1)=20. Padding contains an unrelated value64.
      pixels[0] = 0x40070401u; pixels[1] = 0x41ff0401u;
      pixels[4] = 0x42ff0407u; pixels[5] = 0x43070401u;
      std::vector<uint32_t> values;
      Check(Capture::CollectCapturedTextureBytes(texture, pixels, 7u, values) &&
                values == std::vector<uint32_t>{1u,4u,7u,255u},
            "logical RGB domain includes padding/alpha or loses a texel");
      Check(Capture::CollectCapturedTextureBytes(texture, pixels, 8u, values) &&
                values == std::vector<uint32_t>{64u,65u,66u,67u},
            "alpha domain does not follow selected channel");
      texture.fields[3] = (texture.fields[3] & ~0xfffu) | 7u | (1u << 6u) | (4u << 9u);
      Check(Capture::CollectCapturedTextureBytes(texture, pixels, 7u, values) &&
                values == std::vector<uint32_t>{0u,1u,64u,65u,66u,67u},
            "byte domain ignores swizzle or constant components");
      pixels.pop_back();
      Check(!Capture::CollectCapturedTextureBytes(texture, pixels, 7u, values) && values.empty(),
            "truncated texture accepted as complete byte domain");
      ShaderTextureResource across_blocks;
      across_blocks.fields[1] = 60u << 20u;
      across_blocks.fields[2] = 8u | (1u << 14u); // 33x2, two 32x32 RGBA8 blocks.
      across_blocks.fields[3] = (9u << 28u) | (5u << 20u) | 0xfacu;
      std::vector<uint32_t> block_pixels(2048u, 0x40404040u);
      std::fill_n(block_pixels.begin(), 1024u, 0x01010101u);
      // Independent block-local offsets: x32/y0 starts block1; y1 adds16.
      // x31/y1 in block0 has Standard4KB offset0xa9c. Other block1 texels are padding.
      block_pixels[0xa9cu / 4u] = 0x41ffffffu;
      block_pixels[4096u / 4u] = 0x42070401u;
      block_pixels[(4096u + 16u) / 4u] = 0x43040107u;
      Check(Capture::CollectCapturedTextureBytes(across_blocks, block_pixels, 7u, values) &&
                values == std::vector<uint32_t>{1u,4u,7u,255u},
            "logical RGB collection fails across tiled block boundary");
      across_blocks.fields[1] |= 1u << 30u;
      across_blocks.fields[2] = 32u << 14u; // 2x33, second block row.
      block_pixels[(4096u + 4u) / 4u] = 0x43ffffffu;
      Check(Capture::CollectCapturedTextureBytes(across_blocks, block_pixels, 7u, values) &&
                values == std::vector<uint32_t>{1u,4u,7u,255u},
            "logical RGB collection fails across tiled block rows");
      IR::ResourcePlan plan;
      IR::DescriptorSource buffer;
      buffer.dword_count = 4;
      buffer.dwords[0] = IR::Value(0x1000u);
      buffer.dwords[1] = IR::Value(16u << 16u);
      buffer.dwords[2] = IR::Value(1u);
      buffer.dwords[3] = IR::Value(0x5204u);
      plan.descriptor_sources.push_back(buffer);
      IR::DescriptorSource image;
      image.inline_descriptor = IR::DescriptorSource::InlineDescriptor{0u, 16u, 0u, 0u};
      image.inline_descriptor->selector_limit = 1u;
      plan.descriptor_sources.push_back(image);
      IR::ImageResource metadata;
      metadata.source = 1u; metadata.read = true;
      plan.info.images.push_back(metadata);
      struct State { uint32_t reads = 0; bool dirty = false; } state;
      IR::SrtRuntime runtime;
      runtime.userdata = &state;
      runtime.read_specialization_memory = +[](void* data, uint64_t address, std::span<uint32_t> words) {
        auto& state = *static_cast<State*>(data); ++state.reads;
        if (address == 0x1000u && words.size() == 4u) {
          words[0] = 0x20u; words[1] = 60u << 20u; words[2] = 0u;
          words[3] = (9u << 28u) | (5u << 20u) | 0xfacu;
          return true;
        }
        if (address == 0x2000u && words.size() == 1024u && !state.dirty) {
          std::fill(words.begin(), words.end(), 0x03020104u); return true;
        }
        return false;
      };
      const auto captured = Capture::CaptureTextureInputs(plan, runtime, 0u);
      Check(captured.at("entries").size() == 1u, "texture capture missed bounded candidate");
      const auto& entry = captured.at("entries").at(0);
      Check(entry.at("payload_captured") == true && entry.at("descriptor_stable") == true &&
                entry.at("words").size() == 1024u && entry.at("words").at(0) == 0x03020104u,
            "texture capture altered raw payload or descriptor");
      Check(state.reads == 3u && captured.at("specialization_proof") == false,
            "diagnostic reads/count or proof scope incorrect");
      state.dirty = true;
      const auto dirty = Capture::CaptureTextureInputs(plan, runtime, 0u);
      Check(dirty.at("entries").at(0).at("payload_captured") == false &&
                !dirty.at("entries").at(0).contains("words"), "dirty texture fabricated payload");
      plan.info.images[0].written = true;
      const auto reads = state.reads;
      Check(Capture::CaptureTextureInputs(plan, runtime, 0u).contains("error") && state.reads == reads,
            "written image admitted for diagnostic snapshot");
    } else if (argc == 2 && std::string_view(argv[1]) == "--read-capture-only") {
      struct State {
        uint32_t normal = 0, strict = 0, clamp = 0, read_only_range = 0;
      } state;
      IR::SrtRuntime original;
      original.userdata = &state;
      original.max_dense_images = 97u;
      original.neighboring_stages_read_only = true;
      original.read_memory =
          +[](void *ctx, uint64_t address, std::span<uint32_t> words) {
            auto &data = *static_cast<State *>(ctx);
            ++data.normal;
            if (address == 0x100u) {
              words[0] = 0x12345678u;
              return true;
            }
            words[0] = 0xabcdef01u;
            return false;
          };
      original.read_specialization_memory =
          +[](void *ctx, uint64_t, std::span<uint32_t> words) {
            ++static_cast<State *>(ctx)->strict;
            words[0] = 0x87654321u;
            return true;
          };
      original.clamp_memory_range = +[](void *ctx, uint64_t, uint64_t bytes) {
        ++static_cast<State *>(ctx)->clamp;
        return bytes > 12u ? 12u : bytes;
      };
      original.specialization_range_read_only = +[](void *ctx, uint64_t address, uint64_t bytes) {
        ++static_cast<State *>(ctx)->read_only_range;
        return address == 0x100u && bytes == 16u;
      };
      Capture::ResourceReadCapture capture(original, 2u, 8u);
      auto runtime = capture.Runtime();
      std::array<uint32_t, 1> words{};
      Check(runtime.read_memory(runtime.userdata, 0x100u, words) &&
                words[0] == 0x12345678u,
            "captured normal result changed");
      Check(
          runtime.read_specialization_memory(runtime.userdata, 0x100u, words) &&
              words[0] == 0x87654321u,
          "captured strict result changed");
      Check(!runtime.read_memory(runtime.userdata, 0x200u, words) &&
                words[0] == 0xabcdef01u,
            "captured failed callback result changed");
      Check(runtime.clamp_memory_range(runtime.userdata, 0x100u, 20u) == 12u,
            "captured clamp result changed");
      Check(runtime.specialization_range_read_only(runtime.userdata, 0x100u, 16u) &&
                !runtime.specialization_range_read_only(runtime.userdata, 0x104u, 16u),
            "captured ownership query result changed");
      Check(state.normal == 2u && state.strict == 1u && state.clamp == 1u && state.read_only_range == 2u,
            "capture added or dropped callbacks");
      Check(capture.Complete() && capture.Events().size() == 6u &&
                capture.Events()[2].words.empty() && capture.Events()[4].words.empty() &&
                capture.Events()[4].succeeded && !capture.Events()[5].succeeded,
            "capture recorded undefined failed-read output");
      Check(runtime.max_dense_images == 97u &&
                runtime.neighboring_stages_read_only,
            "capture changed runtime constraints");
      const auto recorded = Capture::BuildResourceReadMetadata(original, capture, false, "expected failure");
      IR::SrtRuntime parsed;
      auto events = Capture::ReadResourceReadMetadata(recorded, parsed);
      Capture::ResourceReadReplay replay(parsed, events);
      auto offline = replay.Runtime();
      Check(offline.read_memory(offline.userdata, 0x100u, words) && words[0] == 0x12345678u,
            "normal replay lost recorded value");
      Check(offline.read_specialization_memory(offline.userdata, 0x100u, words) && words[0] == 0x87654321u,
            "strict replay lost distinct value at same address");
      words[0] = 77u;
      Check(!offline.read_memory(offline.userdata, 0x200u, words) && words[0] == 77u,
            "failed replay fabricated output");
      Check(offline.clamp_memory_range(offline.userdata, 0x100u, 20u) == 12u &&
                offline.specialization_range_read_only(offline.userdata, 0x100u, 16u) &&
                !offline.specialization_range_read_only(offline.userdata, 0x104u, 16u) && replay.Consumed(),
            "ordered replay did not consume exact callbacks");
      Check(state.normal == 2u && state.strict == 1u && state.clamp == 1u && state.read_only_range == 2u,
            "offline replay accessed live backing");
      Capture::ResourceReadReplay changed(parsed, events);
      auto different = changed.Runtime();
      words[0] = 55u;
      Check(!different.read_memory(different.userdata, 0x101u, words) && words[0] == 55u && !changed.Error().empty(),
            "uncaptured access accepted or overwritten");
      auto corrupt = recorded;
      corrupt["events"][0]["words"][0] = 0x100000000ull;
      bool rejected = false;
      try { Capture::ReadResourceReadMetadata(corrupt, parsed); }
      catch (const std::exception&) { rejected = true; }
      Check(rejected, "resource payload integer truncation accepted");
      corrupt = recorded;
      corrupt["specialization_range_read_only"] = false;
      rejected = false;
      try { Capture::ReadResourceReadMetadata(corrupt, parsed); }
      catch (const std::exception&) { rejected = true; }
      Check(rejected, "ownership query accepted when callback presence was false");
      corrupt = recorded;
      corrupt["events"][4]["clamped_bytes"] = 1u;
      rejected = false;
      try { Capture::ReadResourceReadMetadata(corrupt, parsed); }
      catch (const std::exception&) { rejected = true; }
      Check(rejected, "ownership query accepted a clamp payload");
      auto legacy = recorded;
      legacy.erase("specialization_range_read_only");
      legacy["events"].erase(legacy["events"].begin() + 4, legacy["events"].end());
      auto legacy_events = Capture::ReadResourceReadMetadata(legacy, parsed);
      Check(parsed.specialization_range_read_only == nullptr && legacy_events.size() == 4u,
            "legacy capture invented an ownership callback");
      Capture::ResourceReadReplay ownership_mismatch(original, {{Capture::ReadKind::ReadOnlyRange, 0x100u, 16u, true}});
      auto mismatched = ownership_mismatch.Runtime();
      Check(!mismatched.specialization_range_read_only(mismatched.userdata, 0x104u, 16u) &&
                !ownership_mismatch.Error().empty(), "changed ownership query accepted");
      Check(runtime.read_memory(runtime.userdata, 0x100u, words) &&
                words[0] == 0x12345678u && !capture.Complete(),
            "capture overflow altered resource read");
      Check(state.normal == 3u, "capture overflow added or dropped callback");
      IR::SrtRuntime empty;
      Capture::ResourceReadCapture absent(empty);
      Check(absent.Runtime().read_memory == nullptr &&
                absent.Runtime().read_specialization_memory == nullptr &&
                absent.Runtime().clamp_memory_range == nullptr &&
                absent.Runtime().specialization_range_read_only == nullptr,
            "capture invented absent callback");
    } else if (argc == 2 &&
               std::string_view(argv[1]) == "--pixel-metadata-only") {
      ShaderPixelInputInfo pixel;
      pixel.wave_size = 64u;
      pixel.input_num = 2u;
      pixel.interpolator_settings[1] = 0x123456u;
      pixel.initial_fp_state = {true, 0xd0u, true, true};
      pixel.ps_pos_x = true;
      pixel.ps_no_perspective = true;
      pixel.target_export_mapping[3].packed = 0x1bu;
      pixel.target_conversion_format[3] =
          Prospero::BufferFormat::k32_32_32_32Float;
      pixel.alpha_blend_source = ShaderAlphaBlendSource::SourceAlphaOne;
      options.stage = ShaderType::Pixel;
      options.input_info.pixel = &pixel;
      const auto manifest =
          Capture::BuildCompileMetadata(params, options, {}, std::nullopt);
      Check(manifest.at("metadata_complete").get<bool>(),
            "pixel compiler input capture incomplete");
      const auto &input = manifest.at("pixel");
      Check(
          input.at("interpolator_settings").at(1) == 0x123456u &&
              input.at("initial_fp_state").at("float_mode") == 0xd0u &&
              input.at("ps_pos_x") == true &&
              input.at("ps_no_perspective") == true &&
              input.at("target_export_mapping").at(3) == 0x1bu &&
              input.at("alpha_blend_source") ==
                  static_cast<uint32_t>(ShaderAlphaBlendSource::SourceAlphaOne),
          "pixel input values changed during capture");
      ShaderPixelInputInfo restored;
      Capture::ReadPixelInputMetadata(input, restored);
      options.input_info.pixel = &restored;
      Check(Capture::BuildCompileMetadata(params, options, {}, std::nullopt) ==
                manifest,
            "pixel compiler input roundtrip changed metadata");
      auto missing = input;
      missing.erase("ps_pos_x");
      bool rejected = false;
      try {
        Capture::ReadPixelInputMetadata(missing, restored);
      } catch (const std::exception &) {
        rejected = true;
      }
      Check(rejected, "missing pixel compiler input accepted");
      auto overflow = input;
      overflow["target_output_mode"][0] = 256u;
      rejected = false;
      try {
        Capture::ReadPixelInputMetadata(overflow, restored);
      } catch (const std::exception &) {
        rejected = true;
      }
      Check(rejected, "pixel integer truncation accepted");
      if (const auto* directory = std::getenv("KYTY_CAPTURE_TEST_DIR")) {
        const auto path = std::filesystem::u8path(directory);
        std::filesystem::create_directories(path);
        auto fixture = manifest;
        IR::SrtRuntime empty;
        empty.user_data = options.user_data;
        Capture::ResourceReadCapture capture(empty);
        fixture["runtime"] = Capture::BuildResourceReadMetadata(empty, capture, true, "");
        fixture["runtime_resources_captured"] = true;
        std::ofstream binary(path / fixture.at("code_file").get<std::string>(), std::ios::binary);
        binary.write(reinterpret_cast<const char*>(code.data()), code.size() * sizeof(uint32_t));
        std::ofstream json(path / "synthetic-pixel.json");
        json << fixture.dump(2);
        Check(binary.good() && json.good(), "synthetic replay fixture write failed");
      }

    } else if (argc == 2 &&
               std::string_view(argv[1]) == "--vertex-metadata-only") {
      ShaderVertexInputInfo vertex;
      vertex.wave_size = 64u;
      vertex.resources_num = 1;
      vertex.resources[0].fields[0] = 0x12345678u;
      vertex.resources_dst[0].register_start = 4;
      vertex.resources_dst[0].registers_num = 2;
      vertex.resources_dst[0].attr_id = 7;
      vertex.resources_dst[0].fetch_index = 11u;
      vertex.buffers_num = 1;
      vertex.buffers[0].addr = 0x123456789000ull;
      vertex.buffers[0].stride = 28u;
      vertex.buffers[0].num_records = 19u;
      vertex.buffers[0].attr_num = 1;
      vertex.buffers[0].attr_offsets[0] = 12u;
      vertex.linked_param_count = 1u;
      vertex.linked_param_sources[0] = 5u;
      vertex.linked_param_locations[0] = 17u;
      vertex.fetch_external = true;
      vertex.clip_space.enabled = true;
      vertex.clip_space.scale[0] = std::bit_cast<float>(0x80000000u);
      vertex.clip_space.half_extent[1] = std::bit_cast<float>(0x7fc00001u);
      options.stage = ShaderType::Vertex;
      options.compute_workgroup_limits.native_subgroup_size = 32u;
      options.compute_workgroup_limits.max_shared_memory_bytes = 65536u;
      options.input_info.vertex = &vertex;
      options.user_data_base = 8u;
      const auto manifest =
          Capture::BuildCompileMetadata(params, options, {}, std::nullopt);
      Check(manifest.at("metadata_complete").get<bool>(),
            "vertex compiler input capture incomplete");
      Check(manifest.contains("compute_workgroup_limits") &&
                manifest.at("compute_workgroup_limits").contains("native_subgroup_size"),
            "graphics compiler device limits not captured");
      Check(manifest.at("compute_workgroup_limits").at("native_subgroup_size") == 32u &&
                manifest.at("compute_workgroup_limits").at("max_shared_memory_bytes") == 65536u,
            "graphics compiler device limits changed");
      ComputeWorkgroupLimits restored_limits;
      Capture::ReadWorkgroupLimitsMetadata(manifest.at("compute_workgroup_limits"), restored_limits);
      Check(restored_limits.native_subgroup_size == 32u && restored_limits.max_shared_memory_bytes == 65536u,
            "graphics compiler device limits roundtrip changed");
      const auto &input = manifest.at("vertex");
      Check(input.at("resources").at(0).at(0) == 0x12345678u &&
                input.at("resources_dst").at(0).at("attr_id") == 7 &&
                input.at("buffers").at(0).at("addr") == 0x123456789000ull &&
                input.at("linked_param_locations").at(0) == 17u &&
                input.at("clip_space").at("scale_bits").at(0) == 0x80000000u &&
                input.at("clip_space").at("half_extent_bits").at(1) ==
                    0x7fc00001u,
            "vertex input values changed during capture");
    } else if (argc == 2 &&
               std::string_view(argv[1]) == "--compute-metadata-only") {
      ShaderComputeInputInfo compute;
      options.host_profile.known = true;
      options.host_profile.float64 = true;
      options.host_profile.rte_float32 = true;
      compute.host_subgroup_size = 32u;
      compute.wave_size = 64u;
      compute.float_mode = 0xd0u;
      compute.threads_num[0] = 8u;
      compute.threads_num[1] = 4u;
      compute.threads_num[2] = 2u;
      options.stage = ShaderType::Compute;
      options.input_info.compute = &compute;
      const auto manifest = Capture::BuildCompileMetadata(
          params, options, {}, std::array<uint32_t, 3>{3u, 5u, 7u});
      Check(manifest.at("compute").contains("host_subgroup_size"),
            "compute host subgroup not captured");
      Check(manifest.at("compute").at("host_subgroup_size") == 32u &&
                manifest.at("compute").at("float_mode") == 0xd0u,
            "compute input values changed during capture");
      Check(manifest.contains("user_data"),
            "runtime userdata payload not captured");
      Check(manifest.at("user_data") ==
                std::vector<uint32_t>{17u, 0x7fffffffu, 0xf00dcafeu},
            "runtime userdata values changed during capture");
      ShaderComputeInputInfo restored;
      Capture::ReadComputeInputMetadata(manifest.at("compute"), restored);
      ShaderHostProfile host;
      Capture::ReadHostProfileMetadata(manifest.at("host_profile"), host);
      Check(host.known && host.float64 && host.rte_float32 && !host.sampled_image_nonuniform_indexing,
            "captured host capabilities changed");
      options.input_info.compute = &restored;
      Check(Capture::BuildCompileMetadata(
                params, options, {}, std::array<uint32_t, 3>{3u, 5u, 7u}) ==
                manifest,
            "compute compiler input roundtrip changed metadata");

    } else
      throw std::runtime_error("select explicit capture regression");
    std::cout << "KYTY_CAPTURE_METADATA_PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
