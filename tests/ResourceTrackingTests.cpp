#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderComputeInputInfo;
using Libs::Graphics::ShaderType;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;

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
  Block *block = nullptr;

  explicit Fixture(ShaderType stage = ShaderType::Compute) {
    program.stage = stage;
    program.user_data_count = 64;
    block = AddBlock();
  }

  Block *AddBlock() {
    auto storage = std::make_unique<Block>();
    auto *result = storage.get();
    program.block_storage.push_back(std::move(storage));
    program.blocks.push_back(result);
    program.blocks.back()->id =
        static_cast<uint32_t>(program.blocks.size() - 1u);
    return result;
  }

  Value Emit(ValueOpcode opcode, std::initializer_list<Value> args = {},
             uint64_t flags = 0, Block *destination = nullptr) {
    if (NumArgsOf(opcode) != std::numeric_limits<size_t>::max() &&
        NumArgsOf(opcode) != args.size()) {
      throw std::runtime_error(std::string(ValueOpcodeName(opcode)) +
                               " argument count");
    }
    auto &inst = (destination != nullptr ? destination : block)
                     ->AppendNewInst(opcode, args, flags);
    return Value(&inst);
  }

  template <typename T>
  Value Emit(ValueOpcode opcode, std::initializer_list<Value> args, T flags,
             Block *destination = nullptr) {
    uint64_t bits = 0;
    std::memcpy(&bits, &flags, sizeof(flags));
    return Emit(opcode, args, bits, destination);
  }

  Value UserData(uint32_t index) {
    return Emit(ValueOpcode::GetUserData,
                {Value(static_cast<ScalarReg>(index))});
  }

  MemoryFlags AddMemory(MemoryInfo memory, uint32_t pc) {
    const auto index = static_cast<uint32_t>(program.memory_info.size());
    program.memory_info.push_back(memory);
    return {index, pc};
  }

  Value Buffer(std::array<Value, 4> dwords, uint32_t pc = 0) {
    return Emit(ValueOpcode::GetBufferResource,
                {dwords[0], dwords[1], dwords[2], dwords[3]},
                MemoryFlags{0, pc});
  }

  Value Address(Value low, Value high, uint32_t pc = 0) {
    return Emit(ValueOpcode::GetAddressResource, {low, high},
                MemoryFlags{0, pc});
  }

  Value Image(std::array<Value, 8> dwords, uint32_t pc = 0) {
    return Emit(ValueOpcode::GetImageResource,
                {dwords[0], dwords[1], dwords[2], dwords[3], dwords[4],
                 dwords[5], dwords[6], dwords[7]},
                MemoryFlags{0, pc});
  }

  Value Sampler(std::array<Value, 4> dwords, uint32_t pc = 0) {
    return Emit(ValueOpcode::GetSamplerResource,
                {dwords[0], dwords[1], dwords[2], dwords[3]},
                MemoryFlags{0, pc});
  }

  Value ImageAddress() {
    return Emit(ValueOpcode::MakeImageAddress,
                {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
                 Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
                 Value(0u), Value(0u), Value(0u)});
  }

  void PlanAndTrack() {
    for (size_t index = 0; index < program.blocks.size(); ++index) {
      const auto condition = program.blocks[index]->condition;
      if (!condition.IsEmpty())
        Emit(ValueOpcode::Reference, {condition}, 0, program.blocks[index]);
    }
    for (auto *target : program.blocks) {
      for (auto &inst : *target) {
        if (inst.HasUses() || inst.MayHaveSideEffects() ||
            (BufferAccessOf(inst.GetOpcode()) == BufferAccess::None &&
             AddressOpcodeInfoOf(inst.GetOpcode()).access == AddressAccess::None &&
             ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None))
          continue;
        auto value = Value(&inst);
        if (value.GetType() == Type::U32x4)
          value = Emit(ValueOpcode::CompositeExtractU32x4, {value, Value(0u)}, 0, target);
        if (value.GetType() == Type::U8)
          value = Emit(ValueOpcode::ConvertU32U8, {value}, 0, target);
        Check(value.GetType() == Type::U32, "unhandled memory result type in fixture");
        Emit(ValueOpcode::ReferenceU32, {value}, 0, target);
      }
    }
    TrackResources(program, {}, {});
  }
};

struct TestMemory {
  uint64_t base = 0x1000;
  std::array<uint32_t, 8> words{};
  uint32_t reads = 0;
  uint32_t fail_after = UINT32_MAX;
};

bool ReadTestMemory(void *userdata, uint64_t address, std::span<uint32_t> values) {
  auto *memory = static_cast<TestMemory *>(userdata);
  if (memory == nullptr || address < memory->base ||
      values.size_bytes() > memory->words.size() * sizeof(uint32_t) ||
      address - memory->base > memory->words.size() * sizeof(uint32_t) - values.size_bytes() ||
      memory->reads >= memory->fail_after) {
    return false;
  }
  std::copy_n(memory->words.begin() + (address - memory->base) / sizeof(uint32_t),
               values.size(), values.begin());
  memory->reads++;
  return true;
}

struct LinearTestMemory {
  uint64_t base = 0x1000;
  std::vector<uint32_t> words = std::vector<uint32_t>(0x2200 / 4);
  uint64_t fail_address = UINT64_MAX;
  uint64_t watched_address = UINT64_MAX;
  uint32_t watched_reads = 0;
  size_t watched_dwords = 0;
  uint32_t reads = 0;
  uint32_t descriptor_reads = 0;
};

bool ReadLinearTestMemory(void *userdata, uint64_t address, std::span<uint32_t> values) {
  auto *memory = static_cast<LinearTestMemory *>(userdata);
  if (memory == nullptr || address < memory->base ||
      values.size_bytes() > memory->words.size() * sizeof(uint32_t) ||
      address - memory->base > memory->words.size() * sizeof(uint32_t) - values.size_bytes() ||
      (address & 3u) != 0u ||
      (memory->fail_address >= address && memory->fail_address - address < values.size_bytes())) {
    return false;
  }
  std::copy_n(memory->words.begin() + (address - memory->base) / sizeof(uint32_t),
               values.size(), values.begin());
  ++memory->reads;
  if (values.size() == 8u) ++memory->descriptor_reads;
  if (memory->watched_address >= address &&
      memory->watched_address - address < values.size_bytes()) {
    ++memory->watched_reads;
    memory->watched_dwords = values.size();
  }
  return true;
}

std::unique_ptr<Fixture>
MakeIndirectImageFixture(bool malformed, uint32_t table_stride = 32, uint32_t table_offset = 0,
                         uint32_t table_shader_offset = 0, bool nested = false,
                         ValueOpcode op = ValueOpcode::ImageSampleRaw) {
  auto fixture = std::make_unique<Fixture>();
  std::array<Value, 4> material_words;
  std::array<Value, 4> heap_words;
  for (uint32_t dword = 0; dword < 4; dword++) {
    material_words[dword] = fixture->UserData(dword);
    heap_words[dword] = fixture->UserData(dword + 4u);
  }
  const auto material = fixture->Buffer(material_words, 0x10d8);
  const auto heap = fixture->Buffer(heap_words, 0x10d8);
  const auto invocation = fixture->Emit(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)), Value(0u)});
  const auto selector = fixture->Emit(ValueOpcode::ReadFirstLane,
                                      {invocation, Value(true)});
  Value member;
  if (nested) {
    std::array<Value, 4> root_words;
    for (uint32_t word = 0; word < 4u; ++word) root_words[word] = fixture->UserData(12u + word);
    const auto root = fixture->Buffer(root_words);
    MemoryInfo root_memory;
    root_memory.kind = ResourceKind::ScalarBuffer;
    const auto root_offset = fixture->Emit(ValueOpcode::IMul32, {selector, Value(32u)});
    const auto pointer = fixture->Emit(ValueOpcode::ReadConstBuffer, {root, root_offset},
                                       fixture->AddMemory(root_memory, 0x1000));
    const auto masked = fixture->Emit(ValueOpcode::BitwiseAnd32, {pointer, Value(0xfffffff1u)});
    member = fixture->Emit(ValueOpcode::IAdd32, {masked, Value(16u)});
  } else {
    member = fixture->Emit(ValueOpcode::IMul32, {selector, Value(224u)});
  }
  fixture->Emit(ValueOpcode::ReferenceU32, {member});
  MemoryInfo material_scalar;
  material_scalar.kind = ResourceKind::ScalarBuffer;
  material_scalar.offset = 4u;
  const auto key =
      fixture->Emit(ValueOpcode::ReadConstBuffer, {material, member},
                    fixture->AddMemory(material_scalar, 0x10d8));
  auto heap_offset = fixture->Emit(ValueOpcode::IMul32, {key, Value(table_stride)});
  if (table_shader_offset != 0u)
    heap_offset = fixture->Emit(ValueOpcode::IAdd32, {heap_offset, Value(table_shader_offset)});
  std::array<Value, 8> image_words;
  MemoryInfo heap_scalar;
  heap_scalar.kind = ResourceKind::ScalarBuffer;
  for (uint32_t dword = 0; dword < image_words.size(); dword++) {
    auto component = heap_scalar;
    component.offset = table_offset + dword * sizeof(uint32_t);
    if (malformed && dword == image_words.size() - 1u) {
      component.offset += sizeof(uint32_t);
    }
    image_words[dword] =
        fixture->Emit(ValueOpcode::ReadConstBuffer, {heap, heap_offset},
                      fixture->AddMemory(component, 0x10d8));
  }
  if (nested) {
    const auto field = fixture->Emit(ValueOpcode::BitwiseAnd32, {image_words[4], Value(7u)});
    fixture->Emit(ValueOpcode::ReferenceU32, {field});
  }
  const auto image = fixture->Image(image_words, 0x10f0);
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  if (op == ValueOpcode::ImageWrite) {
    const auto texel = fixture->Emit(ValueOpcode::CompositeConstructU32x4,
                                     {Value(0u), Value(0u), Value(0u), Value(0u)});
    fixture->Emit(op, {image, fixture->ImageAddress(), texel, Value(true)},
                  fixture->AddMemory(sample, 0x10f0));
    return fixture;
  }
  Value sampled;
  if (op == ValueOpcode::ImageRead) {
    sampled = fixture->Emit(op, {image, fixture->ImageAddress(), Value(true)},
                            fixture->AddMemory(sample, 0x10f0));
  } else if (op == ValueOpcode::ImageQueryDimensions) {
    sampled = fixture->Emit(op, {image, fixture->ImageAddress()},
                            fixture->AddMemory(sample, 0x10f0));
  } else {
    const auto sampler =
        fixture->Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}, 0x10f0);
    const auto address = fixture->ImageAddress();
    const auto memory = fixture->AddMemory(sample, 0x10f0);
    sampled = op == ValueOpcode::ImageGatherRaw
                  ? fixture->Emit(op, {image, sampler, address, Value(0u), Value(0u), Value(0u),
                                       Value(0u)},
                                  memory)
                  : fixture->Emit(op, {image, sampler, address}, memory);
  }
  const auto sampled_x =
      fixture->Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
  fixture->Emit(ValueOpcode::ReferenceU32, {sampled_x});
  return fixture;
}

void TestInvariantIndirectImageMaterialization() {
  auto fixture = MakeIndirectImageFixture(false, 48u, 0u, 16u, true);
  fixture->PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);
  const auto &source = *plan.descriptor_sources[plan.info.images[0].source].indirect_descriptor;
  Check(!source.selector && fixture->program.info.buffers.size() == 3u,
        "bounded image table retained CPU selector traversal or lost live loads");
  std::array<uint32_t, 8> descriptor{};
  descriptor[0] = 0x20u;
  descriptor[1] = static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
  descriptor[2] = 3u | (3u << 14u);
  descriptor[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  std::array<uint32_t, 16> data{0x3000u, 16u << 16u, 0x200000u, 0u,
                                0x2000u, 48u << 16u, 3u, 0u};
  std::copy_n(data.begin(), 4u, data.begin() + 12u);
  LinearTestMemory memory;
  memory.fail_address = 0x3000u; // GPU-only material backing must never be read on the CPU.
  memory.watched_address = 0x2000u;
  const auto put = [&](uint32_t offset, uint32_t identity) {
    auto value = descriptor;
    value[0] += identity;
    std::copy(value.begin(), value.end(), memory.words.begin() + (0x1000u + offset) / 4u);
  };
  put(16u, 0u);
  put(64u, 1u);
  SrtRuntime runtime{.user_data = data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 3u && memory.watched_reads == 1u &&
            memory.watched_dwords == 36u && memory.reads == 1u,
        "image table was not read once independently of its GPU selector chain");
  const auto root = specialization.images[0];
  const auto mapping = root.indirect_mapping_offset;
  Check(root.indirect_search_iterations == 0u && snapshot.flattened_srt[mapping] == 17u &&
            snapshot.flattened_srt[mapping + 1u] == 0u &&
            snapshot.flattened_srt[mapping + 5u] == 1u &&
            snapshot.flattened_srt[mapping + 17u] == 2u &&
            snapshot.specialization_reads == std::vector<std::pair<uint64_t, uint64_t>>{{0x2000u, 144u}},
        "dense image mapping lost its null slot, trimmed length or coherent read dependency");
  const auto original_specialization = specialization;
  std::fill_n(memory.words.begin() + 0x1040u / 4u, 8u, 0u);
  put(112u, 1u);
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            specialization == original_specialization && memory.watched_reads == 2u &&
            snapshot.flattened_srt[mapping] == 29u,
        "moving a table entry changed shader specialization or reused stale mapping bytes");
  put(112u, 7u);
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images[2].dwords[0] == descriptor[0] + 7u,
        "changed table descriptor was not refreshed");
  // Scalar reads align each DWORD and return zero beyond the V# byte size.
  data[5] = 0u;
  data[6] = 19u;
  std::fill(memory.words.begin() + 0x1000u / 4u, memory.words.end(), 0u);
  put(0u, 0u);
  memory.fail_address = 0x2010u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            memory.watched_dwords == 4u && snapshot.images.size() == 2u &&
            snapshot.images[1].dwords == descriptor,
        "partial descriptor prefix did not zero its out-of-bounds DWORDs");
  memory.fail_address = 0x2008u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "unreadable in-range table bytes were accepted");
  memory.fail_address = UINT64_MAX;
  for (uint32_t size : {0u, 3u, 4u}) {
    data[6] = size;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == 1u &&
              specialization.images[0].indirect_root == ImageResource::NoIndirectImage &&
              std::ranges::all_of(snapshot.images[0].dwords, [](uint32_t word) { return word == 0u; }),
          "empty image table did not specialize as a safe null image");
  }
  // The GPU computes these offsets; host materialization must not enumerate the key.
  for (const auto [shader, immediate, key, expected] :
       {std::array{16u, 0u, 0x55555555u, 0u},
        std::array{0u, 16u, 0x55555555u, UINT32_MAX},
        std::array{0xfffffff0u, 32u, 0u, UINT32_MAX}}) {
    auto wrap = MakeIndirectImageFixture(false, 48u, immediate, shader);
    wrap->PlanAndTrack();
    for (auto &inst : *wrap->block) {
      if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
        inst.ReplaceUsesWith(Value(key));
        break;
      }
    }
    const auto handle = std::ranges::find_if(*wrap->block, [](const Inst &inst) {
      return inst.GetOpcode() == ValueOpcode::GetImageResource;
    });
    SrtWalker walker(wrap->program, runtime);
    uint32_t offset = 0;
    Check(walker.Evaluate(handle->Arg(0), offset) && offset == expected,
          "GPU descriptor offset projection lost U32 wrap or scalar immediate carry");
  }
  auto malformed = MakeIndirectImageFixture(true);
  CheckFatal([&] { malformed->PlanAndTrack(); }, "not a valid runtime value",
             "noncontiguous descriptor DWORDs entered the table plan");
  Check(!malformed->program.resource_tracking_complete && malformed->program.descriptor_sources.empty(),
        "malformed image table tracking was not transactional");
}

void TestBoundedImageViewEligibility() {
  using Type = Libs::Graphics::Prospero::ImageType;
  auto fixture = MakeIndirectImageFixture(false, 48u, 0u, 16u);
  fixture->PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture->program);
  std::array<uint32_t, 8> data{0x3000u, 16u << 16u, 1u, 0u,
                              0x2000u, 48u << 16u, 5u, 0u};
  LinearTestMemory memory;
  memory.fail_address = 0x3000u;
  const auto descriptor = [](uint32_t identity, Type type) {
    return std::array<uint32_t, 8>{identity, 75u << 20u, 3u | (3u << 14u),
        Libs::Graphics::DstSel(4, 5, 6, 7) | (static_cast<uint32_t>(type) << 28u),
        0u, 0u, 0u, 0u};
  };
  const std::array types{Type::kColor2D, Type::kCube, Type::kColor3D,
                         Type::kColor2DArray, Type::kColor2D};
  for (uint32_t row = 0; row < types.size(); ++row) {
    const auto value = descriptor(0x20u + row, types[row]);
    std::copy(value.begin(), value.end(), memory.words.begin() + (0x1010u + row * 48u) / 4u);
  }
  SrtRuntime runtime{.user_data = data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 4u && memory.reads == 1u,
        "unrelated cube/3D heap entries prevented a typed 2D table view");
  const auto mapping = specialization.images[0].indirect_mapping_offset;
  Check(snapshot.flattened_srt[mapping] == 53u &&
            snapshot.flattened_srt[mapping + 5u] == 1u &&
            snapshot.flattened_srt[mapping + 17u] == 0u &&
            snapshot.flattened_srt[mapping + 29u] == 0u &&
            snapshot.flattened_srt[mapping + 41u] == 2u &&
            snapshot.flattened_srt[mapping + 53u] == 3u &&
            snapshot.images[1].dwords == descriptor(0x20u, Type::kColor2D) &&
            snapshot.images[2].dwords == descriptor(0x23u, Type::kColor2DArray) &&
            snapshot.images[3].dwords == descriptor(0x24u, Type::kColor2D),
        "typed table eligibility changed byte offsets or discarded compatible views");

  // An explicitly selected descriptor is not an unrelated entry in a broad heap.
  const auto image_source = plan.info.images[0].source;
  std::vector<uint32_t> selected;
  for (auto type : {Type::kColor2D, Type::kColor3D}) {
    DescriptorSource source;
    source.dword_count = 8u;
    const auto value = descriptor(0x30u, type);
    for (uint32_t word = 0; word < value.size(); ++word) source.dwords[word] = Value(value[word]);
    selected.push_back(static_cast<uint32_t>(plan.descriptor_sources.size()));
    plan.descriptor_sources.push_back(source);
  }
  plan.descriptor_sources[image_source].indirect_descriptor->sources = std::move(selected);
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "explicitly selected incompatible descriptor was silently normalized to null");
}

// Sampled-class accesses select their table candidate on the GPU; storage tables are rejected
// before compilation.
void TestIndirectImageTableOperations() {
  for (const auto op : {ValueOpcode::ImageRead, ValueOpcode::ImageGatherRaw,
                        ValueOpcode::ImageQueryDimensions, ValueOpcode::ImageWrite}) {
    auto fixture = MakeIndirectImageFixture(false, 48u, 0u, 16u, false, op);
    fixture->PlanAndTrack();
    auto plan = ExtractResourcePlan(fixture->program);
    std::array<uint32_t, 8> data{0x3000u, 16u << 16u, 1u, 0u, 0x2000u, 48u << 16u, 3u, 0u};
    LinearTestMemory memory;
    memory.fail_address = 0x3000u;
    for (uint32_t row = 0; row < 3u; ++row) {
      const std::array<uint32_t, 8> descriptor{
          0x20u + row, static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32UInt) << 20u,
          3u | (3u << 14u),
          Libs::Graphics::DstSel(4, 5, 6, 7) |
              (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u),
          0u, 0u, 0u, 0u};
      std::copy(descriptor.begin(), descriptor.end(),
                memory.words.begin() + (0x1010u + row * 48u) / 4u);
    }
    SrtRuntime runtime{.user_data = data, .userdata = &memory,
                       .read_specialization_memory = ReadLinearTestMemory};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const bool materialized = MaterializeResources(plan, runtime, snapshot, specialization);
    if (op == ValueOpcode::ImageWrite) {
      Check(!materialized, "indirect storage image table was accepted");
      continue;
    }
    Check(materialized && snapshot.images.size() == 4u &&
              specialization.images[0].indirect_root == 0u,
          "indirect sampled image access did not materialize its table candidates");
    ApplyResourceSpecialization(fixture->program, specialization);
    Check(fixture->program.info.images[0].indirect_resources.size() == 4u,
          "indirect sampled image access lost its table candidates");
  }
}

