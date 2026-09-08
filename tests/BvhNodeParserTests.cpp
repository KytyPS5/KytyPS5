#include "graphics/rt/bvhNodeParser.h"

#include "common/assert.h"

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

Check(node.vertices[0][0] == 1.0f && node.vertices[0][1] == 2.0f &&
          node.vertices[0][2] == 3.0f,
      "triangle node first vertex did not match the raw bytes");
Check(node.vertices[2][0] == 7.0f && node.vertices[2][1] == 8.0f &&
          node.vertices[2][2] == 9.0f,
      "triangle node third vertex did not match the raw bytes (offset math wrong?)");
Check(node.triangle_id == 0x1234u, "triangle node triangle_id did not match the raw bytes");
Check(node.geometry_id_and_flags == 0x5678u,
      "triangle node geometry_id_and_flags did not match the raw bytes");
Check(node.id == 0x9abcu,
      "triangle node id did not match the raw bytes (reserved2 skip wrong?)");
}

} // namespace

namespace Common {

int DbgExitHandler(const char*, int, std::string_view) { std::abort(); }
int DbgExitHandler(const char*, int, fmt::text_style, std::string_view) { std::abort(); }
int DbgExitIfHandler(const char*, const char*, int) { return 1; }
void DbgExit(int) { std::abort(); }

} // namespace Common

int main() {
TestParseBvhBox32NodeExtractsChildrenAndBounds();
TestParseBvhTriangleNodeExtractsVerticesAndIds();
std::puts("BvhNodeParserTests: all cases passed");
return 0;
}
