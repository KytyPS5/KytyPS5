#include "graphics/host_gpu/deferralBlock.h"

#include <cstdint>

namespace Libs::Graphics {

namespace {
thread_local uint32_t g_deferral_block_depth = 0;
} // namespace

DeferralBlock::DeferralBlock() noexcept {
	g_deferral_block_depth++;
}

DeferralBlock::~DeferralBlock() noexcept {
	EXIT_IF(g_deferral_block_depth == 0);
	g_deferral_block_depth--;
}

bool DeferralBlocked() noexcept {
	return g_deferral_block_depth != 0;
}

} // namespace Libs::Graphics
