$input v_texcoord0

#include <bgfx_shader.sh>

// Renders an environment cubemap as the background.

uniform mat4 u_InverseViewProjection;
uniform vec4 u_Intensity; // .x

SAMPLERCUBE(u_EnvironmentMap, 12); // matches kSkyboxCubemapSlot

void main()
{
	vec2 ndc = v_texcoord0;

	// Near plane is ndc z = 0 under the [0,1] clip depth this project uses.
	vec4 nearP = mul(u_InverseViewProjection, vec4(ndc, 0.0, 1.0));
	vec4 farP  = mul(u_InverseViewProjection, vec4(ndc, 1.0, 1.0));
	vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);

	// Explicit LOD 0. A fullscreen skybox has huge screen-space derivatives in
	// the cubemap direction, so automatic mip selection lands on a high mip -
	// which, for an HDR whose bright light sources dominate the average, reads
	// as a flat near-white wash rather than the panorama.
	vec3 envColor = textureCubeLod(u_EnvironmentMap, dir, 0.0).rgb * max(u_Intensity.x, 0.0);

	gl_FragData[0] = vec4(envColor, 1.0);
	gl_FragData[1] = vec4_splat(-1.0);
}
