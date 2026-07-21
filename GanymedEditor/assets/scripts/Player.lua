--[[ Generated with https://github.com/TypeScriptToLua/TypeScriptToLua ]]
local ____exports = {}
local Player = {
    entity = nil,
    Properties = {speed = 3, drainRate = 12, bobHeight = 1},
    speed = 0,
    drainRate = 0,
    bobHeight = 0,
    elapsed = 0,
    health = 100,
    score = 0,
    OnCreate = function(self)
        Log.Info(((("Player created: " .. self.entity:GetName()) .. " (speed=") .. tostring(self.speed)) .. ")")
        UI.SetHealth(self.health)
        UI.SetScore(self.score)
    end,
    OnUpdate = function(self, ts)
        self.elapsed = self.elapsed + ts
        self.health = self.health - ts * self.drainRate
        if self.health <= 0 then
            self.health = 100
        end
        UI.SetHealth(self.health)
        self.score = math.floor(self.elapsed * 10)
        UI.SetScore(self.score)
        local pos = self.entity:GetTranslation()
        if Input.IsKeyPressed(Key.W) then
            pos.z = pos.z - self.speed * ts
        end
        if Input.IsKeyPressed(Key.S) then
            pos.z = pos.z + self.speed * ts
        end
        if Input.IsKeyPressed(Key.A) then
            pos.x = pos.x - self.speed * ts
        end
        if Input.IsKeyPressed(Key.D) then
            pos.x = pos.x + self.speed * ts
        end
        pos.y = math.sin(self.elapsed * 2) * self.bobHeight
        self.entity:SetTranslation(pos)
    end,
    OnCollisionEnter = function(self, other)
        Log.Warn("Hit " .. other:GetName())
    end,
    OnDestroy = function(self)
        Log.Info("Player destroyed")
    end
}
____exports.default = Player
return ____exports
