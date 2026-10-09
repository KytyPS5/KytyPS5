#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ResourceMaterializationTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, std::span<uint32_t>) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  result->id = static_cast<uint32_t>(program.blocks.size() - 1u);
  program.block_storage.push_back(std::move(block));
  return *result;
}

Libs::Graphics::ShaderRecompiler::IR::Program SrtProgram(uint64_t address) {
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
  return program;
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

Libs::Graphics::ShaderRecompiler::IR::Program MixedSamplerProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);

  const auto AddSource = [&program](uint32_t dword_count) {
    DescriptorSource source;
    source.dword_count = dword_count;
    for (uint32_t i = 0; i < dword_count; i++) {
      source.dwords[i] = Value(0u);
    }
    program.descriptor_sources.push_back(source);
    return static_cast<uint32_t>(program.descriptor_sources.size() - 1u);
  };

  const auto image0 = AddSource(8);
  const auto image1 = AddSource(8);
  const auto sampler0 = AddSource(4);
  const auto sampler1 = AddSource(4);
  program.descriptor_sources[image1].dwords[0] = Value(1u);
  program.descriptor_sources[image1].dwords[1] = Value(static_cast<uint32_t>(
      Libs::Graphics::Prospero::BufferFormat::k11_11_10UInt) << 20u);
  program.descriptor_sources[image1].dwords[3] = Value(static_cast<uint32_t>(
      Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  for (uint32_t index = 0; index < 2; ++index) {
    auto &value = block.AppendNewInst(
        ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(index))});
    program.descriptor_sources[sampler0 + index].dwords[0] = Value(&value);
  }
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
           Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  program.info.samplers.push_back({.source = sampler0});
  program.info.samplers.push_back({.source = sampler1});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 0});
  program.info.sampled_pairs.push_back({.image = 0, .sampler = 1});
  program.info.sampled_pairs.push_back({.image = 1, .sampler = 1});
  return program;
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const uint32_t dword = 0x12345678;
  auto plan = ExtractResourcePlan(SrtProgram(reinterpret_cast<uint64_t>(&dword)));
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

void TestIntegerRuntimeValueFollowsSrtReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = SrtProgram(0x10000);
  const auto root = plan.descriptor_sources.front().dwords[0];
  Check(ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT read was rejected");

  Block values;
  auto &comparison = values.AppendNewInst(ValueOpcode::FPOrdLessThanEqual32,
                                          {Value::F32(1.f), Value::F32(0.f)});
  auto &selection = values.AppendNewInst(
      ValueOpcode::SelectU32, {Value(&comparison), Value(1u), Value(0u)});
  plan.srt_reads[0].value = Value(&selection);
  Check(ValidateRuntimeValue(plan, root),
        "ordinary SRT validation rejected a floating-point dependency");
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "integer SRT validation missed a hidden floating-point dependency");

  auto &first =
      values.AppendNewInst(ValueOpcode::ReadFirstLane, {root, Value(true)});
  Check(!ValidateRuntimeValue(plan, Value(&first), RuntimeValueType::Integer),
        "read-first-lane lost integer-only SRT validation");

  auto &active = values.AppendNewInst(ValueOpcode::ReadFirstLane,
                                      {Value(&selection), Value(&comparison)});
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point execution mask was accepted as integer-only");

  auto &lane = values.AppendNewInst(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  auto &mask =
      values.AppendNewInst(ValueOpcode::INotEqual32, {Value(&lane), Value(0u)});
  selection.SetArg(0, Value(&mask));
  active.SetArg(1, Value(&mask));
  Check(ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "nonuniform integer execution mask was rejected");
  auto &float_value =
      values.AppendNewInst(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  selection.SetArg(2, Value(&float_value));
  Check(!ValidateRuntimeValue(plan, Value(&active), RuntimeValueType::Integer),
        "floating-point inactive arm was accepted as integer-only");

  plan.srt_reads[0].value = Value(&first);
  Check(!ValidateRuntimeValue(plan, root, RuntimeValueType::Integer),
        "cyclic SRT read-first-lane dependency was accepted");
}

void TestSrtAliasesRetainReadPolicy() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = SrtProgram(0x1000u);
  auto &block = *program.blocks[0];
  const auto append_read = [&](uint32_t address) {
    auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                       {Value(address), Value(0u)});
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(0u), Value(0u), Value(true)});
    // Different reads deliberately share MemoryInfo but have different guest PCs.
    read.SetFlags(MemoryFlags{.index = 0, .pc = address});
    const auto slot = static_cast<uint32_t>(program.srt_reads.size());
    program.srt_reads.push_back({Value(&read), slot});
    auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
    return Value(&block.AppendNewInst(ValueOpcode::ReadConst,
                                      {Value(&srt), Value(slot)}));
  };
  const auto ordinary = append_read(0x2000u);
  const auto pointer = append_read(0x3000u);
  program.srt_reads[0].value.Instruction()->Arg(0).Instruction()->SetArg(0, pointer);
  auto &mask = block.AppendNewInst(ValueOpcode::INotEqual32, {ordinary, Value(0u)});
  auto &first = block.AppendNewInst(ValueOpcode::ReadFirstLane,
                                    {Value(9u), Value(&mask)});
  program.descriptor_sources.push_back({.dwords = {Value(&first), Value(0u)},
                                       .dword_count = 2});
  DescriptorSource indirect;
  indirect.indirect_descriptor.emplace().sources = {0, 1};
  program.descriptor_sources.push_back(indirect);
  auto plan = ExtractResourcePlan(program);
  for (const auto &inst : plan.value_storage) {
    Check(inst.GetOpcode() != ValueOpcode::ReadConst &&
              inst.GetOpcode() != ValueOpcode::GetSrtResource,
          "retained plan still contains flattened-read aliases");
  }
  Check(plan.descriptor_sources[0].dwords[0] == plan.srt_reads[0].value &&
            plan.srt_reads[0].value.Instruction()->Flags<SrtReadFlags>().clean == 1u &&
            plan.srt_reads[1].value.Instruction()->Flags<SrtReadFlags>().clean == 0u &&
            plan.srt_reads[2].value.Instruction()->Flags<SrtReadFlags>().clean == 1u,
        "alias normalization lost read identity or retained discarded EXEC provenance");
  struct Reads { uint32_t strict = 0; uint32_t ordinary = 0; bool dirty = false; } reads;
  const SrtRuntime runtime{
      .read_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        ++static_cast<Reads *>(data)->ordinary;
        if (address == 0x2000u) words[0] = 0x2222u;
        else if (address == 0x3000u) words[0] = 0x8000u;
        else if (address == 0x8000u) words[0] = 0xeeeeu;
        else return false;
        return true;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address,
                                        std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.strict;
        if (reads.dirty) return false;
        if (address == 0x3000u) words[0] = 0x1000u;
        else if (address == 0x1000u) words[0] = 0x1111u;
        else return false;
        return true;
      }};
  std::vector<uint32_t> flat;
  DescriptorValue descriptor;
  {
    SrtWalker clean(plan, CleanRuntime(runtime));
    SrtWalker walker(plan, runtime, &clean);
    Check(walker.RefreshFlatBuffer(flat) &&
              flat == std::vector<uint32_t>{0x1111u, 0x2222u, 0x1000u} &&
              walker.EvaluateDescriptor(0, descriptor) && descriptor.dwords[0] == 0x1111u &&
              reads.strict == 2 && reads.ordinary == 1,
          "normalized aliases changed nested strict reads or repeated a shared read");
  }
  Check(!SrtWalker(plan, runtime).RefreshFlatBuffer(flat),
        "strict flat read accepted a missing clean evaluator");
  auto no_reader = runtime;
  no_reader.read_specialization_memory = nullptr;
  {
    SrtWalker clean(plan, CleanRuntime(no_reader));
    Check(!SrtWalker(plan, no_reader, &clean).RefreshFlatBuffer(flat),
          "strict flat read accepted a missing strict reader");
  }
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(0, descriptor) &&
            descriptor.dwords[0] == 0xeeeeu && reads.strict == 2 && reads.ordinary == 3,
        "direct descriptor evaluation without a clean evaluator changed read domains");
  reads.dirty = true;
  {
    SrtWalker clean(plan, CleanRuntime(runtime));
    Check(!SrtWalker(plan, runtime, &clean).RefreshFlatBuffer(flat) &&
              reads.strict == 3 && reads.ordinary == 3,
          "dirty strict pointer fell back to an ordinary read");
  }
}

