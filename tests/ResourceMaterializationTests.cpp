#include "graphics/host_gpu/renderer/pipeline/shaderReadCache.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterializationMemo.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
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

void TestNativeNestedSrtRefresh() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  std::array<uint32_t, 3> first{0u, 0x12345678u, 0xabcdef01u};
  std::array<uint32_t, 3> second{0u, 0x87654321u, 0x10fedcbau};
  const auto pointer_words = [](const void *pointer) {
    const auto address = reinterpret_cast<uint64_t>(pointer);
    return std::array<uint32_t, 2>{static_cast<uint32_t>(address) | 3u,
                                  static_cast<uint32_t>(address >> 32u) | 0xabcd0000u};
  };
  auto table = pointer_words(first.data());
  const auto user_data = pointer_words(table.data());
  auto program = SrtProgram(0u);
  program.user_data_base = 4;
  program.user_data_count = 2;
  auto &block = *program.blocks[0];
  const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
    return Value(&block.AppendNewInst(opcode, args));
  };
  const auto low = emit(ValueOpcode::GetUserData, {Value(ScalarReg(4))});
  const auto high = emit(ValueOpcode::GetUserData, {Value(ScalarReg(5))});
  const auto root = emit(ValueOpcode::GetAddressResource, {low, high});
  const auto read = [&](Value address, uint32_t offset) {
    const auto value = emit(ValueOpcode::LoadAddressU32,
                            {address, Value(offset), Value(0u), Value(true)});
    value.Instruction()->SetFlags(MemoryFlags{.index = 0});
    program.srt_reads.push_back(
        {value, static_cast<uint32_t>(program.srt_reads.size())});
    return value;
  };
  const auto pointer_low = read(root, 0u);
  const auto pointer_high = read(root, 4u);
  auto *leaf = program.srt_reads[0].value.Instruction();
  leaf->Arg(0).Instruction()->SetArg(0, pointer_low);
  leaf->Arg(0).Instruction()->SetArg(1, pointer_high);
  leaf->SetArg(1, Value(7u));
  read(leaf->Arg(0), 11u);
  auto plan = ExtractResourcePlan(program);
  Check(plan.srt_code != nullptr, "nested user-data tree did not compile an x86 walker");
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const SrtRuntime runtime{.user_data = user_data};
  {
    SrtWalker walker(plan, runtime);
    Check(walker.RefreshFlatBuffer(snapshot.flattened_srt),
          "native SRT refresh failed");
    const auto loaded = first[1];
    first[1] ^= 0xffffffffu;
    uint32_t value = 0;
    Check(walker.Evaluate(plan.srt_reads[0].value, value) && value == loaded,
          "descriptor evaluation reloaded a value already copied by the native walker");
    first[1] = loaded;
  }
  for (const auto *data : {&first, &second}) {
    table = pointer_words(data->data());
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.flattened_srt ==
                  std::vector<uint32_t>{(*data)[1], table[0], table[1], (*data)[2]},
          "native SRT refresh lost pointer masking, alignment, or current values");
  }
  Check(!MaterializeResources(plan, {.user_data = std::span(user_data).first(1)},
                              snapshot, specialization),
        "native SRT walk accepted truncated user data");
  const std::array<uint32_t, 2> fake_user_data{0x1003u, 0xabcd0000u};
  uint32_t reads = 0;
  const SrtRuntime callback_runtime{
      .user_data = fake_user_data,
      .read_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        ++*static_cast<uint32_t *>(data);
        switch (address) {
          case 0x1000u: words[0] = 0x2003u; break;
          case 0x1004u: words[0] = 0xabcd0000u; break;
          case 0x2004u: words[0] = 0x1234u; break;
          case 0x2008u: words[0] = 0x5678u; break;
          default: return false;
        }
        return true;
      },
      .userdata = &reads};
  Check(MaterializeResources(plan, callback_runtime, snapshot, specialization) && reads == 4 &&
            snapshot.flattened_srt ==
                std::vector<uint32_t>{0x1234u, 0x2003u, 0xabcd0000u, 0x5678u},
        "native-eligible SRT bypassed its callback or repeated nested pointer reads");
  leaf->SetArg(0, root);
  leaf->SetArg(1, Value(0u));
  program.srt_reads.resize(1);
  for (const int32_t offset : {-1, 7}) {
    program.memory_info[0].offset = static_cast<uint32_t>(offset);
    auto bounded = ExtractResourcePlan(program);
    auto base = pointer_words(second.data() + 1);
    Check(bounded.srt_code != nullptr &&
              MaterializeResources(bounded, {.user_data = base}, snapshot, specialization) &&
              snapshot.flattened_srt[0] == second[offset < 0 ? 0 : 2],
          "native SRT applied a signed immediate offset incorrectly");
    base = offset < 0 ? std::array<uint32_t, 2>{0u, 0u}
                      : std::array<uint32_t, 2>{0xfffffffcu, 0xffffu};
    Check(!MaterializeResources(bounded, {.user_data = base}, snapshot, specialization),
          "native SRT accepted address underflow or 48-bit overflow");
  }
  high.Instruction()->SetArg(0, Value(ScalarReg(6)));
  Check(ExtractResourcePlan(program).srt_code == nullptr,
        "native SRT accepted nonadjacent user-data pointer words");
  high.Instruction()->SetArg(0, Value(ScalarReg(5)));
  program.memory_info[0].offset = 0;
  program.srt_reads.push_back({pointer_low, 1});
  program.srt_reads.push_back({pointer_high, 2});
  leaf->SetArg(0, emit(ValueOpcode::GetAddressResource, {pointer_low, pointer_high}));
  pointer_high.Instruction()->SetArg(1, Value(8u));
  Check(ExtractResourcePlan(program).srt_code == nullptr,
        "native SRT accepted nonadjacent nested pointer words");
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
  indirect.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{}).sources = {0, 1};
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
  Check(plan.srt_code == nullptr,
        "captured strict reads compiled an unused x86 walker");
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
    // A failed predicate explores both edges; its dirty read also rejects the writable descriptor.
    const bool expected = !memory_condition || clean;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) == expected &&
              reads.ordinary == (memory_condition ? 0u : 1u) &&
              reads.strict == (memory_condition ? 1u : 0u),
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
    Check(plan.value_storage.size() <= 2u * depth + 12u &&
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

void TestNestedExecPredicatesReuseReads() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  constexpr uint32_t depth = 12;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  for (uint32_t i = 0; i < 3; ++i) AddValueBlock(program);
  auto &entry = *program.blocks[0];
  auto &use = *program.blocks[1];
  auto &exit = *program.blocks[2];
  const auto emit = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
    return Value(&entry.AppendNewInst(opcode, args));
  };
  const auto lane = emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress, .planning_only = true});
  const auto address = emit(ValueOpcode::GetAddressResource, {Value(0x1000u), Value(0u)});
  const auto count = emit(ValueOpcode::LoadAddressU32,
      {address, Value(0u), Value(0u), Value(true)});
  count.Instruction()->SetFlags(MemoryFlags{.index = 0});
  auto condition = emit(ValueOpcode::LogicalNot,
      {emit(ValueOpcode::ULessThanEqual32, {count, lane})});
  // A compact shared DAG must not become a tree of fresh EXEC evaluations.
  for (uint32_t i = 0; i < depth; ++i) {
    const auto mask = emit(ValueOpcode::INotEqual32, {lane, Value(i)});
    condition = emit(ValueOpcode::LogicalAnd,
        {mask, emit(ValueOpcode::LogicalOr, {condition, condition})});
  }
  entry.condition = condition;
  entry.terminator.kind = TerminatorKind::ConditionalBranch;
  entry.terminator.true_block = &use;
  entry.terminator.false_block = &exit;
  entry.AddBranch(&use);
  entry.AddBranch(&exit);
  use.terminator.kind = TerminatorKind::Branch;
  use.terminator.true_block = &exit;
  use.AddBranch(&exit);
  DescriptorSource source;
  source.dwords = {Value(0x8000u), Value(1u << 16u), Value(16u), Value(3u << 28u)};
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .read = true});
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  const auto buffer = Value(&use.AppendNewInst(ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]}));
  use.AppendNewInst(ValueOpcode::LoadBufferU32,
      {buffer, Value(0u), Value(0u), Value(0u), Value(true)})
      .SetFlags(MemoryFlags{.index = 1});
  auto plan = ExtractResourcePlan(program);
  struct Reads { uint32_t value = 0; bool clean = true; uint32_t strict = 0, ordinary = 0; } reads;
  const SrtRuntime runtime{
      .read_memory = +[](void *data, uint64_t, std::span<uint32_t>) {
        ++static_cast<Reads *>(data)->ordinary;
        return false;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &reads = *static_cast<Reads *>(data);
        ++reads.strict;
        Check(reads.strict <= depth + 1,
              "shared EXEC predicates repeated scalar reads beyond their distinct masks");
        if (!reads.clean || address != 0x1000u || words.size() != 1) return false;
        words[0] = reads.value;
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const auto state : {Reads{64u, true}, Reads{0u, true}, Reads{0u, false}, Reads{64u, true}}) {
    reads = state;
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "nested EXEC predicates rejected a valid resource snapshot");
    const bool inactive = reads.clean && reads.value == 0;
    Check(snapshot.buffers[0].dwords[0] == (inactive ? 0u : 0x8000u),
          "nested EXEC predicates lost an unknown branch or retained stale runtime facts");
    Check(reads.strict > 0 && reads.ordinary == 0 &&
              snapshot.specialization_reads.size() <= depth + 1,
          "shared EXEC predicates repeated scalar reads beyond their distinct masks");
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

void TestEmptyTraversalSkipsUnmappedDescriptor(bool independent_mask, bool counted_loop = false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  for (uint32_t i = 0; i < 6; ++i) AddValueBlock(program);
  auto &entry = *program.blocks[0];
  auto &loop = *program.blocks[1];
  auto &body = *program.blocks[2];
  auto &merge = *program.blocks[3];
  auto &use = *program.blocks[4];
  auto &exit = *program.blocks[5];
  const auto emit = [](Block &block, ValueOpcode opcode, std::initializer_list<Value> args) {
    return Value(&block.AppendNewInst(opcode, args));
  };
  const auto branch = [](Block &block, Block &target, Value condition = {}, Block *other = nullptr) {
    block.terminator.kind = other ? TerminatorKind::ConditionalBranch : TerminatorKind::Branch;
    block.terminator.true_block = &target;
    block.terminator.false_block = other;
    block.condition = condition;
    block.AddBranch(&target);
    if (other) block.AddBranch(other);
  };
  Value lane, active, initial;
  if (!counted_loop) {
    lane = emit(entry, ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
    active = emit(entry, ValueOpcode::INotEqual32, {lane, Value(0u)});
    const auto initial_mask = independent_mask
        ? emit(entry, ValueOpcode::IEqual32, {lane, Value(0u)}) : active;
    initial = emit(entry, ValueOpcode::SelectU32,
        {initial_mask, Value(UINT32_MAX), Value(0u)});
  }
  program.memory_info.push_back({.kind = ResourceKind::ScalarAddress, .planning_only = true});
  const auto address = emit(entry, ValueOpcode::GetAddressResource, {Value(0x1000u), Value(0u)});
  const auto count = emit(entry, ValueOpcode::LoadAddressU32,
      {address, Value(0u), Value(0u), Value(true)});
  count.Instruction()->SetFlags(MemoryFlags{.index = 0});
  if (counted_loop) {
    branch(entry, loop);
  } else {
    const auto rejected = emit(entry, ValueOpcode::ULessThanEqual32, {count, lane});
    const auto accepted = emit(entry, ValueOpcode::LogicalNot, {rejected});
    branch(entry, loop, emit(entry, ValueOpcode::LogicalAnd, {active, accepted}), &merge);
  }

  // A nonempty traversal contains a varying loop-carried value. Its cycle must
  // remain unknown; only the proven empty traversal can select the bypass arm.
  auto &carried = loop.AppendNewInst(ValueOpcode::Phi);
  carried.SetFlags(Type::U32);
  const auto next = emit(body, ValueOpcode::IAdd32, {Value(&carried), Value(1u)});
  carried.AddPhiOperand(&entry, counted_loop ? Value(0u) : lane);
  carried.AddPhiOperand(&body, next);
  const auto condition = counted_loop
      ? emit(loop, ValueOpcode::ULessThan32, {Value(&carried), count})
      : emit(loop, ValueOpcode::INotEqual32, {Value(&carried), Value(UINT32_MAX)});
  branch(loop, counted_loop ? use : body, condition, &merge);
  loop.terminator.loop_header = true;
  loop.terminator.continue_block = &body;
  loop.terminator.merge_block = &merge;
  branch(body, loop);
  if (counted_loop) {
    branch(merge, exit);
    branch(use, body);
  } else {
    auto &selected = merge.AppendNewInst(ValueOpcode::Phi);
    selected.SetFlags(Type::U32);
    selected.AddPhiOperand(&entry, initial);
    selected.AddPhiOperand(&loop, Value(&carried));
    const auto valid = emit(merge, ValueOpcode::INotEqual32, {Value(&selected), Value(UINT32_MAX)});
    branch(merge, use, emit(merge, ValueOpcode::LogicalAnd, {active, valid}), &exit);
    branch(use, exit);
  }
  DescriptorSource source;
  const auto descriptor_address = emit(use, ValueOpcode::GetAddressResource,
      {Value(0x2000u), Value(0u)});
  for (uint32_t i = 0; i < 4; ++i) {
    const auto index = static_cast<uint32_t>(program.memory_info.size());
    program.memory_info.push_back({.kind = ResourceKind::ScalarAddress, .planning_only = true});
    source.dwords[i] = emit(use, ValueOpcode::LoadAddressU32,
        {descriptor_address, Value(i * 4u), Value(0u), Value(true)});
    source.dwords[i].Instruction()->SetFlags(MemoryFlags{.index = index});
  }
  source.dword_count = 4;
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0, .read = true});
  const auto buffer_index = static_cast<uint32_t>(program.memory_info.size());
  program.memory_info.push_back({.kind = ResourceKind::Buffer, .resource = 0});
  const auto buffer = emit(use, ValueOpcode::GetBufferResource,
      {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]});
  emit(use, ValueOpcode::LoadBufferU32, {buffer, Value(0u), Value(0u), Value(0u), Value(true)})
      .Instruction()->SetFlags(MemoryFlags{.index = buffer_index});
  auto plan = ExtractResourcePlan(program);
  struct Reads { uint32_t count = 0; bool clean = true; uint32_t descriptors = 0; } reads;
  const SrtRuntime runtime{
      .read_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        constexpr std::array<uint32_t, 4> descriptor{0x8000u, 1u << 16u, 16u,
                                                   (3u << 28u) | 0x4dfacu};
        if (address < 0x2000u || address >= 0x2010u || address % 4u || words.size() != 1)
          return false;
        ++static_cast<Reads *>(data)->descriptors;
        words[0] = descriptor[(address - 0x2000u) / 4u];
        return true;
      },
      .userdata = &reads,
      .read_specialization_memory = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        const auto &reads = *static_cast<Reads *>(data);
        if (!reads.clean || address != 0x1000u || words.size() != 1) return false;
        words[0] = reads.count;
        return true;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const auto state : {Reads{0u, true}, Reads{4u, true}, Reads{0u, false}, Reads{0u, true}}) {
    reads = state;
    if (!counted_loop) {
      uint32_t condition = 99;
      Check(!SrtWalker(plan, runtime).Evaluate(plan.control_flow[3].condition, condition),
            "direct merge evaluation used missing or previous runtime reachability");
    }
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "partial traversal proof rejected a valid resource snapshot");
    const bool inactive = !independent_mask && reads.clean && reads.count == 0u;
    Check(snapshot.buffers[0].dwords[0] == (inactive ? 0u : 0x8000u),
          "empty traversal kept its descriptor or an uncertain traversal lost it");
    Check(specialization.buffers[0].zero_stride_oob == inactive,
          "inactive traversal did not expose dead vector reads to specialization");
    Check(reads.descriptors == (inactive ? 0u : 4u),
          "inactive traversal read its descriptor or an uncertain traversal skipped it");
    if (inactive) {
      Check(!snapshot.specialization_reads.empty(), "empty traversal lost its clean scalar read");
      for (const auto &read : snapshot.specialization_reads)
        Check(read == std::pair<uint64_t, uint64_t>{0x1000u, 4u},
              "empty traversal proof captured an unrelated memory range");
    }
  }
}

