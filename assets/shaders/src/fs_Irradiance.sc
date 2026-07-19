$input v_worldpos

#include <bgfx_shader.sh>

// Convolves the environment cubemap into a diffuse irradiance cubemap.

SAMPLERCUBE(u_EnvironmentMap, 0);

#define PI 3.14159265359

void main()
{
	vec3 N = normalize(v_worldpos);

	vec3 irradiance = vec3_splat(0.0);

	vec3 up = vec3(0.0, 1.0, 0.0);
	vec3 right = normalize(cross(up, N));
	up = normalize(cross(N, right));

	float sampleDelta = 0.025;
	float nrSamples = 0.0;
	for (float phi = 0.0; phi < 2.0 * PI; phi += sampleDelta)
	{
		for (float theta = 0.0; theta < 0.5 * PI; theta += sampleDelta)
		{
			vec3 tangentSample = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
			vec3 sampleVec = tangentSample.x * right + tangentSample.y * up + tangentSample.z * N;
			irradiance += textureCube(u_EnvironmentMap, sampleVec).rgb * cos(theta) * sin(theta);
			nrSamples++;
		}
	}
	irradiance = PI * irradiance * (1.0 / nrSamples);

	gl_FragColor = vec4(irradiance, 1.0);
}