void TestUniformVectorDescriptorRead() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  auto &handle = block.AppendNewInst(ValueOpcode::GetBufferResource,
      {Value(0x1000u), Value(4u << 16u), Value(1u), Value(0x16204u)});
  program.memory_info.push_back({.kind = ResourceKind::Buffer});
  auto &count = block.AppendNewInst(ValueOpcode::LoadBufferU32,
      {Value(&handle), Value(0u), Value(0u), Value(0u), Value(true)});
  count.SetFlags(MemoryFlags{.index = 0});
  // The captured indirect kernel shares one read across sibling scalar lane reads,
  // enclosed by a different EXEC mask. Its resource plan must share that read too.
  auto &lane = block.AppendNewInst(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  auto &active = block.AppendNewInst(ValueOpcode::ULessThan32, {Value(&lane), Value(32u)});
  auto &first = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&count), Value(true)});
  auto &next = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&count), Value(1u)});
  auto &second = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&next), Value(true)});
  auto &sum = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&first), Value(&second)});
  auto &small = block.AppendNewInst(ValueOpcode::ULessThanEqual32, {Value(&count), Value(72u)});
  auto &selected = block.AppendNewInst(ValueOpcode::SelectU32, {Value(&small), Value(&sum), Value(&count)});
  auto &masked = block.AppendNewInst(ValueOpcode::SelectU32, {Value(&active), Value(&selected), Value(0u)});
  auto &records = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&masked), Value(&active)});
  DescriptorSource source;
  source.dword_count = 4;
  source.dwords = {Value(0x2000u), Value(4u << 16u), Value(&records), Value(0x16204u)};
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  Check(ValidateRuntimeValue(program, Value(&count)), "uniform DWORD count was rejected");
  struct Reads { uint32_t value = 72; uint32_t strict = 0; uint32_t ordinary = 0; bool clean = true; } reads;
  const SrtRuntime runtime{
      .read_memory = [](void *data, uint64_t, std::span<uint32_t> words) {
        ++static_cast<Reads *>(data)->ordinary;
        words[0] = 999;
        return true;
      },
      .userdata = &reads,
      .read_specialization_memory = [](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.strict;
        if (!reads.clean || address != 0x1000u || words.size() != 1) return false;
        words[0] = reads.value;
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const bool written : {false, true}) {
    program.info.buffers[0].written = written;
    auto plan = ExtractResourcePlan(program);
    Check(plan.control_flow.empty() && plan.requires_specialization_memory &&
              plan.capture_specialization_reads,
          "vector descriptor read depended on incidental control-flow capture");
    reads = {};
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers[0].dwords[2] == 145 && reads.strict == 1 && reads.ordinary == 0 &&
              snapshot.specialization_reads ==
                  std::vector<std::pair<uint64_t, uint64_t>>{{0x1000u, 4u}},
          "vector descriptor input was not read and captured exactly once");
    reads.clean = false;
    reads.strict = 0;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
              reads.strict == 1 && reads.ordinary == 0,
          "dirty vector descriptor input fell back to an ordinary memory read");
  }
  count.SetArg(4, Value(false));
  auto &inactive = block.AppendNewInst(ValueOpcode::ReadFirstLane, {Value(&count), Value(false)});
  program.descriptor_sources.push_back({.dwords = {Value(&inactive)}, .dword_count = 1});
  reads.strict = 0;
  uint32_t result = 99;
  {
    const auto plan = ExtractResourcePlan(program);
    Check(SrtWalker(plan, runtime).Evaluate(plan.descriptor_sources.back().dwords[0], result) &&
              result == 0 && reads.strict == 0 && reads.ordinary == 0,
          "literal false EXEC read vector memory");
  }
  count.SetArg(4, Value(true));
  handle.SetArg(3, Value(0x204u));
  program.descriptor_sources.back().dwords[0] = Value(&count);
  {
    const auto plan = ExtractResourcePlan(program);
    Check(SrtWalker(plan, runtime).Evaluate(plan.descriptor_sources.back().dwords[0], result) &&
              result == 0 && reads.strict == 0 && reads.ordinary == 0,
          "invalid vector buffer format read memory");
  }
  handle.SetArg(3, Value(0x16204u));
  count.SetArg(1, Value(&lane));
  Check(!ValidateRuntimeValue(program, Value(&count)),
        "varying vector address was treated as a uniform descriptor read");
}

