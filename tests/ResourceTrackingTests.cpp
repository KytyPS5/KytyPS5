#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
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

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderComputeInputInfo;
using Libs::Graphics::ShaderType;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;
namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;

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
         lhs.user_data == rhs.user_data &&
         lhs.uniform_fill == rhs.uniform_fill;
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

bool RejectTestMemory(void *, uint64_t, std::span<uint32_t>) { return false; }

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
MakeIndirectImageFixture(bool malformed, uint32_t material_immediate = 0,
                         bool memory_backed_material = false,
                         bool storage_write = false,
                         ValueOpcode sampled_opcode = ValueOpcode::ImageSampleRaw) {
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
  const auto invocation = fixture->Emit(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)), Value(0u)});
  const auto selector = fixture->Emit(ValueOpcode::ReadFirstLane,
                                      {invocation, Value(true)});
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
  MemoryInfo access;
  access.kind = ResourceKind::Image;
  access.image_dimension = Decoder::ImageDimension::Dim2D;
  if (storage_write) {
    const auto data = fixture->Emit(ValueOpcode::CompositeConstructU32x4,
                                    {Value(1u), Value(2u), Value(3u), Value(4u)});
    fixture->Emit(ValueOpcode::ImageWrite,
                  {image, fixture->ImageAddress(), data, Value(true)},
                  fixture->AddMemory(access, 0x10f0));
  } else {
    const auto sampler =
        fixture->Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}, 0x10f0);
    const auto sampled = fixture->Emit(sampled_opcode,
                                       {image, sampler, fixture->ImageAddress()},
                                       fixture->AddMemory(access, 0x10f0));
    const auto sampled_x =
        fixture->Emit(ValueOpcode::CompositeExtractU32x4, {sampled, Value(0u)});
    fixture->Emit(ValueOpcode::ReferenceU32, {sampled_x});
  }
  return fixture;
}

std::unique_ptr<Fixture> MakeAddressBackedImageFixture(uint32_t selector_depth = 0u,
                                                      bool shared_selector = true) {
  auto fixture = std::make_unique<Fixture>();
  const auto root = fixture->Address(fixture->UserData(7), fixture->UserData(8), 0x11f0);
  const auto load_root_word = [&](uint32_t offset, uint32_t pc) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.offset = offset;
    return fixture->Emit(ValueOpcode::LoadAddressU32,
                         {root, Value(0u), Value(0u), Value(true)},
                         fixture->AddMemory(memory, pc));
  };
  std::array<Value, 4> pointer_words;
  for (uint32_t dword = 0; dword < pointer_words.size(); dword++) {
    pointer_words[dword] = load_root_word(dword * sizeof(uint32_t), 0x11f4);
  }
  const auto heap_base = fixture->Address(pointer_words[0], pointer_words[1], 0x11f8);
  std::array<Value, 4> material_words;
  for (uint32_t dword = 0; dword < material_words.size(); dword++) {
    material_words[dword] = load_root_word(680u + dword * sizeof(uint32_t), 0x11fc);
  }
  const auto material = fixture->Buffer(material_words, 0x1200);
  auto *entry = fixture->block;
  auto *loop = fixture->AddBlock();
  entry->AddBranch(loop);
  loop->AddBranch(loop);
  auto &selector_phi = loop->AppendNewInst(ValueOpcode::Phi, {},
                                            static_cast<uint64_t>(Type::U32));
  const auto carried = fixture->Emit(
      ValueOpcode::IAdd32, {Value(&selector_phi), Value(1u)}, 0, loop);
  selector_phi.AddPhiOperand(entry, Value(0u));
  selector_phi.AddPhiOperand(loop, carried);
  Value selector(&selector_phi);
  for (uint32_t depth = 0; depth < selector_depth; depth++) {
    selector = fixture->Emit(ValueOpcode::IAdd32,
                             {selector, shared_selector ? selector : Value(1u)}, 0, loop);
  }
  const auto record = fixture->Emit(
      ValueOpcode::IMul32, {selector, Value(384u)}, 0, loop);
  MemoryInfo material_read;
  material_read.kind = ResourceKind::ScalarBuffer;
  material_read.offset = 368u;
  const auto key = fixture->Emit(
      ValueOpcode::ReadConstBuffer, {material, record},
      fixture->AddMemory(material_read, 0x1204), loop);
  fixture->Emit(ValueOpcode::ULessThan32, {key, Value(8u)}, 0, loop);
  const auto key_shift = fixture->Emit(
      ValueOpcode::ShiftLeftLogical32, {key, Value(5u)}, 0, loop);
  const auto key_offset = fixture->Emit(
      ValueOpcode::IAdd32, {key_shift, Value(152u)}, 0, loop);
  std::array<Value, 8> image_words;
  for (uint32_t dword = 0; dword < image_words.size(); dword++) {
    MemoryInfo heap_read;
    heap_read.kind = ResourceKind::ScalarAddress;
    heap_read.offset = dword * sizeof(uint32_t);
    image_words[dword] = fixture->Emit(
      ValueOpcode::LoadAddressU32,
      {heap_base, key_offset, Value(0u), Value(true)},
      fixture->AddMemory(heap_read, 0x1214), loop);
  }
  const auto image = fixture->Emit(
      ValueOpcode::GetImageResource,
      {image_words[0], image_words[1], image_words[2], image_words[3], image_words[4],
       image_words[5], image_words[6], image_words[7]},
      MemoryFlags{0, 0x1220}, loop);
  const auto sampler =
      fixture->Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}, 0x1220);
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  const auto sampled = fixture->Emit(
      ValueOpcode::ImageSampleRaw,
      {image, sampler, fixture->ImageAddress()}, fixture->AddMemory(sample, 0x1220), loop);
  fixture->Emit(ValueOpcode::ReferenceU32,
                {fixture->Emit(ValueOpcode::CompositeExtractU32x4,
                               {sampled, Value(0u)})});
  return fixture;
}

void TestAddressBackedIndirectImages() {
  auto address_backed = MakeAddressBackedImageFixture();
  address_backed->PlanAndTrack();
  Check(address_backed->program.info.images.size() == 1u,
        "address-backed image resource was not tracked");
  const auto& address_indirect = address_backed->program.descriptor_sources[
      address_backed->program.info.images[0].source].indirect_image;
  Check(address_indirect.has_value(),
        "loop-indexed address-backed descriptor table was not recognized");
  Check(address_backed->program.descriptor_sources[address_indirect->table_source].dword_count ==
            2u,
        "address-backed descriptor table pointer was not retained");
  Check(address_indirect->selector_stride == 384u &&
            address_indirect->selector_offset == 368u &&
            address_indirect->table_offset == 152u,
        "image table offsets were not combined with scalar memory metadata");
  auto address_plan = ExtractResourcePlan(address_backed->program);
  LinearTestMemory address_memory;
  const auto store_address_word = [&](uint64_t address, uint32_t word) {
    address_memory.words[(address - address_memory.base) / sizeof(uint32_t)] = word;
  };
  store_address_word(0x1000u, 0x2000u);
  store_address_word(0x1000u + 680u, 0x3000u);
  store_address_word(0x1000u + 684u, 384u << 16u);
  store_address_word(0x1000u + 688u, 1u);
  // A 32-bit loop index can wrap the 384-byte stride into another valid offset.
  store_address_word(0x3000u + 112u, 6u);
  store_address_word(0x3000u + 368u, 5u);
  std::array<uint32_t, 8> address_image_descriptor{};
  address_image_descriptor[0] = 0x20u;
  address_image_descriptor[1] = static_cast<uint32_t>(
      Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
  address_image_descriptor[2] = 3u | (3u << 14u);
  address_image_descriptor[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  for (uint32_t dword = 0; dword < address_image_descriptor.size(); dword++) {
    store_address_word(0x2000u + 5u * 32u + 152u + dword * 4u,
                       address_image_descriptor[dword]);
    store_address_word(0x2000u + 6u * 32u + 152u + dword * 4u,
                       address_image_descriptor[dword]);
  }
  std::array<uint32_t, 9> address_user_data{};
  address_user_data[7] = 0x1000u;
  ResourceSnapshot address_snapshot;
  ResourceSpecialization address_specialization;
  DescriptorValue expected_address_descriptor;
  expected_address_descriptor.dword_count = 8u;
  expected_address_descriptor.dwords = address_image_descriptor;
  const SrtRuntime address_runtime{.user_data = address_user_data,
                                   .read_memory = ReadLinearTestMemory,
                                   .userdata = &address_memory,
                                   .read_specialization_memory = ReadLinearTestMemory};
  Check(MaterializeResources(address_plan, address_runtime, address_snapshot,
                             address_specialization) &&
            address_specialization.images.size() == 2u &&
            address_snapshot.images.size() == 2u &&
            address_specialization.images[0].indirect_root == 0u &&
            address_snapshot.images[1] == expected_address_descriptor &&
            address_snapshot.flattened_srt[
                address_specialization.images[0].indirect_mapping_offset] == 3u &&
            address_snapshot.flattened_srt[
                address_specialization.images[0].indirect_mapping_offset + 5u] == 6u,
        "address-backed indirect image descriptors were not materialized");
}

void TestInvariantIndirectImageMaterialization() {
  TestAddressBackedIndirectImages();
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

  user_data[5] = 0u;
  user_data[6] = 19u;
  memory.fail_address = 0x2010u;
  memory.watched_address = 0x2000u;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            memory.watched_dwords == 4u &&
            std::equal(image_descriptor.begin(), image_descriptor.end(),
                       snapshot.images[0].dwords.begin()),
        "partial scalar-buffer descriptor read crossed bounds instead of zeroing its tail");
  memory.fail_address = 0x2008u;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization),
        "unreadable memory inside the descriptor prefix was accepted");
  user_data[5] = 16u << 16u;
  user_data[6] = 4u;
  memory.watched_address = UINT64_MAX;

  memory.fail_address = 0x1004u;
  Check(!MaterializeResources(resource_plan, runtime, snapshot,
                              specialization),
        "unreadable material-table selector was accepted");
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
            mixed_snapshot.images.size() == 2 &&
            mixed_specialization.images.size() == 2 &&
            mixed_specialization.images[0].dimension ==
                Decoder::ImageDimension::Dim2D &&
            !mixed_specialization.images[0].cube &&
            mixed_specialization.images[1].dimension ==
                Decoder::ImageDimension::Dim2DArray &&
            mixed_specialization.images[1].cube &&
            std::equal(mixed_snapshot.images[1].dwords.begin(),
                       mixed_snapshot.images[1].dwords.end(),
                       memory.words.begin() + second_image),
        "mixed 2D and cube candidates were rejected or discarded");
  memory.words[second_image + 1u] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32UInt)
          << 20u |
      (3u << 30u);
  // Numeric-class rejection applies when the operation cannot preserve raw bits.
  // Raw sampled-image tables are covered separately by heterogeneous admission tests.
  resource_plan.info.images[0].heterogeneous_numeric_compatible = false;
  Check(!MaterializeResources(resource_plan, runtime, mixed_snapshot,
                              mixed_specialization),
        "indirect images with different numeric classes were accepted");
  for (const auto dword : {1u, 2u, 3u, 4u}) {
    memory.words[second_image + dword] = image_descriptor[dword];
  }
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
  const auto old_address = snapshot.images[0].dwords[0];
  memory.words[(0x2000u - memory.base) / 4u] += 0x100u;
  memory.watched_reads = 0;
  Check(MaterializeResources(memory_backed_plan, memory_backed_runtime,
                             snapshot, specialization) && memory.watched_reads == 1u &&
            snapshot.images[0].dwords[0] == old_address + 0x100u,
        "cache refresh reused stale table contents or repeated its clean pointer read");
  Check(snapshot.buffers.data() == buffer_storage && snapshot.images.data() == image_storage &&
            snapshot.flattened_srt.data() == flat_storage &&
            snapshot.user_data.data() == user_data_storage &&
            specialization.buffers.data() == buffer_specialization_storage &&
            specialization.images.data() == image_specialization_storage,
        "a same-shape refresh discarded the runtime output storage");

  auto malformed = MakeIndirectImageFixture(true);
  BuildSrtPlan(malformed->program);
  CheckFatal([&] { TrackResources(malformed->program); }, "not a valid runtime value",
             "malformed indirect image pattern was accepted");
  Check(!malformed->program.resource_tracking_complete &&
            malformed->program.info.images.empty() &&
            malformed->program.descriptor_sources.empty(),
        "malformed indirect image pattern was partially accepted");

  auto wrapped_immediate = MakeIndirectImageFixture(false, UINT32_MAX);
  BuildSrtPlan(wrapped_immediate->program);
  CheckFatal([&] { TrackResources(wrapped_immediate->program); },
             "not a valid runtime value",
             "wrapped scalar immediate entered the invariant image proof");
  Check(!wrapped_immediate->program.resource_tracking_complete,
        "wrapped scalar immediate entered the invariant image proof");
}

void TestSharedUniformLoopIndex() {
  auto shared = MakeAddressBackedImageFixture(24u);
  shared->PlanAndTrack();
  Check(shared->program.info.images.size() == 1u,
        "shared uniform loop selector was not recognized");

  auto deep = MakeAddressBackedImageFixture(40u, false);
  BuildSrtPlan(deep->program);
  CheckFatal([&] { TrackResources(deep->program); }, "not a valid runtime value",
             "deep uniform loop selector exceeded the traversal limit");
}

