#pragma once
// Opt-in, local content cache; misses never change compilation. A filename
// is only an index: exact input/state and output integrity are checked below.
#include "xxhash.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <string>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics {
class SpirvOptimizationCache {
public:
    struct State {
        std::string implementation; // Generated SHA256(recipe + actual linked libraries).
        uint32_t mode = 0, environment = 0;
        bool cooperative_wave64 = false;
    };
    static constexpr uint64_t MaxWords = 4u * 1024u * 1024u;
    explicit SpirvOptimizationCache(std::filesystem::path directory,
                                   uint64_t byte_limit = 2ull * 1024u * 1024u * 1024u,
                                   uint64_t entry_limit = 2048)
        : directory(std::move(directory)), byte_limit(byte_limit), entry_limit(entry_limit) {}

    std::filesystem::path EntryPath(std::span<const uint32_t> input, const State& state) const {
        const auto identity = state.implementation + ":" + std::to_string(state.mode) + ":" +
            std::to_string(state.environment) + ":" + (state.cooperative_wave64 ? "1" : "0");
        const auto hash = XXH3_128bits_withSeed(input.data(), input.size_bytes(), XXH3_64bits(identity.data(), identity.size()));
        char name[64];
        std::snprintf(name, sizeof(name), "koc-%016llx%016llx.bin",
            static_cast<unsigned long long>(hash.high64), static_cast<unsigned long long>(hash.low64));
        return directory / name;
    }

    bool Load(std::span<const uint32_t> input, const State& state, std::vector<uint32_t>& output) const {
        if (!Valid(input, state)) return false;
        try {
            const auto path = EntryPath(input, state);
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(path, ec);
            if (ec || size < HeaderBytes + 64u || size > HeaderBytes + 64u + MaxWords * 8u) return false;
            std::ifstream stream(path, std::ios::binary);
            Header header{};
            if (!Read(stream, header.data(), HeaderBytes) || header[0] != Magic || header[1] != 1u ||
                header[2] != input.size() || !header[3] || header[3] > MaxWords ||
                header[4] != state.mode || header[5] != state.environment || header[6] != state.cooperative_wave64 ||
                size != HeaderBytes + 64u + (header[2] + header[3]) * 4u) return false;
            std::string identity(64u, '\0');
            std::vector<uint32_t> recorded(input.size());
            std::vector<uint32_t> candidate(static_cast<size_t>(header[3]));
            if (!Read(stream, identity.data(), identity.size()) || identity != state.implementation ||
                !Read(stream, recorded.data(), recorded.size() * 4u) ||
                !std::equal(recorded.begin(), recorded.end(), input.begin()) ||
                !Read(stream, candidate.data(), candidate.size() * 4u)) return false;
            const auto hash = XXH3_128bits(candidate.data(), candidate.size() * 4u);
            if (hash.low64 != header[7] || hash.high64 != header[8]) return false;
            output = std::move(candidate);
            return true;
        } catch (...) { return false; }
    }

    bool Store(std::span<const uint32_t> input, const State& state, std::span<const uint32_t> output) {
        if (!Valid(input, state) || output.empty() || output.size() > MaxWords) return false;
        std::lock_guard guard(mutex);
        std::filesystem::path temporary;
        try {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            if (ec || !ScanOnce()) return false;
            const auto target = EntryPath(input, state);
            const auto previous = std::filesystem::file_size(target, ec);
            const bool existed = !ec;
            const uint64_t old_bytes = existed ? previous : 0;
            const uint64_t size = HeaderBytes + 64u + input.size_bytes() + output.size_bytes();
            if (size > byte_limit || used_bytes < old_bytes || used_bytes - old_bytes > byte_limit - size ||
                (!existed && entries >= entry_limit)) return false;
            static std::atomic<uint64_t> sequence{0};
            const auto clock = std::chrono::steady_clock::now().time_since_epoch().count();
            temporary = target;
            temporary += "." + std::to_string(clock) + "." + std::to_string(++sequence) + ".tmp";
            const auto hash = XXH3_128bits(output.data(), output.size_bytes());
            Header header{Magic, 1u, input.size(), output.size(), state.mode, state.environment,
                          state.cooperative_wave64, hash.low64, hash.high64};
            {
                std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
                stream.write(reinterpret_cast<const char*>(header.data()), HeaderBytes);
                stream.write(state.implementation.data(), state.implementation.size());
                stream.write(reinterpret_cast<const char*>(input.data()), static_cast<std::streamsize>(input.size_bytes()));
                stream.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(output.size_bytes()));
                stream.flush();
                if (!stream.good()) { stream.close(); std::filesystem::remove(temporary, ec); return false; }
                stream.close();
                if (stream.fail()) { std::filesystem::remove(temporary, ec); return false; }
            }
            if (!AtomicReplace(temporary, target)) { std::filesystem::remove(temporary, ec); return false; }
            used_bytes = used_bytes - old_bytes + size;
            if (!existed) ++entries;
            return true;
        } catch (...) {
            std::error_code ec;
            if (!temporary.empty()) std::filesystem::remove(temporary, ec);
            return false;
        }
    }

private:
    // Version1 is little-endian; every currently supported native host is LE.
    static_assert(std::endian::native == std::endian::little);
    using Header = std::array<uint64_t, 9>;
    static constexpr uint64_t HeaderBytes = sizeof(Header), Magic = 0x3154434f5653594bull;
    std::filesystem::path directory;
    uint64_t byte_limit, entry_limit, used_bytes = 0, entries = 0;
    bool scanned = false;
    std::mutex mutex;
    static bool Valid(std::span<const uint32_t> input, const State& state) {
        if (input.empty() || input.size() > MaxWords || state.implementation.size() != 64u ||
            state.mode > 2u) return false;
        for (const auto c : state.implementation)
            if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
        return true;
    }
    static bool Read(std::ifstream& stream, void* data, uint64_t bytes) {
        return static_cast<bool>(stream.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes)));
    }
    bool ScanOnce() {
        if (scanned) return true;
        std::error_code ec;
        uint64_t observed_bytes = 0, observed_entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) return false;
            if (!entry.is_regular_file(ec)) { if (ec) return false; continue; }
            const auto size = entry.file_size(ec);
            if (ec || observed_bytes > byte_limit || size > byte_limit - observed_bytes || observed_entries >= entry_limit) return false;
            observed_bytes += size;
            ++observed_entries;
        }
        if (ec) return false;
        used_bytes = observed_bytes;
        entries = observed_entries;
        scanned = true;
        return true;
    }
    static bool AtomicReplace(const std::filesystem::path& temporary, const std::filesystem::path& target) {
#if defined(_WIN32)
        return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        std::error_code ec;
        std::filesystem::rename(temporary, target, ec);
        return !ec;
#endif
    }
};
} // namespace Libs::Graphics
