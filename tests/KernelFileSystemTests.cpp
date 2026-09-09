#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"
#include "loader/systemContent.h"

#include <array>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

namespace Libs::LibKernelApr {
void InitLibKernel_1_Apr(Loader::SymbolDatabase *symbols);
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
  Check(data.Size() == expected.size(), "renamed save size");
  Check(std::memcmp(data.GetData(), expected.data(), expected.size()) == 0,
        "renamed save contents");
}

void CheckAdjacentModuleDiscovery() {
  TempDirectory temporary;
  const auto &root = temporary.Path();
  std::filesystem::create_directories(root / "sce_module");
  std::filesystem::create_directories(root / "sce_modules");
  std::filesystem::create_directories(root / "fakelib");

  for (const auto &path : {root / "root.prx", root / "eboot.bin",
                           root / "ignored.txt",
                           root / "sce_module" / "module.sprx",
                           root / "sce_modules" / "modules.prx",
                           root / "fakelib" / "compat.sprx"}) {
    std::ofstream(path).put('\0');
  }

  std::set<std::string> relative;
  for (const auto &path : Loader::DiscoverAdjacentProgramFiles(root)) {
    relative.insert(std::filesystem::relative(path, root).generic_string());
  }

  Check(relative.contains("root.prx"), "discover root PRX");
  Check(relative.contains("sce_module/module.sprx"),
        "discover sce_module SPRX");
  Check(relative.contains("sce_modules/modules.prx"),
        "discover sce_modules PRX");
  Check(relative.contains("fakelib/compat.sprx"),
        "discover compatibility SPRX");
  Check(!relative.contains("eboot.bin"), "skip eboot");
  Check(!relative.contains("ignored.txt"), "skip non-module file");
}

void CheckPs5PlayGoChunkCount() {
  TempDirectory temporary;
  const auto sce_sys = temporary.Path() / "sce_sys";
  std::filesystem::create_directories(sce_sys);

  {
    std::ofstream param(sce_sys / "param.json", std::ios::binary);
    param << "{}";
  }
  {
    // PS5 PlayGo header ("plgx") with 35 chunks at the shared offset.
    constexpr unsigned char header[] = {
        0x70, 0x6c, 0x67, 0x78, 0x00, 0x10,
        0x00, 0x00, 0x01, 0x00, 0x23, 0x00,
    };
    std::ofstream chunks(sce_sys / "playgo-chunk.dat", std::ios::binary);
    chunks.write(reinterpret_cast<const char *>(header), sizeof(header));
  }

  Loader::SystemContentLoadParamSfo(sce_sys / "param.json");
  uint32_t chunks_num = 0;
  Check(Loader::SystemContentGetChunksNum(&chunks_num),
        "open PS5 PlayGo chunk table");
  Check(chunks_num == 35, "read PS5 PlayGo chunk count");
}

void CheckMountRoot(const std::filesystem::path &root) {
  Common::File cache;
  Check(cache.Create(root / "rpf.cache"), "create directory listing fixture");
  cache.Close();
  FileSystem::Mount(root, "/app0");
  Check(FileSystem::GetRealFilename("/app0/rpf.cache") == root / "rpf.cache",
        "resolve mount descendant");
  Check(FileSystem::GetRealFilename("/app01/rpf.cache") == "/app01/rpf.cache",
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
  FileSystem::Umount("/app0");
}

} // namespace

int main() {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::OutputDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();

  CheckAdjacentModuleDiscovery();
  CheckPs5PlayGoChunkCount();

  TempDirectory temporary;
  FileSystem::Initialize();
  CheckMountRoot(temporary.Path());
  CheckAprPaths(temporary.Path());
  FileSystem::Mount(temporary.Path(), "/savedata0");
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  FileSystem::Shutdown();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
