#include "graphics/host_gpu/renderer/commandRecorder.h"

#include <algorithm>
#include <cstdlib>
#include <immintrin.h>

namespace Libs::Graphics {

CommandStream::Chunk::Chunk(size_t capacity): capacity(capacity), arrays(capacity) {
	data = static_cast<std::byte*>(::operator new(capacity, std::align_val_t {Alignment}));
}

CommandStream::Chunk::~Chunk() {
	::operator delete(data, std::align_val_t {Alignment});
}

void CommandStream::NextChunk(size_t bytes) {
	if (m_current != nullptr && m_output != nullptr) {
		m_output->Push(std::move(m_chunks.back()));
		m_chunks.clear();
	}
	std::unique_ptr<Chunk> chunk;
	if (bytes <= ChunkCapacity) {
		if (m_output != nullptr) {
			chunk = m_output->TakeRecycled();
		} else if (!m_free.empty()) {
			chunk = std::move(m_free.back());
			m_free.pop_back();
		}
	}
	if (chunk == nullptr) {
		chunk = std::make_unique<Chunk>(std::max(ChunkCapacity, RoundUp(bytes)));
	}
	chunk->used   = 0;
	chunk->arrays = chunk->capacity;
	m_current     = chunk.get();
	m_chunks.push_back(std::move(chunk));
}

void CommandStream::Publish() {
	EXIT_IF(m_output == nullptr);
	if (m_current != nullptr) {
		// The recording thread submits what this chunk records as soon as it sees the chunk. The
		// stream and staging buffers this thread wrote itself are write-combined (large copies,
		// uploads): a store to ordinary memory may pass them, so drain them before the chunk is
		// visible (the queue's fence only comes after its store).
		_mm_sfence();
		m_output->Push(std::move(m_chunks.back()));
		m_chunks.clear();
		m_current = nullptr;
	}
}

std::vector<std::unique_ptr<CommandStream::Chunk>> CommandStream::TakeChunks() {
	EXIT_IF(m_output != nullptr);
	std::vector<std::unique_ptr<Chunk>> chunks;
	chunks.swap(m_chunks);
	m_current = nullptr;
	return chunks;
}

void CommandStream::Recycle(std::unique_ptr<Chunk> chunk) {
	// Oversized chunks hold one large command; do not keep them around.
	if (chunk->capacity == ChunkCapacity) {
		m_free.push_back(std::move(chunk));
	}
}

void CommandChunkQueue::Recycle(ChunkPtr chunk) {
	// Oversized chunks hold one large command; do not keep them around, nor more than the ring
	// holds.
	if (chunk->capacity != CommandStream::ChunkCapacity) {
		return;
	}
	(void)m_recycled.TryPush(chunk);
}

void CommandRecorder::pipelineBarrier(vk::PipelineStageFlags src_stages,
                                      vk::PipelineStageFlags dst_stages,
                                      vk::DependencyFlags dependency, uint32_t memory_count,
                                      const vk::MemoryBarrier* memory, uint32_t buffer_count,
                                      const vk::BufferMemoryBarrier* buffers,
                                      uint32_t image_count, const vk::ImageMemoryBarrier* images) const {
	if (m_stream == nullptr) {
		m_direct.pipelineBarrier(src_stages, dst_stages, dependency, memory_count, memory,
		                         buffer_count, buffers, image_count, images);
		return;
	}
	EXIT_IF((memory_count != 0 && memory->pNext != nullptr) ||
	        (buffer_count != 0 && buffers->pNext != nullptr) ||
	        (image_count != 0 && images->pNext != nullptr));
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::MemoryBarrier) * memory_count) +
	                  CommandStream::RoundUp(sizeof(vk::BufferMemoryBarrier) * buffer_count) +
	                  CommandStream::RoundUp(sizeof(vk::ImageMemoryBarrier) * image_count) + 128);
	const auto* memory_copy  = m_stream->Copy(memory, memory_count);
	const auto* buffer_copy  = m_stream->Copy(buffers, buffer_count);
	const auto* image_copy   = m_stream->Copy(images, image_count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.pipelineBarrier(src_stages, dst_stages, dependency, memory_count, memory_copy,
		                  buffer_count, buffer_copy, image_count, image_copy);
	});
}

