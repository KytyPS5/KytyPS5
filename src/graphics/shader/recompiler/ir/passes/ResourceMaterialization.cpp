#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"

#include "common/assert.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/shader/recompiler/BufferFormat.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/shaderBindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <xxhash.h>

namespace Libs::Graphics::ShaderRecompiler::IR {

std::string& MaterializeFailure();

namespace {

constexpr uint64_t AddressMask             = 0x0000ffffffffffffull;
constexpr uint64_t MaxIndirectImageProbes  = 65536u;
constexpr uint64_t MaxIndirectBufferProbes = 65536u;

// The host-resolved half of a GPU-selected buffer table: every record the selector can name,
// deduped into real descriptors. Main's ResourceKind::IndirectBuffer path handles what this
// cannot match, by decoding the V# in the shader instead.
struct IndirectBuffer {
	uint32_t                     resource = 0;
	std::vector<uint32_t>        keys;
	std::vector<uint32_t>        candidates;
	std::vector<DescriptorValue> descriptors;
	// Derived once per enumeration; empty when a candidate is out of range.
	std::vector<uint32_t> mapping;
	std::vector<uint32_t> children;
};

// Not memoed, so a failing table fails on every draw; print each reason at every doubling.
bool SpecializationFail(std::string_view message) {
	static std::mutex                                mutex;
	static std::unordered_map<std::string, uint64_t> counts;
	uint64_t                                         count = 0;
	{
		const std::lock_guard<std::mutex> lock(mutex);
		count = ++counts[std::string(message)];
	}
	MaterializeFailure() = message;
	if ((count & (count - 1u)) == 0u) {
		std::fprintf(stderr, "shader resource specialization failed: %.*s (occurrence %llu)\n",
		             static_cast<int>(message.size()), message.data(),
		             static_cast<unsigned long long>(count));
	}
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

bool IsImageDimension2D(Decoder::ImageDimension dimension) {
	return dimension == Decoder::ImageDimension::Dim2D ||
	       dimension == Decoder::ImageDimension::Dim2DArray;
}

// Whether an enumerated heap record can be the image an access of this dimension reads. A broad
// heap also holds the views of other typed operations; the 2D family shares one table through
// its shape arms.
bool ServesImageCoordinates(const DescriptorValue& descriptor, Decoder::ImageDimension requested) {
	const auto dimension = DescriptorDimension(descriptor, requested);
	return dimension == requested ||
	       (IsImageDimension2D(dimension) && IsImageDimension2D(requested));
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
	const bool array =
	    type == Prospero::ImageType::kColor1DArray || type == Prospero::ImageType::kColor2DArray ||
	    type == Prospero::ImageType::kColor2DMsaaArray || type == Prospero::ImageType::kCube;
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

enum class SamplerClass : uint8_t { Float, Integer, PointInteger };

SamplerClass ClassifySampler(const ImageResource& image) {
	if (image.numeric_class == Prospero::TextureNumericClass::Sint ||
	    (image.numeric_class == Prospero::TextureNumericClass::Uint &&
	     image.conversion_format != Prospero::BufferFormat::kInvalid)) {
		return SamplerClass::PointInteger;
	}
	return image.numeric_class == Prospero::TextureNumericClass::Uint ? SamplerClass::Integer
	                                                                  : SamplerClass::Float;
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
	std::vector<uint32_t>&                      slots;

	void Record(uint64_t address, uint64_t bytes) {
		ranges.emplace_back(address, bytes);
		slots.push_back(CurrentSrtReadSlot());
	}
};

bool CaptureStrictRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<ReadCapture*>(userdata);
	if (!capture.source.read_specialization_memory(capture.source.userdata, address, values)) {
		return false;
	}
	capture.Record(address, values.size_bytes());
	return true;
}

bool CaptureConditionRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<ReadCapture*>(userdata);
	if (!capture.source.read_condition_memory(capture.source.userdata, address, values)) {
		return false;
	}
	capture.Record(address, values.size_bytes());
	return true;
}

bool CaptureOrdinaryRead(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& capture = *static_cast<ReadCapture*>(userdata);
	if (capture.source.read_memory != nullptr) {
		if (!capture.source.read_memory(capture.source.userdata, address, values)) return false;
	} else {
		std::memcpy(values.data(), reinterpret_cast<const void*>(address), values.size_bytes());
	}
	capture.Record(address, values.size_bytes());
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
	std::vector<Value> pending;
	if (source != nullptr) {
		pending.assign(source->dwords.begin(), source->dwords.begin() + source->dword_count);
	}
	if (!extra.IsEmpty()) pending.push_back(extra);
	std::unordered_set<const Inst*> visited;
	while (!pending.empty()) {
		auto value = pending.back().Resolve();
		pending.pop_back();
		const auto* inst = value.TryInstruction();
		if (inst == nullptr || !visited.insert(inst).second) {
			continue;
		}
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

uint64_t HashBytes(const uint8_t* data, uint64_t size) {
	constexpr uint64_t prime = 0x9e3779b97f4a7c15ull;
	const auto         Mix   = [](uint64_t value) {
		value ^= value >> 33u;
		value *= 0xff51afd7ed558ccdull;
		value ^= value >> 33u;
		value *= 0xc4ceb9fe1a85ec53ull;
		return value ^ (value >> 33u);
	};
	uint64_t hash   = size * prime;
	uint64_t offset = 0;
	for (; offset + sizeof(uint64_t) <= size; offset += sizeof(uint64_t)) {
		uint64_t chunk = 0;
		std::memcpy(&chunk, data + offset, sizeof(chunk));
		hash = Mix(hash ^ chunk) + prime;
	}
	uint64_t tail = 0;
	if (offset < size) {
		std::memcpy(&tail, data + offset, static_cast<size_t>(size - offset));
	}
	return Mix(hash ^ tail);
}

class ObservedKeyStore {
public:
	void Add(uint64_t signature, std::span<const uint32_t> keys) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		auto&                             working = m_keys[signature];
		const auto                        before  = working.size();
		Merge(working, keys);
		// A key the working set did not already name is a key the last materialization's mapping
		// did not carry, so the record that selected it took the switch's default arm on the draw
		// that reported it - candidate 0, the null descriptor. Counting the growth of the working
		// set rather than of the lifetime set is deliberate: after a refusal Forgets the working
		// set, every key really does have to be resolved again, and every one of them reads null
		// again until it is.
	}

	[[nodiscard]] std::vector<uint32_t> Get(uint64_t signature) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		const auto                        found = m_keys.find(signature);
		return found == m_keys.end() ? std::vector<uint32_t> {} : found->second;
	}

	// A table that went resident once stays resident: re-running the enumeration it already failed
	// would spend the whole probe budget again on every draw to reach the same answer.
	void MarkResident(uint64_t signature) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_keys.try_emplace(signature);
	}

	[[nodiscard]] bool IsResident(uint64_t signature) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		return m_keys.contains(signature);
	}

	void Forget(uint64_t signature) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_keys.erase(signature);
	}

private:
	static void Merge(std::vector<uint32_t>& observed, std::span<const uint32_t> keys) {
		observed.insert(observed.end(), keys.begin(), keys.end());
		std::ranges::sort(observed);
		observed.erase(std::ranges::unique(observed).begin(), observed.end());
		if (observed.size() > MaxObservedKeys) {
			observed.resize(MaxObservedKeys);
		}
	}

	// A table that selects more distinct materials than the image limit cannot be served this way
	// at all, so there is no point growing the set past the point where that is already decided.
	static constexpr size_t MaxObservedKeys = 4096;

	std::mutex                                          m_mutex;
	std::unordered_map<uint64_t, std::vector<uint32_t>> m_keys;
};

ObservedKeyStore& ObservedKeys() {
	static ObservedKeyStore store;
	return store;
}

bool ReadScalarTable(uint64_t base, uint64_t size, uint64_t dynamic_offset,
                     const SrtRuntime& runtime, std::span<uint32_t> words) {
	const auto offset = dynamic_offset & ~uint64_t {3};
	const auto count  = std::min<uint64_t>(words.size(), offset < size ? (size - offset) / 4u : 0u);
	std::ranges::fill(words.subspan(count), 0u);
	if (count == 0u) {
		return true;
	}
	base &= AddressMask & ~uint64_t {3};
	if (offset > AddressMask - base) {
		return false;
	}
	const auto address = base + offset;
	const auto prefix  = words.first(count);
	return prefix.size_bytes() - 1u <= AddressMask - address &&
	       runtime.read_specialization_memory != nullptr &&
	       runtime.read_specialization_memory(runtime.userdata, address, prefix);
}

// One word out of a scalar buffer, by the same rule ReadScalarTable applies to a span: a read
// past the descriptor extent yields zero rather than a refusal, because that is what the
// hardware returns for it.
bool ReadScalarBufferWord(const ShaderBufferResource& descriptor, uint32_t dynamic_offset,
                          uint32_t immediate_offset, const SrtRuntime& runtime, uint32_t& word) {
	const auto offset = static_cast<uint64_t>(dynamic_offset) + immediate_offset;
	if (offset > UINT32_MAX) {
		return false;
	}
	word = 0;
	return ReadScalarTable(descriptor.Base48(), descriptor.GetSize(), static_cast<uint32_t>(offset),
	                       runtime, {&word, 1});
}

// The flattened SRT ends with a directory followed by the key mappings the directory points at.
// Two words per image resource - its mapping offset and its feedback bitmap offset - at a base
// fixed by the program own SRT width. Both halves of a slot number are behind the program key
// already, so a slot never moves when a sibling table gains a key; the mapping offsets it holds
// are allocation results and stay out of the specialization. See ResourceMaterialization.h.
constexpr size_t IndirectDirectoryStride = 2u;

size_t IndirectDirectoryBase(const ResourcePlan& program) {
	return program.srt_reads.size();
}

size_t IndirectDirectoryWords(const ResourcePlan& program) {
	// A shader with no indirect table reads no directory, so its flattened SRT stays exactly the
	// words it reads.
	const bool has_table =
	    std::ranges::any_of(program.info.images,
	                        [&](const ImageResource& image) {
		                        const auto* source = Source(program, image.source);
		                        return source != nullptr && source->indirect_descriptor.has_value();
	                        }) ||
	    std::ranges::any_of(program.info.buffers, [&](const BufferResource& buffer) {
		    const auto* source = Source(program, buffer.source);
		    return source != nullptr && source->indirect_buffer.has_value();
	    });
	if (!has_table) {
		return 0;
	}
	// Images first, so an image slot keeps the number it has always had.
	return (program.info.images.size() + program.info.buffers.size()) * IndirectDirectoryStride;
}

size_t IndirectMappingSlot(const ResourcePlan& program, uint32_t image_index) {
	return IndirectDirectoryBase(program) +
	       static_cast<size_t>(image_index) * IndirectDirectoryStride;
}

size_t IndirectBufferMappingSlot(const ResourcePlan& program, uint32_t buffer_index) {
	return IndirectDirectoryBase(program) +
	       (program.info.images.size() + static_cast<size_t>(buffer_index)) *
	           IndirectDirectoryStride;
}

bool BindlessTwoDimensional(Decoder::ImageDimension dimension) {
	return dimension == Decoder::ImageDimension::Dim2D ||
	       dimension == Decoder::ImageDimension::Dim2DArray;
}

bool ServedBindless(const ResourcePlan& program, uint32_t image_index) {
	const auto& image  = program.info.images[image_index];
	const auto* source = Source(program, image.source);
	if (source == nullptr || !source->indirect_descriptor.has_value() ||
	    !source->indirect_descriptor->bindless) {
		return false;
	}
	// The comparison arms are the 2D family's: Vulkan has no depth comparison on a volume, and
	// the heap's 1D views are never compared.
	return image.resource_class == ImageResourceClass::Sampled && !image.written && !image.atomic &&
	       (!image.depth_compare || BindlessTwoDimensional(image.dimension)) && !image.r128 &&
	       BindlessShapeFor(image.dimension).has_value();
}

bool BindlessReads(ValueOpcode opcode) {
	return opcode == ValueOpcode::ImageSampleRaw || opcode == ValueOpcode::ImageRead;
}

// The image operations the bindless switch serves: samples (compared ones included, see
// ServedBindless) and reads of every bindless shape, and gathers of the 2D family at the view's
// own level. A gather at an explicit LOD selects a per-mip view, and the heap keeps one view per
// record.
bool BindlessServesOp(ValueOpcode opcode, const MemoryInfo& memory) {
	if (BindlessReads(opcode)) {
		return true;
	}
	return opcode == ValueOpcode::ImageGatherRaw &&
	       BindlessTwoDimensional(memory.image_dimension) &&
	       (memory.image_sample_flags & Decoder::ImageSampleFlagLod) == 0u;
}
// Why an indirect image table could not be enumerated. Every clause below refuses for a different
// reason with a different fix - a handle the host cannot decode, a probe budget the table outgrows,
// an image budget its materials outgrow - and the caller reported all of them as the same line. The
// numbers travel with the reason because the answer to "which one is it" is usually a comparison of
// two of them.
struct IndirectImageFailure {
	enum class Stage : uint8_t {
		None,
		TableDescriptorUndecodable,
		KeyCountUnevaluable,
		KeyCountOverBudget,
		SelectorMaskUnevaluable,
		MaskProbeOffsetOverflow,
		MaskProbeUnreadable,
		MaterialDescriptorUndecodable,
		MaterialStrideMismatch,
		ProbeCountOverBudget,
		MaterialRecordUnreadable,
		HeapRecordUnreadable,
		ImageBudgetExhausted,
		MappingSlotOutOfRange,
		FiniteCandidateUnevaluable,
	};

	Stage    stage           = Stage::None;
	uint32_t selector_stride = 0;
	uint32_t material_stride = 0;
	uint64_t material_size   = 0;
	uint64_t probe_count     = 0;
	uint64_t probe_budget    = 0;
	// What the enumeration actually steps by, which is not the selector stride: an offset that
	// wraps 32 bits reaches every multiple of its gcd with 2^32, so a 224-byte selector walks the
	// table 32 bytes at a time.
	uint64_t probe_step      = 0;
	uint32_t key_count       = 0;
	uint32_t keys            = 0;
	uint32_t distinct_images = 0;
	uint32_t image_budget    = 0;
	uint32_t key             = 0;
	uint64_t offset          = 0;
	// The single guest word whose refusal decided this failure, when one did. Re-probing just that
	// word costs one read and answers whether the enumeration could now get further, which is what
	// lets the memo below hold a read failure without ever making it permanent.
	uint64_t recheck_base   = 0;
	uint64_t recheck_size   = 0;
	uint32_t recheck_offset = 0;
	bool     has_recheck    = false;
};

// Records the word a read refused on so the memo can re-probe exactly it. Called at the refusal,
// where the arguments that produced it are still in hand.
void RecheckAt(IndirectImageFailure& failure, uint64_t base, uint64_t size, uint64_t offset) {
	if (offset > UINT32_MAX) {
		return;
	}
	failure.recheck_base   = base;
	failure.recheck_size   = size;
	failure.recheck_offset = static_cast<uint32_t>(offset);
	failure.has_recheck    = true;
}

std::string DescribeIndirectImageFailure(const IndirectImageFailure& failure) {
	switch (failure.stage) {
		case IndirectImageFailure::Stage::None: return "no recorded reason";
		case IndirectImageFailure::Stage::TableDescriptorUndecodable:
			return "the heap handle is neither a raw address nor a decodable V#";
		case IndirectImageFailure::Stage::KeyCountUnevaluable:
			return "the key count is not a host-evaluable value";
		case IndirectImageFailure::Stage::KeyCountOverBudget:
			return fmt::format(
			    "key count {} is past the {} probe budget or runs the heap past 4 GiB",
			    failure.key_count, failure.probe_budget);
		case IndirectImageFailure::Stage::SelectorMaskUnevaluable:
			return "the selector mask or its key count is not a host-evaluable value";
		case IndirectImageFailure::Stage::MaskProbeOffsetOverflow:
			return fmt::format("selector offset {} runs past 4 GiB", failure.offset);
		case IndirectImageFailure::Stage::MaskProbeUnreadable:
			return fmt::format("the material word at offset {} would not read back",
			                   failure.offset);
		case IndirectImageFailure::Stage::MaterialDescriptorUndecodable:
			return "the material handle is not a decodable V# beside a V# heap";
		case IndirectImageFailure::Stage::MaterialStrideMismatch:
			// The shader scales its selector by one step and the V# strides by another, so the
			// records the enumeration would walk are not the records the draw selects.
			return fmt::format("material V# strides by {} but the shader scales its selector by {}",
			                   failure.material_stride, failure.selector_stride);
		case IndirectImageFailure::Stage::ProbeCountOverBudget:
			return fmt::format(
			    "{} records ({} bytes of material walked at a {}-byte step, selector stride {}) is "
			    "past the {} probe budget",
			    failure.probe_count, failure.material_size, failure.probe_step,
			    failure.selector_stride, failure.probe_budget);
		case IndirectImageFailure::Stage::MaterialRecordUnreadable:
			return fmt::format("the material record at offset {} would not read back",
			                   failure.offset);
		case IndirectImageFailure::Stage::HeapRecordUnreadable:
			return fmt::format("key {} names heap offset {} which would not read back", failure.key,
			                   failure.offset);
		case IndirectImageFailure::Stage::ImageBudgetExhausted:
			return fmt::format(
			    "{} keys resolve to more than the {} images a shader may bind ({} distinct so far)",
			    failure.keys, failure.image_budget, failure.distinct_images);
		case IndirectImageFailure::Stage::MappingSlotOutOfRange:
			return "the indirect directory slot is past the mapping it points at";
		case IndirectImageFailure::Stage::FiniteCandidateUnevaluable:
			return fmt::format("finite candidate {} is not host-evaluable", failure.key);
	}
	return "unknown reason";
}

