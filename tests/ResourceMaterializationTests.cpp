#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, uint32_t *) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan SrtPlan(uint64_t address) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  program.memory_info.push_back(memory);
  const auto low = Value(static_cast<uint32_t>(address));
  const auto high = Value(static_cast<uint32_t>(address >> 32u));
  auto &handle =
      value_block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = value_block.AppendNewInst(
      ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UnbasedFlatPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  program.info.uses_dma = true;
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataBufferPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &value_block = AddValueBlock(program);

  auto &user_data = value_block.AppendNewInst(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  DescriptorSource source;
  source.dwords[0] = Value(&user_data);
  source.dwords[1] = Value(0u);
  source.dwords[2] = Value(0u);
  source.dwords[3] = Value(0u);
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MixedSamplerPlan(uint32_t sampler_count = 2u) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count, uint32_t first) {
    DescriptorSource source;
    source.dword_count = dword_count;
    source.dwords[0] = Value(first);
    for (uint32_t i = 1; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8, 0);
  const auto image1 = AddSource(8, 0);
  const auto sampler0 = AddSource(4, 0x11111111u);
  const auto sampler1 = AddSource(4, 0x22222222u);
  program.info.images.push_back(
      {.source = image0,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.images.push_back(
      {.source = image1,
       .resource_class = ImageResourceClass::Sampled,
       .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
       .dimension =
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D,
       .conversion_format =
           Libs::Graphics::Prospero::BufferFormat::k8_8_8_8UNorm});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  for (uint32_t index = 2u; index < sampler_count; index++) {
    program.info.samplers.push_back({.source = AddSource(4, 0x30000000u + index)});
  }
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return ExtractResourcePlan(program);
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = SrtPlan(reinterpret_cast<uint64_t>(&dword));
  uint32_t specialization_reads = 0;
  const SrtRuntime runtime{.userdata = &specialization_reads,
                           .read_specialization_memory =
                               RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mapped SRT stage materialization failed");
  Check(specialization_reads == 0,
        "ordinary SRT read used the specialization reader");
  Check(snapshot.flattened_srt.size() == 1 &&
            snapshot.flattened_srt[0] == dword,
        "cache rematerialization did not use the direct reader by default");
}

void TestUnbasedFlatCacheHitMaterializes() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UnbasedFlatPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "unbased FLAT stage materialization failed");
  Check(snapshot.buffers.empty() && snapshot.images.empty(),
        "unbased FLAT plan produced unexpected descriptors");
}

void TestFailedMaterializationPreservesPriorStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  snapshot.user_data.push_back(0xfeedbeefu);
  ResourceSpecialization specialization;
  specialization.buffers.push_back({.packed_stride = 7});
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
  Check(snapshot.user_data == std::vector<uint32_t>{0xfeedbeefu} &&
            specialization.buffers.size() == 1 &&
            specialization.buffers[0].packed_stride == 7,
        "failed cache materialization changed its destinations");
}

void TestMixedSamplerDuplicatesTheCorrectSnapshot() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = MixedSamplerPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "mixed sampler materialization failed");
  Check(snapshot.samplers.size() == 3,
        "mixed sampler materialization appended unrelated samplers");
  Check(snapshot.samplers[2] == snapshot.samplers[1] &&
            snapshot.samplers[2] != snapshot.samplers[0],
        "point sampler variant duplicated the wrong runtime descriptor");
}

void TestPointSamplerCapacityIsTransactional() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = MixedSamplerPlan(ShaderInfo::MaxSamplers - 1u);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {}, snapshot, specialization),
        "ordinary samplers plus one point variant rejected the exact sampler capacity");
  Check(snapshot.samplers.size() == ShaderInfo::MaxSamplers &&
            specialization.sampler_origins.size() == ShaderInfo::MaxSamplers - 1u &&
            snapshot.samplers.back() == snapshot.samplers[1] &&
            snapshot.samplers.back() != snapshot.samplers[0],
        "point sampler capacity dropped or cloned the wrong descriptor");
  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  for (const uint32_t count : {ShaderInfo::MaxSamplers, ShaderInfo::MaxSamplers + 1u}) {
    auto overflow = MixedSamplerPlan(count);
    Check(!MaterializeResources(overflow, {}, snapshot, specialization),
          "ordinary samplers plus point expansion exceeded the sampler capacity");
    Check(snapshot.buffers == prior_snapshot.buffers && snapshot.images == prior_snapshot.images &&
              snapshot.samplers == prior_snapshot.samplers &&
              snapshot.flattened_srt == prior_snapshot.flattened_srt &&
              snapshot.user_data == prior_snapshot.user_data &&
              specialization == prior_specialization,
          "sampler capacity failure partially replaced a successful materialization");
  }
}

void TestAtomicFloatImageUsesRawUintSpecialization() {
  using namespace Libs::Graphics;
  using namespace ShaderRecompiler::IR;
  for (const auto format : {Prospero::BufferFormat::k32UInt,
                           Prospero::BufferFormat::k32Float}) {
    for (const bool atomic : {false, true}) {
      Program program;
      program.stage = ShaderType::Compute;
      program.srt_plan_complete = true;
      program.resource_tracking_complete = true;
      AddValueBlock(program);

      // A non-null R32 descriptor must take the format-specialization path.
      const uint32_t words[] = {
          0x304bb700u, 0xc0000000u | (static_cast<uint32_t>(format) << 20u),
          0x0000001fu, 0x91b00204u, 0, 0x00700000u, 0, 0};
      DescriptorSource source;
      source.dword_count = 8;
      for (uint32_t i = 0; i < 8; ++i) {
        source.dwords[i] = Value(words[i]);
      }
      program.descriptor_sources.push_back(source);
      ImageResource image;
      image.source = 0;
      image.resource_class = ImageResourceClass::Storage;
      image.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
      image.read = true;
      image.written = true;
      image.atomic = atomic;
      program.info.images.push_back(image);

      auto plan = ExtractResourcePlan(program);
      ResourceSnapshot snapshot;
      ResourceSpecialization specialization;
      Check(MaterializeResources(plan, {}, snapshot, specialization),
            "R32 image materialization failed");
      Check(snapshot.images.size() == 1 &&
                snapshot.images[0].dwords[1] == words[1],
            "specialization changed the guest image descriptor");
      const auto expected = atomic || format == Prospero::BufferFormat::k32UInt
                                ? Prospero::TextureNumericClass::Uint
                                : Prospero::TextureNumericClass::Float;
      Check(specialization.images.size() == 1 &&
                specialization.images[0].numeric_class == expected,
            "R32F atomic image must use Uint while ordinary R32F stays Float");
    }
  }
}

} // namespace

namespace Common {

int DbgExitHandler(const char *, int, std::string_view) { std::abort(); }

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view) {
  std::abort();
}

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

namespace Libs::Graphics {
SurfaceFormatInfo TextureGetSurfaceFormatInfo(Prospero::BufferFormat format) {
  static_cast<void>(format);
  return SurfaceFormatInfo(vk::Format::eR32Sfloat,
                           Prospero::BufferFormat::kInvalid);
}
} // namespace Libs::Graphics

int main() {
  TestMappedSrtUsesDirectReaderByDefault();
  TestUnbasedFlatCacheHitMaterializes();
  TestFailedMaterializationPreservesPriorStage();
  TestMixedSamplerDuplicatesTheCorrectSnapshot();
  TestPointSamplerCapacityIsTransactional();
  TestAtomicFloatImageUsesRawUintSpecialization();
  std::puts("ResourceMaterializationTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
