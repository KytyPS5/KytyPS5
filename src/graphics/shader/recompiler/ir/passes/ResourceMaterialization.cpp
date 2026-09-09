#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/host_gpu/renderer/image/imageInfo.h"
#include "graphics/host_gpu/renderer/image/textureCommon.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <numeric>
#include <string>
#include <unordered_set>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;
constexpr uint64_t RegisteredBufferAddressLimit = uint64_t{1} << 40u;
constexpr uint64_t MaxIndirectImageProbes = 65536u;
// A 16-bit selector can feed multiple descriptors and their address expressions.
// Bound each selector domain independently, then apply an explicit memory budget
// to the whole coherent transaction instead of treating the first 65,536 words
// as a semantic limit. This includes 256 full-width selector columns.
constexpr uint64_t MaxBoundedSnapshotBytes = 64u * 1024u * 1024u;
constexpr uint64_t MaxBoundedSnapshotWords = MaxBoundedSnapshotBytes / sizeof(uint32_t);

bool SpecializationFail(std::string_view message) {
	g_last_specialization_error.assign(message);
	std::fprintf(stderr, "shader resource specialization failed: %.*s\n",
	             static_cast<int>(message.size()), message.data());
	std::fflush(stderr);
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
	if (type < Prospero::ImageType::kColor1D || !Prospero::IsKnownFormat(format)) {
		return false;
	}
	if (r128 && type != Prospero::ImageType::kColor1D && type != Prospero::ImageType::kColor2D &&
	    type != Prospero::ImageType::kColor2DMsaa) {
		return false;
	}
	const bool array = type == Prospero::ImageType::kColor1DArray ||
	                   type == Prospero::ImageType::kColor2DArray ||
	                   type == Prospero::ImageType::kColor2DMsaaArray ||
	                   type == Prospero::ImageType::kCube;
	if (array && ((words[4] >> 16u) & 0x1fffu) > (words[4] & 0x1fffu)) {
		return false;
	}
	if (type == Prospero::ImageType::kColor2DMsaa ||
	    type == Prospero::ImageType::kColor2DMsaaArray) {
		const auto base_level = (descriptor.dwords[3] >> 12u) & 0xfu;
		const auto fragments  = (descriptor.dwords[3] >> 16u) & 0xfu;
		const auto max_mip    = (descriptor.dwords[5] >> 4u) & 0xfu;
		return base_level == 0 && fragments >= 1 && fragments <= 3 &&
		       (r128 || max_mip == fragments);
	}
	return true;
}

uint32_t DescriptorImageSwizzle(const DescriptorValue& descriptor) {
	return descriptor.dwords[3] & 0xfffu;
}

Prospero::BufferFormat ImageConversionFormat(Prospero::BufferFormat format) {
	return Prospero::RemapTextureFormat(format) != format ? format
	                                                      : Prospero::BufferFormat::kInvalid;
}

bool RequiresPointSampler(const ImageResource& image) {
	return image.numeric_class == Prospero::TextureNumericClass::Uint ||
	       image.numeric_class == Prospero::TextureNumericClass::Sint ||
	       image.conversion_format != Prospero::BufferFormat::kInvalid;
}

bool RequiresPointSampler(const ResourceSpecialization::Image& image) {
	return image.numeric_class == Prospero::TextureNumericClass::Uint ||
	       image.numeric_class == Prospero::TextureNumericClass::Sint ||
	       image.conversion_format != Prospero::BufferFormat::kInvalid;
}

bool DescriptorIsCube(const DescriptorValue& descriptor) {
	return static_cast<Prospero::ImageType>((descriptor.dwords[3] >> 28u) & 0xfu) ==
	       Prospero::ImageType::kCube;
}

uint32_t StorageMipCount(const ImageResource& image, const DescriptorValue& descriptor) {
	if (image.mip_mode != ImageMipMode::DynamicStorage || NullImageDescriptor(descriptor)) {
		return 1;
	}
	const auto base = (descriptor.dwords[3] >> 12u) & 0xfu;
	const auto last = (descriptor.dwords[3] >> 16u) & 0xfu;
	return base <= last ? last - base + 1u : 0u;
}

bool DecodeBufferDescriptor(const DescriptorValue& descriptor, ShaderBufferResource& result) {
	if (descriptor.dword_count != std::size(result.fields)) {
		return false;
	}
	std::copy_n(descriptor.dwords.begin(), std::size(result.fields), result.fields);
	return true;
}

struct ReadCapture {
	SrtRuntime                                  source;
	std::vector<std::pair<uint64_t, uint64_t>>& ranges;
};

bool CaptureStrictRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<ReadCapture*>(userdata);
	if (!capture.source.read_specialization_memory(capture.source.userdata, address, values)) {
		return false;
	}
	capture.ranges.emplace_back(address, values.size_bytes());
	return true;
}

bool CaptureOrdinaryRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<ReadCapture*>(userdata);
	if (!capture.source.read_memory(capture.source.userdata, address, values)) {
		return false;
	}
	capture.ranges.emplace_back(address, values.size_bytes());
	return true;
}

bool WrittenBuffersDisjoint(const ResourcePlan& program, const ResourceSnapshot& snapshot,
                            std::span<const std::pair<uint64_t, uint64_t>> reads) {
	for (uint32_t i = 0; i < program.info.buffers.size(); ++i) {
		if (!program.info.buffers[i].written) continue;
		ShaderBufferResource buffer;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], buffer)) return false;
		const auto base = buffer.Base48();
		const auto size = buffer.GetSize();
		if (size == 0u) continue;
		if (size - 1u > AddressMask - base) return false;
		for (const auto [address, bytes]: reads) {
			if (bytes == 0u) continue;
			if (address > AddressMask || bytes - 1u > AddressMask - address ||
			    (address <= base ? base - address < bytes : address - base < size)) {
				return false;
			}
		}
	}
	return true;
}

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

void MarkCleanFlatSlots(const ResourcePlan& program, const DescriptorSource* source,
                        std::span<uint8_t> slots, Value extra = {}) {
	if (source == nullptr && extra.IsEmpty()) {
		return;
	}
	std::vector<Value>       pending;
	if (source != nullptr) {
		pending.assign(source->dwords.begin(), source->dwords.begin() + source->dword_count);
	}
	if (!extra.IsEmpty()) pending.push_back(extra);
	std::vector<const Inst*> visited;
	while (!pending.empty()) {
		auto value = pending.back().Resolve();
		pending.pop_back();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || std::ranges::find(visited, inst) != visited.end()) {
			continue;
		}
		visited.push_back(inst);
		if (inst->GetOpcode() == ValueOpcode::ReadConst) {
			const auto slot = inst->Arg(1).Resolve();
			if (slot.IsImmediate() && slot.GetType() == Type::U32 && slot.U32() < slots.size()) {
				slots[slot.U32()] = ResourcePlan::FlatSlotClean;
				pending.push_back(program.srt_reads[slot.U32()].value);
			}
			continue;
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			pending.push_back(inst->Arg(arg));
		}
	}
}

bool ReadScalarTable(uint64_t base, uint64_t size, uint32_t dynamic_offset,
                     const SrtRuntime& runtime, std::span<uint32_t> words) {
	const auto offset = static_cast<uint64_t>(dynamic_offset) & ~uint64_t {3};
	const auto count = std::min<uint64_t>(words.size(), offset < size ? (size - offset) / 4u : 0u);
	std::ranges::fill(words.subspan(count), 0u);
	if (count == 0u) {
		return true;
	}
	base &= AddressMask & ~uint64_t {3};
	if (offset > AddressMask - base) {
		return false;
	}
	const auto address = base + offset;
	const auto prefix = words.first(count);
	return prefix.size_bytes() - 1u <= AddressMask - address &&
	       runtime.read_specialization_memory != nullptr &&
	       runtime.read_specialization_memory(runtime.userdata, address, prefix);
}

