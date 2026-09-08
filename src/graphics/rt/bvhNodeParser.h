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

// Decodes a packed 32-bit guest node pointer/id into its type tag and byte offset. Per Mesa
// RADV's bvh_helpers.h (id_to_type/id_to_offset, gitlab.freedesktop.org/mesa/mesa): the low 3
// bits are the type tag, the remaining bits are (byte_offset >> 3), which is lossless because
// every node type is laid out on a 64-byte boundary (the minimum node size). The resulting
// offset is relative to the node pool base, not a fixed or global address.
BvhNodeId DecodeBvhNodeId(uint32_t id);

// Encodes a (type, byte_offset) pair back into a packed node id. byte_offset must be a multiple
// of 64; this exists mainly to build test fixtures without hand-computing the packed value.
uint32_t EncodeBvhNodeId(BvhNodeType type, uint32_t byte_offset);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODEPARSER_H_ */
