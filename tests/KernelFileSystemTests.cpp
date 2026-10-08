#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "common/emulatorConfig.h"
#include "common/archive.h"
#include "common/file.h"
#include "common/hostException.h"
#include "common/platform/sysFileIO.h"
#include "common/virtualMemory.h"
#include "ArchiveTestFixture.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/stringUtils.h"
#include "graphics/presentation/window/windowInternal.h"
#include "kernel/eventQueue.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>
#include <atomic>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Libs::LibKernelApr {
void InitLibKernel_1_Apr(Loader::SymbolDatabase *symbols);
}

namespace Libs {
void InitLibKernel_1(Loader::SymbolDatabase *symbols);
}

namespace Libs::LibAmpr {
void InitAmpr_1(Loader::SymbolDatabase *symbols);
}

namespace Libs::LibNet {
void InitNet_1_Net(Loader::SymbolDatabase *symbols);
}

namespace Libs::LibNpWebApi2 {
void InitNet_1_NpWebApi2(Loader::SymbolDatabase *symbols);
}

namespace {

namespace FileSystem = Libs::LibKernel::FileSystem;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "KernelFileSystemTests: failed: %s\n", text);
    std::abort();
  }
}

class TempDirectory {
public:
  TempDirectory() {
    const auto unique =
        std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("kyty_kernel_file_system_" + std::to_string(unique));
    Check(std::filesystem::create_directories(m_path),
          "create temporary directory");
  }

  ~TempDirectory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  [[nodiscard]] const std::filesystem::path &Path() const { return m_path; }

  KYTY_CLASS_NO_COPY(TempDirectory);

private:
  std::filesystem::path m_path;
};

void CheckSaveRename(const std::filesystem::path &root,
                     std::string_view payload) {
  constexpr char Source[] = "/savedata0/STEMP000.DAT";
  constexpr char Target[] = "/savedata0/SDATA000.DAT";
  constexpr char Suffix[] = "-after-rename";

  const int fd = FileSystem::KernelOpen(Source, 0x601, 0777);
  Check(fd >= 3, "open temporary save file");
  Check(FileSystem::KernelWrite(fd, payload.data(), payload.size()) ==
            payload.size(),
        "write save payload");
  Check(FileSystem::KernelRename(Source, Target) == OK,
        "rename open save file");
  Check(FileSystem::KernelWrite(fd, Suffix, sizeof(Suffix) - 1) ==
            sizeof(Suffix) - 1,
        "write through renamed descriptor");
  Check(FileSystem::KernelClose(fd) == OK, "close renamed descriptor");

  Common::File result(root / "SDATA000.DAT", Common::File::Mode::Read);
  Check(!result.IsInvalid(), "open renamed save file");
  const auto data = result.ReadWholeBuffer();
  const std::string expected = std::string(payload) + Suffix;
  Check(data.size() == expected.size(), "renamed save size");
  Check(std::memcmp(data.data(), expected.data(), expected.size()) == 0,
        "renamed save contents");
}

void TestSaveOpenVisibility() {
  constexpr char Path[] = "/savedata0/visible-save.dat";
  constexpr char Payload[] = "saved progress";

  const int fd = FileSystem::KernelOpen(Path, 0xa01, 0777);
  Check(fd >= 3, "create save file exclusively");
  FileSystem::FileStat stat {};
  Check(FileSystem::KernelStat(Path, &stat) == OK && stat.st_size == 0,
        "created save file is visible before close");
  Check(FileSystem::KernelOpen(Path, 0xa01, 0777) ==
            Libs::LibKernel::KERNEL_ERROR_EEXIST,
        "exclusive creation detects an open save file");
  Check(FileSystem::KernelWrite(fd, Payload, sizeof(Payload) - 1) ==
            sizeof(Payload) - 1,
        "populate save file before truncation");
  Check(FileSystem::KernelClose(fd) == OK, "close populated save file");
  Check(FileSystem::KernelStat(Path, &stat) == OK &&
            stat.st_size == sizeof(Payload) - 1,
        "save file contains the truncation fixture");

  const int truncated = FileSystem::KernelOpen(Path, 0x401, 0777);
  Check(truncated >= 3, "truncate existing save file");
  Check(FileSystem::KernelStat(Path, &stat) == OK && stat.st_size == 0,
        "save truncation is visible before close");
  Check(FileSystem::KernelClose(truncated) == OK, "close truncated save file");
}

// Guest pages the emulator write-protects make a direct host read stop early instead of
// faulting. This handler stands in for the emulator's: it unprotects the guarded pages.
std::atomic<uint64_t> g_guard_base {0};
std::atomic<uint64_t> g_guard_size {0};
std::atomic<int> g_guard_faults {0};

bool UnprotectGuard(const Common::HostException::ExceptionInfo &info) {
  const auto base = g_guard_base.load();
  const auto size = g_guard_size.load();
  if (info.type != Common::HostException::ExceptionType::AccessViolation ||
      info.access_violation_vaddr < base || info.access_violation_vaddr - base >= size) {
    return false;
  }
  g_guard_faults.fetch_add(1);
  return Common::VirtualMemory::Protect(base, size, Common::VirtualMemory::Mode::ReadWrite);
}

void TestReadsIntoProtectedPages() {
  constexpr uint64_t Half = 0x10000;
  std::vector<uint8_t> payload(Half * 2);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i * 7 + i / 251);
  }
  const int fd = FileSystem::KernelOpen("/savedata0/protected-read.dat", 0x602, 0777);
  Check(fd >= 3 && FileSystem::KernelWrite(fd, payload.data(), payload.size()) ==
                       static_cast<int64_t>(payload.size()),
        "create protected-read fixture");
  const auto memory = Common::VirtualMemory::Alloc(0, Half * 2,
                                                   Common::VirtualMemory::Mode::ReadWrite);
  Check(memory != 0 && Common::HostException::InstallHandler(UnprotectGuard),
        "allocate the read target and install the guard handler");
  g_guard_base = memory + Half;
  g_guard_size = Half;
  // The emulator registers GPU-mapped guest memory; here only the guarded target qualifies.
  static uint64_t recoverable_base = 0;
  recoverable_base = memory;
  SysFileSetRecoverableDestination([](const void *data, uint64_t size) {
    const auto address = reinterpret_cast<uint64_t>(data);
    return address >= recoverable_base && address - recoverable_base <= 0x20000 - size;
  });
  auto *target = reinterpret_cast<uint8_t *>(memory);
  const auto guard = [&] {
    std::memset(target, 0, Half * 2);
    g_guard_faults = 0;
    return Common::VirtualMemory::Protect(memory + Half, Half, Common::VirtualMemory::Mode::Read);
  };
  const auto filled = [&] {
    return g_guard_faults.load() > 0 &&
           std::memcmp(target, payload.data(), payload.size()) == 0;
  };
  Check(guard() && FileSystem::KernelPread(fd, target, payload.size(), 0) ==
                       static_cast<int64_t>(payload.size()) && filled(),
        "pread fills a buffer that crosses into a write-protected page");
  Check(guard() && FileSystem::KernelLseek(fd, 0, 0) == 0 &&
            FileSystem::KernelRead(fd, target, payload.size()) ==
                static_cast<int64_t>(payload.size()) &&
            filled() && FileSystem::KernelLseek(fd, 0, 1) == static_cast<int64_t>(payload.size()),
        "read fills a write-protected tail and advances by the full count");
  FileSystem::KernelIovec iov[2] = {{target, Half / 2}, {target + Half / 2, Half * 2 - Half / 2}};
  Check(guard() && FileSystem::KernelPreadv(fd, iov, 2, 0) ==
                       static_cast<int64_t>(payload.size()) && filled(),
        "preadv fills vectors that cross into a write-protected page");
  // Only the bytes before EOF are probed: the request tail past the recoverable
  // range is unused.
  Check(guard() &&
            FileSystem::KernelPread(fd, target + Half, Half + 0x1000, Half) ==
                static_cast<int64_t>(Half) &&
            g_guard_faults.load() > 0 &&
            std::memcmp(target + Half, payload.data() + Half, Half) == 0,
        "pread past EOF recovers the protected bytes the file still supplies");
#if defined(__linux__)
  // A destination the emulator cannot fault in is not recovered: the read stays short.
  const long page = sysconf(_SC_PAGESIZE);
  void *pages = mmap(nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(pages != MAP_FAILED && munmap(static_cast<char *>(pages) + page, page) == 0,
        "map a read target whose second page is unmapped");
  Check(FileSystem::KernelPread(fd, pages, page * 2, 0) == page &&
            std::memcmp(pages, payload.data(), page) == 0,
        "pread into a partly unmapped buffer returns the mapped prefix");
  munmap(pages, page);
#endif
  SysFileSetRecoverableDestination(nullptr);
  g_guard_size = 0;
  Common::VirtualMemory::Free(memory);
  Check(FileSystem::KernelClose(fd) == OK &&
            FileSystem::KernelUnlink("/savedata0/protected-read.dat") == OK,
        "remove protected-read fixture");
}

