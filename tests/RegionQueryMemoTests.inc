  // Every remembered region query must equal a fresh walk of the page owner index.
  void CheckRegionQueryMemo() {
    constexpr const char *name = "RegionQueryMemo";
    constexpr uintptr_t base = 0x0000000250000000ull;
    constexpr uint64_t allocation_size = 0x800000;
    EnsureRuntimeContext();
    int64_t direct_offset = -1;
    Require(name, "direct allocation",
            Libs::LibKernel::Memory::KernelAllocateDirectMemory(
                0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), allocation_size,
                0x10000, 0, &direct_offset) == 0,
            "region query direct-memory allocation failed");
    void *mapped = reinterpret_cast<void *>(base);
    Require(name, "direct mapping",
            Libs::LibKernel::Memory::KernelMapDirectMemory(&mapped, allocation_size, 0x3, 0x10,
                                                           direct_offset, 0x10000) == 0 &&
                mapped == reinterpret_cast<void *>(base),
            "region query fixed mapping failed");
    {
      RenderContext context(m_runtime_context);
      auto &scheduler = context.GetCommandScheduler();
      HW::Context registers{};
      HW::UserConfig user_config{};
      HW::Shader shaders{};
      context.InitializeGpu(nullptr);
      scheduler.Begin(registers, user_config, shaders);
      context.MapMemory(base, allocation_size);
      auto &cache = context.GetTextureCache();
      const auto image_at = [&](uint64_t offset, uint32_t side) {
        const uint64_t size = uint64_t{side} * side * 4;
        auto desc = MakeLinearDesc(base + offset, size, vk::Format::eR8G8B8A8Unorm,
                                   Prospero::BufferFormat::k8_8_8_8UNorm,
                                   Prospero::ImageType::kColor2D, {side, side, 1}, 1, 4, 1);
        return TextureCacheTestAccess::InsertImage(cache, desc.info);
      };
      const auto check = [&](const char *stage, uint64_t offset, uint64_t size) {
        // The second query is answered from the memo.
        for (int round = 0; round < 2; round++) {
          const auto remembered =
              TextureCacheTestAccess::RegionQuery(cache, base + offset, size, true);
          const auto fresh =
              TextureCacheTestAccess::RegionQuery(cache, base + offset, size, false);
          Require(name, stage, remembered == fresh,
                  "a remembered region query differs from the page owner index");
        }
        return TextureCacheTestAccess::RegionQuery(cache, base + offset, size, false).size();
      };
      std::vector<ImageId> ids;
      for (uint32_t i = 0; i < 24; i++) {
        // Overlapping 256 KiB surfaces every 64 KiB put 4 owners on most pages.
        ids.push_back(image_at(0x10000 * i, 256));
      }
      const auto wide = check("initial", 0, 0x200000);
      const auto narrow = check("initial narrow", 0x40000, 0x1000);
      ids.push_back(image_at(0x40000, 64));
      Require(name, "insert seen", check("after insert", 0, 0x200000) == wide + 1 &&
                                       check("after insert narrow", 0x40000, 0x1000) == narrow + 1,
              "a remembered query missed an image registered after it");
      TextureCacheTestAccess::DeleteImage(cache, ids[4]);
      Require(name, "delete seen", check("after delete", 0, 0x200000) == wide &&
                                       check("after delete narrow", 0x40000, 0x1000) == narrow,
              "a remembered query kept an image unregistered after it");
      context.UnmapMemory(base, allocation_size);
      scheduler.Finish();
    }
    std::printf("[gpu]     %-32s ok\n", name);
  }

