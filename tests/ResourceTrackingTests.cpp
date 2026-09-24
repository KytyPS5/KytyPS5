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
#include <optional>
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

// A table's specialization names a directory slot in the flattened SRT, and the slot holds the
// table's real mapping offset - the offset is an allocation result and would respecialize every
// table after it if it reached the module. Tests that read a mapping take the same hop the module
// does.
uint32_t MappingOf(const ResourceSnapshot &snapshot, uint32_t slot) {
  return slot < snapshot.flattened_srt.size() ? snapshot.flattened_srt[slot]
                                              : std::numeric_limits<uint32_t>::max();
}

bool SameResourceSnapshot(const ResourceSnapshot &lhs,
                          const ResourceSnapshot &rhs) {
  return lhs.buffers == rhs.buffers && lhs.images == rhs.images &&
         lhs.samplers == rhs.samplers &&
         lhs.flattened_srt == rhs.flattened_srt &&
         lhs.user_data == rhs.user_data && lhs.uniform_fill == rhs.uniform_fill;
}

// Upstream replaced the free EvaluateDescriptorSource with the SrtWalker class; this keeps
// the one-source form the tests are written against.
bool EvaluateDescriptorSource(const ResourcePlan &program, uint32_t source,
                              const SrtRuntime &runtime, DescriptorValue &result) {
  SrtWalker clean(program, CleanRuntime(runtime));
  SrtWalker walker(program, runtime, program.clean_flat_slots, &clean);
  return walker.EvaluateDescriptor(source, result);
}

// The null shape arms sit at the end of their table, so these indices hold.
size_t Materialized(const ResourceSpecialization &specialization) {
  return static_cast<size_t>(std::ranges::count_if(
      specialization.images, [](const auto &image) { return !image.shape_padding; }));
}

bool SameResourceSnapshot(const ResourceSnapshot &lhs,
                          const ResourceSnapshot &rhs) {
  return lhs.buffers == rhs.buffers && lhs.images == rhs.images &&
         lhs.samplers == rhs.samplers &&
         lhs.flattened_srt == rhs.flattened_srt &&
         lhs.user_data == rhs.user_data && lhs.uniform_fill == rhs.uniform_fill;
}

template <typename F>
void CheckTrackingRejected(F &fixture, std::string_view expected,
                           const char *message) {
  const auto status = fixture.TryPlanAndTrack();
  Check(!status.ok && status.reason.find(expected) != std::string::npos, message);
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
  // A rejected tracking pass may leave the program half planned, so later checks reuse its status.
  std::optional<ResourceTrackingStatus> rejected;

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

  ResourceTrackingStatus TryPlanAndTrack() {
    if (rejected)
      return *rejected;
    for (size_t index = 0; index < program.block_info.size(); ++index) {
      const auto condition = program.block_info[index].condition;
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
    auto status = TrackResources(program, {}, {});
    if (!status.ok)
      rejected = status;
    return status;
  }

  void PlanAndTrack() {
    const auto status = TryPlanAndTrack();
    if (!status.ok) {
      throw std::runtime_error(status.reason);
    }
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
MakeIndirectImageFixture(bool malformed, uint32_t material_immediate = 4,
                         bool memory_backed_material = false, uint32_t member_offset = 0,
                         uint32_t material_stride = 224, uint32_t heap_stride = 32u,
                         uint32_t heap_record_offset = 0u,
                         uint32_t selector_bits = UINT32_MAX) {
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
  if (selector_bits != UINT32_MAX)
    key = fixture->Emit(ValueOpcode::BitwiseAnd32, {key, Value(selector_bits)});
  // A heap of plain T# strides by 32 and the shader shifts; a heap that keeps a T# inside a
  // larger per-record struct strides by the record and the shader multiplies.
  const auto heap_offset =
      heap_stride == 32u
          ? fixture->Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)})
          : fixture->Emit(ValueOpcode::IMul32, {key, Value(heap_stride)});
  std::array<Value, 8> image_words;
  MemoryInfo heap_scalar;
  heap_scalar.kind = ResourceKind::ScalarBuffer;
  for (uint32_t dword = 0; dword < image_words.size(); dword++) {
    auto component = heap_scalar;
    component.offset = heap_record_offset + dword * sizeof(uint32_t);
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
  const auto sampler =
      fixture->Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}, 0x10f0);
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  const auto sampled = fixture->Emit(ValueOpcode::ImageSampleRaw,
                                     {image, sampler, fixture->ImageAddress(), Value(true)},
                                     fixture->AddMemory(sample, 0x10f0));
  const auto sampled_x =
      fixture->Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
  fixture->Emit(ValueOpcode::ReferenceU32, {sampled_x});
  return fixture;
}

std::unique_ptr<Fixture> MakeIndirectBufferFixture(uint32_t record_stride,
                                                   bool broken_offset) {
  auto fixture = std::make_unique<Fixture>();
  std::array<Value, 4> heap_words;
  for (uint32_t dword = 0; dword < 4; dword++) {
    heap_words[dword] = fixture->UserData(dword);
  }
  const auto heap = fixture->Buffer(heap_words, 0x0238);
  // A lane index the host cannot re-execute: without the indirect recognizer the
  // table V# is not a valid runtime value and the shader is dropped.
  const auto lane = fixture->Emit(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)),
       Value(0u)});
  const auto selector =
      fixture->Emit(ValueOpcode::ReadFirstLane, {lane, Value(true)});
  const auto record =
      fixture->Emit(ValueOpcode::IMul32, {selector, Value(record_stride)});
  std::array<Value, 4> table_words;
  MemoryInfo heap_scalar;
  heap_scalar.kind = ResourceKind::ScalarBuffer;
  for (uint32_t dword = 0; dword < table_words.size(); dword++) {
    auto component = heap_scalar;
    component.offset = 8u + dword * sizeof(uint32_t);
    if (broken_offset && dword == table_words.size() - 1u) {
      component.offset += sizeof(uint32_t);
    }
    table_words[dword] =
        fixture->Emit(ValueOpcode::ReadConstBuffer, {heap, record},
                      fixture->AddMemory(component, 0x0264));
  }
  const auto table = fixture->Buffer(table_words, 0x0270);
  MemoryInfo load;
  load.kind = ResourceKind::Buffer;
  const auto value =
      fixture->Emit(ValueOpcode::LoadBufferU32,
                    {table, Value(0u), Value(0u), Value(0u), Value(true)},
                    fixture->AddMemory(load, 0x0270));
  fixture->Emit(ValueOpcode::ReferenceU32, {value});
  return fixture;
}

