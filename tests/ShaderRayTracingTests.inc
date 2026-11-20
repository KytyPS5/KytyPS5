void TestTrinityStackInstruction() {
  namespace D = ShaderRecompiler::Decoder;
  const std::array code{0xdb840008u, 0x01010a09u,
                        0xdb840010u, 0x02010a09u, EncodeSopp(0x01)};
  D::Program decoded;
  D::DecodeProgram(code, decoded);
  const auto& first = decoded.instructions.front();
  Check(first.opcode == D::Opcode::DS_BVH_STACK_PUSH8_POP1_RTN_B32 &&
            first.src_count == 3 && first.offset == 8 && first.dst.reg == 1 &&
            first.src0.reg == 9 && first.src1.reg == 10 && first.src2.reg == 1,
        "Trinity stack operands or capacity were not decoded");
  ShaderComputeInputInfo compute{};
  compute.lds_size_dwords = 1024;
  auto options = MakeCompileOptions(ShaderType::Compute);
  options.input_info.compute = &compute;
  const auto result = RecompileForTest(code, options);
  Check(CountSourceOccurrences(result.ir_dump, " = BvhStack ") == 2 &&
            result.program.info.uses_lds && result.program.info.bvh_stack &&
            !result.program.info.shared_int64_atomics,
        "unused stack results discarded LDS writes or selected 64-bit atomics");
  Check(SpirvInstructionOpcodeCount(result.spirv, 54u) == 2 &&
            SpirvInstructionOpcodeCount(result.spirv, 246u) == 1,
        "Trinity stack calls duplicated the function or unrolled its eight pushes");
  CheckSpirvBinaryValidates(result.spirv);
  compute.lds_storage = true;
  CheckSpirvBinaryValidates(RecompileForTest(code, options).spirv);
  options = MakeCompileOptions(ShaderType::Pixel);
  CheckSpirvBinaryValidates(RecompileForTest(code, options).spirv);
  for (const auto words : {std::array{0xdb860008u, 0x01010a09u},
                           std::array{0xdb840007u, 0x01010a09u},
                           std::array{0xdb840020u, 0x01010a09u},
                           std::array{0xdb840008u, 0x01f90a09u},
                           std::array{0xdb840008u, 0x09010a09u}}) {
    const std::array invalid{words[0], words[1], EncodeSopp(0x01)};
    D::DecodeProgram(invalid, decoded);
    Check(decoded.instructions[0].opcode == D::Opcode::UNSUPPORTED,
          "unsupported stack encoding was silently accepted");
  }
}

void TestTrinityBvhInstruction() {
  namespace D = ShaderRecompiler::Decoder;
  const std::array code{0xf1949f05u, 0x00010000u, 0x0a030201u, 0x0000040du,
      EncodeMubuf0(0x1e, 0, false), EncodeMubuf1(0, 0, 0),
      EncodeMubuf0(0x1e, 16, false), EncodeMubuf1(4, 0, 0),
      EncodeMubuf0(0x1d, 32, false), EncodeMubuf1(8, 0, 0),
      EncodeMubuf0(0x1e, 40, false), EncodeMubuf1(10, 0, 0),
      EncodeMubuf0(0x1d, 56, false), EncodeMubuf1(14, 0, 0), EncodeSopp(0x01)};
  D::Program decoded;
  D::DecodeProgram(code, decoded);
  const auto& inst = decoded.instructions.front();
  Check(inst.opcode == D::Opcode::IMAGE_BVH8_INTERSECT_RAY && inst.word_count == 4 &&
            inst.data_dwords == 10 && inst.image_address_components == 11 &&
            inst.image_nsa_addr[3] == 10 && inst.image_nsa_addr[5] == 12 &&
            inst.image_nsa_addr[6] == 13 && inst.image_nsa_addr[8] == 15 &&
            inst.image_nsa_addr[9] == 4 && inst.src1.reg == 4,
        "Trinity BVH8 grouped ray registers were not decoded");
  const auto result = RecompileForTest(code, MakeCompileOptions(ShaderType::Compute));
  Check(result.program.info.bvh8 && !result.program.info.bvh &&
            result.program.info.uses_dma && result.program.info.images.empty(),
        "Trinity BVH8 did not use its separate raw-memory helper");
  Check(CountSourceOccurrences(result.ir_dump, " = Bvh8Intersect ") == 1,
        "Trinity BVH8 result and ray writes duplicated the intersection");
  CheckSpirvBinaryValidates(result.spirv);
  Check(SpirvInstructionOpcodeCount(result.spirv, 54u) == 6 &&
            SpirvInstructionOpcodeCount(result.spirv, 246u) == 1,
        "Trinity BVH8 duplicated helpers or unrolled its four-child loop");
  CheckSpirvBinaryValidates(RecompileForTest(code, MakeCompileOptions(ShaderType::Pixel)).spirv);
  for (const auto words : {std::array{0xf1949f05u, 0x40010000u, 0x0a030201u, 0x0000040du},
                           std::array{0xf1949f05u, 0x0001f700u, 0x0a030201u, 0x0000040du},
                           std::array{0xf1949f05u, 0x00010000u, 0xfe030201u, 0x0000040du},
                           std::array{0xf1949f05u, 0x00010700u, 0x0a030201u, 0x0000040du},
                           std::array{0xf1949f05u, 0x00010000u, 0x0a030201u, 0x0000040bu}}) {
    const std::array invalid{words[0], words[1], words[2], words[3], EncodeSopp(0x01)};
    D::DecodeProgram(invalid, decoded);
    Check(decoded.instructions[0].opcode == D::Opcode::UNSUPPORTED,
          "Trinity BVH8 accepted truncated ray or destination registers");
  }
}

