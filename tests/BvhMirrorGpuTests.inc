  // A guest BLAS of two fan nodes under a root box over boxes A (records 96, 97) and B (98, 99),
  // and a TLAS naming A, B scaled and moved, the whole BLAS with an empty mask, and an empty root.
  // Rays along +z must hit the mirror as the guest's own walk would, after a relink and after a
  // refit too.
  void CheckBvhMirror() {
    constexpr const char *name = "BvhMirror";
    constexpr auto page = BufferCache::CACHING_PAGESIZE;
    constexpr u32 Empty = ~0u, Miss = ~0u;
    EnsureRuntimeContext();
    int64_t direct = -1;
    void *mapped = nullptr;
    bool allocated =
        LibKernel::Memory::KernelAllocateDirectMemory(
            0, LibKernel::Memory::KernelGetDirectMemorySize(), 2 * page, page, 0, &direct) == 0;
    if (allocated) {
      allocated =
          LibKernel::Memory::KernelMapDirectMemory(&mapped, 2 * page, 0x3, 0, direct, page) == 0;
    }
    Require(name, "allocation", allocated, "guest allocation failed");
    const auto blas = reinterpret_cast<uint64_t>(mapped);
    const auto tlas = blas + page;
    auto *words = static_cast<u32 *>(mapped);
    std::fill_n(words, 2 * page / sizeof(u32), 0u);
    const auto bits = [](float value) { return std::bit_cast<u32>(value); };
    const auto header = [](u32 *at, u32 properties, u32 size) {
      at[0] = 0x5f525350u; // "PSR_BVHL"
      at[1] = 0x4c485642u;
      at[0x10 / 4] = size;
      at[0x18 / 4] = properties;
      at[0xfc / 4] = 1; // Version.
    };
    const auto link = [](u32 index, u32 kind) { return index << 3 | kind; };
    const auto node = [&](u32 unit) { return words + unit * 16; };
    const auto children = [&](u32 unit, std::array<u32, 4> links) {
      std::ranges::copy(links, node(unit));
    };
    // Five vertices (x, y) at z = 5; word 15 holds each triangle's remap byte.
    const auto fan = [&](u32 unit, std::array<float, 10> xy, u32 remap) {
      for (u32 vertex = 0; vertex < 5; ++vertex) {
        node(unit)[vertex * 3] = bits(xy[vertex * 2]);
        node(unit)[vertex * 3 + 1] = bits(xy[vertex * 2 + 1]);
        node(unit)[vertex * 3 + 2] = bits(5.0f);
      }
      node(unit)[15] = remap;
    };
    const auto record = [&](u32 index, u32 unit, u32 triangle, bool last) {
      words[index * 2] = link(unit, triangle);
      words[index * 2 + 1] = index | (last ? 1u << 31 : 0u);
    };
    header(words, 0x40, 100 * 8);
    words[0x50 / 4] = 12;  // Node units.
    words[0x58 / 4] = 100; // Records end
    words[0x84 / 4] = 96;  // and begin.
    children(4, {link(6, 5), link(8, 5), Empty, Empty});
    children(6, {link(96, 0), Empty, Empty, Empty});
    children(8, {link(98, 0), link(99, 0), Empty, Empty});
    // The guest's u and v weigh triangle vertices (2, 0), (0, 1), (1, 2) and
    // (2, 1).
    fan(10, {0, 0, 1, 0, 0, 1, 1, 1, 9, 9}, 0x02 | 0x04 << 8);
    fan(11, {3, 0, 9, 9, 3, 2, 5, 2, 4, 4}, 0x09 << 16 | 0x06 << 24);
    record(96, 10, 0, false);
    record(97, 10, 1, true);
    record(98, 11, 2, true);
    record(99, 11, 3, true);
    auto *top = words + page / sizeof(u32);
    header(top, 0x2c, 0x100 + 4 * 128);
    top[0x28 / 4] = 0x100; // Instance offset.
    top[0x70 / 4] = 4;     // Instance count.
    // Flags: mask in bits 8..15, bit 0 opaque, bit 1 non-opaque;
    // object-to-world columns.
    const auto instance = [&](u32 index, u32 root, u32 flags, std::array<float, 12> columns) {
      u32 *at = top + (0x100 + 128 * index) / 4;
      at[0] = static_cast<u32>(blas);
      at[1] = static_cast<u32>(blas >> 32);
      at[56 / 4] = root;
      at[60 / 4] = flags;
      for (u32 i = 0; i < columns.size(); ++i) {
        at[64 / 4 + i] = bits(columns[i]);
      }
    };
    constexpr std::array<float, 12> identity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    instance(0, link(6, 5), 0xff01, identity);
    instance(1, link(8, 5), 0xff02, {2, 0, 0, 0, 1, 0, 0, 0, 1, 10, 0, 1});
    instance(2, link(4, 5), 0x0001, identity);
    instance(3, Empty, 0xff01, identity);

    RenderContext context(m_runtime_context);
    auto &scheduler = context.GetCommandScheduler();
    auto &cache = context.GetBufferCache();
    HW::Context registers{};
    HW::UserConfig user_config{};
    HW::Shader shaders{};
    scheduler.Begin(registers, user_config, shaders);
    context.MapMemory(blas, 2 * page);
    {
      RayTracing mirror(context);
      constexpr auto usage = vk::BufferUsageFlagBits::eShaderDeviceAddress;
      Libs::Graphics::Buffer origins(m_runtime_context, scheduler, MemoryUsage::Upload, 0, usage,
                                     16 * 8);
      Libs::Graphics::Buffer output(m_runtime_context, scheduler, MemoryUsage::Download, 0, usage,
                                    4 * (2 + 6 * 8));
      const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute, 0, 32};
      vk::PipelineLayoutCreateInfo layout_info{};
      layout_info.pushConstantRangeCount = 1;
      layout_info.pPushConstantRanges = &range;
      vk::PipelineLayout layout = nullptr;
      RequireVk(name, "pipeline", m_device.createPipelineLayout(&layout_info, nullptr, &layout),
                "vkCreatePipelineLayout");
      const auto pipeline = CreateComputePipeline(m_device, GPU_TEST_RAY_QUERY_SPV, layout,
                                                  "create ray-query test pipeline");

      struct Hit {
        std::array<float, 2> origin;
        u32 record = Miss;
        u32 index = 0;
        float t = 0, u = 0, v = 0;
      };
      // Mirrors the TLAS and traces each origin; returns {valid, instances, hit
      // words...}.
      const auto trace = [&](std::span<const Hit> hits) {
        auto *origin = reinterpret_cast<float *>(origins.Mapped().data());
        for (size_t i = 0; i < hits.size(); ++i) {
          origin[i * 4] = hits[i].origin[0];
          origin[i * 4 + 1] = hits[i].origin[1];
          origin[i * 4 + 2] = 0;
        }
        origins.Flush(0, origins.Size());
        const struct {
          vk::DeviceAddress context, origins, output;
          u32 count, unused;
        } push{mirror.Prepare(tlas), origins.BufferDeviceAddress(), output.BufferDeviceAddress(),
               static_cast<u32>(hits.size()), 0};
        const auto command = scheduler.Current().Handle();
        command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push), &push);
        command.dispatch(1, 1, 1);
        const vk::MemoryBarrier barrier{.srcAccessMask = vk::AccessFlagBits::eShaderWrite,
                                        .dstAccessMask = vk::AccessFlagBits::eHostRead};
        command.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                                vk::PipelineStageFlagBits::eHost, {}, 1, &barrier, 0, nullptr, 0,
                                nullptr);
        scheduler.Finish();
        // Tags the pages the job found unsynchronized for the next call.
        cache.ProcessFaultBuffer();
        scheduler.Finish();
        output.Invalidate(0, output.Size());
        std::vector<u32> result(2 + 6 * hits.size());
        std::memcpy(result.data(), output.Mapped().data(), result.size() * sizeof(u32));
        return result;
      };
      const auto expect = [&](const char *step, const std::vector<u32> &result,
                              std::span<const Hit> hits) {
        Require(name, step, result[0] == 1 && result[1] == 4, "the mirror is not valid");
        const auto close_to = [](u32 value, float wanted) {
          return std::abs(std::bit_cast<float>(value) - wanted) < 1e-4f;
        };
        for (size_t i = 0; i < hits.size(); ++i) {
          const u32 *hit = result.data() + 2 + 6 * i;
          const auto &wanted = hits[i];
          const bool ok = wanted.record == Miss
                              ? hit[0] == 0
                              : hit[0] == 1 && hit[4] == wanted.index && hit[5] == wanted.record &&
                                    close_to(hit[1], wanted.t) && close_to(hit[2], wanted.u) &&
                                    close_to(hit[3], wanted.v);
          if (!ok) {
            std::printf("[gpu]     %s ray %zu: hit %u t %f u %f v %f index %u "
                        "record %u\n",
                        step, i, hit[0], std::bit_cast<float>(hit[1]), std::bit_cast<float>(hit[2]),
                        std::bit_cast<float>(hit[3]), hit[4], hit[5]);
          }
          Require(name, step, ok, "a ray hit the wrong triangle, instance or barycentrics");
        }
      };
      // The call after a guest change finds it; a few calls later the mirror
      // holds it.
      const auto settle = [&](const char *step, std::span<const Hit> hits) {
        auto result = trace(hits);
        Require(name, step, result[0] == 0, "the mirror stayed valid over a changed guest BVH");
        for (u32 call = 0; call < 8 && result[0] == 0; ++call) {
          result = trace(hits);
        }
        expect(step, result, hits);
        expect(step, trace(hits), hits);
      };

      const std::array<Hit, 5> built{{
          {{0.2f, 0.3f}, 96, 0, 5.0f, 0.3f, 0.5f},
          {{0.9f, 0.6f}, 97, 0, 5.0f, 0.4f, 0.5f},
          {{19.0f, 2.5f}, 98, 1, 6.0f, 0.625f, 0.25f},
          {{16.4f, 1.0f}, 99, 1, 6.0f, 0.7f, 0.2f},
          {{4.5f, 2.5f}}, // Only the instance with an empty mask covers it.
      }};
      settle("build", built);

      // A rebuild that keeps the records: A takes records 98, 99 and B 96, 97.
      cache.InvalidateMemory(blas, page);
      children(6, {link(98, 0), link(99, 0), Empty, Empty});
      children(8, {link(96, 0), Empty, Empty, Empty});
      std::array<Hit, 6> relinked{{
          {{4.5f, 2.5f}, 98, 0, 5.0f, 0.625f, 0.25f},
          {{3.2f, 1.0f}, 99, 0, 5.0f, 0.7f, 0.2f},
          {{10.4f, 0.3f}, 96, 1, 6.0f, 0.3f, 0.5f},
          {{11.8f, 0.6f}, 97, 1, 6.0f, 0.4f, 0.5f},
          {{0.2f, 0.3f}},
          {{19.0f, 2.5f}},
      }};
      settle("relink", relinked);

      // A refit raises vertex 0 of unit 10 to z = 7; the mirror then updates on
      // every call.
      cache.InvalidateMemory(blas, page);
      node(10)[2] = bits(7.0f);
      relinked[2].t = 7.0f;
      settle("refit", relinked);

      scheduler.Finish();
      m_device.destroyPipeline(pipeline, nullptr);
      m_device.destroyPipelineLayout(layout, nullptr);
    }
    context.UnmapMemory(blas, 2 * page);
    scheduler.Finish();
    bool released = LibKernel::Memory::KernelMunmap(blas, 2 * page) == 0;
    if (released) {
      released = LibKernel::Memory::KernelReleaseDirectMemory(direct, 2 * page) == 0;
    }
    Require(name, "release", released, "guest allocation release failed");
    std::printf("[gpu]     %-32s ok\n", name);
  }