void TestIndirectBufferMaterialization() {
  constexpr uint32_t kStride = 120u;
  auto fixture = MakeIndirectBufferFixture(kStride, false);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);

  Check(fixture->program.info.buffers.size() == 2,
        "indirect buffer table did not keep its own scalar heap resource");
  const auto heap_source = fixture->program.info.buffers[0].source;
  const auto table_source = fixture->program.info.buffers[1].source;
  Check(!fixture->program.descriptor_sources[heap_source]
             .indirect_buffer.has_value() &&
            fixture->program.descriptor_sources[table_source]
                .indirect_buffer.has_value(),
        "indirect buffer source was not recognized");
  const auto &indirect =
      *fixture->program.descriptor_sources[table_source].indirect_buffer;
  Check(indirect.heap_source == heap_source &&
            indirect.selector_stride == kStride &&
            indirect.selector_offset == 0u && indirect.record_offset == 8u,
        "indirect buffer description lost the record layout");

  // A byte-addressed table of two 120-byte records, each holding one V# at
  // byte 8 of the record.
  std::array<uint32_t, 4> user_data{0x1000u, 0u, 2u * kStride, 0u};
  LinearTestMemory memory;
  const std::array<uint32_t, 4> descriptor{0x1800u, 0u, 64u,
                                           Libs::Graphics::DstSel(4, 5, 6, 7)};
  const auto word = [&](uint64_t address) {
    return (address - memory.base) / 4u;
  };
  for (uint32_t record = 0; record < 2u; record++) {
    for (uint32_t dword = 0; dword < descriptor.size(); dword++) {
      memory.words[word(0x1000u + record * kStride + 8u) + dword] =
          descriptor[dword];
    }
  }
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            snapshot.buffers.size() == 2 &&
            std::equal(descriptor.begin(), descriptor.end(),
                       snapshot.buffers[1].dwords.begin()),
        "single-candidate indirect buffer table did not materialize");

  // A second distinct record - an empty slot counts, since it selects nothing -
  // needs a runtime selection stage one cannot express.
  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  for (uint32_t dword = 0; dword < descriptor.size(); dword++) {
    memory.words[word(0x1000u + kStride + 8u) + dword] = 0u;
  }
  Check(!MaterializeResources(resource_plan, runtime, snapshot,
                              specialization) &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "a two-candidate indirect buffer table was accepted by stage one");

  // Two live descriptors split the table the same way.
  memory.words[word(0x1000u + kStride + 8u)] = 0x1900u;
  memory.words[word(0x1000u + kStride + 8u) + 2u] = 64u;
  memory.words[word(0x1000u + kStride + 8u) + 3u] =
      Libs::Graphics::DstSel(4, 5, 6, 7);
  Check(!MaterializeResources(resource_plan, runtime, snapshot,
                              specialization) &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "two live indirect buffer candidates were accepted by stage one");

  // A table whose declared stride is neither the selector stride nor byte
  // addressing is refused rather than probed.
  user_data[1] = (kStride + 8u) << 16u;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "an indirect buffer table with a foreign stride was probed");

  auto broken = MakeIndirectBufferFixture(kStride, true);
  CheckTrackingRejected(*broken, "not a valid runtime value",
                        "a non-consecutive table read was accepted");
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
  // Image 0 is the miss slot and is always null; the enumerated keys start at 1.
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            Materialized(specialization) == 2 &&
            std::ranges::all_of(snapshot.images[0].dwords,
                                [](uint32_t dword) { return dword == 0u; }) &&
            std::equal(image_descriptor.begin(), image_descriptor.end(),
                       snapshot.images[1].dwords.begin()),
        "invariant indirect image table did not materialize");

  user_data[5] = 0u;
  user_data[6] = 19u;
  memory.fail_address = 0x2010u;
  memory.watched_address = 0x2000u;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            memory.watched_dwords == 4u &&
            std::equal(image_descriptor.begin(), image_descriptor.end(),
                       snapshot.images[1].dwords.begin()),
        "partial scalar-buffer descriptor read crossed bounds instead of zeroing its tail");
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
  memory.words[(0x2000u - memory.base) / 4u + 1u] = image_descriptor[1];
  memory.words[(0x2000u - memory.base) / 4u + 3u] = image_descriptor[3];
  memory.words[(0x2020u - memory.base) / 4u + 1u] = image_descriptor[1];
  memory.words[(0x2020u - memory.base) / 4u + 3u] =
      image_descriptor[3] ^ (1u << 28u);
  ResourceSnapshot null_snapshot;
  ResourceSpecialization null_specialization;
  Check(MaterializeResources(resource_plan, runtime, null_snapshot,
                             null_specialization) &&
            std::ranges::all_of(null_snapshot.images[0].dwords,
                                [](uint32_t dword) { return dword == 0u; }),
        "stale typed null image descriptors were not canonicalized");

  for (uint32_t dword = 0; dword < image_descriptor.size(); dword++) {
    memory.words[(0x2000u - memory.base) / 4u + dword] =
        image_descriptor[dword];
    memory.words[(0x2020u - memory.base) / 4u + dword] =
        image_descriptor[dword];
  }
  memory.words[(0x2020u - memory.base) / 4u] ^= 1u;
  memory.words[(0x1000u - memory.base + 36u) / 4u] = 1u;
  ResourceSnapshot dynamic_snapshot;
  ResourceSpecialization dynamic_specialization;
  Check(MaterializeResources(resource_plan, runtime, dynamic_snapshot,
                             dynamic_specialization) &&
            Materialized(dynamic_specialization) == 3 &&
            dynamic_snapshot.images.size() == dynamic_specialization.images.size(),
        "dynamic indirect image table did not materialize");
  const auto second_image = (0x2020u - memory.base) / 4u;
  memory.words[second_image + 1u] |= 3u << 30u;
  memory.words[second_image + 2u] = 3u << 14u;
  memory.words[second_image + 3u] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kCube) << 28u);
  memory.words[second_image + 4u] = 11u;
  ResourceSnapshot mixed_snapshot;
  ResourceSpecialization mixed_specialization;
  Check(MaterializeResources(resource_plan, runtime, mixed_snapshot,
                             mixed_specialization) &&
            mixed_snapshot.images.size() == 3 &&
            mixed_specialization.images.size() == 3 &&
            mixed_specialization.images[1].dimension ==
                Decoder::ImageDimension::Dim2D &&
            !mixed_specialization.images[1].cube &&
            mixed_specialization.images[2].dimension ==
                Decoder::ImageDimension::Dim2DArray &&
            mixed_specialization.images[2].cube &&
            std::equal(mixed_snapshot.images[2].dwords.begin(),
                       mixed_snapshot.images[2].dwords.end(),
                       memory.words.begin() + second_image),
        "mixed 2D and cube candidates were rejected or discarded");
  memory.words[second_image + 1u] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32UInt)
          << 20u |
      (3u << 30u);
  Check(!MaterializeResources(resource_plan, runtime, mixed_snapshot,
                              mixed_specialization),
        "indirect images with different numeric classes were accepted");
  for (const auto dword : {1u, 2u, 3u, 4u}) {
    memory.words[second_image + dword] = image_descriptor[dword];
  }
  ApplyResourceSpecialization(fixture->program, dynamic_specialization);
  // Null miss slot, the two keys, then a null cube arm no key names.
  Check(fixture->program.info.images.size() == 4 &&
            fixture->program.info.images[0].indirect_root == 0 &&
            fixture->program.info.images[0].indirect_search_iterations != 0 &&
            fixture->program.info.images[0].indirect_resources.size() == 4 &&
            fixture->program.info.images[3].cube &&
            dynamic_snapshot.images.size() == 4,
        "dynamic indirect image table was not specialized transactionally");
  const auto &mapping = dynamic_specialization.images[0];
  const auto mapping_offset =
      MappingOf(dynamic_snapshot, mapping.indirect_mapping_offset);
  const auto key_count = dynamic_snapshot.flattened_srt[mapping_offset];
  // The depth is one constant; the mapping holds exactly the keys, then a slot word per ordinal.
  const auto slot_words = mapping_offset + 1u + key_count * 2u;
  Check(mapping.indirect_search_iterations == IndirectSearchIterations &&
            slot_words < dynamic_snapshot.flattened_srt.size() &&
            dynamic_snapshot.flattened_srt[slot_words] == 3u &&
            slot_words + 1u + 3u == dynamic_snapshot.flattened_srt.size(),
        "indirect image mapping retained worst-case padding");

  for (uint32_t dword = 0; dword < image_descriptor.size(); dword++) {
    memory.words[(0x2000u - memory.base) / 4u + dword] =
        image_descriptor[dword];
    memory.words[(0x2020u - memory.base) / 4u + dword] =
        image_descriptor[dword];
  }
  memory.words[(0x2000u - memory.base) / 4u] += 0x100u;
  memory.words[(0x2020u - memory.base) / 4u] += 0x101u;
  ResourceSnapshot rebound_snapshot;
  ResourceSpecialization rebound_specialization;
  Check(MaterializeResources(resource_plan, runtime, rebound_snapshot,
                             rebound_specialization) &&
            rebound_specialization == dynamic_specialization,
        "stable indirect key mapping did not accept changed image addresses");
  memory.words[(0x2020u - memory.base) / 4u] =
      memory.words[(0x2000u - memory.base) / 4u];
  Check(MaterializeResources(resource_plan, runtime, rebound_snapshot,
                             rebound_specialization) &&
            rebound_specialization != dynamic_specialization,
        "collapsed indirect candidates did not select a new specialization");
  const auto collapsed_specialization = rebound_specialization;
  ResourceSnapshot capacity_snapshot;
  ResourceSpecialization capacity_specialization;
  for (const uint32_t records : {1u, 3u}) {
    user_data[2] = records;
    Check(MaterializeResources(resource_plan, runtime, capacity_snapshot,
                               capacity_specialization),
          "runtime indirect key mapping rejected a valid material-table size");
  }
  user_data[2] = 2u;
  memory.words[(0x2020u - memory.base) / 4u] =
      memory.words[(0x2000u - memory.base) / 4u] + 1u;
  memory.words[(0x2040u - memory.base) / 4u] =
      memory.words[(0x2000u - memory.base) / 4u] + 2u;
  for (uint32_t dword = 1; dword < image_descriptor.size(); dword++) {
    memory.words[(0x2040u - memory.base) / 4u + dword] =
        image_descriptor[dword];
  }
  memory.words[(0x1000u - memory.base + 68u) / 4u] = 2u;
  Check(MaterializeResources(resource_plan, runtime, rebound_snapshot,
                             rebound_specialization) &&
            rebound_specialization != collapsed_specialization,
        "larger indirect candidate topology reused the old specialization");

  auto memory_backed = MakeIndirectImageFixture(false, 4u, true);
  memory_backed->PlanAndTrack();
  auto memory_backed_plan = ExtractResourcePlan(memory_backed->program);
  EliminateDeadCode(memory_backed->program.blocks);
  std::array<uint32_t, 11> memory_backed_user_data{0x1000u, 224u << 16u, 2u, 0u,
                                                   0x2000u, 16u << 16u,  4u, 0u,
                                                   7u,      0x3100u,     0u};
  memory.words[(0x3100u - memory.base) / 4u] = 0x3000u;
  memory.words[(0x3000u - memory.base) / 4u] = 0x1000u;
  memory.fail_address = 0x3100u;
  SrtRuntime memory_backed_runtime{.user_data = memory_backed_user_data,
                                   .userdata = &memory,
                                   .read_specialization_memory =
                                       ReadLinearTestMemory};
  Check(!MaterializeResources(memory_backed_plan, memory_backed_runtime,
                              snapshot, specialization),
        "unreadable indirect table descriptor was accepted");
  memory.fail_address = UINT64_MAX;

  memory.watched_address = 0x3100u;
  memory.watched_reads = 0;
  Check(MaterializeResources(memory_backed_plan, memory_backed_runtime,
                             snapshot, specialization) && memory.watched_reads == 1u,
        "descriptor, flattened SRT and indirect-table roots repeated a clean pointer read");
  const auto* buffer_storage = snapshot.buffers.data();
  const auto* image_storage = snapshot.images.data();
  const auto* flat_storage = snapshot.flattened_srt.data();
  const auto* user_data_storage = snapshot.user_data.data();
  const auto* buffer_specialization_storage = specialization.buffers.data();
  const auto* image_specialization_storage = specialization.images.data();
  const auto old_address = snapshot.images[1].dwords[0];
  memory.words[(0x2000u - memory.base) / 4u] += 0x100u;
  memory.watched_reads = 0;
  Check(MaterializeResources(memory_backed_plan, memory_backed_runtime,
                             snapshot, specialization) && memory.watched_reads == 1u &&
            snapshot.images[1].dwords[0] == old_address + 0x100u,
        "cache refresh reused stale table contents or repeated its clean pointer read");
  Check(snapshot.buffers.data() == buffer_storage && snapshot.images.data() == image_storage &&
            snapshot.flattened_srt.data() == flat_storage &&
            snapshot.user_data.data() == user_data_storage &&
            specialization.buffers.data() == buffer_specialization_storage &&
            specialization.images.data() == image_specialization_storage,
        "a same-shape refresh discarded the runtime output storage");

  auto malformed = MakeIndirectImageFixture(true);
  CheckFatal([&] { malformed->PlanAndTrack(); }, "not a valid runtime value",
             "noncontiguous descriptor DWORDs entered the table plan");
  Check(!malformed->program.resource_tracking_complete && malformed->program.descriptor_sources.empty(),
        "malformed image table tracking was not transactional");
}

  CheckTrackingRejected(*malformed, "not a valid runtime value",
                        "malformed indirect image pattern was accepted");
  Check(!malformed->program.resource_tracking_complete &&
            malformed->program.info.images.empty() &&
            malformed->program.descriptor_sources.empty(),
        "malformed indirect image pattern was partially accepted");

  auto negative_immediate = MakeIndirectImageFixture(false, 0xfffffffcu);
  CheckFatal([&] { negative_immediate->PlanAndTrack(); },
             "not a valid runtime value",
             "negative scalar immediate entered the indirect image proof");

  for (const auto [immediate, member, first, stride] :
       {std::array{0u, 4u, 4u, 224u}, std::array{4u, 4u, 8u, 224u},
        std::array{36u, 0u, 36u, 224u}, std::array{1u, 3u, 0u, 224u},
        std::array{1u, 3u, 0u, 1u}, std::array{1u, 3u, 0u, 2u}}) {
    auto split_offset = MakeIndirectImageFixture(false, immediate, false, member, stride);
    split_offset->PlanAndTrack();
    const auto split_plan = ExtractResourcePlan(split_offset->program);
    LinearTestMemory split_memory;
    std::copy(image_descriptor.begin(), image_descriptor.end(),
              split_memory.words.begin() + 0x1000u / 4u);
    split_memory.fail_address = first == 36u ? 0x1004u : UINT64_MAX;
    split_memory.watched_address = split_memory.base + first;
    user_data[1] = stride << 16u;
    user_data[2] = stride < 4u ? 8u / stride : 1u;
    SrtRuntime split_runtime{.user_data = user_data,
                             .userdata = &split_memory,
                             .read_specialization_memory = ReadLinearTestMemory};
    // Image 0 is the miss slot and is always null; the enumerated key is image 1.
    Check(MaterializeResources(split_plan, split_runtime, snapshot, specialization) &&
              Materialized(specialization) == 2 &&
              std::ranges::all_of(snapshot.images[0].dwords,
                                  [](uint32_t word) { return word == 0u; }) &&
              snapshot.images[1].dwords == image_descriptor &&
              split_memory.watched_reads == 1u,
          "indirect scalar offsets did not align and bound their components independently");
  }

  // The material word need not start its record: the scalar load's own immediate is recorded,
  // so this shape is recognized rather than refused.
  auto wrapped_immediate = MakeIndirectImageFixture(false, 4u);
  wrapped_immediate->PlanAndTrack();
  Check(wrapped_immediate->program.resource_tracking_complete &&
            wrapped_immediate->program.info.images.size() == 1,
        "a material read at a nonzero immediate was refused");
  const auto wrapped_source = wrapped_immediate->program.info.images[0].source;
  Check(wrapped_source < wrapped_immediate->program.descriptor_sources.size() &&
            wrapped_immediate->program.descriptor_sources[wrapped_source]
                    .indirect_descriptor.has_value() &&
            wrapped_immediate->program.descriptor_sources[wrapped_source]
                    .indirect_descriptor->material_offset == 4u,
        "the material read's immediate was not carried to the enumeration");

  // The shader packs flags above a 15-bit cube-image key in the final DWORD of
  // each 64-byte material record; the image occupies bytes 16..47 of its record.
  auto packed = MakeIndirectImageFixture(false, 60u, false, 0u, 64u, 48u, 16u, 0x7fffu);
  packed->PlanAndTrack();
  const auto packed_plan = ExtractResourcePlan(packed->program);
  LinearTestMemory packed_memory;
  std::array<uint32_t, 8> packed_data{0x1000u, 64u << 16u, 3u, 0u,
                                      0x2000u, 48u << 16u, 2u, 0u};
  for (uint32_t i = 0; i < 3u; ++i)
    packed_memory.words[(i * 64u + 60u) / 4u] = (0xffffu << 15u) | (i & 1u);
  for (uint32_t i = 0; i < 2u; ++i) {
    auto descriptor = image_descriptor;
    descriptor[0] += i;
    std::copy(descriptor.begin(), descriptor.end(),
              packed_memory.words.begin() + (0x1000u + i * 48u + 16u) / 4u);
  }
  SrtRuntime packed_runtime{.user_data = packed_data, .userdata = &packed_memory,
                            .read_specialization_memory = ReadLinearTestMemory};
  // Image 0 is the miss slot; the mapping is reached through the directory slot.
  const auto packed_mapping = [&] {
    return snapshot.flattened_srt[snapshot.flattened_srt[specialization.images[0].indirect_mapping_offset]];
  };
  Check(MaterializeResources(packed_plan, packed_runtime, snapshot, specialization) &&
            snapshot.images.size() >= 3u && snapshot.images[1].dwords == image_descriptor &&
            snapshot.images[2].dwords[0] == image_descriptor[0] + 1u && packed_mapping() == 2u,
        "packed material flags changed the table key or 48-byte descriptor addressing");
  packed_memory.words[60u / 4u] = 0xffff8002u;
  packed_memory.fail_address = 0x2070u;
  Check(MaterializeResources(packed_plan, packed_runtime, snapshot, specialization) &&
            packed_mapping() == 3u &&
            std::ranges::all_of(snapshot.images[0].dwords, [](uint32_t word) { return word == 0u; }),
        "masked out-of-range descriptor did not return zero without reading past table bounds");
  packed_memory.fail_address = 0x205cu;
  Check(!MaterializeResources(packed_plan, packed_runtime, snapshot, specialization),
        "unreadable in-range DWORD in a 48-byte descriptor record was accepted");
  // The record offset wraps in U32 and the scalar immediate is added after it, so an
  // unmasked key at a nonzero record offset still plans.
  auto overflowing = MakeIndirectImageFixture(false, 60u, false, 0u, 64u, 48u, 16u);
  overflowing->PlanAndTrack();
  Check(overflowing->program.resource_tracking_complete,
        "an unmasked key at a nonzero record offset was refused");
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
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress(), Value(true)},
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
            Materialized(specialization) == 33u &&
            snapshot.images.size() == specialization.images.size() &&
            snapshot.flattened_srt[MappingOf(snapshot, specialization.images[0].indirect_mapping_offset)] == 32u &&
            memory.reads == 34u && memory.descriptor_reads == 32u,
        "direct table did not retain all 32 reachable descriptors");
  const auto captured_word = (table - memory.base) / 4u + 16u * 8u;
  const auto original_descriptor = snapshot.images[17].dwords;
  const std::array<uint32_t, 8> captured_invalid{
      0x101f0000u, 0xcb500000u, 0x001fc01fu, 0xd0970facu,
      0x86000000u, 0x00500003u, 0x00000400u, 0x00005204u};
  std::copy(captured_invalid.begin(), captured_invalid.end(),
             memory.words.begin() + captured_word);
  // The record no longer parses, so its key maps to the miss slot rather than to an image of its
  // own: one fewer candidate, the same 32 keys, and its slot stays in the bucket as null.
  const auto mapping_base =
      MappingOf(snapshot, specialization.images[0].indirect_mapping_offset);
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            Materialized(specialization) == 33u &&
            std::ranges::all_of(snapshot.images[32].dwords,
                                [](uint32_t word) { return word == 0u; }) &&
            snapshot.flattened_srt[mapping_base] == 32u &&
            snapshot.flattened_srt[mapping_base + 2u + 16u * 2u] == 0u &&
            std::ranges::all_of(snapshot.images[0].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "captured non-descriptor record became a host image or lost its key mapping");
  std::copy(original_descriptor.begin(), original_descriptor.end(),
             memory.words.begin() + captured_word);
  memory.words[(table - memory.base) / 4u + 31u * 8u] = 0x987u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images[32].dwords[0] == 0x987u,
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
                 {image, sampler, fixture.ImageAddress(), Value(true)},
                 fixture.AddMemory(sample, 0x29c));
    fixture.PlanAndTrack();
    const auto source = fixture.program.info.images[0].source;
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_descriptor;
    const bool bounded = variant == Variant::Bounded || variant == Variant::Plain ||
                         variant == Variant::Nonzero || variant == Variant::TrueEdge ||
                         variant == Variant::Masked || variant == Variant::GuardedDiamond;
    if (bounded) {
      Check(indirect && indirect->material_source == UINT32_MAX &&
                indirect->table_offset == 0x6b0u &&
                indirect->key_count.Resolve() == count.Resolve(),
            "bounded compute loop lost its runtime image count");
    } else {
      Check(!indirect,
            "an unbounded compute image loop was matched as an indirect table");
    }
    return ExtractResourcePlan(fixture.program);
  };

  auto plan = make_plan(Variant::Bounded);
  make_plan(Variant::Plain);
  make_plan(Variant::Nonzero);
  make_plan(Variant::TrueEdge);
  make_plan(Variant::Masked);
  make_plan(Variant::GuardedDiamond);

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
  // None of these shapes bounds the loop key, so the table match refuses them. Tracking may
  // refuse the descriptor outright, or accept its words through the value the loop is entered
  // with and leave the refusal to the evaluator fixpoint proof, which finds the key advances.
  // Either way it must never be served.
  for (const auto variant :
       {Variant::WrongGuard, Variant::EntryBypass, Variant::ExitBypass, Variant::GuardBlock,
        Variant::WrongStep, Variant::IncrementBypass, Variant::PreviousBound,
        Variant::MaskedWrongGuard, Variant::MaskResurrection, Variant::StatusOverwrite,
        Variant::GuardedDiamondBypass, Variant::DivergentBound, Variant::DivergentKey,
        Variant::Disjunction, Variant::WrongPolarity}) {
    std::optional<ResourcePlan> unbounded;
    try {
      unbounded = make_plan(variant);
    } catch (const std::runtime_error &error) {
      if (std::string_view(error.what()).find("not a valid runtime value") ==
          std::string_view::npos) {
        throw;
      }
    }
    if (unbounded) {
      ResourceSnapshot unbounded_snapshot;
      ResourceSpecialization unbounded_specialization;
      Check(!MaterializeResources(*unbounded, runtime, unbounded_snapshot,
                                  unbounded_specialization),
            "an unbounded compute image loop was materialized from its entry value");
    }
  }
  for (const uint32_t count : {2u, 3u, 2u}) {
    user_data[2] = count;
    // count keys, padded to their bucket, plus the miss slot the search falls back to.
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              Materialized(specialization) == std::bit_ceil(count) + 1u &&
              snapshot.images.size() == specialization.images.size() &&
              snapshot.flattened_srt[MappingOf(
                  snapshot, specialization.images[0].indirect_mapping_offset)] == count &&
              snapshot.images[count].dwords[0] == 0x100u + count - 1u,
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
    const auto sampled = fixture.Emit(ValueOpcode::ImageSampleRaw,
        {image, sampler, address,
         variant == Variant::EmptyFirstEntry ? material_guard : sample_guard},
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
            Materialized(specialization) == 3u &&
            memory.reads == 4u && memory.descriptor_reads == 2u &&
            snapshot.flattened_srt[MappingOf(
                snapshot, specialization.images[0].indirect_mapping_offset)] == 2u &&
            snapshot.flattened_srt[MappingOf(
                snapshot, specialization.images[0].indirect_mapping_offset) + 1u] == 7u &&
            snapshot.flattened_srt[MappingOf(
                snapshot, specialization.images[0].indirect_mapping_offset) + 3u] == 4096u,
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
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress(), Value(true)},
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
                {image, sampler, fixture.ImageAddress(), Value(true)}, fixture.AddMemory(sample, 0x228));
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
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, image_address, Value(true)},
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
               {image, sampler, fixture.ImageAddress(), Value(true)},
               fixture.AddMemory(memory, 0x1ec));
  // PPSA24156's zero border payload leaves the reserved scratch shift as
  // the entire DWORD after the frontend folds its OR with zero.
  const auto shift_sampler = fixture.Sampler(
      {Value(0x36u), Value(0xfff000u), Value(0x02500000u), scratch}, 0x200);
  fixture.Emit(ValueOpcode::ImageSampleRaw,
               {image, shift_sampler, fixture.ImageAddress(), Value(true)},
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
                  {rejected_image, rejected_sampler, rejected.ImageAddress(), Value(true)},
                  rejected.AddMemory(rejected_memory, 0x200));

    CheckTrackingRejected(rejected, "not a valid runtime value", message);
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
                      sampler_words[1], sampler_words[2], Value(true)},
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

