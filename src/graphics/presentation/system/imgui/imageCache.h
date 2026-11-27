#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_IMAGE_CACHE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_IMAGE_CACHE_H_

#include "graphics/presentation/system/core/encodedImage.h"
#include "graphics/presentation/system/core/geometry.h"
#include "graphics/presentation/system/imgui/imageStore.h"

#include <cstddef>
#include <optional>
#include <vector>

struct ImTextureData;

namespace Libs::Graphics::SystemUi {

/// How many images a cache holds at once by default, counting ones still being retired. Whoever
/// sizes the renderer's descriptor pool sizes it for the capacity its cache is made with.
inline constexpr size_t DEFAULT_IMAGE_CACHE_CAPACITY = 6;

/// The largest side an image may have by default, in pixels. Larger images are refused, which
/// bounds the memory one image can take.
inline constexpr int DEFAULT_MAX_IMAGE_SIDE = 512;

/// An image as drawn, or no texture when there is none or it couldn't be decoded.
struct DecodedImage {
	ImTextureData* texture = nullptr;
	Vec2           size;
};

/// Encoded images decoded for the elements that show them. An Image element given a source asks
/// the cache open around it for the texture, so whatever builds the element needs only the bytes.
/// Each image is decoded the first frame an element shows it and kept while elements go on showing
/// it, and released after the first frame none does, so an image no longer drawn holds no texture.
/// An image that can't be decoded is remembered as missing while it is shown, so it isn't tried
/// every frame. One that finds the store full gets no texture that frame, and is tried again in a
/// later one, once a retired image has freed its room.
///
/// It holds an ImageStore, so it has the store's rules. Use it on the present thread only, and
/// destroy it after the renderer backend shuts down and before the ImGui context, so hold it where
/// it can be reset in between. Each image takes a slot in the renderer's descriptor pool, so the
/// pool must have room for its capacity.
class ImageCache {
public:
	/// A cache of at most `capacity` images at once, each at most `max_side` pixels on a side.
	explicit ImageCache(size_t capacity = DEFAULT_IMAGE_CACHE_CAPACITY,
	                    int    max_side = DEFAULT_MAX_IMAGE_SIDE);
	~ImageCache()                            = default;
	ImageCache(const ImageCache&)            = delete;
	ImageCache& operator=(const ImageCache&) = delete;
	ImageCache(ImageCache&&)                 = delete;
	ImageCache& operator=(ImageCache&&)      = delete;

	/// Starts a frame. Releases the images no element showed in the last frame, then forgets
	/// those the renderer has destroyed. Call once a frame, between NewFrame and Render, before
	/// anything is built.
	void BeginFrame();

	/// The decoded `source`, decoding it now if this is the first time it is shown, and marks it
	/// shown this frame. No texture for a null source, one that can't be decoded, or a full store.
	[[nodiscard]] DecodedImage Use(const EncodedImage& source);

	/// Whether any image is held or still being retired, so frames must go on being drawn until
	/// the renderer has destroyed the last one.
	[[nodiscard]] bool NeedsFrames() const;

private:
	/// One image, or a remembered failure when it has none. It holds its source, so the bytes'
	/// address can't be reused by other bytes while the entry stands.
	struct Entry {
		EncodedImage           source;
		std::optional<ImageId> image;
		bool                   used = false;
	};

	ImageStore         m_store;
	size_t             m_capacity;
	int                m_max_side;
	std::vector<Entry> m_entries;
};

/// Makes `cache` the one Image elements decode from on this thread for as long as the scope lives,
/// and puts back the one it replaced when it ends. Scopes nest and end in reverse order, as local
/// variables do. `cache` must outlive the scope.
class ImageCacheScope {
public:
	/// Makes `cache` current on this thread until the scope ends.
	[[nodiscard]] explicit ImageCacheScope(ImageCache& cache);
	/// Makes no cache current on this thread until the scope ends, so images built inside draw
	/// nothing whatever an outer scope has open.
	[[nodiscard]] explicit ImageCacheScope(std::nullptr_t);
	/// Puts back the cache that was current before this scope.
	~ImageCacheScope();
	ImageCacheScope(const ImageCacheScope&)            = delete;
	ImageCacheScope& operator=(const ImageCacheScope&) = delete;
	ImageCacheScope(ImageCacheScope&&)                 = delete;
	ImageCacheScope& operator=(ImageCacheScope&&)      = delete;

private:
	// The cache that was current before this scope, put back when it ends.
	ImageCache* m_previous;
	// The scope that was open before this one. Ending is checked against the scope rather than the
	// cache, so two scopes over one cache, or over none, still can't end out of order unnoticed.
	const ImageCacheScope* m_previous_scope;
};

/// The cache of the innermost open ImageCacheScope on this thread, or null when none is open or
/// the innermost was opened with no cache, and then images given a source draw nothing.
[[nodiscard]] ImageCache* CurrentImageCache();

/// Whether `source` can be shown, decoding it in the current cache if it hasn't been. False for a
/// null source, one that can't be decoded, a full store, or no cache open, so a caller can show
/// something else in its place.
[[nodiscard]] bool ImageReady(const EncodedImage& source);

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_IMAGE_CACHE_H_
