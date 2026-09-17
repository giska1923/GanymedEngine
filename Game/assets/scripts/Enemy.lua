-- P4 - an enemy that patrols, looks, and charges.
--
-- ---- Why this is a dynamic rigid body and not a character controller ----
--
-- The P3 version of this comment said "P4 makes them move, and that is when they become character
-- controllers." **That was wrong, and P4 is where it broke.** A CharacterVirtual is not a body: it
-- has no BodyID, it is not in the broadphase, and nothing in NarrowPhaseQuery can find it. Make an
-- enemy a character and three things stop working at once - projectiles pass through it, its
-- OnCollisionEnter never fires, and no raycast can ever hit it. Shooting them is the entire point
-- of P3, so the controller is not available here.
--
-- So: Dynamic with LockRotation, driven by SetLinearVelocity, which is what the player was before
-- P1's gate pushed it onto a controller. The known cost is the one P1 found - a velocity-driven
-- body cannot slide along a wall, it stops dead against it - and the mitigation is the same stuck
-- escape the autopilot carries. Every firing of it is logged, because each one is a place the
-- engine cannot express what the game wants.
--
-- ---- How seeing works ----
--
-- Cast from this enemy's eye at the player's eye, slightly past it, ignoring our own body. A clear
-- line of sight means **the first thing the ray hits is the player**; anything else is an occluder,
-- and it is named in the log.
--
-- P4 wrote this inside out, and said so. The player was a CharacterVirtual with no presence in the
-- broadphase, so there was nothing to hit at the far end and a **miss** had to stand for "I can see
-- you". That comment predicted its own expiry - "the day the player grows an inner body, every
-- enemy in this scene goes blind at once" - and P5 is that day: landing the inner body on master
-- broke this function, exactly as written, and the log said so in as many words:
--
--   LOS E1 t=3.4s lost player=(0.0, 0.0) self=(-3.8, -17.1) d=17.5m blocked-by=Player
--
-- The distance in that line is the tell. The old cast stopped at 0.98 of the way, which cleared a
-- 0.35 m capsule while the player was further than ~17.8 m away and struck it closer in, so the
-- enemies went blind as they closed. The test below is positive rather than negative now, which is
-- both correct and no longer distance-dependent.

PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }

local Enemy = {
    entity = nil,
    Properties = {
        health = 3,
        -- The far end of the patrol leg, in world XZ; y is ignored. The near end is wherever the
        -- enemy was authored, so a leg is two numbers in the scene rather than a route asset.
        -- Leaving it at zero is not "patrol to the origin" - a target within half a metre of home
        -- means stand still, which is what the sentry inside the Blockhouse wants.
        patrolTo = Vec3(0, 0, 0),
        speed = 2.0,          -- m/s while patrolling
        chargeSpeed = 4.2,    -- m/s once it has seen you. Slower than the player's 6.
        sightRange = 22.0,
        -- Half-angle of the view cone, radians. ~80 degrees, generous on purpose: a narrow cone
        -- gives the P4 gate a second way to fail ("was it the wall, or was it facing away?") and
        -- the gate is about occlusion.
        fov = 1.40,
        eyeHeight = 0.60,     -- above the body centre, so 1.5 m off the ground
        -- Close enough to touch. P4 held enemies at 2.5 m, which looked fine and meant they
        -- never made contact with anything. P5 needs the contact: it is what damages the player.
        -- 0.8 is just under the two half-widths (0.35 + 0.35), so they press in.
        standoff = 0.8,
        label = "E",          -- what it calls itself in the log
    },
    health = 3,
    patrolTo = Vec3(0, 0, 0),
    speed = 2.0,
    chargeSpeed = 4.2,
    sightRange = 22.0,
    fov = 1.40,
    eyeHeight = 0.60,
    standoff = 0.8,
    label = "E",

    dead = false,
    state = "idle",           -- idle | patrol | hunt | search
    body = nil,               -- the child that carries the mesh, the animator and the facing
    home = nil,
    leg = nil,                -- the end of the leg currently being walked
    facing = 0.0,
    moving = false,
    sees = false,
    lastSeen = nil,
    lostFor = 0.0,
    stuckFor = 0.0,
    sidestepFor = 0.0,
    sidestepSign = 1.0,
    escapes = 0,
    lungeCooldown = 0.0,
    touches = 0,
    clip = nil,
    clipLogs = 0,
    lastPos = nil,
    playerEntity = nil,
    t = 0.0,
}

