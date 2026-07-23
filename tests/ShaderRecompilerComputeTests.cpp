#include "common/threads.h"

#include "common/assert.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "graphics/guest_gpu/command_processor/commandProcessor.h"
#include "graphics/guest_gpu/command_processor/pm4Dispatch.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/gpu_format.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/pm4.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/gpuTiler.h"
#include "graphics/host_gpu/hostMemory.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/pageManager.h"
#include "graphics/host_gpu/renderer/bufferCache.h"
#include "graphics/host_gpu/renderer/colorRenderTarget.h"
#include "graphics/host_gpu/renderer/depthRenderTarget.h"
#include "graphics/host_gpu/renderer/descriptors.h"
#include "graphics/host_gpu/renderer/image.h"
#include "graphics/host_gpu/renderer/imageView.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderDraw.h"
#include "graphics/host_gpu/renderer/renderTarget.h"
#include "graphics/host_gpu/renderer/resourceMutex.h"
#include "graphics/host_gpu/renderer/sync.h"
#include "graphics/host_gpu/renderer/textureCache.h"
#include "graphics/host_gpu/renderer/tiler.h"
#include "graphics/host_gpu/transfer.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/shader/recompiler/BindingLayout.h"
#include "graphics/shader/recompiler/ShaderDecoder.h"
#include "graphics/shader/recompiler/ShaderRecompiler.h"
#include "graphics/shader/recompiler/SpirvBuilder.h"
#include "graphics/shader/shader.h"

#include "spirv-tools/libspirv.hpp"

#include <algorithm>
#include <array>
#include <bit>
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