std::unique_ptr<Fixture> MakeBufferRecordImageFixture(bool formatted) {
  auto fixture = std::make_unique<Fixture>();
  const auto material = fixture->Buffer({fixture->UserData(0), fixture->UserData(1),
                                         fixture->UserData(2), fixture->UserData(3)});
  MemoryInfo record;
  record.kind = ResourceKind::Buffer;
  record.data_bits = 32u;
  record.data_dwords = 3u;
  record.formatted = formatted;
  const auto loaded = fixture->Emit(
      ValueOpcode::LoadBufferU32x3,
      {material, fixture->UserData(6), Value(0u), Value(0u), Value(true)},
      fixture->AddMemory(record, 0x60));
  const auto component = fixture->Emit(ValueOpcode::CompositeExtractU32x3,
                                       {loaded, Value(2u)});
  const auto key = fixture->Emit(ValueOpcode::ReadLane,
                                 {component, Value(0u)});
  const auto table = fixture->Address(fixture->UserData(4), fixture->UserData(5));
  const auto offset = fixture->Emit(ValueOpcode::ShiftLeftLogical32,
                                   {key, Value(5u)});
  std::array<Value, 8> words;
  for (uint32_t dword = 0; dword < words.size(); dword++) {
    MemoryInfo entry;
    entry.kind = ResourceKind::ScalarAddress;
    entry.data_bits = 32u;
    entry.data_dwords = 1u;
    entry.offset = dword * 4u;
    words[dword] = fixture->Emit(
        ValueOpcode::LoadAddressU32,
        {table, offset, Value(0u), Value(true)},
        fixture->AddMemory(entry, 0x70));
  }
  const auto image = fixture->Image(words, 0x70);
  MemoryInfo query;
  query.kind = ResourceKind::Image;
  query.image_dimension = Decoder::ImageDimension::Dim2D;
  fixture->Emit(ValueOpcode::ImageQueryDimensions,
                {image, fixture->ImageAddress()}, fixture->AddMemory(query, 0x70));
  const auto reused_image = fixture->Image(words, 0x74);
  fixture->Emit(ValueOpcode::ImageQueryDimensions,
                {reused_image, fixture->ImageAddress()}, fixture->AddMemory(query, 0x74));
  const auto output = fixture->Buffer({fixture->UserData(7), fixture->UserData(8),
                                       fixture->UserData(9), fixture->UserData(10)});
  MemoryInfo store;
  store.kind = ResourceKind::Buffer;
  fixture->Emit(ValueOpcode::StoreBufferU32,
                {output, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
                fixture->AddMemory(store, 0x78));
  return fixture;
}

void TestBufferRecordImageKey() {
  auto formatted = MakeBufferRecordImageFixture(true);
  BuildSrtPlan(formatted->program);
  CheckFatal([&] { TrackResources(formatted->program); },
             "not a valid runtime value",
             "formatted vector load was accepted as a raw record key");

  auto fixture = MakeBufferRecordImageFixture(false);
  fixture->PlanAndTrack();
  Check(fixture->program.info.images.size() == 1u,
        "record-key image was not tracked");
  const auto &indirect = fixture->program.descriptor_sources[
      fixture->program.info.images[0].source].indirect_image;
  Check(indirect.has_value() && indirect->record_key &&
            indirect->selector_offset == 8u,
        "lane-selected buffer record was not recognized as an image key");

  auto plan = ExtractResourcePlan(fixture->program);
  LinearTestMemory memory;
  memory.words[1] = 3u; // A wrapped U32 index reaches byte 4 with stride 12.
  memory.words[2] = 1u;
  memory.words[5] = 2u;
  std::array<uint32_t, 8> descriptor{};
  descriptor[0] = 0x20u;
  descriptor[1] = static_cast<uint32_t>(
      Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
  descriptor[2] = 3u | (3u << 14u);
  descriptor[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  for (uint32_t key_value = 1u; key_value <= 3u; key_value++) {
    for (uint32_t dword = 0; dword < descriptor.size(); dword++) {
      memory.words[(0x2000u - memory.base) / 4u + key_value * 8u + dword] =
          descriptor[dword];
    }
    memory.words[(0x2000u - memory.base) / 4u + key_value * 8u] += key_value;
  }
  std::array<uint32_t, 11> user_data{0x1000u, 12u << 16u, 2u, 0u,
                                     0x2000u, 0u, 0u, 0x4000u,
                                     4u << 16u, 1u, 0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const SrtRuntime runtime{.user_data = user_data,
                           .userdata = &memory,
                           .read_specialization_memory = ReadLinearTestMemory};
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 4u &&
            snapshot.flattened_srt.size() >= 9u &&
            snapshot.flattened_srt[0] == 4u &&
            snapshot.flattened_srt[1] == 0u &&
            snapshot.flattened_srt[3] == 1u &&
            snapshot.flattened_srt[5] == 2u &&
            snapshot.flattened_srt[7] == 3u,
        "wrapped buffer record keys were not materialized");
  user_data[7] = 0x1000u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "written buffer alias with a record key was accepted");
}

void TestGuardedDirectImageTable() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Guard { Nonzero, Zero, Unrelated, Bypass };
  const auto make_plan = [](Guard guard) {
    Fixture fixture(ShaderType::Pixel);
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
    fixture.program.block_info[0].condition =
        fixture.Emit(ValueOpcode::LogicalNot, {nonzero});
    fixture.program.block_info[0].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = guard == Guard::Zero ? 1u : 4u,
        .false_block = guard == Guard::Zero ? 4u : 1u};
    for (uint32_t block = 1; block < 4; ++block) {
      fixture.program.block_info[block].terminator = {
          .kind = CFG::TerminatorKind::Branch, .true_block = block + 1u};
    }
    fixture.program.block_info[4].terminator = {
        .kind = guard == Guard::Bypass ? CFG::TerminatorKind::Branch
                                      : CFG::TerminatorKind::Return,
        .true_block = 3u};
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
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_image;
    Check(indirect && indirect->material_source == UINT32_MAX &&
              indirect->selector_stride == 0u && indirect->table_offset == 344u &&
              indirect->key_count.Resolve().IsImmediate() &&
              indirect->key_count.Resolve().U32() == 32u &&
              fixture.program.descriptor_sources[indirect->table_source].dword_count == 2u,
          "guarded direct image table lost its pointer or proven selector range");
    return ExtractResourcePlan(fixture.program);
  };

  auto plan = make_plan(Guard::Nonzero);
  for (const auto guard : {Guard::Zero, Guard::Unrelated, Guard::Bypass}) {
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
    Bounded, WrongGuard, EntryBypass, ExitBypass, GuardBlock, WrongStep
  };
  const auto make_plan = [](Variant variant) {
    Fixture fixture;
    auto *entry = fixture.block;
    auto *header = fixture.AddBlock();
    auto *body = fixture.AddBlock();
    auto *latch = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    entry->AddBranch(header);
    header->AddBranch(exit);
    header->AddBranch(body);
    body->AddBranch(latch);
    latch->AddBranch(header);
    if (variant == Variant::EntryBypass) entry->AddBranch(body);
    if (variant == Variant::ExitBypass) exit->AddBranch(body);
    fixture.program.block_info[0].terminator = {
        .kind = variant == Variant::EntryBypass ? CFG::TerminatorKind::ConditionalBranch
                                               : CFG::TerminatorKind::Branch,
        .true_block = 1u, .false_block = 2u};
    fixture.program.block_info[1].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = 4u, .false_block = 2u};
    fixture.program.block_info[2].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 3u};
    fixture.program.block_info[3].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 1u};
    fixture.program.block_info[4].terminator = {
        .kind = variant == Variant::ExitBypass ? CFG::TerminatorKind::Branch
                                              : CFG::TerminatorKind::Return,
        .true_block = 2u};

    auto &phi = header->AppendNewInst(ValueOpcode::Phi, {},
                                      static_cast<uint64_t>(Type::U32));
    const auto key = Value(&phi);
    const auto count = fixture.UserData(2);
    const auto in_range = fixture.Emit(ValueOpcode::SLessThan32,
                                       {variant == Variant::WrongGuard
                                            ? Value(0u) : key,
                                        count}, 0, header);
    const auto allowed = fixture.Emit(ValueOpcode::LogicalAnd,
                                      {in_range, Value(true)}, 0, header);
    fixture.program.block_info[1].condition =
        fixture.Emit(ValueOpcode::LogicalNot, {allowed}, 0, header);
    const auto step = fixture.Emit(ValueOpcode::IAdd32,
                                   {key, Value(variant == Variant::WrongStep ? 2u : 1u)},
                                   0, latch);
    phi.AddPhiOperand(entry, Value(0u));
    phi.AddPhiOperand(latch, step);

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
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_image;
    Check(indirect && indirect->material_source == UINT32_MAX &&
              indirect->table_offset == 0x6b0u &&
              indirect->key_count.Resolve() == count.Resolve(),
          "bounded compute loop lost its runtime image count");
    return ExtractResourcePlan(fixture.program);
  };

  auto plan = make_plan(Variant::Bounded);
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
  const auto make_plan = [](bool wrong_update, bool wrong_equality) {
    Fixture fixture;
    auto *entry = fixture.block;
    auto *header = fixture.AddBlock();
    auto *inactive = fixture.AddBlock();
    auto *sentinel = fixture.AddBlock();
    auto *bit = fixture.AddBlock();
    auto *merge = fixture.AddBlock();
    auto *choose = fixture.AddBlock();
    auto *sample = fixture.AddBlock();
    auto *done = fixture.AddBlock();
    entry->AddBranch(header);
    header->AddBranch(inactive);
    inactive->AddBranch(merge);
    inactive->AddBranch(sentinel);
    sentinel->AddBranch(merge);
    sentinel->AddBranch(bit);
    bit->AddBranch(header);
    bit->AddBranch(merge);
    merge->AddBranch(choose);
    choose->AddBranch(sample);
    choose->AddBranch(done);
    sample->AddBranch(done);
    fixture.program.block_info[0].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 1u};
    fixture.program.block_info[1].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 2u};
    const auto active_on_entry = fixture.Emit(
        ValueOpcode::INotEqual32, {fixture.UserData(5u), Value(0u)}, 0, entry);
    auto &active_phi = header->AppendNewInst(
        ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U1));
    auto &mask_phi = header->AppendNewInst(
        ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    const auto mask = Value(&mask_phi);
    const auto active = Value(&active_phi);
    fixture.program.block_info[2].condition = fixture.Emit(
        ValueOpcode::LogicalNot, {active}, 0, inactive);
    fixture.program.block_info[2].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = 5u, .false_block = 3u};
    const auto nonzero = fixture.Emit(
        ValueOpcode::INotEqual32, {Value(0u), mask}, 0, sentinel);
    const auto bit_guard = fixture.Emit(
        ValueOpcode::LogicalAnd, {active, nonzero}, 0, sentinel);
    fixture.program.block_info[3].condition = fixture.Emit(
        ValueOpcode::LogicalNot, {bit_guard}, 0, sentinel);
    fixture.program.block_info[3].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = 5u, .false_block = 4u};
    const auto first = fixture.Emit(ValueOpcode::FindILsb32, {mask}, 0, bit);
    const auto position = fixture.Emit(
        ValueOpcode::BitwiseAnd32, {first, Value(31u)}, 0, bit);
    const auto one_bit = fixture.Emit(
        ValueOpcode::ShiftLeftLogical32, {Value(1u), position}, 0, bit);
    const auto cleared = fixture.Emit(
        wrong_update ? ValueOpcode::BitwiseOr32 : ValueOpcode::BitwiseXor32,
        {mask, one_bit}, 0, bit);
    const auto continuation = fixture.Emit(
        ValueOpcode::LogicalAnd, {bit_guard, active_on_entry}, 0, bit);
    fixture.program.block_info[4].condition = continuation;
    fixture.program.block_info[4].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = 1u, .false_block = 5u};
    active_phi.AddPhiOperand(entry, active_on_entry);
    active_phi.AddPhiOperand(bit, continuation);
    mask_phi.AddPhiOperand(entry, fixture.UserData(4u));
    mask_phi.AddPhiOperand(bit, cleared);
    const auto arbitrary = fixture.UserData(6u);
    const auto sentinel_index = fixture.Emit(
        ValueOpcode::SelectU32, {active, Value(32u), arbitrary}, 0, sentinel);
    const auto bit_index = fixture.Emit(
        ValueOpcode::SelectU32, {bit_guard, first, sentinel_index}, 0, bit);
    auto &index_phi = merge->AppendNewInst(
        ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    index_phi.AddPhiOperand(inactive, arbitrary);
    index_phi.AddPhiOperand(sentinel, sentinel_index);
    index_phi.AddPhiOperand(bit, bit_index);
    const auto index = Value(&index_phi);
    const auto below = fixture.Emit(
        ValueOpcode::SGreaterThan32, {Value(32u), index}, 0, merge);
    const auto material_guard = fixture.Emit(
        ValueOpcode::LogicalAnd, {active_on_entry, below}, 0, merge);
    fixture.program.block_info[5].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 6u};
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
    const auto loaded = fixture.Emit(
        ValueOpcode::LoadAddressU32,
        {base, material_offset, Value(0u), material_guard},
        fixture.AddMemory(material_memory, 0x1a88u), choose);
    const auto local = fixture.Emit(
        ValueOpcode::SelectU32, {material_guard, loaded, arbitrary}, 0, choose);
    const auto key = fixture.Emit(
        ValueOpcode::ReadLane, {local, Value(0u)}, 0, choose);
    const auto compared = fixture.Emit(
        ValueOpcode::IEqual32,
        {key, wrong_equality ? arbitrary : local}, 0, choose);
    const auto sample_guard = fixture.Emit(
        ValueOpcode::LogicalAnd, {material_guard, compared}, 0, choose);
    fixture.program.block_info[6].condition = fixture.Emit(
        ValueOpcode::LogicalNot, {sample_guard}, 0, choose);
    fixture.program.block_info[6].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = 8u, .false_block = 7u};
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
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, address},
                 fixture.AddMemory(image_memory, 0x1aecu), sample);
    fixture.program.block_info[7].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 8u};
    fixture.program.block_info[8].terminator.kind = CFG::TerminatorKind::Return;
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
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_image;
    Check(indirect && indirect->selector_stride == 0x90u &&
              indirect->selector_offset == 0xc00u &&
              indirect->table_offset == 0x20e0u &&
              !indirect->selector_mask.IsEmpty() &&
              indirect->key_count.Resolve().IsImmediate() &&
              indirect->key_count.Resolve().U32() == 32u,
          "uniformized image key lost its finite material range");
    return ExtractResourcePlan(fixture.program);
  };
  auto plan = make_plan(false, false);
  Check(plan.requires_specialization_memory &&
            plan.descriptor_sources[plan.info.images[0].source]
                .indirect_image->selector_mask.Resolve().TryInstruction() != nullptr,
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
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "written buffer alias with a material key was accepted");
  user_data[8] = first_table;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "written buffer alias with an image record was accepted");
  CheckFatal([&] { make_plan(true, false); }, "not a valid runtime value",
             "non-clearing material mask was accepted");
  CheckFatal([&] { make_plan(false, true); }, "not a valid runtime value",
             "unrelated ReadLane key was accepted");
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
    fixture.program.block_info[0].terminator.kind =
        Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
    if (options.branch) {
      fixture.program.block_info[0].terminator.kind = Libs::Graphics::
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
            resource.stride_zero_access_size == 16u &&
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

// Descriptor format is authoritative only when every surviving access uses it.
void TestDescriptorFormattedBufferProvenance() {
  using O = ValueOpcode;
  using Format = Libs::Graphics::Prospero::BufferFormat;
  enum class Neighbor { RawDword, Typed32, Scalar, Atomic, DescriptorFormatted };
  for (const auto neighbor : {Neighbor::RawDword, Neighbor::Typed32, Neighbor::Scalar,
                              Neighbor::Atomic, Neighbor::DescriptorFormatted}) {
    for (const bool reverse : {false, true}) {
      Fixture fixture;
      const auto handle = fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                                           fixture.UserData(2), fixture.UserData(3)}, 4);
      const auto scalar_offset = fixture.UserData(4);
      const auto descriptor_store = [&] {
        MemoryInfo memory;
        memory.kind = ResourceKind::Buffer;
        memory.formatted = true;
        fixture.Emit(O::StoreBufferU32,
            {handle, Value(0u), Value(0u), Value(0u), Value(7u), Value(true)},
            fixture.AddMemory(memory, 4));
      };
      const auto other_access = [&] {
        MemoryInfo memory;
        memory.kind = neighbor == Neighbor::Scalar ? ResourceKind::ScalarBuffer
                                                   : ResourceKind::Buffer;
        memory.formatted = neighbor == Neighbor::Typed32 ||
                           neighbor == Neighbor::DescriptorFormatted;
        memory.typed = neighbor == Neighbor::Typed32;
        if (memory.typed) {
          // Explicit TBUFFER32 format is independent of descriptor R8_UINT.
          const auto format = static_cast<uint32_t>(Format::k32UInt);
          memory.data_format = format & 0xfu;
          memory.number_format = format >> 4u;
        }
        const auto flags = fixture.AddMemory(memory, 8);
        Value read;
        if (neighbor == Neighbor::Scalar)
          read = fixture.Emit(O::ReadConstBuffer, {handle, scalar_offset}, flags);
        else if (neighbor == Neighbor::Atomic)
          read = fixture.Emit(O::BufferAtomicOr32,
              {handle, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)}, flags);
        else
          read = fixture.Emit(O::LoadBufferU32,
              {handle, Value(0u), Value(0u), Value(0u), Value(true)}, flags);
        fixture.Emit(O::ReferenceU32, {read});
      };
      if (reverse) { other_access(); descriptor_store(); }
      else { descriptor_store(); other_access(); }
      fixture.PlanAndTrack();
      EliminateDeadCode(fixture.program.blocks);
      ValidateProgram(fixture.program, true);
      const bool expected = neighbor == Neighbor::DescriptorFormatted;
      Check(fixture.program.info.buffers.size() == 1u &&
                fixture.program.descriptor_sources.size() == 1u,
            "format provenance split accesses sharing one descriptor source");
      const auto& resource = fixture.program.info.buffers[0];
      Check(resource.read && resource.written && resource.formatted &&
                resource.scalar == (neighbor == Neighbor::Scalar) &&
                resource.atomic == (neighbor == Neighbor::Atomic),
            "format provenance fixture lost a live neighboring access");
      Check(resource.descriptor_formatted_only == expected,
            "descriptor format provenance must include every access in either order");
      auto plan = ExtractResourcePlan(fixture.program);
      Check(plan.info.buffers[0].descriptor_formatted_only == expected,
            "resource-plan extraction lost descriptor format provenance");
      std::array<uint32_t, 5> user_data{0x1000u, 0u, 64u,
          Libs::Graphics::DstSel(4, 5, 6, 7) |
              (static_cast<uint32_t>(Format::k8UInt) << 12u) | (1u << 24u), 0u};
      TestMemory memory;
      SrtRuntime runtime{.user_data = user_data, .read_memory = ReadTestMemory,
                         .userdata = &memory};
      ResourceSnapshot snapshot;
      ResourceSpecialization specialization;
      Check(MaterializeResources(plan, runtime, snapshot, specialization),
            "format provenance fixture failed ordinary descriptor materialization");
      ApplyResourceSpecialization(fixture.program, specialization);
      ValidateProgram(fixture.program, true);
      Check(fixture.program.info.buffers.size() == 1u &&
                fixture.program.info.buffers[0].descriptor_formatted_only == expected &&
                fixture.program.info.buffers[0].descriptor_format == Format::k8UInt,
            "specialization lost provenance or confused TBUFFER and descriptor formats");
    }
  }
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
  Check(SrtWalker(fixture.program, runtime).EvaluateDescriptor(source, value) &&
            value.dwords[3] == 0x100u,
        "runtime descriptor unsigned minimum did not clamp its first operand");
  user_data[0] = 0x80u;
  Check(
      SrtWalker(fixture.program, runtime).EvaluateDescriptor(source, value) &&
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

void TestInvalidSampledFormatHoleCanonicalization() {
  Fixture fixture;
  std::array<Value, 8> image_words;
  for (uint32_t index = 0; index < image_words.size(); ++index) {
    image_words[index] = fixture.UserData(index);
  }
  const auto image = fixture.Image(image_words, 0x180);
  const auto sampler = fixture.Sampler(
      {Value(0u), Value(0u), Value(0u), Value(0u)}, 0x180);
  MemoryInfo memory;
  memory.kind = ResourceKind::Image;
  memory.image_dimension = Decoder::ImageDimension::Dim2D;
  const auto sampled = fixture.Emit(
      ValueOpcode::ImageSampleRaw,
      {image, sampler, fixture.ImageAddress()}, fixture.AddMemory(memory, 0x180));
  fixture.Emit(ValueOpcode::ReferenceU32, {sampled});
  fixture.PlanAndTrack();

  // 98 lies in the reserved gap between the ordinary and SRGB format ranges.
  // Random/stale descriptor bits must become the canonical null image instead
  // of surviving validation and failing later numeric specialization.
  std::array<uint32_t, 8> user_data{};
  user_data[0] = 0x2000u;
  user_data[1] = 98u << 20u;
  user_data[2] = 3u | (3u << 14u);
  user_data[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  const auto plan = ExtractResourcePlan(fixture.program);
  const SrtRuntime runtime{.user_data = user_data};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 1u &&
            std::ranges::all_of(snapshot.images[0].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "reserved sampled-image format was not canonicalized to null");
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
  Check(SrtWalker(fixture.program, runtime).EvaluateDescriptor(source, descriptor) &&
            descriptor.dwords[3] == 0x80000abcu,
        "SampleAdjust canonicalization lost sampler border fields");

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
    BuildSrtPlan(rejected.program);
    CheckFatal([&] { TrackResources(rejected.program); },
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
  AllocateBindings(fixture.program);
  const auto kind = DescriptorBindingForImage(fixture.program.info.images[0]);
  Check(kind.has_value() &&
            FindBinding(fixture.program.bindings, *kind)->resources ==
                std::vector<uint32_t>{0},
        "FMASK allocated an ordinary image descriptor");
  user_data[8] = 0;
  user_data[1] = static_cast<uint32_t>(Prospero::BufferFormat::k8UInt) << 20u;
  ResourceSpecialization rebound;
  Check(MaterializeResources(plan, {.user_data = user_data}, snapshot, rebound) &&
            rebound != specialization && snapshot.images.size() == 2,
        "rebinding FMASK as a texture reused the metadata specialization");
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
  DescriptorValue descriptor;
  std::vector<uint32_t> flat;
  const uint32_t request = fixture.program.info.buffers[0].source;
  const auto refresh = [&](const ResourcePlan& plan) {
    SrtWalker walker(plan, runtime);
    return walker.EvaluateDescriptor(request, descriptor) && walker.RefreshFlatBuffer(flat);
  };
  Check(refresh(fixture.program), "typed runtime source evaluation failed");
  Check(descriptor.dwords[0] == 0xdeadbeefu &&
            flat == std::vector<uint32_t>{0xdeadbeefu} && memory.reads == 1,
        "descriptor and flat SRT evaluation did not share one memoized read");

  memory.reads = 0;
  memory.words[1] = 0x12345678u;
  Check(refresh(fixture.program) && descriptor.dwords[0] == 0x12345678u &&
            flat == std::vector<uint32_t>{0x12345678u} && memory.reads == 1,
        "repeated runtime evaluation reused stale scalar memory");

  memory.reads = 0;
  memory.fail_after = 0;
  Check(!refresh(fixture.program), "unavailable scalar memory was accepted");
  memory.fail_after = UINT32_MAX;
  Check(refresh(fixture.program) && descriptor.dwords[0] == 0x12345678u && memory.reads == 1,
        "failed runtime evaluation left a value marked as visiting");

  auto detached = ExtractResourcePlan(fixture.program);
  Check(refresh(detached), "detached resource plan did not evaluate");
  auto moved = std::move(detached);
  memory.reads = 0;
  memory.words[1] = 0x87654321u;
  Check(refresh(moved) && descriptor.dwords[0] == 0x87654321u && memory.reads == 1,
        "moving a cached resource plan lost its evaluation state");

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
  Check(SrtWalker(fixture.program, runtime).EvaluateDescriptor(fixture.program.info.buffers[0].source, value) &&
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

void TestConditionalScalarAddressReadRemainsRuntime() {
  const auto check = [](bool conditional) {
    Fixture fixture;
    auto *entry = fixture.block;
    auto *read_block = conditional ? fixture.AddBlock() : entry;
    auto *exit = fixture.AddBlock();
    if (conditional) {
      entry->AddBranch(read_block);
      entry->AddBranch(exit);
      fixture.program.block_info[0].condition = fixture.UserData(2);
      fixture.program.block_info[0].terminator = {
          .kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = 1u, .false_block = 2u};
      read_block->AddBranch(exit);
      fixture.program.block_info[1].terminator = {
          .kind = CFG::TerminatorKind::Branch, .true_block = 2u};
      fixture.program.block_info[2].terminator.kind = CFG::TerminatorKind::Return;
    } else {
      entry->AddBranch(exit);
      fixture.program.block_info[0].terminator = {
          .kind = CFG::TerminatorKind::Branch, .true_block = 1u};
      fixture.program.block_info[1].terminator.kind = CFG::TerminatorKind::Return;
    }
    const auto address = fixture.Address(fixture.UserData(0), fixture.UserData(1));
    const auto read = fixture.Emit(
        ValueOpcode::LoadAddressU32,
        {address, Value(0u), Value(0u), Value(true)},
        fixture.AddMemory({.kind = ResourceKind::ScalarAddress}, 0x39dcu),
        read_block);
    fixture.Emit(ValueOpcode::ReferenceU32, {read}, 0u, read_block);
    fixture.PlanAndTrack();
    Check(conditional ? fixture.program.srt_reads.empty()
                      : fixture.program.srt_reads.size() == 1u,
          "conditional scalar address read was eagerly flattened");
    const std::array<uint32_t, 3> user_data{0u, 0u, 0u};
    const auto plan = ExtractResourcePlan(fixture.program);
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const bool accepted = MaterializeResources(
        plan, {.user_data = user_data, .read_memory = RejectTestMemory},
        snapshot, specialization);
    Check(accepted == conditional,
          "null optional read affected materialization or mandatory read was accepted");
    if (conditional) {
      Check(read.ResolveInstruction()->GetOpcode() == ValueOpcode::LoadAddressU32,
            "conditional scalar address read lost its guarded runtime operation");
    }
  };
  check(true);
  check(false);
}

enum class BoundedSrtScenario {
  Valid, ReversedGuard, WrappedOffset, SparseBlockIds, NonzeroStart, NonunitStep,
  SignedGuard, UnknownBound, ReadBeforeGuard, WrongSuccessEdge,
  NonlinearOffset, DynamicPointer, GuardedPointerLoad, MixedDescriptorColumns, VertexStage,
};

struct BoundedSrtFixture {
  std::unique_ptr<Fixture> fixture;
  std::array<Value, 4> descriptor_words;
  Value threshold;
  Value index;
};

BoundedSrtFixture MakeBoundedSrtTrackingFixture(BoundedSrtScenario scenario) {
  BoundedSrtFixture result;
  result.fixture = std::make_unique<Fixture>(
      scenario == BoundedSrtScenario::VertexStage ? ShaderType::Vertex : ShaderType::Compute);
  auto &fixture = *result.fixture;
  auto *entry = fixture.block;
  auto *header = fixture.AddBlock();
  auto *body = fixture.AddBlock();
  auto *latch = fixture.AddBlock();
  auto *exit = fixture.AddBlock();
  const auto Branch = [&](uint32_t from, uint32_t to) {
    fixture.program.blocks[from]->AddBranch(fixture.program.blocks[to]);
    auto &term = fixture.program.block_info[from].terminator;
    term.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Branch;
    term.true_block = to;
  };
  Branch(0, 1);
  Branch(2, 3);
  Branch(3, 1);
  header->AddBranch(body);
  header->AddBranch(exit);
  auto &term = fixture.program.block_info[1].terminator;
  term.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  term.true_block = 4; // LogicalNot(i<N): successful loop body is false edge.
  term.false_block = 2;
  auto low = fixture.UserData(0);
  const auto high = fixture.UserData(1);
  const auto count = scenario == BoundedSrtScenario::UnknownBound
                         ? fixture.Emit(ValueOpcode::LaneId)
                         : fixture.UserData(2);
  auto &phi = header->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
  result.index = Value(&phi);
  const auto next = fixture.Emit(ValueOpcode::IAdd32,
      {result.index, Value(scenario == BoundedSrtScenario::NonunitStep ? 2u : 1u)}, 0, latch);
  phi.AddPhiOperand(entry, Value(scenario == BoundedSrtScenario::NonzeroStart ? 1u : 0u));
  phi.AddPhiOperand(latch, next);
  const auto compare = fixture.Emit(
      scenario == BoundedSrtScenario::SignedGuard ? ValueOpcode::SLessThan32
      : scenario == BoundedSrtScenario::ReversedGuard ? ValueOpcode::UGreaterThan32
                                                     : ValueOpcode::ULessThan32,
      scenario == BoundedSrtScenario::ReversedGuard
          ? std::initializer_list<Value>{count, result.index}
          : std::initializer_list<Value>{result.index, count}, 0, header);
  fixture.program.block_info[1].condition =
      fixture.Emit(ValueOpcode::LogicalNot, {compare}, 0, header);
  if (scenario == BoundedSrtScenario::WrongSuccessEdge) {
    // Same CFG shape, but the read is reached on i>=N. A block-dominance-only
    // check must not mistake this for a bounded read.
    term.true_block = 2;
    term.false_block = 4;
  }
  fixture.block = body;
  if (scenario == BoundedSrtScenario::DynamicPointer)
    low = fixture.Emit(ValueOpcode::IAdd32, {low, result.index});
  if (scenario == BoundedSrtScenario::GuardedPointerLoad) {
    const auto pointer_address = fixture.Address(low, high, 0x84);
    MemoryInfo pointer_memory;
    pointer_memory.kind = ResourceKind::ScalarAddress;
    low = fixture.Emit(ValueOpcode::LoadAddressU32,
        {pointer_address, Value(0u), Value(0u), Value(true)},
        fixture.AddMemory(pointer_memory, 0x84));
  }
  const auto address = fixture.Address(low, high, 0x90);
  const auto scale = scenario == BoundedSrtScenario::WrappedOffset ? 0x80000000u : 16u;
  auto offset = fixture.Emit(ValueOpcode::IMul32,
      {result.index, scenario == BoundedSrtScenario::NonlinearOffset ? result.index : Value(scale)});
  if (scenario == BoundedSrtScenario::WrappedOffset)
    offset = fixture.Emit(ValueOpcode::IAdd32, {offset, Value(0xfffffffcu)});
  auto *read_block = scenario == BoundedSrtScenario::ReadBeforeGuard ? header : body;
  // For the before-guard negative, create its independent address and affine
  // offset in the header as well: the fixture remains valid SSA.
  auto read_address = address;
  if (read_block == header) {
    read_address = fixture.Emit(ValueOpcode::GetAddressResource, {low, high},
                               MemoryFlags{0, 0x90}, header);
    offset = fixture.Emit(ValueOpcode::IMul32, {result.index, Value(16u)}, 0, header);
  }
  for (uint32_t word = 0; word < 4u; ++word) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.offset = (scenario == BoundedSrtScenario::WrappedOffset ? 4u : 16u) + word * 4u;
    memory.component_count = 4u;
    memory.component_index = word;
    if (scenario == BoundedSrtScenario::MixedDescriptorColumns && word == 3u)
      memory.offset += 4u;
    result.descriptor_words[word] = fixture.Emit(ValueOpcode::LoadAddressU32,
        {read_address, offset, Value(0u), Value(true)},
        fixture.AddMemory(memory, 0x100), read_block);
  }
  MemoryInfo threshold_memory;
  threshold_memory.kind = ResourceKind::ScalarAddress;
  threshold_memory.offset = 80u;
  const auto threshold_offset = fixture.Emit(ValueOpcode::IMul32, {result.index, Value(8u)});
  result.threshold = fixture.Emit(ValueOpcode::LoadAddressU32,
      {address, threshold_offset, Value(0u), Value(true)},
      fixture.AddMemory(threshold_memory, 0x120));
  const auto buffer = fixture.Buffer(result.descriptor_words, 0x140);
  MemoryInfo store;
  store.kind = ResourceKind::Buffer;
  store.idxen = true;
  fixture.Emit(ValueOpcode::StoreBufferU32,
      {buffer, Value(0u), Value(0u), Value(0u), result.threshold, Value(true)},
      fixture.AddMemory(store, 0x140));
  if (scenario == BoundedSrtScenario::SparseBlockIds) {
    // Real translation prepends an entry whose ID differs from its ordinal.
    // Branch metadata refers to IDs; phi edges refer to block pointers.
    constexpr std::array ids{100u, 7u, 42u, 19u, 81u};
    for (uint32_t i = 0; i < fixture.program.block_info.size(); ++i) {
      auto &info = fixture.program.block_info[i];
      info.id = ids[i];
      if (info.terminator.true_block != UINT32_MAX)
        info.terminator.true_block = ids[info.terminator.true_block];
      if (info.terminator.false_block != UINT32_MAX)
        info.terminator.false_block = ids[info.terminator.false_block];
    }
  }
  return result;
}

void TestBoundedSrtTrackingProofBoundaries() {
  for (auto scenario : {BoundedSrtScenario::NonzeroStart, BoundedSrtScenario::NonunitStep,
                        BoundedSrtScenario::UnknownBound,
                        BoundedSrtScenario::ReadBeforeGuard, BoundedSrtScenario::WrongSuccessEdge,
                        BoundedSrtScenario::NonlinearOffset, BoundedSrtScenario::DynamicPointer,
                        BoundedSrtScenario::GuardedPointerLoad, BoundedSrtScenario::MixedDescriptorColumns,
                        BoundedSrtScenario::VertexStage}) {
    auto fixture = MakeBoundedSrtTrackingFixture(scenario);
    BuildSrtPlan(fixture.fixture->program);
    CheckFatal([&] { TrackResources(fixture.fixture->program); },
               "not a valid runtime value", "unproved scalar descriptor loop was accepted");
    Check(!fixture.fixture->program.resource_tracking_complete &&
              fixture.fixture->program.info.buffers.empty() &&
              fixture.fixture->program.descriptor_sources.empty(),
          "rejected scalar descriptor loop mutated the tracked resource plan");
  }
  std::cout << "bounded SRT rejection boundaries passed: 10\n";
  for (auto scenario : {BoundedSrtScenario::Valid, BoundedSrtScenario::ReversedGuard,
                        BoundedSrtScenario::WrappedOffset, BoundedSrtScenario::SparseBlockIds,
                        BoundedSrtScenario::SignedGuard}) {
    auto fixture = MakeBoundedSrtTrackingFixture(scenario);
    fixture.fixture->PlanAndTrack();
    const auto &program = fixture.fixture->program;
    Check(program.resource_tracking_complete && program.info.buffers.size() == 1u &&
              !program.info.uses_dma,
          "bounded scalar descriptor loop did not retain one logical buffer table");
    for (const auto read : fixture.descriptor_words) {
      const auto *indexed = read.Resolve().TryInstruction();
      Check(indexed != nullptr &&
                std::string_view(ValueOpcodeName(indexed->GetOpcode())) == "ReadBoundedSrtU32" &&
                indexed->Arg(0).Resolve() == fixture.index,
            "bounded descriptor word discarded its live induction index");
    }
    const auto *threshold = fixture.threshold.Resolve().TryInstruction();
    Check(threshold != nullptr &&
              std::string_view(ValueOpcodeName(threshold->GetOpcode())) == "ReadBoundedSrtU32" &&
              threshold->Arg(0).Resolve() == fixture.index,
          "ordinary scalar threshold was not rewritten to the indexed snapshot");
    Check(std::ranges::none_of(program.memory_info, [](const MemoryInfo &memory) {
            return memory.planning_only;
          }), "bounded raw reads were discarded with a planning_only shortcut");
  }

}

struct DispatcherSignedBufferLoopFixture {
  std::unique_ptr<Fixture> fixture;
  std::array<Value, 4> descriptor_words;
  Value index;
};

DispatcherSignedBufferLoopFixture MakeDispatcherSignedBufferLoopFixture(
    bool bypass_count_guard = false, uint32_t step = 1u) {
  DispatcherSignedBufferLoopFixture result;
  result.fixture = std::make_unique<Fixture>();
  auto& fixture = *result.fixture;
  fixture.program.dispatcher_fallback = true;

  auto* entry = fixture.block;
  auto* header = fixture.AddBlock();
  auto* mask_guard = fixture.AddBlock();
  auto* body = fixture.AddBlock();
  auto* latch = fixture.AddBlock();
  auto* exit = fixture.AddBlock();
  const auto Branch = [&](uint32_t from, uint32_t to) {
    fixture.program.blocks[from]->AddBranch(fixture.program.blocks[to]);
    auto& term = fixture.program.block_info[from].terminator;
    term.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Branch;
    term.true_block = to;
  };
  if (bypass_count_guard) {
    entry->AddBranch(header);
    entry->AddBranch(body);
    auto& term = fixture.program.block_info[0];
    term.terminator.kind =
        Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
    term.terminator.true_block = 1u;
    term.terminator.false_block = 3u;
    term.condition = fixture.Emit(ValueOpcode::INotEqual32,
                                  {fixture.UserData(7u), Value(0u)}, 0, entry);
  } else {
    Branch(0u, 1u);
  }
  header->AddBranch(exit);
  header->AddBranch(mask_guard);
  auto& header_info = fixture.program.block_info[1];
  header_info.terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  header_info.terminator.true_block = 5u;
  header_info.terminator.false_block = 2u;
  mask_guard->AddBranch(latch);
  mask_guard->AddBranch(body);
  auto& mask_info = fixture.program.block_info[2];
  mask_info.terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  mask_info.terminator.true_block = 4u;
  mask_info.terminator.false_block = 3u;
  Branch(3u, 4u);
  Branch(4u, 1u);
  fixture.program.block_info[5].terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;

  fixture.block = entry;
  const std::array<Value, 4> table_descriptor{
      fixture.UserData(0u), fixture.UserData(1u), fixture.UserData(2u),
      fixture.UserData(3u)};
  const auto count = fixture.UserData(4u);
  const auto enabled_mask = fixture.UserData(5u);
  auto& phi = header->AppendNewInst(ValueOpcode::Phi, {},
                                    static_cast<uint64_t>(Type::U32));
  result.index = Value(&phi);
  const auto next = fixture.Emit(ValueOpcode::IAdd32,
                                 {result.index, Value(step)}, 0, latch);
  phi.AddPhiOperand(entry, Value(0u));
  phi.AddPhiOperand(latch, next);
  const auto compare = fixture.Emit(ValueOpcode::SLessThan32,
                                    {result.index, count}, 0, header);
  header_info.condition =
      fixture.Emit(ValueOpcode::LogicalNot, {compare}, 0, header);
  const auto bit_index = fixture.Emit(ValueOpcode::BitwiseAnd32,
                                      {result.index, Value(31u)}, 0, header);
  const auto bit = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
                                {Value(1u), bit_index}, 0, header);
  const auto enabled = fixture.Emit(ValueOpcode::BitwiseAnd32,
                                    {enabled_mask, bit}, 0, header);
  const auto enabled_nonzero = fixture.Emit(ValueOpcode::INotEqual32,
                                            {enabled, Value(0u)}, 0, mask_guard);
  mask_info.condition =
      fixture.Emit(ValueOpcode::LogicalNot, {enabled_nonzero}, 0, mask_guard);

  fixture.block = body;
  const auto table = fixture.Buffer(table_descriptor, 0x62d0u);
  const auto row = fixture.Emit(ValueOpcode::IMul32,
                                {result.index, Value(196u)});
  for (uint32_t word = 0u; word < result.descriptor_words.size(); ++word) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarBuffer;
    memory.offset = word * sizeof(uint32_t);
    memory.component_count = 4u;
    memory.component_index = word;
    result.descriptor_words[word] = fixture.Emit(
        ValueOpcode::ReadConstBuffer, {table, row},
        fixture.AddMemory(memory, 0x62d0u + word * sizeof(uint32_t)));
  }
  const auto selected = fixture.Buffer(result.descriptor_words, 0x656cu);
  MemoryInfo load;
  load.kind = ResourceKind::Buffer;
  load.idxen = true;
  const auto value = fixture.Emit(
      ValueOpcode::LoadBufferU16,
      {selected, Value(0u), Value(0u), Value(0u), Value(true)},
      fixture.AddMemory(load, 0x6574u));
  fixture.Emit(ValueOpcode::ReferenceU32,
               {fixture.Emit(ValueOpcode::ConvertU32U16, {value})});
  return result;
}

void TestDispatcherSignedBufferLoop() {
  auto accepted = MakeDispatcherSignedBufferLoopFixture();
  accepted.fixture->PlanAndTrack();
  const auto& program = accepted.fixture->program;
  Check(program.resource_tracking_complete && program.info.buffers.size() == 1u &&
            program.bounded_srt_reads.size() == 4u && !program.info.uses_dma,
        "dispatcher signed loop did not retain one bounded buffer table");
  for (const auto word : accepted.descriptor_words) {
    const auto* read = word.Resolve().TryInstruction();
    Check(read != nullptr && read->GetOpcode() == ValueOpcode::ReadBoundedSrtU32 &&
              read->Arg(0).Resolve() == accepted.index,
          "dispatcher signed loop discarded its live induction index");
  }
  Check(std::ranges::all_of(program.bounded_srt_reads,
                            [](const BoundedSrtRead& read) {
                              return read.count_signed && read.offset_scale == 196u;
                            }),
        "dispatcher signed loop lost its signed count or 196-byte stride");

  const auto plan = ExtractResourcePlan(program);
  LinearTestMemory table;
  table.words.resize((2u * 196u + 16u) / sizeof(uint32_t));
  for (uint32_t row = 0u; row < 2u; ++row) {
    const std::array<uint32_t, 4> descriptor{
        0x20000u + row * 0x100u, 4u << 16u, 4u, 0u};
    for (uint32_t word = 0u; word < descriptor.size(); ++word)
      table.words[(row * 196u) / sizeof(uint32_t) + word] = descriptor[word];
  }
  std::array<uint32_t, 6> user_data{
      static_cast<uint32_t>(table.base), 0u,
      static_cast<uint32_t>(table.words.size() * sizeof(uint32_t)), 0u, 2u,
      0xffffffffu};
  const auto Materialize = [&](uint32_t count, ResourceSnapshot& snapshot,
                               ResourceSpecialization& specialization) {
    user_data[4] = count;
    const SrtRuntime runtime{.user_data = user_data,
                             .read_memory = ReadLinearTestMemory,
                             .userdata = &table,
                             .read_specialization_memory = ReadLinearTestMemory};
    return MaterializeResources(plan, runtime, snapshot, specialization);
  };
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const bool materialized = Materialize(2u, snapshot, specialization);
  const bool two_rows = materialized && specialization.buffer_tables.size() == 1u &&
                        specialization.buffer_tables[0].count == 2u &&
                        snapshot.buffers.size() == 2u &&
                        snapshot.flattened_srt.size() == 10u &&
                        snapshot.buffers[0].dwords[0] == 0x20000u &&
                        snapshot.buffers[1].dwords[0] == 0x20100u;
  if (!two_rows) {
    std::cerr << "dispatcher signed materialization: accepted=" << materialized
              << " tables=" << specialization.buffer_tables.size()
              << " table_count="
              << (specialization.buffer_tables.empty()
                      ? UINT32_MAX
                      : specialization.buffer_tables[0].count)
              << " buffers=" << snapshot.buffers.size()
              << " flat=" << snapshot.flattened_srt.size() << '\n';
  }
  Check(two_rows,
        "dispatcher signed buffer table did not materialize two 196-byte rows");
  for (const auto count : {0u, 0xffffffffu}) {
    snapshot = {};
    specialization = {};
    Check(Materialize(count, snapshot, specialization) &&
              specialization.buffer_tables.size() == 1u &&
              specialization.buffer_tables[0].count == 0u &&
              snapshot.buffers.empty() && snapshot.flattened_srt.empty(),
          "non-positive signed loop count did not materialize zero rows");
  }

  for (const auto [bypass_count_guard, step] :
       {std::pair{true, 1u}, std::pair{false, 2u}}) {
    auto rejected =
        MakeDispatcherSignedBufferLoopFixture(bypass_count_guard, step);
    BuildSrtPlan(rejected.fixture->program);
    CheckFatal([&] { TrackResources(rejected.fixture->program); },
               "not a valid runtime value",
               "unsafe dispatcher descriptor loop was accepted");
    Check(!rejected.fixture->program.resource_tracking_complete &&
              rejected.fixture->program.info.buffers.empty(),
          "rejected dispatcher loop partially changed resource tracking");
  }
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
    fixture.program.block_info[1].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = merge_id};
  }
  fixture.program.block_info[0].terminator = {
      .kind = CFG::TerminatorKind::ConditionalBranch,
      .true_block = reverse ? alternate_id : initial_target,
      .false_block = reverse ? initial_target : alternate_id};
  fixture.program.block_info[alternate_id].terminator = {
      .kind = CFG::TerminatorKind::Branch, .true_block = merge_id};
  fixture.program.block_info[merge_id].terminator.kind =
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
  fixture.program.block_info[0].condition =
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
  Check(fixture.program.value_storage.size() == 4,
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
          Check(SrtWalker(plan, runtime, {}, &clean).EvaluateDescriptor(source, selected),
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
          Check(!SrtWalker(plan, no_clean_reader, {}, &clean).EvaluateDescriptor(source, selected),
                "conditional sampler used unchecked memory for its predicate");
        }
        memory.fail_address = 0x2000 + 196;
        {
          SrtWalker clean(plan, CleanRuntime(runtime));
          Check(!SrtWalker(plan, runtime, {}, &clean).EvaluateDescriptor(source, selected),
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

void TestGuardedSamplerPhi() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  const auto make_plan = [](bool take_true, bool reverse_phi, bool bypass,
                            bool mismatched, bool ambiguous) {
    Fixture fixture(ShaderType::Pixel);
    auto *entry = fixture.block;
    auto *loaded = fixture.AddBlock();
    auto *merge = fixture.AddBlock();
    auto *sample = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    entry->AddBranch(loaded);
    entry->AddBranch(merge);
    loaded->AddBranch(merge);
    merge->AddBranch(sample);
    merge->AddBranch(exit);
    sample->AddBranch(exit);
    if (bypass) exit->AddBranch(sample);
    const auto lane = fixture.Emit(ValueOpcode::LaneId);
    const auto predicate = fixture.Emit(ValueOpcode::IEqual32, {lane, Value(0u)});
    fixture.program.block_info[0].condition = predicate;
    fixture.program.block_info[0].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = 1u, .false_block = 2u};
    fixture.program.block_info[1].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 2u};
    auto &guard = merge->AppendNewInst(ValueOpcode::Phi, {},
                                       static_cast<uint64_t>(Type::U1));
    guard.AddPhiOperand(entry, ambiguous ? predicate : Value(!take_true));
    guard.AddPhiOperand(mismatched ? sample : loaded, predicate);
    fixture.program.block_info[2].condition = Value(&guard);
    fixture.program.block_info[2].terminator = {
        .kind = CFG::TerminatorKind::ConditionalBranch,
        .true_block = take_true ? 3u : 4u,
        .false_block = take_true ? 4u : 3u};
    fixture.program.block_info[3].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 4u};
    fixture.program.block_info[4].terminator = {
        .kind = bypass ? CFG::TerminatorKind::Branch : CFG::TerminatorKind::Return,
        .true_block = 3u};
    std::array<Value, 4> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      // An arbitrary excluded value proves this is control-flow reasoning, not null filtering.
      const auto first = Value(0xbad000u + word);
      const auto second = fixture.UserData(word);
      auto &phi = merge->AppendNewInst(ValueOpcode::Phi, {},
                                       static_cast<uint64_t>(Type::U32));
      phi.AddPhiOperand(reverse_phi ? loaded : entry, reverse_phi ? second : first);
      phi.AddPhiOperand(reverse_phi ? entry : loaded, reverse_phi ? first : second);
      words[word] = Value(&phi);
    }
    fixture.block = sample;
    const auto image = fixture.Image({Value(0u), Value(0u), Value(0u), Value(0u),
                                      Value(0u), Value(0u), Value(0u), Value(0u)});
    const auto sampler = fixture.Sampler(words);
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw,
                  {image, sampler, fixture.ImageAddress()},
                  fixture.AddMemory(memory, 0x2a0));
    fixture.PlanAndTrack();
    Check(words[0].ResolveInstruction()->GetOpcode() == ValueOpcode::Phi,
          "guarded host descriptor selection rewrote the GPU Phi");
    return ExtractResourcePlan(fixture.program);
  };
  for (const bool take_true : {false, true}) {
    for (const bool reverse_phi : {false, true}) {
      auto plan = make_plan(take_true, reverse_phi, false, false, false);
      const std::array<uint32_t, 4> user_data{0x444u, 0x555u, 0x666u, 0x777u};
      DescriptorValue selected;
      Check(SrtWalker(plan, {.user_data = user_data}).EvaluateDescriptor(
                plan.info.samplers[0].source, selected) &&
                std::equal(user_data.begin(), user_data.end(), selected.dwords.begin()),
            "guarded sampler did not match descriptor and predicate predecessors");
    }
  }
  CheckFatal([&] { make_plan(true, false, true, false, false); },
              "not a valid runtime value", "sampler guard accepted an unguarded path");
  CheckFatal([&] { make_plan(true, false, false, true, false); },
              "not a valid runtime value", "sampler guard ignored predecessor identity");
  CheckFatal([&] { make_plan(true, false, false, false, true); },
              "not a valid runtime value", "sampler guard discarded a reachable alternative");
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
  Check(SrtWalker(fixture.program, runtime).EvaluateDescriptor(fixture.program.info.buffers[0].source, descriptor) &&
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

  Check(fixture.program.info.uses_dma && fixture.program.info.writes_dma,
        "typed address store did not enable writable DMA");
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

  Check(fixture.program.info.uses_dma && !fixture.program.info.writes_dma,
        "exec-masked FLAT load did not retain read-only DMA metadata");
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
  Check(mismatch.program.info.uses_dma && !mismatch.program.info.writes_dma,
        "dynamic FLAT load did not retain read-only DMA metadata");
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
  fixture.program.block_info[0].id = 11;
  fixture.program.block_info[1].id = 27;
  fixture.program.block_info[2].id = 42;
  if (use == ConditionalBufferUse::Loop) {
    condition_block = fixture.AddBlock();
    condition_index = 3;
    fixture.program.block_info[3].id = 55;
    fixture.program.block_info[0].terminator = {
        .kind = CFG::TerminatorKind::Branch, .true_block = 55};
    entry->AddBranch(condition_block);
  }
  condition_block->AddBranch(optional);
  condition_block->AddBranch(done);
  optional->AddBranch(use == ConditionalBufferUse::Loop ? condition_block : done);
  fixture.program.block_info[condition_index].terminator = {
      .kind = CFG::TerminatorKind::ConditionalBranch,
      .true_block = 27, .false_block = 42};
  fixture.program.block_info[1].terminator = {
      .kind = CFG::TerminatorKind::Branch,
      .true_block = use == ConditionalBufferUse::Loop ? 55u : 42u};

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
  fixture.program.block_info[condition_index].condition =
      fixture.Emit(ValueOpcode::INotEqual32, {flag, Value(0u)}, 0, condition_block);

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
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers.size() == 2 &&
              std::equal(user_data.begin() + 4, user_data.end(),
                         snapshot.buffers[1].dwords.begin()),
          "shared, loop-dependent, or writable-alias resource was pruned");
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
  fixture->program.block_info[1].condition = fixture->Emit(
      ValueOpcode::INotEqual32, {flag, Value(0u)}, 0, entry);
  fixture->program.block_info[1].terminator = {
      .kind = CFG::TerminatorKind::ConditionalBranch,
      .true_block = 0, .false_block = 2};
  fixture->program.block_info[0].terminator = {
      .kind = CFG::TerminatorKind::Branch, .true_block = 2};
  std::swap(fixture->program.blocks[0], fixture->program.blocks[1]);
  std::swap(fixture->program.block_info[0], fixture->program.block_info[1]);
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

