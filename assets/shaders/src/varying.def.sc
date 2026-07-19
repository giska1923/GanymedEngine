// Shared varying/attribute declarations for every GanymedE shader.
//
// bgfx resolves vertex inputs by *semantic*, not by name, so the semantics on
// the right must match the mapping in GanymedE/Renderer/Buffer.h
// (AttribFromName). Change one, change the other.
//
//   engine attribute   bgfx semantic   declared below
//   ----------------   -------------   --------------
//   a_Position         POSITION        a_position
//   a_Normal           NORMAL          a_normal
//   a_Tangent          TANGENT         a_tangent
//   a_Color            COLOR0          a_color0
//   a_TexCoord         TEXCOORD0       a_texcoord0
//   a_TexIndex         TEXCOORD1       a_texcoord1
//   a_TilingFactor     TEXCOORD2       a_texcoord2
//   a_EntityID         TEXCOORD3       a_texcoord3

vec4  v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec3  v_position  : TEXCOORD7 = vec3(0.0, 0.0, 0.0);
vec3  v_normal    : NORMAL    = vec3(0.0, 1.0, 0.0);
vec3  v_tangent   : TANGENT   = vec3(1.0, 0.0, 0.0);
vec2  v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
float v_texindex  : TEXCOORD1 = 0.0;
float v_tiling    : TEXCOORD2 = 1.0;
float v_entityid  : TEXCOORD3 = -1.0;

vec3  a_position  : POSITION;
vec3  a_normal    : NORMAL;
vec3  a_tangent   : TANGENT;
vec4  a_color0    : COLOR0;
vec2  a_texcoord0 : TEXCOORD0;
float a_texcoord1 : TEXCOORD1;
float a_texcoord2 : TEXCOORD2;
float a_texcoord3 : TEXCOORD3;

// Per-instance data (bgfx feeds these from setInstanceDataBuffer).
vec4 i_data0 : TEXCOORD4;
vec4 i_data1 : TEXCOORD5;
vec4 i_data2 : TEXCOORD6;
vec4 i_data3 : TEXCOORD7;
vec4 i_data4 : COLOR1;
