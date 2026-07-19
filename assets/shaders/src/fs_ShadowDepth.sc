#include <bgfx_shader.sh>

// Depth is written automatically; the colour output is unused. bgfx still
// requires a fragment shader, so emit an opaque constant.
void main()
{
	gl_FragColor = vec4_splat(1.0);
}
