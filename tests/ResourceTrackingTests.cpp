#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/BindingLayout.h"
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
#include <utility>
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

bool SameResourceSnapshot(const ResourceSnapshot &lhs,
                          const ResourceSnapshot &rhs) {
  return lhs.buffers == rhs.buffers && lhs.images == rhs.images &&
         lhs.samplers == rhs.samplers &&
         lhs.flattened_srt == rhs.flattened_srt &&
         lhs.user_data == rhs.user_data;
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
    program.block_info.push_back(
        {.id = static_cast<uint32_t>(program.block_info.size())});
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
    BuildSrtPlan(program);
    TrackResources(program);
  }
};

struct TestMemory {
  uint64_t base = 0x1000;
  std::array<uint32_t, 8> words{};
  uint32_t reads = 0;
  uint32_t fail_after = UINT32_MAX;
};

bool ReadTestMemory(void *userdata, uint64_t address, uint32_t *value) {
  auto *memory = static_cast<TestMemory *>(userdata);
  if (memory == nullptr || value == nullptr || address < memory->base ||
      address - memory->base >= memory->words.size() * sizeof(uint32_t) ||
      memory->reads >= memory->fail_after) {
    return false;
  }
  *value = memory->words[(address - memory->base) / sizeof(uint32_t)];
  memory->reads++;
  return true;
}

struct LinearTestMemory {
  uint64_t base = 0x1000;
  std::vector<uint32_t> words = std::vector<uint32_t>(0x2200 / 4);
  uint64_t fail_address = UINT64_MAX;
  uint32_t reads = 0;
};

bool ReadLinearTestMemory(void *userdata, uint64_t address, uint32_t *value) {
  auto *memory = static_cast<LinearTestMemory *>(userdata);
  if (memory == nullptr || value == nullptr || address < memory->base ||
      address - memory->base >= memory->words.size() * sizeof(uint32_t) ||
      (address & 3u) != 0u || address == memory->fail_address) {
    return false;
  }
  *value = memory->words[(address - memory->base) / sizeof(uint32_t)];
  memory->reads++;
  return true;
}

std::unique_ptr<Fixture>
MakeIndirectImageFixture(bool malformed, uint32_t material_immediate = 0,
                         bool memory_backed_material = false) {
  auto fixture = std::make_unique<Fixture>();
  std::array<Value, 4> material_words;
  std::array<Value, 4> heap_words;
  for (uint32_t dword = 0; dword < 4; dword++) {
    material_words[dword] = fixture->UserData(dword);
    heap_words[dword] = fixture->UserData(dword + 4u);
  }
  if (memory_backed_material) {
    const auto pointer_address =
        fixture->Address(fixture->UserData(9), fixture->UserData(10), 0x10b0);
    MemoryInfo pointer_word;
    pointer_word.kind = ResourceKind::ScalarAddress;
    const auto pointer =
        fixture->Emit(ValueOpcode::LoadAddressU32,
                      {pointer_address, Value(0u), Value(0u), Value(true)},
                      fixture->AddMemory(pointer_word, 0x10b0));
    const auto address = fixture->Address(pointer, Value(0u), 0x10c0);
    MemoryInfo descriptor_word;
    descriptor_word.kind = ResourceKind::ScalarAddress;
    material_words[0] =
        fixture->Emit(ValueOpcode::LoadAddressU32,
                      {address, Value(0u), Value(0u), Value(true)},
                      fixture->AddMemory(descriptor_word, 0x10c0));
  }
  const auto material = fixture->Buffer(material_words, 0x10d8);
  const auto heap = fixture->Buffer(heap_words, 0x10d8);
  if (memory_backed_material) {
    MemoryInfo shared_buffer;
    shared_buffer.kind = ResourceKind::Buffer;
    const auto load =
        fixture->Emit(ValueOpcode::LoadBufferU32,
                      {material, Value(0u), Value(0u), Value(0u), Value(true)},
                      fixture->AddMemory(shared_buffer, 0x10d8));
    fixture->Emit(ValueOpcode::ReferenceU32, {load});
  }
  const auto selector = fixture->Emit(ValueOpcode::ReadFirstLane,
                                      {fixture->UserData(8), Value(true)});
  const auto record =
      fixture->Emit(ValueOpcode::IMul32, {selector, Value(224u)});
  const auto member = fixture->Emit(ValueOpcode::IAdd32, {record, Value(4u)});
  fixture->Emit(ValueOpcode::ReferenceU32, {record});
  fixture->Emit(ValueOpcode::ReferenceU32, {member});
  MemoryInfo material_scalar;
  material_scalar.kind = ResourceKind::ScalarBuffer;
  material_scalar.offset = material_immediate;
  const auto key =
      fixture->Emit(ValueOpcode::ReadConstBuffer, {material, member},
                    fixture->AddMemory(material_scalar, 0x10d8));
  const auto heap_offset =
      fixture->Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)});
  std::array<Value, 8> image_words;
  MemoryInfo heap_scalar;
  heap_scalar.kind = ResourceKind::ScalarBuffer;
  for (uint32_t dword = 0; dword < image_words.size(); dword++) {
    auto component = heap_scalar;
    component.offset = dword * sizeof(uint32_t);
    if (malformed && dword == image_words.size() - 1u) {
      component.offset += sizeof(uint32_t);
    }
    image_words[dword] =
        fixture->Emit(ValueOpcode::ReadConstBuffer, {heap, heap_offset},
                      fixture->AddMemory(component, 0x10d8));
  }
  const auto image = fixture->Image(image_words, 0x10f0);
  const auto sampler =
      fixture->Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}, 0x10f0);
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  const auto sampled = fixture->Emit(ValueOpcode::ImageSampleRaw,
                                     {image, sampler, fixture->ImageAddress()},
                                     fixture->AddMemory(sample, 0x10f0));
  const auto sampled_x =
      fixture->Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
  fixture->Emit(ValueOpcode::ReferenceU32, {sampled_x});
  return fixture;
}

