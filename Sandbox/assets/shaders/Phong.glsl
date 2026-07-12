// Lit mesh shader (Blinn-Phong with glTF metallic-roughness inputs)

#type vertex
#version 330 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;

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
out vec3 v_Normal;
out vec2 v_TexCoord;

void main()
{
	vec4 worldPos = u_Transform * vec4(a_Position, 1.0);
	v_WorldPos = worldPos.xyz;
	v_Normal = mat3(u_Transform) * a_Normal;
	v_TexCoord = a_TexCoord;
	gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;
layout(location = 1) out int entityID;

in vec3 v_WorldPos;
in vec3 v_Normal;
in vec2 v_TexCoord;

uniform vec4 u_AlbedoColor;
uniform float u_Metallic;
uniform float u_Roughness;

uniform sampler2D u_AlbedoMap;
uniform sampler2D u_NormalMap;
uniform sampler2D u_MetallicRoughnessMap;

uniform int u_UseAlbedoMap;
uniform int u_UseNormalMap;
uniform int u_UseMetallicRoughnessMap;

uniform vec3 u_LightDirection;
uniform vec3 u_LightColor;
uniform vec3 u_CameraPosition;
uniform int u_EntityID;

void main()
{
	vec4 albedo = u_AlbedoColor;
	if (u_UseAlbedoMap == 1)
		albedo *= texture(u_AlbedoMap, v_TexCoord);

	float metallic = u_Metallic;
	float roughness = u_Roughness;
	if (u_UseMetallicRoughnessMap == 1)
	{
		vec3 mr = texture(u_MetallicRoughnessMap, v_TexCoord).rgb;
		roughness *= mr.g;
		metallic *= mr.b;
	}

	vec3 N = normalize(v_Normal);
	vec3 L = normalize(-u_LightDirection);
	vec3 V = normalize(u_CameraPosition - v_WorldPos);
	vec3 H = normalize(L + V);

	float NdotL = max(dot(N, L), 0.0);
	float NdotH = max(dot(N, H), 0.0);

	float shininess = mix(8.0, 256.0, 1.0 - clamp(roughness, 0.04, 1.0));
	vec3 diffuse = albedo.rgb * u_LightColor * NdotL;
	vec3 specular = u_LightColor * pow(NdotH, shininess) * mix(0.04, 1.0, metallic);
	vec3 ambient = albedo.rgb * 0.12;

	color = vec4(ambient + diffuse + specular, albedo.a);
	entityID = u_EntityID;
}
