#include "graphics/shader/recompiler/ResourceReadCapture.h"
#include "graphics/shader/recompiler/ResourceReadReplay.h"
#include "graphics/shader/recompiler/ShaderCaptureMetadata.h"
#include <array>
#include <bit>
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
    if (argc == 2 && std::string_view(argv[1]) == "--texture-capture-only") {
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
        uint32_t normal = 0, strict = 0, clamp = 0;
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
      Check(state.normal == 2u && state.strict == 1u && state.clamp == 1u,
            "capture added or dropped callbacks");
      Check(capture.Complete() && capture.Events().size() == 4u &&
                capture.Events()[2].words.empty(),
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
      Check(offline.clamp_memory_range(offline.userdata, 0x100u, 20u) == 12u && replay.Consumed(),
            "ordered replay did not consume exact callbacks");
      Check(state.normal == 2u && state.strict == 1u && state.clamp == 1u,
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
      Check(runtime.read_memory(runtime.userdata, 0x100u, words) &&
                words[0] == 0x12345678u && !capture.Complete(),
            "capture overflow altered resource read");
      Check(state.normal == 3u, "capture overflow added or dropped callback");
      IR::SrtRuntime empty;
      Capture::ResourceReadCapture absent(empty);
      Check(absent.Runtime().read_memory == nullptr &&
                absent.Runtime().read_specialization_memory == nullptr &&
                absent.Runtime().clamp_memory_range == nullptr,
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
