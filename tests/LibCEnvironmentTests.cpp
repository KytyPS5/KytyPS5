#include "common/abi.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Libs::LibC {
void InitLibC_1(Loader::SymbolDatabase *symbols);
} // namespace Libs::LibC

namespace Posix = Libs::Posix;

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "LibCEnvironmentTests:%d: %s\n", __LINE__,          \
                   #condition);                                                \
      failures++;                                                              \
    }                                                                          \
  } while (false)

using InitEnv = void(KYTY_SYSV_ABI *)(const Loader::EntryParams *);
using GetEnv = char *(KYTY_SYSV_ABI *)(const char *);
using SetEnv = int(KYTY_SYSV_ABI *)(const char *, const char *, int);

template <typename T>
T GetLibCFunction(const Loader::SymbolDatabase &symbols, const char *nid) {
  const auto *record = symbols.FindByNid(nid, Loader::SymbolType::Func);
  CHECK(record != nullptr);
  return record != nullptr ? reinterpret_cast<T>(record->vaddr) : nullptr;
}

struct EntryWithEnvironment {
  int argc;
  uint32_t pad;
  const char *argv[6];
};

bool Equals(const char *actual, const char *expected) {
  return actual != nullptr && std::strcmp(actual, expected) == 0;
}

void TestEnvironmentFromEntry(InitEnv init_env, GetEnv getenv) {
  EntryWithEnvironment entry{
      1, 0, {"eboot.bin", nullptr, "HOME=/app0", "EMPTY=", "NOVALUE", nullptr}};
  init_env(reinterpret_cast<const Loader::EntryParams *>(&entry));

  CHECK(Equals(getenv("HOME"), "/app0"));
  CHECK(Equals(getenv("EMPTY"), ""));
  CHECK(getenv("NOVALUE") == nullptr);
  CHECK(getenv("eboot.bin") == nullptr);
  CHECK(getenv("MISSING") == nullptr);
  CHECK(Equals(getenv("HOME=ignored"), "/app0"));
}

void TestInvalidNames(GetEnv getenv, SetEnv setenv) {
  *Posix::GetErrorAddr() = 0;
  CHECK(getenv(nullptr) == nullptr);
  CHECK(*Posix::GetErrorAddr() == Posix::POSIX_EINVAL);

  *Posix::GetErrorAddr() = 0;
  CHECK(getenv("") == nullptr);
  CHECK(*Posix::GetErrorAddr() == Posix::POSIX_EINVAL);

  *Posix::GetErrorAddr() = 0;
  CHECK(setenv("A=B", "value", 1) == -1);
  CHECK(*Posix::GetErrorAddr() == Posix::POSIX_EINVAL);
  CHECK(setenv("", "value", 1) == -1);
  CHECK(setenv(nullptr, "value", 1) == -1);
  CHECK(setenv("NAME", nullptr, 1) == -1);
}

void TestSetenv(GetEnv getenv, SetEnv setenv) {
  CHECK(setenv("KYTY_GUEST_ONLY", "first", 0) == 0);
  CHECK(Equals(getenv("KYTY_GUEST_ONLY"), "first"));

  CHECK(setenv("KYTY_GUEST_ONLY", "second", 0) == 0);
  CHECK(Equals(getenv("KYTY_GUEST_ONLY"), "first"));

  CHECK(setenv("KYTY_GUEST_ONLY", "third", 1) == 0);
  CHECK(Equals(getenv("KYTY_GUEST_ONLY"), "third"));

  CHECK(setenv("HOME", "/data", 1) == 0);
  CHECK(Equals(getenv("HOME"), "/data"));
}

void TestNullEntryClearsEnvironment(InitEnv init_env, GetEnv getenv) {
  init_env(nullptr);
  CHECK(getenv("HOME") == nullptr);
  CHECK(getenv("KYTY_GUEST_ONLY") == nullptr);
}

} // namespace

int main() {
  Loader::SymbolDatabase symbols;
  Libs::LibC::InitLibC_1(&symbols);
  const auto init_env = GetLibCFunction<InitEnv>(symbols, "bzQExy189ZI");
  const auto getenv = GetLibCFunction<GetEnv>(symbols, "smbQukfxYJM");
  const auto setenv = GetLibCFunction<SetEnv>(symbols, "M4YYbSFfJ8g");
  if (init_env == nullptr || getenv == nullptr || setenv == nullptr) {
    return 1;
  }
  TestEnvironmentFromEntry(init_env, getenv);
  TestInvalidNames(getenv, setenv);
  TestSetenv(getenv, setenv);
  TestNullEntryClearsEnvironment(init_env, getenv);
  return failures == 0 ? 0 : 1;
}
