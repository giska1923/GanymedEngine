$input v_texcoord0

#include <bgfx_shader.sh>

// FXAA 3.x-style edge blend. Ported verbatim from the GLSL original - the body
// is pure math, so only the declarations changed: sampler macro, vec4 uniforms
// (component access like u_TexelSize.x is unchanged), and gl_FragColor.

SAMPLER2D(u_Texture, 0);
uniform vec4 u_TexelSize; // .xy = 1 / resolution (bgfx uniforms are vec4)

#define FXAA_EDGE_THRESHOLD_MIN 0.0312
#define FXAA_EDGE_THRESHOLD_MAX 0.125
#define FXAA_ITERATIONS 12
#define FXAA_SUBPIXEL_QUALITY 0.75

float Luma(vec3 rgb)
{
	return dot(rgb, vec3(0.299, 0.587, 0.114));
}

void main()
{
	vec3 centerColor = texture2D(u_Texture, v_texcoord0).rgb;
	float lumaCenter = Luma(centerColor);

	float lumaDown  = Luma(texture2D(u_Texture, v_texcoord0 + vec2( 0.0, -u_TexelSize.y)).rgb);
	float lumaUp    = Luma(texture2D(u_Texture, v_texcoord0 + vec2( 0.0,  u_TexelSize.y)).rgb);
	float lumaLeft  = Luma(texture2D(u_Texture, v_texcoord0 + vec2(-u_TexelSize.x, 0.0)).rgb);
	float lumaRight = Luma(texture2D(u_Texture, v_texcoord0 + vec2( u_TexelSize.x, 0.0)).rgb);

	float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
	float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
	float lumaRange = lumaMax - lumaMin;

	// Early out: not on an edge
	if (lumaRange < max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD_MAX))
	{
		gl_FragColor = vec4(centerColor, 1.0);
		return;
	}

	float lumaDownLeft  = Luma(texture2D(u_Texture, v_texcoord0 + vec2(-u_TexelSize.x, -u_TexelSize.y)).rgb);
	float lumaUpRight   = Luma(texture2D(u_Texture, v_texcoord0 + vec2( u_TexelSize.x,  u_TexelSize.y)).rgb);
	float lumaUpLeft    = Luma(texture2D(u_Texture, v_texcoord0 + vec2(-u_TexelSize.x,  u_TexelSize.y)).rgb);
	float lumaDownRight = Luma(texture2D(u_Texture, v_texcoord0 + vec2( u_TexelSize.x, -u_TexelSize.y)).rgb);

	float lumaDownUp    = lumaDown + lumaUp;
	float lumaLeftRight = lumaLeft + lumaRight;
	float lumaLeftCorners  = lumaDownLeft + lumaUpLeft;
	float lumaDownCorners  = lumaDownLeft + lumaDownRight;
	float lumaRightCorners = lumaDownRight + lumaUpRight;
	float lumaUpCorners    = lumaUpRight + lumaUpLeft;

	float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) + abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 + abs(-2.0 * lumaRight + lumaRightCorners);
	float edgeVertical   = abs(-2.0 * lumaUp + lumaUpCorners) + abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 + abs(-2.0 * lumaDown + lumaDownCorners);
	bool isHorizontal = (edgeHorizontal >= edgeVertical);

	float luma1 = isHorizontal ? lumaDown : lumaLeft;
	float luma2 = isHorizontal ? lumaUp : lumaRight;
	float gradient1 = luma1 - lumaCenter;
	float gradient2 = luma2 - lumaCenter;
	bool is1Steepest = abs(gradient1) >= abs(gradient2);
	float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

	float stepLength = isHorizontal ? u_TexelSize.y : u_TexelSize.x;
	float lumaLocalAverage = 0.0;
	if (is1Steepest)
	{
		stepLength = -stepLength;
		lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
	}
	else
	{
		lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
	}

	vec2 currentUv = v_texcoord0;
	if (isHorizontal)
		currentUv.y += stepLength * 0.5;
	else
		currentUv.x += stepLength * 0.5;

	// March along the edge in both directions until the luma delta ends the edge
	vec2 offset = isHorizontal ? vec2(u_TexelSize.x, 0.0) : vec2(0.0, u_TexelSize.y);
	vec2 uv1 = currentUv - offset;
	vec2 uv2 = currentUv + offset;

	float lumaEnd1 = Luma(texture2D(u_Texture, uv1).rgb) - lumaLocalAverage;
	float lumaEnd2 = Luma(texture2D(u_Texture, uv2).rgb) - lumaLocalAverage;
	bool reached1 = abs(lumaEnd1) >= gradientScaled;
	bool reached2 = abs(lumaEnd2) >= gradientScaled;
	bool reachedBoth = reached1 && reached2;

	if (!reached1) uv1 -= offset;
	if (!reached2) uv2 += offset;

	if (!reachedBoth)
	{
		for (int i = 2; i < FXAA_ITERATIONS; i++)
		{
			if (!reached1)
			{
				lumaEnd1 = Luma(texture2D(u_Texture, uv1).rgb) - lumaLocalAverage;
				reached1 = abs(lumaEnd1) >= gradientScaled;
			}
			if (!reached2)
			{
				lumaEnd2 = Luma(texture2D(u_Texture, uv2).rgb) - lumaLocalAverage;
				reached2 = abs(lumaEnd2) >= gradientScaled;
			}
			reachedBoth = reached1 && reached2;

			if (!reached1) uv1 -= offset * (float(i) * 0.5 + 0.5);
			if (!reached2) uv2 += offset * (float(i) * 0.5 + 0.5);
			if (reachedBoth) break;
		}
	}

	float distance1 = isHorizontal ? (v_texcoord0.x - uv1.x) : (v_texcoord0.y - uv1.y);
	float distance2 = isHorizontal ? (uv2.x - v_texcoord0.x) : (uv2.y - v_texcoord0.y);

	bool isDirection1 = distance1 < distance2;
	float distanceFinal = min(distance1, distance2);
	float edgeThickness = (distance1 + distance2);

	bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
	bool correctVariation1 = (lumaEnd1 < 0.0) != isLumaCenterSmaller;
	bool correctVariation2 = (lumaEnd2 < 0.0) != isLumaCenterSmaller;
	bool correctVariation = isDirection1 ? correctVariation1 : correctVariation2;

	float pixelOffset = -distanceFinal / edgeThickness + 0.5;
	float finalOffset = correctVariation ? pixelOffset : 0.0;

	// Subpixel anti-aliasing
	float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
	float subPixelOffset1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
	float subPixelOffset2 = (-2.0 * subPixelOffset1 + 3.0) * subPixelOffset1 * subPixelOffset1;
	float subPixelOffsetFinal = subPixelOffset2 * subPixelOffset2 * FXAA_SUBPIXEL_QUALITY;
	finalOffset = max(finalOffset, subPixelOffsetFinal);

	vec2 finalUv = v_texcoord0;
	if (isHorizontal)
		finalUv.y += finalOffset * stepLength;
	else
		finalUv.x += finalOffset * stepLength;

	gl_FragColor = vec4(texture2D(u_Texture, finalUv).rgb, 1.0);
}
