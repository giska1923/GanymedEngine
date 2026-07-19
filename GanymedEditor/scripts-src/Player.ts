// The TypeScript port of the hand-written assets/scripts/Player.lua, and what now generates it.
//
// USE OBJECT LITERALS, NOT `class`. ScriptEngine instantiates via
// setmetatable({}, { __index = ... }), so a literal (methods on the table itself) is the
// zero-surprise path. TSTL classes resolve through the loader's `prototype` fallback, but their
// constructors never run — a foot-gun better avoided by convention than debugged later.

const Player: Script & { speed: number; elapsed: number } = {
	entity: undefined!, // injected by ScriptEngine before OnCreate
	speed: 3.0,
	elapsed: 0.0,

	OnCreate() {
		Log.Info(`Player created: ${this.entity.GetName()}`);
	},

	OnUpdate(ts: number) {
		this.elapsed += ts;

		const pos = this.entity.GetTranslation();

		if (Input.IsKeyPressed(Key.W)) pos.z -= this.speed * ts;
		if (Input.IsKeyPressed(Key.S)) pos.z += this.speed * ts;
		if (Input.IsKeyPressed(Key.A)) pos.x -= this.speed * ts;
		if (Input.IsKeyPressed(Key.D)) pos.x += this.speed * ts;

		// Unconditional bob, so the script visibly does something without input.
		pos.y = math.sin(this.elapsed * 2.0);

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
