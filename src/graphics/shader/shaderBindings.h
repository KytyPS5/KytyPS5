#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERBINDINGS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERBINDINGS_H_

#include "common/abi.h"
#include "common/common.h"

namespace Libs::Graphics {

inline constexpr uint16_t AGC_ILLEGAL_DIRECT_OFFSET = 0xffff;

enum class AgcDirectResourceType : uint32_t {
	GdsCounterRange          = 0,
	ShaderResourceTable      = 1,
	SubPtrFetchShader        = 2,
	PtrSoBufferTable         = 3,
	PtrInternalGlobalTable   = 4,
	PtrExtendedUserData      = 5,
	GsFlags                  = 6,
	GdsMemoryRange           = 7,
	PtrVertexBufferTable     = 8,
	StereoXOffset            = 9,
	PtrVertexAttribDescTable = 10,
	Last                     = PtrVertexAttribDescTable,
};

enum class ResourceDescriptorType {
	Texture,
	Buffer,
	Sampler,
	Unused,
};

struct ShaderBufferResource {
	uint32_t fields[4] = {0};

	void UpdateAddress48(uint64_t gpu_addr) {
		auto lo   = static_cast<uint32_t>(gpu_addr & 0xffffffffu);
		auto hi   = static_cast<uint32_t>(gpu_addr >> 32u);
		fields[0] = lo;
		fields[1] = (fields[1] & 0xffff0000u) | (hi & 0x0000ffffu);
	}