function Enemy:OnCreate()
    local p = self.entity:GetTranslation()
    self.home = p
    self.lastPos = p

    -- The mesh, the animator and the facing all live on this child, and they have to: the body is
    -- LockRotation, so Jolt hands SyncTransforms the same orientation every frame and a yaw
    -- written on the parent is erased before anything sees it. Same shape as the player's "Yaw".
    self.body = self.entity:GetChildByName("Body")
    if not self.body then
        Log.Error(string.format(
            "Enemy %s: no child named 'Body' - no mesh, no facing, no animation", self.label))
    end

    -- A leg shorter than half a metre is a standing order, not a patrol.
    local dx, dz = self.patrolTo.x - p.x, self.patrolTo.z - p.z
    if (dx * dx + dz * dz) > 0.25 then
        self.state = "patrol"
        self.leg = self.patrolTo
    else
        self.state = "idle"
    end

    Log.Info(string.format("Enemy %s up at (%.1f, %.1f, %.1f) health=%d state=%s",
        self.label, p.x, p.y, p.z, self.health, self.state))
end

function Enemy:Player()
    -- Resolved lazily and re-resolved if it goes away. FindEntityByName is a linear scan over
    -- TagComponent, so this caches rather than paying for it every frame on every enemy.
    if self.playerEntity and self.playerEntity:IsValid() then
        return self.playerEntity
    end
    self.playerEntity = Scene.FindEntityByName("Player")
    return self.playerEntity
end

-- Returns sees, distance, blockerName.
function Enemy:Look(player)
    local p = self.entity:GetTranslation()
    local q = player:GetTranslation()

    local ex, ey, ez = p.x, p.y + self.eyeHeight, p.z
    local tx, ty, tz = q.x, q.y + 0.5, q.z

    local dx, dy, dz = tx - ex, ty - ey, tz - ez
    local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
    if dist > self.sightRange or dist < 0.01 then
        return false, dist, "range"
    end

    -- The cone, on the ground plane only: an enemy that loses you by looking slightly up is not a
    -- behaviour anyone asked for.
    local flat = math.sqrt(dx * dx + dz * dz)
    if flat > 0.01 then
        local fx, fz = -math.sin(self.facing), -math.cos(self.facing)
        local cosang = (fx * dx + fz * dz) / flat
        if cosang < math.cos(self.fov) then
            return false, dist, "behind"
        end
    end

    -- 1.05 rather than the exact distance: the ray has to reach *past* the player's centre to be
    -- sure of striking its capsule, and overshooting costs nothing, because whenever the line is
    -- clear the player is the first thing in the way.
    local hit = Physics.Raycast(Vec3(ex, ey, ez), Vec3(dx, dy, dz), dist * 1.05, self.entity)
    if not hit then
        -- Nothing at all, not even the player. Reachable when the player sits exactly on the far
        -- edge of the overshoot, so it counts as blocked rather than as a lucky clear line.
        return false, dist, "nothing"
    end
    if not hit.entity or hit.entity ~= player then
        return false, dist, (hit.entity and hit.entity:GetName() or "?")
    end

    return true, dist, nil
end

function Enemy:OnUpdate(ts)
    -- Frame 1 spans boot and is over a second long; a second of anything applied to a body on its
    -- first frame is the one input it can do nothing useful with.
    if ts > 0.25 or self.dead then
        return
    end

    self.t = self.t + ts

    local player = self:Player()
    local sees, dist, blocker = false, 1e9, nil
    if player then
        sees, dist, blocker = self:Look(player)
    end

    -- Every transition, logged. This *is* P4's gate: "an enemy behind a building does not acquire
    -- the player; stepping into the doorway does" is exactly a pair of these lines.
    if sees ~= self.sees then
        self.sees = sees
        local p = self.entity:GetTranslation()
        local q = player and player:GetTranslation() or p
        Log.Info(string.format(
            "LOS %s t=%.1fs %s player=(%.1f, %.1f) self=(%.1f, %.1f) d=%.1fm%s",
            self.label, self.t, sees and "ACQUIRED" or "lost",
            q.x, q.z, p.x, p.z, dist,
            sees and "" or (" blocked-by=" .. tostring(blocker))))
    end

    if sees then
        self.state = "hunt"
        self.lastSeen = player:GetTranslation()
        self.lostFor = 0.0
    elseif self.state == "hunt" then
        self.state = "search"
        self.lostFor = 0.0
    elseif self.state == "search" then
        self.lostFor = self.lostFor + ts
        if self.lostFor > 4.0 then
            self.state = self.leg and "patrol" or "idle"
        end
    end

    self:Move(ts, dist)
    self:Animate()
