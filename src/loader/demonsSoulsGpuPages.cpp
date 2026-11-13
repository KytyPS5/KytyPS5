#include "loader/demonsSoulsGpuPages.h"
#include "common/logging/log.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "loader/guestCode.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace Loader::DemonsSoulsGpuPages {
namespace {
// Demon's Souls (PPSA01340, PPSA01341, PPSA01342).
bool IsSupportedGame() {
    static const bool demons_souls = [] {
        std::string title;
        return SystemContentParamSfoGetString("TITLE_ID", &title) &&
               (title == "PPSA01340" || title == "PPSA01341" || title == "PPSA01342");
    }();
    return demons_souls;
}
uint64_t base = 0, cave = 0;
size_t written = 0;
constexpr size_t SiteCount = 3; // the two CBuffer allocation calls and the render-context call
std::array<uint64_t, SiteCount> sites {};
std::array<std::array<uint8_t, 5>, SiteCount> original {}, installed {};
// Every audited function of the build is the loaded code.
bool Verified(const Program& program, const Build& build) {
    const auto* guest = reinterpret_cast<const uint8_t*>(program.base_vaddr);
    return std::all_of(build.audited.begin(), build.audited.end(), [&](const Function& function) {
        return function.rva + function.size <= program.mapped_size &&
               GuestCode::Readable(program.base_vaddr + function.rva, function.size) &&
               VerifyLoadedFunction(guest, build, function, program.tls.handler_vaddr - program.base_vaddr);
    });
}
}
void Install(Program* program) {
#if defined(__x86_64__) || defined(_M_X64)
    // The two CBuffer allocation calls and the render-context call (KYTY_GPU_BUFFER_PAGES=0: not patched).
    const auto* enabled = std::getenv("KYTY_GPU_BUFFER_PAGES");
    if ((enabled && std::strcmp(enabled, "0") == 0) || cave || !program ||
        program->file_name.filename() != "eboot.bin" ||
        !IsSupportedGame() || program->mapped_size >= CaveOffset ||
        program->base_vaddr > UINT64_MAX - CaveOffset - ImageSize ||
        program->tls.handler_vaddr < program->base_vaddr ||
        program->tls.handler_vaddr - program->base_vaddr > INT32_MAX) return;
    const auto* guest = reinterpret_cast<const uint8_t*>(program->base_vaddr);
    const auto build = std::find_if(Builds.begin(), Builds.end(), [&](const Build& b) { return Verified(*program, b); });
    if (build == Builds.end()) {
        std::printf("GPU buffer pages: the allocation code of no audited build; retaining original allocations\n");
        return;
    }
    std::array<uint8_t, ImageSize> image;
    if (!BuildImage(image.data(), *build)) return;
    sites = {build->sites[0], build->sites[1], build->context_site};
    for (size_t i = 0; i < SiteCount; ++i) {
        if (!BuildCall(installed[i], guest, *build, sites[i])) return;
        std::memcpy(original[i].data(), guest + sites[i], 5);
    }
    const auto requested = program->base_vaddr + CaveOffset;
    const auto allocated = Libs::LibKernel::Memory::AllocateRuntimeMemory(requested, ImageSize,
        Common::VirtualMemory::Mode::ReadWrite, "gpu_buffer_pages", true);
    if (allocated != requested) {
        if (allocated) Libs::LibKernel::Memory::FreeGuestMemory(allocated, ImageSize);
        return;
    }
    base = program->base_vaddr;
    cave = allocated;
    std::memcpy(reinterpret_cast<void*>(cave), image.data(), image.size());
    if (!Common::VirtualMemory::FlushInstructionCache(cave, StatsOffset) ||
        !Libs::LibKernel::Memory::ProtectGuestMemory(cave, StatsOffset,
            Common::VirtualMemory::Mode::ExecuteRead, nullptr)) {
        Clear();
        return;
    }
    for (size_t i = 0; i < SiteCount; ++i) {
        const auto address = base + sites[i];
        std::memcpy(reinterpret_cast<void*>(address), installed[i].data(), 5);
        ++written;
        if (!Common::VirtualMemory::FlushInstructionCache(address, 5)) {
            Clear();
            return;
        }
    }
    std::printf("GPU buffer pages: %zu verified allocation calls of %s installed, 4096-byte pages\n", SiteCount,
                build->version);
#endif
}
void Clear() {
    if (!cave) return;
    for (size_t i = 0; i < written; ++i) {
        auto* address = reinterpret_cast<void*>(base + sites[i]);
        if (std::memcmp(address, installed[i].data(), 5) == 0) {
            std::memcpy(address, original[i].data(), 5);
            Common::VirtualMemory::FlushInstructionCache(base + sites[i], 5);
        }
    }
    const auto* stats = reinterpret_cast<const uint64_t*>(cave + StatsOffset);
    LOGF("GPU buffer pages: cumulative requests=%llu logical=%llu physical=%llu\n",
         static_cast<unsigned long long>(stats[0]), static_cast<unsigned long long>(stats[1]),
         static_cast<unsigned long long>(stats[2]));
    Libs::LibKernel::Memory::FreeGuestMemory(cave, ImageSize);
    base = cave = written = 0;
}
} // namespace Loader::DemonsSoulsGpuPages
