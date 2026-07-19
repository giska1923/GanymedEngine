$input v_texcoord0

#include <bgfx_shader.sh>

// Fullscreen HDR -> LDR tonemap (ACES filmic approximation) with exposure.
//
// Scalar uniforms arrive as vec4 with the value in .x - bgfx has no float or
// int uniform type, so Shader::SetFloat / SetInt pad into a vec4.

SAMPLER2D(u_Texture,      0);
SAMPLER2D(u_BloomTexture, 1);

uniform vec4 u_Exposure;       // .x
uniform vec4 u_BloomIntensity; // .x
uniform vec4 u_UseBloom;       // .x = 0 or 1

// Narkowicz 2015 ACES filmic curve
vec3 ACESFilm(vec3 x)
{
	float a = 2.51;
	float b = 0.03;
	float c = 2.43;
	float d = 0.59;
	float e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
	vec3 hdr = texture2D(u_Texture, v_texcoord0).rgb;

	if (u_UseBloom.x > 0.5)
		hdr += texture2D(u_BloomTexture, v_texcoord0).rgb * u_BloomIntensity.x;

	hdr *= u_Exposure.x;

	vec3 mapped = ACESFilm(hdr);

	// Output is sampled by ImGui as an sRGB-ish 8-bit image; apply gamma
	mapped = pow(mapped, vec3_splat(1.0 / 2.2));

	gl_FragColor = vec4(mapped, 1.0);
}