// A table that refused once refuses again for as long as its two descriptors stay put: every clause
// above is a function of those descriptors and the words they address, and re-deriving the answer
// costs the whole probe budget - one guest read per material record - on every dispatch. That cost
// is what the earlier bindless attempt died of, so the answer is remembered instead. A draw that
// rebinds either descriptor hashes differently and misses the memo, and a refusal only the guest
// memory decides is re-attempted periodically so a table that is merely not filled in yet still
// converges.
class RefusedIndirectTables {
public:
	// Non-null when this table has already refused and the answer still stands. A refusal a budget
	// or a shape decided stands until the descriptors move, because nothing else feeds it. A
	// refusal a guest read decided stands only while that read still refuses, so it is re-probed
	// here - one word, against the same bounds the enumeration used. The remembered failure comes
	// back with the answer so the caller's reason string stays honest on a memo hit.
	[[nodiscard]] const IndirectImageFailure* Find(uint64_t signature, const SrtRuntime& runtime) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		const auto                        found = m_failures.find(signature);
		if (found == m_failures.end()) {
			return nullptr;
		}
		const auto& failure = found->second;
		if (failure.has_recheck) {
			uint32_t word = 0;
			if (ReadScalarTable(failure.recheck_base, failure.recheck_size, failure.recheck_offset,
			                    runtime, {&word, 1})) {
				m_failures.erase(found);
				return nullptr;
			}
		}
		return &found->second;
	}

	void Remember(uint64_t signature, const IndirectImageFailure& failure) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		// A guest that rebinds its heap every frame would otherwise grow this without bound. The
		// memo only has to outlive a few dispatches to pay for itself, so dropping all of it is
		// cheaper than tracking which entry to evict.
		if (m_failures.size() >= MaxRememberedTables) {
			m_failures.clear();
		}
		m_failures[signature] = failure;
	}

	void Forget(uint64_t signature) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		m_failures.erase(signature);
	}

private:
	static constexpr size_t MaxRememberedTables = 1024;

	std::mutex                                         m_mutex;
	std::unordered_map<uint64_t, IndirectImageFailure> m_failures;
};

RefusedIndirectTables& RefusedTables() {
	static RefusedIndirectTables tables;
	return tables;
}

// Identifies a table by everything the enumeration reads that is not the guest memory behind it:
// the two descriptors as bound for this dispatch, and the plan's own view of how the shader indexes
// them.
uint64_t IndirectTableSignature(const DescriptorSource::IndirectDescriptor& indirect,
                                const DescriptorValue&                      material_value,
                                const DescriptorValue& table_value, uint32_t image_index,
                                uint64_t readable_extent, uint32_t workgroup_keys) {
	struct Key {
		uint32_t image_index;
		uint32_t material_count;
		uint32_t table_count;
		uint32_t selector_stride;
		uint32_t selector_offset;
		uint32_t table_offset;
		uint32_t material_offset;
		uint32_t heap_stride;
		uint32_t record_offset;
		uint32_t key_mask;
		uint32_t indexed_heap;
		uint32_t affine_selector;
		uint32_t selector_shift;
		uint32_t workgroup_axis;
		uint32_t workgroup_keys;
		uint64_t readable_extent;
		uint32_t material_dwords[8];
		uint32_t table_dwords[8];
	} key {};
	key.image_index     = image_index;
	key.material_count  = material_value.dword_count;
	key.table_count     = table_value.dword_count;
	key.selector_stride = indirect.selector_stride;
	key.selector_offset = indirect.selector_offset;
	key.table_offset    = indirect.table_offset;
	key.material_offset = indirect.material_offset;
	key.heap_stride     = indirect.heap_stride;
	key.record_offset   = indirect.record_offset;
	key.key_mask        = indirect.key_mask;
	key.indexed_heap    = indirect.indexed_heap ? 1u : 0u;
	key.affine_selector = indirect.affine_selector ? 1u : 0u;
	key.selector_shift  = indirect.selector_shift;
	key.workgroup_axis  = indirect.workgroup_axis;
	key.workgroup_keys  = workgroup_keys;
	// A refusal only holds for the backing the arena had when it was refused.
	key.readable_extent = readable_extent;
	std::copy(material_value.dwords.begin(), material_value.dwords.end(), key.material_dwords);
	std::copy(table_value.dwords.begin(), table_value.dwords.end(), key.table_dwords);
	return HashBytes(reinterpret_cast<const uint8_t*>(&key), sizeof(key));
}

// Every guest read pays a coherence check, so a table is read in spans of at most this size.
constexpr uint64_t MaxSpanBytes = 64u * 1024u;
// Windows bridge small gaps only, so sparse or short tables are still read record by record.
constexpr uint64_t MaxWindowGapBytes = 256u;
constexpr size_t   MinWindowRecords  = 64u;

using CandidateWords = std::array<uint32_t, 8>;

struct CandidateWordsHash {
	size_t operator()(const CandidateWords& words) const {
		return static_cast<size_t>(XXH3_64bits(words.data(), sizeof(words)));
	}
};

// The result of sort + unique. Material keys are small and heavily repeated, so a bitmap is
// cheaper.
void SortUniqueKeys(std::vector<uint32_t>& keys) {
	const auto largest = keys.empty() ? 0u : *std::ranges::max_element(keys);
	if (keys.size() < 64u || largest / 64u > keys.size()) {
		std::ranges::sort(keys);
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
		return;
	}
	static thread_local std::vector<uint64_t> present;
	present.assign(largest / 64u + 1u, 0u);
	for (const auto key: keys) {
		present[key / 64u] |= uint64_t {1} << (key % 64u);
	}
	keys.clear();
	for (size_t word = 0; word < present.size(); word++) {
		for (auto bits = present[word]; bits != 0u; bits &= bits - 1u) {
			keys.push_back(static_cast<uint32_t>(word * 64u + std::countr_zero(bits)));
		}
	}
}

