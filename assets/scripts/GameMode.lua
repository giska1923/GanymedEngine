-- N5 G1 - who has a pawn.
--
-- The player used to be placed in the scene: one Player, with the camera inside its own hierarchy.
-- With several players that cannot work, so the pawn is prefabs/Player.gprefab and this script
-- decides who gets one:
--
--   a dedicated server   every account with a session (Net.Players), assigned with Net.AssignPawn
--   offline              exactly one, for the person at the keyboard: a standalone runtime, or the
--                        editor's Play
--   a client             nothing. Its pawns arrive in snapshots, and the one it controls is the
--                        one the server assigned it
--
-- The camera is the scene's "Camera Rig", which the locally controlled pawn drives (Player.lua).
--
-- N5 G6: it also runs the match (NETCODE.md, step 6), on the server, and decides how it ends:
--
--   waves     the 7 enemies placed in the scene are wave 1. Clearing one spawns the next from
--             prefabs/Enemy.gprefab at the placed enemies' spots, more for more players. Clearing
--             the last is victory. The last HUNT_REMAINING of a wave come looking for the players
--             (PG.huntRemaining, Enemy.lua), so a sentry nobody passes cannot hold a wave open
--   defeat    everyone down at once; the time limit; everyone gone (a leaver's pawn goes with them,
--             so a leaver counts as down); or nobody ever arriving
--
-- The outcome goes to Net.EndMatch, and the dedicated process reports it and exits. Offline the
-- waves run too, and the last one cleared is logged; there is no match to lose.
local PREFAB = "prefabs/Player.gprefab"
local ENEMY = "prefabs/Enemy.gprefab"

-- Waves 2 and 3: base + per player. Wave 1 is the scene's 7. Tuning, not a rule.
local HUNT_REMAINING = 2

local WAVES = {
    { base = 4, perPlayer = 2 },
    { base = 6, perPlayer = 3 },
}