void TestAioBatches() {
  namespace Kernel = Libs::LibKernel;
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto find = [&](const char *nid) {
    const auto *symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "AIO exports resolve");
    return symbol->vaddr;
  };
  struct Result { int64_t return_value; uint32_t state; };
  struct Request {
    int64_t offset;
    size_t size;
    void *buffer;
    Result *result;
    int32_t fd;
  };
  using Submit = int (KYTY_SYSV_ABI *)(Request *, int32_t, int32_t, int32_t *);
  using Batch = int (KYTY_SYSV_ABI *)(int32_t *, int32_t, int32_t *);
  using Single = int (KYTY_SYSV_ABI *)(int32_t, int32_t *);
  using Wait = int (KYTY_SYSV_ABI *)(int32_t, int32_t *, uint32_t *);
  using WaitBatch = int (KYTY_SYSV_ABI *)(int32_t *, int32_t, int32_t *, uint32_t, uint32_t *);
  const auto submit = reinterpret_cast<Submit>(find("HgX7+AORI58"));
  const auto poll = reinterpret_cast<Batch>(find("o7O4z3jwKzo"));
  const auto erase = reinterpret_cast<Batch>(find("Ft3EtsZzAoY"));
  const auto poll_one = reinterpret_cast<Single>(find("2pOuoWoCxdk"));
  const auto erase_one = reinterpret_cast<Single>(find("5TgME6AYty4"));
  const auto wait = reinterpret_cast<Wait>(find("KOF-oJbQVvc"));
  const auto wait_batch = reinterpret_cast<WaitBatch>(find("lgK+oIWkJyA"));
  constexpr char Payload[] = "AIO payload";
  const int fd = FileSystem::KernelOpen("/savedata0/aio.dat", 0x602, 0777);
  Check(fd >= 3 && FileSystem::KernelWrite(fd, Payload, sizeof(Payload)) == sizeof(Payload),
        "create AIO read fixture");
  std::array<int32_t, 3> ids {};
  std::array<char, 4> buffer {};
  Result result {};
  Request request {2, buffer.size(), buffer.data(), &result, fd};
  for (int i = 0; i < 2; ++i) {
    Check(submit(&request, 1, 2, &ids[i]) == OK &&
              result.return_value == buffer.size() && result.state == 3 &&
              std::memcmp(buffer.data(), Payload + 2, buffer.size()) == 0,
          "AIO submission reads bytes at the requested offset");
  }
  ids[2] = -1;
  std::array<int32_t, 3> states {-1, -1, -1};
  uint32_t timeout = 0;
  Check(wait_batch(ids.data(), ids.size(), states.data(), 2, &timeout) == OK &&
            states[0] == 3 && states[1] == 3 && states[2] == Kernel::KERNEL_ERROR_ESRCH,
        "OR wait observes completed reads and invalid-ID errors even with a zero timeout");
  Check(poll_one(ids[0], &states[0]) == OK && states[0] == (3 | 0x10000) &&
            poll(ids.data(), 2, states.data()) == OK &&
            states[0] == (3 | 0x10000) && states[1] == (3 | 0x10000) &&
            wait(ids[0], &states[0], nullptr) == OK && states[0] == (3 | 0x10000),
        "single and batch polls and waits share completion notification state");
  timeout = 1000000;
  Check(wait_batch(ids.data(), ids.size(), states.data(), 1, &timeout) == OK &&
            states[0] == (3 | 0x10000) && states[1] == (3 | 0x10000) &&
            states[2] == Kernel::KERNEL_ERROR_ESRCH && timeout <= 1000000 &&
            wait_batch(ids.data(), ids.size(), states.data(), 2, nullptr) == OK &&
            wait_batch(ids.data(), 1, states.data(), 0, nullptr) == OK,
        "AND and OR waits return when all IDs are terminal and one-ID waits ignore mode");
  Check(erase(ids.data(), ids.size(), states.data()) == OK &&
            states[0] == OK && states[1] == OK && states[2] == Kernel::KERNEL_ERROR_ESRCH,
        "batch deletion writes per-request results");
  Check(poll_one(ids[0], &states[0]) == OK && states[0] == Kernel::KERNEL_ERROR_ESRCH &&
            erase_one(ids[1], &states[1]) == OK && states[1] == Kernel::KERNEL_ERROR_ESRCH,
        "deleted AIO IDs are invalid for single-request APIs");
  for (const auto batch : {poll, erase}) {
    states.fill(42);
    Check(batch(nullptr, 1, states.data()) == Kernel::KERNEL_ERROR_EFAULT &&
              batch(ids.data(), 1, nullptr) == Kernel::KERNEL_ERROR_EFAULT &&
              batch(ids.data(), 0, states.data()) == Kernel::KERNEL_ERROR_EINVAL &&
              batch(ids.data(), 129, states.data()) == Kernel::KERNEL_ERROR_EINVAL &&
              states == std::array<int32_t, 3> {42, 42, 42},
          "invalid batch arguments do not modify outputs");
    std::array<int32_t, 128> invalid_ids {};
    std::array<int32_t, 128> errors {};
    Check(batch(invalid_ids.data(), invalid_ids.size(), errors.data()) == OK &&
              std::all_of(errors.begin(), errors.end(), [](int32_t error) {
                return error == Kernel::KERNEL_ERROR_ESRCH;
              }),
          "maximum-sized batch returns all invalid-ID errors");
  }
  states.fill(42);
  Check(wait_batch(nullptr, 1, states.data(), 1, nullptr) == Kernel::KERNEL_ERROR_EFAULT &&
            wait_batch(ids.data(), 1, nullptr, 1, nullptr) == Kernel::KERNEL_ERROR_EFAULT &&
            wait_batch(ids.data(), 0, states.data(), 1, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
            wait_batch(ids.data(), 129, states.data(), 1, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
            wait_batch(ids.data(), 2, states.data(), 0, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
            wait_batch(ids.data(), 2, states.data(), 3, nullptr) == Kernel::KERNEL_ERROR_EINVAL &&
            states == std::array<int32_t, 3> {42, 42, 42},
        "invalid wait arguments do not modify outputs");
  std::array<int32_t, 128> invalid_ids {};
  std::array<int32_t, 128> errors {};
  Check(wait_batch(invalid_ids.data(), invalid_ids.size(), errors.data(), 1, nullptr) == OK &&
            std::all_of(errors.begin(), errors.end(), [](int32_t error) {
              return error == Kernel::KERNEL_ERROR_ESRCH;
            }),
        "maximum-sized wait returns every invalid-ID error");
  Check(FileSystem::KernelClose(fd) == OK, "close AIO read fixture");
}

void TestNpWebApi2Memory() {
  Loader::SymbolDatabase symbols;
  Libs::LibNpWebApi2::InitNet_1_NpWebApi2(&symbols);
  const auto find = [&](const char *nid) {
    const auto *symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "NpWebApi2 library and memory exports resolve");
    return symbol->vaddr;
  };
  struct Stats { size_t pool, maximum, current; int32_t reserved; };
  static_assert(sizeof(Stats) == 32 && offsetof(Stats, reserved) == 24);
  using Initialize = int (KYTY_SYSV_ABI *)(int, size_t);
  using GetStats = int (KYTY_SYSV_ABI *)(int, Stats *);
  using Terminate = int (KYTY_SYSV_ABI *)(int);
  const auto initialize = reinterpret_cast<Initialize>(find("+o9816YQhqQ"));
  const auto get_stats = reinterpret_cast<GetStats>(find("Xweb+naPZ8Y"));
  const auto terminate = reinterpret_cast<Terminate>(find("bEvXpcEk200"));
  const int first = initialize(1, 65537);
  const int second = initialize(1, 16384);
  Stats stats {1, 2, 3, 4};
  Check(first > 0 && second > 0 && first != second &&
            get_stats(first, &stats) == OK && stats.pool == 81920 &&
            stats.maximum == 0 && stats.current == 0 && stats.reserved == 0 &&
            get_stats(second, &stats) == OK && stats.pool == 16384,
        "NpWebApi2 statistics retain each context's rounded pool capacity");
  Check(get_stats(first, nullptr) == static_cast<int32_t>(0x80553402) &&
            get_stats(0, &stats) == static_cast<int32_t>(0x80553403) &&
            initialize(1, std::numeric_limits<size_t>::max()) == static_cast<int32_t>(0x80553402),
        "NpWebApi2 statistics validate output, context ID, and pool rounding");
  Check(terminate(first) == OK &&
            get_stats(first, &stats) == static_cast<int32_t>(0x80553404) &&
            terminate(first) == static_cast<int32_t>(0x80553404) &&
            terminate(-1) == static_cast<int32_t>(0x80553403) &&
            get_stats(second, &stats) == OK && stats.pool == 16384 &&
            terminate(second) == OK,
        "NpWebApi2 termination removes only the selected library context");
}

void CheckMountRoot(const std::filesystem::path &root) {
  Common::File cache;
  Check(cache.Create(root / "rpf.cache"), "create directory listing fixture");
  cache.Close();
  FileSystem::Mount(root, "/app0");
  Check(FileSystem::GetRealFilename("/app0/rpf.cache") == root / "rpf.cache",
        "resolve mount descendant");
  Check(FileSystem::GetRealFilename("/app01/rpf.cache").empty(),
        "mount prefix must end at a path component");

  for (const char *path : {"/app0", "/app0/"}) {
    for (const int flags : {0, 0x00020000}) {
      const int fd = FileSystem::KernelOpen(path, flags, 0);
      Check(fd >= 3, "open mounted root with O_RDONLY or O_DIRECTORY");
      std::array<char, 512> entries {};
      const int size = FileSystem::KernelGetdents(fd, entries.data(), entries.size());
      Check(size > 0 && size <= entries.size(), "enumerate mounted root");
      bool found = false;
      for (int offset = 0; offset < size;) {
        // Directory record: inode, record length, type, name length, name.
        Check(size - offset >= 8, "directory record header fits");
        uint16_t length = 0;
        std::memcpy(&length, entries.data() + offset + 4, sizeof(length));
        const auto name_length = static_cast<uint8_t>(entries[offset + 7]);
        Check(length >= 8 + name_length + 1 && length <= size - offset,
              "directory record and name fit");
        if (std::string_view(entries.data() + offset + 8, name_length) == "rpf.cache") {
          Check(entries[offset + 6] == 8, "cache directory entry is a regular file");
          found = true;
        }
        offset += length;
      }
      Check(found, "mounted root listing contains rpf.cache");
      Check(FileSystem::KernelClose(fd) == OK, "close mounted root");
    }
  }
  FileSystem::Umount("/app0");
  Check(FileSystem::GetRealFilename("/app0/rpf.cache").empty(),
        "unmount by guest path");
  for (const auto &folder : {root, root / ""}) {
    for (const auto &host : {root, root / ""}) {
      FileSystem::Mount(folder, "/app0");
      FileSystem::Umount(Common::PathToGenericString(host));
      Check(FileSystem::GetRealFilename("/app0/rpf.cache").empty(),
            "unmount by host path with or without trailing separator");
    }
  }
}

void CheckUnmappedPaths(const std::filesystem::path &root) {
  const auto host_file = root / "host-only.dat";
  const auto host_path = Common::PathToGenericString(host_file);
  Common::File fixture;
  Check(fixture.Create(host_file), "create unmapped host file");
  fixture.Close();

  FileSystem::FileStat stat {};
  Check(FileSystem::GetRealFilename(host_path).empty() &&
            FileSystem::KernelOpen(host_path.c_str(), 0, 0) ==
                Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            FileSystem::KernelStat(host_path.c_str(), &stat) ==
                Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            FileSystem::KernelCheckReachability(host_path.c_str()) ==
                Libs::LibKernel::KERNEL_ERROR_ENOENT,
        "existing host files are absent from the guest namespace");
  Check(FileSystem::KernelUnlink(host_path.c_str()) ==
            Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            FileSystem::KernelRmdir(Common::PathToGenericString(root).c_str()) ==
                Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            std::filesystem::exists(host_file),
        "unmapped host files and directories cannot be removed");

  const auto missing = Common::PathToGenericString(root / "unmapped-create");
  Check(FileSystem::KernelOpen(missing.c_str(), 0x601, 0777) ==
            Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            FileSystem::KernelMkdir(missing.c_str(), 0777) ==
                Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            !std::filesystem::exists(root / "unmapped-create"),
        "creation requires a mounted guest destination");

  FileSystem::Mount(root, "/app0");
  Check(FileSystem::KernelRename("/app0/host-only.dat", missing.c_str()) ==
            Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            std::filesystem::exists(host_file) &&
            !std::filesystem::exists(root / "unmapped-create"),
        "rename to an unmapped destination preserves the source");
  FileSystem::Umount("/app0");
}

void CheckArchiveMount(const std::filesystem::path &root) {
  const auto archive = root / u8"game-日本語.zar";
  std::vector<uint8_t> payload(2 * 64 * 1024 + 33);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i * 37 + 11);
  }
  Check(ArchiveTests::CreateArchive(archive, payload), "create mounted archive fixture");
  const auto archive_root = Common::MakeArchivePath(archive);
  const auto host_member = archive_root / "assets/subdir/data.bin";
  constexpr char GuestMember[] = "/app0/assets/subdir/data.bin";
  FileSystem::Mount(archive_root, "/app0");
  Check(FileSystem::GetRealFilename(GuestMember) == host_member &&
            FileSystem::GetRealFilename("/app01/eboot.bin").empty(),
        "resolve archive mounts at path-component boundaries");
  Check(FileSystem::KernelOpen("/app0/../outside.bin", 0, 0) ==
            Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            FileSystem::KernelOpen("/app0/missing.bin", 0, 0) ==
            Libs::LibKernel::KERNEL_ERROR_ENOENT,
        "reject traversal outside an archive and missing members");

  FileSystem::FileStat path_stat{}, descriptor_stat{};
  Check(FileSystem::KernelStat(GuestMember, &path_stat) == OK &&
            path_stat.st_size == payload.size() &&
            path_stat.st_size == Common::File::Size(host_member) &&
            FileSystem::KernelCheckReachability(GuestMember) == OK,
        "archive path stat and reachability agree with Common::File");
  const int fd = FileSystem::KernelOpen("/app0/ASSETS/subdir/DATA.BIN", 0, 0);
  Check(fd >= 3 && FileSystem::KernelFstat(fd, &descriptor_stat) == OK &&
            descriptor_stat.st_size == path_stat.st_size &&
            descriptor_stat.st_mode == path_stat.st_mode,
        "case-insensitive archive open and descriptor stat agree");
  std::array<uint8_t, 97> bytes{};
  constexpr int64_t Offset = 64 * 1024 - 19;
  Check(FileSystem::KernelPread(fd, bytes.data(), bytes.size(), Offset) == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), payload.begin() + Offset) &&
            FileSystem::KernelLseek(fd, 0, 1) == 0,
        "archive pread crosses a compression block without moving position");
  Check(FileSystem::KernelLseek(fd, Offset, 0) == Offset &&
            FileSystem::KernelRead(fd, bytes.data(), bytes.size()) == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), payload.begin() + Offset) &&
            FileSystem::KernelLseek(fd, 0, 1) == Offset + bytes.size(),
        "archive seek and sequential read share descriptor position");
  Check(FileSystem::KernelLseek(fd, -9, 2) == payload.size() - 9 &&
            FileSystem::KernelRead(fd, bytes.data(), bytes.size()) == 9 &&
            std::equal(bytes.begin(), bytes.begin() + 9, payload.end() - 9) &&
            FileSystem::KernelRead(fd, bytes.data(), bytes.size()) == 0,
        "archive reads stop at the member boundary");
  Check(FileSystem::KernelWrite(fd, bytes.data(), 1) ==
            Libs::LibKernel::KERNEL_ERROR_EBADF &&
            FileSystem::KernelPwrite(fd, bytes.data(), 1, 0) ==
            Libs::LibKernel::KERNEL_ERROR_EBADF &&
            FileSystem::KernelFtruncate(fd, 0) ==
            Libs::LibKernel::KERNEL_ERROR_EBADF,
        "archive read-only descriptors reject write and truncate");

  const auto unicode_guest = std::string("/app0/") + std::string(ArchiveTests::UnicodeFilename);
  const int unicode = FileSystem::KernelOpen(unicode_guest.c_str(), 0, 0);
  Check(unicode >= 3 && FileSystem::KernelRead(unicode, bytes.data(), bytes.size()) ==
            ArchiveTests::Eboot.size() &&
            std::memcmp(bytes.data(), ArchiveTests::Eboot.data(), ArchiveTests::Eboot.size()) == 0,
        "open and read a Unicode archive member");
  Check(FileSystem::KernelClose(unicode) == OK, "close Unicode archive member");

  const int directory = FileSystem::KernelOpen("/app0/", 0x00020000, 0);
  Check(directory >= 3, "open mounted archive directory");
  std::array<char, 512> block{};
  const auto expected_entries = Common::File::GetDirEntries(archive_root);
  size_t entries_seen = 0;
  for (;;) {
    const int length = FileSystem::KernelGetdents(directory, block.data(), block.size());
    Check(length >= 0 && length <= block.size(), "read archive directory records");
    if (length == 0) {
      break;
    }
    for (size_t offset = 0; offset < static_cast<size_t>(length);) {
      uint16_t record_length = 0;
      Check(length - offset >= 8, "archive directory record header fits");
      std::memcpy(&record_length, block.data() + offset + 4, sizeof(record_length));
      const auto name_length = static_cast<uint8_t>(block[offset + 7]);
      Check(record_length >= 9 + name_length && record_length <= length - offset,
            "archive directory record fits");
      const std::string_view name(block.data() + offset + 8, name_length);
      Check(std::any_of(expected_entries.begin(), expected_entries.end(), [&](const auto &entry) {
        return entry.name == name && block[offset + 6] == (entry.is_file ? 8 : 4);
      }), "guest directory entry matches Common::File name and type");
      ++entries_seen;
      offset += record_length;
    }
  }
  Check(entries_seen == expected_entries.size(), "guest enumerates every archive directory entry");
  Check(FileSystem::KernelClose(directory) == OK, "close archive directory");

  for (const int flags : {1, 2, 0x0200, 0x0400}) {
    Check(FileSystem::KernelOpen(GuestMember, flags, 0777) ==
              Libs::LibKernel::KERNEL_ERROR_EROFS,
          "archive rejects write, create and truncate open flags");
  }
  Check(FileSystem::KernelUnlink(GuestMember) == Libs::LibKernel::KERNEL_ERROR_EROFS &&
            FileSystem::KernelMkdir("/app0/new-dir", 0777) == Libs::LibKernel::KERNEL_ERROR_EROFS &&
            FileSystem::KernelRmdir("/app0/assets") == Libs::LibKernel::KERNEL_ERROR_EROFS &&
            FileSystem::KernelRename(GuestMember, "/app0/renamed.bin") ==
                Libs::LibKernel::KERNEL_ERROR_EROFS,
        "archive mount rejects path mutations");
  FileSystem::Umount("/app0");
  Check(FileSystem::GetRealFilename(GuestMember).empty() &&
            FileSystem::KernelOpen(GuestMember, 0, 0) == Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            FileSystem::KernelLseek(fd, 0, 0) == 0 &&
            FileSystem::KernelRead(fd, bytes.data(), bytes.size()) == bytes.size() &&
            std::equal(bytes.begin(), bytes.end(), payload.begin()),
        "unmount hides archive paths while open descriptors retain the reader");
  Check(FileSystem::KernelClose(fd) == OK, "close last archive descriptor");
}