bool MaterializeIndirectImage(const ResourcePlan& program,
                              const DescriptorSource::IndirectImage& indirect,
                              const DescriptorValue& material_value,
                              const DescriptorValue& table_value, uint32_t image_index,
                              const SrtRuntime& runtime, SrtWalker& clean,
                              ResourceSnapshot& snapshot,
                              ResourceSpecialization& specialization) {
	uint64_t table_base = 0;
	uint64_t table_size = UINT64_MAX; // Scalar addresses have no buffer descriptor bounds.
	ShaderBufferResource table;
	if (table_value.dword_count == 2u) {
		table_base = (static_cast<uint64_t>(table_value.dwords[1]) << 32u) | table_value.dwords[0];
	} else if (DecodeBufferDescriptor(table_value, table)) {
		table_base = table.Base48();
		table_size = table.GetSize();
	} else {
		return false;
	}
	auto& keys = program.material_keys;
	keys.clear();
	if (indirect.material_source == UINT32_MAX) {
		uint32_t key_count = 0;
		const bool evaluated = clean.Evaluate(indirect.key_count, key_count);
		if (std::bit_cast<int32_t>(key_count) <= 0) key_count = 0;
		if (table_value.dword_count != 2u || !evaluated ||
		    key_count > MaxIndirectImageProbes ||
		    uint64_t {indirect.table_offset} + uint64_t {key_count} * 32u > UINT32_MAX + 1ull) {
			return false;
		}
		keys.resize(key_count);
		std::iota(keys.begin(), keys.end(), 0u);
	} else if (!indirect.selector_mask.IsEmpty()) {
		uint32_t mask = 0;
		uint32_t count = 0;
		if (material_value.dword_count != 2u || table_value.dword_count != 2u ||
		    !clean.Evaluate(indirect.selector_mask, mask) ||
		    !clean.Evaluate(indirect.key_count, count) || count == 0u || count > 32u) {
			return false;
		}
		if (count < 32u) mask &= (1u << count) - 1u;
		const auto material_base =
		    (static_cast<uint64_t>(material_value.dwords[1]) << 32u) | material_value.dwords[0];
		keys.reserve(std::popcount(mask));
		while (mask != 0u) {
			const auto index = std::countr_zero(mask);
			const auto offset = static_cast<uint64_t>(indirect.selector_offset) +
			                    static_cast<uint64_t>(index) * indirect.selector_stride;
			if (offset > UINT32_MAX) return false;
			uint32_t key = 0;
			if (!ReadScalarTable(material_base, UINT64_MAX, static_cast<uint32_t>(offset),
			                     runtime, {&key, 1})) return false;
			keys.push_back(key);
			mask &= mask - 1u;
		}
		std::ranges::sort(keys);
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	} else {
		ShaderBufferResource material;
		if (!DecodeBufferDescriptor(material_value, material) || table_value.dword_count != 4u ||
		    material.Stride() != indirect.selector_stride) {
			return false;
		}
		// Enumerate every wrapped scalar-buffer offset that can pass the descriptor bounds.
		const auto step = std::gcd<uint64_t>(indirect.selector_stride, uint64_t {1} << 32u);
		const auto residue = static_cast<uint64_t>(indirect.selector_offset) % step;
		const auto limit = std::min<uint64_t>(UINT32_MAX, material.GetSize() + 3u);
		const auto probe_count = residue <= limit ? (limit - residue) / step + 1u : 0u;
		if (probe_count > MaxIndirectImageProbes) {
			return false;
		}
		keys.reserve(static_cast<size_t>(probe_count) + 1u);
		keys.push_back(0u);
		for (uint64_t offset = residue; offset <= limit && probe_count != 0u; offset += step) {
			uint32_t key = 0;
			if (!ReadScalarTable(material.Base48(), material.GetSize(),
			                     static_cast<uint32_t>(offset), runtime, {&key, 1})) {
				return false;
			}
			keys.push_back(key);
			if (limit - offset < step) {
				break;
			}
		}
		std::ranges::sort(keys);
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	}

	const auto children_begin = snapshot.images.size();
	const auto mapping_offset = snapshot.flattened_srt.size();
	const auto root_image = specialization.images[image_index];
	snapshot.flattened_srt.resize(mapping_offset + 1u + keys.size() * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(keys.size());
	for (uint32_t entry = 0; entry < keys.size(); ++entry) {
		const auto key = keys[entry];
		DescriptorValue candidate;
		candidate.dword_count = 8u;
		const auto table_offset = (key << 5u) + indirect.table_offset;
		if (!ReadScalarTable(table_base, table_size, table_offset, runtime, candidate.dwords)) {
			return false;
		}
		if (NullImageDescriptor(candidate) ||
		    !ValidImageDescriptor(candidate, program.info.images[image_index].r128)) {
			candidate.dwords.fill(0);
		}
		uint32_t ordinal = 0;
		if (entry == 0) {
			snapshot.images[image_index] = candidate;
		} else if (snapshot.images[image_index] != candidate) {
			const auto found = std::find(snapshot.images.begin() + children_begin,
			                             snapshot.images.end(), candidate);
			ordinal = static_cast<uint32_t>(found - snapshot.images.begin() - children_begin + 1u);
			if (found == snapshot.images.end()) {
				if (snapshot.images.size() >= ShaderInfo::MaxImages) {
					return false;
				}
				snapshot.images.push_back(candidate);
				auto child = root_image;
				child.indirect_root = image_index;
				specialization.images.push_back(child);
			}
		}
		snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = key;
		snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
	}
	if (snapshot.images.size() == children_begin) {
		snapshot.flattened_srt.resize(mapping_offset);
	} else {
		auto& root = specialization.images[image_index];
		root.indirect_root = image_index;
		root.indirect_mapping_offset = static_cast<uint32_t>(mapping_offset);
		root.indirect_search_iterations = std::bit_width(keys.size());
	}
	return true;
}

bool ReadInlineImageTable(const DescriptorSource::InlineDescriptor::ImageTable& table,
                          const DescriptorValue& address_value, uint32_t index,
                          const SrtRuntime& runtime, DescriptorValue& result) {
	if (address_value.dword_count != 2u) {
		return false;
	}
	const auto base = ((static_cast<uint64_t>(address_value.dwords[1]) << 32u) |
	                   address_value.dwords[0]) & AddressMask & ~uint64_t {3};
	// Match SrtWalker::EvaluateRawRead: align the signed immediate and unsigned scalar
	// offset separately, then add them to the aligned 48-bit address without wrapping.
	const auto immediate = static_cast<int64_t>(static_cast<int32_t>(table.table_offset));
	const auto relative = (immediate & ~int64_t {3}) + static_cast<int64_t>(index << 5u);
	uint64_t address = 0;
	if (relative < 0) {
		const auto magnitude = uint64_t {0} - static_cast<uint64_t>(relative);
		if (magnitude > base) {
			return false;
		}
		address = base - magnitude;
	} else {
		if (static_cast<uint64_t>(relative) > AddressMask - base) {
			return false;
		}
		address = base + static_cast<uint64_t>(relative);
	}
	if (address > AddressMask - 31u) {
		return false;
	}
	DescriptorValue next;
	next.dword_count = 8u;
	for (uint32_t dword = 0; dword < 8u; dword++) {
		if (!ReadSpecializationWord(runtime, address + dword * 4u, next.dwords[dword])) {
			return false;
		}
	}
	if (NullImageDescriptor(next) || !ValidImageDescriptor(next)) {
		next.dwords.fill(0u);
	}
	result = next;
	return true;
}

bool MaterializeInlineImage(const DescriptorSource::InlineDescriptor& image,
                            const DescriptorSource::InlineDescriptor* sampler,
                            const DescriptorValue& buffer_value,
                            const DescriptorValue* table_address, uint32_t pc,
                            const SrtRuntime& runtime, IndirectImage& result) {
	ShaderBufferResource buffer;
	if (!DecodeBufferDescriptor(buffer_value, buffer) || image.selector_stride == 0u ||
	    (image.descriptor_dwords != 4u && image.descriptor_dwords != 8u) ||
	    image.descriptor_offset > UINT32_MAX - (image.descriptor_dwords - 1u) * 4u ||
	    (sampler != nullptr && (image.buffer_source != sampler->buffer_source ||
	                            image.selector_stride != sampler->selector_stride ||
	                            image.selector_limit != sampler->selector_limit ||
	                            sampler->descriptor_dwords != 4u ||
	                            sampler->descriptor_offset > UINT32_MAX - 12u ||
	                            sampler->image_table.has_value()))) {
		return SpecializationFail(fmt::format("inline sampled pair at pc 0x{:08x} has invalid metadata", pc));
	}
	if (buffer.Type() != 0u) {
		return SpecializationFail(fmt::format(
		    "inline sampled pair at pc 0x{:08x}: invalid scalar-buffer descriptor type={}",
		    pc, buffer.Type()));
	}
	if (image.image_table.has_value() &&
	    (table_address == nullptr || image.image_table->index_shift >= 32u ||
	     image.image_table->index_mask > 255u ||
	     static_cast<int64_t>(static_cast<int32_t>(image.image_table->table_offset)) + 28 > INT32_MAX)) {
		return SpecializationFail(fmt::format(
		    "inline image table at pc 0x{:08x} requires a bounded 8-bit index and valid raw address metadata", pc));
	}
	const auto size = ScalarBufferSize(buffer);
	const auto step = image.selector_limit != 0u
	                      ? static_cast<uint64_t>(image.selector_stride)
	                      : std::gcd<uint64_t>(image.selector_stride, uint64_t {1} << 32u);
	const auto first_offset = sampler != nullptr
	                              ? std::min(image.descriptor_offset, sampler->descriptor_offset)
	                              : image.descriptor_offset;
	// The live key is the wrapped U32 multiplication result. Enumerating only record starts
	// would miss wrap aliases: for stride 872 every multiple of eight is reachable.
	// Match ReadScalarBufferWord's widened immediate addition and independent dword bounds.
	const auto last_byte = size >= 4u ? ((size - 4u) & ~uint64_t {3}) + 3u : 0u;
	const auto probe_count = image.selector_limit != 0u
	                             ? static_cast<uint64_t>(image.selector_limit)
	                             : size >= 4u && last_byte >= first_offset
	                                   ? std::min<uint64_t>(UINT32_MAX,
	                                                        last_byte - first_offset) /
	                                             step +
	                                         1u
	                                   : 0u;
	const auto fail = [&](std::string_view reason, size_t pairs) {
		return SpecializationFail(fmt::format(
		    "inline sampled pair at pc 0x{:08x}: {} (size={} stride={} buffer_stride={} probes={} pairs={})",
		    pc, reason, size, image.selector_stride, buffer.Stride(), probe_count, pairs));
	};
	if (probe_count > MaxIndirectImageProbes) {
		return fail("probe limit exceeded", 0u);
	}
	IndirectImage next;
	DescriptorValue default_image;
	next.buffer_size = size;
	next.probe_count = probe_count;
	next.selector_stride = image.selector_stride;
	default_image.dword_count = 8u;
	DescriptorValue null_sampler;
	null_sampler.dword_count = 4u;
	std::array<std::optional<DescriptorValue>, 256> table_images;
	const auto table_image = [&](uint32_t word, DescriptorValue& descriptor) {
		const auto& table = *image.image_table;
		const auto index = (word >> table.index_shift) & table.index_mask;
		if (!table_images[index].has_value()) {
			DescriptorValue value;
			if (!ReadInlineImageTable(table, *table_address, index, runtime, value)) {
				return false;
			}
			table_images[index] = value;
		}
		descriptor = *table_images[index];
		return true;
	};
	if (image.image_table.has_value() && !table_image(0u, default_image)) {
		return fail("default image table entry is unavailable, GPU-dirty, or outside the address range", 0u);
	}
	// An out-of-bounds material selector reads zero. Dependent tables therefore default to
	// table[0], which can be a valid image; direct inline descriptors default to a null image.
	next.descriptors.push_back(default_image);
	next.samplers.push_back(null_sampler);
	next.keys.reserve(static_cast<size_t>(probe_count));
	next.candidates.reserve(static_cast<size_t>(probe_count));
	for (uint64_t probe = 0; probe < probe_count; probe++) {
		const auto key = static_cast<uint32_t>(probe * step);
		auto candidate_image = default_image;
		auto candidate_sampler = null_sampler;
		if (image.image_table.has_value()) {
			uint32_t selector_word = 0;
			if (!ReadScalarBufferWord(buffer, key, image.descriptor_offset, runtime, selector_word) ||
			    !table_image(selector_word, candidate_image)) {
				return fail("image table descriptor memory is unavailable, GPU-dirty, or outside the address range",
				            next.descriptors.size());
			}
		}
		if (!image.image_table.has_value()) {
			for (uint32_t dword = 0; dword < image.descriptor_dwords; dword++) {
				if (!ReadScalarBufferWord(buffer, key, image.descriptor_offset + dword * 4u,
				                          runtime, candidate_image.dwords[dword])) {
					return fail("image descriptor memory is unavailable or GPU-dirty", next.descriptors.size());
				}
			}
		}
		if (sampler != nullptr) {
			for (uint32_t dword = 0; dword < 4u; dword++) {
				if (!ReadScalarBufferWord(buffer, key, sampler->descriptor_offset + dword * 4u,
				                          runtime, candidate_sampler.dwords[dword])) {
					return fail("sampler descriptor memory is unavailable or GPU-dirty", next.descriptors.size());
				}
			}
		}
		if (NullImageDescriptor(candidate_image) ||
		    !ValidImageDescriptor(candidate_image,
		                          !image.image_table.has_value() && image.descriptor_dwords == 4u)) {
			candidate_image.dwords.fill(0u);
		}
		// Invalid or null image records cannot be sampled meaningfully. The sampler bits beside
		// them are frequently unrelated material data when a wrapped selector lands between
		// records, and retaining those bits creates distinct Vulkan resources for the same null
		// result. Canonicalize the whole sampled pair so only valid images consume sampler slots.
		if (sampler != nullptr && NullImageDescriptor(candidate_image)) {
			candidate_sampler = null_sampler;
		}
		size_t candidate = 0;
		while (candidate < next.descriptors.size() &&
		       (next.descriptors[candidate] != candidate_image ||
		        next.samplers[candidate] != candidate_sampler)) {
			candidate++;
		}
		if (candidate == next.descriptors.size()) {
			if (candidate >= ShaderInfo::MaxImages) {
				return fail("candidate pair limit exceeded", candidate + 1u);
			}
			next.descriptors.push_back(candidate_image);
			next.samplers.push_back(candidate_sampler);
		}
		if (candidate != 0u) {
			next.keys.push_back(key);
			next.candidates.push_back(static_cast<uint32_t>(candidate));
		}
	}
	if (sampler == nullptr) {
		next.samplers.clear();
	}
	result = std::move(next);
	return true;
}

bool MaterializeInlineBuffer(const DescriptorSource::InlineDescriptor& table,
                             const DescriptorValue& table_value, uint32_t pc,
                             const SrtRuntime& runtime,
                             std::vector<DescriptorValue>& result) {
	ShaderBufferResource buffer;
	if (!DecodeBufferDescriptor(table_value, buffer) || table.selector_stride == 0u ||
	    table.selector_limit == 0u || table.descriptor_dwords != 4u ||
	    table.image_table.has_value() || table.descriptor_offset > UINT32_MAX - 12u) {
		return SpecializationFail(fmt::format(
		    "inline buffer table at pc 0x{:08x} has invalid metadata", pc));
	}
	if (buffer.Type() != 0u || table.selector_limit > MaxIndirectImageProbes) {
		return SpecializationFail(fmt::format(
		    "inline buffer table at pc 0x{:08x} has invalid scalar source or candidate count",
		    pc));
	}
	std::vector<DescriptorValue> candidates;
	candidates.reserve(table.selector_limit);
	for (uint32_t selector = 0; selector < table.selector_limit; ++selector) {
		const auto key = static_cast<uint32_t>(
		    static_cast<uint64_t>(selector) * table.selector_stride);
		DescriptorValue descriptor;
		descriptor.dword_count = 4u;
		for (uint32_t dword = 0; dword < descriptor.dword_count; ++dword) {
			if (!ReadScalarBufferWord(buffer, key,
			                          table.descriptor_offset + dword * sizeof(uint32_t),
			                          runtime, descriptor.dwords[dword])) {
				return SpecializationFail(fmt::format(
				    "inline buffer table at pc 0x{:08x} cannot read candidate {}", pc,
				    selector));
			}
		}
		candidates.push_back(descriptor);
	}
	result = std::move(candidates);
	return true;
}

// This cache makes every clean read of the same source DWORD observe the same snapshot,
// including count/address chains shared with ordinary flattened SRT slots.
struct SnapshotReader {
	const SrtRuntime& runtime;
	std::unordered_map<uint64_t, uint32_t> words;

	static bool Clean(void* userdata, uint64_t address, uint32_t* result) {
		auto& self = *static_cast<SnapshotReader*>(userdata);
		if (address > AddressMask - 3u || self.runtime.read_specialization_memory == nullptr) {
			return false;
		}
		if (const auto found = self.words.find(address); found != self.words.end()) {
			*result = found->second;
			return true;
		}
		if (!self.runtime.read_specialization_memory(self.runtime.userdata, address, result)) {
			return false;
		}
		self.words.emplace(address, *result);
		return true;
	}

	static bool Ordinary(void* userdata, uint64_t address, uint32_t* result) {
		auto& self = *static_cast<SnapshotReader*>(userdata);
		if (self.runtime.read_memory != nullptr) {
			return self.runtime.read_memory(self.runtime.userdata, address, result);
		}
		// Preserve the existing evaluator fallback for callers without an ordinary reader.
		std::memcpy(result, reinterpret_cast<const void*>(address), sizeof(*result));
		return true;
	}

	void Finish(ResourceSnapshot& snapshot) const {
		std::vector<uint64_t> addresses;
		addresses.reserve(words.size());
		for (const auto& [address, word]: words) {
			(void)word;
			addresses.push_back(address);
		}
		std::ranges::sort(addresses);
		for (const auto address: addresses) {
			if (!snapshot.immutable_srt_ranges.empty()) {
				auto& last = snapshot.immutable_srt_ranges.back();
				if (address <= last.address + last.size) {
					last.size = std::max(last.address + last.size, address + 4u) - last.address;
					continue;
				}
			}
			snapshot.immutable_srt_ranges.push_back({address, 4u});
		}
	}
};

bool MaterializeBoundedReads(const ResourcePlan& program, const SrtRuntime& runtime,
                             MaterializedSnapshot& snapshot) {
	if (program.bounded_srt_reads.empty()) {
		return true;
	}
	if (program.stage != ShaderType::Compute || program.info.writes_dma) {
		return SpecializationFail("bounded SRT snapshots require compute without DMA writes");
	}
	SrtRuntime clean_runtime = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	uint64_t snapshot_words = 0;
	for (uint32_t id = 0; id < program.bounded_srt_reads.size(); id++) {
		const auto& read = program.bounded_srt_reads[id];
		const auto* address_source = Source(program, read.address_source);
		if (address_source == nullptr ||
		    (address_source->dword_count != 2u && address_source->dword_count != 4u)) {
			return SpecializationFail("bounded SRT read has invalid address source width");
		}
		uint32_t size = 0;
		if (read.workgroup_axis != UINT32_MAX) {
			if (read.workgroup_axis >= 3u || read.count_source != UINT32_MAX ||
			    !runtime.compute_workgroups.has_value()) {
				return SpecializationFail(fmt::format(
				    "bounded SRT read {} requires actual guest workgroup counts and a valid axis", id));
			}
			const auto& groups = *runtime.compute_workgroups;
			// A zero in any axis creates no invocations, hence no coefficient reads.
			// These are guest counts, before host wave partitioning, not local sizes.
			if (std::ranges::all_of(groups, [](uint32_t count) { return count != 0u; })) {
				size = groups[read.workgroup_axis];
			}
		} else {
			const auto* count_source = Source(program, read.count_source);
			if (count_source == nullptr || count_source->dword_count != 1u) {
				return SpecializationFail("bounded SRT read has invalid count source width");
			}
			DescriptorValue count;
			if (!EvaluateDescriptorSource(program, read.count_source, clean_runtime, count)) {
				return SpecializationFail(fmt::format("bounded SRT read {} cannot snapshot its count", id));
			}
			const auto raw_count = count.dwords[0];
			size = read.count_signed && static_cast<int32_t>(raw_count) <= 0 ? 0u : raw_count;
		}
		if (size > MaxIndirectImageProbes ||
		    snapshot_words > MaxBoundedSnapshotWords - uint64_t{size} ||
		    snapshot.resources.flattened_srt.size() > UINT32_MAX - uint64_t{size}) {
			return SpecializationFail(fmt::format(
			    "bounded SRT read {} exceeds the candidate/snapshot limit "
			    "(count={} stride={} bias={} words={} candidate_limit={} word_limit={})",
			    id, size, read.offset_scale, read.offset_bias, snapshot_words + uint64_t{size},
			    MaxIndirectImageProbes, MaxBoundedSnapshotWords));
		}
		snapshot_words += size;
		auto& flat = snapshot.resources.flattened_srt;
		const auto start = static_cast<uint32_t>(flat.size());
		snapshot.bounded_srt_reads.push_back({size, start});
		if (size == 0u) {
			continue; // The proved guard makes the read unreachable; do not dereference its table.
		}
		DescriptorValue address_words;
		if (!EvaluateDescriptorSource(program, read.address_source, clean_runtime, address_words)) {
			return SpecializationFail(fmt::format("bounded SRT read {} cannot snapshot its address", id));
		}
		const uint64_t base = (((uint64_t{address_words.dwords[1]} << 32u) |
		                        address_words.dwords[0]) & AddressMask) & ~uint64_t{3};
		const int64_t immediate = static_cast<int64_t>(static_cast<int32_t>(read.memory_offset));
		for (uint32_t index = 0; index < size; index++) {
			const uint32_t dynamic = index * read.offset_scale + read.offset_bias;
			int64_t offset = 0;
			if (address_source->dword_count == 4u) {
				if (immediate < 0) {
					return SpecializationFail(fmt::format(
					    "bounded buffer read {} has a negative immediate offset", id));
				}
				const uint64_t byte_offset = static_cast<uint64_t>(immediate) + dynamic;
				const uint64_t aligned = byte_offset & ~uint64_t{3};
				const uint32_t stride = (address_words.dwords[1] >> 16u) & 0x3fffu;
				const uint64_t bytes = stride == 0u
				                           ? uint64_t{address_words.dwords[2]}
				                           : uint64_t{stride} * address_words.dwords[2];
				if (aligned > bytes || bytes - aligned < sizeof(uint32_t)) {
					// Scalar buffer loads return zero outside the descriptor extent. Preserve that
					// guest result in the candidate snapshot without touching host memory.
					flat.push_back(0u);
					continue;
				}
				offset = static_cast<int64_t>(aligned);
			} else {
				offset = (immediate & ~int64_t{3}) +
				         static_cast<int64_t>(dynamic & ~uint32_t{3});
			}
			if ((offset < 0 && base < static_cast<uint64_t>(-offset)) ||
			    (offset >= 0 && base > AddressMask - static_cast<uint64_t>(offset))) {
				return SpecializationFail(fmt::format("bounded SRT read {} index {} overflows its 48-bit address", id, index));
			}
			const uint64_t address = offset < 0 ? base - static_cast<uint64_t>(-offset)
			                                    : base + static_cast<uint64_t>(offset);
			uint32_t word = 0;
			if (address > AddressMask - 3u || !ReadSpecializationWord(runtime, address, word)) {
				return SpecializationFail(fmt::format(
				    "bounded SRT read {} index {} cannot read coherent source at 0x{:x}", id, index, address));
			}
			flat.push_back(word);
		}
	}
	return true;
}

bool MaterializeBoundedBufferExpressions(const ResourcePlan& program, const SrtRuntime& runtime,
	                                      MaterializedSnapshot& snapshot) {
	snapshot.bounded_buffer_expressions.resize(program.info.buffers.size());
	SrtRuntime clean_runtime = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	for (uint32_t logical = 0; logical < program.info.buffers.size(); ++logical) {
		const auto* source = Source(program, program.info.buffers[logical].source);
		if (source == nullptr || !source->bounded_buffer.has_value() ||
		    !source->bounded_buffer->expression) {
			continue;
		}
		const auto& bounded = *source->bounded_buffer;
		if (source->dword_count != 4u || bounded.key_arg != 0u ||
		    bounded.dependencies.empty()) {
			return SpecializationFail("bounded buffer expression has invalid metadata");
		}
		const auto first = bounded.dependencies.front();
		if (first >= snapshot.bounded_srt_reads.size()) {
			return SpecializationFail("bounded buffer expression has an invalid dependency");
		}
		const auto count = snapshot.bounded_srt_reads[first].count;
		for (const auto dependency: bounded.dependencies) {
			if (dependency >= snapshot.bounded_srt_reads.size() ||
			    snapshot.bounded_srt_reads[dependency].count != count) {
				return SpecializationFail(
				    "bounded buffer expression dependencies have different candidate counts");
			}
		}
		auto& candidates = snapshot.bounded_buffer_expressions[logical];
		candidates.reserve(count);
		for (uint32_t candidate = 0; candidate < count; ++candidate) {
			DescriptorValue descriptor;
			if (!EvaluateBoundedDescriptorSource(
			        program, program.info.buffers[logical].source, clean_runtime,
			        snapshot.bounded_srt_reads, snapshot.resources.flattened_srt, candidate,
			        descriptor)) {
				return SpecializationFail(fmt::format(
				    "bounded buffer expression {} candidate {} cannot be evaluated", logical,
				    candidate));
			}
			candidates.push_back(descriptor);
		}
	}
	return true;
}

bool MaterializeBoundedImageExpressions(const ResourcePlan& program, const SrtRuntime& runtime,
	                                     MaterializedSnapshot& snapshot) {
	snapshot.bounded_image_expressions.resize(program.info.images.size());
	SrtRuntime clean_runtime = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	for (uint32_t logical = 0; logical < program.info.images.size(); ++logical) {
		const auto* source = Source(program, program.info.images[logical].source);
		if (source == nullptr || !source->bounded_image.has_value() ||
		    !source->bounded_image->expression || source->bounded_image->wave_uniform) {
			continue;
		}
		const auto& bounded = *source->bounded_image;
		if (source->dword_count != 8u || bounded.key_arg != 0u ||
		    bounded.dependencies.empty()) {
			return SpecializationFail("bounded image expression has invalid metadata");
		}
		const auto first = bounded.dependencies.front();
		if (first >= snapshot.bounded_srt_reads.size()) {
			return SpecializationFail("bounded image expression has an invalid dependency");
		}
		const auto count = snapshot.bounded_srt_reads[first].count;
		for (const auto dependency: bounded.dependencies) {
			if (dependency >= snapshot.bounded_srt_reads.size() ||
			    snapshot.bounded_srt_reads[dependency].count != count) {
				return SpecializationFail(
				    "bounded image expression dependencies have different candidate counts");
			}
		}
		auto& candidates = snapshot.bounded_image_expressions[logical];
		candidates.reserve(count);
		for (uint32_t candidate = 0; candidate < count; ++candidate) {
			DescriptorValue descriptor;
			if (!EvaluateBoundedDescriptorSource(
			        program, program.info.images[logical].source, clean_runtime,
			        snapshot.bounded_srt_reads, snapshot.resources.flattened_srt, candidate,
			        descriptor)) {
				return SpecializationFail(fmt::format(
				    "bounded image expression {} candidate {} cannot be evaluated", logical,
				    candidate));
			}
			candidates.push_back(descriptor);
		}
	}
	return true;
}

bool MaterializeBoundedSamplerExpressions(const ResourcePlan& program, const SrtRuntime& runtime,
	                                       MaterializedSnapshot& snapshot) {
	snapshot.bounded_sampler_expressions.resize(program.info.samplers.size());
	snapshot.bounded_sampler_consumed.resize(program.info.samplers.size());
	SrtRuntime clean_runtime = runtime;
	clean_runtime.read_memory = runtime.read_specialization_memory;
	for (uint32_t logical = 0; logical < program.info.samplers.size(); ++logical) {
		const auto* source = Source(program, program.info.samplers[logical].source);
		if (source == nullptr || !source->bounded_sampler.has_value()) continue;
		const auto& bounded = *source->bounded_sampler;
		if (source->dword_count != 4u || bounded.key_arg != 0u ||
		    bounded.selector_group == UINT32_MAX || bounded.dependencies.empty()) {
			return SpecializationFail("bounded sampler expression has invalid metadata");
		}
		const auto first = bounded.dependencies.front();
		if (first >= snapshot.bounded_srt_reads.size()) {
			return SpecializationFail("bounded sampler expression has an invalid dependency");
		}
		const auto count = snapshot.bounded_srt_reads[first].count;
		for (const auto dependency: bounded.dependencies) {
			if (dependency >= snapshot.bounded_srt_reads.size() ||
			    snapshot.bounded_srt_reads[dependency].count != count) {
				return SpecializationFail(
				    "bounded sampler expression dependencies have different candidate counts");
			}
		}
		auto& candidates = snapshot.bounded_sampler_expressions[logical];
		candidates.reserve(count);
		for (uint32_t candidate = 0; candidate < count; ++candidate) {
			DescriptorValue descriptor;
			if (!EvaluateBoundedDescriptorSource(
			        program, program.info.samplers[logical].source, clean_runtime,
			        snapshot.bounded_srt_reads, snapshot.resources.flattened_srt, candidate,
			        descriptor)) {
				return SpecializationFail(fmt::format(
				    "bounded sampler expression {} candidate {} cannot be evaluated", logical,
				    candidate));
			}
			candidates.push_back(descriptor);
		}
		if (!candidates.empty()) snapshot.resources.samplers[logical] = candidates.front();
	}
	return true;
}

bool MaterializeBoundedImages(const ResourcePlan& program, MaterializedSnapshot& snapshot) {
	for (uint32_t logical = 0; logical < program.info.images.size(); ++logical) {
		const auto* source = Source(program, program.info.images[logical].source);
		if (source == nullptr || !source->bounded_image.has_value()) {
			continue;
		}
		const auto& bounded = *source->bounded_image;
		if (source->dword_count != 8u || bounded.key_arg >= source->dword_count ||
		    (bounded.wave_uniform
		         ? bounded.wave_candidates.empty()
		         : bounded.selector_group == UINT32_MAX || program.bounded_srt_reads.empty())) {
			return SpecializationFail("bounded image has invalid source metadata");
		}
		uint32_t count = 0;
		if (bounded.wave_uniform) {
			count = static_cast<uint32_t>(bounded.wave_candidates.size());
		} else if (bounded.expression) {
			if (logical >= snapshot.bounded_image_expressions.size()) {
				return SpecializationFail("bounded image expression candidates are missing");
			}
			count = static_cast<uint32_t>(snapshot.bounded_image_expressions[logical].size());
		} else {
			const auto first_read = bounded.reads[0];
			if (first_read >= snapshot.bounded_srt_reads.size()) {
				return SpecializationFail("bounded image has an invalid read column");
			}
			const auto& first = program.bounded_srt_reads[first_read];
			count = snapshot.bounded_srt_reads[first_read].count;
			for (uint32_t word = 0; word < bounded.reads.size(); ++word) {
				const auto read_id = bounded.reads[word];
				const auto expected_offset = uint64_t {first.memory_offset} +
				                             uint64_t {word} * sizeof(uint32_t);
				if (read_id >= snapshot.bounded_srt_reads.size() ||
				    snapshot.bounded_srt_reads[read_id].count != count ||
				    program.bounded_srt_reads[read_id].count_source != first.count_source ||
				    program.bounded_srt_reads[read_id].address_source != first.address_source ||
				    program.bounded_srt_reads[read_id].count_signed != first.count_signed ||
				    program.bounded_srt_reads[read_id].workgroup_axis != UINT32_MAX ||
				    program.bounded_srt_reads[read_id].offset_scale != first.offset_scale ||
				    program.bounded_srt_reads[read_id].offset_bias != first.offset_bias ||
				    program.bounded_srt_reads[read_id].memory_offset != expected_offset) {
					return SpecializationFail(
					    "bounded image columns do not form a correlated eight-word descriptor");
				}
			}
		}

		uint32_t bounded_sampler = UINT32_MAX;
		for (const auto& pair: program.info.sampled_pairs) {
			if (pair.image != logical || pair.sampler >= program.info.samplers.size()) continue;
			const auto* sampler_source = Source(program, program.info.samplers[pair.sampler].source);
			if (sampler_source == nullptr || !sampler_source->bounded_sampler.has_value()) continue;
			const auto& sampler = *sampler_source->bounded_sampler;
			if (bounded.wave_uniform) {
				return SpecializationFail(
				    "wave-uniform image cannot share an indexed bounded sampler");
			}
			if (sampler.selector_group != bounded.selector_group) {
				return SpecializationFail(fmt::format(
				    "bounded image {} and sampler {} use different selectors", logical, pair.sampler));
			}
			if (bounded_sampler != UINT32_MAX && bounded_sampler != pair.sampler) {
				return SpecializationFail(
				    "one bounded image is paired with multiple bounded sampler tables");
			}
			bounded_sampler = pair.sampler;
		}

		IndirectImage table;
		table.resource = logical;
		table.probe_count = count;
		if (bounded_sampler != UINT32_MAX) {
			if (bounded_sampler >= snapshot.bounded_sampler_expressions.size() ||
			    snapshot.bounded_sampler_expressions[bounded_sampler].size() != count) {
				return SpecializationFail(
				    "bounded image and sampler tables have different candidate counts");
			}
			table.sampler_resource = bounded_sampler;
			snapshot.bounded_sampler_consumed[bounded_sampler] = 1u;
		}
		table.keys.reserve(count);
		table.candidates.reserve(count);
		table.descriptors.reserve(std::min<size_t>(count, ShaderInfo::MaxImages));
		for (uint32_t index = 0; index < count; ++index) {
			DescriptorValue descriptor;
			if (bounded.wave_uniform) {
				descriptor.dword_count = 8u;
				for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
					const auto& candidate = bounded.wave_candidates[index][word];
					if (candidate.immediate) {
						descriptor.dwords[word] = candidate.value;
						continue;
					}
					if (candidate.value >= program.srt_reads.size()) {
						return SpecializationFail(
						    "wave-uniform image candidate has an invalid flat SRT slot");
					}
					const auto flat = program.srt_reads[candidate.value].flat_offset;
					if (flat >= snapshot.resources.flattened_srt.size()) {
						return SpecializationFail(
						    "wave-uniform image candidate exceeds its flat SRT snapshot");
					}
					descriptor.dwords[word] = snapshot.resources.flattened_srt[flat];
				}
			} else if (bounded.expression) {
				descriptor = snapshot.bounded_image_expressions[logical][index];
			} else {
				descriptor.dword_count = 8u;
				for (uint32_t word = 0; word < bounded.reads.size(); ++word) {
					const auto& layout = snapshot.bounded_srt_reads[bounded.reads[word]];
					const auto flat_index = uint64_t {layout.flat_offset} + index;
					if (flat_index >= snapshot.resources.flattened_srt.size()) {
						return SpecializationFail("bounded image column exceeds its captured snapshot");
					}
					descriptor.dwords[word] =
					    snapshot.resources.flattened_srt[static_cast<size_t>(flat_index)];
				}
			}
			const auto key = descriptor.dwords[bounded.key_arg];
			if (NullImageDescriptor(descriptor) ||
			    !ValidImageDescriptor(descriptor, program.info.images[logical].r128)) {
				descriptor.dwords.fill(0u);
			}
			const DescriptorValue* sampler = bounded_sampler == UINT32_MAX
			                                     ? nullptr
			                                     : &snapshot.bounded_sampler_expressions[bounded_sampler][index];
			uint32_t candidate = 0;
			while (candidate < table.descriptors.size() &&
			       (table.descriptors[candidate] != descriptor ||
			        (sampler != nullptr && table.samplers[candidate] != *sampler))) {
				++candidate;
			}
			if (candidate == table.descriptors.size()) {
				if (table.descriptors.size() >= ShaderInfo::MaxImages) {
					return SpecializationFail("bounded image table exceeds the dense image limit");
				}
				table.descriptors.push_back(descriptor);
				if (sampler != nullptr) table.samplers.push_back(*sampler);
				candidate = static_cast<uint32_t>(table.descriptors.size() - 1u);
			}
			if (bounded.wave_uniform) {
				const auto existing_key = std::ranges::find(table.keys, key);
				if (existing_key != table.keys.end()) {
					const auto mapping = static_cast<size_t>(existing_key - table.keys.begin());
					if (mapping >= table.candidates.size() ||
					    table.candidates[mapping] != candidate) {
						return SpecializationFail(
						    "wave-uniform image candidates have an ambiguous key DWORD");
					}
					continue;
				}
			}
			table.keys.push_back(bounded.wave_uniform ? key : index);
			table.candidates.push_back(candidate);
		}
		if (table.descriptors.empty()) {
			DescriptorValue null_descriptor;
			null_descriptor.dword_count = 8u;
			snapshot.resources.images[logical] = null_descriptor;
			continue;
		}
		snapshot.resources.images[logical] = table.descriptors[0];
		if (table.descriptors.size() > 1u) {
			snapshot.indirect_images.push_back(std::move(table));
		}
	}
	for (uint32_t sampler = 0; sampler < program.info.samplers.size(); ++sampler) {
		const auto* source = Source(program, program.info.samplers[sampler].source);
		if (source != nullptr && source->bounded_sampler.has_value() &&
		    (sampler >= snapshot.bounded_sampler_consumed.size() ||
		     snapshot.bounded_sampler_consumed[sampler] == 0u)) {
			return SpecializationFail(fmt::format(
			    "bounded sampler {} has no correlated bounded image", sampler));
		}
	}
	return true;
}

