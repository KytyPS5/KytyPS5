#include "graphics/host_gpu/renderer/cache/bindlessTranslation.h"
#include "graphics/host_gpu/renderer/pipeline/shaderReadCache.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterializationMemo.h"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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
      // Both pipeline readers synchronize with the GPU before reading, so the ordinary reader
      // sees the same current count the strict one does.
      .read_memory = [](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.ordinary;
        if (address != 0x1000u || words.size() != 1) return false;
        words[0] = reads.value;
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
    Check(plan.control_flow.empty(),
          "vector descriptor read depended on incidental control-flow capture");
    reads = {};
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers[0].dwords[2] == 145 && reads.strict + reads.ordinary >= 1,
          "vector descriptor input was not read");
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
  // An invalid-format V# is host-evaluated like any other; only the in-shader decode zeroes it.
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

void TestWrittenDescriptorPredicateReads(bool memory_condition) {
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
    // A written descriptor always takes the strict reader (c6ff4a35e). A failed memory
    // predicate explores both edges, and the writable descriptor retries its clean read.
    const bool expected = clean;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) == expected &&
              reads.ordinary == 0u &&
              reads.strict == (memory_condition && !clean ? 2u : 1u),
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

void TestSharedControlPredicates() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  constexpr uint32_t depth = 8;
  for (const auto operation : {ValueOpcode::LogicalAnd, ValueOpcode::LogicalOr}) {
    Program program;
    program.user_data_count = 1;
    program.srt_plan_complete = program.resource_tracking_complete = true;
    auto &block = AddValueBlock(program);
    const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
      return Value(&block.AppendNewInst(opcode, args));
    };
    const auto user = emit(ValueOpcode::GetUserData, {Value(ScalarReg(0))});
    const auto uniform = emit(ValueOpcode::INotEqual32, {user, Value(0u)});
    const auto lane = emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
    auto varying = emit(ValueOpcode::INotEqual32, {lane, Value(0u)});
    // Both failed and partially uniform subgraphs share their operands exponentially.
    for (uint32_t i = 0; i < depth; ++i)
      varying = emit(ValueOpcode::LogicalOr, {varying, varying});
    auto condition = emit(operation, {varying, uniform});
    for (uint32_t i = 0; i < depth; ++i)
      condition = emit(ValueOpcode::LogicalOr, {condition, condition});
    condition = emit(ValueOpcode::LogicalNot, {condition});
    condition = emit(ValueOpcode::ConditionRef, {condition});
    for (uint32_t i = 0; i < 3; ++i) AddValueBlock(program);
    for (uint32_t i = 0; i < 2; ++i) {
      program.blocks[i]->condition = condition;
      program.blocks[i]->terminator.kind = TerminatorKind::ConditionalBranch;
      program.blocks[i]->terminator.true_block = program.blocks[i == 0 ? 1 : 3];
      program.blocks[i]->terminator.false_block = program.blocks[2];
    }
    auto plan = ExtractResourcePlan(program);
    Check(plan.value_storage.size() == depth + 5 &&
              plan.control_flow.size() == 4 &&
              plan.control_flow[0].condition == plan.control_flow[1].condition &&
              !plan.capture_specialization_reads,
          "shared predicates expanded into trees or retained varying dependencies");
    for (const uint32_t flag : {0u, 1u}) {
      const std::array<uint32_t, 1> user_data{flag};
      SrtWalker walker(plan, {.user_data = user_data});
      uint32_t result = 99;
      const bool conjunction = operation == ValueOpcode::LogicalAnd;
      const bool known = conjunction ? flag == 0 : flag != 0;
      Check(walker.Evaluate(plan.control_flow[0].condition, result) == known &&
                (!known || result == uint32_t{conjunction}),
            "shared predicate changed its uniform factor or cached runtime values");
      std::vector<uint32_t> flat;
      Check(walker.RefreshFlatBuffer(flat) &&
                plan.visited_blocks == (known
                    ? (conjunction ? std::vector<uint8_t>{1, 1, 0, 1}
                                   : std::vector<uint8_t>{1, 0, 1, 0})
                    : std::vector<uint8_t>{1, 1, 1, 1}),
            "shared predicate skipped an unknown branch or followed a rejected branch");
    }
  }
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
    // The specialization names a directory slot; the slot holds the mapping
    // offset.
    const auto offset =
        snapshot
            .flattened_srt[specialization.images[0].indirect_mapping_offset];
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

namespace Bindless = Libs::Graphics::Bindless;
using Libs::Graphics::Prospero::BufferFormat;
using Libs::Graphics::Prospero::ImageType;
using Libs::Graphics::ShaderRecompiler::IR::BindlessShape;

Bindless::TSharp TestTSharp(uint32_t address, BufferFormat format, ImageType type,
                            uint32_t depth_field = 0) {
  Bindless::TSharp words{};
  words[0] = address;
  words[1] = static_cast<uint32_t>(format) << 20u;
  words[2] = 3u | (3u << 14u);
  words[3] = Libs::Graphics::DstSel(4, 5, 6, 7) | (static_cast<uint32_t>(type) << 28u);
  words[4] = depth_field;
  return words;
}

