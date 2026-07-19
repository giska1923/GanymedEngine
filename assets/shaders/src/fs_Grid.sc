$input v_worldpos

#include <bgfx_shader.sh>

// Infinite-style ground grid (XZ plane).
// u_CameraPosition is supplied every draw by FrameUniforms.

uniform vec4 u_CameraPosition; // .xyz

float gridLine(vec2 uv)
{
	vec2 d = fwidth(uv);
	vec2 a = abs(fract(uv - 0.5) - 0.5) / max(d, vec2(1e-6, 1e-6));
	// 'line' is a reserved word in HLSL.
	float d0 = min(a.x, a.y);
	return 1.0 - min(d0, 1.0);
}

void main()
{
	vec2 uv = v_worldpos.xz;

	float minor = gridLine(uv);
	float major = gridLine(uv * 0.1);

	vec3 gridColor = mix(vec3_splat(0.25), vec3_splat(0.45), major);
	float alpha = max(minor * 0.35, major * 0.55);

	float dist = length(u_CameraPosition.xz - v_worldpos.xz);
	float fade = 1.0 - smoothstep(20.0, 80.0, dist);
	alpha *= fade;

	if (alpha < 0.01)
		discard;

	// Axis emphasis
	float axisWidth = max(fwidth(uv.x), fwidth(uv.y)) * 1.5;
	if (abs(uv.x) < axisWidth)
	{
		gridColor = vec3(0.85, 0.2, 0.2);
		alpha = max(alpha, 0.85 * fade);
	}
	if (abs(uv.y) < axisWidth)
	{
		gridColor = vec3(0.2, 0.85, 0.2);
		alpha = max(alpha, 0.85 * fade);
	}

	gl_FragData[0] = vec4(gridColor, alpha);
	gl_FragData[1] = vec4_splat(-1.0);
}
