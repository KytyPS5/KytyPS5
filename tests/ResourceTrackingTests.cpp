#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ResourceTracking.h"
#include "graphics/shader/recompiler/ScalarProvenance.h"
#include "graphics/shader/recompiler/ShaderInfoCollection.h"
#include "graphics/shader/recompiler/SrtPatcher.h"
#include "graphics/shader/recompiler/SrtWalker.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;
using Libs::Graphics::ShaderComputeInputInfo;
using Libs::Graphics::ShaderPixelInputInfo;
using Libs::Graphics::ShaderTextureResource;
using Libs::Graphics::ShaderType;
using Libs::Graphics::ShaderVertexInputInfo;
namespace Prospero = Libs::Graphics::Prospero;
namespace Decoder = Libs::Graphics::ShaderRecompiler::Decoder;

void Check(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

Operand Sgpr(uint32_t reg) {
  Operand operand;
  operand.kind = OperandKind::Register;
  operand.reg.file = RegisterFile::Scalar;
  operand.reg.index = reg;
  return operand;
}

Operand Imm(uint32_t value) {
  Operand operand;
  operand.kind = OperandKind::ImmediateU32;
  operand.imm = value;
  return operand;
}

Instruction Move(uint32_t pc, uint32_t dst, uint32_t src) {
  Instruction inst;
  inst.pc = pc;
  inst.op = Opcode::MoveU32;
  inst.dst = Sgpr(dst);
  inst.src[0] = Sgpr(src);
  inst.src_count = 1;
  return inst;
}

Instruction MoveImmediate(uint32_t pc, uint32_t dst, uint32_t value) {
  auto inst = Move(pc, dst, 0);
  inst.src[0] = Imm(value);
  return inst;
}

Instruction BufferUse(uint32_t pc, uint32_t base,
                      Opcode op = Opcode::BufferLoadDword) {
  Instruction inst;
  inst.pc = pc;
  inst.op = op;
  inst.memory.kind = ResourceKind::Buffer;
  inst.memory.resource = base / 4;
  return inst;
}

Instruction ImageUse(uint32_t pc, Opcode op, ResourceKind kind,
                     Decoder::ImageDimension dimension, uint32_t resource = 0,
                     uint32_t sampler = 2) {
  Instruction inst;
  inst.pc = pc;
  inst.op = op;
  inst.memory.kind = kind;
  inst.memory.resource = resource;
  inst.memory.sampler = sampler;
  inst.memory.image_dimension = dimension;
  return inst;
}

Instruction ScalarLoad(uint32_t pc, uint32_t dst, uint32_t base,
                       uint32_t offset) {
  Instruction inst;
  inst.pc = pc;
  inst.op = Opcode::SLoadDword;
  inst.dst = Sgpr(dst);
  inst.memory.kind = ResourceKind::ScalarBuffer;
  inst.memory.resource = base;
  inst.memory.offset = offset;
  return inst;
}

Instruction ScalarBufferLoad(uint32_t pc, uint32_t dst, uint32_t descriptor,
                             uint32_t offset) {
  auto inst = ScalarLoad(pc, dst, descriptor * 4, offset);
  inst.op = Opcode::SBufferLoadDword;
  inst.memory.resource = descriptor;
  return inst;
}

void MakeScalarMemoryGroup(std::vector<Instruction> *instructions) {
  const auto count = static_cast<uint32_t>(instructions->size());
  for (uint32_t i = 0; i < count; i++) {
    (*instructions)[i].memory.component_index = i;
    (*instructions)[i].memory.component_count = count;
  }
}

Instruction Export(uint32_t pc, ExportTargetKind kind, uint32_t index = 0,
                   uint32_t en = 0xf) {
  Instruction inst;
  inst.pc = pc;
  inst.op = Opcode::Export;
  inst.export_info.kind = kind;
  inst.export_info.index = index;
  inst.export_info.en = en;
  return inst;
}

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
  const auto index =
      static_cast<size_t>((address - memory->base) / sizeof(uint32_t));
  *value = memory->words[index];
  memory->reads++;
  return true;
}

const StageInput *FindInput(const ShaderInfo &info, StageInputKind kind,
                            uint32_t location = 0) {
  for (const auto &input : info.inputs) {
    if (input.kind == kind && input.location == location) {
      return &input;
    }
  }
  return nullptr;
}

const StageOutput *FindOutput(const ShaderInfo &info, StageOutputKind kind,
                              uint32_t index = 0) {
  for (const auto &output : info.outputs) {
    if (output.kind == kind && output.index == index) {
      return &output;
    }
  }
  return nullptr;
}

void Prepare(Program *program) {
  std::string error;
  if (!BuildScalarProvenance(program, &error) ||
      !BuildSrtPlan(program, &error) || !PatchSrtReads(program, &error) ||
      !TrackResources(program, &error)) {
    throw std::runtime_error(error);
  }
}

void TestDenseBufferPatching() {
  Program program;
  program.blocks.resize(1);
  auto first = BufferUse(4, 0);
  first.memory.offset = 4;
  first.memory.formatted = true;
  auto write = BufferUse(8, 0, Opcode::BufferStoreDword);
  write.memory.offset = 12;
  auto atomic = BufferUse(10, 0, Opcode::AtomicAddU32);
  auto &insts = program.blocks[0].instructions;
  insts = {first,           atomic,          write,           Move(12, 4, 20),
           Move(16, 5, 21), Move(20, 6, 22), Move(24, 7, 23), BufferUse(28, 4)};

  Prepare(&program);
  Check(program.info.buffers.size() == 2,
        "buffer sources were not densely deduplicated");
  const auto &resource = program.info.buffers[0];
  Check(resource.read && resource.written && resource.atomic &&
            resource.formatted && resource.max_byte_extent == 16 &&
            resource.first_use_pc == 4,
        "buffer access facts were not merged");
  Check(insts[0].memory.resource == 0 && insts[1].memory.resource == 0 &&
            insts[2].memory.resource == 0 && insts[7].memory.resource == 1,
        "buffer operands were not patched to dense indices");
  Check(insts[0].memory.resource_source == ScalarProvenance::Undefined &&
            insts[7].memory.resource_source == ScalarProvenance::Undefined,
        "patched instructions retained duplicate descriptor source handles");
  std::string error;
  Check(!TrackResources(&program, &error) &&
            error.find("already tracked") != std::string::npos,
        "resource tracking was not guarded against a second patch pass");
}

void TestScalarAndVectorBufferAlias() {
  Program program;
  program.blocks.resize(1);
  Instruction scalar;
  scalar.pc = 4;
  scalar.op = Opcode::SBufferLoadDword;
  scalar.dst = Sgpr(20);
  scalar.src[0] = Imm(0);
  scalar.src_count = 1;
  scalar.memory.kind = ResourceKind::ScalarBuffer;
  scalar.memory.resource = 2;
  program.blocks[0].instructions = {scalar, BufferUse(8, 8)};

  Prepare(&program);
  Check(program.info.buffers.size() == 1 && program.info.buffers[0].scalar,
        "scalar and vector uses of one descriptor were split");
  Check(program.blocks[0].instructions[0].memory.resource == 0 &&
            program.blocks[0].instructions[1].memory.resource == 0,
        "scalar/vector alias did not share one dense index");
}

void TestImagesAndSamplers() {
  Program program;
  program.blocks.resize(1);
  auto sample0 = ImageUse(4, Opcode::ImageSample, ResourceKind::Image,
                          Decoder::ImageDimension::Dim2D);
  auto sample1 = ImageUse(8, Opcode::ImageSample, ResourceKind::Image,
                          Decoder::ImageDimension::Dim2D);
  auto volume = ImageUse(12, Opcode::ImageSample, ResourceKind::Image,
                         Decoder::ImageDimension::Dim3D);
  auto compare = ImageUse(16, Opcode::ImageSample, ResourceKind::Image,
                          Decoder::ImageDimension::Dim2D);
  compare.memory.image_sample_flags = Decoder::ImageSampleFlagCompare;
  auto storage = ImageUse(20, Opcode::ImageStore, ResourceKind::StorageImage,
                          Decoder::ImageDimension::Dim2D);
  auto storage_mip = storage;
  storage_mip.pc = 24;
  storage_mip.memory.image_has_mip = true;
  auto atomic =
      ImageUse(28, Opcode::AtomicAddU32, ResourceKind::StorageImageUint,
               Decoder::ImageDimension::Dim2D);
  program.blocks[0].instructions = {sample0, sample1,     volume, compare,
                                    storage, storage_mip, atomic};

  Prepare(&program);
  Check(program.info.images.size() == 6 && program.info.samplers.size() == 1 &&
            program.info.sampled_pairs.size() == 3,
        "image view classes or samplers were deduplicated incorrectly");
  const auto &insts = program.blocks[0].instructions;
  Check(insts[0].memory.resource == 0 && insts[1].memory.resource == 0 &&
            insts[2].memory.resource == 1 && insts[3].memory.resource == 2 &&
            insts[4].memory.resource == 3 && insts[5].memory.resource == 4 &&
            insts[6].memory.resource == 5 && insts[0].memory.sampler == 0 &&
            insts[2].memory.sampler == 0,
        "image/sampler operands were not patched to dense list indices");
  Check(program.info.images[0].read && !program.info.images[0].written &&
            program.info.images[2].depth_compare &&
            program.info.images[3].written &&
            program.info.images[4].mip_mode == ImageMipMode::DynamicStorage &&
            program.info.images[5].atomic &&
            program.info.sampled_pairs[0].sampler ==
                program.info.sampled_pairs[1].sampler,
        "image access facts were wrong");
}

