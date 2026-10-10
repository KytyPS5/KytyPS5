#include "ShaderCaptureMetadata.h"
#include "graphics/guest_gpu/tile.h"

#include <algorithm>
#include <bitset>
#include <numeric>

namespace Libs::Graphics::ShaderRecompiler::Capture {
bool CollectCapturedTextureBytes(const ShaderTextureResource& texture,
                                 std::span<const uint32_t> words, uint32_t channels,
                                 std::vector<uint32_t>& values) {
    values.clear();
    if (!channels || (channels & ~15u) || texture.Type() != Prospero::ImageType::kColor2D ||
        texture.Format() != Prospero::BufferFormat::k8_8_8_8UInt ||
        texture.TileMode() != Prospero::TileMode::kStandard4KB || texture.BaseLevel() ||
        texture.LastLevel() || texture.Depth() || texture.BaseArray5() ||
        texture.MetaCompress() || texture.WriteCompress()) return false;
    const uint8_t swizzle[4]{texture.DstSelX(), texture.DstSelY(), texture.DstSelZ(), texture.DstSelW()};
    for (uint32_t channel = 0; channel < 4u; ++channel)
        if ((channels & (1u << channel)) && swizzle[channel] != 0u && swizzle[channel] != 1u &&
            (swizzle[channel] < 4u || swizzle[channel] > 7u)) return false;
    const TileSurfaceDescription description{texture.Format(), texture.TileMode(),
        TileSurfaceDimension::Dim2D, uint32_t{texture.Width5()} + 1u,
        uint32_t{texture.Height5()} + 1u, 1u, 1u, 1u};
    TileSurfaceLayout layout;
    if (!TileGetTiledTextureLayout(description, layout) || layout.total_size > 8u * 1024u * 1024u ||
        words.size_bytes() != layout.total_size) return false;
    const auto& block = layout.texture.block;
    const auto& mip = layout.mips[0];
    const uint64_t blocks_per_row = mip.padded_width / block.block_width;
    std::bitset<256> found;
    for (uint32_t y = 0; y < description.height; ++y) {
        for (uint32_t x = 0; x < description.width; ++x) {
            uint32_t local = 0;
            if (!TileGetBlockOffset(block, x + mip.tail_x, y + mip.tail_y, 0u, local)) return false;
            const uint64_t index = (uint64_t{y / block.block_height} * blocks_per_row + x / block.block_width);
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

nlohmann::ordered_json CaptureTextureInputs(const IR::ResourcePlan& plan,
                                           const IR::SrtRuntime& runtime, uint32_t logical) {
    using Json = nlohmann::ordered_json;
    Json result{{"kind", "diagnostic_texture_inputs"}, {"logical_image", logical},
                {"specialization_proof", false}, {"entries", Json::array()}};
    // Diagnostic evidence only. Successful reads do not establish absence of
    // writes/attachment feedback during the draw, or selector provenance.
    if (logical >= plan.info.images.size() || !runtime.read_specialization_memory) {
        result["error"] = "image or strict reader unavailable";
        return result;
    }
    const auto& image = plan.info.images[logical];
    if (image.source >= plan.descriptor_sources.size() || !image.read || image.written || image.atomic) {
        result["error"] = "read-only image required";
        return result;
    }
    const auto& source = plan.descriptor_sources[image.source];
    if (!source.inline_descriptor || source.inline_descriptor->image_table ||
        source.inline_descriptor->address_table) {
        result["error"] = "direct inline image required";
        return result;
    }
    const auto& table = *source.inline_descriptor;
    if (!table.selector_stride || (table.descriptor_dwords != 4 && table.descriptor_dwords != 8)) {
        result["error"] = "unsupported inline image geometry";
        return result;
    }
    auto strict = runtime;
    strict.read_memory = runtime.read_specialization_memory;
    IR::SrtWalker walker(plan, strict);
    IR::DescriptorValue value;
    if (!walker.EvaluateDescriptor(table.buffer_source, value) || value.dword_count != 4) {
        result["error"] = "source buffer unavailable";
        return result;
    }
    ShaderBufferResource buffer;
    std::copy_n(value.dwords.begin(), 4u, buffer.fields);
    if (buffer.Type() || buffer.SwizzleEnabled() || buffer.AddTid() || buffer.IndexStride()) {
        result["error"] = "unsupported source buffer addressing";
        return result;
    }
    const uint64_t size = buffer.GetSize();
    const uint64_t step = table.selector_limit ? table.selector_stride
        : std::gcd<uint64_t>(table.selector_stride, uint64_t{1} << 32u);
    const uint64_t count = table.selector_limit ? table.selector_limit
        : size >= 4 && size - 4 >= table.descriptor_offset
        ? std::min<uint64_t>(UINT32_MAX, ((size - 4) & ~uint64_t{3}) + 3 - table.descriptor_offset) / step + 1 : 0;
    if (count > 4096u) {
        result["error"] = "diagnostic descriptor budget exceeded";
        return result;
    }
    size_t remaining_bytes = 8u * 1024u * 1024u;
    for (uint64_t probe = 0; probe < count; ++probe) {
        const auto key = static_cast<uint32_t>(probe * step);
        const uint64_t offset = (uint64_t{key} + table.descriptor_offset) & ~uint64_t{3};
        const uint64_t bytes = table.descriptor_dwords * sizeof(uint32_t);
        if (offset > size || bytes > size - offset) continue;
        const uint64_t base = buffer.Base48() & ~uint64_t{3};
        constexpr uint64_t mask = (uint64_t{1} << 48u) - 1;
        if (offset > mask - base || bytes > mask - base - offset + 1) continue;
        ShaderTextureResource texture;
        if (!strict.read_memory(strict.userdata, base + offset,
                                std::span(texture.fields).first(table.descriptor_dwords))) continue;
        if (texture.IsNull() || texture.Type() != Prospero::ImageType::kColor2D ||
            texture.Format() != Prospero::BufferFormat::k8_8_8_8UInt ||
            texture.TileMode() != Prospero::TileMode::kStandard4KB ||
            texture.BaseLevel() || texture.LastLevel() || texture.Depth() || texture.BaseArray5() ||
            texture.MetaCompress() || texture.WriteCompress()) continue;
        TileSurfaceLayout layout;
        const TileSurfaceDescription description{texture.Format(), texture.TileMode(),
            TileSurfaceDimension::Dim2D, uint32_t{texture.Width5()} + 1u,
            uint32_t{texture.Height5()} + 1u, 1u, 1u, 1u};
        if (!TileGetTiledTextureLayout(description, layout) || layout.total_size % 4 ||
            layout.total_size > remaining_bytes || texture.Base40() > mask ||
            layout.total_size > mask - texture.Base40() + 1) {
            result["error"] = "diagnostic texture layout or byte budget exceeded";
            break;
        }
        remaining_bytes -= static_cast<size_t>(layout.total_size);
        Json entry{{"selector_key", key}, {"descriptor_address", base + offset},
                   {"descriptor", std::vector<uint32_t>(std::begin(texture.fields), std::end(texture.fields))},
                   {"address", texture.Base40()}, {"bytes", layout.total_size},
                   {"width", description.width}, {"height", description.height},
                   {"payload_captured", false}, {"descriptor_stable", false}};
        std::vector<uint32_t> words(static_cast<size_t>(layout.total_size / 4));
        if (strict.read_memory(strict.userdata, texture.Base40(), words)) {
            ShaderTextureResource after;
            const bool stable = strict.read_memory(strict.userdata, base + offset,
                std::span(after.fields).first(table.descriptor_dwords)) &&
                std::equal(std::begin(texture.fields), std::end(texture.fields), std::begin(after.fields));
            entry["descriptor_stable"] = stable;
            entry["payload_captured"] = true;
            entry["words"] = std::move(words);
        }
        result["entries"].push_back(std::move(entry));
    }
    return result;
}
} // namespace Libs::Graphics::ShaderRecompiler::Capture