void CheckUnicodePaths(const std::filesystem::path &root) {
  constexpr std::string_view HostDirectory =
      "Test\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
  constexpr std::string_view GuestFilename =
      "asset-\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.bin";

  const auto unicode_root =
      root / Common::PathFromUtf8(HostDirectory);
  const auto nested_root = unicode_root / "nested";

  Check(Common::File::CreateDirectories(nested_root),
        "create nested Unicode host directory");

  CheckMountRoot(nested_root);

  const auto native_file =
      nested_root / Common::PathFromUtf8(GuestFilename);

  Common::File fixture;
  Check(fixture.Create(native_file), "create Unicode filename");
  fixture.Close();

  const auto entries = Common::File::GetDirEntries(nested_root);
  Check(std::any_of(entries.begin(), entries.end(), [&](const auto &entry) {
          return entry.is_file && entry.name == GuestFilename;
        }), "directory enumeration returns UTF-8 filenames");

  FileSystem::Mount(nested_root, "/app0");

  const auto guest_file =
      std::string("/app0/") + std::string(GuestFilename);

  Check(FileSystem::GetRealFilename(guest_file) == native_file,
        "resolve Unicode guest path");

  const int fd = FileSystem::KernelOpen(guest_file.c_str(), 0, 0);
  Check(fd >= 3, "open Unicode guest path");
  Check(FileSystem::KernelClose(fd) == OK,
        "close Unicode guest path");

  FileSystem::Umount("/app0");
}