void TestWaterfallImageTable() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  const auto make = [](bool equality, bool scalar) {
    auto fixture = std::make_unique<Fixture>();
    auto *entry = fixture->block;
    auto *body = fixture->AddBlock();
    auto *exit = fixture->AddBlock();
    entry->AddBranch(body);
    entry->AddBranch(exit);
    body->AddBranch(exit);
    std::array<Value, 4> words;
    for (uint32_t i = 0; i < 4u; ++i) words[i] = fixture->UserData(i);
    const auto table = fixture->Buffer(words);
    const auto lane_value = fixture->Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)});
    const auto active = fixture->Emit(ValueOpcode::INotEqual32, {lane_value, Value(0u)});
    const auto index = fixture->UserData(4u);
    std::array<Value, 8> image_words;
    auto guard = active;
    for (uint32_t group = 0; group < 2u; ++group) {
      MemoryInfo memory;
      memory.kind = ResourceKind::Buffer;
      memory.idxen = true;
      memory.data_dwords = 4u;
      memory.offset = 16u + group * 16u;
      const auto loaded = fixture->Emit(ValueOpcode::LoadBufferU32x4,
          {table, index, Value(0u), Value(0u), active}, fixture->AddMemory(memory, 0x850u));
      for (uint32_t component = 0; component < 4u; ++component) {
        const auto word = group * 4u + component;
        const auto local = fixture->Emit(ValueOpcode::SelectU32,
            {active, fixture->Emit(ValueOpcode::CompositeExtractU32x4,
                                  {loaded, Value(component)}), Value(0xdeadu)});
        image_words[word] = fixture->Emit(ValueOpcode::ReadLane, {local, Value(0u)});
        if (equality || word != 7u)
          guard = fixture->Emit(ValueOpcode::LogicalAnd,
              {guard, fixture->Emit(ValueOpcode::IEqual32, {image_words[word], local})});
      }
    }
    fixture->program.blocks[0]->condition = guard;
    fixture->program.blocks[0]->terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = fixture->program.blocks[1u],
        .false_block = fixture->program.blocks[2u]};
    const auto sample = [&](Value image, Block *block) {
      const auto sampler = fixture->Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
      MemoryInfo memory;
      memory.kind = ResourceKind::Image;
      memory.image_dimension = Decoder::ImageDimension::Dim2D;
      const auto value = fixture->Emit(ValueOpcode::ImageSampleRaw,
          {image, sampler, fixture->ImageAddress()}, fixture->AddMemory(memory, 0x92cu), block);
      fixture->Emit(ValueOpcode::ReferenceU32,
          {fixture->Emit(ValueOpcode::CompositeExtractU32x4, {value, Value(0u)}, 0, block)}, 0, block);
    };
    if (scalar) {
      const auto key = fixture->Emit(ValueOpcode::ReadFirstLane, {lane_value, active});
      const auto offset = fixture->Emit(ValueOpcode::IAdd32,
          {fixture->Emit(ValueOpcode::IMul32, {key, Value(48u)}), Value(16u)});
      std::array<Value, 8> scalar_words;
      for (uint32_t word = 0; word < 8u; ++word) {
        MemoryInfo memory;
        memory.kind = ResourceKind::ScalarBuffer;
        memory.offset = word * 4u;
        scalar_words[word] = fixture->Emit(ValueOpcode::ReadConstBuffer, {table, offset},
                                           fixture->AddMemory(memory, 0x7bcu));
      }
      sample(fixture->Image(scalar_words), entry);
    }
    const auto image = fixture->Emit(ValueOpcode::GetImageResource,
        {image_words[0], image_words[1], image_words[2], image_words[3],
         image_words[4], image_words[5], image_words[6], image_words[7]}, 0, body);
    sample(image, body);
    return fixture;
  };
  auto fixture = make(true, true);
  fixture->PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture->program);
  Check(plan.info.images.size() == 1u &&
            plan.descriptor_sources[plan.info.images[0].source].indirect_descriptor->table_record_bytes == 48u,
        "scalar and waterfall uses did not share their native image table");
  std::array<uint32_t, 5> data{0x2000u, 48u << 16u, 2u, 0x5204u, 1u};
  LinearTestMemory memory;
  const std::array<uint32_t, 8> descriptor{0x20u, 75u << 20u, 3u | (3u << 14u),
      Libs::Graphics::DstSel(4, 5, 6, 7) | (9u << 28u), 0u, 0u, 0u, 0u};
  std::copy(descriptor.begin(), descriptor.end(), memory.words.begin() + 0x1010u / 4u);
  SrtRuntime runtime{.user_data = data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.reads == 1u &&
            snapshot.images.size() == 2u,
        "shared scalar/waterfall table repeated its coherent bulk read");
  const auto handle = std::ranges::find_if(*fixture->program.blocks[1], [](const Inst &inst) {
    return inst.GetOpcode() == ValueOpcode::GetImageResource;
  });
  for (const auto [index, format, expected] :
       {std::array{1u, 0x5204u, 64u}, std::array{2u, 0x5204u, UINT32_MAX},
        std::array{UINT32_MAX, 0x5204u, UINT32_MAX}, std::array{0u, 0u, UINT32_MAX}}) {
    data[4] = index;
    data[3] = format;
    SrtWalker walker(fixture->program, runtime);
    uint32_t offset = 0;
    Check(walker.Evaluate(handle->Arg(0), offset) && offset == expected,
          "VMEM table projection lost record bounds, format disable or multiplication carry");
  }
  auto disabled = make(true, false);
  disabled->PlanAndTrack();
  const auto disabled_plan = ExtractResourcePlan(disabled->program);
  data[3] = 0u;
  memory.fail_address = 0x2000u;
  Check(MaterializeResources(disabled_plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 1u &&
            specialization.images[0].indirect_root == ImageResource::NoIndirectImage,
        "disabled VMEM descriptor table read its unavailable backing");
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "VMEM format disable incorrectly disabled a shared scalar descriptor read");
  auto malformed = make(false, false);
  CheckFatal([&] { malformed->PlanAndTrack(); }, "not a valid runtime value",
             "waterfall without all descriptor equality witnesses was accepted");
}

void TestGuardedDirectImageTable() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Guard { Nonzero, SccNonZero, Plain, Zero, Unrelated, Bypass, ExecZero, VccZero };
  const auto make_plan = [](Guard guard) {
    Fixture fixture(ShaderType::Pixel);
    fixture.program.wave_size = 64u;
    auto *entry = fixture.block;
    auto *middle = fixture.AddBlock();
    auto *before_sample = fixture.AddBlock();
    auto *sample = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    entry->AddBranch(middle);
    entry->AddBranch(exit);
    middle->AddBranch(before_sample);
    before_sample->AddBranch(sample);
    sample->AddBranch(exit);
    if (guard == Guard::Bypass) exit->AddBranch(sample);
    const auto mask = fixture.Emit(ValueOpcode::ReadFirstLane,
        {fixture.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)}), Value(true)});
    const auto nonzero = fixture.Emit(
        ValueOpcode::INotEqual32,
        {Value(0u), guard == Guard::Unrelated ? fixture.UserData(2) : mask});
    const auto kind = guard == Guard::ExecZero ? CFG::BranchCondition::ExecZero
                    : guard == Guard::VccZero ? CFG::BranchCondition::VccZero
                                             : CFG::BranchCondition::SccZero;
    auto condition = nonzero;
    if (guard == Guard::SccNonZero) {
      condition = fixture.Emit(ValueOpcode::ConditionRef, {condition},
                               CFG::BranchCondition::SccNonZero);
    }
    condition = fixture.Emit(ValueOpcode::LogicalNot, {condition});
    if (guard != Guard::Plain && guard != Guard::SccNonZero) {
      condition = fixture.Emit(ValueOpcode::ConditionRef, {condition}, kind);
    }
    fixture.program.blocks[0]->condition = condition;
    fixture.program.blocks[0]->terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = fixture.program.blocks[guard == Guard::Zero ? 1u : 4u],
        .false_block = fixture.program.blocks[guard == Guard::Zero ? 4u : 1u]};
    for (uint32_t block = 1; block < 4; ++block) {
      fixture.program.blocks[block]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[block + 1u]};
    }
    fixture.program.blocks[4]->terminator = {
        .kind = guard == Guard::Bypass ? CFG::TerminatorKind::Branch
                                       : CFG::TerminatorKind::Return,
        .true_block = fixture.program.blocks[3u]};
    const auto srt = fixture.Address(fixture.UserData(0), fixture.UserData(1));
    std::array<Value, 2> pointer;
    for (uint32_t word = 0; word < pointer.size(); ++word) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = word * 4u;
      pointer[word] = fixture.Emit(
          ValueOpcode::LoadAddressU32, {srt, Value(0u), Value(0u), Value(true)},
          fixture.AddMemory(memory, 0x20));
    }
    fixture.block = sample;
    const auto key = fixture.Emit(ValueOpcode::FindILsb32, {mask});
    const auto offset = fixture.Emit(ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)}),
         Value(344u)});
    std::array<Value, 8> words;
    for (uint32_t group = 0; group < 2u; ++group) {
      // Separate equivalent pointer handles mirror the two scalar x4 loads.
      const auto table = fixture.Address(pointer[0], pointer[1]);
      const auto group_offset = group == 0u ? offset : fixture.Emit(
          ValueOpcode::IAdd32, {offset, Value(16u)});
      for (uint32_t word = 0; word < 4u; ++word) {
        MemoryInfo memory;
        memory.kind = ResourceKind::ScalarAddress;
        memory.offset = word * 4u;
        words[group * 4u + word] = fixture.Emit(
            ValueOpcode::LoadAddressU32,
            {table, group_offset, Value(0u), Value(true)},
            fixture.AddMemory(memory, 0x100 + group * 8u));
      }
    }
    const auto image = fixture.Image(words, 0x128);
    const auto sampler = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress()},
                 fixture.AddMemory(memory, 0x128));
    fixture.PlanAndTrack();
    const auto source = fixture.program.info.images[0].source;
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_descriptor;
    Check(indirect && !indirect->selector.has_value() &&
              indirect->table_offset == 344u &&
              indirect->key_count.Resolve().IsImmediate() &&
              indirect->key_count.Resolve().U32() == 32u &&
              fixture.program.descriptor_sources[indirect->table_source].dword_count == 2u,
          "guarded direct image table lost its pointer or proven selector range");
    return ExtractResourcePlan(fixture.program);
  };

  auto plan = make_plan(Guard::Nonzero);
  make_plan(Guard::SccNonZero);
  make_plan(Guard::Plain);
  for (const auto guard : {Guard::Zero, Guard::Unrelated, Guard::Bypass,
                           Guard::ExecZero, Guard::VccZero}) {
    CheckFatal([&] { make_plan(guard); }, "not a valid runtime value",
               "direct table accepted a selector without a dominating nonzero guard");
  }
  LinearTestMemory memory;
  constexpr uint64_t table = 0x1800u + 344u;
  memory.words[0] = 0x1800u;
  const auto fill_table = [](LinearTestMemory &memory, uint64_t base) {
    for (uint32_t key = 0; key < 32u; ++key) {
      const auto word = (base - memory.base) / 4u + key * 8u;
      memory.words[word] = 0x100u + key;
      memory.words[word + 1u] = static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
      memory.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
          (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    }
  };
  fill_table(memory, table);
  std::array<uint32_t, 2> user_data{0x1000u, 0u};
  SrtRuntime runtime{.user_data = user_data, .read_memory = ReadLinearTestMemory,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 32u && specialization.images.size() == 32u &&
            snapshot.flattened_srt[specialization.images[0].indirect_mapping_offset] == 32u &&
            memory.reads == 34u && memory.descriptor_reads == 32u,
        "direct table did not retain all 32 reachable descriptors");
  const auto captured_word = (table - memory.base) / 4u + 16u * 8u;
  const auto original_descriptor = snapshot.images[16].dwords;
  const std::array<uint32_t, 8> captured_invalid{
      0x101f0000u, 0xcb500000u, 0x001fc01fu, 0xd0970facu,
      0x86000000u, 0x00500003u, 0x00000400u, 0x00005204u};
  std::copy(captured_invalid.begin(), captured_invalid.end(),
             memory.words.begin() + captured_word);
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 32u &&
            snapshot.flattened_srt[specialization.images[0].indirect_mapping_offset] == 32u &&
            std::ranges::all_of(snapshot.images[16].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "captured non-descriptor record became a host image or lost its key mapping");
  std::copy(original_descriptor.begin(), original_descriptor.end(),
             memory.words.begin() + captured_word);
  memory.words[(table - memory.base) / 4u + 31u * 8u] = 0x987u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images[31].dwords[0] == 0x987u,
        "direct table refresh reused stale descriptor contents");
  memory.fail_address = table + 31u * 32u + 28u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "direct table accepted an unreadable final descriptor word");

  LinearTestMemory wrapping;
  wrapping.base = 0u;
  wrapping.words[0x1000u / 4u] = 0xffffff00u;
  wrapping.words[0x1000u / 4u + 1u] = 0xffffu;
  wrapping.watched_address = 88u;
  fill_table(wrapping, 88u);
  runtime.userdata = &wrapping;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            wrapping.watched_reads == 0u,
        "direct table wrapped its descriptor address at the 48-bit boundary");

  LinearTestMemory endpoint;
  endpoint.base = (uint64_t{1} << 48u) - 0x1000u;
  const auto crossing = (uint64_t{1} << 48u) - 16u;
  endpoint.words[0] = static_cast<uint32_t>(crossing - 344u);
  endpoint.words[1] = static_cast<uint32_t>((crossing - 344u) >> 32u);
  fill_table(endpoint, crossing);
  endpoint.watched_address = crossing;
  user_data = {static_cast<uint32_t>(endpoint.base),
               static_cast<uint32_t>(endpoint.base >> 32u)};
  runtime.userdata = &endpoint;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            endpoint.watched_reads == 0u,
        "batched descriptor read crossed the 48-bit endpoint");
}

void TestBoundedComputeImageLoop() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant {
    Bounded, Plain, Nonzero, TrueEdge, WrongGuard, EntryBypass, ExitBypass, GuardBlock,
    WrongStep, DivergentBound, DivergentKey, Disjunction, WrongPolarity,
    IncrementBypass, PreviousBound, Forwarded, ForwardedChain, ForwardedJoin,
    ForwardedBranch, ForwardedIncrementBypass,
    Masked, MaskedWrongGuard, MaskResurrection, StatusOverwrite,
    GuardedDiamond, GuardedDiamondBypass
  };
  const auto make_plan = [](Variant variant) {
    const bool diamond = variant >= Variant::GuardedDiamond;
    Fixture fixture(diamond ? ShaderType::Vertex : ShaderType::Compute);
    fixture.program.wave_size = 64u;
    const bool masked = variant >= Variant::Masked;
    auto *entry = fixture.block;
    auto *header = fixture.AddBlock();
    auto *body = fixture.AddBlock();
    auto *latch = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    auto *compare = masked ? fixture.AddBlock() : header;
    auto *guard = masked ? fixture.AddBlock() : header;
    auto *increment = masked ? fixture.AddBlock() : latch;
    auto *final_exit = variant == Variant::PreviousBound ? fixture.AddBlock() : exit;
    const bool forwarded = variant >= Variant::Forwarded && variant <= Variant::ForwardedIncrementBypass;
    auto *forward = forwarded ? fixture.AddBlock() : increment;
    auto *incoming = variant == Variant::ForwardedChain ? fixture.AddBlock() : forward;
    auto *detour = variant == Variant::ForwardedJoin ? fixture.AddBlock() : nullptr;
    entry->AddBranch(header);
    if (!masked) {
      header->AddBranch(exit);
      header->AddBranch(body);
      body->AddBranch(latch);
      latch->AddBranch(forwarded ? forward : header);
      if (variant == Variant::PreviousBound) latch->AddBranch(final_exit);
      if (variant == Variant::EntryBypass) entry->AddBranch(body);
      if (variant == Variant::ExitBypass) exit->AddBranch(body);
      if (variant == Variant::IncrementBypass || variant == Variant::PreviousBound ||
          variant == Variant::ForwardedIncrementBypass)
        exit->AddBranch(latch);
      fixture.program.blocks[0]->terminator = {
          .kind = variant == Variant::EntryBypass
                      ? CFG::TerminatorKind::ConditionalBranch
                      : CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[1u],
          .false_block = fixture.program.blocks[2u]};
      fixture.program.blocks[1]->terminator = {
          .kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block =
              fixture.program.blocks[variant == Variant::TrueEdge ? 2u : 4u],
          .false_block =
              fixture.program.blocks[variant == Variant::TrueEdge ? 4u : 2u]};
      fixture.program.blocks[2]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[3u]};
      fixture.program.blocks[3]->terminator = {
          .kind = variant == Variant::PreviousBound
                      ? CFG::TerminatorKind::ConditionalBranch
                      : CFG::TerminatorKind::Branch,
          .true_block = forwarded ? forward : fixture.program.blocks[1u],
          .false_block = final_exit};
      fixture.program.blocks[4]->terminator = {
          .kind = final_exit != exit || variant == Variant::ExitBypass ||
                          variant == Variant::IncrementBypass || variant == Variant::ForwardedIncrementBypass
                      ? CFG::TerminatorKind::Branch
                      : CFG::TerminatorKind::Return,
          .true_block =
              fixture.program.blocks[final_exit != exit ||
                                             variant == Variant::IncrementBypass || variant == Variant::ForwardedIncrementBypass
                                         ? 3u
                                         : 2u]};
      if (final_exit != exit)
        fixture.program.blocks[5]->terminator.kind =
            CFG::TerminatorKind::Return;
    }

    auto &phi = header->AppendNewInst(ValueOpcode::Phi, {},
                                      static_cast<uint64_t>(Type::U32));
    const auto key = Value(&phi);
    auto *previous = variant == Variant::PreviousBound ?
        &header->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U1)) : nullptr;
    const auto local = fixture.Emit(
        ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(diamond ? StageInputKind::VertexIndex
                                            : StageInputKind::LocalInvocationId)), Value(0u)});
    const auto count = variant == Variant::DivergentBound ? local : fixture.UserData(2);
    const auto in_range = fixture.Emit(ValueOpcode::SLessThan32,
                                       {variant == Variant::WrongGuard
                                            ? Value(0u) : key,
                                        count}, 0, compare);
    const auto initial_active = fixture.Emit(ValueOpcode::INotEqual32,
                                             {local, Value(0u)}, 0, entry);
    if (forwarded) {
      forward->AddBranch(incoming == forward ? header : incoming);
      forward->terminator = {.kind = CFG::TerminatorKind::Branch,
                            .true_block = incoming == forward ? header : incoming};
      if (incoming != forward) {
        incoming->AddBranch(header);
        incoming->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = header};
      }
      if (variant == Variant::ForwardedBranch) {
        forward->AddBranch(exit);
        forward->condition = initial_active;
        forward->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                              .true_block = header, .false_block = exit};
      }
      if (detour != nullptr) {
        latch->AddBranch(detour);
        latch->condition = initial_active;
        latch->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                            .true_block = forward, .false_block = detour};
        detour->AddBranch(forward);
        detour->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = forward};
      }
    }
    if (masked) {
      auto *active_phi = diamond ? nullptr : &header->AppendNewInst(
          ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U1));
      auto &saved_phi = header->AppendNewInst(ValueOpcode::Phi, {},
                                              static_cast<uint64_t>(Type::U1));
      auto &status_phi = header->AppendNewInst(ValueOpcode::Phi, {},
                                               static_cast<uint64_t>(Type::U32));
      const auto saved = Value(&saved_phi);
      const auto status = Value(&status_phi);
      const auto active = diamond ? fixture.Emit(ValueOpcode::LogicalAnd,
          {saved, fixture.Emit(ValueOpcode::UGreaterThanEqual32,
                               {Value(0u), status}, 0, header)}, 0, header) : Value(active_phi);
      const auto mask = fixture.Emit(ValueOpcode::LogicalOr,
          {fixture.Emit(ValueOpcode::LogicalAnd, {in_range, active}, 0, compare),
           fixture.Emit(ValueOpcode::LogicalNot, {active}, 0, compare)}, 0, compare);
      auto next_saved = fixture.Emit(ValueOpcode::LogicalAnd,
          {variant == Variant::MaskResurrection ? Value(true) : saved, mask}, 0, compare);
      const auto execute = fixture.Emit(ValueOpcode::LogicalAnd, {active, mask}, 0, compare);
      const auto body_status = fixture.Emit(ValueOpcode::SelectU32,
          {execute, local, status}, 0, guard);
      const auto next_active = fixture.Emit(ValueOpcode::LogicalAnd,
          {next_saved, fixture.Emit(ValueOpcode::UGreaterThanEqual32,
                                   {Value(3u), body_status}, 0, latch)}, 0, latch);
      auto next_status = fixture.Emit(ValueOpcode::SelectU32,
          {variant == Variant::StatusOverwrite ? Value(true) : next_active,
           Value(0u), body_status}, 0,
          variant == Variant::GuardedDiamondBypass ? latch : increment);
      if (variant == Variant::GuardedDiamondBypass) {
        auto &merged_saved = increment->AppendNewInst(ValueOpcode::Phi, {},
                                                      static_cast<uint64_t>(Type::U1));
        merged_saved.AddPhiOperand(compare, saved);
        merged_saved.AddPhiOperand(latch, next_saved);
        next_saved = Value(&merged_saved);
        auto &merged_status = increment->AppendNewInst(ValueOpcode::Phi, {},
                                                       static_cast<uint64_t>(Type::U32));
        merged_status.AddPhiOperand(compare, status);
        merged_status.AddPhiOperand(latch, next_status);
        next_status = Value(&merged_status);
      }
      if (active_phi != nullptr) {
        active_phi->AddPhiOperand(entry, initial_active);
        active_phi->AddPhiOperand(increment, next_active);
      }
      saved_phi.AddPhiOperand(entry, initial_active);
      saved_phi.AddPhiOperand(increment, next_saved);
      status_phi.AddPhiOperand(entry, diamond ? Value(0u) : local);
      status_phi.AddPhiOperand(increment, next_status);
      const auto branch = [&](uint32_t index, uint32_t yes, uint32_t no,
                              Value predicate, CFG::BranchCondition kind) {
        auto *block = fixture.program.blocks[index];
        block->AddBranch(fixture.program.blocks[yes]);
        block->AddBranch(fixture.program.blocks[no]);
        const auto inverse = fixture.Emit(ValueOpcode::LogicalNot, {predicate}, 0, block);
        fixture.program.blocks[index]->condition =
            fixture.Emit(ValueOpcode::ConditionRef, {inverse}, kind, block);
        fixture.program.blocks[index]->terminator = {
            .kind = CFG::TerminatorKind::ConditionalBranch,
            .true_block = fixture.program.blocks[yes],
            .false_block = fixture.program.blocks[no]};
      };
      fixture.program.blocks[0]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[1u]};
      branch(1u, 4u, 5u, active, CFG::BranchCondition::ExecZero);
      branch(5u, variant == Variant::GuardedDiamondBypass ? 7u : 4u, 6u,
             diamond ? execute : mask,
             diamond ? CFG::BranchCondition::ExecZero : CFG::BranchCondition::VccZero);
      const auto image_guard = variant == Variant::MaskedWrongGuard ? active :
          fixture.Emit(ValueOpcode::LogicalAnd,
              {fixture.Emit(ValueOpcode::LogicalAnd, {execute, in_range}, 0, guard),
               fixture.Emit(ValueOpcode::IEqual32, {local, Value(1u)}, 0, guard)}, 0, guard);
      branch(6u, 3u, 2u, image_guard, CFG::BranchCondition::ExecZero);
      body->AddBranch(latch);
      fixture.program.blocks[2]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[3u]};
      branch(3u, 4u, 7u, next_active, CFG::BranchCondition::ExecZero);
      increment->AddBranch(header);
      fixture.program.blocks[7]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[1u]};
      fixture.program.blocks[4]->terminator.kind = CFG::TerminatorKind::Return;
    } else {
      const auto active = initial_active;
      const auto allowed = fixture.Emit(variant == Variant::Disjunction
                                           ? ValueOpcode::LogicalOr : ValueOpcode::LogicalAnd,
                                        {in_range, active}, 0, header);
      const bool nonzero = variant == Variant::Nonzero || variant == Variant::TrueEdge;
      auto condition = allowed;
      if (!nonzero)
        condition = fixture.Emit(ValueOpcode::LogicalNot, {condition}, 0, header);
      if (variant != Variant::Plain)
        condition = fixture.Emit(ValueOpcode::ConditionRef, {condition},
            nonzero ? CFG::BranchCondition::ExecNonZero
                    : CFG::BranchCondition::ExecZero, header);
      if (variant == Variant::Nonzero || variant == Variant::WrongPolarity)
        condition = fixture.Emit(ValueOpcode::LogicalNot, {condition}, 0, header);
      fixture.program.blocks[1]->condition = condition;
      if (variant == Variant::PreviousBound) {
        previous->AddPhiOperand(entry, Value(false));
        previous->AddPhiOperand(latch, in_range);
        const auto continuing = fixture.Emit(ValueOpcode::LogicalOr,
            {in_range, Value(previous)}, 0, latch);
        fixture.program.blocks[3]->condition =
            fixture.Emit(ValueOpcode::ConditionRef, {continuing},
                         CFG::BranchCondition::SccNonZero, latch);
      }
    }
    const auto step = fixture.Emit(ValueOpcode::IAdd32,
                                   {key, Value(variant == Variant::WrongStep ? 2u : 1u)},
                                   0, increment);
    phi.AddPhiOperand(entry, variant == Variant::DivergentKey ? local : Value(0u));
    phi.AddPhiOperand(forwarded ? incoming : increment, step);
    Check(!forwarded || step.ResolveInstruction()->Parent() != phi.PhiBlock(1),
          "forwarded loop fixture did not separate the increment from its Phi incoming edge");

    fixture.block = variant == Variant::GuardBlock ? header : body;
    const auto table = fixture.Address(fixture.UserData(0), fixture.UserData(1));
    const auto offset = fixture.Emit(ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)}),
         Value(0x6b0u)});
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = word * sizeof(uint32_t);
      words[word] = fixture.Emit(
          ValueOpcode::LoadAddressU32,
          {table, offset, Value(0u), Value(true)},
          fixture.AddMemory(memory, 0x29c));
    }
    const auto image = fixture.Image(words, 0x29c);
    const auto sampler = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw,
                 {image, sampler, fixture.ImageAddress()},
                 fixture.AddMemory(sample, 0x29c));
    fixture.PlanAndTrack();
    const auto source = fixture.program.info.images[0].source;
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_descriptor;
    Check(indirect && !indirect->selector.has_value() &&
              indirect->table_offset == 0x6b0u &&
              indirect->key_count.Resolve() == count.Resolve(),
          "bounded compute loop lost its runtime image count");
    return ExtractResourcePlan(fixture.program);
  };

  auto plan = make_plan(Variant::Bounded);
  make_plan(Variant::Plain);
  make_plan(Variant::Nonzero);
  make_plan(Variant::TrueEdge);
  make_plan(Variant::Masked);
  make_plan(Variant::GuardedDiamond);
  make_plan(Variant::Forwarded);
  make_plan(Variant::ForwardedChain);
  for (const auto variant : {Variant::IncrementBypass, Variant::PreviousBound,
                             Variant::ForwardedJoin, Variant::ForwardedBranch,
                             Variant::ForwardedIncrementBypass,
                             Variant::MaskedWrongGuard,
                             Variant::MaskResurrection, Variant::StatusOverwrite,
                             Variant::GuardedDiamondBypass}) {
    CheckFatal([&] { make_plan(variant); }, "not a valid runtime value",
               "compute image loop allowed an unbounded induction or mask resurrection");
  }
  CheckFatal([&] { make_plan(Variant::WrongGuard); },
             "not a valid runtime value",
             "compute image loop accepted an unrelated guard");
  CheckFatal([&] { make_plan(Variant::EntryBypass); },
             "not a valid runtime value",
             "compute image loop accepted an entry bypass");
  CheckFatal([&] { make_plan(Variant::ExitBypass); },
             "not a valid runtime value",
             "compute image loop accepted an exit bypass");
  CheckFatal([&] { make_plan(Variant::GuardBlock); },
             "not a valid runtime value",
             "compute image loop accepted a descriptor read before the guard");
  CheckFatal([&] { make_plan(Variant::WrongStep); },
             "not a valid runtime value",
             "compute image loop accepted a two-step induction");
  for (const auto variant : {Variant::DivergentBound, Variant::DivergentKey,
                             Variant::Disjunction, Variant::WrongPolarity}) {
    CheckFatal([&] { make_plan(variant); }, "not a valid runtime value",
               "compute image loop accepted a guard without a uniform bound");
  }

  LinearTestMemory memory;
  const auto table = 0x1800u + 0x6b0u;
  for (uint32_t key = 0; key < 3u; ++key) {
    const auto word = (table - memory.base) / 4u + key * 8u;
    memory.words[word] = 0x100u + key;
    memory.words[word + 1u] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }
  std::array<uint32_t, 3> user_data{0x1800u, 0u, 2u};
  SrtRuntime runtime{.user_data = user_data,
                     .read_memory = ReadLinearTestMemory,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const uint32_t count : {2u, 3u, 2u}) {
    user_data[2] = count;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == count &&
              specialization.images.size() == count &&
              snapshot.flattened_srt[
                  specialization.images[0].indirect_mapping_offset] == count &&
              snapshot.images.back().dwords[0] == 0x100u + count - 1u,
          "compute image table did not refresh for a changed loop bound");
  }
  for (const uint32_t count : {0u, UINT32_MAX}) {
    user_data[2] = count;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == 1u &&
              specialization.images.size() == 1u &&
              std::ranges::all_of(snapshot.images[0].dwords,
                                  [](uint32_t word) { return word == 0u; }) &&
              specialization.images[0].indirect_root ==
                  ImageResource::NoIndirectImage &&
              specialization.images[0].indirect_search_iterations == 0u,
          "empty compute loop bound retained unreachable image candidates");
  }
  user_data[2] = 65537u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "oversized compute loop bound was accepted for image enumeration");
}

