#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/alignment.h"
#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/Serialize.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <optional>
#include <array>
#include <atomic>
#include <utility>
#include <bit>
#include <cstdio>
#include <fmt/format.h>
#include <functional>
#include <numeric>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;

struct DescriptorValueHash {
	size_t operator()(const DescriptorValue& value) const noexcept {
		uint64_t hash = 0xcbf29ce484222325ull ^ value.dword_count;
		for (const auto dword: value.dwords) {
			hash = (hash ^ dword) * 0x100000001b3ull;
			hash ^= hash >> 29u;
		}
		return static_cast<size_t>(hash);
	}
};

// With stable layouts, image table mappings round up to this many words, so the offsets after
// them, which shaders take as constants, stay the same when the game resizes a table a little.
constexpr size_t StableSrtWords = 4096u;
// Record-table blocks of the current materialization (see MaterializeIndirectDescriptor()): mapping
// words name them by their offset within the root's blocks until FlushRecordBlocks() appends them.
struct RecordBlocks {
	size_t                mapping_offset = 0;
	size_t                entries        = 0;
	std::vector<uint32_t> blocks;
};
thread_local std::vector<RecordBlocks> t_record_blocks;

void FlushRecordBlocks(std::vector<uint32_t>& flattened_srt) {
	for (auto& record: t_record_blocks) {
		// A block's flattened SRT word; 0 (no block) never names one.
		const auto base = static_cast<uint32_t>(flattened_srt.size());
		for (size_t entry = 0; entry < record.entries; ++entry) {
			flattened_srt[record.mapping_offset + 2u + entry * 2u] += base;
		}
		flattened_srt.insert(flattened_srt.end(), record.blocks.begin(), record.blocks.end());
	}
	t_record_blocks.clear();
}

// Keys an indirect descriptor mapping reserves room for at least (see MaterializeIndirectDescriptor()).
constexpr size_t MinIndirectMappingCapacity = 8u;
// Elements of one table that a GPU-chosen record names (see MaterializeIndirectDescriptor()):
// scene-wide tables hold hundreds of thousands of them.
constexpr uint64_t MaxRecordTableElements = uint64_t {1} << 21u;
// See SetComputeIndirectFloors().
std::atomic<size_t> g_compute_indirect_keys {MinIndirectMappingCapacity};
std::atomic<size_t> g_compute_indirect_images {1u};
// Set by SetComputeIndirectFloors(): without it mappings and images keep their exact counts.
std::atomic<bool> g_stable_indirect_layouts {false};

bool SpecializationFail(std::string_view message) {
	std::fprintf(stderr, "shader resource specialization failed: %.*s\n",
	             static_cast<int>(message.size()), message.data());
	return false;
}

Decoder::ImageDimension DescriptorDimension(const DescriptorValue&  descriptor,
                                            Decoder::ImageDimension requested) {
	const bool is_array = requested == Decoder::ImageDimension::Dim1DArray ||
	                      requested == Decoder::ImageDimension::Dim2DArray ||
	                      requested == Decoder::ImageDimension::Dim2DMsaaArray;
	switch (static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu)) {
		case Prospero::ImageType::kColor1D: return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor1DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim1DArray;
			}
			return Decoder::ImageDimension::Dim1D;
		case Prospero::ImageType::kColor3D: return Decoder::ImageDimension::Dim3D;
		case Prospero::ImageType::kCube: return Decoder::ImageDimension::Dim2DArray;
		case Prospero::ImageType::kColor2DArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DArray;
			}
			return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaaArray:
			if (is_array) {
				return Decoder::ImageDimension::Dim2DMsaaArray;
			}
			return Decoder::ImageDimension::Dim2DMsaa;
		case Prospero::ImageType::kColor2D: return Decoder::ImageDimension::Dim2D;
		case Prospero::ImageType::kColor2DMsaa: return Decoder::ImageDimension::Dim2DMsaa;
		default: return Decoder::ImageDimension::Unknown;
	}
}

bool NullImageDescriptor(const DescriptorValue& descriptor) {
	return descriptor.dwords[0] == 0 && (descriptor.dwords[1] & 0xffu) == 0;
}

bool NullBufferDescriptor(Value value) {
	const auto* handle = value.Resolve().TryInstruction();
	if (handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource ||
	    handle->NumArgs() != 4u) return false;
	for (size_t index = 0; index < 4u; ++index)
		if (handle->Arg(index).Resolve() != Value(0u)) return false;
	return true;
}

bool ValidImageDescriptor(const DescriptorValue& descriptor, bool r128 = false) {
	const auto& words = descriptor.dwords;
	// Reject texture descriptors with nonzero reserved bits.
	if ((words[1] & 0x20000000u) != 0u || (words[2] & 0x70003000u) != 0u ||
	    (!r128 && ((words[4] & 0xe000e000u) != 0u || (words[5] & 0xf9000000u) != 0u ||
	               (words[6] & 0x00007b00u) != 0u))) {
		return false;
	}
	const auto type   = static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu);
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	if (type < Prospero::ImageType::kColor1D || format == Prospero::BufferFormat::kInvalid ||
	    format > Prospero::BufferFormat::kBc7Srgb) {
		return false;
	}
	if (r128 && type != Prospero::ImageType::kColor1D && type != Prospero::ImageType::kColor2D &&
	    type != Prospero::ImageType::kColor2DMsaa) {
		return false;
	}
	if (!IsValidImageSwizzle(words[3] & 0xfffu)) return false;
	const bool array = type == Prospero::ImageType::kColor1DArray ||
	                   type == Prospero::ImageType::kColor2DArray ||
	                   type == Prospero::ImageType::kColor2DMsaaArray ||
	                   type == Prospero::ImageType::kCube;
	if (array && ((words[4] >> 16u) & 0x1fffu) > (words[4] & 0x1fffu)) {
		return false;
	}
	const auto base_level = (words[3] >> 12u) & 0xfu;
	const auto last_level = (words[3] >> 16u) & 0xfu;
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && last_level >= 1 && last_level <= 3 &&
		       (r128 || max_mip == last_level);
	}
	return base_level <= last_level;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

// A storage image in its own format applies the swizzle only to stores (see StoreTexel), and a
// store keeps just the channels the format has. A swizzle that stores each of them from its own
// component is the identity, so an RG target written as (R, G, 0, 1) shares the program of an
// RGBA target written as (R, G, B, A).
uint32_t CanonicalStorageSwizzle(uint32_t swizzle, Prospero::BufferFormat format) {
	const auto info = Format::GetFormatInfo(format);
	if (info.type == Format::ComponentType::Unknown || info.component_count == 0u ||
	    info.component_count > 4u) {
		return swizzle;
	}
	for (uint32_t channel = 0; channel < info.component_count; channel++) {
		uint32_t source = 0;
		while (source < 4u && ((swizzle >> (source * 3u)) & 7u) != 4u + channel) source++;
		if (source != channel) return swizzle;
	}
	return ShaderImageIdentitySwizzle;
}

Prospero::BufferFormat ImageConversionFormat(Prospero::BufferFormat format) {
	return Prospero::RemapTextureFormat(format) != format ? format
	                                                      : Prospero::BufferFormat::kInvalid;
}

bool ValidatePackedUnormSampling(const ResourcePlan& program, const ResourceSnapshot& snapshot,
                                 uint32_t image_index) {
	ShaderTextureResource texture;
	std::copy_n(snapshot.images[image_index].dwords.begin(), std::size(texture.fields),
	            texture.fields);
	if (texture.Type() != Prospero::ImageType::kColor2D ||
	    texture.BaseLevel() != texture.LastLevel() || texture.MinLod() != 0u ||
	    texture.CornerSample()) {
		return SpecializationFail("packed UNorm sampling requires a centered, single-mip 2D image");
	}
	for (const auto& pair: program.info.sampled_pairs) {
		if (pair.image != image_index) continue;
		if (pair.sampler >= snapshot.samplers.size() ||
		    program.descriptor_sources[program.info.samplers[pair.sampler].source].indirect_descriptor) {
			return SpecializationFail("packed UNorm sampling requires a direct sampler");
		}
		ShaderSamplerResource sampler;
		std::copy_n(snapshot.samplers[pair.sampler].dwords.begin(), std::size(sampler.fields),
		            sampler.fields);
		const auto linear = static_cast<uint32_t>(Prospero::SamplerFilter::kBilinear);
		const auto clamp = static_cast<uint32_t>(Prospero::SamplerClampMode::kClampLastTexel);
		if (sampler.XyMagFilter() != linear || sampler.XyMinFilter() != linear ||
		    sampler.ClampX() != clamp || sampler.ClampY() != clamp ||
		    sampler.ForceUnormCoords() || sampler.MaxAnisoRatio() != 0u ||
		    sampler.FilterMode() != 0u || sampler.MinLod() != 0u || sampler.ForceSrgb() ||
		    sampler.TruncCoord() || (sampler.fields[0] & (1u << 19u)) != 0u) { // MC_COORD_TRUNC
			return SpecializationFail(fmt::format(
			    "packed UNorm image {} requires normalized bilinear clamp sampling (sampler {})",
			    image_index, pair.sampler));
		}
	}
	return true;
}

enum class SamplerClass : uint8_t { Float, Integer };