void TestExactReciprocalDescriptorArithmetic() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  auto &block = AddValueBlock(program);
  for (const float divisor : {64.f, 128.f, 256.f, 512.f,
                              std::numeric_limits<float>::min(),
                              std::bit_cast<float>(253u << 23u)}) {
    auto &reciprocal = block.AppendNewInst(ValueOpcode::FPRecipIFlag32, {Value::F32(divisor)});
    uint32_t result = 0;
    Check(SrtWalker(program, {}).Evaluate(Value(&reciprocal), result) &&
              result == std::bit_cast<uint32_t>(1.f / divisor),
          "power-of-two reciprocal was not exact");
  }
  for (const float divisor : {0.f, 3.f, -64.f, std::numeric_limits<float>::infinity(),
                              std::numeric_limits<float>::denorm_min(),
                              std::bit_cast<float>(254u << 23u)}) {
    auto &reciprocal = block.AppendNewInst(ValueOpcode::FPRecipIFlag32, {Value::F32(divisor)});
    uint32_t result = 0;
    Check(!SrtWalker(program, {}).Evaluate(Value(&reciprocal), result),
          "unsupported reciprocal rounding or exceptional input was accepted");
  }
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

void TestWrittenDescriptorPredicateReads(bool memory_condition, bool loop) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 1;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &offset = block.AppendNewInst(ValueOpcode::GetUserData,
                                     {Value(static_cast<ScalarReg>(0))});
  auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
      {Value(&handle), Value(&offset), Value(0u), Value(false)});
  read.SetFlags(MemoryFlags{.index = 0});
  DescriptorSource source;
  source.dwords = {Value(&read), Value(0u), Value(4u), Value(0u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  // Only memory-derived host decisions need clean, disjoint descriptor reads.
  auto &condition = block.AppendNewInst(ValueOpcode::IEqual32,
      {memory_condition ? Value(&read) : Value(&offset),
       Value(memory_condition ? 0x8000u : 4u)});
  auto &store_block = AddValueBlock(program);
  AddValueBlock(program);
  program.blocks[0]->condition = Value(&condition);
  program.blocks[0]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  program.blocks[0]->terminator.true_block = program.blocks[1];
  program.blocks[0]->terminator.false_block = program.blocks[2];

  program.blocks[1]->id = 1;
  program.blocks[1]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  program.blocks[2]->id = 2;
  program.blocks[2]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  if (loop) {
    // The predicate reaches the store through a continue block, then loops back.
    auto &continued = AddValueBlock(program);
    program.blocks[0]->terminator.true_block = &continued;
    continued.terminator.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Branch;
    continued.terminator.true_block = &store_block;
    continued.AddBranch(&store_block);
    store_block.terminator.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Branch;
    store_block.terminator.true_block = &block;
    store_block.AddBranch(&block);
  }
  block.AddBranch(program.blocks[0]->terminator.true_block);
  block.AddBranch(program.blocks[2]);
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  auto &output = store_block.AppendNewInst(ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
  store_block.AppendNewInst(ValueOpcode::StoreBufferU32,
      {Value(&output), Value(0u), Value(0u), Value(0u), Value(1u), Value(true)})
      .SetFlags(MemoryFlags{.index = 1});
  auto plan = ExtractResourcePlan(program);
  Check(plan.capture_specialization_reads == memory_condition && !plan.control_flow.empty(),
        "conditional writable descriptor used the wrong memory dependency policy");
  struct Reads { uint32_t ordinary = 0; uint32_t strict = 0; bool clean = false; } reads;
  const std::array<uint32_t, 1> user_data{4u};
  const SrtRuntime runtime{
      .user_data = user_data,
      .read_memory = +[](void *data, uint64_t, std::span<uint32_t> words) {
        ++static_cast<Reads *>(data)->ordinary;
        words[0] = 0x8000u;
        return true;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.strict;
        if (!reads.clean || address != 0x1004u) return false;
        words[0] = 0x8000u;
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const bool clean : {false, true}) {
    reads = {.clean = clean};
    // A failed predicate explores both edges and the writable descriptor retries its clean read.
    const bool expected = !memory_condition || clean;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) == expected &&
              reads.ordinary == (memory_condition ? 0u : 1u) &&
              reads.strict == (memory_condition ? (clean ? 1u : 2u) : 0u),
          "writable descriptor changed reader policy or bypassed a dirty memory predicate");
    if (!expected) continue;
    Check(snapshot.buffers[0].dwords[0] == 0x8000u &&
              snapshot.specialization_reads == (memory_condition
                  ? std::vector<std::pair<uint64_t, uint64_t>>{{0x1004u, 4u}}
                  : std::vector<std::pair<uint64_t, uint64_t>>{}),
          "writable descriptor lost its address or captured an immutable predicate");
  }
}

void TestFailedMaterializationRejectsStage() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, {}, snapshot, specialization),
        "missing runtime user data did not reject the cached stage");
}

void TestFiniteImageRefreshReusesScalarReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
  for (uint32_t index = 0; index < 3; ++index) {
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(index * 4u), Value(0u), Value(true)});
    read.SetFlags(MemoryFlags{.index = 0});
    program.srt_reads.push_back({Value(&read), index});
    auto &flat = block.AppendNewInst(ValueOpcode::ReadConst,
                                     {Value(&srt), Value(index)});
    DescriptorSource source;
    source.dword_count = 8;
    source.dwords.fill(Value(0u));
    source.dwords[0] = Value(&flat);
    source.dwords[1] = Value(static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u);
    source.dwords[3] = Value(Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u));
    program.descriptor_sources.push_back(source);
  }
  DescriptorSource root;
  root.dword_count = 8;
  root.dwords.fill(Value(0u));
  root.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{}).sources = {0, 1, 2, 1};
  program.descriptor_sources.push_back(root);
  program.info.images.push_back({
      .source = 3,
      .resource_class = ImageResourceClass::Sampled,
      .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
      .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D});
  auto plan = ExtractResourcePlan(program);
  struct Reads {
    std::array<uint32_t, 3> words{0x100u, 0x200u, 0x200u};
    std::array<uint32_t, 3> counts{};
    uint32_t ordinary = 0;
  } reads;
  const SrtRuntime runtime{
      .read_memory = +[](void *data, uint64_t, std::span<uint32_t>) {
        ++static_cast<Reads *>(data)->ordinary;
        return false;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address,
                                        std::span<uint32_t> words) {
        if (words.size() != 1 || address < 0x1000u || address >= 0x100cu ||
            (address & 3u) != 0) return false;
        auto &reads = *static_cast<Reads *>(data);
        const auto index = (address - 0x1000u) / 4u;
        ++reads.counts[index];
        words[0] = reads.words[index];
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto capacities = [&] {
    return std::array{snapshot.images.capacity(), snapshot.flattened_srt.capacity(),
                      snapshot.specialization_reads.capacity(), specialization.images.capacity()};
  };
  const auto check_mapping = [&](std::array<uint32_t, 4> ordinals) {
    const auto offset = specialization.images[0].indirect_mapping_offset;
    Check(snapshot.images.size() == 2 && snapshot.flattened_srt[offset] == 4,
          "finite image candidates were not deduplicated");
    for (uint32_t key = 0; key < ordinals.size(); ++key) {
      Check(snapshot.flattened_srt[offset + 1u + key * 2u] == key &&
                snapshot.flattened_srt[offset + 2u + key * 2u] == ordinals[key],
            "finite image selector mapping is stale or incorrect");
    }
  };
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image materialization failed");
  Check(reads.ordinary == 0 && reads.counts == std::array<uint32_t, 3>{1, 1, 1} &&
            snapshot.specialization_reads.size() == 3,
        "finite image candidates repeated scalar reads or bypassed clean provenance");
  check_mapping({0, 1, 1, 1});
  const auto warm_capacities = capacities();
  reads.words = {0x300u, 0x300u, 0x400u};
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite image refresh failed after descriptor changes");
  Check(reads.ordinary == 0 && reads.counts == std::array<uint32_t, 3>{2, 2, 2} &&
            snapshot.specialization_reads.size() == 3 &&
            snapshot.images[0].dwords[0] == 0x300u && snapshot.images[1].dwords[0] == 0x400u,
        "finite image refresh retained old scalar values or repeated reads");
  check_mapping({0, 0, 1, 0});
  Check(capacities() == warm_capacities,
        "finite image refresh grew reusable resource storage after warmup");
}

void TestMixedSamplerVariantsShareRuntimeDescriptor() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto program = MixedSamplerProgram();
  auto plan = ExtractResourcePlan(program);
  std::array<uint32_t, 2> user_data{0x11111111u, 0x22222222u};
  const SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mixed sampler materialization failed");
  ApplyResourceSpecialization(program, specialization);
  const auto &samplers = program.info.samplers;
  Check(snapshot.samplers.size() == 2 && samplers.size() == 3 &&
            samplers[0].snapshot_index == 0 && samplers[1].snapshot_index == 1 &&
            samplers[2].snapshot_index == 1 &&
            samplers[0].source == plan.info.samplers[0].source &&
            samplers[1].source == plan.info.samplers[1].source &&
            samplers[2].source == samplers[1].source &&
            !samplers[1].force_point_filtering && samplers[2].force_point_filtering &&
            program.info.sampled_pairs[2].sampler == 2,
        "native sampler variants lost their source identity or binding order");
  const auto capacity = snapshot.samplers.capacity();
  user_data[1] = 0x33333333u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.samplers.size() == 2 && snapshot.samplers.capacity() == capacity &&
            snapshot.samplers[samplers[0].snapshot_index].dwords[0] == user_data[0] &&
            snapshot.samplers[samplers[1].snapshot_index].dwords[0] == user_data[1] &&
            snapshot.samplers[samplers[2].snapshot_index].dwords[0] == user_data[1],
        "sampler variants retained stale or duplicated descriptors after refresh");
}