void TestUniformizedMaterialImageKeys() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant {
    Valid, Plain, WrongUpdate, WrongEquality, WrongExit, WrongCarry, WrongBackedge,
    AndNot, Subset, FirstLane, FirstLaneDirect, FirstLaneAndNot, EmptyFirstEntry, WrongFirstBackedge,
    WideningFirstEntry, WideningFirstBackedge, WrongFirstLoadMask
  };
  const auto make_plan = [](Variant variant) {
    Fixture fixture;
    fixture.program.wave_size = 64u;
    const bool first_lane = variant >= Variant::FirstLane;
    const bool first_lane_loop = first_lane && variant != Variant::FirstLaneDirect;
    const uint32_t sample_entry_id = first_lane_loop ? 10u : 9u;
    const auto branch = [&](Value predicate, CFG::BranchCondition kind, Block *block) {
      return variant == Variant::Plain ? predicate : fixture.Emit(
          ValueOpcode::ConditionRef, {predicate}, kind, block);
    };
    auto *entry = fixture.block;
    auto *header = fixture.AddBlock();
    auto *inactive = fixture.AddBlock();
    auto *sentinel = fixture.AddBlock();
    auto *bit = fixture.AddBlock();
    auto *merge = fixture.AddBlock();
    auto *choose = fixture.AddBlock();
    auto *sample = fixture.AddBlock();
    auto *done = fixture.AddBlock();
    auto *sample_header = first_lane_loop ? fixture.AddBlock() : nullptr;
    auto *sample_entry = first_lane ? fixture.AddBlock() : nullptr;
    entry->AddBranch(header);
    header->AddBranch(inactive);
    inactive->AddBranch(merge);
    inactive->AddBranch(sentinel);
    sentinel->AddBranch(merge);
    sentinel->AddBranch(bit);
    bit->AddBranch(header);
    bit->AddBranch(merge);
    merge->AddBranch(choose);
    choose->AddBranch(first_lane ? sample_entry : sample);
    choose->AddBranch(done);
    if (first_lane) {
      sample_entry->AddBranch(first_lane_loop ? sample_header : sample);
      fixture.program.blocks[sample_entry_id]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[first_lane_loop ? 9u : 7u]};
    }
    if (first_lane_loop) {
      sample_header->AddBranch(sample);
      sample->AddBranch(sample_header);
      fixture.program.blocks[9]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[7u],
          .merge_block = fixture.program.blocks[8u],
          .continue_block = fixture.program.blocks[7u],
          .loop_header = true};
    }
    sample->AddBranch(done);
    fixture.program.blocks[0]->terminator = {
        .kind = CFG::TerminatorKind::Branch,
        .true_block = fixture.program.blocks[1u]};
    fixture.program.blocks[1]->terminator = {
        .kind = CFG::TerminatorKind::Branch,
        .true_block = fixture.program.blocks[2u]};
    const auto enabled = fixture.Emit(
        ValueOpcode::INotEqual32, {fixture.UserData(5u), Value(0u)}, 0, entry);
    const auto local = fixture.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)}, 0, entry);
    const auto active_on_entry = fixture.Emit(ValueOpcode::LogicalAnd,
        {enabled, fixture.Emit(ValueOpcode::INotEqual32, {local, Value(0u)}, 0, entry)},
        0, entry);
    auto &active_phi = header->AppendNewInst(
        ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U1));
    auto &mask_phi = header->AppendNewInst(
        ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    auto &carry_phi = header->AppendNewInst(
        ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    const auto mask = Value(&mask_phi);
    const auto active = Value(&active_phi);
    fixture.program.blocks[2]->condition =
        branch(fixture.Emit(ValueOpcode::LogicalNot, {active}, 0, inactive),
               variant == Variant::WrongExit ? CFG::BranchCondition::ExecNonZero
                                             : CFG::BranchCondition::ExecZero,
               inactive);
    fixture.program.blocks[2]->terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = fixture.program.blocks[5u],
        .false_block = fixture.program.blocks[3u]};
    const auto nonzero = fixture.Emit(
        ValueOpcode::INotEqual32, {Value(0u), mask}, 0, sentinel);
    const auto bit_guard = fixture.Emit(
        ValueOpcode::LogicalAnd, {active, nonzero}, 0, sentinel);
    fixture.program.blocks[3]->condition =
        branch(fixture.Emit(ValueOpcode::LogicalNot, {bit_guard}, 0, sentinel),
               CFG::BranchCondition::ExecZero, sentinel);
    fixture.program.blocks[3]->terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = fixture.program.blocks[5u],
        .false_block = fixture.program.blocks[4u]};
    const auto first = fixture.Emit(ValueOpcode::FindILsb32, {mask}, 0, bit);
    const auto position = fixture.Emit(
        ValueOpcode::BitwiseAnd32, {first, Value(31u)}, 0, bit);
    const auto one_bit = fixture.Emit(
        ValueOpcode::ShiftLeftLogical32, {Value(1u), position}, 0, bit);
    const bool and_not = variant == Variant::AndNot || variant == Variant::FirstLaneAndNot;
    const auto removed = and_not
        ? fixture.Emit(ValueOpcode::BitwiseNot32, {one_bit}, 0, bit)
        : variant == Variant::Subset ? fixture.UserData(7u) : one_bit;
    const auto cleared = fixture.Emit(
        variant == Variant::WrongUpdate ? ValueOpcode::BitwiseOr32
        : and_not || variant == Variant::Subset ? ValueOpcode::BitwiseAnd32
                                              : ValueOpcode::BitwiseXor32,
        {mask, removed}, 0, bit);
    const auto continuation = fixture.Emit(
        ValueOpcode::LogicalAnd, {bit_guard, active_on_entry}, 0, bit);
    fixture.program.blocks[4]->condition =
        branch(continuation, CFG::BranchCondition::ExecNonZero, bit);
    fixture.program.blocks[4]->terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = fixture.program.blocks[1u],
        .false_block = fixture.program.blocks[5u]};
    active_phi.AddPhiOperand(entry, active_on_entry);
    active_phi.AddPhiOperand(bit, continuation);
    mask_phi.AddPhiOperand(entry, fixture.UserData(4u));
    mask_phi.AddPhiOperand(bit, cleared);
    const auto arbitrary = fixture.UserData(6u);
    const auto carry = Value(&carry_phi);
    const auto sentinel_index = fixture.Emit(
        ValueOpcode::SelectU32,
        {active, Value(32u), variant == Variant::WrongCarry ? arbitrary : carry},
        0, sentinel);
    const auto bit_index = fixture.Emit(
        ValueOpcode::SelectU32, {bit_guard, first, sentinel_index}, 0, bit);
    carry_phi.AddPhiOperand(entry, arbitrary);
    carry_phi.AddPhiOperand(bit, variant == Variant::WrongBackedge ? arbitrary : bit_index);
    auto &index_phi = merge->AppendNewInst(
        ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    index_phi.AddPhiOperand(inactive, carry);
    index_phi.AddPhiOperand(sentinel, sentinel_index);
    index_phi.AddPhiOperand(bit, bit_index);
    const auto index = Value(&index_phi);
    const auto below = fixture.Emit(
        ValueOpcode::SGreaterThan32, {Value(32u), index}, 0, merge);
    const auto material_guard = fixture.Emit(
        ValueOpcode::LogicalAnd, {active_on_entry, below}, 0, merge);
    fixture.program.blocks[5]->terminator = {
        .kind = CFG::TerminatorKind::Branch,
        .true_block = fixture.program.blocks[6u]};
    const auto scaled = fixture.Emit(
        ValueOpcode::ShiftLeftLogical32, {index, Value(4u)}, 0, choose);
    const auto selected_scale = fixture.Emit(
        ValueOpcode::SelectU32, {material_guard, scaled, Value(0u)}, 0, choose);
    const auto times_eight = fixture.Emit(
        ValueOpcode::ShiftLeftLogical32, {selected_scale, Value(3u)}, 0, choose);
    const auto times_nine = fixture.Emit(
        ValueOpcode::IAdd32, {times_eight, selected_scale}, 0, choose);
    const auto material_offset = fixture.Emit(
        ValueOpcode::SelectU32,
        {material_guard,
         fixture.Emit(ValueOpcode::IAdd32,
                      {times_nine, Value(0xc00u)}, 0, choose),
         times_nine}, 0, choose);
    const auto base = fixture.Address(fixture.UserData(0u), fixture.UserData(1u));
    MemoryInfo material_memory;
    material_memory.kind = ResourceKind::Global;
    const auto load_mask = variant == Variant::WrongFirstLoadMask
        ? fixture.Emit(ValueOpcode::LogicalNot, {material_guard}, 0, choose)
        : material_guard;
    const auto loaded = fixture.Emit(
        ValueOpcode::LoadAddressU32,
        {base, material_offset, Value(0u), load_mask},
        fixture.AddMemory(material_memory, 0x1a88u), choose);
    const auto local_key = fixture.Emit(
        ValueOpcode::SelectU32, {material_guard, loaded, arbitrary}, 0, choose);
    Inst *sample_active_phi = nullptr;
    auto sample_active = material_guard;
    if (first_lane) {
      const auto initial = variant == Variant::EmptyFirstEntry ? Value(false)
          : variant == Variant::WideningFirstEntry ? enabled
          : fixture.Emit(ValueOpcode::LogicalAnd,
              {material_guard, fixture.Emit(ValueOpcode::SLessThanEqual32,
                  {Value(0u), local_key}, 0, choose)}, 0, choose);
      sample_active = initial;
      if (first_lane_loop) {
        sample_active_phi = &sample_header->AppendNewInst(
            ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U1));
        sample_active_phi->AddPhiOperand(sample_entry, initial);
        sample_active = Value(sample_active_phi);
      }
      fixture.program.blocks[6]->condition =
          branch(variant == Variant::EmptyFirstEntry ? enabled : initial,
                 CFG::BranchCondition::ExecNonZero, choose);
      fixture.program.blocks[6]->terminator = {
          .kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = fixture.program.blocks[sample_entry_id],
          .false_block = fixture.program.blocks[8u]};
    }
    auto *key_block = first_lane ? sample : choose;
    const auto key = first_lane
        ? fixture.Emit(ValueOpcode::ReadFirstLane, {local_key, sample_active}, 0, key_block)
        : fixture.Emit(ValueOpcode::ReadLane, {local_key, Value(0u)}, 0, key_block);
    const auto compared = fixture.Emit(
        ValueOpcode::IEqual32,
        {key, variant == Variant::WrongEquality ? arbitrary : local_key}, 0, key_block);
    const auto sample_guard = fixture.Emit(
        ValueOpcode::LogicalAnd, {sample_active, compared}, 0, key_block);
    if (!first_lane) {
      fixture.program.blocks[6]->condition = branch(
          fixture.Emit(ValueOpcode::LogicalNot, {sample_guard}, 0, choose),
          CFG::BranchCondition::ExecZero, choose);
      fixture.program.blocks[6]->terminator = {
          .kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = fixture.program.blocks[8u],
          .false_block = fixture.program.blocks[7u]};
    }
    const auto table_offset = fixture.Emit(
        ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32,
                      {key, Value(5u)}, 0, sample), Value(0x20e0u)}, 0, sample);
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = word * 4u;
      words[word] = fixture.Emit(
          ValueOpcode::LoadAddressU32,
          {base, table_offset, Value(0u), Value(true)},
          fixture.AddMemory(memory, 0x1accu), sample);
    }
    const auto image = fixture.Emit(
        ValueOpcode::GetImageResource,
        {words[0], words[1], words[2], words[3],
         words[4], words[5], words[6], words[7]}, 0, sample);
    const auto sampler = fixture.Emit(
        ValueOpcode::GetSamplerResource,
        {Value(0u), Value(0u), Value(0u), Value(0u)}, 0, sample);
    const auto address = fixture.Emit(
        ValueOpcode::MakeImageAddress,
        {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
         Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
         Value(0u)}, 0, sample);
    MemoryInfo image_memory;
    image_memory.kind = ResourceKind::Image;
    image_memory.image_dimension = Decoder::ImageDimension::Dim2D;
    const auto sampled = fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, address},
        fixture.AddMemory(image_memory, 0x1aecu), sample);
    if (first_lane) {
      const auto component = fixture.Emit(
          ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)}, 0, sample);
      fixture.Emit(ValueOpcode::ReferenceU32,
          {fixture.Emit(ValueOpcode::SelectU32,
              {variant == Variant::EmptyFirstEntry ? material_guard : sample_guard,
               component, Value(0u)}, 0, sample)}, 0, sample);
    }
    if (first_lane_loop) {
      auto remaining = fixture.Emit(ValueOpcode::LogicalAnd,
          {sample_active, fixture.Emit(ValueOpcode::LogicalNot,
              {sample_guard}, 0, sample)}, 0, sample);
      if (variant == Variant::WideningFirstBackedge)
        remaining = fixture.Emit(ValueOpcode::LogicalOr, {remaining, enabled}, 0, sample);
      sample_active_phi->AddPhiOperand(sample, remaining);
      fixture.program.blocks[7]->condition = branch(
          variant == Variant::WrongFirstBackedge ? sample_guard : remaining,
          CFG::BranchCondition::ExecNonZero, sample);
      fixture.program.blocks[7]->terminator = {
          .kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = fixture.program.blocks[9u],
          .false_block = fixture.program.blocks[8u]};
    } else {
      fixture.program.blocks[7]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[8u]};
    }
    fixture.program.blocks[8]->terminator.kind = CFG::TerminatorKind::Return;
    const auto output = fixture.Emit(
        ValueOpcode::GetBufferResource,
        {fixture.UserData(8u), fixture.UserData(9u),
         fixture.UserData(10u), fixture.UserData(11u)}, 0, done);
    MemoryInfo output_memory;
    output_memory.kind = ResourceKind::Buffer;
    fixture.Emit(ValueOpcode::StoreBufferU32,
                 {output, Value(0u), Value(0u), Value(0u),
                  Value(1u), Value(true)},
                 fixture.AddMemory(output_memory, 0x1b00u), done);
    fixture.PlanAndTrack();
    const auto source = fixture.program.info.images[0].source;
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_descriptor;
    Check(indirect && indirect->selector->stride == 0x90u &&
              indirect->selector->offset == 0xc00u &&
              indirect->table_offset == 0x20e0u &&
              !indirect->selector_mask.IsEmpty() &&
              indirect->key_count.Resolve().IsImmediate() &&
              indirect->key_count.Resolve().U32() == 32u,
          "uniformized image key lost its finite material range");
    return ExtractResourcePlan(fixture.program);
  };
  auto plan = make_plan(Variant::Valid);
  make_plan(Variant::Plain);
  make_plan(Variant::AndNot);
  make_plan(Variant::Subset);
  make_plan(Variant::FirstLane);
  make_plan(Variant::FirstLaneDirect);
  plan = make_plan(Variant::FirstLaneAndNot);
  Check(plan.requires_specialization_memory &&
            plan.descriptor_sources[plan.info.images[0].source]
                .indirect_descriptor->selector_mask.Resolve().TryInstruction() != nullptr,
        "uniformized image mask did not survive extraction");
  LinearTestMemory memory;
  memory.words.resize(0x23000u / 4u);
  constexpr uint64_t base = 0x1000u;
  constexpr uint64_t first_material = base + 0xc00u + 2u * 0x90u;
  constexpr uint64_t second_material = base + 0xc00u + 29u * 0x90u;
  constexpr uint64_t first_table = base + 0x20e0u + 7u * 32u;
  constexpr uint64_t second_table = base + 0x20e0u + 4096u * 32u;
  memory.words[(first_material - base) / 4u] = 7u;
  memory.words[(second_material - base) / 4u] = 4096u;
  const auto set_descriptor = [&](uint64_t address, uint32_t color) {
    const auto word = (address - base) / 4u;
    memory.words[word] = color;
    memory.words[word + 1u] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  };
  set_descriptor(first_table, 0x200u);
  set_descriptor(second_table, 0x400u);
  std::array<uint32_t, 12> user_data{};
  user_data[0] = base;
  user_data[4] = (1u << 2u) | (1u << 29u);
  user_data[5] = 1u;
  user_data[8] = 0x400000u;
  user_data[9] = 16u << 16u;
  user_data[10] = 1u;
  memory.fail_address = base + 0xc00u + 3u * 0x90u;
  const SrtRuntime runtime{
      .user_data = user_data, .read_memory = ReadLinearTestMemory,
      .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 2u &&
            memory.reads == 4u && memory.descriptor_reads == 2u &&
            snapshot.flattened_srt[
                specialization.images[0].indirect_mapping_offset] == 2u &&
            snapshot.flattened_srt[
                specialization.images[0].indirect_mapping_offset + 1u] == 7u &&
            snapshot.flattened_srt[
                specialization.images[0].indirect_mapping_offset + 3u] == 4096u,
        "material mask did not limit sparse descriptor reads");
  user_data[8] = first_material;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            std::ranges::find(snapshot.specialization_reads,
                std::pair<uint64_t, uint64_t>{first_material, 4}) !=
                snapshot.specialization_reads.end(),
        "material key read was omitted from the renderer write-overlap check");
  user_data[8] = first_table;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            std::ranges::find(snapshot.specialization_reads,
                std::pair<uint64_t, uint64_t>{first_table, 32}) !=
                snapshot.specialization_reads.end(),
        "image descriptor read was omitted from the renderer write-overlap check");
  CheckFatal([&] { make_plan(Variant::WrongUpdate); }, "not a valid runtime value",
             "non-clearing material mask was accepted");
  CheckFatal([&] { make_plan(Variant::WrongEquality); }, "not a valid runtime value",
             "unrelated ReadLane key was accepted");
  CheckFatal([&] { make_plan(Variant::WrongExit); }, "not a valid runtime value",
             "one inactive lane bypassed material index initialization for active lanes");
  for (const auto variant : {Variant::WrongCarry, Variant::WrongBackedge}) {
    CheckFatal([&] { make_plan(variant); }, "not a valid runtime value",
               "inactive material lane did not preserve its selected index across iterations");
  }
  for (const auto variant : {Variant::EmptyFirstEntry, Variant::WrongFirstBackedge,
                            Variant::WideningFirstEntry, Variant::WideningFirstBackedge,
                            Variant::WrongFirstLoadMask}) {
    CheckFatal([&] { make_plan(variant); }, "not a valid runtime value",
               "first-lane material key escaped its active load mask");
  }
}

