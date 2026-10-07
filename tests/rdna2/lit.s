; v3 = (v0 + v1) * 100000.0   (a literal constant: the instruction is followed by a data word)
v_add_f32 v2, v0, v1
v_mul_f32 v3, 100000.0, v2
s_endpgm
