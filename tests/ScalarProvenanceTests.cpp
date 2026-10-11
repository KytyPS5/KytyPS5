#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderComputeInputInfo;
using Libs::Graphics::ShaderType;

void Check(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

template <typename F>
void CheckFatal(F &&function, std::string_view expected, const char *message) {
  try {
    function();
  } catch (const std::runtime_error &error) {
    Check(std::string_view(error.what()).find(expected) !=
              std::string_view::npos,
          message);
    return;
  }
  Check(false, message);
}

struct Fixture {
  Program program;
  ShaderComputeInputInfo compute{};

  explicit Fixture(uint32_t block_count = 1) {
    program.stage = ShaderType::Compute;
    program.shader_hash = 0x12345678u;
    program.user_data_base = 2;
    for (uint32_t index = 0; index < block_count; index++) {
      program.block_storage.push_back(std::make_unique<Block>());
      auto *block = program.block_storage.back().get();
      program.blocks.push_back(block);
      program.blocks.back()->id = index;
    }
  }

  Block &BlockAt(uint32_t index = 0) { return *program.blocks[index]; }

  Value Emit(ValueOpcode opcode, std::initializer_list<Value> args = {},
             uint64_t flags = 0, uint32_t block = 0) {
    return Value(&BlockAt(block).AppendNewInst(opcode, args, flags));
  }

  Value EmitMemory(ValueOpcode opcode, std::initializer_list<Value> args,
                   uint32_t memory, uint32_t pc = 0x40, uint32_t block = 0) {
    MemoryFlags flags{.index = memory, .pc = pc};
    uint64_t bits = 0;
    std::memcpy(&bits, &flags, sizeof(flags));
    return Emit(opcode, args, bits, block);
  }

  uint32_t AddMemory(ResourceKind kind, int32_t offset = 0) {
    MemoryInfo info;
    info.kind = kind;
    info.offset = static_cast<uint32_t>(offset);
    program.memory_info.push_back(info);
    return static_cast<uint32_t>(program.memory_info.size() - 1u);
  }

  void Plan() { TrackResources(program, {}, {}, {.compute = &compute}); }
};

struct TestMemory {
  std::unordered_map<uint64_t, uint32_t> words;
  uint32_t reads = 0;
};

bool ReadMemory(void *userdata, uint64_t address, std::span<uint32_t> values) {
  auto &memory = *static_cast<TestMemory *>(userdata);
  for (auto &value : values) {
    const auto it = memory.words.find(address);
    if (it == memory.words.end()) return false;
    value = it->second;
    address += sizeof(uint32_t);
  }
  memory.reads++;
  return true;
}

Value Address(Fixture &fixture, Value low, Value high, uint32_t block = 0) {
  return fixture.Emit(ValueOpcode::GetAddressResource, {low, high}, 0, block);
}

Value RawRead(Fixture &fixture, Value address, Value offset, uint32_t memory,
              uint32_t block = 0) {
  return fixture.EmitMemory(ValueOpcode::LoadAddressU32,
                            {address, offset, Value(0u), Value(true)}, memory,
                            0x80, block);
}

void LoadBuffer(Fixture &fixture, std::array<Value, 4> words) {
  const auto handle = fixture.Emit(ValueOpcode::GetBufferResource,
                                   {words[0], words[1], words[2], words[3]});
  const auto loaded = fixture.EmitMemory(
      ValueOpcode::LoadBufferU32,
      {handle, Value(0u), Value(0u), Value(0u), Value(true)},
      fixture.AddMemory(ResourceKind::Buffer));
  fixture.Emit(ValueOpcode::ReferenceU32, {loaded});
}

void TestImmediateFlatteningAndGvn() {
  Fixture fixture;
  const auto memory = fixture.AddMemory(ResourceKind::ScalarAddress, 0x20);
  const auto first = RawRead(
      fixture, Address(fixture, Value(0x1000u), Value(0u)), Value(0u), memory);
  const auto second = RawRead(
      fixture, Address(fixture, Value(0x1000u), Value(0u)), Value(0u), memory);
  LoadBuffer(fixture, {first, second, Value(16u), Value(0u)});

  fixture.Plan();
  Check(fixture.program.srt_reads.size() == 1,
        "equivalent typed scalar reads were not coalesced");
  Check(fixture.program.memory_info[memory].planning_only,
        "flattened raw read was not kept as a planning-only root");

  TestMemory memory_image{{{0x1020u, 0xfeedbeefu}}};
  SrtRuntime runtime{.read_memory = ReadMemory, .userdata = &memory_image};
  std::vector<uint32_t> flat;
  Check(SrtWalker(ExtractResourcePlan(fixture.program), runtime).RefreshFlatBuffer(flat), "flattened SRT walk failed");
  Check(flat == std::vector<uint32_t>{0xfeedbeefu} && memory_image.reads == 1,
        "flattened SRT did not evaluate its canonical read once");
}

void TestRawScalarComponentAlignment() {
  Fixture fixture;
  const auto memory = fixture.AddMemory(ResourceKind::ScalarAddress, 2);
  const auto read = RawRead(
      fixture, Address(fixture, Value(0x1003u), Value(0u)), Value(2u), memory);
  LoadBuffer(fixture, {read, Value(0u), Value(16u), Value(0u)});
  fixture.Plan();

  TestMemory memory_image{{{0x1000u, 0x12345678u}}};
  SrtRuntime runtime{.read_memory = ReadMemory, .userdata = &memory_image};
  std::vector<uint32_t> flat;
  Check(SrtWalker(ExtractResourcePlan(fixture.program), runtime).RefreshFlatBuffer(flat), "raw scalar SRT walk failed");
  Check(
      flat == std::vector<uint32_t>{0x12345678u} && memory_image.reads == 1,
      "raw scalar base, immediate, and offset were not aligned independently");
}

void TestScalarMemoryDomainMismatchFails() {
  Fixture raw;
  const auto raw_memory = raw.AddMemory(ResourceKind::ScalarBuffer);
  RawRead(raw, Address(raw, Value(0x1000u), Value(0u)), Value(0u), raw_memory);
  CheckFatal([&] { raw.Plan(); },
             "incompatible scalar memory metadata",
             "raw scalar load accepted descriptor-buffer metadata");

  Fixture buffer;
  const auto buffer_memory = buffer.AddMemory(ResourceKind::ScalarAddress);
  const auto resource =
      buffer.Emit(ValueOpcode::GetBufferResource,
                  {Value(0x1000u), Value(0u), Value(16u), Value(0u)});
  buffer.EmitMemory(ValueOpcode::ReadConstBuffer, {resource, Value(0u)},
                    buffer_memory);
  CheckFatal([&] { buffer.Plan(); },
             "incompatible scalar memory metadata",
             "descriptor scalar load accepted raw-address metadata");
}

void TestDynamicReadRemainsTyped() {
  Fixture fixture;
  const auto memory = fixture.AddMemory(ResourceKind::ScalarAddress);
  const auto offset = fixture.Emit(ValueOpcode::GetUserData,
                                   {Value(static_cast<ScalarReg>(2))});
  const auto read = RawRead(
      fixture, Address(fixture, Value(0x1000u), Value(0u)), offset, memory);
  LoadBuffer(fixture, {read, Value(0u), Value(16u), Value(0u)});

  fixture.Plan();
  Check(fixture.program.srt_reads.empty() &&
            read.ResolveInstruction()->GetOpcode() == ValueOpcode::LoadAddressU32 &&
            !fixture.program.memory_info[memory].planning_only,
        "dynamic scalar read received a fake flattened slot");
}

void TestNestedSrtWalk() {
  Fixture fixture;
  const auto memory = fixture.AddMemory(ResourceKind::ScalarAddress);
  const auto pointer = RawRead(
      fixture, Address(fixture, Value(0x1000u), Value(0u)), Value(0u), memory);
  const auto value =
      RawRead(fixture, Address(fixture, pointer, Value(0u)), Value(0u), memory);
  LoadBuffer(fixture, {value, Value(0u), Value(16u), Value(0u)});
  fixture.Plan();

  TestMemory memory_image{{{0x1000u, 0x2000u}, {0x2000u, 0xabcdef01u}}};
  SrtRuntime runtime{.read_memory = ReadMemory, .userdata = &memory_image};
  std::vector<uint32_t> flat;
  Check(SrtWalker(ExtractResourcePlan(fixture.program), runtime).RefreshFlatBuffer(flat), "nested SRT walk failed");
  Check(flat == std::vector<uint32_t>({0x2000u, 0xabcdef01u}),
        "nested typed SRT reads were not evaluated in dependency order");
}

void TestShaderBaseAndUserData() {
  Fixture fixture;
  const auto base = fixture.Emit(ValueOpcode::GetShaderBase);
  const auto low =
      fixture.Emit(ValueOpcode::CompositeExtractU64, {base, Value(0u)});
  const auto high =
      fixture.Emit(ValueOpcode::CompositeExtractU64, {base, Value(1u)});
  const auto user = fixture.Emit(ValueOpcode::GetUserData,
                                 {Value(static_cast<ScalarReg>(2))});
  const auto sum = fixture.Emit(ValueOpcode::IAdd32, {user, Value(4u)});

  fixture.program.descriptor_sources.push_back(
      {.dwords = {low, high, sum}, .dword_count = 3});

  const std::array user_data{0x20u};
  SrtRuntime runtime{.user_data = user_data,
                     .shader_base = 0x12345678abcdef00ull};
  DescriptorValue result;
  Check(SrtWalker(ExtractResourcePlan(fixture.program), runtime).EvaluateDescriptor(0, result),
        "shader-relative descriptor evaluation failed");
  Check(result.dword_count == 3 && result.dwords[0] == 0xabcdef00u &&
            result.dwords[1] == 0x12345678u && result.dwords[2] == 0x24u,
        "shader-relative typed descriptor expression evaluated incorrectly");
}

void TestCarryAndBitFields() {
  Fixture fixture;
  const auto low = fixture.Emit(ValueOpcode::IAdd32, {Value(0xffffffffu), Value(2u)});
  const auto carry = fixture.Emit(ValueOpcode::ULessThan32, {low, Value(0xffffffffu)});
  const auto high = fixture.Emit(ValueOpcode::SelectU32, {carry, Value(1u), Value(0u)});
  const auto inserted =
      fixture.Emit(ValueOpcode::BitFieldInsert,
                   {Value(0u), Value(0x89abcdefu), Value(0u), Value(32u)});
  const auto sign = fixture.Emit(ValueOpcode::BitFieldSExtract,
                                 {Value(0x000000f0u), Value(4u), Value(4u)});

  fixture.program.descriptor_sources.push_back(
      {.dwords = {low, high, inserted, sign}, .dword_count = 4});

  DescriptorValue result;
  Check(SrtWalker(ExtractResourcePlan(fixture.program), {}).EvaluateDescriptor(0, result),
        "carry and bit-field descriptor evaluation failed");
  Check(result.dwords[0] == 1u && result.dwords[1] == 1u &&
            result.dwords[2] == 0x89abcdefu && result.dwords[3] == 0xffffffffu,
        "typed carry or bit-field runtime evaluation is incorrect");
}

void TestInvariantAndDivergentPhi() {
  Fixture fixture(3);
  auto &invariant = fixture.BlockAt(2).AppendNewInst(ValueOpcode::Phi);
  invariant.SetFlags(Type::U32);
  invariant.AddPhiOperand(&fixture.BlockAt(0), Value(7u));
  invariant.AddPhiOperand(&fixture.BlockAt(1), Value(7u));
  auto &divergent = fixture.BlockAt(2).AppendNewInst(ValueOpcode::Phi);
  divergent.SetFlags(Type::U32);
  divergent.AddPhiOperand(&fixture.BlockAt(0), Value(7u));
  divergent.AddPhiOperand(&fixture.BlockAt(1), Value(9u));

  fixture.program.descriptor_sources.push_back(
      {.dwords = {Value(&invariant)}, .dword_count = 1});
  fixture.program.descriptor_sources.push_back(
      {.dwords = {Value(&divergent)}, .dword_count = 1});

  const auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(SrtWalker(plan, {}).EvaluateDescriptor(0, result) &&
            result.dwords[0] == 7u,
        "loop-invariant typed phi was rejected");
  Check(!SrtWalker(plan, {}).EvaluateDescriptor(1, result),
        "divergent phi was accepted");
}

void TestOperandMutationLifecycle() {
  Fixture fixture(6);
  const auto definition = fixture.Emit(ValueOpcode::IAdd32, {Value(0x55u), Value(0u)});
  auto *source = definition.TryInstruction();
  const auto inserted = fixture.Emit(ValueOpcode::BitFieldInsert,
      {definition, definition, Value(4u), Value(4u)});
  Check(source->UseCount() == 2, "inline operands lost distinct reverse-use indices");
  inserted.TryInstruction()->SetArg(1, Value(3u));
  Check(source->UseCount() == 1, "inline operand mutation did not detach its old use");
  source->ReplaceUsesWith(Value(0xaau));
  uint32_t result = 0;
  Check(source->UseCount() == 0 &&
            SrtWalker(fixture.program, {}).Evaluate(inserted, result) && result == 0x3au,
        "inline operand replacement changed the evaluated value");

  for (const auto opcode : {ValueOpcode::GetImageResource, ValueOpcode::MakeImageAddress}) {
    auto &large = fixture.program.value_storage.emplace_back(opcode);
    auto &replacement = fixture.program.value_storage.emplace_back(opcode);
    const auto count = large.NumArgs();
    for (size_t index = 0; index < count; ++index) {
      large.SetArg(index, definition);
      replacement.SetArg(index, Value(static_cast<uint32_t>(index)));
    }
    const auto user = fixture.Emit(ValueOpcode::Identity, {Value(&large)}, 0, 1);
    Check(source->UseCount() == count, "large operands lost reverse-use indices");
    large.ReplaceUsesWith(Value(&replacement));
    Check(source->UseCount() == 0 && large.GetOpcode() == ValueOpcode::Identity &&
              large.NumArgs() == 1 && large.Arg(0) == Value(&replacement) &&
              user.Resolve() == Value(&replacement) && replacement.UseCount() == 2,
          "large operand replacement did not preserve users or detach old operands");
    large.Invalidate();
    large.Invalidate();
    Check(large.NumArgs() == 0 && replacement.UseCount() == 1,
          "repeated invalidation detached a surviving large-value user");
  }

  auto &phi = fixture.BlockAt(5).AppendNewInst(ValueOpcode::Phi);
  phi.SetFlags(Type::U32);
  for (size_t index = 0; index < 5; ++index) {
    phi.AddPhiOperand(&fixture.BlockAt(index), definition);
  }
  phi.SetArg(3, Value(7u));
  Check(phi.NumArgs() == 5 && phi.NumPhiBlocks() == 5 && source->UseCount() == 4,
        "Phi mutation lost its incoming values or reverse uses");
  for (size_t index = 0; index < 5; ++index) {
    Check(phi.PhiBlock(index) == &fixture.BlockAt(index),
          "Phi mutation changed an incoming predecessor");
  }
  const auto sum = fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(5u)}, 0, 5);
  phi.ReplaceUsesWith(Value(6u));
  Check(source->UseCount() == 0 && phi.NumPhiBlocks() == 0 && phi.NumArgs() == 1 &&
            SrtWalker(fixture.program, {}).Evaluate(sum, result) && result == 11,
        "Phi replacement retained incoming uses or changed the consumer value");
}