std::string VulkanResultName(vk::Result result) {
  return VulkanToString(result);
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

void RequireVk(const char *shader_name, const char *stage, vk::Result result,
               const char *action) {
  if (result != vk::Result::eSuccess) {
    Fail(shader_name, stage,
         std::string(action) + " returned " + VulkanResultName(result));
  }
}

void Require(const char *shader_name, const char *stage, bool value,
             const std::string &message) {
  if (!value) {
    Fail(shader_name, stage, message);
  }
}

bool RejectUnexpectedPageFault(void *, PageFaultAccess, uint64_t, uint64_t,
                               PageFaultPhase) noexcept {
  return false;
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
  vk::Format sampled_image_format = vk::Format::eR32G32B32A32Sfloat;
  u32 sampled_image_dwords_per_pixel = 4;
  std::vector<u32> storage_image_rgba;
  std::vector<u32> expected_storage_image_rgba;
  vk::Format storage_image_format = vk::Format::eR32G32B32A32Sfloat;
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
  if (!ShaderRecompiler::TryRecompile(test.code, options, result, &error)) {
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
  if (!ShaderRecompiler::TryRecompile(test.fragment_code, options, result,
                                      &error)) {
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
    vk::Buffer buffer = nullptr;
    vk::DeviceMemory memory = nullptr;
    vk::DeviceSize size = 0;
    bool coherent = false;
  };

  struct Image {
    vk::Image image = nullptr;
    vk::DeviceMemory memory = nullptr;
    vk::ImageView view = nullptr;
    vk::Format format = vk::Format::eUndefined;
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    u32 width = 0;
    u32 height = 0;
    u32 mip_levels = 1;
    u32 dwords_per_pixel = 0;
  };

  [[nodiscard]] vk::Device Device() const { return m_device; }

  void CheckCommandPoolGrowth() {
    EnsureRuntimeContext();
    std::array<CommandBuffer, 12> commands;
    for (auto &command : commands) {
      Require("CommandPoolGrowth", "allocation", !command.IsInvalid(),
              "unified command pool failed to grow");
      command.Begin();
      command.End();
      command.Execute();
    }
    for (auto &command : commands) {
      command.WaitForFence();
    }
    std::printf("[host]    %-32s ok\n", "CommandPoolGrowth");
  }

  void CheckMutableStorageSrgbView() {
    constexpr const char *name = "StorageTextureMutableSrgbView";
    vk::ImageCreateInfo image_info{};
    image_info.sType = vk::StructureType::eImageCreateInfo;
    image_info.flags = vk::ImageCreateFlagBits::eMutableFormat;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eR8G8B8A8Srgb;
    image_info.extent = {16, 16, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst |
                       vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eStorage;
    image_info.sharingMode = vk::SharingMode::eExclusive;
    image_info.initialLayout = vk::ImageLayout::eUndefined;

    vk::ImageFormatProperties exact_properties{};
    const bool srgb_storage_supported =
        m_physical_device.getImageFormatProperties(
            image_info.format, image_info.imageType, image_info.tiling,
            image_info.usage, image_info.flags,
            &exact_properties) == vk::Result::eSuccess;
    GraphicContext context{};
    context.physical_device = m_physical_device;
    Require(name, "format fallback", TextureCheckFormat(image_info),
            "mutable sampled/storage RGBA8 image is unsupported");
    Require(name, "format fallback",
            image_info.format == vk::Format::eR8G8B8A8Unorm ||
                (srgb_storage_supported &&
                 image_info.format == vk::Format::eR8G8B8A8Srgb),
            "unsupported sRGB storage format did not fall back to UNORM");

    vk::Image image = nullptr;
    RequireVk(name, "backing",
              m_device.createImage(&image_info, nullptr, &image),
              "vkCreateImage");
    vk::MemoryRequirements requirements{};
    m_device.getImageMemoryRequirements(image, &requirements);
    u32 memory_type = 0;
    Require(name, "backing",
            FindMemoryType(requirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal,
                           &memory_type) ||
                FindMemoryType(requirements.memoryTypeBits, {}, &memory_type),
            "no memory type for mutable storage image");
    vk::MemoryAllocateInfo allocation{};
    allocation.sType = vk::StructureType::eMemoryAllocateInfo;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    vk::DeviceMemory memory = nullptr;
    RequireVk(name, "backing",
              m_device.allocateMemory(&allocation, nullptr, &memory),
              "vkAllocateMemory");
    RequireVk(name, "backing", m_device.bindImageMemory(image, memory, 0),
              "vkBindImageMemory");

    vk::ImageViewUsageCreateInfo view_usage{};
    view_usage.sType = vk::StructureType::eImageViewUsageCreateInfo;
    view_usage.usage =
        vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage;
    vk::ImageViewCreateInfo view_info{};
    view_info.sType = vk::StructureType::eImageViewCreateInfo;
    view_info.pNext = &view_usage;
    view_info.image = image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = image_info.format;
    view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    vk::ImageView default_view = nullptr;
    RequireVk(name, "default view",
              m_device.createImageView(&view_info, nullptr, &default_view),
              "vk::Device::createImageView(default sampled/storage)");

    view_usage.usage = vk::ImageUsageFlagBits::eSampled;
    view_info.format = vk::Format::eR8G8B8A8Srgb;
    vk::ImageView srgb_view = nullptr;
    RequireVk(name, "sRGB view",
              m_device.createImageView(&view_info, nullptr, &srgb_view),
              "vk::Device::createImageView(sRGB sampled)");

    m_device.destroyImageView(srgb_view, nullptr);
    m_device.destroyImageView(default_view, nullptr);
    m_device.destroyImage(image, nullptr);
    m_device.freeMemory(memory, nullptr);
    std::printf("[host]    %-32s ok (backing=%d)\n", name,
                static_cast<int>(image_info.format));
  }

  void CheckMutableRenderTargetBgraStorageView() {
    constexpr const char *name = "RenderTargetMutableBgraStorage";
    vk::ImageCreateInfo image_info{};
    image_info.sType = vk::StructureType::eImageCreateInfo;
    image_info.flags = vk::ImageCreateFlagBits::eMutableFormat |
                       vk::ImageCreateFlagBits::eExtendedUsage;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eB8G8R8A8Srgb;
    image_info.extent = {8, 8, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eColorAttachment |
                       vk::ImageUsageFlagBits::eTransferSrc |
                       vk::ImageUsageFlagBits::eTransferDst |
                       vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eStorage;
    image_info.sharingMode = vk::SharingMode::eExclusive;
    image_info.initialLayout = vk::ImageLayout::eUndefined;

    vk::ImageFormatProperties properties{};
    RequireVk(name, "backing query",
              m_physical_device.getImageFormatProperties(
                  image_info.format, image_info.imageType, image_info.tiling,
                  image_info.usage, image_info.flags, &properties),
              "mutable extended-usage BGRA sRGB backing");

    vk::Image image = nullptr;
    RequireVk(name, "backing",
              m_device.createImage(&image_info, nullptr, &image),
              "vkCreateImage");
    vk::MemoryRequirements requirements{};
    m_device.getImageMemoryRequirements(image, &requirements);
    u32 memory_type = 0;
    Require(name, "backing",
            FindMemoryType(requirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal,
                           &memory_type) ||
                FindMemoryType(requirements.memoryTypeBits, {}, &memory_type),
            "no memory type for mutable render target");
    vk::MemoryAllocateInfo allocation{};
    allocation.sType = vk::StructureType::eMemoryAllocateInfo;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    vk::DeviceMemory memory = nullptr;
    RequireVk(name, "backing",
              m_device.allocateMemory(&allocation, nullptr, &memory),
              "vkAllocateMemory");
    RequireVk(name, "backing", m_device.bindImageMemory(image, memory, 0),
              "vkBindImageMemory");

    vk::ImageViewUsageCreateInfo view_usage{};
    view_usage.sType = vk::StructureType::eImageViewUsageCreateInfo;
    view_usage.usage = vk::ImageUsageFlagBits::eStorage;
    vk::ImageViewCreateInfo view_info{};
    view_info.sType = vk::StructureType::eImageViewCreateInfo;
    view_info.pNext = &view_usage;
    view_info.image = image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = vk::Format::eR8G8B8A8Unorm;
    view_info.components = {
        vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
        vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity};
    view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    vk::ImageView storage_view = nullptr;
    RequireVk(name, "storage view",
              m_device.createImageView(&view_info, nullptr, &storage_view),
              "vk::Device::createImageView(RGBA UNORM storage)");

    m_device.destroyImageView(storage_view, nullptr);
    m_device.destroyImage(image, nullptr);
    m_device.freeMemory(memory, nullptr);
    std::printf("[host]    %-32s ok (backing=%d view=%d)\n", name,
                static_cast<int>(image_info.format),
                static_cast<int>(view_info.format));
  }

  void CheckRenderTargetViewCache() {
    constexpr const char *name = "RenderTargetViewCache";
    vk::ImageCreateInfo image_info{};
    image_info.sType = vk::StructureType::eImageCreateInfo;
    image_info.flags = vk::ImageCreateFlagBits::eMutableFormat;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eR8G8B8A8Unorm;
    image_info.extent = {8, 8, 1};
    image_info.mipLevels = 2;
    image_info.arrayLayers = 2;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eColorAttachment |
                       vk::ImageUsageFlagBits::eSampled |
                       vk::ImageUsageFlagBits::eStorage;
    image_info.sharingMode = vk::SharingMode::eExclusive;

    vk::Image image_handle = nullptr;
    RequireVk(name, "image",
              m_device.createImage(&image_info, nullptr, &image_handle),
              "vkCreateImage");
    vk::MemoryRequirements requirements{};
    m_device.getImageMemoryRequirements(image_handle, &requirements);
    u32 memory_type = 0;
    Require(name, "memory",
            FindMemoryType(requirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal,
                           &memory_type) ||
                FindMemoryType(requirements.memoryTypeBits, {}, &memory_type),
            "no memory type for sampled render target");
    vk::MemoryAllocateInfo allocation{};
    allocation.sType = vk::StructureType::eMemoryAllocateInfo;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    vk::DeviceMemory memory = nullptr;
    RequireVk(name, "memory",
              m_device.allocateMemory(&allocation, nullptr, &memory),
              "vkAllocateMemory");
    RequireVk(name, "memory", m_device.bindImageMemory(image_handle, memory, 0),
              "vkBindImageMemory");

    EnsureRuntimeContext();
    ResourceMutex resource_mutex;
    PageManager page_manager(RejectUnexpectedPageFault, nullptr);
    BufferCache buffer_cache(m_runtime_context, page_manager, resource_mutex);
    TextureCache texture_cache(m_runtime_context, page_manager, buffer_cache,
                               resource_mutex);
    buffer_cache.SetTextureCache(texture_cache);
    RenderTextureVulkanImage image;
    image.image = image_handle;
    image.format = image_info.format;
    image.extent = {image_info.extent.width, image_info.extent.height};
    image.layers = 2;
    image.mip_levels = 2;

    constexpr auto first_swizzle = DstSel(5, 1, 7, 0);
    constexpr auto second_swizzle = DstSel(7, 4, 0, 1);
    const auto first = texture_cache.GetSampledColorView(
        image, image.format, first_swizzle, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto first_again = texture_cache.GetSampledColorView(
        image, image.format, first_swizzle, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto second =
        texture_cache.GetSampledColorView(image, image.format, second_swizzle,
                                          0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto different_mip = texture_cache.GetSampledColorView(
        image, image.format, first_swizzle, 1, 1, vk::ImageViewType::e2D, 0, 1);
    const auto different_layer = texture_cache.GetSampledColorView(
        image, image.format, first_swizzle, 0, 1, vk::ImageViewType::e2D, 1, 1);
    const auto array_view =
        texture_cache.GetSampledColorView(image, image.format, first_swizzle, 0,
                                          1, vk::ImageViewType::e2DArray, 0, 2);
    constexpr auto identity = DstSel(4, 5, 6, 7);
    const auto native_format = texture_cache.GetSampledColorView(
        image, image.format, identity, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto reinterpreted_format = texture_cache.GetSampledColorView(
        image, vk::Format::eR8G8B8A8Uint, identity, 0, 1,
        vk::ImageViewType::e2D, 0, 1);
    const auto storage = texture_cache.GetRenderTargetStorageView(
        image, image.format, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto storage_again = texture_cache.GetRenderTargetStorageView(
        image, image.format, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto attachment = texture_cache.GetRenderTargetAttachmentView(
        image, image.format, 0, 0, 1);
    const auto attachment_again = texture_cache.GetRenderTargetAttachmentView(
        image, image.format, 0, 0, 1);
    const auto attachment_mip = texture_cache.GetRenderTargetAttachmentView(
        image, image.format, 1, 0, 1);
    const auto attachment_array = texture_cache.GetRenderTargetAttachmentView(
        image, image.format, 0, 0, 2);
    Require(name, "cache identity",
            first != nullptr && first_again == first && second != nullptr &&
                second != first && different_mip != nullptr &&
                different_mip != first && different_layer != nullptr &&
                different_layer != first && array_view != nullptr &&
                array_view != first && native_format != nullptr &&
                reinterpreted_format != nullptr &&
                reinterpreted_format != native_format && storage != nullptr &&
                storage != native_format && storage_again == storage &&
                attachment != nullptr && attachment_again == attachment &&
                attachment != native_format && attachment != storage &&
                attachment_mip != nullptr && attachment_mip != attachment &&
                attachment_array != nullptr && attachment_array != attachment &&
                image.view_cache.views.size() == 11,
            "view cache omitted attachment usage, swizzle, format, type, mip, "
            "or layer identity");

    ImageViewOps::DestroyViews(image);
    Require(name, "view teardown", image.view_cache.views.empty(),
            "dynamic render-target views survived DestroyViews");
    m_device.destroyImage(image_handle, nullptr);
    m_device.freeMemory(memory, nullptr);
    std::printf("[host]    %-32s ok\n", name);
  }

  void CheckDepthTargetSampledViewCache() {
    constexpr const char *name = "DepthTargetSampledViewCache";
    vk::ImageCreateInfo image_info{};
    image_info.sType = vk::StructureType::eImageCreateInfo;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = vk::Format::eD32Sfloat;
    image_info.extent = {8, 8, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 2;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                       vk::ImageUsageFlagBits::eSampled;
    image_info.sharingMode = vk::SharingMode::eExclusive;

    vk::Image image_handle = nullptr;
    RequireVk(name, "image",
              m_device.createImage(&image_info, nullptr, &image_handle),
              "vkCreateImage");
    vk::MemoryRequirements requirements{};
    m_device.getImageMemoryRequirements(image_handle, &requirements);
    u32 memory_type = 0;
    Require(name, "memory",
            FindMemoryType(requirements.memoryTypeBits,
                           vk::MemoryPropertyFlagBits::eDeviceLocal,
                           &memory_type) ||
                FindMemoryType(requirements.memoryTypeBits, {}, &memory_type),
            "no memory type for sampled depth target");
    vk::MemoryAllocateInfo allocation{};
    allocation.sType = vk::StructureType::eMemoryAllocateInfo;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    vk::DeviceMemory memory = nullptr;
    RequireVk(name, "memory",
              m_device.allocateMemory(&allocation, nullptr, &memory),
              "vkAllocateMemory");
    RequireVk(name, "memory", m_device.bindImageMemory(image_handle, memory, 0),
              "vkBindImageMemory");

    EnsureRuntimeContext();
    ResourceMutex resource_mutex;
    PageManager page_manager(RejectUnexpectedPageFault, nullptr);
    BufferCache buffer_cache(m_runtime_context, page_manager, resource_mutex);
    TextureCache texture_cache(m_runtime_context, page_manager, buffer_cache,
                               resource_mutex);
    buffer_cache.SetTextureCache(texture_cache);
    DepthStencilVulkanImage image;
    image.image = image_handle;
    image.format = image_info.format;
    image.extent = {image_info.extent.width, image_info.extent.height};
    image.layers = image_info.arrayLayers;
    image.mip_levels = image_info.mipLevels;

    constexpr auto replicated = DstSel(4, 4, 4, 4);
    constexpr auto r001 = DstSel(4, 0, 0, 1);
    const auto first = texture_cache.GetDepthTargetSampledView(
        image, vk::Format::eR32Sfloat, replicated, 0, 1, vk::ImageViewType::e2D,
        0, 1);
    const auto first_again = texture_cache.GetDepthTargetSampledView(
        image, vk::Format::eR32Sfloat, replicated, 0, 1, vk::ImageViewType::e2D,
        0, 1);
    const auto different_swizzle = texture_cache.GetDepthTargetSampledView(
        image, vk::Format::eR32Sfloat, r001, 0, 1, vk::ImageViewType::e2D, 0,
        1);
    const auto different_layer = texture_cache.GetDepthTargetSampledView(
        image, vk::Format::eR32Sfloat, replicated, 0, 1, vk::ImageViewType::e2D,
        1, 1);
    const auto array_view = texture_cache.GetDepthTargetSampledView(
        image, vk::Format::eR32Sfloat, replicated, 0, 1,
        vk::ImageViewType::e2DArray, 0, 2);
    const auto attachment =
        texture_cache.GetDepthTargetAttachmentView(image, 0, 1);
    const auto attachment_again =
        texture_cache.GetDepthTargetAttachmentView(image, 0, 1);
    const auto attachment_layer =
        texture_cache.GetDepthTargetAttachmentView(image, 1, 1);
    const auto attachment_array =
        texture_cache.GetDepthTargetAttachmentView(image, 0, 2);
    Require(
        name, "cache identity",
        first != nullptr && first_again == first &&
            different_swizzle != nullptr && different_swizzle != first &&
            different_layer != nullptr && different_layer != first &&
            array_view != nullptr && array_view != first &&
            attachment != nullptr && attachment_again == attachment &&
            attachment != first && attachment_layer != nullptr &&
            attachment_layer != attachment && attachment_array != nullptr &&
            attachment_array != attachment &&
            image.view_cache.views.size() == 7,
        "depth view cache omitted attachment usage, swizzle, type, or layer "
        "identity");

    ImageViewOps::DestroyViews(image);
    Require(name, "view teardown", image.view_cache.views.empty(),
            "dynamic depth-target views survived DestroyViews");
    m_device.destroyImage(image_handle, nullptr);
    m_device.freeMemory(memory, nullptr);
    std::printf("[host]    %-32s ok\n", name);
  }

  void CheckVideoOutSampledViewCache() {
    constexpr const char *name = "VideoOutSampledViewCache";
    auto backing =
        CreateImage2D(name, 8, 8, vk::Format::eR8G8B8A8Unorm,
                      vk::ImageUsageFlagBits::eColorAttachment |
                          vk::ImageUsageFlagBits::eSampled,
                      {}, 1, vk::ImageLayout::eShaderReadOnlyOptimal);

    EnsureRuntimeContext();
    ResourceMutex resource_mutex;
    PageManager page_manager(RejectUnexpectedPageFault, nullptr);
    BufferCache buffer_cache(m_runtime_context, page_manager, resource_mutex);
    TextureCache texture_cache(m_runtime_context, page_manager, buffer_cache,
                               resource_mutex);
    buffer_cache.SetTextureCache(texture_cache);
    VideoOutVulkanImage image;
    image.image = backing.image;
    image.format = backing.format;
    image.extent = {backing.width, backing.height};
    image.layers = 1;
    image.mip_levels = 1;
    image.image_view[VulkanImage::VIEW_DEFAULT] = backing.view;

    constexpr auto identity = DstSel(4, 5, 6, 7);
    constexpr auto bgra = DstSel(6, 5, 4, 7);
    constexpr auto uncommon = DstSel(5, 1, 7, 0);
    const auto default_view = texture_cache.GetSampledColorView(
        image, image.format, identity, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto bgra_view = texture_cache.GetSampledColorView(
        image, image.format, bgra, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto uncommon_view = texture_cache.GetSampledColorView(
        image, image.format, uncommon, 0, 1, vk::ImageViewType::e2D, 0, 1);
    const auto uncommon_again = texture_cache.GetSampledColorView(
        image, image.format, uncommon, 0, 1, vk::ImageViewType::e2D, 0, 1);
    Require(name, "cache identity",
            default_view == backing.view && bgra_view != nullptr &&
                bgra_view != default_view && uncommon_view != nullptr &&
                uncommon_view != bgra_view && uncommon_again == uncommon_view &&
                image.view_cache.views.size() == 2,
            "video-out sampled views did not use identity fast path and lazy "
            "mappings");

    ImageViewOps::DestroyViews(image);
    Require(name, "view teardown",
            image.view_cache.views.empty() &&
                std::all_of(std::begin(image.image_view),
                            std::end(image.image_view),
                            [](vk::ImageView view) { return view == nullptr; }),
            "dynamic or fixed video-out views survived DestroyViews");
    backing.view = nullptr;
    DestroyImage(&backing);
    std::printf("[host]    %-32s ok\n", name);
  }

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  void CheckQueryRegionImageClassification() {
    constexpr const char *name = "QueryRegionImageClassification";
    constexpr uintptr_t base = 0x0000000200100000ull;
    constexpr uint64_t allocation_size = 0x4000;
    auto *memory = static_cast<uint8_t *>(
        VirtualAlloc(reinterpret_cast<void *>(base), allocation_size,
                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Require(name, "allocation", memory == reinterpret_cast<void *>(base),
            "fixed VirtualAlloc failed");
    std::memset(memory, 0, allocation_size);

    EnsureRuntimeContext();
    ResourceMutex resource_mutex;
    PageManager page_manager(RejectUnexpectedPageFault, nullptr);
    BufferCache buffer_cache(m_runtime_context, page_manager, resource_mutex);
    TextureCache texture_cache(m_runtime_context, page_manager, buffer_cache,
                               resource_mutex);
    buffer_cache.SetTextureCache(texture_cache);
    page_manager.OnGpuMap(base, allocation_size);

    constexpr auto format =
        Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm);
    constexpr auto linear = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
    ImageInfo sampled_info{};
    sampled_info.address = base + 0x100;
    sampled_info.format = format;
    sampled_info.width = 4;
    sampled_info.height = 4;
    sampled_info.pitch =
        TileGetTexturePitch(format, sampled_info.width, 1, linear);
    sampled_info.tile = linear;
    sampled_info.type = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
    TileSizeAlign sampled_size{};
    TileGetTextureTotalSize(format, sampled_info.width, sampled_info.height,
                            sampled_info.depth, sampled_info.pitch,
                            sampled_info.levels, linear, false, sampled_size);
    sampled_info.size = sampled_size.size;
    Require(name, "sampled fixture",
            sampled_info.size != 0 && sampled_size.align != 0 &&
                sampled_info.size <= 0x600 &&
                sampled_info.address % sampled_size.align == 0,
            "sampled image does not fit the intended sub-page fixture");

    RenderTargetInfo target_info{};
    target_info.address = base + 0x1100;
    target_info.size = 0x100;
    target_info.format = vk::Format::eR8G8B8A8Unorm;
    target_info.width = 4;
    target_info.height = 4;
    target_info.pitch = 4;
    target_info.bytes_per_element = 4;
    target_info.tile_mode = linear;

    {
      CommandBuffer command;
      (void)texture_cache.FindTexture(command, sampled_info, false);
      auto& target = texture_cache.FindRenderTarget(command, target_info);

      const auto sampled_bytes =
          texture_cache.QueryRegion(sampled_info.address + 0x20, 0x20);
      const auto sampled_page = texture_cache.QueryRegion(base + 0x800, 0x20);
      Require(name, "sampled classification",
              sampled_bytes.image_pages && sampled_bytes.image_bytes &&
                  !sampled_bytes.gpu_image_bytes &&
                  !sampled_bytes.non_sampled_pages &&
                  sampled_page.image_pages && !sampled_page.image_bytes &&
                  !sampled_page.gpu_image_bytes &&
                  !sampled_page.non_sampled_pages,
              "sampled image page and byte ownership were not separated");

      const auto target_bytes =
          texture_cache.QueryRegion(target_info.address + 0x20, 0x20);
      const auto target_page =
          texture_cache.QueryRegion(target_info.address + 0x400, 0x20);
      Require(name, "non-sampled classification",
              target_bytes.image_pages && target_bytes.image_bytes &&
                  !target_bytes.gpu_image_bytes &&
                  target_bytes.non_sampled_pages && target_page.image_pages &&
                  !target_page.image_bytes && !target_page.gpu_image_bytes &&
                  target_page.non_sampled_pages,
              "non-sampled image page and byte ownership were not separated");

      texture_cache.MarkGpuWritten(target);
      const auto gpu_bytes =
          texture_cache.QueryRegion(target_info.address + 0x20, 0x20);
      const auto gpu_page =
          texture_cache.QueryRegion(target_info.address + 0x400, 0x20);
      Require(name, "GPU ownership",
              gpu_bytes.image_pages && gpu_bytes.image_bytes &&
                  gpu_bytes.gpu_image_bytes && gpu_bytes.non_sampled_pages &&
                  gpu_page.image_pages && !gpu_page.image_bytes &&
                  !gpu_page.gpu_image_bytes && gpu_page.non_sampled_pages,
              "GPU ownership escaped the target's exact byte range");

      texture_cache.UnmapMemory(base, allocation_size);
      page_manager.OnGpuUnmap(base, allocation_size);
    }
    Require(name, "free", VirtualFree(memory, 0, MEM_RELEASE) != 0,
            "VirtualFree failed");
    std::printf("[host]    %-32s ok\n", name);
  }
#endif

  Buffer CreateStorageBuffer(const char *shader_name,
                             const std::vector<u32> &initial,
                             size_t dword_count) {
    Buffer ret;
    ret.size = static_cast<vk::DeviceSize>(std::max<size_t>(dword_count, 1u) *
                                           sizeof(u32));

    vk::BufferCreateInfo buffer_info{};
    buffer_info.sType = vk::StructureType::eBufferCreateInfo;
    buffer_info.size = ret.size;
    buffer_info.usage = vk::BufferUsageFlagBits::eStorageBuffer;
    buffer_info.sharingMode = vk::SharingMode::eExclusive;
    RequireVk(shader_name, "dispatch",
              m_device.createBuffer(&buffer_info, nullptr, &ret.buffer),
              "vkCreateBuffer");

    vk::MemoryRequirements req{};
    m_device.getBufferMemoryRequirements(ret.buffer, &req);
    u32 memory_type = 0;
    ret.coherent = FindMemoryType(req.memoryTypeBits,
                                  vk::MemoryPropertyFlagBits::eHostVisible |
                                      vk::MemoryPropertyFlagBits::eHostCoherent,
                                  &memory_type);
    if (!ret.coherent) {
      Require(shader_name, "dispatch",
              FindMemoryType(req.memoryTypeBits,
                             vk::MemoryPropertyFlagBits::eHostVisible,
                             &memory_type),
              "no host-visible memory type for storage buffer");
    }

    vk::MemoryAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eMemoryAllocateInfo;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type;
    RequireVk(shader_name, "dispatch",
              m_device.allocateMemory(&alloc, nullptr, &ret.memory),
              "vkAllocateMemory");
    RequireVk(shader_name, "dispatch",
              m_device.bindBufferMemory(ret.buffer, ret.memory, 0),
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
    if (buffer->buffer != nullptr) {
      m_device.destroyBuffer(buffer->buffer, nullptr);
      buffer->buffer = nullptr;
    }
    if (buffer->memory != nullptr) {
      m_device.freeMemory(buffer->memory, nullptr);
      buffer->memory = nullptr;
    }
  }

  Image CreateImage2D(const char *shader_name, u32 width, u32 height,
                      vk::Format format, vk::ImageUsageFlags usage,
                      const std::vector<u32> &initial, u32 dwords_per_pixel,
                      vk::ImageLayout final_layout) {
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
                          vk::Format format, vk::ImageUsageFlags usage,
                          const std::vector<std::vector<u32>> &initial_mips,
                          u32 dwords_per_pixel, vk::ImageLayout final_layout) {
    Image ret;
    ret.format = format;
    ret.width = width;
    ret.height = height;
    ret.mip_levels = std::max<u32>(static_cast<u32>(initial_mips.size()), 1u);
    ret.dwords_per_pixel = dwords_per_pixel;

    vk::ImageCreateInfo image_info{};
    image_info.sType = vk::StructureType::eImageCreateInfo;
    image_info.imageType = vk::ImageType::e2D;
    image_info.format = format;
    image_info.extent.width = width;
    image_info.extent.height = height;
    image_info.extent.depth = 1;
    image_info.mipLevels = ret.mip_levels;
    image_info.arrayLayers = 1;
    image_info.samples = vk::SampleCountFlagBits::e1;
    image_info.tiling = vk::ImageTiling::eOptimal;
    image_info.usage = usage | vk::ImageUsageFlagBits::eTransferDst |
                       vk::ImageUsageFlagBits::eTransferSrc;
    image_info.sharingMode = vk::SharingMode::eExclusive;
    image_info.initialLayout = vk::ImageLayout::eUndefined;
    RequireVk(shader_name, "dispatch",
              m_device.createImage(&image_info, nullptr, &ret.image),
              "vkCreateImage");

    vk::MemoryRequirements req{};
    m_device.getImageMemoryRequirements(ret.image, &req);
    u32 memory_type = 0;
    if (!FindMemoryType(req.memoryTypeBits,
                        vk::MemoryPropertyFlagBits::eDeviceLocal,
                        &memory_type)) {
      Require(shader_name, "dispatch",
              FindMemoryType(req.memoryTypeBits, {}, &memory_type),
              "no memory type for image");
    }

    vk::MemoryAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eMemoryAllocateInfo;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type;
    RequireVk(shader_name, "dispatch",
              m_device.allocateMemory(&alloc, nullptr, &ret.memory),
              "vk::Device::allocateMemory(image)");
    RequireVk(shader_name, "dispatch",
              m_device.bindImageMemory(ret.image, ret.memory, 0),
              "vkBindImageMemory");

    vk::ImageViewCreateInfo view_info{};
    view_info.sType = vk::StructureType::eImageViewCreateInfo;
    view_info.image = ret.image;
    view_info.viewType = vk::ImageViewType::e2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = ret.mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    RequireVk(shader_name, "dispatch",
              m_device.createImageView(&view_info, nullptr, &ret.view),
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
                           vk::BufferUsageFlagBits::eTransferSrc, contents);
      UploadImageMips(shader_name, &ret, staging.buffer, final_layout);
      DestroyBuffer(&staging);
    } else {
      TransitionImage(shader_name, &ret, final_layout,
                      vk::PipelineStageFlagBits::eTopOfPipe,
                      vk::PipelineStageFlagBits::eComputeShader, {},
                      AccessForLayout(final_layout));
    }
    return ret;
  }

  void DestroyImage(Image *image) {
    if (image == nullptr) {
      return;
    }
    if (image->view != nullptr) {
      m_device.destroyImageView(image->view, nullptr);
      image->view = nullptr;
    }
    if (image->image != nullptr) {
      m_device.destroyImage(image->image, nullptr);
      image->image = nullptr;
    }
    if (image->memory != nullptr) {
      m_device.freeMemory(image->memory, nullptr);
      image->memory = nullptr;
    }
  }

  std::vector<u32> ReadImage(const char *shader_name, Image *image) {
    const auto dword_count = static_cast<size_t>(image->width) *
                             static_cast<size_t>(image->height) *
                             image->dwords_per_pixel;
    auto staging = CreateHostBuffer(shader_name, dword_count * sizeof(u32),
                                    vk::BufferUsageFlagBits::eTransferDst, {});

    vk::CommandBuffer cmd = BeginCommands(shader_name, "readback");
    AddImageBarrier(
        cmd, image->image, image->layout, vk::ImageLayout::eTransferSrcOptimal,
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eTransfer,
        vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        vk::AccessFlagBits::eTransferRead);
    vk::BufferImageCopy copy{};
    copy.bufferOffset = 0;
    copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
    copy.imageSubresource.mipLevel = 0;
    copy.imageSubresource.baseArrayLayer = 0;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = image->width;
    copy.imageExtent.height = image->height;
    copy.imageExtent.depth = 1;
    cmd.copyImageToBuffer(image->image, vk::ImageLayout::eTransferSrcOptimal,
                          staging.buffer, 1, &copy);
    EndSubmitAndFree(shader_name, "readback", cmd);
    image->layout = vk::ImageLayout::eTransferSrcOptimal;

    auto ret = ReadBuffer(shader_name, staging, dword_count);
    DestroyBuffer(&staging);
    return ret;
  }

  vk::Sampler CreateNearestSampler(const char *shader_name) {
    vk::SamplerCreateInfo sampler_info{};
    sampler_info.sType = vk::StructureType::eSamplerCreateInfo;
    sampler_info.magFilter = vk::Filter::eNearest;
    sampler_info.minFilter = vk::Filter::eNearest;
    sampler_info.mipmapMode = vk::SamplerMipmapMode::eNearest;
    sampler_info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    vk::Sampler sampler = nullptr;
    RequireVk(shader_name, "dispatch",
              m_device.createSampler(&sampler_info, nullptr, &sampler),
              "vkCreateSampler");
    return sampler;
  }

  std::vector<u32> ReadBuffer(const char *shader_name, const Buffer &buffer,
                              size_t dword_count) {
    if (!buffer.coherent) {
      vk::MappedMemoryRange range{};
      range.sType = vk::StructureType::eMappedMemoryRange;
      range.memory = buffer.memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      RequireVk(shader_name, "readback",
                m_device.invalidateMappedMemoryRanges(1, &range),
                "vkInvalidateMappedMemoryRanges");
    }

    void *data = nullptr;
    RequireVk(shader_name, "readback",
              m_device.mapMemory(buffer.memory, 0, buffer.size, {}, &data),
              "vkMapMemory");
    std::vector<u32> ret(dword_count, 0);
    std::memcpy(ret.data(), data, dword_count * sizeof(u32));
    m_device.unmapMemory(buffer.memory);
    return ret;
  }

  void Dispatch(const TestCase &test, const CompiledShader &compiled,
                const Buffer &buffer, const Buffer *gds_buffer = nullptr,
                const Image *sampled_image = nullptr,
                const Image *storage_image = nullptr,
                const Image *storage_image_uint = nullptr,
                vk::Sampler sampler = nullptr) {
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

    vk::ShaderModuleCreateInfo module_info{};
    module_info.sType = vk::StructureType::eShaderModuleCreateInfo;
    module_info.codeSize = compiled.spirv.size() * sizeof(u32);
    module_info.pCode = compiled.spirv.data();
    vk::ShaderModule module = nullptr;
    RequireVk(test.name, "SPIR-V module",
              m_device.createShaderModule(&module_info, nullptr, &module),
              "vkCreateShaderModule");

    std::vector<vk::DescriptorSetLayoutBinding> layout_bindings;
    auto add_layout_binding =
        [&layout_bindings](u32 binding, vk::DescriptorType type, u32 count) {
          if (count == 0) {
            return;
          }
          vk::DescriptorSetLayoutBinding item{};
          item.binding = binding;
          item.descriptorType = type;
          item.descriptorCount = count;
          item.stageFlags = vk::ShaderStageFlagBits::eCompute;
          layout_bindings.push_back(item);
        };
    for (const auto &binding : layout.descriptors) {
      vk::DescriptorType type = vk::DescriptorType::eStorageBuffer;
      u32 count = static_cast<u32>(binding.resources.size());
      switch (binding.kind) {
      case Kind::Sampled2D:
      case Kind::Sampled2DArray:
      case Kind::Sampled3D:
      case Kind::SampledUint2D:
      case Kind::SampledUint2DArray:
      case Kind::SampledUint3D:
        type = vk::DescriptorType::eSampledImage;
        break;
      case Kind::Storage2D:
      case Kind::Storage2DArray:
      case Kind::Storage3D:
      case Kind::StorageUint2D:
      case Kind::StorageUint2DArray:
      case Kind::StorageUint3D:
        type = vk::DescriptorType::eStorageImage;
        break;
      case Kind::Samplers:
        type = vk::DescriptorType::eSampler;
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

    vk::DescriptorSetLayoutCreateInfo layout_info{};
    layout_info.sType = vk::StructureType::eDescriptorSetLayoutCreateInfo;
    layout_info.bindingCount = static_cast<u32>(layout_bindings.size());
    layout_info.pBindings =
        layout_bindings.empty() ? nullptr : layout_bindings.data();
    vk::DescriptorSetLayout descriptor_layout = nullptr;
    RequireVk(test.name, "dispatch",
              m_device.createDescriptorSetLayout(&layout_info, nullptr,
                                                 &descriptor_layout),
              "vkCreateDescriptorSetLayout");

    vk::PipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = vk::StructureType::ePipelineLayoutCreateInfo;
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &descriptor_layout;
    vk::PushConstantRange push_range{};
    if (layout.push_constant_size != 0) {
      push_range.stageFlags = vk::ShaderStageFlagBits::eCompute;
      push_range.offset = layout.push_constant_offset;
      push_range.size = layout.push_constant_size;
      pipeline_layout_info.pushConstantRangeCount = 1;
      pipeline_layout_info.pPushConstantRanges = &push_range;
    }
    vk::PipelineLayout pipeline_layout = nullptr;
    RequireVk(test.name, "dispatch",
              m_device.createPipelineLayout(&pipeline_layout_info, nullptr,
                                            &pipeline_layout),
              "vkCreatePipelineLayout");

    vk::PipelineShaderStageCreateInfo stage{};
    stage.sType = vk::StructureType::ePipelineShaderStageCreateInfo;
    stage.stage = vk::ShaderStageFlagBits::eCompute;
    stage.module = module;
    stage.pName = "main";

    vk::ComputePipelineCreateInfo pipeline_info{};
    pipeline_info.sType = vk::StructureType::eComputePipelineCreateInfo;
    pipeline_info.stage = stage;
    pipeline_info.layout = pipeline_layout;
    vk::Pipeline pipeline = nullptr;
    RequireVk(test.name, "dispatch",
              m_device.createComputePipelines(nullptr, 1, &pipeline_info,
                                              nullptr, &pipeline),
              "vkCreateComputePipelines");

    std::vector<vk::DescriptorPoolSize> pool_sizes;
    auto add_pool_size = [&pool_sizes](vk::DescriptorType type, u32 count) {
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
    add_pool_size(vk::DescriptorType::eStorageBuffer,
                  Count(Kind::Buffers) + Count(Kind::AddressMemory) +
                      Count(Kind::Gds) +
                      (Binding(Kind::FlattenedSrt) != nullptr ? 1u : 0u) +
                      (Binding(Kind::UserData) != nullptr ? 1u : 0u));
    add_pool_size(vk::DescriptorType::eSampledImage,
                  Count(Kind::Sampled2D) + Count(Kind::Sampled2DArray) +
                      Count(Kind::Sampled3D) + Count(Kind::SampledUint2D) +
                      Count(Kind::SampledUint2DArray) +
                      Count(Kind::SampledUint3D));
    add_pool_size(vk::DescriptorType::eStorageImage,
                  Count(Kind::Storage2D) + Count(Kind::Storage2DArray) +
                      Count(Kind::Storage3D) + Count(Kind::StorageUint2D) +
                      Count(Kind::StorageUint2DArray) +
                      Count(Kind::StorageUint3D));
    add_pool_size(vk::DescriptorType::eSampler, Count(Kind::Samplers));
    vk::DescriptorPoolCreateInfo pool_info{};
    pool_info.sType = vk::StructureType::eDescriptorPoolCreateInfo;
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = static_cast<u32>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.empty() ? nullptr : pool_sizes.data();
    vk::DescriptorPool descriptor_pool = nullptr;
    RequireVk(
        test.name, "dispatch",
        m_device.createDescriptorPool(&pool_info, nullptr, &descriptor_pool),
        "vkCreateDescriptorPool");

    vk::DescriptorSetAllocateInfo set_info{};
    set_info.sType = vk::StructureType::eDescriptorSetAllocateInfo;
    set_info.descriptorPool = descriptor_pool;
    set_info.descriptorSetCount = 1;
    set_info.pSetLayouts = &descriptor_layout;
    vk::DescriptorSet descriptor_set = nullptr;
    RequireVk(test.name, "dispatch",
              m_device.allocateDescriptorSets(&set_info, &descriptor_set),
              "vkAllocateDescriptorSets");

    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorBufferInfo> buffer_infos;
    std::vector<vk::DescriptorBufferInfo> address_memory_infos;
    std::vector<vk::DescriptorImageInfo> sampled_infos;
    std::vector<vk::DescriptorImageInfo> storage_infos;
    std::vector<vk::DescriptorImageInfo> storage_uint_infos;
    std::vector<vk::DescriptorImageInfo> sampler_infos;
    Buffer flattened_buffer;
    Buffer user_data_buffer;
    vk::DescriptorBufferInfo flattened_info{};
    vk::DescriptorBufferInfo user_data_info{};
    vk::DescriptorBufferInfo gds_info{};

    const auto *buffers = Binding(Kind::Buffers);
    if (buffers != nullptr) {
      buffer_infos.resize(buffers->resources.size());
      for (auto &info : buffer_infos) {
        info.buffer = buffer.buffer;
        info.offset = 0;
        info.range = buffer.size;
        if (test.storage_buffer_range_dwords != 0) {
          info.range = static_cast<vk::DeviceSize>(
              test.storage_buffer_range_dwords * sizeof(u32));
          Require(test.name, "dispatch", info.range <= buffer.size,
                  "storage buffer descriptor range exceeds backing buffer");
        }
      }
      vk::WriteDescriptorSet write{};
      write.sType = vk::StructureType::eWriteDescriptorSet;
      write.dstSet = descriptor_set;
      write.dstBinding = buffers->binding;
      write.descriptorCount = static_cast<u32>(buffer_infos.size());
      write.descriptorType = vk::DescriptorType::eStorageBuffer;
      write.pBufferInfo = buffer_infos.data();
      writes.push_back(write);
    }
    if (const auto *address = Binding(Kind::AddressMemory);
        address != nullptr) {
      address_memory_infos.resize(address->resources.size());
      for (auto &info : address_memory_infos) {
        info = {buffer.buffer, 0, buffer.size};
      }
      vk::WriteDescriptorSet write{};
      write.sType = vk::StructureType::eWriteDescriptorSet;
      write.dstSet = descriptor_set;
      write.dstBinding = address->binding;
      write.descriptorCount = static_cast<u32>(address_memory_infos.size());
      write.descriptorType = vk::DescriptorType::eStorageBuffer;
      write.pBufferInfo = address_memory_infos.data();
      writes.push_back(write);
    }
    if (const auto *flattened = Binding(Kind::FlattenedSrt);
        flattened != nullptr) {
      flattened_buffer = CreateStorageBuffer(test.name, compiled.flattened_srt,
                                             compiled.flattened_srt.size());
      flattened_info = {flattened_buffer.buffer, 0, flattened_buffer.size};
      vk::WriteDescriptorSet write{};
      write.sType = vk::StructureType::eWriteDescriptorSet;
      write.dstSet = descriptor_set;
      write.dstBinding = flattened->binding;
      write.descriptorCount = 1;
      write.descriptorType = vk::DescriptorType::eStorageBuffer;
      write.pBufferInfo = &flattened_info;
      writes.push_back(write);
    }
    if (const auto *user = Binding(Kind::UserData); user != nullptr) {
      user_data_buffer =
          CreateStorageBuffer(test.name, compiled.packed_user_data,
                              compiled.packed_user_data.size());
      user_data_info = {user_data_buffer.buffer, 0, user_data_buffer.size};
      vk::WriteDescriptorSet write{};
      write.sType = vk::StructureType::eWriteDescriptorSet;
      write.dstSet = descriptor_set;
      write.dstBinding = user->binding;
      write.descriptorCount = 1;
      write.descriptorType = vk::DescriptorType::eStorageBuffer;
      write.pBufferInfo = &user_data_info;
      writes.push_back(write);
    }
    if (const auto *gds = Binding(Kind::Gds); gds != nullptr) {
      Require(test.name, "dispatch", gds_buffer != nullptr,
              "GDS descriptor requested but no GDS buffer was provided");
      gds_info = {gds_buffer->buffer, 0, gds_buffer->size};
      vk::WriteDescriptorSet write{};
      write.sType = vk::StructureType::eWriteDescriptorSet;
      write.dstSet = descriptor_set;
      write.dstBinding = gds->binding;
      write.descriptorCount = 1;
      write.descriptorType = vk::DescriptorType::eStorageBuffer;
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
      vk::WriteDescriptorSet write{};
      write.sType = vk::StructureType::eWriteDescriptorSet;
      write.dstSet = descriptor_set;
      write.dstBinding = sampled->binding;
      write.descriptorCount = static_cast<u32>(sampled_infos.size());
      write.descriptorType = vk::DescriptorType::eSampledImage;
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
        vk::WriteDescriptorSet write{};
        write.sType = vk::StructureType::eWriteDescriptorSet;
        write.dstSet = descriptor_set;
        write.dstBinding = storage->binding;
        write.descriptorCount = static_cast<u32>(storage_infos.size());
        write.descriptorType = vk::DescriptorType::eStorageImage;
        write.pImageInfo = storage_infos.data();
        writes.push_back(write);
      }
      if (storage_uint != nullptr) {
        vk::WriteDescriptorSet write{};
        write.sType = vk::StructureType::eWriteDescriptorSet;
        write.dstSet = descriptor_set;
        write.dstBinding = storage_uint->binding;
        write.descriptorCount = static_cast<u32>(storage_uint_infos.size());
        write.descriptorType = vk::DescriptorType::eStorageImage;
        write.pImageInfo = storage_uint_infos.data();
        writes.push_back(write);
      }
    }
    const auto *samplers = Binding(Kind::Samplers);
    if (samplers != nullptr) {
      Require(test.name, "dispatch", sampler != nullptr,
              "sampler descriptor requested but no sampler was provided");
      sampler_infos.resize(samplers->resources.size());
      for (auto &info : sampler_infos) {
        info.sampler = sampler;
      }
      vk::WriteDescriptorSet write{};
      write.sType = vk::StructureType::eWriteDescriptorSet;
      write.dstSet = descriptor_set;
      write.dstBinding = samplers->binding;
      write.descriptorCount = static_cast<u32>(sampler_infos.size());
      write.descriptorType = vk::DescriptorType::eSampler;
      write.pImageInfo = sampler_infos.data();
      writes.push_back(write);
    }
    if (!writes.empty()) {
      m_device.updateDescriptorSets(static_cast<u32>(writes.size()),
                                    writes.data(), 0, nullptr);
    }

    vk::CommandBuffer cmd = BeginCommands(test.name, "dispatch");
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, pipeline_layout, 0,
                           1, &descriptor_set, 0, nullptr);
    if (layout.push_constant_size != 0) {
      Require(test.name, "dispatch",
              compiled.packed_user_data.size() * sizeof(u32) ==
                  layout.push_constant_size,
              "native user-data size does not match push-constant range");
      cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eCompute,
                        layout.push_constant_offset, layout.push_constant_size,
                        compiled.packed_user_data.data());
    }
    cmd.dispatch(test.dispatch_x, test.dispatch_y, test.dispatch_z);

    if (buffers != nullptr) {
      vk::BufferMemoryBarrier barrier{};
      barrier.sType = vk::StructureType::eBufferMemoryBarrier;
      barrier.srcAccessMask =
          vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
      barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = buffer.buffer;
      barrier.offset = 0;
      barrier.size = buffer.size;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                          vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
                          &barrier, 0, nullptr);
    }
    if (gds_buffer != nullptr) {
      vk::BufferMemoryBarrier barrier{};
      barrier.sType = vk::StructureType::eBufferMemoryBarrier;
      barrier.srcAccessMask =
          vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
      barrier.dstAccessMask = vk::AccessFlagBits::eHostRead;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.buffer = gds_buffer->buffer;
      barrier.offset = 0;
      barrier.size = gds_buffer->size;
      cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                          vk::PipelineStageFlagBits::eHost, {}, 0, nullptr, 1,
                          &barrier, 0, nullptr);
    }
    EndSubmitAndFree(test.name, "dispatch", cmd);
    if (flattened_buffer.buffer != nullptr) {
      DestroyBuffer(&flattened_buffer);
    }
    if (user_data_buffer.buffer != nullptr) {
      DestroyBuffer(&user_data_buffer);
    }
    m_device.destroyDescriptorPool(descriptor_pool, nullptr);
    m_device.destroyPipeline(pipeline, nullptr);
    m_device.destroyPipelineLayout(pipeline_layout, nullptr);
    m_device.destroyDescriptorSetLayout(descriptor_layout, nullptr);
    m_device.destroyShaderModule(module, nullptr);
  }

  std::vector<u32> RenderFragment(const GraphicsCase &test,
                                  const CompiledShader &fragment) {
    const auto vertex_spirv = TestSpv::MakePassthroughVertexSpirv();
    ValidateSpirv(test.name, vertex_spirv);

    Image target =
        CreateImage2D(test.name, 1, 1, vk::Format::eR32G32B32A32Sfloat,
                      vk::ImageUsageFlagBits::eColorAttachment, {}, 4,
                      vk::ImageLayout::eGeneral);
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
                         vk::BufferUsageFlagBits::eVertexBuffer, vertices);

    auto make_module = [&](const std::vector<u32> &spirv) {
      vk::ShaderModuleCreateInfo module_info{};
      module_info.sType = vk::StructureType::eShaderModuleCreateInfo;
      module_info.codeSize = spirv.size() * sizeof(u32);
      module_info.pCode = spirv.data();
      vk::ShaderModule module = nullptr;
      RequireVk(test.name, "graphics",
                m_device.createShaderModule(&module_info, nullptr, &module),
                "vkCreateShaderModule");
      return module;
    };
    vk::ShaderModule vertex_module = make_module(vertex_spirv);
    vk::ShaderModule fragment_module = make_module(fragment.spirv);

    vk::AttachmentDescription attachment{};
    attachment.format = vk::Format::eR32G32B32A32Sfloat;
    attachment.samples = vk::SampleCountFlagBits::e1;
    attachment.loadOp = vk::AttachmentLoadOp::eClear;
    attachment.storeOp = vk::AttachmentStoreOp::eStore;
    attachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    attachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    attachment.initialLayout = vk::ImageLayout::eGeneral;
    attachment.finalLayout = vk::ImageLayout::eGeneral;

    vk::AttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = vk::ImageLayout::eColorAttachmentOptimal;

    vk::SubpassDescription subpass{};
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    vk::RenderPassCreateInfo render_pass_info{};
    render_pass_info.sType = vk::StructureType::eRenderPassCreateInfo;
    render_pass_info.attachmentCount = 1;
    render_pass_info.pAttachments = &attachment;
    render_pass_info.subpassCount = 1;
    render_pass_info.pSubpasses = &subpass;
    vk::RenderPass render_pass = nullptr;
    RequireVk(
        test.name, "graphics",
        m_device.createRenderPass(&render_pass_info, nullptr, &render_pass),
        "vkCreateRenderPass");

    vk::FramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = vk::StructureType::eFramebufferCreateInfo;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &target.view;
    framebuffer_info.width = 1;
    framebuffer_info.height = 1;
    framebuffer_info.layers = 1;
    vk::Framebuffer framebuffer = nullptr;
    RequireVk(
        test.name, "graphics",
        m_device.createFramebuffer(&framebuffer_info, nullptr, &framebuffer),
        "vkCreateFramebuffer");

    const auto &fragment_bind = fragment.program.bindings;
    vk::PushConstantRange push_constant_range{};
    if (fragment_bind.push_constant_size > 0) {
      Require(test.name, "graphics",
              test.push_constants.size() * sizeof(u32) ==
                  fragment_bind.push_constant_size,
              "fragment push constant data size does not match reflection");
      push_constant_range.stageFlags = vk::ShaderStageFlagBits::eFragment;
      push_constant_range.offset = fragment_bind.push_constant_offset;
      push_constant_range.size = fragment_bind.push_constant_size;
    }

    vk::PipelineLayoutCreateInfo pipeline_layout_info{};
    pipeline_layout_info.sType = vk::StructureType::ePipelineLayoutCreateInfo;
    pipeline_layout_info.pushConstantRangeCount =
        push_constant_range.size != 0 ? 1u : 0u;
    pipeline_layout_info.pPushConstantRanges =
        push_constant_range.size != 0 ? &push_constant_range : nullptr;
    vk::PipelineLayout pipeline_layout = nullptr;
    RequireVk(test.name, "graphics",
              m_device.createPipelineLayout(&pipeline_layout_info, nullptr,
                                            &pipeline_layout),
              "vkCreatePipelineLayout");

    vk::PipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = vk::StructureType::ePipelineShaderStageCreateInfo;
    stages[0].stage = vk::ShaderStageFlagBits::eVertex;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = vk::StructureType::ePipelineShaderStageCreateInfo;
    stages[1].stage = vk::ShaderStageFlagBits::eFragment;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    vk::VertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = 6u * sizeof(float);
    vertex_binding.inputRate = vk::VertexInputRate::eVertex;
    vk::VertexInputAttributeDescription attributes[2] = {};
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = vk::Format::eR32G32Sfloat;
    attributes[0].offset = 0;
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = vk::Format::eR32G32B32A32Sfloat;
    attributes[1].offset = 2u * sizeof(float);
    vk::PipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType = vk::StructureType::ePipelineVertexInputStateCreateInfo;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    vk::PipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        vk::StructureType::ePipelineInputAssemblyStateCreateInfo;
    input_assembly.topology = vk::PrimitiveTopology::eTriangleList;

    vk::Viewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vk::Rect2D scissor{};
    scissor.extent.width = 1;
    scissor.extent.height = 1;
    vk::PipelineViewportStateCreateInfo viewport_state{};
    viewport_state.sType = vk::StructureType::ePipelineViewportStateCreateInfo;
    viewport_state.viewportCount = 1;
    viewport_state.pViewports = &viewport;
    viewport_state.scissorCount = 1;
    viewport_state.pScissors = &scissor;

    vk::PipelineRasterizationStateCreateInfo raster{};
    raster.sType = vk::StructureType::ePipelineRasterizationStateCreateInfo;
    raster.polygonMode = vk::PolygonMode::eFill;
    raster.cullMode = vk::CullModeFlagBits::eNone;
    raster.frontFace = vk::FrontFace::eCounterClockwise;
    raster.lineWidth = 1.0f;

    vk::PipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = vk::StructureType::ePipelineMultisampleStateCreateInfo;
    multisample.rasterizationSamples = vk::SampleCountFlagBits::e1;

    vk::PipelineColorBlendAttachmentState color_attachment{};
    color_attachment.colorWriteMask =
        vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
        vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
    vk::PipelineColorBlendStateCreateInfo color_blend{};
    color_blend.sType = vk::StructureType::ePipelineColorBlendStateCreateInfo;
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &color_attachment;

    vk::GraphicsPipelineCreateInfo pipeline_info{};
    pipeline_info.sType = vk::StructureType::eGraphicsPipelineCreateInfo;
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
    vk::Pipeline pipeline = nullptr;
    RequireVk(test.name, "graphics",
              m_device.createGraphicsPipelines(nullptr, 1, &pipeline_info,
                                               nullptr, &pipeline),
              "vkCreateGraphicsPipelines");

    vk::CommandBuffer cmd = BeginCommands(test.name, "graphics");
    vk::ClearValue clear{};
    vk::RenderPassBeginInfo begin{};
    begin.sType = vk::StructureType::eRenderPassBeginInfo;
    begin.renderPass = render_pass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent.width = 1;
    begin.renderArea.extent.height = 1;
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    cmd.beginRenderPass(&begin, vk::SubpassContents::eInline);
    cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
    if (push_constant_range.size != 0) {
      cmd.pushConstants(pipeline_layout, vk::ShaderStageFlagBits::eFragment,
                        fragment_bind.push_constant_offset,
                        fragment_bind.push_constant_size,
                        test.push_constants.data());
    }
    vk::DeviceSize offset = 0;
    cmd.bindVertexBuffers(0, 1, &vertex_buffer.buffer, &offset);
    cmd.draw(3, 1, 0, 0);
    cmd.endRenderPass();
    EndSubmitAndFree(test.name, "graphics", cmd);
    target.layout = vk::ImageLayout::eGeneral;

    auto pixel = ReadImage(test.name, &target);
    pixel.resize(4);

    m_device.destroyPipeline(pipeline, nullptr);
    m_device.destroyPipelineLayout(pipeline_layout, nullptr);
    m_device.destroyFramebuffer(framebuffer, nullptr);
    m_device.destroyRenderPass(render_pass, nullptr);
    m_device.destroyShaderModule(fragment_module, nullptr);
    m_device.destroyShaderModule(vertex_module, nullptr);
    DestroyBuffer(&vertex_buffer);
    DestroyImage(&target);
    return pixel;
  }

  void CheckGpuTilerCpuParity() {
    constexpr const char *name = "GpuTilerCpuParity";
    EnsureRuntimeContext();

    auto fill = [](std::vector<uint8_t> *bytes, u32 salt) {
      for (size_t i = 0; i < bytes->size(); i++) {
        uint64_t value =
            i + static_cast<uint64_t>(salt) * 0x9e3779b97f4a7c15ull;
        value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
        (*bytes)[i] = static_cast<uint8_t>((value ^ (value >> 31u)) >> 56u);
      }
    };
    auto compare = [&](const char *stage, const std::vector<uint8_t> &expected,
                       const std::vector<uint8_t> &actual) {
      if (expected != actual) {
        const auto mismatch = static_cast<size_t>(
            std::mismatch(expected.begin(), expected.end(), actual.begin())
                .first -
            expected.begin());
        std::ostringstream out;
        out << "first mismatch at " << mismatch << " of " << expected.size();
        Fail(name, stage, out.str());
      }
    };
    auto convert_reference = [&](bool to_tiled, std::vector<uint8_t> *dst,
                                 const std::vector<uint8_t> &src,
                                 const GpuTileInfo &info) {
      TileBlockLayout block{};
      Require(name, "reference layout",
              TileGetBlockLayout(info.family, info.bytes_per_element, block),
              "CPU reference rejected GPU tile info");
      const u32 tiled_width =
          info.tiled_width != 0 ? info.tiled_width : info.pitch;
      const u32 tiled_height =
          info.tiled_height != 0 ? info.tiled_height : info.height;
      const uint64_t columns =
          (tiled_width + block.block_width - 1u) / block.block_width;
      const uint64_t rows =
          (tiled_height + block.block_height - 1u) / block.block_height;
      const uint64_t slice = info.linear_slice_stride != 0
                                 ? info.linear_slice_stride
                                 : static_cast<uint64_t>(info.pitch) *
                                       info.height * info.bytes_per_element;
      for (u32 z = 0; z < info.depth; ++z) {
        for (u32 y = 0; y < info.height; ++y) {
          for (u32 x = 0; x < info.width; ++x) {
            const u32 bx = info.tail ? 0 : x / block.block_width;
            const u32 by = info.tail ? 0 : y / block.block_height;
            const u32 bz = info.tail ? 0 : z / block.block_depth;
            const u32 lx = info.tail ? x + info.tail_x : x % block.block_width;
            const u32 ly = info.tail ? y + info.tail_y : y % block.block_height;
            const u32 lz = z % block.block_depth;
            u32 local = 0, block_xor = 0;
            Require(name, "reference offset",
                    TileGetBlockOffset(block, lx, ly, lz, local) &&
                        TileGetBlockXor(block, bx, by, info.surface_z + bz, block_xor),
                    "CPU reference address lookup failed");
            const uint64_t block_index =
                static_cast<uint64_t>(bz) * columns * rows + by * columns + bx;
            const uint64_t tiled = info.tiled_offset +
                                   block_index * block.block_size +
                                   (local ^ block_xor);
            const uint64_t linear =
                info.linear_offset + static_cast<uint64_t>(z) * slice +
                static_cast<uint64_t>(y) * info.pitch * info.bytes_per_element +
                static_cast<uint64_t>(x) * info.bytes_per_element;
            const uint64_t dst_offset = to_tiled ? tiled : linear;
            const uint64_t src_offset = to_tiled ? linear : tiled;
            Require(name, "reference range",
                    dst_offset + info.bytes_per_element <= dst->size() &&
                        src_offset + info.bytes_per_element <= src.size(),
                    "CPU reference address escaped storage");
            std::memcpy(dst->data() + dst_offset, src.data() + src_offset,
                        info.bytes_per_element);
          }
        }
      }
    };
    struct FamilyCase {
      TileBlockFamily family;
      u32 max_bpe;
    };
    constexpr FamilyCase families[] = {
        {TileBlockFamily::Standard256B, 16},
        {TileBlockFamily::Standard4KB, 16},
        {TileBlockFamily::Standard4KB3D, 16},
        {TileBlockFamily::Standard64KB, 16},
        {TileBlockFamily::Standard64KB3D, 16},
        {TileBlockFamily::Prt64KB, 16},
        {TileBlockFamily::Prt64KB3D, 16},
        {TileBlockFamily::RenderTarget64KB, 16},
        {TileBlockFamily::Depth64KB, 8},
    };

    {
      TileBlockLayout standard{}, prt{}, standard_3d{}, prt_3d{}, color{},
          depth{};
      u32 standard_offset = 0, prt_offset = 0, standard_3d_offset = 0,
          prt_3d_offset = 0, color_z = 0, depth_z = 0;
      bool fixed_addresses =
          TileGetBlockLayout(TileBlockFamily::Standard64KB, 4, standard) &&
          TileGetBlockLayout(TileBlockFamily::Prt64KB, 4, prt) &&
          TileGetBlockLayout(TileBlockFamily::Standard64KB3D, 4, standard_3d) &&
          TileGetBlockLayout(TileBlockFamily::Prt64KB3D, 4, prt_3d) &&
          TileGetBlockLayout(TileBlockFamily::RenderTarget64KB, 4, color) &&
          TileGetBlockLayout(TileBlockFamily::Depth64KB, 4, depth) &&
          TileGetBlockOffset(standard, 64, 0, 0, standard_offset) &&
          TileGetBlockOffset(prt, 64, 0, 0, prt_offset) &&
          TileGetBlockOffset(standard_3d, 16, 0, 0, standard_3d_offset) &&
          TileGetBlockOffset(prt_3d, 16, 0, 0, prt_3d_offset) &&
          TileGetBlockXor(color, 0, 0, 1, color_z) &&
          TileGetBlockXor(depth, 0, 0, 15, depth_z);
      Require(name, "fixed address vectors",
              fixed_addresses && standard_offset == 0x8000 &&
                  prt_offset == 0x8100 && standard_3d_offset == 0x8000 &&
                  prt_3d_offset == 0x8400 && standard_3d.block_width == 32 &&
                  standard_3d.block_height == 32 &&
                  standard_3d.block_depth == 16 && color_z == 0x800 &&
                  depth_z == 0xf00,
              "a fixed block address changed");

      constexpr u32 format =
          Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
      constexpr u32 levels = 7;
      TileSizeAlign total{};
      TileSizeOffset mip[levels]{};
      TilePaddedSize padded[levels]{};
      TileGetTextureSize(
          format, 65, 33, 72, levels,
          Prospero::GpuEnumValue(Prospero::TileMode::kStandard256B), &total,
          mip, padded);
      constexpr u32 offsets[levels] = {0x1a00, 0xb00, 0x500, 0x300,
                                       0x200,  0x100, 0};
      constexpr u32 sizes[levels] = {0x2d00, 0xf00, 0x600, 0x200,
                                     0x100,  0x100, 0x100};
      bool layout_matches = total.size == 0x4700 && total.align == 0x100 &&
                            padded[0].width == 72 && padded[0].height == 40;
      for (u32 level = 0; level < levels; ++level) {
        layout_matches &= mip[level].offset == offsets[level] &&
                          mip[level].size == sizes[level];
      }
      Require(name, "fixed mip vector", layout_matches,
              "the reverse-packed small-block mip layout changed");
    }

    u32 case_index = 0;
    auto check_round_trip = [&](const char *stage, uint64_t size,
                                std::span<const GpuTileInfo> infos) {
      std::vector<uint8_t> tiled(size);
      std::vector<uint8_t> cpu(size, 0);
      std::vector<uint8_t> gpu(size, 0xab);
      fill(&tiled, ++case_index);
      for (const auto &info : infos) {
        convert_reference(false, &cpu, tiled, info);
      }
	  GpuDetile(tiled.data(), gpu.data(), size, size, infos);
      compare((std::string(stage) + " detile bytes").c_str(), cpu, gpu);

      std::vector<uint8_t> linear(size);
      std::vector<uint8_t> cpu_tiled(size, 0);
      std::vector<uint8_t> gpu_tiled(size, 0xab);
      fill(&linear, 0x280u + case_index);
      for (const auto &info : infos) {
        convert_reference(true, &cpu_tiled, linear, info);
      }
		GpuTile(linear.data(), gpu_tiled.data(), size, size,
              infos);
      compare((std::string(stage) + " tile bytes").c_str(), cpu_tiled,
              gpu_tiled);
    };
    for (const auto family : families) {
      for (u32 bpe = 1; bpe <= family.max_bpe; bpe <<= 1u) {
        TileBlockLayout block{};
        Require(name, "block layout",
                TileGetBlockLayout(family.family, bpe, block),
                "admitted family/BPE has no block layout");
        const bool volume = block.block_depth > 1;
        const u32 width =
            block.block_width * 3u + std::min(block.block_width, 3u);
        const u32 height =
            block.block_height * 3u + std::min(block.block_height, 3u);
        const u32 depth = volume ? block.block_depth + 1u : 1u;
        const u32 pitch = block.block_width * 4u;
        const uint64_t block_columns =
            (pitch + block.block_width - 1u) / block.block_width;
        const uint64_t block_rows =
            (height + block.block_height - 1u) / block.block_height;
        const uint64_t block_slices =
            (depth + block.block_depth - 1u) / block.block_depth;
        const uint64_t storage_size =
            block_columns * block_rows * block_slices * block.block_size;
        const uint64_t slice_stride =
            static_cast<uint64_t>(pitch) * height * bpe;

        std::vector<uint8_t> tiled(storage_size);
        std::vector<uint8_t> cpu(storage_size, 0);
        std::vector<uint8_t> gpu(storage_size, 0xab);
        fill(&tiled, ++case_index);

        GpuTileInfo info{};
        info.family = block.family;
        info.bytes_per_element = block.bytes_per_element;
        info.linear_size = storage_size;
        info.tiled_size = storage_size;
        info.linear_slice_stride = volume ? slice_stride : 0;
        info.width = width;
        info.height = height;
        info.depth = depth;
        info.pitch = pitch;
        info.surface_z = family.family == TileBlockFamily::RenderTarget64KB ||
                                 family.family == TileBlockFamily::Depth64KB
                             ? 3
                             : 0;
        convert_reference(false, &cpu, tiled, info);
		GpuDetile(tiled.data(), gpu.data(), storage_size,
                  storage_size, std::span<const GpuTileInfo>(&info, 1));
        const auto family_label = [&](const char *operation) {
          std::ostringstream out;
          out << operation << " family=" << static_cast<u32>(family.family)
              << " bpe=" << bpe;
          return out.str();
        };
        compare(family_label("detile bytes").c_str(), cpu, gpu);

        {
          std::vector<uint8_t> linear(storage_size);
          std::vector<uint8_t> cpu_tiled(storage_size, 0);
          std::vector<uint8_t> gpu_tiled(storage_size, 0xab);
          fill(&linear, 0x80u + case_index);
          convert_reference(true, &cpu_tiled, linear, info);
			GpuTile(linear.data(), gpu_tiled.data(),
                  storage_size, storage_size,
                  std::span<const GpuTileInfo>(&info, 1));
          compare(family_label("tile bytes").c_str(), cpu_tiled, gpu_tiled);
        }
      }
    }

    // Exercise the real texture-layout/info-building seam for every format
    // admitted by each standard tile mode.  Formats sharing a byte/block width
    // intentionally share a shader, but this loop still validates their
    // texel-to-element conversion (notably every BCn format).
    struct StandardMode {
      u32 tile;
      TileBlockFamily family;
      bool (*supported)(u32);
    };
    constexpr StandardMode standard_modes[] = {
        {Prospero::GpuEnumValue(Prospero::TileMode::kStandard256B),
         TileBlockFamily::Standard256B, TileIsStandard256BTextureSupported},
        {Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB),
         TileBlockFamily::Standard4KB, TileIsStandard4KBTextureSupported},
        {Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB),
         TileBlockFamily::Standard64KB, TileIsStandard64KBTextureSupported},
        {Prospero::GpuEnumValue(Prospero::TileMode::kPrt),
         TileBlockFamily::Prt64KB, TileIsStandard64KBTextureSupported},
    };
    u32 format_cases = 0;
    for (u32 format = 1;
         format <= Prospero::GpuEnumValue(Prospero::BufferFormat::kBc7Srgb);
         ++format) {
      // FMASK is synthesized as identity metadata by TextureUploadFmask; it is
      // deliberately not a texel surface and must never enter the GPU tiler.
      if (Prospero::IsFmaskTextureFormat(format)) {
        continue;
      }
      for (const auto &mode : standard_modes) {
        if (!mode.supported(format)) {
          continue;
        }
        const u32 bpe =
            std::max(Prospero::NumBytesPerElement(format),
                     Prospero::BlockCompressedBytesPerBlock(format));
        TileBlockLayout block{};
        Require(name, "format block",
                TileGetBlockLayout(mode.family, bpe, block),
                "CPU-supported format has no GPU family/BPE mapping");

        constexpr u32 width = 67;
        constexpr u32 height = 51;
        const u32 pitch = TileGetTexturePitch(format, width, 1, mode.tile);
        TileSizeAlign total{};
        TileGetTextureSize(format, width, height, pitch, 1, mode.tile, &total,
                           nullptr, nullptr);
        Require(name, "format size", total.size != 0,
                "supported format has an empty layout");

        const auto layout =
            TextureCalcUploadLayout(format, width, height, 1, 1, pitch,
                                    mode.tile, total.size, false, false, name);
        const auto regions = TextureBuildUploadRegions(
            layout, TextureGetFormat(format), width, height, 1, 1, false, false,
            TextureUploadDestination::MipLevels);
        std::vector<GpuTileInfo> infos;
        if (!TextureBuildGpuTileInfos(total.size, regions, layout, format, 1, 1,
                                      infos)) {
          std::ostringstream out;
          out << "format=" << format << " tile=" << mode.tile
              << " size=" << total.size << " pitch=" << pitch;
          Fail(name, "format infos", out.str());
        }
        Require(name, "format family",
                infos.size() == 1 && infos[0].family == mode.family &&
                    infos[0].bytes_per_element == bpe,
                "texture info selected the wrong shader family");

        std::vector<uint8_t> tiled(total.size);
        std::vector<uint8_t> cpu(total.size, 0);
        std::vector<uint8_t> gpu(total.size, 0xab);
        fill(&tiled, ++case_index);
        convert_reference(false, &cpu, tiled, infos[0]);
		GpuDetile(tiled.data(), gpu.data(), total.size,
                  total.size, infos);
        compare("format bytes", cpu, gpu);
        ++format_cases;
      }
    }
    Require(name, "format coverage", format_cases != 0,
            "no CPU-supported standard formats were tested");

    {
      constexpr u32 format =
          Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
      constexpr u32 tile =
          Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
      constexpr u32 width = 129, height = 65, layers = 3;
      const u32 pitch = TileGetTexturePitch(format, width, 1, tile);
      TileSizeAlign total{};
      TileGetTextureTotalSize(format, width, height, layers, pitch, 1, tile,
                              false, total);
      const auto layout =
          TextureCalcUploadLayout(format, width, height, 1, layers, pitch, tile,
                                  total.size, true, false, name);
      const auto regions = TextureBuildUploadRegions(
          layout, vk::Format::eR32Sfloat, width, height, layers, 1, true, false,
          TextureUploadDestination::MipLevels);
      std::vector<GpuTileInfo> infos;
      const bool built = TextureBuildGpuTileInfos(total.size, regions, layout,
                                                  format, layers, 1, infos);
      Require(name, "array infos",
              built && infos.size() == layers && infos[0].surface_z == 0 &&
                  infos[1].surface_z == 1 && infos[2].surface_z == 2,
              "array slices lost their absolute surface Z");
      check_round_trip("array", total.size, infos);
    }

    for (const auto &mode : standard_modes) {
      constexpr u32 format =
          Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
      constexpr u32 levels = 2;
      TileBlockLayout block{};
      Require(name, "odd mip block", TileGetBlockLayout(mode.family, 4, block),
              "odd multi-mip format has no block layout");
      const u32 width = block.block_width * 2u + 1u;
      const u32 height = block.block_height * 2u + 1u;
      const u32 pitch = TileGetTexturePitch(format, width, levels, mode.tile);
      TileSizeAlign total{};
      TileGetTextureSize(format, width, height, pitch, levels, mode.tile,
                         &total, nullptr, nullptr);
      const auto layout =
          TextureCalcUploadLayout(format, width, height, levels, 1, pitch,
                                  mode.tile, total.size, false, false, name);
      const auto regions = TextureBuildUploadRegions(
          layout, vk::Format::eR32Sfloat, width, height, 1, levels, false,
          false, TextureUploadDestination::MipLevels);
      std::vector<GpuTileInfo> infos;
      Require(name, "odd mip infos",
              TextureBuildGpuTileInfos(total.size, regions, layout, format, 1,
                                       levels, infos) &&
                  infos.size() == 2 && infos[1].tiled_width >= infos[1].pitch &&
                  infos[1].tiled_height >= infos[1].height &&
                  (mode.family == TileBlockFamily::Standard256B ||
                   infos[1].tiled_height > infos[1].height),
              "odd multi-mip physical stride collapsed to the active linear "
              "extent");
      check_round_trip("odd multi-mip", total.size, infos);
    }

    struct TailMode {
      u32 tile;
      TileBlockFamily family;
    };
    constexpr TailMode tail_modes[] = {
        {Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB),
         TileBlockFamily::Standard4KB},
        {Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB),
         TileBlockFamily::Standard64KB},
        {Prospero::GpuEnumValue(Prospero::TileMode::kPrt),
         TileBlockFamily::Prt64KB},
        {Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget),
         TileBlockFamily::RenderTarget64KB},
        {Prospero::GpuEnumValue(Prospero::TileMode::kDepth),
         TileBlockFamily::Depth64KB},
    };
    for (const auto &mode : tail_modes) {
      constexpr u32 format =
          Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
      constexpr u32 levels = 7;
      TileBlockLayout block{};
      Require(name, "2D tail block", TileGetBlockLayout(mode.family, 4, block),
              "2D tail mode has no block layout");
      const u32 width = block.block_width * 2u;
      const u32 height = block.block_height * 2u;
      const u32 pitch = TileGetTexturePitch(format, width, levels, mode.tile);
      TileSizeAlign total{};
      TileGetTextureSize(format, width, height, pitch, levels, mode.tile,
                         &total, nullptr, nullptr);
      const auto layout =
          TextureCalcUploadLayout(format, width, height, levels, 1, pitch,
                                  mode.tile, total.size, true, false, name);
      const auto regions = TextureBuildUploadRegions(
          layout, vk::Format::eR32Sfloat, width, height, 1, levels, false,
          false, TextureUploadDestination::MipLevels);
      std::vector<GpuTileInfo> infos;
      Require(name, "2D mip tail seam",
              layout.first_tail_level == 2 &&
                  TextureBuildGpuTileInfos(total.size, regions, layout, format,
                                           1, levels, infos) &&
                  infos.size() == levels && !infos[0].tail && !infos[1].tail &&
                  std::all_of(infos.begin() + 2, infos.end(),
                              [](const auto &info) { return info.tail; }),
              "2D mip chain lost its linear/tiled tail boundary");
      check_round_trip("2D mip tail seam", total.size, infos);
    }

    {
      constexpr u32 format =
          Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
      constexpr u32 tile =
          Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB);
      constexpr u32 width = 65;
      constexpr u32 height = 129;
      constexpr u32 depth = 17;
      constexpr u32 levels = 2;
      const u32 pitch = TileGetTexturePitch(format, width, levels, tile);
      TileSizeAlign total{};
      TileGetTextureTotalSize(format, width, height, depth, pitch, levels, tile,
                              true, total);
      const auto layout =
          TextureCalcUploadLayout(format, width, height, levels, depth, pitch,
                                  tile, total.size, false, true, name);
      const auto regions = TextureBuildUploadRegions(
          layout, vk::Format::eR32Sfloat, width, height, depth, levels, false,
          true, TextureUploadDestination::MipLevels);
      std::vector<GpuTileInfo> infos;
      const bool built = TextureBuildGpuTileInfos(total.size, regions, layout,
                                                  format, depth, levels, infos);
      Require(name, "3D mip infos",
              regions.size() == depth + (depth >> 1u) && built &&
                  infos.size() == 4 && infos[0].depth == 8 &&
                  infos[1].depth == 8 && infos[2].depth == 1 &&
                  infos[3].depth == 8 && infos[0].tiled_offset == 0x19000 &&
                  infos[1].tiled_offset == 0x83000 &&
                  infos[2].tiled_offset == 0xed000 &&
                  infos[3].tiled_offset == 0 && infos[3].pitch == 32 &&
                  infos[3].height == 64 && infos[3].tiled_width == 40 &&
                  infos[3].tiled_height == 80,
              "Standard4KB3D mip depth, packing, or physical stride changed");
      check_round_trip("3D mip", total.size, infos);
    }

    {
      constexpr u32 format =
          Prospero::GpuEnumValue(Prospero::BufferFormat::kBc1UNorm);
      constexpr u32 tile =
          Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB);
      constexpr u32 width = 65;
      constexpr u32 height = 129;
      constexpr u32 depth = 17;
      constexpr u32 levels = 2;
      const u32 pitch = TileGetTexturePitch(format, width, levels, tile);
      TileSizeAlign total{};
      TileGetTextureTotalSize(format, width, height, depth, pitch, levels, tile,
                              true, total);
      const auto layout =
          TextureCalcUploadLayout(format, width, height, levels, depth, pitch,
                                  tile, total.size, false, true, name);
      const auto regions = TextureBuildUploadRegions(
          layout, TextureGetFormat(format), width, height, depth, levels, false,
          true, TextureUploadDestination::MipLevels);
      std::vector<GpuTileInfo> infos;
      Require(name, "3D BC mip infos",
              TextureBuildGpuTileInfos(total.size, regions, layout, format,
                                       depth, levels, infos) &&
                  infos.size() == 4 && infos[3].pitch == 8 &&
                  infos[3].height == 16 && infos[3].tiled_width == 16 &&
                  infos[3].tiled_height == 24,
              "block-compressed 3D mip lost its physical row or slice stride");
      check_round_trip("3D BC mip", total.size, infos);
    }

    struct VolumeModeCase {
      u32 tile;
      TileBlockFamily family;
      u32 format;
    };
    constexpr VolumeModeCase volume_modes[] = {
        {Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB),
         TileBlockFamily::Standard64KB3D,
         Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float)},
        {Prospero::GpuEnumValue(Prospero::TileMode::kPrt),
         TileBlockFamily::Prt64KB3D,
         Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float)},
        {Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget),
         TileBlockFamily::RenderTarget64KB,
         Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float)},
        {Prospero::GpuEnumValue(Prospero::TileMode::kDepth),
         TileBlockFamily::Depth64KB,
         Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float)},
        {Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB),
         TileBlockFamily::Standard64KB3D,
         Prospero::GpuEnumValue(Prospero::BufferFormat::kBc1UNorm)},
        {Prospero::GpuEnumValue(Prospero::TileMode::kPrt),
         TileBlockFamily::Prt64KB3D,
         Prospero::GpuEnumValue(Prospero::BufferFormat::kBc3UNorm)},
    };
    for (const auto test : volume_modes) {
      constexpr u32 width = 65, height = 33, depth = 37, levels = 6;
      const u32 pitch =
          TileGetTexturePitch(test.format, width, levels, test.tile);
      TileSizeAlign total{};
      TileGetTextureTotalSize(test.format, width, height, depth, pitch, levels,
                              test.tile, true, total);
      const auto layout = TextureCalcUploadLayout(
          test.format, width, height, levels, depth, pitch, test.tile,
          total.size, true, true, name);
      const auto regions = TextureBuildUploadRegions(
          layout, TextureGetFormat(test.format), width, height, depth, levels,
          false, true, TextureUploadDestination::MipLevels);
      std::vector<GpuTileInfo> infos;
      const bool built = TextureBuildGpuTileInfos(
          total.size, regions, layout, test.format, depth, levels, infos);
      const bool uses_z = test.family == TileBlockFamily::RenderTarget64KB ||
                          test.family == TileBlockFamily::Depth64KB;
      Require(name, "volume family infos",
              built && !infos.empty() &&
                  std::all_of(infos.begin(), infos.end(),
                              [&](const auto &info) {
                                return info.family == test.family;
                              }) &&
                  (!uses_z || std::any_of(infos.begin(), infos.end(),
                                          [](const auto &info) {
                                            return info.surface_z != 0;
                                          })),
              "volume mode selected the wrong family or lost its surface Z");
      check_round_trip("volume family", total.size, infos);
    }

    struct VolumeTailCase {
      u32 format;
      u32 bytes;
    };
    constexpr VolumeTailCase volume_tails[] = {
        {Prospero::GpuEnumValue(Prospero::BufferFormat::k8UNorm), 1},
        {Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 2},
        {Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 4},
        {Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float), 8},
        {Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32Float), 16},
        {Prospero::GpuEnumValue(Prospero::BufferFormat::kBc1UNorm), 8},
        {Prospero::GpuEnumValue(Prospero::BufferFormat::kBc3UNorm), 16},
    };
    constexpr u32 volume_tail_xy[5][5][2] = {
        {{0, 8}, {8, 4}, {8, 0}, {0, 4}, {0, 0}},
        {{0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
        {{0, 8}, {4, 4}, {4, 0}, {0, 4}, {0, 0}},
        {{0, 4}, {4, 2}, {4, 0}, {0, 2}, {0, 0}},
        {{0, 4}, {2, 2}, {2, 0}, {0, 2}, {0, 0}},
    };
    for (const auto &tail : volume_tails) {
      constexpr u32 tile =
          Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB);
      constexpr u32 levels = 5;
      TileBlockLayout block{};
      Require(name, "3D tail block",
              TileGetBlockLayout(TileBlockFamily::Standard4KB3D, tail.bytes, block),
              "Standard4KB3D tail format has no block layout");
      const bool compressed =
          Prospero::BlockCompressedBytesPerBlock(tail.format) != 0;
      const u32 width = block.block_width * (compressed ? 4u : 1u);
      const u32 height = block.block_height / 2u * (compressed ? 4u : 1u);
      const u32 depth = block.block_depth + 1u;
      const u32 pitch = TileGetTexturePitch(tail.format, width, levels, tile);
      TileSizeAlign total{};
      TileGetTextureTotalSize(tail.format, width, height, depth, pitch, levels,
                              tile, true, total);
      const auto layout =
          TextureCalcUploadLayout(tail.format, width, height, levels, depth,
                                  pitch, tile, total.size, false, true, name);
      const auto regions = TextureBuildUploadRegions(
          layout, TextureGetFormat(tail.format), width, height, depth, levels,
          false, true, TextureUploadDestination::MipLevels);
      std::vector<GpuTileInfo> infos;
      bool valid =
          total.size == 8192 &&
          regions.size() ==
              depth + std::max(depth >> 1u, 1u) + std::max(depth >> 2u, 1u) +
                  std::max(depth >> 3u, 1u) + std::max(depth >> 4u, 1u) &&
          TextureBuildGpuTileInfos(total.size, regions, layout, tail.format,
                                   depth, levels, infos) &&
          infos.size() == 6;
      const u32 table = std::countr_zero(tail.bytes);
      for (u32 level = 0; level < levels && valid; level++) {
        const auto &info = infos[level == 0 ? 0 : level + 1];
        valid &= info.tail && info.tail_x == volume_tail_xy[table][level][0] &&
                 info.tail_y == volume_tail_xy[table][level][1] &&
                 info.tiled_offset == 0;
      }
      if (valid) {
        valid = infos[1].tail &&
                infos[1].tail_x == volume_tail_xy[table][0][0] &&
                infos[1].tail_y == volume_tail_xy[table][0][1] &&
                infos[1].tiled_offset == 4096;
      }
      Require(
          name, "3D mip tail infos", valid,
          "Standard4KB3D mip tail coordinates or block-slice packing changed");
      check_round_trip("3D mip tail", total.size, infos);
    }

    for (const auto family :
         {TileBlockFamily::Standard4KB, TileBlockFamily::Standard64KB,
          TileBlockFamily::Prt64KB, TileBlockFamily::RenderTarget64KB,
          TileBlockFamily::Depth64KB}) {
      for (u32 bpe = 1; bpe <= 16; bpe <<= 1u) {
        if (family == TileBlockFamily::Depth64KB && bpe == 16)
          continue;
        TileBlockLayout block{};
        Require(name, "tail layout", TileGetBlockLayout(family, bpe, block),
                "tail family/BPE has no block layout");
        const u32 width = std::max(block.block_width / 4u, 1u);
        const u32 height = std::max(block.block_height / 4u, 1u);
        const u32 x = block.block_width / 2u;
        const u32 y = block.block_height / 2u;
        const u32 pitch = width;
        const uint64_t linear_size =
            static_cast<uint64_t>(pitch) * height * bpe;
        std::vector<uint8_t> tiled(block.block_size);
        std::vector<uint8_t> cpu(linear_size, 0xcd);
        std::vector<uint8_t> gpu(linear_size, 0xab);
        fill(&tiled, ++case_index);
        GpuTileInfo info{};
        info.family = block.family;
        info.bytes_per_element = block.bytes_per_element;
        info.linear_size = linear_size;
        info.tiled_size = block.block_size;
        info.width = width;
        info.height = height;
        info.pitch = pitch;
        info.tail = true;
        info.tail_x = x;
        info.tail_y = y;
        info.surface_z = family == TileBlockFamily::RenderTarget64KB ||
                                 family == TileBlockFamily::Depth64KB
                             ? 2
                             : 0;
        convert_reference(false, &cpu, tiled, info);
		GpuDetile(tiled.data(), gpu.data(), block.block_size,
                  linear_size, std::span<const GpuTileInfo>(&info, 1));
        compare("tail bytes", cpu, gpu);

        std::vector<uint8_t> linear(linear_size);
        std::vector<uint8_t> cpu_tiled(block.block_size, 0);
        std::vector<uint8_t> gpu_tiled(block.block_size, 0xab);
        fill(&linear, 0x400u + case_index);
        convert_reference(true, &cpu_tiled, linear, info);
		GpuTile(linear.data(), gpu_tiled.data(),
                block.block_size, linear_size,
                std::span<const GpuTileInfo>(&info, 1));
        compare("tail tile bytes", cpu_tiled, gpu_tiled);
      }
    }

    TileBlockLayout small_block{};
    Require(name, "small layout",
            TileGetBlockLayout(TileBlockFamily::Standard256B, 4, small_block),
            "small fixture layout is unavailable");
    std::vector<uint8_t> small_input(small_block.block_size);
    std::vector<uint8_t> small_expected(small_block.block_size, 0);
    std::vector<uint8_t> small_output(small_block.block_size, 0xab);
    fill(&small_input, 0xee);
    GpuTileInfo small_info{};
    small_info.family = small_block.family;
    small_info.bytes_per_element = small_block.bytes_per_element;
    small_info.linear_size = small_output.size();
    small_info.tiled_size = small_input.size();
    small_info.width = 1;
    small_info.height = 1;
    small_info.pitch = small_block.block_width;
    convert_reference(false, &small_expected, small_input, small_info);
	GpuDetile(small_input.data(), small_output.data(),
              small_input.size(), small_output.size(),
              std::span<const GpuTileInfo>(&small_info, 1));
    compare("small detile", small_expected, small_output);

	GpuTileRelease();
    std::fill(small_output.begin(), small_output.end(), 0xab);
	GpuDetile(small_input.data(), small_output.data(),
              small_input.size(), small_output.size(),
              std::span<const GpuTileInfo>(&small_info, 1));
    compare("release recovery", small_expected, small_output);
    std::printf("[gpu]     %-32s ok (%u cases, %u format/mode pairs)\n", name,
                case_index, format_cases);
  }