void TestBindlessRecordClassification() {
  using Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension;
  const auto Shape = [](const Bindless::TSharp &words) {
    return Bindless::ClassifyRecord(words);
  };
  const auto plain = Shape(TestTSharp(0x100, BufferFormat::k8_8_8_8UNorm, ImageType::kColor2D));
  Check(plain && plain->array == BindlessShape::Image2D &&
            plain->dimension == ImageDimension::Dim2D && !plain->cube,
        "a unorm 2D T# was not served from the 2D array");
  const auto array =
      Shape(TestTSharp(0x100, BufferFormat::k32Float, ImageType::kColor2DArray, 3u));
  Check(array && array->array == BindlessShape::Image2DArray && !array->cube,
        "a 2D array T# was not served from the 2D array array");
  const auto cube = Shape(TestTSharp(0x100, BufferFormat::kBc1UNorm, ImageType::kCube, 5u));
  Check(cube && cube->array == BindlessShape::Image2DArray && cube->cube &&
            cube->Code() == Libs::Graphics::ShaderRecompiler::IR::IndirectImageShape(
                                ImageDimension::Dim2DArray, true),
        "a cube T# was not served as a 2D array with cube coordinates");
  const auto volume =
      Shape(TestTSharp(0x100, BufferFormat::k16_16_16_16Float, ImageType::kColor3D));
  Check(volume && volume->array == BindlessShape::Image3D &&
            volume->dimension == ImageDimension::Dim3D,
        "a volume T# was not served from the 3D array");
  Check(!Shape(Bindless::TSharp{}), "the null T# was served");
  Check(!Shape(TestTSharp(0x100, BufferFormat::k8UInt, ImageType::kColor2D)),
        "an unsigned integer T# was served before anything loads integer records");
  const auto Integer = [](const Bindless::TSharp &words) {
    return Bindless::ClassifyRecord(words, true);
  };
  const auto mask = Integer(TestTSharp(0x100, BufferFormat::k8UInt, ImageType::kColor2D));
  Check(mask && mask->array == BindlessShape::Uint2D &&
            mask->numeric == Libs::Graphics::Prospero::TextureNumericClass::Uint &&
            mask->Code() == (Libs::Graphics::ShaderRecompiler::IR::IndirectImageShape(
                                 ImageDimension::Dim2D, false) |
                             Libs::Graphics::ShaderRecompiler::IR::BindlessUintShapeCode),
        "an unsigned integer T# was not served from the integer 2D array");
  const auto masks =
      Integer(TestTSharp(0x100, BufferFormat::k32UInt, ImageType::kColor2DArray, 3u));
  Check(masks && masks->array == BindlessShape::Uint2DArray && masks->Code() != array->Code(),
        "an unsigned integer 2D array T# shared the float array's shape code");
  Check(!Integer(TestTSharp(0x100, BufferFormat::k32SInt, ImageType::kColor2D)),
        "a signed integer T# was served");
  const auto line = Shape(TestTSharp(0x100, BufferFormat::k8_8_8_8UNorm, ImageType::kColor1D));
  Check(line && line->array == BindlessShape::Image1D &&
            line->dimension == ImageDimension::Dim1D && !line->cube &&
            line->Code() == Libs::Graphics::ShaderRecompiler::IR::IndirectImageShape(
                                ImageDimension::Dim1D, false),
        "a 1D T# was not served from the 1D array");
  const auto lines =
      Shape(TestTSharp(0x100, BufferFormat::k16_16_16_16Float, ImageType::kColor1DArray, 3u));
  Check(lines && lines->array == BindlessShape::Image1D &&
            lines->dimension == ImageDimension::Dim1D,
        "a 1D array T# was not served as a 1D view");
  Check(!Shape(TestTSharp(0x100, BufferFormat::k16_16_16_16Float, ImageType::kColor1DArray,
                          (5u << 16u) | 3u)),
        "a 1D array T# whose base layer passes its last layer was served");
  Check(line->Code() != plain->Code() && line->Code() != array->Code() &&
            line->Code() != cube->Code() && line->Code() != volume->Code(),
        "the 1D shape code collides with another shape");
  const auto mask_line =
      Integer(TestTSharp(0x100, BufferFormat::k32UInt, ImageType::kColor1D));
  Check(mask_line && mask_line->array == BindlessShape::Uint1D,
        "an unsigned integer 1D T# was not served from the integer 1D array");
  Check(!Shape(TestTSharp(0x100, BufferFormat::k8_8_8_8UNorm, ImageType::kColor2DMsaa)),
        "an MSAA T# was served");
  auto reserved = TestTSharp(0x100, BufferFormat::k8_8_8_8UNorm, ImageType::kColor2D);
  reserved[1] |= 0x20000000u;
  Check(!Shape(reserved), "a T# with reserved bits set was served");
  Check(!Shape(TestTSharp(0x100, BufferFormat::k32Float, ImageType::kColor2DArray,
                          (5u << 16u) | 3u)),
        "a 2D array T# whose base layer passes its last layer was served");
  auto mips = TestTSharp(0x100, BufferFormat::k8_8_8_8UNorm, ImageType::kColor2D);
  mips[3] |= 2u << 16u;
  mips[5] = 2u << 4u;
  Check(Shape(mips).has_value(), "a T# with a sane mip range was refused");
  auto past_max = mips;
  past_max[5] = 1u << 4u;
  Check(!Shape(past_max), "a T# whose last level passes its mip count was served");
  auto inverted = mips;
  inverted[3] |= 3u << 12u;
  Check(!Shape(inverted), "a T# whose base level passes its last level was served");
  auto clamped = mips;
  clamped[1] |= (2u * 256u + 1u) << 8u;
  Check(!Shape(clamped), "a T# whose minimum LOD passes its last level was served");
}

void TestBindlessRecordCount() {
  Check(Bindless::HeapRecordCount(48u * 8u, 48u, 16u, 1u << 20u) == 8u,
        "eight 48-byte records were not counted");
  Check(Bindless::HeapRecordCount(48u * 7u + 16u + 32u, 48u, 16u, 1u << 20u) == 8u,
        "a last record whose descriptor ends at the heap end was dropped");
  Check(Bindless::HeapRecordCount(48u * 7u + 16u + 31u, 48u, 16u, 1u << 20u) == 7u,
        "a last record whose descriptor crosses the heap end was counted");
  Check(Bindless::HeapRecordCount(48u * 32768u, 48u, 16u, 1000u) == 1000u,
        "the record count ignored its ceiling");
  Check(Bindless::HeapRecordCount(16u, 48u, 16u, 1u << 20u) == 0u,
        "a heap too small for one descriptor had records");
  Check(Bindless::HeapRecordCount(1024u, 48u, 18u, 1u << 20u) == 0u &&
            Bindless::HeapRecordCount(1024u, 24u, 0u, 1u << 20u) == 0u &&
            Bindless::HeapRecordCount(1024u, 48u, 20u, 1u << 20u) == 0u,
        "a misaligned or overlapping record layout was accepted");
}

void TestBindlessSlotAllocation() {
  Bindless::SlotAllocator slots;
  const auto a = slots.Assign(BindlessShape::Image2D, 0xa);
  const auto b = slots.Assign(BindlessShape::Image2D, 0xb);
  const auto again = slots.Assign(BindlessShape::Image2D, 0xa);
  const auto volume = slots.Assign(BindlessShape::Image3D, 0xa);
  Check(a == 0u && b == 1u && again == 0u && volume == 0u &&
            slots.Used(BindlessShape::Image2D) == 2u &&
            slots.Used(BindlessShape::Image2DArray) == 0u &&
            slots.Used(BindlessShape::Image3D) == 1u,
        "elements were not handed out once per view and per array");
  for (uint32_t key = 2; key < Libs::Graphics::ShaderRecompiler::IR::BindlessImageSlots; key++) {
    Check(slots.Assign(BindlessShape::Image2D, 0x100 + key).has_value(),
          "an array refused an element below its length");
  }
  Check(!slots.Assign(BindlessShape::Image2D, 0xdead).has_value() &&
            slots.Assign(BindlessShape::Image2D, 0xa) == 0u,
        "a full array handed out an element past its length");
  slots.Reset();
  Check(slots.Assign(BindlessShape::Image2D, 0xdead) == 0u &&
            slots.Used(BindlessShape::Image3D) == 0u,
        "a reset left elements assigned");
}

