// The test provides its own entry point, so SDL must not rename main() to SDL_main().
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/stringUtils.h"
#include "graphics/presentation/window/windowInternal.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <algorithm>
#include <array>

#include <chrono>
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
  Check(FileSystem::GetRealFilename("/app0/rpf.cache") == "/app0/rpf.cache",
        "unmount by guest path");
  for (const auto &folder : {root, root / ""}) {
    for (const auto &host : {root, root / ""}) {
      FileSystem::Mount(folder, "/app0");
      FileSystem::Umount(Common::PathToGenericString(host));
      Check(FileSystem::GetRealFilename("/app0/rpf.cache") == "/app0/rpf.cache",
            "unmount by host path with or without trailing separator");
    }
  }
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
  const auto wait_readable = [&readable, &deadline](int fd, const char* message) {
    const auto fd_bit = uint64_t {1} << (fd % 64);
    readable[fd / 64] = fd_bit;
    Check(Net::Select(fd + 1, readable.data(), nullptr, nullptr, deadline.data()) == 1 &&
              readable[fd / 64] == fd_bit,
          message);
  };
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    deadline.data()) == 1 && readable[reader / 64] == bit,
        "select reports the guest descriptor after wake");
  std::array<char, sizeof(payload)> received {};
  Check(Net::Recv(reader, received.data(), received.size(), 0x42) == sizeof(payload) &&
            std::memcmp(received.data(), payload, sizeof(payload)) == 0,
        "guest PEEK and WAITALL preserve the wake bytes");
  Check(Net::Recv(reader, received.data(), received.size(), 0x40) == sizeof(payload),
        "consume wake bytes with guest WAITALL");

  // A full message split across writes must still complete a WAITALL receive, and the
  // peeked bytes have to survive for the following receive.
  const char text[] = "fragment";
  constexpr std::size_t text_length = 8;    // without the terminator
  constexpr std::size_t prefix_length = 5;  // deliberately short of text_length
  Check(Net::Send(writer, text, text_length / 2, 0) == text_length / 2,
        "send the first fragment");
  const char* const second_fragment = text + text_length / 2;
  const std::size_t second_length   = text_length - text_length / 2;
  int64_t second_send_result        = -1;
  std::thread peer([&writer, second_fragment, second_length, &second_send_result] {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    second_send_result = Net::Send(writer, second_fragment, second_length, 0);
    if (second_send_result != static_cast<int64_t>(second_length)) {
      // A short or failed send would strand the blocking receive below. The writer is
      // still open, so no end of stream ever arrives and the peek loop would spin
      // forever instead of failing. Half-close this end so the reader sees EOF and
      // the receive returns the short prefix, which fails the check below.
      Net::Shutdown(writer, 1);
    }
  });
  std::array<char, text_length> message {};
  Check(Net::Recv(reader, message.data(), message.size(), 0x42) == message.size() &&
            std::memcmp(message.data(), text, text_length) == 0,
        "guest PEEK and WAITALL waits for a fragmented message");
  peer.join();
  Check(second_send_result == second_length, "send second fragment");
  Check(Net::Recv(reader, message.data(), message.size(), 0) == message.size() &&
            std::memcmp(message.data(), text, text_length) == 0,
        "peeked bytes stay available for the following receive");
  Check(Net::Recv(reader, message.data(), 0, 0x42) == 0,
        "zero length guest PEEK and WAITALL succeeds");

  // MSG_DONTWAIT must never wait for the rest of the message.
  Check(Net::Send(writer, text, prefix_length, 0) == prefix_length,
        "send bytes for the non-waiting peek");
  wait_readable(reader, "non-waiting peek bytes arrived");
  Check(Net::Recv(reader, message.data(), message.size(), 0xc2) == prefix_length,
        "guest MSG_DONTWAIT PEEK and WAITALL returns the buffered prefix");
  Check(Net::Recv(reader, message.data(), prefix_length, 0) == prefix_length,
        "consume the non-waiting peek prefix");
  Check(Net::Recv(reader, message.data(), message.size(), 0xc2) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "empty guest MSG_DONTWAIT PEEK and WAITALL translates guest errno");