bool ExpandBufferTables(const ResourcePlan& program, const MaterializedSnapshot& materialized,
                        const SrtRuntime& runtime, ResourceSnapshot& snapshot,
                        ResourceSpecialization& specialization) {
	std::vector<DescriptorValue> buffers;
	specialization.bounded_srt_reads = materialized.bounded_srt_reads;
	if (!program.bounded_srt_reads.empty() ||
	    std::ranges::any_of(program.info.buffers, [&](const BufferResource& buffer) {
		    const auto* source = Source(program, buffer.source);
		    return source != nullptr && (source->bounded_buffer.has_value() ||
		                                 source->inline_descriptor.has_value());
	    })) {
		specialization.buffer_tables.resize(program.info.buffers.size());
	}
	for (uint32_t logical = 0; logical < program.info.buffers.size(); logical++) {
		const auto* source = Source(program, program.info.buffers[logical].source);
		const bool inline_table = source != nullptr && source->inline_descriptor.has_value();
		if (source == nullptr ||
		    (!source->bounded_buffer.has_value() && !inline_table)) {
			buffers.push_back(snapshot.buffers[logical]);
			specialization.buffer_origins.push_back(logical);
			continue;
		}
		uint32_t count = 0;
		uint32_t diagnostic_stride = 0;
		if (inline_table) {
			const auto& inline_descriptor = *source->inline_descriptor;
			if (source->dword_count != 4u || inline_descriptor.key_arg != 0u ||
			    logical >= materialized.inline_buffers.size()) {
				return SpecializationFail("inline buffer has invalid source metadata");
			}
			count = static_cast<uint32_t>(materialized.inline_buffers[logical].size());
			diagnostic_stride = inline_descriptor.selector_stride;
		} else if (source->bounded_buffer->wave_uniform) {
			const auto& bounded = *source->bounded_buffer;
			if (source->dword_count != 4u || bounded.key_arg != 0u ||
			    bounded.wave_candidates.empty() || bounded.expression) {
				return SpecializationFail("wave-uniform buffer has invalid source metadata");
			}
			count = static_cast<uint32_t>(bounded.wave_candidates.size());
		} else if (source->bounded_buffer->expression) {
			const auto& bounded = *source->bounded_buffer;
			if (source->dword_count != 4u || bounded.key_arg != 0u ||
			    program.bounded_srt_reads.empty()) {
				return SpecializationFail("bounded buffer has invalid source metadata");
			}
			if (logical >= materialized.bounded_buffer_expressions.size()) {
				return SpecializationFail("bounded buffer expression candidates are missing");
			}
			count = static_cast<uint32_t>(materialized.bounded_buffer_expressions[logical].size());
		} else {
			const auto& bounded = *source->bounded_buffer;
			if (source->dword_count != 4u || bounded.key_arg != 0u ||
			    program.bounded_srt_reads.empty()) {
				return SpecializationFail("bounded buffer has invalid source metadata");
			}
			const auto first_read = bounded.reads[0];
			if (first_read >= specialization.bounded_srt_reads.size()) {
				return SpecializationFail("bounded buffer has an invalid read column");
			}
			const auto& first = program.bounded_srt_reads[first_read];
			count = specialization.bounded_srt_reads[first_read].count;
			diagnostic_stride = first.offset_scale;
			for (uint32_t word = 0; word < bounded.reads.size(); word++) {
				const auto read_id = bounded.reads[word];
				if (read_id >= specialization.bounded_srt_reads.size() ||
				    specialization.bounded_srt_reads[read_id].count != count ||
				    program.bounded_srt_reads[read_id].count_source != first.count_source ||
				    program.bounded_srt_reads[read_id].address_source != first.address_source ||
				    program.bounded_srt_reads[read_id].count_signed != first.count_signed ||
				    program.bounded_srt_reads[read_id].offset_scale != first.offset_scale ||
				    program.bounded_srt_reads[read_id].offset_bias != first.offset_bias ||
				    program.bounded_srt_reads[read_id].memory_offset !=
				        uint64_t{first.memory_offset} + word * sizeof(uint32_t)) {
					return SpecializationFail(
					    "bounded buffer columns do not form a correlated four-word descriptor");
				}
			}
		}
		auto& table = specialization.buffer_tables[logical];
		table.count = count;
		if (snapshot.flattened_srt.size() > UINT32_MAX - uint64_t{count}) {
			return SpecializationFail("bounded buffer mapping exceeds the flat address limit");
		}
		table.mapping_flat_offset = static_cast<uint32_t>(snapshot.flattened_srt.size());
		for (uint32_t index = 0; index < count; index++) {
			DescriptorValue descriptor;
			if (inline_table) {
				descriptor = materialized.inline_buffers[logical][index];
			} else if (source->bounded_buffer->wave_uniform) {
				descriptor.dword_count = 4u;
				const auto& bounded = *source->bounded_buffer;
				for (uint32_t word = 0; word < descriptor.dword_count; ++word) {
					const auto& candidate = bounded.wave_candidates[index][word];
					if (candidate.immediate) {
						descriptor.dwords[word] = candidate.value;
						continue;
					}
					if (candidate.value >= program.srt_reads.size()) {
						return SpecializationFail(
						    "wave-uniform buffer candidate has an invalid flat SRT slot");
					}
					const auto flat = program.srt_reads[candidate.value].flat_offset;
					if (flat >= snapshot.flattened_srt.size()) {
						return SpecializationFail(
						    "wave-uniform buffer candidate exceeds its flat SRT snapshot");
					}
					descriptor.dwords[word] = snapshot.flattened_srt[flat];
				}
			} else if (source->bounded_buffer->expression) {
				descriptor = materialized.bounded_buffer_expressions[logical][index];
			} else {
				const auto& bounded = *source->bounded_buffer;
				descriptor.dword_count = 4u;
				for (uint32_t word = 0; word < 4u; word++) {
					const auto& layout = specialization.bounded_srt_reads[bounded.reads[word]];
					descriptor.dwords[word] = snapshot.flattened_srt[layout.flat_offset + index];
				}
			}
			if (!CanonicalizeEnumeratedBufferDescriptor(descriptor, runtime)) {
				return SpecializationFail("enumerated buffer candidate has invalid descriptor width");
			}
			auto candidate = std::ranges::find_if(table.resources, [&](uint32_t resource) {
				return buffers[resource] == descriptor;
			});
			uint32_t resource = 0;
			if (candidate == table.resources.end()) {
				if (buffers.size() >= ShaderInfo::MaxBuffers) {
					return SpecializationFail(fmt::format(
					    "bounded buffer {} exceeds the dense buffer limit (count={} stride={} candidates={} buffers={} limit={})",
					    logical, count, diagnostic_stride, table.resources.size() + 1u,
					    buffers.size() + 1u, ShaderInfo::MaxBuffers));
				}
				resource = static_cast<uint32_t>(buffers.size());
				buffers.push_back(descriptor);
				specialization.buffer_origins.push_back(logical);
				table.resources.push_back(resource);
			} else {
				resource = *candidate;
			}
			snapshot.flattened_srt.push_back(resource);
		}
	}
	if (buffers.size() > ShaderInfo::MaxBuffers) {
		return SpecializationFail("specialized buffers exceed the dense buffer limit");
	}
	snapshot.buffers = std::move(buffers);
	return true;
}