// Insert in tests/ResourceTrackingTests.cpp's existing test namespace and call
// TestComparisonBindingsAreIsolated() from its main. Existing APIs only: this
// compiles before the fix and must fail because comparison shares the ordinary
// sampled-image group. No Vulkan/device/guest-memory access.
void TestComparisonBindingsAreIsolated() {
  ImageResource ordinary{};
  ordinary.resource_class = ImageResourceClass::Sampled;
  ordinary.numeric_class = Libs::Graphics::Prospero::TextureNumericClass::Float;
  ordinary.dimension = Decoder::ImageDimension::Dim2D;
  ordinary.read = true;
  ImageResource comparison = ordinary;
  comparison.depth_compare = true;
  const auto ordinary_kind = DescriptorBindingForImage(ordinary);
  const auto comparison_kind = DescriptorBindingForImage(comparison);
  Check(ordinary_kind.has_value() && comparison_kind.has_value(),
        "valid ordinary/comparison images lack descriptor classes");
  Check(*ordinary_kind != *comparison_kind,
        "ordinary and comparison images share one descriptor binding class");

  // Both discovery orders and two ordinary resources: isolate comparison
  // without splitting every ordinary image into its own binding.
  for (const auto stage : {ShaderType::Compute, ShaderType::Vertex, ShaderType::Pixel}) {
    for (const bool comparison_first : {false, true}) {
      Program program{};
      program.stage = stage;
      program.shader_info_complete = true;
      program.resource_tracking_complete = true;
      program.info.images = comparison_first
          ? std::vector<ImageResource>{comparison, ordinary, ordinary}
          : std::vector<ImageResource>{ordinary, comparison, ordinary};
      for (uint32_t index = 0; index < program.info.images.size(); ++index) {
        program.info.images[index].source = index;
      }
      SamplerResource sampler{};
      sampler.source = 3;
      sampler.depth_compare = true;
      program.info.samplers.push_back(sampler);
      const uint32_t comparison_index = comparison_first ? 0u : 1u;
      program.info.sampled_pairs.push_back({comparison_index, 0, 0});
      AllocateBindings(program);
      const auto* ordinary_binding = FindBinding(program.bindings, *ordinary_kind);
      const auto* comparison_binding = FindBinding(program.bindings, *comparison_kind);
      const auto* samplers = FindBinding(program.bindings, DescriptorBindingKind::Samplers);
      const std::vector<uint32_t> ordinary_indices = comparison_first
          ? std::vector<uint32_t>{1, 2} : std::vector<uint32_t>{0, 2};
      Check(ordinary_binding != nullptr && comparison_binding != nullptr &&
                ordinary_binding != comparison_binding &&
                ordinary_binding->resources == ordinary_indices &&
                comparison_binding->resources == std::vector<uint32_t>{comparison_index},
            "mixed image binding arrays did not preserve isolated resource membership");
      Check(NativeBinding(stage, *ordinary_kind) != NativeBinding(stage, *comparison_kind),
            "ordinary and comparison image bindings collide in the native stage");
      Check(samplers != nullptr && samplers->resources == std::vector<uint32_t>{0},
            "image binding isolation changed sampler identity or multiplicity");
    }
  }
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
            static_cast<uint32_t>(DescriptorBindingKind::Count) == 55u,
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
  const std::array storage_classes{NumericClass::Float, NumericClass::Uint,
                                   NumericClass::Sint};
  uint32_t index = 0;
  const auto CheckBinding =
      [&](ImageResourceClass resource_class, NumericClass numeric_class,
          Decoder::ImageDimension dimension, bool atomic, bool comparison = false) {
        ImageResource image;
        image.resource_class = resource_class;
        image.numeric_class = numeric_class;
        image.dimension = dimension;
        image.atomic = atomic;
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
  image.depth_compare = true;
  image.numeric_class = NumericClass::Uint;
  Check(Invalid(image), "integer comparison image received a descriptor binding");
  image.numeric_class = NumericClass::Float;
  image.resource_class = ImageResourceClass::Storage;
  Check(Invalid(image), "storage comparison image received a descriptor binding");
  image.resource_class = ImageResourceClass::Sampled;
  image.depth_compare = false;
  image.atomic = true;
  Check(Invalid(image), "atomic sampled image received a descriptor binding");
  image.resource_class = ImageResourceClass::Storage;
  image.atomic = false;
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
  // Upstream MaxBuffers=64 contract: exact capacity must survive CollectShaderInfo
  // and AllocateBindings without truncating the dense buffer table.
  {
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
    Check(accepted.program.info.buffers.size() == ShaderInfo::MaxBuffers &&
              accepted.program.descriptor_sources.size() == ShaderInfo::MaxBuffers &&
              accepted.program.memory_info.back().resource == ShaderInfo::MaxBuffers - 1u,
          "compute shader did not retain all MaxBuffers distinct buffers");
    ShaderComputeInputInfo compute{};
    CollectShaderInfo(accepted.program, {.compute = &compute});
    AllocateBindings(accepted.program);
    const auto *binding = FindBinding(accepted.program.bindings,
                                      DescriptorBindingKind::Buffers);
    Check(binding != nullptr && binding->resources.size() == ShaderInfo::MaxBuffers &&
              accepted.program.bindings.memory_offset_count == ShaderInfo::MaxBuffers,
          "compute shader binding layout truncated MaxBuffers buffers");
  }

  enum class Limit { Buffers, Images, Samplers, Pairs };
  struct Case {
    Limit kind;
    uint32_t count;
    const char *error;
  };
  for (const auto test : {
           Case{Limit::Buffers, ShaderInfo::MaxBuffers, "buffer resource limit exceeded"},
           Case{Limit::Images, ShaderInfo::MaxImages, "image resource limit exceeded"},
           Case{Limit::Samplers, ShaderInfo::MaxSamplers, "sampler resource limit exceeded"},
           Case{Limit::Pairs, ShaderInfo::MaxSampledPairs,
                "sampled image/sampler pair limit exceeded"}}) {
    for (const uint32_t excess : {0u, 1u}) {
      Fixture fixture;
      for (uint32_t index = 0; index < test.count + excess; index++) {
        MemoryInfo memory;
        if (test.kind == Limit::Buffers) {
          memory.kind = ResourceKind::Buffer;
          const auto handle = fixture.Buffer(
              {Value(index), Value(index + 1u), Value(index + 2u), Value(index + 3u)},
              index * 4u);
          fixture.Emit(ValueOpcode::LoadBufferU32,
                       {handle, Value(0u), Value(0u), Value(0u), Value(true)},
                       fixture.AddMemory(memory, index * 4u));
          continue;
        }
        const auto image_index = test.kind == Limit::Images ? index
                                 : test.kind == Limit::Pairs ? index / ShaderInfo::MaxSamplers
                                                           : 0u;
        const auto sampler_index = test.kind == Limit::Samplers ? index
                                   : test.kind == Limit::Pairs ? index % ShaderInfo::MaxSamplers
                                                             : 0u;
        const auto image = fixture.Image(
            {Value(image_index + 1u), Value(0u), Value(0u), Value(0u),
             Value(0u), Value(0u), Value(0u), Value(0u)}, index * 4u);
        const auto sampler = fixture.Sampler(
            {Value(sampler_index + 1u), Value(0u), Value(0u), Value(0u)}, index * 4u);
        memory.kind = ResourceKind::Image;
        memory.image_dimension = Decoder::ImageDimension::Dim2D;
        fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress()},
                     fixture.AddMemory(memory, index * 4u));
      }
      BuildSrtPlan(fixture.program);
      if (excess == 0u) {
        TrackResources(fixture.program);
        const auto actual = test.kind == Limit::Buffers ? fixture.program.info.buffers.size()
                            : test.kind == Limit::Images ? fixture.program.info.images.size()
                            : test.kind == Limit::Samplers ? fixture.program.info.samplers.size()
                                                          : fixture.program.info.sampled_pairs.size();
        Check(fixture.program.resource_tracking_complete && actual == test.count,
              "resource tracking rejected or truncated its exact configured capacity");
      } else {
        CheckFatal([&] { TrackResources(fixture.program); }, test.error,
                   "resource capacity plus one did not report its specific limit");
        Check(!fixture.program.resource_tracking_complete &&
                  fixture.program.info.buffers.empty() && fixture.program.info.images.empty() &&
                  fixture.program.info.samplers.empty() && fixture.program.info.sampled_pairs.empty() &&
                  fixture.program.descriptor_sources.empty(),
              "resource-limit failure partially mutated typed resource state");
      }
    }
  }
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

