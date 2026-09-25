// Renders an environment cubemap as the background, as a fullscreen quad.

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
uniform samplerCube u_EnvironmentMap;
uniform float u_Intensity;

void main()
{
	vec4 nearP = u_InverseViewProjection * vec4(v_NDC, -1.0, 1.0);
	vec4 farP = u_InverseViewProjection * vec4(v_NDC, 1.0, 1.0);
	vec3 dir = normalize(farP.xyz / farP.w - nearP.xyz / nearP.w);

	vec3 envColor = texture(u_EnvironmentMap, dir).rgb * max(u_Intensity, 0.0);

	color = vec4(envColor, 1.0);
	entityID = -1;
}
