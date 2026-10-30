#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "common/emulatorConfig.h"
#include "common/archive.h"
#include "common/file.h"
#include "ArchiveTestFixture.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "common/stringUtils.h"
#include "graphics/presentation/window/windowInternal.h"
#include "kernel/eventQueue.h"
#include "kernel/fileSystem.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/network.h"
#include "loader/additionalContent.h"
#include "loader/runtimeLinker.h"
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
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Libs::LibKernelApr {
void InitLibKernel_1_Apr(Loader::SymbolDatabase *symbols);
}

namespace Libs {
void InitLibKernel_1(Loader::SymbolDatabase *symbols);
void InitSysmodule_1(Loader::SymbolDatabase *symbols);
void InitSystemService_1(Loader::SymbolDatabase *symbols);
void InitAppContent_1(Loader::SymbolDatabase *symbols);
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

namespace Libs::LibNpCommerce {
void InitNet_1_NpCommerce(Loader::SymbolDatabase *symbols);
}

namespace Libs::LibNpAuth {
void InitNet_1_NpAuth(Loader::SymbolDatabase *symbols);
}

namespace Libs::LibNpEntitlementAccess {
void InitNet_1_NpEntitlementAccess(Loader::SymbolDatabase *symbols);
}

namespace {

namespace FileSystem = Libs::LibKernel::FileSystem;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "KernelFileSystemTests: failed: %s\n", text);
    std::abort();
  }
}

template <typename Predicate>
void WaitForMutexTest(Predicate ready) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!ready()) {
    Check(std::chrono::steady_clock::now() < deadline, "mutex waiter made progress");
    std::this_thread::yield();
  }
}

void TestStaticRwlockDestroy() {
  using namespace Libs::LibKernel;
  PthreadRwlock rwlock = nullptr;
  Check(PthreadRwlockDestroy(nullptr) == KERNEL_ERROR_EINVAL &&
            PthreadRwlockDestroy(&rwlock) == OK && rwlock == nullptr,
        "destroy an untouched static rwlock without allocating it");
  Check(PthreadRwlockRdlock(&rwlock) == OK && PthreadRwlockUnlock(&rwlock) == OK &&
            PthreadRwlockDestroy(&rwlock) == OK,
        "static rwlock remains usable after destroying its untouched initializer");
}

struct MutexWaiter {
  Libs::LibKernel::PthreadMutex *mutex;
  Libs::LibKernel::KernelUseconds timeout = 0;
  bool cancel = false;
  Libs::LibKernel::Pthread thread = nullptr;
  std::vector<uint8_t> stack = std::vector<uint8_t>(1024 * 1024);
  std::atomic<unsigned> parked{0}, resume{0};
  std::atomic<bool> acquired{false}, release{false}, finished{false};
  int result = 0;
};

thread_local MutexWaiter *current_mutex_waiter = nullptr;

void KYTY_SYSV_ABI ParkMutexWaiter(int, void *) {
  auto &waiter = *current_mutex_waiter;
  const auto ticket = waiter.parked.fetch_add(1) + 1;
  WaitForMutexTest([&] { return waiter.resume.load() >= ticket; });
}

void *KYTY_SYSV_ABI RunMutexWaiter(void *arg) {
  using namespace Libs::LibKernel;
  auto &waiter = *static_cast<MutexWaiter *>(arg);
  current_mutex_waiter = &waiter;
  waiter.result = waiter.timeout == 0 ? PthreadMutexLock(waiter.mutex)
                                      : PthreadMutexTimedlock(waiter.mutex, waiter.timeout);
  if (waiter.result == OK) {
    waiter.acquired = true;
    WaitForMutexTest([&] { return waiter.release.load(); });
    Check(PthreadMutexUnlock(waiter.mutex) == OK, "waiter unlocks owned mutex");
  }
  waiter.finished = true;
  if (waiter.cancel) {
    PthreadTestcancel();
    Check(false, "deferred mutex waiter cancellation remains pending");
  }
  return nullptr;
}

void QueueMutexWaiter(MutexWaiter &waiter) {
  using namespace Libs::LibKernel;
  PthreadAttr attr = nullptr;
  Check(PthreadAttrInit(&attr) == OK &&
            PthreadAttrSetstack(&attr, waiter.stack.data(), waiter.stack.size()) == OK &&
            PthreadCreate(&waiter.thread, &attr, RunMutexWaiter, &waiter, "MutexTest") == OK &&
            PthreadAttrDestroy(&attr) == OK,
        "create guest mutex waiter");
  // Pending guest signals are dispatched inside the contended lock, after enqueueing.
  // Holding that callback proves queue order without letting the waiter acquire.
  PthreadQueuePendingSignal(waiter.thread, 30);
  WaitForMutexTest([&] { return waiter.parked.load() == 1; });
}