// A MUBUF load without index or VGPR offset still follows its V#: a swizzled
// buffer spreads an element's dwords index_stride apart, and add_tid indexes
// by thread ID.
void TestUniformBufferReadFollowsBufferAddressing() {
  Fixture fixture;
  const auto table = fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                                     fixture.UserData(2), fixture.UserData(3)},
                                    4);
  MemoryInfo field;
  field.kind = ResourceKind::Buffer;
  field.offset = 4;
  const auto base =
      fixture.Emit(ValueOpcode::LoadBufferU32,
                   {table, Value(0u), Value(0u), Value(0u), Value(true)},
                   fixture.AddMemory(field, 4));
  const auto descriptor =
      fixture.Buffer({base, Value(0u), Value(64u), Value(0u)}, 8);
  MemoryInfo load;
  load.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {descriptor, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(load, 8));
  fixture.PlanAndTrack();

  LinearTestMemory memory;
  memory.words[1] = 0x1111u;
  memory.words[8] = 0x2222u;
  const auto Resolve = [&](std::array<uint32_t, 4> user_data, uint32_t &word) {
    const SrtRuntime runtime{.user_data = user_data,
                             .read_memory = ReadLinearTestMemory,
                             .userdata = &memory,
                             .read_specialization_memory = ReadLinearTestMemory};
    for (const auto &buffer : fixture.program.info.buffers) {
      DescriptorValue value;
      if (EvaluateDescriptorSource(fixture.program, buffer.source, runtime,
                                   value) &&
          value.dwords[2] == 64u) {
        word = value.dwords[0];
        return true;
      }
    }
    return false;
  };

  uint32_t word = 0;
  Check(Resolve({0x1000u, 8u << 16u, 8u, 0u}, word) && word == 0x1111u,
        "a linear uniform buffer read did not resolve at its byte offset");
  // Index stride 8: byte 4 of element 0 lives at (4 & ~3) * 8 = byte 32.
  Check(Resolve({0x1000u, (1u << 31u) | (8u << 16u), 8u, 0u}, word) &&
            word == 0x2222u,
        "a swizzled uniform buffer read ignored the swizzled layout");
  Check(!Resolve({0x1000u, 8u << 16u, 8u, 1u << 23u}, word),
        "an add_tid uniform buffer read was resolved to one lane's record");
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

  CheckTrackingRejected(fixture, "not a valid runtime value",
                        "control-dependent writable descriptor phi was accepted");
  CheckTrackingRejected(fixture, "two entry operands disagree",
                        "a merge of two descriptors was not named as a merge");
  CheckTrackingRejected(fixture, "entries immediate and immediate",
                        "a merge did not name the two operands that disagree");
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
        fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, address, Value(true)},
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
  fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress(), Value(true)},
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
      fixture.Emit(ValueOpcode::ImageSampleRaw, {image, previous, fixture.ImageAddress(), Value(true)},
                   fixture.AddMemory(memory, 0x440u));
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress(), Value(true)},
                 fixture.AddMemory(memory, 0x1dd0u));
    if (format == F::k32Float)
      fixture.Emit(ValueOpcode::ImageSampleRaw, {image, direct, fixture.ImageAddress(), Value(true)},
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
    adjusted.Emit(ValueOpcode::ImageSampleRaw, {image, shared, adjusted.ImageAddress(), Value(true)},
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
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress(), Value(true)},
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

// A phi web nothing enters from outside is the shape only a GPU-side descriptor can serve, and
// the log has to say so: it is not the same defect as two entry values that merely disagree.
void TestLoopPhiWithNoEntryValue() {
  Fixture fixture;
  auto *entry = fixture.block;
  auto *loop = fixture.AddBlock();
  entry->AddBranch(loop);
  loop->AddBranch(loop);
  auto &phi = loop->AppendNewInst(ValueOpcode::Phi, {},
                                  static_cast<uint64_t>(Type::U32));
  const auto entered =
      fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(8u)}, 0, entry);
  const auto carried =
      fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(4u)}, 0, loop);
  phi.AddPhiOperand(entry, entered);
  phi.AddPhiOperand(loop, carried);
  const auto handle = fixture.Emit(ValueOpcode::GetBufferResource,
                                   {Value(&phi), Value(0u), Value(0u), Value(0u)},
                                   MemoryFlags{0, 16}, loop);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  // A store: a read through a GPU-selected descriptor now decodes it in the shader.
  fixture.Emit(ValueOpcode::StoreBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
               fixture.AddMemory(memory, 16), loop);

  CheckTrackingRejected(
      fixture, "no operand enters the phi web from outside",
      "a phi web with no entry value was not named as having none");
}

