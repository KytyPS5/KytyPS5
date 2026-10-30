TestCase TrinityBvhIntersections(bool compressed, bool bary, bool sorted,
                                 bool wide_sort, u32 heuristic = 0, bool triangles_first = false,
                                 u32 wave_size = 32, bool edges = false) {
  TestCase test;
  test.name = compressed ? (sorted ? "TrinityCompressedSorted" : "TrinityCompressedUnsorted")
                         : (sorted ? "TrinityFp32Sorted" : "TrinityFp32Unsorted");
  if (!bary) test.name = "TrinityTriangleIds";
  if (heuristic == 1) test.name = "TrinityLargestInterval";
  if (heuristic == 2) test.name = "TrinityClosestMidpoint";
  if (triangles_first) test.name = "TrinityTrianglesFirst";
  if (wave_size == 64) test.name = "TrinityBvhWave64";
  if (edges) test.name = compressed ? "TrinityCompressedEdges" : "TrinityFp32Edges";
  constexpr u32 NodeStart = 4096, OutputStart = 2048, GuestBase = 0x10000;
  constexpr u32 Invalid = ~0u, Infinity = 0x7f800000, Flag = 0x8484, Shape = 0x12345678;
  const auto bits = [](float x) { return std::bit_cast<u32>(x); };
  test.initial.resize(NodeStart + 256 * 16);
  auto& data = test.initial;
  const std::array<float, 12> vertices {-1,-1,2, 1,-1,2, 0,1,2, 2,1,2};
  for (u32 i = 0; i < vertices.size(); ++i) data[NodeStart + i] = bits(vertices[i]);
  data[NodeStart + 14] = Shape;
  data[NodeStart + 15] = Flag;
  const std::array<float, 12> matrix {2,0,0,1, 0,3,0,-2, 0,0,4,3};
  for (u32 i = 0; i < matrix.size(); ++i) data[NodeStart + 16 + i] = bits(matrix[i]);
  data[NodeStart + 28] = 0x1000;
  data[NodeStart + 29] = 9u << 22; // Force opaque and flip facing.
  data[NodeStart + 30] = 0x98;
  data[NodeStart + 31] = 0x04123456;
  const std::array<float, 8> nearDist {5,1,3,7,4,2,6,8};
  const std::array<float, 8> length {1,8,3,5,7,2,4,6};
  const std::array<u32, 8> children {128,138,171,190,256,266,299,318};
  const auto set_bits = [&](u32 base, u32 offset, u32 count, u32 value) {
    for (u32 bit = 0; bit < count; ++bit)
      data[base + (offset + bit) / 32] |= ((value >> bit) & 1) << ((offset + bit) % 32);
  };
  for (u32 slot = 0; slot < 2; ++slot) {
    const u32 base = NodeStart + (3 + slot * (compressed ? 1 : 2)) * 16;
    if (compressed) {
      set_bits(base, 0, 29, slot ? 32 : 16);
      for (u32 i = 0; i < 4; ++i) set_bits(base, 29 + i * 3, 3, children[slot * 4 + i] & 7);
      set_bits(base, 56, 8, 127); set_bits(base, 64, 8, 127); set_bits(base, 72, 8, 130);
    }
    for (u32 i = 0; i < 4; ++i) {
      const u32 index = slot * 4 + i;
      if (compressed) {
        // Exact integer minima; positive maxima round outward within one quantization step.
        const std::array<u32, 6> planes {0x30000, 0x30000, u32(nearDist[index]) * 8192,
            0x10000, 0x10000, u32(nearDist[index] + length[index]) * 8192};
        for (u32 axis = 0; axis < 6; ++axis) set_bits(base, 80 + i * 108 + axis * 18, 18, planes[axis]);
      } else {
        data[base + i] = children[index];
        const std::array<float, 6> bounds {-1,-1,nearDist[index], 1,1,nearDist[index] + length[index]};
        for (u32 axis = 0; axis < 6; ++axis) data[base + 4 + i * 6 + axis] = bits(bounds[axis]);
      }
    }
  }
  if (compressed) {
    std::copy_n(data.begin() + NodeStart + 3 * 16, 16, data.begin() + NodeStart + 10 * 16);
  } else {
    constexpr std::array<u32, 4> lo {0x4500, 0x3c00, 0x4200, 0x4700};
    constexpr std::array<u32, 4> hi {0x4600, 0x4880, 0x4600, 0x4a00};
    for (u32 i = 0; i < 4; ++i) {
      data[NodeStart + 160 + i] = children[i];
      data[NodeStart + 164 + i * 3] = 0xbc00bc00;
      data[NodeStart + 165 + i * 3] = 0x3c000000 | lo[i];
      data[NodeStart + 166 + i * 3] = (hi[i] << 16) | 0x3c00;
    }
  }
  if (edges) {
    // Slot zero ends exactly at the descriptor limit; slot one lies beyond it.
    std::copy_n(data.begin() + NodeStart + 3 * 16, compressed ? 16 : 32,
                data.begin() + NodeStart + (compressed ? 253 : 252) * 16);
    for (u32 index : {12u, 13u})
      std::copy_n(data.begin() + NodeStart, 16, data.begin() + NodeStart + index * 16);
    data[NodeStart + 12 * 16 + 15] = 0xc4c4; // Opaque procedural pair.
    data[NodeStart + 13 * 16 + 15] = 0x9494; // Opaque, double-sided pair.
    if (compressed) {
      std::copy_n(data.begin() + NodeStart + 3 * 16, 16, data.begin() + NodeStart + 11 * 16);
      set_bits(NodeStart + 11 * 16, 41, 4, 1);
      set_bits(NodeStart + 11 * 16, 45, 4, 2);
      set_bits(NodeStart + 11 * 16, 49, 4, 3);
    }
  }
  std::vector<std::array<u32, 16>> expected(wave_size);
  for (u32 lane = 0; lane < wave_size; ++lane) {
    const u32 which = lane % 16, input = lane * 16;
    auto& out = expected[lane];
    out.fill(Invalid);
    data[input + 2] = bits(20); data[input + 3] = 4; data[input + 9] = bits(1);
    const auto miss = [&](u32 slot, bool invalid = false) {
      out[slot * 4] = Infinity; out[slot * 4 + 1] = bits(1);
      out[slot * 4 + 2] = !invalid && bary ? 0 : Invalid; out[slot * 4 + 3] = 0;
    };
    const auto hit = [&](u32 slot, float u, float v) {
      out[slot * 4] = bits(-8); out[slot * 4 + 1] = bits(-4);
      out[slot * 4 + 2] = bary ? bits(u) : Flag + slot;
      out[slot * 4 + 3] = bary ? bits(v) : 1;
    };
    if (which <= 2 || which >= 9 && which <= 14) {
      data[input + 10] = which == 1 || which == 2 ? 1 : 0;
      miss(0); miss(1);
      out[8] = Shape;
      if (which == 1) {
        data[input + 4] = bits(1); hit(1, 2, 1); out[9] = Shape;
      } else {
        hit(0, 1, 1);
        if (which == 2) { miss(1); out[9] = Shape; }
      }
      if (which == 9 || which == 10 || which == 11) {
        if (which == 9) data[input + 4] = 0x7fc00000;
        if (which == 10) data[input + 10] = 0xfff8;
        if (which == 11) data[input + 1] = 0x41u << 22;
        miss(0, true); miss(1, true); out[8] = out[9] = Invalid;
      } else if (which == 12) {
        data[input + 1] = 0x100u << 22; miss(0);
      } else if (which == 13) {
        data[input + 1] = 8u << 22; out[0] = bits(8); out[1] = bits(4);
      } else if (which == 14) {
        data[input + 1] = 2u << 22;
        if (bary) { out[2] = bits(-1); out[3] = bits(-1); }
      }
    } else if (which >= 3 && which <= 5) {
      data[input + 10] = which == 3 ? (3 * 8 + (compressed ? 3 : 2))
                           : which == 4 ? (3 * 8 + (compressed ? 4 : 5)) : (10 * 8 + 4);
      std::array<u32, 8> order {0,1,2,3,4,5,6,7};
      const auto compare = [&](u32 x, u32 y) {
        const bool xt = (children[x] & 7) < 2, yt = (children[y] & 7) < 2;
        if (triangles_first && xt != yt) return xt;
        if (heuristic == 1) return length[x] > length[y];
        if (heuristic == 2) return 2 * nearDist[x] + length[x] < 2 * nearDist[y] + length[y];
        return nearDist[x] < nearDist[y];
      };
      if (sorted) {
        if (wide_sort && which == 3) std::stable_sort(order.begin(), order.end(), compare);
        else {
          std::stable_sort(order.begin(), order.begin() + 4, compare);
          std::stable_sort(order.begin() + 4, order.end(), compare);
        }
      }
      for (u32 i = 0; i < (which == 3 ? 8u : 4u); ++i) out[i] = children[order[i]];
    } else if (which == 6 || which == 7 || which == 15) {
      data[input + 10] = 14;
      if (which == 15) { data[input] = 8; data[input + 10] = 6; }
      data[input + 4] = bits(1); data[input + 5] = bits(2); data[input + 6] = bits(3);
      if (which == 7) data[input + 3] = 2;
      for (u32 i = 0; i < 10; ++i) out[i] = 0xabc00000 + i;
      out[2] = 0x1000; out[3] = 9u << 22; out[6] = 0x123456; out[7] = which == 7 ? Invalid : 0x98;
    } else data[input + 10] = 7;
    if (edges) {
      switch (which) {
        case 0:
        case 1:
        case 2:
        case 5:
          if (which == 0) data[input] = 1; // Misaligned encoded base must not read across a block.
          if (which == 1) data[input + 4] = Infinity;
          if (which == 2) data[input + 2] = 0x7fc00000;
          if (which == 5) { data[input + 1] = 0x2000; data[input + 10] = 0; }
          miss(0, true); miss(1, true); out[8] = out[9] = Invalid;
          break;
        case 3:
          data[input + 10] = compressed ? 253 * 8 + 3 : 252 * 8 + 2;
          for (u32 i = 4; i < 8; ++i) out[i] = Invalid;
          break;
        case 4:
          if (compressed) {
            data[input + 10] = 11 * 8 + 4; data[input + 1] = 0x40u << 22;
            out[0] = out[2] = Invalid;
          } else {
            data[input + 2] = bits(1);
            for (u32 i = 0; i < 4; ++i) out[i] = Invalid;
          }
          break;
        case 6:
          data[input + 1] = 8u << 22; out[3] = 1u << 22;
          break;
        case 7:
          data[input + 1] = 2u << 22; out[3] = 10u << 22;
          break;
        case 9:
        case 10:
          data[input + 10] = 12 * 8;
          data[input + 4] = bits(100);
          out[8] = Shape; miss(0); miss(1);
          if (which == 9) { out[0] = bits(0); out[1] = bits(1); out[2] = bits(-1); out[3] = bits(1); }
          else data[input + 1] = 0x200u << 22;
          break;
        case 11:
          data[input + 1] = 0x10u << 22; miss(0); miss(1); out[8] = Shape;
          break;
        case 12:
        case 13:
          data[input + 1] = (which == 12 ? 0x14u : 0x10u) << 22;
          if (which == 13) data[input + 10] = 13 * 8;
          hit(0, 1, 1);
          break;
        case 14:
          // The compressed +X bound rounds outward beyond exactly one.
          if (compressed) {
            data[input + 1] = 0; data[input + 4] = bits(1.0f + 0x1p-17f);
            data[input + 10] = 3 * 8 + 4;
            out.fill(Invalid);
            for (u32 i = 0; i < 4; ++i) out[i] = children[i];
          }
          break;
      }
    }
    for (u32 i = 0; i < 6; ++i) out[10 + i] = data[input + 4 + i];
    if (which == 6 || which == 7 || which == 15) {
      out[10] = bits(3); out[11] = bits(4); out[12] = bits(15); out[15] = bits(4);
    }
    if (lane % 32 >= 16) {
      for (u32 i = 0; i < 10; ++i) out[i] = 0xabc00000 + i;
      for (u32 i = 0; i < 6; ++i) out[10 + i] = data[input + 4 + i];
    }
  }
  test.expected = data;
  for (u32 lane = 0; lane < wave_size; ++lane)
    std::copy(expected[lane].begin(), expected[lane].end(), test.expected.begin() + OutputStart + lane * 16);
  auto& code = test.code;
  AppendSMovLiteral(&code, 8, GuestBase >> 8);
  AppendSMovLiteral(&code, 9, (sorted ? 1u << 31 : 0) | (triangles_first ? 1u << 20 : 0) | (heuristic << 21));
  AppendSMovLiteral(&code, 10, edges ? 253 : 255);
  AppendSMovLiteral(&code, 11, 0x80c00000 | (bary ? 1u << 24 : 0) |
      (compressed ? 1u << 20 : 0) | (wide_sort ? 1u << 21 : 0));
  code.push_back(EncodeVop2(0x1a, 1, InlineU32(6), 0));
  for (u32 offset : {0u,4u,8u})
    code.insert(code.end(), {EncodeMubuf0(offset == 8 ? 0x0f : 0x0e, offset * 4), EncodeMubuf1(4 + offset, 0, 1)});
  for (u32 i = 0; i < 10; ++i) AppendVMovLiteral(&code, 32 + i, 0xabc00000 + i);
  AppendSMovLiteral(&code, 126, 0x0000ffff);
  if (wave_size == 64) AppendSMovLiteral(&code, 127, 0x0000ffff);
  code.insert(code.end(), {EncodeMimg0(0xe5, 15, 2, false, 0, true), EncodeMimg1(32, 4, 2),
      0x08070605, 0x00000e0b});
  code.push_back(EncodeSop1(0x04, 126, 193));
  code.push_back(EncodeVop2(0x1a, 2, InlineU32(4), 0));
  for (u32 i = 0; i < 10; ++i) AppendStoreVgprAtLaneDwordOffset(&code, 32 + i, 2, OutputStart + i);
  for (u32 i = 0; i < 6; ++i) AppendStoreVgprAtLaneDwordOffset(&code, 8 + i, 2, OutputStart + 10 + i);
  AppendEnd(&code);
  test.bda_mappings = {{GuestBase, NodeStart * sizeof(u32)}};
  test.compute_info.wave_size = wave_size;
  test.compute_info.threads_num[0] = wave_size;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  test.decoded_counts = {{"IMAGE_BVH8_INTERSECT_RAY", 1}};
  test.required_spirv = {"trinity_bvh_intersect"};
  return test;
}

