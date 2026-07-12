// Fullscreen HDR -> LDR tonemap (ACES filmic approximation) with exposure.

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
uniform float u_Exposure;

// Narkowicz 2015 ACES filmic curve
vec3 ACESFilm(vec3 x)
{
	const float a = 2.51;
	const float b = 0.03;
	const float c = 2.43;
	const float d = 0.59;
	const float e = 0.14;
	return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main()
{
	vec3 hdr = texture(u_Texture, v_TexCoord).rgb;
	hdr *= u_Exposure;

	vec3 mapped = ACESFilm(hdr);

	// Output is sampled by ImGui as an sRGB-ish 8-bit image; apply gamma
	mapped = pow(mapped, vec3(1.0 / 2.2));

	color = vec4(mapped, 1.0);
}