void TestControlDependentStandaloneLoadStaysTyped() {
  Fixture fixture(3);
  const auto memory = fixture.AddMemory(ResourceKind::ScalarAddress);
  auto &base = fixture.BlockAt(2).AppendNewInst(ValueOpcode::Phi);
  base.SetFlags(Type::U32);
  base.AddPhiOperand(&fixture.BlockAt(0), Value(0x1000u));
  base.AddPhiOperand(&fixture.BlockAt(1), Value(0x2000u));
  const auto read =
      RawRead(fixture, Address(fixture, Value(&base), Value(0u), 2), Value(0u),
              memory, 2);
  fixture.Emit(ValueOpcode::ReferenceU32, {read}, 0, 2);
  fixture.Plan();
  Check(fixture.program.srt_reads.empty() &&
            read.ResolveInstruction()->GetOpcode() ==
                ValueOpcode::LoadAddressU32 &&
            !fixture.program.memory_info[memory].planning_only,
        "control-dependent standalone scalar load was flattened into a host "
        "snapshot");
}

void TestRuntime64BitDescriptorOps() {
  Fixture fixture;
  const auto shifted = fixture.Emit(ValueOpcode::ShiftLeftLogical64,
                                    {Value(uint64_t{0x1234u}), Value(32u)});
  const auto masked =
      fixture.Emit(ValueOpcode::BitwiseAnd64,
                   {shifted, Value(uint64_t{0x0000ffff00000000ull})});
  const auto combined = fixture.Emit(
      ValueOpcode::IAdd64, {masked, Value(uint64_t{0x000000010000abcdull})});
  const auto low =
      fixture.Emit(ValueOpcode::CompositeExtractU64, {combined, Value(0u)});
  const auto high =
      fixture.Emit(ValueOpcode::CompositeExtractU64, {combined, Value(1u)});

  fixture.program.descriptor_sources.push_back(
      {.dwords = {low, high}, .dword_count = 2});

  DescriptorValue result;
  Check(SrtWalker(ExtractResourcePlan(fixture.program), {}).EvaluateDescriptor(0, result) &&
            result.dwords[0] == 0xabcdu && result.dwords[1] == 0x1235u,
        "64-bit typed descriptor arithmetic evaluation is incorrect");
}

