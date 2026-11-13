  // Writes a guest BVH of header format `format` in 64-byte node units at `words`, header first.
  struct GuestBvh {
    static constexpr u32 Empty = ~0u;
    u32 *words;
    u32 format = 1;

    static u32 Link(u32 index, u32 kind) { return index << 3 | kind; }
    u32 *Node(u32 unit) const { return words + unit * 16; }
    void Header(u32 properties, u32 size) const {
      words[0] = 0x5f525350u; // "PSR_BVHL"
      words[1] = 0x4c485642u;
      words[0x10 / 4] = size;
      // Format 10 builders also write the base layout (0x20000) and a large build (0x2000000).
      words[0x18 / 4] = properties | (format == 10 ? 0x2020000u : 0u);
      words[0xfc / 4] = format;
    }
    // A BLAS's node units and records [begin, end), 8 bytes higher in format 10.
    void Records(u32 nodes, u32 begin, u32 end) const {
      u32 *at = words + (format == 10 ? 2 : 0);
      at[0x50 / 4] = nodes;
      at[0x58 / 4] = end;
      at[0x84 / 4] = begin;
    }
    // A TLAS's instance offset, 8 bytes higher in format 10, and count, at +0x74 in format 3.
    void Instances(u32 offset, u32 count) const {
      words[(format == 10 ? 0x30 : 0x28) / 4] = offset;
      words[(format == 3 ? 0x74 : 0x70) / 4] = count;
    }
    void Children(u32 unit, std::array<u32, 4> links) const {
      std::ranges::copy(links, Node(unit));
    }
    // Child bounds {min xyz, max xyz} of the FP32 box at `unit`.
    void Bounds(u32 unit, u32 child, std::array<float, 6> bounds) const {
      for (u32 i = 0; i < bounds.size(); ++i) {
        Node(unit)[4 + child * 6 + i] = std::bit_cast<u32>(bounds[i]);
      }
    }
    // Five vertices (x, y) at z = 5; word 15 holds each triangle's remap byte.
    void Fan(u32 unit, std::array<float, 10> xy, u32 remap) const {
      for (u32 vertex = 0; vertex < 5; ++vertex) {
        Node(unit)[vertex * 3] = std::bit_cast<u32>(xy[vertex * 2]);
        Node(unit)[vertex * 3 + 1] = std::bit_cast<u32>(xy[vertex * 2 + 1]);
        Node(unit)[vertex * 3 + 2] = std::bit_cast<u32>(5.0f);
      }
      Node(unit)[15] = remap;
    }
    void Record(u32 index, u32 unit, u32 triangle, bool last) const {
      words[index * 2] = Link(unit, triangle);
      words[index * 2 + 1] = index | (last ? 1u << 31 : 0u);
    }
    // An instance record at byte `offset`. Flags: mask in bits 8..15, bit 0 opaque, bit 1
    // non-opaque; object-to-world columns. Format 10 holds the BLAS >> 3 at +48 under the low
    // flags at bit 54, the mask in bits 24..31 and rows.
    u32 *Instance(u32 offset, uint64_t blas, u32 root, u32 flags,
                  std::array<float, 12> columns) const {
      u32 *at = words + offset / 4;
      const bool v10 = format == 10;
      const uint64_t encoded = v10 ? blas >> 3 | uint64_t{flags & 0xffu} << 54 : blas;
      at[v10 ? 12 : 0] = static_cast<u32>(encoded);
      at[v10 ? 13 : 1] = static_cast<u32>(encoded >> 32);
      at[56 / 4] = root;
      at[60 / 4] = v10 ? (flags >> 8 & 0xffu) << 24 : flags;
      for (u32 i = 0; i < columns.size(); ++i) {
        at[64 / 4 + (v10 ? i % 3 * 4 + i / 3 : i)] = std::bit_cast<u32>(columns[i]);
      }
      return at;
    }
  };

  // A BLAS of two fan nodes under a root box (unit 4) over boxes A (unit 6: records 96, 97) and
  // B (unit 8: records 98, 99).
  static void WriteFanBlas(const GuestBvh &bvh) {
    using G = GuestBvh;
    bvh.Header(0x40, 100 * 8);
    bvh.Records(12, 96, 100);
    bvh.Children(4, {G::Link(6, 5), G::Link(8, 5), G::Empty, G::Empty});
    bvh.Bounds(4, 0, {0, 0, 4.5f, 1, 1, 5.5f});
    bvh.Bounds(4, 1, {3, 0, 4.5f, 5, 4, 5.5f});
    bvh.Children(6, {G::Link(96, 0), G::Empty, G::Empty, G::Empty});
    bvh.Bounds(6, 0, {0, 0, 4.5f, 1, 1, 5.5f});
    bvh.Children(8, {G::Link(98, 0), G::Link(99, 0), G::Empty, G::Empty});
    bvh.Bounds(8, 0, {3, 2, 4.5f, 5, 4, 5.5f});
    bvh.Bounds(8, 1, {3, 0, 4.5f, 4, 4, 5.5f});
    // The guest's u and v weigh triangle vertices (2, 0), (0, 1), (1, 2) and (2, 1).
    bvh.Fan(10, {0, 0, 1, 0, 0, 1, 1, 1, 9, 9}, 0x02 | 0x04 << 8);
    bvh.Fan(11, {3, 0, 9, 9, 3, 2, 5, 2, 4, 4}, 0x09 << 16 | 0x06 << 24);
    bvh.Record(96, 10, 0, false);
    bvh.Record(97, 10, 1, true);
    bvh.Record(98, 11, 2, true);
    bvh.Record(99, 11, 3, true);
  }

  void CheckBvhMirror() {
    for (const u32 format : {1u, 3u, 10u}) {
      CheckBvhMirror(format);
    }
  }

  // The fan BLAS and a TLAS naming A, B scaled and moved, the whole BLAS with an empty mask, and
  // an empty root, in a header format. Rays along +z must hit the mirror as the guest's own walk
  // would, after a relink and after a refit too.
  void CheckBvhMirror(u32 format) {
    using G = GuestBvh;
    const auto label = "BvhMirror format " + std::to_string(format);
    const char *name = label.c_str();
    constexpr auto page = BufferCache::CACHING_PAGESIZE;
    constexpr u32 Miss = ~0u;
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
    const G bottom{words, format};
    WriteFanBlas(bottom);
    const G top{words + page / sizeof(u32), format};
    top.Header(0x2c, 0x100 + 4 * 128);
    top.Instances(0x100, 4);
    constexpr std::array<float, 12> identity{1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    top.Instance(0x100, blas, G::Link(6, 5), 0xff01, identity);
    top.Instance(0x180, blas, G::Link(8, 5), 0xff02, {2, 0, 0, 0, 1, 0, 0, 0, 1, 10, 0, 1});
    top.Instance(0x200, blas, G::Link(4, 5), 0x0001, identity);
    top.Instance(0x280, blas, G::Empty, 0xff01, identity);

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
      // Mirrors the TLAS and traces each origin; returns {valid, instances, hit words...}.
      const auto trace = [&](std::span<const Hit> hits) {
        auto *origin = reinterpret_cast<float *>(origins.Mapped().data());
        for (size_t i = 0; i < hits.size(); ++i) {
          origin[i * 4] = hits[i].origin[0];
          origin[i * 4 + 1] = hits[i].origin[1];
          origin[i * 4 + 2] = 0;
        }
        origins.Flush(0, origins.Size());
        context.PrepareBda();
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
      // The call after a guest change finds it; a few calls later the mirror holds it.
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
      bottom.Children(6, {G::Link(98, 0), G::Link(99, 0), G::Empty, G::Empty});
      bottom.Children(8, {G::Link(96, 0), G::Empty, G::Empty, G::Empty});
      std::array<Hit, 6> relinked{{
          {{4.5f, 2.5f}, 98, 0, 5.0f, 0.625f, 0.25f},
          {{3.2f, 1.0f}, 99, 0, 5.0f, 0.7f, 0.2f},
          {{10.4f, 0.3f}, 96, 1, 6.0f, 0.3f, 0.5f},
          {{11.8f, 0.6f}, 97, 1, 6.0f, 0.4f, 0.5f},
          {{0.2f, 0.3f}},
          {{19.0f, 2.5f}},
      }};
      settle("relink", relinked);

      // A refit raises vertex 0 of unit 10 to z = 7; the mirror then updates on every call.
      cache.InvalidateMemory(blas, page);
      bottom.Node(10)[2] = std::bit_cast<u32>(7.0f);
      relinked[2].t = 7.0f;
      settle("refit", relinked);

      // A format 10 TLAS in the Trinity layout is not mirrored, even once its page is synchronized.
      if (format == 10) {
        cache.InvalidateMemory(tlas, page);
        top.words[0x18 / 4] |= 0x40000u;
        trace(relinked);
        trace(relinked);
        const auto result = trace(relinked);
        Require(name, "layout", result[0] == 0 && result[1] == ~0u,
                "a TLAS in another node layout was mirrored");
      }

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
