$input a_position
$output v_worldpos

#include <bgfx_shader.sh>

// u_model comes from bgfx::setTransform (Renderer3D::DrawGrid), replacing the
// old u_Transform uniform; u_viewProj replaces the CameraData block.
void main()
{
	vec4 worldPos = mul(u_model[0], vec4(a_position, 1.0));
	v_worldpos = worldPos.xyz;
	gl_Position = mul(u_viewProj, worldPos);
}