// Two operands enter the web and disagree. The log has to name both, because that is what says
// whether they are two spellings of one descriptor or two genuinely different buffers.
void TestLoopPhiWithDisagreeingEntries() {
  Fixture fixture;
  auto *first = fixture.block;
  auto *second = fixture.AddBlock();
  auto *loop = fixture.AddBlock();
  first->AddBranch(loop);
  second->AddBranch(loop);
  loop->AddBranch(loop);
  const auto direct = fixture.UserData(0);
  const auto masked = fixture.Emit(ValueOpcode::BitwiseAnd32,
                                   {fixture.UserData(1), Value(0xffu)}, 0, second);
  auto &phi = loop->AppendNewInst(ValueOpcode::Phi, {},
                                  static_cast<uint64_t>(Type::U32));
  const auto carried =
      fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(4u)}, 0, loop);
  phi.AddPhiOperand(first, direct);
  phi.AddPhiOperand(second, masked);
  phi.AddPhiOperand(loop, carried);
  const auto handle = fixture.Emit(ValueOpcode::GetBufferResource,
                                   {Value(&phi), Value(0u), Value(0u), Value(0u)},
                                   MemoryFlags{0, 24}, loop);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  // A store: a read through a GPU-selected descriptor now decodes it in the shader.
  fixture.Emit(ValueOpcode::StoreBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
               fixture.AddMemory(memory, 24), loop);

  CheckTrackingRejected(fixture, "entries GetUserData and BitwiseAnd32",
                        "a merge did not name the two instructions that disagree");
}

