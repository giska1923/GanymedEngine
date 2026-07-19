$input v_color0

#include <bgfx_shader.sh>

// Debug lines. Writes -1 to the entity-ID attachment so lines are never picked.
void main()
{
	gl_FragData[0] = v_color0;
	gl_FragData[1] = vec4_splat(-1.0);
}
