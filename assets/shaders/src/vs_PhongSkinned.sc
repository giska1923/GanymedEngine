$input a_position, a_normal, a_tangent, a_texcoord0, a_indices, a_weight, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_worldpos, v_normal, v_tangent, v_texcoord0, v_entityid

#include <bgfx_shader.sh>

// vs_Phong with a four-weight palette blend in front of it. Pairs with fs_Phong
// unchanged - the varyings below are identical, which is the whole point of
// naming the two stages separately in Shader.
//
// Order matters: skin in mesh space first, then apply the per-instance model
// matrix. The palette is Global * InverseBind, which is identity at the bind
// pose, so an unposed rig comes out exactly where vs_Phong would put it.
//
// a_indices and a_weight arrive on vertex stream 1 (see Mesh::Build). Their
// names are fixed by shaderc, not chosen here.

// Must match Skeleton::MaxBones. bgfx fixes an array uniform's size at creation,
// so this is always the full array however few joints a rig actually has.
#define MAX_BONES 128

uniform mat4 u_Bones[MAX_BONES];

void main()
{
	// Joint indices travel as floats: bgfx has no integer vertex attribute.
	mat4 skin = u_Bones[int(a_indices.x)] * a_weight.x
	          + u_Bones[int(a_indices.y)] * a_weight.y
	          + u_Bones[int(a_indices.z)] * a_weight.z
	          + u_Bones[int(a_indices.w)] * a_weight.w;

	mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);

	vec4 skinnedPos = mul(skin, vec4(a_position, 1.0));
	vec4 worldPos = mul(model, skinnedPos);
	v_worldpos = worldPos.xyz;

	// Two normal matrices for the two stages, in the same order as the position.
	// Ignoring the non-uniform scale correction matches vs_Phong; rigs are not
	// authored with skewed joints often enough to pay for an inverse-transpose
	// per vertex.
	mat3 skinNormal = mat3(skin[0].xyz, skin[1].xyz, skin[2].xyz);
	mat3 normalMatrix = mat3(model[0].xyz, model[1].xyz, model[2].xyz);

	v_normal   = normalize(mul(normalMatrix, mul(skinNormal, a_normal)));
	v_tangent  = normalize(mul(normalMatrix, mul(skinNormal, a_tangent)));
	v_texcoord0 = a_texcoord0;
	v_entityid = i_data4.x;

	gl_Position = mul(u_viewProj, worldPos);
}
