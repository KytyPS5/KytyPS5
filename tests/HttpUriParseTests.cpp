#include "common/abi.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

} // namespace

int main() {
  const auto parse = GetHttpUriParse();
  if (parse == nullptr) {
    return 1;
  }
  TestAbsentQuery(parse);
  TestEmptyUri(parse);
  TestPresentQuery(parse);
  return failures == 0 ? 0 : 1;
}
