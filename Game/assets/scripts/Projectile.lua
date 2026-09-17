-- P3 - a fired round.
--
-- Despawns on the first thing it touches, or after a lifetime if it touches nothing. The lifetime
-- is not a nicety: without it every shot fired into open sky is an entity and a Jolt body that
-- lives until the scene does, and P3's gate is precisely that the entity count comes back down.
--
-- Damage is NOT applied from here. Collision events dispatch to both participants
-- (PhysicsSystem::DispatchCollisionEvents calls notify(a,b) then notify(b,a)), so the enemy sees
-- this hit too and decrements its own health. Doing it from the victim's side is what the current
-- script API can express - there is no way to call a method on another entity's script instance.

-- Shared across every script instance: ScriptEngine holds one sol::state, so a global table is
-- the only cross-entity channel there is. The gate needs spawned/despawned counts and no entity
-- can see another's locals.
PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }

local Projectile = {
    entity = nil,
    Properties = { lifetime = 3.0 },
    lifetime = 3.0,
    age = 0.0,
    dead = false,
}

function Projectile:Despawn(reason)
    if self.dead then return end
    self.dead = true
    PG.despawned = PG.despawned + 1
    self.entity:Destroy()
end

function Projectile:OnUpdate(ts)
    if ts > 0.25 then return end
    self.age = self.age + ts
    if self.age > self.lifetime then
        self:Despawn("timeout")
    end
end

function Projectile:OnCollisionEnter(other)
    if other and other:GetName() == "Projectile" then
        -- Two rounds meeting in flight should not both vanish on each other; it reads as the gun
        -- jamming. Cheap to ignore, and it keeps the hit count meaning "hit something solid".
        return
    end

    -- P5. A sensor causes no collision RESPONSE, so a round flies through a pickup - but the
    -- contact event still fires, and this script would count it as a hit and despawn the round in
    -- mid-air over a heal spot. Scripts cannot ask whether a contact was with a sensor; the table
    -- is filled in by Pickup.lua, which knows, and is the honest workaround until they can.
    if other and PG.passThrough and PG.passThrough[other:GetName()] then
        return
    end
    PG.hits = PG.hits + 1
    self:Despawn("hit")
end

local ____exports = {}
____exports.default = Projectile
return ____exports