void TestInvariantIndirectImageMaterialization() {
  auto fixture = MakeIndirectImageFixture(false);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);

  Check(fixture->program.info.buffers.size() == 1 &&
            fixture->program.info.images.size() == 1 &&
            fixture->program.dynamic_reads.size() == 1,
        "indirect image key was not retained as a scalar-buffer read");
  const auto source = fixture->program.info.images[0].source;
  Check(source < fixture->program.descriptor_sources.size() &&
            fixture->program.descriptor_sources[source]
                .indirect_image.has_value(),
        "indirect image source was not retained for runtime proof");
  const auto image_handle =
      std::ranges::find_if(*fixture->block, [](const Inst &inst) {
        return inst.GetOpcode() == ValueOpcode::GetImageResource;
      });
  Check(image_handle != fixture->block->end() &&
            image_handle->Arg(0).ResolveInstruction() != nullptr &&
            image_handle->Arg(0).ResolveInstruction()->GetOpcode() ==
                ValueOpcode::ReadConstBuffer,
        "indirect image handle discarded the live material key");

  std::array<uint32_t, 9> user_data{0x1000u,    224u << 16u, 2u, 0u, 0x2000u,
                                    16u << 16u, 4u,          0u, 7u};
  LinearTestMemory memory;
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
  for (uint32_t dword = 0; dword < image_descriptor.size(); dword++) {
    memory.words[(0x2000u - memory.base) / 4u + dword] =
        image_descriptor[dword];
    memory.words[(0x2020u - memory.base) / 4u + dword] =
        image_descriptor[dword];
  }
  memory.words[(0x2020u - memory.base) / 4u] ^= 1u;

  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 1 &&
            std::equal(image_descriptor.begin(), image_descriptor.end(),
                       snapshot.images[0].dwords.begin()),
        "invariant indirect image table did not materialize");

  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  memory.fail_address = 0x1004u;
  Check(!MaterializeResources(resource_plan, runtime, snapshot,
                              specialization) &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "rejected planning memory read mutated the snapshot");
  memory.fail_address = UINT64_MAX;

  memory.words[(0x1000u - memory.base + 36u) / 4u] = 1u;
  for (uint32_t dword = 0; dword < image_descriptor.size(); dword++) {
    memory.words[(0x2000u - memory.base) / 4u + dword] = 0u;
    memory.words[(0x2020u - memory.base) / 4u + dword] = 0u;
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
            dynamic_snapshot.images.size() == 2 &&
            dynamic_specialization.images.size() == 2,
        "dynamic indirect image table did not materialize");
  ApplyResourceSpecialization(fixture->program, dynamic_specialization);
  Check(fixture->program.info.images.size() == 2 &&
            fixture->program.info.images[0].indirect_root == 0 &&
            fixture->program.info.images[0].indirect_search_iterations != 0 &&
            fixture->program.info.images[0].indirect_resources.size() == 2 &&
            dynamic_snapshot.images.size() == 2,
        "dynamic indirect image table was not specialized transactionally");
  const auto &mapping = dynamic_specialization.images[0];
  const auto key_count = dynamic_snapshot.flattened_srt[mapping.indirect_mapping_offset];
  Check(mapping.indirect_search_iterations == std::bit_width(key_count) &&
            mapping.indirect_mapping_offset + 1u + key_count * 2u ==
                dynamic_snapshot.flattened_srt.size(),
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

  auto memory_backed = MakeIndirectImageFixture(false, 0u, true);
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
  const auto memory_backed_prior_snapshot = snapshot;
  const auto memory_backed_prior_specialization = specialization;
  Check(!MaterializeResources(memory_backed_plan, memory_backed_runtime,
                              snapshot, specialization) &&
            SameResourceSnapshot(snapshot, memory_backed_prior_snapshot) &&
            specialization == memory_backed_prior_specialization,
        "rejected indirect table descriptor read mutated the snapshot");
  memory.fail_address = UINT64_MAX;

  auto malformed = MakeIndirectImageFixture(true);
  BuildSrtPlan(malformed->program);
  CheckFatal([&] { TrackResources(malformed->program); }, "not a valid runtime value",
             "malformed indirect image pattern was accepted");
  Check(!malformed->program.resource_tracking_complete &&
            malformed->program.info.images.empty() &&
            malformed->program.descriptor_sources.empty(),
        "malformed indirect image pattern was partially accepted");

  auto wrapped_immediate = MakeIndirectImageFixture(false, 4u);
  BuildSrtPlan(wrapped_immediate->program);
  CheckFatal([&] { TrackResources(wrapped_immediate->program); },
             "not a valid runtime value",
             "wrapped scalar immediate entered the invariant image proof");
  Check(!wrapped_immediate->program.resource_tracking_complete,
        "wrapped scalar immediate entered the invariant image proof");
}

std::unique_ptr<Fixture> MakeInlineDescriptorFixture(bool ordinary_samplers = false,
                                                    bool image_table = false,
                                                    bool full_width_images = false) {
  auto fixture = std::make_unique<Fixture>();
  std::array<Value, 4> material_words;
  std::array<Value, 4> index_words;
  for (uint32_t dword = 0; dword < 4; dword++) {
    material_words[dword] = fixture->UserData(dword);
    index_words[dword] = fixture->UserData(dword + 4u);
  }
  const auto material = fixture->Buffer(material_words, 0x244);
  const auto indices = fixture->Buffer(index_words, 0x24c);
  auto *entry = fixture->block;
  auto *loop = fixture->AddBlock();
  entry->AddBranch(loop);
  loop->AddBranch(loop);
  for (auto &info : fixture->program.block_info) {
    info.terminator.kind =
        Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Branch;
    info.terminator.true_block = 1u;
  }
  fixture->block = loop;
  auto &counter = loop->AppendNewInst(ValueOpcode::Phi, {},
                                      static_cast<uint64_t>(Type::U32));
  const auto next =
      fixture->Emit(ValueOpcode::IAdd32, {Value(&counter), Value(1u)});
  counter.AddPhiOperand(entry, Value(0u));
  counter.AddPhiOperand(loop, next);
  const auto index_offset =
      fixture->Emit(ValueOpcode::IMul32, {Value(&counter), Value(4u)});
  MemoryInfo scalar;
  scalar.kind = ResourceKind::ScalarBuffer;
  const auto index =
      fixture->Emit(ValueOpcode::ReadConstBuffer, {indices, index_offset},
                    fixture->AddMemory(scalar, 0x260));
  const auto byte_offset =
      fixture->Emit(ValueOpcode::IMul32,
                    {index, Value(full_width_images ? 440u : 872u)});
  if (full_width_images) {
    const auto sampler = fixture->Sampler(
        {Value(146u), Value(0x00fff000u), Value(0x05000000u), Value(0u)}, 0x1c30);
    for (uint32_t image_index = 0; image_index < 2u; image_index++) {
      std::array<Value, 8> image_words;
      for (uint32_t dword = 0; dword < 8u; dword++) {
        auto component = scalar;
        component.offset = image_index * 32u + dword * 4u;
        component.component_count = 8u;
        component.component_index = dword;
        image_words[dword] = fixture->Emit(
            ValueOpcode::ReadConstBuffer, {material, byte_offset},
            fixture->AddMemory(component, 0x1c10 + image_index * 8u));
      }
      const auto image = fixture->Image(image_words, 0x1c30 + image_index * 8u);
      MemoryInfo sample;
      sample.kind = ResourceKind::Image;
      sample.image_dimension = Decoder::ImageDimension::Dim2D;
      sample.image_r128 = false;
      const auto sampled = fixture->Emit(
          ValueOpcode::ImageSampleRaw, {image, sampler, fixture->ImageAddress()},
          fixture->AddMemory(sample, 0x1c30 + image_index * 8u));
      const auto sampled_x =
          fixture->Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
      fixture->Emit(ValueOpcode::ReferenceU32, {sampled_x});
    }
    return fixture;
  }
  std::array<Value, 4> sampler_words;
  std::array<Value, 8> image_words;
  image_words.fill(Value(0u));
  for (uint32_t dword = 0; dword < 8u; dword++) {
    if ((ordinary_samplers && dword < 4u) || (image_table && dword >= 4u)) {
      continue;
    }
    auto component = scalar;
    component.offset = (ordinary_samplers ? 572u : 136u) + dword * 4u;
    component.component_count = ordinary_samplers || image_table ? 4u : 8u;
    component.component_index = ordinary_samplers ? dword - 4u : dword;
    const auto word = fixture->Emit(
        ValueOpcode::ReadConstBuffer, {material, byte_offset},
        fixture->AddMemory(component, 0x5bc));
    if (dword < 4u) {
      sampler_words[dword] = word;
    } else {
      image_words[dword - 4u] = word;
    }
  }
  if (image_table) {
    auto selector_memory = scalar;
    selector_memory.offset = 564u;
    const auto selector = fixture->Emit(
        ValueOpcode::ReadConstBuffer, {material, byte_offset},
        fixture->AddMemory(selector_memory, 0xbec));
    const auto table_index =
        fixture->Emit(ValueOpcode::BitwiseAnd32, {selector, Value(255u)});
    const auto table_offset =
        fixture->Emit(ValueOpcode::ShiftLeftLogical32, {table_index, Value(5u)});
    const auto address =
        fixture->Address(fixture->UserData(8), fixture->UserData(9), 0xbf8);
    for (uint32_t dword = 0; dword < 8u; dword++) {
      MemoryInfo component;
      component.kind = ResourceKind::ScalarAddress;
      component.offset = 544u + dword * 4u;
      component.component_count = 8u;
      component.component_index = dword;
      image_words[dword] = fixture->Emit(
          ValueOpcode::LoadAddressU32, {address, table_offset, Value(0u), Value(true)},
          fixture->AddMemory(component, 0xbf8));
    }
  }
  const auto image = fixture->Image(image_words, 0x5c8);
  if (ordinary_samplers) {
    sampler_words = {Value(146u), Value(0x00fff000u), Value(0x05000000u), Value(0u)};
  }
  const auto sampler = fixture->Sampler(sampler_words, 0x5c8);
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  sample.image_r128 = !image_table;
  const auto sampled = fixture->Emit(
      ValueOpcode::ImageSampleRaw, {image, sampler, fixture->ImageAddress()},
      fixture->AddMemory(sample, 0x5c8));
  const auto sampled_x =
      fixture->Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
  fixture->Emit(ValueOpcode::ReferenceU32, {sampled_x});
  if (ordinary_samplers) {
    sampler_words[0] = Value(0u);
    const auto repeat = fixture->Sampler(sampler_words, 0x5d0);
    const auto repeated = fixture->Emit(
        ValueOpcode::ImageSampleRaw, {image, repeat, fixture->ImageAddress()},
        fixture->AddMemory(sample, 0x5d0));
    const auto repeated_x =
        fixture->Emit(ValueOpcode::CompositeExtractU32x4, {repeated, Value(0u)});
    fixture->Emit(ValueOpcode::ReferenceU32, {repeated_x});
  }
  return fixture;
}

