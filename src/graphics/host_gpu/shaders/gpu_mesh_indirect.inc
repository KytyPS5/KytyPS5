// The GPU twin of the record derivations in dispatchGuard.h; both must decide every record alike.

// IR::MeshTaskPayloadDwordCount and IR::MeshTaskPayloadIndexBound.
const uint MESH_INDIRECT_PAYLOAD_DWORDS      = 10u;
const uint MESH_INDIRECT_PAYLOAD_INDEX_BOUND = 9u;

struct MeshDrawShape {
	uint  primitive_size;
	uint  primitive_step;
	uint  primitives_per_group;
	uvec3 max_groups;
	uint  max_total;
};

bool MeshSumFits(uint lhs, uint rhs) {
	uint       carry;
	const uint sum = uaddCarry(lhs, rhs, carry);
	return carry == 0u || sum == 0u;
}

bool MeshProductFits(uint lhs, uint rhs, uint limit) {
	uint high;
	uint low;
	umulExtended(lhs, rhs, high, low);
	return high == 0u && low <= limit;
}

bool MeshDrawCountsRepresentable(uvec4 record) {
	return MeshSumFits(record.x, record.z) && MeshSumFits(record.y, record.w) &&
	       MeshProductFits(record.x, record.y, 0xffffffffu);
}

uint MeshInputPrimitiveCount(uint vertices, uint size, uint step) {
	return vertices < size ? 0u : (vertices - size) / step + 1u;
}

// (count, instances, start vertex, start instance) -> the mesh grid, or zero when skipped.
uvec2 MeshIndirectGroups(MeshDrawShape shape, uvec4 record) {
	if (record.x == 0u || record.y == 0u || !MeshDrawCountsRepresentable(record)) {
		return uvec2(0u);
	}
	const uint primitives =
	    MeshInputPrimitiveCount(record.x, shape.primitive_size, shape.primitive_step);
	if (primitives == 0u) {
		return uvec2(0u);
	}
	const uint groups = (primitives - 1u) / shape.primitives_per_group + 1u;
	if (groups > shape.max_groups.x || record.y > shape.max_groups.y || shape.max_groups.z < 1u ||
	    !MeshProductFits(groups, record.y, shape.max_total)) {
		return uvec2(0u);
	}
	return uvec2(groups, record.y);
}

// An indexed record's counts, checked against a first element of zero as DrawIndex does.
uvec2 MeshIndexedGroups(MeshDrawShape shape, uint count, uint instances, uint first_instance) {
	return MeshIndirectGroups(shape, uvec4(count, instances, 0u, first_instance));
}

// The record's first index address (low, high) and how many indices from it are readable.
uvec3 MeshIndexWindow(uint64_t base, uint element_size, uint buffer_size, uint first_index) {
	const uint64_t address  = base + uint64_t(first_index) * uint64_t(element_size);
	uint           readable = 0xffffffffu;
	if (buffer_size != 0u) {
		readable = buffer_size > first_index ? buffer_size - first_index : 0u;
	}
	return uvec3(unpackUint2x32(address), readable);
}