void TestDynamicPhiResource() {
  Program program;
  program.blocks.resize(4);
  program.blocks[0].successors = {1, 2};
  program.blocks[1].predecessors = {0};
  program.blocks[1].successors = {3};
  program.blocks[2].predecessors = {0};
  program.blocks[2].successors = {3};
  program.blocks[3].predecessors = {1, 2};
  Instruction left;
  left.op = Opcode::MoveU32;
  left.dst = Sgpr(0);
  left.src[0] = Imm(1);
  left.src_count = 1;
  Instruction right = left;
  right.pc = 4;
  right.src[0] = Imm(2);
  program.blocks[1].instructions = {left};
  program.blocks[2].instructions = {right};
  program.blocks[3].instructions = {BufferUse(8, 0)};

  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error),
        error.c_str());
  const auto original_source =
      program.blocks[3].instructions[0].memory.resource_source;
  Check(
      !TrackResources(&program, &error) &&
          error.find("unsupported GPU selection") != std::string::npos &&
          program.blocks[3].instructions[0].memory.resource_source ==
              original_source &&
          !program.resource_tracking_complete,
      "control-flow descriptor was patched without an executable GPU selector");
}

void TestTrackingRequiresCompletedSrtPlan() {
  Program program;
  program.stage = ShaderType::Compute;
  program.shader_hash = 0x8899;
  program.blocks.resize(4);
  program.blocks[0].successors = {1, 2};
  program.blocks[1].predecessors = {0};
  program.blocks[1].successors = {3};
  program.blocks[2].predecessors = {0};
  program.blocks[2].successors = {3};
  program.blocks[3].predecessors = {1, 2};
  program.blocks[1].instructions = {MoveImmediate(0, 0, 1)};
  program.blocks[2].instructions = {MoveImmediate(4, 0, 2)};
  program.blocks[3].instructions = {BufferUse(8, 0)};

  std::string error;
  Check(BuildScalarProvenance(&program, &error), error.c_str());
  Check(!program.srt_plan_complete,
        "provenance unexpectedly marked the SRT plan complete");
  const auto original_source =
      program.blocks[3].instructions[0].memory.resource_source;
  BufferResource existing_info;
  existing_info.source = 777;
  program.info.buffers.push_back(existing_info);
  Check(!TrackResources(&program, &error) &&
            error.find("SRT plan is not ready") != std::string::npos &&
            program.blocks[3].instructions[0].memory.resource == 0 &&
            program.blocks[3].instructions[0].memory.resource_source ==
                original_source &&
            program.info.buffers.size() == 1 &&
            program.info.buffers[0].source == 777 &&
            !program.resource_tracking_complete,
        "tracking bypassed the SRT-plan readiness invariant or partially "
        "patched the program");
}

void TestCyclicResourceIsRejected() {
  Program program;
  program.blocks.resize(2);
  program.blocks[0].successors = {1};
  program.blocks[1].predecessors = {0, 1};
  program.blocks[1].successors = {1};
  Instruction increment;
  increment.op = Opcode::IAddU32;
  increment.dst = Sgpr(0);
  increment.src[0] = Sgpr(0);
  increment.src[1] = Imm(1);
  increment.src_count = 2;
  program.blocks[1].instructions = {BufferUse(4, 0), increment};
  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error),
        error.c_str());
  Check(!TrackResources(&program, &error) &&
            error.find("unsupported GPU selection") != std::string::npos &&
            !program.resource_tracking_complete,
        "cyclic descriptor was patched without a bindless/direct path");
}

void TestUnknownSourceFailsWithoutPatching() {
  Program program;
  program.stage = ShaderType::Pixel;
  program.shader_hash = 0x12345678;
  program.blocks.resize(1);
  auto valid = BufferUse(4, 0);
  Instruction unsupported;
  unsupported.op = Opcode::SelectU32;
  unsupported.dst = Sgpr(4);
  unsupported.src[0] = Sgpr(20);
  unsupported.src_count = 1;
  program.blocks[0].instructions = {valid, unsupported, BufferUse(0x44, 4)};

  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error),
        error.c_str());
  BufferResource existing_info;
  existing_info.source = 777;
  program.info.buffers.push_back(existing_info);
  Check(!TrackResources(&program, &error),
        "unknown descriptor unexpectedly tracked");
  Check(error.find("hash=0x0000000012345678") != std::string::npos &&
            error.find("stage=pixel") != std::string::npos &&
            error.find("pc=0x00000044") != std::string::npos &&
            error.find("unknown value") != std::string::npos,
        "unknown descriptor error lost shader context");
  Check(!program.resource_tracking_complete &&
            program.info.buffers.size() == 1 &&
            program.info.buffers[0].source == 777 &&
            program.blocks[0].instructions[0].memory.resource == 0 &&
            program.blocks[0].instructions[0].memory.resource_source !=
                ScalarProvenance::Undefined &&
            program.blocks[0].instructions[2].memory.resource == 1 &&
            program.blocks[0].instructions[2].memory.resource_source !=
                ScalarProvenance::Undefined,
        "failed tracking partially patched the program");
}

void TestResourceLimitFailsTransactionally() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t i = 0; i <= ShaderInfo::MaxBuffers; i++) {
    insts.push_back(MoveImmediate(i * 8, 0, i));
    insts.push_back(BufferUse(i * 8 + 4, 0));
  }
  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error),
        error.c_str());
  const auto first_source = insts[1].memory.resource_source;
  Check(!TrackResources(&program, &error) &&
            error.find("buffer resource limit exceeded") != std::string::npos &&
            insts[1].memory.resource_source == first_source &&
            program.info.buffers.empty() && !program.resource_tracking_complete,
        "resource limit failure partially patched the program");
}

void TestComputeShaderInfoCollection() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  Instruction add_tid;
  add_tid.op = Opcode::DsReadAddtidB32;
  program.blocks[0].instructions = {add_tid, BufferUse(4, 0)};
  Prepare(&program);
  const auto buffers = program.info.buffers;
  const auto images = program.info.images;
  const auto samplers = program.info.samplers;
  const auto pairs = program.info.sampled_pairs;

  ShaderComputeInputInfo compute;
  compute.group_id[1] = true;
  compute.dispatch_thread_dimensions = true;
  std::string error;
  Check(CollectShaderInfo(&program, {.compute = &compute}, &error),
        error.c_str());
  Check(program.shader_info_complete && program.info.inputs.size() == 3 &&
            FindInput(program.info, StageInputKind::WorkgroupId) != nullptr &&
            FindInput(program.info, StageInputKind::LocalInvocationId) == nullptr &&
            FindInput(program.info, StageInputKind::LocalInvocationIndex) != nullptr &&
            FindInput(program.info, StageInputKind::GlobalInvocationId) != nullptr &&
            program.info.buffers == buffers && program.info.images == images &&
            program.info.samplers == samplers &&
            program.info.sampled_pairs == pairs,
        "compute addtid input discovery or resource preservation was wrong");

  Program metadata_program;
  metadata_program.stage = ShaderType::Compute;
  metadata_program.blocks.resize(1);
  Prepare(&metadata_program);
  compute = {};
  compute.thread_ids_num = 2;
  Check(CollectShaderInfo(&metadata_program, {.compute = &compute}, &error),
        error.c_str());
  Check(FindInput(metadata_program.info, StageInputKind::LocalInvocationId) !=
            nullptr &&
            FindInput(metadata_program.info,
                      StageInputKind::LocalInvocationIndex) != nullptr,
        "compute thread metadata did not request local invocation inputs");
}

void TestVertexShaderInfoCollection() {
  Program program;
  program.stage = ShaderType::Vertex;
  program.blocks.resize(1);
  Instruction attr0;
  attr0.op = Opcode::LoadInputF32;
  attr0.input_info.attr = 0;
  attr0.input_info.chan = 2;
  Instruction attr2 = attr0;
  attr2.input_info.attr = 2;
  attr2.input_info.chan = 0;
  program.blocks[0].instructions = {
      attr0, attr2, Export(8, ExportTargetKind::Position),
      Export(12, ExportTargetKind::Parameter, 2)};
  Prepare(&program);

  ShaderVertexInputInfo vertex;
  vertex.resources_num = 3;
  std::string error;
  Check(CollectShaderInfo(&program, {.vertex = &vertex}, &error),
        error.c_str());
  const auto *input0 = FindInput(program.info, StageInputKind::Parameter, 0);
  const auto *input2 = FindInput(program.info, StageInputKind::Parameter, 2);
  Check(program.info.inputs.size() == 4 && input0 != nullptr &&
            input0->component_count == 3 && input2 != nullptr &&
            input2->component_count == 1 &&
            FindInput(program.info, StageInputKind::VertexIndex) != nullptr &&
            FindInput(program.info, StageInputKind::InstanceIndex) != nullptr &&
            FindOutput(program.info, StageOutputKind::Position) != nullptr &&
            FindOutput(program.info, StageOutputKind::Parameter, 2) != nullptr,
        "vertex inputs or exports were not collected from lowered IR");

  const auto info = program.info;
  const auto resources_complete = program.resource_tracking_complete;
  const auto srt_complete = program.srt_plan_complete;
  const auto patching_complete = program.srt_patching_complete;
  Check(!CollectShaderInfo(&program, {.vertex = &vertex}, &error) &&
            error.find("already collected") != std::string::npos &&
            program.info == info && program.shader_info_complete &&
            program.resource_tracking_complete == resources_complete &&
            program.srt_plan_complete == srt_complete &&
            program.srt_patching_complete == patching_complete,
        "repeated shader info collection mutated the immutable interface");
}

