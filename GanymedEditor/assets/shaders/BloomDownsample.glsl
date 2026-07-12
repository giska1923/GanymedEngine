// Bloom downsample (13-tap, Jimenez SIGGRAPH 2014 style). The first pass of the
// chain applies a soft-knee luminance threshold; later passes just filter.

#type vertex
#version 330 core

layout(location = 0) in vec2 a_Position;

out vec2 v_TexCoord;

void main()
{
	v_TexCoord = a_Position * 0.5 + 0.5;
	gl_Position = vec4(a_Position, 0.0, 1.0);
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;

in vec2 v_TexCoord;

uniform sampler2D u_Texture;
uniform vec2 u_TexelSize;     // 1 / source resolution
uniform int u_FirstPass;      // 1 = apply threshold (sampling the HDR scene)
uniform float u_Threshold;    // luminance threshold
uniform float u_Knee;         // soft knee width (fraction of threshold)

vec3 SampleSrc(vec2 uv)
{
	return texture(u_Texture, uv).rgb;
}

// Quadratic soft threshold around u_Threshold
vec3 Prefilter(vec3 c)
{
	float brightness = max(c.r, max(c.g, c.b));
	float knee = u_Threshold * u_Knee;
	float soft = brightness - u_Threshold + knee;
	soft = clamp(soft, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee + 1e-4);
	float contribution = max(soft, brightness - u_Threshold) / max(brightness, 1e-4);
	return c * contribution;
}

// Reduce fireflies on the first downsample by weighting with luma
vec3 KarisAverage(vec3 a, vec3 b, vec3 c, vec3 d)
{
	float wa = 1.0 / (1.0 + max(a.r, max(a.g, a.b)));
	float wb = 1.0 / (1.0 + max(b.r, max(b.g, b.b)));
	float wc = 1.0 / (1.0 + max(c.r, max(c.g, c.b)));
	float wd = 1.0 / (1.0 + max(d.r, max(d.g, d.b)));
	return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

void main()
{
	vec2 uv = v_TexCoord;
	vec2 t = u_TexelSize;

	// 13-tap pattern: 4 inner box samples + 9 on a 2-texel grid
	vec3 a = SampleSrc(uv + t * vec2(-2.0,  2.0));
	vec3 b = SampleSrc(uv + t * vec2( 0.0,  2.0));
	vec3 c = SampleSrc(uv + t * vec2( 2.0,  2.0));
	vec3 d = SampleSrc(uv + t * vec2(-2.0,  0.0));
	vec3 e = SampleSrc(uv);
	vec3 f = SampleSrc(uv + t * vec2( 2.0,  0.0));
	vec3 g = SampleSrc(uv + t * vec2(-2.0, -2.0));
	vec3 h = SampleSrc(uv + t * vec2( 0.0, -2.0));
	vec3 i = SampleSrc(uv + t * vec2( 2.0, -2.0));

	vec3 j = SampleSrc(uv + t * vec2(-1.0,  1.0));
	vec3 k = SampleSrc(uv + t * vec2( 1.0,  1.0));
	vec3 l = SampleSrc(uv + t * vec2(-1.0, -1.0));
	vec3 m = SampleSrc(uv + t * vec2( 1.0, -1.0));

	vec3 result;
	if (u_FirstPass == 1)
	{
		// Karis-weighted 2x2 boxes to suppress single-pixel flicker
		vec3 box0 = KarisAverage(a, b, d, e);
		vec3 box1 = KarisAverage(b, c, e, f);
		vec3 box2 = KarisAverage(d, e, g, h);
		vec3 box3 = KarisAverage(e, f, h, i);
		vec3 box4 = KarisAverage(j, k, l, m);
		result = box4 * 0.5 + (box0 + box1 + box2 + box3) * 0.125;
		result = Prefilter(result);
	}
	else
	{
		result = e * 0.125;
		result += (a + c + g + i) * 0.03125;
		result += (b + d + f + h) * 0.0625;
		result += (j + k + l + m) * 0.125;
	}

	color = vec4(result, 1.0);
}
