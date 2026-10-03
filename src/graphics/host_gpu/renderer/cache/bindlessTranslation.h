#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BINDLESSTRANSLATION_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BINDLESSTRANSLATION_H_

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace Libs::Graphics::Bindless {

using TSharp = std::array<uint32_t, 8>;

struct TSharpHash {
	size_t operator()(const TSharp& words) const noexcept;
};

struct RecordShape {
	ShaderRecompiler::Decoder::ImageDimension dimension =
	    ShaderRecompiler::Decoder::ImageDimension::Dim2D;
	bool                                cube    = false;
	ShaderRecompiler::IR::BindlessShape array   = ShaderRecompiler::IR::BindlessShape::Image2D;
	Prospero::TextureNumericClass       numeric = Prospero::TextureNumericClass::Float;

	[[nodiscard]] uint32_t Code() const {
		return ShaderRecompiler::IR::BindlessShapeCode(dimension, cube, numeric);
	}
};

[[nodiscard]] std::optional<RecordShape> ClassifyRecord(const TSharp& words, bool integer = false);

[[nodiscard]] uint32_t HeapRecordCount(uint64_t size, uint32_t stride, uint32_t offset,
                                       uint32_t max_records);

[[nodiscard]] constexpr uint32_t TranslationWord(const RecordShape& shape, uint32_t element) {
	return ShaderRecompiler::IR::IndirectImageSlot(shape.Code(), element);
}

class SlotAllocator {
public:
	[[nodiscard]] std::optional<uint32_t> Assign(ShaderRecompiler::IR::BindlessShape array,
	                                             uint64_t                            key);
	[[nodiscard]] uint32_t                Used(ShaderRecompiler::IR::BindlessShape array) const;
	void                                  Reset();

private:
	static constexpr size_t Arrays =
	    static_cast<size_t>(ShaderRecompiler::IR::BindlessShape::Count);

	std::array<std::unordered_map<uint64_t, uint32_t>, Arrays> m_keys;
	std::array<uint32_t, Arrays>                               m_next {};
};

using TranslationCache = std::unordered_map<TSharp, uint32_t, TSharpHash>;

using RecordResolver = std::function<std::optional<uint32_t>(const TSharp&, const RecordShape&)>;

void TranslateHeap(std::span<const uint32_t> heap, uint32_t stride, uint32_t offset,
                   uint32_t records, TranslationCache& cache, const RecordResolver& resolve,
                   std::span<uint32_t> words, bool integer = false);

[[nodiscard]] std::vector<TSharp> MissingRecords(std::span<const uint32_t> heap, uint32_t stride,
                                                 uint32_t offset, uint32_t records,
                                                 const TranslationCache& cache,
                                                 bool                    integer = false);

} // namespace Libs::Graphics::Bindless

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_CACHE_BINDLESSTRANSLATION_H_