uint32_t InlineCandidateForKey(const ResourceSnapshot &snapshot,
                                const ResourceSpecialization &specialization,
                                uint32_t key, uint32_t root = 0u) {
  Check(root < specialization.images.size(), "inline image specialization is missing");
  const auto offset = specialization.images[root].indirect_mapping_offset;
  Check(offset < snapshot.flattened_srt.size(), "inline mapping is missing");
  const auto count = snapshot.flattened_srt[offset];
  Check(static_cast<uint64_t>(offset) + 1u + 2ull * count <=
            snapshot.flattened_srt.size(),
        "inline mapping exceeds its SRT allocation");
  for (uint32_t entry = 0; entry < count; entry++) {
    const auto position = offset + 1u + entry * 2u;
    if (entry != 0u) {
      Check(snapshot.flattened_srt[position - 2u] <
                snapshot.flattened_srt[position],
            "inline keys are not unique and sorted");
    }
    if (snapshot.flattened_srt[position] == key) {
      const auto candidate = snapshot.flattened_srt[position + 1u];
      uint32_t ordinal = 0;
      for (uint32_t image = 0; image < specialization.images.size(); image++) {
        if (specialization.images[image].indirect_root == root) {
          if (ordinal++ == candidate) {
            Check(image < snapshot.images.size(),
                  "inline key selects an absent image descriptor");
            return image;
          }
        }
      }
      Check(false, "inline key selects an absent image candidate");
    }
  }
  return root;
}

