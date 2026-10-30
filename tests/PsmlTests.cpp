#include "common/abi.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/pssr.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace Libs::LibPsml {
void InitPsml_1(Loader::SymbolDatabase *symbols);
}

namespace {
bool g_trinity = true;
}

namespace Loader {
bool SystemContentIsTrinityMode() { return g_trinity; }
} // namespace Loader

namespace {

namespace Pm4 = Libs::Graphics::Pm4;
namespace Pssr = Libs::Graphics::Pssr;

constexpr int ERROR_ALREADY_INITIALIZED = static_cast<int>(0x8a810002u);
constexpr int ERROR_TOO_MANY_CONTEXTS = static_cast<int>(0x8a810004u);
constexpr int ERROR_INVALID_CONTEXT = static_cast<int>(0x8a810005u);
constexpr int ERROR_NO_SPACE = static_cast<int>(0x8a810006u);
constexpr int ERROR_BAD_COMMAND_BUFFER = static_cast<int>(0x8a810007u);
constexpr int ERROR_NULL_ARGUMENT = static_cast<int>(0x8a810009u);
constexpr int ERROR_NULL_TEXTURE = static_cast<int>(0x8a81000au);
constexpr int ERROR_INVALID_TEXTURE = static_cast<int>(0x8a81000bu);
constexpr int ERROR_RENDER_SIZE = static_cast<int>(0x8a81000eu);
constexpr int ERROR_OUTPUT_TEXTURE = static_cast<int>(0x8a81000fu);
constexpr int ERROR_NOT_TRINITY = static_cast<int>(0x8a810012u);
constexpr int ERROR_INVALID_SHARED = static_cast<int>(0x8a810014u);
constexpr uint64_t CANARY = 0xfedcba9876543210;

struct MemoryBlock {
  uint64_t offset = 0;
  uint64_t size = 0x200000;
};

struct SharedParameters {
  uint32_t profile = 0;
  uint32_t reserved = 0;
  const MemoryBlock *blocks = nullptr;
  uint64_t block_count = 0;
  uint64_t virtual_address = 0;
};

struct ContextParameters {
  uint32_t profile = 0;
  uint32_t reserved = 0;
  const MemoryBlock *blocks = nullptr;
  uint64_t block_count = 0;
  uint16_t width = 960;
  uint16_t height = 540;
  uint32_t reserved1 = 0;
  const void *depth_copy = nullptr;
  const void *motion_copy = nullptr;
  const char *name = nullptr;
  void *shared = nullptr;
};
static_assert(offsetof(ContextParameters, shared) == 0x38);

struct CommandSpace {
  uint32_t *cmd;
  uint32_t dwords;
};

struct DispatchParameters {
  const uint32_t *output;
  const uint32_t *input;
  std::array<uint8_t, 0xe0> unused{};
  uint32_t width;
  uint32_t height;
};
static_assert(offsetof(DispatchParameters, width) == 0xf0);

struct GuardedHandle {
  uint64_t before = CANARY;
  void *value = nullptr;
  std::array<uint64_t, 128> after;

  GuardedHandle() { after.fill(CANARY); }

  bool Intact() const {
    return before == CANARY &&
           std::all_of(after.begin(), after.end(),
                       [](uint64_t value) { return value == CANARY; });
  }
};

using Initialize = int(KYTY_SYSV_ABI *)();
using CreateShared = int(KYTY_SYSV_ABI *)(void **, const SharedParameters *);
using CreateContext = int(KYTY_SYSV_ABI *)(void **, const ContextParameters *);
using Release = int(KYTY_SYSV_ABI *)(void *);
using GetSize = int(KYTY_SYSV_ABI *)(const void *, uint32_t *);
using Dispatch = int(KYTY_SYSV_ABI *)(const void *, const CommandSpace *,
                                      const void *);
using GetBias = int(KYTY_SYSV_ABI *)(const void *, float *, uint32_t, uint32_t);
using Predicate = int(KYTY_SYSV_ABI *)(const void *);

Loader::SymbolDatabase g_symbols;

template <typename T> T Function(const char *nid) {
  const auto *symbol = g_symbols.FindByNid(nid, Loader::SymbolType::Func);
  EXPECT_NE(symbol, nullptr);
  return symbol == nullptr ? nullptr : reinterpret_cast<T>(symbol->vaddr);
}

std::array<uint32_t, 8> Texture(uint32_t width, uint32_t height,
                                uint32_t address) {
  std::array<uint32_t, 8> result{};
  result[0] = address;
  result[1] = (((width - 1) & 3) << 30) | (36u << 20);
  result[2] = ((width - 1) >> 2) | ((height - 1) << 14);
  result[3] = (9u << 28) | (1u << 20) | 0xfacu;
  return result;
}

class PsmlTest : public testing::Test {
protected:
  static void SetUpTestSuite() {
    Libs::LibPsml::InitPsml_1(&g_symbols);
    g_trinity = false;
    ASSERT_EQ(Function<Initialize>("3WVD91e12ZQ")(), ERROR_NOT_TRINITY);
    g_trinity = true;
    ASSERT_EQ(Function<Initialize>("3WVD91e12ZQ")(), 0);
  }