// Gran Turismo 7's closest hit traversal (CS, wave64; the code at 0x05e0cbbc of its eboot): its
// instance code walks the BLAS itself, and the leaf code calls the any-hit function of the
// triangle's material through a call table whose V# the shader loads once, before its loop.
constexpr uint32_t kGt7ClosestLeafWave64[] = {
    0xbfa00003u, 0xd746001au, 0x04010c02u, 0xf4080700u, 0xfa000060u, 0xf4100100u, 0xfa000090u, 0xf40c0500u,
    0xfa0000d0u, 0xf4001a80u, 0xfa0000f0u, 0x2c043486u, 0x36023487u, 0xd5480000u, 0x020d071au, 0xbe8203ffu,
    0x3dc8c8cau, 0x7e3002ffu, 0x21ffffffu, 0xbf8cc07fu, 0xe0302000u, 0x80070302u, 0xbf8c3f70u, 0x360406ffu,
    0x0000ffffu, 0x2c060690u, 0xd7460016u, 0x04050702u, 0xd7460017u, 0x04010703u, 0x7e160d16u, 0xf0000f08u,
    0x00030416u, 0xf0000f08u, 0x00010016u, 0x7e140d17u, 0xd5410009u, 0x005a1614u, 0xd5410008u, 0x00661617u,
    0xd541002eu, 0x01aa161au, 0x3e121415u, 0x3e101418u, 0x3e5c141bu, 0x10161309u, 0x3e161108u, 0x3e165d2eu,
    0x7e165d0bu, 0x101e130bu, 0x101c110bu, 0x101a5d0bu, 0xbf8c3f71u, 0x7e161704u, 0x7d940880u, 0x7e081705u,
    0x7e362507u, 0x7e0a1706u, 0xbf8c3f70u, 0x0216010bu, 0x020c0304u, 0x101836f9u, 0x06862602u, 0x020a0505u,
    0x10081f0bu, 0xd5410011u, 0x03ca18f0u, 0xd541000au, 0x04121d06u, 0x7e185511u, 0x3e140b0du, 0x1020190cu,
    0x10281d0cu, 0x10381b0cu, 0x1024150au, 0x10081f0cu, 0xd5410012u, 0x84422112u, 0x063224f2u, 0x7c0224f3u,
    0x7e2a6719u, 0x0225e4f9u, 0x86860680u, 0xd5410015u, 0xa456190au, 0x3e280d15u, 0x3e380b15u, 0x3e081715u,
    0x10262912u, 0x10283912u, 0x102a0912u, 0x7c1a26f9u, 0x06868280u, 0x7c1a2880u, 0x7c1a2af9u, 0x06868480u,
    0x88ea6a02u, 0x88fe6a04u, 0xbf88058bu, 0x7e242307u, 0xf4001a80u, 0xfa000168u, 0x100e0703u, 0xbf8cc07fu,
    0xd76d0019u, 0x045e2c6au, 0x7c023680u, 0x102424ffu, 0x3b808081u, 0x7e080280u, 0x7e380280u, 0x36323281u,
    0xbe8403ffu, 0x3a83126fu, 0x10242512u, 0x7e0602f2u, 0xbe82047eu, 0x02323280u, 0x7d8a3280u, 0x023a2507u,
    0x7c3c08f9u, 0x8606001du, 0xbf88001au, 0xd541001cu, 0x03ce3b1du, 0xf4041a80u, 0xfa000158u, 0xf40c0100u,
    0xfa000138u, 0xbf8cc07fu, 0x4a082c6au, 0x4a062e6bu, 0x363a08bfu, 0x363c06bfu, 0xf0000308u, 0x0001031du,
    0xbf8c3f70u, 0xd541001eu, 0x03ca3904u, 0x7e386d03u, 0x7e3a6b03u, 0x7e06551eu, 0xd51f8003u, 0x20020903u,
    0x080806f2u, 0x7e066703u, 0x7e3c6704u, 0x10083d1cu, 0x10383d1du, 0xbefe0402u, 0xbe860b81u, 0x7c03e0f9u,
    0x86260006u, 0x7c0de0f9u, 0x86268406u, 0x7d8a32f9u, 0x06868280u, 0x023c0a80u, 0x023a0cf9u, 0x86160605u,
    0xd501001fu, 0x20120d0bu, 0x10403d1eu, 0x3e403b1du, 0x3e403f1fu, 0x7e405d20u, 0x103c3d20u, 0x103a3b20u,
    0x103e3f20u, 0xbeea2402u, 0xbf88000fu, 0x10443b05u, 0x104a3f0bu, 0x10463d06u, 0x10483d04u, 0x104c3b04u,
    0x10083f04u, 0xd51f0022u, 0x20020d1fu, 0xd51f0025u, 0x20020b1eu, 0xd51f0023u, 0x2002171du, 0x3e483922u,
    0x3e4c3925u, 0x3e083923u, 0x8afe7e6au, 0xbf880020u, 0x10443b02u, 0x10463f00u, 0x10403d01u, 0xd51f0022u,
    0x2002031fu, 0xd51f0023u, 0x2002051eu, 0xd51f0020u, 0x2002011du, 0x103a4522u, 0x3e3a4723u, 0x3e3a4120u,
    0x7e425d1du, 0x103a4721u, 0x103c4121u, 0x10464521u, 0x10423b02u, 0x104a3d00u, 0x10444701u, 0x10484704u,
    0x104c3b04u, 0xd51f0021u, 0x2002031eu, 0x103c3d04u, 0xd51f0025u, 0x20020523u, 0xd51f0022u, 0x2002011du,
    0x3e483921u, 0x3e4c3925u, 0xd5410004u, 0x047a451cu, 0xbefe046au, 0x7d8a3280u, 0x103e2b15u, 0x02381700u,
    0x02400d01u, 0x02320b02u, 0x3e3e2713u, 0x3e48071cu, 0xd541001du, 0x049a4103u, 0xd541001eu, 0x04123303u,
    0x3e3e2914u, 0x10384924u, 0x7e3e5d1fu, 0x3e383b1du, 0x10322af9u, 0x1606061fu, 0x100826f9u, 0x1606061fu,
    0x3e383d1eu, 0x104228f9u, 0x1606061fu, 0x02441ef9u, 0x16060619u, 0x023e1cf9u, 0x16060604u, 0x7e465d1cu,
    0x024a1af9u, 0x16060621u, 0x10384923u, 0x103a3b23u, 0x103c3d23u, 0x10463922u, 0x3e463f1du, 0xd51f0023u,
    0x08024b1eu, 0xd5410022u, 0x848a471cu, 0xd541001fu, 0x847e471du, 0xd5410025u, 0x8496471eu, 0x103a4522u,
    0x3e3a3f1fu, 0x3e3a4b25u, 0x7e405d1du, 0x103a4520u, 0x10383f20u, 0x103c4b20u, 0xbe842402u, 0xbf880009u,
    0x7c1a2af9u, 0x06868680u, 0x7c1a2680u, 0x7c1a28f9u, 0x06868880u, 0x88ea6a06u, 0x88ea086au, 0x022302f9u,
    0x86860680u, 0x8afe7e04u, 0xbf880026u, 0x10403b00u, 0x103e22f9u, 0x06160611u, 0x102a3b11u, 0x10283911u,
    0x10443d11u, 0x3e40031cu, 0x3e40051eu, 0x103840f9u, 0x06160620u, 0x3e3e391fu, 0x063c3ef2u, 0x7c023ef3u,
    0x7e3a66f9u, 0x0026061eu, 0x0239e4f9u, 0x86860680u, 0xd541001du, 0x84764111u, 0xd51f0015u, 0x4002011du,
    0xd51f0014u, 0x4002031du, 0xd51f0022u, 0x4002051du, 0x10262b1cu, 0x1028291cu, 0x102a451cu, 0x10222713u,
    0x3e222914u, 0x3e222b15u, 0x7c1a2280u, 0x7e3c5d11u, 0x022302f9u, 0x86860680u, 0x103a271eu, 0x1038291eu,
    0x103c2b1eu, 0xbefe0404u, 0xbeea03ffu, 0x3c23d70au, 0x10283b00u, 0x1026171du, 0x100e0f07u, 0x10242512u,
    0x3e28031cu, 0x3e263906u, 0x200e0effu, 0x358637bdu, 0x202424ffu, 0x358637bdu, 0x3e28051eu, 0x3e263d05u,
    0x7c0228f9u, 0x0686866au, 0x7c0226f9u, 0x0686846au, 0xbe822402u, 0xbf880042u, 0x102a2713u, 0xd5410014u,
    0x840e2503u, 0x101e191du, 0x101c191cu, 0x101a191eu, 0xd5410010u, 0x84422115u, 0xd5410014u, 0x03ca2903u,
    0xd501001fu, 0x00110280u, 0x060620f2u, 0x7c0220f3u, 0x10202914u, 0x7e286703u, 0x0207e4f9u, 0x86860680u,
    0x7e205510u, 0xd5410014u, 0x8452270cu, 0x10202510u, 0xd51f000fu, 0x40021714u, 0xd51f000eu, 0x40020d14u,
    0xd51f000du, 0x40020b14u, 0x104020ffu, 0x3ea2f983u, 0x100a1f03u, 0x100c1d03u, 0x10061b03u, 0x10160b05u,
    0x3e160d06u, 0x3e160703u, 0x7e165d0bu, 0x3e321705u, 0x3e081706u, 0x3e421703u, 0x10063319u, 0x100a0119u,
    0x3e060904u, 0x3e0a0901u, 0x3e064321u, 0x3e0a4302u, 0x7e045d03u, 0x7c0406f9u, 0x06868880u, 0x10020b02u,
    0x200002f9u, 0x06862680u, 0x7c0c0280u, 0xd5410001u, 0x84020107u, 0x88ea6a08u, 0x8bea066au, 0xd5410000u,
    0x03ca0300u, 0x024300f9u, 0x86860681u, 0x10000100u, 0x7e005500u, 0x10000f00u, 0x104400ffu, 0x3ea2f983u,
    0x8afe7e02u, 0xbf880029u, 0x08041f1du, 0x08021d1cu, 0xd5410004u, 0x840e0f03u, 0x08001b1eu, 0xd5010021u,
    0x00190280u, 0x10180502u, 0x10041702u, 0xd5410003u, 0x03ca0903u, 0x3e180301u, 0xd5410004u, 0x040a0d01u,
    0x10060703u, 0x3e180100u, 0x3e080105u, 0x7e065503u, 0x7e045d0cu, 0x7c0418f9u, 0x06868680u, 0x10020f03u,
    0x10000902u, 0x104402ffu, 0x3ea2f983u, 0x200200f9u, 0x06862680u, 0x7c0c0080u, 0xd5410000u, 0x84060312u,
    0x88ea6a06u, 0x8bea046au, 0xd5410000u, 0x03ca0101u, 0x023f00f9u, 0x86860681u, 0x10000100u, 0x7e005500u,
    0x10002500u, 0x104000ffu, 0x3ea2f983u, 0xbefe0402u, 0x38003f21u, 0x36000111u, 0x7daa0080u, 0xbf880426u,
    0xf4040400u, 0xfa000118u, 0xbf8cc07fu, 0xf4001a88u, 0xfa000018u, 0xbf8cc07fu, 0xbf0d926au, 0xbf840001u,
    0xbf920001u, 0x7e50551du, 0xf40c0100u, 0xfa000070u, 0xf4040588u, 0xfa000058u, 0xbe820380u, 0xbf8cc07fu,
    0xf0000108u, 0x00010016u, 0x876a17ffu, 0x000003ffu, 0xbeeb03ffu, 0x461c4000u, 0xd7610027u, 0x00011802u,
    0xf4040300u, 0xfa000160u, 0xf4040380u, 0xfa0000f4u, 0xf40004c0u, 0xfa0000fcu, 0x7e52551cu, 0x7e54551eu,
    0xbf8cc07fu, 0x085618f9u, 0x8686066bu, 0x7e4a02c1u, 0x7e4c0280u, 0x9494ff10u, 0x00280008u, 0x8817ff6au,
    0x81000000u, 0xf4041a88u, 0xfa000030u, 0xf4081400u, 0xfa000120u, 0xf4041600u, 0xfa000130u, 0xbe9203a5u,
    0xbf8cc07fu, 0x8002106au, 0xbe951d9fu, 0xd7610027u, 0x00010002u, 0x8202116bu, 0x8003ff10u, 0x00000080u,
    0xd7610027u, 0x00010202u, 0x82028011u, 0xd7610027u, 0x00010c03u, 0xd7610027u, 0x00010e02u, 0xbf8c3f70u,
    0x0a00000du, 0xd541002du, 0x003a0109u, 0xd541002eu, 0x004e012eu, 0xd541002fu, 0x003e0108u, 0x3e5a3a0cu,
    0x3e5c3c0cu, 0x3e5e380cu, 0xbe84047eu, 0x876a8712u, 0x816ac46au, 0xbf0b826au, 0xbf850227u, 0xbf0712c1u,
    0xbf800000u, 0x85ea807eu, 0x87188712u, 0x87ea6a04u, 0xbeda246au, 0xbf88021eu, 0xd7600002u, 0x00010d27u,
    0xd7600003u, 0x00010f27u, 0xbf071887u, 0xbedc0402u, 0xbf840009u, 0xd7600002u, 0x00010127u, 0x906a8312u,
    0xbeeb0380u, 0x8fea876au, 0x805c6a02u, 0xd7600002u, 0x00010327u, 0x825d6b02u, 0xf410012eu, 0xfa000000u,
    0xbf8cc07fu, 0xbeeb0311u, 0xbf0d9813u, 0xbf840208u, 0xd7610027u, 0x00010414u, 0x10023a04u, 0x10043a08u,
    0x10063a0cu, 0x8711ff11u, 0x003fffffu, 0xd7610027u, 0x0001105au, 0xd5410004u, 0x001e5a04u, 0xd5410005u,
    0x002e5a08u, 0xd5410000u, 0x003e5a0cu, 0xd5410030u, 0x04063805u, 0xd5410031u, 0x040a3809u, 0xd5410032u,
    0x040e380du, 0x8fde8310u, 0xd5410033u, 0x04125e05u, 0xf408152fu, 0xfa000058u, 0xd5410034u, 0x04165e09u,
    0xbf8cc07fu, 0x816a8154u, 0xd5410035u, 0x04025e0du, 0x3e603c06u, 0x3e623c0au, 0x3e643c0eu, 0xd7600002u,
    0x00011927u, 0xd7610027u, 0x00010615u, 0x7e6c5530u, 0x7e6e5531u, 0x7e705532u, 0x3e665c06u, 0xd7610027u,
    0x00010816u, 0x80066ac1u, 0x3e685c0au, 0x820780c1u, 0x3e6a5c0eu, 0xd7610027u, 0x00010a17u, 0x876a07ffu,
    0x000003ffu, 0x9484ff5eu, 0x00280008u, 0x9062966bu, 0x8807ff6au, 0x81000000u, 0x8855ff5fu, 0x00080000u,
    0xbed4035eu, 0xbed703ffu, 0x00016204u, 0x875a8262u, 0xbf061887u, 0xbee30302u, 0x851012a5u, 0x8764ff6bu,
    0x01000000u, 0x93e5ff6bu, 0x00010019u, 0xbe851d9fu, 0xbe88047eu, 0x876a8710u, 0x816ac46au, 0xbf0b826au,
    0xbf850071u, 0xbf0710c1u, 0xbf84006eu, 0xbe822408u, 0xbf88006bu, 0x876ac510u, 0x906b8310u, 0xf420032au,
    0xd4000000u, 0xbf8cc07fu, 0x876a870cu, 0xbf076a87u, 0xbf840063u, 0xd7610027u, 0x00011402u, 0x876ac710u,
    0x8102816bu, 0xf420036au, 0xd4000004u, 0xbf8cc07fu, 0x7e00020cu, 0x926a9788u, 0xbf076480u, 0xbf800000u,
    0x8588807eu, 0xf1989f07u, 0x00010000u, 0x3534332bu, 0x36323130u, 0x00003837u, 0xbf8c3f70u, 0x7e085501u,
    0x2c02009fu, 0x7d8a00f9u, 0x06868a6au, 0x10000900u, 0x7d840265u, 0x878e080au, 0x7c0856f9u, 0x06069000u,
    0x87ea0a6au, 0x8aea086au, 0x8dea0e6au, 0x8dea6a10u, 0xbee0246au, 0xbf880037u, 0x10020902u, 0xbf8cc07fu,
    0x906a9e0du, 0x10040903u, 0x886a6a62u, 0xbf0d806au, 0xbf800000u, 0x856a8081u, 0xbf075a80u, 0xbf800000u,
    0x856a6a80u, 0x7e0e026au, 0xbf076a80u, 0xbf850020u, 0x7e080301u, 0x906b830cu, 0xf4001aafu, 0xfa00007cu,
    0xbe880bc4u, 0xbf8cc07fu, 0x80eb6a6bu, 0xf4001aafu, 0xfa000028u, 0x360c1af9u, 0x86860608u, 0xbf8cc07fu,
    0x97ea6a6bu, 0x7e0a0302u, 0xf4001aefu, 0xd4000000u, 0xf4001aaeu, 0xfa000078u, 0x7e0e0281u, 0xbf8cc07fu,
    0x816a6a6bu, 0x936a6a58u, 0x816a6a59u, 0x8f6a846au, 0xf4280228u, 0xd4000000u, 0xbf8cc07fu, 0xbe8e0308u,
    0xbe8f0309u, 0xbe90030au, 0xbe91030bu, 0xbe8e210eu, 0x7daa0e80u, 0xbf880007u, 0x7e4a020du, 0x7e30025cu,
    0x7e32025du, 0x7e4c020cu, 0x7e480301u, 0x7e460302u, 0x7e560300u, 0xbf8cc07fu, 0xbf04800du, 0xbefe0460u,
    0xbf850005u, 0x8f6a8302u, 0x81028102u, 0xf424032au, 0xd4000000u, 0xbf82ffa5u, 0xd7600002u, 0x00011527u,
    0xbefe0402u, 0xbf820134u, 0x7e0002c1u, 0x7e0202c1u, 0x7e0402c1u, 0x7e0602c1u, 0xbeea2408u, 0xbf88000au,
    0x7e000210u, 0xbf800000u, 0xf1989f07u, 0x00010000u, 0x3534332bu, 0x36323130u, 0x00003837u, 0xbf8c3f70u,
    0x7d8a00f9u, 0x068688c1u, 0xbf138008u, 0xbefe046au, 0xbf840121u, 0x7d8a02c1u, 0xbf13806au, 0xbf8400e7u,
    0x7d8a04c1u, 0xbf13806au, 0xbf84007cu, 0x7d8a06c1u, 0xbf13806au, 0xbf840019u, 0x7d8a06c1u, 0xd7600002u,
    0x00011927u, 0xbe83146au, 0xd760006au, 0x00000703u, 0xd760006bu, 0x00000701u, 0xd7600010u, 0x00000700u,
    0xd761002cu, 0x0000046au, 0xd760006au, 0x00000702u, 0x81028102u, 0xd761002cu, 0x0000046au, 0x81028102u,
    0xd761002cu, 0x0000046bu, 0x81028102u, 0x8382bc02u, 0xd7610027u, 0x00011802u, 0xbf82ff56u, 0x7d8a04f9u,
    0x06868ac1u, 0xd7600002u, 0x00011927u, 0xbeea140au, 0xd7600010u, 0x0000d500u, 0xd7600011u, 0x0000d501u,
    0xd7600012u, 0x0000d502u, 0x7d8a0410u, 0x7d8a04f9u, 0x06868c11u, 0x7d8a04f9u, 0x06868e12u, 0xd761002cu,
    0x00000412u, 0x81028102u, 0x87ea6a0au, 0xd7610027u, 0x00011802u, 0x87ea0c6au, 0x87ea0e6au, 0xbf13806au,
    0xbf84000eu, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d502u, 0xd761002cu, 0x0000046au,
    0x81028102u, 0xd761002cu, 0x00000411u, 0x81028102u, 0xd7610027u, 0x00011802u, 0xbf820032u, 0x7d8a02f9u,
    0x06868ac1u, 0x7d8a0210u, 0xd7600002u, 0x00011927u, 0x7d8a02f9u, 0x06868c11u, 0x7d8a02f9u, 0x06868e12u,
    0x87ea6a0au, 0xd761002cu, 0x00000411u, 0x81028102u, 0x87ea0c6au, 0xd7610027u, 0x00011802u, 0x87ea0e6au,
    0xbf84000bu, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d501u, 0xd761002cu, 0x0000046au,
    0x81028102u, 0xd7610027u, 0x00011802u, 0xbf820015u, 0x7d8a00f9u, 0x068682c1u, 0x7d8a0010u, 0x7d8a00f9u,
    0x06868a11u, 0x7d8a00f9u, 0x06868c12u, 0x87ea6a02u, 0x87ea0a6au, 0x87ea0c6au, 0xbf84000au, 0xd7600002u,
    0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u,
    0x00011802u, 0xd7600002u, 0x00011927u, 0x8382bc02u, 0xd7610027u, 0x00011802u, 0xbf82fef6u, 0x7d8a02f9u,
    0x06868ac1u, 0xd7600002u, 0x00011927u, 0xbeea140au, 0xd7600010u, 0x0000d500u, 0xd760000eu, 0x0000d501u,
    0x7d8a0210u, 0x7d8a02f9u, 0x06868c0eu, 0xd761002cu, 0x0000040eu, 0x81028102u, 0x87ea6a0au, 0xd7610027u,
    0x00011802u, 0x878a0c6au, 0xbf84002eu, 0xd7600002u, 0x00011927u, 0xbeea140au, 0xd760000cu, 0x0000d501u,
    0x7d8a020cu, 0xd761002cu, 0x0000040cu, 0x81028102u, 0xd7610027u, 0x00011802u, 0x87ea0a6au, 0xbf84000bu,
    0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d501u, 0xd761002cu, 0x0000046au, 0x81028102u,
    0xd7610027u, 0x00011802u, 0xbf820036u, 0x7d8a00f9u, 0x068682c1u, 0x7d8a0010u, 0x7d8a00f9u, 0x06868a0eu,
    0x7d8a00f9u, 0x06868c0cu, 0x87ea6a02u, 0x87ea0a6au, 0x87ea0c6au, 0xbf84000au, 0xd7600002u, 0x00011927u,
    0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u,
    0xbf820020u, 0x7d8a00f9u, 0x068682c1u, 0x7d8a0010u, 0x7d8a00f9u, 0x06868a0eu, 0x87ea6a02u, 0x878a0a6au,
    0xbf840018u, 0xd7600002u, 0x00011927u, 0xbeea140au, 0xd760006au, 0x0000d500u, 0x7d8a00f9u, 0x06868c6au,
    0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0x87ea0a0cu, 0xbf84000au, 0xd7600002u,
    0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u,
    0x00011802u, 0xd7600002u, 0x00011927u, 0x8382bc02u, 0xd7610027u, 0x00011802u, 0xbf82fe8eu, 0x7d8a00f9u,
    0x068682c1u, 0xbe9003c1u, 0xbf138002u, 0xbf840031u, 0xbeea1402u, 0xd7600010u, 0x0000d500u, 0x7d8a0010u,
    0x878a6a02u, 0xbf840026u, 0xd7600002u, 0x00011927u, 0xbeea140au, 0xd760006au, 0x0000d500u, 0x7d8a00f9u,
    0x06868c6au, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0x878a0a0cu, 0xbf840018u,
    0xd7600002u, 0x00011927u, 0xbeea140au, 0xd760006au, 0x0000d500u, 0x7d8a00f9u, 0x06868c6au, 0xd761002cu,
    0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0x87ea0a0cu, 0xbf84000au, 0xd7600002u, 0x00011927u,
    0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u,
    0xd7600002u, 0x00011927u, 0x8382bc02u, 0xd7610027u, 0x00011802u, 0xbf82fe57u, 0xd7600002u, 0x00011927u,
    0xbf076302u, 0xbf840008u, 0xd7600002u, 0x00011927u, 0x8102c102u, 0xd7600010u, 0x0000052cu, 0xd7610027u,
    0x00011802u, 0xbf82fe4au, 0xd7600014u, 0x00010527u, 0xd7600015u, 0x00010727u, 0xd7600016u, 0x00010927u,
    0xd7600017u, 0x00010b27u, 0xd760005au, 0x00011127u, 0xbefe045au, 0xbf820133u, 0x7e0002c1u, 0x7e0202c1u,
    0x7e0402c1u, 0x7e0602c1u, 0xbeea2404u, 0xbf88000au, 0x7e000212u, 0xbf800000u, 0xf1989f07u, 0x00050000u,
    0x2e2f2d2bu, 0x281e1c1du, 0x00002a29u, 0xbf8c3f70u, 0x7d8a00f9u, 0x068684c1u, 0xbf138004u, 0xbefe046au,
    0xbf840120u, 0x7d8a02c1u, 0xbf13806au, 0xbf8400e6u, 0x7d8a04c1u, 0xbf13806au, 0xbf84007bu, 0x7d8a06c1u,
    0xbf13806au, 0xbf840019u, 0x7d8a06c1u, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006bu, 0x0000d503u,
    0xd7600006u, 0x0000d502u, 0xd7600003u, 0x0000d501u, 0xd7600012u, 0x0000d500u, 0xd761002cu, 0x0000046bu,
    0x81028102u, 0xd761002cu, 0x00000406u, 0x81028102u, 0xd761002cu, 0x00000403u, 0x81028102u, 0x8382bc02u,
    0xd7610027u, 0x00011802u, 0xbf82fda0u, 0x7d8a04f9u, 0x068686c1u, 0xd7600002u, 0x00011927u, 0xbeea1406u,
    0xd7600012u, 0x0000d500u, 0xd760000cu, 0x0000d501u, 0xd760000du, 0x0000d502u, 0x7d8a0412u, 0x7d8a04f9u,
    0x0686880cu, 0x7d8a04f9u, 0x06868a0du, 0xd761002cu, 0x0000040du, 0x81028102u, 0x87ea6a06u, 0xd7610027u,
    0x00011802u, 0x87ea086au, 0x87ea0a6au, 0xbf84000eu, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au,
    0x0000d502u, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd761002cu, 0x0000040cu, 0x81028102u, 0xd7610027u,
    0x00011802u, 0xbf820032u, 0x7d8a02f9u, 0x068686c1u, 0x7d8a0212u, 0xd7600002u, 0x00011927u, 0x7d8a02f9u,
    0x0686880cu, 0x7d8a02f9u, 0x06868a0du, 0x87ea6a06u, 0xd761002cu, 0x0000040cu, 0x81028102u, 0x87ea086au,
    0xd7610027u, 0x00011802u, 0x87ea0a6au, 0xbf84000bu, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au,
    0x0000d501u, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0xbf820015u, 0x7d8a00f9u,
    0x068682c1u, 0x7d8a0012u, 0x7d8a00f9u, 0x0686860cu, 0x7d8a00f9u, 0x0686880du, 0x87ea6a02u, 0x87ea066au,
    0x87ea086au, 0xbf84000au, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu,
    0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0xd7600002u, 0x00011927u, 0x8382bc02u, 0xd7610027u,
    0x00011802u, 0xbf82fd41u, 0x7d8a02f9u, 0x068686c1u, 0xd7600002u, 0x00011927u, 0xbeea1406u, 0xd7600012u,
    0x0000d500u, 0xd760000au, 0x0000d501u, 0x7d8a0212u, 0x7d8a02f9u, 0x0686880au, 0xd761002cu, 0x0000040au,
    0x81028102u, 0x87ea6a06u, 0xd7610027u, 0x00011802u, 0x8786086au, 0xbf84002eu, 0xd7600002u, 0x00011927u,
    0xbeea1406u, 0xd7600008u, 0x0000d501u, 0x7d8a0208u, 0xd761002cu, 0x00000408u, 0x81028102u, 0xd7610027u,
    0x00011802u, 0x87ea066au, 0xbf84000bu, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d501u,
    0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0xbf820036u, 0x7d8a00f9u, 0x068682c1u,
    0x7d8a0012u, 0x7d8a00f9u, 0x0686860au, 0x7d8a00f9u, 0x06868808u, 0x87ea6a02u, 0x87ea066au, 0x87ea086au,
    0xbf84000au, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu, 0x0000046au,
    0x81028102u, 0xd7610027u, 0x00011802u, 0xbf820020u, 0x7d8a00f9u, 0x068682c1u, 0x7d8a0012u, 0x7d8a00f9u,
    0x0686860au, 0x87ea6a02u, 0x8786066au, 0xbf840018u, 0xd7600002u, 0x00011927u, 0xbeea1406u, 0xd760006au,
    0x0000d500u, 0x7d8a00f9u, 0x0686886au, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u,
    0x87ea0608u, 0xbf84000au, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu,
    0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0xd7600002u, 0x00011927u, 0x8382bc02u, 0xd7610027u,
    0x00011802u, 0xbf82fcd9u, 0x7d8a00f9u, 0x068682c1u, 0xbe9203c1u, 0xbf138002u, 0xbf840031u, 0xbeea1402u,
    0xd7600012u, 0x0000d500u, 0x7d8a0012u, 0x87866a02u, 0xbf840026u, 0xd7600002u, 0x00011927u, 0xbeea1406u,
    0xd760006au, 0x0000d500u, 0x7d8a00f9u, 0x0686886au, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u,
    0x00011802u, 0x87860608u, 0xbf840018u, 0xd7600002u, 0x00011927u, 0xbeea1406u, 0xd760006au, 0x0000d500u,
    0x7d8a00f9u, 0x0686886au, 0xd761002cu, 0x0000046au, 0x81028102u, 0xd7610027u, 0x00011802u, 0x87ea0608u,
    0xbf84000au, 0xd7600002u, 0x00011927u, 0xbeea146au, 0xd760006au, 0x0000d500u, 0xd761002cu, 0x0000046au,
    0x81028102u, 0xd7610027u, 0x00011802u, 0xd7600002u, 0x00011927u, 0x8382bc02u, 0xd7610027u, 0x00011802u,
    0xbf82fca2u, 0xd7600002u, 0x00011927u, 0xbf070280u, 0xbf840008u, 0xd7600002u, 0x00011927u, 0x8102c102u,
    0xd7600012u, 0x0000052cu, 0xd7610027u, 0x00011802u, 0xbf82fc95u, 0xbe82047eu, 0x7daa4ac1u, 0xbf88001fu,
    0xdc348030u, 0x017d0018u, 0xdc308078u, 0x057d0018u, 0xd5480006u, 0x02710726u, 0x36084affu, 0x3fffffffu,
    0xbf8c3f71u, 0x360404ffu, 0x003fffffu, 0x34000283u, 0xd54e0001u, 0x02760302u, 0xdc30807cu, 0x077d0000u,
    0xdc348028u, 0x027d0000u, 0xbf8c3f71u, 0x4c0c0f06u, 0xbf8c3f70u, 0xd70f6a00u, 0x00020102u, 0x34040c82u,
    0x50020303u, 0x2c060c9eu, 0xd70f6a00u, 0x00020500u, 0x50020701u, 0xdc308000u, 0x007d0000u, 0x8afe7e02u,
    0xbf880001u, 0x7e0802c1u, 0xbefe0402u, 0x7daa08c1u, 0xbf880018u, 0xbf8c3f70u, 0x4a000105u, 0xf4080100u,
    0xfa000050u, 0x100248ffu, 0x47800000u, 0x100446ffu, 0x47800000u, 0x7c023680u, 0xbf8cc07fu, 0xe0302000u,
    0x80011800u, 0xbe880b81u, 0x1e0202ffu, 0x477fff00u, 0x1e0604ffu, 0x477fff00u, 0x020100f9u, 0x86860608u,
    0x7e040f01u, 0x7e060f03u, 0x38020104u, 0xd76f0000u, 0x04092103u, 0x8afe7e02u, 0xbf88000bu, 0x7c023680u,
    0x7e0214f9u, 0x0006061eu, 0xbf8c3f70u, 0x5e00391du, 0x7e3002ffu, 0x20fffffeu, 0xbe840b81u, 0x020500f9u,
    0x86860604u, 0x38020501u, 0xbefe0402u, 0xf4100100u, 0xfa000010u, 0x7c023680u, 0x7d8a42f9u, 0x06868280u,
    0x7d8a3ef9u, 0x06869480u, 0xbf8cc07fu, 0xf0200308u, 0x00010016u, 0x0203e4f9u, 0x86860680u, 0xd5010000u,
    0x0009e480u, 0xd5010001u, 0x00520280u, 0x10060122u, 0x06000300u, 0x3e064101u, 0x7e045503u, 0x7c020680u,
    0x10000102u, 0x02000080u, 0x10020122u, 0x10000120u, 0xd5010001u, 0x000a0280u, 0xd5010002u, 0x00520080u,
    0x1e0002ffu, 0x477de800u, 0x1e0204ffu, 0x477de800u, 0xf0200308u, 0x00030016u, 0xbefe04c1u, 0xf4080100u,
    0xfa000000u, 0xd76f0019u, 0x04592117u, 0xbf8c0070u, 0xe0742000u, 0x8001181au, 0xbf810000u, 0xbf810000u,
};

