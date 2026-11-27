#include "graphics/presentation/system/element/frame/frameView.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace Libs::Graphics::SystemUi {

namespace {

// Big enough that typical labels share a chunk, small enough to waste little on a quiet frame.
constexpr size_t TEXT_CHUNK_BYTES = 4096;

} // namespace

std::string_view FrameView::Intern(std::string_view text) {
	if (text.empty()) {
		return {};
	}
	char* dest = ReserveText(text.size());
	std::memcpy(dest, text.data(), text.size());
	return {dest, text.size()};
}

const char* FrameView::InternCString(std::string_view text) {
	char* dest = ReserveText(text.size() + 1);
	// An empty view may have no data, which memcpy may not be given even to copy nothing.
	if (!text.empty()) {
		std::memcpy(dest, text.data(), text.size());
	}
	dest[text.size()] = '\0';
	return dest;
}

char* FrameView::ReserveText(size_t size) {
	// Skip chunks that can't take the text. Their tail stays unused until the next Begin, which is
	// cheaper than searching every chunk for a fit.
	while (m_current_chunk < m_chunks.size() &&
	       m_chunks[m_current_chunk].capacity - m_chunks[m_current_chunk].used < size) {
		++m_current_chunk;
	}
	if (m_current_chunk == m_chunks.size()) {
		TextChunk chunk;
		// Text longer than a chunk gets a chunk of its own.
		chunk.capacity = std::max(TEXT_CHUNK_BYTES, size);
		chunk.data     = std::make_unique<char[]>(chunk.capacity);
		m_chunks.push_back(std::move(chunk));
	}
	TextChunk& chunk = m_chunks[m_current_chunk];
	char*      dest  = chunk.data.get() + chunk.used;
	chunk.used += size;
	return dest;
}

std::byte* FrameView::StoreBytes(const void* bytes, size_t size) {
	const std::string_view copy = Intern({static_cast<const char*>(bytes), size});
	// The chunk is the frame's own writable storage, so dropping the view's const is sound.
	return const_cast<std::byte*>(reinterpret_cast<const std::byte*>(copy.data()));
}

size_t FrameView::ReservedBytes() const {
	size_t total = m_tree.ReservedBytes() + m_elements.capacity() * sizeof(ElementMetadata) +
	               m_layouts.capacity() * sizeof(ElementLayout) +
	               m_key_scratch.capacity() * sizeof(const ElementMetadata*) +
	               m_child_scratch.capacity() * sizeof(NodeHandle) +
	               m_element_stack.capacity() * sizeof(AnyElement) +
	               m_chunks.capacity() * sizeof(TextChunk);
	for (const TextChunk& chunk: m_chunks) {
		total += chunk.capacity;
	}
	total += m_tables.capacity() * sizeof(std::unique_ptr<TableBase>);
	for (const std::unique_ptr<TableBase>& table: m_tables) {
		if (table != nullptr) {
			total += table->ReservedBytes();
		}
	}
	return total;
}

} // namespace Libs::Graphics::SystemUi
