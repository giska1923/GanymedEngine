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
	/**
	 * The entity's stable id. **Opaque**: equality and table keys work, arithmetic does not.
	 *
	 * It is a full 64-bit value carried in Lua's 64-bit integer, so about half of all ids
	 * print as negative. That is the representation, not a bug — pass the value back to
	 * `Scene.FindEntityByUUID` unchanged and it resolves.
	 */
	GetUUID(): number;
	IsValid(): boolean;

	/**
	 * Direct children only, first tag match. `Scene.FindEntityByName` is a global first-match,
	 * so two boxes both parenting a child named `"Sparks"` cannot use it.
	 */
	GetChildByName(name: string): Entity | undefined;

	/**
	 * Despawn this entity **and everything under it**.
	 *
	 * Takes effect on the next frame, the mirror of `Scene.Spawn` and for the same reason:
	 * structural changes from inside the update are queued. The entity stays usable for the
	 * rest of this frame.
	 */
	Destroy(): void;

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

	/**
	 * Where the entity was drawn LAST frame, in world space. Read-only.
	 *
	 * The only correct way to find an entity on a bone socket (or under one): its local
	 * transform is not where it is drawn. One frame old because scripts run before the
	 * transform and socket systems, so a Set* earlier in the same update is not reflected,
	 * and an entity's first frame reads the origin.
	 */
	GetWorldPosition(): Vec3;
	/** The entity's world -Z (engine forward), unit length, with the same one-frame lag. */
	GetWorldForward(): Vec3;

	HasRigidBody(): boolean;
	HasAnimator(): boolean;
	HasAudioSource(): boolean;
	HasParticleEmitter(): boolean;

	/**
	 * `MarkerComponent.Kind`, or `undefined` when this entity has no marker.
	 * There is no setter — kinds are authored in the editor.
	 */
	GetMarkerKind(): string | undefined;

	/**
	 * Switches to `name` and plays it. Switching clips is a hard cut from the start —
	 * there is no crossfade in v1.
	 *
	 * Calling this with the clip already selected does NOT restart it, it only resumes
	 * playback. That is deliberate: the natural idiom is to call this every frame from a
	 * branch, and restarting unconditionally would pin the clip at its first frame,
	 * because scripts run before the animation system each update. To restart the current
	 * clip, switch away and back.
	 *
	 * An unknown name is not an error here. The animation system warns once and holds the
	 * bind pose.
	 *
	 * No-op on an entity without an animator.
	 */
	PlayAnimation(name: string): void;

	/** Stops the clock and freezes on the current pose. Does not rewind. */
	StopAnimation(): void;

	/** Time multiplier. Negative rewinds; 0 freezes without clearing the playing flag. */
	SetAnimationSpeed(speed: number): void;
	SetAnimationLooping(loop: boolean): void;
	IsAnimationPlaying(): boolean;

	/**
	 * The clip name the animator is set to. Empty when there is no animator, no clip is
	 * selected, or — note — the name does not resolve against the mesh.
	 */
	GetCurrentAnimation(): string;

	HasBoneAttachment(): boolean;

	/**
	 * Pins this entity to `joint` on `target`'s skeleton. Offset and rotation are in joint
	 * space; rotation is Euler radians, X·Y·Z, matching `SetRotation`.
	 *
	 * If this entity already has a BoneAttachmentComponent, the fields update this frame
	 * (the attachment system runs after scripts). Adding the component is queued and
	 * becomes visible next frame, the same delay as `Scene.Spawn`.
	 *
	 * An unknown joint name is not an error here. The attachment system warns once and
	 * leaves the entity at its parent transform.
	 *
	 * No-op if `target` is missing or is this entity.
	 */
	AttachToBone(target: Entity, joint: string): void;
	AttachToBone(target: Entity, joint: string, offset: Vec3): void;
	AttachToBone(target: Entity, joint: string, offset: Vec3, rotation: Vec3): void;

	/**
	 * Clears the joint this frame (the entity snaps to its parent transform) and removes
	 * the component on the next frame. No-op if there is no attachment.
	 */
	DetachFromBone(): void;

	/**
	 * True only when this entity has a CharacterControllerComponent and is standing on ground it
	 * can walk on. A steep slope it is sliding down counts as false, which is what jumping,
	 * footsteps and landing animations all want.
	 *
	 * Always false for a rigid body: it has no such concept, and answering would invite the
	 * question to be asked of the wrong thing.
	 */
	IsGrounded(): boolean;

	/**
	 * Physics, routed to the Jolt body rather than to the transform.
	 *
	 * Writing a dynamic body's translation does nothing visible — the simulation overwrites
	 * it every step — so these are the only way to move one. All four no-op on an entity
	 * with no rigid body, and outside play, when there is no Jolt world at all.
	 *
	 * Each wakes the body first: Jolt silently discards a velocity set on a sleeping one.
	 */
	GetLinearVelocity(): Vec3;
	SetLinearVelocity(velocity: Vec3): void;
	/** A one-shot change in momentum. */
	AddImpulse(impulse: Vec3): void;
	/** Consumed by the next step — call it every frame while the push lasts. */
	AddForce(force: Vec3): void;

	/**
	 * Starts the entity's AudioSourceComponent, building its voice on first use.
	 *
	 * Calling this every frame is safe and is the intended idiom: an already-playing
	 * source is left alone, not restarted. Restarting each tick would pin the sound at
	 * its first sample, because scripts run before the audio system each update. To
	 * restart from the top, call StopSound() then PlaySound().
	 *
	 * A source that has played to its end rewinds. No-op without an AudioSourceComponent,
	 * or when the clip cannot be loaded (the engine warns once, naming the path).
	 */
	PlaySound(): void;

	/** Pauses, keeping the playback position — PlaySound() resumes from it. */
	StopSound(): void;

	IsSoundPlaying(): boolean;

	/**
	 * Linear, 1.0 = unattenuated. These three write the component, which the audio system
	 * re-reads every frame, so they apply on the next update and survive a stop/play.
	 *
	 * Clip, spatialisation and streaming are NOT settable from script: they are baked into
	 * the voice when it is created. Author them on the component.
	 */
	SetSoundVolume(volume: number): void;
	/** 1.0 = unmodified. Also changes playback speed. Values <= 0 are ignored. */
	SetSoundPitch(pitch: number): void;
	SetSoundLooping(loop: boolean): void;

	/**
	 * Starts the entity's ParticleEmitterComponent. Component-direct: the pool lives on the
	 * component, not a system-owned voice, and ParticleEmitterComponent is untracked so
	 * these writes need no change log.
	 *
	 * Calling this every frame is safe: an already-playing emitter is left alone, not
	 * restarted. Restarting each tick would reset the pool, because scripts run before
	 * the particle system each update. To freeze, call StopParticles(). There is no
	 * RestartParticles — that is an inspector preview control.
	 *
	 * No-op without a ParticleEmitterComponent. Curves, textures, meshes and blend mode
	 * are not scriptable.
	 */
	PlayParticles(): void;
	StopParticles(): void;
	IsParticlesPlaying(): boolean;

	/**
	 * Queues `count` spawns consumed this tick while playing (scripts run before the
	 * sim). Two calls in one frame add. Does not auto-play — call PlayParticles first.
	 * Not gated by Duration, so a RateOverTime=0 one-shot still bursts on later impacts.
	 * Remainder past MaxParticles stays queued. count <= 0 is ignored.
	 */
	EmitBurst(count: number): void;

	GetParticleRateOverTime(): number;
	SetParticleRateOverTime(value: number): void;
	GetParticleMaxParticles(): number;
	SetParticleMaxParticles(value: number): void;
	GetParticleLooping(): boolean;
	SetParticleLooping(value: boolean): void;
	GetParticleDuration(): number;
	SetParticleDuration(value: number): void;
	GetParticlePlayOnStart(): boolean;
	SetParticlePlayOnStart(value: boolean): void;
	GetParticleLifetimeMin(): number;
	SetParticleLifetimeMin(value: number): void;
	GetParticleLifetimeMax(): number;
	SetParticleLifetimeMax(value: number): void;
	GetParticleSpeedMin(): number;
	SetParticleSpeedMin(value: number): void;
	GetParticleSpeedMax(): number;
	SetParticleSpeedMax(value: number): void;
	GetParticleConeAngle(): number;
	SetParticleConeAngle(value: number): void;
	GetParticleStartSizeMin(): number;
	SetParticleStartSizeMin(value: number): void;
	GetParticleStartSizeMax(): number;
	SetParticleStartSizeMax(value: number): void;
	GetParticleStartRotationMin(): number;
	SetParticleStartRotationMin(value: number): void;
	GetParticleStartRotationMax(): number;
	SetParticleStartRotationMax(value: number): void;
	GetParticleRotationSpeedMin(): number;
	SetParticleRotationSpeedMin(value: number): void;
	GetParticleRotationSpeedMax(): number;
	SetParticleRotationSpeedMax(value: number): void;
	GetParticleGravityModifier(): number;
	SetParticleGravityModifier(value: number): void;
	GetParticleWorldSpace(): boolean;
	SetParticleWorldSpace(value: boolean): void;
	GetParticleSeed(): number;
	SetParticleSeed(value: number): void;
}

