#include "common/assert.h"
#include "graphics/rt/bvhNodeParser.h"

#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <string_view>
#include <vector>

namespace {

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "BvhNodeParserTests: failed: %s\n", text);
		std::abort();
	}
}

void WriteU32(std::vector<uint8_t>& buffer, size_t offset, uint32_t value) {
	std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void WriteF32(std::vector<uint8_t>& buffer, size_t offset, float value) {
	WriteU32(buffer, offset, std::bit_cast<uint32_t>(value));
}

// Writes a minimal triangle node at `offset`, using `id` as its self-referential id field so
// tests can tell collected triangles apart. Vertex/reserved/triangle_id/geometry_id fields are
// left zeroed; they're already covered by TestParseBvhTriangleNodeExtractsVerticesAndIds.
void PlaceTriangleNode(std::vector<uint8_t>& buffer, size_t offset, uint32_t id) {
	WriteU32(buffer, offset + 60u, id); // id field, per the triangle node layout
}

void PlaceBox32Node(std::vector<uint8_t>& buffer, size_t offset, const uint32_t (&children)[4]) {
	for (size_t i = 0; i < 4; i++) {
		WriteU32(buffer, offset + i * 4u, children[i]);
	}
}

void TestParseBvhBox32NodeExtractsChildrenAndBounds() {
	using namespace Libs::Graphics;

	std::vector<uint8_t> raw(BvhBox32NodeByteSize, 0);
	WriteU32(raw, 0, 0x10);
	WriteU32(raw, 4, 0x20);
	WriteU32(raw, 8, 0x30);
	WriteU32(raw, 12, 0x40);
	// First child's AABB, bytes 16..39: min (1,2,3), max (4,5,6).
	WriteF32(raw, 16, 1.0f);
	WriteF32(raw, 20, 2.0f);
	WriteF32(raw, 24, 3.0f);
	WriteF32(raw, 28, 4.0f);
	WriteF32(raw, 32, 5.0f);
	WriteF32(raw, 36, 6.0f);
	// Fourth child's AABB, bytes 88..111: min (7,8,9), max (10,11,12).
	WriteF32(raw, 88, 7.0f);
	WriteF32(raw, 92, 8.0f);
	WriteF32(raw, 96, 9.0f);
	WriteF32(raw, 100, 10.0f);
	WriteF32(raw, 104, 11.0f);
	WriteF32(raw, 108, 12.0f);
	WriteU32(raw, 112, 0xABCDu);

	const auto node = ParseBvhBox32Node(raw);

	Check(node.children[0] == 0x10u && node.children[1] == 0x20u && node.children[2] == 0x30u &&
	          node.children[3] == 0x40u,
	      "box32 node child pointers did not match the raw bytes");
	Check(node.bounds[0].min[0] == 1.0f && node.bounds[0].min[1] == 2.0f &&
	          node.bounds[0].min[2] == 3.0f && node.bounds[0].max[0] == 4.0f &&
	          node.bounds[0].max[1] == 5.0f && node.bounds[0].max[2] == 6.0f,
	      "box32 node first child AABB did not match the raw bytes");
	Check(node.bounds[3].min[0] == 7.0f && node.bounds[3].min[1] == 8.0f &&
	          node.bounds[3].min[2] == 9.0f && node.bounds[3].max[0] == 10.0f &&
	          node.bounds[3].max[1] == 11.0f && node.bounds[3].max[2] == 12.0f,
	      "box32 node fourth child AABB did not match the raw bytes (offset math wrong?)");
	Check(node.flags == 0xABCDu, "box32 node flags did not match the raw bytes");
}

void TestParseBvhTriangleNodeExtractsVerticesAndIds() {
	using namespace Libs::Graphics;

	std::vector<uint8_t> raw(BvhTriangleNodeByteSize, 0);
	// 3 vertices x 3 floats, bytes 0..35: values 1.0 through 9.0 in order.
	float value = 1.0f;
	for (size_t i = 0; i < 9; i++) {
		WriteF32(raw, i * 4, value);
		value += 1.0f;
	}
	WriteU32(raw, 48, 0x1234u); // triangle_id, after reserved bytes 36..47
	WriteU32(raw, 52, 0x5678u); // geometry_id_and_flags
	WriteU32(raw, 60, 0x9abcu); // id, after reserved2 bytes 56..59

	const auto node = ParseBvhTriangleNode(raw);

	Check(node.vertices[0][0] == 1.0f && node.vertices[0][1] == 2.0f && node.vertices[0][2] == 3.0f,
	      "triangle node first vertex did not match the raw bytes");
	Check(node.vertices[2][0] == 7.0f && node.vertices[2][1] == 8.0f && node.vertices[2][2] == 9.0f,
	      "triangle node third vertex did not match the raw bytes (offset math wrong?)");
	Check(node.triangle_id == 0x1234u, "triangle node triangle_id did not match the raw bytes");
	Check(node.geometry_id_and_flags == 0x5678u,
	      "triangle node geometry_id_and_flags did not match the raw bytes");
	Check(node.id == 0x9abcu,
	      "triangle node id did not match the raw bytes (reserved2 skip wrong?)");
}

void TestDecodeBvhNodeIdMatchesKnownRootPointer() {
	using namespace Libs::Graphics;

	// RADV_BVH_ROOT_NODE: pointer value 5 (type Box32, offset 0) per Mesa RADV.
	const auto root = DecodeBvhNodeId(5u);
	Check(root.type == BvhNodeType::Box32, "root node pointer did not decode as Box32");
	Check(root.byte_offset == 0u, "root node pointer did not decode to offset 0");
}

void TestBvhNodeIdRoundTripsForEveryTypeAndSeveralOffsets() {
	using namespace Libs::Graphics;

	const BvhNodeType types[] = {
	    BvhNodeType::Triangle, BvhNodeType::Box16, BvhNodeType::Box32,
	    BvhNodeType::Instance, BvhNodeType::Aabb,
	};
	const uint32_t offsets[] = {0u, 64u, 128u, 192u, 65536u};

	for (const auto type: types) {
		for (const auto offset: offsets) {
			const uint32_t  id      = EncodeBvhNodeId(type, offset);
			const BvhNodeId decoded = DecodeBvhNodeId(id);
			Check(decoded.type == type,
			      "node id round trip did not preserve type for some (type, offset) pair");
			Check(decoded.byte_offset == offset,
			      "node id round trip did not preserve byte offset for some (type, offset) pair");
		}
	}
}

void TestWalkBvhTrianglesCollectsASingleTriangleRoot() {
	using namespace Libs::Graphics;

	std::vector<uint8_t> pool(256, 0);
	PlaceTriangleNode(pool, 0, 0x1111u);

	std::vector<BvhTriangleNode> triangles;
	const auto                   result = WalkBvhTriangles(
        pool, DecodeBvhNodeId(EncodeBvhNodeId(BvhNodeType::Triangle, 0)), triangles);

	Check(result == BvhWalkResult::Ok, "single triangle root did not walk as Ok");
	Check(triangles.size() == 1, "single triangle root did not collect exactly one triangle");
	Check(triangles[0].id == 0x1111u, "single triangle root collected the wrong triangle");
}

void TestWalkBvhTrianglesSkipsInvalidChildSlots() {
	using namespace Libs::Graphics;

	std::vector<uint8_t> pool(256, 0);
	PlaceTriangleNode(pool, 64, 0xAAAAu);
	PlaceTriangleNode(pool, 128, 0xBBBBu);
	const uint32_t children[4] = {
	    EncodeBvhNodeId(BvhNodeType::Triangle, 64),
	    BvhInvalidNodeId,
	    EncodeBvhNodeId(BvhNodeType::Triangle, 128),
	    BvhInvalidNodeId,
	};
	PlaceBox32Node(pool, 0, children);

	std::vector<BvhTriangleNode> triangles;
	const auto                   result =
	    WalkBvhTriangles(pool, DecodeBvhNodeId(EncodeBvhNodeId(BvhNodeType::Box32, 0)), triangles);

	Check(result == BvhWalkResult::Ok, "box32 with invalid slots did not walk as Ok");
	Check(triangles.size() == 2, "box32 with invalid slots did not collect exactly two triangles");
	const bool has_aaaa = (triangles[0].id == 0xAAAAu || triangles[1].id == 0xAAAAu);
	const bool has_bbbb = (triangles[0].id == 0xBBBBu || triangles[1].id == 0xBBBBu);
	Check(has_aaaa && has_bbbb, "box32 with invalid slots did not collect the expected triangles");
}

void TestWalkBvhTrianglesRecursesThroughNestedBox32() {
	using namespace Libs::Graphics;

	std::vector<uint8_t> pool(512, 0);
	PlaceTriangleNode(pool, 256, 0xCCCCu);
	const uint32_t inner_children[4] = {
	    EncodeBvhNodeId(BvhNodeType::Triangle, 256),
	    BvhInvalidNodeId,
	    BvhInvalidNodeId,
	    BvhInvalidNodeId,
	};
	PlaceBox32Node(pool, 192, inner_children);
	const uint32_t root_children[4] = {
	    EncodeBvhNodeId(BvhNodeType::Box32, 192),
	    BvhInvalidNodeId,
	    BvhInvalidNodeId,
	    BvhInvalidNodeId,
	};
	PlaceBox32Node(pool, 0, root_children);

	std::vector<BvhTriangleNode> triangles;
	const auto                   result =
	    WalkBvhTriangles(pool, DecodeBvhNodeId(EncodeBvhNodeId(BvhNodeType::Box32, 0)), triangles);

	Check(result == BvhWalkResult::Ok, "nested box32 did not walk as Ok");
	Check(triangles.size() == 1, "nested box32 did not collect exactly one triangle");
	Check(triangles[0].id == 0xCCCCu, "nested box32 collected the wrong triangle");
}

void TestWalkBvhTrianglesReportsUnsupportedNodeType() {
	using namespace Libs::Graphics;

	std::vector<uint8_t>         pool(64, 0);
	std::vector<BvhTriangleNode> triangles;
	const auto result = WalkBvhTriangles(pool, BvhNodeId {BvhNodeType::Instance, 0}, triangles);

	Check(result == BvhWalkResult::UnsupportedNodeType,
	      "instance root did not report UnsupportedNodeType");
	Check(triangles.empty(), "instance root should not have collected any triangles");
}

void TestWalkBvhTrianglesReportsOutOfBounds() {
	using namespace Libs::Graphics;

	std::vector<uint8_t>         pool(64, 0);
	std::vector<BvhTriangleNode> triangles;
	const auto result = WalkBvhTriangles(pool, BvhNodeId {BvhNodeType::Box32, 1000u}, triangles);

	Check(result == BvhWalkResult::OutOfBounds, "out-of-range root did not report OutOfBounds");
}

void TestWalkBvhTrianglesReportsTooManyNodesOnACycle() {
	using namespace Libs::Graphics;

	std::vector<uint8_t> pool(256, 0);
	// A box32 node whose first child points back to itself.
	const uint32_t children[4] = {
	    EncodeBvhNodeId(BvhNodeType::Box32, 0),
	    BvhInvalidNodeId,
	    BvhInvalidNodeId,
	    BvhInvalidNodeId,
	};
	PlaceBox32Node(pool, 0, children);

	std::vector<BvhTriangleNode> triangles;
	const auto                   result =
	    WalkBvhTriangles(pool, DecodeBvhNodeId(EncodeBvhNodeId(BvhNodeType::Box32, 0)), triangles);

	Check(result == BvhWalkResult::TooManyNodes,
	      "a self-referential cycle did not report TooManyNodes");
}

} // namespace

namespace Common {

int DbgExitHandler(const char*, int, std::string_view) {
	std::abort();
}
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view) {
	std::abort();
}
int DbgExitIfHandler(const char*, const char*, int) {
	return 1;
}
void DbgExit(int) {
	std::abort();
}

} // namespace Common

int main() {
	TestParseBvhBox32NodeExtractsChildrenAndBounds();
	TestParseBvhTriangleNodeExtractsVerticesAndIds();
	TestDecodeBvhNodeIdMatchesKnownRootPointer();
	TestBvhNodeIdRoundTripsForEveryTypeAndSeveralOffsets();
	TestWalkBvhTrianglesCollectsASingleTriangleRoot();
	TestWalkBvhTrianglesSkipsInvalidChildSlots();
	TestWalkBvhTrianglesRecursesThroughNestedBox32();
	TestWalkBvhTrianglesReportsUnsupportedNodeType();
	TestWalkBvhTrianglesReportsOutOfBounds();
	TestWalkBvhTrianglesReportsTooManyNodesOnACycle();
	std::puts("BvhNodeParserTests: all cases passed");
	return 0;
}