void TestCyclicControlPredicatesRemainUnknown() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  for (uint32_t i = 0; i < 4; ++i) AddValueBlock(program);
  auto &entry = *program.blocks[0];
  auto &left = *program.blocks[1];
  auto &right = *program.blocks[2];
  auto &exit = *program.blocks[3];
  const auto lane = Value(&entry.AppendNewInst(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)}));
  const auto u = Value(&entry.AppendNewInst(ValueOpcode::INotEqual32, {lane, Value(0u)}));
  const auto v = Value(&entry.AppendNewInst(ValueOpcode::INotEqual32, {lane, Value(1u)}));
  auto &p = left.AppendNewInst(ValueOpcode::Phi);
  auto &q = right.AppendNewInst(ValueOpcode::Phi);
  p.SetFlags(Type::U1);
  q.SetFlags(Type::U1);
  const auto a = Value(&left.AppendNewInst(ValueOpcode::LogicalAnd, {u, Value(&p)}));
  const auto b = Value(&right.AppendNewInst(ValueOpcode::LogicalAnd, {v, Value(&q)}));
  p.AddPhiOperand(&entry, Value(true));
  p.AddPhiOperand(&right, b);
  q.AddPhiOperand(&entry, Value(true));
  q.AddPhiOperand(&left, a);
  const auto branch = [](Block &block, Value condition, Block &yes, Block &no) {
    block.condition = condition;
    block.terminator.kind = TerminatorKind::ConditionalBranch;
    block.terminator.true_block = &yes;
    block.terminator.false_block = &no;
    block.AddBranch(&yes);
    block.AddBranch(&no);
  };
  // Two loop entries have no single header whose Phi can be discarded up front.
  branch(entry, u, left, right);
  branch(left, a, right, exit);
  branch(right, b, left, exit);
  auto plan = ExtractResourcePlan(program);
  SrtWalker walker(plan, {});
  uint32_t value = 99;
  Check(!walker.Evaluate(plan.control_flow[1].condition, value) &&
            !walker.Evaluate(plan.control_flow[2].condition, value),
        "cyclic varying control acquired an unsupported uniform value");
  std::vector<uint32_t> flat;
  Check(walker.RefreshFlatBuffer(flat) && plan.visited_blocks == std::vector<uint8_t>({1, 1, 1, 1}),
        "cyclic control lost a possible branch during reachability");
}

