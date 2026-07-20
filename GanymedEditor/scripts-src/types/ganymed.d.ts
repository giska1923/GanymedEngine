/** @noSelfInFile */

// The TypeScript mirror of GanymedEngine/source/GanymedE/Scripting/ScriptBindings.cpp.
//
// KEEP THE TWO IN SYNC. Nothing enforces it — this file is the only thing giving you IntelliSense
// and compile errors against the real engine API, and it is hand-written. If a binding is added or
// its signature changes on the C++ side, change it here in the same edit.
//
// `/** @noSelfInFile */` plus "noImplicitSelf" in tsconfig stop TSTL from inserting a `self`
// parameter on these *static* calls, so `Input.IsKeyPressed(...)` emits a `.` call, not a `:` call.

/** 3-component vector. Arithmetic operators are bound as Lua metamethods. */
declare interface Vec3 {
	x: number;
	y: number;
	z: number;

	Length(): number;
	/** Zero-length vectors return (0,0,0) rather than NaN. */
	Normalized(): Vec3;
	Dot(other: Vec3): number;
	Cross(other: Vec3): Vec3;
}

declare const Vec3: {
	(): Vec3;
	(scalar: number): Vec3;
	(x: number, y: number, z: number): Vec3;
};

declare interface Entity {
	GetName(): string;
	GetUUID(): number;
	IsValid(): boolean;

	/**
	 * Returns a COPY. Mutate it, then call the setter.
	 *
	 * The setter is not a style preference: it routes the write through the engine's change
	 * tracking, without which the world-transform cache goes stale and the entity does not move
	 * on screen even though its component data changed.
	 */
	GetTranslation(): Vec3;
	SetTranslation(value: Vec3): void;

	/** Euler angles, in radians. */
	GetRotation(): Vec3;
	SetRotation(value: Vec3): void;

	GetScale(): Vec3;
	SetScale(value: Vec3): void;

	HasRigidBody(): boolean;
}

/** The shape every gameplay script implements. All hooks are optional. */
declare interface Script {
	/** Injected by ScriptEngine before OnCreate runs. */
	entity: Entity;

	OnCreate?(): void;
	OnUpdate?(ts: number): void;
	OnCollisionEnter?(other: Entity): void;
	OnCollisionExit?(other: Entity): void;
	OnDestroy?(): void;
}

declare namespace Input {
	function IsKeyPressed(key: number): boolean;
	function IsMouseButtonPressed(button: number): boolean;
	/** Returns [x, y]. */
	function GetMousePosition(): LuaMultiReturn<[number, number]>;
}

declare namespace Key {
	const Space: number; const Apostrophe: number; const Comma: number; const Minus: number;
	const Period: number; const Slash: number; const Semicolon: number; const Equal: number;

	const D0: number; const D1: number; const D2: number; const D3: number; const D4: number;
	const D5: number; const D6: number; const D7: number; const D8: number; const D9: number;

	const A: number; const B: number; const C: number; const D: number; const E: number;
	const F: number; const G: number; const H: number; const I: number; const J: number;
	const K: number; const L: number; const M: number; const N: number; const O: number;
	const P: number; const Q: number; const R: number; const S: number; const T: number;
	const U: number; const V: number; const W: number; const X: number; const Y: number;
	const Z: number;

	const LeftBracket: number; const Backslash: number; const RightBracket: number;
	const GraveAccent: number;

	const Escape: number; const Enter: number; const Tab: number; const Backspace: number;
	const Insert: number; const Delete: number; const Right: number; const Left: number;
	const Down: number; const Up: number; const PageUp: number; const PageDown: number;
	const Home: number; const End: number; const CapsLock: number; const ScrollLock: number;
	const NumLock: number; const PrintScreen: number; const Pause: number;

	const F1: number; const F2: number; const F3: number; const F4: number; const F5: number;
	const F6: number; const F7: number; const F8: number; const F9: number; const F10: number;
	const F11: number; const F12: number;

	const LeftShift: number; const LeftControl: number; const LeftAlt: number;
	const LeftSuper: number; const RightShift: number; const RightControl: number;
	const RightAlt: number; const RightSuper: number; const Menu: number;
}

declare namespace Mouse {
	const ButtonLeft: number;
	const ButtonRight: number;
	const ButtonMiddle: number;
}

/** Routed to the engine's CLIENT logger — script output is game output. */
declare namespace Log {
	function Trace(message: string): void;
	function Info(message: string): void;
	function Warn(message: string): void;
	function Error(message: string): void;
}

declare namespace Scene {
	/** Linear scan over tags. Fine for setup; do not call it every frame. */
	function FindEntityByName(name: string): Entity | undefined;
}

/**
 * The game HUD's data model (RmlUi). Setting a value marks it dirty, so any
 * {{expression}} referencing it re-evaluates on the next UI update.
 *
 * Fixed setters rather than a generic bag: RmlUi binds its data model to real
 * C++ addresses declared before any document loads.
 */
declare namespace UI {
	/** 0..100; the health bar's width is bound to this. */
	function SetHealth(health: number): void;
	function SetScore(score: number): void;
	function GetHealth(): number;
	function GetScore(): number;
}
