$input v_texcoord0

#include <bgfx_shader.sh>

// Procedural hemispheric skybox rendered as a fullscreen quad.

uniform mat4 u_InverseViewProjection;
uniform vec4 u_SkyColor;     // .xyz
uniform vec4 u_GroundColor;  // .xyz
uniform vec4 u_SkyIntensity; // .x
uniform vec4 u_SunDirection; // .xyz

void main()
{
	vec2 ndc = v_texcoord0;

	// Near plane is ndc z = 0 under the [0,1] clip depth this project uses
	// (GLM_FORCE_DEPTH_ZERO_TO_ONE); the original GL shader used -1.
	vec4 nearP = mul(u_InverseViewProjection, vec4(ndc, 0.0, 1.0));
	vec4 farP  = mul(u_InverseViewProjection, vec4(ndc,  1.0, 1.0));
	vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);

	float h = smoothstep(-0.05, 0.35, dir.y);
	vec3 sky = mix(u_GroundColor.xyz, u_SkyColor.xyz, h) * max(u_SkyIntensity.x, 0.0);

	// Sun glow along the directional light
	vec3 sunDir = normalize(-u_SunDirection.xyz);
	float sun = pow(max(dot(dir, sunDir), 0.0), 350.0);
	sky += vec3(1.0, 0.96, 0.85) * sun * max(u_SkyIntensity.x, 0.0);

	gl_FragData[0] = vec4(sky, 1.0);
	gl_FragData[1] = vec4_splat(-1.0);
}
