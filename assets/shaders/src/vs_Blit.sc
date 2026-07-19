$input a_position, a_texcoord0
$output v_texcoord0

#include <bgfx_shader.sh>

// Fullscreen/textured blit. Positions arrive already in clip space, so no
// transform is applied - this is the shader the composite and post passes use to
// move one render target into another.
void main()
{
	v_texcoord0 = a_texcoord0;

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
	gl_Position = vec4(a_position, 1.0);
}