bool EnumerateIndirectImage(const ResourcePlan&                         program,
                            const DescriptorSource::IndirectDescriptor& indirect,
                            const DescriptorValue&                      material_value,
                            const DescriptorValue& table_value, uint32_t image_index,
                            const SrtRuntime& runtime, SrtWalker& clean, ResourceSnapshot& snapshot,
                            ResourceSpecialization& specialization, IndirectImageFailure& failure) {
	KYTY_PROFILER_FUNCTION();
	const auto Refuse = [&failure](IndirectImageFailure::Stage stage) {
		failure.stage = stage;
		return false;
	};
	uint64_t table_base = 0;
	uint64_t table_size = UINT64_MAX; // Scalar addresses have no buffer descriptor bounds.
	ShaderBufferResource table;
	if (table_value.dword_count == 2u) {
		table_base = (static_cast<uint64_t>(table_value.dwords[1]) << 32u) | table_value.dwords[0];
	} else if (DecodeBufferDescriptor(table_value, table)) {
		table_base = table.Base48();
		table_size = table.GetSize();
	} else {
		return Refuse(IndirectImageFailure::Stage::TableDescriptorUndecodable);
	}
	auto& keys = program.material_keys;
	keys.clear();
	if (indirect.material_source == UINT32_MAX) {
		uint32_t key_count = 0;
		if (indirect.workgroup_axis != UINT32_MAX) {
			if (indirect.workgroup_axis >= runtime.workgroup_counts.size() ||
			    runtime.workgroup_counts[indirect.workgroup_axis] == 0u) {
				return Refuse(IndirectImageFailure::Stage::KeyCountUnevaluable);
			}
			key_count = runtime.workgroup_counts[indirect.workgroup_axis];
		} else {
			const bool evaluated = clean.Evaluate(indirect.key_count, key_count);
			if (std::bit_cast<int32_t>(key_count) <= 0) key_count = 0;
			if (table_value.dword_count != 2u || !evaluated) {
				return Refuse(IndirectImageFailure::Stage::KeyCountUnevaluable);
			}
		}
		failure.key_count    = key_count;
		failure.probe_budget = MaxIndirectImageProbes;
		if (key_count > MaxIndirectImageProbes ||
		    (key_count != 0u && uint64_t {indirect.table_offset} +
		                                uint64_t {key_count - 1u} * indirect.heap_stride + 32u >
		                            UINT32_MAX + 1ull)) {
			return Refuse(IndirectImageFailure::Stage::KeyCountOverBudget);
		}
		keys.resize(key_count);
		std::iota(keys.begin(), keys.end(), 0u);
	} else if (!indirect.selector_first.IsEmpty()) {
		ShaderBufferResource material;
		uint32_t             first = 0;
		uint32_t             count = 0;
		// A scalar read steps by its instruction's byte stride when the shader names one; the V#
		// stride then only bounds the range.
		const bool scalar_stride = indirect.selector_stride != 0u;
		if (!DecodeBufferDescriptor(material_value, material) || material.Type() != 0u ||
		    ((indirect.selector_shift != 0u || indirect.key_mask != UINT32_MAX) &&
		     (material.Base48() & 3u) != 0u) ||
		    material.SwizzleEnabled() || material.AddTid() || material.OutOfBounds() != 0u ||
		    (!scalar_stride && uint64_t {indirect.selector_offset} + 4u > material.Stride()) ||
		    !clean.Evaluate(indirect.selector_first, first) ||
		    !clean.Evaluate(indirect.key_count, count) ||
		    (!scalar_stride && uint64_t {first} + count > material.NumRecords())) {
			return Refuse(IndirectImageFailure::Stage::MaterialDescriptorUndecodable);
		}
		const uint64_t step = scalar_stride ? indirect.selector_stride : material.Stride();
		const uint64_t end =
		    count != 0u ? (uint64_t {first} + count - 1u) * step + indirect.selector_offset + 4u
		                : 0u;
		failure.probe_count  = count;
		failure.probe_budget = MaxIndirectImageProbes;
		if (count > MaxIndirectImageProbes || end > uint64_t {UINT32_MAX} + 1u ||
		    (scalar_stride && end > material.GetSize())) {
			return Refuse(IndirectImageFailure::Stage::ProbeCountOverBudget);
		}
		keys.resize(count);
		if (step == 4u && indirect.selector_offset == 0u) {
			if (!ReadScalarTable(material.Base48(), material.GetSize(), uint64_t {first} * 4u,
			                     runtime, keys)) {
				failure.offset = uint64_t {first} * 4u;
				RecheckAt(failure, material.Base48(), material.GetSize(), failure.offset);
				return Refuse(IndirectImageFailure::Stage::MaterialRecordUnreadable);
			}
		} else {
			for (uint32_t index = 0; index < count; ++index) {
				const auto offset = (uint64_t {first} + index) * step + indirect.selector_offset;
				if (!ReadScalarTable(material.Base48(), material.GetSize(), offset, runtime,
				                     {&keys[index], 1})) {
					failure.offset = offset;
					RecheckAt(failure, material.Base48(), material.GetSize(), offset);
					return Refuse(IndirectImageFailure::Stage::MaterialRecordUnreadable);
				}
			}
		}
		if (indirect.selector_shift != 0u || indirect.key_mask != UINT32_MAX) {
			for (auto& key: keys) {
				key = (key & indirect.key_mask) >> indirect.selector_shift;
			}
			keys.push_back(0u); // An out-of-range material load returns zero.
		}
		SortUniqueKeys(keys);
	} else if (!indirect.selector_mask.IsEmpty()) {
		uint32_t mask  = 0;
		uint32_t count = 0;
		if (material_value.dword_count != 2u || table_value.dword_count != 2u ||
		    !clean.Evaluate(indirect.selector_mask, mask) ||
		    !clean.Evaluate(indirect.key_count, count) || count == 0u || count > 32u) {
			return Refuse(IndirectImageFailure::Stage::SelectorMaskUnevaluable);
		}
		if (count < 32u) mask &= (1u << count) - 1u;
		const auto material_base =
		    (static_cast<uint64_t>(material_value.dwords[1]) << 32u) | material_value.dwords[0];
		keys.reserve(std::popcount(mask));
		while (mask != 0u) {
			const auto index  = std::countr_zero(mask);
			const auto offset = static_cast<uint64_t>(indirect.selector_offset) +
			                    static_cast<uint64_t>(index) * indirect.selector_stride;
			failure.offset    = offset;
			if (offset > UINT32_MAX) {
				return Refuse(IndirectImageFailure::Stage::MaskProbeOffsetOverflow);
			}
			uint32_t key = 0;
			if (!ReadScalarTable(material_base, UINT64_MAX, static_cast<uint32_t>(offset), runtime,
			                     {&key, 1})) {
				RecheckAt(failure, material_base, UINT64_MAX, offset);
				return Refuse(IndirectImageFailure::Stage::MaskProbeUnreadable);
			}
			keys.push_back(key);
			mask &= mask - 1u;
		}
		std::ranges::sort(keys);
		keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
	} else {
		ShaderBufferResource material;
		if (!DecodeBufferDescriptor(material_value, material) || table_value.dword_count != 4u) {
			return Refuse(IndirectImageFailure::Stage::MaterialDescriptorUndecodable);
		}
		failure.selector_stride = indirect.selector_stride;
		failure.material_stride = material.Stride();
		failure.material_size   = material.GetSize();
		if (!indirect.affine_selector && material.Stride() != indirect.selector_stride) {
			return Refuse(IndirectImageFailure::Stage::MaterialStrideMismatch);
		}
		// The first aligned offset includes the immediate added after shader U32 arithmetic.
		const auto step = std::max<uint64_t>(
		    4u, std::gcd<uint64_t>(indirect.selector_stride, uint64_t {1} << 32u));
		const uint64_t first = indirect.selector_offset;
		const auto     size  = material.GetSize();
		auto limit  = std::min(first + (uint64_t {1} << 32u) - step, size >= 4u ? size - 4u : 0u);
		auto backed = size >= 4u;
		if (backed && runtime.readable_extent != nullptr) {
			// Probe no further than the pages actually behind the arena: a probe past them could
			// only have been refused anyway, and a declared size far larger than its backing is
			// what puts an otherwise ordinary table past the budget. Each probe reads a dword at
			// its offset, so stop at the last offset whose whole word is backed.
			const auto readable =
			    runtime.readable_extent(runtime.userdata, material.Base48(), size);
			backed = readable >= sizeof(uint32_t);
			limit  = std::min(limit, backed ? readable - sizeof(uint32_t) : 0u);
		}
		const auto probe_count = backed && first <= limit ? (limit - first) / step + 1u : 0u;
		failure.probe_count    = probe_count;
		failure.probe_step     = step;
		failure.probe_budget   = MaxIndirectImageProbes;
		if (probe_count > MaxIndirectImageProbes) {
			return Refuse(IndirectImageFailure::Stage::ProbeCountOverBudget);
		}
		keys.reserve(static_cast<size_t>(probe_count) + 1u);
		keys.push_back(0u);
		// A refused span is re-read per record to name the refusal; capturing runtimes always are.
		static thread_local std::vector<uint32_t> span_words;
		const uint64_t                            words_per_probe = step / sizeof(uint32_t);
		const uint64_t                            span_probes =
		    program.capture_specialization_reads && step != sizeof(uint32_t)
		        ? 1u
		        : std::max<uint64_t>(1u, MaxSpanBytes / step);
		for (uint64_t probe = 0, offset = first; probe < probe_count;) {
			const auto count   = std::min(span_probes, probe_count - probe);
			bool       spanned = false;
			if (count > 1u) {
				span_words.resize(static_cast<size_t>((count - 1u) * words_per_probe + 1u));
				spanned = ReadScalarTable(material.Base48(), size, offset, runtime, span_words);
			}
			for (uint64_t index = 0; index < count; ++index, ++probe, offset += step) {
				uint32_t key = 0;
				if (spanned) {
					key = span_words[static_cast<size_t>(index * words_per_probe)];
				} else if (!ReadScalarTable(material.Base48(), size, offset, runtime, {&key, 1})) {
					failure.offset = offset;
					RecheckAt(failure, material.Base48(), material.GetSize(), offset);
					return Refuse(IndirectImageFailure::Stage::MaterialRecordUnreadable);
				}
				// The shader narrows the key before indexing the heap, so the enumeration must too.
				keys.push_back((key & indirect.key_mask) >> indirect.selector_shift);
			}
		}
		SortUniqueKeys(keys);
	}

	const auto children_begin = snapshot.images.size();
	const auto mapping_offset = snapshot.flattened_srt.size();
	const auto root_image     = specialization.images[image_index];
	// Ordinal 0 is the switch's default arm - where the emitted search leaves a key it did not
	// match - so it has to name nothing. See the loop below.
	DescriptorValue null_descriptor;
	null_descriptor.dword_count = 8u;
	null_descriptor.dwords.fill(0);
	snapshot.images[image_index] = null_descriptor;
	snapshot.flattened_srt.resize(mapping_offset + 1u + keys.size() * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(keys.size());
	const auto record_at                   = [&](size_t entry) {
		// The shader scales the key in 32-bit arithmetic, so the record offset wraps with it.
		return uint64_t {static_cast<uint32_t>(keys[entry] * indirect.heap_stride)} +
		       indirect.table_offset + indirect.record_offset;
	};
	// Heap records likewise; a record the table bound truncates is never windowed.
	KYTY_PROFILER_BLOCK("EnumerateIndirectImage heap records");
	static thread_local std::vector<uint32_t> window_words;
	constexpr uint64_t                        RecordBytes      = sizeof(CandidateWords);
	uint64_t                                  window_begin     = 0;
	size_t                                    window_end_entry = 0;
	bool                                      window_valid     = false;
	static thread_local std::unordered_map<CandidateWords, uint32_t, CandidateWordsHash> ordinals;
	ordinals.clear();
	bool null_key = false;
	for (uint32_t entry = 0; entry < keys.size(); ++entry) {
		const auto      key = keys[entry];
		DescriptorValue candidate;
		candidate.dword_count   = 8u;
		const auto table_offset = record_at(entry);
		if (entry >= window_end_entry) {
			window_valid     = false;
			window_end_entry = entry + 1u;
			const auto begin = table_offset & ~uint64_t {3};
			if (!program.capture_specialization_reads && begin + RecordBytes <= table_size) {
				auto end  = begin + RecordBytes;
				auto next = window_end_entry;
				for (; next < keys.size(); ++next) {
					const auto next_begin = record_at(next) & ~uint64_t {3};
					const auto next_end   = next_begin + RecordBytes;
					if ((next_begin > end && next_begin - end > MaxWindowGapBytes) ||
					    next_end - begin > MaxSpanBytes || next_end > table_size) {
						break;
					}
					end = std::max(end, next_end);
				}
				if (next - entry >= MinWindowRecords) {
					window_end_entry = next;
					window_begin     = begin;
					window_words.resize(static_cast<size_t>((end - begin) / sizeof(uint32_t)));
					window_valid =
					    ReadScalarTable(table_base, table_size, begin, runtime, window_words);
				}
			}
		}
		if (window_valid) {
			const auto first_word = static_cast<size_t>(
			    ((table_offset & ~uint64_t {3}) - window_begin) / sizeof(uint32_t));
			std::copy_n(window_words.begin() + static_cast<ptrdiff_t>(first_word),
			            candidate.dwords.size(), candidate.dwords.begin());
		} else if (!ReadScalarTable(table_base, table_size, table_offset, runtime,
		                            candidate.dwords)) {
			failure.key    = key;
			failure.offset = table_offset;
			RecheckAt(failure, table_base, table_size, table_offset);
			return Refuse(IndirectImageFailure::Stage::HeapRecordUnreadable);
		}
		if (NullImageDescriptor(candidate) ||
		    !ValidImageDescriptor(candidate, program.info.images[image_index].r128) ||
		    !ServesImageCoordinates(candidate, program.info.images[image_index].dimension)) {
			candidate.dwords.fill(0);
		}
		// A key the host failed to enumerate resolves to ordinal 0: the search starts there and
		// only leaves it on an exact match, and nothing tells the shader it missed - images have
		// no feedback path, only buffers do. Seeding ordinal 0 with the first enumerated record
		// therefore made every unresolved key sample a real texture, silently. It stays null, and
		// every enumerated key gets a child of its own.
		uint32_t ordinal = 0;
		if (candidate != null_descriptor) {
			const auto children = static_cast<uint32_t>(snapshot.images.size() - children_begin);
			const auto [found, inserted] = ordinals.try_emplace(candidate.dwords, children + 1u);
			ordinal                      = found->second;
			if (inserted) {
				if (snapshot.images.size() >= ShaderInfo::MaxImages) {
					failure.keys            = static_cast<uint32_t>(keys.size());
					failure.distinct_images = static_cast<uint32_t>(snapshot.images.size());
					failure.image_budget    = ShaderInfo::MaxImages;
					return Refuse(IndirectImageFailure::Stage::ImageBudgetExhausted);
				}
				snapshot.images.push_back(candidate);
				auto child          = root_image;
				child.indirect_root = image_index;
				specialization.images.push_back(child);
			}
		}
		null_key |= ordinal == 0u;
		snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = key;
		snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
	}
	if (indirect.workgroup_axis != UINT32_MAX && !null_key &&
	    snapshot.images.size() == children_begin + 1u) {
		// Every workgroup reads the same record, so it binds directly.
		snapshot.images[image_index] = snapshot.images.back();
		snapshot.images.pop_back();
		specialization.images.pop_back();
		snapshot.flattened_srt.resize(mapping_offset);
	} else if (snapshot.images.size() == children_begin) {
		snapshot.flattened_srt.resize(mapping_offset);
	} else {
		const auto slot = IndirectMappingSlot(program, image_index);
		if (slot >= mapping_offset) {
			return Refuse(IndirectImageFailure::Stage::MappingSlotOutOfRange);
		}
		snapshot.flattened_srt[slot] = static_cast<uint32_t>(mapping_offset);
		// Filled by WriteIndirectImageSlots once the bindings the ordinals land in are known.
		const auto ordinals = snapshot.images.size() - children_begin + 1u;
		snapshot.flattened_srt.push_back(static_cast<uint32_t>(ordinals));
		snapshot.flattened_srt.resize(snapshot.flattened_srt.size() + ordinals, 0u);
		auto& root                      = specialization.images[image_index];
		root.indirect_root              = image_index;
		root.indirect_mapping_offset    = static_cast<uint32_t>(slot);
		root.indirect_search_iterations = IndirectSearchIterations;
	}
	return true;
}

// Slot words place each ordinal exactly where the emitter assigns its descriptor_index.
bool WriteIndirectImageSlots(const ResourcePlan& program, ResourceSnapshot& snapshot,
                             const ResourceSpecialization& specialization) {
	const auto& images = specialization.images;
	const auto  roots  = program.info.images.size();
	bool        tables = false;
	for (uint32_t index = 0; index < roots && !tables; index++) {
		tables = images[index].indirect_root == index && !images[index].bindless;
	}
	if (!tables) {
		return true;
	}
	std::array<uint32_t, ImageBindingCount>     next {};
	std::array<uint32_t, ShaderInfo::MaxImages> slots {};
	for (uint32_t index = 0; index < images.size(); index++) {
		const auto& image = images[index];
		if (image.fmask) {
			continue;
		}
		const auto&   base = program.info.images[index < roots ? index : image.indirect_root];
		ImageResource placed;
		placed.resource_class = base.resource_class;
		placed.atomic         = base.atomic;
		placed.atomic64       = base.atomic64;
		placed.depth_compare  = base.depth_compare;
		placed.numeric_class  = image.numeric_class;
		placed.dimension      = image.dimension;
		const auto kind       = DescriptorBindingForImage(placed);
		if (!kind.has_value()) {
			return SpecializationFail(fmt::format("image resource {} has no binding", index));
		}
		auto& element = next[ImageBindingIndex(*kind)];
		slots[index]  = IndirectImageSlot(IndirectImageShape(image.dimension, image.cube), element);
		element += base.mip_mode == ImageMipMode::Dynamic ? image.mip_count : 1u;
	}
	for (uint32_t root = 0; root < roots; root++) {
		if (images[root].indirect_root != root || images[root].bindless) {
			continue;
		}
		const auto mapping =
		    static_cast<size_t>(snapshot.flattened_srt[images[root].indirect_mapping_offset]);
		const auto region =
		    mapping + 1u + static_cast<size_t>(snapshot.flattened_srt[mapping]) * 2u;
		const auto words =
		    region < snapshot.flattened_srt.size() ? snapshot.flattened_srt[region] : 0u;
		if (words == 0u || region + 1u + words > snapshot.flattened_srt.size()) {
			return SpecializationFail("indirect image specialization has no slot words");
		}
		snapshot.flattened_srt[region + 1u] = slots[root];
		uint32_t ordinal                    = 1;
		for (uint32_t index = static_cast<uint32_t>(roots);
		     index < images.size() && ordinal < words; index++) {
			if (images[index].indirect_root == root) {
				snapshot.flattened_srt[region + 1u + ordinal++] = slots[index];
			}
		}
	}
	return true;
}

// A finite table's key is the index of the candidate source the shader selected, so every key is
// enumerated and ordinal 0 can name the first candidate.
bool MaterializeFiniteImage(const ResourcePlan&                         program,
                            const DescriptorSource::IndirectDescriptor& indirect,
                            uint32_t image_index, SrtWalker& clean, ResourceSnapshot& snapshot,
                            ResourceSpecialization& specialization, IndirectImageFailure& failure) {
	const auto Refuse = [&failure](IndirectImageFailure::Stage stage) {
		failure.stage = stage;
		return false;
	};
	const auto& sources        = indirect.sources;
	const auto  children_begin = snapshot.images.size();
	const auto  mapping_offset = snapshot.flattened_srt.size();
	const auto  root_image     = specialization.images[image_index];
	const auto  key_count      = sources.size();
	snapshot.flattened_srt.resize(mapping_offset + 1u + key_count * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(key_count);
	for (uint32_t entry = 0; entry < key_count; ++entry) {
		DescriptorValue candidate;
		candidate.dword_count = 8u;
		if (!clean.EvaluateDescriptor(sources[entry], candidate)) {
			failure.key = entry;
			return Refuse(IndirectImageFailure::Stage::FiniteCandidateUnevaluable);
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
					failure.keys            = static_cast<uint32_t>(key_count);
					failure.distinct_images = static_cast<uint32_t>(snapshot.images.size());
					failure.image_budget    = ShaderInfo::MaxImages;
					return Refuse(IndirectImageFailure::Stage::ImageBudgetExhausted);
				}
				snapshot.images.push_back(candidate);
				auto child          = root_image;
				child.indirect_root = image_index;
				specialization.images.push_back(child);
			}
		}
		snapshot.flattened_srt[mapping_offset + 1u + entry * 2u] = entry;
		snapshot.flattened_srt[mapping_offset + 2u + entry * 2u] = ordinal;
	}
	if (snapshot.images.size() == children_begin) {
		snapshot.flattened_srt.resize(mapping_offset);
	} else {
		const auto slot = IndirectMappingSlot(program, image_index);
		if (slot >= mapping_offset) {
			return Refuse(IndirectImageFailure::Stage::MappingSlotOutOfRange);
		}
		snapshot.flattened_srt[slot] = static_cast<uint32_t>(mapping_offset);
		// Filled by WriteIndirectImageSlots once the bindings the ordinals land in are known.
		const auto ordinals = snapshot.images.size() - children_begin + 1u;
		snapshot.flattened_srt.push_back(static_cast<uint32_t>(ordinals));
		snapshot.flattened_srt.resize(snapshot.flattened_srt.size() + ordinals, 0u);
		auto& root                      = specialization.images[image_index];
		root.indirect_root              = image_index;
		root.indirect_mapping_offset    = static_cast<uint32_t>(slot);
		root.indirect_search_iterations = IndirectSearchIterations;
	}
	return true;
}

// The enumeration above is the expensive half of a dispatch that uses a descriptor heap: one guest
// read per material record, and a table of any size costs that on every dispatch that reaches it.
// A dispatch that goes on to succeed has to pay it - the words behind the descriptors are the
// answer and they move - but a dispatch that refuses pays it to reach a conclusion the last one
// already reached. So refusals, and only refusals, are memoed against the descriptors that produced
// them.
bool MaterializeIndirectImage(const ResourcePlan&                         program,
                              const DescriptorSource::IndirectDescriptor& indirect,
                              const DescriptorValue&                      material_value,
                              const DescriptorValue& table_value, uint32_t image_index,
                              const SrtRuntime& runtime, SrtWalker& clean,
                              ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                              IndirectImageFailure& failure) {
	KYTY_PROFILER_FUNCTION();
	if (!indirect.sources.empty()) {
		return MaterializeFiniteImage(program, indirect, image_index, clean, snapshot,
		                              specialization, failure);
	}
	ShaderBufferResource signature_material;
	const auto           signature_extent =
	    runtime.readable_extent != nullptr &&
	            DecodeBufferDescriptor(material_value, signature_material)
	        ? runtime.readable_extent(runtime.userdata, signature_material.Base48(),
	                                  signature_material.GetSize())
	        : 0u;
	const auto workgroup_keys = indirect.workgroup_axis < runtime.workgroup_counts.size()
	                                ? runtime.workgroup_counts[indirect.workgroup_axis]
	                                : 0u;
	const auto signature = IndirectTableSignature(indirect, material_value, table_value,
	                                              image_index, signature_extent, workgroup_keys);
	if (const auto* remembered = RefusedTables().Find(signature, runtime)) {
		failure = *remembered;
		return false;
	}
	if (!EnumerateIndirectImage(program, indirect, material_value, table_value, image_index,
	                            runtime, clean, snapshot, specialization, failure)) {
		RefusedTables().Remember(signature, failure);
		return false;
	}
	// A table that resolves now must not be held to an answer an earlier binding of it gave.
	RefusedTables().Forget(signature);
	return true;
}

// These numbers decide whether a runtime descriptor selection is needed at all, so they are
// reported once per shader whether the table is accepted or refused.
template <typename... Args>
void ReportIndirectBuffer(uint64_t shader_hash, fmt::format_string<Args...> format,
                          Args&&... args) {
	static std::mutex                   mutex;
	static std::unordered_set<uint64_t> reported;
	{
		const std::lock_guard<std::mutex> lock(mutex);
		if (!reported.insert(shader_hash).second) {
			return;
		}
	}
	const auto message = fmt::format(format, std::forward<Args>(args)...);
	std::printf("shader 0x%016llx indirect buffer table: %s\n",
	            static_cast<unsigned long long>(shader_hash), message.c_str());
	std::fflush(stdout);
}

bool ValidBufferDescriptor(const DescriptorValue& value) {
	ShaderBufferResource descriptor;
	if (!DecodeBufferDescriptor(value, descriptor)) {
		return false;
	}
	// Type 0 is the only buffer V#; a table slot with no base or no records selects nothing.
	return descriptor.Type() == 0u && descriptor.Base48() != 0u && descriptor.NumRecords() != 0u;
}

// Enumerates every record of a descriptor table the shader indexes with a wave-uniform selector
// the host cannot re-execute. Records are probed only at their own stride: a window that straddles
// two records decodes as a plausible descriptor often enough to exhaust the candidate budget.
uint64_t IndirectBufferSignature(const DescriptorSource::IndirectBuffer& indirect,
                                 const DescriptorValue&                  heap_value) {
	struct Key {
		uint32_t heap_count;
		uint32_t selector_stride;
		uint32_t selector_offset;
		uint32_t record_offset;
		uint32_t heap_dwords[8];
	} key {};
	key.heap_count      = heap_value.dword_count;
	key.selector_stride = indirect.selector_stride;
	key.selector_offset = indirect.selector_offset;
	key.record_offset   = indirect.record_offset;
	std::copy(heap_value.dwords.begin(), heap_value.dwords.end(), key.heap_dwords);
	return HashBytes(reinterpret_cast<const uint8_t*>(&key), sizeof(key));
}

// Remembered between dispatches, and returned only while the table's bytes still hash the same.
class EnumeratedIndirectBuffers {
public:
	static EnumeratedIndirectBuffers& Instance() {
		static EnumeratedIndirectBuffers store;
		return store;
	}

	struct Lookup {
		bool                                  found = false;
		std::shared_ptr<const IndirectBuffer> result;
		std::shared_ptr<const std::string>    refusal;
	};

	[[nodiscard]] Lookup Find(uint64_t signature, uint64_t content) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		const auto                        found = m_tables.find(signature);
		if (found == m_tables.end() || found->second.content != content) {
			return {};
		}
		return {true, found->second.result, found->second.refusal};
	}

	void Remember(uint64_t signature, uint64_t content,
	              std::shared_ptr<const IndirectBuffer> result) {
		Store(signature, {content, std::move(result), nullptr});
	}

	void RememberRefusal(uint64_t signature, uint64_t content, std::string reason) {
		static const bool off = [] {
			const char* text = std::getenv("KYTY_NO_REFUSAL_MEMO");
			return text != nullptr && std::strcmp(text, "0") != 0;
		}();
		if (off) {
			return;
		}
		Store(signature,
		      {content, nullptr, std::make_shared<const std::string>(std::move(reason))});
	}