end

function Enemy:Move(ts, dist)
    local p = self.entity:GetTranslation()
    local v = self.entity:GetLinearVelocity()

    -- Gate mode. The LOS probe run wants sensing without motion: six enemies converging on the
    -- probe point would shove the player off the spot the measurement is taken at, and the gate
    -- is about what an enemy can see, not what it does about it. Charging gets its own run.
    if PG.freeze then
        self.entity:SetLinearVelocity(Vec3(0, v.y, 0))
        self.moving = false
        return
    end

    self.lungeCooldown = math.max(0.0, self.lungeCooldown - ts)

    local tx, tz, speed
    if self.state == "hunt" then
        tx, tz, speed = self.lastSeen.x, self.lastSeen.z, self.chargeSpeed
        if self.lungeCooldown > 0.0 then
            -- Just landed one: give ground rather than lean on the player. Backing to 2.2 m puts
            -- the box's corner (0.35 * sqrt(2) = 0.49) clear of the capsule with room to spare.
            local bx, bz = p.x - tx, p.z - tz
            local blen = math.sqrt(bx * bx + bz * bz)
            if blen > 0.01 and blen < 2.2 then
                tx, tz = p.x + (bx / blen) * 2.2, p.z + (bz / blen) * 2.2
                speed = self.speed
            else
                tx, tz = nil, nil
            end
        elseif dist < self.standoff then
            tx, tz = nil, nil
        end
    elseif self.state == "search" and self.lastSeen then
        tx, tz, speed = self.lastSeen.x, self.lastSeen.z, self.chargeSpeed * 0.6
        -- Stop on arrival. P4 left this branch with no stopping rule at all, which was invisible
        -- while a character had no presence: an enemy that had lost sight of the player drove at
        -- its last known position forever, straight through it. P5 gave the player a body, and
        -- the effect was immediate and absurd - enemies bulldozed the player 20 m across the map
        -- during a gate hold. The last known position is a place to go to, not a place to push.
        local sx, sz = tx - p.x, tz - p.z
        if (sx * sx + sz * sz) < (self.standoff * self.standoff) then
            tx, tz = nil, nil
        end
    elseif self.state == "patrol" then
        tx, tz, speed = self.leg.x, self.leg.z, self.speed
        local lx, lz = tx - p.x, tz - p.z
        if (lx * lx + lz * lz) < 1.0 then
            -- Turn round: the far end becomes home and home becomes the far end.
            self.leg = (self.leg == self.patrolTo) and self.home or self.patrolTo
            tx, tz = self.leg.x, self.leg.z
        end
    end

    local moved = math.sqrt((p.x - self.lastPos.x) ^ 2 + (p.z - self.lastPos.z) ^ 2)
    self.lastPos = p

    if not tx then
        self.entity:SetLinearVelocity(Vec3(0, v.y, 0))
        self.moving = false
        self.stuckFor = 0.0
        return
    end

    local dx, dz = tx - p.x, tz - p.z
    local len = math.sqrt(dx * dx + dz * dz)
    if len < 0.01 then
        self.entity:SetLinearVelocity(Vec3(0, v.y, 0))
        self.moving = false
        return
    end
    dx, dz = dx / len, dz / len

    -- Wanting to move and not moving is the P1 failure mode on a velocity-driven body: pressed
    -- flat against a wall, with no tangential component left to carry it along. There is no
    -- pathfinding here (Decision 5), so the answer is to give up on the straight line for a
    -- moment and strafe.
    if moved < 0.0008 then
        self.stuckFor = self.stuckFor + ts
        if self.stuckFor > 1.2 and self.sidestepFor <= 0.0 then
            self.stuckFor = 0.0
            self.escapes = self.escapes + 1
            self.sidestepFor = 0.8
            self.sidestepSign = -self.sidestepSign
            if self.state == "patrol" then
                self.leg = (self.leg == self.patrolTo) and self.home or self.patrolTo
            end
            if self.escapes <= 3 then
                Log.Warn(string.format(
                    "Enemy %s: stuck at (%.1f, %.1f) in state '%s' - sidestepping (escape #%d)",
                    self.label, p.x, p.z, self.state, self.escapes))
            end
        end
    else
        self.stuckFor = 0.0
    end

    if self.sidestepFor > 0.0 then
        self.sidestepFor = self.sidestepFor - ts
        dx, dz = -dz * self.sidestepSign, dx * self.sidestepSign
    end

    self.entity:SetLinearVelocity(Vec3(dx * speed, v.y, dz * speed))
    self.moving = true

    -- Face where it is going. Forward is -Z at yaw 0, the engine's camera convention, and what
    -- Player.lua derives its own forward from.
    self.facing = math.atan(-dx, -dz)
    if self.body then
        self.body:SetRotation(Vec3(0, self.facing, 0))
    end
