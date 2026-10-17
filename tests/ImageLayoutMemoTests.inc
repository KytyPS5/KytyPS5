// A memoized layout must equal a fresh computation, and a refused layout must stay refused.
void CheckImageLayoutMemo() {
  constexpr const char *name = "ImageLayoutMemo";
  const auto make = [](Prospero::BufferFormat guest, vk::Format pixel, Prospero::TileMode tile,
                       Prospero::ImageType type, vk::Extent3D extent, uint32_t levels,
                       uint32_t layers, uint32_t samples = 1) {
    ImageInfo info{};
    info.guest_format = guest;
    info.pixel_format = pixel;
    info.tile_mode = tile;
    info.type = type;
    info.extent = extent;
    info.resources = {levels, layers};
    info.samples = samples;
    return info;
  };
  using BF = Prospero::BufferFormat;
  using TM = Prospero::TileMode;
  using IT = Prospero::ImageType;
  std::vector<ImageInfo> cases{
      make(BF::kBc1UNorm, vk::Format::eBc1RgbaUnormBlock, TM::kStandard64KB, IT::kColor2D,
           {2048, 2048, 1}, 12, 1),
      make(BF::kBc7UNorm, vk::Format::eBc7UnormBlock, TM::kStandard64KB, IT::kColor2D,
           {1024, 512, 1}, 11, 1),
      make(BF::k8_8_8_8UNorm, vk::Format::eR8G8B8A8Unorm, TM::kStandard64KB, IT::kColor2D,
           {512, 512, 1}, 10, 6),
      make(BF::k8_8_8_8UNorm, vk::Format::eR8G8B8A8Unorm, TM::kStandard4KB, IT::kColor2D,
           {300, 200, 1}, 9, 1),
      make(BF::k8_8_8_8UNorm, vk::Format::eR8G8B8A8Unorm, TM::kLinear, IT::kColor2D,
           {300, 200, 1}, 5, 2),
      make(BF::k16_16_16_16Float, vk::Format::eR16G16B16A16Sfloat, TM::kStandard64KB,
           IT::kColor3D, {64, 64, 64}, 7, 1),
      make(BF::k32Float, vk::Format::eD32Sfloat, TM::kDepth, IT::kColor2D, {1920, 1080, 1}, 1,
           1),
      make(BF::k8_8_8_8UNorm, vk::Format::eR8G8B8A8Unorm, TM::kRenderTarget, IT::kColor2D,
           {1280, 720, 1}, 1, 1, 4),
      make(BF::k32_32Float, vk::Format::eR32G32Sfloat, TM::kStandard64KB, IT::kColor2D,
           {178, 100, 1}, 3, 1),
  };
  // Presets the layout reads back: a guest size or pitch the caller already knows.
  auto sized = cases[2];
  sized.data.size = 0x600000;
  cases.push_back(sized);
  auto pitched = cases[4];
  pitched.pitch = 512;
  cases.push_back(pitched);
  // Same inputs as cases[0] but sRGB: the pixel format is an input of its own.
  auto srgb = cases[0];
  srgb.pixel_format = vk::Format::eBc1RgbaSrgbBlock;
  cases.push_back(srgb);
  // A one-mip Standard4KB surface's mip-tail span depends on base alignment and metadata.
  for (const uint64_t address : {0x10000ull, 0x10100ull, 0x10400ull}) {
    for (const bool metadata : {false, true}) {
      for (const uint32_t side : {4u, 8u, 16u}) {
        auto tail = make(BF::k8_8_8_8UNorm, vk::Format::eR8G8B8A8Unorm, TM::kStandard4KB,
                         IT::kColor2D, {side, side, 1}, 1, 1);
        tail.data.address = address;
        tail.metadata.kind = metadata ? ImageMetadataKind::Dcc : ImageMetadataKind::None;
        cases.push_back(tail);
      }
    }
  }

  const auto same = [](const ImageInfo &a, const ImageInfo &b) {
    const auto tiling = [](const TileTextureBlockLayout &t) {
      return std::tuple{t.block.family, t.block.bytes_per_element, t.block.block_size,
                        t.block.block_width, t.block.block_height, t.block.block_depth,
                        t.texel_width, t.texel_height};
    };
    return a.data.size == b.data.size && a.pitch == b.pitch &&
           a.bytes_per_block == b.bytes_per_block && a.mip_layout == b.mip_layout &&
           tiling(a.tiling) == tiling(b.tiling) && a.tiled_slice_stride == b.tiled_slice_stride &&
           a.linear_slice_stride == b.linear_slice_stride &&
           a.first_tail_level == b.first_tail_level;
  };
  for (int round = 0; round < 3; round++) {
    for (size_t i = 0; i < cases.size(); i++) {
      auto fresh = cases[i];
      auto replayed = cases[i];
      const bool fresh_ok = fresh.TryUpdateSizeUncached();
      const bool replayed_ok = replayed.TryUpdateSize();
      Require(name, "layout accepted", fresh_ok && replayed_ok,
              "a supported guest layout was refused");
      Require(name, "replayed layout", same(fresh, replayed),
              "a memoized layout differs from a fresh computation of the same inputs");
    }
  }
  Require(name, "preset size kept", [&] {
    auto info = sized;
    return info.TryUpdateSize() && info.data.size == 0x600000;
  }(), "a preset guest size was replaced by the memoized layout of an unsized twin");
  // A tiled volume cannot carry array layers; the refusal must not be cached as a layout.
  auto refused = make(BF::k8_8_8_8UNorm, vk::Format::eR8G8B8A8Unorm, TM::kStandard64KB,
                      IT::kColor3D, {16, 16, 16}, 1, 2);
  for (int round = 0; round < 2; round++) {
    auto info = refused;
    Require(name, "refused layout", !info.TryUpdateSize(),
            "a tiled volume with array layers was accepted");
  }
  std::printf("[host]    %-32s ok\n", name);
}