  void SetUp() override {
    SharedParameters params;
    params.blocks = blocks.data();
    params.block_count = blocks.size();
    GuardedHandle result;
    ASSERT_EQ(Function<CreateShared>("eWoKNeB6V-k")(&result.value, &params), 0);
    ASSERT_TRUE(result.Intact());
    ASSERT_GT(reinterpret_cast<uintptr_t>(result.value), 0x10000u);
    shared = result.value;

    context_params.blocks = blocks.data();
    context_params.block_count = 1;
    context_params.shared = shared;
    result.value = nullptr;
    ASSERT_EQ(
        Function<CreateContext>("gxv3i+MTEzU")(&result.value, &context_params),
        0);
    ASSERT_TRUE(result.Intact());
    ASSERT_GT(reinterpret_cast<uintptr_t>(result.value), 0x10000u);
    context = result.value;
  }

  void TearDown() override {
    for (auto *extra : extra_contexts) {
      EXPECT_EQ(Function<Release>("JaLBe0P3jSU")(extra), 0);
    }
    if (context != nullptr) {
      EXPECT_EQ(Function<Release>("JaLBe0P3jSU")(context), 0);
    }
    if (shared != nullptr) {
      EXPECT_EQ(Function<Release>("jEevBXmagOQ")(shared), 0);
    }
  }

  std::array<MemoryBlock, 52> blocks;
  ContextParameters context_params;
  void *shared = nullptr;
  void *context = nullptr;
  std::vector<void *> extra_contexts;
};

TEST_F(PsmlTest, HandlesPreserveAdjacentMemoryAndRejectReleasedObjects) {
  EXPECT_EQ(Function<Initialize>("3WVD91e12ZQ")(), ERROR_ALREADY_INITIALIZED);
  uint32_t size = 0;
  EXPECT_EQ(Function<GetSize>("AHalTX9wFZY")(context, &size), 0);
  EXPECT_EQ(size, Pssr::RequestDwords + 1);
  EXPECT_EQ(Function<Release>("jEevBXmagOQ")(shared), ERROR_INVALID_CONTEXT);
  EXPECT_EQ(Function<Predicate>("LXq+6mIxpCw")(context), 0);
  EXPECT_EQ(Function<Release>("JaLBe0P3jSU")(context), 0);
  EXPECT_EQ(Function<Release>("JaLBe0P3jSU")(context), ERROR_INVALID_CONTEXT);
  EXPECT_EQ(Function<GetSize>("AHalTX9wFZY")(context, &size),
            ERROR_INVALID_CONTEXT);
  EXPECT_EQ(Function<Predicate>("LXq+6mIxpCw")(context), ERROR_INVALID_CONTEXT);
  context = nullptr;
  EXPECT_EQ(Function<Release>("jEevBXmagOQ")(shared), 0);
  EXPECT_EQ(Function<Release>("jEevBXmagOQ")(shared), ERROR_INVALID_SHARED);
  shared = nullptr;
  EXPECT_EQ(
      Function<GetSize>("AHalTX9wFZY")(reinterpret_cast<void *>(1), &size),
      ERROR_INVALID_CONTEXT);
}

TEST_F(PsmlTest, LegacyContextVariantsUseTheirOwnLayoutsAndOutputResolution) {
  context_params.shared = reinterpret_cast<void *>(1);
  GuardedHandle result;
  ASSERT_EQ(
      Function<CreateContext>("gxv3i+MTEzU")(&result.value, &context_params),
      ERROR_INVALID_SHARED);
  ASSERT_TRUE(result.Intact());
  ASSERT_EQ(result.value, nullptr);
  ASSERT_EQ(
      Function<CreateContext>("vUk2pWMx3KQ")(&result.value, &context_params),
      0);
  ASSERT_TRUE(result.Intact());
  extra_contexts.push_back(result.value);
  float bias = 0;
  EXPECT_EQ(Function<GetBias>("GHna9-DvnUk")(result.value, &bias, 480, 270), 0);
  EXPECT_FLOAT_EQ(bias, -2.0f);
  ASSERT_EQ(
      Function<CreateContext>("2ecEbQaf9VU")(&result.value, &context_params),
      0);
  ASSERT_TRUE(result.Intact());
  extra_contexts.push_back(result.value);
  EXPECT_EQ(Function<GetBias>("GHna9-DvnUk")(result.value, &bias, 1920, 1080),
            0);
  EXPECT_FLOAT_EQ(bias, -2.0f);
}

TEST_F(PsmlTest, CapsLiveContextsAndReleasesCapacity) {
  for (int i = 0; i < 3; ++i) {
    void *extra = nullptr;
    ASSERT_EQ(Function<CreateContext>("gxv3i+MTEzU")(&extra, &context_params),
              0);
    extra_contexts.push_back(extra);
  }
  void *fifth = nullptr;
  EXPECT_EQ(Function<CreateContext>("gxv3i+MTEzU")(&fifth, &context_params),
            ERROR_TOO_MANY_CONTEXTS);
  EXPECT_EQ(fifth, nullptr);
  EXPECT_EQ(Function<Release>("JaLBe0P3jSU")(extra_contexts.back()), 0);
  extra_contexts.pop_back();
  ASSERT_EQ(Function<CreateContext>("gxv3i+MTEzU")(&fifth, &context_params), 0);
  extra_contexts.push_back(fifth);
}

TEST_F(PsmlTest, DispatchSnapshotsFixedSlotsAndActiveExtent) {
  auto output = Texture(960, 540, 0x10000);
  auto input = Texture(1920, 1080, 0x20000);
  DispatchParameters params{output.data(), input.data(), {}, 640, 360};
  std::array<uint32_t, 32> command;
  command.fill(0xdeadbeef);
  const CommandSpace space{command.data() + 1, 28};
  ASSERT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params), 0);
  EXPECT_EQ(KYTY_PM4_LEN(command[1]), space.dwords);
  EXPECT_EQ(KYTY_PM4_R(command[1]), Pm4::R_PSSR);
  EXPECT_EQ((command[1] >> 8) & 0xffu, Pm4::IT_NOP);
  Pssr::Request request;
  std::memcpy(&request, command.data() + 2, sizeof(request));
  EXPECT_EQ(std::memcmp(request.output, output.data(), sizeof(request.output)),
            0);
  EXPECT_EQ(std::memcmp(request.input, input.data(), sizeof(request.input)), 0);
  EXPECT_EQ(request.width, 640u);
  EXPECT_EQ(request.height, 360u);
  EXPECT_EQ(command.front(), 0xdeadbeefu);
  EXPECT_EQ(command[29], 0xdeadbeefu);
  for (size_t i = 2 + Pssr::RequestDwords; i < 29; ++i) {
    EXPECT_EQ(command[i], 0u);
  }
  output.fill(0);
  input.fill(0);
  params = {};
  Pssr::Request snapshot;
  std::memcpy(&snapshot, command.data() + 2, sizeof(snapshot));
  EXPECT_EQ(std::memcmp(&snapshot, &request, sizeof(request)), 0);
}

