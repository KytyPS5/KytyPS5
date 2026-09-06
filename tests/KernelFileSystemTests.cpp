#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/network.h"

#include <array>

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
  const std::string expected = std::string(payload) + Suffix;
  Check(data.Size() == expected.size(), "renamed save size");
  Check(std::memcmp(data.GetData(), expected.data(), expected.size()) == 0,
        "renamed save contents");
}

void CheckSocketWakeup() {
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
  const int enabled = 1;
  Check(Net::Setsockopt(writer, 6, 1, &enabled, sizeof(enabled)) == 0,
        "enable TCP_NODELAY");
  int socket_error = -1;
  uint32_t error_size = sizeof(socket_error);
  *Libs::Posix::GetErrorAddr() = Libs::Posix::POSIX_EINVAL;
  Check(Net::Getsockopt(writer, 0xffff, 0x1007, &socket_error, &error_size) == 0 &&
            socket_error == 0 && error_size == sizeof(socket_error) &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL,
        "SO_ERROR reports socket status without changing guest errno");

  std::array<uint64_t, 16> readable {};
  const auto bit = uint64_t {1} << (reader % 64);
  readable[reader / 64] = bit;
  const std::array<int64_t, 2> immediate {0, 0};
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    immediate.data()) == 0 && readable[reader / 64] == 0,
        "empty socket is not readable");
  const char payload[] = "wake";
  Check(Net::Send(writer, payload, sizeof(payload), 0x20000) == sizeof(payload),
        "send wake bytes with guest MSG_NOSIGNAL");
  readable[reader / 64] = bit;
  const std::array<int64_t, 2> deadline {1, 0};
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    deadline.data()) == 1 && readable[reader / 64] == bit,
        "select reports the guest descriptor after wake");
  std::array<char, sizeof(payload)> received {};
  Check(Net::Recv(reader, received.data(), received.size(), 0x42) == sizeof(payload) &&
            std::memcmp(received.data(), payload, sizeof(payload)) == 0,
        "guest PEEK and WAITALL preserve the wake bytes");
  Check(Net::Recv(reader, received.data(), received.size(), 0x40) == sizeof(payload),
        "consume wake bytes with guest WAITALL");
#if !defined(_WIN32)
  Check(Net::Recv(reader, received.data(), received.size(), 0x80) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "empty nonblocking receive translates guest errno");
#endif
  Check(Net::SocketClose(reader) == 0 && Net::SocketClose(writer) == 0,
        "close wake sockets");
  readable[reader / 64] = bit;
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    immediate.data()) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF &&
            readable[reader / 64] == bit,
        "closed descriptor fails without clearing input fd_set");
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
  FileSystem::Shutdown();
  CheckSocketWakeup();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
