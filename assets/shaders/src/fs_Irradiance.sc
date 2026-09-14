$input v_worldpos

#include <bgfx_shader.sh>

// Convolves the environment cubemap into a diffuse irradiance cubemap.

SAMPLERCUBE(u_EnvironmentMap, 0);

#define PI 3.14159265359

// Integer bounds, not `phi += 0.025`. Mesa's ANV SPIR-V compiler has produced
// non-terminating fragment loops from float-increment hemisphere walks, which
// shows up as i915 `GPU hung` / `VK_ERROR_DEVICE_LOST` on the first frame that
// runs the bake (Intel UHD, Linux Vulkan). 256 x 64 is the same order as the
// old 0.025-radian grid (~15.8k samples).
#define PHI_SAMPLES 256
#define THETA_SAMPLES 64

void main()
{
	vec3 N = normalize(v_worldpos);

	// When N is colinear with +Y the original `cross((0,1,0), N)` is a zero
	// vector and `normalize` is NaN - the +Y/-Y cube faces baked black. Pick
	// a different up when N is near the Y axis.
	vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);
	vec3 right = normalize(cross(up, N));
	up = normalize(cross(N, right));

	vec3 irradiance = vec3_splat(0.0);
	for (int i = 0; i < PHI_SAMPLES; i++)
	{
		float phi = (float(i) + 0.5) * (2.0 * PI / float(PHI_SAMPLES));
		for (int j = 0; j < THETA_SAMPLES; j++)
		{
			float theta = (float(j) + 0.5) * (0.5 * PI / float(THETA_SAMPLES));
			vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
			vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
			irradiance += textureCube(u_EnvironmentMap, sampleVec).rgb * cos(theta) * sin(theta);
		}
	}
	irradiance = PI * irradiance * (1.0 / float(PHI_SAMPLES * THETA_SAMPLES));

	gl_FragColor = vec4(irradiance, 1.0);
}
