// Per-shader varying override for RmlUi.
//
// Rml::Vertex is fixed by RmlUi as vec2 position + premultiplied RGBA8 colour +
// vec2 texcoord, which matches neither the engine's shared varying.def.sc (vec3
// positions) nor ImGui's (whose colour and texcoord are the other way round).
// compile_shaders picks this up automatically because it is named
// varying.<name>.def.sc.
//
// Attribute ORDER here must match the bgfx::VertexLayout in RmlUiRendererBgfx,
// which in turn must match the field order of Rml::Vertex.

vec2 v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
vec4 v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);

vec2 a_position  : POSITION;
vec4 a_color0    : COLOR0;
vec2 a_texcoord0 : TEXCOORD0;
