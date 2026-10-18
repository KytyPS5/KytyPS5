void TestRayTracingInstructions() {
  namespace D = ShaderRecompiler::Decoder;
  const std::array code{
      0xf1989f01u, 0x00081014u,
      EncodeMubuf0(0x1e, 0, false), EncodeMubuf1(16, 0, 0),
      0xf1989f07u, 0x00071001u, 0x3d3e3c15u, 0x38095851u, 0x00003a39u,
      EncodeMubuf0(0x1e, 16, false), EncodeMubuf1(16, 0, 0),
      0xf19c9f01u, 0x00081014u,
      EncodeMubuf0(0x1e, 32, false), EncodeMubuf1(16, 0, 0),
      // Captured 016c1ab44ee6a526: pointer v2:v7 and final inverse component v5.
      0xf19c9f07u, 0x00090202u, 0x28263607u, 0x221f1e29u, 0x00050403u,
      EncodeMubuf0(0x1e, 48, false), EncodeMubuf1(2, 0, 0),
      EncodeSopp(0x01)};
  D::Program decoded;
  D::DecodeProgram(code, decoded);
  Check(decoded.instructions.size() == 9 &&
            decoded.instructions.back().opcode == D::Opcode::S_ENDPGM,
        "decoder stopped before instructions following BVH intersections");
  for (const auto index : {0u, 2u}) {
    const auto &inst = decoded.instructions[index];
    Check(inst.opcode == D::Opcode::IMAGE_BVH_INTERSECT_RAY &&
              inst.image_address_components == 11 && inst.data_dwords == 4 && inst.image_r128,
          "BVH32 instruction lost its ray or descriptor width");
  }
  for (const auto index : {4u, 6u}) {
    const auto &inst = decoded.instructions[index];
    Check(inst.opcode == D::Opcode::IMAGE_BVH_INTERSECT_RAY && inst.opcode_id == 0xe7 &&
              inst.image_address_components == 12 && inst.data_dwords == 4 && inst.image_r128,
          "BVH64 instruction lost its full node pointer or ray width");
  }
  Check(decoded.instructions[6].word_count == 5 &&
            decoded.instructions[6].image_nsa_addr[0] == 7 &&
            decoded.instructions[6].image_nsa_addr[10] == 5,
        "captured BVH64 NSA pointer or final ray component was not decoded");
  Check(decoded.instructions[2].image_nsa_addr[9] == 58,
        "BVH NSA ray components were not decoded");
  const auto result = RecompileForTest(code, MakeCompileOptions(ShaderType::Compute));
  Check(CountSourceOccurrences(result.ir_dump, "BvhIntersect") == 4 &&
            result.program.info.uses_dma && result.program.info.images.empty() &&
            result.program.info.samplers.empty(),
        "BVH intersections did not use GPU descriptors and raw guest memory");
  CheckSpirvBinaryValidates(result.spirv);

  const std::array literal{EncodeSMovB32(0, 255), 0xf1989f01u, EncodeSopp(0x01)};
  D::DecodeProgram(literal, decoded);
  Check(decoded.instructions.size() == 2 && decoded.instructions[0].opcode == D::Opcode::S_MOV_B32,
        "BVH-shaped scalar literal was decoded as an instruction");
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
  for (const auto words : {std::array{0xf19c9f01u, 0x40081014u}, // BVH64 A16
                           std::array{0xf1989f01u, 0x40081014u}, // BVH32 A16
                           std::array{0xf1981f01u, 0x00081014u}, // not R128
                           std::array{0xf1989f01u, 0x80081014u}}) { // D16
    const std::array unsupported{words[0], words[1], EncodeSopp(0x01)};
    ExpectFatal([&]() { (void)RecompileForTest(unsupported, MakeCompileOptions(ShaderType::Compute)); },
                "unsupported BVH encoding was silently accepted");
  }
  for (const auto encoding : {0xf1989f01u, 0xf19c9f01u}) {
    for (const auto nsa : {1u, 2u}) {
      std::array<uint32_t, 5> truncated{encoding | (nsa << 1), 0x00081014u};
      truncated[2 + nsa] = EncodeSopp(0x01);
      ExpectFatal([&]() {
        (void)RecompileForTest(std::span(truncated).first(3 + nsa), MakeCompileOptions(ShaderType::Compute));
      }, "BVH NSA instruction accepted an incomplete ray address");
    }
  }
#endif
}