void TestUniformFirstLaneSamplerLod() {
  Fixture fixture;
  const auto active = fixture.Emit(
      ValueOpcode::IEqual32, {fixture.Emit(ValueOpcode::LaneId), Value(0u)});
  const auto stale = fixture.Emit(
      ValueOpcode::GetVectorRegister, {Value(static_cast<VectorReg>(0))});
  const auto write = [&](Value value, Value previous) {
    return fixture.Emit(ValueOpcode::SelectU32, {active, value, previous});
  };
  const auto user = fixture.Emit(
      ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(8))});
  const auto as_float = fixture.Emit(ValueOpcode::ConvertF32U32, {user});
  const auto initial = write(
      fixture.Emit(ValueOpcode::BitCastU32F32, {as_float}), stale);
  const auto initial_float =
      fixture.Emit(ValueOpcode::BitCastF32U32, {initial});
  const auto scaled = fixture.Emit(ValueOpcode::FPMul32,
                                   {Value::F32(256.0f), initial_float});
  const auto scaled_write = write(
      fixture.Emit(ValueOpcode::BitCastU32F32, {scaled}), initial);
  const auto scaled_float =
      fixture.Emit(ValueOpcode::BitCastF32U32, {scaled_write});
  const auto nan = fixture.Emit(ValueOpcode::FPIsNan32, {scaled_float});
  const auto low = fixture.Emit(ValueOpcode::FPOrdLessThanEqual32,
                                {scaled_float, Value::F32(0.0f)});
  const auto high = fixture.Emit(ValueOpcode::FPOrdGreaterThanEqual32,
                                 {scaled_float, Value::F32(4294967296.0f)});
  const auto truncated = fixture.Emit(ValueOpcode::FPTrunc32, {scaled_float});
  const auto safe_low = fixture.Emit(
      ValueOpcode::SelectF32,
      {fixture.Emit(ValueOpcode::LogicalOr, {nan, low}), Value::F32(0.0f),
       truncated});
  const auto safe = fixture.Emit(
      ValueOpcode::SelectF32,
      {high, Value::F32(4294967040.0f), safe_low});
  const auto converted = fixture.Emit(ValueOpcode::ConvertU32F32, {safe});
  const auto saturated = write(
      fixture.Emit(ValueOpcode::SelectU32,
                   {high, Value(UINT32_MAX), converted}),
      scaled_write);
  const auto lod = fixture.Emit(ValueOpcode::UMin32,
                                {saturated, Value(0xfffu)});
  const auto lod_write = write(lod, saturated);
  const auto masked = fixture.Emit(ValueOpcode::BitwiseAnd32,
                                   {lod_write, Value(0xfffu)});
  const auto masked_write = write(masked, lod_write);
  const auto packed = write(
      fixture.Emit(
          ValueOpcode::BitwiseOr32,
          {fixture.Emit(ValueOpcode::ShiftLeftLogical32,
                        {masked_write, Value(12u)}),
           masked_write}),
      masked_write);
  const auto first = fixture.Emit(ValueOpcode::ReadFirstLane,
                                  {packed, active});
  const auto divergent = fixture.Emit(
      ValueOpcode::ReadFirstLane, {stale, active});

  Check(ValidateRuntimeValue(fixture.program, first),
        "uniform sampler LOD construction was rejected");
  Check(!ValidateRuntimeValue(fixture.program, divergent),
        "divergent first-lane value was accepted as uniform");
  fixture.program.descriptor_sources.push_back(
      {.dwords = {first}, .dword_count = 1});

  std::array<uint32_t, 7> user_data{};
  user_data[6] = 3u;
  SrtRuntime runtime{.user_data = user_data};
  const auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(0, result) &&
            result.dwords[0] == 0x00300300u,
        "uniform sampler LOD evaluated incorrectly");
  user_data[6] = 20u;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(0, result) &&
            result.dwords[0] == 0x00ffffffu,
        "uniform sampler LOD clamp evaluated incorrectly");
}

void TestFloatComparisonDescriptorInputs() {
  struct Input {
    uint32_t bits;
    uint32_t less_equal;
    uint32_t greater_equal;
  };
  constexpr std::array inputs = {
      Input{0x00000001u, 0, 1}, Input{0x80000001u, 1, 0},
      Input{0x007fffffu, 0, 1}, Input{0x807fffffu, 1, 0},
      Input{0x00800000u, 0, 1}, Input{0x80800000u, 1, 0},
      Input{0x00000000u, 1, 1}, Input{0x80000000u, 1, 1},
      Input{0x7fc00000u, 0, 0}};
  for (const bool flush : {false, true}) {
    Fixture fixture;
    const auto user = fixture.Emit(
        ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(2))});
    const auto value = fixture.Emit(ValueOpcode::BitCastF32U32, {user});
    const auto less_equal = fixture.Emit(ValueOpcode::FPOrdLessThanEqual32,
                                         {value, Value::F32(0.0f)});
    const auto greater_equal = fixture.Emit(ValueOpcode::FPOrdGreaterThanEqual32,
                                            {value, Value::F32(0.0f)});
    less_equal.Instruction()->SetFlags(FPCompareFlags{flush});
    greater_equal.Instruction()->SetFlags(FPCompareFlags{flush});
    const auto low = fixture.Emit(ValueOpcode::SelectU32,
                                  {less_equal, Value(1u), Value(0u)});
    const auto high = fixture.Emit(ValueOpcode::SelectU32,
                                   {greater_equal, Value(1u), Value(0u)});
    fixture.program.descriptor_sources.push_back(
        {.dwords = {low, high}, .dword_count = 2});

    const auto plan = ExtractResourcePlan(fixture.program);
    for (size_t index = 0; index < inputs.size(); index++) {
      const auto &input = inputs[index];
      const std::array user_data{input.bits};
      DescriptorValue result;
      Check(SrtWalker(plan, {.user_data = user_data})
                    .EvaluateDescriptor(0, result) &&
                result.dwords[0] == (flush && index < 4 ? 1 : input.less_equal) &&
                result.dwords[1] == (flush && index < 4 ? 1 : input.greater_equal),
            "descriptor comparison disagrees with native FP32 input mode");
    }
  }
}

void TestSharedIntegerRuntimeDependencies() {
  Fixture fixture;
  const auto lane = fixture.Emit(ValueOpcode::LaneId);
  const auto active =
      fixture.Emit(ValueOpcode::IEqual32, {lane, Value(0u)});
  auto inactive = lane;
  for (uint32_t level = 0; level < 36; level++) {
    const auto left =
        fixture.Emit(ValueOpcode::IAdd32, {inactive, Value(1u)});
    const auto right =
        fixture.Emit(ValueOpcode::IMul32, {inactive, Value(3u)});
    inactive = fixture.Emit(ValueOpcode::BitwiseXor32, {left, right});
  }
  const auto selected = fixture.Emit(
      ValueOpcode::SelectU32, {active, Value(42u), inactive});
  const auto first =
      fixture.Emit(ValueOpcode::ReadFirstLane, {selected, active});
  Check(ValidateRuntimeValue(fixture.program, first, RuntimeValueType::Integer),
        "shared integer dependencies behind an inactive arm were rejected");

  const auto uniform_use = fixture.Emit(ValueOpcode::IAdd32, {first, inactive});
  Check(!ValidateRuntimeValue(fixture.program, uniform_use,
                              RuntimeValueType::Integer),
        "integer-only dependency acceptance was reused as uniform acceptance");

  const auto other_active =
      fixture.Emit(ValueOpcode::IEqual32, {lane, Value(1u)});
  const auto other_first =
      fixture.Emit(ValueOpcode::ReadFirstLane, {selected, other_active});
  const auto both = fixture.Emit(ValueOpcode::IAdd32, {first, other_first});
  Check(!ValidateRuntimeValue(fixture.program, both, RuntimeValueType::Integer),
        "uniform acceptance was reused across different execution masks");

  const auto floating =
      fixture.Emit(ValueOpcode::BitCastU32F32, {Value::F32(1.f)});
  const auto mixed =
      fixture.Emit(ValueOpcode::BitwiseOr32, {inactive, floating});
  selected.ResolveInstruction()->SetArg(2, mixed);
  Check(!ValidateRuntimeValue(fixture.program, first, RuntimeValueType::Integer),
        "shared integer dependencies hid a floating-point sibling");
}

void TestConstantBufferBounds() {
  struct Case {
    uint32_t offset;
    uint32_t immediate;
    uint32_t stride;
    uint32_t records;
    bool valid;
    uint32_t expected;
    uint32_t reads;
  };
  struct MemoryProbe {
    TestMemory memory{{{0x3000u, 0x12345678u}, {0x300cu, 0xa5a5a5a5u}}};
    uint32_t attempts = 0;
  };
  const auto read_memory = [](void *userdata, uint64_t address,
                              std::span<uint32_t> values) {
    auto &probe = *static_cast<MemoryProbe *>(userdata);
    ++probe.attempts;
    return ReadMemory(&probe.memory, address, values);
  };
  // PS5 scalar-buffer loads return zero per out-of-range DWORD; an unreadable
  // in-range address remains a failed memory read.
  for (const auto &test : {Case{12u, 0u, 0u, 16u, true, 0xa5a5a5a5u, 1u},
                           Case{3u, 1u, 0u, 16u, true, 0x12345678u, 1u},
                           Case{0xfffffffcu, 4u, 0u, 16u, true, 0u, 0u},
                           Case{16u, 0u, 0u, 16u, true, 0u, 0u},
                           Case{12u, 0u, 0u, 14u, true, 0u, 0u},
                           Case{0u, 0u, 0u, 0u, true, 0u, 0u},
                           Case{8u, 0u, 0u, 16u, false, 0u, 1u},
                           Case{12u, 0u, 48u, 32768u, true, 0xa5a5a5a5u, 1u},
                           Case{0x6339c0u, 16u, 48u, 32768u, true, 0u, 0u}}) {
    Fixture fixture;
    const auto memory = fixture.AddMemory(ResourceKind::ScalarBuffer, test.immediate);
    const auto buffer =
        fixture.Emit(ValueOpcode::GetBufferResource,
                     {Value(0x3000u), Value(test.stride << 16u),
                      Value(test.records), Value(0u)});
    const auto read = fixture.EmitMemory(ValueOpcode::ReadConstBuffer,
                                         {buffer, Value(test.offset)}, memory);
    LoadBuffer(fixture, {read, Value(0u), Value(16u), Value(0u)});
    fixture.Plan();

    MemoryProbe memory_image;
    SrtRuntime runtime{.read_memory = read_memory, .userdata = &memory_image};
    std::vector<uint32_t> flat;
    Check(SrtWalker(ExtractResourcePlan(fixture.program), runtime).RefreshFlatBuffer(flat) == test.valid,
          "constant-buffer walk misaligned or wrapped its offset components");
    Check((!test.valid || flat == std::vector<uint32_t>{test.expected}) &&
              memory_image.attempts == test.reads,
          "constant-buffer walk read the wrong word or accessed an out-of-bounds address");
  }
}

