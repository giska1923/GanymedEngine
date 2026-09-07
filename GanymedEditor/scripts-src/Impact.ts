// Fires a positional one-shot when this entity is hit. Goes on the falling boxes in the
// runtime demo, which is the milestone's "physics interaction that makes a sound" — and,
// since the particle milestone, a spark burst on a child emitter named "Sparks".
//
// See Player.ts for the object-literal-not-class rule.

const Impact: Script & {
	Properties: { clip: string; minInterval: number; sparkCount: number; };
	clip: string;
	minInterval: number;
	sparkCount: number;
	cooldown: number;
} = {
	entity: undefined!,

	Properties: {
		clip: "audio/impact.wav",
		// A box settling on the floor generates a burst of contacts over a few frames, not
		// one. Without a gate the same thud fires five or six times and sounds like a
		// machine gun; this is the cheapest fix that keeps a real second impact audible.
		minInterval: 0.12,
		sparkCount: 24,
	},

	clip: "",
	minInterval: 0,
	sparkCount: 0,

	cooldown: 0.0,

	OnUpdate(ts: number) {
		if (this.cooldown > 0.0) {
			this.cooldown -= ts;
		}
	},

	OnCollisionEnter(other: Entity) {
		if (this.cooldown > 0.0) {
			return;
		}
		this.cooldown = this.minInterval;

		// GetTranslation, not a world transform: these boxes have no parent, and for a
		// dynamic body the physics step writes this component every frame, so it is the
		// simulated position.
		Audio.PlayOneShot(this.clip, this.entity.GetTranslation());
		Log.Trace(`Impact: ${this.entity.GetName()} hit ${other.GetName()}`);

		// Prefab child, not Scene.FindEntityByName: both boxes parent a child tagged Sparks.
		const sparks = this.entity.GetChildByName("Sparks");
		if (sparks) {
			sparks.PlayParticles();
			sparks.EmitBurst(this.sparkCount);
		}
	},
};

export default Impact;
