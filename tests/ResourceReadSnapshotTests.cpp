#include "graphics/shader/recompiler/ResourceReadSnapshot.h"

#include <array>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace Libs::Graphics::ShaderRecompiler;
using namespace Libs::Graphics::ShaderRecompiler::Capture;
namespace {
void Check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template <typename Function> void Reject(Function function, const char* message) {
    bool rejected = false;
    try { function(); } catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, message);
}
IR::SrtRuntime Input() {
    IR::SrtRuntime runtime;
    runtime.read_memory = +[](void*, uint64_t, std::span<uint32_t>) { return false; };
    runtime.read_specialization_memory = runtime.read_memory;
    runtime.clamp_memory_range = +[](void*, uint64_t, uint64_t) -> uint64_t { return 0; };
    runtime.specialization_range_read_only = +[](void*, uint64_t, uint64_t) { return false; };
    runtime.max_dense_images = 97;
    return runtime;
}
}
int main() {
    try {
        const auto source = Input();
        const std::vector<ReadEvent> reads{
            {ReadKind::Strict, 0x104, 4, true, 0, {0x88776655}},
            {ReadKind::Strict, 0x100, 4, true, 0, {0x44332211}},
            {ReadKind::Strict, 0x104, 4, true, 0, {0x88776655}},
            {ReadKind::Normal, 0x100, 4, true, 0, {0xaabbccdd}},
            {ReadKind::Strict, 0x300, 4, false},
            {ReadKind::Clamp, 0x100, 32, true, 8},
            {ReadKind::ReadOnlyRange, 0x500, 16, true},
            {ReadKind::ReadOnlyRange, 0x600, 16, false},
        };
        ResourceReadSnapshot snapshot(source, reads);
        const std::array<uint32_t, 2> supplemental{0x04030201, 0x08070605};
        snapshot.AddStrictWords(0x200, supplemental);
        const auto runtime = snapshot.Runtime();
        Check(runtime.max_dense_images == 97, "snapshot changes runtime limits");
        Check(snapshot.CapturedBytes(ReadKind::Strict) == 16 && snapshot.CapturedBytes(ReadKind::Normal) == 4,
              "snapshot counted duplicate observations as bytes");
        std::array<uint32_t, 2> output{0, 0};
        Check(runtime.read_specialization_memory(runtime.userdata, 0x100, output) &&
              output == std::array<uint32_t, 2>{0x44332211, 0x88776655},
              "snapshot cannot merge adjacent callbacks or reorder reads");
        Check(runtime.read_specialization_memory(runtime.userdata, 0x102, std::span(output).first(1)) &&
              output[0] == 0x66554433,
              "snapshot subrange read does not preserve little-endian bytes");
        Check(runtime.read_memory(runtime.userdata, 0x100, std::span(output).first(1)) &&
              output[0] == 0xaabbccdd, "snapshot mixed ordinary and strict observations");
        Check(runtime.read_specialization_memory(runtime.userdata, 0x204, std::span(output).first(1)) &&
              output[0] == 0x08070605, "supplemental strict payload was not retained");
        output[0] = 0xdeadbeef;
        Check(!runtime.read_specialization_memory(runtime.userdata, 0x300, std::span(output).first(1)) &&
              output[0] == 0xdeadbeef && snapshot.Error().empty(),
              "captured failed read fabricated bytes or became an input error");
        Check(runtime.clamp_memory_range(runtime.userdata, 0x100, 32) == 8,
              "snapshot changes an exact captured clamp");
        Check(runtime.specialization_range_read_only(runtime.userdata, 0x500, 16) &&
              !runtime.specialization_range_read_only(runtime.userdata, 0x600, 16) && snapshot.Error().empty(),
              "snapshot changes a captured ownership query");
        Reject([&] { snapshot.AddStrictWords(0x400, supplemental); }, "snapshot mutation after use accepted");

        ResourceReadSnapshot missing(source, reads);
        const auto missing_runtime = missing.Runtime();
        output = {0xdeadbeef, 0xcafebabe};
        Check(!missing_runtime.read_specialization_memory(missing_runtime.userdata, 0x104, output) &&
              output == std::array<uint32_t, 2>{0xdeadbeef, 0xcafebabe} &&
              missing.Error().find("address=0x104 bytes=8") != std::string::npos && missing.MissingRequests() == 1,
              "uncaptured suffix returned a partial or zero-filled success");
        ResourceReadSnapshot wrong_kind(source, reads);
        const auto wrong_runtime = wrong_kind.Runtime();
        Check(!wrong_runtime.read_memory(wrong_runtime.userdata, 0x104, std::span(output).first(1)) &&
              wrong_kind.Error().find("ordinary") != std::string::npos,
              "strict observation was reused as an ordinary read");
        ResourceReadSnapshot unknown_clamp(source, reads);
        const auto clamp_runtime = unknown_clamp.Runtime();
        Check(clamp_runtime.clamp_memory_range(clamp_runtime.userdata, 0x100, 16) == 0 &&
              unknown_clamp.Error().find("clamp") != std::string::npos,
              "snapshot inferred an uncaptured clamp");
        ResourceReadSnapshot unknown_ownership(source, reads);
        const auto ownership_runtime = unknown_ownership.Runtime();
        Check(!ownership_runtime.specialization_range_read_only(ownership_runtime.userdata, 0x504, 4) &&
              unknown_ownership.Error().find("ownership") != std::string::npos,
              "snapshot inferred ownership of an uncaptured subrange");
        auto no_ownership = source; no_ownership.specialization_range_read_only = nullptr;
        ResourceReadSnapshot legacy(no_ownership, {});
        legacy.AddStrictWords(0x500, supplemental);
        Check(legacy.Runtime().specialization_range_read_only == nullptr,
              "supplemental bytes invented ownership proof");
        Reject([&] {
            ResourceReadSnapshot conflicting(source, {
                {ReadKind::Strict, 0x100, 4, true, 0, {0x44332211}},
                {ReadKind::Strict, 0x102, 4, true, 0, {0x88776600}},
            });
        }, "conflicting overlapping bytes accepted");
        for (bool failure_first : {false, true}) Reject([&] {
            std::vector<ReadEvent> conflict{
                {ReadKind::Strict, 0x100, 4, true, 0, {17}},
                {ReadKind::Strict, 0x102, 4, false},
            };
            if (failure_first) std::reverse(conflict.begin(), conflict.end());
            ResourceReadSnapshot conflicting(source, conflict);
        }, "success/failure overlap accepted");
        Reject([&] {
            ResourceReadSnapshot conflicting(source, {
                {ReadKind::Clamp, 0x100, 8, true, 4}, {ReadKind::Clamp, 0x100, 8, true, 8},
            });
        }, "changed clamp observation accepted");
        Reject([&] {
            ResourceReadSnapshot conflicting(source, {
                {ReadKind::ReadOnlyRange, 0x500, 16, true}, {ReadKind::ReadOnlyRange, 0x500, 16, false},
            });
        }, "changed ownership observation accepted");
        Reject([&] {
            ResourceReadSnapshot overflow(source, {
                {ReadKind::Strict, std::numeric_limits<uint64_t>::max() - 2u, 4, true, 0, {17}},
            });
        }, "overflowing address accepted");
        Reject([&] {
            auto absent = source; absent.read_specialization_memory = nullptr;
            ResourceReadSnapshot unknown(absent, {{ReadKind::Strict, 0x100, 4, true, 0, {17}}});
        }, "uncaptured callback presence fabricated");
        Reject([&] {
            ResourceReadSnapshot oversized(source, {});
            const std::vector<uint32_t> words(ResourceReadSnapshot::ByteLimit / 4u + 1u);
            oversized.AddStrictWords(0, words);
        }, "snapshot payload budget not enforced");
        std::puts("KYTY_RESOURCE_READ_SNAPSHOT_PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "resource snapshot test failed: %s\n", error.what());
        return 1;
    }
}