SamplerClass ClassifySampler(const ImageResource& image) {
	return image.numeric_class == Prospero::TextureNumericClass::Uint ||
	               image.numeric_class == Prospero::TextureNumericClass::Sint
	           ? SamplerClass::Integer : SamplerClass::Float;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

uint32_t ImageMipCount(const ImageResource& image, const DescriptorValue& descriptor) {
	if (image.mip_mode != ImageMipMode::Dynamic || NullImageDescriptor(descriptor)) {
		return 1;
	}
	const auto base = (descriptor.dwords[3] >> 12u) & 0xfu;
	const auto last = (descriptor.dwords[3] >> 16u) & 0xfu;
	return last - base + 1u;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

bool ReadTable(uint64_t base, uint64_t size, uint64_t offset,
               const SrtRuntime& runtime, std::span<uint32_t> words) {
	const auto count = std::min<uint64_t>(words.size(), offset < size ? (size - offset) / 4u : 0u);
	std::ranges::fill(words.subspan(count), 0u);
	if (count == 0u) {
		return true;
	}
	if (base > AddressMask || offset > AddressMask - base) {
		return false;
	}
	const auto address = base + offset;
	const auto prefix = words.first(count);
	return prefix.size_bytes() - 1u <= AddressMask - address &&
	       runtime.read_specialization_memory != nullptr &&
	       runtime.read_specialization_memory(runtime.userdata, address, prefix);
}

bool ReadScalarTable(uint64_t base, uint64_t size, uint64_t offset,
                     const SrtRuntime& runtime, std::span<uint32_t> words) {
	return ReadTable(base & AddressMask & ~uint64_t {3}, size, offset & ~uint64_t {3}, runtime, words);
}

// The first and the last word of a table are in guest memory.
bool TableInGuestMemory(uint64_t base, uint64_t size, const SrtRuntime& runtime) {
	std::array<uint32_t, 1> word {};
	return base != 0u && size >= 4u && ReadScalarTable(base, size, 0u, runtime, word) &&
	       ReadScalarTable(base, size, size - 4u, runtime, word);
}

bool NormalizeIndirectBuffer(DescriptorValue& value, bool indexed, const BufferResource& resource) {
	ShaderBufferResource descriptor;
	if (!DecodeBufferDescriptor(value, descriptor)) return false;
	const bool formatted_read = resource.formatted && resource.read && !resource.written;
	if (descriptor.Type() != 0u || descriptor.Format() == Prospero::BufferFormat::kInvalid ||
	    (!formatted_read && (descriptor.Base48() == 0u || descriptor.GetSize() == 0u))) {
		value.dwords.fill(0u);
		return true;
	}
	if (descriptor.SwizzleEnabled() || descriptor.AddTid() ||
	    descriptor.OutOfBounds() != (indexed ? 0u : 3u)) {
		return SpecializationFail(fmt::format(
		    "indirect buffers require exact linear index or byte bounds: "
		    "descriptor={:08x},{:08x},{:08x},{:08x} swizzle={} add_tid={} oob={} "
		    "indexed={} formatted={} read={} written={} max_extent={}",
		    value.dwords[0], value.dwords[1], value.dwords[2], value.dwords[3],
		    descriptor.SwizzleEnabled(), descriptor.AddTid(), descriptor.OutOfBounds(), indexed,
		    resource.formatted, resource.read, resource.written, resource.max_byte_extent));
	}
	if (resource.formatted) {
		const auto format = Format::GetFormatInfo(descriptor.Format());
		if (!indexed || format.type == Format::ComponentType::Unknown)
			return SpecializationFail("indirect formatted buffers require a supported indexed format");
		if (!formatted_read) {
			const bool scalar = resource.max_byte_extent == 4u &&
			    (descriptor.Format() == Prospero::BufferFormat::k32UInt ||
			     descriptor.Format() == Prospero::BufferFormat::k8SInt);
			const bool rgba16 = resource.max_byte_extent == 16u &&
			    descriptor.Format() == Prospero::BufferFormat::k16_16_16_16Float;
			if ((!scalar && !rgba16) || descriptor.Stride() != format.byte_size ||
			    descriptor.DstSelXYZW() != (rgba16 ? DstSel(4, 5, 6, 7) : DstSel(4, 0, 0, 0)))
				return SpecializationFail("indirect formatted stores require tight identity SInt8, UInt32 or RGBA16Float records");
		}
		if (formatted_read &&
		    (descriptor.Base48() == 0u || descriptor.Stride() == 0u || descriptor.NumRecords() == 0u)) {
			// Every index is out of bounds. Preserve the format and swizzle constants.
			value.dwords[1] &= ~(0x3fffu << 16u);
			value.dwords[2] = 0u;
			return true;
		}
		const auto alignment = std::min(4u, format.byte_size);
		if (descriptor.Stride() < format.byte_size ||
		    (descriptor.Stride() & (alignment - 1u)) != 0u ||
		    (descriptor.Base48() & (alignment - 1u)) != 0u)
			return SpecializationFail("indirect formatted buffers require aligned full-format records");
	} else if ((!indexed && descriptor.Stride() != 1u) ||
	           (indexed && resource.max_byte_extent != 1u && descriptor.Stride() != 4u)) {
		return SpecializationFail("indirect buffer stores require exact linear DWORD or byte bounds");
	}
	return true;
}

bool IsBoundedDescriptorTable(const ResourcePlan& program,
                              const DescriptorSource::IndirectDescriptor& indirect) {
	const auto* table = Source(program, indirect.table_source);
	return indirect.sources.empty() && (!indirect.selector || indirect.table_scalar) &&
	       (indirect.key_count.IsEmpty() || indirect.table_maximum) && indirect.workgroup_axis == UINT32_MAX &&
	       table != nullptr && (table->dword_count == 4u || indirect.table_scalar);
}

bool CanReadIndexedSelector(const ShaderBufferResource& material, uint32_t offset) {
	if (material.Type() != 0u || material.SwizzleEnabled() || material.AddTid() ||
	    material.OutOfBounds() != 0u) return false;
	if (material.Format() == Prospero::BufferFormat::kInvalid || material.NumRecords() == 0u ||
	    offset >= material.Stride()) return true;
	return material.Base48() != 0u &&
	       ((material.Base48() | material.Stride() | offset) & 3u) == 0u &&
	       material.NumRecords() <= MaxIndirectDescriptorProbes &&
	       uint64_t {material.NumRecords() - 1u} * material.Stride() + offset + 4u <=
	           uint64_t {UINT32_MAX} + 1u;
}

bool EvaluateIndirectCount(const ResourcePlan& program, const DescriptorSource::IndirectDescriptor& indirect,
                           const SrtRuntime& runtime, uint32_t& count) {
	if (indirect.workgroup_axis != UINT32_MAX) {
		if (runtime.workgroup_counts.size() != 3u || indirect.workgroup_axis >= 3u) return false;
		count = std::ranges::find(runtime.workgroup_counts, 0u) != runtime.workgroup_counts.end()
		    ? 0u : runtime.workgroup_counts[indirect.workgroup_axis];
		return true;
	}
	if (!WalkedValue(program, indirect.key_count, count)) return false;
	if (indirect.signed_count && std::bit_cast<int32_t>(count) < 0) count = 0;
	return true;
}

bool ResolveSelectorKeys(const ResourcePlan& program, const DescriptorSource::IndirectDescriptor& indirect,
                         const DescriptorValue& material_value, const SrtRuntime& runtime,
                         std::vector<uint32_t>& keys) {
	const auto* selector = &*indirect.selector;
	keys.clear();
	const auto read_selector = [&](uint64_t base, uint64_t size, uint64_t offset,
	                               std::span<uint32_t> words) {
		return selector->global ? ReadTable(base, size, offset, runtime, words)
		                        : ReadScalarTable(base, size, offset, runtime, words);
	};
	const auto read_keys = [&](uint64_t base, uint64_t size, uint64_t first,
	                           uint64_t step, uint64_t count) {
		keys.resize(count);
		if (count == 0u) return true;
		if (step == 4u) {
			return read_selector(base, size, first, keys);
		}
		if (step < 4u) return false;
		for (uint64_t copied = 0; copied < count;) {
			const auto offset = first + copied * step;
			// Scalar reads align each strided offset independently.
			const auto bias = selector->global ? 0u : offset & 3u;
			const auto length = std::min(count - copied,
			    1u + (uint64_t {MaxIndirectDescriptorProbes - 1u} * 4u - bias) / step);
			keys.resize(count + (bias + (length - 1u) * step) / 4u + 1u);
			const auto batch = std::span(keys).subspan(count);
			if (!read_selector(base, size, offset, batch)) {
				keys.resize(count);
				return false;
			}
			for (uint64_t index = 0; index < length; ++index)
				keys[copied + index] = batch[(bias + index * step) / 4u];
			copied += length;
		}
		keys.resize(count);
		return true;
	};
	// Reads the key of each record whose rank is set.
	const auto read_ranks = [&](uint64_t base, uint64_t size, uint64_t first, uint64_t step,
	                            uint32_t ranks) {
		keys.reserve(std::popcount(ranks));
		for (; ranks != 0u; ranks &= ranks - 1u) {
			const auto offset = first + std::countr_zero(ranks) * step;
			uint32_t key = 0;
			if (offset > UINT32_MAX || !read_selector(base, size, offset, {&key, 1})) return false;
			keys.push_back(key);
		}
		return true;
	};
	if (selector->rows) {
		const auto& rows = *selector->rows;
		DescriptorValue count_value;
		ShaderBufferResource counts, material;
		if (!WalkedDescriptor(program, rows.source, count_value) ||
		    !DecodeBufferDescriptor(count_value, counts) ||
		    !DecodeBufferDescriptor(material_value, material) || selector->stride != 4u ||
		    selector->offset != 0u || rows.stride == 0u || (rows.stride & 3u) != 0u ||
		    counts.GetSize() / 4u > MaxIndirectDescriptorProbes) return false;
		std::vector<uint32_t> lengths(counts.GetSize() / 4u);
		if (!ReadScalarTable(counts.Base48(), counts.GetSize(), 0u, runtime, lengths)) return false;
		uint32_t total = 0;
		for (size_t row = 0; row < lengths.size(); ++row) {
			const auto length = lengths[row];
			if (length > MaxIndirectDescriptorProbes - total ||
			    (length != 0u && uint64_t {row} * rows.stride + uint64_t {length} * 4u >
			                         UINT32_MAX + 1ull)) return false;
			total += length;
		}
		keys.reserve(total);
		std::vector<uint32_t> batch;
		for (size_t row = 0; row < lengths.size();) {
			if (lengths[row] == 0u) { ++row; continue; }
			const auto first = uint64_t {row} * rows.stride / 4u;
			auto end = first + lengths[row];
			size_t next = row + 1u;
			for (; next < lengths.size(); ++next) {
				if (lengths[next] == 0u) continue;
				const auto candidate_end =
				    std::max(end, uint64_t {next} * rows.stride / 4u + lengths[next]);
				if (candidate_end - first > MaxIndirectDescriptorProbes) break;
				end = candidate_end;
			}
			// Coalesce small gaps, but interpret only selected prefixes. The specialization
			// reader captures the entire fetched span for writable-alias rejection.
			batch.resize(end - first);
			if (!ReadScalarTable(material.Base48(), material.GetSize(), first * 4u, runtime, batch))
				return false;
			for (; row < next; ++row) {
				if (lengths[row] == 0u) continue;
				const auto begin = batch.begin() + (uint64_t {row} * rows.stride / 4u - first);
				keys.insert(keys.end(), begin, begin + lengths[row]);
			}
		}
	} else if (selector->bitset) {
		ShaderBufferResource material;
		std::array<uint32_t, 8> storage {};
		const auto record_words = std::min<size_t>(selector->stride / 4u, storage.size());
		const auto masks = std::span(storage).first(record_words);
		uint32_t first = 0, count = 0;
		if (!DecodeBufferDescriptor(material_value, material) ||
		    !WalkedValue(program, indirect.selector_first, first) ||
		    !EvaluateIndirectCount(program, indirect, runtime, count) || count != masks.size() * 32u ||
		    selector->stride != masks.size_bytes() || selector->offset != 0u ||
		    material.Type() != 0u || material.Stride() != selector->stride ||
		    material.SwizzleEnabled() || material.AddTid() || material.OutOfBounds() != 0u ||
		    (material.Base48() & 3u) != 0u) return false;
		if (material.Format() != Prospero::BufferFormat::kInvalid) {
			const auto words = material.GetSize() / sizeof(uint32_t);
			// Bitmap storage can grow with resolution while its candidate domain stays at 32 keys
			// per record word. Bound scratch memory, preserving each coherent read's capture.
			for (uint64_t first_word = 0; first_word < words; first_word += keys.size()) {
				keys.resize(std::min<uint64_t>(words - first_word, MaxIndirectDescriptorProbes));
				if (!ReadScalarTable(material.Base48(), material.GetSize(), first_word * 4u,
				                     runtime, keys)) return false;
				for (size_t word = 0; word < keys.size(); ++word) masks[word % masks.size()] |= keys[word];
				if (std::ranges::all_of(masks, [](uint32_t mask) { return mask == UINT32_MAX; })) break;
			}
			keys.clear();
			for (uint32_t word = 0; word < masks.size(); ++word) {
				auto mask = masks[word];
				while (mask != 0u) {
					const auto rank = word * 32u + std::countr_zero(mask);
					keys.push_back(first + count - 1u - rank);
					mask &= mask - 1u;
				}
			}
		}
	} else if (!selector->byte_ranges.empty()) {
		if (material_value.dword_count != 2u) return false;
		const auto base = (uint64_t {material_value.dwords[1]} << 32u) | material_value.dwords[0];
		std::array<bool, 256> selected {};
		for (const auto [offset, size]: selector->byte_ranges) {
			if (size > MaxIndirectDescriptorProbes || ((offset | size) & 3u) != 0u) return false;
			keys.resize(size / 4u);
			if (!ReadScalarTable(base, UINT64_MAX, offset, runtime, keys)) return false;
			for (const auto word: keys)
				for (uint32_t shift = 0; shift < 32u; shift += 8u)
					selected[(word >> shift) & 255u] = true;
		}
		keys.clear();
		for (uint32_t key = 0; key < selected.size(); ++key)
			if (selected[key]) keys.push_back(key);
	} else if (!indirect.selector_first.IsEmpty()) {
		ShaderBufferResource material;
		uint64_t             base = 0, size = UINT64_MAX;
		uint32_t             first = 0, count = 0;
		if (material_value.dword_count == 2u) {
			if (selector->stride == 0u) return false;
			base = (uint64_t {material_value.dwords[1]} << 32u) | material_value.dwords[0];
		} else {
			if (!DecodeBufferDescriptor(material_value, material)) return false;
			base = material.Base48();
			size = material.GetSize();
		}
		if (!WalkedValue(program, indirect.selector_first, first) ||
		    !EvaluateIndirectCount(program, indirect, runtime, count) || count > MaxIndirectDescriptorProbes)
			return false;
		// SMEM uses its instruction's byte stride; V# stride only supplies its bounds.
		const auto step = selector->stride != 0u ? selector->stride : material.Stride();
		if (selector->stride == 0u &&
		    (material.Type() != 0u || material.SwizzleEnabled() || material.AddTid() ||
		     material.OutOfBounds() != 0u || uint64_t {selector->offset} + 4u > step ||
		     uint64_t {first} + count > material.NumRecords())) return false;
		const auto last = count != 0u ? uint64_t {first} + count - 1u : 0u;
		if (last > UINT32_MAX) return false;
		// Out-of-bounds scalar reads return zero.
		const auto end = count != 0u ? last * step + selector->offset + 4u : 0u;
		if (end > uint64_t {UINT32_MAX} + 1u) return false;
		const auto start = uint64_t {first} * step + selector->offset;
		if (selector->ranks) {
			// A scan selects the ranks some word of its mask's address class sets.
			const auto& mask = *selector->ranks;
			DescriptorValue mask_value;
			ShaderBufferResource masks;
			if (count > 32u || !WalkedDescriptor(program, mask.source, mask_value) ||
			    !DecodeBufferDescriptor(mask_value, masks)) return false;
			// Each chunk holds whole strides of the power-of-two class, or one of its words.
			const auto words = mask.stride / 4u;
			const auto chunk =
			    words <= MaxIndirectDescriptorProbes ? MaxIndirectDescriptorProbes : 1u;
			uint32_t ranks = 0;
			for (auto at = uint64_t {mask.offset}; at + 4u <= masks.GetSize();
			     at += uint64_t {std::max(chunk, words)} * 4u) {
				keys.resize(std::min<uint64_t>((masks.GetSize() - at) / 4u, chunk));
				if (!ReadScalarTable(masks.Base48(), masks.GetSize(), at, runtime, keys))
					return false;
				for (size_t word = 0; word < keys.size(); word += words) ranks |= keys[word];
			}
			keys.clear();
			if (count < 32u) ranks &= (1u << count) - 1u;
			if (!read_ranks(base, size, start, step, ranks)) return false;
		} else if (!read_keys(base, size, start, step, count)) {
			return false;
		}
	} else if (!indirect.selector_mask.IsEmpty()) {
		uint32_t mask = 0;
		uint32_t count = 0;
		if (material_value.dword_count != 2u ||
		    !WalkedValue(program, indirect.selector_mask, mask) ||
		    !EvaluateIndirectCount(program, indirect, runtime, count) || count == 0u || count > 32u) {
			return false;
		}
		if (count < 32u) mask &= (1u << count) - 1u;
		const auto material_base =
		    (static_cast<uint64_t>(material_value.dwords[1]) << 32u) | material_value.dwords[0];
		if (!read_ranks(material_base, UINT64_MAX, selector->offset, selector->stride, mask))
			return false;
	} else if (selector->stride == 0u) {
		ShaderBufferResource material;
		if (!DecodeBufferDescriptor(material_value, material) ||
		    !CanReadIndexedSelector(material, selector->offset)) return false;
		const auto records = material.NumRecords();
		if (material.Format() != Prospero::BufferFormat::kInvalid && records > selector->first_record &&
		    selector->offset < material.Stride() &&
		    !read_keys(material.Base48(), material.GetSize(),
		               uint64_t {selector->first_record} * material.Stride() + selector->offset,
		               uint64_t {selector->record_step} * material.Stride(),
		               (records - selector->first_record - 1u) / selector->record_step + 1u)) return false;
		keys.push_back(0u); // Every out-of-bounds indexed load returns zero.
	} else {
		ShaderBufferResource material;
		if (!DecodeBufferDescriptor(material_value, material) || selector->stride < 4u ||
		    !std::has_single_bit(selector->stride) || (selector->offset & 3u) != 0u)
			return false;
		// Empty selector bounds denote the complete U32 congruence class. SMEM
		// adds its aligned immediate after the dynamic offset, without U32 wrap.
		const auto domain_count = (uint64_t {1} << 32u) / selector->stride;
		const auto available = material.GetSize() > selector->offset
		    ? material.GetSize() - selector->offset : 0u;
		const auto count = std::min(domain_count,
		    available < 4u ? 0u : (available - 4u) / selector->stride + 1u);
		if (count > MaxIndirectDescriptorProbes ||
		    !read_keys(material.Base48(), material.GetSize(), selector->offset, selector->stride, count)) return false;
		if (count < domain_count) keys.push_back(0u); // An out-of-bounds scalar read returns zero.
	}
	if (selector->byte_ranges.empty()) {
		for (auto& key: keys) {
			auto field = (key >> selector->bit_offset) & selector->bit_mask;
			if (selector->non_negative && static_cast<int32_t>(field) < 0) field = 0u;
			key = std::min(field, selector->maximum);
		}
		if (selector->excluded) std::erase(keys, *selector->excluded);
		std::ranges::sort(keys);
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	}
	return true;
}

template <typename Specialization, typename Normalize>
bool MaterializeIndirectDescriptor(const ResourcePlan&                         program,
                                   uint32_t source_index,
                                   uint32_t resource_index, uint32_t dword_count, uint32_t read_dwords,
                                   const SrtRuntime& runtime, ResourceSnapshot& snapshot,
                                   std::vector<DescriptorValue>& descriptors,
                                   std::vector<Specialization>&  specializations,
                                   Normalize&& normalize) {
	const auto& indirect = *program.descriptor_sources[source_index].indirect_descriptor;
	const auto  descriptor_bytes = read_dwords * sizeof(uint32_t);
	const auto& sources = indirect.sources;
	const auto* selector = indirect.selector ? &*indirect.selector : nullptr;
	const bool byte_keys = IsBoundedDescriptorTable(program, indirect);
	auto* selector_keys = selector != nullptr
	    ? &program.selector_keys[program.selector_groups[source_index]] : nullptr;
	auto& keys = selector_keys != nullptr ? selector_keys->values : program.material_keys;
	auto& byte_offsets = program.material_keys;
	const auto children_begin = descriptors.size();
	const auto root_resource = specializations[resource_index];
	// Ordinals of the distinct candidates: tables of tens of thousands of descriptors (the material
	// tables of ray tracing hit groups) make a linear search quadratic.
	std::unordered_map<DescriptorValue, uint32_t, DescriptorValueHash> ordinals;
	const auto intern_candidate = [&](const DescriptorValue& candidate) {
		if (candidate == descriptors[resource_index]) return 0u;
		const auto [found, inserted] = ordinals.try_emplace(
		    candidate, static_cast<uint32_t>(descriptors.size() - children_begin + 1u));
		if (inserted) {
			descriptors.push_back(candidate);
			auto child = root_resource;
			child.indirect_root = resource_index;
			specializations.push_back(child);
		}
		return found->second;
	};
	uint64_t table_base = 0;
	uint64_t table_size = UINT64_MAX; // Scalar addresses have no buffer descriptor bounds.
	DescriptorValue table_value;
	ShaderBufferResource table;
	const auto failed = [&](std::string_view reason, std::string_view details = {}) {
		const auto* pointer = indirect.pointer_table ? &*indirect.pointer_table : nullptr;
		return SpecializationFail(fmt::format(
		    "indirect {} resource={} source={} table_source={} words={:08x},{:08x},{:08x},{:08x} "
		    "base={:#x} size={} type={} runtime_stride={} records={} swizzle={} addtid={} oob={} "
		    "maximum={} offset={} immediate={} record_bytes={} child_count={} child_stride={} "
		    "child_offset={} indexed_pointer={} candidates={} {}",
		    reason, resource_index, source_index, indirect.table_source,
		    table_value.dwords[0], table_value.dwords[1], table_value.dwords[2], table_value.dwords[3],
		    table_base, table_size, table.Type(), table.Stride(), table.NumRecords(),
		    table.SwizzleEnabled(), table.AddTid(), table.OutOfBounds(),
		    indirect.table_maximum ? fmt::format("{}", *indirect.table_maximum) : "none",
		    indirect.table_offset, indirect.table_immediate, indirect.table_record_bytes,
		    pointer != nullptr ? pointer->count : 1u, pointer != nullptr ? pointer->stride : 0u,
		    pointer != nullptr ? pointer->offset : 0u, pointer != nullptr && pointer->indexed,
		    descriptors.size(), details));
	};
	const auto read_failed = [&](std::string_view reason, uint32_t row, uint32_t child,
	                             uint64_t base, uint64_t offset, size_t bytes) {
		return failed(reason, fmt::format("row={} child={} read_base={:#x} read_offset={:#x} "
		                                 "read_address={:#x} requested_bytes={}", row, child, base, offset,
		    (base & AddressMask & ~uint64_t {3}) + (offset & ~uint64_t {3}), bytes));
	};
	const auto normalization_failed = [&](uint64_t offset, uint32_t key, uint64_t step,
	                                      std::string_view key_kind) {
		return failed("candidate", fmt::format("candidate_offset={:#x} {}={} stride={} step={}",
		    offset & ~uint64_t {3}, key_kind, key, indirect.table_stride, step));
	};
	if (indirect.pointer_table && indirect.pointer_table->buffer) {
		// Records named by the rows of a pointer table hold the V# of a descriptor table. The
		// mapping searches the first word of that V# (the shader keys on the word it reads) for
		// the block of its table's elements: [count, ordinal of element 0, 1, ...]. Rows name few
		// distinct records, and records may share one table.
		const auto& pointer = *indirect.pointer_table;
		if (!WalkedDescriptor(program, indirect.table_source, table_value) ||
		    !DecodeBufferDescriptor(table_value, table)) return false;
		table_base = table.Base48();
		table_size = table.GetSize();
		descriptors[resource_index] = {.dword_count = dword_count};
		const uint64_t first = uint64_t {indirect.table_offset & ~3u} + (indirect.table_immediate & ~3u);
		const uint64_t rows = table_size >= first + 8u
		    ? (table_size - first - 8u) / indirect.table_stride + 1u : 0u;
		std::vector<uint32_t> words(table_size / 4u);
		if (rows != 0u && !ReadScalarTable(table_base, table_size, 0u, runtime, words))
			return read_failed("pointer table read", 0u, 0u, table_base, 0u, table_size);
		// Only rows that name the reading function hold records of its layout. The function's
		// address is in one other aligned pointer slot of the row.
		const auto row_word = [&](uint64_t row, uint64_t byte) {
			const auto word = (row * indirect.table_stride + byte) / 4u;
			return (uint64_t {words[word + 1u]} << 32u) | words[word];
		};
		std::optional<uint64_t> function_slot;
		if (pointer.function != 0u) {
			for (uint64_t slot = 0; slot + 8u <= indirect.table_stride && !function_slot; slot += 8u) {
				if (slot == (indirect.table_immediate & ~7u)) continue;
				for (uint64_t row = 0; row < rows; ++row) {
					if (((indirect.table_offset & ~3u) + row * indirect.table_stride + slot + 8u) > table_size) break;
					if ((row_word(row, (indirect.table_offset & ~3u) + slot) & AddressMask) ==
					    (pointer.function & AddressMask)) {
						function_slot = slot;
						break;
					}
				}
			}
			if (!function_slot) return failed("record function", fmt::format("function={:#x}", pointer.function));
		}
		// The function and the targets that share its inlined code.
		std::unordered_set<uint64_t> functions {pointer.function & AddressMask};
		for (const auto alias: pointer.functions) functions.insert(alias & AddressMask);
		std::vector<uint64_t> records;
		for (uint64_t row = 0; row < rows; ++row) {
			if (function_slot &&
			    ((indirect.table_offset & ~3u) + row * indirect.table_stride + *function_slot + 8u > table_size ||
			     !functions.contains(row_word(row, (indirect.table_offset & ~3u) + *function_slot) & AddressMask))) continue;
			const auto word = (first + row * indirect.table_stride) / 4u;
			const auto record = ((uint64_t {words[word + 1u]} << 32u) | words[word]) & AddressMask;
			if (record != 0u) records.push_back(record);
		}
		std::ranges::sort(records);
		records.erase(std::unique(records.begin(), records.end()), records.end());
		std::vector<std::pair<uint32_t, uint32_t>> entries; // V# first word, block offset.
		struct KeyedTable {
			uint64_t base  = 0;
			uint64_t count = 0;
			size_t   entry = 0;
		};
		std::unordered_map<uint32_t, KeyedTable> keyed_tables;
		std::unordered_set<DescriptorValue, DescriptorValueHash> tables; // the V# of each entry
		std::vector<uint32_t> blocks;
		// Sampled roots keep their candidates in the host's resident image heap: the blocks hold
		// heap entries instead of ordinals of materialized descriptors, so neither the number of
		// candidates nor their order reaches the shader's descriptor layout.
		bool heap_list = false;
		if constexpr (std::is_same_v<Specialization, ResourceSpecialization::Image>) {
			const auto& image = program.info.images[resource_index];
			heap_list = runtime.image_heap != nullptr && dword_count == 8u &&
			            image.resource_class == ImageResourceClass::Sampled &&
			            image.mip_mode != ImageMipMode::Dynamic;
		}
		std::vector<uint32_t>        element_words;
		std::vector<DescriptorValue> heap_values;
		std::vector<size_t>          heap_blocks; // The block word of each heap value.
		for (const auto record: records) {
			DescriptorValue record_table {.dword_count = 4u};
			if (!ReadScalarTable(record, UINT64_MAX, pointer.offset, runtime,
			                     std::span(record_table.dwords).first(4u)))
				return read_failed("record table read", 0u, 0u, record, pointer.offset, 16u);
			const std::array<uint32_t, 4> table_words {record_table.dwords[0], record_table.dwords[1],
			                                           record_table.dwords[2], record_table.dwords[3]};
			if (!tables.insert(record_table).second) continue;
			ShaderBufferResource elements;
			if (!DecodeBufferDescriptor(record_table, elements)) return false;
			const uint64_t element_base = elements.Base48();
			const uint64_t element_size = elements.GetSize();
			const uint64_t count = pointer.element_stride == 0u ? 1u
			    : element_size >= pointer.element_offset + descriptor_bytes
			        ? (element_size - pointer.element_offset - descriptor_bytes) / pointer.element_stride + 1u
			        : 0u;
			// Rows of scene-wide pointer tables can name records that hold no table any more (the GPU
			// never selects them): a table outside guest memory gives the shader no elements, and its
			// key reads the null descriptor like any other key without a table.
			if (count != 0u && !TableInGuestMemory(element_base, element_size, runtime)) continue;
			if (count > MaxRecordTableElements)
				return failed("record table elements", fmt::format("record={:#x} count={}", record, count));
			const auto block = static_cast<uint32_t>(blocks.size());
			blocks.push_back(static_cast<uint32_t>(count));
			// One read of the whole table: large ones (a scene-wide sampler table of 720 thousand
			// entries) would otherwise take one capture entry per element.
			if (element_base != 0u && count > 1u) {
				element_words.resize((element_size + 3u) / 4u);
				if (!ReadScalarTable(element_base, element_size, 0u, runtime, element_words))
					return read_failed("record table read", 0u, 0u, element_base, 0u, element_size);
			}
			for (uint32_t element = 0; element < count; ++element) {
				const auto offset = pointer.element_offset + uint64_t {element} * pointer.element_stride;
				DescriptorValue candidate {.dword_count = dword_count};
				if (element_base != 0u && offset + descriptor_bytes <= element_size && count > 1u) {
					std::copy_n(element_words.begin() + static_cast<std::ptrdiff_t>(offset / 4u),
					            read_dwords, candidate.dwords.begin());
				} else if (element_base != 0u && offset + descriptor_bytes <= element_size &&
				    !ReadScalarTable(element_base, element_size, offset, runtime,
				                     std::span(candidate.dwords).first(read_dwords)))
					return read_failed("record descriptor read", 0u, element, element_base, offset, descriptor_bytes);
				if (!normalize(candidate)) return normalization_failed(offset, element, pointer.element_stride, "element");
				if (heap_list) {
					heap_blocks.push_back(blocks.size());
					heap_values.push_back(candidate);
					blocks.push_back(0u);
				} else {
					blocks.push_back(intern_candidate(candidate));
				}
			}
			// Tables at one base (a scene-wide table and a range of it) give the shader the same
			// elements for the keys both cover: the longer one serves both.
			const auto [same_key, inserted] =
			    keyed_tables.try_emplace(table_words[0], KeyedTable {element_base, count, entries.size()});
			if (inserted) {
				entries.emplace_back(table_words[0], block);
			} else if (same_key->second.base != element_base) {
				return failed("record tables", "two record tables share the first word of their V#");
			} else if (count > same_key->second.count) {
				same_key->second.count                 = count;
				entries[same_key->second.entry].second = block;
			}
		}
		ImageHeapView heap_view;
		if (heap_list) {
			heap_list = runtime.image_heap->AcquireList(
			    {.owner = &program, .resource = resource_index,
			     .root = &program.info.images[resource_index], .values = heap_values},
			    heap_view);
			const auto heap_kinds = heap_view.kinds;
			// Candidate i selects slot i + 1 of the root's list.
			for (size_t i = 0; i < heap_blocks.size(); ++i) {
				blocks[heap_blocks[i]] = heap_list ? static_cast<uint32_t>(i + 1u)
				                                   : intern_candidate(heap_values[i]);
			}
			if (heap_list) {
				for (const auto& kind: heap_kinds) {
					descriptors.push_back(kind);
					auto child          = root_resource;
					child.indirect_root = resource_index;
					specializations.push_back(child);
				}
			}
		}
		// Every element null: the root's null descriptor serves every key.
		if (descriptors.size() == children_begin) return true;
		std::ranges::sort(entries);
		for (size_t i = 1; i < entries.size(); ++i) {
			if (entries[i].first == entries[i - 1u].first)
				return failed("record tables", "two record tables share the first word of their V#");
		}
		if (entries.empty()) return true;
		const bool stable_layout = g_stable_indirect_layouts.load(std::memory_order_relaxed);
		const auto key_capacity  = !stable_layout ? entries.size() : std::max<size_t>(
		    program.stage == ShaderType::Compute ? g_compute_indirect_keys.load(std::memory_order_relaxed)
		                                         : MinIndirectMappingCapacity,
		    std::bit_ceil(entries.size()));
		// A list's slot count and device address precede the mapping (see EmitImageHeapEntry()).
		const auto list_offset    = snapshot.flattened_srt.size();
		const auto mapping_offset = list_offset + (heap_list ? 3u : 0u);
		snapshot.flattened_srt.resize(mapping_offset + 1u + key_capacity * 2u, 0u);
		if (heap_list) {
			snapshot.flattened_srt[list_offset]      = heap_view.slot_count;
			snapshot.flattened_srt[list_offset + 1u] = static_cast<uint32_t>(heap_view.slots);
			snapshot.flattened_srt[list_offset + 2u] = static_cast<uint32_t>(heap_view.slots >> 32u);
		}
		snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(entries.size());
		for (size_t entry = 0; entry < entries.size(); ++entry) {
			snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = entries[entry].first;
			// The block's offset in the root's blocks until FlushRecordBlocks() adds where they land.
			snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = entries[entry].second;
		}
		// The blocks follow the candidate counts, which streaming changes, while flattened SRT
		// offsets reach the shader as constants: they go after every other region, so their size
		// moves no constant (mapping entries name them by data).
		t_record_blocks.push_back({mapping_offset, entries.size(), std::move(blocks)});
		// Padded children keep the descriptor arrays the same size while candidate counts change.
		if (((std::is_same_v<Specialization, ResourceSpecialization::Image> && !heap_list) ||
		     std::is_same_v<Specialization, ResourceSpecialization::Sampler>) && stable_layout) {
			const auto children = descriptors.size() - children_begin;
			const auto total    = std::max<size_t>(
                program.stage == ShaderType::Compute
                    ? g_compute_indirect_images.load(std::memory_order_relaxed)
                    : 1u,
                std::bit_ceil(children + 1u));
			for (auto padded = children; padded + 1u < total; ++padded) {
				descriptors.push_back(descriptors.back());
				specializations.push_back(specializations.back());
			}
		}
		auto& root                      = specializations[resource_index];
		root.indirect_root              = resource_index;
		root.indirect_mapping_offset    = static_cast<uint32_t>(mapping_offset);
		root.indirect_search_iterations = std::bit_width(key_capacity);
		if constexpr (std::is_same_v<Specialization, ResourceSpecialization::Image>) {
			root.heap = heap_list;
			if (heap_list) {
				for (auto child = children_begin; child < specializations.size(); ++child) {
					specializations[child].heap = true;
				}
			}
		}
		return true;
	}
	if (sources.empty()) {
		if (selector == nullptr) keys.clear();
		DescriptorValue material_value;
		if ((selector != nullptr && (byte_keys || !selector_keys->resolved) &&
		     !WalkedDescriptor(program, selector->source, material_value)) ||
		    !WalkedDescriptor(program, indirect.table_source, table_value)) {
			return false;
		}
		if (table_value.dword_count == 2u) {
			table_base = (static_cast<uint64_t>(table_value.dwords[1]) << 32u) | table_value.dwords[0];
			// A bounded address table's heap spans the slots of keys 0 through its maximum.
			if (byte_keys) {
				table_base += uint64_t {indirect.table_offset} + (indirect.table_immediate & ~3u);
				table_size = (uint64_t {*indirect.table_maximum} + 1u) * indirect.table_stride;
			}
		} else if (DecodeBufferDescriptor(table_value, table)) {
			table_base = table.Base48();
			table_size = table.GetSize();
		} else {
			return false;
		}
		if (byte_keys && table_size > UINT32_MAX) return failed("table geometry");
		bool use_selector = selector != nullptr;
		if (byte_keys && selector != nullptr) {
			ShaderBufferResource material;
			if (!DecodeBufferDescriptor(material_value, material)) return false;
			use_selector = CanReadIndexedSelector(material, selector->offset);
			if (use_selector && material.Format() != Prospero::BufferFormat::kInvalid &&
			    material.NumRecords() != 0u && selector->offset < material.Stride()) {
				const auto begin = material.Base48() + selector->offset;
				const auto end = begin + uint64_t {material.NumRecords() - 1u} * material.Stride() + 4u;
				for (uint32_t i = 0; i < program.info.buffers.size(); ++i) {
					const auto& buffer = program.info.buffers[i];
					if (!buffer.written) continue;
					ShaderBufferResource written;
					if (program.descriptor_sources[buffer.source].indirect_descriptor ||
					    !DecodeBufferDescriptor(snapshot.buffers[i], written) ||
					    (written.Base48() != 0u && written.GetSize() != 0u &&
					     begin < written.Base48() + written.GetSize() &&
					     written.Base48() < end)) {
						use_selector = false;
						break;
					}
				}
			}
		}
		if (byte_keys && !use_selector) {
			if (!indirect.table_scalar && table.Format() == Prospero::BufferFormat::kInvalid) {
				descriptors[resource_index] = {.dword_count = dword_count};
				return true;
			}
			if (indirect.table_record_bytes != 0u &&
			     (table.Type() != 0u || table.SwizzleEnabled() || table.AddTid() ||
			      table.OutOfBounds() != 0u || (table_base & 3u) != 0u ||
			      (table.Stride() & 3u) != 0u || table.Stride() < indirect.table_record_bytes))
				return failed("table geometry");
			const auto* pointer = indirect.pointer_table ? &*indirect.pointer_table : nullptr;
			const bool indexed_pointer = pointer != nullptr && pointer->indexed;
			const auto child_count = pointer != nullptr ? pointer->count : 1u;
			uint64_t domain = indirect.table_maximum ? uint64_t {*indirect.table_maximum} + 1u
			                                       : uint64_t {1} << 32u;
			if (!indirect.key_count.IsEmpty()) {
				uint32_t key_count = 0u;
				if (!EvaluateIndirectCount(program, indirect, runtime, key_count)) return false;
				domain = std::min(domain, uint64_t {key_count});
			}
			if (indexed_pointer &&
			    (!indirect.table_maximum || table.Type() != 0u || table.SwizzleEnabled() || table.AddTid() ||
			     table.OutOfBounds() != 0u || (table_base & 3u) != 0u || (table.Stride() & 3u) != 0u ||
			     uint64_t {indirect.table_immediate} + 8u > table.Stride() ||
			     domain > table.NumRecords()))
				return failed("indexed pointer geometry", fmt::format("rows={}", domain));
			const bool affine = indirect.table_stride != 0u || indexed_pointer;
			const bool image_heap = dword_count == 8u && pointer == nullptr;
			const uint64_t step = indexed_pointer ? table.Stride()
			    : indirect.table_maximum ? indirect.table_stride
			    : affine ? std::max(uint64_t {1} << std::countr_zero(indirect.table_stride), uint64_t {4})
			             : 4u;
			uint64_t first = 0u;
			if (table_value.dword_count != 2u)
				first = ((indirect.table_maximum ? indirect.table_offset : indirect.table_offset % step) &
				         ~uint64_t {3}) + (indirect.table_immediate & ~3u);
			if (!indirect.table_maximum) domain /= step;
			const auto count = table_size >= first + 4u
			    ? std::min((table_size - first - 4u) / step + 1u, domain)
			    : 0u;
			descriptors[resource_index] = {.dword_count = dword_count};
			if constexpr (std::is_same_v<Specialization, ResourceSpecialization::Image>) {
				const auto& image = program.info.images[resource_index];
				// A resident heap maps slot words instead of materializing its entries.
				ImageHeapView view;
				if (image_heap && runtime.image_heap != nullptr &&
				    image.resource_class == ImageResourceClass::Sampled &&
				    image.mip_mode != ImageMipMode::Dynamic &&
				    runtime.image_heap->Acquire({table_base, table_size, step, first, count, &image},
				                                view)) {
					snapshot.specialization_reads.emplace_back(table_base, table_size);
					if (view.kinds.empty()) return true;
					auto root = root_resource;
					root.indirect_root = resource_index;
					root.indirect_mapping_offset = static_cast<uint32_t>(snapshot.flattened_srt.size());
					root.heap = true;
					specializations[resource_index] = root;
					snapshot.flattened_srt.insert(snapshot.flattened_srt.end(),
					    {view.slot_count, static_cast<uint32_t>(view.slots), static_cast<uint32_t>(view.slots >> 32u)});
					for (const auto& kind: view.kinds) {
						descriptors.push_back(kind);
						specializations.push_back(root);
					}
					return true;
				}
			}
			if (child_count == 0u || (!image_heap && count * child_count > MaxIndirectDescriptorProbes))
				return failed("candidate count", fmt::format("rows={} limit={}", count, MaxIndirectDescriptorProbes));
			// Affine SMEM offsets wrap in U32 before adding the aligned immediate.
			// Image heaps retain one bulk snapshot and their absolute byte-offset mapping.
			auto& heap_words = program.material_keys;
			if (image_heap) {
				heap_words.resize(table_size / 4u);
				if (!ReadScalarTable(table_base, table_size, 0u, runtime, heap_words))
					return read_failed("image heap read", 0u, 0u, table_base, 0u, heap_words.size() * sizeof(uint32_t));
			}
			const auto mapping_offset = snapshot.flattened_srt.size();
			snapshot.flattened_srt.resize(mapping_offset + 2u, 0u);
			for (uint32_t word = 0; word < count; ++word) {
				const auto table_offset = affine ? first + word * step : uint64_t {word} * 4u;
				uint64_t pointer_base = 0u;
				if (pointer != nullptr) {
					std::array<uint32_t, 2> address {};
					if (!ReadScalarTable(table_base, table_size, table_offset, runtime, address))
						return read_failed("parent pointer read", word, 0u, table_base, table_offset, sizeof(address));
					pointer_base = (uint64_t {address[1]} << 32u) | address[0];
				}
				for (uint32_t child = 0; child < child_count; ++child) {
					DescriptorValue candidate {.dword_count = dword_count};
					if (pointer != nullptr) {
						if (!ReadScalarTable(pointer_base, UINT64_MAX,
						        uint64_t {child} * pointer->stride + pointer->offset, runtime,
						        std::span(candidate.dwords).first(read_dwords)))
							return read_failed("child descriptor read", word, child, pointer_base,
							    uint64_t {child} * pointer->stride + pointer->offset, descriptor_bytes);
					} else if (!image_heap) {
						if (!ReadScalarTable(table_base, table_size, table_offset, runtime,
						                     std::span(candidate.dwords).first(read_dwords))) return false;
					} else {
						const auto table_word = table_offset / 4u;
						std::copy_n(heap_words.begin() + table_word,
						            std::min<size_t>(read_dwords, heap_words.size() - table_word),
						            candidate.dwords.begin());
					}
					if (!normalize(candidate))
						return normalization_failed(table_offset, word, step,
						                            indirect.table_maximum ? "key" : "probe");
					const auto ordinal = intern_candidate(candidate);
					if (ordinal == 0u) continue;
					const auto mapping_word = image_heap ? table_offset / 4u : word * child_count + child;
					snapshot.flattened_srt.resize(mapping_offset + 2u + mapping_word, 0u);
					snapshot.flattened_srt[mapping_offset + 1u + mapping_word] = ordinal;
				}
			}

			if (descriptors.size() == children_begin) {
				snapshot.flattened_srt.resize(mapping_offset);
			} else {
				snapshot.flattened_srt[mapping_offset] =
				    static_cast<uint32_t>(snapshot.flattened_srt.size() - mapping_offset - 1u);
				auto& root = specializations[resource_index];
				root.indirect_root = resource_index;
				root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
				root.indirect_search_iterations = 0u;
			}
			return true;
		}
		if (selector == nullptr) {
			uint32_t key_count = 0;
			uint32_t first_key = 0;
			if (!EvaluateIndirectCount(program, indirect, runtime, key_count)) return false;
			if (!indirect.selector_first.IsEmpty() && !WalkedValue(program, indirect.selector_first, first_key))
				return false;
			if (indirect.table_stride == 0u || key_count > MaxIndirectDescriptorProbes ||
			    (indirect.selector_first.IsEmpty() && key_count != 0u &&
			         uint64_t {indirect.table_offset} + indirect.table_immediate +
			         uint64_t {key_count - 1u} * indirect.table_stride + descriptor_bytes >
			             UINT32_MAX + 1ull)) {
				return false;
			}
			keys.resize(key_count);
			std::iota(keys.begin(), keys.end(), first_key);
			if (!keys.empty() && keys.back() < keys.front())
				std::rotate(keys.begin(), keys.begin() + (UINT32_MAX - first_key + 1ull), keys.end());
			if (indirect.workgroup_shift != 0u) {
				for (auto& key: keys)
					key = static_cast<uint32_t>(uint64_t {key} * indirect.workgroup_multiplier >>
					                            indirect.workgroup_shift);
				keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
			}
		} else {
			if (!indirect.selector_mask.IsEmpty() && table_value.dword_count != 2u) return false;
			if (!selector_keys->resolved) {
				if (!ResolveSelectorKeys(program, indirect, material_value, runtime, keys)) return false;
				selector_keys->resolved = true;
			}
		}
		if (byte_keys) {
			// Keep cached selector values independent of each destination table's
			// U32 arithmetic, and use the same byte keys as the full-heap GPU path.
			byte_offsets.clear();
			byte_offsets.reserve(keys.size());
			for (const auto key: keys) {
				const auto offset = uint64_t {(key * indirect.table_stride + indirect.table_offset) & ~3u} +
				                    (indirect.table_immediate & ~3u);
				byte_offsets.push_back(offset <= UINT32_MAX ? static_cast<uint32_t>(offset) : UINT32_MAX);
			}
			std::ranges::sort(byte_offsets);
			byte_offsets.erase(std::unique(byte_offsets.begin(), byte_offsets.end()), byte_offsets.end());
		}
	}

	const auto mapping_offset = snapshot.flattened_srt.size();
	const auto selected_keys = byte_keys ? std::span<const uint32_t>(byte_offsets)
	                                    : std::span<const uint32_t>(keys);
	const auto key_count = sources.empty() ? selected_keys.size() : sources.size();
	// The mapping's layout is part of the shader permutation (its offset in the flattened SRT
	// and the number of search steps), while the number of keys follows guest data from dispatch
	// to dispatch (GTA V's ray tracing shader saw dozens of permutations, each a multi-second
	// pipeline compile). Reserve room for a power-of-two number of keys instead: the shader
	// searches the runtime count, and later mappings keep their offsets until a count crosses a
	// power of two.
	// Compute shaders can keep one layout for more keys (see SetComputeIndirectFloors()).
	const bool stable_layout = g_stable_indirect_layouts.load(std::memory_order_relaxed);
	const auto key_capacity  = !stable_layout ? key_count : std::max<size_t>(
	    program.stage == ShaderType::Compute ? g_compute_indirect_keys.load(std::memory_order_relaxed)
	                                         : MinIndirectMappingCapacity,
	    std::bit_ceil(key_count));
	snapshot.flattened_srt.resize(mapping_offset + 1u + key_capacity * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(key_count);
	for (uint32_t entry = 0; entry < key_count; ++entry) {
		const auto key = sources.empty() ? selected_keys[entry] : entry;
		uint64_t table_offset = 0;
		DescriptorValue candidate;
		candidate.dword_count = dword_count;
		if (sources.empty()) {
			table_offset = byte_keys ? key
			    : uint64_t {(key * indirect.table_stride + indirect.table_offset) & ~3u} +
			          (indirect.table_immediate & ~3u);
			if (!(byte_keys && key == UINT32_MAX) &&
			    !ReadScalarTable(table_base, table_size, table_offset, runtime,
			                     std::span(candidate.dwords).first(read_dwords))) {
				return read_failed("table descriptor read", key, 0u, table_base, table_offset, descriptor_bytes);
			}
		} else if (!WalkedDescriptor(program, sources[entry], candidate)) {
			return false;
		}
		if (!normalize(candidate))
			return normalization_failed(table_offset, key, indirect.table_stride, "key");
		uint32_t ordinal = 0;
		if (entry == 0 && !byte_keys) {
			descriptors[resource_index] = candidate;
		} else {
			ordinal = intern_candidate(candidate);
		}
		snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = key;
		snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
	}
	if (descriptors.size() == children_begin) {
		snapshot.flattened_srt.resize(mapping_offset);
	} else {
		if (std::is_same_v<Specialization, ResourceSpecialization::Image> && stable_layout) {
			// The same for the distinct T#s: pad them with copies of the last one, which no key
			// selects, so that with the root they are a power of two (at least 8 for compute).
			const auto children = descriptors.size() - children_begin;
			const auto total    = std::max<size_t>(
                program.stage == ShaderType::Compute
                    ? g_compute_indirect_images.load(std::memory_order_relaxed)
                    : 1u,
                std::bit_ceil(children + 1u));
			for (auto padded = children;
			     padded + 1u < total; ++padded) {
				descriptors.push_back(descriptors.back());
				specializations.push_back(specializations.back());
			}
		}
		auto& root                      = specializations[resource_index];
		root.indirect_root              = resource_index;
		root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
		root.indirect_search_iterations = std::bit_width(key_capacity);
	}
	return true;
}

// Exemplar that specializes a table root: a T# with the root's dimension and a native format,
// preferring the float textures that sampled material tables mostly hold.
bool TableExemplar(const ImageResource& root, const DescriptorValue& descriptor, bool float_only) {
	if (NullImageDescriptor(descriptor) || DescriptorIsCube(descriptor) != root.cube) {
		return false;
	}
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	const auto numeric_class = Prospero::SampledTextureNumericClass(format);
	return DescriptorDimension(descriptor, root.dimension) == root.dimension &&
	       numeric_class != Prospero::TextureNumericClass::Unsupported &&
	       (!float_only || numeric_class == Prospero::TextureNumericClass::Float) &&
	       ImageConversionFormat(format) == Prospero::BufferFormat::kInvalid &&
	       !Prospero::IsFmaskTextureFormat(format);
}

uint32_t ImageTableCapacity(size_t slots) {
	return std::bit_ceil(std::max<uint32_t>(static_cast<uint32_t>(slots), 256u));
}

// A bindless table: the key the shader computes from GPU data selects entry key & entry_mask,
// so every entry the table holds is a candidate. Distinct valid T#s become descriptor slots and
// the per-entry slot mapping is appended to the flattened SRT.
bool MaterializeImageTable(const ResourcePlan& program,
                           const DescriptorSource::IndirectDescriptor& indirect,
                           const DescriptorValue& table_value, uint32_t image_index,
                           const SrtRuntime& runtime, ResourceSnapshot& snapshot,
                           ResourceSpecialization& specialization,
                           std::vector<ImageTableSnapshot>& previous,
                           std::vector<uint32_t>& mapping_offsets) {
	ShaderBufferResource table;
	if (table_value.dword_count != 4u || !DecodeBufferDescriptor(table_value, table) ||
	    (indirect.table_offset & 31u) != 0u) {
		return false;
	}
	const auto base       = table.Base48();
	const auto size       = table.GetSize();
	const auto entry_mask = indirect.table_mask >> 5u;
	const auto entries    = std::min<uint64_t>(
	    {size > indirect.table_offset ? (size - indirect.table_offset) / 32u : 0u,
	     uint64_t {entry_mask} + 1u, MaxIndirectDescriptorProbes});
	const auto& root = program.info.images[image_index];
	const auto same_table = [&](const ImageTableSnapshot& candidate) {
		return candidate.base == base && candidate.size == size &&
		       candidate.offset == indirect.table_offset && candidate.entry_mask == entry_mask;
	};
	auto found = std::ranges::find_if(snapshot.image_tables, same_table);
	if (found == snapshot.image_tables.end()) {
		ImageTableSnapshot next {.base = base, .size = size, .offset = indirect.table_offset,
		                         .entry_mask = entry_mask};
		// Tables span megabytes and rarely change: read into a reused buffer and keep the
		// previous snapshot's words when they are equal.
		static thread_local std::vector<uint32_t> words;
		words.resize(static_cast<size_t>(entries) * 8u);
		if (!words.empty() && !ReadScalarTable(base, size, indirect.table_offset, runtime, words)) {
			return false;
		}
		const auto reusable = std::ranges::find_if(previous, [&](const ImageTableSnapshot& old) {
			return same_table(old) && old.raw == words && !old.slots.empty();
		});
		// The tables this thread built last, by any program: resolutions ahead of execution
		// materialize into a ring of snapshots, so `previous` rarely holds the table.
		static thread_local std::array<ImageTableSnapshot, 4> built;
		static thread_local uint32_t                          built_next = 0;
		const auto cached = reusable != previous.end()
		                        ? built.end()
		                        : std::ranges::find_if(built, [&](const ImageTableSnapshot& old) {
			                          return same_table(old) && !old.slots.empty() &&
			                                 old.raw == words;
		                          });
		if (reusable != previous.end()) {
			next.raw      = std::move(reusable->raw);
			next.slots    = std::move(reusable->slots);
			next.mapping  = std::move(reusable->mapping);
			next.slots_id = std::exchange(reusable->slots_id, 0);
		} else if (cached != built.end()) {
			next.raw.swap(words);
			next.slots    = cached->slots;
			next.mapping  = cached->mapping;
			next.slots_id = cached->slots_id;
		} else {
			static std::atomic<uint64_t> next_slots_id {0};
			next.slots_id = ++next_slots_id;
			next.raw.swap(words);
			next.slots.push_back(DescriptorValue {.dword_count = 8u});
			next.mapping.assign(static_cast<size_t>(entries) + 1u, 0u);
			next.mapping[0] = static_cast<uint32_t>(entries);
			std::unordered_map<std::string_view, uint32_t> slot_of;
			for (uint32_t entry = 0; entry < entries; entry++) {
				DescriptorValue candidate {.dword_count = 8u};
				std::copy_n(next.raw.begin() + entry * 8u, 8u, candidate.dwords.begin());
				if (NullImageDescriptor(candidate) || !ValidImageDescriptor(candidate, root.r128)) {
					continue;
				}
				const std::string_view key(
				    reinterpret_cast<const char*>(next.raw.data() + entry * 8u),
				    8u * sizeof(uint32_t));
				const auto [slot, inserted] =
				    slot_of.try_emplace(key, static_cast<uint32_t>(next.slots.size()));
				if (inserted) {
					next.slots.push_back(candidate);
				}
				next.mapping[entry + 1u] = slot->second;
			}
			built[built_next++ % built.size()] = next;
		}
		snapshot.image_tables.push_back(std::move(next));
		mapping_offsets.push_back(static_cast<uint32_t>(snapshot.flattened_srt.size()));
		const auto& mapping = snapshot.image_tables.back().mapping;
		snapshot.flattened_srt.insert(snapshot.flattened_srt.end(), mapping.begin(), mapping.end());
		// The mapping follows the table's size, and later flattened SRT offsets reach the shader
		// as constants: a size rounded up to StableSrtWords keeps the program when the game
		// resizes the table a little.
		if (g_stable_indirect_layouts.load(std::memory_order_relaxed)) {
			snapshot.flattened_srt.resize(
			    mapping_offsets.back() + Common::AlignUp(mapping.size(), StableSrtWords), 0u);
		}
		found = snapshot.image_tables.end() - 1;
	}
	const auto table_index     = static_cast<uint32_t>(found - snapshot.image_tables.begin());
	auto&      image           = specialization.images[image_index];
	image.table                = table_index;
	image.table_capacity       = ImageTableCapacity(found->slots.size());
	image.table_mapping_offset = mapping_offsets[table_index];
	image.table_entry_mask     = entry_mask;
	snapshot.images[image_index] = {.dword_count = 8u};
	for (const bool float_only: {true, false}) {
		const auto exemplar = std::ranges::find_if(found->slots, [&](const DescriptorValue& slot) {
			return TableExemplar(root, slot, float_only);
		});
		if (exemplar != found->slots.end()) {
			snapshot.images[image_index] = *exemplar;
			break;
		}
	}
	return true;
}

} // namespace

bool ImageTableSlotCompatible(const ImageResource& root, const DescriptorValue& descriptor) {
	if (NullImageDescriptor(descriptor) || !ValidImageDescriptor(descriptor, root.r128) ||
	    DescriptorIsCube(descriptor) != root.cube) {
		return false;
	}
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	const auto dimension = DescriptorDimension(descriptor, root.dimension);
	const auto is_2d     = [](Decoder::ImageDimension value) {
		return value == Decoder::ImageDimension::Dim2D ||
		       value == Decoder::ImageDimension::Dim2DArray;
	};
	return (dimension == root.dimension || (is_2d(dimension) && is_2d(root.dimension))) &&
	       Prospero::SampledTextureNumericClass(format) == root.numeric_class &&
	       ImageConversionFormat(format) == root.conversion_format &&
	       !Prospero::IsFmaskTextureFormat(format);
}

struct SamplerPlan {
	struct Binding {
		uint32_t     source;
		SamplerClass type;
	};
	std::vector<std::array<uint32_t, 2>> mapping;
	std::vector<Binding>                bindings;
};

template <typename T, typename Keep>
void CompactImages(std::vector<T>& images, Keep&& keep) {
	size_t count = 0;
	for (size_t index = 0; index < images.size(); ++index) {
		if (!keep(index)) continue;
		if (count != index) images[count] = std::move(images[index]);
		++count;
	}
	images.resize(count);
}

struct ImageRemap {
	explicit ImageRemap(const ResourceSpecialization& specialization)
	    : indices(specialization.images.size()),
	      source_count(static_cast<uint32_t>(specialization.images.size())) {
		for (uint32_t index = 0; index < source_count; index++) {
			indices[index] = specialization.images[index].fmask ? UINT32_MAX : count++;
		}
	}

	uint32_t operator[](uint32_t index) const {
		EXIT_IF(index >= source_count);
		return indices[index];
	}

	template <typename T>
	void Apply(std::vector<T>& images) const {
		EXIT_IF(images.size() != source_count);
		if (count == source_count) {
			return;
		}
		CompactImages(images, [&](size_t index) { return indices[index] != UINT32_MAX; });
	}

private:
	std::vector<uint32_t> indices;
	uint32_t                                    source_count;
	uint32_t                                    count = 0;
};

static bool BuildResourceSpecialization(const ResourcePlan& program, ResourceSnapshot& snapshot,
                                        ResourceSpecialization& specialization) {
	for (uint32_t i = 0; i < specialization.images.size(); i++) {
		const auto& descriptor = snapshot.images[i];
		auto&       image      = specialization.images[i];
		const auto  base_index = i < program.info.images.size() ? i : image.indirect_root;
		if (base_index >= program.info.images.size()) {
			return SpecializationFail(fmt::format("image resource {} has an invalid root", i));
		}
		const auto& base = program.info.images[base_index];
		if (base.resource_class == ImageResourceClass::None ||
		    (base.atomic && base.resource_class != ImageResourceClass::Storage)) {
			return SpecializationFail(fmt::format("image resource {} has an invalid class", i));
		}
		image.mip_count = ImageMipCount(base, descriptor);
		// A null T# reads zero; it keeps the dimension and cube its instructions declare.
		if (NullImageDescriptor(descriptor)) {
			image.numeric_class = base.atomic ? Prospero::TextureNumericClass::Uint
			                                  : Prospero::TextureNumericClass::Float;
			// A shader without a declared dimension gets a plain 2D view, as before.
			if (image.dimension == Decoder::ImageDimension::Unknown) {
				image.dimension = Decoder::ImageDimension::Dim2D;
				image.cube      = false;
			}
			continue;
		}
		const auto descriptor_dimension = DescriptorDimension(descriptor, base.dimension);
		if (descriptor_dimension == Decoder::ImageDimension::Unknown) {
			return SpecializationFail(fmt::format(
			    "image descriptor {} has unsupported type {}: {:08x},{:08x},{:08x},{:08x},"
			    "{:08x},{:08x},{:08x},{:08x}",
			    i, (descriptor.dwords[3] >> 28u) & 0xfu, descriptor.dwords[0], descriptor.dwords[1],
			    descriptor.dwords[2], descriptor.dwords[3], descriptor.dwords[4],
			    descriptor.dwords[5], descriptor.dwords[6], descriptor.dwords[7]));
		}
		image.dimension = descriptor_dimension;
		image.cube      = DescriptorIsCube(descriptor);
		const auto format =
		    static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
		if (base.atomic &&
		    (base.atomic64 ? format != Prospero::BufferFormat::k32_32UInt
		                   : format != Prospero::BufferFormat::k32UInt &&
		                         format != Prospero::BufferFormat::k32SInt &&
		                         format != Prospero::BufferFormat::k32Float)) {
			return SpecializationFail(
			    fmt::format("atomic image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
		const bool storage      = base.resource_class == ImageResourceClass::Storage;
		image.fmask             = Prospero::IsFmaskTextureFormat(format);
		if (image.fmask) {
			if (storage || base.depth_compare ||
			    image.indirect_root != ImageResource::NoIndirectImage ||
			    std::ranges::any_of(program.info.sampled_pairs,
			                        [&](const auto& pair) { return pair.image == i; })) {
				return SpecializationFail("FMASK requires a direct image load");
			}
		}
		// See ReadsDepthBits; raw loads only, since filtering would blend the codes.
		const bool depth_bits =
		    !storage && base.unfiltered_access_only && format == Prospero::BufferFormat::k16UInt &&
		    static_cast<Prospero::TileMode>((descriptor.dwords[3] >> 20u) & 0x1fu) ==
		        Prospero::TileMode::kDepth;
		image.conversion_format = depth_bits ? format : ImageConversionFormat(format);
		// SKIP_DEGAMMA (S# bit 31) reads sRGB texels as stored.
		image.skip_degamma = Prospero::IsSrgbFormat(format) &&
		    std::ranges::any_of(program.info.sampled_pairs, [&](const auto& pair) {
			    return pair.image == base_index && pair.sampler < snapshot.samplers.size() &&
			           (snapshot.samplers[pair.sampler].dwords[0] >> 31u) != 0u;
		    });
		// The shader decodes direct 8 and 8_8 sRGB textures where the device has no sRGB view.
		if (!storage && !base.depth_compare && !image.skip_degamma &&
		    image.indirect_root == ImageResource::NoIndirectImage &&
		    (format == Prospero::BufferFormat::k8Srgb || format == Prospero::BufferFormat::k8_8Srgb)) {
			image.srgb_decode = format;
		}
		if (storage || image.conversion_format != Prospero::BufferFormat::kInvalid) {
			image.shader_swizzle = DescriptorImageSwizzle(descriptor);
		}
		const auto raw_sint_format = RawSignedStorageFormat(base, format);
		const bool raw_sint_storage = raw_sint_format != Prospero::BufferFormat::kInvalid;
		if (raw_sint_format == Prospero::BufferFormat::k16UInt) image.conversion_format = format;
		if (storage && image.conversion_format == Prospero::BufferFormat::kInvalid) {
			image.shader_swizzle = CanonicalStorageSwizzle(image.shader_swizzle, format);
		}
		image.numeric_class = Prospero::SampledTextureNumericClass(
		    depth_bits ? Prospero::BufferFormat::k16UNorm : Prospero::RemapTextureFormat(format));
		if (!storage && format == Prospero::BufferFormat::k11_11_10UNorm) {
			if (image.indirect_root != ImageResource::NoIndirectImage) {
				return SpecializationFail("packed UNorm sampling requires a direct image");
			}
			if (std::ranges::any_of(program.info.sampled_pairs,
			                       [&](const auto& pair) { return pair.image == i; }) &&
			    !ValidatePackedUnormSampling(program, snapshot, i)) return false;
		}
		if (storage) {
			if ((!raw_sint_storage && image.numeric_class == Prospero::TextureNumericClass::Sint) ||
			    image.numeric_class == Prospero::TextureNumericClass::Unsupported) {
				return SpecializationFail(
				    fmt::format("storage image descriptor {} uses unsupported format {}", i,
				                static_cast<uint32_t>(format)));
			}
			if (raw_sint_storage || base.atomic) {
				image.numeric_class = Prospero::TextureNumericClass::Uint;
			}
		} else if (image.numeric_class == Prospero::TextureNumericClass::Unsupported ||
		           (base.depth_compare &&
		            image.numeric_class != Prospero::TextureNumericClass::Float)) {
			auto message = fmt::format(
			    "sampled image descriptor {} uses unsupported format {}: root={} source={} "
			    "first_use_pc={:#x} r128={} words={:08x},{:08x},{:08x},{:08x},"
			    "{:08x},{:08x},{:08x},{:08x}",
			    i, static_cast<uint32_t>(format), base_index, base.source, base.first_use_pc,
			    base.r128, descriptor.dwords[0], descriptor.dwords[1], descriptor.dwords[2],
			    descriptor.dwords[3], descriptor.dwords[4], descriptor.dwords[5],
			    descriptor.dwords[6], descriptor.dwords[7]);
			const auto* source = Source(program, base.source);
			if (i >= program.info.images.size() && source != nullptr &&
			    source->indirect_descriptor &&
			    IsBoundedDescriptorTable(program, *source->indirect_descriptor) &&
			    specialization.images[base_index].indirect_search_iterations == 0u &&
			    !source->indirect_descriptor->pointer_table) {
				const auto& indirect = *source->indirect_descriptor;
				const size_t mapping = specialization.images[base_index].indirect_mapping_offset;
				const auto& flat = snapshot.flattened_srt;
				if (mapping < flat.size() && flat[mapping] <= flat.size() - mapping - 1u) {
					uint32_t ordinal = 0;
					for (size_t child = program.info.images.size(); child <= i; ++child)
						ordinal += specialization.images[child].indirect_root == base_index;
					uint32_t matches = 0;
					uint64_t first_offset = 0;
					for (uint32_t probe = 0; probe < flat[mapping]; ++probe) {
						if (flat[mapping + 1u + probe] != ordinal) continue;
						if (matches++ == 0u) first_offset = uint64_t {probe} * 4u;
					}
					message += fmt::format(" table_source={} ordinal={} dense_matches={} "
					                       "first_candidate_offset={:#x}",
					                       indirect.table_source, ordinal, matches, first_offset);
				}
			}
			return SpecializationFail(message);
		}
	}
	for (uint32_t root_index = 0; root_index < specialization.images.size(); root_index++) {
		auto& root = specialization.images[root_index];
		if (root.indirect_root != root_index) {
			continue;
		}
		const auto key_count = root.indirect_mapping_offset < snapshot.flattened_srt.size()
		                           ? snapshot.flattened_srt[root.indirect_mapping_offset]
		                           : 0u;
		// A heap maps its slot count and slot address; tables map each key.
		size_t mapping_words = 3u;
		if (!root.heap)
			mapping_words = 1u + size_t {key_count} * (root.indirect_search_iterations == 0u ? 1u : 2u);
		if (key_count == 0u ||
		    root.indirect_mapping_offset + mapping_words > snapshot.flattened_srt.size()) {
			return SpecializationFail("indirect image specialization has an invalid key mapping");
		}
		uint32_t exemplar       = ImageResource::NoIndirectImage;
		uint32_t resource_count = 0;
		for (uint32_t resource = 0; resource < specialization.images.size(); resource++) {
			if (specialization.images[resource].indirect_root != root_index) {
				continue;
			}
			resource_count++;
			if (exemplar == ImageResource::NoIndirectImage &&
			    !NullImageDescriptor(snapshot.images[resource])) {
				exemplar = resource;
			}
		}
		if (resource_count < 2u || exemplar == ImageResource::NoIndirectImage) {
			return SpecializationFail("indirect image specialization has no typed candidate");
		}
		const auto& image_class = specialization.images[exemplar];
		const auto& base = program.info.images[root_index];
		const bool storage_swizzle = base.unfiltered_access_only &&
		    base.resource_class == ImageResourceClass::Storage;
		const auto is_2d = [](Decoder::ImageDimension dimension) {
			return dimension == Decoder::ImageDimension::Dim2D ||
			       dimension == Decoder::ImageDimension::Dim2DArray;
		};
		const auto is_1d_or_2d = [](Decoder::ImageDimension dimension) {
			return dimension == Decoder::ImageDimension::Dim1D ||
			       dimension == Decoder::ImageDimension::Dim2D;
		};
		// The descriptor's type selects the address layout. A 2D or 3D candidate reads the leading
		// coordinates of an equal or larger dimension, but an array with more than one slice reads
		// the next as its slice. Derivative, LOD, clamp and mip operands shift with the dimension.
		const auto leading = [&](Decoder::ImageDimension dimension, const DescriptorValue& value) {
			const bool sliced =
			    static_cast<Prospero::ImageType>((value.dwords[3] >> 28u) & 0xfu) ==
			        Prospero::ImageType::kColor2DArray &&
			    (value.dwords[4] & 0x1fffu) != ((value.dwords[4] >> 16u) & 0x1fffu);
			return !sliced &&
			       (dimension == Decoder::ImageDimension::Dim2D || dimension == base.dimension);
		};
		const bool leading_coordinates = (base.dimension == Decoder::ImageDimension::Dim2D ||
		                                  base.dimension == Decoder::ImageDimension::Dim3D) &&
		    leading(image_class.dimension, snapshot.images[exemplar]) &&
		    std::ranges::none_of(program.memory_info, [&](const MemoryInfo& memory) {
			    return memory.kind == ResourceKind::Image && memory.resource == root_index &&
			           (memory.image_has_mip || (memory.image_sample_flags &
			               (Decoder::ImageSampleFlagLod | Decoder::ImageSampleFlagDerivative |
			                Decoder::ImageSampleFlagLodClamp)) != 0u);
		    });
		for (uint32_t candidate = 0; candidate < specialization.images.size(); candidate++) {
			auto& image = specialization.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(snapshot.images[candidate])) {
				image.numeric_class     = image_class.numeric_class;
				image.dimension         = image_class.dimension;
				image.mip_count         = image_class.mip_count;
				image.conversion_format = image_class.conversion_format;
				image.shader_swizzle    = image_class.shader_swizzle;
				image.cube              = image_class.cube;
			}
			const bool same_coordinates = image.dimension == image_class.dimension &&
			                              image.cube == image_class.cube;
			const bool unfiltered_coordinates = base.unfiltered_access_only &&
			    base.dimension == Decoder::ImageDimension::Dim2D &&
			    !image.cube && !image_class.cube && is_1d_or_2d(image.dimension) &&
			    is_1d_or_2d(image_class.dimension);
			const bool prefix_coordinates =
			    leading_coordinates && leading(image.dimension, snapshot.images[candidate]);
			if ((!same_coordinates && !(is_2d(image.dimension) && is_2d(image_class.dimension)) &&
			     !unfiltered_coordinates && !prefix_coordinates) ||
			    image.conversion_format != image_class.conversion_format ||
			    (!storage_swizzle && image.shader_swizzle != image_class.shader_swizzle)) {
				return SpecializationFail(
				    fmt::format("indirect image table at pc 0x{:08x} has incompatible candidates",
				                program.info.images[root_index].first_use_pc));
			}
		}
	}
	CompactImages(snapshot.images, [&](size_t index) { return !specialization.images[index].fmask; });
	return true;
}

bool BuildSamplerPlan(const ShaderInfo& base, SamplerPlan& plan) {
	plan.mapping.resize(base.samplers.size());
	plan.bindings.resize(base.samplers.size());
	std::vector<uint8_t> usage(base.samplers.size());
	for (const auto& pair: base.sampled_pairs) {
		if (pair.image >= base.images.size() || pair.sampler >= base.samplers.size()) {
			return false;
		}
		usage[pair.sampler] |= 1u << static_cast<uint32_t>(ClassifySampler(base.images[pair.image]));
		for (const auto candidate: base.images[pair.image].indirect_resources) {
			if (candidate >= base.images.size()) return false;
			usage[pair.sampler] |= 1u << static_cast<uint32_t>(ClassifySampler(base.images[candidate]));
		}
		for (const auto candidate: base.samplers[pair.sampler].indirect_resources) {
			if (candidate >= base.samplers.size()) return false;
			usage[candidate] |= usage[pair.sampler];
		}
	}
	for (uint32_t index = 0; index < base.samplers.size(); index++) {
		auto& mapping = plan.mapping[index];
		mapping.fill(UINT32_MAX);
		const auto classes = usage[index] == 0u ? 1u : usage[index];
		bool       first   = true;
		for (uint32_t type = 0; type < mapping.size(); type++) {
			if ((classes & (1u << type)) == 0u) continue;
			const auto target = first ? index : static_cast<uint32_t>(plan.bindings.size());
			if (!first) plan.bindings.emplace_back();
			mapping[type]         = target;
			plan.bindings[target] = {index, static_cast<SamplerClass>(type)};
			first                = false;
		}
	}
	return true;
}

template <typename Predicate>
static std::vector<ResourceBlock> ResourceControlFlow(const Program& program, const Predicate& predicate) {
	// Address writes can change the values control flow reads.
	if (program.has_address_writes) {
		return {};
	}
	std::unordered_map<const Block*, uint32_t> indices;
	for (uint32_t i = 0; i < program.blocks.size(); i++) {
		if (!indices.emplace(program.blocks[i], i).second) {
			return {};
		}
	}
	std::vector<ResourceBlock> blocks(program.blocks.size());
	for (uint32_t i = 0; i < blocks.size(); i++) {
		auto&                 block      = blocks[i];
		const auto&             info       = *program.blocks[i];
		const auto&           terminator = info.terminator;
		const std::array        targets {terminator.true_block, terminator.false_block};
		std::span<Block* const> successors;
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch: successors = std::span(targets).first(1); break;
			case CFG::TerminatorKind::ConditionalBranch:
				successors      = targets;
				block.condition = info.condition;
				break;
			case CFG::TerminatorKind::IndirectBranch: successors = info.ImmSuccessors(); break;
			case CFG::TerminatorKind::Return: break;
			default: return {};
		}
		for (const auto successor: successors) {
			const auto found = indices.find(successor);
			if (found == indices.end()) {
				return {};
			}
			block.successors.push_back(found->second);
		}
		for (const auto& inst: *program.blocks[i]) {
			const auto op     = inst.GetOpcode();
			if (op == ValueOpcode::ReadConst) {
				block.srt_reads.push_back(inst.Arg(1).Resolve().U32());
				continue;
			}
			const auto buffer = BufferAccessOf(op);
			const auto image  = ImageOpcodeInfoOf(op);
			if (buffer == BufferAccess::None && image.access == ImageAccess::None) {
				continue;
			}
			const auto& memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			if (memory.planning_only || memory.kind == ResourceKind::IndirectBuffer) {
				continue;
			}
			if (buffer != BufferAccess::None) {
				block.sources.push_back(program.info.buffers.at(memory.resource).source);
			} else {
				block.sources.push_back(program.info.images.at(memory.resource).source);
				block.images.push_back(memory.resource);
				if (image.needs_sampler) {
					block.sources.push_back(program.info.samplers.at(memory.sampler).source);
				}
			}
		}
		for (auto* list: {&block.sources, &block.images}) {
			std::ranges::sort(*list);
			list->erase(std::unique(list->begin(), list->end()), list->end());
		}
	}
	for (auto& block: blocks) block.condition = predicate(block.condition);
	if (std::ranges::none_of(
	        blocks, [](const ResourceBlock& block) { return !block.condition.IsEmpty(); })) {
		return {};
	}
	return blocks;
}

// ResolveComputeBufferFill proves complete workgroups and matching requested extents, so every
// dispatch-bound predicate accepted here is true for every invocation covered by the fill.
static bool IsFullDispatchPredicate(Value value, uint32_t depth = 0) {
	value = value.Resolve();
	if (value == Value(true)) return true;
	const auto* inst = value.TryInstruction();
	if (inst == nullptr || depth > 32) return false;
	if (inst->GetOpcode() == ValueOpcode::LogicalAnd) {
		return IsFullDispatchPredicate(inst->Arg(0), depth + 1) &&
		       IsFullDispatchPredicate(inst->Arg(1), depth + 1);
	}
	if (inst->GetOpcode() != ValueOpcode::ULessThan32) return false;
	const auto* global = inst->Arg(0).Resolve().TryInstruction();
	const auto* extent = inst->Arg(1).Resolve().TryInstruction();
	if (global == nullptr || global->GetOpcode() != ValueOpcode::GetBuiltin ||
	    global->Arg(0).Resolve() != Value(static_cast<uint32_t>(StageInputKind::GlobalInvocationId)) ||
	    extent == nullptr || extent->GetOpcode() != ValueOpcode::GetDispatchThreadExtent)
		return false;
	const auto axis = global->Arg(1).Resolve();
	return axis.IsImmediate() && axis.U32() < 3 && extent->Arg(0).Resolve() == axis;
}

static std::optional<std::array<uint64_t, 3>> FillIndex(Value value, uint32_t axis, Value guard) {
	return AffineIndex<3>(value, [&](Value at) { return ResolveActiveU32(at, guard); },
	    [&](Value at) -> std::optional<size_t> {
		    const auto* inst = at.TryInstruction();
		    if (inst == nullptr || inst->GetOpcode() != ValueOpcode::GetBuiltin ||
		        inst->Arg(1) != Value(axis)) return {};
		    if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId))) return 1u;
		    if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId))) return 2u;
		    return {};
	    });
}