private:
	// Shared so a hit does not copy tens of thousands of keys.
	struct Entry {
		uint64_t                              content = 0;
		std::shared_ptr<const IndirectBuffer> result;
		std::shared_ptr<const std::string>    refusal;
	};

	void Store(uint64_t signature, Entry entry) {
		const std::lock_guard<std::mutex> lock(m_mutex);
		// Bounded the same way the image memo is.
		if (m_tables.size() >= MaxRememberedTables) {
			m_tables.clear();
		}
		m_tables[signature] = std::move(entry);
	}
	static constexpr size_t MaxRememberedTables = 256;

	std::mutex                          m_mutex;
	std::unordered_map<uint64_t, Entry> m_tables;
};

// A null record maps to ordinal 0; any other candidate takes the next ordinal when first named.
void DeriveIndirectBufferMapping(IndirectBuffer& table) {
	table.mapping.clear();
	table.children.clear();
	if (table.candidates.size() != table.keys.size()) {
		return;
	}
	DescriptorValue null_descriptor;
	null_descriptor.dword_count = 4u;
	std::vector<uint32_t> ordinal_of(table.descriptors.size(), 0u);
	table.mapping.resize(table.keys.size() * 2u);
	for (size_t entry = 0; entry < table.keys.size(); entry++) {
		const auto candidate = table.candidates[entry];
		if (candidate >= table.descriptors.size()) {
			table.mapping.clear();
			table.children.clear();
			return;
		}
		uint32_t ordinal = 0;
		if (table.descriptors[candidate] != null_descriptor) {
			if (ordinal_of[candidate] == 0u) {
				table.children.push_back(candidate);
				ordinal_of[candidate] = static_cast<uint32_t>(table.children.size());
			}
			ordinal = ordinal_of[candidate];
		}
		table.mapping[entry * 2u]      = table.keys[entry];
		table.mapping[entry * 2u + 1u] = ordinal;
	}
}

bool MaterializeIndirectBuffer(const DescriptorSource::IndirectBuffer& indirect,
                               const DescriptorValue& heap_value, const SrtRuntime& runtime,
                               uint64_t shader_hash, std::shared_ptr<const IndirectBuffer>& result,
                               bool& refused) {
	KYTY_PROFILER_FUNCTION();
	refused = false;
	ShaderBufferResource heap;
	if (!DecodeBufferDescriptor(heap_value, heap) || indirect.selector_stride == 0u) {
		return false;
	}
	const auto declared = static_cast<uint32_t>(heap.Stride());
	const auto records  = heap.NumRecords();
	if (declared != indirect.selector_stride && declared != 0u) {
		ReportIndirectBuffer(shader_hash,
		                     "refused: table stride {} does not match the selector stride {} "
		                     "(records {}, record offset {})",
		                     declared, indirect.selector_stride, records, indirect.record_offset);
		refused = true;
		return false;
	}
	const auto size        = heap.GetSize();
	const auto probe_count = size / indirect.selector_stride;
	if (probe_count > MaxIndirectBufferProbes) {
		ReportIndirectBuffer(shader_hash,
		                     "refused: {} probes exceed the {} probe budget (table stride {}, "
		                     "selector stride {}, records {}, record offset {})",
		                     probe_count, MaxIndirectBufferProbes, declared,
		                     indirect.selector_stride, records, indirect.record_offset);
		refused = true;
		return false;
	}

	const auto word_count = static_cast<size_t>(size / sizeof(uint32_t));
	const auto signature  = IndirectBufferSignature(indirect, heap_value);
	const auto Remembered = [&](uint64_t content, bool& accepted) {
		auto cached = EnumeratedIndirectBuffers::Instance().Find(signature, content);
		if (!cached.found) {
			return false;
		}
		accepted = cached.refusal == nullptr;
		refused  = !accepted;
		if (!accepted && !cached.refusal->empty()) {
			ReportIndirectBuffer(shader_hash, "{}", *cached.refusal);
		}
		result = std::move(cached.result);
		return true;
	};
	if (word_count != 0 && runtime.hash_specialization_block != nullptr) {
		const auto address = heap.Base48() & AddressMask & ~uint64_t {3};
		const auto bytes   = static_cast<uint64_t>(word_count) * sizeof(uint32_t);
		uint64_t   content = 0;
		bool       hashed  = false;
		if (bytes - 1u <= AddressMask - address) {
			hashed = runtime.hash_specialization_block(runtime.userdata, address, bytes, &content);
		}
		if (bool accepted = false; hashed && Remembered(content, accepted)) {
			return accepted;
		}
	}

	// One read of the whole table, not four per record. A successful read writes every word.
	static thread_local std::vector<uint32_t> table_words;
	table_words.resize(word_count);
	{
		KYTY_PROFILER_BLOCK("MaterializeIndirectBuffer read");
		if (word_count != 0 && !ReadScalarTable(heap.Base48(), size, 0u, runtime, table_words)) {
			return false;
		}
	}
	// Validated against the bytes the walk would have read, so a rewritten table is never stale.
	uint64_t content = 0;
	{
		KYTY_PROFILER_BLOCK("MaterializeIndirectBuffer hash");
		content = XXH3_64bits(table_words.data(), table_words.size() * sizeof(uint32_t));
	}
	if (bool accepted = false; Remembered(content, accepted)) {
		return accepted;
	}
	const auto word_at = [&](uint64_t byte_offset, uint32_t& word) {
		const auto index = byte_offset / sizeof(uint32_t);
		if ((byte_offset & 3u) != 0u || index >= table_words.size()) {
			word = 0u;
			return byte_offset <= size;
		}
		word = table_words[static_cast<size_t>(index)];
		return true;
	};

	IndirectBuffer next;
	next.keys.reserve(static_cast<size_t>(probe_count));
	next.candidates.reserve(static_cast<size_t>(probe_count));
	// The linear find this replaces was quadratic in a table whose records mostly repeat.
	std::map<std::array<uint32_t, 4>, uint32_t> seen;
	for (uint64_t record = 0; record < probe_count; record++) {
		const auto dynamic =
		    record * indirect.selector_stride + static_cast<uint64_t>(indirect.selector_offset);
		if (dynamic > UINT32_MAX) {
			break;
		}
		DescriptorValue candidate;
		candidate.dword_count = 4u;
		for (uint32_t dword = 0; dword < candidate.dword_count; dword++) {
			if (!word_at(dynamic + indirect.record_offset + dword * sizeof(uint32_t),
			             candidate.dwords[dword])) {
				EnumeratedIndirectBuffers::Instance().RememberRefusal(signature, content, {});
				refused = true;
				return false;
			}
		}
		if (!ValidBufferDescriptor(candidate)) {
			candidate.dwords.fill(0);
		}
		next.keys.push_back(static_cast<uint32_t>(record));
		const std::array<uint32_t, 4> words {candidate.dwords[0], candidate.dwords[1],
		                                     candidate.dwords[2], candidate.dwords[3]};
		const auto [entry, inserted] =
		    seen.try_emplace(words, static_cast<uint32_t>(next.descriptors.size()));
		if (inserted) {
			// MaxBuffers is how many buffers a shader may track; the candidates a table expands
			// into are budgeted by MaxDenseBuffers, as the image twin of this loop uses MaxImages.
			if (next.descriptors.size() >= ShaderInfo::MaxDenseBuffers) {
				auto reason = fmt::format(
				    "refused: distinct descriptors exceed the {} buffer limit over {} probes "
				    "(table stride {}, selector stride {}, records {}, record offset {})",
				    ShaderInfo::MaxDenseBuffers, probe_count, declared, indirect.selector_stride,
				    records, indirect.record_offset);
				ReportIndirectBuffer(shader_hash, "{}", reason);
				EnumeratedIndirectBuffers::Instance().RememberRefusal(signature, content,
				                                                      std::move(reason));
				refused = true;
				return false;
			}
			next.descriptors.push_back(candidate);
		}
		next.candidates.push_back(entry->second);
	}
	if (next.descriptors.empty()) {
		// An empty table still has to name one descriptor for the binding the shader declares.
		DescriptorValue null_descriptor;
		null_descriptor.dword_count = 4u;
		next.keys.push_back(0u);
		next.descriptors.push_back(null_descriptor);
		next.candidates.push_back(0u);
	}
	ReportIndirectBuffer(shader_hash,
	                     "probes {}, distinct descriptors {}, table stride {}, selector stride {}, "
	                     "records {}, record offset {}",
	                     probe_count, next.descriptors.size(), declared, indirect.selector_stride,
	                     records, indirect.record_offset);
	DeriveIndirectBufferMapping(next);
	result = std::make_shared<const IndirectBuffer>(std::move(next));
	EnumeratedIndirectBuffers::Instance().Remember(signature, content, result);
	return true;
}

// Binds every record the table can select and writes the key -> ordinal mapping the search reads.
// Layout is the indirect image table's: a directory slot, the key count, then key/ordinal pairs.
bool ExpandIndirectBuffer(const ResourcePlan& program, uint32_t root_index,
                          const IndirectBuffer& resolved, ResourceSnapshot& snapshot,
                          ResourceSpecialization& specialization) {
	KYTY_PROFILER_FUNCTION();
	const auto keys = resolved.keys.size();
	if (keys == 0 || resolved.candidates.size() != keys ||
	    root_index >= specialization.buffers.size() || root_index >= snapshot.buffers.size()) {
		return false;
	}
	DescriptorValue null_descriptor;
	null_descriptor.dword_count = 4u;
	null_descriptor.dwords.fill(0);

	const auto slot           = IndirectBufferMappingSlot(program, root_index);
	const auto directory_end  = IndirectDirectoryBase(program) + IndirectDirectoryWords(program);
	auto       mapping_offset = snapshot.flattened_srt.size();
	if (mapping_offset < directory_end) {
		mapping_offset = directory_end;
	}
	if (slot >= mapping_offset) {
		return false;
	}
	const auto children_begin = snapshot.buffers.size();
	if (children_begin + resolved.descriptors.size() > ShaderInfo::MaxDenseBuffers) {
		return false;
	}
	if (resolved.mapping.size() != keys * 2u) {
		return false;
	}
	snapshot.flattened_srt.resize(mapping_offset + 1u + keys * 2u);
	snapshot.flattened_srt[mapping_offset] = static_cast<uint32_t>(keys);
	// Ordinal 0 is the default arm, so the root names nothing rather than the first enumerated
	// record.
	snapshot.buffers[root_index] = null_descriptor;
	std::ranges::copy(resolved.mapping,
	                  snapshot.flattened_srt.begin() + static_cast<ptrdiff_t>(mapping_offset + 1u));
	for (const auto candidate: resolved.children) {
		snapshot.buffers.push_back(resolved.descriptors[candidate]);
		auto child                       = specialization.buffers[root_index];
		child.indirect_root              = root_index;
		child.indirect_mapping_offset    = 0;
		child.indirect_search_iterations = 0;
		specialization.buffers.push_back(child);
	}
	if (snapshot.buffers.size() == children_begin) {
		// Every record was null: nothing to select between, and the root already reads nothing.
		snapshot.flattened_srt.resize(mapping_offset);
		return true;
	}
	snapshot.flattened_srt[slot]    = static_cast<uint32_t>(mapping_offset);
	auto& root                      = specialization.buffers[root_index];
	root.indirect_root              = root_index;
	root.indirect_mapping_offset    = static_cast<uint32_t>(slot);
	root.indirect_search_iterations = std::bit_width(keys);
	return true;
}

// Powers of two first; quarter-octave steps when the tables cannot all afford them.
size_t IndirectImageBucket(size_t count, bool fine) {
	if (count < 2u) {
		return count;
	}
	if (!fine) {
		return std::bit_ceil(count);
	}
	const auto step = std::max<size_t>(1u, std::bit_floor(count) / 4u);
	return (count + step - 1u) / step * step;
}

// A finite table's candidates are the shader's own, so its module has nothing to stabilise.
bool FiniteImageTable(const ResourcePlan& program, uint32_t root) {
	const auto* source = Source(program, program.info.images[root].source);
	return source != nullptr && source->indirect_descriptor.has_value() &&
	       !source->indirect_descriptor->sources.empty();
}

using ShapeList = std::array<std::pair<Decoder::ImageDimension, bool>, 3>;

// The shapes a 2D-family table's sample can meet that none of its candidates has.
uint32_t MissingShapes(const ResourcePlan& program, const ResourceSnapshot& snapshot, uint32_t root,
                       size_t begin, size_t end, ShapeList& missing) {
	using Decoder::ImageDimension;
	const auto requested = program.info.images[root].dimension;
	if (requested != ImageDimension::Dim2D && requested != ImageDimension::Dim2DArray) {
		return 0;
	}
	std::array<bool, 3> present {};
	for (auto index = begin; index < end; index++) {
		const auto& descriptor = snapshot.images[index];
		if (NullImageDescriptor(descriptor)) {
			continue;
		}
		const auto dimension = DescriptorDimension(descriptor, requested);
		if (DescriptorIsCube(descriptor)) {
			present[2] = true;
		} else if (dimension == ImageDimension::Dim2D || dimension == ImageDimension::Dim2DArray) {
			present[dimension == ImageDimension::Dim2D ? 0u : 1u] = true;
		} else {
			return 0;
		}
	}
	// Only an array request can meet a plain 2D array; a cube can meet either.
	constexpr ShapeList shapes {{{ImageDimension::Dim2D, false},
	                             {ImageDimension::Dim2DArray, false},
	                             {ImageDimension::Dim2DArray, true}}};
	uint32_t            count = 0;
	for (uint32_t shape = 0; shape < shapes.size(); shape++) {
		if (!present[shape] && (shape != 1u || requested == ImageDimension::Dim2DArray)) {
			missing[count++] = shapes[shape];
		}
	}
	return count;
}

// Ordered by kind and null-padded to a bucket so the module survives heap streaming.
void CanonicalizeIndirectImageTables(const ResourcePlan& program, ResourceSnapshot& snapshot,
                                     ResourceSpecialization& specialization) {
	struct Table {
		uint32_t root;
		size_t   begin;
		size_t   end;
	};
	std::array<Table, ShaderInfo::MaxImages> tables;
	size_t                                   table_count = 0;
	const auto                               roots       = program.info.images.size();
	for (size_t index = roots; index < specialization.images.size(); index++) {
		const auto root = specialization.images[index].indirect_root;
		if (root < roots && FiniteImageTable(program, root)) {
			continue;
		}
		if (table_count != 0u && tables[table_count - 1u].root == root &&
		    tables[table_count - 1u].end == index) {
			tables[table_count - 1u].end = index + 1u;
		} else if (root < roots && table_count < tables.size()) {
			tables[table_count++] = {root, index, index + 1u};
		} else {
			return;
		}
	}
	const auto Kind = [&](uint32_t root, size_t index) {
		const auto& descriptor = snapshot.images[index];
		return (static_cast<uint32_t>(
		            DescriptorDimension(descriptor, program.info.images[root].dimension))
		        << 1u) |
		       (DescriptorIsCube(descriptor) ? 1u : 0u);
	};
	for (size_t t = 0; t < table_count; t++) {
		const auto& table = tables[t];
		const auto  count = table.end - table.begin;
		const auto  first = Kind(table.root, table.begin);
		bool        mixed = false;
		for (size_t index = table.begin + 1u; index < table.end && !mixed; index++) {
			mixed = Kind(table.root, index) != first;
		}
		if (!mixed) {
			continue;
		}
		// Stable, so the first-enumerated candidate of each kind still leads it.
		static thread_local std::vector<uint32_t>        order;
		static thread_local std::vector<uint32_t>        ordinal_of;
		static thread_local std::vector<DescriptorValue> images;
		order.resize(count);
		std::iota(order.begin(), order.end(), 0u);
		std::ranges::stable_sort(
		    order, {}, [&](uint32_t child) { return Kind(table.root, table.begin + child); });
		ordinal_of.assign(count + 1u, 0u);
		images.assign(snapshot.images.begin() + static_cast<ptrdiff_t>(table.begin),
		              snapshot.images.begin() + static_cast<ptrdiff_t>(table.end));
		for (uint32_t position = 0; position < count; position++) {
			snapshot.images[table.begin + position] = images[order[position]];
			ordinal_of[order[position] + 1u]        = position + 1u;
		}
		// Children carry identical specializations until BuildResourceSpecialization reads them.
		const auto slot    = IndirectMappingSlot(program, table.root);
		const auto mapping = snapshot.flattened_srt[slot];
		const auto keys    = snapshot.flattened_srt[mapping];
		for (uint32_t entry = 0; entry < keys; entry++) {
			auto& ordinal = snapshot.flattened_srt[mapping + 2u + entry * 2u];
			ordinal       = ordinal_of[ordinal];
		}
	}
	// The buckets only spare permutations; the shape arms keep the module, so they go first.
	size_t reserved = 0;
	for (size_t t = 0; t < table_count; t++) {
		ShapeList missing;
		reserved += MissingShapes(program, snapshot, tables[t].root, tables[t].begin, tables[t].end,
		                          missing);
	}
	for (const bool fine: {false, true}) {
		size_t padding = 0;
		for (size_t t = 0; t < table_count; t++) {
			const auto count = tables[t].end - tables[t].begin;
			padding += IndirectImageBucket(count, fine) - count;
		}
		if (padding == 0u) {
			return;
		}
		if (snapshot.images.size() + padding + reserved > ShaderInfo::MaxImages) {
			continue;
		}
		DescriptorValue null_descriptor;
		null_descriptor.dword_count = 8u;
		null_descriptor.dwords.fill(0);
		// Back to front, so the tables not yet padded keep their indices.
		for (size_t t = table_count; t-- > 0u;) {
			const auto& table = tables[t];
			const auto  count = table.end - table.begin;
			const auto  pad   = IndirectImageBucket(count, fine) - count;
			const auto  child = specialization.images[table.begin];
			snapshot.images.insert(snapshot.images.begin() + static_cast<ptrdiff_t>(table.end), pad,
			                       null_descriptor);
			specialization.images.insert(
			    specialization.images.begin() + static_cast<ptrdiff_t>(table.end), pad, child);
		}
		return;
	}
}

