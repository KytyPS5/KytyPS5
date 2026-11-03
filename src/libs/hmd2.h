#ifndef EMULATOR_SRC_LIBS_HMD2_H_
#define EMULATOR_SRC_LIBS_HMD2_H_

#include <atomic>
#include <cstdint>

namespace Libs::Hmd2 {

inline constexpr uint32_t PANEL_WIDTH  = 4000;
inline constexpr uint32_t PANEL_HEIGHT = 2040;
inline constexpr uint64_t OUTPUT_MODE_89_91HZ  = 0x2000c;
inline constexpr uint64_t OUTPUT_MODE_119_88HZ = 0x2000f;
inline constexpr int32_t ERROR_REPROJECTION_NOT_INITIALIZED = -1972240373; // 0x8a72000b

// Virtual ER15 field of view per eye: 55/42.5/51.2/51.2 degrees. "Out" is the temporal side and
// "in" the nasal side. These tangents describe that geometry, not bit-exact firmware output.
inline constexpr float FOV_TAN_OUT    = 1.4281480312f;
inline constexpr float FOV_TAN_IN     = 0.9163311720f;
inline constexpr float FOV_TAN_TOP    = 1.2437491417f;
inline constexpr float FOV_TAN_BOTTOM = 1.2437491417f;

// Each eye sits half the lens separation to the side of the head.
inline constexpr uint8_t LENS_SEPARATION_MM = 63;
inline constexpr float   EYE_OFFSET_M       = LENS_SEPARATION_MM / 2000.0f;

struct ReprojectionState {
	std::atomic<uint64_t> output_mode {0};
	std::atomic<uint32_t> timing_us {3000};
};

[[nodiscard]] ReprojectionState* GetReprojectionState();

struct SceFVector3 {
	float x;
	float y;
	float z;
};

// The headset video-out port registers SceHmd2ReprojectionBuffer objects. The eye texture of their
// first layer is the surface the game flips; both eyes sit side by side in it.
struct ReprojectionSurface {
	uint64_t address;
	uint32_t width;
	uint32_t height;
	uint64_t pixel_format; // SceVideoOutPixelFormat
};

[[nodiscard]] bool GetReprojectionSurface(const void*          reprojection_buffer,
                                          ReprojectionSurface& out);

} // namespace Libs::Hmd2

#endif // EMULATOR_SRC_LIBS_HMD2_H_
