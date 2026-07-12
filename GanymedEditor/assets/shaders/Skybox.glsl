// Procedural hemispheric skybox rendered as a fullscreen quad.

#type vertex
#version 330 core

layout(location = 0) in vec2 a_Position;

out vec2 v_NDC;

void main()
{
	v_NDC = a_Position;
	gl_Position = vec4(a_Position, 0.9999, 1.0);
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;
layout(location = 1) out int entityID;

in vec2 v_NDC;

uniform mat4 u_InverseViewProjection;
uniform vec3 u_SkyColor;
uniform vec3 u_GroundColor;
uniform float u_SkyIntensity;
uniform vec3 u_SunDirection;

void main()
{
	vec4 nearP = u_InverseViewProjection * vec4(v_NDC, -1.0, 1.0);
	vec4 farP = u_InverseViewProjection * vec4(v_NDC, 1.0, 1.0);
	vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);

	float h = smoothstep(-0.05, 0.35, dir.y);
	vec3 sky = mix(u_GroundColor, u_SkyColor, h) * max(u_SkyIntensity, 0.0);

	// Sun glow along the directional light
	vec3 sunDir = normalize(-u_SunDirection);
	float sun = pow(max(dot(dir, sunDir), 0.0), 350.0);
	sky += vec3(1.0, 0.96, 0.85) * sun * max(u_SkyIntensity, 0.0);

	color = vec4(sky, 1.0);
	entityID = -1;
}