private:
  void EnsureRuntimeContext() {
    if (m_runtime_context.allocator != nullptr) {
      return;
    }
    m_runtime_context.instance = m_instance;
    m_runtime_context.physical_device = m_physical_device;
    m_runtime_context.device = m_device;
    m_physical_device.getProperties(
        &m_runtime_context.physical_device_properties);
    m_runtime_context.physical_device_memory_properties = m_memory_properties;
    m_runtime_context.queue_family = m_queue_family;
    m_runtime_context.queue = m_queue;

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr =
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr =
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocator_info{};
    allocator_info.instance = m_instance;
    allocator_info.physicalDevice = m_physical_device;
    allocator_info.device = m_device;
    allocator_info.pVulkanFunctions = &functions;
    allocator_info.vulkanApiVersion = VK_API_VERSION_1_2;
    RequireVk("VulkanHarness", "runtime context",
              static_cast<vk::Result>(vmaCreateAllocator(
                  &allocator_info, &m_runtime_context.allocator)),
              "vmaCreateAllocator");
    GraphicsRenderInit(m_runtime_context);
  }

  void Init() {
    static vk::detail::DynamicLoader loader;
    const auto get_instance_proc_addr =
        loader.getProcAddress<PFN_vkGetInstanceProcAddr>(
            "vkGetInstanceProcAddr");
    Require("VulkanHarness", "dispatch", get_instance_proc_addr != nullptr,
            "could not load the Vulkan loader");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(get_instance_proc_addr);

    vk::ApplicationInfo app{};
    app.sType = vk::StructureType::eApplicationInfo;
    app.pApplicationName = "ShaderRecompilerComputeTests";
    app.apiVersion = VK_API_VERSION_1_2;

    vk::InstanceCreateInfo instance_info{};
    instance_info.sType = vk::StructureType::eInstanceCreateInfo;
    instance_info.pApplicationInfo = &app;
    RequireVk("VulkanHarness", "dispatch",
              vk::createInstance(&instance_info, nullptr, &m_instance),
              "vkCreateInstance");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);

    u32 physical_count = 0;
    RequireVk("VulkanHarness", "dispatch",
              m_instance.enumeratePhysicalDevices(&physical_count, nullptr),
              "vkEnumeratePhysicalDevices");
    Require("VulkanHarness", "dispatch", physical_count != 0,
            "no Vulkan physical devices");
    std::vector<vk::PhysicalDevice> physical_devices(physical_count);
    RequireVk("VulkanHarness", "dispatch",
              m_instance.enumeratePhysicalDevices(&physical_count,
                                                  physical_devices.data()),
              "vkEnumeratePhysicalDevices");

    for (auto physical : physical_devices) {
      u32 queue_count = 0;
      physical.getQueueFamilyProperties(&queue_count, nullptr);
      std::vector<vk::QueueFamilyProperties> queues(queue_count);
      physical.getQueueFamilyProperties(&queue_count, queues.data());
      for (u32 i = 0; i < queue_count; i++) {
        if ((queues[i].queueFlags &
             (vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eGraphics)) ==
            (vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eGraphics)) {
          m_physical_device = physical;
          m_queue_family = i;
          break;
        }
      }
      if (m_physical_device != nullptr) {
        break;
      }
    }
    Require("VulkanHarness", "dispatch", m_physical_device != nullptr,
            "no Vulkan graphics+compute queue family");
    m_physical_device.getMemoryProperties(&m_memory_properties);

    vk::PhysicalDeviceFeatures available_features{};
    m_physical_device.getFeatures(&available_features);
    Require("VulkanHarness", "dispatch",
            available_features.shaderStorageImageWriteWithoutFormat == true,
            "shaderStorageImageWriteWithoutFormat is not supported");
    Require("VulkanHarness", "dispatch",
            available_features.shaderStorageImageReadWithoutFormat == true,
            "shaderStorageImageReadWithoutFormat is not supported");

    float priority = 1.0f;
    vk::DeviceQueueCreateInfo queue_info{};
    queue_info.sType = vk::StructureType::eDeviceQueueCreateInfo;
    queue_info.queueFamilyIndex = m_queue_family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;

    vk::DeviceCreateInfo device_info{};
    device_info.sType = vk::StructureType::eDeviceCreateInfo;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    vk::PhysicalDeviceFeatures device_features{};
    device_features.shaderStorageImageWriteWithoutFormat = true;
    device_features.shaderStorageImageReadWithoutFormat = true;
    device_info.pEnabledFeatures = &device_features;
    RequireVk("VulkanHarness", "dispatch",
              m_physical_device.createDevice(&device_info, nullptr, &m_device),
              "vkCreateDevice");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device);
    m_device.getQueue(m_queue_family, 0, &m_queue);

    vk::CommandPoolCreateInfo pool_info{};
    pool_info.sType = vk::StructureType::eCommandPoolCreateInfo;
    pool_info.queueFamilyIndex = m_queue_family;
    pool_info.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    RequireVk("VulkanHarness", "dispatch",
              m_device.createCommandPool(&pool_info, nullptr, &m_command_pool),
              "vkCreateCommandPool");
  }

  void Destroy() {
    if (m_device != nullptr) {
      RequireVulkanSuccess(m_device.waitIdle(), "vkDeviceWaitIdle");
      if (m_runtime_context.allocator != nullptr) {
        GraphicsRenderReleaseThreadCommandPool();
        Transfer::ReleaseCachedResources();
        vmaDestroyAllocator(m_runtime_context.allocator);
        m_runtime_context.allocator = nullptr;
      }
      if (m_command_pool != nullptr) {
        m_device.destroyCommandPool(m_command_pool, nullptr);
      }
      m_device.destroy(nullptr);
    }
    if (m_instance != nullptr) {
      m_instance.destroy(nullptr);
    }
  }

  bool FindMemoryType(u32 type_bits, vk::MemoryPropertyFlags required,
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

  static vk::AccessFlags AccessForLayout(vk::ImageLayout layout) {
    switch (layout) {
    case vk::ImageLayout::eTransferDstOptimal:
      return vk::AccessFlagBits::eTransferWrite;
    case vk::ImageLayout::eTransferSrcOptimal:
      return vk::AccessFlagBits::eTransferRead;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
      return vk::AccessFlagBits::eShaderRead;
    case vk::ImageLayout::eGeneral:
      return vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eShaderWrite;
    default:
      return {};
    }
  }

  vk::CommandBuffer BeginCommands(const char *shader_name, const char *stage) {
    vk::CommandBufferAllocateInfo cmd_alloc{};
    cmd_alloc.sType = vk::StructureType::eCommandBufferAllocateInfo;
    cmd_alloc.commandPool = m_command_pool;
    cmd_alloc.level = vk::CommandBufferLevel::ePrimary;
    cmd_alloc.commandBufferCount = 1;
    vk::CommandBuffer cmd = nullptr;
    RequireVk(shader_name, stage,
              m_device.allocateCommandBuffers(&cmd_alloc, &cmd),
              "vkAllocateCommandBuffers");

    vk::CommandBufferBeginInfo begin{};
    begin.sType = vk::StructureType::eCommandBufferBeginInfo;
    begin.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
    RequireVk(shader_name, stage, cmd.begin(&begin), "vkBeginCommandBuffer");
    return cmd;
  }

  void EndSubmitAndFree(const char *shader_name, const char *stage,
                        vk::CommandBuffer cmd) {
    RequireVk(shader_name, stage, cmd.end(), "vkEndCommandBuffer");

    vk::FenceCreateInfo fence_info{};
    fence_info.sType = vk::StructureType::eFenceCreateInfo;
    vk::Fence fence = nullptr;
    RequireVk(shader_name, stage,
              m_device.createFence(&fence_info, nullptr, &fence),
              "vkCreateFence");

    vk::SubmitInfo submit{};
    submit.sType = vk::StructureType::eSubmitInfo;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    RequireVk(shader_name, stage, m_queue.submit(1, &submit, fence),
              "vkQueueSubmit");
    RequireVk(shader_name, stage,
              m_device.waitForFences(1, &fence, true, UINT64_MAX),
              "vkWaitForFences");

    m_device.destroyFence(fence, nullptr);
    m_device.freeCommandBuffers(m_command_pool, 1, &cmd);
  }

  void AddImageBarrier(vk::CommandBuffer cmd, vk::Image image,
                       vk::ImageLayout old_layout, vk::ImageLayout new_layout,
                       vk::PipelineStageFlags src_stage,
                       vk::PipelineStageFlags dst_stage,
                       vk::AccessFlags src_access, vk::AccessFlags dst_access,
                       u32 mip_levels = 1) {
    vk::ImageMemoryBarrier barrier{};
    barrier.sType = vk::StructureType::eImageMemoryBarrier;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    cmd.pipelineBarrier(src_stage, dst_stage, {}, 0, nullptr, 0, nullptr, 1,
                        &barrier);
  }

  void TransitionImage(const char *shader_name, Image *image,
                       vk::ImageLayout new_layout,
                       vk::PipelineStageFlags src_stage,
                       vk::PipelineStageFlags dst_stage,
                       vk::AccessFlags src_access, vk::AccessFlags dst_access) {
    vk::CommandBuffer cmd = BeginCommands(shader_name, "dispatch");
    AddImageBarrier(cmd, image->image, image->layout, new_layout, src_stage,
                    dst_stage, src_access, dst_access, image->mip_levels);
    EndSubmitAndFree(shader_name, "dispatch", cmd);
    image->layout = new_layout;
  }

  void UploadImage(const char *shader_name, Image *image, vk::Buffer staging,
                   vk::ImageLayout final_layout) {
    UploadImageMips(shader_name, image, staging, final_layout);
  }

  void UploadImageMips(const char *shader_name, Image *image,
                       vk::Buffer staging, vk::ImageLayout final_layout) {
    vk::CommandBuffer cmd = BeginCommands(shader_name, "dispatch");
    AddImageBarrier(cmd, image->image, vk::ImageLayout::eUndefined,
                    vk::ImageLayout::eTransferDstOptimal,
                    vk::PipelineStageFlagBits::eTopOfPipe,
                    vk::PipelineStageFlagBits::eTransfer, {},
                    vk::AccessFlagBits::eTransferWrite, image->mip_levels);
    std::vector<vk::BufferImageCopy> copies;
    copies.reserve(image->mip_levels);
    vk::DeviceSize offset = 0;
    for (u32 level = 0; level < image->mip_levels; level++) {
      vk::BufferImageCopy copy{};
      copy.bufferOffset = offset;
      copy.imageSubresource.aspectMask = vk::ImageAspectFlagBits::eColor;
      copy.imageSubresource.mipLevel = level;
      copy.imageSubresource.baseArrayLayer = 0;
      copy.imageSubresource.layerCount = 1;
      copy.imageExtent.width = MipExtent(image->width, level);
      copy.imageExtent.height = MipExtent(image->height, level);
      copy.imageExtent.depth = 1;
      copies.push_back(copy);
      offset += static_cast<vk::DeviceSize>(
          ImageMipDwordCount(image->width, image->height,
                             image->dwords_per_pixel, level) *
          sizeof(u32));
    }
    cmd.copyBufferToImage(staging, image->image,
                          vk::ImageLayout::eTransferDstOptimal,
                          static_cast<u32>(copies.size()), copies.data());
    AddImageBarrier(cmd, image->image, vk::ImageLayout::eTransferDstOptimal,
                    final_layout, vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eComputeShader,
                    vk::AccessFlagBits::eTransferWrite,
                    AccessForLayout(final_layout), image->mip_levels);
    EndSubmitAndFree(shader_name, "dispatch", cmd);
    image->layout = final_layout;
  }

  Buffer CreateHostBuffer(const char *shader_name, vk::DeviceSize size,
                          vk::BufferUsageFlags usage,
                          const std::vector<u32> &contents) {
    Buffer ret;
    ret.size = std::max<vk::DeviceSize>(size, sizeof(u32));

    vk::BufferCreateInfo buffer_info{};
    buffer_info.sType = vk::StructureType::eBufferCreateInfo;
    buffer_info.size = ret.size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = vk::SharingMode::eExclusive;
    RequireVk(shader_name, "dispatch",
              m_device.createBuffer(&buffer_info, nullptr, &ret.buffer),
              "vkCreateBuffer");

    vk::MemoryRequirements req{};
    m_device.getBufferMemoryRequirements(ret.buffer, &req);
    u32 memory_type = 0;
    ret.coherent = FindMemoryType(req.memoryTypeBits,
                                  vk::MemoryPropertyFlagBits::eHostVisible |
                                      vk::MemoryPropertyFlagBits::eHostCoherent,
                                  &memory_type);
    if (!ret.coherent) {
      Require(shader_name, "dispatch",
              FindMemoryType(req.memoryTypeBits,
                             vk::MemoryPropertyFlagBits::eHostVisible,
                             &memory_type),
              "no host-visible memory type for staging buffer");
    }

    vk::MemoryAllocateInfo alloc{};
    alloc.sType = vk::StructureType::eMemoryAllocateInfo;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = memory_type;
    RequireVk(shader_name, "dispatch",
              m_device.allocateMemory(&alloc, nullptr, &ret.memory),
              "vkAllocateMemory");
    RequireVk(shader_name, "dispatch",
              m_device.bindBufferMemory(ret.buffer, ret.memory, 0),
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
              m_device.mapMemory(buffer.memory, 0, buffer.size, {}, &data),
              "vkMapMemory");
    std::memcpy(data, contents.data(), contents.size() * sizeof(u32));
    if (!buffer.coherent) {
      vk::MappedMemoryRange range{};
      range.sType = vk::StructureType::eMappedMemoryRange;
      range.memory = buffer.memory;
      range.offset = 0;
      range.size = VK_WHOLE_SIZE;
      RequireVk(shader_name, "dispatch",
                m_device.flushMappedMemoryRanges(1, &range),
                "vkFlushMappedMemoryRanges");
    }
    m_device.unmapMemory(buffer.memory);
  }

  vk::Instance m_instance = nullptr;
  vk::PhysicalDevice m_physical_device = nullptr;
  vk::Device m_device = nullptr;
  vk::Queue m_queue = nullptr;
  vk::CommandPool m_command_pool = nullptr;
  u32 m_queue_family = 0;
  vk::PhysicalDeviceMemoryProperties m_memory_properties{};
  GraphicContext m_runtime_context{};
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
            "storage image descriptor swizzle did not reach the specialized "
            "program");
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
  vk::Sampler sampler = nullptr;
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
          vk::Format::eR32G32B32A32Sfloat, vk::ImageUsageFlagBits::eSampled,
          test.sampled_image_rgba_mips, 4,
          vk::ImageLayout::eShaderReadOnlyOptimal);
    } else {
      sampled_image = vulkan->CreateImage2D(
          test.name, test.image_width, test.image_height,
          test.sampled_image_format, vk::ImageUsageFlagBits::eSampled,
          test.sampled_image_rgba, test.sampled_image_dwords_per_pixel,
          vk::ImageLayout::eShaderReadOnlyOptimal);
    }
  }
  if (needs_storage_image) {
    storage_image = vulkan->CreateImage2D(
        test.name, test.image_width, test.image_height,
        test.storage_image_format, vk::ImageUsageFlagBits::eStorage,
        test.storage_image_rgba, test.storage_image_dwords_per_pixel,
        vk::ImageLayout::eGeneral);
    storage_image_uint = vulkan->CreateImage2D(
        test.name, test.image_width, test.image_height, vk::Format::eR32Uint,
        vk::ImageUsageFlagBits::eStorage, test.storage_image_r32ui, 1,
        vk::ImageLayout::eGeneral);
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
  if (sampler != nullptr) {
    vulkan->Device().destroySampler(sampler, nullptr);
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
  AppendTBufferLoadFormatOpcode(&code, 0x00, 0, 20,
                                Prospero::BufferFormat::k16Float);
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
  AppendTBufferLoadFormatOpcode(&code, 0x01, 0, 20,
                                Prospero::BufferFormat::k8_8SNorm);
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
      EncodeVop1(0x01, 1, 0),          EncodeVop2(0x1a, 1, InlineU32(2), 1),
      EncodeVop2(0x25, 1, Vgpr(0), 1), EncodeVop2(0x1a, 3, InlineU32(2), 1),
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
  test.sampled_image_format = vk::Format::eR32Uint;
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
  AppendVMovLiteral(&code, 1, 0x3f008081u);
  AppendVMovLiteral(&code, 2, 0x3e808081u);
  AppendVMovLiteral(&code, 3, 0x3f40c0c1u);
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
  test.storage_image_format = vk::Format::eR8G8B8A8Unorm;
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
  AppendVMovLiteral(&code, 1, 0x3f008081u); // 128/255
  AppendVMovLiteral(&code, 2, 0x3e808081u); // 64/255
  AppendVMovLiteral(&code, 3, 0x3f3fbfc0u); // 191/255
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
  test.storage_image_format = vk::Format::eR8G8B8A8Unorm;
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
  test.user_data =
      MakeStorageTextureData(Prospero::BufferFormat::k32_32_32_32Float);
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
  test.storage_image_format = vk::Format::eR32Sfloat;
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
  user_data[3] =
      (Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32_32_32UInt)
       << 12u) |
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
            ShaderRecompiler::TryRecompile(code, options, result, &error),
            error);
    ValidateSpirv("Ps5GameExampleImageClear", result.spirv);
    return result;
  };

  const auto code = MakeCode();
  auto positive = Compile("exact Prospero kernel", code);

  compute.stage.program =
      std::make_shared<const ShaderRecompiler::IR::Program>(positive.program);
  compute.stage.resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
          positive.resources);
  ShaderBufferResource descriptor{};
  u32 packed_clear = 0;
  uint64_t size = 0;
  Require("Ps5GameExampleImageClear", "runtime shape",
          ResolveComputeImageClear(compute, 64, 1, 1, 0x61u, descriptor,
                                   packed_clear, size) &&
              descriptor.Base48() == 0x10000u && size == 64u * 16u &&
              packed_clear == 0xff000000u,
          "exact Prospero runtime binding did not resolve to a complete clear");

  auto non_repeated = positive.resources;
  non_repeated.user_data[7] ^= 1u;
  compute.stage.resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
          non_repeated);
  Require("Ps5GameExampleImageClear", "non-repeated clear",
          !ResolveComputeImageClear(compute, 64, 1, 1, 0x61u, descriptor,
                                    packed_clear, size),
          "non-uniform uint4 data was replaced with a color clear");
  compute.stage.resources =
      std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
          positive.resources);
  compute.dispatch_threads_num[0] = 32;
  Require("Ps5GameExampleImageClear", "partial dispatch",
          !ResolveComputeImageClear(compute, 32, 1, 1, 0x61u, descriptor,
                                    packed_clear, size),
          "partial buffer coverage was classified as a complete clear");
  std::printf("[host]    %-32s ok\n", "Ps5GameExampleImageClear");
}

