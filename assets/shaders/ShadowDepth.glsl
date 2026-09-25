// Depth-only pass for the directional shadow map (instanced).

#type vertex
#version 330 core

layout(location = 0) in vec3 a_Position;
// Per-instance (divisor 1): world transform (locations 4..7; 8 = entity ID, unused here)
layout(location = 4) in mat4 a_InstanceTransform;

uniform mat4 u_LightSpaceMatrix;

void main()
{
	gl_Position = u_LightSpaceMatrix * a_InstanceTransform * vec4(a_Position, 1.0);
}

#type fragment
#version 330 core

void main()
{
	// Depth is written automatically; no color output needed.
}