void TestRayTracingClosestLeafCalls() {
  namespace D = ShaderRecompiler::Decoder;
  // User data s[0:1] point at the shader's table of V#s; the call table's V# (at +0x120, loaded
  // before the traversal loop) has 4 records of 16 bytes: two any-hit functions (one accepts the
  // hit, v7 = 0, the other returns at once) and two null slots.
  constexpr uint64_t Tables = 0x10000000u, CallTable = 0x20000000u, Functions = 0x30000000u;
  const std::array<uint32_t, 4> vsharp{static_cast<uint32_t>(CallTable), 16u << 16u, 4u, 0u};
  const auto read = [&](uint64_t address, std::span<uint32_t> words) {
    for (size_t i = 0; i < words.size(); i++) {
      const uint64_t at = address + i * 4u;
      if (at >= Tables && at < Tables + 0x1000u) {
        words[i] = vsharp[(at / 4u) % 4u];
      } else if (at >= CallTable && at < CallTable + 64u) {
        const uint64_t slot = (at - CallTable) / 16u, word = ((at - CallTable) % 16u) / 4u;
        const uint64_t target = slot < 2u ? Functions + slot * 0x100u : 0u;
        words[i] = word == 0u ? static_cast<uint32_t>(target) : word == 1u ? static_cast<uint32_t>(target >> 32u) : 0u;
      } else if (at >= Functions && at < Functions + 0x200u) {
        const uint64_t word = (at - Functions) / 4u;
        words[i] = word == 0u ? 0x7e0e0280u   // v_mov_b32 v7, 0
                   : word == 1u || word == 0x40u ? 0xbe80200eu // s_setpc_b64 s[14:15]
                                                  : 0xbf800000u; // s_nop 0
      } else {
        return false;
      }
    }
    return true;
  };
  std::vector<uint32_t> user_data(16, 0u);
  user_data[0] = static_cast<uint32_t>(Tables);
  D::ShaderFunctionExpander expander;
  std::vector<uint32_t> expanded;
  std::string reason;
  Check(expander.Expand(kGt7ClosestLeafWave64, 0x400000000ull, user_data, read, expanded, reason, 64u) &&
            !expanded.empty() && expander.Bodies().size() == 2u,
        ("the call table loaded before the traversal loop did not expand: " + reason).c_str());
  D::Program program;
  D::DecodeProgram(expanded, program, expander.LongBranches());
  program.code = expanded;
  program.call_bodies = expander.Bodies();
  program.runtime_literals = expander.RuntimeLiterals();
  // The walk is ClosestLeaf on the IR (IR::TraceNativeTraversals): a wave-wide closest hit walk whose leaf
  // code (with the inlined calls) decides the triangles that are not opaque. Its TLAS source and instance
  // record layout: user data s[0:1] + 0x118, root 0x25; records of 128 bytes at +0x30, the BLAS address
  // packed in dwords 12 and 13, the root in dword 14, the mask in dword 15 from bit 24, the flags in dword
  // 13 from bit 22 (not flipped), inline instances of type 7 at +0x80 (root 0x25).
  ShaderRecompiler::IR::RayTracingLayout layout;
  layout.valid = true;
  layout.instance_offset = 0x30;
  layout.record_stride = 128;
  for (uint8_t i = 0; i < 12; i++) {
    layout.transform[i] = i;
  }
  layout.blas_dword = 12;
  layout.blas_packed = true;
  layout.root_dword = 14;
  layout.mask_dword = 15;
  layout.mask_shift = 24;
  layout.flags_dword = 13;
  layout.flags_shift = 22;
  layout.inline_type = 7;
  layout.inline_offset = 0x80;
  layout.inline_root = 0x25;
  layout.flip_facing = false;
  // Only the instrumented module ballots lanes for the native and fallback counters.
  for (const bool stats : {false, true}) {
    auto options = MakeCompileOptions(ShaderType::Compute);
    options.wave_size = 64;
    options.native_ray_tracing = true;
    options.ray_stats = stats;
    auto translated = ShaderRecompiler::TranslateProgram(program, options);
    const auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
    ShaderRecompiler::IR::ResourceSpecialization specialization;
    specialization.buffers.assign(plan.info.buffers.size(), {});
    specialization.images.assign(plan.info.images.size(), {});
    for (size_t i = 0; i < plan.info.images.size(); i++) {
      specialization.images[i].numeric_class = Prospero::TextureNumericClass::Float;
      specialization.images[i].dimension = plan.info.images[i].dimension != D::ImageDimension::Unknown
                                               ? plan.info.images[i].dimension
                                               : D::ImageDimension::Dim2D;
    }
    specialization.samplers.assign(plan.info.samplers.size(), {});
    const auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
    const auto &info = compiled.program.info;
    Check(info.ray_query && info.ray_query_sites == 1 && info.ray_query_user_sgpr == 0 &&
              info.ray_query_header_offset == 0x118 && info.ray_query_root == 0x25 &&
              info.ray_query_layout == layout.Pack() && SpirvInstructionOpcodeCount(compiled.spirv, 4473u) != 0,
          "the closest hit traversal with any-hit calls does not trace with ray queries, or has the wrong TLAS "
          "source or instance record layout");
    // OpGroupNonUniformBallotBitCount
    Check((SpirvInstructionOpcodeCount(compiled.spirv, 342u) != 0) == stats,
          stats ? "a traversal compiled with ray statistics does not count its guest rays"
                : "a traversal compiled without ray statistics counts its guest rays");
    CheckSpirvBinaryValidates(compiled.spirv);
  }
}

