#include "graphics/shader/recompiler/ImageOps.h"

#include <algorithm>
#include <fmt/format.h>
#include <iterator>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

struct MimgSampleInfo {
	uint32_t    opcode             = 0;
	const char* name               = nullptr;
	uint32_t    flags              = 0;
	uint32_t    address_components = 3;
};

struct MimgGatherInfo {
	uint32_t    opcode             = 0;
	const char* name               = nullptr;
	Opcode      decoded            = Opcode::Unsupported;
	uint32_t    flags              = 0;
	uint32_t    address_components = 3;
};

struct MimgAtomicInfo {
	uint32_t    opcode  = 0;
	const char* name    = nullptr;
	Opcode      decoded = Opcode::Unsupported;
};

constexpr ImageDimension DecodeImageDimension(uint32_t dim) {
	switch (dim) {
		case 1u: return ImageDimension::Dim2D;
		case 2u: return ImageDimension::Dim3D;
		case 3u: return ImageDimension::Dim2DArray;
		case 5u:
		case 7u: return ImageDimension::Dim2DArray;
		case 6u: return ImageDimension::Dim2D;
		default: return ImageDimension::Unknown;
	}
}

constexpr uint32_t ImageCoordComponents(ImageDimension dimension) {
	switch (dimension) {
		case ImageDimension::Dim3D:
		case ImageDimension::Dim2DArray: return 3u;
		default: return 2u;
	}
}

constexpr uint32_t ImageSampleAddressComponents(uint32_t flags, ImageDimension dimension) {
	const auto coord_components = ImageCoordComponents(dimension);
	uint32_t   components       = coord_components;
	if ((flags & ImageSampleFlagOffset) != 0) {
		components++;
	}
	if ((flags & ImageSampleFlagCompare) != 0) {
		components++;
	}
	if ((flags & ImageSampleFlagBias) != 0) {
		components++;
	}
	if ((flags & ImageSampleFlagLod) != 0) {
		components++;
	}
	if ((flags & ImageSampleFlagDerivative) != 0) {
		components += 4u;
	}
	return components;
}

constexpr MimgSampleInfo SampleInfo(uint32_t opcode, const char* name, uint32_t flags) {
	return {opcode, name, flags, ImageSampleAddressComponents(flags, ImageDimension::Dim2D)};
}

