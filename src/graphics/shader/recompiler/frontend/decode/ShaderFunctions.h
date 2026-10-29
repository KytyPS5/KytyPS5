#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERFUNCTIONS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_RECOMPILER_SHADERFUNCTIONS_H_

#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::Decoder {

using ShaderCodeReader = std::function<bool(uint64_t, std::span<uint32_t>)>;

// The s_trap code the expansion emits for a call target outside the captured set. Guest
// s_trap instructions are no-ops, as on retail hardware; only this code reports a trap.
constexpr uint32_t ShaderCallMissTrapCode = 0x7fu;

// Expands scalar calls through immutable pointers or bounded scalar-buffer tables.
// Table calls retain runtime selection and trap on targets outside the captured set.
// Empty output means no calls; unresolved calls never discard guest instructions.
// consulted_user_data, when given, receives the user-data registers the result depends on;
// together with the code and the memory read through `read`, they determine the result.
// long_branches receives the branches beyond the reach of their encoding (see LongBranch): the
// expanded code decodes correctly only with them.
bool InlineShaderFunctions(std::span<const uint32_t> code, uint64_t base,
                           std::span<const uint32_t> user_data, const ShaderCodeReader& read,
                           std::vector<uint32_t>& expanded, std::vector<LongBranch>& long_branches,
                           std::string& reason, uint32_t wave_size = 64,
                           std::vector<uint32_t>* consulted_user_data = nullptr);

// InlineShaderFunctions for a caller that expands the same shaders over and over (the pipeline
// cache, once per dispatch). The decoded shader is kept while its code is unchanged, and an
// expansion while the call targets resolve to the same addresses and the callee code reads the
// same; only the target resolution runs every time. The results equal InlineShaderFunctions'.
class ShaderFunctionExpander {
public:
	ShaderFunctionExpander();
	~ShaderFunctionExpander();
	ShaderFunctionExpander(const ShaderFunctionExpander&)            = delete;
	ShaderFunctionExpander& operator=(const ShaderFunctionExpander&) = delete;

	// code_hash, when nonzero, is the code's content hash (the program cache's shader hash): a
	// shader decoded with the same hash and size is taken as unchanged without comparing its code.
	bool Expand(std::span<const uint32_t> code, uint64_t base, std::span<const uint32_t> user_data,
	            const ShaderCodeReader& read, std::vector<uint32_t>& expanded, std::string& reason,
	            uint32_t wave_size, uint64_t code_hash = 0);
	// Where the last successful Expand() placed each call target's code.
	[[nodiscard]] const std::vector<InlinedCallBody>& Bodies() const;
	// The long branches of the last successful Expand(): its code decodes correctly only with them.
	[[nodiscard]] const std::vector<LongBranch>& LongBranches() const;
	// Whether the code Expand last analyzed at this address contains calls (true when unknown).
	[[nodiscard]] bool HasCalls(std::span<const uint32_t> code) const;
	// The user-data registers the last complete call resolution of this code (same content hash
	// and wave size) consulted, or nullptr when unknown. The resolution is a function of the
	// code, the base, the wave size, the memory it reads and the values of the registers it
	// consults: with those equal, another resolution consults the same registers and gives the
	// same result whatever the other registers hold.
	[[nodiscard]] const std::vector<uint32_t>* ConsultedUserData(std::span<const uint32_t> code,
	                                                             uint64_t code_hash,
	                                                             uint32_t wave_size) const;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Libs::Graphics::ShaderRecompiler::Decoder

#endif
