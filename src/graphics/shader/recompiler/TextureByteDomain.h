#pragma once

#include "graphics/guest_gpu/tile.h"
#include "graphics/shader/shaderBindings.h"

#include <bitset>
#include <span>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler {
// A bounded integer-image contract. The caller separately proves instruction
// provenance and immutable, coherent backing; this helper only handles layout.
inline bool GetTextureByteLayout(const ShaderTextureResource& texture,
                                 TileSurfaceLayout& layout) {
    if (texture.Type() != Prospero::ImageType::kColor2D ||
        texture.Format() != Prospero::BufferFormat::k8_8_8_8UInt ||
        texture.TileMode() != Prospero::TileMode::kStandard4KB || texture.BaseLevel() ||
        texture.LastLevel() || texture.Depth() || texture.BaseArray5() ||
        texture.MetaCompress() || texture.WriteCompress()) return false;
    const TileSurfaceDescription description{texture.Format(), texture.TileMode(),
        TileSurfaceDimension::Dim2D, uint32_t{texture.Width5()} + 1u,
        uint32_t{texture.Height5()} + 1u, 1u, 1u, 1u};
    return TileGetTiledTextureLayout(description, layout) &&
           layout.total_size <= 8u * 1024u * 1024u;
}

inline bool CollectTextureByteDomain(const ShaderTextureResource& texture,
                                     std::span<const uint32_t> words, uint32_t channels,
                                     std::vector<uint32_t>& values) {
    values.clear();
    if (!channels || (channels & ~15u)) return false;
    const uint8_t swizzle[4]{texture.DstSelX(), texture.DstSelY(), texture.DstSelZ(), texture.DstSelW()};
    for (uint32_t channel = 0; channel < 4u; ++channel)
        if ((channels & (1u << channel)) && swizzle[channel] != 0u && swizzle[channel] != 1u &&
            (swizzle[channel] < 4u || swizzle[channel] > 7u)) return false;
    TileSurfaceLayout layout;
    if (!GetTextureByteLayout(texture, layout) || words.size_bytes() != layout.total_size) return false;
    const auto& block = layout.texture.block;
    const auto& mip = layout.mips[0];
    const uint64_t blocks_per_row = mip.padded_width / block.block_width;
    std::bitset<256> found;
    for (uint32_t y = 0; y <= texture.Height5(); ++y) {
        for (uint32_t x = 0; x <= texture.Width5(); ++x) {
            uint32_t local = 0;
            if (!TileGetBlockOffset(block, x % block.block_width + mip.tail_x,
                                    y % block.block_height + mip.tail_y, 0u, local)) return false;
            const uint64_t index = uint64_t{y / block.block_height} * blocks_per_row + x / block.block_width;
            const uint64_t offset = mip.offset + index * block.block_size + local;
            if (offset % 4u || offset > words.size_bytes() || words.size_bytes() - offset < 4u) return false;
            const auto word = words[static_cast<size_t>(offset / 4u)];
            for (uint32_t channel = 0; channel < 4u; ++channel) {
                if (!(channels & (1u << channel))) continue;
                const auto selected = swizzle[channel];
                found.set(selected <= 1u ? selected : (word >> ((selected - 4u) * 8u)) & 255u);
            }
        }
    }
    for (uint32_t value = 0; value < 256u; ++value) if (found.test(value)) values.push_back(value);
    return true;
}
} // namespace Libs::Graphics::ShaderRecompiler
