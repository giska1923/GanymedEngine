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

-- Damage per round, by gun. The ladder is PROVING_GROUND.md's "weapon ladder", smallest to
-- largest: 8 for the pistol and +2 a rung, so the rifle in the player's hands deals 12.
local GUN_LADDER = { "Pistol", "SMG", "Rifle", "LMG", "Minigun" }
local GUN_DAMAGE = {}
for i, name in ipairs(GUN_LADDER) do
    GUN_DAMAGE[name] = 8 + 2 * (i - 1)
end
local UPGRADE_DAMAGE = 2

-- How each gun sits on this rig: scale, socket joint, offset, rotation (Euler X*Y*Z radians), and
-- whether the left hand holds it too. The same numbers prefabs/weapons/<Gun>.gprefab carries, from
-- the Meshy tooling's holds.json (weapon_holds.py). They live here as well because the guns on Body
-- are authored with no socket: a socket is attached only to the gun in hand, since hand IK holds the
-- first socketed child, and AttachToBone takes the whole socket every call, with no getter to read
-- one back. The pistol is held in the right hand like the knife, so it takes no IK.
local GUNS = {
    Pistol  = { 1.0,  "RightHand", Vec3(-0.141403, 0.116582, 0.011800), Vec3(-2.714311, 0.254526, 0.767077), false },
    SMG     = { 1.0,  "Spine", Vec3(0.059234, -0.081445, 0.252583), Vec3(3.042971, 0.887004, -2.925548), true },
    Rifle   = { 0.45, "Spine", Vec3(0.173818, 0.000528, 0.397881),  Vec3(3.039984, 0.884929, -2.919410), true },
    LMG     = { 1.0,  "Spine", Vec3(0.370200, 0.080711, 0.367841),  Vec3(3.042971, 0.887004, -2.925548), true },
    Minigun = { 1.0,  "Spine", Vec3(0.283391, -0.097242, 0.617023), Vec3(3.042971, 0.887004, -2.925548), true },
}
-- How each gun fires: rounds a second, and whether holding LMB keeps it firing. The pistol is
-- semi-automatic: one round per click, however long the button is held, and its rate is only a
-- cap on how fast clicks are honoured. The rest are automatic and climb with the ladder, so a rung
-- up is both harder-hitting and faster: rounds/s x GUN_DAMAGE is 80, 120, 168 and 240 a second.
--
-- `spread` is the cone a round can leave in, as the most it can stray from the aim (the cone's
-- half-angle, degrees): the first, careful shot, and the cone after a long burst. Sustained fire
-- widens it (bloom), stopping narrows it again - so short bursts stay accurate and holding the
-- trigger sprays, the trade every shooter is built on. SPREAD_BLOOM and SPREAD_RECOVER set how.
local GUN_FIRE = {
    Pistol  = { rate = 6.0,  auto = false, spread = { 0.5, 1.5 } },
    SMG     = { rate = 8.0,  auto = true,  spread = { 1.5, 6.0 } },
    Rifle   = { rate = 10.0, auto = true,  spread = { 0.5, 3.0 } },
    LMG     = { rate = 12.0, auto = true,  spread = { 1.0, 4.0 } },
    Minigun = { rate = 15.0, auto = true,  spread = { 2.0, 7.0 } },
}
-- Seconds of sustained fire, at the gun's own rate, to bloom from the first-shot cone to the full
-- one. Each round adds 1 / (rate * SPREAD_BLOOM) of "heat", so every gun takes the same time.
local SPREAD_BLOOM = 1.5
-- Heat starts to cool this long after the last round, and cools from full in SPREAD_RECOVER. The
-- delay is what lets bloom build at all: cooling every frame would outrun a 1.5 s bloom. It also
-- sets the pistol's rhythm - clicked at ~3/s the gaps cool it and it stays at 0.5 deg; spammed at
-- its 6/s cap no gap is long enough, and it blooms like the automatics.
local SPREAD_RECOVER_DELAY = 0.25
local SPREAD_RECOVER = 0.5
-- The floor's half-size (make_floor.py: 50 * sqrt(10), a 316 m square). Past it there is nothing
-- to stand on.
local WORLD_EDGE = 158.1
-- Seconds of falling off the world before the player dies and respawns.
local FALL_DEATH_TIME = 3.0
-- Where a gun that is swapped out lands: this far behind the player, as a pickup again.
local TOSS_DISTANCE = 2.5