void TestHeterogeneousIndirectImageDimensions() {
  auto fixture = MakeIndirectImageFixture(false);
  fixture->PlanAndTrack();
  auto resource_plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);

  std::array<uint32_t, 9> user_data{0x1000u,    224u << 16u, 2u, 0u, 0x2000u,
                                    16u << 16u, 4u,          0u, 7u};
  LinearTestMemory memory;
  std::array<uint32_t, 8> descriptor{};
  descriptor[0] = 0x20u;
  descriptor[1] = static_cast<uint32_t>(
                      Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
                  << 20u;
  descriptor[2] = 3u | (3u << 14u);
  descriptor[3] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
       << 28u);
  for (uint32_t dword = 0; dword < descriptor.size(); dword++) {
    memory.words[(0x2000u - memory.base) / 4u + dword] = descriptor[dword];
    memory.words[(0x2020u - memory.base) / 4u + dword] = descriptor[dword];
  }
  memory.words[(0x2020u - memory.base) / 4u] = 0x40u;
  memory.words[(0x2020u - memory.base) / 4u + 3u] =
      Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor1D)
       << 28u);
  memory.words[(0x1000u - memory.base + 36u) / 4u] = 1u;

  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 2 && specialization.images.size() == 2 &&
            specialization.images[0].dimension == Decoder::ImageDimension::Dim2D &&
            specialization.images[1].dimension == Decoder::ImageDimension::Dim1D,
        "mixed 2D/1D indirect image table was rejected");
  memory.words[(0x2020u - memory.base) / 4u + 1u] =
      static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32UInt)
      << 20u;
  Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 2 && specialization.images.size() == 2 &&
            specialization.images[0].numeric_class ==
                Libs::Graphics::Prospero::TextureNumericClass::Float &&
            specialization.images[1].numeric_class ==
                Libs::Graphics::Prospero::TextureNumericClass::Uint,
        "mixed Float/Uint sampled indirect image table was rejected");
  const auto heterogeneous_snapshot = snapshot;
  const auto heterogeneous_specialization = specialization;
  resource_plan.info.images[0].heterogeneous_numeric_compatible = false;
  Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
            SameResourceSnapshot(snapshot, heterogeneous_snapshot) &&
            specialization == heterogeneous_specialization,
        "mixed numeric classes bypassed the tracked opcode compatibility proof");
  resource_plan.info.images[0].heterogeneous_numeric_compatible = true;
  auto gather_fixture = MakeIndirectImageFixture(false, 0u, false, false,
                                                 ValueOpcode::ImageGatherRaw);
  gather_fixture->PlanAndTrack();
  Check(!gather_fixture->program.info.images[0].heterogeneous_numeric_compatible,
        "indirect gather was marked compatible with mixed numeric classes");
  ApplyResourceSpecialization(fixture->program, specialization);
  Check(fixture->program.info.images.size() == 2 &&
            fixture->program.info.images[0].indirect_root == 0u &&
            fixture->program.info.images[0].indirect_resources.size() == 2u,
        "mixed 2D/1D indirect image topology was not applied");
}

void TestHeterogeneousIndirectImageViewSwizzles() {
  for (const bool storage_write : {false, true}) {
    auto fixture = MakeIndirectImageFixture(false, 0u, false, storage_write);
    fixture->PlanAndTrack();

    constexpr auto root_swizzle = Libs::Graphics::DstSel(4, 1, 1, 1);
    fixture->program.info.images[0].shader_swizzle = root_swizzle;
    fixture->program.info.images[0].indirect_root = 0u;

    std::array<uint32_t, 8> direct_descriptor{};
    direct_descriptor[0] = 0x40u;
    direct_descriptor[1] = static_cast<uint32_t>(
                               Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
                           << 20u;
    direct_descriptor[2] = 3u | (3u << 14u);
    direct_descriptor[3] =
        Libs::Graphics::DstSel(0, 0, 0, 0) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor1D)
         << 28u);
    DescriptorSource direct_source;
    direct_source.dword_count = 8u;
    for (uint32_t dword = 0; dword < direct_descriptor.size(); dword++) {
      direct_source.dwords[dword] = Value(direct_descriptor[dword]);
    }
    const auto direct_source_index =
        static_cast<uint32_t>(fixture->program.descriptor_sources.size());
    fixture->program.descriptor_sources.push_back(std::move(direct_source));

    auto candidate = fixture->program.info.images[0];
    candidate.source = direct_source_index;
    candidate.dimension = Decoder::ImageDimension::Dim1D;
    candidate.shader_swizzle = Libs::Graphics::DstSel(0, 0, 0, 0);
    candidate.indirect_resources.clear();
    fixture->program.info.images.push_back(candidate);

    auto resource_plan = ExtractResourcePlan(fixture->program);
    std::array<uint32_t, 9> user_data{0x1000u,    224u << 16u, 2u, 0u, 0x2000u,
                                      16u << 16u, 4u,          0u, 7u};
    LinearTestMemory memory;
    auto table_descriptor = direct_descriptor;
    table_descriptor[3] =
        root_swizzle |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D)
         << 28u);
    for (uint32_t dword = 0; dword < table_descriptor.size(); dword++) {
      memory.words[(0x2000u - memory.base) / 4u + dword] = table_descriptor[dword];
      memory.words[(0x2020u - memory.base) / 4u + dword] = direct_descriptor[dword];
    }
    memory.words[(0x1000u - memory.base + 36u) / 4u] = 1u;

    SrtRuntime runtime{.user_data = user_data,
                       .userdata = &memory,
                       .read_specialization_memory = ReadLinearTestMemory};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
              specialization.images.size() >= 2u &&
              specialization.images[0].dimension == Decoder::ImageDimension::Dim2D &&
              specialization.images[0].shader_swizzle == root_swizzle &&
              specialization.images[1].dimension == Decoder::ImageDimension::Dim1D &&
              specialization.images[1].shader_swizzle == Libs::Graphics::DstSel(0, 0, 0, 0),
          storage_write ? "mixed storage image dimensions/swizzles were rejected"
                        : "mixed sampled image-view swizzles were rejected");
    if (storage_write) {
      ApplyResourceSpecialization(fixture->program, specialization);
      resource_plan.info.images[0].atomic = true;
      resource_plan.info.images[1].atomic = true;
      const auto atomic_format =
          static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32Float) << 20u;
      resource_plan.descriptor_sources[direct_source_index].dwords[1] = Value(atomic_format);
      memory.words[(0x2000u - memory.base) / 4u + 1u] = atomic_format;
      memory.words[(0x2020u - memory.base) / 4u + 1u] = atomic_format;
      const auto prior_snapshot = snapshot;
      const auto prior_specialization = specialization;
      Check(!MaterializeResources(resource_plan, runtime, snapshot, specialization) &&
                SameResourceSnapshot(snapshot, prior_snapshot) &&
                specialization == prior_specialization,
            "heterogeneous atomic storage images were accepted or failed nontransactionally");
    }
  }
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

std::unique_ptr<Fixture> MakeInlineBufferDescriptorFixture(
    bool guarded_selector = true, bool correlated_columns = true) {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  auto fixture = std::make_unique<Fixture>();
  const auto table = fixture->Buffer(
      {fixture->UserData(0u), fixture->UserData(1u), fixture->UserData(2u),
       fixture->UserData(3u)},
      0x6200u);
  auto *entry = fixture->block;
  auto *header = fixture->AddBlock();
  auto *body = fixture->AddBlock();
  auto *exit = fixture->AddBlock();
  entry->AddBranch(header);
  fixture->program.block_info[0].terminator.kind = CFG::TerminatorKind::Branch;
  fixture->program.block_info[0].terminator.true_block = 1u;
  fixture->block = header;
  auto &counter = header->AppendNewInst(ValueOpcode::Phi, {},
                                        static_cast<uint64_t>(Type::U32));
  const auto selector = fixture->Emit(ValueOpcode::ReadFirstLane,
                                      {Value(&counter), Value(true)});
  const auto guarded_value =
      guarded_selector ? selector : fixture->UserData(5u);
  const auto bounded = fixture->Emit(ValueOpcode::ULessThan32,
                                     {guarded_value, Value(3u)});
  header->AddBranch(body);
  header->AddBranch(exit);
  fixture->program.block_info[1].terminator.kind =
      CFG::TerminatorKind::ConditionalBranch;
  fixture->program.block_info[1].terminator.true_block = 2u;
  fixture->program.block_info[1].terminator.false_block = 3u;
  fixture->program.block_info[1].condition = bounded;

  fixture->block = body;
  const auto next =
      fixture->Emit(ValueOpcode::IAdd32, {Value(&counter), Value(1u)});
  counter.AddPhiOperand(entry, Value(0u));
  counter.AddPhiOperand(body, next);
  const auto offset =
      fixture->Emit(ValueOpcode::IMul32, {selector, Value(16u)});
  std::array<Value, 4> descriptor;
  for (uint32_t word = 0; word < descriptor.size(); ++word) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarBuffer;
    memory.offset = word * sizeof(uint32_t);
    if (!correlated_columns && word == 3u) memory.offset += sizeof(uint32_t);
    memory.component_count = 4u;
    memory.component_index = word;
    descriptor[word] = fixture->Emit(
        ValueOpcode::ReadConstBuffer, {table, offset},
        fixture->AddMemory(memory, 0x62e8u));
  }
  const auto selected = fixture->Buffer(descriptor, 0x656cu);
  MemoryInfo load;
  load.kind = ResourceKind::Buffer;
  const auto value = fixture->Emit(
      ValueOpcode::LoadBufferU16,
      {selected, Value(0u), Value(0u), Value(0u), Value(true)},
      fixture->AddMemory(load, 0x656cu));
  fixture->Emit(ValueOpcode::ReferenceU32, {value});
  body->AddBranch(header);
  fixture->program.block_info[2].terminator.kind = CFG::TerminatorKind::Branch;
  fixture->program.block_info[2].terminator.true_block = 1u;
  fixture->program.block_info[3].terminator.kind = CFG::TerminatorKind::Return;
  return fixture;
}

void TestInlineBufferDescriptorTable() {
  for (const auto [guarded, correlated] :
       {std::pair{false, true}, std::pair{true, false}}) {
    auto rejected = MakeInlineBufferDescriptorFixture(guarded, correlated);
    CheckFatal([&] { rejected->PlanAndTrack(); }, "not a valid runtime value",
               "unbounded or uncorrelated inline buffer table was accepted");
    Check(!rejected->program.resource_tracking_complete &&
              rejected->program.info.buffers.empty(),
          "rejected inline buffer table mutated the committed resource plan");
  }
  auto fixture = MakeInlineBufferDescriptorFixture();
  fixture->PlanAndTrack();
  Check(fixture->program.info.buffers.size() == 1u,
        "inline buffer table did not retain one logical resource");
  const auto &memory = fixture->program.memory_info.back();
  Check(memory.buffer_table == 0u,
        "inline buffer access was not routed through its logical table");

  auto plan = ExtractResourcePlan(fixture->program);
  LinearTestMemory table;
  table.words.resize(12u);
  table.words = {0x2000u, 0u, 16u, 0u, 0x3000u, 0u,
                 32u,     0u, 0x4000u, 0u, 48u,     0u};
  std::array<uint32_t, 5> user_data = {
      static_cast<uint32_t>(table.base), 0u,
      static_cast<uint32_t>(table.words.size() * sizeof(uint32_t)), 0u, 1u};
  const SrtRuntime runtime{user_data, 0u, ReadLinearTestMemory, &table,
                           ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization),
        "inline buffer table could not be materialized");
  Check(specialization.buffer_tables.size() == 1u &&
            specialization.buffer_tables[0].count == 3u &&
            snapshot.buffers.size() == 3u &&
            snapshot.flattened_srt.size() == 3u,
        "inline buffer table lost its guarded three-candidate domain");
  for (uint32_t candidate = 0; candidate < 3u; ++candidate) {
    Check(snapshot.buffers[candidate].dword_count == 4u &&
              snapshot.buffers[candidate].dwords[0] ==
                  0x2000u + candidate * 0x1000u,
          "inline buffer candidate descriptor was read from the wrong record");
  }
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

void TestInlineImageMixedDynamicAndOrdinarySamplers() {
  for (const bool reverse_pairs : {false, true}) {
    auto fixture = MakeInlineDescriptorFixture();
    const auto image = std::ranges::find_if(*fixture->block, [](const Inst &inst) {
      return inst.GetOpcode() == ValueOpcode::GetImageResource;
    });
    Check(image != fixture->block->end(), "mixed sampler fixture lost its image");
    const auto ordinary = fixture->Sampler(
        {Value(146u), Value(0x00fff000u), Value(0x05000000u), Value(0u)}, 0x5d0u);
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    sample.image_r128 = true;
    const auto sampled = fixture->Emit(
        ValueOpcode::ImageSampleRaw,
        {Value(&*image), ordinary, fixture->ImageAddress()},
        fixture->AddMemory(sample, 0x5d0u));
    fixture->Emit(ValueOpcode::ReferenceU32,
                  {fixture->Emit(ValueOpcode::CompositeExtractU32x4,
                                 {sampled, Value(0u)})});
    fixture->PlanAndTrack();
    auto plan = ExtractResourcePlan(fixture->program);
    Check(plan.info.images.size() == 1u && plan.info.samplers.size() == 2u &&
              plan.info.sampled_pairs.size() == 2u,
          "mixed sampler fixture did not retain both pairings");
    if (reverse_pairs) {
      std::swap(plan.info.sampled_pairs[0], plan.info.sampled_pairs[1]);
    }

    const std::array<uint32_t, 8> user_data{
        0x1000u, 872u << 16u, 2u, 0u, 0x2000u, 0u, 16u, 0u};
    LinearTestMemory memory;
    DescriptorValue image_descriptor;
    image_descriptor.dword_count = 8u;
    image_descriptor.dwords[0] = 0x20u;
    image_descriptor.dwords[1] =
        static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float)
        << 20u;
    image_descriptor.dwords[2] = 3u | (3u << 14u);
    image_descriptor.dwords[3] =
        Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    for (uint32_t record = 0u; record < 2u; record++) {
      for (uint32_t word = 0u; word < 4u; word++) {
        memory.words[(record * 872u + 136u) / 4u + word] =
            word == 0u ? record + 1u : 0u;
        memory.words[(record * 872u + 152u) / 4u + word] =
            image_descriptor.dwords[word];
      }
    }
    SrtRuntime runtime{.user_data = user_data,
                       .userdata = &memory,
                       .read_specialization_memory = ReadLinearTestMemory};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "mixed dynamic and ordinary sampler uses were rejected");
    Check(snapshot.samplers.size() >= 3u &&
              specialization.sampled_pairs.size() ==
                  specialization.images.size() * 2u,
          "mixed sampler specialization dropped an image/sampler pairing");
    for (uint32_t image_index = 0u; image_index < specialization.images.size(); image_index++) {
      const auto dynamic_sampler = specialization.images[image_index].indirect_sampler;
      Check(dynamic_sampler < snapshot.samplers.size() &&
                snapshot.samplers[dynamic_sampler].dwords[0] <= 2u,
            "mixed sampler image lost its dynamic sampler candidate");
      for (const uint32_t sampler_index : {1u, dynamic_sampler}) {
        Check(std::ranges::any_of(
                  specialization.sampled_pairs,
                  [&](const SampledResourcePair &pair) {
                    return pair.image == image_index && pair.sampler == sampler_index;
                  }),
              "mixed sampler image selected the wrong sampler for a sampled use");
      }
    }
  }
}

void TestInlineImageResourceLimits() {
  auto fixture = MakeInlineDescriptorFixture(true);
  fixture->PlanAndTrack();
  const auto plan = ExtractResourcePlan(fixture->program);
  constexpr uint32_t stride = 872u;
  constexpr uint32_t descriptor_offset = 588u;
  // At most 112 KiB with the 128-image budget; the probe boundary below needs
  // no backing allocation because its reader rejects the first payload read.
  LinearTestMemory memory;
  memory.words.resize(ShaderInfo::MaxImages * stride / sizeof(uint32_t));
  for (uint32_t record = 0; record < ShaderInfo::MaxImages; record++) {
    const auto start = (record * stride + descriptor_offset) / sizeof(uint32_t);
    memory.words[start] = record + 1u;
    memory.words[start + 1u] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[start + 2u] = 3u | (3u << 14u);
    memory.words[start + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }
  std::array<uint32_t, 8> user_data{0x1000u, stride << 16u,
                                   ShaderInfo::MaxImages - 1u, 0u,
                                   0x2000u, 0u, 16u, 0u};
  SrtRuntime runtime{.user_data = user_data,
                     .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == ShaderInfo::MaxImages &&
            specialization.sampled_pairs.size() == ShaderInfo::MaxSampledPairs &&
            specialization.sampler_origins == std::vector<uint32_t>{0u, 1u} &&
            snapshot.samplers.size() == 2u,
        "inline candidates and ordinary sampler pairs did not fill their configured capacities");
  const auto last = InlineCandidateForKey(
      snapshot, specialization, (ShaderInfo::MaxImages - 2u) * stride);
  Check(snapshot.images[last].dwords[0] == ShaderInfo::MaxImages - 1u,
        "inline candidate capacity silently truncated the last valid descriptor");
  const auto prior_snapshot = snapshot;
  const auto prior_specialization = specialization;
  user_data[2] = ShaderInfo::MaxImages;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
            SameResourceSnapshot(snapshot, prior_snapshot) &&
            specialization == prior_specialization,
        "inline candidate capacity plus one was accepted or partially committed");

  uint32_t attempts = 0;
  runtime.userdata = &attempts;
  runtime.read_specialization_memory = [](void *userdata, uint64_t, std::span<uint32_t>) {
    ++*static_cast<uint32_t *>(userdata);
    return false;
  };
  constexpr uint32_t max_probes = 65'536u;
  user_data[1] = 0u;
  user_data[2] = descriptor_offset + (max_probes - 1u) * 8u + 4u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) && attempts == 1u &&
            SameResourceSnapshot(snapshot, prior_snapshot) && specialization == prior_specialization,
        "exactly 65536 inline probes were rejected before reaching the payload reader");
  attempts = 0u;
  user_data[2] += 8u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) && attempts == 0u &&
            SameResourceSnapshot(snapshot, prior_snapshot) && specialization == prior_specialization,
        "65537 inline probes performed memory reads or mutated prior resources");
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
              std::string_view(LastResourceSpecializationError()).find(
                  "inline sampled pair at pc") != std::string_view::npos &&
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
              std::string_view(LastResourceSpecializationError()).find(
                  "inline sampled pair at pc") != std::string_view::npos &&
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

std::unique_ptr<Fixture> MakeWaveUniformBufferPhiFixture(bool wave_uniform) {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  auto fixture = std::make_unique<Fixture>(ShaderType::Compute);
  auto *split = fixture->block;
  auto *left = fixture->AddBlock();
  auto *right = fixture->AddBlock();
  auto *merge = fixture->AddBlock();
  split->AddBranch(left);
  split->AddBranch(right);
  left->AddBranch(merge);
  right->AddBranch(merge);
  fixture->program.block_info[0].terminator.kind =
      CFG::TerminatorKind::ConditionalBranch;
  fixture->program.block_info[0].terminator.true_block = 1u;
  fixture->program.block_info[0].terminator.false_block = 2u;
  fixture->program.block_info[1].terminator.kind = CFG::TerminatorKind::Branch;
  fixture->program.block_info[1].terminator.true_block = 3u;
  fixture->program.block_info[2].terminator.kind = CFG::TerminatorKind::Branch;
  fixture->program.block_info[2].terminator.true_block = 3u;
  fixture->program.block_info[3].terminator.kind = CFG::TerminatorKind::Return;

  const auto lane = fixture->Emit(ValueOpcode::LaneId, {}, 0, split);
  const auto even = fixture->Emit(
      ValueOpcode::IEqual32,
      {fixture->Emit(ValueOpcode::BitwiseAnd32, {lane, Value(1u)}, 0, split),
       Value(0u)},
      0, split);
  Value condition = even;
  if (wave_uniform) {
    const auto ballot = fixture->Emit(ValueOpcode::Ballot, {even}, 0, split);
    const auto low = fixture->Emit(ValueOpcode::CompositeExtractU32x4,
                                   {ballot, Value(0u)}, 0, split);
    const auto high = fixture->Emit(ValueOpcode::CompositeExtractU32x4,
                                    {ballot, Value(1u)}, 0, split);
    condition = fixture->Emit(
        ValueOpcode::IEqual32,
        {fixture->Emit(ValueOpcode::BitwiseOr32, {low, high}, 0, split),
         Value(0u)},
        0, split);
  }
  fixture->program.block_info[0].condition = condition;

  const auto address = fixture->Address(fixture->UserData(0u),
                                        fixture->UserData(1u), 0x8d54u);
  std::array<Value, 4> left_words;
  std::array<Value, 4> right_words;
  for (uint32_t word = 0; word < 4u; ++word) {
    MemoryInfo left_memory;
    left_memory.kind = ResourceKind::ScalarAddress;
    left_memory.offset = word * sizeof(uint32_t);
    left_words[word] = fixture->Emit(
        ValueOpcode::LoadAddressU32,
        {address, Value(0u), Value(0u), Value(true)},
        fixture->AddMemory(left_memory, 0x8d54u), left);
    auto right_memory = left_memory;
    right_memory.offset += 4u * sizeof(uint32_t);
    right_words[word] = fixture->Emit(
        ValueOpcode::LoadAddressU32,
        {address, Value(0u), Value(0u), Value(true)},
        fixture->AddMemory(right_memory, 0x8d54u), right);
  }
  std::array<Value, 4> selected;
  for (uint32_t word = 0; word < 4u; ++word) {
    auto &phi = merge->AppendNewInst(ValueOpcode::Phi, {},
                                     static_cast<uint64_t>(Type::U32));
    phi.AddPhiOperand(left, left_words[word]);
    phi.AddPhiOperand(right, right_words[word]);
    selected[word] = Value(&phi);
  }
  const auto handle = fixture->Emit(ValueOpcode::GetBufferResource,
                                    {selected[0], selected[1], selected[2], selected[3]},
                                    MemoryFlags{0, 0x8d54u}, merge);
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  const auto loaded = fixture->Emit(
      ValueOpcode::LoadBufferU32,
      {handle, Value(0u), Value(0u), Value(0u), Value(true)},
      fixture->AddMemory(memory, 0x8d54u), merge);
  fixture->Emit(ValueOpcode::ReferenceU32, {loaded}, 0, merge);
  return fixture;
}

