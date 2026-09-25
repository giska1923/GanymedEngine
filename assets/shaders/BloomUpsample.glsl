// Bloom upsample: 9-tap tent filter. Blended additively onto the next-larger
// mip target (GL blend ONE, ONE set by the C++ pass).

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
uniform vec2 u_TexelSize; // 1 / source resolution
uniform float u_FilterRadius;

void main()
{
	vec2 t = u_TexelSize * u_FilterRadius;
	vec2 uv = v_TexCoord;

	vec3 result = texture(u_Texture, uv).rgb * 4.0;
	result += texture(u_Texture, uv + vec2(-t.x,  0.0)).rgb * 2.0;
	result += texture(u_Texture, uv + vec2( t.x,  0.0)).rgb * 2.0;
	result += texture(u_Texture, uv + vec2( 0.0, -t.y)).rgb * 2.0;
	result += texture(u_Texture, uv + vec2( 0.0,  t.y)).rgb * 2.0;
	result += texture(u_Texture, uv + vec2(-t.x, -t.y)).rgb;
	result += texture(u_Texture, uv + vec2( t.x, -t.y)).rgb;
	result += texture(u_Texture, uv + vec2(-t.x,  t.y)).rgb;
	result += texture(u_Texture, uv + vec2( t.x,  t.y)).rgb;
	result /= 16.0;

	color = vec4(result, 1.0);
}