void TestEmptyScalarBufferSpecialization() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  struct Fixture {
    Program program;
    std::vector<std::pair<Inst *, Type>> uses;
    Inst *nonnull = nullptr;
  };
  const auto make_fixture = [] {
    Fixture fixture;
    auto &program = fixture.program;
    program.stage = Libs::Graphics::ShaderType::Compute;
    program.srt_plan_complete = program.resource_tracking_complete = true;
    program.user_data_count = 4u;
    auto &block = AddValueBlock(program);
    DescriptorSource source;
    source.dword_count = 4u;
    for (uint32_t word = 0; word < 4u; ++word)
      source.dwords[word] = Value(&block.AppendNewInst(ValueOpcode::GetUserData,
          {Value(static_cast<ScalarReg>(word))}));
    program.descriptor_sources.push_back(source);
    program.info.buffers.push_back({.source = 0u, .read = true, .scalar = true});
    const auto table = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
        {source.dwords[0], source.dwords[1], source.dwords[2], source.dwords[3]}));
    std::array<Value, 4> words;
    for (uint32_t word = 0; word < 4u; ++word) {
      program.memory_info.push_back({.kind = ResourceKind::ScalarBuffer, .resource = 0u,
                                     .offset = word * 4u});
      auto &read = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {table, Value(0u)});
      read.SetFlags(MemoryFlags{.index = word});
      words[word] = Value(&read);
    }
    const auto handle = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
        {words[0], words[1], words[2], words[3]}));
    for (const auto opcode : {ValueOpcode::ReadConstBuffer, ValueOpcode::LoadBufferU8,
                              ValueOpcode::LoadBufferU16, ValueOpcode::LoadBufferU32,
                              ValueOpcode::LoadBufferU32x2, ValueOpcode::LoadBufferU32x3,
                              ValueOpcode::LoadBufferU32x4}) {
      const auto index = static_cast<uint32_t>(program.memory_info.size());
      program.memory_info.push_back({.kind = ResourceKind::IndirectBuffer, .resource = UINT32_MAX,
          .data_dwords = BufferComponentCount(opcode),
          .data_bits = opcode == ValueOpcode::LoadBufferU8 ? 8u : opcode == ValueOpcode::LoadBufferU16 ? 16u : 32u,
          .formatted = opcode == ValueOpcode::LoadBufferU32});
      auto &read = opcode == ValueOpcode::ReadConstBuffer
          ? block.AppendNewInst(opcode, {handle, Value(0u)})
          : block.AppendNewInst(opcode, {handle, Value(0u), Value(0u), Value(0u), Value(true)});
      read.SetFlags(MemoryFlags{.index = index});
      fixture.uses.emplace_back(&block.AppendNewInst(ValueOpcode::Identity, {Value(&read)}), read.GetType());
    }
    const auto nonnull = Value(&block.AppendNewInst(ValueOpcode::GetBufferResource,
        {words[0], words[1], words[2], Value(1u)}));
    auto &read = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {nonnull, Value(0u)});
    read.SetFlags(MemoryFlags{.index = 4u});
    fixture.nonnull = &block.AppendNewInst(ValueOpcode::Identity, {Value(&read)});
    return fixture;
  };
  auto fixture = make_fixture();
  auto plan = ExtractResourcePlan(fixture.program);
  std::array<uint32_t, 4> descriptor{0x1000u, 8u << 16u, 0u, 3u << 28u};
  uint32_t reads = 0u;
  const SrtRuntime runtime{.user_data = descriptor, .read_memory = RejectSpecializationRead,
                           .userdata = &reads, .read_specialization_memory = RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization, empty, populated;
  for (const uint32_t records : {0u, 1u, 0x20000000u, 0u}) {
    descriptor[2] = records;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) && reads == 0u &&
              specialization.buffers[0].scalar_empty == (records == 0u),
          "scalar emptiness read table contents or truncated its 64-bit byte size");
    if (records == 0u) {
      if (empty.buffers.empty()) empty = specialization;
      else Check(specialization == empty, "returning to an empty scalar table changed its specialization");
    }
    else if (records == 1u) populated = specialization;
    else Check(specialization == populated, "nonempty scalar size created a shader permutation");
    auto current = make_fixture();
    ApplyResourceSpecialization(current.program, specialization);
    for (const auto &[use, type] : current.uses) {
      const auto value = use->Arg(0).Resolve();
      Check(value.GetType() == type, "empty scalar-table folding changed the load result type");
      if (records != 0u) {
        Check(value.TryInstruction() != nullptr && BufferAccessOf(value.Instruction()->GetOpcode()) == BufferAccess::Read,
              "nonempty scalar table lost a runtime buffer load");
      } else if (value.IsImmediate()) {
        Check(value == Value(uint8_t{0}) || value == Value(uint16_t{0}) || value == Value(0u),
              "empty scalar table did not produce a typed zero");
      } else {
        for (size_t component = 0; component < value.Instruction()->NumArgs(); ++component)
          Check(value.Instruction()->Arg(component) == Value(0u),
                "empty scalar table retained a nonzero vector component");
      }
    }
    Check(current.nonnull->Arg(0).Resolve().TryInstruction() != nullptr,
          "a partly nonzero indirect descriptor was treated as unbound");
  }
  Check(empty != populated, "empty-to-populated scalar transition reused the folded specialization");
  plan.info.buffers[0].scalar = false;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            !specialization.buffers[0].scalar_empty,
        "vector-only resource acquired scalar-empty specialization");
  empty.buffers[0].indirect_root = 0u;
  empty.buffers.push_back(empty.buffers[0]);
  empty.buffers[1].scalar_empty = false;
  ApplyResourceSpecialization(fixture.program, empty);
  for (const auto &[use, type] : fixture.uses)
    Check(use->Arg(0).GetType() == type && use->Arg(0).Resolve().TryInstruction() != nullptr &&
              BufferAccessOf(use->Arg(0).ResolveInstruction()->GetOpcode()) == BufferAccess::Read,
          "empty root folded a scalar read with nonempty candidates");
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

