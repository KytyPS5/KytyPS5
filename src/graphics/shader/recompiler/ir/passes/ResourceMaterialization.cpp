#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <fmt/format.h>
#include <functional>
#include <numeric>
#include <unordered_set>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;

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

void MarkCleanReads(const std::unordered_set<const Inst*>& planned_reads,
                    const DescriptorSource* source, Value extra = {}) {
	if (source == nullptr && extra.IsEmpty()) {
		return;
	}
	std::vector<Value> pending;
	if (source != nullptr) {
		pending.assign(source->dwords.begin(), source->dwords.begin() + source->dword_count);
	}
	if (!extra.IsEmpty()) pending.push_back(extra);
	std::vector<const Inst*> visited;
	while (!pending.empty()) {
		auto* inst = pending.back().Resolve().TryInstruction();
		pending.pop_back();
		if (inst == nullptr || std::ranges::find(visited, inst) != visited.end()) {
			continue;
		}
		visited.push_back(inst);
		if (planned_reads.contains(inst)) {
			auto flags = inst->Flags<SrtReadFlags>();
			flags.clean = 1u;
			inst->SetFlags(flags);
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			pending.push_back(inst->Arg(arg));
		}
	}
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
	return indirect.sources.empty() && !indirect.selector &&
	       (indirect.key_count.IsEmpty() || indirect.table_maximum) && indirect.workgroup_axis == UINT32_MAX &&
	       table != nullptr && table->dword_count == 4u;
}

bool EvaluateIndirectCount(const DescriptorSource::IndirectDescriptor& indirect,
                           const SrtRuntime& runtime, SrtWalker& clean, uint32_t& count) {
	if (indirect.workgroup_axis != UINT32_MAX) {
		if (runtime.workgroup_counts.size() != 3u || indirect.workgroup_axis >= 3u) return false;
		count = std::ranges::find(runtime.workgroup_counts, 0u) != runtime.workgroup_counts.end()
		    ? 0u : runtime.workgroup_counts[indirect.workgroup_axis];
		return true;
	}
	if (!clean.Evaluate(indirect.key_count, count)) return false;
	if (indirect.signed_count && std::bit_cast<int32_t>(count) < 0) count = 0;
	return true;
}