void CheckEmbeddedFetchVertexOffset() {
  const auto MakeFetch = [](std::initializer_list<std::pair<u32, u32>> adds,
                            std::optional<std::pair<u32, u32>> late_add = {},
                            u32 accumulator_vgpr = 0, bool ngg_sad = false) {
    std::vector<u32> code;
    code.push_back(EncodeSMovB32(0, InlineU32(0)));
    code.push_back(EncodeSmem0(0x02u, 20, 4));
    code.push_back(EncodeSmem1(0));
    const auto AppendOffsets = [&]() {
      for (const auto [sgpr, index_vgpr] : adds) {
        if (ngg_sad) {
          AppendVop3(&code, 0x15du, accumulator_vgpr, sgpr, InlineU32(0),
                     Vgpr(index_vgpr));
        } else {
          AppendVop3B(&code, 0x30fu, accumulator_vgpr, 0, sgpr,
                      Vgpr(index_vgpr));
        }
      }
    };
    if (ngg_sad) {
      AppendOffsets();
    }
    code.push_back(EncodeVop2(0x01u, 0, Vgpr(8), 5));
    if (!ngg_sad) {
      AppendOffsets();
    }
    code.push_back(EncodeMubuf0(0x03u, 0, true));
    code.push_back(EncodeMubuf1(9, 5, 0));
    if (late_add.has_value()) {
      AppendVop3B(&code, 0x30fu, 0, 0, late_add->first, Vgpr(late_add->second));
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
            ShaderRecompiler::TryRecompile(code, options, result, &error),
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
        std::make_shared<const ShaderRecompiler::IR::ResourceSnapshot>(
            result.resources);
    return ResolveVertexOffset(index_offset, vertex);
  };

  const auto valid =
      Compile("EmbeddedFetchVertexOffset", MakeFetch({{18, 0}}), 7);
  Require(
      "EmbeddedFetchVertexOffset", "parse",
      valid.program.info.vertex_offset_sgpr == 18 && Resolve(valid, 0) == 7 &&
          Resolve(valid, 5) == 5,
      "canonical fetch offset or register index-offset precedence is wrong");

  const auto ngg_code = MakeFetch({{18, 5}}, {}, 5, true);
  Require("EmbeddedFetchNggVertexOffset", "encoding",
          ngg_code[3] == 0xd55d0005u && ngg_code[4] == 0x04150012u,
          "test does not encode the PS5 V_SAD_U32 vertex-offset prolog");
  const auto ngg = Compile("EmbeddedFetchNggVertexOffset", ngg_code, 8);
  Require("EmbeddedFetchNggVertexOffset", "parse",
          ngg.program.info.vertex_offset_sgpr == 18 && Resolve(ngg, 0) == 8 &&
              Resolve(ngg, 5) == 5,
          "PS5 NGG vertex-index offset or register index-offset precedence is "
          "wrong");

  const auto pointer = 0x5b7c5100u;
  const auto late = Compile("EmbeddedFetchLateOffset",
                            MakeFetch({}, std::pair<u32, u32>{18, 0}), pointer);
  const auto conflict = Compile("EmbeddedFetchConflictingOffset",
                                MakeFetch({{17, 0}, {18, 0}}), pointer);
  const auto malformed =
      Compile("EmbeddedFetchMalformedOffset", MakeFetch({{18, 1}}), pointer);
  const auto outside =
      Compile("EmbeddedFetchOutsideOffset", MakeFetch({{19, 0}}), pointer);
  for (const auto *result : {&late, &conflict, &malformed, &outside}) {
    Require("EmbeddedFetchVertexOffset", "fail closed",
            result->program.info.vertex_offset_sgpr == -1 &&
                Resolve(*result, 0) == 0,
            "non-prolog, conflicting, malformed, or out-of-window add was "
            "classified");
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

[[noreturn]] void RunReverseRenderTargetDeathCase() {
  (void)TextureGetRenderTargetFormat(12u, 7u, 3u);
  std::_Exit(0x7f);
}

void CheckReverseRenderTargetFormatContract() {
  const auto format = TextureGetRenderTargetFormat(12u, 7u, 2u);
  Require("ReverseRenderTarget", "exact format",
          format.format == vk::Format::eR16G16B16A16Sfloat &&
              format.bytes_per_element == 8u &&
              format.export_mapping == Prospero::ColorMappingAbgr,
          "exact reverse RGBA16F render-target tuple was rejected");
  Require("ReverseRenderTarget", "write masks",
          format.export_mapping.ApplyMask(0x1u) == 0x8u &&
              format.export_mapping.ApplyMask(0x2u) == 0x4u &&
              format.export_mapping.ApplyMask(0x4u) == 0x2u &&
              format.export_mapping.ApplyMask(0x8u) == 0x1u &&
              format.export_mapping.ApplyMask(0xfu) == 0xfu,
          "reverse RGBA16F component mask was not mapped exactly once");

  char path[MAX_PATH]{};
  Require("ReverseRenderTarget", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  std::string command = std::string("\"") + path + "\" --reverse-rt-death";
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  Require("ReverseRenderTarget", "host",
          CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                         FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &process) != 0,
          "CreateProcess failed");
  Require("ReverseRenderTarget", "host",
          WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
          "unsupported adjacent render-target tuple timed out");
  DWORD exit_code = 0;
  const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  Require(
      "ReverseRenderTarget", "hard failure", exited && exit_code == 321,
      "adjacent unproven render-target tuple did not retain the fatal guard");
  std::printf("[host]    %-32s ok\n", "ReverseRenderTargetFormat");
}

[[noreturn]] void RunImageViewDeathCase(const char *kind) {
  if (std::strcmp(kind, "sampled") == 0) {
    (void)SelectSampledColorView(vk::Format::eR8G8B8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 2));
  } else if (std::strcmp(kind, "sampled-compatible-swizzle") == 0) {
    (void)SelectSampledColorView(vk::Format::eB8G8R8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-compatible-reverse") == 0) {
    (void)SelectSampledColorView(vk::Format::eR8G8B8A8Unorm,
                                 vk::Format::eB8G8R8A8Unorm,
                                 DstSel(6, 5, 4, 7));
  } else if (std::strcmp(kind, "sampled-compatible-class") == 0) {
    (void)SelectSampledColorView(vk::Format::eR8G8B8A8Uint,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-compatible-colorspace") == 0) {
    (void)SelectSampledColorView(vk::Format::eB8G8R8A8Srgb,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-compatible-snorm") == 0) {
    (void)SelectSampledColorView(vk::Format::eR8G8B8A8Snorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-rgb10-swizzle") == 0) {
    (void)SelectSampledColorView(vk::Format::eA2R10G10B10UnormPack32,
                                 vk::Format::eA2B10G10R10UnormPack32,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "sampled-rgb10-reverse") == 0) {
    (void)SelectSampledColorView(vk::Format::eA2B10G10R10UnormPack32,
                                 vk::Format::eA2R10G10B10UnormPack32,
                                 DstSel(6, 5, 4, 7));
  } else if (std::strcmp(kind, "sampled-invalid-high") == 0) {
    (void)SelectSampledColorView(vk::Format::eR8G8B8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(7, 6, 5, 3));
  } else if (std::strcmp(kind, "sampled-depth-format") == 0) {
    (void)SelectSampledDepthView(vk::Format::eD24UnormS8Uint,
                                 vk::Format::eR32Sfloat, DstSel(4, 4, 4, 4));
  } else if (std::strcmp(kind, "sampled-depth-swizzle") == 0) {
    (void)SelectSampledDepthView(vk::Format::eD32SfloatS8Uint,
                                 vk::Format::eR32Sfloat, DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "storage") == 0) {
    (void)SelectStorageColorView(vk::Format::eR8G8B8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 0));
  } else if (std::strcmp(kind, "storage-format") == 0) {
    (void)SelectStorageColorView(vk::Format::eB8G8R8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "storage-compatible-swizzle") == 0) {
    (void)SelectStorageColorView(vk::Format::eB8G8R8A8Srgb,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7));
  } else if (std::strcmp(kind, "storage-abgr") == 0) {
    (void)SelectStorageColorView(vk::Format::eR16G16B16A16Sfloat,
                                 vk::Format::eR16G16B16A16Sfloat,
                                 DstSel(7, 6, 5, 4));
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
          SelectSampledColorView(vk::Format::eR8G8B8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7),
          "RGBA did not select the identity view");
  ShaderRecompiler::IR::ImageResource cube_resource{};
  cube_resource.kind = ShaderRecompiler::IR::ResourceKind::Image;
  cube_resource.dimension =
      ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
  cube_resource.read = true;
  const auto cube_view =
      ResolveTargetTextureView(cube_resource, Prospero::ImageType::kCube, 0, 6);
  Require("SampledColorViews", "PPSA17337 cubemap render target",
          cube_view.type == vk::ImageViewType::e2DArray &&
              cube_view.base_layer == 0 && cube_view.layer_count == 6,
          "captured six-face cubemap did not resolve to a 2D-array view");
  const auto cube_array_view = ResolveTargetTextureView(
      cube_resource, Prospero::ImageType::kCube, 6, 18);
  Require("SampledColorViews", "cubemap array subview",
          cube_array_view.type == vk::ImageViewType::e2DArray &&
              cube_array_view.base_layer == 6 &&
              cube_array_view.layer_count == 12,
          "nonzero-base multi-cube view did not preserve whole face groups");
  auto non_array_cube_resource = cube_resource;
  non_array_cube_resource.dimension =
      ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  Require("SampledColorViews", "cubemap hard guards",
          ResolveTargetTextureView(non_array_cube_resource,
                                   Prospero::ImageType::kCube, 0, 6)
                      .type ==
                  static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM) &&
              ResolveTargetTextureView(cube_resource,
                                       Prospero::ImageType::kCube, 0, 7)
                      .type ==
                  static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM) &&
              ResolveTargetTextureView(cube_resource,
                                       Prospero::ImageType::kCube, 1, 6)
                      .type ==
                  static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM) &&
              ResolveTargetTextureView(cube_resource,
                                       Prospero::ImageType::kCube, 6, 6)
                      .type ==
                  static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM),
          "non-array or partial cubemap views were accepted");
  uint32_t valid_swizzles = 0;
  for (uint32_t swizzle = 0; swizzle <= 0xfffu; swizzle++) {
    bool expected = true;
    for (uint32_t channel = 0; channel < 4; channel++) {
      const auto selector = GetDstSel(swizzle, channel);
      if (selector == 2 || selector == 3) {
        expected = false;
      }
    }
    const bool valid = IsValidSampledColorSwizzle(swizzle);
    const bool supported = IsSupportedSampledColorView(
        vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm, swizzle);
    Require("SampledColorViews", "exhaustive read swizzle domain",
            valid == expected && supported == expected,
            "sampled swizzle validator disagreed with the PS5 selector domain");
    valid_swizzles += valid;
  }
  Require("SampledColorViews", "all PS5 read swizzles",
          valid_swizzles == 1296 &&
              !IsValidSampledColorSwizzle(DstSel(4, 5, 6, 2)) &&
              !IsValidSampledColorSwizzle(DstSel(4, 5, 6, 3)) &&
              !IsValidSampledColorSwizzle(0x1000),
          "valid PS5 sampled mappings were rejected or reserved selectors were "
          "admitted");
  const auto arbitrary = DstSel(5, 1, 7, 0);
  const auto components = TextureGetComponentMapping(arbitrary);
  Require("SampledColorViews", "generic Vulkan component mapping",
          SelectSampledColorView(vk::Format::eR8G8B8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 arbitrary) == arbitrary &&
              components.r == vk::ComponentSwizzle::eG &&
              components.g == vk::ComponentSwizzle::eOne &&
              components.b == vk::ComponentSwizzle::eA &&
              components.a == vk::ComponentSwizzle::eZero,
          "arbitrary valid sampled mapping did not use the generic view path");
  Require("SampledColorViews", "R8 R001",
          SelectSampledColorView(vk::Format::eR8Unorm, vk::Format::eR8Unorm,
                                 DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
          "R8 did not select its R001 view");
  Require("SampledColorViews", "R8 000R",
          SelectSampledColorView(vk::Format::eR8Unorm, vk::Format::eR8Unorm,
                                 DstSel(0, 0, 0, 4)) == DstSel(0, 0, 0, 4),
          "R8 did not select its 000R component-mapped view");
  Require("SampledColorViews", "R16G16 RG01",
          SelectSampledColorView(vk::Format::eR16G16Sfloat,
                                 vk::Format::eR16G16Sfloat,
                                 DstSel(4, 5, 0, 1)) == DstSel(4, 5, 0, 1),
          "R16G16 did not select its RG01 view");
  Require("SampledColorViews", "alpha one",
          SelectSampledColorView(vk::Format::eR8G8B8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 1)) == DstSel(4, 5, 6, 1),
          "RGB1 did not select the alpha-one view");
  Require("SampledColorViews", "mutable BGRA target",
          SelectSampledColorView(vk::Format::eB8G8R8A8Unorm,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
          "BGRA target did not select the exact RGBA/BGRA mutable view");
  Require(
      "SampledColorViews", "mutable sRGB view of UNORM BGRA target",
      SelectSampledColorView(vk::Format::eB8G8R8A8Unorm,
                             vk::Format::eR8G8B8A8Srgb,
                             DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
      "UNORM BGRA target did not select its compatible sRGB RGBA sampled view");
  Require("SampledColorViews", "mutable sRGB BGRA target",
          BgraToRgbaSampledViewFormat(vk::Format::eB8G8R8A8Srgb) ==
                  vk::Format::eR8G8B8A8Srgb &&
              SelectSampledColorView(vk::Format::eB8G8R8A8Srgb,
                                     vk::Format::eR8G8B8A8Srgb,
                                     DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
          "sRGB BGRA target did not select its matching mutable RGBA view");
  constexpr auto colorspace_swizzle = DstSel(5, 1, 7, 0);
  Require("SampledColorViews", "mutable sRGB/UNORM views",
          SelectSampledColorView(vk::Format::eR8G8B8A8Srgb,
                                 vk::Format::eR8G8B8A8Unorm,
                                 DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7) &&
              SelectSampledColorView(vk::Format::eB8G8R8A8Unorm,
                                     vk::Format::eB8G8R8A8Srgb,
                                     colorspace_swizzle) == colorspace_swizzle,
          "same-order mutable sRGB/UNORM sampled views were rejected");
  Require("SampledColorViews", "mutable packed RGB10 view",
          BgraToRgbaSampledViewFormat(vk::Format::eA2R10G10B10UnormPack32) ==
                  vk::Format::eA2B10G10R10UnormPack32 &&
              SelectSampledColorView(vk::Format::eA2R10G10B10UnormPack32,
                                     vk::Format::eA2B10G10R10UnormPack32,
                                     DstSel(6, 5, 4, 7)) == DstSel(6, 5, 4, 7),
          "packed RGB10 target did not select its matching mutable "
          "channel-order view");
  Require(
      "SampledColorViews", "reverse RGBA16F sampled view",
      SelectSampledColorView(vk::Format::eR16G16B16A16Sfloat,
                             vk::Format::eR16G16B16A16Sfloat,
                             DstSel(7, 6, 5, 4)) == DstSel(7, 6, 5, 4),
      "reverse RGBA16F target did not select its reciprocal ABGR sampled view");
  Require("SampledColorViews", "mutable integer-class views",
          SelectSampledColorView(vk::Format::eR16G16B16A16Sfloat,
                                 vk::Format::eR16G16B16A16Uint,
                                 DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7) &&
              SelectSampledColorView(
                  vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Uint,
                  DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7) &&
              SelectSampledColorView(vk::Format::eR8G8B8A8Uint,
                                     vk::Format::eR8G8B8A8Unorm,
                                     DstSel(4, 5, 6, 7)) == DstSel(4, 5, 6, 7),
          "compatible integer render-target sampled view was rejected");
  Require("SampledColorViews", "D32 depth target",
          SelectSampledDepthView(vk::Format::eD32SfloatS8Uint,
                                 vk::Format::eR32Sfloat,
                                 DstSel(4, 4, 4, 4)) == DstSel(4, 4, 4, 4),
          "D32 depth target did not select its depth-aspect view");
  Require("SampledColorViews", "D16 R000 depth target",
          SelectSampledDepthView(vk::Format::eD16Unorm, vk::Format::eR16Unorm,
                                 DstSel(4, 0, 0, 0)) == DstSel(4, 0, 0, 0),
          "D16 depth target did not select its R000 depth-aspect view");
  Require("SampledColorViews", "D16S8 R001 depth target",
          SelectSampledDepthView(vk::Format::eD16UnormS8Uint,
                                 vk::Format::eR16Unorm,
                                 DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
          "D16S8 depth target did not select its R001 depth-aspect view");
  Require("SampledColorViews", "promoted D24S8 R001 depth target",
          SelectSampledDepthView(vk::Format::eD24UnormS8Uint,
                                 vk::Format::eR16Unorm,
                                 DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
          "D24S8 host fallback did not preserve the guest R16 depth view");
  Require("SampledColorViews", "promoted D32S8 R001 depth target",
          SelectSampledDepthView(vk::Format::eD32SfloatS8Uint,
                                 vk::Format::eR16Unorm,
                                 DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
          "D32S8 host fallback did not preserve the guest R16 depth view");
  Require("SampledColorViews", "D32S8 R001 depth target",
          SelectSampledDepthView(vk::Format::eD32SfloatS8Uint,
                                 vk::Format::eR32Sfloat,
                                 DstSel(4, 0, 0, 1)) == DstSel(4, 0, 0, 1),
          "D32S8 depth target did not select its R001 depth-aspect view");
  Require("SampledColorViews", "storage identity",
          SelectStorageColorView(
              vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
              DstSel(4, 5, 6, 7)) == VulkanImage::VIEW_STORAGE,
          "RGBA storage did not select the identity storage view");
  Require("SampledColorViews", "storage RGB1 write mapping",
          SelectStorageColorView(
              vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
              DstSel(4, 5, 6, 1)) == VulkanImage::VIEW_STORAGE,
          "RGBA8 RGB1 storage did not select the identity storage view");
  Require("SampledColorViews", "storage BGRA write mapping",
          SelectStorageColorView(
              vk::Format::eR8G8B8A8Unorm, vk::Format::eR8G8B8A8Unorm,
              DstSel(6, 5, 4, 7)) == VulkanImage::VIEW_STORAGE,
          "RGBA8 BGRA storage did not select the identity storage view");
  Require("SampledColorViews", "storage uint BGRA write mapping",
          SelectStorageColorView(
              vk::Format::eR8G8B8A8Uint, vk::Format::eR8G8B8A8Uint,
              DstSel(6, 5, 4, 7)) == VulkanImage::VIEW_STORAGE,
          "RGBA8_UINT BGRA storage did not select the identity storage view");
  Require(
      "SampledColorViews", "mutable BGRA sRGB storage view",
      SelectStorageColorView(vk::Format::eB8G8R8A8Srgb,
                             vk::Format::eR8G8B8A8Unorm,
                             DstSel(6, 5, 4, 7)) == VulkanImage::VIEW_STORAGE,
      "BGRA sRGB target did not select its RGBA UNORM identity storage view");
  Require("SampledColorViews", "storage YZWX write mapping",
          SelectStorageColorView(
              vk::Format::eR32G32B32A32Sfloat, vk::Format::eR32G32B32A32Sfloat,
              DstSel(5, 6, 7, 4)) == VulkanImage::VIEW_STORAGE,
          "RGBA32F YZWX storage did not select the identity storage view");
  Require("SampledColorViews", "storage R32 uint R001 write mapping",
          SelectStorageColorView(vk::Format::eR32Uint, vk::Format::eR32Uint,
                                 DstSel(4, 0, 0, 1)) ==
              VulkanImage::VIEW_STORAGE,
          "R32_UINT R001 storage did not select the identity storage view");
  Require("SampledColorViews", "storage R16 uint R000 write mapping",
          SelectStorageColorView(vk::Format::eR16Uint, vk::Format::eR16Uint,
                                 DstSel(4, 0, 0, 0)) ==
              VulkanImage::VIEW_STORAGE,
          "R16_UINT R000 storage did not select the identity storage view");
  Require("SampledColorViews", "storage R16 float R001 write mapping",
          SelectStorageColorView(vk::Format::eR16Sfloat, vk::Format::eR16Sfloat,
                                 DstSel(4, 0, 0, 1)) ==
              VulkanImage::VIEW_STORAGE,
          "R16_SFLOAT R001 storage did not select the identity storage view");
  Require("SampledColorViews", "storage R32 float R001 write mapping",
          SelectStorageColorView(vk::Format::eR32Sfloat, vk::Format::eR32Sfloat,
                                 DstSel(4, 0, 0, 1)) ==
              VulkanImage::VIEW_STORAGE,
          "R32_SFLOAT R001 storage did not select the identity storage view");
  Require("SampledColorViews", "storage R8 unorm R001 write mapping",
          SelectStorageColorView(vk::Format::eR8Unorm, vk::Format::eR8Unorm,
                                 DstSel(4, 0, 0, 1)) ==
              VulkanImage::VIEW_STORAGE,
          "R8_UNORM R001 storage did not select the identity storage view");
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
  storage_resource.dimension =
      ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
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
  for (const char *kind : {"sampled",
                           "sampled-compatible-swizzle",
                           "sampled-compatible-reverse",
                           "sampled-compatible-colorspace",
                           "sampled-compatible-snorm",
                           "sampled-rgb10-swizzle",
                           "sampled-rgb10-reverse",
                           "sampled-invalid-high",
                           "sampled-depth-format",
                           "sampled-depth-swizzle",
                           "storage",
                           "storage-format",
                           "storage-compatible-swizzle",
                           "storage-abgr",
                           "storage-kind",
                           "storage-no-write",
                           "storage-atomic",
                           "storage-compare",
                           "storage-mip",
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
            std::string(kind) +
                " component mapping did not report a fatal error");
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
  Require("SampledDepthResource", "singleton array accepted",
          IsSupportedSampledDepthResource(resource),
          "array depth resource was rejected");
  const auto singleton_array_view = ResolveTargetTextureView(
      resource, Prospero::ImageType::kColor2DArray, 0, 1);
  Require("SampledDepthResource", "singleton array view",
          singleton_array_view.type == vk::ImageViewType::e2DArray &&
              singleton_array_view.base_layer == 0 &&
              singleton_array_view.layer_count == 1,
          "singleton depth array did not preserve the shader array view type");
  Require(
      "SampledDepthResource", "array type mismatch rejected",
      ResolveTargetTextureView(resource, Prospero::ImageType::kColor2D, 0, 1)
              .type ==
          static_cast<vk::ImageViewType>(VK_IMAGE_VIEW_TYPE_MAX_ENUM),
      "array shader resource accepted a non-array descriptor view");
  resource = basic;
  resource.kind = ShaderRecompiler::IR::ResourceKind::ImageUint;
  Require("SampledDepthResource", "integer rejected",
          !IsSupportedSampledDepthResource(resource),
          "integer depth resource was accepted");
  Require("SampledDepthResource", "integer reinterpret resource",
          IsSupportedSampledDepthUintResource(resource),
          "read-only uint depth reinterpretation resource was rejected");
  Require("SampledDepthResource", "D32 to R32_UINT",
          IsDepthUintTextureReinterpretation(
              vk::Format::eD32Sfloat,
              Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt),
              vk::Format::eR32Uint) &&
              !IsDepthUintTextureReinterpretation(
                  vk::Format::eD16Unorm,
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt),
                  vk::Format::eR32Uint),
          "depth uint reinterpretation format boundary changed");
  std::printf("[host]    %-32s ok\n", "SampledDepthResource");
}

void CheckSampledVideoOutView() {
  ShaderRecompiler::IR::ImageResource resource{};
  resource.kind = ShaderRecompiler::IR::ResourceKind::Image;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = true;

  ShaderTextureResource descriptor{};
  descriptor.fields[3] = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D)
                         << 28u;
  VulkanImage image{VulkanImageType::VideoOut};
  image.layers = 1;
  Require("SampledVideoOutView", "basic 2D",
          IsSupportedSampledVideoOutView(resource, descriptor, image),
          "basic 2D video-out view was rejected");

  const auto basic_resource = resource;
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
  const bool rejects_array_resource =
      !IsSupportedSampledVideoOutView(resource, descriptor, image);
  resource = basic_resource;
  descriptor.fields[3] =
      Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) << 28u;
  const bool rejects_array_descriptor =
      !IsSupportedSampledVideoOutView(resource, descriptor, image);
  descriptor.fields[3] = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D)
                         << 28u;
  descriptor.fields[4] = 1u << 16u;
  const bool rejects_base_layer =
      !IsSupportedSampledVideoOutView(resource, descriptor, image);
  descriptor.fields[4] = 1u;
  const bool rejects_layer_count =
      !IsSupportedSampledVideoOutView(resource, descriptor, image);
  descriptor.fields[4] = 0;
  image.layers = 2;
  const bool rejects_layered_image =
      !IsSupportedSampledVideoOutView(resource, descriptor, image);
  Require("SampledVideoOutView", "array hard failures",
          rejects_array_resource && rejects_array_descriptor &&
              rejects_base_layer && rejects_layer_count &&
              rejects_layered_image,
          "unsupported layered video-out view was accepted");
  std::printf("[host]    %-32s ok\n", "SampledVideoOutView");
}

void CheckSampledDepthDescriptor() {
  ShaderTextureResource descriptor{{0x00eb0900u, 0xc1600000u, 0x00bcc14fu,
                                    0x91800924u, 0x00000000u, 0x00700000u,
                                    0x00000000u, 0x00000000u}};
  DepthStencilVulkanImage image;
  image.extent = {1344, 756};
  image.guest_pitch = 1408;
  image.layers = 1;
  image.format = vk::Format::eD32SfloatS8Uint;
  Require("SampledDepthDescriptor", "PPSA04319 padded pitch",
          descriptor.Width5() + 1u == 1344 &&
              descriptor.Height5() + 1u == 756 &&
              TileGetTexturePitch(descriptor.Format(), 1344, 1,
                                  descriptor.TileMode()) == 1408 &&
              IsSupportedDepthTargetDescriptor(descriptor, image),
          "valid padded-pitch depth texture was rejected");

  descriptor.fields[3] =
      (descriptor.fields[3] & ~(0xfu << 28u)) |
      (Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) << 28u);
  Require("SampledDepthDescriptor", "singleton array descriptor",
          IsSupportedDepthTargetDescriptor(descriptor, image),
          "valid singleton-array depth texture was rejected");
  descriptor.fields[3] =
      (descriptor.fields[3] & ~(0xfu << 28u)) |
      (Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) << 28u);

  ShaderTextureResource cube_descriptor{{0x01267d00u, 0xc0700000u, 0x00ffc0ffu,
                                         0xb1800924u, 0x00000005u, 0x00700000u,
                                         0x00000000u, 0x00000000u}};
  DepthStencilVulkanImage cube_image;
  cube_image.extent = {1024, 1024};
  cube_image.guest_pitch = 1024;
  cube_image.layers = 6;
  cube_image.format = vk::Format::eD32Sfloat;
  ShaderRecompiler::IR::ImageResource cube_resource{};
  cube_resource.kind = ShaderRecompiler::IR::ResourceKind::Image;
  cube_resource.dimension =
      ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
  cube_resource.read = true;
  cube_resource.depth_compare = true;
  const auto cube_view = ResolveTargetTextureView(
      cube_resource, Prospero::ImageType::kCube, 0, cube_image.layers);
  Require("SampledDepthDescriptor", "PPSA06084 six-face depth cubemap",
          IsSupportedDepthTargetDescriptor(cube_descriptor, cube_image) &&
              IsSupportedDepthTextureEncoding(cube_descriptor) &&
              cube_view.type == vk::ImageViewType::e2DArray &&
              cube_view.base_layer == 0 && cube_view.layer_count == 6,
          "captured depth cubemap did not resolve to its layered target view");
  auto partial_cube = cube_descriptor;
  partial_cube.fields[4] = 4;
  auto based_cube = cube_descriptor;
  based_cube.fields[4] |= 1u << 16u;
  auto reserved_cube = cube_descriptor;
  reserved_cube.fields[4] |= 1u << 13u;
  auto non_square_cube = cube_descriptor;
  non_square_cube.fields[2] =
      (non_square_cube.fields[2] & ~(0x3fffu << 14u)) | (511u << 14u);
  DepthStencilVulkanImage non_square_image;
  non_square_image.extent = cube_image.extent;
  non_square_image.guest_pitch = cube_image.guest_pitch;
  non_square_image.layers = cube_image.layers;
  non_square_image.format = cube_image.format;
  non_square_image.extent.height = 512;
  Require(
      "SampledDepthDescriptor", "cubemap hard guards",
      !IsSupportedDepthTargetDescriptor(partial_cube, cube_image) &&
          !IsSupportedDepthTargetDescriptor(based_cube, cube_image) &&
          !IsSupportedDepthTextureEncoding(reserved_cube) &&
          !IsSupportedDepthTargetDescriptor(non_square_cube, non_square_image),
      "partial, based, non-square, or reserved-bit depth cubemap was accepted");

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

  DepthTargetInfo target{};
  target.address = 0x4a8c00000;
  target.size = 0x100000;
  target.width = 512;
  target.height = 512;
  target.pitch = 512;
  target.layers = 1;
  target.bytes_per_element = 4;
  target.guest_format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  target.format = vk::Format::eD32Sfloat;
  target.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  ImageInfo expanded{};
  expanded.address = target.address;
  expanded.size = 0x400000;
  expanded.format = target.guest_format;
  expanded.width = target.width;
  expanded.height = target.height;
  expanded.pitch = target.pitch;
  expanded.levels = 1;
  expanded.view_levels = 1;
  expanded.depth = 4;
  expanded.tile = target.tile_mode;
  expanded.type = Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray);
  Require("SampledDepthDescriptor", "PPSA01530 layer expansion",
          IsSampledDepthExpansion(expanded, target),
          "exact D32 per-layer expansion was rejected");
  auto invalid_expansion = expanded;
  invalid_expansion.pitch *= 2;
  const bool rejects_pitch =
      !IsSampledDepthExpansion(invalid_expansion, target);
  invalid_expansion = expanded;
  invalid_expansion.address++;
  const bool rejects_address =
      !IsSampledDepthExpansion(invalid_expansion, target);
  invalid_expansion = expanded;
  invalid_expansion.levels = 2;
  const bool rejects_mips = !IsSampledDepthExpansion(invalid_expansion, target);
  invalid_expansion = expanded;
  invalid_expansion.type =
      Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  const bool rejects_topology =
      !IsSampledDepthExpansion(invalid_expansion, target);
  auto metadata_target = target;
  metadata_target.htile_address = 0x4a9c00000;
  metadata_target.htile_size = 0x10000;
  const bool rejects_metadata =
      !IsSampledDepthExpansion(expanded, metadata_target);
  Require("SampledDepthDescriptor", "expansion boundaries",
          rejects_pitch && rejects_address && rejects_mips &&
              rejects_topology && rejects_metadata,
          "unsupported depth expansion topology was accepted");
  std::printf("[host]    %-32s ok\n", "SampledDepthDescriptor");
}

