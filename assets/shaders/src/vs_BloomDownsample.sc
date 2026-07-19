$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

// Fullscreen pass: positions arrive in NDC already, so no transform. The
// vertex buffer supplies 2 components; bgfx pads the rest.
void main()
{
	v_texcoord0 = a_position.xy * 0.5 + 0.5;
	gl_Position = vec4(a_position.xy, 0.0, 1.0);
}