void CheckUnicodeLogPath(const std::filesystem::path &root) {
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::File;
  options.printf_output_file = root / u8"logs-\u65e5\u672c\u8a9e-\U0001f600" / u8"log-\u00e9.txt";
  Config::Load(options);
  Log::Initialize();
  constexpr std::string_view Payload = "Unicode log path\n";
  Log::Write(Payload);
  Log::Shutdown();

  Common::File result(options.printf_output_file, Common::File::Mode::Read);
  Check(!result.IsInvalid(), "open Unicode log file");
  const auto data = result.ReadWholeBuffer();
  Check(data.size() == Payload.size() &&
            std::memcmp(data.data(), Payload.data(), data.size()) == 0,
        "Unicode log file contains output");
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  Log::Initialize();
}

void CheckDirectoryStream(const std::filesystem::path &root) {
  const auto directory = root / "directory-stream";
  Check(std::filesystem::create_directory(directory),
        "create seek fixture directory");
  for (int i = 0; i < 48; ++i) {
    Common::File fixture;
    Check(fixture.Create(directory / ("directory-entry-" + std::to_string(i))),
          "create enough entries to cross directory blocks");
    fixture.Close();
  }
  FileSystem::Mount(directory, "/app0");
  const int fd = FileSystem::KernelOpen("/app0/", 0, 0);
  Check(fd >= 3, "open directory as read-only asset");
  const auto end = FileSystem::KernelLseek(fd, 0, 2);
  Check(end > 512 && end % 512 == 0,
        "directory SEEK_END uses padded stream size");
  FileSystem::FileStat stat{};
  Check(FileSystem::KernelFstat(fd, &stat) == OK && stat.st_size == end &&
            stat.st_blksize == 512,
        "directory stat agrees with seek and enumeration");
  Check(FileSystem::KernelStat("/app0/", &stat) == OK && stat.st_size == end &&
            stat.st_blocks == end / 512,
        "path and descriptor directory stat agree");
  Check(FileSystem::KernelLseek(fd, 0, 0) == 0,
        "rewind directory for asset read");
  std::vector<char> raw(static_cast<size_t>(end));
  Check(FileSystem::KernelRead(fd, raw.data(), raw.size()) == end &&
            FileSystem::KernelRead(fd, raw.data(), 1) == 0,
        "raw directory read reaches EOF");
  Check(FileSystem::KernelLseek(fd, -end, 1) == 0,
        "directory SEEK_CUR uses the position advanced by read");

  std::array<char, 512> block{};
  int64_t base = -1;
  for (int64_t offset = 0; offset < end; offset += block.size()) {
    Check(FileSystem::KernelGetdirentries(fd, block.data(), block.size(),
                                          &base) == block.size() &&
              base == offset &&
              std::memcmp(block.data(), raw.data() + offset, block.size()) == 0,
          "directory enumeration shares raw bytes and reports each block "
          "position");
    for (size_t pos = 0; pos < block.size();) {
      Check(block.size() - pos >= 8, "directory record header fits");
      uint16_t length = 0;
      std::memcpy(&length, block.data() + pos + 4, sizeof(length));
      const auto name_length = static_cast<uint8_t>(block[pos + 7]);
      Check(length >= 9 + name_length && length <= block.size() - pos &&
                block[pos + 8 + name_length] == '\0',
            "directory entries remain complete within every block");
      pos += length;
    }
  }
  Check(FileSystem::KernelGetdirentries(fd, block.data(), block.size(),
                                        &base) == 0 &&
            base == end,
        "directory enumeration reports EOF position");
  Check(FileSystem::KernelLseek(fd, 512, 0) == 512 &&
            FileSystem::KernelGetdirentries(fd, block.data(), block.size(),
                                            &base) == block.size() &&
            base == 512 &&
            std::memcmp(block.data(), raw.data() + 512, block.size()) == 0,
        "restore and reread a directory enumeration position");

  const auto position = FileSystem::KernelLseek(fd, 0, 1);
  Check(
      FileSystem::KernelLseek(fd, 0, 9) ==
              Libs::LibKernel::KERNEL_ERROR_EINVAL &&
          FileSystem::KernelLseek(fd, -1, 0) ==
              Libs::LibKernel::KERNEL_ERROR_EINVAL &&
          FileSystem::KernelLseek(fd, std::numeric_limits<int64_t>::min(), 1) ==
              Libs::LibKernel::KERNEL_ERROR_EINVAL &&
          FileSystem::KernelLseek(fd, std::numeric_limits<int64_t>::max(), 1) ==
              Libs::LibKernel::KERNEL_ERROR_EOVERFLOW &&
          FileSystem::KernelLseek(fd, 0, 1) == position,
      "invalid and overflowing directory seeks preserve the position");
  Check(FileSystem::KernelLseek(fd, -19, 2) == end - 19 &&
            FileSystem::KernelRead(fd, block.data(), block.size()) == 19 &&
            std::memcmp(block.data(), raw.data() + end - 19, 19) == 0,
        "raw directory reads support byte positions and stop at EOF");
  Check(FileSystem::KernelLseek(fd, 1, 0) == 1 &&
            FileSystem::KernelGetdents(fd, block.data(), block.size()) ==
                Libs::LibKernel::KERNEL_ERROR_EINVAL &&
            FileSystem::KernelLseek(fd, 0, 1) == 1,
        "directory enumeration rejects an incomplete record position");
  Check(FileSystem::KernelLseek(fd, 512, 2) == end + 512 &&
            FileSystem::KernelGetdirentries(fd, block.data(), block.size(),
                                            &base) ==
                Libs::LibKernel::KERNEL_ERROR_EINVAL &&
            FileSystem::KernelLseek(fd, 0, 1) == end + 512,
        "directory enumeration rejects a position beyond EOF");
  Check(FileSystem::KernelRead(
            fd, block.data(),
            static_cast<size_t>(std::numeric_limits<int>::max()) + 1) ==
                Libs::LibKernel::KERNEL_ERROR_EINVAL &&
            FileSystem::KernelLseek(fd, 0, 1) == end + 512,
        "oversized read fails without changing the directory position");
  Check(FileSystem::KernelClose(fd) == OK, "close directory stream");
  Check(FileSystem::KernelLseek(fd, 0, 0) ==
            Libs::LibKernel::KERNEL_ERROR_EBADF,
        "seek rejects a closed descriptor");
  FileSystem::Umount("/app0");
}