// Null candidates for shapes the heap lacks yet, so streaming never reaches the module.
void PadIndirectImageShapes(const ResourcePlan& program, ResourceSnapshot& snapshot,
                            ResourceSpecialization& specialization) {
	const auto roots = program.info.images.size();
	if (specialization.images.size() <= roots) {
		return;
	}
	struct Table {
		size_t    end;
		ShapeList shapes;
		uint32_t  count;
	};
	std::array<Table, ShaderInfo::MaxImages> tables;
	size_t                                   table_count = 0;
	size_t                                   added       = 0;
	for (size_t index = roots; index < specialization.images.size();) {
		const auto root = specialization.images[index].indirect_root;
		if (root >= roots) {
			return;
		}
		auto end = index;
		while (end < specialization.images.size() &&
		       specialization.images[end].indirect_root == root) {
			end++;
		}
		auto& table = tables[table_count++];
		table.end   = end;
		table.count = FiniteImageTable(program, root)
		                  ? 0u
		                  : MissingShapes(program, snapshot, root, index, end, table.shapes);
		added += table.count;
		index = end;
	}
	if (added == 0u || snapshot.images.size() + added > ShaderInfo::MaxImages) {
		return;
	}
	DescriptorValue null_descriptor;
	null_descriptor.dword_count = 8u;
	null_descriptor.dwords.fill(0);
	// Back to front, so the tables not yet padded keep their indices.
	for (size_t t = table_count; t-- > 0u;) {
		const auto& table = tables[t];
		for (uint32_t shape = 0; shape < table.count; shape++) {
			auto child          = specialization.images[table.end - 1u];
			child.dimension     = table.shapes[shape].first;
			child.cube          = table.shapes[shape].second;
			child.shape_padding = true;
			const auto at       = static_cast<ptrdiff_t>(table.end + shape);
			snapshot.images.insert(snapshot.images.begin() + at, null_descriptor);
			specialization.images.insert(specialization.images.begin() + at, child);
		}
	}
}

} // namespace

struct SamplerPlan {
	struct Binding {
		uint32_t     source;
		SamplerClass type;
	};
	std::array<std::array<uint32_t, 3>, ShaderInfo::MaxSamplers> mapping;
	std::array<Binding, ShaderInfo::MaxSamplers>                 bindings;
	uint32_t                                                     sampler_count = 0;
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
	uint32_t              source_count;
	uint32_t              count = 0;
};