#if defined(_WIN32)
  const int nonblocking = 1;
  Check(Net::Setsockopt(reader, 0xffff, 0x1200, &nonblocking, sizeof(nonblocking)) == 0,
        "enable the guest non-blocking socket");
  Check(Net::Send(writer, text, prefix_length, 0) == prefix_length,
        "send bytes for the non-blocking socket peek");
  wait_readable(reader, "non-blocking socket peek bytes arrived");
  Check(Net::Recv(reader, message.data(), message.size(), 0x42) == prefix_length,
        "non-blocking socket PEEK and WAITALL returns the buffered prefix");
  Check(Net::Recv(reader, message.data(), prefix_length, 0) == prefix_length,
        "consume the non-blocking socket peek prefix");
  const int blocking = 0;
  Check(Net::Setsockopt(reader, 0xffff, 0x1200, &blocking, sizeof(blocking)) == 0,
        "restore the guest blocking socket");
#endif
#if !defined(_WIN32)
  Check(Net::Recv(reader, received.data(), received.size(), 0x80) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "empty nonblocking receive translates guest errno");
#endif
  // A peer that closes before the requested length ends the wait with the buffered bytes.
  Check(Net::Send(writer, text, prefix_length, 0) == prefix_length,
        "send the final message");
  Check(Net::SocketClose(writer) == 0, "close the writer to signal end of file");
  std::array<char, 16> tail {};
  Check(Net::Recv(reader, tail.data(), tail.size(), 0x42) == prefix_length &&
            std::memcmp(tail.data(), text, prefix_length) == 0,
        "guest PEEK and WAITALL returns a short read at end of file");
  Check(Net::Recv(reader, tail.data(), tail.size(), 0x42) == prefix_length,
        "short peek at end of file keeps the bytes queued");
  Check(Net::Recv(reader, tail.data(), tail.size(), 0) == prefix_length,
        "consume the end of file bytes");
  Check(Net::Recv(reader, tail.data(), tail.size(), 0x42) == 0,
        "guest PEEK and WAITALL reports end of file");
  Check(Net::SocketClose(reader) == 0, "close the wake reader");
  readable[reader / 64] = bit;
  Check(Net::Select(reader + 1, readable.data(), nullptr, nullptr,
                    immediate.data()) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF &&
            readable[reader / 64] == bit,
        "closed descriptor fails without clearing input fd_set");

#if defined(_WIN32)
  // FreeBSD accept(2) inherits O_NONBLOCK from the listening socket. Keep the native
  // accepted socket and the emulator's guest-mode bookkeeping in agreement.
  std::array<uint8_t, 16> inherited_address {16, 2, 0, 0, 127, 0, 0, 1};
  const int inherited_listener = Net::Socket(2, 1, 0);
  Check(inherited_listener >= 0 &&
            Net::Bind(inherited_listener, inherited_address.data(),
                      inherited_address.size()) == 0 &&
            Net::Listen(inherited_listener, 1) == 0,
        "create listener for accepted-mode test");
  uint32_t inherited_address_size = inherited_address.size();
  Check(Net::Getsockname(inherited_listener, inherited_address.data(),
                         &inherited_address_size) == 0,
        "get accepted-mode listener port");
  const int inherited_writer = Net::Socket(2, 1, 0);
  const int inherited_nonblocking = 1;
  Check(Net::Setsockopt(inherited_listener, 0xffff, 0x1200, &inherited_nonblocking,
                        sizeof(inherited_nonblocking)) == 0 &&
            Net::Connect(inherited_writer, inherited_address.data(),
                         inherited_address_size) == 0,
        "connect to nonblocking listener");
  wait_readable(inherited_listener, "accepted-mode connection is queued");
  const int inherited_reader = Net::Accept(inherited_listener, nullptr, nullptr);
  Check(inherited_reader >= 0, "accept nonblocking listener socket");
  Check(Net::Send(inherited_writer, text, prefix_length, 0) == prefix_length,
        "send accepted-mode prefix");
  wait_readable(inherited_reader, "accepted-mode prefix arrived");
  std::array<char, text_length> inherited_message {};
  auto inherited_receive = std::async(std::launch::async, [&] {
    return Net::Recv(inherited_reader, inherited_message.data(), inherited_message.size(),
                     0x42);
  });
  const bool returned_before_completion =
      inherited_receive.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  Check(Net::Send(inherited_writer, text + prefix_length,
                  text_length - prefix_length, 0) == text_length - prefix_length,
        "send accepted-mode suffix");
  const int64_t inherited_result = inherited_receive.get();
  Check(returned_before_completion && inherited_result == prefix_length &&
            std::memcmp(inherited_message.data(), text, prefix_length) == 0,
        "accepted nonblocking PEEK and WAITALL returns the prefix");
  Check(Net::Recv(inherited_reader, inherited_message.data(), inherited_message.size(), 0) ==
            text_length,
        "consume accepted-mode peeked message");
  Check(Net::SocketClose(inherited_reader) == 0 &&
            Net::SocketClose(inherited_writer) == 0 &&
            Net::SocketClose(inherited_listener) == 0,
        "close accepted-mode sockets");
