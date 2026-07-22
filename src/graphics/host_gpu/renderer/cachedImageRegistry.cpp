#include "graphics/host_gpu/renderer/cachedImageRegistry.h"

#include "common/assert.h"

#include <algorithm>

namespace Libs::Graphics {

void CachedImageRegistry::Add(Owner owner) {
	if (owner == nullptr || owner->image == nullptr || owner->registered ||
	    m_records_by_image.contains(owner->image)) {
		EXIT("TextureCache: invalid or duplicate image registration\n");
	}
	std::vector<OwnerIndex::ByteRange> ranges;
	ranges.reserve(owner->RangeCount());
	for (uint32_t range = 0; range < owner->RangeCount(); range++) {
		ranges.push_back({owner->Address(range), owner->Size(range)});
	}

	m_owners.push_back(std::move(owner));
	auto& registered = m_owners.back();
	m_records_by_image.emplace(registered->image, registered);
	if (!m_owner_index.Register(registered.get(), ranges)) {
		EXIT("TextureCache: invalid or duplicate image range registration\n");
	}
	registered->registered = true;
}

CachedImageRegistry::Removal CachedImageRegistry::Remove(CachedImageRecord& record) {
	if (!record.registered) {
		EXIT("TextureCache: unregistering an unregistered image\n");
	}
	const auto handle = m_records_by_image.find(record.image);
	if (handle == m_records_by_image.end() || handle->second.lock().get() != &record) {
		EXIT("TextureCache: image missing from handle index\n");
	}
	const auto owner = std::find_if(m_owners.begin(), m_owners.end(),
	                                [&](const auto& item) { return item.get() == &record; });
	if (owner == m_owners.end()) {
		EXIT("TextureCache: indexed image is missing its strong owner\n");
	}

	std::vector<OwnerIndex::ByteRange> final_page_releases;
	if (!m_owner_index.Unregister(&record, final_page_releases)) {
		EXIT("TextureCache: image missing from owner index\n");
	}
	Removal removal;
	removal.final_page_releases.reserve(final_page_releases.size());
	for (const auto& range: final_page_releases) {
		removal.final_page_releases.push_back({range.address, range.size});
	}
	m_records_by_image.erase(handle);
	record.registered = false;
	removal.owner     = std::move(*owner);
	m_owners.erase(owner);
	return removal;
}

void CachedImageRegistry::ReserveAdditional(size_t count) {
	if (count > m_owners.max_size() - m_owners.size()) {
		EXIT("TextureCache: cached image registry capacity overflow\n");
	}
	m_owners.reserve(m_owners.size() + count);
}

CachedImageRegistry::Owner CachedImageRegistry::Find(const CachedImageRecord& record) const {
	const auto handle = m_records_by_image.find(record.image);
	const auto owner = std::find_if(m_owners.begin(), m_owners.end(),
	                                [&](const auto& item) { return item.get() == &record; });
	if (record.image == nullptr || !record.registered || handle == m_records_by_image.end() ||
	    handle->second.lock().get() != &record || owner == m_owners.end()) {
		EXIT("TextureCache: cached image owner is missing\n");
	}
	return *owner;
}

CachedImageRegistry::Owner CachedImageRegistry::Find(VulkanImage& image) const {
	const auto record = m_records_by_image.find(&image);
	if (record == m_records_by_image.end()) {
		EXIT("TextureCache: Vulkan image has no cached owner, image=%p\n",
		     static_cast<void*>(&image));
	}
	auto owner = record->second.lock();
	if (owner == nullptr || owner->image != &image || !owner->registered || !Contains(*owner)) {
		EXIT("TextureCache: Vulkan image owner expired while registered, image=%p\n",
		     static_cast<void*>(&image));
	}
	return owner;
}

std::vector<CachedImageRecord*> CachedImageRegistry::Query(uint64_t address, uint64_t size,
                                                          QueryMode mode) const {
	return mode == QueryMode::PageCandidates ? m_owner_index.QueryCandidates(address, size)
	                                         : m_owner_index.Query(address, size);
}

bool CachedImageRegistry::Contains(const CachedImageRecord& record) const {
	return std::any_of(m_owners.begin(), m_owners.end(),
	                   [&](const auto& owner) { return owner.get() == &record; });
}

} // namespace Libs::Graphics
