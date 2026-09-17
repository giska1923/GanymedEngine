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
    },
    speed = 6.0,
    turnSpeed = 2.4,
    sensitivity = 0.0022,
    muzzleSpeed = 28.0,
    fireInterval = 0.12,
    autofire = false,
    autopilot = false,
    losgate = false,
    losWp = 1,
    losHold = 0.0,
    losDrive = false,
    losSlow = 1.0,
    losDone = false,
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
-- inside solid boxes: the capsule pressed against Block A's west face for 124 s and the route
-- never advanced, because a velocity-driven capsule does not slide along a wall - drive it
-- straight at a surface and the solver cancels the whole velocity, leaving no tangential
-- component to carry it sideways. That is CharacterVirtual's job, not this flag's.
--
-- So the route grazes the geometry instead of aiming through it. Footprints, for reference:
--   Block A  x[4.5, 7.5]  z[-5.5, -2.5]   h 1.5
--   Block B  x[-9, -5]    z[4, 6]         h 1.0 (yawed 0.5 rad)
--   Step     x[-4, 4]     z[-8.5, -7.5]   h 0.2   <- the one it is meant to climb
-- P2 extends this through the two buildings. Their doorways were measured off the meshes rather
-- than guessed, and the route aims at each one and then at a point INSIDE, so "walk in and out of
-- every building" is exercised rather than asserted:
--   Blockhouse  centre (16, -6),  8.0 x 7.2,  doorway 1.8 m on its -Z wall at local x +2.56
--   Warehouse   centre (-22, -14), 20 x 11.5, door    0.9 m on its -X wall at local z -1.92
local ROUTE = {
    {  0, -7 },    -- head-on at the 0.2 m Step: can the controller ride a kerb?
    {  9, -4 },    -- along Block A's east face
    { 18.6, -11 }, -- line up on the Blockhouse doorway from outside
    { 18.6, -6 },  -- through it, into the middle of the Blockhouse
    { 18.6, -11 }, -- and back out the way it came
    {  4,  6 },    -- open floor, a long run to reach full speed
    {-11,  5 },    -- past Block B's west end
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
    {  2.00, -11.0, 0.5, "staging - clear of Block A, which is on the direct line from spawn" },
    { 14.00, -12.0, 8.0, "behind the -Z wall: the Sentry must NOT acquire" },
    { 18.56, -12.0, 8.0, "on the doorway's sight line: it must" },
    { 14.00, -12.0, 6.0, "back behind the wall: it must lose me again" },
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

    Log.Info("Player: click to look, Escape to release the cursor")

    local p = self.entity:GetTranslation()
    self.startY = p.y
    self.lastPos = p
    Log.Info(string.format("Player ready at (%.2f, %.2f, %.2f)", p.x, p.y, p.z))

    if self.losgate then
        -- The full initialiser, not `PG or {}`: Fire() only fills the counters in when PG is
        -- absent entirely, so a PG that exists but holds nothing but `freeze` would make the
        -- first shot add 1 to nil.
        PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }
        PG.freeze = true
        Log.Info("LOSGATE armed: enemies sense but do not move")
    end
end

-- Walk to the next probe point, stand on it, move on. Returns a steering contribution in the
-- same units AutoTurn uses, and sets losDrive / losSlow, which the movement block reads.
--
-- The slowdown is not polish. At 6 m/s a frame covers 0.1 m, so a tight arrival radius is
-- overshot and the capsule orbits the point forever; a large one makes the probe position
-- imprecise, and this gate is a claim about a specific x. Easing to 1.2 m/s inside 3 m makes a
-- 0.35 m radius reachable without either.
function Player:LosTurn(ts)
    local p = self.entity:GetTranslation()
    local target = LOS_ROUTE[self.losWp]

    if self.losDone then
        self.losDrive = false
        self.losSlow = 1.0
        return 0.0
    end

    if self.losHold > 0.0 then
        self.losHold = self.losHold - ts
        self.losDrive = false
        self.losSlow = 1.0
        if self.losHold <= 0.0 then
            Log.Info(string.format("LOSGATE probe %d done, standing at (%.2f, %.2f)",
                self.losWp, p.x, p.z))
            if self.losWp >= #LOS_ROUTE then
                self.losDone = true
                Log.Info("LOSGATE route complete")
            else
                self.losWp = self.losWp + 1
            end
        end
        return 0.0
    end

    local dx, dz = target[1] - p.x, target[2] - p.z
    local dist = math.sqrt(dx * dx + dz * dz)

    if dist < 0.35 then
        -- max(..., 0.001) so a zero-second hold still takes the branch above next frame rather
        -- than re-arriving, and re-logging, every frame.
        self.losHold = math.max(target[3], 0.001)
        self.losDrive = false
        self.losSlow = 1.0
        Log.Info(string.format("LOSGATE probe %d at (%.2f, %.2f), holding %.1fs - %s",
            self.losWp, p.x, p.z, target[3], target[4]))
        return 0.0
    end

    self.losDrive = true
    self.losSlow = dist < 3.0 and 0.2 or 1.0

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
    if self.losgate then turn = turn + self:LosTurn(ts) end
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
    if self.losDrive then ix, iz = ix + fx, iz + fz end
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
    local speed = self.speed * self.losSlow
    self.entity:SetLinearVelocity(Vec3(ix * speed, v.y, iz * speed))

    self:PushPending()
    self:Diagnose(ts)
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
-- reach full speed, turns that bring it back around, and headings aimed at Block A (6, -4),
-- Block B (-7, 5) and the 0.2 m Step at z = -8. Walking *into* things is the point - sticking is
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
    local wants = grounded and (self.autopilot or self.losDrive or Input.IsKeyPressed(Key.W)
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