/** The shape every gameplay script implements. All hooks are optional. */
/**
 * Values a script exposes to the editor. Declare them as a `Properties` table on
 * the script object: the declared value is both the default and the type, and each
 * is applied to `this` before OnCreate.
 *
 * Supported types: boolean, number, string, Vec3. Anything else is skipped with a
 * warning. All numbers are floats — Lua 5.4 distinguishes 5 from 5.0, but TSTL
 * cannot express that (TypeScript has one number type), so the engine treats every
 * number the same way regardless of authoring language.
 *
 * The editor stores only the values you actually change, so editing a default in
 * the script still reaches every entity that did not override it.
 */
declare type ScriptProperties = { [name: string]: boolean | number | string | Vec3 };

declare interface Script {
	/** Injected by ScriptEngine before OnCreate runs. */
	entity: Entity;

	/** Editor-exposed tunables; see ScriptProperties. */
	Properties?: ScriptProperties;

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

	/**
	 * How far the mouse moved during the previous frame, in pixels, as [dx, dy].
	 *
	 * This — not GetMousePosition — is what mouse-look reads: with the cursor locked the absolute
	 * position is an unbounded virtual coordinate that means nothing on its own.
	 *
	 * It is an **amount, not a rate**. Do not multiply it by the frame's timestep; mouse movement
	 * is already a distance, and scaling it by time makes sensitivity depend on framerate.
	 */
	function GetMouseDelta(): [number, number];