TEST_F(PsmlTest, DispatchAcceptsEqualSizedBuffersAndValidatesWithoutWriting) {
  auto output = Texture(960, 540, 0x10000);
  auto input = Texture(960, 540, 0x20000);
  DispatchParameters params{output.data(), input.data(), {}, 960, 540};
  uint32_t dwords = 0;
  ASSERT_EQ(Function<GetSize>("AHalTX9wFZY")(context, &dwords), 0);
  std::vector<uint32_t> command(dwords, 0xdeadbeef);
  CommandSpace space{command.data(), dwords};
  ASSERT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params), 0);
  std::fill(command.begin(), command.end(), 0xdeadbeef);
  space.dwords = dwords - 1;
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_NO_SPACE);
  space.dwords = 0x4002;
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_BAD_COMMAND_BUFFER);
  space.dwords = dwords;
  params.width = 961;
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_RENDER_SIZE);
  params.width = 960;
  output = Texture(960, 539, 0x10000);
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_OUTPUT_TEXTURE);
  params.input = nullptr;
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_NULL_TEXTURE);
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, nullptr),
            ERROR_NULL_ARGUMENT);
  EXPECT_TRUE(std::all_of(command.begin(), command.end(),
                          [](uint32_t value) { return value == 0xdeadbeef; }));
}

TEST_F(PsmlTest, DispatchChecksDescriptorContractAndSelectedMip) {
  auto output = Texture(960, 540, 0x10000);
  auto input = Texture(1920, 1080, 0x20000);
  input[3] |= (1u << 12) | (1u << 16);
  input[5] |= 1u << 4;
  DispatchParameters params{output.data(), input.data(), {}, 960, 540};
  std::array<uint32_t, Pssr::RequestDwords + 1> command{};
  const CommandSpace space{command.data(), command.size()};
  ASSERT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params), 0);
  command.fill(0xdeadbeef);
  params.width = 961;
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_RENDER_SIZE);
  params.width = 960;
  input[1] &= ~(0x1ffu << 20);
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_INVALID_TEXTURE);
  input = Texture(960, 540, 0x20000);
  input[3] = (input[3] & ~(0xfu << 28)) | (14u << 28);
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_INVALID_TEXTURE);
  input = Texture(960, 540, 0x20000);
  output[3] &= ~(0x1fu << 20);
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_OUTPUT_TEXTURE);
  output = Texture(960, 540, 0x20000);
  EXPECT_EQ(Function<Dispatch>("RUNLFro+qok")(context, &space, &params),
            ERROR_OUTPUT_TEXTURE);
  EXPECT_TRUE(std::all_of(command.begin(), command.end(),
                          [](uint32_t value) { return value == 0xdeadbeef; }));
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
