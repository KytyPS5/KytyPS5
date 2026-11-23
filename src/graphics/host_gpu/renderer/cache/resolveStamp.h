#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_RESOLVESTAMP_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_RESOLVESTAMP_H_

#include <cstdint>

namespace Libs::Graphics {

// PS5: what a FindImage result depends on; ResolveCache::Reuse checks it. A zero epoch is never
// current.
struct ResolveStamp {
	uint64_t epoch  = 0;
	uint64_t window = 0;
	uint64_t tick   = 0;
	// FindImage read color clear keys, which the guest may rewrite outside a draw window or between
	// submissions: the result holds only in its draw window and scheduler tick.
	bool clear_keys = false;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_RESOLVESTAMP_H_
