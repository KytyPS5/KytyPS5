#include "graphics/shader/recompiler/ShaderDiskCache.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

using namespace Libs::Graphics::ShaderRecompiler;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ShaderDiskCacheTests: failed: %s\n", text);
    std::abort();
  }
}

IR::Block &AddValueBlock(IR::Program &program) {
  auto block = std::make_unique<IR::Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

// A plan with every immediate type, a loop through a phi, buffers, images, an indirect image and
// stage inputs: enough to touch each field the serializer writes.
IR::ResourcePlan BuildPlan() {
  using namespace IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.shader_hash = 0x1122334455667788ull;
  program.user_data_base = 8;
  program.user_data_count = 16;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  program.has_address_writes = true;
  auto &block = AddValueBlock(program);

  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarBuffer;
  memory.resource = 3;
  memory.data_dwords = 4;
  memory.typed = true;
  memory.planning_only = true;
  program.memory_info.push_back(memory);

  auto &phi = block.AppendNewInst(ValueOpcode::Phi);
  phi.SetFlags(Type::U32);
  auto &next = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&phi), Value(1u)});
  phi.AddPhiOperand(&block, Value(7u));
  phi.AddPhiOperand(&block, Value(&next));

  auto &user_data = block.AppendNewInst(ValueOpcode::GetUserData,
                                        {Value(static_cast<ScalarReg>(4))});
  DescriptorSource source;
  source.dwords[0] = Value(&phi);
  source.dwords[1] = Value(&user_data);
  source.dwords[2] = Value(static_cast<uint8_t>(0xab));
  source.dwords[3] = Value(static_cast<uint16_t>(0xabcd));
  source.dwords[4] = Value(0xfeedface12345678ull);
  source.dwords[5] = Value::F32(1.5f);
  source.dwords[6] = Value::F16(0x3c00);
  source.dwords[7] = Value(true);
  source.dword_count = 8;
  source.indirect_image = DescriptorSource::IndirectImage{
      .material_source = 0,
      .table_source = 1,
      .selector_stride = 12,
      .selector_offset = 4,
      .table_offset = 8,
      .key_count = Value(&next),
      .selector_mask = Value(0xffu),
      .sources = {4, 5}};
  program.descriptor_sources.push_back(source);
  DescriptorSource second;
  second.dwords[0] = Value(static_cast<VectorReg>(9));
  second.dword_count = 1;
  program.descriptor_sources.push_back(second);

  program.srt_reads.push_back({Value(&next), 5});
  program.info.buffers.push_back({.source = 0, .max_byte_extent = 64, .read = true});
  program.info.images.push_back({.source = 1,
                                 .resource_class = ImageResourceClass::Sampled,
                                 .indirect_resources = {1, 2, 3}});
  program.info.samplers.push_back(
      {.source = 1, .depth_compare = true, .integer_border = true});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0, .first_use_pc = 9});
  program.info.inputs.push_back({.kind = StageInputKind::Parameter,
                                 .location = 2,
                                 .component_count = 4,
                                 .debug_name = "attr2"});
  program.info.outputs.push_back({.kind = StageOutputKind::Mrt, .index = 1, .debug_name = "mrt1"});
  program.info.vertex_offset_sgpr = 6;
  program.info.uses_dma = true;
  return ExtractResourcePlan(program);
}

void TestPlanRoundTrip() {
  const auto plan = BuildPlan();
  const auto bytes = DiskCache::SerializePlan(plan, true);
  Check(!bytes.empty(), "plan was not serializable");

  IR::ResourcePlan loaded;
  bool ray_traced = false;
  Check(DiskCache::DeserializePlan(bytes, loaded, ray_traced), "plan did not deserialize");
  Check(ray_traced, "ray_traced flag was lost");
  Check(loaded.shader_hash == plan.shader_hash && loaded.user_data_base == 8 &&
            loaded.user_data_count == 16 && loaded.stage == plan.stage,
        "plan header differs");
  Check(loaded.value_storage.size() == plan.value_storage.size() &&
            loaded.value_storage.size() >= 3,
        "instruction count differs");
  Check(loaded.info == plan.info && loaded.memory_info == plan.memory_info,
        "shader info differs");
  Check(loaded.srt_reads.size() == 1 && loaded.srt_reads[0].flat_offset == 5,
        "srt reads differ");
  Check(loaded.descriptor_sources.size() == 2 &&
            loaded.descriptor_sources[0].indirect_image.has_value() &&
            loaded.descriptor_sources[0].indirect_image->selector_stride == 12 &&
            loaded.descriptor_sources[0].indirect_image->sources.size() == 2 &&
            loaded.descriptor_sources[0].dwords[4] == IR::Value(0xfeedface12345678ull) &&
            loaded.descriptor_sources[0].dwords[5] == IR::Value::F32(1.5f) &&
            loaded.descriptor_sources[0].dwords[6] == IR::Value::F16(0x3c00) &&
            loaded.descriptor_sources[0].dwords[7] == IR::Value(true) &&
            !loaded.descriptor_sources[1].indirect_image.has_value(),
        "descriptor sources differ");
  Check(loaded.requires_specialization_memory == plan.requires_specialization_memory &&
            loaded.capture_specialization_reads == plan.capture_specialization_reads &&
            loaded.clean_flat_slots == plan.clean_flat_slots && loaded.srt_plan_complete &&
            loaded.resource_tracking_complete &&
            loaded.control_flow.size() == plan.control_flow.size(),
        "plan flags differ");
  for (size_t i = 0; i < loaded.control_flow.size(); ++i) {
    Check(loaded.control_flow[i].successors == plan.control_flow[i].successors &&
              loaded.control_flow[i].sources == plan.control_flow[i].sources &&
              loaded.control_flow[i].srt_reads == plan.control_flow[i].srt_reads,
          "control flow differs");
  }
  const auto *phi = loaded.descriptor_sources[0].dwords[0].TryInstruction();
  Check(phi != nullptr && phi->GetOpcode() == IR::ValueOpcode::Phi && phi->NumArgs() == 2,
        "phi lost");
  const auto *add = phi->Arg(1).TryInstruction();
  Check(add != nullptr && add->GetOpcode() == IR::ValueOpcode::IAdd32 &&
            add->Arg(0).TryInstruction() == phi,
        "phi loop edge lost");

  // Serializing what was loaded must reproduce the original bytes exactly.
  Check(DiskCache::SerializePlan(loaded, true) == bytes, "round trip changed the plan");
}

