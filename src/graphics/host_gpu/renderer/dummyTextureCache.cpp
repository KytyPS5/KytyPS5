#include "graphics/host_gpu/renderer/dummyTextureCache.h"

#include "common/assert.h"
#include "graphics/host_gpu/renderer/image.h"
#include "graphics/host_gpu/transfer.h"

namespace Libs::Graphics {

namespace {

[[nodiscard]] constexpr size_t DummyTextureIndex(bool uint_format, bool image_3d) noexcept {
	return (image_3d ? 2u : 0u) + (uint_format ? 1u : 0u);
}

} // namespace

DummyTextureCache::~DummyTextureCache() {
	Common::LockGuard lock(m_mutex);
	if (m_ctx == nullptr) {
		return;
	}

	Transfer::WaitForGraphicsIdle(m_ctx);
	const auto destroy = [this](auto& slots) {
		for (auto& slot: slots) {
			if (slot.image != nullptr) {
				ImageOps::Destroy(m_ctx, slot.image);
				slot.image = nullptr;
			}
		}
	};
	destroy(m_sampled);
	destroy(m_storage);
}

VulkanImage* DummyTextureCache::Get(GraphicContext* ctx, Usage usage, bool uint_format,
                                    bool image_3d) {
	Common::LockGuard lock(m_mutex);
	if (ctx == nullptr) {
		EXIT("TextureCache: dummy texture requires a graphics context\n");
	}
	if (m_ctx != nullptr && m_ctx != ctx) {
		EXIT("TextureCache: dummy texture context changed, previous=%p current=%p usage=%u\n",
		     static_cast<const void*>(m_ctx), static_cast<const void*>(ctx),
		     static_cast<uint32_t>(usage));
	}
	m_ctx = ctx;

	auto& slots = usage == Usage::Storage ? m_storage : m_sampled;
	auto& slot  = slots[DummyTextureIndex(uint_format, image_3d)];
	if (slot.image == nullptr) {
		slot.image =
		    ImageOps::CreateDummyTexture(ctx, uint_format, image_3d, usage == Usage::Storage);
	}
	return slot.image;
}

} // namespace Libs::Graphics