void CheckBufferCacheRangeMerge() {
  BufferCacheRange merged{0x10000, 0x4000};
  Require("BufferCacheRangeMerge", "disjoint",
          !MergeOverlappingBufferCacheRange(merged, {0x14000, 0x4000}) &&
              merged.address == 0x10000 && merged.size == 0x4000,
          "adjacent range was merged");
  Require("BufferCacheRangeMerge", "right growth",
          MergeOverlappingBufferCacheRange(merged, {0x12000, 0x8000}) &&
              merged.address == 0x10000 && merged.size == 0xa000,
          "right overlap did not grow the cache range");
  Require("BufferCacheRangeMerge", "left growth",
          MergeOverlappingBufferCacheRange(merged, {0xc000, 0x8000}) &&
              merged.address == 0xc000 && merged.size == 0xe000,
          "left overlap did not grow the cache range");
  Require("BufferCacheRangeMerge", "contained",
          MergeOverlappingBufferCacheRange(merged, {0x10000, 0x1000}) &&
              merged.address == 0xc000 && merged.size == 0xe000,
          "contained range changed the cache union");
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
  return {{0x00785d00u, 0x04700000u, 0x00080008u, 0xa1b00facu, 0x00000020u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicLinearStorageTextureResource() {
  auto resource = BasicStorageTextureResource();
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = false;
  return resource;
}

ShaderTextureResource BasicLinearStorageTextureDescriptor() {
  return {{0x04bcc401u, 0xc3800000u, 0x021bc3bfu, 0x900003acu, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicBgraStorageTextureResource() {
  auto resource = BasicStorageTextureResource();
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
  resource.read = false;
  return resource;
}

ShaderTextureResource BasicBgraStorageTextureDescriptor() {
  return {{0x007c6500u, 0xc3800000u, 0x010dc1dfu, 0x91b00f2eu, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa01530MaxMipStorageTextureDescriptor() {
  return {{0x04a42900u, 0xc3e00000u, 0x000fc00fu, 0x91b0022cu, 0x00000000u,
           0x00700050u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource Ppsa01530MaxMipStorageTextureResource() {
  auto resource = BasicBgraStorageTextureResource();
  resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
  return resource;
}

ShaderTextureResource Ppsa02527R16FloatStorageTextureDescriptor() {
  return {{0x00ce3500u, 0xc0d00000u, 0x010dc1dfu, 0x91b00204u, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa02527R32FloatStorageTextureDescriptor() {
  return {{0x00cea900u, 0xc1600000u, 0x0086c0efu, 0x91b00204u, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa02527R8UnormStorageTextureDescriptor() {
  return {{0x00c7d500u, 0xc0100000u, 0x0086c0efu, 0x91b00204u, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderTextureResource BasicYzwxStorageTextureDescriptor() {
  return {{0x00627801u, 0xc4d00000u, 0x0001c001u, 0x900009f5u, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicArrayStorageTextureResource() {
  auto resource = BasicLinearStorageTextureResource();
  resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2DArray;
  return resource;
}

ShaderTextureResource BasicArrayStorageTextureDescriptor() {
  return {{0x20179000u, 0x03800000u, 0x00000000u, 0xd1b00f2eu, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicUintArrayStorageTextureResource() {
  auto resource = BasicArrayStorageTextureResource();
  resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
  return resource;
}

ShaderTextureResource BasicUintArrayStorageTextureDescriptor() {
  return {{0x20179200u, 0x01400000u, 0x00000000u, 0xd1b00204u, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource Ppsa14053DepthTileStorageTextureResource() {
  return BasicUintArrayStorageTextureResource();
}

ShaderTextureResource Ppsa14053DepthTileStorageTextureDescriptor() {
  return {{0x20144c00u, 0x00500000u, 0x00000000u, 0xd1800204u, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderRecompiler::IR::ImageResource BasicUintVolumeStorageTextureResource() {
  auto resource = BasicStorageTextureResource();
  resource.kind = ShaderRecompiler::IR::ResourceKind::StorageImageUint;
  resource.read = false;
  return resource;
}

ShaderTextureResource BasicUintVolumeStorageTextureDescriptor() {
  return {{0x20180600u, 0xc0b00000u, 0x0003c003u, 0xa0000004u, 0x0000000fu,
           0x00700000u, 0x00000000u, 0x00000000u}};
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
  } else if (std::strcmp(kind, "r16-float-read") == 0) {
    resource = BasicBgraStorageTextureResource();
    descriptor = Ppsa02527R16FloatStorageTextureDescriptor();
    resource.read = true;
  } else if (std::strcmp(kind, "r8-unorm-read") == 0) {
    resource = BasicBgraStorageTextureResource();
    descriptor = Ppsa02527R8UnormStorageTextureDescriptor();
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
        (Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float)
         << 20u);
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
  } else if (std::strcmp(kind, "array-mip-view") == 0) {
    resource = BasicArrayStorageTextureResource();
    descriptor = BasicArrayStorageTextureDescriptor();
    descriptor.fields[3] |= (1u << 12u) | (1u << 16u);
    descriptor.fields[5] |= 1u << 4u;
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
  } else if (std::strcmp(kind, "depth-tile-read") == 0) {
    resource = Ppsa14053DepthTileStorageTextureResource();
    descriptor = Ppsa14053DepthTileStorageTextureDescriptor();
    resource.read = true;
  } else if (std::strcmp(kind, "depth-tile-extent") == 0) {
    resource = Ppsa14053DepthTileStorageTextureResource();
    descriptor = Ppsa14053DepthTileStorageTextureDescriptor();
    descriptor.fields[4] |= 1u;
  } else {
    std::_Exit(0x7e);
  }
  ValidateStorageTexture(resource, descriptor, 0x10000);
  std::_Exit(0x7f);
}

void CheckBasicStorageTextureDescriptor() {
  const auto descriptor = BasicStorageTextureDescriptor();
  Require("BasicStorageTexture", "descriptor",
          descriptor.Base40() == 0x785d0000ull &&
              descriptor.Width5() + 1u == 33 &&
              descriptor.Height5() + 1u == 33 && descriptor.Depth() + 1u == 33,
          "basic 3D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicStorageTextureResource(), descriptor, 0x10000);

  const auto linear = BasicLinearStorageTextureDescriptor();
  Require("BasicStorageTexture", "linear descriptor",
          linear.Base40() == 0x4bcc40100ull && linear.Width5() + 1u == 3840 &&
              linear.Height5() + 1u == 2160 && linear.Depth() + 1u == 1 &&
              linear.Format() == Prospero::GpuEnumValue(
                                     Prospero::BufferFormat::k8_8_8_8UNorm) &&
              linear.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kLinear),
          "PPSA07429 linear 2D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicLinearStorageTextureResource(), linear,
                         0x1fa4000);

  const auto bgra = BasicBgraStorageTextureDescriptor();
  Require("BasicStorageTexture", "BGRA descriptor",
          bgra.Base40() == 0x7c650000ull && bgra.Width5() + 1u == 1920 &&
              bgra.Height5() + 1u == 1080 && bgra.Depth() + 1u == 1 &&
              bgra.Format() == Prospero::GpuEnumValue(
                                   Prospero::BufferFormat::k8_8_8_8UNorm) &&
              bgra.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              bgra.DstSelXYZW() == DstSel(6, 5, 4, 7),
          "PPSA02604 BGRA 2D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicBgraStorageTextureResource(), bgra, 0x870000);

  const auto max_mip = Ppsa01530MaxMipStorageTextureDescriptor();
  Require("BasicStorageTexture", "PPSA01530 max-mip descriptor",
          max_mip.Base40() == 0x4a4290000ull && max_mip.Width5() + 1u == 64 &&
              max_mip.Height5() + 1u == 64 && max_mip.Depth() + 1u == 1 &&
              max_mip.BaseLevel() == 0 && max_mip.LastLevel() == 0 &&
              max_mip.MaxMip() == 5 &&
              max_mip.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k32_32UInt) &&
              max_mip.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              max_mip.DstSelXYZW() == DstSel(4, 5, 0, 1),
          "PPSA01530 max-mip storage descriptor fixture is malformed");
  ValidateStorageTexture(Ppsa01530MaxMipStorageTextureResource(), max_mip,
                         0x20000);
  auto mip_one = max_mip;
  mip_one.fields[3] |= (1u << 12u) | (1u << 16u);
  Require("BasicStorageTexture", "PPSA01530 mip-one descriptor",
          mip_one.BaseLevel() == 1 && mip_one.LastLevel() == 1 &&
              mip_one.MaxMip() == 5,
          "PPSA01530 mip-one storage descriptor fixture is malformed");
  ValidateStorageTexture(Ppsa01530MaxMipStorageTextureResource(), mip_one,
                         0x20000);

  const auto r16_float = Ppsa02527R16FloatStorageTextureDescriptor();
  Require("BasicStorageTexture", "PPSA02527 R16F descriptor",
          r16_float.Base40() == 0xce350000ull &&
              r16_float.Width5() + 1u == 1920 &&
              r16_float.Height5() + 1u == 1080 && r16_float.Depth() + 1u == 1 &&
              r16_float.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k16Float) &&
              r16_float.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              r16_float.DstSelXYZW() == DstSel(4, 0, 0, 1),
          "PPSA02527 R16F 2D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicBgraStorageTextureResource(), r16_float,
                         0x480000);

  const auto r32_float = Ppsa02527R32FloatStorageTextureDescriptor();
  Require("BasicStorageTexture", "PPSA02527 R32F descriptor",
          r32_float.Base40() == 0xcea90000ull &&
              r32_float.Width5() + 1u == 960 &&
              r32_float.Height5() + 1u == 540 && r32_float.Depth() + 1u == 1 &&
              r32_float.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float) &&
              r32_float.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              r32_float.DstSelXYZW() == DstSel(4, 0, 0, 1),
          "PPSA02527 R32F 2D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicBgraStorageTextureResource(), r32_float,
                         0x280000);

  const auto r8_unorm = Ppsa02527R8UnormStorageTextureDescriptor();
  Require("BasicStorageTexture", "PPSA02527 R8 UNORM descriptor",
          r8_unorm.Base40() == 0xc7d50000ull && r8_unorm.Width5() + 1u == 960 &&
              r8_unorm.Height5() + 1u == 540 && r8_unorm.Depth() + 1u == 1 &&
              r8_unorm.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k8UNorm) &&
              r8_unorm.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              r8_unorm.DstSelXYZW() == DstSel(4, 0, 0, 1),
          "PPSA02527 R8 UNORM 2D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicBgraStorageTextureResource(), r8_unorm, 0xc0000);

  const auto yzwx = BasicYzwxStorageTextureDescriptor();
  Require("BasicStorageTexture", "YZWX descriptor",
          yzwx.Base40() == 0x62780100ull && yzwx.Width5() + 1u == 8 &&
              yzwx.Height5() + 1u == 8 && yzwx.Depth() + 1u == 1 &&
              yzwx.Format() == Prospero::GpuEnumValue(
                                   Prospero::BufferFormat::k32_32_32_32Float) &&
              yzwx.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
              yzwx.DstSelXYZW() == DstSel(5, 6, 7, 4),
          "PPSA04181 linear YZWX storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicLinearStorageTextureResource(), yzwx, 0x800);

  const auto array = BasicArrayStorageTextureDescriptor();
  Require("BasicStorageTexture", "2D-array descriptor",
          array.Base40() == 0x2017900000ull && array.Width5() + 1u == 1 &&
              array.Height5() + 1u == 1 && array.Depth() + 1u == 1 &&
              array.BaseArray5() == 0 &&
              array.Type() ==
                  Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
              array.Format() == Prospero::GpuEnumValue(
                                    Prospero::BufferFormat::k8_8_8_8UNorm) &&
              array.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              array.DstSelXYZW() == DstSel(6, 5, 4, 7),
          "PPSA21268 2D-array storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicArrayStorageTextureResource(), array, 0x10000);

  const auto uint_array = BasicUintArrayStorageTextureDescriptor();
  Require("BasicStorageTexture", "uint 2D-array descriptor",
          uint_array.Base40() == 0x2017920000ull &&
              uint_array.Width5() + 1u == 1 && uint_array.Height5() + 1u == 1 &&
              uint_array.Depth() + 1u == 1 && uint_array.BaseArray5() == 0 &&
              uint_array.Type() ==
                  Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
              uint_array.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt) &&
              uint_array.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget) &&
              uint_array.DstSelXYZW() == DstSel(4, 0, 0, 1),
          "PPSA21268 uint 2D-array storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicUintArrayStorageTextureResource(), uint_array,
                         0x10000);

  const auto uint_volume = BasicUintVolumeStorageTextureDescriptor();
  Require("BasicStorageTexture", "uint 3D descriptor",
          uint_volume.Base40() == 0x2018060000ull &&
              uint_volume.Width5() + 1u == 16 &&
              uint_volume.Height5() + 1u == 16 &&
              uint_volume.Depth() + 1u == 16 && uint_volume.BaseArray5() == 0 &&
              uint_volume.Type() ==
                  Prospero::GpuEnumValue(Prospero::ImageType::kColor3D) &&
              uint_volume.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k16UInt) &&
              uint_volume.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kLinear) &&
              uint_volume.DstSelXYZW() == DstSel(4, 0, 0, 0),
          "PPSA21268 uint 3D storage descriptor fixture is malformed");
  ValidateStorageTexture(BasicUintVolumeStorageTextureResource(), uint_volume,
                         0x10000);

  const auto depth_tile = Ppsa14053DepthTileStorageTextureDescriptor();
  Require("BasicStorageTexture", "PPSA14053 depth-tile descriptor",
          depth_tile.Base40() == 0x20144c0000ull &&
              depth_tile.Width5() + 1u == 1 && depth_tile.Height5() + 1u == 1 &&
              depth_tile.Depth() + 1u == 1 && depth_tile.BaseArray5() == 0 &&
              depth_tile.Type() ==
                  Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray) &&
              depth_tile.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt) &&
              depth_tile.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kDepth) &&
              depth_tile.DstSelXYZW() == DstSel(4, 0, 0, 1),
          "PPSA14053 write-only depth-tile storage descriptor fixture is "
          "malformed");
  ValidateStorageTexture(Ppsa14053DepthTileStorageTextureResource(), depth_tile,
                         0x10000);
  Require("BasicStorageTexture", "D32-compatible R32_UINT depth tile",
          IsSupportedStorageDepthTile(
              Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt),
              Prospero::GpuEnumValue(Prospero::ImageType::kColor2D), 64, 64, 1),
          "write-only R32_UINT depth-tile recreation was rejected");
  Require("BasicStorageTexture", "R32_UINT replicated write mapping",
          IsSupportedStorageSwizzle(
              Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt),
              DstSel(4, 4, 4, 4)),
          "single-channel replicated destination selection was rejected");

  char path[MAX_PATH]{};
  Require("BasicStorageTexture", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind :
       {"resource", "type", "tile", "mip", "swizzle", "linear-rgb1-read",
        "bgra-read", "r16-float-read", "r8-unorm-read", "yzwx-read",
        "yzwx-format", "array-base-view", "array-mip-view", "reserved",
        "metadata", "uint-format", "uint-resource-float-format",
        "depth-tile-read", "depth-tile-extent"}) {
    std::string command = std::string("\"") + path +
                          "\" --storage-texture-descriptor-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("BasicStorageTexture", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != 0,
            "CreateProcess failed");
    Require("BasicStorageTexture", "host",
            WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
            "descriptor death case timed out");
    DWORD exit_code = 0;
    const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != 0;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require("BasicStorageTexture", "host", exited && exit_code == 321,
            std::string(kind) +
                " storage descriptor did not report a fatal error");
  }
  std::printf("[host]    %-32s ok\n", "BasicStorageTextureDescriptor");
}

void CheckStorageTextureLinearUploadLayout() {
  constexpr uint32_t format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm);
  constexpr uint32_t width = 3840;
  constexpr uint32_t height = 2160;
  constexpr uint32_t depth = 1;
  constexpr uint32_t tile = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
  const auto pitch = TileGetTexturePitch(format, width, 1, tile);
  TileSizeAlign total{};
  TileGetTextureTotalSize(format, width, height, depth, pitch, 1, tile, false, total);
  const auto layout = TextureCalcUploadLayout(
      format, width, height, 1, depth, pitch, tile, total.size, true, false,
      "StorageTextureLinearTest");
  const auto regions = TextureBuildUploadRegions(
      layout, vk::Format::eR8G8B8A8Unorm, width, height, depth, 1, false, false,
      TextureUploadDestination::MipLevels);
  Require("StorageTextureLinearUpload", "layout",
          pitch == width && total.size == 0x1fa4000 && total.align == 256 &&
              layout.tile == tile && layout.pitch == width &&
              layout.slice_stride == total.size && regions.size() == 1 &&
              regions[0].offset == 0 && regions[0].width == width &&
              regions[0].height == height && regions[0].pitch == width &&
              TextureCalcUploadSize(layout, regions, 1, depth) == total.size,
          "linear RGBA8 storage upload lost Prospero pitch or allocation size");
  std::printf("[host]    %-32s ok\n", "StorageTextureLinearUpload");
}

void CheckStorageTextureDepthTileUploadLayout() {
  constexpr uint32_t format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k8UInt);
  constexpr uint32_t width = 1;
  constexpr uint32_t height = 1;
  constexpr uint32_t depth = 1;
  constexpr uint32_t tile = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  const auto pitch = TileGetTexturePitch(format, width, 1, tile);
  TileSizeAlign slice{};
  TileSizeAlign total{};
  TileSizeOffset level{};
  TilePaddedSize padded{};
  TileGetTextureSize(format, width, height, pitch, 1, tile, &slice, &level,
                     &padded);
  TileGetTextureTotalSize(format, width, height, depth, pitch, 1, tile, false, total);
  const auto layout = TextureCalcUploadLayout(
      format, width, height, 1, depth, pitch, tile, total.size, true, false,
      "StorageTextureDepthTileTest");
  const auto regions = TextureBuildUploadRegions(
      layout, vk::Format::eR8Uint, width, height, depth, 1, true, false,
      TextureUploadDestination::MipLevels);
  Require("StorageTextureDepthTileUpload", "PPSA14053 layout",
          pitch == 256 && padded.width == 256 && padded.height == 256 &&
              slice.size == 0x10000 && slice.align == 0x10000 &&
              level.size == slice.size && level.offset == 0 &&
              total.size == slice.size && total.align == slice.align &&
              layout.tile == tile &&
              layout.tile_family == TileBlockFamily::Depth64KB &&
              layout.pitch == pitch && layout.slice_stride == pitch &&
              layout.source_slice_stride == total.size &&
              layout.level_sizes[0].size == pitch &&
              layout.level_sizes[0].src_size == total.size &&
              regions.size() == 1 && regions[0].offset == 0 &&
              regions[0].width == width && regions[0].height == height &&
              regions[0].pitch == pitch &&
              TextureCalcUploadSize(layout, regions, 1, depth) == total.size,
          "1x1 R8_UINT depth tile lost its 64 KiB source footprint");
  std::printf("[host]    %-32s ok\n", "StorageTextureDepthTileUpload");
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
      MakeColorImageTransferInfo(info, vk::Format::eR8G8B8A8Unorm, 4);
  const auto linear_size =
      ((static_cast<uint64_t>(transfer.height) - 1) * transfer.pitch +
       transfer.width) *
      transfer.bytes_per_element;
  Require("StorageTextureLinearReadback", "layout",
          transfer.address == info.address && transfer.size == info.size &&
              transfer.format == vk::Format::eR8G8B8A8Unorm &&
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

  const auto identity_id = ShaderGetIdCS(regs, identity_info, true);
  const auto rgb1_id = ShaderGetIdCS(regs, rgb1_info, true);
  Require("StorageImageSwizzleSpecializationId", "pipeline cache key",
          identity_id != rgb1_id &&
              identity_id.ids.size() == rgb1_id.ids.size(),
          "storage swizzle-specialized SPIR-V variants share a pipeline ID");
  std::printf("[host]    %-32s ok\n", "StorageImageSwizzlePipelineId");
}

void CheckStorageTextureVolumeUploadLayout() {
  constexpr uint32_t format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  constexpr uint32_t width = 33;
  constexpr uint32_t height = 33;
  constexpr uint32_t depth = 33;
  const auto pitch = TileGetTexturePitch(
      format, width, 1,
      Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget));
  TileSizeAlign total{};
  TileGetTextureTotalSize(
      format, width, height, depth, pitch, 1,
      Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget), true, total);
  const auto layout = TextureCalcUploadLayout(
      format, width, height, 1, depth, pitch,
      Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget), total.size,
      true, true, "StorageTextureVolumeTest");
  const auto regions = TextureBuildUploadRegions(
      layout, vk::Format::eR16G16B16A16Sfloat, width, height, depth, 1, false,
      true, TextureUploadDestination::MipLevels);
  Require("StorageTextureVolumeUpload", "layout",
          pitch == 128 && total.size == 0x210000 &&
              layout.slice_stride == 0x2208 &&
              layout.source_slice_stride == 0 &&
              layout.level_sizes[0].size == 0x2208 &&
              layout.level_sizes[0].src_size == 0 &&
              TextureCalcUploadSize(layout, regions, 1, depth) ==
                  layout.slice_stride * depth,
          "3D render-target upload did not preserve its compact linear layout");

  std::vector<GpuTileInfo> infos;
  Require("StorageTextureVolumeUpload", "GPU records",
          TextureBuildGpuTileInfos(total.size, regions, layout, format, depth,
                                   1, infos) &&
              infos.size() == depth,
          "3D render-target GPU records were not built");
  for (const uint32_t z : {0u, 1u, depth - 1u}) {
    Require("StorageTextureVolumeUpload", "slice offsets",
            infos[z].linear_offset == static_cast<uint64_t>(z) * 0x2208 &&
                infos[z].tiled_offset == static_cast<uint64_t>(z) * 0x10000 &&
                infos[z].surface_z == z && infos[z].pitch == width,
            "volume slice lost its linear stride, block slice, or Z swizzle");
  }
  std::printf("[host]    %-32s ok\n", "StorageTextureVolumeUpload");
}

void CheckStorageTextureVolumeMipRegions() {
  constexpr uint32_t format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm);
  constexpr uint32_t width = 8;
  constexpr uint32_t height = 4;
  constexpr uint32_t depth = 5;
  constexpr uint32_t levels = 3;
  constexpr uint32_t tile = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
  const auto pitch = TileGetTexturePitch(format, width, levels, tile);
  TileSizeAlign total{};
  TileGetTextureTotalSize(format, width, height, depth, pitch, levels, tile,
                          true, total);
  const auto layout = TextureCalcUploadLayout(
      format, width, height, levels, depth, pitch, tile, total.size, true, true,
      "StorageTextureVolumeMipTest");
  const auto uploads = TextureBuildUploadRegions(
      layout, vk::Format::eR8G8B8A8Unorm, width, height, depth, levels, false,
      true, TextureUploadDestination::MipLevels);
  const auto downloads = TextureBuildDownloadRegions(uploads);

  bool valid = uploads.size() == 8 && downloads.size() == uploads.size();
  size_t index = 0;
  for (uint32_t level = 0; level < levels && valid; level++) {
    const uint32_t mip_depth = std::max(depth >> level, 1u);
    const uint32_t mip_width = std::max(width >> level, 1u);
    const uint32_t mip_height = std::max(height >> level, 1u);
    for (uint32_t z = 0; z < mip_depth; z++, index++) {
      const auto &upload = uploads[index];
      const auto &download = downloads[index];
      valid &=
          upload.dst_level == level && upload.dst_z == static_cast<int>(z) &&
          upload.width == mip_width && upload.height == mip_height &&
          upload.offset ==
              layout.level_sizes[level].offset + z * layout.slice_stride &&
          download.src_level == upload.dst_level &&
          download.src_z == upload.dst_z && download.offset == upload.offset &&
          download.pitch == upload.pitch;
    }
  }
  valid &= index == uploads.size();
  Require("StorageTextureVolumeMipRegions", "per-mip depth", valid,
          "3D upload/readback regions did not shrink depth or preserve Vulkan "
          "Z coordinates");
  const auto upload_size =
      TextureCalcUploadSize(layout, uploads, levels, depth);
  if (upload_size != total.size) {
    std::ostringstream out;
    out << "calculated=" << upload_size << " total=" << total.size
        << " stride=" << layout.slice_stride;
    Fail("StorageTextureVolumeMipRegions", "allocation size", out.str());
  }
  std::printf("[host]    %-32s ok\n", "StorageTextureVolumeMipRegions");
}

void CheckColorResolveLayers() {
  RenderColorInfo src{};
  RenderColorInfo dst{};
  src.base_addr = dst.base_addr = 0x12340000;
  src.base_mip_level = dst.base_mip_level = 2;
  src.base_array_layer = 1;
  dst.base_array_layer = 3;
  src.vulkan_buffer = reinterpret_cast<VulkanImage *>(uintptr_t{1});

  Require(
      "ColorResolveLayers", "identity",
      !IsSameColorResolveSubresource(src, dst),
      "different array layers were treated as the same resolve subresource");
  const auto copy = MakeColorResolveCopy(src, dst, 128, 64);
  Require("ColorResolveLayers", "region",
	          &copy.src_image == src.vulkan_buffer && copy.src_level == 2 &&
              copy.dst_level == 2 && copy.src_layer == 1 &&
              copy.dst_layer == 3 && copy.width == 128 && copy.height == 64,
          "color resolve dropped its source or destination array layer");
  dst.base_array_layer = src.base_array_layer;
  Require("ColorResolveLayers", "same subresource",
          IsSameColorResolveSubresource(src, dst),
          "matching color resolve subresources were not recognized");
  std::printf("[host]    %-32s ok\n", "ColorResolveLayers");
}

void CheckStandard64RenderTargetTileRoundTrip() {
  constexpr uint32_t format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  constexpr uint32_t tile =
      Prospero::GpuEnumValue(Prospero::TileMode::kStandard64KB);

  constexpr uint32_t observed_width = 3840;
  constexpr uint32_t observed_height = 2160;
  const auto observed_pitch =
      TileGetTexturePitch(format, observed_width, 1, tile);
  TileSizeAlign observed{};
  TileGetTextureSize(format, observed_width, observed_height, observed_pitch, 1,
                     tile, &observed, nullptr, nullptr);
  Require("Standard64RenderTarget", "observed layout",
          observed_pitch == 3840 && observed.size == 0x1fe0000 &&
              observed.align == 0x10000,
          "PPSA02721 Standard64KB render-target footprint changed");

  constexpr uint32_t width = 257;
  constexpr uint32_t height = 131;
  const auto pitch = TileGetTexturePitch(format, width, 1, tile);
  TileSizeAlign storage{};
  TileGetTextureSize(format, width, height, pitch, 1, tile, &storage, nullptr,
                     nullptr);
  Require("Standard64RenderTarget", "partial layout",
          pitch == 384 && storage.size == 0x60000 && storage.align == 0x10000,
          "partial Standard64KB footprint was not padded in 128x128 blocks");

  RenderTargetInfo info{};
  info.address = 0x10000;
  info.size = storage.size;
  info.format = vk::Format::eR8G8B8A8Unorm;
  info.width = width;
  info.height = height;
  info.pitch = pitch;
  info.bytes_per_element = 4;
  info.tile_mode = tile;
  Require("Standard64RenderTarget", "support boundary",
          IsSupportedStandard64RenderTarget(info) && IsTiledRenderTarget(info),
          "exact Standard64KB render target was not classified as tiled");
  Require("Standard64RenderTarget", "display tile boundary",
          IsSupportedDisplayRenderTargetTileMode(
              Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget)) &&
              !IsSupportedDisplayRenderTargetTileMode(tile),
          "Standard64KB render target could alias a mode-27 display image");
  auto unsupported = info;
  unsupported.address += 4;
  Require("Standard64RenderTarget", "address guard",
          !IsSupportedStandard64RenderTarget(unsupported),
          "unaligned Standard64KB backing was accepted");
  unsupported = info;
  unsupported.bytes_per_element = 8;
  Require("Standard64RenderTarget", "element guard",
          !IsSupportedStandard64RenderTarget(unsupported),
          "unimplemented Standard64KB element size was accepted");
  unsupported = info;
  unsupported.pitch += 128;
  Require("Standard64RenderTarget", "pitch guard",
          !IsSupportedStandard64RenderTarget(unsupported),
          "non-minimal Standard64KB pitch was accepted");
  unsupported = info;
  unsupported.size += 0x10000;
  Require("Standard64RenderTarget", "size guard",
          !IsSupportedStandard64RenderTarget(unsupported),
          "non-exact Standard64KB allocation was accepted");
  unsupported = info;
  unsupported.levels = 2;
  Require("Standard64RenderTarget", "mip guard",
          !IsSupportedStandard64RenderTarget(unsupported),
          "unimplemented Standard64KB mip chain was accepted");
  unsupported = info;
  unsupported.layers = 2;
  Require("Standard64RenderTarget", "layer guard",
          !IsSupportedStandard64RenderTarget(unsupported),
          "unimplemented Standard64KB array was accepted");

  std::printf("[host]    %-32s ok\n", "Standard64RenderTarget");
}

void CheckStorageTextureGpuOwnedRebindState() {
  constexpr uintptr_t base = 0x0000000200200000ull;
  constexpr uint64_t size = 0x10000;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("StorageTextureGpuOwnedRebind", "allocation",
          memory == reinterpret_cast<void *>(base),
          "fixed VirtualAlloc failed");
  PageManager page_manager(CacheFault, nullptr);
  MemoryTracker tracker(page_manager);
  page_manager.OnGpuMap(base, size);
  tracker.ForEachUploadRange(
      base, size, true, [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
  uint64_t readable = 0;
  uint64_t mapped = 0;
  MEMORY_BASIC_INFORMATION protection{};
  Require(
      "StorageTextureGpuOwnedRebind", "owned",
      tracker.IsRegionGpuModified(base, size) &&
          page_manager.IsMapped(base, size) &&
          (!HostMemoryQueryReadable(base, size, readable) ||
           readable < size) &&
          HostMemoryQueryRange(base, size, HostMemoryAccess::Mapped, mapped) &&
          mapped == size &&
          VirtualQuery(memory, &protection, sizeof(protection)) != 0 &&
          protection.Protect == PAGE_NOACCESS,
      "GPU-owned storage pages remained host-readable or lost tracker "
      "identity");

  tracker.UnmarkRegionAsGpuModified(base, size);
  readable = 0;
  Require("StorageTextureGpuOwnedRebind", "clean readback",
          !tracker.IsRegionGpuModified(base, size) &&
              !tracker.IsRegionCpuModified(base, size) &&
              HostMemoryQueryReadable(base, size, readable) &&
              readable == size,
          "clean storage readback did not publish readable coherent backing");
  tracker.MarkRegionAsGpuModified(base, size);
  readable = 0;
  Require(
      "StorageTextureGpuOwnedRebind", "clean reclaim",
      tracker.IsRegionGpuModified(base, size) &&
          !tracker.IsRegionCpuModified(base, size) &&
          (!HostMemoryQueryReadable(base, size, readable) || readable < size),
      "clean storage rebind did not reclaim GPU ownership without an upload");

  tracker.UnmarkRegionAsGpuModified(base, size);
  tracker.MarkRegionAsCpuModified(base, size);
  uint32_t dirty_ranges = 0;
  bool upload_called = false;
  tracker.ForEachUploadRange(
      base, size, true, [&](uint64_t, uint64_t) noexcept { dirty_ranges++; },
      [&]() noexcept { upload_called = true; });
  readable = 0;
  Require(
      "StorageTextureGpuOwnedRebind", "dirty refresh",
      dirty_ranges == 1 && upload_called &&
          tracker.IsRegionGpuModified(base, size) &&
          !tracker.IsRegionCpuModified(base, size) &&
          (!HostMemoryQueryReadable(base, size, readable) || readable < size),
      "CPU-dirty storage rebind did not refresh once and reclaim GPU "
      "ownership");

  tracker.UnmarkRegionAsGpuModified(base, size);
  tracker.UntrackMemory(base, size);
  page_manager.OnGpuUnmap(base, size);
  Require("StorageTextureGpuOwnedRebind", "free",
          VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
  std::printf("[host]    %-32s ok\n", "StorageTextureGpuOwnedRebind");
}

void CheckStorageTextureSampledReuse() {
  const auto image_2d = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  const auto image_2d_array =
      Prospero::GpuEnumValue(Prospero::ImageType::kColor2DArray);
  const auto image_3d = Prospero::GpuEnumValue(Prospero::ImageType::kColor3D);
  Require(
      "StorageTextureSampledReuse", "view shapes",
      SelectStorageSampledViewShape(image_2d, 1, 1) ==
              StorageSampledViewShape::Image2D &&
          SelectStorageSampledViewShape(image_2d_array, 16, 16) ==
              StorageSampledViewShape::Image2DArray &&
          SelectStorageSampledViewShape(image_3d, 16, 1) ==
              StorageSampledViewShape::Image3D &&
          SelectStorageSampledViewShape(image_2d, 16, 1) ==
              StorageSampledViewShape::Unsupported &&
          SelectStorageSampledViewShape(image_2d_array, 16, 1) ==
              StorageSampledViewShape::Unsupported &&
          SelectStorageSampledViewShape(image_3d, 16, 16) ==
              StorageSampledViewShape::Unsupported,
      "sampled storage view shape or backing-layer validation is incorrect");
  ImageInfo storage{};
  storage.address = 0x78650000;
  storage.size = 0x210000;
  storage.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  storage.width = 33;
  storage.height = 33;
  storage.pitch = 128;
  storage.levels = 1;
  storage.tile = Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  storage.swizzle = DstSel(4, 5, 6, 7);
  storage.depth = 33;
  storage.type = Prospero::GpuEnumValue(Prospero::ImageType::kColor3D);

  Require("StorageTextureSampledReuse", "exact",
          ClassifyStorageSampledOverlap(
              storage, storage, vk::Format::eR16G16B16A16Sfloat,
              vk::Format::eR16G16B16A16Sfloat, true,
              false) == StorageSampledOverlap::ExactImage,
          "exact GPU-owned storage image was not reusable for sampling");
  auto incompatible = storage;
  incompatible.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16UNorm);
  Require("StorageTextureSampledReuse", "incompatible",
          ClassifyStorageSampledOverlap(
              incompatible, storage, vk::Format::eR16G16B16A16Unorm,
              vk::Format::eR16G16B16A16Sfloat, true,
              false) == StorageSampledOverlap::Unsupported &&
              ClassifyStorageSampledOverlap(
                  storage, storage, vk::Format::eR16G16B16A16Sfloat,
                  vk::Format::eR16G16B16A16Sfloat, false,
                  false) == StorageSampledOverlap::Unsupported &&
              ClassifyStorageSampledOverlap(
                  storage, storage, vk::Format::eR16G16B16A16Sfloat,
                  vk::Format::eR16G16B16A16Sfloat, true,
                  true) == StorageSampledOverlap::Unsupported,
          "incompatible storage-image state was accepted for sampling");
  auto separate = storage;
  separate.address += 0x400000;
  Require("StorageTextureSampledReuse", "separate",
          ClassifyStorageSampledOverlap(separate, storage,
                                        vk::Format::eR16G16B16A16Sfloat,
                                        vk::Format::eR16G16B16A16Sfloat, true,
                                        false) == StorageSampledOverlap::None,
          "disjoint storage image was classified as an alias");

  ImageInfo mip_chain{};
  mip_chain.address = 0x10eb50000ull;
  mip_chain.size = 0x2b0000;
  mip_chain.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  mip_chain.width = 512;
  mip_chain.height = 512;
  mip_chain.pitch = 512;
  mip_chain.levels = 10;
  mip_chain.view_levels = 10;
  mip_chain.tile = Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  mip_chain.swizzle = DstSel(4, 5, 6, 7);
  mip_chain.type = image_2d;
  auto mip_storage = mip_chain;
  mip_storage.address = mip_chain.address + 0x30000;
  mip_storage.size = 0x80000;
  mip_storage.width = 256;
  mip_storage.height = 256;
  mip_storage.pitch = 256;
  mip_storage.levels = 1;
  mip_storage.view_levels = 1;
  Require("StorageTextureSampledReuse", "captured mip transition",
          IsExactRenderTargetMipStorage(mip_chain, mip_storage,
                                        vk::Format::eR16G16B16A16Sfloat,
                                        vk::Format::eR16G16B16A16Sfloat) &&
              ClassifyStorageSampledOverlap(
                  mip_chain, mip_storage, vk::Format::eR16G16B16A16Sfloat,
                  vk::Format::eR16G16B16A16Sfloat, true, false, true, false,
                  true) == StorageSampledOverlap::RetireStorage,
          "captured GPU-owned level-1 storage allocation was not materialized "
          "before "
          "rebuilding its sampled chain");
  auto tail_storage = mip_chain;
  tail_storage.size = 0x10000;
  tail_storage.width = 128;
  tail_storage.height = 64;
  tail_storage.pitch = 128;
  tail_storage.levels = 1;
  tail_storage.view_levels = 1;
  Require("StorageTextureSampledReuse", "captured mip-tail transition",
          IsExactRenderTargetMipStorage(mip_chain, tail_storage,
                                        vk::Format::eR16G16B16A16Sfloat,
                                        vk::Format::eR16G16B16A16Sfloat) &&
              ClassifyStorageSampledOverlap(
                  mip_chain, tail_storage, vk::Format::eR16G16B16A16Sfloat,
                  vk::Format::eR16G16B16A16Sfloat, true, false, true, false,
                  true) == StorageSampledOverlap::RetireStorage,
          "captured GPU-owned physical mip-tail block was not materialized "
          "before rebuilding its sampled chain");
  auto malformed_mip = mip_storage;
  malformed_mip.address += 0x10000;
  auto malformed_tail = tail_storage;
  malformed_tail.width = 64;
  auto misaligned_chain = mip_chain;
  misaligned_chain.address += 0x1000;
  auto misaligned_tail = tail_storage;
  misaligned_tail.address += 0x1000;
  Require("StorageTextureSampledReuse", "mip transition guards",
          !IsExactRenderTargetMipStorage(mip_chain, malformed_mip,
                                         vk::Format::eR16G16B16A16Sfloat,
                                         vk::Format::eR16G16B16A16Sfloat) &&
              !IsExactRenderTargetMipStorage(mip_chain, malformed_tail,
                                             vk::Format::eR16G16B16A16Sfloat,
                                             vk::Format::eR16G16B16A16Sfloat) &&
              !IsExactRenderTargetMipStorage(misaligned_chain, misaligned_tail,
                                             vk::Format::eR16G16B16A16Sfloat,
                                             vk::Format::eR16G16B16A16Sfloat) &&
              ClassifyStorageSampledOverlap(
                  mip_chain, mip_storage, vk::Format::eR16G16B16A16Sfloat,
                  vk::Format::eR16G16B16A16Sfloat, true, false, true, true,
                  true) == StorageSampledOverlap::Unsupported &&
              ClassifyStorageSampledOverlap(
                  mip_chain, mip_storage, vk::Format::eR16G16B16A16Sfloat,
                  vk::Format::eR16G16B16A16Sfloat, true, false, true, false,
                  false) == StorageSampledOverlap::Unsupported &&
              ClassifyStorageSampledOverlap(
                  mip_chain, mip_storage, vk::Format::eR16G16B16A16Sfloat,
                  vk::Format::eR16G16B16A16Sfloat, true, true, true, false,
                  true) == StorageSampledOverlap::Unsupported,
          "malformed or unsafe mip-storage ownership was accepted");

  ImageInfo ppsa02604_storage{};
  ppsa02604_storage.address = 0x7c690000;
  ppsa02604_storage.size = 0x870000;
  ppsa02604_storage.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8UNorm);
  ppsa02604_storage.width = 1920;
  ppsa02604_storage.height = 1080;
  ppsa02604_storage.pitch = 1920;
  ppsa02604_storage.tile =
      Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  ppsa02604_storage.swizzle = DstSel(6, 5, 4, 7);
  ppsa02604_storage.type =
      Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  auto ppsa02604_sampled = ppsa02604_storage;
  ppsa02604_sampled.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb);
  Require("StorageTextureSampledReuse", "PPSA02604 sRGB view",
          ClassifyStorageSampledOverlap(
              ppsa02604_sampled, ppsa02604_storage, vk::Format::eR8G8B8A8Srgb,
              vk::Format::eR8G8B8A8Unorm, true,
              false) == StorageSampledOverlap::ExactImage,
          "PPSA02604 GPU-owned UNORM storage image did not accept its exact "
          "sRGB view");
  auto alternate_swizzle = ppsa02604_storage;
  alternate_swizzle.swizzle = DstSel(4, 5, 6, 7);
  Require(
      "StorageTextureSampledReuse", "descriptor swizzle view",
      ClassifyStorageSampledOverlap(alternate_swizzle, ppsa02604_storage,
                                    vk::Format::eR8G8B8A8Unorm,
                                    vk::Format::eR8G8B8A8Unorm, true,
                                    false) == StorageSampledOverlap::ExactImage,
      "exact storage backing rejected a distinct sampled descriptor swizzle");
  ImageInfo depth_uint_storage{};
  depth_uint_storage.address = 0x4a5c90000;
  depth_uint_storage.size = 0x10000;
  depth_uint_storage.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32UInt);
  depth_uint_storage.width = 64;
  depth_uint_storage.height = 64;
  depth_uint_storage.pitch = 128;
  depth_uint_storage.levels = 1;
  depth_uint_storage.view_levels = 1;
  depth_uint_storage.depth = 1;
  depth_uint_storage.tile = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  depth_uint_storage.swizzle = DstSel(4, 4, 4, 4);
  depth_uint_storage.type = image_2d;
  auto depth_float_sampled = depth_uint_storage;
  depth_float_sampled.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  depth_float_sampled.swizzle = DstSel(4, 0, 0, 1);
  Require("StorageTextureSampledReuse", "PPSA01530 R32 view",
          ClassifyStorageSampledOverlap(depth_float_sampled, depth_uint_storage,
                                        vk::Format::eR32Sfloat,
                                        vk::Format::eR32Uint, true, false) ==
              StorageSampledOverlap::ExactImage,
          "PPSA01530 GPU-owned R32_UINT storage image did not accept its exact "
          "R32_SFLOAT view");
  const auto usage = TextureFormatUsage::Sampled | TextureFormatUsage::Storage;
  Require(
      "StorageTextureSampledReuse", "usage",
      (TextureGetUsage(usage) &
       (vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage)) ==
              (vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eStorage) &&
          (TextureGetViewUsage(usage) & (vk::ImageUsageFlagBits::eSampled |
                                         vk::ImageUsageFlagBits::eStorage)) ==
              (vk::ImageUsageFlagBits::eSampled |
               vk::ImageUsageFlagBits::eStorage),
      "shared storage image or view is missing sampled/storage usage");
  std::printf("[host]    %-32s ok\n", "StorageTextureSampledReuse");
}

void CheckStorageTextureDepthAlias() {
  ImageInfo storage{};
  storage.address = 0x201ae60000ull;
  storage.size = 0x870000;
  DepthTargetInfo depth{};
  depth.address = 0x2046ca0000ull;
  depth.size = 0x1fe0000;
  depth.stencil_address = storage.address;
  depth.stencil_size = storage.size;
  Require("StorageTextureDepthAlias", "inactive stencil",
          ClassifyStorageDepthOverlap(storage, true, false, false, true,
                                      depth) == DepthOverlap::RetireStorage,
          "inactive stencil storage ownership was not retired");
  depth.stencil_access = true;
  Require("StorageTextureDepthAlias", "active stencil",
          ClassifyStorageDepthOverlap(storage, true, false, false, true,
                                      depth) == DepthOverlap::Unsupported,
          "active stencil storage contents were discarded");
  depth.stencil_access = false;
  storage.address = depth.address;
  depth.depth_access = true;
  Require("StorageTextureDepthAlias", "depth aspect",
          ClassifyStorageDepthOverlap(storage, true, false, false, true,
                                      depth) == DepthOverlap::Unsupported,
          "depth-aspect storage contents were discarded");
  Require("StorageTextureDepthAlias", "clean depth transition",
          ClassifyStorageDepthOverlap(storage, false, false, false, false,
                                      depth) == DepthOverlap::RetireStorage,
          "guest-current storage image was not retired for a depth target");
  Require("StorageTextureDepthAlias", "depth ownership guards",
          ClassifyStorageDepthOverlap(storage, false, true, false, false,
                                      depth) == DepthOverlap::Unsupported &&
              ClassifyStorageDepthOverlap(storage, false, false, true, false,
                                          depth) == DepthOverlap::Unsupported &&
              ClassifyStorageDepthOverlap(storage, false, false, false, true,
                                          depth) == DepthOverlap::Unsupported,
          "incoherent storage-to-depth transition was admitted");
  storage.address = 0x2000000000ull;
  Require("StorageTextureDepthAlias", "disjoint",
          ClassifyStorageDepthOverlap(storage, true, true, true, true, depth) ==
              DepthOverlap::None,
          "disjoint storage image was classified as an alias");
  std::printf("[host]    %-32s ok\n", "StorageTextureDepthAlias");
}

[[noreturn]] void RunStorageTextureAccessDeathCase(const char *kind) {
  GraphicContext context{};
  constexpr uintptr_t base = 0x0000000200300000ull;
  constexpr uint64_t size = 0x10000;
  auto *memory = VirtualAlloc(reinterpret_cast<void *>(base), size,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (memory != reinterpret_cast<void *>(base)) {
    EXIT("storage access child could not reserve its fixed test address\n");
  }
  ResourceMutex resource_mutex;
  PageManager page_manager(CacheFault, nullptr);
  BufferCache buffer_cache(context, page_manager, resource_mutex);
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
  GraphicContext context{};
  constexpr uintptr_t base = 0x0000000200300000ull;
  constexpr uint64_t size = 0x10000;
  auto *memory = VirtualAlloc(reinterpret_cast<void *>(base), size,
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  Require("StorageTextureAccess", "allocation",
          memory == reinterpret_cast<void *>(base),
          "fixed VirtualAlloc failed");
  ResourceMutex resource_mutex;
  PageManager page_manager(CacheFault, nullptr);
  BufferCache buffer_cache(context, page_manager, resource_mutex);
  page_manager.OnGpuMap(base, size, GpuAccess::ReadWrite);
  buffer_cache.ValidateGpuAccess(base, size, true, true);
  page_manager.OnGpuUnmap(base, size, GpuAccess::ReadWrite);
  Require("StorageTextureAccess", "free",
          VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");

  char path[MAX_PATH]{};
  Require("StorageTextureAccess", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind : {"read", "write"}) {
    std::string command =
        std::string("\"") + path + "\" --storage-texture-access-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("StorageTextureAccess", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != 0,
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
  GraphicContext context{};
  constexpr uint64_t address = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x10000;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(address), allocation_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  if (memory != reinterpret_cast<void *>(address)) {
    EXIT("metadata-overlap child could not reserve its fixed test address\n");
  }
  ResourceMutex resource_mutex;
  PageManager page_manager(CacheFault, nullptr);
  BufferCache buffer_cache(context, page_manager, resource_mutex);
  TextureCache texture_cache(context, page_manager, buffer_cache,
                             resource_mutex);
  buffer_cache.SetTextureCache(texture_cache);
  page_manager.OnGpuMap(address, allocation_size);
  texture_cache.RegisterMeta(address, 0x8000);

  if (std::strcmp(kind, "metadata-size") == 0) {
    texture_cache.RegisterMeta(address, 0x10000);
  } else if (std::strcmp(kind, "texture") == 0) {
    ImageInfo info{};
    info.address = address;
    info.size = 0x1000;
    info.width = 1;
    info.height = 1;
    (void)texture_cache.FindTexture(*reinterpret_cast<CommandBuffer *>(1), info,
                                    false);
  } else if (std::strcmp(kind, "render-target") == 0) {
    RenderTargetInfo info{};
    info.address = address;
    info.size = 0x1000;
    info.format = vk::Format::eR8G8B8A8Unorm;
    info.width = 1;
    info.height = 1;
    info.pitch = 1;
    info.bytes_per_element = 4;
    info.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kLinear);
    (void)texture_cache.FindRenderTarget(*reinterpret_cast<CommandBuffer *>(1),
                                         info);
  } else if (std::strcmp(kind, "depth-target") == 0) {
    DepthTargetInfo info{};
    info.address = address;
    info.size = 0x1000;
    info.format = vk::Format::eD16Unorm;
    info.guest_format =
        Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm);
    info.width = 1;
    info.height = 1;
    info.pitch = 1;
    info.bytes_per_element = 2;
    info.tile_mode = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
    (void)texture_cache.FindDepthTarget(*reinterpret_cast<CommandBuffer *>(1),
                                        info);
  } else if (std::strcmp(kind, "copy-stale") == 0) {
    if (!texture_cache.ClearMeta(address)) {
      std::_Exit(0x7f);
    }
    buffer_cache.CopyBuffer(nullptr, address + 0x9000, address, 0x1000);
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
  return {{0x0062b100u, 0xcad00000u, 0x003fc03fu, 0x90500facu, 0x00000000u,
           0x00700000u, 0x00000000u, 0x00000000u}};
}

ShaderTextureResource Ppsa06084MetadataReuseDescriptor() {
  return {{0x00722290u, 0xc0100000u, 0x000fc00fu, 0x90560800u, 0x00000000u,
           0x00700060u, 0x00000000u, 0x00000000u}};
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
  } else if (std::strcmp(kind, "invalid-swizzle") == 0) {
    descriptor = Ppsa06084MetadataReuseDescriptor();
    descriptor.fields[3] =
        (descriptor.fields[3] & ~0xfffu) | DstSel(2, 0, 0, 4);
  } else if (std::strcmp(kind, "mip-view-outside-allocation") == 0) {
    descriptor = Ppsa06084MetadataReuseDescriptor();
    descriptor.fields[3] =
        (descriptor.fields[3] & ~(0xfu << 16u)) | (7u << 16u);
  } else {
    std::_Exit(0x7e);
  }
  ValidateMetadataReuseTexture(resource, descriptor, 0x10000);
  std::_Exit(0x7f);
}

void CheckMetaOverlapDeaths() {
  char path[MAX_PATH]{};
  Require("MetaOverlap", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind : {"metadata-size", "texture", "render-target",
                           "depth-target", "copy-stale"}) {
    std::string command =
        std::string("\"") + path + "\" --meta-overlap-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("MetaOverlap", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != 0,
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
  GraphicContext context{};
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x60000;
  constexpr uint64_t metadata_size = 0x30000;
  constexpr uint64_t second = base + 0x18000;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), allocation_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("OverlappingMetadataViews", "allocation",
          memory == reinterpret_cast<void *>(base),
          "fixed VirtualAlloc failed");

  ResourceMutex resource_mutex;
  CacheFaultContext fault_context;
  PageManager page_manager(CacheFault, &fault_context);
  BufferCache buffer_cache(context, page_manager, resource_mutex);
  TextureCache texture_cache(context, page_manager, buffer_cache,
                             resource_mutex);
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
          !texture_cache.QueryRegion(second, 0x1000).gpu_metadata_bytes,
          "shared metadata page retained GPU ownership after a CPU fault");

  texture_cache.UnmapMemory(base, allocation_size);
  page_manager.OnGpuUnmap(base, allocation_size);
  Require("OverlappingMetadataViews", "free",
          VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
  std::printf("[host]    %-32s ok\n", "OverlappingMetadataViews");
}

void CheckQueryRegionAggregation() {
  GraphicContext context{};
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x10000;
  constexpr uint64_t metadata_size = 0x180;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), allocation_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("QueryRegionAggregation", "allocation",
          memory == reinterpret_cast<void *>(base),
          "fixed VirtualAlloc failed");

  ResourceMutex resource_mutex;
  CacheFaultContext fault_context;
  PageManager page_manager(CacheFault, &fault_context);
  BufferCache buffer_cache(context, page_manager, resource_mutex);
  TextureCache texture_cache(context, page_manager, buffer_cache,
                             resource_mutex);
  fault_context.texture = &texture_cache;
  buffer_cache.SetTextureCache(texture_cache);
  page_manager.OnGpuMap(base, allocation_size);

  const auto empty = texture_cache.QueryRegion(base + 0x20, 0x40);
  Require("QueryRegionAggregation", "empty",
          !empty.image_pages && !empty.image_bytes && !empty.gpu_image_bytes &&
              !empty.non_sampled_pages && !empty.metadata_pages &&
              !empty.metadata_bytes && !empty.gpu_metadata_bytes,
          "empty region reported cached ownership");

  texture_cache.RegisterMeta(base, metadata_size);
  const auto bytes = texture_cache.QueryRegion(base + 0x20, 0x40);
  const auto page_only = texture_cache.QueryRegion(base + 0x400, 0x40);
  const auto disjoint = texture_cache.QueryRegion(base + 0x1000, 0x40);
  Require(
      "QueryRegionAggregation", "page and byte overlap",
      bytes.metadata_pages && bytes.metadata_bytes &&
          !bytes.gpu_metadata_bytes && page_only.metadata_pages &&
          !page_only.metadata_bytes && !page_only.gpu_metadata_bytes &&
          !disjoint.metadata_pages && !disjoint.metadata_bytes &&
          !bytes.image_pages && !bytes.image_bytes && !bytes.gpu_image_bytes &&
          !bytes.non_sampled_pages,
      "metadata page candidates were not separated from exact byte overlap");

  Require("QueryRegionAggregation", "clear", texture_cache.ClearMeta(base),
          "metadata clear setup failed");
  const auto gpu_bytes = texture_cache.QueryRegion(base + 0x20, 0x40);
  const auto gpu_page_only = texture_cache.QueryRegion(base + 0x400, 0x40);
  Require("QueryRegionAggregation", "GPU ownership",
          gpu_bytes.metadata_pages && gpu_bytes.metadata_bytes &&
              gpu_bytes.gpu_metadata_bytes && gpu_page_only.metadata_pages &&
              !gpu_page_only.metadata_bytes &&
              !gpu_page_only.gpu_metadata_bytes,
          "GPU metadata ownership escaped its exact registered byte range");

  texture_cache.UnmapMemory(base, allocation_size);
  page_manager.OnGpuUnmap(base, allocation_size);
  Require("QueryRegionAggregation", "free",
          VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
  std::printf("[host]    %-32s ok\n", "QueryRegionAggregation");
}

void CheckGpuMetadataReuse() {
  GraphicContext context{};
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x20000;
  constexpr uint64_t metadata_size = 0x18000;
  constexpr uint32_t layers = 3;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), allocation_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("GpuMetadataReuse", "allocation",
          memory == reinterpret_cast<void *>(base),
          "fixed VirtualAlloc failed");

  ResourceMutex resource_mutex;
  CacheFaultContext fault_context;
  PageManager page_manager(CacheFault, &fault_context);
  BufferCache buffer_cache(context, page_manager, resource_mutex);
  TextureCache texture_cache(context, page_manager, buffer_cache,
                             resource_mutex);
  fault_context.texture = &texture_cache;
  buffer_cache.SetTextureCache(texture_cache);
  page_manager.OnGpuMap(base, allocation_size);

  texture_cache.RegisterMeta(base, metadata_size, layers);
  TextureCache::MetaRangeInfo full_meta{};
  TextureCache::MetaRangeInfo slice_meta{};
  TextureCache::MetaRangeInfo invalid_meta{};
  constexpr uint64_t slice_size = metadata_size / layers;
  Require("GpuMetadataReuse", "exact metadata ranges",
          texture_cache.ResolveMetaRange(base, metadata_size, full_meta) &&
              full_meta.metadata_address == base &&
              full_meta.metadata_size == metadata_size && full_meta.full &&
              full_meta.slice == 0 &&
              texture_cache.ResolveMetaRange(base + slice_size, slice_size,
                                             slice_meta) &&
              slice_meta.metadata_address == base &&
              slice_meta.metadata_size == metadata_size && !slice_meta.full &&
              slice_meta.slice == 1 &&
              !texture_cache.ResolveMetaRange(base + 0x1000, slice_size,
                                              invalid_meta) &&
              !texture_cache.ResolveMetaRange(base + slice_size, slice_size / 2,
                                              invalid_meta),
          "whole and per-slice metadata ranges were not classified exactly");
  Require("GpuMetadataReuse", "clear", texture_cache.ClearMeta(base),
          "metadata clear setup failed");
  const bool full_image_transition =
      texture_cache.InvalidateMemoryFromGPU(base, allocation_size);
  Require("GpuMetadataReuse", "discard",
          !full_image_transition && !texture_cache.IsMeta(base) &&
              !texture_cache.QueryRegion(base, allocation_size).metadata_bytes,
          "metadata-only overwrite retained identity or claimed an image "
          "transition");
  texture_cache.RegisterMeta(base, metadata_size, layers);
  Require("GpuMetadataReuse", "re-register",
          texture_cache.IsMetaRange(base, metadata_size) &&
              texture_cache.ClearMeta(base),
          "metadata identity could not be reused after a GPU overwrite");
  Require("GpuMetadataReuse", "layered clear",
          texture_cache.IsMetaCleared(base, 0) &&
              texture_cache.IsMetaCleared(base, 1) &&
              texture_cache.IsMetaCleared(base, 2) &&
              texture_cache.TouchMeta(base, 1, false) &&
              texture_cache.IsMetaCleared(base, 0) &&
              !texture_cache.IsMetaCleared(base, 1) &&
              texture_cache.IsMetaCleared(base, 2),
          "consuming one metadata slice erased another slice's lazy clear");
  const bool partial_image_transition =
      texture_cache.InvalidateMemoryFromGPU(base + 0x1000, 0x1000);
  Require("GpuMetadataReuse", "partial discard",
          !partial_image_transition && !texture_cache.IsMeta(base),
          "partial metadata overwrite retained identity or claimed an image "
          "transition");

  texture_cache.UnmapMemory(base, allocation_size);
  page_manager.OnGpuUnmap(base, allocation_size);
  Require("GpuMetadataReuse", "free", VirtualFree(memory, 0, MEM_RELEASE) != 0,
          "VirtualFree failed");
  std::printf("[host]    %-32s ok\n", "GpuMetadataReuse");
}

void CheckMetadataReuseDescriptors() {
  ValidateMetadataReuseTexture(BasicMetadataReuseResource(),
                               BasicMetadataReuseDescriptor(), 0x10000);
  const auto captured = Ppsa06084MetadataReuseDescriptor();
  ValidateMetadataReuseTexture(BasicMetadataReuseResource(), captured, 0x2000);
  Require("MetadataReuseDescriptor", "PPSA06084 sampled mip chain",
          captured.Format() ==
                  Prospero::GpuEnumValue(Prospero::BufferFormat::k8UNorm) &&
              captured.Type() ==
                  Prospero::GpuEnumValue(Prospero::ImageType::kColor2D) &&
              captured.BaseLevel() == 0 && captured.LastLevel() == 6 &&
              captured.MaxMip() == 6 &&
              captured.TileMode() ==
                  Prospero::GpuEnumValue(Prospero::TileMode::kStandard4KB) &&
              captured.DstSelXYZW() == DstSel(0, 0, 0, 4),
          "captured metadata-reuse descriptor fields decoded unexpectedly");
  char path[MAX_PATH]{};
  Require("MetadataReuseDescriptor", "host",
          GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
          "GetModuleFileName failed");
  for (const char *kind :
       {"field1-reserved", "field2-low-reserved", "field2-high-reserved",
        "unsupported-format", "uint-format", "invalid-swizzle",
        "mip-view-outside-allocation"}) {
    std::string command =
        std::string("\"") + path + "\" --metadata-descriptor-death " + kind;
    std::vector<char> mutable_command(command.begin(), command.end());
    mutable_command.push_back('\0');
    STARTUPINFOA startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    Require("MetadataReuseDescriptor", "host",
            CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr,
                           FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                           &process) != 0,
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

void CheckBufferImageWrites() {
  constexpr uint64_t sampled = 0xe80e0000ull;
  constexpr uint64_t sampled_size = 0x870000ull;
  constexpr uint64_t video = 0x1000c10000ull;
  constexpr uint64_t video_size = 0x1fe0000ull;
  constexpr uint64_t target = 0x1158d80000ull;
  constexpr uint64_t target_size = 0x870000ull;
  constexpr uint64_t storage_address = 0x4a4290000ull;
  constexpr uint64_t storage_size = 0x20000ull;
  struct Case {
    const char *name;
    uint64_t buffer_address;
    uint64_t buffer_size;
    uint64_t image_address;
    uint64_t image_size;
    BufferImageBinding binding;
    bool gpu_modified;
    bool formatted;
    BufferImageWrite expected;
  };
  constexpr std::array cases{
      Case{"sampled exact", sampled, sampled_size, sampled, sampled_size,
           BufferImageBinding::Texture, false, false,
           BufferImageWrite::InvalidateTexture},
      Case{"sampled contained prefix", sampled,
           sampled_size - TRACKER_PAGE_SIZE, sampled, sampled_size,
           BufferImageBinding::Texture, false, false,
           BufferImageWrite::InvalidateTexture},
      Case{"sampled contained raw", sampled + 0x23030, 0xfa0, sampled,
           sampled_size, BufferImageBinding::Texture, false, false,
           BufferImageWrite::InvalidateTexture},
      Case{"sampled prefix crossing", sampled - 0x10, 0x20, sampled,
           sampled_size, BufferImageBinding::Texture, false, false,
           BufferImageWrite::Unsupported},
      Case{"sampled suffix crossing", sampled + sampled_size - 0x10, 0x20,
           sampled, sampled_size, BufferImageBinding::Texture, false, false,
           BufferImageWrite::Unsupported},
      Case{"sampled contained GPU-owned", sampled + 0x23030, 0xfa0, sampled,
           sampled_size, BufferImageBinding::Texture, true, false,
           BufferImageWrite::Unsupported},
      Case{"sampled contained unsupported", sampled + 0x23030, 0xfa0, sampled,
           sampled_size, BufferImageBinding::Unsupported, false, false,
           BufferImageWrite::Unsupported},
      Case{"sampled unaligned", sampled + 1, sampled_size, sampled + 1,
           sampled_size, BufferImageBinding::Texture, false, false,
           BufferImageWrite::Unsupported},
      Case{"sampled GPU-owned", sampled, sampled_size, sampled, sampled_size,
           BufferImageBinding::Texture, true, false,
           BufferImageWrite::Unsupported},
      Case{"sampled disjoint", sampled, sampled_size, sampled + sampled_size,
           sampled_size, BufferImageBinding::Texture, false, false,
           BufferImageWrite::None},
      Case{"video exact", video, video_size, video, video_size,
           BufferImageBinding::VideoOut, false, true,
           BufferImageWrite::InvalidateVideoOut},
      Case{"video disjoint", video, video_size, video + video_size, video_size,
           BufferImageBinding::VideoOut, false, true, BufferImageWrite::None},
      Case{"video partial", video, video_size - 4, video, video_size,
           BufferImageBinding::VideoOut, false, true,
           BufferImageWrite::Unsupported},
      Case{"video wrong binding", video, video_size, video, video_size,
           BufferImageBinding::Unsupported, false, true,
           BufferImageWrite::Unsupported},
      Case{"video GPU-owned", video, video_size, video, video_size,
           BufferImageBinding::VideoOut, true, true,
           BufferImageWrite::SynchronizeVideoOut},
      Case{"video unformatted", video, video_size, video, video_size,
           BufferImageBinding::VideoOut, false, false,
           BufferImageWrite::Unsupported},
      Case{"video GPU-owned unformatted", video, video_size, video, video_size,
           BufferImageBinding::VideoOut, true, false,
           BufferImageWrite::Unsupported},
      Case{"video GPU-owned partial", video, video_size - 4, video, video_size,
           BufferImageBinding::VideoOut, true, true,
           BufferImageWrite::Unsupported},
      Case{"video unaligned", video + 0x100, video_size, video + 0x100,
           video_size, BufferImageBinding::VideoOut, true, true,
           BufferImageWrite::Unsupported},
      Case{"target exact", target, target_size, target, target_size,
           BufferImageBinding::RenderTarget, true, true,
           BufferImageWrite::SynchronizeRenderTarget},
      Case{"target partial", target, target_size - 4, target, target_size,
           BufferImageBinding::RenderTarget, true, true,
           BufferImageWrite::Unsupported},
      Case{"target CPU-owned", target, target_size, target, target_size,
           BufferImageBinding::RenderTarget, false, true,
           BufferImageWrite::Unsupported},
      Case{"target unformatted", target, target_size, target, target_size,
           BufferImageBinding::RenderTarget, true, false,
           BufferImageWrite::Unsupported},
      Case{"target unaligned", target + 0x100, target_size, target + 0x100,
           target_size, BufferImageBinding::RenderTarget, true, true,
           BufferImageWrite::Unsupported},
      Case{"storage mip page", storage_address + 0x10000, 0x10000,
           storage_address, storage_size, BufferImageBinding::StorageTexture,
           true, true, BufferImageWrite::SynchronizeStorageTexture},
      Case{"storage unformatted", storage_address + 0x10000, 0x10000,
           storage_address, storage_size, BufferImageBinding::StorageTexture,
           true, false, BufferImageWrite::Unsupported},
      Case{"storage unaligned", storage_address + 0x100, 0x10000,
           storage_address, storage_size, BufferImageBinding::StorageTexture,
           true, true, BufferImageWrite::Unsupported},
      Case{"depth exact", 0x4a5c80000ull, 0x10000, 0x4a5c80000ull, 0x10000,
           BufferImageBinding::DepthTarget, true, true,
           BufferImageWrite::SynchronizeDepthTarget},
      Case{"depth partial", 0x4a5c80000ull, 0x8000, 0x4a5c80000ull, 0x10000,
           BufferImageBinding::DepthTarget, true, true,
           BufferImageWrite::Unsupported},
      Case{"depth unformatted", 0x4a5c80000ull, 0x10000, 0x4a5c80000ull,
           0x10000, BufferImageBinding::DepthTarget, true, false,
           BufferImageWrite::Unsupported},
  };
  for (const auto &test : cases) {
    const auto actual = ClassifyBufferImageWrite(
        test.buffer_address, test.buffer_size, test.image_address,
        test.image_size, test.binding, test.gpu_modified, test.formatted);
    Require("BufferImageWrite", test.name, actual == test.expected,
            "buffer/image write classification mismatch");
  }
  Require("BufferImageWrite", "storage containing overwrite",
          ClassifyBufferImageWrite(
              storage_address - 0x80000, 0x100000, storage_address,
              storage_size, BufferImageBinding::StorageTexture, false, true,
              true) == BufferImageWrite::InvalidateStorageTexture,
          "formatted containing overwrite did not retain refreshable storage "
          "ownership");
  Require("BufferImageWrite", "storage containing overwrite GPU-owned",
          ClassifyBufferImageWrite(
              storage_address - 0x80000, 0x100000, storage_address,
              storage_size, BufferImageBinding::StorageTexture, true, true,
              false) == BufferImageWrite::Unsupported,
          "containing overwrite bypassed GPU-owned storage synchronization");
  Require("BufferImageWrite", "depth repeated buffer overwrite",
          ClassifyBufferImageWrite(0x4a5cd0000ull, 0x10000, 0x4a5cd0000ull,
                                   0x10000, BufferImageBinding::DepthTarget,
                                   false, true, true) ==
              BufferImageWrite::InvalidateDepthTarget,
          "buffer-owned exact depth overwrite was rejected");

  // PPSA01530's packed mip0 page is first synchronized out of a GPU-owned
  // storage image, written through a persistent formatted buffer, and then
  // rebound as the same storage image.
  const auto partial_refresh =
      ClassifyStorageBufferRebind(true, false, true, false, false, true);
  const auto refreshed_reuse =
      ClassifyStorageBufferRebind(true, true, false, true, false, true);
  Require("BufferImageWrite", "storage partial lifecycle",
          partial_refresh == StorageBufferRebind::RefreshFromBacking &&
              refreshed_reuse == StorageBufferRebind::Reuse,
          "storage-to-partial-buffer-to-storage lifecycle was rejected");
  Require("BufferImageWrite", "storage partial incoherent",
          ClassifyStorageBufferRebind(true, false, true, false, false, false) ==
              StorageBufferRebind::Unsupported,
          "incoherent partial storage backing was admitted");
  Require("BufferImageWrite", "storage contradictory ownership",
          ClassifyStorageBufferRebind(true, false, true, true, false, true) ==
              StorageBufferRebind::Unsupported,
          "contradictory storage tracker ownership was admitted");
  Require(
      "BufferImageWrite", "storage mip-one cache miss",
      SelectImageBackingBaseLevel(true, 1) == 0 &&
          SelectImageBackingBaseLevel(false, 1) == 1,
      "storage backing creation remained dependent on first-bound view mip");
  Require("BufferImageWrite", "dynamic one-layer array view",
          !NeedsStaticSampledArrayView(true, true) &&
              NeedsStaticSampledArrayView(true, false) &&
              !NeedsStaticSampledArrayView(false, false),
          "dynamic sampled array view was replaced by a missing static view");

  DepthTargetInfo old_depth{};
  old_depth.address = 0x4a5c90000ull;
  old_depth.size = 0x10000;
  old_depth.width = old_depth.height = 32;
  old_depth.pitch = 32;
  old_depth.bytes_per_element = 4;
  old_depth.layers = 1;
  DepthTargetInfo cleared_depth = old_depth;
  cleared_depth.width = cleared_depth.height = 64;
  cleared_depth.depth_load_clear = true;
  Require(
      "BufferImageWrite", "cleared depth reinterpretation",
      ClassifyDepthTargetOverlap(old_depth, true, false, cleared_depth) ==
          DepthOverlap::DiscardTarget,
      "exact cleared depth allocation could not discard its old native shape");
  cleared_depth.depth_load_clear = false;
  Require("BufferImageWrite", "loaded depth reinterpretation",
          ClassifyDepthTargetOverlap(old_depth, true, false, cleared_depth) ==
              DepthOverlap::Unsupported,
          "loaded depth reinterpretation discarded live native contents");
  RenderTargetInfo old_color{};
  old_color.address = 0x4a5cb0000ull;
  old_color.size = 0x10000;
  old_color.width = old_color.height = old_color.pitch = 128;
  old_color.bytes_per_element = 4;
  old_color.format = vk::Format::eR8G8B8A8Unorm;
  auto reinterpreted_color = old_color;
  reinterpreted_color.format = vk::Format::eR32Uint;
  Require("BufferImageWrite", "clean color format recreation",
          ClassifyRenderTargetOverlap(old_color, false, false, false, true,
                                      reinterpreted_color) ==
                  RenderTargetOverlap::RetireTarget &&
              ClassifyRenderTargetOverlap(old_color, true, false, true, true,
                                          reinterpreted_color) ==
                  RenderTargetOverlap::Unsupported,
          "clean equal-base color allocation did not preserve GPU-owned format "
          "failures");
  auto pool_depth = old_depth;
  pool_depth.stencil_address = old_depth.address + 0x20000;
  pool_depth.stencil_size = old_depth.size;
  auto shifted_depth = pool_depth;
  shifted_depth.address = old_depth.address - 0x20000;
  shifted_depth.stencil_address = old_depth.address;
  Require(
      "BufferImageWrite", "shifted depth-plane recreation",
      ClassifyDepthTargetOverlap(pool_depth, true, false, shifted_depth) ==
              DepthOverlap::RecreateTarget &&
          ClassifyDepthTargetOverlap(pool_depth, true, true, shifted_depth) ==
              DepthOverlap::Unsupported,
      "exact stencil/depth pool reuse did not require clean single-sample "
      "recreation");
  shifted_depth.samples = 8;
  Require(
      "BufferImageWrite", "multisampled depth-plane recreation",
      ClassifyDepthTargetOverlap(pool_depth, true, false, shifted_depth) ==
          DepthOverlap::Unsupported,
      "multisampled depth-plane reuse bypassed the unsupported readback guard");
  RenderTargetInfo pool_color{};
  pool_color.address = shifted_depth.stencil_address;
  pool_color.size = shifted_depth.stencil_size;
  shifted_depth.samples = 1;
  Require(
      "BufferImageWrite", "color/depth pool recreation",
      CanRecreateRenderTargetForDepth(pool_color, false, false, false, true,
                                      shifted_depth) &&
          !CanRecreateRenderTargetForDepth(pool_color, false, true, false, true,
                                           shifted_depth),
      "exact color-plane reuse did not require clean single-sample recreation");
  pool_color.size *= 2;
  Require(
      "BufferImageWrite", "larger color/depth pool recreation",
      CanRecreateRenderTargetForDepth(pool_color, false, false, false, true,
                                      shifted_depth),
      "equal-base page-isolated color allocation was not recreated for depth");
  auto contained_depth = shifted_depth;
  contained_depth.address = pool_color.address + TRACKER_PAGE_SIZE;
  contained_depth.size = TRACKER_PAGE_SIZE;
  contained_depth.stencil_address = 0;
  contained_depth.stencil_size = 0;
  Require(
      "BufferImageWrite", "contained color/depth pool recreation",
      CanRecreateRenderTargetForDepth(pool_color, false, false, false, true,
                                      contained_depth) &&
          !CanRecreateRenderTargetForDepth(pool_color, false, false, false,
                                           false, contained_depth) &&
          !CanRecreateRenderTargetForDepth(pool_color, true, false, false, true,
                                           contained_depth),
      "contained depth allocation bypassed source or tracker ownership guards");
  pool_color.samples = 8;
  Require(
      "BufferImageWrite", "multisampled color/depth recreation",
      !CanRecreateRenderTargetForDepth(pool_color, false, false, false, true,
                                       shifted_depth),
      "multisampled color-plane reuse bypassed the unsupported readback guard");
  auto multisample_depth = old_depth;
  multisample_depth.samples = 8;
  multisample_depth.depth_access = true;
  multisample_depth.stencil_access = true;
  Require("BufferImageWrite", "multisampled depth refresh state",
          !RequiresMultisampleDepthRefresh(multisample_depth, false, false,
                                           false) &&
              RequiresMultisampleDepthRefresh(multisample_depth, false, true,
                                              false) &&
              RequiresMultisampleDepthRefresh(multisample_depth, false, false,
                                              true) &&
              RequiresMultisampleDepthRefresh(multisample_depth, true, false,
                                              false),
          "multisampled refresh used consumed source history instead of "
          "current plane ownership");
  RenderTargetInfo color_alias{};
  color_alias.address = old_depth.address;
  color_alias.size = old_depth.size;
  color_alias.width = color_alias.height = 32;
  color_alias.pitch = 32;
  color_alias.bytes_per_element = 4;
  Require("BufferImageWrite", "guest-backed depth to color",
          CanRecreateDepthForRenderTarget(old_depth, false, false, false, true,
                                          color_alias) &&
              CanRecreateDepthForRenderTarget(old_depth, true, false, true,
                                              false, color_alias) &&
              !CanRecreateDepthForRenderTarget(old_depth, true, true, true,
                                               true, color_alias),
          "depth-to-color recreation did not require coherent guest ownership");
  Require("BufferImageWrite", "guest-current depth to color",
          !CanRecreateDepthForRenderTarget(old_depth, false, false, false,
                                           false, color_alias),
          "stale guest backing authorized depth-to-color recreation");
  auto mismatched_color = color_alias;
  mismatched_color.address += 0x1000;
  Require("BufferImageWrite", "mismatched depth to color",
          !CanRecreateDepthForRenderTarget(old_depth, false, false, false, true,
                                           mismatched_color),
          "mismatched depth shape was discarded as a color allocation");
  color_alias.address = pool_depth.stencil_address;
  Require(
      "BufferImageWrite", "stencil-plane depth to color",
      CanRecreateDepthForRenderTarget(pool_depth, false, false, false, true,
                                      color_alias),
      "exact stencil-plane reuse was not treated as whole-image recreation");
  auto containing_color = color_alias;
  containing_color.address = pool_depth.stencil_address - 0x40000;
  containing_color.size = 0x80000;
  Require("BufferImageWrite", "contained depth allocation to color",
          CanRecreateDepthForRenderTarget(pool_depth, false, false, false, true,
                                          containing_color) &&
              !CanRecreateDepthForRenderTarget(pool_depth, false, false, false,
                                               false, containing_color) &&
              !CanRecreateDepthForRenderTarget(pool_depth, false, false, true,
                                               true, containing_color),
          "contained guest-current depth allocation bypassed ownership guards");

  TileSizeAlign storage{};
  Require("BufferImageWrite", "target layout",
          TileGetRenderTargetPitch(1920, 4) == 1920 &&
              TileGetRenderTargetSize(1920, 1080, 1920, 4, storage) &&
              storage.align == 0x10000 && storage.size == target_size,
          "render-target write fixture has an invalid tiled layout");
  Require("BufferImageWrite", "PPSA02721 video layout",
          TileGetRenderTargetPitch(3840, 4) == 3840 &&
              TileGetRenderTargetSize(3840, 2160, 3840, 4, storage) &&
              storage.align == 0x10000 && storage.size == video_size,
          "4K video-out ownership fixture lost its exact tiled layout");
  std::printf("[host]    %-32s ok\n", "BufferImageWrite");
}

void CheckImageOverlapResolution() {
  CheckBufferImageWrites();
  VideoOutPixelFormatInfo video_format{};
  Require(
      "ImageOverlapResolution", "existing video-out formats",
      DecodeVideoOutPixelFormat(0x8000000022000000ull, video_format) &&
          video_format.format == vk::Format::eR8G8B8A8Srgb &&
          video_format.guest_format ==
              Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb) &&
          DecodeVideoOutPixelFormat(0x8000000000000000ull, video_format) &&
          video_format.format == vk::Format::eB8G8R8A8Srgb &&
          video_format.guest_format ==
              Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb) &&
          DecodeVideoOutPixelFormat(0x8100000022000000ull, video_format) &&
          video_format.format == vk::Format::eA2B10G10R10UnormPack32 &&
          video_format.guest_format ==
              Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm),
      "pre-existing PS5 video-out format mappings changed during decoder "
      "centralization");
  Require(
      "ImageOverlapResolution", "B10G10R10A2 video-out format",
      DecodeVideoOutPixelFormat(0x8100000000000000ull, video_format) &&
          video_format.format == vk::Format::eA2R10G10B10UnormPack32 &&
          video_format.guest_format ==
              Prospero::GpuEnumValue(Prospero::BufferFormat::k10_10_10_2UNorm),
      "PS5 B10G10R10A2 sRGB video-out format did not preserve alternate "
      "channel order");
  Require(
      "ImageOverlapResolution", "RGBA16 float video-out formats",
      DecodeVideoOutPixelFormat(0xc001000622000000ull, video_format) &&
          video_format.format == vk::Format::eR16G16B16A16Sfloat &&
          video_format.guest_format ==
              Prospero::GpuEnumValue(
                  Prospero::BufferFormat::k16_16_16_16Float) &&
          video_format.bytes_per_element == 8 &&
          DecodeVideoOutPixelFormat(0xc001000600000000ull, video_format) &&
          video_format.bgra16,
      "PS5 RGBA16-float video-out formats lost their 64-bpp storage contract");
  VideoOutInfo supported_video_out{};
  supported_video_out.format = video_format.format;
  supported_video_out.guest_format = video_format.guest_format;
  supported_video_out.bytes_per_element = video_format.bytes_per_element;
  supported_video_out.bgra16 = video_format.bgra16;
  Require(
      "ImageOverlapResolution", "video-out format policy",
      IsSupportedVideoOutFormat(supported_video_out),
      "decoded PS5 video-out format was rejected by the centralized policy");
  std::array<uint16_t, 8> bgra16_pixels{1, 2, 3, 4, 5, 6, 7, 8};
  ImageOps::SwapVideoOutBgra16(bgra16_pixels.data(), sizeof(bgra16_pixels));
  Require("ImageOverlapResolution", "BGRA16 channel conversion",
          bgra16_pixels == std::array<uint16_t, 8>{3, 2, 1, 4, 7, 6, 5, 8},
          "BGRA16 video-out conversion did not swap red and blue");
  ImageOps::SwapVideoOutBgra16(bgra16_pixels.data(), sizeof(bgra16_pixels));
  Require("ImageOverlapResolution", "BGRA16 channel round trip",
          bgra16_pixels == std::array<uint16_t, 8>{1, 2, 3, 4, 5, 6, 7, 8},
          "BGRA16 video-out conversion was not reversible for readback");
  Require(
      "ImageOverlapResolution", "video-out byte-size guards", ([&] {
        auto mismatched = supported_video_out;
        mismatched.bytes_per_element = 4;
        return !IsSupportedVideoOutFormat(mismatched);
      })(),
      "video-out format validation admitted a mismatched storage element size");
  Require(
      "ImageOverlapResolution", "video-out format guards",
      !DecodeVideoOutPixelFormat(0x8100070400000000ull, video_format) &&
          !DecodeVideoOutPixelFormat(0xc001070700000000ull, video_format) && !([&] {
            auto mismatched = supported_video_out;
            mismatched.guest_format =
                Prospero::GpuEnumValue(Prospero::BufferFormat::k8_8_8_8Srgb);
            return IsSupportedVideoOutFormat(mismatched);
          })(),
      "unsupported PS5 video-out formats or mismatched guest/host pairs were "
      "admitted");
  TileSizeAlign render_target_layout{};
  Require("ImageOverlapResolution", "R8 render-target pitch",
          TileGetRenderTargetPitch(1920, 1) == 2048,
          "R8 target did not use native 256-pixel block width");
  Require("ImageOverlapResolution", "R8 render-target size",
          TileGetRenderTargetSize(1920, 1080, 2048, 1, render_target_layout) &&
              render_target_layout.size == 0x280000 &&
              render_target_layout.align == 0x10000,
          "R8 target did not use native 64-KiB block geometry");
  Require(
      "ImageOverlapResolution", "RGBA8 render-target size",
      TileGetRenderTargetPitch(1920, 4) == 1920 &&
          TileGetRenderTargetSize(1920, 1080, 1920, 4, render_target_layout) &&
          render_target_layout.size == 0x870000,
      "RGBA8 target layout regressed");
  Require("ImageOverlapResolution", "PPSA19268 render-target edge",
          TileGetRenderTargetPitch(120, 8) == 128 &&
              TileGetRenderTargetSize(120, 67, 128, 8, render_target_layout) &&
              render_target_layout.size == 0x20000 &&
              ImageRangeOverlaps(0x11029fff8ull, 1, 0x110280000ull,
                                 render_target_layout.size),
          "title write did not land in the final native render-target block");
  Require(
      "ImageOverlapResolution", "render-target layout rejection",
      TileGetRenderTargetPitch(1920, 3) == 0 &&
          !TileGetRenderTargetSize(1920, 1080, 1920, 1, render_target_layout),
      "unsupported element size or mismatched pitch was admitted");

  TileSizeOffset mip_levels[16]{};
  TilePaddedSize mip_padded[16]{};
  Require(
      "ImageOverlapResolution", "PPSA09076 mip-chain layout",
      TileGetRenderTargetMipLayout(960, 540, 1024, 8, 10, render_target_layout,
                                   mip_levels, mip_padded) &&
          render_target_layout.size == 0x650000 &&
          render_target_layout.align == 0x10000 &&
          mip_levels[0].offset == 0x1d0000 && mip_levels[0].size == 0x480000 &&
          mip_levels[1].offset == 0x90000 && mip_levels[1].size == 0x140000 &&
          mip_padded[1].width == 512 && mip_padded[1].height == 320,
      "native render-target mip allocation or subresource geometry regressed");
  Require(
      "ImageOverlapResolution", "render-target mip-chain rejection",
      !TileGetRenderTargetMipLayout(960, 540, 960, 8, 10, render_target_layout,
                                    nullptr, nullptr) &&
          !TileGetRenderTargetMipLayout(
              960, 540, 1024, 3, 10, render_target_layout, nullptr, nullptr) &&
          !TileGetRenderTargetMipLayout(
              960, 540, 1024, 8, 0, render_target_layout, nullptr, nullptr) &&
          !TileGetRenderTargetMipLayout(
              960, 540, 1024, 8, 17, render_target_layout, nullptr, nullptr) &&
          !TileGetRenderTargetMipLayout(4, 4, TileGetRenderTargetPitch(4, 4), 4,
                                        4, render_target_layout, nullptr,
                                        nullptr),
      "invalid render-target mip topology was admitted");

  ImageInfo sampled{};
  sampled.address = 0x200000;
  sampled.size = 0x8000;
  ImageInfo sampled_alias = sampled;
  sampled_alias.address += 0x1000;
  Require("ImageOverlapResolution", "sampled alias",
          ClassifySampledOverlap(sampled_alias, sampled, false) ==
              SampledOverlap::ReadOnlyAlias,
          "read-only sampled pool alias was rejected");
  Require("ImageOverlapResolution", "sampled GPU owner",
          ClassifySampledOverlap(sampled_alias, sampled, true) ==
              SampledOverlap::Unsupported,
          "GPU-modified sampled alias was admitted");
  ImageInfo dirty_info{};
  dirty_info.address = 0x500100;
  dirty_info.size = 0x100;
  Image dirty_image;
  dirty_image = dirty_info;
  dirty_image.InvalidateCpuWrite(0x500300, 0x10);
  Require(
      "ImageOverlapResolution", "sampled edge-page maybe dirty",
      dirty_image.IsCpuDirty() && dirty_image.IsMaybeCpuDirty() &&
          !dirty_image.IsDefinitelyCpuDirty(),
      "byte-disjoint write sharing a tracker page was not kept maybe-dirty");
  Require("ImageOverlapResolution", "sampled edge hash required",
          dirty_image.NeedsMaybeCpuHash(),
          "collapsed edge tracking did not request a baseline hash");
  dirty_image.SetMaybeCpuHash(0x1234);
  Require("ImageOverlapResolution", "sampled unchanged edge hash",
          !dirty_image.ResolveMaybeCpuHash(0x1234) &&
              !dirty_image.IsCpuDirty() && dirty_image.IsCpuTrackingComplete(),
          "unchanged edge bytes forced an image upload");
  dirty_image.InvalidateCpuWrite(0x500300, 0x10);
  dirty_image.SetMaybeCpuHash(0x1234);
  Require("ImageOverlapResolution", "sampled changed edge hash",
          dirty_image.ResolveMaybeCpuHash(0x5678) &&
              dirty_image.IsDefinitelyCpuDirty(),
          "changed edge bytes were not promoted to definitely dirty");
  dirty_image.RefreshComplete();
  dirty_image.InvalidateCpuWrite(dirty_info.address, 1);
  Require("ImageOverlapResolution", "sampled byte overlap dirty",
          dirty_image.IsCpuDirty() && dirty_image.IsDefinitelyCpuDirty() &&
              !dirty_image.IsMaybeCpuDirty(),
          "true byte overlap was not promoted to definitely dirty");
  ImageInfo two_page_info{};
  two_page_info.address = 0x600100;
  two_page_info.size = 0x1800;
  Image two_page_image;
  two_page_image = two_page_info;
  two_page_image.InvalidateCpuWrite(0x600000, 1);
  Require("ImageOverlapResolution", "sampled head tracking shrink",
          !two_page_image.IsCpuDirty(),
          "a single disjoint edge fault prematurely dirtied a two-page image");
  two_page_image.InvalidateCpuWrite(0x601a00, 1);
  Require("ImageOverlapResolution", "sampled head-tail collapse",
          two_page_image.IsMaybeCpuDirty() &&
              two_page_image.NeedsMaybeCpuHash(),
          "both disjoint edge faults did not collapse logical tracking");
  constexpr uint64_t storage_subrange = 0x649b0100;
  constexpr uint64_t storage_subrange_size = 0x800;
  constexpr uint64_t sampled_backing = 0x649a3000;
  constexpr uint64_t sampled_backing_size = 0x24000;
  Require("ImageOverlapResolution", "storage retires clean sampled backing",
          ClassifyStorageImageOverlap(storage_subrange, storage_subrange_size,
                                      sampled_backing, sampled_backing_size,
                                      true, false, false, false) ==
              StorageImageOverlap::RetireSampled,
          "clean sampled subrange was not retired before storage creation");
  Require("ImageOverlapResolution", "storage preserves dirty sampled backing",
          ClassifyStorageImageOverlap(storage_subrange, storage_subrange_size,
                                      sampled_backing, sampled_backing_size,
                                      true, true, false,
                                      true) == StorageImageOverlap::Unsupported,
          "GPU-owned sampled subrange was admitted as storage backing");
  ImageInfo page_left = sampled;
  page_left.size = TRACKER_PAGE_SIZE / 2;
  ImageInfo same_page = page_left;
  same_page.address += page_left.size;
  Require(
      "ImageOverlapResolution", "sampled shared page",
      !ImageRangeOverlaps(same_page.address, same_page.size, page_left.address,
                          page_left.size) &&
          ClassifySampledOverlap(same_page, page_left, false) ==
              SampledOverlap::ReadOnlyAlias,
      "byte-disjoint sampled images sharing a tracker page were not aliased");
  ImageInfo separate_page = same_page;
  separate_page.address = page_left.address + TRACKER_PAGE_SIZE;
  Require("ImageOverlapResolution", "sampled separate page",
          ClassifySampledOverlap(separate_page, page_left, false) ==
              SampledOverlap::None,
          "sampled images on separate pages were aliased");
  constexpr uint64_t edge_target_address = 0x108ad00100ull;
  constexpr uint64_t edge_target_size = 0x1fa400;
  constexpr uint64_t edge_fault_address =
      edge_target_address + edge_target_size + 0x1b8;
  Require(
      "ImageOverlapResolution", "GPU target shared edge page",
      !ImageRangeOverlaps(edge_fault_address, 1, edge_target_address,
                          edge_target_size) &&
          ImagePageRangesOverlap(edge_fault_address, 1, edge_target_address,
                                 edge_target_size),
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
                                   texture_size, true, false,
                                   false) == HostWriteOverlap::InvalidateImage,
          "host-backed fill did not invalidate its CPU-current sampled image");
  Require("ImageOverlapResolution", "host fill GPU owner",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, true, true,
                                   false) == HostWriteOverlap::Unsupported,
          "host-backed fill was admitted over a GPU-owned sampled image");
  Require("ImageOverlapResolution", "host fill clean render target",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, true, false,
                                   false) == HostWriteOverlap::InvalidateImage,
          "host-backed fill did not invalidate its clean render target");
  Require("ImageOverlapResolution", "host fill non-refreshable image",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, false, false,
                                   false) == HostWriteOverlap::Unsupported,
          "host-backed fill was admitted over a non-refreshable image");
  constexpr uint64_t dma_address = 0x10f562000ull;
  constexpr uint64_t dma_size = 0x40000;
  constexpr uint64_t target_address = 0x10f570000ull;
  constexpr uint64_t target_size = 0x20000;
  Require(
      "ImageOverlapResolution", "host copy clean render target",
      ClassifyHostWriteOverlap(dma_address, dma_size, target_address,
                               target_size, true, false,
                               false) == HostWriteOverlap::InvalidateImage,
      "host-backed copy did not invalidate the covered clean render target");
  Require("ImageOverlapResolution", "host copy GPU render target",
          ClassifyHostWriteOverlap(dma_address, dma_size, target_address,
                                   target_size, true, true,
                                   false) == HostWriteOverlap::Unsupported,
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
          "shared-page host copy did not conservatively invalidate the clean "
          "target");
  Require("ImageOverlapResolution", "host fill metadata",
          ClassifyHostWriteOverlap(fill_address, fill_size, texture_address,
                                   texture_size, true, false,
                                   true) == HostWriteOverlap::Unsupported,
          "host-backed sampled-image fill was admitted over metadata pages");
  Require("ImageOverlapResolution", "host fill disjoint page",
          ClassifyHostWriteOverlap(texture_address + texture_size, fill_size,
                                   texture_address, texture_size, true, false,
                                   true) == HostWriteOverlap::None,
          "host-backed fill aliased a sampled image on another page");

  constexpr uint64_t video_metadata = 0x1020c10000ull;
  Require("ImageOverlapResolution", "video-out DCC 256/256/0",
          ClassifyVideoOutCompression(true, video_metadata, 0x48u, 0) ==
              VideoOutCompression::Dcc256_256_0,
          "Prospero video-out DCC 256/256/0 mode was rejected");
  Require("ImageOverlapResolution", "video-out DCC 256/64/64",
          ClassifyVideoOutCompression(true, video_metadata, 0x208u, 0) ==
              VideoOutCompression::Dcc256_64_64,
          "Prospero video-out DCC 256/64/64 mode was rejected");
  Require(
      "ImageOverlapResolution", "video-out compression guards",
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
  Require(
      "ImageOverlapResolution", "compressed video-out native ownership",
      CanUseVideoOutNativeWithoutUpload(VideoOutCompression::Dcc256_64_64, true,
                                        false, false) &&
          CanUseVideoOutNativeWithoutUpload(VideoOutCompression::Dcc256_64_64,
                                            false, true, false) &&
          !CanUseVideoOutNativeWithoutUpload(VideoOutCompression::Dcc256_64_64,
                                             false, false, false) &&
          CanUseVideoOutNativeWithoutUpload(VideoOutCompression::Dcc256_256_0,
                                            true, false, false) &&
          !CanUseVideoOutNativeWithoutUpload(VideoOutCompression::Dcc256_256_0,
                                             false, false, true) &&
          !CanUseVideoOutNativeWithoutUpload(VideoOutCompression::Dcc256_64_64,
                                             true, false, true) &&
          !CanUseVideoOutNativeWithoutUpload(VideoOutCompression::Uncompressed,
                                             true, true, false),
      "compressed video-out upload/read ownership boundary regressed");

  vk::ClearColorValue clear{};
  Require(
      "ImageOverlapResolution", "RGBA8 compute clear decode",
      DecodePackedColorClear(vk::Format::eR8G8B8A8Unorm, 0x80402010u, clear) &&
          std::abs(clear.float32[0] - 16.0f / 255.0f) < 0.000001f &&
          std::abs(clear.float32[1] - 32.0f / 255.0f) < 0.000001f &&
          std::abs(clear.float32[2] - 64.0f / 255.0f) < 0.000001f &&
          std::abs(clear.float32[3] - 128.0f / 255.0f) < 0.000001f,
      "packed RGBA8 clear did not preserve component values");
  Require(
      "ImageOverlapResolution", "BGRA8 compute clear decode",
      DecodePackedColorClear(vk::Format::eB8G8R8A8Unorm, 0x80402010u, clear) &&
          std::abs(clear.float32[0] - 64.0f / 255.0f) < 0.000001f &&
          std::abs(clear.float32[1] - 32.0f / 255.0f) < 0.000001f &&
          std::abs(clear.float32[2] - 16.0f / 255.0f) < 0.000001f,
      "packed BGRA8 clear did not account for memory channel order");
  const uint32_t rgb10_clear =
      1023u | (512u << 10u) | (1u << 20u) | (3u << 30u);
  Require("ImageOverlapResolution", "RGB10 compute clear decode",
          DecodePackedColorClear(vk::Format::eA2B10G10R10UnormPack32,
                                 rgb10_clear, clear) &&
              clear.float32[0] == 1.0f &&
              std::abs(clear.float32[1] - 512.0f / 1023.0f) < 0.000001f &&
              std::abs(clear.float32[2] - 1.0f / 1023.0f) < 0.000001f &&
              clear.float32[3] == 1.0f,
          "packed RGB10 clear did not preserve component values");
  Require(
      "ImageOverlapResolution", "sRGB compute clear decode",
      DecodePackedColorClear(vk::Format::eR8G8B8A8Srgb, 0xff808080u, clear) &&
          std::abs(clear.float32[0] - 0.215861f) < 0.00001f &&
          std::abs(clear.float32[1] - 0.215861f) < 0.00001f &&
          std::abs(clear.float32[2] - 0.215861f) < 0.00001f &&
          clear.float32[3] == 1.0f &&
          !DecodePackedColorClear(vk::Format::eR16G16Unorm, 0, clear),
      "sRGB clear decode or unsupported-format guard regressed");
  uint8_t stencil_clear = 0;
  Require("ImageOverlapResolution", "stencil compute clear decode",
          DecodePackedStencilClear(0x7f7f7f7fu, stencil_clear) &&
              stencil_clear == 0x7fu &&
              DecodePackedStencilClear(0, stencil_clear) &&
              stencil_clear == 0 &&
	          !DecodePackedStencilClear(0x0000007fu, stencil_clear),
          "packed stencil clear admitted non-uniform byte values");
  float depth_clear = -1.0f;
  Require(
      "ImageOverlapResolution", "D32 compute clear decode",
      DecodePackedDepthClear(vk::Format::eD32Sfloat, 0u, depth_clear) &&
          depth_clear == 0.0f &&
          DecodePackedDepthClear(vk::Format::eD32SfloatS8Uint, 0x3f800000u,
                                 depth_clear) &&
          depth_clear == 1.0f &&
          !DecodePackedDepthClear(vk::Format::eD16Unorm, 0u, depth_clear) &&
          !DecodePackedDepthClear(vk::Format::eD32Sfloat, 0xbf800000u,
                                  depth_clear) &&
          !DecodePackedDepthClear(vk::Format::eD32Sfloat, 0x40000000u,
                                  depth_clear) &&
          !DecodePackedDepthClear(vk::Format::eD32Sfloat, 0x7f800000u,
	                              depth_clear),
      "D32 compute clear decode admitted an unsupported format or value");
  DepthTargetInfo native_clear_target{};
  native_clear_target.address = 0x1158d80000ull;
  native_clear_target.size = 0x870000ull;
  native_clear_target.format = vk::Format::eD32Sfloat;
  native_clear_target.guest_format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  native_clear_target.bytes_per_element = 4;
  native_clear_target.tile_mode =
      Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  Require("ImageOverlapResolution", "native D32 buffer clear target",
          CanNativeClearDepthFromBuffer(native_clear_target,
                                        native_clear_target.address,
                                        native_clear_target.size),
          "exact uncompressed PS5 D32 target was rejected");
  auto compressed_depth = native_clear_target;
  compressed_depth.htile_address = 0x1161480000ull;
  compressed_depth.htile_size = 0x10000ull;
  auto d16_depth = native_clear_target;
  d16_depth.format = vk::Format::eD16Unorm;
  d16_depth.guest_format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm);
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

  RenderTargetInfo target{};
  target.address = sampled.address;
  target.size = sampled.size;
  Require(
      "ImageOverlapResolution", "render target reuse",
      ClassifyRenderTargetOverlap(sampled, false, target) ==
          RenderTargetOverlap::RetireSampled,
      "exact CPU-owned sampled allocation was not retired for a render target");
  Require("ImageOverlapResolution", "render target GPU owner",
          ClassifyRenderTargetOverlap(sampled, true, target) ==
              RenderTargetOverlap::Unsupported,
          "GPU-owned sampled allocation was retired for a render target");
  ImageInfo partial_target_sample{};
  partial_target_sample.address = 0x79b01000;
  partial_target_sample.size = 0x20000;
  RenderTargetInfo layered_target{};
  layered_target.address = 0x79b10000;
  layered_target.size = 0x60000;
  Require("ImageOverlapResolution", "sampled partial target transition",
          ClassifySampledRenderTargetOverlap(partial_target_sample,
                                             layered_target, false) ==
              RenderTargetOverlap::RetireTarget,
          "sampled partial target alias was not routed through readback");
  Require("ImageOverlapResolution", "sampled partial target rejection",
          ClassifySampledRenderTargetOverlap(partial_target_sample,
                                             layered_target, true) ==
              RenderTargetOverlap::Unsupported,
          "buffer-owned target alias was admitted");

  ImageInfo storage{};
  storage.address = 0x112cd0000ull;
  storage.size = 0xff0000;
  storage.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16_16_16_16Float);
  storage.width = 1920;
  storage.height = 1080;
  storage.pitch = 1920;
  storage.tile = Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  storage.type = Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  RenderTargetInfo storage_target{};
  storage_target.address = storage.address;
  storage_target.size = storage.size;
  storage_target.format = vk::Format::eR16G16B16A16Sfloat;
  storage_target.width = storage.width;
  storage_target.height = storage.height;
  storage_target.pitch = storage.pitch;
  storage_target.bytes_per_element = 8;
  storage_target.tile_mode = storage.tile;
  Require("ImageOverlapResolution", "storage render-target native transition",
          ClassifyStorageRenderTargetOverlap(
              storage, storage_target.format, true, false, false, true,
              storage_target) == RenderTargetOverlap::PreserveStorage,
          "exact GPU-owned RGBA16F storage image was not preserved for a "
          "render target");
  auto partial_storage_target = storage_target;
  partial_storage_target.address += 0x10000;
  partial_storage_target.size = 0x10000;
  Require("ImageOverlapResolution", "clean storage render-target transition",
          ClassifyStorageRenderTargetOverlap(
              storage, storage_target.format, false, false, false, false,
              partial_storage_target) == RenderTargetOverlap::RetireStorage,
          "clean storage allocation was not retired for an overlapping render "
          "target");
  auto mismatched_storage_target = storage_target;
  mismatched_storage_target.width--;
  Require("ImageOverlapResolution", "storage render-target guards",
          ClassifyStorageRenderTargetOverlap(
              storage, storage_target.format, true, true, false, true,
              storage_target) == RenderTargetOverlap::Unsupported &&
              ClassifyStorageRenderTargetOverlap(
                  storage, storage_target.format, true, false, true, true,
                  storage_target) == RenderTargetOverlap::Unsupported &&
              ClassifyStorageRenderTargetOverlap(
                  storage, vk::Format::eR32G32B32A32Sfloat, true, false, false,
                  true, storage_target) == RenderTargetOverlap::Unsupported &&
              ClassifyStorageRenderTargetOverlap(storage, storage_target.format,
                                                 true, false, false, true,
                                                 mismatched_storage_target) ==
                  RenderTargetOverlap::Unsupported &&
              ClassifyStorageRenderTargetOverlap(
                  storage, storage_target.format, false, false, false, true,
                  storage_target) == RenderTargetOverlap::Unsupported &&
              ClassifyStorageRenderTargetOverlap(
                  storage, storage_target.format, true, false, false, false,
                  storage_target) == RenderTargetOverlap::Unsupported,
          "unsupported storage-to-render-target transition was admitted");
  partial_storage_target.address = storage.address + storage.size;
  Require("ImageOverlapResolution", "storage render-target adjacency",
          ClassifyStorageRenderTargetOverlap(
              storage, storage_target.format, false, false, false, false,
              partial_storage_target) == RenderTargetOverlap::None,
          "byte-disjoint storage and render-target allocations were treated as "
          "aliases");
  target.address += 0x2000;
  target.size = 0x2000;
  Require("ImageOverlapResolution", "render target pool reuse",
          ClassifyRenderTargetOverlap(sampled, false, target) ==
              RenderTargetOverlap::RetireSampled,
          "page-contained CPU-owned sampled pool was not retired for a render "
          "target");
  target.address = sampled.address + 0x6000;
  target.size = 0x4000;
  Require(
      "ImageOverlapResolution", "render target chance overlap",
      ClassifyRenderTargetOverlap(sampled, false, target) ==
          RenderTargetOverlap::RetireSampled,
      "page-isolated sampled pool overlap was not retired for a render target");
  target.address++;
  Require("ImageOverlapResolution", "render target partial page",
          ClassifyRenderTargetOverlap(sampled, false, target) ==
              RenderTargetOverlap::RetireSampled,
          "true byte overlap within a partial tracker page was not retired");
  target.address = sampled.address + 0x6101;
  target.size = 0x4000;
  Require("ImageOverlapResolution", "render target unaligned chance overlap",
          ClassifyRenderTargetOverlap(sampled, false, target) ==
              RenderTargetOverlap::RetireSampled,
          "unaligned clean sampled chance overlap was not retired");
  target.address = page_left.address + page_left.size;
  target.size = page_left.size;
  Require("ImageOverlapResolution", "render target shared page",
          ClassifyRenderTargetOverlap(page_left, false, target) ==
              RenderTargetOverlap::None,
          "byte-disjoint allocation sharing a tracker page was treated as an "
          "alias");
  Require("ImageOverlapResolution", "sampled render-target shared page",
          ClassifySampledRenderTargetOverlap(page_left, target, false) ==
              RenderTargetOverlap::None,
          "reverse byte-disjoint tracker-page relationship was treated as an "
          "alias");
  target.address = sampled.address + sampled.size;
  target.size = sampled.size;
  Require(
      "ImageOverlapResolution", "render target adjacent",
      ClassifyRenderTargetOverlap(sampled, false, target) ==
          RenderTargetOverlap::None,
      "adjacent sampled and render-target allocations were treated as aliases");

  RenderTargetInfo old_target{};
  old_target.address = 0x108d80000ull;
  old_target.size = 0x10000;
  old_target.format = vk::Format::eR8G8B8A8Unorm;
  old_target.width = 48;
  old_target.height = 48;
  old_target.pitch = 128;
  old_target.bytes_per_element = 4;
  old_target.tile_mode =
      Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  RenderTargetInfo replacement_target = old_target;
  replacement_target.size = 0x20000;
  replacement_target.format = vk::Format::eR16G16B16A16Sfloat;
  replacement_target.width = 128;
  replacement_target.height = 128;
  replacement_target.bytes_per_element = 8;
  Require("ImageOverlapResolution", "render target allocation-pool replacement",
          ClassifyRenderTargetOverlap(old_target, false, false, false, true,
                                      replacement_target) ==
              RenderTargetOverlap::RetireTarget,
          "clean same-base render-target pool allocation was not retired");
  Require("ImageOverlapResolution", "render target allocation-pool GPU owner",
          ClassifyRenderTargetOverlap(old_target, true, false, true, true,
                                      replacement_target) ==
              RenderTargetOverlap::Unsupported,
          "GPU-owned render-target pool allocation was retired");
  Require("ImageOverlapResolution",
          "render target allocation-pool buffer owner",
          ClassifyRenderTargetOverlap(old_target, false, true, false, true,
                                      replacement_target) ==
              RenderTargetOverlap::Unsupported,
          "buffer-dirty render-target pool allocation was retired");
  auto offset_target = replacement_target;
  offset_target.address += TRACKER_PAGE_SIZE;
  Require("ImageOverlapResolution", "render target allocation-pool offset",
          ClassifyRenderTargetOverlap(old_target, false, false, false, true,
                                      offset_target) ==
              RenderTargetOverlap::RetireTarget,
          "clean offset render-target allocation was not retired");
  auto contained_target = old_target;
  contained_target.address += TRACKER_PAGE_SIZE;
  contained_target.size = TRACKER_PAGE_SIZE;
  Require("ImageOverlapResolution",
          "render target contained allocation-pool reuse",
          ClassifyRenderTargetOverlap(old_target, false, false, false, true,
                                      contained_target) ==
                  RenderTargetOverlap::RetireTarget &&
              ClassifyRenderTargetOverlap(old_target, false, false, false,
                                          false, contained_target) ==
                  RenderTargetOverlap::Unsupported,
          "contained render-target allocation bypassed guest ownership guards");
  auto partial_page_target = replacement_target;
  partial_page_target.address++;
  Require("ImageOverlapResolution",
          "render target allocation-pool partial page",
          ClassifyRenderTargetOverlap(old_target, false, false, false, true,
                                      partial_page_target) ==
              RenderTargetOverlap::Unsupported,
          "partially shared tracker page was treated as allocation-pool "
          "replacement");
  auto same_shape_target = old_target;
  same_shape_target.format = vk::Format::eR8G8B8A8Srgb;
  Require("ImageOverlapResolution", "render target RGBA8 sRGB reinterpretation",
          IsCompatibleRenderTargetView(old_target, same_shape_target),
          "exact RGBA8 UNORM-to-sRGB target was not recognized as a compatible "
          "view");
  Require("ImageOverlapResolution", "render target sRGB no retirement",
          ClassifyRenderTargetOverlap(old_target, false, false, false, true,
                                      same_shape_target) ==
              RenderTargetOverlap::Unsupported,
          "compatible render-target view fell through to target retirement");
  auto incompatible_same_shape_target = same_shape_target;
  incompatible_same_shape_target.format = vk::Format::eR8G8B8A8Uint;
  Require("ImageOverlapResolution", "render target incompatible same shape",
          !IsCompatibleRenderTargetView(old_target,
                                        incompatible_same_shape_target) &&
              ClassifyRenderTargetOverlap(old_target, false, false, false, true,
                                          incompatible_same_shape_target) ==
                  RenderTargetOverlap::RetireTarget,
          "clean incompatible render-target format was not recreated");

  RenderTargetInfo ppsa02604_unorm_target{};
  ppsa02604_unorm_target.address = 0x79c50000ull;
  ppsa02604_unorm_target.size = 0x870000;
  ppsa02604_unorm_target.format = vk::Format::eB8G8R8A8Unorm;
  ppsa02604_unorm_target.width = 1920;
  ppsa02604_unorm_target.height = 1080;
  ppsa02604_unorm_target.pitch = 1920;
  ppsa02604_unorm_target.bytes_per_element = 4;
  ppsa02604_unorm_target.tile_mode =
      Prospero::GpuEnumValue(Prospero::TileMode::kRenderTarget);
  auto ppsa02604_srgb_target = ppsa02604_unorm_target;
  ppsa02604_srgb_target.format = vk::Format::eB8G8R8A8Srgb;
  Require("ImageOverlapResolution", "PPSA02604 BGRA8 sRGB reinterpretation",
          IsCompatibleRenderTargetView(ppsa02604_unorm_target,
                                       ppsa02604_srgb_target),
          "PPSA02604 exact BGRA8 UNORM-to-sRGB target was not recognized as a "
          "view");
  auto adjacent_target = replacement_target;
  adjacent_target.address = old_target.address + old_target.size;
  Require(
      "ImageOverlapResolution", "render target allocation-pool adjacent",
      ClassifyRenderTargetOverlap(old_target, false, false, false, true,
                                  adjacent_target) == RenderTargetOverlap::None,
      "adjacent render targets were treated as allocation-pool replacement");

  Require("ImageOverlapResolution", "metadata retains sampled image",
          ClassifyMetaImageOverlap(true, false, false, false, false) ==
              MetaImageOverlap::RetainSampled,
          "CPU-current sampled metadata alias was rejected");
  Require("ImageOverlapResolution", "metadata retires writable image",
          ClassifyMetaImageOverlap(false, true, false, false, false) ==
              MetaImageOverlap::RetireImage,
          "guest-current writable image was not retired for metadata reuse");
  Require("ImageOverlapResolution", "metadata rejects GPU target",
          ClassifyMetaImageOverlap(false, true, true, false, false) ==
                  MetaImageOverlap::Unsupported &&
              ClassifyMetaImageOverlap(false, true, false, true, false) ==
                  MetaImageOverlap::Unsupported &&
              ClassifyMetaImageOverlap(false, true, false, false, true) ==
                  MetaImageOverlap::Unsupported,
          "unordered or dirty writable image was admitted for metadata "
          "reuse");
  Require("ImageOverlapResolution", "metadata rejects unsupported image",
          ClassifyMetaImageOverlap(false, false, false, false, false) ==
              MetaImageOverlap::Unsupported,
          "unsupported cached image kind was admitted for metadata reuse");
  Require(
      "ImageOverlapResolution", "guest-current depth metadata pool reuse",
      CanRetireGuestCurrentDepthForMetadataReuse(false, false, false, false,
                                                 false, 0),
      "guest-current depth owner was not retired when its HTile allocation was "
      "reused");
  Require("ImageOverlapResolution", "depth metadata pool ownership guards",
          !CanRetireGuestCurrentDepthForMetadataReuse(true, false, true, false,
                                                      false, 0) &&
              !CanRetireGuestCurrentDepthForMetadataReuse(false, true, false,
                                                          false, false, 0) &&
              !CanRetireGuestCurrentDepthForMetadataReuse(false, false, false,
                                                          true, true, 1),
          "GPU-owned, buffer-owned, or cleared HTile metadata was "
          "admitted for pool reuse");

  ImageInfo pooled_sampled{};
  pooled_sampled.address = 0x71141000;
  pooled_sampled.size = 0x40000;
  DepthTargetInfo pooled_depth{};
  pooled_depth.address = 0x71150000;
  pooled_depth.size = 0x10000;
  pooled_depth.stencil_address = 0x71170000;
  pooled_depth.stencil_size = 0x10000;
  Require("ImageOverlapResolution", "guest-current sampled/depth pool alias",
          CanRetireGuestCurrentDepthForSampled(pooled_sampled, pooled_depth,
                                               false, false, false, false),
          "guest-current contained depth target was not retired for a sampled "
          "allocation");
  Require("ImageOverlapResolution", "sampled/depth pool ownership guards",
          !CanRetireGuestCurrentDepthForSampled(pooled_sampled, pooled_depth,
                                                true, false, true, true) &&
              !CanRetireGuestCurrentDepthForSampled(
                  pooled_sampled, pooled_depth, false, true, false, false),
          "GPU-owned or buffer-owned sampled/depth alias was "
          "admitted");
  ImageInfo offset_sampled{};
  offset_sampled.address = 0x6c6cd000;
  offset_sampled.size = 0x10000;
  DepthTargetInfo offset_depth{};
  offset_depth.address = 0x6c6d0000;
  offset_depth.size = 0x10000;
  Require("ImageOverlapResolution", "guest-current sampled to depth pool alias",
          CanRetireGuestCurrentSampledForDepth(
              offset_sampled, offset_depth, false, false, false, false, true),
          "offset guest-current sampled allocation was not retired for depth "
          "reuse");
  Require(
      "ImageOverlapResolution", "sampled to depth pool ownership guards",
      !CanRetireGuestCurrentSampledForDepth(offset_sampled, offset_depth, true,
                                            false, false, true, true) &&
          !CanRetireGuestCurrentSampledForDepth(
              offset_sampled, offset_depth, false, true, false, false, true) &&
          !CanRetireGuestCurrentSampledForDepth(
              offset_sampled, offset_depth, false, false, true, false, true) &&
          !CanRetireGuestCurrentSampledForDepth(
              offset_sampled, offset_depth, false, false, false, false, false),
      "dirty or stale sampled allocation was admitted for "
      "depth reuse");

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
  using MipOwnerIndex = MultiRangePageOwnerIndex<uint32_t>;
  constexpr uint64_t mip_chain_address = 0x110a10000ull;
  constexpr uint64_t mip_chain_size = 0x2b0000;
  constexpr uint64_t mip_zero_address = 0x110ac0000ull;
  constexpr uint64_t mip_zero_size = 0x200000;
  constexpr uint64_t mip_one_address = 0x110a40000ull;
  constexpr uint64_t mip_one_size = 0x80000;
  MipOwnerIndex mip_owners;
  Require("ImageOverlapResolution", "PPSA06084 mip alias registration",
          mip_owners.Register(1, {{mip_chain_address, mip_chain_size}}) &&
              mip_owners.Register(2, {{mip_zero_address, mip_zero_size}}) &&
              ClassifyStorageImageOverlap(mip_one_address, mip_one_size,
                                          mip_chain_address, mip_chain_size,
                                          true, false, false, false) ==
                  StorageImageOverlap::RetireSampled &&
              ClassifyStorageImageOverlap(mip_one_address, mip_one_size,
                                          mip_zero_address, mip_zero_size, true,
                                          false, false,
                                          false) == StorageImageOverlap::None,
          "captured full-chain and disjoint mip owners were misclassified");
  std::vector<MipOwnerIndex::ByteRange> mip_releases;
  Require(
      "ImageOverlapResolution", "PPSA06084 retained mip ownership",
      mip_owners.Unregister(1, mip_releases) && mip_releases.size() == 1 &&
          mip_releases[0].address == mip_chain_address &&
          mip_releases[0].size == mip_zero_address - mip_chain_address &&
          mip_owners.Query(mip_zero_address, mip_zero_size) ==
              std::vector<uint32_t>{2} &&
          mip_owners.Register(3, {{mip_one_address, mip_one_size}}) &&
          mip_owners.Query(mip_one_address, mip_one_size) ==
              std::vector<uint32_t>{3} &&
          mip_owners.Query(mip_zero_address, mip_zero_size) ==
              std::vector<uint32_t>{2},
      "retiring the full chain released or conflated a disjoint cached mip");
  DepthTargetInfo depth{};
  depth.address = sampled.address;
  depth.size = 0x6000;
  depth.depth_load_clear = true;
  Require(
      "ImageOverlapResolution", "clear depth reuse",
      ClassifyDepthOverlap(sampled, false, depth) ==
          DepthOverlap::RetireSampled,
      "equal-base CPU-owned sampled image was not classified as pool reuse");
  Require("ImageOverlapResolution", "gpu owner",
          ClassifyDepthOverlap(sampled, true, depth) ==
              DepthOverlap::Unsupported,
          "GPU-owned sampled image was admitted for retirement");
  depth.depth_load_clear = false;
  Require("ImageOverlapResolution", "incompatible depth load",
          ClassifyDepthOverlap(sampled, false, depth) ==
              DepthOverlap::Unsupported,
          "layout-unknown depth load was admitted for retirement");

  ImageInfo ppsa01880_sampled_depth{};
  ppsa01880_sampled_depth.address = 0x1095200000ull;
  ppsa01880_sampled_depth.size = 0x10000000ull;
  ppsa01880_sampled_depth.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  ppsa01880_sampled_depth.width = 8192;
  ppsa01880_sampled_depth.height = 8192;
  ppsa01880_sampled_depth.pitch = 8192;
  ppsa01880_sampled_depth.tile =
      Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  ppsa01880_sampled_depth.type =
      Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  DepthTargetInfo ppsa01880_depth{};
  ppsa01880_depth.address = ppsa01880_sampled_depth.address;
  ppsa01880_depth.size = ppsa01880_sampled_depth.size;
  ppsa01880_depth.format = vk::Format::eD32Sfloat;
  ppsa01880_depth.guest_format = ppsa01880_sampled_depth.format;
  ppsa01880_depth.width = ppsa01880_sampled_depth.width;
  ppsa01880_depth.height = ppsa01880_sampled_depth.height;
  ppsa01880_depth.pitch = ppsa01880_sampled_depth.pitch;
  ppsa01880_depth.bytes_per_element = 4;
  ppsa01880_depth.tile_mode = ppsa01880_sampled_depth.tile;
  Require("ImageOverlapResolution", "PPSA01880 exact depth load",
          ClassifyDepthOverlap(ppsa01880_sampled_depth, false,
                               ppsa01880_depth) == DepthOverlap::RetireSampled,
          "captured CPU-owned R32F/D32F depth layout was not retired");

  ImageInfo ppsa09477_sampled_depth{};
  ppsa09477_sampled_depth.address = 0x10adc00000ull;
  ppsa09477_sampled_depth.size = 0x870000;
  ppsa09477_sampled_depth.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  ppsa09477_sampled_depth.width = 1920;
  ppsa09477_sampled_depth.height = 1080;
  ppsa09477_sampled_depth.pitch = 2048;
  ppsa09477_sampled_depth.tile =
      Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
  ppsa09477_sampled_depth.type =
      Prospero::GpuEnumValue(Prospero::ImageType::kColor2D);
  DepthTargetInfo ppsa09477_depth{};
  ppsa09477_depth.address = ppsa09477_sampled_depth.address;
  ppsa09477_depth.size = ppsa09477_sampled_depth.size;
  ppsa09477_depth.stencil_address = 0x10aec00000ull;
  ppsa09477_depth.stencil_size = 0x220000;
  ppsa09477_depth.format = vk::Format::eD32SfloatS8Uint;
  ppsa09477_depth.guest_format = ppsa09477_sampled_depth.format;
  ppsa09477_depth.width = ppsa09477_sampled_depth.width;
  ppsa09477_depth.height = ppsa09477_sampled_depth.height;
  ppsa09477_depth.pitch = ppsa09477_sampled_depth.pitch;
  ppsa09477_depth.bytes_per_element = 4;
  ppsa09477_depth.tile_mode = ppsa09477_sampled_depth.tile;
  ppsa09477_depth.stencil_access = true;
  Require("ImageOverlapResolution", "PPSA09477 disjoint stencil depth load",
          ClassifyDepthOverlap(ppsa09477_sampled_depth, false,
                               ppsa09477_depth) == DepthOverlap::RetireSampled,
          "captured sampled depth was not preserved beside its disjoint "
          "stencil plane");
  auto ppsa09477_overlapping_stencil = ppsa09477_depth;
  ppsa09477_overlapping_stencil.stencil_address =
      ppsa09477_sampled_depth.address + 0x800000;
  Require("ImageOverlapResolution", "PPSA09477 overlapping stencil guard",
          ClassifyDepthOverlap(ppsa09477_sampled_depth, false,
                               ppsa09477_overlapping_stencil) ==
              DepthOverlap::Unsupported,
          "sampled depth was retired while also aliasing the stencil plane");
  auto promoted_d16_sampled = ppsa09477_sampled_depth;
  promoted_d16_sampled.format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm);
  auto promoted_d16_depth = ppsa09477_depth;
  promoted_d16_depth.guest_format = promoted_d16_sampled.format;
  promoted_d16_depth.format = vk::Format::eD24UnormS8Uint;
  promoted_d16_depth.bytes_per_element = 2;
  Require(
      "ImageOverlapResolution", "promoted D16 native-copy guard",
      ClassifyDepthOverlap(promoted_d16_sampled, false, promoted_d16_depth) ==
          DepthOverlap::Unsupported,
      "sampled D16 depth was admitted without a native transfer-width match");
  Require("ImageOverlapResolution", "PPSA01880 GPU depth load",
          ClassifyDepthOverlap(ppsa01880_sampled_depth, true,
                               ppsa01880_depth) == DepthOverlap::Unsupported,
          "captured GPU-owned sampled depth was admitted for retirement");
  auto mismatched_sampled_depth = ppsa01880_sampled_depth;
  mismatched_sampled_depth.pitch++;
  auto mismatched_depth = ppsa01880_depth;
  mismatched_depth.stencil_address =
      ppsa01880_depth.address + ppsa01880_depth.size;
  mismatched_depth.stencil_size = 0x4000000;
  Require(
      "ImageOverlapResolution", "PPSA01880 depth layout guards",
      ClassifyDepthOverlap(mismatched_sampled_depth, false, ppsa01880_depth) ==
              DepthOverlap::Unsupported &&
          ClassifyDepthOverlap(ppsa01880_sampled_depth, false,
                               mismatched_depth) == DepthOverlap::Unsupported,
      "pitch-mismatched or stencil-bearing depth load was admitted");
  mismatched_sampled_depth = ppsa01880_sampled_depth;
  mismatched_sampled_depth.levels = 2;
  mismatched_sampled_depth.view_levels = 2;
  mismatched_depth = ppsa01880_depth;
  mismatched_depth.format = vk::Format::eD16Unorm;
  Require(
      "ImageOverlapResolution", "PPSA01880 depth topology guards",
      ClassifyDepthOverlap(mismatched_sampled_depth, false, ppsa01880_depth) ==
              DepthOverlap::Unsupported &&
          ClassifyDepthOverlap(ppsa01880_sampled_depth, false,
                               mismatched_depth) == DepthOverlap::Unsupported,
      "mipmapped or format-mismatched depth load was admitted");
  struct DepthTransitionCase {
    const char *name;
    bool clear;
    bool native;
    bool sampled_cpu_dirty;
    bool sampled_buffer_modified;
    bool buffer_overlap;
    bool buffer_cpu_dirty;
    DepthTransitionSource expected;
  };
  constexpr DepthTransitionCase transition_cases[] = {
      {"clear", true, true, false, false, false, true,
       DepthTransitionSource::None},
      {"clean native", false, true, false, false, false, true,
       DepthTransitionSource::Native},
      {"missing native", false, false, false, false, false, false,
       DepthTransitionSource::Guest},
      {"CPU-dirty native", false, true, true, false, false, false,
       DepthTransitionSource::Guest},
      {"buffer-modified native", false, true, false, true, false, false,
       DepthTransitionSource::Guest},
      {"dirty overlapping buffer", false, true, false, false, true, true,
       DepthTransitionSource::Guest},
  };
  for (const auto &test : transition_cases) {
    Require("ImageOverlapResolution", test.name,
            SelectDepthTransitionSource(
                test.clear, test.native, test.sampled_cpu_dirty,
                test.sampled_buffer_modified, test.buffer_overlap,
                test.buffer_cpu_dirty) == test.expected,
            "depth preservation source changed");
  }
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
  depth.stencil_load_clear = false;
  depth.stencil_htile_compressed = false;
  Require("ImageOverlapResolution", "raw stencil load",
          CanLoadRawStencilPlane(depth),
          "uncompressed guest stencil plane was rejected");
  depth.stencil_htile_compressed = true;
  Require("ImageOverlapResolution", "compressed stencil load",
          !CanLoadRawStencilPlane(depth),
          "HTile-compressed guest stencil plane was silently admitted");
  depth.stencil_htile_compressed = false;
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
  Require(
      "ImageOverlapResolution", "offset depth view",
      !IsDepthTargetRangeCompatible(depth, depth.address + 1, depth.size - 1),
      "offset depth range was silently admitted");
  Require("ImageOverlapResolution", "complete stencil view",
          IsDepthTargetRangeCompatible(depth, depth.stencil_address,
                                       depth.stencil_size),
          "complete stencil range was rejected");
  Require("ImageOverlapResolution", "partial stencil view",
          !IsDepthTargetRangeCompatible(depth, depth.stencil_address,
                                        depth.stencil_size - 1),
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

void CheckNativeMsaaState() {
  Require("NativeMsaaState", "sample encoding",
          render_sample_count(0) == 1 && render_sample_count(1) == 2 &&
              render_sample_count(2) == 4 && render_sample_count(3) == 8 &&
              render_sample_count(4) == 0,
          "PS5 sample encodings were not mapped exactly");
  Require("NativeMsaaState", "Vulkan sample mapping",
          vulkan_sample_count(1) == vk::SampleCountFlagBits::e1 &&
              vulkan_sample_count(2) == vk::SampleCountFlagBits::e2 &&
              vulkan_sample_count(4) == vk::SampleCountFlagBits::e4 &&
              vulkan_sample_count(8) == vk::SampleCountFlagBits::e8 &&
              vulkan_sample_count(3) == vk::SampleCountFlagBits{},
          "native sample counts were not mapped exactly to Vulkan");

  TileSizeAlign color{};
  const auto color_pitch = TileGetRenderTargetPitch(1920, 8, 3);
  Require("NativeMsaaState", "8x color footprint",
          color_pitch == 1920 &&
              TileGetRenderTargetSize(1920, 1080, color_pitch, 8, color, 3) &&
              color.align == 0x10000 && color.size == 0x07f80000,
          "8x R16G16B16A16 color footprint was not preserved");

  TileSizeAlign depth{};
  TileSizeAlign stencil{};
  TileSizeAlign htile{};
  Require("NativeMsaaState", "8x depth/stencil footprint",
          TileGetDepthPitch(1920, 4, 3) == 1920 &&
              TileGetDepthSize(
                  1920, 1080, 0,
                  Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                  Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true, stencil, htile, depth, 3) &&
              depth.align == 0x10000 && depth.size == 0x03fc0000 &&
              stencil.align == 0x10000 && stencil.size == 0x010e0000 &&
              htile.align == 0x8000 && htile.size == 0x00030000,
          "8x depth/stencil or fragment-independent HTile footprint regressed");
  std::printf("[host]    %-32s ok\n", "NativeMsaaState");
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
  PipelineStencilStaticState state{vk::StencilOp::eKeep, vk::StencilOp::eKeep,
                                   vk::StencilOp::eKeep,
                                   vk::CompareOp::eAlways};
  PipelineStencilDynamicState dynamic{0xff, 0xff, 0};
  Require("StencilAttachmentAccess", "always keep",
          !stencil_face_accesses_attachment(state, dynamic),
          "ALWAYS/KEEP state was classified as stencil access");
  state.compareOp = vk::CompareOp::eEqual;
  Require("StencilAttachmentAccess", "compare reads",
          stencil_face_accesses_attachment(state, dynamic),
          "real stencil comparison was classified as no access");
  state.compareOp = vk::CompareOp::eAlways;
  state.passOp = vk::StencilOp::eZero;
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
  struct AttachmentFormatCase {
    const char *name;
    uint32_t depth_format;
    uint32_t stencil_format;
    vk::Format expected;
  };
  constexpr AttachmentFormatCase attachment_cases[] = {
      {"Z16", Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16),
       Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid),
       vk::Format::eD16Unorm},
      {"Z16S8", Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16),
       Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
       vk::Format::eD16UnormS8Uint},
      {"Z32", Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
       Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid),
       vk::Format::eD32Sfloat},
      {"Z32S8", Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
       Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
       vk::Format::eD32SfloatS8Uint},
      {"invalid depth", 2,
       Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
       vk::Format::eUndefined},
      {"invalid stencil", Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16),
       2, vk::Format::eUndefined},
  };
  for (const auto &test : attachment_cases) {
    Require("DepthTargetFootprints", test.name,
            DepthAttachmentFormat(test.depth_format, test.stencil_format) ==
                test.expected,
            "PS5 depth/stencil attachment mapping changed");
  }
  Require(
      "DepthTargetFootprints", "1920x1080 Z16S8 without HTile",
      TileGetDepthSize(1920, 1080, 0,
                       Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16),
                       Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
                       false, stencil, htile, depth) &&
          depth.size == 0x480000 && depth.align == 0x10000 &&
          stencil.size == 0x280000 && stencil.align == 0x10000 &&
          htile.size == 0,
      "captured Z16S8 footprint disagrees with Prospero block rules");
  struct TargetFormatCase {
    const char *name;
    vk::Format host_format;
    uint32_t guest_format;
    uint32_t bytes_per_element;
    bool stencil;
    bool supported;
    bool readback;
  };
  constexpr TargetFormatCase target_cases[] = {
      {"D16", vk::Format::eD16Unorm,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 2, false, true,
       true},
      {"D16S8", vk::Format::eD16UnormS8Uint,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 2, true, true,
       true},
      {"D16 via D24S8", vk::Format::eD24UnormS8Uint,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 2, true, true,
       false},
      {"D16 via D32S8", vk::Format::eD32SfloatS8Uint,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 2, true, true,
       false},
      {"D32", vk::Format::eD32Sfloat,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 4, false, true,
       true},
      {"D32S8", vk::Format::eD32SfloatS8Uint,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 4, true, true,
       true},
      {"D16 plus stencil mismatch", vk::Format::eD16Unorm,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 2, true, false,
       false},
      {"fallback without stencil", vk::Format::eD24UnormS8Uint,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 2, false,
       false, false},
      {"D32 guest via D24", vk::Format::eD24UnormS8Uint,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 4, true, false,
       false},
      {"D16 byte mismatch", vk::Format::eD16Unorm,
       Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm), 4, false,
       false, false},
  };
  for (const auto &test : target_cases) {
    DepthTargetInfo target{};
    target.format = test.host_format;
    target.guest_format = test.guest_format;
    target.bytes_per_element = test.bytes_per_element;
    target.stencil_address = test.stencil ? 0x10000 : 0;
    target.stencil_size = test.stencil ? 0x10000 : 0;
    Require("DepthTargetFootprints", test.name,
            IsSupportedDepthTargetFormat(target) == test.supported &&
                IsSupportedDepthReadbackFormat(target) == test.readback,
            "host/guest depth format or exact readback policy changed");
  }
  DepthTargetInfo compressed_stencil{};
  compressed_stencil.format = vk::Format::eD32SfloatS8Uint;
  compressed_stencil.guest_format =
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
  compressed_stencil.bytes_per_element = 4;
  compressed_stencil.stencil_address = 0x10000;
  compressed_stencil.stencil_size = 0x10000;
  compressed_stencil.stencil_htile_compressed = true;
  Require(
      "DepthTargetFootprints", "compressed stencil readback hard guard",
      IsSupportedDepthTargetFormat(compressed_stencil) &&
          !IsSupportedDepthReadbackFormat(compressed_stencil),
      "compressed stencil unexpectedly entered the raw-plane readback path");
  struct TransferPlaneCase {
    const char *name;
    vk::Format attachment;
    vk::Format transfer;
    uint32_t bytes;
  };
  constexpr TransferPlaneCase transfer_cases[] = {
      {"D16S8 transfer", vk::Format::eD16UnormS8Uint, vk::Format::eD16Unorm, 2},
      {"D24S8 transfer", vk::Format::eD24UnormS8Uint,
       vk::Format::eX8D24UnormPack32, 4},
      {"D32S8 transfer", vk::Format::eD32SfloatS8Uint, vk::Format::eD32Sfloat,
       4},
      {"invalid transfer", vk::Format::eR16Unorm, vk::Format::eUndefined, 0},
  };
  for (const auto &test : transfer_cases) {
    Require("DepthTargetFootprints", test.name,
            DepthAspectTransferFormat(test.attachment) == test.transfer &&
                DepthAspectTransferBytes(test.attachment) == test.bytes,
            "combined depth transfer-plane layout changed");
  }
  struct PromotionCase {
    const char *name;
    uint16_t source;
    uint32_t d24;
    uint32_t d32;
  };
  constexpr PromotionCase promotion_cases[] = {
      {"zero", 0, 0, 0},
      {"midpoint", 0x8000, 0x00800080, 0x3f000080},
      {"maximum", 0xffff, 0x00ffffff, 0x3f800000},
  };
  for (const auto &test : promotion_cases) {
    Require("DepthTargetFootprints", test.name,
            EncodeD16AsD24(test.source) == test.d24 &&
                EncodeD16AsD32(test.source) == test.d32,
            "D16 host promotion changed the represented depth value");
  }
  Require("DepthTargetFootprints", "640x360 Z32S8 without HTile",
          TileGetDepthSize(
              640, 360, 0, Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
              Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), false, stencil, htile, depth),
          "valid non-HTile depth/stencil footprint was rejected");
  Require(
      "DepthTargetFootprints", "640x360 Prospero block sizes",
      depth.size == 0xf0000 && depth.align == 0x10000 &&
          stencil.size == 0x60000 && stencil.align == 0x10000 &&
          htile.size == 0 && htile.align == 0,
      "non-HTile depth/stencil footprint disagrees with Prospero block rules");

  const auto depth_pitch = TileGetTexturePitch(
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 640, 1,
      Prospero::GpuEnumValue(Prospero::TileMode::kDepth));
  TileSizeAlign texture_depth{};
  TileGetTextureTotalSize(
      Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float), 640, 360, 1,
      depth_pitch, 1, Prospero::GpuEnumValue(Prospero::TileMode::kDepth), false, texture_depth);
  Require("DepthTargetFootprints", "640x360 generic depth tile",
          depth_pitch == 640 && texture_depth.size == 0xf0000 &&
              texture_depth.align == 0x10000,
          "generic depth texture sizing bypassed 64 KiB block padding");

  Require(
      "DepthTargetFootprints", "960x540 Z32S8 with HTile",
      TileGetDepthSize(960, 540, 0,
                       Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                       Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
                       true, stencil, htile, depth) &&
          depth.size == 0x280000 && depth.align == 0x10000 &&
          stencil.size == 0xc0000 && stencil.align == 0x10000 &&
          htile.size == 0x10000 && htile.align == 0x8000,
      "generic Prospero HTile block calculation rejected the title footprint");
  Require(
      "DepthTargetFootprints", "known HTile extent",
      TileGetDepthSize(1280, 720, 0,
                       Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                       Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
                       true, stencil, htile, depth) &&
          depth.size == 0x3c0000 && stencil.size == 0xf0000 &&
          htile.size == 0x20000,
      "validated 1280x720 HTile footprint regressed");
  Require(
      "DepthTargetFootprints", "PPSA06228 3840x2160 Z32S8 with HTile",
      TileGetDepthSize(3840, 2160, 0,
                       Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                       Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
                       true, stencil, htile, depth) &&
          depth.size == 0x1fe0000 && depth.align == 0x10000 &&
          stencil.size == 0x870000 && stencil.align == 0x10000 &&
          htile.size == 0xa0000 && htile.align == 0x8000,
      "captured 3840x2160 depth/stencil/HTile footprint disagrees with "
      "Prospero rules");
  Require(
      "DepthTargetFootprints", "invalid depth format",
      !TileGetDepthSize(960, 540, 0, 2,
                        Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt),
                        true, stencil, htile, depth),
      "unsupported depth format was silently admitted");
  Require("DepthTargetFootprints", "invalid stencil format",
          !TileGetDepthSize(
              960, 540, 0, Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
              2, true, stencil, htile, depth),
          "unsupported stencil format was silently admitted");
  Require("DepthTargetFootprints", "invalid extent",
          !TileGetDepthSize(
              0, 540, 0, Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
              Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true, stencil, htile, depth) &&
              !TileGetDepthSize(
                  16385, 540, 0,
                  Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F),
                  Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt), true, stencil, htile, depth),
          "invalid HTile extent was silently admitted");
  std::printf("[host]    %-32s ok\n", "DepthTargetFootprints");
}

