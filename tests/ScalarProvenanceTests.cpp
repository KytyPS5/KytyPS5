#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/ReadLaneElimination.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "SrtWalkerTestMemory.h"

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

// Evaluates the DWORDs of a descriptor source that no resource consumes.
bool EvaluateSource(ResourcePlan &plan, const SrtRuntime &runtime, uint32_t source,
                    DescriptorValue &result) {
  const auto &words = plan.descriptor_sources[source];
  result = {.dword_count = words.dword_count};
  for (uint32_t word = 0; word < words.dword_count; ++word)
    if (!EvaluateSrtValue(plan, runtime, words.dwords[word], result.dwords[word])) return false;
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
  FakeGuestMemory fake(ReadMemory, &memory_image);
  std::vector<uint32_t> flat;
  Check(WalkSrt(ExtractResourcePlan(fixture.program), {}, flat, nullptr), "flattened SRT walk failed");
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
  FakeGuestMemory fake(ReadMemory, &memory_image);
  std::vector<uint32_t> flat;
  Check(WalkSrt(ExtractResourcePlan(fixture.program), {}, flat, nullptr), "raw scalar SRT walk failed");
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
  FakeGuestMemory fake(ReadMemory, &memory_image);
  std::vector<uint32_t> flat;
  Check(WalkSrt(ExtractResourcePlan(fixture.program), {}, flat, nullptr), "nested SRT walk failed");
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
  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(EvaluateSource(plan, runtime, 0, result),
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

  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(EvaluateSource(plan, {}, 0, result),
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

  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(EvaluateSource(plan, {}, 0, result) &&
            result.dwords[0] == 7u,
        "loop-invariant typed phi was rejected");
  Check(!EvaluateSource(plan, {}, 1, result),
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
            EvaluateSrtValue(fixture.program, {}, inserted, result) && result == 0x3au,
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
            EvaluateSrtValue(fixture.program, {}, sum, result) && result == 11,
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

  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(EvaluateSource(plan, {}, 0, result) &&
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
  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(EvaluateSource(plan, runtime, 0, result) &&
            result.dwords[0] == 0x00300300u,
        "uniform sampler LOD evaluated incorrectly");
  user_data[6] = 20u;
  Check(EvaluateSource(plan, runtime, 0, result) &&
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

    auto plan = ExtractResourcePlan(fixture.program);
    for (size_t index = 0; index < inputs.size(); index++) {
      const auto &input = inputs[index];
      const std::array user_data{input.bits};
      DescriptorValue result;
      Check(EvaluateSource(plan, {.user_data = user_data}, 0, result) &&
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
  // PS5 scalar-buffer loads return zero per out-of-range DWORD without an access;
  // an unreadable in-range address is accessed and reads as zero too.
  for (const auto &test : {Case{12u, 0u, 0u, 16u, 0xa5a5a5a5u, 1u},
                           Case{3u, 1u, 0u, 16u, 0x12345678u, 1u},
                           Case{0xfffffffcu, 4u, 0u, 16u, 0u, 0u},
                           Case{16u, 0u, 0u, 16u, 0u, 0u},
                           Case{12u, 0u, 0u, 14u, 0u, 0u},
                           Case{0u, 0u, 0u, 0u, 0u, 0u},
                           Case{8u, 0u, 0u, 16u, 0u, 1u},
                           Case{12u, 0u, 48u, 32768u, 0xa5a5a5a5u, 1u},
                           Case{0x6339c0u, 16u, 48u, 32768u, 0u, 0u}}) {
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
    FakeGuestMemory fake(read_memory, &memory_image);
    std::vector<uint32_t> flat;
    Check(WalkSrt(ExtractResourcePlan(fixture.program), {}, flat, nullptr),
          "constant-buffer walk misaligned or wrapped its offset components");
    Check(flat == std::vector<uint32_t>{test.expected} && memory_image.attempts == test.reads,
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
  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue result;
  Check(!EvaluateSource(plan, {}, 0, result),
        "undefined typed descriptor source was accepted");
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