void TestMutexHandoff() {
  using namespace Libs::LibKernel;
  Common::Singleton<Loader::RuntimeLinker>::Instance();
  Initialize();
  PthreadInitSelfForMainThread();
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto *install_symbol = symbols.FindByNid("WkwEd3N7w0Y", Loader::SymbolType::Func);
  const auto *remove_symbol = symbols.FindByNid("Qhv5ARAoOEc", Loader::SymbolType::Func);
  Check(install_symbol != nullptr && remove_symbol != nullptr, "mutex signal exports resolve");
  const auto install = reinterpret_cast<int(KYTY_SYSV_ABI *)(int, void *)>(install_symbol->vaddr);
  const auto remove = reinterpret_cast<int(KYTY_SYSV_ABI *)(int)>(remove_symbol->vaddr);
  Check(install(30, reinterpret_cast<void *>(ParkMutexWaiter)) == OK, "install mutex test signal");

  PthreadMutexattr attr = nullptr;
  Check(PthreadMutexattrInit(&attr) == OK && PthreadMutexattrSettype(&attr, 2) == OK,
        "create recursive mutex attributes");
  for (unsigned timed = 0; timed < 3; ++timed) {
    PthreadMutex mutex = nullptr;
    Check(PthreadMutexInit(&mutex, &attr, "MutexHandoff") == OK &&
              PthreadMutexLock(&mutex) == OK && PthreadMutexLock(&mutex) == OK,
          "lock recursive mutex twice");
    std::array<MutexWaiter, 3> waiters{{{&mutex}, {&mutex}, {&mutex}}};
    waiters[timed].timeout = 5000;
    for (auto &waiter : waiters) {
      QueueMutexWaiter(waiter);
    }
    waiters[timed].resume = 1;
    Check(PthreadJoin(waiters[timed].thread, nullptr) == OK &&
              waiters[timed].result == KERNEL_ERROR_ETIMEDOUT,
          "timed waiter unlinks from queue head, middle and tail");
    Check(PthreadMutexUnlock(&mutex) == OK && PthreadMutexTrylock(&mutex) == OK &&
              PthreadMutexUnlock(&mutex) == OK,
          "partial recursive unlock retains ownership");
    Check(PthreadMutexUnlock(&mutex) == OK &&
              PthreadMutexTrylock(&mutex) == KERNEL_ERROR_EBUSY,
          "unlock reserves ownership before queued waiter runs");
    for (auto &waiter : waiters) {
      if (&waiter == &waiters[timed]) {
        continue;
      }
      waiter.resume = 1;
      WaitForMutexTest([&] { return waiter.acquired.load(); });
      Check(PthreadMutexTrylock(&mutex) == KERNEL_ERROR_EBUSY, "FIFO waiter owns mutex");
      waiter.release = true;
      WaitForMutexTest([&] { return waiter.finished.load(); });
      Check(PthreadJoin(waiter.thread, nullptr) == OK && waiter.result == OK,
            "FIFO waiter completes");
    }
    Check(PthreadMutexLock(&mutex) == OK && PthreadMutexUnlock(&mutex) == OK &&
              PthreadMutexDestroy(&mutex) == OK,
          "mutex remains usable after timed waiter removal");
  }
  Check(PthreadMutexattrDestroy(&attr) == OK, "destroy recursive mutex attributes");

  PthreadMutex mutex = nullptr;
  Check(PthreadMutexInit(&mutex, nullptr, "MutexCancel") == OK &&
            PthreadMutexLock(&mutex) == OK &&
            PthreadMutexTimedlock(&mutex, 5000) == KERNEL_ERROR_EDEADLK,
        "nonrecursive self timed lock reports deadlock");
  MutexWaiter waiter{&mutex};
  waiter.cancel = true;
  QueueMutexWaiter(waiter);
  Check(PthreadCancel(waiter.thread) == OK, "cancel queued mutex waiter");
  PthreadQueuePendingSignal(waiter.thread, 30);
  waiter.resume = 1;
  WaitForMutexTest([&] { return waiter.parked.load() == 2; });
  Check(PthreadMutexUnlock(&mutex) == OK &&
            PthreadMutexTrylock(&mutex) == KERNEL_ERROR_EBUSY,
        "canceled waiter retains its queue position");
  waiter.release = true;
  waiter.resume = 2;
  void *result = nullptr;
  Check(PthreadJoin(waiter.thread, &result) == OK &&
            result == reinterpret_cast<void *>(-1) && waiter.acquired && waiter.finished,
        "mutex lock defers cancellation until explicit cancellation point");
  Check(PthreadMutexLock(&mutex) == OK && PthreadMutexUnlock(&mutex) == OK &&
            PthreadMutexDestroy(&mutex) == OK && remove(30) == OK,
        "mutex remains usable after waiter cancellation");
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

void CheckOpenFileCoherence(const std::filesystem::path& root) {
  const auto path = root / "open-file-coherence.dat";
  Common::File writer;
  Check(writer.Create(path), "create shared file fixture");
  uint32_t count = 0;
  writer.Write("abcdef", 6, &count);
  Check(count == 6 && Common::File::Size(path) == 6,
        "created file writes are visible before flush or close");

  Common::File reader(path, Common::File::Mode::Read);
  Common::File update(path, Common::File::Mode::Write);
  Check(!reader.IsInvalid() && !update.IsInvalid(), "open independent file handles");
  std::array<char, 2> bytes {};
  reader.Read(bytes.data(), 1, &count);
  Check(count == 1 && bytes[0] == 'a' && update.Seek(1),
        "prime sequential read and position independent writer");
  update.Write("XY", 2, &count);
  Check(count == 2, "overwrite bytes through write handle");
  reader.Read(bytes.data(), bytes.size(), &count);
  Check(count == bytes.size() && std::memcmp(bytes.data(), "XY", bytes.size()) == 0,
        "sequential reader observes another handle's writes without seeking");

  Common::File read_write(path, Common::File::Mode::ReadWrite);
  Check(!read_write.IsInvalid() && read_write.Seek(3), "open read-write handle");
  read_write.Write("Z", 1, &count);
  Check(count == 1, "overwrite through read-write handle");
  reader.Read(bytes.data(), 1, &count);
  Check(count == 1 && bytes[0] == 'Z', "read-write handle changes are immediately visible");

  writer.Write("g", 1, &count);
  Check(count == 1 && writer.Tell() == 7, "extend through the original writer");
  std::error_code error;
  std::filesystem::resize_file(path, 2, error);
  Check(!error && writer.Tell() == 7 && Common::File::Size(path) == 2,
        "path resize preserves an open writer's position");
  writer.Close();
  update.Close();
  read_write.Close();
  Check(Common::File::Size(path) == 2,
        "closing existing writers cannot undo a path truncation");
}

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

void TestRandomDevices() {
  namespace Net = Libs::Network::Net;
  for (const auto* path : {"/dev/urandom", "/dev/random"}) {
    const int fd = FileSystem::KernelOpen(path, 0, 0);
    FileSystem::FileStat stat {};
    Check(fd >= 3 && FileSystem::KernelFstat(fd, &stat) == OK &&
              (stat.st_mode & 0170000) == 0020000,
          "entropy sources are character devices");
    std::array<uint8_t, 32> entropy {};
    Check(FileSystem::KernelRead(fd, entropy.data(), entropy.size()) == entropy.size(),
          "character device supplies requested entropy");
    // OpenSSL RAND_poll selects the device before reading it; a null timeout must not block.
    std::array<uint64_t, 16> readable {};
    readable[fd / 64] = uint64_t {1} << (fd % 64);
    auto writable = readable;
    const auto requested = readable;
    Check(Net::Select(fd + 1, readable.data(), writable.data(), nullptr, nullptr) == 2 &&
              readable == requested && writable == requested,
          "select reports an entropy source ready at once");
    Check(FileSystem::KernelClose(fd) == OK, "close entropy source");
    Check(Net::Select(fd + 1, readable.data(), nullptr, nullptr, nullptr) == -1 &&
              *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
          "select rejects a closed descriptor");
  }
}

void TestFileDescriptorFlags() {
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto* symbol = symbols.Find(
      {"8nY19bKoiZk", "Posix", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
  Check(symbol != nullptr, "POSIX fcntl export resolves");
  const auto fcntl = reinterpret_cast<int (KYTY_SYSV_ABI *)(int, int, int)>(symbol->vaddr);
  Check(fcntl(std::numeric_limits<int>::min(), 1, 0) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
        "invalid descriptor reports EBADF");
  for (const auto* path : {"/dev/urandom", "/dev/random"}) {
    const int fd = FileSystem::KernelOpen(path, 0, 0);
    Check(fd >= 3, "open descriptor for flag checks");
    Check(fcntl(fd, 1, 0) == 0 && fcntl(fd, 2, 1) == 0 && fcntl(fd, 1, 0) == 1,
          "entropy descriptor retains close-on-exec flag");
    Check(fcntl(fd, 2, 0) == 0 && fcntl(fd, 1, 0) == 0,
          "close-on-exec flag can be cleared");
    Check(fcntl(fd, -1, 0) == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL,
          "unsupported fcntl command reports EINVAL");
    Check(FileSystem::KernelClose(fd) == OK, "close entropy source");
    Check(fcntl(fd, 1, 0) == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
          "closed descriptor reports EBADF");
    const int cloexec_fd = FileSystem::KernelOpen(path, 0x00100000, 0);
    Check(cloexec_fd >= 3 && fcntl(cloexec_fd, 1, 0) == 1,
          "O_CLOEXEC sets the descriptor flag on open");
    Check(FileSystem::KernelClose(cloexec_fd) == OK, "close flagged entropy source");
  }
}

void TestSysmoduleReferences() {
  Loader::SymbolDatabase symbols;
  Libs::InitSysmodule_1(&symbols);
  using ModuleCall = int (KYTY_SYSV_ABI *)(uint16_t);
  const auto resolve = [&](const char* nid) {
    const auto* symbol = symbols.Find(
        {nid, "Sysmodule", 1, "Sysmodule", 1, 1, Loader::SymbolType::Func});
    Check(symbol != nullptr, "Sysmodule export resolves");
    return reinterpret_cast<ModuleCall>(symbol->vaddr);
  };
  const auto load = resolve("g8cM39EUZ6o");
  const auto unload = resolve("eR2bZFAAU0Q");
  const auto is_loaded = resolve("fMP5NHUOaMk");
  constexpr int unloaded = static_cast<int>(0x805a1001u);
  Check(load(0x115) == Libs::LibKernel::KERNEL_ERROR_ENOENT && is_loaded(0x115) == unloaded &&
            unload(0x115) == unloaded,
        "missing packaged NpCppWebApi fails without recording a loaded module");
  Check(load(0x113) == OK && is_loaded(0xb4) == unloaded,
        "loading entitlement access does not mark AppContent loaded");
  Check(unload(0x113) == OK, "release independent module reference");
  for (const uint16_t id : {0x113, 0xb4}) {
    Check(is_loaded(id) == unloaded, "unloaded module allows guest initialization");
    Check(unload(id) == unloaded, "unloading an absent module reports UNLOADED");
    Check(load(id) == OK && load(id) == OK && is_loaded(id) == OK,
          "repeated loads retain references");
    Check(unload(id) == OK && is_loaded(id) == OK,
          "one unload preserves the remaining reference");
    Check(unload(id) == OK && is_loaded(id) == unloaded,
          "last unload restores unloaded status");
  }
  const auto* internal_symbol = symbols.Find(
      {"hHrGoGoNf+s", "Sysmodule", 1, "Sysmodule", 1, 1, Loader::SymbolType::Func});
  Check(internal_symbol != nullptr, "internal Sysmodule load export resolves");
  const auto internal_load =
      reinterpret_cast<int (KYTY_SYSV_ABI *)(uint16_t, int, int, int, int*)>(
          internal_symbol->vaddr);
  int result = -1;
  Check(internal_load(0x115, 0, 0, 0, &result) == Libs::LibKernel::KERNEL_ERROR_ENOENT &&
            result == Libs::LibKernel::KERNEL_ERROR_ENOENT && is_loaded(0x115) == unloaded,
        "internal module loading propagates missing native module failure");
  Check(internal_load(0xb4, 0, 0, 0, &result) == OK && result == OK &&
            is_loaded(0xb4) == OK && unload(0xb4) == OK && is_loaded(0xb4) == unloaded,
        "internal and public module operations share load state");
}

void TestSystemServiceEntitlementEvents() {
  Loader::SymbolDatabase symbols;
  Libs::InitSystemService_1(&symbols);
  Libs::InitAppContent_1(&symbols);
  const auto resolve = [&](const char* nid, const char* library, const char* module) {
    const auto* symbol = symbols.Find(
        {nid, library, 1, module, 1, 1, Loader::SymbolType::Func});
    Check(symbol != nullptr, "entitlement event export resolves");
    return symbol->vaddr;
  };
  const auto initialize = reinterpret_cast<int (KYTY_SYSV_ABI *)(const void*, void*)>(
      resolve("R9lA82OraNs", "AppContent", "AppContentUtil"));
  struct Status {
    int32_t event_num;
    bool overlay, background, vr;
    uint8_t reserved[127];
  };
  struct Event {
    int32_t type;
    uint8_t data[8192];
  };
  static_assert(sizeof(Status) == 136 && sizeof(Event) == 8196);
  const auto get_status = reinterpret_cast<int (KYTY_SYSV_ABI *)(Status*)>(
      resolve("rPo6tV8D9bM", "SystemService", "SystemService"));
  const auto receive = reinterpret_cast<int (KYTY_SYSV_ABI *)(Event*)>(
      resolve("656LMQSrg6U", "SystemService", "SystemService"));
  Status status {};
  Event event {};
  Check(get_status(&status) == OK && status.event_num == 0,
        "SystemService starts without pending events");
  std::array<uint8_t, 32> init {};
  std::array<uint8_t, 40> boot {};
  Check(initialize(init.data(), boot.data()) == OK,
        "AppContent initialization generates an entitlement notification");
  Check(get_status(&status) == OK && status.event_num == 1 &&
            get_status(&status) == OK && status.event_num == 1,
        "status queries preserve pending notifications");
  Check(receive(nullptr) == Libs::SystemService::SYSTEM_SERVICE_ERROR_PARAMETER &&
            get_status(&status) == OK && status.event_num == 1,
        "invalid event output does not consume a notification");
  std::memset(&event, 0xff, sizeof(event));
  Check(receive(&event) == OK && event.type == 0x10000003 &&
            std::all_of(std::begin(event.data), std::end(event.data),
                        [](uint8_t byte) { return byte == 0; }) &&
            get_status(&status) == OK && status.event_num == 0,
        "receive delivers the entitlement event with cleared payload exactly once");
  Check(receive(&event) == Libs::SystemService::SYSTEM_SERVICE_ERROR_NO_EVENT,
        "empty event queue reports NO_EVENT");
}

void TestAddContent(const std::filesystem::path& root) {
  const auto folder = root / "add-ons";
  const auto write = [](const std::filesystem::path& path, const std::string& text) {
    std::filesystem::create_directories(path.parent_path());
    Common::File file;
    Check(file.Create(path), "create add-on fixture");
    uint32_t written = 0;
    file.Write(text.data(), static_cast<uint32_t>(text.size()), &written);
    Check(written == text.size(), "write add-on fixture");
  };
  const auto label = [](unsigned index) {
    std::array<char, 17> value {};
    std::snprintf(value.data(), value.size(), "DLC%013u", index);
    return std::string(value.data());
  };
  const auto param = [](const std::string& title, const std::string& entitlement) {
    return "{\"contentId\":\"UP0001-" + title + "_00-" + entitlement + "\"}";
  };
  write(folder / "foreign" / "sce_sys" / "param.json", param("PPSA22222", label(0)));
  write(folder / "foreign-label" / "sce_sys" / "param.json",
        param("PPSA22222", "PPSA11111XXXXXXX"));
  write(folder / "outside" / "param.json", param("PPSA11111", label(66)));
  write(folder / "invalid" / "sce_sys" / "param.json", "{\"contentId\":17}");
  write(folder / "malformed" / "sce_sys" / "param.json", "{broken");
  write(folder / "short-id" / "sce_sys" / "param.json", param("PPSA11111", "SHORT"));
  write(folder / "base-game" / "sce_sys" / "param.json",
        "{\"contentId\":\"UP0001-PPSA11111_00-BASE000000000000\",\"applicationCategoryType\":0}");
  for (unsigned i = 0; i < 65; ++i) {
    write(folder / label(i) / "sce_sys" / "param.json", param("PPSA11111", label(i)));
  }
  write(folder / "duplicate" / "sce_sys" / "param.json", param("PPSA11111", label(1)));
  write(folder / label(0) / "payload.txt", "DLC payload");

  Loader::AddContentLoad(folder, {}, "");
  Check(Loader::AddContentGetAll().empty(), "unknown title never exposes unrelated add-ons");
  Loader::AddContentLoad(folder, {}, "PPSA11111");
  Check(Loader::AddContentGetAll().size() == 65,
        "scan filters title before deduplication and ignores invalid/non-package metadata");

  struct Label { char data[17]; char padding[3]; };
  struct Info { Label label; uint32_t package_type; uint32_t download_status; };
  struct MountPoint { char data[16]; };
  static_assert(sizeof(Label) == 20 && sizeof(Info) == 28 && sizeof(MountPoint) == 16);
  Loader::SymbolDatabase symbols;
  Libs::InitAppContent_1(&symbols);
  Libs::LibNpEntitlementAccess::InitNet_1_NpEntitlementAccess(&symbols);
  const auto resolve = [&](const char* nid) {
    const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "add-on export resolves");
    return symbol->vaddr;
  };
  const auto initialize = reinterpret_cast<int (KYTY_SYSV_ABI *)(const void*, void*)>(
      resolve("jO8DM8oyego"));
  const auto list = reinterpret_cast<int (KYTY_SYSV_ABI *)(uint32_t, Info*, uint32_t, uint32_t*)>(
      resolve("TFyU+KFBv54"));
  const auto get = reinterpret_cast<int (KYTY_SYSV_ABI *)(uint32_t, const Label*, Info*)>(
      resolve("xddD23+8TfQ"));
  const auto mount = reinterpret_cast<int (KYTY_SYSV_ABI *)(uint32_t, const Label*, MountPoint*)>(
      resolve("VANhIWcqYak"));
  const auto unmount = reinterpret_cast<int (KYTY_SYSV_ABI *)(const MountPoint*)>(
      resolve("3rHWaV-1KC4"));
  std::array<char, 32> init {}, boot {};
  Check(initialize(init.data(), boot.data()) == OK, "initialize add-on entitlements");
  uint32_t count = 0;
  Check(list(0, nullptr, 0, &count) == OK && count == 68,
        "query existing and detected add-ons together");
  Check(list(0, nullptr, 1, &count) == OK && count == 68,
        "count-only query ignores output capacity");
  Check(list(1, nullptr, 0, &count) == OK && count == 68,
        "existing service-label handling remains unchanged");
  std::array<Info, 5> info {};
  info[4].package_type = 99;
  Check(list(0, info.data(), 4, &count) == OK && count == 68 && info[4].package_type == 99,
        "truncated entitlement list reports total and respects output capacity");
  constexpr std::array<const char*, 3> existing {"85y-je", "5d5c48", "_mtqu6"};
  Info single {};
  for (size_t i = 0; i < existing.size(); ++i) {
    Check(std::strcmp(info[i].label.data, existing[i]) == 0 &&
              info[i].package_type == 3 && info[i].download_status == 4 &&
              get(1, &info[i].label, &single) == OK &&
              std::memcmp(&single, &info[i], sizeof(single)) == 0,
          "GTA V entitlements retain their order, metadata, and individual lookups");
  }
  Check(std::strcmp(info[3].label.data, label(0).c_str()) == 0 &&
            info[3].package_type == 2 && info[3].download_status == 4 &&
            get(0, &info[3].label, &single) == OK &&
            std::memcmp(&single, &info[3], sizeof(single)) == 0,
        "detected add-ons follow existing entitlements with matching individual lookups");
  Label missing {};
  std::memcpy(missing.data, "MISSING000000000", 16);
  MountPoint point {};
  Check(get(0, &missing, &single) == static_cast<int>(0x817d0007u) &&
            mount(0, &missing, &point) == static_cast<int>(0x80d90005u),
        "unknown add-on returns entitlement and mount errors");
  Check(mount(0, nullptr, &point) == static_cast<int>(0x80d90002u) &&
            list(0, nullptr, 0, nullptr) == static_cast<int>(0x817d0002u),
        "null add-on parameters return parameter errors");
  Check(mount(0, &info[3].label, &point) == OK &&
            !std::string_view(point.data).ends_with('/'),
        "mount owned add-on without trailing slash");
  MountPoint repeated {};
  Check(mount(0, &info[3].label, &repeated) == static_cast<int>(0x80d90003u),
        "duplicate mount reports BUSY");
  const std::string mounted_file = std::string(point.data) + "/payload.txt";
  const int fd = FileSystem::KernelOpen(mounted_file.c_str(), 0, 0);
  std::array<char, 11> payload {};
  Check(fd >= 3 && FileSystem::KernelRead(fd, payload.data(), payload.size()) == payload.size() &&
            std::string_view(payload.data(), payload.size()) == "DLC payload" &&
            FileSystem::KernelClose(fd) == OK,
        "guest reads the original mount after a rejected duplicate mount");
  Check(unmount(&point) == OK && FileSystem::GetRealFilename(mounted_file).empty() &&
            unmount(&point) == static_cast<int>(0x80d90004u),
        "unmount removes mapping and repeated unmount reports NOT_MOUNTED");
  std::array<MountPoint, 64> mounts {};
  for (unsigned i = 0; i < mounts.size(); ++i) {
    Label entitlement {};
    std::memcpy(entitlement.data, label(i).c_str(), 17);
    Check(mount(0, &entitlement, &mounts[i]) == OK, "mount up to the simultaneous mount limit");
  }
  Label extra {};
  std::memcpy(extra.data, label(64).c_str(), 17);
  Check(mount(0, &extra, &point) == static_cast<int>(0x80d90006u),
        "65th simultaneous mount returns MOUNT_FULL");
  Check(unmount(&mounts[0]) == OK && mount(0, &extra, &point) == OK,
        "freed mount slot can be reused");
  Check(unmount(&point) == OK, "unmount reused slot");
  for (size_t i = 1; i < mounts.size(); ++i) {
    Check(unmount(&mounts[i]) == OK, "unmount fixture");
  }

  const auto user_dir = root / "user-add-on";
  write(user_dir / "payload.txt", "user payload");
  const auto user_arg = label(0) + "=" + Common::PathToString(user_dir);
  Check(!Common::ParseAddContent("SHORT") && !Common::ParseAddContent("lowercase0000000") &&
            !Common::ParseAddContent("LICENSE0000000001") &&
            !Common::ParseAddContent(user_arg + "/missing"),
        "user add-on needs a 16-character A-Z/0-9 label and an existing folder");
  const auto licensed = Common::ParseAddContent("LICENSE000000001");
  const auto replaced = Common::ParseAddContent(user_arg);
  const auto duplicate =
      Common::ParseAddContent("LICENSE000000001=" + Common::PathToString(user_dir));
  Check(licensed && replaced && duplicate, "user add-ons parse with and without extra data");
  Loader::AddContentLoad(folder, {*duplicate, *licensed, *replaced}, "PPSA11111");
  Check(list(0, nullptr, 0, &count) == OK && count == 69,
        "user add-ons replace detected labels and append new ones");
  Label user {};
  std::memcpy(user.data, "LICENSE000000001", 17);
  Check(get(0, &user, &single) == OK && single.package_type == 3 && single.download_status == 0 &&
            mount(0, &user, &point) == static_cast<int>(0x80d90005u),
        "last entry of a label wins and a license-only add-on has no extra data to mount");
  Check(mount(0, &info[3].label, &point) == OK, "user add-on mounts its extra data");
  const std::string user_file = std::string(point.data) + "/payload.txt";
  const int user_fd = FileSystem::KernelOpen(user_file.c_str(), 0, 0);
  std::array<char, 12> user_payload {};
  Check(user_fd >= 3 &&
            FileSystem::KernelRead(user_fd, user_payload.data(), user_payload.size()) ==
                user_payload.size() &&
            std::string_view(user_payload.data(), user_payload.size()) == "user payload" &&
            FileSystem::KernelClose(user_fd) == OK && unmount(&point) == OK,
        "user add-on replaces the detected folder of its label");

  Loader::AddContentLoad(folder / "missing", {}, "PPSA11111");
  Check(list(0, info.data(), 4, &count) == OK && count == 3,
        "missing add-ons folder retains GTA V entitlements");
  for (size_t i = 0; i < existing.size(); ++i) {
    Check(std::strcmp(info[i].label.data, existing[i]) == 0 &&
              get(0, &info[i].label, &single) == OK,
          "GTA V entitlement lookup works without detected add-ons");
  }
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

void TestFsync() {
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto* symbol = symbols.Find(
      {"juWbTNM+8hw", "Posix", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
  Check(symbol != nullptr, "POSIX fsync export resolves");
  const auto fsync = reinterpret_cast<int (KYTY_SYSV_ABI *)(int)>(symbol->vaddr);
  Check(fsync(std::numeric_limits<int>::min()) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
        "invalid fsync descriptor reports POSIX EBADF");

  constexpr char Path[] = "/savedata0/fsync.dat";
  const int fd = FileSystem::KernelOpen(Path, 0x602, 0777);
  Check(fd >= 3 && FileSystem::KernelWrite(fd, "save", 4) == 4 && fsync(fd) == OK &&
            FileSystem::KernelLseek(fd, 0, 1) == 4,
        "fsync flushes a writable file without changing its offset");
  Check(FileSystem::KernelClose(fd) == OK && fsync(fd) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF,
        "closed fsync descriptor reports POSIX EBADF");
}

void TestSaveTruncate() {
  namespace Kernel = Libs::LibKernel;
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto* symbol = symbols.Find(
      {"WlyEA-sLDf0", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
  Check(symbol != nullptr, "sceKernelTruncate export resolves");
  const auto truncate =
      reinterpret_cast<int (KYTY_SYSV_ABI *)(const char*, int64_t)>(symbol->vaddr);
  constexpr char Path[] = "/savedata0/truncate-save.dat";
  constexpr char Payload[] = "profile data";
  const int fd = FileSystem::KernelOpen(Path, 0x602, 0777);
  Check(fd >= 3 && FileSystem::KernelWrite(fd, Payload, sizeof(Payload) - 1) ==
                       sizeof(Payload) - 1,
        "populate path truncation fixture");
  FileSystem::FileStat stat {};
  std::array<char, 8> bytes {};
  Check(truncate(Path, 3) == OK &&
            FileSystem::KernelFstat(fd, &stat) == OK && stat.st_size == 3 &&
            FileSystem::KernelLseek(fd, 0, 1) == sizeof(Payload) - 1 &&
            FileSystem::KernelPread(fd, bytes.data(), bytes.size(), 0) == 3 &&
            std::memcmp(bytes.data(), Payload, 3) == 0,
        "path truncate discards the tail without moving an open descriptor");
  bytes.fill('x');
  Check(truncate(Path, bytes.size()) == OK &&
            FileSystem::KernelFstat(fd, &stat) == OK && stat.st_size == bytes.size() &&
            FileSystem::KernelPread(fd, bytes.data(), bytes.size(), 0) == bytes.size() &&
            std::memcmp(bytes.data(), Payload, 3) == 0 &&
            std::all_of(bytes.begin() + 3, bytes.end(), [](char byte) { return byte == 0; }) &&
            FileSystem::KernelLseek(fd, 0, 1) == sizeof(Payload) - 1,
        "path truncate extends with zero bytes and preserves the descriptor position");
  Check(truncate(Path, -1) == Kernel::KERNEL_ERROR_EINVAL &&
            truncate(nullptr, 0) == Kernel::KERNEL_ERROR_EFAULT &&
            truncate("truncate-save.dat", 0) == Kernel::KERNEL_ERROR_EINVAL &&
            truncate("/savedata0/missing-save.dat", 0) == Kernel::KERNEL_ERROR_ENOENT &&
            truncate("/savedata0", 0) == Kernel::KERNEL_ERROR_EISDIR &&
            truncate("/savedata0/truncate-save.dat/child", 0) == Kernel::KERNEL_ERROR_ENOTDIR &&
            FileSystem::KernelFstat(fd, &stat) == OK && stat.st_size == bytes.size() &&
            FileSystem::KernelLseek(fd, 0, 1) == sizeof(Payload) - 1,
        "path truncate errors preserve file size and descriptor position");
  Check(truncate("/dev/urandom", 0) == OK,
        "path truncate is a no-op for a nonregular nondirectory file");
  std::string long_path = "/savedata0";
  long_path.resize(1024, '/');
  Check(truncate(long_path.c_str(), 0) == Kernel::KERNEL_ERROR_ENAMETOOLONG,
        "path truncate rejects a pathname beyond the guest length limit");
  long_path.pop_back();
  Check(truncate(long_path.c_str(), 0) == Kernel::KERNEL_ERROR_EISDIR &&
            truncate((std::string("/savedata0/") + std::string(256, 'a')).c_str(), 0) ==
                Kernel::KERNEL_ERROR_ENAMETOOLONG,
        "path truncate permits the pathname length limit and bounds each component");
  Check(FileSystem::KernelClose(fd) == OK, "close path truncation fixture");
}

void TestSaveWritev() {
  namespace Kernel = Libs::LibKernel;
  Loader::SymbolDatabase symbols;
  Libs::InitLibKernel_1(&symbols);
  const auto resolve = [&](const char* nid) {
    const auto* symbol = symbols.Find(
        {nid, "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
    Check(symbol != nullptr, "vectored write export resolves");
    return symbol->vaddr;
  };
  const auto writev = reinterpret_cast<int64_t (KYTY_SYSV_ABI *)(
      int, const FileSystem::KernelIovec*, int)>(resolve("kAt6VDbHmro"));
  const auto pwritev = reinterpret_cast<int64_t (KYTY_SYSV_ABI *)(
      int, const FileSystem::KernelIovec*, int, int64_t)>(resolve("mBd4AfLP+u8"));
  constexpr char Path[] = "/savedata0/vector-save.dat";
  char first[] = "abc";
  char second[] = "DE";
  const std::array<FileSystem::KernelIovec, 3> buffers {{
      {first, 3}, {nullptr, 0}, {second, 2}}};
  const int fd = FileSystem::KernelOpen(Path, 0x602, 0777);
  Check(fd >= 3 && FileSystem::KernelWrite(fd, "_", 1) == 1 &&
            writev(fd, buffers.data(), buffers.size()) == 5 &&
            FileSystem::KernelLseek(fd, 0, 1) == 6,
        "writev writes consecutive segments and advances the descriptor position");
  std::array<char, 6> bytes {};
  Check(FileSystem::KernelPread(fd, bytes.data(), bytes.size(), 0) == bytes.size() &&
            std::memcmp(bytes.data(), "_abcDE", bytes.size()) == 0,
        "writev preserves the prefix and writes every nonempty segment in order");
  char replacement[] = "12";
  const std::array<FileSystem::KernelIovec, 2> patch {{
      {replacement, 1}, {replacement + 1, 1}}};
  Check(pwritev(fd, patch.data(), patch.size(), 1) == 2 &&
            FileSystem::KernelLseek(fd, 0, 1) == 6 &&
            FileSystem::KernelPread(fd, bytes.data(), bytes.size(), 0) == bytes.size() &&
            std::memcmp(bytes.data(), "_12cDE", bytes.size()) == 0,
        "pwritev shares segment ordering while preserving the descriptor position");

  const std::array<FileSystem::KernelIovec, 2> invalid {{
      {first, 1}, {nullptr, 1}}};
  const std::array<FileSystem::KernelIovec, 2> oversized {{
      {first, 1}, {second, static_cast<size_t>(std::numeric_limits<int>::max())}}};
  Check(writev(fd, nullptr, 1) == Kernel::KERNEL_ERROR_EFAULT &&
            writev(fd, invalid.data(), invalid.size()) == Kernel::KERNEL_ERROR_EFAULT &&
            writev(fd, buffers.data(), -1) == Kernel::KERNEL_ERROR_EINVAL &&
            writev(fd, buffers.data(), 1025) == Kernel::KERNEL_ERROR_EINVAL &&
            writev(fd, oversized.data(), oversized.size()) == Kernel::KERNEL_ERROR_EINVAL &&
            pwritev(fd, patch.data(), patch.size(), -1) == Kernel::KERNEL_ERROR_EINVAL &&
            writev(-1, buffers.data(), buffers.size()) == Kernel::KERNEL_ERROR_EBADF &&
            writev(fd, nullptr, 0) == 0 &&
            FileSystem::KernelLseek(fd, 0, 1) == 6 &&
            FileSystem::KernelPread(fd, bytes.data(), bytes.size(), 0) == bytes.size() &&
            std::memcmp(bytes.data(), "_12cDE", bytes.size()) == 0,
        "invalid vectors fail before writing any segment or moving the descriptor");
  const int reader = FileSystem::KernelOpen(Path, 0, 0);
  Check(reader >= 3 && writev(reader, buffers.data(), buffers.size()) ==
                           Kernel::KERNEL_ERROR_EBADF &&
            FileSystem::KernelLseek(reader, 0, 1) == 0 &&
            FileSystem::KernelClose(reader) == OK,
        "writev rejects a read-only descriptor without moving it");

  const int append = FileSystem::KernelOpen(Path, 0xa, 0);
  Check(append >= 3 && FileSystem::KernelLseek(append, 1, 0) == 1 &&
            writev(append, buffers.data(), buffers.size()) == 5 &&
            FileSystem::KernelLseek(append, 0, 1) == 11 &&
            FileSystem::KernelLseek(append, 1, 0) == 1 &&
            pwritev(append, patch.data(), patch.size(), 0) == 2 &&
            FileSystem::KernelLseek(append, 0, 1) == 1,
        "append vectored writes target EOF while positional writes preserve the cursor");
  std::array<char, 13> appended {};
  Check(FileSystem::KernelPread(fd, appended.data(), appended.size(), 0) == appended.size() &&
            std::memcmp(appended.data(), "_12cDEabcDE12", appended.size()) == 0,
        "append writes retain the original contents and ordered segments");
  Check(FileSystem::KernelClose(append) == OK && FileSystem::KernelClose(fd) == OK,
        "close vectored write fixtures");
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

void TestNpCommerceDialog() {
  Loader::SymbolDatabase symbols;
  Libs::LibNpCommerce::InitNet_1_NpCommerce(&symbols);
  const auto find = [&](const char *nid) {
    const auto *symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "NpCommerce dialog exports resolve");
    return symbol->vaddr;
  };
  using Call = int (KYTY_SYSV_ABI *)();
  using Open = int (KYTY_SYSV_ABI *)(const void *);
  using GetResult = int (KYTY_SYSV_ABI *)(void *);
  const auto initialize = reinterpret_cast<Call>(find("0aR2aWmQal4"));
  const auto terminate = reinterpret_cast<Call>(find("m-I92Ab50W8"));
  const auto update = reinterpret_cast<Call>(find("LR5cwFMMCVE"));
  const auto open = reinterpret_cast<Open>(find("DfSCDRA3EjY"));
  const auto get_result = reinterpret_cast<GetResult>(find("r42bWcQbtZY"));
  struct BaseParam {
    uint64_t size;
    uint8_t reserved[36];
    uint32_t magic;
  };
  struct Param {
    BaseParam base;
    int32_t size, user_id, mode;
    uint32_t service_label;
    const char *const *targets;
    uint32_t num_targets, padding;
    uint64_t features;
    void *user_data;
    uint8_t reserved[32];
  } param {};
  struct Result {
    int32_t result;
    bool authorized;
    void *user_data;
    uint8_t reserved[32];
  } result {};
  static_assert(sizeof(Param) == 128 && offsetof(Param, user_data) == 88);
  static_assert(sizeof(Result) == 48 && offsetof(Result, user_data) == 8);
  param.base.size = sizeof(BaseParam);
  param.base.magic = static_cast<uint32_t>(0xC0D1A109u + reinterpret_cast<uintptr_t>(&param.base));
  param.size = sizeof(param);
  param.user_id = 1;
  param.user_data = &symbols;
  Check(open(&param) == static_cast<int>(0x80B80003u) &&
            get_result(&result) == static_cast<int>(0x80B80003u),
        "NpCommerce dialog calls require initialization");
  Check(initialize() == OK && update() == 1 &&
            get_result(&result) == static_cast<int>(0x80B80005u),
        "NpCommerce dialog result requires completion");
  Check(open(nullptr) == static_cast<int>(0x80B8000Du) && update() == 1 &&
            get_result(nullptr) == static_cast<int>(0x80B8000Du),
        "NpCommerce dialog rejects null arguments");
  Check(open(&param) == OK && update() == 3, "NpCommerce dialog finishes without a store");
  param.user_data = nullptr;
  result.authorized = true;
  Check(get_result(&result) == 1 && result.result == 1 && !result.authorized &&
            result.user_data == &symbols,
        "NpCommerce cancellation returns the original user data");
  param.mode = 5;
  Check(open(&param) == OK && get_result(&result) == 1 && !result.authorized &&
            result.user_data == nullptr,
        "NpCommerce dialog can reopen with fresh result data");
  const auto invalid_param = [&] {
    Check(open(&param) == static_cast<int>(0x80B8000Au) && update() == 3 &&
              get_result(&result) == static_cast<int>(0x80B8000Au) &&
              result.result == static_cast<int>(0x80B8000Au) && !result.authorized,
          "NpCommerce dialog reports invalid parameters in the completion result");
  };
  param.mode = 99;
  invalid_param();
  param.mode = -1;
  invalid_param();
  param.mode = 0;
  param.size = 64;
  invalid_param();
  param.size = sizeof(param);
  param.reserved[0] = 1;
  invalid_param();
  param.reserved[0] = 0;
  Check(open(&param) == OK && get_result(&result) == 1 && terminate() == OK && update() == 0 &&
            get_result(&result) == static_cast<int>(0x80B80003u),
        "NpCommerce dialog recovers from invalid parameters and terminates");
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
  Check(FileSystem::KernelFsync(fd) == OK &&
            FileSystem::KernelLseek(fd, 0, 1) == payload.size(),
        "fsync succeeds on an immutable archive member without changing its offset");
  Check(FileSystem::KernelWrite(fd, bytes.data(), 1) ==
            Libs::LibKernel::KERNEL_ERROR_EBADF &&
            FileSystem::KernelPwrite(fd, bytes.data(), 1, 0) ==
            Libs::LibKernel::KERNEL_ERROR_EBADF &&
            FileSystem::KernelFtruncate(fd, 0) ==
            Libs::LibKernel::KERNEL_ERROR_EBADF,
        "archive read-only descriptors reject write and truncate");
  Check(FileSystem::KernelTruncate(GuestMember, 0) ==
            Libs::LibKernel::KERNEL_ERROR_EROFS &&
            FileSystem::KernelFstat(fd, &descriptor_stat) == OK &&
            descriptor_stat.st_size == payload.size() &&
            FileSystem::KernelLseek(fd, 0, 1) == payload.size(),
        "path truncate rejects a read-only archive and preserves its member");

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
  using WaitCounter = int (KYTY_SYSV_ABI *)(void *, uint8_t, uint8_t, uint64_t,
                                          uint8_t, uint8_t, uint64_t, uint8_t);
  using WriteCounter = int (KYTY_SYSV_ABI *)(void *, uint8_t, uint8_t, uint64_t,
                                           uint8_t, uint32_t);
  using WriteAddress = int (KYTY_SYSV_ABI *)(void *, volatile uint64_t *, uint64_t);
  using WriteImmediately = int (KYTY_SYSV_ABI *)(void *, volatile uint64_t *, uint64_t,
                                               uint32_t);
  using MeasureWriteAddress = uint64_t (KYTY_SYSV_ABI *)(volatile uint64_t *, uint64_t);
  using MeasureWriteImmediately = uint64_t (KYTY_SYSV_ABI *)(volatile uint64_t *, uint64_t,
                                                           uint32_t);
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
  const auto wait_counter = reinterpret_cast<WaitCounter>(find("cQb8Zr8Q0Y0"));
  const auto write_counter = reinterpret_cast<WriteCounter>(find("jK+yuYCI7MA"));
  const auto write_address = reinterpret_cast<WriteAddress>(find("sJXyWHjP-F8"));
  const auto write_immediately = reinterpret_cast<WriteImmediately>(find("j0+3uJMxYJY"));
  const auto measure_write_address = reinterpret_cast<MeasureWriteAddress>(find("C+IEj+BsAFM"));
  const auto measure_write_immediately = reinterpret_cast<MeasureWriteImmediately>(find("4fgtGfXDrFc"));
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
  const auto reset = [&](size_t apr_count, uint32_t data_size = sizeof(Buffer::data)) {
    for (size_t i = 0; i < buffers.size(); ++i) {
      auto &buffer = buffers[i];
      construct(buffer.header.data());
      if (i < apr_count) {
        construct_apr(buffer.header.data(), &buffer.header[3], &buffer.header[4]);
      } else {
        construct_amm(buffer.header.data());
      }
      Check(set_buffer(buffer.header.data(), buffer.data.data(), data_size) == OK,
            "initialize AMPR command buffer");
    }
  };
  struct LiteralWrite { uint64_t value; uint32_t size; };
  constexpr std::array literal_writes {
      LiteralWrite{0, 8}, LiteralWrite{3, 8}, LiteralWrite{4, 12},
      LiteralWrite{(uint64_t{1} << 34) - 1, 12},
      LiteralWrite{uint64_t{1} << 34, 16}, LiteralWrite{UINT64_MAX, 16}};
  for (const auto &literal : literal_writes) {
    for (const bool immediately : std::array{false, true}) {
      reset(1, literal.size);
      auto *command_buffer = buffers[0].header.data();
      uint64_t fence = ~literal.value;
      const auto append = [&](uint64_t value) {
        return immediately ? write_immediately(command_buffer, &fence, value, 1)
                           : write_address(command_buffer, &fence, value);
      };
      Check(measure_write_address(&fence, literal.value) == literal.size &&
                measure_write_immediately(&fence, literal.value, 1) == literal.size &&
                append(literal.value) == OK && offset(command_buffer) == literal.size,
            "literal write fits exactly its measured command size");
      Check(append(~literal.value) == Libs::LibKernel::KERNEL_ERROR_EBUSY &&
                offset(command_buffer) == literal.size,
            "rejected write preserves the full command buffer offset");
      Result result {1234, 5678};
      uint32_t submission = 0;
      Check(submit_apr(command_buffer, 3, &result, &submission) == OK &&
                wait_apr(submission) == OK && result.result == OK &&
                fence == literal.value,
            "bounded write preserves all sixty-four literal bits");
    }
  }
  reset(1, 768);
  std::array<std::array<char, 3>, 16> batch_output {};
  std::array<uint64_t, 16> batch_fences {};
  auto *batch = buffers[0].header.data();
  for (size_t i = 0; i < batch_fences.size(); ++i) {
    Check(read_file(batch, reinterpret_cast<uint64_t>(&buffers[0].header[3]),
                    reinterpret_cast<uint64_t>(&buffers[0].header[4]), file_id,
                    batch_output[i].data(), batch_output[i].size(), 0) == OK &&
              write_address(batch, &batch_fences[i], i + 1) == OK,
          "sixteen APR read/fence pairs fit the guest's 768-byte command buffer");
  }
  Result batch_result {1234, 5678};
  uint32_t batch_submission = 0;
  Check(submit_apr(batch, 3, &batch_result, &batch_submission) == OK &&
            wait_apr(batch_submission) == OK && batch_result.result == OK,
        "complete the bounded APR read batch");
  for (size_t i = 0; i < batch_fences.size(); ++i) {
    Check(batch_fences[i] == i + 1 &&
              std::memcmp(batch_output[i].data(), "APR", 3) == 0,
          "every read publishes its data and completion, including requests fourteen and fifteen");
  }
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
  constexpr std::array counter_comparisons {
      Comparison{1, 0, 0, 1}, Comparison{1, 0, 0, 1},
      Comparison{4, 0x7ffffffe, 0x7fffffff, 0x80000000},
      Comparison{5, 0xffffffff, 0, 1}};
  for (const auto &comparison : counter_comparisons) {
    reset(1);
    auto *producer = buffers[0].header.data();
    auto *consumer = buffers[1].header.data();
    auto *lower = buffers[2].header.data();
    constexpr uint8_t Counter = 127;
    constexpr auto Invalid = Libs::LibKernel::KERNEL_ERROR_EINVAL;
    Check(write_counter(producer, 128, 1, 1, 0, 0) == Invalid &&
              write_counter(producer, Counter, 8, 1, 0, 0) == Invalid &&
              write_counter(producer, Counter, 1, 1, 5, 0) == Invalid &&
              write_counter(producer, Counter, 1, 1, 0, 2) == Invalid &&
              wait_counter(consumer, Counter, 8, 0, 1, 0, 0, 0) == Invalid &&
              wait_counter(consumer, Counter, 1, 0, 7, 0, 0, 0) == Invalid &&
              wait_counter(consumer, Counter, 1, 0, 1, 2, 0, 0) == Invalid &&
              wait_counter(consumer, Counter, 1, 0, 1, 0, 0, 2) == Invalid &&
              offset(producer) == 0 && offset(consumer) == 0,
          "invalid counter operations fail without appending a command");
    uint64_t retired = 0, lower_done = 0;
    std::array<char, 3> output {};
    Result result {1234, 5678};
    std::array<uint32_t, 4> ids {};
    if (comparison.blocked != 0) {
      auto *initializer = buffers[3].header.data();
      Check(write_counter(initializer, Counter, 1, comparison.blocked, 0, 0) == OK &&
                submit_amm(buffers[3].data.data(), static_cast<uint32_t>(offset(initializer)),
                           0, &ids[3]) == OK && wait_amm(ids[3]) == OK,
            "initialize shared counter before dependent submissions");
    }
    Check(read_file(producer, reinterpret_cast<uint64_t>(&buffers[0].header[3]),
                    reinterpret_cast<uint64_t>(&buffers[0].header[4]), file_id,
                    output.data(), output.size(), 0) == OK &&
              write_counter(producer, Counter, 1, comparison.released, 0, 0) == OK &&
              wait_counter(consumer, Counter, 1, comparison.reference, comparison.compare,
                           0, 0, 0) == OK &&
              write_counter(consumer, Counter, 1, 0, 0, 0) == OK &&
              write_address(consumer, &retired, 1) == OK &&
              write_address(lower, &lower_done, 1) == OK,
          "build APR completion and dependent AMM counter reset");
    Check(submit_amm(buffers[1].data.data(), static_cast<uint32_t>(offset(consumer)),
                     0, &ids[1]) == OK &&
              submit_amm(buffers[2].data.data(), static_cast<uint32_t>(offset(lower)),
                         1, &ids[2]) == OK && wait_amm(ids[2]) == OK &&
              lower_done == 1 && retired == 0,
          "counter wait blocks AMM retirement while its lower priority progresses");
    Check(submit_apr(producer, 3, &result, &ids[0]) == OK && wait_amm(ids[1]) == OK &&
              wait_apr(ids[0]) == OK && result.result == OK && retired == 1 &&
              std::memcmp(output.data(), "APR", 3) == 0,
          "shared counter completion releases AMM only after APR read and resets for reuse");
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

  // These paths share the old 31-bit FNV-1a ID (0x122d1544).
  const char *collision_paths[] = {"perf-audit/test_00015ddf.bin",
                                   "perf-audit/test_000389b8.bin"};
  Check(std::filesystem::create_directory(root / "perf-audit"), "create APR collision directory");
  for (size_t i = 0; i < 2; ++i) {
    Check(fixture.Create(root / collision_paths[i]), "create APR collision fixture");
    fixture.Write(i == 0 ? "APR" : "OTHER", i == 0 ? 3 : 5);
    fixture.Close();
  }
  uint32_t collision_ids[2] = {};
  Check(resolve("/app0/", collision_paths, 2, collision_ids, sizes, &error_index) == OK &&
            collision_ids[0] != collision_ids[1] && collision_ids[0] != 0xffffffffu &&
            collision_ids[1] != 0xffffffffu && sizes[0] == 3 && sizes[1] == 5,
        "APR assigns distinct valid IDs to colliding paths");
  const auto *stat_symbol = symbols.FindByNid("ApkYaHb8Sek", Loader::SymbolType::Func);
  const auto *size_symbol = symbols.FindByNid("WvEu7yl3Ivg", Loader::SymbolType::Func);
  Check(stat_symbol && size_symbol, "APR file metadata exports are registered");
  using Stat = int (KYTY_SYSV_ABI *)(uint32_t, FileSystem::FileStat *);
  using Size = int (KYTY_SYSV_ABI *)(uint32_t, uint64_t *);
  for (size_t i = 0; i < 2; ++i) {
    FileSystem::FileStat stat {};
    uint64_t size = 0;
    Check(resolve("/app0/", &collision_paths[i], 1, ids, nullptr, &error_index) == OK &&
              ids[0] == collision_ids[i] &&
              reinterpret_cast<Stat>(stat_symbol->vaddr)(ids[0], &stat) == OK &&
              reinterpret_cast<Size>(size_symbol->vaddr)(ids[0], &size) == OK &&
              stat.st_size == static_cast<int64_t>(sizes[i]) && size == sizes[i],
          "APR re-resolution preserves each file's ID, host path and cached size");
  }

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
  CheckAmprOrdering(symbols, collision_ids[0]);
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

#if defined(__linux__)
void KYTY_SYSV_ABI RecordNetCtlEvent(int event, void* arg) {
  auto& events = *static_cast<std::array<int, 2>*>(arg);
  ++events[0];
  events[1] = event;
}

void TestNetCtlHostIpv4() {
  using namespace Libs::Network::NetCtl;
  ifaddrs* interfaces = nullptr;
  Check(getifaddrs(&interfaces) == 0, "enumerate host IPv4 addresses");
  std::vector<uint32_t> host_addresses;
  for (auto* interface = interfaces; interface != nullptr; interface = interface->ifa_next) {
    if (interface->ifa_addr == nullptr || interface->ifa_addr->sa_family != AF_INET ||
        (interface->ifa_flags & IFF_UP) == 0 || (interface->ifa_flags & IFF_RUNNING) == 0 ||
        (interface->ifa_flags & IFF_LOOPBACK) != 0) {
      continue;
    }
    const auto address = reinterpret_cast<const sockaddr_in*>(interface->ifa_addr)->sin_addr;
    if (address.s_addr != 0 && (ntohl(address.s_addr) >> 24) != 127) {
      host_addresses.push_back(address.s_addr);
    }
  }
  freeifaddrs(interfaces);

  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Check(!options.network_connected, "host networking requires explicit opt-in");
  alignas(uint32_t) std::array<char, 256> storage {};
  auto* info = reinterpret_cast<NetCtlInfo*>(storage.data());
  constexpr int NotConnected = static_cast<int>(0x80412108u);
  Check(NetCtlGetInfo(14, nullptr) == static_cast<int>(0x80412107u) &&
            NetCtlGetInfo(0, info) == static_cast<int>(0x80412106u) &&
            NetCtlGetInfo(22, info) == static_cast<int>(0x80412106u),
        "NetCtl reports invalid output addresses and information codes");
  for (bool enabled : {false, true}) {
    options.network_connected = enabled;
    Config::Load(options);
    Check(NetCtlInit() == OK, "initialize NetCtl for selected network mode");
    const bool connected = enabled && !host_addresses.empty();
    int state = -1;
    Check(NetCtlGetState(&state) == OK && state == (connected ? 3 : 0),
          "NetCtl IPv4 state matches opt-in and host connectivity");
    Check(NetCtlGetStateV6(&state) == OK && state == 0,
          "IPv4 connectivity does not advertise IPv6 support");

    Check(NetCtlGetInfo(14, info) == (connected ? OK : NotConnected),
          "NetCtl IPv4 query respects network mode and host state");
    in_addr address {};
    if (connected) {
      Check(inet_pton(AF_INET, storage.data(), &address) == 1 &&
                std::find(host_addresses.begin(), host_addresses.end(), address.s_addr) !=
                    host_addresses.end(),
            "NetCtl reports an IPv4 address assigned to an active host interface");
    }
    Check(NetCtlGetInfo(15, info) == (connected ? OK : NotConnected) &&
              (!connected || inet_pton(AF_INET, storage.data(), &address) == 1),
          "NetCtl returns the active IPv4 netmask only in connected mode");
    const int gateway_result = NetCtlGetInfo(16, info);
    Check(gateway_result == NotConnected ||
              (connected && gateway_result == OK &&
               inet_pton(AF_INET, storage.data(), &address) == 1),
          "NetCtl reports a valid IPv4 gateway or its absence");

    std::array<int, 2> events {};
    int callback_id = -1;
    Check(NetCtlRegisterCallback(RecordNetCtlEvent, &events, &callback_id) == OK &&
              NetCtlCheckCallback() == OK && events[0] == 1 &&
              events[1] == (connected ? 3 : 1) && NetCtlCheckCallback() == OK && events[0] == 1,
          "NetCtl delivers the matching connection event once while state is unchanged");
    Check(NetCtlUnregisterCallback(callback_id) == OK, "release NetCtl callback");
    NetCtlTerm();
  }
  options.network_connected = false;
  Config::Load(options);
}
#endif

template <int Mode>
void KYTY_SYSV_ABI RecordNpState(int user_id, uint32_t state, void* arg) {
  auto& event = *static_cast<std::array<int, 3>*>(arg);
  ++event[0];
  event[1] = user_id;
  event[2] = static_cast<int>(state);
}

void TestNpEmulation() {
  using namespace Libs::Network::NpManager;
  using namespace Libs::Network::NetCtl;
  constexpr int SignedOut = static_cast<int>(0x80550006u);
  constexpr int NetworkUnavailable = static_cast<int>(0x8055a02au);
  constexpr int Aborted = static_cast<int>(0x80550012u);
  struct AsyncParameters {
    size_t size = sizeof(AsyncParameters);
    uint64_t affinity = 0;
    int priority = 0;
    uint8_t padding[4] {};
  } async_parameters;
  struct PremiumParameters {
    size_t size = sizeof(PremiumParameters);
    int user_id = 0;
    uint8_t padding[4] {};
    uint64_t features = 1;
    uint8_t reserved[32] {};
  } premium_parameters;
  struct PremiumResult {
    bool authorized = true;
    uint8_t reserved[32] {};
  } premium_result;

  const auto create_request = [&] (bool async) {
    const int request = async
        ? NpCreateAsyncRequest(reinterpret_cast<const NpCreateAsyncRequestParameter*>(
              &async_parameters))
        : NpCreateRequest();
    Check(request > 0, "create NP request");
    return request;
  };
  const auto check_request = [&] (bool async, int expected, auto operation) {
    const int request = create_request(async);
    Check(operation(request) == (async ? OK : expected),
          "NP call separates asynchronous submission from its execution result");
    if (async) {
      int result = 123;
      Check(NpPollAsync(request, &result) == OK && result == expected,
            "NP polling reports the completed operation result");
    }
    Check(NpDeleteRequest(request) == OK, "release completed NP request");
  };

  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Check(!options.network_connected && !options.psn_emulation_enabled,
        "host networking and PSN emulation both require explicit opt-in");
  static std::array<std::array<int, 3>, 4> events {};
  const std::array<NpStateCallbackA, 4> callbacks {
      RecordNpState<0>, RecordNpState<1>, RecordNpState<2>, RecordNpState<3>};
  int mode = 0;
  for (bool network : {false, true}) {
    int previous_network_state = -1;
    for (bool emulate : {false, true}) {
      options.network_connected = network;
      options.psn_emulation_enabled = emulate;
      Config::Load(options);
      Check(Config::IsNetworkConnected() == network && Config::PsnEmulationEnabled() == emulate,
            "loading PSN emulation does not change the independent network option");
      Check(NetCtlInit() == OK, "initialize network state for NP mode");
      int network_state = -1;
      Check(NetCtlGetState(&network_state) == OK && (network || network_state == 0),
            "PSN emulation cannot connect disabled host networking");
      Check(previous_network_state == -1 || previous_network_state == network_state,
            "PSN emulation does not change host network connectivity");
      previous_network_state = network_state;
      const bool connected = network_state == 3;

      Check(NpRegisterStateCallbackA(callbacks[mode], &events[mode]) > 0 &&
                NpCheckCallback() == OK && events[mode][0] == 1 &&
                events[mode][1] == Config::GetUserId() &&
                events[mode][2] == (emulate ? 2 : 1),
            "initial NP callback matches the selected emulated sign-in state");
      ++mode;
      for (int player = 0; player < Config::MAX_PLAYERS; ++player) {
        const int user_id = Config::GetPlayerUserId(player);
        const bool signed_in = emulate && player == 0;
        const int identity_result = signed_in ? OK : SignedOut;
        const int online_result = !signed_in ? SignedOut : connected ? OK : NetworkUnavailable;
        uint32_t state = 99;
        bool signed_up = false;
        Check(NpGetState(user_id, &state) == OK && state == (signed_in ? 2u : 1u) &&
                  NpHasSignedUp(user_id, &signed_up) == OK && signed_up == signed_in,
              "only the primary emulated user has a persistent signed-in account");
        Check(NpGetNpReachabilityState(user_id, &state) == OK &&
                  state == (signed_in && connected ? 2u : 0u),
              "NP reachability requires both the emulated account and host connectivity");

        uint64_t account_id = 0;
        std::array<char, 20> online_id {};
        std::array<char, 36> np_id {};
        std::array<char, 4> country {};
        Check(NpGetAccountIdA(user_id, &account_id) == identity_result &&
                  NpGetOnlineId(user_id, reinterpret_cast<NpOnlineId*>(online_id.data())) ==
                      identity_result &&
                  NpGetNpId(user_id, reinterpret_cast<NpId*>(np_id.data())) == identity_result &&
                  NpGetAccountCountryA(user_id, country.data()) == identity_result,
              "NP identity and account-country getters agree on primary and guest sign-in");
        if (signed_in) {
          Check(account_id != 0 && online_id[0] != 0 && online_id[16] == 0 &&
                    std::strcmp(online_id.data(), np_id.data()) == 0 &&
                    country[0] != 0 && country[1] != 0 && country[2] == 0,
                "emulated account identity and country remain usable while disconnected");
        }

        for (bool async : {false, true}) {
          check_request(async, online_result, [&] (int request) {
            return NpCheckNpReachability(request, user_id);
          });
          uint8_t age = 0;
          check_request(async, identity_result, [&] (int request) {
            return NpGetAccountAge(request, user_id, &age);
          });
          std::array<char, 48> language {};
          check_request(async, identity_result, [&] (int request) {
            return NpGetAccountLanguage2(request, user_id, language.data());
          });
          Check(!signed_in || (age > 0 && language[0] != 0 && language[35] == 0),
                "signed-in account metadata is available independently of host networking");
          premium_parameters.user_id = user_id;
          premium_result.authorized = true;
          check_request(async, online_result, [&] (int request) {
            return NpCheckPremium(request,
                reinterpret_cast<const NpCheckPremiumParameter*>(&premium_parameters),
                reinterpret_cast<NpCheckPremiumResult*>(&premium_result));
          });
          Check(!premium_result.authorized, "PSN emulation does not grant premium eligibility");
          if (player == 0) {
            check_request(async, online_result, [&] (int request) {
              return NpCheckNpAvailability(request, "Kyty", nullptr);
            });
          }
        }
      }
      const int aborted = create_request(true);
      int result = 123;
      Check(NpAbortRequest(aborted) == OK &&
                NpCheckNpReachability(aborted, Config::GetUserId()) == Aborted &&
                NpPollAsync(aborted, &result) == OK && result == Aborted &&
                NpDeleteRequest(aborted) == OK,
            "NP emulation does not turn an aborted request into a successful one");
      NetCtlTerm();
    }
  }
  Config::Load(Config::ConfigOptions {});
}

void TestNpAuthEmulation() {
  Loader::SymbolDatabase symbols;
  Libs::LibNpAuth::InitNet_1_NpAuth(&symbols);
  const auto resolve = [&] (const char* nid) {
    const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "resolve NP authentication exports");
    return symbol->vaddr;
  };
  const auto create = reinterpret_cast<int(KYTY_SYSV_ABI*)()>(resolve("6bwFkosYRQg"));
  const auto create_async = reinterpret_cast<int(KYTY_SYSV_ABI*)(const void*)>(
      resolve("N+mr7GjTvr8"));
  const auto destroy = reinterpret_cast<int(KYTY_SYSV_ABI*)(int)>(resolve("H8wG9Bk-nPc"));
  const auto poll = reinterpret_cast<int(KYTY_SYSV_ABI*)(int, int*)>(resolve("gjSyfzSsDcE"));
  const auto get_code = reinterpret_cast<int(KYTY_SYSV_ABI*)(int, const void*, void*, int*)>(
      resolve("KI4dHLlTNl0"));
  const auto get_token = reinterpret_cast<int(KYTY_SYSV_ABI*)(int, const void*, void*)>(
      resolve("RdsFVsgSpZY"));
  constexpr int InvalidArgument = static_cast<int>(0x80550301u);
  constexpr int InvalidSize = static_cast<int>(0x80550302u);
  constexpr int ServiceUnavailable = static_cast<int>(0x80550401u);
  constexpr int SignedOut = static_cast<int>(0x80550006u);
  struct AuthorizationCodeParameters {
    size_t size = sizeof(AuthorizationCodeParameters);
    int user_id = 0;
    uint8_t padding[4] {};
    const void* client_id = "test-client";
    const char* scope = "test-scope";
  } code_parameters;
  struct IdTokenParameters {
    size_t size = sizeof(IdTokenParameters);
    int user_id = 0;
    uint8_t padding[4] {};
    const void* client_id = "test-client";
    const void* client_secret = "test-secret";
    const char* scope = "test-scope";
  } token_parameters;
  std::array<uint8_t, 136> code {};
  std::array<uint8_t, 4104> token {};
  const std::array<uint64_t, 3> async_parameters {24, 0, 0};
  int issuer = 0;
  const int invalid_request = create();
  code_parameters.size = 0;
  token_parameters.size = 0;
  Check(invalid_request > 0 &&
            get_code(invalid_request, nullptr, code.data(), &issuer) == InvalidArgument &&
            get_token(invalid_request, nullptr, token.data()) == InvalidArgument &&
            get_code(invalid_request, &code_parameters, code.data(), &issuer) == InvalidSize &&
            get_token(invalid_request, &token_parameters, token.data()) == InvalidSize &&
            destroy(invalid_request) == OK,
        "NP authentication rejects missing and undersized user parameters");
  code_parameters.size = sizeof(code_parameters);
  token_parameters.size = sizeof(token_parameters);

  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  for (bool network : {false, true}) {
    for (bool emulate : {false, true}) {
      options.network_connected = network;
      options.psn_emulation_enabled = emulate;
      Config::Load(options);
      for (int player = 0; player < Config::MAX_PLAYERS; ++player) {
        code_parameters.user_id = Config::GetPlayerUserId(player);
        token_parameters.user_id = code_parameters.user_id;
        const int expected = emulate && player == 0 ? ServiceUnavailable : SignedOut;
        for (bool async : {false, true}) {
          for (bool id_token : {false, true}) {
            const int request = async ? create_async(async_parameters.data()) : create();
            Check(request > 0, "create NP authentication request");
            code.fill(0xff);
            token.fill(0xff);
            issuer = -1;
            const int result = id_token
                ? get_token(request, &token_parameters, token.data())
                : get_code(request, &code_parameters, code.data(), &issuer);
            Check(result == (async ? OK : expected),
                  "NP authentication distinguishes an unavailable service from sign-out");
            if (async) {
              int completion = 123;
              Check(poll(request, &completion) == OK && completion == expected,
                    "asynchronous NP authentication delivers the actual failure");
            }
            const auto zero = [] (uint8_t value) { return value == 0; };
            Check(id_token ? std::all_of(token.begin(), token.end(), zero)
                           : std::all_of(code.begin(), code.end(), zero) && issuer == 0,
                  "PSN emulation does not fabricate authorization codes or ID tokens");
            Check(destroy(request) == OK, "release NP authentication request");
          }
        }
      }
    }
  }
  Config::Load(Config::ConfigOptions {});
}

void CheckEtherAddressFormatting() {
  Loader::SymbolDatabase symbols;
  Libs::LibNet::InitNet_1_Net(&symbols);
  const auto *format_symbol = symbols.FindByNid("v6M4txecCuo", Loader::SymbolType::Func);
  const auto *errno_symbol = symbols.FindByNid("HQOwnfMGipQ", Loader::SymbolType::Func);
  Check(format_symbol && errno_symbol, "Ethernet formatting and errno exports resolve");
  using Format = int (KYTY_SYSV_ABI *)(const Libs::Network::Net::NetEtherAddr *, char *, size_t);
  using Errno = int *(KYTY_SYSV_ABI *)();
  const auto format = reinterpret_cast<Format>(format_symbol->vaddr);
  auto *net_errno = reinterpret_cast<Errno>(errno_symbol->vaddr)();
  for (const auto address : {Libs::Network::Net::NetEtherAddr{},
                             Libs::Network::Net::NetEtherAddr{{0x01, 0x23, 0x45, 0xab, 0xcd, 0xef}}}) {
    const auto *expected = address.data[0] == 0 ? "00:00:00:00:00:00" : "01:23:45:ab:cd:ef";
    for (const size_t size : {18u, 127u}) {
      std::array<char, 128> text;
      text.fill('!');
      Check(format(&address, text.data(), size) == OK &&
                std::strcmp(text.data(), expected) == 0 && text[18] == '!',
            "Ethernet formatting accepts exact and larger buffers");
    }
  }
  const Libs::Network::Net::NetEtherAddr address{};
  std::array<char, 18> text;
  text.fill('!');
  Check(format(&address, text.data(), 17) == Libs::Network::NET_ERROR_EINVAL &&
            *net_errno == Libs::Posix::POSIX_EINVAL &&
            std::all_of(text.begin(), text.end(), [](char c) { return c == '!'; }) &&
            format(nullptr, text.data(), text.size()) == Libs::Network::NET_ERROR_EINVAL &&
            format(&address, nullptr, text.size()) == Libs::Network::NET_ERROR_EINVAL,
        "Ethernet formatting rejects invalid arguments without writing output");
}

void CheckSocketWakeup() {
  namespace Net = Libs::Network::Net;
  Loader::SymbolDatabase symbols;
  Libs::LibNet::InitNet_1_Net(&symbols);
  Libs::InitLibKernel_1(&symbols);
  const auto *shutdown_symbol = symbols.Find(
      {"TUuiYS2kE8s", "Posix", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
  Check(shutdown_symbol != nullptr, "POSIX shutdown export resolves");
  const auto shutdown = reinterpret_cast<int (KYTY_SYSV_ABI *)(int, int)>(shutdown_symbol->vaddr);
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
  const auto [shutdown_reader, shutdown_writer] = CreateTcpPair();
  const auto ready_to_read = [](int fd) {
    std::array<uint64_t, 16> readable {};
    readable[fd / 64] = uint64_t {1} << (fd % 64);
    const std::array<int64_t, 2> timeout {2, 0};
    return Net::Select(fd + 1, readable.data(), nullptr, nullptr, timeout.data()) == 1;
  };
  Check(shutdown(-1, 1) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF &&
            shutdown(shutdown_writer, 3) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL,
        "shutdown rejects invalid descriptors and directions");
  Check(Net::Send(shutdown_writer, "x", 1, 0) == 1 && shutdown(shutdown_writer, 1) == 0 &&
            ready_to_read(shutdown_reader) &&
            Net::Recv(shutdown_reader, received.data(), 1, 0) == 1 && received[0] == 'x' &&
            ready_to_read(shutdown_reader) &&
            Net::Recv(shutdown_reader, received.data(), 1, 0) == 0 &&
            Net::Send(shutdown_reader, "y", 1, 0) == 1 &&
            ready_to_read(shutdown_writer) &&
            Net::Recv(shutdown_writer, received.data(), 1, 0) == 1 && received[0] == 'y',
        "write shutdown drains queued bytes, signals EOF, and preserves reverse traffic");
  Check(Net::SocketClose(shutdown_reader) == 0 && Net::SocketClose(shutdown_writer) == 0,
        "close half-shutdown sockets");
  const int encrypted = Net::Socket(2, 6, 0);
  int crypto = -1;
  uint32_t crypto_size = sizeof(crypto);
  Check(encrypted >= 0 &&
            Net::Getsockopt(encrypted, 0xffff, 0x20000, &crypto, &crypto_size) == 0 &&
            crypto == 0,
        "UDPP2P encryption defaults to disabled");
  for (const int setting : {7, 0}) {
    Check(Net::Setsockopt(encrypted, 0xffff, 0x20000, &setting, sizeof(setting)) == 0 &&
              Net::Getsockopt(encrypted, 0xffff, 0x20000, &crypto, &crypto_size) == 0 &&
              crypto == (setting != 0),
          "UDPP2P encryption configuration retains normalized enable and disable values");
    Check(Net::Send(encrypted, "x", 1, 0) == -1 &&
              *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EOPNOTSUPP,
          "UDPP2P configuration never exposes unframed plaintext transport");
  }
  std::array<uint8_t, 16> p2p_address {16, 2, 0x0e, 0x4a, 127, 0, 0, 1, 0x1e, 0x61};
  Check(Net::Bind(encrypted, p2p_address.data(), p2p_address.size()) == 0,
        "bind UDPP2P physical and virtual ports");
  char untouched = '!';
  auto source = p2p_address;
  uint32_t source_size = source.size();
  Check(Net::Recvfrom(encrypted, &untouched, 1, 0x80, source.data(), &source_size) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK,
        "per-call nonblocking UDPP2P receive reports an actually empty queue");
  Check(Net::Setsockopt(encrypted, 0xffff, 0x1200, &enabled, sizeof(enabled)) == 0 &&
            Net::Setsockopt(encrypted, 0xffff, 0x20000, &enabled, sizeof(enabled)) == 0 &&
            Net::Recvfrom(encrypted, &untouched, 1, 0, source.data(), &source_size) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK &&
            untouched == '!' && source == p2p_address && source_size == source.size(),
        "empty encrypted nonblocking receive preserves all caller outputs");
  const int raw_sender = Net::Socket(2, 2, 0);
  Check(raw_sender >= 0 &&
            Net::Sendto(raw_sender, "pending", 7, 0, p2p_address.data(), p2p_address.size()) == 7,
        "queue a datagram requiring unsupported UDPP2P processing");
  const auto pending_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (Net::Recv(encrypted, &untouched, 1, 0) == -1 &&
         *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK &&
         std::chrono::steady_clock::now() < pending_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Check(*Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EOPNOTSUPP &&
            Net::Recv(encrypted, &untouched, 1, 0) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EOPNOTSUPP && untouched == '!',
        "queued packets remain queued and unsupported instead of masquerading as empty");
  Check(Net::SocketClose(encrypted) == 0 && Net::SocketClose(raw_sender) == 0,
        "close UDPP2P readiness fixtures");
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

int main(int argc, char** argv) {
  Common::InitializeThreads();
  Common::Subsystems subsystems;
  subsystems.Initialize<Config::Lifecycle>();
  Config::ConfigOptions options;
  options.printf_direction = Config::LogDirection::Silent;
  Config::Load(options);
  subsystems.Initialize<Log::Lifecycle>();

  if (argc == 2 && std::string_view(argv[1]) == "--network-only") {
#if defined(__linux__)
    TestNetCtlHostIpv4();
#endif
    TestNpEmulation();
    TestNpAuthEmulation();
    subsystems.Destroy();
    std::printf("KernelFileSystemTests: network cases passed\n");
    return 0;
  }

  TestMutexHandoff();
  TestStaticRwlockDestroy();
  if (argc == 2 && std::string_view(argv[1]) == "--mutex-only") {
    subsystems.Destroy();
    std::printf("KernelFileSystemTests: mutex cases passed\n");
    return 0;
  }

  Check(SDL_InitSubSystem(SDL_INIT_VIDEO), "initialize Vulkan test video");
  auto graphics = std::make_unique<Libs::Graphics::WindowContext>();
  graphics->graphic_ctx.screen_width = 64;
  graphics->graphic_ctx.screen_height = 64;
  graphics->window = SDL_CreateWindow("KernelFileSystemTests", 64, 64,
                                      SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
  Check(graphics->window != nullptr, "create hidden Vulkan test window");
  graphics->CreateVulkan();

  TempDirectory temporary;
  CheckOpenFileCoherence(temporary.Path());
  FileSystem::Initialize();
  TestSysmoduleReferences();
  TestSystemServiceEntitlementEvents();
  TestAddContent(temporary.Path());
  TestRandomDevices();
  TestFileDescriptorFlags();
  CheckMountRoot(temporary.Path());
  CheckUnmappedPaths(temporary.Path());
  CheckArchiveMount(temporary.Path());
  CheckUnicodePaths(temporary.Path());
  CheckUnicodeLogPath(temporary.Path());
  CheckDirectoryStream(temporary.Path());
  CheckAprPaths(temporary.Path());
  FileSystem::Mount(temporary.Path(), "/savedata0");
  TestSaveOpenVisibility();
  TestFsync();
  TestSaveTruncate();
  TestSaveWritev();
  TestAioBatches();
  CheckSaveRename(temporary.Path(), "first-save");
  CheckSaveRename(temporary.Path(), "replacement-save");
  CheckSocketWakeup();
  FileSystem::Shutdown();
  CheckEtherAddressFormatting();
#if defined(__linux__)
  TestNetCtlHostIpv4();
#endif
  TestNpEmulation();
  TestNpAuthEmulation();
  TestNpWebApi2Memory();
  TestNpCommerceDialog();
  graphics.reset();
  subsystems.Destroy();

  std::printf("KernelFileSystemTests: all cases passed\n");
  return 0;
}