void TestInlineDescriptorPairs() {
  auto fixture = MakeInlineDescriptorFixture();
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);
  Check(fixture->program.info.images.size() == 1u &&
            fixture->program.info.samplers.size() == 1u,
        "inline descriptor pair was not tracked");
  const auto &image_source = fixture->program.descriptor_sources.at(
      fixture->program.info.images[0].source);
  const auto &sampler_source = fixture->program.descriptor_sources.at(
      fixture->program.info.samplers[0].source);
  Check(image_source.inline_descriptor.has_value() &&
            sampler_source.inline_descriptor.has_value(),
        "inline image or sampler lost its materialization provenance");
  const auto &image_plan = *image_source.inline_descriptor;
  const auto &sampler_plan = *sampler_source.inline_descriptor;
  Check(image_plan.buffer_source == sampler_plan.buffer_source &&
            image_plan.selector_stride == 872u &&
            sampler_plan.selector_stride == 872u &&
            image_plan.descriptor_offset == 152u &&
            sampler_plan.descriptor_offset == 136u,
        "inline descriptor offsets or shared material buffer are incorrect");
  Value live_key;
  for (const auto [opcode, key_arg] :
       {std::pair{ValueOpcode::GetImageResource, image_plan.key_arg},
        std::pair{ValueOpcode::GetSamplerResource, sampler_plan.key_arg}}) {
    const auto handle = std::ranges::find_if(
        *fixture->block, [&](const Inst &inst) { return inst.GetOpcode() == opcode; });
    Check(handle != fixture->block->end() && key_arg < handle->NumArgs(),
          "inline descriptor handle disappeared during DCE");
    const auto key = handle->Arg(key_arg).Resolve();
    const auto *multiply = key.TryInstruction();
    Check(multiply != nullptr && multiply->GetOpcode() == ValueOpcode::IMul32,
          "inline descriptor discarded the live wrapped byte offset");
    const auto *index = multiply->Arg(0).ResolveInstruction();
    Check(index != nullptr && index->GetOpcode() == ValueOpcode::ReadConstBuffer,
          "loop-dependent material index was flattened or removed");
    if (!live_key.IsEmpty()) {
      Check(key == live_key, "image and sampler use different live selection keys");
    }
    live_key = key;
  }

  std::array<uint32_t, 8> user_data{0x1000u, 872u << 16u, 4u, 0u,
                                   0x2000u, 0u, 16u, 0u};
  LinearTestMemory memory;
  DescriptorValue image_a;
  image_a.dword_count = 8u;
  image_a.dwords[0] = 0x20u;
  image_a.dwords[1] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
      << 20u;
  image_a.dwords[2] = 3u | (3u << 14u);
  image_a.dwords[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
       << 28u);
  auto image_b = image_a;
  image_b.dwords[0] = 0x40u;
  DescriptorValue repeat;
  repeat.dword_count = 4u;
  auto clamp = repeat;
  const auto clamp_mode = static_cast<uint32_t>(
      Libs::Graphics::Prospero::SamplerClampMode::kClampLastTexel);
  clamp.dwords[0] = clamp_mode | (clamp_mode << 3u) | (clamp_mode << 6u);
  for (uint32_t record = 0; record < 4u; record++) {
    const auto &image = record < 2u ? image_a : image_b;
    const auto &sampler = record % 2u == 0u ? clamp : repeat;
    for (uint32_t dword = 0; dword < 4u; dword++) {
      memory.words[(record * 872u + 136u) / 4u + dword] = sampler.dwords[dword];
      memory.words[(record * 872u + 152u) / 4u + dword] = image.dwords[dword];
    }
  }
  // This index is outside NumRecords but wraps to the interior byte offset 8.
  constexpr uint32_t wrapped_index = 443287909u;
  static_assert(static_cast<uint32_t>(uint64_t{wrapped_index} * 872u) == 8u);
  memory.words[(0x2000u - memory.base) / 4u] = wrapped_index;
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  // The loop selector belongs to the GPU. Materialization must not evaluate it.
  memory.fail_address = 0x2000u;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "inline image/sampler table did not materialize independently of its loop selector");
  memory.fail_address = UINT64_MAX;
  std::array<uint32_t, 4> candidates;
  for (uint32_t record = 0; record < 4u; record++) {
    const auto candidate = InlineCandidateForKey(snapshot, specialization, record * 872u);
    candidates[record] = candidate;
    const auto sampler = specialization.images[candidate].indirect_sampler;
    Check(candidate != 0u && sampler < snapshot.samplers.size() &&
              snapshot.images[candidate] == (record < 2u ? image_a : image_b) &&
              snapshot.samplers[sampler] == (record % 2u == 0u ? clamp : repeat),
          "inline record selected the wrong image/sampler pair");
    for (uint32_t previous = 0; previous < record; previous++) {
      Check(candidates[previous] != candidate,
            "inline specialization merged distinct image/sampler pairs");
    }
  }
  const auto wrapped = InlineCandidateForKey(snapshot, specialization, 8u);
  Check(wrapped != 0u, "wrapped interior offset was omitted from inline mapping");
  const auto wrapped_sampler = specialization.images[wrapped].indirect_sampler;
  auto overlapping_sampler = repeat;
  overlapping_sampler.dwords[2] = image_a.dwords[0];
  overlapping_sampler.dwords[3] = image_a.dwords[1];
  Check(wrapped_sampler < snapshot.samplers.size() &&
            snapshot.samplers[wrapped_sampler] == overlapping_sampler &&
            std::ranges::all_of(snapshot.images[wrapped].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "wrapped interior descriptor words were read from the wrong byte offsets");
  Check(InlineCandidateForKey(snapshot, specialization, UINT32_MAX - 7u) == 0u &&
            std::ranges::all_of(snapshot.images[0].dwords,
                                [](uint32_t word) { return word == 0u; }) &&
            std::ranges::all_of(snapshot.samplers[0].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "out-of-bounds inline keys did not retain an explicit null pair");

  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  user_data[3] = 1u << 30u;
  const auto reads_before_invalid_type = memory.reads;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            memory.reads == reads_before_invalid_type &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "unsupported inline scalar-buffer type read memory or partially updated resources");
  user_data[3] = 0u;
  memory.fail_address = memory.base + 2u * 872u + 152u;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "failed inline descriptor read partially updated resource state");
  memory.fail_address = UINT64_MAX;
  user_data[2] = 602u;
  const auto prior_reads = memory.reads;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            memory.reads == prior_reads &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "excessive wrapped-offset probes were not rejected before reading memory");
  user_data[2] = 4u;

  for (uint32_t dword = 0; dword < 4u; dword++) {
    memory.words[(3u * 872u + 136u) / 4u + dword] = clamp.dwords[dword];
  }
  ResourceSnapshot collapsed;
  ResourceSpecialization collapsed_specialization;
  Check(MaterializeResources(resource_plan, runtime, collapsed, collapsed_specialization) &&
            InlineCandidateForKey(collapsed, collapsed_specialization, 2u * 872u) ==
                InlineCandidateForKey(collapsed, collapsed_specialization, 3u * 872u) &&
            collapsed.images.size() < snapshot.images.size(),
        "identical inline image/sampler pairs were not deduplicated");

  // A final partial dword must be zero even if backing memory contains that word.
  user_data[1] = 0u;
  user_data[2] = 166u;
  memory.fail_address = memory.base + 164u;
  ResourceSnapshot partial;
  ResourceSpecialization partial_specialization;
  Check(MaterializeResources(resource_plan, runtime, partial, partial_specialization),
        "partial inline descriptor read escaped the scalar buffer bounds");
  const auto partial_candidate = InlineCandidateForKey(partial, partial_specialization, 0u);
  Check(std::ranges::all_of(partial.images[partial_candidate].dwords,
                           [](uint32_t word) { return word == 0u; }),
        "partial compact image descriptor consumed its out-of-bounds type word");

  ApplyResourceSpecialization(fixture->program, prior_specialization);
  Check(fixture->program.info.images.size() == prior_snapshot.images.size(),
        "inline pair specialization did not expand the native image resources");
  for (const auto candidate : candidates) {
    const auto sampler = fixture->program.info.images[candidate].indirect_sampler;
    Check(sampler < fixture->program.info.samplers.size() &&
              std::ranges::any_of(fixture->program.info.sampled_pairs,
                                  [&](const SampledResourcePair &pair) {
                                    return pair.image == candidate && pair.sampler == sampler;
                                  }),
          "specialized inline candidate lost its paired sampler binding");
  }
}

void TestInlineImageUniformSamplers() {
  auto fixture = MakeInlineDescriptorFixture(true);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);
  Check(fixture->program.info.images.size() == 1u &&
            fixture->program.info.samplers.size() == 2u &&
            fixture->program.info.sampled_pairs.size() == 2u,
        "one inline image with two ordinary samplers was not tracked");
  const auto &image_source = fixture->program.descriptor_sources.at(
      fixture->program.info.images[0].source);
  Check(image_source.inline_descriptor.has_value() &&
            image_source.inline_descriptor->descriptor_offset == 588u &&
            image_source.inline_descriptor->selector_stride == 872u,
        "image-only inline source lost its descriptor offset");
  for (const auto &sampler : fixture->program.info.samplers) {
    Check(!fixture->program.descriptor_sources.at(sampler.source).inline_descriptor.has_value(),
          "ordinary sampler was incorrectly marked as an inline descriptor");
  }

  std::array<uint32_t, 8> user_data{0x1000u, 872u << 16u, 2u, 0u,
                                   0x2000u, 0u, 16u, 0u};
  LinearTestMemory memory;
  DescriptorValue image_a;
  image_a.dword_count = 8u;
  image_a.dwords[0] = 0x20u;
  image_a.dwords[1] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
      << 20u;
  image_a.dwords[2] = 3u | (3u << 14u);
  image_a.dwords[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
       << 28u);
  auto image_b = image_a;
  image_b.dwords[0] = 0x40u;
  for (uint32_t dword = 0; dword < 4u; dword++) {
    memory.words[588u / 4u + dword] = image_a.dwords[dword];
    memory.words[(872u + 588u) / 4u + dword] = image_b.dwords[dword];
  }
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "inline image with ordinary samplers did not materialize");
  const auto first = InlineCandidateForKey(snapshot, specialization, 0u);
  const auto second = InlineCandidateForKey(snapshot, specialization, 872u);
  Check(snapshot.images.size() == 3u && first != 0u && second != 0u && first != second &&
            snapshot.images[first] == image_a && snapshot.images[second] == image_b &&
            InlineCandidateForKey(snapshot, specialization, UINT32_MAX - 7u) == 0u &&
            std::ranges::all_of(snapshot.images[0].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "image-only inline mapping lost a texture or its null fallback");
  DescriptorValue clamp;
  clamp.dword_count = 4u;
  clamp.dwords = {146u, 0x00fff000u, 0x05000000u, 0u, 0u, 0u, 0u, 0u};
  auto repeat = clamp;
  repeat.dwords[0] = 0u;
  Check(snapshot.samplers == std::vector<DescriptorValue>{clamp, repeat} &&
            specialization.sampler_origins == std::vector<uint32_t>{0u, 1u} &&
            std::ranges::all_of(specialization.images, [](const auto &image) {
              return image.indirect_sampler == UINT32_MAX;
            }),
        "image-only specialization replaced or cloned ordinary samplers");
  Check(specialization.sampled_pairs.size() == 6u,
        "image-only specialization did not expand both ordinary sampler uses");
  for (uint32_t image = 0; image < snapshot.images.size(); image++) {
    for (uint32_t sampler = 0; sampler < 2u; sampler++) {
      Check(std::ranges::any_of(specialization.sampled_pairs,
                                [&](const SampledResourcePair &pair) {
                                  return pair.image == image && pair.sampler == sampler;
                                }),
            "inline image candidate lost one of its ordinary sampler uses");
    }
  }
  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  const auto prior_reads = memory.reads;
  user_data[3] = 1u << 30u;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            memory.reads == prior_reads &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "image-only inline source accepted an unsupported scalar-buffer type");
  user_data[3] = 0u;
  ApplyResourceSpecialization(fixture->program, specialization);
  Check(fixture->program.info.images.size() == 3u &&
            fixture->program.info.samplers.size() == 2u &&
            fixture->program.info.sampled_pairs == specialization.sampled_pairs &&
            std::ranges::all_of(fixture->program.info.images, [](const auto &image) {
              return image.indirect_sampler == UINT32_MAX;
            }),
        "applied image-only specialization lost ordinary sampler bindings");
}