static UniformFillPlan AnalyzeUniformFill(const Program& program) {
	if (program.stage != ShaderType::Compute || program.blocks.empty() ||
	    !program.info.samplers.empty()) {
		return {};
	}
	std::unordered_set<const Block*> visited;
	const auto*                      block = program.blocks.front();
	const Inst*                      store = nullptr;
	for (;;) {
		if (block == nullptr || !visited.insert(block).second) return {};
		for (const auto& inst: *block) {
			if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::None) return {};
			if (!inst.MayHaveSideEffects()) continue;
			if (store != nullptr || (BufferAccessOf(inst.GetOpcode()) != BufferAccess::Write &&
			                         inst.GetOpcode() != ValueOpcode::ImageWrite))
				return {};
			store = &inst;
		}
		const auto& term = block->terminator;
		if (term.kind == CFG::TerminatorKind::Return) break;
		if (term.kind != CFG::TerminatorKind::Branch) return {};
		block = term.true_block;
	}
	if (store == nullptr || visited.size() != program.blocks.size()) return {};
	for (const auto& buffer: program.info.buffers) {
		if (buffer.read && (!buffer.scalar || buffer.written)) return {};
	}
	const auto& memory = program.memory_info.at(store->Flags<MemoryFlags>().index);
	UniformFillPlan result;
	result.fill.resource = memory.resource;
	Value data;
	Value guard(true);
	if (store->GetOpcode() == ValueOpcode::ImageWrite) {
		if (program.info.images.size() != 1 || memory.dmask != 1 || memory.data_bits != 32 ||
		    memory.image_has_mip || memory.image_sample_flags != 0 || memory.image_r128 ||
		    memory.image_dimension != Decoder::ImageDimension::Dim2DArray ||
		    store->Arg(3).Resolve() != Value(true)) return {};
		const auto& image = program.info.images[memory.resource];
		if (image.read || image.atomic || image.mip_mode != ImageMipMode::None) return {};
		const auto* address = store->Arg(1).ResolveInstruction();
		if (address == nullptr || address->GetOpcode() != ValueOpcode::MakeImageAddress) return {};
		for (uint32_t axis = 0; axis < 3; ++axis) {
			const auto index = FillIndex(address->Arg(axis), axis, guard);
			if (!index || (*index)[0] != 0) return {};
			if (axis < 2) {
				if ((*index)[1] != 1 || (*index)[2] == 0) return {};
			} else if ((*index)[1] != 0 || (*index)[2] != 1) {
				return {};
			}
			result.fill.group_stride[axis] = static_cast<uint32_t>((*index)[2]);
		}
		const auto* values = store->Arg(2).ResolveInstruction();
		if (values == nullptr || values->GetOpcode() != ValueOpcode::CompositeConstructU32x4)
			return {};
		result.fill.kind  = UniformFillKind::Image;
		result.fill.words = 1;
		data = values->Arg(0);
	} else {
		if (!program.info.images.empty()) return {};
		const auto           op = store->GetOpcode();
		constexpr std::array stores {ValueOpcode::StoreBufferU32, ValueOpcode::StoreBufferU32x2,
		                             ValueOpcode::StoreBufferU32x3, ValueOpcode::StoreBufferU32x4};
		const auto           store_op = std::ranges::find(stores, op);
		if (store_op == stores.end() || store->Arg(2).Resolve() != Value(0u) ||
		    store->Arg(3).Resolve() != Value(0u))
			return {};
		guard = store->Arg(5).Resolve();
		if (!IsFullDispatchPredicate(guard)) return {};
		if (!memory.formatted || memory.typed || !memory.idxen || memory.offen || memory.offset != 0 ||
		    memory.data_bits != 32 ||
		    memory.data_dwords != static_cast<uint32_t>(store_op - stores.begin() + 1))
			return {};
		const auto address = FillIndex(store->Arg(1), 0, guard);
		if (!address || (*address)[0] != 0 || (*address)[1] != 1 || (*address)[2] == 0) return {};
		result.fill.kind = UniformFillKind::Buffer;
		result.fill.group_stride[0] = static_cast<uint32_t>((*address)[2]);
		result.fill.words = memory.data_dwords;
		data = store->Arg(4);
	}
	data = ResolveActiveU32(data, guard);
	const auto*          vector = data.TryInstruction();
	constexpr std::array composites {ValueOpcode::CompositeConstructU32x2,
	                                 ValueOpcode::CompositeConstructU32x3,
	                                 ValueOpcode::CompositeConstructU32x4};
	if (result.fill.words > 1 &&
	    (vector == nullptr || vector->GetOpcode() != composites[result.fill.words - 2]))
		return {};
	for (uint32_t i = 0; i < result.fill.words; ++i) {
		const auto word = ResolveActiveU32(result.fill.words == 1 ? data : vector->Arg(i), guard);
		if (word.GetType() != Type::U32 ||
		    !ValidateRuntimeValue(program, word, RuntimeValueType::Integer))
			return {};
		result.values[i] = word;
	}
	return result;
}

