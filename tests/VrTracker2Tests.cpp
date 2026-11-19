#include "common/abi.h"
#include "graphics/presentation/vrInput.h"
#include "kernel/pthread.h"
#include "libs/hmd2.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>

namespace Libs {
void InitVrTracker2_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

// No headset runtime or kernel clock is linked here; the tests place the head and the stage.
namespace Libs::Vr {
namespace {
VrPose head_pose;
VrPose origin_in_stage;
} // namespace
VrPose OpenXrGetHeadPose() {
	return head_pose;
}
VrPose OpenXrGetOriginInStage() {
	return origin_in_stage;
}
VrControllerInput OpenXrGetController(int) {
	return {};
}
} // namespace Libs::Vr

namespace Libs::LibKernel {
uint64_t KYTY_SYSV_ABI KernelGetProcessTime() {
	return 0;
}
} // namespace Libs::LibKernel

namespace {

constexpr int ERROR_NOT_INIT                                = -1972109311; // 0x8a740001
constexpr int ERROR_ALREADY_INITIALIZED                     = -1972109310; // 0x8a740002
constexpr int ERROR_ARGUMENT_INVALID                        = -1972109306; // 0x8a740006
constexpr int ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE = -1972109279; // 0x8a740021

struct BoundaryGeometry {
	uint32_t                              size;
	uint32_t                              num_points;
	std::array<std::array<float, 3>, 256> points;
};
static_assert(sizeof(BoundaryGeometry) == 0xc08);

TEST(VrTracker2, ThreeCoordinateSystemsExistButNoBoundaryGeometryWithoutARoomScalePlayArea) {
	Loader::SymbolDatabase symbols;
	Libs::InitVrTracker2_1(&symbols);
	const auto find = [&](const char* nid) {
		return symbols.FindByNid(nid, Loader::SymbolType::Func)->vaddr;
	};
	const auto initialize = reinterpret_cast<int(KYTY_SYSV_ABI*)(const void*)>(find("6Jy73SRfG-o"));
	const auto set_coordinate_system =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(int)>(find("UVCMLmS-Eas"));
	const auto get_geometry =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(BoundaryGeometry*)>(find("SCph4ZbkqzU"));

	BoundaryGeometry geometry {sizeof(geometry)};
	EXPECT_EQ(get_geometry(&geometry), ERROR_NOT_INIT);
	EXPECT_EQ(set_coordinate_system(3), ERROR_NOT_INIT);
	std::array<uint64_t, 9> param {}; // The memory pointer is at 0x20.
	param[4] = reinterpret_cast<uint64_t>(&param);
	ASSERT_EQ(initialize(param.data()), 0);
	// Local floor, local and stage; the reserved systems start at 1000.
	EXPECT_EQ(set_coordinate_system(-1), ERROR_ARGUMENT_INVALID);
	EXPECT_EQ(set_coordinate_system(3), ERROR_ARGUMENT_INVALID);
	EXPECT_EQ(set_coordinate_system(1000), ERROR_ARGUMENT_INVALID);
	EXPECT_EQ(get_geometry(nullptr), ERROR_ARGUMENT_INVALID);
	geometry.size = 0;
	EXPECT_EQ(get_geometry(&geometry), ERROR_ARGUMENT_INVALID);

	geometry             = {sizeof(geometry), 1};
	geometry.points[255] = {1.0f, 1.0f, 1.0f};
	EXPECT_EQ(get_geometry(&geometry), ERROR_NOT_AVAILABLE_IN_CURRENT_PLAY_AREA_MODE);
	EXPECT_EQ(geometry.size, sizeof(geometry));
	EXPECT_EQ(geometry.num_points, 0u);
	EXPECT_EQ(geometry.points[255], (std::array<float, 3> {}));
}

// The tracking origin is turned 90 degrees right on the stage; the head is pitched 90 degrees up.
TEST(VrTracker2, PosesFollowTheCoordinateSystem) {
	Loader::SymbolDatabase symbols;
	Libs::InitVrTracker2_1(&symbols);
	const auto find = [&](const char* nid) {
		return symbols.FindByNid(nid, Loader::SymbolType::Func)->vaddr;
	};
	const auto initialize = reinterpret_cast<int(KYTY_SYSV_ABI*)(const void*)>(find("6Jy73SRfG-o"));
	const auto register_device =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(int, int)>(find("Dog+g25QYjw"));
	const auto get_result =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(const void*, void*)>(find("J4Vh3VVX0iU"));
	const auto set_coordinate_system =
	    reinterpret_cast<int(KYTY_SYSV_ABI*)(int)>(find("UVCMLmS-Eas"));

	std::array<uint64_t, 9> param {}; // The memory pointer is at 0x20.
	param[4]              = reinterpret_cast<uint64_t>(&param);
	const int initialized = initialize(param.data());
	ASSERT_TRUE(initialized == 0 || initialized == ERROR_ALREADY_INITIALIZED);
	ASSERT_EQ(register_device(0, 7), 0); // The headset.
	ASSERT_EQ(register_device(2, 8), 0); // The left controller.
	constexpr float s         = 0.70710678f;
	constexpr float e         = Libs::Hmd2::EYE_OFFSET_M;
	Libs::Vr::head_pose       = {true, {s, 0.0f, 0.0f, s}, {1.0f, 0.2f, 3.0f}};
	Libs::Vr::origin_in_stage = {true, {0.0f, -s, 0.0f, s}, {2.0f, 1.5f, -1.0f}};
	using Vector              = std::array<float, 3>;
	using Quaternion          = std::array<float, 4>;
	alignas(8) std::array<float, 0x1a8 / 4> result {};
	// The pose at byte offset 0x60 + 0x40 * index of the result for the handle.
	const auto pose_is = [&](int handle, int index, Vector position, Quaternion orientation) {
		std::array<uint64_t, 6> get {}; // The handle is at 4.
		get[0]            = sizeof(get) | uint64_t {static_cast<uint32_t>(handle)} << 32u;
		const float* pose = result.data() + (0x60 + 0x40 * index) / 4;
		bool         near = get_result(get.data(), result.data()) == 0;
		for (int i = 0; i < 4; i++) {
			near = near && (i == 3 || std::abs(pose[i] - position[i]) < 1e-5f) &&
			       std::abs(pose[4 + i] - orientation[i]) < 1e-5f;
		}
		return near;
	};
	// The device pose, then the eyes, each half the lens separation along the head's x axis.
	const auto head_is = [&](Vector head, Vector left, Vector right, Quaternion orientation) {
		return pose_is(7, 0, head, orientation) && pose_is(7, 1, left, orientation) &&
		       pose_is(7, 2, right, orientation);
	};
	const auto local_is = [&] {
		return head_is({1.0f, 0.2f, 3.0f}, {1.0f - e, 0.2f, 3.0f}, {1.0f + e, 0.2f, 3.0f},
		               {s, 0.0f, 0.0f, s});
	};
	const auto local_floor_is = [&] {
		return head_is({1.0f, 1.7f, 3.0f}, {1.0f - e, 1.7f, 3.0f}, {1.0f + e, 1.7f, 3.0f},
		               {s, 0.0f, 0.0f, s});
	};
	EXPECT_TRUE(local_floor_is()) << "the default is not the local floor";
	EXPECT_EQ(set_coordinate_system(1), 0);
	EXPECT_TRUE(local_is());
	EXPECT_EQ(set_coordinate_system(2), 0);
	EXPECT_TRUE(head_is({-1.0f, 1.7f, 0.0f}, {-1.0f, 1.7f, -e}, {-1.0f, 1.7f, e},
	                    {0.5f, -0.5f, 0.5f, 0.5f}));
	EXPECT_TRUE(pose_is(8, 0, {2.0f, 1.5f, -1.0f}, {0.0f, -s, 0.0f, s}));
	Libs::Vr::origin_in_stage.valid = false;
	EXPECT_TRUE(local_is()) << "a missing stage moved the poses";
	EXPECT_EQ(set_coordinate_system(0), 0);
	EXPECT_TRUE(local_is()) << "a missing stage moved the local floor";
	Libs::Vr::origin_in_stage.valid = true;
	EXPECT_TRUE(local_floor_is());
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
