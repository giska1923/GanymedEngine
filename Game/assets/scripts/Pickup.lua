-- P5 - a thing you walk into.
--
-- Every pickup in the map is this one script with a different `kind`. Three kinds, because three
-- is what it takes to cover the shapes a trigger comes in:
--
--   weapon    consumed on touch, and destroys itself
--   heal      permanent, and acts continuously while you stand in it
--   upgrade   permanent, and refuses unless you can pay
--
-- ---- Why these are sensors, and why that needed engine work ----
--
-- A pickup is a static body you walk *through*, which is Jolt's `mIsSensor` and reached Lua as
-- `RigidBodyComponent::IsSensor` for P5. Before that a trigger volume had to be something solid
-- you bumped into.
--
-- Sensor-ness is not decoration here, it is the only thing that makes these work at all. The
-- player is a CharacterVirtual whose presence is a **Kinematic** inner body, and Jolt refuses to
-- pair two non-dynamic bodies - so a plain static box is invisible to the player no matter how
-- squarely they walk into it. The one exemption in Body::sFindCollidingPairsCanCollide is a
-- sensor. See docs/engine/physics.md.
--
-- ---- Who applies the effect ----
--
-- The player does. Collision events dispatch to both participants, so the player's
-- OnCollisionEnter sees this entity and reads its tag; this script only handles what happens to
-- the *pickup*. The alternative - reaching into the player's script instance from here - is not
-- something the script API can express, and the same split is already how Enemy.lua counts its
-- own hits.

PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }

local Pickup = {
    entity = nil,
    Properties = {
        -- "weapon" | "heal" | "upgrade". Matched by the player against the tag it reads off this
        -- entity, so the tag and this have to agree; the tag is what the player can see.
        kind = "heal",
        label = "Pickup",
    },
    kind = "heal",
    label = "Pickup",
    taken = false,
    t = 0.0,
}

function Pickup:OnCreate()
    -- Tell the projectiles to fly through us. A sensor causes no collision response, but the
    -- contact event still reaches both scripts, and a round that despawned on a heal spot would
    -- be indistinguishable from one that hit a wall. Scripts cannot ask "was that a sensor?",
    -- so the side that knows publishes it.
    PG.passThrough = PG.passThrough or {}
    PG.passThrough[self.entity:GetName()] = true

    local p = self.entity:GetTranslation()
    Log.Info(string.format("Pickup %s (%s) at (%.1f, %.1f, %.1f)",
        self.label, self.kind, p.x, p.y, p.z))
end

function Pickup:OnUpdate(ts)
    if ts > 0.25 then return end
    self.t = self.t + ts
end

function Pickup:OnCollisionEnter(other)
    if self.taken or not other or other:GetName() ~= "Player" then
        return
    end

    PG.triggers = (PG.triggers or 0) + 1
    Log.Info(string.format("TRIGGER enter %s (%s) t=%.1fs", self.label, self.kind, self.t))

    -- Only a weapon crate is consumed. A heal spot and an upgrade station are map features: they
    -- have to still be there on the way back, and an upgrade station that vanished the first time
    -- you could not afford it would be the worst version of this.
    if self.kind == "weapon" then
        self.taken = true
        self.entity:Destroy()
    end
end

function Pickup:OnCollisionExit(other)
    if not other or other:GetName() ~= "Player" then
        return
    end
    Log.Info(string.format("TRIGGER exit  %s (%s) t=%.1fs", self.label, self.kind, self.t))
end

local ____exports = {}
____exports.default = Pickup
return ____exports
