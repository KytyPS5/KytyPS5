#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Loader { struct Program; }
namespace Loader::DemonsSoulsGpuPages {
// Experimental, opt-in page ownership at the game's CBuffer allocation boundary.
// The two audited callers pass a zero-extended uint32_t size. The original game
// still owns allocation, initialization, descriptors and deferred destruction.
constexpr uint64_t CaveOffset = 0x8080000;
constexpr size_t ImageSize = 0x8000, StatsOffset = 0x4000;
constexpr uint64_t ContextBridgeOffset = 0x100;
struct Function { uint64_t rva, size, hash; };
// A build of the game whose code was audited: the allocator, the two CBuffer allocation calls, the
// render-context call and the wrapper it calls, the functions checked byte for byte, and the loader's
// FS-load replacements in them.
struct Build {
    const char* version;
    uint64_t allocate_rva;
    std::array<uint64_t, 2> sites;
    uint64_t context_site, context_target;
    std::array<Function, 7> audited;
    std::array<uint64_t, 4> tls_sites;
};
// 01.005.000 has the same functions as 01.007.000, compiled by an older compiler (other registers and
// block order, the same calls, heap selection, request and out-of-memory path).
constexpr std::array<Build, 2> Builds {{
    {"01.007.000", 0x7f9cf0, {0x909509, 0x909537}, 0x903d2d, 0x80a510,
     {{
         {0x909460, 990, 0x551381ceb6002115ull}, // create: logical size and the two calls
         {0x909410, 74, 0x0310824bfaa60e06ull}, // destroy: passes allocation base only
         {0x903460, 361, 0x380005cc9ab9ca19ull}, // deferred free: retains that base
         {0x7f9cf0, 626, 0xc05fedb6d19828f6ull}, // original aligned allocator
         {0x7fa380, 603, 0x3dd03cac0f8f1bdeull}, // original pointer-based deallocator
         {0x903bb0, 923, 0x6c9f9b4d877a80d5ull}, // complete render-context constructor
         {0x80a510, 10, 0xd175875203953d78ull},
     }},
     {0x7f9e07, 0x7f9e4c, 0x7fa4ad, 0x7fa55d}},
    {"01.005.000", 0x7df290, {0x8ed949, 0x8ed977}, 0x8e832d, 0x7efa90,
     {{
         {0x8ed8a0, 971, 0x3416fcc8f8da37a1ull},
         {0x8ed850, 68, 0x4439dc74f79ca146ull},
         {0x8e7a70, 361, 0x177bf12faf349de4ull},
         {0x7df290, 611, 0x6b18c3990b89a030ull},
         {0x7df8f0, 589, 0xa4ccf6f30b41a14dull},
         {0x8e81c0, 965, 0xc8e44e36fdd2385bull},
         {0x7efa90, 10, 0x5061864aea21c198ull},
     }},
     {0x7df3e0, 0x7df457, 0x7dfa17, 0x7dfac3}},
}};
inline uint64_t Fingerprint(const void* data, size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}
inline uint64_t RelativeTarget(const uint8_t* operand, uint64_t next) {
    int32_t displacement;
    std::memcpy(&displacement, operand, sizeof(displacement));
    return next + displacement;
}
inline bool WriteRelative(uint8_t* operand, uint64_t next, uint64_t target) {
    if (next > INT64_MAX || target > INT64_MAX) return false;
    const int64_t difference = static_cast<int64_t>(target) - static_cast<int64_t>(next);
    if (difference < INT32_MIN || difference > INT32_MAX) return false;
    const auto value = static_cast<int32_t>(difference);
    std::memcpy(operand, &value, sizeof(value));
    return true;
}
inline bool VerifyLoadedFunction(const uint8_t* guest, const Build& build, const Function& function,
                                 uint64_t tls_handler) {
    std::array<uint8_t, 1024> normalized {};
    if (function.size > normalized.size()) return false;
    std::memcpy(normalized.data(), guest + function.rva, function.size);
    // Normalize only the loader's four verified FS-load replacements.
    for (const uint64_t site : build.tls_sites) {
        if (site < function.rva || site >= function.rva + function.size) continue;
        if (site + 9 > function.rva + function.size) return false;
        auto* at = normalized.data() + site - function.rva;
        if (tls_handler) {
            if (at[0] != 0x48 || at[1] != 0xe8 || at[6] != 0x48 || at[7] != 0x89 || at[8] != 0xc0 ||
                RelativeTarget(at + 2, site + 6) != tls_handler) return false;
            constexpr std::array<uint8_t, 9> original {0x64, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
            std::memcpy(at, original.data(), original.size());
        }
    }
    return Fingerprint(normalized.data(), function.size) == function.hash;
}
inline bool BuildImage(uint8_t* image, const Build& build, uint64_t cave = CaveOffset) {
    if (cave > INT32_MAX - ImageSize) return false;
    std::fill(image, image + StatsOffset, 0xcc);
    std::fill(image + StatsOffset, image + ImageSize, 0);
    // SysV (bytes, allocator spec, alignment, flags); tail-call original allocator.
    // Reserve BOTH aligned start and rounded physical extent, so another object
    // cannot share the final protected page. Existing 64 KiB alignment survives.
    // Zero-size requests remain unchanged. Counters measure cumulative requests,
    // not live/peak memory. No guest code or table is copied into this bridge.
    constexpr std::array<uint8_t, 61> bridge {
        0x48, 0x85, 0xff, 0x74, 0x33, 0x81, 0xfa, 0x00, 0x10, 0x00, 0x00, 0x73, 0x05,
        0xba, 0x00, 0x10, 0x00, 0x00, 0xf0, 0x48, 0xff, 0x05, 0, 0, 0, 0,
        0xf0, 0x48, 0x01, 0x3d, 0, 0, 0, 0,
        0x48, 0x81, 0xc7, 0xff, 0x0f, 0x00, 0x00,
        0x48, 0x81, 0xe7, 0x00, 0xf0, 0xff, 0xff,
        0xf0, 0x48, 0x01, 0x3d, 0, 0, 0, 0, 0xe9, 0, 0, 0, 0
    };
    std::memcpy(image, bridge.data(), bridge.size());
    auto* context = image + ContextBridgeOffset;
    std::memcpy(context, bridge.data(), bridge.size());
    // RequestNewRenderContext allocates a 192-byte CPU bookkeeping block. The
    // original wrapper supplies its allocation-family flag; do not bypass it.
    return WriteRelative(context + 0x16, cave + ContextBridgeOffset + 0x1a, cave + StatsOffset + 0x80) &&
           WriteRelative(context + 0x1e, cave + ContextBridgeOffset + 0x22, cave + StatsOffset + 0x88) &&
           WriteRelative(context + 0x34, cave + ContextBridgeOffset + 0x38, cave + StatsOffset + 0x90) &&
           WriteRelative(context + 0x39, cave + ContextBridgeOffset + 0x3d, build.context_target) &&
           WriteRelative(image + 0x16, cave + 0x1a, cave + StatsOffset) &&
           WriteRelative(image + 0x1e, cave + 0x22, cave + StatsOffset + 8) &&
           WriteRelative(image + 0x34, cave + 0x38, cave + StatsOffset + 16) &&
           WriteRelative(image + 0x39, cave + 0x3d, build.allocate_rva);
}
inline bool BuildCall(std::array<uint8_t, 5>& call, const uint8_t* guest, const Build& build, uint64_t site,
                      uint64_t cave = CaveOffset) {
    const bool context = site == build.context_site;
    const auto target = context ? build.context_target : build.allocate_rva;
    if (guest[site] != 0xe8 || RelativeTarget(guest + site + 1, site + 5) != target) return false;
    call[0] = 0xe8;
    return WriteRelative(call.data() + 1, site + 5, cave + (context ? ContextBridgeOffset : 0));
}
void Install(Program* program);
void Clear();
} // namespace Loader::DemonsSoulsGpuPages
