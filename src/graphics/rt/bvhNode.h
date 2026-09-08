#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODE_H_

#include <cstdint>

namespace Libs::Graphics {

// RDNA2 hardware BVH4 node layout traversed by IMAGE_BVH_INTERSECT_RAY /
// IMAGE_BVH64_INTERSECT_RAY. The layout is fixed by the hardware, not PS5-specific: any game
// driving this instruction on real hardware must produce data in this shape. Cross-referenced
// against Mesa RADV's src/amd/vulkan/bvh/ (bvh.h for node layout, bvh_helpers.h for node-pointer
// packing; MIT-licensed) as the practical reference, since RADV must build BVHs in this exact
// format for its own ray-query support.
//
// Node pointers are relative to the node pool base (the accel-structure header's bvh_offset),
// not a fixed or global address; locating that base is not implemented here.

enum class BvhNodeType : uint32_t {
Triangle = 0,
Box16    = 4,
Box32    = 5,
Instance = 6,
Aabb     = 7,
};

struct BvhAabb {
float min[3];
float max[3];
};

struct BvhBox32Node {
uint32_t children[4];
BvhAabb  bounds[4];
uint32_t flags;
};

struct BvhTriangleNode {
float    vertices[3][3];
uint32_t triangle_id;
uint32_t geometry_id_and_flags;
uint32_t id;
};

// A decoded guest BVH node pointer/id: which node type it refers to, and its byte offset from
// the start of the node pool (the accel-structure header's node-pool base is not modeled here).
struct BvhNodeId {
BvhNodeType type;
uint32_t    byte_offset;
};

// Sentinel value for an empty/absent child slot in a box node's children[] array. Confirmed in
// Mesa RADV source as RADV_BVH_INVALID_NODE (src/amd/vulkan/bvh/bvh_defines.h), and confirmed as
// what's actually written to unused box32/box16 child slots by the BVH build shaders
// (src/amd/vulkan/bvh/encode.comp), not just a convention inferred from context.
inline constexpr uint32_t BvhInvalidNodeId = 0xFFFFFFFFu;

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODE_H_ */
