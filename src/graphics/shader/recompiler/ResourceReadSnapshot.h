#pragma once

#include "graphics/shader/recompiler/ResourceReadCapture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Capture {
// A diagnostic address snapshot permits a changed access order. It contains
// only observed bytes, separately for ordinary and strict reads. It supplies
// no draw-time ownership, alias or synchronization proof.
class ResourceReadSnapshot {
public:
    static constexpr size_t ByteLimit = 16u * 1024u * 1024u;
    ResourceReadSnapshot(const IR::SrtRuntime& source, const std::vector<ReadEvent>& events)
        : original(source) {
        if (events.size() > 131072u) throw std::runtime_error("snapshot event budget exceeded");
        for (const auto& event : events) {
            if (event.kind == ReadKind::Clamp) {
                if (!original.clamp_memory_range || !event.succeeded || !event.words.empty() || event.clamped_bytes > event.requested_bytes)
                    throw std::runtime_error("invalid snapshot clamp");
                auto key = std::pair{event.address, event.requested_bytes};
                const auto [it, added] = clamps.emplace(key, event.clamped_bytes);
                if (!added && it->second != event.clamped_bytes)
                    throw std::runtime_error("conflicting snapshot clamp");
            } else if (event.kind == ReadKind::ReadOnlyRange) {
                if (!original.specialization_range_read_only || !event.words.empty() || event.clamped_bytes != 0)
                    throw std::runtime_error("invalid snapshot read-only range query");
                const auto [it, added] = read_only_ranges.emplace(
                    std::pair{event.address, event.requested_bytes}, event.succeeded);
                if (!added && it->second != event.succeeded)
                    throw std::runtime_error("conflicting snapshot read-only range query");
            } else if (event.kind == ReadKind::Normal || event.kind == ReadKind::Strict) {
                if (!(event.kind == ReadKind::Normal ? original.read_memory : original.read_specialization_memory) ||
                    event.requested_bytes % 4u ||
                    (event.succeeded ? event.words.size() != event.requested_bytes / 4u : !event.words.empty()))
                    throw std::runtime_error("invalid snapshot read");
                if (event.succeeded) AddWords(event.kind, event.address, event.words);
                else AddFailure(event.kind, event.address, event.requested_bytes);
            } else throw std::runtime_error("invalid snapshot callback kind");
        }
    }

    void AddStrictWords(uint64_t address, std::span<const uint32_t> words) {
        if (sealed) throw std::runtime_error("snapshot is already in use");
        if (!original.read_specialization_memory)
            throw std::runtime_error("strict snapshot callback was not captured");
        AddWords(ReadKind::Strict, address, words);
    }
    IR::SrtRuntime Runtime() {
        sealed = true;
        auto runtime = original;
        runtime.userdata = this;
        if (original.read_memory) runtime.read_memory = Normal;
        if (original.read_specialization_memory) runtime.read_specialization_memory = Strict;
        if (original.clamp_memory_range) runtime.clamp_memory_range = Clamp;
        if (original.specialization_range_read_only) runtime.specialization_range_read_only = ReadOnlyRange;
        return runtime;
    }
    const std::string& Error() const { return error; }
    size_t CapturedBytes(ReadKind kind) const {
        const auto& ranges = bytes.at(Index(kind));
        size_t total = 0;
        for (const auto& [address, payload] : ranges) total += payload.size();
        return total;
    }
    size_t Requests() const { return requests; }
    size_t MissingRequests() const { return missing_requests; }
    size_t ConflictsRejected() const { return conflicts_rejected; }

private:
    struct Failure { uint64_t address, size; };
    IR::SrtRuntime original;
    std::array<std::map<uint64_t, std::vector<uint8_t>>, 2> bytes;
    std::array<std::vector<Failure>, 2> failures;
    std::map<std::pair<uint64_t, uint64_t>, uint64_t> clamps;
    std::map<std::pair<uint64_t, uint64_t>, bool> read_only_ranges;
    size_t stored_bytes = 0, requests = 0, missing_requests = 0, conflicts_rejected = 0;
    std::string error;
    bool sealed = false;