TestCase TrinityBvhAliases() {
  auto test = TrinityBvhIntersections(false, true, false, false);
  test.name = "TrinityBvhRegisterAliases";
  constexpr u32 OutputStart = 2048;
  for (u32 lane = 0; lane < 32; ++lane) {
    auto* out = test.expected.data() + OutputStart + lane * 16;
    const auto* input = test.initial.data() + lane * 16;
    const bool active = lane < 16, instance = active && (lane == 6 || lane == 7 || lane == 15);
    if (!active) {
      for (u32 i = 0; i < 6; ++i) out[4 + i] = input[i];
    } else if (instance) {
      out[4] = input[0]; out[5] = input[1];
      out[8] = out[10]; out[9] = out[11];
    } else {
      // A non-instance writes its shape IDs into registers formerly holding ray XY.
      out[10] = out[8]; out[11] = out[9];
    }
  }
  auto& code = test.code;
  code.clear();
  AppendSMovLiteral(&code, 8, 0x100);
  AppendSMovLiteral(&code, 9, 0);
  AppendSMovLiteral(&code, 10, 255);
  AppendSMovLiteral(&code, 11, 0x81c00000);
  code.push_back(EncodeVop2(0x1a, 50, InlineU32(6), 0));
  code.push_back(EncodeVop2(0x1a, 51, InlineU32(4), 0));
  for (u32 i = 0; i < 10; ++i) AppendVMovLiteral(&code, i, 0xabc00000 + i);
  for (u32 offset : {0u, 4u, 8u})
    code.insert(code.end(), {EncodeMubuf0(offset == 8 ? 0x0f : 0x0e, offset * 4),
        EncodeMubuf1(4 + offset, 0, 50)});
  AppendSMovLiteral(&code, 126, 0x0000ffff);
  code.insert(code.end(), {EncodeMimg0(0xe5, 15, 2, false, 0, true), EncodeMimg1(0, 4, 2),
      0x08070605, 0x00000e0b});
  code.push_back(EncodeSop1(0x04, 126, 193));
  for (u32 i = 0; i < 10; ++i) AppendStoreVgprAtLaneDwordOffset(&code, i, 51, OutputStart + i);
  for (u32 i = 0; i < 6; ++i) AppendStoreVgprAtLaneDwordOffset(&code, 8 + i, 51, OutputStart + 10 + i);
  AppendEnd(&code);
  return test;
}