void CommandRecorder::pipelineBarrier2(const vk::DependencyInfo& dependency) const {
	if (m_stream == nullptr) {
		m_direct.pipelineBarrier2(dependency);
		return;
	}
	EXIT_IF(dependency.pNext != nullptr);
	m_stream->Reserve(
	    CommandStream::RoundUp(sizeof(vk::DependencyInfo)) +
	    CommandStream::RoundUp(sizeof(vk::MemoryBarrier2) * dependency.memoryBarrierCount) +
	    CommandStream::RoundUp(sizeof(vk::BufferMemoryBarrier2) *
	                           dependency.bufferMemoryBarrierCount) +
	    CommandStream::RoundUp(sizeof(vk::ImageMemoryBarrier2) * dependency.imageMemoryBarrierCount) +
	    128);
	auto* copy                     = m_stream->Copy(&dependency, 1);
	copy->pMemoryBarriers          = m_stream->Copy(dependency.pMemoryBarriers,
	                                                dependency.memoryBarrierCount);
	copy->pBufferMemoryBarriers    = m_stream->Copy(dependency.pBufferMemoryBarriers,
	                                                dependency.bufferMemoryBarrierCount);
	copy->pImageMemoryBarriers     = m_stream->Copy(dependency.pImageMemoryBarriers,
	                                                dependency.imageMemoryBarrierCount);
	const vk::DependencyInfo* info = copy;
	m_stream->Record([=](vk::CommandBuffer c) { c.pipelineBarrier2(info); });
}

void CommandRecorder::bindDescriptorSets(vk::PipelineBindPoint bind_point,
                                         vk::PipelineLayout layout, uint32_t first_set,
                                         uint32_t set_count, const vk::DescriptorSet* sets,
                                         uint32_t        dynamic_count,
                                         const uint32_t* dynamic_offsets) const {
	if (m_stream == nullptr) {
		m_direct.bindDescriptorSets(bind_point, layout, first_set, set_count, sets, dynamic_count,
		                            dynamic_offsets);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::DescriptorSet) * set_count) +
	                  CommandStream::RoundUp(sizeof(uint32_t) * dynamic_count) + 128);
	const auto* sets_copy    = m_stream->Copy(sets, set_count);
	const auto* offsets_copy = m_stream->Copy(dynamic_offsets, dynamic_count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.bindDescriptorSets(bind_point, layout, first_set, set_count, sets_copy, dynamic_count,
		                     offsets_copy);
	});
}

void CommandRecorder::pushDescriptorSetKHR(vk::PipelineBindPoint bind_point,
                                           vk::PipelineLayout layout, uint32_t set,
                                           uint32_t                      write_count,
                                           const vk::WriteDescriptorSet* writes) const {
	if (m_stream == nullptr) {
		m_direct.pushDescriptorSetKHR(bind_point, layout, set, write_count, writes);
		return;
	}
	size_t bytes = CommandStream::RoundUp(sizeof(vk::WriteDescriptorSet) * write_count) + 128;
	for (uint32_t i = 0; i < write_count; i++) {
		const auto& write = writes[i];
		EXIT_IF(write.pNext != nullptr);
		if (write.pBufferInfo != nullptr) {
			bytes += CommandStream::RoundUp(sizeof(vk::DescriptorBufferInfo) * write.descriptorCount);
		}
		if (write.pImageInfo != nullptr) {
			bytes += CommandStream::RoundUp(sizeof(vk::DescriptorImageInfo) * write.descriptorCount);
		}
		if (write.pTexelBufferView != nullptr) {
			bytes += CommandStream::RoundUp(sizeof(vk::BufferView) * write.descriptorCount);
		}
	}
	m_stream->Reserve(bytes);
	auto* copy = m_stream->Copy(writes, write_count);
	for (uint32_t i = 0; i < write_count; i++) {
		auto& write = copy[i];
		if (write.pBufferInfo != nullptr) {
			write.pBufferInfo = m_stream->Copy(write.pBufferInfo, write.descriptorCount);
		}
		if (write.pImageInfo != nullptr) {
			write.pImageInfo = m_stream->Copy(write.pImageInfo, write.descriptorCount);
		}
		if (write.pTexelBufferView != nullptr) {
			write.pTexelBufferView = m_stream->Copy(write.pTexelBufferView, write.descriptorCount);
		}
	}
	const vk::WriteDescriptorSet* writes_copy = copy;
	m_stream->Record([=](vk::CommandBuffer c) {
		c.pushDescriptorSetKHR(bind_point, layout, set, write_count, writes_copy);
	});
}