-- Where wave enemies appear: the placed enemies' spots, round the map and away from the spawns.
-- Not the Sentry's post: that is inside the Blockhouse, and a wave enemy has no door written down
-- (Enemy.lua's exit1/exit2), so one spawned there stays there. Outside its doorway instead.
local MARKERS = {
    Vec3(-4.0, 0.9, -12.0), Vec3(2.0, 0.9, -14.0), Vec3(8.0, 0.9, -13.0), Vec3(-8.0, 0.9, -3.0),
    Vec3(5.0, 1.1, 3.0), Vec3(-3.0, 1.1, 8.0), Vec3(16.5, 0.9, 4.0),
}

-- The old Player's spot, and either side of it. Two pawns on one spot push each other apart on
-- the server, while a client sees the other one where it was 100 ms ago, and corrects
-- (NETCODE.md, stage E). So they start apart.
local SPAWNS = {
    Vec3(0.0, 1.2, 0.0),
    Vec3(2.5, 1.2, 0.0),
    Vec3(-2.5, 1.2, 0.0),
    Vec3(5.0, 1.2, 0.0),
}

local GameMode = {
    entity = nil,
    Properties = {
        timeLimit = 900.0,   -- seconds of match, from the first player's arrival: then defeat
        noShow = 120.0,      -- seconds with nobody arriving: defeat, so the match still ends
    },
    timeLimit = 900.0,
    noShow = 120.0,
    pawns = nil,       -- [account] = { id, slot }
    offline = nil,     -- the offline pawn's id
    t = 0.0,           -- since the scene started
    clock = nil,       -- match seconds, from the first arrival
    hadPlayers = false,
    wave = 1,
    waveGrace = 0.0,   -- seconds before a fresh wave can count as cleared: its enemies spawn next frame
    over = false,
}

function GameMode:OnCreate()
    self.pawns = {}
    -- PG outlives a Play in the editor.
    PG = PG or { fired = 0, despawned = 0, hits = 0, kills = 0 }
    PG.huntRemaining = false
end

local function Count(set)
    local n = 0
    for _ in pairs(set or {}) do
        n = n + 1
    end
    return n
end

-- Ends the match once, with the reason in the log. Offline there is nothing to end.
function GameMode:End(outcome, why)
    if self.over then
        return
    end
    self.over = true
    if Net.IsServer() then
        local accepted = Net.EndMatch(outcome)
        Log.Info(string.format("MATCH %s: %s (t=%.0fs, wave %d)%s", string.upper(outcome), why,
            self.clock or 0.0, self.wave, accepted and "" or " - not accepted"))
    else
        Log.Info(string.format("MATCH %s offline: %s (wave %d)", string.upper(outcome), why, self.wave))
    end
end

-- Next wave when the current one is cleared; victory after the last.
function GameMode:Waves(ts, players)
    if PG and PG.gateRun then
        return
    end
    self.waveGrace = math.max(0.0, self.waveGrace - ts)
    local left = Count(PG and PG.enemies)
    if self.waveGrace > 0.0 then
        return
    end
    if left > 0 then
        if left <= HUNT_REMAINING and not PG.huntRemaining then
            PG.huntRemaining = true
            Log.Info(string.format("WAVE %d: %d left - they come looking", self.wave, left))
        end
        return
    end
    PG.huntRemaining = false
    if self.wave > #WAVES then
        self:End("victory", string.format("wave %d cleared", self.wave))
        return
    end
    local w = WAVES[self.wave]
    local n = w.base + w.perPlayer * math.max(1, players)
    self.wave = self.wave + 1
    local spawned = 0
    for i = 1, n do
        local at = MARKERS[((i - 1) % #MARKERS) + 1]
        -- A second ring a metre out for the second lap of the markers, so two do not share a spot.
        local lap = math.floor((i - 1) / #MARKERS)
        if Scene.Spawn(ENEMY, Vec3(at.x + lap * 1.0, at.y, at.z + lap * 1.0)) then
            spawned = spawned + 1
        end
    end
    self.waveGrace = 3.0
    Log.Info(string.format("WAVE %d: %d enemies for %d player%s", self.wave, spawned, players,
        players == 1 and "" or "s"))
end

function GameMode:FreeSlot()
    local used = {}
    for _, pawn in pairs(self.pawns) do
        used[pawn.slot] = true
    end
    local slot = 1
    while used[slot] do
        slot = slot + 1
    end
    return slot
end

function GameMode:OnUpdate(ts)
    if Net.IsClient() or self.over then
        return
    end
    if ts > 0.25 then
        return
    end
    self.t = self.t + ts

    if not Net.IsServer() then
        if not self.offline then
            self.offline = Scene.Spawn(PREFAB, SPAWNS[1])
            if self.offline then
                Log.Info("GameMode: offline, one pawn")
            end
        end
        self:Waves(ts, 1)
        return
    end

    local live = {}
    for _, account in ipairs(Net.Players()) do
        live[account] = true
        if not self.pawns[account] then
            local slot = self:FreeSlot()
            local at = SPAWNS[((slot - 1) % #SPAWNS) + 1]
            local id = Scene.Spawn(PREFAB, at)
            if id then
                Net.AssignPawn(account, id)
                self.pawns[account] = { id = id, slot = slot }
                Log.Info(string.format("GameMode: %s joined, pawn at spawn %d", account, slot))
            end
        end
    end

    -- A session ends 5 s after its last packet. The pawn goes with it.
    for account, pawn in pairs(self.pawns) do
        if not live[account] then
            local e = Scene.FindEntityByUUID(pawn.id)
            if e then
                e:Destroy()
            end
            self.pawns[account] = nil
            Log.Info(string.format("GameMode: %s left, pawn removed", account))
        end
    end

    self:Rules(ts)
end

-- The match's end, on the server.
function GameMode:Rules(ts)
    local present = Count(self.pawns)
    if present > 0 and not self.hadPlayers then
        self.hadPlayers = true
        self.clock = 0.0
        Log.Info(string.format("MATCH started: %d s limit, %d waves after the first", self.timeLimit, #WAVES))
    end
    if not self.hadPlayers then
        if self.t > self.noShow then
            self:End("defeat", string.format("nobody arrived in %.0f s", self.noShow))
        end
        return
    end
    self.clock = self.clock + ts

    if present == 0 then
        self:End("defeat", "everyone left")
        return
    end
    if self.clock > self.timeLimit then
        self:End("defeat", string.format("the %.0f s time limit", self.timeLimit))
        return
    end

    -- Everyone down at once. A pawn not yet in PG.pawnDown has not had a ServerFrame: up.
    local down = 0
    for _, pawn in pairs(self.pawns) do
        if PG and PG.pawnDown and PG.pawnDown[pawn.id] then
            down = down + 1
        end
    end
    if down == present then
        self:End("defeat", string.format("all %d player%s down", present, present == 1 and "" or "s"))
        return
    end

    self:Waves(ts, present)
end

local ____exports = {}
____exports.default = GameMode
return ____exports