void TestInlineFullWidthImages() {
  auto fixture = MakeInlineDescriptorFixture(true, false, true);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);
  Check(fixture->program.info.images.size() == 2u &&
            fixture->program.info.samplers.size() == 1u &&
            fixture->program.info.sampled_pairs.size() == 2u,
        "two full-width inline images with an ordinary sampler were not tracked");
  uint32_t material_source = UINT32_MAX;
  for (uint32_t image = 0; image < 2u; image++) {
    const auto source = fixture->program.info.images[image].source;
    const auto &descriptor = resource_plan.descriptor_sources.at(source);
    Check(!fixture->program.info.images[image].r128 &&
              descriptor.inline_descriptor.has_value() &&
              !descriptor.inline_descriptor->image_table.has_value() &&
              descriptor.inline_descriptor->descriptor_dwords == 8u &&
              descriptor.inline_descriptor->descriptor_offset == image * 32u &&
              descriptor.inline_descriptor->selector_stride == 440u,
          "full-width inline descriptor lost its width, offset, or selector stride");
    if (material_source == UINT32_MAX) {
      material_source = descriptor.inline_descriptor->buffer_source;
    }
    Check(descriptor.inline_descriptor->buffer_source == material_source,
          "full-width inline images lost their shared material buffer");
  }
  Check(!resource_plan.descriptor_sources.at(
             fixture->program.info.samplers[0].source).inline_descriptor.has_value(),
        "full-width inline image incorrectly tagged its ordinary sampler");
  Value selection_key;
  uint32_t image_handles = 0;
  for (const auto &instruction : *fixture->block) {
    if (instruction.GetOpcode() != ValueOpcode::GetImageResource) {
      continue;
    }
    const auto &inline_source = *resource_plan.descriptor_sources.at(
        fixture->program.info.images[image_handles++].source).inline_descriptor;
    const auto key = instruction.Arg(inline_source.key_arg).Resolve();
    const auto *multiply = key.TryInstruction();
    Check(multiply != nullptr && multiply->GetOpcode() == ValueOpcode::IMul32 &&
              multiply->Arg(0).ResolveInstruction() != nullptr &&
              multiply->Arg(0).ResolveInstruction()->GetOpcode() == ValueOpcode::ReadConstBuffer,
          "full-width inline image lost its live loop-dependent selector");
    Check(selection_key.IsEmpty() || selection_key == key,
          "full-width inline images use different live selection keys");
    selection_key = key;
  }
  Check(image_handles == 2u, "full-width inline image handle disappeared during DCE");

  std::array<uint32_t, 8> user_data{0x1000u, 440u << 16u, 2u, 0u,
                                   0x2000u, 0u, 16u, 0u};
  LinearTestMemory memory;
  DescriptorValue image_a;
  image_a.dword_count = 8u;
  image_a.dwords[0] = 0x20u;
  image_a.dwords[1] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
      << 20u;
  image_a.dwords[2] = 3u | (3u << 14u);
  image_a.dwords[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
       << 28u);
  image_a.dwords[4] = 1u;
  image_a.dwords[5] = 2u;
  image_a.dwords[6] = 4u;
  image_a.dwords[7] = 0x80u;
  auto image_b = image_a;
  // The compact halves are identical: deduplication must examine all eight words.
  image_b.dwords[4] = 2u;
  image_b.dwords[5] = 4u;
  image_b.dwords[6] = 8u;
  image_b.dwords[7] = 0x100u;
  for (uint32_t record = 0; record < 2u; record++) {
    for (uint32_t image = 0; image < 2u; image++) {
      const auto &descriptor = (record ^ image) == 0u ? image_a : image_b;
      for (uint32_t dword = 0; dword < 8u; dword++) {
        memory.words[(record * 440u + image * 32u) / 4u + dword] =
            descriptor.dwords[dword];
      }
    }
  }
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  memory.fail_address = 0x2000u;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "full-width inline images required CPU evaluation of the live selector");
  memory.fail_address = UINT64_MAX;
  Check(snapshot.images.size() == 6u && snapshot.samplers.size() == 1u &&
            specialization.sampler_origins == std::vector<uint32_t>{0u} &&
            specialization.sampled_pairs.size() == 6u &&
            std::ranges::all_of(specialization.images, [](const auto &image) {
              return image.indirect_sampler == UINT32_MAX;
            }),
        "full-width image candidates were compacted or cloned the ordinary sampler");
  for (uint32_t image = 0; image < 2u; image++) {
    const auto first = InlineCandidateForKey(snapshot, specialization, 0u, image);
    const auto second = InlineCandidateForKey(snapshot, specialization, 440u, image);
    Check(first != second &&
              snapshot.images[first] == (image == 0u ? image_a : image_b) &&
              snapshot.images[second] == (image == 0u ? image_b : image_a) &&
              InlineCandidateForKey(snapshot, specialization, UINT32_MAX - 7u, image) == image &&
              std::ranges::all_of(snapshot.images[image].dwords,
                                  [](uint32_t word) { return word == 0u; }),
          "full-width image mapping lost an upper descriptor word or its null fallback");
  }
  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  for (const uint64_t failed_address : {memory.base + 28u, memory.base + 60u,
                                        memory.base + 440u + 28u,
                                        memory.base + 440u + 60u}) {
    memory.fail_address = failed_address;
    Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
              SameResourceSnapshot(snapshot, prior_snapshot) &&
              specialization == prior_specialization,
          "failed final inline descriptor word was skipped or partially committed");
  }

  // Scalar-buffer bases are rounded down, and a partial final dword reads as
  // zero without invoking the callback for any of its in-bounds bytes.
  user_data[0] = static_cast<uint32_t>(memory.base) + 3u;
  user_data[1] = 0u;
  memory.fail_address = memory.base + 60u;
  auto partial_b = image_b;
  partial_b.dwords[7] = 0u;
  for (const uint32_t size : {61u, 62u, 63u}) {
    user_data[2] = size;
    ResourceSnapshot partial;
    ResourceSpecialization partial_specialization;
    Check(MaterializeResources(resource_plan, runtime, partial, partial_specialization),
          "partial final inline descriptor word escaped scalar-buffer bounds or base alignment");
    const auto first = InlineCandidateForKey(partial, partial_specialization, 0u, 0u);
    const auto second = InlineCandidateForKey(partial, partial_specialization, 0u, 1u);
    Check(partial.images[first] == image_a && partial.images[second] == partial_b,
          "partial full-width descriptor did not preserve its first seven words");
  }
  user_data[2] = 64u;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "newly in-bounds eighth descriptor word did not invoke the failing callback");
  memory.fail_address = UINT64_MAX;
  ResourceSnapshot complete_tail;
  ResourceSpecialization complete_tail_specialization;
  Check(MaterializeResources(resource_plan, runtime, complete_tail,
                             complete_tail_specialization) &&
            complete_tail.images[InlineCandidateForKey(
                complete_tail, complete_tail_specialization, 0u, 1u)] == image_b,
        "full-width descriptor did not recover its eighth word at the exact buffer boundary");
  ApplyResourceSpecialization(fixture->program, prior_specialization);
  Check(fixture->program.info.images.size() == prior_snapshot.images.size() &&
            fixture->program.info.samplers.size() == 1u &&
            fixture->program.info.sampled_pairs == prior_specialization.sampled_pairs &&
            std::ranges::all_of(fixture->program.info.images, [](const ImageResource &image) {
              return !image.r128 && image.indirect_sampler == UINT32_MAX;
            }),
        "applied full-width image specialization changed width or ordinary sampler bindings");
}

