#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_BUFFERCHUNK_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_BUFFERCHUNK_H_

#include "graphics/host_gpu/regionDefinitions.h"

#include <algorithm>
#include <cstdint>

namespace Libs::Graphics {

// The guest range that a new buffer of the buffer cache covers.
//
// Every buffer of the cache can be reached through device addresses, and MoltenVK attaches each
// such buffer to every command encoder: the cost of a frame grows with the number of buffers. A
// buffer that covers only the bytes of its first use makes many small buffers. In Crash Bandicoot
// 4 the cache held about 6,500 buffers of about 100 KB after five minutes, and on a player's route
// the game fell from 43 to 7 fps.
//
// So a new buffer covers the whole chunk of guest memory around its first use, cut at the ends of
// the mapping that holds it. A buffer is never freed because it is idle (see cacheCollection.h),
// so fewer, larger buffers are what bound the count.
inline constexpr uint64_t BUFFER_CHUNK_SIZE = uint64_t {2} * 1024 * 1024;

// The range a new buffer covers, for a first use `request` inside the guest mapping `mapped`, in
// chunks of `chunk_size` bytes. The result always holds the request, also where the request leaves
// the mapping.
[[nodiscard]] constexpr GuestSpan ChunkRange(GuestSpan request, GuestSpan mapped,
                                             uint64_t chunk_size = BUFFER_CHUNK_SIZE) noexcept {
	const auto chunk_begin = request.begin / chunk_size * chunk_size;
	const auto chunk_end   = (request.end + chunk_size - 1) / chunk_size * chunk_size;
	const bool inside      = mapped.begin <= request.begin && request.begin < mapped.end;
	if (!inside) {
		return request;
	}
	return {std::min(request.begin, std::max(chunk_begin, mapped.begin)),
	        std::max(request.end, std::min(chunk_end, mapped.end))};
}

} // namespace Libs::Graphics

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_BUFFERCHUNK_H_ */
