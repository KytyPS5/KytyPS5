#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "graphics/guest_gpu/gpu_format.h"
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

namespace Libs::Graphics::ShaderRecompiler::IR {
namespace {

constexpr uint64_t AddressMask            = 0x0000ffffffffffffull;
constexpr uint64_t MaxIndirectImageProbes = 65536u;
constexpr uint32_t DenseDescriptorShift   = 5u;

struct IndirectImage {
	uint32_t                     resource = 0;
	std::vector<uint32_t>        keys;
	std::vector<uint32_t>        candidates;
	std::vector<DescriptorValue> descriptors;
	uint32_t                     dropped_candidates = 0;
	uint32_t                     dropped_shapes     = 0;
	std::string                  dropped_summary;
};

struct MaterializedSnapshot {
	ResourceSnapshot           resources;
	std::vector<IndirectImage> indirect_images;
};

bool SpecializationFail(MaterializeReport* report, std::string_view message) {
	if (report != nullptr) {
		report->reason = fmt::format("specialization failed: {}", message);
	}
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
	if (((descriptor.dwords[4] >> 16u) & 0x1fffu) > (descriptor.dwords[4] & 0x1fffu)) {
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

bool ReservedImageBitsClear(const DescriptorValue& descriptor) {
	constexpr std::array<uint32_t, 8> reserved = {0x00000000u, 0x20000000u, 0xf0003000u,
	                                              0x00000000u, 0xe000e000u, 0xf9000000u,
	                                              0x00007b00u, 0x00000000u};
	for (uint32_t dword = 0; dword < reserved.size(); dword++) {
		if ((descriptor.dwords[dword] & reserved[dword]) != 0u) {
			return false;
		}
	}
	return true;
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

struct IndirectImageClass {
	Decoder::ImageDimension dimension    = Decoder::ImageDimension::Unknown;
	Prospero::BufferFormat  conversion   = Prospero::BufferFormat::kInvalid;
	uint32_t                swizzle      = 0;
	uint32_t                mip_count    = 0;
	bool                    cube         = false;
	Prospero::TextureNumericClass numeric_class = Prospero::TextureNumericClass::Unsupported;

	bool operator==(const IndirectImageClass& other) const = default;
};

IndirectImageClass DescriptorClass(const ImageResource& image, const DescriptorValue& descriptor) {
	const auto format = static_cast<Prospero::BufferFormat>((descriptor.dwords[1] >> 20u) & 0x1ffu);
	IndirectImageClass result;
	result.dimension  = DescriptorDimension(descriptor, image.dimension);
	result.conversion = ImageConversionFormat(format);
	result.swizzle    = result.conversion == Prospero::BufferFormat::kInvalid
	                        ? 0u
	                        : DescriptorImageSwizzle(descriptor);
	result.mip_count  = StorageMipCount(image, descriptor);
	result.cube       = DescriptorIsCube(descriptor);
	result.numeric_class = Prospero::SampledTextureNumericClass(format);
	return result;
}

std::string ClassText(const IndirectImageClass& value) {
	return fmt::format("dim={} cube={} class={} conv={} swizzle={:03x} mips={}",
	                   static_cast<uint32_t>(value.dimension), static_cast<uint32_t>(value.cube),
	                   static_cast<uint32_t>(value.numeric_class),
	                   static_cast<uint32_t>(value.conversion), value.swizzle, value.mip_count);
}

std::string SpecializationImageText(const ResourceSpecialization::Image& image) {
	return fmt::format("class={} dim={} mips={} conv={} swizzle={:03x} cube={}",
	                   static_cast<uint32_t>(image.numeric_class),
	                   static_cast<uint32_t>(image.dimension), image.mip_count,
	                   static_cast<uint32_t>(image.conversion_format), image.shader_swizzle,
	                   static_cast<uint32_t>(image.cube));
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
				slots[slot.U32()] = 1u;
				pending.push_back(program.srt_reads[slot.U32()].value);
			}
			continue;
		}
		for (size_t arg = 0; arg < inst->NumArgs(); arg++) {
			pending.push_back(inst->Arg(arg));
		}
	}
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

void MakeRangeReadable(const SrtRuntime& runtime, uint64_t base, uint64_t size) {
	if (runtime.sync_memory != nullptr && size != 0u && base <= AddressMask - size) {
		runtime.sync_memory(runtime.userdata, base, size);
	}
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

bool FinishIndirectImage(const ImageResource& image, std::vector<DescriptorValue>& probed,
                         bool over_approximates, std::string enumeration, IndirectImage& next,
                         std::string* reason) {
	std::vector<IndirectImageClass> classes;
	std::vector<uint32_t>           tally;
	std::vector<uint32_t>           shape(probed.size(), UINT32_MAX);
	for (uint32_t index = 0; index < probed.size(); index++) {
		if (NullImageDescriptor(probed[index])) {
			continue;
		}
		const auto value = DescriptorClass(image, probed[index]);
		const auto found = std::ranges::find(classes, value);
		shape[index]     = static_cast<uint32_t>(found - classes.begin());
		if (found == classes.end()) {
			classes.push_back(value);
			tally.push_back(1u);
		} else {
			tally[shape[index]]++;
		}
	}
	if (classes.size() > 1u) {
		const auto dominant =
		    static_cast<uint32_t>(std::ranges::max_element(tally) - tally.begin());
		if (over_approximates) {
			for (uint32_t index = 0; index < probed.size(); index++) {
				if (shape[index] != UINT32_MAX && shape[index] != dominant) {
					probed[index].dwords.fill(0);
					next.dropped_candidates++;
				}
			}
			next.dropped_shapes = static_cast<uint32_t>(classes.size() - 1u);
		}
		next.dropped_summary =
		    fmt::format("{} {}={}", std::move(enumeration),
		                over_approximates ? "kept" : "exact probe, refused",
		                ClassText(classes[dominant]));
		for (uint32_t index = 0; index < classes.size(); index++) {
			if (index != dominant) {
				next.dropped_summary +=
				    fmt::format(" | {} x{} {}", over_approximates ? "dropped" : "kept",
				                tally[index], ClassText(classes[index]));
			}
		}
	}
	next.candidates.reserve(probed.size());
	for (const auto& candidate: probed) {
		const auto found = std::ranges::find(next.descriptors, candidate);
		if (found == next.descriptors.end()) {
			if (next.descriptors.size() >= ShaderInfo::MaxImages) {
				if (reason != nullptr) {
					*reason = "indirect image candidates exceed the dense image resource limit";
				}
				return false;
			}
			next.descriptors.push_back(candidate);
			next.candidates.push_back(static_cast<uint32_t>(next.descriptors.size() - 1u));
		} else {
			next.candidates.push_back(static_cast<uint32_t>(found - next.descriptors.begin()));
		}
	}
	return true;
}

bool MaterializeDenseIndirectImage(const DescriptorSource::IndirectImage& indirect,
                                   const ImageResource&   image,
                                   const DescriptorValue& heap_value,
                                   const DescriptorValue* bound_value, const SrtRuntime& runtime,
                                   IndirectImage& result, std::string* reason) {
	const auto note = [reason](const char* why) {
		if (reason != nullptr) {
			*reason = why;
		}
		return false;
	};
	if (heap_value.dword_count != 2u || indirect.key_bound == 0u ||
	    indirect.key_bound > MaxIndirectImageProbes) {
		return note("dense table heap is not an address resource");
	}
	const auto base =
	    ((static_cast<uint64_t>(heap_value.dwords[1]) << 32u) | heap_value.dwords[0]) & AddressMask;

	uint32_t entries = indirect.key_bound;
	if (indirect.bound_source != DescriptorSource::IndirectImage::NoBoundSource) {
		if (bound_value == nullptr || bound_value->dword_count != 1u) {
			return note("dense table loop bound did not evaluate");
		}
		const auto raw   = bound_value->dwords[0];
		const auto count = indirect.bound_signed && (raw & 0x80000000u) != 0u ? 0u : raw;
		if (count > indirect.key_bound) {
			return note("dense table loop bound exceeds the enumeration cap");
		}
		entries = count;
	}
	const uint32_t enumerated = std::max(entries, 1u);
	MakeRangeReadable(runtime, base + indirect.table_offset,
	                  static_cast<uint64_t>(enumerated) << DenseDescriptorShift);
	IndirectImage next;
	std::vector<DescriptorValue> probed;
	probed.reserve(enumerated);
	next.keys.reserve(enumerated);
	for (uint32_t key = 0; key < enumerated; key++) {
		DescriptorValue candidate;
		candidate.dword_count = 8u;
		const auto entry      = static_cast<uint64_t>(indirect.table_offset) +
		                   (static_cast<uint64_t>(key) << DenseDescriptorShift);
		for (uint32_t dword = 0; key < entries && dword < candidate.dword_count; dword++) {
			const auto address = base + entry + dword * sizeof(uint32_t);
			if (address > AddressMask ||
			    !ReadSpecializationWord(runtime, address, candidate.dwords[dword])) {
				return note("dense table entry is not readable");
			}
		}
		if (key >= entries || NullImageDescriptor(candidate) ||
		    !ValidImageDescriptor(candidate, image.r128) ||
		    !ReservedImageBitsClear(candidate)) {
			candidate.dwords.fill(0);
		}
		next.keys.push_back(key);
		probed.push_back(candidate);
	}
	if (!FinishIndirectImage(image, probed, true,
	                         fmt::format("dense table=0x{:x} entries={}", indirect.table_offset,
	                                     indirect.key_bound),
	                         next, reason)) {
		return false;
	}
	result = std::move(next);
	return true;
}
bool MaterializeAddressProbeIndirectImage(const DescriptorSource::IndirectImage& indirect,
                                          const ImageResource&                   image,
                                          const DescriptorValue&                 material_value,
                                          const DescriptorValue&                 heap_value,
                                          const SrtRuntime& runtime, IndirectImage& result,
                                          std::string* reason) {
	const auto note = [reason](const char* why) {
		if (reason != nullptr) {
			*reason = why;
		}
		return false;
	};
	if (material_value.dword_count != 2u || heap_value.dword_count != 2u ||
	    indirect.item_bound == 0u || indirect.item_bound > MaxIndirectImageProbes ||
	    indirect.selector_stride == 0u) {
		return note("address probe tables are not address resources");
	}
	const auto material_base =
	    ((static_cast<uint64_t>(material_value.dwords[1]) << 32u) | material_value.dwords[0]) &
	    AddressMask;
	const auto heap_base =
	    ((static_cast<uint64_t>(heap_value.dwords[1]) << 32u) | heap_value.dwords[0]) & AddressMask;
	const auto records = static_cast<uint64_t>(indirect.item_bound);
	MakeRangeReadable(runtime, material_base + indirect.selector_offset,
	                  (records - 1u) * indirect.selector_stride + sizeof(uint32_t));
	std::vector<uint32_t>        keys {0u};
	std::unordered_set<uint32_t> seen {0u};
	for (uint64_t item = 0; item < records; item++) {
		const auto address =
		    material_base + indirect.selector_offset + item * indirect.selector_stride;
		uint32_t key = 0;
		if (address > AddressMask || !ReadSpecializationWord(runtime, address, key)) {
			return note("record key is not readable");
		}
		if (seen.insert(key).second) {
			keys.push_back(key);
		}
	}

	IndirectImage next;
	next.keys = std::move(keys);
	std::vector<DescriptorValue> probed;
	probed.reserve(next.keys.size());
	for (const auto key: next.keys) {
		DescriptorValue candidate;
		candidate.dword_count = 8u;
		const auto entry      = heap_base + indirect.table_offset +
		                   (static_cast<uint64_t>(key) << DenseDescriptorShift);
		bool readable = entry + candidate.dword_count * sizeof(uint32_t) <= AddressMask;
		if (readable) {
			MakeRangeReadable(runtime, entry, candidate.dword_count * sizeof(uint32_t));
			for (uint32_t dword = 0; readable && dword < candidate.dword_count; dword++) {
				readable = ReadSpecializationWord(runtime, entry + dword * sizeof(uint32_t),
				                                  candidate.dwords[dword]);
			}
		}
		if (!readable || NullImageDescriptor(candidate) ||
		    !ValidImageDescriptor(candidate, image.r128) || !ReservedImageBitsClear(candidate)) {
			candidate.dwords.fill(0);
		}
		probed.push_back(candidate);
	}
	if (!FinishIndirectImage(image, probed, true,
	                         fmt::format("address probe records={} stride={} table=0x{:x} keys={}",
	                                     indirect.item_bound, indirect.selector_stride,
	                                     indirect.table_offset, next.keys.size()),
	                         next, reason)) {
		return false;
	}
	result = std::move(next);
	return true;
}


bool MaterializeIndirectImage(const DescriptorSource::IndirectImage& indirect,
                              const ImageResource& image, const DescriptorValue& material_value,
                              const DescriptorValue& heap_value, const SrtRuntime& runtime,
                              IndirectImage& result, std::string* reason) {
	const auto note = [reason](const char* why) {
		if (reason != nullptr) {
			*reason = why;
		}
		return false;
	};
	ShaderBufferResource material;
	ShaderBufferResource heap;
	if (!DecodeBufferDescriptor(material_value, material) ||
	    !DecodeBufferDescriptor(heap_value, heap)) {
		return note("material or heap is not a buffer descriptor");
	}
	if (material.Stride() != indirect.selector_stride) {
		return note("material buffer stride no longer matches the tracked selector");
	}

	const auto period      = uint64_t {1} << 32u;
	const auto step        = std::gcd<uint64_t>(indirect.selector_stride, period);
	const auto residue     = static_cast<uint64_t>(indirect.selector_offset) % step;
	const auto size        = ScalarBufferSize(material);
	const auto limit       = std::min<uint64_t>(UINT32_MAX, size + 3u);
	const auto probe_count = residue <= limit ? (limit - residue) / step + 1u : 0u;
	if (probe_count > MaxIndirectImageProbes) {
		return note("material table is too large to enumerate");
	}

	MakeRangeReadable(runtime, material.Base48() & ~uint64_t {3}, ScalarBufferSize(material));
	MakeRangeReadable(runtime, heap.Base48() & ~uint64_t {3}, ScalarBufferSize(heap));
	std::vector<uint32_t>        keys {0u};
	std::unordered_set<uint32_t> seen {0u};
	keys.reserve(static_cast<size_t>(probe_count) + 1u);
	seen.reserve(static_cast<size_t>(probe_count) + 1u);
	for (uint64_t offset = residue; offset <= limit && probe_count != 0u; offset += step) {
		uint32_t key = 0;
		if (!ReadScalarBufferWord(material, static_cast<uint32_t>(offset), 0u, runtime, key)) {
			return note("material table key is not readable");
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
	std::vector<DescriptorValue> probed;
	probed.reserve(next.keys.size());
	for (const auto key: next.keys) {
		DescriptorValue candidate;
		candidate.dword_count  = 8u;
		const auto heap_offset = key << 5u;
		for (uint32_t dword = 0; dword < candidate.dword_count; dword++) {
			if (!ReadScalarBufferWord(heap, heap_offset, dword * sizeof(uint32_t), runtime,
			                          candidate.dwords[dword])) {
				return note("heap entry is not readable");
			}
		}
		if (NullImageDescriptor(candidate) || !ValidImageDescriptor(candidate, image.r128) ||
		    !ReservedImageBitsClear(candidate)) {
			candidate.dwords.fill(0);
		}
		probed.push_back(candidate);
	}
	if (!FinishIndirectImage(image, probed, step < indirect.selector_stride,
	                         fmt::format("stride={} step={} probes={} keys={}",
	                                     indirect.selector_stride, step, probe_count,
	                                     next.keys.size()),
	                         next, reason)) {
		return false;
	}
	result = std::move(next);
	return true;
}

} // namespace

static bool MaterializeSnapshot(const ResourcePlan& program, const SrtRuntime& runtime,
                                MaterializedSnapshot& snapshot, MaterializeReport* report) {
	const auto fail = [report](const char* why) {
		if (report != nullptr) {
			report->reason = why;
		}
		return false;
	};
	if (!program.resource_tracking_complete) {
		return fail("resources were not tracked");
	}

	if (program.requires_specialization_memory && runtime.read_specialization_memory == nullptr) {
		return fail("indirect image needs a specialization memory reader");
	}
	std::vector<DescriptorValue> values;
	std::vector<uint32_t>        flattened_srt;
	std::vector<uint8_t>         active_sources;
	if (!EvaluateRuntimeSources(program, program.materialization_sources, runtime, values,
	                            flattened_srt, program.clean_flat_slots, active_sources)) {
		return fail("a descriptor source did not evaluate");
	}

	auto&                   next  = snapshot.resources;
	const auto&             fill  = program.uniform_fill;
	const auto              words = fill.fill.words;
	std::array<uint32_t, 4> stored {};
	if (words != 0 &&
	    EvaluateUniformValues(program, std::span(fill.values).first(words), runtime,
	                          std::span(stored).first(words)) &&
	    std::all_of(stored.begin(), stored.begin() + words,
	                [&](uint32_t value) { return value == stored[0]; })) {
		next.uniform_fill       = fill.fill;
		next.uniform_fill.value = stored[0];
	}
	auto cursor = values.begin();
	next.buffers.assign(cursor, cursor + program.info.buffers.size());
	cursor += program.info.buffers.size();
	next.flattened_srt = std::move(flattened_srt);
	next.images.resize(program.info.images.size());
	for (uint32_t image_index = 0; image_index < program.info.images.size(); image_index++) {
		const auto& image  = program.info.images[image_index];
		const auto* source = Source(program, image.source);
		if (source != nullptr && source->indirect_image.has_value()) {
			if (!active_sources[image.source]) {
				next.images[image_index].dword_count = 8u;
				continue;
			}
			const auto&           indirect  = *source->indirect_image;
			const bool            dense     = indirect.key_bound != 0u;
			const bool            has_bound = dense && indirect.bound_source !=
			                                               DescriptorSource::IndirectImage::NoBoundSource;
			std::vector<uint32_t> requests;
			if (!dense) {
				requests.push_back(indirect.material_source);
			}
			requests.push_back(indirect.heap_source);
			if (has_bound) {
				requests.push_back(indirect.bound_source);
			}
			SrtRuntime clean_runtime  = runtime;
			clean_runtime.read_memory = runtime.read_specialization_memory;
			std::vector<DescriptorValue> tables;
			if (!EvaluateDescriptorSources(program, requests, clean_runtime, tables)) {
				return fail("an indirect image table source did not evaluate");
			}
			IndirectImage table;
			std::string   detail;
			const auto&            heap_value  = tables[dense ? 0u : 1u];
			const DescriptorValue* bound_value = has_bound ? &tables.back() : nullptr;
			const bool             materialized =
			    dense ? MaterializeDenseIndirectImage(indirect, image, heap_value, bound_value,
			                                          runtime, table, &detail)
			    : indirect.item_bound != 0u
			        ? MaterializeAddressProbeIndirectImage(indirect, image, tables.front(),
			                                               heap_value, runtime, table, &detail)
			        : MaterializeIndirectImage(indirect, image, tables.front(), heap_value,
			                                   runtime, table, &detail);
			if (!materialized) {
				return fail(detail.c_str());
			}
			if (report != nullptr) {
				report->dropped_candidates += table.dropped_candidates;
				report->dropped_shapes += table.dropped_shapes;
				if (!table.dropped_summary.empty()) {
					report->dropped_summary +=
					    fmt::format("\n  image {}: {}", image_index, table.dropped_summary);
				}
			}
			next.images[image_index] = table.descriptors[table.candidates[0]];
			if (table.descriptors.size() > 1u) {
				table.resource = image_index;
				snapshot.indirect_images.push_back(std::move(table));
			}
		} else {
			auto descriptor = *cursor++;
			if (!ValidImageDescriptor(descriptor, image.r128)) {
				descriptor.dwords.fill(0);
			}
			next.images[image_index] = descriptor;
		}
	}
	next.samplers.assign(cursor, cursor + program.info.samplers.size());
	next.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return true;
}

struct SamplerPlan {
	std::array<uint32_t, ShaderInfo::MaxSamplers> point_sampler {};
	uint32_t                                      sampler_count = 0;
};

struct ImageRemap {
	explicit ImageRemap(const ResourceSpecialization& specialization) {
		for (const auto& image: specialization.images) {
			indices.push_back(image.fmask ? UINT32_MAX : count++);
		}
	}

	template <typename T>
	void Apply(std::vector<T>& images) const {
		EXIT_IF(images.size() != indices.size());
		for (uint32_t index = 0; index < indices.size(); index++) {
			if (indices[index] != UINT32_MAX && indices[index] != index) {
				images[indices[index]] = std::move(images[index]);
			}
		}
		images.resize(count);
	}

	std::vector<uint32_t> indices;
	uint32_t              count = 0;
};

template <typename Images>
bool BuildSamplerPlan(const ShaderInfo& base, const Images& images, SamplerPlan& plan);

static bool BuildResourceSpecialization(const ResourcePlan& program, MaterializedSnapshot snapshot,
                                        ResourceSnapshot&       specialized_snapshot,
                                        ResourceSpecialization& specialization,
                                        MaterializeReport*      report) {
	auto                   next_snapshot = std::move(snapshot.resources);
	ResourceSpecialization next_specialization;
	next_specialization.buffers.reserve(program.info.buffers.size());
	size_t image_count   = program.info.images.size();
	size_t mapping_words = 0;
	for (const auto& table: snapshot.indirect_images) {
		if (table.resource >= program.info.images.size() || table.descriptors.size() < 2u ||
		    image_count + table.descriptors.size() - 1u > ShaderInfo::MaxImages) {
			return SpecializationFail(report, 
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
		    .cube                       = image.cube,
		});
	}
	for (const auto& table: snapshot.indirect_images) {
		const auto root_image = next_specialization.images[table.resource];
		for (uint32_t candidate = 1; candidate < table.descriptors.size(); candidate++) {
			auto image          = root_image;
			image.indirect_root = table.resource;
			next_specialization.images.push_back(image);
			next_snapshot.images.push_back(table.descriptors[candidate]);
		}
		auto& root                      = next_specialization.images[table.resource];
		root.indirect_root              = table.resource;
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
	for (uint32_t i = 0; i < program.info.buffers.size(); i++) {
		auto&                descriptor_value = next_snapshot.buffers[i];
		ShaderBufferResource descriptor;
		if (!DecodeBufferDescriptor(descriptor_value, descriptor)) {
			return SpecializationFail(report, fmt::format("buffer descriptor {} has invalid width", i));
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
		    .descriptor_format = program.info.buffers[i].formatted
		                             ? descriptor.Format()
		                             : Prospero::BufferFormat::kInvalid,
		    .descriptor_swizzle =
		        program.info.buffers[i].formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7),
		});
	}
	for (uint32_t i = 0; i < next_specialization.images.size(); i++) {
		const auto& descriptor = next_snapshot.images[i];
		auto&       image      = next_specialization.images[i];
		const auto  base_index = i < program.info.images.size() ? i : image.indirect_root;
		if (base_index >= program.info.images.size()) {
			return SpecializationFail(report, fmt::format("image resource {} has an invalid root", i));
		}
		const auto& base = program.info.images[base_index];
		if (base.resource_class == ImageResourceClass::None ||
		    (base.atomic && base.resource_class != ImageResourceClass::Storage)) {
			return SpecializationFail(report, fmt::format("image resource {} has an invalid class", i));
		}
		image.mip_count = StorageMipCount(base, descriptor);
		if (image.mip_count == 0u) {
			return SpecializationFail(report, 
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
			return SpecializationFail(report, fmt::format(
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
		if (base.atomic && format != Prospero::BufferFormat::k32UInt) {
			return SpecializationFail(report, 
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
				return SpecializationFail(report, "FMASK requires a direct image load");
			}
		}
		image.conversion_format = ImageConversionFormat(format);
		if (storage || image.conversion_format != Prospero::BufferFormat::kInvalid) {
			image.shader_swizzle = DescriptorImageSwizzle(descriptor);
		}
		const bool raw_sint_storage = storage && format == Prospero::BufferFormat::k32SInt &&
		                              base.written && !base.read && !base.atomic;
		image.numeric_class         = Prospero::SampledTextureNumericClass(format);
		if (storage) {
			if ((!raw_sint_storage && image.numeric_class == Prospero::TextureNumericClass::Sint) ||
			    image.numeric_class == Prospero::TextureNumericClass::Unsupported) {
				return SpecializationFail(report, 
				    fmt::format("storage image descriptor {} uses unsupported format {}", i,
				                static_cast<uint32_t>(format)));
			}
			if (raw_sint_storage) {
				image.numeric_class = Prospero::TextureNumericClass::Uint;
			}
		} else if (image.numeric_class == Prospero::TextureNumericClass::Unsupported ||
		           (base.depth_compare &&
		            image.numeric_class != Prospero::TextureNumericClass::Float)) {
			return SpecializationFail(report, 
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
		if (root.indirect_search_iterations == 0u || key_count < 2u ||
		    static_cast<size_t>(root.indirect_mapping_offset) + 1u +
		            static_cast<size_t>(key_count) * 2u >
		        next_snapshot.flattened_srt.size()) {
			return SpecializationFail(report, "indirect image specialization has an invalid key mapping");
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
		if (resource_count < 2u || exemplar == ImageResource::NoIndirectImage) {
			return SpecializationFail(report, "indirect image specialization has no typed candidate");
		}
		const auto& image_class = next_specialization.images[exemplar];
		for (uint32_t candidate = 0; candidate < next_specialization.images.size(); candidate++) {
			auto& image = next_specialization.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(next_snapshot.images[candidate])) {
				image.numeric_class     = image_class.numeric_class;
				image.dimension         = image_class.dimension;
				image.mip_count         = image_class.mip_count;
				image.conversion_format = image_class.conversion_format;
				image.shader_swizzle    = image_class.shader_swizzle;
				image.cube              = image_class.cube;
			}
			if (image.numeric_class != image_class.numeric_class ||
			    image.dimension != image_class.dimension ||
			    image.mip_count != image_class.mip_count ||
			    image.conversion_format != image_class.conversion_format ||
			    image.shader_swizzle != image_class.shader_swizzle ||
			    image.cube != image_class.cube) {
				return SpecializationFail(report, 
				    fmt::format("indirect image table at pc 0x{:08x} has incompatible candidates: "
				                "resource {} is {}, resource {} is {}",
				                program.info.images[root_index].first_use_pc, exemplar,
				                SpecializationImageText(image_class), candidate,
				                SpecializationImageText(image)));
			}
		}
	}
	SamplerPlan sampler_plan;
	if (!BuildSamplerPlan(program.info, next_specialization.images, sampler_plan)) {
		return SpecializationFail(report, "specialized sampler layout exceeds its resource limit");
	}
	for (uint32_t index = 0; index < program.info.samplers.size(); index++) {
		const auto target = sampler_plan.point_sampler[index];
		if (target != UINT32_MAX && target >= program.info.samplers.size()) {
			next_snapshot.samplers.push_back(next_snapshot.samplers[index]);
		}
	}
	ImageRemap(next_specialization).Apply(next_snapshot.images);
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

static std::vector<ResourceBlock> ResourceControlFlow(const Program& program) {
	if (program.blocks.size() != program.block_info.size()) {
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
			// Any shader write may alias a scalar predicate read, including on a later loop visit.
			if (buffer == BufferAccess::Write || buffer == BufferAccess::Atomic ||
			    image.access == ImageAccess::Write || image.access == ImageAccess::Atomic ||
			    AddressOpcodeInfoOf(op).access == AddressAccess::Write) {
				return {};
			}
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
	plan.materialization_sources.reserve(plan.info.buffers.size() + plan.info.images.size() +
	                                     plan.info.samplers.size());
	for (const auto& buffer: plan.info.buffers) {
		plan.materialization_sources.push_back(buffer.source);
	}
	for (const auto& image: plan.info.images) {
		const auto* source = Source(plan, image.source);
		if (source != nullptr && source->indirect_image.has_value()) {
			plan.requires_specialization_memory = true;
		} else {
			plan.materialization_sources.push_back(image.source);
		}
	}
	for (const auto& sampler: plan.info.samplers) {
		plan.materialization_sources.push_back(sampler.source);
	}
	plan.clean_flat_slots.resize(plan.srt_reads.size());
	for (const auto& image: plan.info.images) {
		const auto* source = Source(plan, image.source);
		if (source == nullptr || !source->indirect_image.has_value()) {
			continue;
		}
		if (source->indirect_image->key_bound == 0u) {
			MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->material_source),
			                   plan.clean_flat_slots);
		}
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->heap_source),
		                   plan.clean_flat_slots);
		if (source->indirect_image->bound_source !=
		    DescriptorSource::IndirectImage::NoBoundSource) {
			MarkCleanFlatSlots(plan, Source(plan, source->indirect_image->bound_source),
			                   plan.clean_flat_slots);
		}
	}
	return plan;
}

bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                          MaterializeReport* report) {
	if (report != nullptr) {
		*report = {};
	}
	MaterializedSnapshot materialized;
	if (!MaterializeSnapshot(program, runtime, materialized, report)) {
		return false;
	}
	return BuildResourceSpecialization(program, std::move(materialized), snapshot, specialization,
	                                   report);
}

void ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization) {
	EXIT_IF(!program.resource_tracking_complete || program.shader_info_complete ||
	        program.binding_layout_complete);
	EXIT_IF(program.info.buffers.size() != specialization.buffers.size() ||
	        program.info.images.size() > specialization.images.size());

	auto buffers = program.info.buffers;
	for (size_t index = 0; index < buffers.size(); index++) {
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

	SamplerPlan sampler_plan;
	EXIT_IF(!BuildSamplerPlan(program.info, images, sampler_plan));
	auto samplers      = program.info.samplers;
	auto sampled_pairs = program.info.sampled_pairs;
	samplers.reserve(sampler_plan.sampler_count);
	for (uint32_t index = 0; index < program.info.samplers.size(); index++) {
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
			        inst.GetOpcode() != ValueOpcode::ImageSampleRaw);
		}
	}
	for (auto* block: program.blocks) {
		for (auto& inst: *block) {
			if (inst.GetOpcode() == ValueOpcode::GetImageResource) {
				inst.SetFlags(image_remap.indices.at(inst.Flags<uint32_t>()));
			}
		}
	}
	for (auto& memory: memory_info) {
		if (memory.kind == ResourceKind::Image && !memory.planning_only) {
			memory.resource = image_remap.indices.at(memory.resource);
		}
	}
	for (auto& buffer: buffers) {
		if (buffer.image_alias != BufferResource::NoImageAlias) {
			buffer.image_alias = image_remap.indices.at(buffer.image_alias);
		}
	}
	for (auto& pair: sampled_pairs) {
		pair.image = image_remap.indices.at(pair.image);
	}
	for (auto& image: images) {
		if (image.indirect_root != ImageResource::NoIndirectImage) {
			image.indirect_root = image_remap.indices.at(image.indirect_root);
		}
		for (auto& resource: image.indirect_resources) {
			resource = image_remap.indices.at(resource);
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
