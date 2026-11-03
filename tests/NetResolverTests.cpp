#include "common/abi.h"
#include "libs/errno.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

namespace Libs::LibNet {
void InitNet_1_Net(Loader::SymbolDatabase *symbols);
}

namespace {

namespace Network = Libs::Network;
using Network::Net::NetResolverInfo;

using StartNtoa = int(KYTY_SYSV_ABI *)(int, const char *, uint32_t *, int, int,
                                       int);
using StartNtoaMultipleRecords = int(KYTY_SYSV_ABI *)(int, const char *,
                                                      NetResolverInfo *, int,
                                                      int, int);

class NetResolverTest : public testing::Test {
protected:
  void SetUp() override {
    Libs::LibNet::InitNet_1_Net(&symbols);
    ntoa = Function<StartNtoa>("Nd91WaWmG2w");
    multiple = Function<StartNtoaMultipleRecords>("RCCY01Xd+58");
    ASSERT_NE(ntoa, nullptr);
    ASSERT_NE(multiple, nullptr);
    Network::Initialize();
    rid = Network::Net::NetResolverCreate(
        "resolver", Network::Net::NetPoolCreate("net", 0x10000, 0), 0);
    ASSERT_GE(rid, 0);
  }

  void TearDown() override { Network::Shutdown(); }

  template <typename T> T Function(const char *nid) const {
    const auto *record = symbols.FindByNid(nid, Loader::SymbolType::Func);
    return record != nullptr ? reinterpret_cast<T>(record->vaddr) : nullptr;
  }

  Loader::SymbolDatabase symbols;
  StartNtoa ntoa = nullptr;
  StartNtoaMultipleRecords multiple = nullptr;
  int rid = -1;
};

TEST_F(NetResolverTest, MultipleRecordsStoresAddressLiteral) {
  NetResolverInfo info;
  std::memset(&info, 0xff, sizeof(info));
  ASSERT_EQ(multiple(rid, "10.1.2.3", &info, 0, 0, 0), 0);

  NetResolverInfo expected{};
  expected.addrs[0].addr = 0x0302010au;
  expected.addrs[0].af = 2;
  expected.records = 1;
  expected.dns4records = 1;
  EXPECT_EQ(std::memcmp(&info, &expected, sizeof(info)), 0);
}

TEST_F(NetResolverTest, MultipleRecordsResolvesHostName) {
  NetResolverInfo info{};
  ASSERT_EQ(multiple(rid, "localhost", &info, 0, 0, 0), 0);
  ASSERT_GE(info.records, 1);
  EXPECT_LE(info.records, 10);
  EXPECT_GE(info.dns4records, info.records);
  EXPECT_EQ(info.addrs[0].addr, 0x0100007fu);
  for (int i = 0; i < info.records; i++) {
    EXPECT_EQ(info.addrs[i].af, 2);
  }
}

TEST_F(NetResolverTest, MultipleRecordsRejectsBadArguments) {
  NetResolverInfo info{};
  EXPECT_EQ(multiple(rid, "10.1.2.3", nullptr, 0, 0, 0),
            Network::NET_ERROR_EINVAL);
  EXPECT_EQ(multiple(rid, nullptr, &info, 0, 0, 0), Network::NET_ERROR_EINVAL);
  EXPECT_EQ(multiple(rid + 1, "10.1.2.3", &info, 0, 0, 0),
            Network::NET_ERROR_EBADF);
}

TEST_F(NetResolverTest, NtoaStoresFirstAddress) {
  uint32_t addr = 0;
  ASSERT_EQ(ntoa(rid, "10.1.2.3", &addr, 0, 0, 0), 0);
  EXPECT_EQ(addr, 0x0302010au);
  ASSERT_EQ(ntoa(rid, "localhost", &addr, 0, 0, 0), 0);
  EXPECT_EQ(addr, 0x0100007fu);
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