// State an extracted or loaded plan derives from its fields: one key set per selector group, which
// extraction numbers densely in order of first use (false when they are not), and the walker.
static bool DerivePlanState(ResourcePlan& plan) {
	for (const auto group: plan.selector_groups) {
		if (group == plan.selector_keys.size()) plan.selector_keys.emplace_back();
		if (group != UINT32_MAX && group >= plan.selector_keys.size()) return false;
	}
	if (plan.srt_plan_complete && plan.resource_tracking_complete) CompileSrtWalker(plan);
	return true;
}

// Which parts of a buffer's specialization can change the program (see the use in
// BuildResourceSpecialization).
static void FindBufferSpecializationUses(const Program& program, ResourcePlan& plan) {
	const auto& buffers = program.info.buffers;
	plan.buffer_vector_access.assign(buffers.size(), 0u);
	plan.buffer_feeds_descriptors.assign(buffers.size(), 0u);
	for (uint32_t index = 0; index < buffers.size(); ++index) {
		if (buffers[index].indirect_root != BufferResource::NoIndirectBuffer ||
		    !buffers[index].indirect_resources.empty()) {
			plan.buffer_vector_access[index] = plan.buffer_feeds_descriptors[index] = 1u;
		}
	}
	for (const auto& memory: program.memory_info) {
		if (!memory.planning_only && memory.resource < buffers.size() &&
		    (memory.kind == ResourceKind::Buffer || memory.kind == ResourceKind::IndirectBuffer)) {
			plan.buffer_vector_access[memory.resource] = 1u;
		}
	}
	std::unordered_set<const Inst*> visited;
	std::vector<const Inst*>        pending;
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto opcode = inst.GetOpcode();
			if (opcode == ValueOpcode::GetBufferResource || opcode == ValueOpcode::GetImageResource ||
			    opcode == ValueOpcode::GetSamplerResource) {
				pending.push_back(&inst);
			}
		}
	}
	while (!pending.empty()) {
		const auto* inst = pending.back();
		pending.pop_back();
		if (!visited.insert(inst).second) continue;
		if (inst->GetOpcode() == ValueOpcode::ReadConstBuffer) {
			const auto& memory = program.memory_info.at(inst->Flags<MemoryFlags>().index);
			if (memory.kind == ResourceKind::ScalarBuffer && !memory.planning_only &&
			    memory.resource < buffers.size()) {
				plan.buffer_feeds_descriptors[memory.resource] = 1u;
			}
		}
		for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
			if (const auto* source = inst->Arg(arg).Resolve().TryInstruction()) pending.push_back(source);
		}
	}
}