void TestSignedStorageSpecialization() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using F = Libs::Graphics::Prospero::BufferFormat;
  using N = Libs::Graphics::Prospero::TextureNumericClass;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  program.user_data_count = 1u;
  auto &block = AddValueBlock(program);
  DescriptorSource source;
  source.dword_count = 8u;
  source.dwords.fill(Value(0u));
  source.dwords[0] = Value(0x100u);
  source.dwords[1] = Value(&block.AppendNewInst(ValueOpcode::GetUserData, {Value(ScalarReg(0))}));
  constexpr auto swizzle = Libs::Graphics::DstSel(5, 4, 1, 0);
  source.dwords[3] = Value(swizzle |
      (uint32_t(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u));
  program.descriptor_sources.push_back(source);
  program.info.images.push_back({.source = 0u, .resource_class = ImageResourceClass::Storage,
      .dimension = Libs::Graphics::ShaderRecompiler::Decoder::ImageDimension::Dim2D, .written = true});
  auto plan = ExtractResourcePlan(program);
  std::array<uint32_t, 1> data{};
  const SrtRuntime runtime{.user_data = data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization, signed16;
  for (const auto format : {F::k16SInt, F::k32SInt, F::k16SInt}) {
    data[0] = uint32_t(format) << 20u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "write-only signed storage image failed materialization");
    const auto conversion = format == F::k16SInt ? format : F::kInvalid;
    const auto view = format == F::k16SInt ? F::k16UInt : F::k32UInt;
    Check(specialization.images[0].numeric_class == N::Uint &&
              specialization.images[0].conversion_format == conversion &&
              specialization.images[0].shader_swizzle == swizzle &&
              snapshot.images[0].dwords[1] == data[0] &&
              RawSignedStorageFormat(plan.info.images[0], format) == view,
          "signed storage specialization lost guest format, conversion, swizzle or raw view width");
    if (format == F::k16SInt) {
      if (signed16.images.empty()) signed16 = specialization;
      else Check(specialization == signed16, "signed storage format refresh retained a stale specialization");
    } else {
      Check(specialization != signed16, "signed 16/32-bit stores shared incompatible conversion state");
    }
    ApplyResourceSpecialization(program, specialization);
    const auto binding = DescriptorBindingForImage(program.info.images[0]);
    Check(binding && ImageBindingResourceClass(*binding) == ImageResourceClass::Storage,
          "signed storage conversion did not reuse the unsigned storage binding");
  }
  for (const auto format : {F::k8SInt, F::k16SInt, F::k32SInt}) {
    data[0] = uint32_t(format) << 20u;
    auto &image = plan.info.images[0];
    image.read = true;
    Check(RawSignedStorageFormat(image, format) == F::kInvalid &&
              !MaterializeResources(plan, runtime, snapshot, specialization),
          "signed raw storage view accepted a non-atomic read/write image");
    image.atomic = true;
    Check((RawSignedStorageFormat(image, format) != F::kInvalid) == (format == F::k32SInt) &&
              MaterializeResources(plan, runtime, snapshot, specialization) == (format == F::k32SInt),
          "signed storage conversion changed atomic format eligibility");
    image.atomic64 = true;
    Check(RawSignedStorageFormat(image, format) == F::kInvalid &&
              !MaterializeResources(plan, runtime, snapshot, specialization),
          "signed 16/32-bit image acquired a 64-bit atomic raw view");
    image.resource_class = ImageResourceClass::Sampled;
    image.written = image.atomic = image.atomic64 = false;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              specialization.images[0].numeric_class == N::Sint &&
              specialization.images[0].conversion_format == F::kInvalid &&
              Libs::Graphics::Prospero::NumBytesPerElement(format) ==
                  (format == F::k8SInt ? 1u : format == F::k16SInt ? 2u : 4u) &&
              RawSignedStorageFormat(image, format) == F::kInvalid,
          "storage conversion changed ordinary signed image reads");
    image.resource_class = ImageResourceClass::Storage;
    image.written = true;
    image.read = false;
  }
}

void TestIndirectImageSamplerClasses(bool mixed) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  using Libs::Graphics::Prospero::BufferFormat;
  using Libs::Graphics::Prospero::TextureNumericClass;
  auto program = MixedSamplerProgram();
  auto &first = program.descriptor_sources[0];
  first.dwords[0] = Value(0x100u);
  first.dwords[1] = Value(static_cast<uint32_t>(BufferFormat::k8UNorm) << 20u);
  first.dwords[3] = program.descriptor_sources[1].dwords[3];
  program.descriptor_sources[1].dwords[1] = Value(static_cast<uint32_t>(
      mixed ? BufferFormat::k8UInt : BufferFormat::k8UNorm) << 20u);
  DescriptorSource root;
  root.dword_count = 8;
  root.dwords.fill(Value(0u));
  root.indirect_descriptor.emplace(DescriptorSource::IndirectDescriptor{}).sources = {0, 1, 0};
  program.info.images.resize(1);
  program.info.images[0].source = static_cast<uint32_t>(program.descriptor_sources.size());
  program.descriptor_sources.push_back(root);
  program.info.samplers.resize(1);
  program.info.sampled_pairs.resize(1);
  auto plan = ExtractResourcePlan(program);
  std::array<uint32_t, 1> user_data{0x11111111u};
  uint32_t reads = 0;
  const SrtRuntime runtime{.user_data = user_data, .userdata = &reads,
                           .read_specialization_memory = RejectSpecializationRead};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && reads == 0 &&
            snapshot.images.size() == 2 && snapshot.images[0].dwords[0] == 0x100u &&
            snapshot.images[1].dwords[0] == 1u &&
            specialization.images[0].numeric_class == TextureNumericClass::Float &&
            specialization.images[1].numeric_class ==
                (mixed ? TextureNumericClass::Uint : TextureNumericClass::Float),
        "indirect image materialization rejected or changed a typed candidate");
  const auto offset = specialization.images[0].indirect_mapping_offset;
  Check(snapshot.flattened_srt[offset] == 3u &&
            snapshot.flattened_srt[offset + 2u] == 0u &&
            snapshot.flattened_srt[offset + 4u] == 1u &&
            snapshot.flattened_srt[offset + 6u] == 0u,
        "mixed image classes changed selector ordinals or duplicated candidates");
  ApplyResourceSpecialization(program, specialization);
  Check(program.info.images[0].indirect_resources == std::vector<uint32_t>({0, 1}) &&
            snapshot.samplers.size() == 1 && program.info.samplers.size() == (mixed ? 2u : 1u),
        "indirect images lost candidates or created unnecessary sampler variants");
  user_data[0] = 0x33333333u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "mixed image sampler refresh failed");
  for (uint32_t index = 0; index < program.info.samplers.size(); ++index) {
    const auto &sampler = program.info.samplers[index];
    Check(sampler.snapshot_index == 0u && sampler.source == plan.info.samplers[0].source &&
              sampler.force_point_filtering == (index != 0u) &&
              sampler.integer_border == (index != 0u) &&
              snapshot.samplers[sampler.snapshot_index].dwords[0] == user_data[0],
          "typed sampler variants lost filtering, border class, or shared live descriptor");
  }
}