void TestReadLaneElimination() {
  Fixture fixture;
  const auto undef = fixture.Emit(ValueOpcode::UndefU32);
  const auto write = fixture.Emit(ValueOpcode::WriteLane,
                                  {undef, Value(0xdeadbeefu), Value(5u)});
  const auto read = fixture.Emit(ValueOpcode::ReadLane, {write, Value(5u)});
  const auto use = fixture.Emit(ValueOpcode::IAdd32, {read, Value(1u)});
  const auto stats = EliminateReadLane(fixture.program, 64);
  Check(stats.rewritten_reads == 1 &&
            use.ResolveInstruction()->Arg(0).Resolve() == Value(0xdeadbeefu),
        "fixed-lane typed read was not rewritten from its SSA write chain");

  const auto selector = fixture.Emit(ValueOpcode::GetUserData,
                                     {Value(static_cast<ScalarReg>(2))});
  const auto dynamic = fixture.Emit(ValueOpcode::ReadLane, {write, selector});
  fixture.Emit(ValueOpcode::IAdd32, {dynamic, Value(1u)});
  Check(EliminateReadLane(fixture.program, 64).rewritten_reads == 0,
        "dynamic-lane read was rewritten unsafely");
}

void TestCompleteUMinReduction() {
  enum class Case {
    Masked, Subset, Literal, LiteralMasked, LiteralMove, Wave32,
    UnrelatedSeed, UnmaskedSymbolic, WrongNeutral, RowMask, BankMask,
    Shift, FetchInactive, BoundControl, Dpp8, MoveExec, UpdateExec,
    Previous, DppSource, CrossExec, CrossRow, CrossSelectorLow,
    CrossSelectorHigh, CrossFetch, CrossBound, CrossSource, FinalExec,
    Endpoint, EndpointSource, DifferentWitness, CrossBlock, WrongOperation
  };
  for (const auto test : {Case::Masked, Case::Subset, Case::Literal,
       Case::LiteralMasked, Case::LiteralMove, Case::Wave32, Case::UnrelatedSeed,
       Case::UnmaskedSymbolic, Case::WrongNeutral, Case::RowMask, Case::BankMask,
       Case::Shift, Case::FetchInactive, Case::BoundControl, Case::Dpp8,
       Case::MoveExec, Case::UpdateExec, Case::Previous, Case::DppSource,
       Case::CrossExec, Case::CrossRow, Case::CrossSelectorLow,
       Case::CrossSelectorHigh, Case::CrossFetch, Case::CrossBound,
       Case::CrossSource, Case::FinalExec, Case::Endpoint, Case::EndpointSource,
       Case::DifferentWitness, Case::CrossBlock, Case::WrongOperation}) {
    Fixture fixture(test == Case::CrossBlock ? 2 : 1);
    const auto source = fixture.Emit(ValueOpcode::LaneId);
    const auto mask = fixture.Emit(ValueOpcode::ULessThan32, {source, Value(19u)});
    const auto other_mask = fixture.Emit(ValueOpcode::IEqual32, {source, Value(7u)});
    const auto negated = fixture.Emit(ValueOpcode::LogicalNot, {mask});
    const bool literal = test == Case::Literal || test == Case::LiteralMasked;
    const auto enabled = literal ? Value(true) :
        fixture.Emit(ValueOpcode::LogicalOr, {negated, mask});
    auto seed_mask = test == Case::UnrelatedSeed ? other_mask : mask;
    if (test == Case::Subset)
      seed_mask = fixture.Emit(ValueOpcode::LogicalAnd, {other_mask, mask});
    const auto seed = test == Case::Literal || test == Case::UnmaskedSymbolic ? source :
        fixture.Emit(ValueOpcode::SelectU32,
            {seed_mask, source, Value(test == Case::WrongNeutral ? 0u : UINT32_MAX)});
    auto prefix = fixture.Emit(ValueOpcode::SelectU32, {enabled, seed, source});
    Value preserved;
    const auto operation = test == Case::WrongOperation ? ValueOpcode::BitwiseOr32 :
                                                        ValueOpcode::UMin32;
    for (const uint32_t shift : {1u, 2u, 4u, 8u}) {
      DppMoveFlags flags{.control = 0x110u + shift};
      if (shift == 2) {
        if (test == Case::RowMask) flags.row_mask = 7;
        if (test == Case::BankMask) flags.bank_mask = 7;
        if (test == Case::Shift) flags.control = 0x113;
        if (test == Case::FetchInactive) flags.fetch_inactive = true;
        if (test == Case::BoundControl) flags.bound_control = true;
        if (test == Case::Dpp8) flags.dpp8 = true;
      }
      const auto move = fixture.Emit(ValueOpcode::DppMoveU32,
          {test == Case::DppSource ? source : prefix,
           test == Case::MoveExec ? mask : test == Case::LiteralMove ? Value(true) : enabled});
      move.Instruction()->SetFlags(flags);
      const auto combined = fixture.Emit(operation, {move, prefix});
      const auto update = fixture.Emit(ValueOpcode::DppUpdateU32,
          {combined, test == Case::Previous ? source : prefix,
           test == Case::UpdateExec ? mask : enabled});
      update.Instruction()->SetFlags(flags);
      prefix = update;
      if (shift == 2) preserved = fixture.Emit(ValueOpcode::ReferenceU32, {prefix});
    }
    auto cross_exec = test == Case::CrossExec ? mask : enabled;
    if (test == Case::DifferentWitness) {
      const auto other_not = fixture.Emit(ValueOpcode::LogicalNot, {other_mask});
      cross_exec = fixture.Emit(ValueOpcode::LogicalOr, {other_mask, other_not});
    }
    const auto cross = fixture.Emit(ValueOpcode::Permlane16U32,
        {test == Case::CrossSource ? source : prefix,
         Value(test == Case::CrossSelectorLow ? 0xfffffff0u : UINT32_MAX),
         Value(test == Case::CrossSelectorHigh ? 0xfffffff0u : UINT32_MAX), cross_exec});
    cross.Instruction()->SetFlags(PermlaneFlags{
        .x16 = test != Case::CrossRow, .fetch_inactive = test == Case::CrossFetch,
        .bound_control = test != Case::CrossBound});
    const auto cross_write = fixture.Emit(ValueOpcode::SelectU32, {enabled, cross, source});
    const auto combined = fixture.Emit(operation, {prefix, cross_write});
    const auto final = fixture.Emit(ValueOpcode::SelectU32,
        {test == Case::FinalExec ? mask : enabled, combined, prefix});
    const uint32_t result_block = test == Case::CrossBlock ? 1u : 0u;
    const auto low = fixture.Emit(ValueOpcode::ReadLane,
        {final, Value(test == Case::Endpoint ? 30u : 31u)}, 0, result_block);
    const auto high = fixture.Emit(ValueOpcode::ReadLane,
        {test == Case::EndpointSource ? prefix : final, Value(63u)}, 0, result_block);
    const auto result = fixture.Emit(operation, {high, low}, 0, result_block);
    const auto use = fixture.Emit(ValueOpcode::ReferenceU32, {result}, 0, result_block);
    using Terminator = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind;
    fixture.BlockAt(result_block).terminator.kind = Terminator::Return;
    if (result_block != 0) {
      fixture.BlockAt().AddBranch(&fixture.BlockAt(1));
      fixture.BlockAt().terminator.kind = Terminator::Branch;
      fixture.BlockAt().terminator.true_block = &fixture.BlockAt(1);
    }
    ValidateProgram(fixture.program, true);
    const auto stats = EliminateReadLane(fixture.program, test == Case::Wave32 ? 32u : 64u);
    const bool matches = test == Case::Masked || test == Case::Subset ||
        test == Case::Literal || test == Case::LiteralMasked || test == Case::LiteralMove;
    Check(stats.rewritten_reductions == (matches ? 1u : 0u) && stats.rewritten_reads == 0,
          ("UMin reduction proof failed case " + std::to_string(static_cast<unsigned>(test))).c_str());
    if (matches) {
      const auto *reduction = use.Instruction()->Arg(0).ResolveInstruction();
      Check(reduction->GetOpcode() == ValueOpcode::SubgroupUMinU32 &&
                reduction->Arg(0).Resolve() == seed.Resolve(),
            "UMin reduction changed the neutral-masked input");
      RemoveIdentities(fixture.program);
      EliminateDeadCode(fixture.program.blocks);
      uint32_t updates = 0;
      for (const auto &inst : fixture.BlockAt()) {
        updates += inst.GetOpcode() == ValueOpcode::DppUpdateU32;
        Check(inst.GetOpcode() != ValueOpcode::ReadLane &&
                  inst.GetOpcode() != ValueOpcode::Permlane16U32,
              "UMin reduction retained unused endpoint or cross-row operations");
      }
      Check(updates == 2 && preserved.Instruction()->Arg(0).Instruction()->HasUses(),
            "UMin reduction destroyed an independently consumed prefix or kept its dead suffix");
      ValidateProgram(fixture.program, true);
    } else {
      Check(use.Instruction()->Arg(0).Resolve() == result,
            "rejected UMin proof changed its original result");
    }
  }
}