ResourcePlan ExtractResourcePlan(const Program& program) {
	ResourcePlan plan;
	plan.stage                      = program.stage;
	plan.shader_hash                = program.shader_hash;
	plan.user_data_base             = program.user_data_base;
	plan.user_data_count            = program.user_data_count;
	plan.info                       = program.info;
	plan.memory_info                = program.memory_info;
	plan.source_reads               = program.source_reads;
	plan.srt_plan_complete          = program.srt_plan_complete;
	plan.resource_tracking_complete = program.resource_tracking_complete;
	FindBufferSpecializationUses(program, plan);

	std::unordered_map<const Inst*, Inst*> cloned;
	std::function<Value(Value)>            Clone = [&](Value value) -> Value {
		value              = value.Resolve();
		const auto* source = value.TryInstruction();
		if (source == nullptr) {
			return value;
		}
		if (source->GetOpcode() == ValueOpcode::ReadConst) {
			const auto slot = source->Arg(1).Resolve();
			EXIT_IF(!slot.IsImmediate() || slot.GetType() != Type::U32 ||
			        slot.U32() >= program.srt_reads.size());
			return Clone(program.srt_reads[slot.U32()].value);
		}
		if (source->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    ValidateRuntimeValue(program, source->Arg(0))) {
			// Uniform values need no new EXEC context; preserve their shared evaluation memo.
			return Clone(source->Arg(0));
		}
		if (source->GetOpcode() == ValueOpcode::Phi) {
			const auto invariant = ResolveInvariantPhi(program, value);
			if (!invariant.IsEmpty() && invariant != value) {
				return Clone(invariant);
			}
		}
		if (const auto found = cloned.find(source); found != cloned.end()) {
			return Value(found->second);
		}
		if (source->GetOpcode() == ValueOpcode::LoadBufferU32) plan.capture_specialization_reads = true;
		auto& target =
		    plan.value_storage.emplace_back(source->GetOpcode(), source->Flags<uint64_t>());
		if (source->GetOpcode() == ValueOpcode::LoadAddressU32 ||
		    source->GetOpcode() == ValueOpcode::ReadConstBuffer ||
		    source->GetOpcode() == ValueOpcode::LoadBufferU32) {
			target.SetFlags(MemoryFlags {.index = source->Flags<MemoryFlags>().index});
		}
		cloned.emplace(source, &target);
		if (source->GetOpcode() == ValueOpcode::Phi) {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.AddPhiOperand(nullptr, Clone(source->Arg(index)));
			}
		} else {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.SetArg(index, Clone(source->Arg(index)));
			}
		}
		return Value(&target);
	};

	bool capture_indirect_reads = false;
	plan.descriptor_sources.reserve(program.descriptor_sources.size());
	for (const auto& source: program.descriptor_sources) {
		auto& target          = plan.descriptor_sources.emplace_back();
		target.dword_count    = source.dword_count;
		target.indirect_descriptor = source.indirect_descriptor;
		if (target.indirect_descriptor.has_value()) {
			capture_indirect_reads = true;
			target.indirect_descriptor->key_count = Clone(target.indirect_descriptor->key_count);
			target.indirect_descriptor->selector_first =
			    Clone(target.indirect_descriptor->selector_first);
			target.indirect_descriptor->selector_mask =
			    Clone(target.indirect_descriptor->selector_mask);
		}
		for (uint32_t dword = 0; dword < source.dword_count; dword++) {
			target.dwords[dword] = Clone(source.dwords[dword]);
		}
	}
	// Destination tables share keys when their complete selector domains agree.
	// Resolve those domains lazily at dispatch time, without repeated guest reads.
	std::vector<uint32_t> group_sources; // The first source of each group.
	plan.selector_groups.resize(plan.descriptor_sources.size(), UINT32_MAX);
	for (uint32_t source = 0; source < plan.descriptor_sources.size(); ++source) {
		const auto& descriptor = program.descriptor_sources[source].indirect_descriptor;
		if (!descriptor || !descriptor->selector) continue;
		const auto& a = *descriptor;
		uint32_t group = 0;
		for (; group < group_sources.size(); ++group) {
			const auto& b = *program.descriptor_sources[group_sources[group]].indirect_descriptor;
			if (EquivalentSelectorDomain(program, a, b)) break;
		}
		if (group == group_sources.size()) group_sources.push_back(source);
		plan.selector_groups[source] = group;
	}
	plan.srt_reads.reserve(program.srt_reads.size());
	for (const auto& read: program.srt_reads) {
		plan.srt_reads.push_back({Clone(read.value), read.flat_offset});
	}
	// Retain integer control expressions with opaque varying leaves. Their identities
	// matter when an EXEC factor also guards a selected value; never clone float work.
	bool capture_control_reads = false;
	bool uniform_control = false;
	const auto control_value_count = plan.value_storage.size();
	struct ControlValue {
		Value value;
		bool complete = false;
	};
	std::unordered_map<const Inst*, ControlValue> predicates;
	std::function<Value(Value)> ClonePredicate = [&](Value value) -> Value {
		value = value.Resolve();
		if (value.IsEmpty()) return {};
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) {
			uniform_control |= value.GetType() == Type::U1;
			return ValidateRuntimeValue(program, value, RuntimeValueType::Integer) ? value : Value {};
		}
		const auto [found, inserted] = predicates.try_emplace(inst);
		auto& entry = found->second;
		auto& result = entry.value;
		const auto opaque = [&]() -> Value {
			if (value.GetType() != Type::U1 && value.GetType() != Type::U32) return {};
			return result = Value(&plan.value_storage.emplace_back(
			    value.GetType() == Type::U1 ? ValueOpcode::UndefU1 : ValueOpcode::UndefU32));
		};
		// A recursive projection remains opaque, including unstructured loop Phis.
		if (!inserted) return entry.complete || !result.IsEmpty() ? result : opaque();
		const auto finish = [&](Value projected) {
			entry.complete = true;
			return result = projected;
		};
		if (ValidateRuntimeValue(program, value, RuntimeValueType::Integer)) {
			uniform_control = true;
			capture_control_reads |=
			    !ValidateRuntimeValue(program, value, RuntimeValueType::ImmutableInteger);
			return finish(Clone(value));
		}
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ConditionRef) return finish(ClonePredicate(inst->Arg(0)));
		if (op == ValueOpcode::Phi) {
			if (inst->Parent() == nullptr || inst->Parent()->terminator.loop_header ||
			    inst->NumPhiBlocks() != inst->NumArgs() || inst->NumArgs() == 0u ||
			    (value.GetType() != Type::U1 && value.GetType() != Type::U32)) return finish(opaque());
			std::vector<uint32_t> predecessors;
			for (size_t index = 0; index < inst->NumArgs(); ++index) {
				const auto predecessor = std::ranges::find(program.blocks, inst->PhiBlock(index));
				if (predecessor == program.blocks.end()) return finish(opaque());
				predecessors.push_back(static_cast<uint32_t>(predecessor - program.blocks.begin()));
			}
			auto& node = plan.value_storage.emplace_back(op, inst->Flags<uint64_t>());
			plan.control_phi_predecessors.emplace(&node, std::move(predecessors));
			for (size_t index = 0; index < inst->NumArgs(); ++index)
				node.AddPhiOperand(nullptr, ClonePredicate(inst->Arg(index)));
			return finish(Value(&node));
		}
		if (op != ValueOpcode::LogicalAnd && op != ValueOpcode::LogicalOr &&
		    op != ValueOpcode::LogicalNot && op != ValueOpcode::IEqual32 &&
		    op != ValueOpcode::INotEqual32 && op != ValueOpcode::ULessThan32 &&
		    op != ValueOpcode::ULessThanEqual32 &&
		    op != ValueOpcode::SelectU1 && op != ValueOpcode::SelectU32) return finish(opaque());
		auto& node = plan.value_storage.emplace_back(op);
		for (size_t index = 0; index < inst->NumArgs(); ++index)
			node.SetArg(index, ClonePredicate(inst->Arg(index)));
		return finish(Value(&node));
	};
	plan.control_flow = ResourceControlFlow(program, ClonePredicate);
	if (!uniform_control) {
		plan.control_flow.clear();
		plan.control_phi_predecessors.clear();
		auto first = std::next(plan.value_storage.begin(), control_value_count);
		for (auto it = first; it != plan.value_storage.end(); ++it) it->Invalidate();
		plan.value_storage.erase(first, plan.value_storage.end());
	}
	plan.uniform_fill = program.uniform_fill.fill.kind != UniformFillKind::None
	    ? program.uniform_fill : AnalyzeUniformFill(program);
	for (auto& value: plan.uniform_fill.values) value = Clone(value);
	plan.uniform_fill.pattern_size = Clone(plan.uniform_fill.pattern_size);
	plan.uniform_fill.element_count = Clone(plan.uniform_fill.element_count);
	plan.capture_specialization_reads |=
	    capture_indirect_reads || capture_control_reads || !plan.source_reads.empty();
	if (capture_indirect_reads || !plan.source_reads.empty())
		plan.resource_tracking_complete &= !program.has_address_writes;
	DerivePlanState(plan);
	return plan;
}