// ---- U3: materialization core (unbound shapes, refusals) ----

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
  // Upstream's zero_stride_oob holds for a null V# too; unbound slots are wildcards instead.
  const auto null = MaterializedDescriptor(indexed, {0, 0, 0, 0});
  Check(null.unbound_buffers[0] && null.buffers[0].packed_stride == 0 &&
            null.buffers[0].zero_stride_oob,
        "a null raw V# was not unbound, or lost the folded out-of-bounds shape");
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
  Check(SpecializationServes(raw_bound, raw_null) && !SpecializationServes(raw_null, raw_bound),
        "a null raw V# did not take a bound raw V#'s shape, or lent its own");

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

  // Samplers are part of the module: a wildcard slot never bridges a sampler difference.
  auto sampled = typed;
  sampled.samplers.push_back({});
  auto other_sampler = sampled;
  other_sampler.samplers[0].indirect_root = 0;
  auto sampled_null = formatted_null;
  sampled_null.samplers = sampled.samplers;
  Check(SpecializationServes(sampled, sampled_null) &&
            !SpecializationServes(other_sampler, sampled_null) && !(sampled == other_sampler),
        "the specialization ignored its samplers");
  auto no_donor = sampled_null;
  const std::array<const ResourceSpecialization *, 1> sampler_donor{&other_sampler};
  InheritUnboundShapes(sampler_donor, no_donor);
  Check(no_donor.unbound_buffers[0], "a donor with other samplers shaped the slot");
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

  // Only a null sampled image is a wildcard; a null storage image keeps its shape.
  auto program = MixedSamplerProgram();
  program.info.images.push_back({.source = 0,
                                 .resource_class = ImageResourceClass::Storage,
                                 .numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float,
                                 .dimension = ImageDimension::Dim2D,
                                 .written = true});
  auto plan = ExtractResourcePlan(program);
  const std::array<uint32_t, 2> user_data{0x11111111u, 0x22222222u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, specialization) &&
            specialization.unbound_images == std::vector<bool>({true, false, false}),
        "a null sampled image was bound, or a real or storage image was unbound");

  using Libs::Graphics::ShaderRecompiler::IR::PushData;
  Check(PushData::StartServes(8, 8) && PushData::StartServes(12, 8) &&
            !PushData::StartServes(4, 8) && PushData::StartServes(PushData::NoStart, 8),
        "a push-data start overlapping an earlier stage was reusable, or a later one was not");
}

// A frozen plan's cached phi resolution must accept, refuse and carry as the walk does.
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

// Plan extraction must copy a descriptor chain far deeper than the native stack allows recursion.
void TestDeepChainPlanExtraction() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  constexpr uint32_t kDepth = 50000;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  const auto UserData = [&](uint32_t reg) {
    return Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                                      {Value(static_cast<ScalarReg>(reg))}));
  };
  auto chain = UserData(0);
  const auto step = UserData(1);
  for (uint32_t i = 0; i < kDepth; ++i) {
    chain = Value(&block.AppendNewInst(ValueOpcode::IAdd32, {chain, step}));
  }
  DescriptorSource source;
  source.dword_count = 4;
  source.dwords = {chain, Value(0u), Value(4u), Value(0u)};
  program.descriptor_sources.push_back(source);
  program.info.buffers.push_back({.source = 0});
  auto plan = ExtractResourcePlan(program);
  Check(plan.value_storage.size() >= kDepth + 2u,
        "a deep descriptor chain was not copied once per value");
  const std::array<uint32_t, 2> user_data{0x1000u, 3u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, specialization) &&
            snapshot.buffers.size() == 1u &&
            snapshot.buffers[0].dwords[0] == 0x1000u + kDepth * 3u,
        "a deep descriptor chain did not evaluate from its plan");
}

struct HeapMemory {
  static constexpr uint64_t base = 0x10000u;
  std::vector<uint32_t> words;
  bool gpu_written = false;
};

bool ReadHeapMemory(void *userdata, uint64_t address, std::span<uint32_t> values) {
  const auto &memory = *static_cast<HeapMemory *>(userdata);
  if (address < HeapMemory::base || (address & 3u) != 0u ||
      (address - HeapMemory::base) / 4u + values.size() > memory.words.size())
    return false;
  std::copy_n(memory.words.begin() + (address - HeapMemory::base) / 4u, values.size(),
              values.begin());
  return true;
}

bool HeapGpuOwned(void *userdata, uint64_t, uint64_t) {
  return static_cast<HeapMemory *>(userdata)->gpu_written;
}

// A table the GPU wrote, or whose V# the host cannot evaluate, is decoded in the shader.
void TestBufferTableRefusal() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  for (const bool unevaluable : {false, true}) {
    Program program;
    program.stage = Libs::Graphics::ShaderType::Compute;
    program.srt_plan_complete = program.resource_tracking_complete = true;
    auto &block = AddValueBlock(program);
    DescriptorSource table;
    table.dword_count = 4;
    table.dwords = {unevaluable ? Value(&block.AppendNewInst(ValueOpcode::GetUserData,
                                                             {Value(static_cast<ScalarReg>(0))}))
                                : Value(static_cast<uint32_t>(HeapMemory::base)),
                    Value(0u), Value(4u * 16u), Value(0u)};
    program.descriptor_sources.push_back(table);
    DescriptorSource records;
    records.dword_count = 4;
    records.dwords.fill(Value(0u));
    auto &indirect = records.indirect_descriptor.emplace();
    indirect.table_source = 0;
    indirect.table_stride = 16u;
    indirect.key_count = Value(4u);
    program.descriptor_sources.push_back(records);
    program.info.buffers.push_back({.source = 1, .first_use_pc = 0x40u, .read = true});
    auto plan = ExtractResourcePlan(program);
    HeapMemory memory;
    memory.words.assign(16u, 0u);
    for (uint32_t record = 0; record < 4u; ++record) {
      memory.words[record * 4u] = 0x8000u + record * 0x100u;
      memory.words[record * 4u + 1u] = 1u << 16u;
      memory.words[record * 4u + 2u] = 64u;
      memory.words[record * 4u + 3u] = (3u << 28u) | (4u << 12u);
    }
    const SrtRuntime runtime{.read_memory = ReadHeapMemory, .userdata = &memory,
                             .read_specialization_memory = ReadHeapMemory,
                             .gpu_owned = HeapGpuOwned};
    const std::span<const DescriptorSource> sources(program.descriptor_sources);
    Check(IndirectBufferTableRefused(program, sources, 1u, runtime) == unevaluable &&
              IndirectBufferTableRefused(program, sources, 0u, runtime) == unevaluable,
          "a host-readable table was demoted, or an unevaluable one enumerated");
    if (unevaluable) continue;
    memory.gpu_written = true;
    Check(IndirectBufferTableRefused(program, sources, 1u, runtime),
          "a GPU-written table was enumerated");
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization, before;
    std::vector<uint32_t> refused;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization, &refused) &&
              refused == std::vector<uint32_t>{0x40u} && !LastMaterializeFailure().empty() &&
              specialization == before,
          "a GPU-written table was enumerated, or its refusal named no resource");
    // Without a rebuild channel the table still enumerates, as upstream does.
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers.size() == 4u,
          "a table with no rebuild channel was refused");
    memory.gpu_written = false;
    refused.clear();
    Check(MaterializeResources(plan, runtime, snapshot, specialization, &refused) &&
              refused.empty(),
          "a host-readable table was refused");
  }
}

// A refusal keeps the specialization the permutation lookup compares, and names itself.
void TestRefusalKeepsSpecialization() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto plan = UserDataBufferPlan();
  const std::array<uint32_t, 1> user_data{0x1000u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, specialization) &&
            LastMaterializeFailure().empty(),
        "a valid buffer did not materialize");
  const auto kept = specialization;
  Check(!MaterializeResources(plan, {}, snapshot, specialization) &&
            specialization == kept && specialization.buffers.size() == 1u &&
            !LastMaterializeFailure().empty() &&
            LastMaterializeFailure().find('\n') == std::string_view::npos,
        "a refusal changed the specialization or gave no one-line reason");

  auto program = MixedSamplerProgram();
  ResourceSpecialization shorter;
  shorter.images.resize(1);
  Check(!ApplyResourceSpecialization(program, shorter),
        "a specialization from another tracking was applied");
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

