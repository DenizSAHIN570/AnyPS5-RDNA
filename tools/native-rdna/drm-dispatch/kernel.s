.text
  s_lshl_b32 s3, s2, 6
  v_add_nc_u32 v1, s3, v0
  v_lshlrev_b32 v2, 2, v1
  v_mad_u32_u24 v3, v1, 3, 7
  global_store_b32 v2, v3, s[0:1]
  s_endpgm
