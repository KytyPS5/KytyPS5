#include "graphics/host_gpu/renderer/cache/bindlessTranslation.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
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
  TestFiniteImageRefreshReusesScalarReads();
  TestMixedSamplerVariantsShareRuntimeDescriptor();
  TestBindlessRecordClassification();
  TestBindlessRecordCount();
  TestBindlessSlotAllocation();
  TestBindlessHeapTranslation();
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