void TestOptimizationPipeline() {
  Fixture fixture;
  const auto sum = fixture.Emit(ValueOpcode::IAdd32, {Value(40u), Value(2u)});
  const auto kept = fixture.Emit(ValueOpcode::BitwiseOr32, {sum, Value(0u)});
  fixture.Emit(ValueOpcode::ReferenceU32, {kept});
  fixture.Emit(ValueOpcode::IMul32, {Value(6u), Value(7u)});

  ConstantPropagationPass(fixture.program.blocks);
  RemoveIdentities(fixture.program);
  EliminateDeadCode(fixture.program.blocks);

  const auto &instructions = fixture.BlockAt().Instructions();
  Check(instructions.size() == 1 &&
            instructions.front().GetOpcode() == ValueOpcode::ReferenceU32 &&
            instructions.front().Arg(0).Resolve() == Value(42u),
        "typed constant propagation, identity folding, or dead-code "
        "elimination regressed");
}

void TestControlFlowValueSurvivesReadLaneFolding() {
  Fixture fixture(3);
  auto *entry = fixture.program.blocks[0];
  auto *taken = fixture.program.blocks[1];
  auto *other = fixture.program.blocks[2];
  entry->AddBranch(taken);
  entry->AddBranch(other);

  auto &entry_info = *fixture.program.blocks[0];
  entry_info.terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  entry_info.terminator.true_block = taken;
  entry_info.terminator.false_block = other;
  fixture.program.blocks[1]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  fixture.program.blocks[2]->terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;

  const auto undef = fixture.Emit(ValueOpcode::UndefU32);
  const auto write =
      fixture.Emit(ValueOpcode::WriteLane, {undef, Value(42u), Value(5u)});
  const auto read = fixture.Emit(ValueOpcode::ReadLane, {write, Value(5u)});
  entry_info.condition =
      fixture.Emit(ValueOpcode::IEqual32, {read, Value(42u)});
  fixture.Emit(ValueOpcode::Reference, {entry_info.condition});

  Check(EliminateReadLane(fixture.program, 64).rewritten_reads == 1,
        "control-flow fixture did not eliminate its fixed-lane read");
  ConstantPropagationPass(fixture.program.blocks);
  RemoveIdentities(fixture.program);
  EliminateDeadCode(fixture.program.blocks);

  Check(entry_info.condition == Value(true),
        "folded branch condition did not survive identity removal");
  ValidateProgram(fixture.program, true);
}

void TestDeadPhiCyclesAndPlanningRoots() {
  Fixture fixture(2);
  auto &entry = fixture.BlockAt(0);
  auto &loop = fixture.BlockAt(1);
  entry.AddBranch(&loop);
  loop.AddBranch(&loop);
  const auto source = fixture.Emit(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(2))});
  const auto identity = fixture.Emit(ValueOpcode::Identity, {source});
  auto &live = loop.AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
  live.AddPhiOperand(&entry, identity);
  live.AddPhiOperand(&loop, Value(&live));
  const auto reference = fixture.Emit(ValueOpcode::ReferenceU32, {Value(&live)}, 0, 1);
  auto &dead = loop.AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
  const auto increment = fixture.Emit(ValueOpcode::IAdd32, {Value(&dead), Value(1u)}, 0, 1);
  dead.AddPhiOperand(&entry, source);
  dead.AddPhiOperand(&loop, increment);
  const auto retained = fixture.Emit(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(3))});
  auto &planning = fixture.program.value_storage.emplace_back(ValueOpcode::Identity);
  planning.SetArg(0, retained);

  EliminateDeadCode(fixture.program.blocks);
  Check(entry.Instructions().size() == 3 && loop.Instructions().size() == 2,
        "dead Phi cycle survived or a live/planning dependency was removed");
  Check(identity.Instruction()->Arg(0) == source && live.Arg(0) == identity &&
            live.Arg(1) == Value(&live) && planning.Arg(0) == retained,
        "DCE did not preserve direct Identity operands or live Phi recurrence");
  // Dropping roots must clear prior marks and remove the newly dead SCC safely.
  reference.Instruction()->Invalidate();
  planning.Invalidate();
  EliminateDeadCode(fixture.program.blocks);
  Check(entry.empty() && loop.empty(), "DCE reused stale marks after removing its roots");
}

void TestUndefinedRuntimeValueFails() {
  Fixture fixture;
  const auto undef = fixture.Emit(ValueOpcode::UndefU32);

  fixture.program.descriptor_sources.push_back(
      {.dwords = {undef}, .dword_count = 1});
  DescriptorValue result;
  Check(!SrtWalker(ExtractResourcePlan(fixture.program), {}).EvaluateDescriptor(0, result),
        "undefined typed descriptor source was accepted");
}

// SRT walker port (U1). Deep enough to overflow a natively recursive walk or validator.
void TestDeepUniformValueChain() {
  constexpr uint32_t kDepth = 50000;
  Fixture fixture;
  const auto step = fixture.Emit(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(3))});
  auto chain = fixture.Emit(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(2))});
  for (uint32_t i = 0; i < kDepth; ++i) {
    chain = fixture.Emit(ValueOpcode::IAdd32, {chain, step});
  }
  Check(ValidateRuntimeValue(fixture.program, chain, RuntimeValueType::Integer),
        "a deep uniform chain was not accepted as a runtime value");
  const std::array<uint32_t, 2> user_data{0x1000u, 3u};
  uint32_t value = 0;
  Check(SrtWalker(fixture.program, {.user_data = user_data}).Evaluate(chain, value) &&
            value == 0x1000u + kDepth * 3u,
        "a deep uniform chain did not evaluate");
  fixture.program.descriptor_sources.push_back({.dwords = {chain}, .dword_count = 1});
  DescriptorValue descriptor;
  Check(SrtWalker(fixture.program, {.user_data = user_data}).EvaluateDescriptor(0, descriptor) &&
            descriptor.dwords[0] == 0x1000u + kDepth * 3u,
        "a deep uniform descriptor dword did not evaluate");
}

Value UserData(Fixture &fixture, uint32_t index) {
  return fixture.Emit(ValueOpcode::GetUserData,
                      {Value(static_cast<ScalarReg>(fixture.program.user_data_base + index))});
}

uint32_t PushDescriptor(Fixture &fixture, std::array<Value, 4> words) {
  fixture.program.descriptor_sources.push_back(
      {.dwords = {words[0], words[1], words[2], words[3]}, .dword_count = 4});
  return static_cast<uint32_t>(fixture.program.descriptor_sources.size() - 1u);
}

// The materializer's pairing: a walker whose selects and clean reads go through a clean one.
bool EvaluateWithClean(const ResourcePlan &plan, uint32_t source, const SrtRuntime &runtime,
                       DescriptorValue &descriptor, SrtRefusal *refusal = nullptr) {
  SrtWalker clean(plan, CleanRuntime(runtime));
  SrtWalker walker(plan, runtime, &clean);
  const bool evaluated = walker.EvaluateDescriptor(source, descriptor);
  if (refusal != nullptr) *refusal = walker.Refusal();
  return evaluated;
}

// Translator::ThreadBit: bit N of a 64-bit scalar mask, for lane N.
Value ThreadBit(Fixture &fixture, Value low, Value high) {
  const auto lane = fixture.Emit(ValueOpcode::LaneId);
  const auto word = fixture.Emit(
      ValueOpcode::SelectU32,
      {fixture.Emit(ValueOpcode::ULessThan32, {lane, Value(32u)}), low, high});
  const auto shifted = fixture.Emit(
      ValueOpcode::ShiftRightLogical32,
      {word, fixture.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(31u)})});
  return fixture.Emit(
      ValueOpcode::INotEqual32,
      {fixture.Emit(ValueOpcode::BitwiseAnd32, {shifted, Value(1u)}), Value(0u)});
}

