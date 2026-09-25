// Infinite-style ground grid (XZ plane)

#type vertex
#version 330 core

layout(location = 0) in vec3 a_Position;

layout(std140) uniform CameraData
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPositionUBO;
	float _pad0;
};

uniform mat4 u_Transform;

out vec3 v_WorldPos;

void main()
{
	vec4 worldPos = u_Transform * vec4(a_Position, 1.0);
	v_WorldPos = worldPos.xyz;
	gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;
layout(location = 1) out int entityID;

in vec3 v_WorldPos;

uniform vec3 u_CameraPosition;

float grid(vec2 uv, float lineWidth)
{
	vec2 d = fwidth(uv);
	vec2 a = abs(fract(uv - 0.5) - 0.5) / max(d, vec2(1e-6));
	float line = min(a.x, a.y);
	return 1.0 - min(line, 1.0);
}

void main()
{
	vec2 uv = v_WorldPos.xz;

	float minor = grid(uv, 1.0);
	float major = grid(uv * 0.1, 1.0);

	vec3 gridColor = mix(vec3(0.25), vec3(0.45), major);
	float alpha = max(minor * 0.35, major * 0.55);

	float dist = length(u_CameraPosition.xz - v_WorldPos.xz);
	float fade = 1.0 - smoothstep(20.0, 80.0, dist);
	alpha *= fade;

	if (alpha < 0.01)
		discard;

	// Axis emphasis
	float axisWidth = max(fwidth(uv.x), fwidth(uv.y)) * 1.5;
	if (abs(uv.x) < axisWidth)
	{
		gridColor = vec3(0.85, 0.2, 0.2);
		alpha = max(alpha, 0.85 * fade);
	}
	if (abs(uv.y) < axisWidth)
	{
		gridColor = vec3(0.2, 0.85, 0.2);
		alpha = max(alpha, 0.85 * fade);
	}

	color = vec4(gridColor, alpha);
	entityID = -1;
}