void CheckAmprOrdering(Loader::SymbolDatabase &symbols, uint32_t file_id) {
  Libs::LibAmpr::InitAmpr_1(&symbols);
  const auto find = [&](const char *nid) {
    const auto *symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "AMPR command and submission exports resolve");
    return symbol->vaddr;
  };
  using Unary = void (KYTY_SYSV_ABI *)(void *);
  using AprConstructor = void (KYTY_SYSV_ABI *)(void *, void *, void *);
  using SetBuffer = int (KYTY_SYSV_ABI *)(void *, void *, uint32_t);
  using Offset = uint64_t (KYTY_SYSV_ABI *)(void *);
  using WaitAddress = int (KYTY_SYSV_ABI *)(void *, volatile uint64_t *, uint64_t,
                                          uint8_t, uint8_t);
  using WriteAddress = int (KYTY_SYSV_ABI *)(void *, volatile uint64_t *, uint64_t);
  using ReadFile = int (KYTY_SYSV_ABI *)(void *, uint64_t, uint64_t, uint32_t,
                                       void *, uint64_t, uint64_t);
  using WriteKernelEvent = int (KYTY_SYSV_ABI *)(void *, uint64_t, uint64_t, uint64_t,
                                               uint64_t, uint64_t);
  struct Result { int32_t result; uint32_t error_offset; };
  using SubmitApr = int (KYTY_SYSV_ABI *)(void *, uint32_t, Result *, uint32_t *);
  using SubmitAmm = int (KYTY_SYSV_ABI *)(void *, uint32_t, uint32_t, uint32_t *);
  using WaitSubmission = int (KYTY_SYSV_ABI *)(uint32_t);
  const auto construct = reinterpret_cast<Unary>(find("8aI7R7WaOlc"));
  const auto construct_apr = reinterpret_cast<AprConstructor>(find("a8uLzYY--tM"));
  const auto construct_amm = reinterpret_cast<Unary>(find("EDq5bqCqYpA"));
  const auto destroy = reinterpret_cast<Unary>(find("GuchCTefuZw"));
  const auto set_buffer = reinterpret_cast<SetBuffer>(find("N-FSPA4S3nI"));
  const auto offset = reinterpret_cast<Offset>(find("GnxKOHEawhk"));
  const auto wait_address = reinterpret_cast<WaitAddress>(find("DLfoNxTFNVk"));
  const auto write_address = reinterpret_cast<WriteAddress>(find("sJXyWHjP-F8"));
  const auto read_file = reinterpret_cast<ReadFile>(find("mQ16-QdKv7k"));
  const auto write_event = reinterpret_cast<WriteKernelEvent>(find("H896Pt-yB4I"));
  const auto submit_apr = reinterpret_cast<SubmitApr>(find("ASoW5WE-UPo"));
  const auto submit_amm = reinterpret_cast<SubmitAmm>(find("NnKhlMJtIsI"));
  const auto wait_apr = reinterpret_cast<WaitSubmission>(find("rqwFKI4PAiM"));
  const auto wait_amm = reinterpret_cast<WaitSubmission>(find("HXymib4T8gc"));
  struct Buffer {
    std::array<uint64_t, 5> header {};
    std::array<uint32_t, 256> data {};
  };
  std::array<Buffer, 4> buffers;
  const auto reset = [&](size_t apr_count) {
    for (size_t i = 0; i < buffers.size(); ++i) {
      auto &buffer = buffers[i];
      construct(buffer.header.data());
      if (i < apr_count) {
        construct_apr(buffer.header.data(), &buffer.header[3], &buffer.header[4]);
      } else {
        construct_amm(buffer.header.data());
      }
      Check(set_buffer(buffer.header.data(), buffer.data.data(), sizeof(buffer.data)) == OK,
            "initialize AMPR command buffer");
    }
  };
  // WaitCompare order: ==, unsigned >/<, !=, wrapped >=, signed >/<.
  struct Comparison { uint8_t compare; uint64_t blocked, reference, released; };
  constexpr std::array comparisons {
      Comparison{0, 1, 2, 2}, Comparison{1, 0x40000000000019c3, 0x40000000000019c3,
                                   0x40000000000019c4},
      Comparison{1, 0, INT64_MAX, uint64_t{1} << 63},
      Comparison{2, UINT64_MAX, uint64_t{1} << 63, INT64_MAX}, Comparison{3, 2, 2, 3},
      Comparison{4, UINT64_MAX - 1, UINT64_MAX, 0},
      Comparison{5, UINT64_MAX, 0, 1}, Comparison{6, 0, 0, UINT64_MAX}};
  for (const auto &comparison : comparisons) {
    reset(3);
    uint64_t fence = comparison.blocked;
    uint64_t blocked_done = 0, read_done = 0, lower_done = 0;
    std::array<char, 3> output {};
    std::array<Result, 3> results {{{1234, 5678}, {1234, 5678}, {1234, 5678}}};
    std::array<uint32_t, 4> ids {};
    auto *blocked = buffers[0].header.data();
    auto *reader = buffers[1].header.data();
    auto *lower = buffers[2].header.data();
    auto *producer = buffers[3].header.data();
    Check(wait_address(blocked, &fence, comparison.reference, comparison.compare, 0) == OK &&
              write_address(blocked, &blocked_done, 1) == OK &&
              read_file(reader, reinterpret_cast<uint64_t>(&buffers[1].header[3]),
                        reinterpret_cast<uint64_t>(&buffers[1].header[4]), file_id,
                        output.data(), output.size(), 0) == OK &&
              write_address(reader, &read_done, 2) == OK &&
              write_address(lower, &lower_done, 3) == OK &&
              write_address(producer, &fence, comparison.released) == OK,
          "build dependent APR read and later AMM fence producer");
    Check(submit_apr(blocked, 3, &results[0], &ids[0]) == OK &&
              submit_apr(reader, 3, &results[1], &ids[1]) == OK &&
              submit_apr(lower, 4, &results[2], &ids[2]) == OK && wait_apr(ids[2]) == OK,
          "APR submit returns while blocked and a lower priority completes");
    Check(lower_done == 3 && blocked_done == 0 && read_done == 0 &&
              output == std::array<char, 3>{} && results[0].result == 1234 &&
              results[1].result == 1234,
          "unsatisfied wait blocks later buffers at its priority without publishing completion");
    Check(submit_amm(buffers[3].data.data(), static_cast<uint32_t>(offset(producer)), 1,
                     &ids[3]) == OK && wait_amm(ids[3]) == OK &&
              wait_apr(ids[0]) == OK && wait_apr(ids[1]) == OK,
          "later AMM submission releases the APR dependency");
    Check(blocked_done == 1 && read_done == 2 && std::memcmp(output.data(), "APR", 3) == 0,
          "APR reads real file bytes only after its dependency completes");
    for (const auto &result : results) {
      Check(result.result == OK,
            "submission wait observes the completed result");
    }
  }
  reset(1);
  namespace EventQueue = Libs::LibKernel::EventQueue;
  EventQueue::KernelEqueue queue = EventQueue::KERNEL_EQUEUE_INVALID;
  uint64_t ampr_user_data = 1, user_data = 2;
  Check(EventQueue::KernelCreateEqueue(&queue, "apr-completion") == OK &&
            EventQueue::KernelAddAmprEvent(queue, 42, &ampr_user_data) == OK &&
            EventQueue::KernelAddUserEventEdge(queue, 42) == OK,
        "AMPR and user events can share an identifier");
  std::array<char, 3> output {};
  Result event_result {1234, 5678};
  uint32_t event_submission = 0;
  auto *reader = buffers[0].header.data();
  Check(read_file(reader, reinterpret_cast<uint64_t>(&buffers[0].header[3]),
                  reinterpret_cast<uint64_t>(&buffers[0].header[4]), file_id,
                  output.data(), output.size(), 0) == OK &&
            write_event(reader, queue, 42, 0x123456789abc, 0, 0) == OK &&
            submit_apr(reader, 3, &event_result, &event_submission) == OK &&
            wait_apr(event_submission) == OK && event_result.result == OK,
        "APR read submits its completion event");
  std::array<EventQueue::KernelEvent, 2> events {};
  int event_count = 0;
  const Libs::LibKernel::KernelUseconds poll = 0;
  Check(EventQueue::KernelWaitEqueue(queue, events.data(), 2, &event_count, &poll) == OK &&
            event_count == 1 && events[0].ident == 42 && events[0].filter == -25 &&
            events[0].data == 0x123456789abc && events[0].udata == &ampr_user_data &&
            std::memcmp(output.data(), "APR", 3) == 0,
        "APR completion reports the AMPR filter, payload and registration user data");
  Check(EventQueue::KernelDeleteAmprEvent(queue, 42) == OK &&
            EventQueue::KernelTriggerEvent(queue, 42, -25, nullptr) ==
                Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            EventQueue::KernelTriggerUserEvent(queue, 42, &user_data) == OK &&
            EventQueue::KernelWaitEqueue(queue, events.data(), 2, &event_count, &poll) == OK &&
            event_count == 1 && events[0].filter == EventQueue::KERNEL_EVFILT_USER &&
            events[0].udata == &user_data,
        "deleting AMPR leaves the user event with the same identifier intact");
  Check(EventQueue::KernelDeleteEqueue(queue) == OK, "delete APR completion queue");
  reset(0);
  uint64_t cpu_fence = 0, amm_done = 0, lower_done = 0;
  std::array<uint32_t, 2> ids {};
  Check(wait_address(buffers[0].header.data(), &cpu_fence, 0, 1, 0) == OK &&
            write_address(buffers[0].header.data(), &amm_done, 1) == OK &&
            write_address(buffers[1].header.data(), &lower_done, 1) == OK &&
            submit_amm(buffers[0].data.data(), static_cast<uint32_t>(offset(buffers[0].header.data())),
                       0, &ids[0]) == OK &&
            submit_amm(buffers[1].data.data(), static_cast<uint32_t>(offset(buffers[1].header.data())),
                       1, &ids[1]) == OK && wait_amm(ids[1]) == OK,
        "AMM lower priority progresses while its high priority waits");
  Check(lower_done == 1 && amm_done == 0, "AMM wait preserves its dependency");
  std::atomic_ref(cpu_fence).store(1, std::memory_order_release);
  Check(wait_amm(ids[0]) == OK && amm_done == 1,
        "AMM observes an external CPU fence store without a submission notification");
  for (auto &buffer : buffers) {
    destroy(buffer.header.data());
  }
}

void CheckAprPaths(const std::filesystem::path &root) {
  Loader::SymbolDatabase symbols;
  Libs::LibKernelApr::InitLibKernel_1_Apr(&symbols);
  const auto *resolve_symbol = symbols.FindByNid("w5fcCG+t31g", Loader::SymbolType::Func);
  const auto *each_symbol = symbols.FindByNid("C+Khtbbx2g8", Loader::SymbolType::Func);
  Check(resolve_symbol && each_symbol, "APR path exports are registered");
  using Resolve = int (KYTY_SYSV_ABI *)(const char *, const char *const *, uint32_t,
                                      uint32_t *, uint64_t *, uint32_t *);
  using ResolveEach = int (KYTY_SYSV_ABI *)(const char *, const char *const *, uint32_t,
                                          uint32_t *, uint64_t *, int *);
  const auto resolve = reinterpret_cast<Resolve>(resolve_symbol->vaddr);
  const auto resolve_each = reinterpret_cast<ResolveEach>(each_symbol->vaddr);
  Common::File fixture;
  Check(fixture.Create(root / "apr.dat"), "create APR fixture");
  fixture.Write("APR", 3);
  fixture.Close();
  FileSystem::Mount(root, "/app0");

  uint32_t expected_id = 0xffffffffu;
  for (const auto &parts : {std::array{"", "/app0/apr.dat"},
                           std::array{"/app0/", "apr.dat"},
                           std::array{"/", "app0/apr.dat"},
                           std::array{"/app", "0/apr.dat"}}) {
    uint32_t id = 0xffffffffu, error_index = 0xffffffffu;
    uint64_t size = 0;
    Check(resolve(parts[0], &parts[1], 1, &id, &size, &error_index) == OK &&
              id != 0xffffffffu && size == 3,
          "APR concatenates empty, one-character and partial-component prefixes");
    if (expected_id == 0xffffffffu) {
      expected_id = id;
    }
    Check(id == expected_id, "equivalent APR paths return the same ID");
  }

  const char *paths[] = {"/app0/missing.dat", "/app0/apr.dat"};
  uint32_t ids[2] = {}, error_index = 0xffffffffu;
  uint64_t sizes[2] = {1, 1};
  int results[2] = {};
  Check(resolve_each("", paths, 2, ids, sizes, results) == 1 &&
            results[0] == Libs::LibKernel::KERNEL_ERROR_ENOENT && results[1] == OK &&
            ids[0] == 0xffffffffu && ids[1] == expected_id && sizes[0] == 0 && sizes[1] == 3,
        "APR foreach reports a missing path and continues to the valid file");

  // PATH_MAX includes NUL; all components remain below NAME_MAX (255).
  std::string longest = "/app0/";
  for (int i = 0; i < 3; ++i) {
    longest += std::string(254, 'a') + '/';
  }
  longest += std::string(1023 - longest.size(), 'b');
  paths[0] = longest.c_str();
  Check(resolve("", paths, 1, ids, sizes, &error_index) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENOENT && error_index == 0,
        "APR accepts a pathname whose final NUL is at PATH_MAX minus one");
  Check(resolve("/", paths, 1, ids, sizes, &error_index) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENAMETOOLONG,
        "APR rejects concatenated paths exceeding PATH_MAX");
  std::array<char, 1024> unterminated;
  unterminated.fill('/');
  paths[0] = unterminated.data();
  Check(resolve("", paths, 1, ids, sizes, &error_index) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENAMETOOLONG,
        "APR rejects an unterminated pathname");
  paths[0] = "apr.dat";
  Check(resolve(unterminated.data(), paths, 1, ids, sizes, &error_index) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ENAMETOOLONG,
        "APR rejects an unterminated prefix");
  CheckAmprOrdering(symbols, expected_id);
  FileSystem::Umount("/app0");
}

