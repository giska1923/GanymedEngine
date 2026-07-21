$input v_texcoord0, v_color0

#include <bgfx_shader.sh>

SAMPLER2D(s_Texture, 0);

// Both the vertex colour and the texture are PREMULTIPLIED alpha (RmlUi's own
// convention - see Rml::Vertex and the premultiply step in LoadTexture), so a
// straight multiply is correct here and the draw state pairs it with
// BLEND_FUNC(ONE, INV_SRC_ALPHA). Using ordinary src-alpha blending instead
// double-applies alpha and shows up as dark fringing on every glyph edge.
void main()
{
	gl_FragColor = texture2D(s_Texture, v_texcoord0) * v_color0;
}