static bool BuildResourceSpecialization(const ResourcePlan& program, ResourceSnapshot& snapshot,
                                        ResourceSpecialization& specialization) {
	KYTY_PROFILER_FUNCTION();
	// In place over the snapshot: an expanded table has already appended a child per record here.
	specialization.buffers.resize(snapshot.buffers.size());
	specialization.unbound_buffers.assign(snapshot.buffers.size(), false);
	specialization.unbound_images.assign(specialization.images.size(), false);
	for (uint32_t i = 0; i < snapshot.buffers.size(); i++) {
		const auto describes =
		    i < program.info.buffers.size() ? i : specialization.buffers[i].indirect_root;
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
		// A child answers for the same access as the root, so only the descriptor words differ.
		const bool known     = describes < program.info.buffers.size();
		const bool formatted = known && program.info.buffers[describes].formatted;
		if (known && program.info.buffers[describes].unindexed &&
		    (packed_stride & ((1u << 14u) | (1u << 20u))) == 0u) {
			packed_stride = 0;
		}
		const bool unbound                = descriptor.Base48() == 0u || descriptor.GetSize() == 0u;
		specialization.unbound_buffers[i] = unbound;
		auto& entry                       = specialization.buffers[i];
		entry.packed_stride               = packed_stride;
		entry.descriptor_format =
		    formatted ? descriptor.Format() : Prospero::BufferFormat::kInvalid;
		entry.descriptor_swizzle = formatted ? descriptor.DstSelXYZW() : DstSel(4, 5, 6, 7);
		entry.zero_stride_oob =
		    descriptor.OutOfBounds() == 0u && stride == 0u && !(unbound && !formatted);
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
		if (image.bindless) {
			image.numeric_class     = Prospero::TextureNumericClass::Float;
			image.dimension         = base.dimension;
			image.cube              = base.cube;
			image.mip_count         = 1;
			image.conversion_format = Prospero::BufferFormat::kInvalid;
			image.shader_swizzle    = ShaderImageIdentitySwizzle;
			continue;
		}
		image.mip_count = ImageMipCount(base, descriptor);
		if (image.mip_count == 0u) {
			return SpecializationFail(
			    fmt::format("image descriptor {} has an invalid mip range", i));
		}
		if (NullImageDescriptor(descriptor)) {
			image.numeric_class = base.atomic ? Prospero::TextureNumericClass::Uint
			                                  : Prospero::TextureNumericClass::Float;
			if (!image.shape_padding) {
				image.dimension = Decoder::ImageDimension::Dim2D;
				image.cube      = false;
			}
			specialization.unbound_images[i] =
			    !image.shape_padding && image.indirect_root == ImageResource::NoIndirectImage &&
			    base.resource_class != ImageResourceClass::Storage && !base.written && !base.atomic;
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
		// Image atomics are unsigned by construction, so k32SInt is the same bits under a
		// different guest label and the host binds it through an R32_UINT storage view.
		const bool atomic_sint =
		    base.atomic && !base.atomic64 && format == Prospero::BufferFormat::k32SInt;
		if (base.atomic && !atomic_sint &&
		    (base.atomic64 ? format != Prospero::BufferFormat::k32_32UInt
		                   : format != Prospero::BufferFormat::k32UInt &&
		                         format != Prospero::BufferFormat::k32Float)) {
			return SpecializationFail(
			    fmt::format("atomic image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
		const bool storage = base.resource_class == ImageResourceClass::Storage;
		image.fmask        = Prospero::IsFmaskTextureFormat(format);
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
		const bool raw_sint_storage = storage && format == Prospero::BufferFormat::k32SInt &&
		                              base.written && !base.read && !base.atomic;
		image.numeric_class         = Prospero::SampledTextureNumericClass(format);
		if (storage) {
			if ((!raw_sint_storage && !atomic_sint &&
			     image.numeric_class == Prospero::TextureNumericClass::Sint) ||
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
			return SpecializationFail(
			    fmt::format("sampled image descriptor {} uses unsupported format {}", i,
			                static_cast<uint32_t>(format)));
		}
	}
	for (uint32_t root_index = 0; root_index < specialization.images.size(); root_index++) {
		auto& root = specialization.images[root_index];
		if (root.indirect_root != root_index || root.bindless) {
			continue;
		}
		// The specialization names a directory slot; the mapping it points at is what has to be
		// in bounds and hold at least one candidate. Two was right while ordinal 0 was the first
		// enumerated record - a single key then needed no mapping at all, because the root was
		// already that key's descriptor. Now that ordinal 0 means "no match", a one-key mapping
		// still decides something: whether this draw's key is that key, or nothing.
		const auto directory_end = IndirectDirectoryBase(program) + IndirectDirectoryWords(program);
		const auto mapping_offset =
		    static_cast<size_t>(root.indirect_mapping_offset) < directory_end
		        ? static_cast<size_t>(snapshot.flattened_srt[root.indirect_mapping_offset])
		        : 0u;
		const auto key_count =
		    mapping_offset >= directory_end && mapping_offset < snapshot.flattened_srt.size()
		        ? snapshot.flattened_srt[mapping_offset]
		        : 0u;
		if (root.indirect_search_iterations == 0u || key_count == 0u ||
		    mapping_offset + 1u + static_cast<size_t>(key_count) * 2u >
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
		for (uint32_t candidate = 0; candidate < specialization.images.size(); candidate++) {
			auto& image = specialization.images[candidate];
			if (image.indirect_root != root_index) {
				continue;
			}
			if (NullImageDescriptor(snapshot.images[candidate])) {
				image.numeric_class     = image_class.numeric_class;
				image.mip_count         = image_class.mip_count;
				image.conversion_format = image_class.conversion_format;
				image.shader_swizzle    = image_class.shader_swizzle;
				if (!image.shape_padding) {
					image.dimension = image_class.dimension;
					image.cube      = image_class.cube;
				}
			}
			const bool same_coordinates =
			    image.dimension == image_class.dimension && image.cube == image_class.cube;
			const auto mismatch = [&](std::string_view property, uint32_t value,
			                          uint32_t expected) {
				return SpecializationFail(fmt::format(
				    "indirect image table at pc 0x{:08x} has incompatible candidates: {} {} vs {}",
				    program.info.images[root_index].first_use_pc, property, value, expected));
			};
			if (image.numeric_class != image_class.numeric_class) {
				return mismatch("numeric class", static_cast<uint32_t>(image.numeric_class),
				                static_cast<uint32_t>(image_class.numeric_class));
			}
			if (!same_coordinates && !(IsImageDimension2D(image.dimension) &&
			                           IsImageDimension2D(image_class.dimension))) {
				return mismatch("dimension",
				                static_cast<uint32_t>(image.dimension) * 2u +
				                    (image.cube ? 1u : 0u),
				                static_cast<uint32_t>(image_class.dimension) * 2u +
				                    (image_class.cube ? 1u : 0u));
			}
			if (image.mip_count != image_class.mip_count) {
				return mismatch("mip count", image.mip_count, image_class.mip_count);
			}
			if (image.conversion_format != image_class.conversion_format) {
				return mismatch("conversion format", static_cast<uint32_t>(image.conversion_format),
				                static_cast<uint32_t>(image_class.conversion_format));
			}
			if (image.shader_swizzle != image_class.shader_swizzle) {
				return mismatch("shader swizzle", static_cast<uint32_t>(image.shader_swizzle),
				                static_cast<uint32_t>(image_class.shader_swizzle));
			}
		}
	}
	CompactImages(snapshot.images,
	              [&](size_t index) { return !specialization.images[index].fmask; });
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
		usage[pair.sampler] |=
		    1u << static_cast<uint32_t>(ClassifySampler(base.images[pair.image]));
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
			first                 = false;
		}
	}
	return true;
}

template <typename Predicate>
static std::vector<ResourceBlock> ResourceControlFlow(const Program&   program,
                                                      const Predicate& predicate) {
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
		auto&                   block      = blocks[i];
		const auto&             info       = *program.blocks[i];
		const auto&             terminator = info.terminator;
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
			const auto op = inst.GetOpcode();
			if (op == ValueOpcode::ReadConst) {
				block.srt_reads.push_back(inst.Arg(1).Resolve().U32());
				continue;
			}
			const auto buffer = BufferAccessOf(op);
			const auto image  = ImageOpcodeInfoOf(op);
			if (buffer == BufferAccess::None && image.access == ImageAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index >= program.memory_info.size()) {
				return {};
			}
			const auto& memory = program.memory_info[index];
			// A V# the shader decodes itself binds nothing; its resource is still the raw operand.
			if (memory.planning_only || memory.dynamic_buffer ||
			    memory.kind == ResourceKind::IndirectBuffer) {
				continue;
			}
			// Without every source, pruning could drop a live resource; plan none instead.
			if (buffer != BufferAccess::None) {
				if (memory.resource >= program.info.buffers.size()) {
					return {};
				}
				block.sources.push_back(program.info.buffers[memory.resource].source);
			} else {
				if (memory.resource >= program.info.images.size() ||
				    (image.needs_sampler && memory.sampler >= program.info.samplers.size())) {
					return {};
				}
				block.sources.push_back(program.info.images[memory.resource].source);
				if (image.needs_sampler) {
					block.sources.push_back(program.info.samplers[memory.sampler].source);
				}
			}
		}
		std::ranges::sort(block.sources);
		block.sources.erase(std::unique(block.sources.begin(), block.sources.end()),
		                    block.sources.end());
	}
	for (auto& block: blocks)
		block.condition = predicate(block.condition);
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
	return inst->GetOpcode() == ValueOpcode::DispatchThreadInRange;
}

// Nonnegative affine coefficients for constant, local and workgroup coordinates. Reject modular
// arithmetic that could wrap; runtime coverage also bounds the largest invocation index.
static std::optional<std::array<uint64_t, 3>> FillIndex(Value value, uint32_t axis, Value guard,
                                                        uint32_t depth = 0) {
	value = ResolveActiveU32(value, guard);
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
	auto left  = FillIndex(inst->Arg(0), axis, guard, depth + 1);
	auto right = FillIndex(inst->Arg(1), axis, guard, depth + 1);
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
	const auto&     memory = program.memory_info.at(store->Flags<MemoryFlags>().index);
	UniformFillPlan result;
	result.fill.resource = memory.resource;
	Value data;
	Value guard(true);
	if (store->GetOpcode() == ValueOpcode::ImageWrite) {
		if (program.info.images.size() != 1 || memory.dmask != 1 || memory.data_bits != 32 ||
		    memory.image_has_mip || memory.image_sample_flags != 0 || memory.image_r128 ||
		    memory.image_dimension != Decoder::ImageDimension::Dim2DArray ||
		    !IsFullDispatchPredicate(store->Arg(3)))
			return {};
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
		data              = values->Arg(0);
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
		if (!memory.formatted || memory.typed || !memory.idxen || memory.offen ||
		    memory.offset != 0 || memory.data_bits != 32 ||
		    memory.data_dwords != static_cast<uint32_t>(store_op - stores.begin() + 1))
			return {};
		const auto address = FillIndex(store->Arg(1), 0, guard);
		if (!address || (*address)[0] != 0 || (*address)[1] != 1 || (*address)[2] == 0) return {};
		result.fill.kind            = UniformFillKind::Buffer;
		result.fill.group_stride[0] = static_cast<uint32_t>((*address)[2]);
		result.fill.words           = memory.data_dwords;
		data                        = store->Arg(4);
	}
	data                        = ResolveActiveU32(data, guard);
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

static BufferCopy AnalyzeBufferCopy(const Program& program) {
	if (program.stage != ShaderType::Compute || program.blocks.empty() || program.info.uses_dma ||
	    !program.info.samplers.empty() || !program.info.images.empty() ||
	    program.info.buffers.size() != 2) {
		return {};
	}
	std::unordered_set<const Block*> visited;
	std::vector<const Inst*>         stores;
	const auto*                      block = program.blocks.front();
	for (;;) {
		if (block == nullptr || !visited.insert(block).second) return {};
		for (const auto& inst: *block) {
			if (AddressOpcodeInfoOf(inst.GetOpcode()).access != AddressAccess::None) return {};
			if (!inst.MayHaveSideEffects()) continue;
			if (BufferAccessOf(inst.GetOpcode()) != BufferAccess::Write ||
			    stores.size() >= BufferCopy::MaxPairs)
				return {};
			stores.push_back(&inst);
		}
		const auto& term = block->terminator;
		if (term.kind == CFG::TerminatorKind::Return) break;
		if (term.kind != CFG::TerminatorKind::Branch) return {};
		block = term.true_block;
	}
	if (stores.empty() || visited.size() != program.blocks.size()) return {};

	constexpr std::array store_ops {ValueOpcode::StoreBufferU32, ValueOpcode::StoreBufferU32x2,
	                                ValueOpcode::StoreBufferU32x3, ValueOpcode::StoreBufferU32x4};
	constexpr std::array load_ops {ValueOpcode::LoadBufferU32, ValueOpcode::LoadBufferU32x2,
	                               ValueOpcode::LoadBufferU32x3, ValueOpcode::LoadBufferU32x4};
	constexpr std::array composites {ValueOpcode::CompositeConstructU32x2,
	                                 ValueOpcode::CompositeConstructU32x3,
	                                 ValueOpcode::CompositeConstructU32x4};
	constexpr std::array extracts {ValueOpcode::CompositeExtractU32x2,
	                               ValueOpcode::CompositeExtractU32x3,
	                               ValueOpcode::CompositeExtractU32x4};
	// Raw dword elements addressed by index alone; a format or type conversion is not a copy.
	const auto raw_element = [](const MemoryInfo& memory, uint32_t words) {
		return !memory.formatted && !memory.typed && memory.idxen && !memory.offen &&
		       memory.offset == 0 && memory.data_bits == 32 && memory.data_dwords == words &&
		       !memory.planning_only && !memory.dynamic_buffer &&
		       memory.kind == ResourceKind::Buffer;
	};
	BufferCopy result;
	for (const auto* store: stores) {
		const auto store_op = std::ranges::find(store_ops, store->GetOpcode());
		if (store_op == store_ops.end()) return {};
		const auto words = static_cast<uint32_t>(store_op - store_ops.begin() + 1);
		const auto guard = store->Arg(5).Resolve();
		if (store->Arg(2).Resolve() != Value(0u) || store->Arg(3).Resolve() != Value(0u) ||
		    !IsFullDispatchPredicate(guard))
			return {};
		const auto& store_memory = program.memory_info.at(store->Flags<MemoryFlags>().index);
		if (!raw_element(store_memory, words)) return {};

		// The stored value must be the loaded one, component for component.
		const Inst* load = nullptr;
		const auto  data = ResolveActiveU32(store->Arg(4), guard);
		if (words == 1) {
			load = data.TryInstruction();
		} else {
			const auto* vector = data.TryInstruction();
			if (vector == nullptr || vector->GetOpcode() != composites[words - 2]) return {};
			for (uint32_t i = 0; i < words; ++i) {
				const auto* extract = ResolveActiveU32(vector->Arg(i), guard).TryInstruction();
				if (extract == nullptr || extract->GetOpcode() != extracts[words - 2] ||
				    extract->Arg(1).Resolve() != Value(i))
					return {};
				const auto* source = extract->Arg(0).Resolve().TryInstruction();
				if (source == nullptr || (load != nullptr && source != load)) return {};
				load = source;
			}
		}
		if (load == nullptr || load->GetOpcode() != load_ops[words - 1] ||
		    load->Arg(1).Resolve() != store->Arg(1).Resolve() ||
		    load->Arg(2).Resolve() != Value(0u) || load->Arg(3).Resolve() != Value(0u) ||
		    !IsFullDispatchPredicate(load->Arg(4).Resolve()))
			return {};
		const auto& load_memory = program.memory_info.at(load->Flags<MemoryFlags>().index);
		if (!raw_element(load_memory, words) || load_memory.resource == store_memory.resource)
			return {};

		const auto element = FillIndex(store->Arg(1), 0, guard);
		if (!element || (*element)[1] != 1 || (*element)[2] == 0 || (*element)[0] > UINT32_MAX)
			return {};
		if (result.pairs == 0) {
			result.src          = load_memory.resource;
			result.dst          = store_memory.resource;
			result.words        = words;
			result.group_stride = static_cast<uint32_t>((*element)[2]);
		} else if (result.src != load_memory.resource || result.dst != store_memory.resource ||
		           result.words != words || result.group_stride != (*element)[2]) {
			return {};
		}
		result.offsets[result.pairs++] = static_cast<uint32_t>((*element)[0]);
	}
	const auto& source      = program.info.buffers.at(result.src);
	const auto& destination = program.info.buffers.at(result.dst);
	if (source.written || source.atomic || !destination.written || destination.read ||
	    destination.atomic)
		return {};
	result.valid = true;
	return result;
}

static std::vector<SrtReadWriteOrder> OrderSrtReadsAgainstWrites(const Program& program) {
	const auto count = program.blocks.size();
	if (count == 0u) {
		return {};
	}
	std::unordered_map<const Block*, uint32_t> indices;
	for (uint32_t i = 0; i < count; i++) {
		if (!indices.emplace(program.blocks[i], i).second) {
			return {};
		}
	}
	std::vector<std::vector<uint32_t>> successors(count);
	for (uint32_t i = 0; i < count; i++) {
		const auto&             terminator = program.blocks[i]->terminator;
		const std::array        branches {terminator.true_block, terminator.false_block};
		std::span<Block* const> targets;
		switch (terminator.kind) {
			case CFG::TerminatorKind::Branch: targets = std::span(branches).first(1); break;
			case CFG::TerminatorKind::ConditionalBranch: targets = branches; break;
			case CFG::TerminatorKind::IndirectBranch:
				targets = program.blocks[i]->ImmSuccessors();
				break;
			case CFG::TerminatorKind::Return: break;
			default: return {};
		}
		for (const auto target: targets) {
			const auto found = indices.find(target);
			if (found == indices.end()) {
				return {};
			}
			successors[i].push_back(found->second);
		}
	}
	const auto written = [&](const Inst& inst, SrtReadWriteOrder& order) {
		const auto op     = inst.GetOpcode();
		const auto buffer = BufferAccessOf(op);
		const auto image  = ImageOpcodeInfoOf(op).access;
		if (image == ImageAccess::Write || image == ImageAccess::Atomic ||
		    AddressOpcodeInfoOf(op).access == AddressAccess::Write) {
			order.unknown = true;
			return;
		}
		if (buffer != BufferAccess::Write && buffer != BufferAccess::Atomic) {
			return;
		}
		const auto index = inst.Flags<MemoryFlags>().index;
		if (index >= program.memory_info.size()) {
			order.unknown = true;
			return;
		}
		const auto& memory = program.memory_info[index];
		if (memory.planning_only || memory.dynamic_buffer || memory.kind != ResourceKind::Buffer ||
		    memory.resource >= program.info.buffers.size() || memory.resource >= 64u) {
			order.unknown = true;
			return;
		}
		order.buffers |= uint64_t {1} << memory.resource;
	};
	std::vector<SrtReadWriteOrder> generated(count, SrtReadWriteOrder {.unknown = false});
	for (uint32_t i = 0; i < count; i++) {
		for (const auto& inst: *program.blocks[i]) {
			written(inst, generated[i]);
		}
	}
	const auto merge = [](SrtReadWriteOrder& into, const SrtReadWriteOrder& from) {
		const auto before = into;
		into.buffers |= from.buffers;
		into.unknown = into.unknown || from.unknown;
		return into != before;
	};
	std::vector<SrtReadWriteOrder> entry(count, SrtReadWriteOrder {.unknown = false});
	std::vector<uint32_t>          pending(count);
	std::vector<uint8_t>           queued(count, 1u);
	for (uint32_t i = 0; i < count; i++) {
		pending[i] = i;
	}
	while (!pending.empty()) {
		const auto block = pending.back();
		pending.pop_back();
		queued[block] = 0u;
		auto exit     = entry[block];
		merge(exit, generated[block]);
		for (const auto next: successors[block]) {
			if (merge(entry[next], exit) && !queued[next]) {
				queued[next] = 1u;
				pending.push_back(next);
			}
		}
	}
	std::vector<SrtReadWriteOrder> order(program.srt_reads.size());
	std::vector<uint8_t>           seen(program.srt_reads.size(), 0u);
	for (uint32_t i = 0; i < count; i++) {
		auto running = entry[i];
		for (const auto& inst: *program.blocks[i]) {
			if (inst.GetOpcode() == ValueOpcode::ReadConst) {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= order.size()) {
					continue;
				}
				auto& into = order[slot.U32()];
				if (!seen[slot.U32()]) {
					seen[slot.U32()] = 1u;
					into             = SrtReadWriteOrder {.unknown = false};
				}
				merge(into, running);
				continue;
			}
			written(inst, running);
		}
	}
	return order;
}

namespace {

// Copies a value graph out of a translated program into a plan that owns it. Shared by the plan
// extraction and the table check, so the check evaluates exactly what the materialization will.
class PlanValueCloner {
public:
	PlanValueCloner(const Program& program, ResourcePlan& plan): m_program(program), m_plan(plan) {}

	Value operator()(Value value) {
		value              = value.Resolve();
		const auto* source = value.TryInstruction();
		if (source == nullptr) {
			return value;
		}
		if (source->GetOpcode() == ValueOpcode::ReadFirstLane &&
		    ValidateRuntimeValue(m_program, source->Arg(0))) {
			// Uniform values need no new EXEC context; preserve their shared evaluation memo.
			return (*this)(source->Arg(0));
		}
		if (source->GetOpcode() == ValueOpcode::Phi) {
			const auto invariant = ResolveInvariantPhi(m_program, value);
			if (!invariant.IsEmpty() && invariant != value) {
				return (*this)(invariant);
			}
		}
		if (const auto found = m_cloned.find(source); found != m_cloned.end()) {
			return Value(found->second);
		}
		auto& target =
		    m_plan.value_storage.emplace_back(source->GetOpcode(), source->Flags<uint64_t>());
		m_cloned.emplace(source, &target);
		if (source->GetOpcode() == ValueOpcode::Phi) {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.AddPhiOperand(nullptr, (*this)(source->Arg(index)));
			}
		} else {
			for (size_t index = 0; index < source->NumArgs(); index++) {
				target.SetArg(index, (*this)(source->Arg(index)));
			}
		}
		return Value(&target);
	}

private:
	const Program&                         m_program;
	ResourcePlan&                          m_plan;
	std::unordered_map<const Inst*, Inst*> m_cloned;
};

void CopyPlanHeader(const Program& program, ResourcePlan& plan) {
	plan.stage                      = program.stage;
	plan.shader_hash                = program.shader_hash;
	plan.user_data_base             = program.user_data_base;
	plan.user_data_count            = program.user_data_count;
	plan.wave_size                  = program.wave_size;
	plan.info                       = program.info;
	plan.memory_info                = program.memory_info;
	plan.source_reads               = program.source_reads;
	plan.srt_plan_complete          = program.srt_plan_complete;
	plan.resource_tracking_complete = program.resource_tracking_complete;
	plan.frozen_values              = true;
}

} // namespace

bool IndirectBufferTableRefused(const Program& program, std::span<const DescriptorSource> sources,
                                uint32_t table_source, const SrtRuntime& runtime) {
	KYTY_PROFILER_FUNCTION();
	if (table_source >= sources.size() || !sources[table_source].indirect_buffer.has_value() ||
	    runtime.read_specialization_memory == nullptr) {
		return false;
	}
	const auto& indirect = *sources[table_source].indirect_buffer;
	if (indirect.heap_source >= sources.size()) {
		return false;
	}
	const auto&  heap = sources[indirect.heap_source];
	ResourcePlan plan;
	CopyPlanHeader(program, plan);
	PlanValueCloner clone(program, plan);
	auto&           target = plan.descriptor_sources.emplace_back();
	target.dword_count     = heap.dword_count;
	for (uint32_t dword = 0; dword < heap.dword_count; dword++) {
		target.dwords[dword] = clone(heap.dwords[dword]);
	}
	DescriptorValue heap_value;
	SrtWalker       clean(plan, CleanRuntime(runtime));
	if (!clean.EvaluateDescriptor(0, heap_value)) {
		return false;
	}
	std::shared_ptr<const IndirectBuffer> result;
	bool                                  refused = false;
	MaterializeIndirectBuffer(indirect, heap_value, runtime, program.shader_hash, result, refused);
	return refused;
}

ResourcePlan ExtractResourcePlan(const Program& program) {
	ResourcePlan plan;
	CopyPlanHeader(program, plan);

	PlanValueCloner cloner(program, plan);
	const auto      Clone = [&](Value value) { return cloner(value); };

	plan.descriptor_sources.reserve(program.descriptor_sources.size());
	for (const auto& source: program.descriptor_sources) {
		auto& target               = plan.descriptor_sources.emplace_back();
		target.dword_count         = source.dword_count;
		target.indirect_descriptor = source.indirect_descriptor;
		// Plain indices and offsets, so it needs no cloning - but it does need carrying, or the
		// plan binds the heap V# in place of the record the table selects.
		target.indirect_buffer = source.indirect_buffer;
		if (target.indirect_descriptor.has_value()) {
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
	plan.srt_reads.reserve(program.srt_reads.size());
	for (const auto& read: program.srt_reads) {
		plan.srt_reads.push_back({Clone(read.value), read.flat_offset});
	}
	plan.data_flat_slots = program.data_flat_slots;
	plan.srt_read_order  = OrderSrtReadsAgainstWrites(program);
	// A proven uniform factor can decide a branch even when its other lanes are unknown.
	// Keep only that Boolean structure, never the varying shader dependency graph.
	CyclicPhiEntryCache                    cyclic_entries {.program = &program};
	std::unordered_map<const Inst*, bool>  validated;
	Value                                  unknown;
	bool                                   capture_control_reads = false;
	std::unordered_map<const Inst*, Value> predicates;
	std::function<Value(Value)>            ClonePredicate = [&](Value value) -> Value {
		value = value.Resolve();
		if (value.IsEmpty()) return {};
		const auto* inst = value.TryInstruction();
		if (inst == nullptr)
			return ValidateRuntimeValue(program, value, RuntimeValueType::Integer) ? value
			                                                                       : Value {};
		// Preserve shared EXEC predicates, including subgraphs with no uniform factor.
		const auto [found, inserted] = predicates.try_emplace(inst);
		if (!inserted) return found->second;
		auto& result = found->second;
		if (inst->GetOpcode() == ValueOpcode::DispatchThreadInRange) return result = Value(true);
		const auto runtime = [&] {
			if (const auto known = validated.find(inst); known != validated.end()) {
				return known->second;
			}
			const bool valid = ValidateRuntimeValue(program, value, RuntimeValueType::Integer,
			                                        nullptr, &cyclic_entries);
			validated.emplace(inst, valid);
			return valid;
		};
		if (runtime()) {
			capture_control_reads |= !ValidateRuntimeValue(
			    program, value, RuntimeValueType::ImmutableInteger, nullptr, &cyclic_entries);
			return result = Clone(value);
		}
		const auto op = inst->GetOpcode();
		if (op == ValueOpcode::ConditionRef || op == ValueOpcode::LogicalNot) {
			const auto operand = ClonePredicate(inst->Arg(0));
			if (operand.IsEmpty() || op == ValueOpcode::ConditionRef) return result = operand;
			auto& node = plan.value_storage.emplace_back(op);
			node.SetArg(0, operand);
			return result = Value(&node);
		}
		if (op != ValueOpcode::LogicalAnd && op != ValueOpcode::LogicalOr) return {};
		auto left  = ClonePredicate(inst->Arg(0));
		auto right = ClonePredicate(inst->Arg(1));
		if (op == ValueOpcode::LogicalAnd && left == Value(true)) return result = right;
		if (op == ValueOpcode::LogicalAnd && right == Value(true)) return result = left;
		if (left.IsEmpty() && right.IsEmpty()) return {};
		if (left.IsEmpty() || right.IsEmpty()) {
			if (unknown.IsEmpty())
				unknown = Value(&plan.value_storage.emplace_back(ValueOpcode::UndefU1));
			if (left.IsEmpty()) left = unknown;
			if (right.IsEmpty()) right = unknown;
		}
		auto& node = plan.value_storage.emplace_back(op);
		node.SetArg(0, left);
		node.SetArg(1, right);
		return result = Value(&node);
	};
	plan.control_flow = ResourceControlFlow(program, ClonePredicate);
	plan.uniform_fill = AnalyzeUniformFill(program);
	plan.buffer_copy  = AnalyzeBufferCopy(program);
	for (uint32_t i = 0; i < plan.uniform_fill.fill.words; ++i) {
		plan.uniform_fill.values[i] = Clone(plan.uniform_fill.values[i]);
	}
	plan.clean_flat_slots.resize(plan.srt_reads.size());
	bool capture_image_reads = false;
	for (const auto& buffer: plan.info.buffers) {
		const auto* source = Source(plan, buffer.source);
		if (source == nullptr || !source->indirect_buffer.has_value()) {
			continue;
		}
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_buffer->heap_source),
		                   plan.clean_flat_slots);
	}
	for (const auto& image: plan.info.images) {
		const auto* source = Source(plan, image.source);
		if (source == nullptr || !source->indirect_descriptor.has_value()) {
			continue;
		}
		plan.requires_specialization_memory = true;
		capture_image_reads |= !source->indirect_descriptor->selector_mask.IsEmpty() ||
		                       !source->indirect_descriptor->selector_first.IsEmpty() ||
		                       source->indirect_descriptor->selector_shift != 0u ||
		                       !source->indirect_descriptor->sources.empty();
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_descriptor->material_source),
		                   plan.clean_flat_slots, source->indirect_descriptor->selector_mask);
		MarkCleanFlatSlots(plan, nullptr, plan.clean_flat_slots,
		                   source->indirect_descriptor->selector_first);
		if (source->indirect_descriptor->sources.empty()) {
			MarkCleanFlatSlots(plan, Source(plan, source->indirect_descriptor->table_source),
			                   plan.clean_flat_slots);
		} else {
			for (const auto candidate: source->indirect_descriptor->sources) {
				MarkCleanFlatSlots(plan, Source(plan, candidate), plan.clean_flat_slots);
			}
		}
	}
	// Guest code a linked SWAPPC leaf was read from is revalidated with the other captured reads.
	plan.capture_specialization_reads =
	    capture_image_reads || capture_control_reads || !plan.source_reads.empty();
	if (plan.capture_specialization_reads) {
		// Clean writable addresses prove that shader writes cannot overlap captured reads.
		for (const auto& buffer: plan.info.buffers) {
			if (buffer.written)
				MarkCleanFlatSlots(plan, Source(plan, buffer.source), plan.clean_flat_slots);
		}
		for (const auto& image: plan.info.images) {
			if (image.written)
				MarkCleanFlatSlots(plan, Source(plan, image.source), plan.clean_flat_slots);
		}
	}
	if (capture_image_reads || !plan.source_reads.empty())
		plan.resource_tracking_complete &= !program.has_address_writes;
	for (const auto& buffer: plan.info.buffers) {
		const auto* source = Source(plan, buffer.source);
		if (source == nullptr || !source->indirect_buffer.has_value()) {
			continue;
		}
		plan.requires_specialization_memory = true;
		MarkCleanFlatSlots(plan, Source(plan, source->indirect_buffer->heap_source),
		                   plan.clean_flat_slots);
	}
	return plan;
}