bool ReadsStrideAtRunTime(const ResourcePlan& program, uint32_t index,
                          const ResourceSpecialization::Buffer& buffer) {
	// The buffers the program names itself (the candidates of an indirect descriptor share their
	// accesses, which compare their strides) and reads by element, with a plain stride. A stride
	// of 0 is one (a shader is given records in one dispatch and a single element in another),
	// but not with the bounds mode that zero_stride_oob stands for.
	if (index >= program.info.buffers.size() || index >= program.buffer_vector_access.size() ||
	    !program.buffer_vector_access[index] || buffer.zero_stride_oob ||
	    (buffer.packed_stride & (1u << 14u)) != 0u) {
		return false;
	}
	const auto source = program.info.buffers[index].source;
	return source < program.descriptor_sources.size() &&
	       !program.descriptor_sources[source].indirect_descriptor.has_value();
}

void ReadStridesAtRunTime(const ResourcePlan& program, ResourceSpecialization& specialization) {
	for (uint32_t i = 0; i < specialization.buffers.size(); i++) {
		if (ReadsStrideAtRunTime(program, i, specialization.buffers[i])) {
			specialization.buffers[i].packed_stride |= RuntimeBufferStride;
		}
	}
}

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	t_record_blocks.clear();
	for (auto& keys: program.selector_keys) keys.resolved = false;
	if (!program.resource_tracking_complete) return false;
	const auto uses_workgroup_count = [](const DescriptorSource& source) {
		return source.indirect_descriptor && source.indirect_descriptor->workgroup_axis != UINT32_MAX;
	};
	const bool capture_reads = program.capture_specialization_reads || !program.source_reads.empty() ||
	    (runtime.indirect_args_address != 0u &&
	     std::ranges::any_of(program.descriptor_sources, uses_workgroup_count));
	auto& reads = snapshot.specialization_reads;
	reads = program.source_reads;
	if (!WalkSrt(program, runtime, snapshot.flattened_srt, capture_reads ? &reads : nullptr)) {
		// Ahead of execution a walk fails on any fault; the execution thread walks again.
		if (runtime.record_load != nullptr) return false;
		return SpecializationFail(fmt::format("SRT refresh complete={} reads={} user_data={}",
		    program.srt_plan_complete, program.srt_reads.size(), runtime.user_data.size()));
	}
	snapshot.flattened_srt.insert(snapshot.flattened_srt.end(), runtime.call_words.begin(),
	                              runtime.call_words.end());
	SrtReadCapture capture {runtime, reads};
	auto observed = capture_reads ? capture.ObservedRuntime() : runtime;
	std::array<uint32_t, 3> workgroup_counts {};
	if (runtime.indirect_args_address != 0u) {
		for (uint32_t source = 0; source < program.descriptor_sources.size(); ++source) {
			if (!WalkedActive(program, source) ||
			    !uses_workgroup_count(program.descriptor_sources[source])) continue;
			if ((runtime.indirect_args_address & 3u) != 0u ||
			    !ReadTable(runtime.indirect_args_address, sizeof(workgroup_counts), 0u,
			               observed, workgroup_counts)) return false;
			observed.workgroup_counts = workgroup_counts;
			break;
		}
	}
	snapshot.uniform_fill = {};
	const auto& fill = program.uniform_fill;
	if (fill.fill.kind == UniformFillKind::Replicate) {
		uint32_t count = 0;
		uint32_t size  = 0;
		if (WalkedValue(program, fill.element_count, count) &&
		    WalkedValue(program, fill.pattern_size, size) && count != 0 && size != 0) {
			snapshot.uniform_fill                  = fill.fill;
			snapshot.uniform_fill.bounded_elements = count;
			snapshot.uniform_fill.pattern_elements = size;
		}
	}
	auto words = fill.fill.kind == UniformFillKind::Replicate ? 0u : fill.fill.words;
	std::array<uint32_t, 4> stored {};
	bool uniform_fill = words != 0;
	if (uniform_fill && !fill.pattern_size.IsEmpty())
		uniform_fill = WalkedValue(program, fill.pattern_size, words) && words >= 1 && words <= 4;
	uint32_t elements = 0;
	if (uniform_fill && !fill.element_count.IsEmpty())
		uniform_fill = WalkedValue(program, fill.element_count, elements) && elements != 0;
	for (uint32_t i = 0; i < words && uniform_fill; ++i) {
		uniform_fill = WalkedValue(program, fill.values[i], stored[i]) && stored[i] == stored[0];
	}
	if (uniform_fill) {
		snapshot.uniform_fill = fill.fill;
		snapshot.uniform_fill.value = stored[0];
		snapshot.uniform_fill.bounded_elements = elements;
	}
	const auto evaluate = [&](uint32_t source, DescriptorValue& value) {
		if (source >= program.descriptor_sources.size()) {
			return false;
		}
		if (WalkedActive(program, source)) {
			return WalkedDescriptor(program, source, value) ||
			    SpecializationFail(fmt::format("descriptor evaluation source={}", source));
		}
		value = {};
		value.dword_count = program.descriptor_sources[source].dword_count;
		return true;
	};
	const auto resource_failed = [&](std::string_view kind, uint32_t resource, uint32_t source,
	                                 size_t candidates) {
		return SpecializationFail(fmt::format("indirect {} resource={} source={} candidates={}",
		    kind, resource, source, candidates));
	};
	snapshot.buffers.resize(program.info.buffers.size());
	specialization.buffers.assign(program.info.buffers.size(), {});
	for (uint32_t i = 0; i < snapshot.buffers.size(); ++i) {
		const auto base_index =
		    i < program.info.buffers.size() ? i : specialization.buffers[i].indirect_root;
		if (base_index >= program.info.buffers.size()) return false;
		const auto& base = program.info.buffers[base_index];
		if (i < program.info.buffers.size()) {
			const auto* source = Source(program, base.source);
			if (source == nullptr) return false;
			if (source->indirect_descriptor.has_value()) {
				snapshot.buffers[i] = {.dword_count = 4u};
				if (WalkedActive(program, base.source)) {
					if (!MaterializeIndirectDescriptor(
					        program, base.source, i, 4u, 4u, observed, snapshot,
					        snapshot.buffers, specialization.buffers,
					        [&](DescriptorValue& value) {
						        return NormalizeIndirectBuffer(
						            value, source->indirect_descriptor->indexed, base);
					        }))
						return resource_failed("buffer", i, base.source, snapshot.buffers.size());
				}
			} else if (!evaluate(base.source, snapshot.buffers[i])) {
				return false;
			}
		}
		auto&                descriptor_value = snapshot.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(descriptor_value, descriptor)) {
			return SpecializationFail(fmt::format("buffer descriptor {} has invalid width", i));
		}
		if (descriptor.Type() != 0) {
			descriptor_value.dwords.fill(0);
			descriptor = {};
		}
		auto       packed_stride = descriptor.PackedStride();
		const auto stride        = packed_stride & 0x3fffu;
		const bool swizzle       = stride != 0u && ((packed_stride >> 14u) & 1u) != 0u;
		if (stride == 0u) {
			packed_stride &= ~((1u << 14u) | (3u << 16u));
		} else if (!swizzle) {
			packed_stride &= ~(3u << 16u);
		}
		auto& buffer         = specialization.buffers[i];
		buffer.packed_stride = packed_stride;
		buffer.descriptor_format =
		    base.formatted ? descriptor.Format() : Prospero::BufferFormat::kInvalid;
		buffer.descriptor_swizzle = base.formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7);
		buffer.zero_stride_oob    = descriptor.OutOfBounds() == 0u && stride == 0u;
		buffer.scalar_empty       = base.scalar && descriptor.GetSize() == 0u;
		// Only vector accesses use the stride and bounds mode, and an empty buffer binds the
		// zeroed null buffer, so its scalar reads give the zero that scalar_empty folds in; the
		// fold matters only where those reads build descriptors. Leaving out what the program
		// does not use keeps one program when a buffer the shader only reads by scalar loads is
		// unbound in some dispatches (Wolverine's ray tracing shader).
		if (i < program.buffer_vector_access.size() && !program.buffer_vector_access[i]) {
			buffer.packed_stride   = 0u;
			buffer.zero_stride_oob = false;
		}
		if (i < program.buffer_feeds_descriptors.size() && !program.buffer_feeds_descriptors[i]) {
			buffer.scalar_empty = false;
		}
		if (runtime.runtime_stride && ReadsStrideAtRunTime(program, i, buffer)) {
			buffer.packed_stride |= RuntimeBufferStride;
		}
	}
	snapshot.images.resize(program.info.images.size());
	specialization.images.resize(program.info.images.size());
	auto previous_tables = std::move(snapshot.image_tables);
	snapshot.image_tables.clear();
	std::vector<uint32_t> table_mappings;
	for (uint32_t i = 0; i < program.info.images.size(); ++i) {
		const auto& image = program.info.images[i];
		specialization.images[i] = {
		    .numeric_class = image.numeric_class,
		    .dimension = image.dimension,
		    .mip_count = image.mip_count,
		    .conversion_format = image.conversion_format,
		    .shader_swizzle = image.shader_swizzle,
		    .indirect_root = image.indirect_root,
		    .indirect_mapping_offset = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .table = image.table,
		    .table_capacity = image.table_capacity,
		    .table_mapping_offset = image.table_mapping_offset,
		    .table_entry_mask = image.table_entry_mask,
		    .cube = image.cube,
		};
		const auto* source = Source(program, image.source);
		if (source == nullptr) {
			return false;
		}
		if (source->indirect_descriptor.has_value()) {
			snapshot.images[i] = {.dword_count = 8u};
			if (!WalkedActive(program, image.source) || !WalkedImageActive(program, i)) {
				continue;
			}
			const auto& indirect = *source->indirect_descriptor;
			if (indirect.table_array) {
				DescriptorValue table;
				if (!WalkedDescriptor(program, indirect.table_source, table) ||
				    !MaterializeImageTable(program, indirect, table, i, observed, snapshot,
				                           specialization, previous_tables, table_mappings)) {
					return false;
				}
				continue;
			}
			const bool bounded_table = IsBoundedDescriptorTable(program, indirect);
			if (!MaterializeIndirectDescriptor(
			        program, image.source, i, 8u, image.r128 ? 4u : 8u, observed, snapshot, snapshot.images,
			        specialization.images, [&](DescriptorValue& value) {
				        // A broad heap also contains resources for other typed image operations.
				        // A sample of a T# whose channels all select 0 reads 0 at any address.
				        if (NullImageDescriptor(value) || !ValidImageDescriptor(value, image.r128) ||
				            (image.sample_access_only && !image.depth_compare &&
				             DescriptorImageSwizzle(value) == 0u) ||
				            (bounded_table && DescriptorDimension(value, image.dimension) != image.dimension))
					        value.dwords.fill(0u);
				        return true;
			        })) {
				return resource_failed("image", i, image.source, snapshot.images.size());
			}
		} else {
			if (!evaluate(image.source, snapshot.images[i])) {
				return false;
			}
			// An image listed only by unreached blocks reads zero, not its source's descriptor.
			if (!WalkedImageActive(program, i) ||
			    !ValidImageDescriptor(snapshot.images[i], image.r128)) {
				snapshot.images[i].dwords.fill(0);
			}
		}
	}
	snapshot.samplers.resize(program.info.samplers.size());
	specialization.samplers.assign(program.info.samplers.size(), {});
	for (uint32_t i = 0; i < snapshot.samplers.size(); ++i) {
		const auto base_index =
		    i < program.info.samplers.size() ? i : specialization.samplers[i].indirect_root;
		if (base_index >= program.info.samplers.size()) return false;
		if (i < program.info.samplers.size()) {
			auto source = program.info.samplers[i].source;
			const auto* descriptor = Source(program, source);
			if (descriptor == nullptr) return false;
			const auto* indirect = descriptor->indirect_descriptor ? &*descriptor->indirect_descriptor : nullptr;
			if (indirect != nullptr && indirect->sources.empty()) {
				snapshot.samplers[i] = {.dword_count = 4u};
				if (WalkedActive(program, source) && !MaterializeIndirectDescriptor(
				        program, source, i, 4u, 4u, observed, snapshot, snapshot.samplers,
				        specialization.samplers,
				        [](DescriptorValue&) { return true; }))
					return resource_failed("sampler", i, source, snapshot.samplers.size());
			} else {
				if (indirect != nullptr) source = indirect->sources[0];
				if (!evaluate(source, snapshot.samplers[i])) return false;
			}
		}
		if (program.info.samplers[base_index].gather_lod) {
			const auto control = snapshot.samplers[i].dwords[2];
			const auto filter = (control >> 26u) & 3u;
			const auto perf_mip = (snapshot.samplers[i].dwords[1] >> 24u) & 15u;
			// MipNone always selects the base level. Point/linear selection requires zero
			// primary/secondary bias; linear mip snapping is not implemented.
			if (filter > 2u || (filter != 0u && (control & 0xfffffu) != 0u) ||
			    (filter == 2u && perf_mip != 0u)) {
				return SpecializationFail(
				    "explicit-LOD gather requires None, unbiased Point, or unbiased Linear without mip snapping");
			}
		}
	}
	FlushRecordBlocks(snapshot.flattened_srt);
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return BuildResourceSpecialization(program, snapshot, specialization);
}