void TestBindlessHeapTranslation() {
  constexpr uint32_t kStride = 48u;
  constexpr uint32_t kOffset = 16u;
  constexpr uint32_t kRecords = 12u;
  std::vector<uint32_t> heap(kRecords * kStride / 4u, 0xcdcdcdcdu);
  const auto Put = [&](uint32_t record, const Bindless::TSharp &tsharp) {
    std::copy(tsharp.begin(), tsharp.end(), heap.begin() + (record * kStride + kOffset) / 4u);
  };
  const auto a = TestTSharp(0x1000, BufferFormat::k8_8_8_8UNorm, ImageType::kColor2D);
  const auto b = TestTSharp(0x2000, BufferFormat::k8_8_8_8Srgb, ImageType::kColor2D);
  const auto volume = TestTSharp(0x3000, BufferFormat::k32Float, ImageType::kColor3D);
  const auto cube = TestTSharp(0x4000, BufferFormat::k8_8_8_8UNorm, ImageType::kCube, 5u);
  const auto integer = TestTSharp(0x5000, BufferFormat::k32UInt, ImageType::kColor2D);
  const auto refused = TestTSharp(0x6000, BufferFormat::k8_8_8_8UNorm, ImageType::kColor2D);
  const auto later = TestTSharp(0x7000, BufferFormat::k8_8_8_8UNorm, ImageType::kColor2D);
  Put(0, Bindless::TSharp{});
  Put(1, a);
  Put(2, b);
  Put(3, a);
  Put(4, volume);
  Put(5, cube);
  Put(6, integer);
  Put(7, refused);
  Put(8, b);
  Put(9, later);
  Put(10, Bindless::TSharp{});
  Put(11, volume);

  Bindless::TranslationCache cache;
  Bindless::SlotAllocator slots;
  std::map<uint32_t, uint32_t> resolved;
  bool later_ready = false;
  const Bindless::RecordResolver resolve =
      [&](const Bindless::TSharp &tsharp,
          const Bindless::RecordShape &shape) -> std::optional<uint32_t> {
    resolved[tsharp[0]]++;
    if (tsharp == refused) {
      return 0u;
    }
    if (tsharp == later && !later_ready) {
      return std::nullopt;
    }
    return Bindless::TranslationWord(shape, *slots.Assign(shape.array, tsharp[0]));
  };
  const auto missing = Bindless::MissingRecords(heap, kStride, kOffset, kRecords, cache);
  Check(missing.size() == 6u,
        "the missing records were not the distinct servable descriptors of the heap");
  std::vector<uint32_t> words(kRecords, 0xffffffffu);
  Bindless::TranslateHeap(heap, kStride, kOffset, kRecords, cache, resolve, words);
  using Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension;
  using Libs::Graphics::ShaderRecompiler::IR::IndirectImageShape;
  using Libs::Graphics::ShaderRecompiler::IR::IndirectImageSlot;
  const auto Word = [](ImageDimension dimension, bool is_cube, uint32_t element) {
    return IndirectImageSlot(IndirectImageShape(dimension, is_cube), element);
  };
  Check(words[0] == 0u && words[10] == 0u, "a null record named an element");
  Check(words[1] == Word(ImageDimension::Dim2D, false, 0) && words[3] == words[1],
        "two records naming one descriptor did not share its element");
  Check(words[2] == Word(ImageDimension::Dim2D, false, 1) && words[8] == words[2],
        "a second descriptor did not take the next element");
  Check(words[4] == Word(ImageDimension::Dim3D, false, 0) && words[11] == words[4],
        "a volume record did not take its own array's element");
  Check(words[5] == Word(ImageDimension::Dim2DArray, true, 0),
        "a cube record did not take a 2D array element with the cube shape");
  Check(words[6] == 0u && words[7] == 0u && words[9] == 0u,
        "an unservable, refused or unsettled record named an element");
  Check(resolved[0x1000] == 1u && resolved[0x2000] == 1u && resolved[0x3000] == 1u &&
            !resolved.contains(0x5000),
        "a descriptor was resolved more than once, or an unservable one at all");

  later_ready = true;
  Check(Bindless::MissingRecords(heap, kStride, kOffset, kRecords, cache).size() == 1u,
        "only the unsettled record should still be missing");
  Bindless::TranslateHeap(heap, kStride, kOffset, kRecords, cache, resolve, words);
  Check(words[9] == Word(ImageDimension::Dim2D, false, 2) && resolved[0x7000] == 2u &&
            resolved[0x1000] == 1u && resolved[0x6000] == 1u,
        "the second pass re-resolved cached records or missed the unsettled one");
}

void TestShaderReadCache() {
  using Libs::Graphics::ShaderGuestReadCache;
  constexpr uint64_t base = 0x10000;
  std::vector<uint32_t> memory(1024);
  for (uint32_t i = 0; i < memory.size(); i++) {
    memory[i] = 0x1000u + i;
  }
  uint64_t dirty_begin = UINT64_MAX;
  uint64_t dirty_end = UINT64_MAX;
  bool drain = false;
  uint32_t line_reads = 0;
  uint32_t word_reads = 0;
  const auto inside = [&](uint64_t address, uint64_t size) {
    return address >= base && address + size <= base + memory.size() * 4u;
  };
  const auto clean = [&](uint64_t address, uint64_t size) {
    return inside(address, size) &&
           (address + size <= dirty_begin || address >= dirty_end);
  };
  const auto copy = [&](uint64_t address, void *data, uint64_t size) {
    std::memcpy(data,
                reinterpret_cast<const uint8_t *>(memory.data()) +
                    (address - base),
                size);
  };
  const auto clean_line = [&](uint64_t address, void *data, uint64_t size) {
    line_reads++;
    if (!clean(address, size)) {
      return false;
    }
    copy(address, data, size);
    return true;
  };
  const auto reader = [&](uint64_t address, std::span<uint32_t> words,
                          bool &drained) {
    word_reads++;
    if (words.empty() || !inside(address, words.size_bytes())) {
      return false;
    }
    drained = drain && !clean(address, words.size_bytes());
    copy(address, words.data(), words.size_bytes());
    return true;
  };
  ShaderGuestReadCache cache;
  cache.Reset();
  uint32_t word = 0;
  const auto read = [&](uint64_t address) {
    return cache.Read(address, {&word, 1}, clean_line, reader);
  };

  bool served = true;
  for (uint32_t i = 0; i < 64; i++) {
    served = served && read(base + i * 4u) && word == 0x1000u + i;
  }
  Check(served && line_reads == 1 && word_reads == 0,
        "a clean line was not read once for every word in it");
  std::array<uint32_t, 8> descriptor{};
  Check(cache.Read(base + 256 + 32, descriptor, clean_line, reader) &&
            descriptor[0] == 0x1000u + 72 && descriptor[7] == 0x1000u + 79 &&
            line_reads == 2 && word_reads == 0,
        "a descriptor inside one line was not served from it");
  std::array<uint32_t, 2> straddle{};
  Check(cache.Read(base + 252, straddle, clean_line, reader) &&
            straddle[0] == 0x1000u + 63 && straddle[1] == 0x1000u + 64 &&
            word_reads == 1 && line_reads == 2,
        "a read across two lines did not go to the reader");

  memory[0] = 0xdeadu;
  Check(read(base) && word == 0x1000u && line_reads == 2,
        "a walk did not see the line it had already read");
  cache.Reset();
  Check(read(base) && word == 0xdeadu && line_reads == 3,
        "a reset cache served a line read before it");

  dirty_begin = base + 512 + 8;
  dirty_end = dirty_begin + 4;
  const auto words_before = word_reads;
  served = true;
  for (uint32_t i = 0; i < 4; i++) {
    served = served && read(base + 512 + i * 4u) && word == 0x1000u + 128 + i;
  }
  Check(served && word_reads == words_before + 4 && line_reads == 4,
        "a line with one dirty word was not read word by word, or was "
        "re-checked per word");

  drain = true;
  Check(read(base + 512 + 8) && read(base) && line_reads == 5,
        "the cache kept its lines across a reader that may have drained");
  Check(!read(base - 4) && !cache.Read(base, {}, clean_line, reader),
        "the cache accepted a read the reader refuses");

  // Ownership asks the same line the read is about to load, so a clean line answers both.
  drain = false;
  dirty_begin = base + 512 + 8;
  dirty_end = dirty_begin + 4;
  cache.Reset();
  const auto lines_before = line_reads;
  Check(cache.CleanCovered(base + 8, 4, clean_line) && line_reads == lines_before + 1 &&
            read(base + 8) && word == 0x1000u + 2u &&
            line_reads == lines_before + 1,
        "a clean line did not answer ownership, or was loaded twice");
  Check(!cache.CleanCovered(base + 512, 4, clean_line) &&
            !cache.CleanCovered(base + 252, 8, clean_line) &&
            !cache.CleanCovered(base - 256, 4, clean_line),
        "a dirty, straddling or unreadable range was reported clean");
}