void TestImageDescriptorFields() {
  constexpr std::array<std::pair<uint32_t, uint32_t>, 5> reserved{
      {{1u, 0x20000000u}, {2u, 0x70003000u}, {4u, 0xe000e000u},
       {5u, 0xf9000000u}, {6u, 0x00007b00u}}};
  for (const bool r128 : {false, true}) {
    Fixture fixture;
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      words[word] = fixture.UserData(word);
    }
    const auto image = fixture.Image(words);
    const auto sampler = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    memory.image_r128 = r128;
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress()},
                 fixture.AddMemory(memory, 0x80));
    fixture.PlanAndTrack();
    auto plan = ExtractResourcePlan(fixture.program);
    std::array<uint32_t, 8> user_data{};
    user_data[0] = 0x100u;
    user_data[1] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    user_data[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    const SrtRuntime runtime{.user_data = user_data};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const auto is_null = [&] {
      return std::ranges::all_of(snapshot.images[0].dwords,
                                 [](uint32_t word) { return word == 0u; });
    };
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images[0].dwords == user_data,
          "valid texture descriptor was rejected");
    const auto valid_descriptor = user_data;
    // Keep RESOURCE_LEVEL set in this valid texture descriptor.
    user_data = {0x0208a200u, 0xca900000u, 0x800fc00fu, 0x90960facu,
                 0u, 0x60u, 0u, 0u};
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images[0].dwords == user_data,
          "instruction-ready texture descriptor lost RESOURCE_LEVEL or became null");
    user_data = valid_descriptor;
    for (const auto [word, mask] : reserved) {
      for (uint32_t bits = mask; bits != 0u; bits &= bits - 1u) {
        const auto bit = bits & (0u - bits);
        user_data[word] |= bit;
        Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                  (r128 && word >= 4u ? snapshot.images[0].dwords == user_data : is_null()),
              "reserved descriptor bits or ignored R128 upper words were misclassified");
        user_data[word] &= ~bit;
      }
    }
    user_data[5] = 0x06800000u;
    user_data[6] = 0x010880ffu;
    user_data[7] = 0x1234u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images[0].dwords == user_data,
          "defined mip-statistics, PRT, or metadata fields were treated as reserved");
    user_data[3] |= 3u << 16u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images[0].dwords == user_data,
          "view LAST_LEVEL above physical MAX_MIP was rejected");
    if (!r128) {
      user_data[3] = (user_data[3] & 0x0fffffffu) |
          (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2DArray) << 28u);
      user_data[4] = 3u | (4u << 16u);
      Check(MaterializeResources(plan, runtime, snapshot, specialization) && is_null(),
            "array view starting after its last slice was accepted");
      for (const auto base : {1u, 3u}) {
        user_data[4] = 3u | (base << 16u);
        Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                  snapshot.images[0].dwords == user_data,
              "valid array view with a nonzero base slice was rejected");
      }
    }
  }
}

void TestUniformScalarBufferImage() {
  Fixture fixture(ShaderType::Pixel);
  std::array<Value, 4> material_words;
  std::array<Value, 4> heap_words;
  for (uint32_t dword = 0; dword < 4; dword++) {
    material_words[dword] = fixture.UserData(dword);
    heap_words[dword] = fixture.UserData(dword + 4u);
  }
  const auto material = fixture.Buffer(material_words);
  const auto heap = fixture.Buffer(heap_words);
  const auto selector = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
                                     {fixture.UserData(8), Value(2u)});
  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarBuffer;
  scalar.offset = 0x40u;
  const auto key = fixture.Emit(ValueOpcode::ReadConstBuffer,
                                {material, selector}, fixture.AddMemory(scalar, 0x100));
  const auto record = fixture.Emit(ValueOpcode::IMul32, {key, Value(48u)});
  std::array<Value, 8> image_words;
  for (uint32_t dword = 0; dword < image_words.size(); dword++) {
    scalar.offset = 0x10u + dword * sizeof(uint32_t);
    image_words[dword] = fixture.Emit(ValueOpcode::ReadConstBuffer,
                                      {heap, record}, fixture.AddMemory(scalar, 0x200));
  }
  const auto image = fixture.Image(image_words);
  const auto sampler = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  fixture.Emit(ValueOpcode::ImageSampleRaw,
                {image, sampler, fixture.ImageAddress()}, fixture.AddMemory(sample, 0x228));
  fixture.PlanAndTrack();
  const auto plan = ExtractResourcePlan(fixture.program);

  Check(std::ranges::none_of(plan.descriptor_sources, [](const DescriptorSource& source) {
          return source.indirect_descriptor.has_value();
        }), "draw-uniform scalar image was expanded to GPU-selected candidates");
  std::array<uint32_t, 9> user_data{0x1000u, 0u, 0x100u, 0u,
                                    0x2000u, 0u, 0x100u, 0u, 0u};
  LinearTestMemory memory;
  memory.words[0x44u / 4u] = 1u;
  std::array<uint32_t, 8> descriptor{};
  descriptor[0] = 0x20u;
  descriptor[1] = static_cast<uint32_t>(
                      Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
                  << 20u;
  descriptor[2] = 3u | (3u << 14u);
  descriptor[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
                  (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
                   << 28u);
  for (uint32_t index = 0; index < 2; index++) {
    std::copy(descriptor.begin(), descriptor.end(),
              memory.words.begin() + (0x1010u + index * 48u) / 4u);
    memory.words[(0x1010u + index * 48u) / 4u] += index;
  }
  SrtRuntime runtime{.user_data = user_data,
                     .read_memory = ReadLinearTestMemory,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto materializes = [&](uint32_t address) {
    return MaterializeResources(plan, runtime, snapshot, specialization) &&
           snapshot.images.size() == 1 && snapshot.images[0].dwords[0] == address;
  };
  Check(materializes(0x20u), "nested draw-uniform image descriptor did not materialize");
  user_data[8] = 1u;
  Check(materializes(0x21u), "changed draw selector reused the previous image descriptor");
  memory.words[0x44u / 4u] = 0u;
  memory.words[0x1010u / 4u] = 0x30u;
  Check(materializes(0x30u), "changed scalar table memory reused the previous image descriptor");
  memory.fail_address = 0x1044u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "unavailable nested image table was accepted");
}

void TestComputeBufferFill() {
  struct Options {
    bool scalar = false;
    bool conditional = false;
    bool shifted = false;
    bool extra_store = false;
    bool clean = false;
    bool branch = false;
  };
  const auto Run = [](Options options) {
    Fixture fixture;
    fixture.program.blocks[0]->terminator.kind =
        Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
    if (options.branch) {
      fixture.program.blocks[0]->terminator.kind = Libs::Graphics::
          ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
    }
    const auto buffer =
        fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                        fixture.UserData(2), fixture.UserData(3)});
    const auto local = fixture.Emit(
        ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
         Value(0u)});
    const auto group = fixture.Emit(
        ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)), Value(0u)});
    auto index =
        fixture.Emit(ValueOpcode::IAdd32,
                     {local, fixture.Emit(ValueOpcode::ShiftLeftLogical32,
                                          {group, Value(6u)})});
    if (options.shifted)
      index = fixture.Emit(ValueOpcode::IAdd32, {index, Value(1u)});
    Value value(0u);
    TestMemory memory;
    memory.words[0] = 0x40404040u;
    if (options.scalar) {
      const auto input =
          fixture.Buffer({Value(static_cast<uint32_t>(memory.base)),
                          Value(4u << 16), Value(1u), Value(0x14204u)});
      MemoryInfo load;
      load.kind = ResourceKind::ScalarBuffer;
      value = fixture.Emit(ValueOpcode::ReadConstBuffer, {input, Value(0u)},
                           fixture.AddMemory(load, 8));
    }
    MemoryInfo store;
    store.kind = ResourceKind::Buffer;
    store.formatted = true;
    store.idxen = true;
    const auto flags = fixture.AddMemory(store, 16);
    const auto predicate =
        options.conditional
            ? fixture.Emit(ValueOpcode::ULessThan32, {local, Value(32u)})
            : Value(true);
    const auto EmitStore = [&] {
      fixture.Emit(ValueOpcode::StoreBufferU32,
                   {buffer, index, Value(0u), Value(0u), value, predicate},
                   flags);
    };
    EmitStore();
    if (options.extra_store)
      EmitStore();
    fixture.PlanAndTrack();
    auto plan = ExtractResourcePlan(fixture.program);
    std::array<uint32_t, 4> userdata{0x200000u, 4u << 16, 0x4000u, 0x14204u};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const auto Read = +[](void *data, uint64_t address, std::span<uint32_t> words) {
      auto &memory = *static_cast<TestMemory *>(data);
      if (address != memory.base || words.size() != 1u)
        return false;
      ++memory.reads;
      words[0] = memory.words[0];
      return true;
    };
    Check(MaterializeResources(
              plan,
              {.user_data = userdata,
               .read_memory = Read,
               .userdata = &memory,
               .read_specialization_memory = options.clean ? Read : nullptr},
              snapshot, specialization),
          "fill fixture did not materialize");
    const bool expected = !options.conditional && !options.shifted &&
                          !options.extra_store && !options.branch &&
                          (!options.scalar || options.clean);
    Check((snapshot.uniform_fill.words != 0) == expected,
          "fill proof accepted an unsafe store or missed the real GTA3 clear");
    if (expected) {
      Check(snapshot.uniform_fill.words == 1 &&
                snapshot.uniform_fill.group_stride[0] == 64 &&
                snapshot.uniform_fill.value ==
                    (options.scalar ? 0x40404040u : 0u),
            "fill proof lost address coverage or the actual stored scalar");
      if (options.scalar && options.clean) {
        Check(MaterializeResources(plan,
                  {.user_data = userdata, .read_memory = Read, .userdata = &memory},
                  snapshot, specialization) && snapshot.uniform_fill.words == 0,
              "an unavailable clean value retained a previous uniform fill");
      }
    }
  };
  Run({});
  Run({.scalar = true, .clean = true});
  Run({.scalar = true});
  Run({.conditional = true, .clean = true});
  Run({.shifted = true, .clean = true});
  Run({.extra_store = true, .clean = true});
  Run({.clean = true, .branch = true});
}

void TestDenseBufferTracking() {
  Fixture fixture;
  std::array<Value, 8> userdata;
  for (uint32_t index = 0; index < userdata.size(); index++) {
    userdata[index] = fixture.UserData(index);
  }
  const auto first =
      fixture.Buffer({userdata[0], userdata[1], userdata[2], userdata[3]}, 4);
  const auto second =
      fixture.Buffer({userdata[4], userdata[5], userdata[6], userdata[7]}, 28);

  MemoryInfo load_info;
  load_info.kind = ResourceKind::Buffer;
  load_info.offset = 4;
  load_info.formatted = true;
  const auto load_flags = fixture.AddMemory(load_info, 4);
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {first, Value(0u), Value(0u), Value(0u), Value(true)},
               load_flags);

  auto store_info = load_info;
  store_info.offset = 12;
  const auto store_flags = fixture.AddMemory(store_info, 8);
  fixture.Emit(ValueOpcode::StoreBufferU32,
               {first, Value(0u), Value(0u), Value(0u), Value(7u), Value(true)},
               store_flags);

  auto atomic_info = load_info;
  atomic_info.offset = 0;
  const auto atomic_flags = fixture.AddMemory(atomic_info, 12);
  fixture.Emit(ValueOpcode::BufferAtomicIAdd32,
               {first, Value(0u), Value(0u), Value(1u), Value(0u), Value(true)},
               atomic_flags);

  const auto other_flags = fixture.AddMemory(load_info, 28);
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {second, Value(0u), Value(0u), Value(0u), Value(true)},
               other_flags);
  fixture.PlanAndTrack();

  Check(fixture.program.info.buffers.size() == 2,
        "typed buffer sources were not densely interned");
  Check(fixture.program.descriptor_sources.size() == 2,
        "descriptor source table did not match dense topology");
  const auto &resource = fixture.program.info.buffers[0];
  Check(resource.read && resource.written && resource.atomic &&
            resource.formatted && resource.max_byte_extent == 16 &&
            resource.first_use_pc == 4,
        "buffer access facts were not merged");
  Check(first.Instruction()->Flags<uint32_t>() == 0 &&
            second.Instruction()->Flags<uint32_t>() == 1,
        "typed handles were not assigned dense indices");
  Check(fixture.program.memory_info[load_flags.index].resource == 0 &&
            fixture.program.memory_info[store_flags.index].resource == 0 &&
            fixture.program.memory_info[other_flags.index].resource == 1,
        "typed memory metadata was not patched to dense indices");

  CheckFatal([&] { fixture.PlanAndTrack(); }, "already tracked",
             "resource tracking allowed a second mutation pass");
}

void TestScalarAndVectorBufferAlias() {
  Fixture fixture;
  const auto d0 = fixture.UserData(0);
  const auto d1 = fixture.UserData(1);
  const auto d2 = fixture.UserData(2);
  const auto d3 = fixture.UserData(3);
  const auto descriptor = fixture.Buffer({d0, d1, d2, d3}, 4);

  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarBuffer;
  const auto scalar_flags = fixture.AddMemory(scalar, 4);
  fixture.Emit(ValueOpcode::ReadConstBuffer, {descriptor, fixture.UserData(4)},
               scalar_flags);
  MemoryInfo vector;
  vector.kind = ResourceKind::Buffer;
  const auto vector_flags = fixture.AddMemory(vector, 8);
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {descriptor, Value(0u), Value(0u), Value(0u), Value(true)},
               vector_flags);
  fixture.PlanAndTrack();

  Check(fixture.program.info.buffers.size() == 1 &&
            fixture.program.info.buffers[0].scalar,
        "typed scalar and vector uses of one descriptor were split");
  Check(fixture.program.memory_info[scalar_flags.index].resource == 0 &&
            fixture.program.memory_info[vector_flags.index].resource == 0,
        "scalar/vector alias did not share a dense index");
}

void TestRuntimeUnsignedMinDescriptor() {
  Fixture fixture;
  const auto word3 =
      fixture.Emit(ValueOpcode::UMin32, {fixture.UserData(0), Value(0x100u)});
  const auto descriptor =
      fixture.Buffer({Value(0u), Value(0u), Value(64u), word3}, 0x330);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {descriptor, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(memory, 0x330));
  fixture.PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.program);

  std::array<uint32_t, 1> user_data{0xffffffffu};
  SrtRuntime runtime{.user_data = user_data};
  DescriptorValue value;
  const auto source = fixture.program.info.buffers[0].source;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(source, value) &&
            value.dwords[3] == 0x100u,
        "runtime descriptor unsigned minimum did not clamp its first operand");
  user_data[0] = 0x80u;
  Check(
      SrtWalker(plan, runtime).EvaluateDescriptor(source, value) &&
          value.dwords[3] == 0x80u,
      "runtime descriptor unsigned minimum did not preserve its first operand");
}

void TestImagesSamplersAndAliases() {
  Fixture fixture;
  std::array<Value, 8> image_words;
  for (uint32_t index = 0; index < image_words.size(); index++) {
    image_words[index] = fixture.UserData(index);
  }
  const auto image_address = fixture.ImageAddress();
  const std::array<Value, 4> sampler0{Value(0u), Value(1u), Value(2u),
                                      Value(0x1111u)};
  const std::array<Value, 4> sampler1{Value(0u), Value(1u), Value(2u),
                                      Value(0x2222u)};

  auto AddSample = [&](uint32_t pc, uint32_t sample_flags,
                       const auto &sampler_words) {
    const auto image = fixture.Image(image_words, pc);
    const auto sampler = fixture.Sampler(sampler_words, pc);
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    memory.image_sample_flags = sample_flags;
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, image_address},
                 fixture.AddMemory(memory, pc));
    return std::pair{image, sampler};
  };
  const auto normal = AddSample(4, 0, sampler0);
  const auto repeated = AddSample(8, 0, sampler1);
  const auto compare = AddSample(12, Decoder::ImageSampleFlagCompare, sampler0);

  const auto storage = fixture.Image(image_words, 16);
  MemoryInfo storage_memory;
  storage_memory.kind = ResourceKind::Image;
  storage_memory.image_dimension = Decoder::ImageDimension::Dim2D;
  fixture.Emit(ValueOpcode::ImageAtomicIAdd32,
               {storage, image_address, Value(1u), Value(true)},
               fixture.AddMemory(storage_memory, 16));

  const auto buffer = fixture.Buffer(
      {image_words[0], image_words[1], image_words[2], image_words[3]}, 20);
  MemoryInfo buffer_memory;
  buffer_memory.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {buffer, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(buffer_memory, 20));
  fixture.PlanAndTrack();

  Check(fixture.program.info.images.size() == 3 &&
            fixture.program.info.samplers.size() == 1 &&
            fixture.program.info.sampled_pairs.size() == 2,
        "typed image view classes or samplers were deduplicated incorrectly");
  Check(normal.first.Instruction()->Flags<uint32_t>() ==
                repeated.first.Instruction()->Flags<uint32_t>() &&
            compare.first.Instruction()->Flags<uint32_t>() !=
                normal.first.Instruction()->Flags<uint32_t>(),
        "image handles did not receive view-class indices");
  Check(normal.second.Instruction()->Flags<uint32_t>() == 0 &&
            repeated.second.Instruction()->Flags<uint32_t>() == 0,
        "unused sampler border colors prevented source interning");
  const auto sampler_source = fixture.program.info.samplers[0].source;
  Check(fixture.program.descriptor_sources[sampler_source].dwords[3].U32() == 0,
        "unused sampler border color was not canonicalized");
  Check(fixture.program.info.buffers[0].image_alias == 0,
        "buffer/image descriptor alias was not linked");
}

