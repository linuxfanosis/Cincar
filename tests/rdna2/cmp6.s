; all six float compares of v0 and v1 as the digits of one number in v19
v_sub_f32 v2, v0, v0
v_add_f32 v3, 1.0, v2
v_cmp_lt_f32 vcc, v0, v1
v_cndmask_b32 v4, 0, v3, vcc
v_cmp_gt_f32 vcc, v0, v1
v_cndmask_b32 v5, 0, v3, vcc
v_cmp_le_f32 vcc, v0, v1
v_cndmask_b32 v6, 0, v3, vcc
v_cmp_ge_f32 vcc, v0, v1
v_cndmask_b32 v7, 0, v3, vcc
v_cmp_eq_f32 vcc, v0, v1
v_cndmask_b32 v8, 0, v3, vcc
v_cmp_neq_f32 vcc, v0, v1
v_cndmask_b32 v9, 0, v3, vcc
v_mul_f32 v10, 100000.0, v4
v_mul_f32 v11, 10000.0, v5
v_mul_f32 v12, 1000.0, v6
v_mul_f32 v13, 100.0, v7
v_mul_f32 v14, 10.0, v8
v_add_f32 v15, v10, v11
v_add_f32 v16, v15, v12
v_add_f32 v17, v16, v13
v_add_f32 v18, v17, v14
v_add_f32 v19, v18, v9
s_endpgm