bool ValidateSnapshotBufferWrites(const ResourcePlan& program, const SrtRuntime& runtime,
                                  const ResourceSnapshot& snapshot,
                                  const ResourceSpecialization& specialization) {
	if (snapshot.immutable_srt_ranges.empty()) {
		return true;
	}
	for (uint32_t resource = 0; resource < snapshot.buffers.size(); resource++) {
		const auto& metadata = program.info.buffers[specialization.buffer_origins[resource]];
		if ((!metadata.written && !metadata.atomic) || metadata.image_alias != BufferResource::NoImageAlias) {
			continue; // Renderer also checks actual padded image allocations before any binding mutation.
		}
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(snapshot.buffers[resource], descriptor)) {
			return SpecializationFail("bounded SRT buffer writer has invalid descriptor width");
		}
		const auto address = descriptor.Base48();
		const auto size = ScalarBufferSize(descriptor);
		if (descriptor.Type() != 0u || address == 0u || size == 0u) {
			continue;
		}
		if (address >= RegisteredBufferAddressLimit) {
			return SpecializationFail(fmt::format(
			    "bounded SRT buffer writer {} base 0x{:x} exceeds the registered 40-bit address range "
			    "(origin={} first_use_pc=0x{:08x} size={} descriptor={:08x}:{:08x}:{:08x}:{:08x})",
			    resource, address, specialization.buffer_origins[resource], metadata.first_use_pc, size,
			    descriptor.fields[0], descriptor.fields[1], descriptor.fields[2], descriptor.fields[3]));
		}
		// Buffer descriptors may conservatively declare more records than the mapped VMA.
		// Match NativeStorageBuffer's exact contiguous mapped prefix when the renderer supplied
		// its address-space query. Offline callers retain the conservative 40-bit fallback.
		const auto requested_writable_size =
		    metadata.LimitDescriptorSize(descriptor.Stride(), size);
		uint64_t writable_size =
		    std::min(requested_writable_size, RegisteredBufferAddressLimit - address);
		if (runtime.clamp_memory_range != nullptr) {
			const auto mapped = runtime.clamp_memory_range(runtime.userdata, address,
			                                                    requested_writable_size);
			if (mapped > requested_writable_size) {
				return SpecializationFail("buffer range clamp exceeded the requested descriptor size");
			}
			writable_size = mapped;
		}
		const auto write_end = address + writable_size;
		for (const auto& read: snapshot.immutable_srt_ranges) {
			if (address < read.address + read.size && read.address < write_end) {
				return SpecializationFail(fmt::format(
				    "immutable SRT snapshot overlaps writable buffer {} "
				    "(source=0x{:x}+{} writer=0x{:x}+{} mapped={} origin={} first_use_pc=0x{:08x} "
				    "max_byte_extent={} stride={} records={} formatted={} descriptor_formatted_only={} "
				    "scalar={} atomic={} descriptor={:08x}:{:08x}:{:08x}:{:08x})",
				    resource, read.address, read.size, address, size, writable_size,
				    specialization.buffer_origins[resource], metadata.first_use_pc,
				    metadata.max_byte_extent, descriptor.Stride(), descriptor.NumRecords(),
				    metadata.formatted, metadata.descriptor_formatted_only, metadata.scalar,
				    metadata.atomic, descriptor.fields[0], descriptor.fields[1],
				    descriptor.fields[2], descriptor.fields[3]));
			}
		}
	}
	return true;
}

} // namespace

