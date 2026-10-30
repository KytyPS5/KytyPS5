  void CheckPssr() {
    constexpr const char *name = "PssrLinear";
    constexpr uintptr_t base = 0x0000000206a00000ull;
    constexpr uint64_t size = 0x100000;
    constexpr auto packed = Prospero::BufferFormat::k11_11_10Float;
    constexpr auto linear = Prospero::TileMode::kLinear;
    constexpr auto tiled = Prospero::TileMode::kStandard256B;
    constexpr u32 red = 0x000003c0, green = 0x001e0000, blue = 0x78000000;
    const auto descriptor = [](uint64_t address, u32 width, u32 height,
                               Prospero::BufferFormat format, Prospero::TileMode tile,
                               u32 swizzle = DstSel(4, 5, 6, 7)) {
      ShaderTextureResource result{};
      result.fields[0] = static_cast<u32>(address >> 8);
      result.fields[1] = static_cast<u32>(address >> 40) |
                         (static_cast<u32>(format) << 20) | (((width - 1) & 3) << 30);
      result.fields[2] = ((width - 1) >> 2) | ((height - 1) << 14);
      result.fields[3] = swizzle | (static_cast<u32>(tile) << 20) |
                         (static_cast<u32>(Prospero::ImageType::kColor2D) << 28);
      result.fields[5] = 0x00700000;
      return result;
    };
    EnsureRuntimeContext();
    GraphicsInitJmpTables();
    int64_t physical = -1;
    Require(name, "allocate", LibKernel::Memory::KernelAllocateDirectMemory(
        0, LibKernel::Memory::KernelGetDirectMemorySize(), size, 0x10000, 0, &physical) == 0,
        "upscaling allocation failed");
    void *mapped = reinterpret_cast<void *>(base);
    Require(name, "map", LibKernel::Memory::KernelMapDirectMemory(
        &mapped, size, 0x3, 0x10, physical, 0x10000) == 0 &&
            mapped == reinterpret_cast<void *>(base), "upscaling mapping failed");
    std::memset(mapped, 0, size);
    RenderContext context(m_runtime_context);
    context.InitializeGpu(nullptr);
    LibKernel::Memory::InstallGpuResources(&context);
    context.GetGpu().SendCommandSync([&] {
      CommandProcessor processor(context, 0);
      processor.BufferInit();
      context.MapMemory(base, size);
      auto &executor = context.GetRenderExecutor();
      auto &cache = context.GetTextureCache();
      auto &scheduler = context.GetCommandScheduler();
      const auto resolve = [&](const ShaderTextureResource &texture) {
        ShaderRecompiler::IR::ImageResource resource{};
        resource.resource_class = ShaderRecompiler::IR::ImageResourceClass::Sampled;
        resource.numeric_class = Prospero::TextureNumericClass::Float;
        resource.dimension = ShaderRecompiler::Decoder::ImageDimension::Dim2D;
        resource.read = true;
        ShaderRecompiler::IR::DescriptorValue value{};
        value.dword_count = 8;
        std::copy_n(texture.fields, 8, value.dwords.begin());
        return executor.ResolveTexture(resource, value);
      };
      const auto write_linear = [&](const ShaderTextureResource &texture,
                                    std::span<const u32> texels) {
        auto info = MakeTilingImage(texture.Format(), texture.Width5() + 1u,
            texture.Height5() + 1u, 1, 1, linear, 0, false);
        const u32 words_per_texel = info.bytes_per_block / sizeof(u32);
        Require(name, "source size", texels.size() ==
            info.extent.width * info.extent.height * words_per_texel,
            "source texel fixture size is wrong");
        std::vector<u32> words(info.data.size / sizeof(u32));
        for (u32 y = 0; y < info.extent.height; ++y) {
          std::copy_n(texels.begin() + y * info.extent.width * words_per_texel,
              info.extent.width * words_per_texel,
              words.begin() + y * info.mip_layout[0].pitch * words_per_texel);
        }
        Require(name, "invalidate source", context.InvalidateMemory(
            texture.Base40(), info.data.size), "source fixture is outside mapped memory");
        LibKernel::Memory::WriteBacking(texture.Base40(), words.data(), info.data.size);
      };
      const auto run = [&](const ShaderTextureResource &input,
                            const ShaderTextureResource &output, u32 width, u32 height) {
        Pssr::Request request{};
        std::copy_n(input.fields, 8, request.input);
        std::copy_n(output.fields, 8, request.output);
        request.width = width;
        request.height = height;
        // The reserved packet includes trailing words that the parser must consume.
        constexpr u32 reserved_words = Pssr::RequestDwords + 9;
        std::vector<u32> packet(reserved_words, 0xdeadbeef);
        packet[0] = KYTY_PM4(reserved_words, Pm4::IT_NOP, Pm4::R_PSSR);
        std::memcpy(packet.data() + 1, &request, sizeof(request));
        u32 sentinel = 0;
        const auto address = reinterpret_cast<uint64_t>(&sentinel);
        packet.insert(packet.end(), {KYTY_PM4(5, Pm4::IT_WRITE_DATA, 0), 0,
            static_cast<u32>(address), static_cast<u32>(address >> 32), 33});
        Pm4Execution execution;
        Require(name, "packet and suffix", processor.Process(execution, packet) ==
            Pm4ProcessResult::Complete && sentinel == 33,
            "upscaling packet did not consume its reserved payload before the suffix");
        const auto binding = resolve(output);
        return ReadCachedTexel(name, context, binding.image_id, {},
            {output.Width5() + 1u, output.Height5() + 1u, 1},
            binding.desc.view_info.base_layer, binding.desc.view_info.base_level);
      };

      auto input = descriptor(base, 2, 1, packed, linear);
      auto output = descriptor(base + 0x10000, 4, 1, packed, tiled);
      write_linear(input, std::array<u32, 2>{red, green});
      Require(name, "CPU-first bilinear source", run(input, output, 2, 1) ==
          std::vector<u32>{red, 0x3a0 | (0x340 << 11), 0x340 | (0x3a0 << 11), green},
          "first-use source upload or linear interpolation is wrong");
      write_linear(input, std::array<u32, 2>{blue, blue});
      Require(name, "CPU source refresh", run(input, output, 2, 1) ==
          std::vector<u32>(4, blue), "reused source retained stale GPU texels");

      input = descriptor(base + 0x20000, 2, 1, packed, linear);
      output = descriptor(base + 0x30000, 2, 1, packed, tiled);
      write_linear(input, std::array<u32, 2>{red, green});
      Require(name, "same-size copy", run(input, output, 2, 1) ==
          std::vector<u32>{red, green}, "same-size upscaling lost source pixels");

      input = descriptor(base + 0x40000, 8, 4, packed, linear);
      output = descriptor(base + 0x50000, 8, 4, packed, tiled);
      std::vector<u32> cropped(32, blue);
      cropped[0] = cropped[1] = cropped[8] = cropped[9] = red;
      write_linear(input, cropped);
      Require(name, "active rectangle", run(input, output, 2, 2) ==
          std::vector<u32>(32, red),
          "equal allocation extents bypassed scaling or sampled outside the active rectangle");

      input = descriptor(base + 0x60000, 2, 1, packed, linear, DstSel(6, 5, 4, 7));
      output = descriptor(base + 0x70000, 2, 1, packed, tiled);
      write_linear(input, std::array<u32, 2>{red, green});
      Require(name, "sampled view swizzle", run(input, output, 2, 1) ==
          std::vector<u32>{blue, green}, "sampled descriptor swizzle was ignored");
      output.fields[3] = (output.fields[3] & ~0xfffu) | DstSel(6, 5, 4, 7);
      Require(name, "storage swizzle", run(input, output, 2, 1) ==
          std::vector<u32>{red, green}, "output channel mapping was ignored");

      input = descriptor(base + 0x80000, 2, 1,
          Prospero::BufferFormat::k16_16_16_16Float, linear);
      output = descriptor(base + 0x90000, 2, 1, packed, tiled);
      write_linear(input, std::array<u32, 4>{0x00003c00, 0x3c000000,
                                            0x3c000000, 0x3c000000});
      Require(name, "float16 input", run(input, output, 2, 1) ==
          std::vector<u32>{red, green}, "float16 input conversion failed");

      ImageDesc parent{};
      parent.type = BindingType::Texture;
      parent.info = MakeTilingImage(packed, 4, 2, 1, 2, tiled, 0, false);
      parent.info.data.address = base + 0xa0000;
      parent.info.pixel_format = vk::Format::eB10G11R11UfloatPack32;
      parent.view_info.format = parent.info.pixel_format;
      parent.view_info.type = vk::ImageViewType::e2DArray;
      parent.view_info.layer_count = 2;
      const auto parent_id = cache.FindImage(parent);
      (void)cache.FindTexture(parent_id, parent);
      vk::ClearValue clear{};
      clear.color.float32 = std::array<float, 4>{1, 0, 0, 1};
      TextureCacheTestAccess::ClearImage(cache, scheduler.Current(), parent_id,
          {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}, clear);
      clear.color.float32 = std::array<float, 4>{0, 0, 1, 1};
      TextureCacheTestAccess::ClearImage(cache, scheduler.Current(), parent_id,
          {vk::ImageAspectFlagBits::eColor, 0, 1, 1, 1}, clear);
      input = descriptor(parent.info.data.address, 4, 2, packed, tiled);
      output = descriptor(parent.info.data.address + parent.info.tiled_slice_stride,
          4, 2, packed, tiled);
      const auto resolved_output = resolve(output);
      Require(name, "parent array resolution", resolved_output.image_id == parent_id &&
          resolved_output.desc.view_info.base_layer == 1,
          "standalone output did not resolve into the existing array's second layer");
      Require(name, "resolved output layer", run(input, output, 4, 2) ==
          std::vector<u32>(8, red), "upscaling wrote layer zero instead of the resolved layer");
      Require(name, "source layer preserved", ReadCachedTexel(name, context, parent_id,
          {}, {4, 2, 1}) == std::vector<u32>(8, red),
          "upscaling modified the source layer of its shared native image");
      clear.color.float32 = std::array<float, 4>{0, 1, 0, 1};
      TextureCacheTestAccess::ClearImage(cache, scheduler.Current(), parent_id,
          {vk::ImageAspectFlagBits::eColor, 0, 1, 1, 1}, clear);
      Require(name, "resolved source layer", run(output, input, 4, 2) ==
          std::vector<u32>(8, green), "upscaling read layer zero instead of the resolved layer");

      input = descriptor(base + 0xc0000, 2, 1, packed, linear);
      output = descriptor(base + 0xd0000, 2, 1, packed, tiled);
      write_linear(input, std::array<u32, 2>{red, green});
      ImageDesc raw{};
      raw.type = BindingType::Texture;
      raw.info = MakeTilingImage(Prospero::BufferFormat::k32UInt, 2, 1, 1, 1,
                                 linear, 0, false);
      raw.info.data.address = input.Base40();
      raw.info.pixel_format = vk::Format::eR32Uint;
      raw.view_info.format = raw.info.pixel_format;
      const auto raw_id = cache.FindImage(raw);
      (void)cache.FindTexture(raw_id, raw);
      const auto float_binding = resolve(input);
      Require(name, "reinterpreted sampled view", float_binding.image_id == raw_id &&
          cache.GetImage(raw_id).backing.format == vk::Format::eR32Uint &&
          float_binding.desc.view_info.format == vk::Format::eB10G11R11UfloatPack32,
          "float sampled descriptor did not reuse the integer native allocation");
      Require(name, "sampled view format", run(input, output, 2, 1) ==
          std::vector<u32>{red, green}, "upscaling used the allocation format instead of its view");

      ImageDesc mip_parent{};
      mip_parent.type = BindingType::Texture;
      mip_parent.info = MakeTilingImage(packed, 128, 64, 2, 1, tiled, 0, false);
      mip_parent.info.data.address = base + 0xe0000;
      mip_parent.info.pixel_format = vk::Format::eB10G11R11UfloatPack32;
      mip_parent.view_info.format = mip_parent.info.pixel_format;
      mip_parent.view_info.level_count = 2;
      const auto mip_parent_id = cache.FindImage(mip_parent);
      (void)cache.FindTexture(mip_parent_id, mip_parent);
      clear.color.float32 = std::array<float, 4>{0, 0, 1, 1};
      TextureCacheTestAccess::ClearImage(cache, scheduler.Current(), mip_parent_id,
          {vk::ImageAspectFlagBits::eColor, 0, 2, 0, 1}, clear);
      clear.color.float32 = std::array<float, 4>{0, 1, 0, 1};
      TextureCacheTestAccess::ClearImage(cache, scheduler.Current(), mip_parent_id,
          {vk::ImageAspectFlagBits::eColor, 1, 1, 0, 1}, clear);
      input = descriptor(mip_parent.info.data.address + mip_parent.info.mip_layout[1].offset,
          64, 32, packed, tiled);
      output = descriptor(base + 0xf0000, 64, 32, packed, tiled);
      const auto resolved_mip = resolve(input);
      Require(name, "parent mip resolution", resolved_mip.image_id == mip_parent_id &&
          resolved_mip.desc.view_info.base_level == 1,
          "standalone source did not resolve into the existing image's second mip");
      Require(name, "resolved source mip", run(input, output, 64, 32) ==
          std::vector<u32>(64 * 32, green),
          "upscaling read mip zero instead of the resolved mip");
      context.UnmapMemory(base, size);
      scheduler.Finish();
    });
    LibKernel::Memory::InstallGpuResources(nullptr);
    context.ShutdownGpu();
    Require(name, "release", LibKernel::Memory::KernelCheckedReleaseDirectMemory(
        physical, size) == 0, "upscaling allocation release failed");
    std::printf("[gpu]     %-32s ok\n", name);
  }
