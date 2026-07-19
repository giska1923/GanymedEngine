--[[ Generated with https://github.com/TypeScriptToLua/TypeScriptToLua ]]
local ____exports = {}
local Player = {
    entity = nil,
    speed = 3,
    elapsed = 0,
    OnCreate = function(self)
        Log.Info("Player created: " .. self.entity:GetName())
    end,
    OnUpdate = function(self, ts)
        self.elapsed = self.elapsed + ts
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
        pos.y = math.sin(self.elapsed * 2)
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