	/** Cursor.Normal | Cursor.Hidden | Cursor.Locked. Locked is mouse-look: hidden, held to the
	 * window, raw motion where the platform supports it. */
	function SetCursorMode(mode: number): void;
	function GetCursorMode(): number;
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

	/**
	 * Look an entity up by the value `Entity.GetUUID()` returned. Unlike a tag, a UUID is
	 * unique and survives a rename, so this is what to hold across frames.
	 *
	 * **Treat the value as opaque.** It is a full 64-bit id: it round-trips exactly, but Lua
	 * prints ids with the high bit set as negative numbers, and arithmetic on one is
	 * meaningless. Store it and pass it back, nothing else.
	 */
	function FindEntityByUUID(id: number): Entity | undefined;

	/**
	 * Linear scan over `MarkerComponent`. Fine for setup; do not call it every frame.
	 *
	 * `kind` is an exact string match. Omit it (or pass `""`) to get every marker.
	 * No matches is `[]`, not `undefined`. A typo in the kind is a silent empty list —
	 * kinds are game vocabulary, not an engine enum.
	 */
	function FindMarkers(kind?: string): Entity[];

	/**
	 * Instantiate a prefab, returning the root's id — or `undefined` if the path is not an
	 * indexed prefab.
	 *
	 * **The entity appears on the NEXT frame.** Scripts run inside the scene update, where
	 * structural changes are illegal, so the spawn is queued and performed by the flush at the
	 * start of the following frame. That is why an id comes back instead of an `Entity`: it is
	 * something to hold across the boundary.
	 *
	 * ```ts
	 * const id = Scene.Spawn("prefabs/SparkBurst.gprefab", this.entity.GetTranslation());
	 * // ... a later frame:
	 * const e = Scene.FindEntityByUUID(id);
	 * if (e) e.SetTranslation(somewhere);
	 * ```
	 *
	 * Omitting both `position` and `rotation` places the root where the `.gprefab` says.
	 * Rotation is Euler angles in radians, matching `Entity.SetRotation`.
	 *
	 * Returns `undefined` if the per-frame spawn cap (64) is hit, so a loop that runs away is
	 * refused rather than allowed to exhaust memory. Check the result if you spawn in bulk.
	 */
	function Spawn(path: string, position?: Vec3, rotation?: Vec3): number | undefined;
}

/** What a ray hit. `entity` is absent when the body's entity vanished between the cast and the
 * lookup — something was hit, and the geometry of the hit is still true. */
declare interface RaycastHit {
	/** World-space point where the ray met the surface. */
	point: Vec3;
	/** Outward surface normal at `point`, unit length. */
	normal: Vec3;
	/** Distance from the ray origin, in world units — not a [0,1] fraction. */
	distance: number;
	entity?: Entity;
}