    static size_t Index(ReadKind kind) {
        if (kind != ReadKind::Normal && kind != ReadKind::Strict)
            throw std::runtime_error("snapshot bytes require a read kind");
        return kind == ReadKind::Normal ? 0u : 1u;
    }
    static uint64_t End(uint64_t address, uint64_t size) {
        if (size > std::numeric_limits<uint64_t>::max() - address)
            throw std::runtime_error("snapshot address range overflows");
        return address + size;
    }
    static bool Overlap(uint64_t a, uint64_t end_a, uint64_t b, uint64_t end_b) {
        return a < end_b && b < end_a;
    }
    void Conflict(const char* message) {
        ++conflicts_rejected;
        throw std::runtime_error(message);
    }
    void AddFailure(ReadKind kind, uint64_t address, uint64_t size) {
        const auto end = End(address, size);
        const auto index = Index(kind);
        for (const auto& [start, payload] : bytes[index])
            if (Overlap(address, end, start, End(start, payload.size())))
                Conflict("snapshot read both succeeded and failed");
        failures[index].push_back({address, size});
    }
    void AddWords(ReadKind kind, uint64_t address, std::span<const uint32_t> words) {
        if (words.size() > ByteLimit / 4u) throw std::runtime_error("snapshot byte budget exceeded");
        const auto end = End(address, words.size_bytes());
        if (words.empty()) return;
        const auto index = Index(kind);
        for (const auto& failed : failures[index])
            if (Overlap(address, end, failed.address, End(failed.address, failed.size)))
                Conflict("snapshot read both succeeded and failed");
        std::vector<uint8_t> payload(words.size_bytes());
        for (size_t i = 0; i < words.size(); ++i)
            for (size_t byte = 0; byte < 4u; ++byte)
                payload[i * 4u + byte] = static_cast<uint8_t>(words[i] >> (byte * 8u));

        auto& ranges = bytes[index];
        auto first = ranges.lower_bound(address);
        if (first != ranges.begin()) {
            auto previous = std::prev(first);
            if (End(previous->first, previous->second.size()) >= address) first = previous;
        }
        auto last = first;
        uint64_t merged_start = address, merged_end = end;
        size_t removed_bytes = 0;
        for (; last != ranges.end() && last->first <= merged_end; ++last) {
            const auto start = last->first, old_end = End(start, last->second.size());
            const auto overlap_start = std::max(start, address);
            const auto overlap_end = std::min(old_end, end);
            for (uint64_t pos = overlap_start; pos < overlap_end; ++pos)
                if (last->second[static_cast<size_t>(pos - start)] != payload[static_cast<size_t>(pos - address)])
                    Conflict("conflicting snapshot byte observations");
            merged_start = std::min(merged_start, start);
            merged_end = std::max(merged_end, old_end);
            removed_bytes += last->second.size();
        }
        const uint64_t merged_size = merged_end - merged_start;
        if (merged_size > ByteLimit || stored_bytes - removed_bytes > ByteLimit - merged_size)
            throw std::runtime_error("snapshot byte budget exceeded");
        std::vector<uint8_t> merged(static_cast<size_t>(merged_size));
        for (auto it = first; it != last; ++it)
            std::copy(it->second.begin(), it->second.end(), merged.begin() + static_cast<size_t>(it->first - merged_start));
        std::copy(payload.begin(), payload.end(), merged.begin() + static_cast<size_t>(address - merged_start));
        ranges.erase(first, last);
        ranges.emplace(merged_start, std::move(merged));
        stored_bytes = stored_bytes - removed_bytes + static_cast<size_t>(merged_size);
    }
    void Missing(ReadKind kind, uint64_t address, uint64_t size) {
        ++missing_requests;
        if (!error.empty()) return;
        char message[160];
        std::snprintf(message, sizeof(message), "snapshot missing %s range address=0x%llx bytes=%llu",
                      kind == ReadKind::Normal ? "ordinary" : kind == ReadKind::Strict ? "strict" :
                      kind == ReadKind::Clamp ? "clamp" : "read-only ownership",
                      static_cast<unsigned long long>(address), static_cast<unsigned long long>(size));
        error = message;
    }
    bool Read(ReadKind kind, uint64_t address, std::span<uint32_t> output) {
        ++requests;
        if (!error.empty()) return false;
        const auto index = Index(kind);
        for (const auto& failed : failures[index])
            if (failed.address == address && failed.size == output.size_bytes()) return false;
        if (output.empty()) return true;
        if (output.size_bytes() > std::numeric_limits<uint64_t>::max() - address) {
            Missing(kind, address, output.size_bytes()); return false;
        }
        const auto& ranges = bytes[index];
        auto it = ranges.upper_bound(address);
        if (it == ranges.begin()) { Missing(kind, address, output.size_bytes()); return false; }
        --it;
        const auto offset = address - it->first;
        if (offset > it->second.size() || output.size_bytes() > it->second.size() - offset) {
            Missing(kind, address, output.size_bytes()); return false;
        }
        for (size_t word = 0; word < output.size(); ++word) {
            uint32_t value = 0;
            for (size_t byte = 0; byte < 4u; ++byte)
                value |= uint32_t{it->second[static_cast<size_t>(offset) + word * 4u + byte]} << (byte * 8u);
            output[word] = value;
        }
        return true;
    }
    static bool Normal(void* data, uint64_t address, std::span<uint32_t> output) {
        return static_cast<ResourceReadSnapshot*>(data)->Read(ReadKind::Normal, address, output);
    }
    static bool Strict(void* data, uint64_t address, std::span<uint32_t> output) {
        return static_cast<ResourceReadSnapshot*>(data)->Read(ReadKind::Strict, address, output);
    }
    static uint64_t Clamp(void* data, uint64_t address, uint64_t size) {
        auto& snapshot = *static_cast<ResourceReadSnapshot*>(data);
        ++snapshot.requests;
        if (!snapshot.error.empty()) return 0;
        const auto it = snapshot.clamps.find({address, size});
        if (it == snapshot.clamps.end()) { snapshot.Missing(ReadKind::Clamp, address, size); return 0; }
        return it->second;
    }
    static bool ReadOnlyRange(void* data, uint64_t address, uint64_t size) {
        auto& snapshot = *static_cast<ResourceReadSnapshot*>(data);
        ++snapshot.requests;
        if (!snapshot.error.empty()) return false;
        const auto it = snapshot.read_only_ranges.find({address, size});
        if (it == snapshot.read_only_ranges.end()) {
            snapshot.Missing(ReadKind::ReadOnlyRange, address, size); return false;
        }
        return it->second;
    }
};
} // namespace Libs::Graphics::ShaderRecompiler::Capture