struct SamplerPlan {
	std::array<uint32_t, ShaderInfo::MaxSamplers> point_sampler {};
	uint32_t                                      sampler_count = 0;
};

struct ImageRemap {
	explicit ImageRemap(const ResourceSpecialization& specialization)
	    : source_count(static_cast<uint32_t>(specialization.images.size())) {
		EXIT_IF(specialization.images.size() > indices.size());
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
		for (uint32_t index = 0; index < source_count; index++) {
			if (indices[index] != UINT32_MAX && indices[index] != index) {
				images[indices[index]] = std::move(images[index]);
			}
		}
		images.resize(count);
	}

private:
	std::array<uint32_t, ShaderInfo::MaxImages> indices;
	uint32_t                                    source_count;
	uint32_t                                    count = 0;
};

template <typename Images>
bool BuildSamplerPlan(const ShaderInfo& base, const Images& images, SamplerPlan& plan);

static bool BuildResourceSpecialization(const ResourcePlan& program, ResourceSnapshot& snapshot,
                                        ResourceSpecialization& specialization) {
	specialization.buffers.clear();
	specialization.buffers.reserve(program.info.buffers.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
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
		specialization.buffers.push_back({
		    .packed_stride     = packed_stride,
		    .descriptor_format = base_buffer.formatted
		                             ? descriptor.Format()
		                             : Prospero::BufferFormat::kInvalid,
		    .descriptor_swizzle =
		        base_buffer.formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7),
		});
	}
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
		image.mip_count = StorageMipCount(base, descriptor);
		if (image.mip_count == 0u) {
			return SpecializationFail(
			    fmt::format("storage image descriptor {} has an invalid mip range", i));
		}
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
		if (base.atomic && format != Prospero::BufferFormat::k32UInt &&
		    format != Prospero::BufferFormat::k32Float) {
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
		// Float image atomics use a CAS loop on raw R32Uint texels. Keep the
		// specialized SPIR-V image type consistent with the host atomic view.
		image.numeric_class = base.atomic ? Prospero::TextureNumericClass::Uint
		                                  : Prospero::SampledTextureNumericClass(format);

		// Check if depth-compare is requested but format doesn't support it on Vulkan
		if (base.depth_compare && !storage) {
			const auto surface_format = TextureGetSurfaceFormatInfo(format);
			if (!IsDepthComparisonSupported(surface_format.vk_format)) {
				// Format doesn't support native depth-compare, enable manual emulation
				image.needs_manual_depth_compare = true;
				image.shader_swizzle             = DescriptorImageSwizzle(descriptor);
			}
		}
		if (storage) {
			if (image.numeric_class == Prospero::TextureNumericClass::Unsupported) {
				return SpecializationFail(
				    fmt::format("storage image descriptor {} uses unsupported format {}", i,
				                static_cast<uint32_t>(format)));
			}
		} else if (image.numeric_class == Prospero::TextureNumericClass::Unsupported ||
		           (base.depth_compare && !image.needs_manual_depth_compare &&
		            image.numeric_class != Prospero::TextureNumericClass::Float)) {
			return SpecializationFail(
			    fmt::format("sampled image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
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
		if (root.indirect_search_iterations == 0u || key_count == 0u ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(key_count) * 2u >
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
		const auto* root_source = Source(program, program.info.images[root_index].source);
		if (exemplar == ImageResource::NoIndirectImage && root_source != nullptr &&
		    root_source->inline_descriptor.has_value()) {
			// Distinct samplers can accompany only null images. Keep the normal null image
			// class instead of requiring an arbitrary non-null descriptor for this table.
			exemplar = root_index;
		}
		if (resource_count < 2u || exemplar == ImageResource::NoIndirectImage) {
			return SpecializationFail("indirect image specialization has no typed candidate");
		}
		const auto& image_class = specialization.images[exemplar];
		const auto is_2d = [](Decoder::ImageDimension dimension) {
			return dimension == Decoder::ImageDimension::Dim2D ||
			       dimension == Decoder::ImageDimension::Dim2DArray;
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
			if (image.numeric_class != image_class.numeric_class ||
			    (!same_coordinates && !(is_2d(image.dimension) && is_2d(image_class.dimension))) ||
			    image.mip_count != image_class.mip_count ||
			    image.conversion_format != image_class.conversion_format ||
			    image.shader_swizzle != image_class.shader_swizzle) {
				return SpecializationFail(
				    fmt::format(
				        "indirect image table at pc 0x{:08x} has incompatible candidates: "
				        "exemplar={} candidate={} numeric={}/{} dimension={}/{} mip_count={}/{} "
				        "conversion={}/{} swizzle=0x{:08x}/0x{:08x} cube={}/{} manual_compare={}/{} "
				        "descriptor=[{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x},{:08x}]",
				        program.info.images[root_index].first_use_pc, exemplar, candidate,
				        static_cast<uint32_t>(image_class.numeric_class),
				        static_cast<uint32_t>(image.numeric_class),
				        static_cast<uint32_t>(image_class.dimension),
				        static_cast<uint32_t>(image.dimension), image_class.mip_count, image.mip_count,
				        static_cast<uint32_t>(image_class.conversion_format),
				        static_cast<uint32_t>(image.conversion_format), image_class.shader_swizzle,
				        image.shader_swizzle, image_class.cube, image.cube,
				        image_class.needs_manual_depth_compare, image.needs_manual_depth_compare,
				        next_snapshot.images[candidate].dwords[0],
				        next_snapshot.images[candidate].dwords[1],
				        next_snapshot.images[candidate].dwords[2],
				        next_snapshot.images[candidate].dwords[3],
				        next_snapshot.images[candidate].dwords[4],
				        next_snapshot.images[candidate].dwords[5],
				        next_snapshot.images[candidate].dwords[6],
				        next_snapshot.images[candidate].dwords[7]));
			}
		}
	}
	// Manual comparison is part of the shader permutation. Extract the final dense sampler
	// table after indirect candidates have been appended so every cloned sampler keeps its
	// own comparison function.
	next_specialization.sampler_depth_compare_funcs.clear();
	next_specialization.sampler_depth_compare_funcs.reserve(next_snapshot.samplers.size());
	for (const auto& sampler_descriptor: next_snapshot.samplers) {
		next_specialization.sampler_depth_compare_funcs.push_back(
		    sampler_descriptor.dword_count >= 1
		        ? static_cast<uint8_t>((sampler_descriptor.dwords[0] >> 12u) & 0x7u)
		        : 0u);
	}
	ShaderInfo sampler_info = program.info;
	sampler_info.samplers.clear();
	for (const auto origin: next_specialization.sampler_origins) {
		if (origin >= program.info.samplers.size()) {
			return SpecializationFail("specialized sampler has an invalid origin");
		}
		sampler_info.samplers.push_back(program.info.samplers[origin]);
	}
	sampler_info.sampled_pairs.clear();
	for (const auto& pair: program.info.sampled_pairs) {
		if (pair.image >= next_specialization.images.size()) {
			return SpecializationFail("sampled pair has an invalid image resource");
		}
		const bool indirect = next_specialization.images[pair.image].indirect_root == pair.image;
		for (uint32_t index = 0; index < next_specialization.images.size(); index++) {
			const auto& image = next_specialization.images[index];
			if (indirect ? image.indirect_root != pair.image : index != pair.image) {
				continue;
			}
			const auto sampler = image.indirect_sampler != UINT32_MAX
			                         ? image.indirect_sampler : pair.sampler;
			if (sampler >= sampler_info.samplers.size()) {
				return SpecializationFail("sampled pair has an invalid sampler resource");
			}
			if (std::ranges::any_of(sampler_info.sampled_pairs, [&](const SampledResourcePair& p) {
				    return p.image == index && p.sampler == sampler;
				})) {
				continue;
			}
			if (sampler_info.sampled_pairs.size() >= ShaderInfo::MaxSampledPairs) {
				return SpecializationFail("specialized sampled pairs exceed the resource limit");
			}
			sampler_info.sampled_pairs.push_back({index, sampler, pair.first_use_pc});
		}
	}
	next_specialization.sampled_pairs = sampler_info.sampled_pairs;
	SamplerPlan sampler_plan;
	if (!BuildSamplerPlan(program.info, specialization.images, sampler_plan)) {
		return SpecializationFail("specialized sampler layout exceeds its resource limit");
	}
	for (uint32_t index = 0; index < sampler_info.samplers.size(); index++) {
		const auto target = sampler_plan.point_sampler[index];
		if (target != UINT32_MAX && target >= program.info.samplers.size()) {
			snapshot.samplers.push_back(snapshot.samplers[index]);
		}
	}
	ImageRemap(specialization).Apply(snapshot.images);
	return true;
}

template <typename Images>
bool BuildSamplerPlan(const ShaderInfo& base, const Images& images, SamplerPlan& plan) {
	if (base.samplers.size() > plan.point_sampler.size()) {
		return false;
	}
	std::array<uint8_t, ShaderInfo::MaxSamplers> usage {};
	plan.point_sampler.fill(UINT32_MAX);
	plan.sampler_count = static_cast<uint32_t>(base.samplers.size());
	for (const auto& pair: base.sampled_pairs) {
		if (pair.image >= images.size() || pair.sampler >= base.samplers.size()) {
			return false;
		}
		usage[pair.sampler] |= RequiresPointSampler(images[pair.image]) ? 2u : 1u;
	}
	for (uint32_t index = 0; index < base.samplers.size(); index++) {
		if ((usage[index] & 2u) == 0u) {
			continue;
		}
		if ((usage[index] & 1u) == 0u) {
			plan.point_sampler[index] = index;
		} else {
			if (plan.sampler_count >= ShaderInfo::MaxSamplers) {
				return false;
			}
			plan.point_sampler[index] = plan.sampler_count++;
		}
	}
	return true;
}

static std::vector<ResourceBlock> ResourceControlFlow(const Program& program) {
	// Any shader write may alias a scalar predicate read, including on a later loop visit.
	if (program.blocks.size() != program.block_info.size() || HasShaderMemoryWrites(program)) {
		return {};
	}
	std::unordered_map<uint32_t, uint32_t> indices;
	for (uint32_t i = 0; i < program.block_info.size(); i++) {
		if (!indices.emplace(program.block_info[i].id, i).second) {
			return {};
		}
	}
	std::vector<ResourceBlock> blocks(program.blocks.size());
	for (uint32_t i = 0; i < blocks.size(); i++) {
		auto&                 block      = blocks[i];
		const auto&           info       = program.block_info[i];
		const auto&           terminator = info.terminator;
		std::vector<uint32_t> successors;
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch: successors.push_back(terminator.true_block); break;
			case CFG::TerminatorKind::ConditionalBranch:
				successors = {terminator.true_block, terminator.false_block};
				if (ValidateRuntimeValue(program, info.condition, RuntimeValueType::Integer)) {
					block.condition = info.condition;
				}
				break;
			case CFG::TerminatorKind::IndirectBranch:
				successors = terminator.indirect_targets;
				break;
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
			const auto buffer = BufferAccessOf(op);
			const auto image  = ImageOpcodeInfoOf(op);
			if (buffer == BufferAccess::None && image.access == ImageAccess::None) {
				continue;
			}
			const auto& memory = program.memory_info.at(inst.Flags<MemoryFlags>().index);
			if (memory.planning_only) {
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
	if (std::ranges::none_of(
	        blocks, [](const ResourceBlock& block) { return !block.condition.IsEmpty(); })) {
		return {};
	}
	return blocks;
}

// Nonnegative affine coefficients for constant, local and workgroup coordinates. Reject modular
// arithmetic that could wrap; runtime coverage also bounds the largest invocation index.
static std::optional<std::array<uint64_t, 3>> FillIndex(Value value, uint32_t axis,
                                                      uint32_t depth = 0) {
	value = value.Resolve();
	if (depth > 32 || value.GetType() != Type::U32) {
		return {};
	}
	if (value.IsImmediate()) {
		return std::array<uint64_t, 3> {value.U32(), 0, 0};
	}
	const auto* inst = value.TryInstruction();
	if (inst == nullptr) {
		return {};
	}
	const auto op = inst->GetOpcode();
	if (op == ValueOpcode::GetBuiltin && inst->Arg(1) == Value(axis)) {
		if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::LocalInvocationId))) {
			return std::array<uint64_t, 3> {0, 1, 0};
		}
		if (inst->Arg(0) == Value(static_cast<uint32_t>(StageInputKind::WorkgroupId))) {
			return std::array<uint64_t, 3> {0, 0, 1};
		}
	}
	if (op != ValueOpcode::IAdd32 && op != ValueOpcode::IMul32 &&
	    op != ValueOpcode::ShiftLeftLogical32) {
		return {};
	}
	auto left  = FillIndex(inst->Arg(0), axis, depth + 1);
	auto right = FillIndex(inst->Arg(1), axis, depth + 1);
	if (!left || !right) {
		return {};
	}
	if (op == ValueOpcode::IMul32 && ((*right)[1] != 0 || (*right)[2] != 0)) {
		std::swap(left, right);
	}
	if (op != ValueOpcode::IAdd32 && ((*right)[1] != 0 || (*right)[2] != 0)) {
		return {};
	}
	if (op == ValueOpcode::ShiftLeftLogical32) {
		if ((*right)[0] >= 32) return {};
		(*right)[0] = uint64_t {1} << (*right)[0];
	}
	for (uint32_t i = 0; i < left->size(); ++i) {
		(*left)[i] =
		    op == ValueOpcode::IAdd32 ? (*left)[i] + (*right)[i] : (*left)[i] * (*right)[0];
		if ((*left)[i] > UINT32_MAX) return {};
	}
	return left;
}

static UniformFillPlan AnalyzeUniformFill(const Program& program) {
	if (program.stage != ShaderType::Compute || program.blocks.empty() ||
	    program.blocks.size() != program.block_info.size() || program.info.uses_dma ||
	    !program.info.samplers.empty()) {
		return {};
	}
	std::unordered_set<uint32_t> visited;
	uint32_t                     index = 0;
	const Inst*                  store = nullptr;
	for (;;) {
		if (!visited.insert(index).second) return {};
		for (const auto& inst: *program.blocks[index]) {
			if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::None) return {};
			if (!inst.MayHaveSideEffects()) continue;
			if (store != nullptr || (BufferAccessOf(inst.GetOpcode()) != BufferAccess::Write &&
			                         inst.GetOpcode() != ValueOpcode::ImageWrite))
				return {};
			store = &inst;
		}
		const auto& term = program.block_info[index].terminator;
		if (term.kind == CFG::TerminatorKind::Return) break;
		if (term.kind != CFG::TerminatorKind::Branch) return {};
		const auto next = std::ranges::find(program.block_info, term.true_block, &BlockInfo::id);
		if (next == program.block_info.end()) return {};
		index = static_cast<uint32_t>(next - program.block_info.begin());
	}
	if (store == nullptr || visited.size() != program.blocks.size()) return {};
	for (const auto& buffer: program.info.buffers) {
		if (buffer.read && (!buffer.scalar || buffer.written)) return {};
	}
	const auto& memory = program.memory_info.at(store->Flags<MemoryFlags>().index);
	UniformFillPlan result;
	result.fill.resource = memory.resource;
	Value data;
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
			const auto index = FillIndex(address->Arg(axis), axis);
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
		    store->Arg(3).Resolve() != Value(0u) || store->Arg(5).Resolve() != Value(true))
			return {};
		if (!memory.formatted || memory.typed || !memory.idxen || memory.offen || memory.offset != 0 ||
		    memory.data_bits != 32 ||
		    memory.data_dwords != static_cast<uint32_t>(store_op - stores.begin() + 1))
			return {};
		const auto address = FillIndex(store->Arg(1), 0);
		if (!address || (*address)[0] != 0 || (*address)[1] != 1 || (*address)[2] == 0) return {};
		result.fill.kind = UniformFillKind::Buffer;
		result.fill.group_stride[0] = static_cast<uint32_t>((*address)[2]);
		result.fill.words = memory.data_dwords;
		data = store->Arg(4);
	}
	data = data.Resolve();
	const auto*          vector = data.TryInstruction();
	constexpr std::array composites {ValueOpcode::CompositeConstructU32x2,
	                                 ValueOpcode::CompositeConstructU32x3,
	                                 ValueOpcode::CompositeConstructU32x4};
	if (result.fill.words > 1 &&
	    (vector == nullptr || vector->GetOpcode() != composites[result.fill.words - 2]))
		return {};
	for (uint32_t i = 0; i < result.fill.words; ++i) {
		const auto word = result.fill.words == 1 ? data : vector->Arg(i);
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
	plan.bounded_srt_reads           = program.bounded_srt_reads;
	plan.srt_plan_complete          = program.srt_plan_complete;
	plan.resource_tracking_complete = program.resource_tracking_complete;
	plan.has_address_writes         = program.has_address_writes;

	std::unordered_map<const Inst*, Inst*> cloned;
	std::function<Value(Value)>            Clone = [&](Value value) -> Value {
		value              = value.Resolve();
		const auto* source = value.TryInstruction();
		if (source == nullptr) {
			return value;
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
		auto& target =
		    plan.value_storage.emplace_back(source->GetOpcode(), source->Flags<uint64_t>());
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

	plan.descriptor_sources.reserve(program.descriptor_sources.size());
	for (const auto& source: program.descriptor_sources) {
		auto& target          = plan.descriptor_sources.emplace_back();
		target.dword_count    = source.dword_count;
		target.indirect_image = source.indirect_image;
		if (target.indirect_image.has_value()) {
			target.indirect_image->key_count = Clone(target.indirect_image->key_count);
			target.indirect_image->selector_mask = Clone(target.indirect_image->selector_mask);
		}
		for (uint32_t dword = 0; dword < source.dword_count; dword++) {
			target.dwords[dword] = Clone(source.dwords[dword]);
		}
	}
	plan.srt_reads.reserve(program.srt_reads.size());
	for (const auto& read: program.srt_reads) {
		plan.srt_reads.push_back({Clone(read.value), read.flat_offset});
	}
	plan.control_flow = ResourceControlFlow(program);
	for (auto& block: plan.control_flow) {
		block.condition = Clone(block.condition);
	}
	plan.uniform_fill = AnalyzeUniformFill(program);
	for (uint32_t i = 0; i < plan.uniform_fill.fill.words; ++i) {
		plan.uniform_fill.values[i] = Clone(plan.uniform_fill.values[i]);
	}
	plan.clean_flat_slots.resize(plan.srt_reads.size());
	for (const auto& read: plan.bounded_srt_reads) {
		plan.requires_specialization_memory = true;
		if (read.workgroup_axis == UINT32_MAX) {
			MarkCleanFlatSlots(plan, Source(plan, read.count_source), plan.clean_flat_slots);
		}
		MarkCleanFlatSlots(plan, Source(plan, read.address_source), plan.clean_flat_slots);
	}
	for (const auto& image: plan.info.images) {
		const auto* source = Source(plan, image.source);
		if (source == nullptr || !source->indirect_image.has_value()) {
			continue;
		}
		plan.requires_specialization_memory = true;
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->material_source),
		                   plan.clean_flat_slots, source->indirect_image->selector_mask);
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->table_source),
		                   plan.clean_flat_slots);
	}
	for (const auto& source: plan.descriptor_sources) {
		if (source.inline_descriptor.has_value()) {
			MarkCleanFlatSlots(plan, Source(plan, source.inline_descriptor->buffer_source),
			                   plan.clean_flat_slots);
			if (source.inline_descriptor->image_table.has_value()) {
				MarkCleanFlatSlots(plan, Source(plan, source.inline_descriptor->image_table->address_source),
				                   plan.clean_flat_slots);
			}
		}
	}
	const auto mark_wave_candidate = [&](const auto& candidate) {
		if (candidate.immediate || candidate.value >= plan.srt_reads.size()) return;
		plan.clean_flat_slots[candidate.value] = ResourcePlan::FlatSlotClean;
		DescriptorSource dependency;
		dependency.dword_count = 1u;
		dependency.dwords[0] = plan.srt_reads[candidate.value].value;
		MarkCleanFlatSlots(plan, &dependency, plan.clean_flat_slots);
	};
	for (const auto& source: plan.descriptor_sources) {
		if (source.bounded_buffer.has_value() && source.bounded_buffer->wave_uniform) {
			for (const auto& descriptor: source.bounded_buffer->wave_candidates)
				for (const auto& candidate: descriptor) mark_wave_candidate(candidate);
		}
		if (source.bounded_image.has_value() && source.bounded_image->wave_uniform) {
			for (const auto& descriptor: source.bounded_image->wave_candidates)
				for (const auto& candidate: descriptor) mark_wave_candidate(candidate);
		}
	}
	for (const auto& source: plan.descriptor_sources) {
		if ((source.bounded_buffer.has_value() && source.bounded_buffer->expression) ||
		    (source.bounded_image.has_value() && source.bounded_image->expression) ||
		    source.bounded_sampler.has_value()) {
			MarkDeferredFlatSlots(plan, &source, plan.clean_flat_slots);
		}
	}
	return plan;
}

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	if (!program.resource_tracking_complete ||
	    (program.requires_specialization_memory && runtime.read_specialization_memory == nullptr)) {
		return false;
	}
	const bool masked_image = std::ranges::any_of(program.info.images, [&](const auto& image) {
		const auto* source = Source(program, image.source);
		return source != nullptr && source->indirect_image.has_value() &&
		       !source->indirect_image->selector_mask.IsEmpty();
	});
	if (masked_image &&
	    (program.has_address_writes ||
	     std::ranges::any_of(program.info.images, &ImageResource::written))) {
		return false;
	}
	const bool capture_reads = masked_image &&
	    std::ranges::any_of(program.info.buffers, &BufferResource::written);
	auto& reads = program.specialization_reads;
	ReadCapture capture {runtime, reads};
	SrtRuntime observed = runtime;
	if (capture_reads) {
		reads.clear();
		observed.userdata = &capture;
		observed.read_specialization_memory = CaptureStrictRead;
		if (observed.read_memory != nullptr) observed.read_memory = CaptureOrdinaryRead;
	}
	SrtWalker clean(program, CleanRuntime(observed));
	SrtWalker walker(program, observed, program.clean_flat_slots, &clean);
	const auto active = clean.FindActiveSources();
	if (!walker.RefreshFlatBuffer(snapshot.flattened_srt)) {
		return false;
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
	const auto evaluate = [&](uint32_t source, DescriptorValue& value) {
		if (source >= program.descriptor_sources.size()) {
			return false;
		}
		if (active.empty() || active[source]) {
			return walker.EvaluateDescriptor(source, value);
		}
		value = {};
		value.dword_count = program.descriptor_sources[source].dword_count;
		return true;
	};
	snapshot.buffers.resize(program.info.buffers.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); ++i) {
		if (!evaluate(program.info.buffers[i].source, snapshot.buffers[i])) {
			return false;
		}
	}
	if (capture_reads) {
		for (uint32_t i = 0; i < program.info.buffers.size(); ++i) {
			const auto& buffer = program.info.buffers[i];
			if (!buffer.written || (!active.empty() && !active[buffer.source])) continue;
			DescriptorValue strict;
			if (!clean.EvaluateDescriptor(buffer.source, strict) ||
			    strict != snapshot.buffers[i]) return false;
		}
	}
	snapshot.images.resize(program.info.images.size());
	specialization.images.clear();
	specialization.images.reserve(program.info.images.size());
	for (const auto& image: program.info.images) {
		specialization.images.push_back({
		    .numeric_class = image.numeric_class,
		    .dimension = image.dimension,
		    .mip_count = image.mip_count,
		    .conversion_format = image.conversion_format,
		    .shader_swizzle = image.shader_swizzle,
		    .indirect_root = image.indirect_root,
		    .indirect_mapping_offset = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .cube = image.cube,
		});
	}
	for (uint32_t i = 0; i < program.info.images.size(); ++i) {
		const auto& image = program.info.images[i];
		const auto* source = Source(program, image.source);
		if (source == nullptr) {
			return false;
		}
		if (source->indirect_image.has_value()) {
			snapshot.images[i] = {.dword_count = 8u};
			if (!active.empty() && !active[image.source]) {
				continue;
			}
			const auto& indirect = *source->indirect_image;
			DescriptorValue material;
			DescriptorValue table;
			if ((indirect.material_source != UINT32_MAX &&
			     !clean.EvaluateDescriptor(indirect.material_source, material)) ||
			    !clean.EvaluateDescriptor(indirect.table_source, table) ||
			    !MaterializeIndirectImage(program, indirect, material, table, i, observed, clean, snapshot,
			                              specialization)) {
				return false;
			}
		} else {
			if (!evaluate(image.source, snapshot.images[i])) {
				return false;
			}
			if (!ValidImageDescriptor(snapshot.images[i], image.r128)) {
				snapshot.images[i].dwords.fill(0);
			}
		}
	}
	snapshot.samplers.resize(program.info.samplers.size());
	for (uint32_t i = 0; i < program.info.samplers.size(); ++i) {
		if (!evaluate(program.info.samplers[i].source, snapshot.samplers[i])) {
			return false;
		}
	}
	if (capture_reads && !WrittenBuffersDisjoint(program, snapshot, reads)) return false;
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return BuildResourceSpecialization(program, snapshot, specialization);
}