/** Queries against the physics world. Only valid in play mode; outside it every cast misses. */
declare namespace Physics {
	/**
	 * Closest hit along a ray, or `undefined` if nothing is in the way — so the idiomatic
	 * shape is `const hit = Physics.Raycast(...); if (hit) { ... }`.
	 *
	 * `direction` need not be normalised; `maxDistance` sets the reach either way. Static and
	 * dynamic bodies are both hit: a line-of-sight test that could not see walls would be
	 * useless.
	 *
	 * Pass `ignore` when casting from an entity's own position — its collider is otherwise the
	 * first thing in the way:
	 *
	 * ```ts
	 * const eye = this.entity.GetTranslation();
	 * const hit = Physics.Raycast(eye, forward, 50, this.entity);
	 * if (hit && hit.entity && hit.entity.GetName() === "Player") { ... }
	 * ```
	 *
	 * A zero-length `direction` or a non-positive `maxDistance` misses rather than throwing.
	 */
	function Raycast(origin: Vec3, direction: Vec3, maxDistance: number,
		ignore?: Entity): RaycastHit | undefined;
}

/**
 * Sound with no entity behind it: fire-and-forget one-shots, and the mixer.
 *
 * Per-entity sound is on Entity (PlaySound/StopSound/…). There is deliberately no way to
 * swap a clip or hold a voice handle from script — a handle to a live engine resource is a
 * lifetime problem the engine would then have to police.
 */
declare namespace Audio {
	/**
	 * Plays a clip once and forgets it; the engine frees it when it finishes. For footsteps
	 * and impacts, which should not need an entity each.
	 *
	 * `path` is relative to `assets/`, e.g. "audio/impact.wav". It needs no registry entry.
	 * With a position the sound is spatialised against the current listener; without one it
	 * plays flat, for UI and 2D. Both go to the SFX group.
	 */
	function PlayOneShot(path: string): void;
	function PlayOneShot(path: string, position: Vec3): void;
	/** volume is a gain, 1 = unchanged, clamped to 0..4. */
	function PlayOneShot(path: string, position: Vec3 | undefined, volume: number): void;

	function SetMasterVolume(volume: number): void;

	/** Component-owned voices alive right now. For leak checks, not gameplay. */
	function GetVoiceCount(): number;
	/** Fire-and-forget voices still waiting to be reaped. */
	function GetOneShotCount(): number;

	/** group is "Master", "Music" or "SFX". An unknown name warns and falls back to SFX. */
	function SetGroupVolume(group: string, volume: number): void;
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
	/** Truncated to a whole number. A fractional score is accepted, not refused. */
	function SetScore(score: number): void;
	function GetHealth(): number;
	function GetScore(): number;
}

// ---------------------------------------------------------------------------
// RmlUi's own Lua API, available because the UI plugin shares this VM.
//
// A deliberately small slice of a large API — only what has been verified
// against the engine. Extend it from RmlUi's Lua manual as needed; these are
// hand-written declarations over a C++ binding, so nothing checks them for you.
//
// Prefer the `UI` data model above for values that change every frame: it is
// declarative and batches through RmlUi's dirty-variable machinery. Reach for
// `rmlui` when you need to restructure a document, not to push numbers into it.
// ---------------------------------------------------------------------------

declare interface RmlElement {
	readonly id: string;
	/** The element's inner markup. Assigning replaces its children. */
	inner_rml: string;
	class_name: string;

	GetElementById(id: string): RmlElement | undefined;
	QuerySelector(selector: string): RmlElement | undefined;
	SetClass(className: string, activate: boolean): void;
	IsClassSet(className: string): boolean;
	SetAttribute(name: string, value: string): void;
	GetAttribute(name: string): string | undefined;
	AppendChild(element: RmlElement): void;
	Focus(): void;
	Blur(): void;
}

declare interface RmlDocument extends RmlElement {
	readonly title: string;
	Show(): void;
	Hide(): void;
	Close(): void;
}

declare interface RmlContext {
	/**
	 * Keyed by the document's `id` — i.e. the `id` attribute on its <body>, NOT
	 * its <title>. Avoid the numeric index: in Debug builds the RmlUi Debugger
	 * injects six documents of its own ahead of yours.
	 */
	readonly documents: { [id: string]: RmlDocument | undefined };
}

declare namespace rmlui {
	/** Keyed by context name; the engine creates a single context called "main". */
	const contexts: { [name: string]: RmlContext | undefined };
}

/** Cursor modes for `Input.SetCursorMode`. */
declare namespace Cursor {
	/** Visible and free to leave the window. The default. */
	const Normal: number;
	/** Invisible over the window, but still a normal cursor underneath. */
	const Hidden: number;
	/** Captured for mouse-look: hidden, held to the window, unbounded coordinates. */
	const Locked: number;
}