// Two SRT DWORDs read through a user-data pointer, with a buffer descriptor built from them.
Libs::Graphics::ShaderRecompiler::IR::Program PointerSrtProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 2;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &low = block.AppendNewInst(ValueOpcode::GetUserData,
                                  {Value(static_cast<ScalarReg>(0))});
  auto &high = block.AppendNewInst(ValueOpcode::GetUserData,
                                   {Value(static_cast<ScalarReg>(1))});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(&low), Value(&high)});
  for (uint32_t slot = 0; slot < 2; ++slot) {
    auto &read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(slot * 4u), Value(0u), Value(true)});
    read.SetFlags(MemoryFlags{.index = 0});
    program.srt_reads.push_back({Value(&read), slot});
  }
  auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &base = block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(0u)});
  auto &size = block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(1u)});
  auto &records = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&size), Value(1u)});
  DescriptorSource source;
  source.dwords = {Value(&base), Value(0u), Value(&records), Value(0u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return program;
}

void TestWalkerFollowsInputsAcrossWalks() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = ExtractResourcePlan(PointerSrtProgram());
  std::array<uint32_t, 2> first{0x1000u, 7u};
  std::array<uint32_t, 2> second{0x2000u, 9u};
  std::array<uint32_t, 2> user_data{};
  const auto point = [&](const std::array<uint32_t, 2> &table) {
    const auto address = reinterpret_cast<uint64_t>(table.data());
    user_data = {static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u)};
  };
  const SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto walk = [&](uint32_t base, uint32_t size) {
    return MaterializeResources(plan, runtime, snapshot, specialization) &&
           snapshot.flattened_srt == std::vector<uint32_t>{base, size} &&
           snapshot.buffers[0].dwords[0] == base && snapshot.buffers[0].dwords[2] == size + 1u;
  };
  point(first);
  Check(walk(0x1000u, 7u), "first walk read wrong SRT values");
  point(second);
  Check(walk(0x2000u, 9u), "a later walk reused values of another SRT");
  point(first);
  first[1] = 11u;
  Check(walk(0x1000u, 11u), "a later walk reused memory contents of an earlier one");
  auto &replaced = plan.value_storage.emplace_back(ValueOpcode::LoadAddressU32);
  replaced.SetArg(0, plan.srt_reads[0].value.ResolveInstruction()->Arg(0));
  replaced.SetArg(1, Value(0u));
  replaced.SetArg(2, Value(0u));
  replaced.SetArg(3, Value(true));
  replaced.SetFlags(SrtReadFlags{.index = 0});
  plan.srt_reads[1].value = Value(&replaced);
  // The descriptor keeps its own clone of the former read.
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0x1000u, 0x1000u} &&
            snapshot.buffers[0].dwords[2] == 12u,
        "a replaced SRT read value was not followed");
  DescriptorSource extra;
  extra.dwords = {Value(3u), Value(4u)};
  extra.dword_count = 2;
  plan.descriptor_sources.push_back(extra);
  DescriptorValue value;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(1, value) && value.dword_count == 2 &&
            value.dwords[0] == 3u && value.dwords[1] == 4u,
        "an appended descriptor source was not evaluated");
  plan.descriptor_sources[0].dwords[1] = Value(9u);
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.buffers[0].dwords[1] == 9u,
        "a replaced descriptor DWORD was not followed");
}

void TestWalkerRecompilesChangedProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  auto &block = AddValueBlock(program);
  auto &input = block.AppendNewInst(ValueOpcode::GetUserData,
                                    {Value(static_cast<ScalarReg>(0))});
  auto &sum = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&input), Value(1u)});
  const std::array<uint32_t, 1> user_data{40u};
  uint32_t result = 0;
  Check(SrtWalker(program, {.user_data = user_data}).Evaluate(Value(&sum), result) && result == 41u,
        "user-data sum was not evaluated");
  sum.SetArg(1, Value(2u));
  Check(SrtWalker(program, {.user_data = user_data}).Evaluate(Value(&sum), result) && result == 42u,
        "a walk of a changed program used its previous form");
}

void TestWalkerKeepsLazyReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.user_data_count = 1;
  auto &block = AddValueBlock(program);
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                     {Value(0x1000u), Value(0u)});
  const auto read = [&](uint32_t offset) -> Inst & {
    auto &inst = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {Value(&handle), Value(offset), Value(0u), Value(true)});
    inst.SetFlags(MemoryFlags{.index = 0});
    return inst;
  };
  auto &good = read(0u);
  auto &bad = read(4u);
  auto &flag = block.AppendNewInst(ValueOpcode::GetUserData,
                                   {Value(static_cast<ScalarReg>(0))});
  auto &taken = block.AppendNewInst(ValueOpcode::INotEqual32, {Value(&flag), Value(0u)});
  auto &select = block.AppendNewInst(ValueOpcode::SelectU32,
                                     {Value(&taken), Value(&good), Value(&bad)});
  auto &both = block.AppendNewInst(ValueOpcode::LogicalAnd, {Value(&taken), Value(&bad)});
  auto &twice = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&bad), Value(&bad)});
  struct Reads { uint32_t count = 0; } reads;
  std::array<uint32_t, 1> user_data{1u};
  const SrtRuntime runtime{
      .user_data = user_data,
      .read_memory = [](void *data, uint64_t address, std::span<uint32_t> words) {
        ++static_cast<Reads *>(data)->count;
        words[0] = 0x55u;
        return address == 0x1000u;
      },
      .userdata = &reads};
  uint32_t result = 0;
  Check(SrtWalker(program, runtime).Evaluate(Value(&select), result) && result == 0x55u &&
            reads.count == 1,
        "a select evaluated its other operand");
  user_data[0] = 0u;
  reads = {};
  Check(SrtWalker(program, runtime).Evaluate(Value(&both), result) && result == 0u &&
            reads.count == 0,
        "a false left operand did not short-circuit a logical AND");
  reads = {};
  Check(!SrtWalker(program, runtime).Evaluate(Value(&twice), result) && reads.count == 1,
        "a failed read did not stop its consumer");
  reads = {};
  user_data[0] = 1u;
  Check(!SrtWalker(program, runtime).Evaluate(Value(&both), result) && reads.count == 1,
        "a failed right operand of a true logical AND was accepted");
}

