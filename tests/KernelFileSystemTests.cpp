#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
  result.Close();
  const std::string expected = std::string(payload) + Suffix;
  Check(data.Size() == expected.size(), "renamed save size");
  Check(std::memcmp(data.GetData(), expected.data(), expected.size()) == 0,
        "renamed save contents");
}

void CheckPosixMetadata() {
  constexpr char Path[] = "/savedata0/metadata.bin";
  const int fd = FileSystem::KernelOpen(Path, 0x601, 0777);
  Check(fd >= 3, "open metadata file");

  const Libs::LibKernel::KernelTimeval times[2] = {{1700000000, 125000},
                                                   {1700000100, 750000}};
  Check(FileSystem::KernelFutimes(fd, times) == OK,
        "set descriptor timestamps");
  Check(FileSystem::KernelFchmod(fd, 0444) == OK,
        "set descriptor read-only mode");
  Check(FileSystem::KernelFchmod(fd, 0666) == OK,
        "restore descriptor writable mode");

  Libs::LibKernel::FileSystem::FileStat stat{};
  Check(FileSystem::KernelFstat(fd, &stat) == OK, "stat metadata descriptor");
  Check(stat.st_mtim.tv_sec == times[1].tv_sec, "descriptor write timestamp");

  const Libs::LibKernel::KernelTimeval invalid[2] = {{1700000000, 1000000},
                                                     {1700000100, 0}};
  Check(FileSystem::KernelFutimes(fd, invalid) ==
            Libs::LibKernel::KERNEL_ERROR_EINVAL,
        "reject invalid microseconds");
  Check(FileSystem::KernelClose(fd) == OK, "close metadata file");

  Check(FileSystem::KernelUtimes(Path, times) == OK, "set path timestamps");
  Check(FileSystem::KernelStat(Path, &stat) == OK, "stat metadata path");
  Check(stat.st_atim.tv_sec == times[0].tv_sec &&
            stat.st_mtim.tv_sec == times[1].tv_sec,
        "path timestamps");
  Check(FileSystem::KernelUtimes("/savedata0", times) == OK,
        "set directory timestamps");
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

  TempDirectory temporary;
  FileSystem::Initialize();
  FileSystem::Mount(temporary.Path(), "/savedata0");
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  CheckPosixMetadata();
  FileSystem::Shutdown();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