std::string_view LastResourceSpecializationError() noexcept {
	return g_last_specialization_error;
}

void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization) {
	EXIT_IF(!program.resource_tracking_complete || program.shader_info_complete ||
	        program.binding_layout_complete);
	EXIT_IF(specialization.buffer_origins.size() != specialization.buffers.size() ||
	        program.info.images.size() > specialization.images.size() ||
	        program.info.samplers.size() > specialization.sampler_origins.size());

	std::vector<BufferResource> buffers;
	std::vector<uint32_t> buffer_remap(program.info.buffers.size(), UINT32_MAX);
	for (size_t index = 0; index < specialization.buffers.size(); index++) {
		const auto origin = specialization.buffer_origins[index];
		EXIT_IF(origin >= program.info.buffers.size());
		buffers.push_back(program.info.buffers[origin]);
		const auto* source = Source(program, buffers.back().source);
		if (source == nullptr || (!source->bounded_buffer.has_value() &&
		                          !source->inline_descriptor.has_value())) {
			EXIT_IF(buffer_remap[origin] != UINT32_MAX);
			buffer_remap[origin] = static_cast<uint32_t>(index);
		}
		buffers[index].packed_stride      = specialization.buffers[index].packed_stride;
		buffers[index].descriptor_format  = specialization.buffers[index].descriptor_format;
		buffers[index].descriptor_swizzle = specialization.buffers[index].descriptor_swizzle;
	}
	auto images = program.info.images;
	images.reserve(specialization.images.size());
	for (uint32_t index = 0; index < specialization.images.size(); index++) {
		const auto& source = specialization.images[index];
		if (index >= images.size()) {
			EXIT_IF(source.indirect_root >= program.info.images.size());
			images.push_back(program.info.images[source.indirect_root]);
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
		image.indirect_sampler           = source.indirect_sampler;
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

	ShaderInfo sampler_info = program.info;
	sampler_info.samplers.clear();
	for (const auto origin: specialization.sampler_origins) {
		EXIT_IF(origin >= program.info.samplers.size());
		sampler_info.samplers.push_back(program.info.samplers[origin]);
	}
	sampler_info.sampled_pairs = specialization.sampled_pairs;
	EXIT_IF(specialization.sampler_depth_compare_funcs.size() != sampler_info.samplers.size());
	for (uint32_t index = 0; index < sampler_info.samplers.size(); index++) {
		sampler_info.samplers[index].depth_compare_func =
		    specialization.sampler_depth_compare_funcs[index];
	}
	SamplerPlan sampler_plan;
	EXIT_IF(!BuildSamplerPlan(sampler_info, images, sampler_plan));
	auto samplers      = sampler_info.samplers;
	auto sampled_pairs = sampler_info.sampled_pairs;
	samplers.reserve(sampler_plan.sampler_count);
	for (uint32_t index = 0; index < sampler_info.samplers.size(); index++) {
		const auto target = sampler_plan.point_sampler[index];
		if (target == UINT32_MAX) {
			continue;
		}
		if (target == index) {
			samplers[index].force_point_filtering = true;
		} else {
			EXIT_IF(target != samplers.size());
			auto sampler                  = samplers[index];
			sampler.force_point_filtering = true;
			samplers.push_back(std::move(sampler));
		}
	}
	for (auto& pair: sampled_pairs) {
		if (RequiresPointSampler(images[pair.image])) {
			EXIT_IF(sampler_plan.point_sampler[pair.sampler] == UINT32_MAX);
			pair.sampler = sampler_plan.point_sampler[pair.sampler];
		}
		samplers[pair.sampler].depth_compare |= images[pair.image].depth_compare;
	}
	for (auto& image: images) {
		if (image.indirect_sampler != UINT32_MAX) {
			EXIT_IF(image.indirect_sampler >= sampler_info.samplers.size());
			if (RequiresPointSampler(image)) {
				EXIT_IF(sampler_plan.point_sampler[image.indirect_sampler] == UINT32_MAX);
				image.indirect_sampler = sampler_plan.point_sampler[image.indirect_sampler];
			}
		}
	}

	auto memory_info = program.memory_info;
	const ImageRemap image_remap(specialization);
	for (auto* block: program.blocks) {
		for (auto it = block->begin(); it != block->end(); ++it) {
			auto& inst = *it;
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
			if (image_opcode.needs_sampler && RequiresPointSampler(image) &&
			    memory.sampler < program.info.samplers.size()) {
				EXIT_IF(sampler_plan.point_sampler[memory.sampler] == UINT32_MAX);
				memory.sampler = sampler_plan.point_sampler[memory.sampler];
			}
			EXIT_IF(image.indirect_root == memory.resource &&
			        inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageRead &&
			        inst.GetOpcode() != ValueOpcode::ImageWrite);
		}
	}
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::GetImageResource) {
				inst.SetFlags(image_remap[inst.Flags<uint32_t>()]);
			}
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
	program.info.buffers       = std::move(buffers);
	program.info.images        = std::move(images);
	program.info.samplers      = std::move(samplers);
	program.info.sampled_pairs = std::move(sampled_pairs);
	program.memory_info        = std::move(memory_info);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