// A constant-buffer DWORD past the 48-bit address space fails before reading, as a scalar read
// does; the last DWORD below it is read.
void TestBufferReadPastAddressSpaceFails() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 2;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  const auto user = [&](uint32_t reg) {
    return Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                                      {Value(static_cast<ScalarReg>(reg))}));
  };
  // 16 records without a stride: the DWORD at byte 8 is within the buffer.
  const std::array<Value, 4> words{user(0), user(1), Value(16u), Value(0u)};
  const auto buffer = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
                                                 {words[0], words[1], words[2], words[3]}));
  MemoryInfo info;
  info.kind = ResourceKind::ScalarBuffer;
  info.offset = 8u;
  program.memory_info.push_back(info);
  auto &read = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {buffer, Value(0u)});
  read.SetFlags(SrtReadFlags{.index = 0});
  program.srt_reads.push_back({Value(&read), 0});
  DescriptorSource source;
  source.dwords = {words[0], words[1], words[2], words[3]};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  auto plan = ExtractResourcePlan(program);
  std::array<uint32_t, 2> user_data{};
  std::vector<uint64_t> reads;
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = +[](void *data, uint64_t address, std::span<uint32_t> values) {
                             static_cast<std::vector<uint64_t> *>(data)->push_back(address);
                             values[0] = 0x5au;
                             return true;
                           },
                           .userdata = &reads};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto walk = [&](uint64_t base) {
    user_data = {static_cast<uint32_t>(base), static_cast<uint32_t>(base >> 32u)};
    reads.clear();
    return MaterializeResources(plan, runtime, snapshot, specialization);
  };
  Check(walk(0xfffffffffff0ull) && reads == std::vector<uint64_t>{0xfffffffffff8ull} &&
            snapshot.flattened_srt == std::vector<uint32_t>{0x5au},
        "the last DWORD of the address space was not read");
  Check(!walk(0xfffffffffff8ull) && reads.empty(),
        "a buffer read past the 48-bit address space was read");
}

