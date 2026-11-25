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

namespace Libs::LibSsl {
void InitNet_1_Ssl(Loader::SymbolDatabase *symbols);
}

namespace {

namespace Network = Libs::Network;
using Network::Net::NetResolverInfo;

using StartNtoa = int(KYTY_SYSV_ABI *)(int, const char *, uint32_t *, int, int,
                                       int);
using StartNtoaMultipleRecords = int(KYTY_SYSV_ABI *)(int, const char *,
                                                      NetResolverInfo *, int,
                                                      int, int);
using SslGetMemoryPoolStats =
    int(KYTY_SYSV_ABI *)(int, Network::Net::NetMemoryPoolStats *);

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

TEST_F(NetResolverTest, BooleanSocketOptions) {
  const auto check = [](int family, int level, int option) {
    const int s = Network::Net::Socket(family, 1, 6);
    ASSERT_GE(s, 0);
    for (const int enabled : {1, 0}) {
      EXPECT_EQ(Network::Net::Setsockopt(s, level, option, &enabled, sizeof(enabled)), 0);
      int value = -1;
      uint32_t length = sizeof(value);
      EXPECT_EQ(Network::Net::Getsockopt(s, level, option, &value, &length), 0);
      EXPECT_EQ(value != 0, enabled != 0);
    }
    EXPECT_EQ(Network::Net::SocketClose(s), 0);
  };
  check(2, 0xffff, 0x4); // SO_REUSEADDR
  check(28, 41, 27);     // IPV6_V6ONLY
}

TEST_F(NetResolverTest, SslMemoryPoolStatsReportInitPoolSize) {
  Libs::LibSsl::InitNet_1_Ssl(&symbols);
  const auto get_stats = Function<SslGetMemoryPoolStats>("-PoIzr3PEk0");
  ASSERT_NE(get_stats, nullptr);
  const int ssl = Network::Ssl::SslInit(0x24000);
  Network::Net::NetMemoryPoolStats stats;
  std::memset(&stats, 0xff, sizeof(stats));
  ASSERT_EQ(get_stats(ssl, &stats), 0);
  EXPECT_EQ(stats.pool_size, 0x24000u);
  EXPECT_EQ(stats.max_inuse_size, 0u);
  EXPECT_EQ(stats.current_inuse_size, 0u);
  EXPECT_EQ(stats.reserved, 0);
  ASSERT_EQ(Network::Ssl::SslTerm(ssl), 0);
  EXPECT_EQ(get_stats(ssl, nullptr), Network::SSL_ERROR_INVALID_VALUE);
  EXPECT_EQ(get_stats(ssl, &stats), Network::SSL_ERROR_INVALID_ID);
  EXPECT_EQ(stats.pool_size, 0u);
}

} // namespace

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
