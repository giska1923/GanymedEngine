$input a_position, a_color0, a_texcoord0, a_texcoord1, a_texcoord2, a_texcoord3
$output v_color0, v_texcoord0, v_texindex, v_tiling, v_entityid

#include <bgfx_shader.sh>

// a_texcoord1/2/3 carry TexIndex / TilingFactor / EntityID - free-form data
// riding in spare TexCoordN slots, per the mapping in Buffer.h.
void main()
{
	v_color0    = a_color0;
	v_texcoord0 = a_texcoord0;
	v_texindex  = a_texcoord1;
	v_tiling    = a_texcoord2;
	v_entityid  = a_texcoord3;

	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
}