// One raw or formatted buffer whose V# is user data s[0:3].
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan UserDataDescriptorPlan(bool unindexed,
                                                                          bool formatted) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  DescriptorSource source;
  for (uint32_t i = 0; i < 4; i++) {
    source.dwords[i] = Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                                                  {Value(static_cast<ScalarReg>(i))}));
  }
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back(
      {.source = 0, .read = true, .formatted = formatted, .unindexed = unindexed});
  return ExtractResourcePlan(program);
}

Libs::Graphics::ShaderRecompiler::IR::ResourceSpecialization
MaterializedDescriptor(const Libs::Graphics::ShaderRecompiler::IR::ResourcePlan &plan,
                       std::array<uint32_t, 4> words) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {.user_data = words}, snapshot, specialization) &&
            specialization.buffers.size() == 1 && specialization.unbound_buffers.size() == 1,
        "a user-data V# did not materialize");
  return specialization;
}

void TestUnboundBufferSpecialization() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  const auto indexed = UserDataDescriptorPlan(false, false);
  const auto null = MaterializedDescriptor(indexed, {0, 0, 0, 0});
  Check(null.unbound_buffers[0] && null.buffers[0].packed_stride == 0 &&
            !null.buffers[0].zero_stride_oob,
        "a null raw V# was not unbound, or kept the folded out-of-bounds shape");
  Check(MaterializedDescriptor(indexed, {0x1000u, 0, 0, 3u << 28u}).unbound_buffers[0] &&
            MaterializedDescriptor(indexed, {0, 0, 8, 3u << 28u}).unbound_buffers[0],
        "a V# with no records or no base was bound");
  const auto structured = MaterializedDescriptor(indexed, {0x1000u, 16u << 16u, 8, 0});
  Check(!structured.unbound_buffers[0] && structured.buffers[0].packed_stride == 16 &&
            !structured.buffers[0].zero_stride_oob,
        "an indexed structured V# lost its stride");
  const auto folded = MaterializedDescriptor(indexed, {0x1000u, 0, 8, 0});
  Check(!folded.unbound_buffers[0] && folded.buffers[0].zero_stride_oob,
        "a bound zero-stride mode-0 V# stopped folding its reads");

  const auto raw = UserDataDescriptorPlan(true, false);
  Check(MaterializedDescriptor(raw, {0x1000u, 16u << 16u, 8, 0}).buffers[0].packed_stride == 0 &&
            MaterializedDescriptor(raw, {0x1000u, 1u << 16u, 8, 3u << 28u}) ==
                MaterializedDescriptor(raw, {0x2000u, 0, 64, 3u << 28u}),
        "an unindexed buffer keyed a stride no access reads");
  Check(MaterializedDescriptor(raw, {0x1000u, (16u << 16u) | (1u << 31u), 8, 0})
                    .buffers[0]
                    .packed_stride != 0 &&
            MaterializedDescriptor(raw, {0x1000u, 16u << 16u, 8, 1u << 23u})
                    .buffers[0]
                    .packed_stride != 0,
        "an unindexed buffer dropped the stride a swizzled or ADD_TID access reads");
  const auto raw_null = MaterializedDescriptor(raw, {0, 0, 0, 0});
  const auto raw_bound = MaterializedDescriptor(raw, {0x1000u, 1u << 16u, 8, 3u << 28u});
  Check(raw_null == raw_bound && SpecializationServes(raw_bound, raw_null),
        "a null raw V# keyed a shape no bound raw V# shares");

  Check(SpecializationServes(structured, null) && !SpecializationServes(null, structured) &&
            !SpecializationServes(structured, folded) &&
            !SpecializationServes(folded, structured),
        "an unbound slot did not take any shape, or a bound one took another's");

  const auto formatted = UserDataDescriptorPlan(false, true);
  const auto formatted_null = MaterializedDescriptor(formatted, {0, 0, 0, 0});
  const auto typed = MaterializedDescriptor(formatted, {0x1000u, 4u << 16u, 8, (20u << 12u) | 4u});
  const auto one = MaterializedDescriptor(formatted, {0x1000u, 4u << 16u, 8, (20u << 12u) | 0x204u});
  Check(formatted_null.unbound_buffers[0] && formatted_null.buffers[0].zero_stride_oob &&
            SpecializationServes(typed, formatted_null) &&
            !SpecializationServes(one, formatted_null),
        "a null formatted V# took a shape whose dst_sel reads it as one");
  const auto one_null = MaterializedDescriptor(formatted, {0, 0, 0, (20u << 12u) | 0x204u});
  Check(one_null.unbound_buffers[0] && SpecializationServes(one_null, one_null) &&
            !SpecializationServes(typed, one_null) && !SpecializationServes(one, one_null),
        "an unbound V# that reads one took another shape");

  auto inherited = formatted_null;
  const std::array<const ResourceSpecialization *, 2> donors{&one, &typed};
  InheritUnboundShapes(donors, inherited);
  Check(inherited == typed && !inherited.unbound_buffers[0],
        "the newest compatible bound shape was not inherited");
  auto kept = formatted_null;
  const std::array<const ResourceSpecialization *, 2> unbound_donors{&formatted_null, &one};
  InheritUnboundShapes(unbound_donors, kept);
  Check(kept == formatted_null && kept.unbound_buffers[0],
        "an unbound or incompatible donor shaped the slot");
  auto bound = typed;
  InheritUnboundShapes(unbound_donors, bound);
  Check(bound == typed, "a bound slot took a donor shape");
}

void TestUnboundImageSpecialization() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension;
  ResourceSpecialization null;
  null.images.resize(2);
  null.images[0].numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float;
  null.images[0].dimension = ImageDimension::Dim2D;
  null.images[1] = null.images[0];
  null.unbound_images = {true, false};
  auto volume = null;
  volume.images[0].dimension = ImageDimension::Dim3D;
  volume.unbound_images = {false, false};
  Check(SpecializationServes(volume, null) && !SpecializationServes(null, volume),
        "a null image did not take a bound image's dimension");
  auto msaa = volume;
  msaa.images[0].dimension = ImageDimension::Dim2DMsaa;
  auto uint_volume = volume;
  uint_volume.images[0].numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Uint;
  auto other = volume;
  other.images[1].dimension = ImageDimension::Dim3D;
  Check(!SpecializationServes(msaa, null) && !SpecializationServes(uint_volume, null) &&
            !SpecializationServes(other, null),
        "a null image took a view the null texture cannot fill, another numeric class, or a "
        "bound neighbour's shape");
  auto inherited = null;
  const std::array<const ResourceSpecialization *, 1> donors{&volume};
  InheritUnboundShapes(donors, inherited);
  Check(inherited == volume, "a null image did not inherit a bound dimension");

  using Libs::Graphics::ShaderRecompiler::IR::PushData;
  Check(PushData::StartServes(8, 8) && PushData::StartServes(12, 8) &&
            !PushData::StartServes(4, 8) && PushData::StartServes(PushData::NoStart, 8),
        "a push-data start overlapping an earlier stage was reusable, or a later one was not");
}

