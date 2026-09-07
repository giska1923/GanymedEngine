$input a_position, a_color0, a_texcoord0, a_texcoord3
$output v_color0, v_texcoord0, v_entityid

#include <bgfx_shader.sh>

// CPU-constructed camera-facing billboards. Positions are already in world
// space; u_viewProj is the SceneHDR view transform (not view 6 — Renderer2D's
// u_view is identity there and billboarding from view rows would collapse).
void main()
{
	v_color0    = a_color0;
	v_texcoord0 = a_texcoord0;
	v_entityid  = a_texcoord3;

	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
}
