#ifndef EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_IMAGE_STORE_H_
#define EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_IMAGE_STORE_H_

#include "graphics/presentation/system/core/geometry.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

struct ImTextureData;

namespace Libs::Graphics::SystemUi {

/// An image the store holds, given when it is added and good until it is retired.
using ImageId = uint32_t;

/// Images the system UI draws, such as trophy icons and save thumbnails, decoded and handed to
/// ImGui's renderer. A texture can't be destroyed while frames that use it are still on the GPU,
/// so a released image is retired, and only forgotten once the renderer has destroyed it.
///
/// Every image, live or retiring, takes a slot in the renderer's descriptor pool, so the store
/// holds at most `capacity`. The pool is shared with the font atlas and other images, so the
/// capacity must fit in what the pool has left, and the pool must grow when the capacity does.
///
/// Use it on the present thread only. Destroy it after the renderer backend has shut down, which
/// still reads the textures, and before the ImGui context, with that context current. A member
/// of an object that shuts the backend down and destroys the context in its own destructor body
/// is destroyed too late, so hold the store where it can be reset in between.
///
/// It decodes with stb_image, whose implementation whoever links the store must compile once, with
/// STB_IMAGE_IMPLEMENTATION, so a program that already has it never gets it twice.
class ImageStore {
public:
	/// A store for at most `capacity` images, counting ones still being retired.
	explicit ImageStore(size_t capacity);
	/// Unregisters every image from ImGui. The ImGui context must still exist and be current.
	~ImageStore();
	ImageStore(const ImageStore&)            = delete;
	ImageStore& operator=(const ImageStore&) = delete;
	ImageStore(ImageStore&&)                 = delete;
	ImageStore& operator=(ImageStore&&)      = delete;

	/// Decodes a PNG or JPEG and registers it. Returns nothing, after logging why, when the data
	/// can't be decoded, either side is over `max_side` pixels, or the store is full.
	[[nodiscard]] std::optional<ImageId> Add(std::span<const std::byte> encoded, int max_side);

	/// The texture for `id`, or null once it has been released.
	[[nodiscard]] ImTextureData* Texture(ImageId id) const;

	/// The size of `id` in pixels, or zero once it has been released.
	[[nodiscard]] Vec2 Size(ImageId id) const;

	/// Stops drawing `id`. Its texture may still be used by the current frame, so the renderer
	/// destroys it after that frame, and Collect then forgets it.
	void Release(ImageId id);

	/// Forgets released images the renderer has destroyed, freeing their slots. Call once a frame,
	/// between NewFrame and Render. A forgotten texture is also taken off the list the renderer
	/// reads, so it is never read after it is freed.
	void Collect();

	/// The images in use.
	[[nodiscard]] size_t LiveCount() const { return m_live.size(); }
	/// The images released but not yet destroyed by the renderer.
	[[nodiscard]] size_t RetiringCount() const { return m_retiring.size(); }

private:
	/// One image the store holds, its id and its texture. Which list it is in says whether it is
	/// live or being retired.
	struct Entry {
		ImageId                        id;
		std::unique_ptr<ImTextureData> texture;
	};

	size_t             m_capacity;
	ImageId            m_last_id = 0;
	std::vector<Entry> m_live;
	std::vector<Entry> m_retiring;
};

} // namespace Libs::Graphics::SystemUi

#endif // EMULATOR_SRC_GRAPHICS_PRESENTATION_SYSTEM_IMGUI_IMAGE_STORE_H_
