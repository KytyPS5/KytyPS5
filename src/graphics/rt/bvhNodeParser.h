#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODEPARSER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODEPARSER_H_

#include "graphics/rt/bvhNode.h"

#include <cstdint>
#include <span>

namespace Libs::Graphics {

// Raw byte size of each node type in guest memory, per the RDNA2 hardware layout.
inline constexpr uint32_t BvhBox32NodeByteSize    = 128;
inline constexpr uint32_t BvhTriangleNodeByteSize = 64;

// Decodes a Box32Node from its raw 128-byte guest representation. `raw` must be exactly
// BvhBox32NodeByteSize bytes; the caller is responsible for locating the node.
BvhBox32Node ParseBvhBox32Node(std::span<const uint8_t> raw);

// Decodes a TriangleNode from its raw 64-byte guest representation. `raw` must be exactly
// BvhTriangleNodeByteSize bytes.
BvhTriangleNode ParseBvhTriangleNode(std::span<const uint8_t> raw);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODEPARSER_H_ */