	[[nodiscard]] uint16_t Stride() const { return (fields[1] >> 16u) & 0x3FFFu; }
	[[nodiscard]] bool     SwizzleEnabled() const { return ((fields[1] >> 31u) & 0x1u) == 1; }
	[[nodiscard]] uint32_t NumRecords() const { return fields[2]; }
	[[nodiscard]] uint8_t  DstSelX() const { return (fields[3] >> 0u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelY() const { return (fields[3] >> 3u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelZ() const { return (fields[3] >> 6u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelW() const { return (fields[3] >> 9u) & 0x7u; }
	[[nodiscard]] uint32_t DstSelXY() const { return (fields[3] >> 0u) & 0x3Fu; }
	[[nodiscard]] uint32_t DstSelXYZ() const { return (fields[3] >> 0u) & 0x1FFu; }
	[[nodiscard]] uint32_t DstSelXYZW() const { return (fields[3] >> 0u) & 0xFFFu; }
	[[nodiscard]] bool     AddTid() const { return ((fields[3] >> 23u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  IndexStride() const { return (fields[3] >> 21u) & 0x3u; }
	[[nodiscard]] uint32_t PackedStride() const {
		return Stride() | (static_cast<uint32_t>(SwizzleEnabled()) << 14u) |
		       (static_cast<uint32_t>(IndexStride()) << 16u) |
		       (static_cast<uint32_t>(AddTid()) << 20u);
	}

	[[nodiscard]] uint64_t Base48() const {
		return (fields[0] | (static_cast<uint64_t>(fields[1]) << 32u)) & 0xFFFFFFFFFFFFu;
	}
	[[nodiscard]] uint8_t Format() const { return (fields[3] >> 12u) & 0x7Fu; }
	[[nodiscard]] uint8_t OutOfBounds() const { return (fields[3] >> 28u) & 0x3u; }
	[[nodiscard]] uint8_t Type() const { return (fields[3] >> 30u) & 0x3u; }
};

struct ShaderTextureResource {
	uint32_t fields[8] = {0};

	[[nodiscard]] uint16_t MinLod() const { return (fields[1] >> 8u) & 0xFFFu; }
	[[nodiscard]] uint8_t  DstSelX() const { return (fields[3] >> 0u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelY() const { return (fields[3] >> 3u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelZ() const { return (fields[3] >> 6u) & 0x7u; }
	[[nodiscard]] uint8_t  DstSelW() const { return (fields[3] >> 9u) & 0x7u; }
	[[nodiscard]] uint32_t DstSelXY() const { return (fields[3] >> 0u) & 0x3Fu; }
	[[nodiscard]] uint32_t DstSelXYZ() const { return (fields[3] >> 0u) & 0x1FFu; }
	[[nodiscard]] uint32_t DstSelXYZW() const { return (fields[3] >> 0u) & 0xFFFu; }
	[[nodiscard]] uint8_t  BaseLevel() const { return (fields[3] >> 12u) & 0xFu; }
	[[nodiscard]] uint8_t  LastLevel() const { return (fields[3] >> 16u) & 0xFu; }
	[[nodiscard]] uint8_t  TileMode() const { return (fields[3] >> 20u) & 0x1Fu; }
	[[nodiscard]] uint8_t  Type() const { return (fields[3] >> 28u) & 0xFu; }
	[[nodiscard]] uint16_t Depth() const { return (fields[4] >> 0u) & 0x1FFFu; }

	[[nodiscard]] uint64_t Base40() const {
		return ((fields[0] | (static_cast<uint64_t>(fields[1]) << 32u)) & 0xFFFFFFFFFFu) << 8u;
	}
	[[nodiscard]] bool     IsNull() const { return Base40() == 0; }
	[[nodiscard]] uint16_t Format() const { return (fields[1] >> 20u) & 0x1FFu; }
	[[nodiscard]] uint16_t Width5() const {
		return ((fields[1] >> 30u) & 0x3u) | (((fields[2] >> 0u) & 0xFFFu) << 2u);
	}
	[[nodiscard]] uint16_t Height5() const { return (fields[2] >> 14u) & 0x3FFFu; }
	[[nodiscard]] uint8_t  BCSwizzle() const { return (fields[3] >> 25u) & 0x7u; }
	[[nodiscard]] uint16_t BaseArray5() const { return (fields[4] >> 16u) & 0x1FFFu; }
	[[nodiscard]] uint8_t  ArrayPitch() const { return (fields[5] >> 0u) & 0xFu; }
	[[nodiscard]] uint8_t  MaxMip() const { return (fields[5] >> 4u) & 0xFu; }
	[[nodiscard]] uint16_t MinLodWarn5() const { return (fields[5] >> 8u) & 0xFFFu; }
	[[nodiscard]] uint8_t  PerfMod5() const { return (fields[5] >> 20u) & 0x7u; }
	[[nodiscard]] bool     CornerSample() const { return ((fields[5] >> 23u) & 0x1u) == 1; }
	[[nodiscard]] bool     MipStatsCntEn() const { return ((fields[5] >> 25u) & 0x1u) == 1; }
	[[nodiscard]] bool     PrtDefColor() const { return ((fields[5] >> 26u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  MipStatsCntId() const { return (fields[6] >> 0u) & 0xFFu; }
	[[nodiscard]] bool     MsaaDepth() const { return ((fields[6] >> 10u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  MaxUncompBlkSize() const { return (fields[6] >> 15u) & 0x3u; }
	[[nodiscard]] uint8_t  MaxCompBlkSize() const { return (fields[6] >> 17u) & 0x3u; }
	[[nodiscard]] bool     MetaPipeAligned() const { return ((fields[6] >> 19u) & 0x1u) == 1; }
	[[nodiscard]] bool     WriteCompress() const { return ((fields[6] >> 20u) & 0x1u) == 1; }
	[[nodiscard]] bool     MetaCompress() const { return ((fields[6] >> 21u) & 0x1u) == 1; }
	[[nodiscard]] bool     DccAlphaPos() const { return ((fields[6] >> 22u) & 0x1u) == 1; }
	[[nodiscard]] bool     DccColorTransf() const { return ((fields[6] >> 23u) & 0x1u) == 1; }
	[[nodiscard]] uint64_t MetaAddr() const {
		return ((fields[6] >> 24u) & 0xFFu) | (static_cast<uint64_t>(fields[7]) << 8u);
	}
};

struct ShaderSamplerResource {
	uint32_t fields[4] = {0};

	[[nodiscard]] uint8_t  ClampX() const { return (fields[0] >> 0u) & 0x7u; }
	[[nodiscard]] uint8_t  ClampY() const { return (fields[0] >> 3u) & 0x7u; }
	[[nodiscard]] uint8_t  ClampZ() const { return (fields[0] >> 6u) & 0x7u; }
	[[nodiscard]] uint8_t  MaxAnisoRatio() const { return (fields[0] >> 9u) & 0x7u; }
	[[nodiscard]] uint8_t  DepthCompareFunc() const { return (fields[0] >> 12u) & 0x7u; }
	[[nodiscard]] bool     ForceUnormCoords() const { return ((fields[0] >> 15u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  AnisoThreshold() const { return (fields[0] >> 16u) & 0x7u; }
	[[nodiscard]] bool     ForceSrgb() const { return ((fields[0] >> 20u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  AnisoBias() const { return (fields[0] >> 21u) & 0x3Fu; }
	[[nodiscard]] bool     TruncCoord() const { return ((fields[0] >> 27u) & 0x1u) == 1; }
	[[nodiscard]] bool     DisableCubeWrap() const { return ((fields[0] >> 28u) & 0x1u) == 1; }
	[[nodiscard]] uint8_t  FilterMode() const { return (fields[0] >> 29u) & 0x3u; }
	[[nodiscard]] uint16_t MinLod() const { return (fields[1] >> 0u) & 0xFFFu; }
	[[nodiscard]] uint16_t MaxLod() const { return (fields[1] >> 12u) & 0xFFFu; }
	[[nodiscard]] uint8_t  PerfMip() const { return (fields[1] >> 24u) & 0xFu; }
	[[nodiscard]] uint8_t  PerfZ() const { return (fields[1] >> 28u) & 0xFu; }
	[[nodiscard]] uint16_t LodBias() const { return (fields[2] >> 0u) & 0x3FFFu; }
	[[nodiscard]] uint8_t  LodBiasSec() const { return (fields[2] >> 14u) & 0x3Fu; }
	[[nodiscard]] uint8_t  XyMagFilter() const { return (fields[2] >> 20u) & 0x3u; }
	[[nodiscard]] uint8_t  XyMinFilter() const { return (fields[2] >> 22u) & 0x3u; }
	[[nodiscard]] uint8_t  ZFilter() const { return (fields[2] >> 24u) & 0x3u; }
	[[nodiscard]] uint8_t  MipFilter() const { return (fields[2] >> 26u) & 0x3u; }
	[[nodiscard]] uint16_t BorderColorPtr() const { return (fields[3] >> 0u) & 0xFFFu; }
	[[nodiscard]] uint8_t  BorderColorType() const { return (fields[3] >> 30u) & 0x3u; }

	[[nodiscard]] bool DisableDegamma() const { return ((fields[0] >> 31u) & 0x1u) == 1; }
	[[nodiscard]] bool PointPreclamp() const { return ((fields[2] >> 28u) & 0x1u) == 1; }
	[[nodiscard]] bool AnisoOverride() const { return ((fields[2] >> 29u) & 0x1u) == 1; }
	[[nodiscard]] bool BlendZeroPrt() const { return ((fields[2] >> 30u) & 0x1u) == 1; }
};

struct ShaderVertexInputBuffer {
	static constexpr int ATTR_MAX = 32;

	uint64_t addr                   = 0;
	uint32_t stride                 = 0;
	uint32_t num_records            = 0;
	uint32_t fetch_index            = 0;
	int      attr_num               = 0;
	int      attr_indices[ATTR_MAX] = {0};
	uint32_t attr_offsets[ATTR_MAX] = {0};
};

struct ShaderVertexDestination {
	int      register_start = 0;
	int      registers_num  = 0;
	int      attr_id        = -1;
	uint32_t fetch_index    = 0;
};

enum class ShaderStorageUsage {
	Unknown,
	Constant,
	ReadOnly,
	ReadWrite,
};

ResourceDescriptorType ShaderClassifyResourceDescriptor(const uint32_t* desc);

} // namespace Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERBINDINGS_H_ */
