// Tests for the run-ahead over a command list that finds its compute
// dispatches.

#include "graphics/guest_gpu/computePrescan.h"

#include <cstring>
#include <iostream>
#include <map>
#include <vector>

namespace {

using namespace Libs::Graphics;

int g_failures = 0;

void Check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    g_failures++;
  }
}

uint32_t Header(uint32_t opcode, uint32_t length_dw) {
  return (3u << 30u) | ((length_dw - 2u) << 16u) | (opcode << 8u);
}

void SetSh(std::vector<uint32_t> &list, uint32_t reg,
           std::initializer_list<uint32_t> values) {
  list.push_back(
      Header(Pm4::IT_SET_SH_REG, static_cast<uint32_t>(values.size()) + 2u));
  list.push_back(reg);
  list.insert(list.end(), values);
}

void Dispatch(std::vector<uint32_t> &list, uint32_t mode) {
  list.push_back(Header(Pm4::IT_DISPATCH_DIRECT, 5u));
  list.insert(list.end(), {8u, 4u, 1u, mode});
}

void IndirectBuffer(std::vector<uint32_t> &list, uint64_t address,
                    uint32_t count) {
  list.push_back(Header(Pm4::IT_INDIRECT_BUFFER, 4u));
  list.insert(list.end(), {static_cast<uint32_t>(address),
                           static_cast<uint32_t>(address >> 32u), count});
}

// Guest memory for the tests: indirect buffers by address.
struct Memory {
  std::map<uint64_t, std::vector<uint32_t>> buffers;
  int reads = 0;

  bool operator()(uint64_t address, std::span<uint32_t> words) {
    reads++;
    const auto found = buffers.find(address);
    if (found == buffers.end() || found->second.size() < words.size())
      return false;
    std::memcpy(words.data(), found->second.data(), words.size_bytes());
    return true;
  }
};

void TestADispatchSeesTheRegistersSetBeforeIt() {
  std::vector<uint32_t> list;
  SetSh(list, Pm4::COMPUTE_PGM_LO, {0x123456u, 0x7fu});
  SetSh(list, Pm4::COMPUTE_PGM_RSRC2,
        {4u << Pm4::COMPUTE_PGM_RSRC2_USER_SGPR_SHIFT});
  SetSh(list, Pm4::COMPUTE_NUM_THREAD_X, {64u, 2u, 1u});
  SetSh(list, Pm4::COMPUTE_USER_DATA_0 + 2u, {0xaaaau, 0xbbbbu});
  list.push_back(0x80000000u); // A one-dword NOP between packets.
  Dispatch(list, Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS);

  HW::ComputeShaderInfo state{};
  Memory memory;
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found);
  Check(found.size() == 1u, "one dispatch found");
  if (found.size() != 1u)
    return;
  const auto &regs = found[0].regs;
  Check(regs.cs_regs.data_addr == ((0x7full << 40u) | (0x123456ull << 8u)),
        "the shader address comes from COMPUTE_PGM_LO and COMPUTE_PGM_HI");
  Check(regs.cs_regs.user_sgpr == 4u, "the user SGPR count comes from RSRC2");
  Check(regs.cs_regs.num_thread_x == 64u && regs.cs_regs.num_thread_y == 2u &&
            regs.cs_regs.num_thread_z == 1u,
        "the group size comes from the NUM_THREAD registers");
  Check(regs.cs_user_sgpr.value[2] == 0xaaaau &&
            regs.cs_user_sgpr.value[3] == 0xbbbbu,
        "user data goes to its slots");
  Check(found[0].mode ==
                Pm4::COMPUTE_DISPATCH_INITIATOR_USE_THREAD_DIMENSIONS &&
            !found[0].indirect,
        "the dispatch keeps its initiator");
  Check(state.cs_regs.data_addr == regs.cs_regs.data_addr,
        "the state after the list holds its last registers");
  Check(memory.reads == 0, "a list without indirect buffers reads no memory");
}

void TestTheInitiatorSetsTheWaveSize() {
  std::vector<uint32_t> list;
  Dispatch(list, 1u << Pm4::COMPUTE_DISPATCH_INITIATOR_CS_W32_EN_SHIFT);
  Dispatch(list, 0u);
  Memory memory;
  HW::ComputeShaderInfo state{};
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found);
  Check(found.size() == 2u && found[0].regs.cs_regs.wave_size == 32u &&
            found[1].regs.cs_regs.wave_size == 64u,
        "each dispatch runs with the wave size of its initiator, as the CP "
        "sets it");
}

void TestTheScanStartsFromTheRegistersItIsGiven() {
  std::vector<uint32_t> list;
  Dispatch(list, 0u);
  HW::ComputeShaderInfo state{};
  state.cs_regs.data_addr = 0x5000;
  Memory memory;
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found);
  Check(found.size() == 1u && found[0].regs.cs_regs.data_addr == 0x5000,
        "a dispatch uses registers set by an earlier list");
}

void TestIndirectBuffersAreFollowed() {
  Memory memory;
  auto &nested = memory.buffers[0x10000];
  Dispatch(nested, 0u);
  SetSh(nested, Pm4::COMPUTE_PGM_LO, {0x2u});

  std::vector<uint32_t> list;
  SetSh(list, Pm4::COMPUTE_PGM_LO, {0x1u});
  IndirectBuffer(list, 0x10000, static_cast<uint32_t>(nested.size()));
  Dispatch(list, 0u);

  HW::ComputeShaderInfo state{};
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found);
  Check(found.size() == 2u,
        "the dispatch inside the indirect buffer and the one after it");
  if (found.size() != 2u)
    return;
  Check(
      found[0].regs.cs_regs.data_addr == 0x100u,
      "the nested dispatch sees the registers set before the indirect buffer");
  Check(found[1].regs.cs_regs.data_addr == 0x200u,
        "a register set inside the indirect buffer stays set after it");
}

