#include "common/threads.h"

#include "common/emulatorConfig.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/descriptors.h"
#include "graphics/host_gpu/renderer/image.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/host_gpu/renderer/resourceMutex.h"
#include "graphics/host_gpu/renderer/textureCache.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/SpirvBuilder.h"
#include "graphics/shader/shader.h"
#include "common/assert.h"
#include "common/logging/log.h"

#include "spirv-tools/libspirv.hpp"
#include "vulkan/vulkan.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef min
#undef max
#endif

namespace Libs::Graphics {
namespace {

using u32 = uint32_t;
using ShaderOpcode = ShaderRecompiler::Decoder::Opcode;

constexpr u32 InlineU32(u32 value) { return 128u + value; }

constexpr u32 Vgpr(u32 reg) { return 256u + reg; }

constexpr u32 EncodeSMovB32(u32 dst, u32 src) {
  return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) | (0x03u << 8u) |
         (src & 0xffu);
}

constexpr u32 EncodeSop1(u32 opcode, u32 dst, u32 src) {
  return 0x80000000u | (0x7du << 23u) | ((dst & 0x7fu) << 16u) |
         ((opcode & 0xffu) << 8u) | (src & 0xffu);
}

constexpr u32 EncodeSop2(u32 opcode, u32 dst, u32 src0, u32 src1) {
  return 0x80000000u | ((opcode & 0x7fu) << 23u) | ((dst & 0x7fu) << 16u) |
         ((src1 & 0xffu) << 8u) | (src0 & 0xffu);
}

constexpr u32 EncodeSopc(u32 opcode, u32 src0, u32 src1) {
  return 0x80000000u | (0x7eu << 23u) | ((opcode & 0x7fu) << 16u) |
         ((src1 & 0xffu) << 8u) | (src0 & 0xffu);
}

constexpr u32 EncodeSopp(u32 opcode, u32 simm = 0) {
  return 0x80000000u | (0x7fu << 23u) | ((opcode & 0x7fu) << 16u) |
         (simm & 0xffffu);
}

constexpr u32 EncodeSopk(u32 opcode, u32 dst, u32 imm) {
  return 0x80000000u | (((opcode + 0x60u) & 0x7fu) << 23u) |
         ((dst & 0x7fu) << 16u) | (imm & 0xffffu);
}

constexpr u32 EncodeVop1(u32 opcode, u32 dst, u32 src0) {
  return (0x3fu << 25u) | ((dst & 0xffu) << 17u) | ((opcode & 0xffu) << 9u) |
         (src0 & 0x1ffu);
}

constexpr u32 EncodeVop1Sdwa(u32 src0, u32 dst_sel = 6, u32 dst_u = 0,
                             u32 src0_sel = 6, u32 src0_sext = 0,
                             u32 src0_neg = 0, u32 src0_abs = 0, u32 s0 = 0,
                             u32 clamp = 0, u32 omod = 0) {
  return (src0 & 0xffu) | ((dst_sel & 0x7u) << 8u) | ((dst_u & 0x3u) << 11u) |
         ((clamp & 0x1u) << 13u) | ((omod & 0x3u) << 14u) |
         ((src0_sel & 0x7u) << 16u) | ((src0_sext & 0x1u) << 19u) |
         ((src0_neg & 0x1u) << 20u) | ((src0_abs & 0x1u) << 21u) |
         ((s0 & 0x1u) << 23u);
}

constexpr u32 EncodeVop2(u32 opcode, u32 dst, u32 src0, u32 src1) {
  return ((opcode & 0x3fu) << 25u) | ((dst & 0xffu) << 17u) |
         ((src1 & 0xffu) << 9u) | (src0 & 0x1ffu);
}

constexpr u32 EncodeVop2Sdwa(u32 src0, u32 dst_sel = 6, u32 dst_u = 0,
                             u32 src0_sel = 6, u32 src1_sel = 6,
                             u32 src0_sext = 0, u32 src1_sext = 0,
                             u32 src0_neg = 0, u32 src0_abs = 0,
                             u32 src1_neg = 0, u32 src1_abs = 0, u32 s0 = 0,
                             u32 s1 = 0, u32 clamp = 0, u32 omod = 0) {
  return (src0 & 0xffu) | ((dst_sel & 0x7u) << 8u) | ((dst_u & 0x3u) << 11u) |
         ((clamp & 0x1u) << 13u) | ((omod & 0x3u) << 14u) |
         ((src0_sel & 0x7u) << 16u) | ((src0_sext & 0x1u) << 19u) |
         ((src0_neg & 0x1u) << 20u) | ((src0_abs & 0x1u) << 21u) |
         ((s0 & 0x1u) << 23u) | ((src1_sel & 0x7u) << 24u) |
         ((src1_sext & 0x1u) << 27u) | ((src1_neg & 0x1u) << 28u) |
         ((src1_abs & 0x1u) << 29u) | ((s1 & 0x1u) << 31u);
}

constexpr u32 EncodeVop2Dpp(u32 src0, u32 dpp_ctrl = 0, u32 row_mask = 0xf,
                            u32 bank_mask = 0xf, u32 src0_neg = 0,
                            u32 src0_abs = 0, u32 src1_neg = 0,
                            u32 src1_abs = 0) {
  return (src0 & 0xffu) | ((dpp_ctrl & 0x1ffu) << 8u) |
         ((src0_neg & 0x1u) << 20u) | ((src0_abs & 0x1u) << 21u) |
         ((src1_neg & 0x1u) << 22u) | ((src1_abs & 0x1u) << 23u) |
         ((bank_mask & 0xfu) << 24u) | ((row_mask & 0xfu) << 28u);
}

constexpr u32 EncodeVop3Word0(u32 opcode, u32 dst, u32 abs = 0, u32 op_sel = 0,
                              bool clamp = false) {
  return (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | ((abs & 0x7u) << 8u) |
         ((op_sel & 0xfu) << 11u) | (clamp ? (1u << 15u) : 0u) | (dst & 0xffu);
}

constexpr u32 EncodeVop3BWord0(u32 opcode, u32 vdst, u32 sdst) {
  return (0x35u << 26u) | ((opcode & 0x3ffu) << 16u) | ((sdst & 0x7fu) << 8u) |
         (vdst & 0xffu);
}

constexpr u32 EncodeVop3Word1(u32 src0, u32 src1, u32 src2 = 0, u32 omod = 0,
                              u32 neg = 0) {
  return (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9u) | ((src2 & 0x1ffu) << 18u) |
         ((omod & 0x3u) << 27u) | ((neg & 0x7u) << 29u);
}

constexpr u32 EncodeVop3pWord0(u32 opcode, u32 dst, u32 op_sel_hi = 0,
                               u32 op_sel = 0, u32 neg_hi = 0,
                               bool clamp = false) {
  return (0x33u << 26u) | ((opcode & 0x7fu) << 16u) | ((neg_hi & 0x7u) << 8u) |
         ((op_sel & 0x7u) << 11u) | ((op_sel_hi & 0x4u) << 12u) |
         (clamp ? (1u << 15u) : 0u) | (dst & 0xffu);
}

constexpr u32 EncodeVop3pWord1(u32 src0, u32 src1, u32 src2 = 0,
                               u32 op_sel_hi = 0, u32 neg = 0) {
  return (src0 & 0x1ffu) | ((src1 & 0x1ffu) << 9u) | ((src2 & 0x1ffu) << 18u) |
         ((op_sel_hi & 0x3u) << 27u) | ((neg & 0x7u) << 29u);
}

constexpr u32 EncodeVopc(u32 opcode, u32 src0, u32 src1) {
  return (0x3eu << 25u) | ((opcode & 0xffu) << 17u) | ((src1 & 0xffu) << 9u) |
         (src0 & 0x1ffu);
}

constexpr u32 EncodeVopcSdwa(u32 src0, u32 sdst = 0, u32 sd = 0,
                             u32 src0_sel = 6, u32 src1_sel = 6,
                             u32 src0_sext = 0, u32 src1_sext = 0,
                             u32 src0_neg = 0, u32 src0_abs = 0,
                             u32 src1_neg = 0, u32 src1_abs = 0, u32 s0 = 0,
                             u32 s1 = 0) {
  return (src0 & 0xffu) | ((sdst & 0x7fu) << 8u) | ((sd & 0x1u) << 15u) |
         ((src0_sel & 0x7u) << 16u) | ((src0_sext & 0x1u) << 19u) |
         ((src0_neg & 0x1u) << 20u) | ((src0_abs & 0x1u) << 21u) |
         ((s0 & 0x1u) << 23u) | ((src1_sel & 0x7u) << 24u) |
         ((src1_sext & 0x1u) << 27u) | ((src1_neg & 0x1u) << 28u) |
         ((src1_abs & 0x1u) << 29u) | ((s1 & 0x1u) << 31u);
}

constexpr u32 EncodeMubuf0(u32 opcode, u32 offset = 0, bool idxen = false,
                           bool offen = true, bool glc = false) {
  return (0x38u << 26u) | ((opcode & 0x7fu) << 18u) |
         (offen ? (1u << 12u) : 0u) | (idxen ? (1u << 13u) : 0u) |
         (glc ? (1u << 14u) : 0u) | (offset & 0xfffu);
}

constexpr u32 EncodeMubuf1(u32 vdata, u32 srsrc, u32 vaddr, u32 soffset = 128) {
  return ((soffset & 0xffu) << 24u) | ((srsrc & 0x1fu) << 16u) |
         ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
}

constexpr u32 EncodeMtbuf0(u32 opcode, u32 dfmt, u32 nfmt, u32 offset = 0,
                           bool idxen = false, bool offen = true) {
  return (0x3au << 26u) | (offset & 0xfffu) | (offen ? (1u << 12u) : 0u) |
         (idxen ? (1u << 13u) : 0u) | ((opcode & 0x7u) << 16u) |
         ((dfmt & 0xfu) << 19u) | ((nfmt & 0x7u) << 23u);
}

constexpr u32 EncodeMtbuf1(u32 opcode, u32 vdata, u32 srsrc, u32 vaddr,
                           u32 soffset = 128) {
  return (((opcode >> 3u) & 1u) << 21u) | ((soffset & 0xffu) << 24u) |
         ((srsrc & 0x1fu) << 16u) | ((vdata & 0xffu) << 8u) | (vaddr & 0xffu);
}

constexpr u32 EncodeSmem0(u32 opcode, u32 dst, u32 sbase = 0) {
  return (0x3du << 26u) | ((opcode & 0xffu) << 18u) | ((dst & 0x7fu) << 6u) |
         (sbase & 0x3fu);
}

constexpr u32 EncodeSmem1(u32 offset, u32 soffset = 0) {
  return (offset & 0x1fffffu) | ((soffset & 0x7fu) << 25u);
}

constexpr u32 EncodeMimg0(u32 opcode, u32 dmask, u32 nsa_dwords = 0,
                          bool glc = false, u32 dim = 1) {
  return (0x3cu << 26u) | ((opcode >> 7u) & 0x1u) |
         ((nsa_dwords & 0x3u) << 1u) | ((dim & 0x7u) << 3u) |
         ((dmask & 0xfu) << 8u) | (glc ? (1u << 13u) : 0u) |
         ((opcode & 0x7fu) << 18u);
}

constexpr u32 EncodeMimg1(u32 vdata, u32 vaddr, u32 srsrc = 0, u32 ssamp = 0,
                          bool a16 = false) {
  return ((ssamp & 0x1fu) << 21u) | ((srsrc & 0x1fu) << 16u) |
         ((vdata & 0xffu) << 8u) | (vaddr & 0xffu) | (a16 ? (1u << 30u) : 0u);
}

constexpr u32 EncodeVintrp(u32 opcode, u32 dst, u32 attr, u32 chan, u32 src) {
  return (0x32u << 26u) | ((dst & 0xffu) << 18u) | ((opcode & 0x3u) << 16u) |
         ((attr & 0x3fu) << 10u) | ((chan & 0x3u) << 8u) | (src & 0xffu);
}

constexpr u32 EncodeExp0(u32 target, u32 en, bool done = true,
                         bool compr = false, bool vm = false) {
  return (0x3eu << 26u) | ((target & 0x3fu) << 4u) | (en & 0xfu) |
         (compr ? (1u << 10u) : 0u) | (done ? (1u << 11u) : 0u) |
         (vm ? (1u << 12u) : 0u);
}

constexpr u32 EncodeExp1(u32 src0, u32 src1, u32 src2, u32 src3) {
  return (src0 & 0xffu) | ((src1 & 0xffu) << 8u) | ((src2 & 0xffu) << 16u) |
         ((src3 & 0xffu) << 24u);
}

constexpr u32 EncodeFlat0(u32 opcode, u32 segment, u32 offset = 0) {
  return (0x37u << 26u) | ((opcode & 0x7fu) << 18u) |
         ((segment & 0x3u) << 14u) | (offset & 0xfffu);
}

constexpr u32 EncodeFlat1(u32 vdst, u32 saddr, u32 data, u32 addr) {
  return ((vdst & 0xffu) << 24u) | ((saddr & 0x7fu) << 16u) |
         ((data & 0xffu) << 8u) | (addr & 0xffu);
}

constexpr u32 EncodeDs0(u32 opcode, u32 offset = 0, bool gds = false) {
  return (0x36u << 26u) | ((opcode & 0xffu) << 18u) | (gds ? (1u << 17u) : 0u) |
         (offset & 0xffffu);
}

constexpr u32 EncodeDs1Ex(u32 vdst, u32 data1, u32 data0, u32 addr) {
  return ((vdst & 0xffu) << 24u) | ((data1 & 0xffu) << 16u) |
         ((data0 & 0xffu) << 8u) | (addr & 0xffu);
}

constexpr u32 EncodeDs1(u32 vdst, u32 data0, u32 addr) {
  return EncodeDs1Ex(vdst, 0, data0, addr);
}

void AppendSMovLiteral(std::vector<u32> *code, u32 dst, u32 literal) {
  code->push_back(EncodeSMovB32(dst, 255u));
  code->push_back(literal);
}

void AppendVMovLiteral(std::vector<u32> *code, u32 dst, u32 literal) {
  code->push_back(EncodeVop1(0x01, dst, 255u));
  code->push_back(literal);
}

void AppendVMovU32(std::vector<u32> *code, u32 dst, u32 value) {
  if (value <= 64u) {
    code->push_back(EncodeVop1(0x01, dst, InlineU32(value)));
    return;
  }
  AppendVMovLiteral(code, dst, value);
}

void AppendVop3(std::vector<u32> *code, u32 opcode, u32 dst, u32 src0, u32 src1,
                u32 src2 = 0, u32 abs = 0, u32 op_sel = 0, bool clamp = false,
                u32 omod = 0, u32 neg = 0) {
  code->push_back(EncodeVop3Word0(opcode, dst, abs, op_sel, clamp));
  code->push_back(EncodeVop3Word1(src0, src1, src2, omod, neg));
}

void AppendVop3B(std::vector<u32> *code, u32 opcode, u32 vdst, u32 sdst,
                 u32 src0, u32 src1, u32 src2 = 0) {
  code->push_back(EncodeVop3BWord0(opcode, vdst, sdst));
  code->push_back(EncodeVop3Word1(src0, src1, src2));
}

void AppendVop3p(std::vector<u32> *code, u32 opcode, u32 dst, u32 src0,
                 u32 src1, u32 src2 = 0, u32 op_sel_hi = 0, u32 op_sel = 0,
                 u32 neg_hi = 0, u32 neg = 0) {
  code->push_back(EncodeVop3pWord0(opcode, dst, op_sel_hi, op_sel, neg_hi));
  code->push_back(EncodeVop3pWord1(src0, src1, src2, op_sel_hi, neg));
}

void AppendBufferLoadDword(std::vector<u32> *code, u32 dst_vgpr,
                           u32 address_vgpr) {
  code->push_back(EncodeMubuf0(0x0cu));
  code->push_back(EncodeMubuf1(dst_vgpr, 0, address_vgpr));
}

void AppendBufferLoadOpcode(std::vector<u32> *code, u32 opcode, u32 dst_vgpr,
                            u32 address_vgpr) {
  code->push_back(EncodeMubuf0(opcode));
  code->push_back(EncodeMubuf1(dst_vgpr, 0, address_vgpr));
}

void AppendBufferStoreDword(std::vector<u32> *code, u32 value_vgpr,
                            u32 address_vgpr) {
  code->push_back(EncodeMubuf0(0x1cu));
  code->push_back(EncodeMubuf1(value_vgpr, 12, address_vgpr));
}

void AppendBufferStoreOpcode(std::vector<u32> *code, u32 opcode, u32 value_vgpr,
                             u32 address_vgpr, bool glc = false) {
  code->push_back(EncodeMubuf0(opcode, 0, false, true, glc));
  code->push_back(EncodeMubuf1(value_vgpr, 12, address_vgpr));
}

void AppendTBufferLoadOpcode(std::vector<u32> *code, u32 opcode, u32 dst_vgpr,
                             u32 address_vgpr) {
  code->push_back(EncodeMtbuf0(opcode, 14, 7));
  code->push_back(EncodeMtbuf1(opcode, dst_vgpr, 0, address_vgpr));
}

constexpr u32 BufferFormat(Prospero::BufferFormat format) {
  return Prospero::GpuEnumValue(format);
}

void AppendTBufferLoadFormatOpcode(std::vector<u32> *code, u32 opcode,
                                   u32 dst_vgpr, u32 address_vgpr,
                                   Prospero::BufferFormat format) {
  const auto value = BufferFormat(format);
  code->push_back(EncodeMtbuf0(opcode, value & 0xfu, (value >> 4u) & 0x7u));
  code->push_back(EncodeMtbuf1(opcode, dst_vgpr, 0, address_vgpr));
}

void AppendTBufferStoreOpcode(std::vector<u32> *code, u32 opcode,
                              u32 value_vgpr, u32 address_vgpr) {
  code->push_back(EncodeMtbuf0(opcode, 14, 7));
  code->push_back(EncodeMtbuf1(opcode, value_vgpr, 0, address_vgpr));
}

void AppendSmemLoadOpcode(std::vector<u32> *code, u32 opcode, u32 dst_sgpr,
                          u32 byte_offset) {
  code->push_back(EncodeSmem0(opcode, dst_sgpr));
  code->push_back(EncodeSmem1(byte_offset));
}

void AppendStoreVgpr(std::vector<u32> *code, u32 value_vgpr, u32 dword_index) {
  AppendVMovU32(code, 31, dword_index * 4u);
  AppendBufferStoreDword(code, value_vgpr, 31);
}

void AppendStoreVgprAtLaneDwordOffset(std::vector<u32> *code, u32 value_vgpr,
                                      u32 lane_vgpr, u32 dword_offset) {
  if (dword_offset == 0u) {
    code->push_back(EncodeVop2(0x1a, 31, InlineU32(2), lane_vgpr));
  } else {
    AppendVMovU32(code, 31, dword_offset);
    code->push_back(EncodeVop2(0x25, 31, Vgpr(lane_vgpr), 31));
    code->push_back(EncodeVop2(0x1a, 31, InlineU32(2), 31));
  }
  AppendBufferStoreDword(code, value_vgpr, 31);
}

void AppendStoreSgpr(std::vector<u32> *code, u32 value_sgpr, u32 dword_index) {
  code->push_back(EncodeVop1(0x01, 30, value_sgpr));
  AppendStoreVgpr(code, 30, dword_index);
}

void AppendStoreSgprAtLaneDwordOffset(std::vector<u32> *code, u32 value_sgpr,
                                      u32 lane_vgpr, u32 dword_offset) {
  code->push_back(EncodeVop1(0x01, 30, value_sgpr));
  AppendStoreVgprAtLaneDwordOffset(code, 30, lane_vgpr, dword_offset);
}

void AppendStoreSgprPair(std::vector<u32> *code, u32 value_sgpr,
                         u32 first_dword_index) {
  AppendStoreSgpr(code, value_sgpr, first_dword_index);
  AppendStoreSgpr(code, value_sgpr + 1u, first_dword_index + 1u);
}

void AppendEnd(std::vector<u32> *code) { code->push_back(0xbf810000u); }

std::string Hex(u32 value) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "0x%08" PRIx32, value);
  return buffer;
}

std::string VkResultName(VkResult result) {
  switch (result) {
  case VK_SUCCESS:
    return "VK_SUCCESS";
  case VK_NOT_READY:
    return "VK_NOT_READY";
  case VK_TIMEOUT:
    return "VK_TIMEOUT";
  case VK_ERROR_OUT_OF_HOST_MEMORY:
    return "VK_ERROR_OUT_OF_HOST_MEMORY";
  case VK_ERROR_OUT_OF_DEVICE_MEMORY:
    return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
  case VK_ERROR_INITIALIZATION_FAILED:
    return "VK_ERROR_INITIALIZATION_FAILED";
  case VK_ERROR_DEVICE_LOST:
    return "VK_ERROR_DEVICE_LOST";
  case VK_ERROR_MEMORY_MAP_FAILED:
    return "VK_ERROR_MEMORY_MAP_FAILED";
  case VK_ERROR_LAYER_NOT_PRESENT:
    return "VK_ERROR_LAYER_NOT_PRESENT";
  case VK_ERROR_EXTENSION_NOT_PRESENT:
    return "VK_ERROR_EXTENSION_NOT_PRESENT";
  case VK_ERROR_FEATURE_NOT_PRESENT:
    return "VK_ERROR_FEATURE_NOT_PRESENT";
  case VK_ERROR_INCOMPATIBLE_DRIVER:
    return "VK_ERROR_INCOMPATIBLE_DRIVER";
  case VK_ERROR_TOO_MANY_OBJECTS:
    return "VK_ERROR_TOO_MANY_OBJECTS";
  case VK_ERROR_FORMAT_NOT_SUPPORTED:
    return "VK_ERROR_FORMAT_NOT_SUPPORTED";
  default:
    return "VkResult(" + std::to_string(static_cast<int>(result)) + ")";
  }
}

[[noreturn]] void Fail(const char *shader_name, const char *stage,
                       const std::string &message) {
  std::fprintf(stderr, "ShaderRecompilerComputeTests: %s failed at %s: %s\n",
               shader_name, stage, message.c_str());
  std::abort();
}

void Require(const char *shader_name, const char *stage, bool value,
             const std::string &message);

void EnsureConfigInitialized() {
  static bool config_initialized = false;
  if (!config_initialized) {
    Common::ThreadsSubsystem::Instance()->Init(nullptr);
    Config::ConfigSubsystem::Instance()->Init(nullptr);
    Config::ConfigOptions options;
	options.printf_direction = Config::OutputDirection::Silent;
    Config::Load(options);
    Log::LogSubsystem::Instance()->Init(nullptr);
    config_initialized = true;
  }
}

void RequireVk(const char *shader_name, const char *stage, VkResult result,
               const char *action) {
  if (result != VK_SUCCESS) {
    Fail(shader_name, stage,
         std::string(action) + " returned " + VkResultName(result));
  }
}

void Require(const char *shader_name, const char *stage, bool value,
             const std::string &message) {
  if (!value) {
    Fail(shader_name, stage, message);
  }
}

struct TestCase {
  const char *name = "";
  std::vector<u32> code;
  std::vector<u32> initial;
  std::vector<u32> expected;
  std::vector<ShaderOpcode> opcodes;
  u32 image_width = 4;
  u32 image_height = 4;
  std::vector<u32> sampled_image_rgba;
  std::vector<std::vector<u32>> sampled_image_rgba_mips;
  VkFormat sampled_image_format = VK_FORMAT_R32G32B32A32_SFLOAT;
  u32 sampled_image_dwords_per_pixel = 4;
  std::vector<u32> storage_image_rgba;
  std::vector<u32> expected_storage_image_rgba;
  VkFormat storage_image_format = VK_FORMAT_R32G32B32A32_SFLOAT;
  u32 storage_image_dwords_per_pixel = 4;
  std::vector<u32> storage_image_r32ui;
  std::vector<u32> expected_storage_image_r32ui;
  std::vector<std::string> required_spirv;
  std::vector<std::string> forbidden_spirv;
  ShaderComputeInputInfo compute_info{};
  bool has_compute_info = false;
  u32 dispatch_x = 1;
  u32 dispatch_y = 1;
  u32 dispatch_z = 1;
  std::array<u32, 64> user_data{};
  bool has_user_data = false;
  u32 image_descriptor_swizzle = DstSel(4, 5, 6, 7);
  bool compile_only = false;
  size_t storage_buffer_range_dwords = 0;
  std::optional<uint64_t> flat_memory_base;
  std::vector<u32> gds_initial;
  std::vector<u32> expected_gds;
};

struct SkippedCase {
  const char *name = "";
  const char *reason = "";
};

struct GraphicsCase {
  const char *name = "";
  std::vector<u32> fragment_code;
  std::vector<u32> expected_pixel;
  std::vector<ShaderOpcode> opcodes;
  std::array<u32, 64> user_data{};
  bool has_user_data = false;
  std::vector<u32> push_constants;
  std::vector<u32> pixel_interpolator_settings;
  bool pixel_no_perspective = false;
  std::vector<u32> vertices;
};

struct CompiledShader {
  std::vector<u32> spirv;
  ShaderRecompiler::IR::Program program;
  std::vector<u32> flattened_srt;
  std::vector<u32> packed_user_data;
};

std::array<u32, 64> MakeNativeUserData(const std::array<u32, 64> *source) {
  std::array<u32, 64> data{};
  data[50] = 1u << 20u;
  if (source != nullptr) {
    data = *source;
  }
  return data;
}

bool ReadTestMemory(void *userdata, uint64_t address, u32 *value) {
  const auto *data = static_cast<const std::vector<u32> *>(userdata);
  if (data == nullptr || value == nullptr || address % 4u != 0 ||
      address / 4u >= data->size()) {
    return false;
  }
  *value = (*data)[address / 4u];
  return true;
}

void ValidateSpirv(const char *shader_name, const std::vector<u32> &spirv) {
  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string messages;
  tools.SetMessageConsumer([&messages](spv_message_level_t, const char *,
                                       const spv_position_t &position,
                                       const char *message) {
    char buffer[1024] = {};
    std::snprintf(buffer, sizeof(buffer), "%zu:%zu: %s\n", position.line,
                  position.column, message);
    messages += buffer;
  });

  if (!tools.Validate(spirv)) {
    Fail(shader_name, "SPIR-V validation", messages);
  }
}

void CheckSpirvText(const TestCase &test, const std::vector<u32> &spirv) {
  if (test.required_spirv.empty() && test.forbidden_spirv.empty()) {
    return;
  }

  spvtools::SpirvTools tools(SPV_ENV_VULKAN_1_2);
  std::string text;
  if (!tools.Disassemble(spirv, &text)) {
    Fail(test.name, "SPIR-V disassembly",
         "failed to disassemble emitted SPIR-V");
  }
  for (const auto &required : test.required_spirv) {
    if (text.find(required) == std::string::npos) {
      Fail(test.name, "SPIR-V disassembly",
           std::string("missing required text: ") + required);
    }
  }
  for (const auto &forbidden : test.forbidden_spirv) {
    if (text.find(forbidden) != std::string::npos) {
      Fail(test.name, "SPIR-V disassembly",
           std::string("found forbidden text: ") + forbidden);
    }
  }
}

CompiledShader CompileCase(const TestCase &test) {
  auto user_data =
      MakeNativeUserData(test.has_user_data ? &test.user_data : nullptr);
  const auto uses_image =
      std::any_of(test.opcodes.begin(), test.opcodes.end(), [](auto op) {
        return op >= ShaderOpcode::ImageGetResinfo &&
               op <= ShaderOpcode::ImageGather4H;
      });
  if (uses_image && ((user_data[3] >> 28u) & 0xfu) == 0) {
    user_data[3] = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) << 28u;
  }
  if (uses_image) {
    user_data[3] = (user_data[3] & ~0xfffu) | test.image_descriptor_swizzle;
  }
  if (!test.has_user_data) {
    user_data[2] = static_cast<u32>(test.initial.size() * sizeof(u32));
  }
  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Compute;
  options.dump_ir = true;
  options.compute_input_info =
      test.has_compute_info ? &test.compute_info : nullptr;
  options.user_data = user_data.data();
  options.read_memory = ReadTestMemory;
  options.read_memory_data = const_cast<std::vector<u32> *>(&test.initial);
  options.flat_memory_base = test.flat_memory_base;
  if (test.has_compute_info) {
    options.wave_size = test.compute_info.wave_size;
  }

  ShaderRecompiler::CompileResult result;
  std::string error;
  if (!ShaderRecompiler::TryRecompile(test.code, options, &result,
                                      &error)) {
    Fail(test.name, "decode/IR", error.c_str());
  }
  Require(test.name, "SPIR-V emit", !result.spirv.empty(),
          "recompiler returned empty SPIR-V");
  ValidateSpirv(test.name, result.spirv);
  CheckSpirvText(test, result.spirv);
  std::vector<u32> packed_user_data;
  for (const auto reg : result.program.bindings.user_data_registers) {
    packed_user_data.push_back(
        result.resources.user_data[reg - result.program.user_data_base]);
  }
  return {std::move(result.spirv), std::move(result.program),
          std::move(result.resources.flattened_srt),
          std::move(packed_user_data)};
}

std::array<u32, 64> MakeStructuredStorageBufferData(u32 stride_bytes,
                                                    u32 num_records,
                                                    bool add_tid = false,
                                                    u32 format = 0) {
  std::array<u32, 64> data{};
  data[1] = (stride_bytes & 0x3fffu) << 16u;
  data[2] = num_records;
  data[3] = 1u << 24u;
  if (add_tid) {
    data[3] |= 1u << 23u;
  }
  data[3] |= (format & 0x7fu) << 12u;
  return data;
}

std::array<u32, 64> MakeStorageTextureData(Prospero::BufferFormat format) {
  std::array<u32, 64> data{};
  data[0] = 0x1000u;
  data[1] = (Prospero::GpuEnumValue(format) & 0x1ffu) << 20u;
  data[3] = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) << 28u;
  return data;
}

CompiledShader CompileFragmentCase(const GraphicsCase &test) {
  const auto user_data =
      MakeNativeUserData(test.has_user_data ? &test.user_data : nullptr);
  ShaderPixelInputInfo pixel_info{};
  pixel_info.input_num =
      test.pixel_interpolator_settings.empty()
          ? 1u
          : static_cast<u32>(test.pixel_interpolator_settings.size());
  pixel_info.ps_no_perspective = test.pixel_no_perspective;
  for (u32 i = 0; i < std::size(pixel_info.interpolator_settings); i++) {
    pixel_info.interpolator_settings[i] = i;
  }
  for (u32 i = 0; i < test.pixel_interpolator_settings.size() &&
                  i < std::size(pixel_info.interpolator_settings);
       i++) {
    pixel_info.interpolator_settings[i] = test.pixel_interpolator_settings[i];
  }

  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Pixel;
  options.dump_ir = false;
  options.pixel_input_info = &pixel_info;
  options.user_data = user_data.data();

  ShaderRecompiler::CompileResult result;
  std::string error;
  if (!ShaderRecompiler::TryRecompile(test.fragment_code, options,
                                      &result, &error)) {
    Fail(test.name, "decode/IR", error.c_str());
  }
  Require(test.name, "SPIR-V emit", !result.spirv.empty(),
          "recompiler returned empty SPIR-V");
  ValidateSpirv(test.name, result.spirv);
  std::vector<u32> packed_user_data;
  for (const auto reg : result.program.bindings.user_data_registers) {
    packed_user_data.push_back(
        result.resources.user_data[reg - result.program.user_data_base]);
  }
  return {std::move(result.spirv), std::move(result.program),
          std::move(result.resources.flattened_srt),
          std::move(packed_user_data)};
}

std::array<u32, 64> MakeSampledTextureData(Prospero::BufferFormat format) {
  return MakeStorageTextureData(format);
}

namespace TestSpv {

enum : u32 {
  ExecutionModelVertex = 0,
  AddressingModelLogical = 0,
  MemoryModelGLSL450 = 1,
  CapabilityShader = 1,
  StorageClassInput = 1,
  StorageClassOutput = 3,
  FunctionControlNone = 0,
  DecorationBlock = 2,
  DecorationBuiltIn = 11,
  DecorationLocation = 30,
  BuiltInPosition = 0,
  OpTypeVoid = 19,
  OpTypeInt = 21,
  OpTypeFloat = 22,
  OpTypeVector = 23,
  OpTypeStruct = 30,
  OpTypePointer = 32,
  OpTypeFunction = 33,
  OpConstant = 43,
  OpFunction = 54,
  OpFunctionEnd = 56,
  OpVariable = 59,
  OpLoad = 61,
  OpStore = 62,
  OpAccessChain = 65,
  OpDecorate = 71,
  OpMemberDecorate = 72,
  OpCompositeConstruct = 80,
  OpCompositeExtract = 81,
  OpLabel = 248,
  OpReturn = 253,
};

std::vector<u32> MakePassthroughVertexSpirv() {
  using ShaderRecompiler::Spirv::Builder;

  Builder b;
  const auto void_type = b.AllocateId();
  const auto uint_type = b.AllocateId();
  const auto float_type = b.AllocateId();
  const auto vec2_type = b.AllocateId();
  const auto vec4_type = b.AllocateId();
  const auto per_vertex_type = b.AllocateId();
  const auto ptr_input_vec2 = b.AllocateId();
  const auto ptr_input_vec4 = b.AllocateId();
  const auto ptr_output_vec4 = b.AllocateId();
  const auto ptr_output_per_vertex = b.AllocateId();
  const auto func_type = b.AllocateId();
  const auto const_u32_0 = b.AllocateId();
  const auto const_f32_0 = b.AllocateId();
  const auto const_f32_1 = b.AllocateId();
  const auto in_pos = b.AllocateId();
  const auto in_color = b.AllocateId();
  const auto out_color = b.AllocateId();
  const auto per_vertex = b.AllocateId();
  const auto main = b.AllocateId();
  const auto label = b.AllocateId();
  const auto pos2 = b.AllocateId();
  const auto color4 = b.AllocateId();
  const auto pos_x = b.AllocateId();
  const auto pos_y = b.AllocateId();
  const auto position = b.AllocateId();
  const auto position_ptr = b.AllocateId();

  b.AddCapability({CapabilityShader});
  b.AddMemoryModel({AddressingModelLogical, MemoryModelGLSL450});
  b.AddEntryPoint(ExecutionModelVertex, main, "main",
                  {in_pos, in_color, per_vertex, out_color});
  b.AddAnnotation({OpDecorate, in_pos, DecorationLocation, 0});
  b.AddAnnotation({OpDecorate, in_color, DecorationLocation, 1});
  b.AddAnnotation({OpDecorate, out_color, DecorationLocation, 0});
  b.AddAnnotation({OpDecorate, per_vertex_type, DecorationBlock});
  b.AddAnnotation({OpMemberDecorate, per_vertex_type, 0, DecorationBuiltIn,
                   BuiltInPosition});

  b.AddType({OpTypeVoid, void_type});
  b.AddType({OpTypeInt, uint_type, 32, 0});
  b.AddType({OpTypeFloat, float_type, 32});
  b.AddType({OpTypeVector, vec2_type, float_type, 2});
  b.AddType({OpTypeVector, vec4_type, float_type, 4});
  b.AddType({OpTypeStruct, per_vertex_type, vec4_type});
  b.AddType({OpTypePointer, ptr_input_vec2, StorageClassInput, vec2_type});
  b.AddType({OpTypePointer, ptr_input_vec4, StorageClassInput, vec4_type});
  b.AddType({OpTypePointer, ptr_output_vec4, StorageClassOutput, vec4_type});
  b.AddType({OpTypePointer, ptr_output_per_vertex, StorageClassOutput,
             per_vertex_type});
  b.AddType({OpTypeFunction, func_type, void_type});
  b.AddType({OpConstant, uint_type, const_u32_0, 0});
  b.AddType({OpConstant, float_type, const_f32_0, 0x00000000u});
  b.AddType({OpConstant, float_type, const_f32_1, 0x3f800000u});
  b.AddType({OpVariable, ptr_input_vec2, in_pos, StorageClassInput});
  b.AddType({OpVariable, ptr_input_vec4, in_color, StorageClassInput});
  b.AddType({OpVariable, ptr_output_vec4, out_color, StorageClassOutput});
  b.AddType(
      {OpVariable, ptr_output_per_vertex, per_vertex, StorageClassOutput});

  b.AddFunction({OpFunction, void_type, main, FunctionControlNone, func_type});
  b.AddFunction({OpLabel, label});
  b.AddFunction({OpLoad, vec2_type, pos2, in_pos});
  b.AddFunction({OpLoad, vec4_type, color4, in_color});
  b.AddFunction({OpCompositeExtract, float_type, pos_x, pos2, 0});
  b.AddFunction({OpCompositeExtract, float_type, pos_y, pos2, 1});
  b.AddFunction({OpCompositeConstruct, vec4_type, position, pos_x, pos_y,
                 const_f32_0, const_f32_1});
  b.AddFunction(
      {OpAccessChain, ptr_output_vec4, position_ptr, per_vertex, const_u32_0});
  b.AddFunction({OpStore, position_ptr, position});
  b.AddFunction({OpStore, out_color, color4});
  b.AddFunction({OpReturn});
  b.AddFunction({OpFunctionEnd});
  return b.Build();
}

} // namespace TestSpv

class VulkanHarness {
public:
  VulkanHarness() { Init(); }
  ~VulkanHarness() { Destroy(); }

  VulkanHarness(const VulkanHarness &) = delete;
  VulkanHarness &operator=(const VulkanHarness &) = delete;

  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    bool coherent = false;
  };

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 1;
    u32 dwords_per_pixel = 0;
  };

  [[nodiscard]] VkDevice Device() const { return m_device; }

  void CheckMutableStorageSrgbView() {
    constexpr const char *name = "StorageTextureMutableSrgbView";
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_info.extent = {16, 16, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT |
                       VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageFormatProperties exact_properties{};
    const bool srgb_storage_supported =
        vkGetPhysicalDeviceImageFormatProperties(
            m_physical_device, image_info.format, image_info.imageType,
            image_info.tiling, image_info.usage, image_info.flags,
            &exact_properties) == VK_SUCCESS;
    GraphicContext context{};
    context.physical_device = m_physical_device;
    Require(name, "format fallback", TextureCheckFormat(&context, &image_info),
            "mutable sampled/storage RGBA8 image is unsupported");
    Require(name, "format fallback",
            image_info.format == VK_FORMAT_R8G8B8A8_UNORM ||
                (srgb_storage_supported &&
                 image_info.format == VK_FORMAT_R8G8B8A8_SRGB),
            "unsupported sRGB storage format did not fall back to UNORM");

    VkImage image = VK_NULL_HANDLE;
    RequireVk(name, "backing", vkCreateImage(m_device, &image_info, nullptr, &image),
              "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, image, &requirements);
    u32 memory_type = 0;
    Require(name, "backing",
            FindMemoryType(requirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type) ||
                FindMemoryType(requirements.memoryTypeBits, 0, &memory_type),
            "no memory type for mutable storage image");
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    RequireVk(name, "backing",
              vkAllocateMemory(m_device, &allocation, nullptr, &memory),
              "vkAllocateMemory");
    RequireVk(name, "backing", vkBindImageMemory(m_device, image, memory, 0),
              "vkBindImageMemory");

    VkImageViewUsageCreateInfo view_usage{};
    view_usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
    view_usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = &view_usage;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = image_info.format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    VkImageView default_view = VK_NULL_HANDLE;
    RequireVk(name, "default view",
              vkCreateImageView(m_device, &view_info, nullptr, &default_view),
              "vkCreateImageView(default sampled/storage)");

    view_usage.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    VkImageView srgb_view = VK_NULL_HANDLE;
    RequireVk(name, "sRGB view",
              vkCreateImageView(m_device, &view_info, nullptr, &srgb_view),
              "vkCreateImageView(sRGB sampled)");

    vkDestroyImageView(m_device, srgb_view, nullptr);
    vkDestroyImageView(m_device, default_view, nullptr);
    vkDestroyImage(m_device, image, nullptr);
    vkFreeMemory(m_device, memory, nullptr);
    std::printf("[host]    %-32s ok (backing=%d)\n", name,
                static_cast<int>(image_info.format));
  }

  void CheckMutableRenderTargetBgraStorageView() {
    constexpr const char *name = "RenderTargetMutableBgraStorage";
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.flags =
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_B8G8R8A8_SRGB;
    image_info.extent = {8, 8, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImageFormatProperties properties{};
    RequireVk(name, "backing query",
              vkGetPhysicalDeviceImageFormatProperties(
                  m_physical_device, image_info.format, image_info.imageType,
                  image_info.tiling, image_info.usage, image_info.flags,
                  &properties),
              "mutable extended-usage BGRA sRGB backing");

    VkImage image = VK_NULL_HANDLE;
    RequireVk(name, "backing",
              vkCreateImage(m_device, &image_info, nullptr, &image),
              "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(m_device, image, &requirements);
    u32 memory_type = 0;
    Require(name, "backing",
            FindMemoryType(requirements.memoryTypeBits,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &memory_type) ||
                FindMemoryType(requirements.memoryTypeBits, 0, &memory_type),
            "no memory type for mutable render target");
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    RequireVk(name, "backing",
              vkAllocateMemory(m_device, &allocation, nullptr, &memory),
              "vkAllocateMemory");
    RequireVk(name, "backing", vkBindImageMemory(m_device, image, memory, 0),
              "vkBindImageMemory");

    VkImageViewUsageCreateInfo view_usage{};
    view_usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
    view_usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.pNext = &view_usage;
    view_info.image = image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    VkImageView storage_view = VK_NULL_HANDLE;
    RequireVk(name, "storage view",
              vkCreateImageView(m_device, &view_info, nullptr, &storage_view),
              "vkCreateImageView(RGBA UNORM storage)");

    vkDestroyImageView(m_device, storage_view, nullptr);
    vkDestroyImage(m_device, image, nullptr);
    vkFreeMemory(m_device, memory, nullptr);
    std::printf("[host]    %-32s ok (backing=%d view=%d)\n", name,
                static_cast<int>(image_info.format),
                static_cast<int>(view_info.format));
  }

  Buffer CreateStorageBuffer(const char *shader_name,
                             const std::vector<u32> &initial,
                             size_t dword_count) {
    Buffer ret;
    ret.size = static_cast<VkDeviceSize>(std::max<size_t>(dword_count, 1u) *
                                         sizeof(u32));

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = ret.size;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    RequireVk(shader_name, "dispatch",
              vkCreateBuffer(m_device, &buffer_info, nullptr, &ret.buffer),
              "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, ret.buffer, &req);
    u32 memory_type = 0;
    ret.coherent = FindMemoryType(req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &memory_type);
    if (!ret.coherent) {
      Require(shader_name, "dispatch",
              FindMemoryType(req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type),
              "no host-visible memory type for storage buffer");
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type;
    RequireVk(shader_name, "dispatch",
              vkAllocateMemory(m_device, &alloc, nullptr, &ret.memory),
              "vkAllocateMemory");
    RequireVk(shader_name, "dispatch",
              vkBindBufferMemory(m_device, ret.buffer, ret.memory, 0),
              "vkBindBufferMemory");

    std::vector<u32> contents(dword_count, 0);
    for (size_t i = 0; i < initial.size() && i < contents.size(); i++) {
      contents[i] = initial[i];
    }
    WriteBuffer(shader_name, ret, contents);
    return ret;
  }

  void DestroyBuffer(Buffer *buffer) {
    if (buffer == nullptr) {
      return;
    }
    if (buffer->buffer != VK_NULL_HANDLE) {
      vkDestroyBuffer(m_device, buffer->buffer, nullptr);
      buffer->buffer = VK_NULL_HANDLE;
    }
    if (buffer->memory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, buffer->memory, nullptr);
      buffer->memory = VK_NULL_HANDLE;
    }
  }

  Image CreateImage2D(const char *shader_name, u32 width, u32 height,
                      VkFormat format, VkImageUsageFlags usage,
                      const std::vector<u32> &initial, u32 dwords_per_pixel,
                      VkImageLayout final_layout) {
    std::vector<std::vector<u32>> mips;
    if (!initial.empty()) {
      mips.push_back(initial);
    }
    return CreateImage2DMips(shader_name, width, height, format, usage, mips,
                             dwords_per_pixel, final_layout);
  }

  static u32 MipExtent(u32 value, u32 level) {
    for (u32 i = 0; i < level && value > 1u; i++) {
      value >>= 1u;
    }
    return std::max(value, 1u);
  }

  static size_t ImageMipDwordCount(u32 width, u32 height, u32 dwords_per_pixel,
                                   u32 level) {
    return static_cast<size_t>(MipExtent(width, level)) *
           static_cast<size_t>(MipExtent(height, level)) * dwords_per_pixel;
  }

  Image CreateImage2DMips(const char *shader_name, u32 width, u32 height,
                          VkFormat format, VkImageUsageFlags usage,
                          const std::vector<std::vector<u32>> &initial_mips,
                          u32 dwords_per_pixel, VkImageLayout final_layout) {
    Image ret;
    ret.format = format;
    ret.width = width;
    ret.height = height;
    ret.mip_levels = std::max<u32>(static_cast<u32>(initial_mips.size()), 1u);
    ret.dwords_per_pixel = dwords_per_pixel;

    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = ret.mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    RequireVk(shader_name, "dispatch",
              vkCreateImage(m_device, &image_info, nullptr, &ret.image),
              "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(m_device, ret.image, &req);
    u32 memory_type = 0;
    if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        &memory_type)) {
      Require(shader_name, "dispatch",
              FindMemoryType(req.memoryTypeBits, 0, &memory_type),
              "no memory type for image");
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type;
    RequireVk(shader_name, "dispatch",
              vkAllocateMemory(m_device, &alloc, nullptr, &ret.memory),
              "vkAllocateMemory(image)");
    RequireVk(shader_name, "dispatch",
              vkBindImageMemory(m_device, ret.image, ret.memory, 0),
              "vkBindImageMemory");

    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = ret.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = ret.mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    RequireVk(shader_name, "dispatch",
              vkCreateImageView(m_device, &view_info, nullptr, &ret.view),
              "vkCreateImageView");

    if (!initial_mips.empty()) {
      size_t total_dwords = 0;
      for (u32 level = 0; level < ret.mip_levels; level++) {
        total_dwords +=
            ImageMipDwordCount(width, height, dwords_per_pixel, level);
      }
      std::vector<u32> contents(total_dwords, 0);
      size_t offset = 0;
      for (u32 level = 0; level < ret.mip_levels; level++) {
        const auto level_dwords =
            ImageMipDwordCount(width, height, dwords_per_pixel, level);
        const auto &src = initial_mips[level];
        for (size_t i = 0; i < src.size() && i < level_dwords; i++) {
          contents[offset + i] = src[i];
        }
        offset += level_dwords;
      }
      auto staging =
          CreateHostBuffer(shader_name, contents.size() * sizeof(u32),
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT, contents);
      UploadImageMips(shader_name, &ret, staging.buffer, final_layout);
      DestroyBuffer(&staging);
    } else {
      TransitionImage(shader_name, &ret, final_layout,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                      AccessForLayout(final_layout));
    }
    return ret;
  }

  void DestroyImage(Image *image) {
    if (image == nullptr) {
      return;
    }
    if (image->view != VK_NULL_HANDLE) {
      vkDestroyImageView(m_device, image->view, nullptr);
      image->view = VK_NULL_HANDLE;
    }
    if (image->image != VK_NULL_HANDLE) {
      vkDestroyImage(m_device, image->image, nullptr);
      image->image = VK_NULL_HANDLE;
    }
    if (image->memory != VK_NULL_HANDLE) {
      vkFreeMemory(m_device, image->memory, nullptr);
      image->memory = VK_NULL_HANDLE;
    }
  }

  std::vector<u32> ReadImage(const char *shader_name, Image *image) {
    const auto dword_count = static_cast<size_t>(image->width) *
                             static_cast<size_t>(image->height) *
                             image->dwords_per_pixel;
    auto staging = CreateHostBuffer(shader_name, dword_count * sizeof(u32),
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, {});

    VkCommandBuffer cmd = BeginCommands(shader_name, "readback");
    AddImageBarrier(
        cmd, image->image, image->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = image->width;
    copy.imageExtent.height = image->height;
    copy.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.buffer,
                           1, &copy);
    EndSubmitAndFree(shader_name, "readback", cmd);
    image->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    auto ret = ReadBuffer(shader_name, staging, dword_count);
    DestroyBuffer(&staging);
    return ret;
  }

  VkSampler CreateNearestSampler(const char *shader_name) {
    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    VkSampler sampler = VK_NULL_HANDLE;
    RequireVk(shader_name, "dispatch",
              vkCreateSampler(m_device, &sampler_info, nullptr, &sampler),
              "vkCreateSampler");
    return sampler;
  }

  std::vector<u32> ReadBuffer(const char *shader_name, const Buffer &buffer,
                              size_t dword_count) {
    if (!buffer.coherent) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = buffer.memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      RequireVk(shader_name, "readback",
                vkInvalidateMappedMemoryRanges(m_device, 1, &range),
                "vkInvalidateMappedMemoryRanges");
    }

    void *data = nullptr;
    RequireVk(shader_name, "readback",
              vkMapMemory(m_device, buffer.memory, 0, buffer.size, 0, &data),
              "vkMapMemory");
    std::vector<u32> ret(dword_count, 0);
    std::memcpy(ret.data(), data, dword_count * sizeof(u32));
    vkUnmapMemory(m_device, buffer.memory);
    return ret;
  }

  void Dispatch(const TestCase &test, const CompiledShader &compiled,
                const Buffer &buffer, const Buffer *gds_buffer = nullptr,
                const Image *sampled_image = nullptr,
                const Image *storage_image = nullptr,
                const Image *storage_image_uint = nullptr,
                VkSampler sampler = VK_NULL_HANDLE) {
    using Kind = ShaderRecompiler::IR::DescriptorBindingKind;
    const auto &layout = compiled.program.bindings;
    auto Binding = [&](Kind kind) {
      return ShaderRecompiler::IR::FindBinding(layout, kind);
    };
    auto Count = [&](Kind kind) {
      const auto *binding = Binding(kind);
      return binding == nullptr  ? 0u
             : kind == Kind::Gds ? 1u
                                 : static_cast<u32>(binding->resources.size());
    };
    Require(test.name, "dispatch",
            Count(Kind::Sampled2DArray) == 0 && Count(Kind::Sampled3D) == 0 &&
                Count(Kind::SampledUint2DArray) == 0 &&
                Count(Kind::SampledUint3D) == 0 &&
                Count(Kind::Storage2DArray) == 0 &&
                Count(Kind::Storage3D) == 0 &&
                Count(Kind::StorageUint2DArray) == 0 &&
                Count(Kind::StorageUint3D) == 0,
            "array/3D image cases must provide matching Vulkan test views "
            "before dispatch");

    VkShaderModuleCreateInfo module_info{};
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = compiled.spirv.size() * sizeof(u32);
    module_info.pCode = compiled.spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    RequireVk(test.name, "SPIR-V module",
              vkCreateShaderModule(m_device, &module_info, nullptr, &module),
              "vkCreateShaderModule");

    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
    auto add_layout_binding =
        [&layout_bindings](u32 binding, VkDescriptorType type, u32 count) {
          if (count == 0) {
            return;
          }
          VkDescriptorSetLayoutBinding item{};
          item.binding = binding;
          item.descriptorType = type;
          item.descriptorCount = count;
          item.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
          layout_bindings.push_back(item);
        };
    for (const auto &binding : layout.descriptors) {
      VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      u32 count = static_cast<u32>(binding.resources.size());
      switch (binding.kind) {
      case Kind::Sampled2D:
      case Kind::Sampled2DArray:
      case Kind::Sampled3D:
      case Kind::SampledUint2D:
      case Kind::SampledUint2DArray:
      case Kind::SampledUint3D:
        type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        break;
      case Kind::Storage2D:
      case Kind::Storage2DArray:
      case Kind::Storage3D:
      case Kind::StorageUint2D:
      case Kind::StorageUint2DArray:
      case Kind::StorageUint3D:
        type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        break;
      case Kind::Samplers:
        type = VK_DESCRIPTOR_TYPE_SAMPLER;
        break;
      case Kind::Gds:
      case Kind::FlattenedSrt:
      case Kind::UserData:
        count = 1;
        break;
      default:
        break;
      }
      add_layout_binding(binding.binding, type, count);
    }

    VkDescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layout_info.bindingCount = static_cast<u32>(layout_bindings.size());
    layout_info.pBindings =
        layout_bindings.empty() ? nullptr : layout_bindings.data();
    VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
    RequireVk(test.name, "dispatch",
              vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr,
                                          &descriptor_layout),
              "vkCreateDescriptorSetLayout");

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    VkPushConstantRange push_range{};
    if (layout.push_constant_size != 0) {
      push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      push_range.offset = layout.push_constant_offset;
      push_range.size = layout.push_constant_size;
      pipeline_layout_info.pushConstantRangeCount = 1;
      pipeline_layout_info.pPushConstantRanges = &push_range;
    }
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    RequireVk(test.name, "dispatch",
              vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr,
                                     &pipeline_layout),
              "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stage{};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = module;
    stage.pName = "main";

    VkComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    RequireVk(test.name, "dispatch",
              vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1,
                                       &pipeline_info, nullptr, &pipeline),
              "vkCreateComputePipelines");

    std::vector<VkDescriptorPoolSize> pool_sizes;
    auto add_pool_size = [&pool_sizes](VkDescriptorType type, u32 count) {
      if (count == 0) {
        return;
      }
      for (auto &size : pool_sizes) {
        if (size.type == type) {
          size.descriptorCount += count;
          return;
        }
      }
      pool_sizes.push_back({type, count});
    };
    add_pool_size(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                  Count(Kind::Buffers) + Count(Kind::AddressMemory) +
                      Count(Kind::Gds) +
                      (Binding(Kind::FlattenedSrt) != nullptr ? 1u : 0u) +
                      (Binding(Kind::UserData) != nullptr ? 1u : 0u));
    add_pool_size(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                  Count(Kind::Sampled2D) + Count(Kind::Sampled2DArray) +
                      Count(Kind::Sampled3D) + Count(Kind::SampledUint2D) +
                      Count(Kind::SampledUint2DArray) +
                      Count(Kind::SampledUint3D));
    add_pool_size(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                  Count(Kind::Storage2D) + Count(Kind::Storage2DArray) +
                      Count(Kind::Storage3D) + Count(Kind::StorageUint2D) +
                      Count(Kind::StorageUint2DArray) +
                      Count(Kind::StorageUint3D));
    add_pool_size(VK_DESCRIPTOR_TYPE_SAMPLER, Count(Kind::Samplers));
    VkDescriptorPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<u32>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.empty() ? nullptr : pool_sizes.data();
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    RequireVk(
        test.name, "dispatch",
        vkCreateDescriptorPool(m_device, &pool_info, nullptr, &descriptor_pool),
        "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &descriptor_layout;
    VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
    RequireVk(test.name, "dispatch",
              vkAllocateDescriptorSets(m_device, &set_info, &descriptor_set),
              "vkAllocateDescriptorSets");

    std::vector<VkWriteDescriptorSet> writes;
    std::vector<VkDescriptorBufferInfo> buffer_infos;
    std::vector<VkDescriptorBufferInfo> address_memory_infos;
    std::vector<VkDescriptorImageInfo> sampled_infos;
    std::vector<VkDescriptorImageInfo> storage_infos;
    std::vector<VkDescriptorImageInfo> storage_uint_infos;
    std::vector<VkDescriptorImageInfo> sampler_infos;
    Buffer flattened_buffer;
    Buffer user_data_buffer;
    VkDescriptorBufferInfo flattened_info{};
    VkDescriptorBufferInfo user_data_info{};
    VkDescriptorBufferInfo gds_info{};

    const auto *buffers = Binding(Kind::Buffers);
    if (buffers != nullptr) {
      buffer_infos.resize(buffers->resources.size());
      for (auto &info : buffer_infos) {
        info.buffer = buffer.buffer;
        info.offset = 0;
        info.range = buffer.size;
        if (test.storage_buffer_range_dwords != 0) {
          info.range = static_cast<VkDeviceSize>(
              test.storage_buffer_range_dwords * sizeof(u32));
          Require(test.name, "dispatch", info.range <= buffer.size,
                  "storage buffer descriptor range exceeds backing buffer");
        }
      }
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptor_set;
      write.dstBinding = buffers->binding;
      write.descriptorCount = static_cast<u32>(buffer_infos.size());
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = buffer_infos.data();
      writes.push_back(write);
    }
    if (const auto *address = Binding(Kind::AddressMemory);
        address != nullptr) {
      address_memory_infos.resize(address->resources.size());
      for (auto &info : address_memory_infos) {
        info = {buffer.buffer, 0, buffer.size};
      }
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptor_set;
      write.dstBinding = address->binding;
      write.descriptorCount = static_cast<u32>(address_memory_infos.size());
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = address_memory_infos.data();
      writes.push_back(write);
    }
    if (const auto *flattened = Binding(Kind::FlattenedSrt);
        flattened != nullptr) {
      flattened_buffer = CreateStorageBuffer(test.name, compiled.flattened_srt,
                                             compiled.flattened_srt.size());
      flattened_info = {flattened_buffer.buffer, 0, flattened_buffer.size};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptor_set;
      write.dstBinding = flattened->binding;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &flattened_info;
      writes.push_back(write);
    }
    if (const auto *user = Binding(Kind::UserData); user != nullptr) {
      user_data_buffer =
          CreateStorageBuffer(test.name, compiled.packed_user_data,
                              compiled.packed_user_data.size());
      user_data_info = {user_data_buffer.buffer, 0, user_data_buffer.size};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptor_set;
      write.dstBinding = user->binding;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &user_data_info;
      writes.push_back(write);
    }
    if (const auto *gds = Binding(Kind::Gds); gds != nullptr) {
      Require(test.name, "dispatch", gds_buffer != nullptr,
              "GDS descriptor requested but no GDS buffer was provided");
      gds_info = {gds_buffer->buffer, 0, gds_buffer->size};
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptor_set;
      write.dstBinding = gds->binding;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      write.pBufferInfo = &gds_info;
      writes.push_back(write);
    }
    const auto *sampled = Binding(Kind::Sampled2D);
    const auto *sampled_uint = Binding(Kind::SampledUint2D);
    Require(test.name, "dispatch",
            sampled == nullptr || sampled_uint == nullptr,
            "Vulkan test harness needs separate float and uint sampled images");
    if (sampled == nullptr) {
      sampled = sampled_uint;
    }
    if (sampled != nullptr) {
      Require(test.name, "dispatch", sampled_image != nullptr,
              "sampled image descriptor requested but no sampled image was "
              "provided");
      sampled_infos.resize(sampled->resources.size());
      for (auto &info : sampled_infos) {
        info.imageView = sampled_image->view;
        info.imageLayout = sampled_image->layout;
      }
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptor_set;
      write.dstBinding = sampled->binding;
      write.descriptorCount = static_cast<u32>(sampled_infos.size());
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = sampled_infos.data();
      writes.push_back(write);
    }
    const auto *storage = Binding(Kind::Storage2D);
    const auto *storage_uint = Binding(Kind::StorageUint2D);
    if (storage != nullptr || storage_uint != nullptr) {
      Require(test.name, "dispatch", storage_image != nullptr,
              "storage image descriptor requested but no storage image was "
              "provided");
      Require(test.name, "dispatch", storage_image_uint != nullptr,
              "uint storage image descriptor requested but no uint storage "
              "image was provided");
      storage_infos.resize(storage != nullptr ? storage->resources.size() : 0u);
      storage_uint_infos.resize(
          storage_uint != nullptr ? storage_uint->resources.size() : 0u);
      for (auto &info : storage_infos) {
        info.imageView = storage_image->view;
        info.imageLayout = storage_image->layout;
      }
      for (auto &info : storage_uint_infos) {
        info.imageView = storage_image_uint->view;
        info.imageLayout = storage_image_uint->layout;
      }
      if (storage != nullptr) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_set;
        write.dstBinding = storage->binding;
        write.descriptorCount = static_cast<u32>(storage_infos.size());
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = storage_infos.data();
        writes.push_back(write);
      }
      if (storage_uint != nullptr) {
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptor_set;
        write.dstBinding = storage_uint->binding;
        write.descriptorCount = static_cast<u32>(storage_uint_infos.size());
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = storage_uint_infos.data();
        writes.push_back(write);
      }
    }
    const auto *samplers = Binding(Kind::Samplers);
    if (samplers != nullptr) {
      Require(test.name, "dispatch", sampler != VK_NULL_HANDLE,
              "sampler descriptor requested but no sampler was provided");
      sampler_infos.resize(samplers->resources.size());
      for (auto &info : sampler_infos) {
        info.sampler = sampler;
      }
      VkWriteDescriptorSet write{};
      write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write.dstSet = descriptor_set;
      write.dstBinding = samplers->binding;
      write.descriptorCount = static_cast<u32>(sampler_infos.size());
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      write.pImageInfo = sampler_infos.data();
      writes.push_back(write);
    }
    if (!writes.empty()) {
      vkUpdateDescriptorSets(m_device, static_cast<u32>(writes.size()),
                             writes.data(), 0, nullptr);
    }

    VkCommandBuffer cmd = BeginCommands(test.name, "dispatch");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);
    if (layout.push_constant_size != 0) {
      Require(test.name, "dispatch",
              compiled.packed_user_data.size() * sizeof(u32) ==
                  layout.push_constant_size,
              "native user-data size does not match push-constant range");
      vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT,
                         layout.push_constant_offset, layout.push_constant_size,
                         compiled.packed_user_data.data());
    }
    vkCmdDispatch(cmd, test.dispatch_x, test.dispatch_y, test.dispatch_z);

    if (buffers != nullptr) {
      VkBufferMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      barrier.srcAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = buffer.buffer;
      barrier.offset = 0;
      barrier.size = buffer.size;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                           &barrier, 0, nullptr);
    }
    if (gds_buffer != nullptr) {
      VkBufferMemoryBarrier barrier{};
      barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      barrier.srcAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = gds_buffer->buffer;
      barrier.offset = 0;
      barrier.size = gds_buffer->size;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                           &barrier, 0, nullptr);
    }
    EndSubmitAndFree(test.name, "dispatch", cmd);
    if (flattened_buffer.buffer != VK_NULL_HANDLE) {
      DestroyBuffer(&flattened_buffer);
    }
    if (user_data_buffer.buffer != VK_NULL_HANDLE) {
      DestroyBuffer(&user_data_buffer);
    }
    vkDestroyDescriptorPool(m_device, descriptor_pool, nullptr);
    vkDestroyPipeline(m_device, pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, pipeline_layout, nullptr);
    vkDestroyDescriptorSetLayout(m_device, descriptor_layout, nullptr);
    vkDestroyShaderModule(m_device, module, nullptr);
  }

  std::vector<u32> RenderFragment(const GraphicsCase &test,
                                  const CompiledShader &fragment) {
    const auto vertex_spirv = TestSpv::MakePassthroughVertexSpirv();
    ValidateSpirv(test.name, vertex_spirv);

    Image target = CreateImage2D(test.name, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, {}, 4,
                                 VK_IMAGE_LAYOUT_GENERAL);
    const std::vector<u32> default_vertices = {
        0xbf800000u, 0xbf800000u, 0x3e800000u, 0x3f000000u, 0x3f400000u,
        0x3f800000u, 0x40400000u, 0xbf800000u, 0x3e800000u, 0x3f000000u,
        0x3f400000u, 0x3f800000u, 0xbf800000u, 0x40400000u, 0x3e800000u,
        0x3f000000u, 0x3f400000u, 0x3f800000u,
    };
    const auto &vertices =
        test.vertices.empty() ? default_vertices : test.vertices;
    Require(test.name, "graphics", vertices.size() == 18u,
            "graphics vertex buffer must contain three pos2/color4 vertices");
    auto vertex_buffer =
        CreateHostBuffer(test.name, vertices.size() * sizeof(u32),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertices);

    auto make_module = [&](const std::vector<u32> &spirv) {
      VkShaderModuleCreateInfo module_info{};
      module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      module_info.codeSize = spirv.size() * sizeof(u32);
      module_info.pCode = spirv.data();
      VkShaderModule module = VK_NULL_HANDLE;
      RequireVk(test.name, "graphics",
                vkCreateShaderModule(m_device, &module_info, nullptr, &module),
                "vkCreateShaderModule");
      return module;
    };
    VkShaderModule vertex_module = make_module(vertex_spirv);
    VkShaderModule fragment_module = make_module(fragment.spirv);

    VkAttachmentDescription attachment{};
    attachment.format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkRenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    RequireVk(
        test.name, "graphics",
        vkCreateRenderPass(m_device, &render_pass_info, nullptr, &render_pass),
        "vkCreateRenderPass");

    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &target.view;
    framebuffer_info.width = 1;
    framebuffer_info.height = 1;
    framebuffer_info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    RequireVk(
        test.name, "graphics",
        vkCreateFramebuffer(m_device, &framebuffer_info, nullptr, &framebuffer),
        "vkCreateFramebuffer");

    const auto &fragment_bind = fragment.program.bindings;
    VkPushConstantRange push_constant_range{};
    if (fragment_bind.push_constant_size > 0) {
      Require(test.name, "graphics",
              test.push_constants.size() * sizeof(u32) ==
                  fragment_bind.push_constant_size,
              "fragment push constant data size does not match reflection");
      push_constant_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
      push_constant_range.offset = fragment_bind.push_constant_offset;
      push_constant_range.size = fragment_bind.push_constant_size;
    }

    VkPipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipeline_layout_info.pushConstantRangeCount =
        push_constant_range.size != 0 ? 1u : 0u;
    pipeline_layout_info.pPushConstantRanges =
        push_constant_range.size != 0 ? &push_constant_range : nullptr;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    RequireVk(test.name, "graphics",
              vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr,
                                     &pipeline_layout),
              "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = 6u * sizeof(float);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attributes[2] = {};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attributes[1].offset = 2u * sizeof(float);
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent.width = 1;
    scissor.extent.height = 1;
    VkPipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState color_attachment{};
    color_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &color_attachment;

    VkGraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &color_blend;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    RequireVk(test.name, "graphics",
              vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1,
                                        &pipeline_info, nullptr, &pipeline),
              "vkCreateGraphicsPipelines");

    VkCommandBuffer cmd = BeginCommands(test.name, "graphics");
    VkClearValue clear{};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent.width = 1;
    begin.renderArea.extent.height = 1;
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (push_constant_range.size != 0) {
      vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                         fragment_bind.push_constant_offset,
                         fragment_bind.push_constant_size,
                         test.push_constants.data());
    }
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    EndSubmitAndFree(test.name, "graphics", cmd);
    target.layout = VK_IMAGE_LAYOUT_GENERAL;

    auto pixel = ReadImage(test.name, &target);
    pixel.resize(4);

    vkDestroyPipeline(m_device, pipeline, nullptr);
    vkDestroyPipelineLayout(m_device, pipeline_layout, nullptr);
    vkDestroyFramebuffer(m_device, framebuffer, nullptr);
    vkDestroyRenderPass(m_device, render_pass, nullptr);
    vkDestroyShaderModule(m_device, fragment_module, nullptr);
    vkDestroyShaderModule(m_device, vertex_module, nullptr);
    DestroyBuffer(&vertex_buffer);
    DestroyImage(&target);
    return pixel;
  }

private:
  void Init() {
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "ShaderRecompilerComputeTests";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info{};
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    RequireVk("VulkanHarness", "dispatch",
              vkCreateInstance(&instance_info, nullptr, &m_instance),
              "vkCreateInstance");

    u32 physical_count = 0;
    RequireVk("VulkanHarness", "dispatch",
              vkEnumeratePhysicalDevices(m_instance, &physical_count, nullptr),
              "vkEnumeratePhysicalDevices");
    Require("VulkanHarness", "dispatch", physical_count != 0,
            "no Vulkan physical devices");
    std::vector<VkPhysicalDevice> physical_devices(physical_count);
    RequireVk("VulkanHarness", "dispatch",
              vkEnumeratePhysicalDevices(m_instance, &physical_count,
                                         physical_devices.data()),
              "vkEnumeratePhysicalDevices");

    for (auto physical : physical_devices) {
      u32 queue_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, nullptr);
      std::vector<VkQueueFamilyProperties> queues(queue_count);
      vkGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count,
                                               queues.data());
      for (u32 i = 0; i < queue_count; i++) {
        if ((queues[i].queueFlags &
             (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) ==
            (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT)) {
          m_physical_device = physical;
          m_queue_family = i;
          break;
        }
      }
      if (m_physical_device != VK_NULL_HANDLE) {
        break;
      }
    }
    Require("VulkanHarness", "dispatch", m_physical_device != VK_NULL_HANDLE,
            "no Vulkan graphics+compute queue family");
    vkGetPhysicalDeviceMemoryProperties(m_physical_device,
                                        &m_memory_properties);

    VkPhysicalDeviceFeatures available_features{};
    vkGetPhysicalDeviceFeatures(m_physical_device, &available_features);
    Require("VulkanHarness", "dispatch",
            available_features.shaderStorageImageWriteWithoutFormat == VK_TRUE,
            "shaderStorageImageWriteWithoutFormat is not supported");
    Require("VulkanHarness", "dispatch",
            available_features.shaderStorageImageReadWithoutFormat == VK_TRUE,
            "shaderStorageImageReadWithoutFormat is not supported");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{};
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = m_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    VkDeviceCreateInfo device_info{};
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    VkPhysicalDeviceFeatures device_features{};
    device_features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    device_features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    device_info.pEnabledFeatures = &device_features;
    RequireVk(
        "VulkanHarness", "dispatch",
        vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device),
        "vkCreateDevice");
    vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = m_queue_family;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    RequireVk(
        "VulkanHarness", "dispatch",
        vkCreateCommandPool(m_device, &pool_info, nullptr, &m_command_pool),
        "vkCreateCommandPool");
  }

  void Destroy() {
    if (m_device != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(m_device);
      if (m_command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(m_device, m_command_pool, nullptr);
      }
      vkDestroyDevice(m_device, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) {
      vkDestroyInstance(m_instance, nullptr);
    }
  }

  bool FindMemoryType(u32 type_bits, VkMemoryPropertyFlags required,
                      u32 *index) const {
    for (u32 i = 0; i < m_memory_properties.memoryTypeCount; i++) {
      if ((type_bits & (1u << i)) == 0) {
        continue;
      }
      if ((m_memory_properties.memoryTypes[i].propertyFlags & required) ==
          required) {
        *index = i;
        return true;
      }
    }
    return false;
  }

  static VkAccessFlags AccessForLayout(VkImageLayout layout) {
    switch (layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
      return VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
      return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
      return VK_ACCESS_SHADER_READ_BIT;
    case VK_IMAGE_LAYOUT_GENERAL:
      return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    default:
      return 0;
    }
  }

  VkCommandBuffer BeginCommands(const char *shader_name, const char *stage) {
    VkCommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_alloc.commandPool = m_command_pool;
    cmd_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_alloc.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    RequireVk(shader_name, stage,
              vkAllocateCommandBuffers(m_device, &cmd_alloc, &cmd),
              "vkAllocateCommandBuffers");

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    RequireVk(shader_name, stage, vkBeginCommandBuffer(cmd, &begin),
              "vkBeginCommandBuffer");
    return cmd;
  }

  void EndSubmitAndFree(const char *shader_name, const char *stage,
                        VkCommandBuffer cmd) {
    RequireVk(shader_name, stage, vkEndCommandBuffer(cmd),
              "vkEndCommandBuffer");

    VkFenceCreateInfo fence_info{};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    RequireVk(shader_name, stage,
              vkCreateFence(m_device, &fence_info, nullptr, &fence),
              "vkCreateFence");

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    RequireVk(shader_name, stage, vkQueueSubmit(m_queue, 1, &submit, fence),
              "vkQueueSubmit");
    RequireVk(shader_name, stage,
              vkWaitForFences(m_device, 1, &fence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences");

    vkDestroyFence(m_device, fence, nullptr);
    vkFreeCommandBuffers(m_device, m_command_pool, 1, &cmd);
  }

  void AddImageBarrier(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout old_layout, VkImageLayout new_layout,
                       VkPipelineStageFlags src_stage,
                       VkPipelineStageFlags dst_stage, VkAccessFlags src_access,
                       VkAccessFlags dst_access, u32 mip_levels = 1) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                         1, &barrier);
  }

  void TransitionImage(const char *shader_name, Image *image,
                       VkImageLayout new_layout, VkPipelineStageFlags src_stage,
                       VkPipelineStageFlags dst_stage, VkAccessFlags src_access,
                       VkAccessFlags dst_access) {
    VkCommandBuffer cmd = BeginCommands(shader_name, "dispatch");
    AddImageBarrier(cmd, image->image, image->layout, new_layout, src_stage,
                    dst_stage, src_access, dst_access, image->mip_levels);
    EndSubmitAndFree(shader_name, "dispatch", cmd);
    image->layout = new_layout;
  }

  void UploadImage(const char *shader_name, Image *image, VkBuffer staging,
                   VkImageLayout final_layout) {
    UploadImageMips(shader_name, image, staging, final_layout);
  }

  void UploadImageMips(const char *shader_name, Image *image, VkBuffer staging,
                       VkImageLayout final_layout) {
    VkCommandBuffer cmd = BeginCommands(shader_name, "dispatch");
    AddImageBarrier(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                    VK_ACCESS_TRANSFER_WRITE_BIT, image->mip_levels);
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(image->mip_levels);
    VkDeviceSize offset = 0;
    for (u32 level = 0; level < image->mip_levels; level++) {
      VkBufferImageCopy copy{};
      copy.bufferOffset = offset;
      copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      copy.imageSubresource.mipLevel = level;
      copy.imageSubresource.baseArrayLayer = 0;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent.width = MipExtent(image->width, level);
      copy.imageExtent.height = MipExtent(image->height, level);
      copy.imageExtent.depth = 1;
      copies.push_back(copy);
      offset += static_cast<VkDeviceSize>(
          ImageMipDwordCount(image->width, image->height,
                             image->dwords_per_pixel, level) *
          sizeof(u32));
    }
    vkCmdCopyBufferToImage(cmd, staging, image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<u32>(copies.size()), copies.data());
    AddImageBarrier(cmd, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    final_layout, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, AccessForLayout(final_layout),
                    image->mip_levels);
    EndSubmitAndFree(shader_name, "dispatch", cmd);
    image->layout = final_layout;
  }

  Buffer CreateHostBuffer(const char *shader_name, VkDeviceSize size,
                          VkBufferUsageFlags usage,
                          const std::vector<u32> &contents) {
    Buffer ret;
    ret.size = std::max<VkDeviceSize>(size, sizeof(u32));

    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = ret.size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    RequireVk(shader_name, "dispatch",
              vkCreateBuffer(m_device, &buffer_info, nullptr, &ret.buffer),
              "vkCreateBuffer");

    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(m_device, ret.buffer, &req);
    u32 memory_type = 0;
    ret.coherent = FindMemoryType(req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  &memory_type);
    if (!ret.coherent) {
      Require(shader_name, "dispatch",
              FindMemoryType(req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, &memory_type),
              "no host-visible memory type for staging buffer");
    }

    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type;
    RequireVk(shader_name, "dispatch",
              vkAllocateMemory(m_device, &alloc, nullptr, &ret.memory),
              "vkAllocateMemory");
    RequireVk(shader_name, "dispatch",
              vkBindBufferMemory(m_device, ret.buffer, ret.memory, 0),
              "vkBindBufferMemory");
    if (!contents.empty()) {
      WriteBuffer(shader_name, ret, contents);
    }
    return ret;
  }

  void WriteBuffer(const char *shader_name, const Buffer &buffer,
                   const std::vector<u32> &contents) {
    void *data = nullptr;
    RequireVk(shader_name, "dispatch",
              vkMapMemory(m_device, buffer.memory, 0, buffer.size, 0, &data),
              "vkMapMemory");
    std::memcpy(data, contents.data(), contents.size() * sizeof(u32));
    if (!buffer.coherent) {
      VkMappedMemoryRange range{};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = buffer.memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      RequireVk(shader_name, "dispatch",
                vkFlushMappedMemoryRanges(m_device, 1, &range),
                "vkFlushMappedMemoryRanges");
    }
    vkUnmapMemory(m_device, buffer.memory);
  }

  VkInstance m_instance = VK_NULL_HANDLE;
  VkPhysicalDevice m_physical_device = VK_NULL_HANDLE;
  VkDevice m_device = VK_NULL_HANDLE;
  VkQueue m_queue = VK_NULL_HANDLE;
  VkCommandPool m_command_pool = VK_NULL_HANDLE;
  u32 m_queue_family = 0;
  VkPhysicalDeviceMemoryProperties m_memory_properties{};
};

void CompareWords(const TestCase &test, const char *stage,
                  const std::vector<u32> &expected,
                  const std::vector<u32> &actual) {
  if (actual == expected) {
    return;
  }
  std::ostringstream out;
  out << "expected [";
  for (size_t i = 0; i < expected.size(); i++) {
    out << (i == 0 ? "" : ", ") << Hex(expected[i]);
  }
  out << "] actual [";
  for (size_t i = 0; i < actual.size(); i++) {
    out << (i == 0 ? "" : ", ") << Hex(actual[i]);
  }
  out << "]";
  Fail(test.name, stage, out.str());
}

void CompareGraphicsWords(const GraphicsCase &test,
                          const std::vector<u32> &actual) {
  if (actual == test.expected_pixel) {
    return;
  }
  std::ostringstream out;
  out << "expected [";
  for (size_t i = 0; i < test.expected_pixel.size(); i++) {
    out << (i == 0 ? "" : ", ") << Hex(test.expected_pixel[i]);
  }
  out << "] actual [";
  for (size_t i = 0; i < actual.size(); i++) {
    out << (i == 0 ? "" : ", ") << Hex(actual[i]);
  }
  out << "]";
  Fail(test.name, "graphics readback", out.str());
}

void RunCase(VulkanHarness *vulkan, const TestCase &test) {
  auto compiled = CompileCase(test);
  if (test.image_descriptor_swizzle != DstSel(4, 5, 6, 7)) {
    Require(test.name, "resource specialization",
            !compiled.program.info.images.empty() &&
                compiled.program.info.images[0].storage_swizzle ==
                    test.image_descriptor_swizzle,
            "storage image descriptor swizzle did not reach the specialized program");
  }
  if (test.compile_only) {
    std::printf("[compute] %-32s ok\n", test.name);
    return;
  }
  const auto dwords = std::max<size_t>(
      {test.initial.size(), test.expected.size(), static_cast<size_t>(1)});
  auto buffer = vulkan->CreateStorageBuffer(test.name, test.initial, dwords);

  using Kind = ShaderRecompiler::IR::DescriptorBindingKind;
  auto Has = [&](Kind kind) {
    return ShaderRecompiler::IR::FindBinding(compiled.program.bindings, kind) !=
           nullptr;
  };
  VulkanHarness::Image sampled_image;
  VulkanHarness::Image storage_image;
  VulkanHarness::Image storage_image_uint;
  VulkanHarness::Buffer gds_buffer;
  VkSampler sampler = VK_NULL_HANDLE;
  const bool needs_sampled_image =
      Has(Kind::Sampled2D) || Has(Kind::Sampled2DArray) ||
      Has(Kind::Sampled3D) || Has(Kind::SampledUint2D) ||
      Has(Kind::SampledUint2DArray) || Has(Kind::SampledUint3D);
  const bool needs_storage_image =
      Has(Kind::Storage2D) || Has(Kind::Storage2DArray) ||
      Has(Kind::Storage3D) || Has(Kind::StorageUint2D) ||
      Has(Kind::StorageUint2DArray) || Has(Kind::StorageUint3D);
  const bool needs_sampler = Has(Kind::Samplers);
  const bool needs_gds = Has(Kind::Gds);
  if (needs_gds) {
    const auto gds_dwords = std::max<size_t>(
        {test.gds_initial.size(), test.expected_gds.size(), 1u});
    gds_buffer =
        vulkan->CreateStorageBuffer(test.name, test.gds_initial, gds_dwords);
  }
  if (needs_sampled_image) {
    if (!test.sampled_image_rgba_mips.empty()) {
      sampled_image = vulkan->CreateImage2DMips(
          test.name, test.image_width, test.image_height,
          VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT,
          test.sampled_image_rgba_mips, 4,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    } else {
      sampled_image = vulkan->CreateImage2D(
          test.name, test.image_width, test.image_height,
          test.sampled_image_format, VK_IMAGE_USAGE_SAMPLED_BIT,
          test.sampled_image_rgba, test.sampled_image_dwords_per_pixel,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
  }
  if (needs_storage_image) {
    storage_image = vulkan->CreateImage2D(
        test.name, test.image_width, test.image_height,
        test.storage_image_format, VK_IMAGE_USAGE_STORAGE_BIT,
        test.storage_image_rgba, test.storage_image_dwords_per_pixel,
        VK_IMAGE_LAYOUT_GENERAL);
    storage_image_uint = vulkan->CreateImage2D(
        test.name, test.image_width, test.image_height, VK_FORMAT_R32_UINT,
        VK_IMAGE_USAGE_STORAGE_BIT, test.storage_image_r32ui, 1,
        VK_IMAGE_LAYOUT_GENERAL);
  }
  if (needs_sampler) {
    sampler = vulkan->CreateNearestSampler(test.name);
  }

  vulkan->Dispatch(test, compiled, buffer, needs_gds ? &gds_buffer : nullptr,
                   needs_sampled_image ? &sampled_image : nullptr,
                   needs_storage_image ? &storage_image : nullptr,
                   needs_storage_image ? &storage_image_uint : nullptr,
                   sampler);
  auto actual = vulkan->ReadBuffer(test.name, buffer, test.expected.size());
  if (!test.expected_gds.empty()) {
    const auto gds_actual =
        vulkan->ReadBuffer(test.name, gds_buffer, test.expected_gds.size());
    CompareWords(test, "GDS readback", test.expected_gds, gds_actual);
  }
  if (!test.expected_storage_image_rgba.empty()) {
    auto image_actual = vulkan->ReadImage(test.name, &storage_image);
    image_actual.resize(test.expected_storage_image_rgba.size());
    CompareWords(test, "storage image readback",
                 test.expected_storage_image_rgba, image_actual);
  }
  if (!test.expected_storage_image_r32ui.empty()) {
    auto image_actual = vulkan->ReadImage(test.name, &storage_image_uint);
    image_actual.resize(test.expected_storage_image_r32ui.size());
    CompareWords(test, "uint storage image readback",
                 test.expected_storage_image_r32ui, image_actual);
  }
  if (sampler != VK_NULL_HANDLE) {
    vkDestroySampler(vulkan->Device(), sampler, nullptr);
  }
  vulkan->DestroyImage(&sampled_image);
  vulkan->DestroyImage(&storage_image);
  vulkan->DestroyImage(&storage_image_uint);
  vulkan->DestroyBuffer(&gds_buffer);
  vulkan->DestroyBuffer(&buffer);
  CompareWords(test, "readback", test.expected, actual);
  std::printf("[compute] %-32s ok\n", test.name);
}

void RunGraphicsCase(VulkanHarness *vulkan, const GraphicsCase &test) {
  auto compiled = CompileFragmentCase(test);
  auto actual = vulkan->RenderFragment(test, compiled);
  CompareGraphicsWords(test, actual);
  std::printf("[graphics] %-31s ok\n", test.name);
}

enum class CoverageClass {
  Covered,
  ControlOrMarker,
  NeedsAluCase,
  NeedsFloatCase,
  NeedsMemoryCase,
  NeedsImageCase,
  NeedsGraphicsStageCase,
};

bool IsCovered(const std::set<ShaderOpcode> &covered, ShaderOpcode opcode) {
  return covered.find(opcode) != covered.end();
}

CoverageClass ClassifyOpcode(ShaderOpcode opcode,
                             const std::set<ShaderOpcode> &covered) {
  using ShaderRecompiler::Decoder::Opcode;

  if (IsCovered(covered, opcode)) {
    return CoverageClass::Covered;
  }

  switch (opcode) {
  case Opcode::SGetpcB64:
  case Opcode::SSetpcB64:
  case Opcode::SNop:
  case Opcode::SWaitcnt:
  case Opcode::SBarrier:
  case Opcode::SBranch:
  case Opcode::SCbranchScc0:
  case Opcode::SCbranchScc1:
  case Opcode::SCbranchVccz:
  case Opcode::SCbranchVccnz:
  case Opcode::SCbranchExecz:
  case Opcode::SCbranchExecnz:
  case Opcode::SSendmsg:
  case Opcode::SSetregB32:
  case Opcode::SSleep:
  case Opcode::STtraceData:
  case Opcode::SInstPrefetch:
  case Opcode::SEndpgm:
    return CoverageClass::ControlOrMarker;

  case Opcode::VAddF32:
  case Opcode::VSubF32:
  case Opcode::VSubrevF32:
  case Opcode::VMulF32:
  case Opcode::VMacF32:
  case Opcode::VMadmkF32:
  case Opcode::VMadakF32:
  case Opcode::VMinF32:
  case Opcode::VMaxF32:
  case Opcode::VMadF32:
  case Opcode::VFmaF32:
  case Opcode::VMin3F32:
  case Opcode::VMax3F32:
  case Opcode::VMed3F32:
  case Opcode::VDot2cF32F16:
  case Opcode::VCvtF32I32:
  case Opcode::VCvtF32U32:
  case Opcode::VCvtU32F32:
  case Opcode::VCvtI32F32:
  case Opcode::VCvtF16F32:
  case Opcode::VCvtF32F16:
  case Opcode::VCvtU16F16:
  case Opcode::VCvtRpiI32F32:
  case Opcode::VCvtFlrI32F32:
  case Opcode::VCvtOffF32I4:
  case Opcode::VCvtF32Ubyte0:
  case Opcode::VCvtF32Ubyte1:
  case Opcode::VCvtF32Ubyte2:
  case Opcode::VCvtF32Ubyte3:
  case Opcode::VRcpF32:
  case Opcode::VFractF32:
  case Opcode::VTruncF32:
  case Opcode::VCeilF32:
  case Opcode::VRndneF32:
  case Opcode::VFloorF32:
  case Opcode::VExpF32:
  case Opcode::VLogF32:
  case Opcode::VRsqF32:
  case Opcode::VSqrtF32:
  case Opcode::VSinF32:
  case Opcode::VCosF32:
  case Opcode::VCubeidF32:
  case Opcode::VCubescF32:
  case Opcode::VCubetcF32:
  case Opcode::VCubemaF32:
  case Opcode::VLdexpF32:
  case Opcode::VCvtPkU8F32:
  case Opcode::VCvtPknormI16F32:
  case Opcode::VCvtPknormU16F32:
  case Opcode::VCvtPkrtzF16F32:
  case Opcode::VPkAddF16:
  case Opcode::VPkMulF16:
  case Opcode::VPkMinF16:
  case Opcode::VPkMaxF16:
  case Opcode::VPkFmaF16:
  case Opcode::VAddF16:
  case Opcode::VSubF16:
  case Opcode::VSubrevF16:
  case Opcode::VMulF16:
  case Opcode::VMaxF16:
  case Opcode::VMinF16:
  case Opcode::VMin3F16:
  case Opcode::VMax3F16:
  case Opcode::VMed3F16:
  case Opcode::VRcpF16:
  case Opcode::VRsqF16:
  case Opcode::VLogF16:
  case Opcode::VExpF16:
  case Opcode::VMadMixloF16:
  case Opcode::VMadMixhiF16:
  case Opcode::DsMinF32:
  case Opcode::DsMaxF32:
  case Opcode::VCmpFF32:
  case Opcode::VCmpLtF32:
  case Opcode::VCmpEqF32:
  case Opcode::VCmpLeF32:
  case Opcode::VCmpGtF32:
  case Opcode::VCmpLgF32:
  case Opcode::VCmpGeF32:
  case Opcode::VCmpOF32:
  case Opcode::VCmpUF32:
  case Opcode::VCmpNgeF32:
  case Opcode::VCmpNlgF32:
  case Opcode::VCmpNgtF32:
  case Opcode::VCmpNleF32:
  case Opcode::VCmpNeqF32:
  case Opcode::VCmpNltF32:
  case Opcode::VCmpTruF32:
  case Opcode::VCmpxLtF32:
  case Opcode::VCmpxEqF32:
  case Opcode::VCmpxLeF32:
  case Opcode::VCmpxGtF32:
  case Opcode::VCmpxLgF32:
  case Opcode::VCmpxGeF32:
  case Opcode::VCmpxNgeF32:
  case Opcode::VCmpxNlgF32:
  case Opcode::VCmpxNgtF32:
  case Opcode::VCmpxNleF32:
  case Opcode::VCmpxNeqF32:
  case Opcode::VCmpxNltF32:
  case Opcode::VCmpClassF32:
  case Opcode::VCmpLtF16:
  case Opcode::VCmpEqF16:
  case Opcode::VCmpLeF16:
  case Opcode::VCmpGtF16:
  case Opcode::VCmpLgF16:
  case Opcode::VCmpGeF16:
  case Opcode::VCmpNeqF16:
  case Opcode::VCmpxLtF16:
  case Opcode::VCmpxEqF16:
  case Opcode::VCmpxLeF16:
  case Opcode::VCmpxGtF16:
  case Opcode::VCmpxGeF16:
  case Opcode::VCmpxNeqF16:
  case Opcode::VCmpxNltF16:
    return CoverageClass::NeedsFloatCase;

  case Opcode::SLoadDword:
  case Opcode::SLoadDwordx2:
  case Opcode::SLoadDwordx4:
  case Opcode::SLoadDwordx8:
  case Opcode::SLoadDwordx16:
  case Opcode::SBufferLoadDword:
  case Opcode::SBufferLoadDwordx2:
  case Opcode::SBufferLoadDwordx4:
  case Opcode::SBufferLoadDwordx8:
  case Opcode::SBufferLoadDwordx16:
  case Opcode::BufferLoadFormatX:
  case Opcode::BufferLoadFormatXy:
  case Opcode::BufferLoadFormatXyz:
  case Opcode::BufferLoadFormatXyzw:
  case Opcode::BufferStoreFormatX:
  case Opcode::BufferStoreFormatXy:
  case Opcode::BufferStoreFormatXyz:
  case Opcode::BufferStoreFormatXyzw:
  case Opcode::BufferLoadUbyte:
  case Opcode::BufferLoadSbyte:
  case Opcode::BufferLoadUshort:
  case Opcode::BufferLoadSshort:
  case Opcode::BufferLoadDwordx2:
  case Opcode::BufferLoadDwordx3:
  case Opcode::BufferLoadDwordx4:
  case Opcode::BufferStoreByte:
  case Opcode::BufferStoreShort:
  case Opcode::BufferStoreDwordx2:
  case Opcode::BufferStoreDwordx3:
  case Opcode::BufferStoreDwordx4:
  case Opcode::TBufferLoadFormatX:
  case Opcode::TBufferLoadFormatXy:
  case Opcode::TBufferLoadFormatXyz:
  case Opcode::TBufferLoadFormatXyzw:
  case Opcode::TBufferStoreFormatX:
  case Opcode::TBufferStoreFormatXy:
  case Opcode::TBufferStoreFormatXyz:
  case Opcode::TBufferStoreFormatXyzw:
  case Opcode::BufferAtomicSwap:
  case Opcode::BufferAtomicAdd:
  case Opcode::BufferAtomicSub:
  case Opcode::BufferAtomicSMin:
  case Opcode::BufferAtomicUMin:
  case Opcode::BufferAtomicSMax:
  case Opcode::BufferAtomicUMax:
  case Opcode::BufferAtomicAnd:
  case Opcode::BufferAtomicOr:
  case Opcode::BufferAtomicXor:
  case Opcode::FlatLoadUbyte:
  case Opcode::FlatLoadSbyte:
  case Opcode::FlatLoadUshort:
  case Opcode::FlatLoadSshort:
  case Opcode::FlatLoadDword:
  case Opcode::FlatLoadDwordx2:
  case Opcode::FlatLoadDwordx3:
  case Opcode::FlatLoadDwordx4:
  case Opcode::FlatStoreByte:
  case Opcode::FlatStoreShort:
  case Opcode::FlatStoreDword:
  case Opcode::FlatStoreDwordx2:
  case Opcode::FlatStoreDwordx3:
  case Opcode::FlatStoreDwordx4:
  case Opcode::DsAddU32:
  case Opcode::DsAddRtnU32:
  case Opcode::DsSubU32:
  case Opcode::DsSubRtnU32:
  case Opcode::DsMinI32:
  case Opcode::DsMinRtnI32:
  case Opcode::DsMaxI32:
  case Opcode::DsMaxRtnI32:
  case Opcode::DsMinU32:
  case Opcode::DsMinRtnU32:
  case Opcode::DsMaxU32:
  case Opcode::DsMaxRtnU32:
  case Opcode::DsAndB32:
  case Opcode::DsAndRtnB32:
  case Opcode::DsOrB32:
  case Opcode::DsOrRtnB32:
  case Opcode::DsXorB32:
  case Opcode::DsXorRtnB32:
  case Opcode::DsWrxchgRtnB32:
  case Opcode::DsSwizzleB32:
  case Opcode::DsReadSbyte:
  case Opcode::DsReadUbyte:
  case Opcode::DsReadSshort:
  case Opcode::DsReadUshort:
  case Opcode::DsRead2B32:
  case Opcode::DsReadB32:
  case Opcode::DsReadB64:
  case Opcode::DsRead2B64:
  case Opcode::DsReadB96:
  case Opcode::DsReadB128:
  case Opcode::DsWriteByte:
  case Opcode::DsWriteShort:
  case Opcode::DsWrite2B32:
  case Opcode::DsWrite2St64B32:
  case Opcode::DsWriteB32:
  case Opcode::DsWriteB64:
  case Opcode::DsWriteB96:
  case Opcode::DsWriteB128:
  case Opcode::DsWriteAddtidB32:
  case Opcode::DsReadAddtidB32:
    return CoverageClass::NeedsMemoryCase;

  case Opcode::ImageGetResinfo:
  case Opcode::ImageGetLod:
  case Opcode::ImageLoad:
  case Opcode::ImageLoadMip:
  case Opcode::ImageStore:
  case Opcode::ImageStoreMip:
  case Opcode::ImageAtomicAdd:
  case Opcode::ImageAtomicUMin:
  case Opcode::ImageAtomicAnd:
  case Opcode::ImageAtomicOr:
  case Opcode::ImageAtomicXor:
  case Opcode::ImageSample:
  case Opcode::ImageGather4Lz:
  case Opcode::ImageGather4C:
  case Opcode::ImageGather4CLz:
  case Opcode::ImageGather4LzO:
  case Opcode::ImageGather4CO:
  case Opcode::ImageGather4CLzO:
    return CoverageClass::NeedsImageCase;

  case Opcode::VInterpP1F32:
  case Opcode::VInterpP2F32:
  case Opcode::VInterpMovF32:
  case Opcode::Exp:
    return CoverageClass::NeedsGraphicsStageCase;

  default:
    return CoverageClass::NeedsAluCase;
  }
}

const char *CoverageClassName(CoverageClass status) {
  switch (status) {
  case CoverageClass::Covered:
    return "covered";
  case CoverageClass::ControlOrMarker:
    return "control";
  case CoverageClass::NeedsAluCase:
    return "alu";
  case CoverageClass::NeedsFloatCase:
    return "float";
  case CoverageClass::NeedsMemoryCase:
    return "memory";
  case CoverageClass::NeedsImageCase:
    return "image";
  case CoverageClass::NeedsGraphicsStageCase:
    return "graphics";
  default:
    return "unknown";
  }
}

void PrintPendingOpcodes(CoverageClass status,
                         const std::vector<ShaderOpcode> &opcodes) {
  if (opcodes.empty()) {
    return;
  }

  std::printf("[coverage] pending_%s:", CoverageClassName(status));
  for (auto opcode : opcodes) {
    const auto name = ShaderRecompiler::Decoder::OpcodeToString(opcode);
    std::printf(" %s", name.c_str());
  }
  std::printf("\n");
}

void CheckOpcodeCoverage(const std::vector<TestCase> &tests,
                         const std::vector<GraphicsCase> &graphics_tests) {
  using ShaderRecompiler::Decoder::Opcode;

  std::set<Opcode> covered;
  for (const auto &test : tests) {
    for (auto opcode : test.opcodes) {
      covered.insert(opcode);
    }
  }
  for (const auto &test : graphics_tests) {
    for (auto opcode : test.opcodes) {
      covered.insert(opcode);
    }
  }

  uint32_t counts[7] = {};
  std::vector<ShaderOpcode> pending[7];
  for (auto value = static_cast<int>(Opcode::SMovB32);
       value <= static_cast<int>(Opcode::Exp); value++) {
    const auto opcode = static_cast<Opcode>(value);
    const auto status = ClassifyOpcode(opcode, covered);
    counts[static_cast<uint32_t>(status)]++;
    if (status != CoverageClass::Covered &&
        status != CoverageClass::ControlOrMarker) {
      pending[static_cast<uint32_t>(status)].push_back(opcode);
    }
  }

  std::printf(
      "[coverage] decoder opcodes: covered=%u control=%u alu_pending=%u "
      "float_pending=%u memory_pending=%u image_pending=%u "
      "graphics_pending=%u\n",
      counts[static_cast<uint32_t>(CoverageClass::Covered)],
      counts[static_cast<uint32_t>(CoverageClass::ControlOrMarker)],
      counts[static_cast<uint32_t>(CoverageClass::NeedsAluCase)],
      counts[static_cast<uint32_t>(CoverageClass::NeedsFloatCase)],
      counts[static_cast<uint32_t>(CoverageClass::NeedsMemoryCase)],
      counts[static_cast<uint32_t>(CoverageClass::NeedsImageCase)],
      counts[static_cast<uint32_t>(CoverageClass::NeedsGraphicsStageCase)]);
  PrintPendingOpcodes(
      CoverageClass::NeedsAluCase,
      pending[static_cast<uint32_t>(CoverageClass::NeedsAluCase)]);
  PrintPendingOpcodes(
      CoverageClass::NeedsFloatCase,
      pending[static_cast<uint32_t>(CoverageClass::NeedsFloatCase)]);
  PrintPendingOpcodes(
      CoverageClass::NeedsMemoryCase,
      pending[static_cast<uint32_t>(CoverageClass::NeedsMemoryCase)]);
  PrintPendingOpcodes(
      CoverageClass::NeedsImageCase,
      pending[static_cast<uint32_t>(CoverageClass::NeedsImageCase)]);
  PrintPendingOpcodes(
      CoverageClass::NeedsGraphicsStageCase,
      pending[static_cast<uint32_t>(CoverageClass::NeedsGraphicsStageCase)]);
}

TestCase IntegerAddSubMul() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeSMovB32(0, InlineU32(7)),
      EncodeSMovB32(1, InlineU32(3)),
      EncodeSop2(0x00, 2, 0, 1),
      EncodeSop2(0x01, 3, 2, InlineU32(1)),
      EncodeSop2(0x26, 4, 3, InlineU32(2)),
      EncodeVop1(0x01, 0, 4),
  };
  AppendBufferStoreDword(&code, 0, 30);
  AppendEnd(&code);
  return {"IntegerAddSubMul",
          code,
          {},
          {18},
          {O::SMovB32, O::SAddU32, O::SSubU32, O::SMulI32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase BitwiseOps() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeSMovB32(0, InlineU32(60)), EncodeSMovB32(1, InlineU32(15)),
      EncodeSop2(0x0e, 2, 0, 1),       EncodeSop2(0x10, 3, 0, 1),
      EncodeSop2(0x12, 4, 3, 2),       EncodeVop1(0x37, 0, 4),
  };
  AppendBufferStoreDword(&code, 0, 30);
  AppendEnd(&code);
  return {"BitwiseAndOrXorNot",
          code,
          {},
          {~0x33u},
          {O::SMovB32, O::SAndB32, O::SOrB32, O::SXorB32, O::VNotB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase Shifts() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeSMovB32(0, InlineU32(3)),
      EncodeSop2(0x1e, 1, 0, InlineU32(2)),
      EncodeSop2(0x20, 2, 1, InlineU32(1)),
      EncodeVop1(0x01, 0, 2),
  };
  AppendBufferStoreDword(&code, 0, 30);
  AppendEnd(&code);
  return {"Shifts",
          code,
          {},
          {6},
          {O::SMovB32, O::SLshlB32, O::SLshrB32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarShiftCountsMaskLowBits() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(0, InlineU32(1)));
  AppendSMovLiteral(&code, 1, 0x80000000u);
  code.push_back(EncodeSMovB32(2, InlineU32(32)));
  code.push_back(EncodeSMovB32(3, InlineU32(33)));
  code.push_back(EncodeSop2(0x1e, 10, 0, 2));
  code.push_back(EncodeSop2(0x1e, 11, 0, 3));
  code.push_back(EncodeSop2(0x20, 12, 1, 2));
  code.push_back(EncodeSop2(0x20, 13, 1, 3));
  code.push_back(EncodeSop2(0x22, 14, 1, 2));
  code.push_back(EncodeSop2(0x22, 15, 1, 3));

  for (u32 i = 0; i < 6u; i++) {
    AppendStoreSgpr(&code, 10u + i, i);
  }
  AppendEnd(&code);

  return {"ScalarShiftCountsMaskLowBits",
          code,
          {},
          {1, 2, 0x80000000u, 0x40000000u, 0x80000000u, 0xc0000000u},
          {O::SMovB32, O::SLshlB32, O::SLshrB32, O::SAshrI32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase Rdna2ScalarOpcodes() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(2, InlineU32(7)));
  code.push_back(EncodeSMovB32(106, InlineU32(16)));
  code.push_back(EncodeSop1(0x1d, 106, InlineU32(0)));
  AppendStoreSgpr(&code, 106, 0);
  code.push_back(EncodeSopk(0x13, 106, 0x1019u));
  AppendStoreSgpr(&code, 106, 1);
  code.push_back(EncodeSop2(0x02, 106, 2, 239u));
  AppendStoreSgpr(&code, 106, 2);
  code.push_back(EncodeSopp(0x0e, 0));
  code.push_back(EncodeSop2(0x02, 106, 239u, 2));
  AppendStoreSgpr(&code, 106, 3);
  AppendEnd(&code);

  return {"Rdna2ScalarOpcodes",
          code,
          {},
          {0x11u, 0x11u, 7u, 7u},
          {O::SMovB32, O::SBitset1B32, O::SSetregB32, O::SAddI32, O::SSleep,
           O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarExtendedArithmetic() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, 0xffffffffu);
  code.push_back(EncodeSMovB32(1, InlineU32(1)));
  code.push_back(EncodeSop2(0x00, 2, 0, 1));
  code.push_back(EncodeSMovB32(3, InlineU32(5)));
  code.push_back(EncodeSMovB32(4, InlineU32(6)));
  code.push_back(EncodeSop2(0x04, 5, 3, 4));
  code.push_back(EncodeSop2(0x02, 6, InlineU32(7), InlineU32(8)));
  code.push_back(EncodeSop2(0x03, 7, InlineU32(7), InlineU32(9)));
  AppendSMovLiteral(&code, 8, 0xfffffffbu);
  code.push_back(EncodeSMovB32(9, InlineU32(3)));
  code.push_back(EncodeSop2(0x06, 10, 8, 9));
  code.push_back(EncodeSop2(0x08, 11, 8, 9));
  AppendSMovLiteral(&code, 12, 0xfffffffeu);
  code.push_back(EncodeSMovB32(13, InlineU32(3)));
  code.push_back(EncodeSop2(0x07, 14, 12, 13));
  code.push_back(EncodeSop2(0x09, 15, 12, 13));
  code.push_back(EncodeSMovB32(16, 8));
  code.push_back(EncodeSop1(0x34, 16, 16));
  code.push_back(EncodeSopk(0x00, 17, 0xfff5u));
  code.push_back(EncodeSopk(0x10, 17, 0xfffdu));

  const u32 results[] = {2, 5, 6, 7, 10, 11, 14, 15, 16, 17};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreSgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"ScalarExtendedArithmetic",
          code,
          {},
          {0, 12, 15, 0xfffffffeu, 0xfffffffbu, 3, 3, 0xfffffffeu, 5, 33},
          {O::SMovB32, O::SAddU32, O::SAddcU32, O::SAddI32, O::SSubI32,
           O::SMinI32, O::SMaxI32, O::SMinU32, O::SMaxU32, O::SAbsI32,
           O::SMovkI32, O::SMulkI32, O::VMovB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase ScalarArithmeticSccCarryBorrowOverflow() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  auto set_scc = [&](bool value) {
    code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
  };
  auto capture_scc = [&](u32 dst_sgpr) {
    code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
  };
  auto append_case = [&](bool prior_scc, u32 instruction, u32 out_sgpr) {
    set_scc(prior_scc);
    code.push_back(instruction);
    capture_scc(out_sgpr);
  };

  code.push_back(EncodeSMovB32(0, InlineU32(0)));
  code.push_back(EncodeSMovB32(1, InlineU32(1)));
  append_case(false, EncodeSop2(0x01, 10, 0, 1), 20);

  code.push_back(EncodeSMovB32(2, InlineU32(5)));
  code.push_back(EncodeSMovB32(3, InlineU32(3)));
  append_case(true, EncodeSop2(0x01, 11, 2, 3), 21);

  AppendSMovLiteral(&code, 4, 0x7fffffffu);
  code.push_back(EncodeSMovB32(5, InlineU32(1)));
  append_case(false, EncodeSop2(0x02, 12, 4, 5), 22);

  code.push_back(EncodeSMovB32(6, InlineU32(1)));
  code.push_back(EncodeSMovB32(7, InlineU32(2)));
  append_case(true, EncodeSop2(0x02, 13, 6, 7), 23);

  AppendSMovLiteral(&code, 8, 0x80000000u);
  code.push_back(EncodeSMovB32(9, InlineU32(1)));
  append_case(false, EncodeSop2(0x03, 14, 8, 9), 24);

  code.push_back(EncodeSMovB32(15, InlineU32(5)));
  code.push_back(EncodeSMovB32(16, InlineU32(3)));
  append_case(true, EncodeSop2(0x03, 17, 15, 16), 25);

  AppendSMovLiteral(&code, 18, 0x7fffffffu);
  set_scc(false);
  code.push_back(EncodeSopk(0x0f, 18, 1));
  capture_scc(26);

  code.push_back(EncodeSMovB32(19, InlineU32(4)));
  set_scc(true);
  code.push_back(EncodeSopk(0x0f, 19, 0xfffeu));
  capture_scc(27);

  for (u32 i = 0; i < 8u; i++) {
    AppendStoreSgpr(&code, 20u + i, i);
  }
  AppendEnd(&code);

  return {"ScalarArithmeticSccCarryBorrowOverflow",
          code,
          {},
          {1, 0, 1, 0, 1, 0, 1, 0},
          {O::SMovB32, O::SSubU32, O::SAddI32, O::SSubI32, O::SCmpEqU32,
           O::SCselectB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarMinMaxSccComparisonEdges() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  auto set_scc = [&](bool value) {
    code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
  };
  auto capture_scc = [&](u32 dst_sgpr) {
    code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
  };
  auto append_case = [&](bool prior_scc, u32 instruction, u32 out_sgpr) {
    set_scc(prior_scc);
    code.push_back(instruction);
    capture_scc(out_sgpr);
  };

  AppendSMovLiteral(&code, 0, 0xfffffffeu);
  code.push_back(EncodeSMovB32(1, InlineU32(3)));
  append_case(false, EncodeSop2(0x06, 10, 0, 1), 20);
  append_case(true, EncodeSop2(0x06, 11, 1, 0), 21);
  append_case(false, EncodeSop2(0x08, 12, 1, 0), 22);
  append_case(true, EncodeSop2(0x08, 13, 0, 1), 23);

  code.push_back(EncodeSMovB32(2, InlineU32(2)));
  code.push_back(EncodeSMovB32(3, InlineU32(3)));
  append_case(false, EncodeSop2(0x07, 14, 2, 3), 24);
  append_case(true, EncodeSop2(0x07, 15, 3, 2), 25);
  append_case(false, EncodeSop2(0x09, 16, 3, 2), 26);
  append_case(true, EncodeSop2(0x09, 17, 2, 3), 27);

  for (u32 i = 0; i < 8u; i++) {
    AppendStoreSgpr(&code, 20u + i, i);
  }
  AppendEnd(&code);

  return {"ScalarMinMaxSccComparisonEdges",
          code,
          {},
          {1, 0, 1, 0, 1, 0, 1, 0},
          {O::SMovB32, O::SMinI32, O::SMaxI32, O::SMinU32, O::SMaxU32,
           O::SCmpEqU32, O::SCselectB32, O::VMovB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase ScalarAbsI32UpdatesScc() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  auto set_scc = [&](bool value) {
    code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
  };
  auto capture_scc = [&](u32 dst_sgpr) {
    code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
  };

  code.push_back(EncodeSMovB32(0, InlineU32(0)));
  set_scc(true);
  code.push_back(EncodeSop1(0x34, 1, 0));
  capture_scc(2);

  AppendSMovLiteral(&code, 3, 0xfffffffbu);
  set_scc(false);
  code.push_back(EncodeSop1(0x34, 4, 3));
  capture_scc(5);

  AppendStoreSgpr(&code, 1, 0);
  AppendStoreSgpr(&code, 2, 1);
  AppendStoreSgpr(&code, 4, 2);
  AppendStoreSgpr(&code, 5, 3);
  AppendEnd(&code);

  return {"ScalarAbsI32UpdatesScc",
          code,
          {},
          {0, 0, 5, 1},
          {O::SMovB32, O::SCmpEqU32, O::SAbsI32, O::SCselectB32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarShiftLeftAddSccCarryEdges() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  auto set_scc = [&](bool value) {
    code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
  };
  auto capture_scc = [&](u32 dst_sgpr) {
    code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
  };
  auto append_case = [&](bool prior_scc, u32 instruction, u32 out_sgpr) {
    set_scc(prior_scc);
    code.push_back(instruction);
    capture_scc(out_sgpr);
  };

  AppendSMovLiteral(&code, 0, 0x80000000u);
  AppendSMovLiteral(&code, 1, 0x40000000u);
  AppendSMovLiteral(&code, 2, 0x20000000u);
  AppendSMovLiteral(&code, 3, 0x10000000u);
  code.push_back(EncodeSMovB32(4, InlineU32(1)));
  code.push_back(EncodeSMovB32(5, InlineU32(0)));
  append_case(false, EncodeSop2(0x2e, 10, 0, 5), 20);
  append_case(true, EncodeSop2(0x2e, 11, 4, 4), 21);
  append_case(false, EncodeSop2(0x2f, 12, 1, 5), 22);
  append_case(true, EncodeSop2(0x2f, 13, 4, 4), 23);
  append_case(false, EncodeSop2(0x30, 14, 2, 5), 24);
  append_case(true, EncodeSop2(0x30, 15, 4, 4), 25);
  append_case(false, EncodeSop2(0x31, 16, 3, 5), 26);
  append_case(true, EncodeSop2(0x31, 17, 4, 4), 27);

  for (u32 i = 0; i < 8u; i++) {
    AppendStoreSgpr(&code, 20u + i, i);
  }
  AppendEnd(&code);

  return {"ScalarShiftLeftAddSccCarryEdges",
          code,
          {},
          {1, 0, 1, 0, 1, 0, 1, 0},
          {O::SMovB32, O::SLshl1AddU32, O::SLshl2AddU32, O::SLshl3AddU32,
           O::SLshl4AddU32, O::SCmpEqU32, O::SCselectB32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarCompareOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, 0xfffffffeu);
  code.push_back(EncodeSMovB32(1, InlineU32(1)));
  code.push_back(EncodeSMovB32(2, InlineU32(5)));
  code.push_back(EncodeSMovB32(3, InlineU32(2)));
  code.push_back(EncodeSMovB32(4, InlineU32(3)));
  AppendSMovLiteral(&code, 10, 0x00000001u);
  code.push_back(EncodeSMovB32(11, InlineU32(0)));
  code.push_back(EncodeSMovB32(12, InlineU32(2)));
  code.push_back(EncodeSMovB32(13, InlineU32(0)));

  u32 dst = 20;
  auto append_compare = [&](u32 opcode, u32 src0, u32 src1) {
    code.push_back(EncodeSopc(opcode, src0, src1));
    code.push_back(EncodeSop2(0x0a, dst++, InlineU32(1), InlineU32(0)));
  };

  append_compare(0x00, 2, 2);
  append_compare(0x01, 0, 1);
  append_compare(0x02, 1, 0);
  append_compare(0x03, 1, 1);
  append_compare(0x04, 0, 1);
  append_compare(0x05, 0, 0);
  append_compare(0x07, 3, 4);
  append_compare(0x08, 4, 3);
  append_compare(0x09, 4, 4);
  append_compare(0x0b, 3, 4);
  append_compare(0x13, 10, 12);

  for (u32 i = 0; i < 11u; i++) {
    AppendStoreSgpr(&code, 20u + i, i);
  }
  AppendEnd(&code);

  return {"ScalarCompareOps",
          code,
          {},
          std::vector<u32>(11, 1),
          {O::SMovB32, O::SCmpEqI32, O::SCmpLgI32, O::SCmpGtI32, O::SCmpGeI32,
           O::SCmpLtI32, O::SCmpLeI32, O::SCmpLgU32, O::SCmpGtU32, O::SCmpGeU32,
           O::SCmpLeU32, O::SCmpLgU64, O::SCselectB32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarShiftAddAndMaskOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(0, InlineU32(2)));
  code.push_back(EncodeSMovB32(1, InlineU32(3)));
  code.push_back(EncodeSop2(0x2e, 2, 0, 1));
  code.push_back(EncodeSop2(0x2f, 3, 0, 1));
  code.push_back(EncodeSop2(0x30, 4, 0, 1));
  code.push_back(EncodeSop2(0x31, 5, 0, 1));
  AppendSMovLiteral(&code, 6, 0xfffffff8u);
  code.push_back(EncodeSop2(0x22, 7, 6, InlineU32(2)));
  AppendSMovLiteral(&code, 8, 0xffffffffu);
  code.push_back(EncodeSMovB32(9, InlineU32(2)));
  code.push_back(EncodeSop2(0x35, 10, 8, 9));
  AppendSMovLiteral(&code, 12, 0x0f0f0f0fu);
  AppendSMovLiteral(&code, 13, 0x00ff00ffu);
  code.push_back(EncodeSop1(0x08, 14, 12));
  code.push_back(EncodeSop1(0x0a, 16, 12));

  AppendStoreSgpr(&code, 2, 0);
  AppendStoreSgpr(&code, 3, 1);
  AppendStoreSgpr(&code, 4, 2);
  AppendStoreSgpr(&code, 5, 3);
  AppendStoreSgpr(&code, 7, 4);
  AppendStoreSgpr(&code, 10, 5);
  AppendStoreSgprPair(&code, 14, 6);
  AppendStoreSgprPair(&code, 16, 8);
  AppendEnd(&code);

  return {"ScalarShiftAddAndMaskOps",
          code,
          {},
          {7, 11, 19, 35, 0xfffffffeu, 1, 0xf0f0f0f0u, 0xff00ff00u, 0x0f0f0f0fu,
           0x00ff00ffu},
          {O::SMovB32, O::SLshl1AddU32, O::SLshl2AddU32, O::SLshl3AddU32,
           O::SLshl4AddU32, O::SAshrI32, O::SMulHiU32, O::SNotB64, O::SWqmB64,
           O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarNotB64UpdatesScc() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  auto set_scc = [&](bool value) {
    code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
  };
  auto capture_scc = [&](u32 dst_sgpr) {
    code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
  };

  AppendSMovLiteral(&code, 0, 0xffffffffu);
  AppendSMovLiteral(&code, 1, 0xffffffffu);
  set_scc(true);
  code.push_back(EncodeSop1(0x08, 2, 0));
  capture_scc(4);

  AppendSMovLiteral(&code, 6, 0xfffffffeu);
  AppendSMovLiteral(&code, 7, 0xffffffffu);
  set_scc(false);
  code.push_back(EncodeSop1(0x08, 8, 6));
  capture_scc(10);

  AppendStoreSgpr(&code, 4, 0);
  AppendStoreSgpr(&code, 10, 1);
  AppendStoreSgprPair(&code, 2, 2);
  AppendStoreSgprPair(&code, 8, 4);
  AppendEnd(&code);

  return {"ScalarNotB64UpdatesScc",
          code,
          {},
          {0, 1, 0, 0, 1, 0},
          {O::SMovB32, O::SCmpEqU32, O::SNotB64, O::SCselectB32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarFlbitI32B64Gpu() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, 0x00000000u);
  AppendSMovLiteral(&code, 1, 0x80000000u);
  code.push_back(EncodeSop1(0x16, 20, 0));

  AppendSMovLiteral(&code, 2, 0x00008000u);
  AppendSMovLiteral(&code, 3, 0x00000000u);
  code.push_back(EncodeSop1(0x16, 21, 2));

  AppendSMovLiteral(&code, 4, 0x00000000u);
  AppendSMovLiteral(&code, 5, 0x00000000u);
  code.push_back(EncodeSop1(0x16, 22, 4));

  AppendSMovLiteral(&code, 14, 0x00000008u);
  AppendSMovLiteral(&code, 15, 0x00000000u);
  code.push_back(EncodeSop1(0x16, 106, 14));

  AppendStoreSgpr(&code, 20, 0);
  AppendStoreSgpr(&code, 21, 1);
  AppendStoreSgpr(&code, 22, 2);
  AppendStoreSgpr(&code, 106, 3);
  AppendEnd(&code);

  return {"ScalarFlbitI32B64Gpu",
          code,
          {},
          {0, 48, 0xffffffffu, 60},
          {O::SMovB32, O::SFlbitI32B64, O::VMovB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase ScalarSaveExecOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 106, 0xffffffffu);
  AppendSMovLiteral(&code, 107, 0xffffffffu);
  code.push_back(0xbe80246au);
  code.push_back(EncodeSop1(0x28, 2, 106));
  code.push_back(EncodeSMovB32(106, InlineU32(0)));
  code.push_back(EncodeSMovB32(107, InlineU32(0)));
  code.push_back(EncodeSop1(0x37, 4, 106));
  AppendSMovLiteral(&code, 106, 0x00000003u);
  code.push_back(EncodeSop1(0x3c, 8, 106));
  code.push_back(EncodeSop1(0x04, 10, 126));
  code.push_back(EncodeSMovB32(106, InlineU32(1)));
  code.push_back(EncodeSop1(0x44, 12, 106));
  code.push_back(EncodeSop1(0x04, 14, 126));
  AppendSMovLiteral(&code, 126, 0xffffffffu);
  AppendSMovLiteral(&code, 127, 0xffffffffu);

  AppendStoreSgprPair(&code, 0, 0);
  AppendStoreSgprPair(&code, 2, 2);
  AppendStoreSgprPair(&code, 4, 4);
  AppendStoreSgpr(&code, 8, 6);
  AppendStoreSgprPair(&code, 10, 7);
  AppendStoreSgpr(&code, 12, 9);
  AppendStoreSgprPair(&code, 14, 10);
  AppendStoreSgpr(&code, 253, 12);
  AppendEnd(&code);

  return {"ScalarSaveExecOps",
          code,
          {},
          {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
           0xffffffffu, 0xffffffffu, 0x00000003u, 0xffffffffu, 0x00000003u,
           0x00000002u, 0xffffffffu, 1},
          {O::SMovB32, O::SAndSaveexecB64, O::SOrn2SaveexecB64,
           O::SAndn1SaveexecB64, O::SAndSaveexecB32, O::SAndn1SaveexecB32,
           O::SMovB64, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarOrn2SaveexecUsesSourceOrNotExec() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 126, 0x0000000cu);
  AppendSMovLiteral(&code, 127, 0x80000000u);
  AppendSMovLiteral(&code, 0, 0x00000001u);
  AppendSMovLiteral(&code, 1, 0x00000001u);
  code.push_back(EncodeSop1(0x28, 2, 0));
  code.push_back(EncodeSop1(0x04, 4, 126));
  code.push_back(EncodeSMovB32(126, InlineU32(1)));
  code.push_back(EncodeSMovB32(127, InlineU32(0)));
  AppendStoreSgprPair(&code, 2, 0);
  AppendStoreSgprPair(&code, 4, 2);
  AppendStoreSgpr(&code, 253, 4);
  AppendEnd(&code);

  return {"ScalarOrn2SaveexecUsesSourceOrNotExec",
          code,
          {},
          {0x0000000cu, 0x80000000u, 0xfffffff3u, 0x7fffffffu, 1},
          {O::SMovB32, O::SOrn2SaveexecB64, O::SMovB64, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarGetpcWritesNextInstructionPc() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSop1(0x1f, 0, 0));
  AppendStoreSgprPair(&code, 0, 0);
  AppendEnd(&code);

  return {"ScalarGetpcWritesNextInstructionPc",
          code,
          {},
          {4, 0},
          {O::SGetpcB64, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarBitfieldPack() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(0, InlineU32(15)));
  code.push_back(EncodeSop1(0x0b, 1, 0));
  AppendSMovLiteral(&code, 2, 0xf0000000u);
  code.push_back(EncodeSop1(0x10, 3, 0));
  AppendSMovLiteral(&code, 19, 0xf0f00001u);
  code.push_back(EncodeSop1(0x0f, 20, 19));
  code.push_back(EncodeSop2(0x0a, 21, InlineU32(1), InlineU32(0)));
  code.push_back(EncodeSMovB32(22, InlineU32(0)));
  code.push_back(EncodeSop1(0x0f, 23, 22));
  code.push_back(EncodeSop2(0x0a, 24, InlineU32(1), InlineU32(0)));
  code.push_back(EncodeSMovB32(4, InlineU32(3)));
  code.push_back(EncodeSop1(0x3b, 5, 4));
  code.push_back(EncodeSop2(0x24, 7, InlineU32(4), InlineU32(8)));
  AppendSMovLiteral(&code, 8, 0x00f00000u);
  AppendSMovLiteral(&code, 9, 0x00040014u);
  code.push_back(EncodeSop2(0x27, 10, 8, 9));
  AppendSMovLiteral(&code, 11, 0xaaaabbbbu);
  AppendSMovLiteral(&code, 12, 0xccccddddu);
  code.push_back(EncodeSop2(0x32, 13, 11, 12));
  code.push_back(EncodeSop2(0x33, 14, 11, 12));
  code.push_back(EncodeSop2(0x34, 15, 11, 12));
  code.push_back(EncodeSopc(0x0d, InlineU32(4), InlineU32(2)));
  code.push_back(EncodeSop2(0x0a, 17, InlineU32(1), InlineU32(0)));
  code.push_back(EncodeSopc(0x0c, InlineU32(4), InlineU32(1)));
  code.push_back(EncodeSop2(0x0a, 18, InlineU32(1), InlineU32(0)));

  AppendStoreSgpr(&code, 1, 0);
  AppendStoreSgpr(&code, 3, 1);
  AppendStoreSgpr(&code, 20, 2);
  AppendStoreSgpr(&code, 21, 3);
  AppendStoreSgpr(&code, 23, 4);
  AppendStoreSgpr(&code, 24, 5);
  AppendStoreSgprPair(&code, 5, 6);
  AppendStoreSgpr(&code, 7, 8);
  AppendStoreSgpr(&code, 10, 9);
  AppendStoreSgpr(&code, 13, 10);
  AppendStoreSgpr(&code, 14, 11);
  AppendStoreSgpr(&code, 15, 12);
  AppendStoreSgpr(&code, 17, 13);
  AppendStoreSgpr(&code, 18, 14);
  AppendEnd(&code);

  return {"ScalarBitfieldPack",
          code,
          {},
          {0xf0000000u, 8, 9, 1, 0, 0, 0x0000000fu, 0, 0x00000f00u, 0x0000000fu,
           0xddddbbbbu, 0xccccbbbbu, 0xccccaaaau, 1, 1},
          {O::SMovB32, O::SBrevB32, O::SBcnt1I32B32, O::SBcnt1I32B64,
           O::SBitreplicateB64B32, O::SBfmB32, O::SBfeU32, O::SPackLlB32B16,
           O::SPackLhB32B16, O::SPackHhB32B16, O::SBitcmp0B32, O::SBitcmp1B32,
           O::SCselectB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarBrevB32PreservesScc() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  auto set_scc = [&](bool value) {
    code.push_back(EncodeSopc(0x06, InlineU32(1), InlineU32(value ? 1u : 0u)));
  };
  auto capture_scc = [&](u32 dst_sgpr) {
    code.push_back(EncodeSop2(0x0a, dst_sgpr, InlineU32(1), InlineU32(0)));
  };

  code.push_back(EncodeSMovB32(0, InlineU32(1)));
  set_scc(false);
  code.push_back(EncodeSop1(0x0b, 1, 0));
  capture_scc(2);

  code.push_back(EncodeSMovB32(3, InlineU32(0)));
  set_scc(true);
  code.push_back(EncodeSop1(0x0b, 4, 3));
  capture_scc(5);

  AppendStoreSgpr(&code, 1, 0);
  AppendStoreSgpr(&code, 2, 1);
  AppendStoreSgpr(&code, 4, 2);
  AppendStoreSgpr(&code, 5, 3);
  AppendEnd(&code);

  return {"ScalarBrevB32PreservesScc",
          code,
          {},
          {0x80000000u, 0, 0, 1},
          {O::SMovB32, O::SCmpEqU32, O::SBrevB32, O::SCselectB32, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase BitfieldExtractWidthPastEndEdges() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, 0xf0000000u);
  AppendSMovLiteral(&code, 1, (20u << 16u) | 20u);
  code.push_back(EncodeSop2(0x27, 2, 0, 1));
  AppendSMovLiteral(&code, 3, 0x80000000u);
  AppendSMovLiteral(&code, 4, (7u << 16u) | 31u);
  code.push_back(EncodeSop2(0x27, 5, 3, 4));
  AppendVMovLiteral(&code, 6, 0xf0000000u);
  AppendVMovU32(&code, 7, 28);
  AppendVMovU32(&code, 8, 8);
  AppendVop3(&code, 0x148, 9, Vgpr(6), Vgpr(7), Vgpr(8));
  AppendStoreSgpr(&code, 2, 0);
  AppendStoreSgpr(&code, 5, 1);
  AppendStoreVgpr(&code, 9, 2);
  AppendEnd(&code);

  return {"BitfieldExtractWidthPastEndEdges",
          code,
          {},
          {0x00000f00u, 1, 0x0000000fu},
          {O::SMovB32, O::SBfeU32, O::VMovB32, O::VBfeU32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase Scalar64BitOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, 0x0f0f0f0fu);
  AppendSMovLiteral(&code, 1, 0x00ff00ffu);
  AppendSMovLiteral(&code, 2, 0x33333333u);
  AppendSMovLiteral(&code, 3, 0x0f0f0f0fu);
  code.push_back(EncodeSop2(0x0f, 4, 0, 2));
  code.push_back(EncodeSop2(0x15, 6, 0, 2));
  code.push_back(EncodeSop2(0x11, 8, 0, 2));
  code.push_back(EncodeSop2(0x17, 10, 0, 2));
  code.push_back(EncodeSop2(0x13, 12, 0, 2));
  code.push_back(EncodeSop2(0x19, 14, 0, 2));
  code.push_back(EncodeSop2(0x1b, 16, 0, 2));
  code.push_back(EncodeSop2(0x1d, 18, 0, 2));
  code.push_back(EncodeSop1(0x04, 20, 0));
  code.push_back(EncodeSop2(0x1f, 22, 0, InlineU32(4)));
  code.push_back(EncodeSop2(0x21, 24, 0, InlineU32(8)));
  code.push_back(EncodeSop2(0x25, 26, InlineU32(36), InlineU32(4)));
  code.push_back(EncodeSop2(0x21, 34, 193u, InlineU32(1)));
  AppendSMovLiteral(&code, 28, 0x000c0004u);
  code.push_back(EncodeSop2(0x29, 30, 0, 28));
  code.push_back(EncodeSopc(0x06, 0, 0));
  code.push_back(EncodeSop2(0x0b, 32, 0, 2));

  const u32 result_pairs[] = {4,  6,  8,  10, 12, 14, 16, 18,
                              20, 22, 24, 26, 34, 30, 32};
  u32 out = 0;
  for (auto sgpr : result_pairs) {
    AppendStoreSgprPair(&code, sgpr, out);
    out += 2;
  }
  AppendEnd(&code);

  return {"Scalar64BitOps",
          code,
          {},
          {0x03030303u, 0x000f000fu, 0x0c0c0c0cu, 0x00f000f0u, 0x3f3f3f3fu,
           0x0fff0fffu, 0xcfcfcfcfu, 0xf0fff0ffu, 0x3c3c3c3cu, 0x0ff00ff0u,
           0xfcfcfcfcu, 0xfff0fff0u, 0xc0c0c0c0u, 0xf000f000u, 0xc3c3c3c3u,
           0xf00ff00fu, 0x0f0f0f0fu, 0x00ff00ffu, 0xf0f0f0f0u, 0x0ff00ff0u,
           0xff0f0f0fu, 0x0000ff00u, 0xfffffff0u, 0x000000ffu, 0xffffffffu,
           0x7fffffffu, 0x000000f0u, 0,           0x0f0f0f0fu, 0x00ff00ffu},
          {O::SMovB32, O::SMovB64, O::SAndB64, O::SAndn2B64, O::SOrB64,
           O::SOrn2B64, O::SXorB64, O::SNandB64, O::SNorB64, O::SXnorB64,
           O::SLshlB64, O::SLshrB64, O::SBfmB64, O::SBfeU64, O::SCmpEqU32,
           O::SCselectB64, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarAndn2B64SccBranch() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeSMovB32(0, InlineU32(3)),
      EncodeSMovB32(1, InlineU32(0)),
      EncodeSMovB32(2, InlineU32(1)),
      EncodeSMovB32(3, InlineU32(0)),
      EncodeSop2(0x15, 4, 0, 2),
      EncodeVop1(0x01, 0, InlineU32(1)),
      EncodeSopp(0x04, 1),
      EncodeVop1(0x01, 0, InlineU32(7)),
  };
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  return {"ScalarAndn2B64SccBranch",
          code,
          {},
          {7},
          {O::SMovB32, O::SAndn2B64, O::VMovB32, O::SCbranchScc0,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase ScalarLiteral() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, 0x12345678u);
  code.push_back(EncodeVop1(0x01, 0, 0));
  AppendBufferStoreDword(&code, 0, 30);
  AppendEnd(&code);
  return {"ScalarLiteral",
          code,
          {},
          {0x12345678u},
          {O::SMovB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMoves() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x00, 0, 0),
      EncodeVop1(0x01, 0, InlineU32(5)),
      EncodeVop1(0x01, 2, Vgpr(0)),
  };
  AppendBufferStoreDword(&code, 2, 30);
  AppendEnd(&code);
  return {"VectorRegisterMoves",
          code,
          {},
          {5},
          {O::VNop, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3MoveAppliesFloatSourceModifiers() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 24, 0x40000000u);
  AppendVop3(&code, 0x181, 1, 24, 0, 0, 0, 0, false, 0, 0x1);
  AppendStoreVgpr(&code, 1, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorVop3MoveAppliesFloatSourceModifiers";
  test.code = code;
  test.expected = {0xc0000000u};
  test.opcodes = {O::SMovB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm};
  test.required_spirv = {"OpFNegate"};
  return test;
}

TestCase VectorIntegerOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0xfffffff8u);
  code.push_back(EncodeVop1(0x01, 1, InlineU32(5)));
  code.push_back(EncodeVop1(0x01, 4, InlineU32(7)));
  AppendVMovLiteral(&code, 10, 0x01000001u);
  code.push_back(EncodeVop1(0x01, 11, InlineU32(2)));
  AppendVMovLiteral(&code, 12, 0x0f0f0f0fu);
  AppendVMovLiteral(&code, 13, 0x33333333u);
  AppendVMovLiteral(&code, 14, 0xfffffff0u);
  code.push_back(EncodeVop1(0x01, 15, InlineU32(4)));
  code.push_back(EncodeVop1(0x01, 29, InlineU32(15)));

  code.push_back(EncodeVop2(0x11, 2, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x12, 3, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x13, 5, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x14, 6, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x25, 7, Vgpr(4), 1));
  code.push_back(EncodeVop2(0x26, 8, Vgpr(4), 1));
  code.push_back(EncodeVop2(0x27, 9, InlineU32(3), 4));
  code.push_back(EncodeVop2(0x26, 35, 249, 11));
  code.push_back(EncodeVop2Sdwa(12, 6, 0, 0, 6));
  code.push_back(EncodeVop2(0x27, 36, 249, 11));
  code.push_back(EncodeVop2Sdwa(12, 6, 0, 0, 6));
  code.push_back(EncodeVop2(0x0b, 16, Vgpr(10), 11));
  code.push_back(EncodeVop2(0x09, 37, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x1b, 17, Vgpr(12), 13));
  code.push_back(EncodeVop2(0x1c, 18, Vgpr(12), 13));
  code.push_back(EncodeVop2(0x1d, 19, Vgpr(12), 13));
  code.push_back(EncodeVop2(0x1e, 20, Vgpr(12), 13));
  code.push_back(EncodeVop2(0x19, 21, Vgpr(11), 15));
  code.push_back(EncodeVop2(0x1a, 22, InlineU32(4), 11));
  code.push_back(EncodeVop2(0x15, 23, Vgpr(14), 15));
  code.push_back(EncodeVop2(0x16, 24, InlineU32(4), 14));
  code.push_back(EncodeVop2(0x17, 25, Vgpr(14), 15));
  code.push_back(EncodeVop2(0x18, 26, InlineU32(4), 14));
  code.push_back(EncodeVop1(0x37, 27, Vgpr(12)));
  code.push_back(EncodeVop1(0x38, 28, Vgpr(29)));
  code.push_back(EncodeVop1(0x3a, 30, Vgpr(11)));
  code.push_back(EncodeVop1(0x39, 32, Vgpr(11)));
  code.push_back(EncodeVopc(0xc2, Vgpr(4), 4));
  code.push_back(EncodeVop2(0x01, 33, InlineU32(3), 4));
  code.push_back(EncodeVop2(0x11, 34, 249, 4));
  code.push_back(EncodeVop2Sdwa(0));

  const u32 results[] = {2,  3,  5,  6,  7,  8,  9,  35, 36, 16, 37, 17, 18, 19,
                         20, 21, 22, 23, 24, 25, 26, 27, 28, 30, 32, 33, 34};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"VectorIntegerOps",
          code,
          {},
          {0xfffffff8u, 5,           5,           0xfffffff8u, 12,
           2,           4,           13,          0xfffffff3u, 2,
           0xffffffd8u, 0x03030303u, 0x3f3f3f3fu, 0x3c3c3c3cu, 0xc3c3c3c3u,
           32,          32,          0x0fffffffu, 0x0fffffffu, 0xffffffffu,
           0xffffffffu, 0xf0f0f0f0u, 0xf0000000u, 1,           30,
           7,           0xfffffff8u},
          {O::VMovB32,    O::VMinI32,     O::VMaxI32,          O::VMinU32,
           O::VMaxU32,    O::VAddNcU32,   O::VSubNcU32,        O::VSubrevNcU32,
           O::VMulU32U24, O::VMulI32I24,  O::VAndB32,          O::VOrB32,
           O::VXorB32,    O::VXnorB32,    O::VLshlB32,         O::VLshlrevB32,
           O::VLshrB32,   O::VLshrrevB32, O::VAshrI32,         O::VAshrrevI32,
           O::VNotB32,    O::VBfrevB32,   O::VFfblB32,         O::VFfbhU32,
           O::VCmpEqU32,  O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorShiftCountsMaskLowBits() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 0, 1);
  AppendVMovU32(&code, 1, 32);
  AppendVMovU32(&code, 2, 33);
  AppendVMovLiteral(&code, 3, 0x80000000u);
  code.push_back(EncodeVop2(0x19, 10, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x1a, 11, Vgpr(2), 0));
  code.push_back(EncodeVop2(0x15, 12, Vgpr(3), 1));
  code.push_back(EncodeVop2(0x16, 13, Vgpr(2), 3));
  code.push_back(EncodeVop2(0x17, 14, Vgpr(3), 1));
  code.push_back(EncodeVop2(0x18, 15, Vgpr(2), 3));

  for (u32 i = 0; i < 6u; i++) {
    AppendStoreVgpr(&code, 10u + i, i);
  }
  AppendEnd(&code);

  return {"VectorShiftCountsMaskLowBits",
          code,
          {},
          {1, 2, 0x80000000u, 0x40000000u, 0x80000000u, 0xc0000000u},
          {O::VMovB32, O::VLshlB32, O::VLshlrevB32, O::VLshrB32, O::VLshrrevB32,
           O::VAshrI32, O::VAshrrevI32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3IntegerOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeVop1(0x01, 0, InlineU32(2)));
  code.push_back(EncodeVop1(0x01, 1, InlineU32(3)));
  code.push_back(EncodeVop1(0x01, 2, InlineU32(4)));
  AppendVMovLiteral(&code, 3, 0xfffffff8u);
  code.push_back(EncodeVop1(0x01, 4, InlineU32(5)));
  AppendVMovLiteral(&code, 5, 0x11223344u);
  AppendVMovLiteral(&code, 6, 0x55667788u);
  AppendVMovLiteral(&code, 7, 0x0f0f0f0fu);
  AppendVMovLiteral(&code, 8, 0x33333333u);
  AppendVMovLiteral(&code, 9, 0xaaaaaaaau);
  AppendVMovLiteral(&code, 14, 0x00f00000u);
  code.push_back(EncodeVop1(0x01, 15, InlineU32(8)));
  AppendVMovLiteral(&code, 30, 0x00010000u);
  AppendVMovLiteral(&code, 31, 0x00010000u);

  AppendVop3(&code, 0x36d, 10, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x152, 11, Vgpr(3), Vgpr(4), Vgpr(0));
  AppendVop3(&code, 0x155, 12, Vgpr(3), Vgpr(4), Vgpr(0));
  AppendVop3(&code, 0x158, 13, Vgpr(3), Vgpr(4), Vgpr(0));
  AppendVop3(&code, 0x153, 16, Vgpr(3), Vgpr(4), Vgpr(0));
  AppendVop3(&code, 0x156, 17, Vgpr(3), Vgpr(4), Vgpr(0));
  AppendVop3(&code, 0x159, 18, Vgpr(3), Vgpr(4), Vgpr(0));
  AppendVop3(&code, 0x15d, 19, Vgpr(0), Vgpr(4), Vgpr(2));
  AppendVop3(&code, 0x346, 20, Vgpr(1), Vgpr(0), Vgpr(2));
  AppendVop3(&code, 0x347, 21, Vgpr(0), Vgpr(1), Vgpr(0));
  AppendVop3(&code, 0x345, 22, Vgpr(7), Vgpr(8), Vgpr(0));
  AppendVop3(&code, 0x36f, 23, Vgpr(1), Vgpr(2), Vgpr(0));
  AppendVop3(&code, 0x371, 24, Vgpr(7), Vgpr(8), Vgpr(2));
  AppendVop3(&code, 0x372, 25, Vgpr(7), Vgpr(8), Vgpr(2));
  AppendVop3(&code, 0x178, 26, Vgpr(7), Vgpr(8), Vgpr(2));
  AppendVop3(&code, 0x148, 27, Vgpr(14), InlineU32(20), Vgpr(2));
  AppendVop3(&code, 0x149, 28, Vgpr(3), Vgpr(2), Vgpr(2));
  AppendVop3(&code, 0x14a, 29, Vgpr(8), Vgpr(7), Vgpr(9));
  AppendVop3(&code, 0x14e, 32, Vgpr(5), Vgpr(6), Vgpr(15));
  AppendVop3(&code, 0x363, 33, Vgpr(2), Vgpr(15));
  AppendVop3(&code, 0x169, 34, Vgpr(30), Vgpr(31));
  AppendVop3(&code, 0x16a, 35, Vgpr(30), Vgpr(31));
  AppendVop3(&code, 0x16b, 36, Vgpr(30), Vgpr(31));
  AppendVop3(&code, 0x16c, 44, Vgpr(3), Vgpr(30));
  AppendVop3(&code, 0x142, 37, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x143, 38, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x30f, 39, Vgpr(0), Vgpr(1));
  AppendVop3(&code, 0x310, 40, Vgpr(0), Vgpr(1));
  AppendVop3(&code, 0x319, 41, Vgpr(0), Vgpr(1));

  const u32 results[] = {10, 11, 12, 13, 16, 17, 18, 19, 20, 21,
                         22, 23, 24, 25, 26, 27, 28, 29, 32, 33,
                         34, 35, 36, 44, 37, 38, 39, 40, 41};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"VectorVop3IntegerOps",
          code,
          {},
          {9,
           0xfffffff8u,
           5,
           2,
           2,
           0xfffffff8u,
           5,
           7,
           16,
           20,
           0x3c3c3c3eu,
           0x32,
           0x03030307u,
           0x3f3f3f3fu,
           0x3c3c3c38u,
           0x0fu,
           0x0fu,
           0x8b8b8b8bu,
           0x44556677u,
           0x00000f00u,
           0,
           1,
           0,
           0xffffffffu,
           10,
           10,
           5,
           0xffffffffu,
           1},
          {O::VMovB32,    O::VAdd3U32,    O::VMin3I32,         O::VMax3I32,
           O::VMed3I32,   O::VMin3U32,    O::VMax3U32,         O::VMed3U32,
           O::VSadU32,    O::VLshlAddU32, O::VAddLshlU32,      O::VXadU32,
           O::VLshlOrB32, O::VAndOrB32,   O::VOr3B32,          O::VXor3B32,
           O::VBfeU32,    O::VBfeI32,     O::VBfiB32,          O::VAlignbitB32,
           O::VBfmB32,    O::VMulLoU32,   O::VMulHiU32,        O::VMulLoI32,
           O::VMulHiI32,  O::VMadI32I24,  O::VMadU32U24,       O::VAddI32,
           O::VSubI32,    O::VSubrevI32,  O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorBfeI32ArithmeticShiftMasksField() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x80000000u);
  AppendVMovLiteral(&code, 1, 0xfffffff8u);
  AppendVMovU32(&code, 2, 31);
  AppendVMovU32(&code, 3, 1);
  AppendVMovU32(&code, 4, 3);
  AppendVMovU32(&code, 5, 4);
  AppendVop3(&code, 0x149, 10, Vgpr(0), Vgpr(2), Vgpr(3));
  AppendVop3(&code, 0x149, 11, Vgpr(1), Vgpr(4), Vgpr(5));
  AppendStoreVgpr(&code, 10, 0);
  AppendStoreVgpr(&code, 11, 1);
  AppendEnd(&code);

  return {"VectorBfeI32ArithmeticShiftMasksField",
          code,
          {},
          {1, 0x0fu},
          {O::VMovB32, O::VBfeI32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCarryAndBitCountOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0xffffffffu);
  code.push_back(EncodeVop1(0x01, 1, InlineU32(0)));
  AppendVMovLiteral(&code, 3, 0x0000f0f0u);
  code.push_back(EncodeVop1(0x01, 4, InlineU32(5)));

  code.push_back(EncodeVopc(0xc7, Vgpr(1), 1));
  code.push_back(EncodeVop2(0x28, 2, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x22, 5, Vgpr(3), 4));
  AppendVMovLiteral(&code, 6, 0xffffffffu);
  code.push_back(EncodeVop2(0x23, 7, Vgpr(6), 4));
  code.push_back(EncodeVop2(0x24, 8, Vgpr(6), 4));

  AppendStoreVgpr(&code, 2, 0);
  AppendStoreSgpr(&code, 106, 1);
  AppendStoreVgpr(&code, 5, 2);
  AppendStoreVgpr(&code, 7, 3);
  AppendStoreVgpr(&code, 8, 4);
  AppendEnd(&code);

  return {"VectorCarryAndBitCountOps",
          code,
          {},
          {0, 1, 13, 5, 5},
          {O::VMovB32, O::VCmpTU32, O::VAddcU32, O::VBcntU32B32,
           O::VMbcntLoU32B32, O::VMbcntHiU32B32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase VectorMbcntUsesThreadMask() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),
      EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1),
      EncodeVop2(0x1a, 3, InlineU32(2), 1),
  };
  AppendVMovLiteral(&code, 2, 0xffffffffu);
  code.push_back(EncodeVop1(0x01, 4, InlineU32(0)));
  code.push_back(EncodeVop2(0x23, 5, Vgpr(2), 4));
  code.push_back(EncodeVop2(0x24, 6, Vgpr(2), 5));
  AppendBufferStoreDword(&code, 6, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorMbcntUsesThreadMask";
  test.code = code;
  test.expected = {0, 1, 2, 3, 0, 1, 2, 3};
  test.opcodes = {O::VMovB32,        O::VLshlrevB32,    O::VAddNcU32,
                  O::VMbcntLoU32B32, O::VMbcntHiU32B32, O::BufferStoreDword,
                  O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  test.dispatch_x = 2;
  return test;
}

TestCase VectorAddcUsesPerLaneCarryIn() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),
      EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1),
      EncodeVopc(0xc7, Vgpr(0), 0),
      EncodeVop1(0x01, 2, InlineU32(0)),
      EncodeVop2(0x28, 3, Vgpr(2), 2),
      EncodeVop2(0x1a, 4, InlineU32(2), 1),
  };
  AppendBufferStoreDword(&code, 3, 4);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorAddcUsesPerLaneCarryIn";
  test.code = code;
  test.expected = std::vector<u32>(8, 1);
  test.opcodes = {O::VMovB32,  O::VLshlrevB32,      O::VAddNcU32, O::VCmpTU32,
                  O::VAddcU32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  test.dispatch_x = 2;
  return test;
}

TestCase VectorAddcWritesPerLaneCarryOut() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),
      EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1),
      EncodeVopc(0xc0, Vgpr(0), 0),
  };
  AppendVMovLiteral(&code, 2, 0xffffffffu);
  code.push_back(EncodeVop1(0x01, 3, InlineU32(1)));
  code.push_back(EncodeVop2(0x28, 5, Vgpr(2), 3));
  code.push_back(EncodeVop1(0x01, 6, 106));
  code.push_back(EncodeVop2(0x1a, 4, InlineU32(2), 1));
  AppendBufferStoreDword(&code, 6, 4);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorAddcWritesPerLaneCarryOut";
  test.code = code;
  test.expected = std::vector<u32>(8, 0x0fu);
  test.opcodes = {O::VMovB32,  O::VLshlrevB32,      O::VAddNcU32, O::VCmpFU32,
                  O::VAddcU32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  test.dispatch_x = 2;
  return test;
}

TestCase VectorVop3BCarryOutWritesSgprMask() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 1, 0xffffffffu);
  AppendVMovU32(&code, 2, 1);
  AppendVMovU32(&code, 3, 0);

  AppendVop3B(&code, 0x30fu, 10, 0, Vgpr(1), Vgpr(2));
  AppendStoreSgprAtLaneDwordOffset(&code, 0, 0, 0);
  AppendVop3B(&code, 0x310u, 11, 0, Vgpr(3), Vgpr(2));
  AppendStoreSgprAtLaneDwordOffset(&code, 0, 0, 4);
  AppendVop3B(&code, 0x319u, 12, 0, Vgpr(2), Vgpr(3));
  AppendStoreSgprAtLaneDwordOffset(&code, 0, 0, 8);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorVop3BCarryOutWritesSgprMask";
  test.code = code;
  test.expected = std::vector<u32>(12, 0x0fu);
  test.opcodes = {O::VMovB32,    O::VAddI32,          O::VSubI32,
                  O::VSubrevI32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  return test;
}

TestCase VectorVop3BCarryOutUsesEncodedSdst() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 1, 0xffffffffu);
  AppendVMovU32(&code, 2, 1);
  AppendVMovU32(&code, 3, 0);

  AppendVop3B(&code, 0x30fu, 10, 20, Vgpr(1), Vgpr(2));
  AppendStoreSgprAtLaneDwordOffset(&code, 20, 0, 0);
  AppendVop3B(&code, 0x310u, 11, 22, Vgpr(3), Vgpr(2));
  AppendStoreSgprAtLaneDwordOffset(&code, 22, 0, 4);
  AppendVop3B(&code, 0x319u, 12, 24, Vgpr(2), Vgpr(3));
  AppendStoreSgprAtLaneDwordOffset(&code, 24, 0, 8);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorVop3BCarryOutUsesEncodedSdst";
  test.code = code;
  test.expected = std::vector<u32>(12, 0x0fu);
  test.opcodes = {O::VMovB32,    O::VAddI32,          O::VSubI32,
                  O::VSubrevI32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  return test;
}

TestCase VectorVop3BSubCoU32UsesRdna2Opcode310() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 0);
  AppendVMovU32(&code, 2, 1);
  AppendVMovU32(&code, 3, 1);
  AppendVMovLiteral(&code, 4, 0xffffffffu);
  AppendVMovLiteral(&code, 5, 0x80000000u);

  code.push_back(EncodeVopc(0xc2, InlineU32(1), 0));
  code.push_back(EncodeVop2(0x01, 1, Vgpr(1), Vgpr(3)));
  code.push_back(EncodeVopc(0xc2, InlineU32(2), 0));
  code.push_back(EncodeVop2(0x01, 1, Vgpr(1), Vgpr(4)));
  code.push_back(EncodeVopc(0xc2, InlineU32(3), 0));
  code.push_back(EncodeVop2(0x01, 1, Vgpr(1), Vgpr(5)));
  code.push_back(EncodeVop2(0x01, 2, Vgpr(2), Vgpr(4)));

  AppendVop3B(&code, 0x310u, 10, 20, Vgpr(1), Vgpr(2));
  AppendStoreVgprAtLaneDwordOffset(&code, 10, 0, 0);
  AppendStoreSgprAtLaneDwordOffset(&code, 20, 0, 4);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorVop3BSubCoU32UsesRdna2Opcode310";
  test.code = code;
  test.expected = {0xffffffffu, 0, 0xfffffffeu, 0x80000001u, 9, 9, 9, 9};
  test.opcodes = {O::VMovB32, O::VCmpEqU32,        O::VCndmaskB32,
                  O::VSubI32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  return test;
}

TestCase VectorMadU64U32UnsignedCarryOut() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 1, 0xffffffffu);
  AppendVMovU32(&code, 2, 2);
  AppendVMovU32(&code, 4, 2);
  AppendVMovLiteral(&code, 5, 0xffffffffu);
  AppendVop3B(&code, 0x176u, 10, 20, Vgpr(1), Vgpr(2), Vgpr(4));
  AppendStoreVgpr(&code, 10, 0);
  AppendStoreVgpr(&code, 11, 1);
  AppendStoreSgprPair(&code, 20, 2);
  AppendEnd(&code);

  return {"VectorMadU64U32UnsignedCarryOut",
          code,
          {},
          {0x00000000u, 0x00000001u, 0x00000001u, 0x00000000u},
          {O::VMovB32, O::VMadU64U32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorLaneAndPackedOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x12345678u);
  code.push_back(EncodeVop1(0x01, 1, InlineU32(0)));
  AppendVMovLiteral(&code, 4, 0x40003c00u);
  AppendVMovLiteral(&code, 5, 0x44004200u);
  AppendVMovLiteral(&code, 6, 0x3c003c00u);
  AppendVMovLiteral(&code, 7, 0x3f800000u);
  AppendVMovLiteral(&code, 8, 0x40000000u);
  AppendVMovLiteral(&code, 9, 0x12345678u);
  AppendVMovLiteral(&code, 10, 0xabcdef01u);

  code.push_back(EncodeVop1(0x02, 0, Vgpr(0)));
  AppendVop3(&code, 0x360, 1, Vgpr(0), InlineU32(0));
  AppendVop3(&code, 0x361, 1, 0, InlineU32(0));
  AppendVop3(&code, 0x377, 2, Vgpr(0), InlineU32(0), InlineU32(0));
  code.push_back(EncodeVop2(0x2f, 3, Vgpr(7), 8));
  AppendVop3p(&code, 0x0f, 11, Vgpr(4), Vgpr(5), 0, 0x3);
  AppendVop3p(&code, 0x10, 12, Vgpr(4), Vgpr(5), 0, 0x3);
  AppendVop3p(&code, 0x11, 13, Vgpr(4), Vgpr(5), 0, 0x3);
  AppendVop3p(&code, 0x12, 14, Vgpr(4), Vgpr(5), 0, 0x3);
  AppendVop3p(&code, 0x0e, 15, Vgpr(4), Vgpr(5), Vgpr(6), 0x7);
  AppendVop3(&code, 0x362, 16, Vgpr(7), InlineU32(1));
  AppendVop3(&code, 0x36a, 17, Vgpr(9), Vgpr(10));

  AppendStoreSgpr(&code, 0, 0);
  AppendStoreSgpr(&code, 1, 1);
  const u32 results[] = {1, 2, 3, 11, 12, 13, 14, 15, 16, 17};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i + 2u);
  }
  AppendEnd(&code);

  return {"VectorLaneAndPackedOps",
          code,
          {},
          {0x12345678u, 0x12345678u, 0x12345678u, 0x12345678u, 0x40003c00u,
           0x46004400u, 0x48004200u, 0x40003c00u, 0x44004200u, 0x48804400u,
           0x40000000u, 0xef015678u},
          {O::VMovB32, O::VReadfirstlaneB32, O::VReadlaneB32, O::VWritelaneB32,
           O::VPermlane16B32, O::VCvtPkrtzF16F32, O::VPkAddF16, O::VPkMulF16,
           O::VPkMinF16, O::VPkMaxF16, O::VPkFmaF16, O::VLdexpF32,
           O::VCvtPkU16U32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase CvtPkU8F32PacksSelectedByte() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  // RDNA2 V_CVT_PK_U8_F32: uint8(S0) replaces byte S1[1:0] in S2.
  AppendVMovLiteral(&code, 0, 0x414c0000u); // 12.75f truncates to 12.
  AppendVMovLiteral(&code, 1, 0x43960000u); // 300.0f saturates to 255.
  AppendVMovLiteral(&code, 2, 0xbf800000u); // Negative values saturate to zero.
  AppendVMovLiteral(&code, 3, 0x7fc00000u); // NaN saturates to zero.
  AppendVMovLiteral(&code, 4, 0x11223344u);

  AppendVop3(&code, 0x15eu, 10, Vgpr(0), InlineU32(0), Vgpr(4));
  AppendVop3(&code, 0x15eu, 11, Vgpr(1), InlineU32(5), Vgpr(4));
  AppendVop3(&code, 0x15eu, 12, Vgpr(2), InlineU32(2), Vgpr(4));
  AppendVop3(&code, 0x15eu, 13, Vgpr(3), InlineU32(3), Vgpr(4));
  for (u32 i = 0; i < 4; i++) {
    AppendStoreVgpr(&code, 10 + i, i);
  }
  AppendEnd(&code);

  return {"CvtPkU8F32PacksSelectedByte",
          code,
          {},
          {0x1122330cu, 0x1122ff44u, 0x11003344u, 0x00223344u},
          {O::VMovB32, O::VCvtPkU8F32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase CvtPkrtzF16F32SubnormalRoundsTowardZero() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x33c00000u);
  AppendVMovLiteral(&code, 1, 0xb3c00000u);
  code.push_back(EncodeVop2(0x2f, 2, Vgpr(0), 1));
  AppendStoreVgpr(&code, 2, 0);
  AppendEnd(&code);

  return {"CvtPkrtzF16F32SubnormalRoundsTowardZero",
          code,
          {},
          {0x80010001u},
          {O::VMovB32, O::VCvtPkrtzF16F32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase PackedMinMaxF16NanAndSignedZeroEdges() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x80004000u);
  AppendVMovLiteral(&code, 1, 0x00007e00u);
  AppendVMovLiteral(&code, 2, 0x00004000u);
  AppendVMovLiteral(&code, 3, 0x80007e00u);
  AppendVMovLiteral(&code, 4, 0x40007d01u);
  AppendVMovLiteral(&code, 5, 0x7d014000u);

  AppendVop3p(&code, 0x11, 10, Vgpr(0), Vgpr(1), 0, 0x3);
  AppendVop3p(&code, 0x12, 11, Vgpr(2), Vgpr(3), 0, 0x3);
  AppendVop3p(&code, 0x11, 12, Vgpr(4), Vgpr(5), 0, 0x3);
  AppendVop3p(&code, 0x12, 13, Vgpr(4), Vgpr(5), 0, 0x3);

  for (u32 i = 0; i < 4; i++) {
    AppendStoreVgpr(&code, 10 + i, i);
  }
  AppendEnd(&code);

  return {"PackedMinMaxF16NanAndSignedZeroEdges",
          code,
          {},
          {0x80004000u, 0x00004000u, 0x7f017f01u, 0x7f017f01u},
          {O::VMovB32, O::VPkMinF16, O::VPkMaxF16, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase VectorMinMaxF16Ops() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0xaaaa4000u); // low=2.0h
  AppendVMovLiteral(&code, 1, 0xbbbb3c00u); // low=1.0h
  AppendVMovLiteral(&code, 2, 0x12345678u);
  AppendVMovLiteral(&code, 3, 0x87654321u);
  AppendVMovLiteral(&code, 4, 0x5555aaaau);
  AppendVMovLiteral(&code, 5, 0x6666bbbbu);
  AppendVMovLiteral(&code, 6, 0x7777bbbbu);
  AppendVMovLiteral(&code, 7, 0x8888bbbbu);
  AppendVMovLiteral(&code, 8, 0x9999bbbbu);
  AppendVMovLiteral(&code, 9, 0x99990000u);
  code.push_back(EncodeVop2(0x32, 5, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x33, 6, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x34, 7, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x35, 4, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x39, 2, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x3a, 3, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x35, 9, 249, 1));
  code.push_back(EncodeVop2Sdwa(0, 4, 2, 4, 4));
  code.push_back(0x660000f9u);
  code.push_back(EncodeVop2Sdwa(0, 4, 2, 4, 4));
  AppendStoreVgpr(&code, 5, 0);
  AppendStoreVgpr(&code, 6, 1);
  AppendStoreVgpr(&code, 7, 2);
  AppendStoreVgpr(&code, 0, 3);
  AppendStoreVgpr(&code, 4, 4);
  AppendStoreVgpr(&code, 2, 5);
  AppendStoreVgpr(&code, 3, 6);
  AppendStoreVgpr(&code, 9, 7);
  AppendEnd(&code);

  return {"VectorMinMaxF16Ops",
          code,
          {},
          {0x66664200u, 0x77773c00u, 0x8888bc00u, 0xaaaa0000u, 0x55554000u,
           0x12344000u, 0x87653c00u, 0x99994000u},
          {O::VMovB32, O::VAddF16, O::VSubF16, O::VSubrevF16, O::VMulF16,
           O::VMaxF16, O::VMinF16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCvtU16F16Sdwa() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x45004200u); // low=3.0h, high=5.0h
  AppendVMovLiteral(&code, 1, 0xbc007c00u); // low=+inf, high=-1.0h
  AppendVMovLiteral(&code, 2, 0x00007e00u); // low=qNaN
  AppendVMovLiteral(&code, 10, 0x12345678u);
  AppendVMovLiteral(&code, 11, 0x87654321u);
  AppendVMovLiteral(&code, 12, 0xabcd1111u);
  AppendVMovLiteral(&code, 13, 0x77772222u);
  AppendVMovLiteral(&code, 14, 0x5555aaaau);
  code.push_back(EncodeVop1(0x52, 10, 249));
  code.push_back(EncodeVop1Sdwa(0, 4, 2, 4));
  code.push_back(EncodeVop1(0x52, 11, 249));
  code.push_back(EncodeVop1Sdwa(0, 5, 2, 5));
  code.push_back(EncodeVop1(0x52, 12, 249));
  code.push_back(EncodeVop1Sdwa(1, 4, 2, 4));
  code.push_back(EncodeVop1(0x52, 13, 249));
  code.push_back(EncodeVop1Sdwa(1, 4, 2, 5));
  code.push_back(EncodeVop1(0x52, 14, 249));
  code.push_back(EncodeVop1Sdwa(2, 4, 2, 4));
  for (u32 i = 0; i < 5; i++) {
    AppendStoreVgpr(&code, 10 + i, i);
  }
  AppendEnd(&code);

  return {"VectorCvtU16F16Sdwa",
          code,
          {},
          {0x12340003u, 0x00054321u, 0xabcdffffu, 0x77770000u, 0x55550000u},
          {O::VMovB32, O::VCvtU16F16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMinMaxMed3F16Ops() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x40003c00u); // low=1.0h, high=2.0h
  AppendVMovLiteral(&code, 1, 0x44004200u); // low=3.0h, high=4.0h
  AppendVMovLiteral(&code, 2, 0xc8004000u); // low=2.0h, high=-8.0h
  AppendVMovLiteral(&code, 3, 0x00007e00u); // low=qNaN
  AppendVMovLiteral(&code, 4, 0x00004000u); // low=2.0h
  AppendVMovLiteral(&code, 5, 0x00003c00u); // low=1.0h
  AppendVMovLiteral(&code, 10, 0xaaaa5555u);
  AppendVMovLiteral(&code, 11, 0x12345678u);
  AppendVMovLiteral(&code, 12, 0x77772222u);

  AppendVop3(&code, 0x351, 10, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x354, 11, Vgpr(0), Vgpr(1), Vgpr(2), 0, 0xf, false, 0,
             0x4);
  AppendVop3(&code, 0x357, 12, Vgpr(3), Vgpr(4), Vgpr(5));
  for (u32 i = 0; i < 3; i++) {
    AppendStoreVgpr(&code, 10 + i, i);
  }
  AppendEnd(&code);

  return {"VectorMinMaxMed3F16Ops",
          code,
          {},
          {0xaaaa3c00u, 0x48005678u, 0x77773c00u},
          {O::VMovB32, O::VMin3F16, O::VMax3F16, O::VMed3F16,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorSpecialF16Ops() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0xc000fc00u); // low=-inf, high=-2.0h
  AppendVMovLiteral(&code, 1, 0x00004400u); // low=+4.0h
  AppendVMovLiteral(&code, 2, 0x00003c00u); // low=+1.0h
  AppendVMovLiteral(&code, 3, 0x0000bc00u); // low=-1.0h
  AppendVMovLiteral(&code, 4, 0x0000fc00u); // low=-inf
  AppendVMovLiteral(&code, 11, 0x12345678u);

  code.push_back(EncodeVop1(0x54, 10, Vgpr(0)));
  code.push_back(EncodeVop1(0x54, 11, 249));
  code.push_back(EncodeVop1Sdwa(0, 5, 2, 5));
  code.push_back(EncodeVop1(0x56, 12, Vgpr(1)));
  code.push_back(EncodeVop1(0x57, 13, Vgpr(2)));
  code.push_back(EncodeVop1(0x57, 14, Vgpr(3)));
  code.push_back(EncodeVop1(0x58, 15, Vgpr(4)));
  for (u32 i = 0; i < 6; i++) {
    AppendStoreVgpr(&code, 10 + i, i);
  }
  AppendEnd(&code);

  return {"VectorSpecialF16Ops",
          code,
          {},
          {0x00008000u, 0xb8005678u, 0x00003800u, 0x00000000u, 0x0000fe00u,
           0x00000000u},
          {O::VMovB32, O::VRcpF16, O::VRsqF16, O::VLogF16, O::VExpF16,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorWritelaneIgnoresExecMask() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 2, 0xaaaaaaaau);
  AppendSMovLiteral(&code, 4, 0x12345678u);
  code.push_back(EncodeSMovB32(126, InlineU32(1)));
  code.push_back(EncodeSMovB32(127, InlineU32(0)));
  AppendVop3(&code, 0x361, 2, 4, InlineU32(1));
  code.push_back(EncodeSMovB32(126, InlineU32(0xf)));
  code.push_back(EncodeSMovB32(127, InlineU32(0)));
  code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorWritelaneIgnoresExecMask";
  test.code = code;
  test.expected = {0xaaaaaaaau, 0x12345678u, 0xaaaaaaaau, 0xaaaaaaaau};
  test.opcodes = {O::VMovB32,     O::SMovB32,          O::VWritelaneB32,
                  O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase VectorPermlanex16() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 3, 0x76543210u);
  AppendVMovLiteral(&code, 4, 0xfedcba98u);
  code.push_back(EncodeVop2(0x1a, 2, InlineU32(2), 0));
  AppendVMovLiteral(&code, 5, 0xfeedbabeu);
  AppendVop3(&code, 0x378, 1, Vgpr(5), Vgpr(3), Vgpr(4));
  AppendBufferStoreDword(&code, 1, 2);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorPermlanex16";
  test.code = code;
  test.expected = std::vector<u32>(32, 0xfeedbabeu);
  test.opcodes = {O::VMovB32, O::VPermlanex16B32, O::VLshlrevB32,
                  O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 32;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase VectorPermlane16FetchInactiveZero() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),           EncodeVop2(0x25, 1, InlineU32(10), 1),
      EncodeSMovB32(126, InlineU32(1)), EncodeSMovB32(127, InlineU32(0)),
      EncodeSMovB32(0, InlineU32(1)),   EncodeSMovB32(1, InlineU32(0)),
  };
  AppendVop3(&code, 0x377, 2, Vgpr(1), 0, 1);
  AppendStoreVgpr(&code, 2, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorPermlane16FetchInactiveZero";
  test.code = code;
  test.expected = {0};
  test.opcodes = {O::VMovB32,        O::VAddNcU32,        O::SMovB32,
                  O::VPermlane16B32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase VectorPermlane16FetchInactiveFi() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, Vgpr(0)),     EncodeVop2(0x25, 1, InlineU32(10), 1),
      EncodeSMovB32(126, InlineU32(1)), EncodeSMovB32(127, InlineU32(0)),
      EncodeSMovB32(0, InlineU32(1)),   EncodeSMovB32(1, InlineU32(0)),
  };
  AppendVop3(&code, 0x377, 2, Vgpr(1), 0, 1, 0, 1);
  AppendStoreVgpr(&code, 2, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorPermlane16FetchInactiveFi";
  test.code = code;
  test.expected = {11};
  test.opcodes = {O::VMovB32,        O::VAddNcU32,        O::SMovB32,
                  O::VPermlane16B32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase VectorDppQuadPermuteReverse() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 100);
  code.push_back(EncodeVop2(0x25, 2, 250, 1));
  code.push_back(EncodeVop2Dpp(0, 0x01b));
  code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorDppQuadPermuteReverse";
  test.code = code;
  test.expected = {103, 102, 101, 100, 107, 106, 105, 104};
  test.opcodes = {O::VMovB32, O::VAddNcU32, O::VLshlrevB32, O::BufferStoreDword,
                  O::SEndpgm};
  test.compute_info.threads_num[0] = 8;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase VectorDppBankMaskPreservesDestination() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 2, 0xaaaaaaaau);
  AppendVMovU32(&code, 1, 100);
  code.push_back(EncodeVop2(0x25, 2, 250, 1));
  code.push_back(EncodeVop2Dpp(0, 0x0e4, 0xf, 0xe));
  code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorDppBankMaskPreservesDestination";
  test.code = code;
  test.expected = {0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau, 0xaaaaaaaau,
                   104,         105,         106,         107};
  test.opcodes = {O::VMovB32, O::VAddNcU32, O::VLshlrevB32, O::BufferStoreDword,
                  O::SEndpgm};
  test.compute_info.threads_num[0] = 8;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase VectorDppBoundsControlZeroPreservesDestination() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 2, 0xaaaaaaaau);
  AppendVMovU32(&code, 1, 100);
  code.push_back(EncodeVop2(0x25, 2, 250, 1));
  code.push_back(EncodeVop2Dpp(0, 0x111));
  code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "VectorDppBoundsControlZeroPreservesDestination";
  test.code = code;
  test.expected = {0xaaaaaaaau, 100, 101, 102, 103, 104, 105, 106};
  test.opcodes = {O::VMovB32, O::VAddNcU32, O::VLshlrevB32, O::BufferStoreDword,
                  O::SEndpgm};
  test.compute_info.threads_num[0] = 8;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase Vop3LdexpSourceModifier() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 9, 0xbfc00000u);
  code.push_back(0xd7620107u);
  code.push_back(0x00018509u);
  AppendStoreVgpr(&code, 7, 0);
  AppendEnd(&code);

  return {"Vop3LdexpSourceModifier",
          code,
          {},
          {0x3ec00000u},
          {O::VMovB32, O::VLdexpF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop1MoveRelSource() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 12, 0x11111111u);
  AppendVMovLiteral(&code, 13, 0x22222222u);
  AppendVMovLiteral(&code, 14, 0x33333333u);
  code.push_back(EncodeSMovB32(124, InlineU32(2)));
  code.push_back(0x7e6e870cu);
  AppendStoreVgpr(&code, 55, 0);
  AppendEnd(&code);

  return {"Vop1MoveRelSource",
          code,
          {},
          {0x33333333u},
          {O::VMovB32, O::SMovB32, O::VMovrelsB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase Vop1MoveRelDestination() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 1, 0x12345678u);
  AppendVMovLiteral(&code, 5, 0xaaaaaaaau);
  AppendVMovLiteral(&code, 6, 0xbbbbbbbbu);
  AppendVMovLiteral(&code, 7, 0xccccccccu);
  code.push_back(EncodeSMovB32(124, InlineU32(2)));
  code.push_back(EncodeSMovB32(126, InlineU32(1)));
  code.push_back(EncodeSMovB32(127, InlineU32(0)));
  code.push_back(EncodeVop1(0x42, 5, Vgpr(1)));
  code.push_back(EncodeSMovB32(126, InlineU32(0xf)));
  code.push_back(EncodeSMovB32(127, InlineU32(0)));
  code.push_back(EncodeVop2(0x1a, 4, InlineU32(2), 0));
  AppendBufferStoreDword(&code, 7, 4);
  AppendEnd(&code);

  TestCase test;
  test.name = "Vop1MoveRelDestination";
  test.code = code;
  test.expected = {0x12345678u, 0xccccccccu, 0xccccccccu, 0xccccccccu};
  test.opcodes = {O::VMovB32,     O::SMovB32,          O::VMovreldB32,
                  O::VLshlrevB32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase VectorFloatSpecialOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0x40000000u);
  AppendVMovLiteral(&code, 2, 0x40400000u);
  AppendVMovLiteral(&code, 3, 0x40800000u);
  AppendVMovLiteral(&code, 4, 0x40003c00u);
  AppendVMovLiteral(&code, 5, 0x44004200u);
  AppendVMovLiteral(&code, 6, 0x3f800000u);
  AppendVMovLiteral(&code, 7, 0x3f800000u);
  AppendVMovLiteral(&code, 8, 0xbf800000u);
  AppendVMovLiteral(&code, 10, 0x3f000000u);
  AppendVMovLiteral(&code, 12, 0xaaaabbbbu);
  AppendVMovLiteral(&code, 13, 0xaaaabbbbu);

  code.push_back(EncodeVop2(0x02, 6, Vgpr(4), 5));
  AppendVop3(&code, 0x368, 9, Vgpr(7), Vgpr(8));
  AppendVop3(&code, 0x369, 11, Vgpr(7), Vgpr(10));
  AppendVop3p(&code, 0x21, 12, Vgpr(1), Vgpr(2), Vgpr(0));
  AppendVop3p(&code, 0x22, 13, Vgpr(1), Vgpr(2), Vgpr(0));
  AppendVop3(&code, 0x144, 14, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x145, 15, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x146, 16, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x147, 17, Vgpr(0), Vgpr(1), Vgpr(2));

  const u32 results[] = {6, 9, 11, 12, 13, 14, 15, 16, 17};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"VectorFloatSpecialOps",
          code,
          {},
          {0x41400000u, 0x80017fffu, 0x8000ffffu, 0xaaaa4700u, 0x4700bbbbu,
           0x40800000u, 0x3f800000u, 0xc0000000u, 0x40c00000u},
          {O::VMovB32, O::VDot2cF32F16, O::VCvtPknormI16F32,
           O::VCvtPknormU16F32, O::VMadMixloF16, O::VMadMixhiF16, O::VCubeidF32,
           O::VCubescF32, O::VCubetcF32, O::VCubemaF32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase MadMixF16LiteralHalfSourceUsesOpsel() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 12, 0xaaaabbbbu);
  AppendVMovLiteral(&code, 13, 0x11112222u);

  AppendVop3p(&code, 0x21, 12, 255u, Vgpr(0), Vgpr(0), 0x1, 0x1);
  code.push_back(0x40003c00u);
  AppendVop3p(&code, 0x22, 13, 255u, Vgpr(0), Vgpr(0), 0x1, 0x0);
  code.push_back(0x40003c00u);

  AppendStoreVgpr(&code, 12, 0);
  AppendStoreVgpr(&code, 13, 1);
  AppendEnd(&code);

  return {"MadMixF16LiteralHalfSourceUsesOpsel",
          code,
          {},
          {0xaaaa4200u, 0x40002222u},
          {O::VMovB32, O::VMadMixloF16, O::VMadMixhiF16, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase MadMixF16NegHiIsAbsAndNegIsIndependent() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0xc0000000u);
  AppendVMovLiteral(&code, 2, 0x40000000u);
  AppendVMovLiteral(&code, 12, 0xaaaabbbbu);
  AppendVMovLiteral(&code, 13, 0x11112222u);

  AppendVop3p(&code, 0x21, 12, Vgpr(1), Vgpr(0), Vgpr(0), 0, 0, 0x1);
  AppendVop3p(&code, 0x22, 13, Vgpr(2), Vgpr(0), Vgpr(0), 0, 0, 0, 0x1);

  AppendStoreVgpr(&code, 12, 0);
  AppendStoreVgpr(&code, 13, 1);
  AppendEnd(&code);

  return {"MadMixF16NegHiIsAbsAndNegIsIndependent",
          code,
          {},
          {0xaaaa4200u, 0xbc002222u},
          {O::VMovB32, O::VMadMixloF16, O::VMadMixhiF16, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase VectorVop3FmaF16UsesRdna2Opcode34b() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 1, 0x40003c00u);
  AppendVMovLiteral(&code, 2, 0x44004200u);
  AppendVMovLiteral(&code, 3, 0x3c003c00u);
  AppendVMovLiteral(&code, 10, 0xaaaa5555u);
  AppendVMovLiteral(&code, 11, 0xbbbb5555u);

  AppendVop3(&code, 0x34bu, 10, Vgpr(1), Vgpr(2), Vgpr(3));
  AppendVop3(&code, 0x34bu, 11, Vgpr(1), Vgpr(2), Vgpr(3), 0, 0xfu);

  AppendStoreVgpr(&code, 10, 0);
  AppendStoreVgpr(&code, 11, 1);
  AppendEnd(&code);

  return {"VectorVop3FmaF16UsesRdna2Opcode34b",
          code,
          {},
          {0xaaaa4400u, 0x48805555u},
          {O::VMovB32, O::VFmaF16, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorFloatArithmeticOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0x40000000u);
  AppendVMovLiteral(&code, 2, 0x40800000u);
  AppendVMovLiteral(&code, 3, 0x40a00000u);
  AppendVMovLiteral(&code, 4, 0x40400000u);
  AppendVMovLiteral(&code, 11, 0x40800000u);
  AppendVMovLiteral(&code, 17, 0x40800000u);

  code.push_back(EncodeVop2(0x03, 5, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x04, 6, Vgpr(3), 1));
  code.push_back(EncodeVop2(0x05, 7, Vgpr(1), 3));
  code.push_back(EncodeVop2(0x08, 8, Vgpr(1), 2));
  code.push_back(EncodeVop1(0x01, 21, Vgpr(8)));
  code.push_back(EncodeVop2(0x0f, 9, Vgpr(3), 1));
  code.push_back(EncodeVop2(0x10, 10, Vgpr(3), 1));
  code.push_back(EncodeVop2(0x1f, 11, Vgpr(1), 4));
  AppendVop3(&code, 0x141, 12, Vgpr(1), Vgpr(4), Vgpr(2));
  AppendVop3(&code, 0x14b, 13, Vgpr(1), Vgpr(4), Vgpr(2));
  AppendVop3(&code, 0x151, 14, Vgpr(3), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x154, 15, Vgpr(3), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x157, 16, Vgpr(3), Vgpr(1), Vgpr(2));
  code.push_back(EncodeVop2(0x20, 18, Vgpr(1), 17));
  code.push_back(0x3f800000u);
  code.push_back(EncodeVop2(0x21, 19, Vgpr(1), 4));
  code.push_back(0x40000000u);
  code.push_back(EncodeVop2(0x03, 20, 249, 1));
  code.push_back(EncodeVop2Sdwa(0));
  AppendSMovLiteral(&code, 70, 0x40000000u);
  code.push_back(0x06088cf9u);
  code.push_back(0x868606f2u);
  AppendSMovLiteral(&code, 26, 0x40000000u);
  code.push_back(0x081034f9u);
  code.push_back(0x868606f2u);
  code.push_back(0xd51f8011u);
  code.push_back(0x00020d0bu);
  AppendVMovLiteral(&code, 22, 0xbf800000u);
  code.push_back(EncodeVop2(0x05, 23, 249, 26));
  code.push_back(EncodeVop2Sdwa(22, 6, 0, 6, 6, 0, 0, 0, 1, 0, 0, 0, 1));
  AppendVMovLiteral(&code, 24, 0x3f800000u);
  AppendVMovLiteral(&code, 25, 0x3f800000u);
  code.push_back(EncodeVop2(0x03, 26, 249, 25));
  code.push_back(EncodeVop2Sdwa(24, 6, 0, 6, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1));
  AppendVMovLiteral(&code, 27, 0xbf800000u);
  AppendVMovLiteral(&code, 28, 0x40000000u);
  AppendVop3(&code, 0x104, 29, Vgpr(27), Vgpr(28), 0, 1);
  AppendVop3(&code, 0x103, 30, Vgpr(0), Vgpr(1), 0, 0, 0, false, 1);
  AppendVop3(&code, 0x151, 32, Vgpr(0), Vgpr(1), Vgpr(2), 0, 0, false, 1);
  AppendVMovLiteral(&code, 33, 0xbf800000u);
  AppendVMovLiteral(&code, 34, 0x40000000u);
  code.push_back(EncodeVop2(0x03, 35, 250, 34));
  code.push_back(EncodeVop2Dpp(33, 0, 0xf, 0xf, 0, 1));

  const u32 results[] = {5,  6,  7,  21, 9, 10, 11, 12, 13, 14, 15, 16,
                         18, 19, 20, 4,  8, 17, 23, 26, 29, 30, 32, 35};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"VectorFloatArithmeticOps",
          code,
          {},
          {0x40400000u, 0x40400000u, 0x40400000u, 0x41000000u, 0x40000000u,
           0x40a00000u, 0x41200000u, 0x41200000u, 0x41200000u, 0x40000000u,
           0x40a00000u, 0x40800000u, 0x40c00000u, 0x41000000u, 0x40400000u,
           0x40400000u, 0xbf800000u, 0x3f800000u, 0x3f800000u, 0x40800000u,
           0xbf800000u, 0x40c00000u, 0x40000000u, 0x40400000u},
          {O::SMovB32, O::VMovB32, O::VAddF32, O::VSubF32, O::VSubrevF32,
           O::VMulF32, O::VMinF32, O::VMaxF32, O::VMacF32, O::VMadF32,
           O::VFmaF32, O::VMin3F32, O::VMax3F32, O::VMed3F32, O::VMadmkF32,
           O::VMadakF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMinMaxF32NanAndSignedZeroEdges() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x40000000u);
  AppendVMovLiteral(&code, 1, 0x7fc00000u);
  AppendVMovLiteral(&code, 2, 0x00000000u);
  AppendVMovLiteral(&code, 3, 0x80000000u);
  AppendVMovLiteral(&code, 4, 0x40400000u);
  AppendVMovLiteral(&code, 5, 0x3f800000u);
  AppendVMovLiteral(&code, 6, 0x7fa00001u);

  code.push_back(EncodeVop2(0x0f, 10, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x10, 11, Vgpr(0), 1));
  code.push_back(EncodeVop2(0x0f, 12, Vgpr(3), 2));
  code.push_back(EncodeVop2(0x10, 13, Vgpr(2), 3));
  code.push_back(EncodeVop2(0x0f, 14, Vgpr(6), 0));
  code.push_back(EncodeVop2(0x10, 15, Vgpr(0), 6));
  AppendVop3(&code, 0x151, 16, Vgpr(0), Vgpr(1), Vgpr(4));
  AppendVop3(&code, 0x154, 17, Vgpr(0), Vgpr(1), Vgpr(5));

  for (u32 i = 0; i < 8; i++) {
    AppendStoreVgpr(&code, 10 + i, i);
  }
  AppendEnd(&code);

  return {"VectorMinMaxF32NanAndSignedZeroEdges",
          code,
          {},
          {0x40000000u, 0x40000000u, 0x80000000u, 0x00000000u, 0x7fe00001u,
           0x7fe00001u, 0x40000000u, 0x40000000u},
          {O::VMovB32, O::VMinF32, O::VMaxF32, O::VMin3F32, O::VMax3F32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorMed3F32NanUsesMin3Path() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x40000000u);
  AppendVMovLiteral(&code, 1, 0x40400000u);
  AppendVMovLiteral(&code, 2, 0x7fa00001u);
  AppendVMovLiteral(&code, 3, 0x7fc00000u);

  AppendVop3(&code, 0x157, 10, Vgpr(0), Vgpr(1), Vgpr(2));
  AppendVop3(&code, 0x157, 11, Vgpr(0), Vgpr(1), Vgpr(3));
  AppendVop3(&code, 0x157, 12, Vgpr(2), Vgpr(0), Vgpr(1));

  AppendStoreVgpr(&code, 10, 0);
  AppendStoreVgpr(&code, 11, 1);
  AppendStoreVgpr(&code, 12, 2);
  AppendEnd(&code);

  return {"VectorMed3F32NanUsesMin3Path",
          code,
          {},
          {0x7fe00001u, 0x40000000u, 0x40400000u},
          {O::VMovB32, O::VMed3F32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorFloatConversionOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0xfffffffdu);
  code.push_back(EncodeVop1(0x01, 1, InlineU32(7)));
  AppendVMovLiteral(&code, 2, 0x40e00000u);
  AppendVMovLiteral(&code, 3, 0xc0400000u);
  AppendVMovLiteral(&code, 4, 0x3f800000u);
  AppendVMovLiteral(&code, 5, 0x00003c00u);
  AppendVMovLiteral(&code, 6, 0x40700000u);
  AppendVMovLiteral(&code, 7, 0x44332211u);
  AppendVMovLiteral(&code, 8, 0x40000000u);
  AppendVMovLiteral(&code, 9, 0x40800000u);
  AppendVMovLiteral(&code, 10, 0x40100000u);
  AppendVMovLiteral(&code, 11, 0x40300000u);
  code.push_back(EncodeVop1(0x01, 12, InlineU32(0)));
  code.push_back(EncodeVop1(0x01, 37, InlineU32(15)));

  code.push_back(EncodeVop1(0x05, 13, Vgpr(0)));
  code.push_back(EncodeVop1(0x06, 14, Vgpr(1)));
  code.push_back(EncodeVop1(0x07, 15, Vgpr(2)));
  code.push_back(EncodeVop1(0x08, 16, Vgpr(3)));
  code.push_back(EncodeVop1(0x0a, 17, Vgpr(4)));
  code.push_back(EncodeVop1(0x0b, 18, Vgpr(5)));
  code.push_back(EncodeVop1(0x0c, 42, Vgpr(11)));
  code.push_back(EncodeVop1(0x0d, 19, Vgpr(6)));
  code.push_back(EncodeVop1(0x11, 20, Vgpr(7)));
  code.push_back(EncodeVop1(0x12, 21, Vgpr(7)));
  code.push_back(EncodeVop1(0x13, 22, Vgpr(7)));
  code.push_back(EncodeVop1(0x14, 23, Vgpr(7)));
  code.push_back(EncodeVop1(0x2a, 24, Vgpr(8)));
  code.push_back(EncodeVop1(0x20, 25, Vgpr(10)));
  code.push_back(EncodeVop1(0x21, 26, Vgpr(11)));
  code.push_back(EncodeVop1(0x22, 27, Vgpr(10)));
  code.push_back(EncodeVop1(0x23, 28, Vgpr(10)));
  code.push_back(EncodeVop1(0x24, 29, Vgpr(11)));
  code.push_back(EncodeVop1(0x25, 30, Vgpr(4)));
  code.push_back(EncodeVop1(0x27, 36, Vgpr(9)));
  code.push_back(EncodeVop1(0x2e, 32, Vgpr(9)));
  code.push_back(EncodeVop1(0x33, 33, Vgpr(9)));
  code.push_back(EncodeVop1(0x35, 34, Vgpr(12)));
  code.push_back(EncodeVop1(0x36, 35, Vgpr(12)));
  code.push_back(EncodeVop1(0x0e, 38, Vgpr(37)));
  code.push_back(EncodeVop1(0x2a, 39, 249));
  code.push_back(EncodeVop1Sdwa(8, 6, 0, 6, 0, 0, 0, 0, 0, 1));
  AppendVMovLiteral(&code, 40, 0xc0000000u);
  AppendVop3(&code, 0x1aa, 41, Vgpr(40), 0, 0, 1, 0, false, 1);
  AppendVMovLiteral(&code, 2, 0xc0003c00u); // low=1.0h, high=-2.0h
  code.push_back(0x7e1016f9u);
  code.push_back(0x00250602u);

  const u32 results[] = {13, 14, 15, 16, 17, 18, 42, 19, 20, 21, 22, 23, 24, 25,
                         26, 27, 28, 29, 30, 36, 32, 33, 34, 35, 38, 39, 41, 8};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"VectorFloatConversionOps",
          code,
          {},
          {0xc0400000u, 0x40e00000u, 7,           0xfffffffdu, 0x00003c00u,
           0x3f800000u, 3,           3,           0x41880000u, 0x42080000u,
           0x424c0000u, 0x42880000u, 0x3f000000u, 0x3e800000u, 0x40000000u,
           0x40400000u, 0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u,
           0x3f000000u, 0x40000000u, 0,           0x3f800000u, 0xbd800000u,
           0x3f800000u, 0x3f800000u, 0x40000000u},
          {O::VMovB32,       O::VCvtF32I32,    O::VCvtF32U32,
           O::VCvtU32F32,    O::VCvtI32F32,    O::VCvtF16F32,
           O::VCvtF32F16,    O::VCvtRpiI32F32, O::VCvtFlrI32F32,
           O::VCvtOffF32I4,  O::VCvtF32Ubyte0, O::VCvtF32Ubyte1,
           O::VCvtF32Ubyte2, O::VCvtF32Ubyte3, O::VRcpF32,
           O::VFractF32,     O::VTruncF32,     O::VCeilF32,
           O::VRndneF32,     O::VFloorF32,     O::VExpF32,
           O::VLogF32,       O::VRsqF32,       O::VSqrtF32,
           O::VSinF32,       O::VCosF32,       O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase CvtF32ToIntSaturatesNaNAndOutOfRange() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x7fc00000u);
  AppendVMovLiteral(&code, 1, 0xbf800000u);
  AppendVMovLiteral(&code, 2, 0x7f800000u);
  AppendVMovLiteral(&code, 3, 0x4f800000u);
  AppendVMovLiteral(&code, 4, 0xff800000u);
  AppendVMovLiteral(&code, 5, 0x4f000000u);

  code.push_back(EncodeVop1(0x07, 10, Vgpr(0)));
  code.push_back(EncodeVop1(0x07, 11, Vgpr(1)));
  code.push_back(EncodeVop1(0x07, 12, Vgpr(2)));
  code.push_back(EncodeVop1(0x07, 13, Vgpr(3)));
  code.push_back(EncodeVop1(0x08, 14, Vgpr(0)));
  code.push_back(EncodeVop1(0x08, 15, Vgpr(2)));
  code.push_back(EncodeVop1(0x08, 16, Vgpr(4)));
  code.push_back(EncodeVop1(0x08, 17, Vgpr(5)));

  for (u32 i = 0; i < 8; i++) {
    AppendStoreVgpr(&code, 10 + i, i);
  }
  AppendEnd(&code);

  return {"CvtF32ToIntSaturatesNaNAndOutOfRange",
          code,
          {},
          {0, 0, 0xffffffffu, 0xffffffffu, 0, 0x7fffffffu, 0x80000000u,
           0x7fffffffu},
          {O::VMovB32, O::VCvtU32F32, O::VCvtI32F32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase VectorSpecialF32FlushesDenormalInputs() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x00000001u);
  code.push_back(EncodeVop1(0x27, 1, Vgpr(0)));
  code.push_back(EncodeVop1(0x2a, 2, Vgpr(0)));
  code.push_back(EncodeVop1(0x2e, 3, Vgpr(0)));
  code.push_back(EncodeVop1(0x33, 4, Vgpr(0)));
  AppendStoreVgpr(&code, 1, 0);
  AppendStoreVgpr(&code, 2, 1);
  AppendStoreVgpr(&code, 3, 2);
  AppendStoreVgpr(&code, 4, 3);
  AppendEnd(&code);

  return {"VectorSpecialF32FlushesDenormalInputs",
          code,
          {},
          {0xff800000u, 0x7f800000u, 0x7f800000u, 0x00000000u},
          {O::VMovB32, O::VLogF32, O::VRcpF32, O::VRsqF32, O::VSqrtF32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorSinCosMaxFiniteSpecialCases() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0xff7fffffu);
  code.push_back(EncodeVop1(0x35, 1, Vgpr(0)));
  code.push_back(EncodeVop1(0x36, 2, Vgpr(0)));
  AppendStoreVgpr(&code, 1, 0);
  AppendStoreVgpr(&code, 2, 1);
  AppendEnd(&code);

  return {
      "VectorSinCosMaxFiniteSpecialCases",
      code,
      {},
      {0x00000000u, 0x3f800000u},
      {O::VMovB32, O::VSinF32, O::VCosF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  std::vector<u32> results;
  std::vector<u32> expected;
  u32 dst = 10;

  code.push_back(EncodeVop1(0x01, 1, InlineU32(1)));
  AppendVMovLiteral(&code, 2, 0x40000000u);
  AppendVMovLiteral(&code, 3, 0x40400000u);
  code.push_back(EncodeVop1(0x01, 4, InlineU32(1)));
  AppendVMovLiteral(&code, 5, 0xffffffffu);

  auto append_compare = [&](u32 opcode, u32 src0, u32 src1, bool value) {
    if (dst == 31u) {
      dst++;
    }
    code.push_back(EncodeVopc(opcode, src0, src1));
    code.push_back(EncodeVop2(0x01, dst, InlineU32(0), 1));
    results.push_back(dst);
    expected.push_back(value ? 1u : 0u);
    dst++;
  };

  append_compare(0x00, Vgpr(2), 3, false);
  append_compare(0x01, Vgpr(2), 3, true);
  append_compare(0x02, Vgpr(2), 3, false);
  append_compare(0x03, Vgpr(2), 3, true);
  append_compare(0x04, Vgpr(2), 3, false);
  append_compare(0x05, Vgpr(2), 3, true);
  append_compare(0x06, Vgpr(2), 3, false);
  append_compare(0x07, Vgpr(2), 3, true);
  append_compare(0x08, Vgpr(2), 3, false);
  append_compare(0x09, Vgpr(2), 3, true);
  append_compare(0x0a, Vgpr(2), 3, false);
  append_compare(0x0b, Vgpr(2), 3, true);
  append_compare(0x0c, Vgpr(2), 3, false);
  append_compare(0x0d, Vgpr(2), 3, true);
  append_compare(0x0e, Vgpr(2), 3, false);
  append_compare(0x0f, Vgpr(2), 3, true);

  append_compare(0x80, Vgpr(5), 4, false);
  append_compare(0x81, Vgpr(5), 4, true);
  append_compare(0x82, Vgpr(5), 4, false);
  append_compare(0x83, Vgpr(5), 4, true);
  append_compare(0x84, Vgpr(5), 4, false);
  append_compare(0x85, Vgpr(5), 4, true);
  append_compare(0x86, Vgpr(5), 4, false);
  append_compare(0x87, Vgpr(5), 4, true);

  append_compare(0xc0, Vgpr(4), 5, false);
  append_compare(0xc1, Vgpr(4), 5, true);
  append_compare(0xc2, Vgpr(4), 5, false);
  append_compare(0xc3, Vgpr(4), 5, true);
  append_compare(0xc4, Vgpr(4), 5, false);
  append_compare(0xc5, Vgpr(4), 5, true);
  append_compare(0xc6, Vgpr(4), 5, false);
  append_compare(0xc7, Vgpr(4), 5, true);

  code.push_back(0x7c1d00f9u);
  code.push_back(0x86069201u);
  code.push_back(EncodeVopc(0x84, 249, 4));
  code.push_back(EncodeVopcSdwa(5, 19, 1));
  code.push_back(EncodeVopc(0x01, 250, 3));
  code.push_back(EncodeVop2Dpp(2));
  AppendVMovLiteral(&code, 6, 0xc0000000u);
  AppendVMovLiteral(&code, 7, 0x3f800000u);
  AppendVop3(&code, 0x04, 20, Vgpr(6), Vgpr(7), 0, 1);
  for (u32 i = 0; i < static_cast<u32>(results.size()); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendStoreSgpr(&code, 18, static_cast<u32>(results.size()));
  expected.push_back(1u);
  AppendStoreSgpr(&code, 19, static_cast<u32>(results.size() + 1u));
  expected.push_back(0u);
  AppendStoreSgpr(&code, 106, static_cast<u32>(results.size() + 2u));
  expected.push_back(1u);
  AppendStoreSgpr(&code, 20, static_cast<u32>(results.size() + 3u));
  expected.push_back(1u);
  AppendEnd(&code);

  return {"VectorCompareOps",
          code,
          {},
          expected,
          {O::VMovB32,    O::VCndmaskB32, O::VCmpFF32,         O::VCmpLtF32,
           O::VCmpEqF32,  O::VCmpLeF32,   O::VCmpGtF32,        O::VCmpLgF32,
           O::VCmpGeF32,  O::VCmpOF32,    O::VCmpUF32,         O::VCmpNgeF32,
           O::VCmpNlgF32, O::VCmpNgtF32,  O::VCmpNleF32,       O::VCmpNeqF32,
           O::VCmpNltF32, O::VCmpTruF32,  O::VCmpFI32,         O::VCmpLtI32,
           O::VCmpEqI32,  O::VCmpLeI32,   O::VCmpGtI32,        O::VCmpNeI32,
           O::VCmpGeI32,  O::VCmpTI32,    O::VCmpFU32,         O::VCmpLtU32,
           O::VCmpEqU32,  O::VCmpLeU32,   O::VCmpGtU32,        O::VCmpNeU32,
           O::VCmpGeU32,  O::VCmpTU32,    O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3CompareNeU64OnGpu() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeVop1(0x01, 1, InlineU32(1)));

  code.push_back(EncodeSop1(0x04, 106, 126)); // s_mov_b64 vcc, exec
  code.push_back(0xd4e5006au);
  code.push_back(0x0000d47eu); // v_cmp_ne_u64 vcc, exec, vcc
  code.push_back(EncodeVop2(0x01, 2, InlineU32(0), 1));
  AppendStoreVgpr(&code, 2, 0);

  AppendSMovLiteral(&code, 106, 0);
  AppendSMovLiteral(&code, 107, 0);
  code.push_back(0xd4e5006au);
  code.push_back(0x0000d47eu); // v_cmp_ne_u64 vcc, exec, vcc
  code.push_back(EncodeVop2(0x01, 3, InlineU32(0), 1));
  AppendStoreVgpr(&code, 3, 1);
  AppendEnd(&code);

  return {"VectorVop3CompareNeU64OnGpu",
          code,
          {},
          {0, 1},
          {O::VMovB32, O::SMovB64, O::VCmpNeU64, O::VCndmaskB32, O::SMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareClassF32() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  std::vector<u32> expected;
  AppendVMovU32(&code, 1, 1);

  u32 out = 0;
  auto append_class = [&](u32 bits, u32 class_mask, bool match) {
    AppendVMovLiteral(&code, 0, bits);
    AppendVMovU32(&code, 2, class_mask);
    code.push_back(EncodeVopc(0x88, Vgpr(0), 2));
    code.push_back(EncodeVop2(0x01, 3, InlineU32(0), 1));
    AppendStoreVgpr(&code, 3, out++);
    expected.push_back(match ? 1u : 0u);
  };

  append_class(0x7fc00000u, 1u << 1u, true);
  append_class(0xff800000u, 1u << 2u, true);
  append_class(0xbf800000u, 1u << 3u, true);
  append_class(0x80000000u, 1u << 5u, true);
  append_class(0x00000000u, 1u << 6u, true);
  append_class(0x40000000u, 1u << 8u, true);
  append_class(0x7f800000u, 1u << 9u, true);
  append_class(0x40000000u, 1u << 3u, false);

  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovU32(&code, 2, 1u << 3u);
  AppendVop3(&code, 0x88, 20, Vgpr(0), Vgpr(2), 0, 0, 0, false, 0, 1);
  AppendStoreSgprPair(&code, 20, out);
  expected.push_back(1u);
  expected.push_back(0u);
  AppendEnd(&code);

  return {"VectorCompareClassF32",
          code,
          {},
          expected,
          {O::VMovB32, O::VCmpClassF32, O::VCndmaskB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase VectorCompareF16Ops() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  std::vector<u32> expected;
  AppendVMovU32(&code, 1, 1);
  AppendVMovLiteral(&code, 2, 0x40003c00u); // low=1.0h, high=2.0h
  AppendVMovLiteral(&code, 3, 0x44004200u); // low=3.0h, high=4.0h
  AppendVMovLiteral(&code, 4, 0x00007e00u); // low=qNaN

  u32 dst = 10;
  u32 out = 0;
  auto append_compare = [&](u32 opcode, u32 src0, u32 src1, bool value) {
    code.push_back(EncodeVopc(opcode, src0, src1));
    code.push_back(EncodeVop2(0x01, dst, InlineU32(0), 1));
    AppendStoreVgpr(&code, dst, out++);
    expected.push_back(value ? 1u : 0u);
    dst++;
  };

  append_compare(0xc9, Vgpr(2), 3, true);
  append_compare(0xca, Vgpr(2), 2, true);
  append_compare(0xcb, Vgpr(2), 3, true);
  append_compare(0xcc, Vgpr(3), 2, true);
  append_compare(0xcd, Vgpr(2), 3, true);
  append_compare(0xce, Vgpr(3), 2, true);
  append_compare(0xed, Vgpr(4), 2, true);

  auto append_cmpx = [&](u32 opcode, u32 src0, u32 src1) {
    code.push_back(EncodeSMovB32(126, InlineU32(1)));
    code.push_back(EncodeVopc(opcode, src0, src1));
    AppendStoreVgpr(&code, 1, out++);
    expected.push_back(1u);
  };

  append_cmpx(0xd9, Vgpr(2), 3);
  append_cmpx(0xda, Vgpr(2), 2);
  append_cmpx(0xdb, Vgpr(2), 3);
  append_cmpx(0xdc, Vgpr(3), 2);
  append_cmpx(0xde, Vgpr(3), 2);
  append_cmpx(0xfd, Vgpr(4), 2);
  append_cmpx(0xfe, Vgpr(2), 2);
  AppendEnd(&code);

  return {"VectorCompareF16Ops",
          code,
          {},
          expected,
          {O::VMovB32, O::SMovB32, O::VCmpLtF16, O::VCmpEqF16, O::VCmpLeF16,
           O::VCmpGtF16, O::VCmpLgF16, O::VCmpGeF16, O::VCmpNeqF16,
           O::VCmpxLtF16, O::VCmpxEqF16, O::VCmpxLeF16, O::VCmpxGtF16,
           O::VCmpxGeF16, O::VCmpxNeqF16, O::VCmpxNltF16, O::VCndmaskB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase Vop2SdwaCndmaskSourceModifier() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 6, 0x00000000u);
  AppendVMovLiteral(&code, 47, 0x12345678u);
  AppendVMovLiteral(&code, 53, 0x3f800000u);
  code.push_back(EncodeVopc(0xc7, Vgpr(6), 6)); // v_cmp_t_u32
  code.push_back(0x025e6af9u);
  code.push_back(0x16060635u);
  AppendStoreVgpr(&code, 47, 0);
  AppendEnd(&code);

  return {"Vop2SdwaCndmaskSourceModifier",
          code,
          {},
          {0xbf800000u},
          {O::VMovB32, O::VCmpTU32, O::VCndmaskB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase Vop3CndmaskUsesSgprMaskLaneBits() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),
      EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1),
      EncodeVopc(0xc0, Vgpr(0), 0),
  };
  AppendSMovLiteral(&code, 4, 1);
  AppendSMovLiteral(&code, 5, 0);
  AppendVop3(&code, 0x101, 2, InlineU32(10), InlineU32(20), 4);
  code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 1));
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "Vop3CndmaskUsesSgprMaskLaneBits";
  test.code = code;
  test.expected = {20, 10, 10, 10, 20, 10, 10, 10};
  test.opcodes = {O::VMovB32, O::VLshlrevB32, O::VAddNcU32,        O::VCmpFU32,
                  O::SMovB32, O::VCndmaskB32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  test.dispatch_x = 2;
  return test;
}

TestCase Vop3CndmaskAllowsDataSourceModifier() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 7, 0x3f800000u);
  AppendSMovLiteral(&code, 34, 1);
  AppendSMovLiteral(&code, 35, 0);
  code.push_back(0xd5010004u);
  code.push_back(0x408a0f07u);
  AppendStoreVgpr(&code, 4, 0);
  AppendEnd(&code);

  return {"Vop3CndmaskAllowsDataSourceModifier",
          code,
          {},
          {0xbf800000u},
          {O::VMovB32, O::SMovB32, O::VCndmaskB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase VectorCompareExecOps() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeVop1(0x01, 1, InlineU32(1)));
  AppendVMovLiteral(&code, 2, 0x40000000u);
  AppendVMovLiteral(&code, 3, 0x40400000u);
  code.push_back(EncodeVop1(0x01, 4, InlineU32(1)));
  AppendVMovLiteral(&code, 5, 0xffffffffu);

  u32 out = 0;
  auto append_cmpx = [&](u32 opcode, u32 src0, u32 src1) {
    code.push_back(EncodeSMovB32(126, InlineU32(1)));
    code.push_back(EncodeVopc(opcode, src0, src1));
    AppendStoreVgpr(&code, 1, out++);
  };

  append_cmpx(0x11, Vgpr(2), 3);
  append_cmpx(0x12, Vgpr(3), 3);
  append_cmpx(0x13, Vgpr(2), 3);
  append_cmpx(0x14, Vgpr(3), 2);
  append_cmpx(0x15, Vgpr(2), 3);
  append_cmpx(0x16, Vgpr(3), 3);
  append_cmpx(0x19, Vgpr(2), 3);
  append_cmpx(0x1a, Vgpr(3), 3);
  append_cmpx(0x1b, Vgpr(2), 3);
  append_cmpx(0x1c, Vgpr(3), 2);
  append_cmpx(0x1d, Vgpr(2), 3);
  append_cmpx(0x1e, Vgpr(3), 3);
  append_cmpx(0x91, Vgpr(5), 4);
  append_cmpx(0x92, Vgpr(4), 4);
  append_cmpx(0x93, Vgpr(5), 4);
  append_cmpx(0x94, Vgpr(4), 5);
  append_cmpx(0x95, Vgpr(5), 4);
  append_cmpx(0x96, Vgpr(4), 4);
  append_cmpx(0xd1, Vgpr(4), 5);
  append_cmpx(0xd2, Vgpr(4), 4);
  append_cmpx(0xd3, Vgpr(4), 5);
  append_cmpx(0xd4, Vgpr(5), 4);
  append_cmpx(0xd5, Vgpr(4), 5);
  append_cmpx(0xd6, Vgpr(4), 4);
  AppendEnd(&code);

  return {"VectorCompareExecOps",
          code,
          {},
          std::vector<u32>(out, 1),
          {O::SMovB32,     O::VMovB32,     O::VCmpxLtF32,       O::VCmpxEqF32,
           O::VCmpxLeF32,  O::VCmpxGtF32,  O::VCmpxLgF32,       O::VCmpxGeF32,
           O::VCmpxNgeF32, O::VCmpxNlgF32, O::VCmpxNgtF32,      O::VCmpxNleF32,
           O::VCmpxNeqF32, O::VCmpxNltF32, O::VCmpxLtI32,       O::VCmpxEqI32,
           O::VCmpxLeI32,  O::VCmpxGtI32,  O::VCmpxNeI32,       O::VCmpxGeI32,
           O::VCmpxLtU32,  O::VCmpxEqU32,  O::VCmpxLeU32,       O::VCmpxGtU32,
           O::VCmpxNeU32,  O::VCmpxGeU32,  O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3FloatCompareNegSourceModifier() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovU32(&code, 1, 0);
  AppendVop3(&code, 0x01, 20, Vgpr(0), Vgpr(1), 0, 0, 0, false, 0,
             0x1); // -1.0 < 0.0
  AppendStoreSgprPair(&code, 20, 0);
  AppendEnd(&code);

  return {"VectorVop3FloatCompareNegSourceModifier",
          code,
          {},
          {1, 0},
          {O::VMovB32, O::VCmpLtF32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVop3CmpxWritesExecMask() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 0, 2);
  AppendVMovU32(&code, 1, 1);
  AppendVMovU32(&code, 2, 0);
  AppendVMovU32(&code, 30, 0);
  AppendVop3(&code, 0xd1, 5, Vgpr(0), Vgpr(1)); // v_cmpx_lt_u32, false
  AppendVMovU32(&code, 2, 7);
  AppendBufferStoreDword(&code, 2, 30);
  AppendEnd(&code);

  return {"VectorVop3CmpxWritesExecMask",
          code,
          {0},
          {0},
          {O::VMovB32, O::VCmpxLtU32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorVopcSdwaCmpxWritesExecMask() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 0, 2);
  AppendVMovU32(&code, 1, 1);
  AppendVMovU32(&code, 2, 0);
  AppendVMovU32(&code, 30, 0);
  code.push_back(EncodeVopc(0xd1, 249, 1)); // v_cmpx_lt_u32.sdwa, false
  code.push_back(EncodeVopcSdwa(0));
  AppendVMovU32(&code, 2, 7);
  AppendBufferStoreDword(&code, 2, 30);
  AppendEnd(&code);

  return {"VectorVopcSdwaCmpxWritesExecMask",
          code,
          {0},
          {0},
          {O::VMovB32, O::VCmpxLtU32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase VectorCompareInvertedMaskSelect() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x12345678u);
  code.push_back(EncodeVopc(0xc7, Vgpr(0), 0));      // v_cmp_t_u32 v0, v0
  code.push_back(EncodeSop1(0x08, 106, 106));        // s_not_b64 vcc, vcc
  code.push_back(EncodeVop2(0x01, 1, Vgpr(0), 128)); // v_cndmask_b32 v1, v0, 0
  AppendBufferStoreDword(&code, 1, 30);
  AppendEnd(&code);

  return {"VectorCompareInvertedMaskSelect",
          code,
          {},
          {0x12345678u},
          {O::VMovB32, O::VCmpTU32, O::SNotB64, O::VCndmaskB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase BranchSelect() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeSMovB32(0, InlineU32(0)),
      EncodeSopc(0x06, 0, InlineU32(0)),
      EncodeSopp(0x05, 2),
      EncodeSMovB32(1, InlineU32(1)),
      EncodeSopp(0x02, 1),
      EncodeSMovB32(1, InlineU32(7)),
      EncodeSop2(0x0a, 2, 1, InlineU32(2)),
      EncodeVop1(0x01, 0, 2),
  };
  AppendBufferStoreDword(&code, 0, 30);
  AppendEnd(&code);
  return {"BranchSelect",
          code,
          {},
          {7},
          {O::SMovB32, O::SCmpEqU32, O::SCbranchScc1, O::SBranch,
           O::SCselectB32, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase SimpleLoop() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeSMovB32(0, InlineU32(0)),
      EncodeSopc(0x0a, 0, InlineU32(4)),
      EncodeSopp(0x04, 2),
      EncodeSop2(0x00, 0, 0, InlineU32(1)),
      EncodeSopp(0x02, 0xfffcu),
      EncodeVop1(0x01, 0, 0),
  };
  AppendBufferStoreDword(&code, 0, 30);
  AppendEnd(&code);
  return {"SimpleLoop",
          code,
          {},
          {4},
          {O::SMovB32, O::SCmpLtU32, O::SCbranchScc0, O::SAddU32, O::SBranch,
           O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BranchVccnzUsesWholeMask() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),
      EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1),
      EncodeVop2(0x1a, 3, InlineU32(2), 1),
      EncodeVopc(0xc2, InlineU32(0), 0),
      EncodeSopp(0x07, 2),
      EncodeVop1(0x01, 2, InlineU32(11)),
      EncodeSopp(0x02, 1),
  };
  AppendVMovU32(&code, 2, 42);
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "BranchVccnzUsesWholeMask";
  test.code = code;
  test.expected = std::vector<u32>(8, 42);
  test.opcodes = {O::VMovB32,          O::VLshlrevB32,   O::VAddNcU32,
                  O::VCmpEqU32,        O::SCbranchVccnz, O::SBranch,
                  O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  test.dispatch_x = 2;
  return test;
}

TestCase ScalarMemoryLoadVariants() {
  using O = ShaderOpcode;

  struct Load {
    u32 opcode;
    u32 dst;
    u32 byte_offset;
    u32 dwords;
  };

  const Load s_loads[] = {
      {0x00, 20, 0, 1},  {0x01, 21, 4, 2},   {0x02, 23, 12, 4},
      {0x03, 27, 28, 8}, {0x04, 40, 60, 16},
  };
  const Load s_buffer_loads[] = {
      {0x08, 56, 0, 1},  {0x09, 57, 4, 2},   {0x0a, 59, 12, 4},
      {0x0b, 63, 28, 8}, {0x0c, 71, 60, 16},
  };

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(0, InlineU32(0)));
  for (const auto &load : s_loads) {
    AppendSmemLoadOpcode(&code, load.opcode, load.dst, load.byte_offset);
  }
  for (const auto &load : s_buffer_loads) {
    AppendSmemLoadOpcode(&code, load.opcode, load.dst, load.byte_offset);
  }

  u32 out = 0;
  for (const auto &load : s_loads) {
    for (u32 i = 0; i < load.dwords; i++) {
      AppendStoreSgpr(&code, load.dst + i, out++);
    }
  }
  for (const auto &load : s_buffer_loads) {
    for (u32 i = 0; i < load.dwords; i++) {
      AppendStoreSgpr(&code, load.dst + i, out++);
    }
  }
  AppendEnd(&code);

  std::vector<u32> initial;
  for (u32 i = 0; i < 31u; i++) {
    initial.push_back(0x10000000u + i);
  }
  std::vector<u32> expected = initial;
  expected.insert(expected.end(), initial.begin(), initial.end());

  return {"ScalarMemoryLoadVariants",
          code,
          initial,
          expected,
          {O::SMovB32, O::SLoadDword, O::SLoadDwordx2, O::SLoadDwordx4,
           O::SLoadDwordx8, O::SLoadDwordx16, O::SBufferLoadDword,
           O::SBufferLoadDwordx2, O::SBufferLoadDwordx4, O::SBufferLoadDwordx8,
           O::SBufferLoadDwordx16, O::VMovB32, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase ScalarLoadSignedImmediateOffsetAddsSoffset() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(0, InlineU32(8)));
  code.push_back(EncodeSMovB32(2, InlineU32(0)));
  code.push_back(EncodeSMovB32(3, InlineU32(0)));
  code.push_back(EncodeSmem0(0x00, 1, 1));
  code.push_back(EncodeSmem1(0x1ffffcu, 0));
  AppendStoreSgpr(&code, 1, 0);
  AppendEnd(&code);

  return {"ScalarLoadSignedImmediateOffsetAddsSoffset",
          code,
          {0x11111111u, 0x22222222u},
          {0x22222222u, 0x22222222u},
          {O::SMovB32, O::SLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadStore() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendBufferLoadDword(&code, 0, 30);
  code.push_back(EncodeVop1(0x01, 31, InlineU32(4)));
  AppendBufferStoreDword(&code, 0, 31);
  AppendEnd(&code);
  return {"BufferLoadStore",
          code,
          {0x11223344u, 0},
          {0x11223344u, 0x11223344u},
          {O::BufferLoadDword, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadDwordOffenIdxenUsesVaddrPlusOneOffset() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovU32(&code, 21, 8);
  code.push_back(EncodeMubuf0(0x0cu, 0, true, true));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  return {"BufferLoadDwordOffenIdxenUsesVaddrPlusOneOffset",
          code,
          {0x11111111u, 0x22222222u, 0x33333333u},
          {0x33333333u, 0x22222222u, 0x33333333u},
          {O::VMovB32, O::BufferLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferStoreDwordOffenIdxenUsesVaddrPlusOneOffset() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovU32(&code, 21, 12);
  AppendVMovLiteral(&code, 0, 0xabcdef01u);
  code.push_back(EncodeMubuf0(0x1cu, 0, true, true));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  return {"BufferStoreDwordOffenIdxenUsesVaddrPlusOneOffset",
          code,
          {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
          {0x11111111u, 0x22222222u, 0x33333333u, 0xabcdef01u},
          {O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadDwordNoAddressFlagsIgnoresVaddr() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 8);
  code.push_back(EncodeMubuf0(0x0cu, 0, false, false));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendStoreVgpr(&code, 0, 3);
  AppendEnd(&code);

  return {"BufferLoadDwordNoAddressFlagsIgnoresVaddr",
          code,
          {0x11111111u, 0x22222222u, 0x33333333u, 0},
          {0x11111111u, 0x22222222u, 0x33333333u, 0x11111111u},
          {O::VMovB32, O::BufferLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferLoadDwordIdxenUsesDescriptorStride() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 2);
  code.push_back(EncodeMubuf0(0x0cu, 0, true, false));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferLoadDwordIdxenUsesDescriptorStride";
  test.code = code;
  test.initial = {0x11111111u, 0x22222222u, 0x33333333u,
                  0x44444444u, 0x55555555u, 0x66666666u,
                  0x77777777u, 0x88888888u, 0x99aabbccu};
  test.expected = {0x99aabbccu, 0x22222222u, 0x33333333u,
                   0x44444444u, 0x55555555u, 0x66666666u,
                   0x77777777u, 0x88888888u, 0x99aabbccu};
  test.opcodes = {O::VMovB32, O::BufferLoadDword, O::BufferStoreDword,
                  O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(16, 3);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreDwordIdxenUsesDescriptorStride() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 2);
  AppendVMovLiteral(&code, 0, 0xabcdef01u);
  code.push_back(EncodeMubuf0(0x1cu, 0, true, false));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreDwordIdxenUsesDescriptorStride";
  test.code = code;
  test.initial = {0x11111111u, 0x22222222u, 0x33333333u,
                  0x44444444u, 0x55555555u, 0x66666666u,
                  0x77777777u, 0x88888888u, 0x99aabbccu};
  test.expected = {0x11111111u, 0x22222222u, 0x33333333u,
                   0x44444444u, 0x55555555u, 0x66666666u,
                   0x77777777u, 0x88888888u, 0xabcdef01u};
  test.opcodes = {O::VMovB32, O::BufferStoreDword, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(16, 3);
  test.has_user_data = true;
  return test;
}

TestCase BufferLoadVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendBufferLoadOpcode(&code, 0x08, 0, 20);
  AppendVMovU32(&code, 20, 2);
  AppendBufferLoadOpcode(&code, 0x09, 1, 20);
  AppendVMovU32(&code, 20, 0);
  AppendBufferLoadOpcode(&code, 0x0a, 2, 20);
  AppendVMovU32(&code, 20, 2);
  AppendBufferLoadOpcode(&code, 0x0b, 3, 20);
  AppendVMovU32(&code, 20, 8);
  AppendBufferLoadOpcode(&code, 0x0d, 4, 20);
  AppendBufferLoadOpcode(&code, 0x0f, 6, 20);
  AppendBufferLoadOpcode(&code, 0x0e, 9, 20);

  for (u32 i = 0; i < 13u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {"BufferLoadVariants",
          code,
          {0x80ff7f01u, 0x00008001u, 0x11223344u, 0x55667788u, 0x99aabbccu,
           0xddeeff00u},
          {0x01u, 0xffffffffu, 0x7f01u, 0xffff80ffu, 0x11223344u, 0x55667788u,
           0x11223344u, 0x55667788u, 0x99aabbccu, 0x11223344u, 0x55667788u,
           0x99aabbccu, 0xddeeff00u},
          {O::BufferLoadUbyte, O::BufferLoadSbyte, O::BufferLoadUshort,
           O::BufferLoadSshort, O::BufferLoadDwordx2, O::BufferLoadDwordx3,
           O::BufferLoadDwordx4, O::VMovB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferStoreVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 0, 0xaa);
  AppendBufferStoreOpcode(&code, 0x18, 0, 20);
  AppendVMovU32(&code, 20, 2);
  AppendVMovLiteral(&code, 1, 0x0000bbccu);
  AppendBufferStoreOpcode(&code, 0x1a, 1, 20);
  AppendVMovU32(&code, 20, 4);
  AppendVMovLiteral(&code, 2, 0x11111111u);
  AppendVMovLiteral(&code, 3, 0x22222222u);
  AppendBufferStoreOpcode(&code, 0x1d, 2, 20);
  AppendVMovU32(&code, 20, 12);
  AppendVMovLiteral(&code, 4, 0x33333333u);
  AppendVMovLiteral(&code, 5, 0x44444444u);
  AppendVMovLiteral(&code, 6, 0x55555555u);
  AppendBufferStoreOpcode(&code, 0x1f, 4, 20);
  AppendVMovU32(&code, 20, 24);
  AppendVMovLiteral(&code, 7, 0x66666666u);
  AppendVMovLiteral(&code, 8, 0x77777777u);
  AppendVMovLiteral(&code, 9, 0x88888888u);
  AppendVMovLiteral(&code, 10, 0x99999999u);
  AppendBufferStoreOpcode(&code, 0x1e, 7, 20);
  AppendEnd(&code);

  return {"BufferStoreVariants",
          code,
          std::vector<u32>(10, 0),
          {0xbbccaa00u, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
           0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u, 0x99999999u},
          {O::VMovB32, O::BufferStoreByte, O::BufferStoreShort,
           O::BufferStoreDwordx2, O::BufferStoreDwordx3, O::BufferStoreDwordx4,
           O::SEndpgm}};
}

TestCase BufferFormatVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendBufferLoadOpcode(&code, 0x00, 0, 20);
  AppendBufferLoadOpcode(&code, 0x01, 1, 20);
  AppendBufferLoadOpcode(&code, 0x02, 3, 20);
  AppendBufferLoadOpcode(&code, 0x03, 6, 20);
  for (u32 i = 0; i < 10u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  TestCase load;
  load.name = "BufferFormatLoadVariants";
  load.code = code;
  load.initial = {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u};
  load.expected = {0x01020304u, 0x01020304u, 0x11121314u, 0x01020304u,
                   0x11121314u, 0x21222324u, 0x01020304u, 0x11121314u,
                   0x21222324u, 0x31323334u};
  load.opcodes = {O::BufferLoadFormatX,
                  O::BufferLoadFormatXy,
                  O::BufferLoadFormatXyz,
                  O::BufferLoadFormatXyzw,
                  O::VMovB32,
                  O::BufferStoreDword,
                  O::SEndpgm};
  return load;
}

TestCase BufferFormatStoreVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0x01020304u);
  AppendBufferStoreOpcode(&code, 0x04, 0, 20);
  AppendVMovU32(&code, 20, 4);
  AppendVMovLiteral(&code, 1, 0x11121314u);
  AppendVMovLiteral(&code, 2, 0x21222324u);
  AppendBufferStoreOpcode(&code, 0x05, 1, 20);
  AppendVMovU32(&code, 20, 12);
  AppendVMovLiteral(&code, 3, 0x31323334u);
  AppendVMovLiteral(&code, 4, 0x41424344u);
  AppendVMovLiteral(&code, 5, 0x51525354u);
  AppendBufferStoreOpcode(&code, 0x06, 3, 20);
  AppendVMovU32(&code, 20, 24);
  AppendVMovLiteral(&code, 6, 0x61626364u);
  AppendVMovLiteral(&code, 7, 0x71727374u);
  AppendVMovLiteral(&code, 8, 0x81828384u);
  AppendVMovLiteral(&code, 9, 0x91929394u);
  AppendBufferStoreOpcode(&code, 0x07, 6, 20);
  AppendEnd(&code);

  return {"BufferFormatStoreVariants",
          code,
          std::vector<u32>(10, 0),
          {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u, 0x41424344u,
           0x51525354u, 0x61626364u, 0x71727374u, 0x81828384u, 0x91929394u},
          {O::VMovB32, O::BufferStoreFormatX, O::BufferStoreFormatXy,
           O::BufferStoreFormatXyz, O::BufferStoreFormatXyzw, O::SEndpgm}};
}

TestCase BufferLoadFormatXResource8UintZeroExtendsByte() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMubuf0(0x00u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendStoreVgpr(&code, 0, 1);
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferLoadFormatXResource8UintZeroExtendsByte";
  test.code = code;
  test.initial = {0x11223344u, 0};
  test.expected = {0x11223344u, 0x00000044u};
  test.opcodes = {O::VMovB32, O::BufferLoadFormatX, O::BufferStoreDword,
                  O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 5);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXResource16UintWritesHalfword() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0x0000aaaau);
  code.push_back(EncodeMubuf0(0x04u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXResource16UintWritesHalfword";
  test.code = code;
  test.initial = {0x11223344u};
  test.expected = {0x1122aaaau};
  test.opcodes = {O::VMovB32, O::BufferStoreFormatX, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 4, false, 11);
  test.has_user_data = true;
  return test;
}

TestCase BufferLoadFormatXyResource88UintExtractsBytes() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMubuf0(0x01u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendStoreVgpr(&code, 0, 2);
  AppendStoreVgpr(&code, 1, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferLoadFormatXyResource8_8UintExtractsBytes";
  test.code = code;
  test.initial = {0x0000807fu, 0x55667788u, 0, 0};
  test.expected = {0x0000807fu, 0x55667788u, 0x0000007fu, 0x00000080u};
  test.opcodes = {O::VMovB32, O::BufferLoadFormatXy, O::BufferStoreDword,
                  O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 18);
  test.has_user_data = true;
  return test;
}

TestCase BufferLoadFormatXyResource8888UnormConvertsFirstTwoComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMubuf0(0x01u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  TestCase test;
  test.name =
      "BufferLoadFormatXyResource8_8_8_8UnormConvertsFirstTwoComponents";
  test.code = code;
  test.initial = {0x44332211u, 0xdeadbeefu};
  test.expected = {0x3d888889u, 0x3e088889u};
  test.opcodes = {O::VMovB32, O::BufferLoadFormatXy, O::BufferStoreDword,
                  O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 56);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXyResource88UintWritesBytes() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0x000000aau);
  AppendVMovLiteral(&code, 1, 0x000000bbu);
  code.push_back(EncodeMubuf0(0x05u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXyResource8_8UintWritesBytes";
  test.code = code;
  test.initial = {0x11223344u, 0x55667788u};
  test.expected = {0x1122bbaau, 0x55667788u};
  test.opcodes = {O::VMovB32, O::BufferStoreFormatXy, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 18);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXyResource32UintWritesOneDword() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0xabcdef01u);
  AppendVMovLiteral(&code, 1, 0x12345678u);
  code.push_back(EncodeMubuf0(0x05u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXyResource32UintWritesOneDword";
  test.code = code;
  test.initial = {0x11111111u, 0x22222222u};
  test.expected = {0xabcdef01u, 0x22222222u};
  test.opcodes = {O::VMovB32, O::BufferStoreFormatXy, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 20);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXyzResource3232UintWritesTwoDwords() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0xabcdef01u);
  AppendVMovLiteral(&code, 1, 0x12345678u);
  AppendVMovLiteral(&code, 2, 0x0badc0deu);
  code.push_back(EncodeMubuf0(0x06u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXyzResource32_32UintWritesTwoDwords";
  test.code = code;
  test.initial = {0x11111111u, 0x22222222u, 0x33333333u};
  test.expected = {0xabcdef01u, 0x12345678u, 0x33333333u};
  test.opcodes = {O::VMovB32, O::BufferStoreFormatXyz, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 62);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXyzwResource323232UintWritesThreeDwords() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0xabcdef01u);
  AppendVMovLiteral(&code, 1, 0x12345678u);
  AppendVMovLiteral(&code, 2, 0x0badc0deu);
  AppendVMovLiteral(&code, 3, 0xfeedfaceu);
  code.push_back(EncodeMubuf0(0x07u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXyzwResource32_32_32UintWritesThreeDwords";
  test.code = code;
  test.initial = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
  test.expected = {0xabcdef01u, 0x12345678u, 0x0badc0deu, 0x44444444u};
  test.opcodes = {O::VMovB32, O::BufferStoreFormatXyzw, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 72);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXyzResource8UintWritesOneByte() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0x000000aau);
  AppendVMovLiteral(&code, 1, 0x000000bbu);
  AppendVMovLiteral(&code, 2, 0x000000ccu);
  code.push_back(EncodeMubuf0(0x06u));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXyzResource8UintWritesOneByte";
  test.code = code;
  test.initial = {0x11223344u, 0x55667788u, 0x99aabbccu};
  test.expected = {0x112233aau, 0x55667788u, 0x99aabbccu};
  test.opcodes = {O::VMovB32, O::BufferStoreFormatXyz, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(0, 8, false, 5);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXAddTidUsesLaneIndex() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x12345678u);
  code.push_back(EncodeMubuf0(0x04u, 0, false, false));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXAddTidUsesLaneIndex";
  test.code = code;
  test.initial = std::vector<u32>(4, 0);
  test.expected = std::vector<u32>(4, 0x12345678u);
  test.opcodes = {O::VMovB32, O::BufferStoreFormatX, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  test.user_data = MakeStructuredStorageBufferData(4, 4, true);
  test.has_user_data = true;
  return test;
}

TestCase BufferStoreFormatXDropsOutOfRangeRecord() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovLiteral(&code, 0, 0xabcdef01u);
  code.push_back(EncodeMubuf0(0x04u, 0, true, false));
  code.push_back(EncodeMubuf1(0, 0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "BufferStoreFormatXDropsOutOfRangeRecord";
  test.code = code;
  test.initial = {0x11111111u, 0x22222222u};
  test.expected = {0x11111111u, 0x22222222u};
  test.storage_buffer_range_dwords = 1;
  test.opcodes = {O::VMovB32, O::BufferStoreFormatX, O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(4, 1, false, 20);
  test.has_user_data = true;
  return test;
}

TestCase TBufferLoadVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadOpcode(&code, 0x00, 0, 20);
  AppendTBufferLoadOpcode(&code, 0x01, 1, 20);
  AppendTBufferLoadOpcode(&code, 0x02, 3, 20);
  AppendTBufferLoadOpcode(&code, 0x03, 6, 20);
  for (u32 i = 0; i < 10u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {"TBufferLoadVariants",
          code,
          {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u},
          {0x01020304u, 0x01020304u, 0x11121314u, 0x01020304u, 0x11121314u,
           0x21222324u, 0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u},
          {O::TBufferLoadFormatX, O::TBufferLoadFormatXy,
           O::TBufferLoadFormatXyz, O::TBufferLoadFormatXyzw, O::VMovB32,
           O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatX8UintWritesOneByte() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovU32(&code, 0, 0xaa);
  code.push_back(EncodeMtbuf0(0x04, 5, 0));
  code.push_back(EncodeMtbuf1(0x04, 0, 0, 20));
  AppendEnd(&code);

  return {"TBufferStoreFormatX8UintWritesOneByte",
          code,
          {0x11223344u},
          {0x112233aau},
          {O::VMovB32, O::TBufferStoreFormatX, O::SEndpgm}};
}

TestCase TBufferLoadFormatX8UintZeroExtendsByte() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x00, 5, 0));
  code.push_back(EncodeMtbuf1(0x00, 0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  return {"TBufferLoadFormatX8UintZeroExtendsByte",
          code,
          {0x11223344u, 0},
          {0x00000044u, 0},
          {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatX8888UintExtractsFirstByte() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadFormatOpcode(&code, 0x00, 0, 20,
                                Prospero::BufferFormat::k8_8_8_8UInt);
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  return {"TBufferLoadFormatX8_8_8_8UintExtractsFirstByte",
          code,
          {0x44332211u, 0},
          {0x00000011u, 0},
          {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXIdxenUsesDescriptorStride() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 2);
  code.push_back(EncodeMtbuf0(0x00, 5, 0, 0, true, false));
  code.push_back(EncodeMtbuf1(0x00, 0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "TBufferLoadFormatXIdxenUsesDescriptorStride";
  test.code = code;
  test.initial = {0x01020304u, 0, 0, 0, 0, 0, 0, 0, 0x0000007eu};
  test.expected = {0x0000007eu, 0, 0, 0, 0, 0, 0, 0, 0x0000007eu};
  test.opcodes = {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword,
                  O::SEndpgm};
  test.user_data = MakeStructuredStorageBufferData(16, 3);
  test.has_user_data = true;
  return test;
}

TestCase TBufferLoadFormatX16FloatConvertsToFloat() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadFormatOpcode(&code, 0x00, 0, 20, Prospero::BufferFormat::k16Float);
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  return {"TBufferLoadFormatX16FloatConvertsToFloat",
          code,
          {0x00003c00u, 0},
          {0x3f800000u, 0},
          {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXSintSignExtendsSubDword() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x00, 6, 0));
  code.push_back(EncodeMtbuf1(0x00, 0, 0, 20));
  AppendVMovU32(&code, 20, 4);
  code.push_back(EncodeMtbuf0(0x00, 12, 0));
  code.push_back(EncodeMtbuf1(0x00, 1, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  return {"TBufferLoadFormatXSintSignExtendsSubDword",
          code,
          {0x00000080u, 0x00008001u},
          {0xffffff80u, 0xffff8001u},
          {O::VMovB32, O::TBufferLoadFormatX, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXSintWritesSubDword() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0xffffff80u);
  code.push_back(EncodeMtbuf0(0x04, 6, 0));
  code.push_back(EncodeMtbuf1(0x04, 0, 0, 20));
  AppendVMovU32(&code, 20, 4);
  AppendVMovLiteral(&code, 1, 0xffff8001u);
  code.push_back(EncodeMtbuf0(0x04, 12, 0));
  code.push_back(EncodeMtbuf1(0x04, 1, 0, 20));
  AppendEnd(&code);

  return {"TBufferStoreFormatXSintWritesSubDword",
          code,
          {0x11223344u, 0x55667788u},
          {0x11223380u, 0x55668001u},
          {O::VMovB32, O::TBufferStoreFormatX, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy88IntegerComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x01, 2, 1));
  code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
  AppendVMovU32(&code, 20, 4);
  code.push_back(EncodeMtbuf0(0x01, 3, 1));
  code.push_back(EncodeMtbuf1(0x01, 2, 0, 20));
  for (u32 i = 0; i < 4; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXy8_8IntegerComponents",
      code,
      {0x0000807fu, 0x00007f80u, 0, 0},
      {0x0000007fu, 0x00000080u, 0xffffff80u, 0x0000007fu},
      {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXy88IntegerComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovU32(&code, 0, 0xaa);
  AppendVMovU32(&code, 1, 0xbb);
  code.push_back(EncodeMtbuf0(0x05, 2, 1));
  code.push_back(EncodeMtbuf1(0x05, 0, 0, 20));
  AppendVMovU32(&code, 20, 4);
  AppendVMovLiteral(&code, 2, 0xffffff80u);
  AppendVMovU32(&code, 3, 0x7f);
  code.push_back(EncodeMtbuf0(0x05, 3, 1));
  code.push_back(EncodeMtbuf1(0x05, 2, 0, 20));
  AppendEnd(&code);

  return {"TBufferStoreFormatXy8_8IntegerComponents",
          code,
          {0x11223344u, 0x55667788u},
          {0x1122bbaau, 0x55667f80u},
          {O::VMovB32, O::TBufferStoreFormatXy, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy1616IntegerComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x01, 11, 1));
  code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
  AppendVMovU32(&code, 20, 4);
  code.push_back(EncodeMtbuf0(0x01, 12, 1));
  code.push_back(EncodeMtbuf1(0x01, 2, 0, 20));
  for (u32 i = 0; i < 4; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXy16_16IntegerComponents",
      code,
      {0x80017fffu, 0x7fff8000u, 0, 0},
      {0x00007fffu, 0x00008001u, 0xffff8000u, 0x00007fffu},
      {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXy1616IntegerComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovU32(&code, 0, 0xaaaa);
  AppendVMovU32(&code, 1, 0xbbbb);
  code.push_back(EncodeMtbuf0(0x05, 11, 1));
  code.push_back(EncodeMtbuf1(0x05, 0, 0, 20));
  AppendVMovU32(&code, 20, 4);
  AppendVMovLiteral(&code, 2, 0xffff8000u);
  AppendVMovU32(&code, 3, 0x7fff);
  code.push_back(EncodeMtbuf0(0x05, 12, 1));
  code.push_back(EncodeMtbuf1(0x05, 2, 0, 20));
  AppendEnd(&code);

  return {"TBufferStoreFormatXy16_16IntegerComponents",
          code,
          {0x11223344u, 0x55667788u},
          {0xbbbbaaaau, 0x7fff8000u},
          {O::VMovB32, O::TBufferStoreFormatXy, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyz16161616UintLoadsHalfwords() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadFormatOpcode(&code, 0x02, 0, 20,
                                Prospero::BufferFormat::k16_16_16_16UInt);
  for (u32 i = 0; i < 3; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXyz16_16_16_16UintLoadsHalfwords",
      code,
      {0x22221111u, 0x44443333u, 0},
      {0x00001111u, 0x00002222u, 0x00003333u},
      {O::VMovB32, O::TBufferLoadFormatXyz, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy88UnormConvertsToFloat() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x01, 14, 0));
  code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXy8_8UnormConvertsToFloat",
      code,
      {0x0000ff80u, 0},
      {0x3f008081u, 0x3f800000u},
      {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy88SnormConvertsToFloat() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadFormatOpcode(&code, 0x01, 0, 20, Prospero::BufferFormat::k8_8SNorm);
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXy8_8SnormConvertsToFloat",
      code,
      {0x00007f80u, 0},
      {0xbf800000u, 0x3f800000u},
      {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy1616UnormConvertsToFloat() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x01, 7, 1));
  code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXy16_16UnormConvertsToFloat",
      code,
      {0xffff8000u, 0},
      {0x3f000080u, 0x3f800000u},
      {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXy8888UnormConvertsFirstTwoComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x01, 8, 3));
  code.push_back(EncodeMtbuf1(0x01, 0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXy8_8_8_8UnormConvertsFirstTwoComponents",
      code,
      {0x44332211u, 0xdeadbeefu},
      {0x3d888889u, 0x3e088889u},
      {O::VMovB32, O::TBufferLoadFormatXy, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyzw8888UintExtractsBytes() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadFormatOpcode(&code, 0x03, 0, 20,
                                Prospero::BufferFormat::k8_8_8_8UInt);
  for (u32 i = 0; i < 4; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXyzw8_8_8_8UintExtractsBytes",
      code,
      {0x44332211u, 0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu},
      {0x00000011u, 0x00000022u, 0x00000033u, 0x00000044u},
      {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyzw1010102SnormConvertsToFloat() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadFormatOpcode(&code, 0x03, 0, 20,
                                Prospero::BufferFormat::k10_10_10_2SNorm);
  for (u32 i = 0; i < 4; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXyzw10_10_10_2SnormConvertsToFloat",
      code,
      {0x800801ffu, 0, 0, 0},
      {0x3f800000u, 0xbf800000u, 0, 0xbf800000u},
      {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyz111110FloatUnpacks() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendTBufferLoadFormatOpcode(&code, 0x02, 0, 20,
                                Prospero::BufferFormat::k11_11_10Float);
  for (u32 i = 0; i < 3; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXyz11_11_10FloatUnpacks",
      code,
      {0x781e03c0u, 0, 0},
      {0x3f800000u, 0x3f800000u, 0x3f800000u},
      {O::VMovB32, O::TBufferLoadFormatXyz, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferLoadFormatXyzw3232FloatZerosMissingComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeMtbuf0(0x03, 0, 4));
  code.push_back(EncodeMtbuf1(0x03, 0, 0, 20));
  for (u32 i = 0; i < 4; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  return {
      "TBufferLoadFormatXyzw32_32FloatZerosMissingComponents",
      code,
      {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u},
      {0x11111111u, 0x22222222u, 0, 0},
      {O::VMovB32, O::TBufferLoadFormatXyzw, O::BufferStoreDword, O::SEndpgm}};
}

TestCase TBufferStoreFormatXyzw3232FloatWritesOnlyPresentComponents() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0x40000000u);
  AppendVMovLiteral(&code, 2, 0x40400000u);
  AppendVMovLiteral(&code, 3, 0x40800000u);
  code.push_back(EncodeMtbuf0(0x07, 0, 4));
  code.push_back(EncodeMtbuf1(0x07, 0, 0, 20));
  AppendEnd(&code);

  return {"TBufferStoreFormatXyzw32_32FloatWritesOnlyPresentComponents",
          code,
          {0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu, 0xddddddddu},
          {0x3f800000u, 0x40000000u, 0xccccccccu, 0xddddddddu},
          {O::VMovB32, O::TBufferStoreFormatXyzw, O::SEndpgm}};
}

TestCase TBufferStoreVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 0, 0x01020304u);
  AppendTBufferStoreOpcode(&code, 0x04, 0, 20);
  AppendVMovU32(&code, 20, 4);
  AppendVMovLiteral(&code, 1, 0x11121314u);
  AppendVMovLiteral(&code, 2, 0x21222324u);
  AppendTBufferStoreOpcode(&code, 0x05, 1, 20);
  AppendVMovU32(&code, 20, 12);
  AppendVMovLiteral(&code, 3, 0x31323334u);
  AppendVMovLiteral(&code, 4, 0x41424344u);
  AppendVMovLiteral(&code, 5, 0x51525354u);
  AppendTBufferStoreOpcode(&code, 0x06, 3, 20);
  AppendVMovU32(&code, 20, 24);
  AppendVMovLiteral(&code, 6, 0x61626364u);
  AppendVMovLiteral(&code, 7, 0x71727374u);
  AppendVMovLiteral(&code, 8, 0x81828384u);
  AppendVMovLiteral(&code, 9, 0x91929394u);
  AppendTBufferStoreOpcode(&code, 0x07, 6, 20);
  AppendEnd(&code);

  return {"TBufferStoreVariants",
          code,
          std::vector<u32>(10, 0),
          {0x01020304u, 0x11121314u, 0x21222324u, 0x31323334u, 0x41424344u,
           0x51525354u, 0x61626364u, 0x71727374u, 0x81828384u, 0x91929394u},
          {O::VMovB32, O::TBufferStoreFormatX, O::TBufferStoreFormatXy,
           O::TBufferStoreFormatXyz, O::TBufferStoreFormatXyzw, O::SEndpgm}};
}

TestCase FlatLoadVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeFlat0(0x08, 0, 0));
  code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x09, 0, 2));
  code.push_back(EncodeFlat1(1, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x0a, 0, 0));
  code.push_back(EncodeFlat1(2, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x0b, 0, 2));
  code.push_back(EncodeFlat1(3, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x0c, 0, 8));
  code.push_back(EncodeFlat1(13, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x0d, 0, 8));
  code.push_back(EncodeFlat1(4, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x0f, 0, 8));
  code.push_back(EncodeFlat1(6, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x0e, 0, 8));
  code.push_back(EncodeFlat1(9, 0x7d, 0, 20));

  for (u32 i = 0; i < 14u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  TestCase test{"FlatLoadVariants",
                code,
                {0x80ff7f01u, 0x00008001u, 0x11223344u, 0x55667788u,
                 0x99aabbccu, 0xddeeff00u},
                {0x01u, 0xffffffffu, 0x7f01u, 0xffff80ffu, 0x11223344u,
                 0x55667788u, 0x11223344u, 0x55667788u, 0x99aabbccu,
                 0x11223344u, 0x55667788u, 0x99aabbccu, 0xddeeff00u,
                 0x11223344u},
                {O::VMovB32, O::FlatLoadUbyte, O::FlatLoadSbyte,
                 O::FlatLoadUshort, O::FlatLoadSshort, O::FlatLoadDword,
                 O::FlatLoadDwordx2, O::FlatLoadDwordx3, O::FlatLoadDwordx4,
                 O::BufferStoreDword, O::SEndpgm}};
  test.flat_memory_base = 0;
  return test;
}

TestCase BranchVccnzUsesCarryProducedWholeMask() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),
      EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1),
      EncodeVop2(0x1a, 3, InlineU32(2), 1),
      EncodeVopc(0xc0, Vgpr(0), 0),
  };
  AppendVMovLiteral(&code, 2, 0xffffffffu);
  code.push_back(EncodeVop2(0x28, 5, Vgpr(2), 1));
  code.push_back(EncodeSopp(0x07, 2));
  code.push_back(EncodeVop1(0x01, 2, InlineU32(11)));
  code.push_back(EncodeSopp(0x02, 1));
  AppendVMovU32(&code, 2, 42);
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "BranchVccnzUsesCarryProducedWholeMask";
  test.code = code;
  test.expected = std::vector<u32>(8, 42);
  test.opcodes = {O::VMovB32,  O::VLshlrevB32,      O::VAddNcU32,
                  O::VCmpFU32, O::VAddcU32,         O::SCbranchVccnz,
                  O::SBranch,  O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  test.dispatch_x = 2;
  return test;
}

TestCase ScalarLoadAlignsComponentsAndMasksAddress() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, 1u);
  AppendSMovLiteral(&code, 2, 3u);
  AppendSMovLiteral(&code, 3, 0xffff0000u);
  code.push_back(EncodeSmem0(0x00, 1, 1));
  code.push_back(EncodeSmem1(3, 0));
  AppendStoreSgpr(&code, 1, 0);
  AppendEnd(&code);

  return {"ScalarLoadAlignsComponentsAndMasksAddress",
          code,
          {0x11111111u, 0x22222222u},
          {0x11111111u, 0x22222222u},
          {O::SMovB32, O::SLoadDword, O::BufferStoreDword, O::SEndpgm}};
}

TestCase FlatVirtualAddressRebasesGuestAllocation() {
  using O = ShaderOpcode;

  constexpr uint64_t GuestBase = 0x0000000110000000ull;
  std::vector<u32> code;
  AppendVMovLiteral(&code, 20, static_cast<u32>(GuestBase + 4u));
  AppendVMovLiteral(&code, 21, static_cast<u32>(GuestBase >> 32u));
  code.push_back(EncodeFlat0(0x0c, 0, 0));
  code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
  code.push_back(EncodeFlat0(0x0c, 2, 0));
  code.push_back(EncodeFlat1(1, 0x7d, 0, 20));
  AppendVMovLiteral(&code, 21, static_cast<u32>((GuestBase >> 32u) + 1u));
  code.push_back(EncodeFlat0(0x0c, 0, 0));
  code.push_back(EncodeFlat1(2, 0x7d, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendStoreVgpr(&code, 2, 2);
  AppendEnd(&code);

  TestCase test;
  test.name = "FlatVirtualAddressRebasesGuestAllocation";
  test.code = std::move(code);
  test.initial = {0, 0x12345678u};
  test.expected = {0x12345678u, 0x12345678u, 0};
  test.flat_memory_base = GuestBase;
  test.opcodes = {O::VMovB32, O::FlatLoadDword, O::BufferStoreDword,
                  O::SEndpgm};
  return test;
}

TestCase GlobalSignedImmediateRebasesBeforeSaddr() {
  using O = ShaderOpcode;

  constexpr uint64_t GuestBase = 0x0000000110000000ull;
  std::vector<u32> code;
  AppendSMovLiteral(&code, 0, static_cast<u32>(GuestBase));
  AppendSMovLiteral(&code, 1, static_cast<u32>(GuestBase >> 32u));
  AppendVMovU32(&code, 20, 8);
  code.push_back(EncodeFlat0(0x0c, 2, 0xffcu));
  code.push_back(EncodeFlat1(0, 0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  return {"GlobalSignedImmediateRebasesBeforeSaddr",
          code,
          {0x11111111u, 0x22222222u, 0x12345678u},
          {0x12345678u},
          {O::SMovB32, O::VMovB32, O::FlatLoadDword, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase FlatSegmentIgnoresSaddrAndMasksOffsetMsb() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(0, InlineU32(4)));
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeFlat0(0x0c, 0, 4));
  code.push_back(EncodeFlat1(0, 0, 0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeFlat0(0x0c, 0, 0x804));
  code.push_back(EncodeFlat1(1, 0x7d, 0, 20));
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  std::vector<u32> initial(514, 0);
  initial[1] = 0x11111111u;
  initial[2] = 0x22222222u;
  initial[513] = 0x33333333u;

  TestCase test;
  test.name = "FlatSegmentIgnoresSaddrAndMasksOffsetMsb";
  test.code = code;
  test.initial = initial;
  test.expected = {0x11111111u, 0x11111111u};
  test.opcodes = {O::SMovB32, O::VMovB32, O::FlatLoadDword, O::BufferStoreDword,
                  O::SEndpgm};
  test.flat_memory_base = 0;
  return test;
}

TestCase FlatStoreVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 0, 0xaa);
  code.push_back(EncodeFlat0(0x18, 0, 0));
  code.push_back(EncodeFlat1(0, 0x7d, 0, 20));
  AppendVMovU32(&code, 20, 2);
  AppendVMovLiteral(&code, 1, 0x0000bbccu);
  code.push_back(EncodeFlat0(0x1a, 0, 0));
  code.push_back(EncodeFlat1(0, 0x7d, 1, 20));
  AppendVMovU32(&code, 20, 4);
  AppendVMovLiteral(&code, 2, 0x11111111u);
  AppendVMovLiteral(&code, 3, 0x22222222u);
  code.push_back(EncodeFlat0(0x1d, 0, 0));
  code.push_back(EncodeFlat1(0, 0x7d, 2, 20));
  AppendVMovU32(&code, 20, 12);
  AppendVMovLiteral(&code, 4, 0x33333333u);
  AppendVMovLiteral(&code, 5, 0x44444444u);
  AppendVMovLiteral(&code, 6, 0x55555555u);
  code.push_back(EncodeFlat0(0x1f, 0, 0));
  code.push_back(EncodeFlat1(0, 0x7d, 4, 20));
  AppendVMovU32(&code, 20, 24);
  AppendVMovLiteral(&code, 7, 0x66666666u);
  AppendVMovLiteral(&code, 8, 0x77777777u);
  AppendVMovLiteral(&code, 9, 0x88888888u);
  AppendVMovLiteral(&code, 10, 0x99999999u);
  code.push_back(EncodeFlat0(0x1e, 0, 0));
  code.push_back(EncodeFlat1(0, 0x7d, 7, 20));
  AppendVMovU32(&code, 20, 0);
  code.push_back(EncodeFlat0(0x1c, 0, 40));
  code.push_back(EncodeFlat1(0, 0x7d, 2, 20));
  AppendEnd(&code);

  TestCase test{"FlatStoreVariants",
                code,
                std::vector<u32>(12, 0),
                {0xbbccaa00u, 0x11111111u, 0x22222222u, 0x33333333u,
                 0x44444444u, 0x55555555u, 0x66666666u, 0x77777777u,
                 0x88888888u, 0x99999999u, 0x11111111u, 0},
                {O::VMovB32, O::FlatStoreByte, O::FlatStoreShort,
                 O::FlatStoreDword, O::FlatStoreDwordx2, O::FlatStoreDwordx3,
                 O::FlatStoreDwordx4, O::SEndpgm}};
  test.flat_memory_base = 0;
  return test;
}

TestCase DsReadWriteVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 0);
  AppendVMovLiteral(&code, 2, 0x11223344u);
  code.push_back(EncodeDs0(0x0d, 0));
  code.push_back(EncodeDs1(0, 2, 1));
  code.push_back(EncodeDs0(0x36, 0));
  code.push_back(EncodeDs1(3, 0, 1));
  AppendVMovU32(&code, 4, 0xaa);
  code.push_back(EncodeDs0(0x1e, 4));
  code.push_back(EncodeDs1(0, 4, 1));
  code.push_back(EncodeDs0(0x3a, 4));
  code.push_back(EncodeDs1(5, 0, 1));
  code.push_back(EncodeDs0(0x39, 4));
  code.push_back(EncodeDs1(6, 0, 1));
  AppendVMovLiteral(&code, 7, 0x000080ffu);
  code.push_back(EncodeDs0(0x1f, 8));
  code.push_back(EncodeDs1(0, 7, 1));
  code.push_back(EncodeDs0(0x3c, 8));
  code.push_back(EncodeDs1(8, 0, 1));
  code.push_back(EncodeDs0(0x3b, 8));
  code.push_back(EncodeDs1(9, 0, 1));
  AppendVMovLiteral(&code, 10, 0x10101010u);
  AppendVMovLiteral(&code, 11, 0x11111111u);
  code.push_back(EncodeDs0(0x4d, 12));
  code.push_back(EncodeDs1(0, 10, 1));
  code.push_back(EncodeDs0(0x76, 12));
  code.push_back(EncodeDs1(14, 0, 1));
  AppendVMovLiteral(&code, 16, 0x20202020u);
  AppendVMovLiteral(&code, 17, 0x21212121u);
  AppendVMovLiteral(&code, 18, 0x22222222u);
  code.push_back(EncodeDs0(0xde, 20));
  code.push_back(EncodeDs1(0, 16, 1));
  code.push_back(EncodeDs0(0xfe, 20));
  code.push_back(EncodeDs1(19, 0, 1));
  AppendVMovLiteral(&code, 22, 0x30303030u);
  AppendVMovLiteral(&code, 23, 0x31313131u);
  AppendVMovLiteral(&code, 24, 0x32323232u);
  AppendVMovLiteral(&code, 25, 0x33333333u);
  code.push_back(EncodeDs0(0xdf, 32));
  code.push_back(EncodeDs1(0, 22, 1));
  code.push_back(EncodeDs0(0xff, 32));
  code.push_back(EncodeDs1(26, 0, 1));

  const u32 results[] = {3, 5, 6, 8, 9, 14, 15, 19, 20, 21, 26, 27, 28, 29};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"DsReadWriteVariants",
          code,
          std::vector<u32>(14, 0),
          {0x11223344u, 0xaau, 0xffffffaau, 0x80ffu, 0xffff80ffu, 0x10101010u,
           0x11111111u, 0x20202020u, 0x21212121u, 0x22222222u, 0x30303030u,
           0x31313131u, 0x32323232u, 0x33333333u},
          {O::VMovB32, O::DsWriteB32, O::DsReadB32, O::DsWriteByte,
           O::DsReadUbyte, O::DsReadSbyte, O::DsWriteShort, O::DsReadUshort,
           O::DsReadSshort, O::DsWriteB64, O::DsReadB64, O::DsWriteB96,
           O::DsReadB96, O::DsWriteB128, O::DsReadB128, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase DsReadWrite2Variants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 0);
  AppendVMovLiteral(&code, 2, 0x11111111u);
  AppendVMovLiteral(&code, 3, 0x22222222u);
  code.push_back(EncodeDs0(0x0e, (3u << 8u) | 1u));
  code.push_back(EncodeDs1Ex(0, 3, 2, 1));
  code.push_back(EncodeDs0(0x37, (3u << 8u) | 1u));
  code.push_back(EncodeDs1Ex(4, 0, 0, 1));
  AppendVMovLiteral(&code, 6, 0x33333333u);
  AppendVMovLiteral(&code, 7, 0x44444444u);
  AppendVMovLiteral(&code, 8, 0x55555555u);
  AppendVMovLiteral(&code, 9, 0x66666666u);
  code.push_back(EncodeDs0(0x4d, 32));
  code.push_back(EncodeDs1(0, 6, 1));
  code.push_back(EncodeDs0(0x4d, 48));
  code.push_back(EncodeDs1(0, 8, 1));
  code.push_back(EncodeDs0(0x77, (6u << 8u) | 4u));
  code.push_back(EncodeDs1Ex(10, 0, 0, 1));
  AppendVMovLiteral(&code, 14, 0x77777777u);
  AppendVMovLiteral(&code, 15, 0x88888888u);
  code.push_back(EncodeDs0(0x0f, (2u << 8u) | 1u));
  code.push_back(EncodeDs1Ex(0, 15, 14, 1));
  code.push_back(EncodeDs0(0x38, (2u << 8u) | 1u));
  code.push_back(EncodeDs1Ex(16, 0, 0, 1));
  AppendVMovLiteral(&code, 18, 0x99999999u);
  AppendVMovLiteral(&code, 19, 0xaaaaaaaau);
  AppendVMovLiteral(&code, 20, 0xbbbbbbbbu);
  AppendVMovLiteral(&code, 21, 0xccccccccu);
  code.push_back(EncodeDs0(0x4e, (10u << 8u) | 8u));
  code.push_back(EncodeDs1Ex(0, 20, 18, 1));
  code.push_back(EncodeDs0(0x77, (10u << 8u) | 8u));
  code.push_back(EncodeDs1Ex(22, 0, 0, 1));
  AppendVMovLiteral(&code, 26, 0xddddddddu);
  AppendVMovLiteral(&code, 27, 0xeeeeeeeeu);
  AppendVMovLiteral(&code, 28, 0xf0f0f0f0u);
  AppendVMovLiteral(&code, 29, 0x12345678u);
  code.push_back(EncodeDs0(0x4f, (2u << 8u) | 1u));
  code.push_back(EncodeDs1Ex(0, 28, 26, 1));
  code.push_back(EncodeDs0(0x78, (2u << 8u) | 1u));
  code.push_back(EncodeDs1Ex(34, 0, 0, 1));

  const u32 results[] = {4,  5,  10, 11, 12, 13, 22, 23,
                         24, 25, 16, 17, 34, 35, 36, 37};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  return {"DsReadWrite2Variants",
          code,
          std::vector<u32>(16, 0),
          {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u,
           0x66666666u, 0x99999999u, 0xaaaaaaaau, 0xbbbbbbbbu, 0xccccccccu,
           0x77777777u, 0x88888888u, 0xddddddddu, 0xeeeeeeeeu, 0xf0f0f0f0u,
           0x12345678u},
          {O::VMovB32, O::DsWrite2B32, O::DsRead2B32, O::DsWriteB64,
           O::DsRead2B64, O::DsWrite2B64, O::DsWrite2St64B32,
           O::DsWrite2St64B64, O::DsRead2St64B64, O::BufferStoreDword,
           O::SEndpgm}};
}

TestCase DsAtomicNoReturnVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 0);
  const u32 initial[] = {10, 10,      0xfffffff0u, 0xfffffff0u, 10,
                         10, 0xf0f0u, 0xf000u,     0xf00fu};
  const u32 values[] = {5, 3, 5, 5, 5, 20, 0x0ff0u, 0x0f00u, 0x00ffu};
  const u32 ops[] = {0x00, 0x01, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b};
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendVMovLiteral(&code, 2, initial[i]);
    code.push_back(EncodeDs0(0x0d, i * 4u));
    code.push_back(EncodeDs1(0, 2, 1));
    AppendVMovLiteral(&code, 3, values[i]);
    code.push_back(EncodeDs0(ops[i], i * 4u));
    code.push_back(EncodeDs1(0, 3, 1));
    code.push_back(EncodeDs0(0x36, i * 4u));
    code.push_back(EncodeDs1(10u + i, 0, 1));
  }
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendStoreVgpr(&code, 10u + i, i);
  }
  AppendEnd(&code);

  return {"DsAtomicNoReturnVariants",
          code,
          std::vector<u32>(9, 0),
          {15, 7, 0xfffffff0u, 5, 5, 20, 0x00f0u, 0xff00u, 0xf0f0u},
          {O::VMovB32, O::DsWriteB32, O::DsAddU32, O::DsSubU32, O::DsMinI32,
           O::DsMaxI32, O::DsMinU32, O::DsMaxU32, O::DsAndB32, O::DsOrB32,
           O::DsXorB32, O::DsReadB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsAtomicReturnVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 0);
  const u32 initial[] = {10, 10,      0xfffffff0u, 0xfffffff0u, 10,
                         10, 0xf0f0u, 0xf000u,     0xf00fu,     10};
  const u32 values[] = {5, 3, 5, 5, 5, 20, 0x0ff0u, 0x0f00u, 0x00ffu, 99};
  const u32 ops[] = {0x20, 0x21, 0x25, 0x26, 0x27,
                     0x28, 0x29, 0x2a, 0x2b, 0x2d};
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendVMovLiteral(&code, 2, initial[i]);
    code.push_back(EncodeDs0(0x0d, i * 4u));
    code.push_back(EncodeDs1(0, 2, 1));
    AppendVMovLiteral(&code, 3, values[i]);
    code.push_back(EncodeDs0(ops[i], i * 4u));
    code.push_back(EncodeDs1(10u + i, 3, 1));
    code.push_back(EncodeDs0(0x36, i * 4u));
    code.push_back(EncodeDs1(20u + i, 0, 1));
  }
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendStoreVgpr(&code, 10u + i, i);
  }
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendStoreVgpr(&code, 20u + i, i + 10u);
  }
  AppendEnd(&code);

  return {
      "DsAtomicReturnVariants",
      code,
      std::vector<u32>(20, 0),
      {10, 10, 0xfffffff0u, 0xfffffff0u, 10, 10, 0xf0f0u, 0xf000u, 0xf00fu, 10,
       15, 7,  0xfffffff0u, 5,           5,  20, 0x00f0u, 0xff00u, 0xf0f0u, 99},
      {O::VMovB32, O::DsWriteB32, O::DsAddRtnU32, O::DsSubRtnU32,
       O::DsMinRtnI32, O::DsMaxRtnI32, O::DsMinRtnU32, O::DsMaxRtnU32,
       O::DsAndRtnB32, O::DsOrRtnB32, O::DsXorRtnB32, O::DsWrxchgRtnB32,
       O::DsReadB32, O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsMiscVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 0);
  AppendVMovLiteral(&code, 2, 0x40800000u);
  code.push_back(EncodeDs0(0x0d, 0));
  code.push_back(EncodeDs1(0, 2, 1));
  AppendVMovLiteral(&code, 3, 0x3f800000u);
  code.push_back(EncodeDs0(0x0d, 4));
  code.push_back(EncodeDs1(0, 3, 1));
  AppendVMovLiteral(&code, 4, 0x40000000u);
  code.push_back(EncodeDs0(0x12, 0));
  code.push_back(EncodeDs1(0, 4, 1));
  AppendVMovLiteral(&code, 5, 0x40400000u);
  code.push_back(EncodeDs0(0x13, 4));
  code.push_back(EncodeDs1(0, 5, 1));
  code.push_back(EncodeDs0(0x36, 0));
  code.push_back(EncodeDs1(6, 0, 1));
  code.push_back(EncodeDs0(0x36, 4));
  code.push_back(EncodeDs1(7, 0, 1));
  AppendVMovLiteral(&code, 8, 0x12345678u);
  code.push_back(EncodeDs0(0x35, 0x001f));
  code.push_back(EncodeDs1(9, 0, 8));
  code.push_back(EncodeSMovB32(124, InlineU32(0)));
  AppendVMovLiteral(&code, 10, 0xabcdef01u);
  code.push_back(EncodeDs0(0xb0, 8));
  code.push_back(EncodeDs1(0, 10, 0));
  code.push_back(EncodeDs0(0xb1, 8));
  code.push_back(EncodeDs1(11, 0, 0));

  const u32 results[] = {6, 7, 9, 11};
  for (u32 i = 0; i < static_cast<u32>(std::size(results)); i++) {
    AppendStoreVgpr(&code, results[i], i);
  }
  AppendEnd(&code);

  TestCase test;
  test.name = "DsMiscVariants";
  test.code = code;
  test.initial = std::vector<u32>(4, 0);
  test.expected = {0x40000000u, 0x40400000u, 0x12345678u, 0xabcdef01u};
  test.opcodes = {O::VMovB32,          O::SMovB32,          O::DsWriteB32,
                  O::DsMinF32,         O::DsMaxF32,         O::DsReadB32,
                  O::DsSwizzleB32,     O::DsWriteAddtidB32, O::DsReadAddtidB32,
                  O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 1;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.has_compute_info = true;
  return test;
}

TestCase DsFloatMinMaxUsesSeparateCompareOperand() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 0);
  AppendVMovLiteral(&code, 2, 0x40800000u);
  code.push_back(EncodeDs0(0x0d, 0));
  code.push_back(EncodeDs1(0, 2, 1));
  AppendVMovLiteral(&code, 3, 0x40800000u);
  code.push_back(EncodeDs0(0x0d, 4));
  code.push_back(EncodeDs1(0, 3, 1));
  AppendVMovLiteral(&code, 4, 0x41100000u);
  AppendVMovLiteral(&code, 5, 0x40000000u);
  code.push_back(EncodeDs0(0x12, 0));
  code.push_back(EncodeDs1Ex(0, 5, 4, 1));
  AppendVMovLiteral(&code, 6, 0x3f800000u);
  AppendVMovLiteral(&code, 7, 0x40400000u);
  code.push_back(EncodeDs0(0x13, 4));
  code.push_back(EncodeDs1Ex(0, 7, 6, 1));
  code.push_back(EncodeDs0(0x36, 0));
  code.push_back(EncodeDs1(8, 0, 1));
  code.push_back(EncodeDs0(0x36, 4));
  code.push_back(EncodeDs1(9, 0, 1));
  AppendStoreVgpr(&code, 8, 0);
  AppendStoreVgpr(&code, 9, 1);
  AppendEnd(&code);

  TestCase test;
  test.name = "DsFloatMinMaxUsesSeparateCompareOperand";
  test.code = code;
  test.initial = std::vector<u32>(2, 0);
  test.expected = {0x41100000u, 0x3f800000u};
  test.opcodes = {O::VMovB32,   O::DsWriteB32,       O::DsMinF32, O::DsMaxF32,
                  O::DsReadB32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 1;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.has_compute_info = true;
  return test;
}

TestCase DsSwizzleInvalidSourceLaneZero() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 1, 100);
  code.push_back(EncodeDs0(0x35, 0x00e0));
  code.push_back(EncodeDs1(2, 0, 1));
  code.push_back(EncodeVop2(0x1a, 3, InlineU32(2), 0));
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "DsSwizzleInvalidSourceLaneZero";
  test.code = code;
  test.expected = {0, 0, 0, 0};
  test.opcodes = {O::VMovB32, O::DsSwizzleB32, O::VLshlrevB32,
                  O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  return test;
}

TestCase BufferAtomicVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  const u32 values[] = {100, 5, 3, 5, 5, 5, 20, 0x0ff0u, 0x0f00u, 0x00ffu};
  const u32 ops[] = {0x30, 0x32, 0x33, 0x35, 0x36,
                     0x37, 0x38, 0x39, 0x3a, 0x3b};
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendVMovU32(&code, 20, i * 4u);
    AppendVMovU32(&code, i, values[i]);
    AppendBufferStoreOpcode(&code, ops[i], i, 20, true);
  }
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendStoreVgpr(&code, i, i + 10u);
  }
  AppendEnd(&code);

  return {
      "BufferAtomicVariants",
      code,
      {10, 10, 10, 0xfffffff0u, 10, 0xfffffff0u, 10, 0xf0f0u, 0xf000u, 0xf00fu,
       0,  0,  0,  0,           0,  0,           0,  0,       0,       0},
      {100,     15,          7,       0xfffffff0u, 5,       5,      20,
       0x00f0u, 0xff00u,     0xf0f0u, 10,          10,      10,     0xfffffff0u,
       10,      0xfffffff0u, 10,      0xf0f0u,     0xf000u, 0xf00fu},
      {O::VMovB32, O::BufferAtomicSwap, O::BufferAtomicAdd, O::BufferAtomicSub,
       O::BufferAtomicSMin, O::BufferAtomicUMin, O::BufferAtomicSMax,
       O::BufferAtomicUMax, O::BufferAtomicAnd, O::BufferAtomicOr,
       O::BufferAtomicXor, O::BufferStoreDword, O::SEndpgm}};
}

TestCase BufferAtomicGlc0DoesNotReturnOldValue() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovU32(&code, 0, 5);
  AppendBufferStoreOpcode(&code, 0x32, 0, 20);
  AppendStoreVgpr(&code, 0, 1);
  AppendEnd(&code);

  return {"BufferAtomicGlc0DoesNotReturnOldValue",
          code,
          {10, 0},
          {15, 5},
          {O::VMovB32, O::BufferAtomicAdd, O::BufferStoreDword, O::SEndpgm}};
}

std::vector<u32> MakeRgbaImage(u32 width, u32 height, u32 value = 0) {
  return std::vector<u32>(static_cast<size_t>(width) * height * 4u, value);
}

void SetRgbaPixel(std::vector<u32> *image, u32 width, u32 x, u32 y, u32 r,
                  u32 g, u32 b, u32 a) {
  const auto base = static_cast<size_t>((y * width + x) * 4u);
  (*image)[base + 0] = r;
  (*image)[base + 1] = g;
  (*image)[base + 2] = b;
  (*image)[base + 3] = a;
}

TestCase ImageLoadVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 2);
  AppendVMovU32(&code, 21, 1);
  AppendVMovU32(&code, 22, 0);
  AppendVMovU32(&code, 23, 0);
  code.push_back(EncodeMimg0(0x00, 0xf));
  code.push_back(EncodeMimg1(0, 20));
  code.push_back(EncodeMimg0(0x01, 0xf));
  code.push_back(EncodeMimg1(4, 20));
  AppendVMovU32(&code, 24, 0);
  code.push_back(EncodeMimg0(0x0e, 0x1));
  code.push_back(EncodeMimg1(8, 24));
  for (u32 i = 0; i < 9u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  auto image = MakeRgbaImage(4, 4);
  SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u,
               0x40800000u);

  TestCase test;
  test.name = "ImageLoadVariants";
  test.code = code;
  test.expected = {0x3f800000u, 0x40000000u, 0x40400000u,
                   0x40800000u, 0x3f800000u, 0x40000000u,
                   0x40400000u, 0x40800000u, 4};
  test.opcodes = {O::VMovB32,         O::ImageLoad,        O::ImageLoadMip,
                  O::ImageGetResinfo, O::BufferStoreDword, O::SEndpgm};
  test.sampled_image_rgba = image;
  return test;
}

TestCase DsAppendConsumeUsesEncodedLdsSelector() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 124, 0x0000ffffu);
  AppendVMovU32(&code, 1, 0);
  AppendVMovU32(&code, 2, 10);
  code.push_back(EncodeDs0(0x0d, 0));
  code.push_back(EncodeDs1(0, 2, 1));
  code.push_back(EncodeDs0(0x3e, 0));
  code.push_back(EncodeDs1(3, 0, 0));
  code.push_back(EncodeDs0(0x3d, 0));
  code.push_back(EncodeDs1(4, 0, 0));
  AppendSMovLiteral(&code, 124, 0);
  code.push_back(EncodeDs0(0x3e, 0));
  code.push_back(EncodeDs1(5, 0, 0));
  code.push_back(EncodeDs0(0x36, 0));
  code.push_back(EncodeDs1(6, 0, 1));
  AppendStoreVgpr(&code, 3, 0);
  AppendStoreVgpr(&code, 4, 1);
  AppendStoreVgpr(&code, 5, 2);
  AppendStoreVgpr(&code, 6, 3);
  AppendEnd(&code);

  return {"DsAppendConsumeLdsSelector",
          code,
          {},
          {10, 74, 0, 10},
          {O::SMovB32, O::VMovB32, O::DsWriteB32, O::DsReadB32, O::DsAppend,
           O::DsConsume, O::BufferStoreDword, O::SEndpgm}};
}

TestCase DsAppendUsesEncodedGdsSelector() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendSMovLiteral(&code, 124, 0x00000001u);
  code.push_back(EncodeDs0(0x3e, 0, true));
  code.push_back(EncodeDs1(0, 0, 0));
  code.push_back(EncodeDs0(0x3d, 0, true));
  code.push_back(EncodeDs1(1, 0, 0));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  TestCase test{
      "DsAppendGdsSelector",
      code,
      {},
      {10, 74},
      {O::SMovB32, O::DsAppend, O::DsConsume, O::BufferStoreDword, O::SEndpgm}};
  test.gds_initial = {10};
  test.expected_gds = {10};
  return test;
}

TestCase DsGdsSubdwordAndAtomicWrites() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeVop1(0x01, 1, Vgpr(0)));
  code.push_back(EncodeVop2(0x25, 2, InlineU32(1), 0));
  code.push_back(EncodeDs0(0x1e, 0, true));
  code.push_back(EncodeDs1(0, 2, 1));
  code.push_back(EncodeVop2(0x1a, 8, InlineU32(1), 0));
  code.push_back(EncodeVop2(0x25, 8, InlineU32(12), 8));
  code.push_back(EncodeDs0(0x1f, 0, true));
  code.push_back(EncodeDs1(0, 2, 8));
  AppendVMovU32(&code, 3, 4);
  AppendVMovU32(&code, 4, 1);
  code.push_back(EncodeDs0(0x00, 0, true));
  code.push_back(EncodeDs1(0, 4, 3));
  AppendVMovU32(&code, 5, 8);
  AppendVMovLiteral(&code, 6, 0x40a00000u);
  AppendVMovLiteral(&code, 7, 0x40a00000u);
  code.push_back(EncodeDs0(0x12, 0, true));
  code.push_back(EncodeDs1Ex(0, 7, 6, 5));
  AppendEnd(&code);

  TestCase test;
  test.name = "DsGdsSubdwordAndAtomics";
  test.code = code;
  test.opcodes = {O::VMovB32,      O::VAddNcU32, O::VLshlrevB32, O::DsWriteByte,
                  O::DsWriteShort, O::DsAddU32,  O::DsMinF32,    O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  test.gds_initial = {0, 0, 0x42c80000u, 0, 0};
  test.expected_gds = {0x04030201u, 4, 0x40a00000u, 0x00020001u, 0x00040003u};
  return test;
}

TestCase ImageLoadR32UintUsesIntegerSampledImage() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 2);
  AppendVMovU32(&code, 21, 1);
  code.push_back(EncodeMimg0(0x00, 0x1));
  code.push_back(EncodeMimg1(0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageLoadR32UintUsesIntegerSampledImage";
  test.code = code;
  test.expected = {0xdeadbeefu};
  test.opcodes = {O::VMovB32, O::ImageLoad, O::BufferStoreDword, O::SEndpgm};
  test.sampled_image_rgba.resize(16);
  test.sampled_image_rgba[6] = 0xdeadbeefu;
  test.sampled_image_format = VK_FORMAT_R32_UINT;
  test.sampled_image_dwords_per_pixel = 1;
  test.user_data = MakeSampledTextureData(Prospero::BufferFormat::k32UInt);
  test.has_user_data = true;
  test.required_spirv = {"sampled_uint_2d"};
  return test;
}

TestCase ImageLoadMipUsesVaddr2Lod2D() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 21, 1);
  AppendVMovU32(&code, 22, 1);
  AppendVMovU32(&code, 23, 0);
  code.push_back(EncodeMimg0(0x01, 0x1));
  code.push_back(EncodeMimg1(0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  auto base = MakeRgbaImage(4, 4);
  SetRgbaPixel(&base, 4, 1, 1, 0x3f800000u, 0, 0, 0);
  auto mip1 = MakeRgbaImage(2, 2);
  SetRgbaPixel(&mip1, 2, 1, 1, 0x40000000u, 0, 0, 0);

  TestCase test;
  test.name = "ImageLoadMipUsesVaddr2Lod2D";
  test.code = code;
  test.expected = {0x40000000u};
  test.opcodes = {O::VMovB32, O::ImageLoadMip, O::BufferStoreDword, O::SEndpgm};
  test.sampled_image_rgba_mips = {base, mip1};
  return test;
}

TestCase ImageLoadMipNsaUsesSelectedAddressVgprs() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 21, 0);
  AppendVMovU32(&code, 22, 0);
  AppendVMovU32(&code, 30, 1);
  AppendVMovU32(&code, 31, 1);
  code.push_back(EncodeMimg0(0x01, 0x1, 1));
  code.push_back(EncodeMimg1(0, 20));
  code.push_back((30u << 0u) | (31u << 8u));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  auto base = MakeRgbaImage(4, 4);
  SetRgbaPixel(&base, 4, 1, 0, 0x3f800000u, 0, 0, 0);
  auto mip1 = MakeRgbaImage(2, 2);
  SetRgbaPixel(&mip1, 2, 1, 1, 0x40a00000u, 0, 0, 0);

  TestCase test;
  test.name = "ImageLoadMipNsaUsesSelectedAddressVgprs";
  test.code = code;
  test.expected = {0x40a00000u};
  test.opcodes = {O::VMovB32, O::ImageLoadMip, O::BufferStoreDword, O::SEndpgm};
  test.sampled_image_rgba_mips = {base, mip1};
  return test;
}

TestCase ImageLoadA16UintCoordsOnGpu() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 20, 0x00010002u); // x=2, y=1 packed as u16.
  AppendVMovU32(&code, 21, 0);
  code.push_back(EncodeMimg0(0x00, 0xf));
  code.push_back(EncodeMimg1(0, 20, 0, 0, true));
  for (u32 i = 0; i < 4u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  auto image = MakeRgbaImage(4, 4);
  SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u,
               0x40800000u);

  TestCase test;
  test.name = "ImageLoadA16UintCoordsOnGpu";
  test.code = code;
  test.expected = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
  test.opcodes = {O::VMovB32, O::ImageLoad, O::BufferStoreDword, O::SEndpgm};
  test.sampled_image_rgba = image;
  test.required_spirv = {"OpShiftRightLogical", "OpBitwiseAnd"};
  test.forbidden_spirv = {"UnpackHalf2x16"};
  return test;
}

TestCase ImageGetResinfoDmaskWidthHeight() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 24, 0);
  code.push_back(EncodeMimg0(0x0e, 0x3));
  code.push_back(EncodeMimg1(0, 24));
  AppendStoreVgpr(&code, 0, 0);
  AppendStoreVgpr(&code, 1, 1);
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageGetResinfoDmaskWidthHeight";
  test.code = code;
  test.expected = {4, 2};
  test.opcodes = {O::VMovB32, O::ImageGetResinfo, O::BufferStoreDword,
                  O::SEndpgm};
  test.image_width = 4;
  test.image_height = 2;
  test.sampled_image_rgba = MakeRgbaImage(test.image_width, test.image_height);
  return test;
}

TestCase ImageGetResinfoDmaskMipLevels() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 24, 0);
  code.push_back(EncodeMimg0(0x0e, 0x8));
  code.push_back(EncodeMimg1(0, 24));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageGetResinfoDmaskMipLevels";
  test.code = code;
  test.expected = {2};
  test.opcodes = {O::VMovB32, O::ImageGetResinfo, O::BufferStoreDword,
                  O::SEndpgm};
  auto base = MakeRgbaImage(4, 4);
  auto mip1 = MakeRgbaImage(2, 2);
  test.sampled_image_rgba_mips = {base, mip1};
  return test;
}

SkippedCase ImageStoreMipWritesExplicitMip2D() {
  return {"ImageStoreMipWritesExplicitMip2D",
          "requires per-mip storage-image view descriptors; Vulkan "
          "OpImageWrite cannot take Lod"};
}

TestCase ImageSampleAndGather() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 20, 0x3f200000u);
  AppendVMovLiteral(&code, 21, 0x3ec00000u);
  AppendVMovU32(&code, 22, 0);
  code.push_back(EncodeMimg0(0x20, 0xf));
  code.push_back(EncodeMimg1(0, 20));
  code.push_back(EncodeMimg0(0x47, 0x1));
  code.push_back(EncodeMimg1(4, 20));
  AppendVMovU32(&code, 24, 0);
  AppendVMovLiteral(&code, 25, 0x3f200000u);
  AppendVMovLiteral(&code, 26, 0x3ec00000u);
  AppendVMovU32(&code, 27, 0);
  code.push_back(EncodeMimg0(0x57, 0x1));
  code.push_back(EncodeMimg1(8, 24));
  code.push_back(EncodeMimg0(0x60, 0x1));
  code.push_back(EncodeMimg1(12, 20));
  for (u32 i = 0; i < 13u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  auto image = MakeRgbaImage(4, 4);
  for (u32 y = 0; y < 4u; y++) {
    for (u32 x = 0; x < 4u; x++) {
      SetRgbaPixel(&image, 4, x, y, 0x3f800000u, 0, 0, 0);
    }
  }
  SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u,
               0x40800000u);

  TestCase test;
  test.name = "ImageSampleAndGather";
  test.code = code;
  test.expected = {0x3f800000u,
                   0x40000000u,
                   0x40400000u,
                   0x40800000u,
                   0x3f800000u,
                   0x3f800000u,
                   0x3f800000u,
                   0x3f800000u,
                   0x3f800000u,
                   0x3f800000u,
                   0x3f800000u,
                   0x3f800000u,
                   0};
  test.opcodes = {O::VMovB32,        O::ImageSample,     O::ImageGetLod,
                  O::ImageGather4Lz, O::ImageGather4LzO, O::BufferStoreDword,
                  O::SEndpgm};
  test.sampled_image_rgba = image;
  return test;
}

TestCase ImageSampleA16SamplerCoordsOnGpu() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 20, 0x36003900u); // x=0.625, y=0.375 packed as f16.
  AppendVMovU32(&code, 21, 0);
  code.push_back(EncodeMimg0(0x20, 0xf));
  code.push_back(EncodeMimg1(0, 20, 0, 0, true));
  for (u32 i = 0; i < 4u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  auto image = MakeRgbaImage(4, 4);
  SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u,
               0x40800000u);

  TestCase test;
  test.name = "ImageSampleA16SamplerCoordsOnGpu";
  test.code = code;
  test.expected = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
  test.opcodes = {O::VMovB32, O::ImageSample, O::BufferStoreDword, O::SEndpgm};
  test.sampled_image_rgba = image;
  test.required_spirv = {"UnpackHalf2x16"};
  return test;
}

TestCase ImageSampleOpcodeAliasUsesNormalCoords() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 2, 0x3f000000u);
  AppendVMovLiteral(&code, 3, 0x3f000000u);
  code.push_back(0xf0800109u); // observed image_sample_a v6, v2, s0, s24
  code.push_back(0x00c00602u);
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageSampleOpcodeAliasUsesNormalCoords";
  test.code = code;
  test.opcodes = {O::VMovB32, O::ImageSample, O::SEndpgm};
  test.required_spirv = {"OpImageSampleExplicitLod"};
  test.forbidden_spirv = {"UnpackHalf2x16"};
  test.compile_only = true;
  return test;
}

TestCase ImageSampleA16OffsetKeepsTexelOffset32BitOnGpu() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0); // Texel offset stays one packed 32-bit VGPR.
  AppendVMovLiteral(&code, 21, 0x36003900u); // x=0.625, y=0.375 packed as f16.
  AppendVMovU32(&code, 22, 0);
  code.push_back(EncodeMimg0(0x30, 0xf));
  code.push_back(EncodeMimg1(0, 20, 0, 0, true));
  for (u32 i = 0; i < 4u; i++) {
    AppendStoreVgpr(&code, i, i);
  }
  AppendEnd(&code);

  auto image = MakeRgbaImage(4, 4);
  SetRgbaPixel(&image, 4, 2, 1, 0x3f800000u, 0x40000000u, 0x40400000u,
               0x40800000u);

  TestCase test;
  test.name = "ImageSampleA16OffsetKeepsTexelOffset32BitOnGpu";
  test.code = code;
  test.expected = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
  test.opcodes = {O::VMovB32, O::ImageSample, O::BufferStoreDword, O::SEndpgm};
  test.sampled_image_rgba = image;
  test.required_spirv = {"UnpackHalf2x16", "OpBitFieldSExtract"};
  return test;
}

TestCase ImageSampleA16CompareBiasRdna2AddressOrder() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 20, 0x00003800u); // bias=0.5 as low f16.
  AppendVMovLiteral(&code, 21, 0x3f000000u); // PCF reference stays 32-bit.
  AppendVMovLiteral(&code, 22, 0x36003900u); // x=0.625, y=0.375 packed as f16.
  code.push_back(EncodeMimg0(0x2d, 0x1));
  code.push_back(EncodeMimg1(0, 20, 0, 0, true));
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageSampleA16CompareBiasRdna2AddressOrder";
  test.code = code;
  test.opcodes = {O::VMovB32, O::ImageSample, O::SEndpgm};
  test.required_spirv = {"OpImageSampleDrefExplicitLod", "UnpackHalf2x16"};
  test.compile_only = true;
  return test;
}

TestCase ImageGatherCompareOpcodes() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovLiteral(&code, 21, 0x3f000000u);
  AppendVMovLiteral(&code, 22, 0x3f200000u);
  AppendVMovLiteral(&code, 23, 0x3ec00000u);
  AppendVMovLiteral(&code, 24, 0x3f000000u);
  AppendVMovLiteral(&code, 25, 0x3f200000u);
  AppendVMovLiteral(&code, 26, 0x3ec00000u);
  AppendVMovLiteral(&code, 28, 0x3f000000u);
  AppendVMovLiteral(&code, 29, 0x3f200000u);
  AppendVMovLiteral(&code, 30, 0x3ec00000u);
  AppendVMovU32(&code, 32, 0);
  AppendVMovLiteral(&code, 33, 0x3f000000u);
  AppendVMovLiteral(&code, 34, 0x3f200000u);
  AppendVMovLiteral(&code, 35, 0x3ec00000u);
  code.push_back(EncodeMimg0(0x48, 0x1));
  code.push_back(EncodeMimg1(4, 24));
  code.push_back(EncodeMimg0(0x4f, 0x1));
  code.push_back(EncodeMimg1(8, 28));
  code.push_back(EncodeMimg0(0x58, 0x1));
  code.push_back(EncodeMimg1(0, 20));
  code.push_back(EncodeMimg0(0x5f, 0x1));
  code.push_back(EncodeMimg1(12, 32));
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageGatherCompareOpcodes";
  test.code = code;
  test.opcodes = {O::VMovB32,        O::ImageGather4C,    O::ImageGather4CLz,
                  O::ImageGather4CO, O::ImageGather4CLzO, O::SEndpgm};
  test.required_spirv = {"OpImageDrefGather", "OpBitFieldSExtract"};
  test.compile_only = true;
  return test;
}

TestCase ImageStoreVariants() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 21, 2);
  AppendVMovU32(&code, 22, 0);
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0x40000000u);
  AppendVMovLiteral(&code, 2, 0x40400000u);
  AppendVMovLiteral(&code, 3, 0x40800000u);
  code.push_back(EncodeMimg0(0x08, 0xf));
  code.push_back(EncodeMimg1(0, 20));
  AppendVMovU32(&code, 24, 3);
  AppendVMovU32(&code, 25, 2);
  AppendVMovU32(&code, 26, 0);
  AppendVMovU32(&code, 27, 0);
  AppendVMovLiteral(&code, 4, 0x40a00000u);
  AppendVMovLiteral(&code, 5, 0x40c00000u);
  AppendVMovLiteral(&code, 6, 0x40e00000u);
  AppendVMovLiteral(&code, 7, 0x41000000u);
  code.push_back(EncodeMimg0(0x09, 0xf));
  code.push_back(EncodeMimg1(4, 24));
  AppendVMovLiteral(&code, 8, 0x12345678u);
  AppendStoreVgpr(&code, 8, 0);
  AppendEnd(&code);

  auto expected_image = MakeRgbaImage(4, 4);
  SetRgbaPixel(&expected_image, 4, 1, 2, 0x3f800000u, 0x40000000u, 0x40400000u,
               0x40800000u);
  SetRgbaPixel(&expected_image, 4, 3, 2, 0x40a00000u, 0x40c00000u, 0x40e00000u,
               0x41000000u);

  TestCase test;
  test.name = "ImageStoreVariants";
  test.code = code;
  test.expected = {0x12345678u};
  test.opcodes = {O::VMovB32, O::ImageStore, O::ImageStoreMip,
                  O::BufferStoreDword, O::SEndpgm};
  test.storage_image_rgba = MakeRgbaImage(4, 4);
  test.storage_image_r32ui = std::vector<u32>(16, 0);
  test.expected_storage_image_rgba = expected_image;
  return test;
}

TestCase ImageStoreRgbOneUsesInverseSwizzle() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 21, 2);
  AppendVMovU32(&code, 22, 0);
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0x3f000000u);
  AppendVMovLiteral(&code, 2, 0x3e800000u);
  AppendVMovLiteral(&code, 3, 0x3f400000u);
  code.push_back(EncodeMimg0(0x08, 0xf));
  code.push_back(EncodeMimg1(0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageStoreRgbOneUsesInverseSwizzle";
  test.code = code;
  test.opcodes = {O::VMovB32, O::ImageStore, O::SEndpgm};
  test.has_user_data = true;
  test.user_data[0] = 0x1000u;
  test.user_data[1] =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm) << 20u;
  test.image_descriptor_swizzle = DstSel(4, 5, 6, 1);
  test.storage_image_format = VK_FORMAT_R8G8B8A8_UNORM;
  test.storage_image_dwords_per_pixel = 1;
  test.storage_image_rgba = std::vector<u32>(16, 0);
  test.expected_storage_image_rgba = std::vector<u32>(16, 0);
  test.expected_storage_image_rgba[2 * 4 + 1] = 0x004080ffu;
  return test;
}

TestCase ImageStoreBgraUsesInverseSwizzle() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 21, 2);
  AppendVMovU32(&code, 22, 0);
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0x3f000000u);
  AppendVMovLiteral(&code, 2, 0x3e800000u);
  AppendVMovLiteral(&code, 3, 0x3f400000u);
  code.push_back(EncodeMimg0(0x08, 0xf));
  code.push_back(EncodeMimg1(0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageStoreBgraUsesInverseSwizzle";
  test.code = code;
  test.opcodes = {O::VMovB32, O::ImageStore, O::SEndpgm};
  test.has_user_data = true;
  test.user_data[0] = 0x1000u;
  test.user_data[1] =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm) << 20u;
  test.image_descriptor_swizzle = DstSel(6, 5, 4, 7);
  test.storage_image_format = VK_FORMAT_R8G8B8A8_UNORM;
  test.storage_image_dwords_per_pixel = 1;
  test.storage_image_rgba = std::vector<u32>(16, 0);
  test.expected_storage_image_rgba = std::vector<u32>(16, 0);
  test.expected_storage_image_rgba[2 * 4 + 1] = 0xbfff8040u;
  return test;
}

TestCase ImageStoreYzwxUsesInverseSwizzle() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 1);
  AppendVMovU32(&code, 21, 2);
  AppendVMovU32(&code, 22, 0);
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  AppendVMovLiteral(&code, 1, 0x3f000000u);
  AppendVMovLiteral(&code, 2, 0x3e800000u);
  AppendVMovLiteral(&code, 3, 0x3f400000u);
  code.push_back(EncodeMimg0(0x08, 0xf));
  code.push_back(EncodeMimg1(0, 20));
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageStoreYzwxUsesInverseSwizzle";
  test.code = code;
  test.opcodes = {O::VMovB32, O::ImageStore, O::SEndpgm};
  test.user_data = MakeStorageTextureData(Prospero::BufferFormat::k32_32_32_32Float);
  test.has_user_data = true;
  test.image_descriptor_swizzle = DstSel(5, 6, 7, 4);
  test.storage_image_rgba = MakeRgbaImage(4, 4);
  test.expected_storage_image_rgba = MakeRgbaImage(4, 4);
  SetRgbaPixel(&test.expected_storage_image_rgba, 4, 1, 2, 0x3f400000u,
               0x3f800000u, 0x3f000000u, 0x3e800000u);
  return test;
}

TestCase ImageStoreR32FloatUsesFormatlessStorageImage() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 2);
  AppendVMovU32(&code, 21, 1);
  AppendVMovU32(&code, 22, 0);
  AppendVMovLiteral(&code, 0, 0x3f800000u);
  code.push_back(EncodeMimg0(0x08, 0x1));
  code.push_back(EncodeMimg1(0, 20));
  AppendEnd(&code);

  std::vector<u32> expected_image(16, 0);
  expected_image[1 * 4 + 2] = 0x3f800000u;

  TestCase test;
  test.name = "ImageStoreR32FloatUsesFormatlessStorageImage";
  test.code = code;
  test.opcodes = {O::VMovB32, O::ImageStore, O::SEndpgm};
  test.storage_image_format = VK_FORMAT_R32_SFLOAT;
  test.storage_image_dwords_per_pixel = 1;
  test.storage_image_rgba = std::vector<u32>(16, 0);
  test.expected_storage_image_rgba = expected_image;
  test.required_spirv = {"OpCapability StorageImageReadWithoutFormat",
                         "OpCapability StorageImageWriteWithoutFormat"};
  test.forbidden_spirv = {"Rgba32f"};
  return test;
}

TestCase ImageStoreR32UintUsesUintStorageImage() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 2);
  AppendVMovU32(&code, 21, 1);
  AppendVMovU32(&code, 22, 0);
  AppendVMovLiteral(&code, 0, 0x12345678u);
  code.push_back(EncodeMimg0(0x08, 0x1));
  code.push_back(EncodeMimg1(0, 20));
  AppendEnd(&code);

  std::vector<u32> expected_image(16, 0);
  expected_image[1 * 4 + 2] = 0x12345678u;

  TestCase test;
  test.name = "ImageStoreR32UintUsesUintStorageImage";
  test.code = code;
  test.opcodes = {O::VMovB32, O::ImageStore, O::SEndpgm};
  test.user_data = MakeStorageTextureData(Prospero::BufferFormat::k32UInt);
  test.has_user_data = true;
  test.storage_image_rgba = MakeRgbaImage(4, 4);
  test.storage_image_r32ui = std::vector<u32>(16, 0);
  test.expected_storage_image_r32ui = expected_image;
  test.required_spirv = {"R32ui", "textures2D_L_U"};
  return test;
}

TestCase ComputeTgSizeSgprUsesWaveMetadata() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendStoreSgpr(&code, 2, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "ComputeTgSizeSgprUsesWaveMetadata";
  test.code = code;
  test.opcodes = {O::VMovB32, O::BufferStoreDword, O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 4;
  test.compute_info.threads_num[2] = 16;
  test.compute_info.group_id[0] = true;
  test.compute_info.group_id[1] = true;
  test.compute_info.wave_size = 32;
  test.compute_info.thread_ids_num = 3;
  test.compute_info.workgroup_register = 0;
  test.compute_info.tg_size_en = true;
  test.has_compute_info = true;
  test.compile_only = true;
  test.required_spirv = {"OpUDiv", "OpShiftLeftLogical", "2147483648"};
  return test;
}

TestCase ImageAtomicVariants() {
  using O = ShaderOpcode;

  const u32 initial[] = {10, 10, 0xf0f0u, 0xf000u, 0xf00fu};
  const u32 values[] = {5, 5, 0x0ff0u, 0x0f00u, 0x00ffu};
  const u32 ops[] = {0x11, 0x15, 0x18, 0x19, 0x1a};

  std::vector<u32> code;
  for (u32 i = 0; i < static_cast<u32>(std::size(values)); i++) {
    AppendVMovU32(&code, 20, i & 3u);
    AppendVMovU32(&code, 21, i >> 2u);
    AppendVMovU32(&code, 22, 0);
    AppendVMovLiteral(&code, 0, values[i]);
    code.push_back(EncodeMimg0(ops[i], 0x1, 0, true));
    code.push_back(EncodeMimg1(0, 20));
    AppendStoreVgpr(&code, 0, i);
  }
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageAtomicVariants";
  test.code = code;
  test.expected = {10, 10, 0xf0f0u, 0xf000u, 0xf00fu};
  test.opcodes = {O::VMovB32,          O::ImageAtomicAdd, O::ImageAtomicUMin,
                  O::ImageAtomicAnd,   O::ImageAtomicOr,  O::ImageAtomicXor,
                  O::BufferStoreDword, O::SEndpgm};
  test.storage_image_rgba = MakeRgbaImage(4, 4);
  test.storage_image_r32ui = std::vector<u32>(16, 0);
  for (u32 i = 0; i < static_cast<u32>(std::size(initial)); i++) {
    test.storage_image_r32ui[i] = initial[i];
  }
  test.expected_storage_image_r32ui = std::vector<u32>(16, 0);
  test.expected_storage_image_r32ui[0] = 15;
  test.expected_storage_image_r32ui[1] = 5;
  test.expected_storage_image_r32ui[2] = 0x00f0u;
  test.expected_storage_image_r32ui[3] = 0xff00u;
  test.expected_storage_image_r32ui[4] = 0xf0f0u;
  return test;
}

TestCase ImageAtomicGlc0DoesNotReturnOldValue() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovU32(&code, 20, 0);
  AppendVMovU32(&code, 21, 0);
  AppendVMovU32(&code, 22, 0);
  AppendVMovU32(&code, 0, 5);
  code.push_back(EncodeMimg0(0x11, 0x1));
  code.push_back(EncodeMimg1(0, 20));
  AppendStoreVgpr(&code, 0, 0);
  AppendEnd(&code);

  TestCase test;
  test.name = "ImageAtomicGlc0DoesNotReturnOldValue";
  test.code = code;
  test.expected = {5};
  test.opcodes = {O::VMovB32, O::ImageAtomicAdd, O::BufferStoreDword,
                  O::SEndpgm};
  test.storage_image_rgba = MakeRgbaImage(4, 4);
  test.storage_image_r32ui = std::vector<u32>(16, 0);
  test.storage_image_r32ui[0] = 10;
  test.expected_storage_image_r32ui = std::vector<u32>(16, 0);
  test.expected_storage_image_r32ui[0] = 15;
  return test;
}

GraphicsCase GraphicsInterpolationExport() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeVintrp(0x00, 0, 0, 0, 0));
  code.push_back(EncodeVintrp(0x01, 0, 0, 0, 0));
  code.push_back(EncodeVintrp(0x02, 1, 0, 1, 2));
  AppendVMovLiteral(&code, 2, 0x3f400000u);
  AppendVMovLiteral(&code, 3, 0x3f800000u);
  code.push_back(EncodeExp0(0x00, 0xf));
  code.push_back(EncodeExp1(0, 1, 2, 3));
  AppendEnd(&code);

  return {"GraphicsInterpolationExport",
          code,
          {0x3e800000u, 0x3f000000u, 0x3f400000u, 0x3f800000u},
          {O::VInterpP1F32, O::VInterpP2F32, O::VInterpMovF32, O::VMovB32,
           O::Exp, O::SEndpgm}};
}

GraphicsCase GraphicsFlatInterpolatorExport() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeVintrp(0x02, 0, 0, 0, 2));
  AppendVMovLiteral(&code, 1, 0x00000000u);
  AppendVMovLiteral(&code, 2, 0x00000000u);
  AppendVMovLiteral(&code, 3, 0x3f800000u);
  code.push_back(EncodeExp0(0x00, 0xf));
  code.push_back(EncodeExp1(0, 1, 2, 3));
  AppendEnd(&code);

  GraphicsCase test;
  test.name = "GraphicsFlatInterpolatorExport";
  test.fragment_code = code;
  test.expected_pixel = {0x3e800000u, 0x00000000u, 0x00000000u, 0x3f800000u};
  test.opcodes = {O::VInterpMovF32, O::VMovB32, O::Exp, O::SEndpgm};
  test.pixel_interpolator_settings = {0x00000400u};
  test.vertices = {
      0xbf800000u, 0xbf800000u, 0x3e800000u, 0x00000000u, 0x00000000u,
      0x3f800000u, 0x40400000u, 0xbf800000u, 0x3f400000u, 0x00000000u,
      0x00000000u, 0x3f800000u, 0xbf800000u, 0x40400000u, 0x3e800000u,
      0x00000000u, 0x00000000u, 0x3f800000u,
  };
  return test;
}

GraphicsCase GraphicsDsAddtidScratchExport() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSMovB32(124, InlineU32(0)));
  AppendVMovLiteral(&code, 0, 0x3f000000u);
  code.push_back(EncodeDs0(0xb0, 0));
  code.push_back(EncodeDs1(0, 0, 0));
  code.push_back(EncodeDs0(0xb1, 0));
  code.push_back(EncodeDs1(4, 0, 0));
  code.push_back(EncodeExp0(0x00, 0xf));
  code.push_back(EncodeExp1(4, 4, 4, 4));
  AppendEnd(&code);

  return {"GraphicsDsAddtidScratchExport",
          code,
          {0x3f000000u, 0x3f000000u, 0x3f000000u, 0x3f000000u},
          {O::SMovB32, O::VMovB32, O::DsWriteAddtidB32, O::DsReadAddtidB32,
           O::Exp, O::SEndpgm}};
}

GraphicsCase GraphicsDirectSgprPushConstantExport() {
  using O = ShaderOpcode;

  const std::vector<u32> values = {0x3e800000u, 0x3f000000u, 0x3f400000u,
                                   0x3f800000u};

  std::vector<u32> code;
  code.push_back(EncodeVop1(0x01, 0, 0));
  code.push_back(EncodeVop1(0x01, 1, 1));
  code.push_back(EncodeVop1(0x01, 2, 2));
  code.push_back(EncodeVop1(0x01, 3, 3));
  code.push_back(EncodeExp0(0x00, 0xf));
  code.push_back(EncodeExp1(0, 1, 2, 3));
  AppendEnd(&code);

  GraphicsCase test;
  test.name = "GraphicsDirectSgprPushConstantExport";
  test.fragment_code = code;
  test.expected_pixel = values;
  test.opcodes = {O::VMovB32, O::Exp, O::SEndpgm};

  for (size_t i = 0; i < values.size(); i++) {
    test.user_data[i] = values[i];
  }
  test.has_user_data = true;
  test.push_constants = values;
  return test;
}

GraphicsCase GraphicsInlineSrtScalarPromotionExport() {
  using O = ShaderOpcode;

  const std::vector<u32> values = {0x00000000u, 0x00000000u, 0x00000000u,
                                   0x3f800000u};

  std::vector<u32> code;
  AppendVop3(&code, 0x12fu, 0, 0, 1);
  AppendVop3(&code, 0x12fu, 1, 2, 3);
  code.push_back(EncodeExp0(0x00, 0xf, true, true, true));
  code.push_back(EncodeExp1(0, 1, 0, 0));
  AppendEnd(&code);

  GraphicsCase test;
  test.name = "GraphicsInlineSrtScalarPromotionExport";
  test.fragment_code = code;
  test.expected_pixel = values;
  test.opcodes = {O::VCvtPkrtzF16F32, O::Exp, O::SEndpgm};

  for (size_t i = 0; i < values.size(); i++) {
    test.user_data[i] = values[i];
  }
  test.has_user_data = true;
  test.push_constants = values;
  return test;
}

GraphicsCase GraphicsNullVmExportDiscardsInactiveExec() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f000000u);
  AppendVMovLiteral(&code, 1, 0x3f400000u);
  AppendVMovLiteral(&code, 2, 0x3f800000u);
  AppendVMovLiteral(&code, 3, 0x40000000u);
  code.push_back(EncodeExp0(0x00, 0xf, false));
  code.push_back(EncodeExp1(0, 1, 2, 3));
  code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
  code.push_back(EncodeExp0(0x09, 0x0, true, false, true));
  code.push_back(EncodeExp1(0, 0, 0, 0));
  AppendEnd(&code);

  return {"GraphicsNullVmExportDiscardsInactiveExec",
          code,
          {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
          {O::VMovB32, O::Exp, O::SMovB64, O::SEndpgm}};
}

GraphicsCase GraphicsMrt0OffVmExportDiscardsInactiveExec() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f000000u);
  AppendVMovLiteral(&code, 1, 0x3f400000u);
  AppendVMovLiteral(&code, 2, 0x3f800000u);
  AppendVMovLiteral(&code, 3, 0x40000000u);
  code.push_back(EncodeExp0(0x00, 0xf, false));
  code.push_back(EncodeExp1(0, 1, 2, 3));
  code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
  code.push_back(EncodeExp0(0x00, 0x0, true, true, true));
  code.push_back(EncodeExp1(0, 0, 0, 0));
  AppendEnd(&code);

  return {"GraphicsMrt0OffVmExportDiscardsInactiveExec",
          code,
          {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
          {O::VMovB32, O::Exp, O::SMovB64, O::SEndpgm}};
}

GraphicsCase GraphicsFinalVmExportSupersedesEarlierVmMask() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f000000u);
  AppendVMovLiteral(&code, 1, 0x3f400000u);
  AppendVMovLiteral(&code, 2, 0x3f800000u);
  AppendVMovLiteral(&code, 3, 0x40000000u);
  code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
  code.push_back(EncodeExp0(0x00, 0xf, false, false, true));
  code.push_back(EncodeExp1(0, 1, 2, 3));
  code.push_back(EncodeSMovB32(126, 193u));
  code.push_back(EncodeSMovB32(127, 193u));
  AppendVMovLiteral(&code, 4, 0x40400000u);
  AppendVMovLiteral(&code, 5, 0x40800000u);
  AppendVMovLiteral(&code, 6, 0x40a00000u);
  AppendVMovLiteral(&code, 7, 0x40c00000u);
  code.push_back(EncodeExp0(0x00, 0xf, true, false, true));
  code.push_back(EncodeExp1(4, 5, 6, 7));
  AppendEnd(&code);

  return {"GraphicsFinalVmExportSupersedesEarlierVmMask",
          code,
          {0x40400000u, 0x40800000u, 0x40a00000u, 0x40c00000u},
          {O::VMovB32, O::SMovB64, O::Exp, O::SMovB32, O::SEndpgm}};
}

GraphicsCase GraphicsBranchPathFinalVmExportDiscardsInactiveExec() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  AppendVMovLiteral(&code, 0, 0x3f000000u);
  AppendVMovLiteral(&code, 1, 0x3f400000u);
  AppendVMovLiteral(&code, 2, 0x3f800000u);
  AppendVMovLiteral(&code, 3, 0x40000000u);
  code.push_back(EncodeExp0(0x00, 0xf, false));
  code.push_back(EncodeExp1(0, 1, 2, 3));
  code.push_back(EncodeSop1(0x04, 126, InlineU32(0)));
  code.push_back(EncodeExp0(0x09, 0x0, true, false, true));
  code.push_back(EncodeExp1(0, 0, 0, 0));
  code.push_back(EncodeSopc(0x06, InlineU32(0), InlineU32(1)));
  const auto branch_index = code.size();
  code.push_back(0);
  AppendEnd(&code);
  const auto later_export_index = code.size();
  code[branch_index] = EncodeSopp(
      0x05, static_cast<u32>(later_export_index - branch_index - 1u));
  code.push_back(EncodeSMovB32(126, 193u));
  code.push_back(EncodeSMovB32(127, 193u));
  AppendVMovLiteral(&code, 4, 0x40400000u);
  AppendVMovLiteral(&code, 5, 0x40800000u);
  AppendVMovLiteral(&code, 6, 0x40a00000u);
  AppendVMovLiteral(&code, 7, 0x40c00000u);
  code.push_back(EncodeExp0(0x00, 0xf, true, false, true));
  code.push_back(EncodeExp1(4, 5, 6, 7));
  AppendEnd(&code);

  return {"GraphicsBranchPathFinalVmExportDiscardsInactiveExec",
          code,
          {0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
          {O::VMovB32, O::Exp, O::SMovB64, O::SCmpEqU32, O::SCbranchScc1,
           O::SMovB32, O::SEndpgm}};
}

TestCase MultipleWorkitemsGlobalId() {
  using O = ShaderOpcode;

  std::vector<u32> code = {
      EncodeVop1(0x01, 1, 0),
      EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1),
      EncodeVop2(0x25, 2, InlineU32(64), 1),
      EncodeVop2(0x1a, 3, InlineU32(2), 1),
  };
  AppendBufferStoreDword(&code, 2, 3);
  AppendEnd(&code);

  TestCase test;
  test.name = "MultipleWorkitemsGlobalId";
  test.code = code;
  test.expected = {64, 65, 66, 67, 68, 69, 70, 71};
  test.opcodes = {O::VMovB32, O::VLshlrevB32, O::VAddNcU32, O::BufferStoreDword,
                  O::SEndpgm};
  test.compute_info.threads_num[0] = 4;
  test.compute_info.threads_num[1] = 1;
  test.compute_info.threads_num[2] = 1;
  test.compute_info.group_id[0] = true;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.workgroup_register = 0;
  test.has_compute_info = true;
  test.dispatch_x = 2;
  return test;
}

TestCase DispatcherIrreducibleControlFlow() {
  using O = ShaderOpcode;

  std::vector<u32> code;
  code.push_back(EncodeSopp(0x05, 5)); // entry -> B, fallthrough A
  AppendVMovU32(&code, 0, 7);
  AppendStoreVgpr(&code, 0, 0);
  code.push_back(EncodeSopp(0x04, 1)); // A exits while SCC is initially zero
  code.push_back(EncodeSopp(0x02, 0xfffau)); // B -> A
  AppendEnd(&code);

  TestCase test;
  test.name = "DispatcherIrreducibleControlFlow";
  test.code = code;
  test.expected = {7};
  test.opcodes = {O::SCbranchScc1, O::VMovB32, O::BufferStoreDword,
                  O::SCbranchScc0, O::SBranch, O::SEndpgm};
  return test;
}

std::vector<TestCase> MakeCases() {
  std::vector<TestCase> cases;
  cases.reserve(128);
  auto AddCase = [&cases](TestCase (*factory)()) {
    cases.push_back(factory());
  };

  AddCase(IntegerAddSubMul);
  AddCase(BitwiseOps);
  AddCase(Shifts);
  AddCase(ScalarShiftCountsMaskLowBits);
  AddCase(Rdna2ScalarOpcodes);
  AddCase(ScalarExtendedArithmetic);
  AddCase(ScalarArithmeticSccCarryBorrowOverflow);
  AddCase(ScalarMinMaxSccComparisonEdges);
  AddCase(ScalarAbsI32UpdatesScc);
  AddCase(ScalarShiftLeftAddSccCarryEdges);
  AddCase(ScalarCompareOps);
  AddCase(ScalarShiftAddAndMaskOps);
  AddCase(ScalarNotB64UpdatesScc);
  AddCase(ScalarFlbitI32B64Gpu);
  AddCase(ScalarSaveExecOps);
  AddCase(ScalarOrn2SaveexecUsesSourceOrNotExec);
  AddCase(ScalarGetpcWritesNextInstructionPc);
  AddCase(ScalarBitfieldPack);
  AddCase(ScalarBrevB32PreservesScc);
  AddCase(BitfieldExtractWidthPastEndEdges);
  AddCase(Scalar64BitOps);
  AddCase(ScalarAndn2B64SccBranch);
  AddCase(ScalarLiteral);
  AddCase(VectorMoves);
  AddCase(VectorVop3MoveAppliesFloatSourceModifiers);
  AddCase(VectorIntegerOps);
  AddCase(VectorShiftCountsMaskLowBits);
  AddCase(VectorVop3IntegerOps);
  AddCase(VectorBfeI32ArithmeticShiftMasksField);
  AddCase(VectorCarryAndBitCountOps);
  AddCase(VectorMbcntUsesThreadMask);
  AddCase(VectorAddcWritesPerLaneCarryOut);
  AddCase(VectorAddcUsesPerLaneCarryIn);
  AddCase(VectorVop3BCarryOutWritesSgprMask);
  AddCase(VectorVop3BCarryOutUsesEncodedSdst);
  AddCase(VectorVop3BSubCoU32UsesRdna2Opcode310);
  AddCase(VectorMadU64U32UnsignedCarryOut);
  AddCase(VectorLaneAndPackedOps);
  AddCase(CvtPkU8F32PacksSelectedByte);
  AddCase(CvtPkrtzF16F32SubnormalRoundsTowardZero);
  AddCase(PackedMinMaxF16NanAndSignedZeroEdges);
  AddCase(VectorMinMaxF16Ops);
  AddCase(VectorCvtU16F16Sdwa);
  AddCase(VectorMinMaxMed3F16Ops);
  AddCase(VectorSpecialF16Ops);
  AddCase(VectorWritelaneIgnoresExecMask);
  AddCase(VectorPermlanex16);
  AddCase(VectorPermlane16FetchInactiveZero);
  AddCase(VectorPermlane16FetchInactiveFi);
  AddCase(VectorDppQuadPermuteReverse);
  AddCase(VectorDppBankMaskPreservesDestination);
  AddCase(VectorDppBoundsControlZeroPreservesDestination);
  AddCase(Vop3LdexpSourceModifier);
  AddCase(Vop1MoveRelSource);
  AddCase(Vop1MoveRelDestination);
  AddCase(VectorFloatSpecialOps);
  AddCase(MadMixF16LiteralHalfSourceUsesOpsel);
  AddCase(MadMixF16NegHiIsAbsAndNegIsIndependent);
  AddCase(VectorVop3FmaF16UsesRdna2Opcode34b);
  AddCase(VectorFloatArithmeticOps);
  AddCase(VectorMinMaxF32NanAndSignedZeroEdges);
  AddCase(VectorMed3F32NanUsesMin3Path);
  AddCase(VectorFloatConversionOps);
  AddCase(CvtF32ToIntSaturatesNaNAndOutOfRange);
  AddCase(VectorSpecialF32FlushesDenormalInputs);
  AddCase(VectorSinCosMaxFiniteSpecialCases);
  AddCase(VectorCompareOps);
  AddCase(VectorVop3CompareNeU64OnGpu);
  AddCase(VectorCompareClassF32);
  AddCase(VectorCompareF16Ops);
  AddCase(Vop2SdwaCndmaskSourceModifier);
  AddCase(Vop3CndmaskUsesSgprMaskLaneBits);
  AddCase(Vop3CndmaskAllowsDataSourceModifier);
  AddCase(VectorCompareExecOps);
  AddCase(VectorVop3FloatCompareNegSourceModifier);
  AddCase(VectorVop3CmpxWritesExecMask);
  AddCase(VectorVopcSdwaCmpxWritesExecMask);
  AddCase(VectorCompareInvertedMaskSelect);
  AddCase(BranchSelect);
  AddCase(SimpleLoop);
  AddCase(BranchVccnzUsesWholeMask);
  AddCase(BranchVccnzUsesCarryProducedWholeMask);
  AddCase(ScalarMemoryLoadVariants);
  AddCase(ScalarLoadSignedImmediateOffsetAddsSoffset);
  AddCase(ScalarLoadAlignsComponentsAndMasksAddress);
  AddCase(BufferLoadStore);
  AddCase(BufferLoadDwordOffenIdxenUsesVaddrPlusOneOffset);
  AddCase(BufferStoreDwordOffenIdxenUsesVaddrPlusOneOffset);
  AddCase(BufferLoadDwordNoAddressFlagsIgnoresVaddr);
  AddCase(BufferLoadDwordIdxenUsesDescriptorStride);
  AddCase(BufferStoreDwordIdxenUsesDescriptorStride);
  AddCase(BufferLoadVariants);
  AddCase(BufferStoreVariants);
  AddCase(BufferFormatVariants);
  AddCase(BufferFormatStoreVariants);
  AddCase(BufferStoreFormatXResource16UintWritesHalfword);
  AddCase(BufferLoadFormatXResource8UintZeroExtendsByte);
  AddCase(BufferLoadFormatXyResource88UintExtractsBytes);
  AddCase(BufferLoadFormatXyResource8888UnormConvertsFirstTwoComponents);
  AddCase(BufferStoreFormatXyResource88UintWritesBytes);
  AddCase(BufferStoreFormatXyzResource3232UintWritesTwoDwords);
  AddCase(BufferStoreFormatXyzwResource323232UintWritesThreeDwords);
  AddCase(BufferStoreFormatXyResource32UintWritesOneDword);
  AddCase(BufferStoreFormatXyzResource8UintWritesOneByte);
  AddCase(BufferStoreFormatXAddTidUsesLaneIndex);
  AddCase(BufferStoreFormatXDropsOutOfRangeRecord);
  AddCase(TBufferLoadVariants);
  AddCase(TBufferLoadFormatX8UintZeroExtendsByte);
  AddCase(TBufferLoadFormatX8888UintExtractsFirstByte);
  AddCase(TBufferLoadFormatXIdxenUsesDescriptorStride);
  AddCase(TBufferLoadFormatX16FloatConvertsToFloat);
  AddCase(TBufferStoreFormatX8UintWritesOneByte);
  AddCase(TBufferStoreFormatXSintWritesSubDword);
  AddCase(TBufferLoadFormatXSintSignExtendsSubDword);
  AddCase(TBufferLoadFormatXy1616IntegerComponents);
  AddCase(TBufferStoreFormatXy1616IntegerComponents);
  AddCase(TBufferLoadFormatXyz16161616UintLoadsHalfwords);
  AddCase(TBufferLoadFormatXy1616UnormConvertsToFloat);
  AddCase(TBufferLoadFormatXy88UnormConvertsToFloat);
  AddCase(TBufferLoadFormatXy88SnormConvertsToFloat);
  AddCase(TBufferLoadFormatXy8888UnormConvertsFirstTwoComponents);
  AddCase(TBufferLoadFormatXyzw8888UintExtractsBytes);
  AddCase(TBufferLoadFormatXyzw1010102SnormConvertsToFloat);
  AddCase(TBufferLoadFormatXyz111110FloatUnpacks);
  AddCase(TBufferLoadFormatXyzw3232FloatZerosMissingComponents);
  AddCase(TBufferStoreFormatXyzw3232FloatWritesOnlyPresentComponents);
  AddCase(TBufferStoreFormatXy88IntegerComponents);
  AddCase(TBufferLoadFormatXy88IntegerComponents);
  AddCase(TBufferStoreVariants);
  AddCase(FlatLoadVariants);
  AddCase(FlatVirtualAddressRebasesGuestAllocation);
  AddCase(GlobalSignedImmediateRebasesBeforeSaddr);
  AddCase(FlatSegmentIgnoresSaddrAndMasksOffsetMsb);
  AddCase(FlatStoreVariants);
  AddCase(DsReadWriteVariants);
  AddCase(DsAppendConsumeUsesEncodedLdsSelector);
  AddCase(DsAppendUsesEncodedGdsSelector);
  AddCase(DsGdsSubdwordAndAtomicWrites);
  AddCase(DsReadWrite2Variants);
  AddCase(DsAtomicNoReturnVariants);
  AddCase(DsAtomicReturnVariants);
  AddCase(DsMiscVariants);
  AddCase(DsFloatMinMaxUsesSeparateCompareOperand);
  AddCase(DsSwizzleInvalidSourceLaneZero);
  AddCase(BufferAtomicVariants);
  AddCase(BufferAtomicGlc0DoesNotReturnOldValue);
  AddCase(ImageLoadVariants);
  AddCase(ImageLoadR32UintUsesIntegerSampledImage);
  AddCase(ImageLoadMipUsesVaddr2Lod2D);
  AddCase(ImageLoadMipNsaUsesSelectedAddressVgprs);
  AddCase(ImageLoadA16UintCoordsOnGpu);
  AddCase(ImageGetResinfoDmaskWidthHeight);
  AddCase(ImageGetResinfoDmaskMipLevels);
  AddCase(ImageSampleAndGather);
  AddCase(ImageSampleA16SamplerCoordsOnGpu);
  AddCase(ImageSampleOpcodeAliasUsesNormalCoords);
  AddCase(ImageSampleA16OffsetKeepsTexelOffset32BitOnGpu);
  AddCase(ImageSampleA16CompareBiasRdna2AddressOrder);
  AddCase(ImageGatherCompareOpcodes);
  AddCase(ImageStoreVariants);
  AddCase(ImageStoreRgbOneUsesInverseSwizzle);
  AddCase(ImageStoreBgraUsesInverseSwizzle);
  AddCase(ImageStoreYzwxUsesInverseSwizzle);
  AddCase(ImageStoreR32FloatUsesFormatlessStorageImage);
  AddCase(ImageStoreR32UintUsesUintStorageImage);
  AddCase(ComputeTgSizeSgprUsesWaveMetadata);
  AddCase(ImageAtomicVariants);
  AddCase(ImageAtomicGlc0DoesNotReturnOldValue);
  AddCase(MultipleWorkitemsGlobalId);
  AddCase(DispatcherIrreducibleControlFlow);

  return cases;
}

std::vector<GraphicsCase> MakeGraphicsCases() {
  return {
      GraphicsInterpolationExport(),
      GraphicsFlatInterpolatorExport(),
      GraphicsDsAddtidScratchExport(),
      GraphicsDirectSgprPushConstantExport(),
      GraphicsInlineSrtScalarPromotionExport(),
      GraphicsNullVmExportDiscardsInactiveExec(),
      GraphicsMrt0OffVmExportDiscardsInactiveExec(),
      GraphicsFinalVmExportSupersedesEarlierVmMask(),
      GraphicsBranchPathFinalVmExportDiscardsInactiveExec(),
  };
}

std::vector<SkippedCase> MakeSkippedCases() {
  return {ImageStoreMipWritesExplicitMip2D()};
}

void CheckPs5GameExampleImageClearRuntimeShape() {
  const auto MakeCode = [] {
    std::vector<u32> code;
    AppendVop3(&code, 0x347u, 4, 8, InlineU32(6), Vgpr(0));
    for (u32 i = 0; i < 4; i++) {
      code.push_back(EncodeVop1(0x01u, i, i + 4u));
    }
    code.push_back(EncodeMubuf0(0x07u, 0, true, false));
    code.push_back(EncodeMubuf1(0, 0, 4));
    AppendEnd(&code);
    return code;
  };
  std::array<u32, 8> user_data{};
  user_data[0] = 0x00010000u;
  user_data[1] = 16u << 16u;
  user_data[2] = 64;
  user_data[3] = (Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32UInt) << 12u) |
                 4u | (5u << 3u) | (6u << 6u) | (7u << 9u);
  std::fill(user_data.begin() + 4, user_data.end(), 0xff000000u);
  ShaderComputeInputInfo compute{};
  compute.threads_num[0] = 64;
  compute.threads_num[1] = 1;
  compute.threads_num[2] = 1;
  compute.dispatch_threads_num[0] = 64;
  compute.dispatch_threads_num[1] = 1;
  compute.dispatch_threads_num[2] = 1;
  compute.group_id[0] = true;
  compute.dispatch_thread_dimensions = true;
  compute.wave_size = 32;
  compute.thread_ids_num = 1;
  compute.workgroup_register = 8;

  const auto Compile = [&](const char *stage, const std::vector<u32> &code) {
    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Compute;
    options.wave_size = 32;
    options.user_data_base = 0;
    options.user_data_count = static_cast<u32>(user_data.size());
    options.user_data = user_data.data();
    options.compute_input_info = &compute;
    options.dump_ir = false;
    ShaderRecompiler::CompileResult result;
    std::string error;
    Require("Ps5GameExampleImageClear", stage,
            ShaderRecompiler::TryRecompile(code, options, &result, &error),
            error);
    ValidateSpirv("Ps5GameExampleImageClear", result.spirv);
    return result;
  };

  const auto code = MakeCode();
  auto positive = Compile("exact Prospero kernel", code);

  compute.stage.program =
      std::make_shared<const ShaderRecompiler::IR::Program>(positive.program);
  compute.stage.resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(positive.resources);
  ShaderBufferResource descriptor{};
  u32 packed_clear = 0;
  uint64_t size = 0;
  Require("Ps5GameExampleImageClear", "runtime shape",
          ResolveComputeImageClear(compute, 64, 1, 1, 0x61u, &descriptor,
                                   &packed_clear, &size) &&
              descriptor.Base48() == 0x10000u && size == 64u * 16u &&
              packed_clear == 0xff000000u,
          "exact Prospero runtime binding did not resolve to a complete clear");

  auto non_repeated = positive.resources;
  non_repeated.user_data[7] ^= 1u;
  compute.stage.resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(non_repeated);
  Require("Ps5GameExampleImageClear", "non-repeated clear",
          !ResolveComputeImageClear(compute, 64, 1, 1, 0x61u, &descriptor,
                                    &packed_clear, &size),
          "non-uniform uint4 data was replaced with a color clear");
  compute.stage.resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(positive.resources);
  compute.dispatch_threads_num[0] = 32;
  Require("Ps5GameExampleImageClear", "partial dispatch",
          !ResolveComputeImageClear(compute, 32, 1, 1, 0x61u, &descriptor,
                                    &packed_clear, &size),
          "partial buffer coverage was classified as a complete clear");
  std::printf("[host]    %-32s ok\n", "Ps5GameExampleImageClear");
}

void CheckEmbeddedFetchVertexOffset() {
  const auto MakeFetch = [](std::initializer_list<std::pair<u32, u32>> adds,
                            std::optional<std::pair<u32, u32>> late_add = {}) {
    std::vector<u32> code;
    code.push_back(EncodeSMovB32(0, InlineU32(0)));
    code.push_back(EncodeSmem0(0x02u, 20, 4));
    code.push_back(EncodeSmem1(0));
    code.push_back(EncodeVop2(0x01u, 0, Vgpr(8), 5));
    for (const auto [sgpr, index_vgpr] : adds) {
      AppendVop3B(&code, 0x30fu, 0, 0, sgpr, Vgpr(index_vgpr));
    }
    code.push_back(EncodeMubuf0(0x03u, 0, true));
    code.push_back(EncodeMubuf1(9, 5, 0));
    if (late_add.has_value()) {
      AppendVop3B(&code, 0x30fu, 0, 0, late_add->first,
                  Vgpr(late_add->second));
    }
    AppendEnd(&code);
    return code;
  };

  const auto Compile = [&](const char *name, const std::vector<u32> &code,
                           u32 slot10) {
    std::array<u32, 11> user_data{};
    user_data[10] = slot10;
    ShaderVertexInputInfo vertex;
    vertex.fetch_embedded = true;
    vertex.fetch_buffer_reg = 0;
    vertex.fetch_attrib_reg = 2;
    vertex.resources_num = 1;
    vertex.resources_dst[0].attr_id = 0;
    vertex.resources_dst[0].registers_num = 4;

    ShaderRecompiler::CompileOptions options;
    options.stage = ShaderType::Vertex;
    options.user_data_base = 8;
    options.user_data_count = static_cast<u32>(user_data.size());
    options.user_data = user_data.data();
    options.vertex_input_info = &vertex;

    ShaderRecompiler::CompileResult result;
    std::string error;
    Require(name, "compile",
            ShaderRecompiler::TryRecompile(code, options, &result, &error),
            error);
    Require(name, "fetch rewrite", vertex.resource_fetch_components[0] == 4,
            "encoded fetch sequence was not recognized and rewritten");
    return result;
  };

  const auto Resolve = [](const ShaderRecompiler::CompileResult &result,
                          u32 index_offset) {
    ShaderVertexInputInfo vertex;
    vertex.fetch_embedded = true;
    vertex.stage.program =
        std::make_shared<const ShaderRecompiler::IR::Program>(result.program);
    vertex.stage.resources =
        std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(result.resources);
    return ResolveVertexOffset(index_offset, vertex);
  };

  const auto valid = Compile("EmbeddedFetchVertexOffset",
                             MakeFetch({{18, 0}}), 7);
  Require("EmbeddedFetchVertexOffset", "parse",
          valid.program.info.vertex_offset_sgpr == 18 &&
              Resolve(valid, 0) == 7 && Resolve(valid, 5) == 5,
          "canonical fetch offset or register index-offset precedence is wrong");

  const auto pointer = 0x5b7c5100u;
  const auto late = Compile("EmbeddedFetchLateOffset",
                            MakeFetch({}, std::pair<u32, u32>{18, 0}), pointer);
  const auto conflict = Compile("EmbeddedFetchConflictingOffset",
                                MakeFetch({{17, 0}, {18, 0}}), pointer);
  const auto malformed = Compile("EmbeddedFetchMalformedOffset",
                                 MakeFetch({{18, 1}}), pointer);
  const auto outside = Compile("EmbeddedFetchOutsideOffset",
                               MakeFetch({{19, 0}}), pointer);
  for (const auto *result : {&late, &conflict, &malformed, &outside}) {
    Require("EmbeddedFetchVertexOffset", "fail closed",
            result->program.info.vertex_offset_sgpr == -1 &&
                Resolve(*result, 0) == 0,
            "non-prolog, conflicting, malformed, or out-of-window add was classified");
  }
  std::printf("[host]    %-32s ok\n", "EmbeddedFetchVertexOffset");
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
struct CacheFaultContext {
  TextureCache *texture = nullptr;
};

bool CacheFault(void *opaque, PageFaultAccess access, uint64_t vaddr,
                uint64_t size, PageFaultPhase phase) noexcept {
  auto *context = static_cast<CacheFaultContext *>(opaque);
  return context != nullptr && context->texture != nullptr &&
         context->texture->InvalidateMemory(access, vaddr, size, phase);
}

[[noreturn]] void RunImageViewDeathCase(const char *kind) {
  if (std::strcmp(kind, "sampled") == 0) {
    (void)SelectSampledColorView(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 0));
  } else if (std::strcmp(kind, "sampled-compatible-swizzle") == 0) {
    (void)SelectSampledColorView(VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-compatible-reverse") == 0) {
    (void)SelectSampledColorView(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_B8G8R8A8_UNORM,
                                 DstSel(6, 5, 4, 7));
  } else if (std::strcmp(kind, "sampled-compatible-class") == 0) {
    (void)SelectSampledColorView(VK_FORMAT_R8G8B8A8_UINT,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-compatible-colorspace") == 0) {
    (void)SelectSampledColorView(VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-compatible-snorm") == 0) {
    (void)SelectSampledColorView(VK_FORMAT_R8G8B8A8_SNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-depth-format") == 0) {
    (void)SelectSampledDepthView(VK_FORMAT_D32_SFLOAT_S8_UINT,
                                 VK_FORMAT_R16_UNORM, DstSel(4, 4, 4, 4));
  } else if (std::strcmp(kind, "sampled-depth-swizzle") == 0) {
    (void)SelectSampledDepthView(VK_FORMAT_D32_SFLOAT_S8_UINT,
                                 VK_FORMAT_R32_SFLOAT, DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "storage") == 0) {
    (void)SelectStorageColorView(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM, DstSel(4, 5, 6, 0));
  } else if (std::strcmp(kind, "storage-format") == 0) {
    (void)SelectStorageColorView(VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM, DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "storage-compatible-swizzle") == 0) {
    (void)SelectStorageColorView(VK_FORMAT_B8G8R8A8_SRGB,
                                 VK_FORMAT_R8G8B8A8_UNORM, DstSel(4, 5, 6, 7));
  } else {
    ShaderRecompiler::IR::ImageResource resource{};
    resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImage;
    resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
    resource.written = true;
    if (std::strcmp(kind, "storage-kind") == 0) {
      resource.kind = ShaderRecompiler::IR::ResourceKind::Image;
    } else if (std::strcmp(kind, "storage-no-write") == 0) {
      resource.written = false;
    } else if (std::strcmp(kind, "storage-atomic") == 0) {
      resource.atomic = true;
    } else if (std::strcmp(kind, "storage-compare") == 0) {
      resource.depth_compare = true;
    } else if (std::strcmp(kind, "storage-mip") == 0) {
      resource.mip_mode = ShaderRecompiler::IR::ImageMipMode::DynamicStorage;
    } else if (std::strcmp(kind, "storage-dimension") == 0) {
      resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Unknown;
    } else {
      std::_Exit(0x7e);
    }
    ValidateStorageImageResource(resource);
  }
  std::_Exit(0x7f);
}

void CheckSampledColorViews() {
  Require("SampledColorViews", "identity",
          SelectSampledColorView(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 7)) == VulkanImage::VIEW_DEFAULT,
          "RGBA did not select the identity view");
  Require("SampledColorViews", "R8 R001",
          SelectSampledColorView(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM,
                                 DstSel(4, 0, 0, 1)) == VulkanImage::VIEW_R001,
          "R8 did not select its R001 view");
  Require("SampledColorViews", "R16G16 RG01",
          SelectSampledColorView(VK_FORMAT_R16G16_SFLOAT,
                                 VK_FORMAT_R16G16_SFLOAT,
                                 DstSel(4, 5, 0, 1)) == VulkanImage::VIEW_RG01,
          "R16G16 did not select its RG01 view");
  Require("SampledColorViews", "alpha one",
          SelectSampledColorView(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 1)) == VulkanImage::VIEW_RGB1,
          "RGB1 did not select the alpha-one view");
  Require("SampledColorViews", "mutable BGRA target",
          SelectSampledColorView(VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(6, 5, 4, 7)) == VulkanImage::VIEW_RGBA8_BGRA,
          "BGRA target did not select the exact RGBA/BGRA mutable view");
  Require("SampledColorViews", "mutable sRGB view of UNORM BGRA target",
          SelectSampledColorView(VK_FORMAT_B8G8R8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_SRGB,
                                 DstSel(6, 5, 4, 7)) == VulkanImage::VIEW_RGBA8_BGRA,
          "UNORM BGRA target did not select its compatible sRGB RGBA sampled view");
  Require("SampledColorViews", "mutable sRGB BGRA target",
          BgraToRgbaSampledViewFormat(VK_FORMAT_B8G8R8A8_SRGB) ==
                  VK_FORMAT_R8G8B8A8_SRGB &&
              SelectSampledColorView(VK_FORMAT_B8G8R8A8_SRGB,
                                     VK_FORMAT_R8G8B8A8_SRGB,
                                     DstSel(6, 5, 4, 7)) ==
                  VulkanImage::VIEW_RGBA8_BGRA,
          "sRGB BGRA target did not select its matching mutable RGBA view");
  Require("SampledColorViews", "D32 depth target",
          SelectSampledDepthView(VK_FORMAT_D32_SFLOAT_S8_UINT,
                                 VK_FORMAT_R32_SFLOAT,
                                 DstSel(4, 4, 4, 4)) ==
              VulkanImage::VIEW_DEPTH_TEXTURE,
          "D32 depth target did not select its depth-aspect view");
  Require("SampledColorViews", "D16 R000 depth target",
          SelectSampledDepthView(VK_FORMAT_D16_UNORM, VK_FORMAT_R16_UNORM,
                                 DstSel(4, 0, 0, 0)) ==
              VulkanImage::VIEW_R000,
          "D16 depth target did not select its R000 depth-aspect view");
  Require("SampledColorViews", "storage identity",
          SelectStorageColorView(VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_FORMAT_R8G8B8A8_UNORM,
                                 DstSel(4, 5, 6, 7)) == VulkanImage::VIEW_STORAGE,
          "RGBA storage did not select the identity storage view");
  Require(
      "SampledColorViews", "storage RGB1 write mapping",
      SelectStorageColorView(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                             DstSel(4, 5, 6, 1)) == VulkanImage::VIEW_STORAGE,
      "RGBA8 RGB1 storage did not select the identity storage view");
  Require(
      "SampledColorViews", "storage BGRA write mapping",
      SelectStorageColorView(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                             DstSel(6, 5, 4, 7)) == VulkanImage::VIEW_STORAGE,
      "RGBA8 BGRA storage did not select the identity storage view");
  Require(
      "SampledColorViews", "mutable BGRA sRGB storage view",
      SelectStorageColorView(VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM,
                             DstSel(6, 5, 4, 7)) == VulkanImage::VIEW_STORAGE,
      "BGRA sRGB target did not select its RGBA UNORM identity storage view");
  Require("SampledColorViews", "storage YZWX write mapping",
          SelectStorageColorView(
              VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT,
              DstSel(5, 6, 7, 4)) == VulkanImage::VIEW_STORAGE,
          "RGBA32F YZWX storage did not select the identity storage view");
  Require("SampledColorViews", "storage R32 uint R001 write mapping",
          SelectStorageColorView(VK_FORMAT_R32_UINT, VK_FORMAT_R32_UINT,
                                 DstSel(4, 0, 0, 1)) ==
              VulkanImage::VIEW_STORAGE,
          "R32_UINT R001 storage did not select the identity storage view");
  Require("SampledColorViews", "storage R16 uint R000 write mapping",
          SelectStorageColorView(VK_FORMAT_R16_UINT, VK_FORMAT_R16_UINT,
                                 DstSel(4, 0, 0, 0)) == VulkanImage::VIEW_STORAGE,
          "R16_UINT R000 storage did not select the identity storage view");
  ShaderRecompiler::IR::ImageResource storage_resource{};
  storage_resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImage;
  storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  storage_resource.written = true;
  Require("SampledColorViews", "storage resource",
          IsSupportedStorageImageResource(storage_resource),
          "exact storage resource contract was rejected");
  storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim3D;
  storage_resource.read = true;
  Require("SampledColorViews", "read-write 3D storage resource",
          IsSupportedStorageImageResource(storage_resource),
          "basic read-write 3D storage resource was rejected");
  storage_resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
  storage_resource.read = false;
  Require("SampledColorViews", "write-only 2D-array storage resource",
          IsSupportedStorageImageResource(storage_resource),
          "basic write-only 2D-array storage resource was rejected");
  storage_resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
  Require("SampledColorViews", "write-only uint 2D-array storage resource",
          IsSupportedStorageImageResource(storage_resource),
          "basic write-only uint 2D-array storage resource was rejected");

  char path[MAX_PATH]{};
  Require("SampledColorViews", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind :
       {"sampled", "sampled-compatible-swizzle", "sampled-compatible-reverse",
        "sampled-compatible-class", "sampled-compatible-colorspace",
        "sampled-compatible-snorm", "sampled-depth-format",
        "sampled-depth-swizzle", "storage", "storage-format",
        "storage-compatible-swizzle", "storage-kind", "storage-no-write",
        "storage-atomic", "storage-compare", "storage-mip",
        "storage-dimension"}) {
    std::string command =
        std::string("\"") + path + "\" --image-view-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("SampledColorViews", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != 0,
            "CreateProcess failed");
    Require("SampledColorViews", "host",
            WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "unsupported view death case timed out");
    DWORD exit_code = 0;
    const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require("SampledColorViews", "host", exited && exit_code == 321,
            std::string(kind) + " component mapping did not report a fatal error");
  }
  std::printf("[host]    %-32s ok\n", "SampledColorRenderTargetViews");
}

void CheckSampledDepthResource() {
  ShaderRecompiler::IR::ImageResource resource{};
  resource.kind = ShaderRecompiler::IR::ResourceKind::Image;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = true;
  resource.depth_compare = true;
  Require("SampledDepthResource", "comparison read",
          IsSupportedSampledDepthResource(resource),
          "basic comparison-sampled depth resource was rejected");

  resource.depth_compare = false;
  Require("SampledDepthResource", "ordinary read",
          IsSupportedSampledDepthResource(resource),
          "basic non-comparison depth read was rejected");

  const auto basic = resource;
  resource.read = false;
  Require("SampledDepthResource", "read required",
          !IsSupportedSampledDepthResource(resource),
          "non-reading depth resource was accepted");
  resource = basic;
  resource.written = true;
  Require("SampledDepthResource", "write rejected",
          !IsSupportedSampledDepthResource(resource),
          "writable depth resource was accepted");
  resource = basic;
  resource.atomic = true;
  Require("SampledDepthResource", "atomic rejected",
          !IsSupportedSampledDepthResource(resource),
          "atomic depth resource was accepted");
  resource = basic;
  resource.mip_mode = ShaderRecompiler::IR::ImageMipMode::DynamicStorage;
  Require("SampledDepthResource", "dynamic mip rejected",
          !IsSupportedSampledDepthResource(resource),
          "dynamic-storage mip depth resource was accepted");
  resource = basic;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
  Require("SampledDepthResource", "array rejected",
          !IsSupportedSampledDepthResource(resource),
          "array depth resource was accepted");
  resource = basic;
  resource.kind = ShaderRecompiler::IR::ResourceKind::ImageUint;
  Require("SampledDepthResource", "integer rejected",
          !IsSupportedSampledDepthResource(resource),
          "integer depth resource was accepted");
  std::printf("[host]    %-32s ok\n", "SampledDepthResource");
}

void CheckSampledDepthDescriptor() {
  ShaderTextureResource descriptor{{0x00eb0900u, 0xc1600000u, 0x00bcc14fu,
                                    0x91800924u, 0x00000000u, 0x00700000u,
                                    0x00000000u, 0x00000000u}};
  DepthStencilVulkanImage image;
  image.extent = {1344, 756};
  image.guest_pitch = 1408;
  image.layers = 1;
  image.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  Require("SampledDepthDescriptor", "PPSA04319 padded pitch",
          descriptor.Width5() + 1u == 1344 && descriptor.Height5() + 1u == 756 &&
              TileGetTexturePitch(descriptor.Format(), 1344, 1,
                                  descriptor.TileMode()) == 1408 &&
              IsSupportedDepthTargetDescriptor(descriptor, image),
          "valid padded-pitch depth texture was rejected");

  image.guest_pitch = 1344;
  Require("SampledDepthDescriptor", "target pitch mismatch",
          !IsSupportedDepthTargetDescriptor(descriptor, image),
          "mismatched unpadded target pitch was accepted");
  image.guest_pitch = 1472;
  Require("SampledDepthDescriptor", "different padded pitch",
          !IsSupportedDepthTargetDescriptor(descriptor, image),
          "different padded target pitch was accepted");
  image.guest_pitch = 1408;
  descriptor.fields[3] =
      (descriptor.fields[3] & ~(0x1fu << 20u)) |
      (Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB) << 20u);
  Require("SampledDepthDescriptor", "non-depth tile",
          !IsSupportedDepthTargetDescriptor(descriptor, image),
          "non-depth texture descriptor was accepted");
  std::printf("[host]    %-32s ok\n", "SampledDepthDescriptor");
}

void CheckBufferCacheRangeMerge() {
  BufferCacheRange merged{0x10000, 0x4000};
  Require("BufferCacheRangeMerge", "disjoint",
          !MergeOverlappingBufferCacheRange(&merged, {0x14000, 0x4000}) &&
              merged.address == 0x10000 && merged.size == 0x4000,
          "adjacent range was merged");
  Require("BufferCacheRangeMerge", "right growth",
          MergeOverlappingBufferCacheRange(&merged, {0x12000, 0x8000}) &&
              merged.address == 0x10000 && merged.size == 0xa000,
          "right overlap did not grow the cache range");
  Require("BufferCacheRangeMerge", "left growth",
          MergeOverlappingBufferCacheRange(&merged, {0xc000, 0x8000}) &&
              merged.address == 0xc000 && merged.size == 0xe000,
          "left overlap did not grow the cache range");
  Require("BufferCacheRangeMerge", "contained",
          MergeOverlappingBufferCacheRange(&merged, {0x10000, 0x1000}) &&
              merged.address == 0xc000 && merged.size == 0xe000,
          "contained range changed the cache union");
  Require("BufferCacheRangeMerge", "queue ownership",
          CanMergeBufferCacheQueueMask(0, 3) &&
              CanMergeBufferCacheQueueMask(uint64_t{1} << 3u, 3) &&
              !CanMergeBufferCacheQueueMask((uint64_t{1} << 2u) |
                                                (uint64_t{1} << 3u),
                                            3) &&
              !CanMergeBufferCacheQueueMask(0, 64),
          "cross-queue or invalid queue ownership was accepted");
  Require("BufferCacheRangeMerge", "readback queue ownership",
          !CanReadbackBufferCacheQueueMask(0, 3) &&
              CanReadbackBufferCacheQueueMask(uint64_t{1} << 3u, 3) &&
              !CanReadbackBufferCacheQueueMask((uint64_t{1} << 2u) |
                                                   (uint64_t{1} << 3u),
                                               3) &&
              !CanReadbackBufferCacheQueueMask(uint64_t{1} << 3u, 64),
          "zero, cross-queue, or invalid readback ownership was accepted");
  std::printf("[host]    %-32s ok\n", "BufferCacheRangeMerge");
}

ShaderRecompiler::IR::ImageResource BasicStorageTextureResource() {
  ShaderRecompiler::IR::ImageResource resource{};
  resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImage;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim3D;
  resource.read = true;
  resource.written = true;
  return resource;
}

ShaderTextureResource BasicStorageTextureDescriptor() {
  return {{0x00785d00u, 0x04700000u, 0x00080008u, 0xa1b00facu,
           0x00000020u, 0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicLinearStorageTextureResource() {
  auto resource = BasicStorageTextureResource();
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = false;
  return resource;
}

ShaderTextureResource BasicLinearStorageTextureDescriptor() {
  return {{0x04bcc401u, 0xc3800000u, 0x021bc3bfu, 0x900003acu,
           0x00000000u, 0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicBgraStorageTextureResource() {
  auto resource = BasicStorageTextureResource();
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = false;
  return resource;
}

ShaderTextureResource BasicBgraStorageTextureDescriptor() {
  return {{0x007c6500u, 0xc3800000u, 0x010dc1dfu, 0x91b00f2eu,
           0x00000000u, 0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderTextureResource BasicYzwxStorageTextureDescriptor() {
  return {{0x00627801u, 0xc4d00000u, 0x0001c001u, 0x900009f5u,
           0x00000000u, 0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicArrayStorageTextureResource() {
  auto resource = BasicLinearStorageTextureResource();
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
  return resource;
}

ShaderTextureResource BasicArrayStorageTextureDescriptor() {
  return {{0x20179000u, 0x03800000u, 0x00000000u, 0xd1b00f2eu,
           0x00000000u, 0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicUintArrayStorageTextureResource() {
  auto resource = BasicArrayStorageTextureResource();
  resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
  return resource;
}

ShaderTextureResource BasicUintArrayStorageTextureDescriptor() {
  return {{0x20179200u, 0x01400000u, 0x00000000u, 0xd1b00204u,
           0x00000000u, 0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicUintVolumeStorageTextureResource() {
  auto resource = BasicStorageTextureResource();
  resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
  resource.read = false;
  return resource;
}

ShaderTextureResource BasicUintVolumeStorageTextureDescriptor() {
  return {{0x20180600u, 0xc0b00000u, 0x0003c003u, 0xa0000004u,
           0x0000000fu, 0x00700000u, 0x00000000u, 0x00000000u}};
}

[[noreturn]] void RunStorageTextureDescriptorDeathCase(const char *kind) {
  auto resource = BasicStorageTextureResource();
  auto descriptor = BasicStorageTextureDescriptor();
  if (std::strcmp(kind, "linear-rgb1-read") == 0) {
    resource = BasicLinearStorageTextureResource();
    descriptor = BasicLinearStorageTextureDescriptor();
    resource.read = true;
  } else if (std::strcmp(kind, "bgra-read") == 0) {
    resource = BasicBgraStorageTextureResource();
    descriptor = BasicBgraStorageTextureDescriptor();
    resource.read = true;
  } else if (std::strcmp(kind, "yzwx-read") == 0) {
    resource = BasicLinearStorageTextureResource();
    descriptor = BasicYzwxStorageTextureDescriptor();
    resource.read = true;
  } else if (std::strcmp(kind, "yzwx-format") == 0) {
    resource = BasicLinearStorageTextureResource();
    descriptor = BasicYzwxStorageTextureDescriptor();
    descriptor.fields[1] =
        (descriptor.fields[1] & ~0x1ff00000u) |
        (Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float) << 20u);
  } else if (std::strcmp(kind, "resource") == 0) {
    resource.written = false;
  } else if (std::strcmp(kind, "type") == 0) {
    descriptor.fields[3] =
        (descriptor.fields[3] & 0x0fffffffu) |
        (Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) << 28u);
  } else if (std::strcmp(kind, "tile") == 0) {
    descriptor.fields[3] =
        (descriptor.fields[3] & ~(0x1fu << 20u)) |
        (Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB) << 20u);
  } else if (std::strcmp(kind, "mip") == 0) {
    descriptor.fields[3] |= 1u << 16u;
  } else if (std::strcmp(kind, "swizzle") == 0) {
    descriptor.fields[3] =
        (descriptor.fields[3] & ~0xfffu) | DstSel(4, 5, 6, 1);
  } else if (std::strcmp(kind, "array-base-view") == 0) {
    resource = BasicArrayStorageTextureResource();
    descriptor = BasicArrayStorageTextureDescriptor();
    descriptor.fields[4] |= 1u << 16u;
  } else if (std::strcmp(kind, "reserved") == 0) {
    descriptor.fields[1] |= 1u << 29u;
  } else if (std::strcmp(kind, "metadata") == 0) {
    descriptor.fields[6] |= 1u << 21u;
  } else if (std::strcmp(kind, "uint-format") == 0) {
    descriptor.fields[1] =
        (descriptor.fields[1] & ~0x1ff00000u) |
        (Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UInt) << 20u);
  } else if (std::strcmp(kind, "uint-resource-float-format") == 0) {
    resource = BasicUintArrayStorageTextureResource();
    descriptor = BasicArrayStorageTextureDescriptor();
  } else {
    std::_Exit(0x7e);
  }
  ValidateStorageTexture(resource, descriptor, 0x10000);
  std::_Exit(0x7f);
}

void CheckBasicStorageTextureDescriptor() {
  const auto descriptor = BasicStorageTextureDescriptor();
  Require("BasicStorageTexture", "descriptor",
          descriptor.Base40() == 0x785d0000ull && descriptor.Width5() + 1u == 33 &&
              descriptor.Height5() + 1u == 33 && descriptor.Depth() + 1u == 33,
          "basic 3D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicStorageTextureResource(), descriptor, 0x10000);

  const auto linear = BasicLinearStorageTextureDescriptor();
  Require("BasicStorageTexture", "linear descriptor",
          linear.Base40() == 0x4bcc40100ull && linear.Width5() + 1u == 3840 &&
              linear.Height5() + 1u == 2160 && linear.Depth() + 1u == 1 &&
              linear.Format() == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm) &&
              linear.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kLinear),
          "PPSA07429 linear 2D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicLinearStorageTextureResource(), linear, 0x1fa4000);

  const auto bgra = BasicBgraStorageTextureDescriptor();
  Require("BasicStorageTexture", "BGRA descriptor",
          bgra.Base40() == 0x7c650000ull && bgra.Width5() + 1u == 1920 &&
              bgra.Height5() + 1u == 1080 && bgra.Depth() + 1u == 1 &&
              bgra.Format() == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm) &&
              bgra.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              bgra.DstSelXYZW() == DstSel(6, 5, 4, 7),
          "PPSA02604 BGRA 2D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicBgraStorageTextureResource(), bgra, 0x870000);

  const auto yzwx = BasicYzwxStorageTextureDescriptor();
  Require("BasicStorageTexture", "YZWX descriptor",
          yzwx.Base40() == 0x62780100ull && yzwx.Width5() + 1u == 8 &&
              yzwx.Height5() + 1u == 8 && yzwx.Depth() + 1u == 1 &&
              yzwx.Format() == Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float) &&
              yzwx.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
              yzwx.DstSelXYZW() == DstSel(5, 6, 7, 4),
          "PPSA04181 linear YZWX storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicLinearStorageTextureResource(), yzwx, 0x800);

  const auto array = BasicArrayStorageTextureDescriptor();
  Require("BasicStorageTexture", "2D-array descriptor",
          array.Base40() == 0x2017900000ull && array.Width5() + 1u == 1 &&
              array.Height5() + 1u == 1 && array.Depth() + 1u == 1 &&
              array.BaseArray5() == 0 &&
              array.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
              array.Format() == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm) &&
              array.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              array.DstSelXYZW() == DstSel(6, 5, 4, 7),
          "PPSA21268 2D-array storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicArrayStorageTextureResource(), array, 0x10000);

  const auto uint_array = BasicUintArrayStorageTextureDescriptor();
  Require("BasicStorageTexture", "uint 2D-array descriptor",
          uint_array.Base40() == 0x2017920000ull && uint_array.Width5() + 1u == 1 &&
              uint_array.Height5() + 1u == 1 && uint_array.Depth() + 1u == 1 &&
              uint_array.BaseArray5() == 0 &&
              uint_array.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
              uint_array.Format() == Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) &&
              uint_array.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              uint_array.DstSelXYZW() == DstSel(4, 0, 0, 1),
          "PPSA21268 uint 2D-array storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicUintArrayStorageTextureResource(), uint_array, 0x10000);

  const auto uint_volume = BasicUintVolumeStorageTextureDescriptor();
  Require("BasicStorageTexture", "uint 3D descriptor",
          uint_volume.Base40() == 0x2018060000ull && uint_volume.Width5() + 1u == 16 &&
              uint_volume.Height5() + 1u == 16 && uint_volume.Depth() + 1u == 16 &&
              uint_volume.BaseArray5() == 0 &&
              uint_volume.Type() == Prospero::GpuEnumValue(Prospero::ImageType::kColor3D) &&
              uint_volume.Format() == Prospero::GpuEnumValue(Prospero::BufferFormat::k16UInt) &&
              uint_volume.TileMode() == Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
              uint_volume.DstSelXYZW() == DstSel(4, 0, 0, 0),
          "PPSA21268 uint 3D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicUintVolumeStorageTextureResource(), uint_volume, 0x10000);

  char path[MAX_PATH]{};
  Require("BasicStorageTexture", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind : {"resource", "type", "tile", "mip", "swizzle",
                           "linear-rgb1-read", "bgra-read", "yzwx-read", "yzwx-format",
                           "array-base-view", "reserved", "metadata", "uint-format",
                           "uint-resource-float-format"}) {
    std::string command = std::string("\"") + path +
                          "\" --storage-texture-descriptor-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("BasicStorageTexture", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
            "CreateProcess failed");
    Require("BasicStorageTexture", "host",
            WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "descriptor death case timed out");
    DWORD exit_code = 0;
    const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require("BasicStorageTexture", "host", exited && exit_code == 321,
            std::string(kind) + " storage descriptor did not report a fatal error");
  }
  std::printf("[host]    %-32s ok\n", "BasicStorageTextureDescriptor");
}

void CheckStorageTextureLinearUploadLayout() {
  constexpr uint32_t format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm);
  constexpr uint32_t width = 3840;
  constexpr uint32_t height = 2160;
  constexpr uint32_t depth = 1;
  constexpr uint32_t tile = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
  const auto pitch = TileGetTexturePitch(format, width, 1, tile);
  TileSizeAlign total{};
  TileGetTextureTotalSize(format, width, height, depth, pitch, 1, tile, false,
                          &total);
  const auto layout = TextureCalcUploadLayout(
      format, width, height, 1, depth, pitch, tile, total.size, true, false,
      false, "StorageTextureLinearTest");
  const auto regions = TextureBuildUploadRegions(
      layout, VK_FORMAT_R8G8B8A8_UNORM, width, height, depth, 1, false,
      false, TextureUploadDestination::MipLevels,
      TextureUploadSliceLayout::MipChainPerSlice);
  Require("StorageTextureLinearUpload", "layout",
          pitch == width && total.size == 0x1fa4000 && total.align == 256 &&
              layout.tile == tile && layout.pitch == width &&
              layout.slice_stride == total.size && regions.size() == 1 &&
              regions[0].offset == 0 && regions[0].width == width &&
              regions[0].height == height && regions[0].pitch == width &&
              TextureCalcUploadSize(layout, regions, 1, depth,
                                    TextureUploadSliceLayout::MipChainPerSlice) ==
                  total.size,
          "linear RGBA8 storage upload lost Prospero pitch or allocation size");
  std::printf("[host]    %-32s ok\n", "StorageTextureLinearUpload");
}

void CheckStorageTextureLinearReadbackLayout() {
  ImageInfo info{};
  info.address = 0x4bcc40100ull;
  info.size = 0x1fa4000;
  info.format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm);
  info.width = 3840;
  info.height = 2160;
  info.pitch = 3840;
  info.levels = 1;
  info.tile = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
  info.swizzle = DstSel(4, 5, 6, 1);
  info.depth = 1;
  info.type = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  const auto transfer =
      MakeColorImageTransferInfo(info, VK_FORMAT_R8G8B8A8_UNORM, 4);
  const auto linear_size =
      ((static_cast<uint64_t>(transfer.height) - 1) * transfer.pitch +
       transfer.width) *
      transfer.bytes_per_element;
  Require("StorageTextureLinearReadback", "layout",
          transfer.address == info.address && transfer.size == info.size &&
              transfer.format == VK_FORMAT_R8G8B8A8_UNORM &&
              transfer.width == info.width && transfer.height == info.height &&
              transfer.pitch == info.pitch && transfer.bytes_per_element == 4 &&
              transfer.tile_mode == info.tile && linear_size == info.size,
          "normalized storage readback lost the exact PS5 linear image layout");
  std::printf("[host]    %-32s ok\n", "StorageTextureLinearReadback");
}

void CheckStorageImageSwizzleSpecializationId() {
  std::array<u32, 1> code{};
  HW::ComputeShaderInfo regs{};
  regs.cs_regs.data_addr = reinterpret_cast<uint64_t>(code.data());

  ShaderRecompiler::IR::Program identity_program;
  identity_program.binding_layout_complete = true;
  ShaderRecompiler::IR::ImageResource image;
  image.kind = ShaderRecompiler::IR::ResourceKind::StorageImage;
  image.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  identity_program.info.images.push_back(image);
  auto rgb1_program = identity_program;
  rgb1_program.info.images[0].storage_swizzle = DstSel(4, 5, 6, 1);

  const auto resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>();
  ShaderComputeInputInfo identity_info;
  identity_info.stage.program =
      std::make_shared<const ShaderRecompiler::IR::Program>(identity_program);
  identity_info.stage.resources = resources;
  auto rgb1_info = identity_info;
  rgb1_info.stage.program =
      std::make_shared<const ShaderRecompiler::IR::Program>(rgb1_program);

  const auto identity_id = ShaderGetIdCS(&regs, &identity_info, true);
  const auto rgb1_id = ShaderGetIdCS(&regs, &rgb1_info, true);
  Require("StorageImageSwizzleSpecializationId", "pipeline cache key",
          identity_id != rgb1_id && identity_id.ids.size() == rgb1_id.ids.size(),
          "storage swizzle-specialized SPIR-V variants share a pipeline ID");
  std::printf("[host]    %-32s ok\n", "StorageImageSwizzlePipelineId");
}

void CheckStorageTextureVolumeUploadLayout() {
  constexpr uint32_t format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  constexpr uint32_t width = 33;
  constexpr uint32_t height = 33;
  constexpr uint32_t depth = 33;
  constexpr uint32_t bytes_per_element = 8;
  const auto pitch = TileGetTexturePitch(
      format, width, 1, Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget));
  TileSizeAlign total{};
  TileGetTextureTotalSize(format, width, height, depth, pitch, 1,
                          Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget), true, &total);
  const auto layout = TextureCalcUploadLayout(
      format, width, height, 1, depth, pitch,
      Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget), total.size, true, false, true,
      "StorageTextureVolumeTest");
  const auto regions = TextureBuildUploadRegions(
      layout, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, depth, 1, false, true,
      TextureUploadDestination::MipLevels,
      TextureUploadSliceLayout::MipChainPerSlice);
  Require("StorageTextureVolumeUpload", "layout",
          pitch == 128 && total.size == 0x210000 &&
              layout.slice_stride == 0x8400 &&
              layout.source_slice_stride == 0x10000 &&
              layout.level_sizes[0].size == 0x8400 &&
              layout.level_sizes[0].src_size == 0x10000 &&
              TextureCalcUploadSize(layout, regions, 1, depth,
                                    TextureUploadSliceLayout::MipChainPerSlice) ==
                  total.size,
          "3D render-target upload did not preserve distinct linear and guest strides");

  std::vector<uint8_t> source(total.size);
  for (uint32_t z = 0; z < depth; z++) {
    std::fill_n(source.data() + z * layout.source_slice_stride,
                layout.source_slice_stride, static_cast<uint8_t>(z + 1));
  }
  std::vector<uint8_t> linear(layout.slice_stride * depth, 0);
  for (const uint32_t z : {0u, 1u, depth - 1u}) {
    const auto src_offset = TextureUploadSliceSourceOffset(
        layout, 0, z, TextureUploadSliceLayout::MipChainPerSlice);
    Require("StorageTextureVolumeUpload", "source offset",
            src_offset == static_cast<uint64_t>(z) * 0x10000,
            "volume slice selected a compact linear source offset");
    TileConvertTiledToLinearRenderTarget(
        linear.data() + regions[z].offset, source.data() + src_offset, width, height,
        regions[z].pitch, bytes_per_element, layout.level_sizes[0].size,
        layout.level_sizes[0].src_size, layout.level_sizes[0].x,
        layout.level_sizes[0].y);
    const auto value = static_cast<uint8_t>(z + 1);
    for (uint32_t y = 0; y < height; y++) {
      const auto row = regions[z].offset +
                       static_cast<uint64_t>(y) * regions[z].pitch * bytes_per_element;
      Require("StorageTextureVolumeUpload", "contents",
              std::all_of(linear.begin() + row,
                          linear.begin() + row + width * bytes_per_element,
                          [value](uint8_t byte) { return byte == value; }),
              "volume slice detiled from another slice's padded allocation");
    }
  }
  std::printf("[host]    %-32s ok\n", "StorageTextureVolumeUpload");
}

void CheckRenderTargetTileRoundTrip() {
  struct Case {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_element;
  };
  constexpr std::array cases{
      Case{129, 65, 1}, Case{257, 131, 2}, Case{3840, 2160, 4}, Case{65, 67, 8}};
  for (const auto test : cases) {
    const auto pitch =
        TileGetRenderTargetPitch(test.width, test.bytes_per_element);
    TileSizeAlign storage{};
    Require("RenderTargetTileRoundTrip", "layout",
            pitch >= test.width &&
                TileGetRenderTargetSize(test.width, test.height, pitch,
                                        test.bytes_per_element, &storage) &&
                storage.size != 0,
            "render-target test layout was rejected");
    std::vector<uint8_t> linear(storage.size, 0);
    for (uint32_t y = 0; y < test.height; y++) {
      for (uint32_t x = 0; x < test.width * test.bytes_per_element; x++) {
        const auto offset = static_cast<uint64_t>(y) * pitch *
                                test.bytes_per_element +
                            x;
        linear[offset] = static_cast<uint8_t>(
            (x * 37u + y * 101u + test.bytes_per_element * 13u) & 0xffu);
      }
    }
    std::vector<uint8_t> tiled(storage.size, 0xcd);
    std::vector<uint8_t> restored(storage.size, 0xa5);
    TileConvertLinearToTiledRenderTarget(
        tiled.data(), linear.data(), test.width, test.height, pitch,
        test.bytes_per_element, storage.size);
    TileConvertTiledToLinearRenderTarget(
        restored.data(), tiled.data(), test.width, test.height, pitch,
        test.bytes_per_element, storage.size);
    for (uint32_t y = 0; y < test.height; y++) {
      const auto row = static_cast<uint64_t>(y) * pitch * test.bytes_per_element;
      Require("RenderTargetTileRoundTrip", "contents",
              std::memcmp(linear.data() + row, restored.data() + row,
                          test.width * test.bytes_per_element) == 0,
              "linear-to-tiled render-target conversion did not round-trip");
    }
  }
  std::printf("[host]    %-32s ok\n", "RenderTargetTileRoundTrip");
}

void CheckDepthTargetTileRoundTrip() {
  struct Case {
    uint32_t width;
    uint32_t height;
    uint32_t guest_format;
    uint32_t depth_format;
    uint32_t bytes_per_element;
  };
  constexpr std::array cases{
      Case{8, 8, Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm),
           Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16), 2},
      Case{129, 130, Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm),
           Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16), 2},
      Case{8, 8, Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float),
           Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F), 4},
      Case{640, 360, Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float),
           Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F), 4}};
  for (const auto test : cases) {
    const auto pitch = TileGetTexturePitch(
        test.guest_format, test.width, 1, Prospero::GpuEnumValue(Prospero::TileMode::kDepth));
    TileSizeAlign stencil{};
    TileSizeAlign htile{};
    TileSizeAlign depth{};
    Require("DepthTargetTileRoundTrip", "layout",
            pitch >= test.width &&
                TileGetDepthSize(test.width, test.height, 0, test.depth_format,
                                 Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid), false,
                                 &stencil, &htile, &depth) &&
                depth.size != 0 && depth.align == 0x10000,
            "Prospero depth layout was rejected");
    std::vector<uint8_t> linear(depth.size, 0);
    for (uint32_t y = 0; y < test.height; y++) {
      for (uint32_t x = 0; x < test.width * test.bytes_per_element; x++) {
        const auto offset = static_cast<uint64_t>(y) * pitch *
                                test.bytes_per_element +
                            x;
        linear[offset] = static_cast<uint8_t>(
            (x * 29u + y * 83u + test.bytes_per_element * 17u) & 0xffu);
      }
    }
    std::vector<uint8_t> tiled(depth.size, 0xcd);
    std::vector<uint8_t> restored(depth.size, 0xa5);
    TileConvertLinearToTiledDepth(tiled.data(), linear.data(), test.guest_format,
                                  test.width, test.height, pitch, depth.size);
    TileConvertTiledToLinearDepth(restored.data(), tiled.data(), test.guest_format,
                                  test.width, test.height, pitch, depth.size);
    for (uint32_t y = 0; y < test.height; y++) {
      const auto row = static_cast<uint64_t>(y) * pitch * test.bytes_per_element;
      Require("DepthTargetTileRoundTrip", "contents",
              std::memcmp(linear.data() + row, restored.data() + row,
                          test.width * test.bytes_per_element) == 0,
              "linear-to-tiled PS5 depth conversion did not round-trip");
    }
    if (test.width == 8 && test.height == 8 && test.bytes_per_element == 2) {
      const auto *linear16 = reinterpret_cast<const uint16_t *>(linear.data());
      const auto *tiled16 = reinterpret_cast<const uint16_t *>(tiled.data());
      Require("DepthTargetTileRoundTrip", "Prospero D16 SW_64KB_Z_X anchors",
              tiled16[0] == linear16[0] && tiled16[1] == linear16[1] &&
                  tiled16[4] == linear16[2] && tiled16[2] == linear16[pitch] &&
                  tiled16[0x27] == linear16[5 * pitch + 3],
              "Prospero D16 depth address anchors changed");
    }
    if (test.width == 8 && test.height == 8 && test.bytes_per_element == 4) {
      const auto *linear32 = reinterpret_cast<const uint32_t *>(linear.data());
      const auto *tiled32 = reinterpret_cast<const uint32_t *>(tiled.data());
      Require("DepthTargetTileRoundTrip", "Prospero D32 SW_64KB_Z_X anchors",
              tiled32[0] == linear32[0] && tiled32[1] == linear32[1] &&
                  tiled32[4] == linear32[2] && tiled32[2] == linear32[pitch] &&
                  tiled32[0x27] == linear32[5 * pitch + 3],
              "Prospero D32 depth address anchors changed");
    }
    if (test.width == 129 && test.height == 130 &&
        test.bytes_per_element == 2) {
      const auto *linear16 = reinterpret_cast<const uint16_t *>(linear.data());
      const auto *tiled16 = reinterpret_cast<const uint16_t *>(tiled.data());
      Require("DepthTargetTileRoundTrip", "Prospero D16 block row",
              tiled16[0x10000 / sizeof(uint16_t)] == linear16[128 * pitch],
              "Prospero D16 depth block-row order changed");
    }
    if (test.width == 640 && test.height == 360 &&
        test.bytes_per_element == 4) {
      const auto *linear32 = reinterpret_cast<const uint32_t *>(linear.data());
      const auto *tiled32 = reinterpret_cast<const uint32_t *>(tiled.data());
      Require("DepthTargetTileRoundTrip", "Prospero D32 block order",
              tiled32[0x10000 / sizeof(uint32_t)] == linear32[128] &&
                  tiled32[0x50000 / sizeof(uint32_t)] == linear32[128 * pitch],
              "Prospero D32 depth block order changed");
    }
  }
  std::printf("[host]    %-32s ok\n", "DepthTargetTileRoundTrip");
}

void CheckStorageTextureGpuOwnedRebindState() {
  constexpr uintptr_t base = 0x0000000200200000ull;
  constexpr uint64_t size = 0x10000;
  auto *memory = static_cast<uint8_t *>(VirtualAlloc(
      reinterpret_cast<void *>(base), size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("StorageTextureGpuOwnedRebind", "allocation",
          memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  PageManager page_manager(CacheFault, nullptr);
  MemoryTracker tracker(page_manager);
  page_manager.OnGpuMap(base, size);
  tracker.ForEachUploadRange(base, size, true,
                             [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
  uint64_t readable = 0;
  MEMORY_BASIC_INFORMATION protection{};
  Require("StorageTextureGpuOwnedRebind", "owned",
          tracker.IsRegionGpuModified(base, size) && page_manager.IsMapped(base, size) &&
              (!HostMemoryQueryReadable(base, size, &readable) || readable < size) &&
              VirtualQuery(memory, &protection, sizeof(protection)) != 0 &&
              protection.Protect == PAGE_NOACCESS,
          "GPU-owned storage pages remained host-readable or lost tracker identity");

  tracker.UnmarkRegionAsGpuModified(base, size);
  readable = 0;
  Require("StorageTextureGpuOwnedRebind", "clean readback",
          !tracker.IsRegionGpuModified(base, size) &&
              !tracker.IsRegionCpuModified(base, size) &&
              HostMemoryQueryReadable(base, size, &readable) && readable == size,
          "clean storage readback did not publish readable coherent backing");
  tracker.MarkRegionAsGpuModified(base, size);
  readable = 0;
  Require("StorageTextureGpuOwnedRebind", "clean reclaim",
          tracker.IsRegionGpuModified(base, size) &&
              !tracker.IsRegionCpuModified(base, size) &&
              (!HostMemoryQueryReadable(base, size, &readable) || readable < size),
          "clean storage rebind did not reclaim GPU ownership without an upload");

  tracker.UnmarkRegionAsGpuModified(base, size);
  tracker.MarkRegionAsCpuModified(base, size);
  uint32_t dirty_ranges = 0;
  bool upload_called = false;
  tracker.ForEachUploadRange(
      base, size, true,
      [&](uint64_t, uint64_t) noexcept { dirty_ranges++; },
      [&]() noexcept { upload_called = true; });
  readable = 0;
  Require("StorageTextureGpuOwnedRebind", "dirty refresh",
          dirty_ranges == 1 && upload_called && tracker.IsRegionGpuModified(base, size) &&
              !tracker.IsRegionCpuModified(base, size) &&
              (!HostMemoryQueryReadable(base, size, &readable) || readable < size),
          "CPU-dirty storage rebind did not refresh once and reclaim GPU ownership");

  tracker.UnmarkRegionAsGpuModified(base, size);
  tracker.UntrackMemory(base, size);
  page_manager.OnGpuUnmap(base, size);
  Require("StorageTextureGpuOwnedRebind", "free", VirtualFree(memory, 0, MEM_RELEASE) != 0,
          "VirtualFree failed");
  std::printf("[host]    %-32s ok\n", "StorageTextureGpuOwnedRebind");
}

void CheckStorageTextureSampledReuse() {
  ImageInfo storage{};
  storage.address = 0x78650000;
  storage.size = 0x210000;
  storage.format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  storage.width = 33;
  storage.height = 33;
  storage.pitch = 128;
  storage.levels = 1;
  storage.tile = Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  storage.swizzle = DstSel(4, 5, 6, 7);
  storage.depth = 33;
  storage.type = Prospero::GpuEnumValue(Prospero::ImageType::kColor3D);

  Require("StorageTextureSampledReuse", "exact",
          ClassifyStorageSampledOverlap(storage, storage,
                                        VK_FORMAT_R16G16B16A16_SFLOAT,
                                        VK_FORMAT_R16G16B16A16_SFLOAT,
                                        true, false, true) ==
              StorageSampledOverlap::ExactImage,
          "exact GPU-owned storage image was not reusable for sampling");
  auto incompatible = storage;
  incompatible.format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16UNorm);
  Require("StorageTextureSampledReuse", "incompatible",
          ClassifyStorageSampledOverlap(incompatible, storage,
                                        VK_FORMAT_R16G16B16A16_UNORM,
                                        VK_FORMAT_R16G16B16A16_SFLOAT,
                                        true, false, true) ==
                  StorageSampledOverlap::Unsupported &&
              ClassifyStorageSampledOverlap(storage, storage,
                                            VK_FORMAT_R16G16B16A16_SFLOAT,
                                            VK_FORMAT_R16G16B16A16_SFLOAT,
                                            false, false, true) ==
                  StorageSampledOverlap::Unsupported &&
              ClassifyStorageSampledOverlap(storage, storage,
                                            VK_FORMAT_R16G16B16A16_SFLOAT,
                                            VK_FORMAT_R16G16B16A16_SFLOAT,
                                            true, true, true) ==
                  StorageSampledOverlap::Unsupported &&
              ClassifyStorageSampledOverlap(storage, storage,
                                            VK_FORMAT_R16G16B16A16_SFLOAT,
                                            VK_FORMAT_R16G16B16A16_SFLOAT,
                                            true, false, false) ==
                  StorageSampledOverlap::Unsupported,
          "incompatible storage-image state was accepted for sampling");
  auto separate = storage;
  separate.address += 0x400000;
  Require("StorageTextureSampledReuse", "separate",
          ClassifyStorageSampledOverlap(separate, storage,
                                        VK_FORMAT_R16G16B16A16_SFLOAT,
                                        VK_FORMAT_R16G16B16A16_SFLOAT,
                                        true, false, true) ==
              StorageSampledOverlap::None,
          "disjoint storage image was classified as an alias");

  ImageInfo ppsa02604_storage{};
  ppsa02604_storage.address = 0x7c690000;
  ppsa02604_storage.size = 0x870000;
  ppsa02604_storage.format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm);
  ppsa02604_storage.width = 1920;
  ppsa02604_storage.height = 1080;
  ppsa02604_storage.pitch = 1920;
  ppsa02604_storage.tile = Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  ppsa02604_storage.swizzle = DstSel(6, 5, 4, 7);
  ppsa02604_storage.type = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  auto ppsa02604_sampled = ppsa02604_storage;
  ppsa02604_sampled.format = Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb);
  Require("StorageTextureSampledReuse", "PPSA02604 sRGB view",
          ClassifyStorageSampledOverlap(
              ppsa02604_sampled, ppsa02604_storage, VK_FORMAT_R8G8B8A8_SRGB,
              VK_FORMAT_R8G8B8A8_UNORM, true, false, true) ==
              StorageSampledOverlap::ExactImage,
          "PPSA02604 GPU-owned UNORM storage image did not accept its exact sRGB view");
  auto alternate_swizzle = ppsa02604_storage;
  alternate_swizzle.swizzle = DstSel(4, 5, 6, 7);
  Require("StorageTextureSampledReuse", "descriptor swizzle view",
          ClassifyStorageSampledOverlap(
              alternate_swizzle, ppsa02604_storage, VK_FORMAT_R8G8B8A8_UNORM,
              VK_FORMAT_R8G8B8A8_UNORM, true, false, true) ==
              StorageSampledOverlap::ExactImage,
          "exact storage backing rejected a distinct sampled descriptor swizzle");
  const auto usage = TextureFormatUsage::Sampled | TextureFormatUsage::Storage;
  Require("StorageTextureSampledReuse", "usage",
          (TextureGetUsage(usage) &
           (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) ==
                  (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT) &&
              (TextureGetViewUsage(usage) &
               (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT)) ==
                  (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT),
          "shared storage image or view is missing sampled/storage usage");
  std::printf("[host]    %-32s ok\n", "StorageTextureSampledReuse");
}

[[noreturn]] void RunStorageTextureAccessDeathCase(const char *kind) {
  constexpr uintptr_t base = 0x0000000200300000ull;
  constexpr uint64_t size = 0x10000;
  auto *memory = VirtualAlloc(reinterpret_cast<void *>(base), size,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (memory != reinterpret_cast<void *>(base)) {
    EXIT("storage access child could not reserve its fixed test address\n");
  }
  ResourceMutex resource_mutex;
  PageManager page_manager(CacheFault, nullptr);
  BufferCache buffer_cache(page_manager, resource_mutex);
  if (std::strcmp(kind, "read") == 0) {
    page_manager.OnGpuMap(base, size, GpuAccess::Write);
    buffer_cache.ValidateGpuAccess(base, size, true, true);
  } else if (std::strcmp(kind, "write") == 0) {
    page_manager.OnGpuMap(base, size, GpuAccess::Read);
    buffer_cache.ValidateGpuAccess(base, size, true, true);
  } else {
    std::_Exit(0x7e);
  }
  std::_Exit(0x7f);
}

void CheckStorageTextureAccessPermissions() {
  constexpr uintptr_t base = 0x0000000200300000ull;
  constexpr uint64_t size = 0x10000;
  auto *memory = VirtualAlloc(reinterpret_cast<void *>(base), size,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  Require("StorageTextureAccess", "allocation", memory == reinterpret_cast<void *>(base),
          "fixed VirtualAlloc failed");
  ResourceMutex resource_mutex;
  PageManager page_manager(CacheFault, nullptr);
  BufferCache buffer_cache(page_manager, resource_mutex);
  page_manager.OnGpuMap(base, size, GpuAccess::ReadWrite);
  buffer_cache.ValidateGpuAccess(base, size, true, true);
  page_manager.OnGpuUnmap(base, size, GpuAccess::ReadWrite);
  Require("StorageTextureAccess", "free", VirtualFree(memory, 0, MEM_RELEASE) != 0,
          "VirtualFree failed");

  char path[MAX_PATH]{};
  Require("StorageTextureAccess", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind : {"read", "write"}) {
    std::string command = std::string("\"") + path +
                          "\" --storage-texture-access-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("StorageTextureAccess", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
            "CreateProcess failed");
    Require("StorageTextureAccess", "host",
            WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "access death case timed out");
    DWORD exit_code = 0;
    const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require("StorageTextureAccess", "host", exited && exit_code == 321,
            std::string(kind) + " access denial did not report a fatal error");
  }
  std::printf("[host]    %-32s ok\n", "StorageTextureAccessPermissions");
}

[[noreturn]] void RunMetaOverlapDeathCase(const char *kind) {
  constexpr uint64_t address = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x10000;
  auto *memory = static_cast<uint8_t *>(VirtualAlloc(
      reinterpret_cast<void *>(address), allocation_size,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (memory != reinterpret_cast<void *>(address)) {
    EXIT("metadata-overlap child could not reserve its fixed test address\n");
  }
  ResourceMutex resource_mutex;
  PageManager page_manager(CacheFault, nullptr);
  BufferCache buffer_cache(page_manager, resource_mutex);
  TextureCache texture_cache(page_manager, buffer_cache, resource_mutex);
  buffer_cache.SetTextureCache(texture_cache);
  page_manager.OnGpuMap(address, allocation_size);
  texture_cache.RegisterMeta(address, 0x8000);
  auto *ctx = reinterpret_cast<GraphicContext *>(1);

  if (std::strcmp(kind, "metadata-size") == 0) {
    texture_cache.RegisterMeta(address, 0x10000);
  } else if (std::strcmp(kind, "texture") == 0) {
    ImageInfo info{};
    info.address = address;
    info.size = 0x1000;
    info.width = 1;
    info.height = 1;
    (void)texture_cache.FindTexture(reinterpret_cast<CommandBuffer *>(1), ctx, info, false);
  } else if (std::strcmp(kind, "render-target") == 0) {
    RenderTargetInfo info{};
    info.address = address;
    info.size = 0x1000;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.width = 1;
    info.height = 1;
    info.pitch = 1;
    info.bytes_per_element = 4;
    info.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
    (void)texture_cache.FindRenderTarget(reinterpret_cast<CommandBuffer *>(1), ctx, info);
  } else if (std::strcmp(kind, "depth-target") == 0) {
    DepthTargetInfo info{};
    info.address = address;
    info.size = 0x1000;
    info.format = VK_FORMAT_D16_UNORM;
    info.guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm);
    info.width = 1;
    info.height = 1;
    info.pitch = 1;
    info.bytes_per_element = 2;
    info.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
    (void)texture_cache.FindDepthTarget(reinterpret_cast<CommandBuffer *>(1), ctx,
                                        info);
  } else if (std::strcmp(kind, "copy-stale") == 0) {
    if (!texture_cache.ClearMeta(address)) {
      std::_Exit(0x7f);
    }
    buffer_cache.CopyBuffer(nullptr, ctx, address + 0x9000, address, 0x1000);
  } else {
    EXIT("metadata-reuse child received an unknown mode\n");
  }
  std::_Exit(0x7f);
}

ShaderRecompiler::IR::ImageResource BasicMetadataReuseResource() {
  ShaderRecompiler::IR::ImageResource resource{};
  resource.kind = ShaderRecompiler::IR::ResourceKind::Image;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = true;
  return resource;
}

ShaderTextureResource BasicMetadataReuseDescriptor() {
  return {{0x0062b100u, 0xcad00000u, 0x003fc03fu, 0x90500facu,
           0x00000000u, 0x00700000u, 0x00000000u, 0x00000000u}};
}

[[noreturn]] void RunMetadataDescriptorDeathCase(const char *kind) {
  auto resource = BasicMetadataReuseResource();
  auto descriptor = BasicMetadataReuseDescriptor();
  if (std::strcmp(kind, "field1-reserved") == 0) {
    descriptor.fields[1] |= 0x20000000u;
  } else if (std::strcmp(kind, "field2-low-reserved") == 0) {
    descriptor.fields[2] |= 0x00001000u;
  } else if (std::strcmp(kind, "field2-high-reserved") == 0) {
    descriptor.fields[2] |= 0x10000000u;
  } else if (std::strcmp(kind, "unsupported-format") == 0) {
    descriptor.fields[1] =
        (descriptor.fields[1] & ~0x1ff00000u) |
        (Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8SInt) << 20u);
  } else if (std::strcmp(kind, "uint-format") == 0) {
    descriptor.fields[1] =
        (descriptor.fields[1] & ~0x1ff00000u) |
        (Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UInt) << 20u);
  } else {
    std::_Exit(0x7e);
  }
  ValidateMetadataReuseTexture(resource, descriptor, 0x10000);
  std::_Exit(0x7f);
}

void CheckMetaOverlapDeaths() {
  char path[MAX_PATH]{};
  Require("MetaOverlap", "host", GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind : {"metadata-size", "texture", "render-target", "depth-target",
                           "copy-stale"}) {
    std::string command = std::string("\"") + path +
                          "\" --meta-overlap-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("MetaOverlap", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
            "CreateProcess failed");
    Require("MetaOverlap", "host",
            WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "death case timed out");
    DWORD exit_code = 0;
    const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require("MetaOverlap", "host", exited && exit_code == 321,
            std::string(kind) + " did not reject registered metadata overlap");
  }
  std::printf("[host]    %-32s ok\n", "MetaOverlapRegistration");
}

void CheckOverlappingMetadataViews() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x60000;
  constexpr uint64_t metadata_size = 0x30000;
  constexpr uint64_t second = base + 0x18000;
  auto *memory = static_cast<uint8_t *>(VirtualAlloc(
      reinterpret_cast<void *>(base), allocation_size,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("OverlappingMetadataViews", "allocation",
          memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");

  ResourceMutex resource_mutex;
  CacheFaultContext fault_context;
  PageManager page_manager(CacheFault, &fault_context);
  BufferCache buffer_cache(page_manager, resource_mutex);
  TextureCache texture_cache(page_manager, buffer_cache, resource_mutex);
  fault_context.texture = &texture_cache;
  buffer_cache.SetTextureCache(texture_cache);
  page_manager.OnGpuMap(base, allocation_size);

  texture_cache.RegisterMeta(base, metadata_size);
  texture_cache.RegisterMeta(second, metadata_size);
  Require("OverlappingMetadataViews", "registration",
          texture_cache.IsMeta(base) && texture_cache.IsMeta(second) &&
              texture_cache.IsMetaRange(base, metadata_size) &&
              texture_cache.IsMetaRange(second, metadata_size) &&
              !texture_cache.IsMetaRange(base, metadata_size - 0x8000) &&
              !texture_cache.IsMetaRange(base + 0x8000, metadata_size),
          "overlapping metadata identities were not retained");
  Require("OverlappingMetadataViews", "clear",
          texture_cache.ClearMeta(base) && texture_cache.ClearMeta(second) &&
              texture_cache.IsMetaCleared(base, 0) &&
              texture_cache.IsMetaCleared(second, 0),
          "overlapping metadata clears were not retained independently");
  Require("OverlappingMetadataViews", "fault",
          page_manager.HandleFault(PageFaultAccess::Write, second),
          "shared metadata page did not transfer to CPU ownership");
  Require("OverlappingMetadataViews", "ownership",
          !texture_cache.IsMetaGpuModified(second, 0x1000),
          "shared metadata page retained GPU ownership after a CPU fault");

  texture_cache.UnmapMemory(base, allocation_size);
  page_manager.OnGpuUnmap(base, allocation_size);
  Require("OverlappingMetadataViews", "free",
          VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
  std::printf("[host]    %-32s ok\n", "OverlappingMetadataViews");
}

void CheckMetadataReuseDescriptors() {
  ValidateMetadataReuseTexture(BasicMetadataReuseResource(),
                               BasicMetadataReuseDescriptor(), 0x10000);
  char path[MAX_PATH]{};
  Require("MetadataReuseDescriptor", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind : {"field1-reserved", "field2-low-reserved",
                           "field2-high-reserved", "unsupported-format", "uint-format"}) {
    std::string command = std::string("\"") + path +
                          "\" --metadata-descriptor-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("MetadataReuseDescriptor", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != 0,
            "CreateProcess failed");
    Require("MetadataReuseDescriptor", "host",
            WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "death case timed out");
    DWORD exit_code = 0;
    const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require("MetadataReuseDescriptor", "host", exited && exit_code == 321,
            std::string(kind) + " descriptor was admitted");
  }
  std::printf("[host]    %-32s ok\n", "MetadataReuseDescriptors");
}

void CheckImageOverlapResolution() {
	VideoOutPixelFormatInfo video_format {};
	Require("ImageOverlapResolution", "existing video-out formats",
	        DecodeVideoOutPixelFormat(0x8000000022000000ull, &video_format) &&
	            video_format.format == VK_FORMAT_R8G8B8A8_SRGB &&
	            video_format.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb) &&
	            DecodeVideoOutPixelFormat(0x8000000000000000ull, &video_format) &&
	            video_format.format == VK_FORMAT_B8G8R8A8_SRGB &&
	            video_format.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb) &&
	            DecodeVideoOutPixelFormat(0x8100000022000000ull, &video_format) &&
	            video_format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 &&
	            video_format.guest_format == Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm),
	        "pre-existing PS5 video-out format mappings changed during decoder centralization");
	Require("ImageOverlapResolution", "B10G10R10A2 video-out format",
	        DecodeVideoOutPixelFormat(0x8100000000000000ull, &video_format) &&
	            video_format.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32 &&
	            video_format.guest_format ==
	                Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm) &&
	            IsSupportedVideoOutFormat(video_format.guest_format, video_format.format),
	        "PS5 B10G10R10A2 sRGB video-out format did not preserve alternate channel order");
	Require("ImageOverlapResolution", "video-out format guards",
	        !DecodeVideoOutPixelFormat(0x8100070400000000ull, &video_format) &&
	            !DecodeVideoOutPixelFormat(0x8100000000000000ull, nullptr) &&
	            !IsSupportedVideoOutFormat(Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb),
	                                       VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
	            !IsSupportedVideoOutFormat(Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm),
	                                       VK_FORMAT_R8G8B8A8_SRGB),
	        "unsupported PS5 video-out formats or mismatched guest/host pairs were admitted");
  TileSizeAlign render_target_layout{};
  Require("ImageOverlapResolution", "R8 render-target pitch",
          TileGetRenderTargetPitch(1920, 1) == 2048,
          "R8 target did not use native 256-pixel block width");
  Require("ImageOverlapResolution", "R8 render-target size",
          TileGetRenderTargetSize(1920, 1080, 2048, 1,
                                  &render_target_layout) &&
              render_target_layout.size == 0x280000 &&
              render_target_layout.align == 0x10000,
          "R8 target did not use native 64-KiB block geometry");
  Require("ImageOverlapResolution", "RGBA8 render-target size",
          TileGetRenderTargetPitch(1920, 4) == 1920 &&
              TileGetRenderTargetSize(1920, 1080, 1920, 4,
                                      &render_target_layout) &&
              render_target_layout.size == 0x870000,
          "RGBA8 target layout regressed");
  Require("ImageOverlapResolution", "PPSA19268 render-target edge",
          TileGetRenderTargetPitch(120, 8) == 128 &&
              TileGetRenderTargetSize(120, 67, 128, 8,
                                      &render_target_layout) &&
              render_target_layout.size == 0x20000 &&
              ImageRangeOverlaps(0x11029fff8ull, 1, 0x110280000ull,
                                 render_target_layout.size),
          "title write did not land in the final native render-target block");
  Require("ImageOverlapResolution", "render-target layout rejection",
          TileGetRenderTargetPitch(1920, 3) == 0 &&
              !TileGetRenderTargetSize(1920, 1080, 1920, 1,
                                       &render_target_layout),
          "unsupported element size or mismatched pitch was admitted");

  TileSizeOffset mip_levels[16]{};
  TilePaddedSize mip_padded[16]{};
  Require("ImageOverlapResolution", "PPSA09076 mip-chain layout",
          TileGetRenderTargetMipLayout(960, 540, 1024, 8, 10,
                                       &render_target_layout, mip_levels,
                                       mip_padded) &&
              render_target_layout.size == 0x650000 &&
              render_target_layout.align == 0x10000 &&
              mip_levels[0].offset == 0x1d0000 &&
              mip_levels[0].size == 0x480000 &&
              mip_levels[1].offset == 0x90000 &&
              mip_levels[1].size == 0x140000 &&
              mip_padded[1].width == 512 && mip_padded[1].height == 320,
          "native render-target mip allocation or subresource geometry regressed");
  Require("ImageOverlapResolution", "render-target mip-chain rejection",
          !TileGetRenderTargetMipLayout(960, 540, 960, 8, 10,
                                        &render_target_layout, nullptr, nullptr) &&
              !TileGetRenderTargetMipLayout(960, 540, 1024, 3, 10,
                                            &render_target_layout, nullptr,
                                            nullptr) &&
              !TileGetRenderTargetMipLayout(960, 540, 1024, 8, 0,
                                            &render_target_layout, nullptr,
                                            nullptr) &&
              !TileGetRenderTargetMipLayout(960, 540, 1024, 8, 17,
                                            &render_target_layout, nullptr,
                                            nullptr) &&
              !TileGetRenderTargetMipLayout(
                  4, 4, TileGetRenderTargetPitch(4, 4), 4, 4,
                  &render_target_layout, nullptr,
                                            nullptr),
          "invalid render-target mip topology was admitted");

  ImageInfo sampled{};
  sampled.address = 0x200000;
  sampled.size = 0x8000;
  ImageInfo sampled_alias = sampled;
  sampled_alias.address += 0x1000;
  Require("ImageOverlapResolution", "sampled alias",
          ClassifySampledOverlap(sampled_alias, sampled, false, true) ==
              SampledOverlap::ReadOnlyAlias,
          "read-only sampled pool alias was rejected");
  Require("ImageOverlapResolution", "sampled GPU owner",
          ClassifySampledOverlap(sampled_alias, sampled, true, true) ==
              SampledOverlap::Unsupported,
          "GPU-modified sampled alias was admitted");
  Require("ImageOverlapResolution", "sampled context",
          ClassifySampledOverlap(sampled_alias, sampled, false, false) ==
              SampledOverlap::Unsupported,
          "cross-context sampled alias was admitted");
  ImageInfo page_left = sampled;
  page_left.size = TRACKER_PAGE_SIZE / 2;
  ImageInfo same_page = page_left;
  same_page.address += page_left.size;
  Require("ImageOverlapResolution", "sampled shared page",
          !ImageRangeOverlaps(same_page.address, same_page.size,
                              page_left.address, page_left.size) &&
              ClassifySampledOverlap(same_page, page_left, false, true) ==
                  SampledOverlap::ReadOnlyAlias,
          "byte-disjoint sampled images sharing a tracker page were not aliased");
  ImageInfo separate_page = same_page;
  separate_page.address = page_left.address + TRACKER_PAGE_SIZE;
  Require("ImageOverlapResolution", "sampled separate page",
          ClassifySampledOverlap(separate_page, page_left, false, true) ==
              SampledOverlap::None,
          "sampled images on separate pages were aliased");
  constexpr uint64_t sampled_buffer_address = 0xe80e0000ull;
  constexpr uint64_t sampled_buffer_size = 0x870000ull;
  Require("ImageOverlapResolution", "sampled buffer write transition",
          IsSampledBufferWriteTransition(
              sampled_buffer_address, sampled_buffer_size,
              sampled_buffer_address, sampled_buffer_size, false),
          "exact page-aligned clean sampled image did not accept a buffer writer");
  Require("ImageOverlapResolution", "sampled buffer write guards",
          !IsSampledBufferWriteTransition(
              sampled_buffer_address, sampled_buffer_size - TRACKER_PAGE_SIZE,
              sampled_buffer_address, sampled_buffer_size, false) &&
              !IsSampledBufferWriteTransition(
                  sampled_buffer_address + 1, sampled_buffer_size,
                  sampled_buffer_address + 1, sampled_buffer_size, false) &&
              !IsSampledBufferWriteTransition(
                  sampled_buffer_address, sampled_buffer_size,
                  sampled_buffer_address, sampled_buffer_size, true),
          "partial, unaligned, or GPU-owned sampled image accepted a buffer writer");
  constexpr uint64_t edge_target_address = 0x108ad00100ull;
  constexpr uint64_t edge_target_size = 0x1fa400;
  constexpr uint64_t edge_fault_address = edge_target_address + edge_target_size + 0x1b8;
  Require("ImageOverlapResolution", "GPU target shared edge page",
          !ImageRangeOverlaps(edge_fault_address, 1, edge_target_address,
                              edge_target_size) &&
              ImagePageRangesOverlap(edge_fault_address, 1,
                                     edge_target_address, edge_target_size),
          "byte-disjoint CPU access lost the GPU owner of its shared tracker page");
  Image sampled_image{};
  sampled_image = page_left;
  Image same_page_image{};
  same_page_image = same_page;
  sampled_image.InvalidateCpuWrite(page_left.address + 1, 1);
  same_page_image.InvalidateCpuWrite(page_left.address + 1, 1);
  Require("ImageOverlapResolution", "sampled dirty fanout",
          sampled_image.IsCpuDirty() && same_page_image.IsCpuDirty(),
          "one guest write did not dirty every sampled image sharing its page");
  sampled_image.RefreshComplete();
  Require("ImageOverlapResolution", "sampled dirty independence",
          !sampled_image.IsCpuDirty() && same_page_image.IsCpuDirty(),
          "refreshing one sampled alias cleaned another alias");

  constexpr uint64_t fill_address = 0x73771080;
  constexpr uint64_t fill_size = sizeof(uint32_t);
  constexpr uint64_t texture_address = 0x73771000;
  constexpr uint64_t texture_size = 0x4000;
  Require("ImageOverlapResolution", "host fill sampled image",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, true, false, false) ==
              HostWriteOverlap::InvalidateImage,
          "host-backed fill did not invalidate its CPU-current sampled image");
  Require("ImageOverlapResolution", "host fill GPU owner",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, true, true, false) ==
              HostWriteOverlap::Unsupported,
          "host-backed fill was admitted over a GPU-owned sampled image");
  Require("ImageOverlapResolution", "host fill clean render target",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, true, false, false) ==
              HostWriteOverlap::InvalidateImage,
          "host-backed fill did not invalidate its clean render target");
  Require("ImageOverlapResolution", "host fill non-refreshable image",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, false, false, false) ==
              HostWriteOverlap::Unsupported,
          "host-backed fill was admitted over a non-refreshable image");
  constexpr uint64_t dma_address = 0x10f562000ull;
  constexpr uint64_t dma_size = 0x40000;
  constexpr uint64_t target_address = 0x10f570000ull;
  constexpr uint64_t target_size = 0x20000;
  Require("ImageOverlapResolution", "host copy clean render target",
          ClassifyHostWriteOverlap(dma_address, dma_size, target_address,
                                   target_size, true, false, false) ==
              HostWriteOverlap::InvalidateImage,
          "host-backed copy did not invalidate the covered clean render target");
  Require("ImageOverlapResolution", "host copy GPU render target",
          ClassifyHostWriteOverlap(dma_address, dma_size, target_address,
                                   target_size, true, true, false) ==
              HostWriteOverlap::Unsupported,
          "host-backed copy was admitted over a GPU-owned render target");
  Require("ImageOverlapResolution", "host copy partial clean render target",
          ClassifyHostWriteOverlap(target_address + 0x1000, 0x1000,
                                   target_address, target_size, true, false,
                                   false) == HostWriteOverlap::InvalidateImage,
          "partial host copy did not invalidate its clean render target");
  Require("ImageOverlapResolution", "host copy shared target page",
          ClassifyHostWriteOverlap(target_address + target_size - 1, 1,
                                   target_address, target_size - 1, true, false,
                                   false) == HostWriteOverlap::InvalidateImage,
          "shared-page host copy did not conservatively invalidate the clean target");
  Require("ImageOverlapResolution", "host fill metadata",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, true, false, true) ==
              HostWriteOverlap::Unsupported,
          "host-backed sampled-image fill was admitted over metadata pages");
  Require("ImageOverlapResolution", "host fill disjoint page",
          ClassifyHostWriteOverlap(texture_address + texture_size, fill_size,
                                   texture_address, texture_size, true, false,
                                   true) ==
              HostWriteOverlap::None,
          "host-backed fill aliased a sampled image on another page");

  constexpr uint64_t video_address = 0x1000c10000ull;
  constexpr uint64_t video_size = 0x1fe0000ull;
  Require("ImageOverlapResolution", "formatted video-out write",
          ClassifyBufferImageAlias(video_address, video_size, video_address,
                                   video_size, true, false, false, true, true) ==
              BufferImageAlias::VideoOutWrite,
          "exact formatted video-out buffer write was rejected");
  Require("ImageOverlapResolution", "video-out alias guards",
          ClassifyBufferImageAlias(video_address, video_size,
                                   video_address + video_size, video_size, true,
                                   false, false, true, true) == BufferImageAlias::None &&
              ClassifyBufferImageAlias(video_address, video_size - 4,
                                       video_address, video_size, true, false, false,
                                       true, true) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(video_address, video_size, video_address,
                                       video_size, false, false, false, true, true) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(video_address, video_size, video_address,
                                       video_size, true, false, true, true, true) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(video_address, video_size, video_address,
                                       video_size, true, false, false, false, true) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(video_address, video_size, video_address,
                                       video_size, true, false, false, true, false) ==
                  BufferImageAlias::Unsupported,
          "unsupported video-out buffer alias was admitted");

  constexpr uint64_t video_metadata = 0x1020c10000ull;
  Require("ImageOverlapResolution", "video-out DCC 256/64/64",
          ClassifyVideoOutCompression(true, video_metadata, 0x208u, 0) ==
              VideoOutCompression::Dcc256_64_64,
          "Prospero video-out DCC 256/64/64 mode was rejected");
  Require("ImageOverlapResolution", "video-out compression guards",
          ClassifyVideoOutCompression(false, 0, 0, 0) ==
                  VideoOutCompression::Uncompressed &&
              ClassifyVideoOutCompression(true, 0, 0x208u, 0) ==
                  VideoOutCompression::Unsupported &&
              ClassifyVideoOutCompression(true, video_metadata + 1, 0x208u, 0) ==
                  VideoOutCompression::Unsupported &&
              ClassifyVideoOutCompression(true, video_metadata, 0x204u, 0) ==
                  VideoOutCompression::Unsupported &&
              ClassifyVideoOutCompression(true, video_metadata, 0x208u, 1) ==
                  VideoOutCompression::Unsupported &&
              ClassifyVideoOutCompression(false, video_metadata, 0, 0) ==
                  VideoOutCompression::Unsupported,
          "unsupported video-out compression state was admitted");
  Require("ImageOverlapResolution", "compressed video-out native ownership",
          CanUseVideoOutNativeWithoutUpload(
              VideoOutCompression::Dcc256_64_64, true, false, false) &&
              CanUseVideoOutNativeWithoutUpload(
                  VideoOutCompression::Dcc256_64_64, false, true, false) &&
              !CanUseVideoOutNativeWithoutUpload(
                  VideoOutCompression::Dcc256_64_64, false, false, false) &&
              !CanUseVideoOutNativeWithoutUpload(
                  VideoOutCompression::Dcc256_64_64, true, false, true) &&
              !CanUseVideoOutNativeWithoutUpload(
                  VideoOutCompression::Uncompressed, true, true, false),
          "compressed video-out upload/read ownership boundary regressed");

  VkClearColorValue clear{};
  Require("ImageOverlapResolution", "RGBA8 compute clear decode",
          DecodePackedColorClear(VK_FORMAT_R8G8B8A8_UNORM, 0x80402010u,
                                 &clear) &&
              std::abs(clear.float32[0] - 16.0f / 255.0f) < 0.000001f &&
              std::abs(clear.float32[1] - 32.0f / 255.0f) < 0.000001f &&
              std::abs(clear.float32[2] - 64.0f / 255.0f) < 0.000001f &&
              std::abs(clear.float32[3] - 128.0f / 255.0f) < 0.000001f,
          "packed RGBA8 clear did not preserve component values");
  Require("ImageOverlapResolution", "BGRA8 compute clear decode",
          DecodePackedColorClear(VK_FORMAT_B8G8R8A8_UNORM, 0x80402010u,
                                 &clear) &&
              std::abs(clear.float32[0] - 64.0f / 255.0f) < 0.000001f &&
              std::abs(clear.float32[1] - 32.0f / 255.0f) < 0.000001f &&
              std::abs(clear.float32[2] - 16.0f / 255.0f) < 0.000001f,
          "packed BGRA8 clear did not account for memory channel order");
  const uint32_t rgb10_clear = 1023u | (512u << 10u) | (1u << 20u) | (3u << 30u);
  Require("ImageOverlapResolution", "RGB10 compute clear decode",
          DecodePackedColorClear(VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                 rgb10_clear, &clear) &&
              clear.float32[0] == 1.0f &&
              std::abs(clear.float32[1] - 512.0f / 1023.0f) < 0.000001f &&
              std::abs(clear.float32[2] - 1.0f / 1023.0f) < 0.000001f &&
              clear.float32[3] == 1.0f,
          "packed RGB10 clear did not preserve component values");
  Require("ImageOverlapResolution", "sRGB compute clear decode",
          DecodePackedColorClear(VK_FORMAT_R8G8B8A8_SRGB, 0xff808080u,
                                 &clear) &&
              std::abs(clear.float32[0] - 0.215861f) < 0.00001f &&
              std::abs(clear.float32[1] - 0.215861f) < 0.00001f &&
              std::abs(clear.float32[2] - 0.215861f) < 0.00001f &&
              clear.float32[3] == 1.0f &&
              !DecodePackedColorClear(VK_FORMAT_R16G16_UNORM, 0, &clear),
          "sRGB clear decode or unsupported-format guard regressed");
  uint8_t stencil_clear = 0;
  Require("ImageOverlapResolution", "stencil compute clear decode",
          DecodePackedStencilClear(0x7f7f7f7fu, &stencil_clear) &&
              stencil_clear == 0x7fu &&
              DecodePackedStencilClear(0, &stencil_clear) &&
              stencil_clear == 0 &&
              !DecodePackedStencilClear(0x0000007fu, &stencil_clear) &&
              !DecodePackedStencilClear(0, nullptr),
          "packed stencil clear admitted non-uniform byte values");
  float depth_clear = -1.0f;
  Require("ImageOverlapResolution", "D32 compute clear decode",
          DecodePackedDepthClear(VK_FORMAT_D32_SFLOAT, 0u, &depth_clear) &&
              depth_clear == 0.0f &&
              DecodePackedDepthClear(VK_FORMAT_D32_SFLOAT_S8_UINT,
                                     0x3f800000u, &depth_clear) &&
              depth_clear == 1.0f &&
              !DecodePackedDepthClear(VK_FORMAT_D16_UNORM, 0u, &depth_clear) &&
              !DecodePackedDepthClear(VK_FORMAT_D32_SFLOAT, 0xbf800000u,
                                      &depth_clear) &&
              !DecodePackedDepthClear(VK_FORMAT_D32_SFLOAT, 0x40000000u,
                                      &depth_clear) &&
              !DecodePackedDepthClear(VK_FORMAT_D32_SFLOAT, 0x7f800000u,
                                      &depth_clear) &&
              !DecodePackedDepthClear(VK_FORMAT_D32_SFLOAT, 0u, nullptr),
          "D32 compute clear decode admitted an unsupported format or value");
  DepthTargetInfo native_clear_target{};
  native_clear_target.address = 0x1158d80000ull;
  native_clear_target.size = 0x870000ull;
  native_clear_target.format = VK_FORMAT_D32_SFLOAT;
  native_clear_target.guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  native_clear_target.bytes_per_element = 4;
  native_clear_target.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  Require("ImageOverlapResolution", "native D32 buffer clear target",
          CanNativeClearDepthFromBuffer(native_clear_target,
                                        native_clear_target.address,
                                        native_clear_target.size),
          "exact uncompressed PS5 D32 target was rejected");
  auto compressed_depth = native_clear_target;
  compressed_depth.htile_address = 0x1161480000ull;
  compressed_depth.htile_size = 0x10000ull;
  auto d16_depth = native_clear_target;
  d16_depth.format = VK_FORMAT_D16_UNORM;
  d16_depth.guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm);
  d16_depth.bytes_per_element = 2;
  Require("ImageOverlapResolution", "native depth buffer clear guards",
          !CanNativeClearDepthFromBuffer(native_clear_target,
                                         native_clear_target.address,
                                         native_clear_target.size - 4) &&
              !CanNativeClearDepthFromBuffer(native_clear_target,
                                             native_clear_target.address + 4,
                                             native_clear_target.size) &&
              !CanNativeClearDepthFromBuffer(compressed_depth,
                                             compressed_depth.address,
                                             compressed_depth.size) &&
              !CanNativeClearDepthFromBuffer(d16_depth, d16_depth.address,
                                             d16_depth.size),
          "partial, offset, HTile-backed, or D16 depth clear was admitted");

  constexpr uint64_t target_buffer_address = 0x1158d80000ull;
  constexpr uint64_t target_buffer_size = 0x870000ull;
  TileSizeAlign target_storage{};
  Require("ImageOverlapResolution", "formatted render-target write",
          TileGetRenderTargetPitch(1920, 4) == 1920 &&
              TileGetRenderTargetSize(1920, 1080, 1920, 4, &target_storage) &&
              target_storage.align == 0x10000 &&
              target_storage.size == target_buffer_size &&
              ClassifyBufferImageAlias(target_buffer_address, target_buffer_size,
                                       target_buffer_address, target_buffer_size,
                                       false, true, true, true, true) ==
                  BufferImageAlias::RenderTargetWrite,
          "exact GPU-owned render target was not admitted for formatted buffer synchronization");
  Require("ImageOverlapResolution", "render-target buffer alias guards",
          ClassifyBufferImageAlias(target_buffer_address,
                                   target_buffer_size - 4,
                                   target_buffer_address, target_buffer_size,
                                   false, true, true, true, true) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(target_buffer_address, target_buffer_size,
                                       target_buffer_address, target_buffer_size,
                                       false, true, false, true, true) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(target_buffer_address, target_buffer_size,
                                       target_buffer_address, target_buffer_size,
                                       false, true, true, false, true) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(target_buffer_address, target_buffer_size,
                                       target_buffer_address, target_buffer_size,
                                       false, true, true, true, false) ==
                  BufferImageAlias::Unsupported &&
              ClassifyBufferImageAlias(target_buffer_address + 0x100,
                                       target_buffer_size,
                                       target_buffer_address + 0x100,
                                       target_buffer_size,
                                       false, true, true, true, true) ==
                  BufferImageAlias::Unsupported,
          "unsupported render-target buffer alias was admitted");

  RenderTargetInfo target{};
  target.address = sampled.address;
  target.size = sampled.size;
  Require("ImageOverlapResolution", "render target reuse",
          ClassifyRenderTargetOverlap(sampled, false, true, target) ==
              RenderTargetOverlap::RetireSampled,
          "exact CPU-owned sampled allocation was not retired for a render target");
  Require("ImageOverlapResolution", "render target GPU owner",
          ClassifyRenderTargetOverlap(sampled, true, true, target) ==
              RenderTargetOverlap::Unsupported,
          "GPU-owned sampled allocation was retired for a render target");
  Require("ImageOverlapResolution", "render target context",
          ClassifyRenderTargetOverlap(sampled, false, false, target) ==
              RenderTargetOverlap::Unsupported,
          "cross-context sampled allocation was retired for a render target");
  target.address += 0x2000;
  target.size = 0x2000;
  Require("ImageOverlapResolution", "render target pool reuse",
          ClassifyRenderTargetOverlap(sampled, false, true, target) ==
              RenderTargetOverlap::RetireSampled,
          "page-contained CPU-owned sampled pool was not retired for a render target");
  target.address = sampled.address + 0x6000;
  target.size = 0x4000;
  Require("ImageOverlapResolution", "render target chance overlap",
          ClassifyRenderTargetOverlap(sampled, false, true, target) ==
              RenderTargetOverlap::RetireSampled,
          "page-isolated sampled pool overlap was not retired for a render target");
  target.address++;
  Require("ImageOverlapResolution", "render target partial page",
          ClassifyRenderTargetOverlap(sampled, false, true, target) ==
              RenderTargetOverlap::Unsupported,
          "partially shared tracker page was retired for a render target");
  target.address = page_left.address + page_left.size;
  target.size = page_left.size;
  Require("ImageOverlapResolution", "render target shared page",
          ClassifyRenderTargetOverlap(page_left, false, true, target) ==
              RenderTargetOverlap::Unsupported,
          "byte-disjoint allocation sharing a tracker page was silently admitted");
  target.address = sampled.address + sampled.size;
  target.size = sampled.size;
  Require("ImageOverlapResolution", "render target adjacent",
          ClassifyRenderTargetOverlap(sampled, false, true, target) ==
              RenderTargetOverlap::None,
          "adjacent sampled and render-target allocations were treated as aliases");

  RenderTargetInfo old_target{};
  old_target.address = 0x108d80000ull;
  old_target.size = 0x10000;
  old_target.format = VK_FORMAT_R8G8B8A8_UNORM;
  old_target.width = 48;
  old_target.height = 48;
  old_target.pitch = 128;
  old_target.bytes_per_element = 4;
  old_target.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  RenderTargetInfo replacement_target = old_target;
  replacement_target.size = 0x20000;
  replacement_target.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  replacement_target.width = 128;
  replacement_target.height = 128;
  replacement_target.bytes_per_element = 8;
  Require("ImageOverlapResolution", "render target allocation-pool replacement",
          ClassifyRenderTargetOverlap(old_target, false, false, true,
                                      replacement_target) ==
              RenderTargetOverlap::RetireTarget,
          "clean same-base render-target pool allocation was not retired");
  Require("ImageOverlapResolution", "render target allocation-pool GPU owner",
          ClassifyRenderTargetOverlap(old_target, true, false, true,
                                      replacement_target) ==
              RenderTargetOverlap::Unsupported,
          "GPU-owned render-target pool allocation was retired");
  Require("ImageOverlapResolution", "render target allocation-pool buffer owner",
          ClassifyRenderTargetOverlap(old_target, false, true, true,
                                      replacement_target) ==
              RenderTargetOverlap::Unsupported,
          "buffer-dirty render-target pool allocation was retired");
  Require("ImageOverlapResolution", "render target allocation-pool context",
          ClassifyRenderTargetOverlap(old_target, false, false, false,
                                      replacement_target) ==
              RenderTargetOverlap::Unsupported,
          "cross-context render-target pool allocation was retired");
  auto offset_target = replacement_target;
  offset_target.address += TRACKER_PAGE_SIZE;
  Require("ImageOverlapResolution", "render target allocation-pool offset",
          ClassifyRenderTargetOverlap(old_target, false, false, true, offset_target) ==
              RenderTargetOverlap::Unsupported,
          "offset render-target overlap was treated as allocation-pool replacement");
  auto partial_page_target = replacement_target;
  partial_page_target.address++;
  Require("ImageOverlapResolution", "render target allocation-pool partial page",
          ClassifyRenderTargetOverlap(old_target, false, false, true,
                                      partial_page_target) ==
              RenderTargetOverlap::Unsupported,
          "partially shared tracker page was treated as allocation-pool replacement");
  auto same_shape_target = old_target;
  same_shape_target.format = VK_FORMAT_R8G8B8A8_SRGB;
  Require("ImageOverlapResolution", "render target RGBA8 sRGB reinterpretation",
          IsCompatibleRenderTargetView(old_target, same_shape_target),
          "exact RGBA8 UNORM-to-sRGB target was not recognized as a compatible view");
  Require("ImageOverlapResolution", "render target sRGB no retirement",
          ClassifyRenderTargetOverlap(old_target, false, false, true, same_shape_target) ==
              RenderTargetOverlap::Unsupported,
          "compatible render-target view fell through to target retirement");
  auto incompatible_same_shape_target = same_shape_target;
  incompatible_same_shape_target.format = VK_FORMAT_R8G8B8A8_UINT;
  Require("ImageOverlapResolution", "render target incompatible same shape",
          !IsCompatibleRenderTargetView(old_target, incompatible_same_shape_target) &&
              ClassifyRenderTargetOverlap(old_target, false, false, true,
                                          incompatible_same_shape_target) ==
                  RenderTargetOverlap::Unsupported,
          "incompatible same-storage render-target format was admitted");

  RenderTargetInfo ppsa02604_unorm_target{};
  ppsa02604_unorm_target.address = 0x79c50000ull;
  ppsa02604_unorm_target.size = 0x870000;
  ppsa02604_unorm_target.format = VK_FORMAT_B8G8R8A8_UNORM;
  ppsa02604_unorm_target.width = 1920;
  ppsa02604_unorm_target.height = 1080;
  ppsa02604_unorm_target.pitch = 1920;
  ppsa02604_unorm_target.bytes_per_element = 4;
  ppsa02604_unorm_target.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  auto ppsa02604_srgb_target = ppsa02604_unorm_target;
  ppsa02604_srgb_target.format = VK_FORMAT_B8G8R8A8_SRGB;
  Require("ImageOverlapResolution", "PPSA02604 BGRA8 sRGB reinterpretation",
          IsCompatibleRenderTargetView(ppsa02604_unorm_target, ppsa02604_srgb_target),
          "PPSA02604 exact BGRA8 UNORM-to-sRGB target was not recognized as a view");
  auto adjacent_target = replacement_target;
  adjacent_target.address = old_target.address + old_target.size;
  Require("ImageOverlapResolution", "render target allocation-pool adjacent",
          ClassifyRenderTargetOverlap(old_target, false, false, true, adjacent_target) ==
              RenderTargetOverlap::None,
          "adjacent render targets were treated as allocation-pool replacement");

  Require("ImageOverlapResolution", "metadata retains sampled image",
          ClassifyMetaImageOverlap(true, false, false, false) ==
              MetaImageOverlap::RetainSampled,
          "CPU-current sampled metadata alias was rejected");
  Require("ImageOverlapResolution", "metadata retires render target",
          ClassifyMetaImageOverlap(false, true, false, false) ==
              MetaImageOverlap::RetireTarget,
          "CPU-current render target was not retired for metadata reuse");
  Require("ImageOverlapResolution", "metadata rejects GPU target",
          ClassifyMetaImageOverlap(false, true, true, false) ==
                  MetaImageOverlap::Unsupported &&
              ClassifyMetaImageOverlap(false, true, false, true) ==
                  MetaImageOverlap::Unsupported,
          "unordered or buffer-dirty render target was admitted for metadata reuse");
  Require("ImageOverlapResolution", "metadata rejects unsupported image",
          ClassifyMetaImageOverlap(false, false, false, false) ==
              MetaImageOverlap::Unsupported,
          "unsupported cached image kind was admitted for metadata reuse");

  const std::array isolated_retirement{
      ImageRetirementRange{sampled.address, TRACKER_PAGE_SIZE, true},
      ImageRetirementRange{sampled.address + 2 * TRACKER_PAGE_SIZE,
                           TRACKER_PAGE_SIZE, false}};
  Require("ImageOverlapResolution", "isolated retirement",
          !FindImageRetirementConflict(isolated_retirement).Exists(),
          "page-isolated sampled retirement was rejected");
  const std::array tail_alias_retirement{
      ImageRetirementRange{sampled.address, 3 * TRACKER_PAGE_SIZE, true},
      ImageRetirementRange{sampled.address + 2 * TRACKER_PAGE_SIZE,
                           TRACKER_PAGE_SIZE, false}};
  const auto retirement_conflict =
      FindImageRetirementConflict(tail_alias_retirement);
  Require("ImageOverlapResolution", "retained tail alias",
          retirement_conflict.Exists() && retirement_conflict.retired == 0 &&
              retirement_conflict.retained == 1,
          "retiring a wide image silently untracked a retained tail alias");
  DepthTargetInfo depth{};
  depth.address = sampled.address;
  depth.size = 0x6000;
  depth.depth_load_clear = true;
  Require("ImageOverlapResolution", "clear depth reuse",
          ClassifyDepthOverlap(sampled, false, depth) ==
              DepthOverlap::RetireSampled,
          "equal-base CPU-owned sampled image was not classified as pool reuse");
  Require("ImageOverlapResolution", "gpu owner",
          ClassifyDepthOverlap(sampled, true, depth) ==
              DepthOverlap::Unsupported,
          "GPU-owned sampled image was admitted for retirement");
  depth.depth_load_clear = false;
  Require("ImageOverlapResolution", "depth load",
          ClassifyDepthOverlap(sampled, false, depth) ==
              DepthOverlap::Unsupported,
          "load-preserving depth use was admitted for retirement");
  depth.depth_load_clear = true;
  depth.stencil_address = sampled.address + 0x6000;
  depth.stencil_size = 0x2000;
  Require("ImageOverlapResolution", "stencil load",
          ClassifyDepthOverlap(sampled, false, depth) ==
              DepthOverlap::Unsupported,
          "load-preserving stencil use was admitted for retirement");
  depth.stencil_load_clear = true;
  Require("ImageOverlapResolution", "clear stencil reuse",
          ClassifyDepthOverlap(sampled, false, depth) ==
              DepthOverlap::RetireSampled,
          "fully cleared depth/stencil reuse was rejected");
  depth.stencil_load_clear = false;
  depth.stencil_access = false;
  Require("ImageOverlapResolution", "unused stencil first load",
          CanLoadStencilAttachment(depth, false),
          "unused stencil unnecessarily required initialized contents");
  depth.stencil_access = true;
  Require("ImageOverlapResolution", "uninitialized stencil access",
          !CanLoadStencilAttachment(depth, false),
          "uninitialized stencil access was silently admitted");
  Require("ImageOverlapResolution", "initialized stencil load",
          CanLoadStencilAttachment(depth, true),
          "initialized stencil contents could not be loaded");
  depth.stencil_load_clear = true;
  Require("ImageOverlapResolution", "stencil clear initializes",
          CanLoadStencilAttachment(depth, false),
          "stencil clear did not initialize the attachment");
  depth.address = sampled.address;
  depth.size = 0xff0000;
  depth.stencil_address = sampled.address + 0x1000000;
  depth.stencil_size = 0x200000;
  Require("ImageOverlapResolution", "logical depth view",
          IsDepthTargetRangeCompatible(depth, depth.address, 0xfd2000),
          "same-base logical depth range did not reuse its padded target");
  Require("ImageOverlapResolution", "complete depth view",
          IsDepthTargetRangeCompatible(depth, depth.address, depth.size),
          "complete depth range was rejected");
  Require("ImageOverlapResolution", "oversized depth view",
          !IsDepthTargetRangeCompatible(depth, depth.address, depth.size + 1),
          "oversized depth range was silently admitted");
  Require("ImageOverlapResolution", "offset depth view",
          !IsDepthTargetRangeCompatible(depth, depth.address + 1, depth.size - 1),
          "offset depth range was silently admitted");
  Require("ImageOverlapResolution", "complete stencil view",
          IsDepthTargetRangeCompatible(depth, depth.stencil_address, depth.stencil_size),
          "complete stencil range was rejected");
  Require("ImageOverlapResolution", "partial stencil view",
          !IsDepthTargetRangeCompatible(depth, depth.stencil_address, depth.stencil_size - 1),
          "partial stencil range was silently admitted");
  depth.address = sampled.address + 0x1000;
  Require("ImageOverlapResolution", "offset alias",
          ClassifyDepthOverlap(sampled, false, depth) ==
              DepthOverlap::Unsupported,
          "offset image overlap was admitted for retirement");
  depth.address = sampled.address + sampled.size;
  depth.stencil_address = 0;
  depth.stencil_size = 0;
  depth.stencil_load_clear = false;
  Require("ImageOverlapResolution", "adjacent",
          ClassifyDepthOverlap(sampled, false, depth) == DepthOverlap::None,
          "byte-disjoint adjacent image ranges were treated as aliases");
  std::printf("[host]    %-32s ok\n", "ImageOverlapResolution");
}

void CheckMsaaCompatibility() {
  Require("MsaaCompatibility", "single sample",
          !depth_msaa_single_sample_compatible(0),
          "native single-sample state was classified as compatibility");
  Require("MsaaCompatibility", "two fragments",
          depth_msaa_single_sample_compatible(1),
          "two-fragment compatibility state was rejected");
  Require("MsaaCompatibility", "four fragments",
          depth_msaa_single_sample_compatible(2),
          "existing four-fragment compatibility state regressed");
  Require("MsaaCompatibility", "eight fragments",
          !depth_msaa_single_sample_compatible(3),
          "unsupported eight-fragment state was silently admitted");
  Require("MsaaCompatibility", "color two fragments",
          color_msaa_single_sample_compatible(1, 1),
          "matching two-sample color compatibility state was rejected");
  Require("MsaaCompatibility", "color four fragments",
          color_msaa_single_sample_compatible(2, 2),
          "existing matching four-sample color compatibility state regressed");
  Require("MsaaCompatibility", "color mismatch",
          !color_msaa_single_sample_compatible(2, 1),
          "mismatched color sample/fragment state was silently admitted");
  Require("MsaaCompatibility", "color eight fragments",
          !color_msaa_single_sample_compatible(3, 3),
          "unsupported eight-sample color state was silently admitted");
  std::printf("[host]    %-32s ok\n", "MsaaCompatibility");
}

void CheckDepthHtileStencilCompatibility() {
  Require("DepthHtileStencilCompatibility", "disabled acceleration",
          depth_htile_stencil_acceleration_compatible(false, false, true),
          "disabled Hi-Stencil state was rejected");
  Require("DepthHtileStencilCompatibility", "PS5 stencil plus HTile",
          depth_htile_stencil_acceleration_compatible(true, true, false),
          "valid PS5 Hi-Stencil attachment was rejected");
  Require("DepthHtileStencilCompatibility", "missing stencil plane",
          !depth_htile_stencil_acceleration_compatible(false, true, false),
          "Hi-Stencil without a stencil plane was silently admitted");
  Require("DepthHtileStencilCompatibility", "missing HTile metadata",
          !depth_htile_stencil_acceleration_compatible(true, false, false),
          "Hi-Stencil without HTile metadata was silently admitted");
  std::printf("[host]    %-32s ok\n", "DepthHtileStencilCompatibility");
}

void CheckStencilAttachmentAccess() {
  PipelineStencilStaticState state{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP,
                                   VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS};
  PipelineStencilDynamicState dynamic{0xff, 0xff, 0};
  Require("StencilAttachmentAccess", "always keep",
          !stencil_face_accesses_attachment(state, dynamic),
          "ALWAYS/KEEP state was classified as stencil access");
  state.compareOp = VK_COMPARE_OP_EQUAL;
  Require("StencilAttachmentAccess", "compare reads",
          stencil_face_accesses_attachment(state, dynamic),
          "real stencil comparison was classified as no access");
  state.compareOp = VK_COMPARE_OP_ALWAYS;
  state.passOp = VK_STENCIL_OP_ZERO;
  Require("StencilAttachmentAccess", "write operation",
          stencil_face_accesses_attachment(state, dynamic),
          "write-capable stencil operation was classified as no access");
  dynamic.writeMask = 0;
  Require("StencilAttachmentAccess", "masked write",
          !stencil_face_accesses_attachment(state, dynamic),
          "fully masked stencil write was classified as access");
  std::printf("[host]    %-32s ok\n", "StencilAttachmentAccess");
}

void CheckDepthTargetFootprints() {
  TileSizeAlign stencil{};
  TileSizeAlign htile{};
  TileSizeAlign depth{};
  Require("DepthTargetFootprints", "640x360 Z32S8 without HTile",
          TileGetDepthSize(640, 360, 0, Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                           Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), false,
                           &stencil, &htile, &depth),
          "valid non-HTile depth/stencil footprint was rejected");
  Require("DepthTargetFootprints", "640x360 Prospero block sizes",
          depth.size == 0xf0000 && depth.align == 0x10000 &&
              stencil.size == 0x60000 && stencil.align == 0x10000 &&
              htile.size == 0 && htile.align == 0,
          "non-HTile depth/stencil footprint disagrees with Prospero block rules");

  const auto depth_pitch = TileGetTexturePitch(
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 640, 1,
      Prospero::GpuEnumValue(Prospero::TileMode::kDepth));
  TileSizeAlign texture_depth{};
  TileGetTextureTotalSize(Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 640, 360,
                          1, depth_pitch, 1,
                          Prospero::GpuEnumValue(Prospero::TileMode::kDepth), false,
                          &texture_depth);
  Require("DepthTargetFootprints", "640x360 generic depth tile",
          depth_pitch == 640 && texture_depth.size == 0xf0000 &&
              texture_depth.align == 0x10000,
          "generic depth texture sizing bypassed 64 KiB block padding");

  Require("DepthTargetFootprints", "960x540 Z32S8 with HTile",
          TileGetDepthSize(960, 540, 0,
                           Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                           Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true,
                           &stencil, &htile, &depth) &&
              depth.size == 0x280000 && depth.align == 0x10000 &&
              stencil.size == 0xc0000 && stencil.align == 0x10000 &&
              htile.size == 0x10000 && htile.align == 0x8000,
          "generic Prospero HTile block calculation rejected the title footprint");
  Require("DepthTargetFootprints", "known HTile extent",
          TileGetDepthSize(1280, 720, 0,
                           Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                           Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true,
                           &stencil, &htile, &depth) &&
              depth.size == 0x3c0000 && stencil.size == 0xf0000 &&
              htile.size == 0x20000,
          "validated 1280x720 HTile footprint regressed");
  Require("DepthTargetFootprints", "PPSA06228 3840x2160 Z32S8 with HTile",
          TileGetDepthSize(3840, 2160, 0,
                           Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                           Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true,
                           &stencil, &htile, &depth) &&
              depth.size == 0x1fe0000 && depth.align == 0x10000 &&
              stencil.size == 0x870000 && stencil.align == 0x10000 &&
              htile.size == 0xa0000 && htile.align == 0x8000,
          "captured 3840x2160 depth/stencil/HTile footprint disagrees with Prospero rules");
  Require("DepthTargetFootprints", "invalid depth format",
          !TileGetDepthSize(960, 540, 0, 2,
                            Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true,
                            &stencil, &htile, &depth),
          "unsupported depth format was silently admitted");
  Require("DepthTargetFootprints", "invalid stencil format",
          !TileGetDepthSize(960, 540, 0,
                            Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F), 2, true,
                            &stencil, &htile, &depth),
          "unsupported stencil format was silently admitted");
  Require("DepthTargetFootprints", "invalid extent",
          !TileGetDepthSize(0, 540, 0,
                            Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                            Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true,
                            &stencil, &htile, &depth) &&
              !TileGetDepthSize(16385, 540, 0,
                                Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                                Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true,
                                &stencil, &htile, &depth),
          "invalid HTile extent was silently admitted");
  std::printf("[host]    %-32s ok\n", "DepthTargetFootprints");
}

void CheckHtileClearTargetResolution() {
  HW::DepthRenderTarget descriptor_backed{};
  descriptor_backed.z_info.format = Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16);
  descriptor_backed.z_info.tile_surface_enable = true;
  descriptor_backed.z_info.zrange_precision = 1;
  descriptor_backed.stencil_info.tile_stencil_disable = true;
  descriptor_backed.z_read_base_addr = 0xc1a80000;
  descriptor_backed.z_write_base_addr = descriptor_backed.z_read_base_addr;
  descriptor_backed.htile_data_base_addr = 0xc1a98000;

  HtileClearTarget resolved{};
  Require("HtileClearTargetResolution", "PPSA09076 descriptor-backed Z16 target",
          ResolveHtileClearTarget(descriptor_backed, 0x8000, &resolved) &&
              resolved.address == 0xc1a98000 && resolved.size == 0x8000,
          "valid descriptor-proven Z16 HTile clear target was rejected");

  auto partial_extent = descriptor_backed;
  partial_extent.size.x_max = 1;
  auto missing_stencil = descriptor_backed;
  missing_stencil.stencil_info.format = Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt);
  auto mismatched_write = descriptor_backed;
  mismatched_write.z_write_base_addr += 0x10000;
  Require("HtileClearTargetResolution", "descriptor-backed rejection boundaries",
          !ResolveHtileClearTarget(descriptor_backed, 0x4000, &resolved) &&
              !ResolveHtileClearTarget(partial_extent, 0x8000, &resolved) &&
              !ResolveHtileClearTarget(missing_stencil, 0x8000, &resolved) &&
              !ResolveHtileClearTarget(mismatched_write, 0x8000, &resolved),
          "ambiguous or inconsistent descriptor-backed HTile target was admitted");

  HW::DepthRenderTarget derived{};
  derived.z_info.format = Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F);
  derived.z_info.tile_surface_enable = true;
  derived.z_info.zrange_precision = 1;
  derived.stencil_info.format = Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt);
  derived.stencil_info.tile_stencil_disable = true;
  derived.z_read_base_addr = 0x100000;
  derived.z_write_base_addr = derived.z_read_base_addr;
  derived.stencil_read_base_addr = 0x500000;
  derived.stencil_write_base_addr = derived.stencil_read_base_addr;
  derived.htile_data_base_addr = 0x700000;
  derived.size.valid = true;
  derived.size.x_max = 959;
  derived.size.y_max = 539;
  TileSizeAlign depth_size{};
  TileSizeAlign stencil_size{};
  TileSizeAlign htile_size{};
  Require("HtileClearTargetResolution", "derived Z32S8 target",
          TileGetDepthSize(960, 540, 0, derived.z_info.format,
                           derived.stencil_info.format, true, &stencil_size,
                           &htile_size, &depth_size) &&
              ResolveHtileClearTarget(derived, htile_size.size, &resolved) &&
              resolved.address == derived.htile_data_base_addr &&
              resolved.size == htile_size.size &&
              !ResolveHtileClearTarget(derived, htile_size.size + 0x8000, &resolved),
          "extent-derived HTile validation or descriptor cross-check regressed");

  auto derived_z16 = derived;
  derived_z16.z_info.format = Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16);
  derived_z16.stencil_info.format = Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid);
  derived_z16.stencil_read_base_addr = 0;
  derived_z16.stencil_write_base_addr = 0;
  Require("HtileClearTargetResolution", "derived Z16 depth-only target",
          TileGetDepthSize(960, 540, 0, derived_z16.z_info.format,
                           derived_z16.stencil_info.format, true, &stencil_size,
                           &htile_size, &depth_size) &&
              stencil_size.align == 0 && stencil_size.size == 0 &&
              ResolveHtileClearTarget(derived_z16, htile_size.size, &resolved) &&
              resolved.address == derived_z16.htile_data_base_addr &&
              resolved.size == htile_size.size,
          "extent-derived Z16 depth-only HTile target was rejected");
  std::printf("[host]    %-32s ok\n", "HtileClearTargetResolution");
}

struct FenceLifetimeProbe {
  explicit FenceLifetimeProbe(bool *destroyed) : destroyed(destroyed) {
    if (destroyed == nullptr || *destroyed) {
      EXIT("fence-lifetime probe has invalid construction state\n");
    }
  }
  ~FenceLifetimeProbe() { *destroyed = true; }

  bool *destroyed = nullptr;
};

void CheckCrossQueueImageLifetime() {
  bool destroyed = false;
  auto image = std::make_shared<FenceLifetimeProbe>(&destroyed);
  FenceResourceRetainer graphics;
  FenceResourceRetainer compute;
  graphics.Retain(image);
  compute.Retain(image);
  graphics.Retain(image);
  image.reset();
  Require("CrossQueueImageLifetime", "retained", !destroyed && !graphics.Empty() &&
              !compute.Empty(),
          "cache removal destroyed an image retained by command buffers");
  graphics.ReleaseAfterFence();
  Require("CrossQueueImageLifetime", "first fence", !destroyed && graphics.Empty() &&
              !compute.Empty(),
          "first command-buffer fence destroyed another queue's image");
  compute.ReleaseAfterFence();
  Require("CrossQueueImageLifetime", "last fence", destroyed && compute.Empty(),
          "last referencing command-buffer fence did not destroy the image");
  std::printf("[host]    %-32s ok\n", "CrossQueueImageLifetime");
}

void CheckHostDmaMetadataReuse() {
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x10000;
  constexpr uint64_t metadata_size = 0x8000;
  auto *memory = static_cast<uint8_t *>(VirtualAlloc(
      reinterpret_cast<void *>(base), allocation_size,
      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("HostDmaMetadataReuse", "allocation",
          memory == reinterpret_cast<void *>(base), "fixed VirtualAlloc failed");
  std::memset(memory + metadata_size, 0x5a, 0x1000);

  ResourceMutex resource_mutex;
  CacheFaultContext fault_context;
  PageManager page_manager(CacheFault, &fault_context);
  BufferCache buffer_cache(page_manager, resource_mutex);
  TextureCache texture_cache(page_manager, buffer_cache, resource_mutex);
  fault_context.texture = &texture_cache;
  buffer_cache.SetTextureCache(texture_cache);
  page_manager.OnGpuMap(base, allocation_size);
  texture_cache.RegisterMeta(base, metadata_size);
  auto *ctx = reinterpret_cast<GraphicContext *>(1);

  Require("HostDmaMetadataReuse", "clear", texture_cache.ClearMeta(base),
          "metadata clear setup failed");
  MEMORY_BASIC_INFORMATION protection{};
  Require("HostDmaMetadataReuse", "protected",
          VirtualQuery(memory, &protection, sizeof(protection)) != 0 &&
              protection.Protect == PAGE_READONLY,
          "virtual metadata was not write-watched");
  Require("HostDmaMetadataReuse", "copy fault",
          page_manager.HandleFault(PageFaultAccess::Write, base),
          "copy destination did not transfer to CPU ownership");
  buffer_cache.CopyBuffer(nullptr, ctx, base, base + metadata_size, 0x1000);
  Require("HostDmaMetadataReuse", "copy",
          std::memcmp(memory, memory + metadata_size, 0x1000) == 0,
          "post-clear CPU DMA copy did not publish backing");
  Require("HostDmaMetadataReuse", "fill fault",
          page_manager.HandleFault(PageFaultAccess::Write, base + 0x1000),
          "fill destination did not transfer to CPU ownership");
  buffer_cache.FillBuffer(nullptr, ctx, base + 0x1000, 0x1000, 0x11223344);
  Require("HostDmaMetadataReuse", "fill",
          reinterpret_cast<uint32_t *>(memory + 0x1000)[0] == 0x11223344 &&
              reinterpret_cast<uint32_t *>(memory + 0x1ffc)[0] == 0x11223344,
          "post-clear CPU DMA fill did not publish backing");
  Require("HostDmaMetadataReuse", "identity",
          texture_cache.HasMetaOverlap(base, metadata_size) &&
              !texture_cache.IsMetaGpuModified(base, 0x2000) &&
              VirtualQuery(memory, &protection, sizeof(protection)) != 0 &&
              protection.Protect == PAGE_READWRITE,
          "CPU writes erased metadata identity or retained virtual ownership");

  texture_cache.UnmapMemory(base, allocation_size);
  page_manager.OnGpuUnmap(base, allocation_size);
  Require("HostDmaMetadataReuse", "free",
          VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
  std::printf("[host]    %-32s ok\n", "HostDmaMetadataReuse");
}
#endif

void CheckEmbeddedFetchLaneSpill() {
  std::vector<u32> code;
  code.push_back(EncodeSMovB32(0, InlineU32(0)));
  code.push_back(EncodeSmem0(0x02u, 20, 4));
  code.push_back(EncodeSmem1(0)); // s_load_dwordx4 s[20:23], s[8:9]
  AppendVop3(&code, 0x361u, 6, 20, InlineU32(0)); // v_writelane_b32 v6, s20, 0
  code.push_back(EncodeSMovB32(20, InlineU32(0)));
  AppendVop3(&code, 0x360u, 20, Vgpr(6), InlineU32(0)); // v_readlane_b32 s20, v6, 0
  code.push_back(EncodeVop2(0x01u, 0, Vgpr(8), 5));
  code.push_back(EncodeMubuf0(0x03u, 0, true));
  code.push_back(EncodeMubuf1(9, 5, 0));
  AppendEnd(&code);

  std::array<u32, 11> user_data{};
  ShaderVertexInputInfo vertex;
  vertex.fetch_embedded = true;
  vertex.fetch_buffer_reg = 0;
  vertex.resources_num = 1;
  vertex.resources_dst[0].attr_id = 0;
  vertex.resources_dst[0].registers_num = 4;

  ShaderRecompiler::CompileOptions options;
  options.stage = ShaderType::Vertex;
  options.user_data_base = 8;
  options.user_data_count = static_cast<u32>(user_data.size());
  options.user_data = user_data.data();
  options.vertex_input_info = &vertex;

  ShaderRecompiler::CompileResult result;
  std::string error;
  Require("EmbeddedFetchLaneSpill", "compile",
          ShaderRecompiler::TryRecompile(code, options, &result, &error), error);
  Require("EmbeddedFetchLaneSpill", "fetch rewrite",
          vertex.resource_fetch_components[0] == 4,
          "lane-spilled fetch descriptor was not recognized and rewritten");
  std::printf("[host]    %-32s ok\n", "EmbeddedFetchLaneSpill");
}

} // namespace
} // namespace Libs::Graphics

int main(int argc, char **argv) {
  using namespace Libs::Graphics;

  EnsureConfigInitialized();
  TileInit();
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  if (argc == 2 && std::strcmp(argv[1], "--image-overlap-only") == 0) {
    CheckImageOverlapResolution();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--htile-clear-only") == 0) {
    CheckHtileClearTargetResolution();
    CheckOverlappingMetadataViews();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--image-view-only") == 0) {
    CheckSampledColorViews();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--sampled-depth-resource-only") == 0) {
    CheckSampledDepthResource();
    CheckSampledDepthDescriptor();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--buffer-cache-range-only") == 0) {
    CheckBufferCacheRangeMerge();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--storage-bgra-only") == 0) {
    CheckSampledColorViews();
    CheckBasicStorageTextureDescriptor();
    VulkanHarness vulkan;
    vulkan.CheckMutableRenderTargetBgraStorageView();
    RunCase(&vulkan, ImageStoreBgraUsesInverseSwizzle());
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--storage-yzwx-only") == 0) {
    CheckSampledColorViews();
    CheckBasicStorageTextureDescriptor();
    CheckStorageTextureGpuOwnedRebindState();
    VulkanHarness vulkan;
    RunCase(&vulkan, ImageStoreYzwxUsesInverseSwizzle());
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--storage-sampled-only") == 0) {
    CheckStorageTextureSampledReuse();
    VulkanHarness vulkan;
    vulkan.CheckMutableStorageSrgbView();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--depth-readback-only") == 0) {
    CheckDepthTargetTileRoundTrip();
    CheckDepthTargetFootprints();
    return 0;
  }
  if (argc == 3 && std::strcmp(argv[1], "--image-view-death") == 0) {
    RunImageViewDeathCase(argv[2]);
  }
  if (argc == 3 &&
      std::strcmp(argv[1], "--storage-texture-descriptor-death") == 0) {
    RunStorageTextureDescriptorDeathCase(argv[2]);
  }
  if (argc == 3 && std::strcmp(argv[1], "--storage-texture-access-death") == 0) {
    RunStorageTextureAccessDeathCase(argv[2]);
  }
  if (argc == 3 && std::strcmp(argv[1], "--meta-overlap-death") == 0) {
    RunMetaOverlapDeathCase(argv[2]);
  }
  if (argc == 3 && std::strcmp(argv[1], "--metadata-descriptor-death") == 0) {
    RunMetadataDescriptorDeathCase(argv[2]);
  }
  CheckSampledColorViews();
  CheckSampledDepthResource();
  CheckSampledDepthDescriptor();
  CheckBufferCacheRangeMerge();
  CheckBasicStorageTextureDescriptor();
  CheckStorageTextureLinearUploadLayout();
  CheckStorageTextureLinearReadbackLayout();
  CheckStorageImageSwizzleSpecializationId();
  CheckRenderTargetTileRoundTrip();
  CheckDepthTargetTileRoundTrip();
  CheckStorageTextureVolumeUploadLayout();
  CheckStorageTextureGpuOwnedRebindState();
  CheckStorageTextureSampledReuse();
  CheckStorageTextureAccessPermissions();
  CheckMetaOverlapDeaths();
  CheckOverlappingMetadataViews();
  CheckMetadataReuseDescriptors();
  CheckImageOverlapResolution();
  CheckMsaaCompatibility();
  CheckDepthHtileStencilCompatibility();
  CheckStencilAttachmentAccess();
  CheckDepthTargetFootprints();
  CheckHtileClearTargetResolution();
  CheckCrossQueueImageLifetime();
  CheckHostDmaMetadataReuse();
#else
  (void)argc;
  (void)argv;
#endif
  CheckEmbeddedFetchVertexOffset();
  CheckEmbeddedFetchLaneSpill();
  CheckPs5GameExampleImageClearRuntimeShape();
  VulkanHarness vulkan;
  vulkan.CheckMutableStorageSrgbView();
  vulkan.CheckMutableRenderTargetBgraStorageView();
  const auto tests = MakeCases();
  const auto graphics_tests = MakeGraphicsCases();
  CheckOpcodeCoverage(tests, graphics_tests);
  for (const auto &test : tests) {
    RunCase(&vulkan, test);
  }
  for (const auto &test : MakeSkippedCases()) {
    std::printf("[skip]    %-32s %s\n", test.name, test.reason);
  }
  for (const auto &test : graphics_tests) {
    RunGraphicsCase(&vulkan, test);
  }
  std::printf("ShaderRecompilerComputeTests: all cases passed\n");
  return 0;
}