uint32_t ImageHeapKind(const ImageResource& root, DescriptorValue& value) {
	// Mirrors BuildResourceSpecialization for sampled roots: entries it would reject, entries of
	// another dimension and entries that need shader-side conversion read as null instead of
	// failing the draw.
	const auto format = static_cast<Prospero::BufferFormat>((value.dwords[1] >> 20u) & 0x1ffu);
	const auto tile = static_cast<Prospero::TileMode>((value.dwords[3] >> 20u) & 0x1fu);
	const auto numeric_class =
	    Prospero::SampledTextureNumericClass(Prospero::RemapTextureFormat(format));
	if (NullImageDescriptor(value) || !ValidImageDescriptor(value, root.r128) ||
	    DescriptorDimension(value, root.dimension) != root.dimension ||
	    Prospero::IsFmaskTextureFormat(format) || format == Prospero::BufferFormat::k11_11_10UNorm ||
	    ImageConversionFormat(format) != Prospero::BufferFormat::kInvalid ||
	    (root.unfiltered_access_only && format == Prospero::BufferFormat::k16UInt &&
	     tile == Prospero::TileMode::kDepth) ||
	    numeric_class == Prospero::TextureNumericClass::Unsupported ||
	    (root.depth_compare && numeric_class != Prospero::TextureNumericClass::Float)) {
		value.dwords.fill(0u);
		return 0u;
	}
	return 1u + static_cast<uint32_t>(numeric_class) * 2u + (DescriptorIsCube(value) ? 1u : 0u);
}