// ASTRO's PLAYROOM, cs 39db12758f92edcd (wave32): the 2020 template's ClosestWave walk (packet TLAS walk,
// wave-wide BLAS walk, leaf code that keeps the nearest triangle of the facing the instance flags select).
constexpr uint32_t kPlayroomClosestWave32[] = {
    0xbfa00003u, 0x8f6a8210u, 0xbe90040au, 0xbe92040cu, 0xf4201ac8u, 0xd4000000u, 0xbf8cc07fu, 0xbf076bc1u,
    0xbf8406a2u, 0x906a906bu, 0x996b806bu, 0xd7460017u, 0x0401086au, 0xd7460018u, 0x0405086bu, 0xf4080407u,
    0xfa000140u, 0xbf8cc07fu, 0xf4240308u, 0xfa000000u, 0xbf8cc07fu, 0x7d862ef9u, 0x06868a0cu, 0x7d86300du,
    0x8d6a6a0au, 0xbeea3c6au, 0xbf880690u, 0x7e060c0cu, 0xf40c0407u, 0xfa000060u, 0xbf8cc07fu, 0xf0000108u,
    0x00041917u, 0x7e020d17u, 0x7e040c0du, 0x7e345503u, 0x7e000d18u, 0xf4080c07u, 0xfa000120u, 0xbf8cc07fu,
    0xf4300818u, 0xfa000100u, 0x7e365502u, 0x10023501u, 0xf4300418u, 0xfa0000c0u, 0x7e100280u, 0x7e120280u,
    0x10003700u, 0xd5410001u, 0x046a02f4u, 0x7e140280u, 0x7e160280u, 0x7e200280u, 0xd5410000u, 0x046e00f4u,
    0x060402f3u, 0xf4080d07u, 0xfa000130u, 0xbe8a0380u, 0xbf8cc07fu, 0xf420031au, 0xfa00000cu, 0x060000f3u,
    0x10060420u, 0xd5410004u, 0x00be0423u, 0x10020421u, 0x100a0422u, 0xd5410002u, 0x440e0024u, 0xd5410003u,
    0x44120027u, 0xd51f0001u, 0x40020025u, 0xd51f0005u, 0x40020026u, 0xbf8c3f70u, 0x3e043228u, 0x3e06322bu,
    0x3e023229u, 0x3e0a322au, 0x0604042cu, 0x7e005503u, 0x0602022du, 0x06060a2eu, 0x10040500u, 0x10020300u,
    0x10000700u, 0xd5410004u, 0x00720410u, 0xd5410003u, 0x00760411u, 0xd5410002u, 0x007a0412u, 0xd5410027u,
    0x04120214u, 0xd5410028u, 0x040e0215u, 0x3e040216u, 0x3e4e0018u, 0x3e500019u, 0xd5410029u, 0x040a001au,
    0xbf8cc07fu, 0xbf0a0c0au, 0xbf84002bu, 0x7e000280u, 0x936aff0au, 0x00000070u, 0xf4080707u, 0xfa000150u,
    0xbf8cc07fu, 0xf428080eu, 0xd4000000u, 0xbf8cc07fu, 0x0a064e20u, 0x0a045021u, 0x0a025222u, 0x10060703u,
    0x3e060502u, 0x3e060301u, 0x7e026703u, 0x7c080223u, 0xbeeb3c6au, 0xbf88000eu, 0x936aff0au, 0x00000070u,
    0xf4080907u, 0xfa000150u, 0x816a906au, 0xbf8cc07fu, 0xf42c0712u, 0xd4000000u, 0xbf8cc07fu, 0x0a000220u,
    0x100000f9u, 0x06862621u, 0xd5410000u, 0x4076001du, 0xbefe036bu, 0xbe8b037cu, 0xd51f0000u, 0x20020110u,
    0xbefc030au, 0x810a810au, 0x06200110u, 0x7e108500u, 0xbefc030bu, 0xbf82ffd2u, 0x060032ffu, 0x358637bdu,
    0xf40c0707u, 0xfa000020u, 0xbf8cc07fu, 0xf0000f08u, 0x00070c17u, 0xf40c0a07u, 0xfa000000u, 0xbf8cc07fu,
    0xf0000f08u, 0x000a0417u, 0xf4080907u, 0xfa000120u, 0xbf8cc07fu, 0xf4280812u, 0xfa000190u, 0x7e005500u,
    0xbf8cc07fu, 0x083c4e20u, 0x08385021u, 0x083a5222u, 0x7e2802ffu, 0xbf933333u, 0xbeb00402u, 0x10263d00u,
    0x10223900u, 0x10243b00u, 0xbeb20404u, 0xbeb40406u, 0xbeb60408u, 0xf4080107u, 0xfa000130u, 0xf0000f08u,
    0x000c0017u, 0xbf8cc07fu, 0xf4201a82u, 0xfa00001cu, 0xbe8203ffu, 0xc0400000u, 0xbe8303ffu, 0x41200000u,
    0xbe8803ffu, 0x3e99999au, 0xbf8cc07fu, 0xbf08806au, 0x7e545510u, 0x7e4a0280u, 0x7e480280u, 0x7e460280u,
    0x7e420280u, 0x7e400280u, 0x7e3e0280u, 0x856b807eu, 0xbeb80380u, 0xbf8c3f72u, 0x102c1810u, 0x10201811u,
    0xbf8c3f71u, 0x204c0affu, 0x3c23d70au, 0x102a1812u, 0x10181effu, 0x47800000u, 0x3e2c1a14u, 0xd5410022u,
    0x04402b0du, 0x7c080affu, 0x3ecccccdu, 0xd541002cu, 0x04542d0du, 0x7e1a4f26u, 0xd541002bu, 0x045a1c18u,
    0x3e441c19u, 0x7e18110cu, 0x3e581c1au, 0x421c1b14u, 0x40400000u, 0x101a272bu, 0xd541802du, 0x000a4c03u,
    0x30181888u, 0xbf8c3f70u, 0x7c0206f9u, 0x0686a280u, 0x081c1cffu, 0x41100000u, 0xd541002eu, 0x04362322u,
    0x7c020af9u, 0x0686a008u, 0x7e180b0cu, 0x871f6a6bu, 0xd557000fu, 0x03fd010eu, 0x40d00000u, 0x3e5c5912u,
    0x105e18ffu, 0x3b808081u, 0x101c5cf5u, 0xd5410013u, 0xa44e1d2bu, 0xd5410011u, 0xa4461d22u, 0xd5410012u,
    0xa44a1d2cu, 0x10202713u, 0x3e202311u, 0x3e202512u, 0x7e205d10u, 0x10602710u, 0x10622310u, 0x10642510u,
    0x40664f30u, 0x3a83126fu, 0x40685131u, 0x3a83126fu, 0x406a5332u, 0x3a83126fu, 0x7e6c5530u, 0x7e6e5531u,
    0x7e705532u, 0xbf0a0c38u, 0xbf84049cu, 0xbe82037cu, 0xbefc0338u, 0x7e288708u, 0xbefc0302u, 0x7c022880u,
    0xbea13c6au, 0xbf880492u, 0x7e7c0280u, 0x7e2a0330u, 0x7e2c0331u, 0x7e780332u, 0xbea33c1fu, 0xbf8803eeu,
    0x7e1802ffu, 0x501502f9u, 0xf4040080u, 0xfa000000u, 0xbf8cc07fu, 0xf4041a81u, 0xfa000050u, 0xbf8cc07fu,
    0x8004c16au, 0x7e2a02c1u, 0x8205c16bu, 0xf4041a81u, 0xfa000028u, 0x9492ff04u, 0x002a0000u, 0xbea40380u,
    0x8813ff13u, 0x81000000u, 0x9490ff02u, 0x00280008u, 0xbf8cc07fu, 0x80256a02u, 0xbe8903a5u, 0x82266b03u,
    0x802aff02u, 0x0000008cu, 0xbe911d9fu, 0x822b8003u, 0xbe86037eu, 0x876b8709u, 0x816ac46bu, 0xbf0b816au,
    0xbf850218u, 0xbf0709c1u, 0x856a807eu, 0xbf076b87u, 0x856b807eu, 0x876a6a06u, 0xbea73c6au, 0xbf88020fu,
    0x876a6b7eu, 0xbe9c042au, 0xbea803a5u, 0xbf860008u, 0x906a8309u, 0x9aebff6au, 0x000000a0u, 0xb86a00a0u,
    0x801c6a25u, 0x821d6b26u, 0xf4000a0eu, 0xfa000098u, 0xf4000a4eu, 0xfa000000u, 0xbf8cc07fu, 0x876aff29u,
    0x0fffff00u, 0xbf8401fdu, 0xf40c0c0eu, 0xfa000008u, 0xf408050eu, 0xfa000040u, 0xbf8cc07fu, 0xd5410010u,
    0x00526632u, 0xd5410011u, 0x005a6634u, 0xd5410012u, 0x00566633u, 0x10266032u, 0x10786034u, 0x107a6033u,
    0xf4080b0eu, 0xfa000028u, 0xd5410016u, 0x04426836u, 0xbf8cc07fu, 0xd5410039u, 0x0446682cu, 0xd541003au,
    0x044a6837u, 0xd541003bu, 0x044e6236u, 0x3e78622cu, 0x3e7a6237u, 0xf4080618u, 0xfa000050u, 0xbf8cc07fu,
    0x8006c118u, 0xf4041a8eu, 0xfa000038u, 0x8207c119u, 0x3e2c6a2eu, 0xbf8cc07fu, 0x3e726a6au, 0x3e746a2fu,
    0x3e76642eu, 0x3e78646au, 0x3e7a642fu, 0x948aff06u, 0x002a0000u, 0xbeb2031au, 0x880bff0bu, 0x81000000u,
    0x9488ff30u, 0x00280008u, 0xbeb303ffu, 0x00016204u, 0x87028c29u, 0xbe830324u, 0xbe891d9fu, 0xbeb11d93u,
    0x7e7c553bu, 0x7e7e553du, 0x7e80553cu, 0xbe94037eu, 0x876a8728u, 0x816ac46au, 0xbf0b816au, 0xbf850037u,
    0xbf0728c1u, 0xbf840034u, 0xbe983c14u, 0xbf880031u, 0x876ac528u, 0x906b8328u, 0xf4200518u, 0xd4000000u,
    0xbf8cc07fu, 0x876a8714u, 0xbf076a87u, 0xbf840029u, 0x876ac728u, 0x816a6a84u, 0x87198402u, 0x901a8302u,
    0x811b816bu, 0xf4200558u, 0xd4000000u, 0xbf8cc07fu, 0x7e200214u, 0x926a9388u, 0xbf071980u, 0x8514807eu,
    0xf1989f07u, 0x00021010u, 0x393a160cu, 0x3e3c3d3bu, 0x0000403fu, 0xbf8c3f70u, 0x7e245511u, 0x2c22209fu,
    0x7d8a20f9u, 0x06869c6au, 0x10202510u, 0x7d84221au, 0x7c0818f9u, 0x0606a810u, 0x876a1c6au, 0x8a6b146au,
    0x876a141cu, 0x886b6a6bu, 0x8a6a6b7eu, 0x886a286au, 0xbf048015u, 0x02181910u, 0x022a2a15u, 0xbf850005u,
    0x8f6a831bu, 0x811b811bu, 0xf4240518u, 0xd4000000u, 0xbf82ffdeu, 0xbefe0318u, 0xbf82017bu, 0x7e2002c1u,
    0x7e2202c1u, 0x7e2402c1u, 0x7e2602c1u, 0xbeea3c14u, 0xbf88000au, 0x7e200228u, 0xbf800000u, 0xf1989f07u,
    0x00021010u, 0x393a160cu, 0x3e3c3d3bu, 0x0000403fu, 0xbf8c3f70u, 0x7d8a20f9u, 0x068694c1u, 0xbf071480u,
    0xbefe036au, 0xbf840168u, 0x7d8a22c1u, 0xbf076a80u, 0xbf840127u, 0x7d8a24c1u, 0xbf076a80u, 0xbf8400a7u,
    0x7d8a26c1u, 0xbf076a80u, 0xbf84002cu, 0x7d8a26c1u, 0xbf09a024u, 0xbe95136au, 0xd7600028u, 0x00002b10u,
    0xd7600018u, 0x00002b11u, 0xd760006bu, 0x00002b12u, 0xd760006au, 0x00002b13u, 0xbf840005u, 0x817cff24u,
    0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u,
    0xbf09a024u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86bu, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86bu, 0x81248124u, 0xbf09a024u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu,
    0x0000f818u, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f818u, 0x81248124u, 0xbf82ff7du, 0x7d8a24f9u,
    0x068698c1u, 0xbf09a024u, 0xbe991318u, 0xd7600028u, 0x00003310u, 0xd7600015u, 0x00003311u, 0xd760006bu,
    0x00003312u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86bu, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86bu, 0x7d8a2428u, 0x7d8a24f9u, 0x06869a15u, 0x7d8a24f9u, 0x06869c6bu, 0x876a6a18u,
    0x876a1a6au, 0x876a1c6au, 0x81248124u, 0xbf076a80u, 0xbf84001au, 0xbe98136au, 0xbf09a024u, 0xd760006au,
    0x00003112u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86au, 0x81248124u, 0xbf09a024u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu,
    0x0000f815u, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f815u, 0x81248124u, 0xbf82003fu, 0x7d8a22f9u,
    0x068698c1u, 0x7d8a2228u, 0x7d8a22f9u, 0x06869a15u, 0x7d8a22f9u, 0x06869c6bu, 0xbf09a024u, 0xbf840005u,
    0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f815u, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f815u,
    0x876a6a18u, 0x876a1a6au, 0x876a1c6au, 0x81248124u, 0xbf076a80u, 0xbf84000fu, 0xbe95136au, 0xbf09a024u,
    0xd760006au, 0x00002b11u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u,
    0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u, 0xbf820019u, 0x7d8a20f9u, 0x068698c1u, 0x7d8a2028u,
    0x7d8a20f9u, 0x06869a15u, 0x7d8a20f9u, 0x06869c6bu, 0x876a6a18u, 0x876a1a6au, 0x876a1c6au, 0xbf84000eu,
    0xbe95136au, 0xbf09a024u, 0xd760006au, 0x00002b10u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu,
    0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u, 0xbf82ff05u, 0x7d8a22f9u,
    0x068698c1u, 0xbf09a024u, 0xbe951318u, 0xd7600028u, 0x00002b10u, 0xd7600015u, 0x00002b11u, 0xbf840005u,
    0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f815u, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f815u,
    0x7d8a2228u, 0x7d8a22f9u, 0x06869a15u, 0x876a6a18u, 0x876b1a6au, 0x81248124u, 0xbf076b80u, 0xbf84003bu,
    0xbe98136bu, 0xbf09a024u, 0xd7600019u, 0x00003111u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu,
    0x0000f819u, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f819u, 0x7d8a2219u, 0x876a6b6au, 0x81248124u,
    0xbf076a80u, 0xbf84000fu, 0xbe95136au, 0xbf09a024u, 0xd760006au, 0x00002b11u, 0xbf840005u, 0x817cff24u,
    0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u,
    0xbf820042u, 0x7d8a20f9u, 0x068698c1u, 0x7d8a2028u, 0x7d8a20f9u, 0x06869a15u, 0x7d8a20f9u, 0x06869c19u,
    0x876a6a18u, 0x876a1a6au, 0x876a1c6au, 0xbf84000eu, 0xbe95136au, 0xbf09a024u, 0xd760006au, 0x00002b10u,
    0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du,
    0x0000f86au, 0x81248124u, 0xbf820028u, 0x7d8a20f9u, 0x068698c1u, 0x7d8a2028u, 0x7d8a20f9u, 0x06869a15u,
    0x876a6a18u, 0x876b1a6au, 0xbf840020u, 0xbe95136bu, 0xbf09a024u, 0xd760006au, 0x00002b10u, 0xbf840005u,
    0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au,
    0x7d8a206au, 0x876a6b6au, 0x81248124u, 0xbf076a80u, 0xbf84000eu, 0xbe95136au, 0xbf09a024u, 0xd760006au,
    0x00002b10u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86au, 0x81248124u, 0xbf82fe88u, 0x7d8a20f9u, 0x068698c1u, 0xbea803c1u, 0xbf071880u,
    0xbf840038u, 0xbe951318u, 0xd7600028u, 0x00002b10u, 0x7d8a2028u, 0x876b6a18u, 0xbf840032u, 0xbe95136bu,
    0xbf09a024u, 0xd760006au, 0x00002b10u, 0x7d8a20f9u, 0x0686986au, 0xbf840005u, 0x817cff24u, 0xffffffe0u,
    0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u, 0x876b6b18u,
    0xbf840020u, 0xbe95136bu, 0xbf09a024u, 0xd760006au, 0x00002b10u, 0x7d8a20f9u, 0x0686986au, 0xbf840005u,
    0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au,
    0x81248124u, 0x876a6b18u, 0xbf84000eu, 0xbe95136au, 0xbf09a024u, 0xd760006au, 0x00002b10u, 0xbf840005u,
    0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au,
    0x81248124u, 0xbf82fe4au, 0xbf070324u, 0xbf84000bu, 0x8124c124u, 0xbf09a024u, 0xbf840005u, 0x8114ff24u,
    0xffffffe0u, 0xd7600028u, 0x0000290eu, 0xbf82fe3cu, 0xd7600028u, 0x0000490du, 0xbf82fe39u, 0xbefe0327u,
    0xbf82017bu, 0x7e2002c1u, 0x7e2202c1u, 0x7e2402c1u, 0x7e2602c1u, 0xbeea3c06u, 0xbf88000au, 0x7e200209u,
    0xbf800000u, 0xf1989f07u, 0x00041010u, 0x3534330cu, 0x36323130u, 0x00003837u, 0xbf8c3f70u, 0x7d8a20f9u,
    0x068686c1u, 0xbf070680u, 0xbefe036au, 0xbf840168u, 0x7d8a22c1u, 0xbf076a80u, 0xbf840127u, 0x7d8a24c1u,
    0xbf076a80u, 0xbf8400a7u, 0x7d8a26c1u, 0xbf076a80u, 0xbf84002cu, 0x7d8a26c1u, 0xbf09a024u, 0xbe85136au,
    0xd7600009u, 0x00000b10u, 0xd7600007u, 0x00000b11u, 0xd760006bu, 0x00000b12u, 0xd760006au, 0x00000b13u,
    0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du,
    0x0000f86au, 0x81248124u, 0xbf09a024u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86bu,
    0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86bu, 0x81248124u, 0xbf09a024u, 0xbf840005u, 0x817cff24u,
    0xffffffe0u, 0xd761000eu, 0x0000f807u, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f807u, 0x81248124u,
    0xbf82fd9cu, 0x7d8a24f9u, 0x068688c1u, 0xbf09a024u, 0xbe851308u, 0xd7600009u, 0x00000b10u, 0xd760006bu,
    0x00000b11u, 0xd7600005u, 0x00000b12u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f805u,
    0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f805u, 0x7d8a2409u, 0x7d8a24f9u, 0x06868a6bu, 0x7d8a24f9u,
    0x06869405u, 0x876a6a08u, 0x876a0a6au, 0x876a146au, 0x81248124u, 0xbf076a80u, 0xbf84001au, 0xbe85136au,
    0xbf09a024u, 0xd760006au, 0x00000b12u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au,
    0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u, 0xbf09a024u, 0xbf840005u, 0x817cff24u,
    0xffffffe0u, 0xd761000eu, 0x0000f86bu, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86bu, 0x81248124u,
    0xbf82003fu, 0x7d8a22f9u, 0x068688c1u, 0x7d8a2209u, 0x7d8a22f9u, 0x06868a6bu, 0x7d8a22f9u, 0x06869405u,
    0xbf09a024u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86bu, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86bu, 0x876a6a08u, 0x876a0a6au, 0x876a146au, 0x81248124u, 0xbf076a80u, 0xbf84000fu,
    0xbe85136au, 0xbf09a024u, 0xd760006au, 0x00000b11u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu,
    0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u, 0xbf820019u, 0x7d8a20f9u,
    0x068688c1u, 0x7d8a2009u, 0x7d8a20f9u, 0x06868a6bu, 0x7d8a20f9u, 0x06869405u, 0x876a6a08u, 0x876a0a6au,
    0x876a146au, 0xbf84000eu, 0xbe85136au, 0xbf09a024u, 0xd760006au, 0x00000b10u, 0xbf840005u, 0x817cff24u,
    0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u,
    0xbf82fd24u, 0x7d8a22f9u, 0x068688c1u, 0xbf09a024u, 0xbe851308u, 0xd7600009u, 0x00000b10u, 0xd7600005u,
    0x00000b11u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f805u, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f805u, 0x7d8a2209u, 0x7d8a22f9u, 0x06868a05u, 0x876a6a08u, 0x876b0a6au, 0x81248124u,
    0xbf076b80u, 0xbf84003bu, 0xbe87136bu, 0xbf09a024u, 0xd7600007u, 0x00000f11u, 0xbf840005u, 0x817cff24u,
    0xffffffe0u, 0xd761000eu, 0x0000f807u, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f807u, 0x7d8a2207u,
    0x876a6b6au, 0x81248124u, 0xbf076a80u, 0xbf84000fu, 0xbe85136au, 0xbf09a024u, 0xd760006au, 0x00000b11u,
    0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du,
    0x0000f86au, 0x81248124u, 0xbf820042u, 0x7d8a20f9u, 0x068688c1u, 0x7d8a2009u, 0x7d8a20f9u, 0x06868a05u,
    0x7d8a20f9u, 0x06869407u, 0x876a6a08u, 0x876a0a6au, 0x876a146au, 0xbf84000eu, 0xbe85136au, 0xbf09a024u,
    0xd760006au, 0x00000b10u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u,
    0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u, 0xbf820028u, 0x7d8a20f9u, 0x068688c1u, 0x7d8a2009u,
    0x7d8a20f9u, 0x06868a05u, 0x876a6a08u, 0x876b0a6au, 0xbf840020u, 0xbe85136bu, 0xbf09a024u, 0xd760006au,
    0x00000b10u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86au, 0x7d8a206au, 0x876a6b6au, 0x81248124u, 0xbf076a80u, 0xbf84000eu, 0xbe85136au,
    0xbf09a024u, 0xd760006au, 0x00000b10u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au,
    0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au, 0x81248124u, 0xbf82fca7u, 0x7d8a20f9u, 0x068688c1u,
    0xbe8903c1u, 0xbf070880u, 0xbf840038u, 0xbe841308u, 0xd7600009u, 0x00000910u, 0x7d8a2009u, 0x876b6a08u,
    0xbf840032u, 0xbe85136bu, 0xbf09a024u, 0xd760006au, 0x00000b10u, 0x7d8a20f9u, 0x0686886au, 0xbf840005u,
    0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u, 0xd761000du, 0x0000f86au,
    0x81248124u, 0x876b6b08u, 0xbf840020u, 0xbe85136bu, 0xbf09a024u, 0xd760006au, 0x00000b10u, 0x7d8a20f9u,
    0x0686886au, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86au, 0x81248124u, 0x876a6b08u, 0xbf84000eu, 0xbe85136au, 0xbf09a024u, 0xd760006au,
    0x00000b10u, 0xbf840005u, 0x817cff24u, 0xffffffe0u, 0xd761000eu, 0x0000f86au, 0xbf820003u, 0xbefc0324u,
    0xd761000du, 0x0000f86au, 0x81248124u, 0xbf82fc69u, 0xbf072480u, 0xbf84000bu, 0x8124c124u, 0xbf09a024u,
    0xbf840005u, 0x8104ff24u, 0xffffffe0u, 0xd7600009u, 0x0000090eu, 0xbf82fc5eu, 0xd7600009u, 0x0000490du,
    0xbf82fc5bu, 0x7d842ac1u, 0x361a2affu, 0x3fffffffu, 0xbeea3c6au, 0xbf880001u, 0x7e1a02c1u, 0xbefe036au,
    0x7e7c0280u, 0x7e2a0330u, 0x7e2c0331u, 0x7e780332u, 0x7daa1ac1u, 0xbf880020u, 0xd5410010u, 0x049e610cu,
    0x936aff38u, 0x00000070u, 0xf4080107u, 0xfa000150u, 0xbf8cc07fu, 0xf4280202u, 0xd4000000u, 0xd541000du,
    0x04a2630cu, 0xd541000cu, 0x04a6650cu, 0xbf8cc07fu, 0x7e240208u, 0x7e220209u, 0x7e1c020au, 0x7e7c0281u,
    0x402a2112u, 0xbc23d70au, 0x402c1b11u, 0xbc23d70au, 0x4078190eu, 0xbc23d70au, 0xbeea3c20u, 0xbf880006u,
    0x08182b30u, 0x081a2d31u, 0x081c7932u, 0x3e2a5b0cu, 0x3e2c5b0du, 0x3e785b0eu, 0x936a9038u, 0xbefe0323u,
    0xf4080107u, 0xfa000160u, 0xbf8cc07fu, 0xf4201a82u, 0xd4000000u, 0xbf8cc07fu, 0x7e180a6au, 0x7e720280u,
    0x7e740280u, 0x7e760280u, 0x107a18ffu, 0x41000000u, 0xbeea3c22u, 0xbf88001cu, 0xd547000eu, 0xe4b2452bu,
    0xd545000du, 0xe4b2452bu, 0xd546000cu, 0xe4b2452bu, 0xd5440010u, 0xe4b2452bu, 0x7e2602ffu, 0x40d00000u,
    0x7e1c54f9u, 0x0026060eu, 0xf4080407u, 0xfa000110u, 0xf40c0107u, 0xfa0000c0u, 0x06247b10u, 0x42201d0du,
    0x3fc00000u, 0x42221d0cu, 0x3fc00000u, 0xbf8cc07fu, 0xf0900718u, 0x00810c10u, 0xbf8c3f70u, 0x10721903u,
    0x10741b03u, 0x10761d03u, 0xbefe036au, 0x936bff38u, 0x00000070u, 0x7d8a7c80u, 0xf4080107u, 0xfa000150u,
    0xbf8cc07fu, 0xf4280202u, 0xd6000000u, 0xbf8cc07fu, 0x022100f9u, 0x8686060bu, 0x7c022080u, 0xbeea3c6au,
    0xbf88001du, 0x081a4e08u, 0x08245009u, 0x0822520au, 0x0a1c4e08u, 0x0a185009u, 0x10261b0du, 0x107c1b15u,
    0x0a1a520au, 0x3e262512u, 0x3e7c2d12u, 0xd5410012u, 0x044e2311u, 0x3e7c7911u, 0xd5410012u, 0x844a2110u,
    0xd5410412u, 0x044a7d3eu, 0x7e206712u, 0x06207d10u, 0x3e1c2b10u, 0x3e182d10u, 0x3e1a7910u, 0x10201d0eu,
    0x3e20190cu, 0x3e201b0du, 0x7e205d10u, 0x102a1d10u, 0x102c1910u, 0x10781b10u, 0xbefe036au, 0xf4080407u,
    0xfa000110u, 0xd5470011u, 0x04f22d15u, 0xd5470010u, 0x04b2452bu, 0xd545000cu, 0x04f22d15u, 0xd546000du,
    0x04f22d15u, 0xd544000eu, 0x04f22d15u, 0x7e2a54f9u, 0x00260611u, 0x7e2254f9u, 0x00260610u, 0xd5450010u,
    0x04b2452bu, 0xd5460013u, 0x04b2452bu, 0xd5440012u, 0x04b2452bu, 0x42182b0cu, 0x3fc00000u, 0x421a2b0du,
    0x3fc00000u, 0x061c7b0eu, 0x42202310u, 0x3fc00000u, 0x42222313u, 0x3fc00000u, 0x06247b12u, 0x7e2602ffu,
    0x40d00000u, 0xf40c0107u, 0xfa0000c0u, 0x1078292au, 0xbf8cc07fu, 0xf0900718u, 0x0081140cu, 0x936aff38u,
    0x00000070u, 0xf0900718u, 0x00810c10u, 0x816a906au, 0xf4080507u, 0xfa000150u, 0xbf8cc07fu, 0xf42c010au,
    0xd4000000u, 0xbf8cc07fu, 0x10247804u, 0x10202407u, 0x1022240au, 0x1024240bu, 0xbf8c3f71u, 0x10265f14u,
    0x10285f15u, 0x102a5f16u, 0xbf8c3f70u, 0x0618730cu, 0x061a750du, 0x061c770eu, 0x3e4a2710u, 0x3e482911u,
    0x3e462b12u, 0x3e42210cu, 0x3e40230du, 0x3e3e250eu, 0xbefe0321u, 0x81388138u, 0xbf82fb62u, 0x4a123081u,
    0x816ac10du, 0x7d8a30f9u, 0x06868880u, 0x7d8a306au, 0x4a0630c1u, 0x20105cf9u, 0x06862680u, 0x10224cffu,
    0xbce147aeu, 0x061802ffu, 0xbd4cccccu, 0x02121318u, 0xd501000au, 0x00220718u, 0xf40c0007u, 0xfa000060u,
    0x101610ffu, 0xc1147ae1u, 0x08064cf2u, 0xbe8c03ffu, 0x3d350b0fu, 0xbf8cc07fu, 0xf000010au, 0x00000917u,
    0x00000009u, 0xf000010au, 0x00000817u, 0x0000000au, 0xf4080207u, 0xfa000120u, 0x7e164b0bu, 0x101a0703u,
    0xbeeb03ffu, 0xbd350b0fu, 0xbf8cc07fu, 0xf4240104u, 0xfa0001a0u, 0xbf8cc07fu, 0x0a1c3204u, 0x4220090cu,
    0x3d4cccccu, 0x1e1a1b0bu, 0x061804ffu, 0xbd4cccccu, 0x061600ffu, 0xbd4cccccu, 0x7e1e550eu, 0xbeea03ffu,
    0x42480000u, 0x3e22070du, 0x10062107u, 0x4220090cu, 0x3d4cccccu, 0x10141ef9u, 0x06960605u, 0x421e090bu,
    0x3d4cccccu, 0xd54b000du, 0x0031ff11u, 0x3f851eb8u, 0xd54b000cu, 0x01adff11u, 0xbf851eb8u, 0x101606f9u,
    0x0686266au, 0x10202107u, 0x10221f07u, 0x400e1b26u, 0x3cb43958u, 0x401c1926u, 0xbf126e98u, 0xf4080407u,
    0xfa000140u, 0xbf8cc07fu, 0xf4201ac8u, 0xfa000020u, 0xbf8cc07fu, 0xbf076b80u, 0x06180effu, 0xbd23d70au,
    0x060e1cffu, 0x3f851eb8u, 0x101c170cu, 0xbf8c3f71u, 0x0a1a1204u, 0xbf8c3f70u, 0x0a101004u, 0xd541000fu,
    0x043a0f03u, 0xd541000bu, 0x043a0f11u, 0x7e18550du, 0x7e1a5508u, 0x3e1c2107u, 0x10241725u, 0xd5410008u,
    0x04280b0cu, 0x3e141a05u, 0x10161f24u, 0x100e1d23u, 0x1e1010f9u, 0x2626060au, 0x7c0210f0u, 0x021fe4f9u,
    0x86060606u, 0x100c1f0fu, 0x10180d12u, 0x101a0d0bu, 0x101c0d07u, 0xbf840009u, 0xf40c0007u, 0xfa000080u,
    0xbf8cc07fu, 0xf0000708u, 0x00000617u, 0xbf8c3f70u, 0x3e180906u, 0x3e1a0907u, 0x3e1c0908u, 0xf4080007u,
    0xfa000140u, 0xbf8cc07fu, 0xf4201a80u, 0xfa000030u, 0xbf8cc07fu, 0xbf076a80u, 0xbf840048u, 0x100e0affu,
    0x40a00000u, 0x7e0c0b17u, 0x7e0a0b18u, 0xf4080207u, 0xfa000100u, 0xf40c0007u, 0xfa0000a0u, 0xd557000bu,
    0x03fd0107u, 0x40a00000u, 0x10123506u, 0x10143705u, 0xbeea03ffu, 0x3f7d70a4u, 0x7e224ef9u, 0x00260611u,
    0x7c02086au, 0x202444f9u, 0x06862680u, 0xbf8cc07fu, 0xf0900f08u, 0x00400509u, 0x7e064ef9u, 0x00260603u,
    0x7e124ef9u, 0x00260610u, 0xf4080407u, 0xfa000140u, 0x10142512u, 0xbf8cc07fu, 0xf4201ac8u, 0xfa000040u,
    0xbf8cc07fu, 0x7c09e4f9u, 0x8686846bu, 0x022014f2u, 0xf4201a88u, 0xfa000050u, 0xbf8cc07fu, 0x1014226au,
    0x1022066au, 0x1016126au, 0x2006d6f9u, 0x86862680u, 0x7e124af9u, 0x0006260au, 0x7e144af9u, 0x00062611u,
    0x7e164af9u, 0x0006260bu, 0xbf8c3f70u, 0x10260a6bu, 0x10240c6bu, 0x10220e6bu, 0x10102108u, 0xd5010005u,
    0x00122705u, 0xd5010006u, 0x00122506u, 0xd5010007u, 0x00122307u, 0x10060708u, 0xd5410005u, 0x84321305u,
    0xd5410006u, 0x84361506u, 0xd5410007u, 0x843a1707u, 0x3e180705u, 0x3e1a0706u, 0x3e1c0707u, 0x080608f2u,
    0xf40c0007u, 0xfa000040u, 0xbf8cc07fu, 0xf0000708u, 0x00000417u, 0x100e430fu, 0x1010410fu, 0x10000700u,
    0x10020701u, 0x10040702u, 0x10123f0fu, 0x10063d1eu, 0xf4080207u, 0xfa000130u, 0xbf8cc07fu, 0xf4201a84u,
    0xfa00002cu, 0x7c0632f9u, 0x068688f2u, 0xf40c0407u, 0xfa0000e0u, 0x3e06391cu, 0x3e063b1du, 0x7e066703u,
    0xd5010003u, 0x00210103u, 0xbf8c3f70u, 0x3e080f00u, 0x3e0a1101u, 0x3e0c1302u, 0xbf8cc07fu, 0x3e08186au,
    0x3e0a1a6au, 0x3e0c1c6au, 0xd5570000u, 0x020208ffu, 0x42c80000u, 0xd5570001u, 0x02020affu, 0x42c80000u,
    0xd5570002u, 0x02020cffu, 0x42c80000u, 0xd5010000u, 0x00210100u, 0xd5010001u, 0x00210101u, 0xd5010002u,
    0x00210102u, 0xf0200f08u, 0x00040017u, 0xbf810000u,
};