// Flat-buffer reads with constant offsets: scalar address reads (with an unaligned offset
// operand, a negative immediate and a pointer read from another table) and buffer reads.
Libs::Graphics::ShaderRecompiler::IR::Program FlatReadProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 4;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  const auto memory = [&](ResourceKind kind, uint32_t offset) {
    MemoryInfo info;
    info.kind = kind;
    info.offset = offset;
    program.memory_info.push_back(info);
    return static_cast<uint32_t>(program.memory_info.size() - 1u);
  };
  const auto user = [&](uint32_t reg) {
    return Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                                      {Value(static_cast<ScalarReg>(reg))}));
  };
  const auto read = [&](ValueOpcode op, Value handle, uint32_t offset, uint32_t index) {
    auto &inst = op == ValueOpcode::ReadConstBuffer
                     ? block.AppendNewInst(op, {handle, Value(offset)})
                     : block.AppendNewInst(op, {handle, Value(offset), Value(0u), Value(true)});
    inst.SetFlags(SrtReadFlags{.index = index});
    const auto slot = static_cast<uint32_t>(program.srt_reads.size());
    program.srt_reads.push_back({Value(&inst), slot});
    return Value(&inst);
  };
  const auto table = Value(&block.AppendNewInst(ValueOpcode::GetAddressResource, {user(0), user(1)}));
  read(ValueOpcode::LoadAddressU32, table, 0u, memory(ResourceKind::ScalarAddress, 0xfffffffcu));
  const auto low = read(ValueOpcode::LoadAddressU32, table, 0u, memory(ResourceKind::ScalarAddress, 0u));
  // (8 & ~3) + (6 & ~3): the DWORD at 12.
  const auto high = read(ValueOpcode::LoadAddressU32, table, 6u, memory(ResourceKind::ScalarAddress, 8u));
  const auto chased = Value(&block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high}));
  read(ValueOpcode::LoadAddressU32, chased, 4u, memory(ResourceKind::ScalarAddress, 4u));
  // The buffer's last two DWORDs are read from the table, only through the buffer's handle.
  const auto nested = [&](uint32_t offset) {
    auto &inst = block.AppendNewInst(ValueOpcode::LoadAddressU32, {table, Value(0u), Value(0u), Value(true)});
    inst.SetFlags(SrtReadFlags{.index = memory(ResourceKind::ScalarAddress, offset)});
    return Value(&inst);
  };
  const auto buffer = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
                                                 {user(2), user(3), nested(20u), nested(16u)}));
  // (4 & ~3) + (6 & ~3): the DWORD at byte 8.
  const auto first = read(ValueOpcode::ReadConstBuffer, buffer, 6u, memory(ResourceKind::ScalarBuffer, 4u));
  DescriptorSource source;
  source.dwords = {low, high, first, Value(0u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return program;
}

void TestFlatReadsKeepReadSemantics() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = ExtractResourcePlan(FlatReadProgram());
  std::array<uint32_t, 8> table{};
  std::array<uint32_t, 4> chased{0x11u, 0x22u, 0x33u, 0x44u};
  std::array<uint32_t, 4> buffer{0x51u, 0x52u, 0x53u, 0x54u};
  const auto address = [](const auto &words, size_t word = 0) {
    return reinterpret_cast<uint64_t>(words.data() + word);
  };
  // The table pointer is one DWORD and two bytes in, aligned down by the reads: the negative
  // immediate reads table[0], then 0 and 12 read table[1] and table[4], the address of the
  // chased table, and 20 and 16 read table[6] and table[5], the buffer's last two DWORDs.
  table[0] = 0x99u;
  table[1] = static_cast<uint32_t>(address(chased));
  table[4] = static_cast<uint32_t>(address(chased) >> 32u);
  std::array<uint32_t, 4> user_data{};
  const auto point = [&](uint64_t table_address, uint32_t records, uint32_t stride) {
    const auto buffer_address = address(buffer) + 1u;
    user_data = {static_cast<uint32_t>(table_address), static_cast<uint32_t>(table_address >> 32u),
                 static_cast<uint32_t>(buffer_address),
                 static_cast<uint32_t>(buffer_address >> 32u) | (stride << 16u)};
    table[6] = records;
  };
  struct Reads {
    std::vector<uint64_t>                       addresses;
    std::vector<std::pair<uint64_t, uint64_t>> mapped;
    uint64_t                                    fail = 0;
  } reads;
  reads.mapped = {{address(table), sizeof(table)}, {address(chased), sizeof(chased)},
                  {address(buffer), sizeof(buffer)}};
  const SrtRuntime runtime{
      .user_data = user_data,
      .read_memory = [](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        reads.addresses.push_back(address);
        for (const auto [base, size] : reads.mapped) {
          if (address >= base && address - base + words.size_bytes() <= size) {
            std::memcpy(words.data(), reinterpret_cast<const void *>(address), words.size_bytes());
            return address != reads.fail;
          }
        }
        return false;
      },
      .userdata = &reads};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const std::vector<uint64_t> order{address(table, 0),  address(table, 1), address(table, 4),
                                    address(chased, 2), address(table, 6), address(table, 5),
                                    address(buffer, 2)};
  point(address(table, 1) + 2u, 4u, 4u);
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0x99u, table[1], table[4], 0x33u, 0x53u} &&
            reads.addresses == order && snapshot.buffers[0].dwords[2] == 0x53u,
        "flat reads changed their addresses, order or values");
  Check(plan.walker_reads.size() == 5u &&
            std::ranges::all_of(plan.walker_reads, [](const auto &read) { return read.low != UINT32_MAX; }),
        "flat reads with constant offsets were not prepared for the direct path");
  chased[2] = 0x66u;
  buffer[2] = 0x67u;
  reads.addresses.clear();
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.flattened_srt[3] == 0x66u && snapshot.flattened_srt[4] == 0x67u &&
            reads.addresses == order,
        "a later walk kept flat reads of an earlier one");
  reads.addresses.clear();
  reads.fail = address(chased, 2);
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads.addresses == std::vector<uint64_t>(order.begin(), order.begin() + 4),
        "a failed flat read did not stop the walk");
  reads.addresses.clear();
  reads.fail = address(table, 5);
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads.addresses == std::vector<uint64_t>(order.begin(), order.begin() + 6),
        "a failed read of a buffer's fourth DWORD did not stop the walk");
  reads.fail = 0;
  // Without a stride, records are bytes: the DWORD at byte 8 needs 12 of them.
  for (const uint32_t records : {4u, 11u}) {
    point(address(table, 1), records, 0u);
    reads.addresses.clear();
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
              reads.addresses == std::vector<uint64_t>(order.begin(), order.begin() + 6),
          "an out-of-bounds buffer read was accepted");
  }
  point(address(table, 1), 12u, 0u);
  reads.addresses.clear();
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads.addresses == order && snapshot.flattened_srt[4] == 0x67u,
        "the last DWORD of a buffer was rejected");
  // A negative immediate below address 0 fails before reading.
  point(0u, 4u, 4u);
  reads.addresses.clear();
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) && reads.addresses.empty(),
        "an underflowing scalar address was read");
}

// A written buffer whose base and size are flat reads of a table, in a block that branches on
// the size: the plan captures its reads, as the branch depends on memory, and these two reads are
// clean. A third flat read feeds no descriptor.
Libs::Graphics::ShaderRecompiler::IR::Program CleanFlatReadProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 2;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  const auto user = [&](uint32_t reg) {
    return Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                                      {Value(static_cast<ScalarReg>(reg))}));
  };
  const auto table = Value(&block.AppendNewInst(ValueOpcode::GetAddressResource, {user(0), user(1)}));
  const auto read = [&](uint32_t offset) {
    MemoryInfo info;
    info.kind = ResourceKind::ScalarAddress;
    info.offset = offset;
    program.memory_info.push_back(info);
    auto &inst = block.AppendNewInst(ValueOpcode::LoadAddressU32,
        {table, Value(0u), Value(0u), Value(true)});
    inst.SetFlags(SrtReadFlags{.index = static_cast<uint32_t>(program.memory_info.size() - 1u)});
    const auto slot = static_cast<uint32_t>(program.srt_reads.size());
    program.srt_reads.push_back({Value(&inst), slot});
    // The shader reads the flattened slot in this block.
    auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
    return Value(&block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(slot)}));
  };
  const auto base = read(0u);
  const auto size = read(8u);
  read(12u);
  DescriptorSource source;
  source.dwords = {base, Value(0u), size, Value(0u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .written = true});
  auto &condition = block.AppendNewInst(ValueOpcode::IEqual32, {size, Value(64u)});
  auto &store_block = AddValueBlock(program);
  AddValueBlock(program);
  program.blocks[0]->condition = Value(&condition);
  program.blocks[0]->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                                   .true_block = program.blocks[1],
                                   .false_block = program.blocks[2]};
  program.blocks[1]->terminator.kind = CFG::TerminatorKind::Return;
  program.blocks[2]->terminator.kind = CFG::TerminatorKind::Return;
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  auto &output = store_block.AppendNewInst(ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
  store_block.AppendNewInst(ValueOpcode::StoreBufferU32,
      {Value(&output), Value(0u), Value(0u), Value(0u), Value(1u), Value(true)})
      .SetFlags(MemoryFlags{.index = static_cast<uint32_t>(program.memory_info.size() - 1u)});
  return program;
}

