$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

// Fullscreen/textured blit. Positions arrive already in clip space, so no
// transform is applied - this is the shader the composite and post passes use to
// move one render target into another.
void main()
{
	v_texcoord0 = a_texcoord0;
	gl_Position = vec4(a_position, 1.0);
}