// Materialization has a dozen distinct ways to refuse and the report carried none of them, so a
// dropped dispatch said only that it was dropped. The last reason stands until the next attempt.
std::string& MaterializeFailure() {
	static thread_local std::string reason;
	return reason;
}

std::string_view LastMaterializeFailure() {
	return MaterializeFailure();
}

// The renderer treats a captured read inside a buffer the shader writes as fatal.
static bool CapturedReadsAvoidWrittenBuffers(const ResourcePlan& program,
                                             ResourceSnapshot&   snapshot) {
	auto&             reads = snapshot.specialization_reads;
	auto&             slots = snapshot.specialization_read_slots;
	std::vector<bool> exempt(reads.size(), false);
	bool              any_exempt = false;
	for (uint32_t i = 0; i < program.info.buffers.size() && i < snapshot.buffers.size(); ++i) {
		if (!program.info.buffers[i].written) continue;
		ShaderBufferResource buffer;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], buffer)) continue;
		const auto base = buffer.Base48();
		const auto size = buffer.GetSize();
		if (size == 0u) continue;
		// The leading ranges are the linked code (source_reads). They only detect a changed
		// leaf and never prune blocks, so a store over them does not invalidate the walk.
		for (size_t r = program.source_reads.size(); r < reads.size(); ++r) {
			const auto [address, bytes] = reads[r];
			if (bytes == 0u || address >= base + size || base >= address + bytes) continue;
			const auto slot = r < slots.size() ? slots[r] : UINT32_MAX;
			if (program.stage != ShaderType::Compute || slot >= program.srt_read_order.size()) {
				return false;
			}
			const auto& order = program.srt_read_order[slot];
			if (order.unknown || i >= 64u || ((order.buffers >> i) & 1u) != 0u) {
				return false;
			}
			exempt[r]  = true;
			any_exempt = true;
		}
	}
	if (any_exempt) {
		size_t kept = 0;
		for (size_t r = 0; r < reads.size(); ++r) {
			if (exempt[r]) continue;
			reads[kept] = reads[r];
			if (r < slots.size()) slots[kept] = slots[r];
			kept++;
		}
		reads.resize(kept);
		slots.resize(std::min(slots.size(), kept));
	}
	return true;
}

bool ImageServedBindless(const ResourcePlan& program, uint32_t image_index) {
	return image_index < program.info.images.size() && ServedBindless(program, image_index);
}

bool ReadOverlapsWrittenBuffer(const ResourcePlan& program, const ResourceSnapshot& snapshot,
                               uint64_t address, uint64_t bytes) {
	for (uint32_t i = 0; i < program.info.buffers.size() && i < snapshot.buffers.size(); ++i) {
		if (!program.info.buffers[i].written) continue;
		ShaderBufferResource buffer;
		if (!DecodeBufferDescriptor(snapshot.buffers[i], buffer)) continue;
		const auto base = buffer.Base48();
		const auto size = buffer.GetSize();
		if (size != 0u && bytes != 0u && address < base + size && base < address + bytes) {
			return true;
		}
	}
	return false;
}

bool MaterializeInto(const ResourcePlan& program, const SrtRuntime& runtime,
                     ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                     std::vector<uint32_t>* refused_tables, bool prune = true) {
	if (!program.resource_tracking_complete ||
	    ((program.requires_specialization_memory || !program.source_reads.empty()) &&
	     runtime.read_specialization_memory == nullptr)) {
		MaterializeFailure() = "plan incomplete";
		return false;
	}
	const bool capture_reads =
	    program.capture_specialization_reads || !program.source_reads.empty();
	auto& reads = snapshot.specialization_reads;
	// Ranges read while linking the shader's code are revalidated like any captured read.
	reads = program.source_reads;
	snapshot.specialization_read_slots.assign(reads.size(), UINT32_MAX);
	ReadCapture capture {runtime, reads, snapshot.specialization_read_slots};
	SrtRuntime  observed = runtime;
	if (capture_reads) {
		observed.userdata = &capture;
		observed.read_specialization_memory =
		    runtime.read_specialization_memory != nullptr ? CaptureStrictRead : nullptr;
		observed.read_memory = CaptureOrdinaryRead;
	}
	if (capture_reads && runtime.read_condition_memory != nullptr) {
		observed.read_condition_memory = CaptureConditionRead;
	}
	SrtRuntime condition_runtime = observed;
	if (observed.read_condition_memory != nullptr) {
		condition_runtime.read_memory                = observed.read_condition_memory;
		condition_runtime.read_specialization_memory = observed.read_condition_memory;
	}
	SrtWalker          clean(program, CleanRuntime(observed));
	SrtWalker          conditions(program, condition_runtime);
	SrtWalker          walker(program, observed, program.clean_flat_slots, &clean);
	FlatRefreshFailure flat_failure;
	if (!walker.RefreshFlatBuffer(snapshot.flattened_srt, &flat_failure, prune,
	                              observed.read_condition_memory != nullptr ? &conditions
	                                                                        : nullptr)) {
		// A hoisted read whose own address never evaluates stays a native load on rebuild.
		if (refused_tables != nullptr) {
			for (const auto slot: flat_failure.gpu_owned_slots) {
				const auto* read = program.srt_reads[slot].value.Resolve().TryInstruction();
				refused_tables->push_back(read->Flags<MemoryFlags>().pc);
			}
		}
		if (refused_tables != nullptr &&
		    flat_failure.stage == FlatRefreshFailure::Stage::ValueUnevaluable &&
		    (flat_failure.raw_read == RawReadReject::HandleOperandUnavailable ||
		     flat_failure.raw_read == RawReadReject::GpuOwned) &&
		    flat_failure.flat_offset < program.srt_reads.size()) {
			if (const auto* read =
			        program.srt_reads[flat_failure.flat_offset].value.Resolve().TryInstruction();
			    read != nullptr && (read->GetOpcode() == ValueOpcode::LoadAddressU32 ||
			                        read->GetOpcode() == ValueOpcode::ReadConstBuffer)) {
				refused_tables->push_back(read->Flags<MemoryFlags>().pc);
			}
		}
		MaterializeFailure() =
		    fmt::format("flat srt refresh: {}", DescribeFlatRefreshFailure(flat_failure));
		return false;
	}
	const auto active = std::span<const uint8_t>(program.active_sources);
	// Reserve the indirect directory before any mapping is appended, so a slot is a function of
	// the resource index and the SRT width alone.
	snapshot.flattened_srt.resize(IndirectDirectoryBase(program) + IndirectDirectoryWords(program),
	                              0u);
	snapshot.uniform_fill         = {};
	snapshot.buffer_copy          = program.buffer_copy;
	const auto&             fill  = program.uniform_fill;
	const auto              words = fill.fill.words;
	std::array<uint32_t, 4> stored {};
	bool                    uniform_fill = words != 0;
	bool                    words_agree  = true;
	for (uint32_t i = 0; i < words && uniform_fill; ++i) {
		uniform_fill = clean.Evaluate(fill.values[i], stored[i]);
		words_agree  = words_agree && stored[i] == stored[0];
	}
	if (uniform_fill) {
		snapshot.uniform_fill             = fill.fill;
		snapshot.uniform_fill.value       = stored[0];
		snapshot.uniform_fill.word_values = stored;
		snapshot.uniform_fill.words_agree = words_agree;
	}
	const auto evaluate = [&](uint32_t source, DescriptorValue& value, bool written = false) {
		if (source >= program.descriptor_sources.size()) {
			MaterializeFailure() = "active source scan";
			return false;
		}
		if (active.empty() || active[source]) {
			return (written ? clean : walker).EvaluateDescriptor(source, value);
		}
		value             = {};
		value.dword_count = program.descriptor_sources[source].dword_count;
		return true;
	};
	bool table_refused = false;
	snapshot.buffers.resize(program.info.buffers.size());
	// Sized before the loop: expanding a table appends a child to both and writes the root's slot
	// here.
	specialization.buffers.resize(program.info.buffers.size());
	for (uint32_t i = 0; i < program.info.buffers.size(); ++i) {
		const auto& buffer = program.info.buffers[i];
		const auto* source = Source(program, buffer.source);
		if (source != nullptr && source->indirect_buffer.has_value()) {
			snapshot.buffers[i] = {.dword_count = 4u};
			if (!active.empty() && !active[buffer.source]) {
				continue;
			}
			DescriptorValue                       table;
			std::shared_ptr<const IndirectBuffer> resolved;
			bool                                  refused = false;
			if (!clean.EvaluateDescriptor(source->indirect_buffer->heap_source, table) ||
			    !MaterializeIndirectBuffer(*source->indirect_buffer, table, runtime,
			                               program.shader_hash, resolved, refused)) {
				if (refused && refused_tables != nullptr) {
					refused_tables->push_back(buffer.first_use_pc);
					table_refused = true;
					continue;
				}
				MaterializeFailure() = "buffer heap descriptor";
				return false;
			}
			// One descriptor needs no selection at all: bind it and be done.
			if (resolved->descriptors.size() == 1u) {
				snapshot.buffers[i] = resolved->descriptors[resolved->candidates[0]];
				continue;
			}
			// More than one: bind every record and let the access pick at runtime, as indirect
			// images do.
			if (!ExpandIndirectBuffer(program, i, *resolved, snapshot, specialization)) {
				MaterializeFailure() = "buffer table expansion";
				return false;
			}
			continue;
		}
		if (!evaluate(buffer.source, snapshot.buffers[i], buffer.written)) {
			// An unprovable loop-carried descriptor is decoded in-shader on rebuild, not dropped.
			if (refused_tables != nullptr && (buffer.written ? clean : walker).RefusedOnPhi()) {
				refused_tables->push_back(buffer.first_use_pc);
				table_refused = true;
				continue;
			}
			// Which buffer, and whether it was the table's own root, is what separates a
			// loop-carried heap record from an ordinary descriptor that simply would not read.
			MaterializeFailure() =
			    fmt::format("buffer descriptor: buffer {} source {}{}{}", i, buffer.source,
			                buffer.indirect_root == i ? " (indirect root)" : "",
			                buffer.written ? " (written)" : "");
			return false;
		}
	}
	if (table_refused) {
		MaterializeFailure() = "buffer heap descriptor";
		return false;
	}
	snapshot.images.resize(program.info.images.size());
	specialization.images.resize(program.info.images.size());
	for (uint32_t i = 0; i < program.info.images.size(); ++i) {
		const auto& image        = program.info.images[i];
		specialization.images[i] = {
		    .numeric_class              = image.numeric_class,
		    .dimension                  = image.dimension,
		    .mip_count                  = image.mip_count,
		    .conversion_format          = image.conversion_format,
		    .shader_swizzle             = image.shader_swizzle,
		    .indirect_root              = image.indirect_root,
		    .indirect_mapping_offset    = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .cube                       = image.cube,
		};
		const auto* source = Source(program, image.source);
		if (source == nullptr) {
			MaterializeFailure() = "image source missing";
			return false;
		}
		if (ServedBindless(program, i)) {
			snapshot.images[i]                = {.dword_count = 8u};
			const auto slot                   = IndirectMappingSlot(program, i);
			auto&      root                   = specialization.images[i];
			root.bindless                     = true;
			root.indirect_root                = i;
			root.indirect_mapping_offset      = static_cast<uint32_t>(slot);
			root.indirect_search_iterations   = 0;
			snapshot.flattened_srt[slot]      = 0;
			snapshot.flattened_srt[slot + 1u] = 0;
			if (!active.empty() && !active[image.source]) {
				continue;
			}
			const auto&     indirect = *source->indirect_descriptor;
			DescriptorValue table;
			if (!clean.EvaluateDescriptor(indirect.table_source, table) ||
			    table.dword_count != 4u) {
				MaterializeFailure() =
				    fmt::format("bindless image table at pc 0x{:08x}: its heap descriptor is not "
				                "host-evaluable",
				                image.first_use_pc);
				return false;
			}
			BindlessImageTable entry;
			entry.srt_offset = static_cast<uint32_t>(slot);
			entry.stride     = indirect.heap_stride;
			if (indirect.indexed_heap) {
				// A vector load through a V# with format 0 reads zero, so every key of a disabled
				// heap is null; the zeroed slot count already selects the null arm.
				if (((table.dwords[3] >> 12u) & 0x7fu) == 0u) {
					continue;
				}
				// A swizzled or ADD_TID heap has no flat record step.
				const auto stride  = (table.dwords[1] >> 16u) & 0x3fffu;
				const bool swizzle = ((table.dwords[1] >> 31u) & 1u) != 0u;
				const bool add_tid = ((table.dwords[3] >> 23u) & 1u) != 0u;
				const auto step    = uint64_t {stride} * indirect.heap_stride;
				if (stride == 0u || swizzle || add_tid || step > UINT32_MAX) {
					MaterializeFailure() = fmt::format(
					    "bindless image table at pc 0x{:08x}: indexed heap V# has stride {}{}{}",
					    image.first_use_pc, stride, swizzle ? " (swizzled)" : "",
					    add_tid ? " (add_tid)" : "");
					return false;
				}
				entry.stride = static_cast<uint32_t>(step);
			}
			entry.record_offset = indirect.table_offset + indirect.record_offset;
			std::copy_n(table.dwords.begin(), 4u, entry.heap.begin());
			snapshot.bindless_tables.push_back(entry);
			continue;
		}
		if (source->indirect_descriptor.has_value()) {
			snapshot.images[i] = {.dword_count = 8u};
			if (!active.empty() && !active[image.source]) {
				continue;
			}
			const auto&     indirect = *source->indirect_descriptor;
			DescriptorValue material;
			DescriptorValue table;
			if (indirect.sources.empty() &&
			    ((indirect.material_source != UINT32_MAX &&
			      !clean.EvaluateDescriptor(indirect.material_source, material)) ||
			     !clean.EvaluateDescriptor(indirect.table_source, table))) {
				MaterializeFailure() =
				    fmt::format("indirect image table at pc 0x{:08x}: its own handles are not "
				                "host-evaluable",
				                image.first_use_pc);
				return false;
			}
			IndirectImageFailure indirect_failure;
			if (!MaterializeIndirectImage(program, indirect, material, table, i, observed, clean,
			                              snapshot, specialization, indirect_failure)) {
				MaterializeFailure() =
				    fmt::format("indirect image table at pc 0x{:08x}: {}", image.first_use_pc,
				                DescribeIndirectImageFailure(indirect_failure));
				return false;
			}
		} else {
			if (!evaluate(image.source, snapshot.images[i], image.written)) {
				MaterializeFailure() = "image descriptor";
				return false;
			}
			if (!ValidImageDescriptor(snapshot.images[i], image.r128)) {
				snapshot.images[i].dwords.fill(0);
			}
		}
	}
	CanonicalizeIndirectImageTables(program, snapshot, specialization);
	PadIndirectImageShapes(program, snapshot, specialization);
	snapshot.samplers.resize(program.info.samplers.size());
	for (uint32_t i = 0; i < program.info.samplers.size(); ++i) {
		auto source = program.info.samplers[i].source;
		if (source >= program.descriptor_sources.size()) {
			MaterializeFailure() = "sampler descriptor";
			return false;
		}
		// A finite inline sampler's root binds its first candidate.
		const auto& descriptor = program.descriptor_sources[source];
		if (descriptor.indirect_descriptor) {
			if (descriptor.indirect_descriptor->sources.empty()) {
				MaterializeFailure() = "sampler descriptor";
				return false;
			}
			source = descriptor.indirect_descriptor->sources[0];
		}
		if (!evaluate(source, snapshot.samplers[i])) {
			MaterializeFailure() = "sampler descriptor";
			return false;
		}
		if (program.info.samplers[i].gather_lod) {
			const auto control = snapshot.samplers[i].dwords[2];
			const auto filter  = (control >> 26u) & 3u;
			// MipNone always selects the base level. Explicit point gathers currently require
			// encoded-zero primary and secondary bias; linear primary-mip selection is unsupported.
			if (filter > 1u || (filter == 1u && (control & 0xfffffu) != 0u)) {
				return SpecializationFail("explicit-LOD gather requires mip filtering None or "
				                          "Point with zero LOD biases");
			}
		}
	}
	snapshot.user_data.assign(runtime.user_data.begin(), runtime.user_data.end());
	return BuildResourceSpecialization(program, snapshot, specialization) &&
	       WriteIndirectImageSlots(program, snapshot, specialization);
}