constexpr MimgSampleInfo MIMG_SAMPLE_OPS[] = {
    SampleInfo(0x20u, "image_sample", 0),
    SampleInfo(0x21u, "image_sample_cl", ImageSampleFlagLodClamp),
    SampleInfo(0x22u, "image_sample_d", ImageSampleFlagDerivative),
    SampleInfo(0x23u, "image_sample_d_cl", ImageSampleFlagDerivative | ImageSampleFlagLodClamp),
    SampleInfo(0x24u, "image_sample_l", ImageSampleFlagLod),
    SampleInfo(0x25u, "image_sample_b", ImageSampleFlagBias),
    SampleInfo(0x26u, "image_sample_b_cl", ImageSampleFlagBias | ImageSampleFlagLodClamp),
    SampleInfo(0x27u, "image_sample_lz", ImageSampleFlagLevelZero),
    SampleInfo(0x28u, "image_sample_c", ImageSampleFlagCompare),
    SampleInfo(0x29u, "image_sample_c_cl", ImageSampleFlagCompare | ImageSampleFlagLodClamp),
    SampleInfo(0x2au, "image_sample_c_d", ImageSampleFlagCompare | ImageSampleFlagDerivative),
    SampleInfo(0x2bu, "image_sample_c_d_cl",
               ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagLodClamp),
    SampleInfo(0x2cu, "image_sample_c_l", ImageSampleFlagCompare | ImageSampleFlagLod),
    SampleInfo(0x2du, "image_sample_c_b", ImageSampleFlagCompare | ImageSampleFlagBias),
    SampleInfo(0x2eu, "image_sample_c_b_cl",
               ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp),
    SampleInfo(0x2fu, "image_sample_c_lz", ImageSampleFlagCompare | ImageSampleFlagLevelZero),
    SampleInfo(0x30u, "image_sample_o", ImageSampleFlagOffset),
    SampleInfo(0x31u, "image_sample_cl_o", ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0x32u, "image_sample_d_o", ImageSampleFlagDerivative | ImageSampleFlagOffset),
    SampleInfo(0x33u, "image_sample_d_cl_o",
               ImageSampleFlagDerivative | ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0x34u, "image_sample_l_o", ImageSampleFlagLod | ImageSampleFlagOffset),
    SampleInfo(0x35u, "image_sample_b_o", ImageSampleFlagBias | ImageSampleFlagOffset),
    SampleInfo(0x36u, "image_sample_b_cl_o",
               ImageSampleFlagBias | ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0x37u, "image_sample_lz_o", ImageSampleFlagLevelZero | ImageSampleFlagOffset),
    SampleInfo(0x38u, "image_sample_c_o", ImageSampleFlagCompare | ImageSampleFlagOffset),
    SampleInfo(0x39u, "image_sample_c_cl_o",
               ImageSampleFlagCompare | ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0x3au, "image_sample_c_d_o",
               ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagOffset),
    SampleInfo(0x3bu, "image_sample_c_d_cl_o",
               ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagLodClamp |
                   ImageSampleFlagOffset),
    SampleInfo(0x3cu, "image_sample_c_l_o",
               ImageSampleFlagCompare | ImageSampleFlagLod | ImageSampleFlagOffset),
    SampleInfo(0x3du, "image_sample_c_b_o",
               ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagOffset),
    SampleInfo(0x3eu, "image_sample_c_b_cl_o",
               ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp |
                   ImageSampleFlagOffset),
    SampleInfo(0x3fu, "image_sample_c_lz_o",
               ImageSampleFlagCompare | ImageSampleFlagLevelZero | ImageSampleFlagOffset),
    SampleInfo(0x68u, "image_sample_cd", ImageSampleFlagDerivative | ImageSampleFlagCd),
    SampleInfo(0x69u, "image_sample_cd_cl",
               ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagLodClamp),
    SampleInfo(0x6au, "image_sample_c_cd",
               ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd),
    SampleInfo(0x6bu, "image_sample_c_cd_cl",
               ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
                   ImageSampleFlagLodClamp),
    SampleInfo(0x6cu, "image_sample_cd_o",
               ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagOffset),
    SampleInfo(0x6du, "image_sample_cd_cl_o",
               ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagLodClamp |
                   ImageSampleFlagOffset),
    SampleInfo(0x6eu, "image_sample_c_cd_o",
               ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
                   ImageSampleFlagOffset),
    SampleInfo(0x6fu, "image_sample_c_cd_cl_o",
               ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
                   ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0xa0u, "image_sample_a", 0),
    SampleInfo(0xa1u, "image_sample_cl_a", ImageSampleFlagLodClamp),
    SampleInfo(0xa5u, "image_sample_b_a", ImageSampleFlagBias),
    SampleInfo(0xa6u, "image_sample_b_cl_a", ImageSampleFlagBias | ImageSampleFlagLodClamp),
    SampleInfo(0xa8u, "image_sample_c_a", ImageSampleFlagCompare),
    SampleInfo(0xa9u, "image_sample_c_cl_a", ImageSampleFlagCompare | ImageSampleFlagLodClamp),
    SampleInfo(0xadu, "image_sample_c_b_a", ImageSampleFlagCompare | ImageSampleFlagBias),
    SampleInfo(0xaeu, "image_sample_c_b_cl_a",
               ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp),
    SampleInfo(0xb0u, "image_sample_o_a", ImageSampleFlagOffset),
    SampleInfo(0xb1u, "image_sample_cl_o_a", ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0xb5u, "image_sample_b_o_a", ImageSampleFlagBias | ImageSampleFlagOffset),
    SampleInfo(0xb6u, "image_sample_b_cl_o_a",
               ImageSampleFlagBias | ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0xb8u, "image_sample_c_o_a", ImageSampleFlagCompare | ImageSampleFlagOffset),
    SampleInfo(0xb9u, "image_sample_c_cl_o_a",
               ImageSampleFlagCompare | ImageSampleFlagLodClamp | ImageSampleFlagOffset),
    SampleInfo(0xbdu, "image_sample_c_b_o_a",
               ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagOffset),
    SampleInfo(0xbeu, "image_sample_c_b_cl_o_a",
               ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp |
                   ImageSampleFlagOffset),
};

