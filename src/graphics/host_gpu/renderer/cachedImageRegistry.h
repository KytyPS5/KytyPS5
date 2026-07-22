#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDIMAGEREGISTRY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDIMAGEREGISTRY_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/cachedImageRecord.h"
#include "graphics/host_gpu/renderer/multiLevelPageTable.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics {

class CachedImageRegistry final {
public:
	using Owner = std::shared_ptr<CachedImageRecord>;
	using Owners = std::vector<Owner>;
	enum class QueryMode : uint8_t { Bytes, PageCandidates };
	struct Removal {
		Owner                   owner;
		std::vector<GuestRange> final_page_releases;
	};

	CachedImageRegistry() = default;
	~CachedImageRegistry() = default;
	KYTY_CLASS_NO_COPY(CachedImageRegistry);

	void Add(Owner owner);
	[[nodiscard]] Removal Remove(CachedImageRecord& record);
	void                  ReserveAdditional(size_t count);

	[[nodiscard]] Owner Find(const CachedImageRecord& record) const;
	[[nodiscard]] Owner Find(VulkanImage& image) const;
	[[nodiscard]] std::vector<CachedImageRecord*> Query(uint64_t address, uint64_t size,
	                                                    QueryMode mode) const;
	[[nodiscard]] bool         Contains(const CachedImageRecord& record) const;
	[[nodiscard]] bool         Empty() const { return m_owners.empty(); }
	[[nodiscard]] size_t       Size() const { return m_owners.size(); }
	[[nodiscard]] const Owners& All() const { return m_owners; }

private:
	using OwnerIndex = MultiRangePageOwnerIndex<CachedImageRecord*>;

	Owners                                                    m_owners;
	OwnerIndex                                                m_owner_index;
	std::unordered_map<VulkanImage*, std::weak_ptr<CachedImageRecord>> m_records_by_image;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHEDIMAGEREGISTRY_H_
