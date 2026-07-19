$input a_position, i_data0, i_data1, i_data2, i_data3

#include <bgfx_shader.sh>

// Depth-only pass for the directional shadow cascades (instanced).
//
// bgfx has no attribute divisor: the per-instance world transform arrives in
// i_data0..3 from setInstanceDataBuffer instead of a mat4 vertex attribute.
// (i_data4 carries the entity ID, unused here.)

uniform mat4 u_LightSpaceMatrix;

void main()
{
	mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
	gl_Position = mul(u_LightSpaceMatrix, mul(model, vec4(a_position, 1.0)));
}