std::vector<TestCase> TrinityBvhCases() {
  return {TrinityBvhIntersections(false, true, false, false),
          TrinityBvhIntersections(false, true, true, true),
          TrinityBvhIntersections(true, true, false, false),
          TrinityBvhIntersections(true, true, true, false),
          TrinityBvhIntersections(false, false, true, true),
          TrinityBvhIntersections(false, true, true, true, 1),
          TrinityBvhIntersections(false, true, true, true, 2),
          TrinityBvhIntersections(true, true, true, true, 0, true),
          TrinityBvhIntersections(true, true, true, true, 0, false, 64),
          TrinityBvhIntersections(false, true, false, false, 0, false, 32, true),
          TrinityBvhIntersections(true, true, false, false, 0, false, 32, true),
          TrinityBvhAliases()};
}

TestCase TrinityStack(u32 wave_size, u32 capacity, bool storage = false) {
  TestCase test;
  static constexpr const char* names[2][3] = {
      {"TrinityStack32Capacity8", "TrinityStack32Capacity16", "TrinityStack32Capacity31"},
      {"TrinityStack64Capacity8", "TrinityStack64Capacity16", "TrinityStack64Capacity31"}};
  test.name = storage ? (wave_size == 32 ? "TrinityStackStorage32" : "TrinityStackStorage64")
                      : names[wave_size == 64][capacity == 8 ? 0 : capacity == 16 ? 1 : 2];
  constexpr u32 Invalid = 0xffffffffu, Terminal = 0xfffffffeu;
  constexpr u32 Parent = 1u << 29, Overflow = 1u << 30, Return = 1u << 31;
  const auto state = [](u32 count, u32 parent, u32 ring, u32 flags = 0) {
    return count | (parent << 5) | (ring << 10) | flags;
  };
  test.initial.resize(wave_size * 32);
  struct Output { u32 control, node; std::array<u32, 8> lds; };
  std::vector<Output> outputs(wave_size);
  for (u32 lane = 0; lane < wave_size; ++lane) {
    const u32 which = lane % 16;
    const u32 base = which == 15 ? 0x3fff : (lane / 32) * capacity * 32 + lane % 32;
    auto* input = test.initial.data() + lane * 32;
    auto& out = outputs[lane];
    input[0] = base << 15;
    input[1] = Invalid;
    std::fill(input + 2, input + 10, Invalid);
    input[10] = base * 4;
    for (u32 i = 0; i < 8; ++i) out.lds[i] = 100 + i * 8;
    if (capacity == 8) out.lds[7] = 88;
    out.node = Terminal;
    out.control = 0;
    const auto children = [&] {
      for (u32 i = 0; i < 8; ++i) input[2 + i] = (i + 1) * 8;
    };
    switch (which) {
      case 0: break;
      case 1: input[0] |= Overflow; out.control = Overflow; out.node = Invalid; break;
      case 2:
        children(); out.control = state(7, 7, 7); out.node = 8;
        out.lds = {64, 56, 48, 40, 32, 24, 16, 8}; break;
      case 3:
        input[2] = 8; input[4] = 24; input[7] = 48; input[9] = 64;
        out.control = state(3, 3, 3); out.node = 8;
        out.lds[0] = 64; out.lds[1] = 48; out.lds[2] = 24; out.lds[3] = 8; break;
      case 4:
      case 14:
        input[0] |= state(capacity, capacity, 0, which == 14 ? Parent : 0);
        input[2] = 8; out.control = state(capacity - 1, capacity - 1, 0, Overflow);
        out.node = out.lds[0] = 8; break;
      case 5:
        input[0] |= state(1, 1, 0); out.control = state(0, 0, capacity - 1); out.node = 88; break;
      case 6:
        input[0] |= state(1, 0, 1, Parent);
        out.control = state(0, 31, 0, Return); out.node = 100; break;
      case 7:
        input[2] = 14; out.control = Parent; out.node = out.lds[0] = 14; break;
      case 8:
        input[0] |= state(3, 1, 3, Parent);
        out.lds[2] = 14;
        out.control = state(0, 0, 2, Parent | Overflow); out.node = 14; break;
      case 9:
        children(); input[1] = 24; out.control = state(4, 4, 4); out.node = 32;
        out.lds[0] = 64; out.lds[1] = 56; out.lds[2] = 48; out.lds[3] = 40; out.lds[4] = 32; break;
      case 10:
        children(); input[1] = 96; input[0] |= state(1, 1, 1);
        out.node = 100; break;
      case 11:
        children(); input[9] = 0xfffffffbu; out.control = state(3, 3, 3); out.node = 8;
        out.lds[0] = 32; out.lds[1] = 24; out.lds[2] = 16; out.lds[3] = 8; break;
      case 12: children(); input[9] = 0xfffffff9u; break;
      case 13:
        children(); input[5] = 0xfffffffdu; out.control = state(3, 3, 3); out.node = 40;
        out.lds[0] = 64; out.lds[1] = 56; out.lds[2] = 48; out.lds[3] = 40; break;
      case 15:
        input[0] |= state(1, 1, 1); out.node = 0; out.lds.fill(0); break;
    }
    out.control |= base << 15;
    if ((lane % 32) >= 16) {
      out.control = input[0]; out.node = input[2];
      for (u32 i = 0; i < 8; ++i) out.lds[i] = 100 + i * 8;
      if (capacity == 8) out.lds[7] = 88;
      if (which == 8) out.lds[2] = 14;
      if (which == 15) out.lds.fill(0);
    }
  }
  test.expected = test.initial;
  for (u32 lane = 0; lane < wave_size; ++lane) {
    test.expected[lane * 32 + 20] = outputs[lane].control;
    test.expected[lane * 32 + 21] = outputs[lane].node;
    std::copy(outputs[lane].lds.begin(), outputs[lane].lds.end(), test.expected.begin() + lane * 32 + 24);
  }
  auto& code = test.code;
  code.push_back(EncodeVop2(0x1a, 1, InlineU32(7), 0));
  code.insert(code.end(), {EncodeMubuf0(0x0d), EncodeMubuf1(8, 0, 1),
                           EncodeMubuf0(0x0e, 8), EncodeMubuf1(16, 0, 1),
                           EncodeMubuf0(0x0e, 24), EncodeMubuf1(20, 0, 1),
                           EncodeMubuf0(0x0c, 40), EncodeMubuf1(2, 0, 1)});
  for (u32 i = 0; i < 8; ++i) {
    AppendVMovU32(&code, 3, 100 + i * 8);
    code.insert(code.end(), {EncodeDs0(0x0d, i * 128), EncodeDs1(0, 3, 2)});
  }
  AppendVMovU32(&code, 3, 88);
  code.insert(code.end(), {EncodeDs0(0x0d, (capacity - 1) * 128), EncodeDs1(0, 3, 2)});
  // Lane eight starts with a nested instance on top of its stack.
  AppendSMovLiteral(&code, 126, 0x01000100u);
  if (wave_size == 64) AppendSMovLiteral(&code, 127, 0x01000100u);
  AppendVMovU32(&code, 3, 14);
  code.insert(code.end(), {EncodeDs0(0x0d, 256), EncodeDs1(0, 3, 2)});
  AppendSMovLiteral(&code, 126, 0x0000ffffu);
  if (wave_size == 64) AppendSMovLiteral(&code, 127, 0x0000ffffu);
  code.insert(code.end(), {EncodeDs0(0xe1, capacity), EncodeDs1Ex(16, 16, 9, 8)});
  code.push_back(EncodeSop1(0x04, 126, 193));
  code.push_back(EncodeVop2(0x1a, 3, InlineU32(5), 0));
  AppendStoreVgprAtLaneDwordOffset(&code, 8, 3, 20);
  AppendStoreVgprAtLaneDwordOffset(&code, 16, 3, 21);
  for (u32 i = 0; i < 8; ++i) {
    code.insert(code.end(), {EncodeDs0(0x36, i * 128), EncodeDs1(24, 0, 2)});
    AppendStoreVgprAtLaneDwordOffset(&code, 24, 3, 24 + i);
  }
  AppendEnd(&code);
  test.compute_info.wave_size = wave_size;
  test.compute_info.threads_num[0] = wave_size;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.lds_size_dwords = wave_size * capacity;
  test.compute_info.lds_storage = storage;
  test.has_compute_info = true;
  test.decoded_counts = {{"DS_BVH_STACK_PUSH8_POP1_RTN_B32", 1}};
  test.forbidden_spirv = {"OpControlBarrier", "OpMemoryBarrier", "get_bda_pointer"};
  return test;
}

