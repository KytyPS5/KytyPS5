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
#include <unordered_set>
#include <unordered_map>

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;
constexpr uint64_t MaxIndirectImageProbes = 65536u;
// A 16-bit selector can feed multiple descriptors and their address expressions.
// Bound each selector domain independently, then apply an explicit memory budget
// to the whole coherent transaction instead of treating the first 65,536 words
// as a semantic limit. This includes 256 full-width selector columns.
constexpr uint64_t MaxBoundedSnapshotBytes = 64u * 1024u * 1024u;
constexpr uint64_t MaxBoundedSnapshotWords = MaxBoundedSnapshotBytes / sizeof(uint32_t);

struct IndirectImage {
	uint32_t                     resource = 0;
	uint32_t                     sampler_resource = UINT32_MAX;
	uint64_t                     buffer_size = 0;
	uint64_t                     probe_count = 0;
	uint32_t                     selector_stride = 0;
	std::vector<uint32_t>        keys;
	std::vector<uint32_t>        candidates;
	std::vector<DescriptorValue> descriptors;
	std::vector<DescriptorValue> samplers;
};

struct MaterializedSnapshot {
	ResourceSnapshot           resources;
	std::vector<IndirectImage> indirect_images;
	std::vector<BoundedSrtLayout> bounded_srt_reads;
	std::vector<std::vector<DescriptorValue>> bounded_buffer_expressions;
	std::vector<std::vector<DescriptorValue>> bounded_image_expressions;
	std::vector<std::vector<DescriptorValue>> bounded_sampler_expressions;
	std::vector<uint8_t> bounded_sampler_consumed;
};

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

bool ValidImageDescriptor(const DescriptorValue& descriptor, bool r128 = false) {
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
	return image.numeric_class == Prospero::TextureNumericClass::Sint ||
	       image.conversion_format != Prospero::BufferFormat::kInvalid;
}

