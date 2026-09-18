-- Proving Ground - player controller (P1)
--
-- Movement is velocity-driven on a rotation-locked dynamic capsule. The capsule itself never
-- turns: its TransformComponent is overwritten from Jolt every frame by SyncTransforms, so a yaw
-- written here would be erased before it was ever seen. Facing therefore lives on a child entity
-- ("Yaw"), which has no body and whose local transform nothing else touches.
--
-- Looking is on the mouse. P0.5 landed cursor capture, and the prediction this comment used to
-- carry - "reads a mouse delta instead and nothing else here changes" - was nearly right: the
-- movement code below is untouched. What it missed is that capture needs a way *out*.
--
-- Click to capture, Escape to release. Not captured on OnCreate, because scripts also run in the
-- editor's play mode, and the editor has no Escape handling at all: a script that grabbed the
-- cursor unconditionally would trap it over the editor with no way to reach the Stop button. In
-- the runtime Escape quits, so the same key gets you out of both.
--
-- Pitch lives on the camera and yaw on the "Yaw" entity, because they must not multiply: pitching
-- a parent that a child yaws under rolls the horizon.
--
-- The capsule is a CharacterControllerComponent, not a rotation-locked rigid body. The first
-- version was the latter, and P1's gate failed on it: a velocity-driven body cannot slide along a
-- wall, so it jammed on the same corner every lap. Nothing in this script changed for the swap -
-- SetLinearVelocity routes to either - which is the point of that API accepting both.