void TestPixelShaderInfoCollection() {
  Program program;
  program.stage = ShaderType::Pixel;
  program.blocks.resize(1);
  program.blocks[0].instructions = {
      Export(4, ExportTargetKind::Mrt, 3),
      Export(8, ExportTargetKind::MrtZ, 0, 0x4),
      Export(12, ExportTargetKind::Mrt, 3),
      Export(16, ExportTargetKind::Mrt, 7, 0)};
  Prepare(&program);

  ShaderPixelInputInfo pixel;
  pixel.ps_pos_x = true;
  pixel.ps_front_face = true;
  pixel.input_num = 2;
  pixel.ps_depth_export_enable = true;
  pixel.ps_sample_mask_export_enable = true;
  std::string error;
  Check(CollectShaderInfo(&program, {.pixel = &pixel}, &error),
        error.c_str());
  Check(program.info.inputs.size() == 4 &&
            FindInput(program.info, StageInputKind::FragCoord) != nullptr &&
            FindInput(program.info, StageInputKind::FrontFacing) != nullptr &&
            FindInput(program.info, StageInputKind::Parameter, 0) != nullptr &&
            FindInput(program.info, StageInputKind::Parameter, 1) != nullptr &&
            program.info.outputs.size() == 2 &&
            FindOutput(program.info, StageOutputKind::Mrt, 3) != nullptr &&
            FindOutput(program.info, StageOutputKind::Mrt, 7) == nullptr &&
            FindOutput(program.info, StageOutputKind::Depth) == nullptr &&
            FindOutput(program.info, StageOutputKind::SampleMask) != nullptr,
        "pixel inputs, disabled exports, or sample-mask-only MRTZ collection was wrong");

  Program depth_program;
  depth_program.stage = ShaderType::Pixel;
  depth_program.blocks.resize(1);
  depth_program.blocks[0].instructions = {
      Export(4, ExportTargetKind::MrtZ, 0, 0x1)};
  Prepare(&depth_program);
  Check(CollectShaderInfo(&depth_program, {.pixel = &pixel}, &error),
        error.c_str());
  Check(depth_program.info.outputs.size() == 1 &&
            FindOutput(depth_program.info, StageOutputKind::Depth) != nullptr &&
            FindOutput(depth_program.info, StageOutputKind::SampleMask) == nullptr,
        "depth-only MRTZ export incorrectly enabled sample-mask output");
}

void TestShaderInfoCollectionIsTransactional() {
  Program program;
  program.stage = ShaderType::Compute;
  StageInput sentinel;
  sentinel.kind = StageInputKind::Parameter;
  sentinel.location = 9;
  program.info.inputs.push_back(sentinel);
  const auto info = program.info;
  std::string error;
  Check(!CollectShaderInfo(&program, {}, &error) &&
            error.find("not tracked") != std::string::npos &&
            program.info == info && !program.shader_info_complete,
        "pre-track shader info collection mutated program state");

  Prepare(&program);
  const auto tracked_info = program.info;
  const auto srt_complete = program.srt_plan_complete;
  const auto patching_complete = program.srt_patching_complete;
  Check(!CollectShaderInfo(&program, {}, &error) &&
            error.find("requires compute metadata") != std::string::npos &&
            program.info == tracked_info && !program.shader_info_complete &&
            program.resource_tracking_complete &&
            program.srt_plan_complete == srt_complete &&
            program.srt_patching_complete == patching_complete,
        "missing stage metadata committed incomplete shader info");
  program.stage = ShaderType::Unknown;
  Check(!CollectShaderInfo(&program, {}, &error) &&
            error.find("unsupported shader stage") != std::string::npos &&
            program.info == tracked_info && !program.shader_info_complete &&
            program.resource_tracking_complete &&
            program.srt_plan_complete == srt_complete &&
            program.srt_patching_complete == patching_complete,
        "unsupported-stage shader info collection was not transactional");
}

void TestShaderInfoMetadataValidation() {
  std::string error;

  Program vertex_program;
  vertex_program.stage = ShaderType::Vertex;
  vertex_program.blocks.resize(1);
  Instruction bad_input;
  bad_input.op = Opcode::LoadInputF32;
  bad_input.input_info.attr = 0;
  bad_input.input_info.chan = 4;
  vertex_program.blocks[0].instructions = {bad_input};
  Prepare(&vertex_program);
  ShaderVertexInputInfo vertex;
  vertex.resources_num = 1;
  const auto vertex_info = vertex_program.info;
  Check(!CollectShaderInfo(&vertex_program, {.vertex = &vertex}, &error) &&
            error.find("vertex input reference") != std::string::npos &&
            vertex_program.info == vertex_info &&
            !vertex_program.shader_info_complete,
        "out-of-range vertex channel produced immutable malformed info");
  vertex.resources_num = -1;
  Check(!CollectShaderInfo(&vertex_program, {.vertex = &vertex}, &error) &&
            error.find("vertex resource count") != std::string::npos &&
            vertex_program.info == vertex_info &&
            !vertex_program.shader_info_complete,
        "negative vertex resource count produced immutable malformed info");

  Program pixel_program;
  pixel_program.stage = ShaderType::Pixel;
  pixel_program.blocks.resize(1);
  Prepare(&pixel_program);
  ShaderPixelInputInfo pixel;
  pixel.input_num = 33;
  const auto pixel_info = pixel_program.info;
  Check(!CollectShaderInfo(&pixel_program, {.pixel = &pixel}, &error) &&
            error.find("pixel input count") != std::string::npos &&
            pixel_program.info == pixel_info &&
            !pixel_program.shader_info_complete,
        "out-of-range pixel input count produced immutable malformed info");

  Program compute_program;
  compute_program.stage = ShaderType::Compute;
  compute_program.blocks.resize(1);
  Prepare(&compute_program);
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 4;
  const auto compute_info = compute_program.info;
  Check(!CollectShaderInfo(&compute_program, {.compute = &compute}, &error) &&
            error.find("thread ID count") != std::string::npos &&
            compute_program.info == compute_info &&
            !compute_program.shader_info_complete,
        "out-of-range compute metadata produced immutable malformed info");
}

void TestTrackingRequiresSrtPatching() {
  Program program;
  program.blocks.resize(1);
  program.blocks[0].instructions = {BufferUse(4, 0)};
  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error),
        error.c_str());
  const auto source = program.blocks[0].instructions[0].memory.resource_source;
  Check(!TrackResources(&program, &error) &&
            error.find("SRT reads were not patched") != std::string::npos &&
            program.blocks[0].instructions[0].memory.resource_source == source &&
            !program.resource_tracking_complete,
        "resource tracking bypassed SRT patch completion");
  Check(PatchSrtReads(&program, &error) && TrackResources(&program, &error),
        error.c_str());
}

void TestDynamicSrtReadRemainsExplicit() {
  Program program;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t i = 0; i < 8; i++) {
    auto load = ScalarLoad(i * 4, i, 16, i * 4);
    load.src[0] = Sgpr(20);
    load.src_count = 1;
    insts.push_back(load);
  }
  insts.push_back(ImageUse(0x40, Opcode::ImageStore,
                           ResourceKind::StorageImage,
                           Decoder::ImageDimension::Dim2D));
  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error),
        error.c_str());
  Check(program.srt.reads.empty() && program.srt.dynamic_reads.size() == 8 &&
            program.srt_patching_complete,
        "dynamic SRT offsets were incorrectly assigned fixed flat slots");
  for (uint32_t i = 0; i < 8; i++) {
    Check(insts[i].op == Opcode::SLoadDword &&
              insts[i].memory.kind == ResourceKind::ScalarBuffer,
          "dynamic SRT read was rewritten as an immediate flat load");
  }
  Check(TrackResources(&program, &error), error.c_str());
}

void TestSrtPatchingFailureIsTransactional() {
  Program program;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t i = 0; i < 4; i++) {
    insts.push_back(ScalarLoad(i * 4, i, 16, i * 4));
  }
  insts.push_back(BufferUse(0x20, 0));
  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && program.srt.reads.size() == 4,
        error.c_str());
  insts[0].scalar_value = ScalarProvenance::Undefined;
  const auto instructions = insts;
  const auto provenance = program.provenance;
  const auto srt = program.srt;
  Check(!PatchSrtReads(&program, &error) &&
            error.find("no scalar-load producer") != std::string::npos &&
            insts == instructions && program.provenance == provenance &&
            program.srt == srt && !program.srt_patching_complete,
        "failed SRT patching partially changed the program");
}

