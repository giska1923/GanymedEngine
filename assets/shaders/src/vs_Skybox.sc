$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

// Fullscreen quad pushed to the far plane so the sky fills anything unwritten.
void main()
{
	v_texcoord0 = a_position.xy;
	gl_Position = vec4(a_position.xy, 0.9999, 1.0);
}