void TestSampleAdjustSamplerScratch() {
  Fixture fixture(ShaderType::Pixel);
  const auto active = fixture.Emit(
      ValueOpcode::IEqual32, {fixture.Emit(ValueOpcode::LaneId), Value(0u)});
  const auto lane =
      fixture.Emit(ValueOpcode::SelectU32, {active, Value(1u), Value(0u)});
  const auto low =
      fixture.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(0xffu)});
  const auto high =
      fixture.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(0xffu)});
  const auto quads = fixture.Emit(
      ValueOpcode::BitwiseOr32,
      {low, fixture.Emit(ValueOpcode::ShiftLeftLogical32, {high, Value(8u)})});
  const auto scratch =
      fixture.Emit(ValueOpcode::ShiftLeftLogical32, {quads, Value(12u)});
  const auto word3 =
      fixture.Emit(ValueOpcode::BitwiseOr32, {fixture.UserData(3), scratch});
  const auto image = fixture.Image({Value(0u), Value(0u), Value(0u), Value(0u),
                                    Value(0u), Value(0u), Value(0u), Value(0u)},
                                   0x1ec);
  const auto sampler = fixture.Sampler(
      {fixture.UserData(0), fixture.UserData(1), fixture.UserData(2), word3},
      0x1ec);
  MemoryInfo memory;
  memory.kind = ResourceKind::Image;
  memory.image_dimension = Decoder::ImageDimension::Dim2D;
  memory.image_sample_flags = Decoder::ImageSampleFlagAdjust;
  fixture.Emit(ValueOpcode::ImageSampleRaw,
               {image, sampler, fixture.ImageAddress()},
               fixture.AddMemory(memory, 0x1ec));
  // PPSA24156's zero border payload leaves the reserved scratch shift as
  // the entire DWORD after the frontend folds its OR with zero.
  const auto shift_sampler = fixture.Sampler(
      {Value(0x36u), Value(0xfff000u), Value(0x02500000u), scratch}, 0x200);
  fixture.Emit(ValueOpcode::ImageSampleRaw,
               {image, shift_sampler, fixture.ImageAddress()},
               fixture.AddMemory(memory, 0x200));
  fixture.PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.program);

  const auto source = fixture.program.info.samplers[0].source;
  const auto stored = fixture.program.descriptor_sources[source]
                          .dwords[3]
                          .Resolve()
                          .TryInstruction();
  Check(stored != nullptr && stored->GetOpcode() == ValueOpcode::GetUserData,
        "SampleAdjust reserved scratch remained in sampler identity");
  std::array<uint32_t, 4> user_data{4u, 1u, 2u, 0x80000abcu};
  SrtRuntime runtime{.user_data = user_data};
  DescriptorValue descriptor;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(source, descriptor) &&
            descriptor.dwords[3] == 0x80000abcu,
        "SampleAdjust canonicalization lost sampler border fields");
  const auto shift_source = fixture.program.info.samplers[
      shift_sampler.Instruction()->Flags<uint32_t>()].source;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(shift_source, descriptor) &&
            std::ranges::equal(std::span(descriptor.dwords).first(4),
                               std::array{0x36u, 0xfff000u, 0x02500000u, 0u}),
        "SampleAdjust shift-only scratch was not reduced to its zero border payload");

  const auto CheckRejected = [](uint32_t flags, uint32_t shift,
                                const char *message) {
    Fixture rejected(ShaderType::Pixel);
    const auto condition = rejected.Emit(
        ValueOpcode::IEqual32, {rejected.Emit(ValueOpcode::LaneId), Value(0u)});
    const auto bit = rejected.Emit(ValueOpcode::SelectU32,
                                   {condition, Value(1u), Value(0u)});
    const auto dynamic =
        rejected.Emit(ValueOpcode::ShiftLeftLogical32, {bit, Value(shift)});
    const auto dynamic_word3 = rejected.Emit(ValueOpcode::BitwiseOr32,
                                             {rejected.UserData(3), dynamic});
    const auto rejected_image =
        rejected.Image({Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
                        Value(0u), Value(0u), Value(0u)},
                       0x200);
    const auto rejected_sampler =
        rejected.Sampler({rejected.UserData(0), rejected.UserData(1),
                          rejected.UserData(2), dynamic_word3},
                         0x200);
    MemoryInfo rejected_memory;
    rejected_memory.kind = ResourceKind::Image;
    rejected_memory.image_dimension = Decoder::ImageDimension::Dim2D;
    rejected_memory.image_sample_flags = flags;
    rejected.Emit(ValueOpcode::ImageSampleRaw,
                  {rejected_image, rejected_sampler, rejected.ImageAddress()},
                  rejected.AddMemory(rejected_memory, 0x200));

    CheckFatal([&] { rejected.PlanAndTrack(); },
               "not a valid runtime value", message);
  };
  CheckRejected(0u, 12u,
                "ordinary sampling accepted SampleAdjust reserved scratch");
  CheckRejected(Decoder::ImageSampleFlagAdjust, 30u,
                "SampleAdjust canonicalization discarded border-mode bits");
}

void TestFmaskLoadSpecialization() {
  namespace Prospero = Libs::Graphics::Prospero;
  Fixture fixture;
  std::array<Value, 8> words;
  for (uint32_t i = 0; i < words.size(); i++) {
    words[i] = fixture.UserData(i);
  }
  const auto fmask = fixture.Image(words, 4);
  const auto active = fixture.Emit(ValueOpcode::IEqual32,
                                    {fixture.UserData(8), Value(0u)});
  MemoryInfo load;
  load.kind = ResourceKind::Image;
  load.image_dimension = Decoder::ImageDimension::Dim2D;
  load.image_address_components = 2;
  load.dmask = 1;
  const auto mapping = fixture.Emit(
      ValueOpcode::ImageRead, {fmask, fixture.ImageAddress(), active},
      fixture.AddMemory(load, 4));
  const auto ordinary = fixture.Image(
      {Value(0x2000u),
       Value(static_cast<uint32_t>(Prospero::BufferFormat::k8UInt) << 20u),
       Value(3u | (3u << 14u)),
       Value(Libs::Graphics::DstSel(4, 5, 6, 7) |
             (static_cast<uint32_t>(Prospero::ImageType::kColor2D) << 28u)),
       Value(0u), Value(0u), Value(0u), Value(0u)}, 8);
  const auto ordinary_flags = fixture.AddMemory(load, 8);
  const auto color = fixture.Emit(
      ValueOpcode::ImageRead, {ordinary, fixture.ImageAddress(), Value(true)},
      ordinary_flags);
  const auto output = fixture.Buffer(
      {Value(0x3000u), Value(0u), Value(12u), Value(0u)}, 12);
  MemoryInfo store;
  store.kind = ResourceKind::Buffer;
  Value result;
  for (uint32_t i = 0; i < 2; i++) {
    const auto value = fixture.Emit(
        ValueOpcode::CompositeExtractU32x4,
        {i == 0 ? mapping : color, Value(0u)});
    fixture.Emit(ValueOpcode::StoreBufferU32,
                 {output, Value(0u), Value(i * 4u), Value(0u), value, Value(true)},
                 fixture.AddMemory(store, 12 + i * 4u));
    if (i == 0) result = value;
  }
  fixture.PlanAndTrack();
  const auto plan = ExtractResourcePlan(fixture.program);
  std::array<uint32_t, 9> user_data{
      0x303ac300u, 0xca100000u, 0x021bc3bfu, 0x91800004u,
      0u, 0x00700000u, 0u, 0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, {.user_data = user_data}, snapshot,
                             specialization),
        "FMASK resources did not materialize");
  ApplyResourceSpecialization(fixture.program, specialization);
  RemoveIdentities(fixture.program.blocks);
  EliminateDeadCode(fixture.program.blocks);
  Check(fixture.program.info.images.size() == 1 && snapshot.images.size() == 1 &&
            snapshot.images[0].dwords[0] == 0x2000u &&
            ordinary.Instruction()->Flags<uint32_t>() == 0 &&
            fixture.program.memory_info[ordinary_flags.index].resource == 0,
        "FMASK removal did not preserve the remaining image and runtime descriptor");
  const auto *vector = result.Instruction()->Arg(0).Resolve().TryInstruction();
  Check(vector != nullptr &&
            vector->GetOpcode() == ValueOpcode::CompositeConstructU32x4,
        "FMASK load did not lower to a value vector");
  result = vector->Arg(0);
  uint32_t value = 0;
  Check(SrtWalker(fixture.program, {.user_data = user_data}).Evaluate(result, value) &&
            value == 0x76543210u,
        "FMASK load did not return the native sample-to-fragment mapping");
  user_data[8] = 1;
  Check(SrtWalker(fixture.program, {.user_data = user_data}).Evaluate(result, value) &&
            value == 0u,
        "inactive FMASK load did not preserve the execution mask");
  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(DescriptorBindingForImage(fixture.program.info.images[0]).has_value(),
        "FMASK removal lost the ordinary image class");
  user_data[8] = 0;
  user_data[1] = static_cast<uint32_t>(Prospero::BufferFormat::k8UInt) << 20u;
  ResourceSpecialization rebound;
  Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, rebound) &&
            rebound != specialization && snapshot.images.size() == 2,
        "rebinding FMASK as a texture reused the metadata specialization");
}

void TestGatherLodSamplerValidation() {
  for (const bool explicit_lod : {false, true}) {
    for (const bool reverse : {false, true}) {
      Fixture fixture;
      std::array<Value, 8> image_words;
      std::array<Value, 4> sampler_words;
      for (uint32_t i = 0; i < image_words.size(); i++)
        image_words[i] = fixture.UserData(i);
      for (uint32_t i = 0; i < sampler_words.size(); i++)
        sampler_words[i] = fixture.UserData(i + 8);
      for (uint32_t i = 0; i < 2; i++) {
        const bool lod = explicit_lod && i == (reverse ? 0u : 1u);
        MemoryInfo memory;
        memory.kind = ResourceKind::Image;
        memory.image_dimension = Decoder::ImageDimension::Dim2D;
        memory.image_sample_flags = lod ? Decoder::ImageSampleFlagLod
                                        : Decoder::ImageSampleFlagLevelZero;
        fixture.Emit(ValueOpcode::ImageGatherRaw,
                     {fixture.Image(image_words), fixture.Sampler(sampler_words),
                      fixture.ImageAddress(), image_words[1], image_words[3],
                      sampler_words[1], sampler_words[2]},
                     fixture.AddMemory(memory, i * 4));
      }
      fixture.PlanAndTrack();
      auto plan = ExtractResourcePlan(fixture.program);
      Check(plan.info.samplers.size() == 1 &&
                plan.info.samplers[0].gather_lod == explicit_lod,
            "shared sampler lost explicit gather validation or restricted LZ gather");
      std::array<uint32_t, 12> user_data{};
      user_data[0] = 0x1000u;
      user_data[1] = static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32Float) << 20u;
      user_data[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
          (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
      user_data[9] = 0xfff000u;
      constexpr std::array sampler_controls{
          0u, 0x4001u, 1u << 26u, (1u << 26u) | 256u,
          (1u << 26u) | (1u << 14u), 2u << 26u, 3u << 26u};
      for (const auto control : sampler_controls) {
        user_data[10] = control;
        ResourceSnapshot snapshot;
        ResourceSpecialization specialization;
        const bool supported = !explicit_lod || (control >> 26u) == 0 ||
                               control == (1u << 26u);
        Check(MaterializeResources(plan, {.user_data = user_data}, snapshot,
                                   specialization) == supported,
              "explicit gather accepted an unsupported sampler or rejected a valid one");
      }
    }
  }
}

void TestDynamicStorageMipTracking() {
  Fixture fixture;
  std::array<Value, 8> image_words;
  for (uint32_t index = 0; index < image_words.size(); index++) {
    image_words[index] = fixture.UserData(index);
  }
  const auto data = fixture.Emit(ValueOpcode::CompositeConstructU32x4,
                                 {Value(1u), Value(2u), Value(3u), Value(4u)});
  const auto AddStore = [&](uint32_t pc, bool has_mip, Value lod) {
    const auto handle = fixture.Image(image_words, pc);
    const auto address = fixture.Emit(
        ValueOpcode::MakeImageAddress,
        {Value(0u), Value(0u), lod, Value(0u), Value(0u), Value(0u), Value(0u),
         Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u)});
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    memory.image_address_components = has_mip ? 3u : 2u;
    memory.image_has_mip = has_mip;
    const auto flags = fixture.AddMemory(memory, pc);
    fixture.Emit(ValueOpcode::ImageWrite, {handle, address, data, Value(true)},
                 flags);
    return std::pair{handle, flags.index};
  };

  const auto plain = AddStore(4, false, Value(0u));
  const auto mip1 = AddStore(8, true, Value(1u));
  const auto mip2 = AddStore(12, true, Value(2u));
  const auto dynamic = AddStore(16, true, fixture.UserData(8));
  fixture.PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture.program);

  const auto &images = fixture.program.info.images;
  Check(images.size() == 2 && images[0].mip_mode == ImageMipMode::None &&
            images[0].mip_count == 1 &&
            images[1].mip_mode == ImageMipMode::Dynamic &&
            images[1].mip_count == 1,
        "storage mip writes did not share one dynamic logical resource");
  Check(plain.first.Instruction()->Flags<uint32_t>() == 0 &&
            mip1.first.Instruction()->Flags<uint32_t>() == 1 &&
            mip2.first.Instruction()->Flags<uint32_t>() == 1 &&
            dynamic.first.Instruction()->Flags<uint32_t>() == 1 &&
            fixture.program.memory_info[plain.second].resource == 0 &&
            fixture.program.memory_info[mip1.second].resource == 1 &&
            fixture.program.memory_info[mip2.second].resource == 1 &&
            fixture.program.memory_info[dynamic.second].resource == 1,
        "dynamic storage mip handles and memory metadata were not patched");

  DescriptorValue descriptor{};
  descriptor.dwords[0] = 0x1000u;
  descriptor.dwords[1] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
      << 20u;
  descriptor.dwords[2] = 3u | (3u << 14u);
  descriptor.dwords[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) | (1u << 12u) | (3u << 16u) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
       << 28u);
  descriptor.dwords[5] = 3u << 4u;
  descriptor.dword_count = 8;
  std::array<uint32_t, 9> user_data{};
  std::copy(descriptor.dwords.begin(), descriptor.dwords.end(),
            user_data.begin());
  user_data[8] = 2u;
  SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot,
                             specialization),
        "dynamic storage resources did not materialize");
  ApplyResourceSpecialization(fixture.program, specialization);
  Check(fixture.program.info.images[1].mip_count == 3 &&
            snapshot.images.size() == fixture.program.info.images.size(),
        "base-1 through last-3 dynamic storage range was not specialized");
  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(DescriptorBindingForImage(images[0]) == DescriptorBindingForImage(images[1]) &&
            images[0].mip_count == 1 && images[1].mip_mode == ImageMipMode::Dynamic,
        "dynamic storage mip metadata did not retain one homogeneous image class");

  Fixture null_fixture;
  const auto null_handle = null_fixture.Image(
      {Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
       Value(0u), Value(0u)});
  const auto null_address = null_fixture.Emit(
      ValueOpcode::MakeImageAddress,
      {Value(0u), Value(0u), null_fixture.UserData(0), Value(0u), Value(0u),
       Value(0u), Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
       Value(0u), Value(0u)});
  MemoryInfo null_memory;
  null_memory.kind = ResourceKind::Image;
  null_memory.image_dimension = Decoder::ImageDimension::Dim2D;
  null_memory.image_address_components = 3u;
  null_memory.image_has_mip = true;
  const auto null_data = null_fixture.Emit(
      ValueOpcode::CompositeConstructU32x4,
      {Value(1u), Value(2u), Value(3u), Value(4u)});
  null_fixture.Emit(ValueOpcode::ImageWrite,
                    {null_handle, null_address, null_data, Value(true)},
                    null_fixture.AddMemory(null_memory, 4));
  null_fixture.PlanAndTrack();
  auto null_plan = ExtractResourcePlan(null_fixture.program);
  ResourceSnapshot null_snapshot;
  ResourceSpecialization null_specialization;
  const std::array<uint32_t, 1> null_user_data{0u};
  Check(MaterializeResources(null_plan, {.user_data = null_user_data},
                             null_snapshot, null_specialization),
        "canonical null dynamic storage image did not materialize");
  ApplyResourceSpecialization(null_fixture.program, null_specialization);
  Check(null_fixture.program.info.images[0].mip_count == 1 &&
            null_snapshot.images.size() == 1,
        "canonical null dynamic storage image did not retain one descriptor");

  auto changed_user_data = user_data;
  changed_user_data[3] =
      (changed_user_data[3] & ~(0xfu << 16u)) | (2u << 16u);
  ResourceSnapshot changed_snapshot;
  ResourceSpecialization changed_specialization;
  Check(MaterializeResources(resource_plan, {.user_data = changed_user_data},
                             changed_snapshot, changed_specialization) &&
            changed_specialization != specialization,
        "a changed dynamic storage mip count reused the specialization key");
  changed_user_data[3] =
      (changed_user_data[3] & ~((0xfu << 12u) | (0xfu << 16u))) |
      (4u << 12u) | (3u << 16u);
  Check(!MaterializeResources(resource_plan, {.user_data = changed_user_data},
                              changed_snapshot, changed_specialization),
        "an inverted dynamic storage mip range was accepted");
}

void TestSrtFlatteningAndRuntimeMemoization() {
  Fixture fixture;
  const auto base =
      fixture.Address(fixture.UserData(0), fixture.UserData(1), 4);
  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarAddress;
  scalar.offset = 4;
  const auto read0 = fixture.Emit(ValueOpcode::LoadAddressU32,
                                  {base, Value(0u), Value(0u), Value(true)},
                                  fixture.AddMemory(scalar, 4));
  const auto descriptor0 =
      fixture.Buffer({read0, Value(0u), Value(64u), Value(0u)}, 12);
  const auto descriptor1 =
      fixture.Buffer({read0, Value(0u), Value(64u), Value(0u)}, 16);
  MemoryInfo buffer;
  buffer.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {descriptor0, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(buffer, 12));
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {descriptor1, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(buffer, 16));
  fixture.PlanAndTrack();

  Check(fixture.program.srt_reads.size() == 1,
        "shared typed scalar read did not receive one flat SRT slot");
  Check(fixture.program.info.buffers.size() == 1 &&
            !fixture.program.info.uses_dma,
        "planning-only scalar reads leaked into resource topology");
  Check(fixture.program.memory_info[0].planning_only,
        "canonical runtime scalar read was not marked planning-only");

  std::array<uint32_t, 2> user_data{0x1000u, 0u};
  TestMemory memory;
  memory.words[1] = 0xdeadbeefu;
  SrtRuntime runtime{.user_data = user_data,
                     .read_memory = ReadTestMemory,
                     .userdata = &memory};
  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue descriptor;
  std::vector<uint32_t> flat;
  const uint32_t request = fixture.program.info.buffers[0].source;
  const auto refresh = [&](const ResourcePlan& plan) {
    SrtWalker walker(plan, runtime);
    return walker.EvaluateDescriptor(request, descriptor) && walker.RefreshFlatBuffer(flat);
  };
  Check(refresh(plan), "typed runtime source evaluation failed");
  Check(descriptor.dwords[0] == 0xdeadbeefu &&
            flat == std::vector<uint32_t>{0xdeadbeefu} && memory.reads == 1,
        "descriptor and flat SRT evaluation did not share one memoized read");

  memory.reads = 0;
  memory.words[1] = 0x12345678u;
  Check(refresh(plan) && descriptor.dwords[0] == 0x12345678u &&
            flat == std::vector<uint32_t>{0x12345678u} && memory.reads == 1,
        "repeated runtime evaluation reused stale scalar memory");

  memory.reads = 0;
  memory.fail_after = 0;
  Check(!refresh(plan), "unavailable scalar memory was accepted");
  memory.fail_after = UINT32_MAX;
  Check(refresh(plan) && descriptor.dwords[0] == 0x12345678u && memory.reads == 1,
        "failed runtime evaluation left a value marked as visiting");

  auto moved = std::move(plan);
  memory.reads = 0;
  memory.words[1] = 0x87654321u;
  Check(refresh(moved) && descriptor.dwords[0] == 0x87654321u && memory.reads == 1,
        "moving a cached resource plan lost its evaluation state");

  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(fixture.program.info.uses_flattened_srt,
        "flattened typed SRT reads were not collected");
}