void CommandRecorder::pushConstants(vk::PipelineLayout layout, vk::ShaderStageFlags stages,
                                    uint32_t offset, uint32_t size, const void* values) const {
	if (m_stream == nullptr) {
		m_direct.pushConstants(layout, stages, offset, size, values);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(size) + 128);
	const auto* values_copy = m_stream->Copy(static_cast<const std::byte*>(values), size);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.pushConstants(layout, stages, offset, size, values_copy);
	});
}

void CommandRecorder::beginRendering(const vk::RenderingInfo& rendering) const {
	if (m_stream == nullptr) {
		m_direct.beginRendering(rendering);
		return;
	}
	EXIT_IF(rendering.pNext != nullptr);
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::RenderingInfo)) +
	                  CommandStream::RoundUp(sizeof(vk::RenderingAttachmentInfo) *
	                                         rendering.colorAttachmentCount) +
	                  2 * CommandStream::RoundUp(sizeof(vk::RenderingAttachmentInfo)) + 128);
	auto* copy               = m_stream->Copy(&rendering, 1);
	copy->pColorAttachments  = m_stream->Copy(rendering.pColorAttachments,
	                                          rendering.colorAttachmentCount);
	copy->pDepthAttachment   = m_stream->Copy(rendering.pDepthAttachment, 1);
	copy->pStencilAttachment = m_stream->Copy(rendering.pStencilAttachment, 1);
	const vk::RenderingInfo* info = copy;
	m_stream->Record([=](vk::CommandBuffer c) { c.beginRendering(info); });
}

void CommandRecorder::bindVertexBuffers2(uint32_t first, uint32_t count,
                                         const vk::Buffer* buffers, const vk::DeviceSize* offsets,
                                         const vk::DeviceSize* sizes,
                                         const vk::DeviceSize* strides) const {
	if (m_stream == nullptr) {
		m_direct.bindVertexBuffers2(first, count, buffers, offsets, sizes, strides);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::Buffer) * count) +
	                  3 * CommandStream::RoundUp(sizeof(vk::DeviceSize) * count) + 128);
	const auto* buffers_copy = m_stream->Copy(buffers, count);
	const auto* offsets_copy = m_stream->Copy(offsets, count);
	const auto* sizes_copy   = m_stream->Copy(sizes, count);
	const auto* strides_copy = m_stream->Copy(strides, count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.bindVertexBuffers2(first, count, buffers_copy, offsets_copy, sizes_copy, strides_copy);
	});
}

void CommandRecorder::copyBuffer(vk::Buffer source, vk::Buffer destination, uint32_t count,
                                 const vk::BufferCopy* regions) const {
	if (m_stream == nullptr) {
		m_direct.copyBuffer(source, destination, count, regions);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::BufferCopy) * count) + 128);
	const auto* copy = m_stream->Copy(regions, count);
	m_stream->Record(
	    [=](vk::CommandBuffer c) { c.copyBuffer(source, destination, count, copy); });
}

void CommandRecorder::copyImage(vk::Image source, vk::ImageLayout source_layout,
                                vk::Image destination, vk::ImageLayout destination_layout,
                                uint32_t count, const vk::ImageCopy* regions) const {
	if (m_stream == nullptr) {
		m_direct.copyImage(source, source_layout, destination, destination_layout, count,
		                   regions);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::ImageCopy) * count) + 128);
	const auto* copy = m_stream->Copy(regions, count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.copyImage(source, source_layout, destination, destination_layout, count, copy);
	});
}

void CommandRecorder::resolveImage(vk::Image source, vk::ImageLayout source_layout,
                                   vk::Image destination, vk::ImageLayout destination_layout,
                                   uint32_t count, const vk::ImageResolve* regions) const {
	if (m_stream == nullptr) {
		m_direct.resolveImage(source, source_layout, destination, destination_layout, count,
		                      regions);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::ImageResolve) * count) + 128);
	const auto* copy = m_stream->Copy(regions, count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.resolveImage(source, source_layout, destination, destination_layout, count, copy);
	});
}

void CommandRecorder::blitImage(vk::Image source, vk::ImageLayout source_layout,
                                vk::Image destination, vk::ImageLayout destination_layout,
                                uint32_t count, const vk::ImageBlit* regions,
                                vk::Filter filter) const {
	if (m_stream == nullptr) {
		m_direct.blitImage(source, source_layout, destination, destination_layout, count, regions,
		                   filter);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::ImageBlit) * count) + 128);
	const auto* copy = m_stream->Copy(regions, count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.blitImage(source, source_layout, destination, destination_layout, count, copy, filter);
	});
}

