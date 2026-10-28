#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ConstantPropagation.h"
#include "graphics/shader/recompiler/ir/passes/DeadCodeElimination.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/ResourceTracking.h"
#include "graphics/shader/recompiler/ir/passes/ShaderInfoCollection.h"
#include "SrtWalkerTestMemory.h"

#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderComputeInputInfo;
using Libs::Graphics::ShaderType;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;

static_assert(std::is_constructible_v<Value, Inst*> &&
              std::is_constructible_v<Value, bool> &&
              !std::is_constructible_v<Value, const Inst*>);

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
  ShaderComputeInputInfo compute_info{};

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

  void PlanAndTrack() { PlanAndTrack({.compute = &compute_info}); }

  void PlanAndTrack(Libs::Graphics::ShaderStageInputInfo input) {
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
    TrackResources(program, {}, {}, input);
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
      (memory->fail_address >= address && memory->fail_address - address < values.size_bytes())) {
    return false;
  }
  std::memcpy(values.data(), reinterpret_cast<const uint8_t *>(memory->words.data()) +
                                (address - memory->base), values.size_bytes());
  ++memory->reads;
  if (values.size() == 8u) ++memory->descriptor_reads;
  if (memory->watched_address >= address &&
      memory->watched_address - address < values.size_bytes()) {
    ++memory->watched_reads;
    memory->watched_dwords = values.size();
  }
  return true;
}

// Serves walker loads from the memory a test runtime currently points at.
bool ReadRuntimeMemory(void *userdata, uint64_t address, std::span<uint32_t> values) {
  const auto &runtime = *static_cast<const SrtRuntime *>(userdata);
  return runtime.read_specialization_memory(runtime.userdata, address, values);
}

std::unique_ptr<Fixture>
MakeIndirectImageFixture(bool malformed, uint32_t table_stride = 32, uint32_t table_offset = 0,
                         uint32_t table_shader_offset = 0, bool nested = false,
                         ValueOpcode access = ValueOpcode::ImageSampleRaw, bool finite_key = false) {
  auto fixture = std::make_unique<Fixture>();
  std::array<Value, 4> material_words;
  std::array<Value, 4> heap_words;
  for (uint32_t dword = 0; dword < 4; dword++) {
    material_words[dword] = fixture->UserData(dword);
    heap_words[dword] = fixture->UserData(dword + 4u);
  }
  const auto material = fixture->Buffer(material_words, 0x10d8);
  const auto heap = finite_key ? fixture->Address(heap_words[0], heap_words[1], 0x10d8)
                               : fixture->Buffer(heap_words, 0x10d8);
  const auto invocation = fixture->Emit(
      ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(finite_key ? StageInputKind::WorkgroupId
                                              : StageInputKind::GlobalInvocationId)), Value(0u)});
  const auto selector = finite_key
      ? fixture->Emit(ValueOpcode::BitwiseXor32, {invocation, Value(1u)})
      : fixture->Emit(ValueOpcode::ReadFirstLane, {invocation, Value(true)});
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
  auto key = fixture->Emit(ValueOpcode::ReadConstBuffer, {material, member},
                           fixture->AddMemory(material_scalar, 0x10d8));
  if (finite_key) key = fixture->Emit(ValueOpcode::BitFieldUExtract,
                                     {key, Value(16u), Value(5u)});
  auto heap_offset = fixture->Emit(ValueOpcode::IMul32, {key, Value(table_stride)});
  if (table_shader_offset != 0u)
    heap_offset = fixture->Emit(ValueOpcode::IAdd32, {heap_offset, Value(table_shader_offset)});
  std::array<Value, 8> image_words;
  MemoryInfo heap_scalar;
  heap_scalar.kind = finite_key ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer;
  for (uint32_t dword = 0; dword < image_words.size(); dword++) {
    auto component = heap_scalar;
    component.offset = table_offset + dword * sizeof(uint32_t);
    if (malformed && dword == image_words.size() - 1u) {
      component.offset += sizeof(uint32_t);
    }
    const auto flags = fixture->AddMemory(component, 0x10d8);
    image_words[dword] = finite_key
        ? fixture->Emit(ValueOpcode::LoadAddressU32,
                        {heap, heap_offset, Value(0u), Value(true)}, flags)
        : fixture->Emit(ValueOpcode::ReadConstBuffer, {heap, heap_offset}, flags);
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
  const auto address = fixture->ImageAddress();
  const auto flags = fixture->AddMemory(sample, 0x10f0);
  Value result;
  if (access == ValueOpcode::ImageWrite) {
    const auto texel = fixture->Emit(ValueOpcode::CompositeConstructU32x4,
                                    {Value(1u), Value(2u), Value(3u), Value(4u)});
    fixture->Emit(access, {image, address, texel, Value(true)}, flags);
  } else if (access == ValueOpcode::ImageRead) {
    result = fixture->Emit(access, {image, address, Value(true)}, flags);
  } else if (access == ValueOpcode::ImageQueryDimensions) {
    result = fixture->Emit(access, {image, address}, flags);
  } else if (access == ValueOpcode::ImageAtomicIAdd32) {
    result = fixture->Emit(access, {image, address, Value(1u), Value(true)}, flags);
  } else {
    result = fixture->Emit(access, {image, sampler, address, Value(true)}, flags);
  }
  if (!result.IsEmpty()) {
    if (result.GetType() == Type::U32x4)
      result = fixture->Emit(ValueOpcode::CompositeExtractU32x4, {result, Value(0u)});
    fixture->Emit(ValueOpcode::ReferenceU32, {result});
  }
  return fixture;
}

void TestInvariantIndirectImageMaterialization(bool nested) {
  auto fixture = MakeIndirectImageFixture(false, 48u, 0u, 16u, nested);
  fixture->PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture->program);
  EliminateDeadCode(fixture->program.blocks);
  ValidateProgram(fixture->program, true);
  const auto &source = *plan.descriptor_sources[plan.info.images[0].source].indirect_descriptor;
  Check(!source.selector && fixture->program.info.buffers.size() == (nested ? 3u : 2u) &&
            std::ranges::any_of(*fixture->block, [](const Inst &inst) {
              return inst.GetOpcode() == ValueOpcode::ReadConstBuffer;
            }),
        "bounded image table retained CPU selector traversal or lost live loads");
  std::array<uint32_t, 8> descriptor{};
  descriptor[0] = 0x20u;
  descriptor[1] = static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
  descriptor[2] = 3u | (3u << 14u);
  descriptor[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  std::array<uint32_t, 16> data{0x3000u, 4u << 16u, 0x100000u, 0u,
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
  // A structurally valid but unsupported format at an unreachable DWORD start.
  const std::array<uint32_t, 8> unreachable{
      0x00fff000u, 0x09500000u, 0u, 0x900003acu, 0u, 0u, 0u, 0u};
  std::copy(unreachable.begin(), unreachable.end(), memory.words.begin() + 0x1064u / 4u);
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
  for (const uint32_t stride : {1u, 2u}) {
    auto subdword = MakeIndirectImageFixture(false, stride, 0u, 0u, nested);
    subdword->PlanAndTrack();
    const auto subdword_plan = ExtractResourcePlan(subdword->program);
    auto subdword_memory = memory;
    auto subdword_runtime = runtime;
    subdword_runtime.userdata = &subdword_memory;
    ResourceSnapshot subdword_snapshot;
    ResourceSpecialization subdword_specialization;
    Check(!MaterializeResources(subdword_plan, subdword_runtime, subdword_snapshot,
                               subdword_specialization) &&
              std::ranges::any_of(subdword_snapshot.images, [&](const auto &image) {
                return image.dwords == unreachable;
              }),
          "sub-DWORD affine stride skipped a reachable unsupported descriptor");
  }
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
    uint32_t offset = 0;
    Check(EvaluateSrtValue(wrap->program, runtime, handle->Arg(0), offset) && offset == expected,
          "GPU descriptor offset projection lost U32 wrap or scalar immediate carry");
  }
  auto malformed = MakeIndirectImageFixture(true);
  CheckFatal([&] { malformed->PlanAndTrack(); }, "not a valid runtime value",
             "noncontiguous descriptor DWORDs entered the table plan");
  Check(!malformed->program.resource_tracking_complete && malformed->program.descriptor_sources.empty(),
        "malformed image table tracking was not transactional");
}

void TestResidentImageHeap() {
  struct FakeImageHeap final : ImageHeapSource {
    bool accept = true;
    std::vector<DescriptorValue> kinds;
    std::vector<ImageHeapRequest> requests;
    bool Acquire(const ImageHeapRequest &request, ImageHeapView &view) override {
      requests.push_back(request);
      if (!accept) return false;
      view = {.slots = 0x123456789abcull, .slot_count = 36u, .kinds = kinds};
      return true;
    }
  };
  auto fixture = MakeIndirectImageFixture(false, 48u, 0u, 16u);
  fixture->PlanAndTrack();
  const auto plan = ExtractResourcePlan(fixture->program);
  std::array<uint32_t, 8> descriptor{};
  descriptor[0] = 0x20u;
  descriptor[1] = static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
  descriptor[2] = 3u | (3u << 14u);
  descriptor[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  auto cube = descriptor;
  cube[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
      (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kCube) << 28u);
  std::array<uint32_t, 16> data{0x3000u, 4u << 16u, 0x100000u, 0u,
                                0x2000u, 48u << 16u, 3u, 0u};
  std::copy_n(data.begin(), 4u, data.begin() + 12u);
  LinearTestMemory memory;
  memory.fail_address = 0x3000u;
  memory.watched_address = 0x2000u;
  std::copy(descriptor.begin(), descriptor.end(), memory.words.begin() + 0x1010u / 4u);
  FakeImageHeap heap;
  heap.kinds = {{descriptor, 8u}, {cube, 8u}};
  SrtRuntime runtime{.user_data = data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory, .image_heap = &heap};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            heap.requests.size() == 1u && heap.requests[0].base == 0x2000u &&
            heap.requests[0].size == 144u && heap.requests[0].step == 16u &&
            heap.requests[0].first == 0u && heap.requests[0].count == 9u &&
            heap.requests[0].root == &plan.info.images[0] &&
            memory.watched_reads == 0u && snapshot.images.size() == 3u,
        "resident image heap was materialized entry by entry");
  const auto &root = specialization.images[0];
  const auto mapping = root.indirect_mapping_offset;
  Check(root.heap && root.indirect_root == 0u && root.indirect_search_iterations == 0u &&
            snapshot.flattened_srt.size() == mapping + 3u &&
            snapshot.flattened_srt[mapping] == 36u &&
            snapshot.flattened_srt[mapping + 1u] == 0x56789abcu &&
            snapshot.flattened_srt[mapping + 2u] == 0x1234u &&
            specialization.images[1].heap && specialization.images[1].indirect_root == 0u &&
            !specialization.images[1].cube && specialization.images[2].heap &&
            specialization.images[2].cube && snapshot.images[2].dwords == cube &&
            std::ranges::find(snapshot.specialization_reads,
                              std::pair<uint64_t, uint64_t>{0x2000u, 144u}) !=
                snapshot.specialization_reads.end(),
        "resident image heap lost its slot mapping, typed kinds or read dependency");
  auto &program = fixture->program;
  ApplyResourceSpecialization(program, specialization);
  ShaderComputeInputInfo compute{};
  CollectShaderInfo(program, {.compute = &compute});
  Check(program.info.uses_image_heap && program.info.images.size() == 3u &&
            program.info.images[0].indirect_resources == std::vector<uint32_t>({0u, 1u, 2u}) &&
            std::ranges::all_of(program.info.images, &ImageResource::heap),
        "applied heap kinds lost their root or heap flag");
  heap.kinds.clear();
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 1u && !specialization.images[0].heap &&
            specialization.images[0].indirect_root == ImageResource::NoIndirectImage,
        "a resident heap without kinds did not read as one null image");
  heap.accept = false;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            heap.requests.size() == 3u && memory.watched_reads == 1u &&
            snapshot.images.size() == 2u && !specialization.images[0].heap &&
            snapshot.images[1].dwords == descriptor,
        "a declined heap was not materialized entry by entry");
}

void TestImageHeapKinds() {
  using Format = Libs::Graphics::Prospero::BufferFormat;
  using Type = Libs::Graphics::Prospero::ImageType;
  ImageResource root;
  root.resource_class = ImageResourceClass::Sampled;
  root.dimension = Decoder::ImageDimension::Dim2DArray;
  root.source = 3u;
  const auto descriptor = [](Format format, Type type) {
    DescriptorValue value{.dword_count = 8u};
    value.dwords[0] = 0x20u;
    value.dwords[1] = static_cast<uint32_t>(format) << 20u;
    value.dwords[2] = 3u | (3u << 14u);
    value.dwords[3] = Libs::Graphics::DstSel(4, 5, 6, 7) | (static_cast<uint32_t>(type) << 28u);
    return value;
  };
  const auto kind_of = [&](Format format, Type type) {
    auto value = descriptor(format, type);
    return ImageHeapKind(root, value);
  };
  const auto float_kind = kind_of(Format::k32_32_32_32Float, Type::kColor2DArray);
  const auto uint_kind = kind_of(Format::k32UInt, Type::kColor2DArray);
  const auto cube_kind = kind_of(Format::k32_32_32_32Float, Type::kCube);
  auto volume = descriptor(Format::k32_32_32_32Float, Type::kColor3D);
  Check(float_kind != 0u && uint_kind != 0u && cube_kind != 0u && float_kind != uint_kind &&
            float_kind != cube_kind && kind_of(Format::kInvalid, Type::kColor2DArray) == 0u &&
            ImageHeapKind(root, volume) == 0u &&
            std::ranges::all_of(volume.dwords, [](uint32_t word) { return word == 0u; }),
        "heap kinds did not separate typed entries or null mismatched ones");
  const auto resource = ImageHeapResource(root, uint_kind);
  const auto cube_resource = ImageHeapResource(root, cube_kind);
  auto other = root;
  other.source = 9u;
  Check(resource.numeric_class == Libs::Graphics::Prospero::TextureNumericClass::Uint &&
            !resource.cube && cube_resource.cube &&
            cube_resource.numeric_class == Libs::Graphics::Prospero::TextureNumericClass::Float &&
            resource.resource_class == ImageResourceClass::Sampled && resource.read &&
            ImageHeapResource(root, 0u) == ImageHeapResource(other, 0u),
        "heap kind resources lost their type or kept root identity");
  root.depth_compare = true;
  Check(kind_of(Format::k32UInt, Type::kColor2DArray) == 0u,
        "depth comparison accepted an integer heap entry");
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

  // CS127daf PC0x163c: an overlapping heap tuple is not a valid texture view.
  const std::array<uint32_t, 8> invalid_view{
      0x00fff000u, 0x09503fccu, 0u, 0x900672c0u, 0u, 0u, 0u, 0u};
  std::copy(invalid_view.begin(), invalid_view.end(), memory.words.begin() + 0x1040u / 4u);
  for (const bool r128 : {false, true}) {
    plan.info.images[0].r128 = r128;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == (r128 ? 3u : 4u) &&
              snapshot.flattened_srt[specialization.images[0].indirect_mapping_offset + 17u] == 0u &&
              snapshot.images[1].dwords == descriptor(0x20u, Type::kColor2D) &&
              snapshot.images.back().dwords == descriptor(0x24u, Type::kColor2D),
          "invalid heap texture view survived normalization or removed valid neighbors");
  }
  plan.info.images[0].r128 = false;

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
  CompileSrtWalker(plan);
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "explicitly selected incompatible descriptor was silently normalized to null");
}

void TestExactUnfilteredImageCandidates() {
  using F = Libs::Graphics::Prospero::BufferFormat;
  using T = Libs::Graphics::Prospero::ImageType;
  constexpr std::array<uint32_t, 8> placeholder{
      0x50091c00u, 0x03800000u, 0u, 0x80000000u, 0u, 0x00700000u, 0u, 0u};
  for (const auto access : {ValueOpcode::ImageRead, ValueOpcode::ImageWrite,
                           ValueOpcode::ImageSampleRaw, ValueOpcode::ImageQueryDimensions,
                           ValueOpcode::ImageAtomicIAdd32}) {
    auto fixture = MakeIndirectImageFixture(false, 32u, 0u, 0u, false, access, true);
    fixture->PlanAndTrack();
    auto plan = ExtractResourcePlan(fixture->program);
    const bool unfiltered = access == ValueOpcode::ImageRead || access == ValueOpcode::ImageWrite;
    Check(plan.info.images[0].unfiltered_access_only == unfiltered,
          "image access classification admitted a sample, query or atomic");
    const auto &source = *plan.descriptor_sources[plan.info.images[0].source].indirect_descriptor;
    Check(source.key_count.Resolve() == Value(32u),
          "five-bit image selector did not preserve its exact 32-row domain");
    LinearTestMemory memory;
    memory.fail_address = 0x3000u; // Selector payload stays on the GPU.
    auto first = placeholder;
    first[0] += 1u;
    first[3] = Libs::Graphics::DstSel(4, 5, 6, 7) | (uint32_t(T::kColor2D) << 28u);
    auto second = placeholder;
    if (access == ValueOpcode::ImageAtomicIAdd32) {
      first[1] = second[1] = uint32_t(F::k32UInt) << 20u;
      second[3] |= Libs::Graphics::DstSel(4, 5, 6, 7);
    }
    const auto fill = [&] {
      for (uint32_t row = 0; row < 32u; ++row) {
        const auto &words = row == 31u ? second : first;
        std::copy(words.begin(), words.end(), memory.words.begin() + 0x1000u / 4u + row * 8u);
      }
    };
    fill();
    std::array<uint32_t, 8> data{0x3000u, 4u << 16u, 32u, 0u,
                                0x2000u, 0u, 0u, 0u};
    const SrtRuntime runtime{.user_data = data, .userdata = &memory,
                             .read_specialization_memory = ReadLinearTestMemory};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) == unfiltered,
          "mixed 1D/2D candidates violated the all-uses access guard");
    if (!unfiltered) continue;
    Check(snapshot.images.size() == 2u && snapshot.images[1].dwords == placeholder &&
              specialization.images[0].dimension == Decoder::ImageDimension::Dim2D &&
              specialization.images[1].dimension == Decoder::ImageDimension::Dim1D,
          "a real 1D zero-swizzle descriptor was filtered, nulled or retyped");
    if (access == ValueOpcode::ImageWrite)
      Check(specialization.images[1].shader_swizzle == 0u,
            "storage placeholder lost its descriptor swizzle");
    const auto operation = std::ranges::find_if(*fixture->block, [&](const Inst &inst) {
      return inst.GetOpcode() == access;
    });
    const auto key = operation->Arg(0).ResolveInstruction()->Arg(0);
    ApplyResourceSpecialization(fixture->program, specialization);
    const auto &images = fixture->program.info.images;
    const auto &selected = fixture->program.memory_info[operation->Flags<MemoryFlags>().index];
    Check(images.size() == 2u && selected.resource == 0u &&
              selected.image_dimension == Decoder::ImageDimension::Dim2D &&
              images[0].indirect_resources == std::vector<uint32_t>({0u, 1u}) &&
              images[1].indirect_root == 0u &&
              images[1].dimension == Decoder::ImageDimension::Dim1D &&
              images[1].unfiltered_access_only &&
              operation->Arg(0).ResolveInstruction()->Arg(0) == key &&
              (access != ValueOpcode::ImageWrite || images[1].shader_swizzle == 0u),
          "applying mixed candidates lost GPU selection, typed child or storage swizzle");
    second[0] += 2u;
    fill();
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images[1].dwords[0] == second[0],
          "mixed-dimension materialization reused a stale descriptor");
    plan.info.images[0].dimension = Decoder::ImageDimension::Dim1D;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization),
          "encoded 1D payload was treated as having a second coordinate");
    plan.info.images[0].dimension = Decoder::ImageDimension::Dim2D;
    second[1] = uint32_t(F::k11_11_10UInt) << 20u;
    fill();
    Check(!MaterializeResources(plan, runtime, snapshot, specialization),
          "mixed-dimension acceptance discarded conversion compatibility");
    if (access == ValueOpcode::ImageRead) {
      first[1] = second[1];
      fill();
      Check(!MaterializeResources(plan, runtime, snapshot, specialization),
            "sampled converted candidates accepted different shader swizzles");
      first[1] = placeholder[1];
    }
    second[1] = placeholder[1];
    second[3] |= 1u << 16u;
    fill();
    plan.info.images[0].mip_mode = ImageMipMode::Dynamic;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              specialization.images[0].mip_count == 1u && specialization.images[1].mip_count == 2u,
          "mixed-dimension candidates lost their individual mip counts");
  }
  // The same image may be merged from several uses; one query keeps the guard strict.
  auto merged = MakeIndirectImageFixture(false, 32u, 0u, 0u, false, ValueOpcode::ImageRead, true);
  const auto handle = std::ranges::find_if(*merged->block, [](const Inst &inst) {
    return inst.GetOpcode() == ValueOpcode::GetImageResource;
  });
  merged->Emit(ValueOpcode::ImageQueryDimensions, {Value(&*handle), merged->ImageAddress()},
      merged->AddMemory({.kind = ResourceKind::Image,
                         .image_dimension = Decoder::ImageDimension::Dim2D}, 0x1100));
  merged->PlanAndTrack();
  Check(merged->program.info.images.size() == 1u &&
            !merged->program.info.images[0].unfiltered_access_only,
        "a later merged query left the unfiltered-use proof intact");
}

void TestWaterfallImageTable() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Sources { ScalarFirst, IndexedFirst, DistinctScalars };
  const auto make = [](bool equality, bool scalar, Sources sources = Sources::ScalarFirst) {
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
          {image, sampler, fixture->ImageAddress(), block == entry ? active : guard},
          fixture->AddMemory(memory, 0x92cu), block);
      fixture->Emit(ValueOpcode::ReferenceU32,
          {fixture->Emit(ValueOpcode::CompositeExtractU32x4, {value, Value(0u)}, 0, block)}, 0, block);
    };
    Value delayed_scalar;
    if (scalar) {
      const auto key = fixture->Emit(ValueOpcode::ReadFirstLane, {lane_value, active});
      for (const uint32_t member : {16u, 20u}) {
        if (member == 20u && sources != Sources::DistinctScalars) break;
        const auto offset = fixture->Emit(ValueOpcode::IAdd32,
            {fixture->Emit(ValueOpcode::IMul32, {key, Value(48u)}), Value(member)});
        std::array<Value, 8> scalar_words;
        for (uint32_t word = 0; word < 8u; ++word) {
          MemoryInfo memory;
          memory.kind = ResourceKind::ScalarBuffer;
          memory.offset = word * 4u;
          scalar_words[word] = fixture->Emit(ValueOpcode::ReadConstBuffer, {table, offset},
                                             fixture->AddMemory(memory, 0x7bcu));
        }
        const auto scalar_image = fixture->Image(scalar_words);
        if (sources == Sources::IndexedFirst) delayed_scalar = scalar_image;
        else sample(scalar_image, entry);
      }
    }
    if (sources != Sources::DistinctScalars) {
      const auto image = fixture->Emit(ValueOpcode::GetImageResource,
          {image_words[0], image_words[1], image_words[2], image_words[3],
           image_words[4], image_words[5], image_words[6], image_words[7]}, 0, body);
      sample(image, body);
    }
    if (!delayed_scalar.IsEmpty()) sample(delayed_scalar, body);
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
  auto reversed = make(true, true, Sources::IndexedFirst);
  reversed->PlanAndTrack();
  const auto reversed_plan = ExtractResourcePlan(reversed->program);
  Check(reversed_plan.info.images.size() == 1u,
        "indexed-first image table did not share its scalar use");
  const auto &shared = *reversed_plan.descriptor_sources[
      reversed_plan.info.images[0].source].indirect_descriptor;
  Check(shared.table_scalar && shared.table_record_bytes == 48u &&
            shared.table_stride == 0u && shared.table_offset == 0u && shared.table_immediate == 0u,
        "indexed-first union lost scalar access or the unrestricted byte domain");
  const auto previous_reads = memory.reads;
  ResourceSnapshot reversed_snapshot;
  ResourceSpecialization reversed_specialization;
  Check(MaterializeResources(reversed_plan, runtime, reversed_snapshot, reversed_specialization) &&
            memory.reads == previous_reads + 1u && reversed_snapshot.images == snapshot.images,
        "indexed-first table repeated its bulk read or changed descriptor candidates");
  auto distinct = make(true, true, Sources::DistinctScalars);
  distinct->PlanAndTrack();
  const auto distinct_plan = ExtractResourcePlan(distinct->program);
  Check(distinct_plan.info.images.size() == 2u,
        "distinct scalar image domains were merged into an unrestricted heap");
  for (uint32_t image = 0; image < 2u; ++image) {
    const auto &restricted = *distinct_plan.descriptor_sources[
        distinct_plan.info.images[image].source].indirect_descriptor;
    Check(restricted.table_record_bytes == 0u && restricted.table_stride == 48u &&
              restricted.table_offset == 16u + image * 4u && restricted.table_immediate == 0u,
          "scalar image union discarded a proven affine residue");
  }
  const auto handle = std::ranges::find_if(*fixture->program.blocks[1], [](const Inst &inst) {
    return inst.GetOpcode() == ValueOpcode::GetImageResource;
  });
  for (const auto [index, format, expected] :
       {std::array{1u, 0x5204u, 64u}, std::array{2u, 0x5204u, UINT32_MAX},
        std::array{UINT32_MAX, 0x5204u, UINT32_MAX}, std::array{0u, 0u, UINT32_MAX}}) {
    data[4] = index;
    data[3] = format;
    uint32_t offset = 0;
    Check(EvaluateSrtValue(fixture->program, runtime, handle->Arg(0), offset) && offset == expected,
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

void TestNestedPointerImageTable() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Address { Shared, Equivalent, ImageMismatch, SamplerMismatch };
  const auto make = [](uint32_t variant = 0u, Address choice = Address::Shared) {
    auto fixture = std::make_unique<Fixture>();
    std::array<Value, 4> table_words, key_words;
    for (uint32_t word = 0; word < 4u; ++word) {
      table_words[word] = fixture->UserData(word);
      key_words[word] = fixture->UserData(word + 4u);
    }
    const auto table = fixture->Buffer(table_words);
    const auto keys = fixture->Buffer(key_words);
    Inst* carried = nullptr;
    auto* entry = fixture->block;
    if (variant == 6u) {
      auto* loop = fixture->AddBlock();
      auto* exit = fixture->AddBlock();
      entry->AddBranch(loop);
      entry->terminator = {.kind = CFG::TerminatorKind::Branch,
                           .true_block = loop};
      loop->AddBranch(loop);
      loop->AddBranch(exit);
      loop->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                          .true_block = loop, .false_block = exit};
      exit->terminator.kind = CFG::TerminatorKind::Return;
      fixture->block = loop;
      carried = &loop->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    }
    const auto active = fixture->Emit(ValueOpcode::INotEqual32,
        {fixture->Emit(ValueOpcode::LaneId), Value(0u)});
    MemoryInfo key_memory;
    key_memory.kind = ResourceKind::Buffer;
    key_memory.offen = true;
    auto key = fixture->Emit(ValueOpcode::LoadBufferU32,
        {keys, Value(0u), fixture->Emit(ValueOpcode::LaneId), Value(0u), Value(true)},
        fixture->AddMemory(key_memory, 0x100u));
    if (carried != nullptr) {
      carried->AddPhiOperand(entry, Value(0u));
      carried->AddPhiOperand(fixture->block,
          fixture->Emit(ValueOpcode::SelectU32, {active, key, Value(carried)}));
      fixture->block->condition = active;
      key = Value(carried);
    }
    const auto unbounded_key = key;
    if (variant != 1u)
      key = fixture->Emit(ValueOpcode::ShiftRightLogical32, {key, Value(variant == 4u ? 15u : 16u)});
    if (variant == 5u || variant == 6u)
      key = fixture->Emit(ValueOpcode::SelectU32,
          {active, key, variant == 5u ? unbounded_key : Value(0u)});
    key = fixture->Emit(ValueOpcode::ReadLane, {key, Value(0u)});
    const auto offset = fixture->Emit(ValueOpcode::IMul32, {key, Value(592u)});
    std::array<Value, 2> pointer;
    for (uint32_t word = 0; word < 2u; ++word) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarBuffer;
      memory.offset = word * (variant == 2u ? 8u : 4u);
      pointer[word] = fixture->Emit(ValueOpcode::ReadConstBuffer, {table, offset},
                                    fixture->AddMemory(memory, 0x1f4u));
    }
    const auto address = fixture->Address(pointer[0], pointer[1]);
    auto other_address = address;
    if (choice == Address::Equivalent) {
      other_address = fixture->Address(pointer[0], pointer[1]);
    } else if (choice != Address::Shared) {
      for (uint32_t word = 0; word < 2u; ++word) {
        MemoryInfo memory;
        memory.kind = ResourceKind::ScalarBuffer;
        memory.offset = 8u + word * 4u;
        pointer[word] = fixture->Emit(ValueOpcode::ReadConstBuffer, {table, offset},
                                      fixture->AddMemory(memory, 0x1fcu));
      }
      other_address = fixture->Address(pointer[0], pointer[1]);
    }
    const auto load = [&](uint32_t first, uint32_t width) {
      if (width == 8u && variant >= 7u && variant <= 9u)
        first = variant == 7u ? 0x4000u : variant == 8u ? 0x3ff0u : 0x3fe0u;
      if (width == 4u && variant >= 9u) first = variant == 9u ? 0x3ff0u : 0x4000u;
      std::array<Value, 8> words;
      for (uint32_t word = 0; word < width; ++word) {
        MemoryInfo memory;
        memory.kind = ResourceKind::ScalarAddress;
        memory.offset = first + word * 4u;
        if (variant == 3u && width == 8u && word == 7u) memory.offset += 4u;
        const bool alternate = word == 1u &&
            (choice == Address::Equivalent ||
             (choice == Address::ImageMismatch && width == 8u) ||
             (choice == Address::SamplerMismatch && width == 4u));
        words[word] = fixture->Emit(ValueOpcode::LoadAddressU32,
            {alternate ? other_address : address, Value(0u), Value(0u), Value(true)},
            fixture->AddMemory(memory, first == 0x3d0u ? 0x1110u : 0x1118u));
      }
      return words;
    };
    const auto image = fixture->Image(load(0x3d0u, 8u));
    const auto sampler_words = load(0x3f8u, 4u);
    const auto sampler = fixture->Sampler(
        {sampler_words[0], sampler_words[1], sampler_words[2], sampler_words[3]});
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture->Emit(ValueOpcode::ImageSampleRaw,
        {image, sampler, fixture->ImageAddress(), variant == 5u ? active : Value(true)},
        fixture->AddMemory(memory, 0x1128u));
    return fixture;
  };
  auto fixture = make();
  fixture->PlanAndTrack();
  const auto plan = ExtractResourcePlan(fixture->program);
  Check(plan.info.images.size() == 1u && plan.info.samplers.size() == 1u,
        "nested pointer descriptors were expanded before their runtime table was available");
  for (const auto source : {plan.info.images[0].source, plan.info.samplers[0].source}) {
    const auto& descriptor = plan.descriptor_sources[source];
    const auto& indirect = descriptor.indirect_descriptor;
    Check(indirect && !indirect->selector && indirect->table_maximum == 65535u &&
              indirect->table_stride == 592u &&
              indirect->pointer_table && indirect->pointer_table->count == 1u &&
              indirect->pointer_table->stride == 0u && !indirect->pointer_table->indexed &&
              indirect->pointer_table->offset == (descriptor.dword_count == 8u ? 0x3d0u : 0x3f8u),
          "nested pointer descriptors lost their row geometry or captured the GPU selector payload");
  }

  constexpr uint32_t table = 0x2000u, objects = 0x10000u, stride = 592u, count = 59u;
  LinearTestMemory memory;
  memory.words.resize((objects + count * 0x430u - memory.base) / 4u);
  memory.fail_address = 0x1800u; // The GPU selector's contents are unavailable to the CPU.
  const auto at = [&](uint32_t address) { return memory.words.begin() + (address - memory.base) / 4u; };
  for (uint32_t row = 0; row < count; ++row) {
    const auto object = objects + row * 0x430u;
    *at(table + row * stride) = object;
    const std::array<uint32_t, 8> image{10u + row,
        uint32_t(row == count - 1u ? Libs::Graphics::Prospero::BufferFormat::k32UInt
                                  : Libs::Graphics::Prospero::BufferFormat::k32Float) << 20u, 0u,
        Libs::Graphics::DstSel(4, 0, 0, 1) | (9u << 28u), 0u, 0u, 0u, 0u};
    std::copy(image.begin(), image.end(), at(object + 0x3d0u));
    const std::array<uint32_t, 4> sampler{row, 0x00fff000u, 0u, 0u};
    std::copy(sampler.begin(), sampler.end(), at(object + 0x3f8u));
  }
  std::array<uint32_t, 8> user_data{table, stride << 16u, count, 0x5204u,
                                 0x1800u, 0u, 4u, 0x30005204u};
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                           .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == count + 1u && snapshot.samplers.size() == count + 1u,
        "bounded nested pointer table lost image/sampler candidates or read the GPU selector");
  for (uint32_t row = 0; row < count; ++row)
    Check(snapshot.images[row + 1u].dwords[0] == 10u + row &&
              snapshot.samplers[row + 1u].dwords[0] == row,
          "nested pointer table mixed rows or pointee members");
  auto typed = make();
  typed->PlanAndTrack();
  ApplyResourceSpecialization(typed->program, specialization);
  const auto& samplers = typed->program.info.samplers;
  Check(samplers.size() == 2u * (count + 1u) && samplers[0].indirect_resources.size() == count + 1u &&
            samplers[count + 1u].indirect_resources.size() == count + 1u &&
            snapshot.samplers[0].dwords[1] == 0u,
        "expanded sampler table lost its null root or numeric-class variants");
  for (uint32_t index = 0; index <= count; ++index) {
    const auto& integer = samplers[index + count + 1u];
    Check(samplers[index].snapshot_index == index && !samplers[index].integer_border &&
              integer.snapshot_index == index && integer.integer_border && integer.force_point_filtering &&
              samplers[count + 1u].indirect_resources[index] == index + count + 1u,
          "large sampler class mapping changed descriptor identity or integer filtering");
  }
  const auto populated = specialization;
  auto equivalent = make(0u, Address::Equivalent);
  equivalent->PlanAndTrack();
  Check(MaterializeResources(ExtractResourcePlan(equivalent->program), runtime, snapshot, specialization) &&
            specialization == populated,
        "equivalent nested pointer address instructions changed descriptor selection");
  for (const auto choice : {Address::ImageMismatch, Address::SamplerMismatch}) {
    auto mismatched = make(0u, choice);
    CheckFatal([&] { mismatched->PlanAndTrack(); }, "not a valid runtime value",
        "nested pointer planning combined descriptor words from different pointer pairs");
  }
  const auto reads = memory.reads;
  user_data[2] = 0u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 1u && snapshot.samplers.size() == 1u &&
            memory.reads == reads && specialization != populated,
        "empty nested pointer table retained candidates or read an unavailable row");
  user_data[2] = count;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            specialization == populated,
        "empty-to-positive pointer table refresh did not restore its specialization");
  *at(table) = objects + 0x430u;
  *at(objects + 0x430u + 0x3d0u) = 10u + count;
  *at(objects + 0x430u + 0x3f8u) = count;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == count && snapshot.samplers.size() == count &&
            snapshot.images[1].dwords[0] == 10u + count && snapshot.samplers[1].dwords[0] == count,
        "nested descriptor refresh retained stale pointers or failed to deduplicate new pointees");
  *at(table) = objects;

  // A final low DWORD is in bounds; the high pointer DWORD is independently zero-filled.
  user_data[1] = 1u << 16u;
  user_data[2] = count * stride + 4u;
  memory.watched_address = table + count * stride;
  *at(static_cast<uint32_t>(memory.watched_address)) = objects;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            memory.watched_reads != 0u && memory.watched_dwords == 1u &&
            snapshot.images.size() == count + 1u && snapshot.samplers.size() == count + 1u,
        "partial pointer row was skipped or read beyond its scalar buffer bounds");
  const auto mapping = specialization.images[0].indirect_mapping_offset;
  Check(snapshot.flattened_srt[mapping] == count + 1u &&
            snapshot.flattened_srt[mapping + count + 1u] == 1u,
        "partial pointer row lost its low-word pointee or dense key mapping");
  user_data[1] = stride << 16u;
  user_data[2] = count;
  memory.fail_address = objects + 0x3d0u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "failed nested image pointee read was treated as a null descriptor");
  memory.fail_address = objects + 0x3f8u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "failed nested sampler pointee read was treated as a null descriptor");
  memory.fail_address = 0x1800u;
  *at(table) = 0u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "unreadable null pointer row bypassed the specialization reader");
  *at(table) = objects;

  auto capped = make(4u);
  capped->PlanAndTrack();
  const auto capped_plan = ExtractResourcePlan(capped->program);
  user_data[2] = 65537u;
  const auto before_cap = memory.reads;
  Check(!MaterializeResources(capped_plan, runtime, snapshot, specialization) &&
            memory.reads == before_cap,
        "oversized nested pointer table performed reads before enforcing its candidate cap");
  user_data[2] = count;
  auto cyclic = make(6u);
  cyclic->PlanAndTrack();
  Check(MaterializeResources(ExtractResourcePlan(cyclic->program), runtime, snapshot, specialization),
        "context-free shift bound rejected a cyclic source or a bounded Select arm");
  auto boundary = make(9u);
  boundary->PlanAndTrack(); // Both descriptor spans end exactly at the unmappable first-page boundary.
  for (const uint32_t variant : {1u, 2u, 3u, 5u, 7u, 8u, 10u}) {
    auto invalid = make(variant);
    CheckFatal([&] { invalid->PlanAndTrack(); }, "not a valid runtime value",
        "nested pointer planning accepted wrapping keys, mismatched words, lane-local bounds or mappable null-pointee bytes");
  }
}

void TestIndexedPointerImageTable() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Case { Valid, ReverseSigned, MissingParent, WrongParent, MissingChild,
                    InclusiveChild, MismatchedPair, DifferentLaneMask, ExtraUse,
                    UnboundedChoice, MissingOuterWitness, WrongOuterWitness, CyclicChoice,
                    ExcessiveDomain, Counted, MissingCount, DifferentCount, MaskedCount, UnprovenMask };
  const auto make = [](Case variant = Case::Valid) {
    auto fixture = std::make_unique<Fixture>();
    std::array<Value, 4> table_words;
    for (uint32_t word = 0; word < 4u; ++word) table_words[word] = fixture->UserData(word);
    const auto table = fixture->Buffer(table_words);
    const auto lane = fixture->Emit(ValueOpcode::LaneId);
    const bool counted = variant >= Case::Counted;
    Value choice, row, count, other_count;
    if (counted) {
      const auto address = fixture->Address(fixture->UserData(4), fixture->UserData(5));
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      count = fixture->Emit(ValueOpcode::LoadAddressU32,
          {address, Value(0u), Value(0u), Value(true)}, fixture->AddMemory(memory, 0x8u));
      if (variant == Case::DifferentCount) {
        memory.offset = 4u;
        other_count = fixture->Emit(ValueOpcode::LoadAddressU32,
            {address, Value(0u), Value(0u), Value(true)}, fixture->AddMemory(memory, 0x10u));
      }
      choice = fixture->Emit(ValueOpcode::ULessThan32, {Value(0u), count});
    } else {
      choice = fixture->Emit(ValueOpcode::ULessThan32, {lane, Value(8u)});
      const auto valid = fixture->Emit(ValueOpcode::ULessThan32, {lane, Value(16u)});
      row = fixture->Emit(ValueOpcode::SelectU32,
          {valid, fixture->Emit(ValueOpcode::SelectU32,
               {choice, Value(0u), variant == Case::UnboundedChoice ? lane : Value(1u)}),
           Value(UINT32_MAX)});
    }
    auto* entry = fixture->block;
    auto* left = fixture->AddBlock();
    auto* right = fixture->AddBlock();
    auto* merge = fixture->AddBlock();
    entry->AddBranch(left);
    entry->AddBranch(right);
    entry->condition = choice;
    entry->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                        .true_block = left, .false_block = right};
    for (auto* from : {left, right}) {
      from->AddBranch(merge);
      from->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = merge};
    }
    if (counted) {
      fixture->block = left;
      const auto selector = fixture->Emit(ValueOpcode::BitwiseAnd32, {lane, Value(3u)});
      row = Value(UINT32_MAX);
      for (uint32_t key = 4u; key-- > 0u;) {
        auto active = fixture->Emit(ValueOpcode::IEqual32, {selector, Value(key)});
        // Row zero uses the uniform CFG-entry bound, like the captured shader.
        if (key != 0u && !(variant == Case::MissingCount && key == 2u)) {
          const auto bound = variant == Case::DifferentCount && key == 2u ? other_count : count;
          active = fixture->Emit(ValueOpcode::LogicalAnd,
              {active, fixture->Emit(ValueOpcode::ULessThan32, {Value(key), bound})});
        }
        row = fixture->Emit(ValueOpcode::SelectU32, {active, Value(key), row});
      }
    }
    fixture->block = merge;
    auto& phi = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    phi.AddPhiOperand(left, row);
    phi.AddPhiOperand(right, counted ? Value(UINT32_MAX) : row);
    row = Value(&phi);
    if (variant == Case::CyclicChoice) {
      merge->AddBranch(merge);
      merge->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = merge};
      phi.AddPhiOperand(merge, row);
    } else merge->terminator.kind = CFG::TerminatorKind::Return;
    Value selected_mask;
    if (variant == Case::MaskedCount || variant == Case::UnprovenMask) {
      selected_mask = fixture->Emit(ValueOpcode::ULessThan32, {lane, Value(2u)});
      row = fixture->Emit(ValueOpcode::SelectU32, {selected_mask, row, Value(0u)});
    }
    auto parent = variant == Case::MissingParent ? Value(true)
        : variant == Case::ReverseSigned
            ? fixture->Emit(ValueOpcode::SGreaterThanEqual32, {row, Value(0u)})
            : fixture->Emit(variant == Case::WrongParent ? ValueOpcode::ULessThanEqual32
                                                       : ValueOpcode::SLessThanEqual32,
                            {Value(0u), row});
    if (variant == Case::MaskedCount)
      parent = fixture->Emit(ValueOpcode::LogicalAnd, {selected_mask, parent});
    MemoryInfo pointer_memory;
    pointer_memory.kind = ResourceKind::Buffer;
    pointer_memory.idxen = true;
    pointer_memory.data_dwords = pointer_memory.component_count = 2u;
    const auto load = fixture->Emit(ValueOpcode::LoadBufferU32x2,
        {table, row, Value(0u), Value(0u), parent},
        fixture->AddMemory(pointer_memory, 0x3d4u));
    const auto other = variant == Case::MismatchedPair
        ? fixture->Emit(ValueOpcode::LoadBufferU32x2,
            {table, Value(0u), Value(0u), Value(0u), parent},
            fixture->AddMemory(pointer_memory, 0x3dcu)) : load;
    const auto low = fixture->Emit(ValueOpcode::SelectU32,
        {parent, fixture->Emit(ValueOpcode::CompositeExtractU32x2, {load, Value(0u)}), Value(0u)});
    const auto high = fixture->Emit(ValueOpcode::SelectU32,
        {parent, fixture->Emit(ValueOpcode::CompositeExtractU32x2, {other, Value(1u)}), Value(0u)});
    const auto child = fixture->Emit(ValueOpcode::ReadFirstLane, {lane, parent});
    const auto range = variant == Case::MissingChild ? Value(true)
        : fixture->Emit(variant == Case::InclusiveChild ? ValueOpcode::ULessThanEqual32
                                                      : ValueOpcode::ULessThan32,
                        {child, Value(variant == Case::ExcessiveDomain ? MaxIndirectDescriptorProbes : 3u)});
    const auto active = fixture->Emit(ValueOpcode::LogicalAnd, {parent, range});
    const auto extended = fixture->Emit(ValueOpcode::SelectU32,
        {active, fixture->Emit(ValueOpcode::BitFieldSExtract, {high, Value(0u), Value(16u)}), Value(0u)});
    const auto uniform_low = fixture->Emit(ValueOpcode::ReadFirstLane, {low, active});
    const auto uniform_high = fixture->Emit(ValueOpcode::ReadFirstLane,
        {extended, variant == Case::DifferentLaneMask
                       ? fixture->Emit(ValueOpcode::LogicalNot, {choice}) : active});
    const auto uniform_row = fixture->Emit(ValueOpcode::ReadFirstLane, {row, active});
    const auto row_equal = fixture->Emit(ValueOpcode::IEqual32,
        {uniform_row, variant == Case::WrongOuterWitness ? lane : row});
    const auto executing = variant == Case::MissingOuterWitness ? active
        : fixture->Emit(ValueOpcode::LogicalAnd, {active, row_equal});
    const auto address = fixture->Address(uniform_low, uniform_high);
    const auto offset = fixture->Emit(ValueOpcode::IMul32, {child, Value(368u)});
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < 8u; ++word) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = 64u + word * 4u;
      words[word] = fixture->Emit(ValueOpcode::LoadAddressU32,
          {address, offset, Value(0u), Value(true)}, fixture->AddMemory(memory, 0x898u));
    }
    const auto image = fixture->Image(words);
    const auto sampler = fixture->Sampler({Value(0u), Value(0x00fff000u), Value(0u), Value(0u)});
    MemoryInfo image_memory;
    image_memory.kind = ResourceKind::Image;
    image_memory.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture->Emit(ValueOpcode::ImageSampleRaw,
        {image, sampler, fixture->ImageAddress(), executing}, fixture->AddMemory(image_memory, 0x8b4u));
    if (variant == Case::ExtraUse)
      fixture->Emit(ValueOpcode::ImageSampleRaw,
          {image, sampler, fixture->ImageAddress(), parent}, fixture->AddMemory(image_memory, 0x8bcu));
    return fixture;
  };
  const auto nested_geometry = [](const auto& plan, uint32_t count) {
    const auto& indirect = *plan.descriptor_sources[plan.info.images[0].source].indirect_descriptor;
    return indirect.table_maximum == 1u && indirect.pointer_table &&
           indirect.pointer_table->indexed && indirect.pointer_table->count == count &&
           indirect.pointer_table->stride == 368u && indirect.pointer_table->offset == 64u;
  };
  auto fixture = make();
  fixture->PlanAndTrack();
  const auto plan = ExtractResourcePlan(fixture->program);
  Check(plan.info.images.size() == 1u && plan.info.buffers.size() == 1u,
        "indexed nested table expanded IR candidates or discarded the GPU pointer load");
  Check(nested_geometry(plan, 3u),
        "nested table did not preserve separate parent and child geometry");
  constexpr uint64_t high = uint64_t{1} << 32u;
  constexpr uint32_t table = 0x2000u, objects = 0x10000u, row_bytes = 1536u;
  LinearTestMemory memory;
  memory.base += high;
  memory.words.resize((objects + 4u * row_bytes - 0x1000u) / 4u, 0xdeadbeefu);
  const auto at = [&](uint32_t address) { return memory.words.begin() + (address - 0x1000u) / 4u; };
  const auto image_words = [](uint32_t id) {
    return std::array<uint32_t, 8>{id,
        uint32_t(Libs::Graphics::Prospero::BufferFormat::k32Float) << 20u, 0u,
        Libs::Graphics::DstSel(4, 0, 0, 1) | (9u << 28u), 0u, 0u, 0u, 0u};
  };
  // Runtime stride differs from the native sampler's 136-byte table arithmetic.
  constexpr uint32_t stride = 160u;
  for (uint32_t row = 0; row < 4u; ++row) {
    *at(table + row * stride) = objects + row * row_bytes;
    *at(table + row * stride + 4u) = 0xbeef0001u;
    for (uint32_t child = 0; child < 3u; ++child) {
      const auto image = image_words(10u + row * 3u + child);
      std::copy(image.begin(), image.end(), at(objects + row * row_bytes + child * 368u + 64u));
    }
  }
  std::array<uint32_t, 4> user_data{table, 1u | (stride << 16u), 2u, 0x5204u};
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                          .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && snapshot.images.size() == 7u,
        "indexed pointer/child domain lost candidates or read poisoned child padding");
  const auto captured = [&](uint64_t address, uint64_t bytes) {
    return std::ranges::any_of(snapshot.specialization_reads, [&](const auto& span) {
      return span.first <= address && address - span.first <= span.second &&
          bytes <= span.second - (address - span.first);
    });
  };
  for (uint32_t row = 0; row < 2u; ++row) {
    Check(captured(high + table + row * stride, 8u),
          "nested pointer read was absent from the captured alias ranges");
    for (uint32_t child = 0; child < 3u; ++child)
      Check(captured(high + objects + row * row_bytes + child * 368u + 64u, 32u),
            "nested child read was absent from the captured alias ranges");
  }
  const auto mapping = specialization.images[0].indirect_mapping_offset;
  Check(snapshot.flattened_srt[mapping] == 6u,
        "nested child table did not produce one dense composite-key mapping");
  for (uint32_t key = 0; key < 6u; ++key)
    Check(snapshot.images[key + 1u].dwords[0] == 10u + key &&
              snapshot.flattened_srt[mapping + key + 1u] == key + 1u,
          "nested composite key mixed parent rows or child records");
  const auto original = specialization;
  user_data[2] = 100u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && snapshot.images.size() == 7u,
        "nested parent enumeration included unreachable poison rows");
  user_data[2] = 2u;
  *at(table) = objects + row_bytes;
  *at(objects + row_bytes + 64u) = 42u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && snapshot.images.size() == 4u &&
            snapshot.images[1].dwords[0] == 42u && specialization != original,
        "nested table refresh retained stale pointer/descriptor data or duplicated aliases");
  *at(table) = objects;
  for (const uint32_t records : {0u, 1u}) {
    user_data[2] = records;
    const auto reads = memory.reads;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.reads == reads,
          "incomplete indexed parent domain became null images or performed partial candidate reads");
  }
  user_data[2] = UINT32_MAX;
  const auto oversized_reads = memory.reads;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.reads == oversized_reads,
        "oversized indexed parent table performed candidate reads");
  user_data[2] = 2u;
  for (const uint32_t bad_stride : {0u, 4u, 161u}) {
    user_data[1] = 1u | (bad_stride << 16u);
    Check(!MaterializeResources(plan, runtime, snapshot, specialization),
          "indexed pointer table accepted incomplete or misaligned records");
  }
  user_data[1] = 1u | (stride << 16u);
  for (const auto [word, bit] : {std::pair{1u, 31u}, std::pair{3u, 23u},
                                 std::pair{3u, 28u}, std::pair{3u, 30u}}) {
    user_data[word] |= 1u << bit;
    const auto reads = memory.reads;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.reads == reads,
          "indexed pointer table accepted unsupported RDNA2 geometry before reading candidates");
    user_data[word] &= ~(1u << bit);
  }
  memory.fail_address = high + table + stride;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "unreadable indexed parent pointer was treated as a null image");
  memory.fail_address = high + objects + 2u * 368u + 64u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "unreadable nested child was silently converted to a null image");
  memory.fail_address = UINT64_MAX;
  *at(table) = 0u;
  *at(table + 4u) = 0u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "explicit null pointer bypassed the specialization reader");
  auto reversed = make(Case::ReverseSigned);
  reversed->PlanAndTrack();
  // An inclusive child limit bounds one more child record, never the exclusive count.
  auto inclusive = make(Case::InclusiveChild);
  inclusive->PlanAndTrack();
  Check(nested_geometry(ExtractResourcePlan(inclusive->program), 4u),
        "an inclusive child limit did not bound one more child record");
  for (const auto variant : {Case::MissingParent, Case::WrongParent, Case::MissingChild,
          Case::MismatchedPair, Case::DifferentLaneMask,
          Case::ExtraUse, Case::UnboundedChoice, Case::MissingOuterWitness, Case::WrongOuterWitness,
          Case::CyclicChoice, Case::ExcessiveDomain}) {
    auto invalid = make(variant);
    CheckFatal([&] { invalid->PlanAndTrack(); }, "not a valid runtime value",
        "nested table accepted an unproved domain, mismatched pointer pair or unguarded use");
  }

  auto counted = make(Case::Counted);
  counted->PlanAndTrack();
  const auto counted_plan = ExtractResourcePlan(counted->program);
  constexpr uint32_t count_address = 0x1800u;
  std::array<uint32_t, 6> counted_data{table, 1u | (stride << 16u), 2u, 0x5204u, count_address, 1u};
  const SrtRuntime counted_runtime{.user_data = counted_data, .userdata = &memory,
                                  .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadLinearTestMemory, &memory);
  *at(table) = objects;
  *at(table + 4u) = 0xbeef0001u;
  *at(count_address) = *at(count_address + 4u) = 2u;
  Check(MaterializeResources(counted_plan, counted_runtime, snapshot, specialization) && snapshot.images.size() == 7u,
        "finite parent choices lost their runtime count and enumerated unavailable rows");
  const auto& counted_indirect = *counted_plan.descriptor_sources[counted_plan.info.images[0].source].indirect_descriptor;
  Check(counted_indirect.table_maximum == 3u && !counted_indirect.key_count.IsEmpty() &&
            captured(high + count_address, 4u),
        "finite parent domain lost its static maximum or captured runtime count");
  auto masked = make(Case::MaskedCount);
  masked->PlanAndTrack();
  const auto masked_plan = ExtractResourcePlan(masked->program);
  Check(!masked_plan.descriptor_sources[masked_plan.info.images[0].source].indirect_descriptor->key_count.IsEmpty() &&
            MaterializeResources(masked_plan, counted_runtime, snapshot, specialization) && snapshot.images.size() == 7u,
        "inactive preserved selector value discarded the selected lane's runtime count");
  memory.watched_address = high + table;
  for (const uint32_t count : {0u, 1u, 2u, 4u, 5u}) {
    *at(count_address) = count;
    counted_data[2] = std::min(count, 4u);
    memory.watched_reads = 0u;
    Check(MaterializeResources(counted_plan, counted_runtime, snapshot, specialization) &&
              snapshot.images.size() == 1u + 3u * std::min(count, 4u) &&
              (count != 0u || memory.watched_reads == 0u),
          "finite parent count refresh lost candidates, ignored its static cap or read an empty domain");
  }
  counted_data[2] = 2u;
  *at(count_address) = 3u;
  memory.watched_reads = 0u;
  Check(!MaterializeResources(counted_plan, counted_runtime, snapshot, specialization) && memory.watched_reads == 0u,
        "runtime parent count exceeding V# records was clamped or read candidates");
  *at(count_address) = 2u;
  // An unreadable parent count reads as zero: an empty domain without candidate reads.
  memory.fail_address = high + count_address;
  Check(MaterializeResources(counted_plan, counted_runtime, snapshot, specialization) &&
            snapshot.images.size() == 1u && memory.watched_reads == 0u,
        "unreadable parent count did not read as an empty domain");
  memory.fail_address = UINT64_MAX;
  for (const auto variant : {Case::MissingCount, Case::DifferentCount, Case::UnprovenMask}) {
    auto unproved = make(variant);
    unproved->PlanAndTrack();
    const auto unproved_plan = ExtractResourcePlan(unproved->program);
    const auto& source = unproved_plan.descriptor_sources[unproved_plan.info.images[0].source];
    memory.watched_reads = 0u;
    Check(source.indirect_descriptor->key_count.IsEmpty() &&
              !MaterializeResources(unproved_plan, counted_runtime, snapshot, specialization) && memory.watched_reads == 0u,
          "finite choice borrowed a missing or different leaf's count or pruned an unproven inactive arm");
  }
}

void TestGuardedDirectImageTable() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Guard {
    Nonzero, SccNonZero, Plain, Zero, Unrelated, Bypass, ExecZero, VccZero,
    Bounded, BoundMissing, BoundUnrelated, BoundBypass, BoundAnyLane,
    BoundDiamond, BoundBranchSpecific, BoundMismatched,
    BoundExec, BoundExecUnguarded, BoundExecMismatched, BoundExecAnd, BoundExecUnprovenActive,
    BoundExecFactored,
    BoundExecBranch, BoundExecBranchUnproven, BoundExecBranchUnguarded, BoundExecBranchMismatched,
    Finite, FiniteZero, FiniteMissing, FiniteWrong, FiniteSigned, FiniteAdd, FiniteXor,
    FiniteShared, FiniteSharedBypass, FiniteLate
  };
  const auto make_plan = [](Guard guard, uint32_t operand_order = 0u, bool packed_key = false) {
    const bool finite = guard >= Guard::Finite;
    const bool bounded = guard >= Guard::Bounded && !finite;
    const bool exec_bound = bounded && guard >= Guard::BoundExec;
    const bool entry_bound = exec_bound && guard >= Guard::BoundExecBranch;
    const bool shared = guard == Guard::FiniteShared || guard == Guard::FiniteSharedBypass;
    const bool bypass = guard == Guard::Bypass || guard == Guard::BoundBypass;
    const uint32_t table_offset = finite ? 3344u : exec_bound ? 672u : bounded ? 3696u : 344u;
    const uint32_t table_stride = exec_bound ? 784u : bounded ? 272u : 32u;
    Fixture fixture(bounded || finite ? ShaderType::Compute : ShaderType::Pixel);
    fixture.program.wave_size = 64u;
    auto *entry = fixture.block;
    auto *middle = fixture.AddBlock();
    auto *before_sample = fixture.AddBlock();
    auto *sample = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    auto *descriptor_block = guard == Guard::FiniteSharedBypass ? fixture.AddBlock()
                              : shared ? before_sample : sample;
    const auto varying = bounded || finite
        ? fixture.Emit(ValueOpcode::GetBuiltin,
            {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)})
        : fixture.Emit(ValueOpcode::GetAttribute, {Value(0u), Value(0u)});
    auto mask = fixture.Emit(ValueOpcode::ReadFirstLane, {varying, Value(true)});
    const auto srt = fixture.Address(fixture.UserData(0), fixture.UserData(1));
    Value key;
    Value limit;
    if (bounded) {
      key = fixture.Emit(ValueOpcode::FindILsb32, {mask});
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = 4u;
      limit = fixture.Emit(ValueOpcode::LoadAddressU32,
          {srt, Value(0u), Value(0u), Value(true)}, fixture.AddMemory(memory, 0x10));
    }
    Block *latch = nullptr;
    if (finite) {
      latch = fixture.AddBlock();
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarBuffer;
      const auto input = fixture.Buffer({Value(0x900000u), Value(0u), Value(4096u), Value(0u)});
      const auto loaded = fixture.Emit(ValueOpcode::ReadConstBuffer, {input, mask},
          fixture.AddMemory(memory, 0x2ecu));
      const auto initial = fixture.Emit(ValueOpcode::BitFieldUExtract,
          {loaded, Value(0u), Value(guard == Guard::FiniteZero ? 0u : 12u)});
      auto &phi = middle->AppendNewInst(ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
      mask = Value(&phi);
      key = fixture.Emit(ValueOpcode::FindILsb32, {mask}, 0, middle);
      const auto bit = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
          {Value(1u), fixture.Emit(ValueOpcode::BitwiseAnd32, {key, Value(31u)}, 0, latch)}, 0, latch);
      const auto next = guard == Guard::FiniteAdd
          ? fixture.Emit(ValueOpcode::BitwiseOr32, {mask, Value(1u << 31u)}, 0, latch)
          : guard == Guard::FiniteXor
              ? fixture.Emit(ValueOpcode::BitwiseXor32, {mask, bit}, 0, latch)
              : fixture.Emit(ValueOpcode::BitwiseAnd32,
                  {mask, fixture.Emit(ValueOpcode::BitwiseNot32, {bit}, 0, latch)}, 0, latch);
      phi.AddPhiOperand(entry, initial);
      phi.AddPhiOperand(latch, next);
    }
    const auto kind = entry_bound ? CFG::BranchCondition::VccNonZero
                    : guard == Guard::ExecZero || guard == Guard::BoundAnyLane || exec_bound
                        ? CFG::BranchCondition::ExecZero
                    : guard == Guard::VccZero ? CFG::BranchCondition::VccZero
                                             : CFG::BranchCondition::SccZero;
    Value condition;
    Value active(true);
    if (finite) {
      condition = guard == Guard::FiniteMissing ? Value(false)
          : guard == Guard::FiniteSigned
              ? fixture.Emit(ValueOpcode::LogicalNot,
                  {fixture.Emit(ValueOpcode::SLessThan32, {key, Value(12u)}, 0, middle)}, 0, middle)
              : fixture.Emit(ValueOpcode::IEqual32,
                  {key, Value(guard == Guard::FiniteWrong ? 31u : UINT32_MAX)}, 0, middle);
    } else if (bounded) {
      condition = guard == Guard::BoundMissing ? Value(false) : fixture.Emit(
          ValueOpcode::UGreaterThanEqual32,
          {guard == Guard::BoundUnrelated ? fixture.UserData(2) : key, limit});
      if (exec_bound) {
        const auto previous = fixture.Emit(ValueOpcode::INotEqual32, {varying, Value(2u)});
        if (entry_bound) {
          condition = fixture.Emit(ValueOpcode::LogicalOr,
              {fixture.Emit(ValueOpcode::LogicalAnd, {previous, condition}),
               fixture.Emit(ValueOpcode::LogicalAnd,
                   {previous, fixture.Emit(ValueOpcode::IEqual32, {key, Value(UINT32_MAX)})})});
          const auto extra = fixture.Emit(ValueOpcode::INotEqual32, {varying, Value(3u)});
          active = guard == Guard::BoundExecBranchUnproven ? extra
              : fixture.Emit(ValueOpcode::LogicalAnd, {previous, extra});
        } else {
          const auto encoded = fixture.Emit(ValueOpcode::SelectU32, {condition, Value(1u), Value(0u)});
          const auto stopped = fixture.Emit(guard == Guard::BoundExecAnd
              ? ValueOpcode::BitwiseAnd32 : ValueOpcode::BitwiseOr32, {encoded, varying});
          const auto retained = fixture.Emit(ValueOpcode::SelectU32,
              {previous, stopped, Value(0u)});
          const auto stopped_lane = fixture.Emit(ValueOpcode::INotEqual32, {retained, Value(0u)});
          auto vcc = fixture.Emit(ValueOpcode::LogicalAnd,
              operand_order & 1u ? std::initializer_list<Value>{stopped_lane, previous}
                                 : std::initializer_list<Value>{previous, stopped_lane});
          if (guard == Guard::BoundExecFactored) {
            const auto empty = fixture.Emit(ValueOpcode::IEqual32, {key, Value(UINT32_MAX)});
            const auto sentinel = fixture.Emit(ValueOpcode::LogicalAnd,
                operand_order & 2u ? std::initializer_list<Value>{empty, previous}
                                   : std::initializer_list<Value>{previous, empty});
            vcc = fixture.Emit(ValueOpcode::LogicalOr,
                operand_order & 4u ? std::initializer_list<Value>{sentinel, vcc}
                                   : std::initializer_list<Value>{vcc, sentinel});
          }
          const auto remaining = fixture.Emit(ValueOpcode::LogicalNot, {vcc});
          active = guard == Guard::BoundExecUnprovenActive ? remaining
              : fixture.Emit(ValueOpcode::LogicalAnd, {previous, remaining});
          condition = fixture.Emit(ValueOpcode::LogicalNot, {active});
        }
      }
    } else {
      condition = fixture.Emit(ValueOpcode::INotEqual32,
          {Value(0u), guard == Guard::Unrelated ? fixture.UserData(2) : mask});
      if (guard == Guard::SccNonZero) {
        condition = fixture.Emit(ValueOpcode::ConditionRef, {condition},
                                 CFG::BranchCondition::SccNonZero);
      }
      condition = fixture.Emit(ValueOpcode::LogicalNot, {condition});
    }
    if (guard != Guard::Plain && guard != Guard::SccNonZero) {
      condition = fixture.Emit(ValueOpcode::ConditionRef, {condition}, kind, finite ? middle : entry);
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
        .kind = bypass ? CFG::TerminatorKind::Branch : CFG::TerminatorKind::Return,
        .true_block = fixture.program.blocks[3u]};
    if (finite) {
      entry->condition = {};
      entry->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = middle};
      middle->condition = condition;
      middle->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = guard == Guard::FiniteXor ? latch : exit, .false_block = before_sample};
      before_sample->condition = fixture.Emit(ValueOpcode::IEqual32, {varying, Value(0u)}, 0, before_sample);
      before_sample->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = sample, .false_block = latch};
      sample->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = latch};
      latch->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = middle};
      if (guard == Guard::FiniteSharedBypass) {
        before_sample->terminator.true_block = descriptor_block;
        before_sample->terminator.false_block = sample;
        descriptor_block->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = sample};
      }
    }
    if (guard == Guard::BoundDiamond || exec_bound) {
      middle->condition = fixture.Emit(ValueOpcode::IEqual32,
          {mask, Value(1u)});
      middle->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = before_sample, .false_block = sample};
    } else if (guard == Guard::BoundBranchSpecific || guard == Guard::BoundMismatched) {
      entry->condition = fixture.Emit(ValueOpcode::IEqual32,
          {mask, Value(1u)});
      entry->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = middle, .false_block = before_sample};
      middle->condition = condition;
      middle->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = exit, .false_block = sample};
      if (guard == Guard::BoundMismatched) {
        before_sample->condition = fixture.Emit(ValueOpcode::UGreaterThanEqual32,
            {key, fixture.UserData(2)});
        before_sample->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
            .true_block = exit, .false_block = sample};
      }
    }
    for (auto *block : fixture.program.blocks) {
      if (block->terminator.kind == CFG::TerminatorKind::Return) continue;
      block->AddBranch(block->terminator.true_block);
      if (block->terminator.kind == CFG::TerminatorKind::ConditionalBranch)
        block->AddBranch(block->terminator.false_block);
    }
    std::array<Value, 2> pointer;
    for (uint32_t word = 0; word < pointer.size(); ++word) {
      if (bounded || finite) {
        pointer[word] = srt.Instruction()->Arg(word);
        continue;
      }
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = word * 4u;
      pointer[word] = fixture.Emit(
          ValueOpcode::LoadAddressU32, {srt, Value(0u), Value(0u), Value(true)},
          fixture.AddMemory(memory, 0x20));
    }
    fixture.block = descriptor_block;
    if (!bounded && !finite) key = fixture.Emit(packed_key ? ValueOpcode::ShiftRightLogical32
                                                        : ValueOpcode::FindILsb32,
        packed_key ? std::initializer_list<Value>{mask, Value(12u)}
                   : std::initializer_list<Value>{mask});
    auto scaled = bounded ? fixture.Emit(ValueOpcode::IMul32, {key, Value(table_stride)})
                          : fixture.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)});
    if (packed_key) scaled = fixture.Emit(ValueOpcode::BitwiseAnd32, {scaled, Value(992u)});
    const auto offset = fixture.Emit(ValueOpcode::IAdd32,
        {scaled, Value(table_offset)});
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
    if (guard == Guard::FiniteLate) {
      auto &instructions = fixture.block->Instructions();
      const auto at = [&](Value value) {
        return std::ranges::find_if(instructions,
            [&](const Inst &inst) { return &inst == value.Instruction(); });
      };
      instructions.splice(at(words[0]), instructions, at(image));
    }
    const auto sampler = fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
    MemoryInfo memory;
    memory.kind = ResourceKind::Image;
    memory.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw, {image, sampler, fixture.ImageAddress(), active},
                 fixture.AddMemory(memory, 0x128));
    if (shared) {
      fixture.block = sample;
      fixture.Emit(ValueOpcode::ImageSampleRaw,
          {fixture.Image(words, 0x12c), sampler, fixture.ImageAddress(), active},
          fixture.AddMemory(memory, 0x12c));
    }
    if (guard == Guard::BoundExecUnguarded || guard == Guard::BoundExecMismatched ||
        guard == Guard::BoundExecBranchUnguarded || guard == Guard::BoundExecBranchMismatched) {
      const auto other_active = guard == Guard::BoundExecUnguarded ||
                                guard == Guard::BoundExecBranchUnguarded ? Value(true)
          : fixture.Emit(ValueOpcode::ULessThan32, {key, fixture.UserData(2)});
      fixture.Emit(ValueOpcode::ImageSampleRaw,
          {image, sampler, fixture.ImageAddress(), other_active}, fixture.AddMemory(memory, 0x12c));
    }
    if (bounded || finite) {
      const auto output = fixture.Image({Value(0x8000u),
          Value(static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32UInt) << 20u),
          Value(0u), Value((static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u) |
                          Libs::Graphics::DstSel(4, 5, 6, 7)),
          Value(0u), Value(0u), Value(0u), Value(0u)}, 0x130);
      const auto texel = fixture.Emit(ValueOpcode::CompositeConstructU32x4,
          {Value(0u), Value(0u), Value(0u), Value(0u)});
      fixture.Emit(ValueOpcode::ImageWrite, {output, fixture.ImageAddress(), texel, Value(true)},
                   fixture.AddMemory(memory, 0x130));
    }
    if (exec_bound || packed_key) ConstantPropagationPass(fixture.program.blocks);
    if (guard == Guard::BoundExec || guard == Guard::BoundExecFactored) {
      std::vector<const Inst *> pending{active.ResolveInstruction()};
      uint32_t conjunctions = 0;
      for (size_t index = 0; index < pending.size(); ++index) {
        const auto *inst = pending[index];
        conjunctions += inst->GetOpcode() == ValueOpcode::LogicalAnd;
        Check(inst->GetOpcode() != ValueOpcode::SelectU32 &&
                  inst->GetOpcode() != ValueOpcode::BitwiseOr32,
              "EXEC bound retained its integer Boolean encoding");
        for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
          const auto *operand = inst->Arg(arg).Resolve().TryInstruction();
          if (operand != nullptr && std::ranges::find(pending, operand) == pending.end())
            pending.push_back(operand);
        }
      }
      Check(conjunctions == 1u, "EXEC bound retained repeated active-lane conjunctions");
    }
    fixture.PlanAndTrack();
    const auto source = fixture.program.info.images[0].source;
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_descriptor;
    Check(indirect && !indirect->selector.has_value() &&
              indirect->table_offset == table_offset && indirect->table_stride == table_stride &&
              (bounded ? !indirect->key_count.IsEmpty()
                       : indirect->key_count.Resolve() == Value(guard == Guard::FiniteZero ? 0u :
                           finite && guard != Guard::FiniteAdd && guard != Guard::FiniteXor ? 12u : 32u)) &&
              fixture.program.descriptor_sources[indirect->table_source].dword_count == 2u,
          "guarded direct image table lost its pointer or proven selector range");
    return ExtractResourcePlan(fixture.program);
  };

  auto plan = make_plan(Guard::Nonzero);
  make_plan(Guard::SccNonZero);
  make_plan(Guard::Plain);
  for (uint32_t order = 0; order < 8u; ++order) make_plan(Guard::BoundExecFactored, order);
  for (const auto guard : {Guard::Zero, Guard::Unrelated, Guard::Bypass,
                           Guard::ExecZero, Guard::VccZero, Guard::BoundMissing,
                           Guard::BoundUnrelated, Guard::BoundBypass, Guard::BoundAnyLane,
                           Guard::BoundBranchSpecific, Guard::BoundMismatched,
                           Guard::BoundExecUnguarded, Guard::BoundExecMismatched,
                           Guard::BoundExecAnd, Guard::BoundExecUnprovenActive,
                           Guard::BoundExecBranchUnproven, Guard::BoundExecBranchUnguarded,
                           Guard::BoundExecBranchMismatched,
                           Guard::FiniteMissing, Guard::FiniteWrong, Guard::FiniteSigned,
                           Guard::FiniteSharedBypass, Guard::FiniteLate}) {
    CheckFatal([&] { make_plan(guard); }, "not a valid runtime value",
               "direct table accepted a selector without a dominating range guard");
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
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadRuntimeMemory, &runtime);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  auto packed = make_plan(Guard::Plain, 0u, true);
  memory.fail_address = table + 32u * 32u;
  Check(MaterializeResources(packed, runtime, snapshot, specialization) &&
            memory.descriptor_reads == 32u && snapshot.images.size() == 32u,
        "packed five-bit selector did not enumerate exactly its finite domain");
  for (uint32_t key = 0; key < 32u; ++key)
    memory.words[(table - memory.base) / 4u + key * 8u] = 0x100u;
  memory.descriptor_reads = 0u;
  Check(MaterializeResources(packed, runtime, snapshot, specialization) &&
            memory.descriptor_reads == 32u && snapshot.images.size() == 1u &&
            specialization.images[0].indirect_root == ImageResource::NoIndirectImage &&
            specialization.images[0].indirect_search_iterations == 0u,
        "packed five-bit selector retained duplicate image bindings or lost keys");
  memory.fail_address = UINT64_MAX;
  memory.reads = 0u;
  memory.descriptor_reads = 0u;
  fill_table(memory, table);
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

  make_plan(Guard::Bounded);
  for (const auto guard : {Guard::BoundDiamond, Guard::BoundExec, Guard::BoundExecFactored,
                           Guard::BoundExecBranch}) {
    auto bounded = make_plan(guard);
    Check(bounded.capture_specialization_reads && bounded.info.images[1].written,
          "bounded table with image writes omitted specialization read capture");
    LinearTestMemory records;
    const bool exec_bound = guard != Guard::BoundDiamond;
    const uint32_t record_offset = exec_bound ? 672u : 3696u;
    const uint32_t record_stride = exec_bound ? 784u : 272u;
    for (uint32_t key = 0; key < 2u; ++key) {
      const auto word = (record_offset + key * record_stride) / 4u;
      records.words[word] = 0x500u + key;
      records.words[word + 1u] = static_cast<uint32_t>(
          Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
      records.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
          (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    }
    user_data = {static_cast<uint32_t>(records.base), 0u};
    runtime.userdata = &records;
    records.fail_address = records.base + record_offset;
    Check(MaterializeResources(bounded, runtime, snapshot, specialization) &&
              snapshot.images.size() == 2u && records.descriptor_reads == 0u &&
              std::ranges::all_of(snapshot.images[0].dwords, [](uint32_t word) { return word == 0u; }),
          "zero runtime count probed an unreachable descriptor row");
    records.fail_address = UINT64_MAX;
    records.words[1] = 2u;
    records.descriptor_reads = 0u;
    Check(MaterializeResources(bounded, runtime, snapshot, specialization) &&
              snapshot.images.size() == 3u && records.descriptor_reads == 2u &&
              snapshot.images[0].dwords[0] == 0x500u && snapshot.images[2].dwords[0] == 0x501u,
          "bounded interleaved table did not read precisely its two descriptor rows");
    const auto captured = [&](uint64_t address, uint64_t size) {
      return std::ranges::any_of(snapshot.specialization_reads, [&](const auto &read) {
        return read.first <= address && address - read.first <= read.second &&
               size <= read.second - (address - read.first);
      });
    };
    Check(captured(records.base + 4u, 4u) &&
              captured(records.base + record_offset, 32u) &&
              captured(records.base + record_offset + record_stride, 32u),
          "bounded table omitted count or descriptor bytes from writable-alias validation");
    records.fail_address = records.base + record_offset + record_stride + 28u;
    Check(!MaterializeResources(bounded, runtime, snapshot, specialization),
          "bounded interleaved table accepted an unreadable final descriptor word");
    records.fail_address = UINT64_MAX;
    records.words[1] = UINT32_MAX;
    records.descriptor_reads = 0u;
    Check(!MaterializeResources(bounded, runtime, snapshot, specialization) &&
              records.descriptor_reads == 0u,
          "unrepresentable table span was read before validating its count");
  }
  for (const auto guard : {Guard::Finite, Guard::FiniteZero, Guard::FiniteAdd,
                           Guard::FiniteXor, Guard::FiniteShared}) {
    const auto finite = make_plan(guard);
    Check(finite.capture_specialization_reads && finite.info.images[1].written,
          "finite bit table with image writes omitted specialization read capture");
    LinearTestMemory records;
    const uint32_t count = guard == Guard::FiniteZero ? 0u
        : guard == Guard::FiniteAdd || guard == Guard::FiniteXor ? 32u : 12u;
    const uint64_t first = records.base + 3344u;
    fill_table(records, first);
    // The GPU may select no bits or a sparse subset. Null rows remain valid candidates.
    std::fill_n(records.words.begin() + (first - records.base) / 4u + 5u * 8u, 8u, 0u);
    records.fail_address = first + count * 32u;
    user_data = {static_cast<uint32_t>(records.base), 0u};
    runtime.userdata = &records;
    Check(MaterializeResources(finite, runtime, snapshot, specialization) &&
              records.descriptor_reads == count && snapshot.images.size() == std::max(count, 1u) + 1u,
          "finite bit table probed beyond its mask bound or lost a null row");
    if (count == 0u) {
      Check(std::ranges::all_of(snapshot.images[0].dwords, [](uint32_t word) { return word == 0u; }),
            "empty bit loop retained a reachable image");
      continue;
    }
    Check(snapshot.flattened_srt[specialization.images[0].indirect_mapping_offset] == count,
          "finite bit table lost its candidate count");
    for (uint32_t key = 0; key < count; ++key) {
      Check(std::ranges::any_of(snapshot.specialization_reads, [&](const auto &read) {
        return read.first == first + key * 32u && read.second == 32u;
      }), "finite bit table omitted a descriptor from writable-alias validation");
    }
    records.fail_address = first + (count - 1u) * 32u + 28u;
    Check(!MaterializeResources(finite, runtime, snapshot, specialization),
          "finite bit table accepted an unreadable last candidate");
  }
}

void TestBoundedComputeImageLoop() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Clamp { None, Right, Left, Dynamic };
  enum class PostTest { None, Signed, Unsigned, WrongPolarity, Previous, Bypass, Variable, AnyLane };
  enum class Variant {
    Bounded, Plain, Nonzero, TrueEdge, WrongGuard, EntryBypass, ExitBypass, GuardBlock,
    WrongStep, DivergentBound, DivergentKey, Disjunction, WrongPolarity,
    IncrementBypass, PreviousBound, Forwarded, ForwardedChain, ForwardedJoin,
    ForwardedBranch, ForwardedIncrementBypass,
    Masked, MaskedWrongGuard, MaskResurrection, StatusOverwrite,
    GuardedDiamond, GuardedDiamondBypass
  };
  const auto make_plan = [](Variant variant, bool material = false, uint32_t first = 0u,
                            Clamp clamp = Clamp::None, PostTest post = PostTest::None,
                            uint32_t post_bound = 6u) {
    const bool post_test = post != PostTest::None;
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
    const bool forwarded = post_test ||
        (variant >= Variant::Forwarded && variant <= Variant::ForwardedIncrementBypass);
    auto *forward = forwarded ? fixture.AddBlock() : increment;
    auto *incoming = variant == Variant::ForwardedChain ? fixture.AddBlock() : forward;
    auto *detour = variant == Variant::ForwardedJoin || post == PostTest::Bypass ? fixture.AddBlock() : nullptr;
    entry->AddBranch(header);
    if (!masked && !post_test) {
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
    auto count = post_test && post != PostTest::Variable ? Value(post_bound) :
        variant == Variant::DivergentBound ? local : fixture.UserData(2);
    if (!post_test && first != 0u && variant != Variant::DivergentBound)
      count = fixture.Emit(ValueOpcode::LoadAddressU32,
          {fixture.Address(fixture.UserData(0), fixture.UserData(1)), Value(0u), Value(0u), Value(true)},
          fixture.AddMemory({.kind = ResourceKind::ScalarAddress}, 0x10));
    const auto in_range = post_test ? Value{} : fixture.Emit(material ? ValueOpcode::ULessThan32 : ValueOpcode::SLessThan32,
                                       {variant == Variant::WrongGuard
                                            ? Value(0u) : key,
                                        count}, 0, compare);
    const auto initial_active = fixture.Emit(ValueOpcode::INotEqual32,
                                             {local, Value(0u)}, 0, entry);
    if (forwarded && !post_test) {
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
    } else if (!post_test) {
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
    if (post_test) {
      const auto branch = [](Block *from, Block *to) {
        from->AddBranch(to);
        from->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = to};
      };
      const auto choose = [](Block *from, Block *yes, Block *no, Value condition) {
        from->AddBranch(yes);
        from->AddBranch(no);
        from->condition = condition;
        from->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                           .true_block = yes, .false_block = no};
      };
      entry->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = header};
      branch(header, body);
      branch(body, latch);
      auto *test_block = detour != nullptr ? detour : latch;
      if (detour != nullptr) choose(latch, detour, forward, initial_active);
      const auto compared = post == PostTest::Previous ? key : step;
      const auto predicate = fixture.Emit(post == PostTest::Unsigned ? ValueOpcode::ULessThan32 :
          ValueOpcode::SLessThan32, {compared, count}, 0, test_block);
      const auto condition = fixture.Emit(ValueOpcode::ConditionRef, {predicate},
          post == PostTest::AnyLane ? CFG::BranchCondition::ExecNonZero :
                                     CFG::BranchCondition::SccNonZero, test_block);
      choose(test_block, post == PostTest::WrongPolarity ? exit : forward,
             post == PostTest::WrongPolarity ? forward : exit, condition);
      branch(forward, incoming == forward ? header : incoming);
      if (incoming != forward) branch(incoming, header);
      exit->terminator.kind = CFG::TerminatorKind::Return;
    }
    phi.AddPhiOperand(entry, variant == Variant::DivergentKey ? local : Value(first));
    phi.AddPhiOperand(forwarded ? incoming : increment, step);
    Check(!forwarded || step.ResolveInstruction()->Parent() != phi.PhiBlock(1),
          "forwarded loop fixture did not separate the increment from its Phi incoming edge");

    fixture.block = variant == Variant::GuardBlock ? header : body;
    const auto table = fixture.Address(fixture.UserData(0), fixture.UserData(1));
    auto image_key = key;
    if (material) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = 2756u;
      image_key = fixture.Emit(ValueOpcode::LoadAddressU32,
          {table, fixture.Emit(ValueOpcode::IMul32, {key, Value(48u)}), Value(0u), Value(true)},
          fixture.AddMemory(memory, 0x2ff0));
      if (clamp != Clamp::None) {
        const auto maximum = clamp == Clamp::Dynamic ? fixture.UserData(3u) : Value(2u);
        image_key = fixture.Emit(ValueOpcode::UMin32,
            clamp == Clamp::Left ? std::initializer_list<Value>{maximum, image_key}
                                 : std::initializer_list<Value>{image_key, maximum});
      }
    }
    const auto offset = fixture.Emit(ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {image_key, Value(5u)}),
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
    if (first != 0u && !post_test) {
      const auto output = fixture.Image({Value(0x8000u),
          Value(static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32UInt) << 20u),
          Value(0u), Value((static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u) |
                          Libs::Graphics::DstSel(4, 5, 6, 7)),
          Value(0u), Value(0u), Value(0u), Value(0u)}, 0x2a0);
      const auto texel = fixture.Emit(ValueOpcode::CompositeConstructU32x4,
          {Value(0u), Value(0u), Value(0u), Value(0u)});
      fixture.Emit(ValueOpcode::ImageWrite, {output, fixture.ImageAddress(), texel, Value(true)},
                   fixture.AddMemory(sample, 0x2a0));
    }
    fixture.PlanAndTrack();
    const auto source = fixture.program.info.images[0].source;
    const auto &indirect = fixture.program.descriptor_sources[source].indirect_descriptor;
    Check(indirect && indirect->selector.has_value() == material &&
              indirect->table_offset == 0x6b0u &&
              (post_test || first == 0u ? indirect->key_count.Resolve() == count.Resolve()
                           : indirect->selector_first == Value(first) && !indirect->signed_count),
          "bounded compute loop lost its runtime image count");
    if (first != 0u)
      for (const auto word : words)
        Check(!word.Instruction()->HasUses() &&
                  fixture.program.memory_info[word.Instruction()->Flags<MemoryFlags>().index].planning_only,
              "nonzero-start image range retained descriptor-only scalar loads");
    if (material) Check(indirect->selector->stride == 48u &&
                            indirect->selector->offset == 2756u &&
                            indirect->selector->maximum == (clamp == Clamp::None ? UINT32_MAX : 2u) &&
                            fixture.program.descriptor_sources[indirect->selector->source].dword_count == 2u,
                        "bounded scalar selector lost its pointer or record layout");
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
  const auto post_test_plan = [&](PostTest post = PostTest::Signed, uint32_t bound = 6u,
                                  uint32_t first = 0u, Variant variant = Variant::Forwarded) {
    return make_plan(variant, false, first, Clamp::None, post, bound);
  };
  post_test_plan(PostTest::Signed, 6u, 0u, Variant::ForwardedChain);
  for (const auto post : {PostTest::WrongPolarity, PostTest::Previous, PostTest::Bypass,
                          PostTest::Variable, PostTest::AnyLane})
    CheckFatal([&] { post_test_plan(post); }, "not a valid runtime value",
               "post-tested image loop accepted an unproved carried-edge bound");
  for (const auto bound : {0u, 0x80000000u, UINT32_MAX})
    CheckFatal([&] { post_test_plan(PostTest::Signed, bound); }, "not a valid runtime value",
               "post-tested image loop accepted a nonpositive signed bound");
  CheckFatal([&] { post_test_plan(PostTest::Unsigned, 0u); }, "not a valid runtime value",
             "post-tested image loop accepted a zero unsigned bound");
  for (const auto first : {1u, UINT32_MAX})
    CheckFatal([&] { post_test_plan(PostTest::Unsigned, 6u, first); }, "not a valid runtime value",
               "post-tested image loop accepted a nonzero or wrapping initial value");
  CheckFatal([&] { post_test_plan(PostTest::Signed, 6u, 0u, Variant::WrongStep); },
             "not a valid runtime value", "post-tested image loop accepted a two-step induction");

  LinearTestMemory memory;
  const auto table = 0x1800u + 0x6b0u;
  for (uint32_t key = 0; key < 6u; ++key) {
    const auto word = (table - memory.base) / 4u + key * 8u;
    memory.words[word] = 0x100u + key;
    memory.words[word + 1u] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }
  std::array<uint32_t, 3> user_data{0x1800u, 0u, 2u};
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadLinearTestMemory, &memory);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const auto post : {PostTest::Signed, PostTest::Unsigned}) {
    for (const auto count : {6u, 1u}) {
      const auto post_plan = post_test_plan(post, count);
      memory.descriptor_reads = 0u;
      memory.fail_address = table + count * 32u;
      Check(MaterializeResources(post_plan, runtime, snapshot, specialization) &&
                memory.descriptor_reads == count && snapshot.images.size() == count &&
                snapshot.images.front().dwords[0] == 0x100u &&
                snapshot.images.back().dwords[0] == 0x100u + count - 1u,
            "post-tested image loop did not enumerate precisely zero through bound minus one");
    }
  }
  memory.fail_address = UINT64_MAX;
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

  const auto nonzero = make_plan(Variant::Forwarded, false, 1u);
  Check(nonzero.capture_specialization_reads && nonzero.info.images[1].written,
        "nonzero-start image loop omitted writable-alias validation");
  memory.fail_address = table;
  for (const uint32_t count : {0u, 1u, 0x80000000u, UINT32_MAX, 2u, 3u, 1u, 2u}) {
    memory.words[(user_data[0] - memory.base) / 4u] = count;
    memory.descriptor_reads = 0u;
    const auto length = count > 1u && count <= INT32_MAX ? count - 1u : 0u;
    Check(MaterializeResources(nonzero, runtime, snapshot, specialization) &&
              memory.descriptor_reads == length &&
              snapshot.images.size() == std::max(1u, length) + 1u &&
              snapshot.images[0].dwords[0] == (length != 0u ? 0x101u : 0u),
          "nonzero-start image loop read index zero or mishandled its signed bound");
    Check(std::ranges::find(snapshot.specialization_reads,
              std::pair{uint64_t{user_data[0]}, uint64_t{4u}}) != snapshot.specialization_reads.end(),
          "nonzero-start loop count escaped writable-alias capture");
    for (uint32_t key = 1u; key <= length; ++key)
      Check(std::ranges::find(snapshot.specialization_reads,
                std::pair{uint64_t{table} + key * 32u, uint64_t{32u}}) !=
                    snapshot.specialization_reads.end() &&
                std::ranges::any_of(snapshot.images, [&](const auto& image) {
                  return image.dwords[0] == 0x100u + key;
                }),
            "nonzero-start loop lost a descriptor or its writable-alias capture");
  }
  for (const auto count : {MaxIndirectDescriptorProbes + 2u, uint32_t{INT32_MAX}}) {
    memory.words[(user_data[0] - memory.base) / 4u] = count;
    memory.descriptor_reads = 0u;
    Check(!MaterializeResources(nonzero, runtime, snapshot, specialization) &&
              memory.descriptor_reads == 0u,
          "oversized nonzero-start image loop probed its descriptor table");
  }
  memory.fail_address = UINT64_MAX;
  for (const auto variant : {Variant::WrongStep, Variant::IncrementBypass,
                             Variant::EntryBypass, Variant::PreviousBound,
                             Variant::DivergentBound})
    CheckFatal([&] { make_plan(variant, false, 1u); }, "not a valid runtime value",
               "nonzero-start image loop accepted an unproved bound or induction");
  CheckFatal([&] { make_plan(Variant::Bounded, false, UINT32_MAX); }, "not a valid runtime value",
             "signed image loop accepted a negative initial key");

  auto material = make_plan(Variant::Bounded, true);
  for (const auto variant : {Variant::WrongGuard, Variant::EntryBypass, Variant::WrongStep})
    CheckFatal([&] { make_plan(variant, true); }, "not a valid runtime value",
               "raw pointer selector accepted an unbounded source");
  const auto first_key = uint64_t{user_data[0]} + 2756u;
  memory.words[(first_key - memory.base) / 4u] = 0u;
  memory.words[(first_key + 48u - memory.base) / 4u] = 2u;
  user_data[2] = 0u;
  memory.reads = 0u;
  memory.fail_address = first_key;
  Check(MaterializeResources(material, runtime, snapshot, specialization) && memory.reads == 0u &&
            snapshot.images.size() == 1u && snapshot.images[0].dwords[0] == 0u,
        "empty raw pointer selector read an unreachable record");
  user_data[2] = 2u;
  memory.fail_address = first_key + 96u;
  memory.descriptor_reads = 0u;
  Check(MaterializeResources(material, runtime, snapshot, specialization) &&
            memory.descriptor_reads == 2u && snapshot.images.size() == 2u &&
            snapshot.images[0].dwords[0] == 0x100u && snapshot.images[1].dwords[0] == 0x102u,
        "bounded raw pointer selector lost sparse keys or read an extra record");
  memory.fail_address = first_key + 48u;
  Check(!MaterializeResources(material, runtime, snapshot, specialization),
        "raw pointer selector accepted an unreadable selected record");
  memory.fail_address = UINT64_MAX;
  memory.words[(first_key - memory.base) / 4u] = UINT32_MAX - 1000u;
  Check(!MaterializeResources(material, runtime, snapshot, specialization),
        "raw pointer selector silently discarded an invalid negative key");

  for (const auto clamp : {Clamp::Right, Clamp::Left}) {
    const auto clamped = make_plan(Variant::Bounded, true, 0u, clamp);
    memory.words[(table - memory.base) / 4u + 8u + 3u] =
        static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor3D) << 28u;
    memory.words[(first_key - memory.base) / 4u] = 0u;
    memory.words[(first_key + 48u - memory.base) / 4u] = UINT32_MAX;
    memory.words[(first_key + 96u - memory.base) / 4u] = 2u;
    user_data[2] = 3u;
    memory.fail_address = first_key + 144u;
    memory.descriptor_reads = 0u;
    Check(MaterializeResources(clamped, runtime, snapshot, specialization) &&
              memory.descriptor_reads == 2u && snapshot.images.size() == 2u &&
              snapshot.images[0].dwords[0] == 0x100u && snapshot.images[1].dwords[0] == 0x102u,
          "clamped scalar selector read an unreachable image or failed to deduplicate clamped keys");
  }
  CheckFatal([&] { make_plan(Variant::Bounded, true, 0u, Clamp::Dynamic); },
             "not a valid runtime value", "scalar selector accepted an unproved dynamic clamp");
}

void TestDescendingImageRange() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant {
    Valid, WrongStep, IncrementBypass, MissingNonzero, NegativeInitial,
    Bitmap, BitmapAddsBit, BitmapWrongWord, BitmapUnknownInitial,
    BitmapByteOffset, BitmapScalarOffset, BitmapOuterClobber, BitmapWrongLoadMask,
    Bitmap8, Bitmap8MisplacedLoad, Bitmap8OtherBuffer, Bitmap8OuterClobber, Bitmap8WrongUpperWord,
    Bitmap8NarrowMask, Bitmap8OtherGuard
  };
  const auto make_plan = [](Variant variant) {
    Fixture fixture;
    const bool bitmap = variant >= Variant::Bitmap;
    // Eight-word records are two four-word loads of one buffer record.
    const uint32_t bitmap_words = variant >= Variant::Bitmap8 ? 8u : 4u;
    auto *entry = fixture.block;
    auto *header = fixture.AddBlock();
    auto *guard = fixture.AddBlock();
    auto *body = fixture.AddBlock();
    auto *latch = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    const auto branch = [](Block *from, Block *to) {
      from->AddBranch(to);
      from->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = to};
    };
    const auto choose = [](Block *from, Block *yes, Block *no, Value condition) {
      from->AddBranch(yes);
      from->AddBranch(no);
      from->condition = condition;
      from->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                         .true_block = yes, .false_block = no};
    };
    branch(entry, header);
    branch(body, bitmap ? guard : latch);
    branch(latch, header);
    auto &induction = header->AppendNewInst(ValueOpcode::Phi);
    induction.SetFlags(Type::U32);
    std::array<Inst *, 8> outer_words {}, remaining_words {};
    std::array<Value, 8> loaded_words {};
    if (bitmap)
      for (uint32_t word = 0; word < bitmap_words; ++word)
        outer_words[word] = &header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    const auto bound = fixture.Emit(ValueOpcode::SGreaterThanEqual32,
        {Value(&induction), Value(0u)}, 0, header);
    choose(header, guard, variant == Variant::IncrementBypass ? latch : exit, bound);
    Value mask;
    Value load_active(true);
    if (bitmap) {
      fixture.block = entry;
      const auto lane = fixture.Emit(ValueOpcode::LaneId);
      load_active = fixture.Emit(ValueOpcode::ULessThan32, {lane, Value(32u)});
      const auto buffer = fixture.Buffer({fixture.UserData(4), fixture.UserData(5),
                                          fixture.UserData(6), fixture.UserData(7)});
      MemoryInfo read;
      read.kind = ResourceKind::Buffer;
      read.idxen = true;
      read.offen = variant == Variant::BitmapByteOffset;
      read.data_dwords = 4u;
      std::array<Value, 2> packed;
      for (uint32_t part = 0; part < bitmap_words / 4u; ++part) {
        read.offset = part * (variant == Variant::Bitmap8MisplacedLoad ? 32u : 16u);
        const auto source = variant == Variant::Bitmap8OtherBuffer && part != 0u
            ? fixture.Buffer({fixture.UserData(8), fixture.UserData(9), fixture.UserData(10),
                              fixture.UserData(11)}) : buffer;
        const auto guard = variant == Variant::Bitmap8OtherGuard && part != 0u
            ? fixture.Emit(ValueOpcode::LogicalAnd,
                  {load_active, fixture.Emit(ValueOpcode::ULessThan32, {lane, Value(16u)})})
            : load_active;
        packed[part] = fixture.Emit(ValueOpcode::LoadBufferU32x4,
            {source, lane,
             Value(variant == Variant::BitmapByteOffset ? 4u : 0u),
             Value(variant == Variant::BitmapScalarOffset ? 4u : 0u), guard},
            fixture.AddMemory(read, 0x10 + part * 8u));
      }
      for (uint32_t word = 0; word < bitmap_words; ++word) {
        const bool wrong_word = variant == Variant::BitmapWrongWord ||
                                (variant == Variant::Bitmap8WrongUpperWord && word >= 4u);
        auto component = fixture.Emit(ValueOpcode::CompositeExtractU32x4,
            {packed[word / 4u], Value((wrong_word ? word + 1u : word) % 4u)}, 0, entry);
        component = fixture.Emit(ValueOpcode::SelectU32,
            {load_active, component, fixture.Emit(ValueOpcode::UndefU32)});
        loaded_words[word] = component;
        if (variant == Variant::BitmapUnknownInitial && word == 0u)
          component = fixture.Emit(ValueOpcode::SelectU32,
              {fixture.Emit(ValueOpcode::IEqual32, {fixture.Emit(ValueOpcode::LaneId), Value(0u)}),
               component, fixture.UserData(3)});
        outer_words[word]->AddPhiOperand(entry, component);
        remaining_words[word] = &guard->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
        remaining_words[word]->AddPhiOperand(header, Value(outer_words[word]));
      }
      auto selected = fixture.Emit(ValueOpcode::UndefU32, {}, 0, guard);
      for (uint32_t word = 0; word < bitmap_words; ++word) {
        selected = fixture.Emit(ValueOpcode::SelectU32,
            {fixture.Emit(ValueOpcode::IEqual32, {Value(&induction), Value(word)}, 0, guard),
             Value(remaining_words[word]), selected}, 0, guard);
      }
      mask = selected;
    } else {
      MemoryInfo lds;
      lds.kind = ResourceKind::Lds;
      mask = fixture.Emit(ValueOpcode::LoadSharedU32,
          {Value(0u), Value(true)}, fixture.AddMemory(lds, 0x10), guard);
    }
    auto active = bound;
    if (variant != Variant::MissingNonzero)
      active = fixture.Emit(ValueOpcode::LogicalAnd,
          {bound, fixture.Emit(ValueOpcode::ULessThan32, {Value(0u), mask}, 0, guard)}, 0, guard);
    if (bitmap && variant != Variant::BitmapWrongLoadMask)
      active = fixture.Emit(ValueOpcode::LogicalAnd, {load_active, active}, 0, guard);
    choose(guard, body, latch, active);
    fixture.block = body;
    const auto stale = fixture.UserData(3);
    const auto leading = fixture.Emit(ValueOpcode::SelectU32,
        {fixture.Emit(ValueOpcode::INotEqual32, {mask, Value(0u)}),
         fixture.Emit(ValueOpcode::ISub32,
             {Value(31u), fixture.Emit(ValueOpcode::FindUMsb32, {mask})}), Value(UINT32_MAX)});
    const auto predicated = fixture.Emit(ValueOpcode::SelectU32, {active, leading, stale});
    auto bit = fixture.Emit(ValueOpcode::SelectU32,
        {fixture.Emit(ValueOpcode::LogicalAnd,
             {active, fixture.Emit(ValueOpcode::IEqual32, {predicated, Value(UINT32_MAX)})}),
         Value(UINT32_MAX), fixture.Emit(ValueOpcode::SelectU32,
             {active, fixture.Emit(ValueOpcode::ISub32, {Value(31u), predicated}), stale})});
    const auto overwritten = fixture.Emit(ValueOpcode::LogicalAnd,
        {active, fixture.Emit(ValueOpcode::IEqual32,
             {fixture.Emit(ValueOpcode::BitwiseAnd32, {Value(&induction), Value(255u)}), Value(26u)})});
    const auto relative_write = fixture.Emit(ValueOpcode::SelectU32, {overwritten, mask, bit});
    const auto ordinal = fixture.Emit(ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {Value(&induction), Value(5u)}), relative_write});
    const auto selected = fixture.Emit(ValueOpcode::ReadFirstLane,
        {fixture.Emit(ValueOpcode::SelectU32, {active, ordinal, stale}), active});
    const auto key = fixture.Emit(ValueOpcode::ISub32, {fixture.UserData(2), selected});
    const auto offset = fixture.Emit(ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)}), Value(0x1000u)});
    const auto table = fixture.Address(fixture.UserData(0), fixture.UserData(1));
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = word * 4u;
      words[word] = fixture.Emit(ValueOpcode::LoadAddressU32,
          {table, offset, Value(0u), Value(true)}, fixture.AddMemory(memory, 0x20));
    }
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw,
        {fixture.Image(words), fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}),
         fixture.ImageAddress(), active}, fixture.AddMemory(sample, 0x30));
    if (bitmap) {
      auto next = fixture.Emit(ValueOpcode::BitwiseAnd32,
          {mask, fixture.Emit(ValueOpcode::BitwiseNot32,
              {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {Value(1u), bit})})});
      if (variant == Variant::BitmapAddsBit)
        next = fixture.Emit(ValueOpcode::BitwiseOr32, {next, Value(1u << 21u)});
      for (uint32_t word = 0; word < bitmap_words; ++word) {
        // Relative register writes index with M0 & 0xff; a narrower mask aliases upper words.
        const auto index = bitmap_words == 4u ? Value(&induction)
            : fixture.Emit(ValueOpcode::BitwiseAnd32,
                  {Value(&induction), Value(variant == Variant::Bitmap8NarrowMask ? 3u : 255u)});
        const auto selected_word = fixture.Emit(ValueOpcode::LogicalAnd,
            {active, fixture.Emit(ValueOpcode::IEqual32, {index, Value(word)})});
        remaining_words[word]->AddPhiOperand(body, fixture.Emit(ValueOpcode::SelectU32,
            {selected_word, next, Value(remaining_words[word])}));
        auto carried = Value(remaining_words[word]);
        if (variant == Variant::BitmapOuterClobber || variant == Variant::Bitmap8OuterClobber)
          carried = fixture.Emit(ValueOpcode::SelectU32,
              {fixture.Emit(ValueOpcode::IEqual32,
                   {Value(&induction), Value(word + 1u)}, 0, latch),
               loaded_words[(word + 1u) % bitmap_words], carried}, 0, latch);
        outer_words[word]->AddPhiOperand(latch, carried);
      }
    }
    const auto step = fixture.Emit(ValueOpcode::IAdd32,
        {Value(&induction), Value(variant == Variant::WrongStep ? UINT32_MAX - 1u : UINT32_MAX)}, 0, latch);
    induction.AddPhiOperand(entry,
        Value(variant == Variant::NegativeInitial ? UINT32_MAX : bitmap_words - 1u));
    induction.AddPhiOperand(latch, step);
    fixture.PlanAndTrack();
    Check(ordinal.Instruction()->Arg(1).Resolve() == bit.Resolve(),
          "descending range retained impossible relative-register stores");
    const auto &indirect = fixture.program.descriptor_sources[
        fixture.program.info.images[0].source].indirect_descriptor;
    const bool selects = variant == Variant::Bitmap || variant == Variant::Bitmap8;
    Check(indirect && indirect->selector.has_value() == selects &&
              (!indirect->selector || (indirect->selector->bitset &&
                                       indirect->selector->stride == bitmap_words * 4u)) &&
              indirect->key_count == Value(bitmap_words * 32u) &&
              !indirect->selector_first.IsEmpty(),
          "descending bitscan range lost its bound or accepted an unproved bitmap subset");
    return ExtractResourcePlan(fixture.program);
  };
  auto plan = make_plan(Variant::Valid);
  for (const auto variant : {Variant::WrongStep, Variant::IncrementBypass,
                             Variant::MissingNonzero, Variant::NegativeInitial})
    CheckFatal([&] { make_plan(variant); }, "not a valid runtime value",
               "descending descriptor range accepted an unproved update or zero bit scan");
  LinearTestMemory memory;
  for (uint32_t record = 1u; record <= 130u; ++record) {
    const auto word = record * 8u;
    memory.words[word] = 0x100u + (record & 1u);
    memory.words[word + 1u] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }
  std::array<uint32_t, 4> user_data {0x1000u, 0u, 0u, UINT32_MAX};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const SrtRuntime runtime {.user_data = user_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  for (const auto count : {0u, 2u, 0u}) {
    user_data[2] = count;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) && snapshot.images.size() == 2u,
          "descending descriptor subtraction failed when the runtime key underflowed");
    const auto offset = specialization.images[0].indirect_mapping_offset;
    Check(snapshot.flattened_srt[offset] == 128u, "descending mapping lost candidate keys");
    uint32_t previous = 0u;
    for (uint32_t entry = 0; entry < 128u; ++entry) {
      const auto key = snapshot.flattened_srt[offset + 1u + entry * 2u];
      Check((entry == 0u || key > previous) && uint32_t(count - key) <= 127u,
            "wrapped descending keys were unsorted or outside the proven domain");
      previous = key;
    }
  }

  auto bitmap_plan = make_plan(Variant::Bitmap);
  auto wide_plan = make_plan(Variant::Bitmap8);
  for (const auto variant : {Variant::BitmapAddsBit, Variant::BitmapWrongWord,
                             Variant::BitmapUnknownInitial, Variant::BitmapByteOffset,
                             Variant::BitmapScalarOffset, Variant::BitmapOuterClobber,
                             Variant::BitmapWrongLoadMask, Variant::Bitmap8MisplacedLoad,
                             Variant::Bitmap8OtherBuffer, Variant::Bitmap8OuterClobber,
                             Variant::Bitmap8WrongUpperWord, Variant::Bitmap8NarrowMask,
                             Variant::Bitmap8OtherGuard})
    make_plan(variant);
  // Every descriptor is distinct, so retained keys are visible in the snapshot as well as
  // its lookup table. An unrelated table slot must never be read, even when it is malformed.
  for (uint32_t record = 1u; record <= 130u; ++record)
    memory.words[record * 8u] = 0x100u + record;
  constexpr uint32_t bitmap_address = 0x4000u;
  constexpr uint32_t display_records = 32400u;
  const auto bitmap_word = (bitmap_address - memory.base) / 4u;
  memory.words.resize(bitmap_word + display_records * 4u, 0u);
  std::array<uint32_t, 8> bitmap_data {0x1000u, 0u, 0u, UINT32_MAX,
                                      bitmap_address, 16u << 16u, 3u, 0x5204u};
  const SrtRuntime bitmap_runtime {.user_data = bitmap_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  memory.watched_address = bitmap_address;
  auto *checked_plan = &bitmap_plan;
  const auto check_bitmap = [&](std::initializer_list<uint32_t> ranks, uint32_t count) {
    bitmap_data[2] = count;
    memory.watched_reads = memory.descriptor_reads = 0u;
    std::vector<uint32_t> keys;
    for (const auto rank : ranks) keys.push_back(count - rank);
    std::ranges::sort(keys);
    const auto dwords = bitmap_data[6] * (bitmap_data[5] >> 18u);
    Check(MaterializeResources(*checked_plan, bitmap_runtime, snapshot, specialization) &&
              memory.watched_reads == uint32_t(bitmap_data[6] != 0u) &&
              (bitmap_data[6] == 0u || memory.watched_dwords ==
                  std::min(dwords, MaxIndirectDescriptorProbes)) &&
              memory.descriptor_reads == keys.size() &&
              snapshot.images.size() == std::max<size_t>(keys.size(), 1u),
          "bitmap selector lost bounded reads or retained unreachable descriptors");
    for (uint32_t first = 0; first < dwords; first += MaxIndirectDescriptorProbes)
      Check(std::ranges::find(snapshot.specialization_reads,
                std::pair{uint64_t{bitmap_address} + first * 4u,
                    uint64_t{std::min(dwords - first, MaxIndirectDescriptorProbes)} * 4u}) !=
                snapshot.specialization_reads.end(),
            "bitmap chunk was omitted from same-dispatch write-alias protection");
    if (keys.empty()) {
      Check(std::ranges::all_of(snapshot.images[0].dwords, [](uint32_t value) { return value == 0u; }) &&
                specialization.images[0].indirect_root == ImageResource::NoIndirectImage,
            "empty bitmap retained a previously materialized image");
      return;
    }
    if (keys.size() == 1u) {
      Check(snapshot.images[0].dwords[0] == 0x100u + uint32_t(0x1000u + keys[0] * 32u) / 32u &&
                specialization.images[0].indirect_root == ImageResource::NoIndirectImage,
            "a single bitmap rank became empty or required an indirect image mapping");
      return;
    }
    const auto offset = specialization.images[0].indirect_mapping_offset;
    Check(snapshot.flattened_srt[offset] == keys.size(), "bitmap mapping lost a selected key");
    for (uint32_t index = 0; index < keys.size(); ++index) {
      const auto descriptor_record = uint32_t(0x1000u + keys[index] * 32u) / 32u;
      Check(snapshot.flattened_srt[offset + 1u + index * 2u] == keys[index] &&
                snapshot.images[index].dwords[0] == 0x100u + descriptor_record,
            "bitmap word association or U32-wrapped key selected a different descriptor");
    }
  };
  memory.fail_address = 0x2000u - 21u * 32u;
  check_bitmap({}, 0u);
  memory.words[bitmap_word] = 1u;
  check_bitmap({0u}, 0u);
  memory.words[bitmap_word + 1u] = 1u << 3u;
  memory.words[bitmap_word + 4u] = 1u; // Duplicate rank in another record.
  memory.words[bitmap_word + 6u] = 1u << 31u;
  memory.words[bitmap_word + 11u] = 1u << 1u;
  check_bitmap({0u, 35u, 95u, 97u}, 0u);
  check_bitmap({0u, 35u, 95u, 97u}, 2u);
  std::fill_n(memory.words.begin() + bitmap_word, 12u, 0u);
  for (uint32_t word = 0; word < 4u; ++word) {
    memory.words[bitmap_word + word] = 1u << 31u;
    memory.words[bitmap_word + 4u + word] = 1u;
  }
  check_bitmap({0u, 31u, 32u, 63u, 64u, 95u, 96u, 127u}, 0u);
  std::fill_n(memory.words.begin() + bitmap_word, 12u, 0u);
  memory.words[bitmap_word + 3u] = 1u << 31u;
  memory.words[bitmap_word + 8u] = 1u << 2u;
  check_bitmap({2u, 127u}, 0u);
  std::fill_n(memory.words.begin() + bitmap_word, 12u, 0u);
  check_bitmap({}, 0u);
  memory.fail_address = bitmap_address + 16u;
  Check(!MaterializeResources(bitmap_plan, bitmap_runtime, snapshot, specialization),
        "bitmap selector used a stale or partial snapshot after a failed coherent read");
  memory.fail_address = UINT64_MAX;

  const auto valid_data = bitmap_data;
  for (const auto [word, value] : std::array<std::pair<uint32_t, uint32_t>, 7>{{
           {4u, bitmap_address + 1u}, {5u, 8u << 16u}, {5u, 32u << 16u}, {5u, (16u << 16u) | (1u << 31u)},
           {7u, 0x5204u | (1u << 23u)}, {7u, 0x5204u | (1u << 28u)},
           {7u, 0x5204u | (1u << 30u)}}}) {
    bitmap_data[word] = value;
    Check(!MaterializeResources(bitmap_plan, bitmap_runtime, snapshot, specialization),
          "bitmap selector accepted an unproved buffer layout");
    bitmap_data = valid_data;
  }
  bitmap_data[7] &= ~(0x7fu << 12u); // FORMAT=INVALID makes even an unformatted load unbound.
  memory.words[bitmap_word] = 1u;
  memory.watched_reads = memory.descriptor_reads = 0u;
  Check(MaterializeResources(bitmap_plan, bitmap_runtime, snapshot, specialization) &&
            memory.watched_reads == 0u && memory.descriptor_reads == 0u &&
            snapshot.images.size() == 1u &&
            std::ranges::all_of(snapshot.images[0].dwords, [](uint32_t value) { return value == 0u; }),
        "unbound bitmap V# read backing bytes or manufactured an image key");
  bitmap_data = valid_data;
  memory.words[bitmap_word] = 0u;
  bitmap_data[6] = 0u;
  check_bitmap({}, 0u);
  bitmap_data[6] = MaxIndirectDescriptorProbes / 4u;
  memory.words[bitmap_word + MaxIndirectDescriptorProbes - 1u] = 1u << 31u;
  memory.words[bitmap_word] = 1u;
  check_bitmap({0u, 127u}, 0u);
  bitmap_data[6] = display_records;
  memory.words[bitmap_word + display_records * 4u - 1u] = 1u << 5u;
  check_bitmap({0u, 101u, 127u}, 0u);
  memory.fail_address = bitmap_address + MaxIndirectDescriptorProbes * 4u;
  Check(!MaterializeResources(bitmap_plan, bitmap_runtime, snapshot, specialization),
        "bitmap selector retained a partial union after a later chunk failed");
  memory.fail_address = UINT64_MAX;
  check_bitmap({0u, 101u, 127u}, 0u);
  std::fill_n(memory.words.begin() + bitmap_word, 4u, UINT32_MAX);
  memory.fail_address = bitmap_address + MaxIndirectDescriptorProbes * 4u;
  Check(MaterializeResources(bitmap_plan, bitmap_runtime, snapshot, specialization) &&
            snapshot.images.size() == 128u &&
            std::ranges::none_of(snapshot.specialization_reads, [&](const auto &read) {
              return read.first >= memory.fail_address;
            }),
        "a complete bitmap domain read redundant chunks or lost candidate keys");

  // Eight-word records keep their upper words' ranks, and a key below the table is never read.
  checked_plan = &wide_plan;
  bitmap_data = valid_data;
  bitmap_data[5] = 32u << 16u;
  bitmap_data[6] = 2u;
  std::fill_n(memory.words.begin() + bitmap_word, 16u, 0u);
  memory.words[bitmap_word] = 1u | (1u << 2u);
  memory.words[bitmap_word + 12u] = 1u << 1u;
  memory.fail_address = 0x2000u - 32u;
  check_bitmap({0u, 2u, 129u}, 2u);
  memory.fail_address = UINT64_MAX;
  for (const auto stride : {16u, 64u}) {
    bitmap_data[5] = stride << 16u;
    Check(!MaterializeResources(wide_plan, bitmap_runtime, snapshot, specialization),
          "an eight-word bitmap selector accepted a different record stride");
  }
}

void TestUniformizedMaterialImageKeys() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant {
    Valid, Plain, WrongUpdate, WrongEquality, WrongExit, WrongCarry, WrongBackedge,
    AndNot, Subset, FirstLane, FirstLaneDirect, FirstLaneAndNot, EmptyFirstEntry, WrongFirstBackedge,
    WideningFirstEntry, WideningFirstBackedge, WrongFirstLoadMask,
    Packed, PackedUnalignedOffset, PackedNegativeOffset, PackedFullAddress,
    PackedWrongGuard, PackedSparseOffset, PackedWrongField,
    PackedMerged, PackedUnboundedMerge, PackedCyclicMerge
  };
  const auto make_plan = [](Variant variant) {
    Fixture fixture;
    fixture.program.wave_size = 64u;
    const bool first_lane = variant >= Variant::FirstLane;
    const bool packed = variant >= Variant::Packed;
    const bool merged = variant >= Variant::PackedMerged;
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
    Value nibble;
    if (merged) {
      const auto low = fixture.Emit(ValueOpcode::BitwiseAnd32, {arbitrary, Value(15u)}, 0, entry);
      const auto high = fixture.Emit(ValueOpcode::BitwiseAnd32, {fixture.UserData(7u), Value(15u)}, 0, entry);
      auto &phi = (variant == Variant::PackedCyclicMerge ? header : merge)->AppendNewInst(
          ValueOpcode::Phi, {}, static_cast<uint64_t>(Type::U32));
      if (variant == Variant::PackedCyclicMerge) {
        phi.AddPhiOperand(entry, low);
        phi.AddPhiOperand(bit, fixture.Emit(ValueOpcode::SelectU32, {active, low, arbitrary}, 0, bit));
      } else {
        phi.AddPhiOperand(inactive, low);
        phi.AddPhiOperand(sentinel, variant == Variant::PackedUnboundedMerge ? arbitrary :
            fixture.Emit(ValueOpcode::SelectU32,
                {fixture.Emit(ValueOpcode::INotEqual32, {fixture.UserData(7u), Value(0u)}, 0, sentinel),
                 low, high}, 0, sentinel));
        phi.AddPhiOperand(bit, fixture.Emit(ValueOpcode::UMax32, {low, high}, 0, bit));
      }
      nibble = fixture.Emit(ValueOpcode::UMax32, {Value(&phi), low}, 0, merge);
    }
    const auto below = fixture.Emit(
        ValueOpcode::SGreaterThan32, {Value(32u), index}, 0, merge);
    auto material_guard = fixture.Emit(
        ValueOpcode::LogicalAnd, {active_on_entry, below}, 0, merge);
    if (variant == Variant::PackedCyclicMerge)
      material_guard = fixture.Emit(ValueOpcode::LogicalAnd, {material_guard, active}, 0, merge);
    const auto packed_load_guard = material_guard;
    if (packed) {
      const auto left = fixture.Emit(ValueOpcode::LogicalAnd,
          {material_guard, fixture.Emit(ValueOpcode::IEqual32, {arbitrary, Value(0u)}, 0, merge)}, 0, merge);
      const auto right = fixture.Emit(ValueOpcode::LogicalAnd,
          {variant == Variant::PackedWrongGuard ? Value(true) : material_guard,
           fixture.Emit(ValueOpcode::IEqual32, {arbitrary, Value(1u)}, 0, merge)}, 0, merge);
      material_guard = fixture.Emit(ValueOpcode::LogicalOr, {left, right}, 0, merge);
    }
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
    auto material_offset = fixture.Emit(
        ValueOpcode::SelectU32,
        {material_guard,
         fixture.Emit(ValueOpcode::IAdd32,
                      {times_nine, Value(0xc00u)}, 0, choose),
         times_nine}, 0, choose);
    if (packed) material_offset = fixture.Emit(ValueOpcode::BitwiseAnd32,
        {Value(variant == Variant::PackedSparseOffset ? 0xd0u : 0xf0u), scaled}, 0, choose);
    if (merged) material_offset = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
        {fixture.Emit(ValueOpcode::SelectU32, {packed_load_guard, nibble, arbitrary}, 0, choose),
         Value(4u)}, 0, choose);
    const auto base = fixture.Address(fixture.UserData(0u), fixture.UserData(1u));
    MemoryInfo material_memory;
    material_memory.kind = ResourceKind::Global;
    if (packed) material_memory.offset = variant == Variant::PackedUnalignedOffset ? 9u : 8u;
    if (variant == Variant::PackedNegativeOffset) material_memory.offset = static_cast<uint32_t>(-4096);
    material_memory.address_is_full = variant == Variant::PackedFullAddress;
    const auto load_mask = variant == Variant::WrongFirstLoadMask
        ? fixture.Emit(ValueOpcode::LogicalNot, {material_guard}, 0, choose)
        : packed ? packed_load_guard : material_guard;
    const auto loaded = fixture.Emit(
        ValueOpcode::LoadAddressU32,
        {base, material_offset, Value(0u), load_mask},
        fixture.AddMemory(material_memory, 0x1a88u), choose);
    auto projected = loaded;
    if (packed) {
      projected = fixture.Emit(ValueOpcode::SelectU32, {load_mask, loaded, arbitrary}, 0, choose);
      projected = fixture.Emit(ValueOpcode::BitFieldUExtract,
          {projected, Value(variant == Variant::PackedWrongField ? 17u : merged ? 0u : 16u), Value(16u)}, 0, choose);
      projected = fixture.Emit(ValueOpcode::UMin32, {Value(16383u), projected}, 0, choose);
    }
    const auto local_key = fixture.Emit(
        ValueOpcode::SelectU32, {material_guard, projected, arbitrary}, 0, choose);
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
    if (packed)
      Check(indirect && indirect->selector && indirect->selector->stride == 16u &&
                indirect->selector->offset == (variant == Variant::PackedUnalignedOffset ? 9u : 8u) &&
                indirect->selector->global && indirect->selector->bit_offset == (merged ? 0u : 16u) &&
                indirect->selector->bit_mask == 0xffffu && indirect->selector->maximum == 16383u &&
                indirect->key_count == Value(16u) && indirect->selector_first == Value(0u) &&
                indirect->selector_mask.IsEmpty(),
            "packed uniformized selector lost its bounded record domain or field projection");
    else Check(indirect && indirect->selector->stride == 0x90u &&
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
  Check(plan.capture_specialization_reads &&
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
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
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

  const auto packed = make_plan(Variant::Packed);
  memory.words.resize(0x84000u / 4u);
  for (uint32_t record = 0; record < 16u; ++record)
    memory.words[(record * 16u + 8u) / 4u] = (7u << 16u) | 0xdead;
  memory.words[(16u + 8u) / 4u] = UINT32_MAX;
  memory.words[(32u + 8u) / 4u] = (4096u << 16u) | 0xbeef;
  set_descriptor(base + 0x20e0u + 16383u * 32u, 0x600u);
  memory.fail_address = base + 16u * 16u + 8u;
  memory.reads = memory.descriptor_reads = 0u;
  Check(MaterializeResources(packed, runtime, snapshot, specialization) &&
            memory.reads == 4u && memory.descriptor_reads == 3u && snapshot.images.size() == 3u &&
            snapshot.images[0].dwords[0] == 0x200u && snapshot.images[1].dwords[0] == 0x400u &&
            snapshot.images[2].dwords[0] == 0x600u,
        "packed selector did not extract before clamping or read outside its sixteen records");
  memory.fail_address = base + 15u * 16u + 8u;
  Check(!MaterializeResources(packed, runtime, snapshot, specialization),
        "packed selector accepted an unreadable final record");
  memory.fail_address = UINT64_MAX;
  const auto unaligned = make_plan(Variant::PackedUnalignedOffset);
  for (const auto [base_offset, immediate] : {std::pair{1u, 8u}, {0u, 9u}, {3u, 9u}}) {
    user_data[0] = base + base_offset;
    for (uint32_t record = 0; record < 16u; ++record) {
      const uint32_t word = ((record == 1u ? 16383u : record == 2u ? 4096u : 7u) << 16u) | 0xdead;
      std::memcpy(reinterpret_cast<uint8_t *>(memory.words.data()) +
                      base_offset + immediate + record * 16u, &word, sizeof(word));
    }
    Check(MaterializeResources(immediate == 8u ? packed : unaligned, runtime, snapshot, specialization) &&
              snapshot.images.size() == 3u && snapshot.images[0].dwords[0] == 0x200u &&
              snapshot.images[1].dwords[0] == 0x400u && snapshot.images[2].dwords[0] == 0x600u &&
              std::ranges::find(snapshot.specialization_reads,
                  std::pair<uint64_t, uint64_t>{base + base_offset + immediate, 244u}) !=
                  snapshot.specialization_reads.end(),
          "global selector did not preserve exact base-plus-offset while scalar descriptor reads aligned the base");
  }
  user_data[0] = base;
  const auto merged = make_plan(Variant::PackedMerged);
  for (uint32_t record = 0; record < 16u; ++record)
    memory.words[(record * 16u + 8u) / 4u] =
        0xdead0000u | (record == 1u ? 0xffffu : record == 2u ? 4096u : 7u);
  memory.fail_address = base + 16u * 16u + 8u;
  memory.reads = memory.descriptor_reads = 0u;
  Check(MaterializeResources(merged, runtime, snapshot, specialization) &&
            memory.reads == 4u && memory.descriptor_reads == 3u && snapshot.images.size() == 3u &&
            snapshot.images[0].dwords[0] == 0x200u && snapshot.images[1].dwords[0] == 0x400u &&
            snapshot.images[2].dwords[0] == 0x600u,
        "merged nibble selector did not preserve its sixteen-record domain and lower-half projection");
  for (const auto variant : {Variant::PackedNegativeOffset, Variant::PackedFullAddress,
                             Variant::PackedWrongGuard, Variant::PackedSparseOffset,
                             Variant::PackedWrongField, Variant::PackedUnboundedMerge,
                             Variant::PackedCyclicMerge})
    CheckFatal([&] { make_plan(variant); }, "not a valid runtime value",
               "packed selector accepted an unproved OR guard, offset domain, or field extraction");
}

void TestIndexedMaterialImageKeys() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  const auto make_program = [](ShaderType stage = ShaderType::Compute, bool shared = false) {
    Fixture f(stage);
    auto *entry = f.block, *body = f.AddBlock();
    auto *later = shared ? f.AddBlock() : nullptr;
    auto *done = f.AddBlock();
    entry->AddBranch(body);
    entry->AddBranch(done);
    body->AddBranch(shared ? later : done);
    if (shared) later->AddBranch(done);
    const auto lane = f.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
    const auto active = f.Emit(ValueOpcode::INotEqual32, {lane, Value(UINT32_MAX)});
    entry->condition = f.Emit(ValueOpcode::ConditionRef, {active}, CFG::BranchCondition::ExecNonZero);
    entry->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                         .true_block = body, .false_block = done};
    body->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = shared ? later : done};
    if (shared) later->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = done};
    done->terminator.kind = CFG::TerminatorKind::Return;
    const auto material = f.Buffer({f.UserData(0), f.UserData(1), f.UserData(2), f.UserData(3)});
    const auto heap = f.Buffer({f.UserData(4), f.UserData(5), f.UserData(6), f.UserData(7)});
    const auto output = f.Buffer({f.UserData(8), f.UserData(9), f.UserData(10), f.UserData(11)});
    const auto sampler = f.Sampler({Value(0u), Value(0u), Value(1u << 26u), Value(0u)});
    const auto address = f.ImageAddress();
    Value owner_key, shared_image;
    for (uint32_t index = 0; index < (shared ? 3u : 2u); ++index) {
      const uint32_t field = index == 1u ? 68u : 64u;
      auto *use = index == 2u ? later : body;
      auto key = owner_key;
      if (index < 2u) {
        MemoryInfo memory;
        memory.kind = ResourceKind::Buffer;
        memory.idxen = true;
        memory.offset = field;
        const auto loaded = f.Emit(ValueOpcode::LoadBufferU32,
            {material, lane, Value(0u), Value(0u), active}, f.AddMemory(memory, field), entry);
        const auto selected = f.Emit(ValueOpcode::SelectU32, {active, loaded, Value(0xdeadbeefu)}, 0, entry);
        key = shared && index == 0u
            ? f.Emit(ValueOpcode::ReadLane, {selected, Value(0u)}, 0, entry)
            : f.Emit(ValueOpcode::ReadFirstLane, {selected, active}, 0, body);
        if (shared && index == 0u) {
          owner_key = key;
          const auto equal = f.Emit(ValueOpcode::IEqual32, {key, selected}, 0, entry);
          const auto guard = f.Emit(ValueOpcode::LogicalAnd, {active, equal}, 0, entry);
          entry->condition = f.Emit(ValueOpcode::ConditionRef, {guard}, CFG::BranchCondition::ExecNonZero, entry);
        }
      }
      const auto offset = f.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)}, 0, use);
      std::array<Value, 8> words;
      for (uint32_t word = 0; word < words.size(); ++word) {
        MemoryInfo scalar;
        scalar.kind = ResourceKind::ScalarBuffer;
        scalar.offset = word * 4u;
        words[word] = f.Emit(ValueOpcode::ReadConstBuffer, {heap, offset},
                            f.AddMemory(scalar, 0x100u + field), use);
      }
      const auto image_handle = [&] {
        return f.Emit(ValueOpcode::GetImageResource,
            {words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]}, 0, use);
      };
      MemoryInfo gather;
      gather.kind = ResourceKind::Image;
      gather.image_dimension = Decoder::ImageDimension::Dim2D;
      if (index == 0u)
        f.Emit(ValueOpcode::ImageQueryDimensions, {image_handle(), address}, f.AddMemory(gather, 0x180u), body);
      const auto image = image_handle();
      if (index == 2u) shared_image = image;
      gather.image_sample_flags = Decoder::ImageSampleFlagLod;
      f.Emit(ValueOpcode::ImageGatherRaw,
          {image, sampler, address, words[1], words[3], Value(0u), Value(1u << 26u), active},
          f.AddMemory(gather, 0x200u + field), use);
    }
    MemoryInfo store;
    store.kind = ResourceKind::Buffer;
    f.Emit(ValueOpcode::StoreBufferU32,
        {output, Value(0u), Value(0u), Value(0u), Value(1u), Value(true)},
        f.AddMemory(store, 0x300u), done);
    f.PlanAndTrack();
    if (shared) {
      owner_key.ResolveInstruction()->ReplaceUsesWith(Value(12994u));
      uint32_t offset = 0u;
      Check(EvaluateSrtValue(f.program, {}, shared_image.ResolveInstruction()->Arg(0), offset) &&
                offset == 12994u * 32u,
            "inherited indexed selector changed GPU byte offsets into raw material keys");
    }
    return std::move(f.program);
  };
  auto program = make_program();
  auto plan = ExtractResourcePlan(program);
  Check(plan.info.images.size() == 3u && plan.selector_keys.size() == 2u,
        "indexed material fields did not share their query/gather selector domain");
  for (const auto &image : plan.info.images) {
    const auto &source = *plan.descriptor_sources[image.source].indirect_descriptor;
    Check(source.table_scalar && source.selector && source.selector->stride == 0u &&
              source.key_count.IsEmpty() && source.selector_first.IsEmpty(),
          "indexed material field fell back to an unconditional heap scan");
  }
  constexpr uint32_t base = 0x1000u, heap = 0x2000u, records = 14517u;
  struct Memory {
    std::vector<uint32_t> words = std::vector<uint32_t>((heap - base + records * 32u) / 4u);
    uint32_t selectors = 0, descriptors = 0;
    bool narrow = true, unreadable = false;
  } memory;
  const auto put = [&](uint32_t key, uint32_t mips) {
    auto *words = memory.words.data() + (heap - base + key * 32u) / 4u;
    words[0] = 0x100u + key;
    words[1] = static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k16UNorm) << 20u;
    words[3] = (9u << 28u) | ((mips - 1u) << 16u) | Libs::Graphics::DstSel(4, 5, 6, 7);
  };
  for (uint32_t key = 0; key < 64u; ++key) put(key, 1u);
  put(12994u, 8u);
  put(12996u, 8u);
  for (uint32_t row = 0; row < 10u; ++row) {
    memory.words[(row * 96u + 64u) / 4u] = 12994u;
    memory.words[(row * 96u + 68u) / 4u] = 12996u;
  }
  std::array<uint32_t, 12> user_data{base, 96u << 16u, 10u, 0x5204u,
      heap, 32u << 16u, records, 0x5204u, 0x90000u, 4u << 16u, 16u, 0x5204u};
  const auto read = +[](void *data, uint64_t address, std::span<uint32_t> words) {
    auto &m = *static_cast<Memory *>(data);
    if (address < base || address - base + words.size_bytes() > m.words.size() * 4u) return false;
    if (address < heap) {
      ++m.selectors;
      if (m.unreadable) return false;
    } else {
      ++m.descriptors;
      const auto key = (address - heap) / 32u;
      if (m.narrow && (words.size() != 8u || (key != 0u && key != 12994u && key != 12996u))) return false;
    }
    std::copy_n(m.words.begin() + (address - base) / 4u, words.size(), words.begin());
    return true;
  };
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                           .read_specialization_memory = read};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            memory.selectors == 2u && memory.descriptors == 6u && snapshot.images.size() == 9u,
        "indexed selector scanned the broad heap or did not share its captured keys");
  ApplyResourceSpecialization(program, specialization);
  Check(program.info.images[0].indirect_resources.size() == 3u,
        "indirect dimension query lost its specialized candidates");
  for (uint32_t root = 0; root < plan.info.images.size(); ++root) {
    const auto &image = specialization.images[root];
    const auto offset = image.indirect_mapping_offset;
    Check(snapshot.images[root].dwords[0] == 0u && image.indirect_search_iterations == 2u &&
              snapshot.flattened_srt[offset] == 2u && snapshot.flattened_srt[offset + 1u] == 0u &&
              snapshot.flattened_srt[offset + 3u] == (root == 2u ? 12996u : 12994u) * 32u,
          "byte-key selector lost native OOB zero, sorted offsets, or the null default");
  }
  Check(std::ranges::count_if(specialization.images, [](const auto &image) {
          return image.mip_count == 8u;
        }) == 2u, "indirect gathers lost candidate-specific mip counts");
  for (const uint32_t field : {64u, 68u})
    Check(std::ranges::find(snapshot.specialization_reads,
              std::pair<uint64_t, uint64_t>{base + field, 868u}) != snapshot.specialization_reads.end(),
          "indexed selector batch escaped writable-alias tracking");
  auto shared_program = make_program(ShaderType::Compute, true);
  auto shared = ExtractResourcePlan(shared_program);
  Check(MaterializeResources(shared, runtime, snapshot, specialization) && snapshot.images.size() == 9u,
        "inherited indexed selector lost its bounded material domain");
  memory.words[64u / 4u] = 12996u;
  memory.selectors = 0;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            memory.selectors == 2u && snapshot.images.size() == 11u,
        "changed material rows reused stale selector keys");
  memory.words[64u / 4u] = 12994u;
  memory.unreadable = true;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "unreadable eligible selector silently fell back to the heap");
  memory.narrow = false;
  const auto original = user_data;
  for (const uint32_t variant : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
    user_data = original;
    if (variant == 0u) user_data[1] |= 1u << 31u;
    if (variant == 1u) user_data[3] |= 1u << 23u;
    if (variant == 2u) user_data[3] |= 1u << 28u;
    if (variant == 3u) user_data[0] += 1u;
    if (variant == 4u) user_data[2] = MaxIndirectDescriptorProbes + 1u;
    if (variant == 5u) user_data[8] = base + 68u;
    if (variant == 6u) user_data[1] = 66u << 16u;
    if (variant == 7u) user_data[0] = 0u;
    memory.selectors = 0;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              memory.selectors == 0u && snapshot.images.size() > 100u,
          "unsupported geometry or writable selector lost the original heap domain");
  }
  user_data = original;
  auto graphics_program = make_program(ShaderType::Pixel);
  auto graphics = ExtractResourcePlan(graphics_program);
  Check(MaterializeResources(graphics, runtime, snapshot, specialization) && memory.selectors == 0u,
        "graphics material lookup acquired unproved selector read dependencies");
  memory.unreadable = false;
  for (uint32_t row = 0; row < 10u; ++row)
    memory.words[(row * 96u + 64u) / 4u] = 0x08000000u; // U32 key*32 wraps to zero.
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && snapshot.images.size() == 7u,
        "indexed selector lost wrapped scalar descriptor offsets");
  for (auto &source : plan.descriptor_sources) {
    if (!source.indirect_descriptor) continue;
    source.indirect_descriptor->table_offset = 0xffffffe0u;
    source.indirect_descriptor->table_immediate = 32u;
  }
  for (uint32_t row = 0; row < 10u; ++row) {
    memory.words[(row * 96u + 64u) / 4u] = row == 0u ? 1u : 0u;
    memory.words[(row * 96u + 68u) / 4u] = 0u;
  }
  memory.selectors = memory.descriptors = 0u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            memory.selectors == 2u && memory.descriptors == 2u && snapshot.images.size() == 5u,
        "overflowed scalar immediate read a descriptor or lost valid wrapped keys");
  for (uint32_t root = 0; root < 2u; ++root) {
    const auto offset = specialization.images[root].indirect_mapping_offset;
    Check(snapshot.images[root].dwords[0] == 0u && snapshot.flattened_srt[offset] == 2u &&
              snapshot.flattened_srt[offset + 1u] == 32u && snapshot.flattened_srt[offset + 3u] == UINT32_MAX &&
              snapshot.flattened_srt[offset + 4u] == 0u,
          "invalid byte key selected a real texture instead of the null root");
  }
  for (auto &source : plan.descriptor_sources) {
    if (!source.indirect_descriptor) continue;
    source.indirect_descriptor->table_offset = source.indirect_descriptor->table_immediate = 0u;
  }
  memory.narrow = true;
  memory.unreadable = true;
  for (const uint32_t variant : {0u, 1u, 2u, 3u}) {
    user_data = original;
    if (variant == 0u) user_data[2] = 0u;
    if (variant == 1u) user_data[1] = 0u;
    if (variant == 2u) user_data[3] = 0u;
    if (variant == 3u) user_data[1] = 64u << 16u;
    memory.selectors = 0;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.selectors == 0u &&
              snapshot.images.size() == 6u,
          "native zero/OOB indexed load read its unavailable payload");
  }
}

void TestPointerTableIndexedMaterialKeys() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  // A waterfall scalarizes one lane's indexed x3 key; matching lanes sample the
  // descriptor at the user-data pointer plus key * 32.
  Fixture f;
  auto *entry = f.block, *body = f.AddBlock(), *done = f.AddBlock();
  entry->AddBranch(body);
  entry->AddBranch(done);
  body->AddBranch(done);
  const auto lane = f.Emit(ValueOpcode::GetBuiltin,
      {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
  const auto active = f.Emit(ValueOpcode::INotEqual32, {lane, Value(UINT32_MAX)});
  const auto material = f.Buffer({f.UserData(0), f.UserData(1), f.UserData(2), f.UserData(3)});
  MemoryInfo request;
  request.kind = ResourceKind::Buffer;
  request.idxen = true;
  const auto loaded = f.Emit(ValueOpcode::LoadBufferU32x3,
      {material, lane, Value(0u), Value(0u), active}, f.AddMemory(request, 0x28));
  const auto selected = f.Emit(ValueOpcode::SelectU32,
      {active, f.Emit(ValueOpcode::CompositeExtractU32x3, {loaded, Value(2u)}), Value(0u)});
  const auto key = f.Emit(ValueOpcode::ReadLane, {selected, f.UserData(6)});
  const auto guard = f.Emit(ValueOpcode::LogicalAnd,
      {active, f.Emit(ValueOpcode::IEqual32, {key, selected})});
  entry->condition = f.Emit(ValueOpcode::ConditionRef, {guard}, CFG::BranchCondition::ExecNonZero);
  entry->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                       .true_block = body, .false_block = done};
  body->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = done};
  done->terminator.kind = CFG::TerminatorKind::Return;
  f.block = body;
  const auto table = f.Address(f.UserData(4), f.UserData(5));
  const auto offset = f.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)});
  std::array<Value, 8> words;
  for (uint32_t word = 0; word < words.size(); ++word) {
    MemoryInfo scalar;
    scalar.kind = ResourceKind::ScalarAddress;
    scalar.offset = word * 4u;
    words[word] = f.Emit(ValueOpcode::LoadAddressU32, {table, offset, Value(0u), Value(true)},
                         f.AddMemory(scalar, 0x60));
  }
  MemoryInfo sample;
  sample.kind = ResourceKind::Image;
  sample.image_dimension = Decoder::ImageDimension::Dim2D;
  f.Emit(ValueOpcode::ImageSampleRaw,
      {f.Image(words, 0x6c), f.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}),
       f.ImageAddress(), guard},
      f.AddMemory(sample, 0x6c));
  f.PlanAndTrack();
  const auto plan = ExtractResourcePlan(f.program);
  Check(plan.info.images.size() == 1u, "pointer-table waterfall image was not tracked");
  const auto &indirect = *plan.descriptor_sources[plan.info.images[0].source].indirect_descriptor;
  Check(indirect.selector && indirect.selector->stride == 0u && indirect.selector->offset == 8u &&
            !indirect.selector->global && !indirect.table_scalar && indirect.table_stride == 32u &&
            plan.descriptor_sources[indirect.table_source].dword_count == 2u,
        "pointer-table waterfall lost its indexed x3 material selector");

  constexpr uint32_t base = 0x1000u, heap = 0x2000u, records = 6u;
  struct Memory {
    std::vector<uint32_t> words = std::vector<uint32_t>((heap - base + 16u * 32u) / 4u);
    uint32_t descriptors = 0;
  } memory;
  const auto put = [&](uint32_t key) {
    auto *texture = memory.words.data() + (heap - base + key * 32u) / 4u;
    texture[0] = 0x100u + key;
    texture[1] = static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k16UNorm) << 20u;
    texture[3] = (9u << 28u) | Libs::Graphics::DstSel(4, 5, 6, 7);
  };
  for (const uint32_t key : {0u, 5u, 9u}) put(key);
  for (uint32_t row = 0; row < records; ++row)
    memory.words[(row * 12u + 8u) / 4u] = row % 2u == 0u ? 9u : 5u;
  const std::array<uint32_t, 7> user_data{base, 12u << 16u, records, 0x5204u, heap, 0u, 0u};
  const auto read = +[](void *data, uint64_t address, std::span<uint32_t> words) {
    auto &m = *static_cast<Memory *>(data);
    if (address < base || address - base + words.size_bytes() > m.words.size() * 4u) return false;
    if (address >= heap) {
      const auto key = (address - heap) / 32u;
      if (words.size() != 8u || (key != 0u && key != 5u && key != 9u)) return false;
      ++m.descriptors;
    }
    std::copy_n(m.words.begin() + (address - base) / 4u, words.size(), words.begin());
    return true;
  };
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                           .read_specialization_memory = read};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.descriptors == 3u &&
            snapshot.images.size() == 3u,
        "pointer-table selector did not read exactly the zero and material-key descriptors");
  Check(std::ranges::find(snapshot.specialization_reads, std::pair<uint64_t, uint64_t>{base + 8u, 64u}) !=
            snapshot.specialization_reads.end(),
        "pointer-table selector rows escaped writable-alias tracking");
  const auto mapping = specialization.images[0].indirect_mapping_offset;
  Check(snapshot.images[0].dwords[0] == 0x100u && specialization.images[0].indirect_search_iterations == 2u &&
            snapshot.flattened_srt[mapping] == 3u && snapshot.flattened_srt[mapping + 3u] == 5u &&
            snapshot.flattened_srt[mapping + 5u] == 9u,
        "pointer-table selector lost sorted raw keys or the OOB-zero root descriptor");
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
    for (const auto mode : {ImageMipMode::None, ImageMipMode::Dynamic}) {
      plan.info.images[0].mip_mode = mode;
      for (const auto [base, last] : {std::pair{0u, 0u}, {1u, 1u}, {2u, 3u},
                                      {15u, 15u}, {7u, 6u}}) {
        user_data = valid_descriptor;
        user_data[3] |= (base << 12u) | (last << 16u);
        Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                  (base <= last ? snapshot.images[0].dwords == user_data : is_null()),
              "texture view mip-range validity depended on the image instruction");
      }
    }
    plan.info.images[0].mip_mode = ImageMipMode::None;
    for (uint32_t shift = 0u; shift < 12u; shift += 3u) {
      for (uint32_t channel = 0u; channel < 8u; ++channel) {
        user_data = valid_descriptor;
        user_data[3] = (user_data[3] & ~(7u << shift)) | (channel << shift);
        Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                  (channel == 2u || channel == 3u ? is_null()
                      : snapshot.images[0].dwords == user_data),
              "texture view accepted an undefined channel selector or rejected a defined one");
      }
    }
    for (uint32_t fragments = 0u; fragments <= 4u; ++fragments) {
      user_data = valid_descriptor;
      user_data[3] = (user_data[3] & 0xfffu) | (fragments << 16u) |
          (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2DMsaa) << 28u);
      user_data[5] = fragments << 4u;
      Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                (fragments >= 1u && fragments <= 3u
                    ? snapshot.images[0].dwords == user_data : is_null()),
            "MSAA fragment encoding was confused with a mip range");
      user_data[3] |= 1u << 12u;
      Check(MaterializeResources(plan, runtime, snapshot, specialization) && is_null(),
            "MSAA view accepted a nonzero base mip");
    }
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
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadLinearTestMemory, &memory);
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
  // An unreadable key reads as zero and selects the first record instead of the second.
  memory.words[0x44u / 4u] = 1u;
  memory.fail_address = 0x1044u;
  Check(materializes(0x30u), "unavailable nested image key did not read as zero");
}

void TestCompactScalarImageTable() {
  using F = Libs::Graphics::Prospero::BufferFormat;
  constexpr uint32_t stride = 0x330u, image_offset = 0x98u, sampler_offset = 0x88u;
  enum class Domain { Unbounded, SharedPrefix, SeparatePrefix, SeparateMask };
  enum class Pairing { SameKey, DifferentKey, PairedFirst, UnpairedFirst, MixedUses };
  enum class Sharing { None, SameBlock, Dominating, Sibling, Later, WrongMask, MixedMask };
  const auto make_plan = [&](bool compact, uint32_t invalid = 0u,
                             Domain domain = Domain::Unbounded, Pairing pairing = Pairing::SameKey,
                             uint32_t member = 0x98u, Sharing sharing = Sharing::None) {
    Fixture fixture;
    const auto list = invalid == 1u
        ? fixture.Address(fixture.UserData(0), fixture.UserData(1))
        : fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                          fixture.UserData(2), fixture.UserData(3)});
    const auto table = fixture.Buffer({fixture.UserData(4), fixture.UserData(5),
                                       fixture.UserData(6), fixture.UserData(7)});
    const auto group = fixture.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)), Value(0u)});
    // The original case has no CPU-evaluable row or workgroup-prefix proof.
    const auto index = domain == Domain::Unbounded
        ? fixture.Emit(ValueOpcode::BitwiseXor32, {group, Value(1u)})
        : fixture.Emit(ValueOpcode::IAdd32, {group, fixture.UserData(8u)});
    const auto offset = fixture.Emit(invalid == 2u ? ValueOpcode::UDiv32
                                                  : ValueOpcode::ShiftLeftLogical32,
                                     {index, Value(2u)});
    MemoryInfo scalar;
    scalar.kind = invalid == 1u ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer;
    const auto key = invalid == 1u
        ? fixture.Emit(ValueOpcode::LoadAddressU32, {list, offset, Value(0u), Value(true)},
                       fixture.AddMemory(scalar, 0x570u))
        : fixture.Emit(ValueOpcode::ReadConstBuffer, {list, offset},
                       fixture.AddMemory(scalar, 0x570u));
    scalar.kind = ResourceKind::ScalarBuffer;
    const auto make_image = [&](Value selected, uint32_t member) {
      const auto record = fixture.Emit(ValueOpcode::IMul32, {selected, Value(stride)});
      std::array<Value, 8> words;
      for (uint32_t word = 0; word < 8u; ++word) {
        scalar.offset = member + word * 4u;
        words[word] = compact && word >= 4u ? Value(0u)
            : fixture.Emit(ValueOpcode::ReadConstBuffer, {table, record},
                            fixture.AddMemory(scalar, 0x590u));
      }
      if (invalid == 3u) words[7] = Value(1u);
      return fixture.Image(words, 0x59cu);
    };
    const auto image = make_image(key, member);
    auto sampler_key = key;
    Value unpaired;
    if (pairing == Pairing::DifferentKey || pairing == Pairing::PairedFirst ||
        pairing == Pairing::UnpairedFirst) {
      scalar.offset = 4u;
      const auto other_key = fixture.Emit(ValueOpcode::ReadConstBuffer, {list, offset},
                                           fixture.AddMemory(scalar, 0x578u));
      if (pairing == Pairing::DifferentKey) sampler_key = other_key;
      else unpaired = make_image(other_key, member);
    } else if (pairing == Pairing::MixedUses) unpaired = image;
    const auto record = fixture.Emit(ValueOpcode::IMul32, {sampler_key, Value(stride)});
    std::array<Value, 4> sampler_words;
    for (uint32_t word = 0; word < 4u; ++word) {
      scalar.offset = sampler_offset + word * 4u;
      sampler_words[word] = fixture.Emit(ValueOpcode::ReadConstBuffer, {table, record},
                                          fixture.AddMemory(scalar, 0x590u));
    }
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    sample.image_r128 = compact;
    const auto sampler = fixture.Sampler(sampler_words, 0x59cu);
    const auto read_unpaired = [&] {
      fixture.Emit(ValueOpcode::ImageRead, {unpaired, fixture.ImageAddress(), Value(true)},
                   fixture.AddMemory(sample, 0x580u));
    };
    if (sharing == Sharing::None) {
      if (pairing == Pairing::UnpairedFirst) read_unpaired();
      fixture.Emit(ValueOpcode::ImageSampleRaw,
          {image, sampler, fixture.ImageAddress(), Value(true)}, fixture.AddMemory(sample, 0x59cu));
      if (pairing == Pairing::PairedFirst || pairing == Pairing::MixedUses) read_unpaired();
    } else {
      const auto owner = make_image(key, image_offset);
      const auto lane = fixture.Emit(ValueOpcode::LaneId);
      const auto owner_active = fixture.Emit(ValueOpcode::UGreaterThanEqual32, {lane, Value(32u)});
      const auto outside = fixture.Emit(ValueOpcode::LogicalNot, {owner_active});
      const auto active = fixture.Emit(ValueOpcode::LogicalAnd, {owner_active,
          fixture.Emit(ValueOpcode::IEqual32,
              {fixture.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(1u)}), Value(0u)})});
      auto *owner_block = fixture.block;
      auto *target_block = fixture.block;
      if (sharing == Sharing::Dominating || sharing == Sharing::Sibling) {
        namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
        auto *entry = fixture.block;
        if (sharing == Sharing::Sibling) {
          owner_block = fixture.AddBlock();
          entry->AddBranch(owner_block);
          entry->condition = fixture.Emit(ValueOpcode::INotEqual32,
                                          {fixture.UserData(8u), Value(0u)});
        }
        target_block = fixture.AddBlock();
        entry->AddBranch(target_block);
        entry->terminator = {.kind = sharing == Sharing::Sibling
                ? CFG::TerminatorKind::ConditionalBranch : CFG::TerminatorKind::Branch,
            .true_block = sharing == Sharing::Sibling ? owner_block : target_block,
            .false_block = sharing == Sharing::Sibling ? target_block : nullptr};
        if (sharing == Sharing::Sibling) owner_block->terminator.kind = CFG::TerminatorKind::Return;
        target_block->terminator.kind = CFG::TerminatorKind::Return;
      }
      const auto read_target = [&](Value mask) {
        fixture.block = target_block;
        fixture.Emit(ValueOpcode::ImageRead, {image, fixture.ImageAddress(), mask},
                     fixture.AddMemory(sample, 0x79cu));
      };
      if (sharing == Sharing::Later) read_target(active);
      fixture.block = owner_block;
      fixture.Emit(ValueOpcode::ImageSampleRaw,
          {owner, sampler, fixture.ImageAddress(), owner_active}, fixture.AddMemory(sample, 0x59cu));
      if (sharing != Sharing::Later)
        read_target(sharing == Sharing::WrongMask ? outside : active);
      if (sharing == Sharing::MixedMask) read_target(outside);
    }
    if (domain != Domain::Unbounded) {
      // Distinct SSA expressions with equivalent runtime first values share a domain.
      const auto second_offset = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
          {fixture.Emit(ValueOpcode::IAdd32, {group, fixture.UserData(8u)}), Value(2u)});
      scalar.offset = domain == Domain::SeparatePrefix ? 4u : 0u;
      auto second_key = fixture.Emit(ValueOpcode::ReadConstBuffer, {list, second_offset},
                                     fixture.AddMemory(scalar, 0x580u));
      if (domain == Domain::SeparateMask)
        second_key = fixture.Emit(ValueOpcode::BitFieldUExtract, {second_key, Value(0u), Value(1u)});
      fixture.Emit(ValueOpcode::ImageSampleRaw,
          {make_image(second_key, 0x16cu), sampler, fixture.ImageAddress(), Value(true)},
          fixture.AddMemory(sample, 0x79cu));
    }
    if (invalid == 4u) {
      sample.image_r128 = false;
      fixture.Emit(ValueOpcode::ImageSampleRaw,
          {image, sampler, fixture.ImageAddress(), Value(true)}, fixture.AddMemory(sample, 0x5a0u));
    }
    fixture.PlanAndTrack();
    const auto &source = fixture.program.descriptor_sources[fixture.program.info.samplers[
        sampler.Instruction()->Flags<uint32_t>()].source];
    Check(source.indirect_descriptor && source.indirect_descriptor->selector &&
              source.indirect_descriptor->selector->stride == 4u &&
              (domain == Domain::Unbounded ? source.indirect_descriptor->selector_first.IsEmpty()
                  : !source.indirect_descriptor->selector_first.IsImmediate() &&
                    source.indirect_descriptor->workgroup_axis == 0u) &&
              source.indirect_descriptor->key_count.IsEmpty() &&
              source.indirect_descriptor->table_stride == stride &&
              source.indirect_descriptor->table_immediate == sampler_offset,
          "compact image sampler lost its complete bounded index-list domain");
    if (domain == Domain::Unbounded) {
      const auto &image_source = fixture.program.descriptor_sources[
          fixture.program.info.images[image.Instruction()->Flags<uint32_t>()].source];
      const bool paired = sharing == Sharing::None ? pairing != Pairing::DifferentKey
          : sharing == Sharing::SameBlock || sharing == Sharing::Dominating;
      Check(image_source.indirect_descriptor &&
                image_source.indirect_descriptor->selector.has_value() == paired,
            "required selector proof ignored key, use order, dominance or active lanes");
      if (!unpaired.IsEmpty() && unpaired != image) {
        const auto &other_source = fixture.program.descriptor_sources[
            fixture.program.info.images[unpaired.Instruction()->Flags<uint32_t>()].source];
        Check(&other_source != &image_source && other_source.indirect_descriptor &&
                  !other_source.indirect_descriptor->selector &&
                  other_source.indirect_descriptor->table_stride == stride &&
                  other_source.indirect_descriptor->table_immediate == member,
              "paired image narrowed an existing unpaired heap source");
      }
    }
    return ExtractResourcePlan(fixture.program);
  };
  struct Case { bool compact; uint32_t member; Pairing pairing; Sharing sharing = Sharing::None; };
  for (const auto [compact, member, pairing, sharing] : {
           Case{false, image_offset, Pairing::SameKey}, Case{true, image_offset, Pairing::SameKey},
           Case{true, 0x16cu, Pairing::SameKey}, Case{true, 0x16cu, Pairing::DifferentKey},
           Case{true, 0x16cu, Pairing::PairedFirst}, Case{true, 0x16cu, Pairing::UnpairedFirst},
           Case{true, 0x16cu, Pairing::MixedUses},
           Case{true, 0x16cu, Pairing::SameKey, Sharing::SameBlock},
           Case{true, 0x16cu, Pairing::SameKey, Sharing::Dominating},
           Case{true, 0x16cu, Pairing::SameKey, Sharing::Sibling},
           Case{true, 0x16cu, Pairing::SameKey, Sharing::Later},
           Case{true, 0x16cu, Pairing::SameKey, Sharing::WrongMask},
           Case{true, 0x16cu, Pairing::SameKey, Sharing::MixedMask}}) {
    auto plan = make_plan(compact, 0u, Domain::Unbounded, pairing, member, sharing);
    const uint32_t width = compact ? 4u : 8u;
    const auto bytes = 2u * stride + member + width * 4u;
    std::array<uint32_t, 9> data{0x1000u, 0u, 8u, 0u, 0x2000u, 0u, bytes, 0u, 0u};
    struct Memory : LinearTestMemory { uint32_t selector_attempts = 0; } memory;
    memory.words.resize((0x1000u + bytes) / 4u, 0xdeadbeefu);
    memory.words[0] = 2u;
    memory.words[1] = 1u;
    for (uint32_t row = 0; row < 3u; ++row) {
      auto at = memory.words.begin() + (0x1000u + row * stride + sampler_offset) / 4u;
      std::fill_n(at, 4u, 0u);
      at[0] = row;
      at = memory.words.begin() + (0x1000u + row * stride + member) / 4u;
      std::fill_n(at, width, 0u);
      at[0] = 0x100u + row;
      at[1] = static_cast<uint32_t>(F::k32_32_32_32Float) << 20u;
      at[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
          (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
      if (!compact) at[5] = 0x00700000u;
      if (sharing != Sharing::None)
        std::copy_n(at, width, memory.words.begin() + (0x1000u + row * stride + image_offset) / 4u);
    }
    const std::array poison{0x00fff000u, 0x09500000u, 0u, 0x900003acu};
    if (member == 0x16cu)
      std::copy(poison.begin(), poison.end(), memory.words.begin() + (0x1000u + 0x17cu) / 4u);
    memory.fail_address = 0x2000u + bytes; // Last descriptor ends at the readable boundary.
    SrtRuntime runtime{.user_data = data, .userdata = &memory,
        .read_specialization_memory = [](void *userdata, uint64_t address,
                                         std::span<uint32_t> words) {
          auto &memory = *static_cast<Memory *>(userdata);
          if (address == memory.base) ++memory.selector_attempts;
          return ReadLinearTestMemory(&memory, address, words);
        }};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const bool shared = sharing == Sharing::SameBlock || sharing == Sharing::Dominating;
    if ((sharing != Sharing::None && !shared) ||
        (pairing != Pairing::SameKey && pairing != Pairing::MixedUses)) {
      Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
                std::ranges::any_of(snapshot.images, [&](const auto &image) {
                  return std::equal(poison.begin(), poison.end(), image.dwords.begin());
                }),
            "unpaired image use silently inherited a narrower sampler domain");
      if (sharing == Sharing::Sibling)
        Check(memory.selector_attempts == 0u,
              "inactive sibling selector owner introduced a new specialization read");
      continue;
    }
    const auto materialize = [&] {
      memory.selector_attempts = 0u;
      Check(MaterializeResources(plan, runtime, snapshot, specialization),
            "paired compact image/sampler table did not materialize");
      Check(snapshot.images.size() == (shared ? 6u : 3u) && snapshot.samplers.size() == 3u &&
                memory.selector_attempts == 1u,
            "paired table omitted an image, index-list value or OOB zero key");
      for (const auto &image : snapshot.images) {
        Check(image.dword_count == 8u, "compact image escaped normalized eight-DWORD ABI");
        if (compact) Check(std::ranges::all_of(std::span(image.dwords).subspan(4u),
                                              [](uint32_t word) { return word == 0u; }),
                           "R128 image copied poisoned adjacent record fields");
      }
    };
    materialize();
    const auto original = specialization;
    memory.words[(0x1000u + stride + member) / 4u] = 0x201u;
    materialize();
    Check(specialization == original && std::ranges::any_of(snapshot.images,
              [](const auto &image) { return image.dwords[0] == 0x201u; }),
          "compact descriptor payload refresh changed specialization or stayed stale");
    data[2] = 4u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == (shared ? 4u : 2u) && snapshot.samplers.size() == 2u,
          "index-list count refresh retained a stale sampler candidate");
    data[2] = 0u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == (shared ? 2u : 1u) && snapshot.images[0].dwords[0] == 0x100u &&
              snapshot.samplers.size() == 1u &&
              snapshot.samplers[0].dwords[0] == 0u,
          "empty index list lost the scalar OOB zero key");
    data[2] = 8u;
    memory.fail_address = 0x1004u;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization),
          "unreadable selected index-list bytes were accepted");
    memory.fail_address = UINT64_MAX;
    data[2] = (MaxIndirectDescriptorProbes + 1u) * 4u;
    memory.selector_attempts = 0u;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.selector_attempts == 0u,
          "oversized index list was read before the existing probe cap");
  }
  for (const auto domain : {Domain::SharedPrefix, Domain::SeparatePrefix, Domain::SeparateMask}) {
    auto plan = make_plan(true, 0u, domain);
    constexpr uint32_t bytes = 2u * stride + 0x16cu + 16u;
    std::array<uint32_t, 9> data{0x1000u, 0u, 8u, 0u, 0x2000u, 0u, bytes, 0u, 0u};
    std::array<uint32_t, 3> counts{3u, 1u, 1u};
    struct Memory : LinearTestMemory {
      std::vector<std::pair<uint64_t, uint64_t>> selector_reads;
    } memory;
    memory.words.resize((0x1000u + bytes) / 4u, 0u);
    memory.words[0] = 2u;
    memory.words[1] = 1u;
    for (uint32_t row = 0; row < 3u; ++row) {
      memory.words[(0x1000u + row * stride + sampler_offset) / 4u] = row;
      for (const auto [member, base] : {std::pair{image_offset, 0x100u},
                                      std::pair{0x16cu, 0x200u}}) {
        auto at = memory.words.begin() + (0x1000u + row * stride + member) / 4u;
        at[0] = base + row;
        at[1] = static_cast<uint32_t>(F::k32_32_32_32Float) << 20u;
        at[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
            (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
      }
    }
    const SrtRuntime runtime{.user_data = data, .userdata = &memory,
        .read_specialization_memory = [](void *userdata, uint64_t address,
                                         std::span<uint32_t> words) {
          auto &memory = *static_cast<Memory *>(userdata);
          if (address < 0x2000u) memory.selector_reads.emplace_back(address, words.size_bytes());
          return ReadLinearTestMemory(&memory, address, words);
        }, .workgroup_counts = counts};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const auto materialize = [&](std::vector<uint32_t> expected, std::vector<uint32_t> expected_samplers) {
      memory.selector_reads.clear();
      Check(MaterializeResources(plan, runtime, snapshot, specialization),
            "shared selector domain failed to materialize");
      std::vector<uint32_t> images;
      for (const auto &image : snapshot.images) images.push_back(image.dwords[0]);
      std::ranges::sort(images);
      std::vector<uint32_t> samplers;
      for (const auto &sampler : snapshot.samplers) samplers.push_back(sampler.dwords[0]);
      std::ranges::sort(samplers);
      Check(images == expected && samplers == expected_samplers,
            "shared selector domain mixed descriptor fields or retained stale keys");
      std::vector<std::pair<uint64_t, uint64_t>> reads;
      const auto expect_read = [&](uint32_t offset) {
        offset += data[8] * 4u;
        if (counts[0] != 0u && offset < data[2])
          reads.emplace_back(0x1000u + offset, std::min(counts[0] * 4u, data[2] - offset));
      };
      expect_read(0u);
      if (domain != Domain::SharedPrefix) expect_read(domain == Domain::SeparatePrefix ? 4u : 0u);
      Check(memory.selector_reads == reads,
            "selector domain was reread per resource or distinct domains were merged");
    };
    materialize(domain == Domain::SharedPrefix
        ? std::vector<uint32_t>{0x100u, 0x101u, 0x102u, 0x200u, 0x201u, 0x202u}
        : std::vector<uint32_t>{0x100u, 0x101u, 0x102u, 0x200u, 0x201u}, {0u, 1u, 2u});
    memory.words[0] = 1u;
    materialize({0x100u, 0x101u, 0x200u, 0x201u}, {0u, 1u});
    counts[0] = 1u;
    materialize({0x101u, 0x201u}, {1u});
    data[8] = 1u;
    materialize({0x101u, domain == Domain::SeparatePrefix ? 0x200u : 0x201u}, {1u});
    data[8] = 0u;
    counts[0] = 0u;
    materialize({0u, 0u}, {0u});
    counts[0] = 3u;
    data[2] = 0u;
    materialize({0x100u, 0x200u}, {0u});
    data[2] = 8u;
    memory.fail_address = 0x1004u;
    memory.selector_reads.clear();
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) &&
              memory.selector_reads == std::vector<std::pair<uint64_t, uint64_t>>{{0x1000u, 8u}},
          "shared selector reused a previous domain after a failed refresh");
    memory.fail_address = UINT64_MAX;
    materialize({0x100u, 0x101u, 0x200u, 0x201u}, {0u, 1u});
  }
  for (uint32_t invalid : {1u, 2u, 3u, 4u})
    CheckFatal([&] { make_plan(true, invalid); }, "not a valid runtime value",
               "compact table accepted an unproved selector or inconsistent descriptor width");
}

void TestScalarBufferSelectorDomain() {
  const auto make_plan = [](bool active = true, bool sparse = false, bool uniformize_first = false) {
    Fixture fixture;
    const auto material = fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                                          fixture.UserData(2), fixture.UserData(3)});
    MemoryInfo lds;
    lds.kind = ResourceKind::Lds;
    auto index = fixture.Emit(ValueOpcode::LoadSharedU32,
        {Value(8u), Value(true)}, fixture.AddMemory(lds, 0x10));
    if (uniformize_first)
      index = fixture.Emit(ValueOpcode::ReadFirstLane, {index, Value(active)});
    auto offset = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
        {index, Value(sparse ? 31u : 5u)});
    if (!sparse)
      offset = fixture.Emit(ValueOpcode::IAdd32,
          {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {offset, Value(2u)}), offset});
    offset = fixture.Emit(ValueOpcode::IAdd32, {offset, Value(3u)});
    if (!uniformize_first)
      offset = fixture.Emit(ValueOpcode::ReadFirstLane, {offset, Value(active)});
    MemoryInfo scalar;
    scalar.kind = ResourceKind::ScalarBuffer;
    scalar.offset = 147u;
    auto key = fixture.Emit(ValueOpcode::ReadConstBuffer,
        {material, offset}, fixture.AddMemory(scalar, 0x20));
    if (uniformize_first) key = fixture.Emit(ValueOpcode::UMin32, {key, Value(2u)});
    const auto table = fixture.Address(fixture.UserData(4), fixture.UserData(5));
    const auto record = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)});
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      scalar.kind = ResourceKind::ScalarAddress;
      scalar.offset = word * 4u;
      words[word] = fixture.Emit(ValueOpcode::LoadAddressU32,
          {table, record, Value(0u), Value(true)}, fixture.AddMemory(scalar, 0x30));
    }
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw,
        {fixture.Image(words), fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}),
         fixture.ImageAddress(), Value(true)}, fixture.AddMemory(sample, 0x40));
    fixture.PlanAndTrack();
    const auto &indirect = fixture.program.descriptor_sources[
        fixture.program.info.images[0].source].indirect_descriptor;
    if (!active && uniformize_first)
      Check(indirect && !indirect->selector && indirect->key_count == Value(3u),
            "scaled empty EXEC lane read retained an unproved selector");
    else Check(indirect && indirect->selector && indirect->key_count.IsEmpty() &&
              indirect->selector_first.IsEmpty() && indirect->selector->offset == 144u &&
              indirect->selector->stride == (sparse ? 0x80000000u : 32u),
          "scalar selector assumed an unwrapped 160-byte record or combined unaligned offsets");
    Check(key.ResolveInstruction()->Parent() != nullptr &&
              std::ranges::count_if(fixture.program.memory_info, [](const MemoryInfo &memory) {
                return memory.kind == ResourceKind::ScalarAddress && memory.planning_only;
              }) == 8u,
          "scalar selector lost its GPU key or retained descriptor-only address loads");
    return ExtractResourcePlan(fixture.program);
  };
  auto plan = make_plan();
  CheckFatal([&] { make_plan(false); }, "not a valid runtime value",
             "scalar selector accepted an empty EXEC lane read");
  make_plan(false, false, true);
  LinearTestMemory memory;
  memory.words[144u / 4u] = 1u;
  memory.words[160u / 4u] = 99u; // This field is outside the affine offset domain.
  memory.words[176u / 4u] = 2u;  // Reachable when the native 160*index multiplication wraps.
  for (uint32_t key = 0; key < 3u; ++key) {
    const auto word = (0x1000u + key * 32u) / 4u;
    memory.words[word] = 0x100u + key;
    memory.words[word + 1u] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }
  std::array<uint32_t, 6> user_data {0x1000u, 0u, 180u, 0u, 0x2000u, 0u};
  const SrtRuntime runtime {.user_data = user_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  memory.watched_address = 0x1000u + 144u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 3u && memory.watched_reads == 1u &&
            memory.watched_dwords == 9u && snapshot.images[0].dwords[0] == 0x100u &&
            snapshot.images[2].dwords[0] == 0x102u,
        "scalar selector interpreted batched padding or omitted the OOB zero key");
  Check(std::ranges::find(snapshot.specialization_reads,
            std::pair<uint64_t, uint64_t>{0x1090u, 36u}) != snapshot.specialization_reads.end(),
        "scalar selector batch escaped specialization overlap tracking");
  const auto scaled_lane = make_plan(true, false, true);
  memory.words[176u / 4u] = UINT32_MAX;
  Check(MaterializeResources(scaled_lane, runtime, snapshot, specialization) &&
            snapshot.images.size() == 3u && snapshot.images[0].dwords[0] == 0x100u &&
            snapshot.images[1].dwords[0] == 0x101u && snapshot.images[2].dwords[0] == 0x102u,
        "scaling after lane selection lost wrapped offsets, clamped keys, or the OOB zero key");
  memory.words[176u / 4u] = 2u;
  for (const auto size : {0u, 144u, 147u, 148u}) {
    user_data[2] = size;
    memory.watched_reads = 0u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == (size == 148u ? 2u : 1u) &&
              memory.watched_reads == (size == 148u ? 1u : 0u) &&
              snapshot.images[0].dwords[0] == 0x100u,
          "scalar selector read an incomplete DWORD or lost a fully OOB zero key");
  }
  user_data[2] = 180u;
  memory.words[176u / 4u] = 1u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 2u,
        "scalar selector reused stale keys after material memory changed");

  plan = make_plan(true, true);
  struct SparseMemory { std::vector<uint64_t> offsets; } sparse;
  const auto read_sparse = +[](void *userdata, uint64_t address, std::span<uint32_t> words) {
    auto &memory = *static_cast<SparseMemory *>(userdata);
    std::ranges::fill(words, 0u);
    if (words.size() == 1u) {
      memory.offsets.push_back(address - 0x1000u);
      words[0] = address == 0x1090u ? 1u : 2u;
      return address == 0x1090u || address == 0x80001090u;
    }
    if (words.size() != 8u || (address != 0x2020u && address != 0x2040u)) return false;
    words[0] = static_cast<uint32_t>(address);
    words[1] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    words[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    return true;
  };
  user_data[1] = 2u << 16u;
  user_data[2] = UINT32_MAX;
  Check(MaterializeResources(plan,
            {.user_data = user_data, .userdata = &sparse, .read_specialization_memory = read_sparse},
            snapshot, specialization) &&
            sparse.offsets == std::vector<uint64_t>{144u, 0x80000090u} &&
            snapshot.images.size() == 2u,
        "large V# bounds extended the native U32 domain or invented an OOB zero key");
}

void TestPrivateLdsForwarding() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant { Valid, MaskedLoad, Overlap, Duplicate, CrossLane, Truncated, OutOfBounds, Mask, Bypass };
  for (const auto variant : {Variant::Valid, Variant::MaskedLoad, Variant::Overlap, Variant::Duplicate,
                            Variant::CrossLane, Variant::Truncated, Variant::OutOfBounds,
                            Variant::Mask, Variant::Bypass}) {
    Fixture fixture;
    auto *entry = fixture.block;
    auto *body = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    const auto local = [&](uint32_t axis) {
      return fixture.Emit(ValueOpcode::GetBuiltin,
          {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(axis)});
    };
    const auto x = local(0u), y = local(1u);
    const auto address = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
        {fixture.Emit(ValueOpcode::IAdd32,
            {x, fixture.Emit(ValueOpcode::IMul32,
                {Value(16u), fixture.Emit(ValueOpcode::BitFieldUExtract,
                    {y, Value(0u), Value(variant == Variant::Truncated ? 1u : 24u)})})}), Value(2u)});
    const auto active = fixture.Emit(ValueOpcode::INotEqual32, {x, Value(0u)});
    entry->AddBranch(body);
    entry->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = body};
    if (variant == Variant::Bypass) {
      entry->condition = active;
      entry->AddBranch(exit);
      entry->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                          .true_block = body, .false_block = exit};
    }
    MemoryInfo memory;
    memory.kind = ResourceKind::Lds;
    memory.offset = 4096u;
    fixture.Emit(ValueOpcode::WriteSharedU32,
        {address, Value(0x12345678u), variant == Variant::Mask ? active : Value(true)},
        fixture.AddMemory(memory, 0x10), variant == Variant::Bypass ? body : entry);
    if (variant == Variant::Overlap || variant == Variant::Duplicate) {
      auto other = memory;
      other.offset += variant == Variant::Overlap ? 4u : 0u;
      fixture.Emit(ValueOpcode::WriteSharedU32, {address, Value(0u), Value(true)},
          fixture.AddMemory(other, 0x14), body);
    }
    body->AddBranch(exit);
    body->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = exit};
    const auto load = fixture.Emit(ValueOpcode::LoadSharedU32,
        {variant == Variant::CrossLane
             ? fixture.Emit(ValueOpcode::IAdd32, {address, Value(4u)}) : address,
         variant == Variant::MaskedLoad ? active : Value(true)},
        fixture.AddMemory(memory, 0x20), exit);
    const auto use = fixture.Emit(ValueOpcode::ReferenceU32, {load}, 0, exit);
    ShaderComputeInputInfo compute;
    compute.threads_num[0] = compute.threads_num[1] = 16u;
    compute.threads_num[2] = 1u;
    compute.lds_size_dwords = variant == Variant::OutOfBounds ? 1024u : 2048u;
    fixture.PlanAndTrack({.compute = &compute});
    const auto result = use.Instruction()->Arg(0).ResolveInstruction();
    Check(result != nullptr &&
              (result->GetOpcode() == ValueOpcode::SelectU32) ==
                  (variant == Variant::Valid || variant == Variant::MaskedLoad),
          "private LDS forwarding lost its value or crossed an alias, mask, bound or dominance hazard");
    if (variant == Variant::MaskedLoad)
      Check(result->Arg(0) == active && result->Arg(1) == Value(0x12345678u) &&
                result->Arg(2) == Value(0u),
            "forwarding an unconditional store lost the load's inactive-lane zero value");
  }
}

void TestCountedRowImageSelector() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant { Valid, Signed, StepTwo, SkippedIteration, WrongRow, CountStride,
                       KeyStride, CountImmediate, KeyImmediate, DifferentBackedgeBound,
                       Masked, EmptyCountMask, VaryingRow,
                       EmptyMask, UnprovenMask, WrongMask, AnyBound, MaskedStepTwo, Spilled,
                       MergedInitial, MissingBound, PreviousIteration,
                       InvariantMask, PossiblyEmptyMask };
  const auto make_plan = [](Variant variant = Variant::Valid, uint32_t capacity = 16u) {
    Fixture fixture;
    const bool masked = variant >= Variant::Masked;
    const bool cyclic_mask = variant >= Variant::InvariantMask;
    const bool stable_index = variant == Variant::DifferentBackedgeBound;
    auto *entry = fixture.block;
    auto *header = fixture.AddBlock();
    auto *body = fixture.AddBlock();
    auto *latch = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    auto *preheader = masked ? fixture.AddBlock() : entry;
    auto *prepare = variant == Variant::MergedInitial ? fixture.AddBlock() : preheader;
    const auto branch = [](Block *from, Block *to) {
      from->AddBranch(to);
      from->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = to};
    };
    branch(preheader, header);
    branch(body, latch);
    if (!stable_index) branch(latch, header);
    const auto keys = fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                                      fixture.UserData(2), fixture.UserData(3)});
    const auto counts = fixture.Buffer({fixture.UserData(4), fixture.UserData(5),
                                        fixture.UserData(6), fixture.UserData(7)});
    const auto material = fixture.Buffer({fixture.UserData(8), fixture.UserData(9),
                                          fixture.UserData(10), fixture.UserData(11)});
    auto row = fixture.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)), Value(0u)});
    MemoryInfo scalar;
    scalar.kind = ResourceKind::ScalarBuffer;
    Value active(true);
    if (masked) {
      const auto local = fixture.Emit(ValueOpcode::GetBuiltin,
          {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
      row = fixture.Emit(ValueOpcode::ReadConstBuffer,
          {keys, fixture.Emit(ValueOpcode::ShiftLeftLogical32, {row, Value(2u)})},
          fixture.AddMemory(scalar, 0x04));
      row = fixture.Emit(ValueOpcode::IMul32,
          {row, fixture.Emit(ValueOpcode::ConvertU32F32,
              {fixture.Emit(ValueOpcode::BitCastF32U32, {fixture.UserData(12u)})})});
      Check(ValidateWaveUniformValue(fixture.program, row) &&
                !ValidateRuntimeValue(fixture.program, row),
            "wave-uniform workgroup data escaped the host evaluation boundary");
      if (variant == Variant::VaryingRow)
        row = fixture.Emit(ValueOpcode::IAdd32, {row, local});
      if (cyclic_mask) {
        branch(entry, preheader);
        auto &mask = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U1));
        auto *restore = fixture.AddBlock();
        body->AddBranch(restore);
        body->condition = fixture.Emit(ValueOpcode::INotEqual32,
            {fixture.UserData(13u), Value(0u)}, 0, body);
        body->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                            .true_block = latch, .false_block = restore};
        branch(restore, latch);
        auto &next = latch->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U1));
        next.AddPhiOperand(body, Value(&mask));
        next.AddPhiOperand(restore, Value(variant == Variant::InvariantMask));
        mask.AddPhiOperand(preheader, Value(true));
        mask.AddPhiOperand(latch, Value(&next));
        active = Value(&mask);
      } else {
        active = fixture.Emit(ValueOpcode::INotEqual32, {local, Value(0u)});
        entry->condition = fixture.Emit(ValueOpcode::ConditionRef, {active},
                                         CFG::BranchCondition::ExecNonZero);
        if (variant == Variant::UnprovenMask) {
          entry->condition = fixture.Emit(ValueOpcode::INotEqual32,
              {fixture.UserData(13u), Value(0u)});
        }
        entry->AddBranch(prepare);
        entry->AddBranch(exit);
        entry->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                              .true_block = prepare, .false_block = exit};
      }
      if (variant == Variant::EmptyMask) active = Value(false);
    }
    Value initial(0u);
    if (variant == Variant::MergedInitial) {
      auto *zero = fixture.AddBlock();
      prepare->condition = fixture.Emit(ValueOpcode::INotEqual32,
          {fixture.UserData(13u), Value(0u)}, 0, prepare);
      prepare->AddBranch(zero);
      prepare->AddBranch(preheader);
      prepare->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                              .true_block = zero, .false_block = preheader};
      branch(zero, preheader);
      auto &merged = preheader->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
      merged.AddPhiOperand(prepare, fixture.UserData(14u));
      merged.AddPhiOperand(zero, Value(0u));
      initial = Value(&merged);
    }
    Value spill_address;
    MemoryInfo spill;
    spill.kind = ResourceKind::Lds;
    spill.offset = 4096u;
    if (variant == Variant::Spilled) {
      spill_address = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
          {fixture.Emit(ValueOpcode::GetBuiltin,
              {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)}), Value(2u)});
      row = fixture.Emit(ValueOpcode::SelectU32, {active, row, Value(0xdeadbeefu)});
      fixture.Emit(ValueOpcode::WriteSharedU32, {spill_address, row, active},
                   fixture.AddMemory(spill, 0x08));
    }
    auto count_row = row;
    if (masked) count_row = fixture.Emit(ValueOpcode::ReadFirstLane,
        {row, variant == Variant::Spilled ? active : Value(variant != Variant::EmptyCountMask)});
    if (variant == Variant::WrongRow)
      count_row = fixture.Emit(ValueOpcode::IAdd32, {row, Value(1u)});
    const auto count_offset = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
        {count_row, Value(variant == Variant::CountStride ? 3u : 2u)});
    scalar.offset = variant == Variant::CountImmediate ? 4u : 0u;
    const auto count = fixture.Emit(ValueOpcode::ReadConstBuffer,
        {counts, count_offset}, fixture.AddMemory(scalar, 0x10));
    auto *induction = stable_index ? nullptr
        : &header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    auto index = stable_index ? fixture.UserData(13u) : Value(induction);
    const auto guard = fixture.Emit(variant == Variant::Signed ? ValueOpcode::SLessThan32
                                                              : ValueOpcode::ULessThan32,
        {index, count}, 0, header);
    header->AddBranch(body);
    if (variant == Variant::PreviousIteration) {
      header->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = body};
      latch->AddBranch(exit);
      latch->condition = guard;
      latch->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                            .true_block = header, .false_block = exit};
    } else {
      auto *outside = variant == Variant::SkippedIteration ? latch : exit;
      header->AddBranch(outside);
      header->condition = variant == Variant::AnyBound
          ? fixture.Emit(ValueOpcode::ConditionRef, {guard}, CFG::BranchCondition::ExecNonZero, header)
          : variant == Variant::MissingBound
              ? fixture.Emit(ValueOpcode::INotEqual32, {fixture.UserData(13u), Value(0u)}, 0, header)
              : guard;
      header->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                            .true_block = body, .false_block = outside};
    }
    if (stable_index) {
      const auto backedge_count = fixture.Emit(ValueOpcode::ReadConstBuffer,
          {keys, count_offset}, fixture.AddMemory(scalar, 0x14), latch);
      latch->condition = fixture.Emit(ValueOpcode::ULessThan32, {index, backedge_count}, 0, latch);
      latch->AddBranch(body);
      latch->AddBranch(exit);
      latch->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                            .true_block = body, .false_block = exit};
    }
    fixture.block = body;
    if (variant == Variant::Spilled) {
      row = fixture.Emit(ValueOpcode::SelectU32,
          {active, fixture.Emit(ValueOpcode::LoadSharedU32, {spill_address, active},
                                 fixture.AddMemory(spill, 0x18)), Value(0xbadbeefu)});
    }
    const auto row_start = fixture.Emit(ValueOpcode::IMul32, {row, Value(capacity)});
    if (variant == Variant::KeyStride)
      index = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {index, Value(1u)});
    auto element = fixture.Emit(ValueOpcode::IAdd32, {row_start, index});
    if (masked) {
      const auto write_mask = variant == Variant::WrongMask
          ? fixture.Emit(ValueOpcode::LogicalNot, {active}) : active;
      element = fixture.Emit(ValueOpcode::ReadFirstLane,
          {fixture.Emit(ValueOpcode::SelectU32, {write_mask, element, Value(0xdeadbeefu)}), active});
    }
    const auto key_offset = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {element, Value(2u)});
    scalar.offset = variant == Variant::KeyImmediate ? 4u : 0u;
    const auto key = fixture.Emit(ValueOpcode::ReadConstBuffer,
        {keys, key_offset}, fixture.AddMemory(scalar, 0x20));
    const auto record = fixture.Emit(ValueOpcode::IAdd32,
        {fixture.Emit(ValueOpcode::IMul32, {key, Value(396u)}), Value(184u)});
    std::array<Value, 8> image_words;
    for (uint32_t word = 0; word < image_words.size(); ++word) {
      scalar.offset = word * 4u;
      image_words[word] = fixture.Emit(ValueOpcode::ReadConstBuffer,
          {material, record}, fixture.AddMemory(scalar, 0x30));
    }
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw,
        {fixture.Image(image_words), fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}),
         fixture.ImageAddress(), Value(true)}, fixture.AddMemory(sample, 0x40));
    if (induction != nullptr) {
      const auto step = fixture.Emit(ValueOpcode::IAdd32,
          {Value(induction), Value(variant == Variant::StepTwo ||
                                   variant == Variant::MaskedStepTwo ? 2u : 1u)}, 0, latch);
      induction->AddPhiOperand(preheader, initial);
      induction->AddPhiOperand(latch, step);
    }
    ShaderComputeInputInfo compute;
    compute.threads_num[0] = 64u;
    compute.threads_num[1] = compute.threads_num[2] = 1u;
    compute.lds_size_dwords = 2048u;
    fixture.PlanAndTrack({.compute = &compute});
    const auto &source = fixture.program.descriptor_sources[fixture.program.info.images[0].source];
    Check(source.indirect_descriptor.has_value(), "counted-row image lost its indirect source");
    const auto &selector = source.indirect_descriptor->selector;
    const bool valid = variant == Variant::Valid || variant == Variant::Masked || variant == Variant::Spilled ||
                       variant == Variant::EmptyCountMask || variant == Variant::MergedInitial ||
                       variant == Variant::StepTwo || variant == Variant::MaskedStepTwo ||
                       variant == Variant::SkippedIteration || variant == Variant::InvariantMask || stable_index;
    Check((selector && selector->rows.has_value()) == valid,
          "counted-row selector lost its proof or accepted mismatched row/induction bounds");
    if (valid)
      Check(selector->stride == 4u && selector->offset == 0u &&
                selector->rows->stride == uint64_t(capacity) * 4u,
            "counted-row selector changed native row or element byte spacing");
    if (stable_index)
      Check(EquivalentValue(fixture.program,
                fixture.program.descriptor_sources[selector->rows->source].dwords[0],
                counts.Instruction()->Arg(0)),
            "a rejected backedge bound replaced the count source proven on entry");
    return ExtractResourcePlan(fixture.program);
  };
  auto plan = make_plan(Variant::MergedInitial);
  make_plan();
  make_plan(Variant::Masked);
  make_plan(Variant::Spilled);
  make_plan(Variant::EmptyCountMask);
  make_plan(Variant::DifferentBackedgeBound);
  make_plan(Variant::InvariantMask);
  make_plan(Variant::PossiblyEmptyMask);
  for (const auto variant : {Variant::Signed, Variant::StepTwo, Variant::SkippedIteration,
                             Variant::WrongRow, Variant::CountStride, Variant::KeyStride,
                             Variant::CountImmediate, Variant::KeyImmediate, Variant::VaryingRow,
                             Variant::EmptyMask, Variant::UnprovenMask, Variant::WrongMask,
                             Variant::AnyBound, Variant::MaskedStepTwo,
                             Variant::MissingBound, Variant::PreviousIteration})
    make_plan(variant);

  constexpr uint32_t CountBase = 0x1000u, KeyBase = 0x2000u, MaterialBase = 0x4000u;
  // This key wraps 396*key to72, aliasing an unrelated field at material byte256.
  constexpr uint32_t PoisonKey = 0x345d1746u;
  LinearTestMemory memory;
  memory.words.resize((MaterialBase - memory.base + 4u * 396u) / 4u);
  const auto at = [&](uint32_t address) -> uint32_t& {
    return memory.words[(address - memory.base) / 4u];
  };
  for (uint32_t index = 0; index < 64u; ++index) at(KeyBase + index * 4u) = PoisonKey;
  for (uint32_t key = 0; key < 4u; ++key) {
    const auto address = MaterialBase + key * 396u + 184u;
    at(address) = 0x100u + key;
    at(address + 4u) = static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    at(address + 12u) = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }
  std::copy_n(memory.words.begin() + (MaterialBase + 184u - memory.base) / 4u, 8u,
              memory.words.begin() + (MaterialBase + 32u - memory.base) / 4u);
  at(MaterialBase + 32u) = 0x104u;
  at(MaterialBase + 256u) = 0x200u;
  at(MaterialBase + 260u) = static_cast<uint32_t>(Libs::Graphics::Prospero::BufferFormat::k8SScaled) << 20u;
  at(MaterialBase + 268u) = at(MaterialBase + 196u);
  // The walker needs every user-data DWORD the plan reads, including the zero s12-s14.
  std::array<uint32_t, 15> user_data{KeyBase, 4u << 16u, 64u, 0x14004u,
      CountBase, 4u << 16u, 4u, 0x14004u, MaterialBase, 396u << 16u, 4u, 0x14004u};
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto captured = [&](uint64_t address, uint64_t size) {
    return std::ranges::any_of(snapshot.specialization_reads, [&](const auto &span) {
      return span.first <= address && address - span.first <= span.second &&
          size <= span.second - (address - span.first);
    });
  };
  const auto check = [&](std::initializer_list<uint32_t> selected) {
    memory.watched_address = KeyBase;
    memory.watched_reads = 0u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization),
          "counted-row selector interpreted an unused slot or failed coherent discovery");
    Check(captured(CountBase, user_data[6] * 4u),
          "row counts escaped writable-overlap capture");
    std::vector<uint32_t> expected(selected);
    std::ranges::sort(expected);
    expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
    if (expected.empty()) {
      Check(memory.watched_reads == 0u && snapshot.images.size() == 1u &&
                std::ranges::all_of(snapshot.images[0].dwords, [](uint32_t word) { return word == 0u; }),
            "empty row counts read unused keys or retained stale image candidates");
      return;
    }
    if (expected.size() > 1u) {
      const auto map = specialization.images[0].indirect_mapping_offset;
      Check(snapshot.flattened_srt[map] == expected.size(), "counted-row mapping lost selected keys");
      for (uint32_t index = 0; index < expected.size(); ++index)
        Check(snapshot.flattened_srt[map + 1u + index * 2u] == expected[index],
              "counted-row mapping changed an unsigned key or retained an unused slot");
    }
    std::vector<uint32_t> image_addresses;
    for (const auto key : expected) {
      const auto offset = uint32_t(key * 396u) + 184u;
      image_addresses.push_back(at(MaterialBase + offset));
    }
    std::ranges::sort(image_addresses);
    image_addresses.erase(std::unique(image_addresses.begin(), image_addresses.end()), image_addresses.end());
    Check(snapshot.images.size() == image_addresses.size() &&
              std::ranges::all_of(snapshot.images, [&](const DescriptorValue &image) {
                return std::ranges::find(image_addresses, image.dwords[0]) != image_addresses.end();
              }), "counted rows lost a selected image or retained an unrelated descriptor");
  };
  check({});
  at(CountBase) = 2u; at(CountBase + 8u) = 1u;
  at(KeyBase) = 2u; at(KeyBase + 4u) = 1u; at(KeyBase + 128u) = 3u;
  check({1u, 2u, 3u});
  Check(captured(KeyBase, 8u) && captured(KeyBase + 128u, 4u),
        "active row keys escaped writable-overlap capture");
  at(KeyBase) = 0x80000001u;
  check({1u, 3u, 0x80000001u});
  at(KeyBase) = 0x1890cedeu; // 396*key +184 wraps to material byte32.
  check({1u, 3u, 0x1890cedeu});
  at(CountBase) = 0u;
  user_data[2] = 1u; // The selected second row reads beyond the index V# and supplies key0.
  check({0u});
  user_data[2] = 64u;
  at(CountBase + 8u) = 0u;
  check({});
  at(CountBase) = 1u;
  at(KeyBase) = PoisonKey;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "an actively selected malformed material descriptor was suppressed");
  at(KeyBase) = 2u;
  check({2u});
  for (const auto fail : {CountBase, KeyBase}) {
    memory.fail_address = fail;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization),
          "failed count/key discovery reused stale selector data");
  }
  memory.fail_address = UINT64_MAX;
  for (uint32_t index = 0; index < 17u; ++index) at(KeyBase + index * 4u) = 1u;
  at(CountBase) = 17u;
  check({1u}); // Native counts can spill past the row spacing into the next row's slots.
  at(CountBase + 4u) = 1u;
  check({1u}); // Overlapping nonempty prefixes preserve the union without duplicating images.
  at(CountBase + 4u) = 0u;
  for (uint32_t index = 0; index < 64u; ++index) at(KeyBase + index * 4u) = 1u;
  at(CountBase) = MaxIndirectDescriptorProbes;
  check({0u, 1u}); // The exact cap is valid; selected OOB scalar entries contribute key0.
  for (const auto count : {65537u, 0x80000000u, UINT32_MAX}) {
    at(CountBase) = count;
    memory.watched_reads = 0u;
    Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.watched_reads == 0u,
          "unsigned row count exceeded the probe cap or wrapped before index discovery");
  }
  at(CountBase) = 0u;
  user_data[6] = MaxIndirectDescriptorProbes + 1u;
  memory.reads = 0u;
  Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.reads == 0u,
        "counted-row discovery exceeded the existing row probe cap");
  user_data[6] = 0u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.reads == 0u &&
            snapshot.images.size() == 1u && snapshot.images[0].dwords[0] == 0u,
        "empty count V# manufactured a selector key or retained stale images");

  user_data[6] = 5u;
  at(CountBase + 16u) = 1u;
  auto wrap_plan = make_plan(Variant::Valid, 1u << 28u);
  memory.watched_reads = 0u;
  Check(!MaterializeResources(wrap_plan, runtime, snapshot, specialization) && memory.watched_reads == 0u,
        "row address multiplication wrapped before the active prefix was validated");

  struct OverflowMemory { uint32_t count_reads = 0, other_reads = 0; } overflow;
  user_data[6] = MaxIndirectDescriptorProbes / 16u + 1u;
  const auto read_overflow = +[](void *data, uint64_t address, std::span<uint32_t> words) {
    auto &memory = *static_cast<OverflowMemory *>(data);
    if (address == CountBase && words.size() == MaxIndirectDescriptorProbes / 16u + 1u) {
      ++memory.count_reads;
      std::ranges::fill(words, 16u);
      return true;
    }
    ++memory.other_reads;
    return false;
  };
  Check(!MaterializeResources(plan,
            {.user_data = user_data, .userdata = &overflow, .read_specialization_memory = read_overflow},
            snapshot, specialization) && overflow.count_reads == 1u && overflow.other_reads == 0u,
        "sum of active row prefixes exceeded the probe cap or read keys before validation");

  auto sparse_plan = make_plan(Variant::Valid, 1u << 16u);
  user_data[2] = 2u * (1u << 16u) + 1u;
  user_data[6] = 3u;
  struct SparseRows { LinearTestMemory *material; uint32_t key_reads = 0u; } sparse{&memory};
  const auto read_sparse = +[](void *data, uint64_t address, std::span<uint32_t> words) {
    auto &memory = *static_cast<SparseRows *>(data);
    if (address == CountBase && words.size() == 3u) {
      words[0] = words[2] = 1u; words[1] = 0u;
      return true;
    }
    if (address == KeyBase || address == KeyBase + 0x80000u) {
      if (words.size() != 1u) return false;
      ++memory.key_reads;
      words[0] = address == KeyBase ? 2u : 3u;
      return true;
    }
    return ReadLinearTestMemory(memory.material, address, words);
  };
  Check(MaterializeResources(sparse_plan,
            {.user_data = user_data, .userdata = &sparse, .read_specialization_memory = read_sparse},
            snapshot, specialization) && sparse.key_reads == 2u && snapshot.images.size() == 2u,
        "sparse rows exceeded the batch cap or read unselected gaps");
}

void TestSharedAppendSelectorBounds() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant {
    Valid, PayloadOob, MissingInit, InactiveInit, UnknownInit, DuplicateAtomic,
    WrongAtomicGuard, UnguardedCandidate, WrongStep, WrongLocalIndex, Multiwave,
    OuterReentry, InnerAppendCycle, EarlyCounterRead, WrongConsumerBound, ExtraWriter, CounterAlias,
    PayloadWrap, CounterOob, Lane, LaneUnimpliedRead, LaneUnimpliedCounter, LaneCounterMask,
    LaneOtherBypass, LaneInactiveSelect
  };
  const auto make_plan = [](Variant variant, uint32_t first_base = 1032u, uint32_t consumed_base = 8u) {
    Fixture fixture;
    ShaderComputeInputInfo compute;
    compute.threads_num[0] = 8u;
    compute.threads_num[1] = variant == Variant::Multiwave ? 16u : 8u;
    compute.threads_num[2] = 1u;
    compute.lds_size_dwords = variant == Variant::PayloadOob ? 2u :
                             variant == Variant::CounterOob ? 1u : 640u;
    compute.host_subgroup_size = 32u;
    fixture.program.wave_size = compute.wave_size = 64u;
    const auto branch = [](Block *from, Block *to) {
      from->AddBranch(to);
      from->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = to};
    };
    const auto choose = [&](Block *from, Block *yes, Block *no, Value condition,
                            CFG::BranchCondition kind) {
      from->AddBranch(yes);
      from->AddBranch(no);
      from->condition = fixture.Emit(ValueOpcode::ConditionRef, {condition}, kind, from);
      from->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                         .true_block = yes, .false_block = no};
    };
    const auto shared = [&](uint32_t offset) {
      MemoryInfo memory;
      memory.kind = ResourceKind::Lds;
      memory.offset = offset;
      return fixture.AddMemory(memory, 0x10u);
    };
    auto *entry = fixture.block;
    auto *initialize = fixture.AddBlock();
    auto *initialized = fixture.AddBlock();
    const bool lane = variant >= Variant::Lane;
    const auto material = fixture.Buffer({fixture.UserData(0), fixture.UserData(1),
                                          fixture.UserData(2), fixture.UserData(3)});
    const auto table = fixture.Address(fixture.UserData(4), fixture.UserData(5));
    const auto counts = fixture.Address(fixture.UserData(6), fixture.UserData(7));
    std::array<Value, 2> count;
    for (uint32_t index = 0; index < count.size(); ++index) {
      MemoryInfo memory;
      memory.kind = ResourceKind::ScalarAddress;
      memory.offset = index * 4u;
      count[index] = fixture.Emit(ValueOpcode::LoadAddressU32,
          {counts, Value(0u), Value(0u), Value(true)}, fixture.AddMemory(memory, 0x20u));
    }
    const auto x = fixture.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
    const auto y = fixture.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(1u)});
    const auto local = fixture.Emit(ValueOpcode::IAdd32,
        {x, fixture.Emit(ValueOpcode::ShiftLeftLogical32, {y, Value(3u)})});
    // The negated form skips the initializer only when every lane is nonzero.
    const auto local_is = [&](uint32_t value) {
      return fixture.Emit(ValueOpcode::LogicalNot,
          {fixture.Emit(ValueOpcode::INotEqual32, {local, Value(value)})});
    };
    auto zero_lane = lane ? local_is(0u) : fixture.Emit(ValueOpcode::IEqual32, {local, Value(0u)});
    if (variant == Variant::InactiveInit) zero_lane = Value(false);
    if (variant == Variant::UnknownInit)
      zero_lane = fixture.Emit(ValueOpcode::LogicalAnd,
          {zero_lane, fixture.Emit(ValueOpcode::IEqual32, {fixture.UserData(8), Value(0u)})});
    choose(entry, initialize, initialized, variant == Variant::LaneOtherBypass ? local_is(1u) : zero_lane,
           CFG::BranchCondition::ExecNonZero);
    if (variant != Variant::MissingInit)
      fixture.Emit(ValueOpcode::WriteSharedU32x2,
          {Value(0u), Value(0u), Value(0u), zero_lane}, shared(0u), initialize);
    branch(initialize, initialized);
    Value final_counter;
    if (variant == Variant::EarlyCounterRead)
      final_counter = fixture.Emit(ValueOpcode::LoadSharedU32,
          {Value(0u), Value(true)}, shared(0u), initialized);
    auto *previous = initialized;
    for (uint32_t list = 0; list < 2u; ++list) {
      auto *header = fixture.AddBlock();
      auto *cull = fixture.AddBlock();
      auto *append = fixture.AddBlock();
      auto *latch = fixture.AddBlock();
      auto *exit = fixture.AddBlock();
      branch(previous, header);
      auto &batch = header->AppendNewInst(ValueOpcode::Phi);
      batch.SetFlags(Type::U32);
      const auto batch_valid = fixture.Emit(ValueOpcode::ULessThan32,
          {Value(&batch), count[list]}, 0, header);
      choose(header, cull, exit, batch_valid, CFG::BranchCondition::SccNonZero);
      const auto local_candidate = variant == Variant::WrongLocalIndex
          ? fixture.Emit(ValueOpcode::IAdd32, {local, x}, 0, cull) : local;
      const auto candidate = fixture.Emit(ValueOpcode::IAdd32, {Value(&batch), local_candidate}, 0, cull);
      const auto even = fixture.Emit(ValueOpcode::IEqual32,
          {fixture.Emit(ValueOpcode::BitwiseAnd32, {candidate, Value(1u)}, 0, cull), Value(0u)}, 0, cull);
      auto active = even;
      if (variant != Variant::UnguardedCandidate) {
        const auto candidate_valid = list == 0u
            ? fixture.Emit(ValueOpcode::LogicalNot,
                {fixture.Emit(ValueOpcode::ULessThanEqual32, {count[list], candidate}, 0, cull)}, 0, cull)
            : fixture.Emit(ValueOpcode::ULessThan32, {candidate, count[list]}, 0, cull);
        active = fixture.Emit(ValueOpcode::LogicalAnd,
            {candidate_valid, even}, 0, cull);
      }
      choose(cull, append, latch, active, CFG::BranchCondition::ExecNonZero);
      const auto atomic_flags = shared(list == 0u || variant == Variant::CounterAlias ? 4u : 0u);
      const auto atomic_guard = variant == Variant::WrongAtomicGuard ? Value(true) : active;
      const auto ordinal = fixture.Emit(ValueOpcode::SharedAtomicIAdd32,
          {Value(0u), Value(1u), atomic_guard}, atomic_flags, append);
      if (variant == Variant::DuplicateAtomic && list == 1u)
        fixture.Emit(ValueOpcode::SharedAtomicIAdd32,
            {Value(0u), Value(1u), active}, atomic_flags, append);
      const auto address = fixture.Emit(ValueOpcode::ShiftLeftLogical32,
          {fixture.Emit(ValueOpcode::SelectU32, {active, ordinal, Value(UINT32_MAX)}, 0, append), Value(2u)},
          0, append);
      const auto payload = fixture.Emit(ValueOpcode::SelectU32,
          {active, candidate, Value(UINT32_MAX)}, 0, append);
      const auto base = list == 0u ? first_base :
          variant == Variant::PayloadWrap ? 0xfffffff8u : consumed_base;
      fixture.Emit(ValueOpcode::WriteSharedU32, {address, payload, active}, shared(base), append);
      if (variant == Variant::InnerAppendCycle && list == 1u) {
        auto *repeat = fixture.AddBlock();
        branch(append, repeat);
        choose(repeat, append, latch, even, CFG::BranchCondition::ExecNonZero);
      } else {
        branch(append, latch);
      }
      const auto step = fixture.Emit(ValueOpcode::IAdd32,
          {Value(&batch), Value(variant == Variant::WrongStep ? 32u : 64u)}, 0, latch);
      branch(latch, header);
      batch.AddPhiOperand(previous, Value(0u));
      batch.AddPhiOperand(latch, step);
      previous = exit;
    }
    if (variant == Variant::ExtraWriter)
      fixture.Emit(ValueOpcode::WriteSharedU32,
          {Value(0u), Value(UINT32_MAX), Value(true)}, shared(12u), previous);
    // Lane variants consume under a lane mask, which their counter and record reads use.
    Value lanes(true), other;
    if (lane) {
      lanes = fixture.Emit(ValueOpcode::ULessThan32, {y, fixture.UserData(8)}, 0, previous);
      other = fixture.Emit(ValueOpcode::ULessThan32, {x, fixture.UserData(8)}, 0, previous);
    }
    auto *guarded = lane ? fixture.AddBlock() : previous;
    auto *header = fixture.AddBlock();
    auto *body = fixture.AddBlock();
    auto *latch = fixture.AddBlock();
    auto *exit = fixture.AddBlock();
    if (lane) choose(previous, guarded, exit, lanes, CFG::BranchCondition::ExecNonZero);
    previous = guarded;
    if (final_counter.IsEmpty())
      final_counter = fixture.Emit(ValueOpcode::LoadSharedU32,
          {Value(0u), variant == Variant::LaneUnimpliedCounter ? other : lanes}, shared(0u), previous);
    if (lane)
      final_counter = fixture.Emit(ValueOpcode::ReadFirstLane,
          {final_counter, variant == Variant::LaneCounterMask ? other : lanes}, 0, previous);
    branch(previous, header);
    auto &index = header->AppendNewInst(ValueOpcode::Phi);
    index.SetFlags(Type::U32);
    const auto in_list = fixture.Emit(ValueOpcode::ULessThan32,
        {Value(&index), variant == Variant::WrongConsumerBound ? count[1] : final_counter}, 0, header);
    choose(header, body, exit, in_list, CFG::BranchCondition::VccNonZero);
    fixture.block = body;
    const auto selected = fixture.Emit(ValueOpcode::LoadSharedU32,
        {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {Value(&index), Value(2u)}),
         variant == Variant::LaneUnimpliedRead ? other : lanes}, shared(consumed_base));
    Value offset;
    if (lane) {
      // The record is selected from one lane and scaled on the scalar side.
      const auto mask = variant == Variant::LaneInactiveSelect
          ? fixture.Emit(ValueOpcode::LogicalAnd, {lanes, other}) : lanes;
      offset = fixture.Emit(ValueOpcode::IMul32,
          {fixture.Emit(ValueOpcode::ReadFirstLane, {selected, mask}), Value(160u)});
    } else {
      const auto offset32 = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {selected, Value(5u)});
      offset = fixture.Emit(ValueOpcode::ReadFirstLane,
          {fixture.Emit(ValueOpcode::IAdd32,
               {fixture.Emit(ValueOpcode::ShiftLeftLogical32, {offset32, Value(2u)}), offset32}),
           Value(true)});
    }
    MemoryInfo scalar;
    scalar.kind = ResourceKind::ScalarBuffer;
    scalar.offset = 144u;
    const auto key = fixture.Emit(ValueOpcode::ReadConstBuffer,
        {material, offset}, fixture.AddMemory(scalar, 0x30u));
    const auto record = fixture.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(5u)});
    std::array<Value, 8> words;
    for (uint32_t word = 0; word < words.size(); ++word) {
      scalar.kind = ResourceKind::ScalarAddress;
      scalar.offset = word * 4u;
      words[word] = fixture.Emit(ValueOpcode::LoadAddressU32,
          {table, record, Value(0u), Value(true)}, fixture.AddMemory(scalar, 0x40u));
    }
    MemoryInfo sample;
    sample.kind = ResourceKind::Image;
    sample.image_dimension = Decoder::ImageDimension::Dim2D;
    fixture.Emit(ValueOpcode::ImageSampleRaw,
        {fixture.Image(words), fixture.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)}),
         fixture.ImageAddress(), Value(true)}, fixture.AddMemory(sample, 0x50u));
    branch(body, latch);
    const auto next = fixture.Emit(ValueOpcode::IAdd32, {Value(&index), Value(1u)}, 0, latch);
    branch(latch, header);
    index.AddPhiOperand(previous, Value(0u));
    index.AddPhiOperand(latch, next);
    if (variant == Variant::OuterReentry) branch(exit, initialized);
    fixture.PlanAndTrack({.compute = &compute});
    const auto &indirect = fixture.program.descriptor_sources[
        fixture.program.info.images[0].source].indirect_descriptor;
    const bool proven = variant == Variant::Valid || variant == Variant::PayloadOob ||
                        variant == Variant::Lane;
    Check(indirect && indirect->selector && indirect->selector->offset == 144u &&
              indirect->selector->stride == (proven ? 160u : 32u) &&
              indirect->key_count.IsEmpty() != proven &&
              (proven ? indirect->selector_first == Value(0u) : indirect->selector_first.IsEmpty()),
          proven ? "shared append provenance did not narrow the material record domain" :
                   "shared append provenance accepted an unproved initialization, bound or ordering");
    return ExtractResourcePlan(fixture.program);
  };
  auto plan = make_plan(Variant::Valid);
  make_plan(Variant::PayloadOob); // Ordinary LDS reads beyond the allocation return zero.
  for (const auto variant : {Variant::MissingInit, Variant::InactiveInit, Variant::UnknownInit,
      Variant::DuplicateAtomic, Variant::WrongAtomicGuard, Variant::UnguardedCandidate,
      Variant::WrongStep, Variant::WrongLocalIndex, Variant::Multiwave, Variant::OuterReentry,
      Variant::InnerAppendCycle, Variant::EarlyCounterRead, Variant::WrongConsumerBound, Variant::ExtraWriter,
      Variant::CounterAlias, Variant::PayloadWrap, Variant::CounterOob})
    make_plan(variant);
  auto lane_plan = make_plan(Variant::Lane);
  for (const auto variant : {Variant::LaneUnimpliedRead, Variant::LaneUnimpliedCounter,
      Variant::LaneCounterMask, Variant::LaneOtherBypass, Variant::LaneInactiveSelect})
    CheckFatal([&] { make_plan(variant); }, "not a valid runtime value",
               "shared append provenance accepted an unimplied lane mask or initializer bypass");

  LinearTestMemory memory;
  for (uint32_t index = 0; index < 8u; ++index) memory.words[(144u + 160u * index) / 4u] = index + 1u;
  memory.words[176u / 4u] = 0xdeadbeefu; // A different field reachable only through unbounded U32 wrap.
  for (uint32_t key = 0; key <= 8u; ++key) {
    const auto word = (0x1000u + key * 32u) / 4u;
    memory.words[word] = 0x100u + key;
    memory.words[word + 1u] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    memory.words[word + 3u] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
  }
  std::array<uint32_t, 9> user_data {0x1000u, 160u << 16u, 8u, 0u, 0x2000u, 0u, 0x1800u, 0u, 8u};
  SrtRuntime runtime {.user_data = user_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadRuntimeMemory, &runtime);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  memory.fail_address = memory.watched_address = 0x1090u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 1u && memory.descriptor_reads == 0u && memory.watched_reads == 0u,
        "empty shared append lists probed material keys or image descriptors");
  memory.fail_address = UINT64_MAX;
  for (const auto counts : {std::array{1u, 2u}, std::array{3u, 1u}, std::array{1u, 1u}, std::array{0u, 0u}}) {
    memory.words[0x800u / 4u] = counts[0];
    memory.words[0x804u / 4u] = counts[1];
    const auto expected = counts[1];
    for (const auto *tested : {&plan, &lane_plan})
      Check(MaterializeResources(*tested, runtime, snapshot, specialization) &&
                snapshot.images.size() == std::max(expected, 1u) &&
                (expected == 0u || snapshot.images.back().dwords[0] == 0x100u + expected),
            "disjoint shared append lists or refreshed counts selected unrelated material keys");
  }
  memory.words[0x800u / 4u] = 24u;
  memory.words[0x804u / 4u] = 2u;
  memory.words[(144u + 320u) / 4u] = 0xbe7cc577u;
  memory.fail_address = 0x1000u + 144u + 320u;
  user_data[2] = 24u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && snapshot.images.size() == 2u &&
            snapshot.images[0].dwords[0] == 0x101u && snapshot.images[1].dwords[0] == 0x102u,
        "a disjoint 24-entry list exposed float fields after the two active material records");
  for (const uint64_t address : {0x1800u, 0x1804u})
    Check(std::ranges::find(snapshot.specialization_reads, std::pair{address, uint64_t{4u}}) !=
              snapshot.specialization_reads.end(),
          "a disjoint append producer escaped count capture");
  memory.fail_address = UINT64_MAX;
  user_data[2] = 8u;
  memory.words[0x800u / 4u] = 1u;
  memory.words[0x804u / 4u] = 2u;
  memory.words[(144u + 160u) / 4u] = 4u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 2u && snapshot.images.back().dwords[0] == 0x104u,
        "shared append materialization reused stale selector contents");
  // Record 1 lies past a one-record V#, so its scalar key reads zero instead of key 4.
  user_data[2] = 1u;
  memory.fail_address = 0x1000u + 144u + 160u;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.images.size() == 2u && snapshot.images[0].dwords[0] == 0x100u &&
            snapshot.images[1].dwords[0] == 0x101u,
        "shared append materialization did not read records past its V# as zero");
  memory.fail_address = UINT64_MAX;
  user_data[2] = 8u;
  for (const auto too_large : {65537u, UINT32_MAX}) {
    for (uint32_t list = 0; list < 2u; ++list) {
      memory.words[0x800u / 4u] = memory.words[0x804u / 4u] = 0u;
      memory.words[0x800u / 4u + list] = too_large;
      memory.watched_reads = 0u;
      Check(!MaterializeResources(plan, runtime, snapshot, specialization) && memory.watched_reads == 0u,
            "inactive consumer hid an oversized producer or wrapped append address");
    }
  }

  struct BoundaryMemory {
    uint32_t keys_read = 0;
    std::array<uint32_t, 2> counts{0u, 65536u};
  } boundary;
  const auto read_boundary = +[](void *userdata, uint64_t address, std::span<uint32_t> words) {
    auto &memory = *static_cast<BoundaryMemory *>(userdata);
    std::ranges::fill(words, 0u);
    if (words.size() == 1u && (address == 0x40000000u || address == 0x40000004u)) {
      words[0] = memory.counts[(address - 0x40000000u) / 4u];
      return true;
    }
    if (address >= 0x1090u && (address - 0x1090u) % 160u == 0u &&
        address - 0x1090u + words.size_bytes() <= 65535u * 160u + 4u) {
      std::ranges::fill(words, 0xdeadbeefu);
      for (size_t word = 0; word < words.size(); word += 40u) {
        words[word] = 1u;
        ++memory.keys_read;
      }
      return true;
    }
    if (words.size() != 8u || address != 0x50000020u) return false;
    words[0] = 0x101u;
    words[1] = static_cast<uint32_t>(
        Libs::Graphics::Prospero::BufferFormat::k32_32_32_32Float) << 20u;
    words[3] = Libs::Graphics::DstSel(4, 5, 6, 7) |
        (static_cast<uint32_t>(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    return true;
  };
  user_data[2] = 65536u;
  user_data[4] = 0x50000000u;
  user_data[6] = 0x40000000u;
  runtime = {.user_data = user_data, .userdata = &boundary, .read_specialization_memory = read_boundary};
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            boundary.keys_read == 65536u && snapshot.images.size() == 1u &&
            snapshot.images[0].dwords[0] == 0x101u,
        "shared append materialization rejected its exact nonwrapping probe limit");
  const std::array bases{std::pair{1032u, 8u}, std::pair{8u, 1032u}, std::pair{8u, 8u}};
  struct OverlapCase { std::array<uint32_t, 2> counts; std::array<uint32_t, 3> expected; };
  const std::array<OverlapCase, 7> overlaps{{
      {{300u, 256u}, {256u, 300u, 300u}}, {{300u, 257u}, {300u, 300u, 300u}},
      {{300u, 256u}, {256u, 300u, 300u}}, {{256u, 3u}, {3u, 3u, 256u}},
      {{257u, 3u}, {3u, 257u, 257u}}, {{300u, 0u}, {0u, 0u, 0u}},
      {{0u, 300u}, {300u, 300u, 300u}}}};
  for (uint32_t layout = 0; layout < bases.size(); ++layout) {
    auto overlap_plan = make_plan(Variant::Valid, bases[layout].first, bases[layout].second);
    for (const auto& test : overlaps) {
      boundary.counts = test.counts;
      boundary.keys_read = 0u;
      Check(MaterializeResources(overlap_plan, runtime, snapshot, specialization) &&
                boundary.keys_read == test.expected[layout] && snapshot.images.size() == 1u &&
                snapshot.images[0].dwords[0] == (test.expected[layout] != 0u ? 0x101u : 0u),
            "shared append bounds lost overlap, included a touching list or retained stale keys");
    }
  }
}

void TestComputeBufferFill() {
  struct Options {
    bool scalar = false;
    bool conditional = false;
    bool shifted = false;
    bool extra_store = false;
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
    const auto materialize = [&](SrtMemoryReader reader) {
      FakeGuestMemory fake(reader, &memory);
      return MaterializeResources(plan, {.user_data = userdata}, snapshot, specialization);
    };
    Check(materialize(Read), "fill fixture did not materialize");
    const bool expected = !options.conditional && !options.shifted &&
                          !options.extra_store && !options.branch;
    Check((snapshot.uniform_fill.words != 0) == expected,
          "fill proof accepted an unsafe store or missed the real GTA3 clear");
    if (expected) {
      Check(snapshot.uniform_fill.words == 1 &&
                snapshot.uniform_fill.group_stride[0] == 64 &&
                snapshot.uniform_fill.value ==
                    (options.scalar ? 0x40404040u : 0u),
            "fill proof lost address coverage or the actual stored scalar");
      if (options.scalar) {
        Check(materialize(nullptr) && snapshot.uniform_fill.words == 1 &&
                  snapshot.uniform_fill.value == 0u,
              "an unreadable fill value retained a previous uniform fill");
      }
    }
  };
  Run({});
  Run({.scalar = true});
  Run({.conditional = true});
  Run({.shifted = true});
  Run({.extra_store = true});
  Run({.branch = true});
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
  Check(EvaluateSrtDescriptor(plan, runtime, source, value) &&
            value.dwords[3] == 0x100u,
        "runtime descriptor unsigned minimum did not clamp its first operand");
  user_data[0] = 0x80u;
  Check(
      EvaluateSrtDescriptor(plan, runtime, source, value) &&
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

void TestIndependentImageSamplerPairs() {
  Fixture fixture(ShaderType::Pixel);
  const auto address = fixture.ImageAddress();
  MemoryInfo memory;
  memory.kind = ResourceKind::Image;
  memory.image_dimension = Decoder::ImageDimension::Dim2D;
  for (uint32_t use = 0; use < 144; ++use) {
    const auto pair = use % 72;
    const auto pc = use * 4;
    const auto image = fixture.Image(
        {Value(pair / 8), Value(0u), Value(0u), Value(0u), Value(0u),
         Value(0u), Value(0u), Value(0u)}, pc);
    const auto sampler = fixture.Sampler(
        {Value(pair % 8), Value(0u), Value(0u), Value(0u)}, pc);
    fixture.Emit(ValueOpcode::ImageSampleRaw,
                 {image, sampler, address, Value(true)},
                 fixture.AddMemory(memory, pc));
  }
  fixture.PlanAndTrack();

  Check(fixture.program.info.images.size() == 9 &&
            fixture.program.info.samplers.size() == 8 &&
            fixture.program.info.sampled_pairs.size() == 72,
        "distinct image/sampler combinations were limited or duplicated");
  for (uint32_t index = 0; index < 72; ++index) {
    const auto &pair = fixture.program.info.sampled_pairs[index];
    Check(pair.image == index / 8 && pair.sampler == index % 8 &&
              pair.first_use_pc == index * 4,
          "sampled pair lost its independent resources or first use");
  }
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
  Check(EvaluateSrtDescriptor(plan, runtime, source, descriptor) &&
            descriptor.dwords[3] == 0x80000abcu,
        "SampleAdjust canonicalization lost sampler border fields");
  const auto shift_source = fixture.program.info.samplers[
      shift_sampler.Instruction()->Flags<uint32_t>()].source;
  Check(EvaluateSrtDescriptor(plan, runtime, shift_source, descriptor) &&
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
  RemoveIdentities(fixture.program);
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
  Check(EvaluateSrtValue(fixture.program, {.user_data = user_data}, result, value) &&
            value == 0x76543210u,
        "FMASK load did not return the native sample-to-fragment mapping");
  user_data[8] = 1;
  Check(EvaluateSrtValue(fixture.program, {.user_data = user_data}, result, value) &&
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
      constexpr std::array sampler_controls{
          0u, 0x4001u, 1u << 26u, (1u << 26u) | 256u,
          (1u << 26u) | (1u << 14u), 2u << 26u, (2u << 26u) | 256u,
          (2u << 26u) | (1u << 14u), 0x0a500000u, 0x1a500000u, 3u << 26u};
      for (const auto control : sampler_controls) {
        for (const uint32_t perf_mip : {0u, 1u}) {
          user_data[9] = 0xfff000u | (perf_mip << 24u);
          user_data[10] = control;
          ResourceSnapshot snapshot;
          ResourceSpecialization specialization;
          const auto filter = (control >> 26u) & 3u;
          const bool supported = !explicit_lod || filter == 0 ||
              (filter <= 2 && (control & 0xfffffu) == 0 &&
               (filter != 2 || perf_mip == 0));
          Check(MaterializeResources(plan, {.user_data = user_data}, snapshot,
                                     specialization) == supported,
                "explicit gather accepted an unsupported sampler or rejected a valid one");
        }
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
  Check(MaterializeResources(resource_plan, {.user_data = changed_user_data},
                             changed_snapshot, changed_specialization) &&
            changed_snapshot.images.size() == 2u &&
            std::ranges::all_of(changed_snapshot.images, [](const auto& image) {
              return std::ranges::all_of(image.dwords, [](uint32_t word) { return word == 0u; });
            }) && changed_specialization.images[0].mip_count == 1u &&
            changed_specialization.images[1].mip_count == 1u &&
            changed_specialization != specialization,
        "an inverted mip range was not normalized consistently for plain and dynamic views");
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
  SrtRuntime runtime{.user_data = user_data};
  FakeGuestMemory fake(ReadTestMemory, &memory);
  auto plan = ExtractResourcePlan(fixture.program);
  DescriptorValue descriptor;
  std::vector<uint32_t> flat;
  const uint32_t request = fixture.program.info.buffers[0].source;
  const auto refresh = [&](const ResourcePlan& plan) {
    return WalkSrt(plan, runtime, flat, nullptr) && WalkedDescriptor(plan, request, descriptor);
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
  Check(refresh(plan) && descriptor.dwords[0] == 0u && flat == std::vector<uint32_t>{0u},
        "unavailable scalar memory did not read as zero");
  memory.fail_after = UINT32_MAX;
  Check(refresh(plan) && descriptor.dwords[0] == 0x12345678u && memory.reads == 1,
        "a zero read left a stale value in the next walk");

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

void TestScalarDataReadControlFlow() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  for (const auto comparison : {ValueOpcode::Void, ValueOpcode::IEqual64,
                                ValueOpcode::INotEqual64}) {
    Fixture fixture;
    const auto low = fixture.UserData(0);
    const auto high = fixture.UserData(1);
    const auto address = fixture.Address(low, high);
    auto* use = fixture.block;
    if (comparison != ValueOpcode::Void) {
      const auto pointer = fixture.Emit(ValueOpcode::CompositeConstructU64, {low, high});
      auto enabled = fixture.Emit(comparison, {pointer, Value(uint64_t{0})});
      if (comparison == ValueOpcode::IEqual64)
        enabled = fixture.Emit(ValueOpcode::LogicalNot, {enabled});
      const auto lane = fixture.Emit(ValueOpcode::GetBuiltin,
          {Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId)), Value(0u)});
      const auto varying = fixture.Emit(ValueOpcode::INotEqual32, {lane, Value(0u)});
      fixture.block->condition = fixture.Emit(ValueOpcode::LogicalAnd, {varying, enabled});
      use = fixture.AddBlock();
      auto* done = fixture.AddBlock();
      fixture.block->AddBranch(use);
      fixture.block->AddBranch(done);
      use->AddBranch(done);
      fixture.block->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
          .true_block = use, .false_block = done};
      use->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = done};
      done->terminator.kind = CFG::TerminatorKind::Return;
    }
    MemoryInfo scalar;
    scalar.kind = ResourceKind::ScalarAddress;
    fixture.Emit(ValueOpcode::LoadAddressU32,
        {address, Value(0u), Value(0u), Value(true)}, fixture.AddMemory(scalar, 4), use);
    fixture.PlanAndTrack();
    auto plan = ExtractResourcePlan(fixture.program);
    Check(plan.srt_reads.size() == 1 && !fixture.program.info.uses_dma && plan.srt_code,
          "scalar data lost flattening or its walker");
    uint32_t reads = 0;
    std::array<uint32_t, 2> user_data {};
    SrtRuntime runtime {.user_data = user_data};
    FakeGuestMemory fake(+[](void* data, uint64_t address, std::span<uint32_t> words) {
          ++*static_cast<uint32_t*>(data);
          if (address == 0) return false;
          words[0] = 42;
          return true;
        }, &reads);
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    for (const uint64_t pointer : {uint64_t{0}, uint64_t{0x1000}, uint64_t{1} << 32u}) {
      if (pointer == 0 && comparison == ValueOpcode::Void) continue;
      user_data = {static_cast<uint32_t>(pointer), static_cast<uint32_t>(pointer >> 32u)};
      reads = 0;
      Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                reads == (pointer != 0) && snapshot.flattened_srt[0] == (pointer ? 42u : 0u),
            "scalar data refresh ignored a mixed EXEC/64-bit null-pointer branch");
    }
  }
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
  SrtRuntime runtime{.user_data = user_data};
  FakeGuestMemory fake(ReadTestMemory, &memory);
  DescriptorValue value;
  Check(EvaluateSrtDescriptor(plan, runtime, fixture.program.info.buffers[0].source, value) &&
            value.dwords[0] == 0xabcdef01u && memory.reads == 1,
        "dynamic typed scalar descriptor source was not evaluated");

  ShaderComputeInputInfo compute{};
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(!fixture.program.info.uses_flattened_srt && fixture.program.info.uses_dma &&
            fixture.program.info.buffers.size() == 1u && fixture.program.info.buffers[0].live,
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

void TestBoundedFormattedBufferStore() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  namespace Prospero = Libs::Graphics::Prospero;
  const auto make = [](uint32_t step = 1u, bool bypass = false, bool byte_offset = false,
                       bool signed_count = false) {
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    auto* entry = f.block;
    auto* header = f.AddBlock();
    auto* body = f.AddBlock();
    auto* latch = f.AddBlock();
    auto* exit = f.AddBlock();
    const auto branch = [](Block* from, Block* to) {
      from->AddBranch(to);
      from->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = to};
    };
    branch(entry, header);
    branch(body, latch);
    branch(latch, header);
    header->AddBranch(body);
    header->AddBranch(bypass ? latch : exit);
    header->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                         .true_block = body, .false_block = bypass ? latch : exit};
    exit->terminator.kind = CFG::TerminatorKind::Return;
    const auto table = f.Address(f.UserData(0), f.UserData(1));
    const auto count = f.Emit(ValueOpcode::LoadAddressU32,
        {table, Value(0u), Value(0u), Value(true)},
        f.AddMemory({.kind = ResourceKind::ScalarAddress, .offset = 288u}, 16u));
    const auto lane = f.Emit(ValueOpcode::LaneId);
    auto& phi = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    phi.AddPhiOperand(entry, Value(0u));
    phi.AddPhiOperand(latch, f.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(step)}, 0, latch));
    header->condition = f.Emit(signed_count ? ValueOpcode::SLessThan32 : ValueOpcode::ULessThan32,
                               {Value(&phi), count}, 0, header);
    f.block = body;
    const auto offset = f.Emit(ValueOpcode::ShiftLeftLogical32, {Value(&phi), Value(4u)});
    std::array<Value, 4> words;
    for (uint32_t component = 0; component < 4u; ++component)
      words[component] = f.Emit(ValueOpcode::LoadAddressU32,
          {table, offset, Value(0u), Value(true)},
          f.AddMemory({.kind = ResourceKind::ScalarAddress, .offset = 128u + component * 4u,
                       .component_index = component, .component_count = 4u}, 88u));
    f.Emit(ValueOpcode::StoreBufferU32,
        {f.Buffer(words), lane, Value(0u), Value(0u), Value(0x80000001u), Value(true)},
        f.AddMemory({.kind = ResourceKind::Buffer, .offset = byte_offset ? 4u : 0u,
                     .formatted = true, .idxen = true}, 100u));
    f.PlanAndTrack();
    return fixture;
  };
  auto fixture = make();
  auto plan = ExtractResourcePlan(fixture->program);
  const auto& source = plan.descriptor_sources[plan.info.buffers[0].source];
  Check(source.indirect_descriptor && source.indirect_descriptor->indexed &&
            !source.indirect_descriptor->signed_count &&
            !source.indirect_descriptor->selector &&
            !source.indirect_descriptor->key_count.IsEmpty() &&
            source.indirect_descriptor->table_stride == 16u &&
            source.indirect_descriptor->table_immediate == 128u,
        "bounded formatted store lost its uniform loop and descriptor layout");
  LinearTestMemory memory;
  constexpr std::array<uint32_t, 8> lengths{16, 1, 4, 32, 48, 4, 32, 48};
  for (uint32_t key = 0; key < 8u; ++key) {
    const auto word = 32u + key * 4u;
    memory.words[word] = 0x100000u + key * 0x1000u;
    memory.words[word + 1u] = 4u << 16u;
    memory.words[word + 2u] = lengths[key];
    memory.words[word + 3u] = static_cast<uint32_t>(Prospero::BufferFormat::k32UInt) << 12u | 4u;
  }
  std::array<uint32_t, 2> user_data{static_cast<uint32_t>(memory.base), 0u};
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadLinearTestMemory, &memory);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto materialize = [&] { return MaterializeResources(plan, runtime, snapshot, specialization); };
  for (const uint32_t count : {0u, 1u, 8u, 3u, 0u, 8u}) {
    memory.words[72] = count;
    memory.reads = memory.watched_reads = 0;
    memory.watched_address = memory.base + 128u;
    Check(materialize() && snapshot.buffers.size() == std::max(1u, count),
          "uniform buffer table count did not refresh or left stale descriptors");
    Check(count != 0u || memory.watched_reads == 0u,
          "an empty loop probed its unreachable descriptor table");
    Check(std::ranges::find(snapshot.specialization_reads,
                std::pair{memory.base + 288u, uint64_t{4u}}) != snapshot.specialization_reads.end(),
          "writable descriptor loop count escaped same-dispatch overlap capture");
    for (uint32_t key = 0; key < count; ++key) {
      Check(snapshot.buffers[key].dwords[0] == memory.words[32u + key * 4u] &&
                snapshot.buffers[key].dwords[2] == lengths[key],
            "finite formatted store lost a destination or its record bound");
      Check(std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + 128u + key * 16u, uint64_t{16u}}) !=
                snapshot.specialization_reads.end(),
            "writable descriptor table escaped same-dispatch overlap capture");
    }
  }
  const auto original = specialization;
  memory.words[32] += 0x100u;
  Check(materialize() && specialization == original &&
            snapshot.buffers[0].dwords[0] == memory.words[32],
        "destination address refresh was frozen or caused needless specialization");
  for (const auto [word, replacement] : std::array<std::pair<uint32_t, uint32_t>, 9>{
           {{35u, static_cast<uint32_t>(Prospero::BufferFormat::k32Float) << 12u | 4u},
            {35u, static_cast<uint32_t>(Prospero::BufferFormat::k16UInt) << 12u | 4u},
            {35u, memory.words[35] & ~7u}, {35u, memory.words[35] | (4u << 3u)},
            {32u, memory.words[32] + 1u},
            {33u, 8u << 16u}, {33u, (4u << 16u) | (1u << 31u)},
            {35u, memory.words[35] | (1u << 23u)}, {35u, memory.words[35] | (3u << 28u)}}}) {
    const auto saved = memory.words[word];
    memory.words[word] = replacement;
    Check(!materialize(), "formatted finite store accepted an unproved format or bounds mode");
    memory.words[word] = saved;
  }
  for (const uint32_t count : {MaxIndirectDescriptorProbes + 1u, 0x80000000u, UINT32_MAX}) {
    memory.words[72] = count;
    memory.watched_reads = 0;
    Check(!materialize() && memory.watched_reads == 0u,
          "oversized unsigned descriptor count became empty or probed an unbounded table");
  }
  auto signed_fixture = make(1u, false, false, true);
  const auto signed_plan = ExtractResourcePlan(signed_fixture->program);
  Check(signed_plan.descriptor_sources[signed_plan.info.buffers[0].source]
            .indirect_descriptor->signed_count,
        "signed loop comparison lost its count semantics");
  for (const uint32_t count : {0x80000000u, UINT32_MAX, 3u}) {
    memory.words[72] = count;
    memory.watched_reads = 0;
    const bool negative = count != 3u;
    Check(MaterializeResources(signed_plan, runtime, snapshot, specialization) &&
              snapshot.buffers.size() == (negative ? 1u : count) &&
              (!negative || memory.watched_reads == 0u),
          "signed loop bound did not preserve empty negative and populated positive domains");
  }
  memory.words[72] = 8u;
  memory.fail_address = memory.base + 128u;
  Check(!materialize(), "formatted destination bypassed failed coherent descriptor reads");
  memory.fail_address = UINT64_MAX;
  Check(materialize(), "valid uniform descriptor table did not recover after a failed refresh");
  ApplyResourceSpecialization(fixture->program, specialization);
  bool formatted = false;
  for (auto* block : fixture->program.blocks)
    for (auto& inst : *block)
      if (inst.GetOpcode() == ValueOpcode::StoreBufferU32) {
        const auto& info = fixture->program.memory_info[inst.Flags<MemoryFlags>().index];
        formatted = info.formatted && info.idxen;
      }
  Check(formatted, "UInt32 candidate store lost the shared formatted store path");
  for (const auto [step, bypass, offset] :
       {std::tuple{2u, false, false}, std::tuple{1u, true, false}, std::tuple{1u, false, true}})
    CheckFatal([&] { make(step, bypass, offset); }, "not a valid runtime value",
               "finite store accepted an unproved induction or indexed byte offset");
}

std::unique_ptr<Fixture> MakeBufferRecordFixture(uint32_t step = 1u, bool bypass = false,
                                                  bool gpu_selected = false, uint32_t bias = 0u,
                                                  bool split_offsets = false,
                                                  bool workgroup_selected = false,
                                                  uint32_t d32_components = 0u) {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  auto fixture = std::make_unique<Fixture>();
  auto& f = *fixture;
  std::array<Value, 4> table_words;
  for (uint32_t word = 0; word < 4u; ++word) table_words[word] = f.UserData(word);
  const auto table = f.Buffer(table_words);
  const auto lane = f.Emit(ValueOpcode::LaneId);
  Value key;
  if (gpu_selected) {
    std::array<Value, 4> selector_words;
    for (uint32_t word = 0; word < 4u; ++word) selector_words[word] = f.UserData(4u + word);
    const auto group = workgroup_selected ? f.Emit(ValueOpcode::GetBuiltin,
        {Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)), Value(0u)}) : lane;
    key = f.Emit(ValueOpcode::ReadConstBuffer,
        {f.Buffer(selector_words), f.Emit(ValueOpcode::IMul32, {group, Value(12u)})},
        f.AddMemory({.kind = ResourceKind::ScalarBuffer}, 0x1bcu));
  } else {
    auto* entry = f.block;
    auto* header = f.AddBlock();
    auto* body = f.AddBlock();
    auto* latch = f.AddBlock();
    auto* exit = f.AddBlock();
    const auto branch = [](Block* from, Block* to) {
      from->AddBranch(to);
      from->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = to};
    };
    branch(entry, header);
    branch(body, latch);
    branch(latch, header);
    header->AddBranch(body);
    header->AddBranch(bypass ? latch : exit);
    header->terminator = {.kind = CFG::TerminatorKind::ConditionalBranch,
                         .true_block = body, .false_block = bypass ? latch : exit};
    exit->terminator.kind = CFG::TerminatorKind::Return;
    const auto count = f.Emit(ValueOpcode::LoadAddressU32,
        {f.Address(f.UserData(4u), f.UserData(5u)), Value(0u), Value(0u), Value(true)},
        f.AddMemory({.kind = ResourceKind::ScalarAddress}, 0x424u));
    auto& phi = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    phi.AddPhiOperand(entry, Value(0u));
    phi.AddPhiOperand(latch, f.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(step)}, 0, latch));
    header->condition = f.Emit(ValueOpcode::SLessThan32, {Value(&phi), count}, 0, header);
    f.block = body;
    key = Value(&phi);
  }
  auto offset = f.Emit(ValueOpcode::IMul32, {key, Value(192u)});
  if (bias != 0u) offset = f.Emit(ValueOpcode::IAdd32, {offset, Value(bias)});
  for (const bool formatted : {true, false}) {
    std::array<Value, 4> words;
    for (uint32_t word = 0; word < 4u; ++word) {
      const auto dynamic = split_offsets
          ? f.Emit(ValueOpcode::IAdd32, {offset, Value(word * 4u)}) : offset;
      words[word] = f.Emit(ValueOpcode::ReadConstBuffer, {table, dynamic},
          f.AddMemory({.kind = ResourceKind::ScalarBuffer,
                       .offset = (formatted ? 160u : 176u) + (split_offsets ? 0u : word * 4u),
                       .component_index = word, .component_count = 4u}, 0x1808u));
    }
    for (uint32_t use = 0; use < (gpu_selected ? 2u : formatted ? 112u : 16u); ++use) {
      const auto handle = f.Buffer(words);
      if (formatted) {
        const auto opcode = d32_components == 3u ? ValueOpcode::LoadBufferU32x3
                          : d32_components == 4u ? ValueOpcode::LoadBufferU32x4
                                                 : ValueOpcode::LoadBufferU32x2;
        const auto extract = d32_components == 3u ? ValueOpcode::CompositeExtractU32x3
                           : d32_components == 4u ? ValueOpcode::CompositeExtractU32x4
                                                  : ValueOpcode::CompositeExtractU32x2;
        const auto loaded = f.Emit(opcode,
            {handle, lane, Value(0u), Value(0u), Value(true)},
            f.AddMemory({.kind = ResourceKind::Buffer,
                         .data_dwords = d32_components == 0u ? 2u : d32_components,
                         .data_bits = d32_components == 0u ? 16u : 32u,
                         .component_count = d32_components == 0u ? 4u : d32_components,
                         .formatted = true, .idxen = true}, 0x1914u));
        f.Emit(ValueOpcode::ReferenceU32,
            {f.Emit(extract, {loaded, Value(0u)})});
      } else {
        f.Emit(ValueOpcode::StoreBufferU8,
            {handle, lane, Value(0u), Value(0u), Value(uint8_t{42u}), Value(true)},
            f.AddMemory({.kind = ResourceKind::Buffer, .data_bits = 8u, .idxen = true}, 0x190cu));
      }
    }
  }
  f.PlanAndTrack();
  return fixture;
}

void TestBoundedBufferRecords(uint32_t d32_components) {
  namespace Prospero = Libs::Graphics::Prospero;
  using Libs::Graphics::DstSel;
  auto fixture = MakeBufferRecordFixture(1u, false, false, 0u, false, false, d32_components);
  const auto load_opcode = d32_components == 3u ? ValueOpcode::LoadBufferU32x3
                         : d32_components == 4u ? ValueOpcode::LoadBufferU32x4
                                                : ValueOpcode::LoadBufferU32x2;
  const auto plan = ExtractResourcePlan(fixture->program);
  Check(plan.info.buffers.size() == 2u, "record descriptors did not share their resource roots");
  uint32_t selections = 0u;
  std::array<const Inst*, 2> selected{};
  for (const auto* block : fixture->program.blocks) {
    for (const auto& inst : *block) {
      if (inst.GetOpcode() == ValueOpcode::GetBufferResourceIndex) {
        ++selections;
        Check(block->id == 2u && inst.Flags<uint32_t>() < 2u,
              "record selection moved outside its loop or lost its buffer root");
        selected[inst.Flags<uint32_t>()] = &inst;
      }
      if (inst.GetOpcode() == load_opcode || inst.GetOpcode() == ValueOpcode::StoreBufferU8) {
        const auto root = fixture->program.memory_info[inst.Flags<MemoryFlags>().index].resource;
        Check(inst.Arg(0).Instruction()->Arg(0).Instruction() == selected[root],
              "record operation did not use its dominating shared selection");
      }
    }
  }
  Check(selections == 2u, "repeated record operations duplicated candidate mapping searches");
  for (uint32_t root = 0; root < 2u; ++root) {
    const auto& source = plan.descriptor_sources[plan.info.buffers[root].source];
    Check(source.indirect_descriptor && source.indirect_descriptor->indexed &&
              source.indirect_descriptor->signed_count &&
              source.indirect_descriptor->table_stride == 192u &&
              source.indirect_descriptor->table_immediate == (root == 0u ? 160u : 176u) &&
              !source.indirect_descriptor->key_count.IsEmpty(),
          "record descriptor lost its bounded loop or member offset");
  }
  LinearTestMemory memory;
  constexpr uint32_t table_offset = 256u;
  const auto format = static_cast<uint32_t>(Prospero::BufferFormat::k16_16_16_16Float);
  for (uint32_t row = 0; row < 4u; ++row) {
    const auto first = (table_offset + row * 192u) / 4u;
    std::fill_n(memory.words.begin() + first, 48u, 0xdeadbeefu);
    for (const bool formatted : {true, false}) {
      const auto word = first + (formatted ? 40u : 44u);
      memory.words[word] = 0x100000u + row * 0x1000u + (formatted ? 0u : 0x8000u);
      memory.words[word + 1u] = (formatted ? 8u : 3u) << 16u;
      memory.words[word + 2u] = 16u;
      memory.words[word + 3u] = formatted ? (format << 12u) | DstSel(4, 5, 6, 1) : 0x5204u;
    }
  }
  std::array<uint32_t, 6> user_data{uint32_t(memory.base) + table_offset, 8u << 16u,
                                   4u * 192u / 8u, 0x5204u, uint32_t(memory.base), 0u};
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadLinearTestMemory, &memory);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto materialize = [&] { return MaterializeResources(plan, runtime, snapshot, specialization); };
  for (const uint32_t count : {0u, 1u, 4u, 2u, UINT32_MAX, 0u, 4u}) {
    memory.words[0] = count;
    const uint32_t rows = count == UINT32_MAX ? 0u : count;
    user_data[2] = rows == 0u ? 0u : 4u * 192u / 8u;
    memory.reads = 0u;
    Check(materialize() && snapshot.buffers.size() == 2u * std::max(rows, 1u) &&
              memory.reads == 1u + rows * 2u,
          "bounded record enumeration read unrelated fields or retained stale candidates");
    for (uint32_t row = 0; row < rows; ++row)
      for (const auto member : {160u, 176u})
        Check(std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + table_offset + row * 192u + member, uint64_t{16u}}) !=
                  snapshot.specialization_reads.end(),
              "record descriptor escaped same-dispatch overlap capture");
  }
  const auto original = specialization;
  memory.words[(table_offset + 160u) / 4u] += 0x100u;
  Check(materialize() && specialization == original,
        "record address refresh changed the shader specialization");
  user_data[2] = 4u;
  memory.reads = 0u;
  Check(materialize() && snapshot.buffers.size() == 2u && memory.reads == 1u,
        "record enumeration read beyond the scalar buffer table bounds");
  user_data[2] = 4u * 192u / 8u;
  const auto first = (table_offset + 160u) / 4u;
  const auto first_address = memory.words[first];
  memory.words[first] = 0u;
  memory.words[first + 2u] = 0u;
  Check(materialize() && specialization.buffers[0].zero_stride_oob &&
            specialization.buffers[0].descriptor_swizzle == DstSel(4, 5, 6, 1) &&
            specialization.buffers[0].indirect_root == 0u,
        "empty formatted candidate lost its OOB constants or other candidates");
  ApplyResourceSpecialization(fixture->program, specialization);
  uint32_t loads = 0u;
  for (const auto* block : fixture->program.blocks)
    for (const auto& inst : *block) loads += inst.GetOpcode() == load_opcode;
  Check(loads == 112u, "empty root incorrectly folded nonempty formatted candidates");
  memory.words[first] = first_address;
  memory.words[first + 2u] = 16u;
  for (const auto [word, replacement] : std::array<std::pair<uint32_t, uint32_t>, 5>{
           {{first + 1u, 4u << 16u}, {first + 1u, 9u << 16u},
            {first, memory.words[first] + 1u}, {first + 3u, memory.words[first + 3u] | (1u << 28u)},
            {first + 1u, (8u << 16u) | (1u << 31u)}}}) {
    const auto saved = memory.words[word];
    memory.words[word] = replacement;
    Check(!materialize(), "formatted record accepted unproved alignment or addressing");
    memory.words[word] = saved;
  }
  memory.words[0] = MaxIndirectDescriptorProbes + 1u;
  Check(!materialize(), "record table accepted an unbounded candidate count");
  for (const auto [step, bypass] : {std::pair{2u, false}, std::pair{1u, true}}) {
    const auto unbounded = MakeBufferRecordFixture(step, bypass, false, 0u, false, false, d32_components);
    for (const auto& buffer : unbounded->program.info.buffers) {
      const auto& source = unbounded->program.descriptor_sources[buffer.source];
      if (source.indirect_descriptor)
        Check(source.indirect_descriptor->table_scalar && source.indirect_descriptor->key_count.IsEmpty(),
              "unproved loop did not use the bounded descriptor table");
    }
  }
}

void TestGuardedWaterfallBufferRecords() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  namespace Prospero = Libs::Graphics::Prospero;
  using Libs::Graphics::DstSel;
  enum class Guard {
    Exact, Reversed, Greater, GreaterEqual, WrongGreater, SignedGreater,
    NoRange, NoEquality, OtherLocal, UnguardedUse, SeparateVotes,
    Loop, LoopReversed, LoopEntries, LoopBadEntry, LoopWiden, LoopUnanchored, LoopVarying
  };
  const auto make = [](Guard guard) {
    auto f = std::make_unique<Fixture>();
    const auto count = f->UserData(2u);
    const auto table = f->Buffer({f->UserData(0u), f->UserData(1u), count, f->UserData(3u)});
    const auto lane = f->Emit(ValueOpcode::LaneId);
    const auto input = f->Buffer({f->UserData(4u), f->UserData(5u), f->UserData(6u), f->UserData(7u)});
    const auto local = f->Emit(ValueOpcode::LoadBufferU32,
        {input, lane, Value(0u), Value(0u), Value(true)},
        f->AddMemory({.kind = ResourceKind::Buffer, .idxen = true}, 0x200u));
    const auto initial = f->Emit(ValueOpcode::ULessThan32, {lane, Value(32u)});
    const auto range_local = guard == Guard::OtherLocal
        ? f->Emit(ValueOpcode::IAdd32, {local, Value(1u)}) : local;
    Value range;
    if (guard == Guard::Greater || guard == Guard::GreaterEqual ||
        guard == Guard::WrongGreater || guard == Guard::SignedGreater) {
      const auto op = guard == Guard::GreaterEqual ? ValueOpcode::UGreaterThanEqual32
          : guard == Guard::SignedGreater ? ValueOpcode::SGreaterThan32 : ValueOpcode::UGreaterThan32;
      const auto compare = f->Emit(op, guard == Guard::WrongGreater
          ? std::initializer_list<Value>{range_local, count}
          : std::initializer_list<Value>{count, range_local});
      range = f->Emit(ValueOpcode::LogicalAnd, {initial, compare});
    } else {
      // Match ANDN1_SAVEEXEC, including its initial-EXEC witness.
      auto executing = initial;
      auto comparison_mask = initial;
      if (guard >= Guard::Loop) {
        auto* entry = f->block;
        auto* header = f->AddBlock();
        f->block = header;
        auto& mask = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U1));
        if (guard != Guard::LoopUnanchored) entry->AddBranch(header);
        header->AddBranch(header);
        if (guard == Guard::LoopVarying) {
          auto& predicate = header->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U1));
          predicate.AddPhiOperand(entry, initial);
          predicate.AddPhiOperand(header, f->Emit(ValueOpcode::LogicalNot, {Value(&predicate)}));
          comparison_mask = Value(&predicate);
        }
        const auto next = guard == Guard::LoopWiden
            ? f->Emit(ValueOpcode::LogicalNot, {initial})
            : guard == Guard::LoopVarying ? comparison_mask
            : f->Emit(ValueOpcode::ULessThan32, {lane, Value(16u)});
        const auto carried = f->Emit(guard == Guard::LoopWiden
            ? ValueOpcode::LogicalOr : ValueOpcode::LogicalAnd, {Value(&mask), next});
        if (guard == Guard::LoopReversed) mask.AddPhiOperand(header, carried);
        if (guard != Guard::LoopUnanchored) mask.AddPhiOperand(entry, initial);
        if (guard != Guard::LoopReversed) mask.AddPhiOperand(header, carried);
        if (guard == Guard::LoopEntries || guard == Guard::LoopBadEntry ||
            guard == Guard::LoopUnanchored) {
          auto* alternate = f->AddBlock();
          (guard == Guard::LoopUnanchored ? header : entry)->AddBranch(alternate);
          alternate->AddBranch(header);
          const auto incoming = guard == Guard::LoopUnanchored
              ? f->Emit(ValueOpcode::LogicalAnd, {Value(&mask), initial}, 0, alternate)
              : guard == Guard::LoopBadEntry ? Value(true) : initial;
          mask.AddPhiOperand(alternate, incoming);
        }
        executing = Value(&mask);
      }
      const auto excluded = f->Emit(ValueOpcode::LogicalAnd,
          {comparison_mask, f->Emit(ValueOpcode::ULessThanEqual32, {count, range_local})});
      range = f->Emit(ValueOpcode::LogicalAnd,
          {executing, f->Emit(ValueOpcode::LogicalNot, {excluded})});
    }
    const auto key = f->Emit(ValueOpcode::ReadLane, {local, Value(0u)});
    const auto equal = f->Emit(ValueOpcode::IEqual32,
        guard == Guard::Reversed ? std::initializer_list<Value>{local, key}
                                 : std::initializer_list<Value>{key, local});
    auto active = f->Emit(ValueOpcode::LogicalAnd, {range, equal});
    if (guard == Guard::NoRange) active = equal;
    if (guard == Guard::NoEquality) active = range;
    if (guard == Guard::SeparateVotes)
      active = f->Emit(ValueOpcode::LogicalAnd,
          {f->Emit(ValueOpcode::ConditionRef, {range}, CFG::BranchCondition::ExecNonZero),
           f->Emit(ValueOpcode::ConditionRef, {equal}, CFG::BranchCondition::ExecNonZero)});
    const auto offset = f->Emit(ValueOpcode::IMul32, {key, Value(488u)});
    std::array<Value, 4> words;
    for (uint32_t word = 0; word < 4u; ++word)
      words[word] = f->Emit(ValueOpcode::ReadConstBuffer, {table, offset},
          f->AddMemory({.kind = ResourceKind::ScalarBuffer, .offset = 216u + word * 4u,
                        .component_index = word, .component_count = 4u}, 0x304u));
    const auto handle = f->Buffer(words);
    for (uint32_t use = 0; use < 2u; ++use) {
      const auto loaded = f->Emit(ValueOpcode::LoadBufferU32x3,
          {handle, lane, Value(0u), Value(0u),
           guard == Guard::UnguardedUse && use == 1u ? Value(true) : active},
          f->AddMemory({.kind = ResourceKind::Buffer, .data_dwords = 3u, .data_bits = 32u,
                        .component_count = 3u, .formatted = true, .idxen = true}, 0x360u));
      f->Emit(ValueOpcode::ReferenceU32,
          {f->Emit(ValueOpcode::CompositeExtractU32x3, {loaded, Value(use)})});
    }
    f->PlanAndTrack();
    return f;
  };
  LinearTestMemory memory;
  constexpr uint32_t table_offset = 256u;
  for (uint32_t row = 0; row < 2u; ++row) {
    const std::array descriptor{0x10000u + row * 0x1000u, 12u << 16u, 32u,
        (uint32_t(Prospero::BufferFormat::k32_32_32Float) << 12u) | DstSel(4, 5, 6, 1)};
    std::copy(descriptor.begin(), descriptor.end(),
              memory.words.begin() + (table_offset + row * 488u + 216u) / 4u);
  }
  // An unbounded key 0x029f79b5 wraps to this real congruence slot, outside key < N.
  constexpr std::array poison{0x012b6890u, 0x501502f9u, 0x493ba30eu, 0x3ea2f352u};
  std::copy(poison.begin(), poison.end(), memory.words.begin() + (table_offset + 480u) / 4u);
  std::array<uint32_t, 8> user_data{uint32_t(memory.base) + table_offset, 488u << 16u,
      2u, 0x5204u, 0x9000u, 4u << 16u, 32u, 0x5204u};
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  for (const auto guard : {Guard::Exact, Guard::Reversed, Guard::Greater, Guard::GreaterEqual,
                          Guard::WrongGreater, Guard::SignedGreater, Guard::NoRange, Guard::NoEquality,
                          Guard::OtherLocal, Guard::UnguardedUse, Guard::SeparateVotes,
                          Guard::Loop, Guard::LoopReversed, Guard::LoopEntries, Guard::LoopBadEntry,
                          Guard::LoopWiden, Guard::LoopUnanchored, Guard::LoopVarying}) {
    auto fixture = make(guard);
    const auto plan = ExtractResourcePlan(fixture->program);
    const auto root = std::ranges::find_if(plan.info.buffers, [&](const auto& buffer) {
      return plan.descriptor_sources[buffer.source].indirect_descriptor.has_value();
    });
    Check(root != plan.info.buffers.end(), "waterfall buffer lost its indirect resource");
    const bool proven = guard == Guard::Exact || guard == Guard::Reversed || guard == Guard::Greater ||
        guard == Guard::Loop || guard == Guard::LoopReversed || guard == Guard::LoopEntries;
    const auto& domain = *plan.descriptor_sources[root->source].indirect_descriptor;
    Check(domain.key_count.IsEmpty() != proven && !domain.selector &&
              domain.table_stride == 488u && domain.table_immediate == 216u,
          "waterfall bound mixed different lanes, values or resource uses");
    const auto materialize = [&] { return MaterializeResources(plan, runtime, snapshot, specialization); };
    if (!proven) {
      Check(!materialize(), "unproved waterfall bypassed conservative descriptor rejection");
      continue;
    }
    for (const uint32_t count : {2u, 0u, 1u, 2u}) {
      user_data[2] = count;
      memory.reads = 0;
      Check(materialize() && memory.reads == count &&
                snapshot.buffers.size() == 1u + std::max(count, 1u),
            "waterfall count refresh read GPU keys or unrelated record fields");
      for (uint32_t row = 0; row < count; ++row)
        Check(std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + table_offset + row * 488u + 216u, uint64_t{16u}}) !=
                  snapshot.specialization_reads.end(), "waterfall descriptor read was not captured");
    }
    // SMEM reads the complete DWORD prefix and zeroes the missing format word.
    user_data[1] = 359u << 16u;
    memory.reads = 0;
    Check(materialize() && memory.reads == 2u &&
              std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + table_offset + 704u, uint64_t{12u}}) !=
                  snapshot.specialization_reads.end() &&
              std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + table_offset + 704u, uint64_t{16u}}) ==
                  snapshot.specialization_reads.end() &&
              std::ranges::all_of(snapshot.buffers.back().dwords, [](auto word) { return word == 0u; }),
          "waterfall partial descriptor overread or retained its missing format word");
    user_data[1] = 488u << 16u;
    memory.fail_address = memory.base + table_offset + 704u;
    Check(!materialize(), "waterfall descriptor bypassed a coherent read failure");
    memory.fail_address = UINT64_MAX;
    Check(materialize(), "waterfall descriptor did not recover after a failed refresh");
    user_data[2] = MaxIndirectDescriptorProbes + 1u;
    memory.reads = 0;
    Check(!materialize() && memory.reads == 0u, "waterfall accepted an excessive runtime bound");
    user_data[2] = 2u;
  }
}

void TestGpuSelectedBufferRecords() {
  namespace Prospero = Libs::Graphics::Prospero;
  using Libs::Graphics::DstSel;
  for (const uint32_t bias : {0u, 7u}) {
    auto fixture = MakeBufferRecordFixture(1u, false, true, bias);
    const auto plan = ExtractResourcePlan(fixture->program);
    std::vector<uint32_t> roots;
    std::vector<const Inst*> selections(plan.info.buffers.size());
    for (uint32_t resource = 0; resource < plan.info.buffers.size(); ++resource) {
      const auto& source = plan.descriptor_sources[plan.info.buffers[resource].source];
      if (!source.indirect_descriptor) continue;
      const auto& indirect = *source.indirect_descriptor;
      Check(indirect.table_scalar && indirect.table_stride == 192u &&
                indirect.table_offset == bias && indirect.key_count.IsEmpty() && !indirect.selector,
            "GPU record selector became a CPU key-list read or lost its affine geometry");
      roots.push_back(resource);
    }
    Check(roots.size() == 2u, "GPU record accesses lost their two descriptor roots");
    uint32_t selection_count = 0;
    for (const auto* block : fixture->program.blocks)
      for (const auto& inst : *block)
        if (inst.GetOpcode() == ValueOpcode::GetBufferResourceIndex) {
          selections[inst.Flags<uint32_t>()] = &inst;
          ++selection_count;
        }
    Check(selection_count == 2u, "GPU descriptor uses duplicated mapping searches");
    const auto evaluate_key = [&](auto&& self, Value value, uint32_t key) -> uint32_t {
      value = value.Resolve();
      if (value.IsImmediate()) return value.U32();
      const auto& inst = *value.Instruction();
      if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) return key;
      const auto a = self(self, inst.Arg(0), key);
      const auto b = self(self, inst.Arg(1), key);
      switch (inst.GetOpcode()) {
        case ValueOpcode::IMul32: return a * b;
        case ValueOpcode::IAdd32: return a + b;
        case ValueOpcode::ShiftRightLogical32: return a >> b;
        default: throw std::runtime_error("unexpected GPU descriptor key expression");
      }
    };
    LinearTestMemory memory;
    constexpr uint32_t table_offset = 256u;
    memory.fail_address = memory.base + 0x2000u;
    const auto aligned_bias = bias & ~3u;
    for (uint32_t slot = 0; slot < 4u; ++slot) {
      for (uint32_t member = 0; member < 2u; ++member) {
        const auto first = (table_offset + aligned_bias + 160u + member * 16u + slot * 64u) / 4u;
        memory.words[first] = 0x100000u + member * 0x10000u + slot * 0x1000u;
        memory.words[first + 1u] = (member == 0u ? 8u : 3u) << 16u;
        memory.words[first + 2u] = 16u;
        memory.words[first + 3u] = member == 0u
            ? (static_cast<uint32_t>(Prospero::BufferFormat::k16_16_16_16Float) << 12u) |
                  DstSel(4, 5, 6, 1) : 0x5204u;
      }
    }
    // SMEM aligns the base and dynamic offset independently; its V# format is ignored.
    std::array<uint32_t, 8> user_data{uint32_t(memory.base) + table_offset + 3u, 0u, 0u, 0u,
        uint32_t(memory.fail_address), 12u << 16u, 129600u, 0x5204u};
    SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
        .read_specialization_memory = ReadLinearTestMemory};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    const auto materialize = [&] { return MaterializeResources(plan, runtime, snapshot, specialization); };
    for (const uint32_t size : {0u, 384u + aligned_bias, 0u, 384u + aligned_bias}) {
      user_data[2] = size;
      memory.reads = 0u;
      Check(materialize() && memory.reads == (size == 0u ? 0u : 8u) &&
                snapshot.buffers.size() == plan.info.buffers.size() + (size == 0u ? 0u : 8u),
            "affine table refresh read GPU selector bytes, unrelated fields or stale candidates");
    }
    for (uint32_t member = 0; member < 2u; ++member) {
      const auto root = roots[member];
      const auto mapping = specialization.buffers[root].indirect_mapping_offset;
      Check(specialization.buffers[root].indirect_search_iterations == 0u &&
                snapshot.flattened_srt[mapping] == 4u,
            "affine descriptor table lost its compact dense mapping");
      // These keys land at byte offsets 0,64,128,192 after U32 multiplication by192.
      for (const auto [key, slot] : {std::pair{0u, 0u}, std::pair{44739243u, 1u},
                                    std::pair{22369622u, 2u}, std::pair{1u, 3u}}) {
        const auto index = evaluate_key(evaluate_key, selections[root]->Arg(0), key) >> 2u;
        Check(index == slot && snapshot.flattened_srt[mapping + 1u + index] == slot + 1u,
              "wrapped GPU record key selected the wrong physical descriptor");
        const auto candidate = plan.info.buffers.size() + member * 4u + slot;
        Check(snapshot.buffers[candidate].dwords[0] == 0x100000u + member * 0x10000u + slot * 0x1000u,
              "affine table read a stride192 row instead of a reachable stride64 member");
        Check(std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + table_offset + aligned_bias + 160u + member * 16u + slot * 64u,
                            uint64_t{16u}}) != snapshot.specialization_reads.end(),
              "affine descriptor read escaped writable-alias capture");
      }
    }
    // The trailing V# has only three readable DWORDs; its missing format word is zero.
    const auto tail = (table_offset + aligned_bias + 416u) / 4u;
    memory.words[tail] = 0x300000u;
    memory.words[tail + 1u] = 8u << 16u;
    memory.words[tail + 2u] = 16u;
    memory.words[tail + 3u] = 0x5204u;
    user_data[2] = 428u + aligned_bias;
    memory.reads = 0u;
    Check(materialize() && memory.reads == 9u &&
              snapshot.buffers.size() == plan.info.buffers.size() + 8u &&
              std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + table_offset + aligned_bias + 416u, uint64_t{12u}}) !=
                  snapshot.specialization_reads.end(),
          "partial descriptor did not zero-fill and capture only in-bounds DWORDs");
    user_data[2] = aligned_bias + 160u + MaxIndirectDescriptorProbes * 64u + 4u;
    memory.reads = 0u;
    Check(!materialize() && memory.reads == 0u,
          "oversized affine table was read before its candidate cap was checked");
  }
  CheckFatal([] { MakeBufferRecordFixture(1u, false, true, 0u, true); },
             "not a valid runtime value", "split dynamic descriptor offsets lost independent U32 wrapping");
}

void TestWorkgroupSelectedBufferRecords() {
  namespace Prospero = Libs::Graphics::Prospero;
  auto fixture = MakeBufferRecordFixture(1u, false, true, 0u, false, true);
  auto plan = ExtractResourcePlan(fixture->program);
  std::vector<uint32_t> roots;
  for (uint32_t root = 0; root < plan.info.buffers.size(); ++root) {
    const auto& source = plan.descriptor_sources[plan.info.buffers[root].source];
    if (!source.indirect_descriptor) continue;
    const auto& indirect = *source.indirect_descriptor;
    Check(indirect.workgroup_axis == 0u && indirect.selector_first == Value(0u) &&
              indirect.selector && indirect.selector->stride == 12u &&
              indirect.table_stride == 192u && !indirect.table_scalar,
          "workgroup selector lost its exact prefix or descriptor geometry");
    roots.push_back(root);
  }
  Check(roots.size() == 2u, "workgroup selector did not retain both buffer operations");
  LinearTestMemory memory;
  constexpr uint32_t table = 256u, selector = 0x1000u;
  memory.words.resize((selector + MaxIndirectDescriptorProbes * 12u) / 4u);
  std::fill_n(memory.words.begin() + table / 4u, 384u / 4u, 0xdeadbeefu);
  for (uint32_t row = 0; row < 2u; ++row) {
    for (uint32_t member = 0; member < 2u; ++member) {
      const auto first = (table + row * 192u + 160u + member * 16u) / 4u;
      memory.words[first] = 0x100000u + row * 0x1000u + member * 0x10000u;
      memory.words[first + 1u] = (member == 0u ? 8u : 1u) << 16u;
      memory.words[first + 2u] = 16u;
      memory.words[first + 3u] = member == 0u
          ? (uint32_t(Prospero::BufferFormat::k16_16_16_16Float) << 12u) | Libs::Graphics::DstSel(4, 5, 6, 7)
          : 0x5204u;
    }
  }
  const std::array<uint32_t, 4> numeric_descriptor{0x640u, 3u, 0x3ba3d70au, 0x3a23d70au};
  std::copy(numeric_descriptor.begin(), numeric_descriptor.end(), memory.words.begin() + (table + 304u) / 4u);
  std::array<uint32_t, 8> user_data{uint32_t(memory.base) + table, 192u << 16u, 2u, 0x5204u,
      uint32_t(memory.base) + selector, 12u << 16u, MaxIndirectDescriptorProbes, 0x5204u};
  memory.watched_address = memory.base;
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory,
      .indirect_args_address = memory.base};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto materialize = [&] {
    memory.reads = memory.watched_reads = 0u;
    return MaterializeResources(plan, runtime, snapshot, specialization);
  };
  for (const auto dimensions : {std::array{2u, 1u, 1u}, std::array{0u, 1u, 1u},
                                std::array{2u, 0u, 1u}, std::array{2u, 1u, 0u},
                                std::array{1u, 1u, 1u}, std::array{2u, 1u, 1u}}) {
    std::copy(dimensions.begin(), dimensions.end(), memory.words.begin());
    memory.words[selector / 4u] = dimensions[0] == 1u ? 0u : 1u;
    memory.words[selector / 4u + 3u] = 0u;
    memory.words[selector / 4u + 6u] = 22369622u; // Would select the numeric fields at +288/+304.
    const bool empty = std::ranges::find(dimensions, 0u) != dimensions.end();
    Check(materialize() && memory.watched_reads == 1u && memory.watched_dwords == 3u &&
              snapshot.buffers.size() == plan.info.buffers.size() + (empty ? 0u : (dimensions[0] - 1u) * 2u) &&
              memory.reads == (empty ? 1u : 2u + dimensions[0] * 2u),
          "workgroup prefix reread arguments, visited an inactive key or retained stale candidates");
    Check(std::ranges::find(snapshot.specialization_reads, std::pair{memory.base, uint64_t{12u}}) !=
              snapshot.specialization_reads.end(),
          "indirect dispatch arguments escaped writable-alias tracking");
    if (!empty)
      Check(std::ranges::find(snapshot.specialization_reads,
                std::pair{memory.base + selector, uint64_t{(dimensions[0] - 1u) * 12u + 4u}}) !=
                snapshot.specialization_reads.end(),
            "batched selector prefix escaped writable-alias tracking");
  }
  const auto valid = specialization;
  memory.words[selector / 4u] = 0x40000001u; // key*192 wraps to the valid second row.
  Check(materialize() && specialization == valid,
        "GPU descriptor key lost its native U32 multiplication wrap");
  memory.words[selector / 4u] = 22369622u;
  Check(!materialize(), "an actually selected numeric descriptor was silently filtered");
  memory.words[selector / 4u] = 1u;
  user_data[5] = 0u;
  user_data[6] = 14u; // Second key has fewer than four in-bounds bytes: SMEM returns zero.
  Check(materialize() && snapshot.buffers.size() == plan.info.buffers.size() + 2u,
        "partial scalar selector did not zero-fill its OOB DWORD");
  user_data[6] = 0u;
  Check(materialize() && snapshot.buffers.size() == plan.info.buffers.size() &&
            memory.reads == 3u,
        "empty selector V# did not select key zero without reading payload");
  // Existing vector selectors use the V# stride; each scalar snapshot read
  // independently rounds its resulting byte offset down to a DWORD.
  for (const uint32_t stride : {5u, 6u}) {
    for (const auto root : roots) {
      auto& indirect = *plan.descriptor_sources[plan.info.buffers[root].source].indirect_descriptor;
      indirect.selector->stride = 0u;
      indirect.selector_first = Value(1u);
    }
    user_data[5] = stride << 16u;
    user_data[6] = 5u;
    memory.words[0] = 4u;
    std::fill_n(memory.words.begin() + selector / 4u, 8u, 0xdeadbeefu);
    for (uint32_t index = 0; index < 4u; ++index)
      memory.words[selector / 4u + ((index + 1u) * stride) / 4u] = index & 1u;
    Check(materialize() && memory.reads == 6u &&
              snapshot.buffers.size() == plan.info.buffers.size() + 2u,
          "batched vector selector changed independent scalar DWORD alignment");
  }
  for (const auto root : roots) {
    auto& indirect = *plan.descriptor_sources[plan.info.buffers[root].source].indirect_descriptor;
    indirect.selector->stride = 12u;
    indirect.selector_first = Value(0u);
  }
  user_data[5] = 12u << 16u;
  user_data[6] = MaxIndirectDescriptorProbes;
  std::fill(memory.words.begin() + selector / 4u, memory.words.end(), 0u);
  memory.words[0] = MaxIndirectDescriptorProbes;
  Check(materialize() && memory.reads == 6u,
        "strided selector prefix was not read in bounded contiguous batches");
  memory.words[0] = MaxIndirectDescriptorProbes + 1u;
  Check(!materialize() && memory.reads == 1u,
        "selector cap was checked after reading its GPU payload");
  memory.words[0] = 2u;
  for (const auto root : roots)
    plan.descriptor_sources[plan.info.buffers[root].source].indirect_descriptor->selector->offset = UINT32_MAX - 3u;
  Check(!materialize() && memory.reads == 1u,
        "selector offset arithmetic accepted a wrapped prefix span");
  for (const auto root : roots)
    plan.descriptor_sources[plan.info.buffers[root].source].indirect_descriptor->selector->offset = 0u;
  runtime.indirect_args_address = 0u;
  const std::array direct_counts{1u, 1u, 1u};
  runtime.workgroup_counts = std::span(direct_counts).first(1u);
  Check(!materialize() && memory.reads == 0u,
        "workgroup prefix accepted an incomplete dispatch extent");
  runtime.workgroup_counts = direct_counts;
  memory.fail_address = memory.base;
  Check(materialize() && memory.watched_reads == 0u,
        "direct workgroup counts unnecessarily read indirect arguments");
  const auto ordinary = MakeBufferRecordFixture(1u, false, true);
  const auto ordinary_plan = ExtractResourcePlan(ordinary->program);
  runtime.indirect_args_address = memory.base;
  user_data[2] = 0u;
  Check(MaterializeResources(ordinary_plan, runtime, snapshot, specialization),
        "a plan without workgroup counts read the indirect argument pointer");
}

void TestGpuSelectedFormattedRecordStore() {
  namespace Prospero = Libs::Graphics::Prospero;
  enum class Prefix { None, Forward, Reversed, Lane, TwoAxes };
  const auto make = [](uint32_t stride = 592u, uint32_t shift = 16u, bool split = false,
                       bool wide = false, Prefix prefix = Prefix::None) {
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    std::array<Value, 4> table_words, selector_words;
    for (uint32_t word = 0; word < 4u; ++word) {
      table_words[word] = f.UserData(word);
      selector_words[word] = f.UserData(4u + word);
    }
    const auto table = wide ? f.Address(table_words[0], table_words[1]) : f.Buffer(table_words);
    const auto lane = f.Emit(ValueOpcode::LaneId);
    auto selector_index = lane;
    if (prefix != Prefix::None) {
      const auto group = f.Emit(ValueOpcode::GetBuiltin,
          {Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)), Value(0u)});
      const auto first = prefix == Prefix::Lane ? lane : prefix == Prefix::TwoAxes
          ? f.Emit(ValueOpcode::GetBuiltin,
              {Value(static_cast<uint32_t>(StageInputKind::WorkgroupId)), Value(1u)})
          : f.Emit(ValueOpcode::LoadAddressU32, {table, Value(0u), Value(0u), Value(true)},
              f.AddMemory({.kind = ResourceKind::ScalarAddress, .offset = 0x234u}, 0x1088u));
      selector_index = f.Emit(ValueOpcode::IAdd32,
          {prefix == Prefix::Reversed ? group : first, prefix == Prefix::Reversed ? first : group});
    }
    const auto packed = wide ? f.Emit(ValueOpcode::ReadConstBuffer,
        {f.Buffer(selector_words), f.Emit(ValueOpcode::IMul32, {selector_index, Value(8u)})},
        f.AddMemory({.kind = ResourceKind::ScalarBuffer, .offset = 4u}, 0x1c4cu)) :
        f.Emit(ValueOpcode::LoadBufferU32,
        {f.Buffer(selector_words), lane, Value(0u), Value(0u), Value(true)},
        f.AddMemory({.kind = ResourceKind::Buffer, .idxen = true}, 0x1bcu));
    const auto key = wide ? (shift == 0u ? packed : f.Emit(ValueOpcode::BitFieldUExtract,
        {packed, Value(16u), Value(5u)})) : f.Emit(ValueOpcode::ReadLane,
        {f.Emit(ValueOpcode::ShiftRightLogical32, {packed, Value(shift)}), Value(0u)});
    const auto offset = f.Emit(ValueOpcode::IMul32, {key, Value(stride)});
    std::array<Value, 4> words;
    for (uint32_t word = 0; word < 4u; ++word) {
      const auto dynamic = split ? f.Emit(ValueOpcode::IAdd32, {offset, Value(word * 4u)}) : offset;
      const auto flags = f.AddMemory({.kind = wide ? ResourceKind::ScalarAddress : ResourceKind::ScalarBuffer,
          .offset = (wide ? 48u : 448u) + (split ? 0u : word * 4u),
          .component_index = word, .component_count = 4u}, wide ? 0x1d88u : 0x2e8u);
      words[word] = wide ? f.Emit(ValueOpcode::LoadAddressU32,
          {table, dynamic, Value(0u), Value(true)}, flags) :
          f.Emit(ValueOpcode::ReadConstBuffer, {table, dynamic}, flags);
    }
    f.Emit(ValueOpcode::LoadBufferU32,
        {f.Buffer(words), lane, Value(0u), Value(0u), Value(true)},
        f.AddMemory({.kind = ResourceKind::Buffer, .formatted = true, .idxen = true}, 0x2f8u));
    const auto data = wide ? f.Emit(ValueOpcode::CompositeConstructU32x4,
        {Value(0x3f800000u), Value(0xc0000000u), Value(0x3f000000u), Value(0x3e800000u)}) :
        Value(uint32_t(-127));
    for (uint32_t use = 0; use < 2u; ++use)
      f.Emit(wide ? ValueOpcode::StoreBufferU32x4 : ValueOpcode::StoreBufferU32,
          {f.Buffer(words), lane, Value(0u), Value(0u), data, Value(true)},
          f.AddMemory({.kind = ResourceKind::Buffer, .data_dwords = wide ? 4u : 1u,
                       .component_count = wide ? 4u : 1u, .formatted = true,
                       .idxen = true, .coherent = true}, 0x1618u + use * 8u));
    f.PlanAndTrack();
    return fixture;
  };
  auto fixture = make();
  const auto plan = ExtractResourcePlan(fixture->program);
  uint32_t root = UINT32_MAX;
  for (uint32_t resource = 0; resource < plan.info.buffers.size(); ++resource) {
    const auto& indirect = plan.descriptor_sources[plan.info.buffers[resource].source].indirect_descriptor;
    if (!indirect) continue;
    Check(root == UINT32_MAX && indirect->table_scalar && indirect->table_maximum == 65535u &&
              !indirect->pointer_table && !indirect->selector && indirect->key_count.IsEmpty() &&
              indirect->table_stride == 592u && indirect->table_immediate == 448u,
          "formatted record store lost its exact rows or captured the GPU selector");
    root = resource;
  }
  Check(root != UINT32_MAX, "formatted record store has no native candidate root");
  const Inst* selection = nullptr;
  uint32_t scalar_reads = 0u, bda_reads = 0u, stores = 0u;
  for (const auto* block : fixture->program.blocks)
    for (const auto& inst : *block) {
      if (inst.GetOpcode() == ValueOpcode::GetBufferResourceIndex) {
        Check(selection == nullptr && inst.Flags<uint32_t>() == root,
              "formatted record stores duplicated selection searches");
        selection = &inst;
        const auto* encoded = inst.Arg(0).Resolve().TryInstruction();
        Check(encoded && encoded->GetOpcode() == ValueOpcode::ShiftLeftLogical32 &&
                  encoded->Arg(1) == Value(2u) &&
                  encoded->Arg(0).ResolveInstruction()->GetOpcode() == ValueOpcode::ReadLane,
              "exact record selection used an affine wrapped-byte ordinal");
        bool defined = false;
        for (const auto& prior : *block) {
          if (&prior == &inst) break;
          defined |= &prior == encoded;
        }
        Check(defined, "exact row key was defined after its resource selection");
      }
      if (inst.GetOpcode() == ValueOpcode::ReadConstBuffer) {
        const auto& memory = fixture->program.memory_info[inst.Flags<MemoryFlags>().index];
        Check(memory.kind == ResourceKind::ScalarBuffer && !memory.planning_only,
              "store planning consumed descriptor words still used by the BDA load");
        ++scalar_reads;
      }
      if (inst.GetOpcode() == ValueOpcode::LoadBufferU32 && inst.Flags<MemoryFlags>().pc == 0x2f8u) {
        const auto& memory = fixture->program.memory_info[inst.Flags<MemoryFlags>().index];
        Check(memory.kind == ResourceKind::IndirectBuffer && memory.formatted,
              "store planning replaced the existing unrestricted BDA formatted load");
        ++bda_reads;
      }
      if (inst.GetOpcode() == ValueOpcode::StoreBufferU32) {
        Check(selection && inst.Arg(0).ResolveInstruction()->Arg(0).ResolveInstruction() == selection,
              "formatted store lost its dominating shared selection");
        ++stores;
      }
    }
  Check(selection && scalar_reads == 4u && bda_reads == 1u && stores == 2u,
        "shared formatted record fixture lost an exercised descriptor consumer");
  LinearTestMemory memory;
  std::ranges::fill(memory.words, 0xdeadbeefu);
  constexpr uint32_t table_offset = 256u;
  constexpr uint32_t first = (table_offset + 448u) / 4u;
  for (uint32_t row = 0; row < 6u; ++row) {
    const auto word = first + row * 592u / 4u;
    memory.words[word] = 0x100000u + row * 32u;
    memory.words[word + 1u] = 1u << 16u;
    memory.words[word + 2u] = 20u;
    memory.words[word + 3u] = (uint32_t(Prospero::BufferFormat::k8SInt) << 12u) | 4u;
  }
  memory.fail_address = memory.base + 0x2000u;
  std::array<uint32_t, 8> user_data{uint32_t(memory.base) + table_offset, 592u << 16u, 6u, 0x5204u,
      uint32_t(memory.fail_address), 4u << 16u, 64u, 0x5204u};
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
      .read_specialization_memory = ReadLinearTestMemory};
  FakeGuestMemory fake(ReadRuntimeMemory, &runtime);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto materialize = [&] { return MaterializeResources(plan, runtime, snapshot, specialization); };
  for (const uint32_t count : {0u, 6u, 0u, 6u}) {
    user_data[2] = count;
    memory.reads = 0u;
    Check(materialize() && memory.reads == count &&
              snapshot.buffers.size() == plan.info.buffers.size() + count,
          "exact formatted records read GPU selector bytes, row padding or stale candidates");
  }
  const auto mapping = specialization.buffers[root].indirect_mapping_offset;
  Check(snapshot.flattened_srt[mapping] == 6u, "exact formatted records lost the dense row map");
  for (uint32_t row = 0; row < 6u; ++row)
    Check(snapshot.flattened_srt[mapping + 1u + row] == row + 1u &&
              std::ranges::find(snapshot.specialization_reads,
                  std::pair{memory.base + table_offset + row * 592u + 448u, uint64_t{16u}}) !=
                  snapshot.specialization_reads.end(),
          "exact row descriptor escaped selection mapping or writable-alias capture");
  const auto original = specialization;
  memory.words[first] += 1u;
  Check(materialize() && specialization == original,
        "signed byte destination refresh required alignment or needless specialization");
  for (const auto [word, value] : std::array<std::pair<uint32_t, uint32_t>, 5>{{
      {first + 1u, 2u << 16u}, {first + 1u, (1u << 16u) | (1u << 31u)},
      {first + 3u, (uint32_t(Prospero::BufferFormat::k8UInt) << 12u) | 4u},
      {first + 3u, memory.words[first + 3u] | (1u << 28u)},
      {first + 3u, memory.words[first + 3u] | (4u << 3u)}}}) {
    const auto saved = memory.words[word];
    memory.words[word] = value;
    Check(!materialize(), "signed byte candidate accepted an unproved format, stride or bounds");
    memory.words[word] = saved;
  }
  user_data[1] = 0u;
  user_data[2] = 5u * 592u + 448u + 12u;
  memory.watched_address = memory.base + table_offset + 5u * 592u + 448u;
  Check(materialize() && memory.watched_dwords == 3u &&
            snapshot.flattened_srt[specialization.buffers[root].indirect_mapping_offset] == 5u &&
            snapshot.buffers.size() == plan.info.buffers.size() + 5u,
        "partial direct V# did not zero-fill its missing format DWORD");
  user_data[1] = 592u << 16u;
  user_data[2] = 6u;
  Check(materialize(), "valid signed byte records did not recover after a partial tail");
  ApplyResourceSpecialization(fixture->program, specialization);
  for (const auto* block : fixture->program.blocks)
    for (const auto& inst : *block)
      if (inst.GetOpcode() == ValueOpcode::StoreBufferU32) {
        const auto& info = fixture->program.memory_info[inst.Flags<MemoryFlags>().index];
        Check(info.kind == ResourceKind::Buffer && info.formatted && info.coherent,
              "signed byte store bypassed shared formatted conversion or GLC");
      }
  for (const auto [stride, shift, split] :
       {std::tuple{592u, 0u, false}, std::tuple{65540u, 16u, false}, std::tuple{592u, 16u, true}})
    CheckFatal([&] { make(stride, shift, split); }, "not a valid runtime value",
               "exact record store accepted an unbounded, wrapping or split descriptor offset");

  auto wide = make(16u, 16u, false, true);
  auto wide_plan = ExtractResourcePlan(wide->program);
  uint32_t wide_root = UINT32_MAX;
  for (uint32_t resource = 0; resource < wide_plan.info.buffers.size(); ++resource) {
    const auto& source = wide_plan.descriptor_sources[wide_plan.info.buffers[resource].source];
    if (!source.indirect_descriptor) continue;
    const auto& indirect = *source.indirect_descriptor;
    Check(wide_root == UINT32_MAX && !indirect.table_scalar && !indirect.selector &&
              !indirect.table_maximum && !indirect.pointer_table &&
              indirect.key_count == Value(32u) && indirect.table_stride == 16u &&
              indirect.table_offset == 0u && indirect.table_immediate == 48u &&
              wide_plan.descriptor_sources[indirect.table_source].dword_count == 2u &&
              wide_plan.info.buffers[resource].max_byte_extent == 16u,
          "RGBA16 store lost its finite raw-pointer rows or captured the GPU selector");
    wide_root = resource;
  }
  Check(wide_root != UINT32_MAX, "RGBA16 store has no native candidate root");
  selection = nullptr;
  uint32_t address_reads = 0u;
  stores = 0u;
  for (const auto* block : wide->program.blocks)
    for (const auto& inst : *block) {
      if (inst.GetOpcode() == ValueOpcode::GetBufferResourceIndex) {
        Check(selection == nullptr && inst.Flags<uint32_t>() == wide_root &&
                  inst.Arg(0).ResolveInstruction()->GetOpcode() == ValueOpcode::BitFieldUExtract,
              "RGBA16 stores duplicated selection or replaced the runtime finite key");
        selection = &inst;
      }
      if (inst.GetOpcode() == ValueOpcode::LoadAddressU32) {
        Check(!wide->program.memory_info[inst.Flags<MemoryFlags>().index].planning_only,
              "RGBA16 store consumed descriptor words still used by the BDA read");
        ++address_reads;
      }
      if (inst.GetOpcode() == ValueOpcode::StoreBufferU32x4) {
        Check(selection && inst.Arg(0).ResolveInstruction()->Arg(0).ResolveInstruction() == selection,
              "RGBA16 store lost its dominating shared selection");
        ++stores;
      }
    }
  Check(address_reads == 4u && stores == 2u, "RGBA16 fixture lost shared descriptor uses");
  std::fill_n(memory.words.begin() + table_offset / 4u, 32u * 4u, 0u);
  constexpr uint32_t rgba = (uint32_t(Prospero::BufferFormat::k16_16_16_16Float) << 12u) | 0xfacu;
  user_data[0] = uint32_t(memory.base) + table_offset - 48u;
  user_data[1] = 0u;
  const auto wide_materialize = [&] {
    return MaterializeResources(wide_plan, runtime, snapshot, specialization);
  };
  for (const bool populated : {false, true, false, true}) {
    for (const uint32_t row : {0u, 3u}) {
      const std::array descriptor{0x100000u + row * 256u, 8u << 16u, 20u, rgba};
      for (uint32_t word = 0; word < 4u; ++word)
        memory.words[table_offset / 4u + row * 4u + word] = populated ? descriptor[word] : 0u;
    }
    memory.reads = 0u;
    Check(wide_materialize() && memory.reads == 32u &&
              snapshot.buffers.size() == wide_plan.info.buffers.size() + (populated ? 2u : 0u),
          "finite RGBA16 table read GPU keys or retained stale null/active candidates");
  }
  const auto wide_mapping = specialization.buffers[wide_root].indirect_mapping_offset;
  Check(snapshot.flattened_srt[wide_mapping] == 32u,
        "finite RGBA16 table lost its complete key map");
  for (uint32_t row = 0; row < 32u; ++row)
    Check(snapshot.flattened_srt[wide_mapping + 1u + row * 2u] == row &&
              snapshot.flattened_srt[wide_mapping + 2u + row * 2u] ==
                  (row == 0u ? 0u : row == 3u ? 2u : 1u),
          "finite RGBA16 table selected the wrong active or null candidate");
  const auto wide_original = specialization;
  memory.words[table_offset / 4u] += 4u;
  Check(wide_materialize() && specialization == wide_original &&
            snapshot.buffers[wide_root].dwords[0] == memory.words[table_offset / 4u],
        "RGBA16 destination refresh froze the address or changed format specialization");
  for (const auto [word, value] : std::array<std::pair<uint32_t, uint32_t>, 6>{{
      {0u, memory.words[table_offset / 4u] | 1u}, {1u, 16u << 16u},
      {1u, (8u << 16u) | (1u << 31u)}, {3u, rgba | (1u << 28u)},
      {3u, (uint32_t(Prospero::BufferFormat::k32UInt) << 12u) | 4u}, {3u, rgba ^ 1u}}}) {
    auto& field = memory.words[table_offset / 4u + word];
    const auto saved = field;
    field = value;
    Check(!wide_materialize(), "RGBA16 store accepted unproved format, alignment, stride, bounds or swizzle");
    field = saved;
  }
  wide_plan.info.buffers[wide_root].max_byte_extent = 4u;
  Check(!wide_materialize(), "scalar formatted store incorrectly admitted an RGBA16 candidate");
  wide_plan.info.buffers[wide_root].max_byte_extent = 16u;
  auto& wide_indirect = *wide_plan.descriptor_sources[
      wide_plan.info.buffers[wide_root].source].indirect_descriptor;
  for (const bool overflow : {false, true}) {
    wide_indirect.key_count = Value(overflow ? 32u : MaxIndirectDescriptorProbes + 1u);
    wide_indirect.table_stride = overflow ? 0x10000000u : 16u;
    memory.reads = 0u;
    Check(!wide_materialize() && memory.reads == 0u,
          "finite RGBA16 table read memory before rejecting its cap or wrapped span");
  }
  wide_indirect.key_count = Value(32u);
  wide_indirect.table_stride = 16u;
  Check(wide_materialize(), "RGBA16 candidates did not recover after invalid metadata");
  ApplyResourceSpecialization(wide->program, specialization);
  for (const auto* block : wide->program.blocks)
    for (const auto& inst : *block)
      if (inst.GetOpcode() == ValueOpcode::StoreBufferU32x4) {
        const auto& info = wide->program.memory_info[inst.Flags<MemoryFlags>().index];
        Check(info.kind == ResourceKind::Buffer && info.resource == wide_root && info.formatted &&
                  info.coherent && info.data_bits == 32u && info.component_count == 4u,
              "RGBA16 specialization lost native wide conversion, selection or GLC");
      }
  for (const auto [shift, split] : {std::pair{0u, false}, std::pair{16u, true}})
    CheckFatal([&] { make(16u, shift, split, true); }, "not a valid runtime value",
               "finite RGBA16 store accepted an unbounded key or independently wrapping DWORDs");

  struct PrefixMemory : LinearTestMemory { uint32_t read_attempts = 0u; } prefix_memory;
  const auto read_prefix = [](void* userdata, uint64_t address, std::span<uint32_t> words) {
    auto* memory = static_cast<PrefixMemory*>(userdata);
    ++memory->read_attempts;
    return ReadLinearTestMemory(memory, address, words);
  };
  std::ranges::fill(prefix_memory.words, 0xdeadbeefu);
  constexpr uint32_t selector_offset = 0x1000u;
  constexpr uint32_t first_offset = table_offset - 48u + 0x234u;
  for (uint32_t row = 0; row < 2u; ++row) {
    const std::array descriptor{0x100000u + row * 256u, 8u << 16u, 20u, rgba};
    std::copy(descriptor.begin(), descriptor.end(),
              prefix_memory.words.begin() + table_offset / 4u + row * 4u);
  }
  const std::array numeric_descriptor{0xbf8d214eu, 0xbf78904bu, 0x3e930d39u, 0x33d6bf95u};
  std::copy(numeric_descriptor.begin(), numeric_descriptor.end(),
            prefix_memory.words.begin() + table_offset / 4u + 4u * 4u);
  for (uint32_t index = 0; index < 12u; ++index)
    prefix_memory.words[selector_offset / 4u + index * 2u + 1u] =
        ((index < 4u ? index & 1u : index < 6u ? 1u : 4u) << 16u) | 0xbeefu;
  std::array<uint32_t, 8> prefix_data{uint32_t(prefix_memory.base) + table_offset - 48u, 0u, 0u, 0u,
      uint32_t(prefix_memory.base) + selector_offset, 8u << 16u, 12u, 0x5204u};
  std::array counts{2u, 1u, 1u};
  runtime = {.user_data = prefix_data, .userdata = &prefix_memory,
      .read_specialization_memory = read_prefix, .workgroup_counts = counts};
  prefix_memory.watched_address = prefix_memory.base + selector_offset + 4u;
  for (const auto prefix : {Prefix::Forward, Prefix::Reversed}) {
    const auto selected = make(16u, 16u, false, true, prefix);
    auto selected_plan = ExtractResourcePlan(selected->program);
    auto& indirect = *selected_plan.descriptor_sources[
        selected_plan.info.buffers[wide_root].source].indirect_descriptor;
    Check(indirect.workgroup_axis == 0u && !indirect.selector_first.IsImmediate() &&
              indirect.key_count.IsEmpty() && indirect.selector && indirect.selector->stride == 8u &&
              indirect.selector->offset == 4u && indirect.table_immediate == 48u,
          "runtime-first selector lost its workgroup prefix or captured table geometry");
    const auto refresh = [&] {
      prefix_memory.watched_reads = 0u;
      prefix_memory.read_attempts = 0u;
      return MaterializeResources(selected_plan, runtime, snapshot, specialization);
    };
    for (const uint32_t first : {0u, 2u, 4u, 0u}) {
      prefix_memory.words[first_offset / 4u] = first;
      Check(refresh() && snapshot.buffers.size() == selected_plan.info.buffers.size() + (first == 4u ? 0u : 1u) &&
                snapshot.buffers[wide_root].dwords[0] == 0x100000u + (first == 4u ? 256u : 0u),
            "runtime-first refresh froze a prior selector range or enumerated unrelated numeric rows");
      for (const auto read : {std::pair{prefix_memory.base + first_offset, uint64_t{4u}},
                             std::pair{prefix_memory.base + selector_offset + first * 8u + 4u, uint64_t{12u}}})
        Check(std::ranges::find(snapshot.specialization_reads, read) != snapshot.specialization_reads.end(),
              "runtime first or selected prefix escaped specialization read capture");
    }
    // An unreadable runtime first reads as zero.
    prefix_memory.words[first_offset / 4u] = 4u;
    prefix_memory.fail_address = prefix_memory.base + first_offset;
    Check(refresh() && snapshot.buffers.size() == selected_plan.info.buffers.size() + 1u &&
              snapshot.buffers[wide_root].dwords[0] == 0x100000u,
          "unreadable runtime first did not read as zero");
    prefix_memory.fail_address = UINT64_MAX;
    prefix_memory.words[first_offset / 4u] = 6u;
    Check(!refresh(), "an actually selected invalid numeric descriptor was silently omitted");
    for (const uint32_t first : {UINT32_MAX, 0x20000000u}) {
      prefix_memory.words[first_offset / 4u] = first;
      Check(!refresh() && prefix_memory.read_attempts == 1u,
            "wrapped first-plus-group or scaled prefix read selector payload");
    }
    prefix_memory.words[first_offset / 4u] = UINT32_MAX;
    counts[0] = 7u;
    indirect.selector->stride = 0xfffffffcu;
    Check(!refresh() && prefix_memory.read_attempts == 1u,
          "wrapped first-plus-group overflowed the host prefix product before rejection");
    indirect.selector->stride = 8u;
    counts[0] = 1u;
    prefix_memory.words[first_offset / 4u] = 0x1fffffffu;
    Check(refresh() && prefix_memory.watched_reads == 0u && snapshot.buffers[wide_root].dwords[0] == 0x100000u,
          "last nonwrapping selector DWORD lost scalar OOB key zero");
    prefix_memory.words[first_offset / 4u] = UINT32_MAX;
    for (const auto empty : {std::array{0u, 1u, 1u}, std::array{2u, 0u, 1u}, std::array{2u, 1u, 0u}}) {
      counts = empty;
      Check(refresh() && prefix_memory.watched_reads == 0u &&
                snapshot.buffers.size() == selected_plan.info.buffers.size() &&
                snapshot.buffers[wide_root].dwords[0] == 0u,
            "empty dispatch evaluated an unreachable wrapped selector prefix");
    }
    counts = {2u, 1u, 1u};
  }
  for (const auto prefix : {Prefix::Lane, Prefix::TwoAxes}) {
    const auto rejected = make(16u, 16u, false, true, prefix);
    const auto& indirect = rejected->program.descriptor_sources[
        rejected->program.info.buffers[wide_root].source].indirect_descriptor;
    Check(indirect && indirect->workgroup_axis == UINT32_MAX && !indirect->selector &&
              indirect->key_count == Value(32u),
          "lane-varying or multiple-axis first incorrectly narrowed the finite descriptor domain");
  }
}

void TestFiniteBufferSelection() {
  enum class Variant { Valid, WiderExec, Unbounded, Cycle, TooMany };
  const auto make = [](Variant variant) {
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    auto* entry = f.block;
    auto* left = f.AddBlock();
    auto* right = f.AddBlock();
    auto* merge = f.AddBlock();
    entry->AddBranch(left);
    (variant == Variant::Cycle ? merge : entry)->AddBranch(right);
    left->AddBranch(merge);
    right->AddBranch(merge);
    const auto table = f.Address(f.UserData(0u), f.UserData(1u));
    const auto lane = f.Emit(ValueOpcode::LaneId);
    const auto active = f.Emit(ValueOpcode::ULessThan32, {lane, Value(16u)});
    auto selected = Value(0u);
    for (uint32_t key = 1; key < 5u; ++key) {
      const auto match = f.Emit(ValueOpcode::IEqual32, {lane, Value(key)}, 0, right);
      selected = f.Emit(ValueOpcode::SelectU32, {match, Value(key), selected}, 0, right);
    }
    auto& phi = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    phi.AddPhiOperand(left, Value(0u));
    phi.AddPhiOperand(right, variant == Variant::Cycle
        ? f.Emit(ValueOpcode::IAdd32, {Value(&phi), Value(1u)}, 0, right)
        : variant == Variant::TooMany ? Value(MaxIndirectDescriptorProbes) : selected);
    f.block = merge;
    const auto stale = f.Emit(ValueOpcode::UndefU32);
    const auto masked = f.Emit(ValueOpcode::SelectU32,
        {active, variant == Variant::Unbounded ? lane : Value(&phi), stale});
    const auto key = f.Emit(ValueOpcode::ReadFirstLane, {masked, active});
    const auto offset = f.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(4u)});
    std::array<Value, 4> words;
    for (uint32_t word = 0; word < words.size(); ++word)
      words[word] = f.Emit(ValueOpcode::LoadAddressU32,
          {table, offset, Value(0u), Value(true)},
          f.AddMemory({.kind = ResourceKind::ScalarAddress, .offset = 32u + word * 4u,
                       .component_index = word, .component_count = 4u}, 0x50cu));
    f.Emit(ValueOpcode::StoreBufferU32,
        {f.Buffer(words), lane, Value(0u), Value(0u), Value(42u),
         variant == Variant::WiderExec ? Value(true) : active},
        f.AddMemory({.kind = ResourceKind::Buffer, .idxen = true, .coherent = true}, 0x530u));
    return fixture;
  };
  auto fixture = make(Variant::Valid);
  fixture->PlanAndTrack();
  const auto plan = ExtractResourcePlan(fixture->program);
  const auto& source = plan.descriptor_sources[plan.info.buffers[0].source];
  Check(source.indirect_descriptor && source.indirect_descriptor->key_count == Value(5u),
        "masked finite selection did not retain all five descriptor candidates");
  EliminateDeadCode(fixture->program.blocks);
  LinearTestMemory memory;
  for (uint32_t key = 0; key < 5u; ++key) {
    const auto word = 8u + key * 4u;
    memory.words[word] = 0x100000u + key * 0x10000u;
    memory.words[word + 1u] = 4u << 16u;
    memory.words[word + 2u] = 64u;
    memory.words[word + 3u] = 0x5204u;
  }
  memory.fail_address = memory.base + 32u + 5u * 16u;
  const std::array<uint32_t, 2> user_data{static_cast<uint32_t>(memory.base), 0u};
  SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                     .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) && snapshot.buffers.size() == 5u,
        "finite buffer selection lost table roots after DCE or probed outside its domain");
  for (const auto variant : {Variant::WiderExec, Variant::Unbounded, Variant::Cycle, Variant::TooMany}) {
    auto invalid = make(variant);
    CheckFatal([&] { invalid->PlanAndTrack(); }, "not a valid runtime value",
               "finite buffer selection accepted an unproved EXEC or selector domain");
  }
}

void TestPackedScalarBufferSelection() {
  const auto make = [](uint32_t shift_mask = 24u, bool cycle = false, uint32_t store_offset = 0u,
                       bool byte_store = false) {
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    auto* entry = f.block;
    auto* left = f.AddBlock();
    auto* right = f.AddBlock();
    auto* merge = f.AddBlock();
    entry->AddBranch(left);
    entry->AddBranch(right);
    left->AddBranch(merge);
    right->AddBranch(merge);
    const auto base = f.Address(f.UserData(0u), f.UserData(1u));
    const auto lane = f.Emit(ValueOpcode::LaneId);
    const auto offset = f.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(511u)});
    const auto shift = f.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(shift_mask)});
    auto& phi = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
    for (const auto [block, immediate] : {std::pair{left, 0x1c1u}, std::pair{right, 0x3c2u}}) {
      const auto read = f.Emit(ValueOpcode::LoadAddressU32,
          {base, offset, Value(0u), Value(true)},
          f.AddMemory({.kind = ResourceKind::ScalarAddress, .offset = immediate}, immediate), block);
      phi.AddPhiOperand(block, cycle ? Value(&phi) : read);
    }
    f.block = merge;
    const auto word = f.Emit(ValueOpcode::ShiftRightLogical32, {Value(&phi), shift});
    const auto key = f.Emit(ValueOpcode::BitwiseAnd32, {word, Value(255u)});
    const auto scaled = f.Emit(ValueOpcode::ShiftLeftLogical32, {key, Value(4u)});
    const auto table_offset = f.Emit(ValueOpcode::IAdd32, {scaled, Value(64u)});
    std::array<Value, 4> descriptor;
    for (uint32_t component = 0; component < descriptor.size(); ++component) {
      descriptor[component] = f.Emit(ValueOpcode::LoadAddressU32,
          {base, table_offset, Value(0u), Value(true)},
          f.AddMemory({.kind = ResourceKind::ScalarAddress, .offset = component * 4u,
                       .component_index = component, .component_count = 4u}, 0x2b4u));
    }
    f.Emit(ValueOpcode::StoreBufferU32,
           {f.Buffer(descriptor), lane, Value(0u), Value(0u), Value(42u), Value(true)},
           f.AddMemory({.kind = ResourceKind::Buffer, .offset = store_offset, .idxen = true}, 0x2c8u));
    if (byte_store)
      f.Emit(ValueOpcode::StoreBufferU32,
             {f.Buffer(descriptor), Value(0u), lane, Value(0u), Value(43u), Value(true)},
             f.AddMemory({.kind = ResourceKind::Buffer, .offen = true}, 0x2d0u));
    return fixture;
  };
  auto fixture = make();
  fixture->PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture->program);
  const auto& source = plan.descriptor_sources[plan.info.buffers[0].source];
  Check(source.indirect_descriptor && source.indirect_descriptor->indexed &&
            source.indirect_descriptor->selector->byte_ranges ==
                std::vector<std::pair<uint32_t, uint32_t>>{{0x1c0u, 1024u}},
        "packed scalar lookup lost a Phi arm or independent DWORD alignment");
  LinearTestMemory memory;
  memory.watched_address = memory.base + 0x1c0u;
  for (uint32_t key = 0; key < 19u; ++key) {
    const auto descriptor = 16u + key * 4u;
    memory.words[descriptor] = 0x100000u + key * 0x10000u;
    memory.words[descriptor + 1u] = 4u << 16u;
    memory.words[descriptor + 2u] = 14400u;
    memory.words[descriptor + 3u] = 0x5204u;
  }
  for (uint32_t i = 0; i < 256u; ++i) {
    for (uint32_t byte = 0; byte < 4u; ++byte)
      memory.words[0x1c0u / 4u + i] |= ((i * 4u + byte) % 19u) << (byte * 8u);
  }
  // No descriptor exists after key 18; broad enumeration would read this address.
  memory.fail_address = memory.base + 0x170u;
  const std::array<uint32_t, 2> user_data{static_cast<uint32_t>(memory.base), 0u};
  const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                          .read_specialization_memory = ReadLinearTestMemory};
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  const auto materialize = [&] { return MaterializeResources(plan, runtime, snapshot, specialization); };
  Check(materialize() && snapshot.buffers.size() == 19u && memory.reads == 20u &&
            memory.watched_reads == 1u && memory.watched_dwords == 256u,
        "packed scalar lookup did not batch its read and deduplicate writable candidates");
  Check(std::ranges::find(snapshot.specialization_reads,
                         std::pair{memory.base + 0x1c0u, uint64_t{1024u}}) != snapshot.specialization_reads.end() &&
            std::ranges::all_of(snapshot.buffers, [&](const DescriptorValue& value) {
              const auto key = (value.dwords[0] - 0x100000u) / 0x10000u;
              return std::ranges::find(snapshot.specialization_reads,
                         std::pair{memory.base + 64u + key * 16u, uint64_t{16u}}) != snapshot.specialization_reads.end();
            }),
        "packed lookup or writable descriptors escaped specialization overlap tracking");
  const auto initial = specialization;
  memory.words[16] += 4096u;
  memory.reads = memory.watched_reads = 0u;
  Check(materialize() && specialization == initial && memory.reads == 20u,
        "packed table refresh changed shader specialization or repeated scalar reads");
  memory.words[17] = 8u << 16u;
  Check(!materialize(), "indexed DWORD write accepted an incompatible descriptor stride");
  memory.words[17] = 4u << 16u;
  memory.words[19] |= 3u << 28u;
  Check(!materialize(), "indexed DWORD write accepted raw byte bounds");
  for (auto bad : {0u, 1u, 2u}) {
    auto invalid = make(bad == 0u ? 28u : 24u, bad == 1u, bad == 2u ? 4u : 0u);
    CheckFatal([&] { invalid->PlanAndTrack(); }, "not a valid runtime value",
               "packed selector accepted a cross-byte shift, leafless cycle, or indexed byte offset");
  }
  auto mixed = make(24u, false, 0u, true);
  mixed->PlanAndTrack();
  const auto& buffers = mixed->program.info.buffers;
  Check(buffers.size() == 2u && buffers[0].source != buffers[1].source &&
            mixed->program.descriptor_sources[buffers[0].source].indirect_descriptor->indexed &&
            !mixed->program.descriptor_sources[buffers[1].source].indirect_descriptor->indexed,
        "indexed and byte-addressed stores incorrectly shared their descriptor bounds contract");
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
        FakeGuestMemory fake(ReadLinearTestMemory, &memory);
        // Sampler selection compares a signed value against zero.
        for (const auto flag : {-1, 0, 1, INT32_MIN, INT32_MAX}) {
          memory.words[(0x1000 + 196) / 4] = std::bit_cast<uint32_t>(flag);
          const uint32_t first = (flag < 0) != reverse ? 480 / 4 : 448 / 4;
          memory.fail_address = 0x1000 + (first == 448 / 4 ? 480u : 448u);
          DescriptorValue selected;
          Check(EvaluateSrtDescriptor(plan, {}, source, selected),
                "conditional sampler did not survive detached plan lifetime");
          for (uint32_t word = 0; word < 4; ++word) {
            Check(selected.dwords[word] == memory.words[first + word],
                  "conditional sampler chose the wrong incoming descriptor");
          }
        }
        // An unreadable negative predicate reads as zero and selects the nonnegative edge.
        memory.words[(0x1000 + 196) / 4] = UINT32_MAX;
        memory.fail_address = 0x2000 + 196;
        DescriptorValue selected;
        Check(EvaluateSrtDescriptor(plan, {}, source, selected) &&
                  selected.dwords[0] == memory.words[reverse ? 480 / 4 : 448 / 4],
              "conditional sampler did not read unavailable predicate memory as zero");
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
  for (uint32_t candidate = 0; candidate < 2; ++candidate) {
    DescriptorValue descriptor;
    Check(EvaluateSrtDescriptor(plan, {.user_data = data}, finite->sources[candidate], descriptor),
          "finite image candidate is not host-readable");
    for (uint32_t word = 0; word < 8; ++word)
      Check(descriptor.dwords[word] == data[candidate * 8 + word],
            "finite image selection mixed descriptor DWORDs across predecessors");
  }
}

void TestPackedDescriptorWaterfall() {
  for (const uint32_t variant : {0u, 1u, 2u}) {
    Fixture f(ShaderType::Pixel);
    const auto packed = f.Emit(ValueOpcode::GetInterpolationParameter,
                               {Value(1u), Value(1u), Value(2u)});
    const auto active = f.Emit(ValueOpcode::ULessThan32,
                               {f.Emit(ValueOpcode::LaneId), Value(16u)});
    const auto image_key = f.Emit(ValueOpcode::BitwiseAnd32, {packed, Value(65535u)});
    const auto sampler_key = variant == 2u ? packed :
        f.Emit(ValueOpcode::ShiftRightLogical32, {packed, Value(16u)});
    auto selected_active = active;
    const auto descriptor = [&](Value key, uint32_t first, uint32_t count, uint32_t shift) {
      const auto selected = f.Emit(ValueOpcode::ReadFirstLane, {key, active});
      selected_active = f.Emit(ValueOpcode::LogicalAnd, {selected_active,
          f.Emit(ValueOpcode::IEqual32, {selected, key})});
      const auto offset = f.Emit(ValueOpcode::ShiftLeftLogical32, {selected, Value(shift)});
      const auto table = f.Buffer({f.UserData(first), f.UserData(first + 1u),
                                   f.UserData(first + 2u), f.UserData(first + 3u)});
      std::array<Value, 8> words;
      for (uint32_t word = 0; word < count; ++word)
        words[word] = f.Emit(ValueOpcode::ReadConstBuffer, {table, offset},
            f.AddMemory({.kind = ResourceKind::ScalarBuffer, .offset = word * 4u}, 0x58u));
      return words;
    };
    const auto image = f.Image(descriptor(image_key, 0u, 8u, 5u));
    const auto sampler_words = descriptor(sampler_key, 4u, 4u, 4u);
    const auto sampler = f.Sampler({sampler_words[0], sampler_words[1],
                                   sampler_words[2], sampler_words[3]});
    f.Emit(ValueOpcode::ImageSampleRaw,
        {image, sampler, f.ImageAddress(), variant == 1u ? Value(true) : selected_active},
        f.AddMemory({.kind = ResourceKind::Image,
                     .image_dimension = Decoder::ImageDimension::Dim2D}, 0x6cu));
    if (variant != 0u) {
      CheckFatal([&] { f.PlanAndTrack(); }, "not a valid runtime value",
                 "packed waterfall accepted widened EXEC or an unbounded sampler key");
      continue;
    }
    f.PlanAndTrack();
    const auto plan = ExtractResourcePlan(f.program);
    for (const auto source : {plan.info.images[0].source, plan.info.samplers[0].source}) {
      const auto& value = plan.descriptor_sources[source];
      const auto& table = value.indirect_descriptor;
      Check(table && !table->selector && table->table_scalar &&
                table->table_stride == value.dword_count * 4u &&
                (value.dword_count != 4u || table->table_maximum == 65535u),
            "packed waterfall lost its static sampler bit bound or captured the varying key");
    }
    LinearTestMemory memory;
    const std::array<uint32_t, 8> user_data{
        uint32_t(memory.base) + 256u, 32u << 16u, 2u, 0x5204u,
        uint32_t(memory.base) + 512u, 16u << 16u, 59u, 0x5204u};
    for (uint32_t row = 0; row < 2u; ++row) {
      const auto word = (256u + row * 32u) / 4u;
      memory.words[word] = 0x1000u + row * 0x1000u;
      memory.words[word + 1u] = uint32_t(Libs::Graphics::Prospero::BufferFormat::k32Float) << 20u;
      memory.words[word + 3u] = Libs::Graphics::DstSel(4, 0, 0, 1) | (9u << 28u);
    }
    for (uint32_t row = 0; row < 59u; ++row) {
      const auto word = (512u + row * 16u) / 4u;
      memory.words[word] = row;
      memory.words[word + 1u] = 0x00fff000u;
      memory.words[word + 2u] = (row & 1u) ? 0x0a500000u : 0u;
    }
    const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
        .read_specialization_memory = ReadLinearTestMemory};
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.images.size() == 3u && snapshot.samplers.size() == 60u,
          "packed waterfall ignored the two-image/59-sampler heap bounds or lost the null roots");
    Check(std::ranges::all_of(snapshot.specialization_reads, [&](const auto& span) {
      return (span.first >= memory.base + 256u && span.first + span.second <= memory.base + 320u) ||
          (span.first >= memory.base + 512u && span.first + span.second <= memory.base + 512u + 59u * 16u);
    }), "packed waterfall materialization read outside the descriptor heaps");
  }
}

void TestFiniteImageWaterfall() {
  namespace CFG = Libs::Graphics::ShaderRecompiler::CFG;
  enum class Variant { Valid, Staggered, Cycle, DifferentMask, MixedWord, DifferentSelect, WiderExec, BadLeaf };
  const auto make = [](Variant variant) {
    auto fixture = std::make_unique<Fixture>();
    auto& f = *fixture;
    const auto lane = f.Emit(ValueOpcode::LaneId);
    const auto active = f.Emit(ValueOpcode::ULessThan32, {lane, Value(16u)});
    const auto choose = f.Emit(ValueOpcode::IEqual32,
        {f.Emit(ValueOpcode::BitwiseAnd32, {lane, Value(1u)}), Value(0u)});
    const auto other = f.Emit(ValueOpcode::LogicalNot, {choose});
    const auto other_mask = f.Emit(ValueOpcode::LogicalNot, {active});
    std::array<Value, 4> table_words;
    for (uint32_t word = 0; word < 4u; ++word) table_words[word] = f.UserData(word);
    const auto table = f.Buffer(table_words);
    std::array<Value, 8> a, b, words;
    for (uint32_t word = 0; word < 8u; ++word)
      for (const bool second : {false, true})
        (second ? b : a)[word] = f.Emit(ValueOpcode::ReadConstBuffer, {table, Value(0u)},
            f.AddMemory({.kind = ResourceKind::ScalarBuffer,
                         .offset = (second ? 440u : 0u) + word * 4u}, 0x100u + word * 4u));
    if (variant == Variant::BadLeaf) a[3] = lane;
    if (variant == Variant::Staggered) {
      f.block->terminator.kind = CFG::TerminatorKind::Return;
      for (uint32_t word = 0; word < 8u; ++word) {
        const auto yes = word == 1u ? f.Emit(ValueOpcode::SelectU32, {other, a[word], b[word]}) : a[word];
        words[word] = f.Emit(ValueOpcode::SelectU32, {choose, yes, b[word]});
      }
    } else {
      auto* entry = f.block;
      auto* left = f.AddBlock();
      auto* right = f.AddBlock();
      auto* merge = f.AddBlock();
      entry->AddBranch(left);
      (variant == Variant::Cycle ? merge : entry)->AddBranch(right);
      left->AddBranch(merge);
      right->AddBranch(merge);
      entry->condition = variant == Variant::Cycle ? Value{} : choose;
      entry->terminator = variant == Variant::Cycle
          ? Block::Terminator{.kind = CFG::TerminatorKind::Branch, .true_block = left}
          : Block::Terminator{.kind = CFG::TerminatorKind::ConditionalBranch,
                       .true_block = left, .false_block = right};
      left->terminator = right->terminator = {.kind = CFG::TerminatorKind::Branch, .true_block = merge};
      merge->terminator = {.kind = variant == Variant::Cycle ? CFG::TerminatorKind::Branch
                                                            : CFG::TerminatorKind::Return,
                           .true_block = variant == Variant::Cycle ? right : nullptr};
      for (uint32_t word = 0; word < 8u; ++word) {
        const auto condition = variant == Variant::DifferentSelect && word == 7u ? other : choose;
        const auto yes = f.Emit(ValueOpcode::SelectU32, {condition, a[word], Value(0u)}, 0, left);
        auto& phi = merge->AppendNewInst(ValueOpcode::Phi, {}, uint64_t(Type::U32));
        const auto no = f.Emit(ValueOpcode::SelectU32,
            {choose, b[word], variant == Variant::Cycle ? Value(&phi) : Value(0u)}, 0, right);
        if (word & 1u) {
          phi.AddPhiOperand(right, no);
          phi.AddPhiOperand(left, yes);
        } else {
          phi.AddPhiOperand(left, yes);
          phi.AddPhiOperand(right, no);
        }
        words[word] = Value(&phi);
      }
      f.block = merge;
    }
    for (uint32_t word = 0; word < 8u; ++word) {
      if (variant == Variant::MixedWord && word == 7u) continue;
      words[word] = f.Emit(ValueOpcode::ReadFirstLane,
          {words[word], variant == Variant::DifferentMask && word == 7u ? other_mask : active});
    }
    const auto image = f.Image(words);
    const auto sampler = f.Sampler({Value(0u), Value(0u), Value(0u), Value(0u)});
    f.Emit(ValueOpcode::ImageSampleRaw,
        {image, sampler, f.ImageAddress(), variant == Variant::WiderExec ? Value(true) : active},
        f.AddMemory({.kind = ResourceKind::Image,
                     .image_dimension = Decoder::ImageDimension::Dim2D}, 0x66cu));
    return fixture;
  };
  for (const auto variant : {Variant::Valid, Variant::Staggered, Variant::Cycle}) {
    auto fixture = make(variant);
    fixture->PlanAndTrack();
    const auto plan = ExtractResourcePlan(fixture->program);
    EliminateDeadCode(fixture->program.blocks);
    ValidateProgram(fixture->program, true);
    const auto& finite = plan.descriptor_sources[plan.info.images[0].source].indirect_descriptor;
    Check(finite && finite->sources.size() == 3u,
          "waterfall image selection mixed descriptor words or lost a candidate");
    uint32_t lanes = 0u, selects = 0u;
    for (const auto* block : fixture->program.blocks) {
      std::vector<const Inst*> defined;
      for (const auto& inst : *block) {
        if (inst.GetOpcode() == ValueOpcode::ReadFirstLane || inst.GetOpcode() == ValueOpcode::SelectU32) {
          lanes += inst.GetOpcode() == ValueOpcode::ReadFirstLane;
          selects += inst.GetOpcode() == ValueOpcode::SelectU32;
          for (size_t arg = 0; arg < inst.NumArgs(); ++arg) {
            const auto* input = inst.Arg(arg).Resolve().TryInstruction();
            Check(input == nullptr || input->Parent() != block ||
                      std::ranges::find(defined, input) != defined.end(),
                  "projected descriptor choice uses an SSA value defined later in its block");
          }
        }
        defined.push_back(&inst);
      }
    }
    Check(lanes == 1u && selects == 2u,
          "waterfall descriptor selection did not share one ordinal graph across all eight words");
    if (variant != Variant::Valid) continue;
    LinearTestMemory memory;
    using F = Libs::Graphics::Prospero::BufferFormat;
    for (uint32_t row = 0; row < 2u; ++row) {
      const auto word = (256u + row * 440u) / 4u;
      memory.words[word] = 0x1000u + row * 0x1000u;
      memory.words[word + 1u] = uint32_t(F::k32Float) << 20u;
      memory.words[word + 3u] = Libs::Graphics::DstSel(4, 0, 0, 0) |
          (uint32_t(Libs::Graphics::Prospero::ImageType::kColor2D) << 28u);
    }
    std::array<uint32_t, 4> user_data{uint32_t(memory.base) + 256u, 8u << 16u, 60u, 0x5204u};
    SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
        .read_specialization_memory = ReadLinearTestMemory};
    FakeGuestMemory fake(ReadLinearTestMemory, &memory);
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization, populated;
    for (const uint32_t count : {0u, 60u, 0u, 60u}) {
      user_data[2] = count;
      memory.reads = 0u;
      Check(MaterializeResources(plan, runtime, snapshot, specialization),
            "finite waterfall resource plan could not refresh its scalar descriptor leaves");
      Check(snapshot.images.size() == (count == 0u ? 1u : 3u) &&
                (count == 0u ? memory.reads == 0u : memory.reads != 0u),
            "finite waterfall leaves froze empty table bounds or retained stale image candidates");
      if (count != 0u) populated = specialization;
    }
    memory.words[256u / 4u] += 0x100u;
    Check(MaterializeResources(plan, runtime, snapshot, specialization) && specialization == populated &&
              std::ranges::any_of(snapshot.images, [](const auto& image) { return image.dwords[0] == 0x1100u; }),
          "finite image payload refresh changed its format specialization or reused an old leaf");
  }
  for (const auto variant : {Variant::DifferentMask, Variant::MixedWord, Variant::DifferentSelect,
                             Variant::WiderExec, Variant::BadLeaf}) {
    auto fixture = make(variant);
    const auto choices = [&] {
      uint32_t count = 0u;
      for (const auto* block : fixture->program.blocks)
        for (const auto& inst : *block)
          count += inst.GetOpcode() == ValueOpcode::ReadFirstLane || inst.GetOpcode() == ValueOpcode::SelectU32;
      return count;
    };
    const auto before = choices();
    CheckFatal([&] { fixture->PlanAndTrack(); }, "not a valid runtime value",
               "finite waterfall accepted mismatched lane masks, choices or an unsupported leaf");
    Check(choices() <= before && !fixture->program.resource_tracking_complete &&
              fixture->program.descriptor_sources.empty(),
          "rejected finite descriptor graph left projected instructions or committed sources");
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
  RemoveIdentities(fixture.program);
  fixture.PlanAndTrack();
  auto plan = ExtractResourcePlan(fixture.program);
  const std::array<uint32_t, 1> user_data{0x4000u};
  DescriptorValue descriptor;
  Check(fixture.program.info.buffers.size() == 1u &&
            EvaluateSrtDescriptor(plan, {.user_data = user_data},
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
  Check(EvaluateSrtDescriptor(plan, runtime, fixture.program.info.buffers[0].source, descriptor) &&
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
    Check(EvaluateSrtDescriptor(plan, runtime, fixture.program.info.buffers[0].source, descriptor) &&
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
  FakeGuestMemory fake(ReadTestMemory, &memory);
  ResourceSnapshot snapshot;
  ResourceSpecialization specialization;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.buffers.size() == 2 &&
            snapshot.buffers[1].dword_count == 4 &&
            snapshot.buffers[1].dwords == std::array<uint32_t, 8>{},
        "untaken scalar branch materialized stale buffer words");
  Check(snapshot.user_data == std::vector<uint32_t>(user_data.begin(), user_data.end()),
        "resource reachability changed native shader user data");

  // The walker needs every user-data DWORD it may read, even for an untaken branch.
  runtime.user_data = std::span(user_data).first(4);
  Check(!MaterializeResources(plan, runtime, snapshot, specialization),
        "truncated user data was accepted");
  memory.words[0] = 1;

  runtime.user_data = user_data;
  const auto CheckActive = [&] {
    Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
              snapshot.buffers.size() == 2 &&
              std::equal(user_data.begin() + 4, user_data.end(),
                         snapshot.buffers[1].dwords.begin()),
          "potentially executed buffer descriptor was discarded");
  };
  CheckActive();
  // An unreadable control word reads as zero and skips the branch.
  memory.fail_after = memory.reads;
  Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
            snapshot.buffers.size() == 2 && snapshot.buffers[1].dwords == std::array<uint32_t, 8>{},
        "unreadable scalar control word did not read as zero");
  // Walker loads do not depend on the specialization reader.
  memory.fail_after = UINT32_MAX;
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
      const SrtRuntime runtime{.user_data = user_data, .userdata = &memory,
                               .read_specialization_memory = Read};
      FakeGuestMemory fake(Read, &memory);
      memory.data.words[0xa8 / 4] = 0x2000u;
      memory.data.words[0xb0 / 4] = 4u;
      ResourceSnapshot snapshot;
      ResourceSpecialization specialization;
      if (!shared) {
        Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.null_reads == 0 &&
                  std::ranges::all_of(snapshot.flattened_srt, [](auto word) { return word == 0; }),
              "disabled feature speculatively dereferenced its null BVH table");
        memory.data.words[7] = 1;
        Check(MaterializeResources(plan, runtime, snapshot, specialization) && memory.null_reads == 1 &&
                  std::ranges::all_of(snapshot.flattened_srt, [](auto word) { return word == 0; }),
              "active feature did not read its unreadable null BVH table as zero");
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
  FakeGuestMemory fake(ReadTestMemory, &memory);
  for (const auto use : {ConditionalBufferUse::Shared, ConditionalBufferUse::Loop,
                         ConditionalBufferUse::Writable}) {
    auto plan = ConditionalBufferPlan(use);
    ResourceSnapshot snapshot;
    ResourceSpecialization specialization;
    // With a zero control word, the loop body's only other predecessor is itself.
    for (const uint32_t flag : {0u, 1u}) {
      memory.words[0] = flag;
      std::array<uint32_t, 4> expected{};
      if (use == ConditionalBufferUse::Shared || flag != 0u)
        std::copy_n(user_data.begin() + 4, expected.size(), expected.begin());
      Check(MaterializeResources(plan, runtime, snapshot, specialization) &&
                snapshot.buffers.size() == 2 &&
                std::equal(expected.begin(), expected.end(),
                           snapshot.buffers[1].dwords.begin()),
            "shared or entered loop resource was pruned, or an untaken branch or loop retained");
    }
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
  const auto load = fixture.Emit(ValueOpcode::LoadBufferU32,
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

  Check(fixture.program.info.buffers.size() == 1u && fixture.program.info.buffers[0].live &&
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
  load.ResolveInstruction()->Invalidate();
  fixture.program.shader_info_complete = false;
  CollectShaderInfo(fixture.program, {.compute = &compute});
  Check(!fixture.program.info.buffers[0].live,
        "shader info recollection retained a removed buffer use");
}

void TestImageBindingAbi() {
  using NumericClass = Libs::Graphics::Prospero::TextureNumericClass;

  Check(ImageBindingCount == 50u &&
            static_cast<uint32_t>(DescriptorBindingKind::Buffers) == 0u &&
            static_cast<uint32_t>(DescriptorBindingKind::Samplers) == 51u &&
            static_cast<uint32_t>(DescriptorBindingKind::Gds) == 52u &&
            static_cast<uint32_t>(DescriptorBindingKind::BdaPagetable) == 53u &&
            static_cast<uint32_t>(DescriptorBindingKind::FaultBuffer) == 54u &&
            static_cast<uint32_t>(DescriptorBindingKind::FlattenedSrt) == 55u &&
            static_cast<uint32_t>(DescriptorBindingKind::ShaderData) == 56u &&
            static_cast<uint32_t>(DescriptorBindingKind::SharedMemory) == 57u &&
            static_cast<uint32_t>(DescriptorBindingKind::Count) == 58u,
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
    CheckBinding(ImageResourceClass::Storage, numeric_class,
                 Decoder::ImageDimension::Dim2DMsaa, false);
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
  image.dimension = Decoder::ImageDimension::Dim2DMsaaArray;
  Check(Invalid(image),
        "multisampled storage array received a descriptor binding");
  image.dimension = Decoder::ImageDimension::Dim2DMsaa;
  image.numeric_class = NumericClass::Uint;
  image.atomic = true;
  Check(Invalid(image), "multisampled atomic image received a descriptor binding");
  image.numeric_class = NumericClass::Float;
  image.dimension = Decoder::ImageDimension::Dim2D;
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
  for (uint32_t index = 0; index < ShaderInfo::MaxBufferRoots; index++) {
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
  Check(std::ranges::all_of(accepted.program.info.buffers,
                          [](const auto &buffer) { return buffer.live; }),
        "shader info truncated the 64 live buffers");

  Fixture fixture;
  MemoryInfo memory;
  memory.kind = ResourceKind::Buffer;
  for (uint32_t index = 0; index <= ShaderInfo::MaxBufferRoots; index++) {
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
    Run("independent image/sampler pairs", TestIndependentImageSamplerPairs);
    Run("SampleAdjust sampler scratch", TestSampleAdjustSamplerScratch);
    Run("FMASK load specialization", TestFmaskLoadSpecialization);
    Run("gather LOD sampler validation", TestGatherLodSamplerValidation);
    Run("dynamic storage mips", TestDynamicStorageMipTracking);
    Run("invariant indirect images", [] { TestInvariantIndirectImageMaterialization(true); });
    Run("unbounded material image keys", [] { TestInvariantIndirectImageMaterialization(false); });
    Run("resident image heap", TestResidentImageHeap);
    Run("image heap kinds", TestImageHeapKinds);
    Run("bounded image view eligibility", TestBoundedImageViewEligibility);
    Run("exact unfiltered image candidates", TestExactUnfilteredImageCandidates);
    Run("waterfall image table", TestWaterfallImageTable);
    Run("nested pointer image table", TestNestedPointerImageTable);
    Run("indexed pointer image table", TestIndexedPointerImageTable);
    Run("guarded direct image table", TestGuardedDirectImageTable);
    Run("bounded compute image loop", TestBoundedComputeImageLoop);
    Run("uniformized material image keys", TestUniformizedMaterialImageKeys);
    Run("indexed material image keys", TestIndexedMaterialImageKeys);
    Run("pointer-table indexed material keys", TestPointerTableIndexedMaterialKeys);
    Run("image descriptor fields", TestImageDescriptorFields);
    Run("draw-uniform scalar image", TestUniformScalarBufferImage);
    Run("compact scalar image table", TestCompactScalarImageTable);
    Run("scalar buffer selector domain", TestScalarBufferSelectorDomain);
    Run("private LDS forwarding", TestPrivateLdsForwarding);
    Run("counted row image selector", TestCountedRowImageSelector);
    Run("shared append selector bounds", TestSharedAppendSelectorBounds);
    Run("descending image range", TestDescendingImageRange);
    Run("SRT runtime", TestSrtFlatteningAndRuntimeMemoization);
    Run("scalar data control flow", TestScalarDataReadControlFlow);
    Run("dynamic SRT", TestDynamicSrtReadRemainsExplicit);
    Run("writable descriptor phi", TestWritableDescriptorPhi);
    Run("bounded formatted buffer store", TestBoundedFormattedBufferStore);
    Run("bounded buffer records", [] {
      for (const uint32_t components : {0u, 2u, 3u, 4u}) TestBoundedBufferRecords(components);
      TestGuardedWaterfallBufferRecords();
    });
    Run("GPU-selected buffer records", TestGpuSelectedBufferRecords);
    Run("workgroup-selected buffer records", TestWorkgroupSelectedBufferRecords);
    Run("GPU-selected formatted record store", TestGpuSelectedFormattedRecordStore);
    Run("finite buffer selection", TestFiniteBufferSelection);
    Run("packed scalar buffer selection", TestPackedScalarBufferSelection);
    Run("conditional sampler phi", TestConditionalSamplerPhi);
    Run("finite image phi cycle", TestFiniteImagePhiCycle);
    Run("packed descriptor waterfall", TestPackedDescriptorWaterfall);
    Run("finite image waterfall", TestFiniteImageWaterfall);
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