void TestDynamicSrtReadRemainsExplicit() {
  Fixture fixture;
  const auto base =
      fixture.Address(fixture.UserData(0), fixture.UserData(1), 4);
  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarAddress;
  const auto read =
      fixture.Emit(ValueOpcode::LoadAddressU32,
                   {base, fixture.UserData(2), Value(0u), Value(true)},
                   fixture.AddMemory(scalar, 4));
  const auto descriptor =
      fixture.Buffer({read, Value(0u), Value(64u), Value(0u)}, 8);
  MemoryInfo buffer;
  buffer.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {descriptor, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(buffer, 8));
  fixture.PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.program);

  Check(fixture.program.srt_reads.empty() &&
            read.ResolveInstruction()->GetOpcode() == ValueOpcode::LoadAddressU32 &&
            !fixture.program.memory_info[0].planning_only &&
            fixture.program.info.uses_dma,
        "dynamic scalar read was incorrectly flattened or lost");
  std::array<uint32_t, 3> user_data{0x1000u, 0u, 4u};
  TestMemory memory;
  memory.words[1] = 0xabcdef01u;
  SrtRuntime runtime{.user_data = user_data,
                     .read_memory = ReadTestMemory,
                     .userdata = &memory};
  DescriptorValue value;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(fixture.program.info.buffers[0].source, value) &&
            value.dwords[0] == 0xabcdef01u && memory.reads == 1,
        "dynamic typed scalar descriptor source was not evaluated");

  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(!fixture.program.info.uses_flattened_srt && fixture.program.info.uses_dma &&
            fixture.program.info.live_buffers == 1u,
        "dynamic scalar read received the wrong resource usage");
}

void TestWritableDescriptorPhi() {
  Fixture fixture;
  auto *left = fixture.block;
  auto *right = fixture.AddBlock();
  auto *merge = fixture.AddBlock();
  left->AddBranch(merge);
  right->AddBranch(merge);
  auto &phi = merge->AppendNewInst(ValueOpcode::Phi, {},
                                   static_cast<uint64_t>(Type::U32));
  phi.AddPhiOperand(left, Value(1u));
  phi.AddPhiOperand(right, Value(2u));
  const auto word3 =
      fixture.Emit(ValueOpcode::UMin32, {Value(&phi), Value(0x100u)}, 0, merge);
  const auto handle = fixture.Emit(ValueOpcode::GetBufferResource,
                                   {Value(0u), Value(0u), Value(0u), word3},
                                   MemoryFlags{0, 20}, merge);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::StoreBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
               fixture.AddMemory(memory, 20), merge);

  CheckFatal([&] { fixture.PlanAndTrack(); }, "not a valid runtime value",
             "control-dependent writable descriptor phi was accepted");
  Check(!fixture.program.resource_tracking_complete &&
            fixture.program.info.buffers.empty() &&
            fixture.program.descriptor_sources.empty(),
        "control-dependent writable descriptor phi was not rejected transactionally");
}

ResourcePlan ConditionalSamplerPlan(bool diamond, bool reverse, bool reverse_phi,
                                    bool nonuniform = false,
                                    bool writable = false) {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  Fixture fixture(ShaderType::Pixel);
  auto *entry = fixture.block;
  auto *initial = diamond ? fixture.AddBlock() : entry;
  auto *alternate = fixture.AddBlock();
  auto *merge = fixture.AddBlock();
  const uint32_t alternate_id = diamond ? 2u : 1u;
  const uint32_t merge_id = alternate_id + 1;
  const uint32_t initial_target = diamond ? 1u : merge_id;
  entry->AddBranch(alternate);
  entry->AddBranch(diamond ? initial : merge);
  alternate->AddBranch(merge);
  if (diamond) {
    initial->AddBranch(merge);
    fixture.program.blocks[1]->terminator = {
        .kind = CFG::TerminatorKind::Branch,
        .true_block = fixture.program.blocks[merge_id]};
  }
  fixture.program.blocks[0]->terminator = {
      .kind = CFG::TerminatorKind::ConditionalBranch,
      .true_block =
          fixture.program.blocks[reverse ? alternate_id : initial_target],
      .false_block =
          fixture.program.blocks[reverse ? initial_target : alternate_id]};
  fixture.program.blocks[alternate_id]->terminator = {
      .kind = CFG::TerminatorKind::Branch,
      .true_block = fixture.program.blocks[merge_id]};
  fixture.program.blocks[merge_id]->terminator.kind =
      CFG::TerminatorKind::Return;
  const auto control =
      fixture.Buffer({Value(0x2000u), Value(0u), Value(200u), Value(0u)});
  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarBuffer;
  auto flag = fixture.Emit(ValueOpcode::ReadConstBuffer, {control, Value(196u)},
                           fixture.AddMemory(scalar, 0x498));
  if (nonuniform) {
    flag = fixture.Emit(ValueOpcode::LaneId);
  }
  fixture.program.blocks[0]->condition =
      fixture.Emit(ValueOpcode::SGreaterThanEqual32, {flag, Value(0u)});
  if (writable) {
    MemoryInfo memory;
    memory.kind = ResourceKind::Buffer;
    fixture.Emit(
        ValueOpcode::StoreBufferU32,
        {control, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
        fixture.AddMemory(memory, 0x170));
  }

  std::array<Value, 4> sampler_words;
  for (uint32_t word = 0; word < sampler_words.size(); ++word) {
    const auto read = [&](Block *block, uint32_t address, uint32_t pc) {
      const auto handle = fixture.Emit(ValueOpcode::GetAddressResource,
                                       {Value(address), Value(0u)}, 0, block);
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = word * 4;
      return fixture.Emit(ValueOpcode::LoadAddressU32,
                          {handle, Value(0u), Value(0u), Value(true)},
                          fixture.AddMemory(memory, pc), block);
    };
    // PS 2190adcc312b2e6e selects SRT+448 or SRT+480 before its sample at
    // 0x4d4.
    const auto first = read(initial, 0x1000 + 448, 0x4c8);
    const auto second = read(alternate, 0x1000 + 480, 0x4bc);
    auto &phi = merge->AppendNewInst(ValueOpcode::Phi, {},
                                     static_cast<uint64_t>(Type::U32));
    if (reverse_phi) {
      phi.AddPhiOperand(alternate, second);
      phi.AddPhiOperand(initial, first);
    } else {
      phi.AddPhiOperand(initial, first);
      phi.AddPhiOperand(alternate, second);
    }
    sampler_words[word] = Value(&phi);
  }
  fixture.block = merge;
  const auto image =
      fixture.Image({Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
                     Value(0u), Value(0u), Value(0u)});
  const auto address = fixture.ImageAddress();
  for (uint32_t use = 0; use < 2; ++use) {
    const auto sampler = fixture.Sampler(sampler_words);
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    const auto result =
        fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, address},
                     fixture.AddMemory(sample, 0x4d4));
    fixture.Emit(ValueOpcode::ReferenceU32,
                 {fixture.Emit(ValueOpcode::CompositeExtractU32x4,
                               {result, Value(0u)})});
  }
  fixture.PlanAndTrack();
  Check(std::ranges::count_if(fixture.program.value_storage, [](const Inst &inst) {
          return inst.GetOpcode() == ValueOpcode::SelectU32;
        }) == 4,
        "repeated sampler uses retained duplicate planning selections");
  Check(sampler_words[0].ResolveInstruction()->GetOpcode() == ValueOpcode::Phi,
        "host descriptor selection changed the GPU Phi");
  EliminateDeadCode(fixture.program.blocks);
  ValidateProgram(fixture.program, true);
  return ExtractResourcePlan(fixture.program);
}

void TestConditionalSamplerPhi() {
  for (const bool diamond : {false, true}) {
    for (const bool reverse : {false, true}) {
      for (const bool reverse_phi : {false, true}) {
        auto plan = ConditionalSamplerPlan(diamond, reverse, reverse_phi);
        const auto source = plan.info.samplers[0].source;
        LinearTestMemory memory;
        for (uint32_t word = 448 / 4; word < (480 + 16) / 4; ++word) {
          memory.words[word] = 0x400u + word;
        }
        const SrtRuntime runtime{.read_memory = ReadLinearTestMemory,
                                 .userdata = &memory,
                                 .read_specialization_memory =
                                     ReadLinearTestMemory};
        // Sampler selection compares a signed value against zero.
        for (const auto flag : {-1, 0, 1, INT32_MIN, INT32_MAX}) {
          memory.words[(0x1000 + 196) / 4] = std::bit_cast<uint32_t>(flag);
          const uint32_t first = (flag < 0) != reverse ? 480 / 4 : 448 / 4;
          memory.fail_address = 0x1000 + (first == 448 / 4 ? 480u : 448u);
          DescriptorValue selected;
          SrtWalker clean(plan, CleanRuntime(runtime));
          Check(SrtWalker(plan, runtime, &clean).EvaluateDescriptor(source, selected),
                "conditional sampler did not survive detached plan lifetime");
          for (uint32_t word = 0; word < 4; ++word) {
            Check(selected.dwords[word] == memory.words[first + word],
                  "conditional sampler chose the wrong incoming descriptor");
          }
        }
        DescriptorValue selected;
        auto no_clean_reader = runtime;
        no_clean_reader.read_specialization_memory = nullptr;
        {
          SrtWalker clean(plan, CleanRuntime(no_clean_reader));
          Check(!SrtWalker(plan, no_clean_reader, &clean).EvaluateDescriptor(source, selected),
                "conditional sampler used unchecked memory for its predicate");
        }
        memory.fail_address = 0x2000 + 196;
        {
          SrtWalker clean(plan, CleanRuntime(runtime));
          Check(!SrtWalker(plan, runtime, &clean).EvaluateDescriptor(source, selected),
                "conditional sampler ignored unavailable coherent predicate memory");
        }
      }
    }
    CheckFatal([&] { ConditionalSamplerPlan(diamond, false, false, true); },
               "not a valid runtime value",
               "nonuniform sampler selection was accepted");
    CheckFatal(
        [&] { ConditionalSamplerPlan(diamond, false, false, false, true); },
        "not a valid runtime value",
        "shader-written sampler predicate was accepted");
  }
}

void TestFiniteImagePhiCycle() {
  Fixture fixture;
  auto *entry = fixture.block;
  auto *header = fixture.AddBlock();
  auto *reload = fixture.AddBlock();
  auto *merge = fixture.AddBlock();
  entry->AddBranch(header);
  header->AddBranch(reload);
  header->AddBranch(merge);
  reload->AddBranch(merge);
  merge->AddBranch(header);
  std::array<Value, 8> words;
  for (uint32_t word = 0; word < words.size(); ++word) {
    auto &loop = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    auto &choice = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    loop.AddPhiOperand(entry, fixture.UserData(word));
    loop.AddPhiOperand(merge, Value(&choice));
    // Incoming order is independent for each descriptor DWORD.
    if (word & 1u) {
      choice.AddPhiOperand(reload, fixture.UserData(word + 8));
      choice.AddPhiOperand(header, Value(&loop));
    } else {
      choice.AddPhiOperand(header, Value(&loop));
      choice.AddPhiOperand(reload, fixture.UserData(word + 8));
    }
    words[word] = Value(&choice);
  }
  fixture.block = merge;
  const auto image = fixture.Image(words);
  const auto sampler = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
  MemoryInfo memory;
  memory.kind = ResourceKind::Image;
  memory.image_dimension = Decoder::ImageDimension::Dim2D;
  fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress()},
               fixture.AddMemory(memory, 4));
  fixture.PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.program);
  const auto source = fixture.program.info.images[0].source;
  const auto &finite = fixture.program.descriptor_sources[source].indirect_descriptor;
  Check(finite && finite->sources.size() == 2,
        "cyclic image selection did not retain two complete descriptor candidates");
  const auto *key = image.Instruction()->Arg(0).Instruction();
  Check(key->GetOpcode() == ValueOpcode::Phi && key->Parent() == merge,
        "finite image key lost its original merge block");
  const auto *loop = key->Arg(0).Instruction();
  Check(loop->GetOpcode() == ValueOpcode::Phi && loop->Parent() == header &&
            loop->Arg(1).Resolve() == Value(const_cast<Inst *>(key)) &&
            loop->Arg(0).U32() == 0 && key->Arg(1).U32() == 1,
        "finite image key did not preserve the loop-carried selection");
  std::array<uint32_t, 16> data;
  for (uint32_t i = 0; i < data.size(); ++i) data[i] = 100u + i;
  SrtWalker walker(plan, SrtRuntime{.user_data = data});
  for (uint32_t candidate = 0; candidate < 2; ++candidate) {
    DescriptorValue descriptor;
    Check(walker.EvaluateDescriptor(finite->sources[candidate], descriptor),
          "finite image candidate is not host-readable");
    for (uint32_t word = 0; word < 8; ++word)
      Check(descriptor.dwords[word] == data[candidate * 8 + word],
            "finite image selection mixed descriptor DWORDs across predecessors");
  }
}

void TestFiniteInlineSampler() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  Fixture fixture;
  auto* entry = fixture.block;
  auto* defined = fixture.AddBlock();
  auto* merge = fixture.AddBlock();
  entry->AddBranch(defined);
  entry->AddBranch(merge);
  defined->AddBranch(merge);
  const auto lane = fixture.Emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  entry->condition = fixture.Emit(ValueOpcode::IEqual32, {lane, Value(0u)});
  entry->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                      .true_block = defined, .false_block = merge};
  defined->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = merge};
  merge->terminator.kind = CFG::TerminatorKind::Return;
  fixture.block = merge;
  auto& lod = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
  auto& filter = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
  lod.AddPhiOperand(entry, Value(0u));
  lod.AddPhiOperand(defined, Value(0x00fff000u));
  filter.AddPhiOperand(defined, Value(0x0a500000u));
  filter.AddPhiOperand(entry, Value(0u));
  const auto sampler = fixture.Sampler({Value(0u), Value(&lod), Value(&filter), Value(0u)});
  const auto direct = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
  const auto previous = fixture.Sampler(
      {Value(0u), Value(0x00fff000u), Value(0x0a500000u), Value(0u)});
  using F = Libs::Graphics::Prospero::BufferFormat;
  for (const auto format : {F::k32Float, F::k32SInt}) {
    const auto image = fixture.Image({Value(0x1000u), Value(uint32_t(format) << 20u),
        Value(0u), Value(Libs::Graphics::DstSel(4, 5, 6, 7) |
            (uint32_t(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u)),
        Value(0u), Value(0u), Value(0u), Value(0u)});
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    if (format == F::k32Float)
      fixture.Emit(ValueOpcode::ImageSampleRaw, {image, previous, fixture.ImageAddress()},
                   fixture.AddMemory(memory, 0x440u));
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress()},
                 fixture.AddMemory(memory, 0x1dd0u));
    if (format == F::k32Float)
      fixture.Emit(ValueOpcode::ImageSampleRaw, {image, direct, fixture.ImageAddress()},
                   fixture.AddMemory(memory, 0x1dd8u));
  }
  fixture.PlanAndTrack();
  const auto root = fixture.program.memory_info[1].sampler;
  const auto source = fixture.program.info.samplers[root].source;
  const auto& finite = fixture.program.descriptor_sources[source].indirect_descriptor;
  Check(finite && finite->sources.size() == 2 && fixture.program.info.samplers.size() == 3,
        "finite inline sampler lost either state or aliased the direct sampler");
  const auto* key = sampler.Instruction()->Arg(0).Instruction();
  Check(key->GetOpcode() == ValueOpcode::Phi && key->Parent() == merge &&
            key->PhiBlock(0) == entry && key->PhiBlock(1) == defined &&
            key->Arg(0).U32() == 0 && key->Arg(1).U32() == 1,
        "inline sampler DWORD correlation did not preserve its GPU selector");
  const auto plan = ExtractResourcePlan(fixture.program);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const SrtRuntime runtime{.read_specialization_memory = ReadTestMemory};
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "finite inline sampler could not materialize literal choices");
  const auto candidates = fixture.program.info.samplers[root].indirect_resources;
  Check(candidates.size() == 2 && snapshot.samplers[candidates[0]].dwords[1] == 0 &&
            snapshot.samplers[candidates[0]].dwords[2] == 0 &&
            snapshot.samplers[candidates[1]].dwords[1] == 0x00fff000u &&
            snapshot.samplers[candidates[1]].dwords[2] == 0x0a500000u,
        "finite sampler materialization mixed the two correlated DWORD states");
  Check(candidates[1] == fixture.program.memory_info[0].sampler && candidates[1] < root,
        "finite sampler did not reuse the earlier constant candidate");
  ApplyResourceSpecialization(fixture.program, specialization);
  for (const uint32_t operation : {1u, 3u}) {
    const auto selected = fixture.program.memory_info[operation].sampler;
    const auto& choices = fixture.program.info.samplers[selected].indirect_resources;
    Check(choices.size() == 2, "sampler class specialization lost a finite candidate");
    for (uint32_t ordinal = 0; ordinal < choices.size(); ++ordinal) {
      const auto& choice = fixture.program.info.samplers[choices[ordinal]];
      Check(choice.snapshot_index == candidates[ordinal] &&
                choice.force_point_filtering == (operation == 3u) &&
                choice.integer_border == (operation == 3u),
            "finite sampler class remapping changed candidate identity or filtering");
    }
  }
  Check(fixture.program.info.samplers[fixture.program.memory_info[2].sampler]
            .indirect_resources.empty(),
        "direct sampler was changed into a dynamic selection");
  auto invalid = ExtractResourcePlan(fixture.program);
  invalid.info.samplers[0].source = static_cast<uint32_t>(invalid.descriptor_sources.size());
  Check(!MaterializeResources(invalid, runtime, snapshot, specialization),
        "invalid sampler source bypassed materialization validation");

  Fixture adjusted;
  auto* initial = adjusted.block;
  auto* alternate = adjusted.AddBlock();
  auto* join = adjusted.AddBlock();
  initial->AddBranch(alternate);
  initial->AddBranch(join);
  alternate->AddBranch(join);
  initial->condition = adjusted.Emit(ValueOpcode::IEqual32,
      {adjusted.Emit(ValueOpcode::LaneId), Value(0u)});
  initial->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                        .true_block = alternate, .false_block = join};
  alternate->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = join};
  join->terminator.kind = CFG::TerminatorKind::Return;
  auto& selected = join->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
  selected.AddPhiOperand(initial, Value(0u));
  selected.AddPhiOperand(alternate, Value(0x00fff000u));
  adjusted.block = join;
  const auto shared = adjusted.Sampler({Value(0u), Value(&selected), Value(0u), Value(0u)});
  const auto image = adjusted.Image({Value(0u), Value(0u), Value(0u), Value(0u),
                                    Value(0u), Value(0u), Value(0u), Value(0u)});
  for (const uint32_t flags : {0u, uint32_t(Decoder::ImageSampleFlagAdjust)}) {
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    memory.image_sample_flags = flags;
    adjusted.Emit(ValueOpcode::ImageSampleRaw, {image, shared, adjusted.ImageAddress()},
                  adjusted.AddMemory(memory, 0x1dd0u));
  }
  CheckFatal([&] { adjusted.PlanAndTrack(); }, "not a valid runtime value",
             "finite sampler planning accepted a shared SampleAdjust handle");
}

void TestFiniteImageBitScanSentinel() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  for (const bool nonzero : {false, true}) {
    Fixture fixture;
    auto *entry = fixture.block;
    auto *dispatch = fixture.AddBlock();
    auto *load = fixture.AddBlock();
    auto *merge = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    entry->AddBranch(dispatch);
    entry->AddBranch(exit);
    dispatch->AddBranch(load);
    dispatch->AddBranch(merge);
    load->AddBranch(merge);
    const auto mask = fixture.Emit(ValueOpcode::GetBuiltin,
        {Value(uint32_t(StageInputKind::LocalInvocationIndex)), Value(0u)});
    const auto condition = fixture.Emit(nonzero ? ValueOpcode::INotEqual32 : ValueOpcode::IEqual32,
                                        {mask, Value(0u)});
    auto &guard = *fixture.program.blocks[0];
    guard.condition = condition;
    guard.terminator.kind = CFG::TerminatorKind::ConditionalBranch;
    guard.terminator.true_block = dispatch;
    guard.terminator.false_block = exit;
    fixture.block = dispatch;
    const auto first = fixture.Emit(ValueOpcode::FindILsb32, {mask});
    const auto minimum = fixture.Emit(ValueOpcode::UMin32, {first, Value(32u)});
    const auto selector = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {minimum, Value(2u)});
    fixture.Emit(ValueOpcode::ReferenceU32, {selector});
    auto &table = *fixture.program.blocks[1];
    table.indirect_target = selector;
    table.terminator.kind = CFG::TerminatorKind::IndirectBranch;
    table.terminator.indexed = true;
    table.terminator.cases = {{0u, load}, {128u, merge}};
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      auto &phi = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
      phi.AddPhiOperand(dispatch, mask);
      phi.AddPhiOperand(load, Value(100u + word));
      words[word] = Value(&phi);
    }
    fixture.block = merge;
    const auto image = fixture.Image(words);
    const auto sampler = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress()},
                 fixture.AddMemory(memory, 4));
    if (!nonzero) {
      CheckFatal([&] { fixture.PlanAndTrack(); }, "not a valid runtime value",
                 "zero bit scan silently accepted a GPU-valued descriptor sentinel");
      Check(fixture.program.descriptor_sources.empty() && image.Instruction()->Arg(0) == words[0],
            "failed finite descriptor analysis partially mutated the resource plan");
      continue;
    }
    fixture.PlanAndTrack();
    const auto source = fixture.program.info.images[0].source;
    const auto &finite = fixture.program.descriptor_sources[source].indirect_descriptor;
    Check(finite && finite->sources.size() == 1 &&
              image.Instruction()->Arg(0).Instruction()->NumPhiBlocks() == 2,
          "nonzero bit scan retained its impossible sentinel or removed a Phi edge");
  }
}