void TestScalarMemoryGroupsSnapshotOperands() {
  const auto CheckGroup = [](bool buffer) {
    Program program;
    program.blocks.resize(1);
    auto &insts = program.blocks[0].instructions;
    for (uint32_t i = 0; i < 4; i++) {
      insts.push_back(buffer ? ScalarBufferLoad(4, 16 + i, 4, i * 4)
                             : ScalarLoad(4, 16 + i, 16, i * 4));
    }
    MakeScalarMemoryGroup(&insts);
    insts.push_back(BufferUse(8, 16));

    std::string error;
    Check(BuildScalarProvenance(&program, &error) &&
              BuildSrtPlan(&program, &error) && program.srt.reads.size() == 4 &&
              PatchSrtReads(&program, &error),
          error.c_str());
    for (uint32_t i = 0; i < 4; i++) {
      Check(insts[i].op == Opcode::LoadSrtDword && insts[i].src[0].imm == i,
            "overlapping scalar-memory group did not snapshot its operands");
    }
  };
  CheckGroup(false);
  CheckGroup(true);

  const auto CheckOffsetOverlap = [](bool buffer) {
    Program program;
    program.blocks.resize(1);
    auto &insts = program.blocks[0].instructions;
    for (uint32_t i = 0; i < 4; i++) {
      auto load = buffer ? ScalarBufferLoad(4, 20 + i, 4, i * 4)
                         : ScalarLoad(4, 20 + i, 16, i * 4);
      load.src[0] = Sgpr(20);
      load.src_count = 1;
      insts.push_back(load);
    }
    MakeScalarMemoryGroup(&insts);
    insts.push_back(BufferUse(8, 20));

    std::string error;
    Check(BuildScalarProvenance(&program, &error) &&
              BuildSrtPlan(&program, &error) &&
              program.srt.dynamic_reads.size() == 4 &&
              PatchSrtReads(&program, &error),
          error.c_str());
    for (uint32_t i = 0; i < 4; i++) {
      const auto value = insts[i].scalar_value;
      const auto &node = program.provenance.values[value];
      const auto offset_arg = buffer ? 4u : 2u;
      Check(program.provenance.values[node.args[offset_arg]].op ==
                    ScalarValueOp::UserData &&
                program.provenance.values[node.args[offset_arg]].imm == 20 &&
                insts[i].op ==
                    (buffer ? Opcode::SBufferLoadDword : Opcode::SLoadDword),
            "overlapping scalar-memory offset was read after a component write");
    }
  };
  CheckOffsetOverlap(false);
  CheckOffsetOverlap(true);
}

void TestSrtPatchingHandlesGvnAndMoveForwarding() {
  Program program;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t copy = 0; copy < 2; copy++) {
    for (uint32_t i = 0; i < 4; i++) {
      insts.push_back(ScalarLoad(copy * 0x20 + i * 4, copy * 4 + i, 16,
                                 i * 4));
    }
  }
  for (uint32_t i = 0; i < 4; i++) {
    insts.push_back(Move(0x40 + i * 4, 8 + i, i));
  }
  insts.push_back(BufferUse(0x60, 4));
  insts.push_back(BufferUse(0x64, 8));

  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && program.srt.reads.size() == 4 &&
            PatchSrtReads(&program, &error),
        error.c_str());
  for (uint32_t i = 0; i < 8; i++) {
    Check(insts[i].op == Opcode::LoadSrtDword &&
              insts[i].src[0].imm == i % 4,
          "all producers of a GVN'd SRT read were not patched");
  }
  for (uint32_t i = 8; i < 12; i++) {
    Check(insts[i].op == Opcode::MoveU32,
          "move forwarding was incorrectly rewritten as an SRT load");
  }
}

void TestSrtPatchingHandlesCfgProducers() {
  Program program;
  program.blocks.resize(4);
  program.blocks[0].successors = {1, 2};
  program.blocks[1].predecessors = {0};
  program.blocks[1].successors = {3};
  program.blocks[2].predecessors = {0};
  program.blocks[2].successors = {3};
  program.blocks[3].predecessors = {1, 2};
  for (uint32_t block = 1; block <= 2; block++) {
    for (uint32_t i = 0; i < 4; i++) {
      program.blocks[block].instructions.push_back(
          ScalarLoad(block * 0x20 + i * 4, i, 16, i * 4));
    }
  }
  program.blocks[3].instructions = {BufferUse(0x60, 0)};

  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && program.srt.reads.size() == 4 &&
            PatchSrtReads(&program, &error),
        error.c_str());
  for (uint32_t block = 1; block <= 2; block++) {
    for (uint32_t i = 0; i < 4; i++) {
      const auto &inst = program.blocks[block].instructions[i];
      Check(inst.op == Opcode::LoadSrtDword && inst.src[0].imm == i,
            "CFG-equivalent SRT producer was not patched");
    }
  }
}

void TestSrtPatchPlanValidation() {
  Program program;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t i = 0; i < 4; i++) {
    insts.push_back(ScalarLoad(i * 4, i, 16, i * 4));
  }
  insts.push_back(BufferUse(0x20, 0));
  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error),
        error.c_str());
  program.srt.reads = {program.srt.reads[0], program.srt.reads[0]};
  const auto instructions = insts;
  Check(!PatchSrtReads(&program, &error) &&
            error.find("dense value-to-offset bijection") != std::string::npos &&
            insts == instructions && !program.srt_patching_complete,
        "duplicate SRT flat slots were accepted or partially patched");

  Program dynamic;
  dynamic.blocks.resize(1);
  dynamic.blocks[0].instructions = {BufferUse(4, 0)};
  Check(BuildScalarProvenance(&dynamic, &error) &&
            BuildSrtPlan(&dynamic, &error),
        error.c_str());
  dynamic.srt.dynamic_sources = {ScalarProvenance::Undefined};
  Check(!PatchSrtReads(&dynamic, &error) &&
            error.find("invalid dynamic descriptor source") != std::string::npos &&
            !dynamic.srt_patching_complete,
        "undefined dynamic descriptor source was accepted");
}

void TestMaterializationSharesReadConstEvaluation() {
  Program program;
  program.stage = ShaderType::Pixel;
  program.shader_hash = 0x10203040;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t i = 0; i < 8; i++) {
    insts.push_back(ScalarLoad(i * 4, i, 16, i * 4));
  }
  for (uint32_t i = 0; i < 4; i++) {
    insts.push_back(MoveImmediate(0x20 + i * 4, 8 + i, 0xa0 + i));
  }
  insts.push_back(ImageUse(0x40, Opcode::ImageSample, ResourceKind::Image,
                           Decoder::ImageDimension::Dim2D));
  insts.push_back(ImageUse(0x44, Opcode::ImageStore,
                           ResourceKind::StorageImage,
                           Decoder::ImageDimension::Dim2D));
  Prepare(&program);
  Check(program.info.images.size() == 2 && program.info.samplers.size() == 1,
        "materialization test did not preserve sampled/storage view topology");
  Check(program.srt.reads.size() == 8 && program.srt_patching_complete,
        "immediate descriptor reads did not produce a compact SRT patch plan");
  for (uint32_t i = 0; i < 8; i++) {
    Check(insts[i].op == Opcode::LoadSrtDword && insts[i].src_count == 1 &&
              insts[i].src[0].kind == OperandKind::ImmediateU32 &&
              insts[i].src[0].imm == i &&
              insts[i].memory.kind == ResourceKind::None,
          "immediate SRT read was not patched to its dense flat-buffer slot");
  }
  std::string error;
  Check(!PatchSrtReads(&program, &error) &&
            error.find("already patched") != std::string::npos,
        "repeated SRT patching was accepted");

  TestMemory memory;
  for (uint32_t i = 0; i < memory.words.size(); i++) {
    memory.words[i] = 0x100 + i;
  }
  std::array<uint32_t, 32> user_data{};
  user_data[16] = static_cast<uint32_t>(memory.base);
  user_data[17] = static_cast<uint32_t>(memory.base >> 32u);
  SrtRuntime runtime{user_data, 0, ReadTestMemory, &memory};
  ResourceSnapshot snapshot;
  error.clear();
  Check(MaterializeResources(program, runtime, &snapshot, &error),
        error.c_str());
  Check(snapshot.buffers.empty() && snapshot.images.size() == 2 &&
            snapshot.samplers.size() == 1 &&
            snapshot.images[0] == snapshot.images[1],
        "dense runtime snapshot did not preserve resource order or aliases");
  Check(snapshot.flattened_srt.size() == 8 &&
            std::equal(snapshot.flattened_srt.begin(), snapshot.flattened_srt.end(),
                       memory.words.begin()) &&
            snapshot.user_data == std::vector<uint32_t>(user_data.begin(), user_data.end()),
        "runtime snapshot omitted flattened SRT or current user data");
  Check(memory.reads == 8,
        "aliased image views repeated ReadConst evaluation instead of sharing it");
  for (uint32_t i = 0; i < 8; i++) {
    Check(snapshot.images[0].dwords[i] == memory.words[i],
          "materialized image descriptor contains the wrong dword");
  }
}

