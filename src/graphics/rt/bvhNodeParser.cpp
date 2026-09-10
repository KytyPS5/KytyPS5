#include "graphics/rt/bvhNodeParser.h"

#include "common/assert.h"

#include <bit>
#include <cstring>

namespace Libs::Graphics {
namespace {

uint32_t ReadU32(std::span<const uint8_t> raw, size_t offset) {
	uint32_t value = 0;
	std::memcpy(&value, raw.data() + offset, sizeof(value));
	return value;
}

float ReadF32(std::span<const uint8_t> raw, size_t offset) {
	return std::bit_cast<float>(ReadU32(raw, offset));
}

// Widens byte_offset before adding node_size so a maliciously/accidentally huge decoded offset
// (near UINT32_MAX) can't wrap back into range and defeat the bounds check.
bool NodeFits(std::span<const uint8_t> node_pool, uint32_t byte_offset, uint32_t node_size) {
	return static_cast<uint64_t>(byte_offset) + node_size <= node_pool.size();
}

} // namespace

BvhBox32Node ParseBvhBox32Node(std::span<const uint8_t> raw) {
	EXIT_IF(raw.size() != BvhBox32NodeByteSize);

	BvhBox32Node node {};
	for (uint32_t i = 0; i < 4; i++) {
		node.children[i] = ReadU32(raw, i * 4u);
	}
	for (uint32_t i = 0; i < 4; i++) {
		const size_t base     = 16u + i * 24u;
		node.bounds[i].min[0] = ReadF32(raw, base + 0u);
		node.bounds[i].min[1] = ReadF32(raw, base + 4u);
		node.bounds[i].min[2] = ReadF32(raw, base + 8u);
		node.bounds[i].max[0] = ReadF32(raw, base + 12u);
		node.bounds[i].max[1] = ReadF32(raw, base + 16u);
		node.bounds[i].max[2] = ReadF32(raw, base + 20u);
	}
	node.flags = ReadU32(raw, 112u);
	return node;
}

BvhTriangleNode ParseBvhTriangleNode(std::span<const uint8_t> raw) {
	EXIT_IF(raw.size() != BvhTriangleNodeByteSize);

	BvhTriangleNode node {};
	for (uint32_t v = 0; v < 3; v++) {
		for (uint32_t c = 0; c < 3; c++) {
			node.vertices[v][c] = ReadF32(raw, (v * 3u + c) * 4u);
		}
	}
	// Bytes 36..47 are reserved (skipped); triangle_id follows at 48.
	node.triangle_id           = ReadU32(raw, 48u);
	node.geometry_id_and_flags = ReadU32(raw, 52u);
	// Bytes 56..59 are reserved2 (skipped); id follows at 60.
	node.id = ReadU32(raw, 60u);
	return node;
}

BvhNodeId DecodeBvhNodeId(uint32_t id) {
	return BvhNodeId {
	    .type        = static_cast<BvhNodeType>(id & 0x7u),
	    .byte_offset = (id & ~0x7u) << 3u,
	};
}

uint32_t EncodeBvhNodeId(BvhNodeType type, uint32_t byte_offset) {
	EXIT_IF((byte_offset & 0x3Fu) != 0u);
	return (byte_offset >> 3u) | static_cast<uint32_t>(type);
}

BvhWalkResult WalkBvhTriangles(std::span<const uint8_t> node_pool, BvhNodeId root_id,
                               std::vector<BvhTriangleNode>& out_triangles) {
	std::vector<BvhNodeId> pending;
	pending.push_back(root_id);

	uint32_t visited = 0;
	while (!pending.empty()) {
		const auto current = pending.back();
		pending.pop_back();

		if (++visited > MaxBvhWalkNodes) {
			return BvhWalkResult::TooManyNodes;
		}

		if (current.type == BvhNodeType::Triangle) {
			if (!NodeFits(node_pool, current.byte_offset, BvhTriangleNodeByteSize)) {
				return BvhWalkResult::OutOfBounds;
			}
			out_triangles.push_back(ParseBvhTriangleNode(
			    node_pool.subspan(current.byte_offset, BvhTriangleNodeByteSize)));
			continue;
		}

		if (current.type != BvhNodeType::Box32) {
			return BvhWalkResult::UnsupportedNodeType;
		}

		if (!NodeFits(node_pool, current.byte_offset, BvhBox32NodeByteSize)) {
			return BvhWalkResult::OutOfBounds;
		}
		const auto box =
		    ParseBvhBox32Node(node_pool.subspan(current.byte_offset, BvhBox32NodeByteSize));
		for (const auto child_id: box.children) {
			if (child_id == BvhInvalidNodeId) {
				continue;
			}
			pending.push_back(DecodeBvhNodeId(child_id));
		}
	}
	return BvhWalkResult::Ok;
}

} // namespace Libs::Graphics
