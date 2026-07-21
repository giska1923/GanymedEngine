// The TypeScript port of the hand-written assets/scripts/Player.lua, and what now generates it.
//
// USE OBJECT LITERALS, NOT `class`. ScriptEngine instantiates via
// setmetatable({}, { __index = ... }), so a literal (methods on the table itself) is the
// zero-surprise path. TSTL classes resolve through the loader's `prototype` fallback, but their
// constructors never run — a foot-gun better avoided by convention than debugged later.

const Player: Script & {
	Properties: { speed: number; drainRate: number; bobHeight: number };
	speed: number;
	drainRate: number;
	bobHeight: number;
	elapsed: number;
	health: number;
	score: number;
} = {
	entity: undefined!, // injected by ScriptEngine before OnCreate

	// Everything here shows up in the inspector, per entity, and is applied to `self`
	// before OnCreate. The declared value is the default; the editor stores only what
	// you actually change, so editing a number here still reaches untouched entities.
	Properties: {
		speed: 3.0,
		drainRate: 12.0,
		bobHeight: 1.0,
	},

	// Applied from Properties at instantiation - these initialisers only exist to
	// satisfy the type. Do NOT reassign a property in OnCreate: that would overwrite
	// whatever the inspector set.
	speed: 0,
	drainRate: 0,
	bobHeight: 0,

	elapsed: 0.0,
	health: 100.0,
	score: 0,

	OnCreate() {
		Log.Info(`Player created: ${this.entity.GetName()} (speed=${this.speed})`);
		UI.SetHealth(this.health);
		UI.SetScore(this.score);
	},

	OnUpdate(ts: number) {
		this.elapsed += ts;

		// Drive the HUD: health drains and refills, score ticks up. Both go through
		// the data model, so the bar's width and the text follow without this script
		// knowing anything about RML or RCSS.
		this.health -= ts * this.drainRate;
		if (this.health <= 0.0) {
			this.health = 100.0;
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
		pos.y = math.sin(this.elapsed * 2.0) * this.bobHeight;

		this.entity.SetTranslation(pos);
	},

	OnCollisionEnter(other: Entity) {
		Log.Warn(`Hit ${other.GetName()}`);
	},

	OnDestroy() {
		Log.Info("Player destroyed");
	},
};

export default Player;