void TestWaveUniformBufferPhiTable() {
  auto fixture = MakeWaveUniformBufferPhiFixture(true);
  fixture->PlanAndTrack();
  Check(fixture->program.resource_tracking_complete &&
            fixture->program.info.buffers.size() == 1u &&
            fixture->program.memory_info.back().buffer_table == 0u,
        "wave-uniform descriptor Phi did not become one buffer table");
  const auto &source = fixture->program.descriptor_sources[
      fixture->program.info.buffers[0].source];
  Check(source.bounded_buffer.has_value(),
        "wave-uniform descriptor Phi lost its bounded buffer source");
  const auto handle = std::ranges::find_if(
      *fixture->program.blocks.back(), [](const Inst &inst) {
        return inst.GetOpcode() == ValueOpcode::GetBufferResource;
      });
  Check(handle != fixture->program.blocks.back()->end() &&
            handle->Arg(0).ResolveInstruction() != nullptr &&
            handle->Arg(0).ResolveInstruction()->GetOpcode() ==
                ValueOpcode::SelectU32,
        "wave-uniform descriptor Phi did not retain its live GPU selector");

  TestMemory memory;
  memory.words = {0x2000u, 4u << 16u, 8u, 0u,
                  0x3000u, 8u << 16u, 4u, 1u};
  std::array<uint32_t, 2> user_data{static_cast<uint32_t>(memory.base), 0u};
  const SrtRuntime runtime{.user_data = user_data,
                           .read_memory = RejectTestMemory,
                           .userdata = &memory,
                           .read_specialization_memory = ReadTestMemory};
  auto plan = ExtractResourcePlan(fixture->program);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.buffers.size() == 2u &&
            specialization.buffer_tables.size() == 1u &&
            specialization.buffer_tables[0].count == 2u,
        "wave-uniform descriptor Phi candidates did not materialize");
  const auto &table = specialization.buffer_tables[0];
  Check(table.mapping_flat_offset + table.count <= snapshot.flattened_srt.size(),
        "wave-uniform buffer mapping exceeds the flat snapshot");
  for (uint32_t candidate = 0; candidate < 2u; ++candidate) {
    const auto dense = snapshot.flattened_srt[table.mapping_flat_offset + candidate];
    Check(dense < snapshot.buffers.size() &&
              snapshot.buffers[dense].dwords[0] == 0x2000u + candidate * 0x1000u,
          "wave-uniform buffer table mixed descriptor candidates");
  }

  auto divergent = MakeWaveUniformBufferPhiFixture(false);
  BuildSrtPlan(divergent->program);
  CheckFatal([&] { TrackResources(divergent->program); },
             "not a valid runtime value",
             "lane-divergent descriptor Phi was accepted as a uniform table");
}


// TEST ONLY: ResourceTrackingTests.cpp, after Fixture and before main.
// Register the four TestBoundedMaterialization* functions below with Run().
struct BoundedSnapshotReader {
  std::vector<std::pair<uint64_t, uint32_t>> words;
  std::vector<uint64_t> reads;
  uint64_t fail_address = UINT64_MAX;
  uint32_t ordinary_reads = 0;
  uint32_t generated_descriptors = 0;
  uint64_t unmapped_address = UINT64_MAX;
  uint64_t clamped_address = UINT64_MAX;
  uint64_t clamped_size = UINT64_MAX;
  bool change_repeated_reads = false;

  static bool Clean(void* userdata, uint64_t address, std::span<uint32_t> words) {
    auto& self = *static_cast<BoundedSnapshotReader*>(userdata);
    self.reads.push_back(address);
    if (words.size() != 1u || address == self.fail_address) return false;
    auto& word = words.front();
    for (const auto& entry : self.words) {
      if (entry.first == address) {
        word = entry.second;
        if (self.change_repeated_reads) {
          for (size_t i = 0; i + 1u < self.reads.size(); ++i)
            if (self.reads[i] == address) ++word;
        }
        return true;
      }
    }
    if (address >= 0x1000u && address - 0x1000u < uint64_t{self.generated_descriptors} * 16u &&
        (address & 3u) == 0u) {
      const auto index = static_cast<uint32_t>((address - 0x1000u) / 16u);
      const auto component = static_cast<uint32_t>((address - 0x1000u) % 16u / 4u);
      const std::array<uint32_t, 4> descriptor{0x20000u + index * 256u, 4u << 16u, 4u, 0u};
      word = descriptor[component];
      return true;
    }
    return false;
  }
  static bool Ordinary(void* userdata, uint64_t, std::span<uint32_t>) {
    ++static_cast<BoundedSnapshotReader*>(userdata)->ordinary_reads;
    return false;
  }
  static uint64_t Clamp(void* userdata, uint64_t address, uint64_t size) {
    const auto& self = *static_cast<BoundedSnapshotReader*>(userdata);
    if (address == self.unmapped_address) return 0u;
    return address == self.clamped_address ? std::min(size, self.clamped_size) : size;
  }
};

uint32_t AddBoundedSnapshotSource(Fixture& fixture, std::initializer_list<Value> words) {
  DescriptorSource source;
  source.dword_count = static_cast<uint32_t>(words.size());
  uint32_t index = 0;
  for (const auto word : words) source.dwords[index++] = word;
  fixture.program.descriptor_sources.push_back(source);
  return static_cast<uint32_t>(fixture.program.descriptor_sources.size() - 1u);
}

void InitializeBoundedSnapshot(Fixture& fixture, uint32_t columns, bool buffer_table) {
  auto& program = fixture.program;
  program.srt_plan_complete = program.resource_tracking_complete = true;
  AddBoundedSnapshotSource(fixture, {fixture.UserData(0u)});
  AddBoundedSnapshotSource(fixture, {fixture.UserData(1u), fixture.UserData(2u)});
  for (uint32_t column = 0; column < columns; ++column)
    program.bounded_srt_reads.push_back({.address_source=1u, .count_source=0u,
                                         .offset_scale=columns * 4u,
                                         .offset_bias=buffer_table ? 0u : column * 4u,
                                         .memory_offset=buffer_table ? column * 4u : 0u});
  if (buffer_table) {
    Check(columns == 4u, "test descriptor table must have four columns");
    const auto source = AddBoundedSnapshotSource(fixture, {Value(0u),Value(0u),Value(0u),Value(0u)});
    program.descriptor_sources[source].bounded_buffer.emplace();
    program.descriptor_sources[source].bounded_buffer->reads = {0u, 1u, 2u, 3u};
    program.info.buffers.push_back({.source=source});
  }
}

SrtRuntime BoundedSnapshotRuntime(BoundedSnapshotReader& reader, std::span<const uint32_t> data) {
  return {.user_data=data, .read_memory=BoundedSnapshotReader::Ordinary, .userdata=&reader,
          .read_specialization_memory=BoundedSnapshotReader::Clean,
          .clamp_memory_range=BoundedSnapshotReader::Clamp};
}

void CheckBoundedTransaction(const ResourceSnapshot& snapshot, const ResourceSnapshot& old_snapshot,
                             const ResourceSpecialization& specialization,
                             const ResourceSpecialization& old_specialization) {
  Check(SameResourceSnapshot(snapshot, old_snapshot) &&
            snapshot.immutable_srt_ranges == old_snapshot.immutable_srt_ranges &&
            specialization == old_specialization,
        "failed bounded materialization changed snapshot, footprints or specialization");
}

// The frontend can place Phi nodes in an empty loop header and the scalar
// comparison in its unconditional successor. The bound may itself be a shared
// scalar load with a runtime-uniform byte offset, not an immediate offset.
void CheckBoundedSrtSplitHeader(bool memory_bound) {
  Fixture fixture;
  auto* entry = fixture.block;
  auto* header = fixture.AddBlock();
  auto* guard = fixture.AddBlock();
  auto* body = fixture.AddBlock();
  auto* latch = fixture.AddBlock();
  auto* exit = fixture.AddBlock();
  const auto branch = [&](uint32_t from, uint32_t to) {
    fixture.program.blocks[from]->AddBranch(fixture.program.blocks[to]);
    auto& term = fixture.program.block_info[from].terminator;
    term.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Branch;
    term.true_block = to;
  };
  branch(0u, 1u);
  branch(1u, 2u);
  branch(3u, 4u);
  branch(4u, 1u);
  guard->AddBranch(exit);
  guard->AddBranch(body);
  auto& term = fixture.program.block_info[2].terminator;
  term.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
  term.true_block = 5u;
  term.false_block = 3u;
  const auto low = fixture.UserData(0u);
  const auto high = fixture.UserData(1u);
  const auto offset_or_count = fixture.UserData(2u);
  auto& phi = header->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
  const auto index = Value(&phi);
  // Match the ordinary unsigned scalar-add lowering, including both low-word
  // projections; neither carry component participates in the induction value.
  const auto sum = fixture.Emit(ValueOpcode::IAddCarry32, {index, Value(1u)}, 0, latch);
  const auto sum_low = fixture.Emit(ValueOpcode::CompositeExtractU32x2,
                                   {sum, Value(0u)}, 0, latch);
  const auto carry_sum = fixture.Emit(ValueOpcode::IAddCarry32,
                                     {sum_low, Value(0u)}, 0, latch);
  const auto next = fixture.Emit(ValueOpcode::CompositeExtractU32x2,
                                {carry_sum, Value(0u)}, 0, latch);
  phi.AddPhiOperand(entry, Value(0u));
  phi.AddPhiOperand(latch, next);
  fixture.block = guard;
  const auto address = fixture.Address(low, high, 0x20u);
  const auto count = memory_bound
      ? fixture.Emit(ValueOpcode::LoadAddressU32,
          {address, offset_or_count, Value(0u), Value(true)},
          fixture.AddMemory({.kind=ResourceKind::ScalarAddress}, 0x20u))
      : offset_or_count;
  const auto compare = fixture.Emit(ValueOpcode::ULessThan32, {index, count});
  fixture.program.block_info[2].condition = fixture.Emit(ValueOpcode::LogicalNot, {compare});
  fixture.block = body;
  const auto offset = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {index, Value(4u)});
  std::array<Value, 4> words;
  for (uint32_t word = 0u; word < 4u; ++word) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.offset = 16u + word * 4u;
    memory.component_count = 4u;
    memory.component_index = word;
    words[word] = fixture.Emit(ValueOpcode::LoadAddressU32,
        {address, offset, Value(0u), Value(true)}, fixture.AddMemory(memory, 0x40u));
  }
  const auto buffer = fixture.Buffer(words, 0x50u);
  // Count is ordinary shader data too: replacing only its descriptor-source
  // copy would leave this live consumer reading mutable guest memory.
  const auto payload = fixture.Emit(ValueOpcode::IAdd32, {count, Value(7u)});
  fixture.Emit(ValueOpcode::StoreBufferU32,
      {buffer, Value(0u), Value(0u), Value(0u), payload, Value(true)},
      fixture.AddMemory({.kind=ResourceKind::Buffer}, 0x50u));
  constexpr std::array ids{100u, 7u, 42u, 19u, 81u, 9u};
  for (auto& info : fixture.program.block_info) {
    info.id = ids[info.id];
    if (info.terminator.true_block != UINT32_MAX)
      info.terminator.true_block = ids[info.terminator.true_block];
    if (info.terminator.false_block != UINT32_MAX)
      info.terminator.false_block = ids[info.terminator.false_block];
  }
  fixture.PlanAndTrack();
  Check(fixture.program.resource_tracking_complete && fixture.program.info.buffers.size() == 1u,
        "unconditional Phi-to-guard chain did not retain its bounded descriptor table");
  for (const auto word : words) {
    const auto* read = word.Resolve().TryInstruction();
    Check(read && read->GetOpcode() == ValueOpcode::ReadBoundedSrtU32 &&
              read->Arg(0).Resolve() == index,
          "split-header descriptor lost its live induction index");
  }
  if (!memory_bound) return;
  Check(!fixture.program.info.uses_dma,
        "shared runtime-uniform loop bound still requires live guest DMA");
  const auto live_count = payload.ResolveInstruction()->Arg(0).Resolve();
  const auto* snapshot_read = live_count.TryInstruction();
  Check(snapshot_read && snapshot_read->GetOpcode() == ValueOpcode::ReadConst &&
            compare.ResolveInstruction()->Arg(1).Resolve() == live_count,
        "loop bound and ordinary consumer do not share a real immutable snapshot read");
  const auto slot = snapshot_read->Arg(1).Resolve().U32();
  auto plan = ExtractResourcePlan(fixture.program);
  for (const uint32_t count_value : {2u, 0u}) {
    BoundedSnapshotReader reader;
    reader.words.emplace_back(0x1000u, count_value);
    for (uint32_t row = 0u; row < count_value; ++row) {
      const std::array descriptor{0x20000u + row * 256u, (row + 1u) * 4u << 16u, 4u, 0u};
      for (uint32_t word = 0u; word < 4u; ++word)
        reader.words.emplace_back(0x1010u + row * 16u + word * 4u, descriptor[word]);
    }
    reader.change_repeated_reads = true;
    const std::array<uint32_t, 3> data{0x1000u, 0u, 0u};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan, BoundedSnapshotRuntime(reader, data), snapshot, specialization),
          "split-header bound failed coherent materialization");
    Check(slot < snapshot.flattened_srt.size() && snapshot.flattened_srt[slot] == count_value &&
              reader.ordinary_reads == 0u && reader.reads.size() == 1u + count_value * 4u,
          "loop bound was read twice, used an ordinary reader, or zero trip read a descriptor");
    const auto expected_ranges = count_value
        ? std::vector<ResourceReadRange>{{0x1000u, 4u}, {0x1010u, 32u}}
        : std::vector<ResourceReadRange>{{0x1000u, 4u}};
    Check(snapshot.immutable_srt_ranges == expected_ranges,
          "shared loop bound source bytes were omitted from immutable dependencies");
  }
}

void TestBoundedSrtSplitHeaderUniformCount() { CheckBoundedSrtSplitHeader(false); }
void TestBoundedSrtSplitHeaderSharedMemoryCount() { CheckBoundedSrtSplitHeader(true); }

void TestBoundedMaterializationAddressesAndSnapshot() {
  Fixture fixture;
  InitializeBoundedSnapshot(fixture, 1u, false);
  fixture.program.bounded_srt_reads[0].offset_scale = 4u;
  fixture.program.bounded_srt_reads[0].offset_bias = 0xfffffffdu;
  fixture.program.bounded_srt_reads[0].memory_offset = 0xfffffffdu; // signed -3, aligns to -4
  auto plan = ExtractResourcePlan(fixture.program);
  BoundedSnapshotReader reader;
  reader.words = {{0x100000ff8ull,0xa1u},{0xffcull,0xb2u},{0x1000ull,0xc3u}};
  const std::array<uint32_t,3> data{3u,0x1003u,0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, BoundedSnapshotRuntime(reader,data), snapshot,specialization),
        "wrapping bounded address materialization failed");
  Check(snapshot.flattened_srt == std::vector<uint32_t>{0xa1u,0xb2u,0xc3u} &&
            reader.reads == std::vector<uint64_t>{0x100000ff8ull,0xffcull,0x1000ull},
        "bounded address combined signed immediate with wrapping U32 offset incorrectly");
  Check(snapshot.immutable_srt_ranges == std::vector<ResourceReadRange>{{0xffcu,8u},{0x100000ff8ull,4u}},
        "bounded snapshot footprints lost exact source bytes or merged a gap");
  Check(reader.ordinary_reads == 0u, "bounded payload used the ordinary reader");
  const auto saved_snapshot = snapshot;
  const auto saved_specialization = specialization;
  reader.fail_address = 0x1000u;
  Check(!MaterializeResources(plan, BoundedSnapshotRuntime(reader,data), snapshot,specialization),
        "missing last bounded word did not reject the transaction");
  CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);
  const std::array<uint32_t,3> underflow{1u,0u,0u};
  plan.bounded_srt_reads[0].offset_bias=0u;
  const auto before_overflow_reads=reader.reads.size();
  Check(!MaterializeResources(plan, BoundedSnapshotRuntime(reader,underflow), snapshot,specialization),
        "negative immediate below address zero was not rejected");
  CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);
  Check(reader.reads.size()==before_overflow_reads, "underflow reached a memory callback");
  plan.bounded_srt_reads[0].offset_bias=8u;
  plan.bounded_srt_reads[0].memory_offset=0u;
  const std::array<uint32_t,3> overflow{1u,0xfffffffcu,0xffffu};
  Check(!MaterializeResources(plan,BoundedSnapshotRuntime(reader,overflow),snapshot,specialization) &&
            reader.reads.size()==before_overflow_reads,
        "48-bit source overflow reached a memory callback or was accepted");
  CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);

  Fixture duplicate;
  InitializeBoundedSnapshot(duplicate,2u,false);
  for (auto& read : duplicate.program.bounded_srt_reads) { read.offset_scale=4u; read.offset_bias=0u; }
  const auto count_address=duplicate.Address(Value(0x800u),Value(0u));
  const auto count_flags=duplicate.AddMemory(
      {.kind=ResourceKind::ScalarAddress,.planning_only=true},0x20u);
  const auto count_raw=duplicate.Emit(ValueOpcode::LoadAddressU32,
      {count_address,Value(0u),Value(0u),Value(true)},count_flags);
  duplicate.program.srt_reads.push_back({count_raw,0u});
  const auto srt=duplicate.Emit(ValueOpcode::GetSrtResource);
  duplicate.program.descriptor_sources[0].dwords[0]=duplicate.Emit(ValueOpcode::ReadConst,{srt,Value(0u)});
  auto duplicate_plan = ExtractResourcePlan(duplicate.program);
  reader = {};
  reader.words = {{0x800u,2u},{0x1000u,0x12u},{0x1004u,0x34u}};
  reader.change_repeated_reads = true;
  const std::array<uint32_t,3> twice{2u,0x1000u,0u};
  Check(MaterializeResources(duplicate_plan,BoundedSnapshotRuntime(reader,twice),snapshot,specialization),
        "duplicate bounded columns failed");
  Check(snapshot.flattened_srt == std::vector<uint32_t>{2u,0x12u,0x34u,0x12u,0x34u} &&
            reader.reads.size() == 3u && reader.ordinary_reads==0u &&
            snapshot.immutable_srt_ranges==std::vector<ResourceReadRange>{{0x800u,4u},{0x1000u,8u}},
        "same coherent source word was read twice or observed inconsistent values");
}

void TestBoundedMaterializationZerosUnmappedScalarRows() {
  Fixture fixture;
  InitializeBoundedSnapshot(fixture, 1u, false);
  fixture.program.bounded_srt_reads[0].offset_scale = 4u;
  fixture.program.bounded_srt_reads[0].offset_bias = 0u;
  fixture.program.bounded_srt_reads[0].memory_offset = 0u;
  auto plan = ExtractResourcePlan(fixture.program);
  BoundedSnapshotReader reader;
  // Row 0 is mapped; row 1 lands on an unmapped foreign address. Dense snapshots
  // must keep width with a zero word instead of aborting materialization.
  reader.words = {{0x1000ull, 0x11u}};
  reader.unmapped_address = 0x1004ull;
  const std::array<uint32_t, 3> data{2u, 0x1000u, 0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, BoundedSnapshotRuntime(reader, data), snapshot, specialization),
        "unmapped bounded scalar row aborted the coherent snapshot");
  Check(snapshot.flattened_srt == std::vector<uint32_t>{0x11u, 0u} &&
            reader.reads == std::vector<uint64_t>{0x1000ull},
        "unmapped bounded scalar row was not zeroed or still reached Clean");
  Check(snapshot.immutable_srt_ranges == std::vector<ResourceReadRange>{{0x1000ull, 4u}},
        "unmapped bounded row was recorded as an immutable footprint");
  Check(reader.ordinary_reads == 0u, "unmapped bounded row used the ordinary reader");
  // Mapped-but-unreadably-dirty sources must still reject the transaction.
  reader.fail_address = 0x1000ull;
  reader.unmapped_address = UINT64_MAX;
  const auto saved_snapshot = snapshot;
  const auto saved_specialization = specialization;
  Check(!MaterializeResources(plan, BoundedSnapshotRuntime(reader, data), snapshot, specialization),
        "dirty coherent source was accepted as a zeroed foreign row");
  CheckBoundedTransaction(snapshot, saved_snapshot, specialization, saved_specialization);
}

void TestBoundedMaterializationCandidatesAndRemap() {
  for (const uint32_t count : {3u,0u}) {
    Fixture fixture;
    InitializeBoundedSnapshot(fixture,4u,true);
    const auto ordinary_source = AddBoundedSnapshotSource(fixture,
        {Value(0x40000u),Value(16u << 16u),Value(4u),Value(0u)});
    fixture.program.info.buffers.push_back({.source=ordinary_source});
    auto ordinary = fixture.Buffer({Value(0x40000u),Value(16u << 16u),Value(4u),Value(0u)});
    ordinary.ResolveInstruction()->SetFlags<uint32_t>(1u);
    const auto flags = fixture.AddMemory({.kind=ResourceKind::Buffer,.resource=1u},0x40u);
    fixture.Emit(ValueOpcode::LoadBufferU32,{ordinary,Value(0u),Value(0u),Value(0u),Value(true)},flags);
    // Two uses share one metadata index: a remap must occur once, not per use.
    fixture.Emit(ValueOpcode::LoadBufferU32,{ordinary,Value(1u),Value(0u),Value(0u),Value(true)},flags);
    fixture.program.memory_info.push_back({.kind=ResourceKind::ScalarBuffer,.resource=99u,.planning_only=true});
    auto plan = ExtractResourcePlan(fixture.program);
    BoundedSnapshotReader reader;
    const std::array<uint32_t,4> first{0x20000u,4u << 16u,8u,0u};
    const std::array<uint32_t,4> second{0x20000u,8u << 16u,8u,1u};
    for (uint32_t index=0; index<3u; ++index)
      for (uint32_t word=0; word<4u; ++word)
        reader.words.emplace_back(0x1000u+index*16u+word*4u, (index==1u ? second : first)[word]);
    // Zero count provides no address user-data words; unreachable table address is not evaluated.
    const std::vector<uint32_t> data = count ? std::vector<uint32_t>{count,0x1000u,0u}
                                            : std::vector<uint32_t>{0u};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan,BoundedSnapshotRuntime(reader,data),snapshot,specialization),
          "bounded candidates or zero-trip table failed materialization");
    const uint32_t ordinary_dense = count ? 2u : 0u;
    Check(snapshot.buffers.size() == ordinary_dense+1u &&
              specialization.buffer_origins == (count ? std::vector<uint32_t>{0u,0u,1u}
                                                     : std::vector<uint32_t>{1u}),
          "bounded full-word dedup or ordinary buffer origins changed");
    const auto& table = specialization.buffer_tables[0];
    Check(table.count == count && table.resources == (count ? std::vector<uint32_t>{0u,1u}
                                                          : std::vector<uint32_t>{}),
          "bounded table inserted a null candidate or lost a distinct descriptor");
    if (count) {
      Check(snapshot.buffers[0].dwords[1]==first[1] && snapshot.buffers[1].dwords[1]==second[1] &&
                snapshot.buffers[1].dwords[3]==second[3] &&
                specialization.buffers[0].packed_stride != specialization.buffers[1].packed_stride,
            "candidate descriptor words or independent strides were lost");
      Check(std::vector<uint32_t>(snapshot.flattened_srt.begin()+table.mapping_flat_offset,
                                 snapshot.flattened_srt.end()) == std::vector<uint32_t>{0u,1u,0u},
            "bounded descriptor indices do not map to their deduplicated candidates");
      // Keep equal address/stride/length; only descriptor dword3 distinguishes the second candidate.
      for (auto& word : reader.words) if (word.first==0x1014u) word.second=first[1];
      ResourceSnapshot full_word_snapshot;
      ResourceSpecialization full_word_specialization;
      Check(MaterializeResources(plan,BoundedSnapshotRuntime(reader,data),full_word_snapshot,full_word_specialization) &&
                full_word_snapshot.buffers.size()==3u &&
                full_word_specialization.buffers[0].packed_stride==full_word_specialization.buffers[1].packed_stride &&
                full_word_snapshot.buffers[0].dwords[3]!=full_word_snapshot.buffers[1].dwords[3],
            "dedup ignored descriptor dword3 when address, stride and length matched");
    } else {
      Check(reader.reads.empty() && snapshot.immutable_srt_ranges.empty() && snapshot.flattened_srt.empty(),
            "zero-count bounded table read memory or retained a placeholder payload");
    }
    const auto saved_snapshot=snapshot;
    const auto saved_specialization=specialization;
    const auto saved_column=plan.bounded_srt_reads[1];
    for (const bool change_bias : {true,false}) {
      if (change_bias) plan.bounded_srt_reads[1].offset_bias=4u;
      else plan.bounded_srt_reads[1].memory_offset=8u;
      // Both redirected source words exist for every record. Rejection must come
      // from incompatible descriptor columns, rather than an incidental read failure.
      Check(!MaterializeResources(plan,BoundedSnapshotRuntime(reader,data),snapshot,specialization),
            "bounded descriptor accepted mismatched dynamic bias or immediate column offset");
      CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);
      plan.bounded_srt_reads[1]=saved_column;
    }
    ApplyResourceSpecialization(fixture.program,specialization);
    Check(fixture.program.info.buffers.size()==ordinary_dense+1u &&
              fixture.program.memory_info[flags.index].resource==ordinary_dense &&
              ordinary.ResolveInstruction()->Flags<uint32_t>()==ordinary_dense &&
              fixture.program.memory_info[1].resource==99u,
          "ordinary buffer after expanded/empty table or stale metadata remapped incorrectly");
  }
}

