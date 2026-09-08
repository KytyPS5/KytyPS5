#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>

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
  FileSystem::Mount(temporary.Path(), "/savedata0");
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  FileSystem::Shutdown();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
