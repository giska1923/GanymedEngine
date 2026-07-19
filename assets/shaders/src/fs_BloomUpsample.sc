$input v_texcoord0

#include <bgfx_shader.sh>

// Bloom upsample: 9-tap tent filter. Blended additively onto the next-larger
// mip target (the additive blend state is set by the C++ pass).

SAMPLER2D(u_Texture, 0);

uniform vec4 u_TexelSize;    // .xy = 1 / source resolution
uniform vec4 u_FilterRadius; // .x

void main()
{
	vec2 r = u_TexelSize.xy * u_FilterRadius.x;

	vec3 a = texture2D(u_Texture, v_texcoord0 + vec2(-r.x,  r.y)).rgb;
	vec3 b = texture2D(u_Texture, v_texcoord0 + vec2( 0.0,  r.y)).rgb;
	vec3 c = texture2D(u_Texture, v_texcoord0 + vec2( r.x,  r.y)).rgb;
	vec3 d = texture2D(u_Texture, v_texcoord0 + vec2(-r.x,  0.0)).rgb;
	vec3 e = texture2D(u_Texture, v_texcoord0).rgb;
	vec3 f = texture2D(u_Texture, v_texcoord0 + vec2( r.x,  0.0)).rgb;
	vec3 g = texture2D(u_Texture, v_texcoord0 + vec2(-r.x, -r.y)).rgb;
	vec3 h = texture2D(u_Texture, v_texcoord0 + vec2( 0.0, -r.y)).rgb;
	vec3 i = texture2D(u_Texture, v_texcoord0 + vec2( r.x, -r.y)).rgb;

	// Tent kernel: centre 4, edges 2, corners 1, over 16
	vec3 result = e * 4.0;
	result += (b + d + f + h) * 2.0;
	result += (a + c + g + i);
	result *= 1.0 / 16.0;

	gl_FragColor = vec4(result, 1.0);
}