// A frozen plan's cached phi resolution must accept, refuse and carry as the
// walk does.
void TestFrozenPhiPlansWalkLikeTheProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  for (const uint32_t stride : {0u, 1u}) {
    Program program;
    program.stage = Libs::Graphics::ShaderType::Compute;
    program.srt_plan_complete = true;
    program.resource_tracking_complete = true;
    auto &block = AddValueBlock(program);
    auto &entry = block.AppendNewInst(ValueOpcode::GetUserData,
                                      {Value(static_cast<ScalarReg>(0))});
    auto &loop = block.AppendNewInst(ValueOpcode::Phi, {},
                                     static_cast<uint64_t>(Type::U32));
    auto &step =
        block.AppendNewInst(ValueOpcode::IAdd32, {Value(&loop), Value(stride)});
    loop.AddPhiOperand(&block, Value(&entry));
    loop.AddPhiOperand(&block, Value(&step));
    DescriptorSource source;
    source.dword_count = 4;
    source.dwords = {Value(&loop), Value(0u), Value(4u), Value(0u)};
    program.descriptor_sources.push_back(source);
    program.info.buffers.push_back({.source = 0});
    auto plan = ExtractResourcePlan(program);
    Check(plan.frozen_values && !program.frozen_values,
          "the fixture did not compare a frozen plan with a program");
    for (const uint32_t value : {0x10u, 0x20u, 0x10u}) {
      const std::array<uint32_t, 1> user_data{value};
      const SrtRuntime runtime{.user_data = user_data};
      ResourceSnapshot frozen;
      ResourceSnapshot walked;
      ResourceSpecialization frozen_specialization;
      ResourceSpecialization walked_specialization;
      const bool frozen_ok =
          MaterializeResources(plan, runtime, frozen, frozen_specialization);
      const bool walked_ok =
          MaterializeResources(program, runtime, walked, walked_specialization);
      Check(frozen_ok == walked_ok && frozen_ok == (stride == 0u),
            "a frozen phi plan accepted or refused a loop the walk did not");
      Check(!frozen_ok || (frozen.buffers == walked.buffers &&
                           frozen.buffers[0].dwords[0] == value),
            "a frozen phi plan carried another value than the loop does");
    }
    Check(plan.phi_plans.size() == 1u,
          "a phi was resolved more than once on a frozen plan");
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

// A cached answer must be the answer: the entry, the web, and on refusal the reason and both
// disagreeing operands, the first time and on every hit after it.
void TestCyclicPhiEntryCacheAnswersLikeTheWalk() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  auto &block = AddValueBlock(program);
  auto &entry = block.AppendNewInst(ValueOpcode::GetUserData,
                                    {Value(static_cast<ScalarReg>(0))});
  auto &loop = block.AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
  auto &step = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&loop), Value(1u)});
  loop.AddPhiOperand(&block, Value(&entry));
  loop.AddPhiOperand(&block, Value(&step));
  auto &merge = block.AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
  merge.AddPhiOperand(&block, Value(1u));
  merge.AddPhiOperand(&block, Value(2u));
  auto &closed = block.AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
  auto &closed_step = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&closed), Value(1u)});
  closed.AddPhiOperand(&block, Value(&closed_step));

  CyclicPhiEntryCache cache{.program = &program};
  for (const auto *phi : {&loop, &merge, &closed}) {
    const Value root(const_cast<Inst *>(phi));
    std::vector<const Inst *> plain_web{&step};
    CyclicPhiFailure plain_reject;
    const auto plain = ResolveCyclicPhiEntry(program, root, &plain_web, &plain_reject);
    for (int pass = 0; pass < 2; pass++) {
      std::vector<const Inst *> web{&step};
      CyclicPhiFailure reject;
      const auto cached = ResolveCyclicPhiEntry(program, root, &web, &reject, &cache);
      Check(cached == plain && web == plain_web && reject.reason == plain_reject.reason &&
                reject.entry == plain_reject.entry && reject.other == plain_reject.other,
            "a cached loop entry differed from the walk it stands for");
    }
  }
  Check(ResolveCyclicPhiEntry(program, Value(&loop)) == Value(&entry),
        "the loop entry was not the value the loop is entered with");
  Check(cache.answers.size() == 3u, "the cache did not hold one answer per phi asked about");
}


struct MemoMemory {
  std::array<uint32_t, 4> table_a{};
  std::array<uint32_t, 4> table_b{};
  bool dirty = false;
};

MemoMemory g_memo_memory;

bool MemoRead(void *, uint64_t address, std::span<uint32_t> values) {
  if (g_memo_memory.dirty) {
    return false;
  }
  std::memcpy(values.data(), reinterpret_cast<const void *>(address), values.size_bytes());
  return true;
}

// Shifted rebuilds the pointer through 32-bit integer operations; Opaque hides it behind an XOR.
enum class MemoBase { Immediate, UserData, ShiftedUserData, OpaqueUserData };

// One flat SRT slot read through a pointer; `structural` also feeds it to a descriptor.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MemoPlan(MemoBase base, uint64_t address,
                                                            bool structural) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  memory.offset = 4;
  program.memory_info.push_back(memory);
  Value low = Value(static_cast<uint32_t>(address));
  Value high = Value(static_cast<uint32_t>(address >> 32u));
  if (base != MemoBase::Immediate) {
    auto &lo = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
    auto &hi = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(1))});
    low = Value(&lo);
    high = Value(&hi);
    if (base == MemoBase::ShiftedUserData) {
      auto &down = block.AppendNewInst(ValueOpcode::ShiftRightLogical32, {low, Value(4u)});
      auto &up = block.AppendNewInst(ValueOpcode::ShiftLeftLogical32, {Value(&down), Value(4u)});
      auto &rest = block.AppendNewInst(ValueOpcode::BitwiseAnd32, {low, Value(0xfu)});
      auto &whole = block.AppendNewInst(ValueOpcode::BitwiseOr32, {Value(&up), Value(&rest)});
      auto &scaled = block.AppendNewInst(ValueOpcode::IMul32, {Value(&whole), Value(1u)});
      low = Value(&block.AppendNewInst(ValueOpcode::ISub32, {Value(&scaled), Value(0u)}));
    }
    if (base == MemoBase::OpaqueUserData) {
      low = Value(&block.AppendNewInst(ValueOpcode::BitwiseXor32, {low, Value(0u)}));
    }
  }
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                  {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});
  if (structural) {
    auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
    auto &flat = block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(0u)});
    DescriptorSource source;
    source.dwords[0] = Value(&flat);
    source.dwords[1] = Value(0u);
    source.dword_count = 2;
    program.descriptor_sources.push_back(source);
  }
  return ExtractResourcePlan(program);
}

