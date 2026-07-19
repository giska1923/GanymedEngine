$input a_position
$output v_worldpos

#include <bgfx_shader.sh>

// Cube-face bake pass: a_position doubles as the sampling direction. The face's
// view and 90-degree projection come from bgfx::setViewTransform, so the old
// u_View / u_Projection uniforms are gone.
void main()
{
	v_worldpos = a_position;
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
}