void TestBoundedMaterializationLimitsAreTransactional() {
  Fixture fixture;
  InitializeBoundedSnapshot(fixture,2u,false);
  for (auto& read : fixture.program.bounded_srt_reads) { read.offset_scale=0u; read.offset_bias=0u; }
  auto plan = ExtractResourcePlan(fixture.program);
  BoundedSnapshotReader reader;
  reader.words={{0x1000u,0x1234u}};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const std::array<uint32_t,3> at_limit{32768u,0x1000u,0u};
  Check(MaterializeResources(plan,BoundedSnapshotRuntime(reader,at_limit),snapshot,specialization) &&
            snapshot.flattened_srt.size()==65536u && reader.reads.size()==1u,
        "exact total 65536-probe bounded snapshot failed");
  const auto saved_snapshot=snapshot;
  const auto saved_specialization=specialization;
  const std::array<uint32_t,3> over_limit{32769u,0x1000u,0u};
  Check(!MaterializeResources(plan,BoundedSnapshotRuntime(reader,over_limit),snapshot,specialization),
        "total bounded probes exceeded 65536 across columns");
  CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);

  Fixture dense;
  InitializeBoundedSnapshot(dense,4u,true);
  auto dense_plan=ExtractResourcePlan(dense.program);
  reader={}; reader.generated_descriptors=ShaderInfo::MaxBuffers+1u;
  const std::array<uint32_t,3> max_candidates{ShaderInfo::MaxBuffers,0x1000u,0u};
  Check(MaterializeResources(dense_plan,BoundedSnapshotRuntime(reader,max_candidates),snapshot,specialization) &&
            snapshot.buffers.size()==ShaderInfo::MaxBuffers,
        "exact dense buffer candidate limit was rejected");
  const auto dense_saved=snapshot;
  const auto dense_specialization=specialization;
  const std::array<uint32_t,3> too_many{ShaderInfo::MaxBuffers+1u,0x1000u,0u};
  Check(!MaterializeResources(dense_plan,BoundedSnapshotRuntime(reader,too_many),snapshot,specialization),
        "dense buffer candidate limit plus one was accepted");
  CheckBoundedTransaction(snapshot,dense_saved,specialization,dense_specialization);
}

void TestBoundedMaterializationRejectsWritableAliases() {
  struct Scenario {
    bool overlap;
    bool oversized;
  };
  for (const auto [overlap, oversized] :
       {Scenario{false, false}, Scenario{true, false},
        Scenario{false, true}, Scenario{true, true}}) {
    Fixture fixture;
    InitializeBoundedSnapshot(fixture,4u,true);
    const uint32_t address = overlap ? 0x100cu : oversized ? 0x2000u : 0x1010u;
    const auto writer=AddBoundedSnapshotSource(fixture,
        {Value(address), Value(oversized ? 0x3fffu << 16u : 0u),
         Value(oversized ? UINT32_MAX : 4u), Value(0u)});
    fixture.program.info.buffers.push_back({.source=writer,.written=true});
    auto plan=ExtractResourcePlan(fixture.program);
    BoundedSnapshotReader reader; reader.generated_descriptors=1u;
    const std::array<uint32_t,3> data{1u,0x1000u,0u};
    ResourceSnapshot snapshot; snapshot.user_data={0xfeedu};
    snapshot.immutable_srt_ranges={{0x800u,4u}};
    ResourceSpecialization specialization;
    const auto old_snapshot=snapshot;
    const auto old_specialization=specialization;
    const bool accepted=MaterializeResources(plan,BoundedSnapshotRuntime(reader,data),snapshot,specialization);
    Check(accepted != overlap,
          oversized
              ? "oversized bounded writer was rejected before conservative alias classification"
              : "bounded source final-word alias or exact-end disjoint writer misclassified");
    if (overlap) CheckBoundedTransaction(snapshot,old_snapshot,specialization,old_specialization);
  }

  // A frontend CFG proof may establish that every live bounded SRT read
  // executes before any possible writer. The pre-dispatch snapshot then has
  // the same value as the guest scalar read even when the later output aliases
  // its source bytes.
  Fixture ordered;
  InitializeBoundedSnapshot(ordered, 4u, true);
  const auto ordered_writer = AddBoundedSnapshotSource(
      ordered, {Value(0x100cu), Value(0u), Value(4u), Value(0u)});
  ordered.program.info.buffers.push_back(
      {.source = ordered_writer, .written = true});
  auto ordered_plan = ExtractResourcePlan(ordered.program);
  ordered_plan.bounded_srt_reads_precede_writes = true;
  BoundedSnapshotReader ordered_reader;
  ordered_reader.generated_descriptors = 1u;
  const std::array<uint32_t, 3> ordered_data{1u, 0x1000u, 0u};
  ResourceSnapshot ordered_snapshot;
  ResourceSpecialization ordered_specialization;
  Check(MaterializeResources(ordered_plan,
                             BoundedSnapshotRuntime(ordered_reader, ordered_data),
                             ordered_snapshot, ordered_specialization),
        "proved read-before-write bounded snapshot overlap was rejected");

  for (const bool writer_first : {false, true}) {
    for (const bool cyclic : {false, true}) {
      Fixture proof;
      const auto buffer = proof.Buffer(
          {Value(0x100cu), Value(0u), Value(4u), Value(0u)});
      const auto emit_read = [&] {
        proof.Emit(ValueOpcode::ReadBoundedSrtU32, {Value(0u)}, 0u);
      };
      const auto emit_write = [&] {
        proof.Emit(ValueOpcode::StoreBufferU32,
                   {buffer, Value(0u), Value(0u), Value(0u), Value(7u),
                    Value(true)},
                   proof.AddMemory({.kind = ResourceKind::Buffer}, 0x1d94u));
      };
      if (writer_first) emit_write();
      emit_read();
      if (!writer_first) emit_write();
      if (cyclic) proof.block->AddBranch(proof.block);
      Check(ProveBoundedSrtReadsPrecedeWrites(proof.program) ==
                (!writer_first && !cyclic),
            "bounded SRT read/write CFG order proof crossed a writer or loop");
    }
  }

  // Guest buffer descriptors may reserve a much larger logical record range
  // than the VMA that NativeStorageBuffer can actually bind.  The immutable
  // table starts exactly after that mapped prefix, so the formal descriptor
  // overlaps it while the native writable interval does not.
  Fixture mapped_prefix;
  InitializeBoundedSnapshot(mapped_prefix,4u,true);
  const auto writer=AddBoundedSnapshotSource(mapped_prefix,
      {Value(0x800u),Value(0u),Value(0x1000u),Value(0u)});
  mapped_prefix.program.info.buffers.push_back({.source=writer,.written=true});
  auto mapped_plan=ExtractResourcePlan(mapped_prefix.program);
  BoundedSnapshotReader mapped_reader;
  mapped_reader.generated_descriptors=1u;
  mapped_reader.clamped_address=0x800u;
  mapped_reader.clamped_size=0x800u;
  const std::array<uint32_t,3> mapped_data{1u,0x1000u,0u};
  ResourceSnapshot mapped_snapshot;
  ResourceSpecialization mapped_specialization;
  Check(MaterializeResources(mapped_plan,BoundedSnapshotRuntime(mapped_reader,mapped_data),
                             mapped_snapshot,mapped_specialization),
        "formal writable descriptor overlap ignored the disjoint mapped prefix");

  // With STRIDE=0, vector-buffer instructions ignore vindex. Every access in
  // this resource has zero voffset/soffset and writes one U32x4, so only the
  // first 16 bytes are writable even though the descriptor reserves a much
  // larger byte-addressed range that crosses the immutable table.
  Fixture stride_zero;
  InitializeBoundedSnapshot(stride_zero,4u,true);
  auto& stride_zero_writer=stride_zero.program.info.buffers[0];
  stride_zero_writer.written=true;
  stride_zero_writer.formatted=true;
  stride_zero_writer.descriptor_formatted_only=true;
  stride_zero_writer.max_byte_extent=16u;
  stride_zero_writer.stride_zero_access_size=16u;
  stride_zero_writer.first_use_pc=0x1d94u;
  const auto stride_zero_plan=ExtractResourcePlan(stride_zero.program);
  BoundedSnapshotReader stride_zero_reader;
  constexpr uint64_t table_address=0x201347c600ull;
  const std::array<uint32_t,4> captured_descriptor{
      0x132143d0u,0x00000020u,0x13214580u,0x00000020u};
  for (uint32_t word=0u; word<captured_descriptor.size(); ++word)
    stride_zero_reader.words.emplace_back(table_address+word*4u,
                                          captured_descriptor[word]);
  const std::array<uint32_t,3> stride_zero_data{
      1u,static_cast<uint32_t>(table_address),
      static_cast<uint32_t>(table_address>>32u)};
  ResourceSnapshot stride_zero_snapshot;
  ResourceSpecialization stride_zero_specialization;
  Check(MaterializeResources(stride_zero_plan,
                             BoundedSnapshotRuntime(stride_zero_reader,stride_zero_data),
                             stride_zero_snapshot,stride_zero_specialization),
        "bounded stride-zero writer used its unreachable formal tail for aliasing");
}

void TestBoundedMaterializationNullsForeignBufferSlots() {
  Fixture fixture;
  InitializeBoundedSnapshot(fixture, 4u, true);
  auto& writer = fixture.program.info.buffers[0];
  writer.written = true;
  writer.first_use_pc = 0x1d94u;
  const auto plan = ExtractResourcePlan(fixture.program);

  BoundedSnapshotReader reader;
  const std::array<uint32_t, 4> valid{0x20000u, 4u << 16u, 4u, 0u};
  // Captured table bytes from an unselected foreign row.  Interpreted as a
  // buffer descriptor, its Base48 is outside the renderer's registered 40-bit
  // guest address range and therefore cannot be bound as a real candidate.
  const std::array<uint32_t, 4> foreign_address{0x0000af71u, 0x3b1cb71eu,
                                                0x0000af1eu, 0x3b32b6c8u};
  const std::array<uint32_t, 4> foreign_reserved{0x05500000u, 0x00000000u,
                                                 0x40000000u, 0x3f700000u};
  const std::array<uint32_t, 4> foreign_unmapped{0x43fa0000u, 0x00010000u,
                                                 0x13214580u, 0x00000000u};
  const std::array descriptors{valid, foreign_address, foreign_reserved, foreign_unmapped};
  for (uint32_t index = 0; index < descriptors.size(); ++index) {
    const auto& descriptor = descriptors[index];
    for (uint32_t word = 0; word < descriptor.size(); ++word) {
      reader.words.emplace_back(0x1000u + index * 16u + word * 4u,
                                descriptor[word]);
    }
  }
  const std::array<uint32_t, 3> data{4u, 0x1000u, 0u};
  reader.unmapped_address = 0x43fa0000u;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, BoundedSnapshotRuntime(reader, data), snapshot,
                             specialization),
        "bounded writable table with an unaddressable foreign slot failed materialization");
  Check(snapshot.buffers.size() == 2u &&
            snapshot.buffers[0].dword_count == valid.size() &&
            std::equal(valid.begin(), valid.end(), snapshot.buffers[0].dwords.begin()) &&
            std::ranges::all_of(snapshot.buffers[1].dwords,
                                [](uint32_t word) { return word == 0u; }),
        "foreign bounded buffer slot was not canonicalized to a distinct null candidate");
  const auto& table = specialization.buffer_tables[0];
  Check(table.count == 4u && table.resources == std::vector<uint32_t>{0u, 1u} &&
            std::vector<uint32_t>(snapshot.flattened_srt.begin() + table.mapping_flat_offset,
                                  snapshot.flattened_srt.end()) ==
                std::vector<uint32_t>{0u, 1u, 1u, 1u},
        "foreign bounded buffer slot lost its stable runtime mapping");
}

enum class FiniteSelectorScenario {
  Select, Phi, CarryOffset, UnknownArm, UndefArm, CyclicPhi,
  ConditionalRoot, MixedColumns, MixedIndex, VertexStage, EmptyExecUndef,
};

struct FiniteSelectorFixture {
  std::unique_ptr<Fixture> fixture;
  std::array<Value, 4> words;
  Value index;
  Value payload;
};

FiniteSelectorFixture MakeFiniteSelectorFixture(FiniteSelectorScenario scenario) {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  FiniteSelectorFixture result;
  result.fixture = std::make_unique<Fixture>(
      scenario == FiniteSelectorScenario::VertexStage ? ShaderType::Vertex : ShaderType::Compute);
  auto& f = *result.fixture;
  const auto base_low = f.UserData(0u);
  const auto base_high = f.UserData(1u);
  const auto unknown = f.UserData(2u);
  const auto lane = f.Emit(ValueOpcode::LaneId);
  const auto p0 = f.Emit(ValueOpcode::IEqual32, {lane, Value(0u)});
  const auto p1 = f.Emit(ValueOpcode::IEqual32, {lane, Value(1u)});
  const auto p2 = f.Emit(ValueOpcode::IEqual32, {lane, Value(2u)});
  const auto p3 = f.Emit(ValueOpcode::IEqual32, {lane, Value(3u)});
  const auto ballot = f.Emit(ValueOpcode::Ballot, {p0});
  const auto ballot_low = f.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(0u)});
  const auto branch_condition = f.Emit(ValueOpcode::INotEqual32, {ballot_low, Value(0u)});
  const auto branch = [&](uint32_t from, uint32_t to) {
    f.program.blocks[from]->AddBranch(f.program.blocks[to]);
    auto& term = f.program.block_info[from].terminator;
    term.kind = CFG::TerminatorKind::Branch;
    term.true_block = to;
  };
  const auto conditional = [&](uint32_t from, uint32_t yes, uint32_t no, Value condition) {
    f.program.blocks[from]->AddBranch(f.program.blocks[yes]);
    f.program.blocks[from]->AddBranch(f.program.blocks[no]);
    auto& info = f.program.block_info[from];
    info.terminator.kind = CFG::TerminatorKind::ConditionalBranch;
    info.terminator.true_block = yes;
    info.terminator.false_block = no;
    info.condition = condition;
    f.Emit(ValueOpcode::Reference, {condition}, 0, f.program.blocks[from]);
  };
  Value selector;
  Value low = base_low;
  Value high = base_high;
  Value active(true);
  if (scenario == FiniteSelectorScenario::Phi || scenario == FiniteSelectorScenario::CyclicPhi) {
    auto* left = f.AddBlock();
    auto* right = f.AddBlock();
    auto* merge = f.AddBlock();
    conditional(0u, 1u, 2u, branch_condition);
    branch(1u, 3u);
    branch(2u, 3u);
    const auto pair = f.Emit(ValueOpcode::SelectU32, {p0, Value(0u), Value(1u)}, 0, left);
    const auto left_value = f.Emit(ValueOpcode::SelectU32, {p1, Value(2u), pair}, 0, left);
    const auto right_value = f.Emit(ValueOpcode::SelectU32, {p2, Value(3u), Value(4u)}, 0, right);
    auto& phi = merge->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
    phi.AddPhiOperand(left, left_value);
    phi.AddPhiOperand(right, right_value);
    selector = Value(&phi);
    f.block = merge;
    if (scenario == FiniteSelectorScenario::CyclicPhi) {
      f.AddBlock();
      const auto next = f.Emit(ValueOpcode::IAdd32, {selector, Value(1u)});
      phi.AddPhiOperand(merge, next);
      const auto again = f.Emit(ValueOpcode::ULessThan32, {next, Value(100u)});
      conditional(3u, 3u, 4u, again);
      f.program.block_info[3].terminator.loop_header = true;
      f.program.block_info[4].terminator.kind = CFG::TerminatorKind::Return;
    } else {
      f.program.block_info[3].terminator.kind = CFG::TerminatorKind::Return;
    }
  } else {
    Value last(4u);
    if (scenario == FiniteSelectorScenario::UnknownArm) last = unknown;
    if (scenario == FiniteSelectorScenario::UndefArm) last = f.Emit(ValueOpcode::UndefU32);
    selector = f.Emit(ValueOpcode::SelectU32, {p3, Value(3u), last});
    selector = f.Emit(ValueOpcode::SelectU32, {p2, Value(2u), selector});
    selector = f.Emit(ValueOpcode::SelectU32, {p1, Value(1u), selector});
    selector = f.Emit(ValueOpcode::SelectU32, {p0, Value(0u), selector});
    f.program.block_info[0].terminator.kind = CFG::TerminatorKind::Return;
    if (scenario == FiniteSelectorScenario::ConditionalRoot) {
      auto* roots = f.AddBlock();
      auto* body = f.AddBlock();
      f.AddBlock();
      conditional(0u, 1u, 3u, branch_condition);
      branch(1u, 2u);
      branch(2u, 3u);
      f.program.block_info[3].terminator.kind = CFG::TerminatorKind::Return;
      f.block = roots;
      low = f.Emit(ValueOpcode::LoadAddressU32,
          {f.Address(base_low, base_high), unknown, Value(0u), Value(true)},
          f.AddMemory({.kind=ResourceKind::ScalarAddress}, 0x10u));
      high = Value(0u);
      f.block = body;
    }
    if (scenario == FiniteSelectorScenario::EmptyExecUndef) {
      active = f.Emit(ValueOpcode::IEqual32, {unknown, Value(0u)});
      selector = f.Emit(ValueOpcode::SelectU32,
          {active, Value(1u), f.Emit(ValueOpcode::UndefU32)});
    }
  }
  result.index = f.Emit(ValueOpcode::ReadFirstLane, {selector, active});
  const auto shifted = f.Emit(ValueOpcode::ShiftLeftLogical32, {result.index, Value(4u)});
  Value offset;
  if (scenario == FiniteSelectorScenario::CarryOffset) {
    const auto sum = f.Emit(ValueOpcode::IAddCarry32, {shifted, Value(32u)});
    offset = f.Emit(ValueOpcode::CompositeExtractU32x2, {sum, Value(0u)});
  } else {
    offset = f.Emit(ValueOpcode::IAdd32, {shifted, Value(32u)});
  }
  const auto address = f.Address(low, high, 0x30u);
  for (uint32_t word = 0; word < 4u; ++word) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.offset = word * 4u;
    memory.component_count = 4u;
    memory.component_index = word;
    if (scenario == FiniteSelectorScenario::MixedColumns && word == 3u) memory.offset += 4u;
    auto word_offset = offset;
    if (scenario == FiniteSelectorScenario::MixedIndex && word == 3u) {
      const auto other_values = f.Emit(ValueOpcode::SelectU32, {p3, Value(0u), Value(4u)});
      const auto other_index = f.Emit(ValueOpcode::ReadFirstLane, {other_values, Value(true)});
      const auto other_shift = f.Emit(ValueOpcode::ShiftLeftLogical32, {other_index, Value(4u)});
      word_offset = f.Emit(ValueOpcode::IAdd32, {other_shift, Value(32u)});
    }
    result.words[word] = f.Emit(ValueOpcode::LoadAddressU32,
        {address, word_offset, Value(0u), Value(true)}, f.AddMemory(memory, 0x40u));
  }
  const auto handle = f.Buffer(result.words, 0x50u);
  // Ordinary consumers of every word must observe the descriptor snapshot.
  result.payload = f.Emit(ValueOpcode::IAdd32, {result.words[0], result.words[1]});
  result.payload = f.Emit(ValueOpcode::IAdd32, {result.payload, result.words[2]});
  result.payload = f.Emit(ValueOpcode::IAdd32, {result.payload, result.words[3]});
  f.Emit(ValueOpcode::StoreBufferU32,
      {handle, Value(0u), Value(0u), Value(0u), result.payload, Value(true)},
      f.AddMemory({.kind=ResourceKind::Buffer, .idxen=true}, 0x50u));
  // Metadata branch targets are block IDs, not vector indices.
  for (auto& info : f.program.block_info) {
    info.id = 100u + info.id * 7u;
    if (info.terminator.true_block != UINT32_MAX)
      info.terminator.true_block = 100u + info.terminator.true_block * 7u;
    if (info.terminator.false_block != UINT32_MAX)
      info.terminator.false_block = 100u + info.terminator.false_block * 7u;
  }
  return result;
}

void TestFiniteSelectorSrtProof() {
  // RDNA2 READFIRSTLANE selects lane0 when EXEC is empty. A matching mask
  // Select cannot erase an undefined old value without a nonempty proof.
  auto empty = MakeFiniteSelectorFixture(FiniteSelectorScenario::EmptyExecUndef);
  Check(!ProveBoundedSrtRead(empty.fixture->program, *empty.words[0].ResolveInstruction()),
        "finite selector pruned an undefined lane0 value under possibly empty EXEC");
  for (auto scenario : {FiniteSelectorScenario::UnknownArm, FiniteSelectorScenario::UndefArm,
                        FiniteSelectorScenario::CyclicPhi, FiniteSelectorScenario::ConditionalRoot,
                        FiniteSelectorScenario::MixedColumns, FiniteSelectorScenario::MixedIndex,
                        FiniteSelectorScenario::VertexStage}) {
    auto test = MakeFiniteSelectorFixture(scenario);
    auto& program = test.fixture->program;
    if (scenario != FiniteSelectorScenario::MixedColumns && scenario != FiniteSelectorScenario::MixedIndex)
      Check(!ProveBoundedSrtRead(program, *test.words[0].ResolveInstruction()),
            "finite selector admitted unknown, undefined, cyclic or conditional-root provenance");
    BuildSrtPlan(program);
    CheckFatal([&] { TrackResources(program); }, "not a valid runtime value",
               "unproved or uncorrelated finite descriptor selection was accepted");
    Check(!program.resource_tracking_complete && program.info.buffers.empty() &&
              program.descriptor_sources.empty(),
          "rejected finite descriptor changed the committed resource plan");
  }
  std::cout << "finite selector rejection boundaries passed: 8\n";
  for (auto scenario : {FiniteSelectorScenario::Select, FiniteSelectorScenario::Phi,
                        FiniteSelectorScenario::CarryOffset}) {
    auto test = MakeFiniteSelectorFixture(scenario);
    auto& program = test.fixture->program;
    const auto proof = ProveBoundedSrtRead(program, *test.words[0].ResolveInstruction());
    Check(proof && proof->workgroup_axis == UINT32_MAX &&
              proof->index.Resolve() == test.index.Resolve() &&
              proof->count.Resolve().IsImmediate() && proof->count.Resolve().U32() == 5u &&
              proof->offset_scale == 16u && proof->offset_bias == 32u && proof->memory_offset == 0u,
          "finite ReadFirstLane selector has no exact five-candidate affine proof");
    test.fixture->PlanAndTrack();
    for (auto& word : test.words) {
      word = word.Resolve();
      const auto* read = word.TryInstruction();
      Check(read && read->GetOpcode() == ValueOpcode::ReadBoundedSrtU32 &&
                read->Arg(0).Resolve() == test.index.Resolve(),
            "finite descriptor word lost its actual GPU-selected table key");
    }
    EliminateDeadCode(program.blocks);
    ValidateProgram(program, true);
    Check(program.resource_tracking_complete && program.info.buffers.size() == 1u &&
              program.bounded_srt_reads.size() == 4u && !program.info.uses_dma,
          "finite selector did not retain one correlated table without live descriptor DMA");
    for (const auto word : test.words)
      Check(word.ResolveInstruction()->HasUses(),
            "finite descriptor snapshot lost an ordinary shader consumer during DCE");
  }
  std::cout << "finite selector proof positives passed: 3\n";
}

