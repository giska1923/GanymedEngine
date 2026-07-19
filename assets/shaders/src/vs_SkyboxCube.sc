$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

// Fullscreen quad at the far plane; the direction is reconstructed in the
// fragment shader from u_InverseViewProjection.
void main()
{
	v_texcoord0 = a_position.xy;
	gl_Position = vec4(a_position.xy, 0.9999, 1.0);
}
