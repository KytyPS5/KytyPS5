#include "graphics/shader/recompiler/frontend/decode/ImageOps.h"

#include "graphics/shader/recompiler/frontend/decode/OpcodeTable.h"

#include <algorithm>

namespace Libs::Graphics::ShaderRecompiler::Decoder {
namespace {

struct MimgSampleInfo {
	uint32_t    encoding = 0;
	const char* name     = nullptr;
	uint32_t    flags    = 0;
};

struct MimgGatherInfo {
	uint32_t encoding = 0;
	Opcode   decoded  = Opcode::UNSUPPORTED;
	uint32_t flags    = 0;
};

constexpr ImageDimension DecodeImageDimension(uint32_t dim) {
	switch (dim) {
		case 0u: return ImageDimension::Dim1D;
		case 1u: return ImageDimension::Dim2D;
		case 2u: return ImageDimension::Dim3D;
		case 3u: return ImageDimension::Dim2DArray;
		case 4u: return ImageDimension::Dim1DArray;
		case 5u: return ImageDimension::Dim2DArray;
		case 6u: return ImageDimension::Dim2DMsaa;
		case 7u: return ImageDimension::Dim2DMsaaArray;
		default: return ImageDimension::Unknown;
	}
}

constexpr uint32_t ImageCoordComponents(ImageDimension dimension) {
	switch (dimension) {
		case ImageDimension::Dim1D: return 1u;
		case ImageDimension::Dim1DArray: return 2u;
		case ImageDimension::Dim2DMsaa:
		case ImageDimension::Dim3D:
		case ImageDimension::Dim2DArray: return 3u;
		case ImageDimension::Dim2DMsaaArray: return 4u;
		default: return 2u;
	}
}

constexpr uint32_t ImageGradientComponents(ImageDimension dimension) {
	switch (dimension) {
		case ImageDimension::Dim1D:
		case ImageDimension::Dim1DArray: return 1u;
		case ImageDimension::Dim3D: return 3u;
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
		components += ImageGradientComponents(dimension) * 2u;
	}
	return components;
}

constexpr MimgSampleInfo MIMG_SAMPLE_OPCODE_LIST[] = {
    {0x20u, "image_sample", 0},
    {0x21u, "image_sample_cl", ImageSampleFlagLodClamp},
    {0x22u, "image_sample_d", ImageSampleFlagDerivative},
    {0x23u, "image_sample_d_cl", ImageSampleFlagDerivative | ImageSampleFlagLodClamp},
    {0x24u, "image_sample_l", ImageSampleFlagLod},
    {0x25u, "image_sample_b", ImageSampleFlagBias},
    {0x26u, "image_sample_b_cl", ImageSampleFlagBias | ImageSampleFlagLodClamp},
    {0x27u, "image_sample_lz", ImageSampleFlagLevelZero},
    {0x28u, "image_sample_c", ImageSampleFlagCompare},
    {0x29u, "image_sample_c_cl", ImageSampleFlagCompare | ImageSampleFlagLodClamp},
    {0x2au, "image_sample_c_d", ImageSampleFlagCompare | ImageSampleFlagDerivative},
    {0x2bu, "image_sample_c_d_cl",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagLodClamp},
    {0x2cu, "image_sample_c_l", ImageSampleFlagCompare | ImageSampleFlagLod},
    {0x2du, "image_sample_c_b", ImageSampleFlagCompare | ImageSampleFlagBias},
    {0x2eu, "image_sample_c_b_cl",
     ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp},
    {0x2fu, "image_sample_c_lz", ImageSampleFlagCompare | ImageSampleFlagLevelZero},
    {0x30u, "image_sample_o", ImageSampleFlagOffset},
    {0x31u, "image_sample_cl_o", ImageSampleFlagLodClamp | ImageSampleFlagOffset},
    {0x32u, "image_sample_d_o", ImageSampleFlagDerivative | ImageSampleFlagOffset},
    {0x33u, "image_sample_d_cl_o",
     ImageSampleFlagDerivative | ImageSampleFlagLodClamp | ImageSampleFlagOffset},
    {0x34u, "image_sample_l_o", ImageSampleFlagLod | ImageSampleFlagOffset},
    {0x35u, "image_sample_b_o", ImageSampleFlagBias | ImageSampleFlagOffset},
    {0x36u, "image_sample_b_cl_o",
     ImageSampleFlagBias | ImageSampleFlagLodClamp | ImageSampleFlagOffset},
    {0x37u, "image_sample_lz_o", ImageSampleFlagLevelZero | ImageSampleFlagOffset},
    {0x38u, "image_sample_c_o", ImageSampleFlagCompare | ImageSampleFlagOffset},
    {0x39u, "image_sample_c_cl_o",
     ImageSampleFlagCompare | ImageSampleFlagLodClamp | ImageSampleFlagOffset},
    {0x3au, "image_sample_c_d_o",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagOffset},
    {0x3bu, "image_sample_c_d_cl_o",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagLodClamp |
         ImageSampleFlagOffset},
    {0x3cu, "image_sample_c_l_o",
     ImageSampleFlagCompare | ImageSampleFlagLod | ImageSampleFlagOffset},
    {0x3du, "image_sample_c_b_o",
     ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagOffset},
    {0x3eu, "image_sample_c_b_cl_o",
     ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp |
         ImageSampleFlagOffset},
    {0x3fu, "image_sample_c_lz_o",
     ImageSampleFlagCompare | ImageSampleFlagLevelZero | ImageSampleFlagOffset},
    {0x68u, "image_sample_cd", ImageSampleFlagDerivative | ImageSampleFlagCd},
    {0x69u, "image_sample_cd_cl",
     ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagLodClamp},
    {0x6au, "image_sample_c_cd",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd},
    {0x6bu, "image_sample_c_cd_cl",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
         ImageSampleFlagLodClamp},
    {0x6cu, "image_sample_cd_o",
     ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagOffset},
    {0x6du, "image_sample_cd_cl_o",
     ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagLodClamp |
         ImageSampleFlagOffset},
    {0x6eu, "image_sample_c_cd_o",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
         ImageSampleFlagOffset},
    {0x6fu, "image_sample_c_cd_cl_o",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
         ImageSampleFlagLodClamp | ImageSampleFlagOffset},
    {0xa0u, "image_sample_a", ImageSampleFlagAdjust},
    {0xa1u, "image_sample_cl_a", ImageSampleFlagLodClamp | ImageSampleFlagAdjust},
    {0xa5u, "image_sample_b_a", ImageSampleFlagBias | ImageSampleFlagAdjust},
    {0xa6u, "image_sample_b_cl_a",
     ImageSampleFlagBias | ImageSampleFlagLodClamp | ImageSampleFlagAdjust},
    {0xa8u, "image_sample_c_a", ImageSampleFlagCompare | ImageSampleFlagAdjust},
    {0xa9u, "image_sample_c_cl_a",
     ImageSampleFlagCompare | ImageSampleFlagLodClamp | ImageSampleFlagAdjust},
    {0xadu, "image_sample_c_b_a",
     ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagAdjust},
    {0xaeu, "image_sample_c_b_cl_a",
     ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp |
         ImageSampleFlagAdjust},
    {0xb0u, "image_sample_o_a", ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xb1u, "image_sample_cl_o_a",
     ImageSampleFlagLodClamp | ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xb5u, "image_sample_b_o_a",
     ImageSampleFlagBias | ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xb6u, "image_sample_b_cl_o_a",
     ImageSampleFlagBias | ImageSampleFlagLodClamp | ImageSampleFlagOffset |
         ImageSampleFlagAdjust},
    {0xb8u, "image_sample_c_o_a",
     ImageSampleFlagCompare | ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xb9u, "image_sample_c_cl_o_a",
     ImageSampleFlagCompare | ImageSampleFlagLodClamp | ImageSampleFlagOffset |
         ImageSampleFlagAdjust},
    {0xbdu, "image_sample_c_b_o_a",
     ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagOffset |
         ImageSampleFlagAdjust},
    {0xbeu, "image_sample_c_b_cl_o_a",
     ImageSampleFlagCompare | ImageSampleFlagBias | ImageSampleFlagLodClamp |
         ImageSampleFlagOffset | ImageSampleFlagAdjust},
    // PlayStation gradient-adjust ("_a") derivative / coarse-derivative variants. Team Asobi
    // shaders (Astro Bot PPSA21564) emit these; they route through the same explicit-LOD sample
    // path as the plain _d / _cd ops, with Adjust only steering sampler dword3 canonicalization.
    {0xa2u, "image_sample_d_a", ImageSampleFlagDerivative | ImageSampleFlagAdjust},
    {0xa3u, "image_sample_d_cl_a",
     ImageSampleFlagDerivative | ImageSampleFlagLodClamp | ImageSampleFlagAdjust},
    {0xaau, "image_sample_c_d_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagAdjust},
    {0xabu, "image_sample_c_d_cl_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagLodClamp |
         ImageSampleFlagAdjust},
    {0xb2u, "image_sample_d_o_a",
     ImageSampleFlagDerivative | ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xb3u, "image_sample_d_cl_o_a",
     ImageSampleFlagDerivative | ImageSampleFlagLodClamp | ImageSampleFlagOffset |
         ImageSampleFlagAdjust},
    {0xbau, "image_sample_c_d_o_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagOffset |
         ImageSampleFlagAdjust},
    {0xbbu, "image_sample_c_d_cl_o_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagLodClamp |
         ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xe8u, "image_sample_cd_a",
     ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagAdjust},
    {0xe9u, "image_sample_cd_cl_a",
     ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagLodClamp |
         ImageSampleFlagAdjust},
    {0xeau, "image_sample_c_cd_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
         ImageSampleFlagAdjust},
    {0xebu, "image_sample_c_cd_cl_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
         ImageSampleFlagLodClamp | ImageSampleFlagAdjust},
    {0xecu, "image_sample_cd_o_a",
     ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagOffset |
         ImageSampleFlagAdjust},
    {0xedu, "image_sample_cd_cl_o_a",
     ImageSampleFlagDerivative | ImageSampleFlagCd | ImageSampleFlagLodClamp |
         ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xeeu, "image_sample_c_cd_o_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
         ImageSampleFlagOffset | ImageSampleFlagAdjust},
    {0xefu, "image_sample_c_cd_cl_o_a",
     ImageSampleFlagCompare | ImageSampleFlagDerivative | ImageSampleFlagCd |
         ImageSampleFlagLodClamp | ImageSampleFlagOffset | ImageSampleFlagAdjust},
};

constexpr MimgGatherInfo MIMG_GATHER_OPCODE_LIST[] = {
    {0x47u, Opcode::IMAGE_GATHER4_LZ, ImageSampleFlagLevelZero},
    {0x48u, Opcode::IMAGE_GATHER4_C, ImageSampleFlagCompare},
    {0x4fu, Opcode::IMAGE_GATHER4_C_LZ,
     ImageSampleFlagCompare | ImageSampleFlagLevelZero},
    {0x57u, Opcode::IMAGE_GATHER4_LZ_O,
     ImageSampleFlagLevelZero | ImageSampleFlagOffset},
    {0x58u, Opcode::IMAGE_GATHER4_C_O,
     ImageSampleFlagCompare | ImageSampleFlagOffset},
    {0x5fu, Opcode::IMAGE_GATHER4_C_LZ_O,
     ImageSampleFlagCompare | ImageSampleFlagLevelZero | ImageSampleFlagOffset},
    {0x61u, Opcode::IMAGE_GATHER4H, ImageSampleFlagGatherHorizontal},
};

constexpr Detail::OpcodeMap MIMG_ATOMIC_OPCODE_LIST[] = {
    {0x0fu, Opcode::IMAGE_ATOMIC_SWAP},
    {0x11u, Opcode::IMAGE_ATOMIC_ADD},
    {0x15u, Opcode::IMAGE_ATOMIC_UMIN},
    {0x17u, Opcode::IMAGE_ATOMIC_UMAX},
    {0x18u, Opcode::IMAGE_ATOMIC_AND},
    {0x19u, Opcode::IMAGE_ATOMIC_OR},
    {0x1au, Opcode::IMAGE_ATOMIC_XOR},
};

constexpr auto MIMG_SAMPLE_OPS = Detail::MakeOpcodeTable<0x100>(MIMG_SAMPLE_OPCODE_LIST);
constexpr auto MIMG_GATHER_OPS = Detail::MakeOpcodeTable<0x100>(MIMG_GATHER_OPCODE_LIST);
constexpr auto MIMG_ATOMIC_OPS = Detail::MakeOpcodeTable<0x100>(MIMG_ATOMIC_OPCODE_LIST);

const MimgSampleInfo* LookupSample(uint32_t opcode) {
	return Detail::FindOpcode(MIMG_SAMPLE_OPS, opcode);
}

const MimgGatherInfo* LookupGather(uint32_t opcode) {
	return Detail::FindOpcode(MIMG_GATHER_OPS, opcode);
}

const Detail::OpcodeMap* LookupAtomic(uint32_t opcode) {
	return Detail::FindOpcode(MIMG_ATOMIC_OPS, opcode);
}

Opcode DecodeMimgOpcode(uint32_t opcode, const MimgSampleInfo* sample, const MimgGatherInfo* gather,
                        const Detail::OpcodeMap* atomic) {
	if (sample != nullptr) {
		return Opcode::IMAGE_SAMPLE;
	}
	if (gather != nullptr) {
		return gather->decoded;
	}
	if (atomic != nullptr) {
		return atomic->decoded;
	}

	switch (opcode) {
		case 0x00u: return Opcode::IMAGE_LOAD;
		case 0x01u: return Opcode::IMAGE_LOAD_MIP;
		case 0x08u: return Opcode::IMAGE_STORE;
		case 0x09u: return Opcode::IMAGE_STORE_MIP;
		case 0x0eu: return Opcode::IMAGE_GET_RESINFO;
		case 0x60u: return Opcode::IMAGE_GET_LOD;
		// Ray tracing (ISA 8.2.10): opcode 230 takes a 32-bit BVH node pointer, 231 a 64-bit
		// one. Neither uses a sampler, and both are encoded with R128=1 / DIM=0 / DMASK=0xf.
		case 0xe6u: return Opcode::IMAGE_BVH_INTERSECT_RAY;
		case 0xe7u: return Opcode::IMAGE_BVH64_INTERSECT_RAY;
		default: return Opcode::UNSUPPORTED;
	}
}

uint32_t DecodeMimgSampleFlags(const MimgSampleInfo* sample, const MimgGatherInfo* gather) {
	if (sample != nullptr) {
		return sample->flags;
	}
	if (gather != nullptr) {
		return gather->flags;
	}
	return 0;
}

uint32_t DecodeMimgAddressComponents(uint32_t opcode, ImageDimension dimension,
                                     const MimgSampleInfo* sample, const MimgGatherInfo* gather,
                                     const Detail::OpcodeMap* atomic, bool a16) {
	// Ray tracing (ISA Table 48). The address registers carry the node pointer, ray extent,
	// origin, direction and inverse direction; A16 packs direction and inverse direction into
	// halves, which drops three registers. The 64-bit variant spends one more on the pointer.
	if (opcode == 0xe6u) {
		return a16 ? 8u : 11u;
	}
	if (opcode == 0xe7u) {
		return a16 ? 9u : 12u;
	}
	if (sample != nullptr) {
		return ImageSampleAddressComponents(sample->flags, dimension);
	}
	if (gather != nullptr) {
		return ImageSampleAddressComponents(gather->flags, dimension);
	}
	if (atomic != nullptr) {
		return ImageCoordComponents(dimension);
	}

	switch (opcode) {
		case 0x0eu: return 1u;
		case 0x01u:
		case 0x09u: return ImageCoordComponents(dimension) + 1u;
		case 0x00u:
		case 0x08u:
		case 0x60u: return ImageCoordComponents(dimension);
		default: return 0;
	}
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

ImageAddressComponent ImageAddressComponentLayout(uint32_t flags, uint32_t component) {
	const auto width = [flags](uint32_t index) {
		if ((flags & ImageSampleFlagA16) == 0u) return 32u;
		uint32_t cursor = 0;
		if ((flags & ImageSampleFlagOffset) != 0u) {
			if (index == cursor++) return 32u;
		}
		if ((flags & ImageSampleFlagBias) != 0u) {
			if (index == cursor++) return 16u;
		}
		if ((flags & ImageSampleFlagCompare) != 0u && index == cursor) return 32u;
		return 16u;
	};
	uint32_t offset = 0;
	for (uint32_t index = 0; index < component; index++) {
		const auto bits = width(index);
		if (bits == 32u) offset = (offset + 31u) & ~31u;
		offset += bits;
	}
	const auto bits = width(component);
	if (bits == 32u) offset = (offset + 31u) & ~31u;
	return {offset, bits};
}

uint32_t ImageAddressDwordCount(uint32_t flags, uint32_t components) {
	if (components == 0u) return 0u;
	const auto last = ImageAddressComponentLayout(flags, components - 1u);
	return (last.bit_offset + last.bit_width + 31u) / 32u;
}

void DecodeMimg(uint32_t pc, std::span<const uint32_t> code, uint32_t word_index,
                Instruction& inst) {
	const uint32_t word0      = code[word_index];
	const uint32_t word1      = code[word_index + 1u];
	const uint32_t opcode     = ((word0 >> 18u) & 0x7fu) | ((word0 & 1u) << 7u);
	const uint32_t nsa_dwords = (word0 >> 1u) & 0x3u;
	const auto     dimension  = DecodeImageDimension((word0 >> 3u) & 0x7u);
	const uint32_t word_count = 2u + nsa_dwords;

	const uint32_t vdata  = (word1 >> 8u) & 0xffu;
	const uint32_t vaddr  = word1 & 0xffu;
	const uint32_t srsrc  = (word1 >> 16u) & 0x1fu;
	const uint32_t ssamp  = (word1 >> 21u) & 0x1fu;
	const bool     r128   = ((word0 >> 15u) & 0x1u) != 0u;
	const bool     a16    = ((word1 >> 30u) & 0x1u) != 0u;
	const bool     d16    = ((word1 >> 31u) & 0x1u) != 0u;
	const auto*    sample = LookupSample(opcode);
	const auto*    gather = LookupGather(opcode);
	const auto*    atomic = LookupAtomic(opcode);

	inst.pc                 = pc;
	inst.word_count         = word_count;
	inst.family             = Family::MIMG;
	inst.opcode_id          = opcode;
	inst.opcode             = DecodeMimgOpcode(opcode, sample, gather, atomic);
	inst.dmask              = (word0 >> 8u) & 0xfu;
	inst.data_components    = gather != nullptr ? 4u : CountDmaskComponents(inst.dmask);
	inst.data_bits          = d16 ? 16u : 32u;
	inst.data_dwords        = d16 ? (inst.data_components + 1u) / 2u : inst.data_components;
	inst.glc                = ((word0 >> 13u) & 1u) != 0;
	inst.slc                = ((word0 >> 25u) & 1u) != 0;
	inst.image_sample_flags = DecodeMimgSampleFlags(sample, gather);
	if (a16) {
		inst.image_sample_flags |= ImageSampleFlagA16;
	}
	inst.image_dimension  = dimension;
	inst.image_r128       = r128;
	inst.image_nsa_dwords = nsa_dwords;
	for (uint32_t i = 0; i < nsa_dwords * 4u; i++) {
		inst.image_nsa_addr[i] = (code[word_index + 2u + i / 4u] >> ((i % 4u) * 8u)) & 0xffu;
	}
	inst.image_address_components =
	    DecodeMimgAddressComponents(opcode, dimension, sample, gather, atomic, a16);
	SetRawWords(inst, code, word_index, word_count);

	if (inst.opcode == Opcode::UNSUPPORTED) {
		SetUnsupported(inst, Family::MIMG, opcode, "MIMG opcode is not implemented");
	}
	if (inst.opcode == Opcode::IMAGE_BVH_INTERSECT_RAY ||
	    inst.opcode == Opcode::IMAGE_BVH64_INTERSECT_RAY) {
		// Decoded so the operand shape is right (and so these stop masquerading as image
		// samples, which sent their 128-bit BVH T# through image-descriptor resolution and
		// dropped the whole dispatch). Traversal itself still needs the ray/box and
		// ray/triangle intersection emitted in SPIR-V; until then drop the shader honestly.
		SetUnsupported(inst, Family::MIMG, opcode,
		               "MIMG BVH ray intersection is not implemented");
	}
	if (gather != nullptr && !IsSingleDmaskBit(inst.dmask)) {
		SetUnsupported(inst, Family::MIMG, opcode,
		               "MIMG image gather requires exactly one dmask bit");
	}
	const bool supports_d16 = sample != nullptr || gather != nullptr || opcode == 0x00u ||
	                          opcode == 0x01u || opcode == 0x08u || opcode == 0x09u;
	if (d16 && !supports_d16) {
		SetUnsupported(inst, Family::MIMG, opcode, "MIMG opcode does not support D16 data");
	}

	DecodeVectorGpr(vdata, inst.dst);
	DecodeVectorGpr(vaddr, inst.src0);
	DecodeScalarSource(srsrc * 4u, pc, inst.src1);
	DecodeScalarSource(ssamp * 4u, pc, inst.src2);
	inst.src_count = 3;
}

const char* MimgSampleOpcodeName(uint32_t opcode) {
	const auto* sample = LookupSample(opcode);
	return sample != nullptr ? sample->name : nullptr;
}

} // namespace Libs::Graphics::ShaderRecompiler::Decoder
