#ifndef KYTY_COMMON_HACK_NOTICE_H_
#define KYTY_COMMON_HACK_NOTICE_H_

#include <cstdint>
#include <string_view>

// Visible notices for the workarounds ("hacks") that change what a game computes. They go to the
// normal log (stdout / emulator.log) with the fixed prefix "[HACK]", without any environment
// variable, so a session shows whether a hack was applied. Report() is safe for hot paths: it takes
// no locks, only atomics. The first report of each distinct (hack, site, shader, where) key prints a
// line; afterwards the keys that kept firing are summarised every 10 seconds with their totals.
namespace Common::HackNotice {

// `hack` and `site` must be string literals (they are compared by address). `shader_hash` 0 means
// "the shader of the enclosing ScopedShader, if any". `where` is part of the key (an address, an
// offset or a limit). `detail` is only used for the first line of a key.
void Report(const char* hack, const char* site, uint64_t shader_hash, uint64_t where,
            std::string_view detail = {});

inline uint64_t& CurrentShaderSlot() {
	static thread_local uint64_t shader_hash = 0;
	return shader_hash;
}

// Names the shader whose resources the calling thread is reading, for the notices of readers that
// do not know it (memory callbacks).
class ScopedShader {
public:
	explicit ScopedShader(uint64_t shader_hash): m_previous(CurrentShaderSlot()) {
		CurrentShaderSlot() = shader_hash;
	}
	~ScopedShader() { CurrentShaderSlot() = m_previous; }
	ScopedShader(const ScopedShader&)            = delete;
	ScopedShader& operator=(const ScopedShader&) = delete;

private:
	uint64_t m_previous;
};

} // namespace Common::HackNotice

#endif /* KYTY_COMMON_HACK_NOTICE_H_ */
