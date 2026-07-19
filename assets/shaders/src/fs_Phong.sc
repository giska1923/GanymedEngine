$input v_worldpos, v_normal, v_tangent, v_texcoord0, v_entityid

#include <bgfx_shader.sh>

// PBR (Cook-Torrance) with cascaded shadows and optional IBL.
//
// Ported from the GLSL original; the maths is untouched. What changed:
//   - the CameraData / LightData std140 blocks are gone (§5.2). Camera matrices
//     come from bgfx predefined uniforms, and the light block is a flat vec4[]
//     set by FrameUniforms, unpacked below by GetLight().
//   - sampler arrays do not exist: u_ShadowMaps[4] became s_shadowMap0..3, on
//     the same texture units the C++ side binds (5..8).
//   - every scalar/int uniform is a vec4 with the value in .x.

#define PI 3.14159265359
#define MAX_LIGHTS 32
#define MAX_CASCADES 4

uniform vec4 u_CameraPosition;      // .xyz, from FrameUniforms

// Light block, flattened: four vec4s per light, matching the old GPULight.
uniform vec4 u_DirLightDirection;   // xyz dir, w = intensity
uniform vec4 u_DirLightColor;       // rgb colour, w = castShadows
uniform vec4 u_AmbientSky;          // rgb sky colour, w = ambient intensity
uniform vec4 u_AmbientGround;       // rgb ground colour, w = hasSkyLight
uniform vec4 u_LightCounts;         // .x = point/spot light count
uniform vec4 u_Lights[MAX_LIGHTS * 4];

struct Light
{
	vec4 Position;   // xyz world position, w = range
	vec4 Direction;  // xyz direction (spot), w = type (0 point, 1 spot)
	vec4 Color;      // rgb colour, w = intensity
	vec4 SpotParams; // x innerCos, y outerCos, z falloff
};

Light GetLight(int i)
{
	Light l;
	l.Position   = u_Lights[i * 4 + 0];
	l.Direction  = u_Lights[i * 4 + 1];
	l.Color      = u_Lights[i * 4 + 2];
	l.SpotParams = u_Lights[i * 4 + 3];
	return l;
}

// Material
uniform vec4 u_AlbedoColor;
uniform vec4 u_Metallic;                  // .x
uniform vec4 u_Roughness;                 // .x
uniform vec4 u_UseAlbedoMap;              // .x
uniform vec4 u_UseNormalMap;              // .x
uniform vec4 u_UseMetallicRoughnessMap;   // .x

SAMPLER2D(u_AlbedoMap,            0);
SAMPLER2D(u_NormalMap,            1);
SAMPLER2D(u_MetallicRoughnessMap, 2);

// Cascaded shadow maps (units 5..8, matching BindMaterialForColorPass)
SAMPLER2D(s_shadowMap0, 5);
SAMPLER2D(s_shadowMap1, 6);
SAMPLER2D(s_shadowMap2, 7);
SAMPLER2D(s_shadowMap3, 8);

uniform mat4 u_LightSpaceMatrices[MAX_CASCADES];
uniform vec4 u_CascadeSplits;   // one float per cascade
uniform vec4 u_CascadeCount;    // .x
uniform vec4 u_UseShadows;      // .x
uniform vec4 u_ShadowTexelSize; // .x

// Image-based lighting (units 9..11)
SAMPLERCUBE(u_IrradianceMap, 9);
SAMPLERCUBE(u_PrefilterMap, 10);
SAMPLER2D(u_BRDFLUT,        11);
uniform vec4 u_UseIBL;           // .x
uniform vec4 u_MaxReflectionLod; // .x

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
	return F0 + (max(vec3_splat(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 CookTorrance(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness, vec3 F0)
{
	float NdotL = max(dot(N, L), 0.0);
	if (NdotL <= 0.0)
		return vec3_splat(0.0);

	vec3 H = normalize(V + L);
	float NDF = DistributionGGX(N, H, roughness);
	float G = GeometrySmith(N, V, L, roughness);
	vec3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

	vec3 numerator = NDF * G * F;
	float denominator = 4.0 * max(dot(N, V), 0.0) * NdotL + 1e-4;
	vec3 specular = numerator / denominator;

	vec3 kS = F;
	vec3 kD = (vec3_splat(1.0) - kS) * (1.0 - metallic);

	return (kD * albedo / PI + specular) * radiance * NdotL;
}

float SampleCascade(BgfxSampler2D shadowMap, vec4 lightSpacePos, vec3 N, vec3 L)
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
			float pcfDepth = texture2D(shadowMap, proj.xy + vec2(x, y) * u_ShadowTexelSize.x).r;
			shadow += (proj.z - bias > pcfDepth) ? 1.0 : 0.0;
		}
	}
	return shadow / 9.0;
}

int SelectCascade(float viewDepth)
{
	for (int i = 0; i < int(u_CascadeCount.x); i++)
	{
		if (viewDepth < u_CascadeSplits[i])
			return i;
	}
	return int(u_CascadeCount.x) - 1;
}

