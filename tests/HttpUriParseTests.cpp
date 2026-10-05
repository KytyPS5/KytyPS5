#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace Libs::LibHttp {
void InitNet_1_Http(Loader::SymbolDatabase *symbols);
}

namespace {

struct SceHttpUriElement {
  int opaque = 0;
  char *scheme = nullptr;
  char *username = nullptr;
  char *password = nullptr;
  char *hostname = nullptr;
  char *path = nullptr;
  char *query = nullptr;
  char *fragment = nullptr;
  uint16_t port = 0;
  uint8_t reserved[10]{};
};

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "HttpUriParseTests:%d: %s\n", __LINE__,             \
                   #condition);                                                \
      failures++;                                                              \
    }                                                                          \
  } while (false)

using HttpUriParse = int(KYTY_SYSV_ABI *)(SceHttpUriElement *, const char *,
                                          void *, size_t *, size_t);

using HttpUriBuild = int(KYTY_SYSV_ABI *)(char *, size_t *, size_t,
                                          const SceHttpUriElement *, uint32_t);

HttpUriBuild GetHttpUriBuild() {
  Loader::SymbolDatabase symbols;
  Libs::LibHttp::InitNet_1_Http(&symbols);
  const auto *record =
      symbols.FindByNid("5LZA+KPISVA", Loader::SymbolType::Func);
  CHECK(record != nullptr);
  return record != nullptr ? reinterpret_cast<HttpUriBuild>(record->vaddr)
                           : nullptr;
}

HttpUriParse GetHttpUriParse() {
  Loader::SymbolDatabase symbols;
  Libs::LibHttp::InitNet_1_Http(&symbols);
  const auto *record =
      symbols.FindByNid("IWalAn-guFs", Loader::SymbolType::Func);
  CHECK(record != nullptr);
  return record != nullptr ? reinterpret_cast<HttpUriParse>(record->vaddr)
                           : nullptr;
}

template <size_t N>
bool PointsIntoPool(const char *ptr, const std::array<char, N> &pool,
                    size_t used) {
  if (ptr == nullptr) {
    return false;
  }
  const auto address = reinterpret_cast<uintptr_t>(ptr);
  const auto begin = reinterpret_cast<uintptr_t>(pool.data());
  return address >= begin && address < begin + used;
}

void TestAbsentQuery(HttpUriParse parse) {
  constexpr char url[] = "http://example.com/path";
  size_t required = 0;
  CHECK(parse(nullptr, url, nullptr, &required, 0) == 0);

  const size_t expected_required =
      sizeof("http") + sizeof("example.com") + sizeof("/path") + 1;
  CHECK(required == expected_required);

  std::array<char, 64> pool{};
  SceHttpUriElement out{};
  size_t parsed_required = 0;
  CHECK(parse(&out, url, pool.data(), &parsed_required, required) == 0);
  CHECK(parsed_required == required);
  CHECK(out.query != nullptr);
  CHECK(PointsIntoPool(out.query, pool, required));
  if (out.query != nullptr) {
    CHECK(out.query[0] == '\0');
  }
}

void TestEmptyUri(HttpUriParse parse) {
  constexpr char url[] = "";
  size_t required = 0;
  CHECK(parse(nullptr, url, nullptr, &required, 0) == 0);
  CHECK(required == 4);

  std::array<char, 4> pool{};
  SceHttpUriElement out{};
  size_t parsed_required = 0;
  CHECK(parse(&out, url, pool.data(), &parsed_required, required) == 0);
  CHECK(parsed_required == required);
  CHECK(out.query != nullptr);
  CHECK(PointsIntoPool(out.query, pool, required));
  if (out.query != nullptr) {
    CHECK(out.query[0] == '\0');
  }
}

void TestPresentQuery(HttpUriParse parse) {
  constexpr char url[] = "http://example.com/path?foo=bar";
  size_t required = 0;
  CHECK(parse(nullptr, url, nullptr, &required, 0) == 0);

  std::array<char, 64> pool{};
  SceHttpUriElement out{};
  size_t parsed_required = 0;
  CHECK(parse(&out, url, pool.data(), &parsed_required, required) == 0);
  CHECK(parsed_required == required);
  CHECK(out.query != nullptr);
  CHECK(PointsIntoPool(out.query, pool, required));
  if (out.query != nullptr) {
    CHECK(std::strcmp(out.query, "?foo=bar") == 0);
  }
}

// Builds `element` with `option` and returns the text, checking that the size query
// (out == nullptr) agrees with what the fill call wrote.
std::string BuildWith(HttpUriBuild build, const SceHttpUriElement &element,
                      uint32_t option) {
  size_t required = 0;
  CHECK(build(nullptr, &required, 0, &element, option) == 0);
  std::array<char, 256> out{};
  size_t filled = 0;
  CHECK(required <= out.size());
  CHECK(build(out.data(), &filled, required, &element, option) == 0);
  CHECK(filled == required);
  return std::string(out.data());
}

// SCE_HTTP_URI_BUILD_WITH_* selects components. Black Myth: Wukong asks for SCHEME alone and
// needs "https://" back; a build that ignored the mask returned the whole URL and the game
// then refused to start any HTTP request.
void TestBuildHonoursOption(HttpUriParse parse, HttpUriBuild build) {
  constexpr char url[] =
      "https://user:pw@gssdk1.gamesci.com.cn:8443/VersionServerImpl?x=1#frag";
  size_t required = 0;
  CHECK(parse(nullptr, url, nullptr, &required, 0) == 0);
  std::array<char, 256> pool{};
  SceHttpUriElement element{};
  CHECK(parse(&element, url, pool.data(), &required, required) == 0);

  constexpr uint32_t scheme = 0x01, hostname = 0x02, port = 0x04, path = 0x08,
                     username = 0x10, password = 0x20, query = 0x40,
                     fragment = 0x80;

  CHECK(BuildWith(build, element, scheme) == "https://");
  CHECK(BuildWith(build, element, hostname) == "gssdk1.gamesci.com.cn");
  CHECK(BuildWith(build, element, port) == "8443");
  CHECK(BuildWith(build, element, path) == "/VersionServerImpl");
  CHECK(BuildWith(build, element, username) == "user");
  CHECK(BuildWith(build, element, password) == "pw");
  CHECK(BuildWith(build, element, query) == "?x=1");
  CHECK(BuildWith(build, element, fragment) == "#frag");
  CHECK(BuildWith(build, element, scheme | hostname) ==
        "https://gssdk1.gamesci.com.cn");
  CHECK(BuildWith(build, element, hostname | path) ==
        "gssdk1.gamesci.com.cn/VersionServerImpl");
  CHECK(BuildWith(build, element, hostname | port) ==
        "gssdk1.gamesci.com.cn:8443");

  // Everything selected rebuilds the original URL, and a mask with no component bit set keeps
  // the older "everything" behaviour for callers that never passed one.
  CHECK(BuildWith(build, element, 0xff) == url);
  CHECK(BuildWith(build, element, 0) == url);
}

} // namespace

int main() {
  const auto parse = GetHttpUriParse();
  const auto build = GetHttpUriBuild();
  if (parse == nullptr || build == nullptr) {
    return 1;
  }
  TestBuildHonoursOption(parse, build);
  TestAbsentQuery(parse);
  TestEmptyUri(parse);
  TestPresentQuery(parse);
  return failures == 0 ? 0 : 1;
}
