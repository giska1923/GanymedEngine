$input a_position, a_color0
$output v_color0

#include <bgfx_shader.sh>

// u_viewProj is predefined, fed by bgfx::setViewTransform - this replaces the
// old CameraData uniform block.
void main()
{
	v_color0 = a_color0;
	gl_Position = mul(u_viewProj, vec4(a_position, 1.0));
}
