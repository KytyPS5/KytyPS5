#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODE_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODE_H_

#include <cstdint>

namespace Libs::Graphics {

// RDNA2 hardware BVH4 node layout traversed by IMAGE_BVH_INTERSECT_RAY /
// IMAGE_BVH64_INTERSECT_RAY. The layout is fixed by the hardware, not PS5-specific: any game
// driving this instruction on real hardware must produce data in this shape. Cross-referenced
// against Mesa RADV's src/amd/vulkan/bvh/bvh.h (MIT-licensed) as the practical reference, since
// RADV must build BVHs in this exact format for its own ray-query support.
//
// The tag/shift arithmetic for decoding a packed guest node pointer into (type, byte offset) is
// not yet verified and is intentionally not implemented here; callers currently locate node
// bytes some other way. See docs/superpowers/specs for the fuller design notes.

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

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_RT_BVHNODE_H_ */