// The IR's ClosestWave form (IR::TraceNativeTraversals) over a real program: one site, the TLAS source and
// instance record layout of the 2020 template (records of 160 bytes, inline instances of type 7 at +0x80,
// root 0x25), ray queries in the SPIR-V, which validates.
void TestRayTracingClosestWave() {
  namespace D = ShaderRecompiler::Decoder;
  D::Program program;
  D::DecodeProgram(kPlayroomClosestWave32, program);
  auto options = MakeCompileOptions(ShaderType::Compute);
  options.wave_size = 32;
  options.native_ray_tracing = true;
  static ShaderComputeInputInfo compute{};
  compute.scratch_size_dwords = 16384;
  compute.lds_size_dwords = 16384;
  compute.wave_size = 32;
  compute.thread_ids_num = 3;
  compute.threads_num[0] = 64;
  compute.threads_num[1] = compute.threads_num[2] = 1;
  options.input_info.compute = &compute;
  auto translated = ShaderRecompiler::TranslateProgram(program, options);
  const auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
  ShaderRecompiler::IR::ResourceSpecialization specialization;
  specialization.buffers.assign(plan.info.buffers.size(), {});
  specialization.images.assign(plan.info.images.size(), {});
  for (auto &image : specialization.images) {
    image.numeric_class = Prospero::TextureNumericClass::Float;
    image.dimension = D::ImageDimension::Dim2D;
  }
  specialization.samplers.assign(plan.info.samplers.size(), {});
  const auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
  const auto &info = compiled.program.info;
  Check(info.ray_query && info.ray_query_sites == 1 && info.ray_query_user_sgpr == 0 &&
            info.ray_query_header_offset == 0 && info.ray_query_root == 0x25 &&
            info.ray_query_layout == std::array<uint32_t, 4>{0xd951c840u, 0x4c25ea62u, 0x28482080u, 0x308f2511u} &&
            SpirvInstructionOpcodeCount(compiled.spirv, 4473u) != 0,
        "the wave-wide closest hit walk of ASTRO's PLAYROOM does not trace with ray queries, or has the wrong TLAS "
        "source or instance record layout");
  CheckSpirvBinaryValidates(compiled.spirv);
}

