// Stores 1.0 through the V# held in the four user SGPRs. Replays cleanly.
  v_mov_b32 v1, 1.0
  buffer_store_dword v1, v0, s[0:3], 0 offen
  s_endpgm
