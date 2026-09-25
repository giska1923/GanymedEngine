#type vertex
#version 330 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;

layout(std140) uniform CameraData
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPositionUBO;
	float _pad0;
};

out vec4 v_Color;

void main()
{
	v_Color = a_Color;
	gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;
layout(location = 1) out int entityID;

in vec4 v_Color;

void main()
{
	color = v_Color;
	entityID = -1;
}
