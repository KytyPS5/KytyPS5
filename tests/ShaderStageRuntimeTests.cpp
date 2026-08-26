#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/shader.h"

#include <cstdio>
#include <cstdlib>
#include <memory>

namespace {

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "ShaderStageRuntimeTests: failed: %s\n", text);
    std::abort();
  }
}

bool RejectSpecializationRead(void *userdata, uint64_t, uint32_t *) {
  ++*static_cast<uint32_t *>(userdata);
  return false;
}

Libs::Graphics::ShaderRecompiler::IR::Block &
AddValueBlock(Libs::Graphics::ShaderRecompiler::IR::Program &program) {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  auto block = std::make_unique<Block>();
  auto *result = block.get();
  program.blocks.push_back(result);
  program.block_info.push_back({.id = 0});
  program.block_storage.push_back(std::move(block));
  return *result;
}

std::shared_ptr<const Libs::Graphics::ShaderRecompiler::IR::Program>
SrtProgram(uint64_t address) {
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
  program.srt_reads.push_back({Value(&raw), 0, 0x40});

  auto &srt = value_block.AppendNewInst(ValueOpcode::GetSrtResource);
  auto &flat = value_block.AppendNewInst(ValueOpcode::ReadConst,
                                         {Value(&srt), Value(0u)});
  DescriptorSource source;
  source.dwords[0] = Value(&flat);
  source.dwords[1] = Value(0u);
  source.dword_count = 2;
  program.descriptor_sources.push_back(source);
  return std::make_shared<const Program>(std::move(program));
}

std::shared_ptr<const Libs::Graphics::ShaderRecompiler::IR::Program>
UnbasedFlatProgram() {
  using namespace Libs::Graphics::ShaderRecompiler::IR;
  Program program;
  program.stage = Libs::Graphics::ShaderType::Compute;
  program.srt_plan_complete = true;
  program.resource_tracking_complete = true;
  AddValueBlock(program);
  AddressResource address;
  address.source = UINT32_MAX;
  address.first_use_pc = 0x80;
  address.kind = ResourceKind::Flat;
  address.unbased = true;
  program.info.addresses.push_back(address);
  return std::make_shared<const Program>(std::move(program));
}

void TestMappedSrtUsesDirectReaderByDefault() {
  using namespace Libs::Graphics;
  const uint32_t dword = 0x12345678;
  auto cached_program = SrtProgram(reinterpret_cast<uint64_t>(&dword));
  ShaderStageRuntime stage;
  std::string error;
  uint32_t specialization_reads = 0;
  Check(ShaderMaterializeStageRuntime(cached_program, {}, 0, stage, &error,
                                      RejectSpecializationRead,
                                      &specialization_reads),
        error.c_str());
  Check(specialization_reads == 0,
        "ordinary SRT read used the specialization reader");
  Check(stage.program == cached_program && stage.resources != nullptr,
        "cache rematerialization did not publish the mapped stage");
  Check(stage.resources->flattened_srt.size() == 1 &&
            stage.resources->flattened_srt[0] == dword,
        "cache rematerialization did not use the direct reader by default");
}

void TestUnbasedFlatCacheHitFailsClosed() {
  using namespace Libs::Graphics;
  auto cached_program = UnbasedFlatProgram();
  auto prior_program = std::make_shared<const ShaderRecompiler::IR::Program>();
  auto prior_resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>();
  ShaderStageRuntime stage{prior_program, prior_resources};
  std::string error;
  Check(!ShaderMaterializeStageRuntime(cached_program, {}, 0, stage, &error) &&
            error.find("requires runtime guest-address translation") !=
                std::string::npos,
        "unbased FLAT cache hit did not fail without a runtime translator");
  Check(stage.program == prior_program && stage.resources == prior_resources,
        "unbased FLAT cache rejection replaced the prior stage snapshot");
}

} // namespace

namespace Common {

int DbgExitIfHandler(const char *, const char *, int) { return 1; }

void DbgExit(int) { std::abort(); }

} // namespace Common

int main() {
  TestMappedSrtUsesDirectReaderByDefault();
  TestUnbasedFlatCacheHitFailsClosed();
  std::puts("ShaderStageRuntimeTests: all cases passed");
  return 0;
}

// Keep this focused standalone target self-contained by amalgamating its small
// typed-IR implementation set.
#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