bool RefreshDescriptorState(const ResourcePlan& program, ResourceSnapshot& snapshot,
                            ResourceSpecialization& specialization) {
	if (snapshot.images.size() != program.info.images.size() ||
	    specialization.images.size() != program.info.images.size() ||
	    snapshot.samplers.size() != program.info.samplers.size()) {
		return false;
	}
	for (uint32_t i = 0; i < program.info.images.size(); ++i) {
		const auto& image  = program.info.images[i];
		const auto* source = Source(program, image.source);
		if (source == nullptr || source->indirect_descriptor.has_value()) {
			continue;
		}
		specialization.images[i] = {
		    .numeric_class              = image.numeric_class,
		    .dimension                  = image.dimension,
		    .mip_count                  = image.mip_count,
		    .conversion_format          = image.conversion_format,
		    .shader_swizzle             = image.shader_swizzle,
		    .indirect_root              = image.indirect_root,
		    .indirect_mapping_offset    = image.indirect_mapping_offset,
		    .indirect_search_iterations = image.indirect_search_iterations,
		    .cube                       = image.cube,
		};
		if (!ValidImageDescriptor(snapshot.images[i], image.r128)) {
			snapshot.images[i].dwords.fill(0);
		}
	}
	for (uint32_t i = 0; i < program.info.samplers.size(); ++i) {
		if (program.info.samplers[i].gather_lod) {
			const auto control = snapshot.samplers[i].dwords[2];
			const auto filter  = (control >> 26u) & 3u;
			if (filter > 1u || (filter == 1u && (control & 0xfffffu) != 0u)) {
				return false;
			}
		}
	}
	return BuildResourceSpecialization(program, snapshot, specialization) &&
	       WriteIndirectImageSlots(program, snapshot, specialization);
}

// The caller holds one snapshot and one specialization per cached shader, and both outlive a
// refused draw: the permutation lookup compares the specialization, so a half-written one names
// a shape nothing was built for. Materialize aside and commit only when the whole draw resolves.
//
// Aside is a reused scratch pair, cleared rather than reconstructed so a warm draw allocates
// nothing, and the commit is a copy rather than a move so the caller keeps the storage its own
// descriptor writes already point at. Clearing is what makes reuse sound: every container then
// grows from empty exactly as a fresh snapshot would, so no flattened-SRT slot a shader leaves
// unwritten can carry another shader value.
bool MaterializeResources(const ResourcePlan& program, const SrtRuntime& runtime,
                          ResourceSnapshot& snapshot, ResourceSpecialization& specialization,
                          std::vector<uint32_t>* refused_tables) {
	KYTY_PROFILER_FUNCTION();
	static thread_local ResourceSnapshot       next;
	static thread_local ResourceSpecialization next_specialization;
	const auto                                 reset = [] {
		next.buffers.clear();
		next.images.clear();
		next.samplers.clear();
		next.flattened_srt.clear();
		next.user_data.clear();
		next.key_feedback.clear();
		next.bindless_tables.clear();
		next.uniform_fill = {};
		next.buffer_copy  = {};
		next_specialization.buffers.clear();
		next_specialization.images.clear();
	};
	reset();
	if (!MaterializeInto(program, runtime, next, next_specialization, refused_tables)) {
		return false;
	}
	// A branch condition read from a buffer the shader writes cannot prune blocks: walk them all.
	if (!CapturedReadsAvoidWrittenBuffers(program, next)) {
		reset();
		if (runtime.walk_restarted != nullptr) {
			runtime.walk_restarted(runtime.userdata);
		}
		if (!MaterializeInto(program, runtime, next, next_specialization, refused_tables, false)) {
			return false;
		}
		if (!CapturedReadsAvoidWrittenBuffers(program, next)) {
			MaterializeFailure() = "scalar reads overlap a buffer the shader writes";
			return false;
		}
	}
	snapshot       = next;
	specialization = next_specialization;
	return true;
}

const char* FirstSpecializationDifference(const ResourceSpecialization& before,
                                          const ResourceSpecialization& after) {
	if (before.buffers.size() != after.buffers.size()) {
		return "buffer count";
	}
	if (before.images.size() != after.images.size()) {
		return "image count";
	}
	for (size_t index = 0; index < before.buffers.size(); index++) {
		const auto& a = before.buffers[index];
		const auto& b = after.buffers[index];
		if (a == b) {
			continue;
		}
		if (a.packed_stride != b.packed_stride) {
			return "buffer stride";
		}
		if (a.descriptor_format != b.descriptor_format) {
			return "buffer format";
		}
		if (a.descriptor_swizzle != b.descriptor_swizzle) {
			return "buffer swizzle";
		}
		if (a.indirect_root != b.indirect_root) {
			return "buffer table root";
		}
		if (a.indirect_search_iterations != b.indirect_search_iterations) {
			return "buffer search depth";
		}
		if (a.indirect_feedback_keys != b.indirect_feedback_keys) {
			return "buffer feedback width";
		}
		return "buffer table offset";
	}
	for (size_t index = 0; index < before.images.size(); index++) {
		const auto& a = before.images[index];
		const auto& b = after.images[index];
		if (a == b) {
			continue;
		}
		if (a.numeric_class != b.numeric_class) {
			return "image numeric class";
		}
		if (a.dimension != b.dimension) {
			return "image dimension";
		}
		if (a.mip_count != b.mip_count) {
			return "image mip count";
		}
		if (a.conversion_format != b.conversion_format) {
			return "image format";
		}
		if (a.shader_swizzle != b.shader_swizzle) {
			return "image swizzle";
		}
		if (a.cube != b.cube || a.fmask != b.fmask || a.shape_padding != b.shape_padding) {
			return "image kind";
		}
		if (a.bindless != b.bindless) {
			return "image bindless";
		}
		if (a.indirect_root != b.indirect_root) {
			return "image table root";
		}
		if (a.indirect_search_iterations != b.indirect_search_iterations) {
			return "image search depth";
		}
		if (a.indirect_feedback_keys != b.indirect_feedback_keys) {
			return "image feedback width";
		}
		return "image table offset";
	}
	return nullptr;
}

namespace {

bool SelectsOne(uint32_t swizzle) {
	for (uint32_t component = 0; component < 4; component++) {
		if (GetDstSel(swizzle, component) == 1u) {
			return true;
		}
	}
	return false;
}

// Out of bounds, a read is zero except where dst_sel names the constant one.
bool UnboundBufferServes(const ResourceSpecialization::Buffer& built,
                         const ResourceSpecialization::Buffer& wanted) {
	auto shape               = wanted;
	shape.packed_stride      = built.packed_stride;
	shape.descriptor_format  = built.descriptor_format;
	shape.descriptor_swizzle = built.descriptor_swizzle;
	shape.zero_stride_oob    = built.zero_stride_oob;
	return shape == built && (built == wanted || (!SelectsOne(built.descriptor_swizzle) &&
	                                              !SelectsOne(wanted.descriptor_swizzle)));
}

bool UnboundImageServes(const ResourceSpecialization::Image& built,
                        const ResourceSpecialization::Image& wanted) {
	auto shape      = wanted;
	shape.dimension = built.dimension;
	return shape == built && built.dimension != Decoder::ImageDimension::Unknown &&
	       built.dimension != Decoder::ImageDimension::Dim2DMsaa &&
	       built.dimension != Decoder::ImageDimension::Dim2DMsaaArray;
}

bool Unbound(const std::vector<bool>& flags, size_t index) {
	return index < flags.size() && flags[index];
}

} // namespace

bool SpecializationServes(const ResourceSpecialization& built,
                          const ResourceSpecialization& wanted) {
	if (built.buffers.size() != wanted.buffers.size() ||
	    built.images.size() != wanted.images.size()) {
		return false;
	}
	for (size_t index = 0; index < wanted.buffers.size(); index++) {
		if (!(Unbound(wanted.unbound_buffers, index)
		          ? UnboundBufferServes(built.buffers[index], wanted.buffers[index])
		          : built.buffers[index] == wanted.buffers[index])) {
			return false;
		}
	}
	for (size_t index = 0; index < wanted.images.size(); index++) {
		if (!(Unbound(wanted.unbound_images, index)
		          ? UnboundImageServes(built.images[index], wanted.images[index])
		          : built.images[index] == wanted.images[index])) {
			return false;
		}
	}
	return true;
}

void InheritUnboundShapes(std::span<const ResourceSpecialization* const> donors,
                          ResourceSpecialization&                        wanted) {
	for (auto donor = donors.rbegin(); donor != donors.rend(); ++donor) {
		const auto& from = **donor;
		if (from.buffers.size() != wanted.buffers.size() ||
		    from.images.size() != wanted.images.size()) {
			continue;
		}
		for (size_t index = 0; index < wanted.buffers.size(); index++) {
			if (Unbound(wanted.unbound_buffers, index) && !Unbound(from.unbound_buffers, index) &&
			    UnboundBufferServes(from.buffers[index], wanted.buffers[index])) {
				wanted.buffers[index]         = from.buffers[index];
				wanted.unbound_buffers[index] = false;
			}
		}
		for (size_t index = 0; index < wanted.images.size(); index++) {
			if (Unbound(wanted.unbound_images, index) && !Unbound(from.unbound_images, index) &&
			    UnboundImageServes(from.images[index], wanted.images[index])) {
				wanted.images[index]         = from.images[index];
				wanted.unbound_images[index] = false;
			}
		}
	}
}

void AddObservedIndirectKeys(uint64_t signature, std::span<const uint32_t> keys) {
	if (!keys.empty()) {
		ObservedKeys().Add(signature, keys);
	}
}

bool ApplyResourceSpecialization(Program& program, const ResourceSpecialization& specialization) {
	EXIT_IF(!program.resource_tracking_complete || program.shader_info_complete);
	// Not a fault: a permutation can be keyed on a specialization that came from a different
	// tracking of the same shader, and the caller refuses it rather than dying on it.
	if (program.info.buffers.size() > specialization.buffers.size() ||
	    program.info.images.size() > specialization.images.size()) {
		return false;
	}
	for (const auto* block: program.blocks) {
		for (const auto& inst: *block) {
			if (ImageOpcodeInfoOf(inst.GetOpcode()).access == ImageAccess::None) {
				continue;
			}
			const auto index = inst.Flags<MemoryFlags>().index;
			if (index < program.memory_info.size() &&
			    BindlessServesOp(inst.GetOpcode(), program.memory_info[index])) {
				continue;
			}
			if (index < program.memory_info.size() &&
			    program.memory_info[index].resource < specialization.images.size() &&
			    specialization.images[program.memory_info[index].resource].bindless) {
				return false;
			}
		}
	}

	// Buffers grow the same way images do: one child per record the expanded table can select.
	auto&      buffers               = program.info.buffers;
	const auto original_buffer_count = buffers.size();
	buffers.reserve(specialization.buffers.size());
	for (uint32_t index = 0; index < specialization.buffers.size(); index++) {
		const auto& source = specialization.buffers[index];
		if (index >= buffers.size()) {
			if (source.indirect_root >= original_buffer_count) {
				return false;
			}
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
	for (uint32_t index = 0; index < buffers.size(); index++) {
		const auto root = buffers[index].indirect_root;
		if (root != BufferResource::NoIndirectBuffer) {
			EXIT_IF(root >= buffers.size());
			// The root names itself first, so ordinal 0 is the arm the search falls to on no match.
			buffers[root].indirect_resources.push_back(index);
		}
	}
	auto&      images               = program.info.images;
	const auto original_image_count = images.size();
	images.reserve(specialization.images.size());
	for (uint32_t index = 0; index < specialization.images.size(); index++) {
		const auto& source = specialization.images[index];
		if (index >= images.size()) {
			if (source.indirect_root >= original_image_count) {
				return false;
			}
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
		image.bindless                   = source.bindless;
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
	EXIT_IF(!BuildSamplerPlan(program.info, sampler_plan));
	auto&      samplers               = program.info.samplers;
	auto&      sampled_pairs          = program.info.sampled_pairs;
	const auto original_sampler_count = samplers.size();
	samplers.reserve(sampler_plan.sampler_count);
	for (uint32_t index = 0; index < sampler_plan.sampler_count; index++) {
		const auto& binding = sampler_plan.bindings[index];
		if (index >= original_sampler_count) {
			samplers.push_back(samplers[binding.source]);
		}
		samplers[index].snapshot_index        = binding.source;
		samplers[index].force_point_filtering = binding.type == SamplerClass::PointInteger;
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
		pair.sampler    = sampler_plan.mapping[pair.sampler][type];
		EXIT_IF(pair.sampler == UINT32_MAX);
		samplers[pair.sampler].depth_compare |= images[pair.image].depth_compare;
		for (const auto candidate: samplers[pair.sampler].indirect_resources)
			samplers[candidate].depth_compare |= images[pair.image].depth_compare;
	}

	auto&            memory_info = program.memory_info;
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
				// An expanded table's root and a dynamic V# are placeholders, not the descriptor
				// read.
				const bool runtime_descriptor =
				    memory.dynamic_buffer ||
				    (memory.resource < buffers.size() &&
				     buffers[memory.resource].indirect_root == memory.resource &&
				     buffers[memory.resource].indirect_resources.size() >= 2u);
				if (memory.kind == ResourceKind::Buffer && !runtime_descriptor &&
				    specialization.buffers[memory.resource].zero_stride_oob) {
					// Bounds mode 0 checks offset >= stride, so zero stride
					// makes every vector read out of bounds regardless of its address.
					const auto           count = BufferComponentCount(inst.GetOpcode());
					std::array<Value, 4> values {Value(0u), Value(0u), Value(0u), Value(0u)};
					if (memory.formatted && !memory.typed) {
						const auto& buffer = buffers[memory.resource];
						const auto  format = Format::GetFormatInfo(buffer.descriptor_format);
						for (uint32_t component = 0; component < count; component++) {
							if (format.type == Format::ComponentType::Unknown ||
							    GetDstSel(buffer.descriptor_swizzle, component) != 1u)
								continue;
							const auto one = Format::FormattedConstantBits(
							    format, Format::FormattedSourceKind::One);
							values[component] = Value(&*block->PrependNewInst(
							    it, ValueOpcode::SelectU32,
							    {inst.Arg(inst.NumArgs() - 1), Value(one), Value(0u)}));
						}
					}
					Value result = values[0];
					switch (inst.GetType()) {
						case Type::U8: result = Value(uint8_t {0}); break;
						case Type::U16: result = Value(uint16_t {0}); break;
						case Type::U32x2:
							result = Value(&*block->PrependNewInst(
							    it, ValueOpcode::CompositeConstructU32x2, {values[0], values[1]}));
							break;
						case Type::U32x3:
							result = Value(
							    &*block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x3,
							                            {values[0], values[1], values[2]}));
							break;
						case Type::U32x4:
							result = Value(&*block->PrependNewInst(
							    it, ValueOpcode::CompositeConstructU32x4,
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
				constexpr uint32_t   indices[] = {0x76543210u, 0xfedcba98u};
				std::array<Value, 2> fragments;
				for (uint32_t component = 0; component < fragments.size(); component++) {
					const auto selected =
					    block->PrependNewInst(it, ValueOpcode::SelectU32,
					                          {inst.Arg(2), Value(indices[component]), Value(0u)});
					fragments[component] = Value(&*selected);
				}
				const auto result =
				    block->PrependNewInst(it, ValueOpcode::CompositeConstructU32x4,
				                          {fragments[0], fragments[1], Value(0u), Value(0u)});
				inst.ReplaceUsesWith(Value(&*result));
				continue;
			}
			if (image_opcode.needs_sampler && memory.sampler < original_sampler_count) {
				const auto type = static_cast<uint32_t>(ClassifySampler(image));
				memory.sampler  = sampler_plan.mapping[memory.sampler][type];
				EXIT_IF(memory.sampler == UINT32_MAX);
			}
			EXIT_IF(image.indirect_root == memory.resource &&
			        inst.GetOpcode() != ValueOpcode::ImageSampleRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageGatherRaw &&
			        inst.GetOpcode() != ValueOpcode::ImageRead &&
			        !(image.bindless && BindlessReads(inst.GetOpcode())));
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
	return true;
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