// RE9 cs_b94b8a45d6d9a74c slot 31, and cs_90bf8b93e0ff9190 / cs_ed77e9e78c5540f8 alike: a scalar
// read whose offset is a constant but whose descriptor the loop carries. A flattened slot holds one
// word for the whole dispatch, so hoisting that read would make every iteration see the record the
// first one named.
void TestCarriedDescriptorReadIsNotFlattened() {
  Fixture fixture;
  auto *entry = fixture.block;
  auto *loop = fixture.AddBlock();
  entry->AddBranch(loop);
  loop->AddBranch(loop);

  // The table the loop walks: its V# is carried, so each iteration names another record.
  auto &phi =
      loop->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
  const auto carried =
      fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(48u)}, 0, loop);
  phi.AddPhiOperand(entry, fixture.UserData(0));
  phi.AddPhiOperand(loop, carried);

  const auto carried_table =
      fixture.Emit(ValueOpcode::GetBufferResource,
                   {Value(&phi), fixture.UserData(1), fixture.UserData(2),
                    fixture.UserData(3)},
                   MemoryFlags{0, 0x20}, loop);
  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarBuffer;
  scalar.offset = 16u;
  const auto carried_read =
      fixture.Emit(ValueOpcode::ReadConstBuffer, {carried_table, Value(0u)},
                   fixture.AddMemory(scalar, 0x24), loop);
  // The read feeds a descriptor, which is what makes planning walk it at all.
  const auto carried_handle =
      fixture.Emit(ValueOpcode::GetBufferResource,
                   {carried_read, carried_read, carried_read, carried_read},
                   MemoryFlags{0, 0x28}, loop);
  MemoryInfo carried_load;
  carried_load.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {carried_handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(carried_load, 0x28), loop);

  // An invariant descriptor read alongside it: this one names one record for the whole dispatch
  // and must still be hoisted, or the flattened SRT stops carrying what it exists to carry.
  const auto steady_table =
      fixture.Emit(ValueOpcode::GetBufferResource,
                   {fixture.UserData(4), fixture.UserData(5),
                    fixture.UserData(6), fixture.UserData(7)},
                   MemoryFlags{0, 0x30}, loop);
  auto steady_scalar = scalar;
  steady_scalar.offset = 32u;
  const auto steady_read =
      fixture.Emit(ValueOpcode::ReadConstBuffer, {steady_table, Value(0u)},
                   fixture.AddMemory(steady_scalar, 0x34), loop);
  const auto steady_handle =
      fixture.Emit(ValueOpcode::GetBufferResource,
                   {steady_read, steady_read, steady_read, steady_read},
                   MemoryFlags{0, 0x38}, loop);
  MemoryInfo steady_load;
  steady_load.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {steady_handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(steady_load, 0x38), loop);

  // Planning is what these checks are about; whether tracking then binds the table is not.
  (void)fixture.TryPlanAndTrack();

  const auto flattened = [&](Value value) {
    return std::any_of(fixture.program.srt_reads.begin(),
                       fixture.program.srt_reads.end(),
                       [&](const SrtRead &read) {
                         return read.value.Resolve() == value.Resolve();
                       });
  };
  Check(!flattened(carried_read),
        "a read through a loop-carried descriptor was hoisted into the flattened SRT");
  Check(flattened(steady_read),
        "a read through an invariant descriptor stopped being hoisted");

  // The rule stops at scalar buffer reads. A raw address read has no table behind it to bind per
  // record, so leaving one to the shader costs the tracker the descriptor it resolves today and
  // buys nothing: RE9's ps_9503fb21376e3f02 goes from tracked to permanently rejected.
  Fixture address_fixture;
  auto *address_entry = address_fixture.block;
  auto *address_loop = address_fixture.AddBlock();
  address_entry->AddBranch(address_loop);
  address_loop->AddBranch(address_loop);
  auto &address_phi = address_loop->AppendNewInst(
      ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
  const auto address_carried = address_fixture.Emit(
      ValueOpcode::IAdd32, {Value(&address_phi), Value(8u)}, 0, address_loop);
  address_phi.AddPhiOperand(address_entry, address_fixture.UserData(0));
  address_phi.AddPhiOperand(address_loop, address_carried);
  const auto carried_address = address_fixture.Emit(
      ValueOpcode::GetAddressResource,
      {Value(&address_phi), address_fixture.UserData(1)}, MemoryFlags{0, 0x40},
      address_loop);
  MemoryInfo address_word;
  address_word.kind = ResourceKind::ScalarAddress;
  const auto address_read = address_fixture.Emit(
      ValueOpcode::LoadAddressU32,
      {carried_address, Value(0u), Value(0u), Value(true)},
      address_fixture.AddMemory(address_word, 0x44), address_loop);
  const auto address_handle = address_fixture.Emit(
      ValueOpcode::GetBufferResource,
      {address_read, address_read, address_read, address_read},
      MemoryFlags{0, 0x48}, address_loop);
  MemoryInfo address_load;
  address_load.kind = ResourceKind::Buffer;
  address_fixture.Emit(
      ValueOpcode::LoadBufferU32,
      {address_handle, Value(0u), Value(0u), Value(0u), Value(true)},
      address_fixture.AddMemory(address_load, 0x48), address_loop);
  (void)address_fixture.TryPlanAndTrack();
  Check(std::any_of(address_fixture.program.srt_reads.begin(),
                    address_fixture.program.srt_reads.end(),
                    [&](const SrtRead &read) {
                      return read.value.Resolve() == address_read.Resolve();
                    }),
        "a carried address read stopped being hoisted, which only loses its descriptor");
}

// RE Engine indexes its material table with an alignment mask on a scalar-memory byte offset
// (`s & -15`), not with a multiply. Hardware forces Dword alignment, so the offsets such a mask can
// reach are exactly the multiples of 16: a record step, which the stride enumeration already walks.
void TestAlignedMaterialSelectorIsARecordStep() {
  for (const uint32_t spelling : {0xfffffff1u, 0xfffffff3u}) {
    Fixture fixture;
    std::array<Value, 4> material_words;
    std::array<Value, 4> heap_words;
    for (uint32_t dword = 0; dword < 4; dword++) {
      material_words[dword] = fixture.UserData(dword);
      heap_words[dword] = fixture.UserData(dword + 4u);
    }
    const auto material = fixture.Buffer(material_words, 0x2000);
    const auto heap = fixture.Buffer(heap_words, 0x2000);
    const auto invocation = fixture.Emit(
        ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)), Value(0u)});
    const auto lane = fixture.Emit(ValueOpcode::ReadFirstLane, {invocation, Value(true)});
    // The guest rounds the byte offset down instead of scaling an index.
    const auto rounded = fixture.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(spelling)});
    MemoryInfo material_scalar;
    material_scalar.kind = ResourceKind::ScalarBuffer;
    const auto key = fixture.Emit(ValueOpcode::ReadConstBuffer, {material, rounded},
                                  fixture.AddMemory(material_scalar, 0x2000));
    const auto record = fixture.Emit(ValueOpcode::IMul32, {key, Value(32u)});
    std::array<Value, 8> descriptor_words;
    for (uint32_t dword = 0; dword < 8; dword++) {
      MemoryInfo heap_scalar;
      heap_scalar.kind = ResourceKind::ScalarBuffer;
      heap_scalar.offset = dword * 4u;
      descriptor_words[dword] =
          fixture.Emit(ValueOpcode::ReadConstBuffer, {heap, record},
                       fixture.AddMemory(heap_scalar, 0x2000));
    }
    const auto image = fixture.Image(descriptor_words, 0x2010);
    const auto sampler =
        fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}, 0x2010);
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    const auto sampled = fixture.Emit(ValueOpcode::ImageSampleRaw,
                                      {image, sampler, fixture.ImageAddress()},
                                      fixture.AddMemory(sample, 0x2010));
    fixture.Emit(ValueOpcode::ReferenceU32,
                 {fixture.Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)})});

    fixture.PlanAndTrack();
    Check(fixture.program.resource_tracking_complete &&
              fixture.program.info.images.size() == 1,
          "an alignment-masked material offset was refused");
    const auto source = fixture.program.info.images[0].source;
    const auto &indirect =
        fixture.program.descriptor_sources[source].indirect_descriptor;
    // Both spellings agree on every bit hardware keeps, so both are a 16-byte step.
    Check(indirect.has_value() && indirect->selector_stride == 16u &&
              indirect->selector_offset == 0u,
          "an alignment-masked material offset did not become a 16-byte record step");
  }
}