// A loop phi stands for one descriptor only when every iteration reproduces the entry value.
void TestAdvancingLoopPhi() {
  for (const uint32_t step : {4u, 0u}) {
    Fixture fixture(2);
    auto &phi = fixture.BlockAt(1).AppendNewInst(ValueOpcode::Phi);
    phi.SetFlags(Type::U32);
    const auto entry = UserData(fixture, 0);
    const auto carried =
        fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(step)}, 0, 1);
    phi.AddPhiOperand(&fixture.BlockAt(0), entry);
    phi.AddPhiOperand(&fixture.BlockAt(1), carried);
    PushDescriptor(fixture, {Value(&phi), Value(0u), Value(16u), Value(0u)});
    const std::array<uint32_t, 1> user_data{0x12345678u};
    SrtWalker walker(fixture.program, {.user_data = user_data});
    DescriptorValue descriptor;
    const bool evaluated = walker.EvaluateDescriptor(0, descriptor);
    if (step != 0u) {
      Check(!evaluated && walker.Refusal() == SrtRefusal::VaryingPhi,
            "an advancing descriptor phi was evaluated to its entry value");
    } else {
      Check(evaluated && descriptor.dwords[0] == user_data[0],
            "a loop phi every iteration leaves unchanged was refused");
    }
  }

  // Nothing enters the web, or two different values do: no host binding stands for either.
  for (const bool merge : {false, true}) {
    Fixture fixture(3);
    auto &phi = fixture.BlockAt(2).AppendNewInst(ValueOpcode::Phi);
    phi.SetFlags(Type::U32);
    const auto carried =
        fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(4u)}, 0, 2);
    if (merge) {
      phi.AddPhiOperand(&fixture.BlockAt(0), UserData(fixture, 0));
      phi.AddPhiOperand(&fixture.BlockAt(1),
                        fixture.Emit(ValueOpcode::BitwiseAnd32,
                                     {UserData(fixture, 1), Value(0xffu)}, 0, 1));
    } else {
      phi.AddPhiOperand(&fixture.BlockAt(0),
                        fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(8u)}));
    }
    phi.AddPhiOperand(&fixture.BlockAt(2), carried);
    PushDescriptor(fixture, {Value(&phi), Value(0u), Value(16u), Value(0u)});
    Check(!ValidateRuntimeValue(fixture.program, Value(&phi)),
          "a loop phi without one entry value was accepted");
    const std::array<uint32_t, 2> user_data{1u, 2u};
    SrtWalker walker(fixture.program, {.user_data = user_data});
    DescriptorValue descriptor;
    Check(!walker.EvaluateDescriptor(0, descriptor) &&
              walker.Refusal() == (merge ? SrtRefusal::PhiMerge : SrtRefusal::PhiNoEntry),
          "a loop phi without one entry value was not named as such");
  }
}

// A cached cyclic-phi answer equals the walk's, on the first lookup and on every hit.
void TestCyclicPhiEntryCacheAnswersLikeTheWalk() {
  Fixture fixture;
  auto &block = fixture.BlockAt();
  auto &entry = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(2))});
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

  const auto &program = fixture.program;
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

// A frozen plan's cached phi resolution must accept, refuse and carry as the walk does.
void TestFrozenPhiPlansWalkLikeTheProgram() {
  for (const uint32_t stride : {0u, 1u}) {
    Fixture fixture;
    auto &block = fixture.BlockAt();
    const auto entry = UserData(fixture, 0);
    auto &loop = block.AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    auto &step = block.AppendNewInst(ValueOpcode::IAdd32, {Value(&loop), Value(stride)});
    loop.AddPhiOperand(&block, entry);
    loop.AddPhiOperand(&block, Value(&step));
    PushDescriptor(fixture, {Value(&loop), Value(0u), Value(4u), Value(0u)});
    auto plan = ExtractResourcePlan(fixture.program);
    plan.frozen_values = true;
    for (const uint32_t value : {0x10u, 0x20u, 0x10u}) {
      const std::array<uint32_t, 1> user_data{value};
      DescriptorValue frozen;
      DescriptorValue walked;
      const bool frozen_ok = SrtWalker(plan, {.user_data = user_data}).EvaluateDescriptor(0, frozen);
      const bool walked_ok =
          SrtWalker(fixture.program, {.user_data = user_data}).EvaluateDescriptor(0, walked);
      Check(frozen_ok == walked_ok && frozen_ok == (stride == 0u),
            "a frozen phi plan accepted or refused a loop the walk did not");
      Check(!frozen_ok || (frozen.dwords == walked.dwords && frozen.dwords[0] == value),
            "a frozen phi plan carried another value than the loop does");
    }
    Check(plan.phi_plans.size() == 1u, "a phi was resolved more than once on a frozen plan");
  }
}

// Each layer of `c = c | c` over a refused loop phi doubled the walk while refusals were retried.
void TestBallotDescriptorDword() {
  const auto build = [](Fixture &fixture, bool lane_predicate, uint32_t wave_size) {
    fixture.program.wave_size = wave_size;
    const auto predicate =
        lane_predicate
            ? fixture.Emit(ValueOpcode::ULessThan32, {fixture.Emit(ValueOpcode::LaneId), Value(3u)})
            : fixture.Emit(ValueOpcode::ULessThan32, {UserData(fixture, 0), Value(0x100u)});
    const auto ballot = fixture.Emit(ValueOpcode::Ballot, {predicate});
    const auto low = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(0u)});
    const auto high = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(1u)});
    const auto spare = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(3u)});
    const auto base = UserData(fixture, 0);
    const auto records = fixture.Emit(ValueOpcode::IAdd32, {UserData(fixture, 1), spare});
    const auto source = PushDescriptor(fixture, {base, records, low, high});
    for (uint32_t dword = 0; dword < 4; dword++) {
      Check(ValidateRuntimeValue(fixture.program,
                                 fixture.program.descriptor_sources[source].dwords[dword]),
            "a ballot descriptor dword was rejected");
    }
    return source;
  };
  const auto evaluate = [](const Fixture &fixture, uint32_t source, uint32_t word0,
                           DescriptorValue &descriptor) {
    const std::array<uint32_t, 2> user_data{word0, 64u};
    return EvaluateWithClean(fixture.program, source, {.user_data = user_data}, descriptor);
  };
  DescriptorValue descriptor;
  {
    Fixture uniform;
    const auto source = build(uniform, false, 64u);
    Check(evaluate(uniform, source, 0x20u, descriptor) && descriptor.dwords[0] == 0x20u &&
              descriptor.dwords[1] == 64u && descriptor.dwords[2] == 0xffffffffu &&
              descriptor.dwords[3] == 0xffffffffu,
          "a ballot true on every lane did not fill both mask words");
    Check(evaluate(uniform, source, 0x200u, descriptor) && descriptor.dwords[0] == 0x200u &&
              descriptor.dwords[2] == 0u && descriptor.dwords[3] == 0u,
          "a ballot false on every lane did not clear both mask words");
  }
  {
    Fixture narrow;
    const auto source = build(narrow, false, 32u);
    Check(evaluate(narrow, source, 0x20u, descriptor) && descriptor.dwords[0] == 0x20u &&
              descriptor.dwords[2] == 0xffffffffu && descriptor.dwords[3] == 0u,
          "a wave32 ballot set lanes the wave does not have");
  }
  {
    Fixture lanes;
    const auto source = build(lanes, true, 64u);
    Check(evaluate(lanes, source, 0x1234u, descriptor) && descriptor.dwords[0] == 0x1234u &&
              descriptor.dwords[1] == 64u && descriptor.dwords[2] == 0x7u &&
              descriptor.dwords[3] == 0u,
          "a lane-indexed ballot did not re-execute its predicate per lane");
  }
}

// Only a ballot re-executes its predicate per lane; a readfirstlane lane index is the tracker's.
void TestLaneIndexOutsideBallot() {
  for (const bool lift : {true, false}) {
    Fixture fixture;
    const auto selected = fixture.Emit(
        ValueOpcode::SelectU32,
        {ThreadBit(fixture, Value(0xffffffffu), Value(0xffffffffu)), Value(0x400u), Value(0x800u)});
    const auto records =
        lift ? fixture.Emit(ValueOpcode::ReadFirstLane, {selected, Value(true)}) : selected;
    const auto source = PushDescriptor(fixture, {UserData(fixture, 0), UserData(fixture, 1),
                                                 records, UserData(fixture, 3)});
    Check(!ValidateRuntimeValue(fixture.program, records),
          "a lane index outside a ballot was accepted as a host value");
    const std::array<uint32_t, 4> user_data{0x2000u, 0u, 0u, 0u};
    DescriptorValue descriptor;
    SrtRefusal refusal = SrtRefusal::None;
    Check(!EvaluateWithClean(fixture.program, source, {.user_data = user_data}, descriptor,
                             &refusal) &&
              refusal == SrtRefusal::VaryingLane,
          "a lane index outside a ballot evaluated on the host");
  }
}

