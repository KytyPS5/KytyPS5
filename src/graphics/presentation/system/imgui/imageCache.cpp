#include "graphics/presentation/system/imgui/imageCache.h"

#include <algorithm>
#include <cassert>
#include <iterator>

namespace Libs::Graphics::SystemUi {

namespace {

// The cache Image elements decode from on this thread, set by ImageCacheScope.
thread_local ImageCache* t_current_cache = nullptr;
// The innermost open scope on this thread, so each scope can check it is the one ending.
thread_local const ImageCacheScope* t_current_scope = nullptr;

} // namespace

ImageCache::ImageCache(size_t capacity, int max_side)
    : m_store(capacity), m_capacity(capacity), m_max_side(max_side) {}

void ImageCache::BeginFrame() {
	// An image released here is only given to the renderer to destroy after this frame, so the
	// collect below frees the room of those released in earlier frames.
	std::erase_if(m_entries, [this](const Entry& entry) {
		if (!entry.used && entry.image.has_value()) {
			m_store.Release(*entry.image);
		}
		return !entry.used;
	});
	for (Entry& entry: m_entries) {
		entry.used = false;
	}
	m_store.Collect();
}

DecodedImage ImageCache::Use(const EncodedImage& source) {
	if (source == nullptr) {
		return {};
	}
	auto entry = std::find_if(m_entries.begin(), m_entries.end(), [&source](const Entry& held) {
		return held.source.get() == source.get();
	});
	if (entry == m_entries.end()) {
		// A full store is not a failure, so nothing is remembered and a later frame tries again.
		if (m_store.LiveCount() + m_store.RetiringCount() >= m_capacity) {
			return {};
		}
		m_entries.push_back({source, m_store.Add(*source, m_max_side)});
		entry = std::prev(m_entries.end());
	}
	entry->used = true;
	if (!entry->image.has_value()) {
		return {};
	}
	return {m_store.Texture(*entry->image), m_store.Size(*entry->image)};
}

bool ImageCache::NeedsFrames() const {
	return m_store.LiveCount() > 0 || m_store.RetiringCount() > 0;
}

ImageCacheScope::ImageCacheScope(ImageCache& cache)
    : m_previous(t_current_cache), m_previous_scope(t_current_scope) {
	t_current_cache = &cache;
	t_current_scope = this;
}

ImageCacheScope::ImageCacheScope(std::nullptr_t)
    : m_previous(t_current_cache), m_previous_scope(t_current_scope) {
	t_current_cache = nullptr;
	t_current_scope = this;
}

ImageCacheScope::~ImageCacheScope() {
	assert(t_current_scope == this && "ImageCacheScopes must end in reverse order");
	t_current_cache = m_previous;
	t_current_scope = m_previous_scope;
}

ImageCache* CurrentImageCache() {
	return t_current_cache;
}

bool ImageReady(const EncodedImage& source) {
	ImageCache* const cache = CurrentImageCache();
	return cache != nullptr && cache->Use(source).texture != nullptr;
}

} // namespace Libs::Graphics::SystemUi
