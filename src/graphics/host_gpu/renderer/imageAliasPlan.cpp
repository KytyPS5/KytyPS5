#include "graphics/host_gpu/renderer/imageAliasPlan.h"

#include "common/assert.h"
#include "graphics/host_gpu/renderer/cachedImageRecord.h"

#include <algorithm>

namespace Libs::Graphics {

ImageAliasPlan::ImageAliasPlan(const char* operation, GuestRange request,
                               PageIsolation isolation)
    : m_operation(operation), m_request(request), m_isolation(isolation) {}

void ImageAliasPlan::Add(const std::shared_ptr<CachedImageRecord>& image, Action action) {
	if (image == nullptr) {
		EXIT("TextureCache: alias plan has an empty image owner\n");
	}
	if (Contains(*image)) {
		EXIT("TextureCache: alias plan contains a duplicate retirement\n");
	}
	if (action == Action::PreserveNative && NativeSource() != nullptr) {
		EXIT("TextureCache: alias plan contains multiple native sources\n");
	}
	m_entries.push_back({image, action});
}

void ImageAliasPlan::PreserveMetadata(uint64_t address) {
	if (address != 0 && m_preserved_metadata_address != 0 &&
	    m_preserved_metadata_address != address) {
		EXIT("TextureCache: alias plan preserves multiple metadata allocations\n");
	}
	m_preserved_metadata_address = address;
}

bool ImageAliasPlan::Contains(const CachedImageRecord& image) const {
	return std::any_of(m_entries.begin(), m_entries.end(),
	                   [&](const auto& entry) { return entry.image.get() == &image; });
}

bool ImageAliasPlan::HasAction(Action action) const {
	return std::any_of(m_entries.begin(), m_entries.end(),
	                   [action](const auto& entry) { return entry.action == action; });
}

std::shared_ptr<CachedImageRecord> ImageAliasPlan::NativeOwner() const {
	const auto entry = std::find_if(m_entries.begin(), m_entries.end(), [](const auto& item) {
		return item.action == Action::PreserveNative;
	});
	return entry != m_entries.end() ? entry->image : nullptr;
}

} // namespace Libs::Graphics