// A scattered mask is not a record step and must stay refused - it would make the enumeration walk
// a set of offsets that is not the table's own stride.
void TestScatteredMaterialMaskIsStillRefused() {
  Fixture fixture;
  std::array<Value, 4> material_words;
  std::array<Value, 4> heap_words;
  for (uint32_t dword = 0; dword < 4; dword++) {
    material_words[dword] = fixture.UserData(dword);
    heap_words[dword] = fixture.UserData(dword + 4u);
  }
  const auto material = fixture.Buffer(material_words, 0x2000);
  const auto heap = fixture.Buffer(heap_words, 0x2000);
  const auto invocation = fixture.Emit(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)), Value(0u)});
  const auto lane = fixture.Emit(ValueOpcode::ReadFirstLane, {invocation, Value(true)});
  const auto scattered = fixture.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(0xff00ff00u)});
  MemoryInfo material_scalar;
  material_scalar.kind = ResourceKind::ScalarBuffer;
  const auto key = fixture.Emit(ValueOpcode::ReadConstBuffer, {material, scattered},
                                fixture.AddMemory(material_scalar, 0x2000));
  const auto record = fixture.Emit(ValueOpcode::IMul32, {key, Value(32u)});
  std::array<Value, 8> descriptor_words;
  for (uint32_t dword = 0; dword < 8; dword++) {
    MemoryInfo heap_scalar;
    heap_scalar.kind = ResourceKind::ScalarBuffer;
    heap_scalar.offset = dword * 4u;
    descriptor_words[dword] = fixture.Emit(ValueOpcode::ReadConstBuffer, {heap, record},
                                           fixture.AddMemory(heap_scalar, 0x2000));
  }
  const auto image = fixture.Image(descriptor_words, 0x2010);
  const auto sampler =
      fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}, 0x2010);
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  const auto sampled = fixture.Emit(ValueOpcode::ImageSampleRaw,
                                    {image, sampler, fixture.ImageAddress()},
                                    fixture.AddMemory(sample, 0x2010));
  fixture.Emit(ValueOpcode::ReferenceU32,
               {fixture.Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)})});

  const auto status = fixture.TryPlanAndTrack();
  Check(!status.ok, "a scattered material mask was recognized as a record step");
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

void TestAdvancingLoopPhi() {
  Fixture fixture;
  auto *entry = fixture.block;
  auto *loop = fixture.AddBlock();
  const auto initial = fixture.UserData(0);
  entry->AddBranch(loop);
  loop->AddBranch(loop);
  auto &phi = loop->AppendNewInst(ValueOpcode::Phi, {},
                                  static_cast<uint64_t>(Type::U32));
  const auto carried =
      fixture.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(4u)}, 0, loop);
  phi.AddPhiOperand(entry, initial);
  phi.AddPhiOperand(loop, carried);
  const auto handle = fixture.Emit(ValueOpcode::GetBufferResource,
                                   {Value(&phi), Value(0u), Value(0u), Value(0u)},
                                   MemoryFlags{0, 16}, loop);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(memory, 16), loop);
  fixture.PlanAndTrack();

  std::array<uint32_t, 1> user_data{0x12345678u};
  SrtRuntime runtime{.user_data = user_data};
  DescriptorValue descriptor;
  Check(!EvaluateDescriptorSource(fixture.program,
                                  fixture.program.info.buffers[0].source, runtime,
                                  descriptor),
        "advancing descriptor phi was evaluated to its entry value");
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
  Check(fixture.program.info.writes_dma,
        "a FLAT address store did not report a BDA store");
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
  Check(!fixture.program.info.writes_dma,
        "a FLAT address load reported a BDA store");
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

// RE9 keeps each light's T# at +16 of a 48-byte record, so its heap neither strides by 32 nor
// starts its descriptor at the record. The recognizer used to admit only the 32-byte shift, which
// dropped the whole shader; the enumeration has always been able to walk any stride.
void TestStridedIndirectImageTable() {
  auto fixture = MakeIndirectImageFixture(false, 4u, false, 0u, 224u, 48u, 16u);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);

  Check(fixture->program.info.images.size() == 1,
        "strided indirect image table was not tracked as one image");
  const auto source = fixture->program.info.images[0].source;
  const auto &indirect =
      fixture->program.descriptor_sources[source].indirect_descriptor;
  Check(indirect.has_value() && indirect->heap_stride == 48u &&
            indirect->table_offset == 16u && indirect->selector_stride == 224u,
        "strided indirect image table did not record its record step");

  // Material V# strides by 224 over two records; heap V# spans both 48-byte records.
  std::array<uint32_t, 9> user_data{0x1000u,    224u << 16u, 2u, 0u, 0x2000u,
                                    16u << 16u, 8u,          0u, 7u};
  LinearTestMemory memory;
  // The second material record names key 1; the first reads back as key 0.
  memory.words[(0x1000u - memory.base + 36u) / 4u] = 1u;
  std::array<uint32_t, 8> image_descriptor{};
  image_descriptor[0] = 0x20u;
  image_descriptor[1] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
      << 20u;
  image_descriptor[2] = 3u | (3u << 14u);
  image_descriptor[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
       << 28u);
  // Key k lands at k * 48 + 16, so the two records are 0x2010 and 0x2040 - neither of which a
  // 32-byte stride at offset 0 would have read.
  for (uint32_t dword = 0; dword < image_descriptor.size(); dword++) {
    memory.words[(0x2010u - memory.base) / 4u + dword] = image_descriptor[dword];
    memory.words[(0x2040u - memory.base) / 4u + dword] = image_descriptor[dword];
  }

  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            Materialized(specialization) == 2 &&
            std::equal(image_descriptor.begin(), image_descriptor.end(),
                       snapshot.images[1].dwords.begin()),
        "strided indirect image table did not materialize its records");
}

// Enumeration decides residency safely; it decides *selection* only if every key the shader can
// produce is in the mapping AND lands on its own descriptor - and if a key that is not in the
// mapping lands on nothing. The second half had no test, and had been wrong: the emitted search
// starts at ordinal 0 and only leaves it on an exact match, so a key the host never enumerated
// sampled whatever ordinal 0 held. This walks a heap end to end - materializes it, then replays
// the binary search spirvEmitterImage emits - for every key the shader could compute, and then
// for one it could not.
void TestIndirectImageSelectionResolvesEveryKey() {
  constexpr uint32_t kSelectorStride  = 112u;
  constexpr uint32_t kHeapStride      = 48u;
  constexpr uint32_t kHeapOffset      = 16u;
  constexpr uint32_t kMaterialRecords = 8u;
  constexpr uint32_t kHeapRecords     = 24u;
  constexpr uint64_t kMaterialBase    = 0x1000u;
  constexpr uint64_t kHeapBase        = 0x4000u;

  auto fixture = MakeIndirectImageFixture(false, 4u, false, 0u, kSelectorStride,
                                          kHeapStride, kHeapOffset);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);

  Check(fixture->program.info.images.size() == 1,
        "the heap shape was not tracked as one image");
  const auto source = fixture->program.info.images[0].source;
  const auto &indirect = fixture->program.descriptor_sources[source].indirect_descriptor;
  Check(indirect.has_value() && indirect->heap_stride == kHeapStride &&
            indirect->table_offset == kHeapOffset &&
            indirect->selector_stride == kSelectorStride,
        "the heap shape did not reach the enumeration intact");

  LinearTestMemory memory;
  memory.words.assign(0x8000u / 4u, 0u);

  // Each heap record's T# names its own index, so a mis-selection shows up as a wrong base.
  for (uint32_t record = 0; record < kHeapRecords; record++) {
    const auto at = (kHeapBase - memory.base + record * kHeapStride + kHeapOffset) / 4u;
    memory.words[at + 0u] = 0x20u + record * 0x40u;
    memory.words[at + 1u] =
        static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[at + 2u] = 3u | (3u << 14u);
    memory.words[at + 3u] =
        Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }

  const std::array<uint32_t, kMaterialRecords> keys{5u, 3u, 9u, 1u, 12u, 7u, 2u, 4u};
  for (uint32_t record = 0; record < kMaterialRecords; record++) {
    const auto base = (kMaterialBase - memory.base + record * kSelectorStride) / 4u;
    // Everything else in the record is ordinary material data. The enumeration cannot tell those
    // words from the key, so they become keys too - which is why the mapping is much larger than
    // the table, and why a key that resolves to nothing must stay resolving to nothing.
    for (uint32_t word = 0; word < kSelectorStride / 4u; word++) {
      memory.words[base + word] = 0x3f800000u + record * 0x11u + word;
    }
    // The key's address is what the shader computes: the record plus the offset it folded in,
    // which already carries the scalar load's own immediate.
    memory.words[base + indirect->selector_offset / 4u] = keys[record];
  }

  std::array<uint32_t, 9> user_data{
      static_cast<uint32_t>(kMaterialBase), kSelectorStride << 16u, kMaterialRecords, 0u,
      static_cast<uint32_t>(kHeapBase),     kHeapStride << 16u,     kHeapRecords,     0u, 7u};
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "the heap did not materialize");

  const auto &root = specialization.images[0];
  Check(root.indirect_search_iterations != 0u, "the table produced no runtime mapping");
  const auto mapping = root.indirect_mapping_offset;
  Check(mapping < snapshot.flattened_srt.size(), "the mapping slot is out of range");
  const auto table = snapshot.flattened_srt[mapping];
  Check(table < snapshot.flattened_srt.size(), "the mapping offset is out of range");
  const auto count = snapshot.flattened_srt[table];

  // Exactly the search spirvEmitterImage emits: low/high bounds, ordinal 0 until an exact match.
  const auto Resolve = [&](uint32_t key) {
    uint32_t low = 0;
    uint32_t high = count;
    uint32_t selected = 0;
    for (uint32_t iteration = 0; iteration < root.indirect_search_iterations; iteration++) {
      const bool active = low < high;
      const auto mid = active ? (low + high) / 2u : 0u;
      const auto entry = table + mid * 2u + 1u;
      const auto mapped_key = snapshot.flattened_srt[entry];
      const auto candidate = snapshot.flattened_srt[entry + 1u];
      if (active && mapped_key == key) selected = candidate;
      if (active && mapped_key < key) low = mid + 1u;
      else if (active) high = mid;
    }
    return selected;
  };

  for (uint32_t record = 0; record < kMaterialRecords; record++) {
    const auto key = keys[record];
    const auto ordinal = Resolve(key);
    Check(ordinal < snapshot.images.size(), "a key resolved past the bound descriptors");
    Check(snapshot.images[ordinal].dwords[0] == 0x20u + key * 0x40u,
          "a key the shader computes did not select its own heap record");
  }

  // And a key the shader cannot compute must not quietly borrow a real descriptor.
  Check(std::ranges::all_of(snapshot.images[Resolve(0x4321u)].dwords,
                            [](uint32_t word) { return word == 0u; }),
        "an unresolved key selected heap record 0 instead of nothing");

  // With two cubes, every key still lands on its record, 2D sorts first, and padding names nothing.
  const auto is_null = [](const DescriptorValue &image) {
    return std::ranges::all_of(image.dwords, [](uint32_t word) { return word == 0u; });
  };
  for (const auto key : {3u, 9u}) {
    const auto at = (kHeapBase - memory.base + key * kHeapStride + kHeapOffset) / 4u;
    memory.words[at + 1u] |= 3u << 30u;
    memory.words[at + 2u] = 3u << 14u;
    memory.words[at + 3u] =
        Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kCube) << 28u);
    memory.words[at + 4u] = 11u;
  }
  ResourceSnapshot mixed;
  ResourceSpecialization mixed_specialization;
  Check(MaterializeResources(resource_plan, runtime, mixed, mixed_specialization),
        "a heap holding cube records did not materialize");
  const auto mixed_root = mixed_specialization.images[0];
  const auto mixed_table = mixed.flattened_srt[mixed_root.indirect_mapping_offset];
  const auto mixed_count = mixed.flattened_srt[mixed_table];
  const auto ResolveMixed = [&](uint32_t key) {
    uint32_t low = 0;
    uint32_t high = mixed_count;
    uint32_t selected = 0;
    for (uint32_t iteration = 0; iteration < mixed_root.indirect_search_iterations; iteration++) {
      const bool active = low < high;
      const auto mid = active ? (low + high) / 2u : 0u;
      const auto entry = mixed_table + mid * 2u + 1u;
      if (active && mixed.flattened_srt[entry] == key) selected = mixed.flattened_srt[entry + 1u];
      if (active && mixed.flattened_srt[entry] < key) low = mid + 1u;
      else if (active) high = mid;
    }
    return selected;
  };
  for (const auto key : keys) {
    const auto ordinal = ResolveMixed(key);
    Check(ordinal < mixed.images.size() && mixed.images[ordinal].dwords[0] == 0x20u + key * 0x40u,
          "reordering the candidates by kind sent a key to another record");
  }
  Check(is_null(mixed.images[ResolveMixed(0x4321u)]),
        "reordering the candidates by kind gave an unresolved key a record");
  const auto cube_at = [&](size_t index) {
    return ((mixed.images[index].dwords[3] >> 28u) & 0xfu) ==
           static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kCube);
  };
  size_t live = 0;
  bool grouped = true;
  for (size_t index = 1; index < mixed.images.size(); index++) {
    if (is_null(mixed.images[index])) continue;
    live++;
    grouped &= index == 1u || cube_at(index) || !cube_at(index - 1u);
  }
  const auto padded = Materialized(mixed_specialization) - 1u;
  Check(grouped && live >= 2u && padded == std::bit_ceil(live) &&
            std::all_of(mixed.images.begin() + 1 + static_cast<ptrdiff_t>(live),
                        mixed.images.end(), is_null),
        "the candidates were not grouped by kind and padded to a null-filled bucket");
}

