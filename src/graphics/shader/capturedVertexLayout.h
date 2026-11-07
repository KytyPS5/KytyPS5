#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_CAPTUREDVERTEXLAYOUT_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_CAPTUREDVERTEXLAYOUT_H_

#include <cstdint>
#include <optional>

// A host that cannot run a guest vertex-side program in place (no mesh shaders) runs it once in a
// compute pass, which stores what the program outputs in a buffer. A generated vertex shader then
// draws from that buffer. The two agree on where each value is through this type.
//
// One record per workgroup, in 32-bit words (a float is stored by its bits):
//   header     [0] vertex count  [1] primitive count  [2..3] unused
//   vertices   `vertices` times: position (4), `parameters` parameters (4 each),
//              `clip_distances` clip distances (1 each), `cull_distances` cull distances (1 each)
//   primitives `primitives` times: [0] three 10-bit vertex indices, cull flag in bit 31
//                                  [1] layer

namespace Libs::Graphics {

struct CapturedVertexLayout {
	static constexpr uint32_t kHeaderWords      = 4;
	static constexpr uint32_t kPrimitiveWords   = 2;
	static constexpr uint32_t kIndexBits        = 10;
	static constexpr uint32_t kCullFlag         = 0x80000000u;
	static constexpr uint32_t kMaxClipDistances = 8;

	uint32_t vertices       = 0; // vertices a workgroup can output
	uint32_t primitives     = 0; // primitives a workgroup can output
	uint32_t parameters     = 0; // vec4 parameters per vertex, after the position
	uint32_t clip_distances = 0;
	uint32_t cull_distances = 0;

	// A layout the record format can hold: indices fit their bits, distances fit their limit.
	[[nodiscard]] constexpr bool Valid() const {
		return vertices != 0 && primitives != 0 && vertices <= (1u << kIndexBits) &&
		       clip_distances + cull_distances <= kMaxClipDistances;
	}

	[[nodiscard]] constexpr uint32_t VertexWords() const {
		return 4u + parameters * 4u + clip_distances + cull_distances;
	}
	// Word of a value inside one record.
	[[nodiscard]] constexpr uint32_t PositionWord(uint32_t vertex) const {
		return kHeaderWords + vertex * VertexWords();
	}
	[[nodiscard]] constexpr uint32_t ParameterWord(uint32_t vertex, uint32_t parameter) const {
		return PositionWord(vertex) + 4u + parameter * 4u;
	}
	[[nodiscard]] constexpr uint32_t ClipDistanceWord(uint32_t vertex, uint32_t index) const {
		return ParameterWord(vertex, parameters) + index;
	}
	[[nodiscard]] constexpr uint32_t CullDistanceWord(uint32_t vertex, uint32_t index) const {
		return ClipDistanceWord(vertex, clip_distances) + index;
	}
	[[nodiscard]] constexpr uint32_t PrimitiveWord(uint32_t primitive) const {
		return PositionWord(vertices) + primitive * kPrimitiveWords;
	}
	[[nodiscard]] constexpr uint32_t RecordWords() const { return PrimitiveWord(primitives); }
	[[nodiscard]] constexpr uint64_t RecordBytes() const {
		return static_cast<uint64_t>(RecordWords()) * sizeof(uint32_t);
	}
	// The generated vertex shader draws one triangle per primitive of every record.
	[[nodiscard]] constexpr uint32_t VerticesPerRecord() const { return primitives * 3u; }

	bool operator==(const CapturedVertexLayout&) const = default;
};

// The most bytes one draw may capture. A draw that needs more stops with a message, before any
// guest code runs: a part of the records would change what the draw does. Crash Bandicoot 4 has an
// emulated mesh draw of 29,952 workgroups that captures 478 MiB; 256 MiB stopped the game there.
inline constexpr uint64_t kCapturedDrawByteLimit = 1ull << 30u;
// A primitive takes at least 8 bytes and is drawn with 3 vertices, so a draw under the limit has
// fewer than 2^32 vertices.
static_assert(kCapturedDrawByteLimit / 8u * 3u < (uint64_t {1} << 32u));

struct CapturedDrawSize {
	uint64_t bytes    = 0; // of all records
	uint32_t vertices = 0; // the generated vertex shader's vertex count
};

// The storage a draw with `records` records needs, or nothing when the draw is over the byte
// limit above. From a record's bytes and the vertices drawn for it, as the renderer has them.
[[nodiscard]] constexpr std::optional<CapturedDrawSize>
PlanCapturedDraw(uint64_t record_bytes, uint64_t vertices_per_record, uint64_t records) {
	if (record_bytes == 0 || vertices_per_record == 0 || records == 0) {
		return std::nullopt;
	}
	if (records > kCapturedDrawByteLimit / record_bytes) {
		return std::nullopt;
	}
	// Under the byte limit the vertex count fits 32 bits: a primitive takes 8 bytes and is drawn
	// with 3 vertices.
	return CapturedDrawSize {records * record_bytes,
	                         static_cast<uint32_t>(records * vertices_per_record)};
}

[[nodiscard]] constexpr std::optional<CapturedDrawSize>
PlanCapturedDraw(const CapturedVertexLayout& layout, uint64_t records) {
	if (!layout.Valid()) {
		return std::nullopt;
	}
	return PlanCapturedDraw(layout.RecordBytes(), layout.VerticesPerRecord(), records);
}

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADER_CAPTUREDVERTEXLAYOUT_H_ */
