#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_DEFERRALBLOCK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_DEFERRALBLOCK_H_

#include "common/assert.h"

namespace Libs::Graphics {

class DeferralBlock {
public:
	DeferralBlock() noexcept;
	~DeferralBlock() noexcept;
	DeferralBlock(const DeferralBlock&)            = delete;
	DeferralBlock& operator=(const DeferralBlock&) = delete;
};

[[nodiscard]] bool DeferralBlocked() noexcept;

} // namespace Libs::Graphics

#endif
