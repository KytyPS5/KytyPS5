#pragma once
#include "graphics/shader/recompiler/ResourceReadCapture.h"

#include <algorithm>
#include <string>

namespace Libs::Graphics::ShaderRecompiler::Capture {
// Ordered callback replay, not a claim that all guest memory was snapshotted.
// A different access sequence is an input error, never a fabricated zero read.
class ResourceReadReplay {
public:
    ResourceReadReplay(const IR::SrtRuntime& source, std::vector<ReadEvent> reads)
        : original(source), events(std::move(reads)) {}
    IR::SrtRuntime Runtime() {
        auto result = original;
        result.userdata = this;
        if (original.read_memory) result.read_memory = Normal;
        if (original.read_specialization_memory) result.read_specialization_memory = Strict;
        if (original.clamp_memory_range) result.clamp_memory_range = Clamp;
        return result;
    }
    bool Consumed() const { return error.empty() && cursor == events.size(); }
    const std::string& Error() const { return error; }
private:
    IR::SrtRuntime original;
    std::vector<ReadEvent> events;
    size_t cursor = 0;
    std::string error;
    const ReadEvent* Next(ReadKind kind, uint64_t address, uint64_t bytes) {
        if (!error.empty()) return nullptr;
        if (cursor == events.size()) {
            error = "uncaptured resource callback";
            return nullptr;
        }
        const auto& event = events[cursor];
        if (event.kind != kind || event.address != address || event.requested_bytes != bytes) {
            error = "resource callback differs from ordered capture";
            return nullptr;
        }
        ++cursor;
        return &event;
    }
    bool Read(ReadKind kind, uint64_t address, std::span<uint32_t> values) {
        const auto* event = Next(kind, address, values.size_bytes());
        if (!event || !event->succeeded) return false;
        if (event->words.size() != values.size()) {
            error = "incomplete resource callback payload";
            return false;
        }
        std::copy(event->words.begin(), event->words.end(), values.begin());
        return true;
    }
    static bool Normal(void* data, uint64_t address, std::span<uint32_t> values) {
        return static_cast<ResourceReadReplay*>(data)->Read(ReadKind::Normal, address, values);
    }
    static bool Strict(void* data, uint64_t address, std::span<uint32_t> values) {
        return static_cast<ResourceReadReplay*>(data)->Read(ReadKind::Strict, address, values);
    }
    static uint64_t Clamp(void* data, uint64_t address, uint64_t bytes) {
        const auto* event = static_cast<ResourceReadReplay*>(data)->Next(ReadKind::Clamp, address, bytes);
        return event ? event->clamped_bytes : 0;
    }
};
} // namespace Libs::Graphics::ShaderRecompiler::Capture
