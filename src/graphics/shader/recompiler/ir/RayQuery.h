#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_RAYQUERY_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_RAYQUERY_H_

#include <array>
#include <cstdint>

// The flags of the RayQuery* IR operations (native ray tracing, see IR::TraceNativeTraversals):
// each native traversal (site) owns one host ray query per lane and a short list of the candidate
// leaves the query found; the wave processes one (instance, leaf) key at a time, the smallest
// any lane has, with every lane that has it.
namespace Libs::Graphics::ShaderRecompiler::IR {

constexpr uint32_t MaxRayQuerySites = 4;
// BvhIntersect flags (the node pointer's words, 1 or 2) bit of the intersections of the walks of
// native sites, which keep the guest's loop for when the host has no structures for their TLAS:
// the ray query pass (TraceTraversalsWithRayQueries) leaves them.
constexpr uint32_t BvhIntersectNative = 1u << 8;
// Candidates a lane collects before the wave processes them.
constexpr uint32_t RayQueryListSize = 32;

// RayQueryResult: the lane's opaque hit (found by the hardware), for lanes whose guest code did
// not accept another one.
enum class RayQueryResultKind : uint32_t {
	Has        = 0, // 1 when the lane has one.
	T          = 1, // The hit distance (float bits).
	Word       = 2, // The primitive word of its guest list entry.
	InstanceLo = 3, // The address of its guest instance record.
	InstanceHi = 4,
	U          = 5, // Barycentrics as the guest triangle test reports them (closest hit sites).
	V          = 6,
	Node       = 7, // The guest triangle node of the hit.
};

// RayQueryInit variant bits.
constexpr uint32_t RayQueryInitBothFacings = 1u;
// Opacity from the geometry only (the primitive word's opaque bit; instance flags do not
// count): every triangle is a candidate, the opaque ones are confirmed and the alpha tested
// ones the per-lane alpha test accepts.
constexpr uint32_t RayQueryInitGeometryOpacity = 2u;
// No guest leaf code (IR::TraceNativeTraversals: Entry and PacketLane): without the per-lane alpha
// test the site keeps the guest traversal. The two last arguments flip the facing the lanes cull,
// and allow the native traversal.
constexpr uint32_t RayQueryInitNoLeaf = 4u;
// Every triangle is a candidate of the guest leaf code, which tests its facing and opacity
// (IR::TraceNativeTraversals: Marvel's Wolverine's closest hit walks with a per lane BLAS walk).
constexpr uint32_t RayQueryInitAllCandidates = 8u;
// With NoLeaf and no alpha test (IR::TraceNativeTraversals: GTA V's PacketWave walks whose leaf code
// takes the opaque triangles only): the triangles that are not opaque are no hit (the lanes with the
// alpha test off take every triangle as opaque).
constexpr uint32_t RayQueryInitOpaqueOnly = 16u;

// RayQueryNext, RayQueryMatch and RayQuerySelectedLeaf variant: the wave takes the smallest
// instance any lane has, and each lane with a key of it its own smallest leaf of it.
constexpr uint32_t RayQueryKeyLaneLeaf = 1u;

// RayQueryAlpha variant: how the alpha test reads its data (IR::TraceNativeTraversals): Scalar, as
// the leaf code of a wave-wide BLAS walk does (scalar loads; the reference is threshold * (M0 bit 0 ?
// factor : 1)), or Lane, as a per lane leaf code does.
constexpr uint32_t RayQueryAlphaScalar = 0u;
constexpr uint32_t RayQueryAlphaLane   = 1u;
// With the Lane layout: the reference is threshold * (M0 bit 0 ? factor : 1).
constexpr uint32_t RayQueryAlphaFactor = 2u;
// The Entry layout: a closest hit site without guest leaf code whose alpha test is Resident Evil
// Village's (the node result of an accepted hit is its leaf list entry's index + 1).
constexpr uint32_t RayQueryAlphaEntry  = 4u;

// RayQueryMark: state changes.
enum class RayQueryMarkKind : uint32_t {
	Resolve = 1, // After the leaf: the selected lanes drop the key.
	Exit    = 4, // Ends the native traversal of the site.
};

// A field of the layouts below that the walk does not have.
constexpr uint32_t NoField = 0xffffffffu;

// How the guest's top-level BVH is laid out, as the traversal code reads it (derived from the
// walk's loads, see DeriveLaneLayout in RayQueryTraversal.cpp); versions of the BVH library differ.
// The instance records are `record_stride` bytes at header + the 64-bit offset at header +
// `instance_offset`, one per instance leaf (node >> 3). Fields are record dwords:
//   transform    the world to object 3x4 matrix: row r (object x, y, z) and column c (world x,
//                y, z, then the translation) at transform[r * 4 + c]
//   BLAS         the BLAS address: dwords blas, blas + 1 as they are (raw), or packed as
//                ((lo | (hi & 0x3fffff) << 32) << 3)
//   root         the BLAS root node
//   mask         the instance mask: (dword >> mask_shift) & 0xff, which the ray's mask selects
//                (NoField: the traversal tests no mask, every instance counts); with a
//                `mask_width`, the instance counts for every ray when any of the `mask_width` bits
//                from `mask_shift` is set
//   flags        the instance flags: (dword >> flags_shift); bit 0 opaque, bit 1 alpha tested
//                anyway, bit 2 both facings, bit 3 the culled facing (NoField: the program never
//                reads them; its walks do not depend on them)
// `flip_facing`: whether the guest's triangle test accepts, with flags bit 3 clear, the triangles
// whose test sign is set (the host's back faces).
struct RayTracingLayout {
	bool                    valid           = false;
	uint32_t                instance_offset = 0;
	uint32_t                record_stride   = 128;
	std::array<uint8_t, 12> transform {};
	uint32_t                blas_dword      = 0;
	bool                    blas_packed     = false;
	uint32_t                root_dword      = 0;
	uint32_t                mask_dword      = 0;
	uint32_t                mask_shift      = 0;
	uint32_t                mask_width      = 0;
	uint32_t                flags_dword     = NoField;
	uint32_t                flags_shift     = 0;
	bool                    flip_facing     = true;
	// Instance leaves of node type `inline_type` (NoField: none) have their record at header
	// + `inline_offset` and the BLAS root `inline_root`.
	uint32_t                inline_type     = NoField;
	uint32_t                inline_offset   = 0;
	uint32_t                inline_root     = 0;
	// Where the guest's BVH headers (the TLAS's and the BLASes') hold their node count: the T#
	// size at header + 88, or (`node_count_80`) the count at header + 80, one more than that size.
	bool                    node_count_80   = false;