// A refused table used to report only that it was refused. Each clause now names itself and the
// two numbers that decided it, and - because reaching the answer costs one guest read per material
// record - the answer is remembered so the next dispatch does not buy it again.
void TestIndirectImageRefusalNamesItselfAndIsMemoed() {
  auto fixture = MakeIndirectImageFixture(false);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);

  // The shader scales its selector by 224 and this material V# strides by 128, so the records the
  // enumeration would walk are not the records the draw selects.
  std::array<uint32_t, 9> user_data{0x1100u,    128u << 16u, 2u, 0u, 0x2100u,
                                    16u << 16u, 4u,          0u, 7u};
  LinearTestMemory memory;
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "a material V# whose stride disagrees with the selector was accepted");
  const std::string reason{LastMaterializeFailure()};
  Check(reason.find("strides by 128") != std::string::npos &&
            reason.find("selector by 224") != std::string::npos &&
            reason.find("0x000010f0") != std::string::npos,
        "the refusal did not name its clause, its numbers and its pc");

  const auto reads = memory.reads;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            std::string{LastMaterializeFailure()} == reason,
        "the memoed refusal changed its reason");
  Check(memory.reads == reads,
        "a refusal the descriptors decide was re-derived instead of remembered");

  // Rebinding the material V# so its stride agrees is a different table, so the memo must not
  // answer for it.
  user_data[1] = 224u << 16u;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "the memo outlived the descriptors that produced it");
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

  CheckTrackingRejected(fixture, "buffer resource limit exceeded",
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

    CheckTrackingRejected(
        fixture, "address operation has invalid resource kind",
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

    CheckTrackingRejected(
        fixture, "image operation has invalid resource kind",
        "resource tracking accepted an image opcode with address metadata");
  }
}

// The emulator models "bit N of a 64-bit scalar mask, for this lane" the way the hardware cannot
// spell it, with a lane index. Translator::ThreadBit is the shape.
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

// A descriptor dword a mask selects and a readfirstlane lifts back into a scalar register. The
// walk has to re-execute it: the lane term cancels whenever the mask is wave-wide, which is the
// only way the shader could have built a descriptor out of it in the first place.
void TestLaneMaskDescriptorDword() {
  const auto Build = [](Value low, Value high, bool lift) {
    auto fixture = std::make_unique<Fixture>();
    const auto selected =
        fixture->Emit(ValueOpcode::SelectU32,
                      {ThreadBit(*fixture, low, high), Value(0x400u), Value(0x800u)});
    const auto records =
        lift ? fixture->Emit(ValueOpcode::ReadFirstLane, {selected, Value(true)})
             : selected;
    const auto buffer = fixture->Buffer({fixture->UserData(0), fixture->UserData(1),
                                         records, fixture->UserData(3)},
                                        0x15c);
    MemoryInfo memory;
    memory.kind = ResourceKind::Buffer;
    const auto load =
        fixture->Emit(ValueOpcode::LoadBufferU32,
                      {buffer, Value(0u), Value(0u), Value(0u), Value(true)},
                      fixture->AddMemory(memory, 0x15c));
    fixture->Emit(ValueOpcode::ReferenceU32, {load});
    return fixture;
  };
  std::array<uint32_t, 4> user_data{0x2000u, 0u, 0u, 0u};
  const SrtRuntime runtime{.user_data = user_data};

  auto uniform = Build(Value(0xffffffffu), Value(0xffffffffu), true);
  uniform->PlanAndTrack();
  DescriptorValue descriptor;
  Check(EvaluateDescriptorSource(uniform->program,
                                 uniform->program.info.buffers[0].source, runtime,
                                 descriptor) &&
            descriptor.dwords[2] == 0x400u,
        "a wave-wide lane mask did not re-execute to one descriptor");

  // Lane 0 alone: the dword the wave would have to agree on differs per lane, so no single
  // descriptor stands for it. Refused at materialization, which drops this dispatch only.
  auto split = Build(Value(1u), Value(0u), true);
  split->PlanAndTrack();
  Check(!EvaluateDescriptorSource(split->program,
                                  split->program.info.buffers[0].source, runtime,
                                  descriptor),
        "a per-lane descriptor dword was materialized anyway");

  // Without the readfirstlane nothing bounds the lane: no host descriptor stands for it, so the
  // load either decodes its V# in the shader or the shader stays refused.
  auto loose = Build(Value(0xffffffffu), Value(0xffffffffu), false);
  const auto loose_status = loose->TryPlanAndTrack();
  Check(loose_status.ok
            ? loose->program.info.buffers.empty() &&
                  std::ranges::any_of(loose->program.memory_info,
                                      [](const auto &memory_info) {
                                        return memory_info.kind == ResourceKind::IndirectBuffer;
                                      })
            : loose_status.reason.find("unsupported opcode LaneId") != std::string::npos,
        "a lane index outside a readfirstlane reached a descriptor");
}