#include "ShaderRayTracingPrograms.inc"

// What the IR's native walks (IR::TraceNativeTraversals) take of a real program compiled as the game compiles it
// with --native-rt: the number of sites, the TLAS source, the instance record layout and the ranges of the guest
// code that stays in the BVH (ray_query_reach), and the SPIR-V validates.
struct NativeWalks {
  const char *name = "";
  std::span<const uint32_t> code;
  uint32_t wave_size = 64;
  uint32_t sites = 1;
  uint32_t user_sgpr = 0;
  uint32_t header_offset = 0;
  uint32_t header_vsharp = ShaderRecompiler::IR::ShaderInfo::NoRayQuerySlot;
  uint32_t root = 0x25;
  std::array<uint32_t, 4> layout{};
  size_t reach = 0;
  // (Not for a pixel shader: its exports are not valid SPIR-V in the compute stage the walks are traced in.)
  bool validates = true;
  bool check_ray_stats = false;
};

void CheckNativeWalks(const NativeWalks &expected) {
  namespace D = ShaderRecompiler::Decoder;
  auto options = MakeCompileOptions(ShaderType::Compute);
  options.wave_size = expected.wave_size;
  options.native_ray_tracing = true;
  static const ShaderRecompiler::Spirv::Profile profile = [] {
    auto with_ray_query = ShaderRecompiler::Spirv::DefaultProfile;
    with_ray_query.ray_query = true;
    return with_ray_query;
  }();
  options.profile = &profile;
  static ShaderComputeInputInfo compute{};
  compute.scratch_size_dwords = 16384;
  compute.lds_size_dwords = 16384;
  compute.wave_size = expected.wave_size;
  compute.thread_ids_num = 3;
  compute.threads_num[0] = 64;
  compute.threads_num[1] = compute.threads_num[2] = 1;
  options.input_info.compute = &compute;
  auto translated = ShaderRecompiler::TranslateProgram(expected.code, options);
  const auto reach = translated.program.info.ray_query_reach.size();
  const auto plan = ShaderRecompiler::IR::ExtractResourcePlan(translated.program);
  ShaderRecompiler::IR::ResourceSpecialization specialization;
  specialization.buffers.assign(plan.info.buffers.size(), {});
  specialization.images.assign(plan.info.images.size(), {});
  for (auto &image : specialization.images) {
    image.numeric_class = Prospero::TextureNumericClass::Float;
    image.dimension = D::ImageDimension::Dim2D;
  }
  specialization.samplers.assign(plan.info.samplers.size(), {});
  const auto compiled = ShaderRecompiler::CompileProgram(std::move(translated), options, specialization);
  const auto &info = compiled.program.info;
  const bool traced = info.ray_query && info.ray_query_sites == expected.sites &&
                      info.ray_query_user_sgpr == expected.user_sgpr &&
                      info.ray_query_header_offset == expected.header_offset &&
                      info.ray_query_header_vsharp == expected.header_vsharp && info.ray_query_root == expected.root &&
                      info.ray_query_layout == expected.layout && reach == expected.reach &&
                      SpirvInstructionOpcodeCount(compiled.spirv, 4473u) != 0;
  if (!traced) {
    std::fprintf(stderr,
                 "%s: %u sites, TLAS header at user data s%u + 0x%x (V# 0x%x), root 0x%x, layout %08x %08x %08x %08x, "
                 "%zu reach entries\n",
                 expected.name, info.ray_query_sites, info.ray_query_user_sgpr, info.ray_query_header_offset,
                 info.ray_query_header_vsharp, info.ray_query_root, info.ray_query_layout[0], info.ray_query_layout[1],
                 info.ray_query_layout[2], info.ray_query_layout[3], reach);
  }
  Check(traced, (std::string("the BVH walks of ") + expected.name +
                 " do not trace with ray queries, or have the wrong TLAS source, instance record layout or reach")
                    .c_str());
  if (expected.validates) {
    CheckSpirvBinaryValidates(compiled.spirv);
  }
  if (expected.check_ray_stats) {
    options.ray_stats = true;
    auto instrumented = ShaderRecompiler::CompileProgram(
        ShaderRecompiler::TranslateProgram(expected.code, options), options, specialization);
    constexpr uint32_t ray_query_initialize = 4473u, atomic_add = 234u, ballot_count = 342u;
    Check(SpirvInstructionOpcodeCount(instrumented.spirv, ray_query_initialize) ==
              SpirvInstructionOpcodeCount(compiled.spirv, ray_query_initialize) &&
              SpirvInstructionOpcodeCount(instrumented.spirv, atomic_add) >
                  SpirvInstructionOpcodeCount(compiled.spirv, atomic_add) &&
              SpirvInstructionOpcodeCount(instrumented.spirv, ballot_count) >
                  SpirvInstructionOpcodeCount(compiled.spirv, ballot_count),
          "ray statistics changed the query sites or did not count their lanes");
    CheckSpirvBinaryValidates(instrumented.spirv);
    std::printf("%s: instrumented shader validated\n", expected.name);
  }
}

