#include "graphics/presentation/system/imgui/imageStore.h"

#include "common/logging/log.h"
#include "imgui.h"
// RegisterUserTexture, which hands a texture the UI owns to the renderer, is only declared here.
#include "imgui_internal.h"
// Only stb's declarations, as whoever links the store compiles its implementation.
#include "stb_image.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace Libs::Graphics::SystemUi {

namespace {

constexpr int RGBA_CHANNELS = 4;
// The largest side an image may have, whatever the caller asks for, so its byte count fits the
// int ImGui sizes textures with.
constexpr int MAX_SIDE = 4096;

// Unregisters `texture` and takes it off the renderer's texture list. ImGui copies its
// registered textures into that list when it renders, and the renderer reads the list later in
// the frame, so a freed texture left on it would be read after it is freed.
void Forget(ImTextureData* texture) {
	ImGui::UnregisterUserTexture(texture);
	ImGui::GetPlatformIO().Textures.find_erase(texture);
}

// Decodes `encoded` to RGBA pixels in a new texture. Returns null, after logging why, when it
// isn't an image stb can read or either side is over `max_side` pixels.
std::unique_ptr<ImTextureData> Decode(std::span<const std::byte> encoded, int max_side) {
	if (encoded.empty() || encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
		LOGF("[SystemUi] image data is empty or too large\n");
		return nullptr;
	}
	const auto* data   = reinterpret_cast<const stbi_uc*>(encoded.data());
	const int   size   = static_cast<int>(encoded.size());
	const int   limit  = std::min(max_side, MAX_SIDE);
	int         width  = 0;
	int         height = 0;
	if (stbi_info_from_memory(data, size, &width, &height, nullptr) == 0 || width <= 0 ||
	    height <= 0 || width > limit || height > limit) {
		LOGF("[SystemUi] image is unreadable or larger than %dx%d\n", limit, limit);
		return nullptr;
	}
	stbi_uc* pixels = stbi_load_from_memory(data, size, &width, &height, nullptr, RGBA_CHANNELS);
	if (pixels == nullptr) {
		LOGF("[SystemUi] image could not be decoded: %s\n", stbi_failure_reason());
		return nullptr;
	}
	auto texture = std::make_unique<ImTextureData>();
	texture->Create(ImTextureFormat_RGBA32, width, height);
	std::memcpy(texture->Pixels, pixels,
	            static_cast<size_t>(width) * static_cast<size_t>(height) * RGBA_CHANNELS);
	texture->UseColors = true;
	stbi_image_free(pixels);
	return texture;
}

// The entry for `id` in `entries`, or the end.
template <class Entries>
auto Find(Entries& entries, ImageId id) {
	return std::find_if(entries.begin(), entries.end(),
	                    [id](const auto& entry) { return entry.id == id; });
}

} // namespace

ImageStore::ImageStore(size_t capacity): m_capacity(capacity) {}

ImageStore::~ImageStore() {
	IM_ASSERT(ImGui::GetCurrentContext() != nullptr);
	for (auto* entries: {&m_live, &m_retiring}) {
		for (const auto& entry: *entries) {
			Forget(entry.texture.get());
		}
	}
}

std::optional<ImageId> ImageStore::Add(std::span<const std::byte> encoded, int max_side) {
	if (m_live.size() + m_retiring.size() >= m_capacity) {
		LOGF("[SystemUi] image store is full at %zu images\n", m_capacity);
		return std::nullopt;
	}
	auto texture = Decode(encoded, max_side);
	if (texture == nullptr) {
		return std::nullopt;
	}
	ImGui::RegisterUserTexture(texture.get());
	m_live.push_back({++m_last_id, std::move(texture)});
	return m_last_id;
}

ImTextureData* ImageStore::Texture(ImageId id) const {
	const auto entry = Find(m_live, id);
	return entry == m_live.end() ? nullptr : entry->texture.get();
}

Vec2 ImageStore::Size(ImageId id) const {
	const auto* texture = Texture(id);
	return texture == nullptr
	           ? Vec2 {}
	           : Vec2 {static_cast<float>(texture->Width), static_cast<float>(texture->Height)};
}

void ImageStore::Release(ImageId id) {
	const auto entry = Find(m_live, id);
	if (entry == m_live.end()) {
		return;
	}
	entry->texture->WantDestroyNextFrame = true;
	m_retiring.push_back(std::move(*entry));
	m_live.erase(entry);
}

void ImageStore::Collect() {
	std::erase_if(m_retiring, [](const Entry& entry) {
		if (entry.texture->Status != ImTextureStatus_Destroyed) {
			return false;
		}
		Forget(entry.texture.get());
		return true;
	});
}

} // namespace Libs::Graphics::SystemUi