ImageResource ImageHeapResource(const ImageResource& root, uint32_t kind) {
	ImageResource resource;
	resource.resource_class         = ImageResourceClass::Sampled;
	resource.read                   = true;
	resource.heap                   = true;
	resource.dimension              = root.dimension;
	resource.unfiltered_access_only = root.unfiltered_access_only;
	resource.depth_compare          = root.depth_compare;
	resource.r128                   = root.r128;
	if (kind != 0u) {
		resource.numeric_class = static_cast<Prospero::TextureNumericClass>((kind - 1u) / 2u);
		resource.cube          = ((kind - 1u) & 1u) != 0u;
	}
	return resource;
}

void SetComputeIndirectFloors(size_t keys, size_t images) {
	g_compute_indirect_keys.store(std::max(std::bit_ceil(keys), MinIndirectMappingCapacity),
	                              std::memory_order_relaxed);
	g_compute_indirect_images.store(std::max<size_t>(std::bit_ceil(images), 1u),
	                                std::memory_order_relaxed);
	g_stable_indirect_layouts.store(true, std::memory_order_relaxed);
}

bool ResourceSpecializationFits(const Program& program, const ResourceSpecialization& specialization) {
	const auto fits = [](size_t count, const auto& elements, uint32_t none) {
		if (count > elements.size()) return false;
		for (size_t index = 0; index < elements.size(); index++) {
			const auto root = elements[index].indirect_root;
			if (index >= count ? root >= count : root != none && root >= elements.size()) return false;
		}
		return true;
	};
	return fits(program.info.buffers.size(), specialization.buffers, BufferResource::NoIndirectBuffer) &&
	       fits(program.info.images.size(), specialization.images, ImageResource::NoIndirectImage) &&
	       fits(program.info.samplers.size(), specialization.samplers,
	            SamplerResource::NoIndirectSampler);
}

void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization) {
	EXIT_IF(!program.resource_tracking_complete || program.shader_info_complete);
	EXIT_IF(program.info.buffers.size() > specialization.buffers.size() ||
	        program.info.images.size() > specialization.images.size() ||
	        program.info.samplers.size() > specialization.samplers.size());

	auto& buffers = program.info.buffers;
	const auto original_buffer_count = buffers.size();
	buffers.reserve(specialization.buffers.size());
	for (uint32_t index = 0; index < specialization.buffers.size(); ++index) {
		const auto& source = specialization.buffers[index];
		if (index >= buffers.size()) {
			EXIT_IF(source.indirect_root >= original_buffer_count);
			buffers.push_back(buffers[source.indirect_root]);
		}
		auto& buffer                      = buffers[index];
		buffer.packed_stride              = source.packed_stride;
		buffer.descriptor_format          = source.descriptor_format;
		buffer.descriptor_swizzle         = source.descriptor_swizzle;
		buffer.indirect_root              = source.indirect_root;
		buffer.indirect_mapping_offset    = source.indirect_mapping_offset;
		buffer.indirect_search_iterations = source.indirect_search_iterations;
		buffer.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < buffers.size(); ++index) {
		const auto root = buffers[index].indirect_root;
		if (root != BufferResource::NoIndirectBuffer) {
			EXIT_IF(root >= buffers.size());
			buffers[root].indirect_resources.push_back(index);
		}
	}
	auto& images = program.info.images;
	const auto original_image_count = images.size();
	images.reserve(specialization.images.size());
	for (uint32_t index = 0; index < specialization.images.size(); index++) {
		const auto& source = specialization.images[index];
		if (index >= images.size()) {
			EXIT_IF(source.indirect_root >= original_image_count);
			images.push_back(images[source.indirect_root]);
		}
		auto& image                      = images[index];
		image.numeric_class              = source.numeric_class;
		image.dimension                  = source.dimension;
		image.mip_count                  = source.mip_count;
		image.conversion_format          = source.conversion_format;
		image.shader_swizzle             = source.shader_swizzle;
		image.srgb_decode                = source.srgb_decode;
		image.indirect_root              = source.indirect_root;
		image.indirect_mapping_offset    = source.indirect_mapping_offset;
		image.indirect_search_iterations = source.indirect_search_iterations;
		image.table                      = source.table;
		image.table_capacity             = source.table_capacity;
		image.table_mapping_offset       = source.table_mapping_offset;
		image.table_entry_mask           = source.table_entry_mask;
		image.cube                       = source.cube;
		image.skip_degamma               = source.skip_degamma;
		image.heap                       = source.heap;
		image.indirect_resources.clear();
	}
	for (uint32_t index = 0; index < images.size(); index++) {
		const auto root = images[index].indirect_root;
		if (root != ImageResource::NoIndirectImage) {
			EXIT_IF(root >= images.size());
			images[root].indirect_resources.push_back(index);
		}
	}

	auto& samplers      = program.info.samplers;
	auto& sampled_pairs = program.info.sampled_pairs;
	const auto base_sampler_count = samplers.size();
	samplers.reserve(specialization.samplers.size());
	for (uint32_t index = 0; index < specialization.samplers.size(); ++index) {
		const auto& source = specialization.samplers[index];
		if (index >= samplers.size()) {
			EXIT_IF(source.indirect_root >= base_sampler_count);
			samplers.push_back(samplers[source.indirect_root]);
			samplers.back().indirect_resources.clear();
		}
		samplers[index].indirect_root = source.indirect_root;
		samplers[index].indirect_mapping_offset = source.indirect_mapping_offset;
		samplers[index].indirect_search_iterations = source.indirect_search_iterations;
	}
	for (uint32_t index = 0; index < samplers.size(); ++index) {
		const auto root = samplers[index].indirect_root;
		if (root != SamplerResource::NoIndirectSampler) {
			EXIT_IF(root >= samplers.size());
			samplers[root].indirect_resources.push_back(index);
		}
	}
	SamplerPlan sampler_plan;
	EXIT_IF(!BuildSamplerPlan(program.info, sampler_plan));
	const auto original_sampler_count = samplers.size();
	samplers.reserve(sampler_plan.bindings.size());
	for (uint32_t index = 0; index < sampler_plan.bindings.size(); index++) {
		const auto& binding = sampler_plan.bindings[index];
		if (index >= original_sampler_count) {
			samplers.push_back(samplers[binding.source]);
		}
		samplers[index].snapshot_index = binding.source;
		samplers[index].force_point_filtering = binding.type == SamplerClass::Integer;
		samplers[index].integer_border        = binding.type != SamplerClass::Float;
	}
	for (uint32_t index = 0; index < sampler_plan.bindings.size(); index++) {
		const auto& binding = sampler_plan.bindings[index];
		for (auto& candidate: samplers[index].indirect_resources) {
			candidate = sampler_plan.mapping[candidate][static_cast<uint32_t>(binding.type)];
			EXIT_IF(candidate == UINT32_MAX);
		}
	}
	for (auto& pair: sampled_pairs) {
		const auto type = static_cast<uint32_t>(ClassifySampler(images[pair.image]));
		pair.sampler = sampler_plan.mapping[pair.sampler][type];
		EXIT_IF(pair.sampler == UINT32_MAX);
		samplers[pair.sampler].depth_compare |= images[pair.image].depth_compare;
		for (const auto candidate: samplers[pair.sampler].indirect_resources)
			samplers[candidate].depth_compare |= images[pair.image].depth_compare;
	}

	auto& memory_info = program.memory_info;
	// Resolve empty scalar-table words before folding the indirect descriptors that use them.
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer) continue;
			const auto& memory = memory_info[inst.Flags<MemoryFlags>().index];
			// Descriptor and SRT reads only plan resources: they hold no buffer index.
			if (memory.kind == ResourceKind::ScalarBuffer && !memory.planning_only &&
			    specialization.buffers[memory.resource].scalar_empty &&
			    buffers[memory.resource].indirect_root == BufferResource::NoIndirectBuffer)
				inst.ReplaceUsesWith(Value(0u));
		}
	}
	const ImageRemap image_remap(specialization);
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
			if (inst.GetOpcode() == ValueOpcode::GetImageResource) {
				inst.SetFlags(image_remap[inst.Flags<uint32_t>()]);
				continue;
			}
			if (BufferAccessOf(inst.GetOpcode()) == BufferAccess::Read) {
				const auto& memory = memory_info[inst.Flags<MemoryFlags>().index];
				const bool native_oob = memory.kind == ResourceKind::Buffer &&
				    specialization.buffers[memory.resource].zero_stride_oob &&
				    buffers[memory.resource].indirect_root == BufferResource::NoIndirectBuffer;
				const bool null_indirect = memory.kind == ResourceKind::IndirectBuffer &&
				    NullBufferDescriptor(inst.Arg(0));
				if (native_oob || null_indirect) {
					// Bounds mode 0 checks offset >= stride, so zero stride
					// makes every vector read out of bounds. An all-zero V# is unbound.
					const auto count = BufferComponentCount(inst.GetOpcode());
					std::array<Value, 4> values {Value(0u), Value(0u), Value(0u), Value(0u)};
					if (native_oob && memory.formatted && !memory.typed) {
						const auto& buffer = buffers[memory.resource];
						const auto format = Format::GetFormatInfo(buffer.descriptor_format);
						const bool d16 = memory.data_bits == 16u;
						const auto components = d16 ? memory.component_count : count;
						std::array<uint32_t, 4> constants {};
						for (uint32_t component = 0; component < components; component++) {
							if (format.type == Format::ComponentType::Unknown ||
							    GetDstSel(buffer.descriptor_swizzle, component) != 1u) continue;
							auto one = Format::FormattedConstantBits(
							    format, Format::FormattedSourceKind::One);
							if (d16 && one == 0x3f800000u) one = 0x3c00u;
							constants[d16 ? component / 2u : component] |=
							    one << (d16 ? (component % 2u) * 16u : 0u);
						}
						for (uint32_t component = 0; component < count; component++) {
							if (constants[component] == 0u) continue;
							values[component] = Value(&*block->PrependNewInst(
							    it, ValueOpcode::SelectU32,
							    {inst.Arg(inst.NumArgs() - 1), Value(constants[component]), Value(0u)}));
						}
					}
					Value result = values[0];
					switch (inst.GetType()) {
						case Type::U8: result = Value(uint8_t {0}); break;
						case Type::U16: result = Value(uint16_t {0}); break;
						case Type::U32x2:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x2,
							                                      {values[0], values[1]}));
							break;
						case Type::U32x3:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x3,
							                                      {values[0], values[1], values[2]}));
							break;
						case Type::U32x4:
							result = Value(&*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x4,
							                                      {values[0], values[1], values[2], values[3]}));
							break;
						default: break;
					}
					inst.ReplaceUsesWith(result);
				}
				continue;
			}
			const auto image_opcode = ImageOpcodeInfoOf(inst.GetOpcode());
			if (image_opcode.access == ImageAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			EXIT_IF(index >= memory_info.size());
			auto& memory = memory_info[index];
			EXIT_IF(memory.resource >= images.size());
			const auto& image = images[memory.resource];
			if (specialization.images[memory.resource].fmask) {
				EXIT_IF(inst.GetOpcode() != ValueOpcode::ImageRead || memory.data_bits != 32u);
				// Vulkan MSAA stores each sample directly; FMASK's four-bit fragment indices
				// therefore map each coverage sample to the same host sample.
				constexpr uint32_t indices[] = {0x76543210u, 0xfedcba98u};
				std::array<Value, 2> fragments;
				for (uint32_t component = 0; component < fragments.size(); component++) {
					const auto selected = block->PrependNewInst(
					    it, ValueOpcode::SelectU32, {inst.Arg(2), Value(indices[component]), Value(0u)});
					fragments[component] = Value(&*selected);
				}
				const auto result = block->PrependNewInst(
				    it, ValueOpcode::CompositeConstructU32x4,
				    {fragments[0], fragments[1], Value(0u), Value(0u)});
				inst.ReplaceUsesWith(Value(&*result));
				continue;
			}
			if (image_opcode.needs_sampler &&
			    memory.sampler < original_sampler_count) {
				const auto type = static_cast<uint32_t>(ClassifySampler(image));
				memory.sampler = sampler_plan.mapping[memory.sampler][type];
				EXIT_IF(memory.sampler == UINT32_MAX);
			}
			EXIT_IF(image.indirect_root == memory.resource &&
			        inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageGatherRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageQueryDimensions &&
			        inst.GetOpcode() != ValueOpcode::ImageRead &&
			        inst.GetOpcode() != ValueOpcode::ImageWrite);
			EXIT_IF(image.table_capacity != 0u && inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageQueryDimensions);
			memory.resource = image_remap[memory.resource];
		}
	}
	for (auto& pair: sampled_pairs) {
		pair.image = image_remap[pair.image];
	}
	for (auto& image: images) {
		if (image.indirect_root != ImageResource::NoIndirectImage) {
			image.indirect_root = image_remap[image.indirect_root];
		}
		for (auto& resource: image.indirect_resources) {
			resource = image_remap[resource];
		}
	}
	image_remap.Apply(images);
}

namespace {

using ControlPhis = decltype(ResourcePlan::control_phi_predecessors);

// Instructions come first, so that any value can refer to them by index, then their arguments.
// Plan Phis have no blocks; a control Phi lists the plan blocks of its arguments after them.
void IoValues(Writer& writer, const std::list<Inst>& values, const ControlPhis& phis) {
	IoAll(writer, static_cast<uint32_t>(values.size()));
	for (const auto& inst: values) {
		writer.index.emplace(&inst, static_cast<uint32_t>(writer.index.size()));
		IoAll(writer, inst.GetOpcode(), inst.Flags<uint64_t>());
	}
	for (const auto& inst: values) {
		if (inst.GetOpcode() != ValueOpcode::Phi) {
			for (size_t index = 0; index < inst.NumArgs(); ++index) IoAll(writer, inst.Arg(index));
			continue;
		}
		std::vector<Value> args;
		for (size_t index = 0; index < inst.NumArgs(); ++index) {
			EXIT_IF(inst.PhiBlock(index) != nullptr);
			args.push_back(inst.Arg(index));
		}
		const auto found = phis.find(&inst);
		IoAll(writer, args, found != phis.end() ? found->second : std::vector<uint32_t> {});
	}
}

void IoValues(Reader& reader, std::list<Inst>& values, ControlPhis& phis) {
	uint32_t count = 0;
	Io(reader, count);
	count = reader.Count(count);
	for (uint32_t index = 0; index < count && reader.ok; ++index) {
		ValueOpcode opcode {};
		uint64_t    flags = 0;
		IoAll(reader, opcode, flags);
		reader.ok = reader.ok && opcode < ValueOpcode::Count;
		if (reader.ok) reader.insts.push_back(&values.emplace_back(opcode, flags));
	}
	for (auto* inst: reader.insts) {
		if (inst->GetOpcode() != ValueOpcode::Phi) {
			for (size_t index = 0; index < inst->NumArgs(); ++index) {
				Value arg;
				Io(reader, arg);
				inst->SetArg(index, arg);
			}
			continue;
		}
		std::vector<Value>    args;
		std::vector<uint32_t> predecessors;
		IoAll(reader, args, predecessors);
		for (const auto arg: args) inst->AddPhiOperand(nullptr, arg);
		if (!predecessors.empty()) phis.emplace(inst, std::move(predecessors));
	}
}

// Binds every plan field: one added later fails to compile here until it is stored or derived.
template <class S, class P> void IoPlan(S& io, P& plan) {
	auto& [stage, shader_hash, user_data_base, user_data_count, values, memory_info, descriptor_sources,
	       selector_groups, control_flow, control_phis, srt_reads, source_reads, capture_specialization_reads,
	       srt_plan_complete, resource_tracking_complete, info, buffer_vector_access,
	       buffer_feeds_descriptors, uniform_fill,
	       // Derived on load.
	       descriptor_refresh, descriptor_word_flat, srt_code, srt_user_data, srt_captures, srt_scratch, material_keys, selector_keys] = plan;
	IoValues(io, values, control_phis);
	IoAll(io, stage, shader_hash, user_data_base, user_data_count, memory_info, descriptor_sources,
	      selector_groups, control_flow, srt_reads, source_reads, capture_specialization_reads,
	      srt_plan_complete, resource_tracking_complete, info, buffer_vector_access,
	      buffer_feeds_descriptors, uniform_fill);
}

} // namespace

std::vector<uint8_t> SaveResourcePlan(const ResourcePlan& plan) {
	Writer writer;
	IoPlan(writer, plan);
	return std::move(writer.bytes);
}

bool LoadResourcePlan(std::span<const uint8_t> bytes, ResourcePlan& plan) {
	Reader reader {bytes};
	IoPlan(reader, plan);
	return reader.ok && reader.bytes.empty() && DerivePlanState(plan);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
