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
	// **Alpha must be 1, and the id must not be splatted.** The blend state a draw sets applies to
	// EVERY colour attachment, so with alpha blending on (every sprite, every particle, any
	// transparent mesh) this target is blended too:
	//
	//     result = src * src.a + dst * (1 - src.a)
	//
	// `vec4_splat(id)` puts the id in alpha as well, so the id blends itself against the -1 clear:
	// id 1 happens to survive (alpha 1 takes the source), id 0 reads back as the clear, and
	// anything else lands on id*id + (1-id)*-1. Picking therefore worked for exactly one entity
	// handle and silently returned "nothing" or a wrong id for all the others.
	//
	// The attachment is R32F, so only .r is stored; alpha exists purely to make the blend a copy.
	gl_FragData[1] = vec4(v_entityid, 0.0, 0.0, 1.0);
}