void TestMaterializationFailureIsTransactional() {
  Program program;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t i = 0; i < 8; i++) {
    insts.push_back(ScalarLoad(i * 4, i, 16, i * 4));
  }
  insts.push_back(ImageUse(0x40, Opcode::ImageStore,
                           ResourceKind::StorageImage,
                           Decoder::ImageDimension::Dim2D));
  Prepare(&program);

  TestMemory memory;
  memory.fail_after = 3;
  std::array<uint32_t, 32> user_data{};
  user_data[16] = static_cast<uint32_t>(memory.base);
  user_data[17] = static_cast<uint32_t>(memory.base >> 32u);
  SrtRuntime runtime{user_data, 0, ReadTestMemory, &memory};
  ResourceSnapshot snapshot;
  DescriptorValue sentinel;
  sentinel.dword_count = 1;
  sentinel.dwords[0] = 777;
  snapshot.images.push_back(sentinel);
  std::string error;
  Check(!MaterializeResources(program, runtime, &snapshot, &error) &&
            error.find("failed at") != std::string::npos &&
            snapshot.images.size() == 1 &&
            snapshot.images[0].dwords[0] == 777 &&
            snapshot.images[0].dword_count == 1,
        "failed runtime materialization partially replaced the prior snapshot");
}

void TestResourceSpecializationIsTypedAndTransactional() {
  Program null_program;
  null_program.stage = ShaderType::Compute;
  null_program.blocks.resize(1);
  null_program.blocks[0].instructions = {
      ImageUse(0x10, Opcode::ImageLoad, ResourceKind::Image,
               Decoder::ImageDimension::Dim3D)};
  Prepare(&null_program);
  ResourceSnapshot null_snapshot;
  null_snapshot.images.resize(1);
  null_snapshot.images[0].dword_count = 8;
  null_snapshot.images[0].dwords[1] = 0x12345600u;
  null_snapshot.images[0].dwords[2] = 0x89abcdefu;
  null_snapshot.images[0].dwords[3] = 0x01234567u;
  std::string error;
  Check(SpecializeResources(&null_program, null_snapshot, &error) &&
            ValidateResourceSpecialization(null_program, null_snapshot, &error) &&
            null_program.info.images[0].kind == ResourceKind::Image &&
            null_program.info.images[0].dimension == Decoder::ImageDimension::Dim3D,
        "zero-base image descriptor did not preserve tracked null-image shape");

  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  program.blocks[0].instructions = {
      ImageUse(0x20, Opcode::ImageStore, ResourceKind::StorageImage,
               Decoder::ImageDimension::Dim3D)};
  Prepare(&program);

  ResourceSnapshot snapshot;
  snapshot.images.resize(1);
  snapshot.images[0].dword_count = 7;
  const auto info = program.info;
  const auto memory = program.blocks[0].instructions[0].memory;
  error.clear();
  Check(!ValidateResourceSnapshot(program, snapshot, &error) &&
            error.find("image descriptor 0 has 7 dwords") != std::string::npos,
        "malformed resource snapshot was accepted");
  Check(!SpecializeResources(&program, snapshot, &error) &&
            program.info == info &&
            program.blocks[0].instructions[0].memory == memory,
        "failed resource specialization partially mutated the program");

  snapshot.images[0].dword_count = 8;
  snapshot.images[0].dwords[0] = 0x1000;
  snapshot.images[0].dwords[1] =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) << 20u;
  snapshot.images[0].dwords[3] =
      (Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) << 28u) | 0x3acu;
  Check(SpecializeResources(&program, snapshot, &error), error.c_str());
  const auto &image = program.info.images[0];
  const auto &inst = program.blocks[0].instructions[0];
  Check(image.kind == ResourceKind::StorageImageUint &&
            image.dimension == Decoder::ImageDimension::Dim2D &&
            image.storage_swizzle == 0x3acu &&
            inst.memory.kind == ResourceKind::StorageImageUint &&
            inst.memory.image_dimension == Decoder::ImageDimension::Dim2D,
        "runtime descriptor shape and integer format did not specialize dense IR");

  auto stale_swizzle = snapshot;
  stale_swizzle.images[0].dwords[3] =
      (stale_swizzle.images[0].dwords[3] & ~0xfffu) |
      StorageImageIdentitySwizzle;
  Check(!ValidateResourceSpecialization(program, stale_swizzle, &error) &&
            error.find("changed swizzle") != std::string::npos,
        "storage image cache validation ignored a SPIR-V-baked swizzle");

  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  Check(CollectShaderInfo(&program, {.compute = &compute}, &error) &&
            AllocateBindings(&program, {}, &error),
        error.c_str());
  const auto *binding =
      FindBinding(program.bindings, DescriptorBindingKind::StorageUint2D);
  Check(binding != nullptr && binding->resources == std::vector<uint32_t>({0}) &&
            FindBinding(program.bindings, DescriptorBindingKind::Storage3D) == nullptr,
        "specialized image topology did not reach the exact native binding group");
}

void TestRuntimeSpecializationCoversBakedBufferAndAddressFields() {
  Program buffer_program;
  buffer_program.stage = ShaderType::Compute;
  buffer_program.blocks.resize(1);
  buffer_program.blocks[0].instructions = {BufferUse(0, 0)};
  Prepare(&buffer_program);

  ResourceSnapshot buffer_snapshot;
  buffer_snapshot.buffers.resize(1);
  buffer_snapshot.buffers[0].dword_count = 4;
  buffer_snapshot.buffers[0].dwords[1] = (16u << 16u) | (1u << 31u);
  buffer_snapshot.buffers[0].dwords[3] =
      (Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) << 12u) | (2u << 21u) |
      (1u << 23u);
  std::string error;
  Check(SpecializeResources(&buffer_program, buffer_snapshot, &error) &&
            ValidateResourceSpecialization(buffer_program, buffer_snapshot, &error),
        error.c_str());
  Check(buffer_program.info.buffers[0].packed_stride ==
                (16u | (1u << 14u) | (2u << 16u) | (1u << 20u)) &&
            buffer_program.info.buffers[0].descriptor_format ==
                Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt),
        "buffer specialization omitted SPIR-V-baked descriptor fields");
  auto stale_buffer = buffer_snapshot;
  stale_buffer.buffers[0].dwords[1] ^= 4u << 16u;
  Check(!ValidateResourceSpecialization(buffer_program, stale_buffer, &error) &&
            error.find("buffer descriptor 0") != std::string::npos,
        "stale buffer stride reused incompatible specialized SPIR-V");

  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  Check(CollectShaderInfo(&buffer_program, {.compute = &compute}, &error) &&
            AllocateBindings(&buffer_program, {}, &error),
        error.c_str());
  Check(buffer_program.bindings.user_data_registers ==
            std::vector<uint32_t>({0, 1, 2, 3}),
        "buffer fixture did not allocate its descriptor user-data roots");
  buffer_snapshot.user_data.resize(4);
  Check(ValidateResourceSpecialization(buffer_program, buffer_snapshot, &error),
        error.c_str());
  auto truncated_user_data = buffer_snapshot;
  truncated_user_data.user_data.resize(3);
  Check(!ValidateResourceSnapshot(buffer_program, truncated_user_data, &error) &&
            error.find("user SGPR 3") != std::string::npos,
        "truncated cache snapshot user-data window was accepted");

  Program based_program;
  based_program.stage = ShaderType::Compute;
  based_program.blocks.resize(1);
  auto raw = ScalarLoad(8, 0, 16, 0);
  raw.memory.offset = static_cast<uint32_t>(-4);
  raw.src[0] = Sgpr(20);
  raw.src_count = 1;
  based_program.blocks[0].instructions = {
      MoveImmediate(0, 16, 0x1000), MoveImmediate(4, 17, 0), raw};
  Prepare(&based_program);
  ResourceSnapshot based_snapshot;
  Check(MaterializeResources(based_program, {}, &based_snapshot, &error) &&
            SpecializeResources(&based_program, based_snapshot, &error),
        error.c_str());
  auto relocated = based_snapshot;
  relocated.addresses[0].guest_base += 0x1000;
  relocated.addresses[0].binding_base += 0x1000;
  Check(ValidateResourceSpecialization(based_program, relocated, &error),
        "relocated based address with identical relative bias changed specialization");
  relocated.addresses[0].binding_base++;
  Check(!ValidateResourceSpecialization(based_program, relocated, &error) &&
            error.find("address resource 0") != std::string::npos,
        "stale based-address bias reused incompatible specialized SPIR-V");

  Program flat_program;
  flat_program.stage = ShaderType::Compute;
  flat_program.blocks.resize(1);
  Instruction flat;
  flat.op = Opcode::FlatLoadDword;
  flat.memory.kind = ResourceKind::Flat;
  flat_program.blocks[0].instructions = {flat};
  Prepare(&flat_program);
  ResourceSnapshot flat_snapshot;
	flat_snapshot.addresses = {{0x11u, 0x10u}};
  flat_snapshot.user_data = {0xdeadbeefu};
  const auto prior_flat_snapshot = flat_snapshot;
  Check(!MaterializeResources(flat_program, {}, &flat_snapshot, &error) &&
            error.find("requires runtime guest-address translation") != std::string::npos &&
			flat_snapshot.addresses == prior_flat_snapshot.addresses &&
            flat_snapshot.user_data == prior_flat_snapshot.user_data,
        "unbased flat memory without a translator did not fail transactionally");
  SrtRuntime flat_runtime;
  flat_runtime.flat_memory_base = 0x100000000ull;
  Check(MaterializeResources(flat_program, flat_runtime, &flat_snapshot, &error) &&
            SpecializeResources(&flat_program, flat_snapshot, &error),
        error.c_str());
  auto stale_flat = flat_snapshot;
  stale_flat.addresses[0].guest_base += 0x1000;
  stale_flat.addresses[0].binding_base += 0x1000;
  Check(!ValidateResourceSpecialization(flat_program, stale_flat, &error),
        "unbased flat memory reused an absolute base baked into SPIR-V");
}