TestCase FlatStackApertures(u32 wave_size) {
  TestCase test;
  test.name = wave_size == 32 ? "FlatStackApertures32" : "FlatStackApertures64";
  auto &code = test.code;
  code = {EncodeSop1(0x04, 8, 237), // private base
          EncodeSop1(0x0b, 11, InlineU32(1)), // S_BREV_B32 shared aperture, as in GTA.
          EncodeVop2(0x1a, 20, InlineU32(2), 0),
          EncodeVop2(0x1b, 22, InlineU32(1), 0),
          EncodeVop2(0x1a, 22, InlineU32(31), 22)};
  AppendVop3(&code, 0x159, 21, 9, 11, Vgpr(22)); // Clamp the stack address to the local apertures.
  AppendVMovU32(&code, 30, 100);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeFlat0(0x1c, 1), EncodeFlat1(0, 0x7d, 30, 20)});
  AppendVMovU32(&code, 30, 200);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeDs0(0x0d), EncodeDs1(0, 30, 20)});
  AppendVMovU32(&code, 30, 300);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20)});
  // FLAT writes must alias both explicit memory instructions, per invocation.
  code.insert(code.end(), {EncodeFlat0(0x0c, 1), EncodeFlat1(24, 0x7d, 0, 20),
                           EncodeDs0(0x36), EncodeDs1(25, 0, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 24, 0, 0);
  AppendStoreVgprAtLaneDwordOffset(&code, 25, 0, wave_size);
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 2 * wave_size);
  // Inactive lanes must preserve both stores and destination VGPRs.
  AppendSMovLiteral(&code, 126, 0x55555555);
  if (wave_size == 64) AppendSMovLiteral(&code, 127, 0x55555555);
  code.push_back(EncodeVop2(0x25, 22, InlineU32(1), 22));
  AppendVop3(&code, 0x159, 21, 9, 11, Vgpr(22));
  AppendVMovU32(&code, 30, 400);
  code.push_back(EncodeVop2(0x25, 30, Vgpr(0), 30));
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20)});
  code.insert(code.end(), {EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  code.push_back(EncodeSop1(0x04, 126, 193)); // -1 sign-extends across EXEC
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 3 * wave_size);
  // Noncanonical addresses outside either aperture, aperture overflow, and
  // offsets outside allocated local storage must not wrap into valid storage.
  AppendVMovLiteral(&code, 21, 0x78000000);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 4 * wave_size);
  code.push_back(EncodeVop1(0x01, 21, 11));
  AppendVMovLiteral(&code, 20, 0xfffffffc);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0, 4), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0, 4), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 5 * wave_size);
  AppendVMovU32(&code, 20, wave_size * 4);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 6 * wave_size);
  AppendVMovU32(&code, 20, 1);
  code.insert(code.end(), {EncodeFlat0(0x1c, 0), EncodeFlat1(0, 0x7d, 30, 20),
                           EncodeFlat0(0x0c, 0), EncodeFlat1(26, 0x7d, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 26, 0, 9 * wave_size);
  code.push_back(EncodeVop2(0x1a, 20, InlineU32(2), 0));
  code.insert(code.end(), {EncodeFlat0(0x0c, 1), EncodeFlat1(24, 0x7d, 0, 20),
                           EncodeDs0(0x36), EncodeDs1(25, 0, 20)});
  AppendStoreVgprAtLaneDwordOffset(&code, 24, 0, 7 * wave_size);
  AppendStoreVgprAtLaneDwordOffset(&code, 25, 0, 8 * wave_size);
  AppendEnd(&code);
  test.expected.resize(10 * wave_size);
  for (u32 lane = 0; lane < wave_size; ++lane) {
    const bool shared = lane & 1;
    test.expected[lane] = (shared ? 100 : 300) + lane;
    test.expected[wave_size + lane] = (shared ? 300 : 200) + lane;
    test.expected[2 * wave_size + lane] = 300 + lane;
    test.expected[3 * wave_size + lane] = (shared ? 300 : 400) + lane;
    test.expected[7 * wave_size + lane] = (shared ? 100 : 400) + lane;
    test.expected[8 * wave_size + lane] = (shared ? 300 : 200) + lane;
  }
  test.compute_info.wave_size = wave_size;
  test.compute_info.threads_num[0] = wave_size;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.compute_info.scratch_size_dwords = wave_size;
  test.compute_info.lds_size_dwords = wave_size;
  test.has_compute_info = true;
  test.decoded_counts = {{"S_BREV_B32", 1}};
  test.ir_counts = {{" = BitReverse32 ", 0}};
  test.forbidden_spirv = {"get_bda_pointer"};
  return test;
}

TestCase BvhIntersections(bool barycentrics, bool sorted, u32 grow = 0, bool wide = false) {
  TestCase test;
  test.name = barycentrics ? (sorted ? "BvhBarycentricSorted" : "BvhBarycentricUnsorted")
                          : (sorted ? "BvhTriangleIdSorted" : "BvhTriangleIdUnsorted");
  if (grow != 0) test.name = "BvhBoxExitGrowth";
  if (wide) test.name = "Bvh64HighNodePointers";
  constexpr u32 NodeStart = 1024, OutputStart = 512, LaneCount = 32;
  constexpr uint64_t GuestBase = 0x10000;
  const u32 node_base = wide ? 1u << 29 : 0;
  const auto bits = [](float value) { return std::bit_cast<u32>(value); };
  constexpr u32 Infinity = 0x7f800000, Invalid = UINT32_MAX, Flag = 0x04040404;
  test.initial.resize(NodeStart + 258 * 16);
  auto &data = test.initial;
  // Pack five float3 vertices and the triangle flag into a 64-byte node.
  const std::array<float, 15> vertices{-1, -1, 2, 1, -1, 2, 0, 1, 2, 2, 1, 2, 0, 3, 2};
  for (u32 i = 0; i < vertices.size(); ++i) data[NodeStart + i] = bits(vertices[i]);
  data[NodeStart + 15] = Flag;
  constexpr std::array<u32, 4> children{0x88, 0x90, 0x98, 0xa0};
  constexpr std::array<float, 4> z{5, 1, 3, 7};
  constexpr std::array<u32, 4> half_z{0x4500, 0x3c00, 0x4200, 0x4700};
  constexpr std::array<u32, 4> half_far{0x4600, 0x4000, 0x4400, 0x4800};
  for (u32 child = 0; child < 4; ++child) {
    data[NodeStart + 16 + child] = data[NodeStart + 32 + child] = children[child];
    const auto fp16 = NodeStart + 20 + child * 3;
    data[fp16] = 0xbc00bc00; // min.xy = -1,-1
    data[fp16 + 1] = 0x3c000000 | half_z[child];
    data[fp16 + 2] = (half_far[child] << 16) | 0x3c00;
    const auto fp32 = NodeStart + 36 + child * 6;
    const std::array<float, 6> bounds{-1, -1, z[child], 1, 1, z[child] + 1};
    for (u32 i = 0; i < 6; ++i) data[fp32 + i] = bits(bounds[i]);
  }
  // A grazing ray enters one ULP after exiting on another axis.
  for (u32 child = 0; child < 4; ++child) {
    data[NodeStart + 64 + child] = children[child];
    const std::array<float, 6> bounds = child == 0
        ? std::array<float, 6>{0, 0, std::nextafter(1.0f, 2.0f), 1, 2, 2}
        : child == 1 ? std::array<float, 6>{-1, 1, -1, -0.0f, 2, 2}
                     : std::array<float, 6>{1, 1, 1, -1, -1, -1};
    for (u32 i = 0; i < 6; ++i) data[NodeStart + 68 + child * 6 + i] = bits(bounds[i]);
  }
  // An FP32 node straddles two guest pages with a 64-byte gap in physical backing.
  std::copy_n(data.begin() + NodeStart + 32, 16, data.begin() + NodeStart + 255 * 16);
  std::copy_n(data.begin() + NodeStart + 48, 16, data.begin() + NodeStart + 257 * 16);
  const std::array<std::array<float, 2>, 4> origins{{{0, 0}, {1, 0}, {.5f, 1.5f}, {-.25f, 1}}};
  std::array<std::array<u32, 4>, LaneCount> expected{};
  for (u32 lane = 0; lane < LaneCount; ++lane) {
    const auto kind = lane % 8;
    const auto input = lane * 16;
    data[input] = kind < 4 ? kind : kind == 4 ? 12 : kind == 5 ? 21 : kind == 6 ? 6 : 0x1000;
    data[input + 1] = bits(6);
    data[input + 2] = bits(kind < 4 ? origins[kind][0] : 0);
    data[input + 3] = bits(kind < 4 ? origins[kind][1] : 0);
    data[input + 7] = bits(1);
    data[input + 8] = data[input + 9] = Infinity;
    data[input + 10] = bits(1);
    if (kind < 4) {
      const float denominator = kind == 3 ? -2 : -4;
      const float i = kind == 0 || kind == 3 ? -1 : -2;
      const float j = kind == 3 ? -.5f : -1;
      expected[lane] = {bits(2 * denominator), bits(denominator),
                        barycentrics ? bits(i) : Flag + kind,
                        barycentrics ? bits(j) : 1};
    } else if (kind < 6) {
      expected[lane] = sorted ? std::array<u32, 4>{children[1], children[2], children[0], Invalid}
                              : std::array<u32, 4>{children[0], children[1], children[2], Invalid};
    } else {
      expected[lane] = kind == 6 ? std::array<u32, 4>{Invalid, Invalid, Invalid, Invalid}
                                 : std::array<u32, 4>{Infinity, bits(1), Invalid, 0};
    }
    if (lane >= 8 && lane < 12) {
      data[input + 2] = 0x7fc00000; // invalid ray origin
      expected[lane] = {Infinity, bits(1), Invalid, 0};
    } else if (lane == 12 || lane == 13) {
      data[input + 1] = bits(1); // nearest face equals the exclusive extent
      expected[lane] = {Invalid, Invalid, Invalid, Invalid};
    } else if (lane >= 16 && lane < 20) {
      const u32 axis = lane & 1;
      data[input + 2 + axis] = bits(origins[kind][axis] - 4);
      data[input + 5 + axis] = bits(2);
      data[input + 8 + axis] = bits(.5f);
      // Dominant X/Y projection scales all intersection numerators and the
      // denominator by four; the distance and barycentrics remain unchanged.
      for (u32 i = 0; i < (barycentrics ? 4u : 2u); ++i)
        expected[lane][i] = bits(std::bit_cast<float>(expected[lane][i]) * 4);
    } else if (lane == 22) {
      data[input] = 37; // node 4, FP32
      data[input + 1] = bits(10);
      for (u32 i = 5; i <= 10; ++i) data[input + i] = bits(1);
      expected[lane] = {grow ? children[0] : Invalid, Invalid, Invalid, Invalid};
    } else if (lane >= 24 && lane < 28) {
      data[input + 4] = bits(4);
      data[input + 7] = data[input + 10] = bits(-1);
      for (u32 i = 0; i < (barycentrics ? 4u : 2u); ++i)
        expected[lane][i] = bits(-std::bit_cast<float>(expected[lane][i]));
    } else if (lane == 28) {
      data[input] = 0;
      data[input + 3] = bits(-1); // horizontal triangle edge: top-left rule rejects
      expected[lane] = {Infinity, bits(1), barycentrics ? bits(-2) : Flag,
                        barycentrics ? bits(-2) : 0};
    } else if (lane == 29) {
      data[input] = (255 << 3) | 5;
    }
    if (wide) {
      // Keep the same small backing while placing nodes beyond the 32-bit pointer range.
      std::copy_backward(data.begin() + input + 1, data.begin() + input + 11,
                         data.begin() + input + 12);
      data[input + 1] = 1;
      if (lane == 14 || lane == 15) {
        data[input] = lane == 14 ? 0 : 5;
        // Neither an oversized index nor a shifted-address wrap may read node zero.
        data[input + 1] = lane == 14 ? 0x00001000 : 0x20000000;
        expected[lane] = lane == 14 ? std::array<u32, 4>{Infinity, bits(1), Invalid, 0}
                                    : std::array<u32, 4>{Invalid, Invalid, Invalid, Invalid};
      }
    }
  }
  test.expected = test.initial;
  for (u32 lane = 0; lane < LaneCount; ++lane) {
    std::copy(expected[lane].begin(), expected[lane].end(),
              test.expected.begin() + OutputStart + lane * 4);
    auto bounded = expected[lane];
    if (lane % 8 == 5) bounded = {Invalid, Invalid, Invalid, Invalid};
    if (lane >= 20) bounded = {0x12345678, 0x12345679, 0x1234567a, 0x1234567b};
    std::copy(bounded.begin(), bounded.end(),
              test.expected.begin() + OutputStart + LaneCount * 4 + lane * 4);
  }
  auto &code = test.code;
  // The resource is a GPU value, not a materialized host image descriptor.
  AppendSMovLiteral(&code, 8, GuestBase >> 8);
  AppendSMovLiteral(&code, 9, (sorted ? 0x80000000 : 0) | (grow << 23));
  AppendSMovLiteral(&code, 10, node_base + 256);
  AppendSMovLiteral(&code, 11, barycentrics ? 0x81000000 : 0x80000000);
  code.push_back(EncodeVop2(0x1a, 1, InlineU32(6), 0));
  for (u32 offset : {0u, 4u, 8u}) {
    code.insert(code.end(), {EncodeMubuf0(offset == 8 && !wide ? 0x0f : 0x0e, offset * 4),
                             EncodeMubuf1(4 + offset, 0, 1)});
  }
  code.insert(code.end(), {EncodeMimg0(wide ? 0xe7 : 0xe6, 15, 0, false, 0, true),
                           EncodeMimg1(20, 4, 2)});
  code.push_back(EncodeVop2(0x1a, 2, InlineU32(2), 0));
  for (u32 i = 0; i < 4; ++i) AppendStoreVgprAtLaneDwordOffset(&code, 20 + i, 2, OutputStart + i);
  // NSA addressing, the FP32 second-block bound, and inactive destination lanes.
  AppendSMovLiteral(&code, 10, node_base + 2);
  for (u32 i = 0; i < 4; ++i) AppendVMovLiteral(&code, 24 + i, 0x12345678 + i);
  AppendSMovLiteral(&code, 126, 0x000fffff);
  code.insert(code.end(), {EncodeMimg0(wide ? 0xe7 : 0xe6, 15, 3, false, 0, true),
                           EncodeMimg1(24, 4, 2), 0x08070605, 0x0c0b0a09,
                           wide ? 0x000f0e0du : 0x00000e0du});
  code.push_back(EncodeSop1(0x04, 126, 193));
  for (u32 i = 0; i < 4; ++i)
    AppendStoreVgprAtLaneDwordOffset(&code, 24 + i, 2, OutputStart + LaneCount * 4 + i);
  AppendEnd(&code);
  const auto nodes_address = GuestBase + (uint64_t{node_base} << 6);
  test.bda_mappings = {{nodes_address, NodeStart * sizeof(u32)},
                       {nodes_address + 0x4000, (NodeStart + 257 * 16) * sizeof(u32)}};
  test.compute_info.wave_size = LaneCount;
  test.compute_info.threads_num[0] = LaneCount;
  test.compute_info.threads_num[1] = test.compute_info.threads_num[2] = 1;
  test.compute_info.thread_ids_num = 1;
  test.has_compute_info = true;
  test.required_spirv = {"get_bda_pointer"};
  return test;
}
