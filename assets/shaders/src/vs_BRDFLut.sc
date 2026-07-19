$input a_position
$output v_texcoord0

#include <bgfx_shader.sh>

void main()
{
	v_texcoord0 = a_position.xy * 0.5 + 0.5;

	// Render-target origin correction. bgfx addresses render targets top-down on
	// D3D/Vulkan/Metal but bottom-up on OpenGL, so a fullscreen pass that derives
	// its UV straight from clip position samples the source VERTICALLY FLIPPED on
	// the top-down backends.
	//
	// It went unnoticed because tonemap + FXAA are two such passes and their flips
	// cancelled - turning FXAA off rendered the whole scene upside down. Bloom has
	// an odd pass count (N downsamples + N-1 upsamples), so its result came out
	// mirrored relative to the scene: the "reflections" under the geometry.
#if !BGFX_SHADER_LANGUAGE_GLSL
	v_texcoord0.y = 1.0 - v_texcoord0.y;
#endif
	gl_Position = vec4(a_position.xy, 0.0, 1.0);
}
