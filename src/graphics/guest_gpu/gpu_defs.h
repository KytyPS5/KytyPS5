#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_DEFS_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_DEFS_H_

#include "common/common.h"

#include <type_traits>

namespace Libs::Graphics::Prospero {

template <typename T>
constexpr auto GpuEnumValue(T value) {
	return static_cast<std::underlying_type_t<T>>(value);
}

enum class PrimitiveType : uint32_t {
	kNone               = 0,
	kPointList          = 1,
	kLineList           = 2,
	kLineStrip          = 3,
	kTriList            = 4,
	kTriFan             = 5,
	kTriStrip           = 6,
	kRectList           = 7,
	kPatch              = 9,
	kLineListAdjacency  = 10,
	kLineStripAdjacency = 11,
	kTriListAdjacency   = 12,
	kTriStripAdjacency  = 13,
	kRectListLegacy     = 17,
	kLineLoop           = 18,
	kQuadListLegacy     = 19,
	kQuadStripLegacy    = 20,
	kPolygon            = 21,
};

enum class IndexType : uint32_t {
	kIndex16 = 0,
	kIndex32 = 1,
	kIndex8  = 2,
};

enum class ShaderBinaryType : uint8_t {
	kCs      = 0,
	kPs      = 1,
	kGs      = 2,
	kHs      = 3,
	kGsFront = 4,
	kHsFront = 5,
	kGsBack  = 6,
	kHsBack  = 7,
	kFs      = 8,
};

enum class GsOutputPrimitiveType : uint32_t {
	kPoints      = 0,
	kLines       = 1,
	kTriangles   = 2,
	k2dRectangle = 3,
	kRectList    = 4,
};

enum class ChannelLayout : uint32_t {
	kInvalid     = 0,
	k8           = 1,
	k16          = 2,
	k8_8         = 3,
	k32          = 4,
	k16_16       = 5,
	k11_11_10    = 6,
	k10_10_10_2  = 9,
	k8_8_8_8     = 10,
	k32_32       = 11,
	k16_16_16_16 = 12,
	k32_32_32_32 = 14,
	k5_5_5_1     = 17,
	k4_4_4_4     = 19,
	kBc1         = 35,
	kBc2         = 36,
	kBc3         = 37,
};

enum class ChannelType : uint32_t {
	kUNorm = 0,
	kSNorm = 1,
	kUInt  = 4,
	kSInt  = 5,
	kSrgb  = 6,
	kFloat = 7,
};

enum class NumberFormat : uint32_t {
	kUNorm = 0,
	kSNorm = 1,
	kUInt  = 4,
	kSInt  = 5,
	kFloat = 7,
	kSrgb  = 9,
};

enum class ChannelOrder : uint32_t {
	kStandard    = 0,
	kAlt         = 1,
	kReversed    = 2,
	kAltReversed = 3,
};

enum class DepthFormat : uint32_t {
	kInvalid = 0,
	kZ16     = 1,
	kZ32F    = 3,
};

enum class StencilFormat : uint32_t {
	kInvalid = 0,
	k8UInt   = 1,
};

enum class TileMode : uint32_t {
	kLinear       = 0x00,
	kStandard256B = 0x01,
	kStandard4KB  = 0x05,
	kStandard64KB = 0x09,
	kPrt          = 0x11,
	kDepth        = 0x18,
	kRenderTarget = 0x1b,
};

enum class StencilOp : uint32_t {
	kKeep        = 0x00,
	kZero        = 0x01,
	kOnes        = 0x02,
	kReplaceTest = 0x03,
	kReplaceOp   = 0x04,
	kAddClamp    = 0x05,
	kSubClamp    = 0x06,
	kInvert      = 0x07,
	kAddWrap     = 0x08,
	kSubWrap     = 0x09,
	kAnd         = 0x0a,
	kOr          = 0x0b,
	kXor         = 0x0c,
	kNand        = 0x0d,
	kNor         = 0x0e,
	kXnor        = 0x0f,
};

enum class BlendFactor : uint32_t {
	kZero                  = 0x00,
	kOne                   = 0x01,
	kSrcColor              = 0x02,
	kOneMinusSrcColor      = 0x03,
	kSrcAlpha              = 0x04,
	kOneMinusSrcAlpha      = 0x05,
	kDstAlpha              = 0x06,
	kOneMinusDstAlpha      = 0x07,
	kDstColor              = 0x08,
	kOneMinusDstColor      = 0x09,
	kSrcAlphaSaturate      = 0x0a,
	kConstantColor         = 0x0d,
	kOneMinusConstantColor = 0x0e,
	kSrc1Color             = 0x0f,
	kOneMinusSrc1Color     = 0x10,
	kSrc1Alpha             = 0x11,
	kOneMinusSrc1Alpha     = 0x12,
	kConstantAlpha         = 0x13,
	kOneMinusConstantAlpha = 0x14,
};

enum class BlendOp : uint32_t {
	kAdd             = 0x00,
	kSubtract        = 0x01,
	kMin             = 0x02,
	kMax             = 0x03,
	kReverseSubtract = 0x04,
};

enum class CompSwizzle : uint32_t {
	kZero  = 0,
	kOne   = 1,
	kRed   = 4,
	kGreen = 5,
	kBlue  = 6,
	kAlpha = 7,
};

enum class SamplerClampMode : uint32_t {
	kWrap                 = 0,
	kMirror               = 1,
	kClampLastTexel       = 2,
	kMirrorOnceLastTexel  = 3,
	kClampHalfBorder      = 4,
	kMirrorOnceHalfBorder = 5,
	kClampBorder          = 6,
	kMirrorOnceBorder     = 7,
};

enum class SamplerAnisoRatio : uint32_t {
	kOne     = 0,
	kTwo     = 1,
	kFour    = 2,
	kEight   = 3,
	kSixteen = 4,
};

enum class SamplerBorderColor : uint32_t {
	kTransBlack  = 0,
	kOpaqueBlack = 1,
	kOpaqueWhite = 2,
	kFromTable   = 3,
};

enum class SamplerFilter : uint32_t {
	kPoint       = 0,
	kBilinear    = 1,
	kAnisoPoint  = 2,
	kAnisoLinear = 3,
};

enum class SamplerMipFilter : uint32_t {
	kNone   = 0,
	kPoint  = 1,
	kLinear = 2,
};

enum class ImageType : uint32_t {
	kColor1D          = 8,
	kColor2D          = 9,
	kColor3D          = 10,
	kCube             = 11,
	kColor1DArray     = 12,
	kColor2DArray     = 13,
	kColor2DMsaa      = 14,
	kColor2DMsaaArray = 15,
};

enum class DescriptorKind : uint32_t {
	kBuffer  = 1,
	kSampler = 2,
	kUnused  = 3,
};

enum class ShaderUsageType : uint32_t {
	kReadOnlyResource  = 0x00,
	kSampler           = 0x01,
	kConstantBuffer    = 0x02,
	kReadWriteResource = 0x04,
	kGdsMemoryRange    = 0x07,
	kFetchShader       = 0x12,
	kVertexBufferTable = 0x17,
	kExtendedUserData  = 0x1b,
};

enum class BufferFormat : uint32_t {
	kInvalid            = 0,
	k8UNorm             = 1,
	k8SNorm             = 2,
	k8UScaled           = 3,
	k8SScaled           = 4,
	k8UInt              = 5,
	k8SInt              = 6,
	k16UNorm            = 7,
	k16SNorm            = 8,
	k16UScaled          = 9,
	k16SScaled          = 10,
	k16UInt             = 11,
	k16SInt             = 12,
	k16Float            = 13,
	k8_8UNorm           = 14,
	k8_8SNorm           = 15,
	k8_8UScaled         = 16,
	k8_8SScaled         = 17,
	k8_8UInt            = 18,
	k8_8SInt            = 19,
	k32UInt             = 20,
	k32SInt             = 21,
	k32Float            = 22,
	k16_16UNorm         = 23,
	k16_16SNorm         = 24,
	k16_16UScaled       = 25,
	k16_16SScaled       = 26,
	k16_16UInt          = 27,
	k16_16SInt          = 28,
	k16_16Float         = 29,
	k11_11_10UNorm      = 30,
	k11_11_10SNorm      = 31,
	k11_11_10UScaled    = 32,
	k11_11_10SScaled    = 33,
	k11_11_10UInt       = 34,
	k11_11_10SInt       = 35,
	k11_11_10Float      = 36,
	k10_11_11UNorm      = 37,
	k10_11_11SNorm      = 38,
	k10_11_11UScaled    = 39,
	k10_11_11SScaled    = 40,
	k10_11_11UInt       = 41,
	k10_11_11SInt       = 42,
	k10_11_11Float      = 43,
	k2_10_10_10UNorm    = 44,
	k2_10_10_10SNorm    = 45,
	k2_10_10_10UScaled  = 46,
	k2_10_10_10SScaled  = 47,
	k2_10_10_10UInt     = 48,
	k2_10_10_10SInt     = 49,
	k10_10_10_2UNorm    = 50,
	k10_10_10_2SNorm    = 51,
	k10_10_10_2UScaled  = 52,
	k10_10_10_2SScaled  = 53,
	k10_10_10_2UInt     = 54,
	k10_10_10_2SInt     = 55,
	k8_8_8_8UNorm       = 56,
	k8_8_8_8SNorm       = 57,
	k8_8_8_8UScaled     = 58,
	k8_8_8_8SScaled     = 59,
	k8_8_8_8UInt        = 60,
	k8_8_8_8SInt        = 61,
	k32_32UInt          = 62,
	k32_32SInt          = 63,
	k32_32Float         = 64,
	k16_16_16_16UNorm   = 65,
	k16_16_16_16SNorm   = 66,
	k16_16_16_16UScaled = 67,
	k16_16_16_16SScaled = 68,
	k16_16_16_16UInt    = 69,
	k16_16_16_16SInt    = 70,
	k16_16_16_16Float   = 71,
	k32_32_32UInt       = 72,
	k32_32_32SInt       = 73,
	k32_32_32Float      = 74,
	k32_32_32_32UInt    = 75,
	k32_32_32_32SInt    = 76,
	k32_32_32_32Float   = 77,
	k8_8_8_8Srgb        = 130,
	k9_9_9_5Float       = 132,
	k5_6_5UNorm         = 133,
	k5_5_5_1UNorm       = 134,
	k4_4_4_4UNorm       = 136,
	kFmask8_S4_F4       = 161,
	kBc1UNorm           = 169,
	kBc1Srgb            = 170,
	kBc2UNorm           = 171,
	kBc2Srgb            = 172,
	kBc3UNorm           = 173,
	kBc3Srgb            = 174,
	kBc4UNorm           = 175,
	kBc4SNorm           = 176,
	kBc5UNorm           = 177,
	kBc5SNorm           = 178,
	kBc6UFloat          = 179,
	kBc6SFloat          = 180,
	kBc7UNorm           = 181,
	kBc7Srgb            = 182,
};

enum class VertexAttribFormat : uint32_t {
	kInvalid            = 0,
	k8UNorm             = 4,
	k8SNorm             = 8,
	k8UScaled           = 12,
	k8SScaled           = 16,
	k8UInt              = 20,
	k8SInt              = 24,
	k16UNorm            = 28,
	k16SNorm            = 32,
	k16UScaled          = 36,
	k16SScaled          = 40,
	k16UInt             = 44,
	k16SInt             = 48,
	k16Float            = 52,
	k8_8UNorm           = 57,
	k8_8SNorm           = 61,
	k8_8UScaled         = 65,
	k8_8SScaled         = 69,
	k8_8UInt            = 73,
	k8_8SInt            = 77,
	k32UInt             = 80,
	k32SInt             = 84,
	k32Float            = 88,
	k16_16UNorm         = 93,
	k16_16SNorm         = 97,
	k16_16UScaled       = 101,
	k16_16SScaled       = 105,
	k16_16UInt          = 109,
	k16_16SInt          = 113,
	k16_16Float         = 117,
	k11_11_10UNorm      = 122,
	k11_11_10SNorm      = 126,
	k11_11_10UScaled    = 130,
	k11_11_10SScaled    = 134,
	k11_11_10UInt       = 138,
	k11_11_10SInt       = 142,
	k11_11_10Float      = 146,
	k10_11_11UNorm      = 150,
	k10_11_11SNorm      = 154,
	k10_11_11UScaled    = 158,
	k10_11_11SScaled    = 162,
	k10_11_11UInt       = 166,
	k10_11_11SInt       = 170,
	k10_11_11Float      = 174,
	k2_10_10_10UNorm    = 179,
	k2_10_10_10SNorm    = 183,
	k2_10_10_10UScaled  = 187,
	k2_10_10_10SScaled  = 191,
	k2_10_10_10UInt     = 195,
	k2_10_10_10SInt     = 199,
	k10_10_10_2UNorm    = 203,
	k10_10_10_2SNorm    = 207,
	k10_10_10_2UScaled  = 211,
	k10_10_10_2SScaled  = 215,
	k10_10_10_2UInt     = 219,
	k10_10_10_2SInt     = 223,
	k8_8_8_8UNorm       = 227,
	k8_8_8_8SNorm       = 231,
	k8_8_8_8UScaled     = 235,
	k8_8_8_8SScaled     = 239,
	k8_8_8_8UInt        = 243,
	k8_8_8_8SInt        = 247,
	k32_32UInt          = 249,
	k32_32SInt          = 253,
	k32_32Float         = 257,
	k16_16_16_16UNorm   = 263,
	k16_16_16_16SNorm   = 267,
	k16_16_16_16UScaled = 271,
	k16_16_16_16SScaled = 275,
	k16_16_16_16UInt    = 279,
	k16_16_16_16SInt    = 283,
	k16_16_16_16Float   = 287,
	k32_32_32UInt       = 290,
	k32_32_32SInt       = 294,
	k32_32_32Float      = 298,
	k32_32_32_32UInt    = 303,
	k32_32_32_32SInt    = 307,
	k32_32_32_32Float   = 311,
};

} // namespace Libs::Graphics::Prospero

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_GUEST_GPU_GPU_DEFS_H_ */
