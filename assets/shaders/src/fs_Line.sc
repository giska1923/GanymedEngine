$input v_color0

#include <bgfx_shader.sh>

// Debug lines. Writes -1 to the entity-ID attachment so lines are never picked.
void main()
{
	gl_FragData[0] = v_color0;
	// Alpha 1, not splatted: the blend state applies to this attachment too, and a splatted
	// -1 would put -1 in alpha and blend the sentinel against whatever was there. See
	// fs_Texture.sc for the full story.
	gl_FragData[1] = vec4(-1.0, 0.0, 0.0, 1.0);
}