void TestLoopCycleEnteredThroughRuntimeValue() {
  Fixture fixture;
  auto *entry = fixture.block;
  auto *loop = fixture.AddBlock();
  const auto initial = fixture.UserData(0);
  entry->AddBranch(loop);
  loop->AddBranch(loop);
  auto &phi = loop->AppendNewInst(ValueOpcode::Phi, {},
                                  static_cast<uint64_t>(Type::U32));
  const auto carried = fixture.Emit(ValueOpcode::BitwiseAnd32,
                                    {Value(&phi), Value(0xffffffffu)}, 0, loop);
  phi.AddPhiOperand(entry, initial);
  phi.AddPhiOperand(loop, carried);
  const auto handle = fixture.Emit(ValueOpcode::GetBufferResource,
               {carried, Value(0u), Value(0u), Value(0u)}, MemoryFlags{0, 12},
               loop);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(memory, 16), loop);
  ConstantPropagationPass(fixture.program.blocks);
  RemoveIdentities(fixture.program.blocks);
  fixture.PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.program);
  const std::array<uint32_t, 1> user_data{0x4000u};
  DescriptorValue descriptor;
  Check(fixture.program.info.buffers.size() == 1u &&
            SrtWalker(plan, {.user_data = user_data}).EvaluateDescriptor(
                fixture.program.info.buffers[0].source, descriptor) &&
            descriptor.dwords[0] == user_data[0],
        "runtime-rooted invariant loop lost its buffer source");
}

void TestInvariantLoopPhi() {
  Fixture fixture;
  auto *entry = fixture.block;
  auto *loop = fixture.AddBlock();
  entry->AddBranch(loop);
  loop->AddBranch(loop);
  const auto invariant = fixture.UserData(0);
  auto &phi = loop->AppendNewInst(ValueOpcode::Phi, {},
                                  static_cast<uint64_t>(Type::U32));
  phi.AddPhiOperand(entry, invariant);
  phi.AddPhiOperand(loop, Value(&phi));
  const auto handle = fixture.Emit(
      ValueOpcode::GetBufferResource,
      {Value(&phi), Value(0u), Value(0u), Value(0u)}, MemoryFlags{0, 4}, loop);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(memory, 4), loop);
  fixture.PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.program);

  std::array<uint32_t, 1> user_data{0x12345678u};
  SrtRuntime runtime{.user_data = user_data};
  DescriptorValue descriptor;
  Check(SrtWalker(plan, runtime).EvaluateDescriptor(fixture.program.info.buffers[0].source, descriptor) &&
            descriptor.dwords[0] == user_data[0],
        "loop-invariant descriptor phi was not evaluated through typed SSA");
}

void TestBufferStoreUsesItsOwnActiveValue() {
  Fixture fixture;
  const auto lane = fixture.Emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  const auto guard = fixture.Emit(ValueOpcode::ULessThan32, {lane, Value(16u)});
  const auto other_guard = fixture.Emit(ValueOpcode::ULessThan32, {lane, Value(32u)});
  const auto inner = fixture.Emit(ValueOpcode::SelectU32, {guard, Value(9u), lane});
  const auto data = fixture.Emit(ValueOpcode::SelectU32, {guard, inner, lane});
  const auto handle = fixture.Buffer({Value(0x1000u), Value(4u << 16u),
                                       Value(64u), Value(0x16204u)});
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  const auto store = [&](Value active) {
    return fixture.Emit(ValueOpcode::StoreBufferU32,
        {handle, Value(0u), Value(0u), Value(0u), data, active},
        fixture.AddMemory(memory, 0));
  };
  const auto matching = store(guard);
  const auto different = store(other_guard);
  ConstantPropagationPass(fixture.program.blocks);
  Check(matching.Instruction()->Arg(4).Resolve() == Value(9u),
        "buffer store retained data from its inactive lanes");
  Check(different.Instruction()->Arg(4).Resolve() == data.Resolve() &&
            data.Resolve().Instruction()->GetOpcode() == ValueOpcode::SelectU32,
        "buffer store changed a shared value outside its own EXEC mask");
}

void TestBoundedRelativeRegisterWrites() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant { Bounded, BoundClobber, DescriptorClobber, EntryBypass };
  for (const auto variant : {Variant::Bounded, Variant::BoundClobber,
                             Variant::DescriptorClobber, Variant::EntryBypass}) {
    Fixture fixture;
    auto *entry = fixture.block;
    auto *header = fixture.AddBlock();
    auto *body = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    entry->AddBranch(header);
    if (variant == Variant::EntryBypass) entry->AddBranch(body);
    header->AddBranch(body);
    header->AddBranch(exit);
    body->AddBranch(header);
    fixture.program.blocks[0]->terminator = {
        .kind = variant == Variant::EntryBypass
                    ? CFG::TerminatorKind::ConditionalBranch
                    : CFG::TerminatorKind::Branch,
        .true_block = fixture.program.blocks[1u],
        .false_block = fixture.program.blocks[2u]};
    fixture.program.blocks[1]->terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = fixture.program.blocks[2u],
        .false_block = fixture.program.blocks[3u]};
    fixture.program.blocks[2]->terminator = {
        .kind = CFG::TerminatorKind::Branch,
        .true_block = fixture.program.blocks[1u]};
    fixture.program.blocks[3]->terminator.kind = CFG::TerminatorKind::Return;

    const auto lane = fixture.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
    const auto enabled = fixture.Emit(ValueOpcode::ULessThan32, {lane, Value(32u)});
    fixture.program.blocks[0]->condition = enabled;
    const auto chunk = fixture.Emit(ValueOpcode::SelectU32,
                                    {enabled, Value(64u), Value(512u)});
    const auto initial_bound = fixture.Emit(ValueOpcode::ShiftRightLogical32,
                                           {chunk, Value(6u)});
    const auto initial_records = fixture.UserData(2u);
    const auto base = fixture.UserData(0u);
    auto &counter = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    auto &bound = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    auto &records = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    const auto in_range = fixture.Emit(ValueOpcode::ULessThan32,
                                       {Value(&counter), Value(&bound)}, 0, header);
    fixture.program.blocks[1]->condition =
        fixture.Emit(ValueOpcode::ConditionRef, {in_range},
                     CFG::BranchCondition::VccNonZero, header);
    const auto m0 = fixture.Emit(ValueOpcode::BitwiseAnd32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32,
                       {Value(&counter), Value(1u)}, 0, body), Value(255u)}, 0, body);
    const auto relative_write = [&](Value old_value, uint32_t offset) {
      const auto selected = fixture.Emit(ValueOpcode::IEqual32,
                                           {m0, Value(offset)}, 0, body);
      const auto active = fixture.Emit(ValueOpcode::LogicalAnd,
                                        {enabled, selected}, 0, body);
      return fixture.Emit(ValueOpcode::SelectU32, {active, lane, old_value}, 0, body);
    };
    bound.AddPhiOperand(entry, initial_bound);
    bound.AddPhiOperand(body, relative_write(Value(&bound),
        variant == Variant::BoundClobber ? 14u : 22u));
    records.AddPhiOperand(entry, initial_records);
    records.AddPhiOperand(body, relative_write(Value(&records),
        variant == Variant::DescriptorClobber ? 14u : 21u));
    counter.AddPhiOperand(entry, Value(0u));
    counter.AddPhiOperand(body, fixture.Emit(ValueOpcode::IAdd32,
                                             {Value(&counter), Value(1u)}, 0, body));
    const auto handle = fixture.Emit(ValueOpcode::GetBufferResource,
        {base, Value(4u << 16u), Value(&records), Value(0x16204u)},
        MemoryFlags{0, 0x3e8}, exit);
    MemoryInfo memory;
    memory.kind = ResourceKind::Buffer;
    fixture.Emit(ValueOpcode::StoreBufferU32,
        {handle, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
        fixture.AddMemory(memory, 0x3e8), exit);
    if (variant != Variant::Bounded) {
      CheckFatal([&] { fixture.PlanAndTrack(); }, "not a valid runtime value",
                 "relative register proof discarded a possible loop clobber");
      continue;
    }
    fixture.PlanAndTrack();
    auto plan = ExtractResourcePlan(fixture.program);
    const std::array<uint32_t, 3> user_data{0x1000u, 0u, 72u};
    SrtRuntime runtime{.user_data = user_data};
    DescriptorValue descriptor;
    Check(SrtWalker(plan, runtime).EvaluateDescriptor(
              fixture.program.info.buffers[0].source, descriptor) &&
              descriptor.dwords[2] == 72u,
          "impossible relative writes left a false descriptor dependency");
  }
}

void TestDmaAddressMaterialization() {
  Fixture fixture;
  const auto based =
      fixture.Address(fixture.UserData(0), fixture.UserData(1), 4);
  MemoryInfo global;
  global.kind = ResourceKind::Global;
  global.offset = static_cast<uint32_t>(-8);
  fixture.Emit(ValueOpcode::LoadAddressU32,
               {based, Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(global, 4));

  const auto undef = fixture.Emit(ValueOpcode::UndefU32);
  const auto unbased = fixture.Address(undef, undef, 8);
  MemoryInfo flat;
  flat.kind = ResourceKind::Flat;
  flat.address_is_full = true;
  fixture.Emit(ValueOpcode::StoreAddressU32,
               {unbased, Value(0u), Value(0u), Value(9u), Value(true)},
               fixture.AddMemory(flat, 8));
  fixture.PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture.program);

  Check(fixture.program.info.uses_dma,
        "typed address operations did not enable DMA");
  std::array<uint32_t, 2> user_data{0x2008u, 0u};
  SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot,
                             specialization),
        "DMA shader resources did not materialize");
  ApplyResourceSpecialization(fixture.program, specialization);
}

void TestDynamicFlatAddressesUseDma() {
  Fixture fixture;
  const auto low_root = fixture.UserData(0);
  const auto high_root = fixture.UserData(1);
  const auto active =
      fixture.Emit(ValueOpcode::INotEqual32, {fixture.UserData(2), Value(0u)});
  const auto inactive_low = fixture.Emit(ValueOpcode::UndefU32);
  const auto inactive_high = fixture.Emit(ValueOpcode::UndefU32);
  const auto low =
      fixture.Emit(ValueOpcode::SelectU32, {active, low_root, inactive_low});
  const auto high =
      fixture.Emit(ValueOpcode::SelectU32, {active, high_root, inactive_high});
  const auto address = fixture.Address(low, high, 0xa4);
  MemoryInfo flat;
  flat.kind = ResourceKind::Flat;
  flat.address_is_full = true;
  fixture.Emit(ValueOpcode::LoadAddressU8, {address, low, high, active},
               fixture.AddMemory(flat, 0xa4));
  fixture.PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture.program);

  Check(fixture.program.info.uses_dma,
        "exec-masked FLAT address did not enable DMA");
  std::array<uint32_t, 3> user_data{0x23456780u, 1u, 1u};
  SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot,
                             specialization),
        "exec-masked FLAT shader resources did not materialize");

  Fixture mismatch;
  const auto mismatch_active = mismatch.Emit(ValueOpcode::INotEqual32,
                                             {mismatch.UserData(2), Value(0u)});
  const auto other_active =
      mismatch.Emit(ValueOpcode::LogicalNot, {mismatch_active});
  const auto mismatch_low = mismatch.Emit(
      ValueOpcode::SelectU32, {mismatch_active, mismatch.UserData(0),
                               mismatch.Emit(ValueOpcode::UndefU32)});
  const auto mismatch_high = mismatch.Emit(
      ValueOpcode::SelectU32, {mismatch_active, mismatch.UserData(1),
                               mismatch.Emit(ValueOpcode::UndefU32)});
  const auto mismatch_address =
      mismatch.Address(mismatch_low, mismatch_high, 0xa4);
  mismatch.Emit(ValueOpcode::LoadAddressU8,
                {mismatch_address, mismatch_low, mismatch_high, other_active},
                mismatch.AddMemory(flat, 0xa4));
  mismatch.PlanAndTrack();
  Check(mismatch.program.info.uses_dma,
        "dynamic FLAT address did not enable DMA");
}

void TestBufferSwizzleSpecialization() {
  Fixture fixture;
  const auto handle = fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                                      fixture.UserData(2), fixture.UserData(3)},
                                     4);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  memory.formatted = true;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(memory, 4));
  fixture.PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture.program);

  constexpr auto swizzle = Libs::Graphics::DstSel(4, 5, 0, 1);
  std::array<uint32_t, 4> user_data{
      0, 16u << 16u, 1,
      swizzle |
          (static_cast<uint32_t>(
               Libs::Graphics::Prospero::BufferFormat::k32_32Float)
           << 12u) |
          (1u << 24u)};
  SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot,
                             specialization),
        "buffer resources did not materialize");
  ApplyResourceSpecialization(fixture.program, specialization);
  Check(fixture.program.info.buffers[0].descriptor_swizzle == swizzle &&
            specialization.buffers[0].descriptor_swizzle == swizzle,
        "buffer destination selectors were not specialized");

  user_data[3] ^= 1u << 9u;
  ResourceSnapshot changed_snapshot;
  ResourceSpecialization changed_specialization;
  Check(MaterializeResources(resource_plan, runtime, changed_snapshot,
                             changed_specialization) &&
            changed_specialization != specialization,
        "buffer swizzle change did not select a new specialization key");
}

enum class ConditionalBufferUse { Optional, Shared, Loop, Writable };

ResourcePlan ConditionalBufferPlan(ConditionalBufferUse use) {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  Fixture fixture;
  auto *entry = fixture.block;
  auto *optional = fixture.AddBlock();
  auto *done = fixture.AddBlock();
  auto *condition_block = entry;
  uint32_t condition_index = 0;
  fixture.program.blocks[0]->id = 11;
  fixture.program.blocks[1]->id = 27;
  fixture.program.blocks[2]->id = 42;
  if (use == ConditionalBufferUse::Loop) {
    condition_block = fixture.AddBlock();
    condition_index = 3;
    fixture.program.blocks[3]->id = 55;
    fixture.program.blocks[0]->terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = condition_block};
    entry->AddBranch(condition_block);
  }
  condition_block->AddBranch(optional);
  condition_block->AddBranch(done);
  optional->AddBranch(use == ConditionalBufferUse::Loop ? condition_block : done);
  fixture.program.blocks[condition_index]->terminator = {
      .kind = CFG::TerminatorKind::ConditionalBranch,
      .true_block = optional,
      .false_block = done};
  fixture.program.blocks[1]->terminator = {
      .kind = CFG::TerminatorKind::Branch,
      .true_block = use == ConditionalBufferUse::Loop ? condition_block : done};

  const auto control = fixture.Buffer(
      {fixture.UserData(0), fixture.UserData(1), fixture.UserData(2),
       fixture.UserData(3)}, 4);
  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarBuffer;
  auto flag = fixture.Emit(ValueOpcode::ReadConstBuffer,
                           {control, Value(0u)}, fixture.AddMemory(scalar, 4));
  if (use == ConditionalBufferUse::Loop) {
    auto &phi = condition_block->AppendNewInst(ValueOpcode::Phi, {},
                                               static_cast<uint64_t>(Type::U32));
    phi.AddPhiOperand(entry, flag);
    phi.AddPhiOperand(optional, Value(1u));
    flag = Value(&phi);
  }
  fixture.program.blocks[condition_index]->condition = fixture.Emit(
      ValueOpcode::INotEqual32, {flag, Value(0u)}, 0, condition_block);

  const auto payload = fixture.Buffer(
      {fixture.UserData(4), fixture.UserData(5), fixture.UserData(6),
       fixture.UserData(7)}, 8);
  MemoryInfo vector;
  vector.kind = ResourceKind::Buffer;
  const auto load = [&](Block *block) {
    fixture.Emit(ValueOpcode::LoadBufferU32,
                 {payload, Value(0u), Value(0u), Value(0u), Value(true)},
                 fixture.AddMemory(vector, 8), block);
  };
  load(optional);
  if (use == ConditionalBufferUse::Shared) {
    load(done);
  }
  if (use == ConditionalBufferUse::Writable) {
    fixture.Emit(ValueOpcode::StoreBufferU32,
                 {control, Value(0u), Value(0u), Value(0u), Value(1u),
                  Value(true)}, fixture.AddMemory(vector, 12));
  }
  fixture.PlanAndTrack();
  return ExtractResourcePlan(fixture.program);
}

void TestConditionalBufferMaterialization() {
  auto plan = ConditionalBufferPlan(ConditionalBufferUse::Optional);
  // GTA III leaves packet words in s[12:15] when its scalar control word is zero.
  std::array<uint32_t, 8> user_data{
      0x1000, 16u << 16u, 1, 0x4dfac,
      0xc0107600, 0x8c, 0x97730000, 0x100020};
  TestMemory memory;
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                     .read_specialization_memory = ReadTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.buffers.size() == 2 &&
            snapshot.buffers[1].dword_count == 4 &&
            snapshot.buffers[1].dwords == std::array<uint32_t, 8>{},
        "untaken scalar branch materialized stale buffer words");
  Check(snapshot.user_data == std::vector<uint32_t>(user_data.begin(), user_data.end()),
        "resource reachability changed native shader user data");

  runtime.user_data = std::span(user_data).first(4);
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "untaken branch evaluated its unavailable descriptor");
  memory.words[0] = 1;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "taken branch accepted an unavailable descriptor");

  runtime.user_data = user_data;
  const auto CheckActive = [&] {
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers.size() == 2 &&
              std::equal(user_data.begin() + 4, user_data.end(),
                         snapshot.buffers[1].dwords.begin()),
          "potentially executed buffer descriptor was discarded");
  };
  CheckActive();
  memory.words[0] = 0;
  memory.fail_after = memory.reads;
  CheckActive();
  runtime.read_specialization_memory = nullptr;
  CheckActive();
}

void TestGuardedScalarDescriptorReads() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  for (const bool shared : {false, true}) {
    for (const bool exec : {false, true}) {
      Fixture fixture;
      auto *entry = fixture.block;
      auto *optional = fixture.AddBlock();
      auto *done = fixture.AddBlock();
      entry->AddBranch(optional);
      entry->AddBranch(done);
      optional->AddBranch(done);
      fixture.program.blocks[0]->terminator = {
          .kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = fixture.program.blocks[1],
          .false_block = fixture.program.blocks[2]};
      fixture.program.blocks[1]->terminator = {
          .kind = CFG::TerminatorKind::Branch,
          .true_block = fixture.program.blocks[2]};
      fixture.program.blocks[2]->terminator.kind = CFG::TerminatorKind::Return;
      const auto control = fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                                          fixture.UserData(2), fixture.UserData(3)});
      MemoryInfo scalar;
      scalar.kind = ResourceKind::ScalarBuffer;
      scalar.offset = 28;
      const auto flag = fixture.Emit(ValueOpcode::ReadConstBuffer, {control, Value(0u)},
                                     fixture.AddMemory(scalar, 4));
      const auto lane = fixture.Emit(ValueOpcode::GetBuiltin,
          {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
      const auto varying = fixture.Emit(ValueOpcode::INotEqual32, {lane, Value(0u)});
      const auto enabled = fixture.Emit(ValueOpcode::INotEqual32, {flag, Value(0u)});
      fixture.program.blocks[0]->condition =
          fixture.Emit(ValueOpcode::LogicalAnd, {varying, enabled});
      const auto root = fixture.Address(fixture.UserData(4), Value(0u));
      const auto Load = [&](Block *block) {
        fixture.block = block;
        MemoryInfo address;
        address.kind = ResourceKind::ScalarAddress;
        const auto pointer = fixture.Emit(ValueOpcode::LoadAddressU32,
            {root, Value(0u), Value(0u), Value(exec)}, fixture.AddMemory(address, 8));
        const auto table = fixture.Address(pointer, Value(0u));
        std::array<Value, 4> words;
        for (uint32_t word = 0; word < words.size(); ++word) {
          address.offset = 40 + word * 4;
          words[word] = fixture.Emit(ValueOpcode::LoadAddressU32,
              {table, Value(0u), Value(0u), Value(exec)}, fixture.AddMemory(address, 12));
        }
        scalar.offset = 0;
        fixture.Emit(ValueOpcode::ReadConstBuffer, {fixture.Buffer(words), Value(0u)},
                     fixture.AddMemory(scalar, 16));
      };
      Load(optional);
      if (shared) Load(done);
      fixture.block = done;
      MemoryInfo store;
      store.kind = ResourceKind::Buffer;
      fixture.Emit(ValueOpcode::StoreBufferU32,
          {fixture.Buffer({Value(0x9000u), Value(0u), Value(4u), Value(0u)}),
           Value(0u), Value(0u), Value(0u), Value(1u), Value(true)}, fixture.AddMemory(store, 20));
      fixture.PlanAndTrack();
      auto plan = ExtractResourcePlan(fixture.program);
      Check(!exec || (!plan.srt_reads.empty() && !plan.control_flow[1].srt_reads.empty() &&
                      (!shared || !plan.control_flow[2].srt_reads.empty())),
            "guarded shared-read fixture lost its flattened execution sites");
      struct Memory { LinearTestMemory data; uint32_t null_reads = 0; } memory;
      const auto Read = +[](void *data, uint64_t address, std::span<uint32_t> words) {
        auto &memory = *static_cast<Memory *>(data);
        if (address == 40) ++memory.null_reads;
        return ReadLinearTestMemory(&memory.data, address, words);
      };
      const std::array<uint32_t, 5> user_data{0x1000u, 0u, 64u, 0u, 0x1040u};
      const SrtRuntime runtime{.user_data = user_data, .read_memory = Read,
                               .userdata = &memory, .read_specialization_memory = Read};
      memory.data.words[0xa8 / 4] = 0x2000u;
      memory.data.words[0xb0 / 4] = 4u;
      ResourceSnapshot snapshot;
      ResourceSpecialization specialization;
      if (!shared) {
        Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.null_reads == 0 &&
                  std::ranges::all_of(snapshot.flattened_srt, [](auto word) { return word == 0; }),
              "disabled feature speculatively dereferenced its null BVH table");
        memory.data.words[7] = 1;
        Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.null_reads == 1,
              "active feature accepted an unreadable BVH table");
      }
      memory.data.words[0x40 / 4] = 0x1080u;
      memory.data.watched_address = 0x10a8u;
      memory.data.watched_reads = 0;
      Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                std::ranges::any_of(snapshot.buffers, [](const auto& descriptor) {
                  return descriptor.dwords[0] == 0x2000u;
                }) &&
                (!exec || (memory.data.watched_reads == 1 &&
                           std::ranges::find(snapshot.flattened_srt, 0x2000u) != snapshot.flattened_srt.end())),
            "active or shared descriptor read was omitted after refreshing the cached plan");
      if (!shared) {
        memory.data.words[7] = 0;
        memory.data.words[0x40 / 4] = 0;
        Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.null_reads == 1 &&
                  std::ranges::all_of(snapshot.flattened_srt, [](auto word) { return word == 0; }),
              "cached plan retained reads after the feature was disabled");
      }
    }
  }
}

