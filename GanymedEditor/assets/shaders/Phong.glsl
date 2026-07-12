// Lit mesh shader — Cook-Torrance PBR (metallic/roughness) with analytic lights,
// a directional shadow map (PCF), normal mapping, and hemispheric ambient.

#type vertex
#version 330 core

layout(location = 0) in vec3 a_Position;
layout(location = 1) in vec3 a_Normal;
layout(location = 2) in vec3 a_Tangent;
layout(location = 3) in vec2 a_TexCoord;
// Per-instance (divisor 1): world transform + entity ID
layout(location = 4) in mat4 a_InstanceTransform;
layout(location = 8) in int a_InstanceEntityID;

layout(std140) uniform CameraData
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition;
	float _camPad0;
};

out vec3 v_WorldPos;
out vec3 v_Normal;
out vec3 v_Tangent;
out vec2 v_TexCoord;
flat out int v_EntityID;

void main()
{
	vec4 worldPos = a_InstanceTransform * vec4(a_Position, 1.0);
	v_WorldPos = worldPos.xyz;

	mat3 normalMatrix = mat3(a_InstanceTransform);
	v_Normal = normalize(normalMatrix * a_Normal);
	v_Tangent = normalize(normalMatrix * a_Tangent);
	v_TexCoord = a_TexCoord;
	v_EntityID = a_InstanceEntityID;

	gl_Position = u_ViewProjection * worldPos;
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;
layout(location = 1) out int entityID;

in vec3 v_WorldPos;
in vec3 v_Normal;
in vec3 v_Tangent;
in vec2 v_TexCoord;
flat in int v_EntityID;

const float PI = 3.14159265359;
const int MAX_LIGHTS = 32;
const int MAX_CASCADES = 4;

layout(std140) uniform CameraData
{
	mat4 u_ViewProjection;
	mat4 u_View;
	mat4 u_Projection;
	vec3 u_CameraPosition;
	float _camPad0;
};

struct Light
{
	vec4 Position;   // xyz world position, w = range
	vec4 Direction;  // xyz direction (spot), w = type (0 point, 1 spot)
	vec4 Color;      // rgb color, w = intensity
	vec4 SpotParams; // x innerCos, y outerCos, z falloff
};

layout(std140) uniform LightData
{
	vec4 u_DirLightDirection; // xyz dir, w = intensity
	vec4 u_DirLightColor;     // rgb color, w = castShadows
	vec4 u_AmbientSky;        // rgb sky color, w = ambient intensity
	vec4 u_AmbientGround;     // rgb ground color, w = hasSkyLight
	ivec4 u_LightCounts;      // x = point/spot light count
	Light u_Lights[MAX_LIGHTS];
};

// Material
uniform vec4 u_AlbedoColor;
uniform float u_Metallic;
uniform float u_Roughness;

uniform sampler2D u_AlbedoMap;
uniform sampler2D u_NormalMap;
uniform sampler2D u_MetallicRoughnessMap;

uniform int u_UseAlbedoMap;
uniform int u_UseNormalMap;
uniform int u_UseMetallicRoughnessMap;

// Cascaded shadow maps
uniform sampler2D u_ShadowMaps[MAX_CASCADES];
uniform mat4 u_LightSpaceMatrices[MAX_CASCADES];
uniform float u_CascadeSplits[MAX_CASCADES];
uniform int u_CascadeCount;
uniform int u_UseShadows;
uniform float u_ShadowTexelSize;

// Image-based lighting (HDR environment)
uniform samplerCube u_IrradianceMap;
uniform samplerCube u_PrefilterMap;
uniform sampler2D u_BRDFLUT;
uniform int u_UseIBL;
uniform float u_MaxReflectionLod;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH * NdotH;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;
	return a2 / max(denom, 1e-6);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
	float r = roughness + 1.0;
	float k = (r * r) / 8.0;
	return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
	float NdotV = max(dot(N, V), 0.0);
	float NdotL = max(dot(N, L), 0.0);
	return GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
}

vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 FresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
	return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 CookTorrance(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness, vec3 F0)
{
	float NdotL = max(dot(N, L), 0.0);
	if (NdotL <= 0.0)
		return vec3(0.0);

	vec3 H = normalize(V + L);
	float NDF = DistributionGGX(N, H, roughness);
	float G = GeometrySmith(N, V, L, roughness);
	vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

	vec3 numerator = NDF * G * F;
	float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 1e-4;
	vec3 specular = numerator / denominator;

	vec3 kS = F;
	vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

	return (kD * albedo / PI + specular) * radiance * NdotL;
}

float SampleCascade(sampler2D shadowMap, vec4 lightSpacePos, vec3 N, vec3 L)
{
	vec3 proj = lightSpacePos.xyz / lightSpacePos.w;
	proj = proj * 0.5 + 0.5;

	if (proj.z > 1.0)
		return 0.0;
	if (proj.x < 0.0 || proj.x > 1.0 || proj.y < 0.0 || proj.y > 1.0)
		return 0.0;

	float bias = max(0.0025 * (1.0 - dot(N, L)), 0.0008);

	float shadow = 0.0;
	for (int x = -1; x <= 1; x++)
	{
		for (int y = -1; y <= 1; y++)
		{
			float pcfDepth = texture(shadowMap, proj.xy + vec2(x, y) * u_ShadowTexelSize).r;
			shadow += (proj.z - bias > pcfDepth) ? 1.0 : 0.0;
		}
	}
	return shadow / 9.0;
}