local Player = {
    entity = nil,
    Properties = {
        speed = 6.0,        -- metres/second on the ground plane
        turnSpeed = 2.4,    -- radians/second, for the keyboard fallback
        -- Radians of turn per pixel of mouse movement. 0.0022 is about 0.13 degrees a pixel,
        -- which is the middle of the range shooters ship with.
        sensitivity = 0.0022,
        -- P3. muzzleSpeed is deliberately moderate: Decision 2 accepted that projectiles tunnel
        -- at some speed, and nothing here does continuous collision detection, so a round fast
        -- enough to cross a 0.3 m wall in one step would pass through it.
        muzzleSpeed = 28.0,
        fireInterval = 0.12,
        -- Gate mode: fire every frame instead of on a trigger, to put Scene.Spawn and the
        -- 64-per-frame cap under sustained load. Off for play.
        autofire = false,
        -- Drives the circuit below with no keyboard, for gate runs. **Off by default now**: with
        -- mouse look there is a human driving. The gate runs in the roadmap all set it true.
        autopilot = false,
        -- P4's gate. Drives LOS_ROUTE instead of ROUTE - walk to a marked spot, stop, stand
        -- still long enough for the Sentry's answer to be unambiguous, move to the next. Also
        -- sets PG.freeze, which makes every enemy sense without moving: six of them converging
        -- on the probe point would shove the player off the spot the measurement is taken at.
        losgate = false,
        -- P5's gate. Drives P5_ROUTE: stand and fight until it hurts, heal, arm, upgrade.
        p5gate = false,

        -- ---- P5: health and the things that change it ----
        maxHealth = 100.0,
        -- Per touch from an enemy. Enemies close to 0.8 m now (Enemy.lua's standoff), so contact
        -- is continuous once one reaches you - the cooldown, not the contact, sets the rate.
        contactDamage = 6.0,
        damageCooldown = 1.0,
        -- Health per second while standing in a heal spot. Faster than two enemies can take it
        -- off, or the spot is scenery.
        healRate = 14.0,
        -- What the upgrade station charges for +1 projectile damage.
        upgradeCost = 2.0,

        -- P6. Seconds between footsteps at full speed. 0.42 is a brisk walk; the sound is
        -- retriggered rather than looped, so this is the only thing setting the cadence.
        stepInterval = 0.42,
    },
    speed = 6.0,
    turnSpeed = 2.4,
    sensitivity = 0.0022,
    muzzleSpeed = 28.0,
    fireInterval = 0.12,
    autofire = false,
    autopilot = false,
    losgate = false,
    p5gate = false,
    maxHealth = 100.0,
    contactDamage = 6.0,
    damageCooldown = 1.0,
    healRate = 14.0,
    upgradeCost = 2.0,
    stepInterval = 0.42,

    -- Probe-route state, shared by both gate modes: they differ only in which table they walk.
    probeWp = 1,
    probeHold = 0.0,
    probeDrive = false,
    probeSlow = 1.0,
    probeDone = false,

    -- P5 state.
    health = 100.0,
    hurtCooldown = 0.0,
    hits = 0,          -- times an enemy has landed a touch
    inHeal = 0,        -- how many heal-spot sensors we are standing in
    healed = 0.0,
    weapon = 1,
    upgrades = 0,
    lastKills = 0,
    downed = false,
    downFor = 0.0,
    uiHealth = -1.0,
    uiScore = -1,
    uiMismatch = 0,
    pendingHurt = 0,
    probeAborted = false,

    -- P6.
    footsteps = nil,
    muzzle = nil,
    -- P8. The mesh child, which carries the skin and the AnimatorComponent.
    body = nil,
    clip = nil,
    stepTimer = 0.0,
    steps = 0,
    muzzleBursts = 0,

    fireCooldown = 0.0,
    refused = 0,
    yaw = 0.0,
    pitch = 0.0,
    looking = false,
    yawEntity = nil,
    cameraEntity = nil,
    -- Gate diagnostics: the P1 gate is "walk for two minutes without tipping, sinking or
    -- sticking", and all three are invisible without a readout.
    t = 0.0,
    nextReport = 5.0,
    startY = 0.0,
    minY = 1e9,
    maxTilt = 0.0,
    stuckFor = 0.0,
    lastPos = nil,
    worstStuck = 0.0,
    fell = false,
    wp = 1,
    frames = 0,
    groundedFrames = 0,
    -- Sampled extents lie: a 5 s report against a ~9 s lap aliases onto two points and makes a
    -- route that crosses the map look like a 4 m box. These are tracked every frame.
    xmn = 1e9, xmx = -1e9, zmn = 1e9, zmx = -1e9,
    wpHits = nil,
    -- P2's gate is about solidity, so the two things worth counting are whether the character
    -- was ever inside a building, and whether it was ever inside one of its *walls*.
    inBH = 0, inWH = 0, inWall = 0,
}

-- Footprints in world XZ. A position inside the outer box but not clear of the wall thickness
-- plus the capsule radius means the character is standing in a 0.3 m wall - which is what
-- tunnelling looks like from here.
-- door = the world XZ of the opening's centre, and the half-width of the corridor through it.
-- Without this, every legitimate doorway transit counts as being inside a wall: passing through a
-- door *is* being inside the footprint and within a wall thickness of the edge. The first run
-- reported inWall=299 for exactly that reason and none of it was tunnelling.
-- Every opening, not just the one the route uses. The colliders were generated with a gap at each
-- opening the mesh actually has, so passing through any of them is legitimate; the first pass at
-- this listed only the main doorway and reported 34 "tunnelling" frames that were all the
-- Blockhouse's other two gaps. Radius is the opening's half-width plus the capsule's 0.35 m.
local FOOTPRINTS = {
    { name = "BH", x = 16,  z = -6,  hx = 4.0,  hz = 3.58, doors = {
        { 20.0,  -6.0,  1.05 },   -- +X wall, 1.4 m gap
        { 18.56, -9.58, 1.25 },   -- -Z wall, 1.8 m doorway (the route uses this one)
        { 12.82, -2.42, 0.85 },   -- +Z wall, 1.0 m gap
    } },
    { name = "WH", x = -22, z = -14, hx = 10.0, hz = 5.775, doors = {
        { -32.0, -15.92, 0.80 },  -- -X wall, 0.9 m door
    } },
}

-- The circuit, in world XZ. Chosen to drive the capsule *into* the three obstacles from more than
-- one side, because sticking only shows up against geometry, and to stay well inside the ground
-- plane, which spans +-50. The first attempt at this steered by constant arcs instead; two arcs
-- of opposite curvature make an S, an S translates, and it wandered off the map at t=80s.
-- Every point is in OPEN space. The second attempt aimed at the obstacles' centres, which are
-- inside solid geometry: the capsule pressed against a block's west face for 124 s and the route
-- never advanced, because a velocity-driven capsule does not slide along a wall - drive it
-- straight at a surface and the solver cancels the whole velocity, leaving no tangential
-- component to carry it sideways. That is CharacterVirtual's job, not this flag's.
--
-- So the route grazes the geometry instead of aiming through it. The three placeholder boxes it
-- was originally laid out against - two cover blocks and a 0.2 m step at z = -8 - are gone; the
-- buildings are the only obstacles now, and the waypoints below are kept because the *path* is
-- what the gate measures, not what it passes. See docs/ToDo/PROVING_GROUND.md for the step-up
-- coverage that went with the Step.
-- P2 extends this through the two buildings. Their doorways were measured off the meshes rather
-- than guessed, and the route aims at each one and then at a point INSIDE, so "walk in and out of
-- every building" is exercised rather than asserted:
--   Blockhouse  centre (16, -6),  8.0 x 7.2,  doorway 1.8 m on its -Z wall at local x +2.56
--   Warehouse   centre (-22, -14), 20 x 11.5, door    0.9 m on its -X wall at local z -1.92
local ROUTE = {
    {  0, -7 },    -- open floor (was head-on at the 0.2 m Step, removed with the boxes)
    {  9, -4 },    -- open floor (was along Block A's east face)
    { 18.6, -11 }, -- line up on the Blockhouse doorway from outside
    { 18.6, -6 },  -- through it, into the middle of the Blockhouse
    { 18.6, -11 }, -- and back out the way it came
    {  4,  6 },    -- open floor, a long run to reach full speed
    {-11,  5 },    -- open floor (was past Block B's west end)
    {-34, -15.9 }, -- line up on the Warehouse door
    {-22, -15.9 }, -- through it, into the middle of the Warehouse
    {-34, -15.9 }, -- and out
    {  0,  0 },    -- back through the middle
}

-- P4's gate route. Not a circuit - a sequence of marked spots to stand on, because the question
-- "does the wall occlude" only has a clean answer while nothing is moving.
--
-- The geometry it is built on, read off the scene rather than guessed. The Blockhouse's -Z wall
-- is two collider segments at world z = -9.43: one spanning x 12.00..17.66, one spanning
-- x 19.46..20.00, leaving the 1.8 m doorway at x 17.66..19.46. The Sentry stands inside at
-- (18.4, -6) and never turns, so its eye is fixed.
--
-- A sight line from (18.4, -6) to a player at (px, -12) crosses z = -9.43 at
--     x = 18.4 + 0.5717 * (px - 18.4)
-- so the wall's inner edge at x = 17.66 predicts the crossover at **px = 17.11**. That is the
-- falsifiable part: the Sentry should acquire the player within a body-width of x = 17.1 on the
-- way east, and lose it again near the same x on the way back.
--
-- { x, z, seconds to stand there, what it is for }
local LOS_ROUTE = {
    {  2.00, -11.0, 0.5, "staging - open floor; Block A used to sit on the line from spawn" },
    { 14.00, -12.0, 8.0, "behind the -Z wall: the Sentry must NOT acquire" },
    { 18.56, -12.0, 8.0, "on the doorway's sight line: it must" },
    { 14.00, -12.0, 6.0, "back behind the wall: it must lose me again" },
}

-- P5's gate route. Each stop exercises one mechanism, in the order that makes the next one mean
-- something: you cannot show a heal spot working without first being hurt, and you cannot show an
-- upgrade station working without first having banked something to spend.
--
-- Leg 1 also fires, which is not decoration: the enemies that come to hurt you are the ones that
-- die to give you the score leg 4 spends.
local P5_ROUTE = {
    {   2.0,  -2.0, 14.0, "stand and fight - take contact damage, and bank kills" },
    {  -6.0,  -6.0,  8.0, "the heal spot: health must climb back" },
    {  10.0,   2.0,  3.0, "the weapon crate: fire interval must halve, and it must be consumed" },
    { -14.0,   6.0,  5.0, "the upgrade station: spend score for projectile damage" },
    {   0.0,   0.0,  3.0, "back to the middle" },
}

function Player:OnCreate()
    self.yawEntity = self.entity:GetChildByName("Yaw")
    if not self.yawEntity then
        Log.Error("Player: no child named 'Yaw' - turning and the camera will not work")
    else
        self.cameraEntity = self.yawEntity:GetChildByName("Main Camera")
        if self.cameraEntity then
            -- Seed pitch from whatever the scene authored, so capturing the mouse does not snap
            -- the view level on the first frame.
            self.pitch = self.cameraEntity:GetRotation().x
        end
    end

    -- P6. Both are optional on purpose: the scripts have to keep working in a scene that has
    -- not been given sound or particles yet, and a missing child is an authoring state rather
    -- than an error.
    self.footsteps = self.entity:GetChildByName("Footsteps")
    if self.yawEntity then
        self.muzzle = self.yawEntity:GetChildByName("Muzzle")
    end
    self.body = self.entity:GetChildByName("Body")
    if not self.footsteps then Log.Warn("Player: no 'Footsteps' child - no step sound") end
    if not self.muzzle then Log.Warn("Player: no 'Muzzle' child under Yaw - no muzzle flash") end
    if not self.body then Log.Warn("Player: no 'Body' child - the player will not animate") end

    Log.Info("Player: click to look, Escape to release the cursor")

    local p = self.entity:GetTranslation()
    self.startY = p.y
    self.lastPos = p
    Log.Info(string.format("Player ready at (%.2f, %.2f, %.2f)", p.x, p.y, p.z))

    -- P5. PG.damage is what an enemy subtracts when a round lands, and it lives in PG because
    -- there is no way to call into another entity's script instance; PG.score is banked here and
    -- spent at the upgrade station. Both are seeded once, by whoever gets here first.
    PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }
    PG.damage = PG.damage or 1
    PG.score = PG.score or 0
    self.health = self.maxHealth
    self:PushUI()

    if self.p5gate then
        PG.freeze = false
        Log.Info("P5GATE armed")
    end

    if self.losgate then
        -- The full initialiser, not `PG or {}`: Fire() only fills the counters in when PG is
        -- absent entirely, so a PG that exists but holds nothing but `freeze` would make the
        -- first shot add 1 to nil.
        PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }
        PG.freeze = true
        Log.Info("LOSGATE armed: enemies sense but do not move")
    end
end

-- ---- P5: health, and the four things that change it ----
--
-- Every one of these arrives as a contact, and every one of them is a contact the engine could not
-- deliver until P5: the player is a CharacterVirtual, and a character had no presence in the
-- broadphase at all. Enemies passed through it, projectiles passed through it, and a trigger
-- volume could not notice it. The inner body landed on master for this phase.
function Player:OnCollisionEnter(other)
    if not other then return end
    local name = other:GetName()

    if name == "Heal Spot" or name == "Weapon Crate" or name == "Upgrade Station" then
        Audio.PlayOneShot("audio/chime.wav", nil, 0.8)
    end

    if name == "Enemy" then
        -- A budget of ticks, not a "while touching" flag, and the difference is an engine gap
        -- rather than a preference. **There is no OnCollisionStay.** Enter fires once when the
        -- contact is made and never again while it lasts, so "an enemy is on me" has to be
        -- reconstructed by counting enter/exit pairs - and that counter leaks the moment an enemy
        -- dies while touching, because the entity is gone before its contact-removed event can be
        -- resolved back to it. A budget cannot leak: three ticks per touch, spent one a second,
        -- and the jostling re-makes the contact often enough to keep it topped up.
        -- Recorded in docs/ToDo/cross-cutting.md.
        self.pendingHurt = math.min((self.pendingHurt or 0) + 3, 6)
    elseif name == "Heal Spot" then
        self.inHeal = self.inHeal + 1
    elseif name == "Weapon Crate" then
        self.weapon = self.weapon + 1
        -- Halving the interval is the visible half of the pickup; the fired count in the gate
        -- report is what proves it, because nothing else about the run changes.
        self.fireInterval = self.fireInterval * 0.5
        Log.Info(string.format("PICKUP weapon -> level %d, fireInterval %.3f",
            self.weapon, self.fireInterval))
    elseif name == "Upgrade Station" then
        self:Upgrade()
    end
end

function Player:OnCollisionExit(other)
    if other and other:GetName() == "Heal Spot" and self.inHeal > 0 then
        self.inHeal = self.inHeal - 1
    end
end

function Player:Hurt()
    if self.downed then
        return
    end
    self.hurtCooldown = self.damageCooldown
    self.hits = self.hits + 1
    self.health = math.max(0.0, self.health - self.contactDamage)
    if self.health <= 0.0 then
        self.downed = true
        self.downFor = 0.0
        -- Not a respawn. A character cannot be teleported from script - its TransformComponent is
        -- overwritten from the controller every frame by SyncTransforms, and nothing exposes
        -- CharacterVirtual::SetPosition - so "back to the spawn point" is not expressible today.
        -- Recorded in docs/ToDo/cross-cutting.md. It recovers where it fell instead.
        Log.Warn(string.format("PLAYER DOWN at %.0f hits taken", self.hits))
    end
end

function Player:Upgrade()
    if PG.score < self.upgradeCost then
        self.refusedUpgrades = (self.refusedUpgrades or 0) + 1
        Log.Info(string.format("UPGRADE refused: score %d < cost %d",
            PG.score, math.floor(self.upgradeCost)))
        return
    end
    PG.score = PG.score - self.upgradeCost
    PG.damage = PG.damage + 1
    self.upgrades = self.upgrades + 1
    Log.Info(string.format("UPGRADE bought: projectile damage -> %d, score left %d",
        PG.damage, PG.score))
end

-- The HUD data model is the thing P5 is meant to put under real gameplay: it has been bound since
-- the runtime milestone and has never had a number in it that gameplay produced. Written only on
-- change, and **read straight back**, because a setter that silently drops its value would look
-- exactly like a setter that worked.
function Player:PushUI()
    if self.health ~= self.uiHealth then
        self.uiHealth = self.health
        UI.SetHealth(self.health)
        if math.abs(UI.GetHealth() - self.health) > 0.001 then
            self.uiMismatch = self.uiMismatch + 1
        end
    end
    if PG.score ~= self.uiScore then
        self.uiScore = PG.score
        UI.SetScore(PG.score)
        if UI.GetScore() ~= PG.score then
            self.uiMismatch = self.uiMismatch + 1
        end
    end
end

-- Walk to the next probe point, stand on it, move on. Returns a steering contribution in the
-- same units AutoTurn uses, and sets probeDrive / probeSlow, which the movement block reads.
--
-- The slowdown is not polish. At 6 m/s a frame covers 0.1 m, so a tight arrival radius is
-- overshot and the capsule orbits the point forever; a large one makes the probe position
-- imprecise, and this gate is a claim about a specific x. Easing to 1.2 m/s inside 3 m makes a
-- 0.35 m radius reachable without either.
function Player:ProbeTurn(ts)
    local route = self.losgate and LOS_ROUTE or P5_ROUTE
    local tag = self.losgate and "LOSGATE" or "P5GATE"
    local p = self.entity:GetTranslation()
    local target = route[self.probeWp]

    if self.probeDone then
        self.probeDrive = false
        self.probeSlow = 1.0
        return 0.0
    end

    -- A route measured in XZ will happily report arriving at every remaining waypoint while the
    -- capsule falls through the world, because horizontal control still works in freefall. The
    -- first P5 run did exactly that: shoved through the ground plane at t=83s, and 700 m down it
    -- was still "reaching" probe 3. Diagnose already shouts about the fall; this makes the route
    -- stop claiming things, so a failed run cannot read as a passing one.
    if self.fell then
        if not self.probeAborted then
            self.probeAborted = true
            Log.Error(tag .. " ABORTED: left the ground plane, so nothing below is measured")
        end
        self.probeDrive = false
        self.probeSlow = 1.0
        return 0.0
    end

    if self.probeHold > 0.0 then
        -- Leg 1 of the P5 route is "stand until it hurts", not "stand for 14 seconds". A fixed
        -- hold made the gate depend on whether an enemy happened to arrive in time: one run
        -- reached the heal spot at full health, where a heal spot proves nothing. It still has a
        -- ceiling, because a gate that can hang is not a gate.
        if not self.losgate and self.probeWp == 1 and self.hits < 3 and self.t < 45.0 then
            return 0.0
        end
        self.probeHold = self.probeHold - ts
        self.probeDrive = false
        self.probeSlow = 1.0
        if self.probeHold <= 0.0 then
            Log.Info(string.format("%s probe %d done, standing at (%.2f, %.2f) hp=%.0f score=%d",
                tag, self.probeWp, p.x, p.z, self.health, PG.score))
            if self.probeWp >= #route then
                self.probeDone = true
                Log.Info(tag .. " route complete")
            else
                self.probeWp = self.probeWp + 1
            end
        end
        return 0.0
    end

    local dx, dz = target[1] - p.x, target[2] - p.z
    local dist = math.sqrt(dx * dx + dz * dz)

    if dist < 0.35 then
        -- max(..., 0.001) so a zero-second hold still takes the branch above next frame rather
        -- than re-arriving, and re-logging, every frame.
        self.probeHold = math.max(target[3], 0.001)
        self.probeDrive = false
        self.probeSlow = 1.0
        Log.Info(string.format("%s probe %d at (%.2f, %.2f), holding %.1fs - %s",
            tag, self.probeWp, p.x, p.z, target[3], target[4]))
        return 0.0
    end

    self.probeDrive = true
    self.probeSlow = dist < 3.0 and 0.2 or 1.0

    local want = math.atan(-dx, -dz)
    local diff = (want - self.yaw + math.pi) % (2 * math.pi) - math.pi
    return math.max(-1.0, math.min(1.0, diff * 2.0))
end

function Player:OnUpdate(ts)
    -- Frame 1 spans boot and is over a second long (docs/ToDo/cross-cutting.md). Applying a
    -- second of input to a character on its first frame launches it across the map, so the first
    -- frame is skipped outright rather than clamped - a controller has nothing useful to do with
    -- it either way.
    if ts > 0.25 then
        return
    end

    self.t = self.t + ts

    -- ---- capture ----
    if not self.looking and Input.IsMouseButtonPressed(Mouse.ButtonLeft) then
        Input.SetCursorMode(Cursor.Locked)
        self.looking = true
    elseif self.looking and Input.IsKeyPressed(Key.Escape) then
        Input.SetCursorMode(Cursor.Normal)
        self.looking = false
    end

    -- ---- look ----
    if self.looking then
        -- NOT scaled by ts. The delta is pixels moved last frame - an amount, not a rate - and
        -- multiplying it by frame time makes sensitivity depend on framerate.
        local dx, dy = Input.GetMouseDelta()
        self.yaw = self.yaw - dx * self.sensitivity
        self.pitch = self.pitch - dy * self.sensitivity
        -- ~80 degrees. Past vertical the forward vector flips and the controls invert.
        if self.pitch > 1.4 then self.pitch = 1.4 end
        if self.pitch < -1.4 then self.pitch = -1.4 end
        if self.cameraEntity then
            self.cameraEntity:SetRotation(Vec3(self.pitch, 0, 0))
        end
    end

    -- ---- fire ----
    -- The same left button captures the cursor and then fires, which is the convention every
    -- shooter uses: the first click is "I am playing now", the rest are shots.
    self.fireCooldown = self.fireCooldown - ts
    if self.autofire then
        -- Three phases, so one run answers all of P3's gate rather than only the easy part:
        --   < 50 s  one round a frame - sustained fire, which is what a leak would show up in
        --   50-60 s 100 requests a frame - the only way to reach a 64-per-frame cap, since one
        --           shot a frame never comes close to it
        --   > 60 s  stop, and let the 3 s lifetime drain the last rounds so "entity count
        --           returns to baseline" can actually be observed rather than inferred
        local shots = 0
        if self.t < 20.0 then shots = 1
        elseif self.t < 24.0 then shots = 80 end
        for _ = 1, shots do self:Fire() end
    elseif self.p5gate and not self.probeDone and self.probeWp == 1 and self.fireCooldown <= 0.0 then
        -- Only on the first leg, and on the ordinary cooldown rather than every frame: this is
        -- meant to look like someone shooting back, and the fired count has to stay a number the
        -- weapon pickup can visibly change later in the run.
        self.fireCooldown = self.fireInterval
        self:Fire()
    elseif self.looking and Input.IsMouseButtonPressed(Mouse.ButtonLeft)
        and self.fireCooldown <= 0.0 then
        self.fireCooldown = self.fireInterval
        self:Fire()
    end

    -- ---- turn (keyboard fallback, and the autopilot's only steering) ----
    local turn = 0.0
    if Input.IsKeyPressed(Key.Q) or Input.IsKeyPressed(Key.Left) then turn = turn + 1.0 end
    if Input.IsKeyPressed(Key.E) or Input.IsKeyPressed(Key.Right) then turn = turn - 1.0 end
    if self.autopilot then turn = turn + self:AutoTurn() end
    if self.losgate or self.p5gate then turn = turn + self:ProbeTurn(ts) end
    self.yaw = self.yaw + turn * self.turnSpeed * ts
    if self.yawEntity then
        self.yawEntity:SetRotation(Vec3(0, self.yaw, 0))
    end

    -- ---- move ----
    -- Forward is -Z at yaw 0, matching the engine's camera convention.
    local sinY, cosY = math.sin(self.yaw), math.cos(self.yaw)
    local fx, fz = -sinY, -cosY
    local rx, rz = cosY, -sinY

    local ix, iz = 0.0, 0.0
    if self.autopilot then ix, iz = ix + fx, iz + fz end
    if self.probeDrive then ix, iz = ix + fx, iz + fz end
    if Input.IsKeyPressed(Key.W) then ix = ix + fx; iz = iz + fz end
    if Input.IsKeyPressed(Key.S) then ix = ix - fx; iz = iz - fz end
    if Input.IsKeyPressed(Key.D) then ix = ix + rx; iz = iz + rz end
    if Input.IsKeyPressed(Key.A) then ix = ix - rx; iz = iz - rz end

    local len = math.sqrt(ix * ix + iz * iz)
    if len > 0.0 then
        ix, iz = ix / len, iz / len
    end

    -- Keep the body's own vertical velocity. Overwriting it with 0 would cancel gravity and the
    -- capsule would hang in the air the moment it walked off anything.
    local v = self.entity:GetLinearVelocity()
    -- Downed is not dead: a character cannot be moved from script, so there is nowhere to
    -- respawn to. It stops instead, and gets back up where it fell.
    local speed = self.downed and 0.0 or (self.speed * self.probeSlow)
    self.entity:SetLinearVelocity(Vec3(ix * speed, v.y, iz * speed))

    self:Tick(ts)
    self:PushPending()
    self:Diagnose(ts)
end

-- Health, score and the HUD, once a frame.
function Player:Tick(ts)
    self.hurtCooldown = math.max(0.0, self.hurtCooldown - ts)

    if (self.pendingHurt or 0) > 0 and self.hurtCooldown <= 0.0 and not self.downed then
        self.pendingHurt = self.pendingHurt - 1
        self:Hurt()
    end

    if self.downed then
        self.downFor = self.downFor + ts
        if self.downFor > 3.0 then
            self.downed = false
            self.health = self.maxHealth
            Log.Info("PLAYER up again (in place - see Player:Hurt)")
        end
    elseif self.inHeal > 0 and self.health < self.maxHealth then
        self.health = math.min(self.maxHealth, self.health + self.healRate * ts)
        self.healed = self.healed + self.healRate * ts
    end

    -- Score is banked from kills. Enemy.lua owns PG.kills, because the victim counts its own
    -- death; this only notices the counter moving.
    if PG.kills > self.lastKills then
        PG.score = PG.score + (PG.kills - self.lastKills)
        self.lastKills = PG.kills
    end

    self:Step(ts)
    self:Animate()
    self:PushUI()
end

-- The player's own three clips, chosen from the capsule's measured horizontal speed rather
-- than from the input. Easing toward a probe point runs at 1.2 m/s (Player:Route), and driving
-- this off "is a key held" would snap between idle and a sprint through the whole approach.
--
-- The Body child sits under the capsule, not under Yaw, so it does not inherit the mouse yaw -
-- which is correct for now: the mesh faces the capsule's forward, and the capsule never turns.
-- The character therefore strafes without turning, the same way the placeholder cube did. A
-- turning mesh needs the Body reparented under Yaw, and that is a change to what the gates
-- measured, so it is written into docs/ToDo/ rather than folded in here.
function Player:Animate()
    if not self.body then
        return
    end

    local v = self.entity:GetLinearVelocity()
    local speed = math.sqrt(v.x * v.x + v.z * v.z)

    -- Airborne or downed is not walking, whatever the horizontal velocity says. Same test
    -- Player:Step uses to keep footsteps off a falling character.
    if self.downed or not self.entity:IsGrounded() then
        speed = 0.0
    end

    local clip, animSpeed
    if speed < 0.5 then
        clip, animSpeed = "Idle", 1.0
    elseif speed < 4.0 then
        clip, animSpeed = "Casual_Walk", 1.0
    else
        clip, animSpeed = "RunFast", 0.6
    end

    self.body:PlayAnimation(clip)
    self.body:SetAnimationSpeed(animSpeed)
    self.clip = clip
end

-- Footsteps.
--
-- An AudioSourceComponent rather than Audio.PlayOneShot. That started as a workaround: the
-- one-shot binding could not be given a volume - AudioEngine::PlayOneShot takes one, and its own
-- comment says it exists "for footsteps and impacts", but the Lua side passed a hardcoded 1.0, so
-- every one-shot was full blast. That is fixed on master now, and PlayOneShot takes an optional
-- gain.
--
-- It stays on the component anyway, because the two routes are not the same test. This one
-- exercises PlaySound/StopSound on a component-owned voice and the Stop-then-Play retrigger idiom;
-- the shot and the impact exercise one-shots, now including the volume that was missing.
--
-- Retriggered with Stop-then-Play, because PlaySound on an already-playing source is documented
-- as a no-op rather than a restart - which is what makes calling it every frame from a branch
-- safe everywhere else, and exactly wrong here.
function Player:Step(ts)
    if not self.footsteps then
        return
    end

    local v = self.entity:GetLinearVelocity()
    local speed = math.sqrt(v.x * v.x + v.z * v.z)
    -- Grounded matters: the same velocity while falling is not walking, and a character sliding
    -- down a slope should not sound like it is striding.
    if speed < 1.0 or not self.entity:IsGrounded() then
        -- Reset rather than pause, so the first step after stopping lands immediately instead of
        -- on whatever fraction of a stride was left over.
        self.stepTimer = self.stepInterval
        return
    end

    self.stepTimer = self.stepTimer - ts
    if self.stepTimer > 0.0 then
        return
    end

    -- Cadence with speed: full speed is 6 m/s, and a slow walk should not tick at the same rate.
    self.stepTimer = self.stepInterval * (self.speed / math.max(speed, 0.1))
    self.steps = self.steps + 1
    self.footsteps:StopSound()
    self.footsteps:PlaySound()
end

function Player:Fire()
    local p = self.entity:GetTranslation()
    local sinY, cosY = math.sin(self.yaw), math.cos(self.yaw)
    local fx, fz = -sinY, -cosY

    -- A metre ahead and at chest height. Spawning inside the capsule would have the round collide
    -- with the player on its first step, and the shot would die where it was born.
    local muzzle = Vec3(p.x + fx * 1.0, p.y + 0.5, p.z + fz * 1.0)

    local id = Scene.Spawn("prefabs/Projectile.gprefab", muzzle)
    if not id then
        -- nil means the per-frame spawn cap refused it. Counting refusals is the point of
        -- autofire: the cap should hold without the game noticing anything but fewer rounds.
        self.refused = self.refused + 1
        return
    end

    PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }
    PG.fired = PG.fired + 1

    -- P6. **After the spawn, not before.** The first version flashed and banged first, and the
    -- gate caught it in one line: muzzle-bursts=2602 against fired=1894 with refused=708, and
    -- 1894 + 708 = 2602 exactly. Every shot the spawn cap turned away still made a noise and a
    -- flash, which is a gun that fires blanks under load - visible only because the counters were
    -- kept separately.
    --
    -- The shot itself is unspatialised: it is at the listener by definition, and spatialising a
    -- sound that starts inside the ear gives you a bang that pans depending on which way you were
    -- facing when you pulled the trigger.
    Audio.PlayOneShot("audio/impact.wav", nil, 0.7)
    if self.muzzle then
        self.muzzleBursts = self.muzzleBursts + 1
        self.muzzle:EmitBurst(6)
    end

    -- The entity does not exist until the command queue flushes at the next FrameBegin, so the
    -- velocity cannot be set here. Stash the id and push it next frame.
    self.pending = self.pending or {}
    self.pending[#self.pending + 1] = { id = id, vx = fx * self.muzzleSpeed, vz = fz * self.muzzleSpeed }
end

function Player:PushPending()
    if not self.pending or #self.pending == 0 then return end
    local still = {}
    for i = 1, #self.pending do
        local q = self.pending[i]
        local e = Scene.FindEntityByUUID(q.id)
        if e then
            e:SetLinearVelocity(Vec3(q.vx, 0, q.vz))
        else
            -- Not there yet; the flush happens at FrameBegin so one frame of lag is normal.
            -- Anything still missing after that is a spawn that failed and is dropped.
            if not q.waited then q.waited = true; still[#still + 1] = q end
        end
    end
    self.pending = still
end

-- A circuit that walks the map and meets every obstacle in it: straight stretches long enough to
-- reach full speed, turns that bring it back around, and headings that used to aim at the three
-- placeholder boxes. Those are gone, so the buildings carry that job alone now. Walking *into*
-- things is the point - sticking is
-- one of the three failure modes and it only shows up against geometry.
function Player:AutoTurn()
    local p = self.entity:GetTranslation()
    local target = ROUTE[self.wp]
    local dx, dz = target[1] - p.x, target[2] - p.z

    -- Advance on arrival, or on having been stuck for a second and a half. The escape stays for
    -- P4's enemies, which need the same behaviour, but on a character controller it should now
    -- never fire: sliding along a wall is exactly what it was compensating for. Every firing is
    -- a finding, so it logs as a warning rather than quietly recovering.
    if self.stuckFor > 1.5 then
        self.stuckFor = 0.0
        self.wp = (self.wp % #ROUTE) + 1
        self.wpHits = self.wpHits or {}
        self.wpHits[self.wp] = (self.wpHits[self.wp] or 0) + 1
        Log.Warn(string.format("autopilot: stuck at (%.1f, %.1f), skipping to waypoint %d",
            p.x, p.z, self.wp))
        target = ROUTE[self.wp]
        dx, dz = target[1] - p.x, target[2] - p.z
    elseif (dx * dx + dz * dz) < 2.25 then
        self.wp = (self.wp % #ROUTE) + 1
        self.wpHits = self.wpHits or {}
        self.wpHits[self.wp] = (self.wpHits[self.wp] or 0) + 1
        target = ROUTE[self.wp]
        dx, dz = target[1] - p.x, target[2] - p.z
    end

    -- Forward is -Z at yaw 0, so the yaw that points at (dx, dz) is atan2(-dx, -dz). Steering is
    -- proportional and clamped to +-1, which is what the keyboard would supply.
    local want = math.atan(-dx, -dz)
    local diff = (want - self.yaw + math.pi) % (2 * math.pi) - math.pi
    return math.max(-1.0, math.min(1.0, diff * 2.0))
end

-- The three ways P1's gate can fail, measured rather than eyeballed.
function Player:Diagnose(ts)
    local p = self.entity:GetTranslation()
    local r = self.entity:GetRotation()

    if p.x < self.xmn then self.xmn = p.x end
    if p.x > self.xmx then self.xmx = p.x end
    if p.z < self.zmn then self.zmn = p.z end
    if p.z > self.zmx then self.zmx = p.z end

    -- grounded: a character that is never grounded is falling, and a gate that only checks
    -- height would not notice it skimming the floor.
    if self.entity:IsGrounded() then self.groundedFrames = self.groundedFrames + 1 end
    self.frames = self.frames + 1

    -- tipping: any rotation away from upright at all - a character never rotates from physics
    local tilt = math.max(math.abs(r.x), math.abs(r.z))
    if tilt > self.maxTilt then self.maxTilt = tilt end

    -- sinking: the capsule's centre should never drop below where it settled
    if p.y < self.minY then self.minY = p.y end

    -- Falling off the world is not "sinking", it is a different failure, and it has to be said
    -- loudly: the first gate run reported a 2.13 s stick and a minY of -5229, both of which were
    -- one event - the capsule left the ground plane and never landed.
    if p.y < -5.0 and not self.fell then
        self.fell = true
        Log.Error(string.format("GATE FAIL: left the ground plane at (%.1f, %.1f, %.1f)",
            p.x, p.y, p.z))
    end

    -- sticking: asking to move but not moving. Excludes anything in freefall, where horizontal
    -- movement is legitimately zero and counting it produced a false 2.13 s.
    local moved = math.sqrt((p.x - self.lastPos.x) ^ 2 + (p.z - self.lastPos.z) ^ 2)
    local grounded = math.abs(self.entity:GetLinearVelocity().y) < 1.0
    local wants = grounded and (self.autopilot or self.probeDrive or Input.IsKeyPressed(Key.W)
        or Input.IsKeyPressed(Key.S) or Input.IsKeyPressed(Key.A) or Input.IsKeyPressed(Key.D))
    if wants and moved < 0.001 then
        self.stuckFor = self.stuckFor + ts
        if self.stuckFor > self.worstStuck then self.worstStuck = self.stuckFor end
    else
        self.stuckFor = 0.0
    end
    self.lastPos = p

    for i = 1, #FOOTPRINTS do
        local f = FOOTPRINTS[i]
        local dx, dz = math.abs(p.x - f.x), math.abs(p.z - f.z)
        if dx < f.hx and dz < f.hz then
            if f.name == "BH" then self.inBH = self.inBH + 1 else self.inWH = self.inWH + 1 end
            if dx > f.hx - 0.65 or dz > f.hz - 0.65 then
                -- ... unless it is in the doorway, which is the one place being there is correct.
                local atDoor = false
                for k = 1, #f.doors do
                    local d = f.doors[k]
                    local ddx, ddz = p.x - d[1], p.z - d[2]
                    if (ddx * ddx + ddz * ddz) <= (d[3] * d[3]) then atDoor = true break end
                end
                if not atDoor then
                    self.inWall = self.inWall + 1
                    if self.inWall == 1 then
                        Log.Warn(string.format(
                            "GATE: inside %s's wall at (%.2f, %.2f) - possible tunnelling",
                            f.name, p.x, p.z))
                    end
                end
            end
        end
    end

    if self.t >= self.nextReport then
        self.nextReport = self.nextReport + 5.0
        Log.Info(string.format(
            "GATE t=%.0fs pos=(%.1f, %.2f, %.1f) grounded=%.0f%% inWall=%d | fired=%d live=%d despawned=%d hits=%d kills=%d refused=%d",
            self.t, p.x, p.y, p.z,
            100.0 * self.groundedFrames / math.max(self.frames, 1), self.inWall,
            PG.fired, PG.fired - PG.despawned, PG.despawned, PG.hits, PG.kills, self.refused))
        Log.Info(string.format(
            "P5   hp=%.0f/%.0f taken=%d healed=%.0f weapon=%d dmg=%d score=%d upgrades=%d "
            .. "triggers=%d ui-mismatch=%d",
            self.health, self.maxHealth, self.hits, self.healed, self.weapon, PG.damage,
            PG.score, self.upgrades, PG.triggers or 0, self.uiMismatch))
        Log.Info(string.format(
            "P6   steps=%d muzzle-bursts=%d impacts=%d impacts-despawned=%d live-impacts=%d "
            .. "ui-health=%.0f ui-score=%d",
            self.steps, self.muzzleBursts, PG.impacts or 0, PG.impactsDespawned or 0,
            (PG.impacts or 0) - (PG.impactsDespawned or 0), UI.GetHealth(), UI.GetScore()))
        -- Voices are the audio equivalent of P3's entity count: component-owned voices should sit
        -- at a fixed number (one per enemy, plus music and footsteps) and one-shots should drain
        -- back toward zero. Both counters were bound for this line.
        Log.Info(string.format("P6b  voices=%d one-shots=%d",
            Audio.GetVoiceCount(), Audio.GetOneShotCount()))
        local hits = ""
        for i = 1, #ROUTE do
            hits = hits .. string.format(" wp%d=%d", i, (self.wpHits and self.wpHits[i]) or 0)
        end
        Log.Info(string.format("EXTENT x %.1f..%.1f  z %.1f..%.1f  arrivals:%s",
            self.xmn, self.xmx, self.zmn, self.zmx, hits))
    end
end

local ____exports = {}
____exports.default = Player
return ____exports
