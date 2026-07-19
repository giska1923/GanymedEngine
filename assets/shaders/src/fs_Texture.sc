$input v_color0, v_texcoord0, v_texindex, v_tiling, v_entityid

#include <bgfx_shader.sh>

// Renderer2D's batched sprite shader.
//
// bgfx has no sampler arrays, so the old `sampler2D u_Textures[16]` becomes 16
// individually declared samplers and the switch stays. See the Option A note in
// docs/BGFX_MIGRATION.md - a texture array or atlas would have changed what
// Renderer2D can batch (tiling/repeat in particular).

SAMPLER2D(s_tex0, 0);
SAMPLER2D(s_tex1, 1);
SAMPLER2D(s_tex2, 2);
SAMPLER2D(s_tex3, 3);
SAMPLER2D(s_tex4, 4);
SAMPLER2D(s_tex5, 5);
SAMPLER2D(s_tex6, 6);
SAMPLER2D(s_tex7, 7);
SAMPLER2D(s_tex8, 8);
SAMPLER2D(s_tex9, 9);
SAMPLER2D(s_tex10, 10);
SAMPLER2D(s_tex11, 11);
SAMPLER2D(s_tex12, 12);
SAMPLER2D(s_tex13, 13);
SAMPLER2D(s_tex14, 14);
SAMPLER2D(s_tex15, 15);

void main()
{
	vec2 texCoord = v_texcoord0 * v_tiling;
	vec4 texColor = vec4_splat(1.0);

	switch (int(v_texindex))
	{
		case 0: texColor = texture2D(s_tex0, texCoord); break;
		case 1: texColor = texture2D(s_tex1, texCoord); break;
		case 2: texColor = texture2D(s_tex2, texCoord); break;
		case 3: texColor = texture2D(s_tex3, texCoord); break;
		case 4: texColor = texture2D(s_tex4, texCoord); break;
		case 5: texColor = texture2D(s_tex5, texCoord); break;
		case 6: texColor = texture2D(s_tex6, texCoord); break;
		case 7: texColor = texture2D(s_tex7, texCoord); break;
		case 8: texColor = texture2D(s_tex8, texCoord); break;
		case 9: texColor = texture2D(s_tex9, texCoord); break;
		case 10: texColor = texture2D(s_tex10, texCoord); break;
		case 11: texColor = texture2D(s_tex11, texCoord); break;
		case 12: texColor = texture2D(s_tex12, texCoord); break;
		case 13: texColor = texture2D(s_tex13, texCoord); break;
		case 14: texColor = texture2D(s_tex14, texCoord); break;
		case 15: texColor = texture2D(s_tex15, texCoord); break;
	}

	gl_FragData[0] = texColor * v_color0;

	// Entity IDs travel as float: bgfx fragment shaders emit float4 only, and
	// the attachment is R32F for the same reason.
	gl_FragData[1] = vec4_splat(v_entityid);
}
