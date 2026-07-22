#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEALIASPLAN_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEALIASPLAN_H_

#include "common/abi.h"
#include "common/common.h"
#include "graphics/host_gpu/renderer/resourceRange.h"

#include <memory>
#include <vector>

namespace Libs::Graphics {

class CachedImageRecord;

class ImageAliasPlan final {
public:
	enum class Action : uint8_t {
		RetireGuestCurrent,
		MaterializeGuest,
		PreserveNative,
		ReleaseGpuOwnership
	};
	enum class PageIsolation : uint8_t { Required, SharedPagesAllowed };
	struct Entry {
		std::shared_ptr<CachedImageRecord> image;
		Action                             action;
	};

	ImageAliasPlan(const char* operation, GuestRange request, PageIsolation isolation);

	void Add(const std::shared_ptr<CachedImageRecord>& image, Action action);
	void PreserveMetadata(uint64_t address);

	[[nodiscard]] bool Empty() const { return m_entries.empty(); }
	[[nodiscard]] size_t RetirementCount() const { return m_entries.size(); }
	[[nodiscard]] bool Contains(const CachedImageRecord& image) const;
	[[nodiscard]] bool HasAction(Action action) const;
	[[nodiscard]] std::shared_ptr<CachedImageRecord> NativeOwner() const;
	[[nodiscard]] CachedImageRecord* NativeSource() const { return NativeOwner().get(); }
	[[nodiscard]] const std::vector<Entry>& Entries() const { return m_entries; }
	[[nodiscard]] const char* Operation() const { return m_operation; }
	[[nodiscard]] GuestRange Request() const { return m_request; }
	[[nodiscard]] PageIsolation Isolation() const { return m_isolation; }
	[[nodiscard]] uint64_t PreservedMetadataAddress() const {
		return m_preserved_metadata_address;
	}

private:
	const char* const          m_operation;
	const GuestRange           m_request;
	const PageIsolation        m_isolation;
	uint64_t                   m_preserved_metadata_address = 0;
	std::vector<Entry>         m_entries;
};

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_IMAGEALIASPLAN_H_