uint32_t Low(uint64_t address) { return static_cast<uint32_t>(address); }
uint32_t High(uint64_t address) { return static_cast<uint32_t>(address >> 32u); }

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
  Value low = Value(Low(address));
  Value high = Value(High(address));
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
  std::array<uint32_t, 2> user_data{Low(a), High(a)};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead};
  const auto point = [&](uint64_t address) { user_data = {Low(address), High(address)}; };
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
    stage.memo.Commit(stage.plan, stage.snapshot, stage.specialization);
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
    Check(!leaf.memo.Replay(leaf.plan, runtime, leaf.snapshot),
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
              relocated.snapshot.user_data[0] == Low(b) &&
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
    Check(!pinned.memo.Replay(pinned.plan, runtime, pinned.snapshot),
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
    Check(!structural.memo.Replay(structural.plan, runtime, structural.snapshot),
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

  {
    Stage buffer{UserDataBufferPlan()};
    std::array<uint32_t, 1> words{7};
    const SrtRuntime direct{.user_data = words,
                            .read_memory = MemoRead,
                            .read_specialization_memory = MemoRead};
    Check(MaterializeResources(buffer.plan, buffer.memo.Record(direct), buffer.snapshot,
                               buffer.specialization),
          "user-data descriptor materialization failed");
    buffer.memo.Commit(buffer.plan, buffer.snapshot, buffer.specialization);
    Check(buffer.memo.Replay(buffer.plan, direct, buffer.snapshot, &buffer.specialization),
          "an unchanged user-data descriptor missed");
    words[0] = 9;
    Check(!buffer.memo.Replay(buffer.plan, direct, buffer.snapshot),
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

  {
    Stage indirect{MemoPlan(MemoBase::Immediate, a, false)};
    indirect.plan.descriptor_sources.emplace_back().indirect_descriptor.emplace();
    Check(!MaterializeMemo::Supports(indirect.plan),
          "a plan with an enumerated table was memoizable");
    const SrtRuntime unreadable{.user_data = user_data, .read_memory = MemoRead};
    Stage blind{MemoPlan(MemoBase::Immediate, a, false)};
    Check(MaterializeResources(blind.plan, blind.memo.Record(unreadable), blind.snapshot,
                               blind.specialization),
          "materialization without a strict reader failed");
    blind.memo.Commit(blind.plan, blind.snapshot, blind.specialization);
    Check(!blind.memo.Valid(), "a walk whose refusals the memo cannot see was committed");
  }
}

// A pointer-relative flat slot, optionally beside a branch on the same table.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan
MemoBranchPlan(bool condition, bool self_compare = false,
               bool capture = false) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  program.capture_specialization_reads = capture;
  auto &block = AddValueBlock(program);
  for (const uint32_t offset : {4u, 8u}) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.planning_only = true;
    memory.offset = offset;
    program.memory_info.push_back(memory);
  }
  auto &lo = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  auto &hi = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(1))});
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource, {Value(&lo), Value(&hi)});
  auto &raw = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                  {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});
  if (condition) {
    auto &word = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                     {Value(&handle), Value(0u), Value(0u), Value(true)});
    word.SetFlags(MemoryFlags{.index = 1, .pc = 0x44});
    auto &test = block.AppendNewInst(
        ValueOpcode::IEqual32, {Value(&word), self_compare ? Value(&word) : Value(2u)});
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
  return ExtractResourcePlan(program);
}

struct MemoConditionMemory {
  uint64_t refused = 0;
};

MemoConditionMemory g_memo_condition;

bool MemoConditionRead(void *data, uint64_t address, std::span<uint32_t> values) {
  return address != g_memo_condition.refused && MemoRead(data, address, values);
}

// The extensions keep the one rule: a replay leaves exactly what a fresh walk would.
void TestMaterializeMemoExtensions() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto &memory = g_memo_memory;
  memory.table_a = {1, 0x1111, 2, 3};
  memory.table_b = {4, 0x2222, 5, 6};
  const auto a = reinterpret_cast<uint64_t>(memory.table_a.data());
  const auto b = reinterpret_cast<uint64_t>(memory.table_b.data());
  std::array<uint32_t, 2> user_data{};
  std::array<uint32_t, 3> groups{4, 1, 1};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoConditionRead,
                           .read_specialization_memory = MemoConditionRead,
                           .workgroup_counts = groups};
  const auto point = [&](uint64_t address) { user_data = {Low(address), High(address)}; };
  const auto same_as_walk = [&](const ResourcePlan &plan, const ResourceSnapshot &replayed,
                                const ResourceSpecialization &specialization) {
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    return MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
           fresh.flattened_srt == replayed.flattened_srt && fresh.buffers == replayed.buffers &&
           fresh.images == replayed.images && fresh.user_data == replayed.user_data &&
           fresh.specialization_reads == replayed.specialization_reads &&
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
  {
    Stage stage{MemoPlan(MemoBase::UserData, 0, true)};
    point(a);
    walk(stage);
    point(b);
    Check(!replay(stage), "a changed descriptor word replayed");
    walk(stage);
    point(a);
    Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
          "an earlier binding did not replay from its own way");
    point(b);
    Check(replay(stage), "a second way did not replay");
    point(a);
  }

  // Dispatch size: no memoizable plan reads it.
  {
    Stage stage{MemoPlan(MemoBase::Immediate, a, false)};
    walk(stage);
    groups = {8, 2, 1};
    Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
          "a dispatch size change missed a plan that never reads it");
    groups = {4, 1, 1};
  }

  // Refusals: a branch read that refused replays only while it still refuses.
  {
    Stage stage{MemoBranchPlan(true)};
    point(a);
    g_memo_condition.refused = a + 8u;
    walk(stage);
    Check(stage.memo.Valid(), "a walk with a refused branch read was not memoized");
    Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
          "a still-refused branch read missed");
    g_memo_condition.refused = 0;
    Check(!replay(stage),
          "a branch read that answers now replayed the walk that saw it refuse");
  }

  // Captured ranges: a relocated branch read moves its captured range with it.
  {
    Stage stage{MemoBranchPlan(true, false, true)};
    point(a);
    walk(stage);
    Check(stage.memo.Valid() && !stage.snapshot.specialization_reads.empty(),
          "the branch plan captured no read ranges");
    memory.table_b[2] = memory.table_a[2];
    point(b);
    Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
          "a relocated captured read did not move its range");
    memory.table_b[2] = 5;
    Check(!replay(stage),
          "a relocated branch word that differs replayed");
    point(a);
  }

  // Integer operations: a pointer rebuilt with shifts, masks and a multiply still relocates.
  {
    Stage stage{MemoPlan(MemoBase::ShiftedUserData, 0, false)};
    point(a);
    walk(stage);
    point(b);
    Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
          "a pointer built with integer operations did not relocate");
    point(a);
  }

  // Self comparison: a branch on x == x reads x, but no value of x changes the branch.
  {
    Stage stage{MemoBranchPlan(true, true)};
    point(a);
    walk(stage);
    memory.table_a[2] = 0x1212;
    Check(replay(stage) && same_as_walk(stage.plan, stage.snapshot, stage.specialization),
          "a word compared only with itself counted as structure");
    memory.table_a[2] = 2;
  }

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

// A data slot the GPU comes to own after recording misses the memo before any read.
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
  memo.Commit(plan, snapshot, specialization);
  Check(memo.Valid() && memo.Replay(plan, runtime, snapshot),
        "a clean data slot missed the memo");
  g_owned_memory.owned = true;
  g_owned_memory.reads = 0;
  Check(!memo.Replay(plan, runtime, snapshot) && g_owned_memory.reads == 0,
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
  const auto point = [&](uint64_t where) { user_data = {Low(where), High(where)}; };
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
  memo.Commit(plan, snapshot, specialization);
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

// N plain buffer V#s read from one table; every dword re-derives, so a replay patches them all.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MemoManyDescriptorPlan(uint32_t count,
                                                                          bool shared = false) {
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
  for (uint32_t index = 0; index < count; index++) {
    DescriptorSource source;
    for (uint32_t dword = 0; dword < 4; dword++) {
      const auto flat = index * 4u + dword;
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.planning_only = true;
      memory.offset = flat * 4u;
      program.memory_info.push_back(memory);
      auto &raw = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                      {Value(&handle), Value(0u), Value(0u), Value(true)});
      raw.SetFlags(MemoryFlags{.index = flat, .pc = 0x40u + flat * 4u});
      program.srt_reads.push_back({Value(&raw), flat});
      auto &read = block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(flat)});
      source.dwords[dword] = Value(&read);
    }
    source.dword_count = 4;
    program.descriptor_sources.push_back(source);
    program.info.buffers.push_back({.source = index});
  }
  if (shared) {
    // One source feeding several bindings: a second buffer and a sampler.
    program.info.buffers.push_back({.source = 0});
    program.info.samplers.push_back({.source = count - 1u});
  }
  return ExtractResourcePlan(program);
}

// A root table at the user-data pointer holding the address of a child table of `count` V#s.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MemoNestedDescriptorPlan(uint32_t count) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  auto &lo = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
  auto &hi = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(1))});
  auto &root = block.AppendNewInst(ValueOpcode::GetAddressResource, {Value(&lo), Value(&hi)});
  std::array<Inst *, 2> pointer{};
  for (uint32_t word = 0; word < 2; word++) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.planning_only = true;
    memory.offset = word * 4u;
    program.memory_info.push_back(memory);
    pointer[word] = &block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                         {Value(&root), Value(0u), Value(0u), Value(true)});
    pointer[word]->SetFlags(
        MemoryFlags{.index = static_cast<uint32_t>(program.memory_info.size() - 1u),
                    .pc = 0x20u + word * 4u});
  }
  auto &child = block.AppendNewInst(ValueOpcode::GetAddressResource,
                                    {Value(pointer[0]), Value(pointer[1])});
  auto &srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
  for (uint32_t index = 0; index < count; index++) {
    DescriptorSource source;
    for (uint32_t dword = 0; dword < 4; dword++) {
      const auto flat = index * 4u + dword;
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.planning_only = true;
      memory.offset = flat * 4u;
      program.memory_info.push_back(memory);
      auto &raw = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                      {Value(&child), Value(0u), Value(0u), Value(true)});
      raw.SetFlags(MemoryFlags{.index = static_cast<uint32_t>(program.memory_info.size() - 1u),
                               .pc = 0x40u + flat * 4u});
      program.srt_reads.push_back({Value(&raw), flat});
      auto &read = block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(flat)});
      source.dwords[dword] = Value(&read);
    }
    source.dword_count = 4;
    program.descriptor_sources.push_back(source);
    program.info.buffers.push_back({.source = index});
  }
  return ExtractResourcePlan(program);
}

// Child tables moved through re-read root words replay like a walk.
void TestMaterializeMemoNestedSharedOperands() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  constexpr uint32_t Count = 6;
  std::vector<uint32_t> children[3];
  for (uint32_t t = 0; t < 3; t++) {
    children[t].resize(Count * 4u);
    for (uint32_t index = 0; index < Count; index++) {
      const uint32_t v[4] = {0x10000000u * (t + 1u) + index * 0x100u, (16u << t) << 16u,
                             32u + t + index, 0x00027facu};
      std::memcpy(&children[t][index * 4u], v, sizeof(v));
    }
  }
  std::array<uint32_t, 2> root{};
  const auto aim = [&](const std::vector<uint32_t> &table) {
    const auto where = reinterpret_cast<uint64_t>(table.data());
    root = {Low(where), High(where)};
  };
  const auto root_address = reinterpret_cast<uint64_t>(root.data());
  const std::array<uint32_t, 2> user_data{Low(root_address), High(root_address)};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead};
  const auto plan = MemoNestedDescriptorPlan(Count);
  MaterializeMemo memo;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  aim(children[0]);
  Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization),
        "the nested plan did not materialize");
  memo.Commit(plan, snapshot, specialization);
  Check(memo.Valid(), "the nested plan was not memoized");
  for (const uint32_t t : {1u, 2u, 2u, 0u, 1u, 0u}) {
    aim(children[t]);
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    Check(memo.Replay(plan, runtime, snapshot, &specialization) &&
              MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
              fresh.buffers == snapshot.buffers && fresh.flattened_srt == snapshot.flattened_srt &&
              fresh_specialization == specialization &&
              snapshot.buffers[Count - 1u].dwords[0] == children[t][(Count - 1u) * 4u],
          "a child table moved through root words did not replay like a walk");
  }
}

// Every binding of a patched descriptor follows a moved V#.
void TestMaterializeMemoPatchTargets() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  constexpr uint32_t Count = 3;
  std::vector<uint32_t> tables[3];
  for (uint32_t t = 0; t < 3; t++) {
    tables[t].resize(Count * 4u);
    for (uint32_t index = 0; index < Count; index++) {
      const uint32_t v[4] = {0x10000000u * (t + 1u) + index * 0x1000u, (16u << t) << 16u,
                             64u + t, 0x00027facu};
      std::memcpy(&tables[t][index * 4u], v, sizeof(v));
    }
  }
  std::array<uint32_t, 2> user_data{};
  const auto point = [&](const std::vector<uint32_t> &table) {
    const auto where = reinterpret_cast<uint64_t>(table.data());
    user_data = {Low(where), High(where)};
  };
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead};
  const auto plan = MemoManyDescriptorPlan(Count, true);
  MaterializeMemo memo;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  point(tables[0]);
  Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization),
        "the shared-source plan did not materialize");
  memo.Commit(plan, snapshot, specialization);
  Check(memo.Valid(), "the shared-source plan was not memoized");
  for (const uint32_t t : {1u, 2u, 2u, 0u, 1u}) {
    point(tables[t]);
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    Check(memo.Replay(plan, runtime, snapshot, &specialization) &&
              MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
              fresh.buffers == snapshot.buffers && fresh.samplers == snapshot.samplers &&
              fresh.flattened_srt == snapshot.flattened_srt &&
              fresh_specialization == specialization &&
              snapshot.buffers[Count].dwords[0] == tables[t][0] &&
              snapshot.samplers[0].dwords[0] == tables[t][(Count - 1u) * 4u],
          "a shared-source descriptor patch did not replay like a walk");
  }
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

// A 64-bit pointer add as IAdd32 halves with a ULessThan32 carry; `guarded` pins the pointer.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MemoCarryPlan(bool guarded) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  const auto user = [&](uint32_t reg) {
    return Value(
        &block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(reg))}));
  };
  const auto base_lo = user(0);
  const auto base_hi = user(1);
  const auto offset = user(2);
  auto low = Value(&block.AppendNewInst(ValueOpcode::IAdd32, {base_lo, offset}));
  const auto carry = Value(&block.AppendNewInst(ValueOpcode::ULessThan32, {low, base_lo}));
  const auto carried =
      Value(&block.AppendNewInst(ValueOpcode::SelectU32, {carry, Value(1u), Value(0u)}));
  const auto high_sum = Value(&block.AppendNewInst(ValueOpcode::IAdd32, {base_hi, Value(0u)}));
  const auto high = Value(&block.AppendNewInst(ValueOpcode::IAdd32, {high_sum, carried}));
  if (guarded) {
    const auto guard =
        Value(&block.AppendNewInst(ValueOpcode::ULessThanEqual32, {user(3), Value(5u)}));
    low = Value(&block.AppendNewInst(ValueOpcode::SelectU32, {guard, low, low}));
  }
  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarAddress;
  memory.planning_only = true;
  memory.offset = 4;
  program.memory_info.push_back(memory);
  auto &handle = block.AppendNewInst(ValueOpcode::GetAddressResource, {low, high});
  auto &raw = block.AppendNewInst(ValueOpcode::LoadAddressU32,
                                  {Value(&handle), Value(0u), Value(0u), Value(true)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});
  return ExtractResourcePlan(program);
}

