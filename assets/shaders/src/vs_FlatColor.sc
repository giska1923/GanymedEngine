$input a_position

#include <bgfx_shader.sh>

// u_modelViewProj is one of bgfx's predefined uniforms, assembled from
// bgfx::setViewTransform (view/proj) and bgfx::setTransform (model). It
// replaces the old hand-rolled u_ViewProjection * u_Transform pair.
void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
}
