#include "gpu_blit_shaders/gpu_mesh_indirect_spv.h"

#include <gtest/gtest.h>
#include <spirv/unified1/spirv.hpp>

#include <cstdint>
#include <iterator>

namespace {

TEST(GpuBlitShaders, MeshIndirectTaskDeclaresLocalSize) {
	bool local_size    = false;
	bool local_size_id = false;
	for (size_t i = 5; i < std::size(GPU_MESH_INDIRECT_SPV);) {
		const uint32_t word  = GPU_MESH_INDIRECT_SPV[i];
		const uint32_t count = word >> spv::WordCountShift;
		ASSERT_NE(count, 0u);
		ASSERT_LE(i + count, std::size(GPU_MESH_INDIRECT_SPV));
		const auto op = static_cast<spv::Op>(word & spv::OpCodeMask);
		if (count > 2 && (op == spv::OpExecutionMode || op == spv::OpExecutionModeId)) {
			const auto mode = static_cast<spv::ExecutionMode>(GPU_MESH_INDIRECT_SPV[i + 2]);
			local_size |= mode == spv::ExecutionModeLocalSize;
			local_size_id |= mode == spv::ExecutionModeLocalSizeId;
		}
		i += count;
	}
	EXPECT_TRUE(local_size);
	EXPECT_FALSE(local_size_id);
}

} // namespace

int main(int argc, char** argv) {
	testing::InitGoogleTest(&argc, argv);
	return RUN_ALL_TESTS();
}