void TestPlanRejectsDamagedInput() {
  const auto plan = BuildPlan();
  const auto bytes = DiskCache::SerializePlan(plan, false);
  IR::ResourcePlan loaded;
  bool ray_traced = false;
  for (size_t size = 0; size < bytes.size(); size += 7) {
    Check(!DiskCache::DeserializePlan({bytes.data(), size}, loaded, ray_traced),
          "truncated plan was accepted");
  }
  auto padded = bytes;
  padded.push_back(0);
  Check(!DiskCache::DeserializePlan(padded, loaded, ray_traced), "trailing bytes were accepted");
  auto wrong_version = bytes;
  wrong_version[4] ^= 0xffu;
  Check(!DiskCache::DeserializePlan(wrong_version, loaded, ray_traced),
        "wrong format version was accepted");
  // Corrupt every byte in turn: the reader may accept (the checksum lives in the file layer) but
  // must never crash or read out of bounds.
  for (size_t i = 0; i < bytes.size(); ++i) {
    auto damaged = bytes;
    damaged[i] ^= 0xa5u;
    (void)DiskCache::DeserializePlan(damaged, loaded, ray_traced);
  }
}

void TestPermutationRoundTrip() {
  IR::CompiledShaderInfo info;
  info.stage = Libs::Graphics::ShaderType::Pixel;
  info.shader_hash = 0xdeadbeefcafef00dull;
  info.wave_size = 32;
  info.user_data_base = 8;
  info.scratch_dwords = 3;
  info.param_export_mask = 0x5;
  info.has_address_writes = true;
  info.info.buffers.push_back({.source = 2, .written = true});
  info.info.outputs.push_back({.kind = IR::StageOutputKind::Parameter, .index = 2});
  info.bindings.push_data_start_dword = 4;
  info.bindings.memory_offset_dword = 1;
  info.bindings.memory_offset_count = 5;
  info.bindings.user_data_registers = {0, 1, 7};
  info.bindings.descriptors.push_back({IR::DescriptorBindingKind::Buffers, {0, 1}});
  const std::vector<uint32_t> spirv {0x07230203, 0x00010500, 0, 42, 0};

  const auto bytes = DiskCache::SerializePermutation(info, spirv);
  IR::CompiledShaderInfo loaded;
  std::vector<uint32_t> words;
  Check(DiskCache::DeserializePermutation(bytes, loaded, words), "permutation did not load");
  Check(words == spirv, "SPIR-V differs");
  Check(loaded.stage == info.stage && loaded.shader_hash == info.shader_hash &&
            loaded.wave_size == 32 && loaded.user_data_base == 8 &&
            loaded.scratch_dwords == 3 && loaded.param_export_mask == 5 &&
            loaded.has_address_writes &&
            loaded.info == info.info && loaded.bindings == info.bindings,
        "permutation info differs");
  for (size_t size = 0; size < bytes.size(); ++size) {
    Check(!DiskCache::DeserializePermutation({bytes.data(), size}, loaded, words),
          "truncated permutation was accepted");
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

int main() {
  TestPlanRoundTrip();
  TestPlanRejectsDamagedInput();
  TestPermutationRoundTrip();
  std::puts("ShaderDiskCacheTests: all cases passed");
  return 0;
}

// Self-contained like ResourceMaterializationTests: amalgamate the typed-IR implementation.
#include "graphics/shader/recompiler/ShaderDiskCache.cpp"
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