int SelectCascade(float viewDepth)
{
	for (int i = 0; i < u_CascadeCount; i++)
	{
		if (viewDepth < u_CascadeSplits[i])
			return i;
	}
	return u_CascadeCount - 1;
}

float ShadowFactor(vec3 N, vec3 L)
{
	if (u_UseShadows == 0)
		return 0.0;

	// Linear view-space depth picks the cascade (GLSL 330 needs constant sampler indices)
	float viewDepth = abs((u_View * vec4(v_WorldPos, 1.0)).z);
	int layer = SelectCascade(viewDepth);

	if (layer == 0)
		return SampleCascade(u_ShadowMaps[0], u_LightSpaceMatrices[0] * vec4(v_WorldPos, 1.0), N, L);
	else if (layer == 1)
		return SampleCascade(u_ShadowMaps[1], u_LightSpaceMatrices[1] * vec4(v_WorldPos, 1.0), N, L);
	else if (layer == 2)
		return SampleCascade(u_ShadowMaps[2], u_LightSpaceMatrices[2] * vec4(v_WorldPos, 1.0), N, L);
	return SampleCascade(u_ShadowMaps[3], u_LightSpaceMatrices[3] * vec4(v_WorldPos, 1.0), N, L);
}

void main()
{
	vec4 albedoSample = u_AlbedoColor;
	if (u_UseAlbedoMap == 1)
		albedoSample *= texture(u_AlbedoMap, v_TexCoord);
	vec3 albedo = albedoSample.rgb;

	float metallic = u_Metallic;
	float roughness = u_Roughness;
	if (u_UseMetallicRoughnessMap == 1)
	{
		vec3 mr = texture(u_MetallicRoughnessMap, v_TexCoord).rgb;
		roughness *= mr.g;
		metallic *= mr.b;
	}
	roughness = clamp(roughness, 0.04, 1.0);

	// Normal (with optional tangent-space normal mapping)
	vec3 N = normalize(v_Normal);
	if (u_UseNormalMap == 1)
	{
		vec3 T = normalize(v_Tangent - N * dot(N, v_Tangent));
		vec3 B = cross(N, T);
		mat3 TBN = mat3(T, B, N);
		vec3 sampled = texture(u_NormalMap, v_TexCoord).xyz * 2.0 - 1.0;
		N = normalize(TBN * sampled);
	}

	vec3 V = normalize(u_CameraPosition - v_WorldPos);
	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	vec3 Lo = vec3(0.0);

	// Directional light (+ shadow)
	float dirIntensity = u_DirLightDirection.w;
	if (dirIntensity > 0.0)
	{
		vec3 L = normalize(-u_DirLightDirection.xyz);
		vec3 radiance = u_DirLightColor.rgb * dirIntensity;
		vec3 contribution = CookTorrance(N, V, L, radiance, albedo, metallic, roughness, F0);
		float shadow = ShadowFactor(N, L);
		Lo += contribution * (1.0 - shadow);
	}

	// Punctual lights
	int count = u_LightCounts.x;
	for (int i = 0; i < count; i++)
	{
		Light light = u_Lights[i];
		vec3 toLight = light.Position.xyz - v_WorldPos;
		float dist = length(toLight);
		float range = max(light.Position.w, 1e-3);
		if (dist > range)
			continue;

		vec3 L = toLight / max(dist, 1e-4);

		float falloff = max(light.SpotParams.z, 0.01);
		float d = clamp(1.0 - pow(dist / range, falloff), 0.0, 1.0);
		float attenuation = d * d;

		// Spot cone
		if (light.Direction.w > 0.5)
		{
			float cosAngle = dot(-L, normalize(light.Direction.xyz));
			float innerCos = light.SpotParams.x;
			float outerCos = light.SpotParams.y;
			float cone = clamp((cosAngle - outerCos) / max(innerCos - outerCos, 1e-4), 0.0, 1.0);
			attenuation *= cone;
		}

		vec3 radiance = light.Color.rgb * light.Color.w * attenuation;
		Lo += CookTorrance(N, V, L, radiance, albedo, metallic, roughness, F0);
	}

	// Ambient (HDR image-based lighting or procedural hemispheric fallback)
	vec3 ambient;
	if (u_UseIBL == 1)
	{
		float NdotV = max(dot(N, V), 0.0);
		vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
		vec3 kS = F;
		vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

		vec3 irradiance = texture(u_IrradianceMap, N).rgb;
		vec3 diffuse = irradiance * albedo;

		vec3 R = reflect(-V, N);
		vec3 prefilteredColor = textureLod(u_PrefilterMap, R, roughness * u_MaxReflectionLod).rgb;
		vec2 envBRDF = texture(u_BRDFLUT, vec2(NdotV, roughness)).rg;
		vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

		ambient = (kD * diffuse + specular) * u_AmbientSky.w;
	}
	else if (u_AmbientGround.w > 0.5)
	{
		float hemi = N.y * 0.5 + 0.5;
		vec3 irradiance = mix(u_AmbientGround.rgb, u_AmbientSky.rgb, hemi) * u_AmbientSky.w;
		vec3 kS = FresnelSchlick(max(dot(N, V), 0.0), F0);
		vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
		ambient = irradiance * albedo * kD + irradiance * F0 * 0.25;
	}
	else
	{
		ambient = albedo * 0.03;
	}

	color = vec4(ambient + Lo, albedoSample.a);
	entityID = v_EntityID;
}