void CheckHtileClearTargetResolution() {
  HW::DepthDepthSizeXY one_pixel{};
  one_pixel.valid = true;
  Require("HtileClearTargetResolution", "1x1 encoded depth extent",
          one_pixel.valid && one_pixel.x_max + 1u == 1u &&
              one_pixel.y_max + 1u == 1u,
          "zero DB_DEPTH_SIZE_XY fields were mistaken for an absent register");
  HW::DepthDepthSizeXY absent{};
  Require("HtileClearTargetResolution", "absent depth extent", !absent.valid,
          "an unwritten DB_DEPTH_SIZE_XY register was admitted");
  HW::DepthRenderTarget descriptor_backed{};
  descriptor_backed.z_info.format =
      Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16);
  descriptor_backed.z_info.tile_surface_enable = true;
  descriptor_backed.z_info.zrange_precision = 1;
  descriptor_backed.stencil_info.tile_stencil_disable = true;
  descriptor_backed.z_read_base_addr = 0xc1a80000;
  descriptor_backed.z_write_base_addr = descriptor_backed.z_read_base_addr;
  descriptor_backed.htile_data_base_addr = 0xc1a98000;

  HtileClearTarget resolved{};
  Require("HtileClearTargetResolution",
          "PPSA09076 descriptor-backed Z16 target",
	          ResolveHtileClearTarget(descriptor_backed, 0x8000, resolved) &&
              resolved.address == 0xc1a98000 && resolved.size == 0x8000,
          "valid descriptor-proven Z16 HTile clear target was rejected");

  auto partial_extent = descriptor_backed;
  partial_extent.size.x_max = 1;
  auto missing_stencil = descriptor_backed;
  missing_stencil.stencil_info.format =
      Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt);
  auto mismatched_write = descriptor_backed;
  mismatched_write.z_write_base_addr += 0x10000;
  Require(
      "HtileClearTargetResolution", "descriptor-backed rejection boundaries",
	      !ResolveHtileClearTarget(descriptor_backed, 0x4000, resolved) &&
	          !ResolveHtileClearTarget(partial_extent, 0x8000, resolved) &&
	          !ResolveHtileClearTarget(missing_stencil, 0x8000, resolved) &&
	          !ResolveHtileClearTarget(mismatched_write, 0x8000, resolved),
      "ambiguous or inconsistent descriptor-backed HTile target was admitted");

  HW::DepthRenderTarget derived{};
  derived.z_info.format = Prospero::GpuEnumValue(Prospero::DepthFormat::kZ32F);
  derived.z_info.tile_surface_enable = true;
  derived.z_info.zrange_precision = 1;
  derived.stencil_info.format =
      Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt);
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
  Require(
      "HtileClearTargetResolution", "derived Z32S8 target",
      TileGetDepthSize(960, 540, 0, derived.z_info.format,
                       derived.stencil_info.format, true, stencil_size, htile_size, depth_size) &&
	          ResolveHtileClearTarget(derived, htile_size.size, resolved) &&
          resolved.address == derived.htile_data_base_addr &&
          resolved.size == htile_size.size &&
	          !ResolveHtileClearTarget(derived, htile_size.size + 0x8000,
	                                   resolved),
      "extent-derived HTile validation or descriptor cross-check regressed");

  auto derived_z16 = derived;
  derived_z16.z_info.format =
      Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16);
  derived_z16.stencil_info.format =
      Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid);
  derived_z16.stencil_read_base_addr = 0;
  derived_z16.stencil_write_base_addr = 0;
  Require(
      "HtileClearTargetResolution", "derived Z16 depth-only target",
      TileGetDepthSize(960, 540, 0, derived_z16.z_info.format,
                       derived_z16.stencil_info.format, true, stencil_size, htile_size, depth_size) &&
          stencil_size.align == 0 && stencil_size.size == 0 &&
	          ResolveHtileClearTarget(derived_z16, htile_size.size, resolved) &&
          resolved.address == derived_z16.htile_data_base_addr &&
          resolved.size == htile_size.size,
      "extent-derived Z16 depth-only HTile target was rejected");

  auto derived_z16s8 = derived;
  derived_z16s8.z_info.format =
      Prospero::GpuEnumValue(Prospero::DepthFormat::kZ16);
  Require(
      "HtileClearTargetResolution", "derived Z16S8 target",
      TileGetDepthSize(960, 540, 0, derived_z16s8.z_info.format,
                       derived_z16s8.stencil_info.format, true, stencil_size, htile_size, depth_size) &&
          stencil_size.size != 0 && stencil_size.align == 0x10000 &&
	          ResolveHtileClearTarget(derived_z16s8, htile_size.size, resolved) &&
          resolved.address == derived_z16s8.htile_data_base_addr &&
          resolved.size == htile_size.size,
      "extent-derived Z16S8 HTile target was rejected");

  auto derived_8x = derived;
  derived_8x.z_info.num_samples = 3;
  auto malformed_fragments = derived;
  malformed_fragments.z_info.num_samples = 4;
  Require("HtileClearTargetResolution", "fragment-independent metadata plane",
	          ResolveHtileClearTarget(derived_8x, htile_size.size, resolved) &&
              resolved.address == derived_8x.htile_data_base_addr &&
              resolved.size == htile_size.size &&
              render_sample_count(derived_8x.z_info.num_samples) == 8 &&
	              !ResolveHtileClearTarget(malformed_fragments, htile_size.size,
	                                       resolved),
          "HTile clear was coupled to attachment MSAA support or admitted an "
          "invalid fragment field");
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