void TestIndirectRegisterPairsAreApplied() {
  Memory memory;
  memory.buffers[0x40000] = {
      Pm4::COMPUTE_PGM_LO | 0x10000000u, 0x77u, 0xffffffffu, 0x1u,
      Pm4::COMPUTE_USER_DATA_0 + 1u,     0x99u};
  std::vector<uint32_t> list;
  list.push_back(Header(Pm4::IT_SET_SH_REG_INDIRECT, 5u));
  list.insert(list.end(), {0x40000u, 0u, 0u, 3u});
  Dispatch(list, 0u);
  HW::ComputeShaderInfo state{};
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found);
  Check(found.size() == 1u &&
            found[0].regs.cs_regs.data_addr == (0x77ull << 8u) &&
            found[0].regs.cs_user_sgpr.value[1] == 0x99u,
        "indirect register pairs set the shader address and user data, past a "
        "skipped pair");
}

void TestAnUnreadableIndirectBufferIsSkipped() {
  std::vector<uint32_t> list;
  IndirectBuffer(list, 0xdead0000, 16u);
  Dispatch(list, 0u);
  Memory memory;
  HW::ComputeShaderInfo state{};
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found);
  Check(memory.reads == 1 && found.size() == 1u,
        "the scan goes on after an indirect buffer it cannot read");
}

void TestIndirectDispatchesKeepTheirInitiator() {
  std::vector<uint32_t> list;
  list.push_back(
      0xc0021600u); // address form: address low, address high, initiator
  list.insert(list.end(), {0x1000u, 0u, 0x21u});
  list.push_back(0xc0011600u); // offset form: offset, initiator
  list.insert(list.end(), {0x40u, 0x1u});
  Memory memory;
  HW::ComputeShaderInfo state{};
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found);
  Check(found.size() == 2u && found[0].indirect && found[0].mode == 0x21u &&
            found[1].indirect && found[1].mode == 0x1u,
        "both indirect dispatch forms are found with their initiator");
}

void TestBrokenListsEndTheScan() {
  std::vector<uint32_t> truncated;
  Dispatch(truncated, 0u);
  truncated.pop_back(); // The last dispatch dword is missing.
  Memory memory;
  HW::ComputeShaderInfo state{};
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(truncated, state, memory, found);
  Check(found.empty(), "a packet past the end of the list is not read");

  std::vector<uint32_t> not_type3{0x12345678u};
  Dispatch(not_type3, 0u);
  found.clear();
  PrescanComputeDispatches(not_type3, state, memory, found);
  Check(found.empty(), "a word that is no type-3 header ends the scan");
}

void TestLimitsBoundTheWork() {
  Memory memory;
  auto &loop = memory.buffers[0x20000];
  IndirectBuffer(loop, 0x20000, 4u); // An indirect buffer that runs itself.
  Dispatch(loop, 0u);
  std::vector<uint32_t> list;
  IndirectBuffer(list, 0x20000, static_cast<uint32_t>(loop.size()));
  HW::ComputeShaderInfo state{};
  std::vector<PrescannedDispatch> found;
  PrescanComputeDispatches(list, state, memory, found, {.max_depth = 3});
  Check(memory.reads == 3, "nesting stops at the depth limit");

  std::vector<uint32_t> many;
  for (int i = 0; i < 10; i++)
    Dispatch(many, 0u);
  found.clear();
  PrescanComputeDispatches(many, state, memory, found, {.max_dispatches = 4});
  Check(found.size() == 4u, "the scan stops at the dispatch limit");

  std::vector<uint32_t> big;
  IndirectBuffer(big, 0x30000, 100u);
  const int before = memory.reads;
  PrescanComputeDispatches(big, state, memory, found, {.max_dwords = 50});
  Check(memory.reads == before,
        "an indirect buffer over the dword budget is not read");
}

void TestUnknownRegistersAreRefused() {
  HW::CsStageRegisters regs{};
  Check(!ApplyComputeShaderRegister(regs, Pm4::COMPUTE_USER_DATA_0, 1u),
        "user data is not a compute shader register");
  Check(ApplyComputeShaderRegister(regs, Pm4::COMPUTE_PACE_ID, 1u),
        "an ignored register is still known");
}

} // namespace

int main() {
  TestADispatchSeesTheRegistersSetBeforeIt();
  TestTheScanStartsFromTheRegistersItIsGiven();
  TestTheInitiatorSetsTheWaveSize();
  TestIndirectBuffersAreFollowed();
  TestIndirectRegisterPairsAreApplied();
  TestAnUnreadableIndirectBufferIsSkipped();
  TestIndirectDispatchesKeepTheirInitiator();
  TestBrokenListsEndTheScan();
  TestLimitsBoundTheWork();
  TestUnknownRegistersAreRefused();
  if (g_failures != 0) {
    std::cerr << g_failures << " failure(s)\n";
    return 1;
  }
  std::cout << "compute prescan: all tests passed\n";
  return 0;
}