std::array<int, 2> CreateTcpPair() {
  namespace Net = Libs::Network::Net;
  // Guest sockaddr_in: length, family, network-order port/address, padding.
  std::array<uint8_t, 16> address {16, 2, 0, 0, 127, 0, 0, 1};
  const int listener = Net::Socket(2, 1, 0);
  Check(listener >= 0, "create loopback listener");
  Check(Net::Bind(listener, address.data(), address.size()) == 0, "bind loopback");
  Check(Net::Listen(listener, 1) == 0, "listen on loopback");
  uint32_t address_size = address.size();
  Check(Net::Getsockname(listener, address.data(), &address_size) == 0,
        "get assigned loopback port");
  const int writer = Net::Socket(2, 1, 0);
  Check(writer >= 0 && Net::Connect(writer, address.data(), address_size) == 0,
        "connect wake socket");
  const int reader = Net::Accept(listener, nullptr, nullptr);
  Check(reader >= 0, "accept wake socket");
  Check(Net::SocketClose(listener) == 0, "close listener");
  return {reader, writer};
}

#if defined(_WIN32)
void CheckSocketReceiveConcurrency() {
  namespace Net = Libs::Network::Net;
  const auto [reader, writer] = CreateTcpPair();
  const int timeout_ms = 3000;
  Check(Net::Setsockopt(reader, 0xffff, 0x1006, &timeout_ms, sizeof(timeout_ms)) == 0,
        "bound the blocked receive concurrency fixture");
  std::promise<void> entered;
  std::promise<int64_t> completed;
  auto entered_future = entered.get_future();
  auto completed_future = completed.get_future();
  std::jthread receiver([&] {
    char byte = 0;
    entered.set_value();
    completed.set_value(Net::Recv(reader, &byte, 1, 0x42));
  });
  Check(entered_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
        "start blocked reader");
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  char byte = 0;
  const auto before_nonblocking = std::chrono::steady_clock::now();
  Check(Net::Recv(reader, &byte, 1, 0x80) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK &&
            std::chrono::steady_clock::now() - before_nonblocking < std::chrono::milliseconds(500) &&
            completed_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout,
        "DONTWAIT does not wait behind another blocked reader");
  const auto before_close = std::chrono::steady_clock::now();
  Check(Net::SocketClose(reader) == 0, "close succeeds while another thread receives");
  Check(std::chrono::steady_clock::now() - before_close < std::chrono::seconds(1),
        "close does not wait for the blocked receive timeout");
  Check(completed_future.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
        "close wakes the blocked receive within one second");
  Check(completed_future.get() <= 0, "closed receive reports EOF or error");
  Check(std::chrono::steady_clock::now() - before_close < std::chrono::seconds(1),
        "closing a socket wakes its blocked receive before the receive timeout");
  receiver.join();
  Check(Net::SocketClose(writer) == 0, "close blocked receive writer");

  const auto [shutdown_reader, shutdown_writer] = CreateTcpPair();
  Check(Net::Setsockopt(shutdown_reader, 0xffff, 0x1006, &timeout_ms, sizeof(timeout_ms)) == 0 &&
            Net::Send(shutdown_writer, "x", 1, 0) == 1 &&
            Net::Recv(shutdown_reader, &byte, 1, 0x42) == 1 && byte == 'x' &&
            Net::Shutdown(shutdown_reader, 0) == 0 &&
            Net::Recv(shutdown_reader, &byte, 1, 0) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_ESHUTDOWN,
        "receive shutdown rejects bytes retained by an earlier peek");
  Check(Net::SocketClose(shutdown_reader) == 0 && Net::SocketClose(shutdown_writer) == 0,
        "close receive shutdown fixture");
}

void CheckSocketReceiveBuffer(int reader, int writer) {
  namespace Net = Libs::Network::Net;
  constexpr int PeekWaitAll = 0x42;
  constexpr char Payload[] = "abcdef";
  const int timeout_ms = 2000;
  Check(Net::Setsockopt(reader, 0xffff, 0x1006, &timeout_ms, sizeof(timeout_ms)) == 0,
        "bound blocking receive regressions with SO_RCVTIMEO");
  Check(Net::Send(writer, Payload, 2, 0) == 2, "send first TCP fragment");
  std::jthread sender([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Check(Net::Send(writer, Payload + 2, 4, 0) == 4, "send second TCP fragment");
  });
  std::array<char, 8> received {};
  Check(Net::Recv(reader, received.data(), 6, PeekWaitAll) == 6 &&
            std::memcmp(received.data(), Payload, 6) == 0,
        "combined peek waits for fragmented TCP bytes");
  sender.join();
  received.fill(0);
  Check(Net::Recv(reader, received.data(), 6, PeekWaitAll) == 6 &&
            std::memcmp(received.data(), Payload, 6) == 0,
        "repeated combined peek preserves all bytes");

  const int epoll = Net::EpollCreate("buffered receive", 0);
  Net::NetEpollEvent registration {};
  registration.events = 1;
  registration.data.u64 = 0x12345678;
  Check(epoll >= 0 && Net::EpollControl(epoll, 1, reader, &registration) == 0,
        "register buffered socket readability");
  const auto check_readable = [&](bool expected) {
    std::array<uint64_t, 16> readable {};
    const auto bit = uint64_t {1} << (reader % 64);
    readable[reader / 64] = bit;
    const std::array<int64_t, 2> timeout {expected ? 1 : 0, 0};
    Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr, timeout.data()) ==
              (expected ? 1 : 0) && readable[reader / 64] == (expected ? bit : 0),
          "select reflects buffered bytes and their removal");
    if (expected) {
      auto writable = readable;
      Check(Net::Select(reader + 1, readable.data(), writable.data(), nullptr,
                        timeout.data()) == 2 && readable[reader / 64] == bit &&
                writable[reader / 64] == bit,
            "select counts readable and writable bits once each");
    }
    Net::NetEpollEvent event {};
    Check(Net::EpollWait(epoll, &event, 1, expected ? 1000000 : 0) ==
              (expected ? 1 : 0) &&
              (!expected || (event.events == 1 && event.data.u64 == registration.data.u64)),
          "epoll reflects buffered bytes and preserves registration data");
  };
  check_readable(true);
  std::array<uint8_t, 16> peer {}, expected_peer {};
  uint32_t peer_size = peer.size(), expected_peer_size = expected_peer.size();
  Check(Net::Getsockname(writer, expected_peer.data(), &expected_peer_size) == 0 &&
            Net::Recv(reader, received.data(), 0, PeekWaitAll) == 0 &&
            Net::Recvfrom(reader, received.data(), 2, 0, peer.data(), &peer_size) == 2 &&
            peer_size == expected_peer_size && peer == expected_peer &&
            std::memcmp(received.data(), Payload, 2) == 0 &&
            FileSystem::KernelRead(reader, received.data(), 2) == 2 &&
            std::memcmp(received.data(), Payload + 2, 2) == 0,
        "zero-length peek preserves bytes; recvfrom returns peer and shares kernel read buffer");
  check_readable(true);
  Check(Net::Send(writer, "gh", 2, 0) == 2 &&
            Net::Recv(reader, received.data(), 4, 0x40) == 4 &&
            std::memcmp(received.data(), "efgh", 4) == 0,
        "waitall joins buffered bytes with newly received bytes");
  check_readable(false);
  Check(Net::Send(writer, "mn", 2, 0) == 2, "send plain peek fixture");
  const auto before_peek = std::chrono::steady_clock::now();
  Check(Net::Recv(reader, received.data(), 4, 2) == 2 &&
            std::memcmp(received.data(), "mn", 2) == 0 &&
            std::chrono::steady_clock::now() - before_peek < std::chrono::milliseconds(500) &&
            Net::Recv(reader, received.data(), 4, 0) == 2 &&
            std::memcmp(received.data(), "mn", 2) == 0,
        "plain peek returns available bytes promptly and preserves them for ordinary receive");

  const int enabled = 1, disabled = 0;
  Check(Net::Setsockopt(reader, 0xffff, 0x1200, &enabled, sizeof(enabled)) == 0 &&
            Net::Recv(reader, received.data(), 4, PeekWaitAll) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "empty nonblocking combined peek returns would-block");
  Check(Net::Send(writer, "ij", 2, 0) == 2, "send nonblocking receive fixture");
  check_readable(true);
  *Libs::Posix::GetErrorAddr() = Libs::Posix::POSIX_EINVAL;
  for (int i = 0; i < 2; ++i) {
    Check(Net::Recv(reader, received.data(), 4, PeekWaitAll) == 2 &&
              std::memcmp(received.data(), "ij", 2) == 0 &&
              *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL,
          "nonblocking combined peek preserves a short result and successful errno");
  }
  Check(Net::Recv(reader, received.data(), 4, 0) == 2 &&
            std::memcmp(received.data(), "ij", 2) == 0,
        "ordinary receive consumes a short buffered result");
  check_readable(false);
  Check(Net::Setsockopt(reader, 0xffff, 0x1200, &disabled, sizeof(disabled)) == 0 &&
            Net::EpollDestroy(epoll) == 0,
        "restore blocking receives and destroy epoll");
  Check(Net::Recv(reader, received.data(), 4, PeekWaitAll | 0x80) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "per-call nonblocking combined peek returns would-block on a blocking socket");
  Check(Net::Send(writer, "kl", 2, 0) == 2 && Net::Shutdown(writer, 1) == 0,
        "send a final partial payload and EOF");
  for (int i = 0; i < 2; ++i) {
    Check(Net::Recv(reader, received.data(), 4, PeekWaitAll) == 2 &&
              std::memcmp(received.data(), "kl", 2) == 0,
          "EOF ends waitall while repeated peeks preserve the short payload");
  }
  Check(Net::Recv(reader, received.data(), 4, 0x40) == 2 &&
            std::memcmp(received.data(), "kl", 2) == 0 &&
            Net::Recv(reader, received.data(), 4, PeekWaitAll) == 0,
        "draining the final buffered payload exposes EOF");

  const int datagram = Net::Socket(2, 2, 0);
  const int datagram_writer = Net::Socket(2, 2, 0);
  std::array<uint8_t, 16> address {16, 2, 0, 0, 127, 0, 0, 1};
  uint32_t address_size = address.size();
  Check(datagram >= 0 && datagram_writer >= 0 &&
            Net::Bind(datagram, address.data(), address.size()) == 0 &&
            Net::Getsockname(datagram, address.data(), &address_size) == 0 &&
            Net::Setsockopt(datagram, 0xffff, 0x1006, &timeout_ms, sizeof(timeout_ms)) == 0 &&
            Net::Sendto(datagram_writer, Payload, 6, 0, address.data(), address_size) == 6,
        "create bounded UDP truncation fixture");
  *Libs::Posix::GetErrorAddr() = Libs::Posix::POSIX_EINVAL;
  Check(Net::Recvfrom(datagram, received.data(), 2, 0, nullptr, nullptr) == 2 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL &&
            std::memcmp(received.data(), Payload, 2) == 0 &&
            Net::Setsockopt(datagram, 0xffff, 0x1200, &enabled, sizeof(enabled)) == 0 &&
            Net::Recv(datagram, received.data(), received.size(), 0) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "UDP truncation returns its prefix and discards the rest of the datagram");
  Check(Net::SocketClose(datagram) == 0 && Net::SocketClose(datagram_writer) == 0,
        "close UDP truncation fixture");
}
#endif