bool ResolveSelectorKeys(const DescriptorSource::IndirectDescriptor& indirect,
                         const DescriptorValue& material_value, const SrtRuntime& runtime,
                         SrtWalker& clean, std::vector<uint32_t>& keys) {
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
	if (selector->rows) {
		const auto& rows = *selector->rows;
		DescriptorValue count_value;
		ShaderBufferResource counts, material;
		if (!clean.EvaluateDescriptor(rows.source, count_value) ||
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
			// Coalesce small gaps, but interpret only selected prefixes. The strict
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
		std::array<uint32_t, 4> masks {};
		uint32_t first = 0, count = 0;
		if (!DecodeBufferDescriptor(material_value, material) ||
		    !clean.Evaluate(indirect.selector_first, first) ||
		    !EvaluateIndirectCount(indirect, runtime, clean, count) || count != masks.size() * 32u ||
		    selector->stride != sizeof(masks) || selector->offset != 0u ||
		    material.Type() != 0u || material.Stride() != sizeof(masks) ||
		    material.SwizzleEnabled() || material.AddTid() || material.OutOfBounds() != 0u ||
		    (material.Base48() & 3u) != 0u) return false;
		if (material.Format() != Prospero::BufferFormat::kInvalid) {
			const auto words = material.GetSize() / sizeof(uint32_t);
			// Bitmap storage can grow with resolution while its candidate domain stays
			// at 128 keys. Bound scratch memory, preserving each coherent read's capture.
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
		if (!clean.Evaluate(indirect.selector_first, first) ||
		    !EvaluateIndirectCount(indirect, runtime, clean, count) || count > MaxIndirectDescriptorProbes)
			return false;
		// SMEM uses its instruction's byte stride; V# stride only supplies its bounds.
		const auto step = selector->stride != 0u ? selector->stride : material.Stride();
		if (selector->stride == 0u &&
		    (material.Type() != 0u || material.SwizzleEnabled() || material.AddTid() ||
		     material.OutOfBounds() != 0u || uint64_t {selector->offset} + 4u > step ||
		     uint64_t {first} + count > material.NumRecords())) return false;
		const auto last = count != 0u ? uint64_t {first} + count - 1u : 0u;
		if (last > UINT32_MAX) return false;
		const auto end = count != 0u ? last * step + selector->offset + 4u : 0u;
		if (end > uint64_t {UINT32_MAX} + 1u ||
		    (indirect.workgroup_axis == UINT32_MAX && end > size))
			return false;
		if (!read_keys(base, size, uint64_t {first} * step + selector->offset, step, count))
			return false;
	} else if (!indirect.selector_mask.IsEmpty()) {
		uint32_t mask = 0;
		uint32_t count = 0;
		if (material_value.dword_count != 2u ||
		    !clean.Evaluate(indirect.selector_mask, mask) ||
		    !EvaluateIndirectCount(indirect, runtime, clean, count) || count == 0u || count > 32u) {
			return false;
		}
		if (count < 32u) mask &= (1u << count) - 1u;
		const auto material_base =
		    (static_cast<uint64_t>(material_value.dwords[1]) << 32u) | material_value.dwords[0];
		keys.reserve(std::popcount(mask));
		while (mask != 0u) {
			const auto index = std::countr_zero(mask);
			const auto offset = static_cast<uint64_t>(selector->offset) +
			                    static_cast<uint64_t>(index) * selector->stride;
			if (offset > UINT32_MAX) return false;
			uint32_t key = 0;
			if (!read_selector(material_base, UINT64_MAX, static_cast<uint32_t>(offset),
			                   {&key, 1})) return false;
			keys.push_back(key);
			mask &= mask - 1u;
		}
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
		if (selector->bit_offset != 0u || selector->bit_mask != UINT32_MAX || selector->maximum != UINT32_MAX)
			for (auto& key: keys)
				key = std::min((key >> selector->bit_offset) & selector->bit_mask, selector->maximum);
		std::ranges::sort(keys);
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	}
	return true;
}

template <typename Specialization, typename Normalize>
bool MaterializeIndirectDescriptor(const ResourcePlan&                         program,
                                   uint32_t source_index,
                                   uint32_t resource_index, uint32_t dword_count, uint32_t read_dwords,
                                   const SrtRuntime& runtime, SrtWalker& clean,
                                   ResourceSnapshot&             snapshot,
                                   std::vector<DescriptorValue>& descriptors,
                                   std::vector<Specialization>&  specializations,
                                   uint32_t maximum_resources, Normalize&& normalize) {
	const auto& indirect = *program.descriptor_sources[source_index].indirect_descriptor;
	const auto  descriptor_bytes = read_dwords * sizeof(uint32_t);
	const auto& sources = indirect.sources;
	const auto* selector = indirect.selector ? &*indirect.selector : nullptr;
	auto* selector_keys = selector != nullptr
	    ? &program.selector_keys[program.selector_groups[source_index]] : nullptr;
	auto& keys = selector_keys != nullptr ? selector_keys->values : program.material_keys;
	const auto children_begin = descriptors.size();
	const auto root_resource = specializations[resource_index];
	const auto intern_candidate = [&](const DescriptorValue& candidate, uint32_t& ordinal) {
		ordinal = 0u;
		if (candidate == descriptors[resource_index]) return true;
		const auto found = std::find(descriptors.begin() + children_begin, descriptors.end(), candidate);
		ordinal = static_cast<uint32_t>(found - descriptors.begin() - children_begin + 1u);
		if (found == descriptors.end()) {
			if (descriptors.size() >= maximum_resources) return false;
			descriptors.push_back(candidate);
			auto child = root_resource;
			child.indirect_root = resource_index;
			specializations.push_back(child);
		}
		return true;
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
		    "child_offset={} indexed_pointer={} candidates={} capacity={} {}",
		    reason, resource_index, source_index, indirect.table_source,
		    table_value.dwords[0], table_value.dwords[1], table_value.dwords[2], table_value.dwords[3],
		    table_base, table_size, table.Type(), table.Stride(), table.NumRecords(),
		    table.SwizzleEnabled(), table.AddTid(), table.OutOfBounds(),
		    indirect.table_maximum ? fmt::format("{}", *indirect.table_maximum) : "none",
		    indirect.table_offset, indirect.table_immediate, indirect.table_record_bytes,
		    pointer != nullptr ? pointer->count : 1u, pointer != nullptr ? pointer->stride : 0u,
		    pointer != nullptr ? pointer->offset : 0u, pointer != nullptr && pointer->indexed,
		    descriptors.size(), maximum_resources, details));
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
	if (sources.empty()) {
		if (selector == nullptr) keys.clear();
		DescriptorValue material_value;
		if ((selector != nullptr && !selector_keys->resolved &&
		     !clean.EvaluateDescriptor(selector->source, material_value)) ||
		    !clean.EvaluateDescriptor(indirect.table_source, table_value)) {
			return false;
		}
		if (table_value.dword_count == 2u) {
			table_base = (static_cast<uint64_t>(table_value.dwords[1]) << 32u) | table_value.dwords[0];
		} else if (DecodeBufferDescriptor(table_value, table)) {
			table_base = table.Base48();
			table_size = table.GetSize();
		} else {
			return false;
		}
		if (IsBoundedDescriptorTable(program, indirect)) {
			if (!indirect.table_scalar && table.Format() == Prospero::BufferFormat::kInvalid) {
				descriptors[resource_index] = {.dword_count = dword_count};
				return true;
			}
			if (table_size > UINT32_MAX ||
			    (indirect.table_record_bytes != 0u &&
			     (table.Type() != 0u || table.SwizzleEnabled() || table.AddTid() ||
			      table.OutOfBounds() != 0u || (table_base & 3u) != 0u ||
			      (table.Stride() & 3u) != 0u || table.Stride() < indirect.table_record_bytes)))
				return failed("table geometry");
			const auto* pointer = indirect.pointer_table ? &*indirect.pointer_table : nullptr;
			const bool indexed_pointer = pointer != nullptr && pointer->indexed;
			const auto child_count = pointer != nullptr ? pointer->count : 1u;
			uint64_t domain = indirect.table_maximum ? uint64_t {*indirect.table_maximum} + 1u
			                                       : uint64_t {1} << 32u;
			if (!indirect.key_count.IsEmpty()) {
				uint32_t key_count = 0u;
				if (!EvaluateIndirectCount(indirect, runtime, clean, key_count)) return false;
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
			const uint64_t first = ((indirect.table_maximum ? indirect.table_offset
			    : indirect.table_offset % step) & ~uint64_t {3}) +
			                       (indirect.table_immediate & ~3u);
			if (!indirect.table_maximum) domain /= step;
			const auto count = table_size >= first + 4u
			    ? std::min((table_size - first - 4u) / step + 1u, domain)
			    : 0u;
			if (child_count == 0u || (!image_heap && count * child_count > MaxIndirectDescriptorProbes))
				return failed("candidate count", fmt::format("rows={} limit={}", count, MaxIndirectDescriptorProbes));
			// Affine SMEM offsets wrap in U32 before adding the aligned immediate.
			// Image heaps retain one bulk snapshot and their absolute byte-offset mapping.
			if (image_heap) {
				keys.resize(table_size / 4u);
				if (!ReadScalarTable(table_base, table_size, 0u, runtime, keys))
					return read_failed("image heap read", 0u, 0u, table_base, 0u, keys.size() * sizeof(uint32_t));
			}
			const auto mapping_offset = snapshot.flattened_srt.size();
			descriptors[resource_index] = {.dword_count = dword_count};
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
						std::copy_n(keys.begin() + table_word,
						            std::min<size_t>(read_dwords, keys.size() - table_word),
						            candidate.dwords.begin());
					}
					if (!normalize(candidate))
						return normalization_failed(table_offset, word, step,
						                            indirect.table_maximum ? "key" : "probe");
					uint32_t ordinal;
					if (!intern_candidate(candidate, ordinal)) return false;
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
			if (!EvaluateIndirectCount(indirect, runtime, clean, key_count)) return false;
			if (!indirect.selector_first.IsEmpty() && !clean.Evaluate(indirect.selector_first, first_key))
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
		} else {
			if (!indirect.selector_mask.IsEmpty() && table_value.dword_count != 2u) return false;
			if (!selector_keys->resolved) {
				if (!ResolveSelectorKeys(indirect, material_value, runtime, clean, keys)) return false;
				selector_keys->resolved = true;
			}
		}
	}

	const auto mapping_offset = snapshot.flattened_srt.size();
	const auto key_count = sources.empty() ? keys.size() : sources.size();
	snapshot.flattened_srt.resize(mapping_offset + 1u + key_count * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(key_count);
	for (uint32_t entry = 0; entry < key_count; ++entry) {
		const auto key = sources.empty() ? keys[entry] : entry;
		uint64_t table_offset = 0;
		DescriptorValue candidate;
		candidate.dword_count = dword_count;
		if (sources.empty()) {
			table_offset = uint64_t {(key * indirect.table_stride + indirect.table_offset) & ~3u} +
			               (indirect.table_immediate & ~3u);
			if (!ReadScalarTable(table_base, table_size, table_offset, runtime,
			                     std::span(candidate.dwords).first(read_dwords))) {
				return read_failed("table descriptor read", key, 0u, table_base, table_offset, descriptor_bytes);
			}
		} else if (!clean.EvaluateDescriptor(sources[entry], candidate)) {
			return false;
		}
		if (!normalize(candidate))
			return normalization_failed(table_offset, key, indirect.table_stride, "key");
		uint32_t ordinal = 0;
		if (entry == 0) {
			descriptors[resource_index] = candidate;
		} else if (!intern_candidate(candidate, ordinal)) {
			return false;
		}
		snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = key;
		snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
	}
	if (descriptors.size() == children_begin) {
		snapshot.flattened_srt.resize(mapping_offset);
	} else {
		auto& root                      = specializations[resource_index];
		root.indirect_root              = resource_index;
		root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
		root.indirect_search_iterations = std::bit_width(key_count);
	}
	return true;
}

} // namespace

struct SamplerPlan {
	struct Binding {
		uint32_t     source;
		SamplerClass type;
	};
	std::array<std::array<uint32_t, 2>, ShaderInfo::MaxSamplers> mapping;
	std::array<Binding, ShaderInfo::MaxSamplers>                bindings;
	uint32_t                                                  sampler_count = 0;
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
		if (NullImageDescriptor(descriptor)) {
			image.numeric_class = base.atomic ? Prospero::TextureNumericClass::Uint
			                                  : Prospero::TextureNumericClass::Float;
			image.dimension     = Decoder::ImageDimension::Dim2D;
			image.cube          = false;
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
		image.conversion_format = ImageConversionFormat(format);
		if (storage || image.conversion_format != Prospero::BufferFormat::kInvalid) {
			image.shader_swizzle = DescriptorImageSwizzle(descriptor);
		}
		const auto raw_sint_format = RawSignedStorageFormat(base, format);
		const bool raw_sint_storage = raw_sint_format != Prospero::BufferFormat::kInvalid;
		if (raw_sint_format == Prospero::BufferFormat::k16UInt) image.conversion_format = format;
		image.numeric_class = Prospero::SampledTextureNumericClass(
		    Prospero::RemapTextureFormat(format));
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
		if (key_count == 0u ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(key_count) * (root.indirect_search_iterations == 0u ? 1u : 2u) >
		        snapshot.flattened_srt.size()) {
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
			if ((!same_coordinates && !(is_2d(image.dimension) && is_2d(image_class.dimension)) &&
			     !unfiltered_coordinates) ||
			    image.mip_count != image_class.mip_count ||
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
	if (base.samplers.size() > plan.mapping.size()) {
		return false;
	}
	std::array<uint8_t, ShaderInfo::MaxSamplers> usage {};
	plan.sampler_count = static_cast<uint32_t>(base.samplers.size());
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
			const auto target = first ? index : plan.sampler_count++;
			if (target >= ShaderInfo::MaxSamplers) return false;
			mapping[type]         = target;
			plan.bindings[target] = {index, static_cast<SamplerClass>(type)};
			first                = false;
		}
	}
	return true;
}

template <typename Predicate>
static std::vector<ResourceBlock> ResourceControlFlow(const Program& program, const Predicate& predicate) {
	// The renderer checks captured scalar reads against final buffer and image write ranges.
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
				if (image.needs_sampler) {
					block.sources.push_back(program.info.samplers.at(memory.sampler).source);
				}
			}
		}
		std::ranges::sort(block.sources);
		block.sources.erase(std::unique(block.sources.begin(), block.sources.end()),
		                    block.sources.end());
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
	if (program.stage != ShaderType::Compute || program.blocks.empty() || program.info.uses_dma ||
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
		if (source->GetOpcode() == ValueOpcode::LoadBufferU32) {
			plan.requires_specialization_memory = true;
			plan.capture_specialization_reads   = true;
		}
		auto& target =
		    plan.value_storage.emplace_back(source->GetOpcode(), source->Flags<uint64_t>());
		if (source->GetOpcode() == ValueOpcode::LoadAddressU32 ||
		    source->GetOpcode() == ValueOpcode::ReadConstBuffer ||
		    source->GetOpcode() == ValueOpcode::LoadBufferU32) {
			target.SetFlags(SrtReadFlags {.index = source->Flags<MemoryFlags>().index});
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
			plan.requires_specialization_memory = true;
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
	plan.selector_groups.resize(plan.descriptor_sources.size(), UINT32_MAX);
	for (uint32_t source = 0; source < plan.descriptor_sources.size(); ++source) {
		const auto& descriptor = program.descriptor_sources[source].indirect_descriptor;
		if (!descriptor || !descriptor->selector) continue;
		const auto& a = *descriptor;
		uint32_t group = 0;
		for (; group < plan.selector_keys.size(); ++group) {
			const auto& b = *program.descriptor_sources[plan.selector_keys[group].source].indirect_descriptor;
			if (EquivalentSelectorDomain(program, a, b)) break;
		}
		if (group == plan.selector_keys.size()) plan.selector_keys.push_back({.source = source});
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
		    op != ValueOpcode::INotEqual32 && op != ValueOpcode::ULessThanEqual32 &&
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
	plan.uniform_fill = AnalyzeUniformFill(program);
	for (uint32_t i = 0; i < plan.uniform_fill.fill.words; ++i) {
		plan.uniform_fill.values[i] = Clone(plan.uniform_fill.values[i]);
	}
	plan.capture_specialization_reads |=
	    capture_indirect_reads || capture_control_reads || !plan.source_reads.empty();
	if (plan.capture_specialization_reads) {
		// Every former flat alias now targets its retained raw read. Mark those reads
		// on the normalized graph, so discarded Phi/EXEC dependencies stay discarded.
		std::unordered_set<const Inst*> planned_reads;
		for (const auto& read: plan.srt_reads) {
			auto* inst = read.value.ResolveInstruction();
			EXIT_IF(inst == nullptr || (inst->GetOpcode() != ValueOpcode::LoadAddressU32 &&
			                           inst->GetOpcode() != ValueOpcode::ReadConstBuffer));
			planned_reads.insert(inst);
		}
		for (const auto& source: plan.descriptor_sources) {
			if (!source.indirect_descriptor.has_value()) continue;
			const auto& indirect = *source.indirect_descriptor;
			if (indirect.selector) {
				MarkCleanReads(planned_reads, Source(plan, indirect.selector->source));
				if (indirect.selector->rows)
					MarkCleanReads(planned_reads, Source(plan, indirect.selector->rows->source));
			}
			MarkCleanReads(planned_reads, nullptr, indirect.selector_mask);
			MarkCleanReads(planned_reads, nullptr, indirect.selector_first);
			MarkCleanReads(planned_reads, nullptr, indirect.key_count);
			if (indirect.sources.empty()) {
				MarkCleanReads(planned_reads, Source(plan, indirect.table_source));
			} else {
				for (const auto candidate: indirect.sources) {
					MarkCleanReads(planned_reads, Source(plan, candidate));
				}
			}
		}
		// Clean writable addresses prove that shader writes cannot overlap captured reads.
		for (const auto& buffer: plan.info.buffers) {
			if (buffer.written) MarkCleanReads(planned_reads, Source(plan, buffer.source));
		}
		for (const auto& image: plan.info.images) {
			if (image.written) MarkCleanReads(planned_reads, Source(plan, image.source));
		}
	}
	if (capture_indirect_reads || !plan.source_reads.empty())
		plan.resource_tracking_complete &= !program.has_address_writes;
	CompileSrtWalker(plan);
	return plan;
}

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	for (auto& keys: program.selector_keys) keys.resolved = false;
	if (!program.resource_tracking_complete ||
	    ((program.requires_specialization_memory || !program.source_reads.empty()) &&
	     runtime.read_specialization_memory == nullptr)) {
		return false;
	}
	const auto uses_workgroup_count = [](const DescriptorSource& source) {
		return source.indirect_descriptor && source.indirect_descriptor->workgroup_axis != UINT32_MAX;
	};
	const bool capture_reads = program.capture_specialization_reads || !program.source_reads.empty() ||
	    (runtime.indirect_args_address != 0u &&
	     std::ranges::any_of(program.descriptor_sources, uses_workgroup_count));
	auto& reads = snapshot.specialization_reads;
	reads = program.source_reads;
	SrtReadCapture capture {runtime, reads};
	auto observed = capture_reads ? capture.ObservedRuntime() : runtime;
	SrtWalker clean(program, CleanRuntime(observed));
	SrtWalker walker(program, observed,
	                 capture_reads || program.requires_specialization_memory ? &clean : nullptr);
	if (!walker.RefreshFlatBuffer(snapshot.flattened_srt)) {
		return SpecializationFail(fmt::format("SRT refresh complete={} reads={} user_data={}",
		    program.srt_plan_complete, program.srt_reads.size(), runtime.user_data.size()));
	}
	const auto active = std::span<const uint8_t>(program.active_sources);
	std::array<uint32_t, 3> workgroup_counts {};
	if (runtime.indirect_args_address != 0u) {
		for (uint32_t source = 0; source < program.descriptor_sources.size(); ++source) {
			if ((!active.empty() && !active[source]) ||
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
	const auto words = fill.fill.words;
	std::array<uint32_t, 4> stored {};
	bool uniform_fill = words != 0;
	for (uint32_t i = 0; i < words && uniform_fill; ++i) {
		uniform_fill = clean.Evaluate(fill.values[i], stored[i]) && stored[i] == stored[0];
	}
	if (uniform_fill) {
		snapshot.uniform_fill = fill.fill;
		snapshot.uniform_fill.value = stored[0];
	}
	const auto evaluate = [&](uint32_t source, DescriptorValue& value, bool written = false) {
		if (source >= program.descriptor_sources.size()) {
			return false;
		}
		if (active.empty() || active[source]) {
			return (capture_reads && written ? clean : walker).EvaluateDescriptor(source, value) ||
			    SpecializationFail(fmt::format("descriptor evaluation source={}", source));
		}
		value = {};
		value.dword_count = program.descriptor_sources[source].dword_count;
		return true;
	};
	const auto resource_failed = [&](std::string_view kind, uint32_t resource, uint32_t source,
	                                 size_t candidates, uint32_t capacity) {
		return SpecializationFail(fmt::format("indirect {} resource={} source={} candidates={} capacity={}",
		    kind, resource, source, candidates, capacity));
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
				if (active.empty() || active[base.source]) {
					if (!MaterializeIndirectDescriptor(
					        program, base.source, i, 4u, 4u, observed, clean, snapshot,
					        snapshot.buffers, specialization.buffers, UINT32_MAX,
					        [&](DescriptorValue& value) {
						        return NormalizeIndirectBuffer(
						            value, source->indirect_descriptor->indexed, base);
					        }))
						return resource_failed("buffer", i, base.source, snapshot.buffers.size(), UINT32_MAX);
				}
			} else if (!evaluate(base.source, snapshot.buffers[i], base.written)) {
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
	}
	snapshot.images.resize(program.info.images.size());
	specialization.images.resize(program.info.images.size());
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
		    .cube = image.cube,
		};
		const auto* source = Source(program, image.source);
		if (source == nullptr) {
			return false;
		}
		if (source->indirect_descriptor.has_value()) {
			snapshot.images[i] = {.dword_count = 8u};
			if (!active.empty() && !active[image.source]) {
				continue;
			}
			const auto& indirect = *source->indirect_descriptor;
			const bool bounded_table = IsBoundedDescriptorTable(program, indirect);
			if (!MaterializeIndirectDescriptor(
			        program, image.source, i, 8u, image.r128 ? 4u : 8u, observed, clean, snapshot, snapshot.images,
			        specialization.images, UINT32_MAX, [&](DescriptorValue& value) {
				        // A broad heap also contains resources for other typed image operations.
				        if (NullImageDescriptor(value) || !ValidImageDescriptor(value, image.r128) ||
				            (bounded_table && DescriptorDimension(value, image.dimension) != image.dimension))
					        value.dwords.fill(0u);
				        return true;
			        })) {
				return resource_failed("image", i, image.source, snapshot.images.size(), UINT32_MAX);
			}
		} else {
			if (!evaluate(image.source, snapshot.images[i], image.written)) {
				return false;
			}
			if (!ValidImageDescriptor(snapshot.images[i], image.r128)) {
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
				if ((active.empty() || active[source]) && !MaterializeIndirectDescriptor(
				        program, source, i, 4u, 4u, observed, clean, snapshot, snapshot.samplers,
				        specialization.samplers, ShaderInfo::MaxSamplers,
				        [](DescriptorValue&) { return true; }))
					return resource_failed("sampler", i, source, snapshot.samplers.size(), ShaderInfo::MaxSamplers);
			} else {
				if (indirect != nullptr) source = indirect->sources[0];
				if (!evaluate(source, snapshot.samplers[i])) return false;
			}
		}
		if (program.info.samplers[base_index].gather_lod) {
			const auto control = snapshot.samplers[i].dwords[2];
			const auto filter = (control >> 26u) & 3u;
			// MipNone always selects the base level. Explicit point gathers currently require
			// encoded-zero primary and secondary bias; linear primary-mip selection is unsupported.
			if (filter > 1u || (filter == 1u && (control & 0xfffffu) != 0u)) {
				return SpecializationFail(
				    "explicit-LOD gather requires mip filtering None or Point with zero LOD biases");
			}
		}
	}
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return BuildResourceSpecialization(program, snapshot, specialization);
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
		image.indirect_root              = source.indirect_root;
		image.indirect_mapping_offset    = source.indirect_mapping_offset;
		image.indirect_search_iterations = source.indirect_search_iterations;
		image.cube                       = source.cube;
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
	samplers.reserve(sampler_plan.sampler_count);
	for (uint32_t index = 0; index < sampler_plan.sampler_count; index++) {
		const auto& binding = sampler_plan.bindings[index];
		if (index >= original_sampler_count) {
			samplers.push_back(samplers[binding.source]);
		}
		samplers[index].snapshot_index = binding.source;
		samplers[index].force_point_filtering = binding.type == SamplerClass::Integer;
		samplers[index].integer_border        = binding.type != SamplerClass::Float;
	}
	for (uint32_t index = 0; index < sampler_plan.sampler_count; index++) {
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
			if (memory.kind == ResourceKind::ScalarBuffer &&
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
			        inst.GetOpcode() != ValueOpcode::ImageRead &&
			        inst.GetOpcode() != ValueOpcode::ImageWrite);
		}
	}
	for (auto& memory: memory_info) {
		if (memory.kind == ResourceKind::Image && !memory.planning_only) {
			memory.resource = image_remap[memory.resource];
		}
	}
	for (auto& buffer: buffers) {
		if (buffer.image_alias != BufferResource::NoImageAlias) {
			buffer.image_alias = image_remap[buffer.image_alias];
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

} // namespace Libs::Graphics::ShaderRecompiler::IR