// worldPos is passed in: varyings are main()'s parameters under HLSL,
// so a helper function cannot reference v_worldpos directly.
float ShadowFactor(vec3 N, vec3 L, vec3 worldPos)
{
	if (u_UseShadows.x < 0.5)
		return 0.0;

	// Linear view-space depth picks the cascade (GLSL 330 needs constant sampler indices)
	float viewDepth = abs(mul(u_view, vec4(worldPos, 1.0)).z);
	int layer = SelectCascade(viewDepth);

	if (layer == 0)
		return SampleCascade(s_shadowMap0, mul(u_LightSpaceMatrices[0], vec4(worldPos, 1.0)), N, L);
	else if (layer == 1)
		return SampleCascade(s_shadowMap1, mul(u_LightSpaceMatrices[1], vec4(worldPos, 1.0)), N, L);
	else if (layer == 2)
		return SampleCascade(s_shadowMap2, mul(u_LightSpaceMatrices[2], vec4(worldPos, 1.0)), N, L);
	return SampleCascade(s_shadowMap3, mul(u_LightSpaceMatrices[3], vec4(worldPos, 1.0)), N, L);
}

void main()
{
	vec4 albedoSample = u_AlbedoColor;
	if (u_UseAlbedoMap.x > 0.5)
		albedoSample *= texture2D(u_AlbedoMap, v_texcoord0);
	vec3 albedo = albedoSample.rgb;

	float metallic = u_Metallic.x;
	float roughness = u_Roughness.x;
	if (u_UseMetallicRoughnessMap.x > 0.5)
	{
		vec3 mr = texture2D(u_MetallicRoughnessMap, v_texcoord0).rgb;
		roughness *= mr.g;
		metallic *= mr.b;
	}
	roughness = clamp(roughness, 0.04, 1.0);

	// Normal (with optional tangent-space normal mapping)
	vec3 N = normalize(v_normal);
	if (u_UseNormalMap.x > 0.5)
	{
		vec3 T = normalize(v_tangent - N * dot(N, v_tangent));
		vec3 B = cross(N, T);
		// mtxFromCols builds the same column-major basis on every backend;
		// bare mat3(a,b,c) differs between GLSL and HLSL.
		mat3 TBN = mtxFromCols(T, B, N);
		vec3 sampled = texture2D(u_NormalMap, v_texcoord0).xyz * 2.0 - 1.0;
		N = normalize(mul(TBN, sampled)); // HLSL has no matrix * vector operator
	}

	vec3 V = normalize(u_CameraPosition - v_worldpos);
	vec3 F0 = mix(vec3_splat(0.04), albedo, metallic);

	vec3 Lo = vec3_splat(0.0);

	// Directional light (+ shadow)
	float dirIntensity = u_DirLightDirection.w;
	if (dirIntensity > 0.0)
	{
		vec3 L = normalize(-u_DirLightDirection.xyz);
		vec3 radiance = u_DirLightColor.rgb * dirIntensity;
		vec3 contribution = CookTorrance(N, V, L, radiance, albedo, metallic, roughness, F0);
		float shadow = ShadowFactor(N, L, v_worldpos);
		Lo += contribution * (1.0 - shadow);
	}

	// Punctual lights
	int count = int(u_LightCounts.x);
	for (int i = 0; i < count; i++)
	{
		Light light = GetLight(i);
		vec3 toLight = light.Position.xyz - v_worldpos;
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
	if (u_UseIBL.x > 0.5)
	{
		float NdotV = max(dot(N, V), 0.0);
		vec3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
		vec3 kS = F;
		vec3 kD = (vec3_splat(1.0) - kS) * (1.0 - metallic);

		vec3 irradiance = textureCube(u_IrradianceMap, N).rgb;
		vec3 diffuse = irradiance * albedo;

		vec3 R = reflect(-V, N);
		vec3 prefilteredColor = textureCubeLod(u_PrefilterMap, R, roughness * u_MaxReflectionLod.x).rgb;
		vec2 envBRDF = texture2D(u_BRDFLUT, vec2(NdotV, roughness)).rg;
		vec3 specular = prefilteredColor * (F * envBRDF.x + envBRDF.y);

		ambient = (kD * diffuse + specular) * u_AmbientSky.w;
	}
	else if (u_AmbientGround.w > 0.5)
	{
		float hemi = N.y * 0.5 + 0.5;
		vec3 irradiance = mix(u_AmbientGround.rgb, u_AmbientSky.rgb, hemi) * u_AmbientSky.w;
		vec3 kS = FresnelSchlick(max(dot(N, V), 0.0), F0);
		vec3 kD = (vec3_splat(1.0) - kS) * (1.0 - metallic);
		ambient = irradiance * albedo * kD + irradiance * F0 * 0.25;
	}
	else
	{
		ambient = albedo * 0.03;
	}

	gl_FragData[0] = vec4(ambient + Lo, albedoSample.a);
	gl_FragData[1] = vec4_splat(v_entityid);
}
