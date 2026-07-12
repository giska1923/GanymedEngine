// Basic Texture Shader

#type vertex
#version 330 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec4 a_Color;
layout(location = 2) in vec2 a_TexCoord;
layout(location = 3) in float a_TexIndex;
layout(location = 4) in float a_TilingFactor;
layout(location = 5) in int a_EntityID;

uniform mat4 u_ViewProjection;

out vec4 v_Color;
out vec2 v_TexCoord;
out float v_TexIndex;
out float v_TilingFactor;
flat out int v_EntityID;

void main()
{
	v_Color = a_Color;
	v_TexCoord = a_TexCoord;
	v_TexIndex = a_TexIndex;
	v_TilingFactor = a_TilingFactor;
	v_EntityID = a_EntityID;
	gl_Position = u_ViewProjection * vec4(a_Position, 1.0);
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;
layout(location = 1) out int entityID;

in vec4 v_Color;
in vec2 v_TexCoord;
in float v_TexIndex;
in float v_TilingFactor;
flat in int v_EntityID;

// GLSL 330 requires constant sampler array indices; 16 units is the GL 4.1 guaranteed minimum
uniform sampler2D u_Textures[16];

void main()
{
	vec2 texCoord = v_TexCoord * v_TilingFactor;
	vec4 texColor = vec4(1.0);
	switch (int(v_TexIndex))
	{
		case  0: texColor = texture(u_Textures[ 0], texCoord); break;
		case  1: texColor = texture(u_Textures[ 1], texCoord); break;
		case  2: texColor = texture(u_Textures[ 2], texCoord); break;
		case  3: texColor = texture(u_Textures[ 3], texCoord); break;
		case  4: texColor = texture(u_Textures[ 4], texCoord); break;
		case  5: texColor = texture(u_Textures[ 5], texCoord); break;
		case  6: texColor = texture(u_Textures[ 6], texCoord); break;
		case  7: texColor = texture(u_Textures[ 7], texCoord); break;
		case  8: texColor = texture(u_Textures[ 8], texCoord); break;
		case  9: texColor = texture(u_Textures[ 9], texCoord); break;
		case 10: texColor = texture(u_Textures[10], texCoord); break;
		case 11: texColor = texture(u_Textures[11], texCoord); break;
		case 12: texColor = texture(u_Textures[12], texCoord); break;
		case 13: texColor = texture(u_Textures[13], texCoord); break;
		case 14: texColor = texture(u_Textures[14], texCoord); break;
		case 15: texColor = texture(u_Textures[15], texCoord); break;
	}
	color = texColor * v_Color;
	entityID = v_EntityID;
}