// A vector compare written to an SGPR is stored as ballot words: Translator::WriteMask keeps
// CompositeExtractU32x4(Ballot(predicate), 0/1), and ReadMask turns them back into one lane's bit
// with ThreadBit. SILENT HILL 2 compute shader 0x68ca7b6fd9c44cfa was refused on that extract.
void TestBallotDescriptorDword() {
  const auto Build = [](bool lane_predicate, uint32_t wave_size) {
    auto fixture = std::make_unique<Fixture>();
    fixture->program.wave_size = wave_size;
    const auto predicate =
        lane_predicate
            ? fixture->Emit(ValueOpcode::ULessThan32,
                            {fixture->Emit(ValueOpcode::LaneId), Value(3u)})
            : fixture->Emit(ValueOpcode::ULessThan32,
                            {fixture->UserData(0), Value(0x100u)});
    const auto ballot = fixture->Emit(ValueOpcode::Ballot, {predicate});
    const auto low =
        fixture->Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(0u)});
    const auto high =
        fixture->Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(1u)});
    const auto spare =
        fixture->Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(3u)});
    // The lane-indexed predicate differs between lanes, so no readfirstlane of a bit taken from
    // it can stand for the wave; that variant exposes the raw words instead.
    const auto base =
        lane_predicate
            ? fixture->UserData(0)
            : fixture->Emit(ValueOpcode::ReadFirstLane,
                            {fixture->Emit(ValueOpcode::SelectU32,
                                           {ThreadBit(*fixture, low, high),
                                            Value(0x1000u), Value(0x2000u)}),
                             Value(true)});
    const auto records =
        fixture->Emit(ValueOpcode::IAdd32, {fixture->UserData(1), spare});
    const auto buffer = fixture->Buffer({base, records, low, high}, 0x464);
    MemoryInfo memory;
    memory.kind = ResourceKind::Buffer;
    fixture->Emit(ValueOpcode::LoadBufferU32,
                  {buffer, Value(0u), Value(0u), Value(0u), Value(true)},
                  fixture->AddMemory(memory, 0x464));
    fixture->PlanAndTrack();
    return fixture;
  };
  const auto Evaluate = [](const Fixture &fixture, uint32_t word0,
                           DescriptorValue &descriptor) {
    std::array<uint32_t, 2> user_data{word0, 64u};
    const SrtRuntime runtime{.user_data = user_data};
    return EvaluateDescriptorSource(fixture.program,
                                    fixture.program.info.buffers[0].source,
                                    runtime, descriptor);
  };

  DescriptorValue descriptor;
  const auto uniform = Build(false, 64u);
  Check(Evaluate(*uniform, 0x20u, descriptor) && descriptor.dwords[0] == 0x1000u &&
            descriptor.dwords[1] == 64u && descriptor.dwords[2] == 0xffffffffu &&
            descriptor.dwords[3] == 0xffffffffu,
        "a ballot true on every lane did not fill both mask words");
  Check(Evaluate(*uniform, 0x200u, descriptor) && descriptor.dwords[0] == 0x2000u &&
            descriptor.dwords[2] == 0u && descriptor.dwords[3] == 0u,
        "a ballot false on every lane did not clear both mask words");

  const auto narrow = Build(false, 32u);
  Check(Evaluate(*narrow, 0x20u, descriptor) && descriptor.dwords[0] == 0x1000u &&
            descriptor.dwords[2] == 0xffffffffu && descriptor.dwords[3] == 0u,
        "a wave32 ballot set lanes the wave does not have");

  const auto lanes = Build(true, 64u);
  Check(Evaluate(*lanes, 0x1234u, descriptor) && descriptor.dwords[0] == 0x1234u &&
            descriptor.dwords[1] == 64u && descriptor.dwords[2] == 0x7u &&
            descriptor.dwords[3] == 0u,
        "a lane-indexed ballot did not re-execute its predicate per lane");
}

// A DWORDX4 vector load of a V# stored in a buffer, taken apart into the four descriptor dwords.
// The address carries no lane identity, so the host reads the same four words the GPU does.
void TestUniformDwordX4DescriptorLoad() {
  const auto Build = [](uint32_t table_bytes, bool indexed) {
    auto fixture = std::make_unique<Fixture>();
    const auto table = fixture->Buffer(
        {fixture->UserData(0), fixture->UserData(1), Value(table_bytes), Value(0u)}, 4);
    MemoryInfo wide;
    wide.kind = ResourceKind::Buffer;
    wide.offset = 8;
    wide.data_dwords = 4;
    wide.component_count = 4;
    wide.idxen = indexed;
    const auto loaded = fixture->Emit(
        ValueOpcode::LoadBufferU32x4,
        {table, indexed ? fixture->UserData(3) : Value(0u), Value(0u),
         fixture->UserData(2), Value(true)},
        fixture->AddMemory(wide, 4));
    std::array<Value, 4> words;
    for (uint32_t index = 0; index < words.size(); index++) {
      words[index] = fixture->Emit(ValueOpcode::CompositeExtractU32x4,
                                   {loaded, Value(index)});
    }
    const auto target = fixture->Buffer(words, 8);
    MemoryInfo memory;
    memory.kind = ResourceKind::Buffer;
    fixture->Emit(ValueOpcode::LoadBufferU32,
                  {target, Value(0u), Value(0u), Value(0u), Value(true)},
                  fixture->AddMemory(memory, 8));
    return std::pair{std::move(fixture), target};
  };

  auto [fixture, target] = Build(64u, false);
  fixture->PlanAndTrack();
  Check(fixture->program.info.buffers.size() == 2 &&
            fixture->program.srt_reads.empty() &&
            !fixture->program.memory_info[0].planning_only,
        "a DWORDX4 descriptor load was flattened or lost its own binding");
  std::array<uint32_t, 3> user_data{0x1000u, 0u, 4u};
  TestMemory memory;
  memory.words = {0u, 0u, 0u, 0xa0a0a0a0u, 0xa1a1a1a1u, 0xa2a2a2a2u, 0xa3a3a3a3u, 0u};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = ReadTestMemory,
                           .userdata = &memory};
  const auto source =
      fixture->program.info.buffers[target.Instruction()->Flags<uint32_t>()].source;
  DescriptorValue descriptor;
  Check(EvaluateDescriptorSource(fixture->program, source, runtime, descriptor) &&
            descriptor.dwords[0] == 0xa0a0a0a0u && descriptor.dwords[1] == 0xa1a1a1a1u &&
            descriptor.dwords[2] == 0xa2a2a2a2u && descriptor.dwords[3] == 0xa3a3a3a3u,
        "DWORDX4 descriptor components were not read at base + soffset + imm + 4k");

  // Sixteen bytes hold only the first component at offset 12; reading on would leave the table.
  auto [short_table, short_target] = Build(16u, false);
  short_table->PlanAndTrack();
  Check(!EvaluateDescriptorSource(
            short_table->program,
            short_table->program.info
                .buffers[short_target.Instruction()->Flags<uint32_t>()]
                .source,
            runtime, descriptor),
        "a DWORDX4 descriptor load read past its own table");

  // An indexed load names a record per lane, which the host cannot pick.
  auto [indexed, indexed_target] = Build(64u, true);
  CheckTrackingRejected(*indexed, "malformed instruction LoadBufferU32x4",
                        "an indexed DWORDX4 load reached a host descriptor");
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
    Run("waterfall image table", TestWaterfallImageTable);
    Run("guarded direct image table", TestGuardedDirectImageTable);
    Run("bounded compute image loop", TestBoundedComputeImageLoop);
    Run("uniformized material image keys", TestUniformizedMaterialImageKeys);
    Run("image descriptor fields", TestImageDescriptorFields);
    Run("draw-uniform scalar image", TestUniformScalarBufferImage);
    Run("indirect buffer table", TestIndirectBufferMaterialization);
    Run("SRT runtime", TestSrtFlatteningAndRuntimeMemoization);
    Run("dynamic SRT", TestDynamicSrtReadRemainsExplicit);
    Run("uniform buffer read addressing",
        TestUniformBufferReadFollowsBufferAddressing);
    Run("writable descriptor phi", TestWritableDescriptorPhi);
    Run("conditional sampler phi", TestConditionalSamplerPhi);
    Run("finite image phi cycle", TestFiniteImagePhiCycle);
    Run("finite inline sampler", TestFiniteInlineSampler);
    Run("finite image bit scan sentinel", TestFiniteImageBitScanSentinel);
    Run("loop phi with no entry value", TestLoopPhiWithNoEntryValue);
    Run("loop phi with disagreeing entries", TestLoopPhiWithDisagreeingEntries);
    Run("carried descriptor read", TestCarriedDescriptorReadIsNotFlattened);
    Run("aligned material selector", TestAlignedMaterialSelectorIsARecordStep);
    Run("scattered material mask", TestScatteredMaterialMaskIsStillRefused);
    Run("runtime-rooted loop", TestLoopCycleEnteredThroughRuntimeValue);
    Run("invariant loop phi", TestInvariantLoopPhi);
    Run("advancing loop phi", TestAdvancingLoopPhi);
    Run("buffer store active value", TestBufferStoreUsesItsOwnActiveValue);
    Run("bounded relative register writes", TestBoundedRelativeRegisterWrites);
    Run("DMA address materialization", TestDmaAddressMaterialization);
    Run("dynamic FLAT address", TestDynamicFlatAddressesUseDma);
    Run("buffer swizzle specialization", TestBufferSwizzleSpecialization);
    Run("conditional buffer materialization", TestConditionalBufferMaterialization);
    Run("guarded scalar descriptor reads", TestGuardedScalarDescriptorReads);
    Run("conservative buffer reachability", TestConservativeBufferReachability);
    Run("strided indirect image table", TestStridedIndirectImageTable);
    Run("indirect image selection", TestIndirectImageSelectionResolvesEveryKey);
    Run("indirect image refusal reporting", TestIndirectImageRefusalNamesItselfAndIsMemoed);
    Run("conditional indirect image", TestConditionalIndirectImageMaterialization);
    Run("shader info collection", TestShaderInfoCollection);
    Run("image binding ABI", TestImageBindingAbi);
    Run("live shader data collection", TestLiveShaderDataCollection);
    Run("resource limit", TestResourceLimitIsTransactional);
    Run("malformed memory kinds", TestMalformedMemoryKindsRejected);
    Run("lane mask descriptor dword", TestLaneMaskDescriptorDword);
    Run("ballot descriptor dword", TestBallotDescriptorDword);
    Run("uniform DWORDX4 descriptor load", TestUniformDwordX4DescriptorLoad);
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
