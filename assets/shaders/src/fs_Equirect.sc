$input v_worldpos

#include <bgfx_shader.sh>

// Projects an equirectangular HDR panorama onto the six faces of a cubemap.

SAMPLER2D(u_EquirectangularMap, 0);

vec2 SampleSphericalMap(vec3 v)
{
	vec2 invAtan = vec2(0.1591, 0.3183);
	vec2 uv = vec2(atan2(v.z, v.x), asin(v.y));
	uv *= invAtan;
	uv += 0.5;

	// Flip V. asin(v.y) maps +Y (up) to v=1.0, which was the TOP of the image
	// under the old GL loader's vertical flip - but bgfx's texture origin is
	// top-left and Texture2D no longer flips, so v=1.0 is now the BOTTOM.
	// Without this the sky samples the floor and vice versa.
	uv.y = 1.0 - uv.y;
	return uv;
}

void main()
{
	vec2 uv = SampleSphericalMap(normalize(v_worldpos));
	gl_FragColor = vec4(texture2D(u_EquirectangularMap, uv).rgb, 1.0);
}
