  // A wave32 BVH traversal with one bvh64 intersection per iteration and a per-lane LDS stack
  // (height in bits 0..15, byte address in bits 16..31). TLAS leaves test instance mask bit 2 and
  // enter the BLAS with the world-to-object ray at +8; BLAS leaves walk prim record lists. A
  // candidate is opaque by record word 1 bit 30 or instance flag bit 0 unless flag bit 1 is set,
  // an any-hit keeps non-opaque ones with u >= 0.25, and the closest kept one commits. s[0:3] is
  // the output buffer and s[4:5] a table {u64 TLAS header, u64 rays}: lane i traces ray i
  // {origin, t_max, direction, -} and writes {t, u, v, prim record word 1, instance user data at
  // +112, iterations} at 32 * i.
  static std::vector<u32> RayQueryTraversalProgram() {
    return {
        // clang-format off
        0xf4080802u, 0xfa000000u, // s_load_dwordx4 s[32:35], s[4:5], 0x0
        0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
        0xbe820320u, // s_mov_b32 s2, s32
        0xbe830321u, // s_mov_b32 s3, s33
        0xf4040201u, 0xfa000028u, // s_load_dwordx2 s[8:9], s[2:3], 0x28
        0x34020085u, // v_lshlrev_b32 v1, 5, v0
        0x7e060223u, // v_mov_b32 v3, s35
        0xd70f6a02u, 0x02020222u, // v_add_co_u32 v2, vcc_lo, s34, v1
        0x50060680u, // v_add_co_ci_u32 v3, vcc_lo, 0, v3, vcc_lo
        0xdc388000u, 0x287d0002u, // global_load_dwordx4 v[40:43], v[2:3], off
        0xdc388010u, 0x2c7d0002u, // global_load_dwordx4 v[44:47], v[2:3], off offset:16
        0xbf8c0070u, // s_waitcnt vmcnt(0) lgkmcnt(0)
        0x800a0802u, // s_add_u32 s10, s2, s8
        0x820b0903u, // s_addc_u32 s11, s3, s9
        0xbe8c0380u, // s_mov_b32 s12, 0
        0xbe8d03ffu, 0x80000000u, // s_mov_b32 s13, 0x80000000
        0xbe8e03c1u, // s_mov_b32 s14, -1
        0xbe8f03ffu, 0x810003ffu, // s_mov_b32 s15, 0x810003ff
        0xbe90037eu, // s_mov_b32 s16, exec_lo
        0x7e1e0202u, // v_mov_b32 v15, s2
        0x7e200203u, // v_mov_b32 v16, s3
        0x7e2202a5u, // v_mov_b32 v17, 37
        0x7e0a032bu, // v_mov_b32 v5, v43
        0x7e0c0328u, // v_mov_b32 v6, v40
        0x7e0e0329u, // v_mov_b32 v7, v41
        0x7e10032au, // v_mov_b32 v8, v42
        0x7e12032cu, // v_mov_b32 v9, v44
        0x7e14032du, // v_mov_b32 v10, v45
        0x7e16032eu, // v_mov_b32 v11, v46
        0x34260092u, // v_lshlrev_b32 v19, 18, v0
        0x7e2802c1u, // v_mov_b32 v20, -1
        0x7e340280u, // v_mov_b32 v26, 0
        0x7e360280u, // v_mov_b32 v27, 0
        0x7e3802c1u, // v_mov_b32 v28, -1
        0x7e3a0280u, // v_mov_b32 v29, 0
        0x7e3c0280u, // v_mov_b32 v30, 0
        0x7e3e0280u, // v_mov_b32 v31, 0
        // loop:
        0xd7000024u, 0x02021e83u, // v_lshrrev_b64 v[36:37], 3, v[15:16]
        0xd70f6a03u, 0x02024911u, // v_add_co_u32 v3, vcc_lo, v17, v36
        0x50084a80u, // v_add_co_ci_u32 v4, vcc_lo, 0, v37, vcc_lo
        0x7e185509u, // v_rcp_f32 v12, v9
        0x7e1a550au, // v_rcp_f32 v13, v10
        0x7e1c550bu, // v_rcp_f32 v14, v11
        0xf19c9f01u, 0x00032003u, // image_bvh64_intersect_ray v[32:35], v[3:14], s[12:15]
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0x4a3e3e81u, // v_add_nc_u32 v31, 1, v31
        0x36482287u, // v_and_b32 v36, 7, v17
        0xd4c10014u, 0x02024883u, // v_cmp_lt_u32 s20, 3, v36
        0xbe953c14u, // s_and_saveexec_b32 s21, s20
        0x7d8a46c1u, // v_cmp_ne_u32 vcc_lo, -1, v35
        0xbe963c6au, // s_and_saveexec_b32 s22, vcc_lo
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8340000u, 0x00002325u, // ds_write_b32 v37, v35
        0x4a2626ffu, 0x00200001u, // v_add_nc_u32 v19, 0x200001, v19
        0xbefe0316u, // s_mov_b32 exec_lo, s22
        0x7d8a44c1u, // v_cmp_ne_u32 vcc_lo, -1, v34
        0xbe963c6au, // s_and_saveexec_b32 s22, vcc_lo
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8340000u, 0x00002225u, // ds_write_b32 v37, v34
        0x4a2626ffu, 0x00200001u, // v_add_nc_u32 v19, 0x200001, v19
        0xbefe0316u, // s_mov_b32 exec_lo, s22
        0x7d8a42c1u, // v_cmp_ne_u32 vcc_lo, -1, v33
        0xbe963c6au, // s_and_saveexec_b32 s22, vcc_lo
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8340000u, 0x00002125u, // ds_write_b32 v37, v33
        0x4a2626ffu, 0x00200001u, // v_add_nc_u32 v19, 0x200001, v19
        0xbefe0316u, // s_mov_b32 exec_lo, s22
        0x7e4c0320u, // v_mov_b32 v38, v32
        0x8a7e7e15u, // s_andn2_b32 exec_lo, s21, exec_lo
        0x7e485521u, // v_rcp_f32 v36, v33
        0x104a4920u, // v_mul_f32 v37, v32, v36
        0x10604922u, // v_mul_f32 v48, v34, v36
        0x10624923u, // v_mul_f32 v49, v35, v36
        0xbe97037eu, // s_mov_b32 s23, exec_lo
        0x7c2c4b05u, // v_cmpx_ge_f32 v5, v37
        0x2c48249eu, // v_lshrrev_b32 v36, 30, v18
        0x38482b24u, // v_or_b32 v36, v36, v21
        0x36484881u, // v_and_b32 v36, 1, v36
        0x364e2a82u, // v_and_b32 v39, 2, v21
        0xd4c50018u, 0x02024880u, // v_cmp_ne_u32 s24, 0, v36
        0x7d844e80u, // v_cmp_eq_u32 vcc_lo, 0, v39
        0x87186a18u, // s_and_b32 s24, s24, vcc_lo
        0x7c0660ffu, 0x3e800000u, // v_cmp_le_f32 vcc_lo, 0x3e800000, v48
        0x886a6a18u, // s_or_b32 vcc_lo, s24, vcc_lo
        0x877e6a7eu, // s_and_b32 exec_lo, exec_lo, vcc_lo
        0x7e0a0325u, // v_mov_b32 v5, v37
        0x7e340330u, // v_mov_b32 v26, v48
        0x7e360331u, // v_mov_b32 v27, v49
        0x7e380312u, // v_mov_b32 v28, v18
        0x7e3a0316u, // v_mov_b32 v29, v22
        0x7e3c0317u, // v_mov_b32 v30, v23
        0xbefe0317u, // s_mov_b32 exec_lo, s23
        0x344e3083u, // v_lshlrev_b32 v39, 3, v24
        0x7e4c02c1u, // v_mov_b32 v38, -1
        0x7d262480u, // v_cmpx_le_i32 0, v18
        0x7e4c0327u, // v_mov_b32 v38, v39
        0xbefe0315u, // s_mov_b32 exec_lo, s21
        0x364826ffu, 0x0000ffffu, // v_and_b32 v36, 0xffff, v19
        0xd4c20018u, 0x02024cc1u, // v_cmp_eq_u32 s24, -1, v38
        0x7d844880u, // v_cmp_eq_u32 vcc_lo, 0, v36
        0x876a6a18u, // s_and_b32 vcc_lo, s24, vcc_lo
        0x8a7e6a7eu, // s_andn2_b32 exec_lo, exec_lo, vcc_lo
        0xbe993c18u, // s_and_saveexec_b32 s25, s24
        0x4a2626ffu, 0xffdfffffu, // v_add_nc_u32 v19, 0xffdfffff, v19
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8d80000u, 0x26000025u, // ds_read_b32 v38, v37
        0x7d842924u, // v_cmp_eq_u32 vcc_lo, v36, v20
        0xbe9a3c6au, // s_and_saveexec_b32 s26, vcc_lo
        0x7e1e0202u, // v_mov_b32 v15, s2
        0x7e200203u, // v_mov_b32 v16, s3
        0x7e2802c1u, // v_mov_b32 v20, -1
        0x7e0c0328u, // v_mov_b32 v6, v40
        0x7e0e0329u, // v_mov_b32 v7, v41
        0x7e10032au, // v_mov_b32 v8, v42
        0x7e12032cu, // v_mov_b32 v9, v44
        0x7e14032du, // v_mov_b32 v10, v45
        0x7e16032eu, // v_mov_b32 v11, v46
        0xbefe031au, // s_mov_b32 exec_lo, s26
        0xbefe0319u, // s_mov_b32 exec_lo, s25
        0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
        0x7e220326u, // v_mov_b32 v17, v38
        0x36484c87u, // v_and_b32 v36, 7, v38
        0x7d8c4883u, // v_cmp_ge_u32 vcc_lo, 3, v36
        0xbe993c6au, // s_and_saveexec_b32 s25, vcc_lo
        0xd4c5001au, 0x020228c1u, // v_cmp_ne_u32 s26, -1, v20
        0xbe9b3c1au, // s_and_saveexec_b32 s27, s26
        0x36484cc8u, // v_and_b32 v36, -8, v38
        0x2c4a4c83u, // v_lshrrev_b32 v37, 3, v38
        0xd70f6a32u, 0x02021f24u, // v_add_co_u32 v50, vcc_lo, v36, v15
        0x50662080u, // v_add_co_ci_u32 v51, vcc_lo, 0, v16, vcc_lo
        0x4a304a81u, // v_add_nc_u32 v24, 1, v37
        0xdc348000u, 0x117d0032u, // global_load_dwordx2 v[17:18], v[50:51], off
        0x8a7e7e1bu, // s_andn2_b32 exec_lo, s27, exec_lo
        0x2c484c83u, // v_lshrrev_b32 v36, 3, v38
        0x7e4a0280u, // v_mov_b32 v37, 0
        0xd6ff0024u, 0x02024887u, // v_lshlrev_b64 v[36:37], 7, v[36:37]
        0xd70f6a34u, 0x0202480au, // v_add_co_u32 v52, vcc_lo, s10, v36
        0x506a4a0bu, // v_add_co_ci_u32 v53, vcc_lo, s11, v37, vcc_lo
        0x7e220286u, // v_mov_b32 v17, 6
        0xdc348038u, 0x367d0034u, // global_load_dwordx2 v[54:55], v[52:53], off offset:56
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0x34486e95u, // v_lshlrev_b32 v36, 21, v55
        0x7d284880u, // v_cmpx_gt_i32 0, v36
        0xdc348000u, 0x0f7d0034u, // global_load_dwordx2 v[15:16], v[52:53], off
        0xdc388008u, 0x387d0034u, // global_load_dwordx4 v[56:59], v[52:53], off offset:8
        0xdc388018u, 0x3c7d0034u, // global_load_dwordx4 v[60:63], v[52:53], off offset:24
        0xdc388028u, 0x407d0034u, // global_load_dwordx4 v[64:67], v[52:53], off offset:40
        0x362826ffu, 0x0000ffffu, // v_and_b32 v20, 0xffff, v19
        0x7e2a0337u, // v_mov_b32 v21, v55
        0x7e2c0334u, // v_mov_b32 v22, v52
        0x7e2e0335u, // v_mov_b32 v23, v53
        0x7e220336u, // v_mov_b32 v17, v54
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0xd54b0006u, 0x05065138u, // v_fma_f32 v6, v56, v40, v65
        0x560c533bu, // v_fmac_f32 v6, v59, v41
        0x560c553eu, // v_fmac_f32 v6, v62, v42
        0xd54b0007u, 0x050a5139u, // v_fma_f32 v7, v57, v40, v66
        0x560e533cu, // v_fmac_f32 v7, v60, v41
        0x560e553fu, // v_fmac_f32 v7, v63, v42
        0xd54b0008u, 0x050e513au, // v_fma_f32 v8, v58, v40, v67
        0x5610533du, // v_fmac_f32 v8, v61, v41
        0x56105540u, // v_fmac_f32 v8, v64, v42
        0x10125938u, // v_mul_f32 v9, v56, v44
        0x56125b3bu, // v_fmac_f32 v9, v59, v45
        0x56125d3eu, // v_fmac_f32 v9, v62, v46
        0x10145939u, // v_mul_f32 v10, v57, v44
        0x56145b3cu, // v_fmac_f32 v10, v60, v45
        0x56145d3fu, // v_fmac_f32 v10, v63, v46
        0x1016593au, // v_mul_f32 v11, v58, v44
        0x56165b3du, // v_fmac_f32 v11, v61, v45
        0x56165d40u, // v_fmac_f32 v11, v64, v46
        0xbefe0319u, // s_mov_b32 exec_lo, s25
        0xbf89ff5au, // s_cbranch_execnz loop
        0xbefe0310u, // s_mov_b32 exec_lo, s16
        0x7e5802c1u, // v_mov_b32 v44, -1
        0x7d8a38c1u, // v_cmp_ne_u32 vcc_lo, -1, v28
        0xbe913c6au, // s_and_saveexec_b32 s17, vcc_lo
        0xdc308070u, 0x2c7d001du, // global_load_dword v44, v[29:30], off offset:112
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0xbefe0311u, // s_mov_b32 exec_lo, s17
        0x7e500305u, // v_mov_b32 v40, v5
        0x7e52031au, // v_mov_b32 v41, v26
        0x7e54031bu, // v_mov_b32 v42, v27
        0x7e56031cu, // v_mov_b32 v43, v28
        0x7e5a031fu, // v_mov_b32 v45, v31
        0xe0781000u, 0x80002801u, // buffer_store_dwordx4 v[40:43], v1, s[0:3], 0 offen
        0xe0741010u, 0x80002c01u, // buffer_store_dwordx2 v[44:45], v1, s[0:3], 0 offen offset:16
        0xbf810000u, // s_endpgm
        // clang-format on
    };
  }

  // That traversal in wave64 over 64 lanes (LDS stacks advance 256 bytes per level). It loads the
  // instance BLAS with the root link and moves it into the next header under the mask test.
  static std::vector<u32> RayQueryTraversalProgram64() {
    return {
        // clang-format off
        0xf4080802u, 0xfa000000u, // s_load_dwordx4 s[32:35], s[4:5], 0x0
        0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
        0xbe820320u, // s_mov_b32 s2, s32
        0xbe830321u, // s_mov_b32 s3, s33
        0xf4040201u, 0xfa000028u, // s_load_dwordx2 s[8:9], s[2:3], 0x28
        0x34020085u, // v_lshlrev_b32 v1, 5, v0
        0x7e060223u, // v_mov_b32 v3, s35
        0xd70f6a02u, 0x02020222u, // v_add_co_u32 v2, vcc_lo, s34, v1
        0x50060680u, // v_add_co_ci_u32 v3, vcc_lo, 0, v3, vcc_lo
        0xdc388000u, 0x287d0002u, // global_load_dwordx4 v[40:43], v[2:3], off
        0xdc388010u, 0x2c7d0002u, // global_load_dwordx4 v[44:47], v[2:3], off offset:16
        0xbf8c0070u, // s_waitcnt vmcnt(0) lgkmcnt(0)
        0x800a0802u, // s_add_u32 s10, s2, s8
        0x820b0903u, // s_addc_u32 s11, s3, s9
        0xbe8c0380u, // s_mov_b32 s12, 0
        0xbe8d03ffu, 0x80000000u, // s_mov_b32 s13, 0x80000000
        0xbe8e03c1u, // s_mov_b32 s14, -1
        0xbe8f03ffu, 0x810003ffu, // s_mov_b32 s15, 0x810003ff
        0xbea8047eu, // s_mov_b64 s[40:41], exec
        0x7e1e0202u, // v_mov_b32 v15, s2
        0x7e200203u, // v_mov_b32 v16, s3
        0x7e2202a5u, // v_mov_b32 v17, 37
        0x7e0a032bu, // v_mov_b32 v5, v43
        0x7e0c0328u, // v_mov_b32 v6, v40
        0x7e0e0329u, // v_mov_b32 v7, v41
        0x7e10032au, // v_mov_b32 v8, v42
        0x7e12032cu, // v_mov_b32 v9, v44
        0x7e14032du, // v_mov_b32 v10, v45
        0x7e16032eu, // v_mov_b32 v11, v46
        0x34260092u, // v_lshlrev_b32 v19, 18, v0
        0x7e2802c1u, // v_mov_b32 v20, -1
        0x7e340280u, // v_mov_b32 v26, 0
        0x7e360280u, // v_mov_b32 v27, 0
        0x7e3802c1u, // v_mov_b32 v28, -1
        0x7e3a0280u, // v_mov_b32 v29, 0
        0x7e3c0280u, // v_mov_b32 v30, 0
        0x7e3e0280u, // v_mov_b32 v31, 0
        // loop:
        0xd7000024u, 0x02021e83u, // v_lshrrev_b64 v[36:37], 3, v[15:16]
        0xd70f6a03u, 0x02024911u, // v_add_co_u32 v3, vcc_lo, v17, v36
        0x50084a80u, // v_add_co_ci_u32 v4, vcc_lo, 0, v37, vcc_lo
        0x7e185509u, // v_rcp_f32 v12, v9
        0x7e1a550au, // v_rcp_f32 v13, v10
        0x7e1c550bu, // v_rcp_f32 v14, v11
        0xf19c9f01u, 0x00032003u, // image_bvh64_intersect_ray v[32:35], v[3:14], s[12:15]
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0x4a3e3e81u, // v_add_nc_u32 v31, 1, v31
        0x36482287u, // v_and_b32 v36, 7, v17
        0xd4c1002au, 0x02024883u, // v_cmp_lt_u32 s[42:43], 3, v36
        0xbeac242au, // s_and_saveexec_b64 s[44:45], s[42:43]
        0x7d8a46c1u, // v_cmp_ne_u32 vcc_lo, -1, v35
        0xbeae246au, // s_and_saveexec_b64 s[46:47], vcc
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8340000u, 0x00002325u, // ds_write_b32 v37, v35
        0x4a2626ffu, 0x01000001u, // v_add_nc_u32 v19, 0x1000001, v19
        0xbefe042eu, // s_mov_b64 exec, s[46:47]
        0x7d8a44c1u, // v_cmp_ne_u32 vcc_lo, -1, v34
        0xbeae246au, // s_and_saveexec_b64 s[46:47], vcc
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8340000u, 0x00002225u, // ds_write_b32 v37, v34
        0x4a2626ffu, 0x01000001u, // v_add_nc_u32 v19, 0x1000001, v19
        0xbefe042eu, // s_mov_b64 exec, s[46:47]
        0x7d8a42c1u, // v_cmp_ne_u32 vcc_lo, -1, v33
        0xbeae246au, // s_and_saveexec_b64 s[46:47], vcc
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8340000u, 0x00002125u, // ds_write_b32 v37, v33
        0x4a2626ffu, 0x01000001u, // v_add_nc_u32 v19, 0x1000001, v19
        0xbefe042eu, // s_mov_b64 exec, s[46:47]
        0x7e4c0320u, // v_mov_b32 v38, v32
        0x8afe7e2cu, // s_andn2_b64 exec, s[44:45], exec
        0x7e485521u, // v_rcp_f32 v36, v33
        0x104a4920u, // v_mul_f32 v37, v32, v36
        0x10604922u, // v_mul_f32 v48, v34, v36
        0x10624923u, // v_mul_f32 v49, v35, v36
        0xbeb0047eu, // s_mov_b64 s[48:49], exec
        0x7c2c4b05u, // v_cmpx_ge_f32 v5, v37
        0x2c48249eu, // v_lshrrev_b32 v36, 30, v18
        0x38482b24u, // v_or_b32 v36, v36, v21
        0x36484881u, // v_and_b32 v36, 1, v36
        0x364e2a82u, // v_and_b32 v39, 2, v21
        0xd4c50032u, 0x02024880u, // v_cmp_ne_u32 s[50:51], 0, v36
        0x7d844e80u, // v_cmp_eq_u32 vcc_lo, 0, v39
        0x87b26a32u, // s_and_b64 s[50:51], s[50:51], vcc
        0x7c0660ffu, 0x3e800000u, // v_cmp_le_f32 vcc_lo, 0x3e800000, v48
        0x88ea6a32u, // s_or_b64 vcc, s[50:51], vcc
        0x87fe6a7eu, // s_and_b64 exec, exec, vcc
        0x7e0a0325u, // v_mov_b32 v5, v37
        0x7e340330u, // v_mov_b32 v26, v48
        0x7e360331u, // v_mov_b32 v27, v49
        0x7e380312u, // v_mov_b32 v28, v18
        0x7e3a0316u, // v_mov_b32 v29, v22
        0x7e3c0317u, // v_mov_b32 v30, v23
        0xbefe0430u, // s_mov_b64 exec, s[48:49]
        0x344e3083u, // v_lshlrev_b32 v39, 3, v24
        0x7e4c02c1u, // v_mov_b32 v38, -1
        0x7d262480u, // v_cmpx_le_i32 0, v18
        0x7e4c0327u, // v_mov_b32 v38, v39
        0xbefe042cu, // s_mov_b64 exec, s[44:45]
        0x364826ffu, 0x0000ffffu, // v_and_b32 v36, 0xffff, v19
        0xd4c20032u, 0x02024cc1u, // v_cmp_eq_u32 s[50:51], -1, v38
        0x7d844880u, // v_cmp_eq_u32 vcc_lo, 0, v36
        0x87ea6a32u, // s_and_b64 vcc, s[50:51], vcc
        0x8afe6a7eu, // s_andn2_b64 exec, exec, vcc
        0xbeb42432u, // s_and_saveexec_b64 s[52:53], s[50:51]
        0x4a2626ffu, 0xfeffffffu, // v_add_nc_u32 v19, 0xfeffffff, v19
        0x2c4a2690u, // v_lshrrev_b32 v37, 16, v19
        0xd8d80000u, 0x26000025u, // ds_read_b32 v38, v37
        0x7d842924u, // v_cmp_eq_u32 vcc_lo, v36, v20
        0xbeb6246au, // s_and_saveexec_b64 s[54:55], vcc
        0x7e1e0202u, // v_mov_b32 v15, s2
        0x7e200203u, // v_mov_b32 v16, s3
        0x7e2802c1u, // v_mov_b32 v20, -1
        0x7e0c0328u, // v_mov_b32 v6, v40
        0x7e0e0329u, // v_mov_b32 v7, v41
        0x7e10032au, // v_mov_b32 v8, v42
        0x7e12032cu, // v_mov_b32 v9, v44
        0x7e14032du, // v_mov_b32 v10, v45
        0x7e16032eu, // v_mov_b32 v11, v46
        0xbefe0436u, // s_mov_b64 exec, s[54:55]
        0xbefe0434u, // s_mov_b64 exec, s[52:53]
        0xbf8cc07fu, // s_waitcnt lgkmcnt(0)
        0x7e220326u, // v_mov_b32 v17, v38
        0x36484c87u, // v_and_b32 v36, 7, v38
        0x7d8c4883u, // v_cmp_ge_u32 vcc_lo, 3, v36
        0xbeb4246au, // s_and_saveexec_b64 s[52:53], vcc
        0xd4c50036u, 0x020228c1u, // v_cmp_ne_u32 s[54:55], -1, v20
        0xbeb82436u, // s_and_saveexec_b64 s[56:57], s[54:55]
        0x36484cc8u, // v_and_b32 v36, -8, v38
        0x2c4a4c83u, // v_lshrrev_b32 v37, 3, v38
        0xd70f6a32u, 0x02021f24u, // v_add_co_u32 v50, vcc_lo, v36, v15
        0x50662080u, // v_add_co_ci_u32 v51, vcc_lo, 0, v16, vcc_lo
        0x4a304a81u, // v_add_nc_u32 v24, 1, v37
        0xdc348000u, 0x117d0032u, // global_load_dwordx2 v[17:18], v[50:51], off
        0x8afe7e38u, // s_andn2_b64 exec, s[56:57], exec
        0x2c484c83u, // v_lshrrev_b32 v36, 3, v38
        0x7e4a0280u, // v_mov_b32 v37, 0
        0xd6ff0024u, 0x02024887u, // v_lshlrev_b64 v[36:37], 7, v[36:37]
        0xd70f6a34u, 0x0202480au, // v_add_co_u32 v52, vcc_lo, s10, v36
        0x506a4a0bu, // v_add_co_ci_u32 v53, vcc_lo, s11, v37, vcc_lo
        0x7e220286u, // v_mov_b32 v17, 6
        0xdc348038u, 0x367d0034u, // global_load_dwordx2 v[54:55], v[52:53], off offset:56
        0xdc348000u, 0x447d0034u, // global_load_dwordx2 v[68:69], v[52:53], off
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0x34486e95u, // v_lshlrev_b32 v36, 21, v55
        0x7d284880u, // v_cmpx_gt_i32 0, v36
        0x7e1e0344u, // v_mov_b32 v15, v68
        0x7e200345u, // v_mov_b32 v16, v69
        0xdc388008u, 0x387d0034u, // global_load_dwordx4 v[56:59], v[52:53], off offset:8
        0xdc388018u, 0x3c7d0034u, // global_load_dwordx4 v[60:63], v[52:53], off offset:24
        0xdc388028u, 0x407d0034u, // global_load_dwordx4 v[64:67], v[52:53], off offset:40
        0x362826ffu, 0x0000ffffu, // v_and_b32 v20, 0xffff, v19
        0x7e2a0337u, // v_mov_b32 v21, v55
        0x7e2c0334u, // v_mov_b32 v22, v52
        0x7e2e0335u, // v_mov_b32 v23, v53
        0x7e220336u, // v_mov_b32 v17, v54
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0xd54b0006u, 0x05065138u, // v_fma_f32 v6, v56, v40, v65
        0x560c533bu, // v_fmac_f32 v6, v59, v41
        0x560c553eu, // v_fmac_f32 v6, v62, v42
        0xd54b0007u, 0x050a5139u, // v_fma_f32 v7, v57, v40, v66
        0x560e533cu, // v_fmac_f32 v7, v60, v41
        0x560e553fu, // v_fmac_f32 v7, v63, v42
        0xd54b0008u, 0x050e513au, // v_fma_f32 v8, v58, v40, v67
        0x5610533du, // v_fmac_f32 v8, v61, v41
        0x56105540u, // v_fmac_f32 v8, v64, v42
        0x10125938u, // v_mul_f32 v9, v56, v44
        0x56125b3bu, // v_fmac_f32 v9, v59, v45
        0x56125d3eu, // v_fmac_f32 v9, v62, v46
        0x10145939u, // v_mul_f32 v10, v57, v44
        0x56145b3cu, // v_fmac_f32 v10, v60, v45
        0x56145d3fu, // v_fmac_f32 v10, v63, v46
        0x1016593au, // v_mul_f32 v11, v58, v44
        0x56165b3du, // v_fmac_f32 v11, v61, v45
        0x56165d40u, // v_fmac_f32 v11, v64, v46
        0xbefe0434u, // s_mov_b64 exec, s[52:53]
        0xbf89ff58u, // s_cbranch_execnz loop
        0xbefe0428u, // s_mov_b64 exec, s[40:41]
        0x7e5802c1u, // v_mov_b32 v44, -1
        0x7d8a38c1u, // v_cmp_ne_u32 vcc_lo, -1, v28
        0xbeba246au, // s_and_saveexec_b64 s[58:59], vcc
        0xdc308070u, 0x2c7d001du, // global_load_dword v44, v[29:30], off offset:112
        0xbf8c3f70u, // s_waitcnt vmcnt(0)
        0xbefe043au, // s_mov_b64 exec, s[58:59]
        0x7e500305u, // v_mov_b32 v40, v5
        0x7e52031au, // v_mov_b32 v41, v26
        0x7e54031bu, // v_mov_b32 v42, v27
        0x7e56031cu, // v_mov_b32 v43, v28
        0x7e5a031fu, // v_mov_b32 v45, v31
        0xe0781000u, 0x80002801u, // buffer_store_dwordx4 v[40:43], v1, s[0:3], 0 offen
        0xe0741010u, 0x80002c01u, // buffer_store_dwordx2 v[44:45], v1, s[0:3], 0 offen offset:16
        0xbf810000u, // s_endpgm
        // clang-format on
    };
  }

  void CheckRayQueryTraversal() {
    constexpr const char *name = "RayQueryTraversal";
    static constexpr ShaderProfile profile{.maintenance8 = true, .ray_query = true};
    const auto program = RayQueryTraversalProgram();

    // The pass leaves alone a commit that also writes LDS, a body that can break out of the loop,
    // a BLAS that enters the next header outside the mask test, and a pixel shader.
    const auto traced = [&](std::span<const u32> code, ShaderType stage, u32 wave = 32) {
      ShaderComputeInputInfo compute{};
      compute.wave_size = wave;
      compute.host_subgroup_size = 32;
      compute.threads_num[0] = 8;
      compute.threads_num[1] = compute.threads_num[2] = 1;
      compute.thread_ids_num = 1;
      compute.lds_size_dwords = 128;
      ShaderPixelInputInfo pixel{};
      const std::array<u32, 64> user_data{};
      ShaderRecompiler::CompileOptions options;
      options.stage = stage;
      options.wave_size = wave;
      options.profile = &profile;
      options.user_data = user_data;
      if (stage == ShaderType::Compute) {
        options.input_info.compute = &compute;
      } else {
        options.input_info.pixel = &pixel;
      }
      auto translated = ShaderRecompiler::TranslateProgram(code, options);
      ShaderRecompiler::IR::ResourceSpecialization specialization;
      specialization.buffers.resize(translated.program.info.buffers.size());
      return ShaderRecompiler::CompileProgram(std::move(translated), options, specialization)
          .program.info.ray_query;
    };
    const auto at = [&](u32 word) {
      return static_cast<size_t>(std::ranges::find(program, word) - program.begin());
    };
    auto stores = program;
    const auto commit = at(0x7e340330u); // v_mov_b32 v26, v48; v_mov_b32 v27, v49
    stores[commit] = EncodeDs0(0x0d);
    stores[commit + 1] = EncodeDs1(0, 37, 37); // ds_write_b32 v37, v37
    auto breaks = program;
    const auto count = at(0x4a3e3e81u); // v_add_nc_u32 v31, 1, v31
    // s_cbranch_scc1 to s_mov_b32 exec_lo, s16 after the loop.
    breaks[count] = EncodeSopp(0x05, static_cast<u32>(at(0xbefe0310u) - count - 1));
    // Header word 0 takes the BLAS before the mask test, which moves it into an output register.
    auto elsewhere = RayQueryTraversalProgram64();
    const auto mask = std::ranges::find(elsewhere, 0x7d284880u); // v_cmpx_gt_i32 0, v36
    mask[-2] = 0x7e1e0344u; // v_mov_b32 v15, v68 for s_waitcnt vmcnt(0)
    mask[1] = 0x7e8c0344u;  // v_mov_b32 v70, v68 for v_mov_b32 v15, v68
    // buffer_store_dword v70, v1, s[0:3], 0 offen offset:24 before s_endpgm
    elsewhere.insert(elsewhere.end() - 1, {0xe0701018u, 0x80004601u});
    Require(name, "recognized loop", traced(program, ShaderType::Compute),
            "the traversal loop was not driven by a ray query");
    Require(name, "stores", !traced(stores, ShaderType::Compute),
            "a loop with a store outside its box path was driven by a ray query");
    Require(name, "break", !traced(breaks, ShaderType::Compute),
            "a loop with a second exit was driven by a ray query");
    Require(name, "mask test", !traced(elsewhere, ShaderType::Compute, 64),
            "a mask test the next header does not take culled the instances");
    Require(name, "pixel shader", !traced(program, ShaderType::Pixel),
            "a pixel shader traversal was driven by a ray query");

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
    // The console reports a loop the pass leaves alone, not a loop-free intersection.
    const auto reported = [&](std::span<const u32> code) {
      std::fflush(stdout);
      const int console = dup(STDOUT_FILENO);
      auto *capture = std::tmpfile();
      dup2(fileno(capture), STDOUT_FILENO);
      traced(code, ShaderType::Compute);
      std::fflush(stdout);
      dup2(console, STDOUT_FILENO);
      close(console);
      std::rewind(capture);
      std::string text(4096, '\0');
      text.resize(std::fread(text.data(), 1, text.size(), capture));
      std::fclose(capture);
      return text.find("BVH traversal left to emulation") != std::string::npos;
    };
    auto once = program;
    once[at(0xbf89ff5au)] = 0xbf800000u; // s_nop 0 for s_cbranch_execnz loop
    Require(name, "unrecognized loop", reported(stores),
            "a loop the pass left alone was not reported");
    Require(name, "loop-free", !reported(once), "a loop-free intersection was reported");
#endif

    CheckRayQueryTraversal(name, program, false);
    CheckRayQueryTraversal("RayQueryTraversal wave64", RayQueryTraversalProgram64(), true);
  }

  // A traversal over the fan BLAS and a TLAS root box over four instances: A; B scaled, moved and
  // non-opaque; A in front of them with a mask the ray excludes; and A moved behind B. Lane i
  // traces case (i + i / 32) % 8, so the halves of a wave64 trace different rays. The first
  // dispatch walks the guest BVH; once the mirror is ready, a ray query over it feeds the loop's
  // own any-hit and commit code. Both must find the same hits.
  void CheckRayQueryTraversal(const char *name, const std::vector<u32> &program, bool wave64) {
    using G = GuestBvh;
    constexpr auto page = BufferCache::CACHING_PAGESIZE;
    constexpr u32 Last = 1u << 31, Miss = ~0u;
    const u32 lanes = wave64 ? 64 : 8;
    EnsureRuntimeContext();
    int64_t direct = -1;
    void *mapped = nullptr;
    bool allocated =
        LibKernel::Memory::KernelAllocateDirectMemory(
            0, LibKernel::Memory::KernelGetDirectMemorySize(), 4 * page, page, 0, &direct) == 0;
    if (allocated) {
      allocated =
          LibKernel::Memory::KernelMapDirectMemory(&mapped, 4 * page, 0x3, 0, direct, page) == 0;
    }
    Require(name, "allocation", allocated, "guest allocation failed");
    const auto blas = reinterpret_cast<uint64_t>(mapped);
    const auto tlas = blas + page, table = tlas + page, output = table + page;
    auto *words = static_cast<u32 *>(mapped);
    std::fill_n(words, 4 * page / sizeof(u32), 0u);
    WriteFanBlas({words});
    const G top{words + page / sizeof(u32)};
    top.Header(0x2c, 0x200 + 4 * 128);
    top.Instances(0x200, 4);
    top.Children(4, {G::Link(0, 0), G::Link(1, 0), G::Link(2, 0), G::Link(3, 0)});
    top.Bounds(4, 0, {0, 0, 4.5f, 1, 1, 5.5f});
    top.Bounds(4, 1, {16, 0, 5.5f, 20, 4, 6.5f});
    top.Bounds(4, 2, {0, 0, 2.5f, 1, 1, 3.5f});
    top.Bounds(4, 3, {17, 2.5f, 8.5f, 18, 3.5f, 9.5f});
    // Instance i scales x and moves, with the inverse at +8 and user data 0x100 + i at +112.
    const auto instance = [&](u32 index, u32 root, u32 flags, float scale,
                              std::array<float, 3> move) {
      u32 *record = top.Instance(0x200 + 128 * index, blas, root, flags,
                                 {scale, 0, 0, 0, 1, 0, 0, 0, 1, move[0], move[1], move[2]});
      const std::array<float, 12> inverse{1 / scale,        0,        0,       0, 1, 0, 0, 0, 1,
                                          -move[0] / scale, -move[1], -move[2]};
      for (u32 i = 0; i < inverse.size(); ++i) {
        record[2 + i] = std::bit_cast<u32>(inverse[i]);
      }
      record[112 / 4] = 0x100 + index;
    };
    instance(0, G::Link(6, 5), 0xff01, 1, {0, 0, 0});
    instance(1, G::Link(8, 5), 0xff02, 2, {10, 0, 1});
    instance(2, G::Link(6, 5), 0xfb01, 1, {0, 0, -2});
    instance(3, G::Link(6, 5), 0xff01, 1, {17, 2.5f, 4});

    struct Lane {
      std::array<float, 3> origin;
      float t_max = 100;
      std::array<float, 3> direction{0, 0, 1};
      float t = 100, u = 0, v = 0;
      u32 record = Miss, user = Miss;
    };
    constexpr auto NaN = std::numeric_limits<float>::quiet_NaN();
    constexpr std::array<Lane, 8> cases{{
        {{0.2f, 0.3f, 0}, 100, {0, 0, 1}, 5, 0.3f, 0.5f, 96, 0x100},
        {{0.9f, 0.6f, 0}, 100, {0, 0, 1}, 5, 0.4f, 0.5f, 97 | Last, 0x100},
        {{0.5f, 0.1f, 0}, 100, {0, 0, 1}, 5, 0.1f, 0.4f, 96, 0x100}, // Opaque below u = 0.25.
        {{19, 2.5f, 0}, 100, {0, 0, 1}, 6, 0.625f, 0.25f, 98 | Last, 0x101},
        {{16.4f, 1, 0}, 100, {0, 0, 1}, 6, 0.7f, 0.2f, 99 | Last, 0x101},
        {{17.6f, 2.8f, 0}, 100, {0, 0, 1}, 9, 0.3f, 0.1f, 96, 0x103}, // B at u = 0.2, then A.
        {{0.2f, 0.3f, 0}, 100, {0, 0, NaN}},                          // An invalid ray.
        {{0.2f, 0.3f, 0}, 4, {0, 0, 1}, 4},                           // A is past t_max.
    }};
    u32 *inputs = words + 2 * page / sizeof(u32);
    const auto rays = table + 0x100;
    inputs[0] = static_cast<u32>(tlas);
    inputs[1] = static_cast<u32>(tlas >> 32);
    inputs[2] = static_cast<u32>(rays);
    inputs[3] = static_cast<u32>(rays >> 32);
    for (u32 i = 0; i < lanes; ++i) {
      const auto &lane = cases[(i + i / 32) % cases.size()];
      const std::array<float, 7> ray{lane.origin[0],   lane.origin[1],    lane.origin[2],
                                     lane.t_max,       lane.direction[0], lane.direction[1],
                                     lane.direction[2]};
      for (u32 k = 0; k < ray.size(); ++k) {
        inputs[0x40 + 8 * i + k] = std::bit_cast<u32>(ray[k]);
      }
    }
    ShaderMapUserData(reinterpret_cast<uint64_t>(program.data()),
                      {.type = Prospero::ShaderBinaryType::kCs,
                       .code_size_bytes = static_cast<uint32_t>(program.size() * sizeof(u32))});

    m_runtime_context.ray_query_enabled = true;
    RenderContext context(m_runtime_context);
    context.InitializeGpu(nullptr);
    LibKernel::Memory::InstallGpuResources(&context);
    context.GetGpu().SendCommandSync([&] {
      CommandProcessor processor(context, 0);
      processor.BufferInit();
      auto &scheduler = context.GetCommandScheduler();
      auto &cache = context.GetBufferCache();
      auto &shaders = processor.GetShCtx();
      context.MapMemory(blas, 4 * page);
      // Buffers back the BVHs and rays, as a guest's GPU-built ones, so the first walk reads them.
      (void)cache.ObtainBuffer(blas, 3 * page, false);
      shaders.SetCsShader({.data_addr = reinterpret_cast<uint64_t>(program.data()),
                           .num_thread_x = lanes,
                           .num_thread_y = 1,
                           .num_thread_z = 1,
                           .user_sgpr = 6,
                           .lds_size = static_cast<uint16_t>(wave64 ? 8 : 1)});
      ShaderBufferResource descriptor{};
      descriptor.UpdateAddress48(output);
      descriptor.fields[2] = lanes * 32;
      descriptor.fields[3] =
          DstSel(4, 5, 6, 7) | (static_cast<u32>(Prospero::BufferFormat::k32UInt) << 12u);
      for (u32 i = 0; i < 4; ++i) {
        shaders.SetCsUserSgpr(i, descriptor.fields[i]);
      }
      shaders.SetCsUserSgpr(4, static_cast<u32>(table));
      shaders.SetCsUserSgpr(5, static_cast<u32>(table >> 32));
      const auto dispatch = [&] {
        const u32 wave32 = wave64 ? 0 : 1u << Pm4::COMPUTE_DISPATCH_INITIATOR_CS_W32_EN_SHIFT;
        processor.DispatchDirect(1, 1, 1, 0x41u | wave32);
        scheduler.Finish();
        // Creates buffers for the pages the mirror faulted on.
        cache.ProcessFaultBuffer();
        cache.ReadMemory(output, lanes * 32);
        std::vector<u32> result(lanes * 8);
        Require(name, "readback",
                LibKernel::Memory::TryReadBacking(output, result.data(), lanes * 32),
                "the traversal output could not be read back");
        return result;
      };
      const auto expect = [&](const char *step, const std::vector<u32> &result) {
        const auto close_to = [](u32 value, float wanted) {
          return std::abs(std::bit_cast<float>(value) - wanted) < 1e-4f;
        };
        for (u32 i = 0; i < lanes; ++i) {
          const u32 *hit = result.data() + 8 * i;
          const auto &lane = cases[(i + i / 32) % cases.size()];
          const bool ok = close_to(hit[0], lane.t) && close_to(hit[1], lane.u) &&
                          close_to(hit[2], lane.v) && hit[3] == lane.record && hit[4] == lane.user;
          if (!ok) {
            std::printf("[gpu]     %s ray %u: t %f u %f v %f record 0x%x user 0x%x\n", step, i,
                        std::bit_cast<float>(hit[0]), std::bit_cast<float>(hit[1]),
                        std::bit_cast<float>(hit[2]), hit[3], hit[4]);
          }
          Require(name, step, ok, "a ray found the wrong hit");
        }
      };
      const auto walked = dispatch();
      expect("guest walk", walked);
      // A query iteration takes one candidate: fewer than the walk, which also visits boxes.
      const auto queried = [&](const std::vector<u32> &result) {
        for (u32 i = 0; i < lanes; ++i) {
          if (result[8 * i + 5] >= walked[8 * i + 5])
            return false;
        }
        return true;
      };
      auto result = dispatch();
      for (u32 call = 0; call < 8 && !queried(result); ++call) {
        expect("guest walk", result);
        result = dispatch();
      }
      Require(name, "ray query", queried(result), "the loop never traced the mirror");
      expect("ray query", result);
      result = dispatch();
      Require(name, "ray query", queried(result), "the loop stopped tracing the mirror");
      expect("ray query", result);
      context.UnmapMemory(blas, 4 * page);
      scheduler.Finish();
    });
    LibKernel::Memory::InstallGpuResources(nullptr);
    context.ShutdownGpu();
    m_runtime_context.ray_query_enabled = false;
    bool released = LibKernel::Memory::KernelMunmap(blas, 4 * page) == 0;
    if (released) {
      released = LibKernel::Memory::KernelReleaseDirectMemory(direct, 4 * page) == 0;
    }
    Require(name, "release", released, "guest allocation release failed");
    std::printf("[gpu]     %-32s ok\n", name);
  }