void CheckSocketWakeup() {
  namespace Net = Libs::Network::Net;
  Loader::SymbolDatabase symbols;
  Libs::LibNet::InitNet_1_Net(&symbols);
  const auto *connect_symbol = symbols.Find(
      {"OXXX4mUk3uk", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  const auto *getsockopt_symbol = symbols.Find(
      {"xphrZusl78E", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  const auto *setsockopt_symbol = symbols.Find(
      {"2mKX2Spso7I", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  const auto *send_symbol = symbols.Find(
      {"beRjXBn-z+o", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  const auto *sendto_symbol = symbols.Find(
      {"gvD1greCu0A", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  const auto *recv_symbol = symbols.Find(
      {"9wO9XrMsNhc", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  const auto *recvfrom_symbol = symbols.Find(
      {"304ooNZxWDY", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  const auto *errno_symbol = symbols.Find(
      {"HQOwnfMGipQ", "Net", 1, "Net", 1, 1, Loader::SymbolType::Func});
  Check(connect_symbol && getsockopt_symbol && setsockopt_symbol && send_symbol && sendto_symbol &&
            recv_symbol && recvfrom_symbol && errno_symbol,
        "Net socket and errno exports resolve with the guest ABI versions");
  using Connect = int (KYTY_SYSV_ABI *)(int, const void *, uint32_t);
  using Getsockopt = int (KYTY_SYSV_ABI *)(int, int, int, void *, uint32_t *);
  using Setsockopt = int (KYTY_SYSV_ABI *)(int, int, int, const void *, uint32_t);
  using Send = int (KYTY_SYSV_ABI *)(int, const void *, size_t, int);
  using Sendto = int (KYTY_SYSV_ABI *)(int, const void *, size_t, int, const void *, uint32_t);
  using Recv = int (KYTY_SYSV_ABI *)(int, void *, size_t, int);
  using Recvfrom = int (KYTY_SYSV_ABI *)(int, void *, size_t, int, void *, uint32_t *);
  using Errno = int *(KYTY_SYSV_ABI *)();
  const auto net_connect = reinterpret_cast<Connect>(connect_symbol->vaddr);
  const auto net_getsockopt = reinterpret_cast<Getsockopt>(getsockopt_symbol->vaddr);
  const auto net_setsockopt = reinterpret_cast<Setsockopt>(setsockopt_symbol->vaddr);
  const auto net_send = reinterpret_cast<Send>(send_symbol->vaddr);
  const auto net_sendto = reinterpret_cast<Sendto>(sendto_symbol->vaddr);
  const auto net_recv = reinterpret_cast<Recv>(recv_symbol->vaddr);
  const auto net_recvfrom = reinterpret_cast<Recvfrom>(recvfrom_symbol->vaddr);
  auto *net_errno = reinterpret_cast<Errno>(errno_symbol->vaddr)();
  const auto [reader, writer] = CreateTcpPair();
  const int enabled = 1;
  Check(Net::Setsockopt(writer, 6, 1, &enabled, sizeof(enabled)) == 0,
        "enable TCP_NODELAY");
  int socket_error = -1;
  uint32_t error_size = sizeof(socket_error);
  *Libs::Posix::GetErrorAddr() = Libs::Posix::POSIX_EINVAL;
  *net_errno = Libs::Posix::POSIX_EINVAL;
  Check(net_getsockopt(writer, 0xffff, 0x1007, &socket_error, &error_size) == 0 &&
            socket_error == 0 && error_size == sizeof(socket_error) &&
            *net_errno == Libs::Posix::POSIX_EINVAL &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL,
        "Net SO_ERROR reports socket status without changing either guest errno");
  Check(net_getsockopt(writer, 0xffff, 0x1007, nullptr, &error_size) ==
            Libs::Network::NET_ERROR_EFAULT && *net_errno == Libs::Posix::POSIX_EFAULT,
        "Net getsockopt translates an invalid output buffer");

  std::array<uint64_t, 16> readable {};
  const auto bit = uint64_t {1} << (reader % 64);
  readable[reader / 64] = bit;
  const std::array<int64_t, 2> immediate {0, 0};
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    immediate.data()) == 0 && readable[reader / 64] == 0,
        "empty socket is not readable");
  const char payload[] = "wake";
  *net_errno = Libs::Posix::POSIX_EINVAL;
  Check(net_send(writer, payload, sizeof(payload), 0) == sizeof(payload) &&
            *net_errno == Libs::Posix::POSIX_EINVAL,
        "Net send forwards bytes and preserves errno on success");
  readable[reader / 64] = bit;
  const std::array<int64_t, 2> deadline {1, 0};
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    deadline.data()) == 1 && readable[reader / 64] == bit,
        "select reports the guest descriptor after wake");
  std::array<char, sizeof(payload)> received {};
  Check(net_recv(reader, received.data(), received.size(), 0x42) == sizeof(payload) &&
            std::memcmp(received.data(), payload, sizeof(payload)) == 0 &&
            *net_errno == Libs::Posix::POSIX_EINVAL,
        "Net receive forwards PEEK and WAITALL without consuming bytes");
  received.fill(0);
  Check(net_recv(reader, received.data(), received.size(), 0x40) == sizeof(payload) &&
            std::memcmp(received.data(), payload, sizeof(payload)) == 0,
        "Net receive consumes the same bytes after peeking");
#if !defined(_WIN32)
  Check(Net::Recv(reader, received.data(), received.size(), 0x80) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "empty nonblocking receive translates guest errno");
  Check(net_recv(reader, received.data(), received.size(), 0x80) ==
            Libs::Network::NET_ERROR_EWOULDBLOCK &&
            *net_errno == Libs::Posix::POSIX_EWOULDBLOCK,
        "Net nonblocking receive translates POSIX failure and Net errno");
#endif
  const int datagram = Net::Socket(2, 2, 0);
  const int datagram_writer = Net::Socket(2, 2, 0);
  std::array<uint8_t, 16> address {16, 2, 0, 0, 127, 0, 0, 1};
  std::array<uint8_t, 16> peer {}, expected_peer {};
  uint32_t address_size = address.size(), peer_size = peer.size();
  uint32_t expected_peer_size = expected_peer.size();
  Check(datagram >= 0 && datagram_writer >= 0 &&
            Net::Bind(datagram, address.data(), address.size()) == 0 &&
            Net::Bind(datagram_writer, address.data(), address.size()) == 0 &&
            Net::Getsockname(datagram, address.data(), &address_size) == 0 &&
            Net::Getsockname(datagram_writer, expected_peer.data(), &expected_peer_size) == 0 &&
            Net::Setsockopt(datagram, 0xffff, 0x1200, &enabled, sizeof(enabled)) == 0,
        "create nonblocking loopback datagrams for Net ABI verification");
  *net_errno = Libs::Posix::POSIX_EINVAL;
  *Libs::Posix::GetErrorAddr() = Libs::Posix::POSIX_EINVAL;
  for (const int option : {0x1001, 0x1002}) {
    constexpr int requested = 16384;
    int actual = 0;
    uint32_t size = sizeof(actual);
    Check(net_setsockopt(datagram, 0xffff, option, &requested, sizeof(requested)) == 0 &&
              net_getsockopt(datagram, 0xffff, option, &actual, &size) == 0 &&
              actual >= requested && size == sizeof(actual),
          "Net UDP send and receive buffers accept guest option numbers");
  }
  for (const int value : {1, 0}) {
    int actual = -1;
    uint32_t size = sizeof(actual);
    Check(net_setsockopt(datagram, 0xffff, 0x20, &value, sizeof(value)) == 0 &&
              net_getsockopt(datagram, 0xffff, 0x20, &actual, &size) == 0 &&
              actual == value && size == sizeof(actual),
          "Net UDP broadcast option can be enabled and disabled");
  }
  int timeout = 0;
  int *timeout_value = &timeout;
#if defined(__linux__)
  int broadcast = -1;
  uint32_t broadcast_size = sizeof(broadcast);
  Check(net_setsockopt(datagram, 0xffff, 0x10000, &enabled, sizeof(enabled)) == 0 &&
            net_getsockopt(datagram, 0xffff, 0x20, &broadcast, &broadcast_size) == 0 &&
            broadcast == 0,
        "preserving the all-ones destination does not enable broadcast permission");
  const long page_size = sysconf(_SC_PAGESIZE);
  Check(page_size > 0, "get host page size for socket timeout boundary");
  void *timeout_pages = mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  Check(timeout_pages != MAP_FAILED &&
            mprotect(static_cast<char *>(timeout_pages) + page_size, page_size,
                     PROT_NONE) == 0,
        "guard memory after the four-byte socket timeout");
  timeout_value = reinterpret_cast<int *>(static_cast<char *>(timeout_pages) +
                                           page_size - sizeof(int));
#endif
  for (const int value : {1500000, 0, -1}) {
    *timeout_value = value;
    Check(net_setsockopt(datagram, 0xffff, 0x1105, timeout_value, sizeof(int)) == 0,
          "Net send timeout reads a four-byte microsecond value");
    *timeout_value = -2;
    uint32_t size = sizeof(int);
    Check(net_getsockopt(datagram, 0xffff, 0x1105, timeout_value, &size) == 0 &&
              *timeout_value == std::max(value, 0) && size == sizeof(int),
          "Net send timeout returns four-byte microseconds and disables nonpositive values");
  }
#if defined(__linux__)
  Check(munmap(timeout_pages, page_size * 2) == 0, "free socket timeout guard pages");
#endif
  Check(*net_errno == Libs::Posix::POSIX_EINVAL &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL,
        "successful Net socket option calls preserve both guest errno values");
  Check(net_setsockopt(datagram, 0xffff, 0x1105, nullptr, sizeof(timeout)) ==
            Libs::Network::NET_ERROR_EFAULT && *net_errno == Libs::Posix::POSIX_EFAULT &&
            net_getsockopt(datagram, 0xffff, 0x1105, &timeout, nullptr) ==
            Libs::Network::NET_ERROR_EFAULT && *net_errno == Libs::Posix::POSIX_EFAULT,
        "Net socket options reject null value and length pointers");
  uint32_t short_size = sizeof(timeout) - 1;
  Check(net_setsockopt(datagram, 0xffff, 0x1105, &timeout, short_size) ==
            Libs::Network::NET_ERROR_EINVAL && *net_errno == Libs::Posix::POSIX_EINVAL &&
            net_getsockopt(datagram, 0xffff, 0x1105, &timeout, &short_size) ==
            Libs::Network::NET_ERROR_EINVAL && *net_errno == Libs::Posix::POSIX_EINVAL,
        "Net send timeout rejects undersized values");
  uint32_t option_size = sizeof(timeout);
  Check(net_setsockopt(datagram, 0xffff, 0x7fffffff, &timeout, option_size) ==
            Libs::Network::NET_ERROR_ENOPROTOOPT &&
            *net_errno == Libs::Posix::POSIX_ENOPROTOOPT &&
            net_getsockopt(datagram, 0xffff, 0x7fffffff, &timeout, &option_size) ==
            Libs::Network::NET_ERROR_ENOPROTOOPT &&
            *net_errno == Libs::Posix::POSIX_ENOPROTOOPT,
        "Net unknown socket options return protocol-option errors");
#if defined(__linux__)
  Check(net_setsockopt(datagram, 0xffff, 0x1007, &timeout, option_size) ==
            Libs::Network::NET_ERROR_ENOPROTOOPT &&
            *net_errno == Libs::Posix::POSIX_ENOPROTOOPT,
        "Net native protocol-option errors retain their guest error code");
#endif
  received.fill(0);
  *net_errno = Libs::Posix::POSIX_EINVAL;
  Check(net_sendto(datagram_writer, payload, sizeof(payload), 0,
                   address.data(), address_size) == sizeof(payload) &&
            *net_errno == Libs::Posix::POSIX_EINVAL,
        "Net sendto delivers to a guest sockaddr and preserves errno on success");
  Check(net_recvfrom(datagram, received.data(), received.size(), 0,
                     peer.data(), &peer_size) == sizeof(payload) &&
            std::memcmp(received.data(), payload, sizeof(payload)) == 0 &&
            peer_size == expected_peer_size && peer == expected_peer &&
            *net_errno == Libs::Posix::POSIX_EINVAL,
        "Net recvfrom returns datagram bytes and sender address while preserving errno");
  Check(net_recvfrom(datagram, received.data(), received.size(), 0, nullptr, nullptr) ==
            Libs::Network::NET_ERROR_EWOULDBLOCK &&
            *net_errno == Libs::Posix::POSIX_EWOULDBLOCK,
        "Net recvfrom translates nonblocking failure with an omitted sender address");
  *net_errno = Libs::Posix::POSIX_EINVAL;
  Check(net_connect(datagram_writer, address.data(), address_size) == 0 &&
            *net_errno == Libs::Posix::POSIX_EINVAL &&
            net_sendto(datagram_writer, payload, sizeof(payload), 0, nullptr, 0) ==
                sizeof(payload) && *net_errno == Libs::Posix::POSIX_EINVAL &&
            net_recvfrom(datagram, received.data(), received.size(), 0, nullptr, nullptr) ==
                sizeof(payload) && std::memcmp(received.data(), payload, sizeof(payload)) == 0,
        "Net connect selects the peer used by sendto with an omitted destination");
  Check(net_connect(-1, address.data(), address_size) == Libs::Network::NET_ERROR_EBADF &&
            *net_errno == Libs::Posix::POSIX_EBADF,
        "Net connect translates an invalid socket");
  Check(net_sendto(-1, payload, sizeof(payload), 0, address.data(), address_size) ==
            Libs::Network::NET_ERROR_EBADF && *net_errno == Libs::Posix::POSIX_EBADF,
        "Net sendto translates an invalid socket");
  Check(Net::SocketClose(datagram) == 0 && Net::SocketClose(datagram_writer) == 0,
        "close Net loopback datagrams");
  Check(net_send(-1, payload, sizeof(payload), 0) == Libs::Network::NET_ERROR_EBADF &&
            *net_errno == Libs::Posix::POSIX_EBADF,
        "Net send translates an invalid socket instead of returning POSIX minus one");
  Check(net_recv(reader, nullptr, received.size(), 0) == Libs::Network::NET_ERROR_EFAULT &&
            *net_errno == Libs::Posix::POSIX_EFAULT,
        "Net receive translates an invalid output buffer");
  Check(net_send(writer, payload, sizeof(payload), 0x100000) ==
            Libs::Network::NET_ERROR_EOPNOTSUPP &&
            *net_errno == Libs::Posix::POSIX_EOPNOTSUPP,
        "Net send preserves the backend's unsupported crypto flag error");
#if defined(__linux__)
  const int disconnected = Net::Socket(2, 1, 0);
  Check(disconnected >= 0, "create unconnected socket for broken pipe check");
  const auto previous_sigpipe = std::signal(SIGPIPE, SIG_DFL);
  Check(previous_sigpipe != SIG_ERR, "set default SIGPIPE disposition for Net send");
  const auto broken_send = net_send(disconnected, payload, sizeof(payload), 0);
  const auto broken_sendto = net_sendto(disconnected, payload, sizeof(payload), 0, nullptr, 0);
  std::signal(SIGPIPE, previous_sigpipe);
  Check(broken_send == Libs::Network::NET_ERROR_EPIPE &&
            broken_sendto == Libs::Network::NET_ERROR_EPIPE &&
            *net_errno == Libs::Posix::POSIX_EPIPE,
        "Net send and sendto report a broken pipe without raising host SIGPIPE");
  Check(Net::SocketClose(disconnected) == 0, "close unconnected socket");
#endif
#if defined(_WIN32)
  CheckSocketReceiveBuffer(reader, writer);
  CheckSocketReceiveConcurrency();
#endif
  Check(Net::SocketClose(writer) == 0, "close wake writer");
  *net_errno = Libs::Posix::POSIX_EINVAL;
  Check(net_recv(reader, received.data(), received.size(), 0) == 0 &&
            *net_errno == Libs::Posix::POSIX_EINVAL,
        "Net receive returns EOF without replacing errno");
  Check(Net::SocketClose(reader) == 0, "close wake reader");
  readable[reader / 64] = bit;
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    immediate.data()) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF &&
            readable[reader / 64] == bit,
        "closed descriptor fails without clearing input fd_set");
}

} // namespace

int main(int, char**) {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();
  Common::VirtualMemory::Init();

  Check(SDL_InitSubSystem(SDL_INIT_VIDEO), "initialize Vulkan test video");
  auto graphics = std::make_unique<Libs::Graphics::WindowContext>();
  graphics->graphic_ctx.screen_width = 64;
  graphics->graphic_ctx.screen_height = 64;
  graphics->window = SDL_CreateWindow("KernelFileSystemTests", 64, 64,
                                      SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  Check(graphics->window != nullptr, "create hidden Vulkan test window");
  graphics->CreateVulkan();

  TempDirectory temporary;
  FileSystem::Initialize();
  CheckMountRoot(temporary.Path());
  CheckUnmappedPaths(temporary.Path());
  CheckArchiveMount(temporary.Path());
  CheckUnicodePaths(temporary.Path());
  CheckUnicodeLogPath(temporary.Path());
  CheckDirectoryStream(temporary.Path());
  CheckAprPaths(temporary.Path());
  FileSystem::Mount(temporary.Path(), "/savedata0");
  TestSaveOpenVisibility();
  TestAioBatches();
  TestReadsIntoProtectedPages();
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  FileSystem::Shutdown();
  CheckSocketWakeup();
  TestNpWebApi2Memory();
  graphics.reset();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
