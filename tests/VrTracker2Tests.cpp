#include "common/abi.h"
#include "graphics/presentation/vrInput.h"
#include "kernel/pthread.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>

namespace Libs {
void InitVrTracker2_1(Loader::SymbolDatabase* symbols);
} // namespace Libs

// No headset runtime or kernel clock is linked here.
namespace Libs::Vr {
VrPose OpenXrGetHeadPose() {
	return {};
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
	EXPECT_EQ(set_coordinate_system(0), 0);
	EXPECT_EQ(set_coordinate_system(2), 0);
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

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
