$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_worldpos, v_normal, v_tangent, v_texcoord0, v_entityid

#include <bgfx_shader.sh>

// Per-instance world transform in i_data0..3 and entity ID in i_data4.x - bgfx
// feeds these from setInstanceDataBuffer, since it has no attribute divisor.
// u_viewProj is predefined, replacing the old CameraData block.
void main()
{
	mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);

	vec4 worldPos = mul(model, vec4(a_position, 1.0));
	v_worldpos = worldPos.xyz;

	mat3 normalMatrix = mat3(model[0].xyz, model[1].xyz, model[2].xyz);
	v_normal   = normalize(mul(normalMatrix, a_normal));
	v_tangent  = normalize(mul(normalMatrix, a_tangent));
	v_texcoord0 = a_texcoord0;
	v_entityid = i_data4.x;

	gl_Position = mul(u_viewProj, worldPos);
}
