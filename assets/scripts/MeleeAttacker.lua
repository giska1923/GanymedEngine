-- Melee hit detection - a weapon trace, driven by the attack clip that is playing.
--
-- Goes on the character's mesh entity (the one with the AnimatorComponent), next to whatever plays
-- its attacks. The weapon is a direct child of that entity, as two-hand IK already requires, and
-- carries `Trace1`..`Trace3` children down its striking edge (prefabs/weapons/Knife, Axe).
--
-- ---- How a hit is found ----
--
-- Every frame of an attack's damage window, each trace point is ray-cast from where it was last
-- frame to where it is now. That is the standard weapon trace (Unreal's weapon-trace sockets,
-- Unity's swept collider checks): the ray covers the whole path between two frames, so a blade
-- moving 20 m/s - 33 cm in a frame - cannot pass through a target without touching it, which a
-- per-frame overlap test would miss. A target is hit at most once per swing.
--
-- ---- Why the window is a table and not an animation event ----
--
-- The engine has no animation events and no `GetAnimationTime` (cut from v1 with blend trees; see
-- docs/engine/scripting.md). What a script *can* read is which clip is playing, so this script
-- times the clip itself from the frame it became current, wrapping at the clip's length for a
-- looping clip. The windows below are the striking point's fast interval (above half its peak
-- speed), measured offline on all five characters (`melee_check.py` in the Meshy tooling); they
-- agree to 33-67 ms across rigs, except the knife's two-phase Charged_Slash, whose window is the
-- union of both. Two things this cannot see: `SetAnimationSpeed` (there is no getter, so a sped-up
-- attack drifts out of its window), and re-playing the clip that is already current, which the
-- binding deliberately does not restart.
--
-- ---- Where the damage goes ----
--
-- A script cannot call into another entity's script instance, and PG is the only channel between
-- two of them (Enemy.lua says the same about projectiles). A hit adds to PG.meleeDamage[uuid];
-- Enemy.lua drains its own entry each frame through the same TakeDamage path a projectile uses.
-- Anything else that wants to be hit can drain its entry the same way.
--
-- ---- Not hitting yourself ----
--
-- The attacker's collider is on its parent, and there is no GetParent binding to name it as the
-- ray's ignore entity. So a hit is discarded when the hit entity's child of our name is us. That
-- covers a body one level under its collider (Enemy, the Armory rows); the player's Body sits two
-- levels down (Player/Yaw/Body), so Player.lua names its capsule through PG.meleeConfig instead.
--
-- ---- Driven from another script ----
--
-- PG.meleeConfig[uuid of this entity] = { weapon = "Knife", ignore = uuid }, when present,
-- overrides the `weapon` property and adds an entity to never hit. It is how Player.lua switches
-- weapons at run time without a way to write another script's fields. The windows table is
-- published as PG.meleeWindows for whatever starts the attacks: it needs each clip's length to
-- know when an attack is over, and its lane to turn into it.

PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }

-- [weapon][clip] = { window start, window end, clip length, lane yaw, lane distance }.
-- Times are seconds of clip time. The lane is where the striking point is at its peak speed,
-- relative to the attacker: yaw in degrees from straight ahead (positive = the attacker's left) and
-- horizontal distance in metres, averaged over the five characters. These attacks are not centred -
-- the chop lands 40 deg left, the thrust 45 deg right - so an enemy squarely in front at the Enemy
-- prefab's collider size is missed by both; whatever drives an attack should turn the attacker
-- until the target sits in the lane. The spin sweeps both sides, so its lane is only the first
-- side it reaches.
local WINDOWS = {
    Knife = {
        Right_Hand_Sword_Slash = { 0.40, 0.70, 1.50, -72.7, 0.50 },
        Thrust_Slash           = { 0.60, 0.77, 3.00, -45.2, 0.92 },
        Axe_Spin_Attack        = { 0.30, 1.20, 2.50,  20.8, 0.59 },
        Charged_Slash          = { 0.23, 1.37, 2.23, -14.8, 0.42 },
    },
    Axe = {
        Axe_Chop               = { 0.77, 0.93, 2.47,  40.4, 1.47 },
        Axe_Spin_Attack        = { 0.80, 1.17, 2.50,  18.7, 1.18 },
    },
}

PG.meleeWindows = WINDOWS

local TRACE_POINTS = 3

local MeleeAttacker = {
    entity = nil,
    Properties = {
        -- Tag of the weapon child: "Knife" or "Axe" (a key of WINDOWS), or "None" for an
        -- attacker whose weapon is set at run time through PG.meleeConfig.
        weapon = "Axe",
        -- Per hit, once per target per swing. Enemy.health is 100.
        damage = 40.0,
    },
    weapon = "Axe",
    damage = 40.0,
}

function MeleeAttacker:OnCreate()
    self.clip = nil
    self.t = 0.0
    self.prev = {}
    self.hitThisSwing = {}
    self.swings = 0
    self.hits = 0
    PG.meleeDamage = PG.meleeDamage or {}
    -- "None" is deliberate: an attacker armed at run time through PG.meleeConfig (the player).
    if self.weapon ~= "None" and not WINDOWS[self.weapon] then
        Log.Warn(string.format("MeleeAttacker on %s: no damage windows for weapon '%s'",
            self.entity:GetName(), self.weapon))
    end
end

function MeleeAttacker:Traces(weaponName)
    if self.traces and self.tracesOf == weaponName then return self.traces end
    self.traces, self.tracesOf = nil, nil
    local weapon = self.entity:GetChildByName(weaponName)
    if not weapon then return nil end
    local traces = {}
    for i = 1, TRACE_POINTS do
        local t = weapon:GetChildByName("Trace" .. i)
        if t then traces[#traces + 1] = t end
    end
    if #traces == 0 then return nil end
    self.traces, self.tracesOf = traces, weaponName
    return traces
end

-- True when `entity` is the parent we cannot name: one of its children carries our name and is us.
function MeleeAttacker:IsSelf(entity, config)
    if config and config.ignore and entity:GetUUID() == config.ignore then return true end
    local child = entity:GetChildByName(self.entity:GetName())
    return child ~= nil and child:GetUUID() == self.entity:GetUUID()
end

function MeleeAttacker:OnUpdate(ts)
    -- A hitch frame would sweep a ray across half the swing; skip it rather than over-reach.
    if ts > 0.25 then return end

    local config = PG.meleeConfig and PG.meleeConfig[self.entity:GetUUID()]
    local weaponName = (config and config.weapon) or self.weapon
    local traces = self:Traces(weaponName)
    local windows = WINDOWS[weaponName]
    if not traces or not windows then
        self.clip, self.prev = nil, {}
        return
    end

    local clip = self.entity:GetCurrentAnimation()
    if clip ~= self.clip then
        self.clip = clip
        self.t = 0.0
        self.hitThisSwing = {}
        self.prev = {}
    else
        self.t = self.t + ts
    end

    local w = windows[clip]
    if not w then
        self.prev = {}
        return
    end

    -- Looping clips: each lap is a new swing with its own hit list.
    local lap = math.floor(self.t / w[3])
    if lap ~= self.lap then
        self.lap = lap
        self.hitThisSwing = {}
        self.swings = self.swings + 1
    end
    local phase = self.t - lap * w[3]
    local live = phase >= w[1] and phase <= w[2]

    for i, trace in ipairs(traces) do
        -- One frame old: GetWorldPosition reads the cache the transform systems wrote last frame,
        -- which only delays the trace by a frame and keeps consecutive segments joined.
        local p = trace:GetWorldPosition()
        local q = self.prev[i]
        if live and q then
            local d = p - q
            local len = d:Length()
            if len > 1e-4 then
                local hit = Physics.Raycast(q, d, len)
                if hit and hit.entity and not self:IsSelf(hit.entity, config) then
                    local id = hit.entity:GetUUID()
                    if not self.hitThisSwing[id] then
                        self.hitThisSwing[id] = true
                        self.hits = self.hits + 1
                        PG.meleeDamage[id] = (PG.meleeDamage[id] or 0) + self.damage
                        Log.Info(string.format("Melee: %s %s %s hit %s at %.2f s (trace %d, %.1f m/s)",
                            self.entity:GetName(), weaponName, clip, hit.entity:GetName(), phase, i, len / ts))
                    end
                end
            end
        end
        self.prev[i] = p
    end
end

return MeleeAttacker