void TestFiniteSelectorSrtMaterialization() {
  auto test = MakeFiniteSelectorFixture(FiniteSelectorScenario::Phi);
  test.fixture->PlanAndTrack();
  EliminateDeadCode(test.fixture->program.blocks);
  ValidateProgram(test.fixture->program, true);
  auto plan = ExtractResourcePlan(test.fixture->program);
  Check(plan.requires_specialization_memory, "finite descriptor table lost coherent-reader admission");
  const std::array<uint32_t, 3> data{0x1000u, 0u, 123u};
  BoundedSnapshotReader reader;
  std::array<std::array<uint32_t, 4>, 5> descriptors;
  for (uint32_t row = 0; row < 5u; ++row) {
    descriptors[row] = {0x20000u + row * 256u, ((row + 1u) * 4u) << 16u, 4u, 0u};
    for (uint32_t word = 0; word < 4u; ++word)
      reader.words.emplace_back(0x1020u + row * 16u + word * 4u, descriptors[row][word]);
  }
  reader.change_repeated_reads = true;
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, BoundedSnapshotRuntime(reader, data), snapshot, specialization),
        "finite GPU selector required host evaluation or failed coherent table materialization");
  Check(snapshot.buffers.size() == 5u && specialization.buffer_tables.size() == 1u &&
            specialization.buffer_tables[0].count == 5u && reader.ordinary_reads == 0u &&
            reader.reads.size() == 20u &&
            snapshot.immutable_srt_ranges == std::vector<ResourceReadRange>{{0x1020u, 80u}},
        "finite table lost a candidate, read a word twice or omitted immutable descriptor bytes");
  const auto& table = specialization.buffer_tables[0];
  for (uint32_t row = 0; row < 5u; ++row) {
    const auto dense = snapshot.flattened_srt[table.mapping_flat_offset + row];
    Check(dense < snapshot.buffers.size() && snapshot.buffers[dense].dword_count == 4u &&
              std::equal(descriptors[row].begin(), descriptors[row].end(), snapshot.buffers[dense].dwords.begin()),
          "finite table mapping combined words from different descriptors or lost a stride");
  }
  const auto saved_snapshot = snapshot;
  const auto saved_specialization = specialization;
  reader.reads.clear(); reader.fail_address = 0x1048u;
  Check(!MaterializeResources(plan, BoundedSnapshotRuntime(reader, data), snapshot, specialization),
        "finite table accepted an unreadable candidate word");
  CheckBoundedTransaction(snapshot, saved_snapshot, specialization, saved_specialization);
  reader.reads.clear(); reader.fail_address = UINT64_MAX;
  // Last candidate can write the final DWORD of the descriptor snapshot.
  for (auto& [address, value] : reader.words) {
    if (address == 0x1060u) value = 0x106cu;
    if (address == 0x1064u) value = 0u;
    if (address == 0x1068u) value = 4u;
  }
  Check(!MaterializeResources(plan, BoundedSnapshotRuntime(reader, data), snapshot, specialization),
        "finite descriptor candidate could write its immutable source footprint");
  CheckBoundedTransaction(snapshot, saved_snapshot, specialization, saved_specialization);
  std::cout << "finite selector materialization, transaction and alias checks passed\n";
}

enum class ActiveFiniteScenario {
  ValidLow, ValidHigh, ValidOr, MissingGuard, WrongEdge, BypassGuard,
  DifferentMask, ConstantBits, WrongBallotHalf, VaryingWord,
  EarlyExit, CyclicControl, UnrelatedEarlyExit,
};

FiniteSelectorFixture MakeActiveFiniteSelectorFixture(ActiveFiniteScenario scenario) {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  FiniteSelectorFixture result;
  result.fixture = std::make_unique<Fixture>(ShaderType::Compute);
  auto& f = *result.fixture;
  auto* entry = f.block;
  auto* guard = f.AddBlock();
  auto* body = f.AddBlock();
  auto* exit = f.AddBlock();
  auto* after_guard = body;
  if (scenario == ActiveFiniteScenario::EarlyExit) {
    after_guard = f.AddBlock();
  }
  if (scenario == ActiveFiniteScenario::UnrelatedEarlyExit) {
    f.AddBlock(); // early exit target; referenced by id below
  }
  const auto branch = [&](uint32_t from, uint32_t to) {
    f.program.blocks[from]->AddBranch(f.program.blocks[to]);
    auto& info = f.program.block_info[from];
    info.terminator.kind = CFG::TerminatorKind::Branch;
    info.terminator.true_block = to;
  };
  const auto conditional = [&](uint32_t from, uint32_t yes, uint32_t no, Value condition) {
    f.program.blocks[from]->AddBranch(f.program.blocks[yes]);
    f.program.blocks[from]->AddBranch(f.program.blocks[no]);
    auto& info = f.program.block_info[from];
    info.terminator.kind = CFG::TerminatorKind::ConditionalBranch;
    info.terminator.true_block = yes;
    info.terminator.false_block = no;
    info.condition = condition;
    f.Emit(ValueOpcode::Reference, {condition}, 0, f.program.blocks[from]);
  };

  const auto ballot = f.Emit(ValueOpcode::Ballot, {Value(true)}, 0, entry);
  const auto ballot_low =
      f.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(0u)}, 0, entry);
  const auto ballot_high =
      f.Emit(ValueOpcode::CompositeExtractU32x4, {ballot, Value(1u)}, 0, entry);
  const auto lane = f.Emit(ValueOpcode::LaneId, {}, 0, entry);
  Value low;
  Value high(0u);
  if (scenario == ActiveFiniteScenario::ValidHigh) {
    high = f.Emit(ValueOpcode::BitwiseAnd32, {ballot_high, Value(0x20u)}, 0, entry);
    low = Value(0u);
  } else if (scenario == ActiveFiniteScenario::ValidOr) {
    const auto first = f.Emit(ValueOpcode::BitwiseAnd32, {ballot_low, Value(1u)}, 0, entry);
    const auto third = f.Emit(ValueOpcode::BitwiseAnd32, {ballot_low, Value(4u)}, 0, entry);
    low = f.Emit(ValueOpcode::BitwiseOr32, {first, third}, 0, entry);
  } else if (scenario == ActiveFiniteScenario::ConstantBits) {
    low = Value(1u);
  } else if (scenario == ActiveFiniteScenario::WrongBallotHalf) {
    low = ballot_high;
  } else if (scenario == ActiveFiniteScenario::VaryingWord) {
    const auto varying =
        f.Emit(ValueOpcode::ShiftLeftLogical32, {Value(1u), lane}, 0, entry);
    low = f.Emit(ValueOpcode::BitwiseAnd32, {ballot_low, varying}, 0, entry);
  } else {
    const auto sparse =
        f.Emit(ValueOpcode::BitwiseAnd32, {ballot_low, Value(0x01010101u)}, 0, entry);
    const auto first_byte =
        f.Emit(ValueOpcode::BitwiseAnd32, {ballot_low, Value(0xffu)}, 0, entry);
    low = f.Emit(ValueOpcode::BitwiseAnd32, {sparse, first_byte}, 0, entry);
    const auto upper_sparse =
        f.Emit(ValueOpcode::BitwiseAnd32, {ballot_high, Value(0x01010101u)}, 0, entry);
    high = f.Emit(ValueOpcode::BitwiseAnd32, {upper_sparse, Value(0u)}, 0, entry);
  }
  const auto bit = f.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(31u)}, 0, entry);
  const auto upper =
      f.Emit(ValueOpcode::UGreaterThanEqual32, {lane, Value(32u)}, 0, entry);
  const auto word = f.Emit(ValueOpcode::SelectU32, {upper, high, low}, 0, entry);
  const auto shifted = f.Emit(ValueOpcode::ShiftRightLogical32, {word, bit}, 0, entry);
  const auto selected =
      f.Emit(ValueOpcode::BitwiseAnd32, {shifted, Value(1u)}, 0, entry);
  const auto active = f.Emit(ValueOpcode::INotEqual32, {selected, Value(0u)}, 0, entry);
  const auto combined = f.Emit(ValueOpcode::BitwiseOr32, {low, high}, 0, entry);
  const auto empty = f.Emit(ValueOpcode::IEqual32, {combined, Value(0u)}, 0, guard);
  const auto branch_choice =
      f.Emit(ValueOpcode::INotEqual32, {f.UserData(3u), Value(0u)}, 0, entry);

  const auto guard_id = 1u;
  const auto body_id = 2u;
  const auto exit_id = 3u;
  const auto after_id = scenario == ActiveFiniteScenario::EarlyExit ? 4u : body_id;
  const auto early_exit_id =
      scenario == ActiveFiniteScenario::UnrelatedEarlyExit ? 4u : UINT32_MAX;
  if (scenario == ActiveFiniteScenario::MissingGuard) {
    branch(0u, body_id);
    f.program.block_info[guard_id].terminator.kind = CFG::TerminatorKind::Return;
  } else if (scenario == ActiveFiniteScenario::BypassGuard) {
    conditional(0u, guard_id, body_id, branch_choice);
    conditional(guard_id, exit_id, body_id, empty);
  } else if (scenario == ActiveFiniteScenario::UnrelatedEarlyExit) {
    // Unrelated early return before the mask guard must not block proving the
    // nonempty path that reaches ReadFirstLane (Ghost of Yōtei 86da pattern).
    conditional(0u, early_exit_id, guard_id, branch_choice);
    conditional(guard_id, exit_id, after_id, empty);
    f.program.block_info[early_exit_id].terminator.kind = CFG::TerminatorKind::Return;
  } else {
    branch(0u, guard_id);
    conditional(guard_id,
                scenario == ActiveFiniteScenario::WrongEdge ? body_id : exit_id,
                scenario == ActiveFiniteScenario::WrongEdge ? exit_id : after_id,
                empty);
  }
  if (scenario == ActiveFiniteScenario::EarlyExit) {
    conditional(after_id, body_id, exit_id, branch_choice);
  }

  f.block = body;
  const auto p0 = f.Emit(ValueOpcode::IEqual32, {lane, Value(0u)});
  const auto p1 = f.Emit(ValueOpcode::IEqual32, {lane, Value(1u)});
  const auto p2 = f.Emit(ValueOpcode::IEqual32, {lane, Value(2u)});
  const auto p3 = f.Emit(ValueOpcode::IEqual32, {lane, Value(3u)});
  auto finite = f.Emit(ValueOpcode::SelectU32, {p3, Value(3u), Value(4u)});
  finite = f.Emit(ValueOpcode::SelectU32, {p2, Value(2u), finite});
  finite = f.Emit(ValueOpcode::SelectU32, {p1, Value(1u), finite});
  finite = f.Emit(ValueOpcode::SelectU32, {p0, Value(0u), finite});
  const auto old_value = f.UserData(2u);
  const auto source_mask = scenario == ActiveFiniteScenario::DifferentMask
                               ? f.Emit(ValueOpcode::LogicalNot, {active})
                               : active;
  const auto source = f.Emit(ValueOpcode::SelectU32, {source_mask, finite, old_value});
  result.index = f.Emit(ValueOpcode::ReadFirstLane, {source, active});
  const auto offset_shift =
      f.Emit(ValueOpcode::ShiftLeftLogical32, {result.index, Value(4u)});
  const auto offset = f.Emit(ValueOpcode::IAdd32, {offset_shift, Value(32u)});
  const auto address = f.Address(f.UserData(0u), f.UserData(1u), 0x30u);
  for (uint32_t component = 0; component < 4u; ++component) {
    MemoryInfo memory;
    memory.kind = ResourceKind::ScalarAddress;
    memory.offset = component * sizeof(uint32_t);
    memory.component_count = 4u;
    memory.component_index = component;
    result.words[component] = f.Emit(ValueOpcode::LoadAddressU32,
        {address, offset, Value(0u), Value(true)}, f.AddMemory(memory, 0x40u));
  }
  const auto handle = f.Buffer(result.words, 0x50u);
  f.Emit(ValueOpcode::StoreBufferU32,
      {handle, Value(0u), Value(0u), Value(0u), Value(0x12345678u), Value(true)},
      f.AddMemory({.kind=ResourceKind::Buffer, .idxen=true}, 0x50u));
  if (scenario == ActiveFiniteScenario::CyclicControl) {
    conditional(body_id, body_id, exit_id, branch_choice);
  } else {
    branch(body_id, exit_id);
  }
  f.program.block_info[exit_id].terminator.kind = CFG::TerminatorKind::Return;
  return result;
}

void TestFiniteSelectorActiveMaskProof() {
  for (auto scenario : {ActiveFiniteScenario::MissingGuard, ActiveFiniteScenario::WrongEdge,
                        ActiveFiniteScenario::BypassGuard, ActiveFiniteScenario::DifferentMask,
                        ActiveFiniteScenario::ConstantBits, ActiveFiniteScenario::WrongBallotHalf,
                        ActiveFiniteScenario::VaryingWord, ActiveFiniteScenario::EarlyExit,
                        ActiveFiniteScenario::CyclicControl}) {
    auto test = MakeActiveFiniteSelectorFixture(scenario);
    Check(!ProveBoundedSrtRead(test.fixture->program, *test.words[0].ResolveInstruction()),
          "finite selector accepted an unsafe active-mask projection");
  }
  std::cout << "finite selector active-mask rejection boundaries passed: 9\n";
  for (auto scenario : {ActiveFiniteScenario::ValidLow, ActiveFiniteScenario::ValidHigh,
                        ActiveFiniteScenario::ValidOr,
                        ActiveFiniteScenario::UnrelatedEarlyExit}) {
    auto test = MakeActiveFiniteSelectorFixture(scenario);
    const auto proof =
        ProveBoundedSrtRead(test.fixture->program, *test.words[0].ResolveInstruction());
    Check(proof && proof->index.Resolve() == test.index.Resolve() &&
              proof->count.Resolve().IsImmediate() && proof->count.Resolve().U32() == 5u &&
              proof->offset_scale == 16u && proof->offset_bias == 32u,
          "finite selector did not use its guarded nonempty active-lane values");
    test.fixture->PlanAndTrack();
    Check(test.fixture->program.resource_tracking_complete &&
              test.fixture->program.info.buffers.size() == 1u &&
              test.fixture->program.bounded_srt_reads.size() == 4u,
          "guarded finite selector did not produce one correlated descriptor table");
  }
  std::cout << "finite selector active-mask positives passed: 4\n";
}


// Synthetic CPU regressions: append after the existing bounded snapshot helpers.
Value WorkgroupSrtIndex(Fixture& fixture, uint32_t axis) {
  return fixture.Emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)), Value(axis)});
}

Value WorkgroupSrtRawRead(Fixture& fixture, Value offset, uint32_t immediate = 0u) {
  fixture.program.block_info[0].terminator.kind =
      Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
  const auto address = fixture.Address(fixture.UserData(0u), fixture.UserData(1u));
  MemoryInfo memory; memory.kind = ResourceKind::ScalarAddress; memory.offset = immediate;
  const auto read = fixture.Emit(ValueOpcode::LoadAddressU32,
      {address, offset, Value(0u), Value(true)}, fixture.AddMemory(memory, 0x40u));
  fixture.Emit(ValueOpcode::ReferenceU32, {read});
  return read;
}

void TestWorkgroupSrtTrackingProof() {
  // Keep unsupported index provenance live; it must not become a fake snapshot.
  for (uint32_t scenario = 0; scenario < 5u; ++scenario) {
    Fixture fixture;
    Value index;
    if (scenario < 2u) {
      index = fixture.Emit(ValueOpcode::GetBuiltin,
          {Value(static_cast<uint32_t>(scenario == 0u ? StageInputKind::LocalInvocationId
                                                      : StageInputKind::GlobalInvocationId)), Value(0u)});
    } else if (scenario == 2u) {
      index = fixture.UserData(2u); // A uniform value is not a dispatch bound.
    } else {
      const auto x = WorkgroupSrtIndex(fixture, 0u);
      index = fixture.Emit(scenario == 3u ? ValueOpcode::IAdd32 : ValueOpcode::IMul32,
          {x, scenario == 3u ? WorkgroupSrtIndex(fixture, 1u) : x});
    }
    const auto read = WorkgroupSrtRawRead(fixture, index);
    Check(!ProveBoundedSrtRead(fixture.program, *read.ResolveInstruction()),
          "workgroup SRT proof accepted local/global/unbounded/multiple-axis/nonlinear index");
  }
  for (uint32_t axis = 0; axis < 3u; ++axis) {
    Fixture fixture;
    const auto index = WorkgroupSrtIndex(fixture, axis);
    const auto scaled = fixture.Emit(axis == 0u ? ValueOpcode::ShiftLeftLogical32 : ValueOpcode::IMul32,
        {index, Value(axis == 0u ? 4u : 12u)});
    // Include the carry-pair lowering of S_ADD_U32, not only plain IAdd.
    const auto carry = fixture.Emit(ValueOpcode::IAddCarry32, {scaled, Value(7u)});
    const auto offset = fixture.Emit(ValueOpcode::CompositeExtractU32x2, {carry, Value(0u)});
    const auto read = WorkgroupSrtRawRead(fixture, offset, 68u);
    const auto payload = fixture.Emit(ValueOpcode::IAdd32, {read, Value(9u)});
    fixture.Emit(ValueOpcode::ReferenceU32, {payload});
    const auto proof = ProveBoundedSrtRead(fixture.program, *read.ResolveInstruction());
    Check(proof && proof->workgroup_axis == axis && proof->index.Resolve() == index &&
              proof->offset_scale == (axis == 0u ? 16u : 12u) && proof->offset_bias == 7u &&
              proof->memory_offset == 68u,
          "affine WorkgroupId scalar read has no exact dispatch-axis proof");
    fixture.PlanAndTrack();
    const auto live_read = read.Resolve();
    EliminateDeadCode(fixture.program.blocks);
    ValidateProgram(fixture.program, true);
    const auto* indexed = live_read.TryInstruction();
    Check(indexed && indexed->GetOpcode() == ValueOpcode::ReadBoundedSrtU32 &&
              indexed->Arg(0).Resolve() == index &&
              payload.ResolveInstruction()->Arg(0).Resolve().TryInstruction() == indexed &&
              !fixture.program.info.uses_dma && fixture.program.bounded_srt_reads.size() == 1u,
          "workgroup SRT tracking dropped a live key/user or retained raw DMA");
    const auto& column = fixture.program.bounded_srt_reads[0];
    Check(column.workgroup_axis == axis && column.count_source == UINT32_MAX,
          "workgroup SRT column used an invented scalar count source");
    auto plan = ExtractResourcePlan(fixture.program);
    Check(plan.bounded_srt_reads == fixture.program.bounded_srt_reads &&
              plan.requires_specialization_memory,
          "extraction lost the dispatch-dependent column or clean-reader requirement");
    BoundedSnapshotReader reader;
    reader.words = {{0x1048u, 0x11u}, {axis == 0u ? 0x1058u : 0x1054u, 0x22u}};
    const std::array<uint32_t, 2> data{0x1000u, 0u};
    auto runtime = BoundedSnapshotRuntime(reader, data);
    std::array<uint32_t, 3> groups{1u, 1u, 1u}; groups[axis] = 2u;
    runtime.compute_workgroups = groups;
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.flattened_srt == std::vector<uint32_t>{0x11u, 0x22u} &&
              reader.ordinary_reads == 0u,
          "DCE or extraction lost the pure coefficient address roots");
  }
}

void TestWorkgroupSrtRootExecutionProof() {
  for (const bool conditional : {false, true}) {
    Fixture fixture;
    const auto low = fixture.UserData(0u);
    const auto high = fixture.UserData(1u);
    const auto root_offset = fixture.UserData(2u);
    const auto index = WorkgroupSrtIndex(fixture, 0u);
    auto* roots = fixture.AddBlock();
    auto* body = fixture.AddBlock();
    auto* exit = fixture.AddBlock();
    const auto branch = [&](uint32_t from, uint32_t to) {
      fixture.program.blocks[from]->AddBranch(fixture.program.blocks[to]);
      auto& term = fixture.program.block_info[from].terminator;
      term.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Branch;
      term.true_block = to;
    };
    branch(0u, 1u); branch(1u, 2u); branch(2u, 3u);
    fixture.program.block_info[3].terminator.kind =
        Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::Return;
    if (conditional) {
      fixture.program.blocks[0]->AddBranch(exit);
      auto& info = fixture.program.block_info[0];
      info.terminator.kind = Libs::Graphics::ShaderRecompiler::CFG::TerminatorKind::ConditionalBranch;
      info.terminator.false_block = 3u;
      info.condition = fixture.Emit(ValueOpcode::INotEqual32, {root_offset, Value(0u)});
    }
    fixture.block = roots;
    const auto pointer = fixture.Emit(ValueOpcode::LoadAddressU32,
        {fixture.Address(low, high), root_offset, Value(0u), Value(true)},
        fixture.AddMemory({.kind=ResourceKind::ScalarAddress}, 0x20u));
    const auto shared = fixture.Emit(ValueOpcode::IAdd32, {pointer, Value(1u)});
    fixture.Emit(ValueOpcode::ReferenceU32, {shared});
    fixture.block = body;
    const auto offset = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {index, Value(2u)});
    const auto read = fixture.Emit(ValueOpcode::LoadAddressU32,
        {fixture.Address(pointer, Value(0u)), offset, Value(0u), Value(true)},
        fixture.AddMemory({.kind=ResourceKind::ScalarAddress}, 0x40u));
    fixture.Emit(ValueOpcode::ReferenceU32, {read});
    Check(ProveBoundedSrtRead(fixture.program, *read.ResolveInstruction()).has_value() != conditional,
          "workgroup snapshot confused guaranteed split-prefix and conditional-only pointer roots");
    if (conditional) continue;
    fixture.PlanAndTrack();
    const auto live = read.Resolve();
    EliminateDeadCode(fixture.program.blocks);
    ValidateProgram(fixture.program, true);
    Check(live.ResolveInstruction()->GetOpcode() == ValueOpcode::ReadBoundedSrtU32 &&
              shared.ResolveInstruction()->Arg(0).ResolveInstruction()->GetOpcode() == ValueOpcode::ReadConst &&
              !fixture.program.info.uses_dma,
          "unconditional pointer root and its ordinary consumer did not share the clean snapshot");
    const auto slot = shared.ResolveInstruction()->Arg(0).ResolveInstruction()->Arg(1).U32();
    auto plan = ExtractResourcePlan(fixture.program);
    Check(slot < plan.clean_flat_slots.size() && plan.clean_flat_slots[slot] != 0u,
          "workgroup address-root snapshot lost its clean-read association");
  }
  Fixture invalid_axis;
  const auto read = WorkgroupSrtRawRead(invalid_axis, WorkgroupSrtIndex(invalid_axis, 3u));
  Check(!ProveBoundedSrtRead(invalid_axis.program, *read.ResolveInstruction()),
        "workgroup snapshot accepted a malformed axis");
}

void InitializeWorkgroupSnapshot(Fixture& fixture, std::initializer_list<uint32_t> axes) {
  fixture.program.srt_plan_complete = fixture.program.resource_tracking_complete = true;
  const auto source = AddBoundedSnapshotSource(fixture, {fixture.UserData(0u), fixture.UserData(1u)});
  for (const auto axis : axes) {
    BoundedSrtRead read{.address_source=source, .count_source=UINT32_MAX,
                       .offset_scale=4u, .offset_bias=0u, .memory_offset=0u};
    read.workgroup_axis = axis;
    fixture.program.bounded_srt_reads.push_back(read);
  }
}

SrtRuntime WorkgroupSnapshotRuntime(BoundedSnapshotReader& reader, std::span<const uint32_t> data,
                                    std::array<uint32_t,3> groups) {
  auto runtime = BoundedSnapshotRuntime(reader, data);
  runtime.compute_workgroups = groups;
  return runtime;
}

