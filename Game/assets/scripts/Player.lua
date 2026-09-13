-- Proving Ground - player controller (P1)
--
-- Movement is velocity-driven on a rotation-locked dynamic capsule. The capsule itself never
-- turns: its TransformComponent is overwritten from Jolt every frame by SyncTransforms, so a yaw
-- written here would be erased before it was ever seen. Facing therefore lives on a child entity
-- ("Yaw"), which has no body and whose local transform nothing else touches.
--
-- Turning is on the keyboard because the engine has no cursor capture yet - see
-- docs/ToDo/PROVING_GROUND.md P0.5. When that lands this reads a mouse delta instead and nothing
-- else here changes.

local Player = {
    entity = nil,
    Properties = {
        speed = 6.0,        -- metres/second on the ground plane
        turnSpeed = 2.4,    -- radians/second
        -- Drives the circuit below with no keyboard. P1's gate is two minutes of walking into
        -- things, and a human holding W for two minutes is a worse instrument than a script
        -- that walks the same route every time. Turn it off to play.
        autopilot = true,
    },
    speed = 6.0,
    turnSpeed = 2.4,
    autopilot = true,
    yaw = 0.0,
    yawEntity = nil,
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
local ROUTE = {
    {  0, -7 },   -- head-on at the 0.2 m Step: can a capsule with no step-up ride a kerb?
    {  9, -4 },   -- along Block A's east face
    {  4,  6 },   -- open floor, a long run to reach full speed
    {-11,  5 },   -- past Block B's west end
    {  0,  0 },   -- back through the middle
}

function Player:OnCreate()
    self.yawEntity = self.entity:GetChildByName("Yaw")
    if not self.yawEntity then
        Log.Error("Player: no child named 'Yaw' - turning and the camera will not work")
    end

    local p = self.entity:GetTranslation()
    self.startY = p.y
    self.lastPos = p
    Log.Info(string.format("Player ready at (%.2f, %.2f, %.2f)", p.x, p.y, p.z))
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

    -- ---- turn ----
    local turn = 0.0
    if Input.IsKeyPressed(Key.Q) or Input.IsKeyPressed(Key.Left) then turn = turn + 1.0 end
    if Input.IsKeyPressed(Key.E) or Input.IsKeyPressed(Key.Right) then turn = turn - 1.0 end
    if self.autopilot then turn = turn + self:AutoTurn() end
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
    self.entity:SetLinearVelocity(Vec3(ix * self.speed, v.y, iz * self.speed))

    self:Diagnose(ts)
end

-- A circuit that walks the map and meets every obstacle in it: straight stretches long enough to
-- reach full speed, turns that bring it back around, and headings aimed at Block A (6, -4),
-- Block B (-7, 5) and the 0.2 m Step at z = -8. Walking *into* things is the point - sticking is
-- one of the three failure modes and it only shows up against geometry.
function Player:AutoTurn()
    local p = self.entity:GetTranslation()
    local target = ROUTE[self.wp]
    local dx, dz = target[1] - p.x, target[2] - p.z

    -- Advance on arrival, or on having been stuck for a second and a half. The escape is not a
    -- workaround for the route: a patrol that cannot get unstuck is a patrol that stops, and P4
    -- needs the same behaviour for enemies. Without it one bad waypoint ends the run.
    if self.stuckFor > 1.5 then
        self.stuckFor = 0.0
        self.wp = (self.wp % #ROUTE) + 1
        Log.Warn(string.format("autopilot: stuck at (%.1f, %.1f), skipping to waypoint %d",
            p.x, p.z, self.wp))
        target = ROUTE[self.wp]
        dx, dz = target[1] - p.x, target[2] - p.z
    elseif (dx * dx + dz * dz) < 2.25 then
        self.wp = (self.wp % #ROUTE) + 1
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

    -- tipping: any rotation away from upright at all, since LockRotation should hold it at zero
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
    local wants = grounded and (self.autopilot or Input.IsKeyPressed(Key.W)
        or Input.IsKeyPressed(Key.S) or Input.IsKeyPressed(Key.A) or Input.IsKeyPressed(Key.D))
    if wants and moved < 0.001 then
        self.stuckFor = self.stuckFor + ts
        if self.stuckFor > self.worstStuck then self.worstStuck = self.stuckFor end
    else
        self.stuckFor = 0.0
    end
    self.lastPos = p

    if self.t >= self.nextReport then
        self.nextReport = self.nextReport + 5.0
        Log.Info(string.format(
            "GATE t=%.0fs wp=%d pos=(%.1f, %.2f, %.1f) maxTilt=%.4f minY=%.3f worstStuck=%.2fs",
            self.t, self.wp, p.x, p.y, p.z, self.maxTilt, self.minY, self.worstStuck))
    end
end

local ____exports = {}
____exports.default = Player
return ____exports