constexpr MimgGatherInfo MIMG_GATHER_OPS[] = {
    {0x47u, "image_gather4_lz", Opcode::ImageGather4Lz, ImageSampleFlagLevelZero, 2u},
    {0x48u, "image_gather4_c", Opcode::ImageGather4C, ImageSampleFlagCompare, 3u},
    {0x4fu, "image_gather4_c_lz", Opcode::ImageGather4CLz,
     ImageSampleFlagCompare | ImageSampleFlagLevelZero, 3u},
    {0x57u, "image_gather4_lz_o", Opcode::ImageGather4LzO,
     ImageSampleFlagLevelZero | ImageSampleFlagOffset, 3u},
    {0x58u, "image_gather4_c_o", Opcode::ImageGather4CO,
     ImageSampleFlagCompare | ImageSampleFlagOffset, 4u},
    {0x5fu, "image_gather4_c_lz_o", Opcode::ImageGather4CLzO,
     ImageSampleFlagCompare | ImageSampleFlagLevelZero | ImageSampleFlagOffset, 4u},
    {0x61u, "image_gather4h", Opcode::ImageGather4H, ImageSampleFlagGatherHorizontal, 2u},
};

constexpr MimgAtomicInfo MIMG_ATOMIC_OPS[] = {
    {0x11u, "image_atomic_add", Opcode::ImageAtomicAdd},
    {0x15u, "image_atomic_umin", Opcode::ImageAtomicUMin},
    {0x17u, "image_atomic_umax", Opcode::ImageAtomicUMax},
    {0x18u, "image_atomic_and", Opcode::ImageAtomicAnd},
    {0x19u, "image_atomic_or", Opcode::ImageAtomicOr},
    {0x1au, "image_atomic_xor", Opcode::ImageAtomicXor},
};

const MimgSampleInfo* LookupSample(uint32_t opcode) {
	for (const auto& info: MIMG_SAMPLE_OPS) {
		if (info.opcode == opcode) {
			return &info;
		}
	}
	return nullptr;
}

const MimgGatherInfo* LookupGather(uint32_t opcode) {
	for (const auto& info: MIMG_GATHER_OPS) {
		if (info.opcode == opcode) {
			return &info;
		}
	}
	return nullptr;
}

const MimgAtomicInfo* LookupAtomic(uint32_t opcode) {
	for (const auto& info: MIMG_ATOMIC_OPS) {
		if (info.opcode == opcode) {
			return &info;
		}
	}
	return nullptr;
}

uint32_t CountDmaskComponents(uint32_t dmask) {
	uint32_t count = 0;
	for (uint32_t i = 0; i < 4u; i++) {
		count += (dmask >> i) & 1u;
	}
	return count != 0 ? count : 1u;
}

bool IsSingleDmaskBit(uint32_t dmask) {
	return dmask != 0u && (dmask & (dmask - 1u)) == 0u;
}

} // namespace

