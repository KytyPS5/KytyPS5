#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Libs::Audio {

struct SpatialPosition {
	float x, y, z;

	bool IsFinite() const;
};

struct SpatialAttributes {
	SpatialPosition position {};
	float           spread     = 0;
	uint32_t        ambisonics = UINT32_MAX;

	static bool ValidAmbisonics(uint32_t channel);
};

struct SpatialObject {
	SpatialAttributes    attributes;
	float                gain     = 1;
	bool                 is_float = true;
	std::vector<uint8_t> pcm;
};

// One loopback context and one SDL submission handle per object bus.
class SpatialRenderer {
public:
	static std::shared_ptr<SpatialRenderer> Open(uint32_t frames, uint32_t max_objects);
	~SpatialRenderer();
	int  OutputHandle() const { return m_handle; }
	bool Render(std::span<const SpatialObject> objects, std::vector<float>& output);

private:
	SpatialRenderer();
	struct Impl;
	std::unique_ptr<Impl> m;
	int                   m_handle = 0;
};

} // namespace Libs::Audio