-- LMB's attack for each melee weapon: the clips the knife and axe grips were fitted to. Their
-- length, damage window and lane come from MeleeAttacker.lua's table (PG.meleeWindows), so the
-- two scripts cannot disagree about when an attack is over.
local MELEE_ATTACK = { Knife = "Thrust_Slash", Axe = "Axe_Chop" }
-- A weapon that is not equipped is shrunk to this fraction of its scale, because a script cannot
-- hide an entity: there is no visibility flag, and DetachFromBone leaves it at the Body's origin.
-- Not zero - a singular world matrix would put NaN into the weapon's normals and into the hand
-- IK's weapon frame.
local HIDDEN_SCALE = 0.001

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
        -- What the upgrade station charges for +2 projectile damage (one rung of GUN_DAMAGE).
        upgradeCost = 2.0,

        -- The gun the player starts with, or "" for none: the knife only, and the guns are on
        -- the rack. The gate modes (autofire, autopilot, losgate, p5gate) always start with the
        -- rifle, because every gate predates pickups and fires from its first second.
        startGun = "",
        -- Seconds after a melee attack's clip has played out before the next one can start. The
        -- clip itself is always a lockout (see Player:Melee); this is the gap on top of it.
        meleeCooldown = 0.3,

        -- P6. Seconds between footsteps at full speed. 0.42 is a brisk walk; the sound is
        -- retriggered rather than looped, so this is the only thing setting the cadence.
        stepInterval = 0.42,
    },
    speed = 6.0,
    turnSpeed = 2.4,
    sensitivity = 0.0022,
    muzzleSpeed = 28.0,
    autofire = false,
    autopilot = false,
    losgate = false,
    p5gate = false,
    maxHealth = 100.0,
    contactDamage = 6.0,
    damageCooldown = 1.0,
    healRate = 14.0,
    upgradeCost = 2.0,
    startGun = "",
    meleeCooldown = 0.3,
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
    gun = nil,         -- the gun on slot 1, picked up off the rack; nil until one is
    gunsTaken = 0,
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
    -- World yaw the legs are turned to. Velocity while moving, including a strafe
    -- inside the aim-offset yaw limit. The camera's yaw when standing and aiming,
    -- and when a backpedal is past that limit.
    meshYaw = nil,
    -- Seconds left in which the body keeps facing the aim after a shot (see Player:Animate).
    aimHold = 0.0,
    -- Legs-versus-aim mode, with hysteresis (see BACKPEDAL_ENTER).
    backpedal = false,
    -- The aim offset's inputs are held while aiming and faded by aimBlend, never zeroed.
    aimBlend = 0.0, -- linear 0..1 ramp; the applied weight is its smoothstep
    aimPitchHeld = 0.0,
    aimYawHeld = 0.0,
    stepTimer = 0.0,
    steps = 0,
    muzzleBursts = 0,

    fireCooldown = 0.0,
    heat = 0.0,           -- spread bloom, 0 (first-shot cone) to 1 (full cone)
    sinceShot = 0.0,
    spreadStats = nil,    -- [gun] = cones used and deviations measured, for the 5 s report
    triggerHeld = false,  -- LMB held (and armed) last frame: the semi-automatic pistol fires on a press
    shots = nil,          -- [gun] = rounds fired by the trigger, for the 5 s report
    refused = 0,

    -- What is in hand: "Gun" (slot 1, whichever gun was picked up) or "Knife" (slot 2).
    equipped = "Knife",
    weapons = nil,     -- [Body child name] = { entity, scale as authored }
    attack = nil,      -- the attack in progress: { clip, t, length, yaw }
    meleeReady = 0.0,  -- seconds of meleeCooldown left
    attacks = 0,
    yaw = 0.0,
    pitch = 0.0,
    looking = false,
    lmbArmed = false,  -- LMB released since the click that captured the cursor
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
    spawn = nil,       -- where the player started, and respawns
    fallFor = 0.0,     -- seconds airborne off the world
    deaths = 0,
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
--
-- Only openings the *mesh* actually has. The first pass listed the Blockhouse's two window-wall
-- holes and the Warehouse's 0.90 m X- gap as doors, because that is where the box sets were open.
-- Those holes are closed (see PROVING_GROUND.md). Treating them as doors now would hide tunnelling
-- at the old locations: inWall would not increment if the capsule clipped a now-solid wall there.
-- Radius is the opening's half-width plus the capsule's 0.35 m.
local FOOTPRINTS = {
    { name = "BH", x = 16,  z = -6,  hx = 4.0,  hz = 3.58, doors = {
        { 16.465, -2.57, 1.29 },  -- +Z doorway, 1.87 m, the only walkable opening on either building
    } },
    { name = "WH", x = -22, z = -14, hx = 10.0, hz = 5.775, doors = {
        -- Sealed shell. No aperture at walking height on any face.
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
-- P2 extends this through the buildings. The doorways these waypoints originally aimed at were
-- NOT measured off the meshes - they were holes in the box sets, on walls the meshes draw solid,
-- and the route walked through them. Re-derived from the geometry (docs/ToDo/PROVING_GROUND.md):
--   Blockhouse  centre (16, -6),  8.0 x 7.2,  the only doorway is 1.87 m wide on its +Z wall,
--                                             world x 15.53..17.40 at z = -2.57
--   Warehouse   centre (-22, -14), 20 x 11.5, SEALED - no opening on any face, at any height
--
-- So one building is walked through and one is walked into, which is the honest version of the
-- gate: "walk inside and out of every building that has a door, and bounce off the one that does
-- not."
local ROUTE = {
    {  0,   -7 },   -- open floor (was head-on at the 0.2 m Step, removed with the boxes)
    { 12,    4 },   -- 0.30 m StepUp Ledge, 4 x 4 m pad (0.2 < h < StepHeight 0.4)
    {  9,   -4 },   -- open floor (was along Block A's east face)
    { 16.5,  1.5 }, -- line up on the Blockhouse doorway from outside, on its +Z face
    { 16.5, -6 },   -- through it, into the middle of the Blockhouse, past the Sentry
    { 16.5,  1.5 }, -- and back out the way it came
    {  4,    6 },   -- open floor, a long run to reach full speed
    {-11,    5 },   -- open floor (was past Block B's west end)
    {-33,  -14 },   -- a long run west, at full speed, ending clear of the Warehouse's -X wall
    {-22,   -6 },   -- cut back northeast. This line meets the sealed -X wall at speed, so the leg
                    -- is the no-tunnelling test and the wall-slide test in one: the capsule has to
                    -- slide north along the wall, round the corner at (-32, -8.2) and arrive.
    {  0,    0 },   -- back through the middle
}

-- P4's gate route. Not a circuit - a sequence of marked spots to stand on, because the question
-- "does the wall occlude" only has a clean answer while nothing is moving.
--
-- The geometry it is built on. The first version of this route ran on the -Z side, through what
-- was believed to be a 1.8 m doorway at x 17.66..19.46 and was in fact a hole in the box set on a
-- wall the mesh draws solid. That hole is closed; the real doorway is on +Z, so the probe line
-- moved with it.
--
-- The Blockhouse's +Z wall is two collider segments at world z = -2.57, spanning x 12.00..15.53
-- and x 17.40..20.00, with the 1.87 m doorway between them and a lintel above it from y = 3.11.
-- The Sentry stands inside at (18.4, -6), facing +Z (yaw pi) through that door. It never turns.
-- At 1.5 m its eye passes well under the lintel.
--
-- A sight line from (18.4, -6) to a player at (px, 1.0) crosses z = -2.57 at
--     x = 18.4 + 0.49 * (px - 18.4)
-- so the doorway's east jamb at x = 17.40 predicts the crossover at **px = 16.36**. That is the
-- falsifiable part: the Sentry should acquire the player within a body-width of x = 16.4 on the
-- way west, and lose it again near the same x on the way back.
--
-- { x, z, seconds to stand there, what it is for }
local LOS_ROUTE = {
    {  2.00, -11.0, 0.5, "staging - open floor, off the Blockhouse's sight lines" },
    { 20.00,   1.0, 8.0, "behind the +Z wall's east segment: the Sentry must NOT acquire" },
    { 15.50,   1.0, 8.0, "on the doorway's sight line: it must" },
    { 20.00,   1.0, 6.0, "back behind the wall: it must lose me again" },
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
    -- The rack, from its east end: line up east of it, walk west onto the minigun, then step out
    -- north at probe speed before turning for the next leg. The rifle is tossed behind, which is
    -- east, back along the line walked in. A first version walked straight onto the LMG in the
    -- middle; leaving at full speed, its turn swung 2.5 m wide and took the minigun as well.
    {   0.0, -12.0,  0.0, "line up east of the gun rack" },
    {  -2.9, -12.0,  3.0, "the gun rack: the rifle swaps for the minigun, which leaves the rack, and the rifle is tossed" },
    {  -2.9,  -9.5,  0.0, "step out of the rack" },
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
    -- Under Yaw, not under the capsule: the capsule is LockRotation and never turns, so a
    -- mesh parented to it cannot face where the player is aiming. Yaw is the entity the mouse
    -- drives, and it already carries the camera.
    self.body = self.yawEntity and self.yawEntity:GetChildByName("Body") or nil
    if not self.footsteps then Log.Warn("Player: no 'Footsteps' child - no step sound") end
    if not self.body then Log.Warn("Player: no 'Body' child - the player will not animate") end

    -- Every gun and the knife are children of Body. The knife keeps its socket; a gun gets one
    -- only while it is in hand (GUNS, above). Each gun's Muzzle is the barrel tip: turned so its
    -- -Z (GetWorldForward) runs down the barrel, the gun's -X. Fire reads its world transform.
    self.weapons = {}
    for _, name in ipairs({ "Pistol", "SMG", "Rifle", "LMG", "Minigun", "Knife" }) do
        local w = self.body and self.body:GetChildByName(name) or nil
        if w then
            self.weapons[name] = { entity = w, scale = w:GetScale(), socketed = name == "Knife" }
        elseif self.body then
            Log.Warn(string.format("Player: no '%s' under Body - it cannot be held", name))
        end
    end

    Log.Info("Player: click to look, Escape to release the cursor")

    local p = self.entity:GetTranslation()
    self.startY = p.y
    self.lastPos = p
    self.spawn = p
    Log.Info(string.format("Player ready at (%.2f, %.2f, %.2f)", p.x, p.y, p.z))

    -- P5. PG.damage is what an enemy subtracts when a round lands, and it lives in PG because
    -- there is no way to call into another entity's script instance; PG.score is banked here and
    -- spent at the upgrade station. Both are seeded once, by whoever gets here first.
    PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }
    PG.damage = 0
    -- Set by the P5 gate after its fight (Player:ProbeTurn). Cleared here because PG lives in the
    -- shared Lua VM, which the editor keeps across Play and Stop: a gate run left it set, and the
    -- next ordinary Play would have had its enemies vanish on the first frame.
    PG.gateClear = false
    PG.score = PG.score or 0
    self.health = self.maxHealth
    self:PushUI()
    self:StartLoadout()
    self:StartLeaderboard()

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

    -- A gun on the rack, or one tossed there by a swap: "<Gun> Pickup".
    local gun = name:match("^(%w+) Pickup$")
    if gun and GUNS[gun] then
        self:PickUpGun(gun, other)
        return
    end

    if name == "Heal Spot" or name == "Upgrade Station" then
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
        self:EndLife("down")
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
    self.upgrades = self.upgrades + 1
    PG.damage = (self.gun and GUN_DAMAGE[self.gun] or 0) + UPGRADE_DAMAGE * self.upgrades
    Log.Info(string.format("UPGRADE bought: projectile damage -> %d, score left %d",
        PG.damage, PG.score))
end

-- ---- The online leaderboard (ONLINE.md O3) ---------------------------------------------------
--
-- The board ranks KILLS IN ONE LIFE: from spawning, or getting up again, until going down or dying.
-- Not PG.score, which is a currency the upgrade station spends - ranking it would rank thrift. A
-- life is submitted when it ends, because there is no "submit when play stops": a request sent from
-- OnDestroy is cancelled along with the instance (docs/engine/online.md). Going down and dying both
-- end a life; the kills of a life that ends with none are not sent.
--
-- The HUD block is written through RmlUi's Lua API (inner_rml), not the data model, whose two
-- variables are declared in C++ and cannot be added to from the game branch. Every backend call
-- waits for sign-in on its own, and a failure only changes what the block says: the game plays on.
local BOARD = "proving-ground"
local BOARD_ROWS = 5

local function EscapeRml(text)
    return (tostring(text):gsub("&", "&amp;"):gsub("<", "&lt;"):gsub(">", "&gt;"):gsub('"', "&quot;"))
end

function Player:StartLeaderboard()
    self.lifeStartKills = PG.kills or 0
    self.board = { rows = nil, best = nil, rank = nil, standingKnown = false, error = nil, lastLife = nil }
    self:DrawLeaderboard()
    self:FetchLeaderboard()
end

function Player:FetchLeaderboard()
    Backend.GetLeaderboard(self.entity, BOARD, BOARD_ROWS, function(ok, rows)
        if ok then
            self.board.rows, self.board.error = rows, nil
        else
            self.board.error = rows
            Log.Warn("LEADERBOARD unavailable: " .. tostring(rows))
        end
        self:DrawLeaderboard()
    end)
    Backend.GetMyStanding(self.entity, BOARD, function(ok, standing)
        if ok then
            self.board.best, self.board.rank, self.board.standingKnown = standing.best, standing.rank, true
        end
        self:DrawLeaderboard()
    end)
end

function Player:EndLife(how)
    local kills = (PG.kills or 0) - (self.lifeStartKills or 0)
    self.lifeStartKills = PG.kills or 0
    if kills <= 0 or not self.board then
        return
    end

    self.board.lastLife = kills
    Log.Info(string.format("LIFE OVER (%s): %d kill%s - submitting to the leaderboard", how, kills,
        kills == 1 and "" or "s"))
    Backend.SubmitScore(self.entity, BOARD, kills, function(ok, result)
        if ok then
            Log.Info(string.format("SCORE submitted: %d -> best %d, rank %d%s", kills, result.best, result.rank,
                result.replayed and " (a replay)" or ""))
            self.board.best, self.board.rank, self.board.standingKnown = result.best, result.rank, true
            self:FetchLeaderboard()   -- the top rows may have moved
        else
            Log.Warn(string.format("SCORE not submitted (%s) - playing on", tostring(result)))
            self.board.error = result
        end
        self:DrawLeaderboard()
    end)
    self:DrawLeaderboard()
end

function Player:DrawLeaderboard()
    local context = rmlui and rmlui.contexts["main"]
    local document = context and context.documents["hud"]
    local element = document and document:GetElementById("leaderboard")
    if not element or not self.board then
        -- No HUD (a scene played without one) is fine; a HUD without the block is a stale hud.rml.
        if document and not element and not self.boardWarned then
            self.boardWarned = true
            Log.Warn("LEADERBOARD: the HUD has no #leaderboard element - is hud.rml up to date?")
        end
        return
    end

    local b = self.board
    local out = { '<div class="lb-title">KILLS IN ONE LIFE</div>' }
    if b.rows then
        if #b.rows == 0 then
            out[#out + 1] = '<div class="lb-note">no scores yet</div>'
        end
        for _, row in ipairs(b.rows) do
            out[#out + 1] = string.format('<div class="lb-row%s">%d. %s - %d</div>',
                row.isMe and " lb-me" or "", row.rank, EscapeRml(row.name), row.score)
        end
    elseif not b.error then
        out[#out + 1] = '<div class="lb-note">loading...</div>'
    end

    if b.error then
        out[#out + 1] = '<div class="lb-note">offline</div>'
    elseif b.best then
        out[#out + 1] = string.format('<div class="lb-note">your best %d (#%d)</div>', b.best, b.rank)
    elseif b.standingKnown then
        out[#out + 1] = '<div class="lb-note">your best: none yet</div>'
    end
    if b.lastLife then
        out[#out + 1] = string.format('<div class="lb-note">last life %d</div>', b.lastLife)
    end

    element.inner_rml = table.concat(out)

    -- Logged when it changes, as plain text: the HUD is the thing nobody reads in a log, and a
    -- block that silently stayed on "loading..." would look exactly like one that worked.
    local shown = table.concat(out, " | "):gsub("<[^>]*>", "")
    if shown ~= self.boardShown then
        self.boardShown = shown
        Log.Info("LEADERBOARD shows: " .. shown)
    end
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
        if not self.losgate and self.probeWp == 1 then
            -- The fight ends on what it is for - hurt, and enough score banked for leg 4's
            -- upgrade - not on a timer. "Hurt, then stand 14 s" banked 2 kills only while enough
            -- enemies happened to cross the line of fire in time; when they moved more freely it
            -- banked 1, and the upgrade leg was refused. 60 s is the ceiling.
            if (self.hits < 3 or PG.score < self.upgradeCost) and self.t < 60.0 then
                return 0.0
            end
            self.probeHold = 0.0
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
                -- The fight is leg 1 only. Every later P5 leg measures a trigger - heal, pickup,
                -- upgrade - and none of them needs an enemy, but enemies left alive kept charging
                -- and shoving the player off a route that has no pathfinding: one run in two ended
                -- jammed behind an obstacle. Freezing them would not do - seven bodies standing
                -- where the fight was are walls to the same route - so the gate removes them.
                if self.p5gate and self.probeWp == 1 then
                    PG.gateClear = true
                    Log.Info(string.format("%s fight over: enemies removed for the trigger legs (kills=%d)",
                        tag, PG.kills))
                end
                self.probeWp = self.probeWp + 1
            end
        end
        return 0.0
    end

    local dx, dz = target[1] - p.x, target[2] - p.z
    local dist = math.sqrt(dx * dx + dz * dz)

    -- Leg 1 is the fight, and the fight comes to the player: charging enemies can shove it off the
    -- spot before it is within 0.35 m, and then it never arrived and the leg never ended - one run
    -- fired for 165 s straight. Hurt, or out of time, is what the leg is waiting for anyway, so
    -- either counts as arriving, wherever it was shoved to.
    local fightOver = self.p5gate and self.probeWp == 1 and (self.hits >= 3 or self.t >= 60.0)

    if dist < 0.35 or fightOver then
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
    -- The capturing click is not an attack, for as long as it is held: a click lasts several
    -- frames, so skipping only the frame that captured would still fire on the next one. LMB arms
    -- once it has been seen up.
    local lmbDown = Input.IsMouseButtonPressed(Mouse.ButtonLeft)
    if not self.looking and lmbDown then
        Input.SetCursorMode(Cursor.Locked)
        self.looking = true
        self.lmbArmed = false
    elseif self.looking and Input.IsKeyPressed(Key.Escape) then
        Input.SetCursorMode(Cursor.Normal)
        self.looking = false
    end

    if not lmbDown then
        self.lmbArmed = true
    end
    local attackHeld = self.looking and self.lmbArmed and lmbDown
    -- The press, for the semi-automatic pistol: tracked every frame, whatever is in hand, so a
    -- click straight after switching to it is not mistaken for a button still held.
    local attackPressed = attackHeld and not self.triggerHeld
    self.triggerHeld = attackHeld

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
    -- shooter uses: the first click is "I am playing now", the rest are shots. Until 2026-10-01
    -- the capturing click fired too, because `looking` was already true by the time it got here;
    -- attackHeld (above) is what keeps it out now.
    self.fireCooldown = self.fireCooldown - ts
    self.sinceShot = self.sinceShot + ts
    if self.sinceShot > SPREAD_RECOVER_DELAY then
        self.heat = math.max(0.0, self.heat - ts / SPREAD_RECOVER)
    end
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
    elseif self.p5gate and not self.probeDone and self.probeWp == 1 then
        -- Only on the first leg, and at the gun's own rate rather than every frame: this is meant
        -- to look like someone shooting back.
        self:PullTrigger(true, false)
    elseif self.equipped == "Gun" and self.gun then
        self:PullTrigger(attackHeld, attackPressed)
    end
    -- Time spent not firing is not banked as a burst: the carry in PullTrigger is only the part
    -- of a frame that a shot overran.
    if self.fireCooldown < 0.0 then
        self.fireCooldown = 0.0
    end

    -- ---- weapon slots and melee ----
    -- The same button attacks with whatever is in hand. Not on the click that captures the
    -- cursor (attackHeld), which for a melee weapon would commit the player to a three-second
    -- swing.
    if not self.attack then
        if Input.IsKeyPressed(Key.D1) then self:Equip("Gun")
        elseif Input.IsKeyPressed(Key.D2) then self:Equip("Knife") end
    end
    self:Melee(ts, attackHeld)

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

    self:FallCheck(ts)

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
    -- An attack roots the player too: the attack clips are in place, so moving during one slides
    -- the feet, and the swing's lane was aimed from where it started.
    local speed = (self.downed or self.attack) and 0.0 or (self.speed * self.probeSlow)
    self.entity:SetLinearVelocity(Vec3(ix * speed, v.y, iz * speed))

    self:Tick(ts)
    self:PushPending()
    self:Diagnose(ts)
end

-- Health, score and the HUD, once a frame.
function Player:Tick(ts)
    -- A gun equipped last frame has its socket now (Equip): full size.
    if self.reveal and self.reveal.ready then
        local sc = self.reveal.scale
        self.reveal.entity:SetScale(Vec3(sc, sc, sc))
        if self.body and self.reveal.twoHanded then
            self.body:SetHandIKEnabled(true)
            self.body:SetHandIKWeight(1.0, 1.0)
            self.body:SetAimLock(1.0)
        end
        self.reveal = nil
    elseif self.reveal then
        self.reveal.ready = true
    end
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
    self:Animate(ts)
    self:PushUI()
end

-- The player's own three clips, chosen from the capsule's measured horizontal speed rather
-- than from the input. Easing toward a probe point runs at 1.2 m/s (Player:Route), and driving
-- this off "is a key held" would snap between idle and a sprint through the whole approach.
--
-- The legs and the torso are not the same facing. The clips are all forward strides, so the
-- body (meshYaw) turns toward the velocity and the forward clip stays the right one.
-- AimOffsetComponent twists the spine on top of that: yaw is wrap(camera yaw - meshYaw), pitch is
-- the elevation from the chest to AimPoint. The rifle is socketed to the chest (Spine), and Body's
-- TwoHandIKComponent turns it onto that same aim (AimLock 1) and solves both hands onto its Grip
-- and Support markers, so the barrel follows these two angles whatever the clip's chest is doing.
--
-- The torso takes at most TORSO_TWIST of that. Past it, the legs turn off the velocity toward
-- the aim just far enough that the torso needs no more, so a pure A/D strafe runs the legs 30
-- degrees off their heading instead of asking the spine for 90 - the point where a spine-only
-- twist stops reading as a person. Moving well behind the aim switches to the backpedal: the
-- body turns to the camera yaw and plays Walk_Backward_While_Shooting.
local AIM_HOLD = 0.8
local TORSO_TWIST = math.rad(60)
-- Hysteresis on the backpedal switch, so no heading sits on a boundary. The first version
-- switched at exactly 90 degrees, which is where a pure strafe lands, and float rounding of the
-- velocity heading chose the branch: at a camera yaw of 0.7 the twist came out one ulp past pi/2
-- on every frame, and the strafe never engaged.
local BACKPEDAL_ENTER = math.rad(115)
local BACKPEDAL_EXIT = math.rad(100)
-- Seconds for the aim offset to fade fully in when aiming starts, or out when it stops. A linear
-- ramp through smoothstep, so the bend starts and ends at rest; an exponential ease moves fastest
-- on its first frame, and a probe measured a 0.255 rad step there out of a 60 degree twist. Only
-- the transitions ease; while aiming, the chest tracks the mouse directly.
local AIM_BLEND_TIME = 0.25
-- Walk_Backward_While_Shooting's authored root speed, m/s.
local BACKPEDAL_CLIP_SPEED = 0.96
local CHEST_HEIGHT = 0.5

local function WrapAngle(a)
    return (a + math.pi) % (2 * math.pi) - math.pi
end

-- One-handed locomotion, for the knife and the axe. The rifle set is wrong for them twice over: its
-- poses hold the right hand across the chest, where the axe sits inside the body on every frame
-- (carry_check.py: 33 of 33 sampled frames of the forward walk), and the free hand holds an
-- invisible handguard. These three put the weapon hand at the side and clear the body on every
-- sampled frame with both weapons:
--   Axe_Breathe_and_Look_Around  installed with the melee set; an idle with an axe in the hand
--   Casual_Walk, run_fast_4      the Soldier's own walk and run, retargeted onto this skeleton
-- Each plays at the character's speed over the speed its own stride covers, so the feet match the
-- ground. The run is measured: the Soldier's download kept its root motion, 3.64 m in 0.67 s,
-- 5.46 m/s, and 5.6 on this rig's 2.6% longer legs - so at the full 6 m/s it plays at 1.07, where
-- Run_and_Shoot needs 1.7 and still slides. The walk was authored in place, so its figure is the
-- planted foot's speed, scaled by 1.34: that estimate read the run at 4.19 against the measured 5.6.
local MELEE_WALK_GROUND = 0.90
local MELEE_RUN_GROUND = 5.6
-- Below this the walk, above it the run. The casual walk is slow, so a walk faster than about
-- 1.8 m/s (2x) slides; the band between is only crossed while accelerating or easing to a stop.
local MELEE_RUN_FROM = 2.0

-- The knife has two grips. The fitted one (holds.json, the socket ProvingGround.ganymede authors)
-- serves the attack, the idle and the walk, where the blade already points up. In run_fast_4 the fist
-- is turned so that grip points the blade back at the holder, so the run swaps to a reverse grip
-- fitted for it (knife_carry_fit.py): the handle stays in the palm, pivoting on its centroid, and the
-- tip sits 53-62 deg up and leans away from the body on every frame, with no body contact. The swap
-- lands on the frame the run clip starts or stops, which is already a hard cut.
local KNIFE_GRIPS = {
    hold = { Vec3(-0.0866638, 0.000847120, -0.0217498), Vec3(0.270508, -0.760665, 1.309071) },
    run  = { Vec3(-0.1307996, 0.148501302, 0.0406194), Vec3(0.550934, -0.227784, -0.859616) },
}

function Player:KnifeGrip(name)
    local knife = self.weapons and self.weapons.Knife
    if not knife or self.knifeGrip == name then
        return
    end
    self.knifeGrip = name
    local g = KNIFE_GRIPS[name]
    -- The component already exists, so this lands this frame (BoneAttachmentSystem runs later).
    knife.entity:AttachToBone(self.body, "RightHand", g[1], g[2])
end

function Player:MeleeLocomotion(speed)
    if speed < 0.5 then
        return "Axe_Breathe_and_Look_Around", 1.0
    elseif speed < MELEE_RUN_FROM then
        return "Casual_Walk", math.max(0.6, math.min(2.0, speed / MELEE_WALK_GROUND))
    end
    return "run_fast_4", math.max(0.6, math.min(2.0, speed / MELEE_RUN_GROUND))
end

function Player:Animate(ts)
    if not self.body then
        return
    end

    if self.attack then
        self:AnimateAttack(ts)
        return
    end

    local v = self.entity:GetLinearVelocity()
    local speed = math.sqrt(v.x * v.x + v.z * v.z)

    -- Airborne or downed is not walking, whatever the horizontal velocity says. Same test
    -- Player:Step uses to keep footsteps off a falling character.
    if self.downed or not self.entity:IsGrounded() then
        speed = 0.0
    end

    self.aimHold = math.max(0.0, self.aimHold - ts)
    local aiming = not self.downed
        and (self.aimHold > 0.0 or (self.looking and speed < 0.5))

    -- Same convention as Enemy:Chase: -Z is forward at yaw 0.
    local moving = speed > 0.5
    local velYaw = math.atan(-v.x, -v.z)
    -- The mode follows the velocity heading, not the eased mesh yaw, or a turn in progress
    -- would flip between "strafe" and "turn the whole body" every frame.
    local twist = WrapAngle(self.yaw - velYaw)
    if not (aiming and moving) then
        self.backpedal = false
    elseif self.backpedal then
        self.backpedal = math.abs(twist) > BACKPEDAL_EXIT
    else
        self.backpedal = math.abs(twist) > BACKPEDAL_ENTER
    end

    local target, rate = nil, 14.0
    if aiming and moving and not self.backpedal then
        -- Legs on the velocity, turned toward the aim only as far as keeps the torso within
        -- TORSO_TWIST. Inside it this is exactly velYaw. The offset carries the rest of the
        -- aim, so this can stay on the slow turn.
        target = self.yaw - math.max(-TORSO_TWIST, math.min(TORSO_TWIST, twist))
    elseif aiming then
        target, rate = self.yaw, 20.0
    elseif moving then
        target = velYaw
    end

    if target then
        self.meshYaw = self.meshYaw or self.yaw
        -- Shortest way round, then eased, so tapping S turns through 180 degrees over a couple
        -- of frames instead of popping.
        local diff = WrapAngle(target - self.meshYaw)
        self.meshYaw = self.meshYaw + diff * math.min(1.0, ts * rate)
    end

    -- Moving against the way the body faces. Outside the backpedal the legs are within
    -- TORSO_TWIST of the aim, so this is only the backpedal branch above.
    local backwards = false
    if moving and self.meshYaw then
        local facing = -math.sin(self.meshYaw) * v.x - math.cos(self.meshYaw) * v.z
        backwards = facing < -0.3 * speed
    end

    -- A1: the weapon-carry set. Names are Meshy's library entries baked into the glb, and the
    -- engine resolves clips by name off the mesh asset, so renaming means re-exporting.
    local clip, animSpeed
    -- One-handed: the knife, and the pistol, which is held the same way.
    local melee = self.equipped ~= "Gun" or (self.gun and not GUNS[self.gun][5])
    if melee then
        clip, animSpeed = self:MeleeLocomotion(speed)
        if self.equipped == "Knife" then
            self:KnifeGrip(clip == "run_fast_4" and "run" or "hold")
        end
    elseif speed < 0.5 then
        clip, animSpeed = "Lower_Weapon_Look_Raise", 1.0
    elseif speed < 4.0 then
        clip, animSpeed = "Walk_Forward_While_Shooting", 1.0
    else
        -- Run_and_Shoot was authored at 2.57 m/s (its own root motion, measured before that
        -- motion was stripped). Against this character's 6 m/s, matching the feet to the ground
        -- exactly would need 2.34, which turns a tactical jog into a sprint on fast-forward.
        -- 1.7 splits the difference: the feet run at ~4.4 m/s, so some slide remains.
        --
        -- The principled fix is the other direction - 6 m/s is 21.6 km/h, which is sprint pace
        -- for someone carrying a rifle. Dropping `speed` to ~4.0 would let this play at 1.55
        -- with almost no slide. It also moves every P1-P7 gate number, so it is not done here.
        clip, animSpeed = "Run_and_Shoot", 1.7
    end

    -- Library 233, retargeted onto this skeleton (PROVING_GROUND.md, "Two more rifle clips"). Same
    -- grip family as the forward walk, so the rifle socket fits it; it replaced the forward clip
    -- played at negative speed, the moon-walk.
    --
    -- Authored at 0.96 m/s (1.22 m in 1.27 s, measured before the root motion was stripped). The
    -- backpedal runs at the full move speed, 6 m/s, which no backpedal clip matches: 6.2x would be
    -- a blur of a stride. Capped at 2x, so the feet cover ~1.9 m/s and slide the rest - the same
    -- kind of compromise as Run_and_Shoot above, only larger. The real fix is a slower backpedal.
    -- Not with a melee weapon: there is no one-handed backpedal, and nothing aims a knife, so the
    -- legs face the velocity and `backwards` can only be left over from a shot before the switch.
    if backwards and not melee then
        clip = "Walk_Backward_While_Shooting"
        animSpeed = math.max(0.5, math.min(2.0, speed / BACKPEDAL_CLIP_SPEED))
    end

    self.body:PlayAnimation(clip)
    self.body:SetAnimationSpeed(animSpeed)

    -- Body is a child of Yaw, so this has to be written in Yaw's frame. The extra pi is the
    -- rig's own +Z facing, the same correction Enemy.lua carries.
    local localYaw = (self.meshYaw or self.yaw) - self.yaw + math.pi
    self.body:SetRotation(Vec3(0, localYaw, 0))

    -- After the ease, so the torso tracks the camera while the legs are still catching the
    -- velocity. Not aiming fades the bend out: running with the cursor captured but not
    -- shooting already faces the velocity, and a pitch on that heading would bend the spine
    -- toward a point the chest is not turned to.
    --
    -- Held and faded, never written as zero. The first version wrote (0, 0) the frame aiming
    -- ended, and a probe logged the chest twist going from 45 degrees to 0 in one 22 ms frame.
    -- The weight fades rather than the angles easing, so the chest stays on the crosshair while
    -- aiming. The inputs update only while aiming, and pitch only while there is an aim point
    -- (Escape during AIM_HOLD makes AimPoint nil), so a fade never chases a camera the player
    -- has stopped aiming with.
    if aiming and self.meshYaw then
        self.aimYawHeld = WrapAngle(self.yaw - self.meshYaw)
        self.aimPitchHeld = self:ChestAimPitch() or self.aimPitchHeld
    end
    local step = ts / AIM_BLEND_TIME
    if aiming then
        self.aimBlend = math.min(1.0, self.aimBlend + step)
    else
        self.aimBlend = math.max(0.0, self.aimBlend - step)
    end
    local weight = self.aimBlend * self.aimBlend * (3.0 - 2.0 * self.aimBlend)
    self.body:SetAimOffset(self.aimPitchHeld * weight, self.aimYawHeld * weight)

    self.clip = clip
end

-- Show what is in hand and shrink the rest, and hand MeleeAttacker (on Body) the weapon to trace.
--
-- Shrunk, because a script cannot hide an entity (HIDDEN_SCALE). And only the gun in hand is
-- socketed: hand IK holds the first socketed child of Body, so another gun that kept its socket
-- ahead of it would get the hands. A gun put away is detached; the IK skips its socket from the
-- frame DetachFromBone clears its joint. The knife keeps its socket throughout and is the last
-- child, so it never comes before a gun.
--
-- A two-handed gun takes both hands and the aim lock through the IK. The knife and the pistol are
-- held in the right hand by the clip, so the IK is switched off.
function Player:Equip(slot, force)
    if slot == "Gun" and not self.gun then
        return
    end
    if slot == self.equipped and not force then
        return
    end
    self.equipped = slot
    self.knifeGrip = nil

    local shown = (slot == "Gun") and self.gun or slot
    self.reveal = nil
    for name, w in pairs(self.weapons or {}) do
        local inHand = name == shown
        local gun = GUNS[name]
        if gun then
            -- Guns are authored shrunk, so their size comes from GUNS. Even the one going in hand
            -- stays shrunk this frame: adding its socket is structural and lands next frame, and
            -- until then it would be drawn at Body's origin. Player:Tick reveals it.
            local sc = gun[1] * HIDDEN_SCALE
            w.entity:SetScale(Vec3(sc, sc, sc))
            if inHand then
                w.entity:AttachToBone(self.body, gun[2], gun[3], gun[4])
                w.socketed, self.reveal = true, { entity = w.entity, scale = gun[1] }
            elseif w.socketed then
                w.entity:DetachFromBone()
                w.socketed = false
            end
        else
            local k = inHand and 1.0 or HIDDEN_SCALE
            w.entity:SetScale(Vec3(w.scale.x * k, w.scale.y * k, w.scale.z * k))
        end
    end

    -- The IK is off unless a two-handed gun is in hand, and it comes on with that gun's socket
    -- (Player:Tick): on now, it would find no socketed gun for a frame. Weight 0 is not off - it
    -- still finds the weapon, and on the knife or the pistol warns that it sits in the right arm
    -- with no left marker (SetHandIKEnabled, docs/engine/scripting.md).
    if self.reveal then
        self.reveal.twoHanded = GUNS[self.gun][5]
    end
    self.muzzle = nil
    if slot == "Gun" then
        self.muzzle = self.weapons[self.gun] and self.weapons[self.gun].entity:GetChildByName("Muzzle")
    end
    if self.body then
        self.body:SetHandIKEnabled(false)
        PG.meleeConfig = PG.meleeConfig or {}
        -- "None" has no damage windows, so MeleeAttacker traces nothing while a gun is out.
        -- `ignore` is the capsule: the Body is two levels under it, too deep for MeleeAttacker's
        -- own self test.
        PG.meleeConfig[self.body:GetUUID()] = { weapon = slot == "Gun" and "None" or slot,
                                                ignore = self.entity:GetUUID() }
    end
    Log.Info(string.format("EQUIP %s", shown))
end

-- The knife in hand, and the start gun if there is one: what the player begins with, and what a
-- respawn gives back. Whatever gun was carried is gone.
function Player:StartLoadout()
    self.gun = nil
    PG.damage = 0
    self:Equip("Knife", true)
    local start = (self.autofire or self.autopilot or self.losgate or self.p5gate) and "Rifle" or self.startGun
    if start ~= "" then
        self:TakeGun(start)
    end
end

-- Off the edge of the world: airborne beyond the floor, or below it. FALL_DEATH_TIME of that and
-- the player dies. Not "below some depth": falling speed depends on where you went over, and three
-- seconds is the same wait whatever the drop.
function Player:FallCheck(ts)
    local p = self.entity:GetTranslation()
    local off = math.abs(p.x) > WORLD_EDGE or math.abs(p.z) > WORLD_EDGE or p.y < self.startY - 2.0
    if off and not self.entity:IsGrounded() then
        self.fallFor = self.fallFor + ts
        if self.fallFor >= FALL_DEATH_TIME then
            self:Die("fell off the edge of the world")
        end
    else
        self.fallFor = 0.0
    end
end

-- Death: back to the spawn point with full health and the starting loadout. The score and the
-- upgrades bought are kept. Teleport (master 8aad1b3) is the only way to put a character
-- anywhere; its transform is written from the controller every step. The fall's velocity goes
-- too, or the respawned player would hit the floor at the speed it was falling.
function Player:Die(reason)
    local p = self.entity:GetTranslation()
    self.deaths = self.deaths + 1
    self:EndLife("died")
    Log.Info(string.format("PLAYER DIED (%d): %s at (%.1f, %.1f, %.1f) - respawning at (%.1f, %.1f, %.1f)",
        self.deaths, reason, p.x, p.y, p.z, self.spawn.x, self.spawn.y, self.spawn.z))

    if self.attack and self.body then
        self.body:SetAnimationLooping(true)
    end
    self.attack = nil
    self.entity:Teleport(self.spawn)
    self.entity:SetLinearVelocity(Vec3(0, 0, 0))
    self.fallFor = 0.0
    self.health = self.maxHealth
    self.downed, self.downFor = false, 0.0
    self.pendingHurt, self.hurtCooldown = 0, 0.0
    self:StartLoadout()
    self:PushUI()
end

-- Put a gun on slot 1 and in hand. Its damage is its rung of the ladder plus the upgrades bought.
function Player:TakeGun(gun)
    self.gun = gun
    self.heat = 0.0
    PG.damage = GUN_DAMAGE[gun] + UPGRADE_DAMAGE * self.upgrades
    self:Equip("Gun", true)
end

-- Walking into "<Gun> Pickup". The same gun as the one on slot 1 does nothing, and the pickup stays.
-- A different one goes on slot 1 and in hand, its pickup leaves the map, and the gun it replaces is
-- tossed TOSS_DISTANCE behind the player as a pickup of its own, so it can be taken back.
--
-- The player decides and destroys the pickup itself: both scripts get the contact, in an order
-- nothing guarantees, and only this side knows what is on slot 1.
function Player:PickUpGun(gun, pickup)
    if self.gun == gun then
        return
    end
    local old = self.gun
    self:TakeGun(gun)
    pickup:Destroy()
    self.gunsTaken = self.gunsTaken + 1
    Audio.PlayOneShot("audio/chime.wav", nil, 0.8)

    local tossed = ""
    if old then
        local p = self.entity:GetTranslation()
        local yaw = self.meshYaw or self.yaw
        -- Behind is +Z at yaw 0 (forward is -Z). The pickup's root sits 1 m over the floor, and
        -- the capsule's centre 0.95 m over it.
        local at = Vec3(p.x + math.sin(yaw) * TOSS_DISTANCE, p.y + 0.05, p.z + math.cos(yaw) * TOSS_DISTANCE)
        if Scene.Spawn("prefabs/pickups/" .. old .. "Pickup.gprefab", at) then
            tossed = string.format(", tossed %s to (%.1f, %.1f)", old, at.x, at.z)
        else
            Log.Warn(string.format("Player: could not toss %s - the spawn was refused", old))
        end
    end
    Log.Info(string.format("PICKUP %s, %d damage a round%s", gun, PG.damage, tossed))
end

-- The melee attack: LMB with the knife or axe in hand.
--
-- The lockout is the attack clip's whole length, then meleeCooldown on top. A fixed interval
-- alone cannot be right for every attack: the thrust is 3.0 s and the chop 2.47 s, and anything
-- shorter than the clip in hand would have to cut the swing off mid-air - there is no crossfade,
-- so that is a pop - while anything longer leaves the shorter attack standing idle. It also cannot
-- simply restart the clip: PlayAnimation on the clip already current does not rewind it, and
-- MeleeAttacker times its damage window from the frame the clip changes, so every attack has to
-- begin with a clip change. Ending on a frame of locomotion guarantees one.
--
-- The lengths are the clips' own and come from MeleeAttacker's table, so a sixth character with
-- a different-length attack needs one table entry, not a new cooldown.
--
-- Each attack lands off-centre (the chop 40 degrees left, the thrust 45 right), so the body turns
-- by the lane's yaw to put the strike where the crosshair is, held for the whole attack.
function Player:Melee(ts, want)
    local a = self.attack
    if a then
        a.t = a.t + ts
        if a.t >= a.length or self.downed then
            self.attack = nil
            self.meleeReady = self.meleeCooldown
            if self.body then self.body:SetAnimationLooping(true) end
        end
        return
    end

    self.meleeReady = math.max(0.0, self.meleeReady - ts)
    if not want or self.equipped == "Gun" or self.downed or self.meleeReady > 0.0 or not self.body then
        return
    end

    local clip = MELEE_ATTACK[self.equipped]
    local w = PG.meleeWindows and PG.meleeWindows[self.equipped] and PG.meleeWindows[self.equipped][clip]
    if not w then
        if not self.warnedMelee then
            self.warnedMelee = true
            Log.Warn(string.format("Player: no melee window for %s %s - is MeleeAttacker on Body?",
                self.equipped, clip))
        end
        return
    end

    self.attack = { clip = clip, t = 0.0, length = w[3], yaw = self.yaw - math.rad(w[4]) }
    self.attacks = self.attacks + 1
    -- One play-through, not a loop: a frame late ending the attack would otherwise start a
    -- second lap, and MeleeAttacker would count it as a second swing.
    self.body:SetAnimationLooping(false)
    Log.Info(string.format("MELEE %d: %s %s, turned %.0f deg into its lane, %.2f s + %.2f s",
        self.attacks, self.equipped, clip, w[4], w[3], self.meleeCooldown))
end

-- The pose during an attack: the attack clip at its authored speed (MeleeAttacker's windows are in
-- clip seconds), the legs turned into its lane, and the aim offset faded out, because the lanes
-- were measured on the clip without a spine twist on top.
function Player:AnimateAttack(ts)
    local a = self.attack
    self:KnifeGrip("hold")
    self.meshYaw = self.meshYaw or self.yaw
    self.meshYaw = self.meshYaw + WrapAngle(a.yaw - self.meshYaw) * math.min(1.0, ts * 20.0)
    self.body:PlayAnimation(a.clip)
    self.body:SetAnimationSpeed(1.0)
    self.body:SetRotation(Vec3(0, self.meshYaw - self.yaw + math.pi, 0))

    self.aimBlend = math.max(0.0, self.aimBlend - ts / AIM_BLEND_TIME)
    local weight = self.aimBlend * self.aimBlend * (3.0 - 2.0 * self.aimBlend)
    self.body:SetAimOffset(self.aimPitchHeld * weight, self.aimYawHeld * weight)
    self.clip = a.clip
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

-- The world point under the crosshair, or nil when there is no mouse aim to honour.
--
-- The crosshair is the centre of the camera, and the camera sits behind and above the capsule
-- (Yaw-local (0, 1.6, 5) in the scene). A round fired along the camera's forward from the chest
-- would run parallel to the crosshair ray and miss it by that offset at every range. So: cast
-- the camera ray, take what it hits, and aim the round from the muzzle *at that point* - the
-- usual third-person convergence. Past 200 m nothing is hit and the far point stands in.
--
-- Computed by hand because Lua can only read local transforms. The capsule never rotates, so
-- Yaw's translation is already a world offset; the camera's is in Yaw's frame and needs the yaw.
function Player:AimPoint()
    if not self.looking or not self.yawEntity or not self.cameraEntity then
        return nil
    end

    local sinY, cosY = math.sin(self.yaw), math.cos(self.yaw)
    local c = self.cameraEntity:GetTranslation()
    local origin = self.entity:GetTranslation() + self.yawEntity:GetTranslation()
        + Vec3(c.x * cosY + c.z * sinY, c.y, -c.x * sinY + c.z * cosY)

    -- Ry(yaw) * Rx(pitch) * (0, 0, -1): positive pitch looks up.
    local cosP = math.cos(self.pitch)
    local dir = Vec3(-sinY * cosP, math.sin(self.pitch), -cosY * cosP)

    -- Start the cast level with the player, not at the lens. Anything between the camera and the
    -- character - an enemy that has run round behind, a doorframe - is behind the muzzle and
    -- cannot be shot, and the probe run caught exactly that: an Enemy at 4 m on the camera ray
    -- put the aim point behind the gun. The crosshair still sits on the same ray.
    local along = (self.entity:GetTranslation() - origin):Dot(dir)
    if along > 0.0 then
        origin = origin + dir * along
    end

    -- Sensors (heal spots, pickups) stop a ray like a wall does, and rounds fly through them
    -- (Projectile:OnCollisionEnter's passThrough). Converging on a sensor's surface would bend
    -- the shot off the crosshair, so step past any of them, a few at most.
    local from, left = origin, 200.0
    for _ = 1, 4 do
        local hit = Physics.Raycast(from, dir, left, self.entity)
        if not hit then
            break
        end
        local name = hit.entity and hit.entity:GetName()
        if not (name and PG and PG.passThrough and PG.passThrough[name]) then
            return hit.point
        end
        from = hit.point + dir * 0.01
        left = left - hit.distance - 0.01
    end
    return origin + dir * 200.0
end

-- Elevation from the chest to the aim point, or nil when there is nothing to aim at.
--
-- Not the camera pitch. The lens is 1.6 m above the capsule and 5 m behind it, so at short
-- range its pitch and the gun's pitch disagree by the parallax AimPoint already exists to
-- remove. The chest is the same point Fire uses for the fallback spawn (CHEST_HEIGHT above
-- the capsule origin), not the muzzle: the muzzle moves with this pitch, and feeding it
-- back would be the closed loop the offset deliberately is not.
function Player:ChestAimPitch()
    local aim = self:AimPoint()
    if not aim then
        return nil
    end

    local p = self.entity:GetTranslation()
    local to = aim - Vec3(p.x, p.y + CHEST_HEIGHT, p.z)
    local horiz = math.sqrt(to.x * to.x + to.z * to.z)
    return math.atan(to.y, horiz)
end

-- Where the round leaves the gun, or nil when the gun is not in a position to have fired it.
--
-- The barrel no longer moves with the clip: the two-hand IK aim lock holds it on the aim offset's
-- pitch and yaw (to 1e-4 degrees, measured), and the idle no longer lowers it. The ~35 degree
-- check stays anyway, as a guard for what the lock does not cover: the aim offset fading in and
-- out over AIM_BLEND_TIME (the barrel follows the faded angles, not the aim point), and parallax
-- at close range (the lock aims along the chest's line to the aim point, and the muzzle is half
-- a metre off the chest). Two ways a shot from the gun goes wrong, both caught by a probe run:
--
--   - A barrel behind or beside the player: the round flies through the player's own capsule,
--     and Projectile:OnCollisionEnter counts that as a hit and despawns it. So the barrel has to
--     be clear of the capsule (0.35 radius + the round's 0.075 + slack) and ahead along the yaw.
--   - A barrel pointing at the floor: the round leaves a lowered gun and climbs to the crosshair.
--     So the barrel's own forward has to be within ~35 degrees of the line to the aim point.
--
-- Otherwise the chest point stands in, as it did before there was a gun. GetWorldPosition is
-- last frame's, so at a 6 m/s run the round starts ~0.1 m behind the barrel: inside the flash,
-- and not worth a per-frame velocity correction.
function Player:BarrelPoint(p, fx, fz, aim)
    if not self.muzzle then
        return nil
    end
    local b = self.muzzle:GetWorldPosition()
    local hx, hz = b.x - p.x, b.z - p.z
    if hx * hx + hz * hz < 0.5 * 0.5 or hx * fx + hz * fz < 0.25 then
        return nil
    end
    local toAim = (aim - b):Normalized()
    if toAim:Dot(self.muzzle:GetWorldForward()) < 0.82 then
        return nil
    end
    return b
end

-- One frame of the trigger. Automatic guns fire while it is held; the pistol fires once per press.
--
-- The cooldown carries its overrun instead of being reset to the full interval. Reset, every shot
-- would wait for the first whole frame past its interval, and the rate would round down to the
-- frame rate: 15 rounds/s is 4 frames at 60 fps, and a reset turns that into one shot every 5,
-- which is 12. Carried, the shots land on the frames nearest the true schedule.
function Player:PullTrigger(held, pressed)
    local f = GUN_FIRE[self.gun or "Rifle"]
    if self.fireCooldown > 0.0 or not (f.auto and held or pressed) then
        return
    end
    self.fireCooldown = self.fireCooldown + 1.0 / f.rate
    self.shots = self.shots or {}
    local gun = self.gun or "Rifle"
    self.shots[gun] = (self.shots[gun] or 0) + 1

    -- This round's cone is the heat before it, so a first shot gets the first-shot cone. The gate
    -- modes fire with none: their hit and kill counts are measurements, and spread would make
    -- them vary from run to run.
    local gate = self.autopilot or self.losgate or self.p5gate
    local cone = gate and 0.0 or (f.spread[1] + (f.spread[2] - f.spread[1]) * self.heat)
    self.heat = math.min(1.0, self.heat + 1.0 / (f.rate * SPREAD_BLOOM))
    self.sinceShot = 0.0
    self:Fire(cone, gun)
end

-- A direction inside a cone of half-angle `deg` around `dir`, uniform over the cone's area. Picking
-- the angle off the axis uniformly instead would crowd rounds toward the centre: there is far less
-- solid angle near the axis than near the rim. Uniform area is cos(theta) uniform in [cos deg, 1].
local function Scatter(dir, deg)
    if deg <= 0.0 then
        return dir
    end
    local c = 1.0 - math.random() * (1.0 - math.cos(math.rad(deg)))
    local r = math.sqrt(math.max(0.0, 1.0 - c * c))
    local phi = 2.0 * math.pi * math.random()
    local helper = math.abs(dir.y) < 0.99 and Vec3(0, 1, 0) or Vec3(1, 0, 0)
    local u = dir:Cross(helper):Normalized()
    local v = dir:Cross(u)
    return dir * c + u * (r * math.cos(phi)) + v * (r * math.sin(phi))
end

function Player:Fire(cone, gun)
    local p = self.entity:GetTranslation()
    local sinY, cosY = math.sin(self.yaw), math.cos(self.yaw)
    local fx, fz = -sinY, -cosY

    -- A metre ahead and at chest height. Spawning inside the capsule would have the round collide
    -- with the player on its first step, and the shot would die where it was born.
    local muzzle = Vec3(p.x + fx * 1.0, p.y + CHEST_HEIGHT, p.z + fz * 1.0)

    -- Horizontal along yaw unless a human is aiming with the mouse. The gate modes never capture
    -- the cursor, so they keep firing flat from the chest and their numbers do not move.
    local dir = Vec3(fx, 0.0, fz)
    local aim = self:AimPoint()
    if aim then
        muzzle = self:BarrelPoint(p, fx, fz, aim) or muzzle
        local toAim = aim - muzzle
        -- A point behind the muzzle, or almost on it (a wall right in front of the camera), has
        -- no useful direction from here. Flat forward is the least surprising fallback.
        if toAim:Dot(dir) > 0.5 then
            dir = toAim:Normalized()
        end
    end
    self.aimHold = AIM_HOLD

    if cone and cone > 0.0 then
        local aimed = dir
        dir = Scatter(dir, cone)
        local off = math.deg(math.acos(math.max(-1.0, math.min(1.0, aimed:Dot(dir)))))
        self.spreadStats = self.spreadStats or {}
        local st = self.spreadStats[gun]
        if not st then
            st = { first = cone, lo = cone, hi = cone, worst = 0.0, sum = 0.0, n = 0, outside = 0 }
            self.spreadStats[gun] = st
        end
        st.lo, st.hi = math.min(st.lo, cone), math.max(st.hi, cone)
        st.worst, st.sum, st.n = math.max(st.worst, off), st.sum + off, st.n + 1
        if off > cone + 1e-3 then st.outside = st.outside + 1 end
    end

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
    self.pending[#self.pending + 1] = {
        id = id,
        vx = dir.x * self.muzzleSpeed,
        vy = dir.y * self.muzzleSpeed,
        vz = dir.z * self.muzzleSpeed,
    }
end

function Player:PushPending()
    if not self.pending or #self.pending == 0 then return end
    local still = {}
    for i = 1, #self.pending do
        local q = self.pending[i]
        local e = Scene.FindEntityByUUID(q.id)
        if e then
            e:SetLinearVelocity(Vec3(q.vx, q.vy, q.vz))
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
    -- Gate modes only: in play, falling off the edge is a death and a respawn (Player:FallCheck),
    -- not a failure.
    local gate = self.autopilot or self.losgate or self.p5gate or self.autofire
    if gate and p.y < -5.0 and not self.fell then
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
            "GATE t=%.0fs pos=(%.1f, %.2f, %.1f) grounded=%.0f%% inWall=%d inBH=%d inWH=%d | fired=%d live=%d despawned=%d hits=%d kills=%d refused=%d",
            self.t, p.x, p.y, p.z,
            100.0 * self.groundedFrames / math.max(self.frames, 1), self.inWall, self.inBH, self.inWH,
            PG.fired, PG.fired - PG.despawned, PG.despawned, PG.hits, PG.kills, self.refused))
        Log.Info(string.format(
            "P5   hp=%.0f/%.0f taken=%d healed=%.0f gun=%s guns-taken=%d dmg=%d score=%d upgrades=%d "
            .. "triggers=%d ui-mismatch=%d",
            self.health, self.maxHealth, self.hits, self.healed, self.gun or "none", self.gunsTaken, PG.damage,
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
        if self.shots then
            local line = ""
            for _, gun in ipairs(GUN_LADDER) do
                if self.shots[gun] then line = line .. string.format(" %s=%d", gun, self.shots[gun]) end
            end
            Log.Info("SHOTS" .. line)
        end
        -- Cone: the first-shot and widest cone used. Off: how far rounds actually strayed, mean and
        -- worst; `outside` counts rounds past their own cone, which must stay 0.
        for _, gun in ipairs(GUN_LADDER) do
            local st = self.spreadStats and self.spreadStats[gun]
            if st then
                Log.Info(string.format("SPREAD %s cone %.2f..%.2f deg, off mean %.2f worst %.2f, outside %d of %d",
                    gun, st.lo, st.hi, st.sum / st.n, st.worst, st.outside, st.n))
            end
        end
        if self.attacks > 0 or self.equipped ~= "Gun" then
            Log.Info(string.format("MELEE equipped=%s attacks=%d", self.equipped, self.attacks))
        end
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