void TestInlineImageAddressTable() {
  auto fixture = MakeInlineDescriptorFixture(false, true);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);
  Check(fixture->program.info.images.size() == 1u &&
            fixture->program.info.samplers.size() == 1u &&
            !fixture->program.info.images[0].r128,
        "nested raw image table was not tracked as a full-width sampled image");
  const auto &source = fixture->program.descriptor_sources.at(
      fixture->program.info.images[0].source);
  Check(source.inline_descriptor.has_value() &&
            source.inline_descriptor->image_table.has_value(),
        "nested image table lost its material and address provenance");
  const auto &inline_plan = *source.inline_descriptor;
  const auto &table_plan = *inline_plan.image_table;
  Check(inline_plan.selector_stride == 872u && inline_plan.descriptor_offset == 564u &&
            table_plan.table_offset == 544u && table_plan.index_shift == 0u &&
            table_plan.index_mask == 255u &&
            table_plan.address_source < resource_plan.descriptor_sources.size() &&
            resource_plan.descriptor_sources[table_plan.address_source].dword_count == 2u,
        "nested image-table plan changed the selector or uniform raw address");
  const auto handle = std::ranges::find_if(*fixture->block, [](const Inst &inst) {
    return inst.GetOpcode() == ValueOpcode::GetImageResource;
  });
  Check(handle != fixture->block->end() && inline_plan.key_arg < handle->NumArgs(),
        "nested image-table handle was removed during DCE");
  const auto *live_key = handle->Arg(inline_plan.key_arg).ResolveInstruction();
  Check(live_key != nullptr && live_key->GetOpcode() == ValueOpcode::IMul32 &&
            live_key->Arg(0).ResolveInstruction() != nullptr &&
            live_key->Arg(0).ResolveInstruction()->GetOpcode() == ValueOpcode::ReadConstBuffer,
        "nested image table evaluated or discarded the loop-dependent byte-offset key");

  std::array<uint32_t, 10> user_data{0x1000u, 872u << 16u, 2u, 0u,
                                    0x2000u, 0u, 16u, 0u, 0x2400u, 0u};
  LinearTestMemory memory;
  DescriptorValue image_a;
  image_a.dword_count = 8u;
  image_a.dwords[0] = 0x20u;
  image_a.dwords[1] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
      << 20u;
  image_a.dwords[2] = 3u | (3u << 14u);
  image_a.dwords[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
       << 28u);
  image_a.dwords[4] = 1u;
  image_a.dwords[7] = 0x80u;
  auto image_b = image_a;
  image_b.dwords[0] = 0x40u;
  const uint64_t table_address = 0x2400u + 544u;
  for (uint32_t dword = 0; dword < 8u; dword++) {
    memory.words[(table_address - memory.base) / 4u + dword] = image_a.dwords[dword];
    memory.words[(table_address + 32u - memory.base) / 4u + dword] = image_b.dwords[dword];
  }
  DescriptorValue clamp;
  clamp.dword_count = 4u;
  clamp.dwords[0] = 146u;
  for (uint32_t record = 0; record < 2u; record++) {
    memory.words[(record * 872u + 136u) / 4u] = clamp.dwords[0];
    // The high selector bits must not leak into the table index.
    memory.words[(record * 872u + 564u) / 4u] = record == 0u ? 0x101u : 0x100u;
  }
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  memory.fail_address = 0x2000u;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "nested image table required CPU evaluation of the live loop selector");
  memory.fail_address = UINT64_MAX;
  const auto first = InlineCandidateForKey(snapshot, specialization, 0u);
  const auto second = InlineCandidateForKey(snapshot, specialization, 872u);
  Check(first != 0u && second != 0u && first != second &&
            snapshot.images[first] == image_b && snapshot.images[second] == image_a,
        "nested image table ignored its masked index or raw eight-dword descriptor");
  for (const auto candidate : {first, second}) {
    const auto sampler = specialization.images[candidate].indirect_sampler;
    Check(sampler < snapshot.samplers.size() && snapshot.samplers[sampler] == clamp,
          "nested image table lost the inline sampler paired with its material record");
  }
  const auto fallback_sampler = specialization.images[0].indirect_sampler;
  Check(InlineCandidateForKey(snapshot, specialization, UINT32_MAX - 7u) == 0u &&
            snapshot.images[0] == image_a && fallback_sampler < snapshot.samplers.size() &&
            std::ranges::all_of(snapshot.samplers[fallback_sampler].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "out-of-bounds material selector replaced valid image-table entry zero with null");

  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  for (const uint64_t failed_address : {table_address + 28u, table_address + 32u + 28u}) {
    memory.fail_address = failed_address;
    Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
              SameResourceSnapshot(snapshot, prior_snapshot) &&
              specialization == prior_specialization,
          "failed raw image-table read partially updated resources or skipped descriptor tail");
  }
  memory.fail_address = memory.base + 564u;
  user_data[1] = 0u;
  user_data[2] = 566u;
  ResourceSnapshot partial;
  ResourceSpecialization partial_specialization;
  Check(MaterializeResources(resource_plan, runtime, partial, partial_specialization),
        "partial material selector read escaped scalar-buffer bounds");
  const auto partial_candidate = InlineCandidateForKey(partial, partial_specialization, 0u);
  const auto partial_sampler = partial_specialization.images[partial_candidate].indirect_sampler;
  Check(partial.images[partial_candidate] == image_a && partial_sampler < partial.samplers.size() &&
            partial.samplers[partial_sampler] == clamp,
        "partial material selector did not select table entry zero with its still-valid sampler");

  ApplyResourceSpecialization(fixture->program, prior_specialization);
  Check(fixture->program.info.images.size() == prior_snapshot.images.size() &&
            std::ranges::all_of(fixture->program.info.images,
                                [](const ImageResource &image) { return !image.r128; }),
        "nested table specialization changed the native descriptor width");
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

  CheckFatal([&] { TrackResources(fixture.program); }, "already tracked",
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

  std::array<uint32_t, 1> user_data{0xffffffffu};
  SrtRuntime runtime{.user_data = user_data};
  DescriptorValue value;
  const auto source = fixture.program.info.buffers[0].source;
  Check(EvaluateDescriptorSource(fixture.program, source, runtime, value) &&
            value.dwords[3] == 0x100u,
        "runtime descriptor unsigned minimum did not clamp its first operand");
  user_data[0] = 0x80u;
  Check(
      EvaluateDescriptorSource(fixture.program, source, runtime, value) &&
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
  const auto active = fixture.Emit(ValueOpcode::WqmMask, {Value(true)});
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
  fixture.PlanAndTrack();

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
  Check(EvaluateDescriptorSource(fixture.program, source, runtime, descriptor) &&
            descriptor.dwords[3] == 0x80000abcu,
        "SampleAdjust canonicalization lost sampler border fields");

  const auto CheckRejected = [](uint32_t flags, uint32_t shift,
                                const char *message) {
    Fixture rejected(ShaderType::Pixel);
    const auto condition = rejected.Emit(ValueOpcode::WqmMask, {Value(true)});
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
    BuildSrtPlan(rejected.program);
    CheckFatal([&] { TrackResources(rejected.program); },
               "not a valid runtime value", message);
  };
  CheckRejected(0u, 12u,
                "ordinary sampling accepted SampleAdjust reserved scratch");
  CheckRejected(Decoder::ImageSampleFlagAdjust, 30u,
                "SampleAdjust canonicalization discarded border-mode bits");
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
            images[1].mip_mode == ImageMipMode::DynamicStorage &&
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
  AllocateBindings(fixture.program);
  const auto storage_kind = DescriptorBindingForImage(images[0]);
  Check(storage_kind.has_value(), "storage image has no descriptor binding");
  const auto *storage_binding =
      FindBinding(fixture.program.bindings, *storage_kind);
  Check(storage_binding != nullptr &&
            storage_binding->resources == std::vector<uint32_t>({0, 1, 1, 1}),
        "dynamic storage mip descriptors were not expanded consecutively");

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
  const auto valid_snapshot = changed_snapshot;
  const auto valid_specialization = changed_specialization;
  Check(!MaterializeResources(resource_plan, {.user_data = changed_user_data},
                              changed_snapshot, changed_specialization) &&
            SameResourceSnapshot(changed_snapshot, valid_snapshot) &&
            changed_specialization == valid_specialization,
        "an inverted dynamic storage mip range was accepted or mutated output");
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
  std::vector<DescriptorValue> descriptors;
  std::vector<uint32_t> flat;
  const uint32_t request = fixture.program.info.buffers[0].source;
  Check(EvaluateRuntimeSources(fixture.program, std::span{&request, 1}, runtime,
                               descriptors, flat, {}),
        "typed runtime source evaluation failed");
  Check(descriptors.size() == 1 && descriptors[0].dwords[0] == 0xdeadbeefu &&
            flat == std::vector<uint32_t>{0xdeadbeefu} && memory.reads == 1,
        "descriptor and flat SRT evaluation did not share one memoized read");

  memory.reads = 0;
  memory.fail_after = 0;
  descriptors = {{{1u}, 1u}};
  flat = {2u};
  Check(!EvaluateRuntimeSources(fixture.program, std::span{&request, 1},
                                runtime, descriptors, flat, {}) &&
            descriptors == std::vector<DescriptorValue>{{{1u}, 1u}} &&
            flat == std::vector<uint32_t>{2u},
        "runtime evaluation failure was not transactional");

  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  AllocateBindings(fixture.program);
  Check(FindBinding(fixture.program.bindings,
                    DescriptorBindingKind::FlattenedSrt) != nullptr,
        "flattened typed SRT reads did not receive a binding");
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

  Check(fixture.program.srt_reads.empty() &&
            fixture.program.dynamic_reads.size() == 1 &&
            fixture.program.info.uses_dma,
        "dynamic scalar read was incorrectly flattened or lost");
  std::array<uint32_t, 3> user_data{0x1000u, 0u, 4u};
  TestMemory memory;
  memory.words[1] = 0xabcdef01u;
  SrtRuntime runtime{.user_data = user_data,
                     .read_memory = ReadTestMemory,
                     .userdata = &memory};
  DescriptorValue value;
  Check(EvaluateDescriptorSource(fixture.program,
                                 fixture.program.info.buffers[0].source, runtime, value) &&
            value.dwords[0] == 0xabcdef01u && memory.reads == 1,
        "dynamic typed scalar descriptor source was not evaluated");

  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  AllocateBindings(fixture.program);
  Check(FindBinding(fixture.program.bindings,
                    DescriptorBindingKind::FlattenedSrt) == nullptr &&
            FindBinding(fixture.program.bindings,
                        DescriptorBindingKind::BdaPagetable) != nullptr &&
            FindBinding(fixture.program.bindings,
                        DescriptorBindingKind::FaultBuffer) != nullptr,
        "dynamic scalar read received the wrong resource bindings");
  Check(fixture.program.bindings.memory_offset_dword ==
                fixture.program.bindings.user_data_registers.size() &&
            fixture.program.bindings.memory_offset_count == 1u &&
            fixture.program.bindings.ShaderDataDwords() ==
                fixture.program.bindings.memory_offset_dword + 1u,
        "unified memory-offset layout is inconsistent");
}

void TestPhiValidation() {
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
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(memory, 20), merge);

  BuildSrtPlan(fixture.program);
  CheckFatal([&] { TrackResources(fixture.program); }, "not a valid runtime value",
             "control-dependent descriptor phi was accepted");
  Check(!fixture.program.resource_tracking_complete &&
            fixture.program.info.buffers.empty() &&
            fixture.program.descriptor_sources.empty(),
        "control-dependent descriptor phi was not rejected transactionally");
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
  fixture.Emit(ValueOpcode::GetBufferResource,
               {carried, Value(0u), Value(0u), Value(0u)}, MemoryFlags{0, 12},
               loop);

  BuildSrtPlan(fixture.program);
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

  std::array<uint32_t, 1> user_data{0x12345678u};
  SrtRuntime runtime{.user_data = user_data};
  DescriptorValue descriptor;
  Check(EvaluateDescriptorSource(fixture.program,
                                 fixture.program.info.buffers[0].source, runtime, descriptor) &&
            descriptor.dwords[0] == user_data[0],
        "loop-invariant descriptor phi was not evaluated through typed SSA");
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

void TestShaderInfoAndBindingLayout() {
  Fixture fixture;
  const auto handle = fixture.Buffer(
      {fixture.UserData(3), fixture.UserData(4), Value(64u), Value(0u)}, 4);
  MemoryInfo buffer;
  buffer.kind = ResourceKind::Buffer;
  fixture.Emit(ValueOpcode::LoadBufferU32,
               {handle, Value(0u), Value(0u), Value(0u), Value(true)},
               fixture.AddMemory(buffer, 4));
  fixture.Emit(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)),
       Value(2u)});
  fixture.Emit(ValueOpcode::BitwiseXor32, {Value(1u), Value(2u)});
  MemoryInfo gds;
  gds.kind = ResourceKind::Gds;
  fixture.Emit(ValueOpcode::WriteSharedU32, {Value(0u), Value(1u), Value(true)},
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

  AllocateBindings(fixture.program);
  Check(FindBinding(fixture.program.bindings, DescriptorBindingKind::Buffers) !=
                nullptr &&
            FindBinding(fixture.program.bindings, DescriptorBindingKind::Gds) !=
                nullptr &&
            FindBinding(fixture.program.bindings,
                        DescriptorBindingKind::ShaderData) == nullptr &&
	        fixture.program.bindings.UsesPushData(),
        "typed resources were not assigned native bindings");
  Check(NativeBinding(ShaderType::Compute, DescriptorBindingKind::Buffers) ==
                static_cast<uint32_t>(DescriptorBindingKind::Buffers) &&
            NativeBinding(ShaderType::Vertex, DescriptorBindingKind::Buffers) ==
                static_cast<uint32_t>(DescriptorBindingKind::Buffers) &&
            NativeBinding(ShaderType::Pixel, DescriptorBindingKind::Buffers) ==
                static_cast<uint32_t>(DescriptorBindingKind::Count) +
                    static_cast<uint32_t>(DescriptorBindingKind::Buffers),
        "fixed stage binding ranges are inconsistent");
  Check(fixture.program.bindings.user_data_registers ==
            std::vector<uint32_t>({3u, 4u}),
        "binding layout did not collect live typed user-data values");
}