void TestMaterializeMemo() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto &memory = g_memo_memory;
  memory.table_a = {1, 0x1111, 2, 3};
  memory.table_b = {4, 0x2222, 5, 6};
  const auto a = reinterpret_cast<uint64_t>(memory.table_a.data());
  const auto b = reinterpret_cast<uint64_t>(memory.table_b.data());
  std::array<uint32_t, 2> user_data{static_cast<uint32_t>(a), static_cast<uint32_t>(a >> 32u)};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead};
  const auto point = [&](uint64_t address) {
    user_data = {static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u)};
  };
  // A replay must leave exactly what a fresh walk of the same inputs produces.
  const auto same_as_walk = [&](const ResourcePlan &plan, const ResourceSnapshot &replayed) {
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    return MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
           fresh.flattened_srt == replayed.flattened_srt && fresh.buffers == replayed.buffers &&
           fresh.user_data == replayed.user_data;
  };
  struct Stage {
    ResourcePlan plan;
    MaterializeMemo memo;
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
  };
  const auto walk = [&](Stage &stage) {
    Check(MaterializeResources(stage.plan, stage.memo.Record(runtime), stage.snapshot,
                               stage.specialization),
          "memo-recorded materialization failed");
    stage.memo.Commit(stage.plan, stage.snapshot);
  };

  {
    Stage leaf{MemoPlan(MemoBase::Immediate, a, false)};
    Check(!leaf.memo.Replay(leaf.plan, runtime, leaf.snapshot), "an empty memo replayed");
    walk(leaf);
    Check(leaf.memo.Valid() && leaf.snapshot.flattened_srt == std::vector<uint32_t>{0x1111},
          "a fixed-address flat slot was not memoized");
    Check(leaf.memo.Replay(leaf.plan, runtime, leaf.snapshot), "an unchanged walk missed");
    memory.table_a[1] = 0x1212;
    Check(leaf.memo.Replay(leaf.plan, runtime, leaf.snapshot) &&
              leaf.snapshot.flattened_srt == std::vector<uint32_t>{0x1212} &&
              same_as_walk(leaf.plan, leaf.snapshot),
          "a guest write to a flat-only word was not refreshed in place");
    memory.dirty = true;
    Check(!leaf.memo.Replay(leaf.plan, runtime, leaf.snapshot) &&
              leaf.memo.LastMiss() == MaterializeMemo::Miss::ReadRefused,
          "a read the walk would refuse (GPU-owned) replayed");
    memory.dirty = false;
    memory.table_a[1] = 0x1111;
  }

  {
    Stage relocated{MemoPlan(MemoBase::UserData, 0, false)};
    point(a);
    walk(relocated);
    Check(relocated.snapshot.flattened_srt == std::vector<uint32_t>{0x1111},
          "the pointer plan read the wrong word");
    point(b);
    Check(relocated.memo.Replay(relocated.plan, runtime, relocated.snapshot) &&
              relocated.snapshot.flattened_srt == std::vector<uint32_t>{0x2222} &&
              relocated.snapshot.user_data[0] == static_cast<uint32_t>(b) &&
              same_as_walk(relocated.plan, relocated.snapshot),
          "a moved per-draw pointer did not relocate the replay");
    point(a);
  }

  {
    Stage pinned{MemoPlan(MemoBase::OpaqueUserData, 0, false)};
    point(a);
    walk(pinned);
    Check(pinned.memo.Replay(pinned.plan, runtime, pinned.snapshot),
          "an unchanged pointer behind an underived operand missed");
    point(b);
    Check(!pinned.memo.Replay(pinned.plan, runtime, pinned.snapshot) &&
              pinned.memo.LastMiss() == MaterializeMemo::Miss::InputChanged,
          "user data behind an operand the replay cannot re-derive was allowed to change");
    point(a);
  }

  {
    Stage structural{MemoPlan(MemoBase::UserData, 0, true)};
    point(a);
    walk(structural);
    Check(structural.memo.Replay(structural.plan, runtime, structural.snapshot),
          "an unchanged descriptor walk missed");
    memory.table_a[1] = 0x3333;
    Check(!structural.memo.Replay(structural.plan, runtime, structural.snapshot) &&
              structural.memo.LastMiss() == MaterializeMemo::Miss::StructureChanged,
          "a word that feeds a descriptor changed without a walk");
    memory.table_a[1] = 0x1111;
    point(b);
    Check(!structural.memo.Replay(structural.plan, runtime, structural.snapshot),
          "a relocated descriptor word that differs replayed");
    memory.table_b[1] = 0x1111;
    Check(structural.memo.Replay(structural.plan, runtime, structural.snapshot) &&
              same_as_walk(structural.plan, structural.snapshot),
          "a relocated table with the same descriptor word missed");
    memory.table_b[1] = 0x2222;
    point(a);
  }

  for (const bool patch : {false, true}) {
    auto &config = MaterializeMemoConfig::Get();
    const auto saved = config.descriptor_patch;
    config.descriptor_patch = patch;
    Stage buffer{UserDataBufferPlan()};
    std::array<uint32_t, 1> words{7};
    const SrtRuntime direct{.user_data = words,
                            .read_memory = MemoRead,
                            .read_specialization_memory = MemoRead};
    Check(MaterializeResources(buffer.plan, buffer.memo.Record(direct), buffer.snapshot,
                               buffer.specialization),
          "user-data descriptor materialization failed");
    buffer.memo.Commit(buffer.plan, buffer.snapshot);
    Check(buffer.memo.Replay(buffer.plan, direct, buffer.snapshot, &buffer.specialization),
          "an unchanged user-data descriptor missed");
    words[0] = 9;
    if (!patch) {
      Check(!buffer.memo.Replay(buffer.plan, direct, buffer.snapshot, &buffer.specialization) &&
                buffer.memo.LastMiss() == MaterializeMemo::Miss::InputChanged,
            "user data that is a descriptor dword changed without a walk");
    } else {
      Check(!buffer.memo.Replay(buffer.plan, direct, buffer.snapshot) &&
                buffer.memo.LastMiss() == MaterializeMemo::Miss::StructureChanged,
            "a moved descriptor word replayed without a specialization to refresh");
      ResourceSnapshot fresh;
      ResourceSpecialization fresh_specialization;
      Check(buffer.memo.Replay(buffer.plan, direct, buffer.snapshot, &buffer.specialization) &&
                MaterializeResources(buffer.plan, direct, fresh, fresh_specialization) &&
                buffer.snapshot.buffers == fresh.buffers &&
                buffer.snapshot.buffers[0].dwords[0] == 9u &&
                buffer.specialization == fresh_specialization &&
                buffer.specialization.unbound_buffers == fresh_specialization.unbound_buffers,
            "a moved user-data descriptor dword was not patched like a walk");
    }
    config.descriptor_patch = saved;
  }

  {
    Stage indirect{MemoPlan(MemoBase::Immediate, a, false)};
    indirect.plan.descriptor_sources.emplace_back().indirect_buffer.emplace();
    Check(!MaterializeMemo::Supports(indirect.plan), "a plan with an indirect table was memoizable");
    const SrtRuntime unreadable{.user_data = user_data, .read_memory = MemoRead};
    Stage blind{MemoPlan(MemoBase::Immediate, a, false)};
    Check(MaterializeResources(blind.plan, blind.memo.Record(unreadable), blind.snapshot,
                               blind.specialization),
          "materialization without a strict reader failed");
    blind.memo.Commit(blind.plan, blind.snapshot);
    Check(!blind.memo.Valid(), "a walk whose refusals the memo cannot see was committed");
  }
}

// A pointer-relative flat slot whose word may also steer a branch or sit next to a bindless heap.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MemoBranchPlan(bool condition, bool bindless,
                                                                  bool self_compare = false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  memory.offset = 4;
  program.memory_info.push_back(memory);
  auto &lo = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  auto &hi = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(1))});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource, {Value(&lo), Value(&hi)});
  auto &raw = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                  {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});
  if (condition) {
    auto &test = block.AppendNewInst(
        ValueOpcode::IEqual32, {Value(&raw), self_compare ? Value(&raw) : Value(0x1111u)});
    AddValueBlock(program);
    AddValueBlock(program);
    program.blocks[0]->condition = Value(&test);
    program.blocks[0]->terminator.kind =
        Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
    program.blocks[0]->terminator.true_block = program.blocks[1];
    program.blocks[0]->terminator.false_block = program.blocks[2];
    for (uint32_t index : {1u, 2u}) {
      program.blocks[index]->id = index;
      program.blocks[index]->terminator.kind =
          Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
    }
  }
  if (bindless) {
    DescriptorSource heap;
    heap.dwords = {Value(0x1000u), Value(0u), Value(0x100u), Value(0u)};
    heap.dword_count = 4;
    program.descriptor_sources.push_back(heap);
    DescriptorSource table;
    table.indirect_descriptor.emplace();
    table.indirect_descriptor->table_source = 0;
    table.indirect_descriptor->bindless = true;
    table.dword_count = 8;
    program.descriptor_sources.push_back(table);
    program.info.images.push_back({.source = 1,
                                   .resource_class = ImageResourceClass::Sampled,
                                   .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
                                   .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D,
                                   .read = true});
  }
  return ExtractResourcePlan(program);
}

