$input v_worldpos

#include <bgfx_shader.sh>

// Pre-filtered specular environment: one cubemap mip per roughness level.
// Importance-sampling maths ported verbatim; only declarations changed.

SAMPLERCUBE(u_EnvironmentMap, 0);

uniform vec4 u_Roughness;  // .x
uniform vec4 u_Resolution; // .x = source cubemap face resolution, .y = its highest valid mip

#define PI 3.14159265359
// Compile-time loop bound: a mutable `uint SAMPLE_COUNT = 1024u` is a dynamic
// loop to some SPIR-V compilers, and Intel ANV has hung on those.
//
// 128, not 1024, and this one is free rather than a trade: the loop below already picks a mip of
// the environment cubemap from the sample PDF and reads it with textureCubeLod. That technique
// (Karis, "Real Shading in Unreal Engine 4") exists precisely so that a low sample count does not
// alias - the mip does the averaging the missing samples would have done. Karis uses 64-128.
// Sampling 1024 times into a mip chain built for 128 is paying eight times over for the same
// answer.
#define SAMPLE_COUNT 128u

float RadicalInverse_VdC(uint bits)
{
	bits = (bits << 16u) | (bits >> 16u);
	bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
	bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
	bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
	bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
	return float(bits) * 2.3283064365386963e-10;
}

vec2 Hammersley(uint i, uint N)
{
	return vec2(float(i) / float(N), RadicalInverse_VdC(i));
}

vec3 ImportanceSampleGGX(vec2 Xi, vec3 N, float roughness)
{
	float a = roughness * roughness;

	float phi = 2.0 * PI * Xi.x;
	float cosTheta = sqrt((1.0 - Xi.y) / (1.0 + (a * a - 1.0) * Xi.y));
	float sinTheta = sqrt(1.0 - cosTheta * cosTheta);

	vec3 H = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

	vec3 up = abs(N.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
	vec3 tangent = normalize(cross(up, N));
	vec3 bitangent = cross(N, tangent);

	return normalize(tangent * H.x + bitangent * H.y + N * H.z);
}

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	float NdotH2 = NdotH * NdotH;
	float denom = (NdotH2 * (a2 - 1.0) + 1.0);
	denom = PI * denom * denom;
	return a2 / denom;
}

void main()
{
	vec3 N = normalize(v_worldpos);
	vec3 R = N;
	vec3 V = R;

	vec3 prefilteredColor = vec3_splat(0.0);
	float totalWeight = 0.0;

	for (uint i = 0u; i < SAMPLE_COUNT; i++)
	{
		vec2 Xi = Hammersley(i, SAMPLE_COUNT);
		vec3 H = ImportanceSampleGGX(Xi, N, u_Roughness.x);
		vec3 L = normalize(2.0 * dot(V, H) * H - V);

		float NdotL = max(dot(N, L), 0.0);
		if (NdotL > 0.0)
		{
			// Sample from the environment's mip level based on roughness/pdf to reduce fireflies
			float D = DistributionGGX(N, H, u_Roughness.x);
			float NdotH = max(dot(N, H), 0.0);
			float HdotV = max(dot(H, V), 0.0);
			float pdf = D * NdotH / (4.0 * HdotV) + 0.0001;

			float saTexel = 4.0 * PI / (6.0 * u_Resolution.x * u_Resolution.x);
			float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 0.0001);
			float mipLevel = u_Roughness.x == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);

			// **Clamped, and it was not before.** The source cubemap has 5 mips, so the only
			// levels that exist are 0..4 - and this formula asks for up to 11.6. It exceeds 4
			// even at the GGX peak once roughness reaches 0.5, so it is not an edge case: every
			// rough material was sampling past the end of the chain on every backend, which is a
			// correctness bug quite apart from what it does to a driver.
			//
			// It is also the one thing this stage does that no other bake stage does - an
			// explicit, computed, out-of-range LOD - and this stage is the one that hangs Intel
			// ANV while the irradiance convolution beside it finishes 25M cube samples in 21 ms.
			mipLevel = clamp(mipLevel, 0.0, u_Resolution.y);

			prefilteredColor += textureCubeLod(u_EnvironmentMap, L, mipLevel).rgb * NdotL;
			totalWeight += NdotL;
		}
	}

	prefilteredColor = prefilteredColor / totalWeight;
	gl_FragColor = vec4(prefilteredColor, 1.0);
}
