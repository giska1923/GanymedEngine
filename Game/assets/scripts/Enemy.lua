-- P3 - something to shoot.
--
-- Static, not dynamic. A dynamic enemy gets shoved across the map by every round that lands, and
-- P3 is testing spawn/despawn and collision dispatch rather than knockback. P4 makes them move,
-- and that is when they become character controllers.
--
-- Health is decremented here rather than from the projectile because the projectile cannot reach
-- into this instance: collision events are delivered to both sides, so the victim counting its own
-- hits is what the script API can actually express.

PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }

local Enemy = {
    entity = nil,
    Properties = { health = 3 },
    health = 3,
    dead = false,
}

function Enemy:OnCreate()
    Log.Info(string.format("Enemy up at (%.1f, %.1f, %.1f) with %d health",
        self.entity:GetTranslation().x, self.entity:GetTranslation().y,
        self.entity:GetTranslation().z, self.health))
end

function Enemy:OnCollisionEnter(other)
    if self.dead or not other or other:GetName() ~= "Projectile" then
        return
    end

    self.health = self.health - 1
    if self.health > 0 then
        return
    end

    self.dead = true
    PG.kills = PG.kills + 1
    Log.Info("Enemy down: " .. self.entity:GetName())
    -- Destroys the whole subtree, not just this entity. Scene::DestroyEntity unparents children
    -- instead of destroying them, which would leave any child of an enemy in the scene forever.
    self.entity:Destroy()
end

local ____exports = {}
____exports.default = Enemy
return ____exports