void TestTrackedProgramIsImmutable() {
  Program program;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  for (uint32_t i = 0; i < 8; i++) {
    insts.push_back(ScalarLoad(i * 4, i, 16, i * 4));
  }
  for (uint32_t i = 0; i < 4; i++) {
    insts.push_back(MoveImmediate(0x20 + i * 4, 8 + i, 0xa0 + i));
    insts.push_back(MoveImmediate(0x30 + i * 4, 12 + i, 0xb0 + i));
  }
  insts.push_back(ImageUse(0x50, Opcode::ImageSample, ResourceKind::Image,
                           Decoder::ImageDimension::Dim2D));
  insts.push_back(ImageUse(0x54, Opcode::ImageStore,
                           ResourceKind::StorageImage,
                           Decoder::ImageDimension::Dim2D));
  insts.push_back(BufferUse(0x58, 12));
  Prepare(&program);
  Check(program.info.buffers.size() == 1 && program.info.images.size() == 2 &&
            program.info.samplers.size() == 1 &&
            program.info.sampled_pairs.size() == 1,
        "post-track immutability fixture lacks complete resource topology");

  const auto provenance = program.provenance;
  const auto srt = program.srt;
  const auto info = program.info;
  const auto srt_complete = program.srt_plan_complete;
  const auto tracking_complete = program.resource_tracking_complete;
  const auto patching_complete = program.srt_patching_complete;
  std::vector<MemoryInfo> memory;
  memory.reserve(insts.size());
  for (const auto &inst : insts) {
    memory.push_back(inst.memory);
  }

  const auto CheckUnchanged = [&]() {
    Check(program.provenance == provenance && program.srt == srt &&
              program.info == info &&
              program.srt_plan_complete == srt_complete &&
              program.srt_patching_complete == patching_complete &&
              program.resource_tracking_complete == tracking_complete &&
              insts.size() == memory.size(),
          "rejected post-track pass mutated immutable program state");
    for (uint32_t i = 0; i < memory.size(); i++) {
      Check(insts[i].memory == memory[i],
            "rejected post-track pass mutated a dense operand or source handle");
    }
  };

  std::string error;
  Check(!BuildScalarProvenance(&program, &error) &&
            error.find("after resource tracking") != std::string::npos,
        "post-track provenance rebuild was accepted");
  CheckUnchanged();
  Check(!BuildSrtPlan(&program, &error) &&
            error.find("after resource tracking") != std::string::npos,
        "post-track SRT rebuild was accepted");
  CheckUnchanged();
}

void TestNativeBindingLayout() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  insts.push_back(BufferUse(4, 48));
  insts.push_back(ImageUse(8, Opcode::ImageSample, ResourceKind::Image,
                           Decoder::ImageDimension::Dim2D, 0, 2));
  insts.push_back(ImageUse(12, Opcode::ImageSample, ResourceKind::Image,
                           Decoder::ImageDimension::Dim2DArray, 0, 2));
  insts.push_back(ImageUse(16, Opcode::ImageStore,
                           ResourceKind::StorageImage,
                           Decoder::ImageDimension::Dim3D, 4));
  insts.push_back(ImageUse(20, Opcode::ImageStore,
                           ResourceKind::StorageImageUint,
                           Decoder::ImageDimension::Dim2DArray, 6));
  Prepare(&program);

  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  std::string error;
  Check(CollectShaderInfo(&program, {.compute = &compute}, &error) &&
            AllocateBindings(&program, {.descriptor_set = 3}, &error),
        error.c_str());
  const auto *buffers =
      FindBinding(program.bindings, DescriptorBindingKind::Buffers);
  const auto *sampled2d =
      FindBinding(program.bindings, DescriptorBindingKind::Sampled2D);
  const auto *sampled_array =
      FindBinding(program.bindings, DescriptorBindingKind::Sampled2DArray);
  const auto *storage3d =
      FindBinding(program.bindings, DescriptorBindingKind::Storage3D);
  const auto *storage_uint_array =
      FindBinding(program.bindings, DescriptorBindingKind::StorageUint2DArray);
  const auto *samplers =
      FindBinding(program.bindings, DescriptorBindingKind::Samplers);
  Check(program.bindings.descriptor_set == 3 && buffers != nullptr &&
            sampled2d != nullptr && sampled_array != nullptr &&
            storage3d != nullptr && storage_uint_array != nullptr &&
            samplers != nullptr && buffers->binding == 0 &&
            sampled2d->binding == 1 && sampled_array->binding == 2 &&
            storage3d->binding == 3 && storage_uint_array->binding == 4 &&
            samplers->binding == 5 && buffers->resources ==
                std::vector<uint32_t>{0} &&
            sampled2d->resources == std::vector<uint32_t>{0} &&
            sampled_array->resources == std::vector<uint32_t>{1} &&
            storage3d->resources == std::vector<uint32_t>{2} &&
            storage_uint_array->resources == std::vector<uint32_t>{3} &&
            samplers->resources == std::vector<uint32_t>{0} &&
            !program.bindings.user_data_registers.empty() &&
            program.bindings.push_constant_size != 0 &&
            FindBinding(program.bindings,
                        DescriptorBindingKind::FlattenedSrt) == nullptr &&
            program.binding_layout_complete,
        "native binding allocator did not preserve dense typed resource groups");
}

void TestNativeBindingLayoutSrtAndUserDataOverflow() {
  Program srt;
  srt.stage = ShaderType::Compute;
  srt.blocks.resize(1);
  for (uint32_t i = 0; i < 4; i++) {
    srt.blocks[0].instructions.push_back(
        ScalarLoad(i * 4, i, 16, i * 4));
  }
  srt.blocks[0].instructions.push_back(BufferUse(0x20, 0));
  Prepare(&srt);
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  std::string error;
  Check(CollectShaderInfo(&srt, {.compute = &compute}, &error) &&
            AllocateBindings(&srt, {}, &error),
        error.c_str());
  const auto *flat =
      FindBinding(srt.bindings, DescriptorBindingKind::FlattenedSrt);
  Check(flat != nullptr && flat->binding == 1 && flat->resources.empty(),
        "flattened SRT did not receive one native backend binding");

  Program overflow;
  overflow.stage = ShaderType::Compute;
  overflow.blocks.resize(1);
  for (uint32_t i = 0; i < 33; i++) {
    Instruction direct;
    direct.pc = i * 4;
    direct.op = Opcode::MoveU32;
    direct.dst.kind = OperandKind::Register;
    direct.dst.reg = {RegisterFile::Vector, i};
    direct.src[0] = Sgpr(i);
    direct.src_count = 1;
    overflow.blocks[0].instructions.push_back(direct);
  }
  Check(BuildScalarProvenance(&overflow, &error) &&
            BuildSrtPlan(&overflow, &error) && PatchSrtReads(&overflow, &error) &&
            TrackResources(&overflow, &error) &&
            CollectShaderInfo(&overflow, {.compute = &compute}, &error) &&
            AllocateBindings(&overflow, {}, &error),
        error.c_str());
  const auto *user_data =
      FindBinding(overflow.bindings, DescriptorBindingKind::UserData);
  Check(overflow.bindings.user_data_registers.size() == 33 &&
            overflow.bindings.user_data_registers.front() == 0 &&
            overflow.bindings.user_data_registers.back() == 32 &&
            overflow.bindings.push_constant_size == 0 && user_data != nullptr &&
            user_data->binding == 0,
        "oversized sparse user data was not moved to a descriptor binding");
}

void TestNativeBindingLayoutGds() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  Instruction append;
  append.op = Opcode::DsAppend;
	append.memory.kind = ResourceKind::Gds;
  program.blocks[0].instructions.push_back(append);

  ShaderComputeInputInfo compute;
  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error) &&
            TrackResources(&program, &error) &&
            CollectShaderInfo(&program, {.compute = &compute}, &error) &&
            AllocateBindings(&program, {}, &error),
        error.c_str());
  const auto *gds = FindBinding(program.bindings, DescriptorBindingKind::Gds);
  Check(gds != nullptr && gds->binding == 0 && gds->resources.empty(),
        "GDS append/consume did not allocate one native storage binding");

	Program lds;
	lds.stage = ShaderType::Compute;
	lds.blocks.resize(1);
	append.memory.kind = ResourceKind::Lds;
	lds.blocks[0].instructions.push_back(append);
	Check(BuildScalarProvenance(&lds, &error) && BuildSrtPlan(&lds, &error) &&
	          PatchSrtReads(&lds, &error) && TrackResources(&lds, &error) &&
	          CollectShaderInfo(&lds, {.compute = &compute}, &error) &&
	          AllocateBindings(&lds, {}, &error),
	      error.c_str());
	Check(FindBinding(lds.bindings, DescriptorBindingKind::Gds) == nullptr,
	      "LDS append/consume incorrectly allocated a GDS binding");
}