struct MemoConditionMemory {
  bool refuse = false;
};

MemoConditionMemory g_memo_condition;

bool MemoConditionRead(void *data, uint64_t address, std::span<uint32_t> values) {
  return !g_memo_condition.refuse && MemoRead(data, address, values);
}

// The extensions keep the one rule: a replay leaves exactly what a fresh walk would.
void TestMaterializeMemoExtensions() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto &memory = g_memo_memory;
  auto &config = MaterializeMemoConfig::Get();
  const auto defaults = config;
  memory.table_a = {1, 0x1111, 2, 3};
  memory.table_b = {4, 0x2222, 5, 6};
  const auto a = reinterpret_cast<uint64_t>(memory.table_a.data());
  const auto b = reinterpret_cast<uint64_t>(memory.table_b.data());
  std::array<uint32_t, 2> user_data{};
  std::array<uint32_t, 3> groups{4, 1, 1};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead,
                           .read_condition_memory = MemoConditionRead,
                           .workgroup_counts = groups};
  const auto point = [&](uint64_t address) {
    user_data = {static_cast<uint32_t>(address), static_cast<uint32_t>(address >> 32u)};
  };
  const auto same_as_walk = [&](const ResourcePlan &plan, const ResourceSnapshot &replayed,
                                const ResourceSpecialization &specialization) {
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    return MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
           fresh.flattened_srt == replayed.flattened_srt && fresh.buffers == replayed.buffers &&
           fresh.images == replayed.images && fresh.user_data == replayed.user_data &&
           fresh.specialization_reads == replayed.specialization_reads &&
           fresh.bindless_tables == replayed.bindless_tables &&
           fresh_specialization == specialization;
  };
  struct Stage {
    ResourcePlan plan;
    MaterializeMemoWays memo;
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
  };
  const auto walk = [&](Stage &stage) {
    Check(MaterializeResources(stage.plan, stage.memo.Record(runtime), stage.snapshot,
                               stage.specialization),
          "memo-recorded materialization failed");
    stage.memo.Commit(stage.plan, stage.snapshot, stage.specialization);
  };
  const auto replay = [&](Stage &stage) {
    return stage.memo.Replay(stage.plan, runtime, stage.snapshot, stage.specialization);
  };

  // Ways: a stage alternating between two descriptor words replays both after one walk each.
  for (const uint32_t ways : {8u, 1u}) {
    config.ways = ways;
    Stage stage{MemoPlan(MemoBase::UserData, 0, true)};
    point(a);
    walk(stage);
    point(b);
    Check(!replay(stage) && stage.memo.LastMiss() == MaterializeMemo::Miss::StructureChanged,
          "a changed descriptor word replayed");
    walk(stage);
    point(a);
    const bool back = replay(stage);
    Check(back == (ways > 1u) && (!back || same_as_walk(stage.plan, stage.snapshot,
                                                        stage.specialization)),
          "an earlier binding did not replay from its own way");
    if (!back) {
      walk(stage);
    }
    point(b);
    Check(replay(stage) == (ways > 1u), "a second way did not replay");
    point(a);
  }
  config = defaults;

  // Dispatch size: no memoizable plan reads it.
  {
    config.exact_workgroups = false;
    Stage stage{MemoPlan(MemoBase::Immediate, a, false)};
    walk(stage);
    groups = {8, 2, 1};
    Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
          "a dispatch size change missed a plan that never reads it");
    config.exact_workgroups = true;
    Check(!replay(stage) && stage.memo.LastMiss() == MaterializeMemo::Miss::InputChanged,
          "the exact dispatch-size switch did not miss");
    config = defaults;
    groups = {4, 1, 1};
  }

  // Refusals: a branch read that refused replays only while it still refuses.
  for (const bool steps : {true, false}) {
    config.refusal_steps = steps;
    Stage stage{MemoBranchPlan(true, false)};
    point(a);
    g_memo_condition.refuse = true;
    walk(stage);
    Check(stage.memo.Valid() == steps &&
              (steps || stage.memo.LastMiss() == MaterializeMemo::Miss::Unreplayable),
          "a walk with a refused branch read was memoized against the switch");
    if (steps) {
      Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
            "a still-refused branch read missed");
      g_memo_condition.refuse = false;
      Check(!replay(stage) && stage.memo.LastMiss() == MaterializeMemo::Miss::StructureChanged,
            "a branch read that answers now replayed the walk that saw it refuse");
    }
    g_memo_condition.refuse = false;
  }
  config = defaults;

  // Captured ranges: a relocated branch read moves its captured range with it.
  for (const bool relocate : {true, false}) {
    config.capture_relocation = relocate;
    Stage stage{MemoBranchPlan(true, false)};
    point(a);
    walk(stage);
    Check(stage.memo.Valid() && !stage.snapshot.specialization_reads.empty(),
          "the branch plan captured no read ranges");
    memory.table_b[1] = 0x1111;
    point(b);
    const bool moved = replay(stage);
    Check(moved == relocate &&
              (moved ? same_as_walk(stage.plan, stage.snapshot, stage.specialization)
                     : stage.memo.LastMiss() == MaterializeMemo::Miss::AddressChanged),
          "a relocated captured read did not follow the relocation switch");
    memory.table_b[1] = 0x2222;
    point(a);
  }
  config = defaults;

  // Integer operations: a pointer rebuilt with shifts, masks and a multiply still relocates.
  for (const bool integer_ops : {true, false}) {
    config.integer_ops = integer_ops;
    Stage stage{MemoPlan(MemoBase::ShiftedUserData, 0, false)};
    point(a);
    walk(stage);
    point(b);
    Check(replay(stage) == integer_ops &&
              (integer_ops ? same_as_walk(stage.plan, stage.snapshot, stage.specialization)
                           : stage.memo.LastMiss() == MaterializeMemo::Miss::InputChanged),
          "a pointer built with integer operations did not follow its switch");
    point(a);
  }
  config = defaults;

  // Self comparison: a branch on x == x reads x, but no value of x changes the branch.
  for (const bool self_compare : {true, false}) {
    config.self_compare = self_compare;
    Stage stage{MemoBranchPlan(true, false, true)};
    point(a);
    walk(stage);
    memory.table_a[1] = 0x1212;
    Check(replay(stage) == self_compare &&
              (self_compare ? same_as_walk(stage.plan, stage.snapshot, stage.specialization)
                            : stage.memo.LastMiss() == MaterializeMemo::Miss::StructureChanged),
          "a word compared only with itself did not follow the self-compare switch");
    memory.table_a[1] = 0x1111;
  }
  config = defaults;

  // Bindless: the heap descriptor is part of the walk, so the plan replays like any other.
  for (const bool bindless : {true, false}) {
    config.bindless = bindless;
    Stage stage{MemoBranchPlan(false, true)};
    Check(MaterializeMemo::Supports(stage.plan) == bindless,
          "bindless support did not follow its switch");
    point(a);
    walk(stage);
    Check(stage.memo.Valid() == bindless && stage.snapshot.bindless_tables.size() == 1u,
          "a bindless plan was not memoized");
    point(b);
    Check(replay(stage) == bindless &&
              (!bindless || same_as_walk(stage.plan, stage.snapshot, stage.specialization)),
          "a relocated bindless plan did not replay like its walk");
    point(a);
  }
  config = defaults;
}

struct OwnedMemory {
  bool owned = false;
  uint32_t reads = 0;
};

