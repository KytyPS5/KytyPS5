#pragma once
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Capture {
enum class ReadKind : uint8_t { Normal, Strict, Clamp };
struct ReadEvent {
	ReadKind              kind;
	uint64_t              address;
	uint64_t              requested_bytes;
	bool                  succeeded;
	uint64_t              clamped_bytes = 0;
	std::vector<uint32_t> words;
};
// Diagnostic wrapper: delegates each callback once and never changes its result.
class ResourceReadCapture {
public:
	explicit ResourceReadCapture(const IR::SrtRuntime& source,
	                             size_t max_words = 2u * 1024u * 1024u, size_t max_events = 131072u)
	    : original(source), word_limit(max_words), event_limit(max_events) {}
	IR::SrtRuntime Runtime() {
		auto result     = original;
		result.userdata = this;
		if (original.read_memory) result.read_memory = Normal;
		if (original.read_specialization_memory) result.read_specialization_memory = Strict;
		if (original.clamp_memory_range) result.clamp_memory_range = Clamp;
		return result;
	}
	const std::vector<ReadEvent>& Events() const { return events; }
	bool                          Complete() const { return complete; }

private:
	IR::SrtRuntime         original;
	size_t                 word_limit, event_limit, captured_words = 0;
	bool                   complete = true;
	std::vector<ReadEvent> events;
	bool                   Read(ReadKind kind, uint64_t address, std::span<uint32_t> values) {
        const auto reader =
            kind == ReadKind::Normal ? original.read_memory : original.read_specialization_memory;
        // Preserve callback invocation, userdata, output and result. A failed
        // reader has no defined output payload; never inspect its words.
        const bool ok = reader(original.userdata, address, values);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		try {
#endif
			if (events.size() >= event_limit ||
			    (ok && values.size() > word_limit - captured_words)) {
				complete = false;
				return ok;
			}
			ReadEvent event {kind, address, values.size_bytes(), ok};
			if (ok) {
				event.words.assign(values.begin(), values.end());
				captured_words += values.size();
			}
			events.push_back(std::move(event));
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		} catch (...) {
			complete = false;
		}
#endif
		return ok;
	}
	static bool Normal(void* data, uint64_t address, std::span<uint32_t> values) {
		return static_cast<ResourceReadCapture*>(data)->Read(ReadKind::Normal, address, values);
	}
	static bool Strict(void* data, uint64_t address, std::span<uint32_t> values) {
		return static_cast<ResourceReadCapture*>(data)->Read(ReadKind::Strict, address, values);
	}
	static uint64_t Clamp(void* data, uint64_t address, uint64_t bytes) {
		auto&      capture = *static_cast<ResourceReadCapture*>(data);
		const auto actual =
		    capture.original.clamp_memory_range(capture.original.userdata, address, bytes);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		try {
#endif
			if (capture.events.size() >= capture.event_limit)
				capture.complete = false;
			else
				capture.events.push_back({ReadKind::Clamp, address, bytes, true, actual});
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
		} catch (...) {
			capture.complete = false;
		}
#endif
		return actual;
	}
};
} // namespace Libs::Graphics::ShaderRecompiler::Capture