#endif

  // A zero-length datagram peek still has to report its source address on Winsock.
  std::array<uint8_t, 16> datagram_address {16, 2, 0, 0, 127, 0, 0, 1};
  const int datagram_receiver = Net::Socket(2, 2, 0);
  Check(datagram_receiver >= 0 &&
            Net::Bind(datagram_receiver, datagram_address.data(),
                      datagram_address.size()) == 0,
        "create datagram receiver");
  uint32_t datagram_address_size = datagram_address.size();
  Check(Net::Getsockname(datagram_receiver, datagram_address.data(),
                         &datagram_address_size) == 0,
        "get datagram receiver port");
  const int datagram_sender = Net::Socket(2, 2, 0);
  std::array<uint8_t, 16> datagram_sender_address {16, 2, 0, 0, 127, 0, 0, 1};
  Check(datagram_sender >= 0 &&
            Net::Bind(datagram_sender, datagram_sender_address.data(),
                      datagram_sender_address.size()) == 0,
        "bind datagram sender");
  uint32_t datagram_sender_address_size = datagram_sender_address.size();
  Check(Net::Getsockname(datagram_sender, datagram_sender_address.data(),
                         &datagram_sender_address_size) == 0,
        "get datagram sender port");
  const char empty_datagram = 0;
  Check(Net::Sendto(datagram_sender, &empty_datagram, 0, 0, datagram_address.data(),
                    datagram_address.size()) == 0,
        "send zero-length datagram");
  std::array<uint8_t, 16> datagram_source {};
  uint32_t datagram_source_size = datagram_source.size();
  std::array<char, 1> datagram_buffer {};
  Check(Net::Recvfrom(datagram_receiver, datagram_buffer.data(), 0, 0x42,
                      datagram_source.data(), &datagram_source_size) == 0 &&
            datagram_source_size == datagram_sender_address.size() &&
            std::memcmp(datagram_source.data(), datagram_sender_address.data(),
                        datagram_sender_address.size()) == 0,
        "zero-length datagram PEEK and WAITALL reports its source");
  datagram_source_size = datagram_source.size();
  Check(Net::Recvfrom(datagram_receiver, datagram_buffer.data(), datagram_buffer.size(), 0,
                      datagram_source.data(), &datagram_source_size) == 0,
        "consume zero-length datagram");

  // Winsock needs a one-byte scratch buffer to obtain the source of a zero-length peek.
  // A queued one-byte datagram must still be reported as a zero-length receive.
  constexpr char one_byte_datagram = 'x';
  Check(Net::Sendto(datagram_sender, &one_byte_datagram, sizeof(one_byte_datagram), 0,
                    datagram_address.data(), datagram_address.size()) == 1,
        "send one-byte datagram");
  datagram_source_size = datagram_source.size();
  Check(Net::Recvfrom(datagram_receiver, datagram_buffer.data(), 0, 0x42,
                      datagram_source.data(), &datagram_source_size) == 0 &&
            datagram_source_size == datagram_sender_address.size() &&
            std::memcmp(datagram_source.data(), datagram_sender_address.data(),
                        datagram_sender_address.size()) == 0,
        "zero-length peek does not report scratch data");
  datagram_source_size = datagram_source.size();
  Check(Net::Recvfrom(datagram_receiver, datagram_buffer.data(), datagram_buffer.size(), 0,
                      datagram_source.data(), &datagram_source_size) == 1 &&
            datagram_buffer[0] == one_byte_datagram,
        "consume one-byte datagram after zero-length peek");
  Check(Net::SocketClose(datagram_sender) == 0 &&
            Net::SocketClose(datagram_receiver) == 0,
        "close datagram sockets");
}

} // namespace

int main() {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();

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
  CheckUnicodePaths(temporary.Path());
  CheckUnicodeLogPath(temporary.Path());
  CheckDirectoryStream(temporary.Path());
  CheckAprPaths(temporary.Path());
  FileSystem::Mount(temporary.Path(), "/savedata0");
  TestSaveOpenVisibility();
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  FileSystem::Shutdown();
  CheckSocketWakeup();
  graphics.reset();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