void TestNativeBindingLayoutIsTransactional() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  std::string error;
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error) &&
            TrackResources(&program, &error) &&
            CollectShaderInfo(&program, {.compute = &compute}, &error),
        error.c_str());
  BindingLayout sentinel;
  sentinel.descriptor_set = 99;
  program.bindings = sentinel;
  Check(!AllocateBindings(&program, {.push_constant_offset = 129}, &error) &&
            error.find("Vulkan minimum") != std::string::npos &&
            program.bindings == sentinel && !program.binding_layout_complete,
        "failed native binding allocation partially changed the program");
  Check(!AllocateBindings(&program, {.push_constant_offset = 2}, &error) &&
            error.find("dword aligned") != std::string::npos &&
            program.bindings == sentinel && !program.binding_layout_complete,
        "misaligned push-constant allocation partially changed the program");
  Check(AllocateBindings(&program, {}, &error), error.c_str());
  const auto layout = program.bindings;
  Check(!AllocateBindings(&program, {}, &error) &&
            error.find("already allocated") != std::string::npos &&
            program.bindings == layout,
        "repeated native binding allocation was accepted or mutated layout");

  Program tail;
  tail.stage = ShaderType::Compute;
  tail.blocks.resize(1);
  Instruction direct;
  direct.op = Opcode::MoveU32;
  direct.dst.kind = OperandKind::Register;
  direct.dst.reg = {RegisterFile::Vector, 0};
  direct.src[0] = Sgpr(0);
  direct.src_count = 1;
  tail.blocks[0].instructions = {direct};
  Check(BuildScalarProvenance(&tail, &error) && BuildSrtPlan(&tail, &error) &&
            PatchSrtReads(&tail, &error) && TrackResources(&tail, &error) &&
            CollectShaderInfo(&tail, {.compute = &compute}, &error) &&
            AllocateBindings(&tail, {.push_constant_offset = 124}, &error) &&
            tail.bindings.push_constant_size == 4 &&
            FindBinding(tail.bindings, DescriptorBindingKind::UserData) == nullptr,
        "valid final dword of the guaranteed push-constant range was rejected");
}

void TestNativeBindingLayoutTracksReachingUserData() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  auto &insts = program.blocks[0].instructions;
  insts.push_back(Move(0, 100, 4));
  Instruction forwarded;
  forwarded.pc = 4;
  forwarded.op = Opcode::MoveU32;
  forwarded.dst.kind = OperandKind::Register;
  forwarded.dst.reg = {RegisterFile::Vector, 0};
  forwarded.src[0] = Sgpr(100);
  forwarded.src_count = 1;
  insts.push_back(forwarded);
  auto sparse = forwarded;
  sparse.pc = 8;
  sparse.dst.reg.index = 1;
  sparse.src[0] = Sgpr(20);
  insts.push_back(sparse);
  insts.push_back(MoveImmediate(12, 101, 7));
  auto constant = forwarded;
  constant.pc = 16;
  constant.dst.reg.index = 2;
  constant.src[0] = Sgpr(101);
  insts.push_back(constant);

  std::string error;
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error) &&
            TrackResources(&program, &error) &&
            CollectShaderInfo(&program, {.compute = &compute}, &error),
        error.c_str());
  auto fallback = program;
  Check(AllocateBindings(&program, {}, &error) &&
            program.bindings.user_data_registers ==
                std::vector<uint32_t>({4, 20}) &&
            program.bindings.push_constant_size == 8 &&
            FindBinding(program.bindings, DescriptorBindingKind::UserData) ==
                nullptr,
        "native user-data map included a temporary or missed sparse roots");
  Check(AllocateBindings(&fallback, {.max_push_dwords = 1}, &error) &&
            fallback.bindings.user_data_registers ==
                std::vector<uint32_t>({4, 20}) &&
            fallback.bindings.push_constant_size == 0 &&
            FindBinding(fallback.bindings, DescriptorBindingKind::UserData) !=
                nullptr,
        "max push-dword limit did not move exact sparse user data to storage");
}

void TestNativeBindingLayoutRejectsUnknownShapeAndBadProvenance() {
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  std::string error;

  Program image;
  image.stage = ShaderType::Compute;
  image.blocks.resize(1);
  image.blocks[0].instructions = {
      ImageUse(4, Opcode::ImageStore, ResourceKind::StorageImage,
               Decoder::ImageDimension::Dim2D)};
  Prepare(&image);
  Check(CollectShaderInfo(&image, {.compute = &compute}, &error),
        error.c_str());
  image.info.images[0].dimension = Decoder::ImageDimension::Unknown;
  image.bindings.descriptor_set = 77;
  const auto image_layout = image.bindings;
  Check(!AllocateBindings(&image, {}, &error) &&
            error.find("invalid image binding class") != std::string::npos &&
            image.bindings == image_layout && !image.binding_layout_complete,
        "unknown image shape was defaulted or partially allocated");

  Program provenance;
  provenance.stage = ShaderType::Compute;
  provenance.blocks.resize(1);
  Instruction direct;
  direct.op = Opcode::MoveU32;
  direct.dst.kind = OperandKind::Register;
  direct.dst.reg = {RegisterFile::Vector, 0};
  direct.src[0] = Sgpr(4);
  direct.src_count = 1;
  provenance.blocks[0].instructions = {direct};
  Check(BuildScalarProvenance(&provenance, &error) &&
            BuildSrtPlan(&provenance, &error) &&
            PatchSrtReads(&provenance, &error) &&
            TrackResources(&provenance, &error) &&
            CollectShaderInfo(&provenance, {.compute = &compute}, &error),
        error.c_str());
  provenance.blocks[0].instructions[0].scalar_sources[0] =
      static_cast<uint32_t>(provenance.provenance.values.size() + 1);
  provenance.bindings.descriptor_set = 88;
  const auto provenance_layout = provenance.bindings;
  Check(!AllocateBindings(&provenance, {}, &error) &&
            error.find("invalid scalar provenance reference") !=
                std::string::npos &&
            provenance.bindings == provenance_layout &&
            !provenance.binding_layout_complete,
        "malformed per-use provenance was ignored or partially allocated");
}

void TestNativeBindingLayoutDynamicSrtDoesNotUseFlatBinding() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  for (uint32_t i = 0; i < 4; i++) {
    auto load = ScalarLoad(i * 4, i, 16, i * 4);
    load.src[0] = Sgpr(20);
    load.src_count = 1;
    program.blocks[0].instructions.push_back(load);
  }
  program.blocks[0].instructions.push_back(BufferUse(0x20, 0));
  Prepare(&program);
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  std::string error;
  Check(program.srt.reads.empty() && program.srt.dynamic_reads.size() == 4 &&
            CollectShaderInfo(&program, {.compute = &compute}, &error) &&
            AllocateBindings(&program, {}, &error),
        error.c_str());
  Check(program.bindings.user_data_registers ==
                std::vector<uint32_t>({16, 17, 20}) &&
            FindBinding(program.bindings,
                        DescriptorBindingKind::FlattenedSrt) == nullptr,
        "dynamic-only SRT reads were flattened or lost reaching user data");
}

void TestNativeBindingLayoutTracksRawScalarMemoryBase() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  auto load = ScalarLoad(0, 0, 16, 0);
  load.src[0] = Sgpr(20);
  load.src_count = 1;
  Instruction consume;
  consume.pc = 4;
  consume.op = Opcode::MoveU32;
  consume.dst.kind = OperandKind::Register;
  consume.dst.reg = {RegisterFile::Vector, 0};
  consume.src[0] = Sgpr(0);
  consume.src_count = 1;
  program.blocks[0].instructions = {load, consume};

  std::string error;
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  Check(BuildScalarProvenance(&program, &error) &&
            BuildSrtPlan(&program, &error) && program.srt.reads.empty() &&
            program.srt.dynamic_reads.empty() && PatchSrtReads(&program, &error) &&
            TrackResources(&program, &error) &&
            CollectShaderInfo(&program, {.compute = &compute}, &error) &&
            AllocateBindings(&program, {}, &error),
        error.c_str());
  const auto *address_memory =
      FindBinding(program.bindings, DescriptorBindingKind::AddressMemory);
  Check(program.blocks[0].instructions[0].op == Opcode::SLoadDword &&
            program.bindings.user_data_registers ==
                std::vector<uint32_t>({16, 17, 20}) &&
            FindBinding(program.bindings,
                        DescriptorBindingKind::FlattenedSrt) == nullptr &&
            address_memory != nullptr &&
            address_memory->resources == std::vector<uint32_t>({0}) &&
            program.info.addresses.size() == 1 &&
            program.info.addresses[0].kind == ResourceKind::ScalarBuffer,
        "ordinary raw scalar memory omitted its implicit base or offset roots");
}