void TestConservativeBufferReachability() {
  std::array<uint32_t, 8> user_data{
      0x1000, 16u << 16u, 1, 0x4dfac,
      0x2000, 16u << 16u, 1, 0x4dfac};
  TestMemory memory;
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                           .read_specialization_memory = ReadTestMemory};
  for (const auto use : {ConditionalBufferUse::Shared, ConditionalBufferUse::Loop,
                         ConditionalBufferUse::Writable}) {
    auto plan = ConditionalBufferPlan(use);
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    std::array<uint32_t, 4> expected{};
    if (use != ConditionalBufferUse::Writable)
      std::copy_n(user_data.begin() + 4, expected.size(), expected.begin());
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers.size() == 2 &&
              std::equal(expected.begin(), expected.end(),
                         snapshot.buffers[1].dwords.begin()),
          "shared or loop-dependent resource was pruned, or inactive scalar branch retained");
  }
}

void TestConditionalIndirectImageMaterialization() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  auto fixture = MakeIndirectImageFixture(false);
  auto *body = fixture->block;
  auto *entry = fixture->AddBlock();
  auto *done = fixture->AddBlock();
  entry->AddBranch(body);
  entry->AddBranch(done);
  body->AddBranch(done);
  const auto flag = fixture->Emit(ValueOpcode::GetUserData,
                                  {Value(static_cast<ScalarReg>(8))}, 0, entry);
  fixture->program.blocks[1]->condition =
      fixture->Emit(ValueOpcode::INotEqual32, {flag, Value(0u)}, 0, entry);
  fixture->program.blocks[1]->terminator = {
      .kind = CFG::TerminatorKind::ConditionalBranch,
      .true_block = fixture->program.blocks[0],
      .false_block = fixture->program.blocks[2]};
  fixture->program.blocks[0]->terminator = {.kind = CFG::TerminatorKind::Branch,
                                            .true_block =
                                                fixture->program.blocks[2]};
  std::swap(fixture->program.blocks[0], fixture->program.blocks[1]);
  fixture->PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture->program);
  std::array<uint32_t, 9> user_data{
      0x1000, 224u << 16u, 2, 0, 0x2000, 16u << 16u, 4, 0, 0};
  uint32_t reads = 0;
  const SrtRuntime runtime{
      .user_data = user_data, .userdata = &reads,
      .read_specialization_memory = [](void *data, uint64_t, std::span<uint32_t>) {
        ++*static_cast<uint32_t *>(data);
        return false;
      }};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            reads == 0 && snapshot.images.size() == 1 &&
            snapshot.images[0].dwords == std::array<uint32_t, 8>{},
        "untaken indirect image branch probed its descriptor table");
  user_data[8] = 1;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) && reads != 0,
        "taken indirect image branch did not require its descriptor table");
}

void TestShaderInfoCollection() {
  Fixture fixture;
  const auto handle = fixture.Buffer(
      {fixture.UserData(3), fixture.UserData(4), Value(64u), Value(0u)}, 4);
  MemoryInfo buffer;
  buffer.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(buffer, 4));
  const auto invocation = fixture.Emit(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)),
       Value(2u)});
  const auto value = fixture.Emit(ValueOpcode::BitwiseXor32,
                                   {invocation, Value(2u)});
  MemoryInfo gds;
  gds.kind = ResourceKind::Gds;
  fixture.Emit(ValueOpcode::WriteSharedU32, {Value(0u), value, Value(true)},
               fixture.AddMemory(gds, 8));
  fixture.PlanAndTrack();

  ShaderComputeInputInfo compute{};
  compute.dispatch_thread_dimensions = true;
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(fixture.program.info.has_bitwise_xor &&
            !fixture.program.info.inputs.empty() &&
            fixture.program.info.inputs[0].kind ==
                StageInputKind::GlobalInvocationId,
        "typed shader values were not reflected in shader info");

  Check(fixture.program.info.live_buffers == 1u &&
            fixture.program.info.uses_gds && !fixture.program.info.uses_lds,
        "typed resources were not collected in shader info");
  Check(NativeBinding(ShaderType::Compute, DescriptorBindingKind::Buffers) ==
                static_cast<uint32_t>(DescriptorBindingKind::Buffers) &&
            NativeBinding(ShaderType::Vertex, DescriptorBindingKind::Buffers) ==
                static_cast<uint32_t>(DescriptorBindingKind::Buffers) &&
            NativeBinding(ShaderType::Pixel, DescriptorBindingKind::Buffers) ==
                static_cast<uint32_t>(DescriptorBindingKind::Count) +
                    static_cast<uint32_t>(DescriptorBindingKind::Buffers),
        "fixed stage binding ranges are inconsistent");
  Check(fixture.program.info.user_data_registers == std::vector<uint32_t>({3u, 4u}),
        "shader info did not collect live typed user-data values");
}

void TestImageBindingAbi() {
  using NumericClass = Libs::Graphics::Prospero::TextureNumericClass;

  Check(ImageBindingCount == 48u &&
            static_cast<uint32_t>(DescriptorBindingKind::Buffers) == 0u &&
            static_cast<uint32_t>(DescriptorBindingKind::Samplers) == 49u &&
            static_cast<uint32_t>(DescriptorBindingKind::Gds) == 50u &&
            static_cast<uint32_t>(DescriptorBindingKind::BdaPagetable) == 51u &&
            static_cast<uint32_t>(DescriptorBindingKind::FaultBuffer) == 52u &&
            static_cast<uint32_t>(DescriptorBindingKind::FlattenedSrt) == 53u &&
            static_cast<uint32_t>(DescriptorBindingKind::ShaderData) == 54u &&
            static_cast<uint32_t>(DescriptorBindingKind::SharedMemory) == 55u &&
            static_cast<uint32_t>(DescriptorBindingKind::Count) == 56u,
        "native descriptor binding anchors changed");

  const std::array sampled_dimensions{
      Decoder::ImageDimension::Dim1D,
      Decoder::ImageDimension::Dim1DArray,
      Decoder::ImageDimension::Dim2D,
      Decoder::ImageDimension::Dim2DArray,
      Decoder::ImageDimension::Dim2DMsaa,
      Decoder::ImageDimension::Dim2DMsaaArray,
      Decoder::ImageDimension::Dim3D,
  };
  const std::array storage_dimensions{
      Decoder::ImageDimension::Dim1D, Decoder::ImageDimension::Dim1DArray,
      Decoder::ImageDimension::Dim2D, Decoder::ImageDimension::Dim2DArray,
      Decoder::ImageDimension::Dim3D,
  };
  const std::array sampled_classes{NumericClass::Float, NumericClass::Uint,
                                   NumericClass::Sint};
  const std::array storage_classes{NumericClass::Float, NumericClass::Uint};
  uint32_t index = 0;
  const auto CheckBinding =
      [&](ImageResourceClass resource_class, NumericClass numeric_class,
          Decoder::ImageDimension dimension, bool atomic, bool comparison = false, bool atomic64 = false) {
        ImageResource image;
        image.resource_class = resource_class;
        image.numeric_class = numeric_class;
        image.dimension = dimension;
        image.atomic = atomic;
        image.atomic64 = atomic64;
        image.depth_compare = comparison;
        const auto kind = DescriptorBindingForImage(image);
        Check(kind.has_value() &&
                  static_cast<uint32_t>(*kind) == FirstImageBinding + index &&
                  ImageBindingIndex(*kind) == index &&
                  ImageBindingResourceClass(*kind) == resource_class &&
                  NativeBinding(ShaderType::Compute, *kind) ==
                      FirstImageBinding + index &&
                  NativeBinding(ShaderType::Pixel, *kind) ==
                      static_cast<uint32_t>(DescriptorBindingKind::Count) +
                          FirstImageBinding + index,
              "generated image descriptor binding changed ABI");
        index++;
      };
  for (const auto numeric_class : sampled_classes) {
    for (const auto dimension : sampled_dimensions) {
      CheckBinding(ImageResourceClass::Sampled, numeric_class, dimension,
                   false);
    }
  }
  for (const auto dimension : sampled_dimensions) {
    CheckBinding(ImageResourceClass::Sampled, NumericClass::Float, dimension,
                 false, true);
  }
  for (const auto numeric_class : storage_classes) {
    for (const auto dimension : storage_dimensions) {
      CheckBinding(ImageResourceClass::Storage, numeric_class, dimension,
                   false);
    }
  }
  for (const auto dimension : storage_dimensions) {
    CheckBinding(ImageResourceClass::Storage, NumericClass::Uint, dimension,
                 true);
  }
  for (const auto dimension : storage_dimensions) {
    CheckBinding(ImageResourceClass::Storage, NumericClass::Uint, dimension,
                 true, false, true);
  }
  Check(index == ImageBindingCount, "image descriptor ABI case count changed");

  const auto Invalid = [](ImageResource image) {
    return !DescriptorBindingForImage(image).has_value();
  };
  ImageResource image;
  Check(Invalid(image), "untyped image received a descriptor binding");
  image.resource_class = ImageResourceClass::Sampled;
  image.numeric_class = NumericClass::Float;
  image.dimension = Decoder::ImageDimension::Unknown;
  Check(Invalid(image),
        "unknown sampled dimension received a descriptor binding");
  image.dimension = Decoder::ImageDimension::Dim2D;
  image.numeric_class = NumericClass::Unsupported;
  Check(Invalid(image),
        "unsupported sampled class received a descriptor binding");
  image.numeric_class = NumericClass::Uint;
  image.depth_compare = true;
  Check(Invalid(image), "integer comparison image received a descriptor binding");
  image.depth_compare = false;
  image.numeric_class = static_cast<NumericClass>(UINT32_MAX);
  Check(Invalid(image), "invalid sampled class received a descriptor binding");
  image.numeric_class = NumericClass::Float;
  image.dimension = static_cast<Decoder::ImageDimension>(UINT32_MAX);
  Check(Invalid(image),
        "invalid sampled dimension received a descriptor binding");
  image.dimension = Decoder::ImageDimension::Dim2D;
  image.atomic = true;
  Check(Invalid(image), "atomic sampled image received a descriptor binding");
  image.resource_class = ImageResourceClass::Storage;
  image.atomic = false;
  image.numeric_class = NumericClass::Sint;
  Check(Invalid(image), "signed storage image received a descriptor binding");
  image.numeric_class = NumericClass::Float;
  image.dimension = Decoder::ImageDimension::Dim2DMsaa;
  Check(Invalid(image),
        "multisampled storage image received a descriptor binding");
  image.dimension = Decoder::ImageDimension::Dim2D;
  image.atomic = true;
  Check(Invalid(image), "float atomic image received a descriptor binding");
}

void TestLiveShaderDataCollection() {
  Fixture fixture;
  fixture.UserData(7);
  fixture.Emit(ValueOpcode::ReferenceU32, {fixture.UserData(5)});
  fixture.Emit(ValueOpcode::ReferenceU32, {fixture.UserData(2)});
  fixture.Emit(ValueOpcode::ReferenceU32, {fixture.UserData(5)});
  fixture.Emit(ValueOpcode::GetDispatchThreadExtent, {Value(1u)});
  fixture.PlanAndTrack();
  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(fixture.program.info.user_data_registers == std::vector<uint32_t>({2u, 5u}) &&
            !fixture.program.info.uses_dispatch_threads,
        "shader data collection retained unused values or duplicated registers");

  const auto extent = fixture.Emit(ValueOpcode::GetDispatchThreadExtent, {Value(0u)});
  fixture.Emit(ValueOpcode::ReferenceU32, {extent});
  fixture.program.shader_info_complete = false;
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(fixture.program.info.uses_dispatch_threads &&
            fixture.program.info.user_data_registers == std::vector<uint32_t>({2u, 5u}),
        "shader data recollection lost a live dispatch extent or retained stale registers");
}

void TestResourceLimitIsTransactional() {
  Fixture accepted;
  MemoryInfo accepted_memory;
  accepted_memory.kind = ResourceKind::Buffer;
  for (uint32_t index = 0; index < ShaderInfo::MaxBuffers; index++) {
    const auto handle = accepted.Buffer(
        {Value(index), Value(index + 1u), Value(index + 2u), Value(index + 3u)},
        index * 4u);
    accepted.Emit(ValueOpcode::LoadBufferU32,
                  {handle, Value(0u), Value(0u), Value(0u), Value(true)},
                  accepted.AddMemory(accepted_memory, index * 4u));
  }
  accepted.PlanAndTrack();
  Check(accepted.program.info.buffers.size() == 64u &&
            accepted.program.descriptor_sources.size() == 64u &&
            accepted.program.memory_info.back().resource == 63u,
        "compute shader did not retain all 64 distinct buffers");
  ShaderComputeInputInfo compute{};
  CollectShaderInfo(accepted.program, {.compute = &compute});
  Check(accepted.program.info.live_buffers == UINT64_MAX,
        "shader info truncated the 64 live buffers");

  Fixture fixture;
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  for (uint32_t index = 0; index <= ShaderInfo::MaxBuffers; index++) {
    const auto handle = fixture.Buffer(
        {Value(index), Value(index + 1u), Value(index + 2u), Value(index + 3u)},
        index * 4u);
    fixture.Emit(ValueOpcode::LoadBufferU32,
                 {handle, Value(0u), Value(0u), Value(0u), Value(true)},
                 fixture.AddMemory(memory, index * 4u));
  }

  CheckFatal([&] { fixture.PlanAndTrack(); },
             "buffer resource limit exceeded",
             "resource-limit failure was not reported");
  Check(!fixture.program.resource_tracking_complete &&
            fixture.program.info.buffers.empty() &&
            fixture.program.descriptor_sources.empty(),
        "resource-limit failure partially mutated typed resource state");
}

void TestMalformedMemoryKindsRejected() {
  {
    Fixture fixture;
    const auto address = fixture.Address(Value(0u), Value(0u), 4);
    MemoryInfo memory;
    memory.kind = ResourceKind::Buffer;
    fixture.Emit(ValueOpcode::StoreAddressU32,
                 {address, Value(0u), Value(0u), Value(1u), Value(true)},
                 fixture.AddMemory(memory, 4));

    CheckFatal(
        [&] { fixture.PlanAndTrack(); },
        "address operation has invalid resource kind",
        "resource tracking accepted an address opcode with buffer metadata");
  }
  {
    Fixture fixture;
    const auto image =
        fixture.Image({Value(0u), Value(0u), Value(0u), Value(0u), Value(0u),
                       Value(0u), Value(0u), Value(0u)},
                      8);
    MemoryInfo memory;
    memory.kind = ResourceKind::Flat;
    fixture.Emit(ValueOpcode::ImageRead,
                 {image, fixture.ImageAddress(), Value(true)},
                 fixture.AddMemory(memory, 8));

    CheckFatal(
        [&] { fixture.PlanAndTrack(); },
        "image operation has invalid resource kind",
        "resource tracking accepted an image opcode with address metadata");
  }
}

} // namespace

int main() {
  try {
    const auto Run = [](const char *name, auto test) {
      try {
        test();
      } catch (const std::exception &exception) {
        throw std::runtime_error(std::string(name) + ": " + exception.what());
      }
    };
    Run("dense buffers", TestDenseBufferTracking);
    Run("compute buffer fill", TestComputeBufferFill);
    Run("scalar/vector alias", TestScalarAndVectorBufferAlias);
    Run("runtime unsigned min", TestRuntimeUnsignedMinDescriptor);
    Run("images and samplers", TestImagesSamplersAndAliases);
    Run("SampleAdjust sampler scratch", TestSampleAdjustSamplerScratch);
    Run("FMASK load specialization", TestFmaskLoadSpecialization);
    Run("gather LOD sampler validation", TestGatherLodSamplerValidation);
    Run("dynamic storage mips", TestDynamicStorageMipTracking);
    Run("invariant indirect images", TestInvariantIndirectImageMaterialization);
    Run("bounded image view eligibility", TestBoundedImageViewEligibility);
    Run("indirect image table operations", TestIndirectImageTableOperations);
    Run("waterfall image table", TestWaterfallImageTable);
    Run("guarded direct image table", TestGuardedDirectImageTable);
    Run("bounded compute image loop", TestBoundedComputeImageLoop);
    Run("uniformized material image keys", TestUniformizedMaterialImageKeys);
    Run("image descriptor fields", TestImageDescriptorFields);
    Run("draw-uniform scalar image", TestUniformScalarBufferImage);
    Run("SRT runtime", TestSrtFlatteningAndRuntimeMemoization);
    Run("dynamic SRT", TestDynamicSrtReadRemainsExplicit);
    Run("writable descriptor phi", TestWritableDescriptorPhi);
    Run("conditional sampler phi", TestConditionalSamplerPhi);
    Run("finite image phi cycle", TestFiniteImagePhiCycle);
    Run("finite inline sampler", TestFiniteInlineSampler);
    Run("finite image bit scan sentinel", TestFiniteImageBitScanSentinel);
    Run("runtime-rooted loop", TestLoopCycleEnteredThroughRuntimeValue);
    Run("invariant loop phi", TestInvariantLoopPhi);
    Run("buffer store active value", TestBufferStoreUsesItsOwnActiveValue);
    Run("bounded relative register writes", TestBoundedRelativeRegisterWrites);
    Run("DMA address materialization", TestDmaAddressMaterialization);
    Run("dynamic FLAT address", TestDynamicFlatAddressesUseDma);
    Run("buffer swizzle specialization", TestBufferSwizzleSpecialization);
    Run("conditional buffer materialization", TestConditionalBufferMaterialization);
    Run("guarded scalar descriptor reads", TestGuardedScalarDescriptorReads);
    Run("conservative buffer reachability", TestConservativeBufferReachability);
    Run("conditional indirect image", TestConditionalIndirectImageMaterialization);
    Run("shader info collection", TestShaderInfoCollection);
    Run("image binding ABI", TestImageBindingAbi);
    Run("live shader data collection", TestLiveShaderDataCollection);
    Run("resource limit", TestResourceLimitIsTransactional);
    Run("malformed memory kinds", TestMalformedMemoryKindsRejected);
  } catch (const std::exception &exception) {
    std::cerr << "resource tracking test failed: " << exception.what() << '\n';
    return 1;
  }
  std::cout << "resource tracking tests passed\n";
  return 0;
}

// The full emulator supplies these assertion hooks through common. This focused
// target links only fmt; keep assertion failures observable without widening
// its focused build manifest.
namespace Common {
int DbgExitHandler(const char *, int, std::string_view text) {
  throw std::runtime_error(std::string(text));
}

int DbgExitHandler(const char *, int, fmt::text_style, std::string_view text) {
  throw std::runtime_error(std::string(text));
}

int DbgExitIfHandler(const char *expression, const char *file, int line) {
  throw std::runtime_error(std::string("typed IR assertion: ") + expression +
                           " at " + file + ':' + std::to_string(line));
}

int DbgNotImplementedHandler(const char *expression, const char *file,
                             int line) {
  throw std::runtime_error(std::string("typed IR not implemented: ") +
                           expression + " at " + file + ':' +
                           std::to_string(line));
}

void DbgExit(int) { throw std::runtime_error("typed IR assertion failed"); }
} // namespace Common

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.cpp"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.cpp"
