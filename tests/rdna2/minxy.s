; v2 = min(v0, v1) built from a compare and a conditional move
v_cmp_lt_f32 vcc, v0, v1
v_cndmask_b32 v2, v1, v0, vcc
s_endpgm