void TestRawScalarMemoryTracksReachingBaseIdentity() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  auto first = ScalarLoad(8, 0, 16, 0);
  first.memory.offset = static_cast<uint32_t>(-4);
  first.src[0] = Sgpr(20);
  first.src_count = 1;
  auto second = ScalarLoad(20, 1, 16, 0);
  second.src[0] = Sgpr(20);
  second.src_count = 1;
  program.blocks[0].instructions = {
      MoveImmediate(0, 16, 0x1000), MoveImmediate(4, 17, 0), first,
      MoveImmediate(12, 16, 0x2000), MoveImmediate(16, 17, 0), second};

  std::string error;
  Check(BuildScalarProvenance(&program, &error) && BuildSrtPlan(&program, &error) &&
            PatchSrtReads(&program, &error) && TrackResources(&program, &error),
        error.c_str());
  Check(program.info.addresses.size() == 2 &&
            program.info.addresses[0].source != program.info.addresses[1].source &&
            program.blocks[0].instructions[2].memory.resource == 0 &&
            program.blocks[0].instructions[5].memory.resource == 1,
        "raw scalar loads with redefined SBASE collapsed to one address resource");

  std::array<uint32_t, 64> user_data{};
  user_data[20] = 4;
  ResourceSnapshot snapshot;
  Check(MaterializeResources(program, {user_data}, &snapshot, &error), error.c_str());
  Check(snapshot.addresses.size() == 2 &&
            snapshot.addresses[0].guest_base == 0x1000 &&
            snapshot.addresses[0].binding_base == 0x0ffc &&
            snapshot.addresses[1].guest_base == 0x2000 &&
            snapshot.addresses[1].binding_base == 0x2000,
        "runtime snapshot lost per-use raw scalar base values");
}

void TestNativeBindingLayoutUsesExplicitFlatMemory() {
  Program program;
  program.stage = ShaderType::Compute;
  program.blocks.resize(1);
  Instruction load;
  load.op = Opcode::FlatLoadDword;
  load.memory.kind = ResourceKind::Flat;
  program.blocks[0].instructions = {load};
  Prepare(&program);

  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  std::string error;
  Check(CollectShaderInfo(&program, {.compute = &compute}, &error) &&
            AllocateBindings(&program, {}, &error),
        error.c_str());
  const auto *flat =
      FindBinding(program.bindings, DescriptorBindingKind::AddressMemory);
  Check(flat != nullptr && flat->resources == std::vector<uint32_t>({0}) &&
            program.info.addresses.size() == 1 &&
            program.info.addresses[0].kind == ResourceKind::Flat &&
            FindBinding(program.bindings, DescriptorBindingKind::Buffers) == nullptr,
        "flat address space was aliased to an ordinary buffer descriptor group");

  ResourceSnapshot snapshot;
  SrtRuntime runtime;
  runtime.flat_memory_base = 0x1234567887654000ull;
  Check(MaterializeResources(program, runtime, &snapshot, &error) &&
            snapshot.addresses.size() == 1 &&
            snapshot.addresses[0].guest_base == 0x1234567887654000ull &&
            snapshot.addresses[0].binding_base == 0x1234567887654000ull,
        "unbased flat virtual address space lost its runtime binding base");
}

void TestNativeBindingLayoutHonorsUserDataCount() {
  Program program;
  program.stage = ShaderType::Compute;
  program.user_data_count = 8;
  program.blocks.resize(1);
  for (uint32_t i = 0; i < 2; i++) {
    Instruction direct;
    direct.pc = i * 4;
    direct.op = Opcode::MoveU32;
    direct.dst.kind = OperandKind::Register;
    direct.dst.reg = {RegisterFile::Vector, i};
    direct.src[0] = Sgpr(7 + i);
    direct.src_count = 1;
    program.blocks[0].instructions.push_back(direct);
  }
  std::string error;
  ShaderComputeInputInfo compute;
  compute.thread_ids_num = 1;
  Check(BuildScalarProvenance(&program, &error) &&
            program.provenance.values[program.blocks[0].instructions[0]
                                          .scalar_sources[0]]
                    .op == ScalarValueOp::UserData &&
            program.blocks[0].instructions[1].scalar_sources[0] ==
                ScalarProvenance::Unknown &&
            BuildSrtPlan(&program, &error) && PatchSrtReads(&program, &error) &&
            TrackResources(&program, &error) &&
            CollectShaderInfo(&program, {.compute = &compute}, &error) &&
            AllocateBindings(&program, {}, &error) &&
            program.bindings.user_data_registers == std::vector<uint32_t>({7}),
        "stage user-data count did not bound provenance roots");

  Program invalid;
  invalid.user_data_count = 65;
  invalid.blocks.resize(1);
  Check(!BuildScalarProvenance(&invalid, &error) &&
            error.find("exceeds 64") != std::string::npos,
        "out-of-range stage user-data count was accepted");
}

void TestNativeBindingLayoutHonorsUserDataBase() {
  Program program;
  program.stage = ShaderType::Vertex;
  program.user_data_base = 8;
  program.user_data_count = 8;
  program.blocks.resize(1);
  program.blocks[0].instructions = {
      Move(0, 20, 7), Move(4, 21, 8), Move(8, 22, 15), Move(12, 23, 16),
      BufferUse(16, 8)};

  std::string error;
  Check(BuildScalarProvenance(&program, &error) &&
            program.blocks[0].instructions[0].scalar_sources[0] ==
                ScalarProvenance::Unknown &&
            program.provenance.values[
                program.blocks[0].instructions[1].scalar_sources[0]].op ==
                ScalarValueOp::UserData &&
            program.blocks[0].instructions[3].scalar_sources[0] ==
                ScalarProvenance::Unknown &&
            BuildSrtPlan(&program, &error),
        error.c_str());

  const std::array<uint32_t, 8> user_data = {
      0x11111111, 0x22222222, 0x33333333, 0x44444444,
      0, 0, 0, 0xaaaaaaaa};
  DescriptorValue descriptor;
  const auto source = program.blocks[0].instructions.back().memory.resource_source;
  Check(EvaluateDescriptorSource(program, source, 16, {user_data}, &descriptor, &error) &&
            descriptor.dwords[0] == user_data[0] &&
            descriptor.dwords[3] == user_data[3],
        "vertex user-data base was not translated to runtime-local indices");

  ShaderVertexInputInfo vertex;
  Check(PatchSrtReads(&program, &error) && TrackResources(&program, &error) &&
            CollectShaderInfo(&program, {.vertex = &vertex}, &error) &&
            AllocateBindings(&program, {}, &error),
        error.c_str());
  Check(program.bindings.user_data_registers ==
            std::vector<uint32_t>({8, 9, 10, 11, 15}),
        "native binding plan did not retain physical shifted user-SGPR indices");
}

void TestTextureNullDescriptorUsesAddressBits() {
  ShaderTextureResource texture;
  const uint32_t captured[8] = {0x00000000, 0xc3800000, 0x0059c09f, 0x91b00fac,
                                0x00000000, 0x00700000, 0x00000000, 0x00000000};
  std::copy(std::begin(captured), std::end(captured), std::begin(texture.fields));
  Check(texture.IsNull(),
        "zero-address image with populated metadata was not classified as null");
  texture.fields[0] = 1;
  Check(!texture.IsNull(), "nonzero image base address was classified as null");
}

} // namespace

int main() {
  const char* current = "startup";
#define RUN(test) current = #test; test()
  try {
    RUN(TestDenseBufferPatching);
    RUN(TestScalarAndVectorBufferAlias);
    RUN(TestImagesAndSamplers);
    RUN(TestDynamicPhiResource);
    RUN(TestTrackingRequiresCompletedSrtPlan);
    RUN(TestCyclicResourceIsRejected);
    RUN(TestUnknownSourceFailsWithoutPatching);
    RUN(TestResourceLimitFailsTransactionally);
    RUN(TestComputeShaderInfoCollection);
    RUN(TestVertexShaderInfoCollection);
    RUN(TestPixelShaderInfoCollection);
    RUN(TestShaderInfoCollectionIsTransactional);
    RUN(TestShaderInfoMetadataValidation);
    RUN(TestTrackingRequiresSrtPatching);
    RUN(TestDynamicSrtReadRemainsExplicit);
    RUN(TestSrtPatchingFailureIsTransactional);
    RUN(TestScalarMemoryGroupsSnapshotOperands);
    RUN(TestSrtPatchingHandlesGvnAndMoveForwarding);
    RUN(TestSrtPatchingHandlesCfgProducers);
    RUN(TestSrtPatchPlanValidation);
    RUN(TestMaterializationSharesReadConstEvaluation);
    RUN(TestMaterializationFailureIsTransactional);
    RUN(TestResourceSpecializationIsTypedAndTransactional);
    RUN(TestRuntimeSpecializationCoversBakedBufferAndAddressFields);
    RUN(TestTrackedProgramIsImmutable);
    RUN(TestNativeBindingLayout);
    RUN(TestNativeBindingLayoutSrtAndUserDataOverflow);
    RUN(TestNativeBindingLayoutGds);
    RUN(TestNativeBindingLayoutIsTransactional);
    RUN(TestNativeBindingLayoutTracksReachingUserData);
    RUN(TestNativeBindingLayoutRejectsUnknownShapeAndBadProvenance);
    RUN(TestNativeBindingLayoutDynamicSrtDoesNotUseFlatBinding);
    RUN(TestNativeBindingLayoutTracksRawScalarMemoryBase);
    RUN(TestRawScalarMemoryTracksReachingBaseIdentity);
    RUN(TestNativeBindingLayoutUsesExplicitFlatMemory);
    RUN(TestNativeBindingLayoutHonorsUserDataCount);
    RUN(TestNativeBindingLayoutHonorsUserDataBase);
    RUN(TestTextureNullDescriptorUsesAddressBits);
    std::cout << "ResourceTrackingTests: all cases passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "ResourceTrackingTests: " << current << " failed: " << e.what() << '\n';
    return 1;
  }
#undef RUN
}