bool DecodeMimg(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index, Instruction* inst,
                std::string* error) {
	if (word_index + 1u >= code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated MIMG instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t word0      = code[word_index];
	const uint32_t word1      = code[word_index + 1u];
	const uint32_t opcode     = ((word0 >> 18u) & 0x7fu) | ((word0 & 1u) << 7u);
	const uint32_t nsa_dwords = (word0 >> 1u) & 0x3u;
	const auto     dimension  = DecodeImageDimension((word0 >> 3u) & 0x7u);
	const uint32_t word_count = 2u + nsa_dwords;
	if (word_index + word_count > code.size()) {
		if (error != nullptr) {
			*error = fmt::format("truncated MIMG NSA instruction at pc 0x{:08x}", pc);
		}
		return false;
	}

	const uint32_t vdata  = (word1 >> 8u) & 0xffu;
	const uint32_t vaddr  = word1 & 0xffu;
	const uint32_t srsrc  = (word1 >> 16u) & 0x1fu;
	const uint32_t ssamp  = (word1 >> 21u) & 0x1fu;
	const bool     a16    = ((word1 >> 30u) & 0x1u) != 0u;
	const auto*    sample = LookupSample(opcode);
	const auto*    gather = LookupGather(opcode);
	const auto*    atomic = LookupAtomic(opcode);

	inst->pc                 = pc;
	inst->word               = word0;
	inst->word_count         = word_count;
	inst->family             = Family::MIMG;
	inst->opcode_id          = opcode;
	inst->opcode             = (sample != nullptr   ? Opcode::ImageSample
	                            : gather != nullptr ? gather->decoded
	                            : atomic != nullptr ? atomic->decoded
	                            : opcode == 0x00u   ? Opcode::ImageLoad
	                            : opcode == 0x01u   ? Opcode::ImageLoadMip
	                            : opcode == 0x08u   ? Opcode::ImageStore
	                            : opcode == 0x09u   ? Opcode::ImageStoreMip
	                            : opcode == 0x0eu   ? Opcode::ImageGetResinfo
	                            : opcode == 0x60u   ? Opcode::ImageGetLod
	                                                : Opcode::Unsupported);
	inst->dmask              = (word0 >> 8u) & 0xfu;
	inst->data_dwords        = gather != nullptr ? 4u : CountDmaskComponents(inst->dmask);
	inst->glc                = ((word0 >> 13u) & 1u) != 0;
	inst->slc                = ((word0 >> 25u) & 1u) != 0;
	inst->image_sample_flags = (sample != nullptr   ? sample->flags
	                            : gather != nullptr ? gather->flags
	                                                : 0u) |
	                           (a16 ? ImageSampleFlagA16 : 0u);
	inst->image_dimension    = dimension;
	inst->image_nsa_dwords   = nsa_dwords;
	for (uint32_t i = 0; i < nsa_dwords * 4u && i < MaxImageNsaAddressComponents; i++) {
		inst->image_nsa_addr[i] = (code[word_index + 2u + i / 4u] >> ((i % 4u) * 8u)) & 0xffu;
	}
	inst->image_address_components =
	    sample != nullptr   ? ImageSampleAddressComponents(sample->flags, dimension)
	    : gather != nullptr ? ImageSampleAddressComponents(gather->flags, dimension)
	    : atomic != nullptr ? 3u
	    : opcode == 0x60u   ? ImageCoordComponents(dimension)
	    : opcode == 0x01u   ? ImageCoordComponents(dimension) + 1u
	    : opcode == 0x00u   ? ImageCoordComponents(dimension)
	                        : (opcode == 0x09u   ? ImageCoordComponents(dimension) + 1u
	                           : opcode == 0x08u ? ImageCoordComponents(dimension)
	                                             : 0u);
	SetRawWords(inst, code, word_index, word_count);

	if (inst->opcode == Opcode::Unsupported) {
		SetUnsupported(inst, Family::MIMG, opcode, "MIMG opcode is not implemented");
	}
	if (gather != nullptr && !IsSingleDmaskBit(inst->dmask)) {
		SetUnsupported(inst, Family::MIMG, opcode,
		               "MIMG image gather requires exactly one dmask bit");
	}

	DecodeVectorGpr(vdata, &inst->dst, nullptr);
	DecodeVectorGpr(vaddr, &inst->src0, nullptr);
	DecodeScalarSource(srsrc * 4u, pc, &inst->src1, nullptr);
	DecodeScalarSource(ssamp * 4u, pc, &inst->src2, nullptr);
	inst->src_count = 3;
	return true;
}

const char* MimgSampleOpcodeName(uint32_t opcode) {
	const auto* sample = LookupSample(opcode);
	return sample != nullptr ? sample->name : nullptr;
}

uint32_t MimgSampleOpcodeFlags(uint32_t opcode) {
	const auto* sample = LookupSample(opcode);
	return sample != nullptr ? sample->flags : 0u;
}

uint32_t MimgSampleAddressComponents(uint32_t opcode) {
	const auto* sample = LookupSample(opcode);
	return sample != nullptr ? sample->address_components : 0u;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