bool RequiresPointSampler(const ResourceSpecialization::Image& image) {
	return image.numeric_class == Prospero::TextureNumericClass::Sint ||
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

const DescriptorSource* Source(const ResourcePlan& program, uint32_t source) {
	if (source >= program.descriptor_sources.size()) {
		return nullptr;
	}
	return &program.descriptor_sources[source];
}

void MarkCleanFlatSlots(const ResourcePlan& program, const DescriptorSource* source,
                        std::span<uint8_t> slots) {
	if (source == nullptr) {
		return;
	}
	std::vector<Value>       pending(source->dwords.begin(),
	                                 source->dwords.begin() + source->dword_count);
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

void MarkDeferredFlatSlots(const ResourcePlan& program, const DescriptorSource* source,
	                       std::span<uint8_t> slots) {
	if (source == nullptr) return;
	std::vector<const Inst*> visiting;
	std::function<bool(Value)> depends_on_bounded = [&](Value value) {
		value = value.Resolve();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr) return false;
		if (inst->GetOpcode() == ValueOpcode::ReadBoundedSrtU32) return true;
		if (std::ranges::find(visiting, inst) != visiting.end()) return false;
		visiting.push_back(inst);
		bool dependent = false;
		if (inst->GetOpcode() == ValueOpcode::ReadConst && inst->NumArgs() == 2u) {
			const auto slot = inst->Arg(1).Resolve();
			if (slot.IsImmediate() && slot.GetType() == Type::U32 &&
			    slot.U32() < program.srt_reads.size() && slot.U32() < slots.size()) {
				dependent = depends_on_bounded(program.srt_reads[slot.U32()].value);
				if (dependent) slots[slot.U32()] = ResourcePlan::FlatSlotDeferred;
			}
		} else {
			for (uint32_t arg = 0; arg < inst->NumArgs(); ++arg)
				dependent = depends_on_bounded(inst->Arg(arg)) || dependent;
		}
		visiting.pop_back();
		return dependent;
	};
	for (uint32_t word = 0; word < source->dword_count; ++word)
		depends_on_bounded(source->dwords[word]);
}

uint64_t ScalarBufferSize(const ShaderBufferResource& descriptor) {
	return descriptor.Stride() == 0u
	           ? descriptor.NumRecords()
	           : static_cast<uint64_t>(descriptor.Stride()) * descriptor.NumRecords();
}

bool ReadSpecializationWord(const SrtRuntime& runtime, uint64_t address, uint32_t& word) {
	return runtime.read_specialization_memory != nullptr &&
	       runtime.read_specialization_memory(runtime.userdata, address, &word);
}

bool ReadScalarBufferWord(const ShaderBufferResource& descriptor, uint32_t dynamic_offset,
                          uint32_t immediate_offset, const SrtRuntime& runtime, uint32_t& word) {
	const auto byte_offset = static_cast<uint64_t>(dynamic_offset) + immediate_offset;
	const auto aligned     = byte_offset & ~uint64_t {3};
	const auto size        = ScalarBufferSize(descriptor);
	if (aligned > size || size - aligned < sizeof(uint32_t)) {
		word = 0;
		return true;
	}
	const auto base = descriptor.Base48() & ~uint64_t {3};
	if (aligned > AddressMask - base) {
		return false;
	}
	const auto address = base + aligned;
	if (!ReadSpecializationWord(runtime, address, word)) {
		return false;
	}
	return true;
}

bool MaterializeIndirectImage(const DescriptorSource::IndirectImage& indirect,
                              const DescriptorValue&                 material_value,
                              const DescriptorValue& heap_value, bool r128,
                              const SrtRuntime& runtime, IndirectImage& result) {
	ShaderBufferResource material;
	ShaderBufferResource heap;
	if (!DecodeBufferDescriptor(material_value, material) ||
	    !DecodeBufferDescriptor(heap_value, heap)) {
		return false;
	}
	if (material.Stride() != indirect.selector_stride) {
		return false;
	}

	// S_BUFFER_LOAD ignores vector-buffer swizzle/add-thread fields. The shader computes the
	// record stride explicitly; enumerate every wrapped 32-bit offset that can pass bounds.
	const auto period      = uint64_t {1} << 32u;
	const auto step        = std::gcd<uint64_t>(indirect.selector_stride, period);
	const auto residue     = static_cast<uint64_t>(indirect.selector_offset) % step;
	const auto size        = ScalarBufferSize(material);
	const auto limit       = std::min<uint64_t>(UINT32_MAX, size + 3u);
	const auto probe_count = residue <= limit ? (limit - residue) / step + 1u : 0u;
	if (probe_count > MaxIndirectImageProbes) {
		return false;
	}

	std::vector<uint32_t>        keys {0u};
	std::unordered_set<uint32_t> seen {0u};
	keys.reserve(static_cast<size_t>(probe_count) + 1u);
	seen.reserve(static_cast<size_t>(probe_count) + 1u);
	for (uint64_t offset = residue; offset <= limit && probe_count != 0u; offset += step) {
		uint32_t key = 0;
		if (!ReadScalarBufferWord(material, static_cast<uint32_t>(offset), 0u, runtime, key)) {
			return false;
		}
		if (seen.insert(key).second) {
			keys.push_back(key);
		}
		if (limit - offset < step) {
			break;
		}
	}

	IndirectImage next;
	next.keys = std::move(keys);
	next.candidates.reserve(next.keys.size());
	next.descriptors.reserve(
	    std::min(next.keys.size(), static_cast<size_t>(ShaderInfo::MaxImages)));
	for (const auto key: next.keys) {
		DescriptorValue candidate;
		candidate.dword_count  = 8u;
		const auto heap_offset = key << 5u;
		for (uint32_t dword = 0; dword < candidate.dword_count; dword++) {
			if (!ReadScalarBufferWord(heap, heap_offset, dword * sizeof(uint32_t), runtime,
			                          candidate.dwords[dword])) {
				return false;
			}
		}
		if (NullImageDescriptor(candidate) || !ValidImageDescriptor(candidate, r128)) {
			candidate.dwords.fill(0);
		}
		const auto found = std::ranges::find(next.descriptors, candidate);
		if (found == next.descriptors.end()) {
			if (next.descriptors.size() >= ShaderInfo::MaxImages) {
				return false;
			}
			next.descriptors.push_back(candidate);
			next.candidates.push_back(static_cast<uint32_t>(next.descriptors.size() - 1u));
		} else {
			next.candidates.push_back(static_cast<uint32_t>(found - next.descriptors.begin()));
		}
	}
	result = std::move(next);
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
                        ResourceSnapshot& snapshot, ResourceSpecialization& specialization) {
	std::vector<DescriptorValue> buffers;
	specialization.bounded_srt_reads = materialized.bounded_srt_reads;
	if (!program.bounded_srt_reads.empty()) {
		specialization.buffer_tables.resize(program.info.buffers.size());
	}
	for (uint32_t logical = 0; logical < program.info.buffers.size(); logical++) {
		const auto* source = Source(program, program.info.buffers[logical].source);
		if (source == nullptr || !source->bounded_buffer.has_value()) {
			buffers.push_back(snapshot.buffers[logical]);
			specialization.buffer_origins.push_back(logical);
			continue;
		}
		const auto& bounded = *source->bounded_buffer;
		if (source->dword_count != 4u || bounded.key_arg != 0u || program.bounded_srt_reads.empty()) {
			return SpecializationFail("bounded buffer has invalid source metadata");
		}
		uint32_t count = 0;
		uint32_t diagnostic_stride = 0;
		if (bounded.expression) {
			if (logical >= materialized.bounded_buffer_expressions.size()) {
				return SpecializationFail("bounded buffer expression candidates are missing");
			}
			count = static_cast<uint32_t>(materialized.bounded_buffer_expressions[logical].size());
		} else {
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
			if (bounded.expression) {
				descriptor = materialized.bounded_buffer_expressions[logical][index];
			} else {
				descriptor.dword_count = 4u;
				for (uint32_t word = 0; word < 4u; word++) {
					const auto& layout = specialization.bounded_srt_reads[bounded.reads[word]];
					descriptor.dwords[word] = snapshot.flattened_srt[layout.flat_offset + index];
				}
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

bool ValidateSnapshotBufferWrites(const ResourcePlan& program, const ResourceSnapshot& snapshot,
                                 const ResourceSpecialization& specialization) {
	if (snapshot.immutable_srt_ranges.empty()) {
		return true;
	}
	constexpr uint64_t buffer_limit = uint64_t{1} << 40u;
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
		if (address >= buffer_limit || size > buffer_limit - address) {
			return SpecializationFail("bounded SRT buffer writer exceeds the registered 40-bit address range");
		}
		for (const auto& read: snapshot.immutable_srt_ranges) {
			if (address < read.address + read.size && read.address < address + size) {
				return SpecializationFail(fmt::format(
				    "immutable SRT snapshot overlaps writable buffer {} (source=0x{:x}+{} writer=0x{:x}+{})",
				    resource, read.address, read.size, address, size));
			}
		}
	}
	return true;
}

} // namespace

static bool MaterializeSnapshot(const ResourcePlan& program, const SrtRuntime& input_runtime,
                                MaterializedSnapshot& snapshot) {
	if (!program.resource_tracking_complete) {
		return SpecializationFail("resource plan is incomplete");
	}

	SnapshotReader reader {input_runtime};
	SrtRuntime runtime = input_runtime;
	if (!program.bounded_srt_reads.empty()) {
		runtime.userdata = &reader;
		runtime.read_memory = SnapshotReader::Ordinary;
		runtime.read_specialization_memory = SnapshotReader::Clean;
	}
	if ((program.requires_specialization_memory || !program.bounded_srt_reads.empty()) &&
	    input_runtime.read_specialization_memory == nullptr) {
		return SpecializationFail("coherent specialization memory reader is unavailable");
	}
	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	if (!EvaluateRuntimeSources(program, program.materialization_sources, runtime, values,
	                            flattened_srt, program.clean_flat_slots)) {
		return SpecializationFail("runtime descriptor/SRT evaluation failed");
	}

	auto& next   = snapshot.resources;
	auto  cursor = values.begin();
	next.buffers.resize(program.info.buffers.size());
	for (uint32_t index = 0; index < program.info.buffers.size(); index++) {
		const auto* source = Source(program, program.info.buffers[index].source);
		if (source == nullptr || !source->bounded_buffer.has_value()) {
			if (cursor == values.end()) {
				return SpecializationFail("buffer materialization source list is incomplete");
			}
			next.buffers[index] = *cursor++;
		}
	}
	next.flattened_srt = std::move(flattened_srt);
	next.images.resize(program.info.images.size());
	for (uint32_t image_index = 0; image_index < program.info.images.size(); image_index++) {
		const auto& image  = program.info.images[image_index];
		const auto* source = Source(program, image.source);
		if (source != nullptr && source->inline_descriptor.has_value()) {
			const SampledResourcePair* pair = nullptr;
			const DescriptorSource* sampler = nullptr;
			for (const auto& candidate: program.info.sampled_pairs) {
				if (candidate.image != image_index) {
					continue;
				}
				if (candidate.sampler >= program.info.samplers.size()) {
					return SpecializationFail("inline image references a missing sampler");
				}
				const auto* candidate_sampler = Source(program, program.info.samplers[candidate.sampler].source);
				if (candidate_sampler == nullptr || candidate_sampler->dword_count != 4u) {
					return SpecializationFail("inline image has an invalid sampler source");
				}
				if (pair != nullptr && pair->sampler != candidate.sampler &&
				    (sampler->inline_descriptor.has_value() || candidate_sampler->inline_descriptor.has_value())) {
					return SpecializationFail(fmt::format(
					    "inline image at pc 0x{:08x} mixes dynamic sampler sources", image.first_use_pc));
				}
				pair = &candidate;
				sampler = candidate_sampler;
			}
			const bool expects_r128 = !source->inline_descriptor->image_table.has_value() &&
			                          source->inline_descriptor->descriptor_dwords == 4u;
			if (image.r128 != expects_r128 ||
			    image.resource_class != ImageResourceClass::Sampled ||
			    pair == nullptr || pair->sampler >= program.info.samplers.size()) {
				return SpecializationFail(fmt::format(
				    "inline image at pc 0x{:08x} has an invalid sampled descriptor width", image.first_use_pc));
			}
			const auto* inline_sampler = sampler->inline_descriptor.has_value()
			                                 ? &*sampler->inline_descriptor : nullptr;
			std::vector<uint32_t> requests {source->inline_descriptor->buffer_source};
			if (source->inline_descriptor->image_table.has_value()) {
				requests.push_back(source->inline_descriptor->image_table->address_source);
			}
			SrtRuntime clean_runtime = runtime;
			clean_runtime.read_memory = runtime.read_specialization_memory;
			std::vector<DescriptorValue> tables;
			if (!EvaluateDescriptorSources(program, requests, clean_runtime, tables)) {
				return SpecializationFail(fmt::format(
				    "inline sampled pair at pc 0x{:08x}: buffer or table address descriptor is unavailable or GPU-dirty",
				    image.first_use_pc));
			}
			IndirectImage table;
			if (!MaterializeInlineImage(*source->inline_descriptor, inline_sampler,
			                            tables[0], tables.size() > 1u ? &tables[1] : nullptr,
			                            image.first_use_pc, runtime, table)) {
				return SpecializationFail("inline sampled table materialization failed");
			}
			next.images[image_index] = table.descriptors[0];
			if (table.descriptors.size() > 1u) {
				table.resource = image_index;
				table.sampler_resource = inline_sampler != nullptr ? pair->sampler : UINT32_MAX;
				snapshot.indirect_images.push_back(std::move(table));
			}
		} else if (source != nullptr && source->indirect_image.has_value()) {
			const std::array requests {source->indirect_image->material_source,
			                           source->indirect_image->heap_source};
			SrtRuntime       clean_runtime = runtime;
			clean_runtime.read_memory      = runtime.read_specialization_memory;
			std::vector<DescriptorValue> tables;
			if (!EvaluateDescriptorSources(program, requests, clean_runtime, tables)) {
				return SpecializationFail("indirect image table source evaluation failed");
			}
			const auto&   material = tables[0];
			const auto&   heap     = tables[1];
			IndirectImage table;
			if (!MaterializeIndirectImage(*source->indirect_image, material, heap, image.r128,
			                              runtime, table)) {
				return SpecializationFail("indirect image table materialization failed");
			}
			next.images[image_index] = table.descriptors[table.candidates[0]];
			if (table.descriptors.size() > 1u) {
				table.resource = image_index;
				snapshot.indirect_images.push_back(std::move(table));
			}
		} else if (source == nullptr || !source->bounded_image.has_value()) {
			if (cursor == values.end()) {
				return SpecializationFail("image materialization source list is incomplete");
			}
			auto descriptor = *cursor++;
			if (!ValidImageDescriptor(descriptor, image.r128)) {
				descriptor.dwords.fill(0);
			}
			next.images[image_index] = descriptor;
		}
	}
	next.samplers.resize(program.info.samplers.size());
	for (uint32_t index = 0; index < program.info.samplers.size(); index++) {
		const auto* source = Source(program, program.info.samplers[index].source);
		if (source != nullptr && (source->inline_descriptor.has_value() ||
		                          source->bounded_sampler.has_value())) {
			next.samplers[index].dword_count = 4u;
		} else {
			if (cursor == values.end()) {
				return SpecializationFail("sampler materialization source list is incomplete");
			}
			next.samplers[index] = *cursor++;
		}
	}
	for (const auto& pair: program.info.sampled_pairs) {
		if (pair.image >= program.info.images.size() || pair.sampler >= program.info.samplers.size()) {
			return SpecializationFail("sampled pair references a missing resource");
		}
		const auto* sampler = Source(program, program.info.samplers[pair.sampler].source);
		const auto* image = Source(program, program.info.images[pair.image].source);
		if (sampler != nullptr && sampler->inline_descriptor.has_value() &&
		    (image == nullptr || !image->inline_descriptor.has_value())) {
			return SpecializationFail(fmt::format(
			    "inline sampler at pc 0x{:08x} requires a matching inline image", pair.first_use_pc));
		}
	}
	if (cursor != values.end()) {
		return SpecializationFail(fmt::format(
		    "runtime descriptor source count does not match resources (unused={})",
		    std::distance(cursor, values.end())));
	}
	if (!MaterializeBoundedReads(program, runtime, snapshot)) {
		return SpecializationFail("bounded SRT materialization failed");
	}
	if (!MaterializeBoundedBufferExpressions(program, runtime, snapshot)) {
		return SpecializationFail("bounded buffer expression materialization failed");
	}
	if (!MaterializeBoundedImageExpressions(program, runtime, snapshot)) {
		return SpecializationFail("bounded image expression materialization failed");
	}
	if (!MaterializeBoundedSamplerExpressions(program, runtime, snapshot)) {
		return SpecializationFail("bounded sampler expression materialization failed");
	}
	if (!MaterializeBoundedImages(program, snapshot)) {
		return SpecializationFail("bounded image materialization failed");
	}
	if (!program.bounded_srt_reads.empty()) {
		reader.Finish(next);
	}
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return true;
}

struct SamplerPlan {
	std::array<uint32_t, ShaderInfo::MaxSamplers> point_sampler {};
	uint32_t                                      sampler_count = 0;
};

template <typename Images>
bool BuildSamplerPlan(const ShaderInfo& base, const Images& images, SamplerPlan& plan);

static bool BuildResourceSpecialization(const ResourcePlan& program, MaterializedSnapshot snapshot,
                                        ResourceSnapshot&       specialized_snapshot,
                                        ResourceSpecialization& specialization) {
	auto                   next_snapshot = std::move(snapshot.resources);
	ResourceSpecialization next_specialization;
	if (!ExpandBufferTables(program, snapshot, next_snapshot, next_specialization)) {
		return false;
	}
	next_specialization.buffers.reserve(next_snapshot.buffers.size());
	next_specialization.sampler_origins.resize(program.info.samplers.size());
	std::iota(next_specialization.sampler_origins.begin(),
	          next_specialization.sampler_origins.end(), 0u);
	size_t image_count   = program.info.images.size();
	size_t mapping_words = 0;
	for (const auto& table: snapshot.indirect_images) {
		if (table.resource >= program.info.images.size() || table.descriptors.size() < 2u ||
		    image_count + table.descriptors.size() - 1u > ShaderInfo::MaxImages) {
			if (table.selector_stride != 0u) {
				return SpecializationFail(fmt::format(
				    "inline sampled pairs exceed the dense image resource limit (size={} stride={} probes={} pairs={} images={})",
				    table.buffer_size, table.selector_stride, table.probe_count,
				    table.descriptors.size(), image_count + table.descriptors.size() - 1u));
			}
			return SpecializationFail(
			    "indirect image candidates exceed the dense image resource limit");
		}
		image_count += table.descriptors.size() - 1u;
		mapping_words += 1u + table.keys.size() * 2u;
	}
	next_snapshot.images.reserve(image_count);
	next_snapshot.flattened_srt.reserve(next_snapshot.flattened_srt.size() + mapping_words);
	next_specialization.images.reserve(image_count);
	for (const auto& image: program.info.images) {
		next_specialization.images.push_back({
		    .numeric_class              = image.numeric_class,
		    .dimension                  = image.dimension,
		    .mip_count                  = image.mip_count,
		    .conversion_format          = image.conversion_format,
		    .shader_swizzle             = image.shader_swizzle,
		    .indirect_root              = image.indirect_root,
		    .indirect_mapping_offset    = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .indirect_sampler           = image.indirect_sampler,
		    .cube                       = image.cube,
		    .needs_manual_depth_compare = false,
		});
	}
	for (const auto& table: snapshot.indirect_images) {
		const auto root_image = next_specialization.images[table.resource];
		std::vector<uint32_t> candidate_samplers(table.descriptors.size(), UINT32_MAX);
		if (table.sampler_resource != UINT32_MAX) {
			if (table.sampler_resource >= program.info.samplers.size() ||
			    table.samplers.size() != table.descriptors.size()) {
				return SpecializationFail("inline sampled pair has an invalid sampler table");
			}
			for (uint32_t candidate = 0; candidate < table.samplers.size(); candidate++) {
				uint32_t sampler = 0;
				while (sampler < next_specialization.sampler_origins.size() &&
				       (next_specialization.sampler_origins[sampler] != table.sampler_resource ||
				        next_snapshot.samplers[sampler] != table.samplers[candidate])) {
					sampler++;
				}
				if (sampler == next_specialization.sampler_origins.size()) {
					if (sampler >= ShaderInfo::MaxSamplers) {
						return SpecializationFail(fmt::format(
						    "inline sampled pair at pc 0x{:08x} exceeds the sampler limit (size={} stride={} probes={} pairs={} samplers={})",
						    program.info.images[table.resource].first_use_pc,
						    table.buffer_size, table.selector_stride, table.probe_count,
						    table.descriptors.size(), sampler + 1u));
					}
					next_specialization.sampler_origins.push_back(table.sampler_resource);
					next_snapshot.samplers.push_back(table.samplers[candidate]);
				}
				candidate_samplers[candidate] = sampler;
			}
		}
		for (uint32_t candidate = 1; candidate < table.descriptors.size(); candidate++) {
			auto image          = root_image;
			image.indirect_root = table.resource;
			image.indirect_sampler = candidate_samplers[candidate];
			next_specialization.images.push_back(image);
			next_snapshot.images.push_back(table.descriptors[candidate]);
		}
		auto& root                      = next_specialization.images[table.resource];
		root.indirect_root              = table.resource;
		root.indirect_sampler           = candidate_samplers[0];
		root.indirect_mapping_offset    = static_cast<uint32_t>(next_snapshot.flattened_srt.size());
		root.indirect_search_iterations = std::bit_width(table.keys.size());
		next_snapshot.flattened_srt.resize(next_snapshot.flattened_srt.size() + 1u +
		                                   table.keys.size() * 2u);
		std::vector<uint32_t> order(table.keys.size());
		std::iota(order.begin(), order.end(), 0u);
		std::ranges::sort(order, {}, [&](uint32_t index) { return table.keys[index]; });
		next_snapshot.flattened_srt[root.indirect_mapping_offset] =
		    static_cast<uint32_t>(table.keys.size());
		for (uint32_t entry = 0; entry < order.size(); entry++) {
			const auto source                   = order[entry];
			const auto offset                   = root.indirect_mapping_offset + 1u + entry * 2u;
			next_snapshot.flattened_srt[offset] = table.keys[source];
			next_snapshot.flattened_srt[offset + 1] = table.candidates[source];
		}
		next_snapshot.images[table.resource] = table.descriptors[0];
	}
	for (uint32_t i = 0; i < next_snapshot.buffers.size(); i++) {
		const auto& base_buffer = program.info.buffers[next_specialization.buffer_origins[i]];
		auto&                descriptor_value = next_snapshot.buffers[i];
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
		next_specialization.buffers.push_back({
		    .packed_stride     = packed_stride,
		    .descriptor_format = base_buffer.formatted
		                             ? descriptor.Format()
		                             : Prospero::BufferFormat::kInvalid,
		    .descriptor_swizzle =
		        base_buffer.formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7),
		});
	}
	for (uint32_t i = 0; i < next_specialization.images.size(); i++) {
		const auto& descriptor = next_snapshot.images[i];
		auto&       image      = next_specialization.images[i];
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
	for (uint32_t root_index = 0; root_index < next_specialization.images.size(); root_index++) {
		auto& root = next_specialization.images[root_index];
		if (root.indirect_root != root_index) {
			continue;
		}
		const auto key_count = root.indirect_mapping_offset < next_snapshot.flattened_srt.size()
		                           ? next_snapshot.flattened_srt[root.indirect_mapping_offset]
		                           : 0u;
		if (root.indirect_search_iterations == 0u || key_count == 0u ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(key_count) * 2u >
		        next_snapshot.flattened_srt.size()) {
			return SpecializationFail("indirect image specialization has an invalid key mapping");
		}
		uint32_t exemplar       = ImageResource::NoIndirectImage;
		uint32_t resource_count = 0;
		for (uint32_t resource = 0; resource < next_specialization.images.size(); resource++) {
			if (next_specialization.images[resource].indirect_root != root_index) {
				continue;
			}
			resource_count++;
			if (exemplar == ImageResource::NoIndirectImage &&
			    !NullImageDescriptor(next_snapshot.images[resource])) {
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
		const auto& image_class = next_specialization.images[exemplar];
		for (uint32_t candidate = 0; candidate < next_specialization.images.size(); candidate++) {
			auto& image = next_specialization.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(next_snapshot.images[candidate])) {
				image.numeric_class              = image_class.numeric_class;
				image.dimension                  = image_class.dimension;
				image.mip_count                  = image_class.mip_count;
				image.conversion_format          = image_class.conversion_format;
				image.shader_swizzle             = image_class.shader_swizzle;
				image.cube                       = image_class.cube;
				image.needs_manual_depth_compare = image_class.needs_manual_depth_compare;
			}
			if (image.numeric_class != image_class.numeric_class ||
			    image.dimension != image_class.dimension ||
			    image.mip_count != image_class.mip_count ||
			    image.conversion_format != image_class.conversion_format ||
			    image.shader_swizzle != image_class.shader_swizzle ||
			    image.cube != image_class.cube ||
			    image.needs_manual_depth_compare != image_class.needs_manual_depth_compare) {
				return SpecializationFail(
				    fmt::format("indirect image table at pc 0x{:08x} has incompatible candidates",
				                program.info.images[root_index].first_use_pc));
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
	if (!BuildSamplerPlan(sampler_info, next_specialization.images, sampler_plan)) {
		return SpecializationFail("specialized sampler layout exceeds its resource limit");
	}
	for (uint32_t index = 0; index < sampler_info.samplers.size(); index++) {
		const auto target = sampler_plan.point_sampler[index];
		if (target != UINT32_MAX && target >= sampler_info.samplers.size()) {
			next_snapshot.samplers.push_back(next_snapshot.samplers[index]);
		}
	}
	if (!ValidateSnapshotBufferWrites(program, next_snapshot, next_specialization)) {
		return false;
	}
	specialization       = std::move(next_specialization);
	specialized_snapshot = std::move(next_snapshot);
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
		target.inline_descriptor = source.inline_descriptor;
		target.bounded_buffer = source.bounded_buffer;
		target.bounded_image = source.bounded_image;
		target.bounded_sampler = source.bounded_sampler;
		for (uint32_t dword = 0; dword < source.dword_count; dword++) {
			target.dwords[dword] = Clone(source.dwords[dword]);
		}
	}
	plan.srt_reads.reserve(program.srt_reads.size());
	for (const auto& read: program.srt_reads) {
		plan.srt_reads.push_back({Clone(read.value), read.flat_offset});
	}
	plan.materialization_sources.reserve(plan.info.buffers.size() + plan.info.images.size() +
	                                     plan.info.samplers.size());
	for (const auto& buffer: plan.info.buffers) {
		const auto* source = Source(plan, buffer.source);
		if (source != nullptr && source->bounded_buffer.has_value()) {
			plan.requires_specialization_memory = true;
		} else {
			plan.materialization_sources.push_back(buffer.source);
		}
	}
	for (const auto& image: plan.info.images) {
		const auto* source = Source(plan, image.source);
		if (source != nullptr && (source->indirect_image.has_value() ||
		                          source->inline_descriptor.has_value() ||
		                          source->bounded_image.has_value())) {
			plan.requires_specialization_memory = true;
		} else {
			plan.materialization_sources.push_back(image.source);
		}
	}
	for (const auto& sampler: plan.info.samplers) {
		const auto* source = Source(plan, sampler.source);
		if (source != nullptr && (source->inline_descriptor.has_value() ||
		                          source->bounded_sampler.has_value())) {
			plan.requires_specialization_memory = true;
		} else {
			plan.materialization_sources.push_back(sampler.source);
		}
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
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->material_source),
		                   plan.clean_flat_slots);
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->heap_source),
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
	MaterializedSnapshot materialized;
	if (!MaterializeSnapshot(program, runtime, materialized)) {
		return false;
	}
	return BuildResourceSpecialization(program, std::move(materialized), snapshot, specialization);
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
		if (source == nullptr || !source->bounded_buffer.has_value()) {
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
	std::vector<uint8_t> remapped_memory(memory_info.size());
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (BufferAccessOf(inst.GetOpcode()) == BufferAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			EXIT_IF(index >= memory_info.size());
			auto& memory = memory_info[index];
			if (memory.planning_only) {
				continue;
			}
			if (!remapped_memory[index]) {
				remapped_memory[index] = 1u;
				if (memory.buffer_table != UINT32_MAX) {
					EXIT_IF(memory.buffer_table >= specialization.buffer_tables.size());
					memory.resource = UINT32_MAX;
				} else {
					EXIT_IF(memory.resource >= buffer_remap.size() || buffer_remap[memory.resource] == UINT32_MAX);
					memory.resource = buffer_remap[memory.resource];
				}
			}
			if (memory.buffer_table == UINT32_MAX) {
				auto* handle = inst.Arg(0).Resolve().TryInstruction();
				EXIT_IF(handle == nullptr || handle->GetOpcode() != ValueOpcode::GetBufferResource);
				handle->SetFlags<uint32_t>(memory.resource);
			}
		}
	}
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			const auto image_opcode = ImageOpcodeInfoOf(inst.GetOpcode());
			if (image_opcode.access == ImageAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			EXIT_IF(index >= memory_info.size());
			auto& memory = memory_info[index];
			EXIT_IF(memory.resource >= images.size());
			const auto& image = images[memory.resource];
			if (image_opcode.needs_sampler && RequiresPointSampler(image) &&
			    memory.sampler < program.info.samplers.size()) {
				EXIT_IF(sampler_plan.point_sampler[memory.sampler] == UINT32_MAX);
				memory.sampler = sampler_plan.point_sampler[memory.sampler];
			}
			EXIT_IF(image.indirect_root == memory.resource &&
			        inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageRead);
		}
	}
	program.info.bounded_srt_reads = specialization.bounded_srt_reads;
	program.info.buffer_tables = specialization.buffer_tables;
	program.info.buffers       = std::move(buffers);
	program.info.images        = std::move(images);
	program.info.samplers      = std::move(samplers);
	program.info.sampled_pairs = std::move(sampled_pairs);
	program.memory_info        = std::move(memory_info);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