void CheckSharedFenceResourceLifetime() {
  bool destroyed = false;
  auto image = std::make_shared<FenceLifetimeProbe>(&destroyed);
  FenceResourceRetainer first;
  FenceResourceRetainer second;
  first.Retain(image);
  second.Retain(image);
  first.Retain(image);
  image.reset();
  Require("SharedFenceResourceLifetime", "retained",
          !destroyed && !first.Empty() && !second.Empty(),
          "cache removal destroyed an image retained by command buffers");
  first.ReleaseAfterFence();
  Require("SharedFenceResourceLifetime", "first fence",
          !destroyed && first.Empty() && !second.Empty(),
          "first command-buffer fence destroyed another buffer's image");
  second.ReleaseAfterFence();
  Require("SharedFenceResourceLifetime", "last fence",
          destroyed && second.Empty(),
          "last referencing command-buffer fence did not destroy the image");
  std::printf("[host]    %-32s ok\n", "SharedFenceResourceLifetime");
}

void CheckHostDmaMetadataReuse() {
  GraphicContext context{};
  constexpr uintptr_t base = 0x0000000200010000ull;
  constexpr uint64_t allocation_size = 0x10000;
  constexpr uint64_t metadata_size = 0x8000;
  auto *memory = static_cast<uint8_t *>(
      VirtualAlloc(reinterpret_cast<void *>(base), allocation_size,
                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
  Require("HostDmaMetadataReuse", "allocation",
          memory == reinterpret_cast<void *>(base),
          "fixed VirtualAlloc failed");
  std::memset(memory + metadata_size, 0x5a, 0x1000);

  ResourceMutex resource_mutex;
  CacheFaultContext fault_context;
  PageManager page_manager(CacheFault, &fault_context);
  BufferCache buffer_cache(context, page_manager, resource_mutex);
  TextureCache texture_cache(context, page_manager, buffer_cache,
                             resource_mutex);
  fault_context.texture = &texture_cache;
  buffer_cache.SetTextureCache(texture_cache);
  page_manager.OnGpuMap(base, allocation_size);
  texture_cache.RegisterMeta(base, metadata_size);

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
  buffer_cache.CopyBuffer(nullptr, base, base + metadata_size, 0x1000);
  Require("HostDmaMetadataReuse", "copy",
          std::memcmp(memory, memory + metadata_size, 0x1000) == 0,
          "post-clear CPU DMA copy did not publish backing");
  Require("HostDmaMetadataReuse", "fill fault",
          page_manager.HandleFault(PageFaultAccess::Write, base + 0x1000),
          "fill destination did not transfer to CPU ownership");
  buffer_cache.FillBuffer(nullptr, base + 0x1000, 0x1000, 0x11223344);
  Require("HostDmaMetadataReuse", "fill",
          reinterpret_cast<uint32_t *>(memory + 0x1000)[0] == 0x11223344 &&
              reinterpret_cast<uint32_t *>(memory + 0x1ffc)[0] == 0x11223344,
          "post-clear CPU DMA fill did not publish backing");
  Require("HostDmaMetadataReuse", "identity",
          texture_cache.QueryRegion(base, metadata_size).metadata_pages &&
              !texture_cache.QueryRegion(base, 0x2000).gpu_metadata_bytes &&
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
  AppendVop3(&code, 0x360u, 20, Vgpr(6),
             InlineU32(0)); // v_readlane_b32 s20, v6, 0
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
          ShaderRecompiler::TryRecompile(code, options, result, &error),
          error);
  Require("EmbeddedFetchLaneSpill", "fetch rewrite",
          vertex.resource_fetch_components[0] == 4,
          "lane-spilled fetch descriptor was not recognized and rewritten");
  std::printf("[host]    %-32s ok\n", "EmbeddedFetchLaneSpill");
}

void CheckReferenceClockScale() {
  uint64_t value = 0;
  Require("ReferenceClockScale", "zero",
          Sync::ScaleReferenceClock(0, 3000000000ull, value) && value == 0,
          "zero host tick did not produce a zero GPU clock");
  Require("ReferenceClockScale", "fractional second",
          Sync::ScaleReferenceClock(1500000000ull, 3000000000ull, value) &&
              value == 50000000ull,
          "host half-second did not scale to 50,000,000 ticks");
  Require("ReferenceClockScale", "whole and fractional",
          Sync::ScaleReferenceClock(3750000000ull, 3000000000ull, value) &&
              value == 125000000ull,
          "host 1.25 seconds did not scale to 125,000,000 ticks");
  Require("ReferenceClockScale", "monotonic floor",
          Sync::ScaleReferenceClock(3750000001ull, 3000000000ull, value) &&
              value == 125000000ull,
          "sub-reference-tick increment did not use a monotonic floor");
  Require("ReferenceClockScale", "guards",
          !Sync::ScaleReferenceClock(1, 0, value) &&
              !Sync::ScaleReferenceClock(UINT64_MAX, 1, value),
          "invalid frequency or overflow was accepted");
  std::printf("[host]    %-32s ok\n", "ReferenceClockScale");
}

void CheckClipControlDepthClipState() {
  HW::ClipControl clip;
  Require("ClipControlDepthClipState", "default",
          clip.IsZClipModeRepresentable() && clip.IsZClipEnabled(),
          "default paired Z clipping was not enabled");

  clip.min_z_clip_disable = true;
  Require("ClipControlDepthClipState", "asymmetric near",
          !clip.IsZClipModeRepresentable(),
          "asymmetric near-plane state was accepted");

  clip.min_z_clip_disable = false;
  clip.max_z_clip_disable = true;
  Require("ClipControlDepthClipState", "asymmetric far",
          !clip.IsZClipModeRepresentable(),
          "asymmetric far-plane state was accepted");

  clip.min_z_clip_disable = true;
  Require("ClipControlDepthClipState", "both disabled",
          clip.IsZClipModeRepresentable() && !clip.IsZClipEnabled(),
          "paired Z-clip disable was not represented");
  std::printf("[host]    %-32s ok\n", "ClipControlDepthClipState");
}

void CheckPm4WaitResume() {
  GraphicsInitJmpTables();
  CommandProcessor processor;

  uint32_t label = 0;
  uint32_t prefix = 0;
  uint32_t child_observation = UINT32_MAX;
  uint32_t suffix = 0;
  const auto address = [](const void *value) {
    return reinterpret_cast<uint64_t>(value);
  };

  std::array<uint32_t, 12> child{};
  child[0] = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
  child[1] = 0;
  child[2] = static_cast<uint32_t>(address(&child_observation));
  child[3] = static_cast<uint32_t>(address(&child_observation) >> 32u);
  child[4] = 0;
  child[5] = KYTY_PM4(7, Pm4::IT_NOP, Pm4::R_WAIT_MEM_32);
  child[6] = static_cast<uint32_t>(address(&label));
  child[7] = static_cast<uint32_t>(address(&label) >> 32u);
  child[8] = UINT32_MAX;
  child[9] = 1;
  child[10] = 0x10u | 3u;

  std::array<uint32_t, 4> nested{};
  nested[0] = KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0);
  nested[1] = static_cast<uint32_t>(address(child.data()));
  nested[2] = static_cast<uint32_t>(address(child.data()) >> 32u);
  nested[3] = 0x0f200000u | static_cast<uint32_t>(child.size());

  std::array<uint32_t, 14> commands{};
  commands[0] = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
  commands[1] = 0;
  commands[2] = static_cast<uint32_t>(address(&prefix));
  commands[3] = static_cast<uint32_t>(address(&prefix) >> 32u);
  commands[4] = 11;
  commands[5] = KYTY_PM4(4, Pm4::IT_INDIRECT_BUFFER, 0);
  commands[6] = static_cast<uint32_t>(address(nested.data()));
  commands[7] = static_cast<uint32_t>(address(nested.data()) >> 32u);
  commands[8] = 0x0f200000u | static_cast<uint32_t>(nested.size());
  commands[9] = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
  commands[10] = 0;
  commands[11] = static_cast<uint32_t>(address(&suffix));
  commands[12] = static_cast<uint32_t>(address(&suffix) >> 32u);
  commands[13] = 22;

  Pm4Execution execution;
  Require("Pm4WaitResume", "suspend",
          processor.Process(execution, commands.data(), commands.size()) ==
                  Pm4ProcessResult::Blocked &&
              prefix == 11 && child_observation == 0 && suffix == 0,
          "blocked indirect wait did not preserve its command position");

  label = 1;
  child[4] = 1;
  Require("Pm4WaitResume", "resume",
          processor.Process(execution, commands.data(), commands.size()) ==
                  Pm4ProcessResult::Complete &&
              prefix == 11 && child_observation == 0 && suffix == 22,
          "resumed indirect wait replayed a child or did not finish its parent");
  std::printf("[host]    %-32s ok\n", "Pm4WaitResume");
}

void CheckPm4CeCompletion() {
  GraphicsInitJmpTables();
  CommandProcessor processor;
  uint32_t suffix = 0;
  const auto address = reinterpret_cast<uint64_t>(&suffix);

  std::array<uint32_t, 7> commands{};
  commands[0] = 0xc0008600u;
  commands[1] = 1;
  commands[2] = KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0);
  commands[3] = 0;
  commands[4] = static_cast<uint32_t>(address);
  commands[5] = static_cast<uint32_t>(address >> 32u);
  commands[6] = 33;

  processor.ResetDeCe();
  Pm4Execution execution;
  Require("Pm4CeCompletion", "wait",
          processor.Process(execution, commands.data(), commands.size()) ==
                  Pm4ProcessResult::Blocked &&
              suffix == 0,
          "DE did not wait for an active CE stream");

  processor.SetCeComplete(true);
  Require("Pm4CeCompletion", "complete",
          processor.Process(execution, commands.data(), commands.size()) ==
                  Pm4ProcessResult::Complete &&
              suffix == 33,
          "DE remained blocked after the CE stream completed");
  std::printf("[host]    %-32s ok\n", "Pm4CeCompletion");
}

} // namespace
} // namespace Libs::Graphics

