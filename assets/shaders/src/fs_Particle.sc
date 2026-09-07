$input v_color0, v_texcoord0, v_entityid

#include <bgfx_shader.sh>

// Unlit texture × vertex colour (the gradient sample). Views 5/6 are an MRT:
// colour + entity ID. Transparent texels still write the ID, so hovering the
// empty area of a quad picks the emitter — the same contract as transparent
// meshes. Particles are not entities; there is nothing else to select.
SAMPLER2D(s_tex0, 0);

void main()
{
	gl_FragData[0] = texture2D(s_tex0, v_texcoord0) * v_color0;
	gl_FragData[1] = vec4_splat(v_entityid);
}