void TestBallotLaneSweepForgetsLaneValues() {
  Fixture fixture;
  fixture.program.wave_size = 64u;
  const auto memory = fixture.AddMemory(ResourceKind::ScalarAddress);
  const auto loaded =
      RawRead(fixture, Address(fixture, UserData(fixture, 0), UserData(fixture, 1)), Value(0u),
              memory);
  const auto shared = fixture.Emit(ValueOpcode::IAdd32, {loaded, UserData(fixture, 2)});
  const auto lane_sum =
      fixture.Emit(ValueOpcode::IAdd32, {fixture.Emit(ValueOpcode::LaneId), shared});
  const auto odd = fixture.Emit(ValueOpcode::BitwiseAnd32, {lane_sum, Value(1u)});
  const auto half = fixture.Emit(ValueOpcode::ShiftRightLogical32, {lane_sum, Value(1u)});
  const auto mixed = fixture.Emit(
      ValueOpcode::IAdd32, {half, fixture.Emit(ValueOpcode::IMul32, {odd, Value(100u)})});
  const auto selected = fixture.Emit(
      ValueOpcode::SelectU32,
      {fixture.Emit(ValueOpcode::IEqual32, {UserData(fixture, 5), Value(0u)}), mixed, shared});
  const auto predicate = fixture.Emit(
      ValueOpcode::LogicalNot,
      {fixture.Emit(ValueOpcode::UGreaterThanEqual32, {selected, UserData(fixture, 3)})});
  const auto ballot = fixture.Emit(ValueOpcode::Ballot, {predicate});
  const auto low = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(0u)});
  const auto high = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(1u)});
  const auto source = PushDescriptor(fixture, {UserData(fixture, 4), Value(64u), low, high});

  const auto expected = [](uint32_t word, uint32_t addend, uint32_t limit) {
    uint64_t mask = 0;
    for (uint32_t lane = 0; lane < 64u; lane++) {
      const uint32_t sum = lane + word + addend;
      const uint32_t chosen = (sum >> 1u) + (sum & 1u) * 100u;
      if (chosen < limit) mask |= uint64_t{1} << lane;
    }
    return mask;
  };
  const auto check = [&](uint32_t word, uint32_t addend, uint32_t limit) {
    TestMemory memory_image{{{0x1000u, word}}};
    const std::array<uint32_t, 6> user_data{0x1000u, 0u, addend, limit, 0x5000u, 0u};
    const SrtRuntime runtime{.user_data = user_data,
                             .read_memory = ReadMemory,
                             .userdata = &memory_image,
                             .read_specialization_memory = ReadMemory};
    DescriptorValue descriptor;
    const auto mask = expected(word, addend, limit);
    Check(EvaluateWithClean(fixture.program, source, runtime, descriptor) &&
              descriptor.dwords[0] == 0x5000u &&
              descriptor.dwords[2] == static_cast<uint32_t>(mask) &&
              descriptor.dwords[3] == static_cast<uint32_t>(mask >> 32u),
          "a ballot sweep reused a lane-dependent value in a later lane");
    return memory_image.reads;
  };
  Check(expected(6u, 4u, 20u) != ~uint64_t{0} && expected(6u, 4u, 20u) != 0u,
        "lane sweep fixture does not separate the lanes");
  Check(check(6u, 4u, 20u) < 64u, "a ballot sweep re-read a lane-free value on every lane");
  check(7u, 0u, 9u);
  check(0u, 0u, 64u);
}

void TestClosedBallotCache() {
  const auto build = [](Fixture &fixture, bool reads_user_data) {
    fixture.program.wave_size = 64u;
    const auto lane = fixture.Emit(ValueOpcode::LaneId);
    const auto exec = fixture.Emit(ValueOpcode::Ballot, {Value(true)});
    const auto active = fixture.Emit(
        ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::BitCount32,
                      {fixture.Emit(ValueOpcode::CompositeExtractU32x4, {exec, Value(0u)})}),
         fixture.Emit(ValueOpcode::BitCount32,
                      {fixture.Emit(ValueOpcode::CompositeExtractU32x4, {exec, Value(1u)})})});
    const auto limit =
        reads_user_data ? fixture.Emit(ValueOpcode::UMin32, {UserData(fixture, 1), active})
                        : fixture.Emit(ValueOpcode::ShiftRightLogical32, {active, Value(2u)});
    const auto rank = fixture.Emit(
        ValueOpcode::SelectU32, {fixture.Emit(ValueOpcode::ULessThan32, {lane, Value(32u)}), lane,
                                 fixture.Emit(ValueOpcode::ISub32, {lane, Value(1u)})});
    const auto ballot =
        fixture.Emit(ValueOpcode::Ballot, {fixture.Emit(ValueOpcode::ULessThan32, {rank, limit})});
    const auto low = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(0u)});
    const auto high = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(1u)});
    PushDescriptor(fixture, {UserData(fixture, 0), Value(64u), low, high});
  };
  const auto expected = [](uint32_t limit) {
    uint64_t mask = 0;
    for (uint32_t lane = 0; lane < 64u; lane++) {
      if ((lane < 32u ? lane : lane - 1u) < limit) mask |= uint64_t{1} << lane;
    }
    return mask;
  };
  const auto walk = [](const ResourcePlan &plan, uint32_t limit, bool with_clean,
                       DescriptorValue &descriptor) {
    const std::array<uint32_t, 2> user_data{0x3000u, limit};
    const SrtRuntime runtime{.user_data = user_data};
    if (with_clean) return EvaluateWithClean(plan, 0, runtime, descriptor);
    return SrtWalker(plan, runtime).EvaluateDescriptor(0, descriptor);
  };
  const auto matches = [](const DescriptorValue &descriptor, uint64_t mask) {
    return descriptor.dwords[0] == 0x3000u &&
           descriptor.dwords[2] == static_cast<uint32_t>(mask) &&
           descriptor.dwords[3] == static_cast<uint32_t>(mask >> 32u);
  };
  const auto known = [](const ResourcePlan &plan) {
    return std::ranges::count_if(plan.closed_ballots, [](const auto &entry) {
      return entry.state == ResourcePlan::ClosedBallot::Known;
    });
  };
  DescriptorValue descriptor;

  Fixture closed;
  build(closed, false);
  auto plan = ExtractResourcePlan(closed.program);
  plan.frozen_values = true;
  for (int pass = 0; pass < 3; pass++) {
    Check(walk(plan, 0u, pass == 1, descriptor) && matches(descriptor, expected(16u)),
          "a closed ballot did not keep its per-lane mask");
  }
  Check(known(plan) == 2, "a closed ballot and the exec ballot inside it were not both kept");
  Check(walk(closed.program, 0u, false, descriptor) && matches(descriptor, expected(16u)) &&
            known(closed.program) == 0,
        "a Program kept a closed ballot");

  Fixture open;
  build(open, true);
  auto open_plan = ExtractResourcePlan(open.program);
  open_plan.frozen_values = true;
  for (const uint32_t limit : {5u, 40u, 0u, 64u, 5u}) {
    Check(walk(open_plan, limit, false, descriptor) && matches(descriptor, expected(limit)),
          "a ballot over user data was served a mask from another draw");
  }
  Check(known(open_plan) == 1, "only the exec ballot of a user-data predicate is closed");
}

// A DWORDX4 vector load of a V# stored in a buffer, split into its four descriptor dwords.
void TestUniformDwordX4DescriptorLoad() {
  struct Built {
    uint32_t source;
    Value word0;
  };
  const auto build = [](Fixture &fixture, uint32_t table_bytes, bool indexed) {
    const auto table = fixture.Emit(ValueOpcode::GetBufferResource,
                                    {UserData(fixture, 0), UserData(fixture, 1),
                                     Value(table_bytes), Value(4u << 12u)});
    MemoryInfo wide;
    wide.kind = ResourceKind::Buffer;
    wide.offset = 8;
    wide.data_dwords = 4;
    wide.component_count = 4;
    wide.idxen = indexed;
    fixture.program.memory_info.push_back(wide);
    const auto loaded = fixture.EmitMemory(
        ValueOpcode::LoadBufferU32x4,
        {table, indexed ? UserData(fixture, 3) : Value(0u), Value(0u), UserData(fixture, 2),
         Value(true)},
        static_cast<uint32_t>(fixture.program.memory_info.size() - 1u));
    std::array<Value, 4> words;
    for (uint32_t index = 0; index < words.size(); index++) {
      words[index] = fixture.Emit(ValueOpcode::CompositeExtractU32x4, {loaded, Value(index)});
    }
    return Built{PushDescriptor(fixture, words), words[0]};
  };
  const std::array<uint32_t, 4> user_data{0x1000u, 0u, 4u, 0u};
  TestMemory memory_image{{{0x100cu, 0xa0a0a0a0u},
                           {0x1010u, 0xa1a1a1a1u},
                           {0x1014u, 0xa2a2a2a2u},
                           {0x1018u, 0xa3a3a3a3u}}};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = ReadMemory,
                           .userdata = &memory_image,
                           .read_specialization_memory = ReadMemory,
                           .zero_read = [](void *userdata) {
                             // Counts zero reads in word 0, which no read address uses.
                             static_cast<TestMemory *>(userdata)->words[0] += 1u;
                           }};
  DescriptorValue descriptor;
  {
    Fixture fixture;
    const auto built = build(fixture, 64u, false);
    Check(ValidateRuntimeValue(fixture.program, built.word0, RuntimeValueType::Integer),
          "a uniform DWORDX4 descriptor load was rejected");
    Check(EvaluateWithClean(fixture.program, built.source, runtime, descriptor) &&
              descriptor.dwords[0] == 0xa0a0a0a0u && descriptor.dwords[1] == 0xa1a1a1a1u &&
              descriptor.dwords[2] == 0xa2a2a2a2u && descriptor.dwords[3] == 0xa3a3a3a3u,
          "DWORDX4 descriptor components were not read at base + soffset + imm + 4k");
  }
  {
    // Sixteen bytes hold only the first component; the rest lie outside the V# and read zero.
    Fixture fixture;
    const auto built = build(fixture, 16u, false);
    memory_image.words[0] = 0u;
    Check(EvaluateWithClean(fixture.program, built.source, runtime, descriptor) &&
              descriptor.dwords[0] == 0xa0a0a0a0u && descriptor.dwords[1] == 0u &&
              descriptor.dwords[2] == 0u && descriptor.dwords[3] == 0u &&
              memory_image.words[0] == 3u,
          "DWORDX4 components past the table did not read zero through the zero-read hook");
  }
  {
    // An indexed load names a record per lane: no host descriptor stands for it.
    Fixture fixture;
    const auto built = build(fixture, 64u, true);
    Check(!ValidateRuntimeValue(fixture.program, built.word0),
          "an indexed DWORDX4 load reached a host descriptor");
  }
}

