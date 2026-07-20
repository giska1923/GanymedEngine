$input a_position, a_color0, a_texcoord0
$output v_texcoord0, v_color0

#include <bgfx_shader.sh>

// RmlUi hands out vertices already in viewport pixels. The per-draw translation
// RenderGeometry supplies is fed through bgfx::setTransform as a model matrix
// rather than a custom uniform, so u_modelViewProj does both jobs at once; the
// orthographic half comes from bgfx::setViewTransform on the UI view.
void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position.xy, 0.0, 1.0));
	v_texcoord0 = a_texcoord0;
	v_color0 = a_color0;
}