void TestWorkgroupSrtMaterializationAndSpecialization() {
  Fixture fixture;
  InitializeWorkgroupSnapshot(fixture, {0u,1u,2u});
  auto plan = ExtractResourcePlan(fixture.program);
  BoundedSnapshotReader reader;
  reader.words = {{0x1000u,0x11u},{0x1004u,0x22u},{0x1008u,0x33u}};
  reader.change_repeated_reads = true;
  const std::array<uint32_t,2> data{0x1003u,0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, WorkgroupSnapshotRuntime(reader,data,{3u,2u,1u}), snapshot,specialization),
        "known guest dispatch bounds did not materialize workgroup coefficients");
  Check(snapshot.flattened_srt == std::vector<uint32_t>{0x11u,0x22u,0x33u,0x11u,0x22u,0x11u} &&
            specialization.bounded_srt_reads == std::vector<BoundedSrtLayout>{{3u,0u},{2u,3u},{1u,5u}} &&
            reader.reads == std::vector<uint64_t>{0x1000u,0x1004u,0x1008u} && reader.ordinary_reads == 0u &&
            snapshot.immutable_srt_ranges == std::vector<ResourceReadRange>{{0x1000u,12u}},
        "axis cardinality, source alignment, cross-column memoization or footprints changed");
  plan.info.uses_dma = true;
  reader.reads.clear();
  Check(MaterializeResources(plan, WorkgroupSnapshotRuntime(reader,data,{3u,2u,1u}), snapshot,specialization) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0x11u,0x22u,0x33u,0x11u,0x22u,0x11u} &&
            snapshot.immutable_srt_ranges == std::vector<ResourceReadRange>{{0x1000u,12u}},
        "read-only DMA access rejected an otherwise coherent bounded SRT snapshot");
  const auto read_only_snapshot = snapshot;
  const auto read_only_specialization = specialization;
  plan.info.writes_dma = true;
  Check(!MaterializeResources(plan, WorkgroupSnapshotRuntime(reader,data,{3u,2u,1u}), snapshot,specialization),
        "dynamic DMA write was admitted alongside an immutable bounded SRT snapshot");
  CheckBoundedTransaction(snapshot,read_only_snapshot,specialization,read_only_specialization);
  plan.info.writes_dma = false;
  plan.info.uses_dma = false;
  const auto saved_snapshot = snapshot;
  const auto saved_specialization = specialization;
  reader.fail_address = 0x1008u;
  Check(!MaterializeResources(plan, WorkgroupSnapshotRuntime(reader,data,{3u,2u,1u}), snapshot,specialization),
        "missing final coefficient did not abort materialization");
  CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);
  reader.fail_address = UINT64_MAX;
  const auto before = reader.reads.size();
  Check(!MaterializeResources(plan, BoundedSnapshotRuntime(reader,data), snapshot,specialization) &&
            reader.reads.size() == before,
        "unknown dispatch bound was invented or read payload before rejection");
  CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);
  auto invalid = ExtractResourcePlan(fixture.program);
  invalid.bounded_srt_reads[0].workgroup_axis = 3u;
  Check(!MaterializeResources(invalid, WorkgroupSnapshotRuntime(reader,data,{3u,2u,1u}), snapshot,specialization),
        "invalid workgroup axis was accepted");
  CheckBoundedTransaction(snapshot,saved_snapshot,specialization,saved_specialization);
  reader.reads.clear();
  Check(MaterializeResources(plan, WorkgroupSnapshotRuntime(reader,data,{1u,1u,1u}), snapshot,specialization) &&
            specialization != saved_specialization &&
            specialization.bounded_srt_reads == std::vector<BoundedSrtLayout>{{1u,0u},{1u,1u},{1u,2u}},
        "dispatch count change reused a stale permutation layout");
  const auto same_layout = specialization;
  reader.reads.clear(); reader.words[0].second = 0x99u;
  Check(MaterializeResources(plan, WorkgroupSnapshotRuntime(reader,data,{1u,1u,1u}), snapshot,specialization) &&
            specialization == same_layout && snapshot.flattened_srt == std::vector<uint32_t>(3u,0x99u),
        "coefficient payload was cached across dispatches or unnecessarily changed the shader key");
  ApplyResourceSpecialization(fixture.program,specialization);
  Check(fixture.program.info.bounded_srt_reads == specialization.bounded_srt_reads,
        "ApplyResourceSpecialization lost dispatch-dependent limits/offsets");
}

void TestWorkgroupSrtZeroDispatchAndProbeLimit() {
  Fixture fixture;
  InitializeWorkgroupSnapshot(fixture,{0u,1u});
  for (auto& read : fixture.program.bounded_srt_reads) read.offset_scale = 0u;
  auto plan = ExtractResourcePlan(fixture.program);
  BoundedSnapshotReader reader;
  reader.words = {{0x1000u,0x77u}};
  const std::array<uint32_t,2> data{0x1000u,0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  // Any empty axis means no invocation can execute any column, even X reads.
  for (uint32_t axis = 0u; axis < 3u; ++axis) {
    std::array<uint32_t,3> groups{3u,2u,5u}; groups[axis] = 0u;
    Check(MaterializeResources(plan, WorkgroupSnapshotRuntime(reader,{},groups),snapshot,specialization) &&
              snapshot.flattened_srt.empty() && snapshot.immutable_srt_ranges.empty() &&
              specialization.bounded_srt_reads == std::vector<BoundedSrtLayout>{{0u,0u},{0u,0u}} &&
              reader.reads.empty(),
          "zero dispatch attempted to evaluate a table address or used another nonzero axis count");
  }
  Check(MaterializeResources(plan,WorkgroupSnapshotRuntime(reader,data,{32768u,32768u,1u}),snapshot,specialization) &&
            snapshot.flattened_srt.size() == 65536u && reader.reads.size() == 1u,
        "exact combined workgroup snapshot probe budget was rejected");
  const auto old_snapshot = snapshot;
  const auto old_specialization = specialization;
  Check(!MaterializeResources(plan,WorkgroupSnapshotRuntime(reader,data,{32769u,32768u,1u}),snapshot,specialization),
        "workgroup columns exceeded the combined 65536-probe budget");
  CheckBoundedTransaction(snapshot,old_snapshot,specialization,old_specialization);
}

void TestWorkgroupSrtWrappedOffsetsAndWriteAliases() {
  Fixture fixture;
  InitializeWorkgroupSnapshot(fixture,{0u});
  auto& column = fixture.program.bounded_srt_reads[0];
  column.offset_bias = 0xfffffffdu;
  column.memory_offset = 0xfffffffdu;
  auto plan = ExtractResourcePlan(fixture.program);
  BoundedSnapshotReader reader;
  reader.words = {{0x100000ff8ull,0xa1u},{0xffcull,0xb2u},{0x1000ull,0xc3u}};
  const std::array<uint32_t,2> data{0x1003u,0u};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan,WorkgroupSnapshotRuntime(reader,data,{3u,1u,1u}),snapshot,specialization) &&
            snapshot.flattened_srt == std::vector<uint32_t>{0xa1u,0xb2u,0xc3u} &&
            reader.reads == std::vector<uint64_t>{0x100000ff8ull,0xffcull,0x1000ull} &&
            snapshot.immutable_srt_ranges == std::vector<ResourceReadRange>{{0xffcu,8u},{0x100000ff8ull,4u}},
        "workgroup affine U32 wrap was combined with signed SMEM immediate or lost exact ranges");
  const auto old_snapshot = snapshot;
  const auto old_specialization = specialization;
  plan.bounded_srt_reads[0].offset_bias = 0u;
  const auto before = reader.reads.size();
  const std::array<uint32_t,2> underflow{0u,0u};
  Check(!MaterializeResources(plan,WorkgroupSnapshotRuntime(reader,underflow,{1u,1u,1u}),snapshot,specialization) &&
            reader.reads.size() == before, "negative SMEM immediate underflow reached the reader");
  CheckBoundedTransaction(snapshot,old_snapshot,specialization,old_specialization);
  plan.bounded_srt_reads[0].offset_bias = 8u;
  plan.bounded_srt_reads[0].memory_offset = 0u;
  const std::array<uint32_t,2> overflow{0xfffffffcu,0xffffu};
  Check(!MaterializeResources(plan,WorkgroupSnapshotRuntime(reader,overflow,{1u,1u,1u}),snapshot,specialization) &&
            reader.reads.size() == before, "48-bit workgroup address overflow reached the reader");
  CheckBoundedTransaction(snapshot,old_snapshot,specialization,old_specialization);
  for (const bool overlap : {false,true}) {
    Fixture alias;
    InitializeWorkgroupSnapshot(alias,{0u});
    const auto writer = AddBoundedSnapshotSource(alias,
        {Value(overlap ? 0x1007u : 0x1008u),Value(0u),Value(1u),Value(0u)});
    alias.program.info.buffers.push_back({.source=writer,.written=true});
    auto alias_plan = ExtractResourcePlan(alias.program);
    reader = {}; reader.words = {{0x1000u,0x11u},{0x1004u,0x22u}};
    const std::array<uint32_t,2> alias_data{0x1000u,0u};
    snapshot = old_snapshot; specialization = old_specialization;
    const bool accepted = MaterializeResources(alias_plan,
        WorkgroupSnapshotRuntime(reader,alias_data,{2u,1u,1u}),snapshot,specialization);
    Check(accepted != overlap, "last coefficient byte alias or exact-end writer was misclassified");
    if (overlap) CheckBoundedTransaction(snapshot,old_snapshot,specialization,old_specialization);
  }
}

// Run each unsafe-baseline probe in its own bounded external child process.
// The test never substitutes a callback for the production raw fallback.
class RawFallbackTestPage {
public:
  explicit RawFallbackTestPage(std::string_view mode) {
    if (mode == "null") return;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    size = info.dwPageSize;
    const DWORD allocation = mode == "reserved" ? MEM_RESERVE : MEM_RESERVE | MEM_COMMIT;
    const DWORD protection = mode == "readable" ? PAGE_READWRITE : PAGE_NOACCESS;
    data = VirtualAlloc(nullptr, size, allocation, protection);
    Check(data != nullptr, "raw fallback test could not allocate its host page");
    MEMORY_BASIC_INFORMATION region{};
    Check(VirtualQuery(data, &region, sizeof(region)) == sizeof(region),
          "raw fallback test could not verify its host page");
    Check(mode == "reserved" ? region.State == MEM_RESERVE : region.State == MEM_COMMIT,
          "raw fallback host page has an unexpected commitment state");
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
    const auto page_size = sysconf(_SC_PAGESIZE);
    Check(page_size > 0, "raw fallback test could not query the host page size");
    size = static_cast<size_t>(page_size);
    const int protection = mode == "readable" ? PROT_READ | PROT_WRITE : PROT_NONE;
    data = mmap(nullptr, size, protection, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(data != MAP_FAILED, "raw fallback test could not map its host page");
#else
    Check(false, "raw fallback test requires the Windows or Linux host readability implementation");
#endif
    Check(reinterpret_cast<uintptr_t>(data) <= 0x0000ffffffffffffull,
          "raw fallback test host allocation is outside the evaluator address width");
    if (mode == "readable") {
      const uint32_t literal = 0x13579bdfu;
      std::memcpy(data, &literal, sizeof(literal));
    }
  }
  ~RawFallbackTestPage() {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    if (data != nullptr) VirtualFree(data, 0, MEM_RELEASE);
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
    if (data != nullptr && data != MAP_FAILED) munmap(data, size);
#endif
  }
  RawFallbackTestPage(const RawFallbackTestPage&) = delete;
  RawFallbackTestPage& operator=(const RawFallbackTestPage&) = delete;
  void* data = nullptr;
  size_t size = 0;
};

void CheckSrtRawFallbackCase(std::string_view name) {
  const auto separator = name.find('-');
  Check(separator != std::string_view::npos, "raw fallback case has no resource kind");
  const auto kind = name.substr(0, separator);
  const auto mode = name.substr(separator + 1);
  Check((kind == "address" || kind == "buffer") &&
            (mode == "readable" || mode == "null" || mode == "reserved" || mode == "noaccess"),
        "unknown raw fallback case");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  // Baseline AV must be reported to the parent instead of opening crash UI.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
  RawFallbackTestPage page(mode);
  const auto address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(page.data));
  const Value low(static_cast<uint32_t>(address));
  const Value high(static_cast<uint32_t>(address >> 32));
  Fixture fixture;
  MemoryInfo memory;
  memory.kind = kind == "address" ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer;
  memory.planning_only = true;
  const auto handle = kind == "address" ? fixture.Address(low, high, 0x459u)
                                         : fixture.Buffer({low, high, Value(4u), Value(0u)}, 0x459u);
  const auto raw = kind == "address"
      ? fixture.Emit(ValueOpcode::LoadAddressU32,
                     {handle, Value(0u), Value(0u), Value(true)}, fixture.AddMemory(memory, 0x459u))
      : fixture.Emit(ValueOpcode::ReadConstBuffer,
                     {handle, Value(0u)}, fixture.AddMemory(memory, 0x459u));
  fixture.program.srt_plan_complete = true;
  fixture.program.resource_tracking_complete = true;
  fixture.program.srt_reads.push_back({raw, 0u});
  DescriptorSource source;
  source.dword_count = 2u;
  // A successful earlier word must not leak into caller output on a failed read.
  source.dwords[0] = Value(0x2468ace0u);
  source.dwords[1] = raw;
  fixture.program.descriptor_sources.push_back(source);
  const auto plan = ExtractResourcePlan(fixture.program);
  const SrtRuntime runtime{};
  Check(runtime.read_memory == nullptr && runtime.read_specialization_memory == nullptr,
        "raw fallback probe accidentally installed a memory reader");
  const uint32_t source_id = 0;
  std::vector<DescriptorValue> descriptors{{{0xfeed1111u, 0xfeed2222u}, 2u}};
  std::vector<uint32_t> flat{0xfeed3333u, 0xfeed4444u};
  const auto saved_descriptors = descriptors;
  const auto saved_flat = flat;
  std::cout << "KYTY_SRT_RAW_FALLBACK_READY " << name << std::endl;
  const bool accepted = EvaluateRuntimeSources(plan, std::span{&source_id, 1}, runtime,
                                               descriptors, flat, {});
  if (mode == "readable") {
    Check(accepted && descriptors.size() == 1 && descriptors[0].dword_count == 2u &&
              descriptors[0].dwords[0] == 0x2468ace0u &&
              descriptors[0].dwords[1] == 0x13579bdfu &&
              flat == std::vector<uint32_t>{0x13579bdfu},
          "raw fallback rejected or changed a readable literal host DWORD");
  } else {
    Check(!accepted, "raw fallback accepted unreadable host memory");
    Check(descriptors == saved_descriptors && flat == saved_flat,
          "raw fallback failed nontransactionally");
  }
  std::cout << "KYTY_SRT_RAW_FALLBACK_PASS " << name << std::endl;
}

void TestSrtRawFallbackReadability() {
  for (const auto* name : {"address-readable", "buffer-readable", "address-null", "buffer-null",
                           "address-reserved", "buffer-reserved", "address-noaccess", "buffer-noaccess"})
    CheckSrtRawFallbackCase(name);
}

} // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::strcmp(argv[1], "--conditional-scalar-address-only") == 0) {
      TestConditionalScalarAddressReadRemainsRuntime();
      std::cout << "KYTY_CONDITIONAL_SCALAR_ADDRESS_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--address-backed-indirect-only") == 0) {
      TestAddressBackedIndirectImages();
      TestSharedUniformLoopIndex();
      TestBufferRecordImageKey();
      std::cout << "KYTY_ADDRESS_BACKED_INDIRECT_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--inline-image-mixed-samplers-only") == 0) {
      TestInlineImageMixedDynamicAndOrdinarySamplers();
      std::cout << "KYTY_INLINE_IMAGE_MIXED_SAMPLERS_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--inline-image-sampler-pairs-only") == 0) {
      TestInlineImageUniformSamplers();
      TestInlineImageMixedDynamicAndOrdinarySamplers();
      std::cout << "KYTY_INLINE_IMAGE_SAMPLER_PAIRS_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--heterogeneous-indirect-images-only") == 0) {
      TestHeterogeneousIndirectImageDimensions();
      TestHeterogeneousIndirectImageViewSwizzles();
      std::cout << "KYTY_HETEROGENEOUS_INDIRECT_IMAGES_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--dispatcher-signed-buffer-loop-only") == 0) {
      TestDispatcherSignedBufferLoop();
      std::cout << "KYTY_DISPATCHER_SIGNED_BUFFER_LOOP_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--inline-buffer-table-only") == 0) {
      TestInlineBufferDescriptorTable();
      std::cout << "KYTY_INLINE_BUFFER_TABLE_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--finite-selector-srt-proof-only") == 0) {
      TestFiniteSelectorSrtProof();
      std::cout << "KYTY_FINITE_SELECTOR_SRT_PROOF_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--finite-selector-srt-materialization-only") == 0) {
      TestFiniteSelectorSrtMaterialization();
      std::cout << "KYTY_FINITE_SELECTOR_SRT_MATERIALIZATION_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--bounded-writer-alias-only") == 0) {
      TestBoundedMaterializationCandidatesAndRemap();
      TestBoundedMaterializationRejectsWritableAliases();
      TestBoundedMaterializationNullsForeignBufferSlots();
      std::cout << "KYTY_BOUNDED_WRITER_ALIAS_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--finite-selector-active-proof-only") == 0) {
      TestFiniteSelectorActiveMaskProof();
      std::cout << "KYTY_FINITE_SELECTOR_ACTIVE_PROOF_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--workgroup-srt-proof-only") == 0) {
      TestWorkgroupSrtTrackingProof();
      TestWorkgroupSrtRootExecutionProof();
      std::cout << "KYTY_WORKGROUP_SRT_PROOF_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--workgroup-srt-materialization-only") == 0) {
      TestWorkgroupSrtMaterializationAndSpecialization();
      TestWorkgroupSrtZeroDispatchAndProbeLimit();
      TestWorkgroupSrtWrappedOffsetsAndWriteAliases();
      std::cout << "KYTY_WORKGROUP_SRT_MATERIALIZATION_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--descriptor-format-provenance-only") == 0) {
      TestDescriptorFormattedBufferProvenance();
      std::cout << "KYTY_DESCRIPTOR_FORMAT_PROVENANCE_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--invalid-sampled-format-hole-only") == 0) {
      TestInvalidSampledFormatHoleCanonicalization();
      std::cout << "KYTY_INVALID_SAMPLED_FORMAT_HOLE_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--bounded-write-alias-only") == 0) {
      TestBoundedMaterializationRejectsWritableAliases();
      std::cout << "KYTY_BOUNDED_WRITE_ALIAS_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--bounded-unmapped-scalar-only") == 0) {
      TestBoundedMaterializationZerosUnmappedScalarRows();
      std::cout << "KYTY_BOUNDED_UNMAPPED_SCALAR_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--wave-uniform-buffer-phi-only") == 0) {
      TestWaveUniformBufferPhiTable();
      std::cout << "KYTY_WAVE_UNIFORM_BUFFER_PHI_PASS\n";
      return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--inline-image-address-table-only") == 0) {
      TestInlineImageAddressTable();
      std::cout << "KYTY_INLINE_IMAGE_ADDRESS_TABLE_PASS\n";
      return 0;
    }
    if (argc == 3 && std::strcmp(argv[1], "--srt-raw-fallback-case") == 0) {
      CheckSrtRawFallbackCase(argv[2]);
      return 0;
    }
    if (argc != 1) {
      std::cerr << "usage: resource_tracking_tests [--srt-raw-fallback-case CASE]\n";
      return 2;
    }
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
    Run("descriptor format provenance", TestDescriptorFormattedBufferProvenance);
    Run("runtime unsigned min", TestRuntimeUnsignedMinDescriptor);
    Run("images and samplers", TestImagesSamplersAndAliases);
    Run("inline image mixed samplers", TestInlineImageMixedDynamicAndOrdinarySamplers);
    Run("SampleAdjust sampler scratch", TestSampleAdjustSamplerScratch);
    Run("FMASK load specialization", TestFmaskLoadSpecialization);
    Run("dynamic storage mips", TestDynamicStorageMipTracking);
    Run("invariant indirect images", TestInvariantIndirectImageMaterialization);
    Run("shared uniform loop index", TestSharedUniformLoopIndex);
    Run("buffer record image key", TestBufferRecordImageKey);
    Run("guarded direct image table", TestGuardedDirectImageTable);
    Run("bounded compute image loop", TestBoundedComputeImageLoop);
    Run("uniformized material image keys", TestUniformizedMaterialImageKeys);
    Run("image descriptor fields", TestImageDescriptorFields);
    Run("draw-uniform scalar image", TestUniformScalarBufferImage);
    Run("SRT runtime", TestSrtFlatteningAndRuntimeMemoization);
    Run("raw fallback readability", TestSrtRawFallbackReadability);
    Run("dynamic SRT", TestDynamicSrtReadRemainsExplicit);
    Run("conditional scalar address read", TestConditionalScalarAddressReadRemainsRuntime);
    Run("finite selector SRT proof", TestFiniteSelectorSrtProof);
    Run("finite selector SRT materialization", TestFiniteSelectorSrtMaterialization);
    Run("finite selector active-mask proof", TestFiniteSelectorActiveMaskProof);
    Run("workgroup SRT proof", TestWorkgroupSrtTrackingProof);
    Run("workgroup SRT root execution", TestWorkgroupSrtRootExecutionProof);
    Run("workgroup SRT materialization", TestWorkgroupSrtMaterializationAndSpecialization);
    Run("workgroup SRT zero dispatch and limits", TestWorkgroupSrtZeroDispatchAndProbeLimit);
    Run("workgroup SRT offsets and aliases", TestWorkgroupSrtWrappedOffsetsAndWriteAliases);
    Run("bounded SRT tracking proof", TestBoundedSrtTrackingProofBoundaries);
    Run("bounded SRT split header", TestBoundedSrtSplitHeaderUniformCount);
    Run("bounded SRT shared memory count", TestBoundedSrtSplitHeaderSharedMemoryCount);
    Run("TestBoundedMaterializationAddressesAndSnapshot", TestBoundedMaterializationAddressesAndSnapshot);
    Run("TestBoundedMaterializationZerosUnmappedScalarRows",
        TestBoundedMaterializationZerosUnmappedScalarRows);
    Run("TestBoundedMaterializationCandidatesAndRemap", TestBoundedMaterializationCandidatesAndRemap);
    Run("TestBoundedMaterializationLimitsAreTransactional", TestBoundedMaterializationLimitsAreTransactional);
    Run("TestBoundedMaterializationRejectsWritableAliases", TestBoundedMaterializationRejectsWritableAliases);
    Run("TestBoundedMaterializationNullsForeignBufferSlots",
        TestBoundedMaterializationNullsForeignBufferSlots);
    Run("phi validation", TestPhiValidation);
    Run("conditional sampler phi", TestConditionalSamplerPhi);
    Run("guarded sampler phi", TestGuardedSamplerPhi);
    Run("runtime-rooted loop", TestLoopCycleEnteredThroughRuntimeValue);
    Run("invariant loop phi", TestInvariantLoopPhi);
    Run("DMA address materialization", TestDmaAddressMaterialization);
    Run("dynamic FLAT address", TestDynamicFlatAddressesUseDma);
    Run("buffer swizzle specialization", TestBufferSwizzleSpecialization);
    Run("conditional buffer materialization", TestConditionalBufferMaterialization);
    Run("conservative buffer reachability", TestConservativeBufferReachability);
    Run("conditional indirect image", TestConditionalIndirectImageMaterialization);
    Run("shader info and bindings", TestShaderInfoAndBindingLayout);
    Run("comparison binding isolation", TestComparisonBindingsAreIsolated);
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

namespace Libs::Graphics {
SurfaceFormatInfo TextureGetSurfaceFormatInfo(Prospero::BufferFormat format) {
  static_cast<void>(format);
  return SurfaceFormatInfo(vk::Format::eR32Sfloat,
                           Prospero::BufferFormat::kInvalid);
}
} // namespace Libs::Graphics

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.cpp"