void TestImageBindingAbi() {
  using NumericClass = Libs::Graphics::Prospero::TextureNumericClass;

  Check(ImageBindingCount == 36u &&
            static_cast<uint32_t>(DescriptorBindingKind::Buffers) == 0u &&
            static_cast<uint32_t>(DescriptorBindingKind::Samplers) == 37u &&
            static_cast<uint32_t>(DescriptorBindingKind::Gds) == 38u &&
            static_cast<uint32_t>(DescriptorBindingKind::BdaPagetable) == 39u &&
            static_cast<uint32_t>(DescriptorBindingKind::FaultBuffer) == 40u &&
            static_cast<uint32_t>(DescriptorBindingKind::FlattenedSrt) == 41u &&
            static_cast<uint32_t>(DescriptorBindingKind::ShaderData) == 42u &&
            static_cast<uint32_t>(DescriptorBindingKind::Count) == 43u,
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
          Decoder::ImageDimension dimension, bool atomic) {
        ImageResource image;
        image.resource_class = resource_class;
        image.numeric_class = numeric_class;
        image.dimension = dimension;
        image.atomic = atomic;
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

void TestGraphicsPushConstantLayout() {
  const auto AddUserData = [](Fixture &fixture, uint32_t count) {
    for (uint32_t index = 0; index < count; index++) {
      fixture.Emit(ValueOpcode::ReferenceU32, {fixture.UserData(index)});
    }
    fixture.program.shader_info_complete = true;
  };
  uint32_t cursor = 0;
  Fixture pixel(ShaderType::Pixel);
  AddUserData(pixel, 4);
  AllocateBindings(pixel.program, cursor);
  Check(
      pixel.program.bindings.UsesPushData() &&
          pixel.program.bindings.push_data_start_dword == 0 &&
          FindBinding(pixel.program.bindings,
                      DescriptorBindingKind::ShaderData) == nullptr,
      "pixel shader did not start the shared push-data block");
  pixel.program.bindings.AdvancePushData(cursor);

  Fixture vertex(ShaderType::Vertex);
  AddUserData(vertex, 9);
  AllocateBindings(vertex.program, cursor);
  Check(vertex.program.bindings.UsesPushData() &&
            vertex.program.bindings.push_data_start_dword == 4,
        "vertex shader did not follow pixel data in the shared push-data block");
  vertex.program.bindings.AdvancePushData(cursor);
  Check(cursor == 13, "graphics push-data cursor advanced incorrectly");

  Fixture edge(ShaderType::Pixel);
  AddUserData(edge, NativePushConstantSize / sizeof(uint32_t));
  AllocateBindings(edge.program);
  Check(edge.program.bindings.UsesPushData() &&
            FindBinding(edge.program.bindings,
                        DescriptorBindingKind::ShaderData) == nullptr,
        "the full shared push-data block did not fit");

  Fixture spill(ShaderType::Pixel);
  AddUserData(spill, 20);
  AllocateBindings(spill.program, cursor);
  Check(
      !spill.program.bindings.UsesPushData() &&
          spill.program.bindings.push_data_start_dword == PushData::NoStart &&
          FindBinding(spill.program.bindings,
                      DescriptorBindingKind::ShaderData) != nullptr,
      "a stage that exceeded the remaining shared push data did not spill to storage");
  const auto spill_layout = spill.program.bindings;
  spill.program.bindings.AdvancePushData(cursor);
  Check(cursor == 13, "a spilled stage consumed shared push-data space");

  Fixture repeated_spill(ShaderType::Pixel);
  AddUserData(repeated_spill, 20);
  AllocateBindings(repeated_spill.program, 20);
  Check(repeated_spill.program.bindings == spill_layout,
        "storage fallback retained an irrelevant attempted push-data position");
}

void TestResourceLimitIsTransactional() {
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
  BuildSrtPlan(fixture.program);
  CheckFatal([&] { TrackResources(fixture.program); },
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
    BuildSrtPlan(fixture.program);
    CheckFatal(
        [&] { TrackResources(fixture.program); },
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
    BuildSrtPlan(fixture.program);
    CheckFatal(
        [&] { TrackResources(fixture.program); },
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
    Run("scalar/vector alias", TestScalarAndVectorBufferAlias);
    Run("runtime unsigned min", TestRuntimeUnsignedMinDescriptor);
    Run("images and samplers", TestImagesSamplersAndAliases);
    Run("SampleAdjust sampler scratch", TestSampleAdjustSamplerScratch);
    Run("dynamic storage mips", TestDynamicStorageMipTracking);
    Run("invariant indirect images", TestInvariantIndirectImageMaterialization);
    Run("inline descriptor pairs", TestInlineDescriptorPairs);
    Run("inline image uniform samplers", TestInlineImageUniformSamplers);
    Run("inline full-width images", TestInlineFullWidthImages);
    Run("inline image address table", TestInlineImageAddressTable);
    Run("SRT runtime", TestSrtFlatteningAndRuntimeMemoization);
    Run("dynamic SRT", TestDynamicSrtReadRemainsExplicit);
    Run("phi validation", TestPhiValidation);
    Run("runtime-rooted loop", TestLoopCycleEnteredThroughRuntimeValue);
    Run("invariant loop phi", TestInvariantLoopPhi);
    Run("DMA address materialization", TestDmaAddressMaterialization);
    Run("dynamic FLAT address", TestDynamicFlatAddressesUseDma);
    Run("buffer swizzle specialization", TestBufferSwizzleSpecialization);
    Run("shader info and bindings", TestShaderInfoAndBindingLayout);
    Run("image binding ABI", TestImageBindingAbi);
    Run("graphics push constants", TestGraphicsPushConstantLayout);
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
