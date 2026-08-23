// The TypeScript port of the hand-written assets/scripts/Player.lua, and what now generates it.
//
// USE OBJECT LITERALS, NOT `class`. ScriptEngine instantiates via
// setmetatable({}, { __index = ... }), so a literal (methods on the table itself) is the
// zero-surprise path. TSTL classes resolve through the loader's `prototype` fallback, but their
// constructors never run — a foot-gun better avoided by convention than debugged later.

const Player: Script & {
	Properties: { speed: number; drainRate: number; bobHeight: number; masterVolume: number };
	speed: number;
	drainRate: number;
	bobHeight: number;
	masterVolume: number;
	elapsed: number;
	health: number;
	score: number;
	musicMuted: boolean;
	musicKeyWasDown: boolean;
	humFrames: number;
} = {
	entity: undefined!, // injected by ScriptEngine before OnCreate

	// Everything here shows up in the inspector, per entity, and is applied to `self`
	// before OnCreate. The declared value is the default; the editor stores only what
	// you actually change, so editing a number here still reaches untouched entities.
	Properties: {
		speed: 3.0,
		drainRate: 12.0,
		bobHeight: 1.0,
		masterVolume: 0.8,
	},

	// Applied from Properties at instantiation - these initialisers only exist to
	// satisfy the type. Do NOT reassign a property in OnCreate: that would overwrite
	// whatever the inspector set.
	speed: 0,
	drainRate: 0,
	bobHeight: 0,
	masterVolume: 0,

	elapsed: 0.0,
	health: 100.0,
	score: 0,

	musicMuted: false,
	musicKeyWasDown: false,
	humFrames: 0,

	OnCreate() {
		Log.Info(`Player created: ${this.entity.GetName()} (speed=${this.speed})`);
		UI.SetHealth(this.health);
		UI.SetScore(this.score);

		Audio.SetMasterVolume(this.masterVolume);

		// The hum is authored on this entity with PlayOnStart off, so nothing is audible
		// until the player holds Space. SetSoundLooping/Volume write the component; the
		// audio system picks them up on its next pass.
		Log.Info(`Player has an audio source: ${this.entity.HasAudioSource()}`);
		this.entity.SetSoundLooping(true);
		this.entity.SetSoundVolume(0.6);
	},

	OnUpdate(ts: number) {
		this.elapsed += ts;

		// Drive the HUD: health drains and refills, score ticks up. Both go through
		// the data model, so the bar's width and the text follow without this script
		// knowing anything about RML or RCSS.
		this.health -= ts * this.drainRate;
		if (this.health <= 0.0) {
			this.health = 100.0;
			// A 2D one-shot: no position, so it plays flat regardless of where the
			// listener is. UI feedback, not a thing in the world.
			Audio.PlayOneShot("audio/chime.wav");
		}
		UI.SetHealth(this.health);

		this.score = math.floor(this.elapsed * 10.0);
		UI.SetScore(this.score);

		const pos = this.entity.GetTranslation();

		if (Input.IsKeyPressed(Key.W)) pos.z -= this.speed * ts;
		if (Input.IsKeyPressed(Key.S)) pos.z += this.speed * ts;
		if (Input.IsKeyPressed(Key.A)) pos.x -= this.speed * ts;
		if (Input.IsKeyPressed(Key.D)) pos.x += this.speed * ts;

		// Unconditional bob, so the script visibly does something without input.
		const bob = math.sin(this.elapsed * 2.0);
		pos.y = bob * this.bobHeight;

		this.entity.SetTranslation(pos);

		// Hold Space to hum. PlaySound is called EVERY FRAME while held, on purpose: an
		// already-playing source is left alone, so this is the idiom rather than a bug.
		// Restarting each tick would hold the sound at its first sample forever.
		if (Input.IsKeyPressed(Key.Space)) {
			this.entity.PlaySound();
			// Wobble the pitch with the bob, to show a per-frame component write landing.
			this.entity.SetSoundPitch(1.0 + bob * 0.15);
			this.humFrames += 1;
			if (this.humFrames === 1 || this.humFrames % 120 === 0) {
				Log.Info(`Hum: ${this.humFrames} PlaySound calls, IsSoundPlaying=${this.entity.IsSoundPlaying()}`);
			}
		} else if (this.humFrames > 0) {
			this.entity.StopSound();
			Log.Info(`Hum stopped after ${this.humFrames} PlaySound calls, IsSoundPlaying=${this.entity.IsSoundPlaying()}`);
			this.humFrames = 0;
		}

		// M mutes the music bus. Edge-triggered: SetGroupVolume every frame would work but
		// says nothing about intent.
		const musicKeyDown = Input.IsKeyPressed(Key.M);
		if (musicKeyDown && !this.musicKeyWasDown) {
			this.musicMuted = !this.musicMuted;
			Audio.SetGroupVolume("Music", this.musicMuted ? 0.0 : 1.0);
			Log.Info(`Music ${this.musicMuted ? "muted" : "unmuted"}`);
		}
		this.musicKeyWasDown = musicKeyDown;
	},

	OnCollisionEnter(other: Entity) {
		Log.Warn(`Hit ${other.GetName()}`);
	},

	OnDestroy() {
		Log.Info("Player destroyed");
	},
};

export default Player;
