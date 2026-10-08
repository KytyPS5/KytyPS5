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

template <typename T>
T GetHttpFunction(const Loader::SymbolDatabase &symbols, const char *nid) {
  const auto *record = symbols.FindByNid(nid, Loader::SymbolType::Func);
  CHECK(record != nullptr);
  return record != nullptr ? reinterpret_cast<T>(record->vaddr) : nullptr;
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

/// Verify individual component masks, combined masks and legacy full-URI builds.
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

  CHECK(BuildWith(build, element, 0xff) == url);
  CHECK(BuildWith(build, element, 0) == url);
}

/// Check pool capacities, including zero, without allowing writes outside the pool.
void TestParseBufferBounds(HttpUriParse parse) {
  struct TestCase {
    const char *url;
    size_t required;
  };
  constexpr TestCase cases[] = {
      {"", 4},
      {"http://example.com/path",
       sizeof("http") + sizeof("example.com") + sizeof("/path") + 1},
      {"https://user:pw@[::1]:8443/p?q#f",
       sizeof("https") + sizeof("user") + sizeof("pw") + sizeof("[::1]") +
           sizeof("/p") + sizeof("?q") + sizeof("#f")},
      {"mailto:user@example.com",
       sizeof("mailto") + sizeof("user@example.com") + 1},
  };
  constexpr int out_of_memory = -2143088606;
  for (const auto &test : cases) {
    size_t required = 0;
    CHECK(parse(nullptr, test.url, nullptr, &required, 0) == 0);
    CHECK(required == test.required);
    for (const size_t capacity : {size_t{0}, size_t{1}, test.required - 1,
                                 test.required, test.required + 1}) {
      for (const bool report_size : {false, true}) {
        std::array<char, 256> pool;
        pool.fill('!');
        const auto untouched = pool;
        SceHttpUriElement element{};
        size_t reported = 0;
        const int result = parse(&element, test.url, pool.data() + 1,
                                 report_size ? &reported : nullptr, capacity);
        if (report_size) {
          CHECK(reported == test.required);
        }
        if (capacity < test.required) {
          CHECK(result == out_of_memory);
          CHECK(pool == untouched);
        } else {
          CHECK(result == 0);
          CHECK(pool.front() == '!');
          for (size_t i = test.required + 1; i < pool.size(); ++i) {
            CHECK(pool[i] == '!');
          }
          for (const char *part : {element.scheme, element.username,
                                   element.password, element.hostname,
                                   element.path, element.query, element.fragment}) {
            if (part != nullptr) {
              CHECK(PointsIntoPool(part, pool, test.required + 1));
              CHECK(part != pool.data());
            }
          }
        }
      }
    }
  }
}

/// Check full, partial and empty builds with undersized and exact output buffers.
void TestBuildBufferBounds(HttpUriParse parse, HttpUriBuild build) {
  static constexpr char url[] = "https://user:pw@[::1]:8443/path#frag";
  std::array<char, 256> pool{};
  SceHttpUriElement element{};
  CHECK(parse(&element, url, pool.data(), nullptr, pool.size()) == 0);
  struct TestCase {
    uint32_t option;
    const char *expected;
  };
  constexpr TestCase cases[] = {
      {0, url}, {0xff, url}, {0x01, "https://"}, {0x06, "[::1]:8443"},
      {0x40, ""},
  };
  constexpr int out_of_memory = -2143088606;
  for (const auto &test : cases) {
    const size_t expected_size = std::strlen(test.expected) + 1;
    size_t required = 0;
    CHECK(build(nullptr, &required, 0, &element, test.option) == 0);
    CHECK(required == expected_size);
    for (const size_t capacity : {size_t{0}, size_t{1}, expected_size - 1,
                                 expected_size, expected_size + 1}) {
      for (const bool report_size : {false, true}) {
        std::array<char, 256> buffer;
        buffer.fill('!');
        const auto untouched = buffer;
        size_t reported = 0;
        const int result = build(buffer.data() + 1,
                                 report_size ? &reported : nullptr, capacity,
                                 &element, test.option);
        if (report_size) {
          CHECK(reported == expected_size);
        }
        if (capacity < expected_size) {
          CHECK(result == out_of_memory);
          CHECK(buffer == untouched);
        } else {
          CHECK(result == 0);
          CHECK(std::memcmp(buffer.data() + 1, test.expected, expected_size) == 0);
          CHECK(buffer.front() == '!');
          for (size_t i = expected_size + 1; i < buffer.size(); ++i) {
            CHECK(buffer[i] == '!');
          }
        }
      }
    }
  }
}

} // namespace

/// Run URI parsing, component selection and buffer-capacity regressions.
int main() {
  Loader::SymbolDatabase symbols;
  Libs::LibHttp::InitNet_1_Http(&symbols);
  const auto parse = GetHttpFunction<HttpUriParse>(symbols, "IWalAn-guFs");
  const auto build = GetHttpFunction<HttpUriBuild>(symbols, "5LZA+KPISVA");
  if (parse == nullptr || build == nullptr) {
    return 1;
  }
  TestBuildHonoursOption(parse, build);
  TestAbsentQuery(parse);
  TestEmptyUri(parse);
  TestPresentQuery(parse);
  TestParseBufferBounds(parse);
  TestBuildBufferBounds(parse, build);
  return failures == 0 ? 0 : 1;
}
