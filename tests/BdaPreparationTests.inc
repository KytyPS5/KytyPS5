// GPU regression for KYTY_BDA_DIRTY_CACHE=1. Included by the renderer harness.
void CheckBdaPreparationCache(GraphicContext& graphics) {
  constexpr const char* name = "BdaPreparationCache";
  constexpr uintptr_t base = 0x0000000200d00000ull;
  constexpr uint64_t size = 0x10000;
  RenderContext context(graphics);
  auto& scheduler = context.GetCommandScheduler();
  HW::Context registers{};
  HW::UserConfig user_config{};
  HW::Shader shaders{};
  scheduler.Begin(registers, user_config, shaders);
  context.InitializeGpu(nullptr);
  int64_t direct_offset = -1;
  Require(name, "allocation", Libs::LibKernel::Memory::KernelAllocateDirectMemory(
      0, Libs::LibKernel::Memory::KernelGetDirectMemorySize(), size, size, 0, &direct_offset) == 0,
      "guest allocation failed");
  void* mapped = reinterpret_cast<void*>(base);
  Require(name, "mapping", Libs::LibKernel::Memory::KernelMapDirectMemory(
      &mapped, size, 0x3, 0x10, direct_offset, size) == 0 && mapped == reinterpret_cast<void*>(base),
      "guest mapping failed");
  std::memset(mapped, 0, size);
  context.MapMemory(base, 0x4000);
  Libs::LibKernel::Memory::InstallGpuResources(&context);
  context.GetGpu().SendCommandSync([&] {
    auto& cache = context.GetBufferCache();
    const auto id = cache.FindBuffer(base, 0x4000);
    context.PrepareBda();
    const auto allocation = cache.ObtainBuffer(base, 0x4000, false, false, id);
    context.PrepareBda();
    const auto first = context.GetBdaPreparationStats();
    context.PrepareBda();
    auto stats = context.GetBdaPreparationStats();
    Require(name, "clean reuse", stats.scans == first.scans && stats.reused == first.reused + 1,
        "unchanged preparation was rescanned");
    cache.InvalidateMemory(base, 4);
    *static_cast<uint32_t*>(mapped) = 0x12345678u;
    context.PrepareBda();
    Require(name, "CPU invalidation", context.GetBdaPreparationStats().scans == stats.scans + 1,
        "CPU update reused stale preparation");
    Buffer readback(graphics, scheduler, MemoryUsage::Download, 0, AllFlags, 0x1000);
    auto verify = [&](uint32_t expected) {
      auto& command = scheduler.Current();
      command.EndRendering();
      vk::MemoryBarrier before{};
      before.srcAccessMask = vk::AccessFlagBits::eMemoryWrite;
      before.dstAccessMask = vk::AccessFlagBits::eTransferRead;
      command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
          vk::PipelineStageFlagBits::eTransfer, {}, 1, &before, 0, nullptr, 0, nullptr);
      const vk::BufferCopy copy{allocation.second, 0, 4};
      command.Handle().copyBuffer(allocation.first->Handle(), readback.Handle(), 1, &copy);
      vk::MemoryBarrier after{};
      after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
      after.dstAccessMask = vk::AccessFlagBits::eHostRead;
      command.Handle().pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
          vk::PipelineStageFlagBits::eHost, {}, 1, &after, 0, nullptr, 0, nullptr);
      scheduler.Finish();
      readback.Invalidate(0, 4);
      uint32_t actual = 0;
      std::memcpy(&actual, readback.Mapped().data(), 4);
      if (actual != expected) std::printf("BdaPreparationCache expected=%08x actual=%08x\n", expected, actual);
      Require(name, "GPU contents", actual == expected, "preparation lost updated GPU buffer contents");
    };
    verify(0x12345678u);
    // GPU-only writes must survive cache reuse without a stale CPU upload.
    const auto writable = cache.ObtainBuffer(base, 4, true, false);
    writable.first->Fill(writable.second, 4, 0xaabbccddu);
    stats = context.GetBdaPreparationStats();
    context.PrepareBda();
    Require(name, "GPU-only reuse", context.GetBdaPreparationStats().reused == stats.reused + 1,
        "GPU ownership unnecessarily invalidated CPU preparation");
    verify(0xaabbccddu);
    context.MapMemory(base + 0x8000, 0x1000);
    stats = context.GetBdaPreparationStats();
    context.PrepareBda();
    Require(name, "new mapping", context.GetBdaPreparationStats().scans == stats.scans + 1,
        "new mapping reused stale preparation");
    (void)cache.ObtainBuffer(base + 0x8000, 4, true, false);
    stats = context.GetBdaPreparationStats();
    context.PrepareBda();
    Require(name, "new buffer", context.GetBdaPreparationStats().scans == stats.scans + 1,
        "new buffer reused stale preparation");
    context.UnmapMemory(base + 0x8000, 0x1000);
    stats = context.GetBdaPreparationStats();
    context.PrepareBda();
    Require(name, "unmap", context.GetBdaPreparationStats().scans == stats.scans + 1,
        "unmap reused stale preparation");
    scheduler.Finish();
  });
  context.UnmapMemory(base, 0x4000);
  context.ShutdownGpu();
  Libs::LibKernel::Memory::InstallGpuResources(nullptr);
  Require(name, "unmapping", Libs::LibKernel::Memory::KernelMunmap(base, size) == 0,
      "guest unmap failed");
  Require(name, "release", Libs::LibKernel::Memory::KernelReleaseDirectMemory(direct_offset, size) == 0,
      "guest release failed");
  std::printf("[gpu]     %-32s ok\n", name);
}
