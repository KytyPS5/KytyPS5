// A waterfall loop: each pass reads one lane's index with v_readfirstlane, fetches that V# with
// s_buffer_load_dwordx4 and stores through it for every lane that shares the index. The index is
// loaded from memory, so which V# the store uses is only known on the GPU.
// KNOWN FAILURE: resource tracking rejects it ("GPU-selected access requires a raw DWORD x2/x3/x4
// load"), the same failure class as GTA V's compute shader 0x6a53456e7ef5d1b0.
  buffer_load_dword v2, v0, s[0:3], 0 offen
  s_waitcnt vmcnt(0)
  v_mov_b32 v1, 1.0
  s_mov_b64 s[10:11], exec
loop:
  v_readfirstlane_b32 s8, v2
  v_cmp_eq_u32 vcc, s8, v2
  s_and_saveexec_b64 s[16:17], vcc
  s_buffer_load_dwordx4 s[12:15], s[0:3], s8
  s_waitcnt lgkmcnt(0)
  buffer_store_dword v1, v0, s[12:15], 0 offen
  s_xor_b64 exec, exec, s[16:17]
  s_cbranch_execnz loop
  s_mov_b64 exec, s[10:11]
  s_endpgm