// The native walks of real programs of each form, with the sites, TLAS sources and instance record layouts the
// decoded patch derived for them: a change of the ray query pass that leaves one of them to the guest's traversal
// fails here.
void TestRayTracingNativeWalks() {
  CheckNativeWalks({.name = "GTA V's cs 2f34bc2dfe6b04a3",
                    .code = kGtaFidelityPacketLeaf64,
                    .sites = 3,
                    .user_sgpr = 4,
                    .header_offset = 0x10,
                    .layout = {0x76543210u, 0x1dc6ba98u, 0x000dbb8fu, 0u},
                    .reach = 1,
                    .check_ray_stats = true});
  CheckNativeWalks({.name = "GTA V's cs 0b41af19bddf132c",
                    .code = kGtaFidelityPacketAlpha64,
                    .sites = 3,
                    .user_sgpr = 4,
                    .header_offset = 0x10,
                    .layout = {0x76543210u, 0x3dc6ba98u, 0x000dbb8fu, 0u},
                    .reach = 3});
  CheckNativeWalks({.name = "GTA V's cs 48d704640ab9fb40",
                    .code = kGtaPerformanceRtPacketLane64,
                    .sites = 3,
                    .user_sgpr = 4,
                    .header_offset = 0x10,
                    .layout = {0x76543210u, 0x3dc6ba98u, 0x000dbb8fu, 0u},
                    .reach = 3,
                    .check_ray_stats = true});
  CheckNativeWalks({.name = "GTA V's cs 84ed00053b3771da",
                    .code = kGtaShortRaysClosestOpaque64,
                    .user_sgpr = 4,
                    .header_offset = 0x70,
                    .layout = {0xc963b852u, 0x3c05da74u, 0u, 0u},
                    .reach = 1});
  CheckNativeWalks({.name = "GTA V's cs 6aeb221de7ff018e",
                    .code = kGtaReflectionsClosestAlpha64,
                    .user_sgpr = 4,
                    .header_offset = 0x40,
                    .layout = {0x76543210u, 0x3dc6ba98u, 0x0008018fu, 0u},
                    .reach = 3});
  CheckNativeWalks({.name = "Resident Evil Village's cs be0b848d2e06ec9b",
                    .code = kRe8ClosestLaneEntry64,
                    .user_sgpr = 2,
                    .header_offset = 0x60,
                    .layout = {0xc963b852u, 0x3c05da74u, 0x00083eafu, 0x20000000u}});
  CheckNativeWalks({.name = "Marvel's Wolverine's cs b790bfb6f776e13a",
                    .code = kWolverineAnyLane32,
                    .wave_size = 32,
                    .user_sgpr = 12,
                    .header_offset = 0x430,
                    .header_vsharp = 0x50,
                    .layout = {0x76543210u, 0x3dc6ba98u, 0x000801cfu, 0u},
                    .reach = 1});
  CheckNativeWalks({.name = "ASTRO BOT's ps 9833d01b08e21d08",
                    .code = kAstroUniformOrigin64,
                    .user_sgpr = 0,
                    .header_offset = 0x28,
                    .layout = {0x76543210u, 0x1dc6ba98u, 0x000dbb8fu, 0x000f2510u},
                    .validates = false});
  CheckNativeWalks({.name = "ASTRO's PLAYROOM's ps 29289be6a0202b6d",
                    .code = kPlayroomUniformOrigin64,
                    .user_sgpr = 0,
                    .header_offset = 0x28,
                    .layout = {0xd951c840u, 0x4c25ea62u, 0x30482080u, 0x308f2511u},
                    .validates = false});
  CheckNativeWalks({.name = "SAROS's cs 0697eb9d76b3160b",
                    .code = kSarosClosestLane32,
                    .wave_size = 32,
                    .user_sgpr = 8,
                    .header_offset = ShaderRecompiler::IR::ShaderInfo::UserDataRayQueryHeader,
                    .layout = {0x76543210u, 0x3dc6ba98u, 0x0008018fu, 0u},
                    .reach = 1});
  CheckNativeWalks({.name = "SAROS's cs 0035a11651ca7301",
                    .code = kSarosFlatAnyHit32,
                    .wave_size = 32,
                    .user_sgpr = 0,
                    .header_offset = ShaderRecompiler::IR::ShaderInfo::UserDataRayQueryHeader,
                    .layout = {0x76543210u, 0x3dc6ba98u, 0x0008018fu, 0u},
                    .reach = 1});
  CheckNativeWalks({.name = "SAROS's cs 192d8cd7fb8773d8",
                    .code = kSarosFlatClosest32,
                    .wave_size = 32,
                    .user_sgpr = 0,
                    .header_offset = ShaderRecompiler::IR::ShaderInfo::UserDataRayQueryHeader,
                    .layout = {0x76543210u, 0x1dc6ba98u, 0x000dbb8fu, 0u},
                    .reach = 1});
  CheckNativeWalks({.name = "SAROS's cs 2fa6b5a3b15e54d4",
                    .code = kSarosWaveCopies32,
                    .wave_size = 32,
                    .sites = 7,
                    .user_sgpr = 8,
                    .header_offset = ShaderRecompiler::IR::ShaderInfo::UserDataRayQueryHeader,
                    .layout = {0x76543210u, 0x1dc6ba98u, 0x000dbb8fu, 0x000f2510u},
                    .reach = 1});
}

void TestRayTracingInstructions() {
  TestTrinityBvhInstruction();
  TestTrinityStackInstruction();
  TestRayTracingClosestLeafCalls();
  TestRayTracingClosestWave();
  TestRayTracingNativeWalks();
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
