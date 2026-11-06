// Native ray tracing (see nativeRayTracing.h): shared definitions of the passes that read the
// guest's AMD BVHs through the BDA page table and write the host acceleration structures.

#extension GL_EXT_buffer_reference2 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_atomic_int64 : require

#ifndef CACHING_PAGEBITS
#error CACHING_PAGEBITS must be defined
#endif

layout(buffer_reference, std430, buffer_reference_align = 4) buffer U32Ref { uint v; };
layout(buffer_reference, std430, buffer_reference_align = 16) buffer U32x4Ref { uvec4 v; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer U64Ref { uint64_t v; };
layout(buffer_reference, std430, buffer_reference_align = 8) buffer U32x2Ref { uvec2 v; };
layout(buffer_reference, std430, buffer_reference_align = 4) buffer F32Ref { float v; };

// The BufferCache device-address page table: the host address of each caching page (0 when the
// page has no cached buffer).
layout(set = 0, binding = 0, std430) readonly buffer PageTable {
	uint64_t pages[];
};

const uint64_t kExtendedMemoryBase = 0x080000000000ul;
const uint64_t kLowerAddressSize   = 1ul << 40;
const uint     kPageMask           = (1u << CACHING_PAGEBITS) - 1u;
const uint     kInvalidNode        = 0xffffffffu;

#ifdef RT_RECORD_PAGES
// The fault bitmap of the BufferCache fault buffer (see faultManager.h). The walks record the
// caching pages without a buffer that they read as the device-address accesses of guest shaders
// do, so that the host caches (and keeps) them for the next walks. Pages with a buffer are not
// recorded as used: a load of the use bitmap in each read made GTA V's walks three times slower.
layout(set = 0, binding = 1, std430) buffer FaultPages {
	uint fault_pages[];
};

void RecordFault(uint page) {
	const uint word = page >> 5;
	if (word < uint(fault_pages.length())) {
		// Not the entry after the last page (the native ray tracing context).
		atomicOr(fault_pages[word], 1u << (page & 31u));
	}
}
#endif

// The host address of guest address `va`, 0 when it has no cached buffer.
uint64_t HostAddress(uint64_t va) {
	const uint64_t packed = va >= kExtendedMemoryBase ? va - kExtendedMemoryBase + kLowerAddressSize : va;
	const uint64_t page   = packed >> CACHING_PAGEBITS;
	if (page >= uint64_t(pages.length())) {
		return 0;
	}
	const uint64_t base = pages[uint(page)];
#ifdef RT_RECORD_PAGES
	if (base == 0) {
		RecordFault(uint(page));
	}
#endif
	return base == 0 ? 0 : base + (va & uint64_t(kPageMask));
}

// Guest loads (zero where the memory is not cached). Aligned 16-byte loads never cross a caching
// page.
uint LoadGuestU32(uint64_t va) {
	const uint64_t host = HostAddress(va);
	return host == 0 ? 0u : U32Ref(host).v;
}

uvec4 LoadGuestU32x4(uint64_t va) {
	const uint64_t host = HostAddress(va);
	return host == 0 ? uvec4(0) : U32x4Ref(host).v;
}

uvec2 LoadGuestU32x2(uint64_t va) {
	const uint64_t host = HostAddress(va);
	return host == 0 ? uvec2(0) : U32x2Ref(host).v;
}

uint64_t LoadGuestU64(uint64_t va) {
	return uint64_t(LoadGuestU32(va)) | (uint64_t(LoadGuestU32(va + 4)) << 32);
}

// An AMD BVH node pointer: the node type in the low 3 bits, the 64-byte node index above.
uint NodeType(uint node) {
	return node & 7u;
}

uint64_t NodeAddress(uint64_t base, uint node) {
	return base + (uint64_t(node >> 3) << 6);
}

bool IsBoxNode(uint node) {
	return node != kInvalidNode && (NodeType(node) == 4u || NodeType(node) == 5u);
}

// A box node: four child pointers and their bounds (box16: fp16 pairs, box32: floats).
struct BoxNode {
	uint children[4];
	vec3 lo[4];
	vec3 hi[4];
};

// A 64-byte node never crosses a caching page: one translation (zeros where it is not cached).
uvec4 LoadNodeU32x4(uint64_t host, uint offset) {
	return host == 0 ? uvec4(0) : U32x4Ref(host + offset).v;
}

BoxNode LoadBoxNode(uint64_t base, uint node) {
	const uint64_t host = HostAddress(NodeAddress(base, node));
	BoxNode        box;
	const uvec4    first = LoadNodeU32x4(host, 0);
	box.children[0] = first.x;
	box.children[1] = first.y;
	box.children[2] = first.z;
	box.children[3] = first.w;
	if (NodeType(node) == 4u) {
		const uvec4 a = LoadNodeU32x4(host, 16);
		const uvec4 b = LoadNodeU32x4(host, 32);
		const uvec4 c = LoadNodeU32x4(host, 48);
		const uint words[12] = uint[12](a.x, a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w);
		for (uint child = 0; child < 4; child++) {
			const vec2 xy = unpackHalf2x16(words[child * 3]);
			const vec2 zx = unpackHalf2x16(words[child * 3 + 1]);
			const vec2 yz = unpackHalf2x16(words[child * 3 + 2]);
			box.lo[child] = vec3(xy.x, xy.y, zx.x);
			box.hi[child] = vec3(zx.y, yz.x, yz.y);
		}
	} else {
		uint words[24];
		for (uint i = 0; i < 6; i++) {
			const uvec4 v = LoadNodeU32x4(host, 16 + i * 16);
			words[i * 4]     = v.x;
			words[i * 4 + 1] = v.y;
			words[i * 4 + 2] = v.z;
			words[i * 4 + 3] = v.w;
		}
		for (uint child = 0; child < 4; child++) {
			box.lo[child] = uintBitsToFloat(uvec3(words[child * 6], words[child * 6 + 1], words[child * 6 + 2]));
			box.hi[child] = uintBitsToFloat(uvec3(words[child * 6 + 3], words[child * 6 + 4], words[child * 6 + 5]));
		}
	}
	return box;
}

// The state of one preparation (NativeRayTracing::Prepare), cleared before the walks.
layout(buffer_reference, std430, buffer_reference_align = 8) buffer RtState {
	uint64_t header_va;
	uint64_t instance_base;
	uint     node_limit;
	uint     instance_count;
	uint     box_count;
	uint     overflow;
	uint     missing_count;
	uint     dispatch_x;
	uint     dispatch_y;
	uint     dispatch_z;
	uint     unknown_blas;
	uint     inactive;
	uint     max_depth;
	uint     verify_count;
	uint     verify_x;
	uint     verify_y;
	uint     verify_z;
	uint     changed_count;
	uint     pool_cursor;
	uint     pool_overflow;
	uint     pad0;
	uint     pad1;
};

// A host BLAS: the guest BLAS it mirrors and its acceleration structure of triangles, in two
// geometries: 0 = opaque, 1 = alpha tested. Per primitive (geometry 1 after the capacity of
// geometry 0), the entry table holds (16 bytes) the guest leaf pointer of its list entry, its primitive
// word, its triangle node and the barycentric remap of the node (see rt_blas_walk.comp).
layout(buffer_reference, std430, buffer_reference_align = 8) buffer SlotInfo {
	uint64_t as_address;
	uint64_t entry_table;
	uint     capacity0;
	uint     capacity1;
	uint     count0;
	uint     count1;
	uint     signature;
	uint     state;
	uint64_t blas_va;
	uint     root;
	uint     stamp;
	uint     leaf_count;
	// Set by a gathered build (see MODE_GATHER in rt_blas_walk.comp) to bit 31 and the preparation:
	// the triangles follow the guest's refit boxes, so the check of that same preparation takes their
	// signature instead of reporting a change. A later check (the guest may have changed the BLAS
	// since) compares as usual.
	uint     rebase;
};

// A guest BLAS the top-level walk did not find, counted by the BLAS walk.
layout(buffer_reference, std430, buffer_reference_align = 8) buffer MissingEntry {
	uint64_t blas_va;
	uint     root;
	uint     table_index;
	uint     leaf_count;
	uint     box_count;
	uint     opaque_count;
	uint     alpha_count;
	uint     node_limit;
	uint     flags;
	uint     pad0;
	uint     pad1;
};

const uint kSlotStride    = 64;
const uint kMissingStride = 48;
const uint kSlotBuilt     = 1u;
// Entries of one guest leaf list at most.
const uint kMaxLeafEntries = 64;

// The vertices of triangle `type` (0-3) of an AMD triangle node with five vertices: (0,1,2),
// (1,3,2), (2,3,4), (2,4,0).
uvec3 TriangleVertexIds(uint type) {
	return uvec3(min(type, 2u), type == 0u ? 1u : (type == 3u ? 4u : 3u),
	             type == 2u ? 4u : (type == 3u ? 0u : 2u));
}

vec3 LoadVertex(uint64_t node, uint id) {
	return uintBitsToFloat(uvec3(LoadGuestU32(node + id * 12), LoadGuestU32(node + id * 12 + 4),
	                             LoadGuestU32(node + id * 12 + 8)));
}

// The three vertices of triangle `type` of a triangle node, read once with one translation (a
// 64-byte node of a BVH, whose base is 256-byte aligned, never crosses a caching page); zeros
// where the node is not cached, as LoadVertex().
void LoadTriangle(uint64_t node, uint type, out vec3 p[3]) {
	const uint64_t host = HostAddress(node);
	const uvec3    ids  = TriangleVertexIds(type);
	for (uint v = 0; v < 3; v++) {
		const uint64_t at = host + uint64_t(ids[v] * 12);
		p[v] = host == 0 ? vec3(0.0) : uintBitsToFloat(uvec3(U32Ref(at).v, U32Ref(at + 4).v, U32Ref(at + 8).v));
	}
}

// A guest TLAS instance record (128 bytes, or the stride of the layout), laid out as the traversal
// shader that traces through the TLAS reads it (IR::RayTracingLayout::Pack(), derived from
// its code; record dwords take 4 bits in their field and 2 more in word 3): the rows of the
// world to object 3x4 transform, the BLAS address and root node, the instance mask (0xff when the
// shader traces every instance) and flags (bit 0 opaque, bit 1 alpha tested anyway, bit 2 both
// facings, bit 3 the culled facing; 0 when the shader never reads them).
struct GuestInstance {
	vec4     rows[3];
	uint64_t blas;
	uint     root;
	uint     mask;
	uint     flags;
};

// The header field holding the offset of the instance records from the header.
uint LayoutInstanceOffset(uvec4 instance_layout) {
	return ((instance_layout.y >> 16) & 15u) * 8u;
}

// Whether instance leaf `node` is of the layout's inline type: its record is at header +
// LayoutInlineOffset() and its BLAS root LayoutInlineRoot().
bool LayoutInline(uvec4 instance_layout, uint node) {
	return ((instance_layout.w >> 19) & 1u) != 0 && NodeType(node) == ((instance_layout.w >> 16) & 7u);
}

uint LayoutInlineOffset(uvec4 instance_layout) {
	return (instance_layout.w & 0xffu) * 8u + ((instance_layout.w >> 28) & 1u) * 4u;
}

uint LayoutRecordStride(uvec4 instance_layout) {
	return 128u + ((instance_layout.y >> 30) & 3u) * 32u;
}

// Whether the BVH headers hold their node count at +80 (the size of the T# plus one) instead of
// the size at +88.
bool LayoutNodeCountAt80(uvec4 instance_layout) {
	return ((instance_layout.w >> 29) & 1u) != 0;
}

// A record dword of the layout: `low` (4 bits) and `high` (2 bits) above it.
uint LayoutDword(uint low, uint high) {
	return (low & 15u) | ((high & 3u) << 4);
}

uint LayoutInlineRoot(uvec4 instance_layout) {
	return (instance_layout.w >> 8) & 0xffu;
}

// The record of instance leaf `node`.
uint64_t InstanceRecord(uint64_t header, uint64_t instance_base, uint node, uvec4 instance_layout) {
	return LayoutInline(instance_layout, node) ? header + LayoutInlineOffset(instance_layout)
	                                           : instance_base + uint64_t(node >> 3) * LayoutRecordStride(instance_layout);
}

// Whether, with flags bit 3 clear, the guest accepts the triangles the host sees from the back.
bool LayoutFlipFacing(uvec4 instance_layout) {
	return ((instance_layout.y >> 29) & 1u) != 0;
}

GuestInstance LoadGuestInstance(uint64_t record, uvec4 instance_layout) {
	// The first 64 bytes, and the rest of a record longer than 128 bytes.
	uint words[40];
	for (uint q = 0; q < 10; q++) {
		const uvec4 v = q < 4u || LayoutRecordStride(instance_layout) > 128u ? LoadGuestU32x4(record + q * 16u) : uvec4(0u);
		words[q * 4u]      = v.x;
		words[q * 4u + 1u] = v.y;
		words[q * 4u + 2u] = v.z;
		words[q * 4u + 3u] = v.w;
	}
	GuestInstance instance;
	const uint    base = (instance_layout.z >> 20) & 31u;
	for (uint r = 0; r < 3; r++) {
		vec4 row;
		for (uint c = 0; c < 4; c++) {
			const uint index = r * 4 + c;
			row[c] = uintBitsToFloat(words[min(base + ((instance_layout[index / 8] >> ((index % 8) * 4)) & 15u), 39u)]);
		}
		instance.rows[r] = row;
	}
	const uint blas = LayoutDword(instance_layout.y >> 20, instance_layout.w >> 20);
	const uint lo   = words[blas];
	const uint hi   = words[min(blas + 1u, 39u)];
	instance.blas   = ((instance_layout.y >> 24) & 1u) != 0 ? (uint64_t(lo) | (uint64_t(hi & 0x3fffffu) << 32)) << 3
	                                                : uint64_t(lo) | (uint64_t(hi) << 32);
	instance.root   = words[LayoutDword(instance_layout.y >> 25, instance_layout.w >> 22)];
	// The mask the ray's selects, or (with a width) every ray when any of its bits is set.
	const uint mask_bits  = words[LayoutDword(instance_layout.z, instance_layout.w >> 24)] >> ((instance_layout.z >> 4) & 31u);
	const uint mask_width = (instance_layout.z >> 25) & 31u;
	instance.mask         = ((instance_layout.z >> 19) & 1u) == 0 ? 0xffu
	                        : mask_width == 0u                    ? mask_bits & 0xffu
	                        : (mask_bits & ((1u << mask_width) - 1u)) != 0u ? 0xffu
	                                                                        : 0u;
	instance.flags = ((instance_layout.z >> 13) & 1u) != 0
	                     ? words[LayoutDword(instance_layout.z >> 9, instance_layout.w >> 26)] >> ((instance_layout.z >> 14) & 31u)
	                     : 0u;
	return instance;
}

// A cheap mix of 32-bit words for signatures.
uint Mix(uint hash, uint value) {
	hash ^= value + 0x9e3779b9u + (hash << 6) + (hash >> 2);
	return hash;
}
