#ifndef EMULATOR_SRC_GRAPHICS_SHADER_SHADERCALLTRACE_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_SHADERCALLTRACE_H_

#include <cstdint>
#include <span>
#include <vector>

namespace Libs::Graphics {

struct IndirectCallSite {
	uint32_t pc          = 0; // byte offset of the S_SWAPPC_B64 within the caller
	uint32_t target_sgpr = 0; // SGPR pair holding the target, and the return address afterwards
	uint64_t handler     = 0; // resolved callee address; 0 when it could not be resolved
};

std::vector<IndirectCallSite> ResolveIndirectCalls(std::span<const uint32_t> code,
                                                   std::span<const uint32_t> user_data,
                                                   uint64_t                  shader_addr);

std::span<const uint32_t> TrimToCode(std::span<const uint32_t> code,
                                     uint32_t                  return_sgpr = UINT32_MAX);

bool EndsWithReturn(std::span<const uint32_t> code, uint32_t target_sgpr);

std::vector<uint32_t> SpliceIndirectCalls(std::span<const uint32_t>                  code,
                                          std::span<const IndirectCallSite>          sites,
                                          std::span<const std::span<const uint32_t>> handlers);

} // namespace Libs::Graphics

#endif