	bool operator==(const RayTracingLayout&) const = default;
	// Four words for the host passes (see LoadGuestInstance in rt_common.inc).
	[[nodiscard]] std::array<uint32_t, 4> Pack() const;
	[[nodiscard]] uint32_t                TransformBase() const;
};

// The fields of the guest BVH GTA V's alpha tests read beyond the instance record's (RayQueryAlpha,
// see IR::TraceNativeTraversals): record dword of the material base, BLAS header dwords of the table of the
// geometry of each triangle node and of the first triangle node, and how the vertex offsets are
// formed: (M1 & mask) + index * (M0 & mask), aligned down to 4 bytes when `align`.
struct RayTracingAlphaFields {
	uint32_t material_dword = 0;
	uint32_t table_dword    = 0;
	uint32_t first_dword    = 0;
	uint32_t vertex_mask    = 0xfc;
	bool     vertex_align   = false;
	// Packed with the BLAS location of the layout into one word (RayQueryAlpha's format).
	[[nodiscard]] uint32_t Pack(const RayTracingLayout& layout) const;
};

[[nodiscard]] constexpr uint32_t RayQueryFlags(uint32_t site, uint32_t variant) noexcept {
	return site | (variant << 8u);
}
[[nodiscard]] constexpr uint32_t RayQuerySite(uint32_t flags) noexcept {
	return flags & 0xffu;
}
[[nodiscard]] constexpr uint32_t RayQueryVariant(uint32_t flags) noexcept {
	return flags >> 8u;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_RAYQUERY_H_
