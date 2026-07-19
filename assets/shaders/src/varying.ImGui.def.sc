// Per-shader varying override for ImGui.
//
// ImDrawVert is fixed by ImGui as vec2 position + vec2 uv + packed uint8 colour,
// which does not match the engine's shared varying.def.sc (vec3 positions).
// compile_shaders picks this up automatically because it is named
// varying.<name>.def.sc.

vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);

vec2 a_position  : POSITION;
vec2 a_texcoord0 : TEXCOORD0;
vec4 a_color0    : COLOR0;