struct CountedReads {
  uint32_t ordinary = 0;
  uint32_t strict = 0;
};

// Both readers read host memory directly and count their reads.
Libs::Graphics::ShaderRecompiler::IR::SrtRuntime CountingRuntime(std::span<const uint32_t> user_data,
                                                                  CountedReads &reads) {
  return {.user_data = user_data,
          .read_memory = +[](void *data, uint64_t address, std::span<uint32_t> values) {
            ++static_cast<CountedReads *>(data)->ordinary;
            std::memcpy(values.data(), reinterpret_cast<const void *>(address), values.size_bytes());
            return true;
          },
          .userdata = &reads,
          .read_specialization_memory = +[](void *data, uint64_t address, std::span<uint32_t> values) {
            ++static_cast<CountedReads *>(data)->strict;
            std::memcpy(values.data(), reinterpret_cast<const void *>(address), values.size_bytes());
            return true;
          }};
}

// Clean flat reads take the direct path in the clean evaluator, through the strict reader, and
// the other one through the ordinary reader. The walk captures all three; the branch reuses the
// memo of the size.
void TestCleanFlatReadsUseTheStrictReader() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = ExtractResourcePlan(CleanFlatReadProgram());
  const auto clean = [&](size_t slot) {
    return plan.srt_reads[slot].value.Instruction()->Flags<SrtReadFlags>().clean;
  };
  Check(plan.capture_specialization_reads && plan.srt_reads.size() == 3u && clean(0) == 1u &&
            clean(1) == 1u && clean(2) == 0u,
        "written buffer reads are not clean in a capturing plan");
  std::array<uint32_t, 4> words{0x9000u, 0u, 64u, 0x77u};
  const auto address = reinterpret_cast<uint64_t>(words.data());
  const std::array<uint32_t, 2> user_data{static_cast<uint32_t>(address),
                                          static_cast<uint32_t>(address >> 32u)};
  CountedReads reads;
  const auto runtime = CountingRuntime(user_data, reads);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            std::ranges::all_of(plan.walker_reads, [](const auto &read) { return read.low != UINT32_MAX; }) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0x9000u, 64u, 0x77u} &&
            snapshot.buffers[0].dwords[0] == 0x9000u && snapshot.buffers[0].dwords[2] == 64u &&
            reads.ordinary == 1u && reads.strict == 2u &&
            snapshot.specialization_reads ==
                std::vector<std::pair<uint64_t, uint64_t>>{{address, 4u}, {address + 8u, 4u}, {address + 12u, 4u}},
        "flat reads bypassed their reader or its capture");
}

// A constant-buffer read below its base (negative immediate) fails after evaluating its handle:
// it is not prepared for the direct path, which would read it.
void TestNegativeBufferImmediateIsNotRead() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.user_data_count = 4;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  const auto user = [&](uint32_t reg) {
    return Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                                      {Value(static_cast<ScalarReg>(reg))}));
  };
  const auto buffer = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
                                                 {user(0), user(1), user(2), user(3)}));
  MemoryInfo info;
  info.kind = ResourceKind::ScalarBuffer;
  info.offset = 0x80000000u;
  program.memory_info.push_back(info);
  auto &inst = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {buffer, Value(0u)});
  inst.SetFlags(SrtReadFlags{.index = 0});
  program.srt_reads.push_back({Value(&inst), 0});
  DescriptorSource source;
  source.dwords = {user(0), user(1), user(2), user(3)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  auto plan = ExtractResourcePlan(program);
  // The largest stride and record count: the read would be in bounds.
  std::array<uint32_t, 4> words{};
  const auto address = reinterpret_cast<uint64_t>(words.data());
  const std::array<uint32_t, 4> user_data{static_cast<uint32_t>(address),
                                          static_cast<uint32_t>(address >> 32u) | (0x3fffu << 16u),
                                          UINT32_MAX, 0u};
  uint32_t reads = 0;
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = +[](void *data, uint64_t, std::span<uint32_t>) {
                             ++*static_cast<uint32_t *>(data);
                             return false;
                           },
                           .userdata = &reads};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            plan.walker_reads.size() == 1u && plan.walker_reads[0].low == UINT32_MAX && reads == 0u,
        "a constant-buffer read below its base was read");
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
  TestMappedSrtUsesDirectReaderByDefault();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestSrtAliasesRetainReadPolicy();
  TestUniformVectorDescriptorRead();
  TestExactReciprocalDescriptorArithmetic();
  TestUnbasedFlatCacheHitMaterializes();
  for (const bool memory_condition : {false, true})
    for (const bool loop : {false, true})
      TestWrittenDescriptorPredicateReads(memory_condition, loop);
  TestFailedMaterializationRejectsStage();
  TestFiniteImageRefreshReusesScalarReads();
  TestMixedSamplerVariantsShareRuntimeDescriptor();
  TestWalkerFollowsInputsAcrossWalks();
  TestWalkerRecompilesChangedProgram();
  TestWalkerKeepsLazyReads();
  TestBufferReadPastAddressSpaceFails();
  TestFlatReadsKeepReadSemantics();
  TestCleanFlatReadsUseTheStrictReader();
  TestNegativeBufferImmediateIsNotRead();
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