void CommandRecorder::copyImageToBuffer(vk::Image source, vk::ImageLayout source_layout,
                                        vk::Buffer destination, uint32_t count,
                                        const vk::BufferImageCopy* regions) const {
	if (m_stream == nullptr) {
		m_direct.copyImageToBuffer(source, source_layout, destination, count, regions);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::BufferImageCopy) * count) + 128);
	const auto* copy = m_stream->Copy(regions, count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.copyImageToBuffer(source, source_layout, destination, count, copy);
	});
}

void CommandRecorder::copyBufferToImage(vk::Buffer source, vk::Image destination,
                                        vk::ImageLayout destination_layout, uint32_t count,
                                        const vk::BufferImageCopy* regions) const {
	if (m_stream == nullptr) {
		m_direct.copyBufferToImage(source, destination, destination_layout, count, regions);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::BufferImageCopy) * count) + 128);
	const auto* copy = m_stream->Copy(regions, count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.copyBufferToImage(source, destination, destination_layout, count, copy);
	});
}

void CommandRecorder::clearColorImage(vk::Image image, vk::ImageLayout layout,
                                      const vk::ClearColorValue* color, uint32_t range_count,
                                      const vk::ImageSubresourceRange* ranges) const {
	if (m_stream == nullptr) {
		m_direct.clearColorImage(image, layout, color, range_count, ranges);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::ClearColorValue)) +
	                  CommandStream::RoundUp(sizeof(vk::ImageSubresourceRange) * range_count) + 128);
	const auto* color_copy  = m_stream->Copy(color, 1);
	const auto* ranges_copy = m_stream->Copy(ranges, range_count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.clearColorImage(image, layout, color_copy, range_count, ranges_copy);
	});
}

void CommandRecorder::clearDepthStencilImage(vk::Image image, vk::ImageLayout layout,
                                             const vk::ClearDepthStencilValue* value,
                                             uint32_t                          range_count,
                                             const vk::ImageSubresourceRange*  ranges) const {
	if (m_stream == nullptr) {
		m_direct.clearDepthStencilImage(image, layout, value, range_count, ranges);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::ClearDepthStencilValue)) +
	                  CommandStream::RoundUp(sizeof(vk::ImageSubresourceRange) * range_count) + 128);
	const auto* value_copy  = m_stream->Copy(value, 1);
	const auto* ranges_copy = m_stream->Copy(ranges, range_count);
	m_stream->Record([=](vk::CommandBuffer c) {
		c.clearDepthStencilImage(image, layout, value_copy, range_count, ranges_copy);
	});
}

void CommandRecorder::setViewport(uint32_t first, uint32_t count,
                                  const vk::Viewport* viewports) const {
	if (m_stream == nullptr) {
		m_direct.setViewport(first, count, viewports);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::Viewport) * count) + 128);
	const auto* copy = m_stream->Copy(viewports, count);
	m_stream->Record([=](vk::CommandBuffer c) { c.setViewport(first, count, copy); });
}

void CommandRecorder::setScissor(uint32_t first, uint32_t count, const vk::Rect2D* scissors) const {
	if (m_stream == nullptr) {
		m_direct.setScissor(first, count, scissors);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::Rect2D) * count) + 128);
	const auto* copy = m_stream->Copy(scissors, count);
	m_stream->Record([=](vk::CommandBuffer c) { c.setScissor(first, count, copy); });
}

void CommandRecorder::setViewportWithCount(uint32_t count, const vk::Viewport* viewports) const {
	if (m_stream == nullptr) {
		m_direct.setViewportWithCount(count, viewports);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::Viewport) * count) + 128);
	const auto* copy = m_stream->Copy(viewports, count);
	m_stream->Record([=](vk::CommandBuffer c) { c.setViewportWithCount(count, copy); });
}

void CommandRecorder::setScissorWithCount(uint32_t count, const vk::Rect2D* scissors) const {
	if (m_stream == nullptr) {
		m_direct.setScissorWithCount(count, scissors);
		return;
	}
	m_stream->Reserve(CommandStream::RoundUp(sizeof(vk::Rect2D) * count) + 128);
	const auto* copy = m_stream->Copy(scissors, count);
	m_stream->Record([=](vk::CommandBuffer c) { c.setScissorWithCount(count, copy); });
}

} // namespace Libs::Graphics
