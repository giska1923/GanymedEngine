// Projects an equirectangular HDR panorama onto the six faces of a cubemap.

#type vertex
#version 330 core

layout(location = 0) in vec3 a_Position;

uniform mat4 u_Projection;
uniform mat4 u_View;

out vec3 v_LocalPos;

void main()
{
	v_LocalPos = a_Position;
	gl_Position = u_Projection * u_View * vec4(a_Position, 1.0);
}

#type fragment
#version 330 core

layout(location = 0) out vec4 color;

in vec3 v_LocalPos;

uniform sampler2D u_EquirectangularMap;

const vec2 invAtan = vec2(0.1591, 0.3183);

vec2 SampleSphericalMap(vec3 v)
{
	vec2 uv = vec2(atan(v.z, v.x), asin(v.y));
	uv *= invAtan;
	uv += 0.5;
	return uv;
}

void main()
{
	vec2 uv = SampleSphericalMap(normalize(v_LocalPos));
	color = vec4(texture(u_EquirectangularMap, uv).rgb, 1.0);
}
