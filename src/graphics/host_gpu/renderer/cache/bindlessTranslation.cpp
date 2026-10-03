#include "graphics/host_gpu/renderer/cache/bindlessTranslation.h"

#include "graphics/guest_gpu/gpu_format.h"

#include <unordered_set>
#include <xxhash.h>

namespace Libs::Graphics::Bindless {

using ShaderRecompiler::Decoder::ImageDimension;
using ShaderRecompiler::IR::BindlessShape;

size_t TSharpHash::operator()(const TSharp& words) const noexcept {
	return static_cast<size_t>(XXH3_64bits(words.data(), sizeof(words)));
}

std::optional<RecordShape> ClassifyRecord(const TSharp& words, bool integer) {
	// Base address and format both zero is the null T#.
	if (words[0] == 0u && (words[1] & 0xffu) == 0u) {
		return std::nullopt;
	}
	if ((words[1] & 0x20000000u) != 0u || (words[2] & 0x70003000u) != 0u ||
	    (words[4] & 0xe000e000u) != 0u || (words[5] & 0xf9000000u) != 0u ||
	    (words[6] & 0x00007b00u) != 0u) {
		return std::nullopt;
	}
	const auto format = static_cast<Prospero::BufferFormat>((words[1] >> 20u) & 0x1ffu);
	if (format == Prospero::BufferFormat::kInvalid || format > Prospero::BufferFormat::kBc7Srgb ||
	    Prospero::IsFmaskTextureFormat(format) || Prospero::RemapTextureFormat(format) != format) {
		return std::nullopt;
	}
	const auto numeric = Prospero::SampledTextureNumericClass(format);
	if (numeric != Prospero::TextureNumericClass::Float &&
	    (numeric != Prospero::TextureNumericClass::Uint || !integer)) {
		return std::nullopt;
	}
	RecordShape shape;
	switch (static_cast<Prospero::ImageType>((words[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor2D:
			shape = {ImageDimension::Dim2D, false, BindlessShape::Image2D};
			break;
		case Prospero::ImageType::kColor2DArray:
			shape = {ImageDimension::Dim2DArray, false, BindlessShape::Image2DArray};
			break;
		case Prospero::ImageType::kCube:
			shape = {ImageDimension::Dim2DArray, true, BindlessShape::Image2DArray};
			break;
		case Prospero::ImageType::kColor3D:
			shape = {ImageDimension::Dim3D, false, BindlessShape::Image3D};
			break;
		case Prospero::ImageType::kColor1D:
		case Prospero::ImageType::kColor1DArray:
			shape = {ImageDimension::Dim1D, false, BindlessShape::Image1D};
			break;
		default: return std::nullopt;
	}
	shape.numeric   = numeric;
	shape.array     = *ShaderRecompiler::IR::BindlessShapeFor(shape.dimension, numeric);
	const auto type = static_cast<Prospero::ImageType>((words[3] >> 28u) & 0xfu);
	if ((shape.dimension == ImageDimension::Dim2DArray ||
	     type == Prospero::ImageType::kColor1DArray) &&
	    ((words[4] >> 16u) & 0x1fffu) > (words[4] & 0x1fffu)) {
		return std::nullopt;
	}
	const auto base_level = (words[3] >> 12u) & 0xfu;
	const auto last_level = (words[3] >> 16u) & 0xfu;
	const auto max_mip    = (words[5] >> 4u) & 0xfu;
	const auto min_lod    = (words[1] >> 8u) & 0xfffu;
	if (base_level > last_level || last_level > max_mip || min_lod > last_level * 256u) {
		return std::nullopt;
	}
	return shape;
}

uint32_t HeapRecordCount(uint64_t size, uint32_t stride, uint32_t offset, uint32_t max_records) {
	constexpr uint64_t DescriptorBytes = sizeof(TSharp);
	if (stride < DescriptorBytes || (stride % 4u) != 0u || (offset % 4u) != 0u ||
	    offset > stride - DescriptorBytes || size < offset + DescriptorBytes) {
		return 0;
	}
	const auto records = (size - offset - DescriptorBytes) / stride + 1u;
	return static_cast<uint32_t>(std::min<uint64_t>(records, max_records));
}

std::optional<uint32_t> SlotAllocator::Assign(BindlessShape array, uint64_t key) {
	const auto index = static_cast<size_t>(array);
	auto&      keys  = m_keys[index];
	if (const auto found = keys.find(key); found != keys.end()) {
		return found->second;
	}
	if (m_next[index] >= ShaderRecompiler::IR::BindlessImageSlots) {
		return std::nullopt;
	}
	const auto slot = m_next[index]++;
	keys.emplace(key, slot);
	return slot;
}

uint32_t SlotAllocator::Used(BindlessShape array) const {
	return m_next[static_cast<size_t>(array)];
}

void SlotAllocator::Reset() {
	for (auto& keys: m_keys) {
		keys.clear();
	}
	m_next.fill(0u);
}

namespace {

TSharp RecordAt(std::span<const uint32_t> heap, uint32_t stride, uint32_t offset, uint32_t record) {
	TSharp     words {};
	const auto first = (static_cast<size_t>(record) * stride + offset) / sizeof(uint32_t);
	for (size_t dword = 0; dword < words.size() && first + dword < heap.size(); dword++) {
		words[dword] = heap[first + dword];
	}
	return words;
}

} // namespace

void TranslateHeap(std::span<const uint32_t> heap, uint32_t stride, uint32_t offset,
                   uint32_t records, TranslationCache& cache, const RecordResolver& resolve,
                   std::span<uint32_t> words, bool integer) {
	for (uint32_t record = 0; record < records && record < words.size(); record++) {
		const auto tsharp = RecordAt(heap, stride, offset, record);
		if (const auto found = cache.find(tsharp); found != cache.end()) {
			words[record] = found->second;
			continue;
		}
		const auto shape = ClassifyRecord(tsharp, integer);
		const auto word =
		    shape.has_value() ? resolve(tsharp, *shape) : std::optional<uint32_t> {0u};
		if (word.has_value()) {
			cache.emplace(tsharp, *word);
		}
		words[record] = word.value_or(0u);
	}
}

std::vector<TSharp> MissingRecords(std::span<const uint32_t> heap, uint32_t stride, uint32_t offset,
                                   uint32_t records, const TranslationCache& cache, bool integer) {
	std::vector<TSharp>                    missing;
	std::unordered_set<TSharp, TSharpHash> seen;
	for (uint32_t record = 0; record < records; record++) {
		const auto tsharp = RecordAt(heap, stride, offset, record);
		if (cache.contains(tsharp) || !ClassifyRecord(tsharp, integer).has_value() ||
		    !seen.insert(tsharp).second) {
			continue;
		}
		missing.push_back(tsharp);
	}
	return missing;
}

} // namespace Libs::Graphics::Bindless