// Flat refresh hooks see a read's slot and raw operands; a GPU-owned data slot reads 0 and fails.
void TestFlatRefreshReadHooks() {
  Fixture fixture;
  const auto memory = fixture.AddMemory(ResourceKind::ScalarAddress, 0x20);
  const auto first = RawRead(fixture, Address(fixture, Value(0x1000u), Value(0u)), Value(0u), memory);
  const auto second = RawRead(fixture, Address(fixture, Value(0x2000u), Value(0u)), Value(0u), memory);
  LoadBuffer(fixture, {first, second, Value(16u), Value(0u)});
  fixture.Plan();
  Check(fixture.program.srt_reads.size() == 2, "the fixture did not plan two flat slots");
  auto plan = ExtractResourcePlan(fixture.program);

  struct Observed {
    TestMemory memory{{{0x1020u, 0x11u}, {0x2020u, 0x22u}}};
    std::vector<std::pair<uint32_t, uint64_t>> slots;
    bool events = true;
    uint64_t owned = 0;
  } observed;
  const SrtRuntime runtime{
      .read_memory =
          [](void *userdata, uint64_t address, std::span<uint32_t> values) {
            auto &observed = *static_cast<Observed *>(userdata);
            const auto *event = CurrentRawReadEvent();
            observed.events &= event != nullptr && !event->zero &&
                               ((event->operands[0] & ~uint64_t{0xfff}) | 0x20u) == address;
            observed.slots.emplace_back(CurrentSrtReadSlot(), address);
            return ReadMemory(&observed.memory, address, values);
          },
      .userdata = &observed,
      .gpu_owned = [](void *userdata, uint64_t address, uint64_t) {
        return static_cast<Observed *>(userdata)->owned == address;
      }};
  std::vector<uint32_t> flat;
  Check(SrtWalker(plan, runtime).RefreshFlatBuffer(flat) &&
            flat == std::vector<uint32_t>{0x11u, 0x22u} && observed.events &&
            observed.slots == std::vector<std::pair<uint32_t, uint64_t>>{{0, 0x1020u}, {1, 0x2020u}} &&
            CurrentSrtReadSlot() == UINT32_MAX && CurrentRawReadEvent() == nullptr,
        "a flat refresh did not publish its slot and raw-read operands to the reader");

  plan.data_flat_slots.assign(2, 0u);
  plan.data_flat_slots[1] = 1u;
  observed.owned = 0x2020u;
  observed.slots.clear();
  std::vector<uint32_t> owned_slots;
  SrtWalker walker(plan, runtime);
  Check(!walker.RefreshFlatBuffer(flat, true, nullptr, &owned_slots) &&
            owned_slots == std::vector<uint32_t>{1u} && flat[0] == 0x11u && flat[1] == 0u &&
            walker.Refusal() == SrtRefusal::GpuOwned,
        "a GPU-owned data slot was read on the host or not reported");
  Check(walker.RefreshFlatBuffer(flat, true, nullptr, &owned_slots) == false &&
            owned_slots.size() == 2u,
        "a GPU-owned refusal was remembered instead of asked again");
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  std::vector<uint32_t> refused;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization, &refused) &&
            refused == std::vector<uint32_t>{0x80u},
        "a GPU-owned data slot did not name its read for the rebuild");
  observed.owned = 0x1020u;
  Check(SrtWalker(plan, runtime).RefreshFlatBuffer(flat),
        "a GPU-owned address outside the data slots refused the refresh");
}

// The SRT walk evaluates the scalar ALU a descriptor chain is built from, signed and float alike.
void TestWalkerOpcodes() {
  Fixture fixture;
  const auto u0 = UserData(fixture, 0);
  const auto u1 = UserData(fixture, 1);
  const auto f = [&](ValueOpcode op, std::initializer_list<Value> args) {
    return fixture.Emit(op, args);
  };
  const std::array<std::pair<Value, uint32_t>, 15> cases{{
      {f(ValueOpcode::SMin32, {u0, u1}), 0xfffffff0u},
      {f(ValueOpcode::SMax32, {u0, u1}), 7u},
      {f(ValueOpcode::SMulHi, {u0, u1}), 0xffffffffu},
      {f(ValueOpcode::SLessThan32, {u0, u1}), 1u},
      {f(ValueOpcode::UGreaterThanEqual32, {u0, u1}), 1u},
      {f(ValueOpcode::BitCount32, {u1}), 3u},
      {f(ValueOpcode::FindUMsb32, {u0}), 31u},
      {f(ValueOpcode::FindILsb32, {u0}), 4u},
      {f(ValueOpcode::BitReverse32, {u1}), 0xe0000000u},
      {f(ValueOpcode::CompositeExtractU64,
         {f(ValueOpcode::WqmU64, {f(ValueOpcode::CompositeConstructU64, {u1, Value(0x10u)})}),
          Value(1u)}),
       0xf0u},
      {f(ValueOpcode::ConvertU32F32,
         {f(ValueOpcode::FPFloor32, {f(ValueOpcode::FPAdd32, {Value::F32(2.5f), Value::F32(1.25f)})})}),
       3u},
      {f(ValueOpcode::FPOrdLessThan32, {Value::F32(1.0f), Value::F32(2.0f)}), 1u},
      {f(ValueOpcode::ConvertU32F32,
         {f(ValueOpcode::FPMedTri32, {Value::F32(1.0f), Value::F32(9.0f), Value::F32(4.0f)})}),
       4u},
      {f(ValueOpcode::ULessThan64, {f(ValueOpcode::CompositeConstructU64, {u0, Value(0u)}),
                                    f(ValueOpcode::CompositeConstructU64, {u1, Value(1u)})}),
       1u},
      {f(ValueOpcode::DispatchThreadInRange, {}), 1u},
  }};
  const std::array<uint32_t, 2> user_data{0xfffffff0u, 7u};
  SrtWalker walker(fixture.program, {.user_data = user_data});
  for (const auto &[value, expected] : cases) {
    uint32_t result = 0;
    Check(ValidateRuntimeValue(fixture.program, value) && walker.Evaluate(value, result) &&
              result == expected,
          "the SRT walk evaluated a scalar opcode incorrectly");
  }
  uint32_t result = 0;
  const auto unknown = f(ValueOpcode::IAbs32, {u0});
  Check(!walker.Evaluate(unknown, result) && walker.Refusal() == SrtRefusal::UnsupportedOpcode,
        "an opcode the walk has no rule for was not named as unsupported");
  Check(SrtRefusalName(SrtRefusal::UnsupportedOpcode) == "unsupported opcode",
        "a refusal was not named");
}

} // namespace

namespace Common {

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

int DbgExitHandler(const char *, int, std::string_view text) {
  throw std::runtime_error(std::string(text));
}

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view text) {
  throw std::runtime_error(std::string(text));
}

void DbgExit(int) { std::abort(); }

} // namespace Common

int main() {
  try {
    TestImmediateFlatteningAndGvn();
    TestRawScalarComponentAlignment();
    TestScalarMemoryDomainMismatchFails();
    TestDynamicReadRemainsTyped();
    TestNestedSrtWalk();
    TestShaderBaseAndUserData();
    TestCarryAndBitFields();
    TestInvariantAndDivergentPhi();
    TestOperandMutationLifecycle();
    TestControlDependentStandaloneLoadStaysTyped();
    TestRuntime64BitDescriptorOps();
    TestUniformFirstLaneSamplerLod();
    TestFloatComparisonDescriptorInputs();
    TestSharedIntegerRuntimeDependencies();
    TestConstantBufferBounds();
    TestReadLaneElimination();
    TestCompleteUMinReduction();
    TestOptimizationPipeline();
    TestControlFlowValueSurvivesReadLaneFolding();
    TestDeadPhiCyclesAndPlanningRoots();
    TestUndefinedRuntimeValueFails();
    TestDeepUniformValueChain();
    TestAdvancingLoopPhi();
    TestCyclicPhiEntryCacheAnswersLikeTheWalk();
    TestFrozenPhiPlansWalkLikeTheProgram();
    TestBallotDescriptorDword();
    TestLaneIndexOutsideBallot();
    TestBallotLaneSweepForgetsLaneValues();
    TestClosedBallotCache();
    TestUniformDwordX4DescriptorLoad();
    TestFlatRefreshReadHooks();
    TestWalkerOpcodes();
    std::cout << "TypedValuePlanningTests: all cases passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "TypedValuePlanningTests: failed: " << e.what() << '\n';
    return 1;
  }
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "../src/graphics/shader/recompiler/ir/Block.cpp"
#include "../src/graphics/shader/recompiler/ir/Program.cpp"
#include "../src/graphics/shader/recompiler/ir/Type.cpp"
#include "../src/graphics/shader/recompiler/ir/Value.cpp"
#include "../src/graphics/shader/recompiler/ir/passes/ResourceTracking.cpp"
#include "../src/graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
#include "../src/graphics/shader/recompiler/ir/passes/ConstantPropagation.cpp"
#include "../src/graphics/shader/recompiler/ir/passes/DeadCodeElimination.cpp"
