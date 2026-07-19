// Shared varying/attribute declarations for GanymedE shaders.
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
//
// Note a_position is declared vec3. Fullscreen passes bind a 2-component
// position buffer; bgfx fills the missing components with (0,0,1), so those
// shaders simply use a_position.xy.

vec4  v_color0    : COLOR0    = vec4(1.0, 1.0, 1.0, 1.0);
vec3  v_worldpos  : TEXCOORD7 = vec3(0.0, 0.0, 0.0);
vec3  v_normal    : NORMAL    = vec3(0.0, 1.0, 0.0);
vec3  v_tangent   : TANGENT   = vec3(1.0, 0.0, 0.0);
vec2  v_texcoord0 : TEXCOORD0 = vec2(0.0, 0.0);
float v_texindex  : TEXCOORD1 = 0.0;
float v_tiling    : TEXCOORD2 = 1.0;
float v_entityid  : TEXCOORD3 = -1.0;
vec4  v_shadowpos : TEXCOORD4 = vec4(0.0, 0.0, 0.0, 1.0);

vec3  a_position  : POSITION;
vec3  a_normal    : NORMAL;
vec3  a_tangent   : TANGENT;
vec4  a_color0    : COLOR0;
vec2  a_texcoord0 : TEXCOORD0;
float a_texcoord1 : TEXCOORD1;
float a_texcoord2 : TEXCOORD2;
float a_texcoord3 : TEXCOORD3;

// Per-instance data. These semantics are NOT free to choose: bgfx binds the
// instance data buffer to TEXCOORD31 counting DOWN, so i_data0..4 must be
// TEXCOORD31..27 exactly (see bgfx examples/05-instancing/varying.def.sc).
// Anything else silently reads undefined data - the symptom is geometry drawn
// with a garbage transform rather than any error.
vec4 i_data0 : TEXCOORD31;
vec4 i_data1 : TEXCOORD30;
vec4 i_data2 : TEXCOORD29;
vec4 i_data3 : TEXCOORD28;
vec4 i_data4 : TEXCOORD27;