OwnedMemory g_owned_memory;

bool OwnedRead(void *, uint64_t address, std::span<uint32_t> values) {
  g_owned_memory.reads++;
  std::memcpy(values.data(), reinterpret_cast<const void *>(address), values.size_bytes());
  return true;
}

bool OwnedQuery(void *, uint64_t, uint64_t) { return g_owned_memory.owned; }

// A data slot the GPU comes to own after recording must miss the memo before any read, so the
// walk can refuse it and the rebuild keeps the load native instead of draining.
void TestMaterializeMemoGpuOwnedDataSlot() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto &memory = g_memo_memory;
  memory.table_a = {1, 0x1111, 2, 3};
  const auto a = reinterpret_cast<uint64_t>(memory.table_a.data());
  auto plan = MemoPlan(MemoBase::Immediate, a, false);
  plan.data_flat_slots = {1u};
  std::array<uint32_t, 2> user_data{};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = OwnedRead,
                           .read_specialization_memory = OwnedRead,
                           .gpu_owned = OwnedQuery};
  MaterializeMemo memo;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  g_owned_memory = {};
  Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0x1111},
        "a clean data slot did not materialize");
  memo.Commit(plan, snapshot);
  Check(memo.Valid() && memo.Replay(plan, runtime, snapshot),
        "a clean data slot missed the memo");
  g_owned_memory.owned = true;
  g_owned_memory.reads = 0;
  Check(!memo.Replay(plan, runtime, snapshot) &&
            memo.LastMiss() == MaterializeMemo::Miss::ReadRefused && g_owned_memory.reads == 0,
        "a GPU-owned data slot replayed, or read the bytes the walk refuses");
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            g_owned_memory.reads == 0,
        "the walk read a GPU-owned data slot instead of refusing it");
}

Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MemoDescriptorPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  auto &lo = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  auto &hi = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(1))});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource, {Value(&lo), Value(&hi)});
  auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
  DescriptorSource source;
  for (uint32_t dword = 0; dword < 4; dword++) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.planning_only = true;
    memory.offset = dword * 4u;
    program.memory_info.push_back(memory);
    auto &raw = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                    {Value(&handle), Value(0u), Value(0u), Value(true)});
    raw.SetFlags(MemoryFlags{.index = dword, .pc = 0x40u + dword * 4u});
    program.srt_reads.push_back({Value(&raw), dword});
    auto &flat = block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(dword)});
    source.dwords[dword] = Value(&flat);
  }
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  return ExtractResourcePlan(program);
}

void TestMaterializeMemoDescriptorPatch() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  // Base 0x1000_0000, stride 16 (dword1 bits 16..29), 64 records, dst_sel + formats in dword3.
  std::array<uint32_t, 4> table_a{0x10000000u, 16u << 16u, 64u, 0x00027fac};
  std::array<uint32_t, 4> table_b{0x20000000u, 32u << 16u, 128u, 0x00027fac};
  std::array<uint32_t, 4> table_c{0x30000000u, 16u << 16u, 64u, 0x00027fac};
  const auto address = [](const auto &table) { return reinterpret_cast<uint64_t>(table.data()); };
  std::array<uint32_t, 2> user_data{};
  const auto point = [&](uint64_t where) {
    user_data = {static_cast<uint32_t>(where), static_cast<uint32_t>(where >> 32u)};
  };
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead};
  const auto plan = MemoDescriptorPlan();
  MaterializeMemo memo;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  point(address(table_a));
  Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization),
        "the descriptor plan did not materialize");
  memo.Commit(plan, snapshot);
  Check(memo.Valid(), "the descriptor plan was not memoized");
  const auto same_as_walk = [&] {
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    return MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
           fresh.buffers == snapshot.buffers && fresh.flattened_srt == snapshot.flattened_srt &&
           fresh.user_data == snapshot.user_data && fresh_specialization == specialization &&
           fresh_specialization.unbound_buffers == specialization.unbound_buffers;
  };
  for (const auto *table : {&table_b, &table_c, &table_a, &table_b}) {
    point(address(*table));
    Check(memo.Replay(plan, runtime, snapshot, &specialization) && same_as_walk() &&
              snapshot.buffers[0].dwords[0] == (*table)[0],
          "a moved V# bound as a buffer did not replay like a walk");
  }
  table_b[2] = 0;
  Check(memo.Replay(plan, runtime, snapshot, &specialization) && same_as_walk() &&
            specialization.unbound_buffers[0],
        "a V# that became unbound did not refresh the specialization");
  table_b[2] = 128u;
}

void TestMaterializeMemoBackoff() {
  using Backoff = Libs::Graphics::ShaderRecompiler::IR::MaterializeMemoBackoff;
  Backoff backoff;
  const auto skipped = [&](uint32_t calls) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < calls; i++) {
      count += backoff.Skip() ? 1u : 0u;
    }
    return count;
  };
  for (uint32_t i = 0; i < Backoff::Threshold; i++) {
    Check(!backoff.Skip(), "the memo backed off before the miss threshold");
    backoff.OnMiss();
  }
  Check(!backoff.Skip(), "the memo backed off before trying to replay a fresh recording");
  backoff.OnMiss();
  Check(skipped(Backoff::InitialWindow) == Backoff::InitialWindow && !backoff.Skip(),
        "the first back-off window has the wrong length");
  backoff.OnMiss();
  Check(!backoff.Skip(), "the memo skipped the call that replays a fresh recording");
  backoff.OnMiss();
  Check(skipped(2 * Backoff::InitialWindow) == 2 * Backoff::InitialWindow && !backoff.Skip(),
        "the back-off window did not double");
  backoff.OnHit();
  for (uint32_t i = 0; i + 1 < Backoff::Threshold; i++) {
    backoff.OnMiss();
  }
  Check(!backoff.Skip(), "a hit did not reset the miss run");
  backoff.OnMiss();
  backoff.OnMiss();
  Check(skipped(Backoff::InitialWindow) == Backoff::InitialWindow && !backoff.Skip(),
        "a hit did not reset the back-off window");
  for (uint32_t round = 0; round < 16; round++) {
    backoff.OnMiss();
    backoff.OnMiss();
    skipped(Backoff::MaxWindow);
  }
  backoff.OnMiss();
  backoff.OnMiss();
  Check(skipped(Backoff::MaxWindow + 1) == Backoff::MaxWindow,
        "the back-off window grew past its cap");
}

int main() {
  TestMappedSrtUsesDirectReaderByDefault();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestUniformVectorDescriptorRead();
  TestExactReciprocalDescriptorArithmetic();
  TestUnbasedFlatCacheHitMaterializes();
  for (const bool memory_condition : {false, true})
    TestWrittenDescriptorPredicateReads(memory_condition);
  TestFailedMaterializationRejectsStage();
  TestSharedControlPredicates();
  TestFiniteImageRefreshReusesScalarReads();
  TestMixedSamplerVariantsShareRuntimeDescriptor();
  TestBindlessRecordClassification();
  TestBindlessRecordCount();
  TestBindlessSlotAllocation();
  TestBindlessHeapTranslation();
  TestShaderReadCache();
  TestUnboundBufferSpecialization();
  TestUnboundImageSpecialization();
  TestCyclicPhiEntryCacheAnswersLikeTheWalk();
  TestFrozenPhiPlansWalkLikeTheProgram();
  TestMaterializeMemo();
  TestMaterializeMemoGpuOwnedDataSlot();
  TestMaterializeMemoExtensions();
  TestMaterializeMemoBackoff();
  TestMaterializeMemoDescriptorPatch();
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