int main(int argc, char **argv) {
  using namespace Libs::Graphics;

  EnsureConfigInitialized();
  if (argc == 2 && std::strcmp(argv[1], "--clip-control-only") == 0) {
    CheckClipControlDepthClipState();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--reference-clock-only") == 0) {
    CheckReferenceClockScale();
    return 0;
  }
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  if (argc == 2 && std::strcmp(argv[1], "--reverse-rt-death") == 0) {
    RunReverseRenderTargetDeathCase();
  }
  if (argc == 2 && std::strcmp(argv[1], "--reverse-rt-only") == 0) {
    CheckReverseRenderTargetFormatContract();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--standard64-rt-only") == 0) {
    CheckStandard64RenderTargetTileRoundTrip();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--image-overlap-only") == 0) {
    CheckImageOverlapResolution();
    CheckQueryRegionAggregation();
    VulkanHarness vulkan;
    vulkan.CheckQueryRegionImageClassification();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--htile-clear-only") == 0) {
    CheckHtileClearTargetResolution();
    CheckOverlappingMetadataViews();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--layered-image-only") == 0) {
    CheckColorResolveLayers();
    CheckGpuMetadataReuse();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--image-view-only") == 0) {
    CheckSampledColorViews();
    CheckSampledVideoOutView();
    return 0;
  }
  if (argc == 2 && std::strcmp(argv[1], "--image-view-cache-only") == 0) {
    VulkanHarness vulkan;
    vulkan.CheckRenderTargetViewCache();
    vulkan.CheckDepthTargetSampledViewCache();
    vulkan.CheckVideoOutSampledViewCache();
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
  if (argc == 3 &&
      std::strcmp(argv[1], "--storage-texture-access-death") == 0) {
    RunStorageTextureAccessDeathCase(argv[2]);
  }
  if (argc == 3 && std::strcmp(argv[1], "--meta-overlap-death") == 0) {
    RunMetaOverlapDeathCase(argv[2]);
  }
  if (argc == 3 && std::strcmp(argv[1], "--metadata-descriptor-death") == 0) {
    RunMetadataDescriptorDeathCase(argv[2]);
  }
  CheckReverseRenderTargetFormatContract();
  CheckSampledColorViews();
  CheckSampledVideoOutView();
  CheckSampledDepthResource();
  CheckSampledDepthDescriptor();
  CheckBufferCacheRangeMerge();
  CheckBasicStorageTextureDescriptor();
  CheckStorageTextureLinearUploadLayout();
  CheckStorageTextureDepthTileUploadLayout();
  CheckStorageTextureLinearReadbackLayout();
  CheckStorageImageSwizzleSpecializationId();
  CheckColorResolveLayers();
  CheckStandard64RenderTargetTileRoundTrip();
  CheckStorageTextureVolumeUploadLayout();
  CheckStorageTextureVolumeMipRegions();
  CheckStorageTextureGpuOwnedRebindState();
  CheckStorageTextureSampledReuse();
  CheckStorageTextureDepthAlias();
  CheckStorageTextureAccessPermissions();
  CheckMetaOverlapDeaths();
  CheckOverlappingMetadataViews();
  CheckGpuMetadataReuse();
  CheckMetadataReuseDescriptors();
  CheckImageOverlapResolution();
  CheckQueryRegionAggregation();
  CheckNativeMsaaState();
  CheckDepthHtileStencilCompatibility();
  CheckStencilAttachmentAccess();
  CheckDepthTargetFootprints();
  CheckHtileClearTargetResolution();
  CheckSharedFenceResourceLifetime();
  CheckHostDmaMetadataReuse();
#else
  (void)argc;
  (void)argv;
#endif
  CheckClipControlDepthClipState();
  CheckReferenceClockScale();
  CheckPm4WaitResume();
  CheckPm4CeCompletion();
  CheckEmbeddedFetchVertexOffset();
  CheckEmbeddedFetchLaneSpill();
  CheckPs5GameExampleImageClearRuntimeShape();
  VulkanHarness vulkan;
  vulkan.CheckCommandPoolGrowth();
  vulkan.CheckGpuTilerCpuParity();
  vulkan.CheckQueryRegionImageClassification();
  vulkan.CheckMutableStorageSrgbView();
  vulkan.CheckMutableRenderTargetBgraStorageView();
  vulkan.CheckRenderTargetViewCache();
  vulkan.CheckDepthTargetSampledViewCache();
  vulkan.CheckVideoOutSampledViewCache();
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
