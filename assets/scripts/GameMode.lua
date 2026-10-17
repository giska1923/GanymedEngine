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
local PREFAB = "prefabs/Player.gprefab"

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
    pawns = nil,       -- [account] = { id, slot }
    offline = nil,     -- the offline pawn's id
}

function GameMode:OnCreate()
    self.pawns = {}
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
    if Net.IsClient() then
        return
    end

    if not Net.IsServer() then
        if not self.offline then
            self.offline = Scene.Spawn(PREFAB, SPAWNS[1])
            if self.offline then
                Log.Info("GameMode: offline, one pawn")
            end
        end
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
end

local ____exports = {}
____exports.default = GameMode
return ____exports
