$input a_position, a_indices, a_weight, i_data0, i_data1, i_data2, i_data3

#include <bgfx_shader.sh>

// vs_ShadowDepth with the same palette blend as vs_PhongSkinned, pairing with
// fs_ShadowDepth unchanged. This exists in the same phase as the color pass on
// purpose: a T-pose shadow under an animating character reads as a bug, and the
// depth pass has to deform the vertices identically or the shadow detaches.

// Must match Skeleton::MaxBones and vs_PhongSkinned.
#define MAX_BONES 128

uniform mat4 u_Bones[MAX_BONES];
uniform mat4 u_LightSpaceMatrix;

void main()
{
	mat4 skin = u_Bones[int(a_indices.x)] * a_weight.x
	          + u_Bones[int(a_indices.y)] * a_weight.y
	          + u_Bones[int(a_indices.z)] * a_weight.z
	          + u_Bones[int(a_indices.w)] * a_weight.w;

	mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);

	gl_Position = mul(u_LightSpaceMatrix, mul(model, mul(skin, vec4(a_position, 1.0))));
}