end

-- Three states, three clips. Fox.glb is the only asset in this tree with more than one clip, and
-- driving six instances between three of them in the same frame is the part of the animator the
-- animation milestone never reached: it verified two entities on different clips.
--
-- PlayAnimation every frame is the documented idiom - it restarts only on an actual clip change,
-- so calling it from a branch like this advances time instead of pinning it at zero.
function Enemy:Animate()
    if not self.body then
        return
    end

    local clip, speed
    if self.state == "hunt" then
        clip, speed = "Run", 1.0
    elseif self.moving then
        clip, speed = "Walk", 1.0
    else
        clip, speed = "Survey", 0.6
    end

    self.body:PlayAnimation(clip)
    self.body:SetAnimationSpeed(speed)

    -- Read the clip back after setting it, and only on a change. An unresolved clip name does not
    -- fail loudly - AnimationSystem warns once and holds the bind pose - so "no warnings in the
    -- log" is weak evidence that a clip switch landed. This is the strong version, and it is
    -- capped because six enemies changing state at human timescales still adds up over a long run.
    if clip ~= self.clip then
        self.clip = clip
        self.clipLogs = (self.clipLogs or 0) + 1
        if self.clipLogs <= 6 then
            Log.Info(string.format("CLIP %s -> '%s' (animator reports '%s', playing=%s)",
                self.label, clip, self.body:GetCurrentAnimation(),
                tostring(self.body:IsAnimationPlaying())))
        end
    end
end

function Enemy:OnCollisionEnter(other)
    if self.dead or not other then
        return
    end

    -- ---- Landing a touch, and then getting out of the way ----
    --
    -- P5 gave the player an inner body, so an enemy pressed against it now reports a contact -
    -- which is what damages the player, and is the point. What it also did, immediately, was let
    -- a charging enemy **bulldoze the player across the map**: a CharacterVirtual resolves any
    -- overlap by moving itself, with no mass and no resistance, and an enemy driving at 4.2 m/s
    -- never stops driving. The first P5 gate run ended with the player shoved from (2, -2) to
    -- (36, 45) and then pushed straight through the ground plane - inside its +-50 extent, so it
    -- went through rather than off - after which it fell forever while the route above cheerfully
    -- reported reaching every remaining waypoint, because those are measured in XZ only.
    --
    -- The engine half of that is recorded in docs/ToDo/cross-cutting.md: Jolt can refuse the push
    -- per contact (CharacterContactSettings::mCanPushCharacter) but only through a
    -- CharacterContactListener, and the engine installs none.
    --
    -- The game half is this: land a touch, then back off. It is what melee AI does anyway, and it
    -- bounds the shove to the moment of contact instead of letting it run for a minute.
    if other:GetName() == "Player" then
        self.touches = self.touches + 1
        self.lungeCooldown = 1.5
        return
    end

    if other:GetName() ~= "Projectile" then
        return
    end

    -- Shot from somewhere it cannot see: go and look. Standard, and it is also the only path in
    -- this script that reaches "search" without ever having had line of sight.
    if self.state ~= "hunt" then
        local player = self:Player()
        if player then
            self.lastSeen = player:GetTranslation()
            self.state = "search"
            self.lostFor = 0.0
        end
    end

    -- The damage is the player's, not the projectile's - the upgrade station raises it - and PG is
    -- the only channel between two script instances there is.
    self.health = self.health - (PG.damage or 1)
    if self.health > 0 then
        return
    end

    self.dead = true
    PG.kills = PG.kills + 1
    Log.Info(string.format("Enemy %s down (escapes=%d)", self.label, self.escapes))
    -- Destroys the whole subtree, not just this entity. Scene::DestroyEntity unparents children
    -- instead of destroying them, which would leave the mesh child in the scene forever.
    self.entity:Destroy()
end

local ____exports = {}
____exports.default = Enemy
return ____exports
