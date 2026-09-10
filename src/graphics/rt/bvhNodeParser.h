#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODEPARSER_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODEPARSER_H_

#include "graphics/rt/bvhNode.h"

#include <cstdint>
#include <span>
#include <vector>

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

enum class BvhWalkResult {
	Ok,                  // Walked the full reachable tree; only box32/triangle nodes were seen.
	UnsupportedNodeType, // Stopped early: reached a node type not yet handled
	                     // (box16/instance/aabb).
	OutOfBounds,         // Stopped early: a node's decoded offset fell outside `node_pool`.
	TooManyNodes,        // Stopped early: visited more nodes than MaxBvhWalkNodes, likely a cycle.
};

// Safety limit on nodes visited per walk, guards against a cyclic or otherwise malformed tree
// causing unbounded work rather than a clean, reportable failure.
inline constexpr uint32_t MaxBvhWalkNodes = 1u << 16;

// Walks a BVH starting at `root_id`, following box32 children (skipping slots equal to
// BvhInvalidNodeId) and collecting every triangle node reached into `out_triangles`. `node_pool`
// must span the full byte range the walk could reach, addressed by BvhNodeId::byte_offset.
// Never crashes on bad input: stops and reports why via the return value instead, leaving
// whatever was already collected in `out_triangles`. Iterative, not recursive, so a cyclic or
// adversarial tree fails via TooManyNodes rather than a stack overflow.
BvhWalkResult WalkBvhTriangles(std::span<const uint8_t> node_pool, BvhNodeId root_id,
                               std::vector<BvhTriangleNode>& out_triangles);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODEPARSER_H_ */