// The carry form re-derives on replay, including a carry that flips, so the pointer never pins.
void TestMaterializeMemoCarryAddress() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto &memory = g_memo_memory;
  memory.table_a = {1, 0x1111, 2, 3};
  memory.table_b = {4, 0x2222, 5, 6};
  const auto a = reinterpret_cast<uint64_t>(memory.table_a.data());
  const auto b = reinterpret_cast<uint64_t>(memory.table_b.data());
  Check(Low(a) >= 0x10u && Low(b) <= 0xffffffefu,
        "the carry test needs table addresses away from a 4 GiB boundary");
  std::array<uint32_t, 4> user_data{};
  // No carry: a = (hi, lo - 0x10) + 0x10. Carry: b = (hi - 1, lo + 0x10) + 0xfffffff0.
  const auto plain = [&](uint64_t address) {
    user_data = {Low(address) - 0x10u, High(address), 0x10u, user_data[3]};
  };
  const auto carrying = [&](uint64_t address) {
    user_data = {Low(address) + 0x10u, High(address) - 1u, 0xfffffff0u, user_data[3]};
  };
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead};
  const auto same_as_walk = [&](const ResourcePlan &plan, const ResourceSnapshot &replayed) {
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    return MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
           fresh.flattened_srt == replayed.flattened_srt && fresh.user_data == replayed.user_data;
  };
  {
    const auto plan = MemoCarryPlan(false);
    MaterializeMemo memo;
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    plain(a);
    Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0x1111},
          "the carry-form pointer read the wrong word");
    memo.Commit(plan, snapshot, specialization);
    Check(memo.Valid(), "the carry-form plan was not memoized");
    carrying(b);
    Check(memo.Replay(plan, runtime, snapshot) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0x2222} &&
              same_as_walk(plan, snapshot),
          "a carry-form pointer whose carry flips pinned or replayed wrong");
    plain(b);
    Check(memo.Replay(plan, runtime, snapshot) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0x2222} &&
              same_as_walk(plan, snapshot),
          "a carry-form pointer without a carry did not relocate");
    carrying(a);
    Check(memo.Replay(plan, runtime, snapshot) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0x1111} &&
              same_as_walk(plan, snapshot),
          "a carry-form pointer moved back did not relocate");
  }
  // A pinned predicate costs only a miss: the next walk records and replays like the first.
  {
    const auto plan = MemoCarryPlan(true);
    MaterializeMemo memo;
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    user_data[3] = 1u;
    plain(a);
    Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization),
          "the guarded carry plan did not materialize");
    memo.Commit(plan, snapshot, specialization);
    Check(memo.Valid() && memo.Replay(plan, runtime, snapshot),
          "an unchanged guarded pointer missed");
    carrying(b);
    Check(!memo.Replay(plan, runtime, snapshot),
          "a pointer behind a pinned predicate replayed after it moved");
    Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0x2222},
          "the guarded carry plan did not walk after a miss");
    memo.Commit(plan, snapshot, specialization);
    Check(memo.Valid() && memo.Replay(plan, runtime, snapshot) && same_as_walk(plan, snapshot),
          "a re-recorded guarded pointer did not replay like a walk");
    user_data[3] = 0u;
  }
}

// One scalar constant-buffer read at byte 8 of the V# s[0:3]; s2 is its size in bytes.
Libs::Graphics::ShaderRecompiler::IR::ResourcePlan MemoBoundedReadPlan() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  auto &block = AddValueBlock(program);
  std::array<Value, 4> words;
  for (uint32_t i = 0; i < 4; i++) {
    words[i] = Value(
        &block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(i))}));
  }
  auto &handle = block.AppendNewInst(ValueOpcode::GetBufferResource,
                                     {words[0], words[1], words[2], words[3]});
  MemoryInfo memory;
  memory.kind = ResourceKind::ScalarBuffer;
  memory.planning_only = true;
  memory.offset = 8;
  program.memory_info.push_back(memory);
  auto &raw = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {Value(&handle), Value(0u)});
  raw.SetFlags(MemoryFlags{.index = 0, .pc = 0x40});
  program.srt_reads.push_back({Value(&raw), 0});
  return ExtractResourcePlan(program);
}

// A read past its descriptor reads 0; a replay that moves inside or out of it misses.
void TestMaterializeMemoZeroRead() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto &memory = g_memo_memory;
  memory.table_a = {1, 0x1111, 0x7777, 3};
  memory.table_b = {4, 0x2222, 0x8888, 6};
  const auto a = reinterpret_cast<uint64_t>(memory.table_a.data());
  const auto b = reinterpret_cast<uint64_t>(memory.table_b.data());
  std::array<uint32_t, 4> user_data{};
  const auto bind = [&](uint64_t address, uint32_t bytes) {
    user_data = {Low(address), High(address) & 0xffffu, bytes, 0u};
  };
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = MemoRead,
                           .read_specialization_memory = MemoRead};
  const auto same_as_walk = [&](const ResourcePlan &plan, const ResourceSnapshot &replayed) {
    ResourceSnapshot fresh;
    ResourceSpecialization fresh_specialization;
    return MaterializeResources(plan, runtime, fresh, fresh_specialization) &&
           fresh.flattened_srt == replayed.flattened_srt && fresh.user_data == replayed.user_data;
  };
  const auto plan = MemoBoundedReadPlan();
  MaterializeMemo memo;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto record = [&] {
    Check(MaterializeResources(plan, memo.Record(runtime), snapshot, specialization),
          "the bounded read did not materialize");
    memo.Commit(plan, snapshot, specialization);
    Check(memo.Valid(), "the bounded read was not memoized");
  };
  bind(a, 8u);
  record();
  Check(snapshot.flattened_srt == std::vector<uint32_t>{0u}, "a read past its V# did not read 0");
  bind(b, 4u);
  Check(memo.Replay(plan, runtime, snapshot) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0u} && same_as_walk(plan, snapshot),
        "a read that stays outside its moved V# did not replay as 0");
  bind(b, 16u);
  Check(!memo.Replay(plan, runtime, snapshot),
        "a read that moved inside its V# replayed the 0 it read outside");
  record();
  Check(snapshot.flattened_srt == std::vector<uint32_t>{0x8888u},
        "an in-range bounded read read the wrong word");
  bind(a, 12u);
  Check(memo.Replay(plan, runtime, snapshot) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0x7777u} &&
            same_as_walk(plan, snapshot),
        "an in-range read in a moved V# did not relocate");
  bind(a, 11u);
  Check(!memo.Replay(plan, runtime, snapshot),
        "a read that left its V# replayed a word instead of 0");
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
  TestNativeNestedSrtRefresh();
  TestIntegerRuntimeValueFollowsSrtReads();
  TestSrtAliasesRetainReadPolicy();
  TestUniformVectorDescriptorRead();
  TestExactReciprocalDescriptorArithmetic();
  TestUnbasedFlatCacheHitMaterializes();
  for (const bool memory_condition : {false, true})
    TestWrittenDescriptorPredicateReads(memory_condition);
  TestFailedMaterializationRejectsStage();
  TestSharedControlPredicates();
  TestNestedExecPredicatesReuseReads();
  TestFiniteImageRefreshReusesScalarReads();
  for (const bool independent_mask : {false, true})
    TestEmptyTraversalSkipsUnmappedDescriptor(independent_mask);
  TestEmptyTraversalSkipsUnmappedDescriptor(false, true);
  TestCyclicControlPredicatesRemainUnknown();
  TestEmptyScalarBufferSpecialization();
  TestMixedSamplerVariantsShareRuntimeDescriptor();
  TestSignedStorageSpecialization();
  for (const bool mixed : {false, true}) TestIndirectImageSamplerClasses(mixed);
  TestUnboundBufferSpecialization();
  TestUnboundImageSpecialization();
  TestFrozenPhiPlansWalkLikeTheProgram();
  TestDeepChainPlanExtraction();
  TestBufferTableRefusal();
  TestRefusalKeepsSpecialization();
  TestShaderReadCache();
  TestMaterializeMemo();
  TestMaterializeMemoGpuOwnedDataSlot();
  TestMaterializeMemoExtensions();
  TestMaterializeMemoBackoff();
  TestMaterializeMemoDescriptorPatch();
  TestMaterializeMemoPatchTargets();
  TestMaterializeMemoNestedSharedOperands();
  TestMaterializeMemoCarryAddress();
  TestMaterializeMemoZeroRead();
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
