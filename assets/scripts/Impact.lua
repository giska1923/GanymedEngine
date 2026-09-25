-- P6 - the spark burst a round leaves where it lands.
--
-- Spawned by Projectile.lua at the point of contact, one per hit. It plays its emitter on start,
-- lives long enough for the last particle to die, and destroys itself.
--
-- ---- Why an entity per impact, rather than one pooled emitter ----
--
-- A shared world-space emitter moved to each hit point is what a shipping game would do, and it is
-- cheaper. It is also wrong for this milestone in a specific way: with several hits in one frame
-- only the last position would win, and the thing P6 is meant to put under load is exactly
-- **many bursts in the same frame**. One entity per hit measures that honestly, and the spawn
-- path it leans on is the one P3 already proved holds at 64 requests a frame.
--
-- The lifetime is not a guess: it is LifetimeMax in the prefab plus a margin. Too short and the
-- entity takes its own particles with it when it goes; too long and the scene fills with dead
-- emitters, which is the leak P3's gate exists to catch.

local Impact = {
    entity = nil,
    Properties = { lifetime = 0.9 },
    lifetime = 0.9,
    age = 0.0,
    dead = false,
}

function Impact:OnCreate()
    -- The emitter is authored RateOverTime 0 and Looping, so it plays forever and emits nothing
    -- on its own; the whole effect is this one call. EmitBurst accumulates rather than replacing,
    -- and is consumed on the tick it is issued, which is why it needs Playing to already be true
    -- (PlayOnStart) rather than calling PlayParticles here.
    self.entity:EmitBurst(16)
    PG = PG or {}
    PG.impacts = (PG.impacts or 0) + 1
end

function Impact:OnUpdate(ts)
    if ts > 0.25 or self.dead then
        return
    end

    self.age = self.age + ts
    if self.age > self.lifetime then
        self.dead = true
        PG = PG or {}
        PG.impactsDespawned = (PG.impactsDespawned or 0) + 1
        self.entity:Destroy()
    end
end

local ____exports = {}
____exports.default = Impact
return ____exports
